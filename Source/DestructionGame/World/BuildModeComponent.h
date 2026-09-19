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
 * Build-mode UI-4a — the interactive build loop's testable core.
 *
 * The component input will drive: it holds the live build's StructureId, moves a translucent
 * ghost actor to the snapped pose a click would land on, and commits that pose on confirm. It
 * invents no physics — every decision is the subsystem's proven snap brain. Real mouse/key
 * binding is a later thin slice; this is the logic that binding calls, unit/world-testable
 * without simulated input.
 *
 * The ghost is not a bound ABrickActor: adopted into the build structure it would become a
 * neighbour of itself and skew the very snap it is previewing, so it is standalone, moved on
 * preview and never handed to the solver.
 */
UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class DESTRUCTIONGAME_API UBuildModeComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UBuildModeComponent();

	/**
	 * Start a build: open a live structure on the subsystem and remember its id.
	 *
	 * A build already open is cancelled first: opening a new build is the player saying "not
	 * that one", and adopting a fresh id would leave the old binding's bricks standing,
	 * owned by nobody.
	 */
	void BeginBuild();

	/**
	 * Abandon the live build: destroy its structure — bricks and binding together — hide the
	 * ghost, drop the held pose and forget the id.
	 *
	 * The cancel path the subsystem says its caller owns: BeginBuild spends an id on an empty
	 * binding nothing else ever tears down. The ghost is hidden rather than destroyed — the
	 * next build reuses it, and EndPlay owns its lifetime.
	 */
	void CancelBuild();

	/**
	 * Choose which piece the next placement lays, deriving the material, the half extent and
	 * the build plane from the palette.
	 *
	 * One door, because three fields must agree: setting CurrentExtentCm by hand and leaving
	 * BuildPlaneZCm stale would preview a board half buried in the course below.
	 */
	void SetPieceKind(DestructionSession::EBuildPieceKind Kind);

	/** Which piece the next placement lays. Default Brick. */
	DestructionSession::EBuildPieceKind GetPieceKind() const;

	/**
	 * Choose which course the build plane sits on, re-deriving the plane for the current piece.
	 *
	 * The clamped course is what is stored: a below-ground course is course 0 everywhere in
	 * this vocabulary, so keeping the raw value would let the getter report a course the
	 * plane does not belong to.
	 */
	void SetCourse(int32 Course);

	/** Which course the build plane is on. Never negative. Default 0, the grounded course. */
	int32 GetCourse() const;

	/**
	 * Lay the next piece the other way round: re-derive its half extent with X and Y swapped.
	 *
	 * The swapped extent is the rotation, the only representation of it: nothing below this
	 * component knows what an angle is — FPieceBox is an axis-aligned centre and a half
	 * extent — so a flag remembered without the swap would go on previewing a stretcher.
	 *
	 * Z is untouched, so the build plane does not move: a plane re-derived from the swapped X
	 * would lay every rotated piece 1.875 cm into the earth on a readout that never mentions it.
	 */
	void SetRotated(bool bInRotated);

	/** Whether the next piece lies the other way round. Default false, along X. */
	bool IsRotated() const;

	/**
	 * Choose whether the next placement is pulled onto the bond or dropped where the cursor is.
	 *
	 * A setter over a field that is still public (S4 decided the controller owns the choice,
	 * fixtures need to reach it), and the setter is what production calls: a toolbar click
	 * that assigned it directly would change the next click's lay without moving the ghost.
	 */
	void SetPlacementMode(DestructionSession::EPlacementMode Mode);

	/** Choose what fastens the joints the next placement forms. The same setter-over-field shape. */
	void SetJointChoice(DestructionSession::EJointChoice Choice);

	/**
	 * Re-drive the held preview at the cursor it is already holding, with no new pointer event.
	 *
	 * The held cursor is put back on the current build plane — (LastCursorCm.X,
	 * LastCursorCm.Y, BuildPlaneZCm) — rather than replayed verbatim, since a literal replay
	 * would make `Course up` a click that lights a chip and moves nothing.
	 *
	 * Refuses when nothing is held, the fail-closed half: LastCursorCm is the world origin on
	 * a fresh component, so a refresh that ran regardless would hold a ghost nobody placed
	 * and let the next click commit it unseen.
	 *
	 * @return whether a valid preview is held after the refresh; false when there was none to refresh.
	 */
	bool RefreshPreview();

	/**
	 * Preview at a world cursor: ask the subsystem what a click here would place, move the ghost
	 * to that snapped centre and show it when the preview is valid (hide it otherwise), remember
	 * the cursor for ConfirmPlace, and return the preview.
	 */
	FBuildPreview UpdatePreviewAt(const FVector& WorldCursorCm);

	/**
	 * Preview from a world ray: intersect it with the horizontal build plane
	 * (Z == BuildPlaneZCm) and, when it meets the plane in front of the origin, drive
	 * UpdatePreviewAt there. A parallel ray or one meeting it behind the origin hides the
	 * ghost and returns an invalid preview. Pure ray-plane geometry — no world trace.
	 */
	FBuildPreview UpdatePreviewFromRay(const FVector& RayOriginCm, const FVector& RayDirectionCm);

	/** Commit the last-previewed cursor pose through PlaceBuildPiece and return the new ref. */
	FPieceRef ConfirmPlace();

	/**
	 * Show nothing and hold nothing: hide the ghost and drop the held preview.
	 *
	 * The one spelling of failing closed, since there are six ways to reach it (four ray
	 * misses, a cancel, and leaving build mode), and a site that hid the ghost but left
	 * bHasValidPreview set would let a confirm commit a pose nobody can see.
	 *
	 * Hides a ghost that exists and never spawns one: spawning here on the first BeginBuild
	 * would leave a brick standing on an empty plot before the player laid anything.
	 *
	 * Public because leaving build mode is one of those ways: the toolbar switches to Destroy
	 * while a ghost is up, and CancelBuild is not an option — it would take the build with it.
	 */
	void HidePreview();

	/** The translucent preview actor the component owns, or null before the first preview. */
	AActor* GetGhostActor() const;

	/** The live build's structure id, or INDEX_NONE before BeginBuild. */
	int32 GetStructureId() const;

	/**
	 * The material the next placed piece carries, and its half extent.
	 *
	 * Both are derived from the piece kind — SetPieceKind is the door — so neither is
	 * written down here a second time. Public because the ray fixtures and the ghost read them.
	 */
	const DestructionProfiles::FMaterialProfile* CurrentMaterial = &DestructionProfiles::ClayBrick;

	FVector CurrentExtentCm = FVector::ZeroVector;

	/**
	 * Whether the next placement is pulled onto the bond or dropped exactly where the cursor is —
	 * the toolbar's Snap/Free pair, passed straight through to the preview and the commit.
	 */
	DestructionSession::EPlacementMode PlacementMode = DestructionSession::EPlacementMode::Snap;

	/**
	 * What fastens every joint the next placement forms — the toolbar's six joint chips, turned into
	 * a profile by DestructionSession::JointOverrideFor on the way to both subsystem doors.
	 *
	 * A bare public field, exactly like PlacementMode and for the same reason (S4): the
	 * controller's FSessionToolbarState owns the choice, and it is public so fixtures can
	 * set it directly.
	 */
	DestructionSession::EJointChoice JointChoice = DestructionSession::EJointChoice::Auto;

	/**
	 * The height in cm of the horizontal build plane the cursor's ray picks a point on.
	 *
	 * Derived from the course and the current piece's half height
	 * (DestructionSession::CoursePlaneZCm). Stays writable because the ray fixtures set it
	 * directly; the next SetCourse or SetPieceKind takes it back.
	 */
	double BuildPlaneZCm = 0.0;

	/**
	 * The farthest a ray-plane hit may be from the ray origin before it is treated as a miss.
	 * A near-grazing ray slips the parallel guard yet solves to an enormous t, previewing a
	 * valid pose thousands of km away; clamping the pick distance fails that closed.
	 */
	double MaxPickDistanceCm = 100000.0;

protected:

	//~ UActorComponent
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:

	/** Lazily spawn the standalone ghost actor on first preview, or return the existing one. */
	ABrickActor* EnsureGhost();

	/**
	 * Re-derive the material, the half extent and the build plane from the kind, the rotation and
	 * the course.
	 *
	 * One derivation site: three doors change one input each, and every one changes the
	 * answer — a rotation applied inside SetRotated alone would be undone by the next
	 * palette click.
	 *
	 * Ends by refreshing the held preview, why the placement and joint setters come through
	 * here too though they derive nothing: every settings door converges on this one, so "a
	 * settings change moves the ghost" is one line rather than a rule each setter must remember.
	 */
	void ApplyPalette();

	DestructionSession::EBuildPieceKind CurrentKind = DestructionSession::EBuildPieceKind::Brick;

	/** Never negative: SetCourse clamps, so the getter and the plane cannot name different courses. */
	int32 CurrentCourse = 0;

	bool bRotated = false;

	int32 StructureId = INDEX_NONE;

	FVector LastCursorCm = FVector::ZeroVector;

	/** Whether the last UpdatePreviewAt held a valid preview. ConfirmPlace fails closed on this. */
	bool bHasValidPreview = false;

	UPROPERTY()
	TObjectPtr<AActor> GhostActor = nullptr;
};
