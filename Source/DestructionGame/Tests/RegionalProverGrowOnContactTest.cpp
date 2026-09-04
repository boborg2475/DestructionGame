// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * SLICE 3 OF THE REGIONAL COLLAPSE PROVER (REGIONAL_PROVER_PLAN.md slice 3 REFRAMED, review item
 * 12) — GROW-ON-CONTACT, RED. The committed flood is GREEDY-FILL-TO-CAP: it floods the cap-ball
 * around the seed and poses it once. On a CHAIN that is already optimal — greedy fills exactly to the
 * cap and fells the cap-bound partial, and a mechanism-following grow would reach the same cap-bound
 * amount (slices 1-2 proved there is no "partial-fell-with-cap-headroom" gap on a chain). The gap only
 * opens on a BRANCHING flood, where the greedy cap-ball spends budget on blocks that are NOT part of
 * the mechanism and so reaches LESS of a long mechanism than a mechanism-following grow does. This
 * test drives that gap and is RED until grow-on-contact lands.
 *
 * THE FIXTURE — a leaning stack plus a DISCONNECTED GROUNDED ISLAND, both named in the seed.
 *   - THE MECHANISM: the 30-course, 10 cm/course mortared leaning stack (the row-3 FALLS rung of
 *     LeaningStackAcceptanceTest). Course 0 is grounded; the whole-structure ground-only LP fells
 *     every course above it, {1..29}. Its free top, piece 29, is a seed.
 *   - THE DEAD-END BRANCH: a run of 20 GROUNDED bricks laid well clear of the stack (a standing wall,
 *     its own foundation). Every brick is bIsGrounded, so it is real earth — it never moves, it is
 *     never in the truth, and it is a GENUINE FOUNDATION, not a cut artifact. Piece 30 is the second
 *     seed.
 *
 * WHY A DISCONNECTED ISLAND RATHER THAN A CONNECTED FORK. The distinction grow-on-contact makes is
 * "spend the cap on the mechanism, not on blocks that stand." For a branch to STAND in the region pose
 * (so grow refuses to grow into it) it must own ground; but any grounded branch bonded INTO the
 * leaning stack PROPS the stack, and the prop reshapes the truth to exactly the part above the prop —
 * which the top-seeded flood reaches BEFORE the branch, erasing the gap (a connected grounded branch
 * cannot steal budget from UNREACHED mechanism without also deleting that mechanism). A disconnected
 * grounded island bonded to nothing sidesteps the prop entirely: the truth stays the whole stack
 * {1..29}, the island stands on its own ground, and — because both are named in the seed — the greedy
 * flood must split its cap-ball between the two while a mechanism-following grow follows only the
 * stack. (A +Y-attached branch was tried first and rejected: it puts an out-of-plane normal on a
 * joint, which the 2D X-Z oracle correctly refuses, so the whole structure could not even be bridged.)
 *
 * THE HAND ARGUMENT (measured, see the pinned constants below).
 *   - Greedy-fill-to-cap seeded with BOTH the stack top {29} and the island {30}, at cap 44: the BFS
 *     interleaves the two components, admits all 20 island bricks (grounded, standing) into the posed
 *     region, and so has only ~24 of its 44 blocks left for the stack. It reaches down to course 7 and
 *     fells {7..29} (23 pieces); courses {1..6} are never reached and read Supported (the router
 *     baseline over-holds the lean).
 *   - Grow-on-contact seeded the same way follows only the mechanism: the island stands under its own
 *     ground (a genuine foundation, never a growth candidate), so grow never grows into it and spends
 *     the whole cap on the stack — reaching the grounded base and felling the whole {1..29}.
 *   - INDEPENDENT WITNESS THAT THE CAP SUFFICES (the "stack-only oracle" arm): the identical flood
 *     seeded with ONLY the stack top {29}, at the SAME cap 44, never reaches the disconnected island
 *     and fells the whole {1..29} in 30 posed blocks. So cap 44 is not the limit — the island is the
 *     ONLY reason the greedy island-seeded flood stops at course 7. Grow-on-contact must recover the
 *     stack-only reach with the island present.
 *
 * THE RED. The island-seeded prove must fell {1..29} — a STRICT SUPERSET of greedy's {7..29}, every
 * felled piece inside the whole-structure LP truth, the grounded island never felled, nothing
 * stranded. Today greedy fells {7..29}, so pieces {1..6} read Supported instead of Falling: RED for
 * the right reason (grow-on-contact is MISSING; the greedy cap-ball wasted 20 blocks on the standing
 * island). It is NOT a chain regression — the stack-only oracle arm and the truth arm pass today.
 *
 * ASSERT ON MECHANISM, NEVER DISPLACEMENT. Solver state only: which pieces read Falling, the released
 * count, subset-of-truth, the island never Falling, 0 stranded. No centimetre of movement is read.
 *
 * INVARIANTS THIS MUST NOT BREAK (for dev-expert): the seam (cap >= size fells {1..29}), the grounded
 * cut (chain, {16..29}/14 unchanged — greedy and grow agree on a chain), the cap-invariant guard
 * (|R u B| <= cap at every growth step), the determinism guard (permutation-invariant), and the
 * cascade seam (a local over-hold bounded by real foundations terminates in ~1 solve). A naive
 * ring-at-a-time grow could iterate cap-many times on a fully-collapsing region (slower than greedy's
 * one solve) — grow by a chunk / geometric step so a local mechanism terminates in 1-2 solves and a
 * cap-spanning one in a few, not cap-many.
 *
 * NEEDS A TICKING WORLD: NO. Pure FOracleProblem solves and FStructure state queries; no Chaos, no
 * world tick. UNITS ARE DERIVED HERE, never imported. NAMED NAMESPACE, not anonymous.
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
	 * The grounded island: 20 bricks. Sized so that a cap-44 greedy flood, forced to admit all 20
	 * standing bricks, has too few blocks left to reach the stack's base — while grow-on-contact,
	 * ignoring the island, spends the whole 44 on the 30-block stack with headroom to spare.
	 */
	constexpr int32 IslandBricks = 20;

	/** The region cap the flood is driven above. Set with 14 blocks of headroom over the 30-block
	 *  stack, so a correct grow-on-contact reaches the whole mechanism even carrying the island's
	 *  modest seed presence, yet greedy (which admits the whole island) stops at course 7. */
	constexpr int32 RegionCap = 44;

	/** Router-only baseline: a tiny gate cap keeps the entry's SolveLoads off the whole-structure LP. */
	constexpr int32 GateCap = 5;

	struct FFixture
	{
		FStructure Structure;
		TArray<FPieceBox> Boxes;
		int32 StackTop = INDEX_NONE;   /* piece 29 — the mechanism seed (the free top). */
		int32 IslandFirst = INDEX_NONE;/* piece 30 — the standing-island seed. */
		TArray<int32> IslandPieces;    /* the grounded standing island (never in the truth). */
	};

	void Build(FFixture& F)
	{
		/* The leaning stack: course 0 grounded, each course offset 10 cm past the one below. */
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
		 * The grounded island: a straight -X run of GROUNDED bricks at Z = half a brick, far enough in
		 * -X (starting at X = -500) that no face ever meets the stack — MakeInterface writes no joint
		 * between the two components, so the island is a genuine disconnected foundation.
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

	/* FIXTURE PRECONDITIONS. */
	TestEqual(TEXT("FIXTURE: 30 stack bricks + 20 island bricks"),
		F.Structure.NumPieces(), Courses + IslandBricks);
	TestEqual(TEXT("FIXTURE: 29 bed joints in the stack + 19 head joints in the island, no cross joint"),
		F.Structure.NumConnections(), (Courses - 1) + (IslandBricks - 1));
	TestTrue(TEXT("FIXTURE: complete geometry"), F.Structure.HasCompleteGeometry());
	TestEqual(TEXT("FIXTURE: stack top is piece 29"), F.StackTop, Courses - 1);
	TestEqual(TEXT("FIXTURE: island starts at piece 30"), F.IslandFirst, Courses);

	/* ================================================================================
	 * (A) THE INDEPENDENT TRUTH — the whole-structure ground-only LP, a DIFFERENT code path.
	 * It fells every stack course above the grounded base, {1..29}, and stands the island.
	 * ================================================================================ */
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

	/* ================================================================================
	 * (B) THE CAP-SUFFICIENCY WITNESS — the SAME flood seeded with ONLY the stack top, at the SAME
	 * cap. The disconnected island is never reached, so this is a stack-only flood: it fells the whole
	 * {1..29} in 30 posed blocks. Proof that cap 44 is not the limit — the island is the sole reason
	 * the island-seeded greedy flood stops short. This arm is green today and stays green after grow.
	 * ================================================================================ */
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

	/* ================================================================================
	 * (C) THE NEW BEHAVIOUR (RED UNTIL BUILT). Seed BOTH the stack top and the standing island, at the
	 * same cap. Grow-on-contact must ignore the grounded island and recover the full mechanism reach:
	 * fell the whole {1..29}, release exactly 29, fell no island piece, strand nothing.
	 *
	 * TODAY (greedy fill-to-cap): the flood admits all 20 island bricks and fells only {7..29} (23),
	 * so pieces {1..6} read Supported — the strict-superset gap this drives.
	 * ================================================================================ */
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

	/* SOUNDNESS: every felled piece is one the whole-structure LP also fells (subset of truth). */
	for (const int32 Piece : IslandFalling)
	{
		TestTrue(
			*FString::Printf(TEXT("SOUNDNESS: felled piece %d is inside the whole-structure LP truth"),
				Piece),
			TruthMoving.Contains(Piece));
	}

	/* The grounded island is a genuine foundation — grow must never fell it. */
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
