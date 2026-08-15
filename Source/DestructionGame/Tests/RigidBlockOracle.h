// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Core/ConnectionStrength.h"
#include "Core/Structure.h"

/**
 * THE RIGID-BLOCK LIMIT-ANALYSIS ORACLE — TEST SUPPORT ONLY, AND THAT IS A RULE, NOT A
 * PHASE. DESIGN.md §7 evolution step 3: everything in this header and its .cpp lives
 * under Tests/, ships nowhere, and is called by nothing in production. Its whole value
 * is being an INDEPENDENTLY DERIVED second opinion the fixture catalogue can be diffed
 * against; the moment it borrows production's routing, tiers, arching groups or
 * composite depth it stops being able to catch them being wrong.
 *
 * WHAT IT COMPUTES. The lower-bound (static / safe) theorem of limit analysis over
 * rigid blocks in the X-Z plane: does there exist ANY system of joint contact forces in
 * equilibrium with lambda times the live loads (gravity, by default) that violates no
 * strength constraint? The largest such lambda is the LOAD FACTOR lambda* — the answer
 * is a MEASUREMENT, not a verdict: lambda* >= 1 stands, below it falls, and the margin
 * is the number the acceptance rulings have never had.
 *
 * THE FORMULATION, with its citations:
 *
 *   - Rigid blocks, forces at joint contact points (Livesley 1978, "Limit analysis of
 *     structures formed from rigid blocks"; Gilbert & Melbourne 1994 extend it to
 *     multi-ring arches; Lourenco's simplified micro-model is the same idealisation —
 *     rigid units, all deformation and failure in zero-thickness interfaces, which is
 *     exactly what FConnection already is). Each rectangular joint is discretised as
 *     TWO CONTACT POINTS at the ends of its in-plane segment — the standard rigid-block
 *     discretisation, exact for straight-line normal-stress distributions and equal to
 *     the fully-plastic stress block in bending.
 *
 *   - Per contact point k: a normal force n_k (positive in COMPRESSION, matching the
 *     project's sign convention that the interface normal points A to B and the stored
 *     force acts on B) and a shear force v_k along the in-plane tangent.
 *
 *   - Per non-grounded block: three equilibrium EQUALITIES (sum Fx, sum Fz, sum of
 *     moments about the block's own centroid). Grounded blocks are the earth: they
 *     balance by definition and contribute no rows, exactly as FStructure's grounded
 *     flag terminates the flow of load.
 *
 *   - Per contact point, the strength INEQUALITIES, every one converted from the
 *     profile's SI megapascals over the contact's tributary area A/2:
 *
 *         tension     n_k >= -f_t * Conv * A/2      the FINITE-TENSION MAPPING
 *         friction    |v_k| <= c * Conv * A/2 + mu * n_k
 *         crushing    n_k <= f_c * Conv * A/2
 *         ceiling     |v_k| <= f_v,max * Conv * A/2     (only when the profile caps it)
 *
 *   - Objective: MAXIMISE lambda, the single multiplier on the live loads. Gravity is
 *     live by default ("how many times its own weight could this carry"); a problem may
 *     mark gravity dead and supply explicit live forces instead, which is what a pushed
 *     block or an eccentric surcharge needs and what classic limit analysis calls the
 *     dead/live split. Lambda is capped at LambdaCap so a structure whose constraints
 *     are all homogeneous (dry stone: every capacity scales with load) reports the cap
 *     rather than an unbounded LP.
 *
 * THE FINITE-TENSION MAPPING, JUSTIFIED. Classic rigid-block analysis (Heyman) is
 * no-tension; this project's mortar carries a real f_xk1 and the case-12 ruling showed
 * the bond term DECIDES real verdicts, so the oracle cannot ignore it. The mapping is a
 * rigid-plastic tension cut-off per contact point, n_k >= -f_t * A/2 — Lourenco's
 * interface tension cut-off in LP clothing — chosen because it reduces EXACTLY to
 * classic no-tension when f_t = 0 (dry stone's exact zero), the same reduction-by-data
 * pattern that keeps mu = 0 fasteners out of the code paths. Two consequences to keep
 * in mind when diffing against production:
 *
 *   - CAPACITY IS READ AT THE PLASTIC LIMIT, NOT FIRST CRACK. Concentrating the
 *     tension resultant at the edge contact point reads up to 3x the uncracked-elastic
 *     first-crack moment (f*b*d^2/2 against f*b*d^2/6). That is the honest lower-bound
 *     theorem — "is there ANY admissible equilibrium" — where production asks "does the
 *     elastic path crack". A structure the oracle fails is failed under the MOST
 *     charitable reading; a structure it stands may still crack first in the elastic
 *     model. The leaning-stack validation ladder pins both directions.
 *
 *   - THE FRICTION ROW IS THE LINEARISED ASSOCIATIVE FORM: |v| <= cA + mu*n with n
 *     SIGNED, so a contact in tension has its cohesion eroded by mu*|tension|. That is
 *     the standard rigid-block linearisation (Livesley, Gilbert) and is CONSERVATIVE
 *     relative to production's rule ("tension buys no friction but keeps cohesion").
 *     The known honesty caveat of the whole method rides along with it: with Coulomb
 *     friction the static theorem is not strictly a bound (non-associative flow), which
 *     is stated here once rather than hidden.
 *
 * UNITS. The oracle derives its own MPa-to-force conversion, DELIBERATELY NOT
 * DestructionForce::ForceUnitsPerMPaSqCm: an oracle that imports the constant it is
 * supposed to check would agree with a wrong value instead of failing against it. Same
 * convention as every acceptance test's independent derivation.
 *
 * DETERMINISM IS A CONTRACT. The solver is a SPARSE REVISED two-phase primal simplex
 * (rewritten 2026-08-12; the original dense tableau accumulated one rounding per cell
 * per pivot and its own verification refused every fixture past ~4,000 pivots): the
 * constraint matrix is held untouched in sparse column form, the basis as an LU
 * factorisation (left-looking, partial pivoting by magnitude with lowest-index ties)
 * plus a product-form eta file, REFACTORISED FROM THE ORIGINAL CLEAN DATA on a fixed
 * cadence — which both bounds the iteration cost and RESETS accumulated error, the
 * property the dense method structurally lacked. Pricing is a CANDIDATE LIST over a
 * rotating window (a bounded slice of columns priced per refill, the best few by the
 * static steepest-edge ratio d_j / ||A_j|| queued, every queued candidate re-priced
 * against the current duals before it may be chosen, and a full sweep of every column
 * required before optimality may be claimed — exact reduced costs from a fresh BTRAN
 * throughout, the ratio ranking them and never deciding candidacy) with a largest-pivot
 * tie-break in the ratio test, falling back to Bland's rule over a FULL scan for the
 * ENTERING choice only after a 500-pivot degenerate streak, over row and column orders
 * fixed by the input arrays. Every tie-break is index-based — no randomness, no time,
 * no hashing — so the same problem gives bit-identical lambda* on every run, which is
 * what lets a fixture-sweep diff be re-run and trusted. Because the fallback's LEAVING choice is
 * not Bland's, the classical no-cycling theorem does NOT apply; termination is
 * guaranteed by the fail-closed iteration cap, which reports failure rather than a
 * number. Do not remove the cap as "redundant" — it is the termination proof. A
 * redundant row can leave an artificial parked BASIC AT ZERO after phase 1; the
 * pivot-out pass immediately after phase 1 clears every one a real column can reach,
 * and the dense solver's recorded basic-artificial residue is contained by that pass
 * together with the post-solve verification gate, which fails closed rather than
 * certifying a basis it cannot check — not by any discipline inside phase 2 itself,
 * since pricing there prices every real column exactly against the final duals before
 * it may stop, and complementary slackness certifies the optimum regardless of what a
 * zero-value artificial sits on.
 *
 * FAIL CLOSED, EVERYWHERE. A problem the oracle cannot validate — NaN anywhere, a
 * non-unit normal, a nonsense index, a structure whose geometry is incomplete — returns
 * bAnswered = false, and OutcomeOf maps that to Unanswerable, which is the ZERO
 * enumerator so a zero-filled verdict can never claim a structure stands. Unlike
 * production's documented NaN-ceiling laundering hazard, a NaN MaxShearStrengthMPa here
 * is REFUSED rather than silently treated as uncapped.
 *
 * SCOPE HONESTY. 2D means the X-Z plane: Y is the wythe and enters only through joint
 * areas. Every current fixture is single-wythe and laid in X-Z; a joint whose normal
 * has a Y component is refused rather than projected. Memory is O(nonzeros + basis
 * fill), so every fixture the project owns — the ~375-piece 30-course walls included —
 * is representable; the measured answering envelope per fixture lives in
 * RigidBlockOracleSweepTest.cpp's headers, and this is still a test oracle, not a
 * shipping solver.
 */
namespace RigidBlockOracle
{
	/**
	 * 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa over 1 cm2 is 10000 uu. Derived here
	 * independently so the oracle FAILS against a wrong production constant instead of
	 * agreeing with it.
	 */
	constexpr double OracleForceUnitsPerMPaSqCm = 100.0 * 100.0;

	/** MassKg * 980 IS the weight in uu — the newton conversion is already inside it. */
	constexpr double OracleGravityCmPerSecondSquared = 980.0;

	/**
	 * Where lambda stops being measured. A structure whose every constraint scales with
	 * its load (dry stone under gravity) is feasible at EVERY lambda, and an unbounded
	 * LP is not an answer; the cap turns "stands however hard you press" into a number.
	 * Any lambda* at the cap means exactly that and nothing finer.
	 */
	constexpr double LambdaCap = 1.0e6;

	/** A rigid block: a mass, where it acts, and whether it is the earth. */
	struct FOracleBlock
	{
		double MassKg = 0.0;
		double CentroidXCm = 0.0;
		double CentroidZCm = 0.0;

		/** A grounded block balances by definition: no equilibrium rows are written. */
		bool bGrounded = false;
	};

	/**
	 * A joint between two blocks, as a 2D contact segment plus the profile strengths.
	 *
	 * The normal points BlockA to BlockB, matching FConnection; contact forces are the
	 * forces on BlockB, and BlockA carries the exact negation. The segment's two contact
	 * points sit at Centre -/+ HalfLength along the in-plane tangent (-Nz, Nx), in that
	 * order — the ordering is part of the determinism contract, not physics.
	 *
	 * AreaSqCm is the full 3D face area: the wythe direction exists only here, exactly
	 * as it does in production's stress arithmetic.
	 */
	struct FOracleJoint
	{
		int32 BlockA = INDEX_NONE;
		int32 BlockB = INDEX_NONE;

		/** Unit normal in the X-Z plane. Refused if not unit length or not finite. */
		double NormalX = 0.0;
		double NormalZ = 0.0;

		double CentreXCm = 0.0;
		double CentreZCm = 0.0;

		/**
		 * Half the contact segment's in-plane length, cm. ZERO IS A POINT CONTACT — both
		 * contact points coincide and the joint carries no moment — which is a legitimate
		 * joint, not a degenerate one, mirroring production's zero-extent convention.
		 */
		double HalfLengthCm = 0.0;

		double AreaSqCm = 0.0;

		/** The profile row, SI megapascals, read exactly as production stores it. */
		FConnectionStrength Strength;
	};

	/**
	 * An explicit external force on one block, for the problems gravity alone cannot
	 * pose: a pushed block, an eccentric surcharge. Applied at a point, so its moment
	 * about the block's centroid is (At - Centroid) x Force and is derived, never
	 * supplied separately.
	 */
	struct FOracleAppliedForce
	{
		int32 Block = INDEX_NONE;
		double ForceXUu = 0.0;
		double ForceZUu = 0.0;
		double AtXCm = 0.0;
		double AtZCm = 0.0;

		/** Live forces scale with lambda; dead forces are constants of the problem. */
		bool bLive = true;
	};

	struct FOracleProblem
	{
		TArray<FOracleBlock> Blocks;
		TArray<FOracleJoint> Joints;
		TArray<FOracleAppliedForce> AppliedForces;

		/**
		 * TRUE: gravity is the live load and lambda* answers "how many times its own
		 * weight". FALSE: gravity is dead and lambda scales only the live applied
		 * forces — the classic dead/live split a sliding or surcharge problem needs.
		 */
		bool bGravityIsLive = true;
	};

	/**
	 * The verdict an FOracleResult maps to. Unanswerable is the ZERO enumerator so an
	 * absent or zero-filled answer can never read as a structure standing — the same
	 * fail-closed polarity as EPieceSupport::Falling.
	 */
	enum class EOracleOutcome : uint8
	{
		Unanswerable,
		Falls,
		Stands,
	};

	/**
	 * WHY THE ORACLE REFUSED, as a value rather than a sentence. None is the ZERO
	 * enumerator, so a default-constructed or zero-filled result reads "no reason", which
	 * is the only reading consistent with bAnswered's own zero-is-refused polarity being
	 * checked against it rather than duplicated by it.
	 *
	 * THE THREE PhaseTwo ARMS ARE THE POINT. Until 2026-08-15 all three — a hit iteration
	 * cap, a spurious unbounded ray and a numerical failure — arrived as the one sentence
	 * "phase-2 simplex failed", so telling them apart had twice required an instrumented
	 * build. They are different events with different fixes, and under the promotion design
	 * production must be able to COUNT refusals by reason (§5.6, §11 R4), which a sentence
	 * is a poor thing to count by.
	 *
	 * The sentences still CONTAIN the old wording (RigidBlockOracle.cpp's RefusalText says
	 * why), so the split names the arm without renaming the event. The test that drives it
	 * is OracleSlowSweep.RigidBlock.RefusalNamesItsReason.
	 */
	enum class EOracleRefusal : uint8
	{
		/** The oracle answered. */
		None = 0,

		/** Input validation refused the problem before any solving happened. */
		InvalidProblem,

		/** Phase 1 did not reach an optimum, or its factorisation refused. */
		PhaseOneFailure,

		/** Phase 2 ran into MaxPivots — the termination guarantee firing. */
		PhaseTwoIterationCap,

		/** Phase 2 found no positive ratio-test entry on a lambda-capped problem. */
		PhaseTwoUnbounded,

		/** Phase 2's basis went singular under refactorisation. */
		PhaseTwoNumericalFailure,

		/** The optimal basis failed the post-solve check against the original rows. */
		VerificationFailure,
	};

	/**
	 * The reason as a phrase, DISTINCT for every refusing enumerator and empty for None.
	 * Distinctness is the whole contract: two different terminations that read the same
	 * are what forced the instrumented builds.
	 */
	FString RefusalText(EOracleRefusal Refusal);

	struct FOracleResult
	{
		/**
		 * Whether the oracle produced an answer at all. FALSE for anything it refused to
		 * validate and for any internal solver failure; Lambda is meaningless then and is
		 * guaranteed finite (never NaN) so no downstream comparison launders it.
		 */
		bool bAnswered = false;

		/** Why it refused, when it did. None exactly when bAnswered is true. */
		EOracleRefusal Refusal = EOracleRefusal::None;

		/** lambda*, in [0, LambdaCap]. At the cap means "stands at every multiplier". */
		double Lambda = 0.0;

		/** Simplex pivots taken, for diagnostics and the determinism assertions. */
		int32 SimplexIterations = 0;

		/*
		 * WHAT AN EARLY EXIT WOULD SAVE, MEASURED WITHOUT TAKING ONE. PROMOTION_DESIGN
		 * §3.2/§5.2 claim most of a standing structure's saving comes from stopping phase 1
		 * the moment its infeasibility sum reaches tolerance, with no optimality proof. The
		 * solver still runs phase 1 to OPTIMALITY — an early exit returns a feasible point
		 * rather than an optimal one, which is a different contract and a decision the
		 * promotion design has not taken — so these two fields report where the exit COULD
		 * have fired while every pivot path, lambda* and count stays what it was. The saving
		 * is (PhaseOnePivots - PivotsToFirstFeasible) / PhaseOnePivots, and
		 * OracleSlowSweep.RigidBlock.FeasibilityReformulationCost is the row that reads it.
		 *
		 * INDEX_NONE rather than zero on both, so "nobody set this" stays loud rather than
		 * reading as a plausible measurement of no work. A solve that never gets as far as
		 * building a basis leaves both saying so.
		 */

		/**
		 * How many of SimplexIterations phase 1 spent — zero on a problem that started
		 * feasible, which is every gravity-live pose. INDEX_NONE: not reported.
		 */
		int32 PhaseOnePivots = INDEX_NONE;

		/**
		 * The pivot at which phase 1's infeasibility sum first reached tolerance, and so
		 * the first moment feasibility was PROVED. INDEX_NONE when it never was: an
		 * infeasible problem has no such pivot. A REFUSED solve may report either — both
		 * fields are assigned before the PhaseOneFailure return, so a phase 1 that reached
		 * feasibility and then refused reports the real pivot indices it reached, and only
		 * a solve refused BEFORE phase 1 ran leaves them at INDEX_NONE.
		 */
		int32 PivotsToFirstFeasible = INDEX_NONE;

		/**
		 * HOW MUCH PRICING WORK THE SOLVE COST: one count per column evaluated against a
		 * dual vector — a reduced cost c_j - y.A_j in phase 1 or 2, or a tableau entry
		 * rho.A_j in the artificial pivot-out pass. It WAS the oracle's dominant cost at
		 * wall scale while pricing was full Dantzig (every column, every iteration:
		 * pivots x columns), which is why it is a measured OUTPUT rather than a comment:
		 * DestructionGame.Oracle.RigidBlock.PricingCost budgets it, and the budget is what
		 * a pricing change has to satisfy while reproducing the same lambda*.
		 *
		 * Deterministic like everything else here — same problem, same count — and int64
		 * because a 30-course wall (~34k columns, tens of thousands of pivots) overflows
		 * int32 by orders of magnitude. Counts up to the last checkpoint on a refusal.
		 */
		int64 PricingColumnScans = 0;

		/**
		 * HOW OFTEN THE ANTI-CYCLING FALLBACK ENGAGED: one count per simplex iteration
		 * entered with the Bland rule in force, i.e. after a run of 500 zero-length steps
		 * has made the partial pricer stand aside. Instrumentation only — nothing branches
		 * on it — and it exists so "the fallback never fires on THIS fixture" is OBSERVED by
		 * a test rather than assumed from reading the code. The two PricingCost fixtures
		 * assert it is zero, and that is the whole of what is asserted anywhere: the
		 * opening-ladder rungs were measured on 2026-08-13 entering the fallback 170-358
		 * times a solve, so the branch is reached constantly while its own behaviour is
		 * still exercised by no fixture chosen for the purpose — recorded in CURRENT_STATE
		 * rather than hidden here.
		 *
		 * Deterministic like every other count. int32 is ample: MaxPivots bounds it.
		 */
		int32 BlandDegenerateEntries = 0;

		/** Why the oracle refused, when it did. Empty on an answered result. */
		FString WhyNot;
	};

	/** Solve the lower-bound LP. Deterministic: same problem, bit-identical result. */
	FOracleResult SolveRigidBlock(const FOracleProblem& Problem);

	/** Stands iff answered and Lambda >= 1. Unanswerable is never Stands. */
	EOracleOutcome OutcomeOf(const FOracleResult& Result);

	/**
	 * THE BRIDGE THE FIXTURE SWEEP CALLS: project a live FStructure into an X-Z
	 * rigid-block problem.
	 *
	 * What it reads is exactly the data model DESIGN.md §7 says is the LP's input:
	 * pieces with mass and centre of mass, joints with centre, half extents, normal and
	 * strength profile. Removed pieces are skipped; joints that have GIVEN are skipped
	 * (a broken joint is out of the structure, so the oracle judges the graph as it
	 * stands now, latch included); a joint between two grounded pieces constrains
	 * nothing and is skipped.
	 *
	 * REFUSED, fail closed: a structure without complete geometry (a defaulted centre
	 * or rectangle would silently become a lever arm of "at the origin"), a live joint
	 * naming a removed piece (the known AddConnection tombstone hole), and any joint
	 * whose normal has a Y component — this is a 2D oracle and projecting an
	 * out-of-plane joint would be a plausible number with wrong statics.
	 *
	 * @return true and a filled problem, or false with the reason; OutProblem is
	 *         emptied on refusal so a caller who ignores the return solves nothing.
	 */
	bool BuildRigidBlockProblem(
		const FStructure& Structure,
		FOracleProblem& OutProblem,
		FString& OutWhyNot);
}

#endif // WITH_DEV_AUTOMATION_TESTS
