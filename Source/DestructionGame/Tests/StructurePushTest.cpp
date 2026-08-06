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

	/*
	 * ================================================================================
	 * THE WALL THAT CANNOT HOLD ITSELF UP, AND THE ONE THAT CAN.
	 * ================================================================================
	 *
	 * Two walls the same producer lays from the same brick and the same mortar, differing
	 * only in how they finish at their ends. Everything below is derived here rather than
	 * imported, and the numbers are measurements of the SOLVER, which is unchanged by any
	 * of this — the question is only whether the world wire asks it to settle.
	 */

	/**
	 * A wall of the kind the game mode lays, with RAGGED ends: full bricks only, so
	 * alternate courses are a brick short and step in.
	 *
	 * WHY A RAGGED END CORBELS AND A FLUSH ONE DOES NOT, WHICH IS THE WHOLE FIXTURE.
	 * Running bond offsets alternate courses by half a cell, 11.25 cm. Where a course
	 * steps in, the end brick of the course ABOVE it overhangs into thin air and keeps
	 * exactly ONE bed joint — the 10.25 cm strip it still shares with the brick below —
	 * whose centroid sits 5.625 cm to the inside of the brick's own centre of mass. That
	 * 5.625 cm is a genuine eccentricity, it is the SAME on every course, and it is the
	 * identical lever arm the staircase corbel has (StaircaseWallTestSupport works it
	 * through). A flush end fills that half cell with a half bat, the end brick gets two
	 * supports, its centre of mass lands on the area-weighted centroid of them, and the
	 * eccentricity is exactly zero.
	 *
	 * SO THIS IS NOT A CONTRIVED WALL. It is the OTHER value of the one enum field the
	 * scenario spec sets, and a player who builds a ragged wall tall enough is entitled to
	 * watch it fall.
	 *
	 * TEN BRICKS PER COURSE RATHER THAN THIRTY, and the reason is arithmetic rather than
	 * cost: the corbel is a LOCAL phenomenon at each end, so the worst joint of a 40-course
	 * ragged wall reads 1.24833683 at ten wide and 1.24807195 at thirty. Ten wide keeps the
	 * world test to 380 actors and lets it tick.
	 */
	inline FRunningBondSpec RaggedWallSpec(int32 CoursesHigh, int32 BricksPerCourse)
	{
		FRunningBondSpec Spec;
		Spec.BrickSizeCm = FVector(21.5, 10.25, 6.5);
		Spec.JointThicknessCm = 1.0;
		Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
		Spec.CoursesHigh = CoursesHigh;
		Spec.BricksPerCourse = BricksPerCourse;
		Spec.End = EWallEnd::Ragged;
		Spec.Strength = GeneralPurposeMortar;
		return Spec;
	}

	/** The game mode's own scenario wall: 40 courses of 30, FLUSH. */
	inline FRunningBondSpec ScenarioWallSpec()
	{
		FRunningBondSpec Spec = RaggedWallSpec(40, 30);
		Spec.End = EWallEnd::Flush;
		return Spec;
	}

	constexpr int32 OverCapacityWallCourses = 40;
	constexpr int32 OverCapacityWallBricksPerCourse = 10;

	/** 20 even courses of 10 plus 20 odd courses of 9. */
	constexpr int32 OverCapacityWallPieceCount = 20 * 10 + 20 * 9;

	/**
	 * What the ragged wall's worst joint reads AS BUILT, before anything is asked to give.
	 *
	 * Over 1, which is the whole point: this is a wall that is not holding itself up. The
	 * figure is pinned rather than merely compared against 1 so that a load model which
	 * drifted would be visible here rather than silently keeping the test green from the
	 * other side of the line.
	 */
	constexpr double OverCapacityWorstAsBuilt = 1.24833683;

	/** The corbel crosses capacity somewhere near course 32, so 13 courses of this wall stand. */
	constexpr int32 CoursesThatStillStand = 13;
	constexpr double StandingWorstAsBuilt = 0.398900914;

	/** The scenario wall's worst joint — MOMENTS_DESIGN.md's regression anchor, and exact. */
	constexpr double ScenarioWorstAsBuilt = 0.00495042219;

	constexpr int32 ScenarioWallPieceCount = 1220;

	/* The coordinating grid: a brick plus a joint, and half of it is the bond offset. */
	constexpr double CoursePitchCm = 6.5 + 1.0;
	constexpr double BrickPitchCm = 21.5 + 1.0;

	/** Which course a laid box is in, read back off its height. */
	inline int32 CourseOf(const FPieceBox& Box)
	{
		return FMath::RoundToInt32((Box.CentreCm.Z - 6.5 * 0.5) / CoursePitchCm);
	}

	/** How many brick positions in from the nearer end of its own course a box sits. */
	inline int32 PositionFromNearestEnd(const FPieceBox& Box, int32 Course, int32 BricksPerCourse)
	{
		/* Odd courses step in half a cell and are one brick short. */
		const int32 BricksInCourse = (Course % 2 == 0) ? BricksPerCourse : BricksPerCourse - 1;
		const double FirstXCm = (Course % 2 == 0) ? 0.0 : BrickPitchCm * 0.5;

		const int32 FromLeft = FMath::RoundToInt32((Box.CentreCm.X - FirstXCm) / BrickPitchCm);

		return FMath::Min(FromLeft, BricksInCourse - 1 - FromLeft);
	}

	/**
	 * WHICH BRICKS A RAGGED WALL SHEDS WHEN IT SETTLES — THE ORACLE, DERIVED FROM THE BOND
	 * RATHER THAN READ BACK OFF THE SOLVER.
	 *
	 * The collapse front is a staircase, and it is the same staircase a player cuts by hand.
	 * The outermost brick of a course is a corbel and gives; the brick that was resting on it
	 * loses a support and becomes the corbel of the course above; and because running bond
	 * offsets by HALF a cell, the front retreats 11.25 cm per course — one whole brick
	 * position every TWO courses. So course c loses floor(c / 2) bricks from each end.
	 *
	 * Courses 0 and 1 lose nothing: course 0 is grounded, and course 1's end brick still
	 * rests squarely on two bricks of it. The two fronts meet when floor(c / 2) x 2 reaches
	 * the number of bricks in the course, and everything above that has nothing left under it
	 * at all — at ten wide that is course 10, so 55 of 380 pieces survive, in a triangle
	 * 10, 9, 8, 7, 6, 5, 4, 3, 2, 1 courses tall.
	 *
	 * THIS IS THE CONSEQUENCE OF SETTLING AT BUILD TIME, WRITTEN DOWN RATHER THAN DISCOVERED.
	 * It is a lot of wall, and it is correct: those bricks were never being held up, and the
	 * only thing the old wire did for them was decline to ask.
	 */
	inline bool ShouldSurviveSettling(const FPieceBox& Box, int32 BricksPerCourse)
	{
		const int32 Course = CourseOf(Box);

		return PositionFromNearestEnd(Box, Course, BricksPerCourse) >= Course / 2;
	}

	/**
	 * HOW FAR A RELEASED BRICK MUST HAVE MOVED TO COUNT AS HAVING BEEN HANDED TO PHYSICS.
	 *
	 * NOT A FALL DISTANCE, AND DELIBERATELY TINY. A kinematic brick moves EXACTLY zero —
	 * 0.000000 cm, measured, in the two-brick spike and again in this suite's own survivors,
	 * because nothing integrates it at all. So any positive number discriminates, and the
	 * only job of this one is to be far enough above the zero a kinematic body reports and
	 * far enough below anything a real body does in a second of gravity. A tenth of a
	 * millimetre is a hundredth of the mortar joint and a thousandth of a course, and the
	 * smallest movement any of the 325 released bricks makes is reported on every run so the
	 * margin is visible rather than assumed.
	 *
	 * WHY THIS IS THE PER-BRICK CLAIM AND A FALL DISTANCE IS NOT, see the outcome block.
	 */
	constexpr double ReleasedMustMoveCm = 0.01;

	/**
	 * HOW LONG A 380-BRICK WALL TAKES TO FALL DOWN, MEASURED RATHER THAN GUESSED.
	 *
	 * A released brick in this collapse is NOT in free fall, which is the thing that caught
	 * the first version of this test out. The whole wall lets go at the same instant, so
	 * every brick is resting on another that is also falling and the stack compacts from the
	 * bottom rather than dropping: nothing outruns what is beneath it. The released centre of
	 * mass reads Z 146.652 after one second, 24.156 after two, -10.147 after three, and then
	 * -10.171, -10.172, -10.173, -10.172, -10.172 for seconds four through eight — the pile
	 * at rest, to a hundredth of a centimetre, for five straight seconds.
	 *
	 * SO ONE SECOND WAS A COLLAPSE A FIFTH OF THE WAY THROUGH. Three is where it finishes,
	 * and the test then ticks ONE MORE and asserts the difference is nothing, so this number
	 * is a claim the run checks rather than a duration somebody liked. It costs about a third
	 * of a second of wall clock per simulated second at this scale.
	 *
	 * A FIXED COUNT OF SECONDS, NEVER A "HAS IT SETTLED" POLL — the reason is written on
	 * BrickWorldTestSupport::PhysicsStepSeconds, and a poll would turn this failure into a
	 * timeout, which reports far worse than an assertion.
	 */
	constexpr double CollapseSeconds = 3.0;

	/**
	 * How still "the collapse has finished" is, in cm of centre of mass over the last second.
	 *
	 * The rubble moves 0.024 cm between three seconds and four, having travelled 180 cm to
	 * get there; the seconds after that move a thousandth. 1 cm is forty times the former and
	 * a hundred and eightieth of the latter, so it is a floor under "at rest" rather than a
	 * fit to it — and it is still less than the mortar joint one brick could settle into.
	 */
	constexpr double RubbleAtRestCm = 1.0;

	/**
	 * A brick's centre IN WORLD SPACE, which is not its actor location.
	 *
	 * SM_Cube's pivot is a CORNER, so GetActorLocation returns a corner of the brick, and
	 * once a falling brick has rotated that corner has swung somewhere else entirely — the
	 * pivot-to-centre offset turns with the body. FBoxSphereBounds::Origin is the component
	 * transform applied to the mesh's local bounds centre, so it is the centre whatever the
	 * rotation: the axis-aligned box of a rotated symmetric box is still centred on it.
	 *
	 * DISPLACEMENT may be measured from the pivot, because the same material point is being
	 * compared with itself. A CENTRE OF MASS may not, because it is compared against
	 * landmarks in the world.
	 */
	inline FVector BrickCentreCm(const ABrickActor& Brick)
	{
		return Brick.GetMesh()->Bounds.Origin;
	}

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

/**
 * A WALL THAT CANNOT HOLD ITSELF UP COMES DOWN WHEN IT IS BUILT, NOT WHEN SOMEBODY
 * HAPPENS TO CLICK.
 *
 * WHAT IS WRONG TODAY. UDestructionStructureSubsystem::SolveAndPush calls
 * FStructureBinding::SolveLoads, which is documented as NON-DESTRUCTIVE and breaks nothing
 * however overloaded a joint is. The commit doors — RunPieceAction and RunPieceActions —
 * call SolveAndBreak. So a structure that is over capacity the moment it is laid stands
 * there indefinitely, and then sheds the moment the player right-clicks ANY brick anywhere
 * in it: a 40-course ragged wall reads 1.248 as built, and deleting one brick twenty-five
 * bricks and thirty courses away from anything overloaded takes 470 joints out in 27
 * passes. The collapse is real; the attribution is a lie, and the player is told they did
 * something they did not do.
 *
 * THE DECISION IS TO SETTLE AT BUILD TIME. A wall that cannot hold itself up should not
 * stand waiting for a click. That is DESIGN.md §3's own rule — a joint over capacity gives,
 * and a pass that breaks nothing is the last one — applied at the only seam that had been
 * left out of it, and it removes the attribution problem rather than hiding it. The
 * rejected alternative was to make "nothing over capacity" a checked precondition of
 * BuildRunningBond, which would have the producer REFUSE geometry that is merely weak; a
 * weak wall is a real thing a player should be allowed to build and watch fall.
 *
 * THE CONSEQUENCE, STATED RATHER THAN DISCOVERED, AND IT IS LARGE. A tall ragged wall now
 * collapses from its own ends on spawn, and the collapse front is a staircase retreating
 * half a brick per course — so at ten bricks wide the two fronts meet at course 10 and 325
 * of 380 pieces come down, leaving a triangle. See ShouldSurviveSettling for the derivation.
 * That is correct: those bricks were never being held up. The ONLY thing making them look
 * held up was a wire that declined to ask.
 *
 * AND THE SCENARIO WALL MUST BE COMPLETELY UNAFFECTED, WHICH IS THE OTHER HALF AND NOT
 * DECORATION. Without it, "settle at build time" is satisfied by an implementation that
 * knocks down every wall in the game. The wall ADestructionGameGameMode actually lays is
 * FLUSH, its end bricks have two supports each, their centres of mass sit exactly on the
 * area-weighted centroid of those supports, and its worst joint is 0.00495042219 — two
 * hundred times under capacity. Settling it must break nothing at all.
 *
 * WHY BOTH HALVES OF THE OUTCOME ARE ASSERTED. DESIGN.md §4 is explicit that an integration
 * test measures the structure actually moving: a break stamp is a step, and a released flag
 * is a step, so the bricks are ticked for a second and the wall is required to have come
 * down. And "the ends came down" alone passes for a world that dropped through its floor, so
 * the surviving triangle is required not to have moved in the same second.
 *
 * THE OUTCOME IS MEASURED ON THE WHOLE RELEASED SET RATHER THAN BRICK BY BRICK, and the
 * block that does it says why at length: a staircase collapse has no clear air at its foot,
 * so a per-brick fall distance is a prediction about RUBBLE and not about the bond.
 *
 * AND NOTHING IS Stranded. A ragged wall is exactly the shape that could make an unroutable
 * knot, and a wall that came down because the solver declined to divide load round a loop
 * would be a model limitation wearing a collapse's clothes.
 *
 * NEEDS A TICKING WORLD: yes, and it is the point — this is about the wire between the
 * solver and the world, and the solver's own answer is unchanged by any of it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructurePushOverCapacityWallSettlesOnBuildTest,
	"DestructionGame.World.Push.AWallOverCapacityDoesNotWaitForAClick",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructurePushOverCapacityWallSettlesOnBuildTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;
	using namespace StructurePushTestSupport;

	const auto WorstJointOf = [](const FStructure& Structure, int32& OutJoint)
	{
		double Worst = 0.0;
		OutJoint = INDEX_NONE;

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			const double Utilisation = Structure.GetConnectionUtilisation(Joint);

			if (Utilisation > Worst)
			{
				Worst = Utilisation;
				OutJoint = Joint;
			}
		}

		return Worst;
	};

	/*
	 * ================================================================================
	 * FIRST, THE TWO WALLS AS ARITHMETIC, WITH NO WORLD IN THE WAY.
	 * ================================================================================
	 *
	 * Core/Layout and FStructure are world-free, so what each wall carries costs
	 * milliseconds and no actors. Establishing it here is what lets everything below be a
	 * statement about the WIRE rather than about the load model — the solver's answer is
	 * the same before and after this change, and this block is where that is pinned.
	 */
	{
		FBrickLayout Ragged;

		TestTrue(TEXT("fixture: the producer should lay a 40-course ragged wall"),
			RunningBond(RaggedWallSpec(OverCapacityWallCourses, OverCapacityWallBricksPerCourse), Ragged));

		Ragged.Structure.SolveLoads();

		int32 WorstJoint = INDEX_NONE;
		const double Worst = WorstJointOf(Ragged.Structure, WorstJoint);

		AddInfo(FString::Printf(
			TEXT("the ragged %d x %d wall's worst joint as built is joint %d at %.9g of capacity"),
			OverCapacityWallCourses, OverCapacityWallBricksPerCourse, WorstJoint, Worst));

		TestTrue(
			FString::Printf(
				TEXT("fixture: a 40-course ragged wall is over capacity as built at %.9g, it reads %.9g"),
				OverCapacityWorstAsBuilt, Worst),
			FMath::IsNearlyEqual(Worst, OverCapacityWorstAsBuilt, OverCapacityWorstAsBuilt * 1.0e-6));

		TestTrue(
			FString::Printf(TEXT("fixture: and that is over 1, which is what makes this wall the fixture")),
			Worst > 1.0);
	}

	/*
	 * AND A SHORT RAGGED WALL IS NOT OVER CAPACITY, which is what stops "ragged" being read
	 * as "doomed". The corbel eccentricity is the same 5.625 cm on every course; what grows
	 * with height is the force on it, so a wall crosses the line at a height rather than at
	 * a bond pattern. Thirteen courses reads 0.399 and must settle to nothing — this is the
	 * row that keeps every existing ragged fixture in this suite meaningful.
	 */
	{
		FBrickLayout Short;

		TestTrue(TEXT("fixture: the producer should lay a short ragged wall"),
			RunningBond(RaggedWallSpec(CoursesThatStillStand, OverCapacityWallBricksPerCourse), Short));

		Short.Structure.SolveLoads();

		int32 WorstJoint = INDEX_NONE;
		const double Worst = WorstJointOf(Short.Structure, WorstJoint);

		TestTrue(
			FString::Printf(
				TEXT("a %d-course ragged wall stands: its worst joint should read %.9g, it reads %.9g"),
				CoursesThatStillStand, StandingWorstAsBuilt, Worst),
			FMath::IsNearlyEqual(Worst, StandingWorstAsBuilt, StandingWorstAsBuilt * 1.0e-6));

		const int32 Passes = Short.Structure.SolveAndBreak();

		TestEqual(
			FString::Printf(
				TEXT("so settling it must break nothing at all: it took %d breaking passes"), Passes),
			Passes, 0);
	}

	/*
	 * THE SCENARIO WALL, UNTOUCHED. The regression anchor is exact rather than approximate:
	 * a flush end brick's centre of mass sits ON the area-weighted centroid of its two
	 * supports, so its eccentricity is exactly zero and the moment term vanishes bit for bit.
	 */
	{
		FBrickLayout Scenario;

		TestTrue(TEXT("fixture: the producer should lay the game mode's own scenario wall"),
			RunningBond(ScenarioWallSpec(), Scenario));

		TestEqual(
			FString::Printf(TEXT("fixture: the scenario wall should be %d pieces"), ScenarioWallPieceCount),
			Scenario.Structure.NumPieces(), ScenarioWallPieceCount);

		Scenario.Structure.SolveLoads();

		int32 WorstJoint = INDEX_NONE;
		const double Worst = WorstJointOf(Scenario.Structure, WorstJoint);

		AddInfo(FString::Printf(
			TEXT("the flush scenario wall's worst joint is joint %d at %.9g of capacity"),
			WorstJoint, Worst));

		TestTrue(
			FString::Printf(
				TEXT("the scenario wall must read %.9g of capacity, it reads %.9g"),
				ScenarioWorstAsBuilt, Worst),
			FMath::IsNearlyEqual(Worst, ScenarioWorstAsBuilt, ScenarioWorstAsBuilt * 1.0e-6));

		const int32 Passes = Scenario.Structure.SolveAndBreak();

		TestEqual(
			FString::Printf(
				TEXT("settling the scenario wall must break nothing: it ran %d breaking passes"), Passes),
			Passes, 0);

		int32 BrokenJoints = 0;

		for (int32 Joint = 0; Joint < Scenario.Structure.NumConnections(); ++Joint)
		{
			if (Scenario.Structure.GetConnection(Joint).HasGiven())
			{
				++BrokenJoints;
			}
		}

		TestEqual(
			FString::Printf(TEXT("and not one of its joints may give; %d did"), BrokenJoints),
			BrokenJoints, 0);
	}

	/*
	 * ================================================================================
	 * NOW THE WIRE: BUILD THE RAGGED WALL IN A WORLD AND SOLVE-AND-PUSH IT ONCE.
	 * ================================================================================
	 */
	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	const int32 StructureId = TestWorld.Subsystem->BuildRunningBond(
		RaggedWallSpec(OverCapacityWallCourses, OverCapacityWallBricksPerCourse));

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
		FString::Printf(TEXT("fixture: the ragged wall should span %d handles, got %d"),
			OverCapacityWallPieceCount, Binding->NumPieces()),
		Binding->NumPieces(), OverCapacityWallPieceCount);

	if (Binding->NumPieces() != OverCapacityWallPieceCount)
	{
		TestWorld.End();
		return true;
	}

	TArray<ABrickActor*> Bricks;
	TArray<FVector> LaidAt;
	TArray<FVector> LaidCentreCm;

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		ABrickActor* Brick = BrickAt(*this, *Binding, Piece);

		if (Brick == nullptr)
		{
			TestWorld.End();
			return true;
		}

		/*
		 * A BRICK WITH NO MESH HAS NO BOUNDS AND NO BODY, so it can neither be weighed into a
		 * centre of mass nor simulate — and every measurement below would quietly read the
		 * actor's pivot instead. Bail rather than measure something else.
		 */
		if (Brick->GetMesh() == nullptr)
		{
			AddError(FString::Printf(TEXT("fixture: brick %d has no mesh component to measure"), Piece));
			TestWorld.End();
			return true;
		}

		Bricks.Add(Brick);
		LaidAt.Add(Brick->GetActorLocation());
		LaidCentreCm.Add(BrickCentreCm(*Brick));
	}

	/*
	 * THE ORACLE, BUILT BEFORE THE PUSH AND FROM THE BOND RATHER THAN FROM THE ANSWER.
	 * Each piece's fate is decided by where it sits in its own course, which is read off
	 * the box the producer laid.
	 */
	TArray<bool> ShouldSurvive;
	TArray<FPieceBox> Boxes;

	int32 ExpectedSurvivors = 0;

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		const FPieceBox& Box = Binding->GetBinding(Piece).Box;

		Boxes.Add(Box);
		ShouldSurvive.Add(ShouldSurviveSettling(Box, OverCapacityWallBricksPerCourse));

		if (ShouldSurvive.Last())
		{
			++ExpectedSurvivors;
		}
	}

	const int32 ExpectedReleased = Binding->NumPieces() - ExpectedSurvivors;

	AddInfo(FString::Printf(
		TEXT("the bond predicts %d of %d pieces survive settling, so %d come down"),
		ExpectedSurvivors, Binding->NumPieces(), ExpectedReleased));

	/*
	 * THE ONLY CALL THIS TEST MAKES ON THE WALL. Nothing is removed, nothing is clicked,
	 * nobody chooses a menu row: this is the wire the game mode runs on BeginPlay.
	 */
	const int32 Released = TestWorld.Subsystem->SolveAndPush(StructureId);

	TestEqual(
		FString::Printf(
			TEXT("building a wall that cannot hold itself up must settle it there and then: %d pieces should have been released, SolveAndPush released %d"),
			ExpectedReleased, Released),
		Released, ExpectedReleased);

	/*
	 * AND THE MECHANISM BEHIND IT: JOINTS GAVE UNDER LOAD. GetBreakPass is the quantity
	 * that survives the breaking — a given joint carries nothing and reads zero
	 * utilisation, which is also what a joint nobody ever loaded reads. A stamp means a
	 * joint failed under load in a cascade pass, which a plain solve can never produce.
	 */
	int32 BrokenJoints = 0;
	int32 HighestPass = 0;

	for (int32 Joint = 0; Joint < Binding->GetStructure().NumConnections(); ++Joint)
	{
		const int32 Pass = Binding->GetStructure().GetBreakPass(Joint);

		if (Pass != INDEX_NONE)
		{
			++BrokenJoints;
			HighestPass = FMath::Max(HighestPass, Pass);
		}
	}

	AddInfo(FString::Printf(
		TEXT("settling the ragged wall broke %d of %d joints, over %d passes"),
		BrokenJoints, Binding->GetStructure().NumConnections(), HighestPass));

	TestTrue(
		FString::Printf(
			TEXT("joints must have GIVEN rather than merely been computed to be over capacity; %d carry a break pass"),
			BrokenJoints),
		BrokenJoints > 0);

	TestTrue(
		FString::Printf(
			TEXT("and it must have CASCADED rather than broken one sweep's worth: the highest pass is %d"),
			HighestPass),
		HighestPass > 1);

	/*
	 * WHICH BRICKS, NOT HOW MANY. A count is satisfied by releasing any 325 of them, and
	 * the defect that matters — a wall that came apart in the wrong place — leaves every
	 * count agreeing.
	 *
	 * COUNTED IN FULL AND REPORTED IN PART. Three hundred and twenty-five pieces can each
	 * be wrong in three ways, and a failure that writes a thousand lines is a failure
	 * nobody reads. The first few are what a maintainer works from; the totals are the
	 * assertions, so nothing is hidden by the cap.
	 *
	 * THE CAP WAS 12 AND IT COST A DIAGNOSIS. A run in which 46 bricks failed one row printed
	 * 12 of them, and the other 34 had to be reconstructed by arithmetic before anyone could
	 * see that the failing set was every released brick of courses 2-9 plus part of course 10
	 * — which was the whole answer. 48 is four courses' worth of a ten-wide wall, so a
	 * failure that follows the collapse front shows its shape directly.
	 */
	constexpr int32 MaxReportedPieces = 48;

	int32 WrongFate = 0;
	int32 WrongBody = 0;
	int32 StrandedPieces = 0;

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		const EPieceSupport Support = Binding->GetStructure().GetPieceSupport(Piece);

		if (Binding->IsReleased(Piece) == ShouldSurvive[Piece])
		{
			++WrongFate;

			if (WrongFate <= MaxReportedPieces)
			{
				AddError(FString::Printf(
					TEXT("piece %d (course %d, %d in from the nearer end, at X %.3f Z %.3f) should%s have been released; the solver says %s and IsReleased is %s"),
					Piece,
					CourseOf(Boxes[Piece]),
					PositionFromNearestEnd(
						Boxes[Piece], CourseOf(Boxes[Piece]), OverCapacityWallBricksPerCourse),
					Boxes[Piece].CentreCm.X, Boxes[Piece].CentreCm.Z,
					ShouldSurvive[Piece] ? TEXT(" not") : TEXT(""),
					SupportName(Support),
					Binding->IsReleased(Piece) ? TEXT("true") : TEXT("false")));
			}
		}

		/*
		 * THIS IS A COLLAPSE, NOT A SOLVER STALL. Stranded means the solver declined to
		 * divide load round a knot, and a wall calibrated on one would come down looking
		 * exactly the same while measuring a limitation of the model.
		 */
		if (Support == EPieceSupport::Stranded)
		{
			++StrandedPieces;

			if (StrandedPieces <= MaxReportedPieces)
			{
				AddError(FString::Printf(
					TEXT("piece %d is Stranded: that would make this a solver limitation rather than a collapse"),
					Piece));
			}
		}

		/* And the flag reached the brick: ABrickActor keeps no second copy of the answer. */
		const bool bSimulating =
			Bricks[Piece]->GetMesh() != nullptr && Bricks[Piece]->GetMesh()->IsSimulatingPhysics();

		if (bSimulating == ShouldSurvive[Piece])
		{
			++WrongBody;

			if (WrongBody <= MaxReportedPieces)
			{
				AddError(FString::Printf(
					TEXT("brick %d should%s be simulating physics once the wall has settled; it is %s"),
					Piece, ShouldSurvive[Piece] ? TEXT(" not") : TEXT(""),
					bSimulating ? TEXT("simulating") : TEXT("kinematic")));
			}
		}
	}

	TestEqual(
		FString::Printf(
			TEXT("every piece must meet the fate the bond predicts for it; %d of %d did not"),
			WrongFate, Binding->NumPieces()),
		WrongFate, 0);

	TestEqual(
		FString::Printf(TEXT("and no piece may be Stranded; %d was"), StrandedPieces),
		StrandedPieces, 0);

	TestEqual(
		FString::Printf(
			TEXT("and every brick's body must agree with its binding; %d of %d did not"),
			WrongBody, Binding->NumPieces()),
		WrongBody, 0);

	/*
	 * ================================================================================
	 * THE OUTCOME. REAL GRAVITY ON A FIXED STEP, AND THE WALL ENDS UP ON THE FLOOR.
	 * ================================================================================
	 *
	 * WHY THIS IS NOT "EVERY RELEASED BRICK FELL A COURSE", WHICH IS WHAT IT ASSERTED FIRST
	 * AND WHICH IS FALSE OF THIS GEOMETRY. BrickWorldTestSupport::FallenAtLeastCm is derived
	 * from a fixture in which released bricks have CLEAR AIR beneath them — the narrow-waist
	 * wall, where taking the waist out leaves 8.5 cm of nothing under the orphans. THE FOOT
	 * OF A STAIRCASE COLLAPSE HAS NO SUCH AIR. The front retreats half a brick per course, so
	 * the lowest released brick — course 2, position 0, spanning X -10.75..10.75 — still has
	 * 10.25 of its 21.5 cm resting on the course 1 brick at X 0.5..22.0, which is STILL
	 * STANDING. Its underside is at Z 15.0 and that brick's top at Z 14.0, so it drops its 1
	 * cm mortar gap and then tips about the support edge with its centre of mass 0.5 cm
	 * outboard: 1.310 cm measured, 1 cm of settle plus 0.31 cm of tip, and the arithmetic and
	 * the measurement agree exactly. Ticking for longer changes nothing — the wall is holding
	 * it. That is a correct outcome of this collapse, and 46 of the 325 are in it.
	 *
	 * AND THE TWO CLASSES CANNOT BE TOLD APART FROM THE BOND, WHICH IS WHY THE OUTCOME CLAIM
	 * IS AN AGGREGATE. Both candidate oracles were worked out and both are wrong: "has a
	 * surviving brick directly beneath it" names 18 bricks, two per course for courses 2-10,
	 * and misses 28 of the 46; its transitive closure — resting on anything that is itself
	 * resting on something standing — names all 325, because every column of this wall
	 * eventually reaches the two complete bottom courses. The real boundary is where the
	 * rubble pile stops jamming and starts falling clear, which is a fact about a pile
	 * mid-collapse rather than about the bond, and an oracle fitted to it would be a number
	 * copied off one run. So the released set is asserted BRICK BY BRICK on the mechanism
	 * reaching the world, and AS A WHOLE on the collapse.
	 *
	 * THE AGGREGATE IS THE CENTRE OF MASS, AND ITS LANDMARK IS THE WALL THAT SURVIVED. The
	 * released set's centre of mass starts at Z 170.212 — asserted below, not assumed — and
	 * the highest thing still standing is the top face of the single course 9 brick at the
	 * apex of the triangle, Z 74, which is READ OFF the survivors rather than written down.
	 * For the released mass to end up UNDER that it has to come down the better part of a
	 * metre on average, which no settle (1 cm), no tip (a few cm) and no shedding of a
	 * handful of bricks can produce, and which reads exactly zero if nothing is released at
	 * all. That is DESIGN.md §4's "a measure of the whole structure actually moving/falling"
	 * taken literally: THE RUBBLE ENDS UP LOWER THAN THE WALL IT FELL OFF.
	 *
	 * AND ABOVE THE FLOOR, which is the other side of the same claim and not decoration. A
	 * centre of mass that dropped because bricks tunnelled out through the world would
	 * satisfy the first half perfectly. Both comparisons are strict, so a NaN anywhere in the
	 * set fails both rather than passing one.
	 *
	 * PLUS A BREADTH ROW, because a mass-weighted average is exactly the sort of number a few
	 * heavy outliers can carry. Requiring a MAJORITY of the released bricks to have fallen
	 * more than a whole course says the collapse is wide as well as deep, and a course pitch
	 * is the same landmark BrickWorldTestSupport::FallenAtLeastCm is derived from — seven and
	 * a half times what a brick settling into its own mortar joint can move.
	 */
	TestWorld.TickSeconds(CollapseSeconds);

	/*
	 * The released set's mass-weighted centre of mass, from wherever the caller says its
	 * pieces are — so the laid reading and the two later ones are the same arithmetic and
	 * cannot drift apart.
	 *
	 * MASS FROM THE LAID BOX, DERIVED HERE. Every brick of a ragged wall is a full brick, so
	 * the weighting cannot change today's answer — and it is written as a centre of mass
	 * anyway, because a bond that ever laid two sizes would otherwise turn this into a mean
	 * of positions that had quietly stopped being one.
	 *
	 * AND THE DIVISION FAILS CLOSED: no released mass at all makes it 0/0, which is a NaN,
	 * and a NaN fails every strict comparison below rather than reading as a plausible
	 * height. There is no FMath::Max to swallow it and no default to substitute.
	 */
	double ReleasedMassKg = 0.0;

	const auto ReleasedCentreOfMassZCm = [&](auto CentreOfPiece)
	{
		double MassKg = 0.0;
		double MomentKgCm = 0.0;

		for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
		{
			if (ShouldSurvive[Piece])
			{
				continue;
			}

			const double PieceMassKg = MassKgFromBox(Boxes[Piece], ClayBrick.DensityGramsPerCubicCm);

			MassKg += PieceMassKg;
			MomentKgCm += PieceMassKg * CentreOfPiece(Piece).Z;
		}

		ReleasedMassKg = MassKg;

		return MomentKgCm / MassKg;
	};

	const double LaidCentreOfMassZCm =
		ReleasedCentreOfMassZCm([&](int32 Piece) { return LaidCentreCm[Piece]; });

	const double FallingCentreOfMassZCm =
		ReleasedCentreOfMassZCm([&](int32 Piece) { return BrickCentreCm(*Bricks[Piece]); });

	/* One more second, so the row below can say the fall had ALREADY finished by the last. */
	TestWorld.TickSeconds(1.0);

	const double RestingCentreOfMassZCm =
		ReleasedCentreOfMassZCm([&](int32 Piece) { return BrickCentreCm(*Bricks[Piece]); });

	int32 DidNotMove = 0;
	int32 Drifted = 0;
	int32 FellAtLeastACourse = 0;

	double SmallestReleasedMoveCm = TNumericLimits<double>::Max();
	double SurvivingTopZCm = -TNumericLimits<double>::Max();

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		const FVector NowAt = Bricks[Piece]->GetActorLocation();
		const double FellCm = LaidAt[Piece].Z - NowAt.Z;
		const double MovedCm = FVector::Dist(NowAt, LaidAt[Piece]);

		if (ShouldSurvive[Piece])
		{
			/*
			 * THE LANDMARK IS MEASURED OFF THE STANDING WALL RATHER THAN WRITTEN DOWN, and in
			 * the same world space as the centre of mass it will be compared against. That the
			 * survivors are still where they were laid is the row immediately below, so this is
			 * a reading of the triangle rather than a second opinion about it.
			 */
			const FBoxSphereBounds& BoundsCm = Bricks[Piece]->GetMesh()->Bounds;

			SurvivingTopZCm = FMath::Max(SurvivingTopZCm, BoundsCm.Origin.Z + BoundsCm.BoxExtent.Z);

			if (MovedCm >= DriftToleranceCm)
			{
				++Drifted;

				if (Drifted <= MaxReportedPieces)
				{
					AddError(FString::Printf(
						TEXT("brick %d (course %d) is still held up and must not move; it drifted %.6f cm"),
						Piece, CourseOf(Boxes[Piece]), MovedCm));
				}
			}

			continue;
		}

		if (FellCm > CoursePitchCm)
		{
			++FellAtLeastACourse;
		}

		/*
		 * NOT A CLAIM ABOUT DISTANCE, AND THAT IS THE POINT. A released brick has been handed
		 * to physics, so seconds of gravity have to have done SOMETHING to it — while a
		 * kinematic one reports exactly 0.000000, because nothing integrates it at all. That
		 * is what makes this discriminating without asserting a fall the wall is preventing.
		 * Written as a negated > so a NaN position counts as not having moved rather than
		 * sailing through a <= comparison every NaN passes.
		 */
		if (!(MovedCm > ReleasedMustMoveCm))
		{
			++DidNotMove;

			if (DidNotMove <= MaxReportedPieces)
			{
				AddError(FString::Printf(
					TEXT("released brick %d (course %d, %d in from the nearer end) was handed to physics and must have moved; it moved %.6f cm"),
					Piece,
					CourseOf(Boxes[Piece]),
					PositionFromNearestEnd(
						Boxes[Piece], CourseOf(Boxes[Piece]), OverCapacityWallBricksPerCourse),
					MovedCm));
			}
		}

		SmallestReleasedMoveCm = FMath::Min(SmallestReleasedMoveCm, MovedCm);
	}

	TestEqual(
		FString::Printf(TEXT("no brick the bond says is still held up may have moved; %d did"), Drifted),
		Drifted, 0);

	AddInfo(FString::Printf(
		TEXT("the smallest movement any of the %d released bricks made is %.6f cm, against the %g cm required and the exactly 0.000000 a kinematic brick reports"),
		ExpectedReleased, SmallestReleasedMoveCm, ReleasedMustMoveCm));

	TestEqual(
		FString::Printf(
			TEXT("every brick the settle released must have moved under gravity; %d of %d did not"),
			DidNotMove, ExpectedReleased),
		DidNotMove, 0);

	AddInfo(FString::Printf(
		TEXT("the released %.1f kg of wall had its centre of mass at Z %.3f cm as laid, Z %.3f after %g s and Z %.3f a second after that, a drop of %.3f cm; the standing triangle's top is Z %.3f and the floor is Z %g"),
		ReleasedMassKg, LaidCentreOfMassZCm, FallingCentreOfMassZCm, CollapseSeconds,
		RestingCentreOfMassZCm, LaidCentreOfMassZCm - RestingCentreOfMassZCm,
		SurvivingTopZCm, FloorTopZCm));

	/*
	 * THE FIXTURE PRECONDITION THAT STOPS THE OUTCOME CLAIM BEING FREE. If the released mass
	 * started BELOW the top of the triangle, "it ended up below it" would be true of a wall
	 * that never moved at all.
	 */
	TestTrue(
		FString::Printf(
			TEXT("fixture: the released mass must start ABOVE the wall that survives, at Z %.3f against a standing top of Z %.3f"),
			LaidCentreOfMassZCm, SurvivingTopZCm),
		LaidCentreOfMassZCm > SurvivingTopZCm);

	TestTrue(
		FString::Printf(
			TEXT("the wall must have COME DOWN: the released mass's centre of mass should end below the top of the triangle still standing at Z %.3f, it is at Z %.3f"),
			SurvivingTopZCm, RestingCentreOfMassZCm),
		RestingCentreOfMassZCm < SurvivingTopZCm);

	TestTrue(
		FString::Printf(
			TEXT("and it must have LANDED rather than left the world: the released mass's centre of mass should stay above the floor at Z %g, it is at Z %.3f"),
			FloorTopZCm, RestingCentreOfMassZCm),
		RestingCentreOfMassZCm > FloorTopZCm);

	/*
	 * AND THE COLLAPSE WAS OVER BEFORE THE LAST SECOND OF IT, which is what makes the tick
	 * length a measurement rather than a guess: the rubble moved 0.024 cm of centre of mass
	 * between three seconds and four, against the 180 cm it had travelled to get there.
	 */
	TestTrue(
		FString::Printf(
			TEXT("the collapse must have FINISHED inside %g s: the released centre of mass moved %.3f cm in the second after that, and may move no more than %g"),
			CollapseSeconds, FMath::Abs(RestingCentreOfMassZCm - FallingCentreOfMassZCm),
			RubbleAtRestCm),
		FMath::Abs(RestingCentreOfMassZCm - FallingCentreOfMassZCm) < RubbleAtRestCm);

	/*
	 * THE BREADTH ROW. A majority, so no small number of bricks falling a long way can carry
	 * it, and a whole course pitch, which is seven and a half times the mortar joint a brick
	 * can settle into and the same landmark the sister test's fall threshold is derived from.
	 */
	AddInfo(FString::Printf(
		TEXT("%d of the %d released bricks fell more than the %g cm course pitch"),
		FellAtLeastACourse, ExpectedReleased, CoursePitchCm));

	TestTrue(
		FString::Printf(
			TEXT("the collapse must be WIDE as well as deep: more than half of the %d released bricks should have fallen a whole course, %d did"),
			ExpectedReleased, FellAtLeastACourse),
		FellAtLeastACourse * 2 > ExpectedReleased);

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
