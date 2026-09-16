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
 * BUILD-MODE UI-4a — the interactive build loop's TESTABLE CORE.
 *
 * The component input will drive: it holds the live build's StructureId, moves a translucent
 * GHOST actor to the snapped pose a click would land on, and commits that pose on confirm. It
 * invents no physics — every decision is the subsystem's proven snap brain (PreviewBuildPiece /
 * PlaceBuildPiece). Real mouse/key binding is a later thin slice; this is the logic that binding
 * calls, so it is unit/world-testable without simulated input.
 *
 * THE GHOST IS NOT A BOUND ABrickActor. A ghost adopted into the build structure would become a
 * neighbour of itself and skew the very snap it is previewing, so it is a standalone actor the
 * component owns, moved on preview and never handed to the solver.
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
	 * A BUILD ALREADY OPEN IS CANCELLED FIRST. Opening a new build is the player saying "not that
	 * one", and a component that simply adopted a fresh id would leave the old binding in the
	 * subsystem's map forever with its bricks standing in the world, owned by nobody.
	 */
	void BeginBuild();

	/**
	 * Abandon the live build: destroy its structure — bricks and binding together — hide the
	 * ghost, drop the held pose and forget the id.
	 *
	 * THE CANCEL PATH THE SUBSYSTEM SAYS ITS CALLER OWNS. BeginBuild spends an id on an empty
	 * binding that nothing else ever tears down, so the component that opened it is the one that
	 * must close it. The ghost is the component's OWN actor and is hidden rather than destroyed —
	 * the next build reuses it, and EndPlay owns its lifetime.
	 */
	void CancelBuild();

	/**
	 * Choose which piece the next placement lays, DERIVING the material, the half extent and the
	 * build plane from the palette.
	 *
	 * ONE DOOR, BECAUSE THREE FIELDS MUST AGREE. A caller that set CurrentExtentCm to a timber
	 * plate by hand and left BuildPlaneZCm where a brick put it would preview a 10 cm board centred
	 * at a 6.5 cm brick's height, half of it buried in the course below. Kind and course are what
	 * the player actually chooses; everything else is derived on the way through.
	 */
	void SetPieceKind(DestructionSession::EBuildPieceKind Kind);

	/** Which piece the next placement lays. Default Brick. */
	DestructionSession::EBuildPieceKind GetPieceKind() const;

	/**
	 * Choose which course the build plane sits on, re-deriving the plane for the current piece.
	 *
	 * THE CLAMPED COURSE IS WHAT IS STORED. A below-ground course is course 0 everywhere in this
	 * vocabulary, and keeping the raw value would let the getter report a course the plane does
	 * not belong to.
	 */
	void SetCourse(int32 Course);

	/** Which course the build plane is on. Never negative. Default 0, the grounded course. */
	int32 GetCourse() const;

	/**
	 * Lay the next piece the other way round: re-derive its half extent with X AND Y SWAPPED.
	 *
	 * THE SWAPPED EXTENT IS THE ROTATION, AND THERE IS NO OTHER REPRESENTATION OF IT. Nothing below
	 * this component knows what an angle is — DestructionLayout::FPieceBox is an axis-aligned centre
	 * and a half extent, the snap solver reads a long axis off those numbers, and the joint
	 * inference classifies a contact from the two boxes and a normal. A flag remembered without the
	 * swap would go on previewing a stretcher.
	 *
	 * AND Z IS UNTOUCHED, SO THE BUILD PLANE DOES NOT MOVE. A quarter turn about Z cannot change how
	 * tall a piece is; a plane re-derived from the swapped X would put a rotated brick's centre at
	 * its own half WIDTH and lay every rotated piece 1.875 cm into the earth, on a course readout
	 * that would never mention it.
	 */
	void SetRotated(bool bInRotated);

	/** Whether the next piece lies the other way round. Default false, along X. */
	bool IsRotated() const;

	/**
	 * Choose whether the next placement is pulled onto the bond or dropped where the cursor is.
	 *
	 * A SETTER OVER A FIELD THAT IS STILL PUBLIC, AND THE SETTER IS WHAT PRODUCTION CALLS. The field
	 * stays where the fixtures can reach it (S4 decided the controller owns the choice), but a
	 * toolbar click that assigned it directly would change what the next click lays without moving
	 * the ghost that is showing the player where it lands — which is the whole of the owner's
	 * complaint.
	 */
	void SetPlacementMode(DestructionSession::EPlacementMode Mode);

	/** Choose what fastens the joints the next placement forms. The same setter-over-field shape. */
	void SetJointChoice(DestructionSession::EJointChoice Choice);

	/**
	 * Re-drive the HELD preview at the cursor it is already holding, with no new pointer event.
	 *
	 * THE HELD CURSOR IS PUT BACK ON THE CURRENT BUILD PLANE — (LastCursorCm.X, LastCursorCm.Y,
	 * BuildPlaneZCm) — rather than replayed verbatim. UpdatePreviewAt takes a world point and the
	 * plane only ever entered through a RAY, so a literal replay would make `Course up` a click that
	 * lights a chip and moves nothing: the one setting whose whole meaning is a height.
	 *
	 * IT REFUSES WHEN NOTHING IS HELD, which is the fail-closed half. The settings are clickable
	 * before the player has pointed at anything, and LastCursorCm is the world ORIGIN on a fresh
	 * component — a refresh that ran regardless would spawn a ghost down there, HOLD that preview and
	 * let the next click commit a brick nobody ever saw. A preview ConfirmPlace has SPENT is not held
	 * either, so a refresh cannot re-arm the one-preview-one-commit rule into a double placement.
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
	 * Preview from a world ray: intersect it with the horizontal build plane Z == BuildPlaneZCm and,
	 * when it meets the plane IN FRONT of the ray origin, drive UpdatePreviewAt at the intersection
	 * point (X, Y, BuildPlaneZCm) and return that snap preview. A ray parallel to the plane, or one
	 * meeting it behind the origin, hides the ghost and returns an invalid preview. Pure ray-plane
	 * geometry — no world trace.
	 */
	FBuildPreview UpdatePreviewFromRay(const FVector& RayOriginCm, const FVector& RayDirectionCm);

	/** Commit the last-previewed cursor pose through PlaceBuildPiece and return the new ref. */
	FPieceRef ConfirmPlace();

	/**
	 * Show nothing and hold nothing: hide the ghost and drop the held preview.
	 *
	 * THE ONE SPELLING OF FAILING CLOSED, because there are now six ways to reach it — four ray
	 * misses, a cancel, and leaving build mode — and a site that hid the ghost but left
	 * bHasValidPreview set would let a confirm commit a pose nobody can see.
	 *
	 * IT HIDES A GHOST THAT EXISTS AND NEVER SPAWNS ONE. The ghost is spawned by the first preview;
	 * a hide that spawned one would put a brick in the world on the first BeginBuild, which cancels
	 * first — and an empty plot with a brick standing on it is a level that lays something after all.
	 *
	 * PUBLIC BECAUSE LEAVING BUILD MODE IS ONE OF THOSE WAYS. The session's toolbar switches to
	 * Destroy while a ghost is up, and a gold brick left hanging in the air over a wall the player
	 * is demolishing is the most confusing thing this UI can do. The controller has no other way to
	 * say it: CancelBuild would take the player's build with it.
	 */
	void HidePreview();

	/** The translucent preview actor the component owns, or null before the first preview. */
	AActor* GetGhostActor() const;

	/** The live build's structure id, or INDEX_NONE before BeginBuild. */
	int32 GetStructureId() const;

	/**
	 * The material the next placed piece carries, and its HALF extent.
	 *
	 * BOTH ARE DERIVED FROM THE PIECE KIND — SetPieceKind is the door, and the constructor seeds
	 * them from the default Brick, so neither the dimensions nor the library row is written down
	 * here a second time. They stay public because the ray fixtures and the ghost read them.
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
	 * A BARE PUBLIC FIELD, EXACTLY LIKE PlacementMode, AND FOR THE SAME DECIDED REASON (S4): the
	 * controller's FSessionToolbarState owns the choice and OnToolbarButton is the only thing in
	 * production that writes this copy of it. It is public so the fixtures can set it directly.
	 */
	DestructionSession::EJointChoice JointChoice = DestructionSession::EJointChoice::Auto;

	/**
	 * The height in cm of the horizontal build plane the cursor's ray picks a point ON.
	 *
	 * DERIVED FROM THE COURSE AND THE CURRENT PIECE'S HALF HEIGHT (DestructionSession::
	 * CoursePlaneZCm), so it moves when either does. It stays writable because the ray fixtures set
	 * it directly; the next SetCourse or SetPieceKind takes it back.
	 */
	double BuildPlaneZCm = 0.0;

	/**
	 * The FARTHEST a ray-plane hit may be from the ray origin before it is treated as a MISS. A
	 * near-grazing ray (a tiny but non-zero Direction.Z) slips the parallel guard yet solves to an
	 * enormous t, previewing a valid pose thousands of km away; clamping the pick distance fails
	 * that closed. 1 km is generous for any real build.
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
	 * ONE DERIVATION SITE, WHICH IS THE POINT OF IT BEING A FUNCTION. Three doors change one of
	 * those three inputs each, and every one of them changes the answer: a rotation applied inside
	 * SetRotated alone would be undone by the next palette click, and a palette click that swapped
	 * without asking the rotation would un-turn the ghost while the strip's chip stayed lit.
	 *
	 * AND IT ENDS BY REFRESHING THE HELD PREVIEW, WHICH IS WHY THE PLACEMENT AND JOINT SETTERS COME
	 * THROUGH HERE TOO THOUGH THEY DERIVE NOTHING. Every settings door in this component converges
	 * on this one, so "a settings change moves the ghost" is one line in one place rather than a
	 * rule five setters have to remember — and the next setter added gets it for free.
	 */
	void ApplyPalette();

	DestructionSession::EBuildPieceKind CurrentKind = DestructionSession::EBuildPieceKind::Brick;

	/** Never negative: SetCourse clamps, so the getter and the plane cannot name different courses. */
	int32 CurrentCourse = 0;

	bool bRotated = false;

	int32 StructureId = INDEX_NONE;

	FVector LastCursorCm = FVector::ZeroVector;

	/**
	 * Whether the last UpdatePreviewAt held a VALID preview. ConfirmPlace fails closed on this so a
	 * confirm before any preview does not commit a brick at the default LastCursorCm (world origin).
	 */
	bool bHasValidPreview = false;

	UPROPERTY()
	TObjectPtr<AActor> GhostActor = nullptr;
};
