// Copyright Epic Games, Inc. All Rights Reserved.

#include <limits>

#include "Misc/AutomationTest.h"

#include "CollisionQueryParams.h"
#include "EngineUtils.h"
#include "Engine/HitResult.h"
#include "GameFramework/Actor.h"

#include "Core/Profiles/MaterialProfiles.h"
#include "Core/SessionToolbar.h"
#include "Core/StructureBinding.h"
#include "Tests/BrickWorldTestSupport.h"
#include "World/BrickActor.h"
#include "World/BuildModeComponent.h"
#include "World/DestructionStructureSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Build mode UI-4a: the build loop's testable core (BUILD_MODE_PLAN.md, UI-4). UBuildModeComponent
 * holds the live StructureId, drives a ghost actor from the cursor via PreviewBuildPiece, and
 * commits the last-previewed pose via PlaceBuildPiece.
 *
 * World tests, because the subsystem spawns real ABrickActors and the ghost is an actor. Physics
 * never ticks; assertions are on mechanism (piece count, ghost transform/visibility, placed box,
 * actor identity), never displacement. Running-bond numbers match SnapSolverTest.cpp: a request
 * at (11, 0, 7.5) snaps to (11.25, 0, 7.5).
 */
namespace BuildModeComponentTestSupport
{
	// Half-extent of a full brick; the component's default CurrentExtentCm.
	const FVector HalfBrick(10.75, 5.125, 3.25);

	// Full brick size, as GetComponentsBoundingBox().GetSize() reads it.
	const FVector FullBrickSizeCm(21.5, 10.25, 6.5);

	// Running-bond snap one course up from the origin brick, on its +X side.
	const FVector ExpectedRunningBondCentre(11.25, 0.0, 7.5);

	/*
	 * Grounded-course constants, written out rather than imported (DESIGN §8, 2026-09-15). Course 0
	 * centres a 6.5 cm brick at 3.25 so it rests on the ground. Calling CoursePlaneZCm here would make
	 * these tests agree with the function they check.
	 */
	/** Course 0's plane for a brick. */
	constexpr double GroundedBrickPlaneZCm = 3.25;

	/** A seed brick's centre on the grounded course, at the world origin in X and Y. */
	const FVector GroundedSeedCursorCm(0.0, 0.0, GroundedBrickPlaneZCm);

	/**
	 * An owner actor with a registered UBuildModeComponent, or null with the reason reported.
	 * RegisterComponent is required: without it GetWorld() is null and every call fails closed silently.
	 */
	UBuildModeComponent* MakeComponent(FAutomationTestBase& Test, UWorld* World)
	{
		if (World == nullptr)
		{
			Test.AddError(TEXT("fixture: no world to put the build-mode component in"));
			return nullptr;
		}

		AActor* Owner = World->SpawnActor<AActor>();
		if (Owner == nullptr)
		{
			Test.AddError(TEXT("fixture: the component's owner actor failed to spawn"));
			return nullptr;
		}

		UBuildModeComponent* Comp = NewObject<UBuildModeComponent>(Owner);
		if (Comp == nullptr)
		{
			Test.AddError(TEXT("fixture: the build-mode component failed to construct"));
			return nullptr;
		}

		Comp->RegisterComponent();
		return Comp;
	}

	/** How many ABrickActors are in the world that are NOT the component's ghost. */
	int32 CountPlacedBricks(UWorld* World, const AActor* Ghost)
	{
		int32 Count = 0;
		for (TActorIterator<ABrickActor> It(World); It; ++It)
		{
			if (static_cast<const AActor*>(*It) != Ghost)
			{
				++Count;
			}
		}
		return Count;
	}
}

/**
 * BeginBuild opens a live structure; UpdatePreviewAt drives the ghost to the predicted snap and
 * shows it; ConfirmPlace grows the structure by one piece at the previewed pose. The seed is
 * grounded by the pose rule (DESIGN §8): centred at Z = 0 its underside is below ground. The ghost
 * must be visible, bounds-centred on the snap, full-brick sized and not block a Visibility trace;
 * the placed brick must be a distinct actor whose bounds match the ghost's. Never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildModeComponentDrivesGhostAndCommitsTest,
	"DestructionGame.World.BuildMode.ComponentDrivesGhostAndCommits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildModeComponentDrivesGhostAndCommitsTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildModeComponentTestSupport;
	using namespace DestructionProfiles;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	AActor* Owner = TestWorld.World->SpawnActor<AActor>();
	if (Owner == nullptr)
	{
		AddError(TEXT("fixture: the component's owner actor failed to spawn"));
		return true;
	}

	UBuildModeComponent* Comp = NewObject<UBuildModeComponent>(Owner);
	if (Comp == nullptr)
	{
		AddError(TEXT("fixture: the build-mode component failed to construct"));
		return true;
	}
	Comp->RegisterComponent();

	// Step 1: BeginBuild opens an empty, live structure.
	Comp->BeginBuild();

	const int32 StructureId = Comp->GetStructureId();

	TestNotEqual(
		TEXT("BeginBuild should give the component a real structure id, not INDEX_NONE"),
		StructureId, static_cast<int32>(INDEX_NONE));

	FStructureBinding* Binding = Subsystem.Find(StructureId);

	TestNotNull(
		FString::Printf(TEXT("BeginBuild should register a live structure, but Find(%d) is null"),
			StructureId),
		Binding);

	if (Binding == nullptr)
	{
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("a just-begun build structure holds no pieces, got %d"),
			Binding->NumPieces()),
		Binding->NumPieces(), 0);

	// Step 2: a grounded seed brick at the origin.
	Comp->UpdatePreviewAt(FVector(0.0, 0.0, 0.0));
	Comp->ConfirmPlace();

	Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr)
	{
		AddError(TEXT("the structure vanished after the seed placement"));
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("the seed confirm should grow the structure to 1 piece, got %d"),
			Binding->NumPieces()),
		Binding->NumPieces(), 1);

	if (Binding->NumPieces() >= 1)
	{
		TestTrue(
			TEXT("the seed piece was placed grounded, so the structure must record it grounded"),
			Binding->GetStructure().GetPiece(0).bIsGrounded);
	}

	// Step 3: an off-grid cursor, so the ghost must follow the snap, not the cursor.
	const FBuildPreview Preview = Comp->UpdatePreviewAt(FVector(11.0, 0.0, 7.5));

	TestTrue(
		TEXT("a preview against the seeded structure is valid"),
		Preview.bValid);

	TestTrue(
		FString::Printf(TEXT("the +X next-course preview snaps to BrickNextCourse, got kind %d"),
			static_cast<int32>(Preview.Kind)),
		Preview.Kind == BuildMode::ESnapKind::BrickNextCourse);

	TestTrue(
		FString::Printf(
			TEXT("the preview centre is the running-bond snap (11.25, 0, 7.5), got (%g, %g, %g)"),
			Preview.CentreCm.X, Preview.CentreCm.Y, Preview.CentreCm.Z),
		Preview.CentreCm.Equals(ExpectedRunningBondCentre, KINDA_SMALL_NUMBER));

	/*
	 * Assert on bounds, not actor location: SM_Cube's pivot is a corner, so a ghost placed by
	 * SetActorLocation(CentreCm) is a half-brick off while GetActorLocation reads right.
	 * bNonColliding = true so the box is read with the ghost's collision disabled.
	 */
	AActor* Ghost = Comp->GetGhostActor();

	TestNotNull(
		TEXT("a valid preview should have positioned a ghost actor"),
		Ghost);

	FVector GhostBoundsCentre = FVector::ZeroVector;

	if (Ghost != nullptr)
	{
		TestFalse(
			TEXT("the ghost must be VISIBLE while the preview is valid"),
			Ghost->IsHidden());

		const FBox GhostBounds = Ghost->GetComponentsBoundingBox(/*bNonColliding*/ true);
		GhostBoundsCentre = GhostBounds.GetCenter();
		const FVector GhostBoundsSize = GhostBounds.GetSize();

		TestTrue(
			FString::Printf(
				TEXT("the ghost's BOUNDS centre must be the snapped centre (11.25, 0, 7.5), got (%g, %g, %g)"),
				GhostBoundsCentre.X, GhostBoundsCentre.Y, GhostBoundsCentre.Z),
			GhostBoundsCentre.Equals(ExpectedRunningBondCentre, BoundsToleranceCm));

		TestTrue(
			FString::Printf(
				TEXT("the ghost must be a full-brick 21.5 x 10.25 x 6.5, got (%g, %g, %g)"),
				GhostBoundsSize.X, GhostBoundsSize.Y, GhostBoundsSize.Z),
			GhostBoundsSize.Equals(FullBrickSizeCm, BoundsToleranceCm));

		/*
		 * The ghost must not block a Visibility trace through its own centre. A stock ABrickActor
		 * blocks ECC_Visibility, which would eat the cursor's build raycast. The seed brick is clear
		 * of this ray (it spans X up to 10.75, Z up to 3.25).
		 */
		const FVector TraceStart(GhostBoundsCentre.X, GhostBoundsCentre.Y - 100.0, GhostBoundsCentre.Z);
		const FVector TraceEnd(GhostBoundsCentre.X, GhostBoundsCentre.Y + 100.0, GhostBoundsCentre.Z);

		FHitResult GhostHit;
		TestWorld.World->LineTraceSingleByChannel(
			GhostHit, TraceStart, TraceEnd, ECC_Visibility,
			FCollisionQueryParams(SCENE_QUERY_STAT(GhostTrace), true));

		TestTrue(
			FString::Printf(
				TEXT("the ghost must not block a Visibility trace through its own centre; the trace hit %s"),
				*GetNameSafe(GhostHit.GetActor())),
			GhostHit.GetActor() != Ghost);
	}

	// Step 4: confirm grows the structure by one real, distinct piece.
	const FPieceRef Placed = Comp->ConfirmPlace();

	TestTrue(
		FString::Printf(TEXT("the committed piece should be ref {%d, 1}, got {%d, %d}"),
			StructureId, Placed.StructureId, Placed.PieceIndex),
		Placed == FPieceRef{ StructureId, 1 });

	Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr)
	{
		AddError(TEXT("the structure vanished after the confirming placement"));
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("the confirm should grow the structure to 2 pieces, got %d"),
			Binding->NumPieces()),
		Binding->NumPieces(), 2);

	if (Binding->NumPieces() >= 2)
	{
		const FVector PlacedCentre = Binding->GetBinding(1).Box.CentreCm;
		TestTrue(
			FString::Printf(
				TEXT("the committed box lands at the previewed centre (11.25, 0, 7.5), got (%g, %g, %g)"),
				PlacedCentre.X, PlacedCentre.Y, PlacedCentre.Z),
			PlacedCentre.Equals(ExpectedRunningBondCentre, KINDA_SMALL_NUMBER));

		ABrickActor* PlacedActor = Cast<ABrickActor>(Binding->GetActor(1));
		TestNotNull(
			FString::Printf(TEXT("piece 1 should be backed by a spawned ABrickActor, got %s"),
				*GetNameSafe(Binding->GetActor(1))),
			PlacedActor);

		TestTrue(
			TEXT("the placed brick must be a DISTINCT actor from the ghost"),
			PlacedActor != nullptr && static_cast<AActor*>(PlacedActor) != Ghost);

		// The real brick lands where the ghost stood, compared on bounds so a pivot offset can't hide.
		if (PlacedActor != nullptr)
		{
			const FVector PlacedBoundsCentre =
				PlacedActor->GetComponentsBoundingBox(/*bNonColliding*/ true).GetCenter();

			TestTrue(
				FString::Printf(
					TEXT("the placed brick's bounds centre (%g, %g, %g) must coincide with the ghost's (%g, %g, %g)"),
					PlacedBoundsCentre.X, PlacedBoundsCentre.Y, PlacedBoundsCentre.Z,
					GhostBoundsCentre.X, GhostBoundsCentre.Y, GhostBoundsCentre.Z),
				PlacedBoundsCentre.Equals(GhostBoundsCentre, BoundsToleranceCm));
		}
	}

	return true;
}

/**
 * An invalid preview hides the ghost; a valid one shows it. Before BeginBuild the StructureId is
 * INDEX_NONE, so the preview is invalid (the ghost still spawns, hidden). Characterisation test
 * for SetActorHiddenInGame(!bValid); dropping the `!` fails it. Never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildModeComponentHidesInvalidPreviewTest,
	"DestructionGame.World.BuildMode.ComponentHidesInvalidPreview",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildModeComponentHidesInvalidPreviewTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	AActor* Owner = TestWorld.World->SpawnActor<AActor>();
	if (Owner == nullptr)
	{
		AddError(TEXT("fixture: the component's owner actor failed to spawn"));
		return true;
	}

	UBuildModeComponent* Comp = NewObject<UBuildModeComponent>(Owner);
	Comp->RegisterComponent();

	// Before BeginBuild: StructureId is INDEX_NONE, so the preview is invalid.
	const FBuildPreview InvalidPreview = Comp->UpdatePreviewAt(FVector(11.0, 0.0, 7.5));

	TestFalse(
		TEXT("a preview before BeginBuild names no known structure, so it must be invalid"),
		InvalidPreview.bValid);

	AActor* Ghost = Comp->GetGhostActor();

	TestNotNull(
		TEXT("the ghost is spawned on the first preview regardless of validity"),
		Ghost);

	if (Ghost != nullptr)
	{
		TestTrue(
			TEXT("an INVALID preview must HIDE the ghost, not leave a stale brick floating"),
			Ghost->IsHidden());
	}

	// After BeginBuild the id is known (structure empty), so a preview is valid.
	Comp->BeginBuild();
	const FBuildPreview ValidPreview = Comp->UpdatePreviewAt(FVector(11.0, 0.0, 7.5));

	TestTrue(
		TEXT("a preview against a known (if empty) structure is valid"),
		ValidPreview.bValid);

	if (Ghost != nullptr)
	{
		TestFalse(
			TEXT("a VALID preview must SHOW the ghost"),
			Ghost->IsHidden());
	}

	return true;
}

/**
 * A confirm with no held valid preview fails closed: default ref, no new piece. Without the guard,
 * LastCursorCm's origin default would commit an unpreviewed brick at (0, 0, 0). Asserts on the ref
 * and piece count, since the bug is that a brick appears at all. Never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildModeComponentConfirmWithoutPreviewFailsClosedTest,
	"DestructionGame.World.BuildMode.ComponentConfirmWithoutPreviewFailsClosed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildModeComponentConfirmWithoutPreviewFailsClosedTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	AActor* Owner = TestWorld.World->SpawnActor<AActor>();
	if (Owner == nullptr)
	{
		AddError(TEXT("fixture: the component's owner actor failed to spawn"));
		return true;
	}

	UBuildModeComponent* Comp = NewObject<UBuildModeComponent>(Owner);
	Comp->RegisterComponent();

	Comp->BeginBuild();
	const int32 StructureId = Comp->GetStructureId();

	// No UpdatePreviewAt, so no previewed pose is held.
	const FPieceRef Placed = Comp->ConfirmPlace();

	TestEqual(
		FString::Printf(
			TEXT("a confirm with no held preview must return a default ref, got piece index %d"),
			Placed.PieceIndex),
		Placed.PieceIndex, static_cast<int32>(INDEX_NONE));

	FStructureBinding* Binding = Subsystem.Find(StructureId);

	TestNotNull(
		FString::Printf(TEXT("the structure %d should still exist"), StructureId),
		Binding);

	if (Binding != nullptr)
	{
		TestEqual(
			FString::Printf(
				TEXT("a confirm with no held preview must place NOTHING; the structure holds %d pieces"),
				Binding->NumPieces()),
			Binding->NumPieces(), 0);
	}

	return true;
}

/**
 * One preview, one commit: a repeat confirm with no fresh preview places nothing. A successful
 * commit must spend the held preview; otherwise the re-solve against the grown structure places a
 * stray, unpreviewed brick. Seed to 1, placement to 2, repeat returns a default ref and leaves 2.
 * Never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildModeComponentConfirmTwiceWithoutRepreviewPlacesOnceTest,
	"DestructionGame.World.BuildMode.ComponentConfirmTwiceWithoutRepreviewPlacesOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildModeComponentConfirmTwiceWithoutRepreviewPlacesOnceTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildModeComponentTestSupport;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	AActor* Owner = TestWorld.World->SpawnActor<AActor>();
	if (Owner == nullptr)
	{
		AddError(TEXT("fixture: the component's owner actor failed to spawn"));
		return true;
	}

	UBuildModeComponent* Comp = NewObject<UBuildModeComponent>(Owner);
	Comp->RegisterComponent();

	Comp->BeginBuild();
	const int32 StructureId = Comp->GetStructureId();

	// Grounded seed at the origin.
	Comp->UpdatePreviewAt(FVector(0.0, 0.0, 0.0));
	Comp->ConfirmPlace();

	// The one real next-course placement.
	const FBuildPreview Preview = Comp->UpdatePreviewAt(FVector(11.0, 0.0, 7.5));

	TestTrue(
		TEXT("fixture: the next-course preview against the seed must be valid"),
		Preview.bValid);

	const FPieceRef Placed = Comp->ConfirmPlace();

	TestTrue(
		FString::Printf(TEXT("fixture: the real placement should be ref {%d, 1}, got {%d, %d}"),
			StructureId, Placed.StructureId, Placed.PieceIndex),
		Placed == FPieceRef{ StructureId, 1 });

	FStructureBinding* Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr)
	{
		AddError(TEXT("the structure vanished after the real placement"));
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("fixture: the structure should hold 2 pieces before the repeat, got %d"),
			Binding->NumPieces()),
		Binding->NumPieces(), 2);

	// Repeat confirm with no fresh preview: the held preview is spent, so this fails closed.
	const FPieceRef Repeat = Comp->ConfirmPlace();

	TestEqual(
		FString::Printf(
			TEXT("a repeat confirm with no fresh preview must return a default ref, got piece index %d"),
			Repeat.PieceIndex),
		Repeat.PieceIndex, static_cast<int32>(INDEX_NONE));

	Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr)
	{
		AddError(TEXT("the structure vanished after the repeat confirm"));
		return true;
	}

	TestEqual(
		FString::Printf(
			TEXT("one preview, one commit: the repeat confirm must place NOTHING; the structure holds %d pieces"),
			Binding->NumPieces()),
		Binding->NumPieces(), 2);

	return true;
}

/**
 * Destroying the component destroys its ghost. No binding owns the ghost, so EndPlay must. A
 * TWeakObjectPtr reads invalid immediately on destroy, so this doesn't depend on GC timing. Leak
 * guard; removing GhostActor->Destroy() in EndPlay fails it. Never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildModeComponentDestroysGhostOnTeardownTest,
	"DestructionGame.World.BuildMode.ComponentDestroysGhostOnTeardown",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildModeComponentDestroysGhostOnTeardownTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	AActor* Owner = TestWorld.World->SpawnActor<AActor>();
	if (Owner == nullptr)
	{
		AddError(TEXT("fixture: the component's owner actor failed to spawn"));
		return true;
	}

	UBuildModeComponent* Comp = NewObject<UBuildModeComponent>(Owner);
	Comp->RegisterComponent();

	Comp->BeginBuild();

	Comp->UpdatePreviewAt(FVector(11.0, 0.0, 7.5));

	const TWeakObjectPtr<AActor> Ghost(Comp->GetGhostActor());

	TestTrue(
		TEXT("fixture: a preview should have spawned a live ghost to tear down"),
		Ghost.IsValid());

	if (!Ghost.IsValid())
	{
		return true;
	}

	Comp->DestroyComponent();

	TestFalse(
		TEXT("destroying the component must destroy its ghost — no orphan brick may survive it"),
		Ghost.IsValid());

	return true;
}

/**
 * Build mode UI-4b: a world ray drives the preview through the horizontal plane Z == BuildPlaneZCm.
 * Mouse deprojection needs a viewport, so this tests the seam from ray to preview. The hit is at
 * t = (BuildPlaneZCm - Origin.Z) / Direction.Z, valid only for Direction.Z != 0 and t >= 0. A ray
 * from (11.25, 0, 1000) straight down hits (11.25, 0, 7.5), the running-bond snap. Never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildModeComponentRayDrivesPreviewAndGhostTest,
	"DestructionGame.World.BuildMode.ComponentRayDrivesPreviewAndGhost",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildModeComponentRayDrivesPreviewAndGhostTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildModeComponentTestSupport;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	AActor* Owner = TestWorld.World->SpawnActor<AActor>();
	if (Owner == nullptr)
	{
		AddError(TEXT("fixture: the component's owner actor failed to spawn"));
		return true;
	}

	UBuildModeComponent* Comp = NewObject<UBuildModeComponent>(Owner);
	Comp->RegisterComponent();

	Comp->BeginBuild();
	const int32 StructureId = Comp->GetStructureId();

	// Grounded seed at the origin.
	Comp->UpdatePreviewAt(FVector(0.0, 0.0, 0.0));
	Comp->ConfirmPlace();

	FStructureBinding* Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr)
	{
		AddError(TEXT("the structure vanished after the seed placement"));
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("fixture: the seed confirm should grow the structure to 1 piece, got %d"),
			Binding->NumPieces()),
		Binding->NumPieces(), 1);

	// Plane raised one course; a straight-down ray hits exactly the running-bond snap.
	Comp->BuildPlaneZCm = 7.5;

	const FVector RayOrigin(11.25, 0.0, 1000.0);
	const FVector RayDown(0.0, 0.0, -1.0);
	const FBuildPreview Preview = Comp->UpdatePreviewFromRay(RayOrigin, RayDown);

	TestTrue(
		TEXT("a ray meeting the plane in front of the origin gives a valid preview"),
		Preview.bValid);

	TestTrue(
		FString::Printf(TEXT("the ray-picked next-course preview snaps to BrickNextCourse, got kind %d"),
			static_cast<int32>(Preview.Kind)),
		Preview.Kind == BuildMode::ESnapKind::BrickNextCourse);

	TestTrue(
		FString::Printf(
			TEXT("the preview centre is the ray/plane hit and running-bond snap (11.25, 0, 7.5), got (%g, %g, %g)"),
			Preview.CentreCm.X, Preview.CentreCm.Y, Preview.CentreCm.Z),
		Preview.CentreCm.Equals(ExpectedRunningBondCentre, KINDA_SMALL_NUMBER));

	AActor* Ghost = Comp->GetGhostActor();

	TestNotNull(
		TEXT("a valid ray preview should have positioned a ghost actor"),
		Ghost);

	if (Ghost != nullptr)
	{
		TestFalse(
			TEXT("the ghost must be VISIBLE while the ray preview is valid"),
			Ghost->IsHidden());

		const FBox GhostBounds = Ghost->GetComponentsBoundingBox(/*bNonColliding*/ true);
		const FVector GhostBoundsCentre = GhostBounds.GetCenter();
		const FVector GhostBoundsSize = GhostBounds.GetSize();

		TestTrue(
			FString::Printf(
				TEXT("the ghost's BOUNDS centre must be the snapped centre (11.25, 0, 7.5), got (%g, %g, %g)"),
				GhostBoundsCentre.X, GhostBoundsCentre.Y, GhostBoundsCentre.Z),
			GhostBoundsCentre.Equals(ExpectedRunningBondCentre, BoundsToleranceCm));

		TestTrue(
			FString::Printf(
				TEXT("the ghost must be a full-brick 21.5 x 10.25 x 6.5, got (%g, %g, %g)"),
				GhostBoundsSize.X, GhostBoundsSize.Y, GhostBoundsSize.Z),
			GhostBoundsSize.Equals(FullBrickSizeCm, BoundsToleranceCm));
	}

	const FPieceRef Placed = Comp->ConfirmPlace();

	TestTrue(
		FString::Printf(TEXT("the ray-confirmed piece should be ref {%d, 1}, got {%d, %d}"),
			StructureId, Placed.StructureId, Placed.PieceIndex),
		Placed == FPieceRef{ StructureId, 1 });

	Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr)
	{
		AddError(TEXT("the structure vanished after the confirming placement"));
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("the ray confirm should grow the structure to 2 pieces, got %d"),
			Binding->NumPieces()),
		Binding->NumPieces(), 2);

	if (Binding->NumPieces() >= 2)
	{
		const FVector PlacedCentre = Binding->GetBinding(1).Box.CentreCm;
		TestTrue(
			FString::Printf(
				TEXT("the committed box lands at the ray-previewed centre (11.25, 0, 7.5), got (%g, %g, %g)"),
				PlacedCentre.X, PlacedCentre.Y, PlacedCentre.Z),
			PlacedCentre.Equals(ExpectedRunningBondCentre, KINDA_SMALL_NUMBER));
	}

	return true;
}

/**
 * Build mode UI-4b: a ray that misses the plane places nothing. It misses when pointing away
 * (t < 0) or parallel (Direction.Z == 0); both return an invalid preview and hide the ghost. A valid
 * ray is fired first so each hide is a real state change. Never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildModeComponentRayMissingPlaneHidesGhostTest,
	"DestructionGame.World.BuildMode.ComponentRayMissingPlaneHidesGhost",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildModeComponentRayMissingPlaneHidesGhostTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildModeComponentTestSupport;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	AActor* Owner = TestWorld.World->SpawnActor<AActor>();
	if (Owner == nullptr)
	{
		AddError(TEXT("fixture: the component's owner actor failed to spawn"));
		return true;
	}

	UBuildModeComponent* Comp = NewObject<UBuildModeComponent>(Owner);
	Comp->RegisterComponent();

	Comp->BeginBuild();
	const int32 StructureId = Comp->GetStructureId();

	// A grounded seed for the next-course ray to snap against.
	Comp->UpdatePreviewAt(FVector(0.0, 0.0, 0.0));
	Comp->ConfirmPlace();

	Comp->BuildPlaneZCm = 7.5;

	const FVector RayOrigin(11.25, 0.0, 1000.0);

	// A valid down-ray first, so the later hide is a real transition.
	const FBuildPreview Shown = Comp->UpdatePreviewFromRay(RayOrigin, FVector(0.0, 0.0, -1.0));

	TestTrue(
		TEXT("fixture: a valid down-ray must show a valid preview before the miss cases"),
		Shown.bValid);

	AActor* Ghost = Comp->GetGhostActor();

	TestNotNull(
		TEXT("fixture: the valid preview should have spawned a ghost"),
		Ghost);

	if (Ghost != nullptr)
	{
		TestFalse(
			TEXT("fixture: the ghost must be visible after the valid down-ray"),
			Ghost->IsHidden());
	}

	// Ray pointing away: t = (7.5 - 1000) / 1 = -992.5 < 0, so the plane is behind the origin.
	const FBuildPreview Away = Comp->UpdatePreviewFromRay(RayOrigin, FVector(0.0, 0.0, 1.0));

	TestFalse(
		TEXT("a ray pointing away from the plane must return an invalid preview"),
		Away.bValid);

	if (Ghost != nullptr)
	{
		TestTrue(
			TEXT("a ray pointing away from the plane must HIDE the ghost"),
			Ghost->IsHidden());
	}

	/*
	 * A confirm after the miss fails closed: the miss must clear the held preview. This, not the
	 * IsHidden legs, catches a missing clear, since a hidden ghost with a live preview still commits.
	 */
	const FPieceRef AfterAway = Comp->ConfirmPlace();

	TestEqual(
		FString::Printf(
			TEXT("a confirm after the away miss must return a default ref, got piece index %d"),
			AfterAway.PieceIndex),
		AfterAway.PieceIndex, static_cast<int32>(INDEX_NONE));

	if (FStructureBinding* AfterAwayBinding = Subsystem.Find(StructureId))
	{
		TestEqual(
			FString::Printf(
				TEXT("a confirm after the away miss must place NOTHING; the structure holds %d pieces"),
				AfterAwayBinding->NumPieces()),
			AfterAwayBinding->NumPieces(), 1);
	}

	// Re-show the ghost so the parallel case is also a real hide.
	Comp->UpdatePreviewFromRay(RayOrigin, FVector(0.0, 0.0, -1.0));

	if (Ghost != nullptr)
	{
		TestFalse(
			TEXT("fixture: a valid down-ray must re-show the ghost before the parallel case"),
			Ghost->IsHidden());
	}

	// Ray parallel to the plane: Direction.Z == 0, no intersection.
	const FBuildPreview Parallel = Comp->UpdatePreviewFromRay(RayOrigin, FVector(0.0, 1.0, 0.0));

	TestFalse(
		TEXT("a ray parallel to the plane must return an invalid preview"),
		Parallel.bValid);

	if (Ghost != nullptr)
	{
		TestTrue(
			TEXT("a ray parallel to the plane must HIDE the ghost"),
			Ghost->IsHidden());
	}

	// The parallel miss must also spend the re-shown preview.
	const FPieceRef AfterParallel = Comp->ConfirmPlace();

	TestEqual(
		FString::Printf(
			TEXT("a confirm after the parallel miss must return a default ref, got piece index %d"),
			AfterParallel.PieceIndex),
		AfterParallel.PieceIndex, static_cast<int32>(INDEX_NONE));

	FStructureBinding* Binding = Subsystem.Find(StructureId);

	TestNotNull(
		FString::Printf(TEXT("the structure %d should still exist"), StructureId),
		Binding);

	if (Binding != nullptr)
	{
		TestEqual(
			FString::Printf(TEXT("no miss may place a piece; the structure holds %d pieces"),
				Binding->NumPieces()),
			Binding->NumPieces(), 1);
	}

	return true;
}

/**
 * Build mode UI-4b: oblique-ray intersection arithmetic. The straight-down test can't tell solving
 * the intersection from taking the origin's XY; these rays start far from the hit. Ray A from
 * (-30, 0, 107.5) along (41.25, 0, -100) hits (11.25, 0, 7.5) at t = 1. Ray B, half the XY
 * direction, reaches the same point only at t = 2, which pins t's magnitude. The structure is empty
 * so the snap is Free and the centre is exactly the picked point. Never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildModeComponentObliqueRayHitsIntersectionTest,
	"DestructionGame.World.BuildMode.ComponentObliqueRayHitsIntersection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildModeComponentObliqueRayHitsIntersectionTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildModeComponentTestSupport;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	AActor* Owner = TestWorld.World->SpawnActor<AActor>();
	if (Owner == nullptr)
	{
		AddError(TEXT("fixture: the component's owner actor failed to spawn"));
		return true;
	}

	UBuildModeComponent* Comp = NewObject<UBuildModeComponent>(Owner);
	Comp->RegisterComponent();

	// Empty structure: the snap is Free.
	Comp->BeginBuild();
	Comp->BuildPlaneZCm = 7.5;

	const FVector ExpectedHit(11.25, 0.0, 7.5);

	// Ray A: t = 1.
	const FBuildPreview PreviewA =
		Comp->UpdatePreviewFromRay(FVector(-30.0, 0.0, 107.5), FVector(41.25, 0.0, -100.0));

	TestTrue(
		TEXT("an oblique ray meeting the plane in front of the origin gives a valid preview"),
		PreviewA.bValid);

	TestTrue(
		FString::Printf(
			TEXT("oblique ray A (t = 1) must centre on the intersection (11.25, 0, 7.5), got (%g, %g, %g)"),
			PreviewA.CentreCm.X, PreviewA.CentreCm.Y, PreviewA.CentreCm.Z),
		PreviewA.CentreCm.Equals(ExpectedHit, KINDA_SMALL_NUMBER));

	// Ray B: same hit at t = 2.
	const FBuildPreview PreviewB =
		Comp->UpdatePreviewFromRay(FVector(-30.0, 0.0, 207.5), FVector(20.625, 0.0, -100.0));

	TestTrue(
		TEXT("the second oblique ray also gives a valid preview"),
		PreviewB.bValid);

	TestTrue(
		FString::Printf(
			TEXT("oblique ray B (t = 2) must centre on the SAME intersection (11.25, 0, 7.5), got (%g, %g, %g)"),
			PreviewB.CentreCm.X, PreviewB.CentreCm.Y, PreviewB.CentreCm.Z),
		PreviewB.CentreCm.Equals(ExpectedHit, KINDA_SMALL_NUMBER));

	return true;
}

/**
 * Build mode UI-4b: a non-finite ray fails closed. Every comparison against NaN is false, so NaN
 * slips both the IsNearlyZero parallel guard and the t < 0 guard and yields a NaN hit. It must be a
 * miss: invalid preview, hidden ghost, confirm places nothing. Covers NaN in the direction and in
 * the origin. A valid preview is held first so a stale commit would show. Never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildModeComponentNonFiniteRayFailsClosedTest,
	"DestructionGame.World.BuildMode.ComponentNonFiniteRayFailsClosed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildModeComponentNonFiniteRayFailsClosedTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildModeComponentTestSupport;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	AActor* Owner = TestWorld.World->SpawnActor<AActor>();
	if (Owner == nullptr)
	{
		AddError(TEXT("fixture: the component's owner actor failed to spawn"));
		return true;
	}

	UBuildModeComponent* Comp = NewObject<UBuildModeComponent>(Owner);
	Comp->RegisterComponent();

	Comp->BeginBuild();
	const int32 StructureId = Comp->GetStructureId();

	// A grounded seed for the next-course ray to snap against.
	Comp->UpdatePreviewAt(FVector(0.0, 0.0, 0.0));
	Comp->ConfirmPlace();

	Comp->BuildPlaneZCm = 7.5;

	const FVector RayOrigin(11.25, 0.0, 1000.0);
	const double Nan = std::numeric_limits<double>::quiet_NaN();

	// NaN in the direction, after holding a valid preview.
	const FBuildPreview Shown = Comp->UpdatePreviewFromRay(RayOrigin, FVector(0.0, 0.0, -1.0));
	TestTrue(
		TEXT("fixture: a valid down-ray must hold a preview before the NaN cases"),
		Shown.bValid);

	const FBuildPreview NanDir = Comp->UpdatePreviewFromRay(RayOrigin, FVector(0.0, 0.0, Nan));

	TestFalse(
		TEXT("a ray with a NaN direction must return an invalid preview, not a NaN pose"),
		NanDir.bValid);

	AActor* Ghost = Comp->GetGhostActor();
	if (Ghost != nullptr)
	{
		TestTrue(
			TEXT("a ray with a NaN direction must HIDE the ghost"),
			Ghost->IsHidden());
	}

	const FPieceRef AfterNanDir = Comp->ConfirmPlace();
	TestEqual(
		FString::Printf(
			TEXT("a confirm after a NaN-direction ray must return a default ref, got piece index %d"),
			AfterNanDir.PieceIndex),
		AfterNanDir.PieceIndex, static_cast<int32>(INDEX_NONE));

	// NaN in the origin, after re-holding a valid preview.
	Comp->UpdatePreviewFromRay(RayOrigin, FVector(0.0, 0.0, -1.0));

	const FBuildPreview NanOrigin =
		Comp->UpdatePreviewFromRay(FVector(Nan, 0.0, 1000.0), FVector(0.0, 0.0, -1.0));

	TestFalse(
		TEXT("a ray with a NaN origin must return an invalid preview, not a NaN pose"),
		NanOrigin.bValid);

	if (Ghost != nullptr)
	{
		TestTrue(
			TEXT("a ray with a NaN origin must HIDE the ghost"),
			Ghost->IsHidden());
	}

	const FPieceRef AfterNanOrigin = Comp->ConfirmPlace();
	TestEqual(
		FString::Printf(
			TEXT("a confirm after a NaN-origin ray must return a default ref, got piece index %d"),
			AfterNanOrigin.PieceIndex),
		AfterNanOrigin.PieceIndex, static_cast<int32>(INDEX_NONE));

	FStructureBinding* Binding = Subsystem.Find(StructureId);

	TestNotNull(
		FString::Printf(TEXT("the structure %d should still exist"), StructureId),
		Binding);

	if (Binding != nullptr)
	{
		TestEqual(
			FString::Printf(
				TEXT("no non-finite ray may place a piece; the structure holds %d pieces"),
				Binding->NumPieces()),
			Binding->NumPieces(), 1);
	}

	return true;
}

/**
 * Build mode UI-4b: a near-grazing ray whose hit is beyond MaxPickDistanceCm fails closed. With
 * Direction (1, 0, -1e-6) from (0, 0, 1000), |Direction.Z| clears the ~1e-8 parallel guard but
 * t = 1e9, so the hit is ~10,000 km out, far past the 1 km clamp. A nearby valid ray is held first;
 * it is both the control (not rejected) and the stale preview a missing clear would commit.
 * Never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildModeComponentGrazingRayBeyondClampFailsClosedTest,
	"DestructionGame.World.BuildMode.ComponentGrazingRayBeyondClampFailsClosed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildModeComponentGrazingRayBeyondClampFailsClosedTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildModeComponentTestSupport;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	AActor* Owner = TestWorld.World->SpawnActor<AActor>();
	if (Owner == nullptr)
	{
		AddError(TEXT("fixture: the component's owner actor failed to spawn"));
		return true;
	}

	UBuildModeComponent* Comp = NewObject<UBuildModeComponent>(Owner);
	Comp->RegisterComponent();

	// Empty structure: the snap is Free, so only the pick clamp is under test.
	Comp->BeginBuild();
	const int32 StructureId = Comp->GetStructureId();
	Comp->BuildPlaneZCm = 0.0;

	// Pin the 1 km default so the test breaks if it moves.
	TestEqual(
		FString::Printf(TEXT("the default pick clamp should be 100000 cm (1 km), got %g"),
			Comp->MaxPickDistanceCm),
		Comp->MaxPickDistanceCm, 100000.0);

	// Control: a down-ray hitting ~1000 cm away, well within the clamp.
	const FVector NearOrigin(11.25, 0.0, 1000.0);
	const FBuildPreview Near = Comp->UpdatePreviewFromRay(NearOrigin, FVector(0.0, 0.0, -1.0));

	TestTrue(
		TEXT("control: a nearby down-ray within the clamp must give a valid preview"),
		Near.bValid);

	AActor* Ghost = Comp->GetGhostActor();

	TestNotNull(
		TEXT("the valid control preview should have spawned a ghost"),
		Ghost);

	if (Ghost != nullptr)
	{
		TestFalse(
			TEXT("control: the ghost must be visible after the nearby valid ray"),
			Ghost->IsHidden());
	}

	// The grazing ray: t = 1e9, hit ~1e9 cm out.
	const FVector GrazingOrigin(0.0, 0.0, 1000.0);
	const FVector GrazingDir(1.0, 0.0, -1e-6);
	const FBuildPreview Grazing = Comp->UpdatePreviewFromRay(GrazingOrigin, GrazingDir);

	TestFalse(
		TEXT("a near-grazing ray whose hit is beyond the clamp must return an invalid preview"),
		Grazing.bValid);

	if (Ghost != nullptr)
	{
		TestTrue(
			TEXT("a near-grazing ray beyond the clamp must HIDE the ghost"),
			Ghost->IsHidden());
	}

	// The far miss must clear the held preview, or the stale control pose commits here.
	const FPieceRef AfterGrazing = Comp->ConfirmPlace();

	TestEqual(
		FString::Printf(
			TEXT("a confirm after the beyond-clamp ray must return a default ref, got piece index %d"),
			AfterGrazing.PieceIndex),
		AfterGrazing.PieceIndex, static_cast<int32>(INDEX_NONE));

	FStructureBinding* Binding = Subsystem.Find(StructureId);

	TestNotNull(
		FString::Printf(TEXT("the structure %d should still exist"), StructureId),
		Binding);

	if (Binding != nullptr)
	{
		TestEqual(
			FString::Printf(
				TEXT("no beyond-clamp ray may place a piece; the structure holds %d pieces"),
				Binding->NumPieces()),
			Binding->NumPieces(), 0);
	}

	return true;
}

/**
 * Build session slice 2: a committed piece's grounded flag comes from the snapped pose, never the
 * toolbar course (DESIGN §8, 2026-09-15). The solver ranks by Euclidean distance, so on course 0 a
 * cursor at (11.25, 0, 3.25) beside a seed at (0, 0, 3.25) snaps up to the next-course pose
 * (11.25, 0, 10.75), offset 7.5, beating the same-course pose at 11.25. That piece floats 7.5 cm up;
 * flagging it grounded would make it unable to fall. The test asserts the course is still 0 and
 * IsCourseGrounded(0) is true, so a course-derived flag fails it. Never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildModeCommittedGroundedComesFromTheSnappedPoseNotTheCourseTest,
	"DestructionGame.World.BuildMode.CommittedGroundedComesFromTheSnappedPoseNotTheCourse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildModeCommittedGroundedComesFromTheSnappedPoseNotTheCourseTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildModeComponentTestSupport;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	UBuildModeComponent* Comp = MakeComponent(*this, TestWorld.World);
	if (Comp == nullptr)
	{
		return true;
	}

	Comp->BeginBuild();
	const int32 StructureId = Comp->GetStructureId();

	TestEqual(
		FString::Printf(TEXT("fixture: a fresh component builds on course 0, it reads %d"),
			Comp->GetCourse()),
		Comp->GetCourse(), 0);

	// The seed on the grounded course: centre 3.25, underside on the ground.
	const FBuildPreview SeedPreview = Comp->UpdatePreviewAt(GroundedSeedCursorCm);

	TestTrue(
		TEXT("fixture: the seed preview against an empty build must be valid"),
		SeedPreview.bValid);

	TestTrue(
		TEXT("a preview whose bottom face rests on the ground must read GROUNDED"),
		SeedPreview.bGrounded);

	Comp->ConfirmPlace();

	FStructureBinding* Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr || Binding->NumPieces() < 1)
	{
		AddError(TEXT("fixture: the seed placement did not grow the structure"));
		return true;
	}

	TestTrue(
		TEXT("the seed's snapped pose rests on the earth, so piece 0 must be committed GROUNDED"),
		Binding->GetStructure().GetPiece(0).bIsGrounded);

	// The course is still 0, but the solver lifts the cursor a course.
	TestEqual(
		FString::Printf(TEXT("the course must still be 0 for this to be about the disagreement, it is %d"),
			Comp->GetCourse()),
		Comp->GetCourse(), 0);

	TestTrue(
		TEXT("and the toolbar still INTENDS a grounded plane on course 0"),
		DestructionSession::IsCourseGrounded(Comp->GetCourse()));

	const FVector LiftedCursorCm(11.25, 0.0, GroundedBrickPlaneZCm);
	const FVector LiftedSnapCm(11.25, 0.0, 10.75);

	const FBuildPreview Lifted = Comp->UpdatePreviewAt(LiftedCursorCm);

	TestTrue(
		FString::Printf(TEXT("the course-0 cursor is snapped UP a course, so kind BrickNextCourse, got %d"),
			static_cast<int32>(Lifted.Kind)),
		Lifted.Kind == BuildMode::ESnapKind::BrickNextCourse);

	TestTrue(
		FString::Printf(
			TEXT("the lifted snap centres at (11.25, 0, 10.75), got (%g, %g, %g)"),
			Lifted.CentreCm.X, Lifted.CentreCm.Y, Lifted.CentreCm.Z),
		Lifted.CentreCm.Equals(LiftedSnapCm, KINDA_SMALL_NUMBER));

	TestFalse(
		TEXT("its bottom face is 7.5 cm up, so the preview must NOT read it grounded — the course says otherwise"),
		Lifted.bGrounded);

	Comp->ConfirmPlace();

	Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr || Binding->NumPieces() < 2)
	{
		AddError(TEXT("the lifted placement did not grow the structure to 2 pieces"));
		return true;
	}

	const FVector PlacedCentre = Binding->GetBinding(1).Box.CentreCm;

	TestTrue(
		FString::Printf(
			TEXT("the committed box lands on the lifted snap (11.25, 0, 10.75), got (%g, %g, %g)"),
			PlacedCentre.X, PlacedCentre.Y, PlacedCentre.Z),
		PlacedCentre.Equals(LiftedSnapCm, KINDA_SMALL_NUMBER));

	TestFalse(
		TEXT("piece 1 was bedded a course up, so it must NOT be committed grounded — a floating grounded piece can never fall"),
		Binding->GetStructure().GetPiece(1).bIsGrounded);

	return true;
}

/**
 * A same-course snap on the grounded course commits grounded: the other half of the pose rule, so
 * a never-grounds production can't pass by special-casing the seed. The cursor (22, 0, 3.25) snaps
 * to the head pose (22.5, 0, 3.25) at offset 0.5 (next-course is 13.11 away); its underside is on
 * the ground. Never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildModeSameCourseSnapOnTheGroundCommitsGroundedTest,
	"DestructionGame.World.BuildMode.SameCourseSnapOnTheGroundCommitsGrounded",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildModeSameCourseSnapOnTheGroundCommitsGroundedTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildModeComponentTestSupport;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	UBuildModeComponent* Comp = MakeComponent(*this, TestWorld.World);
	if (Comp == nullptr)
	{
		return true;
	}

	Comp->BeginBuild();
	const int32 StructureId = Comp->GetStructureId();

	Comp->UpdatePreviewAt(GroundedSeedCursorCm);
	Comp->ConfirmPlace();

	const FVector HeadCursorCm(22.0, 0.0, GroundedBrickPlaneZCm);
	const FVector HeadSnapCm(22.5, 0.0, GroundedBrickPlaneZCm);

	const FBuildPreview Head = Comp->UpdatePreviewAt(HeadCursorCm);

	TestTrue(
		FString::Printf(TEXT("the end-to-end cursor snaps to BrickSameCourse, got kind %d"),
			static_cast<int32>(Head.Kind)),
		Head.Kind == BuildMode::ESnapKind::BrickSameCourse);

	TestTrue(
		FString::Printf(
			TEXT("the head snap centres at (22.5, 0, 3.25), got (%g, %g, %g)"),
			Head.CentreCm.X, Head.CentreCm.Y, Head.CentreCm.Z),
		Head.CentreCm.Equals(HeadSnapCm, KINDA_SMALL_NUMBER));

	TestTrue(
		TEXT("the head snap stayed on course 0, bottom face on the earth, so the preview reads GROUNDED"),
		Head.bGrounded);

	Comp->ConfirmPlace();

	FStructureBinding* Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr || Binding->NumPieces() < 2)
	{
		AddError(TEXT("the head placement did not grow the structure to 2 pieces"));
		return true;
	}

	TestTrue(
		TEXT("piece 1 is snapped onto the earth, so it must be committed GROUNDED"),
		Binding->GetStructure().GetPiece(1).bIsGrounded);

	return true;
}

/**
 * Piece kind and course drive the material, extent and build plane, so the plane and extent can't
 * disagree. Expected values are written out, not read from CoursePlaneZCm: a piece of half height h
 * on course k centres at k * 7.5 + h. A plate on course 2 planes at 20 and a brick at 18.25; the
 * difference at the same course proves the plane follows the piece. Material is checked by address
 * (&ClayBrick / &Timber). A negative course clamps to 0 on both getter and plane. Never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildModePieceKindDrivesMaterialExtentAndPlaneTest,
	"DestructionGame.World.BuildMode.PieceKindDrivesMaterialExtentAndPlane",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildModePieceKindDrivesMaterialExtentAndPlaneTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildModeComponentTestSupport;
	using namespace DestructionProfiles;
	using namespace DestructionSession;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UBuildModeComponent* Comp = MakeComponent(*this, TestWorld.World);
	if (Comp == nullptr)
	{
		return true;
	}

	const FVector BrickHalfExtentCm(10.75, 5.125, 3.25);
	const FVector PlateHalfExtentCm(33.75, 5.125, 5.0);

	// A fresh component is a brick on the grounded course.
	TestTrue(
		FString::Printf(TEXT("a fresh component's piece kind must be Brick, it is %d"),
			static_cast<int32>(Comp->GetPieceKind())),
		Comp->GetPieceKind() == EBuildPieceKind::Brick);

	TestTrue(
		TEXT("a fresh component's material must be the shipped ClayBrick row, by address"),
		Comp->CurrentMaterial == &ClayBrick);

	TestTrue(
		FString::Printf(
			TEXT("a fresh component's extent must be the brick half (10.75, 5.125, 3.25), got (%g, %g, %g)"),
			Comp->CurrentExtentCm.X, Comp->CurrentExtentCm.Y, Comp->CurrentExtentCm.Z),
		Comp->CurrentExtentCm.Equals(BrickHalfExtentCm, KINDA_SMALL_NUMBER));

	TestEqual(
		FString::Printf(
			TEXT("a fresh component builds on course 0, so its plane is 0 * 7.5 + 3.25 = 3.25, it is %g"),
			Comp->BuildPlaneZCm),
		Comp->BuildPlaneZCm, 3.25);

	// Timber plate: material, extent and plane all change.
	Comp->SetPieceKind(EBuildPieceKind::TimberPlate);

	TestTrue(
		FString::Printf(TEXT("SetPieceKind(TimberPlate) must be readable back, it reads %d"),
			static_cast<int32>(Comp->GetPieceKind())),
		Comp->GetPieceKind() == EBuildPieceKind::TimberPlate);

	TestTrue(
		TEXT("the plate's material must be the shipped Timber row, by address"),
		Comp->CurrentMaterial == &Timber);

	TestTrue(
		FString::Printf(
			TEXT("the plate's extent must be the demo's (33.75, 5.125, 5), got (%g, %g, %g)"),
			Comp->CurrentExtentCm.X, Comp->CurrentExtentCm.Y, Comp->CurrentExtentCm.Z),
		Comp->CurrentExtentCm.Equals(PlateHalfExtentCm, KINDA_SMALL_NUMBER));

	TestEqual(
		FString::Printf(
			TEXT("the plate on course 0 must plane at 0 * 7.5 + 5 = 5, it is %g"),
			Comp->BuildPlaneZCm),
		Comp->BuildPlaneZCm, 5.0);

	// Plate on course 2: 2 * 7.5 + 5 = 20.
	Comp->SetCourse(2);

	TestEqual(
		FString::Printf(TEXT("SetCourse(2) must be readable back, it reads %d"), Comp->GetCourse()),
		Comp->GetCourse(), 2);

	TestEqual(
		FString::Printf(
			TEXT("the plate on course 2 must plane at 2 * 7.5 + 5 = 20, it is %g"),
			Comp->BuildPlaneZCm),
		Comp->BuildPlaneZCm, 20.0);

	// Back to a brick, same course: 2 * 7.5 + 3.25 = 18.25.
	Comp->SetPieceKind(EBuildPieceKind::Brick);

	TestEqual(
		FString::Printf(TEXT("changing the piece must not move the course, it reads %d"),
			Comp->GetCourse()),
		Comp->GetCourse(), 2);

	TestTrue(
		TEXT("back on a brick, the material returns to the ClayBrick row"),
		Comp->CurrentMaterial == &ClayBrick);

	TestTrue(
		FString::Printf(
			TEXT("back on a brick, the extent returns to (10.75, 5.125, 3.25), got (%g, %g, %g)"),
			Comp->CurrentExtentCm.X, Comp->CurrentExtentCm.Y, Comp->CurrentExtentCm.Z),
		Comp->CurrentExtentCm.Equals(BrickHalfExtentCm, KINDA_SMALL_NUMBER));

	TestEqual(
		FString::Printf(
			TEXT("the brick on course 2 must plane at 2 * 7.5 + 3.25 = 18.25 (NOT the plate's 20), it is %g"),
			Comp->BuildPlaneZCm),
		Comp->BuildPlaneZCm, 18.25);

	// A below-ground course clamps to 0.
	Comp->SetCourse(-1);

	TestEqual(
		FString::Printf(TEXT("a negative course must clamp to 0, the component reads %d"),
			Comp->GetCourse()),
		Comp->GetCourse(), 0);

	TestEqual(
		FString::Printf(
			TEXT("and its plane must be course 0's 3.25, never a plane below the earth, it is %g"),
			Comp->BuildPlaneZCm),
		Comp->BuildPlaneZCm, 3.25);

	return true;
}

/**
 * Free placement honours the cursor and forms no joints; the same cursor against the same structure
 * snaps in Snap mode, proving PlacementMode is the cause. Seed at (0, 0, 3.25), cursor (11, 0, 10):
 * Snap takes the next-course pose (11.25, 0, 10.75) at offset 0.79 with one bed joint; Free sits at
 * the cursor with none, not grounded. Both previews run before the Free commit, since the committed
 * Free brick would occupy the next-course cell. The commit is asserted on counts. Never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildModeFreePlacementHonoursTheCursorAndFormsNoJointsTest,
	"DestructionGame.World.BuildMode.FreePlacementHonoursTheCursorAndFormsNoJoints",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildModeFreePlacementHonoursTheCursorAndFormsNoJointsTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildModeComponentTestSupport;
	using namespace DestructionSession;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	UBuildModeComponent* Comp = MakeComponent(*this, TestWorld.World);
	if (Comp == nullptr)
	{
		return true;
	}

	Comp->BeginBuild();
	const int32 StructureId = Comp->GetStructureId();

	TestTrue(
		FString::Printf(TEXT("fixture: a fresh component starts in Snap, it is in mode %d"),
			static_cast<int32>(Comp->PlacementMode)),
		Comp->PlacementMode == EPlacementMode::Snap);

	Comp->UpdatePreviewAt(GroundedSeedCursorCm);
	Comp->ConfirmPlace();

	FStructureBinding* Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr || Binding->NumPieces() != 1)
	{
		AddError(TEXT("fixture: the seed placement did not grow the structure to exactly 1 piece"));
		return true;
	}

	const FVector CursorCm(11.0, 0.0, 10.0);
	const FVector NextCourseCm(11.25, 0.0, 10.75);

	// Snap: pulled 0.79 cm onto the running-bond bed, one joint.
	const FBuildPreview Snapped = Comp->UpdatePreviewAt(CursorCm);

	TestTrue(
		FString::Printf(TEXT("in Snap mode this cursor takes the next-course pose, got kind %d"),
			static_cast<int32>(Snapped.Kind)),
		Snapped.Kind == BuildMode::ESnapKind::BrickNextCourse);

	TestTrue(
		FString::Printf(
			TEXT("the Snap preview centres at (11.25, 0, 10.75), got (%g, %g, %g)"),
			Snapped.CentreCm.X, Snapped.CentreCm.Y, Snapped.CentreCm.Z),
		Snapped.CentreCm.Equals(NextCourseCm, KINDA_SMALL_NUMBER));

	TestEqual(
		FString::Printf(TEXT("the Snap preview would form one bed joint, got %d"), Snapped.JointCount),
		Snapped.JointCount, 1);

	// Free: same cursor, honoured verbatim, no joints.
	Comp->PlacementMode = EPlacementMode::Free;

	const FBuildPreview Free = Comp->UpdatePreviewAt(CursorCm);

	TestTrue(
		FString::Printf(TEXT("in Free mode the candidate must be Free, got kind %d"),
			static_cast<int32>(Free.Kind)),
		Free.Kind == BuildMode::ESnapKind::Free);

	TestTrue(
		FString::Printf(
			TEXT("the Free preview centres on the cursor (11, 0, 10) EXACTLY, got (%g, %g, %g)"),
			Free.CentreCm.X, Free.CentreCm.Y, Free.CentreCm.Z),
		Free.CentreCm.Equals(CursorCm, KINDA_SMALL_NUMBER));

	TestEqual(
		FString::Printf(TEXT("a Free placement bonds to nothing, so 0 joints, got %d"), Free.JointCount),
		Free.JointCount, 0);

	TestFalse(
		TEXT("the Free pose's bottom face is 6.75 cm up, so it must not read grounded"),
		Free.bGrounded);

	const FPieceRef Placed = Comp->ConfirmPlace();

	TestTrue(
		FString::Printf(TEXT("the Free commit should be ref {%d, 1}, got {%d, %d}"),
			StructureId, Placed.StructureId, Placed.PieceIndex),
		Placed == FPieceRef{ StructureId, 1 });

	Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr)
	{
		AddError(TEXT("the structure vanished after the Free placement"));
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("the Free commit grows the structure to 2 pieces, got %d"),
			Binding->NumPieces()),
		Binding->NumPieces(), 2);

	TestEqual(
		FString::Printf(TEXT("a Free commit forms no joint, so the structure holds %d connections"),
			Binding->GetStructure().NumConnections()),
		Binding->GetStructure().NumConnections(), 0);

	if (Binding->NumPieces() >= 2)
	{
		const FVector PlacedCentre = Binding->GetBinding(1).Box.CentreCm;

		TestTrue(
			FString::Printf(
				TEXT("the Free commit lands its box on the cursor (11, 0, 10), got (%g, %g, %g)"),
				PlacedCentre.X, PlacedCentre.Y, PlacedCentre.Z),
			PlacedCentre.Equals(CursorCm, KINDA_SMALL_NUMBER));
	}

	return true;
}

/**
 * A Free placement resting on the ground is grounded: same rule as Snap, bottom face within one
 * joint (1 cm, written out) of Z = 0. The rows straddle the edge: bottom 0 and 1.00 are grounded
 * (inclusive), 1.25 is not. A `<` for `<=` fails the middle row. Never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildModeFreePlacementRestingOnTheGroundIsGroundedTest,
	"DestructionGame.World.BuildMode.FreePlacementRestingOnTheGroundIsGrounded",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildModeFreePlacementRestingOnTheGroundIsGroundedTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildModeComponentTestSupport;
	using namespace DestructionSession;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	UBuildModeComponent* Comp = MakeComponent(*this, TestWorld.World);
	if (Comp == nullptr)
	{
		return true;
	}

	Comp->BeginBuild();
	const int32 StructureId = Comp->GetStructureId();
	Comp->PlacementMode = EPlacementMode::Free;

	struct FFreeGroundedCase
	{
		const TCHAR* Description;
		double CentreZCm;
		double BottomFaceZCm;
		bool bExpectGrounded;
	};

	const FFreeGroundedCase Cases[] = {
		{ TEXT("resting on the ground plane"),          3.25, 0.00, true  },
		{ TEXT("exactly one joint above it"),           4.25, 1.00, true  },
		{ TEXT("a quarter centimetre past one joint"),  4.50, 1.25, false },
	};

	// Preview mutates nothing, so all three rows see the same empty structure.
	for (const FFreeGroundedCase& Case : Cases)
	{
		const FBuildPreview Preview = Comp->UpdatePreviewAt(FVector(50.0, 0.0, Case.CentreZCm));

		TestTrue(
			FString::Printf(TEXT("fixture: the Free preview at Z %g must be valid"), Case.CentreZCm),
			Preview.bValid);

		TestTrue(
			FString::Printf(TEXT("fixture: the Free preview at Z %g must be the Free candidate, got kind %d"),
				Case.CentreZCm, static_cast<int32>(Preview.Kind)),
			Preview.Kind == BuildMode::ESnapKind::Free);

		TestEqual(
			FString::Printf(
				TEXT("%s: centre %g less the 3.25 half-height puts the bottom face at %g, so the preview must read %s"),
				Case.Description, Case.CentreZCm, Case.BottomFaceZCm,
				Case.bExpectGrounded ? TEXT("grounded") : TEXT("NOT grounded")),
			Preview.bGrounded, Case.bExpectGrounded);
	}

	// Commit one piece from each side of the edge.
	Comp->UpdatePreviewAt(FVector(50.0, 0.0, 3.25));
	Comp->ConfirmPlace();

	Comp->UpdatePreviewAt(FVector(50.0, 0.0, 4.5));
	Comp->ConfirmPlace();

	FStructureBinding* Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr || Binding->NumPieces() < 2)
	{
		AddError(TEXT("the two Free placements did not grow the structure to 2 pieces"));
		return true;
	}

	TestTrue(
		TEXT("the Free piece resting on the ground must be committed GROUNDED"),
		Binding->GetStructure().GetPiece(0).bIsGrounded);

	TestFalse(
		TEXT("the Free piece 1.25 cm up — beyond one joint — must NOT be committed grounded"),
		Binding->GetStructure().GetPiece(1).bIsGrounded);

	return true;
}

/**
 * CancelBuild tears the build down: Destroy(id), hide the ghost, clear the held preview, forget the
 * id. Asserts Find(id) is null, GetStructureId() is INDEX_NONE, no build brick actor survives
 * (counted excluding the ghost, which is hidden and reused), and a following confirm fails closed.
 * A binding dropped while its actors stood would leave orphans a piece count can't see. Never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildModeCancelBuildDestroysTheStructureAndHidesTheGhostTest,
	"DestructionGame.World.BuildMode.CancelBuildDestroysTheStructureAndHidesTheGhost",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildModeCancelBuildDestroysTheStructureAndHidesTheGhostTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildModeComponentTestSupport;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	UBuildModeComponent* Comp = MakeComponent(*this, TestWorld.World);
	if (Comp == nullptr)
	{
		return true;
	}

	Comp->BeginBuild();
	const int32 StructureId = Comp->GetStructureId();

	// A grounded seed and its same-course neighbour.
	Comp->UpdatePreviewAt(GroundedSeedCursorCm);
	Comp->ConfirmPlace();
	Comp->UpdatePreviewAt(FVector(22.0, 0.0, GroundedBrickPlaneZCm));
	Comp->ConfirmPlace();

	FStructureBinding* Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr || Binding->NumPieces() != 2)
	{
		AddError(TEXT("fixture: the two placements did not grow the structure to exactly 2 pieces"));
		return true;
	}

	AActor* Ghost = Comp->GetGhostActor();

	const TWeakObjectPtr<AActor> BrickZero(Cast<AActor>(Binding->GetActor(0)));
	const TWeakObjectPtr<AActor> BrickOne(Cast<AActor>(Binding->GetActor(1)));

	const int32 BricksBefore = CountPlacedBricks(TestWorld.World, Ghost);

	TestEqual(
		FString::Printf(TEXT("fixture: the world should hold the build's 2 bricks besides the ghost, it holds %d"),
			BricksBefore),
		BricksBefore, 2);

	Comp->CancelBuild();

	TestNull(
		FString::Printf(TEXT("CancelBuild must DESTROY the structure, but Find(%d) still answers"),
			StructureId),
		Subsystem.Find(StructureId));

	TestEqual(
		FString::Printf(TEXT("a cancelled component names no build, it names %d"),
			Comp->GetStructureId()),
		Comp->GetStructureId(), static_cast<int32>(INDEX_NONE));

	// A null ghost would silently skip the hidden check, so require one.
	TestNotNull(
		TEXT("fixture: two previews must have spawned the ghost, so GetGhostActor cannot be null"),
		Ghost);

	if (Ghost != nullptr)
	{
		TestTrue(
			TEXT("a cancelled build shows no ghost — it must be HIDDEN, not left floating"),
			Ghost->IsHidden());
	}

	TestFalse(
		TEXT("the build's first brick actor must be destroyed with the structure"),
		BrickZero.IsValid());

	TestFalse(
		TEXT("the build's second brick actor must be destroyed with the structure"),
		BrickOne.IsValid());

	const int32 BricksAfter = CountPlacedBricks(TestWorld.World, Ghost);

	TestEqual(
		FString::Printf(
			TEXT("no brick from the cancelled build may be left standing in the world, %d remain"),
			BricksAfter),
		BricksAfter, 0);

	// The held preview went with the build, so a confirm places nothing.
	const FPieceRef AfterCancel = Comp->ConfirmPlace();

	TestEqual(
		FString::Printf(
			TEXT("a confirm after a cancel must return a default ref, got piece index %d"),
			AfterCancel.PieceIndex),
		AfterCancel.PieceIndex, static_cast<int32>(INDEX_NONE));

	TestEqual(
		FString::Printf(
			TEXT("and it must spawn no brick; %d stand in the world"),
			CountPlacedBricks(TestWorld.World, Ghost)),
		CountPlacedBricks(TestWorld.World, Ghost), 0);

	return true;
}

/**
 * A second BeginBuild cancels the first, so the old binding and its bricks don't leak. Asserts the
 * old id resolves to nothing, its brick actor is destroyed, and the new id differs (ids are never
 * reused) and names an empty binding. Never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildModeBeginBuildTwiceCancelsTheFirstTest,
	"DestructionGame.World.BuildMode.BeginBuildTwiceCancelsTheFirst",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildModeBeginBuildTwiceCancelsTheFirstTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildModeComponentTestSupport;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	UBuildModeComponent* Comp = MakeComponent(*this, TestWorld.World);
	if (Comp == nullptr)
	{
		return true;
	}

	Comp->BeginBuild();
	const int32 FirstId = Comp->GetStructureId();

	Comp->UpdatePreviewAt(GroundedSeedCursorCm);
	Comp->ConfirmPlace();

	FStructureBinding* First = Subsystem.Find(FirstId);
	if (First == nullptr || First->NumPieces() != 1)
	{
		AddError(TEXT("fixture: the first build did not grow to exactly 1 piece"));
		return true;
	}

	const TWeakObjectPtr<AActor> FirstBrick(Cast<AActor>(First->GetActor(0)));

	TestTrue(
		TEXT("fixture: the first build's piece must be backed by a live actor to tear down"),
		FirstBrick.IsValid());

	Comp->BeginBuild();
	const int32 SecondId = Comp->GetStructureId();

	TestNotEqual(
		FString::Printf(TEXT("the re-opened build must take a NEW id, it took %d again"), SecondId),
		SecondId, FirstId);

	TestNull(
		FString::Printf(
			TEXT("the abandoned build must be destroyed, but Find(%d) still answers"), FirstId),
		Subsystem.Find(FirstId));

	TestFalse(
		TEXT("the abandoned build's brick must not be left standing in the world"),
		FirstBrick.IsValid());

	FStructureBinding* Second = Subsystem.Find(SecondId);

	TestNotNull(
		FString::Printf(TEXT("the re-opened build must be live, but Find(%d) is null"), SecondId),
		Second);

	if (Second != nullptr)
	{
		TestEqual(
			FString::Printf(TEXT("the re-opened build starts empty, it holds %d pieces"),
				Second->NumPieces()),
			Second->NumPieces(), 0);
	}

	return true;
}

/**
 * While a preview is held, every settings change (rotation, piece kind, placement mode, course,
 * joint) re-runs it at the held cursor, so the ghost updates without a mouse move (owner playtest,
 * 2026-09-16). Asserted on the ghost's bounds: size encodes kind and rotation, centre is where the
 * click lands.
 *
 * The held cursor (11.25, 3, 3.25) is off-grid in Y so the rotated brick's two corner-return poses
 * aren't tied. Upright, it snaps to the next-course pose (11.25, 0, 10.75) at 8.08 cm, 3.5 cm ahead
 * of the next rival.
 *
 * The refresh re-projects the held cursor onto the current build plane, so a course change moves
 * the ghost. Section six pins this in Free mode, where the pose is the cursor: course 1 lifts it
 * from 3.25 to 10.75. Snap poses are set by neighbours, so the Free legs carry it. Never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildModeSettingsChangeRefreshesTheHeldPreviewTest,
	"DestructionGame.World.BuildMode.SettingsChangeRefreshesTheHeldPreview",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildModeSettingsChangeRefreshesTheHeldPreviewTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildModeComponentTestSupport;
	using namespace DestructionSession;

	// Footprints written out, not read from the palette, since the swap is under test.
	const FVector UprightBrickSizeCm(21.5, 10.25, 6.5);
	const FVector RotatedBrickSizeCm(10.25, 21.5, 6.5);
	const FVector UprightPlateSizeCm(67.5, 10.25, 10.0);

	const FVector HeldCursorCm(11.25, 3.0, 3.25);
	const FVector FreeAtCourse0Cm(11.25, 3.0, 3.25);
	const FVector FreeAtCourse1Cm(11.25, 3.0, 10.75);

	const FVector NextCourseCentreCm(11.25, 0.0, 10.75);

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	UBuildModeComponent* Comp = MakeComponent(*this, TestWorld.World);
	if (Comp == nullptr)
	{
		return true;
	}

	Comp->BeginBuild();
	const int32 StructureId = Comp->GetStructureId();

	// Zero: a grounded seed brick at the origin, X-long.

	Comp->UpdatePreviewAt(GroundedSeedCursorCm);
	Comp->ConfirmPlace();

	{
		FStructureBinding* const Binding = Subsystem.Find(StructureId);

		if (Binding == nullptr || Binding->NumPieces() != 1)
		{
			AddError(TEXT("fixture: the seed confirm must leave exactly one piece to snap against"));
			return true;
		}
	}

	// Ghost bounds (bNonColliding, as its collision is off); a missing ghost is an error, not a zero box.
	const auto GhostBounds = [this, Comp]() -> FBox
	{
		AActor* const Ghost = Comp->GetGhostActor();

		if (Ghost == nullptr)
		{
			AddError(TEXT("there is no ghost actor to read — a valid preview must have posed one"));
			return FBox(ForceInit);
		}

		return Ghost->GetComponentsBoundingBox(/*bNonColliding*/ true);
	};

	// One: the held preview, and a refresh that changes nothing.

	{
		const FBuildPreview Held = Comp->UpdatePreviewAt(HeldCursorCm);

		TestTrue(
			TEXT("fixture: the held preview beside the seed must be valid"),
			Held.bValid);

		TestTrue(
			*FString::Printf(
				TEXT("fixture: the held preview must be the running-bond next-course pose "
					 "(11.25, 0, 10.75); it is (%g, %g, %g), kind %d"),
				Held.CentreCm.X, Held.CentreCm.Y, Held.CentreCm.Z, static_cast<int32>(Held.Kind)),
			Held.CentreCm.Equals(NextCourseCentreCm, KINDA_SMALL_NUMBER));

		const FBox Bounds = GhostBounds();

		TestTrue(
			*FString::Printf(
				TEXT("fixture: the ghost must stand at that pose, bounds centre (%g, %g, %g)"),
				Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.GetCenter().Z),
			Bounds.GetCenter().Equals(NextCourseCentreCm, BoundsToleranceCm));

		// The preview is still held, so the refresh returns true.
		TestTrue(
			TEXT("RefreshPreview must report the held preview still valid when nothing has changed"),
			Comp->RefreshPreview());

		const FBox Again = GhostBounds();

		TestTrue(
			*FString::Printf(
				TEXT("and the ghost must not have moved: (%g, %g, %g)"),
				Again.GetCenter().X, Again.GetCenter().Y, Again.GetCenter().Z),
			Again.GetCenter().Equals(NextCourseCentreCm, BoundsToleranceCm));
	}

	/*
	 * Two: SetRotated turns the ghost with no pointer call. Which corner return wins is the snap
	 * solver's business, so the ghost is snapshotted first, then compared with a fresh
	 * (non-mutating) preview at the held cursor.
	 */
	{
		Comp->SetRotated(true);

		const FBox Refreshed = GhostBounds();
		const FVector RefreshedCentre = Refreshed.GetCenter();
		const FVector RefreshedSize = Refreshed.GetSize();

		AActor* const Ghost = Comp->GetGhostActor();

		if (Ghost != nullptr)
		{
			TestFalse(
				TEXT("a rotation must leave the ghost VISIBLE — the player is still pointing at a "
					 "pose, they have only turned the piece"),
				Ghost->IsHidden());
		}

		TestTrue(
			*FString::Printf(
				TEXT("SetRotated MUST RE-DRIVE THE HELD PREVIEW: the ghost's footprint must be the "
					 "brick turned about Z — 10.25 x 21.5 x 6.5 — with no new pointer event. It is "
					 "(%g, %g, %g)"),
				RefreshedSize.X, RefreshedSize.Y, RefreshedSize.Z),
			RefreshedSize.Equals(RotatedBrickSizeCm, BoundsToleranceCm));

		const FBuildPreview Oracle = Comp->UpdatePreviewAt(HeldCursorCm);

		AddInfo(FString::Printf(
			TEXT("the rotated preview at the held cursor is kind %d at (%.4f, %.4f, %.4f), %d joint(s)"),
			static_cast<int32>(Oracle.Kind), Oracle.CentreCm.X, Oracle.CentreCm.Y, Oracle.CentreCm.Z,
			Oracle.JointCount));

		TestTrue(
			TEXT("fixture: a rotated brick beside the seed must still preview something valid"),
			Oracle.bValid);

		TestTrue(
			*FString::Printf(
				TEXT("AND AT THE POSE A FRESH POINTER WOULD GIVE: the refreshed ghost stood at "
					 "(%g, %g, %g) while a pointer event at the same cursor answers (%g, %g, %g)"),
				RefreshedCentre.X, RefreshedCentre.Y, RefreshedCentre.Z,
				Oracle.CentreCm.X, Oracle.CentreCm.Y, Oracle.CentreCm.Z),
			RefreshedCentre.Equals(Oracle.CentreCm, BoundsToleranceCm));
	}

	// Three: un-rotating returns to section one's pose.

	{
		Comp->SetRotated(false);

		const FBox Bounds = GhostBounds();

		TestTrue(
			*FString::Printf(
				TEXT("un-rotating must re-drive the preview too: the ghost is 21.5 x 10.25 x 6.5 "
					 "again, it is (%g, %g, %g)"),
				Bounds.GetSize().X, Bounds.GetSize().Y, Bounds.GetSize().Z),
			Bounds.GetSize().Equals(UprightBrickSizeCm, BoundsToleranceCm));

		TestTrue(
			*FString::Printf(
				TEXT("and back at the running-bond pose (11.25, 0, 10.75) it held before the turn; "
					 "it is at (%g, %g, %g)"),
				Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.GetCenter().Z),
			Bounds.GetCenter().Equals(NextCourseCentreCm, BoundsToleranceCm));
	}

	/*
	 * Four: SetPieceKind redraws the ghost. Size only: the plate's plane and snap pose legitimately
	 * differ from the brick's, and the footprint is what the owner reported stale.
	 */
	{
		Comp->SetPieceKind(EBuildPieceKind::TimberPlate);

		const FBox Bounds = GhostBounds();

		AddInfo(FString::Printf(
			TEXT("after SetPieceKind(TimberPlate) the ghost is (%.4f, %.4f, %.4f) sized "
				 "(%.4f, %.4f, %.4f), plane %.4f"),
			Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.GetCenter().Z,
			Bounds.GetSize().X, Bounds.GetSize().Y, Bounds.GetSize().Z, Comp->BuildPlaneZCm));

		TestTrue(
			*FString::Printf(
				TEXT("SetPieceKind MUST RE-DRIVE THE HELD PREVIEW: the ghost must be the demo's "
					 "67.5 x 10.25 x 10 plate, with no new pointer event. It is (%g, %g, %g)"),
				Bounds.GetSize().X, Bounds.GetSize().Y, Bounds.GetSize().Z),
			Bounds.GetSize().Equals(UprightPlateSizeCm, BoundsToleranceCm));
	}

	// Five: SetPlacementMode(Free) drops the ghost onto the cursor.

	{
		Comp->SetPieceKind(EBuildPieceKind::Brick);
		Comp->SetPlacementMode(EPlacementMode::Free);

		TestTrue(
			*FString::Printf(
				TEXT("the setter and the field are one thing: PlacementMode reads %d"),
				static_cast<int32>(Comp->PlacementMode)),
			Comp->PlacementMode == EPlacementMode::Free);

		const FBox Bounds = GhostBounds();

		TestTrue(
			*FString::Printf(
				TEXT("SetPlacementMode(Free) MUST RE-DRIVE THE HELD PREVIEW: Free honours the cursor "
					 "verbatim, so the ghost must leave the snapped pose and stand at the held "
					 "cursor (11.25, 3, 3.25). It is at (%g, %g, %g)"),
				Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.GetCenter().Z),
			Bounds.GetCenter().Equals(FreeAtCourse0Cm, BoundsToleranceCm));

		TestTrue(
			*FString::Printf(
				TEXT("and it is still a brick, 21.5 x 10.25 x 6.5; it is (%g, %g, %g)"),
				Bounds.GetSize().X, Bounds.GetSize().Y, Bounds.GetSize().Z),
			Bounds.GetSize().Equals(UprightBrickSizeCm, BoundsToleranceCm));
	}

	// Six: SetCourse lifts the ghost by one course.

	{
		Comp->SetCourse(1);

		TestEqual(
			FString::Printf(TEXT("fixture: the component must be on course 1, it reads %d"),
				Comp->GetCourse()),
			Comp->GetCourse(), 1);

		const FBox Bounds = GhostBounds();

		TestTrue(
			*FString::Printf(
				TEXT("SetCourse MUST RE-DRIVE THE HELD PREVIEW ONTO THE NEW PLANE: a brick on course "
					 "1 rests at 7.5 + 3.25 = 10.75, so the Free ghost must rise exactly one course "
					 "to (11.25, 3, 10.75). It is at (%g, %g, %g)"),
				Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.GetCenter().Z),
			Bounds.GetCenter().Equals(FreeAtCourse1Cm, BoundsToleranceCm));
	}

	/*
	 * Seven: SetJointChoice changes only the fastening (FBuildPreview::JointProfile), so the ghost
	 * must stay put and the preview must not be dropped.
	 */
	{
		Comp->SetJointChoice(EJointChoice::Screw);

		TestTrue(
			*FString::Printf(TEXT("the setter and the field are one thing: JointChoice reads %d"),
				static_cast<int32>(Comp->JointChoice)),
			Comp->JointChoice == EJointChoice::Screw);

		const FBox Bounds = GhostBounds();

		TestTrue(
			*FString::Printf(
				TEXT("and the ghost must still stand at the cursor on course 1, (11.25, 3, 10.75); "
					 "it is at (%g, %g, %g)"),
				Bounds.GetCenter().X, Bounds.GetCenter().Y, Bounds.GetCenter().Z),
			Bounds.GetCenter().Equals(FreeAtCourse1Cm, BoundsToleranceCm));
	}

	// No refresh commits anything.
	if (FStructureBinding* const Binding = Subsystem.Find(StructureId))
	{
		TestEqual(
			FString::Printf(
				TEXT("no refresh may PLACE anything; the structure holds %d pieces"),
				Binding->NumPieces()),
			Binding->NumPieces(), 1);
	}

	return true;
}

/**
 * RefreshPreview with no held preview returns false and spawns nothing, both before any pointer
 * event and after ConfirmPlace spent the preview. Otherwise clicking Rotate on a fresh build would
 * hold a preview at the origin default and the next click would commit an unseen brick; after a
 * commit, re-arming would reopen the double commit. Asserted on the bool, a null ghost, the ref
 * and the piece count. Never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildModeRefreshWithoutAHeldPreviewDoesNothingTest,
	"DestructionGame.World.BuildMode.RefreshWithoutAHeldPreviewDoesNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildModeRefreshWithoutAHeldPreviewDoesNothingTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildModeComponentTestSupport;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	UBuildModeComponent* Comp = MakeComponent(*this, TestWorld.World);
	if (Comp == nullptr)
	{
		return true;
	}

	Comp->BeginBuild();
	const int32 StructureId = Comp->GetStructureId();

	// One: a fresh build, pointed at nothing.

	{
		TestFalse(
			TEXT("a refresh with no held preview must report NO valid preview — the player has not "
				 "pointed at anything yet"),
			Comp->RefreshPreview());

		TestNull(
			*FString::Printf(
				TEXT("and it must not spawn a ghost to show at the default cursor (the world "
					 "origin); it left %s standing"),
				*GetNameSafe(Comp->GetGhostActor())),
			Comp->GetGhostActor());

		FStructureBinding* const Fresh = Subsystem.Find(StructureId);

		TestNotNull(
			FString::Printf(TEXT("fixture: the build structure %d must exist"), StructureId),
			Fresh);

		if (Fresh != nullptr)
		{
			TestEqual(
				FString::Printf(TEXT("a refresh alone must place nothing; the structure holds %d "
									 "pieces"),
					Fresh->NumPieces()),
				Fresh->NumPieces(), 0);
		}
	}

	// Two: a preview, then the commit that spends it.

	Comp->UpdatePreviewAt(GroundedSeedCursorCm);

	const FPieceRef Seed = Comp->ConfirmPlace();

	TestTrue(
		FString::Printf(TEXT("fixture: the seed must land as ref {%d, 0}, it landed {%d, %d}"),
			StructureId, Seed.StructureId, Seed.PieceIndex),
		Seed == FPieceRef{ StructureId, 0 });

	// Three: a spent preview can't be refreshed back.

	{
		TestFalse(
			TEXT("ONE PREVIEW, ONE COMMIT: the commit SPENT the held preview, so a refresh must "
				 "report none — a refresh that re-armed it would re-open the double commit"),
			Comp->RefreshPreview());

		const FPieceRef Repeat = Comp->ConfirmPlace();

		TestEqual(
			FString::Printf(
				TEXT("and a confirm after that refresh must return a default ref, got piece index %d"),
				Repeat.PieceIndex),
			Repeat.PieceIndex, static_cast<int32>(INDEX_NONE));

		FStructureBinding* const Binding = Subsystem.Find(StructureId);

		TestNotNull(
			FString::Printf(TEXT("the structure %d should still exist"), StructureId),
			Binding);

		if (Binding != nullptr)
		{
			TestEqual(
				FString::Printf(
					TEXT("no second piece may be committed without a fresh pointer event; the "
						 "structure holds %d pieces"),
					Binding->NumPieces()),
				Binding->NumPieces(), 1);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
