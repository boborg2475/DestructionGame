// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"
#include "Core/WallCases.h"
#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Validation set for the rigid-block oracle (DESIGN.md §7 step 3, slice 1). Every expected
 * lambda* is hand-worked limit analysis from the brick, the profile figures and an
 * independently derived unit conversion; nothing is transcribed from the oracle or production.
 *
 * Derivation pattern: a joint's two contact normal forces are fixed by equilibrium of the body
 * above (n1 + n2 = V, (n2 - n1) * h = M, h the contact half-spacing), so
 *
 *     n_inboard  = V/2 - M/(2h)   >= -f_t*Conv*A/2  (tension)
 *     n_outboard = V/2 + M/(2h)   <=  f_c*Conv*A/2  (crushing)
 *
 * For a single-column chain these are the only admissible force systems, so the answer is exact.
 *
 * Classes pinned:
 *   - Tipping: resultant inside the bearing; a resultant exactly at the edge stands (closed set).
 *   - Sliding: Mohr-Coulomb rows; the mortar row embeds the 10000 uu conversion, so a 100x
 *     slip fails it.
 *   - Crushing: f_c*Conv*A/W centred, exactly half at a knife edge.
 *   - Finite tension: the acceptance lean (10 cm/course) stands at 5/8 courses and falls at
 *     30/40; the same geometry dry at 5 courses has lambda* = 0.
 *   - One-cell jamming: a half-seated dry brick alone falls. With an abutting head joint a
 *     feasible system exists (seat edge carries W, head top contact n_h = 0.5*W/7.0, friction
 *     at 10.2%), so the oracle stands it. This deliberately disagrees with production's
 *     one-cell gate, which measures thrust at the head joint's centroid (arm 3.75 cm) rather
 *     than its edge (7.0 cm).
 *
 * Every problem is solved twice and must be bit-identical with the same pivot count. No
 * ticking world. Named namespace because of unity builds.
 */
namespace RigidBlockOracleTestSupport
{
	using namespace RigidBlockOracle;

	// The brick and the units, derived here.

	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickWidthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;
	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** Density-first multiplication order (the PieceMassKg contract); 2.72163125 kg. */
	constexpr double BrickMassKg =
		ClayDensityGramsPerCubicCm * BrickLengthCm * BrickWidthCm * BrickHeightCm / 1000.0;

	constexpr double GravityHere = 980.0;

	/** 2667.198625 uu. */
	constexpr double BrickWeightUu = BrickMassKg * GravityHere;

	/** 1 N = 100 uu, 1 cm2 = 100 mm2. Derived independently of production and the oracle. */
	constexpr double ConvHere = 100.0 * 100.0;

	/** Standard mortared coursing: 6.5 brick + 1.0 bed. */
	constexpr double CoursePitchCm = 7.5;

	/** Full bed 220.375 cm2; running-bond half seat 105.0625 cm2; head face 66.625 cm2. */
	constexpr double FullBedAreaSqCm = BrickLengthCm * BrickWidthCm;
	constexpr double HalfSeatAreaSqCm = 10.25 * BrickWidthCm;
	constexpr double HeadAreaSqCm = BrickHeightCm * BrickWidthCm;

	// Problem builders. Geometry is laid here, never read back from the oracle.

	/**
	 * A grounded brick with one on top, the top centroid offset sideways. The bed is centred on
	 * the base: offset 0 is centred, 10.75 is the knife edge, beyond is an unholdable overhang.
	 */
	FOracleProblem TwoBlockTower(
		double TopCentroidOffsetXCm, const FConnectionStrength& Strength,
		double BedHalfLengthCm = BrickLengthCm / 2.0)
	{
		FOracleProblem Problem;

		Problem.Blocks.Add({ BrickMassKg, 0.0, BrickHeightCm / 2.0, /*bGrounded*/ true });
		Problem.Blocks.Add({ BrickMassKg, TopCentroidOffsetXCm, BrickHeightCm * 1.5, false });

		FOracleJoint Bed;
		Bed.BlockA = 0;
		Bed.BlockB = 1;
		Bed.NormalX = 0.0;
		Bed.NormalZ = 1.0;
		Bed.CentreXCm = 0.0;
		Bed.CentreZCm = BrickHeightCm;
		Bed.HalfLengthCm = BedHalfLengthCm;
		Bed.AreaSqCm = FullBedAreaSqCm;
		Bed.Strength = Strength;
		Problem.Joints.Add(Bed);

		return Problem;
	}

	/**
	 * Sliding: gravity dead, a live horizontal push at bed level (no overturning couple), so
	 * lambda* = (c*Conv*A + mu*W) / Push exactly.
	 */
	FOracleProblem SlidingProblem(const FConnectionStrength& Strength, double PushUu)
	{
		FOracleProblem Problem = TwoBlockTower(0.0, Strength);
		Problem.bGravityIsLive = false;

		FOracleAppliedForce Push;
		Push.Block = 1;
		Push.ForceXUu = PushUu;
		Push.ForceZUu = 0.0;
		Push.AtXCm = 0.0;
		Push.AtZCm = BrickHeightCm;
		Push.bLive = true;
		Problem.AppliedForces.Add(Push);

		return Problem;
	}

	/** Leaning stack: one column, each course offset a fixed distance, base grounded. */
	FOracleProblem LeaningStackProblem(
		int32 Courses, double OffsetPerCourseCm, const FConnectionStrength& Strength)
	{
		FOracleProblem Problem;

		for (int32 Course = 0; Course < Courses; ++Course)
		{
			Problem.Blocks.Add({
				BrickMassKg,
				double(Course) * OffsetPerCourseCm,
				BrickHeightCm / 2.0 + double(Course) * CoursePitchCm,
				/*bGrounded*/ Course == 0 });
		}

		const double OverlapCm = BrickLengthCm - OffsetPerCourseCm;

		for (int32 Lower = 0; Lower + 1 < Courses; ++Lower)
		{
			FOracleJoint Bed;
			Bed.BlockA = Lower;
			Bed.BlockB = Lower + 1;
			Bed.NormalX = 0.0;
			Bed.NormalZ = 1.0;
			Bed.CentreXCm = double(Lower) * OffsetPerCourseCm + OffsetPerCourseCm / 2.0;
			Bed.CentreZCm = double(Lower) * CoursePitchCm + (BrickHeightCm + CoursePitchCm) / 2.0;
			Bed.HalfLengthCm = OverlapCm / 2.0;
			Bed.AreaSqCm = OverlapCm * BrickWidthCm;
			Bed.Strength = Strength;
			Problem.Joints.Add(Bed);
		}

		return Problem;
	}

	/**
	 * One-cell geometry, 1 cm joints: grounded seat G, half-seated brick P (centroid 5.625 cm
	 * outboard of its 10.25 cm seat), and optionally neighbour N on grounded G2, met through a
	 * head joint. Matches the bridge test's FStructure fixture exactly.
	 */
	FOracleProblem JammingProblem(bool bWithNeighbour, const FConnectionStrength& Strength)
	{
		FOracleProblem Problem;

		/* G: spans x in [-11.25, 10.25]. P: spans [0, 21.5], one course up. */
		Problem.Blocks.Add({ BrickMassKg, -0.5, BrickHeightCm / 2.0, /*bGrounded*/ true });
		Problem.Blocks.Add({ BrickMassKg, 10.75, BrickHeightCm / 2.0 + CoursePitchCm, false });

		FOracleJoint Seat;
		Seat.BlockA = 0;
		Seat.BlockB = 1;
		Seat.NormalX = 0.0;
		Seat.NormalZ = 1.0;
		Seat.CentreXCm = 5.125;
		Seat.CentreZCm = BrickHeightCm + 0.5;
		Seat.HalfLengthCm = 5.125;
		Seat.AreaSqCm = HalfSeatAreaSqCm;
		Seat.Strength = Strength;
		Problem.Joints.Add(Seat);

		if (bWithNeighbour)
		{
			/* N: spans [22.5, 44] beside P; G2 the same footprint, grounded, below it. */
			Problem.Blocks.Add({ BrickMassKg, 33.25, BrickHeightCm / 2.0 + CoursePitchCm, false });
			Problem.Blocks.Add({ BrickMassKg, 33.25, BrickHeightCm / 2.0, /*bGrounded*/ true });

			FOracleJoint Head;
			Head.BlockA = 1;
			Head.BlockB = 2;
			Head.NormalX = 1.0;
			Head.NormalZ = 0.0;
			Head.CentreXCm = 22.0;
			Head.CentreZCm = CoursePitchCm + BrickHeightCm / 2.0;
			Head.HalfLengthCm = BrickHeightCm / 2.0;
			Head.AreaSqCm = HeadAreaSqCm;
			Head.Strength = Strength;
			Problem.Joints.Add(Head);

			FOracleJoint NeighbourSeat;
			NeighbourSeat.BlockA = 3;
			NeighbourSeat.BlockB = 2;
			NeighbourSeat.NormalX = 0.0;
			NeighbourSeat.NormalZ = 1.0;
			NeighbourSeat.CentreXCm = 33.25;
			NeighbourSeat.CentreZCm = BrickHeightCm + 0.5;
			NeighbourSeat.HalfLengthCm = BrickLengthCm / 2.0;
			NeighbourSeat.AreaSqCm = FullBedAreaSqCm;
			NeighbourSeat.Strength = Strength;
			Problem.Joints.Add(NeighbourSeat);
		}

		return Problem;
	}

	// Hand-worked closed forms: the header's derivation pattern applied to each geometry.

	/** Centred block: crushing binds both contacts, f_c*Conv*A / W. */
	double CentredCrushLambda(const FConnectionStrength& Strength)
	{
		return Strength.CompressiveStrengthMPa * ConvHere * FullBedAreaSqCm / BrickWeightUu;
	}

	/** Pushed block at dead self-weight: lambda* = (c*Conv*A + mu*W) / Push, exactly. */
	double SlidingLambda(const FConnectionStrength& Strength, double PushUu)
	{
		return (Strength.ShearCohesionMPa * ConvHere * FullBedAreaSqCm
			+ Strength.FrictionCoefficient * BrickWeightUu) / PushUu;
	}

	/**
	 * Bottom-joint tension bound, m = Courses - 1 bricks above: resultant m*W at d*m/2 outboard,
	 * so n_inboard = lambda * W * (m/2 - d*m^2/(4h)). Higher joints have smaller m, so the
	 * bottom governs.
	 */
	double StackTensionLambda(
		const FConnectionStrength& Strength, int32 Courses, double OffsetPerCourseCm)
	{
		const double m = double(Courses - 1);
		const double h = (BrickLengthCm - OffsetPerCourseCm) / 2.0;
		const double AreaSqCm = (BrickLengthCm - OffsetPerCourseCm) * BrickWidthCm;
		const double TensionCapacityUu = Strength.TensileStrengthMPa * ConvHere * AreaSqCm / 2.0;

		const double DemandPerLambda =
			BrickWeightUu * (OffsetPerCourseCm * m * m / (4.0 * h) - m / 2.0);

		return TensionCapacityUu / DemandPerLambda;
	}

	/** Same statics, crushing at the outboard contact: n_outboard = lambda * W * (m/2 + d*m^2/(4h)). */
	double StackCrushLambda(
		const FConnectionStrength& Strength, int32 Courses, double OffsetPerCourseCm)
	{
		const double m = double(Courses - 1);
		const double h = (BrickLengthCm - OffsetPerCourseCm) / 2.0;
		const double AreaSqCm = (BrickLengthCm - OffsetPerCourseCm) * BrickWidthCm;
		const double CrushCapacityUu = Strength.CompressiveStrengthMPa * ConvHere * AreaSqCm / 2.0;

		const double DemandPerLambda =
			BrickWeightUu * (m / 2.0 + OffsetPerCourseCm * m * m / (4.0 * h));

		return CrushCapacityUu / DemandPerLambda;
	}

	/**
	 * Half-seated brick alone, mortared: weight at e = 5.625 cm, contacts at +-5.125, so
	 * lambda* = T_A / (W * (e/(2h) - 1/2)), about 2826 at the mean 0.70 bond.
	 */
	double JammingAloneTensionLambda(const FConnectionStrength& Strength)
	{
		const double e = 5.625;
		const double h = 5.125;
		const double TensionCapacityUu =
			Strength.TensileStrengthMPa * ConvHere * HalfSeatAreaSqCm / 2.0;

		return TensionCapacityUu / (BrickWeightUu * (e / (2.0 * h) - 0.5));
	}

	/**
	 * Same statics, crushing at the outboard contact: lambda* = C_A / (W * (1/2 + e/(2h))),
	 * about 1878 for mortar. The fixture's answer is exactly min(tension, crush); which governs
	 * flipped at the 2026-08-14 mean re-anchor.
	 */
	double JammingAloneCrushLambda(const FConnectionStrength& Strength)
	{
		const double e = 5.625;
		const double h = 5.125;
		const double CrushCapacityUu =
			Strength.CompressiveStrengthMPa * ConvHere * HalfSeatAreaSqCm / 2.0;

		return CrushCapacityUu / (BrickWeightUu * (0.5 + e / (2.0 * h)));
	}

	struct FOracleValidationRow
	{
		const TCHAR* Name = nullptr;

		/** The one-line hand derivation, printed beside any failure. */
		const TCHAR* Because = nullptr;

		TFunction<FOracleProblem()> Build;

		/** Negative means assert the floor only (no closed form). */
		double ExpectedLambda = -1.0;
		double LambdaFloor = 0.0;

		double AbsTol = 0.0;
		double RelTol = 0.0;

		EOracleOutcome Expected = EOracleOutcome::Unanswerable;
	};

	/** Solve one row, assert everything a row promises, and record lambda by name. */
	void RunValidationRow(
		FAutomationTestBase& Test, const FOracleValidationRow& Row,
		TMap<FString, double>& OutLambdas)
	{
		const FOracleProblem Problem = Row.Build();
		const FOracleResult Result = SolveRigidBlock(Problem);

		Test.AddInfo(FString::Printf(
			TEXT("%s: lambda %.12g (expected %.12g), answered %d, %d pivots"),
			Row.Name, Result.Lambda, Row.ExpectedLambda, Result.bAnswered ? 1 : 0,
			Result.SimplexIterations));

		if (!Test.TestTrue(
				*FString::Printf(
					TEXT("%s: the oracle must answer (it said: %s). %s"),
					Row.Name, *Result.WhyNot, Row.Because),
				Result.bAnswered))
		{
			return;
		}

		Test.TestTrue(
			*FString::Printf(TEXT("%s: lambda must be finite"), Row.Name),
			FMath::IsFinite(Result.Lambda));

		if (Row.ExpectedLambda >= 0.0)
		{
			const double Tolerance = Row.AbsTol + Row.RelTol * FMath::Abs(Row.ExpectedLambda);

			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: lambda* must be %.12g within %.3g and was %.12g. %s"),
					Row.Name, Row.ExpectedLambda, Tolerance, Result.Lambda, Row.Because),
				FMath::Abs(Result.Lambda - Row.ExpectedLambda) <= Tolerance);
		}
		else
		{
			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: lambda* must be at least %.12g and was %.12g. %s"),
					Row.Name, Row.LambdaFloor, Result.Lambda, Row.Because),
				Result.Lambda >= Row.LambdaFloor);
		}

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: outcome must be %d and was %d (lambda %.12g)"),
				Row.Name, int32(Row.Expected), int32(OutcomeOf(Result)), Result.Lambda),
			OutcomeOf(Result) == Row.Expected);

		const FOracleResult Again = SolveRigidBlock(Row.Build());

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: DETERMINISM — two solves must agree to the last bit ")
				TEXT("(%.17g vs %.17g, %d vs %d pivots)"),
				Row.Name, Result.Lambda, Again.Lambda,
				Result.SimplexIterations, Again.SimplexIterations),
			Result.Lambda == Again.Lambda
				&& Result.bAnswered == Again.bAnswered
				&& Result.SimplexIterations == Again.SimplexIterations);

		OutLambdas.Add(Row.Name, Result.Lambda);
	}
}

/** Twenty hand-worked limit-analysis answers the oracle must reproduce. See the file header. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockOracleValidationTest,
	"DestructionGame.Oracle.RigidBlock.ValidationCatalogue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockOracleValidationTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace RigidBlockOracle;
	using namespace RigidBlockOracleTestSupport;

	// Profile figures the hand answers derive from; a retune must fail here first.
	TestEqual(TEXT("FIXTURE: dry stone has exactly no cohesion"),
		DryStone.ShearCohesionMPa, 0.0);
	TestEqual(TEXT("FIXTURE: dry stone has exactly no tensile bond"),
		DryStone.TensileStrengthMPa, 0.0);
	TestEqual(TEXT("FIXTURE: dry stone's friction is 0.7"),
		DryStone.FrictionCoefficient, 0.7);
	TestEqual(TEXT("FIXTURE: dry stone crushes at 30 MPa"),
		DryStone.CompressiveStrengthMPa, 30.0);
	// Mortar is on the mean basis since the 2026-08-13 re-anchor (Gooch et al.; DESIGN §3).
	TestEqual(TEXT("FIXTURE: mortar's bond is the mean f_x1 = 0.70 (re-anchor 2026-08-13)"),
		GeneralPurposeMortar.TensileStrengthMPa, 0.7);
	TestEqual(TEXT("FIXTURE: mortar's cohesion is the mean f_v0 = 0.90"),
		GeneralPurposeMortar.ShearCohesionMPa, 0.9);
	TestEqual(TEXT("FIXTURE: mortar's friction is the mean 0.75"),
		GeneralPurposeMortar.FrictionCoefficient, 0.75);
	TestEqual(TEXT("FIXTURE: mortar crushes at M10's 10 MPa (already a declared mean)"),
		GeneralPurposeMortar.CompressiveStrengthMPa, 10.0);

	// Shear ceilings bind in no row; pinned so a retune cannot silently move a hand answer.
	TestEqual(TEXT("FIXTURE: mortar's shear ceiling is the mean-basis 2.0 MPa"),
		GeneralPurposeMortar.MaxShearStrengthMPa, 2.0);
	TestEqual(TEXT("FIXTURE: dry stone's shear ceiling is 6.0 MPa"),
		DryStone.MaxShearStrengthMPa, 6.0);

	TestEqual(TEXT("FIXTURE: the standard brick weighs 2.72163125 kg"),
		BrickMassKg, 2.72163125);

	TArray<FOracleValidationRow> Rows;

	// Tipping and crushing: single blocks.

	Rows.Add({ TEXT("centred dry block"),
		TEXT("centred: n splits evenly, crushing binds, lambda* = f_c*Conv*A/W"),
		[] { return TwoBlockTower(0.0, DestructionProfiles::DryStone); },
		CentredCrushLambda(DryStone), 0.0, 0.0, 1.0e-6, EOracleOutcome::Stands });

	Rows.Add({ TEXT("centred unbreakable block"),
		TEXT("no reachable strength bound, so lambda* must report the cap, exactly"),
		[] { return TwoBlockTower(0.0, DestructionProfiles::Unbreakable); },
		LambdaCap, 0.0, 0.0, 1.0e-9, EOracleOutcome::Stands });

	Rows.Add({ TEXT("centred dry block, point contact"),
		TEXT("zero half-length: both contacts coincide; a centred load needs no moment, ")
		TEXT("so lambda* equals the centred crushing bound unchanged"),
		[] { return TwoBlockTower(0.0, DestructionProfiles::DryStone, 0.0); },
		CentredCrushLambda(DryStone), 0.0, 0.0, 1.0e-6, EOracleOutcome::Stands });

	Rows.Add({ TEXT("knife edge dry block"),
		TEXT("resultant exactly AT the bearing edge: feasible (closed constraint set), ")
		TEXT("one contact carries everything, lambda* = half the centred bound"),
		[] { return TwoBlockTower(BrickLengthCm / 2.0, DestructionProfiles::DryStone); },
		CentredCrushLambda(DryStone) / 2.0, 0.0, 0.0, 1.0e-6, EOracleOutcome::Stands });

	Rows.Add({ TEXT("overhung dry block"),
		TEXT("centroid 1 cm past the bearing edge: no no-tension force system exists at ")
		TEXT("any positive load, lambda* = 0"),
		[] { return TwoBlockTower(BrickLengthCm / 2.0 + 1.0, DestructionProfiles::DryStone); },
		0.0, 0.0, 1.0e-6, 0.0, EOracleOutcome::Falls });

	// Sliding.

	Rows.Add({ TEXT("pushed dry block"),
		TEXT("dead weight W, live push W at bed level: slides at exactly mu = 0.7"),
		[] { return SlidingProblem(DestructionProfiles::DryStone, BrickWeightUu); },
		0.7, 0.0, 1.0e-9, 1.0e-9, EOracleOutcome::Falls });

	Rows.Add({ TEXT("pushed mortared block"),
		TEXT("lambda* = (c*Conv*A + mu*W)/W ~ 744.4 at the mean c = 0.9, mu = 0.75 — the row ")
		TEXT("that embeds the 10000 uu/MPa/cm2 conversion, so a factor-of-100 unit slip fails ")
		TEXT("it outright"),
		[] { return SlidingProblem(DestructionProfiles::GeneralPurposeMortar, BrickWeightUu); },
		SlidingLambda(GeneralPurposeMortar, BrickWeightUu), 0.0, 0.0, 1.0e-6,
		EOracleOutcome::Stands });

	/*
	 * No-tension ladder, dry stack, 2 cm lean. Feasible iff m/2 >= d*m^2/(4h), i.e.
	 * m <= 2h/d = 9.75 bricks above: 5 and 8 courses stand, 30 and 40 cannot.
	 */
	Rows.Add({ TEXT("dry stack 2 cm x 5 courses"),
		TEXT("m = 4 <= 9.75: stands; crushing at the outboard contact sets lambda*"),
		[] { return LeaningStackProblem(5, 2.0, DestructionProfiles::DryStone); },
		StackCrushLambda(DryStone, 5, 2.0), 0.0, 0.0, 1.0e-6, EOracleOutcome::Stands });

	Rows.Add({ TEXT("dry stack 2 cm x 8 courses"),
		TEXT("m = 7 <= 9.75: stands; crushing at the outboard contact sets lambda*"),
		[] { return LeaningStackProblem(8, 2.0, DestructionProfiles::DryStone); },
		StackCrushLambda(DryStone, 8, 2.0), 0.0, 0.0, 1.0e-6, EOracleOutcome::Stands });

	Rows.Add({ TEXT("dry stack 2 cm x 30 courses"),
		TEXT("m = 29 > 9.75: the resultant left the bearing, lambda* = 0"),
		[] { return LeaningStackProblem(30, 2.0, DestructionProfiles::DryStone); },
		0.0, 0.0, 1.0e-6, 0.0, EOracleOutcome::Falls });

	Rows.Add({ TEXT("dry stack 2 cm x 40 courses"),
		TEXT("m = 39 > 9.75: the resultant left the bearing, lambda* = 0"),
		[] { return LeaningStackProblem(40, 2.0, DestructionProfiles::DryStone); },
		0.0, 0.0, 1.0e-6, 0.0, EOracleOutcome::Falls });

	// Finite tension at the acceptance lean, 10 cm per course.

	Rows.Add({ TEXT("mortared stack 10 cm x 5 courses"),
		TEXT("bottom-joint plastic tension bound: lambda* = T_A/(W*(d*m^2/(4h) - m/2)) ")
		TEXT("~ 31.2 at the mean bond — STANDS, agreeing with acceptance case 1"),
		[] { return LeaningStackProblem(5, 10.0, DestructionProfiles::GeneralPurposeMortar); },
		StackTensionLambda(GeneralPurposeMortar, 5, 10.0), 0.0, 0.0, 1.0e-6,
		EOracleOutcome::Stands });

	Rows.Add({ TEXT("mortared stack 10 cm x 8 courses"),
		TEXT("~ 8.69 at the mean bond — STANDS, agreeing with acceptance case 2"),
		[] { return LeaningStackProblem(8, 10.0, DestructionProfiles::GeneralPurposeMortar); },
		StackTensionLambda(GeneralPurposeMortar, 8, 10.0), 0.0, 0.0, 1.0e-6,
		EOracleOutcome::Stands });

	Rows.Add({ TEXT("mortared stack 10 cm x 30 courses"),
		TEXT("~ 0.440 at the mean bond — still FALLS, agreeing with acceptance ")
		TEXT("case 3, which production only reaches via the interim guard"),
		[] { return LeaningStackProblem(30, 10.0, DestructionProfiles::GeneralPurposeMortar); },
		StackTensionLambda(GeneralPurposeMortar, 30, 10.0), 0.0, 0.0, 1.0e-6,
		EOracleOutcome::Falls });

	Rows.Add({ TEXT("mortared stack 10 cm x 40 courses"),
		TEXT("~ 0.241 at the mean bond — FALLS, agreeing with acceptance case 4"),
		[] { return LeaningStackProblem(40, 10.0, DestructionProfiles::GeneralPurposeMortar); },
		StackTensionLambda(GeneralPurposeMortar, 40, 10.0), 0.0, 0.0, 1.0e-6,
		EOracleOutcome::Falls });

	Rows.Add({ TEXT("dry stack at the acceptance lean, 5 courses"),
		TEXT("the same geometry the mortared 5-course row stands on: dry, m = 4 puts ")
		TEXT("the resultant 20 cm out on a 5.75 cm half-bearing, lambda* = 0 — the ")
		TEXT("finite-tension mapping is the entire difference"),
		[] { return LeaningStackProblem(5, 10.0, DestructionProfiles::DryStone); },
		0.0, 0.0, 1.0e-6, 0.0, EOracleOutcome::Falls });

	// One-cell jamming pair.

	Rows.Add({ TEXT("one-cell half seat, dry, with abutment"),
		TEXT("the file header's certificate: seat edge carries W, the head joint's top ")
		TEXT("contact supplies the 0.5 cm couple at a 7 cm arm, friction at 10.2% — a ")
		TEXT("feasible system EXISTS, so the limit theorem stands it (recorded ")
		TEXT("disagreement with production's kern-and-centroid one-cell refusal)"),
		[] { return JammingProblem(true, DestructionProfiles::DryStone); },
		-1.0, 100.0, 0.0, 0.0, EOracleOutcome::Stands });

	Rows.Add({ TEXT("one-cell half seat, dry, no abutment"),
		TEXT("weight 0.5 cm past the bearing edge and nothing to jam against: ")
		TEXT("lambda* = 0"),
		[] { return JammingProblem(false, DestructionProfiles::DryStone); },
		0.0, 0.0, 1.0e-6, 0.0, EOracleOutcome::Falls });

	/*
	 * The governing axis flipped at the 2026-08-14 mean re-anchor (see TRAPS): tension rose to
	 * ~2826, crush stayed ~1878, and the LP answers 1877.92298545. The row asserts the minimum.
	 */
	Rows.Add({ TEXT("one-cell half seat, mortared, no abutment"),
		TEXT("two determinate contacts: lambda* = min(T_A/(W*(e/(2h) - 1/2)) ~ 2826, ")
		TEXT("C_A/(W*(1/2 + e/(2h))) ~ 1878) — crushing governs at the mean bond"),
		[] { return JammingProblem(false, DestructionProfiles::GeneralPurposeMortar); },
		FMath::Min(
			JammingAloneTensionLambda(GeneralPurposeMortar),
			JammingAloneCrushLambda(GeneralPurposeMortar)),
		0.0, 0.0, 1.0e-6,
		EOracleOutcome::Stands });

	/*
	 * Redundant joint: the 8-course stack's bottom joint split into two half-area twins with
	 * identical geometry. Capacities are area-proportional, so the feasible set is unchanged,
	 * but the columns are parallel and the optimum degenerate (where the dense solver's
	 * basic-artificial residue lived). A regression net for degeneracy handling; the pivot-out
	 * pass and the fail-closed verification gate are the primary guards (TRAPS.md).
	 */
	Rows.Add({ TEXT("mortared stack, bottom joint split into half-area twins"),
		TEXT("two half-area twins of one joint are the same constraint set, so ")
		TEXT("lambda* equals the unduplicated 8-course closed form within RelTol — the ")
		TEXT("cross-row check below is what actually pins it equal to the un-duplicated ")
		TEXT("row itself"),
		[]
		{
			FOracleProblem Problem =
				LeaningStackProblem(8, 10.0, DestructionProfiles::GeneralPurposeMortar);

			FOracleJoint Twin = Problem.Joints[0];
			Problem.Joints[0].AreaSqCm /= 2.0;
			Twin.AreaSqCm /= 2.0;
			Problem.Joints.Add(Twin);

			return Problem;
		},
		StackTensionLambda(GeneralPurposeMortar, 8, 10.0), 0.0, 0.0, 1.0e-6,
		EOracleOutcome::Stands });

	TestEqual(TEXT("FIXTURE: the catalogue is twenty rows"), Rows.Num(), 20);

	TMap<FString, double> Lambdas;

	for (const FOracleValidationRow& Row : Rows)
	{
		RunValidationRow(*this, Row, Lambdas);
	}

	// Cross-row relations no single row can fake.
	if (Lambdas.Contains(TEXT("centred dry block")) && Lambdas.Contains(TEXT("knife edge dry block")))
	{
		const double Centred = Lambdas[TEXT("centred dry block")];
		const double Knife = Lambdas[TEXT("knife edge dry block")];

		TestTrue(
			*FString::Printf(
				TEXT("RELATION: a knife edge halves the crushing bound exactly — one contact ")
				TEXT("point carries everything (centred %.12g vs knife %.12g)"),
				Centred, Knife),
			FMath::Abs(Centred - 2.0 * Knife) <= 1.0e-6 * Centred);
	}

	if (Lambdas.Contains(TEXT("mortared stack 10 cm x 8 courses"))
		&& Lambdas.Contains(TEXT("mortared stack, bottom joint split into half-area twins")))
	{
		const double Original = Lambdas[TEXT("mortared stack 10 cm x 8 courses")];
		const double Twins = Lambdas[TEXT("mortared stack, bottom joint split into half-area twins")];

		/*
		 * Equality, not proximity: the twins are the same constraint set. The two pivot paths
		 * land one ulp apart at the mean basis (8.6877701307335329 vs ...311), so 2 ulp is
		 * allowed; the S8 mutation (one twin's area x(1+1e-9)) still fails it.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("RELATION: a redundant joint is the same constraint set as its ")
				TEXT("un-duplicated twin — lambda* must be the SAME NUMBER to within 2 ulp, ")
				TEXT("not merely close (8-course %.17g vs twins %.17g)"),
				Original, Twins),
			FMath::Abs(Original - Twins) <= 4.5e-16 * FMath::Abs(Original));
	}

	if (Lambdas.Contains(TEXT("mortared stack 10 cm x 5 courses"))
		&& Lambdas.Contains(TEXT("dry stack at the acceptance lean, 5 courses")))
	{
		TestTrue(
			TEXT("RELATION: at the acceptance lean the bond is the whole verdict — the ")
			TEXT("mortared 5-course stack stands (lambda* >= 1) while the identical dry ")
			TEXT("geometry has no equilibrium at all (lambda* < 1)"),
			Lambdas[TEXT("mortared stack 10 cm x 5 courses")] >= 1.0
				&& Lambdas[TEXT("dry stack at the acceptance lean, 5 courses")] < 1.0);
	}

	return true;
}

/**
 * BuildRigidBlockProblem must project an FStructure laid through MakeInterface into exactly the
 * hand builders' problem, and refuse anything it cannot represent.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockOracleBridgeTest,
	"DestructionGame.Oracle.RigidBlock.StructureBridge",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockOracleBridgeTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace RigidBlockOracle;
	using namespace RigidBlockOracleTestSupport;

	// The mortared 8-course acceptance stack, through FStructure.
	{
		FStructure Structure;
		TArray<FPieceBox> Boxes;

		for (int32 Course = 0; Course < 8; ++Course)
		{
			FPieceBox Box;
			Box.ExtentCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm) * 0.5;
			Box.CentreCm = FVector(
				double(Course) * 10.0, 0.0,
				BrickHeightCm / 2.0 + double(Course) * CoursePitchCm);

			Structure.AddPiece(BrickMassKg, /*bIsGrounded*/ Course == 0, Box.CentreCm);
			Boxes.Add(Box);
		}

		for (int32 First = 0; First < Boxes.Num(); ++First)
		{
			for (int32 Second = First + 1; Second < Boxes.Num(); ++Second)
			{
				FConnection Joint;

				if (MakeInterface(First, Boxes[First], Second, Boxes[Second],
						1.0, GeneralPurposeMortar, Joint))
				{
					Structure.AddConnection(Joint);
				}
			}
		}

		TestEqual(TEXT("stack: the producer emits one bed joint per course above the base"),
			Structure.NumConnections(), 7);

		// The producer's joints must match the hand builder's, or the bit comparison is meaningless.
		for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
		{
			const FConnection& Joint = Structure.GetConnection(Index);

			TestEqual(*FString::Printf(TEXT("stack joint %d: area is the 11.5 cm overlap"), Index),
				Joint.InterfaceAreaSqCm, 11.5 * BrickWidthCm);
			TestEqual(*FString::Printf(TEXT("stack joint %d: in-plane half extent"), Index),
				Joint.InterfaceHalfExtentCm.X, 5.75);
			TestEqual(*FString::Printf(TEXT("stack joint %d: centre X"), Index),
				Joint.InterfaceCentreCm.X, double(Index) * 10.0 + 5.0);
			TestEqual(*FString::Printf(TEXT("stack joint %d: centre Z"), Index),
				Joint.InterfaceCentreCm.Z, double(Index) * CoursePitchCm + 7.0);
		}

		FOracleProblem Bridged;
		FString WhyNot;

		// Own statement: the message reads WhyNot, which the call writes (TRAPS.md, unsequenced evaluation).
		const bool bBridged = BuildRigidBlockProblem(Structure, Bridged, WhyNot);

		if (!TestTrue(
				*FString::Printf(TEXT("stack: the bridge must represent a fully-placed ")
					TEXT("structure (it said: %s)"), *WhyNot),
				bBridged))
		{
			return true;
		}

		TestEqual(TEXT("stack: eight blocks"), Bridged.Blocks.Num(), 8);
		TestEqual(TEXT("stack: seven joints"), Bridged.Joints.Num(), 7);

		const FOracleResult ViaBridge = SolveRigidBlock(Bridged);
		const FOracleResult ByHand =
			SolveRigidBlock(LeaningStackProblem(8, 10.0, GeneralPurposeMortar));

		AddInfo(FString::Printf(
			TEXT("stack: bridge lambda %.17g, hand lambda %.17g, closed form %.12g"),
			ViaBridge.Lambda, ByHand.Lambda,
			StackTensionLambda(GeneralPurposeMortar, 8, 10.0)));

		TestTrue(TEXT("stack: the bridge must answer"), ViaBridge.bAnswered);
		TestTrue(TEXT("stack: the hand-built twin must answer"), ByHand.bAnswered);

		/*
		 * Every coordinate is a multiple of 0.25, so the problems are identical in binary. TestTrue
		 * on ==, because TestEqual's double overload has a tolerance.
		 */
		TestTrue(TEXT("stack: the bridged and hand-built problems are the same problem, ")
			TEXT("so lambda* must agree to the last bit"),
			ViaBridge.Lambda == ByHand.Lambda);

		const double Expected = StackTensionLambda(GeneralPurposeMortar, 8, 10.0);

		TestTrue(
			*FString::Printf(
				TEXT("stack: lambda* must be the closed form %.12g and was %.12g"),
				Expected, ViaBridge.Lambda),
			FMath::Abs(ViaBridge.Lambda - Expected) <= 1.0e-6 * Expected);

		TestTrue(TEXT("stack: eight mortared courses at the acceptance lean STAND"),
			OutcomeOf(ViaBridge) == EOracleOutcome::Stands);
	}

	// The one-cell jamming pair, through FStructure, latch included.
	{
		FStructure Structure;
		TArray<FPieceBox> Boxes;

		const FVector BrickExtent = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm) * 0.5;

		// G, P, N, G2: the JammingProblem geometry, as boxes.
		const FVector Centres[] = {
			FVector(-0.5, 0.0, BrickHeightCm / 2.0),
			FVector(10.75, 0.0, BrickHeightCm / 2.0 + CoursePitchCm),
			FVector(33.25, 0.0, BrickHeightCm / 2.0 + CoursePitchCm),
			FVector(33.25, 0.0, BrickHeightCm / 2.0),
		};
		const bool Grounded[] = { true, false, false, true };

		for (int32 Piece = 0; Piece < 4; ++Piece)
		{
			FPieceBox Box;
			Box.ExtentCm = BrickExtent;
			Box.CentreCm = Centres[Piece];

			Structure.AddPiece(BrickMassKg, Grounded[Piece], Box.CentreCm);
			Boxes.Add(Box);
		}

		int32 HeadJoint = INDEX_NONE;

		for (int32 First = 0; First < Boxes.Num(); ++First)
		{
			for (int32 Second = First + 1; Second < Boxes.Num(); ++Second)
			{
				FConnection Joint;

				if (MakeInterface(First, Boxes[First], Second, Boxes[Second],
						1.0, DryStone, Joint))
				{
					const int32 Handle = Structure.AddConnection(Joint);

					if (First == 1 && Second == 2)
					{
						HeadJoint = Handle;
					}
				}
			}
		}

		TestEqual(TEXT("jamming: seat, head joint and neighbour seat — three joints"),
			Structure.NumConnections(), 3);
		TestTrue(TEXT("jamming: the head joint exists"), HeadJoint != INDEX_NONE);

		FOracleProblem Bridged;
		FString WhyNot;

		const bool bBridged = BuildRigidBlockProblem(Structure, Bridged, WhyNot);

		if (!TestTrue(
				*FString::Printf(
					TEXT("jamming: the bridge must represent it (it said: %s)"), *WhyNot),
				bBridged))
		{
			return true;
		}

		const FOracleResult WithAbutment = SolveRigidBlock(Bridged);

		AddInfo(FString::Printf(TEXT("jamming via bridge: lambda %.12g"), WithAbutment.Lambda));

		TestTrue(TEXT("jamming: answered"), WithAbutment.bAnswered);
		TestTrue(
			*FString::Printf(
				TEXT("jamming: with its abutment the dry half seat jams and STANDS ")
				TEXT("(lambda %.12g, floor 100)"),
				WithAbutment.Lambda),
			OutcomeOf(WithAbutment) == EOracleOutcome::Stands && WithAbutment.Lambda >= 100.0);

		/*
		 * Removing the neighbour severs its joints; the bridge must skip given and removed parts,
		 * leaving the half seat alone (lambda* = 0). The oracle judges the graph as it stands.
		 */
		TestTrue(TEXT("jamming: the neighbour can be removed"), Structure.RemovePiece(2));

		FOracleProblem AfterRemoval;

		const bool bBridgedAfterRemoval =
			BuildRigidBlockProblem(Structure, AfterRemoval, WhyNot);

		if (!TestTrue(
				*FString::Printf(
					TEXT("jamming: the bridge must represent the damaged graph ")
					TEXT("(it said: %s)"), *WhyNot),
				bBridgedAfterRemoval))
		{
			return true;
		}

		TestEqual(TEXT("jamming: three live blocks after the removal"),
			AfterRemoval.Blocks.Num(), 3);
		TestEqual(TEXT("jamming: only the half seat's own joint survives"),
			AfterRemoval.Joints.Num(), 1);

		const FOracleResult Alone = SolveRigidBlock(AfterRemoval);

		AddInfo(FString::Printf(TEXT("jamming after removal: lambda %.12g"), Alone.Lambda));

		TestTrue(TEXT("jamming: answered after removal"), Alone.bAnswered);
		TestTrue(
			*FString::Printf(
				TEXT("jamming: with the abutment gone the dry half seat has no ")
				TEXT("equilibrium — lambda* must be 0 and was %.12g"),
				Alone.Lambda),
			Alone.Lambda <= 1.0e-6 && OutcomeOf(Alone) == EOracleOutcome::Falls);
	}

	// What the bridge must refuse.
	{
		// An unplaced piece has no centre of mass, so no lever arm.
		FStructure Unplaced;
		Unplaced.AddPiece(BrickMassKg, true);
		Unplaced.AddPiece(BrickMassKg, false);

		FOracleProblem Out;
		FString WhyNot;

		TestTrue(TEXT("bridge: a structure without complete geometry is REFUSED"),
			!BuildRigidBlockProblem(Unplaced, Out, WhyNot));
		TestTrue(TEXT("bridge: the refusal names its reason"), !WhyNot.IsEmpty());
		TestEqual(TEXT("bridge: a refused problem is emptied, so ignoring the return ")
			TEXT("solves nothing"), Out.Blocks.Num(), 0);
	}
	{
		// A Y-normal joint cannot be projected into X-Z.
		FStructure OutOfPlane;
		OutOfPlane.AddPiece(BrickMassKg, true, FVector(0.0, 0.0, 3.25));
		OutOfPlane.AddPiece(BrickMassKg, false, FVector(0.0, 11.25, 3.25));

		FConnection Wythe;
		Wythe.PieceA = 0;
		Wythe.PieceB = 1;
		Wythe.InterfaceNormal = FVector(0.0, 1.0, 0.0);
		Wythe.InterfaceAreaSqCm = 4.0 * 10.75 * 3.25;
		Wythe.InterfaceCentreCm = FVector(0.0, 5.625, 3.25);
		Wythe.InterfaceHalfExtentCm = FVector(10.75, 0.0, 3.25);
		Wythe.Strength = GeneralPurposeMortar;

		TestTrue(TEXT("bridge fixture: the wythe joint is a valid production joint"),
			OutOfPlane.AddConnection(Wythe) != INDEX_NONE);

		FOracleProblem Out;
		FString WhyNot;

		TestTrue(TEXT("bridge: a joint with a Y normal is REFUSED, never projected"),
			!BuildRigidBlockProblem(OutOfPlane, Out, WhyNot));
		TestTrue(TEXT("bridge: the out-of-plane refusal names its reason"), !WhyNot.IsEmpty());
	}

	return true;
}

/**
 * Degenerate input fails closed: every poisoned problem comes back unanswered, never NaN, and
 * Unanswerable is never Stands. Green on arrival, so its teeth were proven by mutation (gutting
 * input validation turns every row red). The positive control stops a refuse-everything stub
 * passing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockOracleDegenerateTest,
	"DestructionGame.Oracle.RigidBlock.DegenerateInputsFailClosed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockOracleDegenerateTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace RigidBlockOracle;
	using namespace RigidBlockOracleTestSupport;

	const double NaN = std::numeric_limits<double>::quiet_NaN();

	// Positive control: the unpoisoned base problem is answerable.
	{
		const FOracleResult Healthy = SolveRigidBlock(TwoBlockTower(0.0, DryStone));

		TestTrue(
			*FString::Printf(
				TEXT("CONTROL: the base problem is answerable (it said: %s) — without ")
				TEXT("this, refusing everything would pass the whole matrix"),
				*Healthy.WhyNot),
			Healthy.bAnswered);
	}

	struct FPoisonRow
	{
		const TCHAR* Name;
		TFunction<void(FOracleProblem&)> Poison;
	};

	TArray<FPoisonRow> PoisonRows;

	PoisonRows.Add({ TEXT("NaN mass"),
		[NaN](FOracleProblem& P) { P.Blocks[1].MassKg = NaN; } });
	PoisonRows.Add({ TEXT("negative mass"),
		[](FOracleProblem& P) { P.Blocks[1].MassKg = -1.0; } });
	PoisonRows.Add({ TEXT("NaN centroid"),
		[NaN](FOracleProblem& P) { P.Blocks[1].CentroidXCm = NaN; } });
	PoisonRows.Add({ TEXT("NaN normal"),
		[NaN](FOracleProblem& P) { P.Joints[0].NormalZ = NaN; } });
	PoisonRows.Add({ TEXT("zero normal"),
		[](FOracleProblem& P) { P.Joints[0].NormalX = 0.0; P.Joints[0].NormalZ = 0.0; } });
	PoisonRows.Add({ TEXT("non-unit normal"),
		[](FOracleProblem& P) { P.Joints[0].NormalX = 0.5; P.Joints[0].NormalZ = 0.5; } });
	PoisonRows.Add({ TEXT("NaN joint centre"),
		[NaN](FOracleProblem& P) { P.Joints[0].CentreXCm = NaN; } });
	PoisonRows.Add({ TEXT("NaN area"),
		[NaN](FOracleProblem& P) { P.Joints[0].AreaSqCm = NaN; } });
	PoisonRows.Add({ TEXT("zero area"),
		[](FOracleProblem& P) { P.Joints[0].AreaSqCm = 0.0; } });
	PoisonRows.Add({ TEXT("negative area"),
		[](FOracleProblem& P) { P.Joints[0].AreaSqCm = -1.0; } });
	PoisonRows.Add({ TEXT("NaN half length"),
		[NaN](FOracleProblem& P) { P.Joints[0].HalfLengthCm = NaN; } });
	PoisonRows.Add({ TEXT("negative half length"),
		[](FOracleProblem& P) { P.Joints[0].HalfLengthCm = -1.0; } });
	PoisonRows.Add({ TEXT("NaN cohesion"),
		[NaN](FOracleProblem& P) { P.Joints[0].Strength.ShearCohesionMPa = NaN; } });
	PoisonRows.Add({ TEXT("NaN friction"),
		[NaN](FOracleProblem& P) { P.Joints[0].Strength.FrictionCoefficient = NaN; } });
	PoisonRows.Add({ TEXT("NaN tensile strength"),
		[NaN](FOracleProblem& P) { P.Joints[0].Strength.TensileStrengthMPa = NaN; } });
	PoisonRows.Add({ TEXT("NaN compressive strength"),
		[NaN](FOracleProblem& P) { P.Joints[0].Strength.CompressiveStrengthMPa = NaN; } });

	// A NaN shear ceiling is refused, unlike production, where a NaN cap compares false and means uncapped.
	PoisonRows.Add({ TEXT("NaN shear ceiling"),
		[NaN](FOracleProblem& P) { P.Joints[0].Strength.MaxShearStrengthMPa = NaN; } });

	PoisonRows.Add({ TEXT("joint block index out of range"),
		[](FOracleProblem& P) { P.Joints[0].BlockB = 17; } });
	PoisonRows.Add({ TEXT("joint from a block to itself"),
		[](FOracleProblem& P) { P.Joints[0].BlockB = P.Joints[0].BlockA; } });
	PoisonRows.Add({ TEXT("applied force on a block that does not exist"),
		[](FOracleProblem& P) { P.AppliedForces.Add({ 17, 1.0, 0.0, 0.0, 0.0, true }); } });
	PoisonRows.Add({ TEXT("NaN applied force"),
		[NaN](FOracleProblem& P) { P.AppliedForces.Add({ 1, NaN, 0.0, 0.0, 0.0, true }); } });

	for (const FPoisonRow& Row : PoisonRows)
	{
		FOracleProblem Problem = TwoBlockTower(0.0, DryStone);
		Row.Poison(Problem);

		const FOracleResult Result = SolveRigidBlock(Problem);

		TestTrue(
			*FString::Printf(TEXT("%s: must be refused, not answered"), Row.Name),
			!Result.bAnswered);
		TestTrue(
			*FString::Printf(TEXT("%s: the refusal must say why"), Row.Name),
			!Result.WhyNot.IsEmpty());
		TestTrue(
			*FString::Printf(TEXT("%s: lambda is finite even in refusal"), Row.Name),
			FMath::IsFinite(Result.Lambda));
		TestTrue(
			*FString::Printf(TEXT("%s: Unanswerable, and Unanswerable is never Stands"),
				Row.Name),
			OutcomeOf(Result) == EOracleOutcome::Unanswerable);
	}

	return true;
}

// Pricing-cost support; the namespace is reopened to keep helpers beside their test.
namespace RigidBlockOracleTestSupport
{
	/**
	 * wall-01, shrunk: the same DestructionWallCases::Build producer (flush running bond,
	 * mortared, bottom course grounded) at a smaller course and cell count.
	 */
	bool BuildIntactWall(int32 Courses, int32 Cells, FStructure& Out, FString& OutWhy)
	{
		using namespace DestructionProfiles;

		DestructionWallCases::FWallSpec Spec;
		Spec.BrickSizeCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm);
		Spec.JointThicknessCm = 1.0;
		Spec.DensityGramsPerCubicCm = ClayDensityGramsPerCubicCm;
		Spec.CoursesHigh = Courses;
		Spec.Cells = Cells;
		Spec.Bond = DestructionWallCases::EWallBond::Running;
		Spec.Strength = GeneralPurposeMortar;

		DestructionWallCases::FWallLayout Wall;

		if (!DestructionWallCases::Build(Spec, Wall))
		{
			OutWhy = FString::Printf(
				TEXT("the wall producer refused %d courses x %d cells"), Courses, Cells);
			return false;
		}

		Out = MoveTemp(Wall.Layout.Structure);
		return true;
	}

	/**
	 * Intact wall's lambda*, by hand. Cut above the grounded bottom course: only bed joints cross
	 * it, so vertical equilibrium gives sum(bed normals) = lambda * W_above, and crushing caps
	 * that sum at f_c * Conv * A_plane. Hence lambda* <= f_c * Conv * A_plane / W_above, and an
	 * intact wall attains it (measured 1128.4443217039532).
	 *
	 * Each course boundary carries Cells * 20.5 cm of bed (10.25 cm overlap each side). A flush
	 * odd course's two half bats are 10.25 cm each, so it is 214 cm of masonry against an even
	 * course's 215; ignoring that gives an answer 0.27% too high.
	 */
	double IntactWallCrushLambda(int32 Courses, int32 Cells)
	{
		constexpr double MortarBedCm = 1.0;

		const double HalfCellCm = (BrickLengthCm + MortarBedCm) / 2.0;
		const double HalfBatLengthCm = (BrickLengthCm - MortarBedCm) / 2.0;

		const double PlaneAreaSqCm =
			double(Cells) * 2.0 * (BrickLengthCm - HalfCellCm) * BrickWidthCm;

		const double EvenCourseLengthCm = double(Cells) * BrickLengthCm;
		const double OddCourseLengthCm =
			double(Cells - 1) * BrickLengthCm + 2.0 * HalfBatLengthCm;

		double LengthAboveCm = 0.0;

		for (int32 Course = 1; Course < Courses; ++Course)
		{
			LengthAboveCm += (Course % 2) == 0 ? EvenCourseLengthCm : OddCourseLengthCm;
		}

		// Density-first, as PieceMassKg orders it.
		const double WeightAboveUu = ClayDensityGramsPerCubicCm * LengthAboveCm
			* BrickWidthCm * BrickHeightCm / 1000.0 * GravityHere;

		return DestructionProfiles::GeneralPurposeMortar.CompressiveStrengthMPa
			* ConvHere * PlaneAreaSqCm / WeightAboveUu;
	}
}

/**
 * Pricing cost is a measured, budgeted property of the oracle. Dantzig pricing scans every
 * column every iteration, which left wall-01 (~13k rows x ~34k columns) still pivoting after
 * ~45 minutes. The assertion is a deterministic scan count (PricingColumnScans), not a wall
 * clock, with pivots budgeted alongside so cheaper scans cannot be bought with more pivots.
 *
 * A budget must not buy speed with a wrong answer, so each row also asserts the hand-derived
 * answer, optimum invariance at 1e-9 relative, and bit-identical determinism. Mutation P2 (a
 * pricer that reports INDEX_NONE after one empty window) proved this: the wall answered
 * lambda* = 0 in zero pivots and both budgets passed more easily; only the derived answers
 * caught it.
 *
 * Sized for the default suite; the 30-course original lives in the opt-in sweep. No ticking world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockOraclePricingCostTest,
	"DestructionGame.Oracle.RigidBlock.PricingCost",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockOraclePricingCostTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace RigidBlockOracle;
	using namespace RigidBlockOracleTestSupport;

	/*
	 * Row A: the 40-course leaning stack. Invariance only, no budget: at ~1.4k columns a
	 * candidate list could honestly cost slightly more.
	 */
	{
		const FOracleProblem Problem = LeaningStackProblem(40, 10.0, GeneralPurposeMortar);
		const FOracleResult Result = SolveRigidBlock(Problem);

		AddInfo(FString::Printf(
			TEXT("40-course stack: lambda %.17g, %d pivots, %lld pricing scans"),
			Result.Lambda, Result.SimplexIterations, Result.PricingColumnScans));

		TestTrue(
			*FString::Printf(TEXT("40-course stack: must answer (it said: %s)"), *Result.WhyNot),
			Result.bAnswered);

		/*
		 * The closed form is exact for a single column, so at 1e-9 this is both the derived
		 * answer and the optimum-invariance pin: the optimal value is unique whatever the pivot path.
		 */
		const double ClosedForm = StackTensionLambda(GeneralPurposeMortar, 40, 10.0);

		TestTrue(
			*FString::Printf(
				TEXT("40-course stack: lambda* must be the hand closed form %.17g within ")
				TEXT("1e-9 relative and was %.17g"),
				ClosedForm, Result.Lambda),
			FMath::Abs(Result.Lambda - ClosedForm) <= 1.0e-9 * ClosedForm);

		const FOracleResult Again =
			SolveRigidBlock(LeaningStackProblem(40, 10.0, GeneralPurposeMortar));

		TestTrue(
			*FString::Printf(
				TEXT("40-course stack: DETERMINISM — two solves agree to the last bit and ")
				TEXT("do the same work (%.17g/%d/%lld vs %.17g/%d/%lld)"),
				Result.Lambda, Result.SimplexIterations, Result.PricingColumnScans,
				Again.Lambda, Again.SimplexIterations, Again.PricingColumnScans),
			Result.Lambda == Again.Lambda
				&& Result.SimplexIterations == Again.SimplexIterations
				&& Result.PricingColumnScans == Again.PricingColumnScans);

		/*
		 * After 500 zero-length steps the pricer drops to Bland's full scan, so these rows only
		 * measure the partial pricer while the fallback stays dormant. This holds for these two
		 * fixtures only: the opening-ladder rungs enter it 170-358 times a solve (CURRENT_STATE).
		 */
		TestEqual(
			TEXT("40-course stack: the Bland anti-cycling fallback must stay dormant — the ")
			TEXT("scan and pivot figures on these rows measure the partial pricer, and only ")
			TEXT("do so while it has not stood aside"),
			Result.BlandDegenerateEntries, 0);
	}

	// Row B: the intact wall, budgeted.

	{
		constexpr int32 Courses = 8;
		constexpr int32 Cells = 10;

		FStructure Wall;
		FString Why;

		// Own statement: the message reads Why, which the call writes (TRAPS.md, unsequenced evaluation).
		const bool bLaid = BuildIntactWall(Courses, Cells, Wall, Why);

		if (!TestTrue(
				*FString::Printf(TEXT("wall: the producer must lay it (it said: %s)"), *Why),
				bLaid))
		{
			return true;
		}

		// If the producer lays a different wall, the budget below is for a different problem.
		TestEqual(TEXT("wall FIXTURE: 4 x 10 whole-brick courses + 4 x 11-piece flush ")
			TEXT("courses = 84 pieces"), Wall.NumPieces(), 84);

		FOracleProblem Problem;

		const bool bBridged = BuildRigidBlockProblem(Wall, Problem, Why);

		if (!TestTrue(
				*FString::Printf(TEXT("wall: the bridge must represent it (it said: %s)"), *Why),
				bBridged))
		{
			return true;
		}

		const double Started = FPlatformTime::Seconds();
		const FOracleResult Result = SolveRigidBlock(Problem);
		const double Seconds = FPlatformTime::Seconds() - Started;

		AddInfo(FString::Printf(
			TEXT("wall %dx%d: %d blocks / %d joints, lambda %.17g, %d pivots, ")
			TEXT("%lld pricing scans (%.0f per pivot), %.3f s"),
			Courses, Cells, Problem.Blocks.Num(), Problem.Joints.Num(),
			Result.Lambda, Result.SimplexIterations, Result.PricingColumnScans,
			Result.SimplexIterations > 0
				? double(Result.PricingColumnScans) / double(Result.SimplexIterations)
				: 0.0,
			Seconds));

		TestTrue(
			*FString::Printf(TEXT("wall: must answer (it said: %s)"), *Result.WhyNot),
			Result.bAnswered);

		const double CrushLambda = IntactWallCrushLambda(Courses, Cells);

		AddInfo(FString::Printf(
			TEXT("wall: hand crushing closed form is %.17g"), CrushLambda));

		TestTrue(
			*FString::Printf(
				TEXT("wall: lambda* CANNOT exceed the hand crushing bound %.17g — vertical ")
				TEXT("equilibrium across the bottom bed plane caps it whatever the force ")
				TEXT("system — and was %.17g"),
				CrushLambda, Result.Lambda),
			Result.Lambda <= CrushLambda * (1.0 + 1.0e-9));

		TestTrue(
			*FString::Printf(
				TEXT("wall: lambda* must ATTAIN the hand crushing bound %.17g within 1e-9 ")
				TEXT("relative (an intact wall can load every bottom contact to its cap at ")
				TEXT("once) and was %.17g — this line is also the OPTIMUM-INVARIANCE pin: a ")
				TEXT("pricing change may take any pivot path to this number and none to a ")
				TEXT("different one"),
				CrushLambda, Result.Lambda),
			FMath::Abs(Result.Lambda - CrushLambda) <= 1.0e-9 * CrushLambda);

		const FOracleResult Again = SolveRigidBlock(Problem);

		TestTrue(
			*FString::Printf(
				TEXT("wall: DETERMINISM — two solves agree to the last bit and do the same ")
				TEXT("work (%.17g/%d/%lld vs %.17g/%d/%lld)"),
				Result.Lambda, Result.SimplexIterations, Result.PricingColumnScans,
				Again.Lambda, Again.SimplexIterations, Again.PricingColumnScans),
			Result.Lambda == Again.Lambda
				&& Result.SimplexIterations == Again.SimplexIterations
				&& Result.PricingColumnScans == Again.PricingColumnScans);

		// Dormancy pin; see row A.
		TestEqual(
			TEXT("wall: the Bland anti-cycling fallback must stay dormant — the budgets ")
			TEXT("below are budgets for the partial pricer, and only are while it has not ")
			TEXT("stood aside"),
			Result.BlandDegenerateEntries, 0);

		/*
		 * Budgets, measured. Full Dantzig here (2026-08-12): 6,849 columns, 1,942 pivots,
		 * 3,131,528 scans. ~343,000 of those are the one-off artificial pivot-out pass
		 * (222 equality rows x 1,657 structural columns), a floor pricing cannot touch.
		 * 1,000,000 is a 3.1x cut overall; a candidate list should beat it easily. Partial
		 * pricing usually takes more pivots, so 4,000 is ~2x today's count.
		 */
		constexpr int64 PricingScanBudget = 1000000;
		constexpr int32 PivotBudget = 4000;

		TestTrue(
			*FString::Printf(
				TEXT("wall: PRICING COST — the solve must price at most %lld columns and ")
				TEXT("priced %lld (%.1fx over). Scanning every column every iteration is ")
				TEXT("what makes the 30-course walls unanswerable; the fix is a candidate ")
				TEXT("list or reference weights, with the entering choice still fixed by ")
				TEXT("index arithmetic"),
				PricingScanBudget, Result.PricingColumnScans,
				double(Result.PricingColumnScans) / double(PricingScanBudget)),
			Result.PricingColumnScans <= PricingScanBudget);

		TestTrue(
			*FString::Printf(
				TEXT("wall: PIVOT COST — the solve must take at most %d pivots and took %d. ")
				TEXT("Budgeted beside the scan count so cheaper pricing cannot pay for ")
				TEXT("itself with a grinding pivot path"),
				PivotBudget, Result.SimplexIterations),
			Result.SimplexIterations <= PivotBudget);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
