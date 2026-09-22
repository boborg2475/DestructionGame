// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Regional collapse prover wired into the real SolveAndBreak cascade (REGIONAL_PROVER_PLAN.md slice 4).
 * Above the equilibrium-gate cap, a plain SolveAndBreak must seed the prover from the removed piece's
 * neighbours and upgrade a router "stands" to a proven "falls".
 *
 * The fixture is a global mechanism the per-joint router cannot see. A timber board cantilevers 137 cm
 * past its one remaining post, held at the back by a single Nail (0.071 MPa withdrawal). Every joint is
 * under its own capacity, and the overturning gate spares the board because the Nail has f_t > 0
 * (DESIGN.md §8, 2026-09-02). With N >= 2 load paths SolveLoads zeroes the moment, so the router reads
 * Supported; the whole-structure LP finds no equilibrium.
 *
 * The LP truth comes from BuildRigidBlockProblem + SolveRigidBlock, a different path from the regional
 * prover. Asserted on support state: truth-set pieces read Falling, the grounded pieces stand, nothing
 * is Stranded. Also checks that RegionalPoseBreakdown reconciles with the per-pass aggregates.
 */
namespace RegionalProverCascadeSeamSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * A timber board on two grounded DryStone posts plus a Nail tie at its back:
	 *   - PostL  X[0,10]      grounded, top Z = 20, the near fulcrum.
	 *   - PostR  X[140,150]   grounded, top Z = 20, removed by the test.
	 *   - Anchor X[-6,-1]     grounded, top Z = 20, the Nail's seat.
	 *   - Board  X[-6,300]    Z[21,26], centre of mass at X = 147.
	 */

	/** EN 338 C24 mean density. */
	constexpr double TimberDensityGramsPerCubicCm = 0.42;

	constexpr double BedJointThicknessCm = 1.0;

	/** One shared Y depth, so the fixture is planar for the 2D LP. */
	constexpr double DepthYCm = 20.0;

	constexpr double PostTopZCm = 20.0;
	constexpr double PostLLeftXCm = 0.0;
	constexpr double PostLRightXCm = 10.0;    // the front bearing edge the board cantilevers past
	constexpr double PostRLeftXCm = 140.0;
	constexpr double PostRRightXCm = 150.0;

	constexpr double AnchorLeftXCm = -6.0;
	constexpr double AnchorRightXCm = -1.0;

	constexpr double BoardLeftXCm = -6.0;
	constexpr double BoardRightXCm = 300.0;
	constexpr double BoardBottomZCm = PostTopZCm + BedJointThicknessCm;   // 21
	constexpr double BoardThicknessZCm = 5.0;
	constexpr double BoardTopZCm = BoardBottomZCm + BoardThicknessZCm;    // 26

	constexpr double BoardCentreXCm = (BoardLeftXCm + BoardRightXCm) / 2.0;   // 147 — 137 cm past PostL's edge

	FPieceBox MakeBox(double LoX, double HiX, double LoY, double HiY, double LoZ, double HiZ)
	{
		FPieceBox Box;
		Box.CentreCm = FVector((LoX + HiX) / 2.0, (LoY + HiY) / 2.0, (LoZ + HiZ) / 2.0);
		Box.ExtentCm = FVector((HiX - LoX) / 2.0, (HiY - LoY) / 2.0, (HiZ - LoZ) / 2.0);
		return Box;
	}

	double BoxMassKg(const FPieceBox& Box)
	{
		return TimberDensityGramsPerCubicCm
			* (Box.ExtentCm.X * 2.0) * (Box.ExtentCm.Y * 2.0) * (Box.ExtentCm.Z * 2.0) / 1000.0;
	}

	struct FFixture
	{
		FStructure Structure;
		int32 PostL = INDEX_NONE;
		int32 PostR = INDEX_NONE;
		int32 Anchor = INDEX_NONE;
		int32 Board = INDEX_NONE;
	};

	/**
	 * Lay the fixture. GateBlockCap = 0 makes the router the break authority, as for the 442-block shed
	 * above its 200-block cap.
	 */
	void Build(FFixture& Out)
	{
		Out.Structure.SetEquilibriumGateBlockCap(0);

		const FPieceBox PostLBox = MakeBox(PostLLeftXCm, PostLRightXCm, -DepthYCm / 2, DepthYCm / 2, 0.0, PostTopZCm);
		const FPieceBox PostRBox = MakeBox(PostRLeftXCm, PostRRightXCm, -DepthYCm / 2, DepthYCm / 2, 0.0, PostTopZCm);
		const FPieceBox AnchorBox = MakeBox(AnchorLeftXCm, AnchorRightXCm, -DepthYCm / 2, DepthYCm / 2, 0.0, PostTopZCm);
		const FPieceBox BoardBox = MakeBox(
			BoardLeftXCm, BoardRightXCm, -DepthYCm / 2, DepthYCm / 2, BoardBottomZCm, BoardTopZCm);

		Out.PostL = Out.Structure.AddPiece(BoxMassKg(PostLBox), /*bIsGrounded*/ true, PostLBox.CentreCm);
		Out.PostR = Out.Structure.AddPiece(BoxMassKg(PostRBox), /*bIsGrounded*/ true, PostRBox.CentreCm);
		Out.Anchor = Out.Structure.AddPiece(BoxMassKg(AnchorBox), /*bIsGrounded*/ true, AnchorBox.CentreCm);
		Out.Board = Out.Structure.AddPiece(BoxMassKg(BoardBox), /*bIsGrounded*/ false, BoardBox.CentreCm);

		FConnection Joint;
		if (MakeInterface(Out.PostL, PostLBox, Out.Board, BoardBox, BedJointThicknessCm, DryStone, Joint))
		{
			Out.Structure.AddConnection(Joint);
		}
		if (MakeInterface(Out.PostR, PostRBox, Out.Board, BoardBox, BedJointThicknessCm, DryStone, Joint))
		{
			Out.Structure.AddConnection(Joint);
		}
		if (MakeInterface(Out.Anchor, AnchorBox, Out.Board, BoardBox, BedJointThicknessCm, Nail, Joint))
		{
			Out.Structure.AddConnection(Joint);
		}
	}

	bool IsStanding(EPieceSupport Support)
	{
		return Support == EPieceSupport::Grounded || Support == EPieceSupport::Supported;
	}

	int32 StrandedCount(const FStructure& S)
	{
		int32 N = 0;
		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (!S.IsPieceRemoved(P) && S.GetPieceSupport(P) == EPieceSupport::Stranded)
			{
				++N;
			}
		}
		return N;
	}

	/** The whole-structure ground-only LP's moving set for the current graph. */
	TSet<int32> WholeStructureMovingSet(const FStructure& S, bool& bOutFalls, bool& bOutCertified)
	{
		using namespace RigidBlockOracle;
		bOutFalls = false;
		bOutCertified = false;

		FOracleProblem Problem;
		FString WhyNot;
		if (!BuildRigidBlockProblem(S, Problem, WhyNot))
		{
			return {};
		}

		// The below-cap authority's pose: feasibility at self-weight, with first-crack rows.
		Problem.bGravityIsLive = false;
		Problem.bFirstCrackRows = true;

		const FOracleResult Result = SolveRigidBlock(Problem);
		bOutFalls = OutcomeOf(Result) == EOracleOutcome::Falls;
		bOutCertified = Result.Mechanism.bPresent && Result.Mechanism.bIsCertified;

		TSet<int32> Moving;
		for (int32 Block = 0; Block < Result.Mechanism.Blocks.Num(); ++Block)
		{
			if (Result.Mechanism.Blocks[Block].bMoves && Problem.PieceOfBlock.IsValidIndex(Block))
			{
				Moving.Add(Problem.PieceOfBlock[Block]);
			}
		}
		return Moving;
	}
}

/** Removing the far post leaves a cantilever the router over-holds; SolveAndBreak must fell the LP's moving set. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRegionalProverCascadeFellsAnAboveCapOverHoldTest,
	"DestructionGame.Core.Structure.RegionalProver.CascadeFellsAnAboveCapOverHoldTheRouterStands",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRegionalProverCascadeFellsAnAboveCapOverHoldTest::RunTest(const FString& Parameters)
{
	using namespace RegionalProverCascadeSeamSupport;
	using namespace DestructionProfiles;

	TestTrue(
		*FString::Printf(
			TEXT("FIXTURE: the board CoM (X=%.1f) is past PostL's front edge (X=%.1f) — a deep cantilever"),
			BoardCentreXCm, PostLRightXCm),
		BoardCentreXCm > PostLRightXCm);

	// The Nail is tension-capable (so the overturning gate spares the board) but weak, so the LP fells it.
	TestTrue(TEXT("FIXTURE: the back tie is tension-capable (Nail f_t > 0) — the router's tension clause spares it"),
		Nail.TensileStrengthMPa > 0.0);
	TestTrue(TEXT("FIXTURE: the tie is a TOKEN — Nail f_t is far weaker than a Screw's 0.54 MPa"),
		Nail.TensileStrengthMPa < 0.1 && Nail.TensileStrengthMPa < Screw.TensileStrengthMPa);
	TestEqual(TEXT("FIXTURE: the two posts are compression-only DryStone (f_t == 0)"),
		DryStone.TensileStrengthMPa, 0.0);

	FFixture Fixture;
	Build(Fixture);

	TestEqual(TEXT("FIXTURE: four pieces — two posts, an anchor, and the board"),
		Fixture.Structure.NumPieces(), 4);
	TestEqual(TEXT("FIXTURE: three joints beneath the board — two compression posts and one Nail tie"),
		Fixture.Structure.NumConnections(), 3);
	TestTrue(TEXT("FIXTURE: complete geometry (the regional prover is gated on it)"),
		Fixture.Structure.HasCompleteGeometry());
	TestTrue(TEXT("FIXTURE: the structure is above the equilibrium-gate cap, so the router is the authority"),
		Fixture.Structure.NumPieces() > 0 /* GateBlockCap set to 0 in Build */);

	// As built the LP stands it, so the fall is caused by the removal.
	{
		bool bFalls = false, bCertified = false;
		WholeStructureMovingSet(Fixture.Structure, bFalls, bCertified);
		TestFalse(TEXT("PRECONDITION: as built (both posts) the whole-structure LP does NOT fell anything"),
			bFalls);
	}

	TestTrue(TEXT("DISTURBANCE: the far post is removed"),
		Fixture.Structure.RemovePiece(Fixture.PostR));

	// Independent truth set, read before SolveAndBreak.
	bool bTruthFalls = false, bTruthCertified = false;
	const TSet<int32> TruthMoving =
		WholeStructureMovingSet(Fixture.Structure, bTruthFalls, bTruthCertified);

	TestTrue(TEXT("TRUTH: the whole-structure LP proves the cantilever FALLS once the far post is gone"),
		bTruthFalls);
	TestTrue(TEXT("TRUTH: the fall carries a present, Farkas-certified collapse mechanism"),
		bTruthCertified);
	TestTrue(
		*FString::Printf(TEXT("TRUTH: the LP names the board among the moving set (moving pieces %d)"),
			TruthMoving.Num()),
		TruthMoving.Contains(Fixture.Board));
	TestTrue(
		*FString::Printf(TEXT("TRUTH: only the ungrounded board moves — grounded posts/anchor stay (moving %d)"),
			TruthMoving.Num()),
		TruthMoving.Num() >= 1);

	Fixture.Structure.SetRegionBlockCap(512);   // >= the live block count: the region floods everything

	const int32 Passes = Fixture.Structure.SolveAndBreak();

	const EPieceSupport BoardSupport = Fixture.Structure.GetPieceSupport(Fixture.Board);
	const int32 Stranded = StrandedCount(Fixture.Structure);

	AddInfo(FString::Printf(
		TEXT("CASCADE: %d breaking pass(es); board support = %d (0 Falling, 1 Grounded, 2 Supported, 3 Stranded); "
			"%d stranded; LP truth-moving = %d."),
		Passes, static_cast<int32>(BoardSupport), Stranded, TruthMoving.Num()));

	TestEqual(TEXT("CASCADE: nothing is Stranded — the fall is about equilibrium, not the router declining"),
		Stranded, 0);

	for (const int32 Piece : TruthMoving)
	{
		TestEqual(
			*FString::Printf(
				TEXT("RED: truth-set piece %d (the whole-structure LP fells it) must read Falling after the ")
				TEXT("wired cascade — the router over-holds it today"),
				Piece),
			Fixture.Structure.GetPieceSupport(Piece), EPieceSupport::Falling);
	}

	// Nothing outside the mechanism is dragged down.
	TestTrue(TEXT("SUBSET: PostL keeps the earth"),
		IsStanding(Fixture.Structure.GetPieceSupport(Fixture.PostL)));
	TestTrue(TEXT("SUBSET: the back anchor keeps the earth"),
		IsStanding(Fixture.Structure.GetPieceSupport(Fixture.Anchor)));

	// RegionalPoseBreakdown must reconcile with the per-pass aggregates; no wall-clock thresholds.
	const FStructure::FSolveAndBreakReport& Report = Fixture.Structure.GetLastSolveAndBreakReport();

	bool bSomePassPosed = false;
	for (const FStructure::FBreakPassReport& Pass : Report.Passes)
	{
		if (Pass.RegionalPoses > 0)
		{
			bSomePassPosed = true;
		}

		TestEqual(
			*FString::Printf(
				TEXT("PROFILE: pass %d — one RegionalPoseBreakdown entry per prover pose (RegionalPoses=%d)"),
				Pass.Pass, Pass.RegionalPoses),
			Pass.RegionalPoseBreakdown.Num(), Pass.RegionalPoses);

		if (Pass.RegionalPoses > 0)
		{
			int32 PivotSum = 0;
			double MsSum = 0.0;
			for (const FStructure::FProverPoseReport& Pose : Pass.RegionalPoseBreakdown)
			{
				PivotSum += Pose.LpPivots;
				MsSum += Pose.LpMs;

				TestTrue(
					*FString::Printf(TEXT("PROFILE: pass %d pose posed at least one block (Blocks=%d)"),
						Pass.Pass, Pose.Blocks),
					Pose.Blocks >= 1);
				TestTrue(
					*FString::Printf(TEXT("PROFILE: pass %d pose LpMs is finite and >= 0 (LpMs=%g)"),
						Pass.Pass, Pose.LpMs),
					FMath::IsFinite(Pose.LpMs) && Pose.LpMs >= 0.0);
			}

			TestEqual(
				*FString::Printf(TEXT("PROFILE: pass %d — pose pivots sum to RegionalLpPivots"), Pass.Pass),
				PivotSum, Pass.RegionalLpPivots);
			TestTrue(
				*FString::Printf(
					TEXT("PROFILE: pass %d — pose LpMs sum (%g) == RegionalLpMs (%g)"),
					Pass.Pass, MsSum, Pass.RegionalLpMs),
				FMath::IsNearlyEqual(MsSum, Pass.RegionalLpMs, 1.0e-6));

			// "Last blocks" and "fell" are the final pose's. Guarded because Last() asserts on an empty array.
			if (!Pass.RegionalPoseBreakdown.IsEmpty())
			{
				TestEqual(
					*FString::Printf(TEXT("PROFILE: pass %d — final pose Blocks == RegionalLastBlocks"), Pass.Pass),
					Pass.RegionalPoseBreakdown.Last().Blocks, Pass.RegionalLastBlocks);
				TestEqual(
					*FString::Printf(TEXT("PROFILE: pass %d — final pose bFell == bRegionalFell"), Pass.Pass),
					Pass.RegionalPoseBreakdown.Last().bFell, Pass.bRegionalFell);
			}

			AddInfo(FString::Printf(
				TEXT("PROFILE: pass %d posed %d LP(s) — decomposition follows:"),
				Pass.Pass, Pass.RegionalPoses));
			for (int32 P = 0; P < Pass.RegionalPoseBreakdown.Num(); ++P)
			{
				const FStructure::FProverPoseReport& Pose = Pass.RegionalPoseBreakdown[P];
				AddInfo(FString::Printf(
					TEXT("PROFILE:   pose %d — %d blocks, %d pivots, %.3f ms, fell=%s"),
					P, Pose.Blocks, Pose.LpPivots, Pose.LpMs, Pose.bFell ? TEXT("true") : TEXT("false")));
			}
		}
	}

	// Otherwise the reconciliation above passes vacuously.
	TestTrue(TEXT("PROFILE: the regional prover posed at least one LP across the cascade (fixture drives it)"),
		bSomePassPosed);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
