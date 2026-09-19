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
 * Both fields fail closed together: a miss, a brick outside this subsystem's structures, or
 * a piece since removed all produce the default — a ref naming nothing and no handle. The
 * ref is what a menu and a commit are built from; keeping the handle too lets a test see
 * which step failed closed.
 */
struct FPieceHit
{
	FPieceRef Ref;

	int32 PieceHandle = INDEX_NONE;
};

/**
 * What the ghost preview needs: the snap decision PlaceBuildPiece would commit at a requested
 * pose, surfaced without mutating the structure. bValid is false only for an unknown structure
 * id; otherwise it carries the best-ranked candidate's kind, its snapped centre, and how many
 * joints it would form — exactly what a following PlaceBuildPiece at the same pose produces.
 *
 * Predicts the commit only while the binding is unchanged, so a UI must re-preview after
 * every mutation. bValid means "the id is known and a pose was chosen", not "a piece would
 * land": a degenerate extent previews valid, yet the commit fails closed.
 */
struct FBuildPreview
{
	bool bValid = false;

	BuildMode::ESnapKind Kind = BuildMode::ESnapKind::Free;

	FVector CentreCm = FVector::ZeroVector;

	int32 JointCount = 0;

	/**
	 * Whether the snapped pose rests on the earth — the flag the commit will store on the
	 * piece, shown before the click (DESIGN §8, 2026-09-15).
	 *
	 * A property of the pose, not the toolbar's course: grounded means the bottom face sits
	 * at or below one joint thickness above Z = 0. DestructionSession::IsCourseGrounded
	 * reports the toolbar's intent instead, and the two genuinely disagree, since a
	 * distance-ranked snap can lift a course-0 cursor onto a next-course bed.
	 */
	bool bGrounded = false;

	/**
	 * Which shipped library row the joints would carry, so a ghost card can say "perpend"
	 * before the click — the override when given, else the row the first joint's inferred
	 * profile matches, null when the placement forms no joint.
	 *
	 * A shipped row's address, never a pointer into the decision: the inferred profile is
	 * returned by value and lives inside the placement decision, which dies with the call —
	 * a pointer into that would dangle the moment the preview returned.
	 */
	const FConnectionStrength* JointProfile = nullptr;
};

/**
 * The world's structures: one FStructureBinding per built wall, keyed by structure id.
 *
 * TUniquePtr rather than a flat array: Find hands out a pointer, and a caller that cached
 * one across a later Add would read freed storage after the array grew. Boxing each binding
 * keeps the address stable structurally rather than by convention.
 */
UCLASS()
class DESTRUCTIONGAME_API UDestructionStructureSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:

	/**
	 * Lay a running-bond wall, spawn one ABrickActor per box, and adopt the lot.
	 *
	 * The same call the scenario will make, so a test of it exercises the real seam.
	 *
	 * @return the new structure's id, or INDEX_NONE if nothing was built.
	 */
	int32 BuildRunningBond(const DestructionLayout::FRunningBondSpec& Spec);

	/**
	 * Spawn one ABrickActor per piece of any laid layout, and adopt the lot.
	 *
	 * The general seam BuildRunningBond is one case of: the corbel family and every
	 * acceptance wall are laid by producers that are not RunningBond, so a subsystem whose
	 * only door takes a running-bond spec cannot stand any of them up.
	 *
	 * @return the new structure's id, or INDEX_NONE if nothing was built. A refusal spends no id.
	 */
	int32 BuildLayout(const DestructionLayout::FBrickLayout& Layout);

	/**
	 * Begin an empty, live build structure: register a new, empty FStructureBinding and
	 * return its id, so an interactive build can start with nothing and grow one placed
	 * piece at a time. BuildLayout refuses an empty layout as fail-open; this is the
	 * opposite — the id is spent on a structure the next call names.
	 *
	 * The caller owns the cancel path: an abandoned build leaks an empty binding until
	 * Destroy, so a UI that opens a build and walks away must Destroy(id) itself.
	 */
	int32 BeginBuild();

	/**
	 * Run the snap brain against the pieces already in this structure, adopt the piece at the
	 * best-ranked snapped pose, spawn one ABrickActor for it and form that candidate's joints
	 * as real FConnections. The snap decision is exactly BuildMode::PlacePiece's; only the
	 * world-add differs, spawning the actor and growing the binding.
	 *
	 * Fails closed: if the piece is refused (a degenerate box gives a NaN mass), the
	 * just-spawned actor is destroyed and a default ref returned, so Boxes/actors never
	 * desync from the piece array.
	 *
	 * Does not solve: a placed piece that cannot stand sits kinematic with no support answer
	 * until something calls SolveAndPush, matching "live structural feedback off by default".
	 *
	 * Grounded is derived from the snapped pose, never from a caller's own answer (DESIGN §8,
	 * 2026-09-15): snapping ranks by raw distance, so a cursor on the grounded course can be
	 * pulled onto a neighbour's bed a course up, and a floating piece flagged grounded would
	 * terminate load at the earth — unable to fall, and neither can anything stacked on it.
	 *
	 * Placement selects which candidate is committed: Snap takes the best-ranked one, Free
	 * takes the requested pose verbatim, bonded to nothing.
	 *
	 * A non-null JointOverride fastens every joint this placement forms with that profile, in
	 * place of the one BuildMode::JointForContact inferred — a substitution, not a different
	 * placement, and it never re-prices joints already in the structure. Null infers.
	 */
	FPieceRef PlaceBuildPiece(
		int32 StructureId,
		const FVector& RequestedCentreCm,
		const FVector& ExtentCm,
		const DestructionProfiles::FMaterialProfile& Material,
		DestructionSession::EPlacementMode Placement = DestructionSession::EPlacementMode::Snap,
		const FConnectionStrength* JointOverride = nullptr);

	/**
	 * The snap decision PlaceBuildPiece would commit at this pose, without mutating anything.
	 * The same gather-and-solve PlaceBuildPiece runs, so the returned kind, centre, joint
	 * count and grounded flag match the piece a following PlaceBuildPiece would land. Fails
	 * closed: bValid is false for an unknown structure id, or a Placement with no candidate.
	 *
	 * JointOverride is the commit's, too: a ghost that previewed the inference over a piece
	 * the click will screw down would be predicting a different structure.
	 */
	FBuildPreview PreviewBuildPiece(
		int32 StructureId,
		const FVector& RequestedCentreCm,
		const FVector& ExtentCm,
		const DestructionProfiles::FMaterialProfile& Material,
		DestructionSession::EPlacementMode Placement = DestructionSession::EPlacementMode::Snap,
		const FConnectionStrength* JointOverride = nullptr) const;

	/**
	 * Where to put a brick actor, and how big to scale it, so its mesh bounds fill the box —
	 * the one placement formula the world spawn and the build-mode ghost both go through.
	 *
	 * Neither half is a constant: the scale is the box's size over the mesh's own local size,
	 * never a hard 100 — SM_Cube happens to be authored at 100 uu today, but that would be
	 * silently wrong the day a real brick mesh lands.
	 *
	 * The pivot is not assumed to be the centre, because SM_Cube's is a corner (bounds run
	 * (0,0,0) to (100,100,100)): placing the actor at the box's centre would put the mesh a
	 * half-size out on every axis. Subtracting the scaled local centre puts the mesh's
	 * bounds where the box is regardless of pivot — why a ghost positioned by a bare
	 * SetActorLocation(CentreCm) read a half-brick off.
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
	 * The scenario switcher's missing half: the Structures map only grows today, so a second
	 * build leaves the first structure's bricks standing and clickable. Destroying the actors
	 * and dropping the map entry are one operation — either alone leaves an orphan.
	 *
	 * @return true if a structure was held under this id and has been destroyed; false for an
	 *         id that names nothing.
	 */
	bool Destroy(int32 StructureId);

	/**
	 * Settle one structure and push the answer onto the world: every piece the solver is
	 * no longer holding up is handed to physics.
	 *
	 * Settle, not merely solve: every joint over capacity gives and the share moves onto
	 * whatever is left, repeating until a pass breaks nothing — DESIGN.md §3's rule, since a
	 * wall that cannot hold itself up must not stand waiting for a click and then come down
	 * attributed to whichever brick was clicked. A structure under capacity costs one solve.
	 *
	 * Destructive, not a readout: joints never heal, so nothing asking what-if may call it.
	 *
	 * @return how many pieces this call released. Zero for a settled structure, and zero
	 *         for a structure id that names nothing.
	 */
	int32 SolveAndPush(int32 StructureId);

	/**
	 * Turn a world-space ray into the piece it hit.
	 *
	 * The whole chain, and every step fails closed: a visibility trace, a cast to
	 * ABrickActor, the ref the brick carries, the binding that ref names, then
	 * FStructureBinding::ResolvePiece. Anything not a live piece of a held structure comes
	 * back as a default FPieceHit.
	 *
	 * ECC_Visibility because that is the channel the player's own click uses.
	 */
	FPieceHit TracePiece(const FVector& StartCm, const FVector& EndCm);

	/**
	 * Commit an action against a piece and destroy whatever the commit orphaned.
	 *
	 * Where FPieceActionResult::ActorToDestroy is consumed: RunPieceAction is world-free and
	 * hands the orphan back rather than destroying it, so without this a deleted brick's mesh
	 * stays standing with no piece naming it.
	 *
	 * @return true if the action ran. False for a ref that names nothing, and false without
	 *         destroying anything.
	 */
	bool CommitPieceAction(const FPieceRef& Ref, const FPieceAction& Action);

	/**
	 * Commit an action against a whole selection: run them all, solve once, push once.
	 *
	 * The batching is the requirement: RunPieceActions solves exactly once, so this destroys
	 * every orphan and pushes one answer. Looping CommitPieceAction instead would solve and
	 * push once per brick — same final state at N times the cost.
	 *
	 * The push follows the solve that saw every removal, load-bearing rather than tidy:
	 * pushing against an answer computed before the last action ran would leave the pieces
	 * that action orphaned hanging in the air.
	 *
	 * Every ref must name the same structure, what a selection built by clicking one wall is;
	 * refs naming anything else are refused piece by piece, as the single-piece commit does.
	 *
	 * @return how many pieces the action actually ran against. Zero for an empty selection and
	 *         for a selection none of whose refs resolve.
	 */
	int32 CommitPieceActionForAll(TArrayView<const FPieceRef> Refs, const FPieceAction& Action);

private:

	TMap<int32, TUniquePtr<FStructureBinding>> Structures;

	int32 NextStructureId = 0;
};
