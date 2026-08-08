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
 * THE CORBEL BUILDER IN PRODUCTION LAYS EXACTLY THE STRUCTURE THE FIXTURE HAS BEEN MEASURED ON.
 *
 * =====================================================================================
 * WHY THIS TEST EXISTS AT ALL
 * =====================================================================================
 *
 * `Tests/CorbelCaseTestSupport.h` builds the corbel family and is TEST-ONLY, so no level can
 * reach it — which is why the seven corbel scenarios cannot be catalogue rows until the builder
 * is production. Moving it is the cheap part. The expensive part is that
 * `Core.Structure.CorbelStepsBeforeTensionWins`, `AHundredStepCorbelMustComeDown`,
 * `ACorbelReadsLinearlyInScale`, `ACorbelOrdersByItsProfileNumbers` and
 * `ACorbelReadsItsOwnStepSize` all read that fixture and their expected values are worked to
 * fifteen digits. A move that shifted one brick by an ulp would change every one of them, and
 * would look like a solver regression rather than like a fixture edit.
 *
 * So this pins the move: same piece count, same boxes, same masses, same grounded flags, same
 * connection set, same root-joint utilisation. The fixture header's own rule — "a second copy of
 * a corbel definition is two fixtures that drift" — is the whole reason the header must end up
 * INCLUDING this builder rather than keeping its copy.
 *
 * =====================================================================================
 * TWO HALVES, AND ONLY ONE OF THEM SURVIVES THE MOVE
 * =====================================================================================
 *
 * PART ONE derives every box INDEPENDENTLY, from the coordinating grid, and never looks at the
 * fixture at all. It keeps biting forever, including after `CorbelCaseTestSupport.h` has been
 * reduced to a `#include` of production — at which point PART TWO is comparing a thing with
 * itself and proves nothing. That is a known and accepted property of the sequencing rather than
 * an oversight: part two is the RED STEP that gates the move, part one is the standing net.
 *
 * PART TWO is the bit-for-bit comparison against the fixture as it stands today. It is the
 * assertion that matters at the moment of the move and the reason to run this test before
 * touching the header.
 *
 * =====================================================================================
 * THE BARE ARM HAS NO FIXTURE TO COMPARE AGAINST, AND THAT IS WHY IT IS HERE
 * =====================================================================================
 *
 * Case A — a stepped arm of SINGLE bricks — is deliberately absent from
 * `CorbelCaseTestSupport.h` and lives only inside `Tests/CorbelScreenshotTest.cpp`, in a `.cpp`
 * no other test can include. It is a scenario row, so production has to be able to lay it, and
 * part one is the only thing that can say whether it laid it right. `bFilled` is what separates
 * the two: one brick per stepped course instead of every cell inboard of it.
 *
 * NEEDS A TICKING WORLD: NO. Boxes, doubles and one arithmetic solve.
 */
namespace CorbelBuilderTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * --- the grid, spelled out here rather than imported -----------------------------------
	 *
	 * DESIGN.md's standard UK metric clay brick and the 1 cm mortar joint that makes the
	 * coordinating grid 22.5 x 11.25 x 7.5. Written from first principles for the reason the
	 * fixture header gives: a test that reaches for production's own constant agrees with a
	 * wrong one instead of failing.
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
	 * WHAT ONE BRICK WEIGHS, MULTIPLIED IN THE OTHER ORDER ON PURPOSE.
	 *
	 * Volume first and density last, where `DestructionLayout::PieceMassKg` does it the other way
	 * round — so this is an independent computation rather than the same one twice, and it is
	 * compared with a relative tolerance because that order costs an ulp. Part two compares the
	 * masses EXACTLY, against the fixture.
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
	 * EVERY BRICK THE SPEC DESCRIBES, DERIVED FROM THE PICTURE RATHER THAN FROM THE BUILDER.
	 *
	 * `claude_plans/CORBEL_CASES.html` draws the family; the generalisation that expresses it for
	 * any step size is: the base is `BaseCells` cells per course with alternate courses shifted
	 * one step, and the arm's OUTERMOST brick advances one step per course with as many whole
	 * cells inboard of it as fit before the base's left edge. At the half-cell step the two agree
	 * brick for brick, which `Core.Structure.CorbelStepsBeforeTensionWins` already asserts
	 * against the drawing's own `{2, 3, 3, 4, 4, 5, 5, 6, 6, 7}`.
	 *
	 * THE ORDER IS PART OF THE CLAIM. Base courses bottom-up, then arm courses bottom-up, each
	 * course left to right — because a piece HANDLE is the identity the joints, the break stamps
	 * and every fixture reading are expressed in, and a builder that laid the same bricks in a
	 * different order would renumber all of them while every geometric assertion still passed.
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
			 * A HALF-OPEN CELL COUNT. Several step sizes divide the cell pitch exactly — 11.25
			 * goes into 22.5 twice — so the quotient lands on a whole number and a bare floor is
			 * one cell either way depending on the last bit. Nudging UP keeps the leftmost brick
			 * inboard of the base's own left edge, which is the direction that never invents
			 * masonry that is not in the drawing.
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
	 * THE PIECE COUNT IN CLOSED FORM, so a row's literal below is checkable rather than recorded.
	 *
	 * The base is `BaseCourses x BaseCells`. A bare arm adds one brick per step. A FILLED arm at
	 * the half-cell step adds `BaseCells + floor(i/2)` on step i — the outer face advances half a
	 * cell each course, so a whole new cell appears every second one — and summing that from 1 to
	 * k gives `k x BaseCells + floor(k^2 / 4)`.
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
	 * THE ROOT JOINT, FOUND BY GEOMETRY AND NEVER BY A HANDLE THE BUILDER HANDED BACK.
	 *
	 * It is the bed joint under the arm's lowest outermost brick — the one place a corbel on an
	 * immovable base can fail, because a rigid body cannot rotate about a fixed base without
	 * separating from it. Both ends are located by their CENTRES, worked out from the grid, so
	 * this test can read the same joint out of two structures whose handle numbering it has not
	 * yet proved identical.
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

	/** One row of the family, and the piece count its own arithmetic predicts. */
	struct FCorbelBuilderRow
	{
		const TCHAR* Label;
		int32 BaseCells;
		double LeftOriginCm;
		int32 Steps;
		bool bFilled;
		int32 ExpectedPieces;
	};

	/**
	 * THE FAMILY, AND IT IS THE ONE THE SEVEN CATALOGUE ROWS ARE MADE OF.
	 *
	 * A to D are the structures the user reviewed; E35 and E36 straddle the crossover
	 * `Core.Structure.CorbelStepsBeforeTensionWins` locates at 36 steps. F (a hundred steps,
	 * 3,015 bricks) is deliberately ABSENT: it is the largest structure in the suite,
	 * `AHundredStepCorbelMustComeDown` already lays and cascades it, and nothing this file
	 * asserts would be truer for having built it a second time.
	 */
	const FCorbelBuilderRow CorbelBuilderRows[] =
	{
		{ TEXT("A: bare stepped arm, four single bricks"),
			2, 0.0, 4, false, 10 },

		{ TEXT("B: the same four-step profile filled solid"),
			2, 0.0, 4, true, 18 },

		{ TEXT("C: filled, ten steps, on the bare two-cell base"),
			2, 0.0, 10, true, 51 },

		{ TEXT("D: case C plus three cells of masonry opposite"),
			5, CorbelBuilderCounterweightOriginCm, 10, true, 90 },

		{ TEXT("E35: the last corbel the model says stands"),
			5, CorbelBuilderCounterweightOriginCm, 35, true, 496 },

		{ TEXT("E36: the first whose root joint reads over 1.0"),
			5, CorbelBuilderCounterweightOriginCm, 36, true, 519 },
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
 * PART ONE: every brick of every case, where the coordinating grid says it goes.
 *
 * Independent of the fixture entirely, which is what makes it the half that keeps biting once
 * `CorbelCaseTestSupport.h` has been reduced to an include of production.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorbelBuilderLaysTheGridTest,
	"DestructionGame.Core.Corbel.LaysTheFamilyOnItsGrid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCorbelBuilderLaysTheGridTest::RunTest(const FString& Parameters)
{
	using namespace CorbelBuilderTestSupport;

	/*
	 * THE FIXTURE'S OWN PREMISE, ASSERTED RATHER THAN IMPORTED. Every mass below is derived
	 * against 1.9 g/cm3, so a profile that moved would make them all quietly wrong.
	 */
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
		 * THE DERIVATION IS CHECKED AGAINST THE FIXTURE BEFORE IT IS USED AS AN EXPECTATION, and
		 * this block is a FIXTURE PRECONDITION rather than the claim.
		 *
		 * Everything below holds production against `CorbelBuilderExpectedBricks`, which is a
		 * second reading of `claude_plans/CORBEL_CASES.html` written in this file — and a wrong
		 * expectation is worse than no test, because it sends whoever is implementing the builder
		 * hunting a defect that is in the test. The fixture the whole solver suite already
		 * measures is standing right there, so the derivation is held against it and any
		 * disagreement is reported as a fixture fault in this file rather than as a defect in the
		 * builder.
		 *
		 * IT IS GREEN BEFORE THE BUILDER EXISTS, WHICH IS THE POINT: it is the thing that says
		 * the red below is red for the right reason. The bare arm has no fixture and is skipped;
		 * only its fill differs, and one brick per course needs no arbitration.
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

		/*
		 * THE FIRST DISAGREEMENT, NOT ALL OF THEM. A 519-piece corbel laid one cell out would
		 * otherwise print five hundred failures and bury every other row in this file.
		 */
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

		/*
		 * AND THE STRUCTURE KNOWS WHERE EVERYTHING IS. Without complete geometry every moment in
		 * the solver is silently zero, and a corbel with no moments stands however far it steps —
		 * a confident, plausible, entirely wrong answer.
		 */
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
 * PART TWO: BIT FOR BIT AGAINST THE FIXTURE EVERY EXISTING CORBEL READING WAS TAKEN ON.
 *
 * THIS IS THE ASSERTION THAT GATES THE MOVE. `Core.Structure.*` and the E-to-J family read
 * `CorbelCaseTestSupport::CorbelBuild` and their expected values are worked to fifteen digits —
 * 2.68132 for the hundred-step arm, a crossover at exactly 36 steps, a ladder of five step sizes
 * whose ORDER is the claim. Every one of those is a statement about a particular arrangement of
 * particular bricks, so a production builder that differs from the fixture by one ulp anywhere is
 * not "a fixture that moved", it is five tests changing their answers at once for no visible
 * reason.
 *
 * WHAT IS COMPARED: the piece count, every box centre and extent, every mass, every grounded
 * flag, the whole connection set in ORDER — pairing, normal, area, centre and half-extent — and
 * the root joint's utilisation after a solve. Exact `==` throughout, because "bit-identical" is
 * the claim and a tolerance would let exactly the drift this exists to refuse through.
 *
 * THE BARE ARM IS ABSENT because the fixture header deliberately does not build one; part one is
 * what covers case A.
 *
 * AND IT GOES QUIET AFTER THE MOVE. Once `CorbelCaseTestSupport.h` includes production instead of
 * carrying its own copy, this compares a thing with itself. That is the intended end state — the
 * value is spent at the moment of the move — and part one is what remains.
 *
 * NEEDS A TICKING WORLD: NO.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorbelBuilderMatchesTheFixtureTest,
	"DestructionGame.Core.Corbel.MatchesTheFixtureBitForBit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCorbelBuilderMatchesTheFixtureTest::RunTest(const FString& Parameters)
{
	using namespace CorbelBuilderTestSupport;

	for (const FCorbelBuilderRow& Row : CorbelBuilderRows)
	{
		if (!Row.bFilled)
		{
			continue;
		}

		const DestructionCorbel::FCorbelSpec Spec = CorbelBuilderSpecOf(Row);

		FBrickLayout Laid;

		if (!DestructionCorbel::Build(Spec, Laid))
		{
			AddError(FString::Printf(
				TEXT("CASE %s: production must lay this corbel before it can be compared to the ")
				TEXT("fixture every existing reading was taken on"),
				Row.Label));

			continue;
		}

		CorbelCaseTestSupport::FCorbelStructure Fixture;

		if (!CorbelCaseTestSupport::CorbelBuild(CorbelBuilderFixtureSpecOf(Row), Fixture))
		{
			AddError(FString::Printf(
				TEXT("FIXTURE: CASE %s must build through CorbelCaseTestSupport too, or there is ")
				TEXT("nothing to compare against"),
				Row.Label));

			continue;
		}

		TestTrue(
			*FString::Printf(
				TEXT("CASE %s: production laid %d pieces and %d joints; the fixture lays %d and %d"),
				Row.Label, Laid.Structure.NumPieces(), Laid.Structure.NumConnections(),
				Fixture.Structure.NumPieces(), Fixture.Structure.NumConnections()),
			Laid.Structure.NumPieces() == Fixture.Structure.NumPieces()
				&& Laid.Structure.NumConnections() == Fixture.Structure.NumConnections());

		/* --- every piece, first disagreement only ------------------------------------------- */

		int32 FirstWrongPiece = INDEX_NONE;
		FString WhyPieceWrong;

		const int32 CommonPieces = FMath::Min(Laid.Boxes.Num(), Fixture.Boxes.Num());

		for (int32 Piece = 0; Piece < CommonPieces; ++Piece)
		{
			const FPieceBox& Mine = Laid.Boxes[Piece];
			const FPieceBox& Theirs = Fixture.Boxes[Piece];

			const bool bSame = Mine.CentreCm == Theirs.CentreCm
				&& Mine.ExtentCm == Theirs.ExtentCm
				&& Laid.Structure.GetPiece(Piece).MassKg == Fixture.Structure.GetPiece(Piece).MassKg
				&& Laid.Structure.GetPiece(Piece).bIsGrounded
					== Fixture.Structure.GetPiece(Piece).bIsGrounded;

			if (!bSame)
			{
				FirstWrongPiece = Piece;

				WhyPieceWrong = FString::Printf(
					TEXT("production has it at %s, half-extent %s, %s kg, %s; the fixture has it at ")
					TEXT("%s, half-extent %s, %s kg, %s"),
					*CorbelBuilderVectorBits(Mine.CentreCm),
					*CorbelBuilderVectorBits(Mine.ExtentCm),
					*CorbelBuilderBits(Laid.Structure.GetPiece(Piece).MassKg),
					Laid.Structure.GetPiece(Piece).bIsGrounded ? TEXT("grounded") : TEXT("free"),
					*CorbelBuilderVectorBits(Theirs.CentreCm),
					*CorbelBuilderVectorBits(Theirs.ExtentCm),
					*CorbelBuilderBits(Fixture.Structure.GetPiece(Piece).MassKg),
					Fixture.Structure.GetPiece(Piece).bIsGrounded ? TEXT("grounded") : TEXT("free"));

				break;
			}
		}

		TestTrue(
			*FString::Printf(
				TEXT("CASE %s: every piece must be BIT-IDENTICAL to the fixture's — piece %d is the ")
				TEXT("first that is not: %s"),
				Row.Label, FirstWrongPiece,
				FirstWrongPiece == INDEX_NONE ? TEXT("none is") : *WhyPieceWrong),
			FirstWrongPiece == INDEX_NONE);

		/* --- and every joint, in order ------------------------------------------------------ */

		int32 FirstWrongJoint = INDEX_NONE;
		FString WhyJointWrong;

		const int32 CommonJoints =
			FMath::Min(Laid.Structure.NumConnections(), Fixture.Structure.NumConnections());

		for (int32 Joint = 0; Joint < CommonJoints; ++Joint)
		{
			const FConnection& Mine = Laid.Structure.GetConnection(Joint);
			const FConnection& Theirs = Fixture.Structure.GetConnection(Joint);

			const bool bSame = Mine.PieceA == Theirs.PieceA
				&& Mine.PieceB == Theirs.PieceB
				&& Mine.InterfaceNormal == Theirs.InterfaceNormal
				&& Mine.InterfaceAreaSqCm == Theirs.InterfaceAreaSqCm
				&& Mine.InterfaceCentreCm == Theirs.InterfaceCentreCm
				&& Mine.InterfaceHalfExtentCm == Theirs.InterfaceHalfExtentCm;

			if (!bSame)
			{
				FirstWrongJoint = Joint;

				WhyJointWrong = FString::Printf(
					TEXT("production joins %d-%d, normal %s, %s cm2, centred %s, half-extent %s; the ")
					TEXT("fixture joins %d-%d, normal %s, %s cm2, centred %s, half-extent %s"),
					Mine.PieceA, Mine.PieceB, *CorbelBuilderVectorBits(Mine.InterfaceNormal),
					*CorbelBuilderBits(Mine.InterfaceAreaSqCm),
					*CorbelBuilderVectorBits(Mine.InterfaceCentreCm),
					*CorbelBuilderVectorBits(Mine.InterfaceHalfExtentCm),
					Theirs.PieceA, Theirs.PieceB, *CorbelBuilderVectorBits(Theirs.InterfaceNormal),
					*CorbelBuilderBits(Theirs.InterfaceAreaSqCm),
					*CorbelBuilderVectorBits(Theirs.InterfaceCentreCm),
					*CorbelBuilderVectorBits(Theirs.InterfaceHalfExtentCm));

				break;
			}
		}

		TestTrue(
			*FString::Printf(
				TEXT("CASE %s: the connection set must be the fixture's, IN ORDER — joint %d is the ")
				TEXT("first that is not: %s"),
				Row.Label, FirstWrongJoint,
				FirstWrongJoint == INDEX_NONE ? TEXT("none is") : *WhyJointWrong),
			FirstWrongJoint == INDEX_NONE);

		/* --- and the one number every corbel test in the suite is about ---------------------- */

		int32 SeatPiece = INDEX_NONE;
		int32 ArmPiece = INDEX_NONE;

		const int32 RootJoint = CorbelBuilderRootJoint(Spec, Laid, SeatPiece, ArmPiece);

		if (RootJoint == INDEX_NONE || Fixture.RootJoint == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("CASE %s: both structures must present a root joint to compare — production ")
				TEXT("gave %d, the fixture gave %d"),
				Row.Label, RootJoint, Fixture.RootJoint));

			continue;
		}

		Laid.Structure.SolveLoads();
		Fixture.Structure.SolveLoads();

		const double Mine = Laid.Structure.GetConnectionUtilisation(RootJoint);
		const double Theirs = Fixture.Structure.GetConnectionUtilisation(Fixture.RootJoint);

		TestTrue(
			*FString::Printf(
				TEXT("CASE %s: THE ROOT JOINT MUST READ THE SAME NUMBER — production reads %s at ")
				TEXT("joint %d, the fixture reads %s at joint %d. Every corbel expectation in this ")
				TEXT("suite is a statement about that number."),
				Row.Label, *CorbelBuilderBits(Mine), RootJoint, *CorbelBuilderBits(Theirs),
				Fixture.RootJoint),
			Mine == Theirs);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
