// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"
#include "Core/WallCases.h"
#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE BLOCK-SIZE THRESHOLD FOR SIZE-SCOPED LP AUTHORITY (PROMOTION_DESIGN §12 D2⁗/D2‴,
 * §5.6; DESIGN §8's 2026-08-18 "scope LP authority by structure size" ruling). This file
 * writes NO production code — it is a MEASUREMENT, and under this project's convention a
 * measurement lands as a PINNED ROW whose unmeasured state is its red (PROMOTION_DESIGN §6).
 *
 * THE QUESTION D2⁗ LEFT OPEN, and it is the last number the ruling rests on:
 *
 *     Below what structure size does the DELETION-CAUSED re-solve production actually runs
 *     — a piece deleted, the neighbourhood re-posed as FEASIBILITY at λ = 1 — answer inside
 *     the ~50 ms per-solve / ~100 ms per-action budget, so full cascade authority can be
 *     promoted below it and the two-tier router kept above it?
 *
 * D2⁗ cites "small structures already re-solve in ~51 ms" as the fact that makes the
 * promotion real where it lands. That 51 ms is one point; this file measures the CURVE it
 * sits on and reads the crossings off it. The ruling is stated in wall-clock (100 ms), but
 * wall-clock is NOT bit-reproducible and this machine varies ~5% under load, so the
 * BITE-ABLE contract is the DETERMINISTIC pair §5.6 says a fail-closed budget is actually
 * expressed in — PIVOTS and PROBLEM SIZE (rows × columns) — with the ms table beside it as
 * the informative-but-unpinned reading, and the derived threshold stated in both.
 *
 * ================================================================================
 * WHAT IS MEASURED, AND WHY EACH CHOICE IS THE PRODUCTION-RELEVANT ONE
 * ================================================================================
 *
 * THE DELETION-CAUSED RE-SOLVE, NOT THE INTACT SOLVE. §5.2 measured the intact λ* solve;
 * production, per player action, DELETES a piece and re-solves the neighbourhood, and R1
 * cites ~51 ms for exactly that. So every rung here lays an intact wall, removes ONE brick,
 * and times the solve that FOLLOWS the removal — the bridge skips the removed piece, so the
 * re-solve is genuinely the reduced problem.
 *
 * THE FEASIBILITY POSE (bGravityIsLive = false), NOT λ*. §5.2 is the crux of the whole
 * promotion: cascade authority asks "does it still stand", which is FEASIBILITY at λ = 1,
 * not the maximise. The dead pose is the 5–16× faster one production would actually run
 * (§5.2's table), so it is the production-relevant number. The λ* (gravity-live) pose is
 * NOT re-measured across this ladder — it is tens of seconds at these sizes (wall-06 at 146
 * blocks is 16.1 s live in the spike table) and its live-vs-dead contrast is already pinned
 * in OracleSweepFull.RigidBlock.FeasibilityReformulationCost. Re-measuring it here would
 * turn a ~15 s benchmark into a multi-minute one to re-establish a contrast already in hand.
 *
 * A MID-SPAN, MID-HEIGHT LOAD-BEARING BRICK, AND WHICH BRICK IS CONTROLLED FOR. The rule is
 * stated and deterministic: over the intact wall's live pieces take the centroid bounding
 * box, and delete the NON-GROUNDED piece nearest its centre (lowest index breaks a tie).
 * That is the common, interesting case — an interior brick a standing wall arches over, so
 * the re-solve comes back FEASIBLE, which every rung asserts (a rung that came back
 * infeasible would mean the deletion COLLAPSED that wall, a different measurement). WHICH
 * brick can matter, so it is controlled rather than assumed: the anchor rung also deletes an
 * EDGE mid-height brick (nearest min-X) and pins its pivot count beside the mid-span one, so
 * the sensitivity is a measurement, not a hope. See the CONTRAST block at the bottom.
 *
 * PROBLEM SIZE IS READ FROM THE SOLVER'S OWN OUTPUT, and cross-checked independently. An
 * answered FOracleResult reports FinalBasis, which carries one column index per standard-form
 * ROW (so Columns.Num() is the row count), NumStructCols and ArtificialStart. Total columns
 * = ArtificialStart + rows (one artificial per row). Structural columns are independently
 * 1 + 8·joints (RigidBlockOracle.h: 1 λ column + 4 per contact × 2 contacts per joint), which
 * this file DERIVES and asserts against NumStructCols — two derivations of one number, so the
 * size pin cannot quietly agree with a wrong assembly.
 *
 * ================================================================================
 * PREDICTIONS — DERIVATION RECORD REVISION 1, WRITTEN BEFORE THE FIRST RUN
 * (PROMOTION_DESIGN §7.3: a prediction written after the measurement is a transcription.)
 * ================================================================================
 *
 * THE LADDER. Running-bond C×Cells walls, bottom course grounded. An even course carries
 * `Cells` full bricks; an odd course carries Cells−1 full bricks + 2 half-bats = Cells+1
 * pieces, so a pair of courses is 2·Cells+1 and a C-course wall (C even) is C·Cells + C/2
 * blocks. The 8×10 gate fixture is 8·10 + 4 = 84, matching PricingCost. Post-deletion is one
 * fewer. The rungs bracket both sides of where the crossing is expected (below 84 → above):
 *
 *     rung     intact blocks   post-deletion blocks (WantBlocks)
 *     4×10          42              41
 *     6×10          63              62
 *     8×10          84              83   (the gate fixture, D2⁗'s ~51 ms anchor)
 *    10×10         105             104
 *    12×10         126             125
 *    12×12         150             149   (the sub-1.0 / repaired-sandwich fixture size)
 *    14×12         175             174
 *    16×12         200             199
 *
 * The far side above 375 blocks is NOT laddered here: the crossing is expected near 84
 * blocks (D2⁗'s own anchor), wall-01's 375-block feasibility answer is 26 s (§5.2, three
 * orders over budget), and re-measuring the far tail would cost minutes to re-state a number
 * already pinned. If measurement puts the crossing ABOVE 200 blocks the ladder is extended;
 * that would itself be a finding against D2⁗'s ~51 ms anchor.
 *
 * PREDICTED PIVOTS. The dead pose starts INFEASIBLE (gravity is a constant RHS) and phase 1
 * must drive out one artificial per equality row — 3 rows per non-grounded block — so pivots
 * have a floor of ~3·(blocks − Cells). The spike measured the multiplier at 2.2× that floor
 * on the 84-block wall and ~5× at 375, i.e. it GROWS with scale, so the prediction interpolates
 * k from ~2.1 (small) to ~3.0 (200 blocks). PREDICTED PIVOTS, which the pins below assert and
 * which are therefore RED until measured (an exact 4-digit pivot count will not equal a guess):
 *
 *     rung   post-del blocks   non-grounded   3·ng    k     PREDICTED pivots
 *     4×10        41               31           93    2.1        195
 *     6×10        62               52          156    2.15       335
 *     8×10        83               73          219    2.2        482
 *    10×10       104               94          282    2.35       660
 *    12×10       125              115          345    2.5        860
 *    12×12       149              137          411    2.6       1070
 *    14×12       174              162          486    2.8       1360
 *    16×12       199              187          561    3.0       1680
 *
 * PREDICTED WALL-CLOCK, anchored on D2⁗'s ~51 ms at 84 blocks and steepening because
 * per-pivot cost grows with basis size (BTRAN/refactorisation over a larger basis), so ms
 * climbs FASTER than pivots:
 *
 *     rung    PREDICTED ms
 *     4×10        13
 *     6×10        28
 *     8×10        51   (anchor)
 *    10×10        82
 *    12×10       125
 *    12×12       185
 *    14×12       270
 *    16×12       370
 *
 * PREDICTED THRESHOLD: the 50 ms crossing sits at ~84 blocks / ~490 pivots (the gate fixture,
 * by construction of D2⁗'s anchor); the 100 ms crossing at ~115 blocks / ~760 pivots. So the
 * predicted headline is: full LP authority is promotable below ~84–115 blocks, and the router
 * is kept above it — which is BELOW the 12×12 sub-1.0 fixture and far below wall-01.
 *
 * ================================================================================
 * WHAT WAS MEASURED — 2026-08-21, one machine, unloaded. The deterministic columns are
 * now PINNED at these bits; the ms columns are REPORTED and unpinned (median of 7 samples).
 * ================================================================================
 *
 *   rung   blocks  joints  PIVOTS   rows  structCols  totalCols   median ms
 *   4x10      41      83      122   1090       665        2752         5.2
 *   6x10      62     142      248   1861      1137        4703        20.7
 *   8x10      83     201      357   2632      1609        6654        47.1   <- ~D2⁗ 51 ms
 *  10x10     104     260      552   3403      2081        8605       101.7
 *  12x10     125     319      744   4174      2553       10556       225.9
 *  12x12     149     385      873   5032      3081       12734       306.8
 *  14x12     174     456     1274   5959      3649       15081       744.5
 *  16x12     199     527     1456   6886      4217       17428       904.8
 *
 * THE DERIVED THRESHOLD (reported deterministically, ms beside it as the flapping reading):
 *
 *   - 50 ms PER-SOLVE (the design's target, so two solves fit a 100 ms action): the median
 *     crosses between 83 blocks / 357 pivots (47.1 ms) and 104 blocks / 552 pivots (101.7 ms)
 *     -> ~84 blocks / ~367 pivots. So under the "two solves per action" budget, LP authority
 *     is promotable BELOW ~84 blocks — exactly the gate-fixture size D2⁗'s ~51 ms anchor names.
 *   - 100 ms PER-ACTION when an action is ONE re-solve: crosses in the SAME 83->104 interval
 *     -> ~103 blocks / ~546 pivots.
 *
 *   Both crossings land in one ladder interval because the ms curve is STEEP there (47 ms at
 *   83 blocks doubles to 102 ms by 104): the 50 ms and 100 ms thresholds are only ~20 blocks
 *   apart. The promotable band is therefore ~84–104 blocks / ~360–550 pivots — comfortably
 *   below the 12×12 sub-1.0 fixture (149) and three-plus fold below wall-01 (375) and
 *   scenario scale (1220). The crossing is INSIDE the ladder, so no rung had to be added.
 *
 * FOUR DISAGREEMENTS AGAINST THE PREDICTIONS, which are the point of writing them first:
 *
 *   - PIVOTS CAME IN LOW EVERYWHERE (357 vs 482 predicted at 83 blocks; 1456 vs 1680 at 199).
 *     The prediction anchored the multiplier on the INTACT 84-block solve (491 pivots), but a
 *     mid-span deletion removes a brick AND its ~6 joints, so the reduced problem has fewer
 *     contacts, fewer rows and a lower phase-1 floor. The deletion re-solve is CHEAPER in
 *     pivots than the intact solve the estimate was built on — a real property of the thing
 *     production runs, not the thing §5.2 measured.
 *   - WALL-CLOCK CLIMBS FAR STEEPER THAN PIVOTS. Pivots grow 11.9× across the ladder
 *     (122 -> 1456) while ms grows 174× (5.2 -> 904.8). Per-pivot cost rises with basis size
 *     (rows grow 6.3×, and BTRAN/refactorisation over a denser basis is super-linear in rows),
 *     so a deterministic budget expressed in PIVOTS is not a fixed multiple of a ms budget —
 *     the pivot cap that holds 50 ms at 84 blocks (~360) would badly overshoot 50 ms at 150.
 *     A fail-closed deterministic budget (§5.6) is therefore safest stated as a BLOCK cap, or
 *     as a pivot cap re-derived per size band, not one global pivot number.
 *   - THE 100 ms CROSSING IS LOWER THAN PREDICTED (~103 blocks vs ~115), for the same reason:
 *     ms steepens faster than the prediction's per-pivot model assumed.
 *   - THE MID-SPAN 8×10 RE-SOLVE IS 47 ms, slightly UNDER D2⁗'s ~51 ms anchor and under the
 *     spike's 66 ms INTACT 84-block feasibility solve — consistent: the deletion makes the
 *     problem smaller, and this machine's ~5% variance covers the rest.
 *
 * WHICH BRICK (the CONTRAST rung): deleting the EDGE mid-height brick of the 8×10 wall costs
 * 381 pivots against the mid-span brick's 357 — ~7% more, from a SMALLER graph change (204
 * joints survive vs 201). So which brick is deleted matters modestly and measurably; the
 * ladder controls for it by using one rule, so the size trend is not the which-brick effect
 * in disguise.
 *
 * ================================================================================
 * WHAT IS PINNED AND WHAT IS DELIBERATELY NOT
 * ================================================================================
 *
 * PINNED EXACTLY (deterministic, bit-reproducible, the fail-closed budget's own vocabulary):
 * per-rung post-deletion block count, joint count, feasibility-pose PIVOTS, rows, structural
 * columns and total columns; the independent structural-column identity 1 + 8·joints; the
 * feasibility verdict (answered and λ ≥ 1). The edge-vs-mid-span contrast pivot counts are
 * pinned too.
 *
 * NOT PINNED: WALL-CLOCK MS. It is not bit-reproducible; it measures the machine, which
 * varies ~5% under load. Every millisecond is REPORTED with min/median across samples and
 * NONE is asserted, with one exception stated at its site: a single order-of-magnitude
 * ceiling on the anchor rung set ~30× above the reading so it can only fire on a catastrophe.
 * The derived threshold is stated in DETERMINISTIC terms (block count / pivot count) with the
 * ms table beside it as the unpinned reading — so a machine 20% slower moves the ms table and
 * the STATED crossing, never a pinned assertion.
 *
 * TIER: `OracleSweepFull` (folded in on review, 2026-08-21). Although this is a latency
 * benchmark and only its wall-clock ms is the headline reading, its PINNED quantities are the
 * deterministic pivots/rows/columns of the deletion re-solve at every ladder rung — those are
 * solver regression guards at scale (up to 199 blocks / 6,886 rows / 1,456 pivots), and a
 * pivot-count drift IS a solver change. OracleSweepFull is the tier CLAUDE.md makes mandatory
 * before any solver-touching commit, so it is the only bucket that guarantees these guards fire
 * on the exact change they exist to catch; an orphan stem in no bucket is the "opt-in tier
 * rots" footgun TRAPS records (it once silently skipped a solver-scale test). Not in the
 * default suite (the name contains no "DestructionGame"). Cost is ~15 s (feasibility pose,
 * <= 200 blocks, 7 timing samples per rung) — negligible against Full's ~22 min. The single ms
 * assertion is a loose 1.5 s catastrophe ceiling on a ~49 ms solve, so it cannot flake in CI;
 * every other reading is deterministic. The runner guard is Full = 5 / All = 15 with a
 * $FullBuckets entry; CLAUDE.md and TRAPS carry the same counts.
 *
 * NEEDS A TICKING WORLD: NO. Producers, the bridge and the LP are arithmetic on plain structs
 * — no UWorld, no tick.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges files into one translation unit and
 * every file-scope name shares it (TRAPS). Every constant here carries a Ladder prefix.
 */
namespace OracleDeletionLatencySupport
{
	using namespace DestructionProfiles;
	using namespace RigidBlockOracle;

	/* ================================================================================
	 * THE BRICK AND THE GRID — transcribed, imported from nowhere, so a wrong production
	 * constant DISAGREES with this file instead of being echoed by it.
	 * ================================================================================ */

	constexpr double LadderBrickLengthCm = 21.5;
	constexpr double LadderBrickWidthCm = 10.25;
	constexpr double LadderBrickHeightCm = 6.5;
	constexpr double LadderClayDensityGramsPerCubicCm = 1.9;
	constexpr double LadderJointCm = 1.0;

	/** How many times each rung's feasibility re-solve is timed. Pivots are identical every
	 *  run; only the wall-clock varies, so a handful of samples gives a min and a median. */
	constexpr int32 LadderTimingSamples = 7;

	/** "Nobody has measured this yet" — the sentinel a size pin carries until the red run
	 *  fills it. The solver can never emit a negative pivot/row/column count, so it is loud. */
	constexpr int32 LadderUnmeasured = -2;

	/* ================================================================================
	 * FIXTURE BUILDER — the acceptance wall producer, transcribed (its helpers live in
	 * another translation unit) with the gate fixture's own dimensions.
	 * ================================================================================ */

	bool LadderBuildIntactWall(
		int32 Courses, int32 Cells, DestructionWallCases::FWallLayout& Out, FString& OutWhy)
	{
		using namespace DestructionWallCases;

		FWallSpec Spec;
		Spec.BrickSizeCm = FVector(LadderBrickLengthCm, LadderBrickWidthCm, LadderBrickHeightCm);
		Spec.JointThicknessCm = LadderJointCm;
		Spec.DensityGramsPerCubicCm = LadderClayDensityGramsPerCubicCm;
		Spec.CoursesHigh = Courses;
		Spec.Cells = Cells;
		Spec.Bond = EWallBond::Running;
		Spec.Strength = GeneralPurposeMortar;

		if (!Build(Spec, Out))
		{
			OutWhy = FString::Printf(
				TEXT("the wall producer refused %d courses x %d cells"), Courses, Cells);
			return false;
		}

		return true;
	}

	/* ================================================================================
	 * WHICH BRICK — a stated, deterministic rule, independent of running-bond parity.
	 * ================================================================================ */

	/**
	 * The non-grounded live piece nearest a target point in the X-Z plane (Y is the wythe).
	 * A strict-less update makes the lowest index win a tie, so the choice is bit-stable.
	 */
	int32 LadderNearestLoadBearing(const FStructure& S, double TargetXCm, double TargetZCm)
	{
		int32 Best = INDEX_NONE;
		double BestDistSq = TNumericLimits<double>::Max();

		for (int32 I = 0; I < S.NumPieces(); ++I)
		{
			if (S.IsPieceRemoved(I))
			{
				continue;
			}

			const FStructurePiece& P = S.GetPiece(I);

			if (P.bIsGrounded)
			{
				continue;
			}

			const double Dx = P.CentreOfMassCm.X - TargetXCm;
			const double Dz = P.CentreOfMassCm.Z - TargetZCm;
			const double DistSq = Dx * Dx + Dz * Dz;

			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				Best = I;
			}
		}

		return Best;
	}

	/** The centroid bounding box over the wall's live pieces. */
	void LadderCentroidBounds(
		const FStructure& S,
		double& OutMinX, double& OutMaxX, double& OutMinZ, double& OutMaxZ)
	{
		OutMinX = OutMinZ = TNumericLimits<double>::Max();
		OutMaxX = OutMaxZ = -TNumericLimits<double>::Max();

		for (int32 I = 0; I < S.NumPieces(); ++I)
		{
			if (S.IsPieceRemoved(I))
			{
				continue;
			}

			const FVector& C = S.GetPiece(I).CentreOfMassCm;

			OutMinX = FMath::Min(OutMinX, C.X);
			OutMaxX = FMath::Max(OutMaxX, C.X);
			OutMinZ = FMath::Min(OutMinZ, C.Z);
			OutMaxZ = FMath::Max(OutMaxZ, C.Z);
		}
	}

	/** The mid-span, mid-height load-bearing brick: nearest the centroid box's centre. */
	int32 LadderSelectMidSpan(const FStructure& S)
	{
		double MinX, MaxX, MinZ, MaxZ;
		LadderCentroidBounds(S, MinX, MaxX, MinZ, MaxZ);

		return LadderNearestLoadBearing(S, 0.5 * (MinX + MaxX), 0.5 * (MinZ + MaxZ));
	}

	/** The CONTRAST brick: mid-height but at the wall's left end (nearest min-X, mid-Z). */
	int32 LadderSelectEdge(const FStructure& S)
	{
		double MinX, MaxX, MinZ, MaxZ;
		LadderCentroidBounds(S, MinX, MaxX, MinZ, MaxZ);

		return LadderNearestLoadBearing(S, MinX, 0.5 * (MinZ + MaxZ));
	}

	/* ================================================================================
	 * ONE DELETION-CAUSED FEASIBILITY RE-SOLVE, TIMED.
	 * ================================================================================ */

	struct FResolveReading
	{
		bool bBuilt = false;
		FString Why;

		int32 Blocks = 0;
		int32 Joints = 0;

		bool bAnswered = false;
		double Lambda = 0.0;

		/* Deterministic, pinned. */
		int32 Pivots = 0;
		int32 Rows = 0;
		int32 StructCols = 0;
		int32 TotalCols = 0;

		/* Reported only. */
		double BestSeconds = 0.0;
		double MedianSeconds = 0.0;
	};

	/**
	 * Delete piece DeletePiece from a freshly-laid C×Cells wall, pose the reduced structure
	 * as FEASIBILITY (gravity dead), and solve it LadderTimingSamples times — pivots identical
	 * every time, wall-clock sampled for a min and a median.
	 */
	FResolveReading LadderResolveAfterDeletion(int32 Courses, int32 Cells, bool bEdgeBrick)
	{
		FResolveReading Out;

		DestructionWallCases::FWallLayout Laid;

		if (!LadderBuildIntactWall(Courses, Cells, Laid, Out.Why))
		{
			return Out;
		}

		FStructure& Structure = Laid.Layout.Structure;

		const int32 Victim =
			bEdgeBrick ? LadderSelectEdge(Structure) : LadderSelectMidSpan(Structure);

		if (Victim == INDEX_NONE)
		{
			Out.Why = TEXT("no non-grounded brick to delete");
			return Out;
		}

		if (!Structure.RemovePiece(Victim))
		{
			Out.Why = FString::Printf(TEXT("could not remove piece %d"), Victim);
			return Out;
		}

		FOracleProblem Problem;

		if (!BuildRigidBlockProblem(Structure, Problem, Out.Why))
		{
			return Out;
		}

		/* THE FEASIBILITY POSE — the production-relevant re-solve, gravity a constant RHS. */
		Problem.bGravityIsLive = false;

		Out.Blocks = Problem.Blocks.Num();
		Out.Joints = Problem.Joints.Num();

		TArray<double> Samples;
		Samples.Reserve(LadderTimingSamples);

		FOracleResult Result;

		for (int32 Sample = 0; Sample < LadderTimingSamples; ++Sample)
		{
			const double Started = FPlatformTime::Seconds();
			Result = SolveRigidBlock(Problem);
			Samples.Add(FPlatformTime::Seconds() - Started);
		}

		Out.bBuilt = true;
		Out.bAnswered = Result.bAnswered;
		Out.Lambda = Result.Lambda;
		Out.Pivots = Result.SimplexIterations;
		Out.Rows = Result.FinalBasis.Columns.Num();
		Out.StructCols = Result.FinalBasis.NumStructCols;
		Out.TotalCols = Result.FinalBasis.ArtificialStart + Out.Rows;

		Samples.Sort();
		Out.BestSeconds = Samples[0];
		Out.MedianSeconds = Samples[Samples.Num() / 2];

		return Out;
	}
}

/**
 * THE LADDER. Each rung lays a wall, deletes its mid-span mid-height brick, and times the
 * feasibility re-solve; the deterministic quantities are pinned and the ms reported. The red
 * is the predicted-pivot pins (wrong until measured) and the sentinel size pins.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleDeletionResolveLatencyLadderTest,
	"OracleSweepFull.RigidBlock.DeletionResolveLatencyLadder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleDeletionResolveLatencyLadderTest::RunTest(const FString& Parameters)
{
	using namespace OracleDeletionLatencySupport;

	struct FRung
	{
		const TCHAR* Name = nullptr;
		int32 Courses = 0;
		int32 Cells = 0;

		/* Derived from the producer's own bond rule — should PASS, proving the harness. */
		int32 WantBlocks = 0;

		/*
		 * PREDICTED pivots (red until measured) and the SENTINEL size pins. Every non-block
		 * Want is a prediction or a sentinel, so the red is "the number is wrong/unmeasured",
		 * never a broken harness — the block pin and the feasibility verdict passing are the
		 * evidence the harness reached the right reduced problem.
		 */
		int32 WantPivots = 0;
		int32 WantJoints = 0;
		int32 WantRows = 0;
		int32 WantStructCols = 0;
		int32 WantTotalCols = 0;
	};

	/*
	 * MEASURED 2026-08-21, RED RUN's own numbers (this machine, unloaded). WantBlocks was
	 * derived and passed on the red run, proving the harness reached the right reduced
	 * problem; every other Want was a prediction or a sentinel and is now the measured bit.
	 *          {  name,    C, Cells, blocks, pivots, joints,  rows, structCols, totalCols }
	 */
	FRung Rungs[] =
	{
		{ TEXT("4x10"),  4, 10,  41,  122,  83, 1090,  665,  2752 },
		{ TEXT("6x10"),  6, 10,  62,  248, 142, 1861, 1137,  4703 },
		{ TEXT("8x10"),  8, 10,  83,  357, 201, 2632, 1609,  6654 },
		{ TEXT("10x10"), 10, 10, 104,  552, 260, 3403, 2081,  8605 },
		{ TEXT("12x10"), 12, 10, 125,  744, 319, 4174, 2553, 10556 },
		{ TEXT("12x12"), 12, 12, 149,  873, 385, 5032, 3081, 12734 },
		{ TEXT("14x12"), 14, 12, 174, 1274, 456, 5959, 3649, 15081 },
		{ TEXT("16x12"), 16, 12, 199, 1456, 527, 6886, 4217, 17428 },
	};

	constexpr int32 NumRungs = UE_ARRAY_COUNT(Rungs);

	/* For the threshold derivation: (blocks, pivots, median ms) per answered rung. */
	TArray<int32> CrossBlocks;
	TArray<int32> CrossPivots;
	TArray<double> CrossMillis;

	for (int32 Index = 0; Index < NumRungs; ++Index)
	{
		const FRung& Rung = Rungs[Index];

		const FResolveReading R =
			LadderResolveAfterDeletion(Rung.Courses, Rung.Cells, /*bEdgeBrick*/ false);

		if (!TestTrue(
				*FString::Printf(TEXT("%s: the reduced problem must build and solve (it said: %s)"),
					Rung.Name, *R.Why),
				R.bBuilt))
		{
			continue;
		}

		const double MedianMs = R.MedianSeconds * 1000.0;
		const double BestMs = R.BestSeconds * 1000.0;

		const FString Line = FString::Printf(
			TEXT("LADDER %-5s: blocks=%d joints=%d answered=%d lambda=%.17g pivots=%d ")
			TEXT("rows=%d structCols=%d totalCols=%d  ms(best=%.1f median=%.1f)"),
			Rung.Name, R.Blocks, R.Joints, R.bAnswered ? 1 : 0, R.Lambda, R.Pivots,
			R.Rows, R.StructCols, R.TotalCols, BestMs, MedianMs);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);

		/* THE DELETION MUST LEAVE THE WALL STANDING — the representative "survives" case. */
		TestTrue(
			*FString::Printf(TEXT("%s: the re-solve must ANSWER (a refusal is not an infeasibility)"),
				Rung.Name),
			R.bAnswered);

		TestTrue(
			*FString::Printf(
				TEXT("%s: the wall must still stand with one mid-span brick out (feasible, ")
				TEXT("lambda=%.17g >= 1) — an infeasible rung would be measuring collapse, not a ")
				TEXT("survived deletion"),
				Rung.Name, R.Lambda),
			R.bAnswered && R.Lambda >= 1.0);

		/* THE SIZE PIN: only this catches a rung quietly solving another's wall. */
		TestEqual(
			*FString::Printf(TEXT("%s: post-deletion block count"), Rung.Name),
			R.Blocks, Rung.WantBlocks);

		TestEqual(
			*FString::Printf(TEXT("%s: post-deletion joint count"), Rung.Name),
			R.Joints, Rung.WantJoints);

		/* INDEPENDENT SIZE CHECK: structural columns are 1 + 8*joints, derived two ways. */
		TestEqual(
			*FString::Printf(
				TEXT("%s: structural columns must be 1 + 8*joints = %d (an independent ")
				TEXT("derivation of the problem width)"),
				Rung.Name, 1 + 8 * R.Joints),
			R.StructCols, 1 + 8 * R.Joints);

		/* THE DETERMINISTIC LATENCY VOCABULARY — pivots and problem size, pinned. */
		TestEqual(
			*FString::Printf(TEXT("%s: feasibility-pose deletion re-solve PIVOTS"), Rung.Name),
			R.Pivots, Rung.WantPivots);

		TestEqual(
			*FString::Printf(TEXT("%s: standard-form ROWS"), Rung.Name),
			R.Rows, Rung.WantRows);

		TestEqual(
			*FString::Printf(TEXT("%s: structural COLUMNS"), Rung.Name),
			R.StructCols, Rung.WantStructCols);

		TestEqual(
			*FString::Printf(TEXT("%s: total COLUMNS"), Rung.Name),
			R.TotalCols, Rung.WantTotalCols);

		if (R.bAnswered)
		{
			CrossBlocks.Add(R.Blocks);
			CrossPivots.Add(R.Pivots);
			CrossMillis.Add(MedianMs);
		}
	}

	/* ================================================================================
	 * THE DERIVED THRESHOLD — reported in DETERMINISTIC terms (block / pivot count) with
	 * the ms table beside it. NOT a pinned assertion: it rests on wall-clock, which flaps.
	 * ================================================================================ */

	auto ReportCrossing = [&](double BudgetMs)
	{
		int32 CrossedIndex = INDEX_NONE;

		for (int32 I = 0; I < CrossMillis.Num(); ++I)
		{
			if (CrossMillis[I] > BudgetMs)
			{
				CrossedIndex = I;
				break;
			}
		}

		if (CrossedIndex == INDEX_NONE)
		{
			AddInfo(FString::Printf(
				TEXT("THRESHOLD @ %.0f ms: every laddered rung is UNDER budget (largest is ")
				TEXT("%d blocks / %d pivots at %.1f ms) — the crossing is above the ladder"),
				BudgetMs,
				CrossBlocks.Num() ? CrossBlocks.Last() : 0,
				CrossPivots.Num() ? CrossPivots.Last() : 0,
				CrossMillis.Num() ? CrossMillis.Last() : 0.0));
			return;
		}

		if (CrossedIndex == 0)
		{
			AddInfo(FString::Printf(
				TEXT("THRESHOLD @ %.0f ms: even the smallest rung (%d blocks / %d pivots) is ")
				TEXT("OVER budget at %.1f ms — the crossing is below the ladder"),
				BudgetMs, CrossBlocks[0], CrossPivots[0], CrossMillis[0]));
			return;
		}

		/* Linear interpolation between the last under-budget rung and the first over it. */
		const int32 Lo = CrossedIndex - 1;
		const int32 Hi = CrossedIndex;
		const double T =
			(BudgetMs - CrossMillis[Lo]) / (CrossMillis[Hi] - CrossMillis[Lo]);
		const double Blocks = CrossBlocks[Lo] + T * (CrossBlocks[Hi] - CrossBlocks[Lo]);
		const double Pivots = CrossPivots[Lo] + T * (CrossPivots[Hi] - CrossPivots[Lo]);

		AddInfo(FString::Printf(
			TEXT("THRESHOLD @ %.0f ms: crosses between %d blocks (%.1f ms) and %d blocks ")
			TEXT("(%.1f ms) -> ~%.0f blocks / ~%.0f pivots"),
			BudgetMs, CrossBlocks[Lo], CrossMillis[Lo], CrossBlocks[Hi], CrossMillis[Hi],
			Blocks, Pivots));
	};

	ReportCrossing(50.0);
	ReportCrossing(100.0);

	/* ================================================================================
	 * CONTRAST — WHICH BRICK MATTERS. Same 8×10 gate wall, an EDGE mid-height brick out
	 * instead of the mid-span one. Pivots pinned; the difference (or its absence) is the
	 * control on the mid-span rule the ladder is built on.
	 * ================================================================================ */
	{
		const FResolveReading Edge =
			LadderResolveAfterDeletion(8, 10, /*bEdgeBrick*/ true);

		constexpr int32 WantEdgeBlocks = 83;

		/*
		 * MEASURED 2026-08-21: 381, against the mid-span brick's 357 — the edge brick has
		 * fewer neighbours (204 joints survive vs 201), so its deletion is a SMALLER change
		 * to the graph yet costs ~7% MORE pivots. Which brick is deleted matters modestly and
		 * measurably; the ladder holds it constant with the mid-span rule so the size trend is
		 * not confounded by it.
		 */
		constexpr int32 WantEdgePivots = 381;

		if (TestTrue(
				*FString::Printf(TEXT("edge contrast: the reduced problem must build (it said: %s)"),
					*Edge.Why),
				Edge.bBuilt))
		{
			const FString Line = FString::Printf(
				TEXT("CONTRAST 8x10 EDGE brick out: blocks=%d joints=%d answered=%d ")
				TEXT("lambda=%.17g pivots=%d  ms(best=%.1f median=%.1f)"),
				Edge.Blocks, Edge.Joints, Edge.bAnswered ? 1 : 0, Edge.Lambda, Edge.Pivots,
				Edge.BestSeconds * 1000.0, Edge.MedianSeconds * 1000.0);

			UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
			AddInfo(Line);

			TestEqual(TEXT("edge contrast: post-deletion block count"),
				Edge.Blocks, WantEdgeBlocks);

			TestEqual(TEXT("edge contrast: feasibility-pose PIVOTS (which brick can matter)"),
				Edge.Pivots, WantEdgePivots);

			/*
			 * A LOOSE CATASTROPHE CEILING, the file's ONE ms assertion and the only one,
			 * set ~30x above D2⁗'s ~51 ms anchor so it fires only on a pathological slowdown,
			 * never on the ~5% this machine varies. The gate rung's own ms is reported and
			 * unpinned; THIS is a guard against the whole harness silently hanging.
			 */
			TestTrue(
				*FString::Printf(
					TEXT("edge contrast: the 83-block re-solve must finish under a 1.5 s ")
					TEXT("catastrophe ceiling (best was %.1f ms) — the ONLY ms assertion here"),
					Edge.BestSeconds * 1000.0),
				Edge.BestSeconds < 1.5);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
