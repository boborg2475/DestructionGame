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

UBuildModeComponent::UBuildModeComponent()
{
	/*
	 * A FRESH COMPONENT IS A BRICK ON THE GROUNDED COURSE, AND THE PALETTE SAYS WHAT THAT MEANS.
	 * Seeding through SetPieceKind rather than by member initialisers is what keeps the material,
	 * the extent and the build plane derived from one place: a plane left at 0 until the first
	 * SetCourse would put a brick's centre on the ground and half of it under the earth, and it
	 * would only be wrong before the player touched the toolbar — the hardest moment to notice.
	 */
	SetPieceKind(CurrentKind);
}

void UBuildModeComponent::BeginBuild()
{
	/*
	 * A BUILD ALREADY OPEN IS CANCELLED, NOT ABANDONED. Adopting a fresh id would leave the old
	 * binding in the subsystem's map with its bricks standing in the world and nothing naming
	 * them; opening a new build is the player saying "not that one". CancelBuild is a no-op when
	 * nothing is open — Destroy fails closed on INDEX_NONE.
	 */
	CancelBuild();

	if (UDestructionStructureSubsystem* Subsystem = SubsystemFor(*this))
	{
		/*
		 * A FRESH BUILD HOLDS NO POSE. The cancel above dropped the held preview; the remembered
		 * cursor goes with it, so a confirm right after a second BeginBuild cannot commit the
		 * previous build's last cursor into the new, empty structure.
		 */
		StructureId = Subsystem->BeginBuild();
		LastCursorCm = FVector::ZeroVector;
	}
}

void UBuildModeComponent::CancelBuild()
{
	/*
	 * THE STRUCTURE GOES FIRST, AND IT TAKES ITS BRICKS WITH IT. Destroy tears down every actor
	 * the binding still names and drops the binding itself, so no orphan brick is left standing
	 * where a build used to be; an id naming nothing is refused there, which is what makes a
	 * cancel with no build open a silent no-op.
	 */
	if (UDestructionStructureSubsystem* Subsystem = SubsystemFor(*this))
	{
		Subsystem->Destroy(StructureId);
	}

	/*
	 * THEN THE COMPONENT FORGETS THE BUILD ENTIRELY. The held preview named a structure that no
	 * longer exists, so a confirm after a cancel must find nothing to commit — and the id is
	 * cleared last so a stray ConfirmPlace reaches the subsystem's own unknown-id refusal too.
	 */
	HidePreview();
	StructureId = INDEX_NONE;
}

void UBuildModeComponent::SetPieceKind(DestructionSession::EBuildPieceKind Kind)
{
	/*
	 * THE PALETTE IS THE SOURCE OF ALL THREE. BuildPieceMaterial hands back the SHIPPED library
	 * row by reference — identity matters, because a copy would go on serving stale numbers after
	 * a retune and every joint inferred off the piece with it.
	 */
	CurrentKind = Kind;
	CurrentMaterial = &DestructionSession::BuildPieceMaterial(Kind);
	CurrentExtentCm = DestructionSession::BuildPieceHalfExtentCm(Kind);

	/*
	 * AND THE PLANE MOVES WITH THE PIECE, not only with the course. CoursePlaneZCm rests the piece
	 * ON the course rather than centring it there, so a 10 cm-thick plate planes 1.75 cm higher
	 * than a brick on the same course — which is the difference between a board bearing on the
	 * wall and a board buried in it.
	 */
	BuildPlaneZCm = DestructionSession::CoursePlaneZCm(CurrentCourse, CurrentExtentCm.Z);
}

DestructionSession::EBuildPieceKind UBuildModeComponent::GetPieceKind() const
{
	return CurrentKind;
}

void UBuildModeComponent::SetCourse(int32 Course)
{
	/*
	 * THE CLAMP IS APPLIED ON THE WAY IN, so the stored course is the one the plane was derived
	 * from. The course vocabulary in DestructionSession treats a negative course as course 0
	 * everywhere; storing the raw value and clamping only inside CoursePlaneZCm would leave the
	 * getter reporting a course below the earth while the plane sat on it.
	 */
	CurrentCourse = FMath::Max(0, Course);

	BuildPlaneZCm = DestructionSession::CoursePlaneZCm(CurrentCourse, CurrentExtentCm.Z);
}

int32 UBuildModeComponent::GetCourse() const
{
	return CurrentCourse;
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

	const FBuildPreview Preview = Subsystem->PreviewBuildPiece(
		StructureId, WorldCursorCm, CurrentExtentCm, *CurrentMaterial, PlacementMode);

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

FBuildPreview UBuildModeComponent::UpdatePreviewFromRay(
	const FVector& RayOriginCm,
	const FVector& RayDirectionCm)
{
	/*
	 * PURE RAY-PLANE GEOMETRY, NO WORLD TRACE. The ray P(t) = RayOriginCm + t * RayDirectionCm meets
	 * the horizontal build plane Z == BuildPlaneZCm where RayOriginCm.Z + t * RayDirectionCm.Z ==
	 * BuildPlaneZCm, i.e. t = (BuildPlaneZCm - RayOriginCm.Z) / RayDirectionCm.Z.
	 *
	 * A RAY THAT CANNOT REACH THE PLANE IN FRONT OF THE ORIGIN FAILS CLOSED: parallel to the plane
	 * (RayDirectionCm.Z == 0, no intersection) or meeting it behind the origin (t < 0). Either hides
	 * the ghost, does NOT drive UpdatePreviewAt — so no valid preview is HELD — and returns a default
	 * invalid preview, so a ConfirmPlace after a missed ray places nothing. The parallel guard uses
	 * FMath::IsNearlyZero so a near-grazing ray with a huge t is treated as a miss, not a wild snap.
	 *
	 * A NON-FINITE RAY FAILS CLOSED FIRST OF ALL. Every comparison against NaN is false, so a NaN in
	 * either operand would slip both guards below — IsNearlyZero(NaN) is false and NaN < 0 is false —
	 * and drive the ghost to a NaN pose that PreviewBuildPiece reports valid. This is reachable once
	 * the mouse wiring lands: DeprojectMousePositionToWorld can return false without setting its
	 * out-params, leaving the ray uninitialised. Rejecting a NaN or infinite origin/direction here
	 * turns that garbage into an ordinary miss rather than a committed, unseen brick.
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

	/*
	 * The hit's X/Y come off the ray; Z is pinned to BuildPlaneZCm exactly rather than reconstructed
	 * as t * RayDirectionCm.Z, so float drift in the division cannot nudge the picked point off the
	 * plane. UpdatePreviewAt already drives the ghost, holds the valid preview and remembers the
	 * cursor for ConfirmPlace — the ray path reuses it whole.
	 */
	const FVector Hit = RayOriginCm + HitT * RayDirectionCm;

	/*
	 * A HIT BEYOND THE PICK CLAMP IS A MISS. A near-grazing ray (a tiny but non-zero
	 * RayDirectionCm.Z) slips the parallel guard yet solves to an enormous t, naming a point
	 * thousands of km out along the ray — a place the player is not pointing at, only one the ray
	 * technically meets the plane at. Rejecting it fails closed exactly as the branches above do.
	 * The NaN-safe !(dist <= max) form also catches a non-finite distance that slipped the guards.
	 */
	if (!((Hit - RayOriginCm).Size() <= MaxPickDistanceCm))
	{
		HidePreview();
		return FBuildPreview{};
	}

	return UpdatePreviewAt(FVector(Hit.X, Hit.Y, BuildPlaneZCm));
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
		StructureId, LastCursorCm, CurrentExtentCm, *CurrentMaterial, PlacementMode);

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

void UBuildModeComponent::HidePreview()
{
	/*
	 * HIDDEN AND UNHELD, TOGETHER. Hiding the ghost without clearing the held preview would leave
	 * a confirm able to commit the pose the player can no longer see, which is the one way a brick
	 * lands somewhere nobody looked.
	 *
	 * HIDE WHAT EXISTS, NEVER SPAWN ONE TO HIDE IT. EnsureGhost here would put an ABrickActor in the
	 * world on the first BeginBuild — which cancels first, and a cancel hides — so opening an empty
	 * plot would leave a brick standing on it before the player had laid anything. Hidden is not
	 * absent: World.Scenario.GameModeOpensAnEmptyBuildSandbox counts the actors in the world, and it
	 * is right to. A ghost with nothing to preview is nothing to hide.
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
