// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, and named differently from every other one in this module — an
 * anonymous namespace is private to a TRANSLATION UNIT, not to a file, and a unity build
 * merges many files into one. The world harness itself is NOT redeclared here: it lives
 * in Tests/BrickWorldTestSupport.h and is shared with Tests/BrickActorTest.cpp, because a
 * second copy of a floor height, a settle threshold and a tick length is two fixtures that
 * drift. Only what is specific to the push tests is below.
 */
namespace StructurePushTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * THE WALL THIS FILE USES IS BrickWorldTestSupport::NarrowWaistWallSpec(4), and the
	 * narrowness is the whole fixture — everything above course 0 reaches the ground only
	 * through the single brick of course 1. Four courses, so there are two pieces above the
	 * waist with each other as their only head joint and one more piece above those:
	 *
	 *      course 3            [ 5 ]              rests on 3 and 4
	 *      course 2         [ 3 ][ 4 ]            head joint 3-4 between them
	 *      course 1            [ 2 ]              THE WAIST - removed by the test
	 *      course 0         [ 0 ][ 1 ]            grounded
	 *
	 * Why a waist is the only shape that can see any of this, and why the wall is ragged and
	 * two bricks per course, is written where the spec now lives.
	 */

	/**
	 * FIXTURE PRECONDITIONS, asserted rather than assumed.
	 *
	 * 2 + 1 + 2 + 1 pieces, and the joints are: the two head joints 0-1 and 3-4, the four
	 * bed joints into the waist (0-2, 1-2, 2-3, 2-4) and the two carrying the top brick
	 * (3-5, 4-5). If a producer change moves either number, the arrangement this test
	 * reasons about is no longer the arrangement it built, and it should say so here rather
	 * than fail somewhere downstream with a plausible-looking wrong count.
	 */
	constexpr int32 NarrowWallPieceCount = 6;
	constexpr int32 NarrowWallJointCount = 8;

	/** The single course-1 brick everything above the bottom course hangs from. */
	constexpr int32 WaistPiece = 2;

	/**
	 * Exactly the pieces that lose their path to the ground when the waist goes.
	 *
	 * 3 and 4 lose their only bed joint beneath and fall back on the head joint between
	 * them, so each names the other as its support. That looks like a cycle, and it is NOT
	 * reported as one: LoadReturnsToPiece walks LoadPaths, which holds only supports that
	 * themselves reach the ground, and neither of these does — so the walk finds nothing and
	 * both read plain Falling. 5 keeps two intact bed joints, onto 3 and 4, and is Falling
	 * for the ordinary reason that what was carrying it is not being carried.
	 *
	 * SO NOTHING HERE IS Stranded, AND THAT IS ASSERTED RATHER THAN ASSUMED. Stranded means
	 * the solver declined to divide load round a knot — a limitation of the model, not a
	 * statement that anything fell — and a collapse fixture calibrated on one would look
	 * identical while measuring something else entirely. CURRENT_STATE.md flags exactly this
	 * trap for the collapse test, and three bricks with no load path at all is the shape that
	 * avoids it.
	 *
	 * 0 and 1 are grounded and stay exactly where they were laid. Piece 2 is neither: it
	 * has been removed, so it is never released and has no actor left to release.
	 */
	constexpr bool bOrphanedByRemovingTheWaist[NarrowWallPieceCount] =
	{
		false, false, false, true, true, true
	};

	constexpr int32 ExpectedReleaseCount = 3;

	/**
	 * How far the orphans have to fall, and why it is NOT the 50 cm of clear air the brick
	 * actor test gets.
	 *
	 * Orphaning a piece means taking away what was under it, so the hole is one course. The
	 * course pitch is 7.5 cm and a brick is 6.5 cm tall, so pieces 3 and 4 start with their
	 * undersides at Z = 15 and land on the top of course 0 at Z = 6.5: a drop of 8.5 cm.
	 * Piece 5 lands on top of them, about 9.5 cm below where it was laid.
	 *
	 * That is comfortably clear of BrickWorldTestSupport::FallenAtLeastCm, which is 5 cm and
	 * is itself five times the 1 cm mortar joint a brick settling into its own gap would
	 * move. It is a smaller margin than a brick dropped over open floor, and it is the best
	 * available in a wall where removing ONE brick genuinely orphans something — which is
	 * the property this fixture is chosen for.
	 */

	/**
	 * When the second push happens, and why it is EARLY.
	 *
	 * The idempotence claim is that a second SolveAndPush does not disturb bricks already
	 * falling, and the assertion that can fail is the linear velocity surviving the call —
	 * "still simulating" is unfalsifiable, because SetSimulatePhysics(true) on an
	 * already-simulating body satisfies it either way while quietly recreating the body at
	 * rest. So the comparison has to be made while the bricks are genuinely moving. Three
	 * physics steps is 0.05 s, about -49 cm/s and 1.2 cm of travel, so nothing has landed
	 * yet; by 0.25 s these bricks have already hit the course below and the comparison would
	 * be 0 against 0.
	 */
	constexpr double SecondPushAtSeconds = 0.05;
	constexpr double RemainingFallSeconds = 0.95;

	/** Well under the -49 cm/s three steps of gravity give, and well clear of zero. */
	constexpr double FallingFasterThanCmPerSecond = -20.0;

	const TCHAR* SupportName(EPieceSupport Support)
	{
		switch (Support)
		{
		case EPieceSupport::Grounded:  return TEXT("Grounded");
		case EPieceSupport::Supported: return TEXT("Supported");
		case EPieceSupport::Stranded:  return TEXT("Stranded");
		default:                       return TEXT("Falling");
		}
	}

	/** The whole structure's answer in one log line, so a failure reads without a debugger. */
	void ReportSupport(FAutomationTestBase& Test, const FStructureBinding& Binding, const TCHAR* When)
	{
		FString Line;

		for (int32 Piece = 0; Piece < Binding.NumPieces(); ++Piece)
		{
			Line += FString::Printf(
				TEXT("%s%d=%s%s"),
				Piece == 0 ? TEXT("") : TEXT(", "),
				Piece,
				SupportName(Binding.GetStructure().GetPieceSupport(Piece)),
				Binding.IsPieceRemoved(Piece) ? TEXT("(removed)") : TEXT(""));
		}

		Test.AddInfo(FString::Printf(TEXT("support %s: %s"), When, *Line));
	}
}

/**
 * A WALL THAT IS STANDING IS SOLVED AND PUSHED AND NOTHING COMES DOWN.
 *
 * THIS IS THE CONTROL, AND WITHOUT IT THE COLLAPSE TEST IS FREE. "Release everything the
 * binding knows about" passes an assertion that the orphans fell; only an untouched wall
 * that stays up can tell that implementation from the real one. Proved to bite by exactly
 * that mutation.
 *
 * IT ALSO PINS THAT A PUSH WITH NO SOLVE BEHIND IT RELEASES NOTHING. FStructureBinding
 * ::ApplyResults refuses to act on a piece the last solve never answered for, because
 * EPieceSupport::Falling is also what an ABSENT answer reads as — and before any solve
 * that is every piece in the wall, foundation included. SolveAndPush inherits that
 * obligation the moment it is written, and its solve is what discharges it, so the order
 * of the two halves is load-bearing rather than stylistic. That row is GREEN ON ARRIVAL
 * (the guard already exists and is pinned by Core.StructureBinding.ReleaseNeedsASolve); it
 * is here as a regression net so nobody optimises the solve out of SolveAndPush later.
 *
 * AND AN UNKNOWN STRUCTURE ID FAILS CLOSED — no crash, and nothing released anywhere,
 * which is the second half and the one a bare "it did not crash" would miss.
 *
 * NEEDS A TICKING WORLD: yes. A full second of real gravity with nothing released is the
 * only way "the wall stands" means anything; a wall of kinematic actors that was never
 * ticked would stand however wrong the answer was.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructurePushStandingWallReleasesNothingTest,
	"DestructionGame.World.Push.StandingWallReleasesNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructurePushStandingWallReleasesNothingTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace StructurePushTestSupport;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	/* Nothing built yet, so every id is unknown, including the one about to be handed out. */
	TestEqual(
		TEXT("SolveAndPush on a subsystem holding no structures at all must release nothing"),
		TestWorld.Subsystem->SolveAndPush(0), 0);

	const int32 StructureId = TestWorld.Subsystem->BuildRunningBond(NarrowWaistWallSpec(4));
	FStructureBinding* Binding = TestWorld.Subsystem->Find(StructureId);

	TestNotNull(
		*FString::Printf(TEXT("fixture: BuildRunningBond returned %d and Find should hand back its binding"),
			StructureId),
		Binding);

	if (Binding == nullptr)
	{
		TestWorld.End();
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("fixture: the narrow wall should span %d handles, got %d"),
			NarrowWallPieceCount, Binding->NumPieces()),
		Binding->NumPieces(), NarrowWallPieceCount);

	TestEqual(
		FString::Printf(TEXT("fixture: the narrow wall should carry %d joints, got %d"),
			NarrowWallJointCount, Binding->GetStructure().NumConnections()),
		Binding->GetStructure().NumConnections(), NarrowWallJointCount);

	if (Binding->NumPieces() != NarrowWallPieceCount)
	{
		TestWorld.End();
		return true;
	}

	/*
	 * THE OBLIGATION SolveAndPush INHERITS, ASSERTED BEFORE ANYTHING SOLVES. Green on
	 * arrival — see the note on the test — and it is the reason a push may never be run
	 * without the solve that precedes it.
	 */
	TestEqual(
		TEXT("a push with no solve behind it must release nothing: no answer is not an instruction"),
		Binding->ApplyResults(), 0);

	TArray<ABrickActor*> Bricks;
	TArray<FVector> LaidAt;

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		ABrickActor* Brick = BrickAt(*this, *Binding, Piece);

		if (Brick == nullptr)
		{
			TestWorld.End();
			return true;
		}

		Bricks.Add(Brick);
		LaidAt.Add(Brick->GetActorLocation());
	}

	TestEqual(
		TEXT("SolveAndPush on a wall that is standing must release nothing"),
		TestWorld.Subsystem->SolveAndPush(StructureId), 0);

	ReportSupport(*this, *Binding, TEXT("after the first push"));

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		const EPieceSupport Support = Binding->GetStructure().GetPieceSupport(Piece);

		/*
		 * THE MECHANISM BEHIND THE OUTCOME. Every piece is held up, so nothing SHOULD be
		 * released — which is what makes "nothing moved" a statement about the push rather
		 * than about a wall that had no way of moving.
		 */
		TestTrue(
			FString::Printf(TEXT("piece %d in a standing wall should be held up, the solver says %s"),
				Piece, SupportName(Support)),
			Support == EPieceSupport::Grounded || Support == EPieceSupport::Supported);

		TestTrue(
			FString::Printf(TEXT("piece %d must not be released by a push on a standing wall"), Piece),
			!Binding->IsReleased(Piece));

		TestTrue(
			FString::Printf(TEXT("brick %d must still be kinematic after a push on a standing wall"), Piece),
			Bricks[Piece]->GetMesh() != nullptr && !Bricks[Piece]->GetMesh()->IsSimulatingPhysics());
	}

	/*
	 * FAIL CLOSED ON AN ID THAT NAMES NOTHING, and the wall is re-checked afterwards: a
	 * push that ignored its argument and swept every binding it owns would answer zero
	 * here quite happily while releasing the whole wall.
	 */
	TestEqual(
		TEXT("SolveAndPush(INDEX_NONE) must release nothing"),
		TestWorld.Subsystem->SolveAndPush(INDEX_NONE), 0);

	TestEqual(
		FString::Printf(TEXT("SolveAndPush(%d) names no structure and must release nothing"),
			StructureId + 1),
		TestWorld.Subsystem->SolveAndPush(StructureId + 1), 0);

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		TestTrue(
			FString::Printf(TEXT("piece %d must survive a push aimed at an id that names nothing"), Piece),
			!Binding->IsReleased(Piece));
	}

	/* ONE SECOND OF REAL GRAVITY, ON A FIXED STEP, AND THE WALL IS STILL A WALL. */
	TestWorld.TickSeconds(1.0);

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		const double DriftCm = FVector::Dist(Bricks[Piece]->GetActorLocation(), LaidAt[Piece]);

		TestTrue(
			FString::Printf(
				TEXT("brick %d must not move in a second of gravity after a push that released nothing; it moved %.6f cm"),
				Piece, DriftCm),
			DriftCm < DriftToleranceCm);

		TestTrue(
			FString::Printf(TEXT("brick %d must still be kinematic a second later"), Piece),
			Bricks[Piece]->GetMesh() != nullptr && !Bricks[Piece]->GetMesh()->IsSimulatingPhysics());
	}

	/* And nothing about a settled wall changes on being asked again. */
	TestEqual(
		TEXT("a second SolveAndPush on the same standing wall must release nothing"),
		TestWorld.Subsystem->SolveAndPush(StructureId), 0);

	TestWorld.End();

	return true;
}

/**
 * TAKE OUT THE ONE BRICK EVERYTHING ABOVE RESTS ON, AND EXACTLY THE PIECES THAT LOST
 * THEIR PATH TO THE GROUND ARE RELEASED AND FALL.
 *
 * WHICH HANDLES, NOT HOW MANY. A count is satisfied by releasing any three bricks, and
 * the defect that matters here — a push that walked the wrong array, or released by
 * position rather than by the solver's answer — produces a wall that comes apart in the
 * wrong place while every count agrees. So the assertion is per handle, against a set
 * derived from the tier rule rather than from what the code did.
 *
 * AND THE MECHANISM IS NOT THE OUTCOME, so both are asserted. DESIGN.md §4 is explicit
 * that an integration test has to measure the structure actually moving: a binding flag
 * flipping is a step, and a piece can be flagged released while nothing ever hands it to
 * physics. BOTH HALVES OF THE MOVEMENT CLAIM MATTER TOO — "the orphans fell" alone passes
 * when the entire world drops through the floor, so the still-supported bricks are
 * asserted not to have moved in the same second.
 *
 * THE REMOVED BRICK'S ACTOR IS DESTROYED BY THE TEST. FStructureBinding::RemovePiece takes
 * the piece out of the graph and clears the binding's actor, but nothing destroys the actor
 * itself yet — that belongs to the piece context menu's Delete action, which does not exist.
 * Left in place it would be a kinematic brick still filling the hole, the orphans would land
 * on it after 1 cm and the fall assertion would be measuring the mortar settle it is
 * specifically chosen to be distinguishable from.
 *
 * NEEDS A TICKING WORLD: yes, and it is the point of the test.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructurePushOrphanedPiecesFallTest,
	"DestructionGame.World.Push.LosingASupportDropsExactlyTheOrphans",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructurePushOrphanedPiecesFallTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace StructurePushTestSupport;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	const int32 StructureId = TestWorld.Subsystem->BuildRunningBond(NarrowWaistWallSpec(4));
	FStructureBinding* Binding = TestWorld.Subsystem->Find(StructureId);

	TestNotNull(
		*FString::Printf(TEXT("fixture: BuildRunningBond returned %d and Find should hand back its binding"),
			StructureId),
		Binding);

	if (Binding == nullptr)
	{
		TestWorld.End();
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("fixture: the narrow wall should span %d handles, got %d"),
			NarrowWallPieceCount, Binding->NumPieces()),
		Binding->NumPieces(), NarrowWallPieceCount);

	TestEqual(
		FString::Printf(TEXT("fixture: the narrow wall should carry %d joints, got %d"),
			NarrowWallJointCount, Binding->GetStructure().NumConnections()),
		Binding->GetStructure().NumConnections(), NarrowWallJointCount);

	if (Binding->NumPieces() != NarrowWallPieceCount)
	{
		TestWorld.End();
		return true;
	}

	TArray<ABrickActor*> Bricks;
	TArray<FVector> LaidAt;

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		ABrickActor* Brick = BrickAt(*this, *Binding, Piece);

		if (Brick == nullptr)
		{
			TestWorld.End();
			return true;
		}

		Bricks.Add(Brick);
		LaidAt.Add(Brick->GetActorLocation());
	}

	/*
	 * FIXTURE PRECONDITION: THE WALL STANDS AS BUILT. If this wall came apart on its own,
	 * every assertion below about what the removal caused would be measuring something else.
	 */
	TestEqual(
		TEXT("fixture: the wall as built should stand, so the first push releases nothing"),
		TestWorld.Subsystem->SolveAndPush(StructureId), 0);

	ReportSupport(*this, *Binding, TEXT("as built"));

	/*
	 * OUT COMES THE WAIST. The actor is captured first because RemovePiece clears the
	 * binding's pointer to it, and destroyed afterwards because the graph is what decides
	 * whether a removal happened at all.
	 */
	ABrickActor* WaistBrick = Bricks[WaistPiece];

	TestTrue(
		FString::Printf(TEXT("fixture: removing piece %d should report a live piece removed"), WaistPiece),
		Binding->RemovePiece(WaistPiece));

	WaistBrick->Destroy();
	Bricks[WaistPiece] = nullptr;

	const int32 ReleasedCount = TestWorld.Subsystem->SolveAndPush(StructureId);

	ReportSupport(*this, *Binding, TEXT("after the waist was removed"));

	TestEqual(
		FString::Printf(
			TEXT("removing the waist should release the %d pieces above it, SolveAndPush released %d"),
			ExpectedReleaseCount, ReleasedCount),
		ReleasedCount, ExpectedReleaseCount);

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		if (Piece == WaistPiece)
		{
			/* A removed piece has no actor left, so it is never released whatever it reads as. */
			TestTrue(
				TEXT("the removed waist piece must not be released"),
				!Binding->IsReleased(Piece));

			TestNull(
				TEXT("the removed waist piece must have no actor left in its binding"),
				Binding->GetActor(Piece));

			continue;
		}

		const bool bExpected = bOrphanedByRemovingTheWaist[Piece];
		const EPieceSupport Support = Binding->GetStructure().GetPieceSupport(Piece);

		TestTrue(
			FString::Printf(
				TEXT("piece %d should%s be released once the waist has gone; the solver says %s and IsReleased is %s"),
				Piece, bExpected ? TEXT("") : TEXT(" not"), SupportName(Support),
				Binding->IsReleased(Piece) ? TEXT("true") : TEXT("false")),
			Binding->IsReleased(Piece) == bExpected);

		/*
		 * THIS IS A COLLAPSE, NOT A SOLVER STALL. A Stranded piece is one the solver
		 * declined to route load around rather than one that lost its support, and a
		 * fixture calibrated on a knot would come down looking exactly the same. See the
		 * note on bOrphanedByRemovingTheWaist.
		 */
		TestTrue(
			FString::Printf(
				TEXT("piece %d must not be Stranded: that would make this a solver limitation rather than a collapse"),
				Piece),
			Support != EPieceSupport::Stranded);

		if (bExpected)
		{
			TestTrue(
				FString::Printf(TEXT("orphaned piece %d should read Falling, the solver says %s"),
					Piece, SupportName(Support)),
				Support == EPieceSupport::Falling);
		}

		/*
		 * AND THE FLAG REACHED THE BRICK. IsReleased is the binding's record; simulating is
		 * the actor's, and ABrickActor deliberately keeps no second copy of the answer — so
		 * a push that flipped every flag and called nothing is caught right here.
		 */
		const bool bSimulating =
			Bricks[Piece]->GetMesh() != nullptr && Bricks[Piece]->GetMesh()->IsSimulatingPhysics();

		TestTrue(
			FString::Printf(
				TEXT("brick %d should%s be simulating physics once the waist has gone; it is %s"),
				Piece, bExpected ? TEXT("") : TEXT(" not"), bSimulating ? TEXT("simulating") : TEXT("kinematic")),
			bSimulating == bExpected);
	}

	/*
	 * A SHORT TICK, THEN THE SECOND PUSH WHILE THE BRICKS ARE STILL IN THE AIR — see
	 * SecondPushAtSeconds for why it cannot wait until they have landed.
	 */
	TestWorld.TickSeconds(SecondPushAtSeconds);

	TArray<FVector> VelocityBefore;

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		VelocityBefore.Add(
			Bricks[Piece] != nullptr && Bricks[Piece]->GetMesh() != nullptr
				? Bricks[Piece]->GetMesh()->GetPhysicsLinearVelocity()
				: FVector::ZeroVector);

		if (Piece == WaistPiece || !bOrphanedByRemovingTheWaist[Piece])
		{
			continue;
		}

		/* The precondition for the idempotence row: 0 against 0 would prove nothing. */
		TestTrue(
			FString::Printf(
				TEXT("fixture: released brick %d should be moving after %.2f s, velocity Z is %.3f cm/s"),
				Piece, SecondPushAtSeconds, VelocityBefore[Piece].Z),
			VelocityBefore[Piece].Z < FallingFasterThanCmPerSecond);
	}

	TestEqual(
		TEXT("a second SolveAndPush with nothing changed must release nothing"),
		TestWorld.Subsystem->SolveAndPush(StructureId), 0);

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		if (Piece == WaistPiece || !bOrphanedByRemovingTheWaist[Piece])
		{
			continue;
		}

		/*
		 * THE ROW THE IDEMPOTENCE CLAIM RESTS ON. Nothing ticked between the two reads, so a
		 * push whose release derives its state from the body and returns early leaves this
		 * exactly equal; one that re-creates the body loses the velocity entirely and a brick
		 * a twentieth of a second into its fall is suddenly hanging still. "Still simulating"
		 * cannot tell those apart.
		 */
		const FVector VelocityAfter = Bricks[Piece]->GetMesh()->GetPhysicsLinearVelocity();

		TestTrue(
			FString::Printf(
				TEXT("a second push must not disturb falling brick %d: velocity was (%.3f, %.3f, %.3f), now (%.3f, %.3f, %.3f)"),
				Piece,
				VelocityBefore[Piece].X, VelocityBefore[Piece].Y, VelocityBefore[Piece].Z,
				VelocityAfter.X, VelocityAfter.Y, VelocityAfter.Z),
			VelocityAfter.Equals(VelocityBefore[Piece], 1.0e-3));
	}

	/* One second of simulated time in total, on a fixed step, never on a settle poll. */
	TestWorld.TickSeconds(RemainingFallSeconds);

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		if (Piece == WaistPiece)
		{
			continue;
		}

		const double FellCm = LaidAt[Piece].Z - Bricks[Piece]->GetActorLocation().Z;
		const double MovedCm = FVector::Dist(Bricks[Piece]->GetActorLocation(), LaidAt[Piece]);

		if (bOrphanedByRemovingTheWaist[Piece])
		{
			AddInfo(FString::Printf(
				TEXT("released brick %d fell %.3f cm in one second, from Z %.3f to Z %.3f"),
				Piece, FellCm, LaidAt[Piece].Z, Bricks[Piece]->GetActorLocation().Z));

			TestTrue(
				FString::Printf(
					TEXT("released brick %d should have fallen more than %.1f cm in a second; it dropped %.3f cm"),
					Piece, FallenAtLeastCm, FellCm),
				FellCm > FallenAtLeastCm);
		}
		else
		{
			/*
			 * THE OTHER HALF, AND IT IS NOT DECORATION. Without it, a world in which
			 * everything fell through the floor passes the row above.
			 */
			TestTrue(
				FString::Printf(
					TEXT("brick %d is still held up and must not have moved; it drifted %.6f cm"),
					Piece, MovedCm),
				MovedCm < DriftToleranceCm);

			TestTrue(
				FString::Printf(TEXT("brick %d is still held up and must still be kinematic"), Piece),
				Bricks[Piece]->GetMesh() != nullptr && !Bricks[Piece]->GetMesh()->IsSimulatingPhysics());
		}
	}

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
