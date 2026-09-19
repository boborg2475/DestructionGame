// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The regional prover's grounded-boundary cut: a region posed with a one-hop ring of
 * interior pieces held grounded rather than excluded. A characterisation net, green on
 * arrival — no production code to write for it; its value is catching a future
 * regression (proven below by mutation).
 *
 * The plan sketched teeth where a GROUNDED pose falls while the FREE (ring-excluded) pose
 * stands — impossible in this model. A grounded boundary block writes no equilibrium rows
 * and carries no weight (a perfect reaction), so the grounded pose's admissible-force set
 * is always a superset of the free pose's: grounded feasibility ⊇ free feasibility always.
 *
 * The realizable contrast (arm C below) has the opposite polarity: on a region that owns
 * its own ground, a grounded ring sitting above it can be a real load-bearing anchor —
 * grounded it stands, free it falls. That is why the prover must never read "stands" from
 * a grounded pose: a grounded anchor can hide a genuine collapse, so the override is
 * one-directional, toward Falling only (REGIONAL_PROVER_PLAN.md §2).
 *
 * Fixture: the 30-course, 10 cm/course mortared leaning stack (LeaningStackAcceptanceTest's
 * row-3 FALLS rung). A grounded cut's interior behaves as a shorter stack grounded at that
 * cut, whose bottom-joint bond-tension demand grows with the square of the interior course
 * count, so a tall enough interior still topples. At cap 15, seed {29}, the interior is the
 * top 14 courses {16..29} and it falls; the whole-structure ground-only LP (a different code
 * path) fells {1..29} — the region's felled set is a genuine subset of the truth.
 *
 * Assert on mechanism, never displacement: which pieces read Falling, the released count,
 * subset-of-truth, 0 stranded, and the grounded-vs-free feasibility outcomes.
 *
 * No ticking world needed (pure FOracleProblem solves and FStructure queries). Units
 * derived here, never imported. Named namespace, not anonymous.
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
 * The grounded cut fells the interior sub-stack, soundly: seed {29}, cap 15 releases
 * exactly the top 14 courses {16..29}, a subset of the whole-structure LP's truth
 * {1..29}, nothing is stranded, and the grounding is proven load-bearing by the
 * grounded-vs-free contrast on a region that owns its ground. No ticking world needed;
 * see the file header.
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

	// (A) The independent truth — the whole-structure ground-only LP, a different code path.
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

	/*
	 * (B) The grounded cut through the production entry point: seed {29}, cap 15.
	 *
	 * Why the interior is {16..29}, not {15..29}: the flood bounds region UNION
	 * grounded-boundary <= cap (REGIONAL_PROVER_PLAN.md §1). With cap 15 it holds the 15th
	 * block out as the one-hop grounded ring B = {15}, posing a 14-block interior
	 * R = {16..29} so |R∪B| = 15 fits the cap exactly — rather than a 15-block interior
	 * {15..29} on ring {14}, which would be 16 blocks. Piece 15 is now the grounded
	 * boundary: it reads Supported and never moves; the 14 interior courses above it fall.
	 */
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

	/*
	 * (C) Grounding is load-bearing — the realizable contrast (see the file header for why
	 * the plan's inverted "grounded Falls / free Stands" teeth are impossible). On
	 * region{0..14}, which owns its ground at piece 0, the ring {15} is a top anchor:
	 * grounded it Stands, free it Falls.
	 *
	 * On a fresh stack, deliberately: arm (B) is destructive — the prover severs the
	 * joints its mechanism opens, including the 14-15 bed joint — so posing region{0..14}
	 * against the mutated structure would find that top anchor already disconnected.
	 */
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
