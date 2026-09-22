// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "EngineUtils.h"
#include "Core/PieceActions.h"
#include "Core/StructureBinding.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Characterisation guards over UDestructionStructureSubsystem::Destroy (review item 9). Green on
 * arrival; a red is a real defect. They need a world that spawns actors, via FBrickTestWorld
 * (starts brick-empty). Live bricks are counted with TActorIterator, since a leaked actor is one
 * the removed binding no longer names. CountBricks is redeclared here rather than shared with the
 * lifecycle test's namespace.
 */
namespace SubsystemDestroyCharacterizationSupport
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

	/** The Delete row, found by label. */
	inline const FPieceAction* FindDelete()
	{
		for (const FPieceAction& Action : AllPieceActions())
		{
			if (Action.Label != nullptr && FCString::Strcmp(Action.Label, TEXT("Delete")) == 0)
			{
				return &Action;
			}
		}

		return nullptr;
	}
}

/**
 * Destroy skips a piece whose actor is already gone and tears down the rest. Piece 0 is deleted
 * through CommitPieceAction first, so its handle casts to null. Destroy must return true, forget
 * the binding and remove the 6 surviving bricks of the 7-piece wall (orphans pushed to physics
 * keep their actors). Needs a spawning world; never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSubsystemDestroyAfterReleaseCleansUpTest,
	"DestructionGame.World.Subsystem.DestroyAfterReleaseCleansUp",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSubsystemDestroyAfterReleaseCleansUpTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace SubsystemDestroyCharacterizationSupport;
	using namespace DestructionLayout;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	const FPieceAction* Delete = FindDelete();

	if (Delete == nullptr || Delete->CanRun == nullptr || Delete->Run == nullptr)
	{
		AddError(TEXT("fixture: the action table must contain a well-formed row labelled 'Delete'"));
		TestWorld.End();
		return true;
	}

	const int32 BaselineBricks = CountBricks(TestWorld.World);

	TestEqual(
		FString::Printf(TEXT("fixture: the world should start with no bricks, got %d"), BaselineBricks),
		BaselineBricks, 0);

	// A flush 2x3 wall: 7 bricks.
	const int32 StructureId = TestWorld.Subsystem->BuildRunningBond(WallSpec());

	TestTrue(
		FString::Printf(TEXT("fixture: BuildRunningBond should return a real id, got %d"), StructureId),
		StructureId != INDEX_NONE);

	FStructureBinding* const Binding = TestWorld.Subsystem->Find(StructureId);

	TestNotNull(TEXT("fixture: Find should hand back the binding it just built"), Binding);

	if (Binding == nullptr)
	{
		TestWorld.End();
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("fixture: the built wall should put %d bricks in the world, got %d"),
			WallPieceCount, CountBricks(TestWorld.World)),
		CountBricks(TestWorld.World), BaselineBricks + WallPieceCount);

	// Delete piece 0 through the real path, destroying its actor.
	const int32 ReleasedPiece = 0;
	const FPieceRef ReleasedRef{ StructureId, ReleasedPiece };

	TestTrue(
		TEXT("fixture: committing Delete against piece 0 should report that it ran"),
		TestWorld.Subsystem->CommitPieceAction(ReleasedRef, *Delete));

	// Precondition for the skip path: the released handle no longer has an actor.
	TestTrue(
		FString::Printf(TEXT("fixture: the released piece should read removed, IsPieceRemoved reports %d"),
			Binding->IsPieceRemoved(ReleasedPiece) ? 1 : 0),
		Binding->IsPieceRemoved(ReleasedPiece));

	TestNull(
		TEXT("fixture: the released piece must have let go of its actor, so Destroy skips it"),
		Binding->GetActor(ReleasedPiece));

	// Only the deleted actor left; orphans were pushed to physics, not destroyed.
	TestEqual(
		FString::Printf(
			TEXT("fixture: exactly the released brick should be gone, leaving %d, got %d"),
			BaselineBricks + WallPieceCount - 1, CountBricks(TestWorld.World)),
		CountBricks(TestWorld.World), BaselineBricks + WallPieceCount - 1);

	// Destroy frees Binding; do not touch it after this.
	const bool bDestroyed = TestWorld.Subsystem->Destroy(StructureId);

	TestTrue(
		TEXT("Destroy should report it tore down a structure it was holding"),
		bDestroyed);

	TestNull(
		TEXT("after Destroy, Find must no longer hand back the binding"),
		TestWorld.Subsystem->Find(StructureId));

	// Every survivor left the world; reaching here also shows the null handle did not crash.
	TestEqual(
		FString::Printf(
			TEXT("after Destroy, the world should be back to %d bricks, got %d"),
			BaselineBricks, CountBricks(TestWorld.World)),
		CountBricks(TestWorld.World), BaselineBricks);

	TestWorld.End();
	return true;
}

/**
 * Structure ids are monotonic across Destroy and never recycled, so a stale ref cannot resolve to
 * a later structure. Builds A, destroys it, then B and C: B > A and C > B. Needs a spawning world;
 * never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSubsystemIdsAreMonotonicAcrossDestroyTest,
	"DestructionGame.World.Subsystem.IdsAreMonotonicAcrossDestroy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSubsystemIdsAreMonotonicAcrossDestroyTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	const int32 IdA = TestWorld.Subsystem->BuildRunningBond(WallSpec());

	TestTrue(
		FString::Printf(TEXT("fixture: structure A should get a real id, got %d"), IdA),
		IdA != INDEX_NONE);

	TestTrue(
		FString::Printf(TEXT("fixture: Destroy should tear down structure A (id %d)"), IdA),
		TestWorld.Subsystem->Destroy(IdA));

	TestNull(
		TEXT("fixture: after Destroy, A's id must name nothing"),
		TestWorld.Subsystem->Find(IdA));

	// B must not reuse A's freed id.
	const int32 IdB = TestWorld.Subsystem->BuildRunningBond(WallSpec());

	TestTrue(
		FString::Printf(TEXT("fixture: structure B should get a real id, got %d"), IdB),
		IdB != INDEX_NONE);

	TestTrue(
		FString::Printf(
			TEXT("B's id (%d) must be strictly greater than the destroyed A's id (%d) — never recycled"),
			IdB, IdA),
		IdB > IdA);

	// C shows the counter keeps climbing, not merely differing.
	const int32 IdC = TestWorld.Subsystem->BuildRunningBond(WallSpec());

	TestTrue(
		FString::Printf(TEXT("fixture: structure C should get a real id, got %d"), IdC),
		IdC != INDEX_NONE);

	TestTrue(
		FString::Printf(
			TEXT("C's id (%d) must be strictly greater than B's id (%d)"),
			IdC, IdB),
		IdC > IdB);

	TestWorld.End();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
