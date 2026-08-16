// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"
#include "Core/WallCases.h"
#include "Tests/RigidBlockOracle.h"
#include "World/DestructionScenarios.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * SLICE 0b — THE LATENCY SPIKE, AND IT IS A HARD GATE (PROMOTION_DESIGN.md §6, §11 R1/R2;
 * user ruling D2, 2026-08-14). This file measures two things and rules on one question.
 * It writes no production code and it is measurement only: under the project's convention
 * a measurement lands as a PINNED ROW whose UNMEASURED state is its red.
 *
 * THE QUESTION, and the criterion that is binding rather than advisory:
 *
 *     Can an 84-block region answer in ~50 ms — two solves inside a 100 ms budget?
 *
 * If it cannot, synchronous LP authority at scenario scale is OFF THE TABLE and the
 * promotion design changes shape rather than proceeding. A fired kill criterion is a
 * successful measurement. Nothing in this file may be softened to make the gate pass; the
 * arithmetic that decides it is written out at the bottom of this header and re-stated
 * beside the measurement it rests on.
 *
 * ================================================================================
 * MEASUREMENT 1 — DOES ABANDONING lambda* PAY, AND BY HOW MUCH?
 * ================================================================================
 *
 * THE REFORMULATION. Every fixture the project owns is posed with gravity LIVE, so the
 * LP maximises lambda and answers "how many times its own weight". That pose starts
 * TRIVIALLY FEASIBLE — every equality row's right-hand side is zero, lambda = 0 solves it
 * — so phase 1 never runs and the entire measured cost is phase 2 climbing lambda from 0
 * to lambda*. wall-01's 58,605 pivots and ~252 s are exactly that climb.
 *
 * Production does not want lambda*. It wants: IS THERE AN ADMISSIBLE EQUILIBRIUM AT
 * lambda = 1, and if not what moves. That is a FEASIBILITY problem, and it is posed with
 * a field that already exists — FOracleProblem::bGravityIsLive = false — so gravity
 * enters the equality rows as a CONSTANT and phase 1 must genuinely run.
 *
 * WHAT THE DEAD POSE MAKES THE SOLVER DO, worked from the assembly rather than assumed:
 *
 *   - Gravity goes to DeadZ, so RowFz.Rhs = +W per block and phase 1 starts infeasible.
 *     Phase 1 must drive out one artificial per equality row (3 per non-grounded block).
 *   - The lambda column then has NO entry in any equilibrium row — its only constraint is
 *     the cap row lambda <= LambdaCap. So phase 2 has nothing to climb: a feasible problem
 *     reports lambda EXACTLY LambdaCap, and an infeasible one returns the oracle's
 *     dead-load arm, bAnswered = true with lambda EXACTLY 0.
 *   - Those two exact values are asserted per row and they are not decoration: if a live
 *     load ever leaked into a pose meant to be dead, lambda would land somewhere between
 *     and the pin would fire. A feasibility measurement taken on a problem that still has
 *     a live load is the whole slice being wrong quietly.
 *
 * THE CROSS-VALIDATION, which is what makes this a measurement rather than a timing run.
 * The feasibility answer is a BOOLEAN and it must agree with the lambda* pose's own
 * verdict: feasible at lambda = 1 exactly when lambda* >= 1. Both poses are solved in the
 * same run on the same structure, so the agreement is measured and not inherited. The
 * fixture list deliberately carries BOTH ARMS — the 30-course leaning stack is
 * lambda* = 0.44 and MUST come back infeasible — because an "always feasible" list would
 * make the boolean vacuous, which is TRAPS' invariant-over-fixtures-that-share-a-hidden-
 * property trap in its exact form.
 *
 * THE EARLY EXIT WAS UNMEASURABLE FROM OUTSIDE AND IS NOT ANY MORE. §3.2 and §5.2 claim a
 * feasible problem is proved the moment phase 1's infeasibility sum reaches tolerance — no
 * optimality proof needed — and that most of a standing structure's saving lives there.
 * RunRevisedSimplex runs phase 1 to OPTIMALITY and reads the sum only after it returns, so
 * the pivots spent proving optimality AFTER feasibility was already reached were invisible.
 * The seam landed inside this slice: FOracleResult::PhaseOnePivots and
 * PivotsToFirstFeasible are set during the solve, as OBSERVATION ONLY — the solver still
 * runs to optimality, so every lambda*, pivot count and scan count in the suite came back
 * bit-identical. The rows below now carry both as EXACT PINS, per this file's own rule that
 * every deterministic integer it measures is pinned rather than printed.
 *
 * ================================================================================
 * MEASUREMENT 2 — THE REGIONAL SANDWICH, WHICH HAS NEVER BEEN RUN AT ALL
 * ================================================================================
 *
 * The roadmap's "a deleted brick provably cannot affect masonry beyond a bonded distance"
 * is false in this model and PROMOTION_DESIGN §5.3 says so with counter-examples. What it
 * proposes instead is a two-sided sandwich, provable rather than hopeful:
 *
 *   - boundary blocks pinned GROUNDED — an artificial foundation, so the region is
 *     OPTIMISTIC: infeasible here means the whole structure is infeasible;
 *   - boundary blocks FREE — they carry their own weight and nothing holds them, so the
 *     region is PESSIMISTIC: feasible here means the whole structure is feasible;
 *   - agreement certifies; disagreement grows the region.
 *
 * THE REGION EXTRACTOR IS TEST-SIDE, exactly as the case-21 strength-override probes are:
 * FOracleProblem is a plain struct of public arrays, so a region is a filter over blocks
 * and joints plus one flag per boundary block. Nothing in RigidBlockOracle.h/.cpp is
 * needed to pose it, which is the property that keeps the oracle independently derived.
 *
 * ================================================================================
 * PREDICTIONS — DERIVATION RECORD REVISION 1, WRITTEN BEFORE THE FIRST RUN
 * (PROMOTION_DESIGN §7.3 makes this mandatory: a prediction written after the
 *  measurement is not evidence, it is a transcription.)
 * ================================================================================
 *
 * MEASUREMENT 1. Phase 1 must pivot out one artificial per equality row, and there are
 * 3 rows per non-grounded block, so its pivot count has a FLOOR of ~3 x blocks and should
 * land within a small multiple of it. Phase 2's climb has no such floor and is set by how
 * many capacities bind on the way to lambda*. So the prediction is: the dead pose costs
 * ~O(rows) pivots on EVERY fixture, and the speedup is large exactly where the lambda*
 * climb was long.
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
 * PREDICTED SPEEDUP: ~3-5x on the 84-block wall, ~10-20x on corbel D, ABOUT 1x ON
 * WALL-18 (its lambda* climb is only 791 pivots, which is already near the phase-1 floor
 * — the design's 10-100x estimate cannot hold on a fixture whose climb is short), and
 * ~15-40x on wall-01. So the honest predicted headline is NOT the design's 10-100x: it is
 * "a few x on small standing structures, an order or more only where the climb was long".
 *
 * PREDICTED GATE ARITHMETIC: 0.61 s / 4 ~ 0.15 s for the 84-block wall, against a ~50 ms
 * target. THE KILL CRITERION IS PREDICTED TO FIRE on the feasibility lever alone, with
 * ~3x still owed to warm starts and the per-iteration levers (PROMOTION_DESIGN §5.5
 * estimates ~1 order for all of those together, so the gate is predicted RECOVERABLE but
 * NOT MET TODAY).
 *
 * MEASUREMENT 2, and this prediction is arithmetic rather than a guess. SUM THE VERTICAL
 * EQUILIBRIUM ROWS OF A FREE-BOUNDARY REGION: every internal contact force appears twice
 * with opposite signs (BlockA carries -1 where BlockB carries +1) and cancels, and every
 * joint leaving the region has been dropped. What is left is
 *
 *         0 = - (total weight of the region)
 *
 * which has no solution for any region of positive weight. UNLESS the region contains a
 * GROUNDED block — a grounded block writes no equilibrium row, so its contacts do not
 * cancel and it is the only thing that can supply the reaction. Therefore:
 *
 *     PREDICTED: the free-boundary (pessimistic) side is INFEASIBLE for every region that
 *     does not reach the ground, whatever the radius and whatever the wall. It can only
 *     certify a ground-connected region.
 *
 *     PREDICTED CONSEQUENCE: for a brick deleted k courses above the foundation, the
 *     sandwich closes at the first radius R >= k and at no smaller one.
 *
 *     PREDICTED R2 ANSWER: over ten deletions at courses 1..10 with radius fixed at 4,
 *     the two sides agree on FOUR of ten (courses 1-4) and disagree on six.
 *
 * If that prediction holds, the sandwich does not degenerate to "always grow to the whole
 * structure" — it degenerates to "always grow to the ground", which for a 30-course wall
 * is a region of full height and is a materially different answer from the design's
 * "should agree at a small radius". Saying which of those it is, with a number, is this
 * measurement's whole job.
 *
 * ================================================================================
 * WHAT WAS MEASURED — 2026-08-15, one run each, tree at HEAD 9dcbd76
 * ================================================================================
 *
 * MEASUREMENT 1. Every dead-pose pivot count landed inside its predicted range except
 * corbel D (1,022 against a 400-900 range) and wall-01 (5,407 against 1,500-4,000), both
 * HIGH. The speedups, live pose over dead pose:
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
 * (t) = TRIMMED 2026-08-16. Those four rows no longer re-solve the LIVE pose — its lambda*
 * is pinned in OracleSweepFull.RigidBlock.WallsAndLadders and its solve was identical —
 * so their live columns above are the 2026-08-15 measurement of record and their speedup
 * is a CROSS-RUN reading from here on. Every DEAD column is still measured every run.
 *
 * FOUR THINGS THE MEASUREMENT SAID THAT THE PREDICTION DID NOT:
 *
 *   - THE SECONDS RATIO BEATS THE PIVOT RATIO EVERYWHERE EXCEPT wall-01, by up to 5x
 *     (wall-18: 8.3x of pivots buying 44.6x of time). A phase-1 pivot is cheaper than a
 *     phase-2 pivot on the same problem — unexplained, and worth an attribution slice
 *     before anyone budgets on the pivot ratio alone.
 *   - THE LEVER WEAKENS WITH SCALE. Dead-pose pivots run 2.2x the equality-row count at
 *     84 blocks and ~5x it at 375, so phase 1 is not O(rows) with a small constant and
 *     wall-01's 10.9x is the SMALLEST large-fixture speedup, not the largest.
 *   - THE SMALL FIXTURES GO THE OTHER WAY: the 5- and 30-course stacks cost 5.5x MORE
 *     pivots in the dead pose (22 against 4, 163 against 29). Where the lambda* climb is
 *     genuinely short, phase 1 is the expensive half. The design's 10-100x estimate is
 *     refuted as a general figure in BOTH directions.
 *   - PROVING INFEASIBILITY IS NOT THE EXPENSIVE DIRECTION here. The 30-course stack, the
 *     one infeasible fixture, answers in 163 pivots and 0.006 s — the same time as its
 *     lambda* solve. §3.2's "standing is cheap and falling is expensive" is not what this
 *     measurement shows.
 *
 * MEASUREMENT 1b — THE EARLY EXIT, AND §5.2's LARGEST REMAINING CLAIM IS REFUTED.
 *
 * THE PREDICTION ON THE TABLE was the design's, not this file's: §3.2 promised that
 * proving feasibility can stop the moment the infeasibility sum reaches zero while proving
 * INfeasibility needs phase 1 run to optimality, and §5.2 put "most of a standing
 * structure's saving" there. Revision 1 of this file's derivation record priced it at
 * nothing, because the two fields were unpopulated and it could not. So the honest
 * prediction of record is the design's qualitative one: LARGE.
 *
 * MEASURED, dead pose, phase-1 pivots / the pivot at which the problem FIRST read feasible
 * (INDEX_NONE = never, which is what an infeasible problem must report):
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
 * **0.63% POOLED, 3.85% ON THE WORST SINGLE FIXTURE.** And the reason is STRUCTURAL rather
 * than a bad fixture list, which is what makes it a refutation rather than a disappointing
 * number: phase 1's OBJECTIVE *is* the infeasibility sum, so "the sum reaches tolerance"
 * and "phase 1 is optimal" coincide by construction. There is no optimality-proof tail to
 * skip — only the handful of degenerate pivots after the last artificial reaches zero.
 * §3.2's "proving feasibility can stop the moment the sum reaches zero" is TRUE and worth
 * nothing: the saving it names is already spent.
 *
 * TWO SUPPORTING FACTS, recorded beside the conclusion because each closes an escape:
 *
 *   - THE PHASE-1 GATE IS ALREADY LOOSE — InfeasibilityTolerance() is (1 + largest basic)
 *     x 1e-9 x rows, which is ~13 in absolute terms at wall-01 scale with the 1e6 cap RHS.
 *     A STRICTER gate can therefore only move first-feasibility LATER and make the saving
 *     SMALLER. The 0.63% is an upper bound on this lever, not a mid-point.
 *   - THE ONE RESIDUE THESE TWO COUNTERS CANNOT SEE is the pricer's closing FULL sweep,
 *     which costs column SCANS rather than pivots. It is bounded above by pricing's own
 *     measured share of runtime — 0.5% on wall-01 since partial pricing landed — so it
 *     cannot be hiding the lever either.
 *
 * ALL EIGHT GRAVITY-LIVE POSES REPORT 0/0, which confirms from the OUTSIDE what the
 * solver's comment claims from the inside: a live pose is feasible before a single pivot
 * and phase 1 never runs at all. That pair is asserted on every row for the same reason
 * the exact LambdaCap-or-0 pin exists — it is how a live load leaking into a pose posed as
 * dead would be caught.
 *
 * MEASUREMENT 2. The conservation prediction held exactly, and everything downstream of
 * it followed:
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
 * THE CLOSING RADIUS IS 5, AND THE REGION THERE IS 142 OF 149 BLOCKS — 95% of the wall.
 * The free-boundary side read INFEASIBLE at every radius whose region contained no
 * grounded block and FEASIBLE at every radius whose region contained one, with no
 * exception, exactly as the vertical-equilibrium sum requires.
 *
 * R2's ANSWER: FIVE of ten deletions certify at radius 4 — courses 1-5, the ones whose
 * region reaches the foundation. Predicted four; the extra one is the SHELL reaching a
 * course beyond the core, so the prediction was wrong by exactly one course for a named
 * reason. Courses 6-10 do not certify at any radius short of the ground.
 *
 * AND THE COST GOES THE WRONG WAY. At the closing radius the sandwich's two solves take
 * 753 + 872 = 1,625 pivots where solving the WHOLE WALL takes 965: the lever is 1.68x
 * more expensive than the work it exists to avoid.
 *
 * THE COLLAPSE ARM IS WORSE. On the 30-course leaning stack the optimistic region is
 * still feasible at radius 13 (29 of 30 blocks) and only certifies at radius 14, where
 * the region is ALL THIRTY — the whole structure. No proper subset certifies.
 *
 * AND THE PESSIMISTIC SIDE IS NOT A BOUND AT ALL (Part D). The stack's bottom band —
 * courses 0..5, grounded, twenty-four courses dropped — is FEASIBLE with a free boundary
 * AND with a grounded boundary, so the sandwich CLOSES and certifies "the full structure
 * is feasible" about a stack whose lambda* is 0.4405. Dropping the material outside a
 * region removes its WEIGHT along with its restraint, so §5.3's "the found force system
 * extends" does not hold and a closed sandwich can be a FALSE CERTIFICATE. This is not a
 * lever that under-delivers; as specified it is unsound.
 *
 * AND IT IS NOT A KNIFE EDGE. The certified band's OWN lambda*, measured in the live pose,
 * is **18.481256459924058** against the whole stack's 0.44048 — the region the sandwich
 * certified stands at **41.96x** the load under which the structure it certified has no
 * equilibrium. That factor is what says no tightening of the extractor rescues the lever:
 * a boundary rule can move a marginal answer, it cannot move forty times. Only carrying
 * the omitted material's weight can, which is the repair §12 D2' rules for. (Independent
 * check: review priced the band at ~18.7 from a (N-1)^-2.29 fit to the pinned stack rungs,
 * 1.2% from the solve — a second derivation agreeing, not a transcription.)
 *
 * PART B ALSO CHECKS ITS FIVE CERTIFICATES against the whole cut wall's own verdict, one
 * 149-block dead solve per deletion. It did not until 2026-08-15, which left the count of
 * five as a count of certificates nobody had checked — in the one test that demonstrates a
 * false certificate. All five hold on these ten fixtures.
 *
 * ================================================================================
 * THE GATE VERDICT
 * ================================================================================
 *
 * R1 (latency of one solve): the 84-block fixture answers feasibility in 0.065 s against
 * a ~0.050 s target. The design's stated gap was 12x; the MEASURED gap is 1.3x, with warm
 * starts — the criterion's own named escape — unbuilt and unmeasured. **The kill criterion
 * does not fire on R1.**
 *
 * R2 (regional decomposition): **it fires.** The sandwich certifies only when its region
 * reaches the ground, its closing region is 95-100% of the structure, it costs 1.68x a
 * global solve on the fixture where it does close, and its pessimistic side can certify a
 * structure that has no equilibrium. Regional decomposition is REQUIRED for scenario scale
 * — wall-01's 375 blocks answer feasibility in 26 s and scenario scale is ~3.3x that — so
 * R2 firing is the one that changes the design's shape.
 *
 * ================================================================================
 * WHAT IS PINNED AND WHAT IS DELIBERATELY NOT
 * ================================================================================
 *
 * PINNED EXACTLY: block and joint counts per fixture (the N10 rung-flip lesson — a window
 * and a ratio cannot catch a row secretly solving the wrong problem), pivot counts (the
 * solver is bit-deterministic, so a pivot count is an exact quantity), the feasibility
 * verdict, the exact lambda values the pose implies (LambdaCap or 0), the early exit's two
 * pivot indices per pose and their pooled totals, region block counts, the sandwich's
 * closing radius, and the agreement count.
 *
 * ALSO ASSERTED, ON EVERY SOLVE IN BOTH TESTS: that the solve ANSWERED. A REFUSAL IS NOT
 * AN INFEASIBILITY and SpikeIsFeasible cannot tell them apart — Refuse() returns
 * bAnswered = false with lambda = 0, which is bit-for-bit what the dead-load infeasibility
 * arm returns. Without that assertion a region solve that hit NumericalFailure — the arm
 * that refused 128- and 200-block members one commit ago, against regions here of 44 to
 * 149 blocks — would read as "the free-boundary side is infeasible", which is the shape of
 * this file's headline finding. The verdict is not corrupted today (every region solve's
 * pivot count is 7-965, nowhere near MaxPivots, and every WhyNot is empty), and the point
 * of the assertion is that nothing was defending that.
 *
 * PINNED IN A WINDOW: lambda* of the live pose, at the file-wide +/-2e-5 relative, so a
 * moved lambda* fails HERE too and the cross-validation cannot quietly rest on a number
 * that changed.
 *
 * **NOT PINNED: WALL-CLOCK TIME.** Timings are not bit-reproducible; they measure the
 * machine. Every second below is REPORTED and none is asserted, with one exception stated
 * at its assertion: a single order-of-magnitude ceiling on the gate fixture, set ~30x above
 * the measurement so it can only fire on a catastrophe. Ratios of pivots ARE pinned,
 * because pivot counts are deterministic; ratios of seconds are not.
 *
 * COST: this file's three tests live in the OPT-IN sweep, and SINCE 2026-08-16 THAT SWEEP
 * HAS TWO TIERS — `OracleSweepFast` (7 tests, 80 s, for iteration) and `OracleSweepFull`
 * (3 tests, 20.8 min, MANDATORY before any commit touching the LP oracle). The cost test is
 * FULL; both sandwiches are FAST. `Automation RunTests OracleSweep` runs both by substring
 * (measured: the three filters return 7 / 3 / 10 tests).
 * MEASURED: 344 s for the cost test, **313 s since the 2026-08-16 live-pose trim**, with
 * 7.2 s for the sandwich and 9.6 s for the repaired one.
 * The group is 10 tests and 10 GREEN since the repaired sandwich landed (2026-08-15) — this
 * file adds NO deliberate red. It briefly did: the
 * early-exit seam was written here as a red hand-off and the seam landed inside the same
 * slice, so the two fields are now measured and pinned rather than demanded. The default
 * suite is untouched and re-run at 173 = 167 green + 6 deliberate reds.
 *
 * GREEN ON ARRIVAL IS NOT THE SAME AS ASSERTING NOTHING, and §9.5 is explicit that a row
 * which invalidated a published design section may not rest on an argued bite. So every
 * conclusion here has a RECORDED MUTATION, listed in TRAPS' registry as X1-X10:
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
 *
 * X5, X6 and X7 exist because the early-exit numbers, Part D's booleans and every region
 * verdict were the three places this file's conclusions rested on assertions that a
 * wrong-but-plausible answer would have walked straight through.
 *
 * NEEDS A TICKING WORLD: NO. Producers, the bridge and the LP — arithmetic on plain
 * structs, no world, no tick.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges many files into one translation
 * unit and every file-scope name in the blob shares it (TRAPS). Every constant here also
 * carries a Spike prefix for the same reason.
 */
namespace OracleFeasibilitySpikeSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace RigidBlockOracle;

	/* ================================================================================
	 * THE BRICK AND THE GRID — derived here, imported from nowhere, so a wrong production
	 * constant DISAGREES with this file instead of being echoed by it.
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
	 * How far outside an exact grid multiple a block may sit and still count as inside a
	 * radius. Half bats close a flush course at a half-cell offset, so a window written as
	 * an exact multiple would include or exclude them on a rounding; 1 cm is far below the
	 * 22.5 cm it discriminates and far above any arithmetic noise in a centroid.
	 */
	constexpr double SpikeRadiusSlackCm = 1.0;

	/** The file-wide relative window on a certified lambda*, per the sweep's discipline. */
	constexpr double SpikeLambdaRelativeWindow = 2.0e-5;

	/**
	 * "NOBODY HAS MEASURED THIS YET", for the one pin whose measured values include
	 * INDEX_NONE. Every other unmeasured pin in this file is INDEX_NONE and that is loud
	 * enough; PivotsToFirstFeasible reports INDEX_NONE as a genuine reading (an infeasible
	 * problem never reaches feasibility), so its unmeasured state needs a value the solver
	 * can never produce.
	 */
	constexpr int32 SpikeUnmeasured = -2;

	/* ================================================================================
	 * FIXTURE BUILDERS — production's own producers, transcribed rather than shared
	 * because the sweep file's helpers live in its own translation unit.
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
	 * WALL-01, SHRUNK — the same acceptance producer at two smaller integers, which is
	 * the fixture DestructionGame.Oracle.RigidBlock.PricingCost budgets at 8 x 10 = 84
	 * blocks. The gate is stated about "an 84-block region"; this is that wall.
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
	 * The leaning-stack acceptance fixture, transcribed from LeaningStackAcceptanceTest:
	 * course i at (10*i, 0, 3.25 + 7.5*i), base grounded, mortared through MakeInterface
	 * with a 1 cm bed. Carried here because it is the only cheap fixture the LP prices
	 * BELOW 1.0, which is the infeasible arm of the cross-validation.
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
		 * The early exit's two readings, carried so every fixture reports what stopping at
		 * feasibility would have saved rather than only the one a seam assertion happens to
		 * build. BOTH ARE PINNED PER ROW. They were diagnostic-only in the first draft, on
		 * the argument that the saving was an open question and not a contract — which is
		 * exactly backwards: the whole point of the row is that "refuted at 0.63%" is now a
		 * recorded CONCLUSION, and a conclusion nothing asserts is one a future pricing or
		 * tolerance change can invert while every test stays green.
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
	 * THE FEASIBILITY VERDICT, and it is deliberately read the same way production would
	 * have to read it: answered, and lambda at or above 1. In the dead pose lambda can only
	 * be LambdaCap (feasible) or exactly 0 (the oracle's dead-load infeasibility arm), so
	 * this is a boolean wearing a double's clothes — which the rows assert directly rather
	 * than trusting.
	 */
	bool SpikeIsFeasible(const FPoseReading& Reading)
	{
		return Reading.bAnswered && Reading.Lambda >= 1.0;
	}

	/* ================================================================================
	 * THE REGION EXTRACTOR — test-side, and one flag apart between the two boundary
	 * conditions so the sandwich's two sides cannot differ in anything else.
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
		 * WHO IS IN AND WHERE THEY WENT, published so the surcharge can be computed from
		 * the SAME membership the region was cut with. Recomputing "core or shell" beside
		 * the extractor would be a second derivation of the one thing the two sides of the
		 * sandwich must agree about, and a divergence there would look like physics.
		 */
		TArray<bool> bInRegion;

		/** Full-problem block index -> region block index; INDEX_NONE for an omitted block. */
		TArray<int32> Remap;
	};

	/**
	 * Cut a region out of a whole-structure problem.
	 *
	 * CORE is whatever the caller's mask selects. SHELL is every block outside the core
	 * that shares a joint with it — the region's boundary, and the only thing the two
	 * sides of the sandwich disagree about:
	 *
	 *   - bGroundTheShell TRUE  -> every shell block becomes the earth (OPTIMISTIC: the
	 *     region is granted an artificial foundation all round);
	 *   - bGroundTheShell FALSE -> shell blocks keep their own grounded flag, so they
	 *     write equilibrium rows and carry their own weight with nothing holding them
	 *     (PESSIMISTIC).
	 *
	 * Joints leaving the region are dropped, and a joint between two grounded blocks is
	 * dropped exactly as BuildRigidBlockProblem drops it: it constrains nothing.
	 *
	 * The output is posed DEAD — this extractor exists to ask feasibility questions.
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
	 * REPAIR (1) — THE OMITTED MATERIAL'S WEIGHT, KEPT AS A DEAD SURCHARGE.
	 *
	 * The refuted form dropped an omitted block's RESTRAINT and its WEIGHT together, and
	 * the weight was the half that mattered: the leaning stack's twenty-four omitted
	 * courses were carrying the destabilising moment, so deleting them turned a structure
	 * with no equilibrium into a short stack that comfortably stands (lambda* 18.481 against
	 * the whole stack's 0.44048 — wrong by 41.96x, PART D of the sandwich test).
	 *
	 * The repair keeps the demand and still refuses the restraint: an omitted block that
	 * BEARS ON THE REGION contributes its weight as a DEAD applied force, while writing no
	 * equilibrium row, no joint row, and holding nothing down.
	 *
	 * WHAT "BEARS ON THE REGION" MEANS, and getting this wrong in either direction is the
	 * whole slice. Charge too little and the false certificate survives; charge everything
	 * and a strip cut out of a wall is asked to carry the entire building, so the
	 * pessimistic side never certifies and the lever is dead by a second route.
	 *
	 *   - MATERIAL ABOVE, whose only road to the ground runs through the region: the region
	 *     genuinely carries it. CHARGED.
	 *   - MATERIAL TO THE SIDE (and below) that stands on its own foundation: it reaches the
	 *     earth without the region's help, so the region does not carry its weight.
	 *     NOT CHARGED. This is the case the leaning stack does not have and every wall does,
	 *     and it is why "the omitted weight" cannot mean "all of it".
	 *
	 * Stated as one rule with no geometry in it: DELETE THE REGION FROM THE JOINT GRAPH; an
	 * omitted block whose remaining connected component contains no grounded block is
	 * carried by the region, and every block in that component is charged. That is a
	 * reachability question on the graph, derived from nothing the LP knows.
	 *
	 * WHERE THE LOAD IS APPLIED, and this is the part the false certificate turns on. A
	 * component's weight is delivered at its interface joints, shared by AREA — but on the
	 * VERTICAL LINE THROUGH THE COMPONENT'S OWN CENTRE OF GRAVITY, not at the contact's X.
	 * A vertical force's moment does not depend on the Z of its application point
	 * (torque = Rx*Fz - Rz*Fx, and Fx = 0), so applying every share on one vertical line
	 * reproduces the component's total force AND its total moment about every point, exactly.
	 * Applying at the contact centre instead would throw away precisely the overturning
	 * moment that produced the 41.96x error — the stack's omitted courses weigh on a line
	 * at X = 175 cm while their contact with the band sits at X = 55.
	 *
	 * THE SURCHARGE GOES ON THE PESSIMISTIC SIDE ONLY. The optimistic side's soundness
	 * argument is "restrict a full-structure admissible force system to a grounded-shell
	 * region and it stays admissible" — a restriction satisfies no row that was not in the
	 * original problem, so adding a surcharge row to the optimistic side would break the one
	 * half of the sandwich that survived review.
	 *
	 * KNOWN SCOPE LIMIT, NAMED RATHER THAN HIDDEN: the rule is exact when omitted material
	 * is either wholly carried by the region or wholly self-supporting. It UNDER-CHARGES
	 * material that has its own ground path AND bears on the region — which is what a
	 * PARTIAL-HEIGHT strip in a laterally-connected wall produces, since the courses above
	 * its top still reach the ground down the columns either side. That is why the wall
	 * regions below are FULL HEIGHT by construction: full height is not a convenience, it is
	 * the condition under which this rule is defensible on a wall.
	 * ================================================================================ */

	enum class ESpikeSurcharge : uint8
	{
		/** The refuted form: omitted material contributes nothing at all. */
		None,

		/** Only what the region carries — the component-reaches-no-ground rule above. */
		Carried,

		/**
		 * Every omitted block, whatever it stands on. Deliberately TOO pessimistic, and
		 * carried as a control so "charging everything never certifies" is a measurement
		 * rather than the assertion of an argument.
		 */
		EveryOmittedBlock
	};

	struct FSurchargeCounts
	{
		int32 ChargedBlocks = 0;
		int32 ChargedComponents = 0;
		int32 InterfaceJoints = 0;

		/**
		 * A charged component sharing no joint with the region: its weight has nowhere to
		 * be delivered and is silently lost, which is the refuted form returning by the
		 * back door. Must be zero, and is asserted rather than assumed.
		 */
		int32 OrphanComponents = 0;

		double ChargedWeightUu = 0.0;
		double AppliedWeightUu = 0.0;

		/**
		 * Surcharge delivered onto a GROUNDED region block. A grounded block writes no
		 * equilibrium row, so the assembly discards such a force in silence — the same
		 * shape of loss as an orphan component and equally fatal to the repair. Counted so
		 * it can be asserted zero.
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
		 * Adjacency among OMITTED blocks only. The region is deleted for this question, so
		 * every joint with an end inside it is gone — which is what makes "can this block
		 * still reach the ground" mean "without the region's help".
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
	 * REPAIR (2) — REGIONS THAT REACH THE GROUND BY CONSTRUCTION.
	 *
	 * The free-boundary side is provably infeasible for any region containing no grounded
	 * block: sum its vertical equilibrium rows, every internal contact cancels pairwise,
	 * every leaving joint has been dropped, and what remains is 0 = -W_region. So the
	 * growth parameter cannot be a RADIUS around the deletion — a ball only reaches the
	 * foundation by accident, which is exactly why the refuted form certified five of ten
	 * deletions and those five were the low ones.
	 *
	 * The parameter becomes the WIDTH of a full-height strip: every block within
	 * (0.5 + w) cells of the deletion, at every height. The half cell is not slop — running
	 * bond offsets alternate courses by half a cell, so a window written in whole cells
	 * takes 2w+1 bricks from the aligned courses and NOTHING from the offset ones, and the
	 * "strip" would be a ladder of disconnected rungs. With the half cell the aligned
	 * courses give 2w+1 and the offset courses 2w+2, which is a connected strip at every w
	 * including w = 0.
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
	 * One fixture, both poses. Every pin is INDEX_NONE or negative until measured, and an
	 * unmeasured pin is an ERROR rather than a skip — that is this slice's red.
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
		 * THE EARLY EXIT, PINNED RATHER THAN PRINTED. How many of the dead pose's pivots
		 * phase 1 spent, and at which pivot the problem FIRST read feasible — the two
		 * numbers whose difference is the whole of what an early exit would buy.
		 */
		int32 DeadPhaseOnePivots = INDEX_NONE;

		/**
		 * INDEX_NONE is a MEASURED VALUE here and not a sentinel: an infeasible problem
		 * never reaches feasibility and must report exactly that, which is what the
		 * 30-course stack's row pins. So "unmeasured" needs its own value, and it is
		 * SpikeUnmeasured (-2) — the one place in this file where the usual INDEX_NONE
		 * convention would have made a real reading indistinguishable from a missing one.
		 */
		int32 DeadFirstFeasible = SpikeUnmeasured;

		/** 1 feasible at lambda = 1, 0 infeasible. INDEX_NONE is unmeasured. */
		int32 Feasible = INDEX_NONE;

		/*
		 * WHETHER THIS ROW RE-SOLVES THE GRAVITY-LIVE POSE, and the four rows that do not
		 * are the point of the 2026-08-16 tier split. corbel D, wall-06, wall-15 and
		 * wall-18 have their lambda* pinned — same producer, same cut, same bridge, same
		 * solver, so bit-for-bit the same solve — in OracleSweepFull.RigidBlock.
		 * WallsAndLadders, which runs in the SAME TIER as this test. Re-solving them here
		 * bought a lambda* window that already exists and ~33 s per run.
		 *
		 * WHAT IS DELIBERATELY KEPT LIVE, and each for a reason that is not duplication:
		 * wall-01 (the ONLY pinned wall-01 lambda* in the project — the sweep excludes the
		 * 30-course walls outright — and the 10.88x headline), the 84-block gate fixture
		 * (the gate arithmetic and the 5.31x it rests on), and BOTH leaning stacks (the
		 * 30-course one is the infeasible arm of the cross-validation, without which the
		 * boolean is vacuous, and both are microseconds).
		 *
		 * WHAT IT COSTS, stated rather than glossed: on those four rows the lambda* window,
		 * the LIVE pivot pin, the within-run cross-validation and the LIVE 0/0 phase-1 pins
		 * all go, so their pivot SPEEDUP ratio becomes a cross-run comparison against
		 * WallsAndLadders instead of a within-run one. The DEAD pose is untouched on every
		 * row — which is why the pooled early-exit totals (9,245 and 58), the eight-point
		 * block spread behind "the lever weakens with scale", and every feasibility verdict
		 * come back bit-identical.
		 */
		bool bSolveLivePose = true;
	};

	/**
	 * The sandwich's pins, as struct members rather than as local constants: a pin the
	 * compiler can fold turns its own unmeasured branch into dead code, and dead code is
	 * how a measurement quietly stops being taken.
	 */
	struct FSandwichPins
	{
		int32 LadderBlocks = 149;
		int32 GlobalFeasible = 1;
		int32 GlobalPivots = 965;

		/*
		 * MEASURED 2026-08-15. The brick comes out of course 6 of a 12-course wall, and the
		 * sandwich first closes at radius 5 — the first radius whose region reaches the
		 * foundation, exactly as the conservation lemma predicts and NOT at the design's
		 * "small radius". The region at that radius is 142 of the wall's 149 blocks.
		 */
		int32 ClosingRadius = 5;
		int32 ClosingRegionBlocks = 142;
		int32 ClosingOptimisticPivots = 753;
		int32 ClosingPessimisticPivots = 872;

		/*
		 * R2's ANSWER. Ten deletions, courses 1..10, radius fixed at 4: FIVE certify.
		 * Revision 1 predicted four — the miss is the SHELL, which reaches one course
		 * further than the core, so a radius-4 box around a course-5 brick touches the
		 * foundation. Prediction wrong by exactly one course, for a named reason.
		 */
		int32 Agreements = 5;

		/*
		 * And the collapse arm: the 30-course leaning stack's OPTIMISTIC region is still
		 * feasible at radius 13 (29 of 30 blocks) and only becomes infeasible at radius 14
		 * — where the region is all 30 blocks, i.e. THE WHOLE STRUCTURE.
		 */
		int32 StackCertifyingRadius = 14;
		int32 StackCertifyingRegionBlocks = 30;

		/*
		 * PART D's BAND. Courses 0..4 core plus course 5 as shell, so six blocks and five
		 * joints out of the 30-course stack.
		 */
		int32 BandBlocks = 6;

		/*
		 * AND HOW WRONG THE FALSE CERTIFICATE IS, as the band's own lambda* in the LIVE
		 * pose, in the file-wide +/-2e-5 relative window. Negative is UNMEASURED and is a
		 * red. Never scaled from another fixture — measured on this exact region.
		 *
		 * MEASURED 2026-08-15: **18.481256459924058**, 5 pivots. The whole stack is
		 * 0.44048, so the sandwich certified as feasible a region standing at **41.96x**
		 * the load under which the structure it certified has no equilibrium. Review's
		 * independent estimate — a (N-1)^-2.29 fit to the pinned stack rungs — predicted
		 * ~18.7 and lands 1.2% off, which is a second derivation agreeing rather than a
		 * transcription: the fit knows nothing about how the extractor cuts a region.
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
	 * THE REPAIRED SANDWICH'S PINS. Every one is INDEX_NONE or negative until measured, and
	 * an unmeasured pin is an ERROR rather than a skip — that is this slice's red, and it is
	 * the same red every pinned measurement in this file arrived through.
	 */
	struct FRepairPins
	{
		/* The refuted form's own fixture, pinned again so the two are the same wall. */
		int32 WallBlocks = 149;
		int32 WallGlobalPivots = 965;

		/*
		 * PREDICTED (P3, derivation record revision 1, written before the first run): the
		 * strip closes at half-width 0 — a two-cell zig-zag column — because running bond
		 * gives the course above a one-brick hole a bearing on BOTH sides inside the strip,
		 * and GP mortar's 0.70 MPa of bond is ample for it. Predicted region 41 of 149
		 * blocks (17 core + 24 shell), 28%, against the refuted form's 142 and 95%.
		 *
		 * MEASURED 2026-08-15: half-width 0, 41 blocks, 27.5%. HIT, block for block.
		 *
		 * P5 predicted a cost of 0.3-0.6x the global solve with a point estimate of 0.48x.
		 * MEASURED 49 + 262 = 311 against 965 — 0.322x. Inside the range, and the point
		 * estimate MISSED by a third for a reason worth keeping: the OPTIMISTIC side is
		 * nearly free (49 pivots for 41 blocks) because grounding the shell turns most of
		 * the region's boundary into earth, and a grounded block writes no equilibrium row
		 * at all. The prediction priced both sides as if they had the same row count.
		 */
		int32 ClosingHalfWidth = 0;
		int32 ClosingRegionBlocks = 41;
		int32 ClosingOptimisticPivots = 49;
		int32 ClosingPessimisticPivots = 262;

		/*
		 * PREDICTED (P4): TEN of ten deletions certify, against the refuted form's five. The
		 * five that failed there were the five whose ball did not happen to reach the
		 * foundation, and a ground-anchored strip reaches it by construction at every
		 * deletion height, so the deletion's course should stop mattering entirely.
		 *
		 * MEASURED: 10. HIT, and the per-course lines show why — the region is 39 or 41
		 * blocks at every course, alternating only with the deleted brick's bond parity.
		 *
		 * Named Agreements and not Certificates deliberately: the test also keeps a running
		 * count of every closed sandwich in the whole file (20), and two counters called the
		 * same thing at two different scopes is how a comparison comes to read plausibly and
		 * mean nothing.
		 */
		int32 Agreements = 10;

		/*
		 * PREDICTED (P8): the over-inclusive control (charge EVERY omitted block, not only
		 * what the region carries) reads INFEASIBLE at the closing width, i.e. 0 — a strip
		 * cut out of a wall cannot carry the whole building.
		 *
		 * MEASURED 1: FEASIBLE. **P8 IS A MISS, and it is the most useful miss in the run.**
		 * Charging all 108 omitted blocks — 271,309.925 uu on a 41-block strip — still
		 * leaves it standing. The arithmetic says why: the whole wall weighs ~4 kN, the
		 * surcharge is 2.7 kN of it, spread by area over 44 interface joints up the full
		 * height of the strip, and every one of those joints has a mortar bond of 0.70 MPa
		 * over ~110-220 cm². The wall arm has so much margin that IT CANNOT DISCRIMINATE
		 * SURCHARGE RULES AT ALL. Two consequences, both recorded rather than smoothed over:
		 * the "too pessimistic to certify" failure mode is NOT demonstrated by this fixture
		 * and remains an untested hypothesis; and the closing width measured above would be
		 * the same under either rule, so the wall arm is evidence about REPAIR (2) only.
		 */
		int32 ControlPessimistic = 1;

		/*
		 * PREDICTED (P7): a ground-anchored region's size scales with the STRUCTURE'S
		 * HEIGHT, because reaching the foundation is what makes it certifiable at all. Same
		 * wall 18 courses instead of 12: predicted the same closing half-width 0 and a
		 * region of ~62 blocks, 1.5x the 12-course 41 for 1.5x the height.
		 *
		 * MEASURED: 224 blocks, closes at half-width 0, region 62 — 1.512x for 1.5x of
		 * height, and 27.7% against 27.5%. HIT on the number and on the mechanism. The
		 * fraction is height-invariant and the COUNT is not, which is the finding that
		 * decides scenario scale: a 30-course wall's certified strip is ~102 blocks
		 * (ESTIMATE, linear in height off two points), i.e. larger than the 84-block
		 * fixture that answers in 0.0666 s.
		 *
		 * AND ITS TWO PIVOT COUNTS ARE PINNED, because they are the INPUT to that
		 * extrapolation and not decoration: 79 + 474 = 553 against the 12-course
		 * 49 + 262 = 311 is the 1.78x-per-1.5x-of-height slope the whole scenario-scale
		 * answer rests on. Pinning the region's SIZE and leaving its COST unpinned would let
		 * the number the conclusion is made of rot while every verdict stayed green.
		 */
		int32 TallWallBlocks = 224;
		int32 TallClosingHalfWidth = 0;
		int32 TallClosingRegionBlocks = 62;
		int32 TallClosingOptimisticPivots = 79;
		int32 TallClosingPessimisticPivots = 474;

		/*
		 * THE COLLAPSE ARM. PREDICTED (P9): the repaired pessimistic side is INFEASIBLE at
		 * every band short of the whole stack (the surcharge restores the destabilising
		 * moment the refuted form deleted), and the optimistic side is untouched by the
		 * repair and still only goes infeasible with all thirty blocks. So the sandwich
		 * closes at the WHOLE STRUCTURE and nowhere below it: the repair converts a FALSE
		 * CERTIFICATE into a refusal to certify, and buys the collapse arm nothing.
		 *
		 * MEASURED: exactly that. Bands 0..5, 0..10, 0..15, 0..20 and 0..25 all read
		 * optimistic FEASIBLE / pessimistic INFEASIBLE — no closure, grow the region — and
		 * only 0..29, all thirty blocks, closes. HIT.
		 */
		int32 StackClosingTopCourse = 29;
		int32 StackClosingRegionBlocks = 30;

		/*
		 * AND WHAT THE COLLAPSE ARM COSTS, which the first draft of this file measured and
		 * did not report. At closure the two solves are 163 + 163 = 326 against a 163-pivot
		 * global solve — **2.0x** — and the whole growth ladder from 0..5 upward is 1,300
		 * pivots, **8x**. The standing arm's 0.322x and this 2.0x are the same measurement's
		 * two halves and neither may be quoted alone: 2.0x is WORSE than the 1.68x the
		 * refuted form was condemned for, and it lands on the arm a player's deletion cares
		 * about. Pinned so the ratio is a contract rather than a sentence in a header.
		 */
		int32 StackClosingOptimisticPivots = 163;
		int32 StackClosingPessimisticPivots = 163;
		int32 StackLadderPivots = 1300;

		/*
		 * And the surcharge's own reading on the arm where it does the work: the band
		 * 0..5 is 7 blocks (six core courses plus one shell) and charges the 23 courses
		 * above it. Pinned because a reachability walk that quietly charged the wrong SET
		 * would still produce an infeasible band and every verdict above would stay green.
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
}

/* ====================================================================================
 * TEST 1 — THE FEASIBILITY REFORMULATION'S COST.
 *
 * COST OF THIS TEST: the lambda* pose is almost all of it — wall-01 alone is 280 s.
 * MEASURED 2026-08-15 in the whole group: **344 s (5 min 44 s)**, of which the cheap half
 * of the answer (the DEAD poses, which are what the slice measures) is 53 s. It lives in
 * the OPT-IN group's FULL tier for exactly that reason.
 *
 * THE TRIM IS TAKEN (2026-08-16), AND ONE ROW IS EXPLICITLY EXCLUDED FROM IT. Four rows —
 * corbel D, wall-06, wall-15 and wall-18 — no longer re-solve the gravity-live pose: it is
 * the same producer, the same cut, the same bridge and the same solver as
 * `OracleSweepFull.RigidBlock.WallsAndLadders`, which pins each of those four lambda* and
 * runs in the SAME TIER, so the solve was duplicated work and its window a duplicated pin.
 * MEASURED: 344 s -> 313 s, and WallsAndLadders' own log lines confirm the duplication was
 * exact (corbel D 9,490 pivots, wall-18 3,885, wall-15 6,076 — the very counts this test
 * used to pin). WALL-01 IS EXCLUDED AND STAYS LIVE: `RigidBlockOracleSweepTest` excludes the
 * 30-course walls (cases 1-5) outright on arithmetic, so THIS FILE HOLDS THE ONLY PINNED
 * WALL-01 lambda* IN THE PROJECT and dropping its live re-solve would delete that anchor
 * rather than relocate it. The gate fixture and both leaning stacks stay live too — see
 * FCostRow::bSolveLivePose for each reason and for what the trim costs.
 *
 * WHAT THE TRIM DID NOT TOUCH, and it is the reason it was worth taking: every DEAD solve.
 * All eight fixtures still measure the reformulation, so the pooled early-exit totals
 * (9,245 and 58), the eight-point block spread from 5 to 375 behind "the lever weakens
 * with scale", the gate arithmetic and every feasibility verdict are bit-identical.
 *
 * NEEDS A TICKING WORLD: NO.
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
	 * EVERY ROW CARRIES ITS PREDICTION AND ITS MEASUREMENT, side by side and permanently:
	 * a prediction deleted once it is checked leaves nobody able to tell an agreement from
	 * a transcription. Measured 2026-08-15, one run, on the tree at HEAD 9dcbd76.
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
	 * THE EARLY EXIT'S POOLED TOTALS, accumulated across the rows and pinned after the
	 * loop. The per-row pins already fix these two sums arithmetically, and that is the
	 * point of having them as well: the pooled 0.63% is the sentence CURRENT_STATE and
	 * PROMOTION_DESIGN §5.2 both record, and a conclusion that appears in the documents
	 * should appear in the suite in the same shape rather than only as ingredients.
	 */
	int32 PooledPhaseOnePivots = 0;
	int32 PooledSkippedPivots = 0;

	for (const FCostRow& Row : Rows)
	{
		FStructure Structure;
		FString Why;

		/*
		 * Each call is its OWN statement before its message is built: folding a call that
		 * writes Why into the Printf that reads it is unsequenced, and MSVC evaluates the
		 * condition last (TRAPS).
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
		 * THE TWO POSES ARE THE SAME PROBLEM AND ONE FLAG. Copying rather than rebuilding
		 * is the point: if the dead pose were assembled from a separately-built structure
		 * the comparison would be between two problems, and a difference in the producer
		 * would read as a difference in the reformulation.
		 */
		FOracleProblem Dead = Live;
		Dead.bGravityIsLive = false;

		/*
		 * AND THE LIVE POSE IS SOLVED ONLY WHERE ITS ANSWER IS NOT ALREADY PINNED
		 * ELSEWHERE IN THE SAME TIER — see FCostRow::bSolveLivePose for which four rows
		 * that is and what dropping them costs. A skipped row leaves LiveRead at its
		 * default, which is bAnswered = false: every reader of it below is inside the same
		 * flag, and the log line says NOT RE-SOLVED rather than printing a zero that reads
		 * like a refusal.
		 */
		const FPoseReading LiveRead = Row.bSolveLivePose ? SpikeSolve(Live) : FPoseReading();
		const FPoseReading DeadRead = SpikeSolve(Dead);

		/*
		 * The early-exit saving, printed per row and pinned nowhere: the fraction of phase
		 * 1's pivots spent AFTER the problem first read feasible, which is what stopping
		 * there would have skipped. Negative readings mean the fixture never reached
		 * feasibility (the infeasible arm), and the fraction is reported as -1 there rather
		 * than as a plausible zero.
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
		 * THE POSE IS DEAD, AND THIS IS HOW A TEST KNOWS IT. With no live load the lambda
		 * column appears in no equilibrium row and its only constraint is the cap, so a
		 * feasible problem MUST report exactly LambdaCap and an infeasible one exactly 0.
		 * Anything between means a live load survived into a problem posed as dead, and
		 * every pivot count and every second measured on it would be measuring something
		 * else.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s: a DEAD pose carries no live load, so lambda must be exactly ")
				TEXT("LambdaCap (%.17g) or exactly 0, and was %.17g"),
				Row.Name, LambdaCap, DeadRead.Lambda),
			DeadRead.Lambda == LambdaCap || DeadRead.Lambda == 0.0);

		/*
		 * THE CROSS-VALIDATION. The reformulation is only worth measuring if it means what
		 * it should: an admissible equilibrium at lambda = 1 exists exactly when the
		 * lambda* pose reads at or above 1. Two different formulations, one answer.
		 *
		 * IT IS A WITHIN-RUN COMPARISON OR IT IS NOTHING, which is why it is inside the
		 * live-pose flag rather than run against a transcribed lambda*: comparing a
		 * measured boolean to a number typed into this file would assert the typing. The
		 * four rows that keep it span both arms — the 30-course stack is lambda* = 0.44 and
		 * must come back INFEASIBLE, without which the boolean is vacuous.
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
		 * THE TWO PIVOT PINS ARE SEPARATE GUARDS, not one. They were a single branch while
		 * every row solved both poses; a row that does not re-solve the live pose has no
		 * live pivot count to pin, and folding the two together would have made the DEAD
		 * pin — the reformulation's whole cost measurement — vanish with it.
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
		 * WHY THESE ARE EXACT PINS AND NOT A RANGE. Both are deterministic integers on a
		 * fixture that is being solved anyway, so they cost nothing, and the number they
		 * decide is a published conclusion: "0.63% of phase-1 pivots happen after the
		 * problem first reads feasible", which is what refuted §5.2's largest remaining
		 * claim. An assertion that checked only population and ordering (0 <= first <=
		 * phase1) passes on a mutation reporting a wrong-but-in-range index — and the
		 * failure that matters is exactly that shape: move wall-18's 449 to 40 and the
		 * lever becomes worth 91%, the conclusion inverts, and nothing goes red.
		 *
		 * The prediction of record is the DESIGN's qualitative "most of the saving lives
		 * here" (§3.2/§5.2); revision 1 of this file's derivation record could not price it
		 * because the fields were unpopulated. The header's MEASUREMENT 1b table carries
		 * both halves side by side.
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
			 * The ordering property is kept BESIDE the values rather than instead of them.
			 * It is the one thing the values cannot say: that the two fields are consistent
			 * with each other whatever they read.
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
		 * AND THE LIVE POSE MUST REPORT 0/0 ON EVERY ROW THAT SOLVES ONE. A gravity-live
		 * problem has every
		 * equality right-hand side at zero, so it is feasible before a single pivot and
		 * phase 1 never runs — the solver's own comment says so and this is what checks it
		 * from the outside. It is also a second, independent guard on the pose itself: a
		 * live load leaking into a problem posed as dead would put a non-zero phase 1 here.
		 * Four rows no longer re-solve the live pose (bSolveLivePose), so the guard now
		 * stands on four fixtures from 5 to 375 blocks rather than eight.
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
	 * THE EARLY EXIT'S HEADLINE, ASSERTED AS ONE NUMBER.
	 *
	 * 58 pivots out of 9,245 pooled — 0.63% — is the figure §5.2 and CURRENT_STATE both
	 * record as refuting the lever. It is pinned here as a sum so that the CONCLUSION has
	 * an assertion of its own and not merely eight ingredients: a future change that moves
	 * two rows in opposite directions would leave the per-row pins red in a way that reads
	 * like noise, where this line says what actually changed.
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
	 * THE GATE ARITHMETIC, WRITTEN OUT WHERE IT IS MEASURED.
	 *
	 * PROMOTION_DESIGN §5.3 states the target: "A region of that size needs to answer in
	 * ~50 ms to fit two solves inside a 100 ms budget: 12x needed", against the 0.61 s the
	 * 84-block wall cost as a lambda* solve.
	 *
	 * MEASURED 2026-08-15: the same 84 blocks answer the FEASIBILITY question in 0.065 s.
	 * So the design's 12x is REFUTED as the size of the gap — the measured gap is
	 *
	 *     0.065 s / 0.050 s = 1.3x
	 *
	 * with the reformulation alone and no warm start of any kind. Two solves are 0.130 s
	 * against a 0.100 s budget: NOT MET TODAY, and short by a factor smaller than any
	 * single unmeasured lever in §5.4/§5.5. The kill criterion is conditioned on "cannot be
	 * brought under ~50 ms by the feasibility reformulation PLUS WARM STARTS", and warm
	 * starts are unbuilt and unmeasured, so this measurement does not fire it.
	 *
	 * WHAT DOES NOT FOLLOW, and must not be read into the number: this says nothing about
	 * whether a REGION of 84 blocks is a legitimate unit of work. The second test in this
	 * file measures that separately and the answer there is much worse.
	 *
	 * NO TIMING IS PINNED AS A NUMBER — timings measure the machine. The single assertion
	 * below is an ORDER-OF-MAGNITUDE ceiling ~30x above the measurement, which can only
	 * fire on a catastrophe (a solver change that puts the gate fixture back into seconds)
	 * and cannot flake on a busy machine.
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
	 * THE EARLY EXIT — THE SEAM, RE-SOLVED, AND WHY THIS BLOCK SURVIVED ITS OWN RED.
	 *
	 * PROMOTION_DESIGN §3.2 and §5.2: a feasible problem is PROVED the moment phase 1's
	 * infeasibility sum reaches tolerance, with no optimality proof needed, and that was
	 * supposed to be where most of a standing structure's saving lives. It could not be
	 * measured from outside the solver, so this block was written as this slice's ONE
	 * DELIBERATE RED, asserting only that two unset fields became populated:
	 *
	 *     PhaseOnePivots        how many of SimplexIterations phase 1 spent
	 *     PivotsToFirstFeasible the pivot at which the basic-artificial infeasibility sum
	 *                           first fell to InfeasibilityTolerance()
	 *
	 * The seam landed inside the same slice, so the red is gone and the population
	 * assertion with it — population is now the weakest of the things the rows above pin.
	 * What is left here is the property the per-row pins CANNOT state, and it is worth its
	 * 0.065 s: the SAME problem solved a SECOND time reports the SAME two numbers. The
	 * fields are read out of solver state that no other field depends on, so an
	 * accumulation left un-reset between solves would show up here and nowhere else.
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
 * COST OF THIS TEST: one global feasibility solve plus two region solves per radius, THREE
 * per deletion for the ten-deletion agreement ladder (the third is the whole cut wall, so
 * that section's five certificates are checked against something rather than counted), the
 * leaning-stack arms, and Part D's band in both poses. MEASURED 2026-08-15: **7.2 s**, of
 * which the ten per-deletion global solves are about half — the whole regional question
 * still costs less than one lambda* solve of a mid-size wall, which is itself part of the
 * answer.
 *
 * NEEDS A TICKING WORLD: NO.
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
	 * A REFUSAL IS NOT AN INFEASIBILITY, AND NOTHING ELSE HERE CAN TELL THEM APART.
	 * SpikeIsFeasible reads "answered AND lambda >= 1", and Refuse() returns bAnswered =
	 * false with lambda = 0 — bit-for-bit what the dead-load infeasibility arm returns. So
	 * a region solve that REFUSED reads as "this region has no admissible equilibrium",
	 * which is precisely the finding this test reports, and every verdict pin, the
	 * conservation lemma, the closing radius and the 5-of-10 count would all be computed
	 * from it and all stay green.
	 *
	 * That is not a hypothetical arm: NumericalFailure from the periodic refactorisation
	 * refused 128- and 200-block members ONE COMMIT AGO, and the regions here are 44 to
	 * 149 blocks. Today's readings are clean (pivot counts 7-965, nowhere near MaxPivots,
	 * every WhyNot empty), so this guard defends a verdict that is currently correct — it
	 * is the defence that was missing, not the verdict.
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
	 * 2/4/8 IS THE DESIGN'S LADDER AND 5/6 ARE ADDED BECAUSE IT SKIPS THE ANSWER. On a
	 * 12-course wall a box of radius 8 around a mid-height brick IS the whole wall, so
	 * "closes at 8" would be indistinguishable from "closes only at the whole structure".
	 * The brick comes out of course 6 and the region's SHELL reaches one course further
	 * than its core, so the first radius whose region touches the foundation is 5 — which
	 * is exactly where the conservation lemma below says the free side can first certify.
	 *
	 * MEASURED 2026-08-15, and the size column is the finding: the region that finally
	 * closes is 142 of the wall's 149 blocks.
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
		 * AND HOW BIG THE REGION IS WHEN IT FINALLY CLOSES, which is the whole point of
		 * a decomposition lever. A radius pinned without its block count says nothing
		 * about whether the region is a neighbourhood or the building.
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
		 * THE COST COMPARISON, IN PIVOTS BECAUSE PIVOTS ARE DETERMINISTIC AND SECONDS ARE
		 * NOT. The sandwich buys locality with TWO solves; at the radius where it finally
		 * certifies, those two solves cost 753 + 872 = 1,625 pivots against the 965 that
		 * simply solving the whole wall costs. So on this fixture the lever is 1.68x MORE
		 * expensive than the thing it exists to avoid, and the assertion is written as a
		 * strict inequality rather than as a ratio so it states the finding rather than a
		 * measurement's last digits.
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
	 * PART B — R2's QUESTION: TEN DELETIONS, RADIUS FIXED AT 4.
	 *
	 * "If the free-boundary side is so pessimistic it never certifies, the sandwich
	 * degenerates to 'always grow to the whole structure' and the lever is worthless."
	 * This is the count that answers it. The deletions are a FIXED LADDER over courses,
	 * not a random sample: same ten bricks every run, each one printed, so a disagreeing
	 * row can be lifted straight out into a named regression fixture.
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
		 * THE WHOLE CUT WALL, SOLVED PER DELETION, because a certificate is only worth
		 * anything against the answer it certifies. Part A checks its closed sandwich
		 * against the global verdict and Part B did not — which left the five certificates
		 * this section counts as certificates of nothing. It costs one 149-block dead solve
		 * per course, and this test's whole budget is seconds.
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
		 * AND WHERE THIS DELETION'S SANDWICH CLOSES, WHAT IT CERTIFIES MUST BE TRUE. Same
		 * shape as Part A's line and the same deliberate scope: this is a characterisation
		 * of these ten fixtures, not the law "closure implies correctness" — Part D
		 * measures a closed sandwich whose certificate is wrong by ~40x. If one of these
		 * five ever certifies the wrong verdict, this is the line that says so, and it
		 * would be the second demonstrated false certificate rather than a test failure.
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
	 * PART C — THE OTHER ARM: A STRUCTURE THAT GENUINELY HAS NO EQUILIBRIUM.
	 *
	 * Every wall fixture the project owns is LP-feasible, so Part A and Part B can only
	 * ever exercise the sandwich's STANDING side. The 30-course leaning stack is the one
	 * cheap fixture the LP prices below 1.0 (lambda* 0.4405), and it is a chain, so
	 * "radius in courses" is exact rather than a box. What is measured is the smallest
	 * radius at which the OPTIMISTIC side — boundary grounded, the region granted an
	 * artificial foundation at both ends — is still infeasible, which is the radius at
	 * which a region can certify a COLLAPSE.
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
				 * The certifying radius is read off the OPTIMISTIC side going infeasible,
				 * so a refusal here would manufacture a certificate out of a solve that
				 * failed — the single most misleading direction available in this test.
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
				 * AND THE REGION AT THAT RADIUS IS THE WHOLE STRUCTURE — all 30 blocks. The
				 * optimistic side is still FEASIBLE at radius 13, where the region is 29 of
				 * 30, so on this fixture no proper subset of the structure can certify the
				 * collapse. That is the sharpest single statement about the lever: for a
				 * global mechanism there is no smaller region, by measurement.
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
			 * PART D — THE FREE BOUNDARY IS NOT PESSIMISTIC, AND A CLOSED SANDWICH CAN
			 * CERTIFY THE WRONG ANSWER.
			 *
			 * PROMOTION_DESIGN §5.3 justifies the free-boundary side like this: "if this
			 * problem is feasible, the full structure is feasible, because the found force
			 * system extends." IT DOES NOT EXTEND, and the reason is one sentence:
			 * dropping the material outside the region removes its RESTRAINT (pessimistic)
			 * and its WEIGHT (optimistic) together, so the free-boundary region is not a
			 * bound in either direction.
			 *
			 * The cheapest structure that shows it is the 30-course leaning stack, which
			 * has NO admissible equilibrium as a whole (lambda* = 0.4405). Take its BOTTOM
			 * BAND — courses 0..5, grounded, the twenty-four courses above simply dropped:
			 *
			 *   - free boundary: a short leaning stack on the ground, comfortably feasible
			 *     (the 5-course fixture reads lambda* = 31.2);
			 *   - grounded boundary: the same band with its one shell course pinned, which
			 *     can only be MORE feasible.
			 *
			 * Both feasible, so THE SANDWICH CLOSES and the design's rule certifies "the
			 * full structure is feasible". The full structure is not.
			 *
			 * ASSERTED AS THE THREE MEASURED FACTS, not as a failure: the band is feasible
			 * both ways and the whole stack is not. That trio IS the refutation and it is
			 * green because it is true.
			 *
			 * BUT A BOOLEAN TRIO ONLY SAYS "THE CERTIFICATE IS WRONG", AND THIS IS THE ROW
			 * THAT INVALIDATED A PUBLISHED DESIGN SECTION — §9.5 is explicit that such a row
			 * may not rest on an argued bite. Two things were added for that:
			 *
			 *   - THE BAND'S OWN lambda*, measured in the LIVE pose and pinned in the
			 *     file-wide window. It turns "wrong" into "wrong by a factor", and the
			 *     factor is what says no tightening of the extractor rescues the lever: the
			 *     certified band is not marginally feasible while the stack marginally
			 *     falls, it is comfortably feasible while the stack falls. MEASURED BELOW —
			 *     never scaled from the 5-course fixture's 31.2, because the band is a
			 *     6-block region cut out of a 30-course problem and only the solve knows
			 *     what that costs.
			 *   - X6 in the mutation registry: an extractor that keeps the out-of-region
			 *     blocks instead of dropping them. That is the repair's direction in its
			 *     crudest form, and it must turn the band infeasible.
			 *
			 * RE-AIMED 2026-08-15, WHEN THE REPAIR LANDED, AND HOW TO READ IT NOW. The rows
			 * above still measure the UNREPAIRED extractor, because the 41.96x false
			 * certificate is the evidence that no tightening of a boundary rule could have
			 * rescued the old form, and it must stay executable rather than become a sentence
			 * in a header. The rows BELOW measure the REPAIRED extractor on the IDENTICAL band
			 * and carry the honest post-repair claim: the repaired band reads INFEASIBLE and
			 * therefore AGREES with the whole stack, so the sandwich no longer closes on a
			 * proper subset and no certificate is issued at all. Neither half may be tuned to
			 * restore the other's green.
			 *
			 * THE FORWARD HAZARD BELOW CAME HALF TRUE, AND THE HALF THAT DID NOT IS THE MORE
			 * USEFUL LESSON. Its PHYSICS was right to the digit: the repaired band is
			 * infeasible, so "the bottom band must be FEASIBLE" is a false statement about
			 * the repaired extractor. Its claim about the SUITE was wrong. It promised this
			 * pin would go "red BY CONSTRUCTION on the day the repair lands" and would be
			 * "the only thing in the suite that would notice"; in fact the repair landed as a
			 * NEW function — SpikeExtractRepairedRegion — beside an untouched
			 * SpikeExtractRegion, so PART D went on calling the old one and stayed GREEN
			 * throughout. Nothing in the suite noticed. The re-aim above was made deliberately
			 * rather than forced by a failure.
			 *
			 * Generalised, because it will happen again and it is not specific to regions:
			 * A PIN CAN ONLY NOTICE A CHANGE THAT GOES THROUGH THE CODE IT CALLS, AND A
			 * REPAIR IMPLEMENTED BESIDE THE THING IT REPAIRS IS INVISIBLE TO EVERY PIN ON THE
			 * ORIGINAL. A forward hazard that names a behaviour is sound; one that predicts a
			 * specific test will fail is a prediction about an implementation nobody has
			 * chosen yet, and it should say so.
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
				 * AND THE SAME FREE-BOUNDARY BAND POSED LIVE, which is the only way to ask
				 * HOW feasible the false certificate's region is. One flag apart from the
				 * problem two lines above, for the same reason the cost test copies rather
				 * than rebuilds: a separately-extracted region would compare two problems.
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
				 * HOW WRONG THE CERTIFICATE IS, AS A NUMBER. The band certifies "the full
				 * structure is feasible" about a stack whose lambda* is 0.4405; the band's
				 * own lambda* says by how much. A boolean trio would survive almost any
				 * perturbation of the fixture — it says only that the certificate is on the
				 * wrong side of 1.0. This says the region is nowhere near the boundary, so
				 * the failure is not a knife edge that a tighter extractor could tune away.
				 *
				 * PINNED IN THE FILE-WIDE +/-2e-5 RELATIVE WINDOW, and MEASURED rather than
				 * derived: the 5-course free-standing fixture reads 31.2, and this band is a
				 * different problem — six blocks with the top course free of any load it
				 * used to carry.
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
				 * One thing changes and it is repair (1) — the twenty-four omitted
				 * courses' weight is applied as a DEAD surcharge on the vertical line
				 * through their own centre of gravity, while they keep contributing no
				 * restraint whatever (no equilibrium row, no joint row, nothing to hang
				 * from). The band's blocks, joints and boundary condition are identical
				 * to the free-boundary problem above, so a difference in the answer can
				 * only be the surcharge.
				 *
				 * WHAT MUST NOW BE TRUE, and it is the claim the whole repair exists to
				 * make: the repaired band AGREES with the whole stack. The certificate
				 * that was wrong by 41.96x is not merely tightened, it is withdrawn —
				 * the sandwich's two sides disagree (optimistic feasible, pessimistic
				 * infeasible), which under the rule means GROW THE REGION rather than
				 * certify. A repair that left this band feasible would have changed the
				 * arithmetic and not the answer.
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
					 * The omitted courses' weight, derived here from the brick mass and
					 * the count rather than read off the extractor, so a surcharge that
					 * charges the wrong SET cannot agree with a total it computed itself.
					 */
					const double ExpectedChargedUu =
						24.0 * SpikeBrickMassKg * OracleGravityCmPerSecondSquared;

					/*
					 * AND WHERE IT ACTS, DERIVED A SECOND TIME FROM THE FIXTURE BUILDER'S OWN
					 * EXPRESSION. The placement is the load-bearing half of repair (1) — the
					 * force is the easy part and the MOMENT is what the refuted form lost —
					 * and until this pin existed its only defence was a mutation recorded in
					 * a markdown file. That is not enough: mutation X8 moves the load to the
					 * CONTACT's X (~55 cm) and flips the band's verdict, but a wrong-but-
					 * nearer X — the region block's centroid, the charged component's lowest
					 * block — might not flip anything and would pass in silence.
					 *
					 * So the expected line of action is recomputed here from
					 * SpikeBuildLeaningStack's own "double(Course) * 10.0", summed over the
					 * charged courses, rather than transcribed off the extractor's weighted
					 * mean. Two derivations, one number: 175.0 cm, which is 120 cm outboard
					 * of the joint the band meets the omitted stack at (X = 55).
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
 * WHAT WAS REFUTED, so nothing here rebuilds it. The two-sided sandwich as designed —
 * solve a region with its boundary pinned grounded, solve it again with the boundary free,
 * certify if the two agree — fails three ways. It closes only at 95-100% of the structure;
 * it costs 1.68x a global solve; and its pessimistic side IS NOT A BOUND. A closed sandwich
 * certified a leaning stack as standing whose real lambda* is 0.44048, while the certified
 * band's own lambda* is 18.481256459924058 — wrong by 41.96x. The cause is one sentence:
 * dropping material outside a region removed its WEIGHT along with its restraint, and in
 * that fixture the omitted twenty-four courses were carrying the destabilising moment.
 *
 * WHAT SURVIVES AND IS NOT REPAIRED HERE: the optimistic side. Restricting a full-structure
 * admissible force system to a grounded-shell region stays admissible (core blocks keep all
 * their joints; shell blocks write no rows), so OPTIMISTIC-INFEASIBLE => GLOBALLY INFEASIBLE
 * holds. It is used unchanged, and the surcharge is deliberately kept off it.
 *
 * THE TWO REPAIRS, both user-ruled, both defined at their code in the support namespace:
 *
 *   (1) The pessimistic side keeps the omitted material's weight as a DEAD SURCHARGE while
 *       removing its restraint. "Bears on the region" is settled by graph reachability with
 *       the region deleted, so material that stands on its own foundation is not charged and
 *       material whose only road to the ground runs through the region is charged in full.
 *       The load acts on the vertical line through the charged component's own centre of
 *       gravity, which is what carries the overturning moment the old form lost.
 *   (2) Regions grow GROUND-ANCHORED BY CONSTRUCTION. The growth parameter stops being a
 *       radius and becomes the WIDTH of a full-height strip, because the free-boundary side
 *       is provably infeasible for any region containing no grounded block.
 *
 * THE QUESTION THIS TEST EXISTS TO ANSWER, stated so the measurement can fail honestly:
 *
 *     WITH BOTH REPAIRS IN PLACE, HOW SMALL CAN A CERTIFIED REGION BE?
 *
 * Success needs regions that are BOTH much smaller than the structure AND cheap enough that
 * two solves fit inside ~100 ms — the 84-block feasibility solve measures 0.0666 s, so a
 * certified region near that size is the target. If the repaired sandwich certifies only at
 * 90%+ of the structure, or costs more than a global solve, that is the same refutation in a
 * new coat and it points at the asynchronous / size-scoped fallbacks rather than at another
 * repair. This file must say so plainly if that is what it measures.
 *
 * ================================================================================
 * PREDICTIONS — DERIVATION RECORD REVISION 1, WRITTEN BEFORE THE FIRST RUN
 * (PROMOTION_DESIGN §7.3, mandatory: a prediction written after the measurement is not
 *  evidence, it is a transcription. Each one is repeated at the pin it decides.)
 * ================================================================================
 *
 *  P1  The carried set of a FULL-HEIGHT ground-anchored strip in a grounded wall is EMPTY
 *      at every width, because every omitted block's own column reaches the grounded bottom
 *      course. So repair (1) is a NO-OP on the wall arm and repair (2) does all the work
 *      there; repair (1) earns its keep on the collapse arm alone. If that holds it is a
 *      finding worth stating rather than a triviality — it says the two repairs are not
 *      alternatives, they answer different fixtures.
 *  P2  The optimistic side is FEASIBLE at every strip width (the cut wall stands globally,
 *      and grounding the shell can only help), so on the wall arm closure is decided
 *      entirely by the pessimistic side.
 *  P3  CLOSES AT HALF-WIDTH 0 — a two-cell zig-zag column, 41 of 149 blocks, 28%. Running
 *      bond gives the course above a one-brick hole a bearing on both sides inside the
 *      strip. Fallback if wrong: half-width 1, 65 blocks, 44%.
 *  P4  TEN of ten deletions certify at the closing width, against the refuted form's five.
 *  P5  COST 0.3-0.6x a global solve. The dead pose runs ~2.2 pivots per equality row at
 *      this scale and a 41-block region has ~35 ungrounded blocks = ~105 rows, so ~230
 *      pivots a side, ~460 for the pair, against the whole wall's 965. Point estimate
 *      0.48x, against the refuted form's 1.68x.
 *  P6  TIME: two region solves well inside 100 ms (84 blocks answer in 0.0666 s; 41 blocks
 *      should be ~0.02 s), so the VERDICT AT 12 COURSES IS PREDICTED YES.
 *  P7  HEIGHT SCALES THE REGION. An 18-course wall closes at the same half-width with ~62
 *      blocks — 1.5x the region for 1.5x the height. At the 30-course scenario height the
 *      same strip is ~100 blocks, at or past the 84-block / 0.0666 s figure, so the honest
 *      predicted answer is "small enough at 12 courses, marginal at 30".
 *  P8  The over-inclusive control (charge EVERY omitted block) is INFEASIBLE at the closing
 *      width and never certifies — the too-pessimistic failure mode, measured.
 *  P9  COLLAPSE ARM: the repaired pessimistic side is infeasible at every band, the
 *      optimistic side is unchanged, so the sandwich closes only with all thirty blocks.
 *      The repair withdraws the false certificate; it does not make collapse cheaper.
 * P10  NO FALSE CERTIFICATE ANYWHERE in this test: every closed sandwich agrees with the
 *      whole structure's own verdict. This is the assertion the slice would be worthless
 *      without, and it is counted rather than spot-checked.
 *
 * ================================================================================
 * WHAT WAS MEASURED — 2026-08-15, one run, tree at HEAD 0564bf5
 * ================================================================================
 *
 * THE WALL ARM. 12 courses x 12 cells, one brick out of course 6, 149 blocks / 385 joints,
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
 * **IT CLOSES AT THE FIRST RUNG: 41 OF 149 BLOCKS, 27.5%**, against the refuted form's 142
 * and 95%. Its two solves take 49 + 262 = 311 pivots against the whole wall's 965 —
 * **0.322x**, where the refuted form cost 1.68x — and 0.021 s for the pair against a
 * 0.100 s budget. Ten deletions at courses 1..10 certify **TEN of ten** (the refuted form:
 * five), every certificate checked against that deletion's own 149-block global solve.
 *
 * THE COLLAPSE ARM. Ground-anchored bands of the 30-course leaning stack:
 *
 *     band     blocks   charged   surcharge uu   optimistic   pessimistic   closes
 *     0..5          7        23      61,345.57     feasible    INFEASIBLE       no
 *     0..10        12        18      48,009.58     feasible    INFEASIBLE       no
 *     0..15        17        13      34,673.58     feasible    INFEASIBLE       no
 *     0..20        22         8      21,337.59     feasible    INFEASIBLE       no
 *     0..25        27         3       8,001.60     feasible    INFEASIBLE       no
 *     0..29        30         0              0   INFEASIBLE    INFEASIBLE      YES
 *
 * The repair does exactly what it was ruled to do and no more: **every false certificate is
 * withdrawn** (the bands now DISAGREE, which means grow the region), and **the collapse arm
 * is no cheaper than before** — it still certifies only with all thirty blocks, because the
 * optimistic side is the side that certifies a collapse and the repair deliberately does
 * not touch it. Across the whole test, 20 certificates issued and 0 of them false (and read
 * the caveat block below before treating that as evidence of anything).
 *
 * **AND THE COLLAPSE ARM'S COST IS 2.0x, NOT 0.322x. QUOTE BOTH RATIOS OR NEITHER.** At
 * closure the stack's two solves take 163 + 163 = 326 pivots against a 163-pivot global
 * solve, and the whole growth ladder — every band from 0..5 up — costs 1,300 pivots, **8x**
 * the global solve. So the standing arm's 0.322x and the collapse arm's 2.0x are the same
 * measurement's two halves, and the 2.0x is WORSE THAN THE 1.68x THE REFUTED FORM WAS
 * CONDEMNED FOR. The lever is cheap exactly where the answer is "it stands" and dear
 * exactly where the answer is "it falls", which is the arm a player's deletion cares about.
 *
 * HEIGHT SCALES THE REGION, WHICH IS THE FINDING THAT PRICES SCENARIO SCALE. The same wall
 * at 18 courses is 224 blocks and closes at the same half-width with 62 blocks — 27.7% of
 * the structure, and 1.512x the 12-course region for 1.5x the height. The FRACTION is
 * height-invariant; the COUNT is not. Linear in height off those two points (ESTIMATE), a
 * 30-course scenario wall's certified strip is ~102 blocks, larger than the 84-block
 * fixture that answers in 0.0666 s — so the answer to "is a certified region small enough
 * for synchronous authority" is YES at 12 courses, MARGINAL at 30, and it is region SIZE
 * rather than the sandwich that decides it.
 *
 * TWO PREDICTIONS MISSED, both recorded as misses at their pins:
 *
 *   - P5's point estimate (0.48x) against the measured 0.322x. The range held; the point
 *     estimate priced both sides as if they had the same row count, and the OPTIMISTIC side
 *     is nearly free (49 pivots for 41 blocks) because grounding the shell turns most of
 *     the region's boundary into earth and a grounded block writes no equilibrium row.
 *   - P8 outright. Charging EVERY omitted block — all 108, 271,309.925 uu — still leaves
 *     the 41-block strip FEASIBLE, so the predicted "too pessimistic to certify" failure
 *     mode is not demonstrated by this fixture and remains untested. The wall arm has so
 *     much margin that it cannot discriminate surcharge rules at all.
 *
 * ================================================================================
 * THE CAVEAT THAT MATTERS MOST, AND IT IS NOT A FOOTNOTE
 * ================================================================================
 *
 * P1 held: the carried set of a full-height strip in a grounded wall is EMPTY at every
 * width. So on the wall arm THE REPAIRED PESSIMISTIC SIDE IS BIT-IDENTICAL TO THE REFUTED
 * ONE — same problem, no applied forces — and every wall certificate above rests on repair
 * (2) alone. What repair (1) removes is one specific unsoundness: omitted material whose
 * only road to the ground ran through the region, which is precisely the leaning stack's
 * mode and which a full-height ground-anchored strip cannot produce at all.
 *
 * "20 CERTIFICATES, 0 FALSE" IS NO EVIDENCE OF SOUNDNESS, AND THE REASON IS MECHANICAL
 * RATHER THAN A MATTER OF SAMPLE SIZE. Nineteen of the twenty are issued about structures
 * the global solve reads FEASIBLE, and the twentieth is the whole leaning stack — so NOT
 * ONE certificate in this file is issued about a PROPER SUBSET of an INFEASIBLE structure,
 * which is the only shape a false certificate can take. CheckCertificate is therefore
 * re-testing the OPTIMISTIC side (already sound by restriction) on every row it runs. This
 * is TRAPS' "an invariant asserted over fixtures that all share a hidden property is not an
 * invariant", verbatim: the hidden property is that every fixture with a proper-subset
 * region stands. The honest split of the twenty is 18 proper-subset + 2 trivial
 * whole-structure closures (the wall at w = 5, 149 of 149, where the pessimistic side's 965
 * pivots ARE the global solve; and the stack at 0..29, 30 of 30).
 *
 * AND AT LEAST THREE UNSOUNDNESSES REMAIN, not one. Each is a way a certificate could be
 * issued about a structure that falls, and none is exercised by any fixture here:
 *
 *   1. THE OMITTED MATERIAL THAT HAS ITS OWN GROUND PATH AND ALSO BEARS ON THE REGION. The
 *      carried-set rule charges it nothing. No fixture can expose it, because every wall
 *      the LP prices is feasible — the only sub-1.0 fixture in the catalogue is the stack.
 *   2. THE INTERFACE JOINT IS NEVER CHECKED. SpikeExtractRegion drops every joint with an
 *      end outside the region, and SpikeAddSurcharge applies the load DIRECTLY to the
 *      region block — so the contact that would have to transmit it is not in the problem
 *      and its capacity is never asked about. Review worked the stack's case: joint 5-6
 *      would need T = 636,000 uu against a 412,562 uu cap, a demand the full structure
 *      cannot deliver and the region problem never poses. The surcharge is a rigid
 *      attachment where reality has a joint with a strength.
 *   3. THE CHARGED COMPONENT'S OWN INTERNAL JOINTS VANISH — courses 6..29's twenty-three
 *      bed joints are in no problem at all — so a hanging component that fails INTERNALLY
 *      leaves the region certifying happily. And where a component meets the region at
 *      SEVERAL joints, the area-proportional split is a fixed arbitrary choice where the
 *      real structure lets the LP choose the distribution.
 *
 * The concrete shape to watch for, worth writing down because it is not exotic: a wall
 * region carrying an omitted spandrel whose own bed joint sits at 1.01 of capacity. The
 * spandrel's weight is charged, the region carries it comfortably, the certificate is
 * issued, the spandrel drops, and nothing in this file notices.
 *
 * What would settle (1) is a wall the LP prices below 1.0 whose ground-anchored strip still
 * stands. What would settle (2) and (3) is a surcharge that keeps the interface joint and
 * the charged component's internal joints in the problem while still writing no equilibrium
 * row for the component — a different and larger repair. All three are specified follow-ups,
 * not findings this file may assume away.
 *
 * ================================================================================
 * GREEN ON ARRIVAL IS NOT THE SAME AS ASSERTING NOTHING — THE MUTATIONS
 * ================================================================================
 *
 * Every row here is green the day it lands, because the extractor and the measurement land
 * together. TRAPS' registry carries the proof, and two of the three are new:
 *
 *   X8  the surcharge applied at the CONTACT's X instead of the charged component's centre
 *       of gravity  ->  4 assertions here + **3** in RegionalSandwich's PART D, and it
 *       **RE-CREATES THE FALSE CERTIFICATE**: band 0..5 certifies feasible=1 against a
 *       stack that reads infeasible. That is the sharpest evidence in the slice — it says
 *       the load-bearing half of repair (1) is WHERE the weight acts, not that it acts.
 *       The third PART D assertion is the LINE-OF-ACTION pin, added 2026-08-15 in review:
 *       "independently derived as 175 cm ... and it acted at 55 cm". It exists because the
 *       verdict rows alone would let a wrong-but-NEARER X through in silence — the contact
 *       is the extreme case, and a load misplaced onto the region block's own centroid
 *       might flip nothing.
 *   X9  the carried set computed as empty (repair (1) removed, plumbing left in place)
 *       ->  8 assertions here + 5 in PART D, and THREE false certificates.
 *  X10  the Carried rule made to charge EVERY component, grounded or not (the rule
 *       selector inverted)  ->  7 here: five of the six wall Charged pins (0 -> 108, 84,
 *       60, 36, 12 as the strip widens and the omitted set shrinks; w = 5 stays 0 because
 *       nothing is omitted) plus BOTH pessimistic pivot pins (262 -> 514 and the tall
 *       arm's 474 -> 761, the surcharge rows costing pivots). The prover the Charged pins
 *       lacked, and the one that shows the RULE SELECTION is read and not only the total.
 *       No verdict moves under it, which re-states P8's miss from a second direction.
 *   X6  the extractor keeps the out-of-region blocks — re-measured against the repaired
 *       extractor  ->  12 here (five block counts, the closing region's size and both its
 *       pivot pins, the cost-ratio row, the under-half-the-wall row, the tall region and
 *       the stack's closing band) and still 14 in RegionalSandwich.
 *
 * WHAT STILL HAS NO BITE-PROVER, recorded rather than papered over, because a green row
 * nothing can fail is indistinguishable from a row that asserts nothing:
 *
 *   - **Agreements == 10.** Its only failure mode is a deletion that stops certifying, and
 *     no mutation available moves it: X6 changes region sizes without changing verdicts,
 *     X9/X10 change the surcharge where the wall arm's carried set is empty either way, and
 *     an N6-style rung flip of the ten courses would leave the count at ten. It is a
 *     ceiling-shaped pin and should be read as one.
 *   - **The three surcharge-integrity guards in PoseSandwich** (no orphan component, none
 *     delivered onto a grounded block, applied == charged). They RUN on a live non-zero
 *     path — the EveryOmittedBlock control charges 108 blocks through 44 interface joints —
 *     so they are exercised, but no mutation makes one FAIL. The cheapest provers are
 *     recorded here rather than built: drop the interface search's "other end in region"
 *     test to manufacture an orphan; deliver every share to the joint's BlockA to
 *     manufacture a grounded delivery.
 *
 * COST OF THIS TEST: **9.4 s**, measured and printed at the bottom of the run, of which the
 * ten per-deletion global solves are about a third. It sits in the OPT-IN sweep's FAST
 * tier (7 tests, ~78 s — the tier is for iteration; `OracleSweepFull` is what verifies a
 * solver change), where it is one of the two most expensive rows; the default suite is
 * untouched at 173 = 167 green + the six standing deliberate reds.
 *
 * NEEDS A TICKING WORLD: NO.
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
	 * A REFUSAL IS NOT AN INFEASIBILITY, and SpikeIsFeasible cannot tell them apart —
	 * Refuse() returns bAnswered = false with lambda = 0, bit-for-bit what the dead-load
	 * infeasibility arm returns. Every region solve in this test is guarded, for the same
	 * reason the sandwich test's are: an unguarded refusal reads as "this region has no
	 * admissible equilibrium", which is the shape of half the conclusions below.
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
		 * THE SURCHARGE IS THE PESSIMISTIC SIDE'S ALONE, asserted rather than trusted: it
		 * is the one thing that would silently unsound the half of the sandwich review
		 * verified, and the extractor's contract is one line of code away from doing it.
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
		 * REPAIR (2), MADE EXECUTABLE. A region with no grounded block is infeasible on the
		 * free side whatever else is true of it (sum its vertical rows: 0 = -W_region), so
		 * "ground-anchored by construction" is the property the whole strip parameterisation
		 * exists to guarantee. If it ever fails, every certificate below is vacuous.
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
	 * The same fixture the refuted form measured: a 12-course x 12-cell wall with one brick
	 * out of course 6, 149 blocks, whose whole-wall feasibility solve takes 965 pivots. Same
	 * wall, same deletion, so the two tables are directly comparable and the only difference
	 * is the region shape and the surcharge.
	 * ================================================================================ */

	constexpr int32 WallCourses = 12;
	constexpr int32 WallCells = 12;
	constexpr int32 WallDeleteCourse = 6;

	FStructure Wall;
	FString Why;

	/*
	 * Each producer call is its OWN statement before its message is built: folding a call
	 * that writes Why into the Printf that reads it is unsequenced, and MSVC evaluates the
	 * condition last (TRAPS). Every such pair in this test is split the same way.
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
	 * MEASURED / PREDICTED per rung. The Charged column is P1's test: it should read 0 at
	 * every width, because a full-height strip leaves nothing stranded in a grounded wall.
	 *
	 * MEASURED 2026-08-15: the block count is exactly 24w + 41 up to the wall's own width
	 * (17 core + 24 shell at w = 0, and 24 more core per step), the strip closes at the
	 * VERY FIRST rung, and the Charged column reads 0 on all six — P1 held.
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
		 * THE COST COMPARISON, IN PIVOTS BECAUSE PIVOTS ARE DETERMINISTIC AND SECONDS ARE
		 * NOT. The refuted form's two solves cost 1.68x the global solve it existed to
		 * avoid, and this line is written as the strict inequality that says which side of
		 * 1.0 the repaired form lands on. If it ever crosses back, the lever has stopped
		 * being worth anything and this is where that is noticed.
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
		 * AND THE REGION MUST BE A NEIGHBOURHOOD RATHER THAN THE BUILDING. Half the
		 * structure is the loosest reading of "much smaller than the whole" that could
		 * still be called a decomposition; the refuted form's 95% failed it by a mile.
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
	 * The refuted form certified five of ten and the five were exactly those whose ball
	 * happened to reach the foundation. A ground-anchored strip reaches it by construction,
	 * so the deletion's height should stop mattering — which is a prediction with a number
	 * in it (P4: ten of ten) and a fixed ladder of ten bricks, the same ten every run.
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
	 * "Keep the omitted material's weight" has an obvious reading — keep ALL of it — and
	 * that reading is the second way to kill the lever, quietly: a pessimistic side that is
	 * too pessimistic never certifies and looks exactly like a decomposition that does not
	 * decompose. Measured rather than argued, at the same width and on the same wall, so the
	 * only difference between this row and the certified one is the surcharge rule.
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
	 * PART 4 — HEIGHT SCALES THE REGION, AND THAT IS WHAT DECIDES SCENARIO SCALE.
	 *
	 * A ground-anchored region has to reach the foundation, so its size is set by the
	 * structure's HEIGHT and not by any bonded distance around the deletion. Scenario walls
	 * are 30 courses; this measures the slope with the same wall at 18.
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
					 * THE COST AT HEIGHT, PINNED — the second point of the two the
					 * scenario-scale extrapolation is drawn through. 79 + 474 = 553 here
					 * against 49 + 262 = 311 at twelve courses is 1.78x of cost for 1.5x of
					 * height, and that slope is the whole of the answer to "does a certified
					 * region stay inside the budget at thirty courses".
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
	 * Every wall the project owns is LP-feasible, so Parts 1-4 exercise only the standing
	 * side. The 30-course leaning stack is the one cheap fixture the LP prices below 1.0
	 * (lambda* 0.44048) and it is the fixture that exposed the false certificate. Here the
	 * regions are ground-anchored BANDS — courses 0..k — which is repair (2)'s shape for a
	 * chain, and each one is posed with repair (1) live.
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
				 * that has to GROW is charged for the whole walk, not only for the rung it
				 * stops on, and on this arm the walk is the entire cost.
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
					 * THE SURCHARGE'S OWN SET, PINNED ON THE ARM WHERE IT DOES THE WORK. A
					 * reachability walk that charged the wrong blocks would still leave this
					 * band infeasible and every verdict in this section would stay green, so
					 * the count is asserted rather than inferred from the answer.
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
					 * THE RATIO, STATED AS THE INEQUALITY IT IS. The wall arm asserts the
					 * sandwich costs LESS than the global solve; on the collapse arm the same
					 * comparison goes the other way, and both directions are asserted so
					 * neither ratio can be quoted without the other showing up beside it.
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
	 * The refuted form did not fail by certifying too rarely; it failed by certifying a
	 * structure that has no equilibrium. Every closed sandwich in this file is checked
	 * against the whole structure's own verdict as it is produced, and the count is asserted
	 * here so "no false certificate" is one number rather than a scattering of rows that
	 * could all be skipped by an early return.
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

#endif // WITH_DEV_AUTOMATION_TESTS
