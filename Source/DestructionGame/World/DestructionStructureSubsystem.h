// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/BuildMode/SnapSolver.h"
#include "Core/Layout.h"
#include "Core/PieceActions.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/SessionToolbar.h"
#include "Core/StructureBinding.h"
#include "Subsystems/WorldSubsystem.h"
#include "DestructionStructureSubsystem.generated.h"

class UStaticMesh;

/**
 * What a trace into the world found, if it found a piece at all.
 *
 * BOTH FIELDS FAIL CLOSED TOGETHER. A trace that hit the floor, hit a brick belonging to a
 * structure this subsystem no longer holds, hit a brick whose piece has since been removed,
 * or hit nothing at all, all produce the default: a ref naming nothing and no handle. The
 * ref is what a menu and a commit are built from; the handle is what the last step of the
 * chain resolved to, and keeping both is what lets a test see WHICH step failed closed.
 */
struct FPieceHit
{
	FPieceRef Ref;

	int32 PieceHandle = INDEX_NONE;
};

/**
 * What the ghost preview needs: the snap decision PlaceBuildPiece WOULD commit at a requested
 * pose, surfaced without mutating the structure. bValid is false only for an unknown structure
 * id; otherwise it carries the best-ranked candidate's kind, its snapped centre, and how many
 * joints it would form — exactly what a subsequent PlaceBuildPiece at the same pose produces.
 *
 * IT PREDICTS THE COMMIT ONLY WHILE THE BINDING IS UNCHANGED. The prediction holds because the
 * commit runs the SAME gather-and-solve on the SAME pieces; any placement or removal in between
 * moves the answer, so a UI must re-preview after every mutation and treat the commit as
 * authoritative. Note also that bValid means "the id is known and a pose was chosen", NOT "a piece
 * would land": a degenerate extent previews valid with a kind, yet the commit fails closed with a
 * default ref.
 */
struct FBuildPreview
{
	bool bValid = false;

	BuildMode::ESnapKind Kind = BuildMode::ESnapKind::Free;

	FVector CentreCm = FVector::ZeroVector;

	int32 JointCount = 0;

	/**
	 * Whether the SNAPPED pose rests on the earth — the very flag the commit will store on the
	 * piece, shown before the click (DESIGN §8, 2026-09-15).
	 *
	 * IT IS A PROPERTY OF THE POSE, NOT OF THE COURSE THE TOOLBAR IS ON. A piece is grounded when
	 * its bottom face sits at or below one joint thickness above Z = 0, and nothing else decides
	 * it; DestructionSession::IsCourseGrounded reports the toolbar's INTENT and the two genuinely
	 * disagree, because a distance-ranked snap can lift a course-0 cursor onto a next-course bed.
	 */
	bool bGrounded = false;
};

/**
 * The world's structures: one FStructureBinding per built wall, keyed by structure id.
 *
 * TUniquePtr RATHER THAN A FLAT ARRAY OF BINDINGS. Find hands out a pointer, and a
 * caller that cached one across a later Add would be reading freed storage after the
 * array grew. Boxing each binding makes the address stable structurally rather than by
 * convention.
 */
UCLASS()
class DESTRUCTIONGAME_API UDestructionStructureSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:

	/**
	 * Lay a running-bond wall, spawn one ABrickActor per box, and adopt the lot.
	 *
	 * THE SAME CALL THE SCENARIO WILL MAKE, so a test of it exercises the real seam
	 * rather than a parallel one written for testing.
	 *
	 * @return the new structure's id, or INDEX_NONE if nothing was built.
	 */
	int32 BuildRunningBond(const DestructionLayout::FRunningBondSpec& Spec);

	/**
	 * Spawn one ABrickActor per piece of ANY laid layout, and adopt the lot.
	 *
	 * THE GENERAL SEAM BuildRunningBond IS ONE CASE OF. The corbel family, the staircase and
	 * every acceptance wall are laid by producers that are not RunningBond, so a subsystem whose
	 * only door takes a running-bond SPEC cannot stand any of them up — which is why
	 * Tests/CorbelScreenshotTest.cpp hand-rolls a spawn loop of its own.
	 *
	 * @return the new structure's id, or INDEX_NONE if nothing was built. A refusal spends no id.
	 */
	int32 BuildLayout(const DestructionLayout::FBrickLayout& Layout);

	/**
	 * Begin an empty, live build structure: register a new, EMPTY FStructureBinding in
	 * Structures and return its id, so an interactive build can start with nothing and grow one
	 * placed piece at a time. BuildLayout refuses an empty layout because an id spent on a
	 * structure nothing will ever name is fail-open; this is the opposite — the id is spent on a
	 * structure the NEXT call names, and the monotonic-never-reused id discipline is preserved.
	 *
	 * THE CALLER OWNS THE CANCEL PATH: an abandoned build leaks an empty binding until Destroy,
	 * so a UI that opens a build and walks away must Destroy(id) itself. Returns the new id.
	 */
	int32 BeginBuild();

	/**
	 * Run the snap brain against the pieces already in this structure, adopt the piece at the
	 * best-ranked snapped pose, spawn one ABrickActor for it and form that candidate's joints as
	 * real FConnections — growing the world structure by one live, jointed, actor-backed piece.
	 * The snap DECISION is exactly BuildMode::PlacePiece's (SolveSnapCandidates against the live
	 * pieces' boxes/materials, Candidates[0] — for a Snap placement; a Free placement takes the
	 * solver's Free candidate by Kind instead); only the world-add differs (it spawns the actor
	 * and grows the binding rather than a bare FBrickLayout).
	 *
	 * FAILS CLOSED: if the piece is refused (a degenerate box gives a NaN mass), the just-spawned
	 * actor is destroyed and a default (INDEX_NONE) ref returned, so Boxes/actors never desync
	 * from the piece array. Returns the new piece's FPieceRef, or a default ref on failure/unknown
	 * structure.
	 *
	 * IT DOES NOT SOLVE. A placed piece that cannot stand (a Free brick in mid-air) sits kinematic
	 * with no support answer until something calls SolveAndPush — which matches the plan's
	 * "live structural feedback OFF by default" recommendation; the destroy/run path is what
	 * solves and settles.
	 *
	 * GROUNDED IS DERIVED FROM THE SNAPPED POSE, AND THERE IS NO ARGUMENT FOR IT (DESIGN §8,
	 * 2026-09-15). The committed piece is grounded when its bottom face lands at or below one joint
	 * thickness above Z = 0. A caller's own answer is exactly what the ruling forbids: snapping
	 * ranks by raw distance, so a cursor on the grounded course can be pulled onto a neighbour's
	 * bed a whole course up, and a floating piece flagged grounded terminates load at the earth —
	 * it can never fall, and neither can anything stacked on it.
	 *
	 * Placement selects WHICH candidate is committed: Snap takes the best-ranked one, Free takes
	 * the requested pose verbatim, bonded to nothing.
	 */
	FPieceRef PlaceBuildPiece(
		int32 StructureId,
		const FVector& RequestedCentreCm,
		const FVector& ExtentCm,
		const DestructionProfiles::FMaterialProfile& Material,
		DestructionSession::EPlacementMode Placement = DestructionSession::EPlacementMode::Snap);

	/**
	 * The snap decision PlaceBuildPiece WOULD commit at this pose, WITHOUT mutating anything —
	 * no piece added, no actor spawned, no connection formed. It is the same gather-and-solve
	 * PlaceBuildPiece runs (SolveSnapCandidates against the live pieces, under the same Placement
	 * mode), so the returned kind, snapped centre, joint count and pose-derived grounded flag match
	 * the piece a following PlaceBuildPiece at the same pose lands. Fails closed: bValid is false
	 * for an unknown structure id, and for a Placement the solver offered no candidate for.
	 */
	FBuildPreview PreviewBuildPiece(
		int32 StructureId,
		const FVector& RequestedCentreCm,
		const FVector& ExtentCm,
		const DestructionProfiles::FMaterialProfile& Material,
		DestructionSession::EPlacementMode Placement = DestructionSession::EPlacementMode::Snap) const;

	/**
	 * Where to put a brick actor, and how big to scale it, so its MESH bounds fill the box —
	 * the ONE placement formula the world spawn and the build-mode ghost both go through.
	 *
	 * BOTH HALVES ARE READ OFF THE MESH, AND NEITHER IS A CONSTANT. The scale is the box's size
	 * over the mesh's own local size, never over 100 — SM_Cube happens to be authored at 100 uu,
	 * so a hard 100 is right by accident today and silently wrong the day a real brick mesh lands.
	 *
	 * AND THE PIVOT IS NOT ASSUMED TO BE THE CENTRE, because SM_Cube's is not: its local bounds run
	 * from (0, 0, 0) to (100, 100, 100), so its origin is a CORNER. Placing the actor at the box's
	 * centre would put the mesh a half-size out on all three axes while GetActorLocation agreed with
	 * the layout perfectly. Subtracting the scaled local centre puts the mesh's BOUNDS where the box
	 * is, whatever the pivot happens to be — the quantity Tests/BrickActorTest.cpp asserts on, and
	 * the reason a ghost positioned by a bare SetActorLocation(CentreCm) read a half-brick off.
	 */
	static FTransform BrickSpawnTransform(const UStaticMesh& BrickMesh, const DestructionLayout::FPieceBox& Box);

	/** The binding for a structure id, or null. */
	FStructureBinding* Find(int32 StructureId);

	/** Const overload, so a const query (PreviewBuildPiece) can reach the binding read-only. */
	const FStructureBinding* Find(int32 StructureId) const;

	/**
	 * Tear a structure down: destroy every actor its binding still names and forget the
	 * binding, so Find(StructureId) returns null and a ray along a former piece hits nothing.
	 *
	 * THE SCENARIO SWITCHER'S MISSING HALF. The Structures map only grows today, so a second
	 * build leaves the first structure's bricks standing and clickable in the world; a
	 * switcher needs one call that removes them. Destroying the actors AND dropping the map
	 * entry are one operation because a dropped binding whose actors survived is exactly the
	 * orphan-in-the-world failure, and destroyed actors whose binding survives is a Find that
	 * hands back dangling actor pointers.
	 *
	 * @return true if a structure was held under this id and has been destroyed; false for an
	 *         id that names nothing.
	 */
	bool Destroy(int32 StructureId);

	/**
	 * Settle one structure and push the answer onto the world: every piece the solver is
	 * no longer holding up is handed to physics.
	 *
	 * SETTLE, NOT MERELY SOLVE. Every joint over its own capacity gives, the share moves
	 * onto whatever is left, and it repeats until a pass breaks nothing — DESIGN.md §3's
	 * rule, applied here because a wall that cannot hold itself up must not stand waiting
	 * for a click and then come down attributed to whichever brick was clicked. A structure
	 * under capacity is untouched: settling it is one solve and zero breaking passes.
	 *
	 * SO THIS IS DESTRUCTIVE AND IS NOT A READOUT. Joints never heal and the pass stamps
	 * are the record a collapse is replayed from, so nothing asking what-if may call it.
	 *
	 * @return how many pieces THIS CALL released. Zero for a settled structure, and zero
	 *         for a structure id that names nothing.
	 */
	int32 SolveAndPush(int32 StructureId);

	/**
	 * Turn a world-space ray into the piece it hit.
	 *
	 * THE WHOLE CHAIN, AND EVERY STEP FAILS CLOSED: a visibility trace, then
	 * HitResult::GetActor, then a cast to ABrickActor, then the ref the brick carries, then
	 * the binding that ref names, then FStructureBinding::ResolvePiece. Anything that is not
	 * a live piece of a structure this subsystem holds comes back as a default FPieceHit.
	 *
	 * ECC_Visibility because that is the channel the player's own click will use; a channel
	 * invented for this would be a second answer to "can you see it".
	 */
	FPieceHit TracePiece(const FVector& StartCm, const FVector& EndCm);

	/**
	 * Commit an action against a piece and destroy whatever the commit orphaned.
	 *
	 * THIS IS WHERE FPieceActionResult::ActorToDestroy IS CONSUMED, and it is the only place
	 * it can be: RunPieceAction is deliberately world-free and hands the orphan back rather
	 * than destroying it, so without a caller that does the destroying a deleted brick's mesh
	 * stays standing in the world with no piece naming it.
	 *
	 * @return true if the action ran. False for a ref that names nothing, and false without
	 *         destroying anything.
	 */
	bool CommitPieceAction(const FPieceRef& Ref, const FPieceAction& Action);

	/**
	 * Commit an action against a whole selection: run them all, solve once, push once.
	 *
	 * THE BATCHING IS THE REQUIREMENT. RunPieceActions runs every action and then solves
	 * exactly one time; this destroys every orphan it hands back and then pushes that single
	 * answer onto the world. Looping CommitPieceAction instead would solve and push once per
	 * brick — the same final state at N times the cost, which at scenario scale is the
	 * difference between a click and a stutter.
	 *
	 * THE PUSH FOLLOWS THE SOLVE THAT SAW EVERY REMOVAL, and that ordering is load-bearing
	 * rather than tidy: FStructureBinding::ApplyResults refuses to release a piece the last
	 * solve has no answer for, so pushing against an answer computed before the last action
	 * ran leaves the pieces that action orphaned hanging in the air — the exact failure a
	 * player found in ten seconds, reintroduced by a batch.
	 *
	 * EVERY REF MUST NAME THE SAME STRUCTURE, which is what a selection built by clicking one
	 * wall is; refs naming a structure this subsystem does not hold contribute nothing and are
	 * refused piece by piece, exactly as the single-piece commit refuses them.
	 *
	 * @return how many pieces the action actually ran against. Zero for an empty selection and
	 *         for a selection none of whose refs resolve.
	 */
	int32 CommitPieceActionForAll(TArrayView<const FPieceRef> Refs, const FPieceAction& Action);

private:

	TMap<int32, TUniquePtr<FStructureBinding>> Structures;

	int32 NextStructureId = 0;
};
