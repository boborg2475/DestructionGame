// Copyright Epic Games, Inc. All Rights Reserved.

#include <limits>

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
	Comp->bBuildGrounded = true;
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
	Comp->bBuildGrounded = false;
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
	Comp->bBuildGrounded = true;
	Comp->UpdatePreviewAt(FVector(0.0, 0.0, 0.0));
	Comp->ConfirmPlace();

	Comp->bBuildGrounded = false;
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
	Comp->bBuildGrounded = false;
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
	Comp->bBuildGrounded = true;
	Comp->UpdatePreviewAt(FVector(0.0, 0.0, 0.0));
	Comp->ConfirmPlace();

	Comp->bBuildGrounded = false;
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

#endif // WITH_DEV_AUTOMATION_TESTS
