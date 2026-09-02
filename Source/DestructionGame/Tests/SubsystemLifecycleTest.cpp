// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "EngineUtils.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE WORLD-LAYER LIFECYCLE GUARDS. Two defects live where the subsystem turns a layout
 * into actors and where it should turn them back: a refused build that leaves its bricks
 * standing, and the absence of any teardown at all. Both need a world that can spawn actors
 * — a world-free FStructureBinding test cannot see a leaked AActor — so both ride the shared
 * FBrickTestWorld harness under AGameModeBase (no scenario, so the world starts brick-empty).
 *
 * THE COUNT IS OF LIVE ABrickActors IN THE WORLD, not of binding handles: a leak is precisely
 * an actor the binding no longer (or never did) name, so a handle count cannot see it. A
 * TActorIterator over the test world is the only place the leak is visible.
 */
namespace SubsystemLifecycleTestSupport
{
	/** How many ABrickActors are alive in the world right now. */
	inline int32 CountBricks(UWorld* World)
	{
		int32 Count = 0;

		for (TActorIterator<ABrickActor> It(World); It; ++It)
		{
			if (IsValid(*It))
			{
				++Count;
			}
		}

		return Count;
	}
}

/**
 * A REFUSED BuildLayout MUST LEAVE NO BRICKS IN THE WORLD.
 *
 * BuildLayout spawns one ABrickActor per piece and only THEN hands the lot to AdoptLayout,
 * which refuses a layout whose Boxes array is not one-per-piece (StructureBinding.cpp's door).
 * On that refusal BuildLayout returns INDEX_NONE — but the actors it already spawned are never
 * destroyed, so a refused build silently litters the world with bricks that name no structure.
 *
 * THE FIXTURE IS A VALID WALL WITH ONE EXTRA BOX, which is the refusal that DOES NOT crash.
 * A layout with FEWER boxes than pieces makes BuildLayout read Layout.Boxes past its end in
 * the spawn loop (Layout.Boxes[PieceIndex] for the whole 0..PieceCount range) BEFORE AdoptLayout
 * can refuse it — and TArray's range check is fatal, so that case aborts the whole run rather
 * than failing an assertion. An EXTRA box keeps every spawn-loop index in bounds (the loop runs
 * to PieceCount, not Boxes.Num()), so the seven actors spawn cleanly and AdoptLayout then refuses
 * on Boxes.Num() != PieceCount — exercising the orphan-on-refusal path without the crash. The
 * same top-of-BuildLayout guard that would make this green (validate Boxes.Num() == PieceCount
 * before spawning anything) also closes the fatal short-boxes read, which is why one test drives
 * both halves of the hole. See the test-expert report accompanying this file.
 *
 * THE ASSERTION IS ON THE COUNT, NOT DISPLACEMENT: zero ABrickActor in the world after the
 * refused build. BuildLayout returning INDEX_NONE already passes today (AdoptLayout does refuse)
 * — the brick count is what bites.
 *
 * NEEDS A TICKING WORLD: yes for the spawn, though it never ticks — this is about what exists
 * in the world, not about anything falling.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSubsystemBuildLayoutRefusalLeavesNoBricksTest,
	"DestructionGame.World.Subsystem.BuildLayoutRefusalLeavesNoBricks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSubsystemBuildLayoutRefusalLeavesNoBricksTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace SubsystemLifecycleTestSupport;
	using namespace DestructionLayout;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	/* A brick-empty world under a plain game mode: the baseline the leak is measured against. */
	TestEqual(
		FString::Printf(TEXT("fixture: the world should start with no bricks, got %d"),
			CountBricks(TestWorld.World)),
		CountBricks(TestWorld.World), 0);

	/* A well-formed wall, then one extra box so its arrays no longer match one-per-piece. */
	FBrickLayout Layout;
	TestTrue(TEXT("fixture: RunningBond should lay the wall"), RunningBond(WallSpec(), Layout));

	TestEqual(
		FString::Printf(TEXT("fixture: a flush 2 x 3 wall should be %d pieces, got %d"),
			WallPieceCount, Layout.Structure.NumPieces()),
		Layout.Structure.NumPieces(), WallPieceCount);

	TestEqual(
		FString::Printf(TEXT("fixture: the layout should start one-box-per-piece, got %d boxes for %d pieces"),
			Layout.Boxes.Num(), Layout.Structure.NumPieces()),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());

	/*
	 * ONE EXTRA BOX, a duplicate of the last so it is a well-formed FPieceBox and the ONLY
	 * thing wrong with the layout is its length. Now Boxes.Num() == PieceCount + 1.
	 */
	const FPieceBox ExtraBox = Layout.Boxes.Last();
	Layout.Boxes.Add(ExtraBox);

	TestEqual(
		FString::Printf(TEXT("fixture: the layout should now carry one extra box, got %d boxes for %d pieces"),
			Layout.Boxes.Num(), Layout.Structure.NumPieces()),
		Layout.Boxes.Num(), Layout.Structure.NumPieces() + 1);

	const int32 StructureId = TestWorld.Subsystem->BuildLayout(Layout);

	/* The refusal itself: an out-of-step layout builds nothing and spends no id. */
	TestEqual(
		FString::Printf(TEXT("a layout with a box too many must be refused, got id %d"), StructureId),
		StructureId, static_cast<int32>(INDEX_NONE));

	/*
	 * THE BITE: no bricks left standing. Today BuildLayout has already spawned all seven
	 * actors before AdoptLayout refuses, and nothing destroys them — so this reads 7.
	 */
	TestEqual(
		FString::Printf(
			TEXT("a refused BuildLayout must leave no bricks orphaned in the world, got %d"),
			CountBricks(TestWorld.World)),
		CountBricks(TestWorld.World), 0);

	TestWorld.End();
	return true;
}

/**
 * DESTROYING A STRUCTURE MUST REMOVE ITS BRICKS FROM THE WORLD AND FORGET THE BINDING.
 *
 * The subsystem's Structures map only ever grows: there is no teardown, so the planned
 * scenario switcher's second build leaves the first structure's bricks standing and clickable.
 * Destroy(StructureId) is the missing half — it must destroy every actor the binding names,
 * drop the map entry so Find returns null, and leave a ray along a former piece hitting nothing.
 *
 * THIS RED IS RED-BECAUSE-UNIMPLEMENTED, NOT BECAUSE-UNCOMPILABLE. Destroy exists as an
 * empty stub returning false (the smallest declaration that lets this compile and run); the
 * behaviour is dev-expert's to write. Every assertion below fails against that stub: it returns
 * false, Find still hands back the binding, the seven bricks are still in the world, and a trace
 * through piece 0 still resolves to it.
 *
 * THREE INDEPENDENT WITNESSES so a partial implementation cannot pass by accident: the return
 * value, the count of live ABrickActors (a leaked actor is invisible to Find), and a TracePiece
 * that must now hit nothing (the clickability the switcher must not leave behind). The count is
 * against a measured baseline, so it cannot pass vacuously in an already-empty world.
 *
 * NEEDS A TICKING WORLD: yes, for the spawn and the trace; it never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSubsystemDestroyRemovesTheStructureTest,
	"DestructionGame.World.Subsystem.DestroyRemovesTheStructure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSubsystemDestroyRemovesTheStructureTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace SubsystemLifecycleTestSupport;
	using namespace DestructionLayout;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	const int32 BaselineBricks = CountBricks(TestWorld.World);

	TestEqual(
		FString::Printf(TEXT("fixture: the world should start with no bricks, got %d"), BaselineBricks),
		BaselineBricks, 0);

	/*
	 * A REFERENCE LAYOUT, laid separately, so there is a box centre to fire a trace through
	 * that did not come out of the subsystem. The subsystem lays its own copy from the same
	 * spec, so piece 0's actor sits at Reference.Boxes[0].CentreCm.
	 */
	FBrickLayout Reference;
	TestTrue(TEXT("fixture: RunningBond should lay the reference wall"), RunningBond(WallSpec(), Reference));

	const int32 StructureId = TestWorld.Subsystem->BuildRunningBond(WallSpec());

	TestTrue(
		FString::Printf(TEXT("fixture: BuildRunningBond should return a real id, got %d"), StructureId),
		StructureId != INDEX_NONE);

	TestNotNull(
		TEXT("fixture: Find should hand back the binding it just built"),
		TestWorld.Subsystem->Find(StructureId));

	TestEqual(
		FString::Printf(TEXT("fixture: the built wall should put %d bricks in the world, got %d"),
			WallPieceCount, CountBricks(TestWorld.World)),
		CountBricks(TestWorld.World), BaselineBricks + WallPieceCount);

	/*
	 * A TRACE THROUGH PIECE 0 HITS IT WHILE IT STANDS, so the same trace hitting nothing
	 * after Destroy is a real change and not a ray that always missed.
	 */
	const FPieceBox& Box0 = Reference.Boxes[0];
	const FVector TraceStart(Box0.CentreCm.X, Box0.CentreCm.Y - 200.0, Box0.CentreCm.Z);
	const FVector TraceEnd(Box0.CentreCm.X, Box0.CentreCm.Y + 200.0, Box0.CentreCm.Z);

	const FPieceHit Before = TestWorld.Subsystem->TracePiece(TraceStart, TraceEnd);

	TestEqual(
		FString::Printf(TEXT("fixture: a trace through piece 0 should hit it while it stands, got handle %d"),
			Before.PieceHandle),
		Before.PieceHandle, 0);

	/* THE ACT UNDER TEST. */
	const bool bDestroyed = TestWorld.Subsystem->Destroy(StructureId);

	TestTrue(
		TEXT("Destroy should report it tore down a structure it was holding"),
		bDestroyed);

	/* ONE: the binding is forgotten. */
	TestNull(
		TEXT("after Destroy, Find must no longer hand back the binding"),
		TestWorld.Subsystem->Find(StructureId));

	/* TWO: the bricks are gone from the world, back to the baseline. */
	TestEqual(
		FString::Printf(
			TEXT("after Destroy, the world should be back to %d bricks, got %d"),
			BaselineBricks, CountBricks(TestWorld.World)),
		CountBricks(TestWorld.World), BaselineBricks);

	/* THREE: the old ray hits nothing — nothing left to click where the wall stood. */
	const FPieceHit After = TestWorld.Subsystem->TracePiece(TraceStart, TraceEnd);

	TestEqual(
		FString::Printf(
			TEXT("after Destroy, a trace along a former piece must hit no piece, got handle %d"),
			After.PieceHandle),
		After.PieceHandle, static_cast<int32>(INDEX_NONE));

	TestEqual(
		FString::Printf(
			TEXT("after Destroy, that trace must carry no structure id, got %d"),
			After.Ref.StructureId),
		After.Ref.StructureId, static_cast<int32>(INDEX_NONE));

	TestWorld.End();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
