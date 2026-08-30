// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/RigidBlock/RigidBlockOracle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * E2a — THE FIRST 3D COLLAPSE-MECHANISM EXTRACTION, RED.
 *
 * Slice 3a taught the oracle to read the phase-1 dual at the infeasible arm as the kinematic
 * collapse mechanism (which blocks move, which joints open), but ONLY in 2D: ExtractMechanism reads
 * each non-grounded block's THREE equilibrium rows (Fx, Fz, M) into a triple (VirtualUx, VirtualUz,
 * VirtualOmega) with a SINGLE SCALAR omega about the Y axis, and its virtual-velocity readout is the
 * 2D form v = u + omega x r with that one scalar (RigidBlockOracle.cpp: OutVx = Ux - Omega*(Pz - Cz);
 * OutVz = Uz + Omega*(Px - Cx)). E1a/E1b then added AssembleThreeD (six equilibrium rows per block,
 * four-corner contacts, the k=8 friction pyramid) but DELIBERATELY LEFT THE 3D MECHANISM UNBUILT:
 * SolveRigidBlockOnce sets EqFxRowOfBlock all INDEX_NONE on the Dim3D path (RigidBlockOracle.cpp
 * ~line 1941), which was safe there because every E1 fixture is gravity-live/feasible so the
 * infeasible arm that reads it is never taken.
 *
 * THIS TEST TAKES THAT ARM IN 3D. It poses a genuinely-3D FALLING structure on the feasibility
 * formulation (gravity DEAD, bGravityIsLive = false, no live forces) so phase 1 genuinely runs and,
 * finding no admissible force system, terminates on the infeasible arm where the mechanism is read.
 * Because EqFxRowOfBlock is all INDEX_NONE on the 3D path, ExtractMechanism sees every block as
 * grounded (its Fx row is INDEX_NONE), moves nothing, and FAILS its own non-empty check, so the
 * solve REFUSES with VerificationFailure — bAnswered = false, Mechanism.bPresent = false. That is
 * the RED: no 3D mechanism is extracted, cleanly (a well-defined empty/absent mechanism, not a
 * crash), NOT a compile error and NOT a broken fixture.
 *
 * THE FIXTURE — a free block tipping OUT OF THE X-Z PLANE about a base edge:
 *
 *   A single grounded support carries one free block through one rectangular contact patch centred
 *   at plan (0, 0), normal N = +Z, half-extents (h_u, h_v) = (10, 10) cm — a 20 x 20 cm base whose
 *   four corners sit at plan (+/-10, +/-10). The free block's centre of mass is at plan
 *   (X, Y) = (0, 20), i.e. 10 cm BEYOND the patch's +Y edge (the edge at Y = +10 that runs parallel
 *   to X), at height Z = 30 above the contact top at Z = 10. Dry (no tension), generous crushing and
 *   friction, mass 10 kg (W = 9800 uu). Gravity is -Z.
 *
 * WHY IT IS INFEASIBLE, BY HAND (independent of the LP). Take moments about the X axis through the
 * centroid. Gravity acts AT the centroid: zero moment. Every compressive corner reaction is +Z
 * (N_i >= 0, no tension) at plan-Y <= +10, i.e. at offset (cy - 20) <= -10 from the centroid, so its
 * Mx contribution (cy - 20)*N_i is <= -10*W < 0 and can never be positive. The shears in Y (V) that
 * force balance forces to sum to zero all act at the SAME height (one patch, one Z), so their net Mx
 * contribution is -dz * sum(s_v) = 0 — shear cannot rescue the overturn. Hence Mx = sum (cy-20)*N_i
 * < 0 is unbalanceable while any weight is carried: NO equilibrium exists. The block overturns.
 *
 * THE HAND-DERIVED MECHANISM. The single collapse degree of freedom is a rigid rotation about the
 * +Y base edge (the line Y = +10, Z = 10, parallel to X). That axis is the X direction: the virtual
 * angular velocity is omega = (Omega_x, 0, 0) — a rotation ABOUT X, tipping the block in the Y-Z
 * plane, which is IRREDUCIBLY OUT OF THE X-Z PLANE and CANNOT be represented by the 2D extractor's
 * single scalar VirtualOmega (that scalar is the Y-axis component). The X-symmetry of the fixture
 * (patch symmetric in X, centroid at X = 0) forbids any Omega_y or Omega_z. The centroid, sitting
 * beyond the edge, DESCENDS as it tips, so by the oracle's fixed global sign convention (a descending
 * centroid reads VirtualUz < 0) the reported triple must have VirtualUz < 0. The rigid-body velocity
 * of the centroid about the edge is v = omega x (C_com - E_edge) = (Omega_x, 0, 0) x (0, 10, 20) =
 * (0, -20*Omega_x, 10*Omega_x): no X translation, a real Y (out-of-plane) translation, and VirtualUz
 * = 10*Omega_x. So |Omega_x| is ~1/31 of the block's total generalised velocity (|u|_1 = 30|Omega_x|
 * plus |omega|_1 = |Omega_x|), which the assertions below pin conservatively.
 *
 * WHAT WE ASSERT — THE MECHANISM, NEVER DISPLACEMENT (two blocks can sever and rest in place):
 *
 *   (a) the oracle ANSWERS and reports lambda* = 0 (the dead load admits no equilibrium: it falls);
 *   (b) the mechanism is PRESENT and CERTIFIED (Farkas-verified);
 *   (c) the FREE block moves and the GROUNDED support does not;
 *   (d) the one joint OPENS/SLIDES;
 *   (e) THE GENUINELY-3D PART: the free block's rotation is about the X axis — |Omega_x| is a real
 *       fraction of the motion while the Y-axis scalar VirtualOmega (all a 2D extractor has) and the
 *       Z-axis component are negligible beside it — and the centroid descends (VirtualUz < 0) with no
 *       X translation (VirtualUx ~ 0). A 2D single-scalar-omega extractor cannot produce this.
 *
 * UNITS are derived here (mass * 980 already carries 1 N = 100 uu; 1 MPa over 1 cm2 = 10000 uu), NOT
 * imported, so a wrong production constant disagrees rather than agrees. Nothing in the mechanism
 * assertions is a force/strength comparison — they are pure kinematics — so no unit boundary is
 * crossed there; the strengths only have to make the block dry-no-tension and non-crushing.
 *
 * NEEDS A TICKING WORLD: NO. A pure hand-built FOracleProblem fed to SolveRigidBlock — no bridge, no
 * Chaos, no world tick.
 *
 * NAMED NAMESPACE, not anonymous: the unity build merges many files into one translation unit.
 */
namespace MechanismTipsThreeDSupport
{
	using namespace RigidBlockOracle;

	/* ================================================================================
	 * UNITS, derived here so a wrong production constant fails rather than agrees.
	 * ================================================================================ */

	/** MassKg * 980 is a weight in uu — the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/* ================================================================================
	 * THE OVERHANGING BLOCK, hand-built so the tipping geometry stays checkable.
	 * ================================================================================ */

	constexpr double MassKg = 10.0;         /* W = 9800 uu */
	constexpr double HalfUCm = 10.0;        /* base half-extent along X (the tipping edge's direction) */
	constexpr double HalfVCm = 10.0;        /* base half-extent along Y; +Y edge at Y = +10 */
	constexpr double ContactZCm = 10.0;     /* patch top */
	constexpr double ComXCm = 0.0;          /* X-symmetric: forbids Omega_y and Omega_z */
	constexpr double ComYCm = 20.0;         /* 10 cm BEYOND the +Y edge => overturns about it */
	constexpr double ComZCm = 30.0;         /* above the contact */
	constexpr double AreaSqCm = 400.0;      /* 20 x 20 cm patch face */

	enum { Support = 0, Free = 1 };         /* block indices */
	enum { J0 = 0 };                        /* the one joint */

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

	/**
	 * Dry, no-tension, generous elsewhere: no tensile bond can pin the overhang down (that is what
	 * makes it tip), crushing and friction are far from binding, so the only kinematic escape is a
	 * rigid rotation about the +Y base edge.
	 */
	FConnectionStrength DryBond()
	{
		FConnectionStrength S;
		S.CompressiveStrengthMPa = 1000.0;   /* >> the tiny reactions here: never crushes */
		S.TensileStrengthMPa = 0.0;          /* dry no-tension: the overhang cannot be held down */
		S.ShearCohesionMPa = 1000.0;         /* generous: the mechanism is tipping, not sliding */
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
		J.HalfLengthCm = 0.0;   /* the 2D field is unused on the 3D path */
		J.HalfUCm = HalfUCm;    /* a real rectangular base, so the tipping edge is a line (|| X) */
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
		 * The MECHANISM lives on the INFEASIBLE arm of the FEASIBILITY formulation: gravity DEAD,
		 * no live forces, so phase 1 genuinely runs and, finding no admissible force system, hands
		 * its dual to ExtractMechanism. bMinViolationReadout stays default (false) so the solve
		 * routes to SolveRigidBlockOnce, the maximise-lambda path that owns the mechanism seam.
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

/* ================================================================================================
 * THE OVERHANGING BLOCK TIPS ABOUT ITS +Y BASE EDGE — E2a, RED.
 *
 * RED BECAUSE the 3D path leaves EqFxRowOfBlock all INDEX_NONE, so ExtractMechanism reads no block
 * dual, moves nothing, and fails its non-empty check — the solve refuses with VerificationFailure
 * (bAnswered = false, Mechanism.bPresent = false) instead of naming the tipping block and its
 * opening joint. Assertions (a)/(b) fail on the missing answer/mechanism; (c)/(d)/(e) then fail on
 * the empty triples. NONE of them is displacement.
 *
 * BITE (for dev, at green): the genuinely-3D assertions in (e) — |Omega_x| dominant, the Y-axis
 * scalar VirtualOmega negligible — cannot be satisfied by a 2D single-scalar-omega extractor at all;
 * the tipping axis is X, out of the X-Z plane. Zeroing the Mx moment coefficients in AssembleThreeD
 * would also red this (the Mx row is what senses the overturn about the X-parallel edge).
 *
 * NEEDS A TICKING WORLD: NO.
 * ================================================================================================ */
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
	const double Ox = FreeT.VirtualOmegaX;   /* rotation about X — the tipping axis */
	const double Oy = FreeT.VirtualOmega;    /* rotation about Y — all a 2D extractor has */
	const double Oz = FreeT.VirtualOmegaZ;   /* rotation about Z */

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

	/* ---- (a) the oracle answers, and it FALLS (dead load admits no equilibrium => lambda* 0). ---- */
	TestTrue(TEXT("(a) [RED]: the oracle answers the infeasible 3D arm"), R.bAnswered);
	TestTrue(
		*FString::Printf(TEXT("(a) [RED]: lambda* = 0 (it falls), got %.9g"), R.Lambda),
		FMath::Abs(R.Lambda) <= 1.0e-6);

	/* ---- (b) the mechanism is present and Farkas-certified. ---- */
	TestTrue(TEXT("(b) [RED]: the mechanism is present"), M.bPresent);
	TestTrue(TEXT("(b) [RED]: the mechanism is certified"), M.bIsCertified);

	/* ---- (c) the free block moves; the grounded support does not. ---- */
	TestTrue(TEXT("(c) [RED]: the free block's triple is reported"), bFreeValid);
	TestTrue(TEXT("(c) [RED]: the free block moves"),
		bFreeValid && M.Blocks[Free].bMoves);
	TestTrue(TEXT("(c) [RED]: the grounded support's triple is reported"), bSupportValid);
	TestFalse(TEXT("(c): the grounded support does not move"),
		bSupportValid && M.Blocks[Support].bMoves);

	/* ---- (d) the one joint opens/slides. ---- */
	TestTrue(TEXT("(d) [RED]: the joint flags are reported"), bJointValid);
	TestTrue(TEXT("(d) [RED]: the base joint opens/slides"),
		bJointValid && M.JointOpensOrSlides[J0]);

	/*
	 * ---- (e) THE GENUINELY-3D PART: rotation about the X axis, out of the X-Z plane. ----
	 *
	 * The tipping axis is the +Y base edge (parallel to X), so the reported omega must be along X:
	 * |Omega_x| is a real fraction of the total motion (geometry gives ~1/31; 1% is a safe floor)
	 * while the Y-axis scalar VirtualOmega (Omega_y) — the ONLY rotation a 2D extractor can name —
	 * and the Z-axis component are negligible beside it. The centroid descends (VirtualUz < 0, the
	 * fixed global sign convention) with no X translation (X-symmetric fixture). Guarded on GenMag
	 * being positive so a still-empty mechanism reds on the moving/present checks above, not here.
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
