// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Components/ActorComponent.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/SessionToolbar.h"
#include "Core/StructureBinding.h"
#include "World/DestructionStructureSubsystem.h"

#include "BuildModeComponent.generated.h"

class ABrickActor;

/**
 * Build-mode UI-4a: the testable core of the interactive build loop. Holds the live build's
 * StructureId, moves a translucent ghost to the snapped pose a click would land on, and commits
 * it on confirm. All snap decisions are the subsystem's.
 *
 * The ghost is standalone, never handed to the solver: bound into the build it would neighbour
 * itself and skew the snap it previews.
 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class DESTRUCTIONGAME_API UBuildModeComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UBuildModeComponent();

	/** Open a live structure on the subsystem and remember its id. Cancels any open build first so its bricks are not orphaned. */
	void BeginBuild();

	/**
	 * Abandon the live build: destroy its structure (bricks and binding), hide the ghost, drop the
	 * held pose and forget the id. The ghost is hidden, not destroyed; EndPlay owns it.
	 */
	void CancelBuild();

	/**
	 * Choose the next piece, deriving material, half extent and build plane together so they
	 * cannot disagree (a stale plane would bury a board in the course below).
	 */
	void SetPieceKind(DestructionSession::EBuildPieceKind Kind);

	/** Default Brick. */
	DestructionSession::EBuildPieceKind GetPieceKind() const;

	/** Choose the build plane's course (clamped to >= 0 and stored clamped), re-deriving the plane. */
	void SetCourse(int32 Course);

	/** Never negative. Default 0, the grounded course. */
	int32 GetCourse() const;

	/**
	 * Rotate the next piece by swapping X and Y of its half extent; the swapped extent is the only
	 * representation of rotation. Z is unchanged, so the build plane does not move.
	 */
	void SetRotated(bool bInRotated);

	/** Default false, along X. */
	bool IsRotated() const;

	/**
	 * Snap to the bond or drop at the cursor. Production uses this setter rather than the public
	 * field (kept public for fixtures, S4) so the ghost moves too.
	 */
	void SetPlacementMode(DestructionSession::EPlacementMode Mode);

	/** What fastens the next placement's joints. Same setter-over-field shape. */
	void SetJointChoice(DestructionSession::EJointChoice Choice);

	/**
	 * Re-drive the held preview with no new pointer event, at (LastCursorCm.X, LastCursorCm.Y,
	 * BuildPlaneZCm) so a course change moves the ghost. Refuses when nothing is held, since
	 * LastCursorCm starts at the origin.
	 *
	 * @return whether a valid preview is held afterwards.
	 */
	bool RefreshPreview();

	/**
	 * Ask the subsystem what a click here would place, move the ghost there (shown only if
	 * valid), remember the cursor for ConfirmPlace, and return the preview.
	 */
	FBuildPreview UpdatePreviewAt(const FVector& WorldCursorCm);

	/**
	 * Intersect the ray with the plane Z == BuildPlaneZCm and preview there. A parallel ray or a
	 * hit behind the origin hides the ghost and returns an invalid preview. No world trace.
	 */
	FBuildPreview UpdatePreviewFromRay(const FVector& RayOriginCm, const FVector& RayDirectionCm);

	/** Commit the last-previewed pose through PlaceBuildPiece and return the new ref. */
	FPieceRef ConfirmPlace();

	/**
	 * Hide the ghost and drop the held preview together, so a confirm can never commit an unseen
	 * pose. Never spawns a ghost. Public because switching the toolbar to Destroy calls it
	 * without cancelling the build.
	 */
	void HidePreview();

	/** The translucent preview actor the component owns, or null before the first preview. */
	AActor* GetGhostActor() const;

	/** The live build's structure id, or INDEX_NONE before BeginBuild. */
	int32 GetStructureId() const;

	/** Material and half extent of the next piece, derived via SetPieceKind. Public for fixtures and the ghost. */
	const DestructionProfiles::FMaterialProfile* CurrentMaterial = &DestructionProfiles::ClayBrick;

	FVector CurrentExtentCm = FVector::ZeroVector;

	/** The toolbar's Snap/Free choice, passed to preview and commit. */
	DestructionSession::EPlacementMode PlacementMode = DestructionSession::EPlacementMode::Snap;

	/**
	 * The toolbar's joint chip, mapped to a profile by DestructionSession::JointOverrideFor.
	 * Public for fixtures (S4); FSessionToolbarState owns the choice.
	 */
	DestructionSession::EJointChoice JointChoice = DestructionSession::EJointChoice::Auto;

	/**
	 * Height of the build plane, cm (DestructionSession::CoursePlaneZCm). Writable for ray
	 * fixtures; the next SetCourse or SetPieceKind re-derives it.
	 */
	double BuildPlaneZCm = 0.0;

	/** Max ray-plane hit distance; a near-grazing ray otherwise previews a pose kilometres away. */
	double MaxPickDistanceCm = 100000.0;

protected:

	//~ UActorComponent
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:

	/** Lazily spawn the standalone ghost actor on first preview, or return the existing one. */
	ABrickActor* EnsureGhost();

	/**
	 * The single derivation of material, half extent and build plane from kind, rotation and
	 * course. Every settings setter ends here, and it refreshes the held preview so the ghost moves.
	 */
	void ApplyPalette();

	DestructionSession::EBuildPieceKind CurrentKind = DestructionSession::EBuildPieceKind::Brick;

	/** Never negative (SetCourse clamps). */
	int32 CurrentCourse = 0;

	bool bRotated = false;

	int32 StructureId = INDEX_NONE;

	FVector LastCursorCm = FVector::ZeroVector;

	/** Whether a valid preview is held. ConfirmPlace fails closed on this. */
	bool bHasValidPreview = false;

	UPROPERTY()
	TObjectPtr<AActor> GhostActor = nullptr;
};
