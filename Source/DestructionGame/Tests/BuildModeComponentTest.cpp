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
 * BUILD-MODE UI-4a — the interactive build loop's TESTABLE CORE (BUILD_MODE_PLAN.md, UI-4).
 *
 * UBuildModeComponent is the logic real mouse/key input will call. It holds the live build's
 * StructureId, drives a translucent GHOST actor from the cursor via the subsystem's non-mutating
 * PreviewBuildPiece, and commits the last-previewed pose on confirm via PlaceBuildPiece. This
 * slice proves the DRIVE LOOP — begin, preview (ghost tracks the predicted snap and shows/hides),
 * confirm (the structure grows by one real piece at the previewed pose) — reusing the proven
 * seams rather than inventing physics.
 *
 * A WORLD TEST, NOT A CORE UNIT TEST, for the same reason its siblings in BuildPlacePieceTest.cpp
 * are: the component reaches the UDestructionStructureSubsystem, which spawns real ABrickActors
 * and binds them, and the ghost is itself a spawned actor. It rides the shared FBrickTestWorld
 * harness under AGameModeBase (brick-empty). It NEVER ticks physics: every assertion is on the
 * MECHANISM — the subsystem's piece count, the ghost actor's transform and visibility, the placed
 * piece's box, actor identity — never on displacement. Two severed pieces can rest exactly in
 * place, and nothing here is even released, so a displacement assertion would measure nothing.
 *
 * THE RUNNING-BOND NUMBERS ARE READ, NOT RE-DERIVED, matching BuildPlacePieceTest.cpp and
 * SnapSolverTest.cpp: a 21.5 x 10.25 x 6.5 brick on 1 cm joints gives the 22.5 x 11.25 x 7.5
 * coordinating grid, so a next-course brick requested at (11, 0, 7.5) snaps to (11.25, 0, 7.5).
 */
namespace BuildModeComponentTestSupport
{
	/* HALF-extent of a FULL 21.5 x 10.25 x 6.5 brick — the component's default CurrentExtentCm. */
	const FVector HalfBrick(10.75, 5.125, 3.25);

	/* FULL size of that brick — what GetComponentsBoundingBox().GetSize() must read. */
	const FVector FullBrickSizeCm(21.5, 10.25, 6.5);

	/* The running-bond half-stagger snap one course up from the origin brick, on its +X side. */
	const FVector ExpectedRunningBondCentre(11.25, 0.0, 7.5);

	/*
	 * THE RESTS-ON-THE-GROUND CONSTANTS, SPELLED OUT RATHER THAN IMPORTED (DESIGN §8, 2026-09-15).
	 *
	 * A brick is 6.5 cm deep on a 1 cm bed joint, so a course is 7.5 cm and the brick's own half
	 * height is 3.25 cm. Course 0 therefore centres a brick at 3.25 with its underside on the earth,
	 * and the next course up centres at 10.75. Every number below is that arithmetic written out —
	 * calling DestructionSession::CoursePlaneZCm here would make these tests agree with the plane
	 * function however wrong it was, which is the one thing they exist to catch.
	 */
	/** Course 0's plane for a brick: the piece rests ON the ground rather than straddling it. */
	constexpr double GroundedBrickPlaneZCm = 3.25;

	/** A seed brick's centre on the grounded course, at the world origin in X and Y. */
	const FVector GroundedSeedCursorCm(0.0, 0.0, GroundedBrickPlaneZCm);

	/**
	 * An owner actor with a registered UBuildModeComponent on it, or null with the reason reported.
	 *
	 * The same six lines opened every test in this file, which CURRENT_STATE has been carrying as an
	 * owed helper. RegisterComponent is what makes GetWorld() resolve, and every door on the
	 * component fails closed without it — so a fixture that forgot it would turn every assertion
	 * below into a silent no-op rather than a failure.
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
 * shows it; ConfirmPlace grows the structure by one real piece at the previewed pose.
 *
 * STEP BY STEP:
 *  1. A component attached to an actor in the world. BeginBuild registers a structure —
 *     GetStructureId names one and Subsystem.Find(id) is a non-null, empty binding.
 *  2. A preview at the origin then ConfirmPlace seeds a brick — Find(id) holds 1 piece, and it is
 *     recorded GROUNDED because its snapped pose puts its bottom face on the earth, not because
 *     any caller said so (the 2026-09-15 DESIGN §8 ruling; a seed centred at Z = 0 has its
 *     underside at -3.25, below the ground plane, so it is grounded by the pose rule).
 *  3. UpdatePreviewAt((11, 0, 7.5)) returns a valid BrickNextCourse
 *     preview centred at (11.25, 0, 7.5), AND the ghost actor is VISIBLE with its world BOUNDS
 *     centred there (not its corner-pivot origin), sized as a full brick, and NOT blocking a
 *     Visibility trace through itself — the "ghost shows where the click will land" proof.
 *  4. ConfirmPlace grows the structure to 2 pieces; the real brick's box centre is the previewed
 *     (11.25, 0, 7.5); its ABrickActor is DISTINCT from the ghost and its BOUNDS coincide with it.
 *
 * NEEDS A TICKING WORLD: a real world for the actor spawns (ghost and bricks), but it never ticks
 * — this is about what the structure and its actors ARE, not about anything moving.
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

	/* An owner actor in the world, and the component registered onto it so GetWorld() resolves. */
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

	/* STEP 1: BeginBuild opens an empty, live structure. */
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

	/* STEP 2: a grounded seed brick, placed through the component's confirm at the origin. */
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

	/*
	 * STEP 3: a next-course preview drives the ghost. The requested cursor is off-grid on
	 * purpose — the snap must move it, and the ghost must follow the snap, not the cursor.
	 */
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
	 * THE GHOST SHOWS WHERE THE CLICK WILL LAND: visible, and with its world BOUNDS — not its
	 * actor origin — centred on the snapped centre. SM_Cube's pivot is a CORNER, so a ghost
	 * placed by SetActorLocation(CentreCm) has its bounds a half-brick off on every axis while
	 * GetActorLocation reads perfectly right (BrickWorldTestSupport.h and BrickActorTest.cpp warn
	 * about exactly this). Production's BrickSpawnTransform subtracts the scaled local centre to
	 * compensate; the ghost must do the same, and the only pivot-agnostic proof is on the bounds.
	 * bNonColliding = true so the box is read even once the ghost's collision is disabled (below).
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
		 * AND THE GHOST IS NOT A CLICK-BLOCKING COLLIDER. A Visibility trace driven straight
		 * through the ghost's own bounds centre — so it cannot miss the ghost's volume — must not
		 * come back holding the ghost. A stock ABrickActor blocks ECC_Visibility (BrickActorTest.cpp
		 * traces on exactly this channel), which would eat the build raycast the real cursor casts
		 * and let the ghost depenetrate against released bricks. Nothing else sits on this line: the
		 * seed brick spans X -10.75..10.75, Z -3.25..3.25, and this ray is at X 11.25, Z 7.5.
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

	/* STEP 4: confirm the previewed pose grows the structure by one real, distinct piece. */
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

		/* The placed brick is a REAL bound ABrickActor, and it is NOT the ghost. */
		ABrickActor* PlacedActor = Cast<ABrickActor>(Binding->GetActor(1));
		TestNotNull(
			FString::Printf(TEXT("piece 1 should be backed by a spawned ABrickActor, got %s"),
				*GetNameSafe(Binding->GetActor(1))),
			PlacedActor);

		TestTrue(
			TEXT("the placed brick must be a DISTINCT actor from the ghost"),
			PlacedActor != nullptr && static_cast<AActor*>(PlacedActor) != Ghost);

		/*
		 * AND THE REAL BRICK LANDS EXACTLY WHERE THE GHOST STOOD. The ghost previews the click;
		 * the click must place the piece the ghost was showing, in the same world position. Both
		 * are compared on BOUNDS so the corner-pivot offset cannot make one agree while the other
		 * is a half-brick out — the whole point of bug 1.
		 */
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
 * A PREVIEW HIDES THE GHOST WHEN IT IS INVALID AND SHOWS IT WHEN IT IS VALID.
 *
 * bValid is false only for an unknown structure id (FBuildPreview's own contract), so a preview
 * BEFORE BeginBuild — when the component's StructureId is still INDEX_NONE — is invalid. The ghost
 * must exist (it is spawned on the first preview regardless) but be hidden. Then BeginBuild names
 * a known, empty structure, a preview against which is valid, and the same ghost must show.
 *
 * THIS GUARDS THE SetActorHiddenInGame(!bValid) BRANCH. It is green on arrival — the production
 * already hides on invalid — so it is a hardening/characterisation row, proved to bite by dropping
 * the `!`: hide-on-valid then fails the "must be VISIBLE" leg here.
 *
 * NEEDS A TICKING WORLD: a world for the ghost's spawn, but it never ticks.
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

	/* BEFORE BeginBuild: StructureId is INDEX_NONE, so the preview is invalid and the ghost hides. */
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

	/* AFTER BeginBuild: the id is known (though the structure is empty), so a preview is valid. */
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
 * A CONFIRM WITH NO HELD VALID PREVIEW FAILS CLOSED — it places nothing.
 *
 * LastCursorCm defaults to the origin, so a ConfirmPlace with no prior UpdatePreviewAt currently
 * commits a brick at world (0, 0, 0) — a piece the player never previewed, at a pose they never
 * saw. ConfirmPlace must instead refuse when no valid preview is held: return a default
 * (INDEX_NONE) FPieceRef and grow the structure by nothing.
 *
 * THE ASSERTION IS ON THE MECHANISM — the returned ref and the piece count — not on any position,
 * because the bug is that a brick appears AT ALL. dev will add a bHasPreview / valid-preview guard.
 *
 * NEEDS A TICKING WORLD: a world for the subsystem and any spawn, but it never ticks.
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

	/* NO UpdatePreviewAt — the component holds no previewed pose. */
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
 * ONE PREVIEW, ONE COMMIT — a repeat confirm with no fresh preview places nothing.
 *
 * ConfirmPlace guards on bHasValidPreview but currently never CLEARS it after a successful commit,
 * so a SECOND ConfirmPlace with no intervening UpdatePreviewAt re-runs PlaceBuildPiece against the
 * now-mutated structure. The stale ghost pose the player last saw is gone: occupancy filters the
 * course cell the first commit just filled, so the re-solve either snaps a stray brick elsewhere or
 * falls to a Free placement that interpenetrates — a piece the player never previewed. ConfirmPlace
 * must instead treat a held valid preview as spent: consume it on a successful commit, so a repeat
 * confirm fails closed exactly as a confirm-before-any-preview does.
 *
 * THE ASSERTION IS ON THE MECHANISM — the returned ref and the piece count. The seed grows the
 * structure to 1, the one real placement to 2; the repeat confirm must return a default
 * (INDEX_NONE) ref and leave the count at 2. No position or displacement assertion: the bug is that
 * a piece is committed AT ALL, so counting is the exact and jitter-immune proof. dev will clear
 * bHasValidPreview after a successful ConfirmPlace (and reset it, with LastCursorCm, on BeginBuild).
 *
 * NEEDS A TICKING WORLD: a world for the subsystem and the actor spawns, but it never ticks.
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

	/* A grounded seed brick at the origin: preview then confirm grows the structure to 1. */
	Comp->UpdatePreviewAt(FVector(0.0, 0.0, 0.0));
	Comp->ConfirmPlace();

	/* The one real next-course placement: a fresh valid preview, then confirm grows it to 2. */
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

	/*
	 * THE REPEAT CONFIRM — no intervening UpdatePreviewAt. The held valid preview was spent on the
	 * placement above, so this must fail closed: a default ref and no growth.
	 */
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
 * TEARING THE COMPONENT DOWN DESTROYS ITS GHOST — no orphan brick left standing in the world.
 *
 * The ghost is the component's own actor and is never adopted into any binding, so nothing else
 * will ever destroy it. EndPlay must. A weak pointer to the ghost is held, the component destroyed,
 * and the weak pointer must go invalid — TWeakObjectPtr reports a destroyed actor as invalid
 * immediately (the idiom Tests/ScenarioLevelTest.cpp uses for "the actor is gone"), so this does
 * not depend on GC timing.
 *
 * A LEAK GUARD, likely green on arrival. Proved to bite by removing the GhostActor->Destroy() in
 * EndPlay: the ghost then survives the component and the weak pointer stays valid.
 *
 * NEEDS A TICKING WORLD: a world for the ghost's spawn, but it never ticks.
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

	/* Any preview spawns the ghost. */
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
 * BUILD-MODE UI-4b — a WORLD RAY drives the preview through a horizontal build plane.
 *
 * The real player controller deprojects the mouse into a world ray; that deprojection needs a
 * viewport and is untestable by construction. This slice is the pure, testable seam BETWEEN a ray
 * and the preview: UpdatePreviewFromRay intersects the ray with the horizontal plane Z ==
 * BuildPlaneZCm and, when it meets the plane IN FRONT of the origin, drives UpdatePreviewAt at the
 * intersection (X, Y, BuildPlaneZCm) — so the whole ray -> plane -> snap -> ghost -> confirm chain
 * is exercised with no simulated input.
 *
 * THE RAY-PLANE MATH, DERIVED HERE not imported. A ray P(t) = Origin + t * Direction meets the
 * plane Z == BuildPlaneZCm where Origin.Z + t * Direction.Z == BuildPlaneZCm, i.e.
 * t = (BuildPlaneZCm - Origin.Z) / Direction.Z, valid only when Direction.Z != 0 (else parallel)
 * and t >= 0 (else the plane is behind the origin). With BuildPlaneZCm = 7.5, a ray from
 * (11.25, 0, 1000) pointing straight down (0, 0, -1) gives t = (7.5 - 1000) / (-1) = 992.5 >= 0,
 * so the hit is Origin + 992.5 * Direction = (11.25, 0, 7.5). That XY on the plane is exactly the
 * running-bond next-course snap over the origin seed, so the preview centres on (11.25, 0, 7.5).
 *
 * NEEDS A TICKING WORLD: a real world for the actor spawns (ghost and bricks), but it never ticks —
 * every assertion is on the MECHANISM (the returned preview, the ghost's bounds and visibility, the
 * subsystem's piece count), never on displacement.
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

	/* A grounded seed brick at the origin: preview then confirm grows the structure to 1. */
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

	/*
	 * ONE COURSE UP, PICKED BY A RAY. The build plane is raised to the next course, and a ray fired
	 * straight DOWN through the running-bond pose meets it at (11.25, 0, 7.5) — the ray hit and the
	 * snapped centre coincide, which is what makes the numbers read cleanly.
	 */
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

	/* The ghost must show where the click will land — visible, bounds centred on the snapped pose. */
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

	/* Confirm lands the real piece at the ray-previewed pose, growing the structure to 2. */
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
 * BUILD-MODE UI-4b — a ray that never reaches the plane in front of the origin PLACES NOTHING.
 *
 * Two ways a ray misses: it points AWAY from the plane (the intersection is behind the origin,
 * t < 0), or it runs PARALLEL to the plane (Direction.Z == 0, no intersection at all). Both must
 * return an invalid preview and hide the ghost, exactly as an unknown-structure preview does, so a
 * cursor off the build plane never leaves a stale brick floating.
 *
 * A valid down-ray is fired first purely to SHOW the ghost, so the subsequent hide is a real state
 * change rather than a ghost that was hidden all along. Assertions are on the returned preview's
 * bValid and the ghost's IsHidden — the mechanism, never displacement.
 *
 * NEEDS A TICKING WORLD: a world for the ghost's spawn, but it never ticks.
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

	/* A grounded seed so a next-course ray has something to snap against. */
	Comp->UpdatePreviewAt(FVector(0.0, 0.0, 0.0));
	Comp->ConfirmPlace();

	Comp->BuildPlaneZCm = 7.5;

	const FVector RayOrigin(11.25, 0.0, 1000.0);

	/* A valid down-ray first, to SHOW the ghost — so the later hide is a genuine transition. */
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

	/*
	 * RAY POINTING AWAY: (0, 0, +1) from Z = 1000 with the plane at Z = 7.5 gives
	 * t = (7.5 - 1000) / (+1) = -992.5 < 0 — the plane is behind the origin, so no placement.
	 */
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
	 * A CONFIRM RIGHT AFTER THE AWAY MISS FAILS CLOSED. The valid down-ray above HELD a preview;
	 * the miss must have SPENT it (bHasValidPreview cleared), so this confirm commits nothing. If
	 * the miss path failed to clear that flag, the STALE previewed pose would commit here — this is
	 * the assertion that bites the missing clear, not the IsHidden legs (a hidden ghost with a live
	 * preview still commits). A default ref AND an unchanged count are the mechanism, not position.
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

	/* Re-show the ghost, so the parallel case is likewise a real hide rather than a no-op. */
	Comp->UpdatePreviewFromRay(RayOrigin, FVector(0.0, 0.0, -1.0));

	if (Ghost != nullptr)
	{
		TestFalse(
			TEXT("fixture: a valid down-ray must re-show the ghost before the parallel case"),
			Ghost->IsHidden());
	}

	/*
	 * RAY PARALLEL TO THE PLANE: Direction.Z == 0, so it never meets Z = 7.5 — no intersection,
	 * no placement, regardless of how far along the ray runs.
	 */
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

	/* Same fail-closed proof for the parallel miss: the re-shown preview must be spent by it. */
	const FPieceRef AfterParallel = Comp->ConfirmPlace();

	TestEqual(
		FString::Printf(
			TEXT("a confirm after the parallel miss must return a default ref, got piece index %d"),
			AfterParallel.PieceIndex),
		AfterParallel.PieceIndex, static_cast<int32>(INDEX_NONE));

	/* Nothing was placed by any miss: the structure still holds only the seed. */
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
 * BUILD-MODE UI-4b — an OBLIQUE ray's intersection ARITHMETIC, pinned independently of the origin.
 *
 * The happy-path ray test above fires straight DOWN from (11.25, 0, 1000): its origin XY already
 * equals the plane hit XY, so the mutant `Hit = RayOriginCm` — dropping the t * Direction term
 * entirely — would still pass it. That test cannot tell "solve the intersection" from "take the
 * origin's XY". This one fires SLANTED rays whose origin XY is NOWHERE NEAR the hit, so only the
 * real intersection formula lands on the running-bond pose.
 *
 * DERIVED HERE, NOT IMPORTED. With the plane at Z = 7.5 and t = (7.5 - Origin.Z) / Direction.Z:
 *   Ray A: Origin (-30, 0, 107.5), Direction (41.25, 0, -100) -> t = (7.5 - 107.5)/(-100) = 1,
 *          Hit = Origin + 1 * Direction = (-30 + 41.25, 0, 107.5 - 100) = (11.25, 0, 7.5).
 *   Ray B: Origin (-30, 0, 207.5), Direction (20.625, 0, -100) -> t = (7.5 - 207.5)/(-100) = 2,
 *          Hit = Origin + 2 * Direction = (-30 + 41.25, 0, 7.5) = (11.25, 0, 7.5).
 * Ray B's SECOND, HALVED direction reaches the SAME point only at t = 2, so it pins the MAGNITUDE
 * of t, not merely its sign: the mutant `Hit = Origin + Direction` (t forced to 1) lands B at
 * (-30 + 20.625, 0, ...) = (-9.375, 0, 7.5) and fails.
 *
 * AN EMPTY STRUCTURE ON PURPOSE. With no neighbours the snap is Free, so the preview centre is the
 * requested pose EXACTLY — no running-bond tolerance to blur whether the ray math hit the point.
 * The assertion is on CentreCm, which for a Free placement IS the picked point.
 *
 * PROVED TO BITE: under the mutant `Hit = RayOriginCm` the centre reads (-30, 0, 7.5) for both
 * rays and both legs fail. NEEDS A TICKING WORLD for the ghost spawn, but never ticks.
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

	/* EMPTY structure: no seed, so the snap is Free and the centre is the picked point exactly. */
	Comp->BeginBuild();
	Comp->BuildPlaneZCm = 7.5;

	const FVector ExpectedHit(11.25, 0.0, 7.5);

	/* Ray A: t = 1. Origin XY (-30) is far from the hit XY (11.25); only the real solve lands it. */
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

	/* Ray B: same hit, but only reached at t = 2 — this pins the magnitude of t, not just its sign. */
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
 * BUILD-MODE UI-4b — a NON-FINITE ray FAILS CLOSED (RED until the guards reject NaN).
 *
 * The parallel guard is FMath::IsNearlyZero and the front-of-origin guard is t < 0. Every
 * comparison against NaN is FALSE, so a NaN slips BOTH: IsNearlyZero(NaN) is false (not parallel),
 * NaN < 0 is false (not behind), and the code proceeds to Hit = Origin + NaN * Direction — a NaN
 * point that PreviewBuildPiece reports valid (the id is known) and that the ghost is
 * SetActorTransform'd to. A garbage ray must instead be treated as a MISS: invalid preview, hidden
 * ghost, and a following confirm that places nothing.
 *
 * TWO NON-FINITE SHAPES, because the fix must guard BOTH operands: a NaN in the DIRECTION
 * (0, 0, NaN) and a NaN in the ORIGIN (NaN, 0, 1000) with an otherwise-finite downward direction.
 *
 * A VALID DOWN-RAY IS HELD FIRST so the confirm-places-nothing leg bites: if the NaN path leaves
 * the previously-held valid preview standing, the confirm would commit that stale pose. Assertions
 * are on the mechanism — bValid, IsHidden, the returned ref and the piece count — never position.
 *
 * RED TODAY. dev fixes with FVector::ContainsNaN / IsFinite guards on origin and direction (and
 * `!(HitT >= 0.0)` in place of `HitT < 0.0`). NEEDS A TICKING WORLD for the spawns, never ticks.
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

	/* A grounded seed so a next-course ray has something valid to snap against. */
	Comp->UpdatePreviewAt(FVector(0.0, 0.0, 0.0));
	Comp->ConfirmPlace();

	Comp->BuildPlaneZCm = 7.5;

	const FVector RayOrigin(11.25, 0.0, 1000.0);
	const double Nan = std::numeric_limits<double>::quiet_NaN();

	/*
	 * NaN IN THE DIRECTION. Hold a valid preview first so the confirm-nothing leg can bite a stale
	 * commit, then fire the garbage ray.
	 */
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

	/*
	 * NaN IN THE ORIGIN, finite downward direction. Re-hold a valid preview first so this leg's
	 * confirm-nothing is likewise a genuine fail-closed rather than an already-empty confirm.
	 */
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

	/* No non-finite ray may place a piece: the structure still holds only the seed. */
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
 * BUILD-MODE UI-4b hardening — a NEAR-GRAZING ray whose hit is BEYOND the pick clamp FAILS CLOSED.
 *
 * The parallel guard is FMath::IsNearlyZero(Direction.Z) at ~1e-8. A ray with a TINY but non-zero
 * Direction.Z passes that guard, yet t = (BuildPlaneZCm - Origin.Z) / Direction.Z is enormous, so
 * the solved hit sits thousands of kilometres out along the ray — a pose PreviewBuildPiece reports
 * VALID (the id is known, the snap is Free) and the ghost is teleported to, and a click would build
 * there. The fix clamps the pick: a hit farther than MaxPickDistanceCm from the origin is a MISS,
 * exactly like the parallel / behind-origin / non-finite branches.
 *
 * THE CONCRETE NUMBERS, derived here not imported (BuildPlaneZCm = 0):
 *   Origin (0, 0, 1000), Direction (1, 0, -1e-6).
 *   |Direction.Z| = 1e-6 > 1e-8, so this is NOT caught by the parallel guard — a genuine new case
 *   (the whole point; a Direction.Z the guard already rejected would prove nothing).
 *   t = (0 - 1000) / (-1e-6) = 1e9.
 *   Hit = Origin + t * Direction = (1e9, 0, 1000 - 1000) = (1e9, 0, 0); Z pinned to the plane.
 *   distance(Hit, Origin) = sqrt((1e9)^2 + 1000^2) ~= 1e9 cm = 10,000 km, FAR beyond the
 *   MaxPickDistanceCm = 100,000 cm (1 km) clamp.
 *
 * A VALID nearby down-ray is HELD FIRST so the confirm-places-nothing leg bites: if the far ray
 * leaves the previously-held valid preview standing, the confirm would commit that stale pose. The
 * held ray hits (11.25, 0, 0) at distance ~1000 cm — well WITHIN the clamp, so it also serves as
 * the control that a legitimate nearby pick is NOT rejected. Assertions are on the mechanism —
 * bValid, IsHidden, the returned ref and the piece count — never position.
 *
 * RED TODAY: the far ray currently previews a valid Free pose at (1e9, 0, 0), shows the ghost, and
 * a confirm commits a brick there. dev clamps on MaxPickDistanceCm. NEEDS A TICKING WORLD for the
 * spawns, but never ticks.
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

	/*
	 * EMPTY structure at the origin build plane: the snap is Free, so every hit previews as valid
	 * and the only thing under test is the pick-distance clamp, not any running-bond geometry.
	 */
	Comp->BeginBuild();
	const int32 StructureId = Comp->GetStructureId();
	Comp->BuildPlaneZCm = 0.0;

	/* The clamp default is a concrete 1 km; pin it so the test breaks if that default moves. */
	TestEqual(
		FString::Printf(TEXT("the default pick clamp should be 100000 cm (1 km), got %g"),
			Comp->MaxPickDistanceCm),
		Comp->MaxPickDistanceCm, 100000.0);

	/*
	 * CONTROL: a nearby down-ray. Origin (11.25, 0, 1000), straight down -> t = 1000, hit
	 * (11.25, 0, 0), distance ~1000 cm — WELL WITHIN the clamp. This both HOLDS a valid preview
	 * (so the far ray's clear can bite below) and proves the clamp does not reject a legitimate
	 * nearby pick.
	 */
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

	/*
	 * THE NEAR-GRAZING FAR RAY. Direction.Z = -1e-6 clears the ~1e-8 parallel guard, t = 1e9, and
	 * the hit lands ~1e9 cm (10,000 km) out — far beyond the 1 km clamp. It must fail closed.
	 */
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

	/*
	 * AND A CONFIRM AFTER THE FAR MISS PLACES NOTHING. The control ray HELD a valid preview; the
	 * far ray must have SPENT it (bHasValidPreview cleared), so this confirm commits nothing. If
	 * the clamp path failed to clear that flag, the stale (11.25, 0, 0) pose would commit here —
	 * this is the leg that bites the missing clear, not the IsHidden legs. A default ref AND an
	 * unchanged (zero) count are the mechanism, never position.
	 */
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
 * BUILD-MODE SESSION SLICE 2 — THE COMMITTED PIECE'S GROUNDED FLAG COMES FROM THE SNAPPED POSE,
 * NEVER FROM THE TOOLBAR'S COURSE. The review-named hazard, and the first red of this slice.
 *
 * DESIGN §8's 2026-09-15 ruling says it in as many words: the course is the toolbar's INTENT for
 * the build plane, and the flag the piece carries is derived from where it actually landed. The two
 * genuinely disagree, and this fixture is the case where they do. The snap solver ranks candidates
 * by RAW EUCLIDEAN DISTANCE, so with the course left on 0 — plane 3.25, the grounded course — a
 * cursor placed beside a standing brick is pulled UP onto that brick's next-course bed, 7.5 cm into
 * the air. A piece put there and flagged grounded would terminate load at the earth: FStructure
 * routes to bIsGrounded, so a floating brick carrying it is a brick that CAN NEVER FALL, and every
 * piece stacked on it inherits the lie.
 *
 * THE ARITHMETIC, DERIVED HERE. Brick 21.5 x 10.25 x 6.5 on 1 cm joints: half extent
 * (10.75, 5.125, 3.25), coordinating grid 22.5 x 11.25 x 7.5. Against a seed centred at
 * (0, 0, 3.25) the solver offers two poses, and the CURSOR (11.25, 0, 3.25) is deliberately placed
 * where the further-looking one wins:
 *   next-course (0 + 11.25, 0, 3.25 + 7.5) = (11.25, 0, 10.75) — offset sqrt(0 + 7.5^2)  =  7.50
 *   same-course (0 + 22.50, 0, 3.25      ) = (22.50, 0,  3.25) — offset sqrt(11.25^2)    = 11.25
 * so the next-course pose outranks the same-course one and the cursor is lifted a course. Its
 * bottom face is 10.75 - 3.25 = 7.5 cm, well beyond one joint of the ground.
 *
 * THE CONTRAST IS ASSERTED, NOT IMPLIED. The course is checked to be STILL 0 and
 * DestructionSession::IsCourseGrounded(0) to be STILL true at the moment the lifted piece is
 * committed not-grounded. That is what makes this test about the two answers DIFFERING: a
 * production that derived the flag from the course would pass every other leg here and fail
 * exactly this one.
 *
 * THE ASSERTION IS THE FLAG AND THE POSE, NOT DISPLACEMENT. Nothing is released and nothing moves;
 * bIsGrounded and the box centre are binary and exact.
 *
 * NEEDS A TICKING WORLD: a real world for the ghost and brick spawns, but it never ticks.
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

	/* THE SEED, on the grounded course: centre 3.25, underside exactly on the ground plane. */
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

	/*
	 * THE HAZARD. The course has NOT moved — the build plane is still the grounded 3.25 — but the
	 * cursor beside the seed is nearer the next-course bed than the same-course head, so the solver
	 * lifts it a whole course into the air.
	 */
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
 * THE SAME-COURSE SNAP ON THE GROUNDED COURSE COMMITS GROUNDED — the other half of the pose rule.
 *
 * ITS SIBLING ABOVE WOULD PASS ON ITS OWN FOR A PRODUCTION THAT NEVER GROUNDS ANYTHING. "Derive
 * grounded from the pose" is two claims, and a `bIsGrounded = false` everywhere satisfies the
 * lifted leg completely; the seed leg is the only counterweight there, and a seed is the one piece
 * a caller could plausibly special-case. So this lays a SECOND piece — snapped, not seeded, with a
 * real neighbour in the structure — that must still come out grounded because its pose is on the
 * earth.
 *
 * THE ARITHMETIC. Same seed at (0, 0, 3.25). The cursor (22, 0, 3.25) sits half a centimetre short
 * of the same-course head pose:
 *   same-course (22.50, 0,  3.25) — offset 0.50
 *   next-course (11.25, 0, 10.75) — offset sqrt(10.75^2 + 7.5^2) = 13.11
 * so the head snap wins by a wide margin, the piece stays on course 0, its bottom face is
 * 3.25 - 3.25 = 0, and it must read grounded.
 *
 * NEEDS A TICKING WORLD: a real world for the spawns, but it never ticks.
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
 * THE PIECE KIND AND THE COURSE DRIVE THE MATERIAL, THE EXTENT AND THE BUILD PLANE — one setter
 * each, and no fourth place brick dimensions are written down.
 *
 * THE COMPONENT USED TO CARRY THE PALETTE AS TWO RAW PUBLIC FIELDS a caller set by hand, which is
 * how the plane and the extent get to disagree: a caller that switched CurrentExtentCm to a timber
 * plate and forgot BuildPlaneZCm would preview a 10 cm-thick board centred where a 6.5 cm brick
 * goes, half of it buried in the course below. Kind and course are the two things the player
 * actually chooses, so they are the two setters, and everything else is DERIVED on the way through.
 *
 * THE EXPECTED NUMBERS ARE WRITTEN OUT, NOT READ BACK FROM THE MODEL. Calling
 * DestructionSession::CoursePlaneZCm or BuildPieceHalfExtentCm to build the expectation would make
 * this test agree with the palette however wrong the palette was. So: a course is 6.5 + 1 = 7.5 cm,
 * a brick's half height is 3.25 and the demo's wall plate's is 5.0, and a piece of half height h on
 * course k is centred at k * 7.5 + h. Hence
 *     brick, course 0  ->  0 * 7.5 + 3.25  =  3.25
 *     plate, course 0  ->  0 * 7.5 + 5.00  =  5.00
 *     plate, course 2  ->  2 * 7.5 + 5.00  = 20.00
 *     brick, course 2  ->  2 * 7.5 + 3.25  = 18.25
 * The plate-on-course-2 row is the one the demo building already builds (its plate sits on two
 * brick courses), and 20.00 vs 18.25 at the SAME course is what proves the plane moved with the
 * PIECE as well as with the course — a plane derived from the course alone would give one answer
 * for both.
 *
 * MATERIAL IDENTITY, BY ADDRESS. Two profiles with equal fields are equal in every way except the
 * one that matters — which library row a future retune moves — so the assertion is that
 * CurrentMaterial IS &ClayBrick / &Timber, exactly as the palette's own test pins it.
 *
 * AND A NEGATIVE COURSE IS COURSE 0. The whole course vocabulary clamps rather than admitting a
 * build plane below the earth; the component stores the clamped course so its getter and its plane
 * cannot report different courses.
 *
 * NEEDS A TICKING WORLD: only to construct and register the component (GetWorld must resolve).
 * Nothing here previews, spawns or ticks.
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

	/* A FRESH COMPONENT IS A BRICK ON THE GROUNDED COURSE — the default a player opens with. */
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

	/* THE TIMBER PLATE: a different material, a different extent, and a plane that follows both. */
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

	/* TWO COURSES UP, plate still selected: 2 * 7.5 + 5 = 20 — where the demo puts its plate. */
	Comp->SetCourse(2);

	TestEqual(
		FString::Printf(TEXT("SetCourse(2) must be readable back, it reads %d"), Comp->GetCourse()),
		Comp->GetCourse(), 2);

	TestEqual(
		FString::Printf(
			TEXT("the plate on course 2 must plane at 2 * 7.5 + 5 = 20, it is %g"),
			Comp->BuildPlaneZCm),
		Comp->BuildPlaneZCm, 20.0);

	/* BACK TO A BRICK WITHOUT TOUCHING THE COURSE: 2 * 7.5 + 3.25 = 18.25, not 20. */
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

	/* FAIL CLOSED: a below-ground course is course 0, on the getter and on the plane alike. */
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
 * FREE PLACEMENT HONOURS THE CURSOR AND FORMS NO JOINTS — and the SAME cursor, against the SAME
 * structure, snaps when the mode says Snap.
 *
 * PlacementMode is the toolbar's Snap/Free pair arriving at the component. The proof that it is the
 * CAUSE is running one cursor through both modes against an unchanged structure: a production that
 * ignored the field would return the same answer twice and one of the two legs would fail whichever
 * way it ignored it. Preview is non-mutating, so both legs genuinely see the one-brick structure.
 *
 * THE ORDER IS SNAP FIRST, DELIBERATELY. Committing the Free piece first would change the answer to
 * the Snap leg — the free brick at (11, 0, 10) interpenetrates the next-course cell (11.25, 0,
 * 10.75), which the solver's occupancy filter then drops, and it also becomes a snap origin of its
 * own; the nearest surviving candidate would be the seed's same-course pose at 13.33 cm rather than
 * the next-course one. So the two previews are taken back to back against the one-piece structure,
 * and only then is the Free pose committed.
 *
 * THE NUMBERS. Seed at (0, 0, 3.25); cursor (11, 0, 10), off-grid on both axes on purpose.
 *   next-course (11.25, 0, 10.75) — offset sqrt(0.25^2 + 0.75^2) =  0.79
 *   same-course (22.50, 0,  3.25) — offset sqrt(11.5^2 + 6.75^2) = 13.33
 * so Snap must take the next-course pose and form its one bed joint, while Free must sit at
 * (11, 0, 10) exactly with none. The Free pose's bottom face is 10 - 3.25 = 6.75 cm, so it is not
 * grounded either.
 *
 * THE COMMIT IS ASSERTED ON COUNTS — pieces 2, connections still 0 — which is the mechanism reading
 * of "bonded to nothing", exact and countable. No displacement assertion: nothing is released, and
 * a jointless brick resting exactly where it was put moves not at all.
 *
 * NEEDS A TICKING WORLD: a real world for the spawns, but it never ticks.
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

	/* SNAP: the same cursor is pulled 0.79 cm onto the running-bond bed and offered one joint. */
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

	/* FREE: the same cursor, the same structure, honoured verbatim and bonded to nothing. */
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

	/* THE COMMIT: one more piece, and still not one connection in the structure. */
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
 * A FREE PLACEMENT RESTING ON THE GROUND IS STILL GROUNDED — the pose rule does not care which mode
 * chose the pose.
 *
 * FREE IS THE MODE THAT MAKES THE HAZARD RUN THE OTHER WAY. Snap at least lands a piece on the
 * bond; Free lands it wherever the cursor was, so if grounded were ever inferred from "did a snap
 * bed me onto something" a Free brick laid on the earth would come out ungrounded and the player's
 * foundation would have nothing holding it up. It is the same one rule either way: bottom face
 * within one joint of Z = 0.
 *
 * THE TOLERANCE EDGE IS THE POINT, and the three rows straddle it with a brick half-height of 3.25
 * doing the arithmetic in plain sight:
 *   centre 3.25 -> bottom  0.00, on the ground plane           -> GROUNDED
 *   centre 4.25 -> bottom  1.00, exactly one joint up          -> GROUNDED (the inclusive edge)
 *   centre 4.50 -> bottom  1.25, a quarter centimetre past it  -> NOT grounded
 * The 1 cm is FSnapSettings::JointThicknessCm written out independently rather than imported, so a
 * retuned joint fails here instead of quietly agreeing. A `<` where the ruling says `<=` fails the
 * middle row; a tolerance widened to a course pitch fails the last.
 *
 * X = 50 KEEPS EVERY ROW CLEAR OF THE OTHERS' BOXES, which matters only for tidiness — Free is
 * honoured verbatim, overlaps and all — but it keeps the fixture readable.
 *
 * NEEDS A TICKING WORLD: a real world for the spawns, but it never ticks.
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

	/* PREVIEW-ONLY ROWS: preview mutates nothing, so all three see the same empty structure. */
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

	/* AND THE COMMIT CARRIES THE SAME ANSWER, one piece from each side of the edge. */
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
 * CANCEL TEARS THE BUILD DOWN — the structure, its bricks in the world, and the held pose all go.
 *
 * THE LEAK THIS CLOSES IS RECORDED IN TWO PLACES. PreviewBuildPiece's own header says "the caller
 * owns the cancel path: an abandoned build leaks an empty binding until Destroy", and
 * CURRENT_STATE's UI-4a deferral (d) says a second BeginBuild leaks the old structure. The
 * component is that caller, so CancelBuild is where the obligation is discharged: Destroy(id) —
 * which destroys the bricks' actors and drops the binding — then hide the ghost, clear the held
 * preview and forget the id.
 *
 * FOUR THINGS ARE ASSERTED, AND EVERY ONE IS A COUNT OR AN IDENTITY.
 *  - Find(id) is null: the binding is gone, not merely emptied.
 *  - GetStructureId() is INDEX_NONE: the component no longer names a build, so a stray confirm has
 *    nothing to grow.
 *  - NO ABrickActor from the build survives in the world — counted with TActorIterator before and
 *    after, EXCLUDING the ghost, which is the component's own actor and is hidden rather than
 *    destroyed (it is reused by the next build; EndPlay owns its destruction). A binding dropped
 *    while its actors stood would leave exactly this orphan, and it is invisible to a count of
 *    pieces.
 *  - A ConfirmPlace after the cancel FAILS CLOSED: a default ref and still no bricks. The held
 *    preview named a structure that no longer exists, so a component that kept it would try to
 *    commit into the void.
 *
 * NO DISPLACEMENT ANYWHERE: destruction is counted, never measured.
 *
 * NEEDS A TICKING WORLD: a real world for the spawns and destroys, but it never ticks.
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

	/* Two real placements: a grounded seed and its same-course neighbour. */
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

	/* THE CANCEL. */
	Comp->CancelBuild();

	TestNull(
		FString::Printf(TEXT("CancelBuild must DESTROY the structure, but Find(%d) still answers"),
			StructureId),
		Subsystem.Find(StructureId));

	TestEqual(
		FString::Printf(TEXT("a cancelled component names no build, it names %d"),
			Comp->GetStructureId()),
		Comp->GetStructureId(), static_cast<int32>(INDEX_NONE));

	/*
	 * THE GHOST MUST EXIST FOR THE HIDDEN ASSERTION TO MEAN ANYTHING. Two real placements have
	 * previewed, so the component has spawned one; a null here is a regression that would
	 * otherwise SKIP the hidden check silently, leaving the leg green by absence.
	 */
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

	/* AND A CONFIRM AFTER THE CANCEL PLACES NOTHING — the held preview went with the build. */
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
 * A SECOND BeginBuild CANCELS THE FIRST — the logged leak (CURRENT_STATE, UI-4a deferral (d)),
 * closed by routing the re-open through the same cancel path.
 *
 * TODAY THE COMPONENT SIMPLY ADOPTS A FRESH ID and the previous structure is left standing: its
 * binding stays in the subsystem's map forever and its bricks stay in the world, clickable, owned
 * by nobody. Opening a new build is the player saying "not that one", so it must cancel first.
 *
 * THE ASSERTION IS ON IDENTITY AND ABSENCE, not on any count of the new build: the OLD id must
 * resolve to nothing, the old brick's actor must be destroyed, and the new id must DIFFER (ids are
 * monotonic and never reused, so re-handing the same id would be its own bug). The new binding is
 * checked to exist and be empty, which is what a fresh build is.
 *
 * NEEDS A TICKING WORLD: a real world for the spawns and destroys, but it never ticks.
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

	/* THE RE-OPEN. */
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

#endif // WITH_DEV_AUTOMATION_TESTS
