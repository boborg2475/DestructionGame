// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Core/ConnectionStrength.h"

/**
 * THE RIGID-BLOCK LIMIT-ANALYSIS ORACLE — NOW PRODUCTION, BUT ITS INDEPENDENCE FROM
 * PRODUCTION IS STILL A RULE, NOT A PHASE. As of PROMOTION_DESIGN.md §6 Slice 1 this
 * solver lives under Core/RigidBlock/ and compiles in every configuration, Shipping
 * included, because the equilibrium gate it feeds is production authority. What has NOT
 * changed, and must not, is the wall between it and the rest of production: the solver
 * core reads nothing but the plain data structs and Core/ConnectionStrength.h, calls no
 * production arithmetic, borrows no routing, tiers, arching groups or composite depth,
 * and derives its own unit conversion. That independence used to be phrased as "test
 * support only"; it is now a deliberate design rule with a sharper reason. The sweep's
 * whole value is being an INDEPENDENTLY DERIVED second opinion the fixture catalogue can
 * be diffed against, and the day the solver borrows production's own strength or geometry
 * code is the day the diff can no longer catch production being wrong — because the two
 * sides would then share the defect. The FStructure translation lives in the separate
 * RigidBlockBridge unit precisely to keep this file free of any structural dependency.
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

	/*
	 * THE INDEPENDENCE MADE ENFORCEABLE, NOT MERELY ASSERTED IN PROSE. The oracle derives
	 * its own conversion above and this static_assert checks it AGREES with production's
	 * DestructionForce::ForceUnitsPerMPaSqCm — it never shares the symbol, because an
	 * oracle that imported the constant it is meant to check would agree with a wrong value
	 * instead of failing against it. Two constants derived independently and proven equal is
	 * the check; one constant used twice is no check at all. A production change that moved
	 * the factor to a wrong 100x would break this build rather than passing a tuned sweep.
	 */
	static_assert(
		OracleForceUnitsPerMPaSqCm == DestructionForce::ForceUnitsPerMPaSqCm,
		"the oracle's independently derived MPa->uu conversion must agree with production's; "
		"if this fires, one of the two is wrong — do NOT unify them into one symbol");

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

	/**
	 * A SIMPLEX BASIS AS A VALUE, so one solve can hand its starting point to the next.
	 * PROMOTION_DESIGN.md §5.4's warm-start lever, whose seam this is: production re-solves
	 * after a SMALL CHANGE (a brick deleted from a structure it already answered), and the
	 * question nobody has measured is whether starting from the previous solve's basis
	 * collapses the pivot count.
	 *
	 * Columns holds ONE ENTRY PER ROW of the problem's standard form, in slot order.
	 * INDEX_NONE means "no hint for this row, use the cold default", which is what lets a
	 * caller supply a basis it could only map partially - the common case after a deletion,
	 * where some of the previous basis names columns that no longer exist.
	 *
	 * NumStructCols and ArtificialStart DESCRIBE THE SHAPE THE BASIS BELONGS TO: a column
	 * index is meaningless without knowing where the structural columns end and the
	 * artificials begin, so they are what a caller needs in order to MAP one problem's basis
	 * onto another's columns, and that is why a solve reports them. On the way IN they are
	 * the caller's own bookkeeping and the solve does not read them — it validates the
	 * columns themselves, one at a time, against the matrix it actually built, which is a
	 * stronger check than agreeing with a shape the caller also supplied.
	 *
	 * A SUPPLIED BASIS IS A HINT AND MAY NEVER CHANGE THE ANSWER. That is the contract the
	 * whole lever stands on - a warm start that moves lambda* or a verdict is a defect, not
	 * a speedup - and it is enforceable rather than hoped for, because the post-solve
	 * verification gate already checks the final basis against the ORIGINAL assembly rows.
	 */
	struct FOracleBasis
	{
		/** One column index per standard-form row; INDEX_NONE = use the cold default. */
		TArray<int32> Columns;

		/** Where the slack columns begin - i.e. how many structural columns there are. */
		int32 NumStructCols = 0;

		/** Where the artificial columns begin - i.e. structural plus slack columns. */
		int32 ArtificialStart = 0;
	};

	struct FOracleProblem
	{
		TArray<FOracleBlock> Blocks;
		TArray<FOracleJoint> Joints;
		TArray<FOracleAppliedForce> AppliedForces;

		/**
		 * WHERE THE SIMPLEX STARTS, when a caller has a related problem's answer in hand.
		 * Empty Columns means a cold start and MUST be bit-identical to the solver before
		 * this field existed - every pinned pivot count in the sweep is that assertion.
		 */
		FOracleBasis StartingBasis;

		/**
		 * TRUE: gravity is the live load and lambda* answers "how many times its own
		 * weight". FALSE: gravity is dead and lambda scales only the live applied
		 * forces — the classic dead/live split a sliding or surcharge problem needs.
		 */
		bool bGravityIsLive = true;

		/**
		 * TRUE: carry the uncracked first-crack rows for every joint with a real tensile
		 * bond (f_t > 0) — -(n1+n2) + 3|n1-n2| <= f_t * A per joint, two linear rows,
		 * cutting a bonded section's plastic bending capacity to one third (PROMOTION_DESIGN
		 * Sec 4.3). Keyed on DATA, not material: an f_t = 0 joint has no bond to crack and
		 * keeps the plastic no-tension form, so every dry-stone row stays bit-identical.
		 *
		 * SLICE 0d COMPILE SEAM ONLY, DEFAULT OFF: this flag is declared so the red test can
		 * ask for the rows; the solver does not read it yet, which is precisely why that
		 * test is red (the rows are absent, not the type). dev-expert wires it to actually
		 * assemble the rows in SolveRigidBlockOnce.
		 */
		bool bFirstCrackRows = false;

		/**
		 * TRUE: solve the MIN-VIOLATION (goal-programming) LP that sources the per-joint
		 * STRAIN READOUT (PROMOTION_DESIGN §3.1/§3.5/§3.6 — Slice 6a), instead of reading a
		 * per-joint number off the maximise-lambda primal (which is VACUOUS at lambda >= 1:
		 * that LP is DEFINED as the search for a force system in which no joint reads over
		 * 1.0, so every joint reads "misleadingly comfortable" — §3.1, and the trap worth a
		 * comment at the seam). The min-violation LP is the DIFFERENT solve the readout needs:
		 * fix the load at lambda = 1 (real self-weight — posed with bGravityIsLive = false so
		 * gravity enters the equality rows as a dead constant), keep the per-block equilibrium
		 * EQUALITY rows HARD, add one non-negative slack s_k >= 0 to every STRENGTH inequality
		 * row relaxing a_k.x <= b_k to a_k.x <= b_k + s_k, and MINIMISE sum w_k*s_k. Because
		 * equilibrium stays hard and only strength is relaxed a solution ALWAYS exists — the
		 * least-infeasible force system — so a STANDING structure reads all slacks zero and an
		 * OVER-CAPACITY one reads positive slack exactly on the over-stressed joints, with a
		 * magnitude that is a genuine per-joint "distance past failure". See FOracleReadout.
		 *
		 * SLICE 6a COMPILE SEAM ONLY, DEFAULT OFF: this flag is declared so the red test can
		 * ASK for the readout; the solver does not read it yet, which is precisely why that
		 * test is red (the readout is EMPTY, not the type). dev-expert wires the min-violation
		 * formulation and fills FOracleResult::Readout. It is an ADDITIVE new formulation
		 * behind this flag: with it OFF the maximise-lambda solve stays bit-identical, so no
		 * sweep pin (the slow OracleSweepFull included) may move — the D5/first-crack precedent.
		 */
		bool bMinViolationReadout = false;

		/**
		 * BRIDGE PROVENANCE — how an oracle block/joint index maps back to the FStructure
		 * piece/connection it came from, so the mechanism (below) can NAME the bricks and
		 * joints a caller understands. PieceOfBlock[b] is the piece that produced oracle
		 * block b; ConnectionOfJoint[j] is the connection that produced oracle joint j.
		 *
		 * SLICE 3a COMPILE SEAM ONLY, EMPTY BY DEFAULT (PROMOTION_DESIGN §12 D7). The core
		 * solver never reads these — they are carried through from the bridge, which is the
		 * one unit that knows FStructure indices. They are declared empty so the red test can
		 * ASK the bridge to fill them; the bridge does not yet, which is why that half of the
		 * test is red (the provenance is absent, not the type). dev-expert (Slice 3a) fills
		 * both in RigidBlockBridge alongside BlockOfPiece. A cold, hand-built FOracleProblem
		 * that never went through the bridge legitimately leaves them empty.
		 */
		TArray<int32> PieceOfBlock;
		TArray<int32> ConnectionOfJoint;
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
	 * is OracleSweepFast.RigidBlock.RefusalNamesItsReason.
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

	/**
	 * ONE BLOCK'S VIRTUAL RIGID-BODY MOTION in the collapse mechanism — the phase-1 dual
	 * triple (u_x, u_z, omega) read off the block's three equilibrium rows (PROMOTION_DESIGN
	 * §3.3). (VirtualUx, VirtualUz) is the virtual translation of the block's CENTROID (the
	 * moment rows are taken about the centroid, so the (Fx, Fz) duals ARE the centroid's
	 * velocity), and VirtualOmega its virtual rotation. bMoves is the CANONICAL verdict:
	 * true iff, after the certificate is normalized, this block's triple magnitude clears the
	 * relative threshold tau. A grounded block writes no rows, so its triple is exactly zero
	 * and bMoves is false.
	 */
	struct FOracleMechanismBlock
	{
		double VirtualUx = 0.0;
		double VirtualUz = 0.0;
		double VirtualOmega = 0.0;
		bool bMoves = false;
	};

	/**
	 * THE KINEMATIC COLLAPSE MECHANISM, extracted from the infeasible arm's phase-1 dual.
	 *
	 * When the feasibility formulation (bGravityIsLive = false) finds no admissible force
	 * system, phase 1 terminates with a positive optimum whose dual y is a FARKAS CERTIFICATE
	 * of infeasibility; in rigid-block limit analysis that dual IS the kinematic (upper-bound)
	 * mechanism (Livesley 1978; PROMOTION_DESIGN §3.3). Mapped through the assembly's
	 * row->block and column->contact bookkeeping it names, per block, a virtual-motion triple,
	 * and per joint whether a contact of it opens or slides. THE BLOCKS WHOSE TRIPLE MOVES ARE
	 * THE BLOCKS THAT FALL; THE JOINTS THAT OPEN/SLIDE ARE THE BREAK SET.
	 *
	 * THE RAW DUAL MULTIPLIERS ARE NON-UNIQUE where the problem is degenerate — measured: on
	 * the 24-course degenerate fixture the per-joint plastic multipliers named 8 opening joints
	 * on one column order and 13-16 on another. So the joint set is NOT read from the multipliers.
	 * Instead a joint opens/slides iff its two blocks have relative rigid-body velocity at a
	 * contact (associated flow), read off the BLOCK VELOCITY TRIPLES — which measured unique to
	 * ~1e-15 on the fixtures owned. A block moves iff its normalized triple magnitude exceeds a
	 * per-block relative threshold tau (MechanismRelativeTol). See RigidBlockOracle.cpp's
	 * ExtractMechanism and PROMOTION_DESIGN §3.3 / §12 D7.
	 *
	 * DETERMINISM AT SCALE IS NOT YET PROVEN, AND IT IS THE GATE ON SLICE 3b. Block-velocity
	 * uniqueness is a property of SINGLE-MODE collapses (the fixtures here have 0 Bland entries);
	 * a marginally-infeasible wall with several simultaneous hinge lines has multiple Farkas rays
	 * and its velocity field may NOT be permutation-unique. Before 3b wires any wall to this
	 * mechanism, a Bland-degenerate multi-mode permutation fixture must show the named set stable
	 * (the design's R3 test) OR the minimal-support tie-break named in D7 must be built — it is
	 * NOT implemented, because no owned fixture drives it. The Shipping-FP run is also owed.
	 */
	struct FOracleMechanism
	{
		/** One entry per ORACLE BLOCK, in block-index order. Grounded/uninvolved read ~zero. */
		TArray<FOracleMechanismBlock> Blocks;

		/** One entry per ORACLE JOINT, in joint-index order: true iff a contact opens or slides. */
		TArray<bool> JointOpensOrSlides;

		/**
		 * TRUE once the extracted certificate has been Farkas-verified (yA <= tol on every
		 * structural column, yb > 0, and the named set non-empty). A present-but-uncertified
		 * mechanism must never be handed out — the solve refuses with VerificationFailure
		 * instead, exactly as the primal admissibility gate already refuses (§3.6).
		 */
		bool bIsCertified = false;

		/**
		 * TRUE when a mechanism was extracted at all — i.e. the feasibility formulation was
		 * infeasible. FALSE on a feasible/standing problem, whose mechanism is empty by
		 * definition (nothing moves), and on any answered gravity-live solve.
		 */
		bool bPresent = false;
	};

	/**
	 * ONE JOINT'S STRAIN READOUT from the min-violation LP (PROMOTION_DESIGN §3.5, Slice 6a).
	 *
	 * NormalUu and MomentUuCm are the joint's resultant read off the min-violation PRIMAL — a
	 * REAL closest-to-admissible equilibrium, NOT the vacuous maximise-lambda one. NormalUu is
	 * N = n1 + n2 (COMPRESSION POSITIVE, the oracle's sign convention, so a joint in tension
	 * reads negative); MomentUuCm is M = HalfLength * (n1 - n2) about the joint centre. Because
	 * the equilibrium rows stay HARD, this resultant balances the block's dead load to solver
	 * tolerance even where the joint is over capacity.
	 *
	 * ViolationUu is the total non-negative strength-row slack charged to this joint, in force
	 * units: 0 exactly when the joint is within capacity, > 0 by the amount the force system had
	 * to exceed a capacity here to keep equilibrium. Utilisation maps that onto the SAME
	 * 0 -> 1 -> >1 scale FConnection::UtilisationUnder produces, so the overlay (Slice 6b) can
	 * consume it directly: 0 within capacity, 1 at capacity, > 1 over — and for an over-capacity
	 * joint Utilisation = 1 + ViolationUu / capacity = demand / capacity.
	 */
	struct FOracleJointReadout
	{
		double NormalUu = 0.0;
		double MomentUuCm = 0.0;
		double ViolationUu = 0.0;
		double Utilisation = 0.0;
	};

	/**
	 * THE PER-JOINT STRAIN READOUT, present only when the min-violation LP was solved
	 * (FOracleProblem::bMinViolationReadout). Empty by default (bPresent = false) — Slice 6a
	 * fills it. One entry per ORACLE JOINT, in joint-index order; a caller maps each back to its
	 * FConnection through FOracleProblem::ConnectionOfJoint.
	 */
	struct FOracleReadout
	{
		bool bPresent = false;
		TArray<FOracleJointReadout> Joints;
	};

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
		 * OracleSweepFull.RigidBlock.FeasibilityReformulationCost is the row that reads it.
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

		/**
		 * THE BASIS THE SOLVE ENDED ON, so the next solve can start from it. Reported on
		 * every ANSWERED solve, both arms — the optimum phase 2 reached, and the basis phase
		 * 1 stopped on when the dead loads admit no equilibrium at all. Empty on a refusal:
		 * a basis whose own solve was not certified is not a starting point anything should
		 * be handed, and no measurement asks for one. Its NumStructCols and ArtificialStart
		 * publish the shape it belongs to, which is the only way a caller can map it onto a
		 * changed problem's columns.
		 */
		FOracleBasis FinalBasis;

		/**
		 * HOW MUCH OF A SUPPLIED WARM START THE SOLVE ACTUALLY STARTED FROM: the number of
		 * FOracleProblem::StartingBasis entries that survived validation and basis repair.
		 * INDEX_NONE when no warm start was supplied.
		 *
		 * It is here because without it a warm start that saved nothing is INDISTINGUISHABLE
		 * from a warm start that was silently thrown away, and those are opposite findings:
		 * one says the lever does not work, the other says it was never pulled. A hint that
		 * names a column that no longer exists, that duplicates another row's column, or
		 * that would make the basis singular is replaced by the cold default for its row -
		 * repaired, never refused, because a warm start that fails closed measures nothing.
		 */
		int32 WarmStartColumnsAccepted = INDEX_NONE;

		/** Why the oracle refused, when it did. Empty on an answered result. */
		FString WhyNot;

		/**
		 * THE COLLAPSE MECHANISM, when the feasibility formulation was infeasible. Empty by
		 * default (bPresent = false) — Slice 3a fills it. See FOracleMechanism.
		 */
		FOracleMechanism Mechanism;

		/**
		 * THE MIN-VIOLATION STRAIN READOUT, when bMinViolationReadout was set. Empty by default
		 * (bPresent = false) — Slice 6a fills it. See FOracleReadout.
		 */
		FOracleReadout Readout;
	};

	/** Solve the lower-bound LP. Deterministic: same problem, bit-identical result. */
	FOracleResult SolveRigidBlock(const FOracleProblem& Problem);

	/** Stands iff answered and Lambda >= 1. Unanswerable is never Stands. */
	EOracleOutcome OutcomeOf(const FOracleResult& Result);
}
