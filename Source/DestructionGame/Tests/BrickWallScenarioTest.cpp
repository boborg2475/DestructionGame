// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "DestructionGameGameMode.h"
#include "HAL/PlatformTime.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges
 * many files into one. The world harness is NOT redeclared here: it lives in
 * Tests/BrickWorldTestSupport.h and is shared with four other files.
 */
namespace BrickWallScenarioTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/**
	 * THE WALL THE GAME MODE BUILDS ON PLAY: 30 bricks across, 40 courses.
	 *
	 * That is thirty to sixty times anything else in this suite, and the size is deliberate
	 * rather than incidental — it is what the player asked to see, and it is the only thing
	 * in the project that says whether the solver's known O(pieces x connections) shape is
	 * affordable at scenario scale. CURRENT_STATE.md records the fix (an adjacency index
	 * built once per solve instead of rescanning the connection array once per piece) as
	 * designed and deliberately unbuilt, precisely because nothing measured slow. This is the
	 * measurement.
	 *
	 * FLUSH RATHER THAN RAGGED, so the wall the player sees has square ends and so the
	 * scenario exercises the MIXED-SIZE case: half bats at alternating course ends mean two
	 * piece sizes and two masses, and a spawner that handed every brick the same one would
	 * otherwise pass at this scale exactly as it would at seven pieces.
	 */
	inline FRunningBondSpec ScenarioWallSpec()
	{
		FRunningBondSpec Spec;
		Spec.BrickSizeCm = FVector(21.5, 10.25, 6.5);
		Spec.JointThicknessCm = 1.0;
		Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
		Spec.CoursesHigh = 40;
		Spec.BricksPerCourse = 30;
		Spec.End = EWallEnd::Flush;
		Spec.Strength = GeneralPurposeMortar;
		return Spec;
	}

	/**
	 * WHAT THAT SPEC MUST COME OUT AS, DERIVED BY HAND RATHER THAN READ BACK OFF THE PRODUCER.
	 *
	 * PIECES. Even courses (0, 2, ... 38) are 30 full bricks: 20 x 30 = 600. Odd courses are
	 * offset half a cell, so a flush wall fills the half cell at each end with a half bat —
	 * 1 + 29 + 1 = 31 pieces: 20 x 31 = 620. Total 1,220.
	 *
	 * JOINTS. Head joints run along a course, one fewer than its pieces: 20 x 29 + 20 x 30 =
	 * 1,180. Bed joints run between adjacent courses, and BOTH directions come to 60 per
	 * transition. Odd on even: the left half bat spans [-10.75, -0.5] and lands on full brick
	 * 0 alone (1); each of the 29 full bricks spans two below it (58); the right half bat
	 * lands on the last full brick alone (1). Even on odd: brick 0 catches the left half bat
	 * and full brick 0, bricks 1..28 catch two full bricks each, brick 29 catches the last
	 * full brick and the right half bat — 30 x 2 = 60. Thirty-nine transitions, so 2,340.
	 * Total 3,520.
	 *
	 * Every one of those overlaps is 10.25 cm, so every bed joint is 105.0625 cm2 and every
	 * head joint 66.625 cm2, which is what Core/Layout's own tests already pin.
	 */
	constexpr int32 ScenarioPieceCount = 1220;
	constexpr int32 ScenarioJointCount = 3520;

	/**
	 * The wall's own extent, cm, from the same grid.
	 *
	 * Along X a course runs from -BrickSizeCm.X / 2 = -10.75 to 29 x 22.5 + 10.75 = 663.25,
	 * so 6.74 m long. Up Z the top course's centre is 3.25 + 39 x 7.5 = 295.75 and its top is
	 * 299, so just under 3 m tall. Asserted because a wall of the right piece COUNT laid in
	 * the wrong shape — every brick at the origin, say — would satisfy every other row here.
	 */
	constexpr double ScenarioWallMinXCm = -10.75;
	constexpr double ScenarioWallMaxXCm = 663.25;
	constexpr double ScenarioWallTopZCm = 299.0;

	const TCHAR* ScenarioSupportName(EPieceSupport Support)
	{
		switch (Support)
		{
		case EPieceSupport::Grounded:  return TEXT("Grounded");
		case EPieceSupport::Supported: return TEXT("Supported");
		case EPieceSupport::Stranded:  return TEXT("Stranded");
		default:                       return TEXT("Falling");
		}
	}

	/** So a wall that is wrong everywhere does not print 1,220 failures. */
	constexpr int32 ScenarioMaxReportedFailures = 8;

	/**
	 * EVERY PIECE OF A WALL NOBODY HAS TOUCHED IS HELD UP, AND NOTHING HAS BEEN RELEASED.
	 *
	 * "It is standing" is the outcome assertion for a structure, and it is two separate
	 * claims that fail differently. The solver saying every piece reaches the ground is the
	 * MECHANISM; nothing having been handed to physics is the OUTCOME, and the two can
	 * disagree — CURRENT_STATE.md records the polarity inversion where a structure nobody had
	 * solved released entire, foundation included, because an absent answer reads as Falling.
	 *
	 * NEITHER Stranded NOR Falling IS ACCEPTABLE HERE, and telling them apart matters: a
	 * Stranded piece means the solver hit a knot it cannot route, which at this scale would
	 * be a solver limitation wearing the clothes of a collapse. The message names which.
	 */
	inline void CheckWallIsStanding(
		FAutomationTestBase& Test,
		FStructureBinding& Binding,
		const TCHAR* When)
	{
		int32 NotHeldUp = 0;
		int32 Released = 0;

		for (int32 Piece = 0; Piece < Binding.NumPieces(); ++Piece)
		{
			const EPieceSupport Support = Binding.GetStructure().GetPieceSupport(Piece);

			if (Support != EPieceSupport::Grounded && Support != EPieceSupport::Supported)
			{
				++NotHeldUp;

				if (NotHeldUp <= ScenarioMaxReportedFailures)
				{
					Test.AddError(FString::Printf(
						TEXT("%s: piece %d of a wall under nothing but its own weight reads %s"),
						When, Piece, ScenarioSupportName(Support)));
				}
			}

			if (Binding.IsReleased(Piece))
			{
				++Released;
			}
		}

		Test.TestEqual(
			FString::Printf(TEXT("%s: no piece of an untouched wall should have lost its support; %d did"),
				When, NotHeldUp),
			NotHeldUp, 0);

		Test.TestEqual(
			FString::Printf(TEXT("%s: no piece of an untouched wall should have been handed to physics; %d was"),
				When, Released),
			Released, 0);
	}
}

/**
 * A 30-WIDE, 40-COURSE WALL LAYS, SPAWNS, SOLVES AND STANDS — AND HOW LONG EACH OF THOSE
 * TAKES IS REPORTED ON EVERY RUN.
 *
 * WHY MEASURE AT ALL, AND WHY HERE. Every structure this suite has built is between two and
 * twenty pieces. The scenario is 1,220 pieces and 3,520 joints, and the solver's documented
 * shape is O(pieces x connections) with a graph walk per piece on top: building the support
 * lists rescans the whole connection array once per piece, and the knot check is a fresh walk
 * with a fresh visited buffer per supported ungrounded piece. Every click runs a complete
 * SolveAndPush. So "is this affordable" is a real question with a real answer, and a measured
 * number is worth more than an opinion — CURRENT_STATE.md already carries the designed fix
 * and says it was left undone because nothing measured slow. The timings are reported rather
 * than asserted: a wall-clock budget in an automation test is a flake, and the size of the
 * wall is a decision for whoever reads the number, not for this file.
 *
 * THIS TEST IS GREEN ON ARRIVAL, AND SAYS SO. It exercises Core/Layout and
 * UDestructionStructureSubsystem::BuildRunningBond, both of which are built and covered — at
 * seven and twenty pieces. What is new is the SCALE, and the claim is a characterisation: a
 * wall this size lays with the piece and joint counts the coordinating grid predicts, spawns
 * one live brick per piece, solves with every piece held up and no knot anywhere, and sits at
 * a utilisation that is nowhere near breaking. It is a scale regression net and a measuring
 * instrument, not a driver of new behaviour, and the report says which.
 *
 * NEEDS A TICKING WORLD: it needs a WORLD, to spawn 1,220 actors into. It never ticks one;
 * nothing here is about anything falling, and ticking a wall of this size would be the most
 * expensive thing in the suite by a wide margin for no extra claim.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBrickWallAtScenarioScaleTest,
	"DestructionGame.World.Scenario.WallAtScenarioScale",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBrickWallAtScenarioScaleTest::RunTest(const FString& Parameters)
{
	using namespace BrickWallScenarioTestSupport;
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;

	const FRunningBondSpec Spec = ScenarioWallSpec();

	/*
	 * THE GRAPH ALONE FIRST, WITH NO WORLD IN THE WAY. RunningBond is the world-free half —
	 * boxes, masses, pairs and joints — so timing it separately is what separates "laying the
	 * wall is slow" from "spawning 1,220 actors is slow", which have entirely different fixes.
	 */
	FBrickLayout Reference;

	const double LayoutStart = FPlatformTime::Seconds();
	const bool bLaid = RunningBond(Spec, Reference);
	const double LayoutSeconds = FPlatformTime::Seconds() - LayoutStart;

	TestTrue(TEXT("RunningBond should lay a 30-wide, 40-course wall"), bLaid);

	AddInfo(FString::Printf(
		TEXT("MEASURED: RunningBond laid %d pieces and %d joints in %.1f ms"),
		Reference.Structure.NumPieces(), Reference.Structure.NumConnections(),
		LayoutSeconds * 1000.0));

	TestEqual(
		FString::Printf(TEXT("a flush 30 x 40 wall should be %d pieces, got %d"),
			ScenarioPieceCount, Reference.Structure.NumPieces()),
		Reference.Structure.NumPieces(), ScenarioPieceCount);

	TestEqual(
		FString::Printf(TEXT("a flush 30 x 40 wall should carry %d joints, got %d"),
			ScenarioJointCount, Reference.Structure.NumConnections()),
		Reference.Structure.NumConnections(), ScenarioJointCount);

	/*
	 * AND IT IS THE RIGHT SHAPE, NOT MERELY THE RIGHT COUNT. A producer that laid every brick
	 * at the origin would satisfy both counts above and nothing else in this file.
	 */
	{
		FBox WallBoundsCm(ForceInit);

		for (const FPieceBox& Box : Reference.Boxes)
		{
			WallBoundsCm += FBox(Box.CentreCm - Box.ExtentCm, Box.CentreCm + Box.ExtentCm);
		}

		AddInfo(FString::Printf(
			TEXT("the scenario wall spans X %g..%g, Y %g..%g, Z %g..%g cm"),
			WallBoundsCm.Min.X, WallBoundsCm.Max.X,
			WallBoundsCm.Min.Y, WallBoundsCm.Max.Y,
			WallBoundsCm.Min.Z, WallBoundsCm.Max.Z));

		TestTrue(
			FString::Printf(TEXT("the wall should run X %g..%g, it runs %g..%g"),
				ScenarioWallMinXCm, ScenarioWallMaxXCm, WallBoundsCm.Min.X, WallBoundsCm.Max.X),
			FMath::IsNearlyEqual(WallBoundsCm.Min.X, ScenarioWallMinXCm, BoundsToleranceCm)
			&& FMath::IsNearlyEqual(WallBoundsCm.Max.X, ScenarioWallMaxXCm, BoundsToleranceCm));

		TestTrue(
			FString::Printf(TEXT("40 courses should reach Z %g, they reach %g"),
				ScenarioWallTopZCm, WallBoundsCm.Max.Z),
			FMath::IsNearlyEqual(WallBoundsCm.Max.Z, ScenarioWallTopZCm, BoundsToleranceCm));
	}

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	/*
	 * NOW THE WHOLE BUILD, WORLD AND ALL: lay it again, spawn one ABrickActor per box, adopt
	 * the lot. This is the call the scenario makes, so it is the number that answers "how long
	 * does pressing Play take".
	 */
	const double BuildStart = FPlatformTime::Seconds();
	const int32 StructureId = TestWorld.Subsystem->BuildRunningBond(Spec);
	const double BuildSeconds = FPlatformTime::Seconds() - BuildStart;

	AddInfo(FString::Printf(
		TEXT("MEASURED: BuildRunningBond (lay + spawn %d actors + adopt) took %.1f ms"),
		ScenarioPieceCount, BuildSeconds * 1000.0));

	FStructureBinding* Binding = TestWorld.Subsystem->Find(StructureId);

	TestNotNull(
		*FString::Printf(TEXT("BuildRunningBond returned %d and Find should hand back its binding"),
			StructureId),
		Binding);

	if (Binding == nullptr)
	{
		TestWorld.End();
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("the built wall should carry %d pieces, got %d"),
			ScenarioPieceCount, Binding->NumPieces()),
		Binding->NumPieces(), ScenarioPieceCount);

	/*
	 * ONE LIVE BRICK PER PIECE. AdoptLayout refuses an actor list that is not one per piece,
	 * so a count would be free; what is not free is every one of them being a real, valid
	 * ABrickActor, which is what a spawner that quietly failed on brick 900 would not be.
	 */
	{
		int32 MissingBricks = 0;

		for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
		{
			const ABrickActor* Brick = Cast<ABrickActor>(Binding->GetActor(Piece));

			if (!IsValid(Brick))
			{
				++MissingBricks;

				if (MissingBricks <= ScenarioMaxReportedFailures)
				{
					AddError(FString::Printf(TEXT("piece %d has no live ABrickActor standing for it"), Piece));
				}
			}
		}

		TestEqual(
			FString::Printf(TEXT("every one of the %d pieces should have a live brick; %d do not"),
				ScenarioPieceCount, MissingBricks),
			MissingBricks, 0);
	}

	/*
	 * THE SOLVE, TIMED ON ITS OWN. This is the figure the O(pieces x connections) note is
	 * about, and it is what every click pays: SolveAndPush runs a complete solve per call.
	 */
	const double SolveStart = FPlatformTime::Seconds();
	Binding->SolveLoads();
	const double SolveSeconds = FPlatformTime::Seconds() - SolveStart;

	AddInfo(FString::Printf(
		TEXT("MEASURED: SolveLoads over %d pieces and %d joints took %.1f ms"),
		ScenarioPieceCount, ScenarioJointCount, SolveSeconds * 1000.0));

	CheckWallIsStanding(*this, *Binding, TEXT("as built and solved"));

	/*
	 * AND NOTHING IS ANYWHERE NEAR BREAKING. Masonry is enormously overbuilt in compression —
	 * CURRENT_STATE.md puts a four-course wall at about 0.0004 of capacity — so forty courses
	 * should still be orders below 1. The number is reported because it is the interesting
	 * one: it says how much taller a wall would have to be before gravity alone did anything.
	 */
	{
		double WorstUtilisation = 0.0;
		int32 WorstJoint = INDEX_NONE;

		for (int32 Joint = 0; Joint < Binding->GetStructure().NumConnections(); ++Joint)
		{
			const double Utilisation = Binding->GetStructure().GetConnectionUtilisation(Joint);

			if (Utilisation > WorstUtilisation)
			{
				WorstUtilisation = Utilisation;
				WorstJoint = Joint;
			}
		}

		AddInfo(FString::Printf(
			TEXT("the worst-loaded joint of the scenario wall is joint %d at %.6g of its capacity"),
			WorstJoint, WorstUtilisation));

		TestTrue(
			FString::Printf(
				TEXT("a wall standing under its own weight should have no joint over capacity; joint %d is at %.6g"),
				WorstJoint, WorstUtilisation),
			WorstUtilisation < 1.0);
	}

	/*
	 * THE PUSH, TWICE. The first is what a click costs; the second is what a click that
	 * changes nothing costs, which is the common case and must not be cheaper by accident —
	 * SolveAndPush deliberately re-solves rather than caching, so the two should be alike.
	 */
	const double FirstPushStart = FPlatformTime::Seconds();
	const int32 FirstReleased = TestWorld.Subsystem->SolveAndPush(StructureId);
	const double FirstPushSeconds = FPlatformTime::Seconds() - FirstPushStart;

	const double SecondPushStart = FPlatformTime::Seconds();
	const int32 SecondReleased = TestWorld.Subsystem->SolveAndPush(StructureId);
	const double SecondPushSeconds = FPlatformTime::Seconds() - SecondPushStart;

	AddInfo(FString::Printf(
		TEXT("MEASURED: SolveAndPush took %.1f ms, and again %.1f ms — this is the per-click cost"),
		FirstPushSeconds * 1000.0, SecondPushSeconds * 1000.0));

	TestEqual(
		FString::Printf(TEXT("pushing a standing wall should release nothing, it released %d"), FirstReleased),
		FirstReleased, 0);

	TestEqual(
		FString::Printf(TEXT("pushing it again should release nothing, it released %d"), SecondReleased),
		SecondReleased, 0);

	CheckWallIsStanding(*this, *Binding, TEXT("after two pushes"));

	AddInfo(FString::Printf(
		TEXT("MEASURED TOTAL: lay %.1f ms + build %.1f ms + solve %.1f ms + push %.1f ms"),
		LayoutSeconds * 1000.0, BuildSeconds * 1000.0,
		SolveSeconds * 1000.0, FirstPushSeconds * 1000.0));

	TestWorld.End();

	return true;
}

/**
 * PRESSING PLAY BUILDS THE WALL: THE GAME MODE PUTS A 30-WIDE, 40-COURSE STRUCTURE INTO THE
 * WORLD WHEN PLAY BEGINS, ONE LIVE BRICK PER PIECE, AND IT IS STANDING BEFORE ANYTHING IS
 * CLICKED.
 *
 * THE LAST PIECE OF THE MVP, AND THE ONLY ONE A PLAYER MEETS FIRST. Everything under it is
 * built and green: the producer lays the wall, the subsystem spawns and adopts it, the solver
 * decides what holds it up, the click chain deletes a brick and re-solves. What has never
 * existed is anything that runs any of it without a test calling it by hand — DESIGN.md §5's
 * "press Play on it and watch demos", which is the whole point of the scaffold.
 *
 * ON THE GAME MODE, AND THE GAME MODE IS NAMED EXPLICITLY. FTestWorldWrapper resolves
 * WorldSettings->DefaultGameMode before the project's GlobalDefaultGameMode, so the harness
 * now names one — which means this test exercises ADestructionGameGameMode because it says
 * so, and every other World.* test does NOT build a 1,220-brick wall it never asked for. That
 * interaction is real and was found here: without it, adding a scenario to begin-play would
 * silently multiply the cost of every world test in the suite.
 *
 * THREE CLAIMS, AND EACH FAILS ON ITS OWN. It BUILT something — an id that names a binding
 * the subsystem holds, rather than a wall standing in the world with nothing owning it. It
 * built the RIGHT thing — the piece count the coordinating grid predicts, with a live brick
 * per piece, so a spec quietly halved is a failure rather than a smaller wall. And it is
 * STANDING — nothing released as built, and nothing released by a push either, which is the
 * assertion that separates a wall that holds itself up from one nobody has solved.
 *
 * NOT ASSERTED, DELIBERATELY: that a brick is visible, that the pawn can see it, or that the
 * level contains anything. Those need a viewport and a map, and none of them can be wrong in
 * a way this test's claims are right.
 *
 * NEEDS A TICKING WORLD: it needs a WORLD with begin-play run, because that is when the game
 * mode acts. It never ticks one.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGameModeBuildsTheWallTest,
	"DestructionGame.World.Scenario.GameModeBuildsTheWallOnBeginPlay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FGameModeBuildsTheWallTest::RunTest(const FString& Parameters)
{
	using namespace BrickWallScenarioTestSupport;
	using namespace BrickWorldTestSupport;

	FBrickTestWorld TestWorld;

	const double BeginStart = FPlatformTime::Seconds();
	const bool bBegun = TestWorld.Begin(*this, ADestructionGameGameMode::StaticClass());
	const double BeginSeconds = FPlatformTime::Seconds() - BeginStart;

	AddInfo(FString::Printf(
		TEXT("MEASURED: creating the world and running begin-play with the real game mode took %.1f ms"),
		BeginSeconds * 1000.0));

	if (!bBegun)
	{
		return true;
	}

	ADestructionGameGameMode* const GameMode =
		TestWorld.World->GetAuthGameMode<ADestructionGameGameMode>();

	TestNotNull(
		*FString::Printf(TEXT("fixture: the world should be running ADestructionGameGameMode, it is running %s"),
			*GetNameSafe(TestWorld.World->GetAuthGameMode())),
		GameMode);

	if (GameMode == nullptr)
	{
		TestWorld.End();
		return true;
	}

	/*
	 * ONE: IT BUILT SOMETHING, AND THE SUBSYSTEM OWNS IT.
	 *
	 * The id rather than a wall in the world, because a wall nothing names is a wall nothing
	 * can click: TracePiece resolves the ref a brick carries against the binding the id names,
	 * so a scenario that spawned actors without registering a structure produces a wall that
	 * looks perfect and cannot be touched.
	 */
	const int32 StructureId = GameMode->GetBuiltStructureId();

	TestTrue(
		FString::Printf(
			TEXT("the game mode should have built a structure when play began; GetBuiltStructureId returned %d"),
			StructureId),
		StructureId != INDEX_NONE);

	FStructureBinding* Binding = TestWorld.Subsystem->Find(StructureId);

	TestNotNull(
		*FString::Printf(TEXT("the subsystem should hold the structure the game mode built (id %d)"),
			StructureId),
		Binding);

	if (Binding == nullptr)
	{
		TestWorld.End();
		return true;
	}

	/*
	 * TWO: IT BUILT THE RIGHT THING.
	 */
	TestEqual(
		FString::Printf(TEXT("the wall on Play should be a flush 30 x 40 running bond, %d pieces; it has %d"),
			ScenarioPieceCount, Binding->NumPieces()),
		Binding->NumPieces(), ScenarioPieceCount);

	TestEqual(
		FString::Printf(TEXT("that wall should carry %d joints, it carries %d"),
			ScenarioJointCount, Binding->GetStructure().NumConnections()),
		Binding->GetStructure().NumConnections(), ScenarioJointCount);

	{
		int32 MissingBricks = 0;

		for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
		{
			const ABrickActor* Brick = Cast<ABrickActor>(Binding->GetActor(Piece));

			if (!IsValid(Brick))
			{
				++MissingBricks;

				if (MissingBricks <= ScenarioMaxReportedFailures)
				{
					AddError(FString::Printf(
						TEXT("piece %d of the wall built on Play has no live ABrickActor standing for it"),
						Piece));
				}
			}
		}

		TestEqual(
			FString::Printf(TEXT("every piece of the wall built on Play should have a live brick; %d do not"),
				MissingBricks),
			MissingBricks, 0);
	}

	/*
	 * THREE: IT IS STANDING, BEFORE ANYTHING IS CLICKED.
	 *
	 * Nothing released as built is the first half, and on its own it is nearly free — a wall
	 * nobody solved releases nothing either. The push is what makes it mean something: it
	 * solves and then acts on the answer, so a wall that only LOOKS built comes down here.
	 */
	CheckWallIsStanding(*this, *Binding, TEXT("as the game mode left it"));

	const double PushStart = FPlatformTime::Seconds();
	const int32 Released = TestWorld.Subsystem->SolveAndPush(StructureId);
	const double PushSeconds = FPlatformTime::Seconds() - PushStart;

	AddInfo(FString::Printf(
		TEXT("MEASURED: SolveAndPush on the wall the game mode built took %.1f ms"),
		PushSeconds * 1000.0));

	TestEqual(
		FString::Printf(TEXT("the wall on Play should stand: pushing it must release nothing, it released %d"),
			Released),
		Released, 0);

	CheckWallIsStanding(*this, *Binding, TEXT("after a push"));

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
