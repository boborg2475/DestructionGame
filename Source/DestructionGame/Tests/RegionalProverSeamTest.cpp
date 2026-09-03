// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * SLICE 1 OF THE REGIONAL COLLAPSE PROVER (REGIONAL_PROVER_PLAN.md slice 1, review item 12) — THE
 * SEAM, RED. The thinnest end-to-end proof that region extraction -> grounded-boundary pose ->
 * SolveRigidBlock -> Falling-only stitch WORKS, on a fixture the WHOLE-STRUCTURE ground-only LP
 * demonstrably fells. It is NOT a router-vs-LP disagreement (that is slice 4): the truth set here
 * is the whole-structure LP's own moving set, and the claim is that the regional prover — grown to
 * cover the entire structure so its grounded boundary is the earth alone (no cut) — releases
 * EXACTLY that set.
 *
 * THE FIXTURE: a 30-course, 10 cm/course mortared leaning stack (the row-3 FALLS rung of
 * LeaningStackAcceptanceTest). Each course is offset a constant 10 cm past the one below, so the
 * bottom bed joint carries a first-crack bond-tension demand of ~4.9 MPa against a mean bond of at
 * most ~1.0 — the lean has no admissible equilibrium and the whole column above the grounded base
 * topples. At 30 blocks it is below the 200-block equilibrium-gate cap, so the rigid-block LP is a
 * genuine authority for it, and the whole-structure ground-only LP (bGravityIsLive = false,
 * bFirstCrackRows = true, matching the below-cap authority) certifies the fall and NAMES the moving
 * blocks. That named set is the INDEPENDENT TRUTH the regional prover is measured against — derived
 * through a DIFFERENT code path (the whole-structure bridge + oracle) than the region prover, so the
 * test has real teeth rather than mirroring the thing under test.
 *
 * ASSERT ON MECHANISM, NEVER DISPLACEMENT. The three regional-arm assertions are all on solver
 * state: every truth-set piece reads EPieceSupport::Falling, the released COUNT equals the truth
 * set's size, and NOTHING reads Stranded. No centimetre of movement is read — a severed lean can
 * rest exactly in place and still be genuinely released, so displacement would be the wrong witness.
 *
 * NOW GREEN — THIS DROVE THE SLICE-1 BUILD, WHICH LANDED. FStructure::SolveAndBreak_WithRegionalProver
 * is FULLY IMPLEMENTED (commit 2fd6e1b): it runs the router baseline, floods a region from the seed,
 * pins the frontier ring grounded, poses R + boundary through BuildRegionalProblem, solves, and
 * stitches the moved interior pieces Falling. So the regional arm passes — every truth-set piece
 * reads Falling, the released count equals the truth size, and nothing is Stranded — alongside the
 * two truth-arm assertions (the whole-structure LP falls the lean and certifies a non-empty
 * mechanism). It began as the red that drove the slice-1 machinery; it stands now as the regression
 * net over that machinery on the no-cut (cap >= structure size) case. The cutting case — a small cap
 * whose frontier ring is interior structure — is pinned by RegionalProverGroundedCutTest (slice 2).
 *
 * THE PRODUCTION SURFACE THIS TEST SPECIFIES (what dev-expert builds to):
 *   - int32 FStructure::SolveAndBreak_WithRegionalProver(const TArray<int32>& Seed,
 *         int32 RegionBlockCap):
 *       run the router baseline, then union a Falling-only regional-prover override — flood a region
 *       from Seed by joint-hops over PieceJoints up to RegionBlockCap, pin the one-hop frontier ring
 *       GROUNDED, pose R + boundary (bGravityIsLive = false, bFirstCrackRows = true) through the new
 *       grounded-boundary bridge overload, SolveRigidBlock, and on a CERTIFIED mechanism mark the
 *       moved INTERIOR pieces Falling (never Supported) and sever the intact joints it opens. Return
 *       the number of pieces released. With RegionBlockCap >= the block count the region floods the
 *       whole structure and the grounded boundary is the earth alone.
 *   - bool RigidBlockOracle::BuildRegionalProblem(const FStructure&, const TSet<int32>& RegionPieces,
 *         const TSet<int32>& BoundaryPieces, FOracleProblem&, FString&):
 *       the grounded-boundary bridge overload — interior region pieces bridged normally, boundary
 *       pieces forced bGrounded, only R + B included, two-grounded-end joints skipped, every other
 *       refusal as the excluded-pieces form. (Kept SEPARATE from BuildRigidBlockProblem so the shared
 *       bridge poses do not shift and OracleSweepFull stays byte-identical.) This test does not call
 *       it directly — it is the surface SolveAndBreak_WithRegionalProver is built on — but it is named
 *       here as part of the slice-1 spec.
 *
 * NEEDS A TICKING WORLD: NO. Every solve is a pure FOracleProblem or an FStructure state query; no
 * Chaos, no world tick. Same footing as the leaning-stack acceptance and the oracle mechanism tests.
 *
 * UNITS ARE DERIVED HERE, never imported, so a wrong production constant disagrees rather than agrees.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges many files into one translation unit.
 */
namespace RegionalProverSeamSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/* ================================================================================
	 * THE ROW-3 LEANING STACK. Centimetres at Unreal's default 1 uu = 1 cm; nothing imported.
	 * ================================================================================ */

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

	/* FIXTURE PRECONDITION: one bed joint per course above the base, and honest geometry. */
	TestEqual(TEXT("FIXTURE: exactly one bed joint per course above the base"),
		Stack.Structure.NumConnections(), Courses - 1);
	TestTrue(TEXT("FIXTURE: the stack has complete geometry"),
		Stack.Structure.HasCompleteGeometry());

	/* ================================================================================
	 * (B) THE INDEPENDENT TRUTH SET — the whole-structure ground-only LP, a DIFFERENT code
	 * path than the regional prover. Pose FEASIBILITY at self-weight (bGravityIsLive = false)
	 * with the below-cap first-crack rows, exactly the authority BreakByEquilibrium poses.
	 * ================================================================================ */
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

	/* The truth set: the FStructure pieces whose oracle blocks move, mapped through PieceOfBlock. */
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

	/* ================================================================================
	 * (C) THE NEW BEHAVIOUR (RED UNTIL BUILT). Run the regional prover from the base course,
	 * with a region cap far exceeding the 30-piece structure so the flood covers the whole
	 * structure and the grounded boundary is the earth alone (no cut). It must release EXACTLY
	 * the truth set: assert on MECHANISM (Falling / released count / 0 stranded), never movement.
	 * ================================================================================ */
	const TArray<int32> Seed = { 0 };
	const int32 RegionBlockCap = 512; /* >= NumPieces (30): the region floods everything. */

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
