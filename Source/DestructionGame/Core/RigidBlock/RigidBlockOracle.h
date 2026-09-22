// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Core/ConnectionStrength.h"

/**
 * Rigid-block limit-analysis oracle (PROMOTION_DESIGN.md §6). Compiles in every configuration,
 * including Shipping, because the equilibrium gate it feeds is production authority. It is kept
 * independent of production: it reads only these structs and ConnectionStrength.h and derives its
 * own unit conversion, so a defect in production's strength or geometry code shows up as a diff
 * rather than being shared. RigidBlockBridge translates FStructure.
 *
 * It computes the lower-bound (static) theorem over rigid blocks in the X-Z plane: the largest
 * lambda for which some equilibrium of joint forces with lambda times the live loads violates no
 * strength row. lambda* >= 1 stands.
 *
 * Formulation (Livesley 1978; Gilbert & Melbourne 1994). Each joint is two contact points at the
 * ends of its segment, each with normal n_k (compression positive) and shear v_k. Each non-grounded
 * block has three equilibrium rows (Fx, Fz, moment about the centroid). Per contact, over the
 * tributary area A/2:
 *
 *         tension     n_k >= -f_t * Conv * A/2
 *         friction    |v_k| <= c * Conv * A/2 + mu * n_k
 *         crushing    n_k <= f_c * Conv * A/2
 *         ceiling     |v_k| <= f_v,max * Conv * A/2     (only when the profile caps it)
 *
 * Maximise lambda. Gravity is live by default; a problem may mark it dead and supply live forces.
 * Lambda is capped at LambdaCap so dry stone (every capacity scales with load) returns a number.
 *
 * The tension row is a rigid-plastic cut-off that reduces to classic no-tension at f_t = 0. It reads
 * capacity at the plastic limit, up to 3x the elastic first-crack moment (f*b*d^2/2 vs f*b*d^2/6), so
 * a structure the oracle fails is failed under the most charitable reading. The friction row is the
 * associative linearisation, so tension erodes cohesion by mu*|n|, which is more conservative than
 * production. With Coulomb friction the static theorem is not strictly a bound (non-associative flow).
 *
 * Units: the oracle derives its own MPa-to-force conversion so a wrong production constant fails
 * rather than agrees.
 *
 * Determinism is a contract. The solver is a sparse revised two-phase primal simplex: LU basis with
 * lowest-index ties plus an eta file, refactorised from clean data on a fixed cadence; candidate-list
 * pricing by steepest-edge ratio; Bland's rule for the entering choice after a 500-pivot degenerate
 * streak. Every tie-break is index-based, so the same problem gives a bit-identical lambda*. Because
 * the fallback's leaving choice is not Bland's, no-cycling is not guaranteed; the fail-closed
 * iteration cap is what guarantees termination. Do not remove it as redundant. Artificials left at
 * zero after phase 1 are cleared by the pivot-out pass and backed by the post-solve verification gate.
 *
 * Fails closed: NaN, a non-unit normal, a bad index or incomplete geometry returns bAnswered = false,
 * which OutcomeOf maps to Unanswerable (the zero enumerator). A NaN MaxShearStrengthMPa is refused,
 * not treated as uncapped.
 *
 * Scope: in 2D, Y is the wythe and enters only through areas; a joint normal with a Y component is
 * refused. Memory is O(nonzeros + fill), enough for every owned fixture; the measured envelope is in
 * RigidBlockOracleSweepTest.cpp.
 */
namespace RigidBlockOracle
{
	/** 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa over 1 cm2 is 10000 uu. Derived independently of production. */
	constexpr double OracleForceUnitsPerMPaSqCm = 100.0 * 100.0;

	/*
	 * Two independently derived constants proven equal, without sharing the symbol. A production
	 * factor off by 100x breaks the build instead of passing a tuned sweep.
	 */
	static_assert(
		OracleForceUnitsPerMPaSqCm == DestructionForce::ForceUnitsPerMPaSqCm,
		"the oracle's independently derived MPa->uu conversion must agree with production's; "
		"if this fires, one of the two is wrong — do NOT unify them into one symbol");

	/** MassKg * 980 is the weight in uu; the newton conversion is already inside it. */
	constexpr double OracleGravityCmPerSecondSquared = 980.0;

	/**
	 * Upper bound on lambda. Dry stone under gravity is feasible at every lambda, so the cap turns
	 * an unbounded LP into a number. lambda* at the cap means "stands at any load", nothing finer.
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
		 * Makes this block's weight a live load (scales with lambda) under otherwise-dead gravity
		 * (PROMOTION_DESIGN Slice 6d). Poses a surcharge such as the shed's roof live while the rest
		 * stays dead, which the global flag cannot. Live iff bGravityIsLive OR bLiveGravity, so it
		 * can only add liveness.
		 */
		bool bLiveGravity = false;

		/** 3D only: centroid plan-Y. Declared last so existing brace inits are unchanged; inert in 2D. */
		double CentroidYCm = 0.0;
	};

	/**
	 * A joint as a 2D contact segment plus strengths. The normal points BlockA to BlockB, as in
	 * FConnection; forces are on BlockB. The two contacts sit at Centre -/+ HalfLength along the
	 * tangent (-Nz, Nx), in that order, which is part of the determinism contract. AreaSqCm is the
	 * full 3D face area.
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

		/** Half the segment length, cm. Zero is a valid point contact that carries no moment. */
		double HalfLengthCm = 0.0;

		double AreaSqCm = 0.0;

		/**
		 * 3D only, declared last so the 2D path is unchanged: Y components of the normal and centre,
		 * and the patch's in-plane half-extents (both zero is a point patch). Unread in 2D.
		 */
		double NormalY = 0.0;
		double CentreYCm = 0.0;
		double HalfUCm = 0.0;
		double HalfVCm = 0.0;

		/** Strength profile in MPa, as production stores it. */
		FConnectionStrength Strength;
	};

	/** An external point force on one block (a push, a surcharge). Its moment is derived from At. */
	struct FOracleAppliedForce
	{
		int32 Block = INDEX_NONE;
		double ForceXUu = 0.0;
		double ForceZUu = 0.0;
		double AtXCm = 0.0;
		double AtZCm = 0.0;

		/** Live forces scale with lambda; dead forces are constants of the problem. */
		bool bLive = true;

		/** 3D only, declared last for brace inits. Inert in 2D; the 3D assembler does not pose applied forces yet. */
		double ForceYUu = 0.0;
		double AtYCm = 0.0;
	};

	/**
	 * A simplex basis, so one solve can warm-start the next (PROMOTION_DESIGN §5.4), e.g. after a
	 * brick is deleted. NumStructCols and ArtificialStart give the column layout for mapping onto a
	 * changed problem; the solve validates columns against its own matrix. A warm start is a hint
	 * and must never change lambda* or a verdict; the post-solve verification gate enforces this.
	 */
	struct FOracleBasis
	{
		/** One column index per standard-form row; INDEX_NONE = use the cold default. */
		TArray<int32> Columns;

		/** Number of structural columns (where slacks begin). */
		int32 NumStructCols = 0;

		/** Structural plus slack columns (where artificials begin). */
		int32 ArtificialStart = 0;
	};

	/** Which physics the assembler poses. Dim2D is the zero default; Dim3D routes to AssembleThreeD. */
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

		EOracleDim Dim = EOracleDim::Dim2D;

		/** Warm-start basis. Empty means a cold start, which must stay bit-identical (the sweep's pivot pins). */
		FOracleBasis StartingBasis;

		/** True: lambda scales gravity. False: gravity is dead and lambda scales only live applied forces. */
		bool bGravityIsLive = true;

		/**
		 * Adds first-crack rows, -(n1+n2) + 3|n1-n2| <= f_t * A, for every joint with f_t > 0, cutting
		 * bonded bending capacity to one third (PROMOTION_DESIGN §4.3). f_t = 0 joints are unchanged.
		 */
		bool bFirstCrackRows = false;

		/**
		 * Solve the min-violation LP for the per-joint strain readout (PROMOTION_DESIGN §3.5) instead
		 * of maximise-lambda, whose primal reads every joint at or under 1.0. Fixes lambda = 1 (pose
		 * with bGravityIsLive = false), keeps equilibrium hard, adds slack s_k to each strength row and
		 * minimises sum w_k*s_k. Always feasible; slack is positive only on over-capacity joints. Off
		 * leaves the maximise-lambda solve bit-identical. See FOracleReadout.
		 */
		bool bMinViolationReadout = false;

		/**
		 * Bridge provenance: the FStructure piece/connection each oracle block/joint came from, so the
		 * mechanism can name them. The solver never reads these. Empty on a hand-built problem.
		 */
		TArray<int32> PieceOfBlock;
		TArray<int32> ConnectionOfJoint;
	};

	/** Verdict. Unanswerable is the zero enumerator so a zero-filled result never reads as standing. */
	enum class EOracleOutcome : uint8
	{
		Unanswerable,
		Falls,
		Stands,
	};

	/**
	 * Why the oracle refused. None is the zero enumerator. The three PhaseTwo arms are separate
	 * because they have different fixes and production counts refusals by reason (§5.6, §11 R4).
	 */
	enum class EOracleRefusal : uint8
	{
		None = 0,

		/** Input validation refused the problem before any solving happened. */
		InvalidProblem,

		/** Phase 1 did not reach an optimum, or its factorisation refused. */
		PhaseOneFailure,

		/** Phase 2 hit MaxPivots, the termination guarantee. */
		PhaseTwoIterationCap,

		/** Phase 2 found no positive ratio-test entry on a lambda-capped problem. */
		PhaseTwoUnbounded,

		/** Phase 2's basis went singular under refactorisation. */
		PhaseTwoNumericalFailure,

		/** The optimal basis failed the post-solve check against the original rows. */
		VerificationFailure,
	};

	/** The reason as a phrase, distinct for every refusing enumerator and empty for None. */
	FString RefusalText(EOracleRefusal Refusal);

	/**
	 * One block's virtual motion in the collapse mechanism: the phase-1 dual triple read off its
	 * equilibrium rows (PROMOTION_DESIGN §3.3). (VirtualUx, VirtualUz) is the centroid's translation,
	 * VirtualOmega its rotation. bMoves is true iff the normalized triple clears the relative
	 * threshold tau. Grounded blocks have no rows and never move.
	 */
	struct FOracleMechanismBlock
	{
		double VirtualUx = 0.0;
		double VirtualUz = 0.0;
		double VirtualOmega = 0.0;
		bool bMoves = false;

		/**
		 * 3D only, declared last; zero in 2D. Y translation and rotation about X and Z, so omega =
		 * (VirtualOmegaX, VirtualOmega, VirtualOmegaZ) (THREED_DESIGN).
		 */
		double VirtualUy = 0.0;
		double VirtualOmegaX = 0.0;
		double VirtualOmegaZ = 0.0;
	};

	/**
	 * Collapse mechanism from the infeasible arm's phase-1 dual, a Farkas certificate that is the
	 * kinematic mechanism in limit analysis (Livesley 1978; PROMOTION_DESIGN §3.3). Moving blocks
	 * fall; opening or sliding joints are the break set.
	 *
	 * Raw dual multipliers are not unique under degeneracy (a 24-course fixture named 8 opening
	 * joints on one column order, 13-16 on another), so joints are read off block velocities
	 * instead: a joint opens or slides iff its blocks have relative velocity at a contact.
	 *
	 * Determinism at scale is not proven. A wall with several simultaneous hinge lines may not give
	 * a permutation-unique field; before wiring one, a multi-mode fixture must show the set stable
	 * or the minimal-support tie-break (§12 D7) must be built.
	 */
	struct FOracleMechanism
	{
		/** One entry per oracle block, in index order. */
		TArray<FOracleMechanismBlock> Blocks;

		/** One entry per oracle joint: true iff a contact opens or slides. */
		TArray<bool> JointOpensOrSlides;

		/**
		 * True once Farkas-verified (yA <= tol, yb > 0, named set non-empty). An uncertified
		 * mechanism is never returned; the solve refuses with VerificationFailure instead (§3.6).
		 */
		bool bIsCertified = false;

		/** True when the feasibility formulation was infeasible and a mechanism was extracted. */
		bool bPresent = false;
	};

	/**
	 * One joint's strain readout from the min-violation LP (PROMOTION_DESIGN §3.5). NormalUu =
	 * n1 + n2 (compression positive) and MomentUuCm = HalfLength * (n1 - n2); equilibrium is hard,
	 * so these balance the dead load even over capacity. ViolationUu is the strength slack charged
	 * here, in force units. Utilisation uses UtilisationUnder's scale: demand / capacity, which is
	 * 1 + ViolationUu / capacity when over.
	 */
	struct FOracleJointReadout
	{
		double NormalUu = 0.0;
		double MomentUuCm = 0.0;
		double ViolationUu = 0.0;
		double Utilisation = 0.0;
	};

	/** Per-joint strain readout, present only when bMinViolationReadout was set. One entry per oracle joint. */
	struct FOracleReadout
	{
		bool bPresent = false;
		TArray<FOracleJointReadout> Joints;
	};

	struct FOracleResult
	{
		/** False on any refusal or solver failure. Lambda is then meaningless but always finite, never NaN. */
		bool bAnswered = false;

		/** Why it refused, when it did. None exactly when bAnswered is true. */
		EOracleRefusal Refusal = EOracleRefusal::None;

		/** lambda*, in [0, LambdaCap]. At the cap means "stands at every multiplier". */
		double Lambda = 0.0;

		/** Simplex pivots taken. */
		int32 SimplexIterations = 0;

		/*
		 * Early-exit measurement: the solver still runs phase 1 to optimality, but these report where
		 * an exit at first feasibility could have fired. Saving = (PhaseOnePivots -
		 * PivotsToFirstFeasible) / PhaseOnePivots (FeasibilityReformulationCost). INDEX_NONE means
		 * not reported, so it cannot read as zero work.
		 */

		/** Pivots phase 1 spent; zero on every gravity-live pose. */
		int32 PhaseOnePivots = INDEX_NONE;

		/** Pivot at which phase 1 first reached feasibility. INDEX_NONE if never, or refused before phase 1. */
		int32 PivotsToFirstFeasible = INDEX_NONE;

		/**
		 * Columns priced against a dual vector, across all phases and the pivot-out pass; budgeted by
		 * the PricingCost test. int64 because a 30-course wall (~34k columns) overflows int32.
		 */
		int64 PricingColumnScans = 0;

		/**
		 * Iterations entered under the Bland fallback (after 500 zero-length steps). Instrumentation
		 * only. The opening-ladder rungs measured 170-358 per solve, so the branch is hot but its
		 * behaviour is untested (see CURRENT_STATE).
		 */
		int32 BlandDegenerateEntries = 0;

		/** Basis the solve ended on, for warm-starting the next. Empty on a refusal, since an uncertified basis is no start point. */
		FOracleBasis FinalBasis;

		/**
		 * StartingBasis entries that survived validation and repair; INDEX_NONE if none was supplied.
		 * Distinguishes a warm start that saved nothing from one discarded. Bad hints are repaired to
		 * the cold default, not refused.
		 */
		int32 WarmStartColumnsAccepted = INDEX_NONE;

		/** Why the oracle refused. Empty on an answered result. */
		FString WhyNot;

		/** Collapse mechanism, when the feasibility formulation was infeasible. */
		FOracleMechanism Mechanism;

		/** Min-violation strain readout, when bMinViolationReadout was set. */
		FOracleReadout Readout;
	};

	/**
	 * In-plane axes (U, V) from a unit normal, with U x V = N. Shared with RigidBlockBridge so both
	 * pose the 3D patch in the same frame. Pure geometry, so sharing it keeps the oracle independent.
	 */
	void DeriveInPlaneAxes(const double N[3], double U[3], double V[3]);

	/** Solve the lower-bound LP. Deterministic: same problem, bit-identical result. */
	FOracleResult SolveRigidBlock(const FOracleProblem& Problem);

	/** Stands iff answered and Lambda >= 1. Unanswerable is never Stands. */
	EOracleOutcome OutcomeOf(const FOracleResult& Result);
}
