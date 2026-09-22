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
 * What a trace into the world found, if it found a piece at all. Both fields fail closed together:
 * a miss, a foreign brick or a removed piece all give the default. The ref builds a menu and a
 * commit; the handle lets a test see which step failed closed.
 */
struct FPieceHit
{
	FPieceRef Ref;

	int32 PieceHandle = INDEX_NONE;
};

/**
 * What the ghost preview needs: the snap decision PlaceBuildPiece would commit at a pose, without
 * mutating the structure. bValid is false only for an unknown structure id; otherwise it carries
 * the best candidate's kind, snapped centre and joint count, as a following PlaceBuildPiece produces.
 *
 * Valid only while the binding is unchanged, so a UI must re-preview after every mutation. bValid
 * means the id is known and a pose was chosen, not that a piece would land — a degenerate extent
 * previews valid yet the commit fails closed.
 */
struct FBuildPreview
{
	bool bValid = false;

	BuildMode::ESnapKind Kind = BuildMode::ESnapKind::Free;

	FVector CentreCm = FVector::ZeroVector;

	int32 JointCount = 0;

	/**
	 * Whether the snapped pose rests on the earth, shown before the click (DESIGN §8, 2026-09-15).
	 * A property of the pose, not the toolbar's course: grounded means the bottom face sits within
	 * one joint thickness of Z = 0. IsCourseGrounded reports the toolbar's intent instead, and the
	 * two disagree when a distance-ranked snap lifts a course-0 cursor onto a next-course bed.
	 */
	bool bGrounded = false;

	/**
	 * Which shipped library row the joints would carry, so a ghost card can name it before the
	 * click: the override when given, else the row the first joint's inferred profile matches, null
	 * when no joint forms. A shipped row's address, never a pointer into the decision, which dies
	 * with the call and would dangle.
	 */
	const FConnectionStrength* JointProfile = nullptr;
};

/**
 * The world's structures: one FStructureBinding per built wall, keyed by structure id. TUniquePtr,
 * not a flat array: Find hands out a pointer, so boxing each binding keeps its address stable when
 * the map grows rather than leaving a cached pointer dangling.
 */
UCLASS()
class DESTRUCTIONGAME_API UDestructionStructureSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:

	/**
	 * Lay a running-bond wall, spawn one ABrickActor per box, and adopt the lot. The same call the
	 * scenario makes, so a test exercises the real seam.
	 *
	 * @return the new structure's id, or INDEX_NONE if nothing was built.
	 */
	int32 BuildRunningBond(const DestructionLayout::FRunningBondSpec& Spec);

	/**
	 * Spawn one ABrickActor per piece of any laid layout, and adopt the lot. The general seam
	 * BuildRunningBond is one case of: corbels and acceptance walls come from other producers, so a
	 * door taking only a running-bond spec could not stand them up.
	 *
	 * @return the new structure's id, or INDEX_NONE if nothing was built. A refusal spends no id.
	 */
	int32 BuildLayout(const DestructionLayout::FBrickLayout& Layout);

	/**
	 * Begin an empty, live build structure: register a new, empty FStructureBinding and return its
	 * id, so an interactive build grows one placed piece at a time. BuildLayout refuses an empty
	 * layout; this deliberately spends an id on one the next call names.
	 *
	 * The caller owns the cancel path: an abandoned build leaks an empty binding until Destroy(id).
	 */
	int32 BeginBuild();

	/**
	 * Run the snap brain against this structure's pieces, adopt the piece at the best-ranked snapped
	 * pose, spawn one ABrickActor and form that candidate's joints as real FConnections. The snap
	 * decision is exactly BuildMode::PlacePiece's; only the world-add differs.
	 *
	 * Fails closed: a refused piece (a degenerate box gives a NaN mass) destroys the just-spawned
	 * actor and returns a default ref, so Boxes/actors never desync from the piece array.
	 *
	 * Does not solve: a placed piece sits kinematic with no support answer until SolveAndPush.
	 *
	 * Grounded is derived from the snapped pose, never a caller's answer (DESIGN §8, 2026-09-15): a
	 * distance-ranked snap can pull a grounded-course cursor onto a bed a course up, and a floating
	 * piece flagged grounded would wrongly terminate load at the earth.
	 *
	 * Placement selects the committed candidate: Snap the best-ranked, Free the requested pose
	 * bonded to nothing. A non-null JointOverride fastens every joint this placement forms with that
	 * profile instead of the inferred one; it never re-prices existing joints. Null infers.
	 */
	FPieceRef PlaceBuildPiece(
		int32 StructureId,
		const FVector& RequestedCentreCm,
		const FVector& ExtentCm,
		const DestructionProfiles::FMaterialProfile& Material,
		DestructionSession::EPlacementMode Placement = DestructionSession::EPlacementMode::Snap,
		const FConnectionStrength* JointOverride = nullptr);

	/**
	 * The snap decision PlaceBuildPiece would commit at this pose, without mutating anything. Runs
	 * the same gather-and-solve, so the returned kind, centre, joint count and grounded flag match
	 * the committed piece. Fails closed: bValid is false for an unknown id or a Placement with no
	 * candidate. JointOverride is the commit's too, or a ghost would predict a different structure.
	 */
	FBuildPreview PreviewBuildPiece(
		int32 StructureId,
		const FVector& RequestedCentreCm,
		const FVector& ExtentCm,
		const DestructionProfiles::FMaterialProfile& Material,
		DestructionSession::EPlacementMode Placement = DestructionSession::EPlacementMode::Snap,
		const FConnectionStrength* JointOverride = nullptr) const;

	/**
	 * Where to put a brick actor and how to scale it so its mesh bounds fill the box — the one
	 * placement formula the world spawn and the build-mode ghost share.
	 *
	 * Neither half is a constant. The scale is the box size over the mesh's local size, never a hard
	 * 100: SM_Cube is 100 uu today but a real brick mesh would break that. The pivot is not assumed
	 * centred either — SM_Cube's is a corner (bounds (0,0,0) to (100,100,100)) — so subtracting the
	 * scaled local centre places the bounds correctly whatever the pivot, which a bare
	 * SetActorLocation(CentreCm) got a half-brick wrong.
	 */
	static FTransform BrickSpawnTransform(const UStaticMesh& BrickMesh, const DestructionLayout::FPieceBox& Box);

	/** The binding for a structure id, or null. */
	FStructureBinding* Find(int32 StructureId);

	/** Const overload, so a const query (PreviewBuildPiece) can reach the binding read-only. */
	const FStructureBinding* Find(int32 StructureId) const;

	/**
	 * Tear a structure down: destroy every actor its binding names and forget the binding, so Find
	 * returns null and a ray along a former piece hits nothing. Destroying the actors and dropping
	 * the map entry are one operation — either alone leaves an orphan.
	 *
	 * @return true if a structure was held under this id and destroyed; false for an id that names
	 *         nothing.
	 */
	bool Destroy(int32 StructureId);

	/**
	 * Settle one structure and push the answer onto the world: every piece the solver no longer
	 * holds up is handed to physics.
	 *
	 * Settle, not merely solve: every over-capacity joint gives and its share moves onto what is
	 * left, until a pass breaks nothing (DESIGN.md §3), so a wall that cannot hold itself up comes
	 * down now rather than when a brick is clicked. A structure under capacity costs one solve.
	 *
	 * Destructive: joints never heal, so no what-if query may call it.
	 *
	 * @return pieces released. Zero for a settled structure, and zero for an id that names nothing.
	 */
	int32 SolveAndPush(int32 StructureId);

	/**
	 * Turn a world-space ray into the piece it hit. Every step fails closed: a visibility trace, a
	 * cast to ABrickActor, the brick's ref, the binding it names, then ResolvePiece. Anything not a
	 * live piece of a held structure comes back a default FPieceHit. ECC_Visibility is the channel
	 * the player's own click uses.
	 */
	FPieceHit TracePiece(const FVector& StartCm, const FVector& EndCm);

	/**
	 * Commit an action against a piece and destroy whatever it orphaned. Where
	 * FPieceActionResult::ActorToDestroy is consumed: RunPieceAction is world-free and hands the
	 * orphan back, so without this a deleted brick's mesh stays standing.
	 *
	 * @return true if the action ran. False for a ref that names nothing, destroying nothing.
	 */
	bool CommitPieceAction(const FPieceRef& Ref, const FPieceAction& Action);

	/**
	 * Commit an action against a whole selection: run them all, solve once, push once. The batching
	 * is the requirement — looping CommitPieceAction would solve and push once per brick, same final
	 * state at N times the cost. The push must follow the solve that saw every removal, or pieces
	 * that action orphaned hang in the air.
	 *
	 * Every ref must name the same structure (a selection built by clicking one wall); others are
	 * refused piece by piece.
	 *
	 * @return pieces the action ran against. Zero for an empty selection and for one whose refs none
	 *         resolve.
	 */
	int32 CommitPieceActionForAll(TArrayView<const FPieceRef> Refs, const FPieceAction& Action);

private:

	TMap<int32, TUniquePtr<FStructureBinding>> Structures;

	int32 NextStructureId = 0;
};
