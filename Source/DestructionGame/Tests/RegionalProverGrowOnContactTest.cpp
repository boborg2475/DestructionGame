// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Regional collapse prover, slice 3 (REGIONAL_PROVER_PLAN.md slice 3 reframed, review item 12) —
 * grow-on-contact, red. The committed flood is greedy-fill-to-cap: it floods the cap-ball around
 * the seed and poses it once. On a chain that is already optimal (slices 1-2 proved no
 * partial-fell-with-headroom gap). The gap opens only on a branching flood, where the greedy
 * cap-ball spends budget on blocks not in the mechanism and so reaches less of a long mechanism
 * than a mechanism-following grow does. This test drives that gap and is red until grow lands.
 *
 * The fixture — a leaning stack plus a disconnected grounded island, both named in the seed.
 *   - The mechanism: the 30-course leaning stack (row-3 FALLS rung). Course 0 grounded; the
 *     whole-structure LP fells {1..29}. Its free top, piece 29, is a seed.
 *   - The dead-end branch: 20 grounded bricks laid clear of the stack (a standing wall on its own
 *     foundation). Every brick is grounded, so it never moves and is never in the truth. Piece 30
 *     is the second seed.
 *
 * A disconnected island rather than a connected fork: for a branch to stand in the region pose it
 * must own ground, but a grounded branch bonded into the stack props it and reshapes the truth to
 * the part above the prop — which the top-seeded flood reaches first, erasing the gap. A
 * disconnected island bonded to nothing keeps the truth the whole stack {1..29} while forcing the
 * greedy flood to split its cap-ball between the two components. (A +Y-attached branch was rejected:
 * it puts an out-of-plane normal on a joint that the 2D X-Z oracle correctly refuses.)
 *
 * The hand argument (measured, see the constants below):
 *   - Greedy at cap 44 seeded with both {29} and the island {30}: the BFS interleaves the two,
 *     admits all 20 island bricks, and has only ~24 blocks left for the stack — reaching course 7
 *     and felling {7..29} (23). Courses {1..6} read Supported.
 *   - Grow-on-contact seeded the same way follows only the mechanism: the island stands on its own
 *     ground and is never a growth candidate, so grow spends the whole cap on the stack and fells
 *     {1..29}.
 *   - Cap-sufficiency witness (the stack-only arm): the same flood seeded with only {29}, at cap 44,
 *     never reaches the island and fells {1..29} in 30 blocks. So cap 44 is not the limit — the
 *     island is the sole reason the island-seeded greedy flood stops at course 7.
 *
 * The red: the island-seeded prove must fell {1..29} — a strict superset of greedy's {7..29}, every
 * felled piece inside the truth, the island never felled, nothing stranded. Today greedy fells
 * {7..29}, so {1..6} read Supported: red because grow-on-contact is missing. The stack-only and
 * truth arms pass today, so it is not a chain regression.
 *
 * Assert on mechanism, never displacement: which pieces read Falling, the released count,
 * subset-of-truth, the island never Falling, 0 stranded.
 *
 * Invariants this must not break (for dev-expert): the seam (cap >= size fells {1..29}), the
 * grounded cut (chain {16..29}/14 unchanged), the cap-invariant guard (|R u B| <= cap at every
 * step), determinism (permutation-invariant), and the cascade seam (a local over-hold terminates
 * in ~1 solve). Grow by a chunk / geometric step, not a ring at a time, so a local mechanism
 * terminates in 1-2 solves and a cap-spanning one in a few, not cap-many.
 *
 * No ticking world: pure FOracleProblem solves and FStructure state queries. Units derived here,
 * never imported. Named namespace, not anonymous.
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

	/** The row-3 FALLS rung: 30 courses, whose whole-structure ground-only LP fells 29 pieces. */
	constexpr int32 Courses = 30;

	/**
	 * The grounded island: 20 bricks. Sized so a cap-44 greedy flood that admits all 20 has too few
	 * blocks left to reach the stack's base, while grow-on-contact ignores it and spends the whole 44
	 * on the 30-block stack.
	 */
	constexpr int32 IslandBricks = 20;

	/** The region cap: 14 blocks of headroom over the 30-block stack, so grow reaches the whole
	 *  mechanism, yet greedy (admitting the whole island) stops at course 7. */
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

		/*
		 * The grounded island: a straight -X run of grounded bricks at Z = half a brick, far enough
		 * in -X (from X = -500) that no face meets the stack, so MakeInterface writes no cross joint
		 * and the island is a genuine disconnected foundation.
		 */
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
 * Grow-on-contact, seeded with a mechanism and a standing grounded island, fells the WHOLE mechanism
 * {1..29} — a strict superset of what the greedy cap-ball reaches, every felled piece inside the
 * whole-structure LP truth, the island never felled, nothing stranded.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
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

	/*
	 * (A) The independent truth: the whole-structure ground-only LP, a different code path. It fells
	 * every stack course above the grounded base, {1..29}, and stands the island.
	 */
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

	/*
	 * (B) The cap-sufficiency witness: the same flood seeded with only the stack top, at the same cap.
	 * The island is never reached, so this stack-only flood fells {1..29} in 30 blocks — proof that
	 * cap 44 is not the limit. Green today and after grow.
	 */
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
	 * (C) The new behaviour (red until built). Seed both the stack top and the island, at the same
	 * cap. Grow-on-contact must ignore the grounded island and recover the full reach: fell {1..29},
	 * release exactly 29, fell no island piece, strand nothing. Today greedy admits all 20 island
	 * bricks and fells only {7..29} (23), so {1..6} read Supported — the gap this drives.
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

	// SOUNDNESS: every felled piece is one the whole-structure LP also fells (subset of truth).
	for (const int32 Piece : IslandFalling)
	{
		TestTrue(
			*FString::Printf(TEXT("SOUNDNESS: felled piece %d is inside the whole-structure LP truth"),
				Piece),
			TruthMoving.Contains(Piece));
	}

	// The grounded island is a genuine foundation — grow must never fell it.
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
