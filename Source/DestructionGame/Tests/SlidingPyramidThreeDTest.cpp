// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/RigidBlock/RigidBlockOracle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * E1b — THE 3D FRICTION PYRAMID, INSCRIBED. The driver for the k=8 INSCRIBED Coulomb pyramid
 * (THREED_DESIGN §"The 3D physics" friction PYRAMID [RULED k=8, INSCRIBED], §"E1 slice sequence" E1b).
 *
 * The true 3D Coulomb limit is the CONE  sqrt(s_u^2 + s_v^2) <= c*A + mu*n , which is not LP-able; E1b
 * approximates it with a k=8 octagon of 8 linear facets  cos(theta_i)*(p_u - q_u) + sin(theta_i)*(p_v -
 * q_v) - cos(pi/8)*mu*(n+ - n-) <= cos(pi/8)*c*Conv*A/4 , theta_i = i*pi/4, i = 0..7 (the per-corner
 * tributary A/4). The cos(pi/8) factor on BOTH sides is what INSCRIBES the octagon in the cone: it pulls
 * every facet in to the apothem distance cos(pi/8)*R so the octagon's VERTICES land exactly ON the cone at
 * radius R, and no direction is admitted beyond the true Coulomb limit.
 *
 * WHY cos(pi/8) MATTERS — the E1b review's safety bug. The FIRST implementation set the facet distance at
 * the true cone radius R itself (no cos(pi/8)). That CIRCUMSCRIBES the cone: with the flat facets held out
 * at R, the octagon's vertices bulge to R/cos(pi/8) ~ 1.082*R, so a shear push aimed at a VERTEX direction
 * (22.5deg / 67.5deg / ...) is admitted up to 1.082*R — 8.2% MORE than the true Coulomb limit. That
 * certifies a sliding collapse as standing: a plausible number, wrong statics, exactly the failure this
 * project exists to avoid. INSCRIBED (facets pulled in by cos(pi/8), vertices on the cone) reads <= the
 * true cone in every direction — conservative, the honest direction for a demolition gate.
 *
 * THE FIXTURE — one FREE block (weight W) bearing on ONE grounded patch (unit normal N = +Z, so the derived
 * in-plane frame is u = +X, v = +Y), under DEAD gravity plus a LIVE horizontal push. Gravity dead puts the
 * full weight W into the normal (sum of the four corner normals = W); the push is the only live load, so
 * lambda* answers "how many pushes until it slides". The push is applied AT BEARING LEVEL (the patch plane),
 * so it and the friction reaction are colinear in height and exert no overturning couple — the corners stay
 * uniformly loaded, nothing tips, and FRICTION ALONE decides, exactly as the 2D SlidingProblem does it
 * (RigidBlockOracleTest.cpp). The fixture is UNCHANGED from the original two-arm test; only the assertions move.
 *
 * THE TRUE-CONE CAPACITY, summed over the four corners (sum n_k = W, four tributaries of A/4 = A):
 *
 *     R = c*Conv*A + mu*W        (force units)     = the cone radius, in every direction
 *
 * With |push| = P, the TRUE-cone load factor is R/P in every direction. The fixture is tuned so R/P = 2.0
 * exactly. The INSCRIBED octagon reads:
 *
 *   - along a FACET normal (theta_i, e.g. 0deg or the 45deg diagonal): the apothem, cos(pi/8)*R, so
 *     lambda* = cos(pi/8)*R/P = 2*cos(pi/8) = 1.8477590650  (conservative, BELOW the true cone);
 *   - along a VERTEX direction (22.5deg, mid-way between facets 0 and 45): the vertex sits ON the cone at
 *     R, so lambda* = R/P = 2.0 exactly (== the true cone, never above it).
 *
 * THREE ARMS. The facet and diagonal arms alone CANNOT distinguish inscribed from circumscribed k=8: a push
 * along a facet normal reads the apothem either way relative to the same octagon, so both geometries look
 * identical there (they differ only in whether that apothem equals R or cos(pi/8)*R against the fixed R/P=2
 * fixture — which is exactly what those arms now pin). The VERTEX arm is the discriminator: only there do
 * inscribed (2.0) and circumscribed (2/cos(pi/8) = 2.16478) diverge, because that is the one direction where
 * the octagon's boundary is a vertex, not a facet.
 *
 *   (i)   FACET push, along +u (= +X), magnitude P. Binding facet theta = 0. lambda* = cos(pi/8)*R/P = 1.8478.
 *   (ii)  DIAGONAL push at 45deg, SAME magnitude P (components P/sqrt(2) each). 45deg IS a facet (facets at
 *         0,45,90,...), so lambda* = cos(pi/8)*R/P = 1.8478 too — NOT R*sqrt(2)/P (a k=4 box) and NOT R/P.
 *   (iii) VERTEX push at 22.5deg, SAME magnitude P (components cos(22.5)*P, sin(22.5)*P). The boundary here is
 *         a VERTEX, on the cone at R, so lambda* = R/P = 2.0 — the SAFETY BOUND. The circumscribed code reads
 *         2/cos(pi/8) = 2.16478 here, 8.2% over the true cone: the red this arm exists to catch.
 *
 * NUMBERS (all derived here, nothing imported):
 *     M = 10 kg           -> W = M*980 = 9800 uu
 *     patch A = 100 cm^2  (half-extents 5 x 5 cm; tributary A/4 = 25 per corner)
 *     mu = 0.5 , c = 0.02 MPa , Conv = 1 MPa over 1 cm^2 = 10000 uu
 *     R = c*Conv*A + mu*W = 0.02*10000*100 + 0.5*9800 = 20000 + 4900 = 24900 uu
 *     P = 12450 uu  ->  R/P = 2.0 (true cone / vertex),  cos(pi/8)*R/P = 1.8477590650 (facet / diagonal)
 *
 * UNITS derived here (1 MPa over 1 cm^2 = 10000 uu; M*980 already carries the 1 N = 100 uu factor), NOT
 * imported, so a wrong production constant disagrees rather than agrees.
 *
 * NEEDS A TICKING WORLD: NO. A pure hand-built FOracleProblem fed to SolveRigidBlock.
 *
 * NAMED NAMESPACE, not anonymous: the unity build merges many files into one translation unit.
 */
namespace SlidingPyramidThreeDSupport
{
	using namespace RigidBlockOracle;

	/* ================================================================================
	 * UNITS, derived here so a wrong production constant fails rather than agrees.
	 * ================================================================================ */

	/** MassKg * 980 IS a weight in uu — the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa over 1 cm2 is 10000 uu. NOT imported. */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	/* ================================================================================
	 * OCTAGON GEOMETRY, hand-computed to full double precision (NOT FMath::Cos, which
	 * is float). cos(pi/8) is the apothem-to-circumradius ratio of a regular octagon:
	 * an INSCRIBED octagon's flat facets sit this fraction of R from the centre, while
	 * its vertices reach the full R. sin(pi/8)/cos(pi/8) also give the 22.5deg push
	 * components for the vertex arm (22.5deg = pi/8).
	 * ================================================================================ */

	constexpr double CosPiOver8 = 0.92387953251128674;   /* apothem / R,  and cos(22.5deg) */
	constexpr double SinPiOver8 = 0.38268343236508978;   /* sin(22.5deg) */

	/* ================================================================================
	 * THE SLIDING PATCH, hand-built so the Coulomb capacity stays checkable.
	 * ================================================================================ */

	constexpr double MassKg = 10.0;          /* W = 9800 uu */
	constexpr double HalfUCm = 5.0;          /* patch is 10 x 10 cm */
	constexpr double HalfVCm = 5.0;
	constexpr double AreaSqCm = 100.0;       /* = (2*HalfU)*(2*HalfV); tributary A/4 = 25 */
	constexpr double FrictionMu = 0.5;
	constexpr double CohesionMPa = 0.02;
	constexpr double ContactZCm = 0.0;       /* bearing plane */
	constexpr double ComZCm = 5.0;           /* block centroid above the patch, plan (0,0) */

	constexpr double PushUu = 12450.0;       /* |push|; R / PushUu = 2.0 (true cone / vertex) */

	enum { Support = 0, Free = 1 };          /* block indices */

	double WeightUu() { return MassKg * GravityCmPerSecondSquared; }

	/**
	 * THE TRUE-CONE CAPACITY (cone radius R): R = c*Conv*A + mu*W. The inscribed octagon reads the apothem
	 * cos(pi/8)*R along a facet and exactly R along a vertex; this is the R those two facts derive from.
	 */
	double TrueConeCapacityUu()
	{
		return CohesionMPa * ForceUnitsPerMPaSqCmHere * AreaSqCm + FrictionMu * WeightUu();
	}

	/** True-cone load factor R/P = 2.0. The VERTEX arm reads this exactly; it is also the SAFETY BOUND. */
	double TrueConeLoadFactor() { return TrueConeCapacityUu() / PushUu; }

	/** Inscribed FACET load factor cos(pi/8)*R/P = 2*cos(pi/8) = 1.8478. Facet and diagonal alike. */
	double FacetLoadFactor() { return CosPiOver8 * TrueConeLoadFactor(); }

	FConnectionStrength SlidingBond()
	{
		FConnectionStrength S;
		S.CompressiveStrengthMPa = 1000.0;   /* >> the corner normals: crushing never binds */
		S.TensileStrengthMPa = 0.0;          /* dry no-tension */
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

	/** The bearing patch: N = +Z, a real 10 x 10 cm face (four distinct corners). */
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
		P.bGravityIsLive = false;   /* gravity is the dead load; the push is the live load */

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
		Push.AtZCm = ContactZCm;   /* bearing level: no overturning couple */
		Push.bLive = true;
		P.AppliedForces.Add(Push);

		return P;
	}

	bool Near(double A, double B, double Tol) { return FMath::Abs(A - B) <= Tol; }
}

/* ================================================================================================
 * SLIDING ON A PYRAMID FACET AND ON A DIAGONAL — E1b, INSCRIBED-pyramid RED.
 *
 * RED BECAUSE the current production pyramid is CIRCUMSCRIBED (facet distance = R, no cos(pi/8) factor).
 * Against the R/P=2.0 fixture that reads:
 *   - FACET / DIAGONAL: lambda* = R/P = 2.0, but the inscribed expectation is cos(pi/8)*R/P = 1.8478.
 *   - VERTEX (22.5deg): lambda* = R/(cos(pi/8)*P) = 2/cos(pi/8) = 2.16478, but the inscribed expectation is
 *     R/P = 2.0 AND the safety bound says <= 2.0. Circumscribed OVERSHOOTS the true cone by 8.2% here.
 *
 * BITE (for dev, at green): scaling the facet capacity by cos(pi/8) (§3 corrected) inscribes the octagon —
 * facet/diagonal drop to 1.8478 and the vertex drops from 2.16478 to exactly 2.0, satisfying the safety
 * bound. The VERTEX arm is the ONLY one of the three that distinguishes inscribed from circumscribed; a
 * facet/diagonal push reads the apothem in both geometries and is blind to the defect.
 *
 * NEEDS A TICKING WORLD: NO.
 * ================================================================================================ */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSlidingOnAPyramidFacetAndOnADiagonalTest,
	"DestructionGame.Oracle.RigidBlock.ThreeD.SlidingOnAPyramidFacetAndOnADiagonal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSlidingOnAPyramidFacetAndOnADiagonalTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace SlidingPyramidThreeDSupport;

	const double TrueCone = TrueConeLoadFactor();   /* R/P = 2.0  (vertex value AND safety bound) */
	const double Facet = FacetLoadFactor();      /* cos(pi/8)*R/P = 1.8477590650  (facet/diagonal) */

	/*
	 * Tolerance is keyed to the true-cone factor, tight enough to separate the three distinct answers:
	 * facet 1.8478, vertex 2.0, and the circumscribed vertex 2.16478 are all > 0.15 apart, and 2e-4 is
	 * far below that while still absorbing LP round-off.
	 */
	const double Tol = 1.0e-4 * TrueCone;

	/* ---------------- ARM (i): FACET push along +u (= +X), lambda* = cos(pi/8)*R/P ---------------- */
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

	/* ---------------- ARM (ii): DIAGONAL push at 45deg, SAME |push|, lambda* = cos(pi/8)*R/P ---------------- */
	{
		const double Component = PushUu / FMath::Sqrt(2.0);   /* |push| stays PushUu */
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
	 * ---------------- ARM (iii): VERTEX push at 22.5deg — THE INSCRIBED-vs-CIRCUMSCRIBED DISCRIMINATOR ----------------
	 *
	 * 22.5deg (= pi/8) sits MID-WAY between facet 0 and facet 45, so the octagon boundary in this direction
	 * is a VERTEX, not a facet. That is the ONLY place the two geometries diverge: an INSCRIBED octagon puts
	 * its vertex ON the cone at R (lambda* = R/P = 2.0 exactly), while a CIRCUMSCRIBED octagon — flat facets
	 * held out at R — bulges its vertex to R/cos(pi/8), admitting lambda* = 2/cos(pi/8) = 2.16478, 8.2% OVER
	 * the true Coulomb limit. The facet/diagonal arms above read the apothem in both geometries and cannot
	 * see this; only a vertex-direction push can. We assert BOTH the exact inscribed value (== 2.0) and the
	 * SAFETY INVARIANT (lambda* <= the true cone R/P) — the property that the LP-feasible friction region
	 * never exceeds the true Coulomb cone in ANY direction. Circumscribed violates the invariant.
	 */
	{
		const double PushX = CosPiOver8 * PushUu;   /* cos(22.5deg)*|push| */
		const double PushY = SinPiOver8 * PushUu;   /* sin(22.5deg)*|push|; magnitude stays PushUu */
		const FOracleProblem P = SlidingProblem(PushX, PushY);
		const FOracleResult R = SolveRigidBlock(P);

		AddInfo(FString::Printf(
			TEXT("VERTEX: answered %d, got lambda* %.9g, expected R/P = %.9g ")
			TEXT("(circumscribed reads 2/cos(pi/8) = %.9g, which OVERSHOOTS the true cone)"),
			R.bAnswered ? 1 : 0, R.Lambda, TrueCone, TrueCone / CosPiOver8));

		TestTrue(TEXT("VERTEX: the oracle answers"), R.bAnswered);

		/* The safety invariant: no direction may exceed the true Coulomb cone. Circumscribed reads 2.16478. */
		TestTrue(
			*FString::Printf(
				TEXT("VERTEX SAFETY [RED]: lambda* must not exceed the true cone R/P = %.9g, got %.9g ")
				TEXT("(circumscribed overshoots to %.9g)"),
				TrueCone, R.Lambda, TrueCone / CosPiOver8),
			R.Lambda <= TrueCone + Tol);

		/* And exactly R/P: an inscribed vertex sits ON the cone. */
		TestTrue(
			*FString::Printf(
				TEXT("VERTEX [RED]: inscribed vertex sits ON the cone, so lambda* = R/P = %.9g, got %.9g"),
				TrueCone, R.Lambda),
			Near(R.Lambda, TrueCone, Tol));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
