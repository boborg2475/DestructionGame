// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Corbel.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"
#include "Tests/CorbelCaseTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The corbel builder lays exactly the structure every corbel reading was taken on. Several tests
 * (CorbelStepsBeforeTensionWins, AHundredStepCorbelMustComeDown, ACorbelReadsLinearlyInScale and
 * others) reach DestructionCorbel::Build through Tests/CorbelCaseTestSupport.h with expected values
 * worked to fifteen digits, so a builder that shifted one brick by an ulp, or offered MakeInterface
 * one pair fewer, would move all of them at once and look like a solver regression. This file pins
 * the producer: same pieces, boxes, masses, grounded flags, connection set in order, and root number.
 *
 * Two tests, and the second is not a restatement. LaysTheFamilyOnItsGrid derives every brick from
 * the coordinating grid. LaysTheJointsTheGridImplies derives every joint from those bricks by a rule
 * production does not use (every pair offered to MakeInterface, versus production's within-course and
 * adjacent-course pairs, which its comment calls a cost bound not a rule); the two agree iff that
 * claim holds. A corbel whose bricks are all placed can still be joined wrongly, leaving the grid
 * test and HasCompleteGeometry green while every reading moves. That is the second test's failure.
 *
 * And one absolute number per case, because the rest of the suite's corbel claims are ordinal (a
 * crossover at 36 steps, an increasing ladder, one profile above another), so a joint set that
 * changed but kept its shape would move every absolute reading while every ordinal claim held. Each
 * row carries its root utilisation, pinned exactly. Case A's is the one COMPOSITE_DEPTH_DESIGN.md
 * works out by hand; the rest are the readings the levels catalogue and scenario reports publish.
 *
 * The bare arm (case A) is covered here and nowhere else but CorbelScreenshotTest.cpp. bFilled is
 * the whole difference: one brick per stepped course versus every cell inboard, a different load path.
 *
 * No ticking world: boxes, doubles and one arithmetic solve.
 */
namespace CorbelBuilderTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * The grid, spelled out rather than imported: DESIGN.md's standard clay brick and 1 cm joint that
	 * make the coordinating grid 22.5 x 11.25 x 7.5. From first principles, so a test does not agree
	 * with a wrong production constant instead of failing.
	 */
	constexpr double CorbelBuilderBrickLengthCm = 21.5;
	constexpr double CorbelBuilderBrickWidthCm = 10.25;
	constexpr double CorbelBuilderBrickHeightCm = 6.5;
	constexpr double CorbelBuilderMortarCm = 1.0;
	constexpr double CorbelBuilderDensityGramsPerCubicCm = 1.9;

	constexpr double CorbelBuilderCellPitchCm =
		CorbelBuilderBrickLengthCm + CorbelBuilderMortarCm;

	constexpr double CorbelBuilderCoursePitchCm =
		CorbelBuilderBrickHeightCm + CorbelBuilderMortarCm;

	/** The half-cell step every fixture in this project uses. */
	constexpr double CorbelBuilderHalfCellStepCm = CorbelBuilderCellPitchCm / 2.0;

	/** Case D's origin: three whole cells to the left of case C's, so both roots sit at the same X. */
	constexpr double CorbelBuilderCounterweightOriginCm = -3.0 * CorbelBuilderCellPitchCm;

	inline FString CorbelBuilderBits(double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	inline FString CorbelBuilderVectorBits(const FVector& Value)
	{
		return FString::Printf(TEXT("(%.17g, %.17g, %.17g)"), Value.X, Value.Y, Value.Z);
	}

	/**
	 * What one brick weighs, multiplied volume-first and density-last where PieceMassKg does the
	 * reverse: an independent computation, compared with a relative tolerance because that order costs an ulp.
	 */
	inline double CorbelBuilderMassKg(const FVector& HalfExtentCm)
	{
		const double VolumeCubicCm =
			(2.0 * HalfExtentCm.X) * (2.0 * HalfExtentCm.Y) * (2.0 * HalfExtentCm.Z);

		return VolumeCubicCm * CorbelBuilderDensityGramsPerCubicCm / 1000.0;
	}

	/** One expected brick: where it is, how big, and whether it is standing on the earth. */
	struct FCorbelBuilderBrick
	{
		FVector CentreCm = FVector::ZeroVector;
		FVector HalfExtentCm = FVector::ZeroVector;
		bool bGrounded = false;
	};

	/**
	 * Every brick the spec describes, derived from the drawing (CORBEL_CASES.html) rather than the
	 * builder: the base is BaseCells cells per course with alternate courses shifted one step, and the
	 * arm's outermost brick advances one step per course with as many whole cells inboard as fit. At
	 * the half-cell step this agrees with the drawing's {2, 3, 3, 4, 4, 5, 5, 6, 6, 7}.
	 *
	 * The order is part of the claim (base courses bottom-up, then arm, each course left to right): a
	 * piece handle is the identity joints, break stamps and readings use, so a different lay order
	 * renumbers all of them while every geometric assertion still passes.
	 */
	inline TArray<FCorbelBuilderBrick> CorbelBuilderExpectedBricks(
		const DestructionCorbel::FCorbelSpec& Spec)
	{
		TArray<FCorbelBuilderBrick> Bricks;

		const double CellPitch = CorbelBuilderCellPitchCm * Spec.Scale;
		const double CoursePitch = CorbelBuilderCoursePitchCm * Spec.Scale;
		const double Step = Spec.StepCm * Spec.Scale;
		const double LeftOrigin = Spec.LeftOriginCm * Spec.Scale;

		const FVector HalfExtentCm(
			CorbelBuilderBrickLengthCm * Spec.Scale / 2.0,
			CorbelBuilderBrickWidthCm * Spec.Scale / 2.0,
			CorbelBuilderBrickHeightCm * Spec.Scale / 2.0);

		const auto CourseZCm = [&](int32 Course)
		{
			return CorbelBuilderBrickHeightCm * Spec.Scale / 2.0 + Course * CoursePitch;
		};

		const auto Add = [&](double CentreXCm, int32 Course, bool bGrounded)
		{
			FCorbelBuilderBrick Brick;
			Brick.CentreCm = FVector(CentreXCm, 0.0, CourseZCm(Course));
			Brick.HalfExtentCm = HalfExtentCm;
			Brick.bGrounded = bGrounded;

			Bricks.Add(Brick);
		};

		for (int32 Course = 0; Course < Spec.BaseCourses; ++Course)
		{
			const double CourseOrigin = LeftOrigin + (Course % 2 == 1 ? Step : 0.0);

			for (int32 Cell = 0; Cell < Spec.BaseCells; ++Cell)
			{
				Add(CourseOrigin + Cell * CellPitch, Course, true);
			}
		}

		const double BaseOuterCentreCm = LeftOrigin + (Spec.BaseCells - 1) * CellPitch;

		for (int32 StepIndex = 1; StepIndex <= Spec.Steps; ++StepIndex)
		{
			const int32 Course = Spec.BaseCourses + StepIndex - 1;
			const double OuterCentreCm = BaseOuterCentreCm + StepIndex * Step;

			if (!Spec.bFilled)
			{
				Add(OuterCentreCm, Course, false);
				continue;
			}

			/*
			 * A half-open cell count. Several step sizes divide the cell pitch exactly (11.25 into
			 * 22.5 twice), so a bare floor is one cell either way on the last bit. The nudge keeps the
			 * leftmost brick inboard of the base's left edge, never inventing masonry not in the drawing.
			 */
			const int32 Cells =
				FMath::FloorToInt32((OuterCentreCm - LeftOrigin) / CellPitch + 1.0e-9) + 1;

			for (int32 Cell = Cells - 1; Cell >= 0; --Cell)
			{
				Add(OuterCentreCm - Cell * CellPitch, Course, false);
			}
		}

		return Bricks;
	}

	/**
	 * Every joint those bricks imply, found by a rule production does not use: "any two bricks that
	 * share a face are joined", every unordered pair offered to MakeInterface. Corbel.cpp instead
	 * offers only within-course and adjacent-course pairs, its comment explicit that this is a cost
	 * bound not a rule (farther bricks exceed the joint thickness and MakeInterface refuses them). If
	 * that holds the two sets are identical; if the loop ever stops offering a course's beds they differ.
	 *
	 * MakeInterface is shared on purpose, the one thing not independent here: its areas, normals,
	 * centres and rectangles are asserted in Core.Layout.* against hand-worked faces. What is derived
	 * here is which pairs are joined and in what order. The order is part of the claim (handles ascend
	 * course-major then by X), so a set that is right but renumbered is not the same structure.
	 */
	inline TArray<FConnection> CorbelBuilderExpectedJoints(
		const DestructionCorbel::FCorbelSpec& Spec, const TArray<FCorbelBuilderBrick>& Bricks)
	{
		TArray<FPieceBox> Boxes;
		Boxes.Reserve(Bricks.Num());

		for (const FCorbelBuilderBrick& Brick : Bricks)
		{
			FPieceBox Box;
			Box.CentreCm = Brick.CentreCm;
			Box.ExtentCm = Brick.HalfExtentCm;

			Boxes.Add(Box);
		}

		const double JointCm = CorbelBuilderMortarCm * Spec.Scale;

		TArray<FConnection> Joints;

		for (int32 PieceA = 0; PieceA < Boxes.Num(); ++PieceA)
		{
			for (int32 PieceB = PieceA + 1; PieceB < Boxes.Num(); ++PieceB)
			{
				FConnection Joint;

				if (MakeInterface(
						PieceA, Boxes[PieceA], PieceB, Boxes[PieceB], JointCm, Spec.Strength, Joint))
				{
					Joints.Add(Joint);
				}
			}
		}

		return Joints;
	}

	/**
	 * The piece count in closed form, so a row's literal is checkable. Base is BaseCourses x BaseCells;
	 * a bare arm adds one per step; a filled arm at the half-cell step adds BaseCells + floor(i/2) on
	 * step i, summing to k x BaseCells + floor(k^2 / 4).
	 */
	inline int32 CorbelBuilderExpectedPieces(const DestructionCorbel::FCorbelSpec& Spec)
	{
		const int32 Base = Spec.BaseCourses * Spec.BaseCells;

		if (!Spec.bFilled)
		{
			return Base + Spec.Steps;
		}

		return Base + Spec.Steps * Spec.BaseCells + (Spec.Steps * Spec.Steps) / 4;
	}

	/**
	 * The root joint, found by geometry and never by a builder handle: the bed joint under the arm's
	 * lowest outermost brick, the one place a corbel on an immovable base can fail. Both ends are
	 * located by their grid-derived centres, so the same joint reads out of two structures whose
	 * handle numbering is not yet proved identical.
	 */
	inline int32 CorbelBuilderPieceAt(const TArray<FPieceBox>& Boxes, const FVector& CentreCm)
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

	inline int32 CorbelBuilderJointBetween(const FStructure& Structure, int32 PieceA, int32 PieceB)
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

	/** The root joint of a laid corbel, or INDEX_NONE, with the two pieces it joins reported. */
	inline int32 CorbelBuilderRootJoint(
		const DestructionCorbel::FCorbelSpec& Spec,
		const FBrickLayout& Layout,
		int32& OutSeatPiece,
		int32& OutArmPiece)
	{
		const double CellPitch = CorbelBuilderCellPitchCm * Spec.Scale;
		const double CoursePitch = CorbelBuilderCoursePitchCm * Spec.Scale;
		const double Step = Spec.StepCm * Spec.Scale;
		const double LeftOrigin = Spec.LeftOriginCm * Spec.Scale;
		const double HalfHeight = CorbelBuilderBrickHeightCm * Spec.Scale / 2.0;

		const double SeatXCm = LeftOrigin + (Spec.BaseCells - 1) * CellPitch;

		OutSeatPiece = CorbelBuilderPieceAt(
			Layout.Boxes,
			FVector(SeatXCm, 0.0, HalfHeight + (Spec.BaseCourses - 1) * CoursePitch));

		OutArmPiece = CorbelBuilderPieceAt(
			Layout.Boxes,
			FVector(SeatXCm + Step, 0.0, HalfHeight + Spec.BaseCourses * CoursePitch));

		if (OutSeatPiece == INDEX_NONE || OutArmPiece == INDEX_NONE)
		{
			return INDEX_NONE;
		}

		return CorbelBuilderJointBetween(Layout.Structure, OutSeatPiece, OutArmPiece);
	}

	/** One row of the family: the piece count its own arithmetic predicts, and what its root reads. */
	struct FCorbelBuilderRow
	{
		const TCHAR* Label;
		int32 BaseCells;
		double LeftOriginCm;
		int32 Steps;
		bool bFilled;
		int32 ExpectedPieces;

		/**
		 * The root joint's utilisation after a solve, an anchor not a derivation. Every other corbel
		 * claim is ordinal (a crossover at 36 steps, an increasing ladder, one profile above another),
		 * so a joint set that changed while keeping the family's shape would satisfy them all and move
		 * this. Written to seventeen digits, compared exactly. Case A's is COMPOSITE_DEPTH_DESIGN.md's
		 * by-hand figure.
		 */
		double ExpectedRootUtilisation;
	};

	/**
	 * The family the seven catalogue rows are made of. A to D are the structures the user reviewed;
	 * E35 and E36 straddle the crossover CorbelStepsBeforeTensionWins locates at 36 steps. F (a
	 * hundred steps, 3,015 bricks) is absent: the joint derivation below is quadratic in piece count
	 * (nine million pairs) and would tell this file nothing E36 does not.
	 */
	/*
	 * Mean re-anchor (2026-08-13): every root here is tension-governed, so each anchor is the old
	 * characteristic measurement / 7 (f_x1 0.10 -> 0.70). Rows are pinned with exact ==, so the red
	 * phase wrote each as old / 7.0 and the green phase re-pinned measured bits an ulp off. At the
	 * flip (2026-08-14) rows A, C and D read one ulp above their division (the literal is the measured
	 * value, the trailing comment keeps the old expression); B, E35 and E36 came back bit-identical.
	 * E35/E36 no longer straddle 1.0 (the crossover moved to ~124 steps in compression, CURRENT_STATE);
	 * their labels keep the history.
	 */
	const FCorbelBuilderRow CorbelBuilderRows[] =
	{
		{ TEXT("A: bare stepped arm, four single bricks"),
			2, 0.0, 4, false, 10, 0.022304100000000004 }, // measured; 0.15612870000000001 / 7.0 + 1 ulp

		{ TEXT("B: the same four-step profile filled solid"),
			2, 0.0, 4, true, 18, 0.19516087500000001 / 7.0 },

		{ TEXT("C: filled, ten steps, on the bare two-cell base"),
			2, 0.0, 10, true, 51, 0.049258953351562489 }, // measured; 0.34481267346093736 / 7.0 + 1 ulp

		{ TEXT("D: case C plus three cells of masonry opposite"),
			5, CorbelBuilderCounterweightOriginCm, 10, true, 90, 0.049069020000000019 }, // measured; 0.34348314000000008 / 7.0 + 1 ulp

		{ TEXT("E35: the characteristic-era straddle's lower rung"),
			5, CorbelBuilderCounterweightOriginCm, 35, true, 496, 0.99046165225599581 / 7.0 },

		{ TEXT("E36: the characteristic-era straddle's upper rung"),
			5, CorbelBuilderCounterweightOriginCm, 36, true, 519, 1.0164705641576046 / 7.0 },
	};

	/** The production spec for a row. */
	inline DestructionCorbel::FCorbelSpec CorbelBuilderSpecOf(const FCorbelBuilderRow& Row)
	{
		DestructionCorbel::FCorbelSpec Spec;

		Spec.Scale = 1.0;
		Spec.StepCm = CorbelBuilderHalfCellStepCm;
		Spec.BaseCourses = 3;
		Spec.BaseCells = Row.BaseCells;
		Spec.Steps = Row.Steps;
		Spec.LeftOriginCm = Row.LeftOriginCm;
		Spec.bFilled = Row.bFilled;
		Spec.Strength = GeneralPurposeMortar;

		return Spec;
	}

	/** The same row, as the test fixture's own spec. Filled rows only; the fixture builds no arm. */
	inline CorbelCaseTestSupport::FCorbelSpec CorbelBuilderFixtureSpecOf(const FCorbelBuilderRow& Row)
	{
		CorbelCaseTestSupport::FCorbelSpec Spec;

		Spec.Scale = 1.0;
		Spec.StepCm = CorbelBuilderHalfCellStepCm;
		Spec.BaseCourses = 3;
		Spec.BaseCells = Row.BaseCells;
		Spec.Steps = Row.Steps;
		Spec.LeftOriginCm = Row.LeftOriginCm;
		Spec.Strength = GeneralPurposeMortar;

		return Spec;
	}
}

/**
 * Every brick of every case, where the coordinating grid says it goes.
 *
 * The claim is independent of the fixture; the test is not entirely. What is asserted about
 * production is asserted against `CorbelBuilderExpectedBricks`, a second reading of
 * `claude_plans/CORBEL_CASES.html` written in this file and owing nothing to
 * `CorbelCaseTestSupport.h`. But the fixture-precondition block below arbitrates that derivation
 * against the fixture before using it, and the fixture is now a call to production — so the
 * precondition compares two runs of one builder and can no longer fail. It is retained as the
 * statement of what the derivation is answerable to, and carries no weight: delete it and every
 * assertion in this test says exactly what it says today.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorbelBuilderLaysTheGridTest,
	"DestructionGame.Core.Corbel.LaysTheFamilyOnItsGrid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCorbelBuilderLaysTheGridTest::RunTest(const FString& Parameters)
{
	using namespace CorbelBuilderTestSupport;

	/* The fixture's own premise, asserted rather than imported: every mass below is derived
	 * against 1.9 g/cm3, so a profile that moved would make them all quietly wrong. */
	TestTrue(
		*FString::Printf(
			TEXT("FIXTURE: derived against clay brick at %s g/cm3, the profile carries %s"),
			*CorbelBuilderBits(CorbelBuilderDensityGramsPerCubicCm),
			*CorbelBuilderBits(ClayBrick.DensityGramsPerCubicCm)),
		ClayBrick.DensityGramsPerCubicCm == CorbelBuilderDensityGramsPerCubicCm);

	for (const FCorbelBuilderRow& Row : CorbelBuilderRows)
	{
		const DestructionCorbel::FCorbelSpec Spec = CorbelBuilderSpecOf(Row);

		/*
		 * The derivation is checked against the fixture before it is used as an expectation, and
		 * this block is a fixture precondition rather than the claim.
		 *
		 * Everything below holds production against `CorbelBuilderExpectedBricks`, a second
		 * reading of `claude_plans/CORBEL_CASES.html` written in this file — a wrong expectation
		 * is worse than no test, because it sends whoever is implementing the builder hunting a
		 * defect that is in the test instead. The fixture the whole solver suite already measures
		 * is standing right there, so the derivation is held against it and any disagreement is
		 * reported as a fixture fault in this file rather than as a defect in the builder.
		 *
		 * It no longer arbitrates anything, because `CorbelCaseTestSupport::CorbelBuild` is now a
		 * call to `DestructionCorbel::Build` plus indexing. It was what said the red below was red
		 * for the right reason while the builder was being moved into production; it is kept as
		 * the written statement of what this file's derivation is answerable to. The bare arm has
		 * no fixture and is skipped — only its fill differs, and one brick per course needs no
		 * arbitration.
		 */
		if (Row.bFilled)
		{
			CorbelCaseTestSupport::FCorbelStructure Fixture;

			if (CorbelCaseTestSupport::CorbelBuild(CorbelBuilderFixtureSpecOf(Row), Fixture))
			{
				const TArray<FCorbelBuilderBrick> Derived = CorbelBuilderExpectedBricks(Spec);

				int32 FirstDerivedWrong = INDEX_NONE;

				const int32 CommonDerived = FMath::Min(Derived.Num(), Fixture.Boxes.Num());

				for (int32 Piece = 0; Piece < CommonDerived; ++Piece)
				{
					if (!(Derived[Piece].CentreCm == Fixture.Boxes[Piece].CentreCm)
						|| !(Derived[Piece].HalfExtentCm == Fixture.Boxes[Piece].ExtentCm)
						|| Derived[Piece].bGrounded
							!= Fixture.Structure.GetPiece(Piece).bIsGrounded)
					{
						FirstDerivedWrong = Piece;
						break;
					}
				}

				TestTrue(
					*FString::Printf(
						TEXT("FIXTURE: CASE %s: this file's own reading of the coordinating grid ")
						TEXT("must agree with CorbelCaseTestSupport, or every expectation below is ")
						TEXT("wrong in the same way — %d bricks against the fixture's %d, and ")
						TEXT("piece %d is the first to disagree (%s against %s)"),
						Row.Label, Derived.Num(), Fixture.Boxes.Num(), FirstDerivedWrong,
						FirstDerivedWrong == INDEX_NONE
							? TEXT("-")
							: *CorbelBuilderVectorBits(Derived[FirstDerivedWrong].CentreCm),
						FirstDerivedWrong == INDEX_NONE
							? TEXT("-")
							: *CorbelBuilderVectorBits(
								Fixture.Boxes[FirstDerivedWrong].CentreCm)),
					Derived.Num() == Fixture.Boxes.Num() && FirstDerivedWrong == INDEX_NONE);
			}
			else
			{
				AddError(FString::Printf(
					TEXT("FIXTURE: CASE %s must build through CorbelCaseTestSupport, or this ")
					TEXT("file's derivation is unarbitrated"),
					Row.Label));
			}
		}

		FBrickLayout Laid;

		if (!DestructionCorbel::Build(Spec, Laid))
		{
			AddError(FString::Printf(
				TEXT("CASE %s: a %d-step corbel on a %d-cell base must lay — production cannot ")
				TEXT("stand up a scenario it cannot build"),
				Row.Label, Row.Steps, Row.BaseCells));

			continue;
		}

		const TArray<FCorbelBuilderBrick> Expected = CorbelBuilderExpectedBricks(Spec);

		TestTrue(
			*FString::Printf(
				TEXT("CASE %s: the closed form must agree with the brick-by-brick derivation — %d ")
				TEXT("against %d, and the row's own literal is %d"),
				Row.Label, CorbelBuilderExpectedPieces(Spec), Expected.Num(), Row.ExpectedPieces),
			CorbelBuilderExpectedPieces(Spec) == Expected.Num()
				&& Expected.Num() == Row.ExpectedPieces);

		TestTrue(
			*FString::Printf(
				TEXT("CASE %s: must lay %d pieces (%d courses of base %d wide, then %d stepped ")
				TEXT("courses %s); it laid %d"),
				Row.Label, Row.ExpectedPieces, Spec.BaseCourses, Spec.BaseCells, Spec.Steps,
				Spec.bFilled ? TEXT("filled") : TEXT("of one brick each"),
				Laid.Structure.NumPieces()),
			Laid.Structure.NumPieces() == Row.ExpectedPieces);

		TestTrue(
			*FString::Printf(
				TEXT("CASE %s: must hand back one box per piece — %d boxes for %d pieces"),
				Row.Label, Laid.Boxes.Num(), Laid.Structure.NumPieces()),
			Laid.Boxes.Num() == Laid.Structure.NumPieces());

		/* The first disagreement, not all of them. A 519-piece corbel laid one cell out would
		 * otherwise print five hundred failures and bury every other row in this file. */
		int32 FirstWrongPiece = INDEX_NONE;
		FString WhyWrong;

		const int32 Common = FMath::Min(Laid.Boxes.Num(), Expected.Num());

		for (int32 Piece = 0; Piece < Common; ++Piece)
		{
			const FPieceBox& Box = Laid.Boxes[Piece];
			const FCorbelBuilderBrick& Want = Expected[Piece];

			const double MassKg = Laid.Structure.GetPiece(Piece).MassKg;
			const double WantMassKg = CorbelBuilderMassKg(Want.HalfExtentCm);

			if (!(Box.CentreCm == Want.CentreCm))
			{
				WhyWrong = FString::Printf(
					TEXT("it is centred at %s and the grid puts it at %s"),
					*CorbelBuilderVectorBits(Box.CentreCm),
					*CorbelBuilderVectorBits(Want.CentreCm));
			}
			else if (!(Box.ExtentCm == Want.HalfExtentCm))
			{
				WhyWrong = FString::Printf(
					TEXT("its half-extent is %s and a brick of this scale is %s"),
					*CorbelBuilderVectorBits(Box.ExtentCm),
					*CorbelBuilderVectorBits(Want.HalfExtentCm));
			}
			else if (Laid.Structure.GetPiece(Piece).bIsGrounded != Want.bGrounded)
			{
				WhyWrong = FString::Printf(
					TEXT("it is %s and the %s must be %s — the base is IMMOVABLE, which is what ")
					TEXT("makes the root joint the only failure available"),
					Laid.Structure.GetPiece(Piece).bIsGrounded
						? TEXT("grounded") : TEXT("not grounded"),
					Want.bGrounded ? TEXT("base") : TEXT("arm"),
					Want.bGrounded ? TEXT("grounded") : TEXT("free"));
			}
			else if (!(FMath::Abs(MassKg - WantMassKg) <= 1.0e-12 * WantMassKg))
			{
				WhyWrong = FString::Printf(
					TEXT("it weighs %s kg and %s cm3 of clay at %s g/cm3 is %s kg"),
					*CorbelBuilderBits(MassKg),
					*CorbelBuilderBits(
						8.0 * Want.HalfExtentCm.X * Want.HalfExtentCm.Y * Want.HalfExtentCm.Z),
					*CorbelBuilderBits(CorbelBuilderDensityGramsPerCubicCm),
					*CorbelBuilderBits(WantMassKg));
			}

			if (!WhyWrong.IsEmpty())
			{
				FirstWrongPiece = Piece;
				break;
			}
		}

		TestTrue(
			*FString::Printf(
				TEXT("CASE %s: every brick must sit where the 22.5 x 11.25 x 7.5 coordinating grid ")
				TEXT("puts it, weighing what its own volume weighs, with the base grounded and the ")
				TEXT("arm free — piece %d is the first that does not: %s"),
				Row.Label, FirstWrongPiece,
				FirstWrongPiece == INDEX_NONE ? TEXT("none is") : *WhyWrong),
			FirstWrongPiece == INDEX_NONE);

		/* And the structure knows where everything is. Without complete geometry every moment in
		 * the solver is silently zero, and a corbel with no moments stands however far it steps —
		 * a confident, plausible, entirely wrong answer. */
		TestTrue(
			*FString::Printf(
				TEXT("CASE %s: the laid corbel must know where every piece and every joint is, or ")
				TEXT("every moment it reads is silently zero"),
				Row.Label),
			Laid.Structure.HasCompleteGeometry());

		/* --- and the root joint is a bed joint under the arm's lowest outermost brick -------- */

		int32 SeatPiece = INDEX_NONE;
		int32 ArmPiece = INDEX_NONE;

		const int32 RootJoint = CorbelBuilderRootJoint(Spec, Laid, SeatPiece, ArmPiece);

		TestTrue(
			*FString::Printf(
				TEXT("CASE %s: the arm's lowest outermost brick (piece %d) must stand on the base's ")
				TEXT("top-course outermost brick (piece %d) through a real joint; the joint is %d"),
				Row.Label, ArmPiece, SeatPiece, RootJoint),
			RootJoint != INDEX_NONE);

		if (RootJoint == INDEX_NONE)
		{
			continue;
		}

		TestEqual(
			*FString::Printf(
				TEXT("CASE %s: the root must be a BED joint beneath the arm, or every reading taken ")
				TEXT("there is about a different mechanism"),
				Row.Label),
			Laid.Structure.GetJointRole(RootJoint, ArmPiece),
			EJointRole::BedBeneath);

		Laid.Structure.SolveLoads();

		AddInfo(FString::Printf(
			TEXT("CASE %s: %d pieces, %d joints; the root joint (%d, between pieces %d and %d) ")
			TEXT("reads %s"),
			Row.Label, Laid.Structure.NumPieces(), Laid.Structure.NumConnections(), RootJoint,
			SeatPiece, ArmPiece,
			*CorbelBuilderBits(Laid.Structure.GetConnectionUtilisation(RootJoint))));
	}

	return true;
}

/**
 * Every joint the grid implies, in order, and the number the root one then reads.
 *
 * WHAT IS ASSERTED, AND WHY THE BRICK TEST ABOVE CANNOT SUBSTITUTE FOR IT. The bricks and the
 * joints are two separate decisions the producer makes, and only the second carries load. Offer
 * `MakeInterface` one course's bed joints fewer and not a single brick moves: `LaysTheFamilyOnItsGrid`
 * stays green, `HasCompleteGeometry` stays green, the root is still a `BedBeneath` joint, and every
 * utilisation in the corbel family changes. The suite's other corbel claims are ordinal — a
 * crossover at 36 steps, a ladder that increases — so a shift that keeps the family's shape hides
 * inside them. So the connection set is compared element for element, in order: pairing, normal,
 * area, centre, half-extent, exact `==`, plus the count — exact rather than tolerant because the
 * claim is that production emits this set of joints and not one near it.
 *
 * THE ORACLE IS DERIVED THE OTHER WAY ROUND. `CorbelBuilderExpectedJoints` offers every pair of
 * this file's own derived bricks to `MakeInterface` and keeps what it accepts — "two bricks that
 * share a face are joined" — where production walks courses and offers only pairs within one and
 * between adjacent ones. An oracle that repeated production's walk would be worth nothing; this one
 * is a different statement that happens to have the same answer, and `Core/Corbel.cpp` says in as
 * many words that its restriction is a cost bound rather than a rule. That sentence is under test.
 *
 * AND ONE ABSOLUTE READING PER CASE. `Row.ExpectedRootUtilisation` is an anchor and is documented
 * as one — a number somebody wrote down, from `LEVELS.md` and the scenario reports, that a
 * joint-set change would move even if it preserved every ordinal claim in the suite. Case A's is
 * the one `COMPOSITE_DEPTH_DESIGN.md` derives by hand and is the only one with a provenance outside
 * a previous run.
 *
 * The bare arm runs here too. Case A is one brick per stepped course — a different load path, a
 * different joint set, and nothing else in the suite asserts either.
 *
 * Needs no ticking world: boxes, doubles and one arithmetic solve.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorbelBuilderLaysTheJointsTest,
	"DestructionGame.Core.Corbel.LaysTheJointsTheGridImplies",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCorbelBuilderLaysTheJointsTest::RunTest(const FString& Parameters)
{
	using namespace CorbelBuilderTestSupport;

	for (const FCorbelBuilderRow& Row : CorbelBuilderRows)
	{
		const DestructionCorbel::FCorbelSpec Spec = CorbelBuilderSpecOf(Row);

		FBrickLayout Laid;

		if (!DestructionCorbel::Build(Spec, Laid))
		{
			AddError(FString::Printf(
				TEXT("CASE %s: a %d-step corbel on a %d-cell base must lay before anything can be ")
				TEXT("asked about its joints"),
				Row.Label, Row.Steps, Row.BaseCells));

			continue;
		}

		const TArray<FConnection> Expected =
			CorbelBuilderExpectedJoints(Spec, CorbelBuilderExpectedBricks(Spec));

		TestTrue(
			*FString::Printf(
				TEXT("CASE %s: every pair of these %d bricks that shares a face is a joint, which is ")
				TEXT("%d of them; production laid %d"),
				Row.Label, Row.ExpectedPieces, Expected.Num(), Laid.Structure.NumConnections()),
			Laid.Structure.NumConnections() == Expected.Num());

		/* The first disagreement, not all of them. A corbel that stopped bedding one course would
		 * otherwise print a thousand failures and bury every other row in this file. */
		int32 FirstWrongJoint = INDEX_NONE;
		FString WhyWrong;

		const int32 Common = FMath::Min(Laid.Structure.NumConnections(), Expected.Num());

		for (int32 Joint = 0; Joint < Common; ++Joint)
		{
			const FConnection& LaidJoint = Laid.Structure.GetConnection(Joint);
			const FConnection& Want = Expected[Joint];

			const bool bSame = LaidJoint.PieceA == Want.PieceA
				&& LaidJoint.PieceB == Want.PieceB
				&& LaidJoint.InterfaceNormal == Want.InterfaceNormal
				&& LaidJoint.InterfaceAreaSqCm == Want.InterfaceAreaSqCm
				&& LaidJoint.InterfaceCentreCm == Want.InterfaceCentreCm
				&& LaidJoint.InterfaceHalfExtentCm == Want.InterfaceHalfExtentCm;

			if (!bSame)
			{
				FirstWrongJoint = Joint;

				WhyWrong = FString::Printf(
					TEXT("production joins %d-%d, normal %s, %s cm2, centred %s, half-extent %s; the ")
					TEXT("grid joins %d-%d, normal %s, %s cm2, centred %s, half-extent %s"),
					LaidJoint.PieceA, LaidJoint.PieceB,
					*CorbelBuilderVectorBits(LaidJoint.InterfaceNormal),
					*CorbelBuilderBits(LaidJoint.InterfaceAreaSqCm),
					*CorbelBuilderVectorBits(LaidJoint.InterfaceCentreCm),
					*CorbelBuilderVectorBits(LaidJoint.InterfaceHalfExtentCm),
					Want.PieceA, Want.PieceB, *CorbelBuilderVectorBits(Want.InterfaceNormal),
					*CorbelBuilderBits(Want.InterfaceAreaSqCm),
					*CorbelBuilderVectorBits(Want.InterfaceCentreCm),
					*CorbelBuilderVectorBits(Want.InterfaceHalfExtentCm));

				break;
			}
		}

		TestTrue(
			*FString::Printf(
				TEXT("CASE %s: the connection set must be the one the coordinating grid implies, IN ")
				TEXT("ORDER — joint %d is the first that is not: %s"),
				Row.Label, FirstWrongJoint,
				FirstWrongJoint == INDEX_NONE ? TEXT("none is") : *WhyWrong),
			FirstWrongJoint == INDEX_NONE);

		/* --- and what those joints then read where the corbel can actually fail -------------- */

		int32 SeatPiece = INDEX_NONE;
		int32 ArmPiece = INDEX_NONE;

		const int32 RootJoint = CorbelBuilderRootJoint(Spec, Laid, SeatPiece, ArmPiece);

		if (RootJoint == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("CASE %s: there must be a bed joint under the arm's lowest outermost brick ")
				TEXT("(piece %d, on piece %d) for a reading to be taken at"),
				Row.Label, ArmPiece, SeatPiece));

			continue;
		}

		Laid.Structure.SolveLoads();

		const double Reading = Laid.Structure.GetConnectionUtilisation(RootJoint);

		TestTrue(
			*FString::Printf(
				TEXT("CASE %s: the root joint (%d, between pieces %d and %d) must read %s and reads ")
				TEXT("%s. This is an ANCHOR: every other corbel claim in the suite is ordinal, so a ")
				TEXT("joint set that changed while keeping the family's shape would satisfy all of ")
				TEXT("them and move this."),
				Row.Label, RootJoint, SeatPiece, ArmPiece,
				*CorbelBuilderBits(Row.ExpectedRootUtilisation), *CorbelBuilderBits(Reading)),
			Reading == Row.ExpectedRootUtilisation);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
