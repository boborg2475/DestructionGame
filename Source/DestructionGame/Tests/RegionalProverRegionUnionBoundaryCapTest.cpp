// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Regional prover slice 2 (REGIONAL_PROVER_PLAN.md §1, gate 1): the posed problem, interior region
 * R plus grounded ring B, must fit the region cap. Written against a flood that capped R alone and
 * added B on top: on the 30-course stack, seed {29}, cap 15, it posed R = {15..29} plus B = {14},
 * 16 blocks.
 *
 * Not a soundness bug (grounding a boundary only adds support, so felled sets stay correct), but it
 * overspends the per-action block budget (D8, DESIGN §8). Asserted on the posed block count via
 * GetLastRegionalProblemBlockCount (INDEX_NONE until a prove has run), never displacement. With
 * R ∪ B bounded, a 14-block interior is posed and still topples. No world; units derived locally.
 */
namespace RegionalProverUnionCapSupport
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

	/** Density first, per the PieceMassKg contract; 2.72163125 kg. */
	constexpr double BrickMassKg =
		ClayDensityGramsPerCubicCm * BrickLengthCm * BrickWidthCm * BrickHeightCm / 1000.0;

	constexpr int32 Courses = 30;

	struct FStack
	{
		FStructure Structure;
		TArray<FPieceBox> Boxes;
	};

	/** Course i centred at (i*10, 0, 3.25 + i*7.5); course 0 grounded, each course bedded below. */
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

/** The posed region plus its grounded ring must fit within the region cap. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRegionalProverUnionCapTest,
	"DestructionGame.Core.Structure.RegionalProver.RegionUnionBoundaryFitsTheCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRegionalProverUnionCapTest::RunTest(const FString& Parameters)
{
	using namespace RegionalProverUnionCapSupport;

	FStack Stack;
	LayStack(Stack);

	// A 30-block chain, one bed joint per course.
	TestEqual(TEXT("FIXTURE: 30 pieces"), Stack.Structure.NumPieces(), Courses);
	TestEqual(TEXT("FIXTURE: exactly one bed joint per course above the base"),
		Stack.Structure.NumConnections(), Courses - 1);
	TestTrue(TEXT("FIXTURE: the stack has complete geometry"),
		Stack.Structure.HasCompleteGeometry());

	// Seed the top course, cap 15. Only the posed size is checked here, not what falls.
	const int32 Cap = 15;
	const TArray<int32> Seed = { 29 };

	const int32 Released = Stack.Structure.SolveAndBreak_WithRegionalProver(Seed, Cap);

	TestTrue(
		*FString::Printf(
			TEXT("PRECONDITION: the cut must actually pose and prove a region, or the count means ")
			TEXT("nothing (released %d)"),
			Released),
		Released > 0);

	const int32 Posed = Stack.Structure.GetLastRegionalProblemBlockCount();

	// (1) The count was recorded.
	TestTrue(
		*FString::Printf(
			TEXT("the posed region∪boundary block count must be recorded (got %d, the sentinel is ")
			TEXT("%d); wire GetLastRegionalProblemBlockCount to the number of blocks ")
			TEXT("BuildRegionalProblem was handed"),
			Posed, int32(INDEX_NONE)),
		Posed != INDEX_NONE);

	// (2) 1 <= |R ∪ B| <= cap; the lower bound stops the sentinel passing vacuously.
	TestTrue(
		*FString::Printf(
			TEXT("region ∪ grounded boundary (%d blocks) must be <= the region cap (%d) — the flood ")
			TEXT("must count R∪B, not R alone, or a cap chosen for a per-action block budget is ")
			TEXT("silently overspent by the ring"),
			Posed, Cap),
		Posed >= 1 && Posed <= Cap);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
