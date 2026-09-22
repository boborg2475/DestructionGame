// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/RigidBlock/RigidBlockOracle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * E1b: the k=8 inscribed friction pyramid (THREED_DESIGN §"The 3D physics", §"E1 slice sequence").
 *
 * The 3D Coulomb cone sqrt(s_u^2 + s_v^2) <= c*A + mu*n is not LP-able, so it is approximated by 8
 * facets at theta_i = i*pi/4, each scaled by cos(pi/8) on both sides. That inscribes the octagon:
 * facets sit at the apothem cos(pi/8)*R and vertices lie on the cone at R, so no direction exceeds
 * the true limit. Without the factor the octagon circumscribes the cone and admits 8.2% more shear
 * along vertex directions, certifying a sliding collapse as standing.
 *
 * Fixture: one free block on one grounded 10 x 10 cm patch (N = +Z), gravity dead, a live
 * horizontal push at bearing level so nothing tips and friction alone decides.
 *   W = 10 kg * 980 = 9800 uu; R = c*Conv*A + mu*W = 0.02*10000*100 + 0.5*9800 = 24900 uu;
 *   P = 12450 uu, so R/P = 2.0.
 * Arms:
 *   (i)   facet push along +X: lambda* = cos(pi/8)*R/P = 1.8478.
 *   (ii)  45deg push (also a facet): 1.8478, not a k=4 box's 2*sqrt(2).
 *   (iii) 22.5deg vertex push: exactly 2.0, and never above it. Only this arm distinguishes
 *         inscribed (2.0) from circumscribed (2.16478).
 *
 * Units are derived here, not imported, so a wrong production constant disagrees. Needs no world.
 * Uniquely named namespace for unity builds.
 */
namespace SlidingPyramidThreeDSupport
{
	using namespace RigidBlockOracle;

	// Units, derived here rather than imported.

	/** MassKg * 980 is a weight in uu; the 1 N = 100 uu factor is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa over 1 cm2 is 10000 uu. */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	/*
	 * Octagon constants to full double precision (FMath::Cos is float). cos(pi/8) is the octagon's
	 * apothem/circumradius; both also give the 22.5deg push components.
	 */

	constexpr double CosPiOver8 = 0.92387953251128674;   // apothem / R, and cos(22.5deg)
	constexpr double SinPiOver8 = 0.38268343236508978;   // sin(22.5deg)

	// The sliding patch.

	constexpr double MassKg = 10.0;          // W = 9800 uu
	constexpr double HalfUCm = 5.0;          // patch is 10 x 10 cm
	constexpr double HalfVCm = 5.0;
	constexpr double AreaSqCm = 100.0;       // tributary A/4 = 25 per corner
	constexpr double FrictionMu = 0.5;
	constexpr double CohesionMPa = 0.02;
	constexpr double ContactZCm = 0.0;       // bearing plane
	constexpr double ComZCm = 5.0;           // block centroid above the patch, plan (0,0)

	constexpr double PushUu = 12450.0;       // |push|; R / PushUu = 2.0

	enum { Support = 0, Free = 1 };          // block indices

	double WeightUu() { return MassKg * GravityCmPerSecondSquared; }

	/** Cone radius R = c*Conv*A + mu*W, uu. */
	double TrueConeCapacityUu()
	{
		return CohesionMPa * ForceUnitsPerMPaSqCmHere * AreaSqCm + FrictionMu * WeightUu();
	}

	/** R/P = 2.0: the vertex-arm answer and the safety bound. */
	double TrueConeLoadFactor() { return TrueConeCapacityUu() / PushUu; }

	/** cos(pi/8)*R/P = 1.8478: the facet and diagonal answer. */
	double FacetLoadFactor() { return CosPiOver8 * TrueConeLoadFactor(); }

	FConnectionStrength SlidingBond()
	{
		FConnectionStrength S;
		S.CompressiveStrengthMPa = 1000.0;   // crushing never binds
		S.TensileStrengthMPa = 0.0;          // dry, no tension
		S.ShearCohesionMPa = CohesionMPa;
		S.FrictionCoefficient = FrictionMu;
		return S;
	}

	FOracleBlock GroundedSupport()
	{
		FOracleBlock B;
		B.MassKg = 1.0;
		B.CentroidXCm = 0.0;
		B.CentroidYCm = 0.0;
		B.CentroidZCm = ContactZCm - 1.0;
		B.bGrounded = true;
		return B;
	}

	FOracleBlock FreeBlock()
	{
		FOracleBlock B;
		B.MassKg = MassKg;
		B.CentroidXCm = 0.0;
		B.CentroidYCm = 0.0;
		B.CentroidZCm = ComZCm;
		B.bGrounded = false;
		return B;
	}

	/** The bearing patch: N = +Z, 10 x 10 cm. */
	FOracleJoint BearingPatch()
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
		J.HalfLengthCm = 0.0;
		J.HalfUCm = HalfUCm;
		J.HalfVCm = HalfVCm;
		J.AreaSqCm = AreaSqCm;
		J.Strength = SlidingBond();
		return J;
	}

	/** Gravity dead, one live horizontal push at bearing level, so friction alone decides. */
	FOracleProblem SlidingProblem(double PushXUu, double PushYUu)
	{
		FOracleProblem P;
		P.Dim = EOracleDim::Dim3D;
		P.bGravityIsLive = false;   // gravity dead; the push is the live load

		P.Blocks.SetNum(2);
		P.Blocks[Support] = GroundedSupport();
		P.Blocks[Free] = FreeBlock();

		P.Joints.Add(BearingPatch());

		FOracleAppliedForce Push;
		Push.Block = Free;
		Push.ForceXUu = PushXUu;
		Push.ForceYUu = PushYUu;
		Push.ForceZUu = 0.0;
		Push.AtXCm = 0.0;
		Push.AtYCm = 0.0;
		Push.AtZCm = ContactZCm;   // bearing level: no overturning couple
		Push.bLive = true;
		P.AppliedForces.Add(Push);

		return P;
	}

	bool Near(double A, double B, double Tol) { return FMath::Abs(A - B) <= Tol; }
}

/**
 * The friction pyramid is inscribed: facet and diagonal pushes give cos(pi/8)*R/P = 1.8478, and a
 * vertex push gives exactly R/P = 2.0 and never more. A circumscribed pyramid reads 2.0 on the
 * facets and 2.16478 on the vertex. Needs no world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSlidingOnAPyramidFacetAndOnADiagonalTest,
	"DestructionGame.Oracle.RigidBlock.ThreeD.SlidingOnAPyramidFacetAndOnADiagonal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSlidingOnAPyramidFacetAndOnADiagonalTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace SlidingPyramidThreeDSupport;

	const double TrueCone = TrueConeLoadFactor();   // R/P = 2.0 (vertex value and safety bound)
	const double Facet = FacetLoadFactor();      // cos(pi/8)*R/P = 1.8477590650 (facet/diagonal)

	// 2e-4 absorbs LP round-off; the three candidate answers are over 0.15 apart.
	const double Tol = 1.0e-4 * TrueCone;

	// Arm (i): facet push along +X.
	{
		const FOracleProblem P = SlidingProblem(PushUu, 0.0);
		const FOracleResult R = SolveRigidBlock(P);

		AddInfo(FString::Printf(
			TEXT("FACET: answered %d, got lambda* %.9g, expected cos(pi/8)*R/P = %.9g ")
			TEXT("(true cone R/P = %.9g; a circumscribed pyramid would read %.9g)"),
			R.bAnswered ? 1 : 0, R.Lambda, Facet, TrueCone, TrueCone));

		TestTrue(TEXT("FACET: the oracle answers"), R.bAnswered);
		TestTrue(
			*FString::Printf(
				TEXT("FACET [RED]: inscribed facet capacity is cos(pi/8)*R, so lambda* = %.9g, got %.9g ")
				TEXT("(circumscribed reads the true-cone %.9g)"),
				Facet, R.Lambda, TrueCone),
			Near(R.Lambda, Facet, Tol));
	}

	// Arm (ii): 45deg push, same magnitude (45deg is also a facet).
	{
		const double Component = PushUu / FMath::Sqrt(2.0);   // |push| stays PushUu
		const FOracleProblem P = SlidingProblem(Component, Component);
		const FOracleResult R = SolveRigidBlock(P);

		AddInfo(FString::Printf(
			TEXT("DIAGONAL: answered %d, got lambda* %.9g, expected cos(pi/8)*R/P = %.9g ")
			TEXT("(45deg is a facet; a k=4 box would read R*sqrt(2)/P = %.9g)"),
			R.bAnswered ? 1 : 0, R.Lambda, Facet, TrueCone * FMath::Sqrt(2.0)));

		TestTrue(TEXT("DIAGONAL: the oracle answers"), R.bAnswered);
		TestTrue(
			*FString::Printf(
				TEXT("DIAGONAL [RED]: 45deg is an inscribed facet, so lambda* = cos(pi/8)*R/P = %.9g, ")
				TEXT("got %.9g (NOT the box's %.9g, NOT the circumscribed %.9g)"),
				Facet, R.Lambda, TrueCone * FMath::Sqrt(2.0), TrueCone),
			Near(R.Lambda, Facet, Tol));
	}

	/*
	 * Arm (iii): 22.5deg vertex push, the inscribed/circumscribed discriminator. Asserts both the
	 * safety bound (lambda* <= R/P) and the exact inscribed value (R/P).
	 */
	{
		const double PushX = CosPiOver8 * PushUu;   // cos(22.5deg)*|push|
		const double PushY = SinPiOver8 * PushUu;   // sin(22.5deg)*|push|
		const FOracleProblem P = SlidingProblem(PushX, PushY);
		const FOracleResult R = SolveRigidBlock(P);

		AddInfo(FString::Printf(
			TEXT("VERTEX: answered %d, got lambda* %.9g, expected R/P = %.9g ")
			TEXT("(circumscribed reads 2/cos(pi/8) = %.9g, which OVERSHOOTS the true cone)"),
			R.bAnswered ? 1 : 0, R.Lambda, TrueCone, TrueCone / CosPiOver8));

		TestTrue(TEXT("VERTEX: the oracle answers"), R.bAnswered);

		// Safety: no direction may exceed the true cone.
		TestTrue(
			*FString::Printf(
				TEXT("VERTEX SAFETY [RED]: lambda* must not exceed the true cone R/P = %.9g, got %.9g ")
				TEXT("(circumscribed overshoots to %.9g)"),
				TrueCone, R.Lambda, TrueCone / CosPiOver8),
			R.Lambda <= TrueCone + Tol);

		// Exactly R/P: an inscribed vertex lies on the cone.
		TestTrue(
			*FString::Printf(
				TEXT("VERTEX [RED]: inscribed vertex sits ON the cone, so lambda* = R/P = %.9g, got %.9g"),
				TrueCone, R.Lambda),
			Near(R.Lambda, TrueCone, Tol));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
