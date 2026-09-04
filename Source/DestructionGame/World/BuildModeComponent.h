// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Components/ActorComponent.h"
#include "Core/Profiles/MaterialProfiles.h"
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

	/** Start a build: open a live structure on the subsystem and remember its id. */
	void BeginBuild();

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

	/** The material the next placed piece carries. Default ClayBrick. */
	const DestructionProfiles::FMaterialProfile* CurrentMaterial = &DestructionProfiles::ClayBrick;

	/** HALF-extent of the next placed piece. Default is a full 21.5 x 10.25 x 6.5 brick. */
	FVector CurrentExtentCm = FVector(10.75, 5.125, 3.25);

	/** Whether the next placed piece is grounded. The caller sets this; default false. */
	bool bBuildGrounded = false;

	/**
	 * The height in cm of the horizontal build plane the cursor's ray picks a point ON. Raise it to
	 * stack a course; the snap solver then snaps the picked point relative to nearby pieces.
	 */
	double BuildPlaneZCm = 0.0;

protected:

	//~ UActorComponent
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:

	/** Lazily spawn the standalone ghost actor on first preview, or return the existing one. */
	ABrickActor* EnsureGhost();

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
