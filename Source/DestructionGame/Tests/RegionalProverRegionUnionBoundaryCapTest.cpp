// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * SLICE 2 OF THE REGIONAL COLLAPSE PROVER (REGIONAL_PROVER_PLAN.md §1, review item 12) — THE
 * R-UNION-B CAP, RED. When the region cap CUTS (is smaller than the structure), the flood must
 * bound the WHOLE posed problem — the interior region R PLUS the grounded frontier ring B — at the
 * cap. That is the plan's stated stopping gate: "region ∪ grounded boundary <= region cap"
 * (§1, gate 1). Slice 1's flood does NOT: it caps `Region.Num() < RegionBlockCap` and then appends
 * the ring on top (Structure.cpp's flood, the two `Region.Num() < RegionBlockCap` guards), so the
 * problem it poses is R (up to `cap` blocks) plus B — strictly MORE than the cap once the cap bites.
 *
 * THE WITNESS, MEASURED. On the 30-course leaning stack, seed = {29} (top course), cap = 15: the
 * flood fills the interior R = {15..29} (15 blocks) and then grounds the one-hop ring B = {14}
 * (1 block), so BuildRegionalProblem is handed 16 blocks against a cap of 15. The prover works and
 * fells the right pieces (that is slice 1's machinery, characterised green in the sibling test) —
 * this test is only about the SIZE INVARIANT the plan's latency gate depends on: the posed problem
 * must never exceed the cap, or a cap chosen for a per-action block budget (D8, DESIGN §8) is
 * silently overspent by the ring.
 *
 * WHY THIS MATTERS AND WHY IT IS NOT A SOUNDNESS BUG. Counting R-only can only make the region
 * LARGER, and by the relaxation theorem (grounding a boundary only adds support) every felled set is
 * a subset of the whole-structure truth regardless of the R/B partition — so this over-count never
 * produces a WRONG collapse, only a bigger-than-budgeted solve. It is a latency/scale correctness
 * gap, and the honest way to pin it is the posed problem SIZE, which is mechanism-adjacent (the
 * count of blocks the oracle was actually handed) and immune to jitter — never displacement.
 *
 * RED FOR THE RIGHT REASON. FStructure::GetLastRegionalProblemBlockCount is a slice-2 STUB that
 * returns INDEX_NONE (the count is not recorded yet), so the first assertion — "the posed R∪B count
 * was recorded" — fails on the SENTINEL, and the invariant assertion fails too (-1 is not in
 * [1, cap]). The red is the MISSING behaviour, not a malformed test: the accessor has no member
 * behind it and the flood still counts R alone.
 *
 * THE PRODUCTION SURFACE THIS TEST SPECIFIES (what dev-expert builds to), TWO PARTS:
 *   1. int32 FStructure::GetLastRegionalProblemBlockCount() const — record and return the number of
 *      blocks the last SolveAndBreak_WithRegionalProver posed to BuildRegionalProblem, i.e.
 *      |R ∪ B| (interior region blocks plus grounded ring blocks). A member stamped at the end of
 *      the pose (Problem.Blocks.Num()); INDEX_NONE until a prove has run.
 *   2. The flood in SolveAndBreak_WithRegionalProver must bound region ∪ boundary <= RegionBlockCap
 *      — stop adding to R once |R| + (the ring it would carry) would exceed the cap, so a smaller
 *      interior is posed and |R ∪ B| lands at or under the cap. After the fix, seed = {29}, cap = 15
 *      poses <= 15 blocks (a 14-block interior on a grounded ring, still infeasible — demand grows
 *      with K^2 so 14 courses on a grounded cut still topple).
 *
 * NEEDS A TICKING WORLD: NO. One FStructure state query after one in-process solve; no Chaos, no
 * world tick. Same footing as the leaning-stack acceptance and the sibling seam test.
 *
 * UNITS ARE DERIVED HERE, never imported. NAMED NAMESPACE, not anonymous: a unity build merges many
 * files into one translation unit.
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

	/** Density-first multiplication order — the PieceMassKg contract; 2.72163125 kg. */
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

/**
 * The posed problem — region united with its grounded boundary ring — must fit inside the region
 * cap. A cut that grounds a one-hop ring on top of a cap-full interior overspends the cap by the
 * ring, and the flood must count R ∪ B, not R alone.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRegionalProverUnionCapTest,
	"DestructionGame.Core.Structure.RegionalProver.RegionUnionBoundaryFitsTheCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRegionalProverUnionCapTest::RunTest(const FString& Parameters)
{
	using namespace RegionalProverUnionCapSupport;

	FStack Stack;
	LayStack(Stack);

	/* FIXTURE PRECONDITION: a 30-block chain, one bed joint per course above the base. */
	TestEqual(TEXT("FIXTURE: 30 pieces"), Stack.Structure.NumPieces(), Courses);
	TestEqual(TEXT("FIXTURE: exactly one bed joint per course above the base"),
		Stack.Structure.NumConnections(), Courses - 1);
	TestTrue(TEXT("FIXTURE: the stack has complete geometry"),
		Stack.Structure.HasCompleteGeometry());

	/*
	 * SEED THE TOP COURSE and cap the region at 15. The flood grows DOWN through the chain to a
	 * 15-block interior R = {15..29}, then grounds the one-hop ring B = {14}: 16 blocks posed
	 * against a cap of 15. (The prove itself works — that it fells {15..29} is the sibling
	 * characterisation test's job; here we only need a cut to have HAPPENED so a problem was posed.)
	 */
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

	/*
	 * (1) THE COUNT MUST BE RECORDED. The stub returns INDEX_NONE, so this is the first thing that
	 * goes red: dev wires the accessor to the posed |R ∪ B| block count.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("the posed region∪boundary block count must be recorded (got %d, the sentinel is ")
			TEXT("%d); wire GetLastRegionalProblemBlockCount to the number of blocks ")
			TEXT("BuildRegionalProblem was handed"),
			Posed, int32(INDEX_NONE)),
		Posed != INDEX_NONE);

	/*
	 * (2) THE INVARIANT. Region ∪ grounded boundary must fit the cap. Today the flood counts R alone
	 * and grounds the ring on top, so it poses 16 against a cap of 15; dev bounds R ∪ B <= cap so a
	 * 14-block interior is posed on the grounded ring (still infeasible — the demand grows with K^2).
	 * A recorded value below 1 (e.g. the sentinel) also fails here, so the invariant cannot be met
	 * vacuously by an unrecorded count.
	 */
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
