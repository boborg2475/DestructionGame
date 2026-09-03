// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "EngineUtils.h"
#include "Core/PieceActions.h"
#include "Core/StructureBinding.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * TWO CHARACTERISATION GUARDS OVER UDestructionStructureSubsystem::Destroy, pinning behaviour
 * that already ships (review item 9). They are EXPECTED GREEN ON ARRIVAL: the production code
 * exists and was reviewed, so these lock it against regression rather than driving anything.
 * If either goes red it is a real defect, not a test to force green.
 *
 * Both need a world that can spawn actors — a world-free FStructureBinding test cannot see a
 * leaked or a correctly-torn-down AActor — so both ride the shared FBrickTestWorld harness
 * under AGameModeBase (no scenario, so the world starts brick-empty). The count under test is
 * of LIVE ABrickActors in the world, read by a TActorIterator: a survivor Destroy failed to
 * tear down is precisely an actor the (now-removed) binding no longer names, so only the world
 * iterator can see it. This reuses SubsystemLifecycleTestSupport::CountBricks, declared in the
 * sibling lifecycle test file's header-free namespace — so it is redeclared locally here rather
 * than shared, for the same one-per-translation-unit reason the harness header documents.
 */
namespace SubsystemDestroyCharacterizationSupport
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

	/** The Delete row, looked up by label so nothing hard-codes a position in the table. */
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
 * DESTROY SKIPS AN ALREADY-RELEASED PIECE AND TEARS DOWN ONLY WHAT SURVIVES THE CAST.
 *
 * Destroy iterates the binding's whole handle range, casts each handle's actor to ABrickActor
 * and Destroys only what the cast returns non-null. A piece released by any other route first —
 * here, Delete committed through CommitPieceAction, which consumes FPieceActionResult::
 * ActorToDestroy and destroys the brick, after which the binding's weak pointer answers null
 * for that handle — must be SKIPPED cleanly rather than dereferenced. This test releases piece
 * 0 that way, then Destroys the structure and asserts on the MECHANISM: it returned true, the
 * binding is forgotten (Find null), and every SURVIVING brick left the world (count back to the
 * measured baseline). Reaching those assertions at all is the proof it did not crash on the
 * null-casting released handle.
 *
 * THE COUNTS ARE EXACT AND MEASURED. A flush 2x3 wall is 7 pieces. CommitPieceAction(Delete)
 * destroys exactly ONE actor — the deleted piece's; any pieces its re-solve orphans are pushed
 * to physics (made to simulate), which does NOT destroy their actors, so they stay in the world
 * and still cast to ABrickActor. So the world holds 6 bricks after the release, and Destroy must
 * take it to 0 by tearing down those 6 survivors while skipping the 1 released handle.
 *
 * NEEDS A TICKING WORLD: yes for the spawn and the commit's push; it never actually ticks.
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

	/* A flush 2x3 wall: 7 pieces spawned as 7 ABrickActors. */
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

	/*
	 * RELEASE PIECE 0 THROUGH THE REAL WORLD PATH. CommitPieceAction re-resolves the ref,
	 * runs Delete, and consumes the orphan it hands back — destroying that one actor. This is
	 * the release the Destroy-under-test must then skip: after it, GetActor(0) is null.
	 */
	const int32 ReleasedPiece = 0;
	const FPieceRef ReleasedRef{ StructureId, ReleasedPiece };

	TestTrue(
		TEXT("fixture: committing Delete against piece 0 should report that it ran"),
		TestWorld.Subsystem->CommitPieceAction(ReleasedRef, *Delete));

	/*
	 * THE PRECONDITION THE SKIP PATH RELIES ON: the released handle no longer casts to a
	 * brick. Destroy's Cast<ABrickActor>(GetActor(0)) will be null and must be skipped.
	 */
	TestTrue(
		FString::Printf(TEXT("fixture: the released piece should read removed, IsPieceRemoved reports %d"),
			Binding->IsPieceRemoved(ReleasedPiece) ? 1 : 0),
		Binding->IsPieceRemoved(ReleasedPiece));

	TestNull(
		TEXT("fixture: the released piece must have let go of its actor, so Destroy skips it"),
		Binding->GetActor(ReleasedPiece));

	/*
	 * ONLY THE DELETED ACTOR LEFT THE WORLD. Any piece the re-solve orphaned was pushed to
	 * physics, not destroyed, so it is still a live ABrickActor here — hence exactly 6 remain,
	 * which is the number of SURVIVORS Destroy must then tear down.
	 */
	TestEqual(
		FString::Printf(
			TEXT("fixture: exactly the released brick should be gone, leaving %d, got %d"),
			BaselineBricks + WallPieceCount - 1, CountBricks(TestWorld.World)),
		CountBricks(TestWorld.World), BaselineBricks + WallPieceCount - 1);

	/* THE ACT UNDER TEST. Do not touch Binding after this: Destroy frees it. */
	const bool bDestroyed = TestWorld.Subsystem->Destroy(StructureId);

	TestTrue(
		TEXT("Destroy should report it tore down a structure it was holding"),
		bDestroyed);

	/* ONE: the binding is forgotten. */
	TestNull(
		TEXT("after Destroy, Find must no longer hand back the binding"),
		TestWorld.Subsystem->Find(StructureId));

	/*
	 * TWO: every SURVIVING brick left the world, back to the baseline. The released handle was
	 * skipped (its actor was already gone), the six survivors were destroyed, and reaching this
	 * assertion is itself the proof the null-casting released handle did not crash the loop.
	 */
	TestEqual(
		FString::Printf(
			TEXT("after Destroy, the world should be back to %d bricks, got %d"),
			BaselineBricks, CountBricks(TestWorld.World)),
		CountBricks(TestWorld.World), BaselineBricks);

	TestWorld.End();
	return true;
}

/**
 * IDS ARE MONOTONIC ACROSS A DESTROY — A TORN-DOWN ID IS NEVER RECYCLED.
 *
 * BuildLayout sets NextStructureId = StructureId + 1 on adopt, and Destroy deliberately leaves
 * NextStructureId where it is ("ids are monotonic and never reused"), so a ref left over from a
 * torn-down structure can never resolve against a later structure that happened to reuse the
 * slot. This builds A, Destroys it, then builds B and asserts B's id is STRICTLY GREATER than
 * A's — B did not inherit A's freed id. A third build C then pins that the counter keeps
 * climbing past B as well, so the property is monotonicity of the counter and not merely
 * "the next id differs".
 *
 * NEEDS A TICKING WORLD: yes for the spawns; it never ticks.
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

	/* STRUCTURE A. */
	const int32 IdA = TestWorld.Subsystem->BuildRunningBond(WallSpec());

	TestTrue(
		FString::Printf(TEXT("fixture: structure A should get a real id, got %d"), IdA),
		IdA != INDEX_NONE);

	/* TEAR A DOWN. Destroy leaves NextStructureId where it is. */
	TestTrue(
		FString::Printf(TEXT("fixture: Destroy should tear down structure A (id %d)"), IdA),
		TestWorld.Subsystem->Destroy(IdA));

	TestNull(
		TEXT("fixture: after Destroy, A's id must name nothing"),
		TestWorld.Subsystem->Find(IdA));

	/* STRUCTURE B — must NOT reuse A's freed id. */
	const int32 IdB = TestWorld.Subsystem->BuildRunningBond(WallSpec());

	TestTrue(
		FString::Printf(TEXT("fixture: structure B should get a real id, got %d"), IdB),
		IdB != INDEX_NONE);

	TestTrue(
		FString::Printf(
			TEXT("B's id (%d) must be strictly greater than the destroyed A's id (%d) — never recycled"),
			IdB, IdA),
		IdB > IdA);

	/* STRUCTURE C — the counter keeps climbing past B, so this is monotonicity, not just "differs". */
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
