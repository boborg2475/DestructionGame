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
 * The piece a world trace hit. A miss, a foreign brick or a removed piece all give the default for
 * both fields.
 */
struct FPieceHit
{
	FPieceRef Ref;

	int32 PieceHandle = INDEX_NONE;
};

/**
 * The snap decision PlaceBuildPiece would commit at a pose, for the ghost preview. Valid only until
 * the binding changes. bValid means a pose was chosen, not that a piece would land: a degenerate
 * extent previews valid but the commit fails closed.
 */
struct FBuildPreview
{
	bool bValid = false;

	BuildMode::ESnapKind Kind = BuildMode::ESnapKind::Free;

	FVector CentreCm = FVector::ZeroVector;

	int32 JointCount = 0;

	/**
	 * Whether the snapped pose's bottom face is within one joint thickness of Z = 0 (DESIGN §8). A
	 * property of the pose, not the toolbar course (IsCourseGrounded); they differ when a snap lifts a
	 * course-0 cursor onto a higher bed.
	 */
	bool bGrounded = false;

	/**
	 * The shipped library row the joints would carry: the override, else the first joint's inferred
	 * match, else null. Points at the shipped row, never into the decision, which would dangle.
	 */
	const FConnectionStrength* JointProfile = nullptr;
};

/**
 * The world's structures, one FStructureBinding per id. Boxed in TUniquePtr so pointers from Find stay
 * valid as the map grows.
 */
UCLASS()
class DESTRUCTIONGAME_API UDestructionStructureSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:

	/**
	 * Lay a running-bond wall, spawn one ABrickActor per box, and adopt them.
	 *
	 * @return the new structure's id, or INDEX_NONE if nothing was built.
	 */
	int32 BuildRunningBond(const DestructionLayout::FRunningBondSpec& Spec);

	/**
	 * Spawn one ABrickActor per piece of any layout and adopt them.
	 *
	 * @return the new structure's id, or INDEX_NONE if nothing was built. A refusal spends no id.
	 */
	int32 BuildLayout(const DestructionLayout::FBrickLayout& Layout);

	/**
	 * Register an empty build structure and return its id, for building one piece at a time. An
	 * abandoned build leaks an empty binding until Destroy(id).
	 */
	int32 BeginBuild();

	/**
	 * Place a piece using BuildMode::PlacePiece's snap decision, spawn its ABrickActor and form its
	 * joints. Snap commits the best-ranked candidate; Free the requested pose with no joints. A
	 * non-null JointOverride replaces the inferred profile on new joints only.
	 *
	 * Fails closed: a refused piece (a degenerate box gives a NaN mass) destroys the spawned actor and
	 * returns a default ref. Grounded comes from the snapped pose, never the caller (DESIGN §8). Does
	 * not solve; call SolveAndPush.
	 */
	FPieceRef PlaceBuildPiece(
		int32 StructureId,
		const FVector& RequestedCentreCm,
		const FVector& ExtentCm,
		const DestructionProfiles::FMaterialProfile& Material,
		DestructionSession::EPlacementMode Placement = DestructionSession::EPlacementMode::Snap,
		const FConnectionStrength* JointOverride = nullptr);

	/**
	 * What PlaceBuildPiece would commit with the same arguments, without mutating anything. bValid is
	 * false for an unknown id or a Placement with no candidate.
	 */
	FBuildPreview PreviewBuildPiece(
		int32 StructureId,
		const FVector& RequestedCentreCm,
		const FVector& ExtentCm,
		const DestructionProfiles::FMaterialProfile& Material,
		DestructionSession::EPlacementMode Placement = DestructionSession::EPlacementMode::Snap,
		const FConnectionStrength* JointOverride = nullptr) const;

	/**
	 * Transform that makes the mesh's bounds fill the box; shared by the world spawn and the ghost.
	 * Scale is box size over mesh size (not a hard 100), and the scaled local centre is subtracted so
	 * a corner pivot like SM_Cube's still lands correctly.
	 */
	static FTransform BrickSpawnTransform(const UStaticMesh& BrickMesh, const DestructionLayout::FPieceBox& Box);

	/** The binding for a structure id, or null. */
	FStructureBinding* Find(int32 StructureId);

	/** Const overload for read-only queries such as PreviewBuildPiece. */
	const FStructureBinding* Find(int32 StructureId) const;

	/**
	 * Destroy every actor a structure's binding names and forget the binding.
	 *
	 * @return false for an id that names nothing.
	 */
	bool Destroy(int32 StructureId);

	/**
	 * Settle a structure (break over-capacity joints and redistribute until a pass breaks nothing,
	 * DESIGN.md §3) and hand every unsupported piece to physics. Destructive: joints never heal, so no
	 * what-if query may call it.
	 *
	 * @return pieces released; zero for a settled structure or an unknown id.
	 */
	int32 SolveAndPush(int32 StructureId);

	/**
	 * The live piece a world-space ray hits on ECC_Visibility (the player's click channel), or a
	 * default FPieceHit. Every step fails closed.
	 */
	FPieceHit TracePiece(const FVector& StartCm, const FVector& EndCm);

	/**
	 * Commit an action against a piece and destroy the actor it orphaned (FPieceActionResult::
	 * ActorToDestroy), which world-free RunPieceAction cannot.
	 *
	 * @return false for a ref that names nothing.
	 */
	bool CommitPieceAction(const FPieceRef& Ref, const FPieceAction& Action);

	/**
	 * Run an action against a whole selection, then solve and push once. The push must follow the
	 * solve that saw every removal, or orphaned pieces hang in the air. Refs naming a different
	 * structure from the first are refused.
	 *
	 * @return pieces the action ran against.
	 */
	int32 CommitPieceActionForAll(TArrayView<const FPieceRef> Refs, const FPieceAction& Action);

private:

	TMap<int32, TUniquePtr<FStructureBinding>> Structures;

	int32 NextStructureId = 0;
};
