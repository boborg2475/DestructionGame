// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "EngineUtils.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Subsystem lifecycle: a refused build must not leak bricks, and Destroy must tear a structure
 * down. Uses FBrickTestWorld under AGameModeBase (starts brick-empty). Counts live ABrickActors
 * in the world, since a leaked actor is invisible to binding handles.
 */
namespace SubsystemLifecycleTestSupport
{
	/** Live ABrickActors in the world. */
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
 * A refused BuildLayout leaves no bricks in the world. AdoptLayout refuses a layout whose Boxes
 * are not one per piece; the bricks must not be spawned (or must be cleaned up) when it does.
 *
 * The fixture has one extra box rather than one too few: too few would read past the array in
 * the spawn loop, a fatal range check. Validating Boxes.Num() == PieceCount before spawning fixes
 * both. The brick count is the real assertion; the INDEX_NONE return alone would not catch the leak.
 *
 * Needs a world to spawn into; never ticks.
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

	// Baseline: no bricks.
	TestEqual(
		FString::Printf(TEXT("fixture: the world should start with no bricks, got %d"),
			CountBricks(TestWorld.World)),
		CountBricks(TestWorld.World), 0);

	// A valid wall, then one extra box.
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

	// Duplicate the last box, so the only defect is the array length.
	const FPieceBox ExtraBox = Layout.Boxes.Last();
	Layout.Boxes.Add(ExtraBox);

	TestEqual(
		FString::Printf(TEXT("fixture: the layout should now carry one extra box, got %d boxes for %d pieces"),
			Layout.Boxes.Num(), Layout.Structure.NumPieces()),
		Layout.Boxes.Num(), Layout.Structure.NumPieces() + 1);

	const int32 StructureId = TestWorld.Subsystem->BuildLayout(Layout);

	// Refused: no id.
	TestEqual(
		FString::Printf(TEXT("a layout with a box too many must be refused, got id %d"), StructureId),
		StructureId, static_cast<int32>(INDEX_NONE));

	// No bricks left behind (without the guard, all seven spawned bricks remain).
	TestEqual(
		FString::Printf(
			TEXT("a refused BuildLayout must leave no bricks orphaned in the world, got %d"),
			CountBricks(TestWorld.World)),
		CountBricks(TestWorld.World), 0);

	TestWorld.End();
	return true;
}

/**
 * Destroy(StructureId) removes the structure's bricks from the world and forgets the binding, so
 * a scenario switch leaves nothing clickable behind.
 *
 * Checked three ways: Find returns null, the brick count returns to the measured baseline (a
 * leaked actor is invisible to Find), and a trace that hit piece 0 before now hits nothing.
 *
 * Needs a world for the spawn and trace; never ticks.
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

	// A separately laid reference layout, so the trace target doesn't come from the subsystem.
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

	// Positive control: the trace hits piece 0 before Destroy.
	const FPieceBox& Box0 = Reference.Boxes[0];
	const FVector TraceStart(Box0.CentreCm.X, Box0.CentreCm.Y - 200.0, Box0.CentreCm.Z);
	const FVector TraceEnd(Box0.CentreCm.X, Box0.CentreCm.Y + 200.0, Box0.CentreCm.Z);

	const FPieceHit Before = TestWorld.Subsystem->TracePiece(TraceStart, TraceEnd);

	TestEqual(
		FString::Printf(TEXT("fixture: a trace through piece 0 should hit it while it stands, got handle %d"),
			Before.PieceHandle),
		Before.PieceHandle, 0);

	const bool bDestroyed = TestWorld.Subsystem->Destroy(StructureId);

	TestTrue(
		TEXT("Destroy should report it tore down a structure it was holding"),
		bDestroyed);

	// 1. The binding is forgotten.
	TestNull(
		TEXT("after Destroy, Find must no longer hand back the binding"),
		TestWorld.Subsystem->Find(StructureId));

	// 2. The bricks are gone.
	TestEqual(
		FString::Printf(
			TEXT("after Destroy, the world should be back to %d bricks, got %d"),
			BaselineBricks, CountBricks(TestWorld.World)),
		CountBricks(TestWorld.World), BaselineBricks);

	// 3. The same trace now hits nothing.
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
