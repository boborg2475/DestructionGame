// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/RigidBlock/RigidBlockOracle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The 3D assembly omits the truncated-shear ceiling the 2D assembly has — review item 8, defect 1.
 *
 * The 2D assembler (RigidBlockOracle.cpp ~2329-2343) writes a MaxShear ceiling row
 * `±(p - q) <= f_v,max * Conv * A/2` on every joint whose profile caps its shear, on top of the
 * Mohr-Coulomb friction row `|v| <= c + mu*sigma`. AssembleThreeD (~1786-2010) writes the tension
 * row, the k=8 inscribed friction pyramid and the crush row but no ceiling — so in 3D a joint's
 * shear capacity is the unbounded Coulomb value c + mu*sigma however high the compression drives
 * it. For GeneralPurposeMortar (c = 0.9, mu = 0.75, cap f_v,max = 2.0 MPa) the bite point is
 * sigma = (2.0 - 0.9)/0.75 = 1.467 MPa: above it, `c + mu*sigma` exceeds the cap and the 3D LP
 * credits shear the 2D LP forbids — so a heavily-compressed 3D joint that should fail in shear at
 * the ceiling does not.
 *
 * THE FIXTURE — one free block on one bonded point-patch joint (normal +Z), determinate. A dead
 * vertical load (the block's own weight) sets a fixed pre-compression well past the bite; a live
 * horizontal push (+X, the U shear axis) provides the shear demand, so lambda* is exactly (shear
 * capacity) / (shear demand at lambda = 1) and nothing else binds.
 *
 *     Face area A = 100 cm2, Conv = 10000 uu/(MPa.cm2).
 *     Dead compression: sigma_n = 4.0 MPa -> N = 4.0*Conv*A = 4,000,000 uu (W = N, MassKg = N/980).
 *         4.0 MPa is 2.7x the 1.467 MPa bite.
 *     Live push: tau_1 = 2.5 MPa -> H = 2.5*Conv*A = 2,500,000 uu, applied at the joint point so it
 *         raises no moment.
 *
 * THE TWO ENVELOPES, worked independently of production (the octagon apothem cos(pi/8) is derived
 * here, not imported). Pushed purely along U, the governing friction facet is theta = 0, so the 3D
 * friction octagon credits pure-U shear up to cos(pi/8) * (mu*N + c*Conv*A) — the same apothem
 * factor the assembler scales its pyramid by:
 *
 *     UNCAPPED (3D today): cap_uncapped = cos(pi/8)*(0.75*4e6 + 0.9*1e6) = 0.9238795*3.9e6
 *                          = 3,603,130 uu -> lambda*_uncapped = 3,603,130/2,500,000 = 1.4413 -> stands
 *
 *     CAPPED (2D form mirrored to 3D): flat f_v,max octagon, no mu term —
 *                          cap_capped = cos(pi/8)*f_v,max*Conv*A = 0.9238795*2e6 = 1,847,759 uu
 *                          -> lambda*_capped = 1,847,759/2,500,000 = 0.7391 -> falls
 *
 * RED TODAY: 3D omits the ceiling, credits the unbounded 1.4413 friction and stands a joint whose
 * shear demand (2.5 MPa) is past its 2.0 MPa truncation. Green once AssembleThreeD writes the
 * ceiling as an octagon on the in-plane shear magnitude. The Falls verdict is robust to whether dev
 * applies the inscribe factor to the ceiling: without it lambda*_capped = 2e6/2.5e6 = 0.8, still < 1.
 *
 * Asserts on the LP outcome (Falls / lambda* < 1), never displacement. Units derived here, not
 * imported. Needs no ticking world. Named namespace for the unity build.
 */
namespace ThreeDShearCapSupport
{
	using namespace RigidBlockOracle;

	constexpr double GravityCmPerSecondSquared = 980.0;
	constexpr double ConvUuPerMPaSqCm = 100.0 * 100.0;   /* 1 MPa over 1 cm2 = 10000 uu */

	constexpr double AreaSqCm = 100.0;

	/* GeneralPurposeMortar, mean basis — spelled out so the test fails if a profile constant moves. */
	constexpr double CohesionMPa = 0.9;
	constexpr double FrictionMu = 0.75;
	constexpr double MaxShearMPa = 2.0;
	constexpr double TensileMPa = 0.7;
	constexpr double CompressiveMPa = 10.0;

	/* cos(pi/8), the octagon apothem/circumradius — derived, not imported from the oracle. */
	constexpr double InscribeFactor = 0.92387953251128674;

	constexpr double SigmaNMPa = 4.0;      /* 2.7x the (2.0-0.9)/0.75 = 1.467 MPa bite */
	constexpr double Tau1MPa = 2.5;        /* between capped 2.0 and uncapped c+mu*sigma = 3.9 */

	double DeadNormalUu() { return SigmaNMPa * ConvUuPerMPaSqCm * AreaSqCm; }   /* 4,000,000 */
	double LivePushUu()   { return Tau1MPa  * ConvUuPerMPaSqCm * AreaSqCm; }    /* 2,500,000 */
	double BlockMassKg()  { return DeadNormalUu() / GravityCmPerSecondSquared; }

	/* Pure-U shear capacities, uu. */
	double UncappedShearCapUu()
	{
		return InscribeFactor * (FrictionMu * DeadNormalUu() + CohesionMPa * ConvUuPerMPaSqCm * AreaSqCm);
	}
	double CappedShearCapUu()
	{
		return InscribeFactor * MaxShearMPa * ConvUuPerMPaSqCm * AreaSqCm;
	}

	double ExpectedUncappedLambda() { return UncappedShearCapUu() / LivePushUu(); }   /* 1.4413 */
	double ExpectedCappedLambda()   { return CappedShearCapUu()   / LivePushUu(); }   /* 0.7391 */

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
		P.bGravityIsLive = false;   /* the block's weight is DEAD pre-compression */

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
		J.HalfUCm = 0.0;      /* point patch: no bending, so first-crack cannot confound the shear cap */
		J.HalfVCm = 0.0;
		J.AreaSqCm = AreaSqCm;
		J.Strength = Mortar();
		P.Joints.Add(J);

		/* The LIVE horizontal push, +X = the U shear axis, applied AT the joint point (no moment). */
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

	/*
	 * A joint loaded to 2.5 MPa of shear on a 2.0 MPa truncation is over capacity, so the LP must
	 * find no admissible equilibrium at self-weight — Falls, lambda* < 1. Today the 3D assembly
	 * omits the ceiling and stands it at ~1.44 on unbounded friction.
	 */
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
