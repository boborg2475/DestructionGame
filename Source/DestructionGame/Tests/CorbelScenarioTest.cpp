// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"
#include "HAL/PlatformTime.h"
#include "Tests/CorbelCaseTestSupport.h"
#include "World/DestructionScenarios.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE CORBEL FAMILY AS PLAYABLE SCENARIOS — the seven structures the user reviewed, addressable
 * as levels a human can join and watch.
 *
 * =====================================================================================
 * WHY THESE ROWS CUT NOTHING, WHICH IS THE WHOLE DIFFERENCE FROM `free-end-40`
 * =====================================================================================
 *
 * `Tests/CorbelScreenshotTest.cpp` draws the distinction and it is worth restating, because a
 * catalogue row that copied `free-end-40`'s shape would be a level that shows the wrong thing:
 *
 *   - `free-end-40` is BEFORE AND AFTER A CUT. The wall stands, the player watches a brick go,
 *     and the question is what the remainder does about it.
 *   - A CORBEL IS CONDEMNED BY ITS OWN GEOMETRY. It is laid reaching too far and its root joint
 *     is over capacity the moment it exists, so the honest pair is AS LAID versus SETTLED. There
 *     is no cut to wait for and nothing for a delay to do.
 *
 * So every row here names an empty cut list, and that is asserted rather than left implied: a
 * corbel row that quietly deleted a brick would be showing a human a different experiment from
 * the one its own expectation line describes.
 *
 * =====================================================================================
 * WHAT IS PINNED, AND WHERE EACH NUMBER COMES FROM
 * =====================================================================================
 *
 *   - THE PIECE COUNT, twice over: against a closed form derived here from the coordinating
 *     grid, and against what the world-free fixture actually lays. A level that is a LOOKALIKE
 *     of the fixture — one step taller, one cell wider — proves nothing about any number the
 *     solver suite prints, and nothing about it would look wrong on screen.
 *
 *   - E35 AND E36 STRADDLE THE CROSSOVER, and that is the entire reason those two rows exist as
 *     a PAIR. `Core.Structure.CorbelStepsBeforeTensionWins` locates it by bisection at 36 steps:
 *     the smallest step count whose root joint reads over 1.0, with the step below at or under
 *     capacity. Those numbers are READ OUT OF THAT TEST rather than re-derived here — its own
 *     header records the closed-form check, 0.99029 at 35 against 1.01625 at 36 — so if the
 *     crossover ever moves, that test is where the argument happens and this one follows.
 *
 *   - AND THE LEVEL READS WHAT THE FIXTURE READS, EXACTLY. The scenario's root joint utilisation
 *     is compared with `==` against the same joint of the world-free structure. This is the drift
 *     check that keeps a level and a headless reading about ONE structure: a row with the wrong
 *     base cell count or the wrong step would still build, still look like a corbel, and read a
 *     completely different number.
 *
 * `corbel-f-100` IS DELIBERATELY CHEAP. It is 3,015 bricks — the largest structure in the suite —
 * and `Core.Structure.AHundredStepCorbelMustComeDown` already lays it, solves it and cascades it.
 * Here it is only built and counted; nothing is solved.
 *
 * NEEDS A TICKING WORLD: NO. The catalogue is world-free by construction, which is what keeps
 * this in the fast suite.
 */
namespace CorbelScenarioTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * --- the grid, restated ---------------------------------------------------------------
	 *
	 * DESIGN.md's standard UK metric clay brick and the 1 cm joint that makes the coordinating
	 * grid 22.5 x 11.25 x 7.5. Written out rather than imported, so a catalogue row that quietly
	 * changed brick format fails here rather than agreeing with itself.
	 */
	constexpr double CorbelScenarioBrickLengthCm = 21.5;
	constexpr double CorbelScenarioBrickHeightCm = 6.5;
	constexpr double CorbelScenarioMortarCm = 1.0;

	constexpr double CorbelScenarioCellPitchCm =
		CorbelScenarioBrickLengthCm + CorbelScenarioMortarCm;

	constexpr double CorbelScenarioCoursePitchCm =
		CorbelScenarioBrickHeightCm + CorbelScenarioMortarCm;

	/** The half-cell step every corbel fixture in this project uses. */
	constexpr double CorbelScenarioStepCm = CorbelScenarioCellPitchCm / 2.0;

	/** Three courses of immovable base under every case in the family. */
	constexpr int32 CorbelScenarioBaseCourses = 3;

	inline FString CorbelScenarioBits(double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	inline FString CorbelScenarioVectorBits(const FVector& Value)
	{
		return FString::Printf(TEXT("(%.17g, %.17g, %.17g)"), Value.X, Value.Y, Value.Z);
	}

	/** The row with this name, or null with the reason reported. */
	inline const DestructionScenarios::FScenario* CorbelScenarioRowNamed(
		FAutomationTestBase& Test, const TCHAR* Name)
	{
		const int32 Index = DestructionScenarios::IndexOfName(FName(Name));

		if (!DestructionScenarios::Catalogue().IsValidIndex(Index))
		{
			Test.AddError(FString::Printf(
				TEXT("the catalogue must carry a row named '%s' — the corbel family is not ")
				TEXT("joinable until it does; IndexOfName returned %d against %d row(s)"),
				Name, Index, DestructionScenarios::Catalogue().Num()));

			return nullptr;
		}

		return &DestructionScenarios::Catalogue()[Index];
	}

	/** The piece whose box is centred here, or INDEX_NONE. */
	inline int32 CorbelScenarioPieceAt(const TArray<FPieceBox>& Boxes, const FVector& CentreCm)
	{
		for (int32 Piece = 0; Piece < Boxes.Num(); ++Piece)
		{
			if (Boxes[Piece].CentreCm.Equals(CentreCm, 1.0e-9))
			{
				return Piece;
			}
		}

		return INDEX_NONE;
	}

	inline int32 CorbelScenarioJointBetween(const FStructure& Structure, int32 PieceA, int32 PieceB)
	{
		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			const FConnection& Connection = Structure.GetConnection(Joint);

			if ((Connection.PieceA == PieceA && Connection.PieceB == PieceB)
				|| (Connection.PieceA == PieceB && Connection.PieceB == PieceA))
			{
				return Joint;
			}
		}

		return INDEX_NONE;
	}

	/**
	 * THE ROOT JOINT OF A LAID CORBEL, FOUND BY GEOMETRY.
	 *
	 * The bed joint under the arm's lowest outermost brick — the one place a corbel on an
	 * immovable base can fail, because a rigid body cannot rotate about a fixed base without
	 * separating from it. Located by CENTRES worked out from the grid rather than by a handle
	 * anything handed back, so the same joint can be read out of the scenario's layout and out of
	 * the world-free fixture without either having to agree about handle numbering first.
	 *
	 * The base's top course is even-indexed (three courses), so it is unshifted and its outermost
	 * brick sits at `LeftOrigin + (BaseCells - 1) x 22.5`; the arm's first step lands one step
	 * outboard of that, one course up.
	 */
	inline int32 CorbelScenarioRootJoint(
		const FStructure& Structure,
		const TArray<FPieceBox>& Boxes,
		int32 BaseCells,
		double LeftOriginCm,
		int32& OutSeatPiece,
		int32& OutArmPiece)
	{
		const double SeatXCm = LeftOriginCm + (BaseCells - 1) * CorbelScenarioCellPitchCm;
		const double HalfHeightCm = CorbelScenarioBrickHeightCm / 2.0;

		OutSeatPiece = CorbelScenarioPieceAt(
			Boxes,
			FVector(
				SeatXCm, 0.0,
				HalfHeightCm + (CorbelScenarioBaseCourses - 1) * CorbelScenarioCoursePitchCm));

		OutArmPiece = CorbelScenarioPieceAt(
			Boxes,
			FVector(
				SeatXCm + CorbelScenarioStepCm, 0.0,
				HalfHeightCm + CorbelScenarioBaseCourses * CorbelScenarioCoursePitchCm));

		if (OutSeatPiece == INDEX_NONE || OutArmPiece == INDEX_NONE)
		{
			return INDEX_NONE;
		}

		return CorbelScenarioJointBetween(Structure, OutSeatPiece, OutArmPiece);
	}

	/** One catalogue row of the corbel family, and what it must be. */
	struct FCorbelScenarioRow
	{
		const TCHAR* Name;
		const TCHAR* MapName;

		/** Cells wide the base is: two bare, five when three of them stand opposite. */
		int32 BaseCells;

		int32 Steps;

		/** False is the bare stepped arm of single bricks — case A, and only case A. */
		bool bFilled;

		int32 ExpectedPieces;

		/**
		 * Whether to solve it and read its root joint. E35 and E36 straddle the crossover and are
		 * the reason the pair exists; F is 3,015 bricks and is counted only.
		 */
		bool bReadTheRootJoint;
	};

	/** Case D's origin: three whole cells left of case C's, so both roots sit at the same X. */
	constexpr double CorbelScenarioCounterweightOriginCm = -3.0 * CorbelScenarioCellPitchCm;

	inline double CorbelScenarioLeftOriginOf(const FCorbelScenarioRow& Row)
	{
		return Row.BaseCells == 2 ? 0.0 : CorbelScenarioCounterweightOriginCm;
	}

	/**
	 * THE SEVEN ROWS, AND THEY ARE EXACTLY `Tests/CorbelScreenshotTest.cpp`'s `ShotCases` FAMILY.
	 *
	 * THE PIECE COUNTS ARE DERIVED, NOT RECORDED. The base is `3 x BaseCells`. A bare arm adds one
	 * brick per step. A filled arm at the half-cell step adds `BaseCells + floor(i/2)` on step i —
	 * the outer face advances half a cell per course, so a whole new cell appears every second one
	 * — and summing from 1 to k gives `k x BaseCells + floor(k^2 / 4)`:
	 *
	 *     A   6 + 4                        = 10
	 *     B   6 + 4x2 + floor(16/4)        = 18
	 *     C   6 + 10x2 + floor(100/4)      = 51
	 *     D   15 + 10x5 + floor(100/4)     = 90
	 *     E35 15 + 35x5 + floor(1225/4)    = 496
	 *     E36 15 + 36x5 + floor(1296/4)    = 519
	 *     F   15 + 100x5 + floor(10000/4)  = 3015
	 *
	 * The last agrees with the 3,015 `Core.Structure.AHundredStepCorbelMustComeDown` reports, which
	 * is the cross-check on the closed form rather than its source.
	 */
	const FCorbelScenarioRow CorbelScenarioRows[] =
	{
		{ TEXT("corbel-a-bare-4"), TEXT("Lvl_CorbelABare4"), 2, 4, false, 10, false },
		{ TEXT("corbel-b-filled-4"), TEXT("Lvl_CorbelBFilled4"), 2, 4, true, 18, false },
		{ TEXT("corbel-c-10"), TEXT("Lvl_CorbelC10"), 2, 10, true, 51, true },
		{ TEXT("corbel-d-10-counterweight"), TEXT("Lvl_CorbelD10Counterweight"), 5, 10, true, 90, true },
		{ TEXT("corbel-e35"), TEXT("Lvl_CorbelE35"), 5, 35, true, 496, true },
		{ TEXT("corbel-e36"), TEXT("Lvl_CorbelE36"), 5, 36, true, 519, true },
		{ TEXT("corbel-f-100"), TEXT("Lvl_CorbelF100"), 5, 100, true, 3015, false },
	};

	/** The world-free fixture's spec for a row. Filled rows only; the fixture builds no bare arm. */
	inline CorbelCaseTestSupport::FCorbelSpec CorbelScenarioFixtureSpecOf(
		const FCorbelScenarioRow& Row)
	{
		CorbelCaseTestSupport::FCorbelSpec Spec;

		Spec.Scale = 1.0;
		Spec.StepCm = CorbelScenarioStepCm;
		Spec.BaseCourses = CorbelScenarioBaseCourses;
		Spec.BaseCells = Row.BaseCells;
		Spec.Steps = Row.Steps;
		Spec.LeftOriginCm = CorbelScenarioLeftOriginOf(Row);
		Spec.Strength = GeneralPurposeMortar;

		return Spec;
	}

	/**
	 * THE STEP COUNTS THE E35/E36 LEVELS WERE BUILT AT — the CHARACTERISTIC-basis crossover's
	 * straddle (0.99029 at 35, 1.01625 at 36, from `F(s) = 1 + s + s(s+1)/4` and
	 * `M(s) = 5.625(s+1) + 11.25 x SUM F` against f_xk1 = 0.10).
	 *
	 * SINCE THE 2026-08-14 MEAN RE-ANCHOR FLIP THIS IS A CONTENT PIN, NOT A CROSSOVER: at
	 * f_x1 = 0.70 the worst-axis crossover moves to ~124 steps (compression), so 35/36 no
	 * longer straddle anything. The pair's replacement is owed (CURRENT_STATE); these rows
	 * stay pinned so the shipped levels do not drift while the replacement is designed.
	 */
	constexpr int32 CorbelScenarioCrossoverSteps = 36;
}

/**
 * THE SEVEN CORBEL ROWS ARE IN THE CATALOGUE, THEY BUILD, AND THEY ARE THE FIXTURES THE SOLVER
 * SUITE HAS BEEN MEASURING.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorbelScenarioCatalogueTest,
	"DestructionGame.World.Scenarios.CorbelRows",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCorbelScenarioCatalogueTest::RunTest(const FString& Parameters)
{
	using namespace CorbelScenarioTestSupport;
	using namespace DestructionScenarios;
	using namespace DestructionLayout;

	TestTrue(
		*FString::Printf(
			TEXT("FIXTURE: E35 and E36 must sit either side of the %d-step crossover ")
			TEXT("Core.Structure.CorbelStepsBeforeTensionWins locates"),
			CorbelScenarioCrossoverSteps),
		CorbelScenarioRows[4].Steps == CorbelScenarioCrossoverSteps - 1
			&& CorbelScenarioRows[5].Steps == CorbelScenarioCrossoverSteps);

	/*
	 * --- the piece counts are arbitrated BEFORE they are used as expectations --------------
	 *
	 * The closed form in the table's own comment is a second reading of the family, written in
	 * this file. A wrong expectation is worse than no test, because a level that laid the right
	 * corbel would then be reported as laying the wrong one and whoever went looking would find
	 * nothing. So each literal is held against what the world-free fixture actually lays, which
	 * costs one build per row — 0.06 s even for the hundred-step case — and is GREEN BEFORE THE
	 * CATALOGUE ROWS EXIST. That is what says the red below is red for the right reason.
	 *
	 * The bare arm is skipped: the fixture header deliberately builds no bare arm, and
	 * `Core.Corbel.LaysTheFamilyOnItsGrid` is what arbitrates case A.
	 */
	for (const FCorbelScenarioRow& Row : CorbelScenarioRows)
	{
		if (!Row.bFilled)
		{
			continue;
		}

		CorbelCaseTestSupport::FCorbelStructure Fixture;

		if (!CorbelCaseTestSupport::CorbelBuild(CorbelScenarioFixtureSpecOf(Row), Fixture))
		{
			AddError(FString::Printf(
				TEXT("FIXTURE: '%s' must build through CorbelCaseTestSupport, or this file's ")
				TEXT("piece counts are unarbitrated"),
				Row.Name));

			continue;
		}

		TestTrue(
			*FString::Printf(
				TEXT("FIXTURE: '%s': the closed form says %d pieces and the world-free fixture ")
				TEXT("lays %d (%d joints). If these disagree the expectation below is wrong, not ")
				TEXT("the catalogue."),
				Row.Name, Row.ExpectedPieces, Fixture.Structure.NumPieces(),
				Fixture.Structure.NumConnections()),
			Fixture.Structure.NumPieces() == Row.ExpectedPieces);
	}

	/** Filled in as the sweep goes, so the straddle can be asserted as a PAIR at the end. */
	double RootReadingAtSteps[2] = { -1.0, -1.0 };

	for (const FCorbelScenarioRow& Row : CorbelScenarioRows)
	{
		const FScenario* const Scenario = CorbelScenarioRowNamed(*this, Row.Name);

		if (Scenario == nullptr)
		{
			continue;
		}

		TestEqual(
			*FString::Printf(TEXT("'%s' must be joinable by its own map"), Row.Name),
			FString(Scenario->MapName), FString(Row.MapName));

		/*
		 * A CORBEL IS CONDEMNED BY ITS OWN GEOMETRY, so the level's whole story is the settle.
		 * A row here that named a cut would be showing a human a different experiment from the
		 * one its expectation line describes.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("'%s' must cut NOTHING — a corbel is condemned by its own geometry and the ")
				TEXT("level's whole story is as-laid versus settled; it names %d cut(s)"),
				Row.Name, Scenario->CutCentresCm.Num()),
			Scenario->CutCentresCm.Num() == 0);

		const double LaidStartedAt = FPlatformTime::Seconds();

		FBrickLayout Laid;
		TArray<int32> Cut;

		if (!Build(*Scenario, Laid, Cut))
		{
			AddError(FString::Printf(
				TEXT("'%s' must build: a catalogue row that cannot be laid is a level that cannot ")
				TEXT("be joined"),
				Row.Name));

			continue;
		}

		const double LaidSeconds = FPlatformTime::Seconds() - LaidStartedAt;

		TestTrue(
			*FString::Printf(
				TEXT("'%s' resolves no cut, so it must hand back an empty cut list; it handed back %d"),
				Row.Name, Cut.Num()),
			Cut.Num() == 0);

		TestTrue(
			*FString::Printf(
				TEXT("'%s' must lay %d pieces — %d courses of base %d cells wide, then %d stepped ")
				TEXT("courses %s; it laid %d"),
				Row.Name, Row.ExpectedPieces, CorbelScenarioBaseCourses, Row.BaseCells, Row.Steps,
				Row.bFilled ? TEXT("filled") : TEXT("of one brick each"),
				Laid.Structure.NumPieces()),
			Laid.Structure.NumPieces() == Row.ExpectedPieces);

		TestTrue(
			*FString::Printf(
				TEXT("'%s' must hand back one box per piece: %d boxes for %d pieces"),
				Row.Name, Laid.Boxes.Num(), Laid.Structure.NumPieces()),
			Laid.Boxes.Num() == Laid.Structure.NumPieces());

		AddInfo(FString::Printf(
			TEXT("'%s': %d pieces and %d joints, laid in %.1f ms"),
			Row.Name, Laid.Structure.NumPieces(), Laid.Structure.NumConnections(),
			LaidSeconds * 1000.0));

		/*
		 * `corbel-f-100` STOPS HERE, DELIBERATELY. 3,015 bricks is the largest structure in the
		 * suite and Core.Structure.AHundredStepCorbelMustComeDown already solves and cascades it;
		 * nothing this file asserts would be truer for doing it twice.
		 */
		if (!Row.bReadTheRootJoint)
		{
			continue;
		}

		/* --- and it is the fixture's own structure, not a lookalike ------------------------- */

		CorbelCaseTestSupport::FCorbelStructure Fixture;

		if (!CorbelCaseTestSupport::CorbelBuild(CorbelScenarioFixtureSpecOf(Row), Fixture))
		{
			AddError(FString::Printf(
				TEXT("FIXTURE: '%s' must also build through CorbelCaseTestSupport, or there is ")
				TEXT("nothing to hold the level against"),
				Row.Name));

			continue;
		}

		TestTrue(
			*FString::Printf(
				TEXT("'%s' must lay the SAME structure the world-free fixture lays — %d pieces and ")
				TEXT("%d joints against the fixture's %d and %d. A lookalike proves nothing about ")
				TEXT("any number the solver suite prints and nothing about it looks wrong on screen."),
				Row.Name, Laid.Structure.NumPieces(), Laid.Structure.NumConnections(),
				Fixture.Structure.NumPieces(), Fixture.Structure.NumConnections()),
			Laid.Structure.NumPieces() == Fixture.Structure.NumPieces()
				&& Laid.Structure.NumConnections() == Fixture.Structure.NumConnections());

		int32 SeatPiece = INDEX_NONE;
		int32 ArmPiece = INDEX_NONE;

		const int32 RootJoint = CorbelScenarioRootJoint(
			Laid.Structure, Laid.Boxes, Row.BaseCells, CorbelScenarioLeftOriginOf(Row),
			SeatPiece, ArmPiece);

		if (RootJoint == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("'%s' must present a root joint — the arm's lowest outermost brick (piece %d) ")
				TEXT("standing on the base's top-course outermost brick (piece %d)"),
				Row.Name, ArmPiece, SeatPiece));

			continue;
		}

		TestEqual(
			*FString::Printf(
				TEXT("'%s': the root must be a BED joint beneath the arm, or the reading taken ")
				TEXT("there is about a different mechanism"),
				Row.Name),
			Laid.Structure.GetJointRole(RootJoint, ArmPiece),
			EJointRole::BedBeneath);

		Laid.Structure.SolveLoads();
		Fixture.Structure.SolveLoads();

		const double Reading = Laid.Structure.GetConnectionUtilisation(RootJoint);

		const double FixtureReading = Fixture.RootJoint != INDEX_NONE
			? Fixture.Structure.GetConnectionUtilisation(Fixture.RootJoint)
			: -1.0;

		AddInfo(FString::Printf(
			TEXT("'%s': the root joint (%d, between pieces %d and %d) reads %s; the world-free ")
			TEXT("fixture's reads %s"),
			Row.Name, RootJoint, SeatPiece, ArmPiece, *CorbelScenarioBits(Reading),
			*CorbelScenarioBits(FixtureReading)));

		TestTrue(
			*FString::Printf(
				TEXT("'%s': THE LEVEL MUST READ WHAT THE FIXTURE READS, EXACTLY — %s against %s. A ")
				TEXT("row with the wrong base or the wrong step still builds, still looks like a ")
				TEXT("corbel, and reads a completely different number."),
				Row.Name, *CorbelScenarioBits(Reading), *CorbelScenarioBits(FixtureReading)),
			Reading == FixtureReading);

		if (Row.Steps == CorbelScenarioCrossoverSteps - 1)
		{
			RootReadingAtSteps[0] = Reading;
		}
		else if (Row.Steps == CorbelScenarioCrossoverSteps)
		{
			RootReadingAtSteps[1] = Reading;
		}
	}

	/*
	 * THE STRADDLE IS DEAD — THE 2026-08-13 MEAN RE-ANCHOR KILLED IT, AND SAYING SO IS THE
	 * HONEST FORM. On the characteristic basis E35/E36 sat either side of the 36-step tension
	 * crossover (0.990 / 1.016); at the mean f_x1 = 0.70 both read a seventh of that (~0.142 /
	 * ~0.145) and the worst-axis crossover moves to ~124 steps, where COMPRESSION crushes the
	 * root (F(k)/A against the unmoved 10 MPa — see CorbelStepsBeforeTensionWins' re-derived
	 * header). The two levels stay in the catalogue as content; what they can no longer do is
	 * discriminate the crossover, and the REPLACEMENT PAIR IS OWED — levels either side of the
	 * measured mean-basis crossover, specified in CURRENT_STATE, laid and MEASURED in the
	 * green phase.
	 *
	 * What still separates the pair is the monotone step term, pinned as an ordering so the
	 * two rows are not two pictures of the same thing.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("'corbel-e35' must still read a real, positive root utilisation — it reads %s"),
			*CorbelScenarioBits(RootReadingAtSteps[0])),
		RootReadingAtSteps[0] > 0.0 && RootReadingAtSteps[0] < 1.0);

	TestTrue(
		*FString::Printf(
			TEXT("'corbel-e36' must read STRICTLY MORE than E35 (one more step of mass outboard ")
			TEXT("of the root) and, at mean strengths, still under capacity — %s against %s"),
			*CorbelScenarioBits(RootReadingAtSteps[1]),
			*CorbelScenarioBits(RootReadingAtSteps[0])),
		RootReadingAtSteps[1] > RootReadingAtSteps[0] && RootReadingAtSteps[1] < 1.0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
