// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "CollisionQueryParams.h"
#include "Engine/HitResult.h"
#include "GameFramework/Actor.h"

#include "Core/Profiles/MaterialProfiles.h"
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
}

/**
 * BeginBuild opens a live structure; UpdatePreviewAt drives the ghost to the predicted snap and
 * shows it; ConfirmPlace grows the structure by one real piece at the previewed pose.
 *
 * STEP BY STEP:
 *  1. A component attached to an actor in the world. BeginBuild registers a structure —
 *     GetStructureId names one and Subsystem.Find(id) is a non-null, empty binding.
 *  2. bBuildGrounded = true; a preview at the origin then ConfirmPlace seeds a grounded brick —
 *     Find(id) holds 1 piece, recorded grounded.
 *  3. bBuildGrounded = false; UpdatePreviewAt((11, 0, 7.5)) returns a valid BrickNextCourse
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
	Comp->bBuildGrounded = true;
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
	Comp->bBuildGrounded = false;
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
	Comp->bBuildGrounded = true;
	Comp->UpdatePreviewAt(FVector(0.0, 0.0, 0.0));
	Comp->ConfirmPlace();

	/* The one real next-course placement: a fresh valid preview, then confirm grows it to 2. */
	Comp->bBuildGrounded = false;
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

#endif // WITH_DEV_AUTOMATION_TESTS
