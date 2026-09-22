// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The regional prover's grounded-boundary cut: a region posed with a one-hop ring held grounded
 * rather than excluded. A characterisation net, green on arrival.
 *
 * A grounded boundary is a perfect reaction, so grounded feasibility is a superset of free
 * feasibility. A grounded anchor can therefore hide a real collapse, which is why the prover only
 * overrides toward Falling (REGIONAL_PROVER_PLAN.md §2); arm C shows that contrast.
 *
 * Fixture: the 30-course leaning stack (10 cm per course, mortared). At cap 15, seed {29}, the
 * interior {16..29} falls, a subset of the whole-structure LP's {1..29}.
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

	/** Density first, as in PieceMassKg: 2.72163125 kg. */
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

	/** Pose region + grounded boundary (feasibility, first-crack) and return the outcome. */
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
 * Seed {29}, cap 15 releases exactly {16..29}, a subset of the LP truth, with nothing stranded;
 * and the grounded ring is shown to be load-bearing.
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

	// (A) Independent truth: the whole-structure ground-only LP.
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

	TestEqual(
		*FString::Printf(TEXT("TRUTH: the whole-structure LP fells every course above the base (%d)"),
			TruthMoving.Num()),
		TruthMoving.Num(), Courses - 1);
	TestFalse(TEXT("TRUTH: the grounded base is not in the moving set"), TruthMoving.Contains(0));

	/*
	 * (B) The cap bounds region ∪ boundary (REGIONAL_PROVER_PLAN.md §1), so cap 15 gives interior
	 * {16..29} on grounded ring {15}.
	 */
	const int32 Cap = 15;
	const TArray<int32> Seed = { 29 };

	const int32 Released = Stack.Structure.SolveAndBreak_WithRegionalProver(Seed, Cap);

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
	 * (C) On region {0..14}, which owns its ground, ring {15} is a top anchor: grounded it stands,
	 * free it falls. A fresh stack, because arm (B) severed the 14-15 joint.
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
