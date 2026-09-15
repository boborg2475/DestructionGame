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
	 * Show nothing and hold nothing: hide the ghost and drop the held preview.
	 *
	 * THE ONE SPELLING OF FAILING CLOSED, because there are now five ways to reach it — four ray
	 * misses and a cancel — and a site that hid the ghost but left bHasValidPreview set would let a
	 * confirm commit a pose nobody can see.
	 */
	void HidePreview();

	DestructionSession::EBuildPieceKind CurrentKind = DestructionSession::EBuildPieceKind::Brick;

	/** Never negative: SetCourse clamps, so the getter and the plane cannot name different courses. */
	int32 CurrentCourse = 0;

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
