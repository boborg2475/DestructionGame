// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * SLICE 2 OF THE REGIONAL COLLAPSE PROVER (REGIONAL_PROVER_PLAN.md slice 2, review item 12) — THE
 * GROUNDED CUT, A CHARACTERISATION NET, GREEN ON ARRIVAL. Slice 1 landed a COMPLETE
 * flood -> grounded-boundary pose -> Falls -> Falling-only stitch (its own header's "slice-1 STUB"
 * language is stale — the function is fully implemented), and slice-1's test only exercised the cap
 * >= structure-size case (boundary = earth alone, NO cut). This test pins the behaviour when the cap
 * actually CUTS mid-structure and the frontier ring is a real interior piece pinned grounded — a
 * regression net, not a driver: there was no production code to write for it, so it is green the day
 * it lands and its value is catching a future regression, proven below by mutation.
 *
 * WHY THE "TEETH" THE PLAN SKETCHED ARE UNREALIZABLE, AND WHAT THE HONEST CONTRAST IS. The plan's
 * slice-2 sketch asked for teeth where the GROUNDED pose FALLS while the FREE (ring-excluded) pose
 * STANDS ("free -> wrongly feasible"). That contrast CANNOT EXIST in this model. A grounded boundary
 * block writes NO equilibrium rows and carries NO weight — it is a perfect reaction and nothing
 * else — so the grounded pose's admissible-force set is always a SUPERSET of the free pose's
 * (grounding only ADDS reaction variables, never load or constraint the region must satisfy).
 * Therefore grounded feasibility ⊇ free feasibility, ALWAYS: if the grounded (most-supported) pose
 * falls, the free pose falls too. "Grounded Falls AND free Stands on the same region" is impossible;
 * measured across three cuts it never occurs. The plan's wording conflated the free-EXCLUDED pose
 * with the refuted IMPORTED-LOAD pose ("the omitted weight left with it", PROMOTION_DESIGN) — a
 * plain free cut drops SUPPORT and falls, it does not wrongly stand.
 *
 * THE REALIZABLE, PHYSICALLY-CORRECT CONTRAST (arm 4 below) is the OPPOSITE polarity: on a region
 * that owns its own ground, region{0..14} of the stack, the grounded ring {15} sits ABOVE the region
 * and acts as a real top anchor — GROUNDED it STANDS (lambda at the cap), the same region posed FREE
 * (ring excluded) FALLS (14 blocks move). That proves the grounding is LOAD-BEARING — it adds real
 * support — and it is exactly why the prover must NEVER read "stands" from a grounded pose: a
 * grounded anchor can hide a genuine collapse, so the override is one-directional, toward Falling
 * only (REGIONAL_PROVER_PLAN.md §2, "no false stand by construction").
 *
 * THE FIXTURE: the 30-course, 10 cm/course mortared leaning stack (the row-3 FALLS rung of
 * LeaningStackAcceptanceTest). The region above a grounded cut behaves as a shorter stack grounded
 * at that cut; its bottom joint's first-crack bond-tension demand grows with the SQUARE of the
 * interior course count, so a tall enough interior on a grounded ring still topples. At cap 15,
 * seed {29}, the interior is the top 14 courses {16..29} and it falls; the whole-structure
 * ground-only LP (a DIFFERENT code path — the whole-structure bridge + oracle) fells {1..29}, so the
 * region's felled set is a genuine SUBSET of the truth (relaxation soundness).
 *
 * ASSERT ON MECHANISM, NEVER DISPLACEMENT. Solver state only: which pieces read Falling, the released
 * count, subset-of-truth, 0 stranded, and the grounded-vs-free feasibility outcomes. No centimetre
 * of movement is read — a severed lean can rest in place and still be genuinely released.
 *
 * NEEDS A TICKING WORLD: NO. Pure FOracleProblem solves and FStructure state queries; no Chaos, no
 * world tick. UNITS ARE DERIVED HERE, never imported. NAMED NAMESPACE, not anonymous.
 */
namespace RegionalProverGroundedCutSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickWidthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;
	constexpr double ClayDensityGramsPerCubicCm = 1.9;
	constexpr double OffsetPerCourseCm = 10.0;
	constexpr double BedJointThicknessCm = 1.0;
	constexpr double CoursePitchCm = BrickHeightCm + BedJointThicknessCm;

	/** Density-first multiplication order — the PieceMassKg contract; 2.72163125 kg. */
	constexpr double BrickMassKg =
		ClayDensityGramsPerCubicCm * BrickLengthCm * BrickWidthCm * BrickHeightCm / 1000.0;

	constexpr int32 Courses = 30;

	struct FStack
	{
		FStructure Structure;
		TArray<FPieceBox> Boxes;
	};

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

	/** Pose R + B (feasibility, first-crack) through the grounded-boundary bridge and read outcome. */
	RigidBlockOracle::EOracleOutcome PoseOutcome(
		const FStructure& S, const TSet<int32>& Region, const TSet<int32>& Boundary, int32& OutMoved)
	{
		using namespace RigidBlockOracle;

		OutMoved = 0;

		FOracleProblem Problem;
		FString WhyNot;

		if (!BuildRegionalProblem(S, Region, Boundary, Problem, WhyNot))
		{
			return EOracleOutcome::Unanswerable;
		}

		Problem.bGravityIsLive = false;
		Problem.bFirstCrackRows = true;

		const FOracleResult Result = SolveRigidBlock(Problem);

		for (const FOracleMechanismBlock& Block : Result.Mechanism.Blocks)
		{
			if (Block.bMoves)
			{
				++OutMoved;
			}
		}

		return OutcomeOf(Result);
	}
}

/**
 * The grounded cut fells the interior sub-stack, soundly. seed {29}, cap 15 releases exactly the
 * top 14 courses {16..29}, that set is a subset of the whole-structure LP's truth {1..29}, nothing
 * is stranded, and the grounding is proven load-bearing by the grounded-vs-free contrast on a region
 * that owns its ground.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRegionalProverGroundedCutTest,
	"DestructionGame.Core.Structure.RegionalProver.GroundedCutFellsTheInteriorSubStack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRegionalProverGroundedCutTest::RunTest(const FString& Parameters)
{
	using namespace RegionalProverGroundedCutSupport;
	using namespace RigidBlockOracle;

	FStack Stack;
	LayStack(Stack);

	TestEqual(TEXT("FIXTURE: 30 pieces"), Stack.Structure.NumPieces(), Courses);
	TestEqual(TEXT("FIXTURE: exactly one bed joint per course above the base"),
		Stack.Structure.NumConnections(), Courses - 1);
	TestTrue(TEXT("FIXTURE: the stack has complete geometry"),
		Stack.Structure.HasCompleteGeometry());

	/* ================================================================================
	 * (A) THE INDEPENDENT TRUTH — the whole-structure ground-only LP, a DIFFERENT code path.
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

	TSet<int32> TruthMoving;
	for (int32 Block = 0; Block < WholeResult.Mechanism.Blocks.Num(); ++Block)
	{
		if (WholeResult.Mechanism.Blocks[Block].bMoves && WholeProblem.PieceOfBlock.IsValidIndex(Block))
		{
			TruthMoving.Add(WholeProblem.PieceOfBlock[Block]);
		}
	}

	/* Every course above the grounded base loses the earth: truth is {1..29}. */
	TestEqual(
		*FString::Printf(TEXT("TRUTH: the whole-structure LP fells every course above the base (%d)"),
			TruthMoving.Num()),
		TruthMoving.Num(), Courses - 1);
	TestFalse(TEXT("TRUTH: the grounded base is not in the moving set"), TruthMoving.Contains(0));

	/* ================================================================================
	 * (B) THE GROUNDED CUT through the production entry point: seed {29}, cap 15.
	 *
	 * WHY THE INTERIOR IS {16..29}, NOT {15..29}: the flood bounds region UNION grounded-boundary
	 * <= cap (REGIONAL_PROVER_PLAN.md §1). With cap 15 it holds the 15th block out as the one-hop
	 * grounded ring B = {15}, posing a 14-block interior R = {16..29} so |R∪B| = 15 fits the cap
	 * exactly — rather than posing a 15-block interior {15..29} on ring {14}, which would be 16
	 * blocks (the pre-fix over-count). Piece 15 is now the grounded boundary: it reads Supported and
	 * never moves; the 14 interior courses above it fall.
	 * ================================================================================ */
	const int32 Cap = 15;
	const TArray<int32> Seed = { 29 };

	const int32 Released = Stack.Structure.SolveAndBreak_WithRegionalProver(Seed, Cap);

	/* The released set: exactly the top 14 courses {16..29} (piece 15 is the grounded ring), each reading Falling. */
	TArray<int32> Falling;
	int32 Stranded = 0;
	for (int32 Piece = 0; Piece < Stack.Structure.NumPieces(); ++Piece)
	{
		if (Stack.Structure.IsPieceRemoved(Piece))
		{
			continue;
		}

		const EPieceSupport Support = Stack.Structure.GetPieceSupport(Piece);
		if (Support == EPieceSupport::Falling)
		{
			Falling.Add(Piece);
		}
		if (Support == EPieceSupport::Stranded)
		{
			++Stranded;
		}
	}
	Falling.Sort();

	TestEqual(
		*FString::Printf(TEXT("CUT: the prover releases 14 interior pieces (released %d)"), Released),
		Released, 14);
	TestEqual(
		*FString::Printf(TEXT("CUT: exactly 14 pieces read Falling (%d)"), Falling.Num()),
		Falling.Num(), 14);

	for (int32 Piece = 16; Piece <= 29; ++Piece)
	{
		TestEqual(
			*FString::Printf(TEXT("CUT: top-of-stack piece %d reads Falling"), Piece),
			Stack.Structure.GetPieceSupport(Piece), EPieceSupport::Falling);
	}

	/* SOUNDNESS: every felled piece is one the whole-structure LP also fells (subset of truth). */
	for (const int32 Piece : Falling)
	{
		TestTrue(
			*FString::Printf(
				TEXT("SOUNDNESS: the region fells piece %d, which the whole-structure LP must also ")
				TEXT("fell (grounding only adds support, so the region mechanism is a subset of truth)"),
				Piece),
			TruthMoving.Contains(Piece));
	}

	TestEqual(
		*FString::Printf(TEXT("CUT: nothing is Stranded — a routing decline is not a collapse (%d)"),
			Stranded),
		Stranded, 0);

	/* ================================================================================
	 * (C) GROUNDING IS LOAD-BEARING — the realizable contrast (see the file header for why the
	 * plan's inverted "grounded Falls / free Stands" teeth are impossible). On region{0..14}, which
	 * owns its ground at piece 0, the ring {15} is a top anchor: GROUNDED it Stands, FREE it Falls.
	 *
	 * ON A FRESH STACK, deliberately: arm (B) is DESTRUCTIVE — the prover severs the joints its
	 * mechanism opens, including the 14-15 bed joint — so posing region{0..14} against the mutated
	 * structure would find that top anchor already disconnected. These are pure bridge
	 * characterisations of the intact stack, so they take an unmutated copy.
	 * ================================================================================ */
	FStack Fresh;
	LayStack(Fresh);

	TSet<int32> BaseRegion;
	for (int32 Piece = 0; Piece <= 14; ++Piece)
	{
		BaseRegion.Add(Piece);
	}

	int32 GroundedMoved = 0;
	const EOracleOutcome GroundedOutcome =
		PoseOutcome(Fresh.Structure, BaseRegion, TSet<int32>{ 15 }, GroundedMoved);

	int32 FreeMoved = 0;
	const EOracleOutcome FreeOutcome =
		PoseOutcome(Fresh.Structure, BaseRegion, TSet<int32>(), FreeMoved);

	TestEqual(
		TEXT("LOAD-BEARING: region{0..14} with the ring {15} GROUNDED as a top anchor STANDS"),
		GroundedOutcome, EOracleOutcome::Stands);
	TestEqual(
		*FString::Printf(
			TEXT("LOAD-BEARING: the SAME region{0..14} posed FREE (ring excluded) FALLS — the ")
			TEXT("grounding was carrying real load; %d blocks move"),
			FreeMoved),
		FreeOutcome, EOracleOutcome::Falls);
	TestTrue(
		*FString::Printf(
			TEXT("LOAD-BEARING: the free-pose fall moves the 14 interior courses above the base ")
			TEXT("(moved %d)"),
			FreeMoved),
		FreeMoved == 14);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
