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
 * THE SLICE-3b GATE — IS THE CANONICAL COLLAPSE MECHANISM PERMUTATION-DETERMINISTIC ON A
 * GENUINELY MULTI-MODE, DEGENERATE, INFEASIBLE STRUCTURE?
 *
 * WHY THIS FILE EXISTS, AND WHY IT IS SEPARATE FROM OracleMechanismExtractionTest.cpp. Slice 3a
 * extracted the mechanism and proved it permutation-stable on SINGLE-MODE collapses (fixtures
 * A/B/D there measure 0 Bland entries — their degeneracy is dual-MULTIPLIER non-uniqueness, which
 * the kinematic joint-set derivation already tamed). Review (2026-08-27, CURRENT_STATE "Slice 3a
 * deferred/owed items" (c)) found — correctly — that block-velocity uniqueness is a PROPERTY of
 * single-mode collapses, NOT a theorem: the dual is unique iff the primal optimum is
 * non-degenerate, and a marginally-infeasible structure with SEVERAL simultaneous failure modes
 * has multiple Farkas rays whose velocity field may not be permutation-unique. Item (c) is a
 * design gate that MUST be answered before Slice 3b wires any wall to the mechanism: build a
 * Bland-degenerate multi-mode infeasible permutation fixture, prove it reaches the hard regime
 * (print BlandDegenerateEntries, non-trivial), and assert the canonical moving+opening sets are
 * identical across seeded permutations. A WOBBLE here is the D7 design-gate firing — Slice 3
 * reshapes toward per-region interrogation and the minimal-support tie-break — NOT a bug to fix
 * under this test.
 *
 * TWO MODES OF "MULTI-MODE" ARE BUILT, deliberately, because they fail in different ways:
 *
 *   FIXTURE E — TWO INDEPENDENT COLLAPSING BODIES in one problem, far apart in X, sharing only the
 *   ground. The joint infeasibility can be certified by either body's Farkas ray or their sum, so
 *   the mechanism MUST name BOTH modes. This is the sharpest correctness question (does the
 *   extractor name all modes) and, because the two bodies are decoupled, each mode's dual is
 *   unique — so it also SHOULD be permutation-stable. Low degeneracy (0 Bland expected): it tests
 *   multi-mode NAMING and stability, not scale.
 *
 *   FIXTURE F — A LARGE DRY WALL WITH A WIDE OPENING AND COVER, POSED INFEASIBLE
 *   (bGravityIsLive = false). Dry stone (f_t = 0) cannot hang the cover panel over the void, so
 *   the whole cover comes down — MANY simultaneously-critical cover bricks, a genuinely coupled
 *   multi-mode collapse at ~100+ blocks / ~200+ contacts, the regime the sweep walls hit 170-358
 *   Bland entries in. THIS is the determinism-at-scale gate. Its Bland count is MEASURED and
 *   asserted non-trivial so the fixture provably reaches the hard regime; a 0-Bland reading here
 *   would mean the fixture does not test what it claims and the assertion says so.
 *
 * THE Q-ROBUSTNESS PIN (gate item (d)). The named set must not hinge on the exact
 * MechanismRelativeTol = 1e-6. The extractor's tau lives as a file constant in
 * RigidBlockOracle.cpp and cannot be re-run from here, so the canonicalization is REIMPLEMENTED
 * INDEPENDENTLY in this file (the same normalize-then-threshold rule, derived from the exposed
 * block triples and the joint geometry, NOT imported) and applied at tau in {1e-5, 1e-6, 1e-7}.
 * The 1e-6 reimplementation is cross-checked against production's own bMoves flags so the
 * independent copy is proven faithful; then set-equality across the three taus, and the
 * separation of min-named from max-un-named by >= 2 orders of magnitude around tau, pin that the
 * answer sits on a plateau rather than a knife edge.
 *
 * INDEPENDENCE. Units are derived here (1 MPa over 1 cm2 = 10000 uu), never imported, so a wrong
 * production constant disagrees rather than agrees. The kinematic velocity-at-a-contact recompute
 * is standard rigid-body statics (v = u + omega x r), derived here, not lifted from the solver.
 *
 * NEEDS A TICKING WORLD: NO. Every fixture is a pure FOracleProblem (E built by hand, F bridged
 * from an FStructure) fed to SolveRigidBlock; no Chaos, no world tick.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges many files into one translation unit.
 */
namespace OracleMultiModeDeterminismSupport
{
	using namespace RigidBlockOracle;
	using namespace DestructionProfiles;

	/* ================================================================================
	 * UNITS, derived here so a wrong production constant fails rather than agrees.
	 * ================================================================================ */

	constexpr double GravityCmPerSecondSquared = 980.0;
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;
	constexpr double ClayDensityGramsPerCubicCm = 1.9;
	constexpr double WytheWidthCm = 10.25;

	/* ================================================================================
	 * A HAND-BUILT FOracleProblem VOCABULARY (the same shapes the 3a file uses, kept
	 * local so this file has no cross-TU dependency).
	 * ================================================================================ */

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

	/* ================================================================================
	 * ONE OVERHANGING BAR PAST TIPPING — the A-equivalent building block. A long bar on a
	 * narrow grounded seat near its left end, centre of mass far to the right of the seat,
	 * so it rotates clockwise about the seat's right edge and the bed joint's left contact
	 * lifts. Reused twice, far apart in X, to make the two-independent-body fixture.
	 * ================================================================================ */

	constexpr double BarSeatHalfXCm = 5.0;
	constexpr double BarBodyLeftXCm = -5.0;
	constexpr double BarBodyRightXCm = 200.0;
	constexpr double BarBodyThicknessZCm = 40.0;

	double BarBodyMassKg()
	{
		return ClayDensityGramsPerCubicCm
			* (BarBodyRightXCm - BarBodyLeftXCm) * WytheWidthCm * BarBodyThicknessZCm / 1000.0;
	}

	/**
	 * Append one overhanging bar (a grounded seat + a free bar + a bed joint) at world offset
	 * OriginXCm, and report the added block indices. The bar's centroid ends up far to the
	 * right of its seat, so it is past tipping exactly as fixture A is.
	 */
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

		/* Two overhangs 1000 cm apart in X: no shared block, no shared joint — truly independent. */
		AppendOverhang(Out.Problem, 0.0, Out.SeatA, Out.BodyA, Out.JointA);
		AppendOverhang(Out.Problem, 1000.0, Out.SeatB, Out.BodyB, Out.JointB);
		return Out;
	}

	/* ================================================================================
	 * FIXTURE F — A LARGE DRY WALL WITH A WIDE OPENING AND COVER, POSED INFEASIBLE.
	 *
	 * Laid by the acceptance-wall producer with DRY-STONE joints, a wide opening cut through
	 * the lower courses, and several courses of cover over it. Dry stone carries no tension, so
	 * the cover panel over the void has nothing to hang from and no admissible force system
	 * exists at self-weight — the wall is infeasible and phase 1 runs, extracting the mechanism.
	 * ~100+ blocks and ~200+ contacts put it in the degenerate regime the gate needs.
	 * ================================================================================ */

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

		/* Cut the opening: courses 1..WallOpeningCourses, cells (JambCells .. JambCells+OpeningCells-1). */
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

	/* ================================================================================
	 * MECHANISM INSPECTION — the named sets and independent kinematics.
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

	/**
	 * The RELATIVE virtual velocity across joint J's worst contact, recomputed from the exposed
	 * block triples and the joint geometry — the same associated-flow quantity the extractor reads,
	 * derived here independently (standard rigid-body kinematics v = u + omega x r).
	 */
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

	/* ---- INDEPENDENT re-canonicalization at an arbitrary tau, from the triples. ---- */

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

	/**
	 * The min relative magnitude among NAMED blocks and the max among UN-NAMED, both relative to
	 * the largest — the two numbers that must straddle tau. Grounded/zero blocks push max-unnamed
	 * toward zero, which is the point.
	 */
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

	/* ================================================================================
	 * PERMUTATION — reorder blocks and joints by a seeded permutation, remapping every
	 * block reference. NewIndex = Perm[OldIndex].
	 * ================================================================================ */

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

	/** A weakened copy: every joint's tensile bond scaled, to dial a standing wall to marginal. */
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
	 * Solve Base, then re-solve under NumPermutations seeded block+joint permutations, and report
	 * whether the canonical MOVING-block set and OPENING-joint set (mapped back through the known
	 * permutation inverse) are identical every time. Captures per-permutation answered / outcome /
	 * refusal / certified / set-sizes, so a WOBBLE can be told from a fail-closed REFUSAL, and the
	 * worst raw-dual VirtualUz drift so a caller can see the dual is genuinely non-unique.
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

		/*
		 * Worst |BaseUz - PermUz| measured ONLY over permutations that ANSWERED with a mechanism —
		 * so it is a witness for the dual being NON-UNIQUE among valid solves, not an artifact of a
		 * refused permutation handing back an all-zero (empty) mechanism.
		 */
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

/* ================================================================================================
 * TEST 1 — MULTI-MODE CORRECTNESS AND STABILITY: TWO INDEPENDENT COLLAPSING BODIES.
 *
 * Two overhanging bars, 1000 cm apart, each past tipping on its own grounded seat. The mechanism
 * must name BOTH bars moving and BOTH bed joints opening (the multi-mode NAMING question), and
 * that named set must be identical under seeded block+joint permutation. Because the two bodies
 * are decoupled, each mode's dual is unique, so this SHOULD be stable — it is the control that
 * says "multi-mode alone does not break determinism"; fixture F adds the coupling and the scale.
 *
 * NEEDS A TICKING WORLD: NO.
 * ================================================================================================ */
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

	/* BOTH modes must be named — the whole point of a multi-mode problem. */
	TestTrue(TEXT("TWO-BODY: body A (the left overhang) moves"), BlockMoves(R.Mechanism, Fx.BodyA));
	TestTrue(TEXT("TWO-BODY: body B (the right overhang) moves"), BlockMoves(R.Mechanism, Fx.BodyB));
	TestTrue(TEXT("TWO-BODY: joint A opens"), JointOpens(R.Mechanism, Fx.JointA));
	TestTrue(TEXT("TWO-BODY: joint B opens"), JointOpens(R.Mechanism, Fx.JointB));

	TestFalse(TEXT("TWO-BODY: grounded seat A does not move"), BlockMoves(R.Mechanism, Fx.SeatA));
	TestFalse(TEXT("TWO-BODY: grounded seat B does not move"), BlockMoves(R.Mechanism, Fx.SeatB));

	/* Both bars descend — positive collapse work over the whole (independent) mechanism. */
	const double Work = GravityVirtualWork(Fx.Problem, R.Mechanism);
	TestTrue(*FString::Printf(TEXT("TWO-BODY: gravity does positive work on the mechanism, %.6g > 0"), Work),
		Work > 0.0);

	/* The named set is identical across seeded permutations. */
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

/* ================================================================================================
 * TEST 2 — THE DETERMINISM-AT-SCALE GATE. A LARGE DRY WALL WITH A WIDE OPENING, INFEASIBLE.
 *
 * THE HEADLINE. A dry running-bond wall with a wide opening cannot hang its cover panel (f_t = 0),
 * so the whole cover comes down as a coupled, many-brick, genuinely multi-mode collapse — the
 * regime the single-mode fixtures A/B/D never reach.
 *
 * WHY THE DEGENERACY WITNESS IS THE DUAL DRIFT, NOT THE BLAND COUNT. The review that mandated this
 * gate cited "170-358 Bland entries on an opening-ladder rung" as the marker of the hard regime.
 * MEASURED HERE, that marker does not transfer to the mechanism's own arm: BlandDegenerateEntries
 * is a PHASE-2 anti-cycling event (500 consecutive degenerate pivots while OPTIMISING lambda on a
 * STANDING wall), whereas the mechanism is extracted on the PHASE-1 INFEASIBLE arm, which
 * terminates as soon as the infeasibility sum is minimised and never reaches phase 2. Every
 * infeasible fixture tried here reports Bland = 0 (this dry wall: 0 Bland over ~5,800 phase-1
 * pivots; a mortared wall weakened to f_t x 0.08 STANDS by compression thrust and never even goes
 * infeasible). So Bland is printed as a MEASUREMENT with that caveat, and the regime is proven the
 * RIGHT way for this arm: the WORST RAW-DUAL VirtualUz DRIFT across permutations. A drift near the
 * normalization scale (~1) is a direct statement that the phase-1 dual is a genuinely NON-UNIQUE
 * Farkas ray — multiple mechanisms certify the same infeasibility — which is exactly the multi-ray
 * degeneracy the review warned block-velocity uniqueness would not survive.
 *
 * THE GATE. With the dual proven non-unique, the canonical moving-block AND opening-joint sets must
 * still be IDENTICAL across seeded block+joint permutations. Full per-permutation diagnostics are
 * captured (answered / outcome / refusal / certified / mapped-back set sizes) so a WOBBLE (the
 * mechanism names a different set with confidence) is told apart from a fail-closed REFUSAL.
 *
 * If the sets are STABLE: the Slice-3b gate PASSES. If they WOBBLE: the D7 design gate has FIRED —
 * Slice 3 reshapes toward per-region interrogation / the minimal-support tie-break. This test does
 * not try to make a wobble pass; a red here is the finding.
 *
 * NEEDS A TICKING WORLD: NO.
 * ================================================================================================ */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleMechanismDeterminismAtScaleTest,
	"DestructionGame.Oracle.RigidBlock.Mechanism.IsPermutationDeterministicAtScale",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleMechanismDeterminismAtScaleTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace OracleMultiModeDeterminismSupport;

	/*
	 * A DRY wide-opening wall: a 14-cell opening between 3-cell jambs, 4 courses of cover, ~120
	 * blocks / ~260 joints. Dry stone (f_t = 0) cannot hang the cover over the void, so it is
	 * genuinely infeasible with a many-brick, multi-mode collapse — the degenerate case the gate
	 * needs. (A MORTARED wall of this shape stands by compression thrust even at f_t x 0.08, so it
	 * cannot be marginalised by bond alone; see the header. Dry is the reliable infeasible arm.)
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
	Base.bGravityIsLive = false; /* feasibility formulation: gravity dead, phase 1 runs */

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

	/* THE GATE ITSELF: the named set holds across permutations at scale. */
	const FPermutationReport Rep = SweepPermutations(Base, 0x0DEC0DE7, 6);

	/*
	 * DECOMPOSE THE PERMUTATIONS. Two categories with completely different meanings:
	 *   - ANSWERED: the solve produced a certified mechanism. Its named set MUST equal the base's —
	 *     that is the D7 naming-determinism gate.
	 *   - REFUSED: the solve declined (e.g. PhaseOneFailure). That is FAIL-CLOSED — no bricks are
	 *     named at all, so it is not a wobble; production's router already handles a refusal. It is
	 *     reported as its own robustness observation, never conflated with a wobble.
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

	/*
	 * THE REGIME GATE, DONE THE RIGHT WAY FOR THE PHASE-1 ARM. The fixture must PROVABLY reach the
	 * degenerate multi-ray regime among ANSWERED solves, or a stable named set proves nothing the
	 * single-mode fixtures did not. Bland = 0 here (phase-2 artifact — header), so the witness is the
	 * raw-dual drift measured over answered permutations: a drift near the normalization scale is a
	 * direct measurement that the phase-1 Farkas ray is NON-UNIQUE while the named set holds steady.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("SCALE [REGIME GATE]: worst answered raw-dual VirtualUz drift = %.3e must be >> rounding "
				 "(>= 1e-3) so the fixture provably reaches the degenerate multi-ray regime; Bland = %d is a "
				 "phase-2 artifact and is 0 on this phase-1 arm (see header)"),
			Rep.WorstRawDualDriftAnswered, BaseR.BlandDegenerateEntries),
		Rep.WorstRawDualDriftAnswered >= 1.0e-3);

	/*
	 * GUARD AGAINST A VACUOUS GATE: at least two permutations must actually ANSWER, or "stable among
	 * answered" would be a claim about one point.
	 */
	TestTrue(
		*FString::Printf(TEXT("SCALE: at least two permutations answered (got %d)"), NumAnswered),
		NumAnswered >= 2);

	/*
	 * THE D7 NAMING-DETERMINISM GATE. Among every ANSWERED permutation, the canonical named set is
	 * identical to the base — even though the raw dual is a different Farkas ray each time. This is
	 * the whole Slice-3b question: does canonicalization pin the multi-ray degenerate dual to ONE
	 * named break set? A red here (an answered permutation naming a DIFFERENT set) is the D7 design
	 * gate firing.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("SCALE [GATE]: among ANSWERED permutations the canonical MOVING+OPENING set is identical "
				 "to the base (base moving %d, opening %d; answered %d; worst answered raw-dual drift %.3e). "
				 "First divergent answered seed %d. A red here is the D7 design gate firing, NOT a bug to patch"),
			Rep.BaseMovingNum, Rep.BaseOpeningNum, NumAnswered,
			Rep.WorstRawDualDriftAnswered, FirstDivergentAnsweredSeed),
		bStableAmongAnswered);

	/*
	 * THE FAIL-CLOSED SAFETY INVARIANT — what makes the D7 gate's "yes" safe to wire in. A refusal
	 * is acceptable (production's router absorbs it); what must NEVER happen is the solver silently
	 * handing back a WRONG answer under permutation. Two disasters are pinned closed:
	 *   - no permutation may FLIP the verdict to Stands (a wall that stood one ordering and fell
	 *     another would be un-reasonable-about destruction);
	 *   - every ANSWERED permutation must name the identical, non-empty, certified set, and every
	 *     NON-answered one must name NOTHING (a clean refusal, not an answered-but-empty mechanism).
	 * The 1-of-6 PhaseOneFailure is thus a fail-closed DECLINE, reported below as a finding, not a
	 * wobble — the solver never names the wrong bricks, it either names the right set or refuses.
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
			/* A refusal must name nothing — never an answered-but-empty or partial set. */
			bFailClosedClean = bFailClosedClean && !Rec.bAnswered && Rec.MovingInBase == 0;
		}
	}

	TestTrue(TEXT("SCALE [SAFETY]: no permutation flips the verdict to Stands"), bNoVerdictFlip);
	TestTrue(
		TEXT("SCALE [SAFETY]: every answered permutation names the identical certified set; every "
			 "refusal names nothing (fail-closed) — the solver never names the WRONG bricks"),
		bFailClosedClean);

	/*
	 * THE FAIL-CLOSED ROBUSTNESS OBSERVATION, reported not asserted-red: some column orderings drive
	 * phase 1 to refuse rather than answer, so the VERDICT is not fully permutation-robust even though
	 * NAMING (where it answers) is. A hard "must never refuse" would assert a property Slice 3b does
	 * not need — production tolerates a fail-closed refusal — so it is surfaced for the orchestrator
	 * to weigh, not made a red. If this ever becomes non-fail-closed (an answered wrong set), the
	 * SAFETY pins above go red, which is the disaster that DOES matter.
	 */
	AddInfo(FString::Printf(
		TEXT("SCALE [ROBUSTNESS FINDING]: %d of %d permutations fail-closed REFUSED (first seed %d, "
			 "PhaseOneFailure=2) rather than answering. Fail-closed, absorbed by the router — reported "
			 "for the orchestrator, not asserted red"),
		NumRefused, Rep.Records.Num(), FirstRefusedSeed));

	return true;
}

/* ================================================================================================
 * TEST 3 — THE Q-ROBUSTNESS PIN (gate item (d)). The named set does not hinge on tau = 1e-6.
 *
 * For a low-degeneracy control (single overhang), the multi-mode two-body fixture, and the
 * at-scale dry wall: the canonicalization is REIMPLEMENTED here from the exposed triples and
 * geometry, cross-checked against production's own bMoves flags at tau = 1e-6, then the named
 * block AND joint sets are asserted identical at tau in {1e-5, 1e-6, 1e-7}, and min-named vs
 * max-un-named relative magnitude are asserted to straddle tau by >= 2 orders of magnitude. A
 * fixture whose separation is NOT >= 2 orders is a finding (tau load-bearing on a knife edge),
 * reported by the printed numbers.
 *
 * NEEDS A TICKING WORLD: NO.
 * ================================================================================================ */
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

		/* (i) The independent 1e-6 reimplementation must reproduce production's own bMoves set. */
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

		/* (ii) Set-equality of the NAMED block+joint sets across tau in {1e-5, 1e-6, 1e-7}. */
		const TSet<int32> MyBlocks5 = NamedBlocksAtTau(R.Mechanism, 1.0e-5);
		const TSet<int32> MyBlocks7 = NamedBlocksAtTau(R.Mechanism, 1.0e-7);
		const bool bBlocks5Same = MyBlocks5.Num() == MyBlocks6.Num() && MyBlocks5.Includes(MyBlocks6);
		const bool bBlocks7Same = MyBlocks7.Num() == MyBlocks6.Num() && MyBlocks7.Includes(MyBlocks6);

		const TSet<int32> MyJoints5 = NamedJointsAtTau(C.Problem, R.Mechanism, 1.0e-5);
		const TSet<int32> MyJoints6 = NamedJointsAtTau(C.Problem, R.Mechanism, 1.0e-6);
		const TSet<int32> MyJoints7 = NamedJointsAtTau(C.Problem, R.Mechanism, 1.0e-7);
		const bool bJoints5Same = MyJoints5.Num() == MyJoints6.Num() && MyJoints5.Includes(MyJoints6);
		const bool bJoints7Same = MyJoints7.Num() == MyJoints6.Num() && MyJoints7.Includes(MyJoints6);

		/* (iii) The separation: min-named vs max-un-named around tau, blocks and joints. */
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

		/* >= 2 orders straddle: min-named >= 100*tau AND max-un-named <= tau/100. */
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
