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
 * Pins DestructionCorbel::Build, which many corbel tests reach through CorbelCaseTestSupport.h
 * with values to fifteen digits: a builder change would look like a solver regression.
 *
 * LaysTheFamilyOnItsGrid derives every brick from the grid. LaysTheJointsTheGridImplies derives
 * every joint by offering every pair to MakeInterface, where production offers only
 * within-course and adjacent-course pairs; they agree only if that restriction is just a cost
 * bound. Each row also pins one absolute root utilisation, since the suite's other corbel
 * claims are ordinal. Covers the bare arm (case A). No ticking world.
 */
namespace CorbelBuilderTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	// The grid from first principles (DESIGN.md brick, 1 cm joint), not production constants.
	constexpr double CorbelBuilderBrickLengthCm = 21.5;
	constexpr double CorbelBuilderBrickWidthCm = 10.25;
	constexpr double CorbelBuilderBrickHeightCm = 6.5;
	constexpr double CorbelBuilderMortarCm = 1.0;
	constexpr double CorbelBuilderDensityGramsPerCubicCm = 1.9;

	constexpr double CorbelBuilderCellPitchCm =
		CorbelBuilderBrickLengthCm + CorbelBuilderMortarCm;

	constexpr double CorbelBuilderCoursePitchCm =
		CorbelBuilderBrickHeightCm + CorbelBuilderMortarCm;

	constexpr double CorbelBuilderHalfCellStepCm = CorbelBuilderCellPitchCm / 2.0;

	/** Case D's origin: three cells left of case C's, so both roots sit at the same X. */
	constexpr double CorbelBuilderCounterweightOriginCm = -3.0 * CorbelBuilderCellPitchCm;

	inline FString CorbelBuilderBits(double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	inline FString CorbelBuilderVectorBits(const FVector& Value)
	{
		return FString::Printf(TEXT("(%.17g, %.17g, %.17g)"), Value.X, Value.Y, Value.Z);
	}

	/** Brick mass, kg, computed independently of PieceMassKg (compare with a relative tolerance). */
	inline double CorbelBuilderMassKg(const FVector& HalfExtentCm)
	{
		const double VolumeCubicCm =
			(2.0 * HalfExtentCm.X) * (2.0 * HalfExtentCm.Y) * (2.0 * HalfExtentCm.Z);

		return VolumeCubicCm * CorbelBuilderDensityGramsPerCubicCm / 1000.0;
	}

	/** One expected brick. */
	struct FCorbelBuilderBrick
	{
		FVector CentreCm = FVector::ZeroVector;
		FVector HalfExtentCm = FVector::ZeroVector;
		bool bGrounded = false;
	};

	/**
	 * Every brick the spec describes, from the drawing (CORBEL_CASES.html). Order is part of the
	 * claim (base bottom-up, then arm, each course left to right), because handles identify joints
	 * and readings.
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

			// The nudge stops an exact division (11.25 into 22.5) flooring one cell short on the last bit.
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
	 * Every joint those bricks imply: every pair offered to MakeInterface, unlike Corbel.cpp's
	 * within/adjacent-course walk. MakeInterface itself is shared (tested in Core.Layout.*); what
	 * is derived here is which pairs join, and in what order.
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
	 * Piece count in closed form: base BaseCourses x BaseCells, plus one per step (bare) or
	 * k x BaseCells + floor(k^2 / 4) (filled, half-cell step).
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

	/** The piece centred here, or INDEX_NONE. Used to find the root joint by geometry, not handle. */
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

	/** The bed joint under the arm's lowest outermost brick, or INDEX_NONE; reports its two pieces. */
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

	/** One row of the family. */
	struct FCorbelBuilderRow
	{
		const TCHAR* Label;
		int32 BaseCells;
		double LeftOriginCm;
		int32 Steps;
		bool bFilled;
		int32 ExpectedPieces;

		/**
		 * Root utilisation after a solve: a measured anchor, compared exactly. Case A's matches
		 * COMPOSITE_DEPTH_DESIGN.md's hand figure.
		 */
		double ExpectedRootUtilisation;
	};

	/**
	 * The catalogue's corbel rows except F, whose 3,015 bricks make the all-pairs joint
	 * derivation too slow and add nothing E36 does not.
	 */
	/*
	 * Mean re-anchor (2026-08-13): all roots are tension-governed, so each anchor is the old
	 * characteristic value / 7 (f_x1 0.10 -> 0.70). A, C and D measured one ulp above that
	 * division, so their literals are the measured bits. E35/E36 no longer straddle 1.0; the
	 * crossover moved to ~124 steps (CURRENT_STATE).
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

	/** The same row as the test fixture's spec. Filled rows only. */
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
 * Every brick of every case is where the grid puts it, with the right mass and grounding,
 * checked against this file's own CorbelBuilderExpectedBricks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCorbelBuilderLaysTheGridTest,
	"DestructionGame.Core.Corbel.LaysTheFamilyOnItsGrid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCorbelBuilderLaysTheGridTest::RunTest(const FString& Parameters)
{
	using namespace CorbelBuilderTestSupport;

	// Every mass below assumes 1.9 g/cm3.
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
		 * Fixture precondition: the derivation must match CorbelCaseTestSupport. That fixture now
		 * calls production, so this can no longer fail; it is kept as a statement of what the
		 * derivation answers to. The bare arm has no fixture.
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

		// Report only the first disagreement, not hundreds.
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

		// Without complete geometry every moment is silently zero and any corbel stands.
		TestTrue(
			*FString::Printf(
				TEXT("CASE %s: the laid corbel must know where every piece and every joint is, or ")
				TEXT("every moment it reads is silently zero"),
				Row.Label),
			Laid.Structure.HasCompleteGeometry());

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
 * Every joint the grid implies, in order, and the root's reading. Missing one course's beds moves
 * no brick, so the grid test cannot catch it, but it changes every reading. Joints are compared
 * exactly (pairing, normal, area, centre, half-extent) against the all-pairs oracle, then the
 * root utilisation against its anchor.
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

		// Report only the first disagreement.
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
