// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/RigidBlock/RigidBlockOracle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * E2a: 3D collapse-mechanism extraction.
 *
 * A dead-load (bGravityIsLive = false) 3D problem with no equilibrium, so phase 1 ends on the
 * infeasible arm where ExtractMechanism reads the mechanism. Written red: the Dim3D path left
 * EqFxRowOfBlock all INDEX_NONE, so no block moved and the solve refused with VerificationFailure.
 *
 * Fixture: a free block on a grounded support through one 20 x 20 cm patch at plan (0, 0), normal
 * +Z, top at Z = 10. The block's centroid is at (0, 20, 30), 10 cm beyond the patch's +Y edge. Dry,
 * 10 kg. Every corner reaction sits at Y <= 10, so its moment about X is negative, and shear at a
 * single height adds none: no equilibrium exists.
 *
 * Mechanism: rotation about the +Y base edge, omega = (Omega_x, 0, 0), which a 2D single-scalar
 * extractor cannot represent. Centroid velocity is (0, -20, 10) * Omega_x, so it descends
 * (VirtualUz < 0) and |Omega_x| is about 1/31 of the generalised velocity. Asserts the mechanism,
 * never displacement. Units derived locally. No world.
 */
namespace MechanismTipsThreeDSupport
{
	using namespace RigidBlockOracle;

	/** MassKg * 980 is already uu force (includes 1 N = 100 uu). */
	constexpr double GravityCmPerSecondSquared = 980.0;

	constexpr double MassKg = 10.0;         // W = 9800 uu
	constexpr double HalfUCm = 10.0;        // along X, the tipping edge's direction
	constexpr double HalfVCm = 10.0;        // along Y; +Y edge at Y = +10
	constexpr double ContactZCm = 10.0;     // patch top
	constexpr double ComXCm = 0.0;          // X-symmetric: forbids Omega_y and Omega_z
	constexpr double ComYCm = 20.0;         // 10 cm beyond the +Y edge
	constexpr double ComZCm = 30.0;
	constexpr double AreaSqCm = 400.0;      // 20 x 20 cm

	enum { Support = 0, Free = 1 };         // block indices
	enum { J0 = 0 };                        // the one joint

	FOracleBlock GroundedSupport()
	{
		FOracleBlock B;
		B.MassKg = 1.0;
		B.CentroidXCm = 0.0;
		B.CentroidYCm = 0.0;
		B.CentroidZCm = ContactZCm / 2.0;
		B.bGrounded = true;
		return B;
	}

	FOracleBlock FreeBlock()
	{
		FOracleBlock B;
		B.MassKg = MassKg;
		B.CentroidXCm = ComXCm;
		B.CentroidYCm = ComYCm;
		B.CentroidZCm = ComZCm;
		B.bGrounded = false;
		return B;
	}

	/** No tension, everything else generous, so tipping is the only mechanism. */
	FConnectionStrength DryBond()
	{
		FConnectionStrength S;
		S.CompressiveStrengthMPa = 1000.0;
		S.TensileStrengthMPa = 0.0;
		S.ShearCohesionMPa = 1000.0;
		S.FrictionCoefficient = 1000.0;
		return S;
	}

	FOracleJoint BasePatch()
	{
		FOracleJoint J;
		J.BlockA = Support;
		J.BlockB = Free;
		J.NormalX = 0.0;
		J.NormalY = 0.0;
		J.NormalZ = 1.0;
		J.CentreXCm = 0.0;
		J.CentreYCm = 0.0;
		J.CentreZCm = ContactZCm;
		J.HalfLengthCm = 0.0;   // 2D field, unused on the 3D path
		J.HalfUCm = HalfUCm;
		J.HalfVCm = HalfVCm;
		J.AreaSqCm = AreaSqCm;
		J.Strength = DryBond();
		return J;
	}

	FOracleProblem BuildTippingBlock()
	{
		FOracleProblem P;
		P.Dim = EOracleDim::Dim3D;

		/*
		 * Dead gravity, no live forces: phase 1 ends infeasible and hands its dual to
		 * ExtractMechanism. bMinViolationReadout stays false to route via SolveRigidBlockOnce.
		 */
		P.bGravityIsLive = false;

		P.Blocks.SetNum(2);
		P.Blocks[Support] = GroundedSupport();
		P.Blocks[Free] = FreeBlock();

		P.Joints.Add(BasePatch());
		return P;
	}

	double WeightUu() { return MassKg * GravityCmPerSecondSquared; }
}

/*
 * The overhanging block tips about its +Y base edge. Assertion (e) cannot be met by a 2D
 * extractor, and zeroing AssembleThreeD's Mx coefficients would also fail it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMechanismTipsThreeDTest,
	"DestructionGame.Oracle.RigidBlock.ThreeD.MechanismTipsAboutABaseEdge",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMechanismTipsThreeDTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace MechanismTipsThreeDSupport;

	const FOracleProblem P = BuildTippingBlock();
	const FOracleResult R = SolveRigidBlock(P);
	const FOracleMechanism& M = R.Mechanism;

	const bool bFreeValid = M.Blocks.IsValidIndex(Free);
	const bool bSupportValid = M.Blocks.IsValidIndex(Support);
	const bool bJointValid = M.JointOpensOrSlides.IsValidIndex(J0);

	const FOracleMechanismBlock FreeT = bFreeValid ? M.Blocks[Free] : FOracleMechanismBlock();

	const double Ux = FreeT.VirtualUx;
	const double Uy = FreeT.VirtualUy;
	const double Uz = FreeT.VirtualUz;
	const double Ox = FreeT.VirtualOmegaX;   // the tipping axis
	const double Oy = FreeT.VirtualOmega;    // the only rotation a 2D extractor has
	const double Oz = FreeT.VirtualOmegaZ;

	const double GenMag =
		FMath::Abs(Ux) + FMath::Abs(Uy) + FMath::Abs(Uz) +
		FMath::Abs(Ox) + FMath::Abs(Oy) + FMath::Abs(Oz);

	AddInfo(FString::Printf(
		TEXT("answered %d, lambda* %.6g, mechanism present %d certified %d; ")
		TEXT("free moves %d, support moves %d, joint opens %d; ")
		TEXT("free triple u=(%.6g, %.6g, %.6g) omega=(Ox %.6g, Oy %.6g, Oz %.6g), genMag %.6g"),
		R.bAnswered ? 1 : 0, R.Lambda, M.bPresent ? 1 : 0, M.bIsCertified ? 1 : 0,
		(bFreeValid && M.Blocks[Free].bMoves) ? 1 : 0,
		(bSupportValid && M.Blocks[Support].bMoves) ? 1 : 0,
		(bJointValid && M.JointOpensOrSlides[J0]) ? 1 : 0,
		Ux, Uy, Uz, Ox, Oy, Oz, GenMag));

	// (a) Answered, and it falls: lambda* = 0.
	TestTrue(TEXT("(a) [RED]: the oracle answers the infeasible 3D arm"), R.bAnswered);
	TestTrue(
		*FString::Printf(TEXT("(a) [RED]: lambda* = 0 (it falls), got %.9g"), R.Lambda),
		FMath::Abs(R.Lambda) <= 1.0e-6);

	// (b) Mechanism present and Farkas-certified.
	TestTrue(TEXT("(b) [RED]: the mechanism is present"), M.bPresent);
	TestTrue(TEXT("(b) [RED]: the mechanism is certified"), M.bIsCertified);

	// (c) The free block moves; the support does not.
	TestTrue(TEXT("(c) [RED]: the free block's triple is reported"), bFreeValid);
	TestTrue(TEXT("(c) [RED]: the free block moves"),
		bFreeValid && M.Blocks[Free].bMoves);
	TestTrue(TEXT("(c) [RED]: the grounded support's triple is reported"), bSupportValid);
	TestFalse(TEXT("(c): the grounded support does not move"),
		bSupportValid && M.Blocks[Support].bMoves);

	// (d) The joint opens or slides.
	TestTrue(TEXT("(d) [RED]: the joint flags are reported"), bJointValid);
	TestTrue(TEXT("(d) [RED]: the base joint opens/slides"),
		bJointValid && M.JointOpensOrSlides[J0]);

	/*
	 * (e) Rotation about X: |Omega_x| above 1% of the motion (geometry gives ~1/31), Omega_y and
	 * Omega_z negligible, centroid descends, no X translation. Guarded on GenMag so an empty
	 * mechanism fails the checks above instead.
	 */
	const bool bMoving = GenMag > 0.0;

	TestTrue(
		*FString::Printf(
			TEXT("(e) [RED]: rotation is about X — |Omega_x| %.6g is a real fraction of genMag %.6g"),
			FMath::Abs(Ox), GenMag),
		bMoving && FMath::Abs(Ox) > 0.01 * GenMag);
	TestTrue(
		*FString::Printf(
			TEXT("(e) [RED]: NOT a 2D X-Z rotation — the Y-axis omega %.6g << |Omega_x| %.6g"),
			FMath::Abs(Oy), FMath::Abs(Ox)),
		bMoving && FMath::Abs(Oy) <= 1.0e-4 * FMath::Abs(Ox));
	TestTrue(
		*FString::Printf(
			TEXT("(e) [RED]: no plan twist — the Z-axis omega %.6g << |Omega_x| %.6g"),
			FMath::Abs(Oz), FMath::Abs(Ox)),
		bMoving && FMath::Abs(Oz) <= 1.0e-4 * FMath::Abs(Ox));
	TestTrue(
		*FString::Printf(TEXT("(e) [RED]: the centroid descends, VirtualUz %.6g < 0"), Uz),
		bMoving && Uz < 0.0);
	TestTrue(
		*FString::Printf(
			TEXT("(e) [RED]: no X translation — |VirtualUx| %.6g << genMag %.6g"),
			FMath::Abs(Ux), GenMag),
		bMoving && FMath::Abs(Ux) <= 1.0e-4 * GenMag);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
