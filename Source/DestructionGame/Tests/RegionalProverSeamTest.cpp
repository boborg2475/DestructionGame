// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Regional collapse prover, slice 1 (REGIONAL_PROVER_PLAN.md, review item 12) — the seam. The
 * thinnest end-to-end proof that region extraction -> grounded-boundary pose -> SolveRigidBlock ->
 * Falling-only stitch works, on a fixture the whole-structure ground-only LP demonstrably fells.
 * Grown to cover the entire structure so its grounded boundary is the earth alone (no cut), the
 * regional prover must release exactly the whole-structure LP's moving set.
 *
 * The fixture: a 30-course, 10 cm/course mortared leaning stack (the row-3 FALLS rung of
 * LeaningStackAcceptanceTest). The lean has no admissible equilibrium and the column above the base
 * topples. At 30 blocks it is below the 200-block cap, so the rigid-block LP is a genuine authority,
 * and the whole-structure ground-only LP (bGravityIsLive = false, bFirstCrackRows = true) certifies
 * the fall and names the moving blocks. That set is the independent truth, derived through a
 * different code path than the region prover, so the test has teeth rather than mirroring itself.
 *
 * Assert on mechanism, never displacement: every truth-set piece reads Falling, the released count
 * equals the truth size, and nothing reads Stranded. A severed lean can rest in place and still be
 * released, so displacement is the wrong witness.
 *
 * Now green: it drove the slice-1 build (SolveAndBreak_WithRegionalProver, commit 2fd6e1b) and now
 * guards the no-cut (cap >= structure size) case. The cutting case is pinned by
 * RegionalProverGroundedCutTest (slice 2).
 *
 * The production surface this specifies (for dev-expert):
 *   - int32 FStructure::SolveAndBreak_WithRegionalProver(const TArray<int32>& Seed, int32
 *     RegionBlockCap): run the router baseline, then union a Falling-only override — flood a region
 *     from Seed by joint-hops up to RegionBlockCap, pin the one-hop frontier ring grounded, pose
 *     R + boundary (bGravityIsLive = false, bFirstCrackRows = true) through the grounded-boundary
 *     bridge, SolveRigidBlock, and on a certified mechanism mark the moved interior pieces Falling
 *     and sever the joints it opens. Returns pieces released. Cap >= block count floods everything.
 *   - bool RigidBlockOracle::BuildRegionalProblem(...): the grounded-boundary bridge overload —
 *     interior pieces bridged normally, boundary pieces forced bGrounded, only R + B included,
 *     two-grounded-end joints skipped. Kept separate from BuildRigidBlockProblem so shared poses do
 *     not shift and OracleSweepFull stays byte-identical. Named here as spec, not called directly.
 *
 * No ticking world: pure FOracleProblem solves and FStructure state queries. Units derived here,
 * never imported. Named namespace, not anonymous (a unity build merges files).
 */
namespace RegionalProverSeamSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	// The row-3 leaning stack. Centimetres at 1 uu = 1 cm; nothing imported.

	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickWidthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;

	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** Just under half a brick per course — a lean the mortar bond genuinely cannot hold at height. */
	constexpr double OffsetPerCourseCm = 10.0;

	constexpr double BedJointThicknessCm = 1.0;
	constexpr double CoursePitchCm = BrickHeightCm + BedJointThicknessCm;

	/** Density-first multiplication order — the PieceMassKg contract; 2.72163125 kg. */
	constexpr double BrickMassKg =
		ClayDensityGramsPerCubicCm * BrickLengthCm * BrickWidthCm * BrickHeightCm / 1000.0;

	/** The row-3 FALLS rung: 30 courses, whose whole-structure ground-only LP fells 29 pieces. */
	constexpr int32 Courses = 30;

	struct FStack
	{
		FStructure Structure;
		TArray<FPieceBox> Boxes;
	};

	/** Lay course i centred at (i*10, 0, 3.25 + i*7.5); course 0 grounded, each course bedded on the one below. */
	void LayStack(FStack& OutStack)
	{
		for (int32 Course = 0; Course < Courses; ++Course)
		{
			FPieceBox Box;
			Box.ExtentCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm) * 0.5;
			Box.CentreCm = FVector(
				double(Course) * OffsetPerCourseCm,
				0.0,
				BrickHeightCm / 2.0 + double(Course) * CoursePitchCm);

			OutStack.Structure.AddPiece(BrickMassKg, /*bIsGrounded*/ Course == 0, Box.CentreCm);
			OutStack.Boxes.Add(Box);
		}

		for (int32 First = 0; First < OutStack.Boxes.Num(); ++First)
		{
			for (int32 Second = First + 1; Second < OutStack.Boxes.Num(); ++Second)
			{
				FConnection Joint;

				if (MakeInterface(
						First, OutStack.Boxes[First], Second, OutStack.Boxes[Second],
						BedJointThicknessCm, GeneralPurposeMortar, Joint))
				{
					OutStack.Structure.AddConnection(Joint);
				}
			}
		}
	}
}

/**
 * The regional prover, grown to cover the whole structure, releases EXACTLY the whole-structure
 * ground-only LP's moving set — every one of those pieces Falling, the released count equal to the
 * set's size, and nothing Stranded.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRegionalProverCoversWholeStructureTest,
	"DestructionGame.Core.Structure.RegionalProver.CoversWholeStructureReleasesTheWholeStructureLPMovingSet",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRegionalProverCoversWholeStructureTest::RunTest(const FString& Parameters)
{
	using namespace RegionalProverSeamSupport;
	using namespace RigidBlockOracle;

	FStack Stack;
	LayStack(Stack);

	// FIXTURE PRECONDITION: one bed joint per course above the base, and honest geometry.
	TestEqual(TEXT("FIXTURE: exactly one bed joint per course above the base"),
		Stack.Structure.NumConnections(), Courses - 1);
	TestTrue(TEXT("FIXTURE: the stack has complete geometry"),
		Stack.Structure.HasCompleteGeometry());

	/*
	 * (B) The independent truth set: the whole-structure ground-only LP, a different code path.
	 * Pose feasibility at self-weight (bGravityIsLive = false) with below-cap first-crack rows,
	 * exactly the authority BreakByEquilibrium poses.
	 */
	FOracleProblem WholeProblem;
	FString WhyNot;

	TestTrue(
		*FString::Printf(TEXT("TRUTH: the whole structure bridges to an oracle problem (%s)"), *WhyNot),
		BuildRigidBlockProblem(Stack.Structure, WholeProblem, WhyNot));

	WholeProblem.bGravityIsLive = false;
	WholeProblem.bFirstCrackRows = true;

	const FOracleResult WholeResult = SolveRigidBlock(WholeProblem);

	TestEqual(TEXT("TRUTH: the whole-structure ground-only LP proves the lean FALLS"),
		OutcomeOf(WholeResult), EOracleOutcome::Falls);
	TestTrue(TEXT("TRUTH: the fall carries a present, Farkas-certified collapse mechanism"),
		WholeResult.Mechanism.bPresent && WholeResult.Mechanism.bIsCertified);

	// The truth set: the FStructure pieces whose oracle blocks move, mapped through PieceOfBlock.
	TSet<int32> TruthMoving;

	for (int32 Block = 0; Block < WholeResult.Mechanism.Blocks.Num(); ++Block)
	{
		if (WholeResult.Mechanism.Blocks[Block].bMoves)
		{
			if (WholeProblem.PieceOfBlock.IsValidIndex(Block))
			{
				TruthMoving.Add(WholeProblem.PieceOfBlock[Block]);
			}
		}
	}

	TestTrue(
		*FString::Printf(
			TEXT("TRUTH: the LP must genuinely fell something, or the regional arm has no target ")
			TEXT("(moving pieces %d)"),
			TruthMoving.Num()),
		TruthMoving.Num() > 0);

	/*
	 * (C) The regional prover from the base course, with a cap far exceeding the 30-piece structure
	 * so the flood covers everything and the grounded boundary is the earth alone (no cut). It must
	 * release exactly the truth set: assert on mechanism (Falling / count / 0 stranded), never movement.
	 */
	const TArray<int32> Seed = { 0 };
	const int32 RegionBlockCap = 512; // >= NumPieces (30): the region floods everything.

	const int32 Released = Stack.Structure.SolveAndBreak_WithRegionalProver(Seed, RegionBlockCap);

	for (const int32 Piece : TruthMoving)
	{
		TestEqual(
			*FString::Printf(
				TEXT("REGIONAL: truth-set piece %d (moved by the whole-structure LP) must read ")
				TEXT("Falling after the regional prover"),
				Piece),
			Stack.Structure.GetPieceSupport(Piece), EPieceSupport::Falling);
	}

	TestEqual(
		*FString::Printf(
			TEXT("REGIONAL: the prover releases exactly the whole-structure LP's moving set ")
			TEXT("(%d) — released %d"),
			TruthMoving.Num(), Released),
		Released, TruthMoving.Num());

	int32 Stranded = 0;

	for (int32 Piece = 0; Piece < Stack.Structure.NumPieces(); ++Piece)
	{
		if (!Stack.Structure.IsPieceRemoved(Piece)
			&& Stack.Structure.GetPieceSupport(Piece) == EPieceSupport::Stranded)
		{
			++Stranded;
		}
	}

	TestEqual(
		*FString::Printf(TEXT("REGIONAL: nothing is Stranded — a routing decline is not a collapse (%d)"),
			Stranded),
		Stranded, 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
