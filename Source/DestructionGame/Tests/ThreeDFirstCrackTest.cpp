// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/RigidBlock/RigidBlockOracle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The 3D assembly honours bFirstCrackRows as the 2D one does (review item 8, defect 2; DESIGN §8
 * brittleness precondition). Below the block cap BreakByEquilibrium sets the flag, so a bonded
 * joint in bending cracks at its peak-fibre limit `-(n1+n2) + 3|n1-n2| <= f_t*A`, three times
 * stricter than the plastic no-tension form. Without it, 3D would read up to 3x more permissive.
 *
 * Fixture: one free block cantilevering off one bonded bed joint (normal +Z), weight live and
 * eccentric, so the joint is in pure bending about V. Uniaxial bending reduces the 3D condition to
 * the 2D two-point form, so 3D lambda* must equal 2D's exactly.
 *
 *     Face area A = 200 cm2 (2*HalfU=20 by 2*HalfV=10), Conv = 10000 uu/(MPa.cm2), f_t = 0.7 MPa.
 *     Half-length h = HalfU = 10 cm. Centroid eccentricity e = 20 cm (overhangs the +h contact by
 *     10 cm). Weight W = 700,000 uu (MassKg = W/980), gravity live.
 *
 * Hand statics about the joint centre, contacts at u = +/-h: n1 + n2 = lambda*W and
 * n2 - n1 = 2*lambda*W, so n1 = -lambda*W/2 (tension) and n2 = 3*lambda*W/2.
 *
 *     PLASTIC:     n1 >= -f_t*Conv*A/2 => lambda*_plastic = f_t*Conv*A/W = 1.4e6/0.7e6 = 2.0 -> stands
 *     FIRST CRACK: 5*lambda*W <= f_t*Conv*A => lambda*_firstcrack = f_t*Conv*A/(5*W) = 0.4    -> falls
 *
 * The two straddle 1.0 by design. Both arms set bFirstCrackRows (the production below-cap pose):
 * the 2D control falls at 0.4, and 3D must too; ignoring the flag would stand at 2.0.
 *
 * Asserts the LP outcome, never displacement. Units written out. World-free.
 */
namespace ThreeDFirstCrackSupport
{
	using namespace RigidBlockOracle;

	constexpr double GravityCmPerSecondSquared = 980.0;
	constexpr double ConvUuPerMPaSqCm = 100.0 * 100.0;   /* 10000 uu per MPa.cm2 */

	constexpr double AreaSqCm = 200.0;
	constexpr double HalfUCm = 10.0;      /* h: contacts at u = +/-10 */
	constexpr double HalfVCm = 5.0;       /* uniform across V: 4*HalfU*HalfV = 200 = AreaSqCm */
	constexpr double EccentricityCm = 20.0;   /* e/h = 2 */

	constexpr double TensileMPa = 0.7;
	constexpr double CompressiveMPa = 10.0;
	constexpr double CohesionMPa = 0.9;
	constexpr double FrictionMu = 0.75;
	constexpr double MaxShearMPa = 2.0;

	double WeightUu() { return TensileMPa * ConvUuPerMPaSqCm * AreaSqCm / 2.0; }   /* 700,000, K = f_t*A/W = 2 */
	double BlockMassKg() { return WeightUu() / GravityCmPerSecondSquared; }

	/* Plastic 2.0, first crack 0.4. */
	double ExpectedPlasticLambda()    { return TensileMPa * ConvUuPerMPaSqCm * AreaSqCm / WeightUu(); }
	double ExpectedFirstCrackLambda() { return TensileMPa * ConvUuPerMPaSqCm * AreaSqCm / (5.0 * WeightUu()); }

	FConnectionStrength Mortar()
	{
		FConnectionStrength S;
		S.CompressiveStrengthMPa = CompressiveMPa;
		S.ShearCohesionMPa = CohesionMPa;
		S.TensileStrengthMPa = TensileMPa;
		S.FrictionCoefficient = FrictionMu;
		S.MaxShearStrengthMPa = MaxShearMPa;
		return S;
	}

	enum { Ground = 0, Free = 1 };

	/** The same cantilever posed 2D (X-Z) or 3D (Dim3D), both with first crack asked for. */
	FOracleProblem BuildProblem(EOracleDim Dim)
	{
		FOracleProblem P;
		P.Dim = Dim;
		P.bGravityIsLive = true;
		P.bFirstCrackRows = true;   /* the production below-cap pose */

		FOracleBlock G;
		G.MassKg = 1.0;
		G.CentroidXCm = 0.0;
		G.CentroidYCm = 0.0;
		G.CentroidZCm = -1.0;
		G.bGrounded = true;

		FOracleBlock B;
		B.MassKg = BlockMassKg();
		B.CentroidXCm = EccentricityCm;   /* overhangs the joint's +h edge by e - h = 10 cm */
		B.CentroidYCm = 0.0;
		B.CentroidZCm = 10.0;
		B.bGrounded = false;

		P.Blocks.SetNum(2);
		P.Blocks[Ground] = G;
		P.Blocks[Free] = B;

		FOracleJoint J;
		J.BlockA = Ground;
		J.BlockB = Free;
		J.NormalX = 0.0;
		J.NormalY = 0.0;
		J.NormalZ = 1.0;
		J.CentreXCm = 0.0;
		J.CentreYCm = 0.0;
		J.CentreZCm = 0.0;
		J.HalfLengthCm = HalfUCm;   /* 2D: two contacts at +/-h along the X tangent */
		J.HalfUCm = HalfUCm;        /* 3D: four corners at (+/-HalfU, +/-HalfV) */
		J.HalfVCm = HalfVCm;
		J.AreaSqCm = AreaSqCm;
		J.Strength = Mortar();
		P.Joints.Add(J);

		return P;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FThreeDFirstCrackNoMorePermissiveTest,
	"DestructionGame.Oracle.RigidBlock.ThreeD.FirstCrackRowsAreNoMorePermissiveThan2D",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FThreeDFirstCrackNoMorePermissiveTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace ThreeDFirstCrackSupport;

	/* ---------------- ARM 2D (control): first crack falls the corbel at lambda* = 0.4 ---------------- */
	const FOracleProblem P2D = BuildProblem(EOracleDim::Dim2D);
	const FOracleResult R2D = SolveRigidBlock(P2D);

	AddInfo(FString::Printf(
		TEXT("2D CONTROL: answered %d, lambda* %.10g (expected first-crack %.6g; plastic would be %.6g)"),
		R2D.bAnswered ? 1 : 0, R2D.Lambda, ExpectedFirstCrackLambda(), ExpectedPlasticLambda()));

	TestTrue(TEXT("2D CONTROL: the oracle answers"), R2D.bAnswered);
	TestEqual(TEXT("2D CONTROL: with first crack the bonded corbel FALLS (proves the fixture bends)"),
		static_cast<int32>(OutcomeOf(R2D)),
		static_cast<int32>(EOracleOutcome::Falls));

	/* ---------------- ARM 3D: the same joint must be no more permissive ---------------- */
	const FOracleProblem P3D = BuildProblem(EOracleDim::Dim3D);
	const FOracleResult R3D = SolveRigidBlock(P3D);

	AddInfo(FString::Printf(
		TEXT("3D: answered %d, lambda* %.10g. Expected == the 2D first-crack %.6g (uniaxial bending "
			 "reduces to the 2D two-point form). 3D TODAY ignores bFirstCrackRows -> plastic %.6g, STANDS."),
		R3D.bAnswered ? 1 : 0, R3D.Lambda, ExpectedFirstCrackLambda(), ExpectedPlasticLambda()));

	TestTrue(TEXT("3D: the oracle answers"), R3D.bAnswered);

	// 3D must fall as 2D does, and its lambda* must not exceed 2D's.
	TestEqual(
		*FString::Printf(
			TEXT("3D [RED]: the bonded joint FALLS in 3D as it does in 2D; got lambda* %.10g "
				 "(expected %.6g, NOT the plastic %.6g)"),
			R3D.Lambda, ExpectedFirstCrackLambda(), ExpectedPlasticLambda()),
		static_cast<int32>(OutcomeOf(R3D)),
		static_cast<int32>(EOracleOutcome::Falls));

	const double Tol = 1.0e-4 * ExpectedFirstCrackLambda();
	TestTrue(
		*FString::Printf(
			TEXT("3D [RED]: 3D lambda* is no more permissive than 2D; got 3D %.10g vs 2D %.10g"),
			R3D.Lambda, R2D.Lambda),
		R3D.Lambda <= R2D.Lambda + Tol);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
