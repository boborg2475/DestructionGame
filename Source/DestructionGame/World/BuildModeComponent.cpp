// Copyright Epic Games, Inc. All Rights Reserved.

#include "World/BuildModeComponent.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "World/BrickActor.h"

/*
 * THE DRIVE LOOP, AND NOTHING ELSE. Every structural decision is the subsystem's proven snap
 * brain (PreviewBuildPiece / PlaceBuildPiece); this only holds the live build's id, keeps the
 * ghost pointed at the predicted snap, and forwards a confirm. It invents no physics.
 */

namespace
{
	/**
	 * Reach the world's structure subsystem, or null.
	 *
	 * A component with no world (unregistered) reaches nothing, and every door below fails
	 * closed on that rather than dereferencing it — the same fail-closed shape the subsystem's
	 * own doors take against an unknown id.
	 */
	UDestructionStructureSubsystem* SubsystemFor(const UBuildModeComponent& Component)
	{
		const UWorld* World = Component.GetWorld();
		return World != nullptr ? World->GetSubsystem<UDestructionStructureSubsystem>() : nullptr;
	}
}

void UBuildModeComponent::BeginBuild()
{
	if (UDestructionStructureSubsystem* Subsystem = SubsystemFor(*this))
	{
		/*
		 * A second BeginBuild simply adopts a fresh id; the old structure is left standing in
		 * the world, which this slice does not tear down.
		 *
		 * A FRESH BUILD HOLDS NO POSE. Clearing the held preview and the remembered cursor stops a
		 * confirm right after a second BeginBuild from committing the previous build's last cursor
		 * (world origin by default) into the new, empty structure.
		 */
		StructureId = Subsystem->BeginBuild();
		bHasValidPreview = false;
		LastCursorCm = FVector::ZeroVector;
	}
}

FBuildPreview UBuildModeComponent::UpdatePreviewAt(const FVector& WorldCursorCm)
{
	/* Remembered so ConfirmPlace commits the pose the ghost is showing, not a fresh cursor. */
	LastCursorCm = WorldCursorCm;

	UDestructionStructureSubsystem* Subsystem = SubsystemFor(*this);
	if (Subsystem == nullptr)
	{
		return FBuildPreview{};
	}

	const FBuildPreview Preview =
		Subsystem->PreviewBuildPiece(StructureId, WorldCursorCm, CurrentExtentCm, *CurrentMaterial);

	/*
	 * THE GHOST TRACKS THE PREDICTED SNAP, NOT THE CURSOR. It sits at the snapped centre the
	 * commit would land on and shows only while the preview is valid — an invalid preview (an
	 * unknown build, or a pose the solver dropped) hides it rather than leaving a stale brick
	 * floating where the last valid one was.
	 *
	 * IT IS POSED THROUGH THE PRODUCTION PLACEMENT FORMULA, not by a bare SetActorLocation. SM_Cube's
	 * pivot is a CORNER, so dropping the actor at the snapped centre puts its BOUNDS a half-brick out
	 * on every axis. UDestructionStructureSubsystem::BrickSpawnTransform is the one formula the real
	 * spawn uses — it scales the mesh to fill the box and subtracts the scaled local centre — so the
	 * ghost's bounds land exactly where the committed brick's will, whatever the pivot is. Re-posed on
	 * every preview because CurrentExtentCm can change with the palette.
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

	/* ConfirmPlace commits only a HELD valid preview, so remember whether this one was. */
	bHasValidPreview = Preview.bValid;

	return Preview;
}

FPieceRef UBuildModeComponent::ConfirmPlace()
{
	/*
	 * NO HELD VALID PREVIEW, NO PLACEMENT. LastCursorCm defaults to the origin, so a confirm before
	 * any preview would otherwise commit a brick at world (0, 0, 0) — a piece the player never saw.
	 * The guard fails closed on the last preview's validity.
	 *
	 * ONE PREVIEW, ONE COMMIT. A successful placement CONSUMES the held preview (below), so a repeat
	 * confirm with no fresh UpdatePreviewAt lands right here and fails closed exactly as a
	 * confirm-before-any-preview does. That consumption is what makes a confirm only ever commit the
	 * pose the ghost was actually showing: the previewed pose predicts the commit only while the
	 * binding is unchanged (FBuildPreview says so), and the first commit mutates it — so the UI must
	 * re-preview after each placement to hold a fresh, still-accurate pose.
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
		StructureId, LastCursorCm, CurrentExtentCm, *CurrentMaterial, bBuildGrounded);

	/* The preview is spent on this commit; a repeat confirm now fails the guard above. */
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
	 * A STANDALONE ABrickActor, NEVER ADOPTED. It is the same class the build spawns so it reads
	 * as a brick, but it is never handed to a binding — a ghost adopted into the structure would
	 * become a neighbour of itself and skew the very snap it is previewing. The component owns it
	 * and destroys it in EndPlay.
	 */
	ABrickActor* Brick = World->SpawnActor<ABrickActor>();
	if (Brick == nullptr)
	{
		return nullptr;
	}

	/*
	 * THE GHOST NEVER COLLIDES. A stock ABrickActor blocks ECC_Visibility, so a ghost sitting on the
	 * build raycast would eat the very click that drives it, and a solid ghost would depenetrate
	 * against released bricks. Disabling actor collision keeps it a pure visual — its pose is set
	 * from BrickSpawnTransform in UpdatePreviewAt, not from any physical scale here.
	 */
	Brick->SetActorEnableCollision(false);
	Brick->SetHighlighted(EBrickHighlight::Hovered);
	Brick->SetActorHiddenInGame(true);

	GhostActor = Brick;
	return Brick;
}

void UBuildModeComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	/* The ghost is the component's own actor, so it goes when the component does. */
	if (GhostActor != nullptr)
	{
		GhostActor->Destroy();
		GhostActor = nullptr;
	}

	Super::EndPlay(EndPlayReason);
}
