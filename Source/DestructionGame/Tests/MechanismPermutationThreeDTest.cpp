// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Math/RandomStream.h"

#include "Core/RigidBlock/RigidBlockOracle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * E2c: the 3D mechanism permutation-determinism fuzz, the decision point on the top 3D risk.
 *
 * THREED_DESIGN's #1 risk: "Friction-pyramid degeneracy worsens R3 mechanism determinism." In 3D a
 * block has 6-DOF motion, each contact carries a k=8 friction pyramid, and redundant supports (four
 * bearings under one block) admit multiple Farkas rays, so the raw dual is non-unique. The open
 * question: does that leak into the extracted mechanism (which blocks move, which joints open, each
 * block's (u, omega) 6-vector), or does the 3D extractor, like 2D Slice 3a, read the mechanism off
 * the stable block kinematics and stay permutation-invariant?
 *
 *   - Both fixtures holding => the 3D block-kinematics mechanism is deterministic, E2 complete and
 *     E2b (per-group canonicalisation) not needed.
 *   - The redundant fixture drifting => the 3D non-determinism the design flagged, and E2b is the
 *     fix. This test characterises it, it does not fix it.
 *
 * The harness mirrors the 2D gate (OracleMechanismExtractionTest.cpp): a seeded Fisher-Yates
 * permutation of block and joint indices, the physics re-posed and solved, and the mechanism
 * translated back through the inverse and compared to the base. Re-derived here, so it also copies
 * the Dim3D flag the 2D Permute helper never had to.
 *
 * Two fixtures:
 *
 *   1. Determinate tipping (expected green). One free block on a 20x20 patch, CoM 10 cm beyond the
 *      +Y edge, tipping in a unique edge-rotation ray. Pins that the harness works.
 *
 *   2. Indeterminate redundant-support falling (the real stressor). One free block bearing on four
 *      grounded point supports at plan (+/-10, +/-10), each a k=8 pyramid, CoM at plan (5, 30),
 *      X-offset so no symmetry pins the reactions. Four normal reactions balance three equations, 1x
 *      indeterminate, and the pyramid adds facet freedom, so the force certificate is non-unique, yet
 *      the block tips about the +Y line in one rotation, so its kinematics should be unique.
 *
 * Fixture 2 falls, by hand: moments about the +Y line give overturning W*(30-10) = 20W; the +Y
 * supports sit on the axis and the -Y supports could only restore in tension (forbidden), so
 * Mx = 20W is unbalanceable. Redundant because any vertical load admits infinitely many splits across
 * four supports. The hand-derived mechanism is a rotation about the +Y line: the two -Y joints open,
 * the two +Y joints on the axis do not, a non-trivial opening subset (2 of 4).
 *
 * Asserted (the mechanism, never displacement): base falls and names a non-empty moving set (guarding
 * the vacuity); under permutation the verdict stays Falls, the moving-block and opening-joint sets
 * mapped back are identical, and each block's (u, omega) 6-vector, normalised to a unit ray (a Farkas
 * ray is defined only up to positive scale), matches within tolerance.
 *
 * Units derived here (MassKg*980 carries 1 N = 100 uu), not imported; mechanism assertions are pure
 * kinematics, so no unit boundary is crossed. No ticking world (hand-built FOracleProblems). Named
 * namespace, since the unity build merges files.
 */
namespace MechanismPermutationThreeDSupport
{
	using namespace RigidBlockOracle;

	/** MassKg * 980 is a weight in uu — the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/*
	 * Fixture 1: the determinate tipping block (unique edge-rotation ray). One free block on a 20x20
	 * patch (top Z = 10), CoM at plan (0, 20), 10 cm beyond the +Y edge, at Z = 30. Dry no-tension,
	 * generous crush/friction. It tips about the +Y edge in a unique rotation about X.
	 */

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

	/*
	 * Fixture 2: the redundant four-support falling block (multiple Farkas rays). One free block on
	 * four grounded point supports (each a normal contact plus a k=8 pyramid) at plan (+/-10, +/-10),
	 * tops at Z = 10. CoM at plan (5, 30), Z = 30, X-offset so nothing symmetric pins the reactions.
	 * Finite friction (mu = 0.6, small cohesion) so the pyramid facets participate.
	 */

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

	/* The permutation harness, re-derived from the 2D gate, plus copying Dim3D. */

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
	 * whole-struct copy so Dim (and every scalar flag) carries over, the one thing the 2D Permute
	 * helper never needed and would silently drop, reverting to Dim2D.
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

	/* Mechanism inspection: the named sets and the scale-invariant velocity fingerprint. */

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
	 * Dividing by it turns a Farkas ray (unique only up to positive scale) into a comparable unit ray,
	 * so an invariant direction reads identical even if the solves scaled it differently. Returns 0
	 * for an empty mechanism.
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

/*
 * The 3D permutation-determinism gate (E2c). For each fixture and several seeded block+joint
 * permutations, asserts the extracted mechanism is permutation-invariant: the moving-block set, the
 * opening-joint set, and every block's unit (u,omega) 6-vector unchanged once the permutation is
 * inverted. The base is guarded non-empty so the set-equality isn't vacuously green.
 *
 * Outcome read from the run:
 *   - Both green => the 3D block-kinematics mechanism is permutation-stable; E2 complete, E2b not
 *     needed. Bite: mutate ExtractMechanism to derive the opening-joint set from the raw plastic
 *     multipliers (the non-unique dual) instead of the block kinematics, and the redundant fixture's
 *     opening set tracks column order and this goes red.
 *   - Redundant red => the 3D non-determinism the design flagged is real; characterise the drift and
 *     hand to E2b, do not fix it here.
 *
 * No ticking world.
 */
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

		/* Guard against vacuity: two empty sets compare equal, so the base must name something. */
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
