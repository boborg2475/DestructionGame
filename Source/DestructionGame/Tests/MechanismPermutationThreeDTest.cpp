// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Math/RandomStream.h"

#include "Core/RigidBlock/RigidBlockOracle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * E2c — THE 3D MECHANISM PERMUTATION-DETERMINISM FUZZ. THE DECISION POINT ON THE TOP 3D RISK.
 *
 * THREED_DESIGN's #1 risk: "Friction-pyramid degeneracy worsens R3 mechanism determinism." In 3D a
 * block has 6-DOF motion, each contact carries a k=8 friction pyramid, and REDUNDANT supports (E0-B,
 * four bearings under one block) admit MULTIPLE Farkas rays — so the raw dual / plastic multipliers
 * are non-unique. The OPEN question this test decides: does that force non-uniqueness leak into the
 * extracted MECHANISM (which blocks move, which joints open, each block's (u, omega) 6-vector), or
 * does the 3D extractor — like 2D Slice 3a — read the mechanism off the STABLE BLOCK KINEMATICS and
 * stay permutation-invariant regardless?
 *
 *   - If BOTH fixtures below hold under permutation, the 3D block-kinematics mechanism is
 *     deterministic: E2 is complete and E2b (per-group canonicalisation / the D7 minimal-support
 *     tie-break) is NOT needed — the stable-kinematics set suffices exactly as it did in 2D.
 *   - If the REDUNDANT fixture DRIFTS (names different blocks/joints or different velocities under
 *     different column orderings), that is the 3D non-determinism the design flagged, and E2b is the
 *     fix. This test CHARACTERISES it; it does not fix it.
 *
 * THE HARNESS mirrors the 2D gate DestructionGame.Oracle.RigidBlock.Mechanism.IsPermutationDeterministic
 * (OracleMechanismExtractionTest.cpp): a seeded Fisher-Yates permutation of block and joint indices
 * (which reorders every downstream struct COLUMN), the physics re-posed in that order, solved, and the
 * extracted mechanism translated back through the known permutation inverse and compared to the base.
 * It is re-derived here (not linked across translation units) so it also copies the Dim3D flag the 2D
 * Permute helper never had to.
 *
 * TWO FIXTURES:
 *
 *   1. DETERMINATE TIPPING (the E2a-review-recommended guard, expected GREEN). The E2a geometry: one
 *      free block on a single 20x20 rectangular patch, CoM 10 cm beyond the patch's +Y edge, so it
 *      tips about that edge in a UNIQUE edge-rotation ray. A unique ray must be permutation-invariant;
 *      this pins that the harness itself works and that a determinate 3D mechanism is stable.
 *
 *   2. INDETERMINATE REDUNDANT-SUPPORT FALLING (the real E2c stressor). E0-B lifted to a collapse:
 *      one free block bearing on FOUR grounded point supports at plan (+/-10, +/-10), each with a k=8
 *      friction pyramid, its CoM at plan (5, 30) — 20 cm BEYOND the +Y support line (Y = 10) and
 *      X-OFFSET so no symmetry pins the reactions. FOUR normal reactions balance three equations
 *      (SigmaFz, Mx, My) => 1x statically INDETERMINATE, and the pyramid adds shear/facet freedom:
 *      the FORCE certificate is genuinely non-unique (multiple Farkas rays). Yet the block tips about
 *      the +Y support line in a single rigid rotation, so its KINEMATICS should be unique. This is the
 *      exact "force non-unique, kinematics unique" case the 2D result predicts stays stable.
 *
 * WHY FIXTURE 2 FALLS, BY HAND (independent of the LP). Moments about the +Y support line (axis || X
 * at Y = 10, Z = 10). Gravity acts at the CoM (Y = 30): overturning moment W*(30 - 10) = 20W about
 * that line, tipping the block over the +Y edge. The two +Y supports sit ON the axis (zero restoring
 * lever). The two -Y supports (Y = -10) could only restore by pulling the block DOWN (tension) as
 * that side rises — dry no-tension forbids it, so they lift off. All contacts share one height
 * (Z = 10), so the in-plane shears have zero lever about the axis and cannot make Mx. Hence
 * Mx = 20W > 0 is unbalanceable: NO equilibrium exists, the block overturns. WHY REDUNDANT: any
 * vertical load admits infinitely many splits across four supports (four unknowns, three equations),
 * and each contact's pyramid adds shear unknowns — the reaction system, and thus the Farkas
 * certificate, is non-unique even though the tip is one rotation.
 *
 * THE HAND-DERIVED MECHANISM (fixture 2). A rigid rotation about the +Y support line: omega along X
 * (Omega_x != 0, Omega_y ~ Omega_z ~ 0), the CoM beyond the edge DESCENDS (VirtualUz < 0). The two
 * -Y support joints OPEN (their contact points lift off the axis); the two +Y support joints sit on
 * the rotation axis (zero relative velocity) and do NOT open. So the opening set is a NON-TRIVIAL
 * subset (2 of 4) — the case a wobbling canonicalisation would get wrong.
 *
 * WHAT WE ASSERT — THE MECHANISM, NEVER DISPLACEMENT:
 *   (base) the fixture FALLS and names a NON-EMPTY moving set (guards the set-equality against the
 *          two-empty-sets-compare-equal vacuity);
 *   (perm) the verdict stays Falls; the moving-block set and the opening-joint set, mapped back
 *          through the permutation inverse, are IDENTICAL to the base; and each block's (u, omega)
 *          6-vector, normalised to a unit ray (the Farkas ray is defined only up to positive scale),
 *          matches the base within a tight tolerance.
 *
 * UNITS are derived here (MassKg*980 already carries 1 N = 100 uu), NOT imported. Nothing asserted is
 * a force/strength comparison — the mechanism assertions are pure kinematics — so no unit boundary is
 * crossed; the strengths only have to make the block dry-no-tension and non-crushing.
 *
 * NEEDS A TICKING WORLD: NO. Pure hand-built FOracleProblems fed to SolveRigidBlock — no bridge, no
 * Chaos, no world tick.
 *
 * NAMED NAMESPACE, not anonymous: the unity build merges many files into one translation unit.
 */
namespace MechanismPermutationThreeDSupport
{
	using namespace RigidBlockOracle;

	/* ================================================================================
	 * UNITS, derived here so a wrong production constant fails rather than agrees.
	 * ================================================================================ */

	/** MassKg * 980 is a weight in uu — the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/* ================================================================================
	 * FIXTURE 1 — THE DETERMINATE E2a TIPPING BLOCK (unique edge-rotation ray).
	 *
	 * Mirrors DestructionGame.Oracle.RigidBlock.ThreeD.MechanismTipsAboutABaseEdge: one free block on
	 * a single 20x20 patch (half-extents 10,10, normal +Z, top Z = 10), CoM at plan (0, 20) — 10 cm
	 * beyond the patch's +Y edge — at Z = 30. Dry no-tension, generous crush/friction. It tips about
	 * the +Y edge (line || X at Y = +10): a UNIQUE rotation about X.
	 * ================================================================================ */

	FConnectionStrength GenerousDryBond()
	{
		FConnectionStrength S;
		S.CompressiveStrengthMPa = 1000.0; /* never crushes at these tiny reactions */
		S.TensileStrengthMPa = 0.0;        /* dry no-tension: the overhang cannot be held down */
		S.ShearCohesionMPa = 1000.0;       /* generous: the mode is tipping, not sliding */
		S.FrictionCoefficient = 1000.0;
		return S;
	}

	FOracleBlock GroundedPatchSupport(double Cx, double Cy, double Cz)
	{
		FOracleBlock B;
		B.MassKg = 1.0;
		B.CentroidXCm = Cx;
		B.CentroidYCm = Cy;
		B.CentroidZCm = Cz;
		B.bGrounded = true;
		return B;
	}

	FOracleBlock FreeBlock(double MassKg, double Cx, double Cy, double Cz)
	{
		FOracleBlock B;
		B.MassKg = MassKg;
		B.CentroidXCm = Cx;
		B.CentroidYCm = Cy;
		B.CentroidZCm = Cz;
		B.bGrounded = false;
		return B;
	}

	FOracleProblem BuildDeterminateTippingBlock()
	{
		FOracleProblem P;
		P.Dim = EOracleDim::Dim3D;
		P.bGravityIsLive = false; /* feasibility formulation: gravity dead, phase 1 runs the mechanism arm */

		const int32 Support = P.Blocks.Add(GroundedPatchSupport(0.0, 0.0, 5.0));
		const int32 Free = P.Blocks.Add(FreeBlock(10.0, 0.0, 20.0, 30.0));

		FOracleJoint J;
		J.BlockA = Support;
		J.BlockB = Free;
		J.NormalX = 0.0;
		J.NormalY = 0.0;
		J.NormalZ = 1.0;
		J.CentreXCm = 0.0;
		J.CentreYCm = 0.0;
		J.CentreZCm = 10.0;
		J.HalfLengthCm = 0.0;
		J.HalfUCm = 10.0; /* 20 x 20 patch: the +Y edge is a line || X at Y = +10 */
		J.HalfVCm = 10.0;
		J.AreaSqCm = 400.0;
		J.Strength = GenerousDryBond();
		P.Joints.Add(J);

		return P;
	}

	/* ================================================================================
	 * FIXTURE 2 — THE REDUNDANT FOUR-SUPPORT FALLING BLOCK (multiple Farkas rays).
	 *
	 * One free block on FOUR grounded point supports (half-extents 0 -> a single normal contact each,
	 * plus the k=8 friction pyramid) at plan (+/-10, +/-10), tops at Z = 10. CoM at plan (5, 30),
	 * Z = 30 — beyond the +Y support line and X-offset so nothing symmetric pins the reactions. FINITE
	 * friction (mu = 0.6, small cohesion) so the pyramid facets are real, participating constraints.
	 * ================================================================================ */

	FConnectionStrength FiniteFrictionDryBearing()
	{
		FConnectionStrength S;
		S.CompressiveStrengthMPa = 1000.0; /* generous: crushing never binds; the mode is tipping */
		S.TensileStrengthMPa = 0.0;        /* dry no-tension: -Y supports lift off, cannot restore */
		S.ShearCohesionMPa = 0.5;          /* small but non-zero: the pyramid has finite cohesion */
		S.FrictionCoefficient = 0.6;       /* FINITE: the k=8 pyramid facets bind, not a free cone */
		return S;
	}

	FOracleJoint PointBearing(int32 BlockA, int32 BlockB, double Cx, double Cy)
	{
		FOracleJoint J;
		J.BlockA = BlockA;
		J.BlockB = BlockB;
		J.NormalX = 0.0;
		J.NormalY = 0.0;
		J.NormalZ = 1.0;
		J.CentreXCm = Cx;
		J.CentreYCm = Cy;
		J.CentreZCm = 10.0;
		J.HalfLengthCm = 0.0;
		J.HalfUCm = 0.0; /* point patch: the four corners collapse onto the centre */
		J.HalfVCm = 0.0;
		J.AreaSqCm = 100.0;
		J.Strength = FiniteFrictionDryBearing();
		return J;
	}

	FOracleProblem BuildRedundantFourSupportFalling()
	{
		FOracleProblem P;
		P.Dim = EOracleDim::Dim3D;
		P.bGravityIsLive = false;

		/* Four grounded supports at the corners of a 20 x 20 plan square, tops at Z = 10. */
		const int32 SmY = P.Blocks.Add(GroundedPatchSupport(-10.0, -10.0, 5.0));
		const int32 SpX = P.Blocks.Add(GroundedPatchSupport(10.0, -10.0, 5.0));
		const int32 SnX = P.Blocks.Add(GroundedPatchSupport(-10.0, 10.0, 5.0));
		const int32 SpY = P.Blocks.Add(GroundedPatchSupport(10.0, 10.0, 5.0));

		/* The overhanging free block: CoM beyond the +Y support line (Y = 10), X-offset for asymmetry. */
		const int32 Free = P.Blocks.Add(FreeBlock(10.0, 5.0, 30.0, 30.0));

		P.Joints.Add(PointBearing(SmY, Free, -10.0, -10.0));
		P.Joints.Add(PointBearing(SpX, Free, 10.0, -10.0));
		P.Joints.Add(PointBearing(SnX, Free, -10.0, 10.0));
		P.Joints.Add(PointBearing(SpY, Free, 10.0, 10.0));

		return P;
	}

	/* ================================================================================
	 * THE PERMUTATION HARNESS — re-derived from the 2D gate, plus copying Dim3D.
	 * ================================================================================ */

	/** A seeded Fisher-Yates permutation returning OldIndex -> NewIndex (the 2D harness's convention). */
	TArray<int32> SeededPermutation(FRandomStream& Rng, int32 N)
	{
		TArray<int32> Perm;
		Perm.SetNumUninitialized(N);
		for (int32 I = 0; I < N; ++I)
		{
			Perm[I] = I;
		}
		for (int32 I = N - 1; I > 0; --I)
		{
			const int32 J = Rng.RandRange(0, I);
			Swap(Perm[I], Perm[J]);
		}
		TArray<int32> Inv;
		Inv.SetNumUninitialized(N);
		for (int32 New = 0; New < N; ++New)
		{
			Inv[Perm[New]] = New;
		}
		return Inv;
	}

	/**
	 * Re-order an FOracleProblem's blocks and joints, remapping every block reference. Starts from a
	 * whole-struct copy so Dim (and every other scalar flag) carries over — the ONE thing the 2D
	 * Permute helper never needed and would silently drop, reverting the 3D path to Dim2D.
	 */
	FOracleProblem Permute(const FOracleProblem& In, const TArray<int32>& BlockPerm, const TArray<int32>& JointPerm)
	{
		FOracleProblem Out = In;

		Out.Blocks.SetNum(In.Blocks.Num());
		for (int32 Old = 0; Old < In.Blocks.Num(); ++Old)
		{
			Out.Blocks[BlockPerm[Old]] = In.Blocks[Old];
		}

		Out.Joints.SetNum(In.Joints.Num());
		for (int32 Old = 0; Old < In.Joints.Num(); ++Old)
		{
			FOracleJoint J = In.Joints[Old];
			J.BlockA = BlockPerm[J.BlockA];
			J.BlockB = BlockPerm[J.BlockB];
			Out.Joints[JointPerm[Old]] = J;
		}

		for (int32 F = 0; F < Out.AppliedForces.Num(); ++F)
		{
			Out.AppliedForces[F].Block = BlockPerm[In.AppliedForces[F].Block];
		}
		return Out;
	}

	/* ================================================================================
	 * MECHANISM INSPECTION — the named sets and the scale-invariant velocity fingerprint.
	 * ================================================================================ */

	TSet<int32> MovingBlocks(const FOracleMechanism& M)
	{
		TSet<int32> Moving;
		for (int32 B = 0; B < M.Blocks.Num(); ++B)
		{
			if (M.Blocks[B].bMoves)
			{
				Moving.Add(B);
			}
		}
		return Moving;
	}

	TSet<int32> OpeningJoints(const FOracleMechanism& M)
	{
		TSet<int32> Opening;
		for (int32 J = 0; J < M.JointOpensOrSlides.Num(); ++J)
		{
			if (M.JointOpensOrSlides[J])
			{
				Opening.Add(J);
			}
		}
		return Opening;
	}

	bool BlockMoves(const FOracleMechanism& M, int32 B)
	{
		return M.Blocks.IsValidIndex(B) && M.Blocks[B].bMoves;
	}

	bool JointOpens(const FOracleMechanism& M, int32 J)
	{
		return M.JointOpensOrSlides.IsValidIndex(J) && M.JointOpensOrSlides[J];
	}

	/** A block's full 3D virtual-motion 6-vector (u_x,u_y,u_z, omega_x, omega_y, omega_z). */
	void SixVector(const FOracleMechanismBlock& T, double Out[6])
	{
		Out[0] = T.VirtualUx;
		Out[1] = T.VirtualUy;
		Out[2] = T.VirtualUz;
		Out[3] = T.VirtualOmegaX;
		Out[4] = T.VirtualOmega; /* the Y-axis rotation */
		Out[5] = T.VirtualOmegaZ;
	}

	/**
	 * The mechanism's L-infinity scale: the largest absolute component over all blocks' 6-vectors.
	 * Dividing by it turns a Farkas ray (unique only up to POSITIVE scale) into a comparable unit
	 * ray, so a genuinely-invariant DIRECTION reads identical even if the two solves scaled the ray
	 * differently. Returns 0 for an empty/zero mechanism.
	 */
	double MechanismScale(const FOracleMechanism& M)
	{
		double Scale = 0.0;
		for (int32 B = 0; B < M.Blocks.Num(); ++B)
		{
			double Six[6];
			SixVector(M.Blocks[B], Six);
			for (int32 K = 0; K < 6; ++K)
			{
				Scale = FMath::Max(Scale, FMath::Abs(Six[K]));
			}
		}
		return Scale;
	}
}

/* ================================================================================================
 * THE 3D PERMUTATION-DETERMINISM GATE — E2c, THE DECISION POINT ON THE TOP 3D RISK.
 *
 * For each fixture and several seeded block+joint permutations, asserts the extracted mechanism is
 * permutation-invariant: the moving-block set, the opening-joint set, and every block's unit (u,omega)
 * 6-vector are unchanged once the permutation is inverted. The base is guarded non-empty so the
 * set-equality is not vacuously green.
 *
 * OUTCOME READ FROM THE RUN:
 *   - BOTH GREEN  => the 3D block-kinematics mechanism is permutation-stable; E2 is complete; the
 *     E2b canonicalisation is not needed. BITE: mutate ExtractMechanism to derive the opening-joint
 *     set from the raw plastic MULTIPLIERS (the non-unique strength-row dual) instead of the block
 *     kinematics, or to name the moving set off a pivot-order-dependent quantity — the redundant
 *     fixture's opening set then tracks column order and this test goes red.
 *   - REDUNDANT RED => the 3D non-determinism the design flagged is real; characterise the drift and
 *     hand to E2b. Do NOT fix it here.
 *
 * NEEDS A TICKING WORLD: NO.
 * ================================================================================================ */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMechanismPermutationThreeDTest,
	"DestructionGame.Oracle.RigidBlock.ThreeD.MechanismIsPermutationDeterministic",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMechanismPermutationThreeDTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace MechanismPermutationThreeDSupport;

	struct FNamed
	{
		FOracleProblem Base;
		FString Label;
	};

	TArray<FNamed> Fixtures;
	Fixtures.Add({ BuildDeterminateTippingBlock(), TEXT("1-determinate-tip") });
	Fixtures.Add({ BuildRedundantFourSupportFalling(), TEXT("2-redundant-four-support") });

	const int32 BaseSeed = 0x3D0FA11;
	const int32 NumPermutations = 8;

	/* The velocity fingerprint tolerance: a unit ray, so a genuine direction change is O(0.1-1). */
	const double VelTol = 1.0e-6;

	for (const FNamed& Fixture : Fixtures)
	{
		const FOracleProblem& Base = Fixture.Base;
		const FOracleResult BaseR = SolveRigidBlock(Base);

		TestEqual(*FString::Printf(TEXT("%s: base is infeasible (Falls)"), *Fixture.Label),
			static_cast<int32>(OutcomeOf(BaseR)), static_cast<int32>(EOracleOutcome::Falls));
		TestTrue(*FString::Printf(TEXT("%s: base mechanism is present"), *Fixture.Label),
			BaseR.Mechanism.bPresent);

		const TSet<int32> BaseMoving = MovingBlocks(BaseR.Mechanism);
		const TSet<int32> BaseOpening = OpeningJoints(BaseR.Mechanism);
		const double BaseScale = MechanismScale(BaseR.Mechanism);

		FString MovingList;
		for (int32 B : BaseMoving.Array())
		{
			MovingList += FString::Printf(TEXT("%d "), B);
		}
		FString OpeningList;
		for (int32 J : BaseOpening.Array())
		{
			OpeningList += FString::Printf(TEXT("%d "), J);
		}

		AddInfo(*FString::Printf(
			TEXT("%s: base Falls, present %d, moving {%s} (%d), opening {%s} (%d); ")
			TEXT("DEGENERACY: %d Bland entries, %d pivots over %d blocks/%d joints; scale %.6g"),
			*Fixture.Label, BaseR.Mechanism.bPresent ? 1 : 0, *MovingList, BaseMoving.Num(),
			*OpeningList, BaseOpening.Num(), BaseR.BlandDegenerateEntries, BaseR.SimplexIterations,
			Base.Blocks.Num(), Base.Joints.Num(), BaseScale));

		/* GUARD AGAINST VACUITY: two empty sets compare equal. The base MUST name something. */
		TestTrue(*FString::Printf(TEXT("%s [GUARD]: the base names a NON-EMPTY moving set"), *Fixture.Label),
			BaseMoving.Num() >= 1);
		TestTrue(*FString::Printf(TEXT("%s [GUARD]: the base names a NON-EMPTY opening set"), *Fixture.Label),
			BaseOpening.Num() >= 1);
		TestTrue(*FString::Printf(TEXT("%s [GUARD]: the base ray has a positive scale"), *Fixture.Label),
			BaseScale > 0.0);

		double WorstRawDualDrift = 0.0;   /* raw VirtualUz drift: > 0 means the dual is genuinely non-unique */
		double WorstUnitVelDrift = 0.0;   /* the scale-invariant direction drift the gate asserts on */

		for (int32 Perm = 0; Perm < NumPermutations; ++Perm)
		{
			const int32 Seed = BaseSeed + Perm;
			FRandomStream Rng(Seed);

			const TArray<int32> BlockPerm = SeededPermutation(Rng, Base.Blocks.Num());
			const TArray<int32> JointPerm = Base.Joints.Num() > 0
				? SeededPermutation(Rng, Base.Joints.Num())
				: TArray<int32>();

			const FOracleProblem PermProblem = Permute(Base, BlockPerm, JointPerm);
			const FOracleResult PermR = SolveRigidBlock(PermProblem);
			const double PermScale = MechanismScale(PermR.Mechanism);

			/* The verdict itself must be permutation-invariant. */
			TestEqual(
				*FString::Printf(TEXT("%s seed=0x%X: the verdict is permutation-invariant (Falls)"), *Fixture.Label, Seed),
				static_cast<int32>(OutcomeOf(PermR)), static_cast<int32>(EOracleOutcome::Falls));

			/* Translate the permuted named sets back to the base index space. */
			TSet<int32> PermMovingInBase;
			for (int32 Old = 0; Old < Base.Blocks.Num(); ++Old)
			{
				if (BlockMoves(PermR.Mechanism, BlockPerm[Old]))
				{
					PermMovingInBase.Add(Old);
				}

				const double BaseUz = BaseR.Mechanism.Blocks.IsValidIndex(Old)
					? BaseR.Mechanism.Blocks[Old].VirtualUz : 0.0;
				const double PermUz = PermR.Mechanism.Blocks.IsValidIndex(BlockPerm[Old])
					? PermR.Mechanism.Blocks[BlockPerm[Old]].VirtualUz : 0.0;
				WorstRawDualDrift = FMath::Max(WorstRawDualDrift, FMath::Abs(BaseUz - PermUz));

				/* The scale-invariant unit-ray comparison — every one of the six components. */
				if (BaseScale > 0.0 && PermScale > 0.0
					&& BaseR.Mechanism.Blocks.IsValidIndex(Old)
					&& PermR.Mechanism.Blocks.IsValidIndex(BlockPerm[Old]))
				{
					double BaseSix[6];
					double PermSix[6];
					SixVector(BaseR.Mechanism.Blocks[Old], BaseSix);
					SixVector(PermR.Mechanism.Blocks[BlockPerm[Old]], PermSix);
					for (int32 K = 0; K < 6; ++K)
					{
						WorstUnitVelDrift = FMath::Max(WorstUnitVelDrift,
							FMath::Abs(BaseSix[K] / BaseScale - PermSix[K] / PermScale));
					}
				}
			}

			TSet<int32> PermOpeningInBase;
			for (int32 Old = 0; Old < Base.Joints.Num(); ++Old)
			{
				if (JointOpens(PermR.Mechanism, JointPerm[Old]))
				{
					PermOpeningInBase.Add(Old);
				}
			}

			const bool bMovingSame =
				PermMovingInBase.Num() == BaseMoving.Num() && PermMovingInBase.Includes(BaseMoving);
			const bool bOpeningSame =
				PermOpeningInBase.Num() == BaseOpening.Num() && PermOpeningInBase.Includes(BaseOpening);

			TestTrue(
				*FString::Printf(
					TEXT("%s seed=0x%X [GATE]: the MOVING set is invariant (base %d, permuted %d)"),
					*Fixture.Label, Seed, BaseMoving.Num(), PermMovingInBase.Num()),
				bMovingSame);
			TestTrue(
				*FString::Printf(
					TEXT("%s seed=0x%X [GATE]: the OPENING set is invariant (base %d, permuted %d)"),
					*Fixture.Label, Seed, BaseOpening.Num(), PermOpeningInBase.Num()),
				bOpeningSame);
		}

		/* The velocity fingerprint gate: every block's unit 6-vector is permutation-stable. */
		TestTrue(
			*FString::Printf(
				TEXT("%s [GATE]: the unit (u,omega) 6-vectors are permutation-invariant (worst drift %.3e <= %.1e). ")
				TEXT("If RED here on the redundant fixture, this is the E2c 3D non-determinism -> E2b, do not fix."),
				*Fixture.Label, WorstUnitVelDrift, VelTol),
			WorstUnitVelDrift <= VelTol);

		AddInfo(*FString::Printf(
			TEXT("%s: worst raw VirtualUz drift %.3e (> 0 => the raw dual is non-unique, so the stable "
				 "named set/kinematics is doing real work); worst unit-velocity drift %.3e"),
			*Fixture.Label, WorstRawDualDrift, WorstUnitVelDrift));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
