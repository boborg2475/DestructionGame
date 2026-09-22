// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Regional prover slice 3, grow-on-contact (REGIONAL_PROVER_PLAN.md, review item 12). The greedy
 * flood fills the cap-ball around its seeds, so on a branching flood it spends budget on blocks
 * outside the mechanism. Grow-on-contact follows only the mechanism.
 *
 * Fixture: a 30-course leaning stack (course 0 grounded; the whole-structure LP fells {1..29};
 * seed piece 29) and a disconnected island of 20 grounded bricks (seed piece 30). The island is
 * disconnected because a bonded grounded branch would prop the stack and change the truth; a
 * +Y branch would give an out-of-plane joint the 2D oracle refuses.
 *
 * At cap 44 greedy admits all 20 island bricks and fells only {7..29}; grow ignores the island and
 * fells {1..29}. The stack-only arm fells {1..29} at the same cap, so the cap is not the limit.
 * Asserts on support state, never displacement.
 *
 * Invariants dev must keep: cap >= size fells {1..29}; the grounded-cut chain {16..29}/14; |R u B|
 * <= cap at every step; permutation invariance; a local over-hold settles in about one solve. Grow
 * in chunks, not a ring at a time. No world. Named namespace.
 */
namespace RegionalProverGrowOnContactSupport
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

	/** The row-3 FALLS rung: 30 courses, of which the LP fells 29. */
	constexpr int32 Courses = 30;

	/** Island size: enough that a cap-44 greedy flood admitting it cannot reach the stack's base. */
	constexpr int32 IslandBricks = 20;

	/** 14 blocks over the stack, so grow reaches it all while greedy stops at course 7. */
	constexpr int32 RegionCap = 44;

	/** Router-only baseline: a tiny gate cap keeps the entry's SolveLoads off the whole-structure LP. */
	constexpr int32 GateCap = 5;

	struct FFixture
	{
		FStructure Structure;
		TArray<FPieceBox> Boxes;
		int32 StackTop = INDEX_NONE;   // piece 29 — the mechanism seed (the free top).
		int32 IslandFirst = INDEX_NONE;// piece 30 — the standing-island seed.
		TArray<int32> IslandPieces;    // the grounded standing island (never in the truth).
	};

	void Build(FFixture& F)
	{
		// The leaning stack: course 0 grounded, each course offset 10 cm past the one below.
		for (int32 Course = 0; Course < Courses; ++Course)
		{
			FPieceBox Box;
			Box.ExtentCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm) * 0.5;
			Box.CentreCm = FVector(
				double(Course) * OffsetPerCourseCm,
				0.0,
				BrickHeightCm / 2.0 + double(Course) * CoursePitchCm);

			const int32 Piece = F.Structure.AddPiece(BrickMassKg, /*bIsGrounded*/ Course == 0, Box.CentreCm);
			F.Boxes.Add(Box);

			if (Course == Courses - 1)
			{
				F.StackTop = Piece;
			}
		}

		// The island: a grounded run from X = -500, far enough that no joint forms with the stack.
		for (int32 B = 0; B < IslandBricks; ++B)
		{
			FPieceBox Box;
			Box.ExtentCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm) * 0.5;
			Box.CentreCm = FVector(
				-500.0 - double(B) * (BrickLengthCm + BedJointThicknessCm),
				0.0,
				BrickHeightCm / 2.0);

			const int32 Piece = F.Structure.AddPiece(BrickMassKg, /*bIsGrounded*/ true, Box.CentreCm);
			F.Boxes.Add(Box);
			F.IslandPieces.Add(Piece);

			if (B == 0)
			{
				F.IslandFirst = Piece;
			}
		}

		for (int32 First = 0; First < F.Boxes.Num(); ++First)
		{
			for (int32 Second = First + 1; Second < F.Boxes.Num(); ++Second)
			{
				FConnection Joint;
				if (MakeInterface(
						First, F.Boxes[First], Second, F.Boxes[Second],
						BedJointThicknessCm, GeneralPurposeMortar, Joint))
				{
					F.Structure.AddConnection(Joint);
				}
			}
		}
	}

	/** Run the prover on a fresh fixture with the given seed and return the sorted Falling set. */
	TArray<int32> ProveAndCollectFalling(const TArray<int32>& Seed, int32& OutReleased, int32& OutStranded)
	{
		FFixture F;
		Build(F);
		F.Structure.SetEquilibriumGateBlockCap(GateCap);

		OutReleased = F.Structure.SolveAndBreak_WithRegionalProver(Seed, RegionCap);

		TArray<int32> Falling;
		OutStranded = 0;
		for (int32 P = 0; P < F.Structure.NumPieces(); ++P)
		{
			if (F.Structure.IsPieceRemoved(P))
			{
				continue;
			}
			const EPieceSupport Support = F.Structure.GetPieceSupport(P);
			if (Support == EPieceSupport::Falling)
			{
				Falling.Add(P);
			}
			if (Support == EPieceSupport::Stranded)
			{
				++OutStranded;
			}
		}
		Falling.Sort();
		return Falling;
	}
}

/**
 * Seeded with the stack top and the island, grow-on-contact fells all of {1..29}, only pieces the
 * whole-structure LP also fells, never the island, and strands nothing. No world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRegionalProverGrowOnContactTest,
	"DestructionGame.Core.Structure.RegionalProver.GrowOnContactReachesPastAStandingBranch",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRegionalProverGrowOnContactTest::RunTest(const FString& Parameters)
{
	using namespace RegionalProverGrowOnContactSupport;
	using namespace RigidBlockOracle;

	FFixture F;
	Build(F);

	// FIXTURE PRECONDITIONS.
	TestEqual(TEXT("FIXTURE: 30 stack bricks + 20 island bricks"),
		F.Structure.NumPieces(), Courses + IslandBricks);
	TestEqual(TEXT("FIXTURE: 29 bed joints in the stack + 19 head joints in the island, no cross joint"),
		F.Structure.NumConnections(), (Courses - 1) + (IslandBricks - 1));
	TestTrue(TEXT("FIXTURE: complete geometry"), F.Structure.HasCompleteGeometry());
	TestEqual(TEXT("FIXTURE: stack top is piece 29"), F.StackTop, Courses - 1);
	TestEqual(TEXT("FIXTURE: island starts at piece 30"), F.IslandFirst, Courses);

	// (A) Independent truth from the whole-structure LP: fells {1..29}, stands the island.
	FOracleProblem Whole;
	FString WhyNot;
	TestTrue(*FString::Printf(TEXT("TRUTH: the whole structure bridges (%s)"), *WhyNot),
		BuildRigidBlockProblem(F.Structure, Whole, WhyNot));

	Whole.bGravityIsLive = false;
	Whole.bFirstCrackRows = true;

	const FOracleResult WholeResult = SolveRigidBlock(Whole);
	TestEqual(TEXT("TRUTH: the whole-structure LP proves the lean FALLS"),
		OutcomeOf(WholeResult), EOracleOutcome::Falls);

	TSet<int32> TruthMoving;
	for (int32 Block = 0; Block < WholeResult.Mechanism.Blocks.Num(); ++Block)
	{
		if (WholeResult.Mechanism.Blocks[Block].bMoves && Whole.PieceOfBlock.IsValidIndex(Block))
		{
			TruthMoving.Add(Whole.PieceOfBlock[Block]);
		}
	}

	TestEqual(
		*FString::Printf(TEXT("TRUTH: the LP fells every course above the base, {1..29} (%d)"),
			TruthMoving.Num()),
		TruthMoving.Num(), Courses - 1);
	for (const int32 Island : F.IslandPieces)
	{
		TestFalse(
			*FString::Printf(TEXT("TRUTH: grounded island piece %d stands (not in the truth)"), Island),
			TruthMoving.Contains(Island));
	}

	// (B) Cap-sufficiency witness: seeded with the stack top alone, cap 44 fells {1..29}.
	int32 StackOnlyReleased = 0;
	int32 StackOnlyStranded = 0;
	const TArray<int32> StackOnlyFalling =
		ProveAndCollectFalling({ F.StackTop }, StackOnlyReleased, StackOnlyStranded);

	TestEqual(
		*FString::Printf(TEXT("WITNESS: the stack-only flood at cap %d fells all 29 (%d)"),
			RegionCap, StackOnlyReleased),
		StackOnlyReleased, Courses - 1);
	for (int32 Piece = 1; Piece <= Courses - 1; ++Piece)
	{
		TestTrue(
			*FString::Printf(TEXT("WITNESS: cap %d reaches stack piece %d on the mechanism alone"),
				RegionCap, Piece),
			StackOnlyFalling.Contains(Piece));
	}

	/*
	 * (C) Seeded with both, grow must ignore the island and fell {1..29}. Greedy fells only
	 * {7..29}.
	 */
	int32 IslandReleased = 0;
	int32 IslandStranded = 0;
	const TArray<int32> IslandFalling =
		ProveAndCollectFalling({ F.StackTop, F.IslandFirst }, IslandReleased, IslandStranded);

	for (int32 Piece = 1; Piece <= Courses - 1; ++Piece)
	{
		TestTrue(
			*FString::Printf(
				TEXT("GROW: mechanism piece %d must read Falling — grow reaches past the standing island"),
				Piece),
			IslandFalling.Contains(Piece));
	}

	TestEqual(
		*FString::Printf(
			TEXT("GROW: the prove releases the whole mechanism (29), a strict superset of greedy's ")
			TEXT("{7..29}/23 — released %d"),
			IslandReleased),
		IslandReleased, Courses - 1);

	// Soundness: every felled piece is in the LP truth.
	for (const int32 Piece : IslandFalling)
	{
		TestTrue(
			*FString::Printf(TEXT("SOUNDNESS: felled piece %d is inside the whole-structure LP truth"),
				Piece),
			TruthMoving.Contains(Piece));
	}

	for (const int32 Island : F.IslandPieces)
	{
		TestFalse(
			*FString::Printf(TEXT("GROW: the grounded island piece %d is never felled"), Island),
			IslandFalling.Contains(Island));
	}

	TestEqual(
		*FString::Printf(TEXT("GROW: nothing is Stranded — a routing decline is not a collapse (%d)"),
			IslandStranded),
		IslandStranded, 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
