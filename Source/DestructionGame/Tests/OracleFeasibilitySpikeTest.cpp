// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"
#include "Core/WallCases.h"
#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"
#include "World/DestructionScenarios.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * SLICE 0b — the latency spike, a hard gate (PROMOTION_DESIGN.md §6, §11 R1/R2; user
 * ruling D2, 2026-08-14). Measurement only, no production code; a measurement lands as a
 * pinned row whose unmeasured state is its red.
 *
 * THE QUESTION: can an 84-block region answer in ~50 ms — two solves inside a 100 ms
 * budget? If not, synchronous LP authority at scenario scale is off the table. A fired
 * kill criterion is a successful measurement; nothing here is softened to make it pass.
 *
 * ================================================================================
 * MEASUREMENT 1 — DOES ABANDONING lambda* PAY, AND BY HOW MUCH?
 * ================================================================================
 *
 * The live pose maximises lambda ("how many times its own weight") and starts trivially
 * feasible (every equality RHS is zero, lambda = 0 solves it), so phase 1 never runs and
 * the whole cost is phase 2 climbing to lambda*. wall-01: 58,605 pivots, ~252 s.
 *
 * Production instead wants feasibility at lambda = 1. Posing dead
 * (FOracleProblem::bGravityIsLive = false) puts gravity into the equality rows as a
 * constant so phase 1 genuinely runs. Worked from the assembly:
 *
 *   - Gravity goes to DeadZ, RowFz.Rhs = +W per block, phase 1 starts infeasible and
 *     drives out one artificial per equality row (3 per non-grounded block).
 *   - The lambda column then has no equilibrium-row entry, only the cap row
 *     lambda <= LambdaCap. Phase 2 has nothing to climb: feasible reports lambda exactly
 *     LambdaCap, infeasible returns the dead-load arm, bAnswered = true with lambda 0.
 *   - Both exact values are pinned per row: a live load leaking into a dead pose would
 *     land lambda between them and fire.
 *
 * THE CROSS-VALIDATION makes this a measurement, not a timing run: the feasibility
 * boolean must agree with the same run's lambda* verdict (feasible at lambda = 1 iff
 * lambda* >= 1). The fixture list carries both arms — the 30-course leaning stack is
 * lambda* = 0.44 and must come back infeasible — so the boolean is not vacuous (TRAPS'
 * shared-hidden-property trap).
 *
 * THE EARLY EXIT. §3.2 and §5.2 claim a feasible problem is proved the moment phase 1's
 * infeasibility sum reaches tolerance, and put most of a standing structure's saving
 * there. RunRevisedSimplex ran phase 1 to optimality and read the sum only after, hiding
 * that. FOracleResult::PhaseOnePivots and PivotsToFirstFeasible are now set during the
 * solve, observation only — every lambda*, pivot and scan count came back bit-identical.
 * Both are pinned exactly per row.
 *
 * ================================================================================
 * MEASUREMENT 2 — THE REGIONAL SANDWICH
 * ================================================================================
 *
 * The roadmap's "a deleted brick cannot affect masonry beyond a bonded distance" is
 * false here (PROMOTION_DESIGN §5.3). The alternative is a two-sided sandwich:
 *
 *   - boundary blocks pinned grounded (artificial foundation) makes the region
 *     optimistic: infeasible here means the whole structure is infeasible;
 *   - boundary blocks free makes it pessimistic: feasible here means the whole is;
 *   - agreement certifies; disagreement grows the region.
 *
 * The region extractor is test-side (as the case-21 probes are): FOracleProblem is a
 * plain struct of public arrays, so a region is a filter plus one flag per boundary
 * block, needing nothing from RigidBlockOracle — keeping the oracle independently derived.
 *
 * ================================================================================
 * PREDICTIONS — derivation record revision 1, written before the first run
 * (PROMOTION_DESIGN §7.3: a prediction written after the measurement is a transcription).
 * ================================================================================
 *
 * MEASUREMENT 1. Phase 1 pivots out one artificial per equality row (3 per non-grounded
 * block), so its pivot count floors at ~3 x blocks; phase 2's climb has no such floor.
 * Prediction: the dead pose costs ~O(rows) pivots, and the speedup is large where the
 * lambda* climb was long.
 *
 *     fixture           blocks   lambda* pose (pinned)   PREDICTED dead-pose pivots
 *     leaning stack 5        5        (fast suite)                     10 -  40
 *     leaning stack 30      30   0.4405, infeasible arm               100 - 400
 *     8x10 intact wall      84   1,942 pv / 0.61 s                    400 - 800
 *     corbel D              90   8,439 pv / ~9.0 s                    400 - 900
 *     wall-18              119     791 pv / 1.3 s                     400 - 900
 *     wall-15              125   3,745 pv / 4.9 s                     500 - 1,200
 *     wall-06              146   8,819 pv / ~127 s (Dantzig era)      600 - 1,500
 *     wall-01              375  58,605 pv / ~252 s                  1,500 - 4,000
 *
 * PREDICTED SPEEDUP: ~3-5x on the 84-block wall, ~10-20x on corbel D, ~1x on wall-18 (its
 * 791-pivot climb is near the phase-1 floor), ~15-40x on wall-01. Not the design's
 * 10-100x: "a few x on small standing structures, an order or more only where the climb
 * was long".
 *
 * PREDICTED GATE ARITHMETIC: 0.61 s / 4 ~ 0.15 s for the 84-block wall against a ~50 ms
 * target — the kill fires on the feasibility lever alone, with ~3x still owed to warm
 * starts and per-iteration levers (PROMOTION_DESIGN §5.5 estimates ~1 order together), so
 * the gate is predicted recoverable but not met today.
 *
 * MEASUREMENT 2 — arithmetic, not a guess. Sum the vertical equilibrium rows of a
 * free-boundary region: internal contact forces appear twice with opposite signs and
 * cancel, joints leaving the region are dropped, leaving 0 = -(region weight). No
 * solution for positive weight unless the region contains a grounded block (which writes
 * no equilibrium row, so its contacts do not cancel). Therefore:
 *
 *     PREDICTED: the pessimistic side is infeasible for every region not reaching the
 *     ground, at any radius; it can only certify a ground-connected region.
 *     CONSEQUENCE: for a brick deleted k courses up, the sandwich closes at the first
 *     radius R >= k and no smaller.
 *     R2 ANSWER: over ten deletions at courses 1..10 with radius 4, the sides agree on
 *     four of ten (courses 1-4).
 *
 * So the sandwich degenerates to "grow to the ground" (full height for a 30-course wall),
 * not the design's "agree at a small radius". Which, with a number, is this test's job.
 *
 * ================================================================================
 * WHAT WAS MEASURED — 2026-08-15, one run each, tree at HEAD 9dcbd76
 * ================================================================================
 *
 * MEASUREMENT 1. Every dead-pose pivot count landed in range except corbel D (1,022 vs
 * 400-900) and wall-01 (5,407 vs 1,500-4,000), both high. Speedups, live over dead:
 *
 *     fixture           blocks   live pv / s     dead pv / s     pivots   seconds
 *     leaning stack 5        5        4 / 0.000      22 / 0.000    0.18x        —
 *     leaning stack 30      30       29 / 0.006     163 / 0.006    0.18x    0.99x
 *     8x10 intact wall      84    2,606 / 0.749     491 / 0.065    5.31x    11.5x
 *     corbel D    (t)       90    9,490 / 10.21   1,022 / 0.274    9.29x    37.3x
 *     wall-18     (t)      119    3,885 / 2.765     468 / 0.062    8.30x    44.6x
 *     wall-15     (t)      125    6,076 / 3.819     803 / 0.204    7.57x    18.7x
 *     wall-06     (t)      146   14,209 / 16.10     876 / 0.314   16.22x    51.2x
 *     wall-01              375   58,806 / 280.5   5,407 / 25.99   10.88x    10.8x
 *
 * (t) = trimmed 2026-08-16. These four no longer re-solve the live pose (its lambda* is
 * pinned in OracleSweepFull.RigidBlock.WallsAndLadders, solve identical), so their live
 * columns are the 2026-08-15 record and their speedup is cross-run. Dead columns are
 * measured every run.
 *
 * FOUR THINGS THE PREDICTION MISSED:
 *
 *   - The seconds ratio beats the pivot ratio everywhere but wall-01, by up to 5x
 *     (wall-18: 8.3x pivots, 44.6x time). A phase-1 pivot is cheaper than a phase-2 one —
 *     unexplained, worth an attribution slice before budgeting on the pivot ratio.
 *   - The lever weakens with scale: dead pivots are 2.2x the equality-row count at 84
 *     blocks, ~5x at 375, so phase 1 is not O(rows) with a small constant and wall-01's
 *     10.9x is the smallest large-fixture speedup.
 *   - Small fixtures go the other way: the 5- and 30-course stacks cost 5.5x more dead
 *     pivots (22 vs 4, 163 vs 29). Where the climb is short, phase 1 is the expensive
 *     half — the 10-100x estimate is refuted in both directions.
 *   - Proving infeasibility is not the expensive direction: the 30-course stack (the one
 *     infeasible fixture) answers in 163 pivots / 0.006 s, same as its lambda* solve.
 *     §3.2's "standing cheap, falling expensive" is not what this shows.
 *
 * MEASUREMENT 1b — THE EARLY EXIT, §5.2's LARGEST REMAINING CLAIM REFUTED. §3.2 promised
 * feasibility can stop when the infeasibility sum reaches zero while infeasibility needs
 * phase 1 to optimality; §5.2 put most of the saving there. The design's prediction of
 * record: large.
 *
 * MEASURED, dead pose, phase-1 pivots / first pivot reading feasible (INDEX_NONE = never,
 * as an infeasible problem must report):
 *
 *     fixture           phase 1   first feasible   skipped   share
 *     leaning stack 5        21               21         0    0.00%
 *     leaning stack 30      163           never -1         —        —
 *     8x10 gate wall        490              487         3    0.61%
 *     corbel D            1,021            1,021         0    0.00%
 *     wall-18               467              449        18    3.85%
 *     wall-15               802              799         3    0.37%
 *     wall-06               875              866         9    1.03%
 *     wall-01             5,406            5,381        25    0.46%
 *     ---------------------------------------------------------------
 *     POOLED              9,245                —        58    0.63%
 *
 * **0.63% pooled, 3.85% worst.** Structural, not a bad fixture list: phase 1's objective
 * IS the infeasibility sum, so "sum reaches tolerance" and "phase 1 optimal" coincide by
 * construction — no optimality tail to skip, only a few degenerate pivots after the last
 * artificial zeroes. §3.2's early-stop is true and worth nothing; the saving is spent.
 *
 * TWO SUPPORTING FACTS, each closing an escape:
 *
 *   - The phase-1 gate is already loose: InfeasibilityTolerance() is (1 + largest basic)
 *     x 1e-9 x rows, ~13 at wall-01 scale. A stricter gate only moves first-feasibility
 *     later, so 0.63% is an upper bound on this lever.
 *   - The residue these counters miss is the pricer's closing full sweep (column scans,
 *     not pivots), bounded by pricing's runtime share — 0.5% on wall-01 — so not hiding
 *     the lever either.
 *
 * All eight live poses report 0/0: feasible before a single pivot, phase 1 never runs.
 * Asserted per row, like the LambdaCap-or-0 pin, to catch a live load leaking into a
 * dead pose.
 *
 * MEASUREMENT 2. The conservation prediction held exactly, and everything followed:
 *
 *     12-course x 12-cell wall, one brick out of course 6, 149 blocks / 385 joints
 *     global feasibility: FEASIBLE, 965 pivots, 0.324 s
 *
 *     radius   region blocks   grounded-boundary   free-boundary   closes
 *          2              44           feasible      INFEASIBLE        no
 *          4             114           feasible      INFEASIBLE        no
 *          5             142           feasible        feasible       YES
 *          6             149           feasible        feasible       yes
 *          8             149           feasible        feasible       yes
 *
 * Closing radius 5, region 142 of 149 blocks (95%). The free-boundary side read
 * infeasible at every region with no grounded block and feasible at every one with, no
 * exception, as the vertical-equilibrium sum requires.
 *
 * R2's answer: five of ten deletions certify at radius 4 (courses 1-5, whose region
 * reaches the foundation). Predicted four; the extra is the shell reaching one course
 * beyond the core. Courses 6-10 do not certify short of the ground.
 *
 * And the cost goes the wrong way: at the closing radius the two solves take
 * 753 + 872 = 1,625 pivots where the whole wall takes 965 — 1.68x the work it avoids.
 *
 * The collapse arm is worse: on the 30-course stack the optimistic region is still
 * feasible at radius 13 (29 of 30) and certifies only at 14, the whole structure. No
 * proper subset certifies.
 *
 * And the pessimistic side is not a bound (Part D). The stack's bottom band (courses
 * 0..5, grounded, 24 courses dropped) is feasible free- and grounded-boundary, so the
 * sandwich certifies "full structure feasible" about a stack whose lambda* is 0.4405.
 * Dropping material removes its weight with its restraint, so §5.3 does not hold and a
 * closed sandwich can be a false certificate — as specified, unsound.
 *
 * Not a knife edge. The certified band's lambda* is **18.481256459924058** against the
 * stack's 0.44048 — **41.96x** the load under which the certified structure has no
 * equilibrium. No extractor tightening rescues this; only carrying the omitted weight
 * does (the repair §12 D2' rules for). (Review independently priced the band at ~18.7
 * from a (N-1)^-2.29 fit, 1.2% from the solve.)
 *
 * PART B ALSO CHECKS ITS FIVE CERTIFICATES against the whole cut wall's verdict, one
 * 149-block dead solve per deletion (added 2026-08-15). All five hold.
 *
 * ================================================================================
 * THE GATE VERDICT
 * ================================================================================
 *
 * R1 (one solve): the 84-block fixture answers in 0.065 s against a ~0.050 s target — a
 * 1.3x gap, not the design's stated 12x, with warm starts unbuilt. **R1 does not fire.**
 *
 * R2 (regional decomposition): **it fires.** The sandwich certifies only when its region
 * reaches the ground, closes at 95-100% of the structure, costs 1.68x a global solve, and
 * its pessimistic side can certify a structure with no equilibrium. Regional decomposition
 * is required for scenario scale (wall-01's 375 blocks answer in 26 s, scenario scale
 * ~3.3x that), so R2 is the one that changes the design's shape.
 *
 * ================================================================================
 * WHAT IS PINNED AND WHAT IS NOT
 * ================================================================================
 *
 * PINNED EXACTLY: block and joint counts per fixture (the N10 rung-flip lesson: a window
 * cannot catch a row solving the wrong problem), pivot counts (bit-deterministic), the
 * feasibility verdict, the exact pose lambda (LambdaCap or 0), the early exit's two pivot
 * indices and pooled totals, region block counts, the closing radius, the agreement count.
 *
 * ALSO ASSERTED ON EVERY SOLVE: that it answered. A refusal is not an infeasibility, and
 * SpikeIsFeasible cannot tell them apart — Refuse() returns bAnswered = false, lambda = 0,
 * bit-for-bit the dead-load infeasibility arm. Without this a region solve hitting
 * NumericalFailure (the arm that refused 128-/200-block members a commit ago) would read
 * as "pessimistic side infeasible", this file's headline shape. Not corrupted today (every
 * region pivot count 7-965, every WhyNot empty), but nothing else was defending it.
 *
 * PINNED IN A WINDOW: live-pose lambda*, at the file-wide +/-2e-5 relative, so a moved
 * lambda* fails here too.
 *
 * **NOT PINNED: WALL-CLOCK TIME.** Timings measure the machine, not the solver. Every
 * second is reported, none asserted, with one exception at its assertion: an
 * order-of-magnitude ceiling on the gate fixture, ~30x above the measurement, firing only
 * on a catastrophe. Pivot ratios are pinned; second ratios are not.
 *
 * COST: these four tests are opt-in, split 2026-08-16 into `OracleSweepFast` (80 s,
 * iteration) and `OracleSweepFull` (3 tests, 20.8 min, mandatory before any commit
 * touching the LP oracle). The cost test is full; the sandwiches and sub-1.0 wall are
 * fast. `Automation RunTests OracleSweep` runs both by substring (filters return 7 / 3 /
 * 10 tests). MEASURED: cost test 344 s, **313 s since the 2026-08-16 live-pose trim**;
 * sandwich 7.2 s, repaired 9.6 s, **sub-1.0 wall 8.0 s**. All 10 green since 2026-08-15;
 * no deliberate red today (the early-exit seam was a red hand-off, now measured and
 * pinned). Default suite re-runs at 173 = 167 green + 6 deliberate reds.
 *
 * GREEN ON ARRIVAL IS NOT ASSERTING NOTHING (§9.5: a row invalidating a published design
 * section may not rest on an argued bite). Every conclusion has a recorded mutation,
 * TRAPS' registry X1-X10:
 *
 *     X1  the ladder deletes from course 5 instead of 6      ->  7 assertions
 *     X2  the gate fixture built 8x11 instead of 8x10        ->  5 assertions
 *     X3  PhaseOnePivots reported as the constant 0          ->  the phase-1 pins
 *     X4  the feasibility watch disabled                     ->  the first-feasible pins
 *     X5  the watch records at pivot 1 unconditionally       -> 10 assertions, and the
 *                                                               ORDERING rows stay green
 *     X6  the extractor keeps the out-of-region blocks       -> 14 assertions, four of
 *                                                               them PART D's; RE-MEASURED
 *                                                               against the repaired
 *                                                               extractor 2026-08-15 and
 *                                                               still 14 here, plus 12 in
 *                                                               RepairedRegionalSandwich
 *     X7  a region solve forced to REFUSE                    ->  1 assertion, and before
 *                                                               the guard landed, ZERO
 *     X8  the surcharge applied at the CONTACT's X instead   ->  3 here + 4 in
 *         of the charged component's centre of gravity           RepairedRegionalSandwich,
 *                                                               and it RE-CREATES THE FALSE
 *                                                               CERTIFICATE: band 0..5
 *                                                               certifies feasible against
 *                                                               an infeasible stack. The
 *                                                               third is the LINE-OF-ACTION
 *                                                               pin: 175 cm derived, 55
 *                                                               measured
 *     X9  the carried set computed as empty (repair (1)      ->  5 here + 8 in
 *         removed, its plumbing left in place)                   RepairedRegionalSandwich,
 *                                                               3 false certificates
 *    X10  the Carried rule made to charge EVERY component    ->  0 here, 7 in
 *                                                               RepairedRegionalSandwich —
 *                                                               the Charged pins' prover
 *    X11  the sub-1.0 wall's chimney built with NO LEAN      ->  SubUnityWallCertificate's
 *         (SpikeChimneyLeanCm 10.0 -> 0.0), which leaves         fixture rows and its
 *         every block count, joint count and region size         false-certificate count;
 *         identical and makes the structure STAND               signature in the registry
 *
 * X5, X6 and X7 exist because the early-exit numbers, Part D's booleans and every region
 * verdict were where a wrong-but-plausible answer would have walked through.
 *
 * NEEDS A TICKING WORLD: no. Producers, bridge and LP are arithmetic on plain structs.
 *
 * NAMED NAMESPACE, not anonymous, and every constant carries a Spike prefix: a unity
 * build merges files into one translation unit, sharing every file-scope name (TRAPS).
 */
namespace OracleFeasibilitySpikeSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace RigidBlockOracle;

	/* ================================================================================
	 * THE BRICK AND THE GRID — derived here, imported from nowhere, so a wrong production
	 * constant disagrees with this file instead of being echoed by it.
	 * ================================================================================ */

	constexpr double SpikeBrickLengthCm = 21.5;
	constexpr double SpikeBrickWidthCm = 10.25;
	constexpr double SpikeBrickHeightCm = 6.5;
	constexpr double SpikeClayDensityGramsPerCubicCm = 1.9;
	constexpr double SpikeJointCm = 1.0;

	/** Course pitch and cell pitch on the coordinating grid: 7.5 cm and 22.5 cm. */
	constexpr double SpikeCoursePitchCm = SpikeBrickHeightCm + SpikeJointCm;
	constexpr double SpikeCellPitchCm = SpikeBrickLengthCm + SpikeJointCm;

	/** Density-first multiplication order — the PieceMassKg contract; 2.72163125 kg. */
	constexpr double SpikeBrickMassKg = SpikeClayDensityGramsPerCubicCm
		* SpikeBrickLengthCm * SpikeBrickWidthCm * SpikeBrickHeightCm / 1000.0;

	/**
	 * Slack outside an exact grid multiple for radius membership. Half bats sit at a
	 * half-cell offset, so an exact-multiple window would include or exclude them on a
	 * rounding; 1 cm is far below the 22.5 cm it discriminates and above centroid noise.
	 */
	constexpr double SpikeRadiusSlackCm = 1.0;

	/** The file-wide relative window on a certified lambda*, per the sweep's discipline. */
	constexpr double SpikeLambdaRelativeWindow = 2.0e-5;

	/**
	 * The unmeasured sentinel for the one pin whose measured values include INDEX_NONE.
	 * PivotsToFirstFeasible reports INDEX_NONE as a genuine reading (an infeasible problem
	 * never reaches feasibility), so its unmeasured state needs a value the solver cannot produce.
	 */
	constexpr int32 SpikeUnmeasured = -2;

	/* ================================================================================
	 * FIXTURE BUILDERS — production's own producers, transcribed because the sweep file's
	 * helpers live in its own translation unit.
	 * ================================================================================ */

	/** Lay a scenario row's structure and apply its cut — production data end to end. */
	bool SpikeBuildScenario(const TCHAR* ScenarioName, FStructure& Out, FString& OutWhy)
	{
		using namespace DestructionScenarios;

		const int32 Index = IndexOfName(FName(ScenarioName));

		if (Index == INDEX_NONE)
		{
			OutWhy = FString::Printf(TEXT("no scenario row named %s"), ScenarioName);
			return false;
		}

		FBrickLayout Layout;
		TArray<int32> CutPieces;

		if (!Build(Catalogue()[Index], Layout, CutPieces))
		{
			OutWhy = FString::Printf(TEXT("the producer refused %s"), ScenarioName);
			return false;
		}

		for (const int32 Piece : CutPieces)
		{
			if (!Layout.Structure.RemovePiece(Piece))
			{
				OutWhy = FString::Printf(
					TEXT("%s: cut piece %d could not be removed"), ScenarioName, Piece);
				return false;
			}
		}

		Out = MoveTemp(Layout.Structure);
		return true;
	}

	/**
	 * WALL-01, shrunk: the acceptance producer at 8 x 10 = 84 blocks, the fixture
	 * DestructionGame.Oracle.RigidBlock.PricingCost budgets. The 84-block gate wall.
	 */
	bool SpikeBuildIntactWall(int32 Courses, int32 Cells, FStructure& Out, FString& OutWhy)
	{
		DestructionWallCases::FWallSpec Spec;
		Spec.BrickSizeCm = FVector(SpikeBrickLengthCm, SpikeBrickWidthCm, SpikeBrickHeightCm);
		Spec.JointThicknessCm = SpikeJointCm;
		Spec.DensityGramsPerCubicCm = SpikeClayDensityGramsPerCubicCm;
		Spec.CoursesHigh = Courses;
		Spec.Cells = Cells;
		Spec.Bond = DestructionWallCases::EWallBond::Running;
		Spec.Strength = GeneralPurposeMortar;

		DestructionWallCases::FWallLayout Wall;

		if (!DestructionWallCases::Build(Spec, Wall))
		{
			OutWhy = FString::Printf(
				TEXT("the wall producer refused %d courses x %d cells"), Courses, Cells);
			return false;
		}

		Out = MoveTemp(Wall.Layout.Structure);
		return true;
	}

	/**
	 * The leaning-stack acceptance fixture, from LeaningStackAcceptanceTest: course i at
	 * (10*i, 0, 3.25 + 7.5*i), base grounded, mortared via MakeInterface with a 1 cm bed.
	 * The only cheap fixture the LP prices below 1.0 — the infeasible arm of the cross-validation.
	 */
	bool SpikeBuildLeaningStack(int32 Courses, FStructure& Out, FString& OutWhy)
	{
		TArray<FPieceBox> Boxes;

		for (int32 Course = 0; Course < Courses; ++Course)
		{
			FPieceBox Box;
			Box.ExtentCm =
				FVector(SpikeBrickLengthCm, SpikeBrickWidthCm, SpikeBrickHeightCm) * 0.5;
			Box.CentreCm = FVector(
				double(Course) * 10.0, 0.0,
				SpikeBrickHeightCm / 2.0 + double(Course) * SpikeCoursePitchCm);

			Out.AddPiece(SpikeBrickMassKg, /*bIsGrounded*/ Course == 0, Box.CentreCm);
			Boxes.Add(Box);
		}

		for (int32 First = 0; First < Boxes.Num(); ++First)
		{
			for (int32 Second = First + 1; Second < Boxes.Num(); ++Second)
			{
				FConnection Joint;

				if (MakeInterface(First, Boxes[First], Second, Boxes[Second],
						SpikeJointCm, GeneralPurposeMortar, Joint))
				{
					Out.AddConnection(Joint);
				}
			}
		}

		if (Out.NumConnections() != Courses - 1)
		{
			OutWhy = FString::Printf(
				TEXT("stack fixture emitted %d joints for %d courses"),
				Out.NumConnections(), Courses);
			return false;
		}

		return true;
	}

	/* ================================================================================
	 * ONE SOLVE, BOTH POSES.
	 * ================================================================================ */

	struct FPoseReading
	{
		bool bAnswered = false;
		double Lambda = 0.0;
		int32 Pivots = 0;
		int64 Scans = 0;
		double Seconds = 0.0;
		FString WhyNot;

		/*
		 * The early exit's two readings, so every fixture reports what stopping at feasibility
		 * would have saved. Both pinned per row: "refuted at 0.63%" is a conclusion, and a
		 * conclusion nothing asserts can be inverted by a pricing or tolerance change while
		 * every test stays green.
		 */
		int32 PhaseOnePivots = INDEX_NONE;
		int32 PivotsToFirstFeasible = INDEX_NONE;
	};

	FPoseReading SpikeSolve(const FOracleProblem& Problem)
	{
		FPoseReading Out;

		const double Started = FPlatformTime::Seconds();
		const FOracleResult Result = SolveRigidBlock(Problem);
		Out.Seconds = FPlatformTime::Seconds() - Started;

		Out.bAnswered = Result.bAnswered;
		Out.Lambda = Result.Lambda;
		Out.Pivots = Result.SimplexIterations;
		Out.Scans = Result.PricingColumnScans;
		Out.WhyNot = Result.WhyNot;
		Out.PhaseOnePivots = Result.PhaseOnePivots;
		Out.PivotsToFirstFeasible = Result.PivotsToFirstFeasible;

		return Out;
	}

	/**
	 * The feasibility verdict, read as production would: answered, and lambda >= 1. In the
	 * dead pose lambda is only LambdaCap (feasible) or 0 (infeasibility arm), so it is a
	 * boolean in a double's clothes — which the rows assert directly.
	 */
	bool SpikeIsFeasible(const FPoseReading& Reading)
	{
		return Reading.bAnswered && Reading.Lambda >= 1.0;
	}

	/* ================================================================================
	 * THE REGION EXTRACTOR — test-side, one flag apart between the two boundary conditions
	 * so the sandwich's two sides cannot differ in anything else.
	 * ================================================================================ */

	struct FRegionCounts
	{
		int32 Core = 0;
		int32 Shell = 0;
		int32 Blocks = 0;
		int32 Joints = 0;

		/** Blocks in the region that are the EARTH — the only reaction a region can have. */
		int32 Grounded = 0;

		/*
		 * Membership and remap, published so the surcharge uses the SAME membership the
		 * region was cut with. Recomputing "core or shell" would be a second derivation of
		 * the one thing the two sandwich sides must agree about, and a divergence would look
		 * like physics.
		 */
		TArray<bool> bInRegion;

		/** Full-problem block index -> region block index; INDEX_NONE for an omitted block. */
		TArray<int32> Remap;
	};

	/**
	 * Cut a region out of a whole-structure problem. CORE is the caller's mask; SHELL is
	 * every block outside it sharing a joint with it — the boundary, and the only thing the
	 * two sandwich sides disagree about:
	 *
	 *   - bGroundTheShell TRUE  -> shell blocks become earth (OPTIMISTIC: artificial
	 *     foundation all round);
	 *   - bGroundTheShell FALSE -> shell blocks keep their grounded flag, writing
	 *     equilibrium rows and carrying their own weight (PESSIMISTIC).
	 *
	 * Joints leaving the region are dropped, as is a joint between two grounded blocks (it
	 * constrains nothing, as BuildRigidBlockProblem drops it). Output is posed DEAD.
	 */
	void SpikeExtractRegion(
		const FOracleProblem& Full,
		const TArray<bool>& bInCore,
		bool bGroundTheShell,
		FOracleProblem& Out,
		FRegionCounts& Counts)
	{
		Out = FOracleProblem();
		Counts = FRegionCounts();

		const int32 NumBlocks = Full.Blocks.Num();

		TArray<bool> bIsShell;
		bIsShell.Init(false, NumBlocks);

		for (const FOracleJoint& Joint : Full.Joints)
		{
			const bool bA = bInCore[Joint.BlockA];
			const bool bB = bInCore[Joint.BlockB];

			if (bA && !bB)
			{
				bIsShell[Joint.BlockB] = true;
			}
			else if (bB && !bA)
			{
				bIsShell[Joint.BlockA] = true;
			}
		}

		TArray<int32> Remap;
		Remap.Init(INDEX_NONE, NumBlocks);

		for (int32 Block = 0; Block < NumBlocks; ++Block)
		{
			if (!bInCore[Block] && !bIsShell[Block])
			{
				continue;
			}

			FOracleBlock Copy = Full.Blocks[Block];

			if (bIsShell[Block] && bGroundTheShell)
			{
				Copy.bGrounded = true;
			}

			Remap[Block] = Out.Blocks.Num();
			Out.Blocks.Add(Copy);

			if (bInCore[Block])
			{
				++Counts.Core;
			}
			else
			{
				++Counts.Shell;
			}

			if (Copy.bGrounded)
			{
				++Counts.Grounded;
			}
		}

		for (const FOracleJoint& Joint : Full.Joints)
		{
			const int32 A = Remap[Joint.BlockA];
			const int32 B = Remap[Joint.BlockB];

			if (A == INDEX_NONE || B == INDEX_NONE)
			{
				continue;
			}

			if (Out.Blocks[A].bGrounded && Out.Blocks[B].bGrounded)
			{
				continue;
			}

			FOracleJoint Copy = Joint;
			Copy.BlockA = A;
			Copy.BlockB = B;
			Out.Joints.Add(Copy);
		}

		Out.bGravityIsLive = false;

		Counts.Blocks = Out.Blocks.Num();
		Counts.Joints = Out.Joints.Num();

		Counts.bInRegion.Init(false, NumBlocks);

		for (int32 Block = 0; Block < NumBlocks; ++Block)
		{
			Counts.bInRegion[Block] = bInCore[Block] || bIsShell[Block];
		}

		Counts.Remap = MoveTemp(Remap);
	}

	/** A box mask on the coordinating grid: R courses up and down, R cells left and right. */
	void SpikeBoxMask(
		const FOracleProblem& Full,
		double CentreXCm,
		double CentreZCm,
		int32 RadiusCourses,
		int32 RadiusCells,
		TArray<bool>& OutMask)
	{
		OutMask.Init(false, Full.Blocks.Num());

		const double ReachZ = double(RadiusCourses) * SpikeCoursePitchCm + SpikeRadiusSlackCm;
		const double ReachX = double(RadiusCells) * SpikeCellPitchCm + SpikeRadiusSlackCm;

		for (int32 Block = 0; Block < Full.Blocks.Num(); ++Block)
		{
			const FOracleBlock& B = Full.Blocks[Block];

			OutMask[Block] = FMath::Abs(B.CentroidZCm - CentreZCm) <= ReachZ
				&& FMath::Abs(B.CentroidXCm - CentreXCm) <= ReachX;
		}
	}

	/** A chain mask for the leaning stack: every block within R courses of a height. */
	void SpikeCourseBandMask(
		const FOracleProblem& Full,
		double CentreZCm,
		int32 RadiusCourses,
		TArray<bool>& OutMask)
	{
		OutMask.Init(false, Full.Blocks.Num());

		const double ReachZ = double(RadiusCourses) * SpikeCoursePitchCm + SpikeRadiusSlackCm;

		for (int32 Block = 0; Block < Full.Blocks.Num(); ++Block)
		{
			OutMask[Block] =
				FMath::Abs(Full.Blocks[Block].CentroidZCm - CentreZCm) <= ReachZ;
		}
	}

	/* ================================================================================
	 * REPAIR (1) — the omitted material's weight, kept as a dead surcharge.
	 *
	 * The refuted form dropped an omitted block's restraint and weight together, and the
	 * weight mattered: the stack's 24 omitted courses carried the destabilising moment, so
	 * deleting them turned a no-equilibrium structure into a short stack that stands
	 * (lambda* 18.481 vs 0.44048, wrong by 41.96x, Part D). The repair keeps the demand and
	 * still refuses the restraint: an omitted block bearing on the region contributes its
	 * weight as a dead applied force, writing no equilibrium row and holding nothing down.
	 *
	 * "Bears on the region" must be right both ways — charge too little and the false
	 * certificate survives; charge everything and a strip is asked to carry the whole
	 * building, so the pessimistic side never certifies:
	 *
	 *   - material whose only road to the ground runs through the region: charged.
	 *   - material on its own foundation, reaching the earth without the region: not
	 *     charged. Every wall has this and the leaning stack does not, so "omitted weight"
	 *     cannot mean "all of it".
	 *
	 * The rule, no geometry: delete the region from the joint graph; an omitted block whose
	 * remaining component has no grounded block is carried by the region, and every block in
	 * that component is charged — reachability on the graph, from nothing the LP knows.
	 *
	 * WHERE THE LOAD IS APPLIED is what the false certificate turns on. Each component's
	 * weight is delivered at its interface joints, shared by area, but on the vertical line
	 * through the component's centre of gravity, not the contact's X. A vertical force's
	 * moment is independent of the Z of its application point (torque = Rx*Fz - Rz*Fx,
	 * Fx = 0), so one vertical line reproduces the total force and moment exactly. Applying
	 * at the contact centre would drop the overturning moment behind the 41.96x error: the
	 * omitted courses weigh on a line at X = 175 cm while their contact sits at X = 55.
	 *
	 * Pessimistic side only. The optimistic side's soundness is "restrict an admissible
	 * force system to a grounded-shell region and it stays admissible" — a surcharge row
	 * there would break the one half that survived review.
	 *
	 * KNOWN SCOPE LIMIT: exact when omitted material is wholly carried or wholly
	 * self-supporting. It under-charges material with its own ground path that also bears on
	 * the region (a partial-height strip in a laterally-connected wall, whose top courses
	 * reach the ground down the columns either side) — which is why the wall regions below
	 * are full height by construction.
	 * ================================================================================ */

	enum class ESpikeSurcharge : uint8
	{
		/** The refuted form: omitted material contributes nothing at all. */
		None,

		/** Only what the region carries — the component-reaches-no-ground rule above. */
		Carried,

		/**
		 * Every omitted block, whatever it stands on. Deliberately too pessimistic, a control
		 * so "charging everything never certifies" is measured, not argued.
		 */
		EveryOmittedBlock
	};

	struct FSurchargeCounts
	{
		int32 ChargedBlocks = 0;
		int32 ChargedComponents = 0;
		int32 InterfaceJoints = 0;

		/**
		 * A charged component sharing no joint with the region: its weight has nowhere to go
		 * and is silently lost, the refuted form by the back door. Asserted zero.
		 */
		int32 OrphanComponents = 0;

		double ChargedWeightUu = 0.0;
		double AppliedWeightUu = 0.0;

		/**
		 * Surcharge delivered onto a GROUNDED region block. A grounded block writes no
		 * equilibrium row, so the assembly discards the force silently — as fatal as an
		 * orphan component. Asserted zero.
		 */
		double DiscardedOntoGroundedUu = 0.0;
	};

	void SpikeAddSurcharge(
		const FOracleProblem& Full,
		const FRegionCounts& Region,
		ESpikeSurcharge Rule,
		FOracleProblem& InOut,
		FSurchargeCounts& Out)
	{
		Out = FSurchargeCounts();

		if (Rule == ESpikeSurcharge::None)
		{
			return;
		}

		const int32 NumBlocks = Full.Blocks.Num();

		/*
		 * Adjacency among OMITTED blocks only. The region is deleted here, so every joint
		 * with an end inside it is gone — which makes "can this block reach the ground" mean
		 * "without the region's help".
		 */
		TArray<TArray<int32>> Adjacency;
		Adjacency.SetNum(NumBlocks);

		for (const FOracleJoint& Joint : Full.Joints)
		{
			if (Region.bInRegion[Joint.BlockA] || Region.bInRegion[Joint.BlockB])
			{
				continue;
			}

			Adjacency[Joint.BlockA].Add(Joint.BlockB);
			Adjacency[Joint.BlockB].Add(Joint.BlockA);
		}

		TArray<int32> Component;
		Component.Init(INDEX_NONE, NumBlocks);

		TArray<TArray<int32>> Members;

		for (int32 Seed = 0; Seed < NumBlocks; ++Seed)
		{
			if (Region.bInRegion[Seed] || Component[Seed] != INDEX_NONE)
			{
				continue;
			}

			const int32 Label = Members.Num();
			Members.AddDefaulted();
			Component[Seed] = Label;

			TArray<int32> Pending;
			Pending.Add(Seed);

			while (Pending.Num() > 0)
			{
				const int32 At = Pending.Pop();
				Members[Label].Add(At);

				for (const int32 Next : Adjacency[At])
				{
					if (Component[Next] == INDEX_NONE)
					{
						Component[Next] = Label;
						Pending.Add(Next);
					}
				}
			}
		}

		for (int32 Label = 0; Label < Members.Num(); ++Label)
		{
			bool bReachesGround = false;

			for (const int32 Block : Members[Label])
			{
				bReachesGround = bReachesGround || Full.Blocks[Block].bGrounded;
			}

			if (Rule == ESpikeSurcharge::Carried && bReachesGround)
			{
				continue;
			}

			double WeightUu = 0.0;
			double WeightedXCm = 0.0;

			for (const int32 Block : Members[Label])
			{
				const double W =
					Full.Blocks[Block].MassKg * OracleGravityCmPerSecondSquared;

				WeightUu += W;
				WeightedXCm += W * Full.Blocks[Block].CentroidXCm;
			}

			if (!(WeightUu > 0.0))
			{
				continue;
			}

			const double GravityLineXCm = WeightedXCm / WeightUu;

			++Out.ChargedComponents;
			Out.ChargedBlocks += Members[Label].Num();
			Out.ChargedWeightUu += WeightUu;

			/* The contacts where this component meets the region, in joint index order. */
			TArray<int32> Interface;
			double TotalAreaSqCm = 0.0;

			for (int32 J = 0; J < Full.Joints.Num(); ++J)
			{
				const FOracleJoint& Joint = Full.Joints[J];

				const bool bAIn = Region.bInRegion[Joint.BlockA];
				const bool bBIn = Region.bInRegion[Joint.BlockB];

				const bool bTouches =
					(bAIn && !bBIn && Component[Joint.BlockB] == Label)
					|| (bBIn && !bAIn && Component[Joint.BlockA] == Label);

				if (bTouches)
				{
					Interface.Add(J);
					TotalAreaSqCm += Joint.AreaSqCm;
				}
			}

			if (Interface.Num() == 0 || !(TotalAreaSqCm > 0.0))
			{
				++Out.OrphanComponents;
				continue;
			}

			Out.InterfaceJoints += Interface.Num();

			for (const int32 J : Interface)
			{
				const FOracleJoint& Joint = Full.Joints[J];

				const int32 RegionSide =
					Region.bInRegion[Joint.BlockA] ? Joint.BlockA : Joint.BlockB;

				const int32 Target = Region.Remap[RegionSide];
				const double ShareUu = WeightUu * (Joint.AreaSqCm / TotalAreaSqCm);

				FOracleAppliedForce Applied;
				Applied.Block = Target;
				Applied.ForceXUu = 0.0;
				Applied.ForceZUu = -ShareUu;
				Applied.AtXCm = GravityLineXCm;
				Applied.AtZCm = Joint.CentreZCm;
				Applied.bLive = false;

				InOut.AppliedForces.Add(Applied);
				Out.AppliedWeightUu += ShareUu;

				if (InOut.Blocks[Target].bGrounded)
				{
					Out.DiscardedOntoGroundedUu += ShareUu;
				}
			}
		}
	}

	/** The repaired extractor: the same cut, plus repair (1) on the pessimistic side only. */
	void SpikeExtractRepairedRegion(
		const FOracleProblem& Full,
		const TArray<bool>& bInCore,
		bool bGroundTheShell,
		ESpikeSurcharge Rule,
		FOracleProblem& Out,
		FRegionCounts& Counts,
		FSurchargeCounts& Surcharge)
	{
		SpikeExtractRegion(Full, bInCore, bGroundTheShell, Out, Counts);

		Surcharge = FSurchargeCounts();

		if (bGroundTheShell)
		{
			return;
		}

		SpikeAddSurcharge(Full, Counts, Rule, Out, Surcharge);
	}

	/* ================================================================================
	 * REPAIR (2) — regions that reach the ground by construction.
	 *
	 * The free side is provably infeasible for any region with no grounded block (sum its
	 * vertical rows: contacts cancel, leaving joints are dropped, 0 = -W_region). So the
	 * growth parameter cannot be a radius — a ball reaches the foundation only by accident,
	 * which is why the refuted form certified the five low deletions of ten.
	 *
	 * It becomes the width of a full-height strip: every block within (0.5 + w) cells of the
	 * deletion, at every height. The half cell is not slop — running bond offsets alternate
	 * courses by half a cell, so a whole-cell window takes 2w+1 bricks from aligned courses
	 * and nothing from offset ones, a ladder of disconnected rungs. The half cell gives 2w+1
	 * aligned and 2w+2 offset, a connected strip at every w including 0.
	 * ================================================================================ */

	void SpikeGroundStripMask(
		const FOracleProblem& Full,
		double CentreXCm,
		int32 HalfWidthCells,
		TArray<bool>& OutMask)
	{
		OutMask.Init(false, Full.Blocks.Num());

		const double ReachX =
			(0.5 + double(HalfWidthCells)) * SpikeCellPitchCm + SpikeRadiusSlackCm;

		for (int32 Block = 0; Block < Full.Blocks.Num(); ++Block)
		{
			OutMask[Block] =
				FMath::Abs(Full.Blocks[Block].CentroidXCm - CentreXCm) <= ReachX;
		}
	}

	/** Everything from the foundation up to a height — the chain fixtures' ground strip. */
	void SpikeGroundBandMask(
		const FOracleProblem& Full,
		double TopZCm,
		TArray<bool>& OutMask)
	{
		OutMask.Init(false, Full.Blocks.Num());

		for (int32 Block = 0; Block < Full.Blocks.Num(); ++Block)
		{
			OutMask[Block] = Full.Blocks[Block].CentroidZCm <= TopZCm + SpikeRadiusSlackCm;
		}
	}

	/* ================================================================================
	 * PICKING A BRICK TO DELETE, DETERMINISTICALLY.
	 * ================================================================================ */

	/**
	 * The live, ungrounded piece nearest the middle of course k (counting the grounded
	 * bottom course as 0). Index order decides exact ties, so the choice is reproducible
	 * and the fixture is the same wall on every run.
	 */
	/**
	 * One fixture, both poses. Every pin is INDEX_NONE or negative until measured; an
	 * unmeasured pin is an error, this slice's red.
	 */
	struct FCostRow
	{
		const TCHAR* Name = nullptr;

		/** What revision 1 of the derivation record predicted, printed beside the result. */
		const TCHAR* Prediction = nullptr;

		TFunction<bool(FStructure&, FString&)> Build;

		int32 Blocks = INDEX_NONE;
		int32 Joints = INDEX_NONE;

		/** lambda* of the gravity-LIVE pose, in the file's +/-2e-5 relative window. */
		double LambdaLo = -1.0;
		double LambdaHi = -1.0;

		int32 LivePivots = INDEX_NONE;
		int32 DeadPivots = INDEX_NONE;

		/**
		 * The early exit, pinned: dead-pose phase-1 pivots, and the pivot at which the
		 * problem first read feasible. Their difference is what an early exit would buy.
		 */
		int32 DeadPhaseOnePivots = INDEX_NONE;

		/**
		 * INDEX_NONE is a MEASURED value here, not a sentinel: an infeasible problem never
		 * reaches feasibility and must report exactly that (the 30-course stack's row pins
		 * it), so "unmeasured" needs its own value, SpikeUnmeasured (-2).
		 */
		int32 DeadFirstFeasible = SpikeUnmeasured;

		/** 1 feasible at lambda = 1, 0 infeasible. INDEX_NONE is unmeasured. */
		int32 Feasible = INDEX_NONE;

		/*
		 * Whether this row re-solves the gravity-live pose. Four rows do not (the 2026-08-16
		 * tier split): corbel D, wall-06, wall-15 and wall-18 have lambda* pinned in
		 * OracleSweepFull.RigidBlock.WallsAndLadders (same producer, cut, bridge, solver, so
		 * bit-identical), which runs in the same tier; re-solving bought a window that
		 * already exists and ~33 s per run.
		 *
		 * Kept live for non-duplication reasons: wall-01 (the project's only pinned wall-01
		 * lambda*, plus the 10.88x headline), the 84-block gate fixture (the gate arithmetic
		 * and its 5.31x), and both leaning stacks (the 30-course one is the infeasible arm,
		 * and both are microseconds).
		 *
		 * Cost on the four trimmed rows: their lambda* window, live pivot pin, within-run
		 * cross-validation and live 0/0 pins go, so their speedup ratio becomes cross-run
		 * against WallsAndLadders. The dead pose is untouched on every row, so the pooled
		 * early-exit totals (9,245 and 58), the scale spread and every verdict are bit-identical.
		 */
		bool bSolveLivePose = true;
	};

	/**
	 * The sandwich's pins, as struct members not local constants: a pin the compiler can
	 * fold turns its unmeasured branch into dead code, and a measurement stops being taken.
	 */
	struct FSandwichPins
	{
		int32 LadderBlocks = 149;
		int32 GlobalFeasible = 1;
		int32 GlobalPivots = 965;

		/*
		 * MEASURED 2026-08-15. Brick out of course 6 of a 12-course wall; the sandwich first
		 * closes at radius 5, the first radius reaching the foundation (as the conservation
		 * lemma predicts, not the design's "small radius"). Region there: 142 of 149 blocks.
		 */
		int32 ClosingRadius = 5;
		int32 ClosingRegionBlocks = 142;
		int32 ClosingOptimisticPivots = 753;
		int32 ClosingPessimisticPivots = 872;

		/*
		 * R2's ANSWER. Ten deletions, courses 1..10, radius 4: FIVE certify. Revision 1
		 * predicted four; the miss is the shell reaching one course past the core, so a
		 * radius-4 box around a course-5 brick touches the foundation. Wrong by one course.
		 */
		int32 Agreements = 5;

		/*
		 * The collapse arm: the 30-course stack's OPTIMISTIC region is still feasible at
		 * radius 13 (29 of 30) and only infeasible at radius 14, all 30 blocks — the whole
		 * structure.
		 */
		int32 StackCertifyingRadius = 14;
		int32 StackCertifyingRegionBlocks = 30;

		/*
		 * PART D's BAND. Courses 0..4 core plus course 5 as shell, so six blocks and five
		 * joints out of the 30-course stack.
		 */
		int32 BandBlocks = 6;

		/*
		 * How wrong the false certificate is: the band's lambda* in the LIVE pose, file-wide
		 * +/-2e-5 window. Negative is unmeasured (a red). Measured on this exact region.
		 *
		 * MEASURED 2026-08-15: **18.481256459924058**, 5 pivots. The stack is 0.44048, so the
		 * sandwich certified a region standing at **41.96x** the load under which the
		 * certified structure has no equilibrium. Review's independent (N-1)^-2.29 fit
		 * predicted ~18.7, 1.2% off — a second derivation, not a transcription.
		 */
		double BandLiveLambdaLo = 18.48088;
		double BandLiveLambdaHi = 18.48163;
	};

	/** One rung of the radius ladder, pinned by size and by both verdicts. */
	struct FRadiusPin
	{
		int32 Radius = INDEX_NONE;
		int32 Blocks = INDEX_NONE;
		int32 Optimistic = INDEX_NONE;
		int32 Pessimistic = INDEX_NONE;
	};

	/** One rung of the REPAIRED sandwich's strip ladder. INDEX_NONE is unmeasured. */
	struct FStripPin
	{
		int32 HalfWidthCells = INDEX_NONE;
		int32 Blocks = INDEX_NONE;

		/** Blocks the Carried rule charges as a surcharge at this width. */
		int32 Charged = INDEX_NONE;

		int32 Optimistic = INDEX_NONE;
		int32 Pessimistic = INDEX_NONE;
	};

	/**
	 * THE REPAIRED SANDWICH'S PINS. Every one is INDEX_NONE or negative until measured; an
	 * unmeasured pin is an error, this slice's red.
	 */
	struct FRepairPins
	{
		/* The refuted form's fixture, pinned again so the two are the same wall. */
		int32 WallBlocks = 149;
		int32 WallGlobalPivots = 965;

		/*
		 * PREDICTED (P3): the strip closes at half-width 0 (a two-cell zig-zag column)
		 * because running bond gives the course above the hole a bearing on both sides
		 * inside the strip, and GP mortar's 0.70 MPa bond is ample. Predicted 41 of 149
		 * blocks (17 core + 24 shell), 28%, vs the refuted form's 142 / 95%.
		 * MEASURED 2026-08-15: half-width 0, 41 blocks, 27.5%. HIT, block for block.
		 *
		 * P5 predicted cost 0.3-0.6x global, point estimate 0.48x. MEASURED 49 + 262 = 311
		 * vs 965 — 0.322x. In range; the point estimate missed by a third because the
		 * optimistic side is nearly free (49 pivots for 41 blocks): grounding the shell turns
		 * most of the boundary into earth, which writes no equilibrium row.
		 */
		int32 ClosingHalfWidth = 0;
		int32 ClosingRegionBlocks = 41;
		int32 ClosingOptimisticPivots = 49;
		int32 ClosingPessimisticPivots = 262;

		/*
		 * PREDICTED (P4): ten of ten deletions certify, vs the refuted form's five (the five
		 * that failed were those not reaching the foundation; a ground-anchored strip reaches
		 * it at every height, so the course stops mattering).
		 * MEASURED: 10. HIT — the region is 39 or 41 blocks at every course, alternating with
		 * bond parity.
		 *
		 * Named Agreements, not Certificates: the test also counts every closed sandwich in
		 * the file (20), and two counters named alike at different scopes read plausibly and
		 * mean nothing.
		 */
		int32 Agreements = 10;

		/*
		 * PREDICTED (P8): the over-inclusive control (charge EVERY omitted block) reads
		 * INFEASIBLE at the closing width — a strip cannot carry the whole building.
		 *
		 * MEASURED 1: FEASIBLE. **P8 is a miss, the most useful in the run.** Charging all
		 * 108 omitted blocks (271,309.925 uu on a 41-block strip) still leaves it standing:
		 * the surcharge is 2.7 kN of the wall's ~4 kN, spread by area over 44 joints each
		 * with 0.70 MPa bond over ~110-220 cm². The wall arm has too much margin to
		 * discriminate surcharge rules. So the "too pessimistic to certify" mode is untested
		 * here, and the closing width is rule-independent — the wall arm is evidence about
		 * REPAIR (2) only.
		 */
		int32 ControlPessimistic = 1;

		/*
		 * PREDICTED (P7): a ground-anchored region's size scales with structure height. Same
		 * wall at 18 courses: predicted half-width 0 and ~62 blocks, 1.5x the 12-course 41.
		 * MEASURED: 224 blocks, half-width 0, region 62 — 1.512x, 27.7% vs 27.5%. HIT. The
		 * fraction is height-invariant, the count is not: a 30-course wall's strip is ~102
		 * blocks (estimate, linear off two points), larger than the 84-block fixture that
		 * answers in 0.0666 s.
		 *
		 * Its two pivot counts are pinned as the INPUT to that extrapolation: 79 + 474 = 553
		 * against the 12-course 49 + 262 = 311 is the 1.78x-per-1.5x-height slope the
		 * scenario-scale answer rests on. Pinning the size and not the cost would let that
		 * number rot while every verdict stayed green.
		 */
		int32 TallWallBlocks = 224;
		int32 TallClosingHalfWidth = 0;
		int32 TallClosingRegionBlocks = 62;
		int32 TallClosingOptimisticPivots = 79;
		int32 TallClosingPessimisticPivots = 474;

		/*
		 * THE COLLAPSE ARM. PREDICTED (P9): the repaired pessimistic side is INFEASIBLE at
		 * every band short of the whole stack (the surcharge restores the destabilising
		 * moment), and the optimistic side still goes infeasible only with all 30. So the
		 * sandwich closes at the whole structure and nowhere below: the repair turns a false
		 * certificate into a refusal to certify, and buys the collapse arm nothing.
		 * MEASURED: exactly that. Bands 0..5, 0..10, 0..15, 0..20, 0..25 read optimistic
		 * FEASIBLE / pessimistic INFEASIBLE; only 0..29 closes. HIT.
		 */
		int32 StackClosingTopCourse = 29;
		int32 StackClosingRegionBlocks = 30;

		/*
		 * WHAT THE COLLAPSE ARM COSTS. At closure the two solves are 163 + 163 = 326 against
		 * a 163-pivot global solve — **2.0x** — and the whole growth ladder from 0..5 up is
		 * 1,300 pivots, **8x**. The standing arm's 0.322x and this 2.0x are two halves of one
		 * measurement: 2.0x is worse than the 1.68x the refuted form was condemned for, on the
		 * arm a player's deletion cares about. Pinned so the ratio is a contract.
		 */
		int32 StackClosingOptimisticPivots = 163;
		int32 StackClosingPessimisticPivots = 163;
		int32 StackLadderPivots = 1300;

		/*
		 * The surcharge's reading where it works: band 0..5 is 7 blocks (six core plus one
		 * shell) and charges the 23 courses above. Pinned because a reachability walk
		 * charging the wrong set would still give an infeasible band with every verdict green.
		 */
		int32 StackBandChargedAtFive = 23;
	};

	int32 SpikePieceInCourse(const FStructure& Wall, int32 Course, double& OutXCm, double& OutZCm)
	{
		double LowestZ = TNumericLimits<double>::Max();
		double LowX = TNumericLimits<double>::Max();
		double HighX = -TNumericLimits<double>::Max();

		for (int32 Piece = 0; Piece < Wall.NumPieces(); ++Piece)
		{
			if (Wall.IsPieceRemoved(Piece))
			{
				continue;
			}

			const FVector Centre = Wall.GetPiece(Piece).CentreOfMassCm;
			LowestZ = FMath::Min(LowestZ, Centre.Z);
			LowX = FMath::Min(LowX, Centre.X);
			HighX = FMath::Max(HighX, Centre.X);
		}

		const double WantZ = LowestZ + double(Course) * SpikeCoursePitchCm;
		const double WantX = 0.5 * (LowX + HighX);

		int32 Best = INDEX_NONE;
		double BestScore = TNumericLimits<double>::Max();

		for (int32 Piece = 0; Piece < Wall.NumPieces(); ++Piece)
		{
			if (Wall.IsPieceRemoved(Piece) || Wall.GetPiece(Piece).bIsGrounded)
			{
				continue;
			}

			const FVector Centre = Wall.GetPiece(Piece).CentreOfMassCm;

			if (FMath::Abs(Centre.Z - WantZ) > 0.5 * SpikeCoursePitchCm)
			{
				continue;
			}

			const double Score = FMath::Abs(Centre.X - WantX);

			if (Score < BestScore)
			{
				BestScore = Score;
				Best = Piece;
			}
		}

		if (Best != INDEX_NONE)
		{
			OutXCm = Wall.GetPiece(Best).CentreOfMassCm.X;
			OutZCm = Wall.GetPiece(Best).CentreOfMassCm.Z;
		}

		return Best;
	}

	/* ================================================================================
	 * THE SUB-1.0 WALL — a wall carrying a leaning chimney.
	 *
	 * WHY IT EXISTS: the repaired pessimistic side has never been shown to be a bound, and
	 * cannot be on any fixture the project owns (PROMOTION_DESIGN §5.3, §11 R2). Nineteen of
	 * the repaired test's twenty certificates are about already-feasible structures and the
	 * twentieth is the whole structure, so none is about a proper subset of an infeasible
	 * structure — the only shape a false certificate takes. The 30-course stack is the only
	 * sub-1.0 fixture and never certifies short of all thirty.
	 *
	 * WHAT THE FIXTURE NEEDS, beyond lambda* < 1: the failure must live outside the strip. A
	 * wall failing at its own foundation makes the strip infeasible too and the sandwich
	 * opens — the lever behaving correctly, teaching nothing.
	 *
	 * THE MECHANISM: PROMOTION_DESIGN's second route, a projecting mass that overturns
	 * globally while a ground-anchored strip stays locally stable. A 30-course leaning
	 * chimney (leaning-stack geometry, 10 cm lean per course) is mortared onto the rightmost
	 * full brick of the top course and leans out past the wall's end. Its bed joints cannot
	 * carry the eccentric load above; that chain is the fixture the LP prices at
	 * lambda* = 0.44048, and the wall cannot help because the chain sees the same loads on
	 * earth or on a wall.
	 *
	 * THREE ROUTES NOT TAKEN, so nobody re-derives them:
	 *   - Widen an opening to the crossing: case 21's family crosses lambda* = 1 near 40-45
	 *     cells, so 500+ blocks and tens of seconds per dead solve — against cost discipline.
	 *   - A corbel: a stepped course is a corbel with the whole wall as counterweight, priced
	 *     at lambda* 17-161 — reaching 1.0 is a search.
	 *   - A weak course high up: zeroing a band's bond does not by itself make masonry fall
	 *     (dry stone stands), so it needs the opening again.
	 *
	 * HONEST LIMITATION: the chimney makes the structure infeasible before the deletion, so
	 * this counterexample is not deletion-caused. That is fine — the pessimistic claim is
	 * unconditional, so any counterexample refutes it. A deletion-caused non-local failure
	 * needs the wide-opening fixture, a specified follow-up.
	 * ================================================================================ */

	constexpr int32 SpikeChimneyCourses = 30;

	/**
	 * Chimney lean per course, cm. The leaning stack's 10 cm on a 21.5 cm brick, so the two
	 * chains are the same and the 0.44048 pricing one is expected to price the other.
	 */
	constexpr double SpikeChimneyLeanCm = 10.0;

	/**
	 * The bypassed root bed joint's margin (capacity over demand), which the surcharge never
	 * crosses, from the two-contact form. Measured 2026-08-18 at 1.54375, pinned in a ~6e-5
	 * window: bit-deterministic arithmetic, but a window says the claim is "comfortably over
	 * 1.0", not "these exact digits".
	 */
	constexpr double SpikeBypassedMarginLo = 1.5437;
	constexpr double SpikeBypassedMarginHi = 1.5438;

	struct FChimneyWall
	{
		FStructure Structure;

		/** The brick removed from the wall, and where it sat. */
		int32 Victim = INDEX_NONE;
		double DeleteXCm = 0.0;
		double DeleteZCm = 0.0;

		/** The wall brick the chimney stands on, which is the far end of the structure. */
		double RootXCm = 0.0;
		double RootZCm = 0.0;

		int32 ChimneyBlocks = 0;
	};

	/**
	 * Lay the wall, choose the victim, mortar the chimney onto the top course, then delete.
	 * ChimneyCourses = 0 lays the same wall with no chimney through the same code, making the
	 * two comparable rather than similar.
	 */
	bool SpikeBuildChimneyWall(
		int32 Courses,
		int32 Cells,
		int32 ChimneyCourses,
		int32 DeleteCourse,
		FChimneyWall& Out,
		FString& OutWhy)
	{
		Out = FChimneyWall();

		if (!SpikeBuildIntactWall(Courses, Cells, Out.Structure, OutWhy))
		{
			return false;
		}

		/*
		 * The victim is chosen before the chimney exists. SpikePieceInCourse takes the middle
		 * of the structure's X extent, and a chimney reaching ~290 cm past the wall's right
		 * face would drag that middle to the far end, putting the deletion beside the failing
		 * part instead of far from it. Choosing first also makes the victim the same brick
		 * RepairedRegionalSandwich deletes, so the two region problems are comparable.
		 */
		Out.Victim = SpikePieceInCourse(Out.Structure, DeleteCourse, Out.DeleteXCm, Out.DeleteZCm);

		if (Out.Victim == INDEX_NONE)
		{
			OutWhy = FString::Printf(
				TEXT("no brick to delete in course %d of a %d x %d wall"),
				DeleteCourse, Courses, Cells);
			return false;
		}

		if (ChimneyCourses > 0)
		{
			double TopZCm = -TNumericLimits<double>::Max();

			for (int32 Piece = 0; Piece < Out.Structure.NumPieces(); ++Piece)
			{
				if (!Out.Structure.IsPieceRemoved(Piece))
				{
					TopZCm = FMath::Max(TopZCm, Out.Structure.GetPiece(Piece).CentreOfMassCm.Z);
				}
			}

			/*
			 * The root must be a full brick, and mass is how this knows. A flush running-bond
			 * course closes with a half bat at each end, so the rightmost top-course piece is
			 * 10.25 cm, not 21.5. MakeInterface takes a box and FStructure keeps none, so it
			 * is reconstructed from the standard brick size; doing that for a half bat would
			 * hand the solver a joint 2x the real area. Selecting on mass keeps the
			 * reconstruction sound.
			 */
			int32 Root = INDEX_NONE;
			double RootXCm = -TNumericLimits<double>::Max();

			for (int32 Piece = 0; Piece < Out.Structure.NumPieces(); ++Piece)
			{
				if (Out.Structure.IsPieceRemoved(Piece))
				{
					continue;
				}

				const FStructurePiece& Candidate = Out.Structure.GetPiece(Piece);

				if (FMath::Abs(Candidate.CentreOfMassCm.Z - TopZCm) > 0.5 * SpikeCoursePitchCm)
				{
					continue;
				}

				if (FMath::Abs(Candidate.MassKg - SpikeBrickMassKg) > 1.0e-9 * SpikeBrickMassKg)
				{
					continue;
				}

				if (Candidate.CentreOfMassCm.X > RootXCm)
				{
					RootXCm = Candidate.CentreOfMassCm.X;
					Root = Piece;
				}
			}

			if (Root == INDEX_NONE)
			{
				OutWhy = TEXT("the top course holds no FULL brick to stand a chimney on");
				return false;
			}

			const FVector FullExtentCm =
				FVector(SpikeBrickLengthCm, SpikeBrickWidthCm, SpikeBrickHeightCm) * 0.5;

			TArray<int32> Handles;
			TArray<FPieceBox> Boxes;

			FPieceBox RootBox;
			RootBox.CentreCm = Out.Structure.GetPiece(Root).CentreOfMassCm;
			RootBox.ExtentCm = FullExtentCm;

			Handles.Add(Root);
			Boxes.Add(RootBox);

			Out.RootXCm = RootBox.CentreCm.X;
			Out.RootZCm = RootBox.CentreCm.Z;

			for (int32 Course = 0; Course < ChimneyCourses; ++Course)
			{
				FPieceBox Box;
				Box.ExtentCm = FullExtentCm;
				Box.CentreCm = FVector(
					RootBox.CentreCm.X + double(Course) * SpikeChimneyLeanCm,
					RootBox.CentreCm.Y,
					RootBox.CentreCm.Z + double(Course + 1) * SpikeCoursePitchCm);

				const int32 Handle = Out.Structure.AddPiece(
					SpikeBrickMassKg, /*bIsGrounded*/ false, Box.CentreCm);

				if (Handle == INDEX_NONE)
				{
					OutWhy = FString::Printf(TEXT("chimney course %d was refused"), Course);
					return false;
				}

				Handles.Add(Handle);
				Boxes.Add(Box);
			}

			/*
			 * All pairs, as the leaning stack's builder does, so the joint count is a check:
			 * an N-course chimney must emit N joints (root bed plus N-1 chain beds). A lean
			 * that lost the overlap, or a pitch that stopped matching the joint thickness,
			 * shows up as a wrong count rather than a quietly missing joint.
			 */
			int32 Made = 0;

			for (int32 First = 0; First < Handles.Num(); ++First)
			{
				for (int32 Second = First + 1; Second < Handles.Num(); ++Second)
				{
					FConnection Joint;

					if (MakeInterface(Handles[First], Boxes[First], Handles[Second], Boxes[Second],
							SpikeJointCm, GeneralPurposeMortar, Joint))
					{
						Out.Structure.AddConnection(Joint);
						++Made;
					}
				}
			}

			if (Made != ChimneyCourses)
			{
				OutWhy = FString::Printf(
					TEXT("the chimney emitted %d joints for %d courses"), Made, ChimneyCourses);
				return false;
			}

			Out.ChimneyBlocks = ChimneyCourses;
		}

		if (!Out.Structure.RemovePiece(Out.Victim))
		{
			OutWhy = FString::Printf(TEXT("piece %d could not be removed"), Out.Victim);
			return false;
		}

		return true;
	}

	/**
	 * THE SUB-1.0 WALL'S PINS. Every one is INDEX_NONE or negative until measured; an
	 * unmeasured pin is an error, this slice's red.
	 */
	struct FSubUnityPins
	{
		/*
		 * MEASURED 2026-08-16. The plain half is RepairedRegionalSandwich's fixture at its own
		 * pinned numbers (149 blocks, 965 pivots), measured here in the same run not
		 * transcribed, because the argument is that the two are the same wall.
		 */
		int32 PlainBlocks = 149;
		int32 PlainJoints = 385;
		int32 PlainGlobalPivots = 965;

		int32 CompositeBlocks = 179;
		int32 CompositeJoints = 415;
		int32 CompositeGlobalPivots = 1308;

		/*
		 * lambda* of the whole composite, gravity-live, file-wide +/-2e-5 window.
		 * MEASURED 0.44049301907204619, inside the bare 30-course stack's pinned
		 * [0.440484, 0.440502] — Q1 predicted this: the binding joint is inside the chain, and
		 * a chain does not care whether it stands on earth or a wall. The named fallback (the
		 * root joint at a hand-priced ~1.5) did not fire.
		 */
		double CompositeLambdaLo = 0.4404842;
		double CompositeLambdaHi = 0.4405019;

		/**
		 * How many of the six strip widths closed while the structure has no equilibrium.
		 * MEASURED 4 — w = 0, 1, 2 and 3, the smallest of them 41 of 179 blocks.
		 */
		int32 FalseCertificates = 4;

		/*
		 * The certified strip's own lambda*, how false the certificate is.
		 * MEASURED 621.95652149729517 against 0.44049 — **1,412x**, where Part D's refuted
		 * form was wrong by 41.96x.
		 */
		double StripLambdaLo = 621.9441;
		double StripLambdaHi = 621.9689;

		/*
		 * The contrast strip, cut at the chimney's root instead of the deletion: the chimney
		 * above has no ground path, so all 27 blocks above the region are charged, the
		 * pessimistic side goes INFEASIBLE and nothing is certified. The row showing the
		 * repair does real work where it can see the failing material.
		 */
		int32 ContrastBlocks = 38;
		int32 ContrastCharged = 27;
		int32 ContrastOptimistic = 1;
		int32 ContrastPessimistic = 0;
	};
}

/* ====================================================================================
 * TEST 1 — THE FEASIBILITY REFORMULATION'S COST.
 *
 * COST: the lambda* pose is almost all of it (wall-01 alone is 280 s). MEASURED 2026-08-15
 * for the group: **344 s**, of which the dead poses (what the slice measures) are 53 s.
 * Opt-in full tier for that reason.
 *
 * THE TRIM (2026-08-16). Four rows — corbel D, wall-06, wall-15, wall-18 — no longer
 * re-solve the live pose: same producer, cut, bridge and solver as
 * `OracleSweepFull.RigidBlock.WallsAndLadders`, which pins each lambda* in the same tier,
 * so the solve and its window were duplicated. 344 s -> 313 s, and WallsAndLadders' logs
 * confirm the duplication exact (corbel D 9,490 pivots, wall-18 3,885, wall-15 6,076).
 * Wall-01 stays live: `RigidBlockOracleSweepTest` excludes the 30-course walls, so this
 * file holds the project's only pinned wall-01 lambda*. The gate fixture and both stacks
 * stay live too — see FCostRow::bSolveLivePose.
 *
 * The trim touched no dead solve: all eight fixtures still measure the reformulation, so
 * the pooled early-exit totals (9,245 and 58), the scale spread and every verdict are
 * bit-identical. Needs a ticking world: no.
 * ==================================================================================== */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleFeasibilityCostTest,
	"OracleSweepFull.RigidBlock.FeasibilityReformulationCost",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleFeasibilityCostTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace OracleFeasibilitySpikeSupport;

	TArray<FCostRow> Rows;

	/*
	 * Every row carries its prediction and its measurement side by side: a prediction
	 * deleted once checked leaves nobody able to tell an agreement from a transcription.
	 * Measured 2026-08-15, one run, tree at HEAD 9dcbd76.
	 */

	Rows.Add({ TEXT("leaning stack, 5 courses"),
		TEXT("PREDICTED 10-40 dead-pose pivots; feasible (lambda* 31.2). MEASURED 22 — HIT"),
		[](FStructure& Out, FString& Why) { return SpikeBuildLeaningStack(5, Out, Why); },
		5, 4, 31.2067, 31.2081, 4, 22, 21, 21, 1 });

	Rows.Add({ TEXT("leaning stack, 30 courses"),
		TEXT("PREDICTED 100-400 dead-pose pivots; INFEASIBLE (lambda* 0.4405) — the arm ")
		TEXT("that stops the boolean being vacuous. MEASURED 163, infeasible — HIT both ways"),
		[](FStructure& Out, FString& Why) { return SpikeBuildLeaningStack(30, Out, Why); },
		30, 29, 0.440484, 0.440502, 29, 163, 163, INDEX_NONE, 0 });

	Rows.Add({ TEXT("8x10 intact wall (THE GATE FIXTURE, 84 blocks)"),
		TEXT("PREDICTED 400-800 dead-pose pivots against 1,942 live, ~3-5x, ~0.15 s ")
		TEXT("against a 50 ms target. MEASURED 491 pivots (HIT) against 2,606 live — the ")
		TEXT("1,942 in the prediction was a 2026-08-12 characteristic-era figure and ")
		TEXT("today's live path is 2,606. 5.31x on pivots, 11.5x on seconds, 0.065 s"),
		[](FStructure& Out, FString& Why) { return SpikeBuildIntactWall(8, 10, Out, Why); },
		84, 207, 1128.4217, 1128.4670, 2606, 491, 490, 487, 1 });

	Rows.Add({ TEXT("corbel D, ten steps with counterweight"),
		TEXT("PREDICTED 400-900 dead-pose pivots against 8,439 live, ~10-20x. MEASURED ")
		TEXT("1,022 — MISS, 14% over the top of the range; 9.29x on pivots, 37.3x on ")
		TEXT("seconds. LIVE POSE NOT RE-SOLVED HERE since 2026-08-16: lambda* 161.14 is ")
		TEXT("pinned in WallsAndLadders at [161.1375, 161.1439] and the 9,490-pivot live ")
		TEXT("solve was 10.2 s of duplication"),
		[](FStructure& Out, FString& Why)
		{
			return SpikeBuildScenario(TEXT("corbel-d-10-counterweight"), Out, Why);
		},
		90, 200, -1.0, -1.0, INDEX_NONE, 1022, 1021, 1021, 1, /*bSolveLivePose*/ false });

	Rows.Add({ TEXT("wall-18 stack bond, one brick out"),
		TEXT("PREDICTED 400-900 dead-pose pivots against 791 live — ABOUT 1x, the row ")
		TEXT("chosen to test the design's 10-100x estimate where the climb was already ")
		TEXT("short. MEASURED 468 dead pivots (HIT) but 3,885 LIVE, not 791 — THE ")
		TEXT("PREDICTION'S PREMISE WAS STALE (791 was a 2026-08-12 Dantzig-path, ")
		TEXT("characteristic-era reading), so 8.30x on pivots and 44.6x on seconds. No ")
		TEXT("fixture in this list has a short climb today and the 'about 1x' hypothesis ")
		TEXT("is UNTESTED rather than refuted. LIVE POSE NOT RE-SOLVED HERE since ")
		TEXT("2026-08-16: lambda* 897.73 is pinned in WallsAndLadders at [897.712, ")
		TEXT("897.749]"),
		[](FStructure& Out, FString& Why)
		{
			return SpikeBuildScenario(TEXT("wall-18"), Out, Why);
		},
		119, 203, -1.0, -1.0, INDEX_NONE, 468, 467, 449, 1, /*bSolveLivePose*/ false });

	Rows.Add({ TEXT("wall-15 header with six courses on top"),
		TEXT("PREDICTED 500-1,200 dead-pose pivots against 3,745 live, ~3-7x. MEASURED ")
		TEXT("803 (HIT); 7.57x on pivots against 6,076 live, 18.7x on seconds. LIVE POSE ")
		TEXT("NOT RE-SOLVED HERE since 2026-08-16: lambda* 868.6237 is pinned in ")
		TEXT("WallsAndLadders at [868.62287, 868.62461], and as the 15/16 cross-row ")
		TEXT("identity beside it"),
		[](FStructure& Out, FString& Why)
		{
			return SpikeBuildScenario(TEXT("wall-15"), Out, Why);
		},
		125, 320, -1.0, -1.0, INDEX_NONE, 803, 802, 799, 1, /*bSolveLivePose*/ false });

	Rows.Add({ TEXT("wall-06 two-cell opening, deep cover"),
		TEXT("PREDICTED 600-1,500 dead-pose pivots against 8,819 live, ~6-15x. MEASURED ")
		TEXT("876 (HIT); 16.2x on pivots against 14,209 live, 51.2x on seconds — the ")
		TEXT("largest speedup in the set, and the one the 2026-08-16 trim makes a ")
		TEXT("CROSS-RUN reading: LIVE POSE NOT RE-SOLVED HERE, lambda* 634.58 pinned in ")
		TEXT("WallsAndLadders at [634.570, 634.596], 16.1 s of duplication dropped"),
		[](FStructure& Out, FString& Why)
		{
			return SpikeBuildScenario(TEXT("wall-06"), Out, Why);
		},
		146, 372, -1.0, -1.0, INDEX_NONE, 876, 875, 866, 1, /*bSolveLivePose*/ false });

	Rows.Add({ TEXT("wall-01 thirty courses (THE HEADLINE, 375 blocks)"),
		TEXT("PREDICTED 1,500-4,000 dead-pose pivots against 58,605 live, ~15-40x. ")
		TEXT("MEASURED 5,407 — MISS, 35% over the top; 10.9x on pivots and 10.8x on ")
		TEXT("seconds, 280.5 s -> 26.0 s. The phase-1 pivot count is NOT O(rows) with a ")
		TEXT("small constant: it is 2.2x the equality-row count at 84 blocks and ~5x it ")
		TEXT("here, so the lever WEAKENS with scale rather than strengthening"),
		[](FStructure& Out, FString& Why)
		{
			return SpikeBuildScenario(TEXT("wall-01"), Out, Why);
		},
		375, 1030, 272.1994, 272.2104, 58806, 5407, 5406, 5381, 1 });

	/* The gate fixture's own reading, kept out of the loop for the arithmetic below. */
	double GateDeadSeconds = -1.0;
	double GateLiveSeconds = -1.0;

	/*
	 * The early exit's pooled totals, accumulated across rows and pinned after the loop. The
	 * per-row pins already fix these sums; pinning the pooled 0.63% too makes the conclusion
	 * CURRENT_STATE and PROMOTION_DESIGN §5.2 record appear in the suite in the same shape.
	 */
	int32 PooledPhaseOnePivots = 0;
	int32 PooledSkippedPivots = 0;

	for (const FCostRow& Row : Rows)
	{
		FStructure Structure;
		FString Why;

		/*
		 * Each call is its own statement before its message: folding a call that writes Why
		 * into the Printf that reads it is unsequenced, and MSVC evaluates the condition last
		 * (TRAPS).
		 */
		const bool bLaid = Row.Build(Structure, Why);

		if (!TestTrue(
				*FString::Printf(TEXT("%s: the producer must lay it (it said: %s)"),
					Row.Name, *Why),
				bLaid))
		{
			continue;
		}

		FOracleProblem Live;

		const bool bBridged = BuildRigidBlockProblem(Structure, Live, Why);

		if (!TestTrue(
				*FString::Printf(TEXT("%s: the bridge must represent it (it said: %s)"),
					Row.Name, *Why),
				bBridged))
		{
			continue;
		}

		/*
		 * The two poses are the same problem and one flag. Copying rather than rebuilding: a
		 * separately-built dead pose would compare two problems, and a producer difference
		 * would read as a reformulation difference.
		 */
		FOracleProblem Dead = Live;
		Dead.bGravityIsLive = false;

		/*
		 * The live pose is solved only where its answer is not already pinned in the same
		 * tier (see FCostRow::bSolveLivePose). A skipped row leaves LiveRead default
		 * (bAnswered = false); every reader below is inside the same flag, and the log says
		 * NOT RE-SOLVED rather than a zero that reads like a refusal.
		 */
		const FPoseReading LiveRead = Row.bSolveLivePose ? SpikeSolve(Live) : FPoseReading();
		const FPoseReading DeadRead = SpikeSolve(Dead);

		/*
		 * The early-exit saving, printed per row: the fraction of phase-1 pivots spent after
		 * first feasibility, which stopping there would skip. Negative means the fixture never
		 * reached feasibility (the infeasible arm), reported -1 rather than a plausible zero.
		 */
		const double EarlyExitSaving =
			DeadRead.PhaseOnePivots > 0 && DeadRead.PivotsToFirstFeasible >= 0
				? double(DeadRead.PhaseOnePivots - DeadRead.PivotsToFirstFeasible)
					/ double(DeadRead.PhaseOnePivots)
				: -1.0;

		const FString LiveText = Row.bSolveLivePose
			? FString::Printf(
				TEXT("LIVE lambda*=%.17g answered=%d pivots=%d secs=%.3f | pivot ratio %.3f ")
				TEXT("| time ratio %.3f | LIVE phase1=%d firstfeasible=%d"),
				LiveRead.Lambda, LiveRead.bAnswered ? 1 : 0, LiveRead.Pivots, LiveRead.Seconds,
				DeadRead.Pivots > 0 ? double(LiveRead.Pivots) / double(DeadRead.Pivots) : 0.0,
				DeadRead.Seconds > 0.0 ? LiveRead.Seconds / DeadRead.Seconds : 0.0,
				LiveRead.PhaseOnePivots, LiveRead.PivotsToFirstFeasible)
			: FString(
				TEXT("LIVE POSE NOT RE-SOLVED — lambda* is pinned in ")
				TEXT("OracleSweepFull.RigidBlock.WallsAndLadders, which runs in the same tier"));

		const FString Line = FString::Printf(
			TEXT("SPIKE %s: blocks=%d joints=%d | %s ")
			TEXT("| DEAD feasible=%d lambda=%.17g answered=%d pivots=%d secs=%.3f ")
			TEXT("| DEAD phase1=%d firstfeasible=%d ")
			TEXT("earlyexit saving %.4f | %s%s%s"),
			Row.Name, Live.Blocks.Num(), Live.Joints.Num(), *LiveText,
			SpikeIsFeasible(DeadRead) ? 1 : 0, DeadRead.Lambda, DeadRead.bAnswered ? 1 : 0,
			DeadRead.Pivots, DeadRead.Seconds,
			DeadRead.PhaseOnePivots, DeadRead.PivotsToFirstFeasible, EarlyExitSaving,
			Row.Prediction,
			DeadRead.WhyNot.IsEmpty() ? TEXT("") : TEXT(" | dead whynot: "),
			DeadRead.WhyNot.IsEmpty() ? TEXT("") : *DeadRead.WhyNot);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);

		if (FCString::Strstr(Row.Name, TEXT("GATE FIXTURE")) != nullptr)
		{
			GateDeadSeconds = DeadRead.Seconds;
			GateLiveSeconds = LiveRead.Seconds;
		}

		/* ---- The size pins, first, so a row cannot secretly solve another wall. ---- */

		if (Row.Blocks == INDEX_NONE || Row.Joints == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("%s: UNMEASURED SIZE — pin blocks=%d joints=%d"),
				Row.Name, Live.Blocks.Num(), Live.Joints.Num()));
		}
		else
		{
			TestEqual(*FString::Printf(TEXT("%s: block count"), Row.Name),
				Live.Blocks.Num(), Row.Blocks);
			TestEqual(*FString::Printf(TEXT("%s: joint count"), Row.Name),
				Live.Joints.Num(), Row.Joints);
		}

		/* ---- The live pose, whose lambda* is the cross-validation's other half. ---- */

		if (Row.bSolveLivePose)
		{
			if (!TestTrue(
					*FString::Printf(TEXT("%s: the LIVE pose must answer (it said: %s)"),
						Row.Name, *LiveRead.WhyNot),
					LiveRead.bAnswered))
			{
				continue;
			}

			if (Row.LambdaLo < 0.0)
			{
				AddError(FString::Printf(
					TEXT("%s: UNMEASURED lambda* — measured %.17g"), Row.Name, LiveRead.Lambda));
			}
			else
			{
				TestTrue(
					*FString::Printf(
						TEXT("%s: lambda* must lie in [%.9g, %.9g] and was %.17g"),
						Row.Name, Row.LambdaLo, Row.LambdaHi, LiveRead.Lambda),
					LiveRead.Lambda >= Row.LambdaLo && LiveRead.Lambda <= Row.LambdaHi);
			}
		}

		/* ---- The dead pose: the feasibility answer, and what it costs. ---- */

		if (!TestTrue(
				*FString::Printf(TEXT("%s: the DEAD pose must answer (it said: %s)"),
					Row.Name, *DeadRead.WhyNot),
				DeadRead.bAnswered))
		{
			continue;
		}

		/*
		 * How a test knows the pose is dead. With no live load the lambda column is in no
		 * equilibrium row and its only constraint is the cap, so feasible reports exactly
		 * LambdaCap and infeasible exactly 0. Anything between means a live load survived into
		 * a dead pose, and every count measured on it is measuring something else.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s: a DEAD pose carries no live load, so lambda must be exactly ")
				TEXT("LambdaCap (%.17g) or exactly 0, and was %.17g"),
				Row.Name, LambdaCap, DeadRead.Lambda),
			DeadRead.Lambda == LambdaCap || DeadRead.Lambda == 0.0);

		/*
		 * The cross-validation. The reformulation is worth measuring only if an admissible
		 * equilibrium at lambda = 1 exists exactly when the lambda* pose reads >= 1: two
		 * formulations, one answer.
		 *
		 * Within-run or nothing, which is why it is inside the live-pose flag rather than run
		 * against a transcribed lambda*: comparing a measured boolean to a typed number would
		 * assert the typing. The four rows span both arms — the 30-course stack is
		 * lambda* = 0.44 and must come back infeasible, else the boolean is vacuous.
		 */
		const bool bFeasible = SpikeIsFeasible(DeadRead);

		if (Row.bSolveLivePose)
		{
			const bool bStandsByLambda = LiveRead.Lambda >= 1.0;

			TestTrue(
				*FString::Printf(
					TEXT("%s: CROSS-VALIDATION — feasibility at lambda = 1 (%d) must agree with ")
					TEXT("lambda* >= 1 (%d, lambda* = %.17g). A disagreement means the ")
					TEXT("reformulation does not pose the question the design says it poses."),
					Row.Name, bFeasible ? 1 : 0, bStandsByLambda ? 1 : 0, LiveRead.Lambda),
				bFeasible == bStandsByLambda);
		}

		if (Row.Feasible == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("%s: UNMEASURED FEASIBILITY — measured %d"), Row.Name, bFeasible ? 1 : 0));
		}
		else
		{
			TestEqual(*FString::Printf(TEXT("%s: feasibility verdict"), Row.Name),
				bFeasible ? 1 : 0, Row.Feasible);
		}

		/*
		 * The two pivot pins are separate guards. They were one branch while every row solved
		 * both poses; a row not re-solving the live pose has no live pivot count, and folding
		 * them together would have taken the dead pin — the whole cost measurement — with it.
		 */
		if (Row.bSolveLivePose)
		{
			if (Row.LivePivots == INDEX_NONE)
			{
				AddError(FString::Printf(
					TEXT("%s: UNMEASURED LIVE COST — pin the live pivot count. Measured: ")
					TEXT("%d pivots in %.3f s. %s"),
					Row.Name, LiveRead.Pivots, LiveRead.Seconds, Row.Prediction));
			}
			else
			{
				TestEqual(
					*FString::Printf(
						TEXT("%s: the LIVE pose's pivot count is deterministic and pinned"),
						Row.Name),
					LiveRead.Pivots, Row.LivePivots);
			}
		}

		if (Row.DeadPivots == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("%s: UNMEASURED DEAD COST — pin the dead pivot count. Measured: ")
				TEXT("%d pivots in %.3f s. %s"),
				Row.Name, DeadRead.Pivots, DeadRead.Seconds, Row.Prediction));
		}
		else
		{
			TestEqual(
				*FString::Printf(
					TEXT("%s: the DEAD pose's pivot count is deterministic and pinned — this ")
					TEXT("is the reformulation's cost and the whole of what slice 0b buys"),
					Row.Name),
				DeadRead.Pivots, Row.DeadPivots);
		}

		/* ---- The early exit, pinned by VALUE and not merely by ordering. ---- */

		/*
		 * Why exact pins, not a range. Both are deterministic integers on a fixture solved
		 * anyway, so free, and they decide a published conclusion ("0.63% of phase-1 pivots
		 * happen after first feasibility", refuting §5.2's largest claim). A population-and-
		 * ordering check (0 <= first <= phase1) passes on a wrong-but-in-range index — the
		 * failure that matters: move wall-18's 449 to 40 and the lever is worth 91%, the
		 * conclusion inverts, nothing goes red.
		 *
		 * Prediction of record: the design's qualitative "most of the saving lives here"
		 * (§3.2/§5.2). The header's MEASUREMENT 1b table carries both halves.
		 */
		if (Row.DeadPhaseOnePivots == INDEX_NONE || Row.DeadFirstFeasible == SpikeUnmeasured)
		{
			AddError(FString::Printf(
				TEXT("%s: UNMEASURED EARLY EXIT — pin phase1=%d firstfeasible=%d"),
				Row.Name, DeadRead.PhaseOnePivots, DeadRead.PivotsToFirstFeasible));
		}
		else
		{
			TestEqual(
				*FString::Printf(
					TEXT("%s: the DEAD pose's PHASE-1 pivot count"), Row.Name),
				DeadRead.PhaseOnePivots, Row.DeadPhaseOnePivots);

			TestEqual(
				*FString::Printf(
					TEXT("%s: the pivot at which the DEAD pose FIRST read feasible — the ")
					TEXT("difference from the line above is the whole of what an early exit ")
					TEXT("would save, and INDEX_NONE is the measured answer on an infeasible ")
					TEXT("problem rather than a missing reading"),
					Row.Name),
				DeadRead.PivotsToFirstFeasible, Row.DeadFirstFeasible);

			/*
			 * The ordering property is kept beside the values, not instead: it is the one
			 * thing they cannot say, that the two fields are consistent whatever they read.
			 */
			TestTrue(
				*FString::Printf(
					TEXT("%s: first-feasible (%d) must lie in [0, phase-1 pivots] (%d) or be ")
					TEXT("INDEX_NONE for an infeasible problem"),
					Row.Name, DeadRead.PivotsToFirstFeasible, DeadRead.PhaseOnePivots),
				DeadRead.PivotsToFirstFeasible == INDEX_NONE
					|| (DeadRead.PivotsToFirstFeasible >= 0
						&& DeadRead.PivotsToFirstFeasible <= DeadRead.PhaseOnePivots));

			PooledPhaseOnePivots += DeadRead.PhaseOnePivots;

			if (DeadRead.PivotsToFirstFeasible >= 0)
			{
				PooledSkippedPivots +=
					DeadRead.PhaseOnePivots - DeadRead.PivotsToFirstFeasible;
			}
		}

		/*
		 * The live pose must report 0/0 on every row that solves one. A gravity-live problem
		 * has every equality RHS at zero, so it is feasible before a single pivot and phase 1
		 * never runs (checking the solver's claim from outside). Also a second guard on the
		 * pose: a live load leaking into a dead pose would put a non-zero phase 1 here. Four
		 * rows no longer re-solve the live pose, so the guard now stands on four fixtures.
		 */
		if (!Row.bSolveLivePose)
		{
			continue;
		}

		TestEqual(
			*FString::Printf(
				TEXT("%s: the LIVE pose starts feasible, so phase 1 spends no pivots"),
				Row.Name),
			LiveRead.PhaseOnePivots, 0);

		TestEqual(
			*FString::Printf(
				TEXT("%s: the LIVE pose is feasible at pivot zero, so first-feasible is 0 ")
				TEXT("— not INDEX_NONE, which would mean it never got there"),
				Row.Name),
			LiveRead.PivotsToFirstFeasible, 0);
	}

	/* ================================================================================
	 * THE EARLY EXIT'S HEADLINE, asserted as one number.
	 *
	 * 58 of 9,245 pooled — 0.63% — is the figure §5.2 and CURRENT_STATE record as refuting
	 * the lever. Pinned as a sum so the conclusion has its own assertion, not just eight
	 * ingredients: a change moving two rows opposite ways would leave the per-row pins red
	 * like noise, where this line says what changed.
	 * ================================================================================ */
	{
		AddInfo(FString::Printf(
			TEXT("EARLY EXIT POOLED: %d of %d phase-1 pivots happen after the problem first ")
			TEXT("reads feasible = %.4f%%. §5.2 predicted 'most of a standing structure's ")
			TEXT("saving'."),
			PooledSkippedPivots, PooledPhaseOnePivots,
			PooledPhaseOnePivots > 0
				? 100.0 * double(PooledSkippedPivots) / double(PooledPhaseOnePivots)
				: 0.0));

		TestEqual(
			TEXT("EARLY EXIT: the pooled phase-1 pivot count across the eight fixtures"),
			PooledPhaseOnePivots, 9245);

		TestEqual(
			TEXT("EARLY EXIT: the pooled pivots an early exit would SKIP — 58 of 9,245, ")
			TEXT("0.63%, which is the measurement that refutes §5.2's largest remaining ")
			TEXT("claim. Phase 1's objective IS the infeasibility sum, so there is no ")
			TEXT("optimality-proof tail to skip and the saving is already spent."),
			PooledSkippedPivots, 58);
	}

	/* ================================================================================
	 * THE GATE ARITHMETIC, written out where it is measured.
	 *
	 * PROMOTION_DESIGN §5.3's target: "~50 ms to fit two solves inside a 100 ms budget: 12x
	 * needed", against the 0.61 s the 84-block wall cost as a lambda* solve.
	 *
	 * Measured 2026-08-15: the same 84 blocks answer feasibility in 0.065 s, a 0.065 / 0.050
	 * = 1.3x gap, not 12x, with the reformulation alone and no warm start. Two solves are
	 * 0.130 s against 0.100 s: not met, short by less than any single unmeasured lever in
	 * §5.4/§5.5. The kill is conditioned on "cannot be brought under ~50 ms by the
	 * reformulation plus warm starts", and warm starts are unbuilt, so this does not fire it.
	 *
	 * This says nothing about whether 84 blocks is a legitimate unit of work; the second
	 * test measures that, and the answer is much worse.
	 *
	 * No timing is pinned — timings measure the machine. The one assertion below is an
	 * order-of-magnitude ceiling ~30x above the measurement, firing only on a catastrophe.
	 * ================================================================================ */
	{
		constexpr double GateCatastropheCeilingSeconds = 2.0;

		AddInfo(FString::Printf(
			TEXT("GATE ARITHMETIC: 84 blocks answer feasibility in %.4f s (lambda* pose ")
			TEXT("%.4f s). Target 0.050 s for one solve / 0.100 s for two. Measured shortfall ")
			TEXT("%.2fx. Design's stated shortfall was 12x."),
			GateDeadSeconds, GateLiveSeconds,
			GateDeadSeconds > 0.0 ? GateDeadSeconds / 0.050 : 0.0));

		TestTrue(
			*FString::Printf(
				TEXT("GATE ORDER OF MAGNITUDE: the 84-block feasibility solve must stay ")
				TEXT("under %.1f s — ~30x the 0.065 s measured, so this is a catastrophe ")
				TEXT("ceiling and not a timing pin — and took %.4f s"),
				GateCatastropheCeilingSeconds, GateDeadSeconds),
			GateDeadSeconds >= 0.0 && GateDeadSeconds < GateCatastropheCeilingSeconds);
	}

	/* ================================================================================
	 * THE EARLY EXIT — the seam, re-solved.
	 *
	 * PROMOTION_DESIGN §3.2/§5.2: a feasible problem is proved the moment phase 1's
	 * infeasibility sum reaches tolerance, supposedly where most of the saving lives. Not
	 * measurable from outside, so this block was the slice's one deliberate red, asserting
	 * two unset fields became populated:
	 *
	 *     PhaseOnePivots        how many of SimplexIterations phase 1 spent
	 *     PivotsToFirstFeasible the pivot at which the infeasibility sum first fell to
	 *                           InfeasibilityTolerance()
	 *
	 * The seam landed in the same slice, so the red and the population assertion are gone.
	 * What remains, worth 0.065 s: solved a second time the problem reports the same two
	 * numbers. The fields are read from solver state nothing else depends on, so an un-reset
	 * accumulation between solves would show here and nowhere else.
	 * ================================================================================ */
	{
		FStructure Wall;
		FString Why;

		const bool bLaid = SpikeBuildIntactWall(8, 10, Wall, Why);

		if (TestTrue(
				*FString::Printf(TEXT("early exit: the gate wall must lay (it said: %s)"), *Why),
				bLaid))
		{
			FOracleProblem Problem;

			const bool bBridged = BuildRigidBlockProblem(Wall, Problem, Why);

			if (TestTrue(
					*FString::Printf(TEXT("early exit: the bridge must represent it (%s)"), *Why),
					bBridged))
			{
				Problem.bGravityIsLive = false;

				const FOracleResult Result = SolveRigidBlock(Problem);

				TestTrue(
					*FString::Printf(
						TEXT("EARLY EXIT SEAM: the re-solve must ANSWER (it said: %s)"),
						*Result.WhyNot),
					Result.bAnswered);

				TestEqual(
					TEXT("EARLY EXIT SEAM: a second solve of the identical gate problem must ")
					TEXT("report the identical phase-1 pivot count (490) — the two fields are ")
					TEXT("read from state nothing else depends on, so a stale accumulation ")
					TEXT("between solves would show here first"),
					Result.PhaseOnePivots, 490);

				TestEqual(
					TEXT("EARLY EXIT SEAM: and the identical first-feasible pivot (487), so ")
					TEXT("the 0.61% saving on this fixture is reproducible rather than a ")
					TEXT("property of one solve's history"),
					Result.PivotsToFirstFeasible, 487);
			}
		}
	}

	return true;
}

/* ====================================================================================
 * TEST 2 — THE REGIONAL SANDWICH.
 *
 * COST: one global feasibility solve plus two region solves per radius, three per deletion
 * for the ten-deletion agreement ladder (the third is the whole cut wall, so its five
 * certificates are checked not just counted), the leaning-stack arms, and Part D's band in
 * both poses. Measured 2026-08-15: **7.2 s**, of which the ten global solves are ~half —
 * the whole regional question costs less than one mid-size wall's lambda* solve.
 *
 * Needs a ticking world: no.
 * ==================================================================================== */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleRegionalSandwichTest,
	"OracleSweepFast.RigidBlock.RegionalSandwich",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleRegionalSandwichTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace OracleFeasibilitySpikeSupport;

	/*
	 * A refusal is not an infeasibility, and nothing else here tells them apart.
	 * SpikeIsFeasible reads "answered and lambda >= 1"; Refuse() returns bAnswered = false,
	 * lambda = 0, bit-for-bit the dead-load infeasibility arm. A refused region solve would
	 * read as "no admissible equilibrium" — this test's finding — while every verdict pin,
	 * the conservation lemma, the closing radius and the 5-of-10 count stayed green. Not
	 * hypothetical: NumericalFailure refused 128-/200-block members a commit ago, and regions
	 * here are 44-149 blocks. Today's readings are clean (pivots 7-965, every WhyNot empty);
	 * this guard is the defence that was missing, not the verdict.
	 */
	const auto MustAnswer = [this](const FPoseReading& Read, const FString& Where)
	{
		return TestTrue(
			*FString::Printf(
				TEXT("%s: the solve must ANSWER — a REFUSAL and an INFEASIBILITY are ")
				TEXT("indistinguishable downstream (both are bAnswered=false, lambda=0), so ")
				TEXT("an unguarded refusal reads as 'no admissible equilibrium'. It said: %s"),
				*Where, Read.WhyNot.IsEmpty() ? TEXT("(nothing)") : *Read.WhyNot),
			Read.bAnswered);
	};

	/* ================================================================================
	 * PART A — THE RADIUS LADDER ON ONE WALL, ONE DELETION.
	 * ================================================================================ */

	constexpr int32 LadderCourses = 12;
	constexpr int32 LadderCells = 12;
	constexpr int32 LadderDeleteCourse = 6;

	/* Pins: INDEX_NONE is UNMEASURED and is this slice's red. */
	const FSandwichPins Pins;

	FStructure Wall;
	FString Why;

	const bool bLadderLaid = SpikeBuildIntactWall(LadderCourses, LadderCells, Wall, Why);

	if (!TestTrue(
			*FString::Printf(TEXT("sandwich: the ladder wall must lay (it said: %s)"), *Why),
			bLadderLaid))
	{
		return true;
	}

	double DeleteXCm = 0.0;
	double DeleteZCm = 0.0;
	const int32 Victim = SpikePieceInCourse(Wall, LadderDeleteCourse, DeleteXCm, DeleteZCm);

	if (!TestTrue(
			TEXT("sandwich: there must be a brick to delete in the chosen course"),
			Victim != INDEX_NONE))
	{
		return true;
	}

	if (!TestTrue(TEXT("sandwich: the brick must be removable"), Wall.RemovePiece(Victim)))
	{
		return true;
	}

	FOracleProblem Full;

	const bool bFullBridged = BuildRigidBlockProblem(Wall, Full, Why);

	if (!TestTrue(
			*FString::Printf(TEXT("sandwich: the bridge must represent the cut wall (%s)"), *Why),
			bFullBridged))
	{
		return true;
	}

	if (Pins.LadderBlocks == INDEX_NONE)
	{
		AddError(FString::Printf(
			TEXT("sandwich ladder: UNMEASURED SIZE — %d courses x %d cells with one brick ")
			TEXT("out at course %d gives %d blocks / %d joints; the deleted brick sat at ")
			TEXT("X = %.4f, Z = %.4f"),
			LadderCourses, LadderCells, LadderDeleteCourse,
			Full.Blocks.Num(), Full.Joints.Num(), DeleteXCm, DeleteZCm));
	}
	else
	{
		TestEqual(TEXT("sandwich ladder: block count"), Full.Blocks.Num(), Pins.LadderBlocks);
	}

	/* The truth the sandwich is judged against: the whole cut wall, posed dead. */
	FOracleProblem Global = Full;
	Global.bGravityIsLive = false;

	const FPoseReading GlobalRead = SpikeSolve(Global);
	const bool bGlobalFeasible = SpikeIsFeasible(GlobalRead);

	{
		const FString Line = FString::Printf(
			TEXT("SANDWICH GLOBAL: blocks=%d joints=%d feasible=%d lambda=%.17g pivots=%d ")
			TEXT("secs=%.3f whynot='%s'"),
			Full.Blocks.Num(), Full.Joints.Num(), bGlobalFeasible ? 1 : 0,
			GlobalRead.Lambda, GlobalRead.Pivots, GlobalRead.Seconds, *GlobalRead.WhyNot);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);
	}

	MustAnswer(GlobalRead, TEXT("sandwich ladder: the whole cut wall"));

	if (Pins.GlobalFeasible == INDEX_NONE)
	{
		AddError(FString::Printf(
			TEXT("sandwich ladder: UNMEASURED GLOBAL VERDICT — measured feasible=%d"),
			bGlobalFeasible ? 1 : 0));
	}
	else
	{
		TestEqual(TEXT("sandwich ladder: the global verdict"),
			bGlobalFeasible ? 1 : 0, Pins.GlobalFeasible);

		TestEqual(
			TEXT("sandwich ladder: the whole wall's feasibility pivot count, which is what ")
			TEXT("the regional lever has to beat"),
			GlobalRead.Pivots, Pins.GlobalPivots);
	}

	int32 ClosingRadius = INDEX_NONE;
	int32 ClosingRegionBlocks = INDEX_NONE;
	int32 ClosingOptimisticPivots = INDEX_NONE;
	int32 ClosingPessimisticPivots = INDEX_NONE;

	/*
	 * 2/4/8 is the design's ladder; 5/6 are added because it skips the answer. On a
	 * 12-course wall a radius-8 box around a mid-height brick IS the whole wall, so
	 * "closes at 8" would be indistinguishable from "closes only at the whole structure".
	 * The brick is course 6 and the shell reaches one course past the core, so the first
	 * radius touching the foundation is 5 — where the conservation lemma says the free side
	 * can first certify. MEASURED 2026-08-15: the closing region is 142 of 149.
	 */
	TArray<FRadiusPin> RadiusPins;
	RadiusPins.Add({ 2,  44, 1, 0 });
	RadiusPins.Add({ 4, 114, 1, 0 });
	RadiusPins.Add({ 5, 142, 1, 1 });
	RadiusPins.Add({ 6, 149, 1, 1 });
	RadiusPins.Add({ 8, 149, 1, 1 });

	for (const FRadiusPin& Pin : RadiusPins)
	{
		const int32 Radius = Pin.Radius;

		TArray<bool> Mask;
		SpikeBoxMask(Full, DeleteXCm, DeleteZCm, Radius, Radius, Mask);

		FOracleProblem Optimistic;
		FRegionCounts OptimisticCounts;
		SpikeExtractRegion(Full, Mask, /*bGroundTheShell*/ true, Optimistic, OptimisticCounts);

		FOracleProblem Pessimistic;
		FRegionCounts PessimisticCounts;
		SpikeExtractRegion(Full, Mask, /*bGroundTheShell*/ false, Pessimistic, PessimisticCounts);

		const FPoseReading OptimisticRead = SpikeSolve(Optimistic);
		const FPoseReading PessimisticRead = SpikeSolve(Pessimistic);

		const bool bOptimistic = SpikeIsFeasible(OptimisticRead);
		const bool bPessimistic = SpikeIsFeasible(PessimisticRead);
		const bool bCloses = bOptimistic == bPessimistic;

		if (bCloses && ClosingRadius == INDEX_NONE)
		{
			ClosingRadius = Radius;
			ClosingRegionBlocks = OptimisticCounts.Blocks;
			ClosingOptimisticPivots = OptimisticRead.Pivots;
			ClosingPessimisticPivots = PessimisticRead.Pivots;
		}

		const FString Line = FString::Printf(
			TEXT("SANDWICH r=%d: core=%d shell=%d blocks=%d joints=%d | GROUNDED-boundary ")
			TEXT("grounded=%d feasible=%d pivots=%d secs=%.3f whynot='%s' | FREE-boundary ")
			TEXT("grounded=%d feasible=%d pivots=%d secs=%.3f whynot='%s' | closes=%d"),
			Radius, OptimisticCounts.Core, OptimisticCounts.Shell, OptimisticCounts.Blocks,
			OptimisticCounts.Joints, OptimisticCounts.Grounded, bOptimistic ? 1 : 0,
			OptimisticRead.Pivots, OptimisticRead.Seconds, *OptimisticRead.WhyNot,
			PessimisticCounts.Grounded, bPessimistic ? 1 : 0,
			PessimisticRead.Pivots, PessimisticRead.Seconds, *PessimisticRead.WhyNot,
			bCloses ? 1 : 0);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);

		/* ---- Both solves must have ANSWERED before any verdict is read off them. ---- */

		MustAnswer(OptimisticRead,
			FString::Printf(TEXT("r=%d GROUNDED-boundary region"), Radius));
		MustAnswer(PessimisticRead,
			FString::Printf(TEXT("r=%d FREE-boundary region"), Radius));

		/* ---- The rung's own pins: size first, then both verdicts. ---- */

		TestEqual(
			*FString::Printf(TEXT("r=%d: region block count"), Radius),
			OptimisticCounts.Blocks, Pin.Blocks);

		TestEqual(
			*FString::Printf(
				TEXT("r=%d: the GROUNDED-boundary (optimistic) verdict — infeasible here ")
				TEXT("would certify that the whole structure is infeasible"),
				Radius),
			bOptimistic ? 1 : 0, Pin.Optimistic);

		TestEqual(
			*FString::Printf(
				TEXT("r=%d: the FREE-boundary (pessimistic) verdict — feasible here is what ")
				TEXT("the design says certifies the whole structure"),
				Radius),
			bPessimistic ? 1 : 0, Pin.Pessimistic);

		/*
		 * THE CONSERVATION LEMMA, MADE EXECUTABLE. Sum the vertical equilibrium rows of a
		 * free-boundary region: every internal contact force cancels pairwise and every
		 * joint leaving the region has been dropped, so what remains is 0 = -W_region. A
		 * region with no grounded block therefore has NO admissible force system at any
		 * radius, in any wall, for any deletion. This is the one line that decides whether
		 * the sandwich can ever certify a region that does not reach the ground, so it is
		 * asserted rather than argued.
		 *
		 * It is also the line the refusal guard above protects: a REFUSED solve satisfies
		 * !bPessimistic just as an infeasible one does, so without MustAnswer this lemma
		 * would read as confirmed by a solve that never happened.
		 */
		if (PessimisticCounts.Grounded == 0)
		{
			TestTrue(
				*FString::Printf(
					TEXT("r=%d: a FREE-boundary region containing no grounded block cannot be ")
					TEXT("feasible — summing its vertical rows gives 0 = -W_region — and it ")
					TEXT("read feasible=%d"),
					Radius, bPessimistic ? 1 : 0),
				!bPessimistic);
		}

		/*
		 * AND WHERE THE SANDWICH CLOSES ON THIS FIXTURE, IT AGREES WITH THE WHOLE WALL.
		 * Pinned as a characterisation of THIS fixture and deliberately not as an
		 * invariant: PART D measures a closed sandwich whose certificate is WRONG, so
		 * "closure implies correctness" is known to be false in general and a test that
		 * asserted it as a law would be asserting the design's claim rather than checking
		 * it.
		 */
		if (bCloses)
		{
			TestTrue(
				*FString::Printf(
					TEXT("r=%d: on THIS fixture the closed sandwich reads feasible=%d and the ")
					TEXT("whole wall reads feasible=%d — see PART D for the fixture where a ")
					TEXT("closed sandwich certifies the wrong answer"),
					Radius, bOptimistic ? 1 : 0, bGlobalFeasible ? 1 : 0),
				bOptimistic == bGlobalFeasible);
		}
	}

	if (Pins.ClosingRadius == INDEX_NONE)
	{
		AddError(FString::Printf(
			TEXT("sandwich ladder: UNMEASURED CLOSING RADIUS — measured %d at %d blocks ")
			TEXT("(%d + %d pivots); INDEX_NONE means the sandwich did not close at any ")
			TEXT("radius in the ladder"),
			ClosingRadius, ClosingRegionBlocks,
			ClosingOptimisticPivots, ClosingPessimisticPivots));
	}
	else
	{
		TestEqual(TEXT("sandwich ladder: the closing radius"),
			ClosingRadius, Pins.ClosingRadius);

		/*
		 * How big the region is when it closes — the whole point of a decomposition lever.
		 * A radius without its block count says nothing about neighbourhood vs building.
		 */
		TestEqual(
			TEXT("sandwich ladder: the region at the closing radius is 142 of the wall's ")
			TEXT("149 blocks — a decomposition that must take 95% of the structure before ")
			TEXT("it can certify is not a decomposition"),
			ClosingRegionBlocks, Pins.ClosingRegionBlocks);

		TestEqual(TEXT("sandwich ladder: optimistic pivots at the closing radius"),
			ClosingOptimisticPivots, Pins.ClosingOptimisticPivots);

		TestEqual(TEXT("sandwich ladder: pessimistic pivots at the closing radius"),
			ClosingPessimisticPivots, Pins.ClosingPessimisticPivots);

		/*
		 * The cost comparison, in pivots (deterministic; seconds are not). The sandwich buys
		 * locality with two solves; at the closing radius they cost 753 + 872 = 1,625 pivots
		 * against 965 for the whole wall — 1.68x more than the work it avoids. A strict
		 * inequality, not a ratio, so it states the finding, not a measurement's last digits.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("sandwich ladder: THE SANDWICH COSTS MORE THAN THE GLOBAL SOLVE — its ")
				TEXT("two solves at the closing radius take %d + %d = %d pivots against the ")
				TEXT("whole wall's %d. If this ever reverses, the lever has become worth ")
				TEXT("something and this line is where that is noticed."),
				ClosingOptimisticPivots, ClosingPessimisticPivots,
				ClosingOptimisticPivots + ClosingPessimisticPivots, GlobalRead.Pivots),
			ClosingOptimisticPivots + ClosingPessimisticPivots > GlobalRead.Pivots);
	}

	/* ================================================================================
	 * PART B — R2's question: ten deletions, radius fixed at 4.
	 *
	 * If the free-boundary side never certifies, the sandwich degenerates to "always grow to
	 * the whole structure" and the lever is worthless. This is the count that answers it. The
	 * deletions are a fixed ladder over courses, not a sample: same ten bricks every run,
	 * each printed, so a disagreeing row lifts straight out into a named regression fixture.
	 * ================================================================================ */

	constexpr int32 AgreementRadius = 4;

	int32 Agreements = 0;

	for (int32 Course = 1; Course <= 10; ++Course)
	{
		FStructure Cut;
		FString CutWhy;

		const bool bCutLaid = SpikeBuildIntactWall(LadderCourses, LadderCells, Cut, CutWhy);

		if (!TestTrue(
				*FString::Printf(TEXT("agreement course %d: the wall must lay (%s)"),
					Course, *CutWhy),
				bCutLaid))
		{
			continue;
		}

		double XCm = 0.0;
		double ZCm = 0.0;
		const int32 Brick = SpikePieceInCourse(Cut, Course, XCm, ZCm);
		const bool bRemoved = Brick != INDEX_NONE && Cut.RemovePiece(Brick);

		if (!TestTrue(
				*FString::Printf(TEXT("agreement course %d: a brick must exist there"), Course),
				bRemoved))
		{
			continue;
		}

		FOracleProblem CutProblem;

		const bool bCutBridged = BuildRigidBlockProblem(Cut, CutProblem, CutWhy);

		if (!TestTrue(
				*FString::Printf(TEXT("agreement course %d: the bridge must represent it (%s)"),
					Course, *CutWhy),
				bCutBridged))
		{
			continue;
		}

		TArray<bool> Mask;
		SpikeBoxMask(CutProblem, XCm, ZCm, AgreementRadius, AgreementRadius, Mask);

		FOracleProblem Optimistic;
		FRegionCounts OptimisticCounts;
		SpikeExtractRegion(CutProblem, Mask, true, Optimistic, OptimisticCounts);

		FOracleProblem Pessimistic;
		FRegionCounts PessimisticCounts;
		SpikeExtractRegion(CutProblem, Mask, false, Pessimistic, PessimisticCounts);

		/*
		 * The whole cut wall, solved per deletion: a certificate is only worth its answer.
		 * Part A checks its closed sandwich against the global verdict and Part B did not,
		 * leaving five certificates of nothing. One 149-block dead solve per course.
		 */
		FOracleProblem CutGlobal = CutProblem;
		CutGlobal.bGravityIsLive = false;

		const FPoseReading CutGlobalRead = SpikeSolve(CutGlobal);
		const bool bCutGlobalFeasible = SpikeIsFeasible(CutGlobalRead);

		const FPoseReading OptimisticRead = SpikeSolve(Optimistic);
		const FPoseReading PessimisticRead = SpikeSolve(Pessimistic);

		const bool bOptimistic = SpikeIsFeasible(OptimisticRead);
		const bool bPessimistic = SpikeIsFeasible(PessimisticRead);
		const bool bAgree = bOptimistic == bPessimistic;

		if (bAgree)
		{
			++Agreements;
		}

		const FString Line = FString::Printf(
			TEXT("SANDWICH AGREEMENT course=%d (brick at X=%.4f Z=%.4f): blocks=%d ")
			TEXT("grounded(grounded-boundary)=%d grounded(free-boundary)=%d | optimistic=%d ")
			TEXT("pessimistic=%d agree=%d | global=%d | secs %.3f + %.3f | whynot opt='%s' ")
			TEXT("pess='%s' global='%s'"),
			Course, XCm, ZCm, OptimisticCounts.Blocks, OptimisticCounts.Grounded,
			PessimisticCounts.Grounded, bOptimistic ? 1 : 0, bPessimistic ? 1 : 0,
			bAgree ? 1 : 0, bCutGlobalFeasible ? 1 : 0,
			OptimisticRead.Seconds, PessimisticRead.Seconds,
			*OptimisticRead.WhyNot, *PessimisticRead.WhyNot, *CutGlobalRead.WhyNot);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);

		MustAnswer(OptimisticRead,
			FString::Printf(TEXT("agreement course %d GROUNDED-boundary region"), Course));
		MustAnswer(PessimisticRead,
			FString::Printf(TEXT("agreement course %d FREE-boundary region"), Course));
		MustAnswer(CutGlobalRead,
			FString::Printf(TEXT("agreement course %d whole cut wall"), Course));

		/*
		 * Where this deletion's sandwich closes, what it certifies must hold. Same scope as
		 * Part A's line: a characterisation of these ten fixtures, not the law "closure
		 * implies correctness" (Part D has a closed sandwich wrong by ~40x). If one of these
		 * five ever certifies wrong, this line says so.
		 */
		if (bAgree)
		{
			TestTrue(
				*FString::Printf(
					TEXT("agreement course %d: the closed sandwich certifies feasible=%d and ")
					TEXT("the whole cut wall reads feasible=%d"),
					Course, bOptimistic ? 1 : 0, bCutGlobalFeasible ? 1 : 0),
				bOptimistic == bCutGlobalFeasible);
		}
	}

	if (Pins.Agreements == INDEX_NONE)
	{
		AddError(FString::Printf(
			TEXT("sandwich agreement: UNMEASURED — at radius %d over ten deletions the two ")
			TEXT("sides agreed %d times. Revision 1 of the derivation record predicted 4 ")
			TEXT("(courses 1-4, the ones whose region reaches the ground)."),
			AgreementRadius, Agreements));
	}
	else
	{
		TestEqual(TEXT("sandwich agreement: how many of ten deletions certify at radius 4"),
			Agreements, Pins.Agreements);
	}

	/* ================================================================================
	 * PART C — the other arm: a structure that genuinely has no equilibrium.
	 *
	 * Every wall fixture is LP-feasible, so Parts A and B only exercise the standing side.
	 * The 30-course stack is the one cheap sub-1.0 fixture (lambda* 0.4405), and it is a
	 * chain, so "radius in courses" is exact. Measured: the smallest radius at which the
	 * optimistic side (boundary grounded, an artificial foundation both ends) is still
	 * infeasible — the radius at which a region can certify a collapse.
	 * ================================================================================ */

	FStructure Stack;
	FString StackWhy;

	const bool bStackLaid = SpikeBuildLeaningStack(30, Stack, StackWhy);

	if (TestTrue(
			*FString::Printf(TEXT("stack sandwich: the 30-course stack must lay (%s)"), *StackWhy),
			bStackLaid))
	{
		FOracleProblem StackProblem;

		const bool bStackBridged = BuildRigidBlockProblem(Stack, StackProblem, StackWhy);

		if (TestTrue(
				*FString::Printf(TEXT("stack sandwich: the bridge must represent it (%s)"),
					*StackWhy),
				bStackBridged))
		{
			const double MiddleZCm =
				SpikeBrickHeightCm / 2.0 + 15.0 * SpikeCoursePitchCm;

			int32 CertifyingRadius = INDEX_NONE;
			int32 CertifyingRegionBlocks = INDEX_NONE;

			const int32 StackRadii[] = { 2, 4, 8, 10, 12, 13, 14, 15 };

			for (const int32 Radius : StackRadii)
			{
				TArray<bool> Mask;
				SpikeCourseBandMask(StackProblem, MiddleZCm, Radius, Mask);

				FOracleProblem Optimistic;
				FRegionCounts OptimisticCounts;
				SpikeExtractRegion(StackProblem, Mask, true, Optimistic, OptimisticCounts);

				FOracleProblem Pessimistic;
				FRegionCounts PessimisticCounts;
				SpikeExtractRegion(StackProblem, Mask, false, Pessimistic, PessimisticCounts);

				const FPoseReading OptimisticRead = SpikeSolve(Optimistic);
				const FPoseReading PessimisticRead = SpikeSolve(Pessimistic);

				const bool bOptimistic = SpikeIsFeasible(OptimisticRead);
				const bool bPessimistic = SpikeIsFeasible(PessimisticRead);

				if (!bOptimistic && CertifyingRadius == INDEX_NONE)
				{
					CertifyingRadius = Radius;
					CertifyingRegionBlocks = OptimisticCounts.Blocks;
				}

				const FString Line = FString::Printf(
					TEXT("STACK SANDWICH r=%d: core=%d shell=%d blocks=%d joints=%d | ")
					TEXT("optimistic feasible=%d pivots=%d | pessimistic feasible=%d ")
					TEXT("pivots=%d | secs %.3f + %.3f | whynot opt='%s' pess='%s'"),
					Radius, OptimisticCounts.Core, OptimisticCounts.Shell,
					OptimisticCounts.Blocks, OptimisticCounts.Joints,
					bOptimistic ? 1 : 0, OptimisticRead.Pivots,
					bPessimistic ? 1 : 0, PessimisticRead.Pivots,
					OptimisticRead.Seconds, PessimisticRead.Seconds,
					*OptimisticRead.WhyNot, *PessimisticRead.WhyNot);

				UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
				AddInfo(Line);

				/*
				 * The certifying radius is read off the optimistic side going infeasible,
				 * so a refusal here would manufacture a certificate from a failed solve —
				 * the most misleading direction in this test.
				 */
				MustAnswer(OptimisticRead,
					FString::Printf(TEXT("stack r=%d GROUNDED-boundary region"), Radius));
				MustAnswer(PessimisticRead,
					FString::Printf(TEXT("stack r=%d FREE-boundary region"), Radius));
			}

			if (Pins.StackCertifyingRadius == INDEX_NONE)
			{
				AddError(FString::Printf(
					TEXT("stack sandwich: UNMEASURED — the smallest radius at which the ")
					TEXT("OPTIMISTIC region is still infeasible (and so certifies the ")
					TEXT("collapse) measured %d, at %d blocks"),
					CertifyingRadius, CertifyingRegionBlocks));
			}
			else
			{
				TestEqual(TEXT("stack sandwich: the certifying radius"),
					CertifyingRadius, Pins.StackCertifyingRadius);

				/*
				 * The region at that radius is the whole structure — all 30 blocks. The
				 * optimistic side is still feasible at radius 13 (29 of 30), so no proper
				 * subset can certify the collapse: for a global mechanism there is no smaller
				 * region, by measurement.
				 */
				TestEqual(
					TEXT("stack sandwich: the certifying region is all 30 blocks — the WHOLE ")
					TEXT("structure; the 29-block region one radius below is still feasible"),
					CertifyingRegionBlocks, Pins.StackCertifyingRegionBlocks);

				TestEqual(
					TEXT("stack sandwich: and 30 blocks IS the whole fixture, so 'certifies ")
					TEXT("at radius 14' means 'certifies only with everything'"),
					CertifyingRegionBlocks, StackProblem.Blocks.Num());
			}

			/* ========================================================================
			 * PART D — the free boundary is not pessimistic, and a closed sandwich can
			 * certify the wrong answer.
			 *
			 * PROMOTION_DESIGN §5.3 justifies the free-boundary side as "if this problem
			 * is feasible, the full structure is, because the found force system extends."
			 * It does not: dropping material outside the region removes its restraint
			 * (pessimistic) and its weight (optimistic) together, so the free-boundary
			 * region is a bound in neither direction.
			 *
			 * The cheapest witness is the 30-course stack (lambda* = 0.4405, no whole-stack
			 * equilibrium). Its bottom band — courses 0..5, grounded, the 24 above dropped
			 * — is feasible with a free boundary (a short stack on the ground, 5-course
			 * fixture reads 31.2) and with a grounded one, so the sandwich closes and
			 * certifies "the full structure is feasible". It is not. The three measured
			 * facts (band feasible both ways, whole stack not) are the refutation.
			 *
			 * A boolean trio only says "wrong", and §9.5 forbids a design-invalidating row
			 * resting on an argued bite, so two things were added: the band's own live
			 * lambda* (turning "wrong" into "wrong by a factor" no extractor tightening
			 * closes), and X6 (an extractor keeping out-of-region blocks must turn the band
			 * infeasible).
			 *
			 * RE-AIMED 2026-08-15 when the repair landed. The rows above still measure the
			 * unrepaired extractor (the 41.96x false certificate must stay executable); the
			 * rows below measure the repaired extractor on the identical band and claim the
			 * repaired band reads infeasible and so agrees with the whole stack — no
			 * certificate issued. Neither half may be tuned to restore the other's green.
			 *
			 * A lesson from the forward hazard below, which came half true: its physics was
			 * right (the repaired band is infeasible) but its suite claim was wrong. The
			 * repair landed as a new function (SpikeExtractRepairedRegion) beside an
			 * untouched SpikeExtractRegion, so Part D kept calling the old one and stayed
			 * green; the re-aim was made deliberately, not forced. Generally: a pin notices
			 * only changes going through the code it calls, so a repair built beside the
			 * thing it repairs is invisible to every pin on the original.
			 *
			 * The hazard as it stood, kept verbatim so the miss is legible:
			 *
			 * FORWARD HAZARD, AND IT IS NOT A DEFECT — READ THIS BEFORE "FIXING" THIS ROW.
			 * PART D's GREEN ENCODES TODAY'S UNREPAIRED EXTRACTOR. The user-ruled repair
			 * (§12 D2': the pessimistic side keeps the omitted material's WEIGHT as a dead
			 * surcharge while removing its restraint) makes this bottom band INFEASIBLE, and
			 * "PART D: the bottom band ... must be FEASIBLE" therefore goes red BY
			 * CONSTRUCTION on the day the repair lands. That is this pin working — it is the
			 * only thing in the suite that would notice the repair actually changed the
			 * region's answer. It must then be RE-AIMED deliberately, at "the REPAIRED band
			 * agrees with the whole stack", and never patched or deleted to restore green.
			 * ======================================================================== */
			{
				/* Core = courses 0..4, shell = course 5; twenty-four courses dropped. */
				constexpr int32 BandCentreCourse = 2;
				constexpr int32 BandRadius = 2;

				const double BandCentreZCm = SpikeBrickHeightCm / 2.0
					+ double(BandCentreCourse) * SpikeCoursePitchCm;

				TArray<bool> BandMask;
				SpikeCourseBandMask(StackProblem, BandCentreZCm, BandRadius, BandMask);

				FOracleProblem BandFree;
				FRegionCounts BandFreeCounts;
				SpikeExtractRegion(StackProblem, BandMask, false, BandFree, BandFreeCounts);

				FOracleProblem BandGrounded;
				FRegionCounts BandGroundedCounts;
				SpikeExtractRegion(StackProblem, BandMask, true, BandGrounded, BandGroundedCounts);

				const FPoseReading BandFreeRead = SpikeSolve(BandFree);
				const FPoseReading BandGroundedRead = SpikeSolve(BandGrounded);

				FOracleProblem WholeStack = StackProblem;
				WholeStack.bGravityIsLive = false;
				const FPoseReading WholeRead = SpikeSolve(WholeStack);

				/*
				 * The same free-boundary band posed live, the only way to ask how feasible
				 * the false certificate's region is. One flag apart from the problem above,
				 * copying not rebuilding so it does not compare two problems.
				 */
				FOracleProblem BandFreeLive = BandFree;
				BandFreeLive.bGravityIsLive = true;
				const FPoseReading BandLiveRead = SpikeSolve(BandFreeLive);

				const bool bBandFree = SpikeIsFeasible(BandFreeRead);
				const bool bBandGrounded = SpikeIsFeasible(BandGroundedRead);
				const bool bWhole = SpikeIsFeasible(WholeRead);

				const FString Line = FString::Printf(
					TEXT("STACK BOTTOM BAND: core=%d shell=%d blocks=%d joints=%d ")
					TEXT("grounded(free)=%d | free-boundary feasible=%d | grounded-boundary ")
					TEXT("feasible=%d | WHOLE STACK feasible=%d | sandwich closes=%d | band ")
					TEXT("LIVE lambda*=%.17g answered=%d pivots=%d | whynot free='%s' ")
					TEXT("grounded='%s' whole='%s' live='%s'"),
					BandFreeCounts.Core, BandFreeCounts.Shell, BandFreeCounts.Blocks,
					BandFreeCounts.Joints, BandFreeCounts.Grounded, bBandFree ? 1 : 0,
					bBandGrounded ? 1 : 0, bWhole ? 1 : 0,
					bBandFree == bBandGrounded ? 1 : 0,
					BandLiveRead.Lambda, BandLiveRead.bAnswered ? 1 : 0, BandLiveRead.Pivots,
					*BandFreeRead.WhyNot, *BandGroundedRead.WhyNot, *WholeRead.WhyNot,
					*BandLiveRead.WhyNot);

				UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
				AddInfo(Line);

				MustAnswer(BandFreeRead, TEXT("PART D: the band with a FREE boundary"));
				MustAnswer(BandGroundedRead, TEXT("PART D: the band with a GROUNDED boundary"));
				MustAnswer(WholeRead, TEXT("PART D: the whole 30-course stack"));
				MustAnswer(BandLiveRead, TEXT("PART D: the band posed LIVE"));

				/* The band's size, pinned before anything is read off it. */
				TestEqual(
					TEXT("PART D: the band is 6 blocks — five core courses and one shell"),
					BandFreeCounts.Blocks, Pins.BandBlocks);

				TestTrue(
					TEXT("PART D: the bottom band solved with a FREE boundary must be ")
					TEXT("FEASIBLE — it is a short leaning stack standing on the ground, and ")
					TEXT("dropping the twenty-four courses above it drops their WEIGHT as well ")
					TEXT("as their restraint"),
					bBandFree);

				TestTrue(
					TEXT("PART D: the same band with a GROUNDED boundary must also be feasible ")
					TEXT("— so the two sides AGREE and the design's rule certifies"),
					bBandGrounded);

				TestTrue(
					TEXT("PART D: and the WHOLE stack must be INFEASIBLE (lambda* = 0.4405). ")
					TEXT("These three lines together are the refutation: a closed sandwich ")
					TEXT("certified 'the full structure is feasible' about a structure that has ")
					TEXT("no admissible equilibrium. The free-boundary side is not a ")
					TEXT("pessimistic bound and PROMOTION_DESIGN §5.3's 'the found force ")
					TEXT("system extends' does not hold."),
					!bWhole);

				/*
				 * How wrong the certificate is, as a number. The band certifies "the full
				 * structure is feasible" about a stack whose lambda* is 0.4405; the band's own
				 * lambda* says by how much. A boolean trio only says the certificate is on the
				 * wrong side of 1.0; this says the region is nowhere near the boundary, so the
				 * failure is not a knife edge a tighter extractor could tune away.
				 *
				 * Pinned in the file-wide +/-2e-5 window, measured not derived: the 5-course
				 * fixture reads 31.2, and this band is a different problem (six blocks, top
				 * course free of any load it used to carry).
				 */
				if (Pins.BandLiveLambdaLo < 0.0)
				{
					AddError(FString::Printf(
						TEXT("PART D: UNMEASURED BAND lambda* — the free-boundary band's live ")
						TEXT("pose read %.17g in %d pivots. Pin it, then state the ratio ")
						TEXT("against the whole stack's 0.4405."),
						BandLiveRead.Lambda, BandLiveRead.Pivots));
				}
				else
				{
					TestTrue(
						*FString::Printf(
							TEXT("PART D: the certified band's OWN lambda* must lie in ")
							TEXT("[%.9g, %.9g] and was %.17g. The whole stack is 0.4405, so ")
							TEXT("the false certificate is not a knife edge — the region the ")
							TEXT("sandwich certified stands at many times its own weight ")
							TEXT("while the structure it certified falls under one. No ")
							TEXT("tightening of the extractor closes a gap of that size; only ")
							TEXT("carrying the omitted weight does (§12 D2')."),
							Pins.BandLiveLambdaLo, Pins.BandLiveLambdaHi, BandLiveRead.Lambda),
						BandLiveRead.Lambda >= Pins.BandLiveLambdaLo
							&& BandLiveRead.Lambda <= Pins.BandLiveLambdaHi);
				}

				/* ----------------------------------------------------------------
				 * THE RE-AIM: THE SAME BAND, REPAIRED.
				 *
				 * One thing changes, repair (1): the 24 omitted courses' weight is applied
				 * as a dead surcharge on the vertical through their centre of gravity, while
				 * they contribute no restraint (no equilibrium row, no joint row). Blocks,
				 * joints and boundary are identical to the free-boundary problem above, so a
				 * difference in the answer can only be the surcharge.
				 *
				 * What must now be true, the claim the repair exists to make: the repaired
				 * band agrees with the whole stack. The 41.96x certificate is not tightened
				 * but withdrawn — the two sides disagree (optimistic feasible, pessimistic
				 * infeasible), which means grow the region. A repair leaving the band feasible
				 * would have changed the arithmetic, not the answer.
				 * ---------------------------------------------------------------- */
				{
					FOracleProblem RepairedBand;
					FRegionCounts RepairedCounts;
					FSurchargeCounts RepairedSurcharge;

					SpikeExtractRepairedRegion(
						StackProblem, BandMask, /*bGroundTheShell*/ false,
						ESpikeSurcharge::Carried,
						RepairedBand, RepairedCounts, RepairedSurcharge);

					const FPoseReading RepairedRead = SpikeSolve(RepairedBand);
					const bool bRepaired = SpikeIsFeasible(RepairedRead);

					/*
					 * The omitted courses' weight, derived here from the brick mass and count
					 * rather than read off the extractor, so a surcharge charging the wrong set
					 * cannot agree with a total it computed itself.
					 */
					const double ExpectedChargedUu =
						24.0 * SpikeBrickMassKg * OracleGravityCmPerSecondSquared;

					/*
					 * Where it acts, derived a second time from the fixture builder's own
					 * expression. Placement is the load-bearing half of repair (1): the force
					 * is easy, the moment is what the refuted form lost. Mutation X8 moves the
					 * load to the contact's X (~55 cm) and flips the verdict, but a wrong-but-
					 * nearer X might not flip anything and pass in silence. So the line of
					 * action is recomputed from SpikeBuildLeaningStack's "double(Course) * 10.0"
					 * over the charged courses, not the extractor's weighted mean. Two
					 * derivations, one number: 175.0 cm, 120 cm outboard of the band's joint at X = 55.
					 */
					double IndependentWeightUu = 0.0;
					double IndependentMomentUuCm = 0.0;

					for (int32 Course = 6; Course < 30; ++Course)
					{
						const double CourseWeightUu =
							SpikeBrickMassKg * OracleGravityCmPerSecondSquared;

						IndependentWeightUu += CourseWeightUu;
						IndependentMomentUuCm += CourseWeightUu * (double(Course) * 10.0);
					}

					const double ExpectedLineXCm = IndependentMomentUuCm / IndependentWeightUu;

					const FString RepairLine = FString::Printf(
						TEXT("STACK BOTTOM BAND, REPAIRED: blocks=%d joints=%d charged=%d ")
						TEXT("components=%d interface=%d orphans=%d chargedW=%.9g ")
						TEXT("appliedW=%.9g discardedOntoGrounded=%.9g forces=%d | free+")
						TEXT("surcharge feasible=%d pivots=%d secs=%.3f | WHOLE STACK ")
						TEXT("feasible=%d | whynot='%s'"),
						RepairedCounts.Blocks, RepairedCounts.Joints,
						RepairedSurcharge.ChargedBlocks, RepairedSurcharge.ChargedComponents,
						RepairedSurcharge.InterfaceJoints, RepairedSurcharge.OrphanComponents,
						RepairedSurcharge.ChargedWeightUu, RepairedSurcharge.AppliedWeightUu,
						RepairedSurcharge.DiscardedOntoGroundedUu,
						RepairedBand.AppliedForces.Num(), bRepaired ? 1 : 0,
						RepairedRead.Pivots, RepairedRead.Seconds, bWhole ? 1 : 0,
						*RepairedRead.WhyNot);

					UE_LOG(LogTemp, Display, TEXT("%s"), *RepairLine);
					AddInfo(RepairLine);

					MustAnswer(RepairedRead, TEXT("PART D: the REPAIRED band"));

					TestEqual(
						TEXT("PART D repaired: the region itself is unchanged — same six ")
						TEXT("blocks, so the only difference from the row above is the ")
						TEXT("surcharge"),
						RepairedCounts.Blocks, BandFreeCounts.Blocks);

					TestEqual(
						TEXT("PART D repaired: the twenty-four courses above the band cannot ")
						TEXT("reach the ground without it, so all twenty-four are charged"),
						RepairedSurcharge.ChargedBlocks, 24);

					TestEqual(
						TEXT("PART D repaired: they are one connected component and it meets ")
						TEXT("the band at exactly one joint"),
						RepairedSurcharge.InterfaceJoints, 1);

					TestEqual(
						TEXT("PART D repaired: no charged component may be an ORPHAN — a ")
						TEXT("component with no joint into the region loses its weight in ")
						TEXT("silence, which is the refuted form coming back by the back door"),
						RepairedSurcharge.OrphanComponents, 0);

					TestEqual(
						TEXT("PART D repaired: and none of it may land on a GROUNDED region ")
						TEXT("block, which writes no equilibrium row and would swallow it"),
						RepairedSurcharge.DiscardedOntoGroundedUu, 0.0);

					TestTrue(
						*FString::Printf(
							TEXT("PART D repaired: the applied surcharge must equal the ")
							TEXT("charged blocks' own weight, %.9g uu, and was %.9g uu"),
							ExpectedChargedUu, RepairedSurcharge.AppliedWeightUu),
						FMath::Abs(RepairedSurcharge.AppliedWeightUu - ExpectedChargedUu)
							<= 1.0e-9 * ExpectedChargedUu);

					/* ---- THE LINE OF ACTION, WHICH IS THE REPAIR'S ACTUAL CONTENT. ---- */

					if (TestEqual(
							TEXT("PART D repaired: one component meeting the band at one joint ")
							TEXT("makes exactly one applied force"),
							RepairedBand.AppliedForces.Num(), 1))
					{
						const FOracleAppliedForce& Applied = RepairedBand.AppliedForces[0];

						TestTrue(
							TEXT("PART D repaired: the surcharge is DEAD — a live surcharge ")
							TEXT("would scale with lambda and the pose would stop being the ")
							TEXT("feasibility question it is posed as"),
							!Applied.bLive);

						TestEqual(
							TEXT("PART D repaired: the omitted material's weight is vertical, ")
							TEXT("so the surcharge carries no horizontal component"),
							Applied.ForceXUu, 0.0);

						TestTrue(
							*FString::Printf(
								TEXT("PART D repaired: and it acts DOWNWARD at %.9g uu against ")
								TEXT("the charged weight %.9g uu"),
								-Applied.ForceZUu, ExpectedChargedUu),
							FMath::Abs(-Applied.ForceZUu - ExpectedChargedUu)
								<= 1.0e-9 * ExpectedChargedUu);

						TestTrue(
							*FString::Printf(
								TEXT("PART D repaired, THE LINE OF ACTION: the surcharge must ")
								TEXT("act on the vertical through the CHARGED COMPONENT'S OWN ")
								TEXT("CENTRE OF GRAVITY — independently derived here as %.9g cm ")
								TEXT("from the fixture builder's own course spacing — and it ")
								TEXT("acted at %.9g cm. The contact it is delivered through ")
								TEXT("sits at X = 55, so this pin is what separates the repair ")
								TEXT("from the mutation that keeps the force and loses the ")
								TEXT("moment. A boolean verdict alone cannot: a wrong X nearer ")
								TEXT("than the contact's might not flip the band at all."),
								ExpectedLineXCm, Applied.AtXCm),
							FMath::Abs(Applied.AtXCm - ExpectedLineXCm)
								<= 1.0e-9 * FMath::Abs(ExpectedLineXCm));
					}

					TestTrue(
						TEXT("PART D repaired, THE CLAIM: the band that falsely certified a ")
						TEXT("collapsing stack must now read INFEASIBLE, so it AGREES with the ")
						TEXT("whole stack instead of contradicting it. The false certificate is ")
						TEXT("withdrawn rather than tightened — the two sides now disagree, ")
						TEXT("which means GROW THE REGION and issue no certificate at all."),
						!bRepaired);

					TestTrue(
						TEXT("PART D repaired: stated as the agreement itself, so the row says ")
						TEXT("what it is for — the repaired band's verdict equals the whole ")
						TEXT("stack's verdict"),
						bRepaired == bWhole);
				}
			}
		}
	}

	return true;
}

/* ====================================================================================
 * TEST 3 — THE REPAIRED REGIONAL SANDWICH (PROMOTION_DESIGN §12 D2', ruled 2026-08-15).
 *
 * WHAT WAS REFUTED, so nothing rebuilds it. The designed two-sided sandwich (solve a
 * region boundary-grounded, again boundary-free, certify if they agree) fails three ways:
 * closes only at 95-100% of the structure, costs 1.68x a global solve, and its pessimistic
 * side is not a bound. A closed sandwich certified a leaning stack (lambda* 0.44048) as
 * standing while the certified band's lambda* is 18.481256459924058 — wrong by 41.96x.
 * Cause: dropping material outside a region removed its weight with its restraint, and
 * here the omitted 24 courses carried the destabilising moment.
 *
 * WHAT SURVIVES, unrepaired: the optimistic side. Restricting an admissible force system
 * to a grounded-shell region stays admissible, so optimistic-infeasible => globally
 * infeasible holds. Used unchanged, surcharge kept off it.
 *
 * THE TWO REPAIRS, user-ruled, defined at their code in the support namespace:
 *
 *   (1) The pessimistic side keeps the omitted weight as a dead surcharge while removing
 *       its restraint. "Bears on the region" is graph reachability with the region deleted:
 *       material on its own foundation is not charged, material whose only road to ground
 *       runs through the region is charged in full. The load acts on the vertical through
 *       the charged component's centre of gravity, carrying the overturning moment.
 *   (2) Regions grow ground-anchored by construction. The growth parameter becomes the
 *       width of a full-height strip, since the free side is provably infeasible for any
 *       region containing no grounded block.
 *
 * THE QUESTION: with both repairs, how small can a certified region be? Success needs
 * regions much smaller than the structure and cheap enough for two solves inside ~100 ms
 * (the 84-block solve is 0.0666 s, the target size). If it certifies only at 90%+ or costs
 * more than a global solve, that is the same refutation in a new coat, pointing at the
 * asynchronous / size-scoped fallbacks. This file must say so plainly.
 *
 * ================================================================================
 * PREDICTIONS — derivation record revision 1, before the first run (PROMOTION_DESIGN §7.3;
 * a prediction written after the measurement is a transcription). Each repeated at its pin.
 * ================================================================================
 *
 *  P1  The carried set of a full-height ground-anchored strip in a grounded wall is
 *      empty at every width, because every omitted block's own column reaches the
 *      grounded bottom course. So repair (1) is a no-op on the wall arm and repair (2)
 *      does all the work there; repair (1) earns its keep on the collapse arm alone. If
 *      that holds it says the two repairs are not alternatives, they answer different
 *      fixtures.
 *  P2  The optimistic side is feasible at every strip width (the cut wall stands
 *      globally, and grounding the shell can only help), so on the wall arm closure is
 *      decided entirely by the pessimistic side.
 *  P3  Closes at half-width 0 — a two-cell zig-zag column, 41 of 149 blocks, 28%.
 *      Running bond gives the course above a one-brick hole a bearing on both sides
 *      inside the strip. Fallback if wrong: half-width 1, 65 blocks, 44%.
 *  P4  Ten of ten deletions certify at the closing width, against the refuted form's five.
 *  P5  Cost 0.3-0.6x a global solve. The dead pose runs ~2.2 pivots per equality row at
 *      this scale and a 41-block region has ~35 ungrounded blocks = ~105 rows, so ~230
 *      pivots a side, ~460 for the pair, against the whole wall's 965. Point estimate
 *      0.48x, against the refuted form's 1.68x.
 *  P6  Time: two region solves well inside 100 ms (84 blocks answer in 0.0666 s; 41
 *      blocks should be ~0.02 s), so the verdict at 12 courses is predicted yes.
 *  P7  Height scales the region. An 18-course wall closes at the same half-width with
 *      ~62 blocks — 1.5x the region for 1.5x the height. At the 30-course scenario
 *      height the same strip is ~100 blocks, at or past the 84-block / 0.0666 s figure,
 *      so the honest predicted answer is "small enough at 12 courses, marginal at 30".
 *  P8  The over-inclusive control (charge every omitted block) is infeasible at the
 *      closing width and never certifies — the too-pessimistic failure mode, measured.
 *  P9  Collapse arm: the repaired pessimistic side is infeasible at every band, the
 *      optimistic side is unchanged, so the sandwich closes only with all thirty
 *      blocks. The repair withdraws the false certificate; it does not make collapse
 *      cheaper.
 * P10  No false certificate anywhere in this test: every closed sandwich agrees with
 *      the whole structure's own verdict. This is the assertion the slice would be
 *      worthless without, and it is counted rather than spot-checked.
 *
 * ================================================================================
 * WHAT WAS MEASURED — 2026-08-15, one run, tree at HEAD 0564bf5
 * ================================================================================
 *
 * The wall arm. 12 courses x 12 cells, one brick out of course 6, 149 blocks / 385 joints,
 * whole-wall feasibility 965 pivots (the refuted form's own fixture and numbers):
 *
 *     half-width   blocks   % of wall   charged   optimistic   pessimistic   closes
 *              0       41       27.5%         0     feasible      feasible      YES
 *              1       65       43.6%         0     feasible      feasible      yes
 *              2       89       59.7%         0     feasible      feasible      yes
 *              3      113       75.8%         0     feasible      feasible      yes
 *              4      137       91.9%         0     feasible      feasible      yes
 *              5      149      100.0%         0     feasible      feasible      yes
 *
 * **Closes at the first rung: 41 of 149 blocks, 27.5%**, against the refuted form's 142 /
 * 95%. Its two solves take 49 + 262 = 311 pivots against 965 — **0.322x** (refuted form:
 * 1.68x) — and 0.021 s for the pair against 0.100 s. Ten deletions at courses 1..10 certify
 * **ten of ten** (refuted form: five), each checked against that deletion's 149-block solve.
 *
 * The collapse arm. Ground-anchored bands of the 30-course leaning stack:
 *
 *     band     blocks   charged   surcharge uu   optimistic   pessimistic   closes
 *     0..5          7        23      61,345.57     feasible    INFEASIBLE       no
 *     0..10        12        18      48,009.58     feasible    INFEASIBLE       no
 *     0..15        17        13      34,673.58     feasible    INFEASIBLE       no
 *     0..20        22         8      21,337.59     feasible    INFEASIBLE       no
 *     0..25        27         3       8,001.60     feasible    INFEASIBLE       no
 *     0..29        30         0              0   INFEASIBLE    INFEASIBLE      YES
 *
 * The repair does what it was ruled to and no more: **every false certificate is
 * withdrawn** (bands now disagree, meaning grow the region), and **the collapse arm is no
 * cheaper** — it still certifies only with all thirty blocks, because the optimistic side
 * is what certifies a collapse and the repair does not touch it. Across the test, 20
 * certificates, 0 false (read the caveat block before treating that as evidence).
 *
 * **The collapse arm's cost is 2.0x, not 0.322x — quote both or neither.** At closure the
 * stack's two solves take 163 + 163 = 326 pivots against a 163-pivot global solve, and the
 * whole growth ladder (every band from 0..5 up) is 1,300 pivots, **8x**. The standing arm's
 * 0.322x and this 2.0x are two halves of one measurement, and 2.0x is worse than the 1.68x
 * the refuted form was condemned for. Cheap where the answer is "it stands", dear where it
 * is "it falls" — the arm a player's deletion cares about.
 *
 * HEIGHT SCALES THE REGION, which prices scenario scale. The same wall at 18 courses is 224
 * blocks and closes at the same half-width with 62 blocks — 27.7%, 1.512x the 12-course
 * region for 1.5x the height. Fraction height-invariant, count not: linear off two points
 * (estimate), a 30-course wall's strip is ~102 blocks, larger than the 84-block / 0.0666 s
 * fixture. So a certified region is small enough for synchronous authority at 12 courses,
 * marginal at 30 — and region size, not the sandwich, decides it.
 *
 * TWO PREDICTIONS MISSED, recorded at their pins:
 *
 *   - P5's point estimate (0.48x) against 0.322x. The range held; it priced both sides as
 *     equal-row, but the optimistic side is nearly free (49 pivots for 41 blocks) because
 *     grounding the shell turns most of the boundary into earth, which writes no row.
 *   - P8 outright. Charging every omitted block (all 108, 271,309.925 uu) still leaves the
 *     41-block strip feasible, so "too pessimistic to certify" is untested — the wall arm
 *     has too much margin to discriminate surcharge rules.
 *
 * ================================================================================
 * THE CAVEAT THAT MATTERS MOST, AND IT IS NOT A FOOTNOTE
 * ================================================================================
 *
 * P1 held: the carried set of a full-height strip in a grounded wall is empty at every
 * width. So on the wall arm the repaired pessimistic side is bit-identical to the refuted
 * one (same problem, no applied forces) and every wall certificate rests on repair (2)
 * alone. Repair (1) removes one unsoundness: omitted material whose only road to ground ran
 * through the region — the leaning stack's mode, which a full-height strip cannot produce.
 *
 * "20 CERTIFICATES, 0 FALSE" IS NO EVIDENCE OF SOUNDNESS, mechanically not by sample size.
 * Nineteen are about structures the global solve reads feasible and the twentieth is the
 * whole stack, so not one is about a proper subset of an infeasible structure — the only
 * shape a false certificate takes. CheckCertificate is re-testing the optimistic side
 * (sound by restriction) every row. This is TRAPS' shared-hidden-property trap: the hidden
 * property is that every proper-subset fixture stands. Honest split: 18 proper-subset + 2
 * trivial whole-structure closures (wall at w = 5, 149 of 149; stack at 0..29, 30 of 30).
 *
 * AT LEAST THREE UNSOUNDNESSES REMAIN, none exercised by any fixture here:
 *
 *   1. Omitted material with its own ground path that also bears on the region: the
 *      carried-set rule charges it nothing. No fixture exposes it — every wall the LP
 *      prices is feasible, and the only sub-1.0 fixture is the stack.
 *   2. The interface joint is never checked. SpikeExtractRegion drops joints leaving the
 *      region and SpikeAddSurcharge applies the load directly to the region block, so the
 *      transmitting contact's capacity is never asked. Review worked the stack: joint 5-6
 *      would need T = 636,000 uu against a 412,562 uu cap. The surcharge is a rigid
 *      attachment where reality has a joint with a strength.
 *   3. The charged component's own internal joints vanish (courses 6..29's 23 bed joints
 *      are in no problem), so a component failing internally leaves the region certifying.
 *      And where a component meets the region at several joints, the area-proportional
 *      split is arbitrary where the real structure lets the LP choose.
 *
 * Watch for: a wall region carrying an omitted spandrel whose bed joint sits at 1.01 of
 * capacity — weight charged, region carries it, certificate issued, spandrel drops, nothing
 * notices.
 *
 * (1) is settled by a wall the LP prices below 1.0 whose ground-anchored strip stands; (2)
 * and (3) by a surcharge keeping the interface and internal joints in the problem while
 * writing no equilibrium row — a larger repair. All three are specified follow-ups.
 *
 * ================================================================================
 * GREEN ON ARRIVAL IS NOT THE SAME AS ASSERTING NOTHING — THE MUTATIONS
 * ================================================================================
 *
 * Every row is green the day it lands, since extractor and measurement land together.
 * TRAPS' registry carries the proof; two of three are new:
 *
 *   X8  surcharge applied at the contact's X instead of the charged component's centre of
 *       gravity  ->  4 assertions here + 3 in RegionalSandwich Part D, and it **re-creates
 *       the false certificate**: band 0..5 certifies feasible=1 against an infeasible stack
 *       — the sharpest evidence, saying repair (1)'s load-bearing half is WHERE the weight
 *       acts. The third Part D assertion is the line-of-action pin (review, 2026-08-15):
 *       "175 cm derived ... acted at 55 cm", because the verdict rows alone would let a
 *       wrong-but-nearer X through in silence.
 *   X9  the carried set computed empty (repair (1) removed, plumbing left)  ->  8 here + 5
 *       in Part D, three false certificates.
 *  X10  the Carried rule made to charge every component (selector inverted)  ->  7 here:
 *       five of the six wall Charged pins (0 -> 108, 84, 60, 36, 12 as the strip widens;
 *       w = 5 stays 0) plus both pessimistic pivot pins (262 -> 514, tall arm 474 -> 761) —
 *       the prover the Charged pins lacked. No verdict moves, restating P8's miss.
 *   X6  the extractor keeps out-of-region blocks, re-measured against the repaired
 *       extractor  ->  12 here (five block counts, the closing region size and both its
 *       pivot pins, the cost-ratio row, the under-half row, the tall region, the stack's
 *       closing band) and still 14 in RegionalSandwich.
 *
 * WHAT STILL HAS NO BITE-PROVER, recorded because a green row nothing can fail is
 * indistinguishable from one asserting nothing:
 *
 *   - **Agreements == 10.** Its only failure mode is a deletion that stops certifying, and
 *     no available mutation moves it (X6 changes sizes not verdicts, X9/X10 change a
 *     surcharge where the wall's carried set is empty either way, a rung flip leaves ten).
 *     A ceiling-shaped pin.
 *   - **The three surcharge-integrity guards in PoseSandwich** (no orphan, none onto a
 *     grounded block, applied == charged). Exercised on a live non-zero path (the
 *     EveryOmittedBlock control charges 108 blocks through 44 joints) but no mutation fails
 *     one. Cheapest provers, recorded not built: drop the interface search's "other end in
 *     region" test for an orphan; deliver every share to BlockA for a grounded delivery.
 *
 * COST: **9.4 s**, printed at the run's end, of which the ten global solves are ~a third.
 * Opt-in fast tier (7 tests, ~78 s; `OracleSweepFull` verifies solver changes), one of the
 * two most expensive rows; default suite untouched at 173 = 167 green + 6 deliberate reds.
 *
 * Needs a ticking world: no.
 * ==================================================================================== */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleRepairedSandwichTest,
	"OracleSweepFast.RigidBlock.RepairedRegionalSandwich",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleRepairedSandwichTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace OracleFeasibilitySpikeSupport;

	const FRepairPins Pins;

	const double Started = FPlatformTime::Seconds();

	/*
	 * A refusal is not an infeasibility, and SpikeIsFeasible cannot tell them apart:
	 * Refuse() returns bAnswered = false, lambda = 0, bit-for-bit the dead-load infeasibility
	 * arm. Every region solve is guarded, since an unguarded refusal reads as "no admissible
	 * equilibrium" — the shape of half the conclusions below.
	 */
	const auto MustAnswer = [this](const FPoseReading& Read, const FString& Where)
	{
		return TestTrue(
			*FString::Printf(
				TEXT("%s: the solve must ANSWER — a REFUSAL and an INFEASIBILITY are ")
				TEXT("indistinguishable downstream (both are bAnswered=false, lambda=0). ")
				TEXT("It said: %s"),
				*Where, Read.WhyNot.IsEmpty() ? TEXT("(nothing)") : *Read.WhyNot),
			Read.bAnswered);
	};

	/** One region, both boundary conditions, one verdict. */
	struct FSandwich
	{
		int32 Blocks = 0;
		int32 Joints = 0;
		int32 GroundedFree = 0;
		bool bOptimistic = false;
		bool bPessimistic = false;
		bool bCloses = false;
		int32 OptimisticPivots = 0;
		int32 PessimisticPivots = 0;
		double Seconds = 0.0;
		FSurchargeCounts Surcharge;
	};

	const auto PoseSandwich = [this, &MustAnswer](
		const FOracleProblem& Full,
		const TArray<bool>& Mask,
		ESpikeSurcharge Rule,
		const FString& Where) -> FSandwich
	{
		FSandwich Out;

		FOracleProblem Optimistic;
		FRegionCounts OptimisticCounts;
		FSurchargeCounts OptimisticSurcharge;

		SpikeExtractRepairedRegion(
			Full, Mask, /*bGroundTheShell*/ true, Rule,
			Optimistic, OptimisticCounts, OptimisticSurcharge);

		FOracleProblem Pessimistic;
		FRegionCounts PessimisticCounts;
		FSurchargeCounts PessimisticSurcharge;

		SpikeExtractRepairedRegion(
			Full, Mask, /*bGroundTheShell*/ false, Rule,
			Pessimistic, PessimisticCounts, PessimisticSurcharge);

		/*
		 * The surcharge is the pessimistic side's alone, asserted not trusted: it would
		 * silently unsound the half review verified, and the extractor is one line from it.
		 */
		TestEqual(
			*FString::Printf(
				TEXT("%s: the OPTIMISTIC side must carry no surcharge — its soundness is a ")
				TEXT("restriction argument and a restriction satisfies no row that was not ")
				TEXT("in the original problem"),
				*Where),
			Optimistic.AppliedForces.Num(), 0);

		const FPoseReading OptimisticRead = SpikeSolve(Optimistic);
		const FPoseReading PessimisticRead = SpikeSolve(Pessimistic);

		MustAnswer(OptimisticRead, FString::Printf(TEXT("%s GROUNDED-boundary"), *Where));
		MustAnswer(PessimisticRead, FString::Printf(TEXT("%s FREE-boundary"), *Where));

		Out.Blocks = OptimisticCounts.Blocks;
		Out.Joints = OptimisticCounts.Joints;
		Out.GroundedFree = PessimisticCounts.Grounded;
		Out.bOptimistic = SpikeIsFeasible(OptimisticRead);
		Out.bPessimistic = SpikeIsFeasible(PessimisticRead);
		Out.bCloses = Out.bOptimistic == Out.bPessimistic;
		Out.OptimisticPivots = OptimisticRead.Pivots;
		Out.PessimisticPivots = PessimisticRead.Pivots;
		Out.Seconds = OptimisticRead.Seconds + PessimisticRead.Seconds;
		Out.Surcharge = PessimisticSurcharge;

		/*
		 * Repair (2), made executable. A region with no grounded block is free-side
		 * infeasible whatever else holds (sum its vertical rows: 0 = -W_region), so
		 * "ground-anchored by construction" is what the strip parameterisation guarantees. If
		 * it fails, every certificate below is vacuous.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s: a ground-anchored region must actually contain the earth, and this ")
				TEXT("one holds %d grounded blocks"),
				*Where, Out.GroundedFree),
			Out.GroundedFree > 0);

		/* The surcharge's own integrity, everywhere it is applied. */
		TestEqual(
			*FString::Printf(TEXT("%s: no ORPHAN charged component"), *Where),
			Out.Surcharge.OrphanComponents, 0);

		TestEqual(
			*FString::Printf(
				TEXT("%s: no surcharge delivered onto a GROUNDED block, which writes no ")
				TEXT("equilibrium row and would swallow it"),
				*Where),
			Out.Surcharge.DiscardedOntoGroundedUu, 0.0);

		TestTrue(
			*FString::Printf(
				TEXT("%s: the applied surcharge (%.9g uu) must equal the charged blocks' own ")
				TEXT("weight (%.9g uu)"),
				*Where, Out.Surcharge.AppliedWeightUu, Out.Surcharge.ChargedWeightUu),
			FMath::Abs(Out.Surcharge.AppliedWeightUu - Out.Surcharge.ChargedWeightUu)
				<= 1.0e-9 * (1.0 + Out.Surcharge.ChargedWeightUu));

		return Out;
	};

	/* Every closed sandwich in this test is checked against the truth it certifies. */
	int32 Certificates = 0;
	int32 FalseCertificates = 0;

	const auto CheckCertificate = [this, &Certificates, &FalseCertificates](
		const FSandwich& Sandwich, bool bGlobalFeasible, const FString& Where)
	{
		if (!Sandwich.bCloses)
		{
			return;
		}

		++Certificates;

		if (Sandwich.bOptimistic != bGlobalFeasible)
		{
			++FalseCertificates;
		}

		TestTrue(
			*FString::Printf(
				TEXT("%s: the closed sandwich certifies feasible=%d and the whole structure ")
				TEXT("reads feasible=%d. A disagreement here is a FALSE CERTIFICATE — the ")
				TEXT("failure the repair exists to remove, and the one thing this slice ")
				TEXT("cannot be allowed to reproduce quietly."),
				*Where, Sandwich.bOptimistic ? 1 : 0, bGlobalFeasible ? 1 : 0),
			Sandwich.bOptimistic == bGlobalFeasible);
	};

	/* ================================================================================
	 * PART 1 — THE WALL, AND THE STRIP-WIDTH LADDER.
	 *
	 * The refuted form's fixture: a 12x12 wall, one brick out of course 6, 149 blocks, 965
	 * pivots for whole-wall feasibility. Same wall and deletion, so the two tables are
	 * directly comparable and only the region shape and surcharge differ.
	 * ================================================================================ */

	constexpr int32 WallCourses = 12;
	constexpr int32 WallCells = 12;
	constexpr int32 WallDeleteCourse = 6;

	FStructure Wall;
	FString Why;

	/*
	 * Each producer call is its own statement before its message: folding a call that writes
	 * Why into the Printf that reads it is unsequenced, and MSVC evaluates the condition last
	 * (TRAPS). Every such pair in this test is split the same way.
	 */
	const bool bWallLaid = SpikeBuildIntactWall(WallCourses, WallCells, Wall, Why);

	if (!TestTrue(
			*FString::Printf(TEXT("repaired: the wall must lay (it said: %s)"), *Why),
			bWallLaid))
	{
		return true;
	}

	double DeleteXCm = 0.0;
	double DeleteZCm = 0.0;
	const int32 Victim = SpikePieceInCourse(Wall, WallDeleteCourse, DeleteXCm, DeleteZCm);

	if (!TestTrue(TEXT("repaired: there must be a brick to delete"), Victim != INDEX_NONE)
		|| !TestTrue(TEXT("repaired: the brick must be removable"), Wall.RemovePiece(Victim)))
	{
		return true;
	}

	FOracleProblem Full;

	const bool bBridged = BuildRigidBlockProblem(Wall, Full, Why);

	if (!TestTrue(
			*FString::Printf(TEXT("repaired: the bridge must represent the cut wall (%s)"), *Why),
			bBridged))
	{
		return true;
	}

	TestEqual(TEXT("repaired: the wall is the refuted form's own fixture"),
		Full.Blocks.Num(), Pins.WallBlocks);

	FOracleProblem Global = Full;
	Global.bGravityIsLive = false;

	const FPoseReading GlobalRead = SpikeSolve(Global);
	const bool bGlobalFeasible = SpikeIsFeasible(GlobalRead);

	MustAnswer(GlobalRead, TEXT("repaired: the whole cut wall"));

	TestEqual(
		TEXT("repaired: the whole wall's feasibility pivot count — the number the regional ")
		TEXT("lever has to beat, and the refuted form did not"),
		GlobalRead.Pivots, Pins.WallGlobalPivots);

	int32 ClosingHalfWidth = INDEX_NONE;
	int32 ClosingRegionBlocks = INDEX_NONE;
	int32 ClosingOptimisticPivots = INDEX_NONE;
	int32 ClosingPessimisticPivots = INDEX_NONE;
	double ClosingSeconds = -1.0;

	/*
	 * MEASURED / PREDICTED per rung. The Charged column is P1's test: 0 at every width,
	 * because a full-height strip strands nothing in a grounded wall.
	 * MEASURED 2026-08-15: block count is 24w + 41 up to the wall width (17 core + 24 shell
	 * at w = 0, 24 more core per step), the strip closes at the first rung, Charged reads 0
	 * on all six — P1 held.
	 */
	TArray<FStripPin> StripPins;
	StripPins.Add({ 0,  41, 0, 1, 1 });
	StripPins.Add({ 1,  65, 0, 1, 1 });
	StripPins.Add({ 2,  89, 0, 1, 1 });
	StripPins.Add({ 3, 113, 0, 1, 1 });
	StripPins.Add({ 4, 137, 0, 1, 1 });
	StripPins.Add({ 5, 149, 0, 1, 1 });

	for (const FStripPin& Pin : StripPins)
	{
		TArray<bool> Mask;
		SpikeGroundStripMask(Full, DeleteXCm, Pin.HalfWidthCells, Mask);

		const FSandwich Sandwich = PoseSandwich(
			Full, Mask, ESpikeSurcharge::Carried,
			FString::Printf(TEXT("strip w=%d"), Pin.HalfWidthCells));

		if (Sandwich.bCloses && ClosingHalfWidth == INDEX_NONE)
		{
			ClosingHalfWidth = Pin.HalfWidthCells;
			ClosingRegionBlocks = Sandwich.Blocks;
			ClosingOptimisticPivots = Sandwich.OptimisticPivots;
			ClosingPessimisticPivots = Sandwich.PessimisticPivots;
			ClosingSeconds = Sandwich.Seconds;
		}

		const FString Line = FString::Printf(
			TEXT("REPAIRED STRIP w=%d: blocks=%d (%.1f%% of %d) joints=%d groundedFree=%d | ")
			TEXT("charged=%d components=%d interface=%d chargedW=%.9g | optimistic=%d ")
			TEXT("pivots=%d | pessimistic=%d pivots=%d | closes=%d | secs=%.3f"),
			Pin.HalfWidthCells, Sandwich.Blocks,
			100.0 * double(Sandwich.Blocks) / double(Full.Blocks.Num()), Full.Blocks.Num(),
			Sandwich.Joints, Sandwich.GroundedFree,
			Sandwich.Surcharge.ChargedBlocks, Sandwich.Surcharge.ChargedComponents,
			Sandwich.Surcharge.InterfaceJoints, Sandwich.Surcharge.ChargedWeightUu,
			Sandwich.bOptimistic ? 1 : 0, Sandwich.OptimisticPivots,
			Sandwich.bPessimistic ? 1 : 0, Sandwich.PessimisticPivots,
			Sandwich.bCloses ? 1 : 0, Sandwich.Seconds);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);

		CheckCertificate(Sandwich, bGlobalFeasible,
			FString::Printf(TEXT("strip w=%d"), Pin.HalfWidthCells));

		if (Pin.Blocks == INDEX_NONE || Pin.Charged == INDEX_NONE
			|| Pin.Optimistic == INDEX_NONE || Pin.Pessimistic == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("strip w=%d: UNMEASURED RUNG — pin blocks=%d charged=%d optimistic=%d ")
				TEXT("pessimistic=%d"),
				Pin.HalfWidthCells, Sandwich.Blocks, Sandwich.Surcharge.ChargedBlocks,
				Sandwich.bOptimistic ? 1 : 0, Sandwich.bPessimistic ? 1 : 0));
		}
		else
		{
			TestEqual(*FString::Printf(TEXT("strip w=%d: region block count"),
				Pin.HalfWidthCells), Sandwich.Blocks, Pin.Blocks);

			TestEqual(
				*FString::Printf(
					TEXT("strip w=%d: how many omitted blocks the region CARRIES — P1 ")
					TEXT("predicted zero at every width, because a full-height strip in a ")
					TEXT("grounded wall leaves nothing stranded"),
					Pin.HalfWidthCells),
				Sandwich.Surcharge.ChargedBlocks, Pin.Charged);

			TestEqual(*FString::Printf(TEXT("strip w=%d: GROUNDED-boundary verdict"),
				Pin.HalfWidthCells), Sandwich.bOptimistic ? 1 : 0, Pin.Optimistic);

			TestEqual(*FString::Printf(TEXT("strip w=%d: FREE-boundary verdict"),
				Pin.HalfWidthCells), Sandwich.bPessimistic ? 1 : 0, Pin.Pessimistic);
		}
	}

	if (Pins.ClosingHalfWidth == INDEX_NONE)
	{
		AddError(FString::Printf(
			TEXT("repaired ladder: UNMEASURED CLOSING WIDTH — measured half-width %d at %d ")
			TEXT("of %d blocks (%.1f%%), %d + %d pivots against the whole wall's %d ")
			TEXT("(ratio %.3f), %.4f s for the pair. PREDICTED (P3/P5/P6): half-width 0, 41 ")
			TEXT("blocks, 28%%, ~0.48x, under 100 ms. INDEX_NONE means it did not close at ")
			TEXT("any width in the ladder."),
			ClosingHalfWidth, ClosingRegionBlocks, Full.Blocks.Num(),
			ClosingRegionBlocks > 0
				? 100.0 * double(ClosingRegionBlocks) / double(Full.Blocks.Num()) : -1.0,
			ClosingOptimisticPivots, ClosingPessimisticPivots, GlobalRead.Pivots,
			GlobalRead.Pivots > 0
				? double(ClosingOptimisticPivots + ClosingPessimisticPivots)
					/ double(GlobalRead.Pivots)
				: -1.0,
			ClosingSeconds));
	}
	else
	{
		TestEqual(TEXT("repaired ladder: the closing strip half-width"),
			ClosingHalfWidth, Pins.ClosingHalfWidth);

		TestEqual(
			TEXT("repaired ladder: HOW BIG the certified region is — the number the refuted ")
			TEXT("form answered with 142 of 149, and the whole point of a decomposition"),
			ClosingRegionBlocks, Pins.ClosingRegionBlocks);

		TestEqual(TEXT("repaired ladder: optimistic pivots at the closing width"),
			ClosingOptimisticPivots, Pins.ClosingOptimisticPivots);

		TestEqual(TEXT("repaired ladder: pessimistic pivots at the closing width"),
			ClosingPessimisticPivots, Pins.ClosingPessimisticPivots);

		/*
		 * The cost comparison, in pivots (deterministic; seconds not). The refuted form's two
		 * solves cost 1.68x the global solve; this strict inequality says which side of 1.0
		 * the repaired form lands on. If it crosses back, the lever is worthless, noticed here.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("repaired ladder: THE SANDWICH MUST COST LESS THAN THE GLOBAL SOLVE — ")
				TEXT("its two solves at the closing width take %d + %d = %d pivots against ")
				TEXT("the whole wall's %d (ratio %.3f; the refuted form's was 1.68x)"),
				ClosingOptimisticPivots, ClosingPessimisticPivots,
				ClosingOptimisticPivots + ClosingPessimisticPivots, GlobalRead.Pivots,
				double(ClosingOptimisticPivots + ClosingPessimisticPivots)
					/ double(GlobalRead.Pivots)),
			ClosingOptimisticPivots + ClosingPessimisticPivots < GlobalRead.Pivots);

		/*
		 * The region must be a neighbourhood, not the building. Half the structure is the
		 * loosest reading of "much smaller" still worth calling a decomposition; the refuted
		 * form's 95% failed it by a mile.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("repaired ladder: the certified region must be under half the wall — %d ")
				TEXT("of %d blocks, %.1f%%"),
				ClosingRegionBlocks, Full.Blocks.Num(),
				100.0 * double(ClosingRegionBlocks) / double(Full.Blocks.Num())),
			2 * ClosingRegionBlocks < Full.Blocks.Num());
	}

	/* ================================================================================
	 * PART 2 — TEN DELETIONS AT THE CLOSING WIDTH.
	 *
	 * The refuted form certified five of ten — those whose ball reached the foundation. A
	 * ground-anchored strip reaches it by construction, so the deletion's height stops
	 * mattering (P4: ten of ten). A fixed ladder of ten bricks, the same ten every run.
	 * ================================================================================ */

	int32 Agreements = 0;

	if (ClosingHalfWidth == INDEX_NONE)
	{
		AddError(TEXT("repaired agreement: skipped — the ladder never closed, so there is no ")
			TEXT("width to run ten deletions at"));
	}
	else
	{
		for (int32 Course = 1; Course <= 10; ++Course)
		{
			FStructure Cut;
			FString CutWhy;

			const bool bCutLaid = SpikeBuildIntactWall(WallCourses, WallCells, Cut, CutWhy);

			if (!TestTrue(
					*FString::Printf(TEXT("repaired course %d: the wall must lay (%s)"),
						Course, *CutWhy),
					bCutLaid))
			{
				continue;
			}

			double XCm = 0.0;
			double ZCm = 0.0;
			const int32 Brick = SpikePieceInCourse(Cut, Course, XCm, ZCm);
			const bool bRemoved = Brick != INDEX_NONE && Cut.RemovePiece(Brick);

			if (!TestTrue(
					*FString::Printf(TEXT("repaired course %d: a brick must exist there"), Course),
					bRemoved))
			{
				continue;
			}

			FOracleProblem CutProblem;

			const bool bCutBridged = BuildRigidBlockProblem(Cut, CutProblem, CutWhy);

			if (!TestTrue(
					*FString::Printf(TEXT("repaired course %d: the bridge must represent it (%s)"),
						Course, *CutWhy),
					bCutBridged))
			{
				continue;
			}

			FOracleProblem CutGlobal = CutProblem;
			CutGlobal.bGravityIsLive = false;

			const FPoseReading CutGlobalRead = SpikeSolve(CutGlobal);
			const bool bCutFeasible = SpikeIsFeasible(CutGlobalRead);

			MustAnswer(CutGlobalRead,
				FString::Printf(TEXT("repaired course %d: the whole cut wall"), Course));

			TArray<bool> Mask;
			SpikeGroundStripMask(CutProblem, XCm, ClosingHalfWidth, Mask);

			const FSandwich Sandwich = PoseSandwich(
				CutProblem, Mask, ESpikeSurcharge::Carried,
				FString::Printf(TEXT("repaired course %d"), Course));

			if (Sandwich.bCloses)
			{
				++Agreements;
			}

			const FString Line = FString::Printf(
				TEXT("REPAIRED AGREEMENT course=%d (brick at X=%.4f Z=%.4f): blocks=%d ")
				TEXT("charged=%d | optimistic=%d pessimistic=%d closes=%d | global=%d | ")
				TEXT("pivots %d + %d against %d | secs=%.3f"),
				Course, XCm, ZCm, Sandwich.Blocks, Sandwich.Surcharge.ChargedBlocks,
				Sandwich.bOptimistic ? 1 : 0, Sandwich.bPessimistic ? 1 : 0,
				Sandwich.bCloses ? 1 : 0, bCutFeasible ? 1 : 0,
				Sandwich.OptimisticPivots, Sandwich.PessimisticPivots, CutGlobalRead.Pivots,
				Sandwich.Seconds);

			UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
			AddInfo(Line);

			CheckCertificate(Sandwich, bCutFeasible,
				FString::Printf(TEXT("repaired course %d"), Course));
		}
	}

	if (Pins.Agreements == INDEX_NONE)
	{
		AddError(FString::Printf(
			TEXT("repaired agreement: UNMEASURED — at the closing width, %d of ten deletions ")
			TEXT("certified. PREDICTED (P4) ten; the refuted form managed five at radius 4 ")
			TEXT("with a region of 114 blocks."),
			Agreements));
	}
	else
	{
		TestEqual(
			TEXT("repaired agreement: how many of ten deletions certify at the closing width ")
			TEXT("— distinct from the file-wide count of closed sandwiches, which is 20"),
			Agreements, Pins.Agreements);
	}

	/* ================================================================================
	 * PART 3 — THE OVER-INCLUSIVE CONTROL.
	 *
	 * "Keep the omitted weight" has an obvious reading — keep all of it — and that is the
	 * second way to kill the lever: a too-pessimistic side never certifies and looks like a
	 * decomposition that does not decompose. Measured at the same width and wall, so the only
	 * difference from the certified row is the surcharge rule.
	 * ================================================================================ */

	if (ClosingHalfWidth != INDEX_NONE)
	{
		TArray<bool> Mask;
		SpikeGroundStripMask(Full, DeleteXCm, ClosingHalfWidth, Mask);

		const FSandwich Control = PoseSandwich(
			Full, Mask, ESpikeSurcharge::EveryOmittedBlock, TEXT("control (charge everything)"));

		const FString Line = FString::Printf(
			TEXT("REPAIRED CONTROL w=%d, EVERY omitted block charged: blocks=%d charged=%d ")
			TEXT("components=%d interface=%d chargedW=%.9g | optimistic=%d pessimistic=%d ")
			TEXT("closes=%d"),
			ClosingHalfWidth, Control.Blocks, Control.Surcharge.ChargedBlocks,
			Control.Surcharge.ChargedComponents, Control.Surcharge.InterfaceJoints,
			Control.Surcharge.ChargedWeightUu, Control.bOptimistic ? 1 : 0,
			Control.bPessimistic ? 1 : 0, Control.bCloses ? 1 : 0);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);

		if (Pins.ControlPessimistic == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("repaired control: UNMEASURED — charging every omitted block leaves the ")
				TEXT("FREE-boundary side reading feasible=%d (charged %d blocks, %.9g uu). ")
				TEXT("PREDICTED (P8) 0 — infeasible, so the over-inclusive rule never ")
				TEXT("certifies."),
				Control.bPessimistic ? 1 : 0, Control.Surcharge.ChargedBlocks,
				Control.Surcharge.ChargedWeightUu));
		}
		else
		{
			TestEqual(
				TEXT("repaired control: the FREE-boundary verdict when EVERY omitted block is ")
				TEXT("charged — the too-pessimistic failure mode, measured on the same wall ")
				TEXT("and the same width as the certificate above"),
				Control.bPessimistic ? 1 : 0, Pins.ControlPessimistic);
		}
	}

	/* ================================================================================
	 * PART 4 — HEIGHT SCALES THE REGION, WHICH DECIDES SCENARIO SCALE.
	 *
	 * A ground-anchored region reaches the foundation, so its size is set by height, not by a
	 * bonded distance around the deletion. Scenario walls are 30 courses; this measures the
	 * slope with the same wall at 18.
	 * ================================================================================ */

	{
		constexpr int32 TallCourses = 18;

		FStructure Tall;
		FString TallWhy;

		const bool bTallLaid = SpikeBuildIntactWall(TallCourses, WallCells, Tall, TallWhy);

		if (TestTrue(
				*FString::Printf(TEXT("repaired tall: the wall must lay (%s)"), *TallWhy),
				bTallLaid))
		{
			double TallXCm = 0.0;
			double TallZCm = 0.0;
			const int32 TallVictim = SpikePieceInCourse(Tall, WallDeleteCourse, TallXCm, TallZCm);
			const bool bTallRemoved = TallVictim != INDEX_NONE && Tall.RemovePiece(TallVictim);

			FOracleProblem TallProblem;

			const bool bTallBridged = bTallRemoved
				&& BuildRigidBlockProblem(Tall, TallProblem, TallWhy);

			if (TestTrue(TEXT("repaired tall: the brick must be removable"), bTallRemoved)
				&& TestTrue(
					*FString::Printf(TEXT("repaired tall: the bridge must represent it (%s)"),
						*TallWhy),
					bTallBridged))
			{
				FOracleProblem TallGlobal = TallProblem;
				TallGlobal.bGravityIsLive = false;

				const FPoseReading TallGlobalRead = SpikeSolve(TallGlobal);
				const bool bTallFeasible = SpikeIsFeasible(TallGlobalRead);

				MustAnswer(TallGlobalRead, TEXT("repaired tall: the whole cut wall"));

				int32 TallClosing = INDEX_NONE;
				int32 TallClosingBlocks = INDEX_NONE;
				int32 TallClosingOptimisticPivots = INDEX_NONE;
				int32 TallClosingPessimisticPivots = INDEX_NONE;

				for (int32 HalfWidth = 0; HalfWidth <= 2; ++HalfWidth)
				{
					TArray<bool> Mask;
					SpikeGroundStripMask(TallProblem, TallXCm, HalfWidth, Mask);

					const FSandwich Sandwich = PoseSandwich(
						TallProblem, Mask, ESpikeSurcharge::Carried,
						FString::Printf(TEXT("tall strip w=%d"), HalfWidth));

					if (Sandwich.bCloses && TallClosing == INDEX_NONE)
					{
						TallClosing = HalfWidth;
						TallClosingBlocks = Sandwich.Blocks;
						TallClosingOptimisticPivots = Sandwich.OptimisticPivots;
						TallClosingPessimisticPivots = Sandwich.PessimisticPivots;
					}

					const FString Line = FString::Printf(
						TEXT("REPAIRED TALL (%d courses) w=%d: blocks=%d (%.1f%% of %d) ")
						TEXT("charged=%d | optimistic=%d pessimistic=%d closes=%d | pivots ")
						TEXT("%d + %d against %d | secs=%.3f"),
						TallCourses, HalfWidth, Sandwich.Blocks,
						100.0 * double(Sandwich.Blocks) / double(TallProblem.Blocks.Num()),
						TallProblem.Blocks.Num(), Sandwich.Surcharge.ChargedBlocks,
						Sandwich.bOptimistic ? 1 : 0, Sandwich.bPessimistic ? 1 : 0,
						Sandwich.bCloses ? 1 : 0, Sandwich.OptimisticPivots,
						Sandwich.PessimisticPivots, TallGlobalRead.Pivots, Sandwich.Seconds);

					UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
					AddInfo(Line);

					CheckCertificate(Sandwich, bTallFeasible,
						FString::Printf(TEXT("tall strip w=%d"), HalfWidth));
				}

				if (Pins.TallWallBlocks == INDEX_NONE
					|| Pins.TallClosingHalfWidth == INDEX_NONE
					|| Pins.TallClosingRegionBlocks == INDEX_NONE)
				{
					AddError(FString::Printf(
						TEXT("repaired tall: UNMEASURED — an %d-course wall is %d blocks and ")
						TEXT("closes at half-width %d with a region of %d blocks (%.1f%%), ")
						TEXT("costing %d + %d pivots against the whole wall's %d. PREDICTED ")
						TEXT("(P7) half-width 0 and ~62 blocks, 1.5x the 12-course region for ")
						TEXT("1.5x the height."),
						TallCourses, TallProblem.Blocks.Num(), TallClosing, TallClosingBlocks,
						TallClosingBlocks > 0
							? 100.0 * double(TallClosingBlocks) / double(TallProblem.Blocks.Num())
							: -1.0,
						TallClosingOptimisticPivots, TallClosingPessimisticPivots,
						TallGlobalRead.Pivots));
				}
				else
				{
					TestEqual(TEXT("repaired tall: the taller wall's block count"),
						TallProblem.Blocks.Num(), Pins.TallWallBlocks);

					TestEqual(TEXT("repaired tall: its closing half-width"),
						TallClosing, Pins.TallClosingHalfWidth);

					TestEqual(
						TEXT("repaired tall: and the certified region's size, which is the ")
						TEXT("measurement that says whether this lever survives a 30-course ")
						TEXT("scenario wall"),
						TallClosingBlocks, Pins.TallClosingRegionBlocks);

					/*
					 * The cost at height, pinned — the second of the two points the
					 * scenario-scale extrapolation runs through. 79 + 474 = 553 here against
					 * 49 + 262 = 311 at twelve courses is 1.78x cost for 1.5x height, the slope
					 * behind "does a certified region stay in budget at thirty courses".
					 */
					TestEqual(
						TEXT("repaired tall: optimistic pivots at the closing width"),
						TallClosingOptimisticPivots, Pins.TallClosingOptimisticPivots);

					TestEqual(
						TEXT("repaired tall: pessimistic pivots at the closing width — with ")
						TEXT("the line above, the INPUT to the scenario-scale extrapolation"),
						TallClosingPessimisticPivots, Pins.TallClosingPessimisticPivots);
				}
			}
		}
	}

	/* ================================================================================
	 * PART 5 — THE COLLAPSE ARM.
	 *
	 * Every wall is LP-feasible, so Parts 1-4 exercise only the standing side. The 30-course
	 * stack is the one cheap sub-1.0 fixture (lambda* 0.44048) that exposed the false
	 * certificate. Here the regions are ground-anchored bands (courses 0..k), repair (2)'s
	 * shape for a chain, each posed with repair (1) live.
	 * ================================================================================ */

	{
		FStructure Stack;
		FString StackWhy;

		const bool bStackLaid = SpikeBuildLeaningStack(30, Stack, StackWhy);

		if (TestTrue(
				*FString::Printf(TEXT("repaired stack: it must lay (%s)"), *StackWhy),
				bStackLaid))
		{
			FOracleProblem StackProblem;

			const bool bStackBridged = BuildRigidBlockProblem(Stack, StackProblem, StackWhy);

			if (TestTrue(
					*FString::Printf(TEXT("repaired stack: the bridge must represent it (%s)"),
						*StackWhy),
					bStackBridged))
			{
				FOracleProblem StackGlobal = StackProblem;
				StackGlobal.bGravityIsLive = false;

				const FPoseReading StackGlobalRead = SpikeSolve(StackGlobal);
				const bool bStackFeasible = SpikeIsFeasible(StackGlobalRead);

				MustAnswer(StackGlobalRead, TEXT("repaired stack: the whole stack"));

				TestTrue(
					TEXT("repaired stack: the whole 30-course stack must be INFEASIBLE — it is ")
					TEXT("the collapse arm and without it every certificate below is about a ")
					TEXT("structure that stands"),
					!bStackFeasible);

				int32 StackClosingCourse = INDEX_NONE;
				int32 StackClosingBlocks = INDEX_NONE;
				int32 StackClosingOptimisticPivots = INDEX_NONE;
				int32 StackClosingPessimisticPivots = INDEX_NONE;

				/*
				 * Every pivot the growth ladder spends before it certifies. A decomposition
				 * that must grow is charged for the whole walk, not only the rung it stops on,
				 * and on this arm the walk is the entire cost.
				 */
				int32 StackLadderPivots = 0;

				const int32 TopCourses[] = { 5, 10, 15, 20, 25, 29 };

				for (const int32 TopCourse : TopCourses)
				{
					const double TopZCm =
						SpikeBrickHeightCm / 2.0 + double(TopCourse) * SpikeCoursePitchCm;

					TArray<bool> Mask;
					SpikeGroundBandMask(StackProblem, TopZCm, Mask);

					const FSandwich Sandwich = PoseSandwich(
						StackProblem, Mask, ESpikeSurcharge::Carried,
						FString::Printf(TEXT("stack band 0..%d"), TopCourse));

					StackLadderPivots +=
						Sandwich.OptimisticPivots + Sandwich.PessimisticPivots;

					if (Sandwich.bCloses && StackClosingCourse == INDEX_NONE)
					{
						StackClosingCourse = TopCourse;
						StackClosingBlocks = Sandwich.Blocks;
						StackClosingOptimisticPivots = Sandwich.OptimisticPivots;
						StackClosingPessimisticPivots = Sandwich.PessimisticPivots;
					}

					const FString Line = FString::Printf(
						TEXT("REPAIRED STACK BAND 0..%d: blocks=%d (%.1f%% of %d) charged=%d ")
						TEXT("components=%d interface=%d chargedW=%.9g | optimistic=%d ")
						TEXT("pessimistic=%d closes=%d | pivots %d + %d against %d"),
						TopCourse, Sandwich.Blocks,
						100.0 * double(Sandwich.Blocks) / double(StackProblem.Blocks.Num()),
						StackProblem.Blocks.Num(), Sandwich.Surcharge.ChargedBlocks,
						Sandwich.Surcharge.ChargedComponents, Sandwich.Surcharge.InterfaceJoints,
						Sandwich.Surcharge.ChargedWeightUu, Sandwich.bOptimistic ? 1 : 0,
						Sandwich.bPessimistic ? 1 : 0, Sandwich.bCloses ? 1 : 0,
						Sandwich.OptimisticPivots, Sandwich.PessimisticPivots,
						StackGlobalRead.Pivots);

					UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
					AddInfo(Line);

					CheckCertificate(Sandwich, bStackFeasible,
						FString::Printf(TEXT("stack band 0..%d"), TopCourse));

					/*
					 * The surcharge's own set, pinned where it does the work. A reachability
					 * walk charging the wrong blocks would still leave this band infeasible
					 * with every verdict green, so the count is asserted, not inferred.
					 */
					if (TopCourse == 5)
					{
						TestEqual(
							TEXT("repaired stack: the band 0..5 is seven blocks and CARRIES the ")
							TEXT("twenty-three courses above it — the weight the refuted form ")
							TEXT("deleted along with their restraint"),
							Sandwich.Surcharge.ChargedBlocks, Pins.StackBandChargedAtFive);

						TestEqual(
							TEXT("repaired stack: and they are one component meeting the band at ")
							TEXT("exactly one joint, which is what a chain must give"),
							Sandwich.Surcharge.InterfaceJoints, 1);
					}
				}

				if (Pins.StackClosingTopCourse == INDEX_NONE
					|| Pins.StackClosingRegionBlocks == INDEX_NONE)
				{
					AddError(FString::Printf(
						TEXT("repaired stack: UNMEASURED — the smallest ground-anchored band ")
						TEXT("whose two sides AGREE is courses 0..%d, a region of %d of %d ")
						TEXT("blocks, costing %d + %d pivots against the whole stack's %d and ")
						TEXT("%d over the whole growth ladder. PREDICTED (P9) only the whole ")
						TEXT("structure: the repair withdraws the false certificate without ")
						TEXT("making collapse cheaper."),
						StackClosingCourse, StackClosingBlocks, StackProblem.Blocks.Num(),
						StackClosingOptimisticPivots, StackClosingPessimisticPivots,
						StackGlobalRead.Pivots, StackLadderPivots));
				}
				else
				{
					TestEqual(TEXT("repaired stack: the closing band's top course"),
						StackClosingCourse, Pins.StackClosingTopCourse);

					TestEqual(TEXT("repaired stack: and the region there"),
						StackClosingBlocks, Pins.StackClosingRegionBlocks);

					/* ---- AND WHAT THE COLLAPSE ARM COSTS, WHICH IS THE OTHER HALF. ---- */

					TestEqual(TEXT("repaired stack: optimistic pivots at closure"),
						StackClosingOptimisticPivots, Pins.StackClosingOptimisticPivots);

					TestEqual(TEXT("repaired stack: pessimistic pivots at closure"),
						StackClosingPessimisticPivots, Pins.StackClosingPessimisticPivots);

					TestEqual(
						TEXT("repaired stack: and every pivot the growth ladder spends before ")
						TEXT("it gets there — a decomposition that must GROW is charged for the ")
						TEXT("whole walk, not only the rung it stops on"),
						StackLadderPivots, Pins.StackLadderPivots);

					/*
					 * The ratio, as the inequality it is. The wall arm asserts the sandwich
					 * costs less than the global solve; the collapse arm goes the other way,
					 * and both are asserted so neither ratio is quoted without the other.
					 */
					TestTrue(
						*FString::Printf(
							TEXT("repaired stack: THE COLLAPSE ARM COSTS MORE THAN THE GLOBAL ")
							TEXT("SOLVE — %d + %d = %d pivots at closure against %d (%.2fx), ")
							TEXT("and %d over the whole ladder (%.2fx). The refuted form was ")
							TEXT("condemned at 1.68x. If this ever drops below 1.0 the lever ")
							TEXT("has become worth something on the arm that matters, and this ")
							TEXT("is where that is noticed."),
							StackClosingOptimisticPivots, StackClosingPessimisticPivots,
							StackClosingOptimisticPivots + StackClosingPessimisticPivots,
							StackGlobalRead.Pivots,
							double(StackClosingOptimisticPivots + StackClosingPessimisticPivots)
								/ double(StackGlobalRead.Pivots),
							StackLadderPivots,
							double(StackLadderPivots) / double(StackGlobalRead.Pivots)),
						StackClosingOptimisticPivots + StackClosingPessimisticPivots
							> StackGlobalRead.Pivots);
				}
			}
		}
	}

	/* ================================================================================
	 * THE ONE ASSERTION THIS SLICE WOULD BE WORTHLESS WITHOUT.
	 *
	 * The refuted form failed by certifying a structure with no equilibrium, not by
	 * certifying too rarely. Every closed sandwich is checked against the whole structure's
	 * verdict as produced, and the count is asserted here so "no false certificate" is one
	 * number, not a scattering of rows an early return could skip.
	 * ================================================================================ */
	{
		const FString Line = FString::Printf(
			TEXT("REPAIRED TOTALS: %d certificates issued, %d of them FALSE. Test took %.3f s."),
			Certificates, FalseCertificates, FPlatformTime::Seconds() - Started);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);

		TestEqual(
			TEXT("REPAIRED: not one closed sandwich in this test may disagree with the whole ")
			TEXT("structure it certifies. The refuted form produced one that was wrong by ")
			TEXT("41.96x; this is the count that says whether the repair removed it."),
			FalseCertificates, 0);

		TestTrue(
			*FString::Printf(
				TEXT("REPAIRED: and at least one certificate must actually be issued (%d), or ")
				TEXT("'no false certificate' is true of a lever that certifies nothing"),
				Certificates),
			Certificates > 0);
	}

	return true;
}

/* ====================================================================================
 * TEST 4 — THE SUB-1.0 WALL: IS THE REPAIRED PESSIMISTIC SIDE A BOUND, OR A HEURISTIC?
 *
 * The gating follow-up of PROMOTION_DESIGN §5.3's box and §11 R2, and the only fixture
 * shape that can answer it. The repaired sandwich's "20 certificates, 0 false" cannot fire
 * in the direction that matters: nineteen are about already-feasible structures and the
 * twentieth is the whole structure, so none is about a proper subset of an infeasible one.
 * The pessimistic side has had one counterexample removed (X8's centre-of-gravity finding)
 * and was never shown to be a bound.
 *
 * THE FIXTURE (and three routes not taken) is documented at SpikeBuildChimneyWall: the
 * 12x12 wall RepairedRegionalSandwich measures, with a 30-course leaning chimney mortared
 * onto the rightmost full brick of the top course, leaning 10 cm per course past the wall's
 * end. The chimney is the leaning stack's chain, so the structure has no equilibrium; the
 * failure lives at the far end, so a strip through a mid-wall deletion can certify while
 * the structure falls.
 *
 * ================================================================================
 * PREDICTIONS — derivation record revision 1, before the first run (PROMOTION_DESIGN §7.3).
 * Each repeated at its pin.
 * ================================================================================
 *
 *  Q1  lambda* of the composite is the bare stack's, [0.440484, 0.440502]. The binding
 *      joint is inside the chain, and the wall below changes none of the loads above it.
 *      Named fallback if that misses: the chimney's root joint governs instead,
 *      hand-priced at ~1.5 — the thirty courses weigh 30 x 2.72163125 x 981 = 80,100 uu
 *      on a vertical line 145 cm outboard of a joint whose half-length is 10.75, so
 *      equilibrium needs T = (13.49 - 1)/2 x W = 500,000 uu of tension against
 *      0.70 MPa x 110 cm2 = 770,000 uu of bond. Either way lambda* < 1 and the fixture
 *      does its job; only the number moves, and which joint governs is the finding.
 *  Q2  The composite is infeasible at lambda = 1 and the same wall without the chimney
 *      is feasible. Both are measured here, in the same run, through the same builder,
 *      so the chimney is demonstrably the whole of the difference rather than assumed.
 *  Q3  Sizes: plain 149 blocks / 385 joints (RepairedRegionalSandwich's own fixture, and
 *      pinned there); composite 179 / 415 — thirty blocks and thirty joints more (the
 *      root bed joint plus twenty-nine chain beds).
 *  Q4  The strip at w = 0 charges nothing. Delete the strip from the joint graph and
 *      what is left is one component containing the wall's grounded bottom course,
 *      with the chimney hanging off it — so the Carried rule, which charges only
 *      material with no ground path of its own, charges zero blocks. Repair (1) is a
 *      no-op here, exactly as P1 measured on the chimney-free wall.
 *  Q5  The identity, the mechanism stated as an equation: the w = 0 region problem is
 *      the same problem whether or not the chimney is attached — same block count,
 *      same joint count, same verdicts, same pivot counts on both sides. The extractor
 *      drops every chimney block and joint and the surcharge adds nothing, so the
 *      region cannot see the chimney at all. Predicted 49 optimistic / 262 pessimistic,
 *      RepairedRegionalSandwich's own w = 0 rung.
 *  Q6  So the sandwich closes at w = 0 — both sides feasible — issuing a certificate
 *      about 41 of 179 blocks (23%) of a structure with no equilibrium. **Predicted:
 *      the certificate is false, and the repaired pessimistic side is not a bound.**
 *  Q7  The ladder. A half-width w reaches 12.25 + 22.5w cm; the deletion sits at
 *      X = 135.0 and the chimney root at X = 236.25, 101.25 cm away, so w = 4 is the
 *      first width whose strip contains the root. Predicted: false certificates at
 *      w = 0, 1, 2, 3 — four of them, the last covering 113 of 179 blocks — and no
 *      closure at w = 4 or 5, where the chimney above the strip becomes a component
 *      with no ground path, is charged, and sinks the pessimistic side.
 *  Q8  How false, as a number. The certified strip's own lambda*: 41 blocks, twelve
 *      courses, two cells wide, predicted 400-1,100 (the 8-course 84-block wall reads
 *      1128.4 and lambda* falls with height — wall-01's thirty courses read 272.2).
 *      Against the composite's 0.4405 that is a ratio of ~1,000-2,500x, an order of
 *      magnitude past Part D's 41.96x.
 *  Q9  The contrast strip, cut at the chimney's root instead of at the deletion: the
 *      chimney above it becomes a component with no ground path, is charged
 *      (~27 blocks), and the pessimistic side reads infeasible while the optimistic
 *      side stays feasible — so it does not close and issues no certificate. The
 *      repair works where it can see the failing material and is blind where that
 *      material has a ground path of its own, unsoundness (1) of §5.3's box
 *      demonstrated rather than argued.
 * Q10  Cost: under 10 s, fast tier.
 *
 * ================================================================================
 * WHAT IT MEANS IF Q6 MISSES, because a prediction with no other arm is a wish
 * ================================================================================
 *
 * If the pessimistic side finds this strip infeasible, that is the first genuine evidence
 * the repaired side works, on the only fixture shape that could catch it lying — and one
 * fixture is not a proof. Either outcome is pinned as measured; nothing is tuned to produce
 * the other.
 *
 * ================================================================================
 * WHAT WAS MEASURED — 2026-08-16, one run, tree at HEAD 74bb081
 * ================================================================================
 *
 * **The certificate is false. The repaired pessimistic side is not a bound.**
 *
 *     composite   179 blocks / 415 joints, lambda* = 0.44049301907204619, INFEASIBLE
 *     plain wall  149 blocks / 385 joints, FEASIBLE — so the chimney is the whole difference
 *
 *     w   blocks   % of 179   charged   optimistic   pessimistic   closes   FALSE
 *     0       41      22.9%         0     feasible      feasible      YES     YES
 *     1       65      36.3%         0     feasible      feasible      yes     yes
 *     2       89      49.7%         0     feasible      feasible      yes     yes
 *     3      113      63.1%        30     feasible      feasible      yes     yes
 *     4      139      77.7%        28     feasible    INFEASIBLE       no       —
 *     5      153      85.5%        26     feasible    INFEASIBLE       no       —
 *
 * Four false certificates, the smallest about 41 of 179 blocks — 22.9% of a structure with
 * no equilibrium. Q1-Q9 all hit, with one miss below more useful than any hit.
 *
 *   - Q1 hit: lambda* = 0.44049301907204619, inside the bare stack's [0.440484, 0.440502].
 *     The chain governs and the wall under it changes nothing, so the named fallback (root
 *     joint at ~1.5) did not fire — and that hand price matters anyway; see the Q7 miss.
 *   - Q2 hit: composite infeasible, the same wall without the chimney feasible.
 *   - Q3 hit block for block: 179/415 and 149/385.
 *   - Q4/Q5 hit, Q5 the finding as an equation: the w = 0 region is 41 blocks / 66 joints /
 *     0 charged and takes **49 + 262 pivots** with a collapsing chimney attached — the same
 *     numbers and pivots as the chimney-free wall. The region cannot see the chimney.
 *   - Q6 hit: the sandwich closes and the certificate is false.
 *   - Q8 hit: the certified strip's lambda* is **621.95652149729517** against 0.44049 —
 *     **1,412x**, where Part D's refuted-form certificate was wrong by 41.96x.
 *   - Q9 hit to the block: the contrast strip at the chimney's root charges 27, reads
 *     optimistic feasible / pessimistic infeasible, issues no certificate.
 *
 * **THE MISS, AND THE MOST USEFUL THING IN THE RUN. Q7 predicted `charged = 0` at every
 * closing width — all four false certificates from the carried-set rule being blind. At
 * w = 3 the rule is not blind: it charges all thirty chimney blocks (80,100 uu on the
 * vertical through their centre of gravity) and the strip still certifies.** Why, and it is
 * hole (3) of §5.3's box, not hole (1):
 *
 *   - at w = 0..2 the chimney's root sits outside the region, so the omitted component
 *     reaches ground down the wall's columns and is charged nothing. Hole (1): material
 *     with its own ground path that also bears on the region.
 *   - at w = 3 the root is pulled in as a shell block, so the chimney above becomes a
 *     component with no ground path and is charged in full. **The load is applied directly
 *     to the root brick, and the interface joint (the root's bed joint under the chimney)
 *     is dropped** (SpikeExtractRegion drops joints leaving the region). Hole (1) again:
 *     the joint that must transmit the surcharge is never checked.
 *
 *     It makes no difference to the verdict — hand statics say the joint stood anyway, so
 *     the false certificate rests on hole (3) alone. The model's two-contact form: the
 *     joint has two contacts at +/- h = 10.75 cm, tributary area A/2 = 110.1875 cm2 each.
 *     Thirty courses weigh W = 30 x 2.72163125 kg x 980 = 80,015.96 uu on a line e = 145 cm
 *     outboard, so the far contact carries
 *
 *         T = W x (e/h - 1) / 2 = 80,015.96 x (13.4884 - 1)/2 = 499,634.5 uu of TENSION
 *
 *     against f_t x A/2 = 0.70 MPa x 110.1875 cm2 x 1e4 = 771,312.5 uu — **a ratio of
 *     1.5438, pinned below.** The near contact carries 579,650 uu compression (0.053 of the
 *     10 MPa cap) and the vertical surcharge sees no shear. Tension governs, at 0.648.
 *     (The `f_t x A x h` capacity quoted until 2026-08-18 is ~2x this two-contact value and
 *     was right by luck; the number feeds a decision, so it is derived as the LP rows it.)
 *
 *     **The chain's 29 internal bed joints — the ones that fail, at lambda* 0.44 — are in
 *     no problem at all.** Hole (3): the charged component's internal joints vanish, so a
 *     component failing internally leaves the region certifying.
 *   - at w = 4 the region swallows two chimney courses, posing one failing chain joint, and
 *     the pessimistic side goes infeasible. **The region sees the failure only when it
 *     contains the failing joint**, not the failing material's weight.
 *
 * So the fixture exercises two of the three named unsoundnesses, and the second is sharper:
 * a surcharge carries a force and a moment, no strength. §5.3's hypothetical "spandrel
 * whose bed joint sits at 1.01 of capacity" is measured here.
 *
 * TWO SMALLER MISSES:
 *
 *   - Q1's cost reasoning. A small lambda* was expected to make the live pose cheap (the
 *     bare stack's is 29 pivots); the composite's is **2,321 pivots / 4.2 s**, over half
 *     the runtime. A short lambda climb is not a short pivot climb.
 *   - Q7's widths were right, its mechanism wrong — TRAPS' shape exactly: the count (four)
 *     and widths (0-3) came out predicted for a reason the prediction did not contain.
 *
 * WHAT THIS SETTLES: §11 R2's open half. **A closed sandwich is a heuristic, not a
 * certificate, and §5.6's fail-closed fallback is mandatory on every region solve.** It
 * does not settle how to repair it — the w = 3 rung shows charging the weight correctly is
 * not enough; the region would have to carry the charged component's internal joints and
 * the interface joint too (§5.3's "different and larger repair").
 *
 * THE LIMITATION: this structure is infeasible before the deletion, so the counterexample
 * is not deletion-caused. The refuted claim is unconditional, so that does not weaken it,
 * but a deletion-caused non-local failure (the wide-opening cover) would be stronger and is
 * a specified follow-up in CURRENT_STATE.
 *
 * COST: **8.0-10.3 s**, printed at the run's end, of which the composite's live solve is
 * ~4.2 s and the six-rung ladder ~2.4 s. A range on purpose: 8.0 s serial, 10.285 s the
 * same test in a parallel bucket the same day; a single reading would be a number the next
 * person disbelieves. Opt-in fast tier, ~80 s to ~90 s. Default suite untouched.
 *
 * GREEN ON ARRIVAL, three bite-provers (§9.5: a row settling a published design question
 * may not rest on an argued bite):
 *
 *   X11  the chimney built with no lean (`SpikeChimneyLeanCm` 10.0 -> 0.0), so the
 *        structure stands  ->  **14 assertions**: composite-infeasible, its lambda* window
 *        (0.44049 -> 275.414), global pivot pin, the w = 4/5 block/charged/free pins, the
 *        false-certificate count (4 -> 0), the charged-but-certifying row, all three
 *        contrast pins. **The w = 0..3 rungs and the identity correctly stay green** — the
 *        certified region is the same either way. This also found the silent-skip hole (with
 *        nothing closing falsely, Parts 3 and 4 would skip wordlessly), so the loud-skip
 *        AddError above exists because of it.
 *   X6   the extractor keeps out-of-region blocks (re-measured here)  ->  **26 assertions**,
 *        including all five identity rows and the certified strip's lambda* window
 *        (621.957 -> 0.44049). X6 proves the identity is an assertion, not a restatement.
 *   X13  the chimney built one course short (`SpikeChimneyCourses` 30 -> 29)  ->  **9
 *        assertions**: composite block/joint pins (179/415 -> 178/414), lambda* window
 *        (0.44049 -> 0.47322), global pivot pin, the w = 3/4/5 charged pins, the contrast
 *        charged pin, the bypassed-joint margin window (1.5438 -> 1.6588). Added 2026-08-18
 *        because the composite size pins had no prover. **What still has no prover: the
 *        plain wall's 149/385 pins and its feasible control** — no chimney mutation reaches
 *        them (the point of a control); it needs a wall-side rung flip, not yet written.
 *
 * Needs a ticking world: no.
 * ==================================================================================== */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleSubUnityWallCertificateTest,
	"OracleSweepFast.RigidBlock.SubUnityWallCertificate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleSubUnityWallCertificateTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace OracleFeasibilitySpikeSupport;

	const FSubUnityPins Pins;
	const double Started = FPlatformTime::Seconds();

	/*
	 * A refusal is not an infeasibility, and SpikeIsFeasible cannot tell them apart. Every
	 * conclusion is a verdict read off a solve, the headline being "the region says FEASIBLE
	 * about a structure that says INFEASIBLE", so a refusal manufactures or destroys the
	 * finding in silence.
	 */
	const auto MustAnswer = [this](const FPoseReading& Read, const FString& Where)
	{
		return TestTrue(
			*FString::Printf(
				TEXT("%s: the solve must ANSWER — a REFUSAL and an INFEASIBILITY are ")
				TEXT("indistinguishable downstream (both are bAnswered=false, lambda=0). ")
				TEXT("It said: %s"),
				*Where, Read.WhyNot.IsEmpty() ? TEXT("(nothing)") : *Read.WhyNot),
			Read.bAnswered);
	};

	struct FStrip
	{
		int32 Blocks = 0;
		int32 Joints = 0;
		int32 Charged = 0;
		bool bOptimistic = false;
		bool bPessimistic = false;
		bool bCloses = false;
		int32 OptimisticPivots = 0;
		int32 PessimisticPivots = 0;
		double Seconds = 0.0;
	};

	/*
	 * Both boundary conditions on one mask, repair (1) live on the pessimistic side and
	 * absent from the optimistic one — the same posing RepairedRegionalSandwich uses, written
	 * locally so a shared helper's change cannot silently move the other test's measurement.
	 */
	const auto PoseStrip = [this, &MustAnswer](
		const FOracleProblem& Full, const TArray<bool>& Mask, const FString& Where) -> FStrip
	{
		FStrip Out;

		FOracleProblem Optimistic;
		FRegionCounts OptimisticCounts;
		FSurchargeCounts OptimisticSurcharge;

		SpikeExtractRepairedRegion(
			Full, Mask, /*bGroundTheShell*/ true, ESpikeSurcharge::Carried,
			Optimistic, OptimisticCounts, OptimisticSurcharge);

		FOracleProblem Pessimistic;
		FRegionCounts PessimisticCounts;
		FSurchargeCounts PessimisticSurcharge;

		SpikeExtractRepairedRegion(
			Full, Mask, /*bGroundTheShell*/ false, ESpikeSurcharge::Carried,
			Pessimistic, PessimisticCounts, PessimisticSurcharge);

		TestEqual(
			*FString::Printf(
				TEXT("%s: the OPTIMISTIC side must carry no surcharge — its soundness is a ")
				TEXT("restriction argument, and a restriction satisfies no row that was not in ")
				TEXT("the original problem"),
				*Where),
			Optimistic.AppliedForces.Num(), 0);

		const FPoseReading OptimisticRead = SpikeSolve(Optimistic);
		const FPoseReading PessimisticRead = SpikeSolve(Pessimistic);

		MustAnswer(OptimisticRead, FString::Printf(TEXT("%s GROUNDED-boundary"), *Where));
		MustAnswer(PessimisticRead, FString::Printf(TEXT("%s FREE-boundary"), *Where));

		Out.Blocks = OptimisticCounts.Blocks;
		Out.Joints = OptimisticCounts.Joints;
		Out.Charged = PessimisticSurcharge.ChargedBlocks;
		Out.bOptimistic = SpikeIsFeasible(OptimisticRead);
		Out.bPessimistic = SpikeIsFeasible(PessimisticRead);
		Out.bCloses = Out.bOptimistic == Out.bPessimistic;
		Out.OptimisticPivots = OptimisticRead.Pivots;
		Out.PessimisticPivots = PessimisticRead.Pivots;
		Out.Seconds = OptimisticRead.Seconds + PessimisticRead.Seconds;

		/* Repair (2)'s own precondition: a strip that does not reach the earth certifies nothing. */
		TestTrue(
			*FString::Printf(
				TEXT("%s: a ground-anchored region must actually contain the earth, and this one ")
				TEXT("holds %d grounded blocks"),
				*Where, PessimisticCounts.Grounded),
			PessimisticCounts.Grounded > 0);

		TestEqual(
			*FString::Printf(TEXT("%s: no ORPHAN charged component"), *Where),
			PessimisticSurcharge.OrphanComponents, 0);

		TestEqual(
			*FString::Printf(
				TEXT("%s: no surcharge delivered onto a GROUNDED block, which writes no ")
				TEXT("equilibrium row and would swallow it"),
				*Where),
			PessimisticSurcharge.DiscardedOntoGroundedUu, 0.0);

		return Out;
	};

	/* ================================================================================
	 * PART 1 — THE FIXTURE, AND THE TWO GLOBAL VERDICTS THAT MAKE IT ONE.
	 * ================================================================================ */

	constexpr int32 WallCourses = 12;
	constexpr int32 WallCells = 12;
	constexpr int32 WallDeleteCourse = 6;

	FChimneyWall Composite;
	FString Why;

	const bool bCompositeLaid = SpikeBuildChimneyWall(
		WallCourses, WallCells, SpikeChimneyCourses, WallDeleteCourse, Composite, Why);

	if (!TestTrue(
			*FString::Printf(TEXT("sub-1.0: the chimney wall must lay (it said: %s)"), *Why),
			bCompositeLaid))
	{
		return true;
	}

	FChimneyWall Plain;
	FString PlainWhy;

	const bool bPlainLaid = SpikeBuildChimneyWall(
		WallCourses, WallCells, /*ChimneyCourses*/ 0, WallDeleteCourse, Plain, PlainWhy);

	if (!TestTrue(
			*FString::Printf(TEXT("sub-1.0: the plain wall must lay (it said: %s)"), *PlainWhy),
			bPlainLaid))
	{
		return true;
	}

	FOracleProblem CompositeFull;
	FOracleProblem PlainFull;

	const bool bCompositeBridged = BuildRigidBlockProblem(Composite.Structure, CompositeFull, Why);
	const bool bPlainBridged = BuildRigidBlockProblem(Plain.Structure, PlainFull, PlainWhy);

	if (!TestTrue(
			*FString::Printf(TEXT("sub-1.0: the bridge must represent the chimney wall (%s)"), *Why),
			bCompositeBridged)
		|| !TestTrue(
			*FString::Printf(TEXT("sub-1.0: the bridge must represent the plain wall (%s)"),
				*PlainWhy),
			bPlainBridged))
	{
		return true;
	}

	/*
	 * The two fixtures must be the same wall: the same brick is deleted from the same place
	 * in both, so any difference below is the chimney. A within-run comparison, not a
	 * transcription of RepairedRegionalSandwich's 149.
	 */
	TestEqual(
		TEXT("sub-1.0: the same brick is deleted from both fixtures (X)"),
		Composite.DeleteXCm, Plain.DeleteXCm);

	TestEqual(
		TEXT("sub-1.0: the same brick is deleted from both fixtures (Z)"),
		Composite.DeleteZCm, Plain.DeleteZCm);

	FOracleProblem CompositeGlobal = CompositeFull;
	CompositeGlobal.bGravityIsLive = false;

	FOracleProblem PlainGlobal = PlainFull;
	PlainGlobal.bGravityIsLive = false;

	const FPoseReading CompositeGlobalRead = SpikeSolve(CompositeGlobal);
	const FPoseReading PlainGlobalRead = SpikeSolve(PlainGlobal);
	const FPoseReading CompositeLiveRead = SpikeSolve(CompositeFull);

	const bool bCompositeFeasible = SpikeIsFeasible(CompositeGlobalRead);
	const bool bPlainFeasible = SpikeIsFeasible(PlainGlobalRead);

	{
		const FString Line = FString::Printf(
			TEXT("SUB-1.0 FIXTURE: composite blocks=%d joints=%d (chimney %d courses, root at ")
			TEXT("X=%.4f Z=%.4f, deletion at X=%.4f Z=%.4f) | composite DEAD feasible=%d ")
			TEXT("pivots=%d secs=%.3f | composite LIVE lambda*=%.17g answered=%d pivots=%d ")
			TEXT("secs=%.3f | plain blocks=%d joints=%d DEAD feasible=%d pivots=%d | whynot ")
			TEXT("dead='%s' live='%s' plain='%s'"),
			CompositeFull.Blocks.Num(), CompositeFull.Joints.Num(), Composite.ChimneyBlocks,
			Composite.RootXCm, Composite.RootZCm, Composite.DeleteXCm, Composite.DeleteZCm,
			bCompositeFeasible ? 1 : 0, CompositeGlobalRead.Pivots, CompositeGlobalRead.Seconds,
			CompositeLiveRead.Lambda, CompositeLiveRead.bAnswered ? 1 : 0,
			CompositeLiveRead.Pivots, CompositeLiveRead.Seconds,
			PlainFull.Blocks.Num(), PlainFull.Joints.Num(), bPlainFeasible ? 1 : 0,
			PlainGlobalRead.Pivots, *CompositeGlobalRead.WhyNot, *CompositeLiveRead.WhyNot,
			*PlainGlobalRead.WhyNot);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);
	}

	MustAnswer(CompositeGlobalRead, TEXT("sub-1.0: the whole composite, posed dead"));
	MustAnswer(CompositeLiveRead, TEXT("sub-1.0: the whole composite, posed live"));
	MustAnswer(PlainGlobalRead, TEXT("sub-1.0: the plain wall, posed dead"));

	/* ---- Sizes first, so nothing below can secretly be solving another structure. ---- */

	if (Pins.CompositeBlocks == INDEX_NONE || Pins.PlainBlocks == INDEX_NONE)
	{
		AddError(FString::Printf(
			TEXT("sub-1.0: UNMEASURED SIZES — composite %d blocks / %d joints, plain %d / %d. ")
			TEXT("PREDICTED (Q3) 179/415 and 149/385."),
			CompositeFull.Blocks.Num(), CompositeFull.Joints.Num(),
			PlainFull.Blocks.Num(), PlainFull.Joints.Num()));
	}
	else
	{
		TestEqual(TEXT("sub-1.0: composite block count"),
			CompositeFull.Blocks.Num(), Pins.CompositeBlocks);
		TestEqual(TEXT("sub-1.0: composite joint count"),
			CompositeFull.Joints.Num(), Pins.CompositeJoints);
		TestEqual(TEXT("sub-1.0: the plain wall is RepairedRegionalSandwich's own fixture"),
			PlainFull.Blocks.Num(), Pins.PlainBlocks);
		TestEqual(TEXT("sub-1.0: and its joint count"),
			PlainFull.Joints.Num(), Pins.PlainJoints);

		TestEqual(
			TEXT("sub-1.0: the chimney is the ONLY difference — thirty blocks more, and thirty ")
			TEXT("joints more (its root bed plus twenty-nine chain beds)"),
			CompositeFull.Blocks.Num() - PlainFull.Blocks.Num(), SpikeChimneyCourses);
	}

	/* ---- The two verdicts that make this a fixture rather than a wall. ---- */

	TestTrue(
		*FString::Printf(
			TEXT("sub-1.0, THE WHOLE POINT: the composite must have NO admissible equilibrium at ")
			TEXT("lambda = 1, and it read feasible=%d. Without this the test is measuring a ")
			TEXT("standing wall and can say nothing about the pessimistic side."),
			bCompositeFeasible ? 1 : 0),
		!bCompositeFeasible);

	TestTrue(
		TEXT("sub-1.0: and the SAME WALL WITHOUT THE CHIMNEY must be FEASIBLE — so the chimney ")
		TEXT("is measurably the whole of the difference, rather than a structure that was ")
		TEXT("failing for some reason nobody attributed"),
		bPlainFeasible);

	if (Pins.CompositeLambdaLo < 0.0)
	{
		AddError(FString::Printf(
			TEXT("sub-1.0: UNMEASURED COMPOSITE lambda* — the live pose read %.17g in %d pivots ")
			TEXT("(%.3f s). PREDICTED (Q1) the bare stack's [0.440484, 0.440502], because the ")
			TEXT("binding joint is inside the chain; named fallback ~1.5 if the chimney's ROOT ")
			TEXT("joint governs instead."),
			CompositeLiveRead.Lambda, CompositeLiveRead.Pivots, CompositeLiveRead.Seconds));
	}
	else
	{
		TestTrue(
			*FString::Printf(
				TEXT("sub-1.0: the composite's lambda* must lie in [%.9g, %.9g] and was %.17g. ")
				TEXT("This is how far below 1.0 the structure the strip certifies actually is."),
				Pins.CompositeLambdaLo, Pins.CompositeLambdaHi, CompositeLiveRead.Lambda),
			CompositeLiveRead.Lambda >= Pins.CompositeLambdaLo
				&& CompositeLiveRead.Lambda <= Pins.CompositeLambdaHi);
	}

	if (Pins.CompositeGlobalPivots == INDEX_NONE || Pins.PlainGlobalPivots == INDEX_NONE)
	{
		AddError(FString::Printf(
			TEXT("sub-1.0: UNMEASURED GLOBAL COST — the composite answers feasibility in %d ")
			TEXT("pivots and the plain wall in %d; these are what a regional lever exists to ")
			TEXT("avoid"),
			CompositeGlobalRead.Pivots, PlainGlobalRead.Pivots));
	}
	else
	{
		TestEqual(TEXT("sub-1.0: the composite's feasibility pivot count"),
			CompositeGlobalRead.Pivots, Pins.CompositeGlobalPivots);
		TestEqual(TEXT("sub-1.0: the plain wall's feasibility pivot count"),
			PlainGlobalRead.Pivots, Pins.PlainGlobalPivots);
	}

	/* ================================================================================
	 * PART 2 — the strip ladder, and what each rung certifies.
	 *
	 * A closed sandwich here claims the whole structure feasible about one Part 1 just
	 * measured as having no equilibrium, so every closure is a false certificate and their
	 * count is the headline — pinned as a measurement, not asserted zero (as Part D does). A
	 * test demanding zero would assert the design's claim instead of checking it.
	 * ================================================================================ */

	int32 FalseCertificates = 0;
	int32 ClosingWidth = INDEX_NONE;

	FStrip ClosingStrip;
	TArray<bool> ClosingMask;

	/*
	 * The rung that charges the failing material in full and certifies anyway, kept out of
	 * the loop because it is a different finding the table leaves as two cells nobody reads
	 * together.
	 */
	int32 ChargedCertifierWidth = INDEX_NONE;
	FStrip ChargedCertifier;

	/*
	 * PREDICTED (Q7): closes at w = 0..3, not at 4 or 5. Reach at half-width w is 12.25 +
	 * 22.5w cm, the deletion is 101.25 cm from the root, so w = 4 is the first strip
	 * containing the root — from there the chimney above it has no ground path, gets charged,
	 * and sinks the pessimistic side.
	 *
	 * MEASURED 2026-08-16: widths and count as predicted, the charged column NOT. w = 3
	 * already charges all thirty chimney blocks (the root is pulled in as a shell block) and
	 * certifies anyway. The load is applied directly to the root brick and the interface
	 * joint under the chimney is dropped, never checked (hole (1)); hand statics below say it
	 * stood anyway at 1.5438 of bond, so the certificate rests on hole (3) — the chain's 29
	 * failing internal joints are in no problem. Block counts are 24w + 41 up to w = 3, then
	 * +2 and +4 as chimney courses enter.
	 */
	TArray<FStripPin> StripPins;
	StripPins.Add({ 0,  41,  0, 1, 1 });
	StripPins.Add({ 1,  65,  0, 1, 1 });
	StripPins.Add({ 2,  89,  0, 1, 1 });
	StripPins.Add({ 3, 113, 30, 1, 1 });
	StripPins.Add({ 4, 139, 28, 1, 0 });
	StripPins.Add({ 5, 153, 26, 1, 0 });

	for (const FStripPin& Pin : StripPins)
	{
		TArray<bool> Mask;
		SpikeGroundStripMask(CompositeFull, Composite.DeleteXCm, Pin.HalfWidthCells, Mask);

		const FStrip Strip = PoseStrip(
			CompositeFull, Mask, FString::Printf(TEXT("sub-1.0 strip w=%d"), Pin.HalfWidthCells));

		const bool bFalseCertificate = Strip.bCloses && (Strip.bOptimistic != bCompositeFeasible);

		if (bFalseCertificate)
		{
			++FalseCertificates;
		}

		if (Strip.bCloses && ClosingWidth == INDEX_NONE)
		{
			ClosingWidth = Pin.HalfWidthCells;
			ClosingStrip = Strip;
			ClosingMask = Mask;
		}

		if (bFalseCertificate && Strip.Charged > 0 && ChargedCertifierWidth == INDEX_NONE)
		{
			ChargedCertifierWidth = Pin.HalfWidthCells;
			ChargedCertifier = Strip;
		}

		const FString Line = FString::Printf(
			TEXT("SUB-1.0 STRIP w=%d: blocks=%d (%.1f%% of %d) joints=%d charged=%d | ")
			TEXT("optimistic=%d pivots=%d | pessimistic=%d pivots=%d | closes=%d | WHOLE ")
			TEXT("STRUCTURE feasible=%d | FALSE CERTIFICATE=%d | secs=%.3f"),
			Pin.HalfWidthCells, Strip.Blocks,
			100.0 * double(Strip.Blocks) / double(CompositeFull.Blocks.Num()),
			CompositeFull.Blocks.Num(), Strip.Joints, Strip.Charged,
			Strip.bOptimistic ? 1 : 0, Strip.OptimisticPivots,
			Strip.bPessimistic ? 1 : 0, Strip.PessimisticPivots,
			Strip.bCloses ? 1 : 0, bCompositeFeasible ? 1 : 0,
			bFalseCertificate ? 1 : 0, Strip.Seconds);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);

		if (Pin.Blocks == INDEX_NONE || Pin.Charged == INDEX_NONE
			|| Pin.Optimistic == INDEX_NONE || Pin.Pessimistic == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("sub-1.0 strip w=%d: UNMEASURED RUNG — pin blocks=%d charged=%d ")
				TEXT("optimistic=%d pessimistic=%d"),
				Pin.HalfWidthCells, Strip.Blocks, Strip.Charged,
				Strip.bOptimistic ? 1 : 0, Strip.bPessimistic ? 1 : 0));
		}
		else
		{
			TestEqual(*FString::Printf(TEXT("sub-1.0 strip w=%d: region block count"),
				Pin.HalfWidthCells), Strip.Blocks, Pin.Blocks);

			TestEqual(
				*FString::Printf(
					TEXT("sub-1.0 strip w=%d: how many omitted blocks the region is CHARGED for ")
					TEXT("— zero while the chimney can still reach the ground without the strip, ")
					TEXT("and non-zero from the width that swallows its root"),
					Pin.HalfWidthCells),
				Strip.Charged, Pin.Charged);

			TestEqual(*FString::Printf(TEXT("sub-1.0 strip w=%d: GROUNDED-boundary verdict"),
				Pin.HalfWidthCells), Strip.bOptimistic ? 1 : 0, Pin.Optimistic);

			TestEqual(*FString::Printf(TEXT("sub-1.0 strip w=%d: FREE-boundary verdict"),
				Pin.HalfWidthCells), Strip.bPessimistic ? 1 : 0, Pin.Pessimistic);
		}
	}

	if (Pins.FalseCertificates == INDEX_NONE)
	{
		AddError(FString::Printf(
			TEXT("sub-1.0: UNMEASURED FALSE-CERTIFICATE COUNT — %d of the six strip widths ")
			TEXT("closed while the whole structure has no equilibrium. PREDICTED (Q6/Q7) FOUR: ")
			TEXT("w = 0, 1, 2 and 3. Zero would mean the repaired pessimistic side held on the ")
			TEXT("only fixture shape that could catch it lying."),
			FalseCertificates));
	}
	else
	{
		TestEqual(
			TEXT("sub-1.0, THE HEADLINE: how many of the six ground-anchored strips certify ")
			TEXT("'the whole structure is feasible' about a structure that has no equilibrium. ")
			TEXT("This is pinned as a MEASUREMENT, not asserted to be zero — a test that ")
			TEXT("demanded zero would be asserting the design's claim instead of checking it."),
			FalseCertificates, Pins.FalseCertificates);
	}

	/*
	 * The second unsoundness, which the prediction did not contain. Q7 expected every false
	 * certificate from the carried-set rule being blind (§5.3's hole (1): omitted material
	 * with its own ground path, charged nothing). One rung is not blind: it charges the whole
	 * chimney on the vertical through its centre of gravity and certifies anyway.
	 *
	 * Where the load goes: the surcharge is applied directly to the root brick, and the
	 * interface joint it would cross is dropped with every other joint leaving the region. So
	 * hole (1) is present too — that joint is bypassed, not exercised — but it does not matter
	 * (hand statics below: it stood at 1.5438 of two-contact bond). **The certificate rests on
	 * hole (3) alone**: the charged component's internal joints vanish, so the 29 failing
	 * chain joints are in no problem. A surcharge carries a force and a moment, no strength.
	 */
	if (Pins.FalseCertificates != INDEX_NONE && Pins.FalseCertificates > 0)
	{
		TestTrue(
			*FString::Printf(
				TEXT("sub-1.0: at least one FALSE certificate must come from a strip that ")
				TEXT("CHARGED the failing material in full and certified anyway — measured at ")
				TEXT("half-width %d, %d blocks charged, pessimistic feasible=%d. If this ever ")
				TEXT("stops holding, the surcharge has started carrying the charged component's ")
				TEXT("own strength and hole (3) of PROMOTION_DESIGN §5.3's box has moved."),
				ChargedCertifierWidth, ChargedCertifier.Charged,
				ChargedCertifier.bPessimistic ? 1 : 0),
			ChargedCertifierWidth != INDEX_NONE && ChargedCertifier.Charged > 0);

		/*
		 * The bypassed joint, priced by hand so the attribution is a pin, not a sentence.
		 * The surcharge goes straight to the root brick and the interface joint is dropped, so
		 * hole (1) is present at this rung too. Attributing the certificate to hole (3) alone
		 * claims the bypassed joint would have stood — arithmetic on the fixture's constants,
		 * not on what the solver reports, so it can disagree with the solver rather than echo it.
		 *
		 * The model's two-contact form: a joint has two contacts at +/- h, each with tributary
		 * area A/2, capped at f_t x A/2. Vertical equilibrium gives n1 + n2 = W; moment about
		 * the centre gives h x (n2 - n1) = W x e. So the far contact carries T = W x (e/h - 1)/2
		 * tension, the near W x (e/h + 1)/2 compression.
		 *
		 * The other two axes were worked first, since ComputeUtilisation returning the worst of
		 * three is how a test aimed at one axis silently measures another (TRAPS): the near
		 * contact's compression is ~0.053 of the 10 MPa cap, and a vertical surcharge puts no
		 * shear through a bed joint. Tension governs, at 0.648.
		 */
		const double ChimneyWeightUu =
			double(SpikeChimneyCourses) * SpikeBrickMassKg * OracleGravityCmPerSecondSquared;

		/* Course c sits c x lean outboard of the root, so the mean of 0..N-1 is the lever. */
		const double LeverArmCm =
			0.5 * double(SpikeChimneyCourses - 1) * SpikeChimneyLeanCm;

		const double HalfLengthCm = 0.5 * SpikeBrickLengthCm;
		const double ContactAreaSqCm = 0.5 * SpikeBrickLengthCm * SpikeBrickWidthCm;

		/* 1 MPa = 100 N/cm2 and 1 N = 100 uu, derived here rather than imported. */
		const double UuPerMPaSqCm = 100.0 * 100.0;

		const double TensionDemandUu =
			ChimneyWeightUu * (LeverArmCm / HalfLengthCm - 1.0) * 0.5;
		const double TensionCapacityUu =
			GeneralPurposeMortar.TensileStrengthMPa * ContactAreaSqCm * UuPerMPaSqCm;
		const double CompressionDemandUu =
			ChimneyWeightUu * (LeverArmCm / HalfLengthCm + 1.0) * 0.5;
		const double CrushCapacityUu =
			GeneralPurposeMortar.CompressiveStrengthMPa * ContactAreaSqCm * UuPerMPaSqCm;

		const double BypassedMargin = TensionCapacityUu / TensionDemandUu;

		const FString BypassLine = FString::Printf(
			TEXT("SUB-1.0 BYPASSED JOINT (the root bed joint the surcharge never crosses): ")
			TEXT("W=%.17g uu at e=%.17g cm, h=%.17g cm, contact area=%.17g cm2 | TENSION ")
			TEXT("demand=%.17g capacity=%.17g margin=%.17g | COMPRESSION demand=%.17g ")
			TEXT("capacity=%.17g utilisation=%.6g"),
			ChimneyWeightUu, LeverArmCm, HalfLengthCm, ContactAreaSqCm,
			TensionDemandUu, TensionCapacityUu, BypassedMargin,
			CompressionDemandUu, CrushCapacityUu, CompressionDemandUu / CrushCapacityUu);

		UE_LOG(LogTemp, Display, TEXT("%s"), *BypassLine);
		AddInfo(BypassLine);

		TestTrue(
			*FString::Printf(
				TEXT("sub-1.0: the axis must be TENSION, not crushing — the near contact reads ")
				TEXT("%.6g of its cap against the far contact's %.6g, and an attribution made ")
				TEXT("against the wrong axis is how this project has been wrong before"),
				CompressionDemandUu / CrushCapacityUu, TensionDemandUu / TensionCapacityUu),
			TensionDemandUu / TensionCapacityUu > CompressionDemandUu / CrushCapacityUu);

		TestTrue(
			*FString::Printf(
				TEXT("sub-1.0: the BYPASSED interface joint would have STOOD — %.17g uu of ")
				TEXT("tension against %.17g uu of two-contact bond, a margin of %.17g. This is ")
				TEXT("what licenses attributing the false certificate to hole (3) ALONE: hole ")
				TEXT("(1) is present (the joint is dropped, never checked) and is not what the ")
				TEXT("certificate rests on."),
				TensionDemandUu, TensionCapacityUu, BypassedMargin),
			BypassedMargin > 1.0);

		TestTrue(
			*FString::Printf(
				TEXT("sub-1.0: and that margin is pinned in [%.9g, %.9g] rather than left as a ")
				TEXT("greater-than, because a bare 'it stands' would survive the fixture's ")
				TEXT("geometry moving by a factor. It read %.17g."),
				SpikeBypassedMarginLo, SpikeBypassedMarginHi, BypassedMargin),
			BypassedMargin >= SpikeBypassedMarginLo && BypassedMargin <= SpikeBypassedMarginHi);
	}

	/* ================================================================================
	 * PART 3 — why it happens, as an identity rather than a sentence.
	 *
	 * The certified region problem is the same whether or not a collapsing chimney is
	 * attached: the extractor drops every chimney block and joint, and the Carried rule
	 * charges nothing because the chimney reaches ground down the wall's columns. Same blocks,
	 * joints, verdicts, pivots — and a region whose answer cannot depend on the material it
	 * dropped cannot be a bound on a structure that includes it. The false certificate's
	 * mechanism, measured within one run.
	 * ================================================================================ */

	/*
	 * A skip must be loud. Parts 3 and 4 hang off a sandwich having closed, and a run where
	 * none does would otherwise pass with the identity and the "how false" number never
	 * evaluated — a test asserting nothing. Found by mutation X11, which does exactly that.
	 */
	if (ClosingWidth == INDEX_NONE)
	{
		AddError(
			TEXT("sub-1.0: NO STRIP CLOSED at any width, so the IDENTITY and the certified ")
			TEXT("strip's own lambda* were never evaluated. That is either the finding ")
			TEXT("inverting — the repaired pessimistic side holding on this fixture, which is a ")
			TEXT("result and must be pinned as one — or a fixture that stopped being what it ")
			TEXT("was built to be. It is never a pass."));
	}

	if (ClosingWidth != INDEX_NONE)
	{
		TArray<bool> PlainMask;
		SpikeGroundStripMask(PlainFull, Plain.DeleteXCm, ClosingWidth, PlainMask);

		const FStrip PlainStrip = PoseStrip(
			PlainFull, PlainMask,
			FString::Printf(TEXT("sub-1.0 CHIMNEY-FREE strip w=%d"), ClosingWidth));

		const FString Line = FString::Printf(
			TEXT("SUB-1.0 IDENTITY w=%d: with chimney blocks=%d joints=%d charged=%d ")
			TEXT("optimistic=%d/%d pessimistic=%d/%d | WITHOUT chimney blocks=%d joints=%d ")
			TEXT("charged=%d optimistic=%d/%d pessimistic=%d/%d"),
			ClosingWidth, ClosingStrip.Blocks, ClosingStrip.Joints, ClosingStrip.Charged,
			ClosingStrip.bOptimistic ? 1 : 0, ClosingStrip.OptimisticPivots,
			ClosingStrip.bPessimistic ? 1 : 0, ClosingStrip.PessimisticPivots,
			PlainStrip.Blocks, PlainStrip.Joints, PlainStrip.Charged,
			PlainStrip.bOptimistic ? 1 : 0, PlainStrip.OptimisticPivots,
			PlainStrip.bPessimistic ? 1 : 0, PlainStrip.PessimisticPivots);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);

		TestEqual(
			TEXT("sub-1.0 IDENTITY: the certified region has the same block count with the ")
			TEXT("chimney attached as without it"),
			ClosingStrip.Blocks, PlainStrip.Blocks);

		TestEqual(
			TEXT("sub-1.0 IDENTITY: and the same joint count — the extractor drops every ")
			TEXT("chimney joint"),
			ClosingStrip.Joints, PlainStrip.Joints);

		TestEqual(
			TEXT("sub-1.0 IDENTITY: and charges the same nothing — the Carried rule sees a ")
			TEXT("chimney that reaches the ground down the wall's own columns"),
			ClosingStrip.Charged, PlainStrip.Charged);

		TestEqual(
			TEXT("sub-1.0 IDENTITY: the GROUNDED-boundary side takes the same pivot path"),
			ClosingStrip.OptimisticPivots, PlainStrip.OptimisticPivots);

		TestEqual(
			TEXT("sub-1.0 IDENTITY, THE MECHANISM: the FREE-boundary side takes the same pivot ")
			TEXT("path and returns the same verdict whether or not a collapsing chimney is ")
			TEXT("attached to the wall. A region whose answer CANNOT DEPEND on the material it ")
			TEXT("dropped cannot be a bound on a structure that includes it — that is why the ")
			TEXT("certificate above is what it is, stated as an equation rather than as prose."),
			ClosingStrip.PessimisticPivots, PlainStrip.PessimisticPivots);

		TestEqual(
			TEXT("sub-1.0 IDENTITY: and the same FREE-boundary verdict"),
			ClosingStrip.bPessimistic ? 1 : 0, PlainStrip.bPessimistic ? 1 : 0);

		/* ============================================================================
		 * PART 4 — how false, as a number.
		 *
		 * Part D's precedent: a boolean trio only says the certificate is on the wrong side of
		 * 1.0. The certified region's own lambda* says whether the failure is a knife edge a
		 * tighter extractor could tune away, or a gap no boundary rule can close.
		 * ============================================================================ */

		FOracleProblem CertifiedLive;
		FRegionCounts CertifiedCounts;
		FSurchargeCounts CertifiedSurcharge;

		SpikeExtractRepairedRegion(
			CompositeFull, ClosingMask, /*bGroundTheShell*/ false, ESpikeSurcharge::Carried,
			CertifiedLive, CertifiedCounts, CertifiedSurcharge);

		CertifiedLive.bGravityIsLive = true;

		const FPoseReading CertifiedLiveRead = SpikeSolve(CertifiedLive);

		MustAnswer(CertifiedLiveRead, TEXT("sub-1.0: the certified strip, posed live"));

		const FString RatioLine = FString::Printf(
			TEXT("SUB-1.0 HOW FALSE: the certified strip's own lambda*=%.17g (%d pivots) against ")
			TEXT("the whole structure's %.17g — the region the sandwich certified stands at ")
			TEXT("%.1fx the load under which the structure it certified has no equilibrium"),
			CertifiedLiveRead.Lambda, CertifiedLiveRead.Pivots, CompositeLiveRead.Lambda,
			CompositeLiveRead.Lambda > 0.0
				? CertifiedLiveRead.Lambda / CompositeLiveRead.Lambda : -1.0);

		UE_LOG(LogTemp, Display, TEXT("%s"), *RatioLine);
		AddInfo(RatioLine);

		if (Pins.StripLambdaLo < 0.0)
		{
			AddError(FString::Printf(
				TEXT("sub-1.0: UNMEASURED CERTIFIED-STRIP lambda* — it read %.17g in %d pivots. ")
				TEXT("PREDICTED (Q8) 400-1,100, a ratio of ~1,000-2,500x against the structure's ")
				TEXT("own lambda*, an order of magnitude past PART D's 41.96x."),
				CertifiedLiveRead.Lambda, CertifiedLiveRead.Pivots));
		}
		else
		{
			TestTrue(
				*FString::Printf(
					TEXT("sub-1.0: the certified strip's OWN lambda* must lie in [%.9g, %.9g] and ")
					TEXT("was %.17g. Measured on this exact region and never scaled from another ")
					TEXT("fixture — the point of the number is that the certified region is ")
					TEXT("nowhere near the boundary, so no tightening of a boundary rule closes ")
					TEXT("a gap of that size."),
					Pins.StripLambdaLo, Pins.StripLambdaHi, CertifiedLiveRead.Lambda),
				CertifiedLiveRead.Lambda >= Pins.StripLambdaLo
					&& CertifiedLiveRead.Lambda <= Pins.StripLambdaHi);
		}
	}

	/* ================================================================================
	 * PART 5 — the contrast: the same structure, cut where the failure is.
	 *
	 * The repair is not useless; this row says where it works. Cut the strip at the chimney's
	 * root instead of the deletion and the chimney above becomes a component with no ground
	 * path, so the Carried rule charges it, its weight acts on the vertical through its centre
	 * of gravity, and the pessimistic side reads infeasible. The sandwich opens, no
	 * certificate — correct.
	 *
	 * So the same structure gives a false certificate at one cut and a correct refusal at
	 * another, differing exactly in whether the failing material has a ground path the region
	 * does not provide — unsoundness (1) of §5.3's box, demonstrated.
	 * ================================================================================ */

	{
		TArray<bool> ContrastMask;
		SpikeGroundStripMask(CompositeFull, Composite.RootXCm, /*HalfWidthCells*/ 0, ContrastMask);

		const FStrip Contrast = PoseStrip(
			CompositeFull, ContrastMask, TEXT("sub-1.0 CONTRAST strip at the chimney root"));

		const FString Line = FString::Printf(
			TEXT("SUB-1.0 CONTRAST (strip cut at the chimney root X=%.4f, w=0): blocks=%d ")
			TEXT("joints=%d charged=%d | optimistic=%d pivots=%d | pessimistic=%d pivots=%d | ")
			TEXT("closes=%d"),
			Composite.RootXCm, Contrast.Blocks, Contrast.Joints, Contrast.Charged,
			Contrast.bOptimistic ? 1 : 0, Contrast.OptimisticPivots,
			Contrast.bPessimistic ? 1 : 0, Contrast.PessimisticPivots,
			Contrast.bCloses ? 1 : 0);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);

		if (Pins.ContrastBlocks == INDEX_NONE || Pins.ContrastCharged == INDEX_NONE
			|| Pins.ContrastOptimistic == INDEX_NONE || Pins.ContrastPessimistic == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("sub-1.0 CONTRAST: UNMEASURED — pin blocks=%d charged=%d optimistic=%d ")
				TEXT("pessimistic=%d. PREDICTED (Q9) ~27 charged, optimistic feasible, ")
				TEXT("pessimistic INFEASIBLE, so no certificate is issued."),
				Contrast.Blocks, Contrast.Charged,
				Contrast.bOptimistic ? 1 : 0, Contrast.bPessimistic ? 1 : 0));
		}
		else
		{
			TestEqual(TEXT("sub-1.0 CONTRAST: region block count"),
				Contrast.Blocks, Pins.ContrastBlocks);

			TestEqual(
				TEXT("sub-1.0 CONTRAST: the chimney above this strip has no ground path of its ")
				TEXT("own, so the Carried rule charges it — this is repair (1) doing the work it ")
				TEXT("was ruled for, on the same structure where it charged nothing"),
				Contrast.Charged, Pins.ContrastCharged);

			TestEqual(TEXT("sub-1.0 CONTRAST: GROUNDED-boundary verdict"),
				Contrast.bOptimistic ? 1 : 0, Pins.ContrastOptimistic);

			TestEqual(
				TEXT("sub-1.0 CONTRAST: FREE-boundary verdict — the two sides disagree, so the ")
				TEXT("rule says GROW THE REGION and no certificate is issued"),
				Contrast.bPessimistic ? 1 : 0, Pins.ContrastPessimistic);
		}
	}

	{
		const FString Line = FString::Printf(
			TEXT("SUB-1.0 TOTALS: %d false certificates over six strip widths. Test took %.3f s."),
			FalseCertificates, FPlatformTime::Seconds() - Started);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
