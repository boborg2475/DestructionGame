// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Connection.h"

// Forward-declared to keep the oracle header out of Structure.h; defined in Core/RigidBlock.
namespace RigidBlockOracle
{
	struct FOracleProblem;
	struct FOracleResult;
}

// Forward-declared to keep the material library out of Structure.h; defined in Core/Profiles/MaterialProfiles.h.
namespace DestructionProfiles
{
	struct FMaterialProfile;
}

/** A piece of a structure: plain data (mass, handle, grounded flag). No actor or transform. */
struct FStructurePiece
{
	/** Position in the structure's piece array. */
	int32 Index = INDEX_NONE;

	/** Real published values, unconverted (DESIGN.md §3). */
	double MassKg = 0.0;

	/** Where the weight acts, cm. Zero means "not supplied"; see bHasCentreOfMass. */
	FVector CentreOfMassCm = FVector::ZeroVector;

	/** False by default, so an unplaced piece has no eccentricity rather than a lever arm to the origin. */
	bool bHasCentreOfMass = false;

	/** A grounded piece rests on the earth and terminates the flow of load. */
	bool bIsGrounded = false;

	/**
	 * Whether this slot still holds a piece. Removed pieces are tombstoned: handles are array
	 * indices, so compacting would re-point connections.
	 *
	 * Do not add a free list: reusing a slot without a generation counter lets a stale handle
	 * name a different live piece undetectably. False by default so GetPiece's placeholder reads as dead.
	 */
	bool bIsInTheStructure = false;

	/**
	 * For pairing a joint's two face materials (DestructionForce::EffectiveBondedStrength).
	 * Nullptr means not supplied: the joint reads its bare connection. Non-owning; must point at a
	 * program-lifetime profile (MaterialProfiles.cpp), never a temporary.
	 */
	const DestructionProfiles::FMaterialProfile* Material = nullptr;
};

/**
 * Why a piece is or is not held up, refining IsPieceSupported. Separates physics (Falling) from a
 * solver limitation (Stranded, DESIGN.md §3). Falling is zero so a default entry fails closed.
 */
enum class EPieceSupport : uint8
{
	/** Nothing holds it up. Also for no piece, a removed piece, an unsolved structure, or a piece resting on a knot. */
	Falling,

	/** Rests on the earth. */
	Grounded,

	/** Reaches the earth through supports the solver could route. */
	Supported,

	/** In an unroutable support loop; reported unsupported conservatively, not a physical claim. */
	Stranded,
};

/**
 * What a connection is to one of its pieces. Directed: one joint is BedBeneath for the piece above
 * and BedAbove for the piece below. Split at 45 degrees (DESIGN.md §3). None is zero, so a default
 * entry claims no tier.
 */
enum class EJointRole : uint8
{
	/** Not a joint on this piece, or a normal that describes no plane. */
	None,

	/** Vertical normal, other piece below: bears this piece's weight. */
	BedBeneath,

	/** Vertical normal, other piece above. */
	BedAbove,

	/** Horizontal normal: carries weight only in shear. */
	Head,
};

/**
 * The graph of pieces and connections, and what each connection carries.
 *
 * Load flows down: each piece passes its weight plus everything above into its supports, split by
 * interface area; grounded pieces terminate it. Supports are the bed joints beneath a piece, falling
 * back to head joints if it has none (DESIGN.md §3). Given or removed joints leave the support
 * relation, so their share moves to neighbours. A support with no path to earth is not a support.
 *
 * Units: weight in uu is MassKg * 980 (a 2.72 kg brick weighs 2666), which already includes
 * 1 N = 100 uu. Applying it again is a 100x error.
 *
 * SolveLoads is non-destructive and re-runnable; SolveAndBreak breaks. No UObject or world.
 */
struct FStructure
{
	/** Returns the handle, or INDEX_NONE for a negative or non-finite mass. Zero mass is allowed. */
	int32 AddPiece(double MassKg, bool bIsGrounded = false);

	/** As above, with a centre of mass. A non-finite centre is refused, since it would become a NaN lever arm. */
	int32 AddPiece(double MassKg, bool bIsGrounded, const FVector& CentreOfMassCm);

	/**
	 * Returns the handle, or INDEX_NONE. Rejects out-of-range or equal piece handles, a
	 * non-finite or non-positive area (the load split divides by it), and a degenerate normal.
	 *
	 * A supplied rectangle must also be consistent: 4 x h_u x h_v must match the area, no extent on
	 * the normal's axis, finite non-negative values, axis-aligned normal. Zero extents mean "not
	 * measured" and are accepted. Accepted rectangles are stored unmodified, never normalised.
	 */
	int32 AddConnection(const FConnection& Connection);

	/**
	 * Remove a piece; its handle stays valid.
	 *
	 * @return false for a handle that names no piece or one already removed.
	 */
	bool RemovePiece(int32 PieceIndex);

	bool IsPieceRemoved(int32 PieceIndex) const;

	/**
	 * The handle range, which never shrinks; not the live count. Iterate 0..NumPieces() and check
	 * IsPieceRemoved; use NumLivePieces for a count.
	 */
	int32 NumPieces() const;

	/** For counts, never for iteration. */
	int32 NumLivePieces() const;

	int32 NumConnections() const;

	/** How many times this structure has been solved. Test-only cost observable; no production code branches on it. */
	int32 NumSolves() const;

	/**
	 * Whether every live piece and joint has position data. Separates "no eccentricity" from "no
	 * positions supplied", which both give a zero moment. True for an empty structure.
	 */
	bool HasCompleteGeometry() const;

	/** Out-of-range handles return a default-constructed placeholder. */
	const FStructurePiece& GetPiece(int32 PieceIndex) const;
	const FConnection& GetConnection(int32 ConnectionIndex) const;

	/** For the mortar-sensitivity sweep. An out-of-range handle returns a throwaway placeholder. */
	FConnection& GetConnectionMutable(int32 ConnectionIndex);

	/** Recompute connection loads and support. Skips given joints; never breaks anything. */
	void SolveLoads();

	/**
	 * Solve, break every over-capacity joint, re-solve, and repeat until nothing gives. Between
	 * passes is the collapse playback order. Terminates because joints never heal.
	 *
	 * A pass breaks via the per-joint capacity sweep and the equilibrium gate (BreakByEquilibrium,
	 * DESIGN.md §7 step 4), which severs the mechanism when the rigid-block LP finds no equilibrium.
	 * Pass numbers are global across calls, so a shared number means "gave simultaneously".
	 *
	 * @return passes in this call that broke at least one joint; zero if the structure stands.
	 */
	int32 SolveAndBreak();

	/**
	 * The equilibrium gate is authoritative at or below this block count and declines to the
	 * router above it (PROMOTION_DESIGN.md §12 D6-c). Tests lower it to reach the decline arm.
	 */
	void SetEquilibriumGateBlockCap(int32 MaxBlocks);

	/**
	 * Max |region ∪ grounded boundary| the regional prover may pose in the above-cap arm
	 * (REGIONAL_PROVER_PLAN.md §4). A larger mechanism is an accepted miss; the prover only moves
	 * toward collapse, so a miss never over-holds.
	 */
	void SetRegionBlockCap(int32 MaxBlocks);

	/** Out-of-range handles are ignored; null clears back to "not supplied". */
	void SetPieceMaterial(int32 PieceIndex, const DestructionProfiles::FMaterialProfile* Material);

	/**
	 * Flag the structure 3D, so the oracle bridge poses the 3D LP and accepts Y joint normals.
	 * Stated, not inferred, so a 2D structure with a stray Y normal is refused, not promoted.
	 */
	void SetThreeDimensional(bool bIsThreeDimensional);

	/** False by default (2D). */
	bool IsThreeDimensional() const;

	/**
	 * Test entry for the regional collapse prover (REGIONAL_PROVER_PLAN.md slice 1), outside the
	 * real cascade. Runs SolveLoads, then ProveRegionalCollapse. With RegionBlockCap >= the block
	 * count the region is the whole structure and matches the ground-only LP.
	 *
	 * @param Seed           The pieces the region grows from.
	 * @param RegionBlockCap The largest region the flood may reach.
	 * @return pieces marked Falling this call.
	 */
	int32 SolveAndBreak_WithRegionalProver(const TArray<int32>& Seed, int32 RegionBlockCap);

	/**
	 * |region ∪ grounded boundary| of the last regional pose. Only stamped when a problem is posed,
	 * so a refused pose keeps the previous value. INDEX_NONE until the first.
	 */
	int32 GetLastRegionalProblemBlockCount() const;

	/**
	 * Dimension of the last equilibrium-gate pose: 2, 3, or INDEX_NONE before any. 2D when every
	 * posed joint normal is in-plane at one common Y (DESIGN §8). An int because EOracleDim cannot
	 * be forward-declared here.
	 */
	int32 GetLastEquilibriumProblemDim() const;

	/** Per-phase wall-clock ms of one solve. Observability only; never assert on thresholds. */
	struct FSolveLoadsProfile
	{
		/** Per-piece joint lists and two-tier support lists. */
		double SupportListsMs = 0.0;

		/** ReseatSpannedGroups. */
		double ReseatMs = 0.0;

		/** The reachability / load-split fixpoint, all iterations. */
		double FixpointMs = 0.0;

		int32 FixpointIterations = 0;

		/** Per iteration: pieces supported, overturned, stranded, and released from refused arches. */
		TArray<int32> SupportedPerIteration;
		TArray<int32> OverturnedPerIteration;
		TArray<int32> StrandedPerIteration;
		TArray<int32> ReleasedPerIteration;

		/** ApplyArchingThrust. */
		double ArchingMs = 0.0;

		double TotalMs = 0.0;
	};

	/** Cost of one LP the regional prover posed. Test-only observability. */
	struct FProverPoseReport
	{
		/** |region ∪ grounded boundary| for this pose. */
		int32 Blocks = INDEX_NONE;

		/** Result.SimplexIterations. */
		int32 LpPivots = 0;

		double LpMs = 0.0;

		/** Whether this pose was a certified fall. */
		bool bFell = false;
	};

	/**
	 * One cascade pass: SolveLoads, the equilibrium gate, then (only if the gate declined) the
	 * capacity sweep and regional prover.
	 */
	struct FBreakPassReport
	{
		/** The global stamp written on every joint severed this pass. */
		int32 Pass = 0;

		FSolveLoadsProfile Solve;

		double GateMs = 0.0;

		/** EEquilibriumGateDisposition as an int: 0 declined to the router. */
		int32 GateDisposition = 0;

		int32 JointsSeveredByGate = 0;

		double CapacitySweepMs = 0.0;

		int32 JointsGivenToSweep = 0;

		double RegionalProverMs = 0.0;

		/** Prover totals this pass: poses, pivots, LP ms. */
		int32 RegionalPoses = 0;
		int32 RegionalLpPivots = 0;
		double RegionalLpMs = 0.0;

		/** Blocks in the prover's final pose, or INDEX_NONE. */
		int32 RegionalLastBlocks = INDEX_NONE;

		/** Whether the prover's final pose was a certified fall. */
		bool bRegionalFell = false;

		/** One entry per prover pose, in order; sums to the Regional* totals above. */
		TArray<FProverPoseReport> RegionalPoseBreakdown;

		int32 JointsSeveredByProver = 0;
		int32 PiecesFelledByProver = 0;

		/** State after this pass. */
		int32 LivePieces = 0;
		int32 IntactJointsAfter = 0;
		int32 NotHeldAfter = 0;

		double PassMs = 0.0;
	};

	/**
	 * What the last SolveAndBreak did, pass by pass. Observability only. Includes the terminal
	 * pass, so a standing structure reports one pass and zero breaks.
	 */
	struct FSolveAndBreakReport
	{
		double TotalMs = 0.0;

		/** SolveAndBreak's return value. */
		int32 BreakingPasses = 0;

		int32 LivePiecesBefore = 0;
		int32 IntactJointsBefore = 0;
		int32 IntactJointsAfter = 0;

		/** Every pass, breaking or terminal, in order. */
		TArray<FBreakPassReport> Passes;
	};

	/** Default-constructed if none has run. */
	const FSolveAndBreakReport& GetLastSolveAndBreakReport() const;

	const FSolveLoadsProfile& GetLastSolveLoadsProfile() const;

	/**
	 * The breaking pass that gave this joint, from 1, or INDEX_NONE. INDEX_NONE is not "intact":
	 * a joint that went with a removed piece also has none. With HasGiven:
	 *
	 *     intact              HasGiven false, INDEX_NONE
	 *     went with a piece   HasGiven true,  INDEX_NONE
	 *     broke in pass N     HasGiven true,  N >= 1
	 */
	int32 GetBreakPass(int32 ConnectionIndex) const;

	/**
	 * Which authority severed a joint: 1 the equilibrium gate, 2 the capacity sweep, 3 the
	 * regional prover. INDEX_NONE if intact or it went with a removed piece. Observability only.
	 */
	int32 GetBreakAuthority(int32 ConnectionIndex) const;

	/**
	 * The solver's own two-tier decision for this connection and piece, not a re-derived copy.
	 * Pure geometry, so it answers before any solve and for given joints. None for a bad handle,
	 * a piece not on the joint, or a degenerate normal.
	 */
	EJointRole GetJointRole(int32 ConnectionIndex, int32 PieceIndex) const;

	/**
	 * Force on this connection after SolveLoads, uu. Normally vertical; ApplyForce resolves it
	 * against the normal. At an arch springing it is (H, 0, -V), and the horizontal part is shear
	 * (ARCHING_DESIGN.md). The force acts on PieceB (ConnectionLoad.h); the wrong end turns
	 * compression into tension. Zero for a bad handle or a joint with no path to ground.
	 */
	FVector GetConnectionForce(int32 ConnectionIndex) const;

	/**
	 * Bending moment about the joint centroid after SolveLoads, uu.cm. The identity:
	 *
	 *     GetConnectionUtilisation(I)
	 *         == GetConnection(I).UtilisationUnder(
	 *                GetConnectionForce(I), GetConnectionMoment(I),
	 *                GetConnectionCompositeDepthCm(I))
	 *
	 * Those parameters default to zero, so an assertion that omits them is silently wrong. Zero is
	 * exact "no eccentricity" (centred load, unplaced piece, or unmeasured rectangle); use
	 * HasCompleteGeometry to tell them apart. Zero for a bad handle or before any solve.
	 */
	FVector GetConnectionMoment(int32 ConnectionIndex) const;

	/**
	 * Depth of bonded masonry over this connection after SolveLoads, cm; the third input in the
	 * identity above. Non-zero only on a bent bed joint. Zero means none measured and fails closed:
	 * the relief is withheld, so the joint reads more loaded. Zero for a bad handle or before any solve.
	 */
	double GetConnectionCompositeDepthCm(int32 ConnectionIndex) const;

	/**
	 * Utilisation under the last solve's load, via FConnection::UtilisationUnder. Zero for a given
	 * joint (check HasGiven) and before any solve. Fails closed to TNumericLimits<double>::Max() for
	 * a bad handle, since zero would read as healthy.
	 */
	double GetConnectionUtilisation(int32 ConnectionIndex) const;

	/**
	 * The bare connection strength paired with both face materials by the weakest-link rule
	 * (SHED_PATH.md Phase B/B3). The one pairing point for both the router and the LP bridge, so
	 * they agree. Bare strength unless both faces have a material. Computed on demand.
	 */
	FConnectionStrength EffectiveJointStrength(int32 ConnectionIndex) const;

	/**
	 * One connection's strain readout from the cached min-violation LP (PROMOTION_DESIGN.md §3.5),
	 * so the overlay shows the LP's forces rather than the router's estimate. NormalUu is
	 * compression-positive; ViolationUu is strength slack (0 within capacity); Utilisation uses the
	 * same scale as GetConnectionUtilisation. bPresent is false until a below-cap settle; otherwise
	 * the overlay falls back to GetConnectionUtilisation.
	 */
	struct FConnectionReadout
	{
		bool bPresent = false;
		double NormalUu = 0.0;
		double MomentUuCm = 0.0;
		double ViolationUu = 0.0;
		double Utilisation = 0.0;
	};

	FConnectionReadout GetConnectionReadout(int32 ConnectionIndex) const;

	/**
	 * Min-violation readout LPs run since the last SolveAndBreak began (across all its passes).
	 * Test scaffolding; drives no behaviour.
	 */
	int32 GetMinViolationReadoutSolveCount() const;

	/**
	 * Whether this piece reaches ground through supports after SolveLoads. Pieces in a support
	 * cycle cannot be ordered, so they read unsupported; a piece beneath the knot keeps its support
	 * (DESIGN.md §3). False for a bad handle. Only SolveLoads writes this:
	 *
	 *     never solved      false, for every handle
	 *     removed           the last solve's answer, unchanged, until the next solve
	 *     after that solve  false, and a removed grounded piece is no longer earth
	 *
	 * The middle row is deliberately stale; re-solve before deciding what to release.
	 * Structure.RemovedPieceSupportNeedsASolve pins all three rows.
	 */
	bool IsPieceSupported(int32 PieceIndex) const;

	/**
	 * Why this piece is or is not held up, after SolveLoads. Always consistent with:
	 *
	 *     IsPieceSupported(H)  ==  (GetPieceSupport(H) is Grounded or Supported)
	 *
	 * Stranded is only for pieces in the knot; a piece resting on one is Falling. There is no
	 * Removed value (use IsPieceRemoved). Falling for a bad handle.
	 */
	EPieceSupport GetPieceSupport(int32 PieceIndex) const;

	/**
	 * Whether the last solve computed a support state for this handle, since Falling also means
	 * "not yet asked". False before any solve, for handles added since, and for bad handles; true
	 * for a removed piece the last solve covered.
	 *
	 * Anything that turns "not held up" into an irreversible action must check this first: default
	 * Falling would release the whole structure, foundation included.
	 */
	bool HasSupportAnswer(int32 PieceIndex) const;

private:
	/**
	 * One spanned opening: the seated pieces at its two ends. H is applied equally and oppositely
	 * at both ends, so both are needed at once; applying it to one only creates a net force from
	 * nowhere (ARCHING_DESIGN trap 2). Which end is 0 is arbitrary.
	 */
	struct FSpannedArch
	{
		/** Seated pieces at each end. Both non-empty; one-sided is a cantilever. */
		TArray<int32> Abutments[2];

		/** Unit direction toward end 0. One vector, so +H*D and -H*D cancel exactly. */
		FVector TowardEndZero = FVector::ZeroVector;

		/**
		 * L, cm, between the mean abutment centres at each end. That is the clear opening in running
		 * bond; seat centroid to seat centroid reads 5% high on a ten-cell hole.
		 */
		double SpanCm = 0.0;
	};

	/**
	 * Re-seat pieces a hole left with no seat onto their group's abutments; the one place the
	 * solver reads geometry to route. Otherwise the middle bricks over a wide hole fall back to
	 * head joints, support each other, and are stranded. Contiguous unseated pieces form a group
	 * via intact head joints, and each re-seats toward its nearer abutment (acyclic). No-op without
	 * HasCompleteGeometry(). GetJointRole is unaffected.
	 *
	 * @param PieceJoints          Every joint touching each piece, in ascending index order.
	 * @param PieceHasNoSeat       Live, ungrounded pieces with no intact bed joint beneath.
	 * @param SupportConnections   Rewritten in place for abutted group members only.
	 * @param PieceReseatedOnAnArch Set for each piece rewritten.
	 * @param PieceInRefusedArchGroup Set for members of a group refused as one-sided, so SolveLoads
	 *                             reads them Falling rather than Stranded.
	 * @param Arches               One entry per spanning group. Emptied first.
	 */
	void ReseatSpannedGroups(
		const TArray<TArray<int32>>& PieceJoints,
		const TArray<bool>& PieceHasNoSeat,
		TArray<TArray<int32>>& SupportConnections,
		TArray<bool>& PieceReseatedOnAnArch,
		TArray<bool>& PieceInRefusedArchGroup,
		TArray<FSpannedArch>& Arches) const;

	/**
	 * Push each spanned opening's abutments apart by the arch thrust (ARCHING_DESIGN.md):
	 *
	 *     d_e = min( cover above the span , 0.866 * L )      arching depth
	 *     r   = d_e / 3                                      thrust line rise, kern-limited
	 *     W   = the load the solver already accumulated      NOT a triangle
	 *     H   = W * L / (8r)      V = W / 2                  per abutment
	 *
	 * The thrust is shear on the bed joint, resisted by the existing `c + mu*sigma_n`, so dry
	 * stone (zero cohesion) never arches. Runs after accumulation, so vertical answers are
	 * unchanged. H grows as 1/cover, so openings near the top of a wall cannot arch.
	 *
	 * @param PieceJoints Every joint touching each piece, in ascending index order.
	 * @param Arches      From ReseatSpannedGroups; usually empty.
	 */
	void ApplyArchingThrust(
		const TArray<TArray<int32>>& PieceJoints,
		const TArray<FSpannedArch>& Arches);

	/**
	 * The live piece resting on this one via an intact bed joint, or INDEX_NONE. Shared by
	 * MasonryDepthAboveCm and SolveLoads so they cannot diverge.
	 */
	int32 PieceRestingOn(int32 Piece, const TArray<TArray<int32>>& PieceJoints) const;

	/**
	 * Depth of bonded masonry over a bed joint, cm, by an upward walk over bed joints. Used for
	 * arch cover (d_e) and for composite depth over a seat (deep beam, t*D^2/6).
	 *
	 * Counted in courses including the joint's own, so the minimum is one course pitch. A given
	 * joint stops the walk. Bounded by EnoughDepthCm and the piece count (guards a cyclic graph).
	 * Follows the first piece above by joint index, so a stepped wall's answer depends on the
	 * column taken (untested). Zero if nothing to measure.
	 *
	 * @param Piece         The piece standing on the joint.
	 * @param SeatJointIndex Its bed joint.
	 * @param PieceJoints   Every joint touching each piece, in ascending index order.
	 * @param EnoughDepthCm Past this depth the caller's answer cannot change.
	 */
	double MasonryDepthAboveCm(
		int32 Piece,
		int32 SeatJointIndex,
		const TArray<TArray<int32>>& PieceJoints,
		double EnoughDepthCm) const;

	/**
	 * Depth of the corbelling body on a bed joint, cm: the floor under the composite depth that
	 * no lever arm may trim, since those courses generate the moment:
	 *
	 *     D  =  min( masonry above , max( THIS , lambda*|M|/|F| ) )
	 *
	 * The walk stops at the first non-corbelling course. Corbelling means seated on exactly one
	 * course. It follows the corbelling chain, not PieceRestingOn, which on a filled corbel would
	 * take the 2 cm inboard lap instead of the 18 cm seat.
	 *
	 * ⚠ Every piece in a stack-bond column or one-brick-wide wall counts as corbelling, so the floor
	 * becomes the whole wall. Unfixed because zero eccentricity there means this is never reached
	 * (CURRENT_STATE.md).
	 *
	 * @param Piece          The body's first course, standing on the joint.
	 * @param SeatJointIndex Its bed joint.
	 * @param PieceJoints    Every joint touching each piece, in ascending index order.
	 */
	double CorbellingBodyDepthCm(
		int32 Piece,
		int32 SeatJointIndex,
		const TArray<TArray<int32>>& PieceJoints) const;

	/**
	 * Whether a piece that lost a seat can arch rather than cantilever: it needs an intact head
	 * joint on the eccentric side to a neighbour that reaches ground and is not resting on this
	 * piece (a re-seated arch neighbour counts). FConnection::ArchingMomentScale handles the
	 * single-joint part.
	 *
	 * The seat must also be able to supply the deleted couple `(1-k)*|M|`, carrying `dM/z` against
	 * `c + mu*sigma_n`; otherwise the relief is withheld, so dry stone cannot (DESIGN.md §7 gap 4).
	 * Spanned groups are handled by ApplyArchingThrust instead, not here.
	 *
	 * @param PieceIndex            The loaded piece; must be placed.
	 * @param BedJoint              Its one seat; must have a rectangle.
	 * @param SeatForceUu           Force on the seat, as stored.
	 * @param DeletedCoupleUuCm     `(1-k)*|M|`, a magnitude.
	 * @param PieceJoints           Every joint touching each piece, in ascending index order.
	 * @param SupportConnections    Supports before the reaching-ground filter.
	 * @param PieceReseatedOnAnArch Pieces ReseatSpannedGroups re-seated.
	 */
	bool HasArchingAbutment(
		int32 PieceIndex,
		const FConnection& BedJoint,
		const FVector& SeatForceUu,
		double DeletedCoupleUuCm,
		const TArray<TArray<int32>>& PieceJoints,
		const TArray<TArray<int32>>& SupportConnections,
		const TArray<bool>& PieceReseatedOnAnArch) const;

	/**
	 * Whether a piece on compression-only supports overturns because its centre of mass lies
	 * outside their contact region; no per-joint number shows this. Any tension-capable support
	 * (read from FConnectionStrength, so data-driven) holds it down; that is what stands the porch
	 * overhang on its screwed cleat. The region is a conservative axis-aligned bound of the contacts.
	 *
	 * @param PieceIndex The loaded piece.
	 * @param LoadPath   Its supports that reach ground.
	 * @return true only if certainly outside; false on missing or non-finite geometry (fail closed).
	 */
	bool PieceOverturnsOffItsSupports(int32 PieceIndex, const TArray<int32>& LoadPath) const;

	/**
	 * Whether the equilibrium gate answered (skip the capacity sweep) or declined (the sweep
	 * decides). Below the block cap the LP is the only break authority (PROMOTION_DESIGN.md §3.7).
	 * DeclinedToRouter is zero, so a default value fails closed to the sweep.
	 */
	enum class EEquilibriumGateDisposition : uint8
	{
		/** Over cap, no geometry, bridge refused, or LP unanswerable. */
		DeclinedToRouter = 0,

		/** The LP answered and severed nothing this pass. */
		AuthoritativeNoBreak,

		/** The LP answered Falls and severed the mechanism's joints. */
		AuthoritativeBroke,
	};

	/**
	 * The equilibrium gate (DESIGN.md §7 step 4, PROMOTION_DESIGN.md §6 Slice 3). Below the block
	 * cap, asks the rigid-block LP whether any equilibrium with self-weight exists; on "no", uses
	 * the Farkas certificate (the collapse mechanism) to sever the joints it opens or slides.
	 * Catches what per-joint checks miss, e.g. a body whose resultant has left its bearings
	 * (DESIGN.md §5.7).
	 *
	 * Poses feasibility at lambda = 1. No-op without complete geometry or on LP refusal (fail
	 * closed). On an answer it rewrites support so GetPieceSupport is LP-authoritative below the cap.
	 *
	 * @param Pass The pass any break is stamped with.
	 */
	EEquilibriumGateDisposition BreakByEquilibrium(int32 Pass);

	/**
	 * The regional collapse prover (REGIONAL_PROVER_PLAN.md §§1-4), used by the test entry and the
	 * cascade. Floods a region from Seed up to RegionBlockCap, grounds the one-hop frontier, and
	 * poses it via BuildRegionalProblem. On a certified Falls it marks moved pieces Falling (never
	 * Supported) and severs the mechanism's joints. Does not solve loads; the caller has settled
	 * the graph.
	 *
	 * @param Seed           The pieces the region grows from.
	 * @param RegionBlockCap The largest region the flood may reach.
	 * @param BreakPass      Stamp for severed joints (default 1 for the isolated tests).
	 * @return pieces marked Falling this call.
	 */
	int32 ProveRegionalCollapse(const TArray<int32>& Seed, int32 RegionBlockCap, int32 BreakPass = 1);

	/**
	 * Rewrite support from the LP verdict so GetPieceSupport is LP-authoritative below the cap
	 * (PROMOTION_DESIGN.md §3b), overriding the router's Stranded.
	 */
	void ApplyLimitAnalysisSupport(
		const RigidBlockOracle::FOracleProblem& Problem,
		const RigidBlockOracle::FOracleResult& Result);

	/**
	 * Solve and cache the min-violation strain readout (PROMOTION_DESIGN.md §3.5). A separate LP,
	 * so it changes no break decision, support or force.
	 */
	void CacheMinViolationReadout(const RigidBlockOracle::FOracleProblem& Problem);

	/**
	 * Stamp every over-capacity joint with this pass. The break authority above the block cap and
	 * on LP refusal. Returns whether any joint gave.
	 */
	bool BreakByCapacitySweep(int32 Pass);

	/**
	 * Keeps synchronous LP authority off the flagship scenarios (D6-c): the medium wall fixtures
	 * (125-174 blocks) are in scope, the 375-block flagship is not. See CURRENT_STATE for latency.
	 */
	int32 EquilibriumGateBlockCap = 200;

	/*
	 * Max |region ∪ grounded boundary| for the regional prover (REGIONAL_PROVER_PLAN.md §4). This
	 * is the per-pass LP cost, so it should stay modest. Named differently from the RegionBlockCap
	 * parameter to avoid the C4458 shadow warning.
	 */
	int32 RegionalProverBlockCap = 200;

	// Backs GetLastRegionalProblemBlockCount.
	int32 LastRegionalProblemBlockCount = INDEX_NONE;

	// Backs GetLastEquilibriumProblemDim.
	int32 LastEquilibriumProblemDim = INDEX_NONE;

	/** Observability, never authority. */
	FSolveAndBreakReport LastSolveAndBreakReport;
	FSolveLoadsProfile LastSolveLoadsProfile;

	/** Last ProveRegionalCollapse's stats, for the pass report. */
	int32 LastProverPoses = 0;
	int32 LastProverLpPivots = 0;
	double LastProverLpMs = 0.0;
	bool bLastProverFell = false;
	int32 LastProverJointsSevered = 0;

	// Per-pose breakdown of the above; reset per ProveRegionalCollapse call.
	TArray<FProverPoseReport> LastProverPoseBreakdown;

	// See SetThreeDimensional (THREED_DESIGN.md E3).
	bool bThreeDimensional = false;

	TArray<FStructurePiece> Pieces;
	TArray<FConnection> Connections;

	/**
	 * Solver output, parallel to the arrays above and rebuilt every solve. Kept separate so nothing
	 * can read a cached answer before a solve produces it.
	 */

	TArray<bool> PieceSupported;

	/** In an unroutable knot, not merely resting on one. Never both stranded and supported. */
	TArray<bool> PieceStranded;

	/** Unreal force units. */
	TArray<FVector> ConnectionForces;

	/**
	 * Moment about each joint centroid, uu.cm; kept out of ConnectionForces so the force stays
	 * accurate. Non-zero only where statics is determinate (a placed piece's load reaching ground
	 * through exactly one joint).
	 */
	TArray<FVector> ConnectionMoments;

	/** Composite depth per connection, cm. Non-zero only on bent bed joints. */
	TArray<double> ConnectionCompositeDepthCm;

	/**
	 * Breaking pass per connection, from 1, or INDEX_NONE. Never cleared: joints never heal, and
	 * this is the only record of collapse order.
	 */
	TArray<int32> ConnectionBreakPass;

	/** Which authority severed each connection; index-aligned with ConnectionBreakPass. Observability only. */
	TArray<int32> ConnectionBreakAuthority;

	/**
	 * Cached min-violation readout per connection (PROMOTION_DESIGN.md §3.5). Cleared by
	 * SolveAndBreak. From a different LP than the verdict, so it never feeds a break decision.
	 */
	TArray<FConnectionReadout> ConnectionReadoutCache;

	// Backs GetMinViolationReadoutSolveCount.
	int32 MinViolationReadoutSolves = 0;

	/** SolveLoads entries, ever. See NumSolves. The one field a solve accumulates; never cleared. */
	int32 SolveCount = 0;
};
