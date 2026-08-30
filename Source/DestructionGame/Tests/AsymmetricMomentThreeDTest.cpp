// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/RigidBlock/RigidBlockOracle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * E1b HARDENING — THE ASYMMETRIC-MOMENT SIGN LOCK (E1a-review finding 2), riding with the pyramid.
 *
 * WHY THIS FIXTURE EXISTS. The E0-A tripod (TripodThreeDTest.cpp) is X<->Y SYMMETRIC (CoM at
 * (L/4, L/4), supports at (0,0)/(L,0)/(0,L)) and puts every support at ONE height, so its normal
 * reactions are pinned by Fz, Mx, My alone — the Mz row and the shear->moment (height) coupling
 * are TRIVIALLY satisfied and never bind. A wrong Mz sign, or an Mx<->My swap, would leave the
 * tripod green. This second DETERMINATE 3D fixture removes both blind spots: an OFF-DIAGONAL CoM
 * (cx = 10 != cy = 20, breaking X<->Y) and supports at DIFFERENT HEIGHTS, driven by a horizontal
 * push, so a horizontal reaction carries net moment and Mz AND the U/V moment coupling BIND. Its
 * reactions and lambda* are exact hand statics, so nothing here launders a wrong sign into a
 * plausible number.
 *
 * THE FIXTURE — one FREE block, centroid C = (10, 20, 30), on FIVE FRICTIONLESS point-patch
 * supports (c = 0, mu = 0), plus a horizontal push. Frictionless is load-bearing: with the k=8
 * pyramid a c = mu = 0 support caps its shear at zero, so each carries ONLY its normal force — a
 * pure "link" — which is what makes the system statically DETERMINATE (five normal unknowns, five
 * non-trivial equilibrium equations). The supports:
 *
 *     THREE VERTICAL rollers (N = +Z), at plan (0,0), (40,0), (0,40)   -> normals nv1, nv2, nv3
 *     TWO HORIZONTAL rollers (N = -X, pushing the block in -X), at
 *         HxA: plan-y = 20, height z = 0                               -> normal Ra
 *         HxB: plan-y =  0, height z = 10  (DIFFERENT height)          -> normal Rb
 *
 * Loads: DEAD gravity W = M*980 = 9800 uu at C, and a push H = (P, 0, 0), P = 9800 uu, applied at
 * (10, 10, 0). H's moment about C is (0, -30P, +10P): it feeds Fx, My AND Mz.
 *
 * THE HAND STATICS (all six equilibrium equations, verified). The horizontal reactions decouple
 * (vertical forces give nothing to Fx, Fy, Mz):
 *
 *     Fx : P - Ra - Rb = 0
 *     Mz : +10P (push) - 20*Rb (HxB at plan-y 0) = 0     -> Rb = P/2 = 4900,  Ra = P/2 = 4900
 *
 * then the vertical normals, whose My equation carries the horizontal reactions' moment through
 * their DIFFERENT heights (30*Ra from HxA at z=0, 20*Rb from HxB at z=10):
 *
 *     Fz : nv1 + nv2 + nv3 = W
 *     Mx : -nv1 - nv2 + nv3 = 0                            -> nv3 = nv1 + nv2 = W/2 = 4900
 *     My : 10*nv1 - 30*nv2 + 10*nv3 - 30P + 30*Ra + 20*Rb = 0   -> nv1 = 3*nv2
 *
 *     => nv1 = 3W/8 = 3675 ,  nv2 = W/8 = 1225 ,  nv3 = W/2 = 4900 ,  Ra = Rb = 4900
 *
 * All five reactions are positive (compression) — a valid dry no-tension force system. They are
 * asymmetric in x and y (cx != cy), and Rb is pinned by the Mz balance, so:
 *
 *   BITE 1 (Mx<->My swap in AppendThreeDContactCoeffs): the vertical-normal equations use the x-
 *          and y-plan geometry, which differ (V2 at x=40 vs V3 at y=40, cx != cy); swapping the Mx
 *          and My coefficients scrambles them and moves (nv1, nv2, nv3) off (3675, 1225, 4900).
 *   BITE 2 (negate the contact Mz coefficients): the Mz balance becomes +10P + 20*Rb = 0 -> Rb =
 *          -P/2 < 0, which a no-tension roller cannot supply -> the readout can no longer reproduce
 *          the reactions (infeasible / relaxed), reddening the assertion.
 *
 * lambda* (arm ii): make gravity AND the push LIVE (both scale with lambda), and cap crushing on
 * every support at C = f_c*Conv*A = 0.01*10000*98 = 9800 uu. Every reaction scales linearly, the
 * most-loaded three (nv3, Ra, Rb = 4900) crush together, so lambda* = C / 4900 = 2 exactly.
 *
 * STATE — RED NOW, THE SIGN-LOCKING GUARD ONCE E1b LANDS. This fixture's determinacy DEPENDS on
 * the friction pyramid zeroing the frictionless supports' shear and on the 3D push being posed;
 * E1a supplies NEITHER, so against current production it is RED (free shear -> the system is
 * indeterminate, the min-violation readout lands on the wrong vertex, and lambda* is not 2). That
 * red is the ABSENCE of E1b, NOT a sign bug. Once dev lands E1b (pyramid + applied force) it turns
 * GREEN and thereafter GUARDS the moment signs the tripod cannot — the two bites above are what it
 * is worth. (If it were ever red AFTER E1b with the frictionless links determinate, THAT would be a
 * genuine sign defect to surface.)
 *
 * UNITS derived here, NOT imported. NEEDS A TICKING WORLD: NO. NAMED NAMESPACE for the unity build.
 */
namespace AsymmetricMomentThreeDSupport
{
	using namespace RigidBlockOracle;

	constexpr double GravityCmPerSecondSquared = 980.0;
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;   /* 1 MPa over 1 cm2 = 10000 uu */

	constexpr double MassKg = 10.0;          /* W = 9800 uu */
	constexpr double AreaSqCm = 98.0;        /* per support patch face */
	constexpr double ComXCm = 10.0;          /* OFF-DIAGONAL: cx != cy */
	constexpr double ComYCm = 20.0;
	constexpr double ComZCm = 30.0;

	constexpr double PushUu = 9800.0;        /* P = W, so the hand reactions come out round */

	/* Support / joint indexing. Five grounded supports (one per patch), one free block. */
	enum { V1 = 0, V2 = 1, V3 = 2, HxA = 3, HxB = 4, Free = 5 };
	enum { JV1 = 0, JV2 = 1, JV3 = 2, JHxA = 3, JHxB = 4 };

	constexpr double CrushMPa = 0.01;        /* C = 0.01*10000*98 = 9800 uu; lambda* = C/4900 = 2 */

	double WeightUu() { return MassKg * GravityCmPerSecondSquared; }
	double CrushCapUu() { return CrushMPa * ForceUnitsPerMPaSqCmHere * AreaSqCm; }

	/* The hand reactions (all compression, positive). */
	double Nv1Uu() { return 3.0 * WeightUu() / 8.0; }   /* 3675 */
	double Nv2Uu() { return WeightUu() / 8.0; }         /* 1225 */
	double Nv3Uu() { return WeightUu() / 2.0; }         /* 4900 */
	double RaUu()  { return PushUu / 2.0; }             /* 4900 */
	double RbUu()  { return PushUu / 2.0; }             /* 4900 */

	/** Most-loaded reaction at lambda = 1 (nv3 = Ra = Rb = 4900); crushing binds here. */
	double MaxReactionUu() { return Nv3Uu(); }
	double ExpectedLoadFactor() { return CrushCapUu() / MaxReactionUu(); }   /* 9800/4900 = 2 */

	FConnectionStrength Frictionless(double CompressiveMPa)
	{
		FConnectionStrength S;
		S.CompressiveStrengthMPa = CompressiveMPa;
		S.TensileStrengthMPa = 0.0;          /* dry no-tension */
		S.ShearCohesionMPa = 0.0;            /* frictionless: shear capacity is mu*n + c*A = 0 */
		S.FrictionCoefficient = 0.0;
		return S;
	}

	FOracleBlock Grounded(double X, double Y, double Z)
	{
		FOracleBlock B;
		B.MassKg = 1.0;
		B.CentroidXCm = X;
		B.CentroidYCm = Y;
		B.CentroidZCm = Z;
		B.bGrounded = true;
		return B;
	}

	FOracleBlock FreeBlock(bool bLiveGravity)
	{
		FOracleBlock B;
		B.MassKg = MassKg;
		B.CentroidXCm = ComXCm;
		B.CentroidYCm = ComYCm;
		B.CentroidZCm = ComZCm;
		B.bGrounded = false;
		B.bLiveGravity = bLiveGravity;
		return B;
	}

	FOracleJoint PointPatch(
		int32 SupportBlock, double Nx, double Ny, double Nz,
		double Cx, double Cy, double Cz, const FConnectionStrength& Strength)
	{
		FOracleJoint J;
		J.BlockA = SupportBlock;
		J.BlockB = Free;
		J.NormalX = Nx;
		J.NormalY = Ny;
		J.NormalZ = Nz;
		J.CentreXCm = Cx;
		J.CentreYCm = Cy;
		J.CentreZCm = Cz;
		J.HalfLengthCm = 0.0;
		J.HalfUCm = 0.0;      /* point patch: four coincident corners -> a single normal contact */
		J.HalfVCm = 0.0;
		J.AreaSqCm = AreaSqCm;
		J.Strength = Strength;
		return J;
	}

	/**
	 * The full five-support fixture. Compressive cap governs whether crushing can bind: generous
	 * (1000 MPa) for the reactions readout, the finite CrushMPa for the load factor.
	 */
	FOracleProblem BuildProblem(bool bLiveLoads, double CompressiveMPa)
	{
		FOracleProblem P;
		P.Dim = EOracleDim::Dim3D;

		/*
		 * bLiveLoads chooses the pose. FALSE: gravity dead and the push dead (both at lambda = 1),
		 * read the reactions off the min-violation readout. TRUE: gravity and push both LIVE, so
		 * every reaction scales with lambda and crushing at the most-loaded support gives lambda*.
		 */
		P.bGravityIsLive = bLiveLoads;
		P.bMinViolationReadout = !bLiveLoads;

		const FConnectionStrength Bond = Frictionless(CompressiveMPa);

		P.Blocks.SetNum(6);
		P.Blocks[V1] = Grounded(0.0, 0.0, -1.0);
		P.Blocks[V2] = Grounded(40.0, 0.0, -1.0);
		P.Blocks[V3] = Grounded(0.0, 40.0, -1.0);
		P.Blocks[HxA] = Grounded(31.0, 20.0, 0.0);
		P.Blocks[HxB] = Grounded(31.0, 0.0, 10.0);
		P.Blocks[Free] = FreeBlock(bLiveLoads);

		/* Three vertical rollers (N = +Z) at plan (0,0), (40,0), (0,40), contact plane z = 0. */
		P.Joints.Add(PointPatch(V1, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, Bond));
		P.Joints.Add(PointPatch(V2, 0.0, 0.0, 1.0, 40.0, 0.0, 0.0, Bond));
		P.Joints.Add(PointPatch(V3, 0.0, 0.0, 1.0, 0.0, 40.0, 0.0, Bond));

		/* Two horizontal rollers (N = -X) at DIFFERENT heights; plan-x is immaterial to the moments. */
		P.Joints.Add(PointPatch(HxA, -1.0, 0.0, 0.0, 30.0, 20.0, 0.0, Bond));
		P.Joints.Add(PointPatch(HxB, -1.0, 0.0, 0.0, 30.0, 0.0, 10.0, Bond));

		/* The horizontal push H = (P,0,0) at (10,10,0): feeds Fx, My and Mz. */
		FOracleAppliedForce Push;
		Push.Block = Free;
		Push.ForceXUu = PushUu;
		Push.ForceYUu = 0.0;
		Push.ForceZUu = 0.0;
		Push.AtXCm = 10.0;
		Push.AtYCm = 10.0;
		Push.AtZCm = 0.0;
		Push.bLive = bLiveLoads;
		P.AppliedForces.Add(Push);

		return P;
	}

	double NormalOf(const FOracleReadout& R, int32 J)
	{
		return R.Joints.IsValidIndex(J) ? R.Joints[J].NormalUu : 0.0;
	}

	bool Near(double A, double B, double Tol) { return FMath::Abs(A - B) <= Tol; }
}

/* ================================================================================================
 * THE ASYMMETRIC-MOMENT FIXTURE MATCHES HAND STATICS — reactions AND load factor.
 *
 * RED NOW: E1b (the friction pyramid that zeroes the frictionless supports' shear, and the 3D
 * applied-force posing) does not exist yet, so the system is indeterminate and neither the
 * reactions nor lambda* match. GREEN once E1b lands, thereafter a sign-locking regression guard
 * (BITE 1 Mx<->My swap; BITE 2 negate contact Mz — see the header).
 *
 * NEEDS A TICKING WORLD: NO.
 * ================================================================================================ */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAsymmetricMomentThreeDMatchesHandStaticsTest,
	"DestructionGame.Oracle.RigidBlock.ThreeD.AsymmetricMomentMatchesHandStatics",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAsymmetricMomentThreeDMatchesHandStaticsTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace AsymmetricMomentThreeDSupport;

	/* ---------------- ARM (i): the five reactions ---------------- */
	{
		const FOracleProblem P = BuildProblem(/*bLiveLoads*/ false, /*CompressiveMPa*/ 1000.0);
		const FOracleResult R = SolveRigidBlock(P);

		AddInfo(FString::Printf(
			TEXT("REACTIONS: answered %d, readout present %d; got (nv1 %.6g, nv2 %.6g, nv3 %.6g, ")
			TEXT("Ra %.6g, Rb %.6g), expected (%.6g, %.6g, %.6g, %.6g, %.6g)"),
			R.bAnswered ? 1 : 0, R.Readout.bPresent ? 1 : 0,
			NormalOf(R.Readout, JV1), NormalOf(R.Readout, JV2), NormalOf(R.Readout, JV3),
			NormalOf(R.Readout, JHxA), NormalOf(R.Readout, JHxB),
			Nv1Uu(), Nv2Uu(), Nv3Uu(), RaUu(), RbUu()));

		TestTrue(TEXT("REACTIONS: the oracle answers"), R.bAnswered);
		TestTrue(TEXT("REACTIONS [RED]: the readout is present"), R.Readout.bPresent);

		const double Tol = 1.0e-4 * Nv3Uu();

		TestTrue(
			*FString::Printf(TEXT("REACTIONS [RED]: nv1 = 3W/8, got %.6g == %.6g"),
				NormalOf(R.Readout, JV1), Nv1Uu()),
			Near(NormalOf(R.Readout, JV1), Nv1Uu(), Tol));
		TestTrue(
			*FString::Printf(TEXT("REACTIONS [RED]: nv2 = W/8, got %.6g == %.6g"),
				NormalOf(R.Readout, JV2), Nv2Uu()),
			Near(NormalOf(R.Readout, JV2), Nv2Uu(), Tol));
		TestTrue(
			*FString::Printf(TEXT("REACTIONS [RED]: nv3 = W/2, got %.6g == %.6g"),
				NormalOf(R.Readout, JV3), Nv3Uu()),
			Near(NormalOf(R.Readout, JV3), Nv3Uu(), Tol));
		TestTrue(
			*FString::Printf(TEXT("REACTIONS [RED]: Ra = P/2 (pinned by Mz), got %.6g == %.6g"),
				NormalOf(R.Readout, JHxA), RaUu()),
			Near(NormalOf(R.Readout, JHxA), RaUu(), Tol));
		TestTrue(
			*FString::Printf(TEXT("REACTIONS [RED]: Rb = P/2 (pinned by Mz), got %.6g == %.6g"),
				NormalOf(R.Readout, JHxB), RbUu()),
			Near(NormalOf(R.Readout, JHxB), RbUu(), Tol));
	}

	/* ---------------- ARM (ii): lambda* = C / max reaction = 9800 / 4900 = 2 ---------------- */
	{
		const FOracleProblem P = BuildProblem(/*bLiveLoads*/ true, /*CompressiveMPa*/ CrushMPa);
		const FOracleResult R = SolveRigidBlock(P);

		AddInfo(FString::Printf(
			TEXT("LOAD FACTOR: answered %d, got lambda* %.9g, expected C/max = %.9g (C = %.6g, max = %.6g)"),
			R.bAnswered ? 1 : 0, R.Lambda, ExpectedLoadFactor(), CrushCapUu(), MaxReactionUu()));

		TestTrue(TEXT("LOAD FACTOR: the oracle answers"), R.bAnswered);

		const double Tol = 1.0e-4 * ExpectedLoadFactor();
		TestTrue(
			*FString::Printf(TEXT("LOAD FACTOR [RED]: lambda* = C/max reaction, got %.9g == %.9g"),
				R.Lambda, ExpectedLoadFactor()),
			Near(R.Lambda, ExpectedLoadFactor(), Tol));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
