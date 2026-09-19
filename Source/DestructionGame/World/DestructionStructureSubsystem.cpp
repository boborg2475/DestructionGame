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

/*
 * File-local names are spelled for what they are and where they live, deliberately: an
 * anonymous namespace is private to a translation unit, not a file, and a unity build
 * merges many files into one, so colliding file-local names become a hard compile error
 * between files that never refer to each other. See CURRENT_STATE.md.
 */
namespace
{
	/**
	 * The base-colour asset a piece's structural material paints element 0 with, or null for a
	 * material the shed does not use.
	 *
	 * A plain table keyed on pointer identity, not a name string: a piece's Material is a
	 * non-owning pointer into the program-lifetime profile library, so ClayBrick and Timber
	 * compare against their own addresses. Anything else returns null, leaving the mesh's
	 * grey default.
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

	/** One brick, sized, placed, weighed and told who it is. Null if it could not be built. */
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

		/*
		 * No mesh, no brick: the mesh is a hard content reference resolved on the CDO, so a
		 * deleted asset leaves it null rather than failing to compile, and the sizing below
		 * divides by its bounds — an infinite scale otherwise.
		 */
		if (BrickMesh == nullptr)
		{
			Brick->Destroy();
			return nullptr;
		}

		Brick->SetPieceRef(Ref);

		/*
		 * Mass at spawn, not release, and it is the mass the producer already derived for this
		 * piece rather than a second derivation from the same box. Setting it before
		 * FinishSpawning gets it into the body at creation; at release instead, every intact
		 * brick would carry whatever mass the mesh's volume implies — not what the solver uses.
		 */
		Mesh->SetMassOverrideInKg(NAME_None, static_cast<float>(MassKg), true);

		/*
		 * Base colour by structural material, on element 0 — brick reads brick-red, timber
		 * reads timber-tan. This is the look underneath the highlight overlay, never the
		 * overlay itself, so a brick keeps its colour when the cursor is nowhere near it.
		 */
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
	 * The snap decision for a piece placed into a binding, with no mutation and no world.
	 *
	 * The container-independent middle PlaceBuildPiece and PreviewBuildPiece share: one
	 * gathers the live pieces to spawn a real brick, the other to draw a ghost, but the
	 * decision is identical. NearbyBoxes and NearbyMaterials run parallel to the live piece
	 * array, so a candidate's OtherPieceIndex is exactly the existing piece's handle. Takes a
	 * const reference: it decides, it never places.
	 */
	struct FBuildPlacement
	{
		/**
		 * Whether a candidate was found at all. A default-constructed placement is a massless
		 * piece at the origin, which AddPiece would accept (zero mass is meaningful), so "no
		 * decision" must be said out loud.
		 */
		bool bDecided = false;

		BuildMode::ESnapKind Kind = BuildMode::ESnapKind::Free;
		FVector CentreCm = FVector::ZeroVector;
		double MassKg = 0.0;

		/** Whether the CHOSEN pose rests on the earth. Derived below, never taken from a caller. */
		bool bGrounded = false;

		TArray<BuildMode::FFormedJoint> Joints;

		/** Which SHIPPED library row those joints carry, for the ghost card. Null if there are none. */
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
		 * Snap takes the best-ranked candidate, Free takes the free one — found by kind, not
		 * index, since SolveSnapCandidates appends the Free fallback last, after the distance
		 * ranking.
		 *
		 * An absent Free candidate is a refusal, never a fall back to the nearest snap: Free
		 * is the player naming this exact pose, and answering with a different one would move
		 * a piece they are watching land.
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
		 * A non-finite chosen centre is refused here, before either door acts on it — the
		 * chosen pose is tested, not the requested one, since Snap may substitute a finite
		 * candidate for a wild cursor.
		 *
		 * The ghost: a pose no click can commit must not be previewed, so leaving bDecided
		 * false makes PreviewBuildPiece answer a default FBuildPreview instead.
		 *
		 * The commit: an actor must never be spawned at a transform the engine ensures on.
		 * Without this guard, PlaceBuildPiece spawned the brick first and only then let
		 * AddPiece refuse the pose, running a NaN transform through SetWorldTransform on its
		 * way to destroying the actor again.
		 *
		 * Both tests, deliberately not one: FVector::ContainsNaN also catches infinities
		 * today, but the name only promises NaN, so the explicit IsFinite sweep pins it too.
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
		 * The override replaces every joint's profile — a substitution, not a different
		 * placement: only the strength moves. A timber plate across a two-brick course forms
		 * two bearings from one placement, so writing the override onto Joints[0] alone would
		 * leave the far end resting on friction.
		 *
		 * It touches only the joints this placement forms; joints already in the structure
		 * are never re-priced, or a chip could silently change committed physics an hour later.
		 */
		if (JointOverride != nullptr)
		{
			for (BuildMode::FFormedJoint& Joint : Decision.Joints)
			{
				Joint.Profile = *JointOverride;
			}
		}

		/*
		 * What the ghost card may name: the override when given, otherwise the shipped row
		 * the first joint's inferred profile matches — never a pointer into Decision.Joints,
		 * which dies with the caller's copy.
		 *
		 * Both arms are guarded on there being a joint at all: the first brick of every build
		 * is jointless, and a card naming a fastener would falsely promise the piece is
		 * screwed to something. "Bonded to nothing" is a fact about the pose, not a missing
		 * reading, and the card has to be able to say it.
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
		 * Grounded is derived from the pose the piece actually took, never from a caller's
		 * own answer (2026-09-15 DESIGN §8): candidates rank by raw distance, so a cursor on
		 * the grounded course beside a standing brick is pulled up onto that brick's bed, and
		 * a piece flagged grounded in mid-air would be a brick that can never fall — a lie
		 * every piece stacked on it inherits.
		 *
		 * One joint of tolerance, edge inclusive, so both conventions this project lays
		 * bricks under read grounded.
		 *
		 * The one comparison deliberately not in the house `!(x > y)` form: every comparison
		 * against NaN is false, so `!(Bottom > Joint)` would answer true for a non-finite pose
		 * — fail-open in the expensive direction. `Bottom <= Joint` answers false for a NaN
		 * instead, so garbage arithmetic lands a piece that is not grounded.
		 */
		const double BottomFaceZCm = Chosen->CentreCm.Z - ExtentCm.Z;
		Decision.bGrounded = BottomFaceZCm <= Settings.JointThicknessCm;

		return Decision;
	}

	/**
	 * Hand every piece the last solve stopped holding up to physics.
	 *
	 * The caller must already have solved: ApplyResults refuses to release a piece the last
	 * solve has no answer for, since EPieceSupport::Falling is also what an absent answer
	 * reads as — an unsolved wall would otherwise drop entire, permanently, via the one-way
	 * latch. Both callers discharge that obligation before they arrive.
	 *
	 * The walk is over every released piece, not this call's: ApplyResults answers how many
	 * it released, not which, so bricks released by an earlier push are revisited here —
	 * harmless because ABrickActor::Release is idempotent and returns early on a falling brick.
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

			/* GetActor already answers null for a removed piece, so the Cast is the only check needed. */
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

	/*
	 * The producer lays the wall, and nothing here re-derives any of it: RunningBond emits
	 * the boxes and the solved-ready graph together, indexed by the same handles, and this
	 * spawns one actor per box and hands the lot to AdoptLayout. A second opinion about where
	 * a brick goes or which pairs touch would be a second producer, and the whole point of
	 * the split in Core/Layout is that there is one.
	 */
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
	 * The layout is validated before a single brick is spawned, and refused whole if its
	 * arrays are out of step.
	 *
	 * AdoptLayout below already refuses a Boxes array that is not one-per-piece — but by
	 * then the spawn loop has run, so too many boxes has already littered the world with
	 * actors naming no structure. Too few is worse: the spawn loop indexes
	 * Layout.Boxes[PieceIndex] across the whole range, so a short array aborts the process
	 * before AdoptLayout can refuse anything. Checking `!=` here turns both into a clean
	 * refusal that spawns nothing.
	 *
	 * An empty layout is refused too, matching AdoptLayout's own door: spending an id on a
	 * structure nothing will ever name is the same fail-open.
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
		/* Each brick is told its own identity at spawn, so no actor-to-handle map is needed. */
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

	/*
	 * Adoption is the only route in, and it refuses rather than adopting anything out of
	 * step. Writing the replay loop here instead would put the two-arrays-in-lockstep code
	 * at a call site nothing checks — what FStructureBinding exists to outlaw.
	 */
	if (!AdoptLayout(Layout, Actors, *Binding))
	{
		return INDEX_NONE;
	}

	/* The id is only spent once the structure exists, so a refused build leaves the numbering untouched. */
	NextStructureId = StructureId + 1;
	Structures.Add(StructureId, MoveTemp(Binding));

	return StructureId;
}

int32 UDestructionStructureSubsystem::BeginBuild()
{
	/*
	 * Open an empty live structure: the only door to one that starts with nothing and grows
	 * one placed piece at a time. Mirrors BuildLayout's id discipline but adopts no layout —
	 * the binding is created empty and PlaceBuildPiece fills it.
	 */
	const int32 StructureId = NextStructureId;

	TUniquePtr<FStructureBinding> Binding = MakeUnique<FStructureBinding>();
	Binding->StructureId = StructureId;

	/*
	 * A player's build is three-dimensional, stated at the door and never inferred: the
	 * player can rotate a piece at any moment, and a rotated snap forms Y-normal head joints
	 * the 2D X-Z oracle refuses for the whole problem, silently demoting the break authority
	 * from the LP to the router.
	 *
	 * Unconditional, because flagging on the first rotation is exactly the inference
	 * FStructure::SetThreeDimensional's contract rules out (the E3 ruling), and would put a
	 * cliff in the middle of a build where the brick that lands moves the authority deciding
	 * whether the wall stands.
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

	/* An id that names nothing places nothing, the same fail-closed shape as every other door. */
	if (Binding == nullptr)
	{
		return FPieceRef{};
	}

	/* The snap decision is the shared helper; only the world-add lives here, so a placement lands where its own preview said it would. */
	const FBuildPlacement Decision =
		ComputeBuildPlacement(*Binding, RequestedCentreCm, ExtentCm, Material, Placement, JointOverride);

	/* A pose the solver would not name places nothing, the same refusal an unknown id gets. */
	if (!Decision.bDecided)
	{
		return FPieceRef{};
	}

	const FPieceBox Box{ Decision.CentreCm, ExtentCm };

	/* Handles are sequential, so the actor can be told its ref before AddPiece runs, as BuildLayout does. */
	FPieceRef Ref;
	Ref.StructureId = StructureId;
	Ref.PieceIndex = Binding->NumPieces();

	ABrickActor* Actor = SpawnBrickForPiece(*GetWorld(), Box, Decision.MassKg, Ref, &Material);

	const int32 Handle = Binding->AddPiece(Decision.MassKg, Decision.bGrounded, Actor, Box, &Material);

	/* Fails closed: AddPiece refuses a degenerate box with INDEX_NONE, so the just-spawned actor is destroyed rather than left an orphan. */
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

	/* Fails closed: an unknown structure id previews nothing. */
	if (Binding == nullptr)
	{
		return FBuildPreview{};
	}

	/* The same decision PlaceBuildPiece commits, and nothing more: reads the binding const, mutates neither it nor the world. */
	const FBuildPlacement Decision =
		ComputeBuildPlacement(*Binding, RequestedCentreCm, ExtentCm, Material, Placement, JointOverride);

	/* A pose the solver would not name previews nothing, exactly as the commit would place nothing. */
	if (!Decision.bDecided)
	{
		return FBuildPreview{};
	}

	FBuildPreview Preview;
	Preview.bValid = true;
	Preview.Kind = Decision.Kind;
	Preview.CentreCm = Decision.CentreCm;
	Preview.JointCount = Decision.Joints.Num();

	/* The same pose-derived answer the commit will store on the piece, shown before the click. */
	Preview.bGrounded = Decision.bGrounded;

	/* And the library row every one of those joints will carry — see FBuildPreview::JointProfile. */
	Preview.JointProfile = Decision.JointProfile;

	return Preview;
}


int32 UDestructionStructureSubsystem::SolveAndPush(int32 StructureId)
{
	FStructureBinding* Binding = Find(StructureId);

	/* An id that names nothing releases nothing, here and in every binding we own. */
	if (Binding == nullptr)
	{
		return 0;
	}

	/*
	 * The solve comes first, and that order is the guard: ApplyResults refuses to release a
	 * piece the last solve has no answer for, so a freshly built wall with nothing solved
	 * would otherwise drop entire via the one-way latch.
	 *
	 * It settles rather than merely solving — the seam DESIGN.md §3's own rule had left out.
	 * SolveLoads breaks nothing however far a joint is over capacity, while both commit doors
	 * run SolveAndBreak, so a wall that could not hold itself up the moment it was laid stood
	 * indefinitely and then shed on the first click anywhere in it, crediting the player with
	 * a collapse they had not caused.
	 *
	 * (2026-09-02: the ragged-wall figure once cited here is retired — the dry-joint edge
	 * rule now stands a running-bond ragged wall entirely. The seam's point now rides a bare
	 * dry cantilever arm instead, which sheds on spawn — see
	 * `World.Push.AWallOverCapacityDoesNotWaitForAClick`.)
	 *
	 * The price is paid only by structures that were never standing: a wall under capacity is
	 * untouched bit for bit, since SolveAndBreak's last act is a complete solve that broke
	 * nothing. Settling is the only thing that changes, and a settled answer is what
	 * ApplyResults wants.
	 */
	Binding->SolveAndBreak();

	return PushSolvedResultsToWorld(*Binding);
}

FPieceHit UDestructionStructureSubsystem::TracePiece(const FVector& StartCm, const FVector& EndCm)
{
	FPieceHit Hit;

	FHitResult TraceResult;

	/* ECC_Visibility because that is the channel the player's own click uses. */
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

	/* The floor, the sky and everything else in the world leave here: only a brick carries a ref. */
	const ABrickActor* Brick = Cast<ABrickActor>(TraceResult.GetActor());

	if (Brick == nullptr)
	{
		return Hit;
	}

	/*
	 * The ref the brick carries is an actor's claim, not an answer, so it is resolved against
	 * the structure it names before being handed back. A brick whose structure this
	 * subsystem no longer holds fails the whole hit closed, rather than a handle of
	 * INDEX_NONE a caller must remember to check.
	 */
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
	/*
	 * An id that names nothing commits nothing, checked here rather than by sweeping every
	 * binding — the same shape as SolveAndPush, and what a click on the floor arrives as: a
	 * wholly default ref.
	 */
	FStructureBinding* Binding = Find(Ref.StructureId);

	if (Binding == nullptr)
	{
		return false;
	}

	const FPieceActionResult Result = RunPieceAction(*Binding, Ref, Action);

	/*
	 * Where ActorToDestroy is finally consumed: RunPieceAction is world-free and hands the
	 * orphan back rather than destroying it, so without this a deleted brick's mesh stays
	 * standing — a collider nothing in the model knows about. A commit that did nothing
	 * hands back nothing, so no second check on whether it ran is needed.
	 */
	if (AActor* Orphan = Cast<AActor>(Result.ActorToDestroy))
	{
		Orphan->Destroy();
	}

	/*
	 * The answer is pushed onto the world — the line whose absence a player found in ten
	 * seconds: RunPieceAction re-solves, so bricks above a deleted one had lost the ground
	 * and nothing told them, hanging kinematic in the air.
	 *
	 * The push half only, not SolveAndPush, now a correctness rule as well as a cost one:
	 * RunPieceAction already settles the wall, so solving again doubles the cost of a click,
	 * and cascading again would stamp a second collapse for one click. Unconditional, since
	 * a commit that ran nothing settled an unchanged wall and ApplyResults answers zero.
	 */
	PushSolvedResultsToWorld(*Binding);

	return Result.bRan;
}

int32 UDestructionStructureSubsystem::CommitPieceActionForAll(
	TArrayView<const FPieceRef> Refs,
	const FPieceAction& Action)
{
	/*
	 * A selection is built by clicking one wall, so the first ref's structure is as good as
	 * any. An empty selection commits nothing; refs naming anything else are refused piece
	 * by piece by the re-resolve inside RunPieceActions.
	 */
	FStructureBinding* Binding = Refs.Num() > 0 ? Find(Refs[0].StructureId) : nullptr;

	if (Binding == nullptr)
	{
		return 0;
	}

	const FPieceBatchActionResult Result = RunPieceActions(*Binding, Refs, Action);

	/* Where the orphans are finally consumed, one per piece that ran, since RunPieceActions hands them back rather than destroying them. */
	for (UObject* const Orphan : Result.ActorsToDestroy)
	{
		if (AActor* Actor = Cast<AActor>(Orphan))
		{
			Actor->Destroy();
		}
	}

	/*
	 * One push, behind the one settle — the whole point of batching rather than looping
	 * CommitPieceAction. RunPieceActions settles exactly once, after the last action ran, so
	 * this pushes an answer that saw every removal; a push behind a mistimed settle would
	 * leave the batch's orphaned pieces hanging in the air.
	 *
	 * The push half only, not SolveAndPush, for the reason the single-piece commit gives.
	 * Unconditional, because a batch that ran nothing settled an unchanged wall.
	 */
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
	/* An id that names nothing tears down nothing, answering false — the same fail-closed shape every other door takes. */
	FStructureBinding* Binding = Find(StructureId);

	if (Binding == nullptr)
	{
		return false;
	}

	/*
	 * Every actor the binding still names is destroyed, the same idiom the commit doors use:
	 * Cast the actor and destroy only what survives it. Iterating the handle range is
	 * correct because GetActor fails closed on a tombstoned handle, so a removed piece is
	 * simply skipped.
	 */
	for (int32 PieceIndex = 0; PieceIndex < Binding->NumPieces(); ++PieceIndex)
	{
		if (ABrickActor* Brick = Cast<ABrickActor>(Binding->GetActor(PieceIndex)))
		{
			Brick->Destroy();
		}
	}

	/*
	 * The map entry is dropped last, so Find answers null. NextStructureId is left where it
	 * is — ids are monotonic and never reused, so a stale ref can never resolve against a
	 * later structure taking the same slot.
	 */
	Structures.Remove(StructureId);

	return true;
}
