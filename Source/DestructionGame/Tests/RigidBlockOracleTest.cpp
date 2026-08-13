// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"
#include "Core/WallCases.h"
#include "Tests/RigidBlockOracle.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE RIGID-BLOCK ORACLE'S OWN VALIDATION SET — DESIGN.md §7 evolution step 3, slice 1.
 *
 * Every expected lambda* below is HAND-WORKED LIMIT ANALYSIS, derived in this file from
 * the brick, the published profile figures and an independently derived unit conversion.
 * Nothing is transcribed from the oracle, and nothing is transcribed from production:
 * the oracle exists to disagree with production where production is wrong, so its
 * validation must stand on statics done by hand.
 *
 * THE DERIVATION PATTERN, used throughout. Each hand answer rests on one fact about the
 * two-contact-point discretisation: a joint's contact normal forces are DETERMINED by
 * the equilibrium of the body above it (n1 + n2 = the vertical resultant V, and
 * (n2 - n1) * h = the moment M about the joint centre, h the contact half-spacing), so
 *
 *     n_inboard = V/2 - M/(2h)      and lambda* is the largest lambda at which every
 *     n_outboard = V/2 + M/(2h)     joint's n_inboard >= -f_t*Conv*A/2  (tension)
 *                                   and n_outboard <= f_c*Conv*A/2      (crushing)
 *
 * For a single-column chain (every block exactly one seat below, one load above) these
 * are the ONLY admissible force systems, so the hand answer is exact, not a bound.
 *
 * WHAT EACH VALIDATION CLASS PINS:
 *
 *   - TIPPING (single blocks, dry stacks): equilibrium + the no-tension bound. The
 *     rigid-block tipping rule "resultant inside the bearing" must fall out of the LP,
 *     including the KNIFE-EDGE CONVENTION: a resultant exactly AT the bearing edge is
 *     feasible (the constraint set is closed) — the boundary STANDS, pinned here so
 *     nobody re-decides it silently.
 *
 *   - SLIDING (pushed block): the Mohr-Coulomb rows and the dead/live split. A block
 *     pushed by its own weight slides at exactly mu (dry) and at (c*Conv*A + mu*W)/W
 *     (mortar) — the mortar figure embeds the 10000 uu conversion, so an open-coded
 *     factor of 100 fails this row by two orders of magnitude.
 *
 *   - CRUSHING (centred blocks): the compressive cap. lambda* for a centred block is
 *     f_c*Conv*A/W — and at a knife edge exactly HALF that, because one contact point
 *     carries everything: a relation between two rows that no tuning can fake.
 *
 *   - THE FINITE-TENSION MAPPING (leaning stacks): the acceptance-suite geometry, 10 cm
 *     of lean per course, at the CODED characteristic f_xk1 = 0.10. The rigid-plastic
 *     hand ladder gives lambda* = {4.458, 1.241, 0.0629, 0.0344} at 5/8/30/40 courses:
 *     the same STANDS/STANDS/FALLS/FALLS the acceptance rulings reached on first-crack
 *     arithmetic at MEAN strength — two criteria, one verdict ladder, now with margins
 *     measured. The same geometry DRY at 5 courses is infeasible (lambda* = 0): the
 *     finite-tension mapping is the entire difference, asserted as a relation.
 *
 *   - THE ONE-CELL JAMMING PAIR: a half-seated brick alone cannot equilibrate (its
 *     weight acts 0.5 cm past its bearing edge; with f_t = 0 nothing can supply the
 *     couple) — lambda* = 0. Give it an abutment through a head joint and a dry-stone
 *     feasible system EXISTS; the certificate, verifiable by hand on the free bodies:
 *
 *         seat outer contact carries n = W (all weight at the edge),
 *         head-joint TOP contact pushes the brick back with n_h = 0.5*W/7.0
 *           (0.5 cm of weight lever balanced at the head contact 7.0 cm above the
 *            bed plane), the abutting reaction returning as bed shear 0.0714*W,
 *         every friction row at most 0.0714/0.7 = 10.2% of capacity.
 *
 *     So the ORACLE READS THE ONE-CELL DRY CASE AS STANDING, and that is a DELIBERATE,
 *     RECORDED DISAGREEMENT with production's one-cell gate (which refuses dry stone
 *     the arching relief at 1.4921 of sliding capacity): production's rule keeps the
 *     uncracked-section kern and measures the thrust at the head joint's CENTROID
 *     (arm 3.75 cm), where the limit theorem may place contact at the section's EDGE
 *     (arm 7.0 cm) and needs no kern. Slice 2's fixture sweep measures that gap; this
 *     file only proves the oracle implements the limit theorem it claims to.
 *
 * DETERMINISM IS ASSERTED, NOT ASSUMED: every problem is solved twice and the two
 * lambdas must be BIT-IDENTICAL, with the same pivot count. A seeded-fuzz oracle that
 * drifts between runs is worse than none.
 *
 * NEEDS A TICKING WORLD: NO. Pure arithmetic on plain structs, like the solver it will
 * be diffed against.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges many files into one translation
 * unit.
 */
namespace RigidBlockOracleTestSupport
{
	using namespace RigidBlockOracle;

	/* ================================================================================
	 * THE BRICK AND THE UNITS — derived here, imported from nowhere.
	 * ================================================================================ */

	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickWidthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;
	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** Density-first multiplication order — the PieceMassKg contract; 2.72163125 kg. */
	constexpr double BrickMassKg =
		ClayDensityGramsPerCubicCm * BrickLengthCm * BrickWidthCm * BrickHeightCm / 1000.0;

	constexpr double GravityHere = 980.0;

	/** 2667.198625 uu. */
	constexpr double BrickWeightUu = BrickMassKg * GravityHere;

	/**
	 * 1 N = 100 uu, 1 cm2 = 100 mm2 — derived independently a THIRD time (production,
	 * the oracle, and this file must each be able to fail the other two).
	 */
	constexpr double ConvHere = 100.0 * 100.0;

	/** Standard mortared coursing: 6.5 brick + 1.0 bed. */
	constexpr double CoursePitchCm = 7.5;

	/** Full bed 220.375 cm2; running-bond half seat 105.0625 cm2; head face 66.625 cm2. */
	constexpr double FullBedAreaSqCm = BrickLengthCm * BrickWidthCm;
	constexpr double HalfSeatAreaSqCm = 10.25 * BrickWidthCm;
	constexpr double HeadAreaSqCm = BrickHeightCm * BrickWidthCm;

	/* ================================================================================
	 * PROBLEM BUILDERS. Geometry is laid here, never read back from the oracle.
	 * ================================================================================ */

	/**
	 * A grounded brick with one brick on top of it, the top centroid offset sideways.
	 * The bed contact segment is centred on the BASE, so the offset walks the top
	 * block's resultant across the bearing: 0 is centred, 10.75 is the knife edge,
	 * beyond it is an overhang no no-tension bearing can hold.
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
	 * The classic sliding problem: gravity DEAD, a live horizontal push applied AT BED
	 * LEVEL so it exerts no overturning couple and friction alone decides. The clean
	 * closed form is the point: lambda* = (c*Conv*A + mu*W) / Push, exactly.
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

	/**
	 * The leaning stack: one column of bricks, each course offset a fixed distance, the
	 * base grounded — the acceptance family's geometry, hand-built so the oracle's
	 * answer and the FStructure bridge's answer can be compared bit for bit.
	 */
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
	 * THE ONE-CELL GEOMETRY, mortared coursing, 1 cm joints: a grounded running-bond
	 * seat G, the half-seated brick P over the hole (its centroid 5.625 cm outboard of
	 * its 10.25 cm seat), and optionally the abutment pair — neighbour N fully seated
	 * on its own grounded brick, met through a head joint whose face spans the course.
	 *
	 * Identical to the FStructure fixture the bridge test lays through MakeInterface,
	 * so the two paths can be compared exactly.
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

	/* ================================================================================
	 * THE HAND-WORKED CLOSED FORMS. Each is the derivation pattern from the file header
	 * applied to one geometry; none knows anything about the LP.
	 * ================================================================================ */

	/** Centred block: n splits evenly, crushing binds both contacts: f_c*Conv*A / W. */
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
	 * The stack's bottom-joint statics, m = Courses - 1 bricks above it: resultant
	 * m*W at d*m/2 outboard of the joint centre, so with contact half-spacing h,
	 *
	 *     n_inboard(lambda)  = lambda * W * (m/2 - d*m^2/(4h))
	 *     n_outboard(lambda) = lambda * W * (m/2 + d*m^2/(4h))
	 *
	 * Every higher joint is the same formula at a smaller m, so the bottom governs.
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

	/** The same statics where crushing at the outboard contact is what binds. */
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
	 * The half-seated brick alone, mortared: weight at e = 5.625 cm from the seat
	 * centre, contacts at +-5.125, so n_inboard = lambda*W*(1/2 - e/(2h)) and the bond
	 * holds until lambda* = T_A / (W * (e/(2h) - 1/2)) — about 404: one brick is
	 * nothing to a real bond, which is why the one-cell verdicts only ever turn on
	 * geometry, not load.
	 */
	double JammingAloneTensionLambda(const FConnectionStrength& Strength)
	{
		const double e = 5.625;
		const double h = 5.125;
		const double TensionCapacityUu =
			Strength.TensileStrengthMPa * ConvHere * HalfSeatAreaSqCm / 2.0;

		return TensionCapacityUu / (BrickWeightUu * (e / (2.0 * h) - 0.5));
	}

	/* ================================================================================
	 * THE CATALOGUE ROW.
	 * ================================================================================ */

	struct FOracleValidationRow
	{
		const TCHAR* Name = nullptr;

		/** The one-line hand derivation, printed beside any failure. */
		const TCHAR* Because = nullptr;

		TFunction<FOracleProblem()> Build;

		/** Negative means "assert the floor only" — used where no closed form exists. */
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

		/* Determinism: same problem, bit-identical answer, same pivot path. */
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

/**
 * THE VALIDATION CATALOGUE: twenty hand-worked limit-analysis answers the oracle must
 * reproduce, each with its arithmetic in this file. See the file header for what each
 * class pins and for the recorded one-cell disagreement with production.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockOracleValidationTest,
	"DestructionGame.Oracle.RigidBlock.ValidationCatalogue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockOracleValidationTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace RigidBlockOracle;
	using namespace RigidBlockOracleTestSupport;

	/*
	 * FIXTURE PRECONDITIONS: the profile figures every hand answer below is derived
	 * from, asserted rather than trusted. A retune moves this file's margins and must
	 * land here first, loudly.
	 */
	TestEqual(TEXT("FIXTURE: dry stone has exactly no cohesion"),
		DryStone.ShearCohesionMPa, 0.0);
	TestEqual(TEXT("FIXTURE: dry stone has exactly no tensile bond"),
		DryStone.TensileStrengthMPa, 0.0);
	TestEqual(TEXT("FIXTURE: dry stone's friction is 0.7"),
		DryStone.FrictionCoefficient, 0.7);
	TestEqual(TEXT("FIXTURE: dry stone crushes at 30 MPa"),
		DryStone.CompressiveStrengthMPa, 30.0);
	TestEqual(TEXT("FIXTURE: mortar's characteristic bond is EN 1996-1-1's 0.10"),
		GeneralPurposeMortar.TensileStrengthMPa, 0.1);
	TestEqual(TEXT("FIXTURE: mortar's cohesion is f_vk0 = 0.20"),
		GeneralPurposeMortar.ShearCohesionMPa, 0.2);
	TestEqual(TEXT("FIXTURE: mortar's friction is 0.6"),
		GeneralPurposeMortar.FrictionCoefficient, 0.6);
	TestEqual(TEXT("FIXTURE: mortar crushes at M10's 10 MPa"),
		GeneralPurposeMortar.CompressiveStrengthMPa, 10.0);
	TestEqual(TEXT("FIXTURE: the standard brick weighs 2.72163125 kg"),
		BrickMassKg, 2.72163125);

	TArray<FOracleValidationRow> Rows;

	/* ---- Tipping and crushing: single blocks. ---------------------------------- */

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

	/* ---- Sliding: the friction rows and the dead/live split. ------------------- */

	Rows.Add({ TEXT("pushed dry block"),
		TEXT("dead weight W, live push W at bed level: slides at exactly mu = 0.7"),
		[] { return SlidingProblem(DestructionProfiles::DryStone, BrickWeightUu); },
		0.7, 0.0, 1.0e-9, 1.0e-9, EOracleOutcome::Falls });

	Rows.Add({ TEXT("pushed mortared block"),
		TEXT("lambda* = (c*Conv*A + mu*W)/W ~ 165.85 — the row that embeds the 10000 ")
		TEXT("uu/MPa/cm2 conversion, so a factor-of-100 unit slip fails it outright"),
		[] { return SlidingProblem(DestructionProfiles::GeneralPurposeMortar, BrickWeightUu); },
		SlidingLambda(GeneralPurposeMortar, BrickWeightUu), 0.0, 0.0, 1.0e-6,
		EOracleOutcome::Stands });

	/* ---- The no-tension rigid-block ladder: dry stack, 2 cm of lean. ----------- */

	/*
	 * Feasible iff the bottom joint's inboard contact needs no tension: m/2 >= d*m^2/(4h),
	 * i.e. m <= 2h/d = 9.75 bricks above. Five and eight courses stand (crushing sets
	 * their finite lambda*); thirty and forty have no admissible system at any load.
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

	/* ---- The finite-tension mapping: the acceptance lean, 10 cm per course. ---- */

	Rows.Add({ TEXT("mortared stack 10 cm x 5 courses"),
		TEXT("bottom-joint plastic tension bound: lambda* = T_A/(W*(d*m^2/(4h) - m/2)) ")
		TEXT("~ 4.458 — STANDS, agreeing with acceptance case 1"),
		[] { return LeaningStackProblem(5, 10.0, DestructionProfiles::GeneralPurposeMortar); },
		StackTensionLambda(GeneralPurposeMortar, 5, 10.0), 0.0, 0.0, 1.0e-6,
		EOracleOutcome::Stands });

	Rows.Add({ TEXT("mortared stack 10 cm x 8 courses"),
		TEXT("~ 1.241 — STANDS with the margin measured, agreeing with acceptance case 2"),
		[] { return LeaningStackProblem(8, 10.0, DestructionProfiles::GeneralPurposeMortar); },
		StackTensionLambda(GeneralPurposeMortar, 8, 10.0), 0.0, 0.0, 1.0e-6,
		EOracleOutcome::Stands });

	Rows.Add({ TEXT("mortared stack 10 cm x 30 courses"),
		TEXT("~ 0.0629 — FALLS at a sixteenth of its weight, agreeing with acceptance ")
		TEXT("case 3, which production only reaches via the interim guard"),
		[] { return LeaningStackProblem(30, 10.0, DestructionProfiles::GeneralPurposeMortar); },
		StackTensionLambda(GeneralPurposeMortar, 30, 10.0), 0.0, 0.0, 1.0e-6,
		EOracleOutcome::Falls });

	Rows.Add({ TEXT("mortared stack 10 cm x 40 courses"),
		TEXT("~ 0.0344 — FALLS, agreeing with acceptance case 4"),
		[] { return LeaningStackProblem(40, 10.0, DestructionProfiles::GeneralPurposeMortar); },
		StackTensionLambda(GeneralPurposeMortar, 40, 10.0), 0.0, 0.0, 1.0e-6,
		EOracleOutcome::Falls });

	Rows.Add({ TEXT("dry stack at the acceptance lean, 5 courses"),
		TEXT("the same geometry the mortared 5-course row stands on: dry, m = 4 puts ")
		TEXT("the resultant 20 cm out on a 5.75 cm half-bearing, lambda* = 0 — the ")
		TEXT("finite-tension mapping is the entire difference"),
		[] { return LeaningStackProblem(5, 10.0, DestructionProfiles::DryStone); },
		0.0, 0.0, 1.0e-6, 0.0, EOracleOutcome::Falls });

	/* ---- The one-cell jamming pair. -------------------------------------------- */

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

	Rows.Add({ TEXT("one-cell half seat, mortared, no abutment"),
		TEXT("the bond alone holds one brick at lambda* = T_A/(W*(e/(2h) - 1/2)) ~ 404"),
		[] { return JammingProblem(false, DestructionProfiles::GeneralPurposeMortar); },
		JammingAloneTensionLambda(GeneralPurposeMortar), 0.0, 0.0, 1.0e-6,
		EOracleOutcome::Stands });

	/* ---- The redundant-joint path. ---------------------------------------------- */

	/*
	 * THE ROW CURRENT_STATE QUEUED against the dense solver's recorded
	 * basic-artificial residue: duplicate one joint of the mortared 8-course stack as
	 * two half-area twins with identical geometry. Per contact point every capacity is
	 * area-proportional, so two half-area twins at the same two contact positions have
	 * EXACTLY the original joint's summed feasible set and lambda* must not move — but
	 * the columns are parallel and the optimum degenerate, the shape the dense
	 * solver's residue lived in. Added 2026-08-12 with the sparse rewrite, green on
	 * arrival. HONESTY ABOUT ITS TEETH, measured by mutation: it fails under the
	 * formulation mutations like every strength row (M3/M4). The residue itself is
	 * contained by the pivot-out pass immediately after phase 1 (it clears every
	 * zero-value basic artificial a real column can reach) together with the
	 * post-solve verification gate, which fails closed rather than certifying a basis
	 * it cannot check; disabling both fires the whole suite as verification refusals
	 * (TRAPS.md) rather than a silent under-report, so this row's own value is a
	 * regression net for future degeneracy-handling changes, not the only thing
	 * watching the residue.
	 *
	 * THE PROPERTY UNDER TEST IS EQUALITY, NOT PROXIMITY: a redundant joint is the same
	 * constraint set as its un-duplicated twin, so lambda* must be the SAME NUMBER, not
	 * merely close to the closed form both rows also happen to satisfy. Measured (this
	 * build): both rows report 1.241110018676219, bit-identical (diff 0) despite the
	 * twin row taking one more pivot (8 vs 7) to reach it — the cross-row check below
	 * asserts that exact equality directly, rather than trusting two separately-toleranced
	 * closed-form checks to imply it.
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

	/*
	 * THE CROSS-ROW RELATIONS — statements no single row can fake.
	 */
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
		 * A redundant joint IS the same constraint set as its un-duplicated twin, so
		 * this is equality, not proximity to a shared closed form. Measured (this
		 * build, both directions): 1.241110018676219 for both rows, diff exactly 0
		 * despite the twin row taking one more pivot (8 vs 7) — the degenerate,
		 * parallel-column path the twins create still lands on the identical bit
		 * pattern the un-duplicated joint does. Asserted as `==`, not a tolerance.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("RELATION: a redundant joint is the same constraint set as its ")
				TEXT("un-duplicated twin — lambda* must be the SAME NUMBER, not merely ")
				TEXT("close (8-course %.17g vs twins %.17g)"),
				Original, Twins),
			Original == Twins);
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
 * THE BRIDGE THE FIXTURE SWEEP WILL CALL: BuildRigidBlockProblem must project a live
 * FStructure — laid through the SAME producer the fixtures use (MakeInterface) — into
 * exactly the problem the hand builders describe, byte for byte, and must refuse
 * anything it cannot represent honestly.
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

	/* ---- The mortared 8-course acceptance stack, through FStructure. ----------- */
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

		/*
		 * The producer's joints must be the exact rectangles the hand builder writes,
		 * or the bit-for-bit equivalence below is comparing different structures.
		 */
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

		/* Its own statement first — a message reading state the condition writes is the
		 * unsequenced-evaluation trap TRAPS.md records. */
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
		 * BIT-FOR-BIT: the producer's geometry is exact binary arithmetic on this
		 * fixture (every coordinate is a multiple of 0.25), so the two problems are
		 * identical and the deterministic solver must return the identical double.
		 * TestTrue on ==, not TestEqual, whose double overload carries a tolerance.
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

	/* ---- The one-cell jamming pair, through FStructure, latch included. -------- */
	{
		FStructure Structure;
		TArray<FPieceBox> Boxes;

		const FVector BrickExtent = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm) * 0.5;

		/* G, P, N, G2 — the JammingProblem geometry, as boxes. */
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
		 * REMOVE THE NEIGHBOUR: removal severs its joints, the bridge must skip what
		 * has given and what is gone, and the half seat is alone again — lambda* = 0.
		 * This is the latch-awareness the sweep depends on: the oracle judges the
		 * graph AS IT STANDS, not as it was built.
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

	/* ---- What the bridge must refuse. ------------------------------------------ */
	{
		/* A piece nobody placed: no centre of mass means no honest lever arm. */
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
		/* An out-of-plane joint: a Y normal cannot be projected into X-Z honestly. */
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
 * DEGENERATE INPUT FAILS CLOSED, ALWAYS: never NaN out, never a garbage answer wearing
 * a verdict's clothes. Every poisoned problem must come back unanswered — and
 * Unanswerable is not Stands, which is the polarity that matters when the sweep starts
 * branching on verdicts.
 *
 * GREEN-ON-ARRIVAL BY NATURE once the happy path exists (the rows assert refusals), so
 * per TRAPS this test's teeth are proven by mutation: gut the oracle's input
 * validation and every row here goes red. The positive control row is what keeps the
 * matrix from passing against a stub that refuses everything.
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

	/* The positive control: the unpoisoned base problem must be answerable at all. */
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

	/*
	 * A NaN shear CEILING is refused too — deliberately the OPPOSITE of production's
	 * recorded laundering hazard, where a NaN cap compares false and quietly means
	 * "uncapped". The oracle is the second opinion; it must not inherit the hole.
	 */
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

/* ====================================================================================
 * THE PRICING-COST SEAM — the support this file's third test needs.
 *
 * REOPENED, NOT A SECOND NAMESPACE: a unity build merges files into one translation
 * unit, so every test helper in this project lives in a named namespace; reopening the
 * file's own keeps the helpers beside the test that reads them without inventing a
 * second name for the same file's support.
 * ==================================================================================== */
namespace RigidBlockOracleTestSupport
{
	/**
	 * WALL-01, SHRUNK. Laid by production's OWN acceptance-wall producer — the same
	 * DestructionWallCases::Build, flush running bond, mortared, bottom course grounded,
	 * nothing cut — that the scenario catalogue lays wall-01 with at thirty courses and
	 * twelve cells. Only the two integers differ, so the fixture the pricing budget is
	 * measured on is the SHAPE the pricing change exists to make practical, not a
	 * separate toy that happens to be big.
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
	 * THE INTACT WALL'S lambda*, DERIVED BY HAND — and it is a closed form, not a bracket.
	 *
	 * Cut the wall horizontally just above the grounded bottom course. Head joints lie
	 * WITHIN a course, so the only joints the cut crosses are that course's bed joints,
	 * whose normals are vertical. Vertical equilibrium of everything above the cut reads
	 *
	 *     sum of bed normal forces = lambda * W_above, exactly,
	 *
	 * and every contact point is capped by crushing at f_c * Conv * A/2, which sums over
	 * the plane to f_c * Conv * A_plane. So for ANY admissible force system
	 *
	 *     lambda* <= f_c * Conv * A_plane / W_above,
	 *
	 * an inequality no redistribution can escape. The bound is also ATTAINED here — an
	 * intact wall can carry uniform bed compression, so nothing stops every contact
	 * reaching its cap together — and the LP agrees with it to the last bits (measured
	 * 1128.4443217039532 against 1128.44432170395), which is why the test asserts BOTH the
	 * inequality and the equality. The inequality is the statics; the equality is the
	 * measurement, and it doubles as the optimum-invariance pin.
	 *
	 * THE GEOMETRY, on the coordinating grid (21.5 cm brick, 1 cm joint, 22.5 cm cell
	 * pitch, half-cell offset, ends closed flush with half bats):
	 *
	 *   - a full brick overlaps each of the two beneath it by 21.5 - 11.25 = 10.25 cm, so
	 *     20.5 cm of bed per cell; the two half bats closing a flush course contribute
	 *     10.25 cm each, exactly one more cell's worth. Every course boundary therefore
	 *     carries Cells * 20.5 * BrickWidth of bed.
	 *
	 *   - A COURSE OF BATS DOES NOT WEIGH A COURSE OF BRICKS, and that 0.5% is the whole
	 *     difference between this closed form and an answer 0.27% too high (paid for once
	 *     while writing this): a half bat is (21.5 - 1)/2 = 10.25 cm, so two of them are
	 *     20.5 cm of brick where the full brick they replace is 21.5. A flush odd course
	 *     is 214 cm of masonry against an even course's 215.
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

		/* Density-first, exactly as PieceMassKg orders it. */
		const double WeightAboveUu = ClayDensityGramsPerCubicCm * LengthAboveCm
			* BrickWidthCm * BrickHeightCm / 1000.0 * GravityHere;

		return DestructionProfiles::GeneralPurposeMortar.CompressiveStrengthMPa
			* ConvHere * PlaneAreaSqCm / WeightAboveUu;
	}
}

/**
 * PRICING COST IS A MEASURED PROPERTY OF THIS ORACLE, NOT AN IMPLEMENTATION DETAIL.
 *
 * WHY THIS TEST EXISTS. The sparse rewrite (2026-08-12) made the 30-course walls
 * REPRESENTABLE — the constraint matrix is tens of MB rather than a 3.5 GB dense tableau
 * — and they are still unanswerable, because DANTZIG PRICING SCANS EVERY COLUMN EVERY
 * ITERATION. wall-01 is ~13k rows x ~34k columns; one pivot prices ~34k columns, so the
 * pricing work is pivots x columns and wall-01 was still pivoting after ~45 minutes when
 * its measuring run was cut off. The cost is not the factorisation, the FTRAN or the
 * BTRAN: it is the scan. That is what this test measures and budgets.
 *
 * THE ASSERTION IS A SCAN COUNT, NOT A WALL CLOCK. FOracleResult::PricingColumnScans
 * counts every column priced against a dual vector, and it is DETERMINISTIC — the same
 * problem gives the same count on every machine, where seconds give a different number
 * on every run and a flaky test is worse than none. Pivots are budgeted alongside it, so
 * a pricing rule that scans less by pivoting more cannot pass by trading one cost for
 * the other.
 *
 * WHAT A BUDGET MAY NOT DO IS BUY SPEED WITH A WRONG ANSWER, so every row also asserts:
 *
 *   - the DERIVED answer — the leaning stack's exact closed form, the wall's hard
 *     crushing upper bound (both worked in this file, neither read off the solver);
 *   - OPTIMUM INVARIANCE at 1e-9 relative against the measured lambda*. An LP's optimal
 *     VALUE is unique and method-independent: a different pivot path is allowed, a
 *     different optimum is not. This is the pin that makes a pricing change safe to
 *     make, and it is deliberately three orders tighter than the sweep's windows;
 *   - DETERMINISM, bit-identical across two solves, including the scan count itself.
 *
 * PROVEN TO BITE BY P2, AND THIS IS THE WHOLE REASON THOSE THREE BULLETS ARE HERE. Every
 * correctness line on both rows passed the day it was written — the partial pricer was
 * already correct — so until it was mutated, this test was indistinguishable from one
 * that asserts nothing about the answer. Mutation P2 is the QUEUE-IS-THE-ANSWER bug a
 * candidate list invites: make the refill take ONE window instead of widening until it
 * bites (`while (Scanned < AllowedCols)` -> `while (Scanned == 0)` in
 * FPartialPricer::ChooseEntering), so an empty window reports INDEX_NONE and the simplex
 * believes it. Measured on this tree, 8 assertion failures across 4 tests:
 *
 *     PricingCost          40-course stack lambda 2.150669592637872e-23 against the
 *                          closed form 0.034429736317305934 (1 pivot, not 39); wall
 *                          lambda* 0 against the crushing bound 1128.4443217039529,
 *                          reached in ZERO pivots
 *     ValidationCatalogue  the mortared 30- and 40-course stacks, same collapse
 *     Sweep.CorbelFamily   corbel B at 1.29e-25, which also FLIPS its pinned relation
 *     Sweep.LeaningStack   both stack rows
 *
 * READ THE COST SIDE OF THAT RUN BEFORE TRUSTING ANY BUDGET: the wall's scans fell from
 * 538,200 to 343,653 and its pivots from 2,016 to 0, so BOTH budgets below passed more
 * comfortably than ever while the oracle answered lambda* = 0. A cost assertion cannot
 * notice a solver that stops early — only the derived answers can, which is why the
 * closed form and the crushing bound sit on the same rows as the budgets and not in
 * some other test.
 *
 * WHERE THE FLOOR IS, so nobody chases an unreachable budget. The artificial pivot-out
 * pass after phase 1 prices every non-artificial column once per basic artificial it
 * clears, and a gravity-live problem starts with one basic artificial per equality row.
 * Those scans are counted here too, and they are a ONE-OFF O(equality rows x columns)
 * cost that per-iteration pricing cannot touch — about a ninth of the wall row's total,
 * worked out in that row's own comment. The budget sits comfortably above the floor: it
 * is aimed squarely at the per-iteration scan, and the pass is a separate question that
 * the same measurement says is not the blocker at wall scale.
 *
 * SIZED FOR THE DEFAULT SUITE, DELIBERATELY. The wall row is the same producer as
 * wall-01 at a height the ~30 s suite can afford; the 30-course original belongs to the
 * opt-in sweep group, whose file owns its own rows. Raising the two integers is the
 * whole difference, and the budget scales with the fixture, so this is a seam and not a
 * one-off measurement.
 *
 * NEEDS A TICKING WORLD: NO. Producers, graph, LP — arithmetic on plain structs.
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

	/* ---- Row A: the 40-course leaning stack — invariance, no budget. ------------- */

	/*
	 * THE OPTIMUM-INVARIANCE ANCHOR, and deliberately NOT budgeted: at ~1.4k columns a
	 * candidate-list scheme has little room to win and could honestly cost slightly
	 * more, so a budget here would refuse a correct implementation for being small. Its
	 * job is to prove the answer does not move — against the exact closed form AND
	 * against the last bit of the measured value.
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
		 * ONE ASSERTION DOING TWO JOBS, AND AT 1e-9 RATHER THAN THE CATALOGUE'S 1e-6. The
		 * bottom-joint closed form is exact for a single-column chain, and full Dantzig
		 * reproduces it to the last bit today (0.034429736317305934 against
		 * 0.0344297363173059 recomputed here) — so the same line is the DERIVED answer and
		 * the OPTIMUM-INVARIANCE pin a pricing change has to survive. An LP's optimal
		 * value is unique and method-independent: a different pivot path is allowed, a
		 * different optimum is not, and 1e-9 leaves seven orders of headroom over today's
		 * agreement for the rounding a different basis brings.
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
		 * THE FALLBACK IS ASSUMED NOT TO FIRE, SO SAY SO WHERE A TEST CAN READ IT. The
		 * pricer stands aside entirely after 500 zero-length steps and the entering rule
		 * drops to Bland's — a full index-order scan, which is both far more scanning per
		 * iteration and, on the dense solver's measurement, a rule that happily pivots on
		 * near-tolerance elements. Every budget and every pivot count on these two rows is
		 * a measurement of the PARTIAL PRICER, and that claim is only true while the
		 * fallback is dormant. Before FOracleResult::BlandDegenerateEntries existed that
		 * was an assumption from reading the code; these two lines make it an observation.
		 *
		 * WHAT THIS DOES NOT DO is exercise the fallback. No fixture in the suite is known
		 * to drive a 500-step degenerate streak, so the branch itself is covered by
		 * nothing — logged in CURRENT_STATE rather than papered over here. A zero that has
		 * never been anything else is a weak assertion on its own; its value is that the
		 * day a solver change starts grinding, these rows name the reason instead of
		 * reporting a mysteriously larger scan count.
		 */
		TestEqual(
			TEXT("40-course stack: the Bland anti-cycling fallback must stay dormant — the ")
			TEXT("scan and pivot figures on these rows measure the partial pricer, and only ")
			TEXT("do so while it has not stood aside"),
			Result.BlandDegenerateEntries, 0);
	}

	/* ---- Row B: the intact wall — the budgeted row. ------------------------------ */

	{
		constexpr int32 Courses = 8;
		constexpr int32 Cells = 10;

		FStructure Wall;
		FString Why;

		/* Its own statement first — a message reading what the condition writes is the
		 * unsequenced-evaluation trap TRAPS.md records. */
		const bool bLaid = BuildIntactWall(Courses, Cells, Wall, Why);

		if (!TestTrue(
				*FString::Printf(TEXT("wall: the producer must lay it (it said: %s)"), *Why),
				bLaid))
		{
			return true;
		}

		/*
		 * FIXTURE PRECONDITIONS. Four even courses of ten whole bricks and four flush odd
		 * courses of nine whole bricks plus two half bats — 84 pieces. If the producer
		 * ever lays a different wall, the budget below is a budget for a different
		 * problem, and this is where that must be noticed.
		 */
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

		/* ---- What the answer must be. ---- */

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

		/* The dormancy pin, for the reasons written out on row A. */
		TestEqual(
			TEXT("wall: the Bland anti-cycling fallback must stay dormant — the budgets ")
			TEXT("below are budgets for the partial pricer, and only are while it has not ")
			TEXT("stood aside"),
			Result.BlandDegenerateEntries, 0);

		/* ---- What the answer may cost. ---- */

		/*
		 * THE BUDGETS, MEASURED RATHER THAN CHOSEN, with the arithmetic written out so
		 * nobody has to re-derive it to know whether a number is reachable.
		 *
		 * FULL DANTZIG ON THIS FIXTURE, measured 2026-08-12: 84 blocks / 207 joints,
		 * 2,707 rows and 6,849 columns, lambda* in 1,942 pivots, 3,131,528 column scans,
		 * 4.716 s. Dividing those two straight through gives 1,612 columns per pivot, and
		 * THAT RATIO IS NOT THE PER-ITERATION COST: it charges the one-off artificial
		 * pivot-out pass (~343,000 scans, worked out below) to the pivot loop as though
		 * the loop had done it. Net of the pass the phase-2 loop prices 1,435 columns per
		 * iteration — exactly the non-basic structural set, 1,657 - 222, every iteration
		 * — and (3,131,528 - 343,000) / 1,942 = 1,436 recovers it from the measurement.
		 * That is the shape that leaves wall-01 (~13k x ~34k) still pivoting after 45
		 * minutes.
		 *
		 * WHERE THE FLOOR IS. The artificial pivot-out pass prices every non-artificial
		 * column once per basic artificial it clears. A gravity-live problem starts
		 * feasible, so phase 1 never runs and all 222 equality rows arrive at that pass
		 * with their artificial basic; the 1,657 structural columns are the non-basic ones
		 * it scans, one fewer each time it succeeds. That is 222 * 1657 - (0 + 1 + ... +
		 * 221) = ~343,000 scans, ~11% of today's total, and per-iteration pricing cannot
		 * touch any of it. The budget sits nearly three times above that floor on purpose:
		 * it is aimed at the 2.79M scans the phase-2 loop spends, not at the pass.
		 *
		 * 1,000,000 IS THEREFORE A 3.1x CUT OVERALL AND A 4.2x CUT ON THE PRICING LOOP,
		 * which no tuning of a full scan can reach and which a candidate list should beat
		 * by an order: a rotating window with a short queue prices tens of columns per
		 * iteration, not 1,435. It is deliberately not tighter than that — a budget set at
		 * the best imaginable implementation refuses good ones for being merely good.
		 *
		 * THE PIVOT BUDGET IS THE OTHER HALF OF THE SAME SENTENCE. Partial pricing buys
		 * cheap iterations by choosing worse entering columns, so it usually takes more of
		 * them; 4,000 is 2.06x today's 1,942, which absorbs that honestly while refusing a
		 * rule that pays for cheap scans with a grinding pivot path. Both numbers are
		 * deterministic — a wall-clock assertion here would flake on a busy machine and
		 * measure the machine rather than the algorithm.
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
