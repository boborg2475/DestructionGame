// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Math/RandomStream.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"
#include "Core/WallCases.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Slice-3b gate: is the canonical collapse mechanism permutation-deterministic on a multi-mode,
 * degenerate, infeasible structure? Slice 3a proved it only on single-mode collapses; with several
 * simultaneous modes there are multiple Farkas rays (CURRENT_STATE Slice 3a item (c)). A wobble
 * here is the D7 design gate firing, not a bug to patch under this test.
 *
 * Fixture E: two independent overhangs far apart. Tests that both modes are named. Fixture F: a
 * dry wall with a wide opening, whose cover comes down as a coupled many-brick collapse. Tests
 * determinism at scale.
 *
 * Tau pin (item (d)): the canonicalization is reimplemented here, checked against production's
 * bMoves at tau = 1e-6, then required to give the same sets at 1e-5 and 1e-7 with >= 2 orders of
 * separation around tau.
 *
 * Units and contact kinematics (v = u + omega x r) are derived here, not imported. Pure
 * FOracleProblem fixtures; no world tick. Named namespace for unity builds.
 */
namespace OracleMultiModeDeterminismSupport
{
	using namespace RigidBlockOracle;
	using namespace DestructionProfiles;

	// Units, derived here so a wrong production constant fails rather than agrees.
	constexpr double GravityCmPerSecondSquared = 980.0;
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;
	constexpr double ClayDensityGramsPerCubicCm = 1.9;
	constexpr double WytheWidthCm = 10.25;

	FOracleBlock GroundedSeat(double CentreXCm, double CentreZCm)
	{
		FOracleBlock B;
		B.MassKg = 1.0;
		B.CentroidXCm = CentreXCm;
		B.CentroidZCm = CentreZCm;
		B.bGrounded = true;
		return B;
	}

	FOracleBlock FreeBlock(double MassKg, double CentreXCm, double CentreZCm)
	{
		FOracleBlock B;
		B.MassKg = MassKg;
		B.CentroidXCm = CentreXCm;
		B.CentroidZCm = CentreZCm;
		B.bGrounded = false;
		return B;
	}

	FOracleJoint BedJoint(
		int32 BlockA, int32 BlockB, double CentreXCm, double CentreZCm, double HalfLengthCm,
		double AreaSqCm, const FConnectionStrength& Strength)
	{
		FOracleJoint J;
		J.BlockA = BlockA;
		J.BlockB = BlockB;
		J.NormalX = 0.0;
		J.NormalZ = 1.0;
		J.CentreXCm = CentreXCm;
		J.CentreZCm = CentreZCm;
		J.HalfLengthCm = HalfLengthCm;
		J.AreaSqCm = AreaSqCm;
		J.Strength = Strength;
		return J;
	}

	/*
	 * An overhanging bar past tipping: a long bar on a narrow seat at its left end, so it rotates
	 * about the seat's right edge and the joint's left contact lifts.
	 */

	constexpr double BarSeatHalfXCm = 5.0;
	constexpr double BarBodyLeftXCm = -5.0;
	constexpr double BarBodyRightXCm = 200.0;
	constexpr double BarBodyThicknessZCm = 40.0;

	double BarBodyMassKg()
	{
		return ClayDensityGramsPerCubicCm
			* (BarBodyRightXCm - BarBodyLeftXCm) * WytheWidthCm * BarBodyThicknessZCm / 1000.0;
	}

	/** Appends a seat, bar and bed joint at OriginXCm and returns their indices. */
	void AppendOverhang(FOracleProblem& P, double OriginXCm, int32& OutSeat, int32& OutBody, int32& OutJoint)
	{
		const double SeatCentreXCm = OriginXCm;
		const double BodyCentreXCm = OriginXCm + (BarBodyLeftXCm + BarBodyRightXCm) / 2.0;
		const double JointAreaSqCm = (2.0 * BarSeatHalfXCm) * WytheWidthCm;

		OutSeat = P.Blocks.Add(GroundedSeat(SeatCentreXCm, 10.0));
		OutBody = P.Blocks.Add(FreeBlock(BarBodyMassKg(), BodyCentreXCm, 41.0));
		OutJoint = P.Joints.Add(BedJoint(
			OutSeat, OutBody, SeatCentreXCm, 20.5, BarSeatHalfXCm, JointAreaSqCm, GeneralPurposeMortar));
	}

	FOracleProblem BuildSingleOverhang()
	{
		FOracleProblem P;
		P.bGravityIsLive = false;
		int32 Seat, Body, Joint;
		AppendOverhang(P, 0.0, Seat, Body, Joint);
		return P;
	}

	struct FTwoIndependent
	{
		FOracleProblem Problem;
		int32 SeatA = INDEX_NONE, BodyA = INDEX_NONE, JointA = INDEX_NONE;
		int32 SeatB = INDEX_NONE, BodyB = INDEX_NONE, JointB = INDEX_NONE;
	};

	FTwoIndependent BuildTwoIndependentOverhangs()
	{
		FTwoIndependent Out;
		Out.Problem.bGravityIsLive = false;

		// 1000 cm apart: no shared block or joint.
		AppendOverhang(Out.Problem, 0.0, Out.SeatA, Out.BodyA, Out.JointA);
		AppendOverhang(Out.Problem, 1000.0, Out.SeatB, Out.BodyB, Out.JointB);
		return Out;
	}

	/*
	 * Fixture F: a dry wall with a wide opening. With no tension the cover cannot hang over the
	 * void, so it is infeasible at self-weight. ~100+ blocks puts it in the degenerate regime.
	 */

	constexpr double WallBrickLengthCm = 21.5;
	constexpr double WallBrickHeightCm = 6.5;
	constexpr double WallJointCm = 1.0;

	/** One grounded course, a 3-course opening, then CoverCourses of cover. */
	constexpr int32 WallOpeningCourses = 3;

	bool BuildOpeningWall(
		int32 OpeningCells, int32 JambCells, int32 CoverCourses,
		const FConnectionStrength& Strength, FStructure& Out, FString& OutWhy)
	{
		DestructionWallCases::FWallSpec Spec;
		Spec.BrickSizeCm = FVector(WallBrickLengthCm, WytheWidthCm, WallBrickHeightCm);
		Spec.JointThicknessCm = WallJointCm;
		Spec.DensityGramsPerCubicCm = ClayDensityGramsPerCubicCm;
		Spec.CoursesHigh = 1 + WallOpeningCourses + CoverCourses;
		Spec.Cells = OpeningCells + 2 * JambCells;
		Spec.Bond = DestructionWallCases::EWallBond::Running;
		Spec.Strength = Strength;

		DestructionWallCases::FWallLayout Laid;
		if (!DestructionWallCases::Build(Spec, Laid))
		{
			OutWhy = FString::Printf(
				TEXT("the wall producer refused a %d-course, %d-cell wall"), Spec.CoursesHigh, Spec.Cells);
			return false;
		}

		// Courses 1..WallOpeningCourses, cells JambCells..JambCells+OpeningCells-1.
		TArray<DestructionWallCases::FWallRegion> Cut;
		Cut.Add({ 1, WallOpeningCourses,
			double(JambCells) - 0.25,
			double(JambCells + OpeningCells - 1) + 0.25 });

		TArray<int32> CutPieces;
		DestructionWallCases::PiecesInRegions(Laid, Cut, CutPieces);

		if (CutPieces.Num() == 0)
		{
			OutWhy = TEXT("the opening cut named no bricks");
			return false;
		}

		for (const int32 Piece : CutPieces)
		{
			if (!Laid.Layout.Structure.RemovePiece(Piece))
			{
				OutWhy = FString::Printf(TEXT("could not remove opening brick %d"), Piece);
				return false;
			}
		}

		Out = MoveTemp(Laid.Layout.Structure);
		return true;
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

	double BlockMagnitude(const FOracleMechanismBlock& T)
	{
		return FMath::Abs(T.VirtualUx) + FMath::Abs(T.VirtualUz) + FMath::Abs(T.VirtualOmega);
	}

	double GravityVirtualWork(const FOracleProblem& P, const FOracleMechanism& M)
	{
		double Work = 0.0;
		const int32 N = FMath::Min(P.Blocks.Num(), M.Blocks.Num());
		for (int32 B = 0; B < N; ++B)
		{
			const double WeightUu = P.Blocks[B].MassKg * GravityCmPerSecondSquared;
			Work += (-WeightUu) * M.Blocks[B].VirtualUz;
		}
		return Work;
	}

	/** Relative virtual velocity at the joint's worst contact, v = u + omega x r, derived independently. */
	double JointRelativeVelocity(const FOracleProblem& P, const FOracleMechanism& M, int32 JointIndex)
	{
		const FOracleJoint& J = P.Joints[JointIndex];
		if (!M.Blocks.IsValidIndex(J.BlockA) || !M.Blocks.IsValidIndex(J.BlockB))
		{
			return 0.0;
		}

		const FOracleBlock& A = P.Blocks[J.BlockA];
		const FOracleBlock& B = P.Blocks[J.BlockB];
		const FOracleMechanismBlock& TA = M.Blocks[J.BlockA];
		const FOracleMechanismBlock& TB = M.Blocks[J.BlockB];

		const double TangentX = -J.NormalZ;
		const double TangentZ = J.NormalX;

		double Worst = 0.0;
		for (int32 End = 0; End < 2; ++End)
		{
			const double Sign = End == 0 ? -1.0 : 1.0;
			const double PointX = J.CentreXCm + Sign * J.HalfLengthCm * TangentX;
			const double PointZ = J.CentreZCm + Sign * J.HalfLengthCm * TangentZ;

			const double AVx = TA.VirtualUx - TA.VirtualOmega * (PointZ - A.CentroidZCm);
			const double AVz = TA.VirtualUz + TA.VirtualOmega * (PointX - A.CentroidXCm);
			const double BVx = TB.VirtualUx - TB.VirtualOmega * (PointZ - B.CentroidZCm);
			const double BVz = TB.VirtualUz + TB.VirtualOmega * (PointX - B.CentroidXCm);

			Worst = FMath::Max(Worst, FMath::Abs(BVx - AVx) + FMath::Abs(BVz - AVz));
		}
		return Worst;
	}

	// Independent re-canonicalization at an arbitrary tau.
	TSet<int32> NamedBlocksAtTau(const FOracleMechanism& M, double Tau)
	{
		double Largest = 0.0;
		for (int32 B = 0; B < M.Blocks.Num(); ++B)
		{
			Largest = FMath::Max(Largest, BlockMagnitude(M.Blocks[B]));
		}

		TSet<int32> Named;
		if (Largest > 0.0)
		{
			for (int32 B = 0; B < M.Blocks.Num(); ++B)
			{
				if (BlockMagnitude(M.Blocks[B]) > Tau * Largest)
				{
					Named.Add(B);
				}
			}
		}
		return Named;
	}

	TSet<int32> NamedJointsAtTau(const FOracleProblem& P, const FOracleMechanism& M, double Tau)
	{
		TArray<double> Rel;
		Rel.SetNumZeroed(P.Joints.Num());
		double Largest = 0.0;
		for (int32 J = 0; J < P.Joints.Num(); ++J)
		{
			Rel[J] = JointRelativeVelocity(P, M, J);
			Largest = FMath::Max(Largest, Rel[J]);
		}

		TSet<int32> Named;
		if (Largest > 0.0)
		{
			for (int32 J = 0; J < P.Joints.Num(); ++J)
			{
				if (Rel[J] > Tau * Largest)
				{
					Named.Add(J);
				}
			}
		}
		return Named;
	}

	/** Min relative magnitude among named blocks and max among un-named; they must straddle tau. */
	void BlockSeparation(const FOracleMechanism& M, double& OutMinNamed, double& OutMaxUnnamed)
	{
		double Largest = 0.0;
		for (int32 B = 0; B < M.Blocks.Num(); ++B)
		{
			Largest = FMath::Max(Largest, BlockMagnitude(M.Blocks[B]));
		}

		OutMinNamed = TNumericLimits<double>::Max();
		OutMaxUnnamed = 0.0;
		if (Largest <= 0.0)
		{
			OutMinNamed = 0.0;
			return;
		}

		for (int32 B = 0; B < M.Blocks.Num(); ++B)
		{
			const double Rel = BlockMagnitude(M.Blocks[B]) / Largest;
			if (M.Blocks[B].bMoves)
			{
				OutMinNamed = FMath::Min(OutMinNamed, Rel);
			}
			else
			{
				OutMaxUnnamed = FMath::Max(OutMaxUnnamed, Rel);
			}
		}
		if (OutMinNamed == TNumericLimits<double>::Max())
		{
			OutMinNamed = 0.0;
		}
	}

	void JointSeparation(
		const FOracleProblem& P, const FOracleMechanism& M, double& OutMinNamed, double& OutMaxUnnamed)
	{
		TArray<double> Rel;
		Rel.SetNumZeroed(P.Joints.Num());
		double Largest = 0.0;
		for (int32 J = 0; J < P.Joints.Num(); ++J)
		{
			Rel[J] = JointRelativeVelocity(P, M, J);
			Largest = FMath::Max(Largest, Rel[J]);
		}

		OutMinNamed = TNumericLimits<double>::Max();
		OutMaxUnnamed = 0.0;
		if (Largest <= 0.0)
		{
			OutMinNamed = 0.0;
			return;
		}

		for (int32 J = 0; J < P.Joints.Num(); ++J)
		{
			const double R = Rel[J] / Largest;
			if (JointOpens(M, J))
			{
				OutMinNamed = FMath::Min(OutMinNamed, R);
			}
			else
			{
				OutMaxUnnamed = FMath::Max(OutMaxUnnamed, R);
			}
		}
		if (OutMinNamed == TNumericLimits<double>::Max())
		{
			OutMinNamed = 0.0;
		}
	}

	// Seeded permutation of blocks and joints, remapping block references. NewIndex = Perm[OldIndex].
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

	FOracleProblem Permute(const FOracleProblem& In, const TArray<int32>& BlockPerm, const TArray<int32>& JointPerm)
	{
		FOracleProblem Out;
		Out.bGravityIsLive = In.bGravityIsLive;
		Out.bFirstCrackRows = In.bFirstCrackRows;

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

		for (const FOracleAppliedForce& F : In.AppliedForces)
		{
			FOracleAppliedForce G = F;
			G.Block = BlockPerm[F.Block];
			Out.AppliedForces.Add(G);
		}
		return Out;
	}

	/** Copy with every joint's tensile strength scaled. */
	FOracleProblem WithScaledTension(const FOracleProblem& In, double Factor)
	{
		FOracleProblem P = In;
		for (FOracleJoint& J : P.Joints)
		{
			J.Strength.TensileStrengthMPa *= Factor;
		}
		return P;
	}

	/**
	 * Per-permutation record, so a wobble can be told from a fail-closed refusal. The report also
	 * carries the worst raw-dual drift, showing whether the dual is genuinely non-unique.
	 */
	struct FPermRecord
	{
		int32 Seed = 0;
		bool bAnswered = false;
		int32 Outcome = 0;
		int32 Refusal = 0;
		bool bCertified = false;
		int32 MovingInBase = 0;
		int32 OpeningInBase = 0;
		bool bMovingSame = false;
		bool bOpeningSame = false;
	};

	struct FPermutationReport
	{
		bool bMovingStable = true;
		bool bOpeningStable = true;
		int32 BaseMovingNum = 0;
		int32 BaseOpeningNum = 0;

		// Worst |BaseUz - PermUz| over answered permutations only, so a refusal's empty mechanism cannot inflate it.
		double WorstRawDualDriftAnswered = 0.0;
		TArray<FPermRecord> Records;
	};

	FPermutationReport SweepPermutations(const FOracleProblem& Base, int32 BaseSeed, int32 NumPermutations)
	{
		FPermutationReport Rep;

		const FOracleResult BaseR = SolveRigidBlock(Base);
		const TSet<int32> BaseMoving = MovingBlocks(BaseR.Mechanism);
		const TSet<int32> BaseOpening = OpeningJoints(BaseR.Mechanism);
		Rep.BaseMovingNum = BaseMoving.Num();
		Rep.BaseOpeningNum = BaseOpening.Num();

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
			const bool bPermAnswered = PermR.bAnswered && PermR.Mechanism.bPresent;

			TSet<int32> PermMovingInBase;
			for (int32 Old = 0; Old < Base.Blocks.Num(); ++Old)
			{
				if (BlockMoves(PermR.Mechanism, BlockPerm[Old]))
				{
					PermMovingInBase.Add(Old);
				}
				if (bPermAnswered)
				{
					const double BaseUz = BaseR.Mechanism.Blocks.IsValidIndex(Old)
						? BaseR.Mechanism.Blocks[Old].VirtualUz : 0.0;
					const double PermUz = PermR.Mechanism.Blocks.IsValidIndex(BlockPerm[Old])
						? PermR.Mechanism.Blocks[BlockPerm[Old]].VirtualUz : 0.0;
					Rep.WorstRawDualDriftAnswered =
						FMath::Max(Rep.WorstRawDualDriftAnswered, FMath::Abs(BaseUz - PermUz));
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

			FPermRecord Rec;
			Rec.Seed = Seed;
			Rec.bAnswered = PermR.bAnswered;
			Rec.Outcome = static_cast<int32>(OutcomeOf(PermR));
			Rec.Refusal = static_cast<int32>(PermR.Refusal);
			Rec.bCertified = PermR.Mechanism.bIsCertified;
			Rec.MovingInBase = PermMovingInBase.Num();
			Rec.OpeningInBase = PermOpeningInBase.Num();
			Rec.bMovingSame =
				PermMovingInBase.Num() == BaseMoving.Num() && PermMovingInBase.Includes(BaseMoving);
			Rec.bOpeningSame =
				PermOpeningInBase.Num() == BaseOpening.Num() && PermOpeningInBase.Includes(BaseOpening);

			Rep.bMovingStable = Rep.bMovingStable && Rec.bMovingSame;
			Rep.bOpeningStable = Rep.bOpeningStable && Rec.bOpeningSame;
			Rep.Records.Add(Rec);
		}
		return Rep;
	}
}

/*
 * Test 1: two independent overhangs. Both bars must move and both joints open, identically under
 * permutation. Decoupled modes have unique duals, so this is the control for fixture F.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleMechanismTwoIndependentModesTest,
	"DestructionGame.Oracle.RigidBlock.Mechanism.NamesBothIndependentModes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleMechanismTwoIndependentModesTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace OracleMultiModeDeterminismSupport;

	FTwoIndependent Fx = BuildTwoIndependentOverhangs();
	const FOracleResult R = SolveRigidBlock(Fx.Problem);

	AddInfo(FString::Printf(
		TEXT("TWO-BODY: answered %d, lambda* %.6g, outcome %d (1=Falls), present %d, certified %d, "
			 "moving %d, opening %d, Bland %d, pivots %d"),
		R.bAnswered ? 1 : 0, R.Lambda, static_cast<int32>(OutcomeOf(R)),
		R.Mechanism.bPresent ? 1 : 0, R.Mechanism.bIsCertified ? 1 : 0,
		MovingBlocks(R.Mechanism).Num(), OpeningJoints(R.Mechanism).Num(),
		R.BlandDegenerateEntries, R.SimplexIterations));

	TestTrue(TEXT("TWO-BODY: the oracle answers"), R.bAnswered);
	TestEqual(TEXT("TWO-BODY: infeasible under dead gravity (Falls)"),
		static_cast<int32>(OutcomeOf(R)), static_cast<int32>(EOracleOutcome::Falls));
	TestTrue(TEXT("TWO-BODY: a mechanism is present and certified"),
		R.Mechanism.bPresent && R.Mechanism.bIsCertified);

	TestTrue(TEXT("TWO-BODY: body A (the left overhang) moves"), BlockMoves(R.Mechanism, Fx.BodyA));
	TestTrue(TEXT("TWO-BODY: body B (the right overhang) moves"), BlockMoves(R.Mechanism, Fx.BodyB));
	TestTrue(TEXT("TWO-BODY: joint A opens"), JointOpens(R.Mechanism, Fx.JointA));
	TestTrue(TEXT("TWO-BODY: joint B opens"), JointOpens(R.Mechanism, Fx.JointB));

	TestFalse(TEXT("TWO-BODY: grounded seat A does not move"), BlockMoves(R.Mechanism, Fx.SeatA));
	TestFalse(TEXT("TWO-BODY: grounded seat B does not move"), BlockMoves(R.Mechanism, Fx.SeatB));

	const double Work = GravityVirtualWork(Fx.Problem, R.Mechanism);
	TestTrue(*FString::Printf(TEXT("TWO-BODY: gravity does positive work on the mechanism, %.6g > 0"), Work),
		Work > 0.0);

	const FPermutationReport Rep = SweepPermutations(Fx.Problem, 0x02B0D1CE, 8);

	AddInfo(FString::Printf(
		TEXT("TWO-BODY PERMUTATION: base moving %d, opening %d; movingStable %d, openingStable %d; "
			 "worst answered raw-dual drift %.3e"),
		Rep.BaseMovingNum, Rep.BaseOpeningNum, Rep.bMovingStable ? 1 : 0, Rep.bOpeningStable ? 1 : 0,
		Rep.WorstRawDualDriftAnswered));

	TestTrue(TEXT("TWO-BODY: the base names both bodies (moving == 2)"), Rep.BaseMovingNum == 2);
	TestTrue(TEXT("TWO-BODY [GATE]: the canonical MOVING set is permutation-identical"), Rep.bMovingStable);
	TestTrue(TEXT("TWO-BODY [GATE]: the canonical OPENING set is permutation-identical"), Rep.bOpeningStable);

	return true;
}

/*
 * Test 2: determinism at scale on fixture F. The degeneracy witness is raw-dual drift, not the
 * Bland count: Bland entries are a phase-2 event, and the mechanism comes from phase 1, which
 * measured 0 Bland over ~5,800 pivots here. A drift near 1 shows the Farkas ray is non-unique.
 * With that established, the canonical moving and opening sets must match across permutations.
 * A red here is the D7 design gate firing, not a bug to patch.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleMechanismDeterminismAtScaleTest,
	"OracleSweepFull.RigidBlock.Mechanism.IsPermutationDeterministicAtScale",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleMechanismDeterminismAtScaleTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace OracleMultiModeDeterminismSupport;

	/*
	 * 14-cell opening, 3-cell jambs, 4 cover courses: ~120 blocks, ~260 joints. Dry, because a
	 * mortared wall of this shape still stands by thrust even at f_t x 0.08.
	 */
	FStructure Wall;
	FString BuildWhy;
	const bool bBuilt = BuildOpeningWall(
		/*OpeningCells*/ 14, /*JambCells*/ 3, /*CoverCourses*/ 4, DryStone, Wall, BuildWhy);
	if (!TestTrue(*FString::Printf(TEXT("SCALE: the dry opening wall builds (%s)"), *BuildWhy), bBuilt))
	{
		return false;
	}

	FOracleProblem Base;
	FString BridgeWhy;
	const bool bBridged = BuildRigidBlockProblem(Wall, Base, BridgeWhy);
	if (!TestTrue(*FString::Printf(TEXT("SCALE: the bridge accepts the wall (%s)"), *BridgeWhy), bBridged))
	{
		return false;
	}
	Base.bGravityIsLive = false; // feasibility formulation: gravity dead, phase 1 runs

	const FOracleResult BaseR = SolveRigidBlock(Base);

	AddInfo(FString::Printf(
		TEXT("SCALE: %d blocks, %d joints; answered %d, refusal %d (%s), outcome %d (1=Falls); "
			 "present %d, certified %d, moving %d, opening %d; Bland %d (phase-2 artifact, see header), "
			 "pivots %d"),
		Base.Blocks.Num(), Base.Joints.Num(), BaseR.bAnswered ? 1 : 0,
		static_cast<int32>(BaseR.Refusal), *BaseR.WhyNot, static_cast<int32>(OutcomeOf(BaseR)),
		BaseR.Mechanism.bPresent ? 1 : 0, BaseR.Mechanism.bIsCertified ? 1 : 0,
		MovingBlocks(BaseR.Mechanism).Num(), OpeningJoints(BaseR.Mechanism).Num(),
		BaseR.BlandDegenerateEntries, BaseR.SimplexIterations));

	TestTrue(TEXT("SCALE: the oracle answers the wall"), BaseR.bAnswered);
	TestEqual(TEXT("SCALE: the dry wide-opening wall is infeasible at self-weight (Falls)"),
		static_cast<int32>(OutcomeOf(BaseR)), static_cast<int32>(EOracleOutcome::Falls));
	TestTrue(TEXT("SCALE: an infeasible wall carries a certified mechanism"),
		BaseR.Mechanism.bPresent && BaseR.Mechanism.bIsCertified);
	TestTrue(TEXT("SCALE: the base mechanism names a NON-EMPTY moving set"),
		MovingBlocks(BaseR.Mechanism).Num() >= 1);

	const FPermutationReport Rep = SweepPermutations(Base, 0x0DEC0DE7, 6);

	/*
	 * Answered permutations must name the base's set (the D7 gate). A refusal names nothing and is
	 * fail-closed, so it is reported separately and never counted as a wobble.
	 */
	int32 NumAnswered = 0;
	int32 NumRefused = 0;
	bool bStableAmongAnswered = true;
	int32 FirstDivergentAnsweredSeed = INDEX_NONE;
	int32 FirstRefusedSeed = INDEX_NONE;

	for (const FPermRecord& Rec : Rep.Records)
	{
		const bool bRecAnswered = Rec.bAnswered && Rec.Outcome == static_cast<int32>(EOracleOutcome::Falls);
		if (bRecAnswered)
		{
			++NumAnswered;
			if (!(Rec.bMovingSame && Rec.bOpeningSame))
			{
				bStableAmongAnswered = false;
				if (FirstDivergentAnsweredSeed == INDEX_NONE)
				{
					FirstDivergentAnsweredSeed = Rec.Seed;
				}
			}
		}
		else
		{
			++NumRefused;
			if (FirstRefusedSeed == INDEX_NONE)
			{
				FirstRefusedSeed = Rec.Seed;
			}
		}
	}

	AddInfo(FString::Printf(
		TEXT("SCALE PERMUTATION: base moving %d, opening %d; %d answered / %d refused of %d; "
			 "stable-among-answered %d; worst answered raw-dual VirtualUz drift %.3e (>> 0 => the dual is "
			 "genuinely NON-UNIQUE among valid solves, so canonicalization is load-bearing)"),
		Rep.BaseMovingNum, Rep.BaseOpeningNum, NumAnswered, NumRefused, Rep.Records.Num(),
		bStableAmongAnswered ? 1 : 0, Rep.WorstRawDualDriftAnswered));

	for (const FPermRecord& Rec : Rep.Records)
	{
		AddInfo(FString::Printf(
			TEXT("SCALE PERM seed=%d: answered %d, outcome %d, refusal %d, certified %d, movingInBase %d "
				 "(same %d), openingInBase %d (same %d)"),
			Rec.Seed, Rec.bAnswered ? 1 : 0, Rec.Outcome, Rec.Refusal, Rec.bCertified ? 1 : 0,
			Rec.MovingInBase, Rec.bMovingSame ? 1 : 0, Rec.OpeningInBase, Rec.bOpeningSame ? 1 : 0));
	}

	// Regime gate: without real dual drift, a stable set proves nothing beyond the single-mode fixtures.
	TestTrue(
		*FString::Printf(
			TEXT("SCALE [REGIME GATE]: worst answered raw-dual VirtualUz drift = %.3e must be >> rounding "
				 "(>= 1e-3) so the fixture provably reaches the degenerate multi-ray regime; Bland = %d is a "
				 "phase-2 artifact and is 0 on this phase-1 arm (see header)"),
			Rep.WorstRawDualDriftAnswered, BaseR.BlandDegenerateEntries),
		Rep.WorstRawDualDriftAnswered >= 1.0e-3);

	// Stability over fewer than two answers would be vacuous.
	TestTrue(
		*FString::Printf(TEXT("SCALE: at least two permutations answered (got %d)"), NumAnswered),
		NumAnswered >= 2);

	// D7 gate: every answered permutation names the base's set, though the raw dual differs each time.
	TestTrue(
		*FString::Printf(
			TEXT("SCALE [GATE]: among ANSWERED permutations the canonical MOVING+OPENING set is identical "
				 "to the base (base moving %d, opening %d; answered %d; worst answered raw-dual drift %.3e). "
				 "First divergent answered seed %d. A red here is the D7 design gate firing, NOT a bug to patch"),
			Rep.BaseMovingNum, Rep.BaseOpeningNum, NumAnswered,
			Rep.WorstRawDualDriftAnswered, FirstDivergentAnsweredSeed),
		bStableAmongAnswered);

	/*
	 * Safety: a refusal is acceptable (the router absorbs it), a wrong answer is not. No permutation
	 * may flip to Stands; every answered one names the same certified non-empty set; every refusal
	 * names nothing.
	 */
	bool bNoVerdictFlip = true;
	bool bFailClosedClean = true;
	for (const FPermRecord& Rec : Rep.Records)
	{
		const bool bRecAnswered = Rec.bAnswered && Rec.Outcome == static_cast<int32>(EOracleOutcome::Falls);
		if (Rec.Outcome == static_cast<int32>(EOracleOutcome::Stands))
		{
			bNoVerdictFlip = false;
		}
		if (bRecAnswered)
		{
			bFailClosedClean = bFailClosedClean
				&& Rec.bCertified && Rec.bMovingSame && Rec.bOpeningSame && Rec.MovingInBase >= 1;
		}
		else
		{
			bFailClosedClean = bFailClosedClean && !Rec.bAnswered && Rec.MovingInBase == 0;
		}
	}

	TestTrue(TEXT("SCALE [SAFETY]: no permutation flips the verdict to Stands"), bNoVerdictFlip);
	TestTrue(
		TEXT("SCALE [SAFETY]: every answered permutation names the identical certified set; every "
			 "refusal names nothing (fail-closed) — the solver never names the WRONG bricks"),
		bFailClosedClean);

	/*
	 * Some orderings make phase 1 refuse. Reported, not asserted: production tolerates a fail-closed
	 * refusal, and a wrong answered set is caught by the safety pins above.
	 */
	AddInfo(FString::Printf(
		TEXT("SCALE [ROBUSTNESS FINDING]: %d of %d permutations fail-closed REFUSED (first seed %d, "
			 "PhaseOneFailure=2) rather than answering. Fail-closed, absorbed by the router — reported "
			 "for the orchestrator, not asserted red"),
		NumRefused, Rep.Records.Num(), FirstRefusedSeed));

	return true;
}

/*
 * Test 3: the named set does not hinge on tau = 1e-6. On the single overhang, two-body and wall
 * fixtures, the reimplemented canonicalization must match production at 1e-6, give the same sets
 * at 1e-5 and 1e-7, and separate named from un-named by >= 2 orders of magnitude around tau.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleMechanismTauRobustnessTest,
	"DestructionGame.Oracle.RigidBlock.Mechanism.TauSeparationIsWide",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleMechanismTauRobustnessTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace OracleMultiModeDeterminismSupport;

	struct FCase
	{
		FOracleProblem Problem;
		FString Label;
	};

	TArray<FCase> Cases;
	Cases.Add({ BuildSingleOverhang(), TEXT("single-overhang(control)") });
	Cases.Add({ BuildTwoIndependentOverhangs().Problem, TEXT("two-independent-bodies") });

	{
		FStructure Wall;
		FString BuildWhy;
		if (BuildOpeningWall(14, 3, 4, DryStone, Wall, BuildWhy))
		{
			FOracleProblem P;
			FString BridgeWhy;
			if (BuildRigidBlockProblem(Wall, P, BridgeWhy))
			{
				P.bGravityIsLive = false;
				Cases.Add({ MoveTemp(P), TEXT("dry-opening-wall(at-scale)") });
			}
			else
			{
				AddError(*FString::Printf(TEXT("TAU: the wall bridge refused (%s)"), *BridgeWhy));
			}
		}
		else
		{
			AddError(*FString::Printf(TEXT("TAU: the wall build refused (%s)"), *BuildWhy));
		}
	}

	constexpr double TauProd = 1.0e-6;

	for (const FCase& C : Cases)
	{
		const FOracleResult R = SolveRigidBlock(C.Problem);

		TestEqual(*FString::Printf(TEXT("TAU %s: infeasible (Falls)"), *C.Label),
			static_cast<int32>(OutcomeOf(R)), static_cast<int32>(EOracleOutcome::Falls));
		if (!R.Mechanism.bPresent)
		{
			AddError(*FString::Printf(TEXT("TAU %s: no mechanism to inspect"), *C.Label));
			continue;
		}

		// (i) The reimplementation at 1e-6 must reproduce production's bMoves set.
		const TSet<int32> MyBlocks6 = NamedBlocksAtTau(R.Mechanism, TauProd);
		const TSet<int32> ProdBlocks = MovingBlocks(R.Mechanism);
		const bool bReimplFaithful =
			MyBlocks6.Num() == ProdBlocks.Num() && MyBlocks6.Includes(ProdBlocks);
		TestTrue(
			*FString::Printf(
				TEXT("TAU %s: the independent tau=1e-6 canonicalization reproduces production's moving set "
					 "(mine %d, prod %d)"),
				*C.Label, MyBlocks6.Num(), ProdBlocks.Num()),
			bReimplFaithful);

		// (ii) Same sets across tau in {1e-5, 1e-6, 1e-7}.
		const TSet<int32> MyBlocks5 = NamedBlocksAtTau(R.Mechanism, 1.0e-5);
		const TSet<int32> MyBlocks7 = NamedBlocksAtTau(R.Mechanism, 1.0e-7);
		const bool bBlocks5Same = MyBlocks5.Num() == MyBlocks6.Num() && MyBlocks5.Includes(MyBlocks6);
		const bool bBlocks7Same = MyBlocks7.Num() == MyBlocks6.Num() && MyBlocks7.Includes(MyBlocks6);

		const TSet<int32> MyJoints5 = NamedJointsAtTau(C.Problem, R.Mechanism, 1.0e-5);
		const TSet<int32> MyJoints6 = NamedJointsAtTau(C.Problem, R.Mechanism, 1.0e-6);
		const TSet<int32> MyJoints7 = NamedJointsAtTau(C.Problem, R.Mechanism, 1.0e-7);
		const bool bJoints5Same = MyJoints5.Num() == MyJoints6.Num() && MyJoints5.Includes(MyJoints6);
		const bool bJoints7Same = MyJoints7.Num() == MyJoints6.Num() && MyJoints7.Includes(MyJoints6);

		// (iii) Separation around tau.
		double BMinNamed, BMaxUnnamed, JMinNamed, JMaxUnnamed;
		BlockSeparation(R.Mechanism, BMinNamed, BMaxUnnamed);
		JointSeparation(C.Problem, R.Mechanism, JMinNamed, JMaxUnnamed);

		AddInfo(FString::Printf(
			TEXT("TAU %s: blocks named@{1e-5,1e-6,1e-7} = {%d,%d,%d}; joints = {%d,%d,%d}; "
				 "BLOCK sep min-named %.3e vs max-unnamed %.3e; JOINT sep min-named %.3e vs max-unnamed %.3e; "
				 "tau=1e-6"),
			*C.Label, MyBlocks5.Num(), MyBlocks6.Num(), MyBlocks7.Num(),
			MyJoints5.Num(), MyJoints6.Num(), MyJoints7.Num(),
			BMinNamed, BMaxUnnamed, JMinNamed, JMaxUnnamed));

		TestTrue(*FString::Printf(TEXT("TAU %s: block set identical at 1e-5 vs 1e-6"), *C.Label), bBlocks5Same);
		TestTrue(*FString::Printf(TEXT("TAU %s: block set identical at 1e-7 vs 1e-6"), *C.Label), bBlocks7Same);
		TestTrue(*FString::Printf(TEXT("TAU %s: joint set identical at 1e-5 vs 1e-6"), *C.Label), bJoints5Same);
		TestTrue(*FString::Printf(TEXT("TAU %s: joint set identical at 1e-7 vs 1e-6"), *C.Label), bJoints7Same);

		TestTrue(
			*FString::Printf(
				TEXT("TAU %s: BLOCK min-named %.3e >= 100*tau (%.1e) — named blocks sit >= 2 orders above tau"),
				*C.Label, BMinNamed, 100.0 * TauProd),
			BMinNamed >= 100.0 * TauProd);
		TestTrue(
			*FString::Printf(
				TEXT("TAU %s: BLOCK max-unnamed %.3e <= tau/100 (%.1e) — excluded blocks sit >= 2 orders below tau"),
				*C.Label, BMaxUnnamed, TauProd / 100.0),
			BMaxUnnamed <= TauProd / 100.0);
		TestTrue(
			*FString::Printf(
				TEXT("TAU %s: JOINT min-named %.3e >= 100*tau (%.1e) — opening joints sit >= 2 orders above tau"),
				*C.Label, JMinNamed, 100.0 * TauProd),
			JMinNamed >= 100.0 * TauProd);
		TestTrue(
			*FString::Printf(
				TEXT("TAU %s: JOINT max-unnamed %.3e <= tau/100 (%.1e) — closed joints sit >= 2 orders below tau"),
				*C.Label, JMaxUnnamed, TauProd / 100.0),
			JMaxUnnamed <= TauProd / 100.0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
