// Copyright Epic Games, Inc. All Rights Reserved.

#include "World/BuildModeComponent.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "World/BrickActor.h"

/*
 * The drive loop only: structural decisions belong to PreviewBuildPiece / PlaceBuildPiece.
 * This holds the build id, keeps the ghost on the predicted snap, and forwards a confirm.
 */

namespace
{
	/** The world's structure subsystem, or null for an unregistered component (callers fail closed). */
	UDestructionStructureSubsystem* SubsystemFor(const UBuildModeComponent& Component)
	{
		const UWorld* World = Component.GetWorld();
		return World != nullptr ? World->GetSubsystem<UDestructionStructureSubsystem>() : nullptr;
	}
}

UBuildModeComponent::UBuildModeComponent()
{
	// Seed through SetPieceKind so material, extent and plane are derived in one place.
	SetPieceKind(CurrentKind);
}

void UBuildModeComponent::BeginBuild()
{
	// Cancel any open build so its binding and bricks don't leak; a no-op when none is open.
	CancelBuild();

	if (UDestructionStructureSubsystem* Subsystem = SubsystemFor(*this))
	{
		// Reset the cursor so the old build's last pose can't commit into the new one.
		StructureId = Subsystem->BeginBuild();
		LastCursorCm = FVector::ZeroVector;
	}
}

void UBuildModeComponent::CancelBuild()
{
	// Destroy removes the binding and its brick actors; an unknown id is a no-op.
	if (UDestructionStructureSubsystem* Subsystem = SubsystemFor(*this))
	{
		Subsystem->Destroy(StructureId);
	}

	// Drop the held preview and the id, so a confirm after a cancel commits nothing.
	HidePreview();
	StructureId = INDEX_NONE;
}

void UBuildModeComponent::SetPieceKind(DestructionSession::EBuildPieceKind Kind)
{
	CurrentKind = Kind;

	ApplyPalette();
}

DestructionSession::EBuildPieceKind UBuildModeComponent::GetPieceKind() const
{
	return CurrentKind;
}

void UBuildModeComponent::SetCourse(int32 Course)
{
	// Clamp on the way in so the getter and the plane agree on the course.
	CurrentCourse = FMath::Max(0, Course);

	ApplyPalette();
}

int32 UBuildModeComponent::GetCourse() const
{
	return CurrentCourse;
}

void UBuildModeComponent::SetRotated(bool bInRotated)
{
	bRotated = bInRotated;

	ApplyPalette();
}

bool UBuildModeComponent::IsRotated() const
{
	return bRotated;
}

void UBuildModeComponent::SetPlacementMode(DestructionSession::EPlacementMode Mode)
{
	PlacementMode = Mode;

	ApplyPalette();
}

void UBuildModeComponent::SetJointChoice(DestructionSession::EJointChoice Choice)
{
	JointChoice = Choice;

	ApplyPalette();
}

void UBuildModeComponent::ApplyPalette()
{
	// By reference to the library row, so a retune isn't masked by a stale copy.
	CurrentMaterial = &DestructionSession::BuildPieceMaterial(CurrentKind);

	const FVector UprightCm = DestructionSession::BuildPieceHalfExtentCm(CurrentKind);

	// Rotation is an X/Y swap, since everything downstream is axis-aligned; Z is unchanged.
	CurrentExtentCm = bRotated
		? FVector(UprightCm.Y, UprightCm.X, UprightCm.Z)
		: UprightCm;

	// The plane rests the piece on the course, so it depends on the piece height too.
	BuildPlaneZCm = DestructionSession::CoursePlaneZCm(CurrentCourse, CurrentExtentCm.Z);

	/*
	 * Update the ghost now, not on the next mouse move (owner playtest, 2026-09-16). Safe
	 * from the constructor: nothing is held yet, so RefreshPreview returns early.
	 */
	RefreshPreview();
}

bool UBuildModeComponent::RefreshPreview()
{
	/*
	 * No held preview, no refresh. Otherwise a settings click before any pointing would hold a
	 * preview at the origin default, and a spent preview would be re-armed.
	 */
	if (!bHasValidPreview)
	{
		return false;
	}

	// Re-project the held cursor onto the current plane so a course change moves the ghost.
	return UpdatePreviewAt(FVector(LastCursorCm.X, LastCursorCm.Y, BuildPlaneZCm)).bValid;
}

FBuildPreview UBuildModeComponent::UpdatePreviewAt(const FVector& WorldCursorCm)
{
	// Remembered so ConfirmPlace commits the pose the ghost shows.
	LastCursorCm = WorldCursorCm;

	UDestructionStructureSubsystem* Subsystem = SubsystemFor(*this);
	if (Subsystem == nullptr)
	{
		return FBuildPreview{};
	}

	// Preview with the same joint override the commit uses.
	const FBuildPreview Preview = Subsystem->PreviewBuildPiece(
		StructureId, WorldCursorCm, CurrentExtentCm, *CurrentMaterial, PlacementMode,
		DestructionSession::JointOverrideFor(JointChoice));

	/*
	 * The ghost sits on the snapped centre and hides when the preview is invalid. Posed with
	 * BrickSpawnTransform, as the real spawn is, because SM_Cube's pivot is a corner and a bare
	 * SetActorLocation would be a half-brick off.
	 */
	ABrickActor* Ghost = EnsureGhost();
	if (Ghost != nullptr)
	{
		if (Preview.bValid)
		{
			if (const UStaticMeshComponent* Mesh = Ghost->GetMesh())
			{
				if (const UStaticMesh* GhostMesh = Mesh->GetStaticMesh())
				{
					const DestructionLayout::FPieceBox Box{ Preview.CentreCm, CurrentExtentCm };
					Ghost->SetActorTransform(
						UDestructionStructureSubsystem::BrickSpawnTransform(*GhostMesh, Box));
				}
			}
		}
		Ghost->SetActorHiddenInGame(!Preview.bValid);
	}

	// ConfirmPlace commits only a held valid preview.
	bHasValidPreview = Preview.bValid;

	return Preview;
}

FBuildPreview UBuildModeComponent::UpdatePreviewFromRay(
	const FVector& RayOriginCm,
	const FVector& RayDirectionCm)
{
	/*
	 * Ray-plane intersection with Z == BuildPlaneZCm: t = (BuildPlaneZCm - Origin.Z) / Dir.Z.
	 * A miss (parallel, or t < 0) hides the ghost, clears the held preview and returns an
	 * invalid preview.
	 *
	 * Non-finite rays are rejected first: NaN slips both later guards, and
	 * DeprojectMousePositionToWorld can return false leaving its out-params uninitialised.
	 */
	if (RayOriginCm.ContainsNaN() || RayDirectionCm.ContainsNaN() ||
		!FMath::IsFinite(RayOriginCm.X) || !FMath::IsFinite(RayOriginCm.Y) || !FMath::IsFinite(RayOriginCm.Z) ||
		!FMath::IsFinite(RayDirectionCm.X) || !FMath::IsFinite(RayDirectionCm.Y) || !FMath::IsFinite(RayDirectionCm.Z))
	{
		HidePreview();
		return FBuildPreview{};
	}

	if (FMath::IsNearlyZero(RayDirectionCm.Z))
	{
		HidePreview();
		return FBuildPreview{};
	}

	const double HitT = (BuildPlaneZCm - RayOriginCm.Z) / RayDirectionCm.Z;
	if (!(HitT >= 0.0))
	{
		HidePreview();
		return FBuildPreview{};
	}

	const FVector Hit = RayOriginCm + HitT * RayDirectionCm;

	/*
	 * A near-grazing ray slips the parallel guard but lands thousands of km out, so a hit
	 * beyond MaxPickDistanceCm is a miss. `!(dist <= max)` also catches a non-finite distance.
	 */
	if (!((Hit - RayOriginCm).Size() <= MaxPickDistanceCm))
	{
		HidePreview();
		return FBuildPreview{};
	}

	return UpdatePreviewAt(FVector(Hit.X, Hit.Y, BuildPlaneZCm)); // Z pinned so float drift can't leave the plane
}

FPieceRef UBuildModeComponent::ConfirmPlace()
{
	/*
	 * No held valid preview, no placement; otherwise the origin default would commit an unseen
	 * brick. A commit spends the preview (one preview, one commit), since a preview only
	 * predicts the commit while the binding is unchanged.
	 */
	if (!bHasValidPreview)
	{
		return FPieceRef{};
	}

	UDestructionStructureSubsystem* Subsystem = SubsystemFor(*this);
	if (Subsystem == nullptr)
	{
		return FPieceRef{};
	}

	const FPieceRef Placed = Subsystem->PlaceBuildPiece(
		StructureId, LastCursorCm, CurrentExtentCm, *CurrentMaterial, PlacementMode,
		DestructionSession::JointOverrideFor(JointChoice));

	bHasValidPreview = false;

	return Placed;
}

AActor* UBuildModeComponent::GetGhostActor() const
{
	return GhostActor;
}

int32 UBuildModeComponent::GetStructureId() const
{
	return StructureId;
}

void UBuildModeComponent::HidePreview()
{
	/*
	 * Hide and clear the held preview together, or a confirm could commit an unseen pose.
	 * Never spawn a ghost just to hide it: BeginBuild cancels first, so an empty plot would
	 * gain a brick actor (GameModeOpensAnEmptyBuildSandbox counts actors).
	 */
	if (GhostActor != nullptr)
	{
		GhostActor->SetActorHiddenInGame(true);
	}

	bHasValidPreview = false;
}

ABrickActor* UBuildModeComponent::EnsureGhost()
{
	if (ABrickActor* Existing = Cast<ABrickActor>(GhostActor))
	{
		return Existing;
	}

	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return nullptr;
	}

	/*
	 * Never adopted into a binding, or it would become a snap neighbour of itself. The
	 * component owns it and destroys it in EndPlay.
	 */
	ABrickActor* Brick = World->SpawnActor<ABrickActor>();
	if (Brick == nullptr)
	{
		return nullptr;
	}

	/*
	 * No collision: a stock ABrickActor blocks ECC_Visibility and would eat the build raycast,
	 * and a solid ghost would push against released bricks.
	 */
	Brick->SetActorEnableCollision(false);
	Brick->SetHighlighted(EBrickHighlight::Hovered);
	Brick->SetActorHiddenInGame(true);

	GhostActor = Brick;
	return Brick;
}

void UBuildModeComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (GhostActor != nullptr)
	{
		GhostActor->Destroy();
		GhostActor = nullptr;
	}

	Super::EndPlay(EndPlayReason);
}
