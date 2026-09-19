// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Core/ConnectionStrength.h"

/**
 * THE RIGID-BLOCK LIMIT-ANALYSIS ORACLE — production code (PROMOTION_DESIGN.md §6 Slice
 * 1: lives under Core/RigidBlock/, compiles in every configuration including Shipping,
 * because the equilibrium gate it feeds is production authority) but still walled off
 * from the rest of production: it reads nothing but the plain data structs here and
 * Core/ConnectionStrength.h, calls no production arithmetic, borrows no routing, tiers,
 * arching or composite-depth logic, and derives its own unit conversion. That
 * independence is what lets the sweep be an independently derived second opinion the
 * fixture catalogue can be diffed against — if the solver shared production's strength
 * or geometry code, a defect in one would be invisible to the diff. RigidBlockBridge,
 * not this file, is where FStructure gets translated, to keep this file free of that
 * dependency.
 *
 * WHAT IT COMPUTES. The lower-bound (static/safe) theorem of limit analysis over rigid
 * blocks in the X-Z plane: does there exist any system of joint contact forces in
 * equilibrium with lambda times the live loads (gravity, by default) that violates no
 * strength constraint? The largest such lambda is the load factor lambda* — a
 * measurement, not a verdict: lambda* >= 1 stands, below it falls.
 *
 * THE FORMULATION (Livesley 1978, "Limit analysis of structures formed from rigid
 * blocks"; Gilbert & Melbourne 1994 extend it to multi-ring arches; Lourenco's
 * simplified micro-model is the same idealisation — rigid units, all deformation and
 * failure in zero-thickness interfaces, exactly what FConnection already is):
 *
 *   - Each rectangular joint is two contact points at the ends of its in-plane segment
 *     — exact for straight-line normal-stress distributions and equal to the
 *     fully-plastic stress block in bending. Per contact point k: a normal force n_k
 *     (positive in compression, matching the interface normal's A-to-B convention) and
 *     a shear v_k along the in-plane tangent.
 *   - Per non-grounded block: three equilibrium equalities (sum Fx, sum Fz, moment about
 *     the centroid). Grounded blocks are the earth and contribute no rows, exactly as
 *     FStructure's grounded flag terminates the flow of load.
 *   - Per contact point, the strength inequalities, converted from the profile's SI
 *     megapascals over the contact's tributary area A/2:
 *
 *         tension     n_k >= -f_t * Conv * A/2      the finite-tension mapping, below
 *         friction    |v_k| <= c * Conv * A/2 + mu * n_k
 *         crushing    n_k <= f_c * Conv * A/2
 *         ceiling     |v_k| <= f_v,max * Conv * A/2     (only when the profile caps it)
 *
 *   - Objective: maximise lambda, the multiplier on the live loads. Gravity is live by
 *     default ("how many times its own weight could this carry"); a problem may mark
 *     gravity dead and supply explicit live forces instead (a pushed block, an
 *     eccentric surcharge) — the classic dead/live split. Lambda is capped at LambdaCap
 *     so an all-homogeneous structure (dry stone: every capacity scales with load)
 *     reports the cap rather than an unbounded LP.
 *
 * THE FINITE-TENSION MAPPING. Classic rigid-block analysis (Heyman) is no-tension; this
 * project's mortar carries a real f_xk1 and the case-12 ruling showed the bond term
 * decides real verdicts. n_k >= -f_t * A/2 is a rigid-plastic tension cut-off per
 * contact point — Lourenco's interface cut-off in LP clothing — chosen because it
 * reduces exactly to classic no-tension when f_t = 0, the same reduction-by-data
 * pattern that keeps mu = 0 fasteners out of the code paths. Two consequences when
 * diffing against production:
 *
 *   - Capacity is read at the plastic limit, not first crack: concentrating the tension
 *     resultant at the edge contact reads up to 3x the uncracked-elastic first-crack
 *     moment (f*b*d^2/2 against f*b*d^2/6) — the honest lower-bound answer to "is there
 *     any admissible equilibrium", against production's "does the elastic path crack".
 *     A structure the oracle fails is failed under the most charitable reading; one it
 *     stands may still crack first elastically. The leaning-stack validation ladder
 *     pins both directions.
 *   - The friction row is the linearised associative form, |v| <= cA + mu*n with n
 *     signed, so a contact in tension has its cohesion eroded by mu*|tension| — the
 *     standard rigid-block linearisation (Livesley, Gilbert) and conservative relative
 *     to production's rule (tension buys no friction but keeps cohesion). With Coulomb
 *     friction the static theorem is not strictly a bound (non-associative flow) — the
 *     one honesty caveat of the whole method.
 *
 * UNITS. The oracle derives its own MPa-to-force conversion, deliberately not
 * DestructionForce::ForceUnitsPerMPaSqCm: an oracle that imported the constant it is
 * supposed to check would agree with a wrong value instead of failing against it. Same
 * convention as every acceptance test's independent derivation.
 *
 * DETERMINISM IS A CONTRACT. The solver is a sparse revised two-phase primal simplex
 * (rewritten 2026-08-12; the original dense tableau accumulated one rounding per cell
 * per pivot and its own verification refused every fixture past ~4,000 pivots): the
 * constraint matrix is held untouched in sparse column form, the basis as an LU
 * factorisation (left-looking, partial pivoting by magnitude with lowest-index ties)
 * plus a product-form eta file, refactorised from the original clean data on a fixed
 * cadence — bounding the iteration cost and resetting accumulated error, which the
 * dense method structurally lacked. Pricing is a candidate list over a rotating column
 * window (best few by static steepest-edge ratio d_j / ||A_j||, re-priced against
 * current duals before any is chosen, a full sweep required before optimality may be
 * claimed) with a largest-pivot tie-break in the ratio test, falling back to Bland's
 * rule over a full scan for the entering choice only after a 500-pivot degenerate
 * streak. Every tie-break is index-based — no randomness, no time, no hashing — so the
 * same problem gives bit-identical lambda* on every run, which is what lets a
 * fixture-sweep diff be re-run and trusted. Because the fallback's leaving choice is
 * not Bland's, the classical no-cycling theorem does not apply; termination is instead
 * guaranteed by the fail-closed iteration cap, which reports failure rather than a
 * number — do not remove it as "redundant". A redundant row can leave an artificial
 * parked basic at zero after phase 1; the pivot-out pass right after phase 1 clears
 * every one a real column can reach, contained together with the post-solve
 * verification gate (which fails closed rather than certifying a basis it cannot
 * check) — not by any discipline inside phase 2 itself, since phase 2 prices every
 * real column exactly against the final duals before stopping and complementary
 * slackness certifies the optimum regardless of what a zero-value artificial sits on.
 *
 * FAIL CLOSED, EVERYWHERE. A problem the oracle cannot validate — NaN anywhere, a
 * non-unit normal, a nonsense index, incomplete geometry — returns bAnswered = false,
 * and OutcomeOf maps that to Unanswerable, the zero enumerator, so a zero-filled verdict
 * can never claim a structure stands. Unlike production's documented NaN-ceiling
 * laundering hazard, a NaN MaxShearStrengthMPa here is refused rather than silently
 * treated as uncapped.
 *
 * SCOPE HONESTY. 2D means the X-Z plane: Y is the wythe and enters only through joint
 * areas. Every current fixture is single-wythe and laid in X-Z; a joint whose normal
 * has a Y component is refused rather than projected. Memory is O(nonzeros + basis
 * fill), so every fixture the project owns — the ~375-piece 30-course walls included —
 * is representable; the measured envelope per fixture lives in
 * RigidBlockOracleSweepTest.cpp's headers. Still a test oracle, not a shipping solver.
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
	 * The independence made enforceable, not merely asserted in prose: this static_assert
	 * checks the oracle's own conversion above agrees with production's
	 * DestructionForce::ForceUnitsPerMPaSqCm without sharing the symbol. Two constants
	 * derived independently and proven equal is the check; one constant used twice is no
	 * check at all. A production change that moved the factor to a wrong 100x would break
	 * this build rather than passing a tuned sweep.
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

		/**
		 * TRUE: this block's own weight is a live load, scaling with lambda exactly as
		 * Problem.bGravityIsLive does globally but for one block. FALSE (default): dead,
		 * a fixed right-hand-side constant. This is the per-block dead/live split
		 * (PROMOTION_DESIGN §10/§472, Slice 6d) — how a surcharge (wall-15's projecting
		 * headers, the shed's roof) is posed live while the rest of a structure's
		 * self-weight is dead, which the global flag cannot express (it scales every
		 * block by the same lambda, so it cannot tell the surcharge's demand apart from
		 * the pre-compression that steadies it — why wall-15 and wall-16 read one
		 * identical lambda* today).
		 *
		 * Can only ADD liveness: a block's weight routes to the lambda column iff
		 * Problem.bGravityIsLive OR Block.bLiveGravity, so a globally-live pose is
		 * unchanged and an untagged block under a globally-dead pose is unchanged — the
		 * flag only turns a specific block live under otherwise-dead gravity. There is
		 * deliberately no way to force a block dead while global gravity is live.
		 */
		bool bLiveGravity = false;

		/**
		 * 3D stub: the block centroid's plan-Y coordinate. Declared at the struct end so
		 * every existing 4-element brace init still names the same members. Default 0.0
		 * and inert on the 2D path; AssembleThreeD is what consumes it for a 3D problem.
		 */
		double CentroidYCm = 0.0;
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

		/**
		 * 3D stub, declared at the struct end so the 2D path (which sets NormalX/NormalZ,
		 * CentreXCm/CentreZCm, HalfLengthCm by name) is unchanged: NormalY and CentreYCm
		 * are the normal's and patch centre's Y components; HalfUCm/HalfVCm are the
		 * rectangular patch's two in-plane half-extents (the 3D analogue of HalfLengthCm
		 * — both zero is the point patch, four coincident corners, as HalfLengthCm = 0
		 * collapses the 2D pair into one). AssembleThreeD turns them into six equilibrium
		 * rows and four-corner contacts; the 2D assembler reads none of them.
		 */
		double NormalY = 0.0;
		double CentreYCm = 0.0;
		double HalfUCm = 0.0;
		double HalfVCm = 0.0;

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

		/**
		 * 3D stub, declared at the struct end so every existing 2D brace init still names
		 * the same members. ForceYUu/AtYCm are the applied force's Y component and the
		 * application point's plan-Y — default 0.0, inert on the 2D path. A 3D push needs
		 * U and V components, which these carry; the current assembler poses no applied
		 * force on the 3D path yet.
		 */
		double ForceYUu = 0.0;
		double AtYCm = 0.0;
	};

	/**
	 * A simplex basis as a value, so one solve can hand its starting point to the next —
	 * PROMOTION_DESIGN.md §5.4's warm-start lever: production re-solves after a small
	 * change (a brick deleted from a structure it already answered), and starting from
	 * the previous solve's basis may collapse the pivot count.
	 *
	 * Columns holds one entry per row of the problem's standard form, in slot order.
	 * INDEX_NONE means "no hint for this row, use the cold default", letting a caller
	 * supply a basis it could only map partially — the common case after a deletion,
	 * where some of the previous basis names columns that no longer exist.
	 *
	 * NumStructCols and ArtificialStart describe the shape the basis belongs to: a
	 * column index is meaningless without knowing where structural columns end and
	 * artificials begin, so a caller needs them to map one problem's basis onto
	 * another's columns. On the way in they are the caller's own bookkeeping — the
	 * solve validates the columns themselves against the matrix it actually built,
	 * which is a stronger check than agreeing with a shape the caller also supplied.
	 *
	 * A supplied basis is a hint and may never change the answer — a warm start that
	 * moves lambda* or a verdict is a defect, not a speedup — enforced by the post-solve
	 * verification gate checking the final basis against the original assembly rows.
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

	/**
	 * Which physics the assembler poses. Dim2D is the zero enumerator and the default
	 * (safe-default, enum-not-bool house idiom), so every existing fixture is a 2D
	 * problem exactly as before. SolveRigidBlock routes a Dim3D problem to
	 * AssembleThreeD.
	 */
	enum class EOracleDim : uint8
	{
		Dim2D,
		Dim3D,
	};

	struct FOracleProblem
	{
		TArray<FOracleBlock> Blocks;
		TArray<FOracleJoint> Joints;
		TArray<FOracleAppliedForce> AppliedForces;

		/** 2D (X-Z) by default so no existing fixture moves; set to Dim3D for a 3D pose. */
		EOracleDim Dim = EOracleDim::Dim2D;

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
		 * Sec 4.3). Keyed on data, not material: an f_t = 0 joint has no bond to crack and
		 * keeps the plastic no-tension form, so every dry-stone row stays bit-identical.
		 * Default off.
		 */
		bool bFirstCrackRows = false;

		/**
		 * TRUE: solve the min-violation (goal-programming) LP that sources the per-joint
		 * strain readout (PROMOTION_DESIGN §3.1/§3.5/§3.6), instead of reading a per-joint
		 * number off the maximise-lambda primal — which is vacuous at lambda >= 1, since
		 * that LP is defined as the search for a force system in which no joint reads
		 * over 1.0, so every joint would read "misleadingly comfortable". The min-violation
		 * LP is the different solve the readout needs: fix the load at lambda = 1 (real
		 * self-weight, posed with bGravityIsLive = false so gravity enters as a dead
		 * constant), keep the per-block equilibrium equality rows hard, add one
		 * non-negative slack s_k to every strength inequality row (a_k.x <= b_k + s_k),
		 * and minimise sum w_k*s_k. Because only strength is relaxed a solution always
		 * exists — the least-infeasible force system — so a standing structure reads all
		 * slacks zero and an over-capacity one reads positive slack exactly on the
		 * over-stressed joints, a genuine per-joint "distance past failure". See
		 * FOracleReadout. Additive: with this off the maximise-lambda solve stays
		 * bit-identical, so no sweep pin may move. Default off.
		 */
		bool bMinViolationReadout = false;

		/**
		 * BRIDGE PROVENANCE — how an oracle block/joint index maps back to the FStructure
		 * piece/connection it came from, so the mechanism (below) can name the bricks and
		 * joints a caller understands. PieceOfBlock[b] is the piece that produced oracle
		 * block b; ConnectionOfJoint[j] is the connection that produced oracle joint j.
		 * The core solver never reads these — they are carried through from the bridge,
		 * the one unit that knows FStructure indices (PROMOTION_DESIGN §12 D7). Empty on
		 * a cold, hand-built FOracleProblem that never went through the bridge.
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
	 * WHY THE ORACLE REFUSED, as a value rather than a sentence. None is the zero
	 * enumerator, so a default-constructed or zero-filled result reads "no reason",
	 * consistent with bAnswered's own zero-is-refused polarity.
	 *
	 * The three PhaseTwo arms are the point: until 2026-08-15 a hit iteration cap, a
	 * spurious unbounded ray, and a numerical failure all arrived as one sentence
	 * ("phase-2 simplex failed"), so telling them apart needed an instrumented build.
	 * They are different events with different fixes, and production must be able to
	 * count refusals by reason (§5.6, §11 R4), which a sentence is a poor thing to count
	 * by. RigidBlockOracle.cpp's RefusalText still carries the old wording per arm. See
	 * OracleSweepFast.RigidBlock.RefusalNamesItsReason.
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

		/**
		 * 3D stub, declared at the struct end so every 2D reader is unchanged. On the 2D
		 * path the collapse lives entirely in the X-Z plane — translation (VirtualUx,
		 * VirtualUz), rotation the single scalar VirtualOmega about Y — so these stay
		 * zero. VirtualUy is the centroid's Y translation; VirtualOmegaX/Z are rotation
		 * about X and Z. Together with VirtualOmega they form the full 3-vector omega =
		 * (VirtualOmegaX, VirtualOmega, VirtualOmegaZ) the 3D mechanism needs
		 * (THREED_DESIGN Data). A 2D or genuinely-in-plane collapse leaves them zero.
		 */
		double VirtualUy = 0.0;
		double VirtualOmegaX = 0.0;
		double VirtualOmegaZ = 0.0;
	};

	/**
	 * THE KINEMATIC COLLAPSE MECHANISM, extracted from the infeasible arm's phase-1 dual.
	 *
	 * When the feasibility formulation (bGravityIsLive = false) finds no admissible force
	 * system, phase 1 terminates with a positive optimum whose dual y is a Farkas
	 * certificate of infeasibility; in rigid-block limit analysis that dual IS the
	 * kinematic (upper-bound) mechanism (Livesley 1978; PROMOTION_DESIGN §3.3). Mapped
	 * through the assembly's row->block and column->contact bookkeeping it names, per
	 * block, a virtual-motion triple, and per joint whether a contact opens or slides —
	 * the blocks whose triple moves are the blocks that fall, the joints that open/slide
	 * are the break set.
	 *
	 * The raw dual multipliers are non-unique where the problem is degenerate — measured:
	 * on the 24-course degenerate fixture the per-joint plastic multipliers named 8
	 * opening joints on one column order and 13-16 on another. So the joint set is read
	 * instead off the block velocity triples: a joint opens/slides iff its two blocks have
	 * relative rigid-body velocity at a contact (associated flow), which measured unique
	 * to ~1e-15 on the fixtures owned. A block moves iff its normalized triple magnitude
	 * exceeds a per-block relative threshold tau (MechanismRelativeTol). See
	 * RigidBlockOracle.cpp's ExtractMechanism and PROMOTION_DESIGN §3.3 / §12 D7.
	 *
	 * DETERMINISM AT SCALE IS NOT YET PROVEN. Block-velocity uniqueness is a property of
	 * single-mode collapses (the fixtures here have 0 Bland entries); a
	 * marginally-infeasible wall with several simultaneous hinge lines has multiple
	 * Farkas rays and its velocity field may not be permutation-unique. Before wiring any
	 * such wall to this mechanism, a Bland-degenerate multi-mode fixture must show the
	 * named set stable, or the minimal-support tie-break (D7) must be built — neither
	 * exists yet, because no owned fixture drives it.
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
	 * ONE JOINT'S STRAIN READOUT from the min-violation LP (PROMOTION_DESIGN §3.5).
	 *
	 * NormalUu and MomentUuCm are the joint's resultant read off the min-violation
	 * primal — a real closest-to-admissible equilibrium, not the vacuous maximise-lambda
	 * one. NormalUu is N = n1 + n2 (compression positive, so a joint in tension reads
	 * negative); MomentUuCm is M = HalfLength * (n1 - n2) about the joint centre. Because
	 * the equilibrium rows stay hard, this resultant balances the block's dead load to
	 * solver tolerance even where the joint is over capacity.
	 *
	 * ViolationUu is the total non-negative strength-row slack charged to this joint, in
	 * force units: 0 when within capacity, > 0 by the amount the force system had to
	 * exceed a capacity here to keep equilibrium. Utilisation maps that onto the same
	 * 0 -> 1 -> >1 scale FConnection::UtilisationUnder produces, so an overlay can
	 * consume it directly — for an over-capacity joint Utilisation = 1 + ViolationUu /
	 * capacity = demand / capacity.
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
	 * (FOracleProblem::bMinViolationReadout). Empty by default (bPresent = false). One
	 * entry per oracle joint, in joint-index order; a caller maps each back to its
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

		/**
		 * WHAT AN EARLY EXIT WOULD SAVE, MEASURED WITHOUT TAKING ONE. PROMOTION_DESIGN §3.2/§5.2
		 * claim most of a standing structure's saving comes from stopping phase 1 the moment its
		 * infeasibility sum reaches tolerance, with no optimality proof; the solver still runs
		 * phase 1 to optimality (an early exit returns a feasible, not optimal, point — a
		 * different contract not yet taken), so PhaseOnePivots and PivotsToFirstFeasible below
		 * report where the exit could have fired while every pivot path and lambda* stays what
		 * it was. Saving = (PhaseOnePivots - PivotsToFirstFeasible) / PhaseOnePivots, read by
		 * OracleSweepFull.RigidBlock.FeasibilityReformulationCost. INDEX_NONE rather than zero
		 * on both, so "nobody set this" stays loud rather than reading as a measurement of no
		 * work; a solve that never builds a basis leaves both saying so.
		 */

		/**
		 * How many of SimplexIterations phase 1 spent — zero on a problem that started
		 * feasible, which is every gravity-live pose. INDEX_NONE: not reported.
		 */
		int32 PhaseOnePivots = INDEX_NONE;

		/**
		 * The pivot at which phase 1's infeasibility sum first reached tolerance — the first
		 * moment feasibility was proved. INDEX_NONE when it never was (an infeasible problem
		 * has no such pivot), and also on a solve refused before phase 1 ran; a phase 1 that
		 * reached feasibility and then refused still reports the real pivot indices it reached.
		 */
		int32 PivotsToFirstFeasible = INDEX_NONE;

		/**
		 * HOW MUCH PRICING WORK THE SOLVE COST: one count per column evaluated against a dual
		 * vector — a reduced cost c_j - y.A_j in phase 1 or 2, or a tableau entry rho.A_j in
		 * the artificial pivot-out pass. It was the oracle's dominant cost at wall scale while
		 * pricing was full Dantzig (every column, every iteration), which is why it is a
		 * measured output: DestructionGame.Oracle.RigidBlock.PricingCost budgets it. int64
		 * because a 30-course wall (~34k columns) overflows int32. Counts up to the last
		 * checkpoint on a refusal.
		 */
		int64 PricingColumnScans = 0;

		/**
		 * HOW OFTEN THE ANTI-CYCLING FALLBACK ENGAGED: one count per simplex iteration entered
		 * with the Bland rule in force, i.e. after a run of 500 zero-length steps. Instrumentation
		 * only — nothing branches on it — so "the fallback never fires on this fixture" is
		 * observed by a test rather than assumed. The two PricingCost fixtures assert it is
		 * zero; the opening-ladder rungs measured 170-358 fallback entries a solve on
		 * 2026-08-13, so the branch is reached constantly while its own behaviour is untested —
		 * recorded in CURRENT_STATE. int32 is ample: MaxPivots bounds it.
		 */
		int32 BlandDegenerateEntries = 0;

		/**
		 * THE BASIS THE SOLVE ENDED ON, so the next solve can start from it. Reported on every
		 * answered solve, both arms — the optimum phase 2 reached, and the basis phase 1
		 * stopped on when the dead loads admit no equilibrium at all. Empty on a refusal: a
		 * basis whose own solve was not certified is not a starting point anything should be
		 * handed. NumStructCols and ArtificialStart publish the shape it belongs to, the only
		 * way a caller can map it onto a changed problem's columns.
		 */
		FOracleBasis FinalBasis;

		/**
		 * HOW MUCH OF A SUPPLIED WARM START THE SOLVE ACTUALLY STARTED FROM: the number of
		 * FOracleProblem::StartingBasis entries that survived validation and basis repair.
		 * INDEX_NONE when no warm start was supplied. Without it, a warm start that saved
		 * nothing is indistinguishable from one silently thrown away — opposite findings. A
		 * hint that names a column that no longer exists, duplicates another row's column, or
		 * would make the basis singular is repaired to the cold default for its row rather
		 * than refused, because a warm start that fails closed measures nothing.
		 */
		int32 WarmStartColumnsAccepted = INDEX_NONE;

		/** Why the oracle refused, when it did. Empty on an answered result. */
		FString WhyNot;

		/** THE COLLAPSE MECHANISM, when the feasibility formulation was infeasible. Empty by default. See FOracleMechanism. */
		FOracleMechanism Mechanism;

		/** THE MIN-VIOLATION STRAIN READOUT, when bMinViolationReadout was set. Empty by default. See FOracleReadout. */
		FOracleReadout Readout;
	};

	/**
	 * Two in-plane axes (U, V) derived deterministically from a unit normal — the frame the
	 * 3D assembler places its four contact corners in. Shared with RigidBlockBridge so the
	 * 3D bridge poses HalfUCm/HalfVCm in the same frame the solver reads them against; if the
	 * two derivations disagreed the posed rectangle would be measured against a different
	 * frame than it was written in. Pure geometry — no strength or force crosses it — so
	 * sharing it does not breach the oracle's independence. N must be unit length; U x V = N.
	 */
	void DeriveInPlaneAxes(const double N[3], double U[3], double V[3]);

	/** Solve the lower-bound LP. Deterministic: same problem, bit-identical result. */
	FOracleResult SolveRigidBlock(const FOracleProblem& Problem);

	/** Stands iff answered and Lambda >= 1. Unanswerable is never Stands. */
	EOracleOutcome OutcomeOf(const FOracleResult& Result);
}
