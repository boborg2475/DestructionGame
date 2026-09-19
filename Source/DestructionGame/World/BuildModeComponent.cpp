// Copyright Epic Games, Inc. All Rights Reserved.

#include "World/BuildModeComponent.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "World/BrickActor.h"

/*
 * The drive loop, and nothing else. Every structural decision is the subsystem's proven snap
 * brain (PreviewBuildPiece / PlaceBuildPiece); this only holds the live build's id, keeps the
 * ghost pointed at the predicted snap, and forwards a confirm. It invents no physics.
 */

namespace
{
	/**
	 * Reach the world's structure subsystem, or null.
	 *
	 * A component with no world (unregistered) reaches nothing, and every door below fails
	 * closed on that rather than dereferencing it.
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
	 * A fresh component is a brick on the grounded course. Seeding through SetPieceKind
	 * rather than member initialisers keeps material, extent and build plane derived from
	 * one place — a plane left at 0 would bury half the brick, wrong only before the player
	 * touched the toolbar, the hardest moment to notice.
	 */
	SetPieceKind(CurrentKind);
}

void UBuildModeComponent::BeginBuild()
{
	/*
	 * A build already open is cancelled, not abandoned. Adopting a fresh id would leave the
	 * old binding in the subsystem's map with its bricks standing in the world and nothing
	 * naming them; opening a new build is the player saying "not that one". CancelBuild is a
	 * no-op when nothing is open — Destroy fails closed on INDEX_NONE.
	 */
	CancelBuild();

	if (UDestructionStructureSubsystem* Subsystem = SubsystemFor(*this))
	{
		/*
		 * A fresh build holds no pose. The cancel above dropped the held preview; the
		 * remembered cursor goes with it, so a confirm right after a second BeginBuild cannot
		 * commit the previous build's last cursor into the new, empty structure.
		 */
		StructureId = Subsystem->BeginBuild();
		LastCursorCm = FVector::ZeroVector;
	}
}

void UBuildModeComponent::CancelBuild()
{
	/*
	 * The structure goes first, and it takes its bricks with it: Destroy tears down every
	 * actor the binding still names and drops the binding itself, so no orphan brick is left
	 * standing where a build used to be; an id naming nothing is refused there, which is what
	 * makes a cancel with no build open a silent no-op.
	 */
	if (UDestructionStructureSubsystem* Subsystem = SubsystemFor(*this))
	{
		Subsystem->Destroy(StructureId);
	}

	/*
	 * Then the component forgets the build entirely. The held preview named a structure that
	 * no longer exists, so a confirm after a cancel must find nothing to commit — and the id
	 * is cleared last so a stray ConfirmPlace reaches the subsystem's own unknown-id refusal too.
	 */
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
	/*
	 * The clamp is applied on the way in, so the stored course is the one the plane was
	 * derived from. The course vocabulary in DestructionSession treats a negative course as
	 * course 0 everywhere; storing the raw value and clamping only inside CoursePlaneZCm
	 * would leave the getter reporting a course below the earth while the plane sat on it.
	 */
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
	/*
	 * The palette is the source of all three. BuildPieceMaterial hands back the shipped
	 * library row by reference — identity matters, because a copy would go on serving stale
	 * numbers after a retune and every joint inferred off the piece with it.
	 */
	CurrentMaterial = &DestructionSession::BuildPieceMaterial(CurrentKind);

	const FVector UprightCm = DestructionSession::BuildPieceHalfExtentCm(CurrentKind);

	/*
	 * A rotation is X and Y swapped, the only spelling of it there is: everything downstream
	 * is axis-aligned — an FPieceBox is a centre and a half extent, the snap solver reads
	 * which way a piece runs off those numbers, and the joint inference classifies a quoin
	 * from two boxes and a normal — so the swapped footprint is the quarter turn rather than
	 * a consequence of one held somewhere else.
	 *
	 * Z is not one of the two: a turn about Z cannot change how tall a piece is, keeping the
	 * plane below a plane the piece rests on rather than one it is buried in.
	 */
	CurrentExtentCm = bRotated
		? FVector(UprightCm.Y, UprightCm.X, UprightCm.Z)
		: UprightCm;

	/*
	 * The plane moves with the piece, not only with the course: CoursePlaneZCm rests the
	 * piece on the course rather than centring it there, so a 10 cm-thick plate planes
	 * 1.75 cm higher than a brick on the same course — the difference between a board
	 * bearing on the wall and a board buried in it.
	 */
	BuildPlaneZCm = DestructionSession::CoursePlaneZCm(CurrentCourse, CurrentExtentCm.Z);

	/*
	 * The ghost catches up with the setting immediately, rather than at the player's next
	 * mouse movement (the owner's playtest, 2026-09-16: "it should show where the brick is
	 * going to go without clicking anything"). A chip that lights while the ghost keeps the
	 * old footprint at the old pose reads as a click that was dropped.
	 *
	 * Safe from the constructor, which seeds the palette through SetPieceKind: nothing is
	 * held before the first preview, so the refusal below runs and reaches neither the
	 * world nor the subsystem — which an unregistered component has yet to have.
	 */
	RefreshPreview();
}

bool UBuildModeComponent::RefreshPreview()
{
	/*
	 * No held preview, no refresh — the same fail-closed guard ConfirmPlace takes, at the
	 * door that would otherwise arm it. The settings chips are clickable before the player
	 * has pointed at anything and LastCursorCm is the world origin until they do, so a
	 * refresh that ran regardless would put a ghost down there and hold it, and the next
	 * click would commit a brick nobody saw. A preview a commit has spent is not held either,
	 * which keeps one preview to one commit.
	 */
	if (!bHasValidPreview)
	{
		return false;
	}

	/*
	 * The held cursor, put back on the current build plane. UpdatePreviewAt takes a world
	 * point and the plane enters only through a ray, so replaying LastCursorCm verbatim would
	 * leave the one setting whose entire meaning is a height — the course — unable to move
	 * the ghost at all. X and Y are where the player is pointing; Z is which course they are
	 * laying on.
	 */
	return UpdatePreviewAt(FVector(LastCursorCm.X, LastCursorCm.Y, BuildPlaneZCm)).bValid;
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

	/*
	 * The choice is turned into a profile at the door, and the ghost is previewed with the
	 * same override the commit below will use — a ghost that previewed the inference over a
	 * piece the click will screw down is a ghost predicting a different structure.
	 */
	const FBuildPreview Preview = Subsystem->PreviewBuildPiece(
		StructureId, WorldCursorCm, CurrentExtentCm, *CurrentMaterial, PlacementMode,
		DestructionSession::JointOverrideFor(JointChoice));

	/*
	 * The ghost tracks the predicted snap, not the cursor. It sits at the snapped centre the
	 * commit would land on and shows only while the preview is valid — an invalid preview (an
	 * unknown build, or a pose the solver dropped) hides it rather than leaving a stale brick
	 * floating where the last valid one was.
	 *
	 * Posed through the production placement formula, not a bare SetActorLocation: SM_Cube's
	 * pivot is a corner, so dropping the actor at the snapped centre puts its bounds a
	 * half-brick out on every axis. UDestructionStructureSubsystem::BrickSpawnTransform is
	 * the one formula the real spawn uses — it scales the mesh to fill the box and subtracts
	 * the scaled local centre — so the ghost's bounds land exactly where the committed
	 * brick's will, whatever the pivot is. Re-posed on every preview because CurrentExtentCm
	 * can change with the palette.
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
	 * Pure ray-plane geometry, no world trace. The ray P(t) = RayOriginCm + t * RayDirectionCm
	 * meets the horizontal build plane Z == BuildPlaneZCm where RayOriginCm.Z +
	 * t * RayDirectionCm.Z == BuildPlaneZCm, i.e. t = (BuildPlaneZCm - RayOriginCm.Z) / RayDirectionCm.Z.
	 *
	 * A ray that cannot reach the plane in front of the origin fails closed: parallel to the
	 * plane (RayDirectionCm.Z == 0, no intersection) or meeting it behind the origin (t < 0).
	 * Either hides the ghost, does not drive UpdatePreviewAt — so no valid preview is held —
	 * and returns a default invalid preview, so a ConfirmPlace after a missed ray places
	 * nothing. The parallel guard uses FMath::IsNearlyZero so a near-grazing ray with a huge
	 * t is treated as a miss, not a wild snap.
	 *
	 * A non-finite ray fails closed first of all. Every comparison against NaN is false, so a
	 * NaN in either operand would slip both guards below — IsNearlyZero(NaN) is false and
	 * NaN < 0 is false — and drive the ghost to a NaN pose that PreviewBuildPiece reports
	 * valid. This is reachable once the mouse wiring lands: DeprojectMousePositionToWorld can
	 * return false without setting its out-params, leaving the ray uninitialised. Rejecting a
	 * NaN or infinite origin/direction here turns that garbage into an ordinary miss rather
	 * than a committed, unseen brick.
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
	 * The hit's X/Y come off the ray; Z is pinned to BuildPlaneZCm exactly rather than
	 * reconstructed as t * RayDirectionCm.Z, so float drift in the division cannot nudge the
	 * picked point off the plane. UpdatePreviewAt already drives the ghost, holds the valid
	 * preview and remembers the cursor for ConfirmPlace — the ray path reuses it whole.
	 */
	const FVector Hit = RayOriginCm + HitT * RayDirectionCm;

	/*
	 * A hit beyond the pick clamp is a miss. A near-grazing ray (a tiny but non-zero
	 * RayDirectionCm.Z) slips the parallel guard yet solves to an enormous t, naming a point
	 * thousands of km out along the ray — a place the player is not pointing at, only one the
	 * ray technically meets the plane at. Rejecting it fails closed exactly as the branches
	 * above do. The NaN-safe `!(dist <= max)` form also catches a non-finite distance that
	 * slipped the guards.
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
	 * No held valid preview, no placement. LastCursorCm defaults to the origin, so a confirm
	 * before any preview would otherwise commit a brick at world (0, 0, 0) — a piece the
	 * player never saw. The guard fails closed on the last preview's validity.
	 *
	 * One preview, one commit: a successful placement consumes the held preview (below), so
	 * a repeat confirm with no fresh UpdatePreviewAt lands right here and fails closed
	 * exactly as a confirm-before-any-preview does. That consumption is what makes a confirm
	 * only ever commit the pose the ghost was actually showing: the previewed pose predicts
	 * the commit only while the binding is unchanged (FBuildPreview says so), and the first
	 * commit mutates it — so the UI must re-preview after each placement to hold a fresh,
	 * still-accurate pose.
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
	 * Hidden and unheld, together: hiding the ghost without clearing the held preview would
	 * leave a confirm able to commit the pose the player can no longer see, the one way a
	 * brick lands somewhere nobody looked.
	 *
	 * Hide what exists, never spawn one to hide it: EnsureGhost here would put an ABrickActor
	 * in the world on the first BeginBuild — which cancels first, and a cancel hides — so
	 * opening an empty plot would leave a brick standing on it before the player had laid
	 * anything. Hidden is not absent: World.Scenario.GameModeOpensAnEmptyBuildSandbox counts
	 * the actors in the world, and it is right to. A ghost with nothing to preview is nothing
	 * to hide.
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
	 * A standalone ABrickActor, never adopted. It is the same class the build spawns so it
	 * reads as a brick, but it is never handed to a binding — a ghost adopted into the
	 * structure would become a neighbour of itself and skew the very snap it is previewing.
	 * The component owns it and destroys it in EndPlay.
	 */
	ABrickActor* Brick = World->SpawnActor<ABrickActor>();
	if (Brick == nullptr)
	{
		return nullptr;
	}

	/*
	 * The ghost never collides. A stock ABrickActor blocks ECC_Visibility, so a ghost sitting
	 * on the build raycast would eat the very click that drives it, and a solid ghost would
	 * depenetrate against released bricks. Disabling actor collision keeps it a pure visual —
	 * its pose is set from BrickSpawnTransform in UpdatePreviewAt, not from any physical
	 * scale here.
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
