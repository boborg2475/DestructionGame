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
 * The seven corbel structures as joinable catalogue levels.
 *
 * No row cuts anything: unlike free-end-40 (before/after a cut), a corbel is condemned by its own
 * geometry, so its level is as-laid versus settled. The empty cut list is asserted.
 *
 * Pinned: the piece count (against a closed form and against the world-free fixture), and the
 * root joint utilisation, compared with == against the fixture's so a lookalike row fails.
 * corbel-f-100 (3,015 bricks) is only built and counted; AHundredStepCorbelMustComeDown solves it.
 *
 * World-free; runs in the fast suite.
 */
namespace CorbelScenarioTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	// The DESIGN.md brick and 1 cm joint, written out so a changed brick format fails here.
	constexpr double CorbelScenarioBrickLengthCm = 21.5;
	constexpr double CorbelScenarioBrickHeightCm = 6.5;
	constexpr double CorbelScenarioMortarCm = 1.0;

	constexpr double CorbelScenarioCellPitchCm =
		CorbelScenarioBrickLengthCm + CorbelScenarioMortarCm;

	constexpr double CorbelScenarioCoursePitchCm =
		CorbelScenarioBrickHeightCm + CorbelScenarioMortarCm;

	/** The half-cell corbel step. */
	constexpr double CorbelScenarioStepCm = CorbelScenarioCellPitchCm / 2.0;

	/** Courses of immovable base under every case. */
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
	 * The corbel's root joint: the bed under the arm's lowest outermost brick, the only place a
	 * corbel on an immovable base can fail. Found by grid centres, not handles, so the scenario and
	 * the fixture can be compared. The seat is at LeftOrigin + (BaseCells - 1) x 22.5 on the top
	 * base course; the arm brick is one step outboard, one course up.
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

		/** False only for case A, the bare arm of single bricks. */
		bool bFilled;

		int32 ExpectedPieces;

		/** Whether to solve and read the root joint. F (3,015 bricks) is counted only. */
		bool bReadTheRootJoint;
	};

	/** Case D's origin: three whole cells left of case C's, so both roots sit at the same X. */
	constexpr double CorbelScenarioCounterweightOriginCm = -3.0 * CorbelScenarioCellPitchCm;

	inline double CorbelScenarioLeftOriginOf(const FCorbelScenarioRow& Row)
	{
		return Row.BaseCells == 2 ? 0.0 : CorbelScenarioCounterweightOriginCm;
	}

	/**
	 * The seven rows, matching CorbelScreenshotTest.cpp's ShotCases. Piece counts are derived: base
	 * 3 x BaseCells; a bare arm adds one per step; a filled arm adds BaseCells + floor(i/2) on step
	 * i, summing to k x BaseCells + floor(k^2 / 4):
	 *
	 *     A   6 + 4                        = 10
	 *     B   6 + 4x2 + floor(16/4)        = 18
	 *     C   6 + 10x2 + floor(100/4)      = 51
	 *     D   15 + 10x5 + floor(100/4)     = 90
	 *     E35 15 + 35x5 + floor(1225/4)    = 496
	 *     E36 15 + 36x5 + floor(1296/4)    = 519
	 *     F   15 + 100x5 + floor(10000/4)  = 3015
	 *
	 * F cross-checks against AHundredStepCorbelMustComeDown's 3,015.
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

	/** The world-free fixture's spec for a filled row. */
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
	 * E35/E36 were built to straddle the characteristic-basis crossover (0.99029 / 1.01625). On the
	 * mean basis the crossover is ~124 steps, so this is now only a content pin; a replacement pair
	 * is owed (CURRENT_STATE).
	 */
	constexpr int32 CorbelScenarioCrossoverSteps = 36;
}

/** The seven corbel rows are in the catalogue, build, and match the solver suite's fixtures. */
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
	 * Check the expected piece counts against the world-free fixture before using them, so a wrong
	 * expectation cannot blame a correct level. The bare arm is covered by
	 * Core.Corbel.LaysTheFamilyOnItsGrid instead.
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

	/** E35 and E36 root readings, compared as a pair at the end. */
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

		// corbel-f-100 stops here; AHundredStepCorbelMustComeDown already solves it.
		if (!Row.bReadTheRootJoint)
		{
			continue;
		}

		/* --- the fixture's own structure, not a lookalike ----------------------------------- */

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
	 * On the mean basis E35/E36 no longer straddle the crossover (~0.142 / ~0.145; the crossover is
	 * ~124 steps, in compression; see CorbelStepsBeforeTensionWins). A replacement pair is owed
	 * (CURRENT_STATE). Meanwhile the pair is pinned as an ordering, both under capacity.
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
