// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Math/RandomStream.h"

#include "Core/RigidBlock/RigidBlockOracle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * E2c: is the 3D extracted mechanism permutation-invariant? THREED_DESIGN's top risk is that
 * friction-pyramid degeneracy and redundant supports make the 3D dual non-unique. Green means the
 * block-kinematics extraction is deterministic and E2b is not needed; red on the redundant fixture
 * means E2b is the fix (this test characterises, it does not fix).
 *
 * Harness as in OracleMechanismExtractionTest.cpp: seeded Fisher-Yates permutation of blocks and
 * joints, re-solve, map the mechanism back and compare. Permute here also copies the Dim3D flag.
 *
 * Fixtures: (1) one block tipping off a 20x20 patch, a unique ray, proving the harness works;
 * (2) one block on four point supports at (+/-10, +/-10), CoM at (5, 30). Overturning moment
 * 20W about the +Y line can only be resisted by tension (forbidden), so it falls; the reactions are
 * indeterminate but the rotation is unique, with the two -Y joints opening.
 *
 * Asserts on the mechanism, never displacement: base falls with a non-empty moving set, and under
 * permutation the verdict, moving and opening sets, and each block's unit-normalised (u, omega)
 * 6-vector are unchanged. World-free.
 */
namespace MechanismPermutationThreeDSupport
{
	using namespace RigidBlockOracle;

	/** MassKg * 980 is a weight in uu; the 1 N = 100 uu factor is included. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/*
	 * Fixture 1: block on a 20x20 patch (top Z = 10), CoM at (0, 20, 30), 10 cm past the +Y edge.
	 * Dry, no tension. Tips about the +Y edge.
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
	 * Fixture 2: block on four point supports at (+/-10, +/-10), CoM at (5, 30, 30), X-offset to
	 * break symmetry. Finite friction (mu 0.6) so the pyramid facets take part.
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

		// Four grounded supports at the corners of a 20 x 20 square, tops at Z = 10.
		const int32 SmY = P.Blocks.Add(GroundedPatchSupport(-10.0, -10.0, 5.0));
		const int32 SpX = P.Blocks.Add(GroundedPatchSupport(10.0, -10.0, 5.0));
		const int32 SnX = P.Blocks.Add(GroundedPatchSupport(-10.0, 10.0, 5.0));
		const int32 SpY = P.Blocks.Add(GroundedPatchSupport(10.0, 10.0, 5.0));

		// CoM beyond the +Y support line, X-offset for asymmetry.
		const int32 Free = P.Blocks.Add(FreeBlock(10.0, 5.0, 30.0, 30.0));

		P.Joints.Add(PointBearing(SmY, Free, -10.0, -10.0));
		P.Joints.Add(PointBearing(SpX, Free, 10.0, -10.0));
		P.Joints.Add(PointBearing(SnX, Free, -10.0, 10.0));
		P.Joints.Add(PointBearing(SpY, Free, 10.0, 10.0));

		return P;
	}

	/** Seeded Fisher-Yates permutation, returned as OldIndex -> NewIndex. */
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
	 * Reorder blocks and joints, remapping block references. Starts from a full copy so Dim3D and
	 * other flags carry over.
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

	/** A block's virtual-motion 6-vector (u_x, u_y, u_z, omega_x, omega_y, omega_z). */
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
	 * L-infinity scale over all blocks' 6-vectors, used to normalise a Farkas ray (defined only up
	 * to positive scale). 0 for an empty mechanism.
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
 * E2c gate: over seeded permutations, the moving set, opening set and unit 6-vectors are unchanged.
 * Deriving the opening set from the raw plastic multipliers instead of block kinematics turns the
 * redundant fixture red. World-free.
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

	// Unit-ray tolerance; a real direction change is O(0.1-1).
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

		// Guard: empty sets would compare equal vacuously.
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

			TestEqual(
				*FString::Printf(TEXT("%s seed=0x%X: the verdict is permutation-invariant (Falls)"), *Fixture.Label, Seed),
				static_cast<int32>(OutcomeOf(PermR)), static_cast<int32>(EOracleOutcome::Falls));

			// Map the permuted sets back to base indices.
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

				// Compare all six unit-ray components.
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
