// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/RigidBlock/RigidBlockOracle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * E1a (E0-A, the tripod): the first 3D rigid-block LP slice. A determinate fixture, so its
 * reactions are exact hand statics with no LP freedom to hide a wrong answer.
 *
 * One free block of weight W on three grounded point-patch supports (+Z normal, zero half-extents):
 *
 *     P1 = (0, 0)     P2 = (L, 0)     P3 = (0, L)      (plan positions, L = 40 cm)
 *     centre of mass at (cx, cy) = (L/4, L/4) = (10, 10)
 *
 * P1 and P3 differ only in Y, so a 2D X-Z model cannot tell R1 from R3. Vertical reactions follow
 * from Sigma Fz, Mx and My alone:
 *
 *     R2 = W * cx / L,  R3 = W * cy / L,  R1 = W * (1 - cx/L - cy/L)
 *
 * so R1 = W/2 and R2 = R3 = W/4. With 10 kg, W = 9800 uu.
 *
 * Arm (i), reactions: with generous strengths the min-violation readout's NormalUu per joint is the
 * reaction. Arm (ii), load factor: with only a crushing cap C per support, lambda* = C / (W/2).
 * f_c = 0.01 MPa over 98 cm2 gives C = 9800 uu = W, so lambda* = 2. The 2D projection gives 8/3.
 *
 * Units are derived here, not imported. No world needed. Named namespace for the unity build.
 */
namespace TripodThreeDSupport
{
	using namespace RigidBlockOracle;

	/** MassKg * 980 is a weight in uu; the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** 1 MPa over 1 cm2 is 10000 uu. Not imported, so a wrong production constant fails. */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	constexpr double MassKg = 10.0;                 // W = 9800 uu
	constexpr double LCm = 40.0;                    // support span; CoM at L/4
	constexpr double ComXCm = LCm / 4.0;
	constexpr double ComYCm = LCm / 4.0;
	constexpr double ComZCm = 10.0;                 // height is irrelevant to the reactions
	constexpr double ContactZCm = 5.0;              // support tops, below the CoM
	constexpr double AreaSqCm = 98.0;               // per support patch face

	double WeightUu() { return MassKg * GravityCmPerSecondSquared; }

	double R1Uu() { return WeightUu() / 2.0; }
	double R2Uu() { return WeightUu() / 4.0; }
	double R3Uu() { return WeightUu() / 4.0; }

	enum { S1 = 0, S2 = 1, S3 = 2, Free = 3 };   // block indices
	enum { J1 = 0, J2 = 1, J3 = 2 };             // joint indices, one per support

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
		J.HalfLengthCm = 0.0;   // 2D point contact, for fields the 2D path still reads
		J.HalfUCm = 0.0;        // both half-extents zero: a single normal contact
		J.HalfVCm = 0.0;
		J.AreaSqCm = AreaSqCm;
		J.Strength = Strength;
		return J;
	}

	/** Dry no-tension, otherwise generous, so nothing binds and arm (i) reads pure equilibrium. */
	FConnectionStrength GenerousBond()
	{
		FConnectionStrength S;
		S.CompressiveStrengthMPa = 1000.0;
		S.TensileStrengthMPa = 0.0;          // dry no-tension: reactions stay >= 0
		S.ShearCohesionMPa = 1000.0;
		S.FrictionCoefficient = 1000.0;
		return S;
	}

	/** The load-factor bond: only crushing can govern, capped at C = f_c * Conv * A per support. */
	constexpr double CrushMPa = 0.01;   // C = 0.01 * 10000 * 98 = 9800 uu = W, so 2C/W = 2

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
		P.bGravityIsLive = false;      // lambda fixed at 1: gravity is the dead load
		P.bMinViolationReadout = true; // per-joint reaction readout

		const FConnectionStrength Bond = GenerousBond();
		P.Blocks.SetNum(4);
		P.Blocks[S1] = GroundedSupport(0.0, 0.0);
		P.Blocks[S2] = GroundedSupport(LCm, 0.0);
		P.Blocks[S3] = GroundedSupport(0.0, LCm);
		P.Blocks[Free] = FreeBlock();

		P.Joints.Add(PointPatch(S1, 0.0, 0.0, Bond));   // J1 -> R1 = W/2
		P.Joints.Add(PointPatch(S2, LCm, 0.0, Bond));   // J2 -> R2 = W/4
		P.Joints.Add(PointPatch(S3, 0.0, LCm, Bond));   // J3 -> R3 = W/4
		return P;
	}

	FOracleProblem BuildLoadFactor()
	{
		FOracleProblem P;
		P.Dim = EOracleDim::Dim3D;
		P.bGravityIsLive = true;   // lambda* = how many times its own weight it can carry

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

/*
 * The tripod matches hand statics: reactions (W/2, W/4, W/4) and lambda* = 2. A 2D solve reads 8/3,
 * and zeroing the Mx or My coefficients in AssembleThreeD breaks the split.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTripodThreeDMatchesHandStaticsTest,
	"DestructionGame.Oracle.RigidBlock.ThreeD.TripodMatchesHandStatics",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FTripodThreeDMatchesHandStaticsTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace TripodThreeDSupport;

	// Arm (i): the three reactions are (W/2, W/4, W/4).
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

		// Tight tolerance: the 3D statics are exact.
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

	// Arm (ii): lambda* = 2C/W = 2, crushing at the most-loaded support.
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
