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
 * THE CORBEL BUILDER IN PRODUCTION LAYS EXACTLY THE STRUCTURE EVERY CORBEL READING WAS TAKEN ON.
 *
 * =====================================================================================
 * WHY THIS FILE EXISTS AT ALL
 * =====================================================================================
 *
 * `Core.Structure.CorbelStepsBeforeTensionWins`, `AHundredStepCorbelMustComeDown`,
 * `ACorbelReadsLinearlyInScale`, `ACorbelOrdersByItsProfileNumbers` and
 * `ACorbelReadsItsOwnStepSize` all reach `DestructionCorbel::Build` through
 * `Tests/CorbelCaseTestSupport.h`, and their expected values are worked to fifteen digits. A
 * builder that shifted one brick by an ulp — or that offered `MakeInterface` one PAIR of bricks
 * fewer — would move every one of those readings at once, and it would look like a solver
 * regression rather than like a producer edit.
 *
 * So this file pins the producer itself: same piece count, same boxes, same masses, same grounded
 * flags, same connection set in order, and the same number at the root.
 *
 * =====================================================================================
 * TWO TESTS, AND THE SECOND IS NOT A RESTATEMENT OF THE FIRST
 * =====================================================================================
 *
 * `LaysTheFamilyOnItsGrid` derives every BRICK from the coordinating grid — where it goes, how big
 * it is, what it weighs, whether it stands on the earth — and holds production against that.
 *
 * `LaysTheJointsTheGridImplies` derives every JOINT from those same derived bricks, by a rule
 * production does not use. EVERY PAIR of bricks in the structure is offered to `MakeInterface`,
 * and whatever it accepts is a joint; production offers only pairs within a course and between
 * ADJACENT courses, which its own comment calls a cost bound rather than a second rule. The two
 * therefore agree exactly if and only if that claim holds, and a loop that quietly stops offering
 * one course's bed joints is a disagreement rather than a cheaper way to the same answer.
 *
 * A CORBEL WHOSE BRICKS ARE ALL IN THE RIGHT PLACE CAN STILL BE JOINED WRONGLY, and the joints
 * are what carry the load: piece geometry is untouched by such a change, so the grid test stays
 * green, `HasCompleteGeometry` stays green, the root is still a bed joint, and every reading in
 * the suite moves. That is the failure this second test exists for.
 *
 * =====================================================================================
 * AND ONE ABSOLUTE NUMBER PER CASE, BECAUSE THE REST OF THE SUITE'S CORBEL CLAIMS ARE ORDINAL
 * =====================================================================================
 *
 * The corbel expectations elsewhere are RELATIONS — a crossover at 36 steps, a strictly increasing
 * ladder, one profile ordering above another. A joint set that changed but kept its SHAPE could
 * move every absolute reading in the family while every ordinal claim still held. So each row
 * carries the utilisation its root joint reads, pinned exactly. Case A's 0.15612870000000001 is
 * the one `COMPOSITE_DEPTH_DESIGN.md` works out by hand — bearing 10.25 cm, `F = 4`, `M = 90`,
 * `e = 22.5`, composite over four courses — and the rest are the readings the levels catalogue and
 * the scenario reports publish. They are ANCHORS, not derivations, and they say so.
 *
 * =====================================================================================
 * THE BARE ARM IS COVERED HERE AND NOWHERE ELSE
 * =====================================================================================
 *
 * Case A — a stepped arm of SINGLE bricks — is a scenario row whose only other appearance is
 * inside `Tests/CorbelScreenshotTest.cpp`, in a `.cpp` no other test can include. `bFilled` is
 * the whole of the difference: one brick per stepped course instead of every cell inboard of it,
 * which is a different load path rather than a thinner version of the same one. Both tests below
 * run it.
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
	 * EVERY JOINT THOSE BRICKS IMPLY, FOUND BY A RULE PRODUCTION DOES NOT USE.
	 *
	 * THE RULE IS "ANY TWO BRICKS THAT SHARE A FACE ARE JOINED", spelled as every unordered pair
	 * in the structure offered to `MakeInterface`. `Core/Corbel.cpp` instead offers only pairs
	 * within one course and between ADJACENT courses, and its comment is explicit that this is a
	 * COST BOUND and not a second rule — bricks two courses or two cells apart are separated by
	 * more than the joint thickness and `MakeInterface` would refuse them anyway. That claim is
	 * exactly what this derivation puts under test: if it is true the two sets are identical, and
	 * if the loop ever stops offering a course's beds the sets differ by those beds.
	 *
	 * `MakeInterface` ITSELF IS SHARED ON PURPOSE, and it is the one thing here that is not
	 * independent. Areas, normals, centres and rectangles are that function's answers, asserted in
	 * `Core.Layout.*` against hand-worked faces; what is derived here is WHICH PAIRS are joined and
	 * IN WHAT ORDER, which is the corbel producer's own decision and the thing nothing else pins.
	 *
	 * THE ORDER IS PART OF THE CLAIM, for the reason the brick order is. Handles ascend
	 * course-major and then by X — `CorbelBuilderExpectedBricks` derives that and
	 * `LaysTheFamilyOnItsGrid` asserts it — so ascending A then ascending B is the same sequence
	 * production's course-by-course walk emits, reached without knowing what a course is. A joint
	 * HANDLE is what a break stamp and every cascade reading are expressed in, so a set that is
	 * right but renumbered is not the same structure.
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
		 * THE ROOT JOINT'S UTILISATION AFTER A SOLVE — AN ANCHOR, NOT A DERIVATION.
		 *
		 * Every other corbel claim in the suite is ORDINAL: a crossover at 36 steps, a ladder that
		 * increases, one profile above another. A joint set that changed while keeping the family's
		 * SHAPE would move all of these absolute numbers and satisfy every ordinal claim on the way
		 * down. So each is written out to seventeen digits and compared exactly. `LEVELS.md` and
		 * the scenario reports publish the same figures to five, and case A's is the one
		 * `COMPOSITE_DEPTH_DESIGN.md` derives by hand.
		 */
		double ExpectedRootUtilisation;
	};

	/**
	 * THE FAMILY, AND IT IS THE ONE THE SEVEN CATALOGUE ROWS ARE MADE OF.
	 *
	 * A to D are the structures the user reviewed; E35 and E36 straddle the crossover
	 * `Core.Structure.CorbelStepsBeforeTensionWins` locates at 36 steps. F (a hundred steps,
	 * 3,015 bricks) is deliberately ABSENT: it is the largest structure in the suite,
	 * `AHundredStepCorbelMustComeDown` already lays and cascades it, and the joint derivation below
	 * is quadratic in the piece count — nine million candidate pairs for one row that would tell
	 * this file nothing E36's five hundred bricks do not.
	 */
	/*
	 * MEAN RE-ANCHOR (2026-08-13): every root here is tension-governed, so each anchor is
	 * the old characteristic-basis measurement divided by 7 (f_x1 0.10 -> 0.70; the stress
	 * side is statics). The rows are pinned with exact ==, and production divides the
	 * stress by 0.7 directly, so the red phase wrote each as `old / 7.0` and instructed the
	 * green phase to re-pin the measured bits wherever that landed an ulp off. MEASURED AT
	 * THE FLIP (2026-08-14): rows A, C and D each read one ulp above their division (the
	 * literal below is the measured value; the trailing comment keeps the old expression),
	 * while B, E35 and E36 came back bit-identical to theirs and keep the division form.
	 * One ulp there is rounding-path, not physics. E35/E36 no longer straddle 1.0
	 * (~0.1415 / ~0.1452 at the mean basis; the crossover moved to ~124 steps in
	 * compression — see CorbelStepsBeforeTensionWins and the owed replacement pair in
	 * CURRENT_STATE); their labels keep the history.
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
 * EVERY BRICK OF EVERY CASE, WHERE THE COORDINATING GRID SAYS IT GOES.
 *
 * THE CLAIM IS INDEPENDENT OF THE FIXTURE; THE TEST IS NOT ENTIRELY. What is asserted about
 * production is asserted against `CorbelBuilderExpectedBricks`, a second reading of
 * `claude_plans/CORBEL_CASES.html` written in this file and owing nothing to
 * `CorbelCaseTestSupport.h`. But the FIXTURE-PRECONDITION block below arbitrates that derivation
 * against the fixture before using it, and the fixture is now a call to production — so the
 * precondition compares two runs of one builder and can no longer fail. It is retained as the
 * statement of what the derivation is answerable to, and it carries no weight: delete it and every
 * assertion in this test says exactly what it says today.
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
		 * AND IT NO LONGER ARBITRATES ANYTHING, because `CorbelCaseTestSupport::CorbelBuild` is now
		 * a call to `DestructionCorbel::Build` plus indexing. It was the thing that said the red
		 * below was red for the right reason while the builder was being moved into production; it
		 * is kept as the written statement of what this file's derivation is answerable to. The
		 * bare arm has no fixture and is skipped; only its fill differs, and one brick per course
		 * needs no arbitration.
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
 * EVERY JOINT THE GRID IMPLIES, IN ORDER, AND THE NUMBER THE ROOT ONE THEN READS.
 *
 * =====================================================================================
 * WHAT IS ASSERTED, AND WHY THE BRICK TEST ABOVE CANNOT SUBSTITUTE FOR IT
 * =====================================================================================
 *
 * The bricks and the joints are two separate decisions the producer makes, and only the second
 * one carries load. Offer `MakeInterface` one course's bed joints fewer and not a single brick
 * moves: `LaysTheFamilyOnItsGrid` stays green, `HasCompleteGeometry` stays green, the root is
 * still a `BedBeneath` joint, and every utilisation in the corbel family changes. The suite's
 * other corbel claims are ordinal — a crossover at 36 steps, a ladder that increases — so a shift
 * that keeps the family's shape hides inside them.
 *
 * SO THE CONNECTION SET IS COMPARED ELEMENT FOR ELEMENT, IN ORDER: pairing, normal, area, centre,
 * half-extent, exact `==`, plus the count. Exact rather than tolerant because the claim is that
 * production emits THIS set of joints and not one near it, and a tolerance admits precisely the
 * drift this exists to refuse.
 *
 * THE ORACLE IS DERIVED THE OTHER WAY ROUND. `CorbelBuilderExpectedJoints` offers EVERY pair of
 * this file's own derived bricks to `MakeInterface` and keeps what it accepts — "two bricks that
 * share a face are joined" — where production walks courses and offers only pairs within one and
 * between adjacent ones. An oracle that repeated production's walk would be worth nothing; this
 * one is a different statement that happens to have the same answer, and `Core/Corbel.cpp` says in
 * as many words that its restriction is a cost bound rather than a rule. That sentence is the
 * thing under test.
 *
 * =====================================================================================
 * AND ONE ABSOLUTE READING PER CASE
 * =====================================================================================
 *
 * `Row.ExpectedRootUtilisation` is an ANCHOR and is documented as one — a number somebody wrote
 * down, from `LEVELS.md` and the scenario reports, that a joint-set change would move even if it
 * preserved every ordinal claim in the suite. Case A's is the one `COMPOSITE_DEPTH_DESIGN.md`
 * derives by hand and is the only one with a provenance outside a previous run.
 *
 * THE BARE ARM RUNS HERE TOO. Case A is one brick per stepped course — a different load path, a
 * different joint set, and nothing else in the suite asserts either.
 *
 * NEEDS A TICKING WORLD: NO. Boxes, doubles and one arithmetic solve.
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

		/*
		 * THE FIRST DISAGREEMENT, NOT ALL OF THEM. A corbel that stopped bedding one course would
		 * otherwise print a thousand failures and bury every other row in this file.
		 */
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
