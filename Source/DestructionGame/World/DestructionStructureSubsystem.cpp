// Copyright Epic Games, Inc. All Rights Reserved.

#include "World/DestructionStructureSubsystem.h"

#include "CollisionQueryParams.h"
#include "Components/StaticMeshComponent.h"
#include "Core/BuildMode/SnapSolver.h"
#include "Core/Connection.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Engine/HitResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "RequiredContent.h"
#include "World/BrickActor.h"

// File-local names are specific because unity builds merge anonymous namespaces across files.
namespace
{
	/**
	 * Base-colour material path for a piece's structural material, keyed on library row
	 * identity. Null for anything else, leaving the mesh's grey default.
	 */
	const TCHAR* ShedBaseMaterialPathFor(const DestructionProfiles::FMaterialProfile* Material)
	{
		if (Material == &DestructionProfiles::ClayBrick)
		{
			return DestructionContent::ShedBrickMaterialPath;
		}

		if (Material == &DestructionProfiles::Timber)
		{
			return DestructionContent::ShedTimberMaterialPath;
		}

		return nullptr;
	}

	/** Spawn one sized, placed, weighed brick with its piece ref. Null on failure. */
	ABrickActor* SpawnBrickForPiece(
		UWorld& World,
		const DestructionLayout::FPieceBox& Box,
		double MassKg,
		const FPieceRef& Ref,
		const DestructionProfiles::FMaterialProfile* Material)
	{
		ABrickActor* Brick = World.SpawnActorDeferred<ABrickActor>(
			ABrickActor::StaticClass(), FTransform::Identity);

		if (Brick == nullptr)
		{
			return nullptr;
		}

		UStaticMeshComponent* Mesh = Brick->GetMesh();
		UStaticMesh* BrickMesh = Mesh->GetStaticMesh();

		// A deleted mesh asset leaves this null, and sizing divides by its bounds.
		if (BrickMesh == nullptr)
		{
			Brick->Destroy();
			return nullptr;
		}

		Brick->SetPieceRef(Ref);

		// Set the solver's mass before FinishSpawning so the body is created with it.
		Mesh->SetMassOverrideInKg(NAME_None, static_cast<float>(MassKg), true);

		// Base colour by material on element 0; the highlight overlay draws on top of it.
		if (const TCHAR* const BasePath = ShedBaseMaterialPathFor(Material))
		{
			if (UMaterialInterface* BaseMaterial = LoadObject<UMaterialInterface>(nullptr, BasePath))
			{
				Mesh->SetMaterial(0, BaseMaterial);
			}
		}

		Brick->FinishSpawning(UDestructionStructureSubsystem::BrickSpawnTransform(*BrickMesh, Box));

		return Brick;
	}

	/**
	 * The snap decision shared by PlaceBuildPiece and PreviewBuildPiece, so a placement lands
	 * where its preview showed. No mutation, no world.
	 */
	struct FBuildPlacement
	{
		/** Whether a candidate was chosen; a default placement would otherwise be a valid massless piece. */
		bool bDecided = false;

		BuildMode::ESnapKind Kind = BuildMode::ESnapKind::Free;
		FVector CentreCm = FVector::ZeroVector;
		double MassKg = 0.0;

		/** Whether the chosen pose rests on the earth. Derived from the pose, never from a caller. */
		bool bGrounded = false;

		TArray<BuildMode::FFormedJoint> Joints;

		/** The library row those joints carry, for the ghost card. Null if there are none. */
		const FConnectionStrength* JointProfile = nullptr;
	};

	FBuildPlacement ComputeBuildPlacement(
		const FStructureBinding& Binding,
		const FVector& RequestedCentreCm,
		const FVector& ExtentCm,
		const DestructionProfiles::FMaterialProfile& Material,
		DestructionSession::EPlacementMode Placement,
		const FConnectionStrength* JointOverride)
	{
		using namespace DestructionLayout;

		const int32 PieceCount = Binding.NumPieces();

		TArray<FPieceBox> NearbyBoxes;
		TArray<DestructionProfiles::FMaterialProfile> NearbyMaterials;
		NearbyBoxes.Reserve(PieceCount);
		NearbyMaterials.Reserve(PieceCount);
		for (int32 i = 0; i < PieceCount; ++i)
		{
			NearbyBoxes.Add(Binding.GetBinding(i).Box);
			const DestructionProfiles::FMaterialProfile* Existing = Binding.GetStructure().GetPiece(i).Material;
			NearbyMaterials.Add(Existing != nullptr ? *Existing : DestructionProfiles::FMaterialProfile());
		}

		const BuildMode::FSnapSettings Settings;
		const FPieceBox Requested{ RequestedCentreCm, ExtentCm };
		const TArray<BuildMode::FSnapCandidate> Candidates = BuildMode::SolveSnapCandidates(
			Requested, Material, NearbyBoxes, NearbyMaterials, Settings);

		/*
		 * Snap takes the best-ranked candidate; Free finds the Free candidate by kind. No Free
		 * candidate is a refusal, never a fallback to a snap the player did not ask for.
		 */
		const BuildMode::FSnapCandidate* Chosen = nullptr;

		if (Placement == DestructionSession::EPlacementMode::Free)
		{
			Chosen = Candidates.FindByPredicate(
				[](const BuildMode::FSnapCandidate& Candidate)
				{
					return Candidate.Kind == BuildMode::ESnapKind::Free;
				});
		}
		else if (Candidates.Num() > 0)
		{
			Chosen = &Candidates[0];
		}

		if (Chosen == nullptr)
		{
			return FBuildPlacement{};
		}

		/*
		 * Refuse a non-finite chosen centre before either caller acts, so no ghost is previewed
		 * and no actor is spawned at a NaN transform. IsFinite is checked explicitly because
		 * ContainsNaN only promises NaN.
		 */
		if (Chosen->CentreCm.ContainsNaN()
			|| !FMath::IsFinite(Chosen->CentreCm.X)
			|| !FMath::IsFinite(Chosen->CentreCm.Y)
			|| !FMath::IsFinite(Chosen->CentreCm.Z))
		{
			return FBuildPlacement{};
		}

		FBuildPlacement Decision;
		Decision.bDecided = true;
		Decision.Kind = Chosen->Kind;
		Decision.CentreCm = Chosen->CentreCm;
		Decision.MassKg = PieceMassKg(FPieceBox{ Chosen->CentreCm, ExtentCm }, Material.DensityGramsPerCubicCm);
		Decision.Joints = Chosen->Joints;

		/*
		 * The override replaces the profile of every joint this placement forms (a plate forms
		 * two), never joints already in the structure.
		 */
		if (JointOverride != nullptr)
		{
			for (BuildMode::FFormedJoint& Joint : Decision.Joints)
			{
				Joint.Profile = *JointOverride;
			}
		}

		/*
		 * The ghost card's row: the override, else the library row matching the first joint's
		 * profile (never a pointer into Decision.Joints, which dies with the copy). Null when
		 * there are no joints, so a jointless piece names no fastener.
		 */
		if (JointOverride != nullptr && Decision.Joints.Num() > 0)
		{
			Decision.JointProfile = JointOverride;
		}
		else if (Decision.Joints.Num() > 0)
		{
			if (const DestructionProfiles::FNamedConnectionProfile* const Row =
				DestructionProfiles::FindConnectionProfileRow(Decision.Joints[0].Profile))
			{
				Decision.JointProfile = &Row->Strength;
			}
		}

		/*
		 * Grounded comes from the chosen pose (DESIGN §8, 2026-09-15): a snap can lift the piece
		 * onto a brick above, and a mid-air piece flagged grounded could never fall. One joint of
		 * tolerance, edge inclusive. Written `<=`, not the house `!(x > y)`, so NaN reads not grounded.
		 */
		const double BottomFaceZCm = Chosen->CentreCm.Z - ExtentCm.Z;
		Decision.bGrounded = BottomFaceZCm <= Settings.JointThicknessCm;

		return Decision;
	}

	/**
	 * Release to physics every piece the last solve stopped holding up. The caller must have
	 * solved first. Revisits earlier releases too, which is harmless: Release is idempotent.
	 *
	 * @return how many pieces this call released.
	 */
	int32 PushSolvedResultsToWorld(FStructureBinding& Binding)
	{
		const int32 ReleasedCount = Binding.ApplyResults();

		for (int32 PieceIndex = 0; PieceIndex < Binding.NumPieces(); ++PieceIndex)
		{
			if (!Binding.IsReleased(PieceIndex))
			{
				continue;
			}

			// GetActor is null for a removed piece.
			if (ABrickActor* Brick = Cast<ABrickActor>(Binding.GetActor(PieceIndex)))
			{
				Brick->Release();
			}
		}

		return ReleasedCount;
	}
}

int32 UDestructionStructureSubsystem::BuildRunningBond(const DestructionLayout::FRunningBondSpec& Spec)
{
	DestructionLayout::FBrickLayout Layout;

	// RunningBond produces boxes and graph together; nothing here re-derives them.
	if (!DestructionLayout::RunningBond(Spec, Layout))
	{
		return INDEX_NONE;
	}

	return BuildLayout(Layout);
}

int32 UDestructionStructureSubsystem::BuildLayout(const DestructionLayout::FBrickLayout& Layout)
{
	const int32 StructureId = NextStructureId;
	const int32 PieceCount = Layout.Structure.NumPieces();

	/*
	 * Validate before spawning: too many boxes would litter orphan actors, too few would index
	 * out of range before AdoptLayout could refuse. An empty layout is refused too.
	 */
	if (PieceCount < 1 || Layout.Boxes.Num() != PieceCount)
	{
		return INDEX_NONE;
	}

	UWorld& World = *GetWorld();

	TArray<UObject*> Actors;
	Actors.Reserve(PieceCount);

	for (int32 PieceIndex = 0; PieceIndex < PieceCount; ++PieceIndex)
	{
		FPieceRef Ref;
		Ref.StructureId = StructureId;
		Ref.PieceIndex = PieceIndex;

		Actors.Add(SpawnBrickForPiece(
			World,
			Layout.Boxes[PieceIndex],
			Layout.Structure.GetPiece(PieceIndex).MassKg,
			Ref,
			Layout.Structure.GetPiece(PieceIndex).Material));
	}

	TUniquePtr<FStructureBinding> Binding = MakeUnique<FStructureBinding>();
	Binding->StructureId = StructureId;

	if (!AdoptLayout(Layout, Actors, *Binding))
	{
		return INDEX_NONE;
	}

	// The id is spent only on success.
	NextStructureId = StructureId + 1;
	Structures.Add(StructureId, MoveTemp(Binding));

	return StructureId;
}

int32 UDestructionStructureSubsystem::BeginBuild()
{
	// An empty structure that PlaceBuildPiece grows one piece at a time.
	const int32 StructureId = NextStructureId;

	TUniquePtr<FStructureBinding> Binding = MakeUnique<FStructureBinding>();
	Binding->StructureId = StructureId;

	/*
	 * Always 3D, never inferred (E3 ruling): a rotated piece forms Y-normal joints the 2D oracle
	 * refuses, and flipping mid-build would change the break authority as a brick lands.
	 */
	Binding->SetThreeDimensional(true);

	NextStructureId = StructureId + 1;
	Structures.Add(StructureId, MoveTemp(Binding));

	return StructureId;
}

FPieceRef UDestructionStructureSubsystem::PlaceBuildPiece(
	int32 StructureId,
	const FVector& RequestedCentreCm,
	const FVector& ExtentCm,
	const DestructionProfiles::FMaterialProfile& Material,
	DestructionSession::EPlacementMode Placement,
	const FConnectionStrength* JointOverride)
{
	using namespace DestructionLayout;

	FStructureBinding* Binding = Find(StructureId);

	if (Binding == nullptr)
	{
		return FPieceRef{};
	}

	const FBuildPlacement Decision =
		ComputeBuildPlacement(*Binding, RequestedCentreCm, ExtentCm, Material, Placement, JointOverride);

	if (!Decision.bDecided)
	{
		return FPieceRef{};
	}

	const FPieceBox Box{ Decision.CentreCm, ExtentCm };

	// Handles are sequential, so the ref is known before AddPiece.
	FPieceRef Ref;
	Ref.StructureId = StructureId;
	Ref.PieceIndex = Binding->NumPieces();

	ABrickActor* Actor = SpawnBrickForPiece(*GetWorld(), Box, Decision.MassKg, Ref, &Material);

	const int32 Handle = Binding->AddPiece(Decision.MassKg, Decision.bGrounded, Actor, Box, &Material);

	// AddPiece refused the box: destroy the just-spawned actor rather than orphan it.
	if (Handle == INDEX_NONE)
	{
		if (Actor != nullptr)
		{
			Actor->Destroy();
		}
		return FPieceRef{};
	}

	const BuildMode::FSnapSettings Settings;
	for (const BuildMode::FFormedJoint& Joint : Decision.Joints)
	{
		FConnection Conn;
		if (MakeInterface(
				Handle,
				Box,
				Joint.OtherPieceIndex,
				Binding->GetBinding(Joint.OtherPieceIndex).Box,
				Settings.JointThicknessCm,
				Joint.Profile,
				Conn))
		{
			Binding->AddConnection(Conn);
		}
	}

	return FPieceRef{ StructureId, Handle };
}

FBuildPreview UDestructionStructureSubsystem::PreviewBuildPiece(
	int32 StructureId,
	const FVector& RequestedCentreCm,
	const FVector& ExtentCm,
	const DestructionProfiles::FMaterialProfile& Material,
	DestructionSession::EPlacementMode Placement,
	const FConnectionStrength* JointOverride) const
{
	const FStructureBinding* Binding = Find(StructureId);

	if (Binding == nullptr)
	{
		return FBuildPreview{};
	}

	// The same decision PlaceBuildPiece commits.
	const FBuildPlacement Decision =
		ComputeBuildPlacement(*Binding, RequestedCentreCm, ExtentCm, Material, Placement, JointOverride);

	if (!Decision.bDecided)
	{
		return FBuildPreview{};
	}

	FBuildPreview Preview;
	Preview.bValid = true;
	Preview.Kind = Decision.Kind;
	Preview.CentreCm = Decision.CentreCm;
	Preview.JointCount = Decision.Joints.Num();

	Preview.bGrounded = Decision.bGrounded;
	Preview.JointProfile = Decision.JointProfile;

	return Preview;
}


int32 UDestructionStructureSubsystem::SolveAndPush(int32 StructureId)
{
	FStructureBinding* Binding = Find(StructureId);

	if (Binding == nullptr)
	{
		return 0;
	}

	/*
	 * Solve before pushing, or an unsolved wall drops entire via the one-way latch. Settle
	 * (SolveAndBreak), not just solve, so a wall over capacity at spawn sheds now rather than on
	 * the player's first click (World.Push.AWallOverCapacityDoesNotWaitForAClick). A wall under
	 * capacity is unchanged bit for bit.
	 */
	Binding->SolveAndBreak();

	return PushSolvedResultsToWorld(*Binding);
}

FPieceHit UDestructionStructureSubsystem::TracePiece(const FVector& StartCm, const FVector& EndCm)
{
	FPieceHit Hit;

	FHitResult TraceResult;

	// The player's click uses ECC_Visibility.
	const bool bHitSomething = GetWorld()->LineTraceSingleByChannel(
		TraceResult,
		StartCm,
		EndCm,
		ECC_Visibility,
		FCollisionQueryParams(SCENE_QUERY_STAT(PieceTrace), true));

	if (!bHitSomething)
	{
		return Hit;
	}

	const ABrickActor* Brick = Cast<ABrickActor>(TraceResult.GetActor());

	if (Brick == nullptr)
	{
		return Hit;
	}

	// The brick's ref is resolved against its structure; an unknown structure fails the whole hit.
	const FStructureBinding* Binding = Find(Brick->GetPieceRef().StructureId);

	if (Binding == nullptr)
	{
		return Hit;
	}

	const int32 PieceHandle = Binding->ResolvePiece(Brick->GetPieceRef());

	if (PieceHandle == INDEX_NONE)
	{
		return Hit;
	}

	Hit.Ref = Brick->GetPieceRef();
	Hit.PieceHandle = PieceHandle;

	return Hit;
}

bool UDestructionStructureSubsystem::CommitPieceAction(const FPieceRef& Ref, const FPieceAction& Action)
{
	// A click on the floor arrives as a default ref and commits nothing.
	FStructureBinding* Binding = Find(Ref.StructureId);

	if (Binding == nullptr)
	{
		return false;
	}

	const FPieceActionResult Result = RunPieceAction(*Binding, Ref, Action);

	// RunPieceAction is world-free and hands the orphan actor back for destruction here.
	if (AActor* Orphan = Cast<AActor>(Result.ActorToDestroy))
	{
		Orphan->Destroy();
	}

	/*
	 * Push the settled answer so unsupported bricks fall. Push only, not SolveAndPush:
	 * RunPieceAction already settled, and settling again would stamp a second collapse.
	 */
	PushSolvedResultsToWorld(*Binding);

	return Result.bRan;
}

int32 UDestructionStructureSubsystem::CommitPieceActionForAll(
	TArrayView<const FPieceRef> Refs,
	const FPieceAction& Action)
{
	// A selection comes from one wall, so the first ref names the structure; RunPieceActions refuses strays.
	FStructureBinding* Binding = Refs.Num() > 0 ? Find(Refs[0].StructureId) : nullptr;

	if (Binding == nullptr)
	{
		return 0;
	}

	const FPieceBatchActionResult Result = RunPieceActions(*Binding, Refs, Action);

	// Destroy the orphan actors RunPieceActions handed back.
	for (UObject* const Orphan : Result.ActorsToDestroy)
	{
		if (AActor* Actor = Cast<AActor>(Orphan))
		{
			Actor->Destroy();
		}
	}

	// One push behind RunPieceActions' single settle; push only, as in CommitPieceAction.
	PushSolvedResultsToWorld(*Binding);

	return Result.RanCount;
}

FTransform UDestructionStructureSubsystem::BrickSpawnTransform(
	const UStaticMesh& BrickMesh, const DestructionLayout::FPieceBox& Box)
{
	const FBox LocalBounds = BrickMesh.GetBoundingBox();

	const FVector Scale = (Box.ExtentCm * 2.0) / LocalBounds.GetSize();

	return FTransform(
		FRotator::ZeroRotator,
		Box.CentreCm - Scale * LocalBounds.GetCenter(),
		Scale);
}

FStructureBinding* UDestructionStructureSubsystem::Find(int32 StructureId)
{
	const TUniquePtr<FStructureBinding>* Found = Structures.Find(StructureId);

	return Found != nullptr ? Found->Get() : nullptr;
}

const FStructureBinding* UDestructionStructureSubsystem::Find(int32 StructureId) const
{
	const TUniquePtr<FStructureBinding>* Found = Structures.Find(StructureId);

	return Found != nullptr ? Found->Get() : nullptr;
}

bool UDestructionStructureSubsystem::Destroy(int32 StructureId)
{
	FStructureBinding* Binding = Find(StructureId);

	if (Binding == nullptr)
	{
		return false;
	}

	// GetActor is null for removed pieces, so the whole handle range is safe to walk.
	for (int32 PieceIndex = 0; PieceIndex < Binding->NumPieces(); ++PieceIndex)
	{
		if (ABrickActor* Brick = Cast<ABrickActor>(Binding->GetActor(PieceIndex)))
		{
			Brick->Destroy();
		}
	}

	// Ids are never reused, so a stale ref cannot resolve against a later structure.
	Structures.Remove(StructureId);

	return true;
}
