// Copyright Epic Games, Inc. All Rights Reserved.

#include "World/DestructionStructureSubsystem.h"

#include "CollisionQueryParams.h"
#include "Components/StaticMeshComponent.h"
#include "Core/BuildMode/SnapSolver.h"
#include "Core/Connection.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Engine/HitResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "RequiredContent.h"
#include "World/BrickActor.h"

/*
 * File-local names are spelled for what they are and where they live, deliberately.
 * An anonymous namespace is private to a TRANSLATION UNIT rather than to a file, and a
 * unity build merges many files into one — so two file-local names that collide are a
 * hard compile error between files that never refer to each other. See CURRENT_STATE.md.
 */
namespace
{
	/**
	 * The base-colour asset a piece's structural material paints element 0 with, or null for a
	 * material the shed does not use.
	 *
	 * A PLAIN TABLE KEYED ON POINTER IDENTITY, not on a name string or a per-material class. A
	 * piece's Material is a non-owning pointer into the program-lifetime profile library, so the
	 * ClayBrick and Timber rows compare against their own addresses — the same identity the layout
	 * itself carries. Anything else (a third material, or a piece nobody said what it is made of)
	 * returns null, and the caller leaves the mesh's grey default in place.
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
		 * NO MESH, NO BRICK. The mesh is a hard content reference resolved on the CDO, so
		 * deleting the asset leaves it null rather than failing to compile — and the
		 * sizing below divides by its bounds, which would make an infinite scale out of a
		 * missing asset and put a brick of no known size somewhere plausible.
		 */
		if (BrickMesh == nullptr)
		{
			Brick->Destroy();
			return nullptr;
		}

		Brick->SetPieceRef(Ref);

		/*
		 * MASS AT SPAWN, NOT AT RELEASE, and it is the mass the producer already derived
		 * for this very piece rather than a second derivation from the same box.
		 * DestructionLayout::PieceMassKg is the one place geometry becomes a mass; reading
		 * the piece's own figure back out means there is not even a second CALL to it here
		 * to disagree with the graph the solver is using.
		 *
		 * Setting it before FinishSpawning is what gets it into the body at creation. Doing
		 * it at release instead would leave every intact brick carrying whatever mass the
		 * mesh's volume implies, which is what the solver is emphatically not using.
		 */
		Mesh->SetMassOverrideInKg(NAME_None, static_cast<float>(MassKg), true);

		/*
		 * BASE COLOUR BY STRUCTURAL MATERIAL, ON ELEMENT 0. A piece made of a material the shed
		 * uses wears that material's colour so a brick wall reads brick-red and a timber roof reads
		 * timber-tan; a material the table does not map keeps the mesh's grey default. This is the
		 * look UNDERNEATH the highlight overlay, never the overlay itself, so a brick keeps its
		 * colour when the cursor is nowhere near it.
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
	 * The snap DECISION for a piece placed into a binding, with no mutation and no world.
	 *
	 * THE CONTAINER-INDEPENDENT MIDDLE PlaceBuildPiece AND PreviewBuildPiece SHARE. One gathers
	 * the live pieces to spawn a real brick, the other to draw a ghost — but the decision between
	 * is identical, so it lives here once. NearbyBoxes is the whole live piece array and
	 * NearbyMaterials is parallel to it, so a candidate's OtherPieceIndex is exactly the existing
	 * piece's handle. Both are read through const routes off the binding, which is why this takes
	 * a const reference: it decides, it never places.
	 */
	struct FBuildPlacement
	{
		BuildMode::ESnapKind Kind = BuildMode::ESnapKind::Free;
		FVector CentreCm = FVector::ZeroVector;
		double MassKg = 0.0;
		TArray<BuildMode::FFormedJoint> Joints;
	};

	FBuildPlacement ComputeBuildPlacement(
		const FStructureBinding& Binding,
		const FVector& RequestedCentreCm,
		const FVector& ExtentCm,
		const DestructionProfiles::FMaterialProfile& Material)
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

		/* The solver always offers at least the Free fallback, so Candidates[0] exists. */
		const BuildMode::FSnapCandidate& Chosen = Candidates[0];

		FBuildPlacement Placement;
		Placement.Kind = Chosen.Kind;
		Placement.CentreCm = Chosen.CentreCm;
		Placement.MassKg = PieceMassKg(FPieceBox{ Chosen.CentreCm, ExtentCm }, Material.DensityGramsPerCubicCm);
		Placement.Joints = Chosen.Joints;
		return Placement;
	}

	/**
	 * Hand every piece the last solve stopped holding up to physics.
	 *
	 * THE CALLER MUST ALREADY HAVE SOLVED, AND THE NAME SAYS SO. FStructureBinding::ApplyResults
	 * refuses to release any piece the last solve has no answer for, because
	 * EPieceSupport::Falling is also what an ABSENT answer reads as — so a wall nobody has solved
	 * would otherwise drop entire, foundation included, with the one-way latch making it
	 * permanent. Both callers discharge that obligation before they arrive: SolveAndPush settles on
	 * the line above, and RunPieceAction ends by settling the wall — a cascade whose last act is a
	 * complete solve that broke nothing, which is exactly the settled answer this wants.
	 *
	 * THE WALK IS OVER EVERY RELEASED PIECE, NOT OVER THIS CALL'S. ApplyResults answers how many
	 * it released, not which, and IsReleased is the record — so bricks released by an earlier push
	 * are revisited here. That is correct rather than merely tolerable because ABrickActor::Release
	 * is idempotent: the body's own simulating state is the record it derives from, so a second
	 * call on a falling brick returns early instead of recreating the body and leaving it hanging
	 * still in mid-air.
	 *
	 * @return how many pieces THIS call released.
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

			/*
			 * GetActor already answers null for a removed piece and for an actor destroyed by
			 * any route at all, so the Cast is the only check needed and there is no second
			 * lifetime test here to disagree with the binding's.
			 */
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
	 * THE PRODUCER LAYS THE WALL, AND NOTHING HERE RE-DERIVES ANY OF IT. RunningBond emits
	 * the boxes and the solved-ready graph together, indexed by the same handles; this
	 * spawns one actor per box and hands the lot to AdoptLayout. A second opinion about
	 * where a brick goes or which pairs touch would be a second producer, and the whole
	 * point of the split in Core/Layout is that there is one.
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
	 * THE LAYOUT IS VALIDATED BEFORE A SINGLE BRICK IS SPAWNED, and refused whole if its
	 * arrays are out of step. Two failures live here and one guard closes both.
	 *
	 * AdoptLayout below already refuses a layout whose Boxes array is not one-per-piece — but
	 * by then the spawn loop has run, so a TOO-MANY-boxes layout has already littered the world
	 * with actors that name no structure, and returning INDEX_NONE never destroys them. A
	 * TOO-FEW-boxes layout is worse: the spawn loop indexes Layout.Boxes[PieceIndex] across the
	 * whole 0..PieceCount range, so a short array is read past its end and TArray's range check
	 * aborts the process before AdoptLayout can refuse anything at all. Checking `!=` here — both
	 * directions — turns each of those into a clean refusal that spawns nothing.
	 *
	 * An empty layout is refused as well, matching AdoptLayout's own door: a build with no
	 * pieces is a caller mistake, and spending an id on a structure nothing will ever name is
	 * the same fail-open the box check is guarding against.
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
		/*
		 * EACH BRICK IS TOLD ITS OWN IDENTITY AT SPAWN, which is why there is no
		 * actor-to-handle map anywhere in this subsystem: we spawn the bricks, so the
		 * answer can travel on them. See FPieceRef.
		 */
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
	 * ADOPTION IS THE ONLY ROUTE IN, and it refuses rather than adopting anything already
	 * out of step — a layout whose arrays disagree, or an actor list that is not one per
	 * piece. Writing the replay loop here instead would put the two-arrays-in-lockstep code
	 * at a call site where nothing checks it, which is exactly what FStructureBinding
	 * exists to outlaw.
	 */
	if (!AdoptLayout(Layout, Actors, *Binding))
	{
		return INDEX_NONE;
	}

	/*
	 * THE ID IS ONLY SPENT ONCE THE STRUCTURE EXISTS, so a refused build leaves the
	 * numbering untouched and no id is ever handed to a brick that names nothing.
	 */
	NextStructureId = StructureId + 1;
	Structures.Add(StructureId, MoveTemp(Binding));

	return StructureId;
}

int32 UDestructionStructureSubsystem::BeginBuild()
{
	/*
	 * OPEN AN EMPTY LIVE STRUCTURE. BuildLayout refuses an empty layout, so this is the only
	 * door to a structure that starts with nothing and grows one placed piece at a time. It
	 * mirrors BuildLayout's id discipline — an id is spent only once the structure exists —
	 * but adopts no layout: the binding is created empty and PlaceBuildPiece fills it.
	 */
	const int32 StructureId = NextStructureId;

	TUniquePtr<FStructureBinding> Binding = MakeUnique<FStructureBinding>();
	Binding->StructureId = StructureId;

	NextStructureId = StructureId + 1;
	Structures.Add(StructureId, MoveTemp(Binding));

	return StructureId;
}

FPieceRef UDestructionStructureSubsystem::PlaceBuildPiece(
	int32 StructureId,
	const FVector& RequestedCentreCm,
	const FVector& ExtentCm,
	const DestructionProfiles::FMaterialProfile& Material,
	bool bGrounded)
{
	using namespace DestructionLayout;

	FStructureBinding* Binding = Find(StructureId);

	/* An id that names nothing places nothing, the same fail-closed shape as every other door. */
	if (Binding == nullptr)
	{
		return FPieceRef{};
	}

	/*
	 * THE SNAP DECISION IS THE SHARED HELPER; only the world-add lives here. ComputeBuildPlacement
	 * gathers the live pieces and solves exactly as PreviewBuildPiece does, so a placement lands
	 * where its own preview said it would.
	 */
	const FBuildPlacement Placement =
		ComputeBuildPlacement(*Binding, RequestedCentreCm, ExtentCm, Material);

	const FPieceBox Box{ Placement.CentreCm, ExtentCm };

	/*
	 * Handles are sequential, so the actor can be told its intended ref before AddPiece runs —
	 * exactly how BuildLayout spawns each brick with the index it is about to take.
	 */
	FPieceRef Ref;
	Ref.StructureId = StructureId;
	Ref.PieceIndex = Binding->NumPieces();

	ABrickActor* Actor = SpawnBrickForPiece(*GetWorld(), Box, Placement.MassKg, Ref, &Material);

	const int32 Handle = Binding->AddPiece(Placement.MassKg, bGrounded, Actor, Box, &Material);

	/*
	 * FAILS CLOSED. AddPiece refuses a degenerate box (NaN mass) with INDEX_NONE; on refusal the
	 * just-spawned actor names a piece that will never exist, so it is destroyed and a default ref
	 * returned rather than leaving an orphan in the world.
	 */
	if (Handle == INDEX_NONE)
	{
		if (Actor != nullptr)
		{
			Actor->Destroy();
		}
		return FPieceRef{};
	}

	const BuildMode::FSnapSettings Settings;
	for (const BuildMode::FFormedJoint& Joint : Placement.Joints)
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
	const DestructionProfiles::FMaterialProfile& Material) const
{
	const FStructureBinding* Binding = Find(StructureId);

	/* Fails closed: an unknown structure id previews nothing. */
	if (Binding == nullptr)
	{
		return FBuildPreview{};
	}

	/*
	 * THE SAME DECISION PlaceBuildPiece COMMITS, and nothing more — the shared helper reads the
	 * binding const and mutates neither it nor the world, so this surfaces the snapped kind, pose
	 * and joint count a following place at the same pose would produce.
	 */
	const FBuildPlacement Placement =
		ComputeBuildPlacement(*Binding, RequestedCentreCm, ExtentCm, Material);

	FBuildPreview Preview;
	Preview.bValid = true;
	Preview.Kind = Placement.Kind;
	Preview.CentreCm = Placement.CentreCm;
	Preview.JointCount = Placement.Joints.Num();
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
	 * THE SOLVE COMES FIRST, AND THAT ORDER IS THE GUARD RATHER THAN A STYLE.
	 * FStructureBinding::ApplyResults refuses to release any piece the last solve has no
	 * answer for, because EPieceSupport::Falling is also what an ABSENT answer reads as —
	 * so a freshly built wall with nothing solved would otherwise drop entire, foundation
	 * included, with the one-way latch making it permanent. Solving is what discharges
	 * that obligation, so the push may never be run without it.
	 *
	 * AND IT SETTLES RATHER THAN MERELY SOLVING, WHICH IS THE ONE SEAM THAT HAD BEEN LEFT
	 * OUT OF DESIGN.md §3'S OWN RULE. SolveLoads is documented as non-destructive and
	 * breaks nothing however far a joint is over capacity, while both commit doors run
	 * SolveAndBreak — so a wall that could not hold itself up the moment it was laid stood
	 * there indefinitely and then shed on the first click ANYWHERE in it, and a player was
	 * told they had done something they had not: a 40-course ragged wall reads 1.248 as
	 * built, and deleting one brick thirty courses away from anything overloaded took the
	 * collapse with it. A wall that cannot hold itself up should not stand waiting for a
	 * click.
	 *
	 * (2026-09-02: the ragged-wall figure above is retired — the dry-joint edge rule now
	 * stands a running-bond ragged wall entirely, because a corbel past its bed-face edge
	 * leans on its in-course neighbour across the head joint and the LP finds equilibrium,
	 * so eccentricity alone no longer sheds one. The seam's point is unchanged and now
	 * rides a structure that genuinely cannot stand: a BARE dry cantilever arm, no
	 * neighbour to lean on, sheds its whole arm on spawn — see
	 * `World.Push.AWallOverCapacityDoesNotWaitForAClick`.)
	 *
	 * THE PRICE IS PAID BY STRUCTURES THAT WERE NEVER STANDING. A wall UNDER capacity is untouched bit for bit, because the last thing
	 * SolveAndBreak does is a complete solve that broke nothing: the game mode's own flush
	 * scenario wall reads 0.00495 and settles in zero passes, so this costs it one solve
	 * and nothing else.
	 *
	 * SETTLING IS THE ONLY THING THAT CHANGES; the answer this then pushes is a settled
	 * one rather than a mid-cascade one, which is exactly what ApplyResults wants.
	 */
	Binding->SolveAndBreak();

	return PushSolvedResultsToWorld(*Binding);
}

FPieceHit UDestructionStructureSubsystem::TracePiece(const FVector& StartCm, const FVector& EndCm)
{
	FPieceHit Hit;

	FHitResult TraceResult;

	/*
	 * ECC_Visibility because that is the channel the player's own click will use. A channel
	 * invented for this would be a second answer to "can you see it", and the first thing to
	 * go wrong with two answers is a brick you can see and cannot click.
	 */
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

	/*
	 * THE FLOOR, THE SKY AND EVERYTHING ELSE IN THE WORLD LEAVE HERE. Only a brick carries a
	 * ref, and only a brick can name a piece.
	 */
	const ABrickActor* Brick = Cast<ABrickActor>(TraceResult.GetActor());

	if (Brick == nullptr)
	{
		return Hit;
	}

	/*
	 * THE REF THE BRICK CARRIES IS AN ACTOR'S CLAIM, NOT AN ANSWER, so it is resolved against
	 * the structure it names before any of it is handed back. A brick whose structure this
	 * subsystem no longer holds, or whose piece has since been removed, is a brick standing in
	 * the world for something that is not there — and the whole hit fails closed, rather than
	 * a ref coming back beside a handle of INDEX_NONE for a caller to remember to check.
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
	 * AN ID THAT NAMES NOTHING COMMITS NOTHING, checked here rather than by sweeping every
	 * binding — the same shape as SolveAndPush, and it is what a click on the floor arrives
	 * as: a wholly default ref.
	 */
	FStructureBinding* Binding = Find(Ref.StructureId);

	if (Binding == nullptr)
	{
		return false;
	}

	const FPieceActionResult Result = RunPieceAction(*Binding, Ref, Action);

	/*
	 * AND THIS IS WHERE ActorToDestroy IS FINALLY CONSUMED. RunPieceAction is world-free and
	 * hands the orphan back rather than destroying it, so until something does this a deleted
	 * brick's mesh stays standing in the hole it was deleted from — not merely untidy, but a
	 * collider nothing in the model knows about.
	 *
	 * A commit that did nothing hands back nothing, so there is no second check here for
	 * whether it ran; the result already answers that in the only way that matters.
	 */
	if (AActor* Orphan = Cast<AActor>(Result.ActorToDestroy))
	{
		Orphan->Destroy();
	}

	/*
	 * AND THE ANSWER IS PUSHED ONTO THE WORLD, which is the line whose absence a player found
	 * in ten seconds. RunPieceAction re-solves, so the graph knew perfectly well that the
	 * bricks above a deleted one had lost the ground — and nothing ever told them. They hung
	 * in the air, kinematic, held up by a piece that was no longer there.
	 *
	 * THE PUSH HALF ONLY, NOT SolveAndPush, AND THAT IS NOW A CORRECTNESS RULE AS WELL AS A COST
	 * ONE. RunPieceAction ends by settling the wall, so solving again here would be a second
	 * complete solve per click for an answer already in hand — 30 ms of it at scenario scale,
	 * which doubles the cost of a click for nothing. Cascading again would be worse than
	 * wasteful: breaking is irreversible and stamps the pass numbers a collapse is replayed in,
	 * so one click would stamp twice and the replay would show two collapses.
	 *
	 * Unconditional past this point, for the same reason the destroy above needs no second
	 * check on bRan: a commit that ran nothing settled an unchanged wall, so ApplyResults finds
	 * nothing new to release and answers zero.
	 */
	PushSolvedResultsToWorld(*Binding);

	return Result.bRan;
}

int32 UDestructionStructureSubsystem::CommitPieceActionForAll(
	TArrayView<const FPieceRef> Refs,
	const FPieceAction& Action)
{
	/*
	 * A SELECTION IS BUILT BY CLICKING ONE WALL, so the structure is the one its refs name
	 * and the first of them is as good as any. An empty selection and an id that names
	 * nothing commit nothing, the same shape as the single-piece commit and as SolveAndPush;
	 * refs naming anything else are then refused piece by piece by the re-resolve inside
	 * RunPieceActions, which needs no help from here.
	 */
	FStructureBinding* Binding = Refs.Num() > 0 ? Find(Refs[0].StructureId) : nullptr;

	if (Binding == nullptr)
	{
		return 0;
	}

	const FPieceBatchActionResult Result = RunPieceActions(*Binding, Refs, Action);

	/*
	 * AND THIS IS WHERE THE ORPHANS ARE FINALLY CONSUMED — one per piece that ran, because
	 * RunPieceActions is world-free and hands them back rather than destroying them. Until
	 * something does this, every deleted brick's mesh stays standing in the hole it was
	 * deleted from: not merely untidy, but a collider nothing in the model knows about.
	 */
	for (UObject* const Orphan : Result.ActorsToDestroy)
	{
		if (AActor* Actor = Cast<AActor>(Orphan))
		{
			Actor->Destroy();
		}
	}

	/*
	 * ONE PUSH, BEHIND THE ONE SETTLE, AND THAT ORDERING IS THE WHOLE POINT OF BATCHING HERE
	 * RATHER THAN LOOPING CommitPieceAction. RunPieceActions settles exactly once and does it
	 * after the LAST action ran, so this is pushing an answer that saw every removal AND every
	 * joint that gave because of them; FStructureBinding::ApplyResults refuses to release a
	 * piece the last solve has no answer for, so a push behind a mistimed settle leaves the
	 * pieces the batch orphaned hanging in the air — the exact defect a player found in ten
	 * seconds, reintroduced by a batch.
	 *
	 * THE PUSH HALF ONLY, NOT SolveAndPush, for the reason the single-piece commit gives: the
	 * answer is already in hand, a second full solve doubles the cost of a click for nothing,
	 * and a second CASCADE would stamp a second collapse for one click. Unconditional, because
	 * a batch that ran nothing settled an unchanged wall and ApplyResults then finds nothing
	 * new to release.
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
	/*
	 * AN ID THAT NAMES NOTHING TEARS DOWN NOTHING, and answers false so a caller can tell a
	 * teardown that happened from one that had nothing to do — the same fail-closed shape
	 * Find, SolveAndPush and CommitPieceAction all take against an unknown id.
	 */
	FStructureBinding* Binding = Find(StructureId);

	if (Binding == nullptr)
	{
		return false;
	}

	/*
	 * EVERY ACTOR THE BINDING STILL NAMES IS DESTROYED, and this is the same idiom the two
	 * commit doors use to consume their orphans: Cast the actor, which the binding's weak
	 * pointer already answers null for a piece removed or destroyed by any other route, and
	 * Destroy only what survives the cast. Iterating the handle range rather than a live count
	 * is correct because GetActor fails closed on a tombstoned handle, so a removed piece is
	 * simply skipped by the null cast rather than needing a separate check.
	 */
	for (int32 PieceIndex = 0; PieceIndex < Binding->NumPieces(); ++PieceIndex)
	{
		if (ABrickActor* Brick = Cast<ABrickActor>(Binding->GetActor(PieceIndex)))
		{
			Brick->Destroy();
		}
	}

	/*
	 * THE MAP ENTRY IS DROPPED LAST, so Find answers null and a ray along a former piece hits
	 * nothing. NextStructureId is left where it is — ids are monotonic and never reused, so a
	 * ref left over from a torn-down structure can never resolve against a later one that
	 * happened to take the same slot.
	 */
	Structures.Remove(StructureId);

	return true;
}
