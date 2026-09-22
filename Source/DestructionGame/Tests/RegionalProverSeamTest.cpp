// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Regional collapse prover, slice 1 (REGIONAL_PROVER_PLAN.md): the end-to-end seam. With a cap
 * covering the whole structure there is no cut, so the prover must release exactly the
 * whole-structure LP's moving set. The cutting case is RegionalProverGroundedCutTest.
 *
 * Fixture: the 30-course leaning stack (10 cm per course, mortared), below the 200-block cap. The
 * truth set comes from BuildRigidBlockProblem + SolveRigidBlock, a different path from the prover.
 * Asserted: every truth piece reads Falling, the released count matches, nothing is Stranded.
 */
namespace RegionalProverSeamSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickWidthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;

	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** Just under half a brick per course; the bond cannot hold this at height. */
	constexpr double OffsetPerCourseCm = 10.0;

	constexpr double BedJointThicknessCm = 1.0;
	constexpr double CoursePitchCm = BrickHeightCm + BedJointThicknessCm;

	/** Density first, as in PieceMassKg: 2.72163125 kg. */
	constexpr double BrickMassKg =
		ClayDensityGramsPerCubicCm * BrickLengthCm * BrickWidthCm * BrickHeightCm / 1000.0;

	/** 30 courses; the whole-structure LP fells 29. */
	constexpr int32 Courses = 30;

	struct FStack
	{
		FStructure Structure;
		TArray<FPieceBox> Boxes;
	};

	/** Course i centred at (i*10, 0, 3.25 + i*7.5); course 0 grounded. */
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

/** Covering the whole structure, the prover releases exactly the whole-structure LP's moving set. */
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

	TestEqual(TEXT("FIXTURE: exactly one bed joint per course above the base"),
		Stack.Structure.NumConnections(), Courses - 1);
	TestTrue(TEXT("FIXTURE: the stack has complete geometry"),
		Stack.Structure.HasCompleteGeometry());

	// Truth: the whole-structure ground-only LP, posed as BreakByEquilibrium poses it.
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
