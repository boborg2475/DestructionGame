// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/RigidBlock/RigidBlockOracle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * E1b: asymmetric-moment sign lock (E1a-review finding 2). The E0-A tripod is X<->Y symmetric
 * with supports at one height, so a wrong Mz sign or an Mx<->My swap leaves it green. This
 * fixture has an off-diagonal CoM and supports at different heights under a horizontal push, so
 * Mz and the height coupling both bind.
 *
 * One free block, centroid (10, 20, 30), on five frictionless point patches (c = mu = 0, so the
 * pyramid caps shear at zero and each is a pure link: statically determinate).
 *
 *     THREE VERTICAL rollers (N = +Z), at plan (0,0), (40,0), (0,40)   -> normals nv1, nv2, nv3
 *     TWO HORIZONTAL rollers (N = -X, pushing the block in -X), at
 *         HxA: plan-y = 20, height z = 0                               -> normal Ra
 *         HxB: plan-y =  0, height z = 10  (different height)          -> normal Rb
 *
 * Loads: W = 9800 uu at C, and push P = 9800 uu in +X at (10, 10, 0); moment about C is
 * (0, -30P, +10P).
 *
 *     Fx : P - Ra - Rb = 0
 *     Mz : +10P (push) - 20*Rb (HxB at plan-y 0) = 0     -> Rb = P/2 = 4900,  Ra = P/2 = 4900
 *     Fz : nv1 + nv2 + nv3 = W
 *     Mx : -nv1 - nv2 + nv3 = 0                            -> nv3 = nv1 + nv2 = W/2 = 4900
 *     My : 10*nv1 - 30*nv2 + 10*nv3 - 30P + 30*Ra + 20*Rb = 0   -> nv1 = 3*nv2
 *
 *     => nv1 = 3W/8 = 3675 ,  nv2 = W/8 = 1225 ,  nv3 = W/2 = 4900 ,  Ra = Rb = 4900
 *
 * Bite 1 (Mx<->My swap in AppendThreeDContactCoeffs) moves the vertical normals off these values.
 * Bite 2 (negated contact Mz) gives Rb = -P/2, which a no-tension roller cannot supply.
 *
 * Arm (ii): all loads live, crush cap C = 0.01 MPa * 10000 * 98 cm2 = 9800 uu, so the three
 * 4900 uu reactions crush together at lambda* = 2.
 *
 * Red until E1b lands (pyramid plus posed applied force); without it free shear makes the system
 * indeterminate. Red after E1b would be a real sign defect. Units derived here. No ticking world.
 * Named namespace for the unity build.
 */
namespace AsymmetricMomentThreeDSupport
{
	using namespace RigidBlockOracle;

	constexpr double GravityCmPerSecondSquared = 980.0;
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;   // 1 MPa over 1 cm2 = 10000 uu

	constexpr double MassKg = 10.0;          // W = 9800 uu
	constexpr double AreaSqCm = 98.0;        // per support patch
	constexpr double ComXCm = 10.0;          // off-diagonal: cx != cy
	constexpr double ComYCm = 20.0;
	constexpr double ComZCm = 30.0;

	constexpr double PushUu = 9800.0;        // P = W, for round reactions

	// Five grounded supports (one per patch), one free block.
	enum { V1 = 0, V2 = 1, V3 = 2, HxA = 3, HxB = 4, Free = 5 };
	enum { JV1 = 0, JV2 = 1, JV3 = 2, JHxA = 3, JHxB = 4 };

	constexpr double CrushMPa = 0.01;        // C = 9800 uu; lambda* = 2

	double WeightUu() { return MassKg * GravityCmPerSecondSquared; }
	double CrushCapUu() { return CrushMPa * ForceUnitsPerMPaSqCmHere * AreaSqCm; }

	// Hand reactions, all compression.
	double Nv1Uu() { return 3.0 * WeightUu() / 8.0; }   // 3675
	double Nv2Uu() { return WeightUu() / 8.0; }         // 1225
	double Nv3Uu() { return WeightUu() / 2.0; }         // 4900
	double RaUu()  { return PushUu / 2.0; }             // 4900
	double RbUu()  { return PushUu / 2.0; }             // 4900

	/** Most-loaded reaction at lambda = 1, where crushing binds. */
	double MaxReactionUu() { return Nv3Uu(); }
	double ExpectedLoadFactor() { return CrushCapUu() / MaxReactionUu(); }   // 2

	FConnectionStrength Frictionless(double CompressiveMPa)
	{
		FConnectionStrength S;
		S.CompressiveStrengthMPa = CompressiveMPa;
		S.TensileStrengthMPa = 0.0;          // dry, no tension
		S.ShearCohesionMPa = 0.0;            // frictionless: zero shear capacity
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
		J.HalfUCm = 0.0;      // point patch: a single normal contact
		J.HalfVCm = 0.0;
		J.AreaSqCm = AreaSqCm;
		J.Strength = Strength;
		return J;
	}

	/**
	 * The five-support fixture. bLiveLoads false: dead loads, reactions read off the min-violation
	 * readout (use a generous cap). True: live loads, so crushing gives lambda*.
	 */
	FOracleProblem BuildProblem(bool bLiveLoads, double CompressiveMPa)
	{
		FOracleProblem P;
		P.Dim = EOracleDim::Dim3D;

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

		// Vertical rollers, contact plane z = 0.
		P.Joints.Add(PointPatch(V1, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, Bond));
		P.Joints.Add(PointPatch(V2, 0.0, 0.0, 1.0, 40.0, 0.0, 0.0, Bond));
		P.Joints.Add(PointPatch(V3, 0.0, 0.0, 1.0, 0.0, 40.0, 0.0, Bond));

		// Horizontal rollers at different heights; plan-x does not affect the moments.
		P.Joints.Add(PointPatch(HxA, -1.0, 0.0, 0.0, 30.0, 20.0, 0.0, Bond));
		P.Joints.Add(PointPatch(HxB, -1.0, 0.0, 0.0, 30.0, 0.0, 10.0, Bond));

		// Push (P,0,0) at (10,10,0): feeds Fx, My and Mz.
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

/*
 * The asymmetric-moment fixture matches hand statics: reactions and load factor. Red until E1b
 * lands, then a sign-locking guard (see the namespace comment). No ticking world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAsymmetricMomentThreeDMatchesHandStaticsTest,
	"DestructionGame.Oracle.RigidBlock.ThreeD.AsymmetricMomentMatchesHandStatics",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FAsymmetricMomentThreeDMatchesHandStaticsTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace AsymmetricMomentThreeDSupport;

	// Arm (i): the five reactions.
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

	// Arm (ii): lambda* = C / max reaction = 9800 / 4900 = 2.
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
