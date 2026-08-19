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
 * WARM STARTS - THE LAST UNMEASURED LATENCY LEVER (PROMOTION_DESIGN.md §5.4, §12 D2'').
 * Measurement only: no production behaviour is designed here, and under the project's
 * convention a measurement lands as a PINNED ROW whose unmeasured state is its red.
 *
 * THE QUESTION, in one sentence:
 *
 *     Production re-solves after a SMALL CHANGE - a brick deleted from a structure it has
 *     already answered. Does starting the simplex from the previous solve's basis collapse
 *     the pivot count, and by how much?
 *
 * WHY IT IS THE WHOLE REMAINING MARGIN. Posing the problem as feasibility at lambda = 1
 * instead of maximising lambda* took the 84-block gate fixture to 0.0666 s against a 50 ms
 * target - 1.33x short - and wall-01's 375 blocks to 26 s (measured 2026-08-15, pinned in
 * OracleSweepFull.RigidBlock.FeasibilityReformulationCost). Regional decomposition then
 * measured at 27.5% of a wall for 0.322x a global solve, but only on the arm where nothing
 * fell. Every other lever on the roadmap is spent or refuted. If warm starts deliver the
 * ~10x the roadmap hopes for, the latency question closes; if they deliver 2x, the design's
 * fallbacks - asynchronous authority, or size-scoped promotion - become the leading options.
 * Either answer is worth having, which is why this file is a measurement and not a feature.
 *
 * ================================================================================
 * THE SEAM THIS FILE DEMANDS, AND WHY IT IS THE SMALLEST ONE THAT ANSWERS THE QUESTION
 * ================================================================================
 *
 * The oracle has no way to inject a starting basis and no way to read the one it ended on,
 * so the experiment is not expressible with what exists. Three fields, declared in
 * RigidBlockOracle.h with their contracts:
 *
 *   - FOracleResult::FinalBasis     - the basis the solve ended on, plus the SHAPE it
 *                                     belongs to (NumStructCols, ArtificialStart), which is
 *                                     what a caller needs to map it onto a changed problem;
 *   - FOracleProblem::StartingBasis - one column per row, INDEX_NONE meaning "no hint for
 *                                     this row"; empty means a cold start and must be
 *                                     bit-identical to today;
 *   - FOracleResult::WarmStartColumnsAccepted - how many hints the solve actually started
 *                                     from, because a warm start that saved nothing is
 *                                     otherwise indistinguishable from one that was thrown
 *                                     away, and those are OPPOSITE findings.
 *
 * NO DUAL SIMPLEX IS ASKED FOR AND NONE IS NEEDED. Seeding the PRIMAL simplex with a
 * related problem's final basis measures the effect honestly: if the seeded basis is
 * feasible for the new problem phase 1 is trivial, and if it is not, phase 1 repairs it -
 * and the pivots the repair costs are exactly the number this file wants. A dual simplex is
 * a large piece of new machinery and the cheap experiment comes first.
 *
 * TWO HAZARDS THE IMPLEMENTATION MUST FACE, NAMED HERE BECAUSE THE ASSERTIONS BELOW ARE
 * WHAT CATCH THEM:
 *
 *   - A WARM BASIS IS USUALLY PRIMAL INFEASIBLE, and the solver's existing arithmetic would
 *     hide that. The deleted brick's joints carried force; without them the neighbours'
 *     equilibrium rows no longer balance, so B^-1 b has negative entries. FRevisedState::
 *     Refactorise CLAMPS negative basic values to zero, documented as "rounding at
 *     degenerate vertices" - true of a cold start, false of a warm one, where a clamp turns
 *     a genuinely infeasible basis into a plausible feasible-looking one. And phase 1 is
 *     SKIPPED whenever the basic artificials sum to zero, which a warm basis satisfies by
 *     construction: its artificials were driven out by the previous solve. So the repair has
 *     to be reached deliberately; it will not happen by itself.
 *   - A WARM BASIS MAY BE SINGULAR in the new matrix. The old basis is independent in the
 *     OLD matrix; drop some of its columns and some of its rows and independence is not
 *     inherited. A singular basis must be REPAIRED (each column that cannot be pivoted
 *     replaced by the cold default for its row) rather than refused: a warm start that fails
 *     closed measures nothing, and WarmStartColumnsAccepted is what keeps the repair honest.
 *
 * THE ASSERTION THAT MATTERS MOST IS NOT A PIVOT COUNT. A warm start that changes the
 * ANSWER is a defect, not a speedup, so every row asserts lambda and the verdict are
 * IDENTICAL cold and warm - and identical BIT FOR BIT, which is legitimate rather than
 * brittle in this pose for a reason worth stating: with gravity dead and no live load the
 * lambda column appears in no equilibrium row, so lambda can only be exactly LambdaCap
 * (feasible) or exactly 0 (the dead-load infeasibility arm). The answer is a boolean wearing
 * a double's clothes, and each row pins that too, so a live load leaking into a pose meant to
 * be dead cannot pass as a warm-start effect.
 *
 * ================================================================================
 * HOW A DELETION IS POSED SO THAT A BASIS CAN BE MAPPED ACROSS IT
 * ================================================================================
 *
 * A basis is a list of COLUMN INDICES, one per ROW, so carrying one across a change means
 * knowing which of the new problem's rows and columns are the old ones. The assembly's
 * layout (RigidBlockOracle.cpp) makes that a matter of ORDERING rather than bookkeeping:
 *
 *     rows    = 3 per non-grounded block, in block order; then the lambda cap row;
 *               then the strength rows, per contact, in contact order
 *     columns = lambda, then [n+, n-, p, q] per contact, in contact order;
 *               then one slack per inequality row, in row order;
 *               then one artificial per row, in row order
 *
 * So if every joint that dies is LAST in the joint array, every surviving contact keeps its
 * ordinal, every surviving strength row keeps its ordinal, and the rows that vanish are
 * exactly the trailing ones. Two consequences make the mapping pure arithmetic on the four
 * integers a solve reports: a surviving structural column keeps its index outright, and a
 * surviving slack or artificial keeps its ORDINAL and shifts by the change in the counts.
 *
 * THE DELETED BLOCK STAYS IN THE PROBLEM AS A MASSLESS ORPHAN rather than being removed from
 * the array. Removing it would delete its three equilibrium rows from the MIDDLE of the row
 * list - before the cap row and every strength row - and shift everything after them, which
 * is the one thing the mapping cannot absorb. A massless block with no joints writes three
 * rows that are entirely empty with a zero right-hand side (FAssemblyRow::Add drops zero
 * coefficients), so it constrains nothing and its artificial sits basic at zero in a
 * genuinely redundant row - the case the solver's pivot-out pass already documents. It is
 * the same LP as deleting the block outright, and it is compared against ITSELF cold and
 * warm, so nothing in the measurement rests on the equivalence.
 *
 * THE MAPPING IS TEST-SIDE ON PURPOSE, exactly as slice 0b's region extractor is: it is a
 * property of how the caller changed the problem, not of the solver, and production will map
 * from its own assembly rather than from a reported shape. Putting it in the oracle would be
 * building the feature instead of measuring it.
 *
 * ================================================================================
 * PREDICTIONS - DERIVATION RECORD REVISION 1, WRITTEN BEFORE THE FIRST RUN
 * (PROMOTION_DESIGN §7.3 makes this mandatory. This project's estimates have a poor
 *  record - the last three levers measured came in at 5-16x where 10-100x was predicted,
 *  0.63% where "most of the saving" was predicted, and 2.0x where 2.9x was - and saying so
 *  up front is the point of the discipline.)
 * ================================================================================
 *
 * THE RECORDED EXPECTATION is the roadmap's ~10x, and it is a hope with no measurement
 * behind it. Here is a derivation instead.
 *
 * In the feasibility pose the entire cost is phase 1 driving one artificial out of the basis
 * per equality row - three per non-grounded block - and phase 2 is ONE pivot taking lambda to
 * its cap. So the trap this measurement is most likely to fall into cannot even arise here:
 * there is no phase-2 saving to mistake for a phase-1 one, and both are pinned separately
 * anyway. What a warm start saves is the part of phase 1 that re-derives what did not change,
 * and what it must still pay is the repair around the change.
 *
 * TWO COMPETING PREDICTIONS, and the measurement decides between them:
 *
 *   (A) THE REPAIR IS LOCAL. A one-brick deletion disturbs ~6 joints (24 contacts of rows)
 *       plus 3 block rows - call it 50-100 rows out of the 84-block wall's ~2,700. If phase 1
 *       only has to re-price and re-pivot the disturbed neighbourhood, the warm cost is a
 *       small multiple of the disturbed row count and is roughly INDEPENDENT of structure
 *       size, so the ratio GROWS with scale: ~3x at 84 blocks, ~10-18x at 375.
 *   (B) THE REPAIR PROPAGATES. This project has already measured that thrust is not local -
 *       case 21's mechanism runs the full jamb chain to the ground, and slice 0b's regions
 *       only certify when they reach the foundation. If a deleted brick's load has to find a
 *       new path all the way down, the repair is a global re-solve wearing a warm hat and the
 *       ratio is ~1-2x everywhere, with the large fixtures no better than the small ones.
 *
 * PREDICTED, per row, as pivot ratios (cold total / warm total):
 *
 *     row                                     PREDICTED       reasoning
 *     gate wall 8x10, one brick out            1.6x - 5x      (A) says 5x, (B) says 1.6x
 *     gate wall 8x10, top course out           1.2x - 2.5x    10 bricks is a big change
 *     gate wall 8x10, the ground cut            0.7x - 1.5x   THE TRAP ROW: the verdict
 *                                                             flips to falls and the warm
 *                                                             basis is catastrophically
 *                                                             wrong; a warm start may well
 *                                                             cost MORE than a cold one
 *     12x12 wall, one brick out                2.5x - 6x
 *     wall-01 375 blocks, one brick out          5x - 18x     the fixture that separates
 *                                                             (A) from (B) most sharply
 *
 * PREDICTED SHARE ACCEPTED: 90-99% of the supplied columns on the one-brick rows (only the
 * dead contacts' columns and whatever the singularity repair rejects should be lost), 70-95%
 * on the course deletions.
 *
 * PREDICTED GATE ARITHMETIC: the gate fixture needs 50 ms and answers cold in 66.6 ms. Under
 * (A) a warm re-solve lands near 20 ms and the budget closes with room; under (B) it lands
 * near 45 ms and closes with none, which would leave synchronous authority resting on
 * regional decomposition alone - a lever whose pessimistic side is a heuristic rather than a
 * bound. THE HONEST HEADLINE PREDICTION IS THEREFORE NOT "10x": it is that warm starts pay
 * where the change is small and the structure is large, and that the row most likely to
 * refute the lever is the one where the deletion brings the structure down.
 *
 * ================================================================================
 * WHAT IS MEASURED HERE - filled in as the run happens, never before
 * ================================================================================
 *
 * THE COLD HALF IS MEASURED (2026-08-16, one run, tree at HEAD 74bb081 plus this file's
 * seam declarations). It needs no seam - three cold solves per row - so it is pinned now
 * rather than left as a sentinel, and it is the baseline every warm ratio will be read
 * against:
 *
 *     row                                  blocks/joints  deleted  BEFORE  COLD after
 *     gate wall 8x10, one brick, course 3      84 / 207    1 / 6      436    343 (342 ph1)
 *     gate wall 8x10, top course out           84 / 207   11 / 30     491    291 (290 ph1)
 *     gate wall 8x10, the ground cut           84 / 207   10 / 20     442     90 ( 90 ph1)
 *     12x12 wall, one brick, course 6         150 / 391    1 / 6      940    919 (918 ph1)
 *     wall-01, one brick, course 3            375 / 1030   1 / 6    5,643  4,764 (4763)
 *     seam fixture, 4x4 wall                   18 /  35       -        48     -
 *
 * In seconds, unpinned and machine-dependent: the gate wall's cold re-solve is ~51 ms, the
 * 12x12's ~330 ms, and wall-01's ~15.5 s against the ~26 s its intact dead pose costs.
 *
 * THREE THINGS THE COLD HALF ALREADY SAYS, before a warm start exists:
 *
 *   - THE TOP-COURSE ROW'S "BEFORE" SOLVE IS 491 PIVOTS, which is bit-for-bit the intact
 *     8x10 wall's dead-pose count pinned in OracleSweepFull.RigidBlock.
 *     FeasibilityReformulationCost. That is a free cross-check on the joint reordering this
 *     file does: the top course's joints are already last in the bridge's order, so that
 *     row's reorder is a no-op and the count must match - and it does. The other two gate
 *     rows reorder genuinely and read 436 and 442, which is the pivot path moving with the
 *     column order and nothing else: same wall, same lambda, same verdict. wall-01 reads
 *     5,643 against its own pinned 5,407 for the same reason - moving six joints to the end
 *     of a 1,030-joint array moves the path by 4%.
 *   - A RE-SOLVE AFTER A DELETION IS ALREADY CHEAPER THAN THE SOLVE BEFORE IT (343 against
 *     436, 291 against 491, 919 against 940) purely because the problem got smaller. Any
 *     warm-start ratio must be read against the COLD RE-SOLVE, never against the previous
 *     solve, or it books that shrinkage as a saving the lever did not make.
 *   - THE GROUND CUT ANSWERS IN 90 PIVOTS. Proving this collapse is 3.8x CHEAPER than
 *     proving the intact wall stands, which is one more instance of the finding slice 0b
 *     recorded: falling is not the expensive direction here.
 *   - AND THE GATE ARITHMETIC IS ALREADY CLOSER THAN THE HEADLINE SUGGESTS. Slice 0b's
 *     1.33x shortfall is the INTACT 84-block wall at 66.6 ms; the thing production actually
 *     runs is the RE-SOLVE, which is a smaller problem and takes ~51 ms cold on this
 *     machine. Reported, not pinned - it is one machine on one day and seconds are not
 *     bit-reproducible - but it says the warm start is being asked for a factor near 1
 *     rather than near 1.33 at the gate size, and for ~310x at wall-01.
 *
 * THE WARM HALF IS MEASURED (2026-08-16, one run, the seam in the tree). THE LEVER IS
 * REFUTED, and not narrowly:
 *
 *     row                                  COLD    WARM   accepted   ratio (cold/warm)
 *     gate wall 8x10, one brick             343     342   2600         1.003x
 *     gate wall 8x10, top course            291   1,543   2032         0.189x
 *     gate wall 8x10, the ground cut         90      77   2441         1.169x
 *     12x12 wall, one brick                 919   4,192   4560         0.219x
 *     wall-01, one brick                  4,764   4,828      0         0.987x  ABANDONED
 *
 * FOUR THINGS THE RUN SAYS, none of which either prediction contained:
 *
 *   - PREDICTION (A) AND PREDICTION (B) ARE BOTH WRONG, and (B) is the closer of the two.
 *     The best row is 1.003x - a saving of ONE pivot in 343 - against a predicted 1.6x-5x,
 *     and three of the five rows are WORSE than a cold solve. There is no ~10x here and no
 *     2x; on the gate fixture the lever is worth nothing at all.
 *   - IT GETS WORSE WITH SIZE, WHICH IS THE OPPOSITE OF WHAT THE LEVER IS FOR. 1.003x at 84
 *     blocks, 0.219x at 150, abandoned at 375. Prediction (A)'s whole argument was that a
 *     local repair is size-independent so the ratio GROWS with scale; the measurement runs
 *     the other way, which is (B)'s thrust-is-not-local reasoning arriving with a worse
 *     constant than (B) itself predicted.
 *   - THE COURSE DELETION COSTS 5.3x MORE THAN A COLD SOLVE (1,543 against 291). A warm
 *     basis that is far from feasible is not a head start; it is a worse starting point than
 *     the cold default, and phase 1 pays for the difference.
 *   - AND SECONDS ARE WORSE THAN PIVOTS ON EVERY ROW, because a warm solve carries the
 *     seeding, an extra factorisation and the appended columns before its first pivot. Not
 *     pinned - seconds are the machine - but reported, because a lever refuted on pivots and
 *     refuted harder on the clock is refuted twice.
 *
 * THE ONE ROW THAT IS NOT A RATIO AT ALL, and it is the finding the design leans on:
 * WALL-01 ABANDONS. 12,459 of 13,362 mapped columns were carried, and the seeded basis went
 * SINGULAR at the first periodic refactorisation - pivot 64 - so the solver refused, threw
 * the start away and answered from a fresh cold solve. Accepted is 0 because the attempt was
 * discarded, not because nothing was offered, and the 64 wasted pivots are pinned as
 * WarmPivots - ColdPivots so that the two cannot be confused. THE CONSEQUENCE FOR THE
 * ROADMAP: §5.4's dual simplex is proposed as a re-solve FROM THE SAME MAPPED BASIS, so it
 * inherits the same singular wall rather than avoiding it - the fragility measured here is a
 * property of the mapping at scale, not of which simplex reads it.
 *
 * AND THE ONE GRAVITY-LIVE ROW AGREES TO THE BIT (PART C, measured 2026-08-18). The 18-block
 * seam wall, one brick out of course 1, posed with gravity LIVE so lambda* is a real number:
 * cold 186 pivots, warm 458 pivots from 340 of 378 mapped columns, and lambda* =
 * 2166.9018254298085 from BOTH - an absolute gap of exactly zero on two visibly different
 * pivot paths. So the seam is answer-preserving on the maximising arm as well as on the dead
 * one, which is the half every other row in this file is structurally unable to check; and
 * the lever is 0.406x here too, refuted on the arm it was never measured on.
 *
 * WHAT IS STILL NOT PINNED, and it is N1 in the review record: the wrapper discards the
 * abandoned attempt's own refusal reason, so "wall-01 refuses at pivot 64 with
 * NumericalFailure" is not observable through the seam and this file cannot assert it. The
 * pivot-64 attribution therefore rests on hand instrumentation recorded at the wrapper, and
 * closing it needs FOracleResult to carry the discarded attempt's EOracleRefusal - a
 * production change with its own red. Logged in CURRENT_STATE.
 *
 * ================================================================================
 * WHAT IS PINNED AND WHAT IS DELIBERATELY NOT
 * ================================================================================
 *
 * PINNED EXACTLY: block and joint counts per row, and the number of blocks and joints the
 * deletion takes (TRAPS: a lambda window and a ratio cannot catch a rung secretly building
 * the wrong wall - only a count pin can); every pivot count, because the solver is
 * bit-deterministic; the exact lambda the pose admits (LambdaCap or 0); the feasibility
 * verdict before and after the deletion; and the number of warm-start columns accepted.
 *
 * ASSERTED AS AN IDENTITY, NOT A WINDOW: lambda and the verdict, cold against warm, with
 * operator== on the doubles. See above for why exactness is legitimate in this pose - and
 * see PART C, which is the one row where it is NOT, and which exists because "exact" and
 * "vacuous" are the same word when a double can only hold two values.
 *
 * PINNED ON THE ABANDONED ARM INSTEAD: the acceptance count as EXACTLY zero, and the wasted
 * work as WarmPivots - ColdPivots. The difference rather than the total, because the
 * difference asserts two things at once - how far the mapped basis got before it refused,
 * and that the cold retry underneath it cost exactly the cold baseline, which is what makes
 * it a genuine fresh solve rather than a continuation.
 *
 * NOT PINNED: WALL-CLOCK TIME. Timings measure the machine, not the solver; every second
 * below is reported and none is asserted, following the spike file's discipline.
 *
 * NOT ASSERTED, DELIBERATELY: that the warm start is FASTER. The row that says so is the
 * pinned pivot count, and if the measurement says a warm start costs more, the pin records
 * that and the lever is refuted. A test that asserted "warm < cold" would be asserting the
 * conclusion it exists to measure.
 *
 * COST AND TIER, MEASURED 2026-08-16/18 and stated rather than grown quietly (TRAPS: an
 * opt-in tier is a tier that rots, and the slow one is 94% of the group already). The FAST
 * tier gains one test at **5-6 s** - four rows at three solves each, the seam fixture, and
 * PART C's three live solves. The FULL tier gains one test at **56-61 s**, which is wall-01's
 * three solves and is the only thing this slice puts in the slow tier. Both quoted as ranges
 * because seconds on this machine move by more under load than several fast rows cost.
 * Neither name contains DestructionGame, so the default suite - 173 tests - cannot reach
 * either; the two tiers together report **13** completed tests.
 *
 * THE BITE-PROVERS, because the warm pins are green from the moment they are written and a
 * green-on-arrival row is indistinguishable from one that asserts nothing:
 *
 *   X12  the seam's PRIMAL-INFEASIBILITY REPAIR skipped (the sign-flip loop gated off), so a
 *        warm basis's genuine negatives reach Refactorise's clamp -> three of the four PART B
 *        rows STOP ANSWERING, two on verification and one on phase 1. Its lesson is sharper
 *        than its target: the lambda identities are defended by the post-solve VERIFICATION
 *        GATE, not by the repair - with the repair gone the solver never returns a wrong
 *        lambda, it refuses.
 *   X14  `Form.InitialBasis = Seed;` DELETED, so the seed is computed and never installed
 *        while the acceptance count is still derived from it -> **16 assertions across both
 *        tests**: PART A's two idempotence pins (0 -> 48 and 0 -> 47), PART C's live warm
 *        pivots (458 -> 186), all eight PART B warm pivot pins, and wall-01's Accepted == 0
 *        (-> 12,459) and wasted-work pin (64 -> 1,702). **EVERY WarmAccepted PIN STAYS
 *        GREEN**, which is the measured statement of why the pivot pins carry this file: the
 *        count is computed independently of the assignment and will report a warm start the
 *        solver never took.
 *
 * NEEDS A TICKING WORLD: NO. Producers, the bridge and the LP are arithmetic on plain
 * structs.
 *
 * NAMED NAMESPACE, not anonymous, and every file-scope name carries a Warm prefix: a unity
 * build merges many files into one translation unit and every anonymous namespace in the
 * blob is the same namespace (TRAPS).
 */
namespace OracleWarmStartSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace RigidBlockOracle;

	/* ================================================================================
	 * THE BRICK AND THE GRID - derived here, imported from nowhere.
	 * ================================================================================ */

	constexpr double WarmBrickLengthCm = 21.5;
	constexpr double WarmBrickWidthCm = 10.25;
	constexpr double WarmBrickHeightCm = 6.5;
	constexpr double WarmClayDensityGramsPerCubicCm = 1.9;
	constexpr double WarmJointCm = 1.0;

	/** Course pitch on the coordinating grid: 7.5 cm. */
	constexpr double WarmCoursePitchCm = WarmBrickHeightCm + WarmJointCm;

	/**
	 * "NOBODY HAS MEASURED THIS YET". Every pivot count and every accepted-column count is
	 * a non-negative integer and INDEX_NONE is a value the seam can legitimately report
	 * (no warm start supplied), so the unmeasured sentinel has to be a third thing the
	 * solver can never produce.
	 */
	constexpr int32 WarmUnmeasured = -2;

	/*
	 * PART C's pins - the one GRAVITY-LIVE row, where lambda* is a real number rather than a
	 * verdict wearing a double's clothes. Kept here beside the sentinel rather than inline so
	 * the unmeasured state is spelled the same way as every other row's.
	 */
	constexpr int32 WarmLiveDoomedJoints = 6;
	constexpr int32 WarmLiveColdPivots = 186;
	constexpr int32 WarmLiveWarmPivots = 458;
	constexpr int32 WarmLiveAccepted = 340;

	/**
	 * Measured 2166.9018254298085, and the window is narrow rather than exact for the reason
	 * the sweep re-pinned five of its own over: a lambda* is the end of a pivot path. Two
	 * different paths reached this one - 186 pivots cold, 458 warm - and landed on the SAME
	 * BITS, so an exact pin would have held; a ~5e-9 window says the assertion is about the
	 * optimum and not about the arithmetic that walked to it.
	 */
	constexpr double WarmLiveLambdaLo = 2166.90182;
	constexpr double WarmLiveLambdaHi = 2166.90183;

	/**
	 * THE STRUCTURAL COLUMN COUNT, DERIVED FROM THE HEADER'S DOCUMENTED LAYOUT RATHER THAN
	 * READ FROM THE SOLVER: lambda, then four columns [n+, n-, p, q] per contact point, at
	 * two contact points per joint. Written out here so a reported shape that does not mean
	 * what this file thinks it means fails against an independent derivation instead of
	 * agreeing with itself.
	 */
	int32 WarmStructuralColumnsFor(const FOracleProblem& Problem)
	{
		return 1 + 4 * (2 * Problem.Joints.Num());
	}

	/* ================================================================================
	 * FIXTURE BUILDERS - production's own producers, transcribed rather than shared
	 * because the other oracle test files' helpers live in their own translation units.
	 * ================================================================================ */

	/** Lay a scenario row's structure and apply its cut - production data end to end. */
	bool WarmBuildScenario(const TCHAR* ScenarioName, FStructure& Out, FString& OutWhy)
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

	/** The acceptance wall producer at two smaller integers - 8 x 10 is the gate fixture. */
	bool WarmBuildIntactWall(int32 Courses, int32 Cells, FStructure& Out, FString& OutWhy)
	{
		DestructionWallCases::FWallSpec Spec;
		Spec.BrickSizeCm = FVector(WarmBrickLengthCm, WarmBrickWidthCm, WarmBrickHeightCm);
		Spec.JointThicknessCm = WarmJointCm;
		Spec.DensityGramsPerCubicCm = WarmClayDensityGramsPerCubicCm;
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

	/* ================================================================================
	 * ONE SOLVE, READ THE WAY PRODUCTION WOULD HAVE TO READ IT.
	 * ================================================================================ */

	struct FWarmReading
	{
		bool bAnswered = false;
		double Lambda = 0.0;
		int32 Pivots = 0;
		int32 PhaseOnePivots = INDEX_NONE;
		int32 Accepted = INDEX_NONE;
		double Seconds = 0.0;
		FString WhyNot;
		FOracleBasis FinalBasis;
	};

	FWarmReading WarmSolve(const FOracleProblem& Problem)
	{
		FWarmReading Out;

		const double Started = FPlatformTime::Seconds();
		const FOracleResult Result = SolveRigidBlock(Problem);
		Out.Seconds = FPlatformTime::Seconds() - Started;

		Out.bAnswered = Result.bAnswered;
		Out.Lambda = Result.Lambda;
		Out.Pivots = Result.SimplexIterations;
		Out.PhaseOnePivots = Result.PhaseOnePivots;
		Out.Accepted = Result.WarmStartColumnsAccepted;
		Out.WhyNot = Result.WhyNot;
		Out.FinalBasis = Result.FinalBasis;

		return Out;
	}

	/** Answered, and lambda at or above 1 - production's own reading of the verdict. */
	bool WarmIsFeasible(const FWarmReading& Reading)
	{
		return Reading.bAnswered && Reading.Lambda >= 1.0;
	}

	/* ================================================================================
	 * THE DELETION, AND THE ORDERING THAT MAKES A BASIS MAPPABLE ACROSS IT.
	 * ================================================================================ */

	enum class EWarmDelete : uint8
	{
		/** One brick out of the named course, nearest the wall's mid-span. */
		OneBrick,

		/** Every brick of the topmost course - an adjacent rung of a courses ladder. */
		TopCourse,

		/**
		 * Every joint onto the grounded course. The wall loses its foundation, so the
		 * remainder floats and the verdict FLIPS from stands to falls: the arm where the
		 * warm basis is not merely stale but describes a load path that no longer exists.
		 */
		GroundCourse,
	};

	/** Which blocks the deletion takes. Deterministic: lowest index wins every tie. */
	void WarmSelectDoomed(
		const FOracleProblem& Problem, EWarmDelete What, int32 Course, TArray<bool>& bDoomed)
	{
		bDoomed.Init(false, Problem.Blocks.Num());

		double LowestZ = TNumericLimits<double>::Max();
		double HighestZ = -TNumericLimits<double>::Max();
		double LowX = TNumericLimits<double>::Max();
		double HighX = -TNumericLimits<double>::Max();

		for (const FOracleBlock& Block : Problem.Blocks)
		{
			LowestZ = FMath::Min(LowestZ, Block.CentroidZCm);
			HighestZ = FMath::Max(HighestZ, Block.CentroidZCm);
			LowX = FMath::Min(LowX, Block.CentroidXCm);
			HighX = FMath::Max(HighX, Block.CentroidXCm);
		}

		if (What == EWarmDelete::GroundCourse)
		{
			for (int32 Index = 0; Index < Problem.Blocks.Num(); ++Index)
			{
				bDoomed[Index] = Problem.Blocks[Index].bGrounded;
			}

			return;
		}

		if (What == EWarmDelete::TopCourse)
		{
			for (int32 Index = 0; Index < Problem.Blocks.Num(); ++Index)
			{
				const double Height = Problem.Blocks[Index].CentroidZCm;
				bDoomed[Index] = FMath::Abs(Height - HighestZ) <= 0.5 * WarmCoursePitchCm;
			}

			return;
		}

		const double WantZ = LowestZ + double(Course) * WarmCoursePitchCm;
		const double WantX = 0.5 * (LowX + HighX);

		int32 Best = INDEX_NONE;
		double BestScore = TNumericLimits<double>::Max();

		for (int32 Index = 0; Index < Problem.Blocks.Num(); ++Index)
		{
			const FOracleBlock& Block = Problem.Blocks[Index];

			if (Block.bGrounded)
			{
				continue;
			}

			if (FMath::Abs(Block.CentroidZCm - WantZ) > 0.5 * WarmCoursePitchCm)
			{
				continue;
			}

			const double Score = FMath::Abs(Block.CentroidXCm - WantX);

			if (Score < BestScore)
			{
				BestScore = Score;
				Best = Index;
			}
		}

		if (Best != INDEX_NONE)
		{
			bDoomed[Best] = true;
		}
	}

	/**
	 * Move every joint the deletion will take to the END of the joint array, keeping the
	 * relative order of both halves. This is the whole reason a basis can be carried across
	 * the change: trailing joints mean trailing contacts, which mean trailing rows and
	 * columns, which mean every survivor keeps its ordinal.
	 */
	int32 WarmOrderDoomedJointsLast(FOracleProblem& Problem, const TArray<bool>& bDoomed)
	{
		TArray<FOracleJoint> Kept;
		TArray<FOracleJoint> Doomed;

		for (const FOracleJoint& Joint : Problem.Joints)
		{
			if (bDoomed[Joint.BlockA] || bDoomed[Joint.BlockB])
			{
				Doomed.Add(Joint);
			}
			else
			{
				Kept.Add(Joint);
			}
		}

		const int32 Count = Doomed.Num();
		Problem.Joints = MoveTemp(Kept);
		Problem.Joints.Append(Doomed);

		return Count;
	}

	/** The structure AFTER the deletion: the doomed joints gone, the doomed blocks inert. */
	FOracleProblem WarmApplyDeletion(
		const FOracleProblem& Before, const TArray<bool>& bDoomed, int32 DoomedJoints)
	{
		FOracleProblem After = Before;
		After.Joints.SetNum(After.Joints.Num() - DoomedJoints);

		for (int32 Index = 0; Index < After.Blocks.Num(); ++Index)
		{
			if (bDoomed[Index])
			{
				After.Blocks[Index].MassKg = 0.0;
			}
		}

		return After;
	}

	/**
	 * CARRY A BASIS ACROSS THE DELETION, by ordinal arithmetic on the two shapes and
	 * nothing else.
	 *
	 *   - a structural column survives with the same index, if its contact still exists;
	 *   - a slack keeps its ORDINAL among the inequality rows, which is preserved because
	 *     the rows that died are the trailing ones;
	 *   - an artificial belongs to a row and follows it, for the same reason.
	 *
	 * Anything with no image, and anything that would duplicate a column another row has
	 * already claimed, is left INDEX_NONE - "no hint for this row". A duplicate is not a
	 * near miss: two rows naming one column is a singular basis by inspection, and leaving
	 * the solver to discover it would be measuring the repair rather than the lever.
	 */
	FOracleBasis WarmMapBasis(const FOracleBasis& Before, const FOracleBasis& AfterShape)
	{
		FOracleBasis Out;
		Out.NumStructCols = AfterShape.NumStructCols;
		Out.ArtificialStart = AfterShape.ArtificialStart;
		Out.Columns.Init(INDEX_NONE, AfterShape.Columns.Num());

		const int32 AfterSlacks = AfterShape.ArtificialStart - AfterShape.NumStructCols;

		TSet<int32> Claimed;

		for (int32 Row = 0; Row < Out.Columns.Num(); ++Row)
		{
			if (Row >= Before.Columns.Num())
			{
				continue;
			}

			const int32 Was = Before.Columns[Row];
			int32 Now = INDEX_NONE;

			if (Was < 0)
			{
				Now = INDEX_NONE;
			}
			else if (Was < Before.NumStructCols)
			{
				Now = Was < Out.NumStructCols ? Was : INDEX_NONE;
			}
			else if (Was < Before.ArtificialStart)
			{
				const int32 Ordinal = Was - Before.NumStructCols;
				Now = Ordinal < AfterSlacks ? Out.NumStructCols + Ordinal : INDEX_NONE;
			}
			else
			{
				const int32 OwningRow = Was - Before.ArtificialStart;
				Now = OwningRow < Out.Columns.Num()
					? Out.ArtificialStart + OwningRow
					: INDEX_NONE;
			}

			if (Now != INDEX_NONE && !Claimed.Contains(Now))
			{
				Out.Columns[Row] = Now;
				Claimed.Add(Now);
			}
		}

		return Out;
	}

	/** How many rows the mapping could actually offer a hint for. */
	int32 WarmHintsIn(const FOracleBasis& Basis)
	{
		int32 Count = 0;

		for (const int32 Column : Basis.Columns)
		{
			if (Column != INDEX_NONE)
			{
				++Count;
			}
		}

		return Count;
	}

	/* ================================================================================
	 * ONE ROW OF THE MEASUREMENT.
	 * ================================================================================ */

	struct FWarmRow
	{
		const TCHAR* Name = nullptr;

		/** What revision 1 of the derivation record predicted, printed beside the result. */
		const TCHAR* Prediction = nullptr;

		TFunction<bool(FStructure&, FString&)> Build;

		EWarmDelete Delete = EWarmDelete::OneBrick;
		int32 Course = 0;

		/* ---- Fixture guards: what wall is this, and what did the deletion take? ---- */
		int32 Blocks = INDEX_NONE;
		int32 Joints = INDEX_NONE;
		int32 DoomedBlocks = INDEX_NONE;
		int32 DoomedJoints = INDEX_NONE;

		/* ---- The previous solve: the one whose basis is the warm start. ---- */
		bool bBeforeStands = true;
		int32 BeforePivots = WarmUnmeasured;

		/* ---- The re-solve, COLD: the baseline the lever is measured against. ---- */
		bool bAfterStands = true;
		int32 ColdPivots = WarmUnmeasured;
		int32 ColdPhaseOnePivots = WarmUnmeasured;

		/* ---- The re-solve, WARM. ---- */
		int32 WarmPivots = WarmUnmeasured;
		int32 WarmPhaseOnePivots = WarmUnmeasured;
		int32 WarmAccepted = WarmUnmeasured;

		/**
		 * THE WARM ATTEMPT WAS THROWN AWAY AND A COLD SOLVE ANSWERED INSTEAD - a THIRD
		 * outcome, and the reason this flag exists rather than a weaker acceptance assertion.
		 *
		 * WarmStartColumnsAccepted == 0 is overloaded across three distinct events: the
		 * seeding refused the hint outright, every hint was repaired away, or the warm attempt
		 * refused and the wrapper forced 0 after a fresh cold retry. The first two say "the
		 * lever was never pulled"; the third says "the lever was pulled and led into a dead
		 * end", which is a MEASUREMENT and the thing wall-01 is here to report. Weakening the
		 * acceptance row to >= 0 would erase all three distinctions at once, so instead the
		 * row DECLARES which outcome it expects and the assertions differ accordingly.
		 *
		 * Where this is true the row asserts Accepted == 0 exactly, AND pins WastedPivots -
		 * see below - which is what makes the flag a claim about the solver rather than a
		 * licence to skip an assertion.
		 */
		bool bWarmAbandoned = false;

		/**
		 * WARM PIVOTS MINUS COLD PIVOTS, on an abandoned row, and it is two assertions in one
		 * integer. The wrapper adds the discarded attempt's pivots to the cold retry's, so
		 * this difference IS the work the warm start wasted before it refused - the datum the
		 * design's attribution rests on and which is otherwise pinned nowhere. And because the
		 * retry is a fresh solve of the original problem, its own cost must be exactly the
		 * cold baseline: any drift in the difference says the retry was not a clean cold solve,
		 * which pinning the warm TOTAL alone could never distinguish from the waste moving.
		 */
		int32 WarmWastedPivots = WarmUnmeasured;
	};

	/**
	 * Run one row end to end and assert everything about it. Shared between the two tests
	 * because the fast tier and the wall-01 tier differ only in what they can afford, not
	 * in what they measure - and a second transcription of the protocol is how two tiers
	 * quietly measure two different things.
	 */
	void WarmRunRow(FAutomationTestBase& Test, const FWarmRow& Row)
	{
		FStructure Structure;
		FString Why;

		/*
		 * Its own statement before the message that reads it: folding a call that writes
		 * Why into the Printf that prints it is unsequenced, and MSVC evaluates the
		 * condition last (TRAPS).
		 */
		const bool bLaid = Row.Build(Structure, Why);

		if (!Test.TestTrue(
				*FString::Printf(TEXT("%s: the producer must lay it (it said: %s)"),
					Row.Name, *Why),
				bLaid))
		{
			return;
		}

		FOracleProblem Live;

		const bool bBridged = BuildRigidBlockProblem(Structure, Live, Why);

		if (!Test.TestTrue(
				*FString::Printf(TEXT("%s: the bridge must represent it (it said: %s)"),
					Row.Name, *Why),
				bBridged))
		{
			return;
		}

		/*
		 * THE POSE IS THE ONE PRODUCTION WILL RUN: gravity dead, no live load, so the
		 * question is feasibility at lambda = 1 rather than "how many times its own
		 * weight". PROMOTION_DESIGN §3.2, measured in slice 0b.
		 */
		FOracleProblem Before = Live;
		Before.bGravityIsLive = false;

		TArray<bool> bDoomed;
		WarmSelectDoomed(Before, Row.Delete, Row.Course, bDoomed);

		int32 DoomedBlocks = 0;

		for (const bool bIsDoomed : bDoomed)
		{
			DoomedBlocks += bIsDoomed ? 1 : 0;
		}

		const int32 DoomedJoints = WarmOrderDoomedJointsLast(Before, bDoomed);
		const FOracleProblem After = WarmApplyDeletion(Before, bDoomed, DoomedJoints);

		/* ---- The three solves. ---- */

		const FWarmReading BeforeRead = WarmSolve(Before);

		FOracleProblem AfterCold = After;
		const FWarmReading ColdRead = WarmSolve(AfterCold);

		FOracleProblem AfterWarm = After;
		AfterWarm.StartingBasis = WarmMapBasis(BeforeRead.FinalBasis, ColdRead.FinalBasis);
		const int32 Hints = WarmHintsIn(AfterWarm.StartingBasis);
		const FWarmReading WarmRead = WarmSolve(AfterWarm);

		const double PivotRatio = WarmRead.Pivots > 0
			? double(ColdRead.Pivots) / double(WarmRead.Pivots)
			: -1.0;
		const double SecondsRatio = WarmRead.Seconds > 0.0
			? ColdRead.Seconds / WarmRead.Seconds
			: -1.0;

		const FString Line = FString::Printf(
			TEXT("WARMSTART %s: blocks=%d joints=%d | deleted blocks=%d joints=%d ")
			TEXT("| BEFORE feasible=%d lambda=%.17g pivots=%d phase1=%d secs=%.3f ")
			TEXT("| AFTER COLD feasible=%d lambda=%.17g pivots=%d phase1=%d secs=%.3f ")
			TEXT("| AFTER WARM feasible=%d lambda=%.17g pivots=%d phase1=%d secs=%.3f ")
			TEXT("hints=%d accepted=%d ")
			TEXT("| RATIO pivots %.3f seconds %.3f | %s%s%s"),
			Row.Name, Live.Blocks.Num(), Live.Joints.Num(), DoomedBlocks, DoomedJoints,
			WarmIsFeasible(BeforeRead) ? 1 : 0, BeforeRead.Lambda, BeforeRead.Pivots,
			BeforeRead.PhaseOnePivots, BeforeRead.Seconds,
			WarmIsFeasible(ColdRead) ? 1 : 0, ColdRead.Lambda, ColdRead.Pivots,
			ColdRead.PhaseOnePivots, ColdRead.Seconds,
			WarmIsFeasible(WarmRead) ? 1 : 0, WarmRead.Lambda, WarmRead.Pivots,
			WarmRead.PhaseOnePivots, WarmRead.Seconds,
			Hints, WarmRead.Accepted,
			PivotRatio, SecondsRatio, Row.Prediction,
			WarmRead.WhyNot.IsEmpty() ? TEXT("") : TEXT(" | warm whynot: "),
			WarmRead.WhyNot.IsEmpty() ? TEXT("") : *WarmRead.WhyNot);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		Test.AddInfo(Line);

		/* ---- The size pins, first, so a row cannot secretly measure another wall. ---- */

		if (Row.Blocks == INDEX_NONE || Row.Joints == INDEX_NONE)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: UNMEASURED SIZE - pin blocks=%d joints=%d"),
				Row.Name, Live.Blocks.Num(), Live.Joints.Num()));
		}
		else
		{
			Test.TestEqual(*FString::Printf(TEXT("%s: block count"), Row.Name),
				Live.Blocks.Num(), Row.Blocks);
			Test.TestEqual(*FString::Printf(TEXT("%s: joint count"), Row.Name),
				Live.Joints.Num(), Row.Joints);
		}

		if (Row.DoomedBlocks == INDEX_NONE || Row.DoomedJoints == INDEX_NONE)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: UNMEASURED DELETION - pin blocks=%d joints=%d"),
				Row.Name, DoomedBlocks, DoomedJoints));
		}
		else
		{
			Test.TestEqual(*FString::Printf(TEXT("%s: blocks the deletion takes"), Row.Name),
				DoomedBlocks, Row.DoomedBlocks);
			Test.TestEqual(*FString::Printf(TEXT("%s: joints the deletion takes"), Row.Name),
				DoomedJoints, Row.DoomedJoints);
		}

		/* ---- The previous solve, which is what the warm start is made of. ---- */

		if (!Test.TestTrue(
				*FString::Printf(TEXT("%s: the BEFORE solve must answer (it said: %s)"),
					Row.Name, *BeforeRead.WhyNot),
				BeforeRead.bAnswered))
		{
			return;
		}

		/*
		 * THE POSE IS DEAD, AND THIS IS HOW A TEST KNOWS IT. With no live load the lambda
		 * column appears in no equilibrium row, so its only constraint is the cap: a
		 * feasible problem reports EXACTLY LambdaCap and an infeasible one EXACTLY 0. A
		 * live load leaking in would land lambda somewhere between and fire this.
		 */
		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: a DEAD pose carries no live load, so BEFORE lambda must be ")
				TEXT("exactly LambdaCap (%.17g) or exactly 0, and was %.17g"),
				Row.Name, LambdaCap, BeforeRead.Lambda),
			BeforeRead.Lambda == LambdaCap || BeforeRead.Lambda == 0.0);

		Test.TestTrue(
			*FString::Printf(TEXT("%s: the BEFORE verdict (feasible=%d, wanted %d)"),
				Row.Name, WarmIsFeasible(BeforeRead) ? 1 : 0, Row.bBeforeStands ? 1 : 0),
			WarmIsFeasible(BeforeRead) == Row.bBeforeStands);

		if (Row.BeforePivots == WarmUnmeasured)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: UNMEASURED BEFORE pivots - pin %d"), Row.Name, BeforeRead.Pivots));
		}
		else
		{
			Test.TestEqual(*FString::Printf(TEXT("%s: BEFORE pivots"), Row.Name),
				BeforeRead.Pivots, Row.BeforePivots);
		}

		/*
		 * AND IT MUST HAND BACK A BASIS. Without this the whole experiment is a pair of
		 * cold solves with a ceremony in between, which is exactly what it is until the
		 * seam exists - so this is the assertion that is red for the right reason today.
		 * The structural column count is DERIVED from the documented layout rather than
		 * read back from the solver, so a reported shape that means something else fails
		 * here instead of quietly mis-mapping every column below it.
		 */
		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: the BEFORE solve must report the basis it ended on (it reported ")
				TEXT("%d columns)"),
				Row.Name, BeforeRead.FinalBasis.Columns.Num()),
			BeforeRead.FinalBasis.Columns.Num() > 0);

		Test.TestEqual(
			*FString::Printf(TEXT("%s: the BEFORE basis names the right column layout"),
				Row.Name),
			BeforeRead.FinalBasis.NumStructCols, WarmStructuralColumnsFor(Before));

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: the BEFORE basis's slack block is non-negative (structural %d, ")
				TEXT("artificials start %d)"),
				Row.Name, BeforeRead.FinalBasis.NumStructCols,
				BeforeRead.FinalBasis.ArtificialStart),
			BeforeRead.FinalBasis.ArtificialStart >= BeforeRead.FinalBasis.NumStructCols);

		/* ---- The re-solve, cold: the baseline. ---- */

		if (!Test.TestTrue(
				*FString::Printf(TEXT("%s: the COLD re-solve must answer (it said: %s)"),
					Row.Name, *ColdRead.WhyNot),
				ColdRead.bAnswered))
		{
			return;
		}

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: COLD lambda must be exactly LambdaCap (%.17g) or exactly 0, and ")
				TEXT("was %.17g"),
				Row.Name, LambdaCap, ColdRead.Lambda),
			ColdRead.Lambda == LambdaCap || ColdRead.Lambda == 0.0);

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: the verdict after the deletion (feasible=%d, wanted %d)"),
				Row.Name, WarmIsFeasible(ColdRead) ? 1 : 0, Row.bAfterStands ? 1 : 0),
			WarmIsFeasible(ColdRead) == Row.bAfterStands);

		if (Row.ColdPivots == WarmUnmeasured || Row.ColdPhaseOnePivots == WarmUnmeasured)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: UNMEASURED COLD cost - pin pivots=%d phase1=%d"),
				Row.Name, ColdRead.Pivots, ColdRead.PhaseOnePivots));
		}
		else
		{
			Test.TestEqual(*FString::Printf(TEXT("%s: COLD pivots"), Row.Name),
				ColdRead.Pivots, Row.ColdPivots);
			Test.TestEqual(*FString::Printf(TEXT("%s: COLD phase-1 pivots"), Row.Name),
				ColdRead.PhaseOnePivots, Row.ColdPhaseOnePivots);
		}

		Test.TestEqual(
			*FString::Printf(TEXT("%s: the COLD re-solve names the right column layout"),
				Row.Name),
			ColdRead.FinalBasis.NumStructCols, WarmStructuralColumnsFor(After));

		/* ---- The warm re-solve. ---- */

		if (!Test.TestTrue(
				*FString::Printf(TEXT("%s: the WARM re-solve must answer (it said: %s)"),
					Row.Name, *WarmRead.WhyNot),
				WarmRead.bAnswered))
		{
			return;
		}

		/*
		 * THE ASSERTION THE WHOLE LEVER STANDS ON. A warm start is a starting point, not a
		 * modelling choice: it may change how long the answer takes and it may change which
		 * of several optimal bases the solver lands on, but it may NOT change the answer.
		 * Exact equality is legitimate here because the dead pose admits exactly two lambda
		 * values - see the file header - so this is a verdict comparison in a double's
		 * clothing rather than a comparison of two floating-point computations.
		 */
		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: WARM lambda must equal COLD lambda BIT FOR BIT (%.17g vs %.17g)"),
				Row.Name, WarmRead.Lambda, ColdRead.Lambda),
			WarmRead.Lambda == ColdRead.Lambda);

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: WARM verdict must equal COLD verdict (%d vs %d)"),
				Row.Name, WarmIsFeasible(WarmRead) ? 1 : 0, WarmIsFeasible(ColdRead) ? 1 : 0),
			WarmIsFeasible(WarmRead) == WarmIsFeasible(ColdRead));

		/*
		 * AND THE HINT MUST ACTUALLY HAVE BEEN TAKEN. A warm start that accepted nothing is
		 * a cold start wearing a hat, and it would report a pivot ratio of exactly 1.0 that
		 * reads like a refutation of the lever rather than like a lever that was never
		 * pulled. This is the assertion that tells those two apart.
		 *
		 * ON AN ABANDONED ROW THE SAME ZERO IS THE TRUTHFUL ANSWER, and it is asserted rather
		 * than excused. The warm attempt refused, the wrapper solved cold and forced the count
		 * to zero, so demanding > 0 here would be demanding the seam LIE - but demanding only
		 * >= 0 would assert nothing at all. So the abandoned arm asserts the zero EXACTLY and
		 * pins the wasted work beside it, which is a strictly stronger pair of statements than
		 * the standing arm's.
		 */
		if (Row.bWarmAbandoned)
		{
			Test.TestEqual(
				*FString::Printf(
					TEXT("%s: the warm attempt was ABANDONED, so the solve must report ")
					TEXT("EXACTLY ZERO columns accepted - the answer came from a fresh cold ")
					TEXT("solve and reporting the columns the discarded attempt briefly held ")
					TEXT("would credit the lever with work it did not do (it said %d of %d ")
					TEXT("offered)"),
					Row.Name, WarmRead.Accepted, Hints),
				WarmRead.Accepted, 0);

			const int32 Wasted = WarmRead.Pivots - ColdRead.Pivots;

			if (Row.WarmWastedPivots == WarmUnmeasured)
			{
				Test.AddError(FString::Printf(
					TEXT("%s: UNMEASURED WASTED WORK - pin warm-minus-cold pivots=%d"),
					Row.Name, Wasted));
			}
			else
			{
				Test.TestEqual(
					*FString::Printf(
						TEXT("%s: the pivots the abandoned warm attempt burned before it ")
						TEXT("refused, which is warm total minus the cold baseline. Pinned as ")
						TEXT("the DIFFERENCE and not as the total because the difference says ")
						TEXT("two things at once: how far the mapped basis got, and that the ")
						TEXT("retry underneath it was a genuine cold solve costing exactly the ")
						TEXT("cold baseline"),
						Row.Name),
					Wasted, Row.WarmWastedPivots);
			}
		}
		else
		{
			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: the solve must report how much of the warm start it took (it ")
					TEXT("said %d of %d offered)"),
					Row.Name, WarmRead.Accepted, Hints),
				WarmRead.Accepted > 0);
		}

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: it cannot accept more hints than were offered (%d of %d)"),
				Row.Name, WarmRead.Accepted, Hints),
			WarmRead.Accepted <= Hints);

		if (Row.WarmAccepted == WarmUnmeasured)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: UNMEASURED WARM acceptance - pin accepted=%d of %d offered"),
				Row.Name, WarmRead.Accepted, Hints));
		}
		else
		{
			Test.TestEqual(*FString::Printf(TEXT("%s: WARM columns accepted"), Row.Name),
				WarmRead.Accepted, Row.WarmAccepted);
		}

		/*
		 * THE MEASUREMENT ITSELF, PINNED IN BOTH HALVES. Phase 1 and phase 2 are pinned
		 * separately because reporting only phase 2 is how a warm start is made to look
		 * better than it is: a seeded basis that is infeasible for the new problem has to
		 * be REPAIRED, and the repair is phase-1 work. In this pose phase 2 is a single
		 * pivot taking lambda to its cap, so the total IS the repair - but the split is
		 * pinned anyway, because the day someone poses this live it stops being true.
		 */
		if (Row.WarmPivots == WarmUnmeasured || Row.WarmPhaseOnePivots == WarmUnmeasured)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: UNMEASURED WARM cost - pin pivots=%d phase1=%d (cold was %d/%d, ")
				TEXT("so the lever reads %.3f on pivots)"),
				Row.Name, WarmRead.Pivots, WarmRead.PhaseOnePivots,
				ColdRead.Pivots, ColdRead.PhaseOnePivots, PivotRatio));
		}
		else
		{
			Test.TestEqual(*FString::Printf(TEXT("%s: WARM pivots"), Row.Name),
				WarmRead.Pivots, Row.WarmPivots);
			Test.TestEqual(*FString::Printf(TEXT("%s: WARM phase-1 pivots"), Row.Name),
				WarmRead.PhaseOnePivots, Row.WarmPhaseOnePivots);
		}
	}
}

/* ====================================================================================
 * TEST 1 - THE SEAM ITSELF, AND THE FAST FIXTURES.
 *
 * PART A is the seam's own contract on the cheapest fixture there is: a solve's final basis,
 * fed straight back into the SAME problem, must be recognised as optimal and cost nothing.
 * It is the assertion that cannot be faked - a warm start that is quietly ignored takes the
 * cold pivot count, and a warm start that is accepted but mis-mapped moves lambda.
 *
 * PART C is the only row posed GRAVITY-LIVE, where lambda* is a real number rather than a
 * boolean in a double's clothes - the arm on which a warm start could shift an answer by a
 * quantity worth measuring, and the only one that could catch an appended column left basic
 * at a non-zero value slipping past the admissibility gate.
 *
 * PART B is the measurement: the 84-block gate fixture under three sizes of change, and a
 * 149-block wall under one.
 *
 * COST: measured in this file's own run and recorded here; it is a FAST-tier test.
 *
 * NEEDS A TICKING WORLD: NO.
 * ==================================================================================== */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleWarmStartTest,
	"OracleSweepFast.RigidBlock.WarmStartAfterADeletion",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleWarmStartTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace OracleWarmStartSupport;

	/* ================================================================================
	 * PART A - THE SEAM'S CONTRACT: A SOLVE'S OWN BASIS IS AN OPTIMAL START.
	 * ================================================================================ */
	{
		FStructure Small;
		FString Why;

		const bool bLaid = WarmBuildIntactWall(4, 4, Small, Why);

		if (TestTrue(
				*FString::Printf(TEXT("seam fixture: the producer must lay it (it said: %s)"),
					*Why),
				bLaid))
		{
			FOracleProblem Problem;

			const bool bBridged = BuildRigidBlockProblem(Small, Problem, Why);

			if (TestTrue(
					*FString::Printf(
						TEXT("seam fixture: the bridge must represent it (it said: %s)"), *Why),
					bBridged))
			{
				Problem.bGravityIsLive = false;

				const FWarmReading Cold = WarmSolve(Problem);

				const FString Line = FString::Printf(
					TEXT("WARMSTART seam 4x4 wall: blocks=%d joints=%d | COLD lambda=%.17g ")
					TEXT("pivots=%d phase1=%d basis columns=%d structcols=%d ")
					TEXT("artificialstart=%d"),
					Problem.Blocks.Num(), Problem.Joints.Num(), Cold.Lambda, Cold.Pivots,
					Cold.PhaseOnePivots, Cold.FinalBasis.Columns.Num(),
					Cold.FinalBasis.NumStructCols, Cold.FinalBasis.ArtificialStart);

				UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
				AddInfo(Line);

				TestTrue(
					*FString::Printf(TEXT("seam: the cold solve must answer (it said: %s)"),
						*Cold.WhyNot),
					Cold.bAnswered);

				/*
				 * THE FIXTURE GUARDS, measured 2026-08-16. A seam contract asserted about a
				 * problem that quietly became a different problem is the rung-flip trap
				 * (TRAPS): only a count pin catches it, and the pivot count is the cheapest
				 * statement that this is still the solve the contract is written about.
				 */
				TestEqual(TEXT("seam fixture: block count"), Problem.Blocks.Num(), 18);
				TestEqual(TEXT("seam fixture: joint count"), Problem.Joints.Num(), 35);
				TestEqual(TEXT("seam fixture: cold pivots"), Cold.Pivots, 48);
				TestEqual(TEXT("seam fixture: cold phase-1 pivots"), Cold.PhaseOnePivots, 47);

				/*
				 * NO WARM START WAS SUPPLIED, so the acceptance count must say exactly that
				 * rather than report a plausible zero: INDEX_NONE is "nobody asked", 0 is
				 * "asked and got nothing", and reading one as the other is how a lever gets
				 * credited with a saving it never made.
				 */
				TestEqual(
					TEXT("seam: a cold solve reports no warm-start acceptance at all"),
					Cold.Accepted, int32(INDEX_NONE));

				TestTrue(
					*FString::Printf(
						TEXT("seam: the solve must report the basis it ended on (it ")
						TEXT("reported %d columns)"),
						Cold.FinalBasis.Columns.Num()),
					Cold.FinalBasis.Columns.Num() > 0);

				TestEqual(
					TEXT("seam: the reported basis names the documented column layout"),
					Cold.FinalBasis.NumStructCols, WarmStructuralColumnsFor(Problem));

				/*
				 * AND THE IDEMPOTENCE CONTRACT. Handed its own optimal basis back, the
				 * solver has nothing left to do: phase 1 has no infeasibility to drive out
				 * and phase 2 can price no entering column, so the honest reading is ZERO
				 * pivots and a bit-identical lambda. It is pinned as an exact prediction
				 * rather than as a bound because a warm start that costs even one pivot on
				 * a problem it has already solved is describing something the seam does not
				 * yet understand about itself.
				 */
				FOracleProblem Again = Problem;
				Again.StartingBasis = Cold.FinalBasis;

				const FWarmReading Warm = WarmSolve(Again);

				const FString WarmLine = FString::Printf(
					TEXT("WARMSTART seam 4x4 wall RE-SOLVED FROM ITS OWN BASIS: ")
					TEXT("lambda=%.17g pivots=%d phase1=%d accepted=%d of %d"),
					Warm.Lambda, Warm.Pivots, Warm.PhaseOnePivots, Warm.Accepted,
					Cold.FinalBasis.Columns.Num());

				UE_LOG(LogTemp, Display, TEXT("%s"), *WarmLine);
				AddInfo(WarmLine);

				TestTrue(
					*FString::Printf(TEXT("seam: the warm solve must answer (it said: %s)"),
						*Warm.WhyNot),
					Warm.bAnswered);

				TestTrue(
					*FString::Printf(
						TEXT("seam: re-solving from its own basis must give the same lambda ")
						TEXT("BIT FOR BIT (%.17g vs %.17g)"),
						Warm.Lambda, Cold.Lambda),
					Warm.Lambda == Cold.Lambda);

				TestEqual(
					TEXT("seam: its own basis is entirely usable, so every column is taken"),
					Warm.Accepted, Cold.FinalBasis.Columns.Num());

				TestEqual(
					TEXT("seam: an optimal start costs no pivots at all"),
					Warm.Pivots, 0);

				TestEqual(
					TEXT("seam: and none of them in phase 1"),
					Warm.PhaseOnePivots, 0);
			}
		}
	}

	/* ================================================================================
	 * PART C - THE ONE ROW POSED GRAVITY-LIVE, WHERE LAMBDA IS A REAL NUMBER.
	 *
	 * WHY IT EXISTS, and it is a hole every other row in this file shares. In the DEAD pose
	 * the lambda column appears in no equilibrium row, so lambda can only be exactly
	 * LambdaCap or exactly 0 - which makes every "warm lambda equals cold lambda BIT FOR BIT"
	 * assertion in this file a VERDICT comparison in a double's clothing. Those rows would
	 * pass unchanged if a warm start were quietly perturbing the answer, provided it perturbed
	 * it by less than the distance from 0 to LambdaCap.
	 *
	 * The lambda*-MAXIMISING arm is where a warm start could move an answer by a real number:
	 * phase 2 climbs lambda from 0 to the optimum, and a seeded basis reaches that optimum
	 * along a different path. It is also the only place the seam's APPENDED NEGATED COLUMNS
	 * could show themselves - one left basic at a non-zero value contributes to no original
	 * row, so it is invisible to the admissibility gate that verifies the answer, and only a
	 * lambda disagreeing with the cold solve's would say so.
	 *
	 * Cheap: the 18-block seam wall, three live solves, milliseconds.
	 *
	 * THE TOLERANCE IS A WINDOW AND NOT A BIT-IDENTITY, and that is not a weakening. Two
	 * different pivot paths to the same optimum of the same polytope agree to rounding rather
	 * than to the last bit - the sweep has re-pinned windows over exactly this before - so a
	 * bit-identity here would be asserting the pivot path rather than the answer. 1e-6
	 * relative is the sweep's own window.
	 * ================================================================================ */
	{
		FStructure Live;
		FString Why;

		const bool bLaid = WarmBuildIntactWall(4, 4, Live, Why);

		if (TestTrue(
				*FString::Printf(TEXT("live seam: the producer must lay it (it said: %s)"), *Why),
				bLaid))
		{
			FOracleProblem Intact;

			const bool bBridged = BuildRigidBlockProblem(Live, Intact, Why);

			if (TestTrue(
					*FString::Printf(
						TEXT("live seam: the bridge must represent it (it said: %s)"), *Why),
					bBridged))
			{
				/* THE ONE DIFFERENCE FROM EVERY OTHER ROW IN THIS FILE. */
				Intact.bGravityIsLive = true;

				TArray<bool> bDoomed;
				WarmSelectDoomed(Intact, EWarmDelete::OneBrick, 1, bDoomed);

				int32 DoomedBlocks = 0;

				for (const bool bIsDoomed : bDoomed)
				{
					DoomedBlocks += bIsDoomed ? 1 : 0;
				}

				const int32 DoomedJoints = WarmOrderDoomedJointsLast(Intact, bDoomed);
				const FOracleProblem After = WarmApplyDeletion(Intact, bDoomed, DoomedJoints);

				const FWarmReading BeforeRead = WarmSolve(Intact);

				FOracleProblem AfterCold = After;
				const FWarmReading ColdRead = WarmSolve(AfterCold);

				FOracleProblem AfterWarm = After;
				AfterWarm.StartingBasis =
					WarmMapBasis(BeforeRead.FinalBasis, ColdRead.FinalBasis);
				const int32 Hints = WarmHintsIn(AfterWarm.StartingBasis);
				const FWarmReading WarmRead = WarmSolve(AfterWarm);

				const double Gap = FMath::Abs(WarmRead.Lambda - ColdRead.Lambda);
				const double Relative = ColdRead.Lambda != 0.0
					? Gap / FMath::Abs(ColdRead.Lambda)
					: Gap;

				const FString Line = FString::Printf(
					TEXT("WARMSTART LIVE seam 4x4 wall: blocks=%d joints=%d | deleted blocks=%d ")
					TEXT("joints=%d | BEFORE lambda*=%.17g pivots=%d | AFTER COLD lambda*=%.17g ")
					TEXT("pivots=%d phase1=%d | AFTER WARM lambda*=%.17g pivots=%d phase1=%d ")
					TEXT("hints=%d accepted=%d | lambda gap %.3g absolute, %.3g relative"),
					Intact.Blocks.Num(), Intact.Joints.Num(), DoomedBlocks, DoomedJoints,
					BeforeRead.Lambda, BeforeRead.Pivots,
					ColdRead.Lambda, ColdRead.Pivots, ColdRead.PhaseOnePivots,
					WarmRead.Lambda, WarmRead.Pivots, WarmRead.PhaseOnePivots,
					Hints, WarmRead.Accepted, Gap, Relative);

				UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
				AddInfo(Line);

				TestEqual(TEXT("live seam: block count"), Intact.Blocks.Num(), 18);
				TestEqual(TEXT("live seam: joint count"), Intact.Joints.Num(), 35);
				TestEqual(TEXT("live seam: blocks the deletion takes"), DoomedBlocks, 1);
				TestEqual(TEXT("live seam: joints the deletion takes"),
					DoomedJoints, WarmLiveDoomedJoints);

				TestTrue(
					*FString::Printf(
						TEXT("live seam: the BEFORE solve must answer (it said: %s)"),
						*BeforeRead.WhyNot),
					BeforeRead.bAnswered);

				TestTrue(
					*FString::Printf(
						TEXT("live seam: the COLD re-solve must answer (it said: %s)"),
						*ColdRead.WhyNot),
					ColdRead.bAnswered);

				TestTrue(
					*FString::Printf(
						TEXT("live seam: the WARM re-solve must answer (it said: %s)"),
						*WarmRead.WhyNot),
					WarmRead.bAnswered);

				/*
				 * THE PRECONDITION THAT STOPS THIS ROW QUIETLY BECOMING ANOTHER DEAD ONE. If
				 * lambda* ever lands on 0 or on the cap, the pose has stopped exercising the
				 * maximising arm and the agreement below would be the same boolean comparison
				 * the dead rows already make - green, and about nothing.
				 */
				TestTrue(
					*FString::Printf(
						TEXT("live seam: lambda* must be a REAL NUMBER strictly inside ")
						TEXT("(0, LambdaCap=%.17g) - that is the whole reason this row exists, ")
						TEXT("and it read %.17g"),
						LambdaCap, ColdRead.Lambda),
					ColdRead.Lambda > 0.0 && ColdRead.Lambda < LambdaCap);

				/*
				 * THE ASSERTION THE ROW IS FOR. A warm start may choose a different path to
				 * the optimum; it may not choose a different optimum.
				 */
				TestTrue(
					*FString::Printf(
						TEXT("live seam, THE POINT: a warm start may change the PATH and not ")
						TEXT("the ANSWER - warm lambda* %.17g against cold %.17g is %.3g ")
						TEXT("relative, and must be within 1e-6"),
						WarmRead.Lambda, ColdRead.Lambda, Relative),
					Relative <= 1.0e-6);

				TestTrue(
					*FString::Printf(
						TEXT("live seam: and the hint must actually have been taken (it said ")
						TEXT("%d of %d offered)"),
						WarmRead.Accepted, Hints),
					WarmRead.Accepted > 0);

				/*
				 * THE COST PINS. Without them the agreement above is also satisfied by a warm
				 * start that was thrown away, which is the one outcome this file exists to
				 * tell apart from the others.
				 */
				if (WarmLiveColdPivots == WarmUnmeasured)
				{
					AddError(FString::Printf(
						TEXT("live seam: UNMEASURED - pin doomed joints=%d cold pivots=%d ")
						TEXT("warm pivots=%d accepted=%d, and a lambda* window around %.17g"),
						DoomedJoints, ColdRead.Pivots, WarmRead.Pivots, WarmRead.Accepted,
						ColdRead.Lambda));
				}
				else
				{
					TestEqual(TEXT("live seam: COLD pivots"),
						ColdRead.Pivots, WarmLiveColdPivots);
					TestEqual(TEXT("live seam: WARM pivots"),
						WarmRead.Pivots, WarmLiveWarmPivots);
					TestEqual(TEXT("live seam: WARM columns accepted"),
						WarmRead.Accepted, WarmLiveAccepted);

					TestTrue(
						*FString::Printf(
							TEXT("live seam: lambda* must lie in [%.9g, %.9g] and was %.17g"),
							WarmLiveLambdaLo, WarmLiveLambdaHi, ColdRead.Lambda),
						ColdRead.Lambda >= WarmLiveLambdaLo
							&& ColdRead.Lambda <= WarmLiveLambdaHi);
				}
			}
		}
	}

	/* ================================================================================
	 * PART B - THE MEASUREMENT.
	 * ================================================================================ */

	TArray<FWarmRow> Rows;

	/*
	 * EVERY ROW CARRIES ITS PREDICTION AND ITS MEASUREMENT SIDE BY SIDE, permanently: a
	 * prediction deleted once it has been checked leaves nobody able to tell an agreement
	 * from a transcription. The COLD columns were measured 2026-08-16; the WARM ones are
	 * WarmUnmeasured and are this file's red.
	 */

	Rows.Add({ TEXT("gate wall 8x10, ONE BRICK out of course 3"),
		TEXT("PREDICTED 1.6x-5x on pivots: (A) local repair says 5x, (B) thrust-is-not-local ")
		TEXT("says 1.6x. This is the row the gate arithmetic rests on - the cold re-solve is ")
		TEXT("343 pivots and ~51 ms against a 50 ms budget"),
		[](FStructure& Out, FString& Why) { return WarmBuildIntactWall(8, 10, Out, Why); },
		EWarmDelete::OneBrick, 3,
		/*Blocks*/ 84, /*Joints*/ 207, /*DoomedBlocks*/ 1, /*DoomedJoints*/ 6,
		/*bBeforeStands*/ true, /*BeforePivots*/ 436,
		/*bAfterStands*/ true, /*ColdPivots*/ 343, /*ColdPhaseOnePivots*/ 342,
		/*WarmPivots*/ 342, /*WarmPhaseOnePivots*/ 342, /*WarmAccepted*/ 2600 });

	Rows.Add({ TEXT("gate wall 8x10, the WHOLE TOP COURSE out"),
		TEXT("PREDICTED 1.2x-2.5x: eleven bricks is a large change, and this is also the ")
		TEXT("roadmap's neighbouring-ladder-rung case (an 8-course wall re-solved as a ")
		TEXT("7-course one) posed as a deletion so the basis can be mapped. Its BEFORE solve ")
		TEXT("is the reorder's own control: 491 pivots, bit-identical to the intact wall's ")
		TEXT("pinned dead-pose count, because this row's reorder is a no-op"),
		[](FStructure& Out, FString& Why) { return WarmBuildIntactWall(8, 10, Out, Why); },
		EWarmDelete::TopCourse, 0,
		/*Blocks*/ 84, /*Joints*/ 207, /*DoomedBlocks*/ 11, /*DoomedJoints*/ 30,
		/*bBeforeStands*/ true, /*BeforePivots*/ 491,
		/*bAfterStands*/ true, /*ColdPivots*/ 291, /*ColdPhaseOnePivots*/ 290,
		/*WarmPivots*/ 1543, /*WarmPhaseOnePivots*/ 1543, /*WarmAccepted*/ 2032 });

	Rows.Add({ TEXT("gate wall 8x10, the GROUND CUT - the verdict flips to falls"),
		TEXT("PREDICTED 0.7x-1.5x, and this is THE TRAP ROW: the warm basis describes a load ")
		TEXT("path that no longer exists, so a warm start may cost MORE than a cold one. It ")
		TEXT("is also the only row whose AFTER verdict is 'falls', which is what stops the ")
		TEXT("bit-identity assertion resting on a set of fixtures that all stand"),
		[](FStructure& Out, FString& Why) { return WarmBuildIntactWall(8, 10, Out, Why); },
		EWarmDelete::GroundCourse, 0,
		/*Blocks*/ 84, /*Joints*/ 207, /*DoomedBlocks*/ 10, /*DoomedJoints*/ 20,
		/*bBeforeStands*/ true, /*BeforePivots*/ 442,
		/*bAfterStands*/ false, /*ColdPivots*/ 90, /*ColdPhaseOnePivots*/ 90,
		/*WarmPivots*/ 77, /*WarmPhaseOnePivots*/ 77, /*WarmAccepted*/ 2441 });

	Rows.Add({ TEXT("12x12 wall, ONE BRICK out of course 6"),
		TEXT("PREDICTED 2.5x-6x. The wall the regional sandwich measured and the same course ")
		TEXT("it cut, so the two levers are priced on one fixture - intact here at 150 ")
		TEXT("blocks where the sandwich starts from the cut wall's 149"),
		[](FStructure& Out, FString& Why) { return WarmBuildIntactWall(12, 12, Out, Why); },
		EWarmDelete::OneBrick, 6,
		/*Blocks*/ 150, /*Joints*/ 391, /*DoomedBlocks*/ 1, /*DoomedJoints*/ 6,
		/*bBeforeStands*/ true, /*BeforePivots*/ 940,
		/*bAfterStands*/ true, /*ColdPivots*/ 919, /*ColdPhaseOnePivots*/ 918,
		/*WarmPivots*/ 4192, /*WarmPhaseOnePivots*/ 4192, /*WarmAccepted*/ 4560 });

	for (const FWarmRow& Row : Rows)
	{
		WarmRunRow(*this, Row);
	}

	return true;
}

/* ====================================================================================
 * TEST 2 - WALL-01, THE HEADLINE FIXTURE.
 *
 * 375 blocks, and the fixture the whole latency argument is stated about: it answers
 * feasibility COLD in ~26 s, which is 780x a frame, and scenario scale is ~3.3x more
 * structure again. If warm starts are going to close anything, this is where it shows.
 *
 * COST: three solves of a 375-block problem. It is a FULL-tier test of its own so the fast
 * tier stays usable for iteration, and it is the only thing this slice adds to the slow
 * tier - stated plainly rather than grown quietly, per the tier's own rot warning in TRAPS.
 *
 * NEEDS A TICKING WORLD: NO.
 * ==================================================================================== */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleWarmStartAtWallScaleTest,
	"OracleSweepFull.RigidBlock.WarmStartAtWallScale",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleWarmStartAtWallScaleTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle;
	using namespace OracleWarmStartSupport;

	FWarmRow Row;
	Row.Name = TEXT("wall-01 thirty courses, ONE BRICK out of course 3");
	Row.Prediction =
		TEXT("PREDICTED 5x-18x on pivots against a cold 5,407 - the fixture that separates ")
		TEXT("(A) local repair from (B) global repair most sharply, because the change is ")
		TEXT("the same size as the gate wall's while the structure is 4.5x bigger");
	Row.Build = [](FStructure& Out, FString& Why)
	{
		return WarmBuildScenario(TEXT("wall-01"), Out, Why);
	};
	Row.Delete = EWarmDelete::OneBrick;
	Row.Course = 3;

	/* Measured 2026-08-16: three solves, 24.5 + 15.5 + 15.6 s. */
	Row.Blocks = 375;
	Row.Joints = 1030;
	Row.DoomedBlocks = 1;
	Row.DoomedJoints = 6;
	Row.bBeforeStands = true;
	Row.BeforePivots = 5643;
	Row.bAfterStands = true;
	Row.ColdPivots = 4764;
	Row.ColdPhaseOnePivots = 4763;

	/*
	 * AND THE HEADLINE FIXTURE IS THE ONE THAT ABANDONS. 12,459 of 13,362 mapped columns were
	 * offered and the seeded basis went SINGULAR at the first periodic refactorisation, sixty-
	 * four pivots in, so the solver refused, threw the whole start away and answered from a
	 * fresh cold solve - 4,764 pivots, bit-identical to the cold baseline beside it, plus the
	 * 64 the dead end cost. Accepted is therefore ZERO and that zero is the truth rather than a
	 * missing measurement, which is what bWarmAbandoned declares.
	 *
	 * WHY THE 64 IS PINNED AND NOT MERELY REPORTED: it is the whole of the evidence behind the
	 * design's attribution - the mapped basis is fragile at this scale, in a way a dual simplex
	 * starting from the SAME basis would inherit rather than escape - and until this pin it
	 * lived only in a source comment.
	 */
	Row.WarmPivots = 4828;
	Row.WarmPhaseOnePivots = 4827;
	Row.WarmAccepted = 0;
	Row.bWarmAbandoned = true;
	Row.WarmWastedPivots = 64;

	WarmRunRow(*this, Row);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
