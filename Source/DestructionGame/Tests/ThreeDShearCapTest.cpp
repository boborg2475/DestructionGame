// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/RigidBlock/RigidBlockOracle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Review item 8, defect 1: AssembleThreeD omits the MaxShear ceiling row the 2D assembler writes,
 * so 3D shear capacity is the unbounded c + mu*sigma. For GeneralPurposeMortar (c = 0.9,
 * mu = 0.75, f_v,max = 2.0 MPa) that exceeds the cap above sigma = 1.467 MPa.
 *
 * One free block on one bonded point patch (+Z normal), A = 100 cm2, Conv = 10000 uu/(MPa.cm2).
 * Dead self-weight gives sigma_n = 4.0 MPa (N = 4,000,000 uu); a live +X push at the joint point
 * gives tau = 2.5 MPa (H = 2,500,000 uu). lambda* = shear capacity / H.
 *
 *     UNCAPPED (3D today): cap_uncapped = cos(pi/8)*(0.75*4e6 + 0.9*1e6) = 0.9238795*3.9e6
 *                          = 3,603,130 uu -> lambda*_uncapped = 3,603,130/2,500,000 = 1.4413 -> stands
 *
 *     CAPPED (2D form mirrored to 3D): flat f_v,max octagon, no mu term —
 *                          cap_capped = cos(pi/8)*f_v,max*Conv*A = 0.9238795*2e6 = 1,847,759 uu
 *                          -> lambda*_capped = 1,847,759/2,500,000 = 0.7391 -> falls
 *
 * Red until AssembleThreeD writes the ceiling. Falls either way on the inscribe factor (0.8
 * without it). Asserts the LP outcome, not displacement. Units derived here. No ticking world.
 * Named namespace for the unity build.
 */
namespace ThreeDShearCapSupport
{
	using namespace RigidBlockOracle;

	constexpr double GravityCmPerSecondSquared = 980.0;
	constexpr double ConvUuPerMPaSqCm = 100.0 * 100.0;   // 1 MPa over 1 cm2 = 10000 uu

	constexpr double AreaSqCm = 100.0;

	// GeneralPurposeMortar, mean basis, spelled out so a moved profile constant fails.
	constexpr double CohesionMPa = 0.9;
	constexpr double FrictionMu = 0.75;
	constexpr double MaxShearMPa = 2.0;
	constexpr double TensileMPa = 0.7;
	constexpr double CompressiveMPa = 10.0;

	// cos(pi/8), the octagon apothem; derived, not imported.
	constexpr double InscribeFactor = 0.92387953251128674;

	constexpr double SigmaNMPa = 4.0;      // 2.7x the 1.467 MPa bite
	constexpr double Tau1MPa = 2.5;        // between capped 2.0 and uncapped 3.9

	double DeadNormalUu() { return SigmaNMPa * ConvUuPerMPaSqCm * AreaSqCm; }   // 4,000,000
	double LivePushUu()   { return Tau1MPa  * ConvUuPerMPaSqCm * AreaSqCm; }    // 2,500,000
	double BlockMassKg()  { return DeadNormalUu() / GravityCmPerSecondSquared; }

	// Pure-U shear capacities, uu.
	double UncappedShearCapUu()
	{
		return InscribeFactor * (FrictionMu * DeadNormalUu() + CohesionMPa * ConvUuPerMPaSqCm * AreaSqCm);
	}
	double CappedShearCapUu()
	{
		return InscribeFactor * MaxShearMPa * ConvUuPerMPaSqCm * AreaSqCm;
	}

	double ExpectedUncappedLambda() { return UncappedShearCapUu() / LivePushUu(); }   // 1.4413
	double ExpectedCappedLambda()   { return CappedShearCapUu()   / LivePushUu(); }   // 0.7391

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

	FOracleProblem BuildProblem()
	{
		FOracleProblem P;
		P.Dim = EOracleDim::Dim3D;
		P.bGravityIsLive = false;   // self-weight is dead pre-compression

		FOracleBlock G;
		G.MassKg = 1.0;
		G.CentroidXCm = 0.0;
		G.CentroidYCm = 0.0;
		G.CentroidZCm = -1.0;
		G.bGrounded = true;

		FOracleBlock B;
		B.MassKg = BlockMassKg();
		B.CentroidXCm = 0.0;
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
		J.HalfUCm = 0.0;      // point patch: no bending, so first-crack cannot interfere
		J.HalfVCm = 0.0;
		J.AreaSqCm = AreaSqCm;
		J.Strength = Mortar();
		P.Joints.Add(J);

		// Live push along +X (the U shear axis), at the joint point so it adds no moment.
		FOracleAppliedForce Push;
		Push.Block = Free;
		Push.ForceXUu = LivePushUu();
		Push.ForceYUu = 0.0;
		Push.ForceZUu = 0.0;
		Push.AtXCm = 0.0;
		Push.AtYCm = 0.0;
		Push.AtZCm = 0.0;
		Push.bLive = true;
		P.AppliedForces.Add(Push);

		return P;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FThreeDShearCapCeilingBindsTest,
	"DestructionGame.Oracle.RigidBlock.ThreeD.ShearCapCeilingBindsUnderHighCompression",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FThreeDShearCapCeilingBindsTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace ThreeDShearCapSupport;

	const FOracleProblem P = BuildProblem();
	const FOracleResult R = SolveRigidBlock(P);

	AddInfo(FString::Printf(
		TEXT("SHEAR CAP: answered %d, lambda* %.10g. sigma_n %.6g MPa (bite %.6g MPa), tau_1 %.6g MPa; "
			 "uncapped octagon cap %.6g uu -> lambda* %.6g (3D TODAY, credits unbounded friction, STANDS); "
			 "ceiling cap %.6g uu -> lambda* %.6g (CORRECT, FALLS)."),
		R.bAnswered ? 1 : 0, R.Lambda,
		SigmaNMPa, (MaxShearMPa - CohesionMPa) / FrictionMu, Tau1MPa,
		UncappedShearCapUu(), ExpectedUncappedLambda(),
		CappedShearCapUu(), ExpectedCappedLambda()));

	TestTrue(TEXT("SHEAR CAP: the oracle answers this determinate 3D feasibility pose"), R.bAnswered);

	// 2.5 MPa shear on a 2.0 MPa cap must fall (lambda* < 1); without the ceiling it stands at ~1.44.
	TestEqual(
		*FString::Printf(
			TEXT("SHEAR CAP [RED]: an over-truncation shear joint FALLS; got lambda* %.10g "
				 "(expected %.6g capped, NOT %.6g uncapped)"),
			R.Lambda, ExpectedCappedLambda(), ExpectedUncappedLambda()),
		static_cast<int32>(OutcomeOf(R)),
		static_cast<int32>(EOracleOutcome::Falls));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
