// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/RigidBlock/RigidBlockOracle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * E1a (E0-A, THE TRIPOD) — THE FIRST 3D RIGID-BLOCK LP SLICE, RED.
 *
 * The four-walls-and-roof shed watched toppling in Chaos needs a 3D equilibrium LP; today's oracle
 * is 2D (the X-Z plane, Y entering only through joint areas). E1a lights up the Dim3D flag and its
 * assembler; this test is the red that drives it, and E0-A is the fixture the design chose because
 * it is DETERMINATE — its reactions are exact hand statics, with no LP freedom to launder a wrong
 * answer into a plausible one.
 *
 * THE FIXTURE — one FREE block of weight W resting on THREE grounded point-patch supports:
 *
 *     P1 = (0, 0)     P2 = (L, 0)     P3 = (0, L)      (support plan positions, L = 40 cm)
 *     free block centre of mass at (cx, cy) = (L/4, L/4) = (10, 10)
 *
 * Each support-to-block joint is a POINT PATCH: unit normal N = +Z (NormalX = NormalY = 0,
 * NormalZ = 1) with BOTH in-plane half-extents zero (HalfUCm = HalfVCm = 0), so its four corners
 * coincide into a single normal contact — the 3D analogue of the 2D HalfLengthCm = 0 point contact.
 * Dim = Dim3D. Nothing here is expressible in 2D: the three supports differ only in their plan-Y
 * (P1 and P3 share X = 0), so the X-Z model literally cannot tell R1 from R3.
 *
 * THE HAND STATICS (a vertical force f = (0, 0, n) at plan offset r = (Rx, Ry, *) from the centroid
 * has moment r x f = (Ry*n, -Rx*n, 0), independent of height — so the vertical reactions are fixed
 * by Sigma Fz, Sigma Mx, Sigma My alone):
 *
 *     R2 = W * cx / L,  R3 = W * cy / L,  R1 = W * (1 - cx/L - cy/L)
 *
 * At (cx, cy) = (L/4, L/4):  R1 = W/2,  R2 = R3 = W/4  — unique, positive, exact, and IRREDUCIBLY
 * 3D (both Mx and My bind; drop either and the split is lost). With W = mass * 980 and mass = 10 kg,
 * W = 9800 uu, so R1 = 4900, R2 = R3 = 2450 uu.
 *
 * TWO ARMS (THREED_DESIGN E0-A (i)/(ii)):
 *
 *   (i)  REACTIONS. With generous strengths the tripod STANDS; posed as the min-violation readout at
 *        real self-weight, the per-joint normal force NormalUu IS the reaction it carries. Assert
 *        the three read (R1, R2, R3) = (W/2, W/4, W/4). (This is how the 2D readout tests read a
 *        joint reaction — FOracleResult::Readout[j].NormalUu, compression positive.)
 *
 *   (ii) LOAD FACTOR. Give each support a crushing cap C and nothing else that can bind (dry
 *        no-tension, huge cohesion/friction, no shear present anyway), gravity live. The block
 *        stands iff lambda* >= 1, and since crushing binds first at the MOST loaded support,
 *        lambda* = C / max(R_i) = C / (W/2) = 2C/W. With A = 98 cm^2 and f_c = 0.01 MPa,
 *        C = f_c * Conv * A = 0.01 * 10000 * 98 = 9800 uu = W, so 2C/W = 2 exactly.
 *
 * WHY THIS IS RED, AND FOR THE RIGHT REASON. There is no AssembleThreeD yet: SolveRigidBlock does
 * not branch on Problem.Dim, so a Dim3D problem falls through to the 2D assembler, which reads only
 * X and Z. In the 2D projection P1 and P3 collapse onto the SAME point (X = 0, same height) —
 * identical columns — so the model cannot separate R1 from R3:
 *
 *     2D reactions: Sigma Fz + the one X-Z moment give R2 = W/4 (coincidentally right) but leave
 *                   R1 + R3 = 3W/4 a FREE split; the degenerate LP lands on a vertex (all on one),
 *                   so at least one of R1, R3 is wrong. Arm (i) reds on J0 and/or J2.
 *
 *     2D load factor: R2 = lambda*W/4 and R1 + R3 = 3*lambda*W/4 with each <= C; maximising lambda
 *                   spreads R1 = R3 = 3*lambda*W/8 (> W/4), so crushing binds at 3*lambda*W/8 = C,
 *                   giving lambda* = 8C/(3W) = 8/3 ~ 2.667, NOT the true 2. Arm (ii) reds.
 *
 * So the failure is the un-assembled Dim3D path returning wrong statics — not a compile error, not a
 * broken fixture. The assertion is on the MECHANISM (reactions / lambda*), never displacement.
 *
 * UNITS derived here (1 MPa over 1 cm^2 = 10000 uu; mass * 980 already carries the 1 N = 100 uu
 * conversion), NOT imported, so a wrong production constant disagrees rather than agrees.
 *
 * NEEDS A TICKING WORLD: NO. A pure hand-built FOracleProblem fed to SolveRigidBlock — no bridge,
 * no Chaos, no world tick; E0/E1 are core-only.
 *
 * NAMED NAMESPACE, not anonymous: the unity build merges many files into one translation unit.
 */
namespace TripodThreeDSupport
{
	using namespace RigidBlockOracle;

	/* ================================================================================
	 * UNITS, derived here so a wrong production constant fails rather than agrees.
	 * ================================================================================ */

	/** MassKg * 980 is a weight in uu — the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa over 1 cm2 is 10000 uu. NOT imported. */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	/* ================================================================================
	 * THE TRIPOD, hand-built so the statics stay checkable.
	 * ================================================================================ */

	constexpr double MassKg = 10.0;                 /* W = 9800 uu */
	constexpr double LCm = 40.0;                    /* support span; CoM at L/4 */
	constexpr double ComXCm = LCm / 4.0;            /* 10 */
	constexpr double ComYCm = LCm / 4.0;            /* 10 */
	constexpr double ComZCm = 10.0;                 /* height is irrelevant to the reactions */
	constexpr double ContactZCm = 5.0;              /* support tops, below the CoM */
	constexpr double AreaSqCm = 98.0;               /* per support patch face */

	double WeightUu() { return MassKg * GravityCmPerSecondSquared; }

	/* Hand statics: R1 = W/2 at (0,0); R2 = R3 = W/4 at (L,0) and (0,L). */
	double R1Uu() { return WeightUu() / 2.0; }
	double R2Uu() { return WeightUu() / 4.0; }
	double R3Uu() { return WeightUu() / 4.0; }

	enum { S1 = 0, S2 = 1, S3 = 2, Free = 3 };   /* block indices */
	enum { J1 = 0, J2 = 1, J3 = 2 };             /* joint indices, one per support */

	FOracleBlock GroundedSupport(double PlanXCm, double PlanYCm)
	{
		FOracleBlock B;
		B.MassKg = 1.0;
		B.CentroidXCm = PlanXCm;
		B.CentroidZCm = ContactZCm / 2.0;
		B.CentroidYCm = PlanYCm;
		B.bGrounded = true;
		return B;
	}

	FOracleBlock FreeBlock()
	{
		FOracleBlock B;
		B.MassKg = MassKg;
		B.CentroidXCm = ComXCm;
		B.CentroidZCm = ComZCm;
		B.CentroidYCm = ComYCm;
		B.bGrounded = false;
		return B;
	}

	/** A +Z point-patch joint from grounded support A up into free block B, with a strength. */
	FOracleJoint PointPatch(int32 SupportBlock, double PlanXCm, double PlanYCm, const FConnectionStrength& Strength)
	{
		FOracleJoint J;
		J.BlockA = SupportBlock;
		J.BlockB = Free;
		J.NormalX = 0.0;
		J.NormalY = 0.0;
		J.NormalZ = 1.0;
		J.CentreXCm = PlanXCm;
		J.CentreYCm = PlanYCm;
		J.CentreZCm = ContactZCm;
		J.HalfLengthCm = 0.0;   /* 2D point contact, for the fields the 2D path still reads */
		J.HalfUCm = 0.0;        /* both in-plane half-extents zero => single normal contact */
		J.HalfVCm = 0.0;
		J.AreaSqCm = AreaSqCm;
		J.Strength = Strength;
		return J;
	}

	/**
	 * Dry, no-tension, generous elsewhere: under the tripod's pure vertical load every reaction is
	 * compression and no axis but (an absent) crushing can bind, so the block stands with a real,
	 * unique force system whose per-joint NormalUu is the reaction. Crushing left effectively
	 * uncapped so arm (i) reads equilibrium, not a cap.
	 */
	FConnectionStrength GenerousBond()
	{
		FConnectionStrength S;
		S.CompressiveStrengthMPa = 1000.0;   /* >> the tiny reactions here */
		S.TensileStrengthMPa = 0.0;          /* dry no-tension: reactions stay >= 0 */
		S.ShearCohesionMPa = 1000.0;
		S.FrictionCoefficient = 1000.0;
		return S;
	}

	/**
	 * The load-factor bond: dry no-tension, huge cohesion/friction so ONLY crushing can govern, and
	 * a finite crushing cap C = f_c * Conv * A per support. There is no shear in this fixture, so
	 * the huge cohesion/friction are belt-and-braces; crushing at the most-loaded support decides.
	 */
	constexpr double CrushMPa = 0.01;   /* C = 0.01 * 10000 * 98 = 9800 uu = W, so 2C/W = 2 */

	double CrushCapUu() { return CrushMPa * ForceUnitsPerMPaSqCmHere * AreaSqCm; }

	FConnectionStrength CrushingBond()
	{
		FConnectionStrength S;
		S.CompressiveStrengthMPa = CrushMPa;
		S.TensileStrengthMPa = 0.0;
		S.ShearCohesionMPa = 1000.0;
		S.FrictionCoefficient = 1000.0;
		return S;
	}

	/** The block stands iff lambda* >= 1; lambda* = C / max(R_i) = C / (W/2) = 2C/W. */
	double ExpectedLoadFactor() { return CrushCapUu() / R1Uu(); }

	FOracleProblem BuildReactions()
	{
		FOracleProblem P;
		P.Dim = EOracleDim::Dim3D;
		P.bGravityIsLive = false;      /* load fixed at lambda = 1: gravity is the dead load */
		P.bMinViolationReadout = true; /* ask for the per-joint reaction readout */

		const FConnectionStrength Bond = GenerousBond();
		P.Blocks.SetNum(4);
		P.Blocks[S1] = GroundedSupport(0.0, 0.0);
		P.Blocks[S2] = GroundedSupport(LCm, 0.0);
		P.Blocks[S3] = GroundedSupport(0.0, LCm);
		P.Blocks[Free] = FreeBlock();

		P.Joints.Add(PointPatch(S1, 0.0, 0.0, Bond));   /* J1 -> R1 = W/2 */
		P.Joints.Add(PointPatch(S2, LCm, 0.0, Bond));   /* J2 -> R2 = W/4 */
		P.Joints.Add(PointPatch(S3, 0.0, LCm, Bond));   /* J3 -> R3 = W/4 */
		return P;
	}

	FOracleProblem BuildLoadFactor()
	{
		FOracleProblem P;
		P.Dim = EOracleDim::Dim3D;
		P.bGravityIsLive = true;   /* gravity is the live load: lambda* = "how many times its weight" */

		const FConnectionStrength Bond = CrushingBond();
		P.Blocks.SetNum(4);
		P.Blocks[S1] = GroundedSupport(0.0, 0.0);
		P.Blocks[S2] = GroundedSupport(LCm, 0.0);
		P.Blocks[S3] = GroundedSupport(0.0, LCm);
		P.Blocks[Free] = FreeBlock();

		P.Joints.Add(PointPatch(S1, 0.0, 0.0, Bond));
		P.Joints.Add(PointPatch(S2, LCm, 0.0, Bond));
		P.Joints.Add(PointPatch(S3, 0.0, LCm, Bond));
		return P;
	}

	double NormalOf(const FOracleReadout& R, int32 J)
	{
		return R.Joints.IsValidIndex(J) ? R.Joints[J].NormalUu : 0.0;
	}

	bool Near(double A, double B, double Tol) { return FMath::Abs(A - B) <= Tol; }
}

/* ================================================================================================
 * THE TRIPOD MATCHES HAND STATICS — E0-A (i) reactions AND (ii) load factor, both RED.
 *
 * RED BECAUSE the Dim3D problem is never assembled in 3D: SolveRigidBlock has no branch on
 * Problem.Dim, so it drops Y and solves a degenerate 2D projection in which P1 and P3 coincide.
 * Arm (i): J1 and/or J3 read a reaction that is not (W/2, W/4). Arm (ii): lambda* reads 8/3, not 2.
 *
 * BITE (for dev, at green): zeroing the Mx or My moment coefficients in AssembleThreeD reds this
 * fixture — both must bind for the (W/2, W/4, W/4) split to exist.
 *
 * NEEDS A TICKING WORLD: NO.
 * ================================================================================================ */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTripodThreeDMatchesHandStaticsTest,
	"DestructionGame.Oracle.RigidBlock.ThreeD.TripodMatchesHandStatics",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FTripodThreeDMatchesHandStaticsTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace TripodThreeDSupport;

	/* ---------------- ARM (i): the three reactions are (W/2, W/4, W/4) ---------------- */
	{
		const FOracleProblem P = BuildReactions();
		const FOracleResult R = SolveRigidBlock(P);

		AddInfo(FString::Printf(
			TEXT("REACTIONS: answered %d, readout present %d; got (R1 %.6g, R2 %.6g, R3 %.6g), ")
			TEXT("expected (%.6g, %.6g, %.6g)"),
			R.bAnswered ? 1 : 0, R.Readout.bPresent ? 1 : 0,
			NormalOf(R.Readout, J1), NormalOf(R.Readout, J2), NormalOf(R.Readout, J3),
			R1Uu(), R2Uu(), R3Uu()));

		TestTrue(TEXT("REACTIONS: the oracle answers"), R.bAnswered);
		TestTrue(TEXT("REACTIONS [RED]: the readout is present"), R.Readout.bPresent);

		/* Absolute tolerance a hair of the smallest reaction; the 3D statics are exact. */
		const double Tol = 1.0e-4 * R1Uu();

		TestTrue(
			*FString::Printf(TEXT("REACTIONS [RED]: J1 carries R1 = W/2, got %.6g == %.6g"),
				NormalOf(R.Readout, J1), R1Uu()),
			Near(NormalOf(R.Readout, J1), R1Uu(), Tol));
		TestTrue(
			*FString::Printf(TEXT("REACTIONS [RED]: J2 carries R2 = W/4, got %.6g == %.6g"),
				NormalOf(R.Readout, J2), R2Uu()),
			Near(NormalOf(R.Readout, J2), R2Uu(), Tol));
		TestTrue(
			*FString::Printf(TEXT("REACTIONS [RED]: J3 carries R3 = W/4, got %.6g == %.6g"),
				NormalOf(R.Readout, J3), R3Uu()),
			Near(NormalOf(R.Readout, J3), R3Uu(), Tol));
	}

	/* ---------------- ARM (ii): lambda* = 2C/W = 2 (crushing at the most-loaded support) ---------------- */
	{
		const FOracleProblem P = BuildLoadFactor();
		const FOracleResult R = SolveRigidBlock(P);

		AddInfo(FString::Printf(
			TEXT("LOAD FACTOR: answered %d, got lambda* %.9g, expected 2C/W = %.9g (C = %.6g, W = %.6g)"),
			R.bAnswered ? 1 : 0, R.Lambda, ExpectedLoadFactor(), CrushCapUu(), WeightUu()));

		TestTrue(TEXT("LOAD FACTOR: the oracle answers"), R.bAnswered);

		const double Tol = 1.0e-4 * ExpectedLoadFactor();
		TestTrue(
			*FString::Printf(TEXT("LOAD FACTOR [RED]: lambda* = 2C/W, got %.9g == %.9g"),
				R.Lambda, ExpectedLoadFactor()),
			Near(R.Lambda, ExpectedLoadFactor(), Tol));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
