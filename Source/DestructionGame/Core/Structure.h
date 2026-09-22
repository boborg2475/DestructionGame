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

/**
 * A piece of a structure: plain data. No actor, component or transform — a mass, an
 * identity connections refer to, and a grounded flag. The world owner resolves the handle
 * to something visible; the solver never needs to.
 */
struct FStructurePiece
{
	/** Position of this piece in its structure's piece array. */
	int32 Index = INDEX_NONE;

	/** Mass in kilograms. Real published values, unconverted — see DESIGN.md §3. */
	double MassKg = 0.0;

	/**
	 * Where this piece's weight acts, cm. Zero is "not supplied", not the origin — see
	 * bHasCentreOfMass.
	 */
	FVector CentreOfMassCm = FVector::ZeroVector;

	/**
	 * Whether anyone supplied that centre of mass. False by default, so an unplaced piece
	 * carries no eccentricity rather than inventing a lever arm from a defaulted origin.
	 */
	bool bHasCentreOfMass = false;

	/** Whether this piece rests on the earth; a grounded piece terminates the flow of load. */
	bool bIsGrounded = false;

	/**
	 * Whether this slot still holds a piece. A removed piece is tombstoned: the slot and
	 * handle stay valid forever and nothing compacts down, since handles are array indices
	 * and compacting would re-point every connection above the hole.
	 *
	 * Do not add a free list: reusing a slot without a generation counter makes a stale
	 * handle name a different live piece undetectably. Reuse needs generational handles
	 * (index plus a bumped counter), not a free list. Bounded scenarios today, so the leak
	 * is fine.
	 *
	 * False by default so GetPiece's placeholder reads as dead, fail-closed like every accessor.
	 */
	bool bIsInTheStructure = false;

	/**
	 * What this piece is made of, so a joint can pair its two faces' materials through
	 * DestructionForce::EffectiveBondedStrength. Nullptr means "not supplied", the fail-safe
	 * default: absent a material, a joint reads its bare connection.
	 *
	 * Non-owning; must point at a static/program-lifetime profile (the `extern const`
	 * definitions in MaterialProfiles.cpp). Never store the address of a temporary.
	 */
	const DestructionProfiles::FMaterialProfile* Material = nullptr;
};

/**
 * Why a piece is or is not held up, refining IsPieceSupported. Distinguishes real physics
 * (nothing holds it up) from a solver limitation (an unroutable knot, reported
 * conservatively — DESIGN.md §3), which lets a collapse test assert a wall fell from load
 * rather than from a stranded loop.
 *
 * Falling is first so zero is fail-closed: a default entry reads "nothing holds this up",
 * not "rests on the earth".
 */
enum class EPieceSupport : uint8
{
	/**
	 * Nothing is holding this piece up, and the solver is not the reason. Also the answer for
	 * no piece, a removed piece, an unsolved structure, and a piece resting on a knot (it lost
	 * its support — ordinary physics).
	 */
	Falling,

	/** Resting on the earth. Ground terminates the flow of load; nothing carries this. */
	Grounded,

	/** Reaches the earth through supports, and the solver could route its load there. */
	Supported,

	/**
	 * In an unroutable knot: the piece is ultimately one of its own supports. Not physics
	 * (the point of telling it apart from Falling) — dividing load round a loop needs a rule
	 * that does not exist yet, so it is reported unsupported conservatively.
	 */
	Stranded,
};

/**
 * What a connection is to one of its pieces (the two-tier classification). Directed: the
 * same joint is a bed joint beneath the piece above and above the piece below, so a role
 * needs a whose. DESIGN.md §3: a substantially vertical normal bears in compression, a
 * horizontal one only in shear, split at 45 degrees.
 *
 * None is the zero enumerator, so a zero-filled entry claims no tier, not the strongest.
 */
enum class EJointRole : uint8
{
	/** Not a joint on this piece at all, or a normal that describes no plane. */
	None,

	/** Substantially vertical normal, other piece BELOW: this joint bears the weight. */
	BedBeneath,

	/** Substantially vertical normal, other piece ABOVE: something rests on this piece. */
	BedAbove,

	/** Substantially horizontal normal: it can only carry weight in shear. */
	Head,
};

/**
 * The graph that owns pieces and connections and works out what each connection carries.
 *
 * Load flows downward: a piece transmits its own weight plus everything from above into
 * the connections that support it; a grounded piece terminates the flow, and where several
 * connections support a piece the load splits weighted by interface area.
 *
 * Support is two-tiered by joint orientation (DESIGN.md §3): a vertical normal is a bed
 * joint bearing in compression, a horizontal one a head joint carrying only in shear. A
 * piece's supports are the bed joints beneath it, falling back to head joints when it has
 * none. A given or removed joint leaves the support relation, so its share moves onto the
 * neighbours rather than evaporating; a removed piece was not overloaded, so it never
 * enters the collapse sequence (see GetBreakPass).
 *
 * Everything is over that support relation, not raw connectivity: a support that is itself
 * falling is not a support, so a share handed to one with no path to earth is lost.
 * "Supported" means reaching ground through supports, and pieces have no position here, so
 * two can each hang from the other and neither be held up.
 *
 * Units. Gravity is -980 cm/s2 and mass is kg, so weight in uu is MassKg * 980 — a 2.72 kg
 * brick weighs 2666. That already includes the 1 N = 100 uu conversion; applying it a
 * second time is the standard way to be wrong by exactly 100x here.
 *
 * SolveLoads computes loads without breaking, so it can be re-run; breaking is the
 * separate SolveAndBreak. A plain struct with no UObject or world, so the load maths stays
 * plain arithmetic over a graph — fast and deterministic to test.
 */
struct FStructure
{
	/**
	 * Add a piece and return its handle. Rejects a negative or non-finite mass (returns
	 * INDEX_NONE, adds nothing); zero is allowed, a massless piece is meaningful.
	 */
	int32 AddPiece(double MassKg, bool bIsGrounded = false);

	/**
	 * Add a piece that knows where its weight acts, and return its handle. Makes every
	 * rejection the two-argument form makes. A non-finite centre is refused, not stored: a NaN
	 * there launders into a NaN lever arm once anything subtracts a joint centroid from it.
	 */
	int32 AddPiece(double MassKg, bool bIsGrounded, const FVector& CentreOfMassCm);

	/**
	 * Add a connection and return its handle, or INDEX_NONE if it is not a joint. Validation
	 * lives here because the structure owns the graph. Rejected: an out-of-range piece handle
	 * (INDEX_NONE included), a self-connection, a non-finite or non-positive interface area
	 * (fail closed — the load split divides by supporting area), and a normal describing no
	 * plane.
	 *
	 * A supplied rectangle is checked too, since a rectangle disagreeing with the area is a
	 * plausible number with the wrong lever arm. Rejected: a 4 x h_u x h_v that disagrees with
	 * the area, an extent on the normal's own axis (a box, not a face), a negative or
	 * non-finite half-extent or centre, and any rectangle on a non-axis-aligned normal. Zero
	 * extents are healthy — no bending capacity was measured, and the area alone answers a
	 * centred load.
	 *
	 * Validates, never normalises: an accepted rectangle is stored bit for bit, since quietly
	 * zeroing an unverifiable one would turn a refusal into a joint that reads as healthy.
	 */
	int32 AddConnection(const FConnection& Connection);

	/**
	 * Take a piece out of the structure, leaving its handle valid forever.
	 *
	 * @return true if a live piece was removed; false for a handle that names no piece
	 *         and for one that has already gone.
	 */
	bool RemovePiece(int32 PieceIndex);

	/** Whether this handle names a piece that has been removed. */
	bool IsPieceRemoved(int32 PieceIndex) const;

	/**
	 * The valid handle range, which never shrinks — not the number of live pieces. Callers
	 * iterate 0..NumPieces() and resolve each handle, so a live count would silently skip
	 * pieces. Use IsPieceRemoved per slot, NumLivePieces for a count.
	 */
	int32 NumPieces() const;

	/** How many pieces have not been removed. For counts, never for iteration. */
	int32 NumLivePieces() const;

	int32 NumConnections() const;

	/** How many times this structure has been solved. Test-only cost observable; no production code branches on it. */
	int32 NumSolves() const;

	/**
	 * Whether every live piece and joint has position data. Distinguishes "no eccentricity"
	 * from "no positions supplied" — both give a zero moment. Considers only live
	 * pieces/joints; true for an empty structure.
	 */
	bool HasCompleteGeometry() const;

	/** Out-of-range handles return a default-constructed placeholder. */
	const FStructurePiece& GetPiece(int32 PieceIndex) const;
	const FConnection& GetConnection(int32 ConnectionIndex) const;

	/**
	 * Mutable access to a built connection, for the mortar-sensitivity sweep that re-authors a
	 * joint's strength and re-solves. An out-of-range handle returns a throwaway placeholder,
	 * so a bad index writes nowhere observable.
	 */
	FConnection& GetConnectionMutable(int32 ConnectionIndex);

	/**
	 * Recompute what every connection carries and which pieces reach the ground. Given joints
	 * are skipped entirely. Non-destructive: no connection may give from solving, however
	 * overloaded.
	 */
	void SolveLoads();

	/**
	 * Solve, break every over-capacity joint, re-solve so their load moves onto the
	 * neighbours, and repeat until nothing more gives. Ordering within a pass is arbitrary;
	 * between passes it is the sequence a collapse is played back in. Terminates because
	 * joints never heal.
	 *
	 * A pass breaks on two grounds: the per-joint capacity sweep, and the equilibrium gate
	 * (BreakByEquilibrium, DESIGN.md §7 step 4), which asks the rigid-block LP whether the
	 * whole structure has any equilibrium with self-weight and on "no" severs the mechanism.
	 * Scoped by a block cap; see its own contract.
	 *
	 * Pass numbers are global, not per-call: a second cascade continues from the highest stamp
	 * written and rewrites nothing, so a shared number reads as "gave simultaneously".
	 *
	 * @return the number of passes this call broke at least one joint in; zero for a structure
	 *         that stands. Per-call, so it can be smaller than the stamps the call wrote.
	 */
	int32 SolveAndBreak();

	/**
	 * Injectable block cap scoping the equilibrium gate by structure size (PROMOTION_DESIGN.md
	 * §12 D6-c): authoritative when the block count is <= this cap, declining to the router
	 * above it. A test sets a low cap to drive the decline arm without a large fixture.
	 */
	void SetEquilibriumGateBlockCap(int32 MaxBlocks);

	/**
	 * Injectable region cap the cascade hands ProveRegionalCollapse in its above-cap decline
	 * arm (REGIONAL_PROVER_PLAN.md §4). Bounds |region ∪ grounded boundary| the flood may
	 * pose; a larger mechanism is an accepted miss (the router covers it, and the prover only
	 * moves toward collapse, so a miss never over-holds). A bare store the cascade seam reads.
	 */
	void SetRegionBlockCap(int32 MaxBlocks);

	/**
	 * Record what a piece is made of, so a cross-material joint can reach its two faces'
	 * materials via DestructionForce::EffectiveBondedStrength. A bare store. Out-of-range
	 * handles are ignored; a null pointer clears the assignment back to the "not supplied"
	 * default.
	 */
	void SetPieceMaterial(int32 PieceIndex, const DestructionProfiles::FMaterialProfile* Material);

	/**
	 * Flag this structure as genuinely 3D, so the oracle bridge poses the 3D LP
	 * (FOracleProblem::Dim = Dim3D) and stops refusing its out-of-plane (Y) joint normals. A
	 * stated flag rather than inferred from any Y-normal joint, so a 2D structure that
	 * accidentally grows one stays refused rather than silently promoted. False by default.
	 */
	void SetThreeDimensional(bool bIsThreeDimensional);

	/** Whether SetThreeDimensional flagged this structure 3D. False by default (2D). */
	bool IsThreeDimensional() const;

	/**
	 * The isolated test entry for the regional collapse prover (REGIONAL_PROVER_PLAN.md slice
	 * 1): region extraction, grounded-boundary pose, SolveRigidBlock, Falling-only stitch, end
	 * to end without touching the real SolveAndBreak cascade. Runs SolveLoads, then delegates
	 * to ProveRegionalCollapse; see it for the flood and stitch.
	 *
	 * With RegionBlockCap >= the block count the region floods the whole structure, so the
	 * grounded boundary is the earth alone and the released set equals the whole-structure
	 * ground-only LP's moving set.
	 *
	 * @param Seed           The pieces the region grows from (the disturbance neighbourhood).
	 * @param RegionBlockCap The largest region the flood may reach (the grounded boundary rings it).
	 * @return the number of pieces the prover released (marked Falling) this call.
	 */
	int32 SolveAndBreak_WithRegionalProver(const TArray<int32>& Seed, int32 RegionBlockCap);

	/**
	 * Instrumentation: blocks the last regional prove posed, i.e. |region ∪ grounded boundary|,
	 * which comes in at or under RegionBlockCap. Stamped only when a prove posed a problem, so
	 * a later refused pose leaves the previous value. INDEX_NONE until the first prove.
	 */
	int32 GetLastRegionalProblemBlockCount() const;

	/**
	 * Instrumentation for the last equilibrium-gate pose's physics: 2 for 2D (X-Z), 3 for 3D,
	 * INDEX_NONE until a pose is built. BuildRigidBlockProblem poses the cheapest sound
	 * problem — 2D when every posed joint has an in-plane normal at one common Y, 3D otherwise
	 * (the two carry different Coulomb cones, DESIGN §8). Stamped on pose, not call.
	 *
	 * An int, not RigidBlockOracle::EOracleDim: Structure.h forward-declares the oracle structs
	 * to avoid its header, and an enum cannot be forward-declared without fixing its type here.
	 */
	int32 GetLastEquilibriumProblemDim() const;

	/**
	 * What one solve cost, phase by phase — observability only. Wall-clock ms from
	 * FPlatformTime, meant to be read, never asserted against a threshold.
	 */
	struct FSolveLoadsProfile
	{
		/** Building the per-piece joint lists and the two-tier support lists (step one). */
		double SupportListsMs = 0.0;

		/** ReseatSpannedGroups — runs that span a hole are re-seated on their ends. */
		double ReseatMs = 0.0;

		/** The reachability / load-split fixpoint (steps two to five), all its iterations. */
		double FixpointMs = 0.0;

		/** How many times the fixpoint ran a complete solve from scratch before nothing changed. */
		int32 FixpointIterations = 0;

		/**
		 * What each iteration found, one entry each: pieces reached from ground, overturned off
		 * their supports, stranded in a load cycle, and refused-arch members released. Every
		 * iteration but the last changes one of the last three; the last holds the answer.
		 */
		TArray<int32> SupportedPerIteration;
		TArray<int32> OverturnedPerIteration;
		TArray<int32> StrandedPerIteration;
		TArray<int32> ReleasedPerIteration;

		/** ApplyArchingThrust (step six). */
		double ArchingMs = 0.0;

		double TotalMs = 0.0;
	};

	/**
	 * What one prover pose cost — one LP the regional prover posed while growing its region.
	 * A pass poses several; the FBreakPassReport aggregates are these reduced over the pass,
	 * the decomposition that says whether a slow pass is one big pose or several. Test-only
	 * observability; no production code may branch on it.
	 */
	struct FProverPoseReport
	{
		/** |region ∪ grounded boundary| posed for this pose (both interior R and the grounded ring B). */
		int32 Blocks = INDEX_NONE;

		/** Result.SimplexIterations for this pose. */
		int32 LpPivots = 0;

		/** Wall-clock ms of this pose's SolveRigidBlock. */
		double LpMs = 0.0;

		/** Whether this pose was a certified fall. */
		bool bFell = false;
	};

	/**
	 * What one pass of the break cascade did and cost. A pass is one SolveLoads, then the
	 * equilibrium gate (which above the block cap declines to the router), then — only when it
	 * declined — the per-joint capacity sweep and the regional collapse prover.
	 */
	struct FBreakPassReport
	{
		/** The global stamp this pass wrote on every joint it severed. */
		int32 Pass = 0;

		FSolveLoadsProfile Solve;

		double GateMs = 0.0;

		/** EEquilibriumGateDisposition as an integer: 0 declined to the router, otherwise it answered. */
		int32 GateDisposition = 0;

		/** Joints the gate severed when it answered below the cap — the LP mechanism's opened joints. */
		int32 JointsSeveredByGate = 0;

		double CapacitySweepMs = 0.0;

		/** Joints the capacity sweep severed this pass — over their own weakest-link capacity. */
		int32 JointsGivenToSweep = 0;

		double RegionalProverMs = 0.0;

		/** How many LPs the prover posed while growing its region, and their pivots and time in total. */
		int32 RegionalPoses = 0;
		int32 RegionalLpPivots = 0;
		double RegionalLpMs = 0.0;

		/** |region ∪ grounded boundary| of the prover's final pose this pass, or INDEX_NONE. */
		int32 RegionalLastBlocks = INDEX_NONE;

		/** Whether the prover's final pose was a certified fall. */
		bool bRegionalFell = false;

		/** One entry per prover pose this pass, in pose order — a faithful decomposition of the RegionalPoses/RegionalLpPivots/RegionalLpMs aggregates above. */
		TArray<FProverPoseReport> RegionalPoseBreakdown;

		/** Joints the prover severed and pieces it marked Falling this pass. */
		int32 JointsSeveredByProver = 0;
		int32 PiecesFelledByProver = 0;

		/** The structure after this pass: live pieces, intact joints, pieces not held up. */
		int32 LivePieces = 0;
		int32 IntactJointsAfter = 0;
		int32 NotHeldAfter = 0;

		double PassMs = 0.0;
	};

	/**
	 * The break-decision report: what the last SolveAndBreak did, pass by pass, and its cost.
	 * Observability, not authority; no production code branches on it. The terminal pass (the
	 * one that broke nothing) is included, so a structure that stands reports one pass, zero
	 * breaks.
	 */
	struct FSolveAndBreakReport
	{
		double TotalMs = 0.0;

		/** The value SolveAndBreak returned: passes that broke at least one joint. */
		int32 BreakingPasses = 0;

		int32 LivePiecesBefore = 0;
		int32 IntactJointsBefore = 0;
		int32 IntactJointsAfter = 0;

		/** Every pass run, breaking or terminal, in order. */
		TArray<FBreakPassReport> Passes;
	};

	/** The report of the last SolveAndBreak, or a default one if none has run. */
	const FSolveAndBreakReport& GetLastSolveAndBreakReport() const;

	/** The phase profile of the last SolveLoads, however it was called. */
	const FSolveLoadsProfile& GetLastSolveLoadsProfile() const;

	/**
	 * Which breaking pass gave this joint, counted from 1, or INDEX_NONE if none did (including
	 * an out-of-range handle). INDEX_NONE is not "intact": it means "did not fail under load in
	 * a pass", also true of a joint that went because its piece was removed. Whether a joint is
	 * still in the structure is HasGiven's question. Together they encode:
	 *
	 *     intact              HasGiven false, INDEX_NONE
	 *     went with a piece   HasGiven true,  INDEX_NONE
	 *     broke in pass N     HasGiven true,  N >= 1
	 */
	int32 GetBreakPass(int32 ConnectionIndex) const;

	/**
	 * Which authority severed a joint, for observability. The codes:
	 *
	 *     1  the below-cap equilibrium gate (BreakByEquilibrium)
	 *     2  the per-joint capacity sweep (BreakByCapacitySweep)
	 *     3  the regional collapse prover (ProveRegionalCollapse)
	 *
	 * INDEX_NONE means none did: intact, or it went with a removed piece (RemovePiece severs
	 * without stamping). Read-only companion to GetBreakPass; no production code branches on it.
	 */
	int32 GetBreakAuthority(int32 ConnectionIndex) const;

	/**
	 * What this connection is to this piece: the solver's own two-tier decision, exposed. The
	 * same decision SolveLoads routes by, not a re-derived copy that could disagree with the
	 * routing it explains. Pure geometry, so it answers before any solve and keeps answering
	 * for a given joint. None for a handle naming no connection, a piece not on it, or a normal
	 * that will not normalise.
	 */
	EJointRole GetJointRole(int32 ConnectionIndex, int32 PieceIndex) const;

	/**
	 * The force this connection carries, in Unreal force units, after SolveLoads. A vertical
	 * vector of the accumulated magnitude; FConnection::ApplyForce resolves it against the
	 * joint's normal, so the same vector is compression on a bed joint and shear on a head one.
	 *
	 * Except at an arch springing: a spanned opening pushes sideways too, equally and oppositely
	 * at its two ends, so the force is (H, 0, -V). A horizontal component on a bed joint is
	 * shear by ClassifyForce's definition, so the arch needs no new axis or strength
	 * (ARCHING_DESIGN.md).
	 *
	 * Its sign depends on which end is held up: the force belongs to PieceB (ConnectionLoad.h),
	 * so storing it against the wrong end turns compression into tension. Zero for an
	 * out-of-range handle and for a connection with no path to ground.
	 */
	FVector GetConnectionForce(int32 ConnectionIndex) const;

	/**
	 * The bending moment this connection carries about its own centroid, uu.cm, after
	 * SolveLoads. One of the three inputs to GetConnectionUtilisation's contract:
	 *
	 *     GetConnectionUtilisation(I)
	 *         == GetConnection(I).UtilisationUnder(
	 *                GetConnectionForce(I), GetConnectionMoment(I),
	 *                GetConnectionCompositeDepthCm(I))
	 *
	 * That parameter is defaulted, so an assertion written without this accessor silently
	 * supplies zero. Separate from the force, never encoded in it.
	 *
	 * Zero is "no eccentricity", not a tolerance: a centred load, an unplaced piece, and an
	 * unmeasured rectangle all produce it exactly. HasCompleteGeometry tells those apart; this
	 * does not. Zero for an out-of-range handle and before any solve, same scope as
	 * GetConnectionForce.
	 */
	FVector GetConnectionMoment(int32 ConnectionIndex) const;

	/**
	 * How deep the bonded masonry standing over this connection is, cm, after SolveLoads. The
	 * third input to GetConnectionUtilisation (see the identity on GetConnectionMoment); the
	 * parameter is defaulted, so an assertion without this accessor silently supplies zero.
	 *
	 * A length, not a section: the solver knows how much wall stands on the plane, not which
	 * in-plane extent it pairs with. Non-zero only where a joint is bent — a bed joint beneath
	 * a placed piece carrying its whole load and a moment; elsewhere zero, and the joint reads
	 * its bare bed patch.
	 *
	 * Zero is "no masonry measured", fail-closed: the relief is withheld and the joint reads
	 * more heavily loaded, safe for a quantity whose job is to make a joint read less. Zero for
	 * an out-of-range handle and before any solve.
	 */
	double GetConnectionCompositeDepthCm(int32 ConnectionIndex) const;

	/**
	 * How close this connection is to failing under the last solve's load.
	 * FConnection::UtilisationUnder applied to GetConnectionForce, so nothing re-derives the
	 * break decision. Zero for a given joint (HasGiven is the authoritative state, not a low
	 * ratio). Fails closed for a handle naming no joint, returning TNumericLimits<double>::Max()
	 * rather than zero, which would read as "unloaded and healthy". Zero before any solve.
	 */
	double GetConnectionUtilisation(int32 ConnectionIndex) const;

	/**
	 * The joint's effective strength — its bare connection paired with its two faces' materials
	 * by the weakest-link rule (SHED_PATH.md Phase B/B3). The single pairing point both strength
	 * paths consult (the router via GetConnectionUtilisation, the oracle bridge for each LP
	 * strength row), so they cannot disagree about a cross-material joint.
	 *
	 * Gated on both faces naming a material: only then is DestructionForce::EffectiveBondedStrength
	 * consulted; otherwise the bare connection strength is returned unchanged. Computed on demand;
	 * no cache. Bare strength for an out-of-range handle, whose placeholder names no material.
	 */
	FConnectionStrength EffectiveJointStrength(int32 ConnectionIndex) const;

	/**
	 * One connection's strain readout from the cached min-violation LP (PROMOTION_DESIGN.md
	 * §3.5). Below the block cap a settled structure solves the goal-programming LP once and
	 * caches its per-joint result, keyed back through the bridge's ConnectionOfJoint provenance,
	 * so the overlay shows the LP's force distribution rather than the router's estimate.
	 *
	 * NormalUu is N = n1 + n2 (compression positive); MomentUuCm is M about the joint centre;
	 * ViolationUu is the non-negative strength-row slack (0 within capacity); Utilisation maps
	 * that onto the same 0 -> 1 -> >1 scale as GetConnectionUtilisation.
	 *
	 * bPresent is false until a below-cap settle cached the readout; above the cap the gate
	 * declines and the overlay falls back to GetConnectionUtilisation. Absent for an
	 * out-of-range handle and before any solve.
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
	 * Observability only — how many min-violation readout LPs the last SolveAndBreak ran. Reset
	 * at the top of SolveAndBreak and bumped once per call, so it counts across the cascade, not
	 * one pass. Test scaffolding for "compute the readout once per settle"; drives no behaviour.
	 */
	int32 GetMinViolationReadoutSolveCount() const;

	/**
	 * Whether this piece reaches a grounded piece through supports, after SolveLoads. Grounded
	 * pieces are supported by definition. Through supports, not connections, and a path the
	 * solver could route: pieces in a cycle of the support relation cannot be put in an
	 * accumulation order, so they are reported unsupported (not a claim the cycle was solved).
	 * Only the pieces in the knot; a piece beneath one keeps its support (DESIGN.md §3).
	 *
	 * False for an out-of-range handle. This is the last solve's output, which only SolveLoads
	 * writes, so removal does not rewrite it:
	 *
	 *     never solved      false, for every handle — there is no answer yet
	 *     removed           the last solve's answer, unchanged, until the next solve
	 *     after that solve  false, and a removed grounded piece is no longer earth
	 *
	 * The middle row is deliberately stale, not wrong: clearing one entry on removal leaves a
	 * half-stale array, worse than a uniformly stale one with a documented scope. Re-solve
	 * before reading neighbours to decide what to release. Structure.RemovedPieceSupportNeedsASolve
	 * pins all three rows.
	 */
	bool IsPieceSupported(int32 PieceIndex) const;

	/**
	 * Why this piece is or is not held up, after SolveLoads. Refines IsPieceSupported, and the
	 * two must never disagree:
	 *
	 *     IsPieceSupported(H)  ==  (GetPieceSupport(H) is Grounded or Supported)
	 *
	 * for every handle at every moment. Stranded is only for pieces in the knot; a piece
	 * resting on one is Falling (it lost its support — ordinary physics). Same scope as
	 * IsPieceSupported. No "removed" enumerator: IsPieceRemoved answers that, and a Removed
	 * value would contradict a stale Supported in the RemovedPieceSupportNeedsASolve window.
	 * Falling for an out-of-range handle.
	 */
	EPieceSupport GetPieceSupport(int32 PieceIndex) const;

	/**
	 * Whether the last solve actually computed a support state for this handle — the one thing
	 * GetPieceSupport cannot say, since Falling means both "nothing holds this up" and "not yet
	 * asked". PieceSupported is sized by SolveLoads, so its extent is the handles the last solve
	 * answered for. False before any solve, for a handle added since, and for one naming no
	 * piece; true for a removed piece inside that extent.
	 *
	 * Needed by anything that turns "not held up" into an irreversible action, since the
	 * polarity inverts there: as a command, default Falling says "release the whole structure,
	 * foundation included" (fail-open). A readout may take GetPieceSupport at face value; a
	 * command must ask this first.
	 */
	bool HasSupportAnswer(int32 PieceIndex) const;

private:
	/**
	 * One spanned opening, as the group pass leaves it: the seated pieces at its two ends. An
	 * arch is a fact about a run of pieces, not any one, and H is one number for the whole
	 * opening applied equally and oppositely at the two abutments — so the pass needs both ends
	 * at once. ARCHING_DESIGN trap 2: applying +H at one springing and forgetting the other
	 * gives a net horizontal force from nowhere while every joint reads plausibly.
	 *
	 * The two ends are an order, not a world direction: which is 0 falls out of the group walk,
	 * and nothing may depend on it.
	 */
	struct FSpannedArch
	{
		/**
		 * The seated pieces the group pushes against, split by which end they stand at. Both
		 * non-empty: a group with nothing on one side is a cantilever, not a span.
		 */
		TArray<int32> Abutments[2];

		/**
		 * Unit direction toward end 0, the exact negative of the push on end 1. One vector, not
		 * two, so +H*D and -H*D cancel bit for bit and the thrusts sum to exactly zero.
		 */
		FVector TowardEndZero = FVector::ZeroVector;

		/**
		 * L: how far apart the opening's two ends stand, cm. Measured between the abutments' own
		 * centres (one mean per end), which is the clear opening for a running-bond wall: each
		 * springing keeps half a cell of bearing, and the two half cells make up the one removed.
		 * Seat centroid to seat centroid reads 5% high on a ten-cell hole. Recorded here because
		 * the thrust pass has the abutments but not the group, and the span is a fact about the
		 * opening.
		 */
		double SpanCm = 0.0;
	};

	/**
	 * Re-seat the pieces a hole left with no seat at all onto the group they belong to — the
	 * group rule, the one place the solver reads geometry to decide a route. A hole wider than
	 * one brick leaves the middle bricks with no seat; under the two-tier rule they fall back to
	 * their head joints, become each other's support, and the solver strands them, so a wall
	 * that should span loses its middle. Instead, contiguous unseated pieces form a group through
	 * their intact head joints and each member re-seats toward its nearer abutment, acyclic since
	 * every edge runs from a longer path to a shorter one.
	 *
	 * GetJointRole still never reads geometry (one joint's tier stays a fact about one normal);
	 * this group rule sits above the tier. Gated on HasCompleteGeometry() and a no-op without it,
	 * so a geometry-free structure routes exactly as before.
	 *
	 * @param PieceJoints          Every joint touching each piece, in ascending index order.
	 * @param PieceHasNoSeat       Which live, ungrounded pieces have no intact bed joint beneath.
	 * @param SupportConnections   What holds each piece up. Rewritten in place for the members of
	 *                             an abutted group, left as found elsewhere.
	 * @param PieceReseatedOnAnArch Set for each piece this rewrote, false elsewhere.
	 * @param PieceInRefusedArchGroup Set for each member of a group that formed but whose opposition
	 *                             gate refused it (a one-sided cantilever). These were not re-seated;
	 *                             marking them lets SolveLoads exclude them from the reachability walk,
	 *                             so a refused cantilever reads Falling rather than false-knot Stranded.
	 *                             A geometry-free propping pair forms no group and keeps its real strand.
	 * @param Arches               One entry per group that spans, naming the seated pieces at its two
	 *                             ends. Emptied first, so no-geometry and no-hole both return none.
	 */
	void ReseatSpannedGroups(
		const TArray<TArray<int32>>& PieceJoints,
		const TArray<bool>& PieceHasNoSeat,
		TArray<TArray<int32>>& SupportConnections,
		TArray<bool>& PieceReseatedOnAnArch,
		TArray<bool>& PieceInRefusedArchGroup,
		TArray<FSpannedArch>& Arches) const;

	/**
	 * Push each spanned opening's two abutments apart by the thrust the arch develops.
	 * ARCHING_DESIGN.md:
	 *
	 *     d_e = min( cover above the span , 0.866 * L )      arching depth
	 *     r   = d_e / 3                                      thrust line rise, kern-limited
	 *     W   = the load the solver already accumulated      NOT a triangle
	 *     H   = W * L / (8r)      V = W / 2                  per abutment
	 *
	 * What resists it is already in the model: the thrust arrives horizontally at a bed joint,
	 * ClassifyForce calls that shear, and shear capacity is already `c + mu*sigma_n` — no new
	 * axis, strength or profile data, so dry stone's zero cohesion refuses to arch at any span.
	 *
	 * Run after accumulation, never inside it: W is already routed to the seats, so the shear
	 * component changes no split, support list or moment — every vertical answer is bit-identical.
	 * The cover usually decides: `H` grows as `1/cover`, which is why a wide opening near the top
	 * of a wall cannot arch. One cover per arch, the thinnest end — see MasonryDepthAboveCm.
	 *
	 * @param PieceJoints Every joint touching each piece, in ascending index order.
	 * @param Arches      What ReseatSpannedGroups found. Empty is the ordinary case, costs nothing.
	 */
	void ApplyArchingThrust(
		const TArray<TArray<int32>>& PieceJoints,
		const TArray<FSpannedArch>& Arches);

	/**
	 * The live piece resting on this one through an intact bed joint, or INDEX_NONE. One step of
	 * the upward walk, shared so MasonryDepthAboveCm and SolveLoads's composite gate cannot
	 * diverge. A given joint holds nothing up and a removed piece stands nowhere; both are
	 * excluded, which is what shortens the depth as a wall comes apart.
	 */
	int32 PieceRestingOn(int32 Piece, const TArray<TArray<int32>>& PieceJoints) const;

	/**
	 * How much bonded masonry stands over one bed joint, cm, by a bounded upward walk. One
	 * measurement, two callers: ApplyArchingThrust wants the cover over a springing (d_e =
	 * min(cover, 0.866*L)), SolveLoads the composite depth over a seat (a stack resists its
	 * overturning moment as a deep beam, t*D^2/6). Sharing the walk stops the two diverging.
	 *
	 * Counted in courses, the joint's own course first, so the shallowest answer is one course
	 * pitch, never zero. A walk over bed joints, never a spatial query, so it needs no
	 * broadphase; a given joint conducts nothing and stops the walk, bounding the depth to
	 * bonded masonry. Bounded twice: EnoughDepthCm (past which the caller's answer cannot
	 * change), and the piece count (defence against a graph claiming A above B and B above A).
	 *
	 * A chain, not a traversal: running bond puts two pieces above each brick and this follows
	 * the first by ascending joint index; a stepped or gabled wall's depth would depend on which
	 * column the walk took, untested. Zero where there is nothing to measure; both callers guard
	 * on that.
	 *
	 * @param Piece         The piece standing on the joint.
	 * @param SeatJointIndex Its bed joint, the plane the masonry stands on.
	 * @param PieceJoints   Every joint touching each piece, in ascending index order.
	 * @param EnoughDepthCm The depth past which more masonry cannot change the caller's answer.
	 */
	double MasonryDepthAboveCm(
		int32 Piece,
		int32 SeatJointIndex,
		const TArray<TArray<int32>>& PieceJoints,
		double EnoughDepthCm) const;

	/**
	 * How deep the corbelling body standing on one bed joint is, cm — the floor under the
	 * composite depth, the part no lever arm may trim. The corbelling courses generate the
	 * moment, so they cannot be refused the section they generate it with. Masonry above the cut
	 * is bounded differently, and only it is:
	 *
	 *     D  =  min( masonry above , max( THIS , lambda*|M|/|F| ) )
	 *
	 * The floor never credits a course above the cut, because the floor is the cut: the walk
	 * stops at the first non-corbelling course. Corbelling is "seated on exactly one course", a
	 * graph fact: a bonded brick straddles two below, a stepped-out one hangs off one.
	 *
	 * ⚠ True of every piece in a stack-bond column or one-brick-wide wall, where the walk runs
	 * to the top and the floor becomes the whole wall — the defect lambda*e was added to remove.
	 * The outboard-of-seat direction test that would refuse it is deliberately not built: no
	 * fixture can tell the two apart, since a stack-bond wall has zero eccentricity so no joint
	 * carries a moment and this is never reached. See CURRENT_STATE.md.
	 *
	 * The walked piece is chosen by the corbelling predicate, not PieceRestingOn: that chain
	 * takes the first bed joint above by index, which on a filled corbel is the 2 cm inboard lap
	 * rather than the 18 cm seat under the front. So this walks the corbelling chain: the first
	 * piece resting here that is itself corbelling.
	 *
	 * @param Piece          The piece standing on the joint — always the body's first course.
	 * @param SeatJointIndex Its bed joint, the plane the body stands on.
	 * @param PieceJoints    Every joint touching each piece, in ascending index order.
	 */
	double CorbellingBodyDepthCm(
		int32 Piece,
		int32 SeatJointIndex,
		const TArray<TArray<int32>>& PieceJoints) const;

	/**
	 * Is there something on the overhanging side for this piece to arch against? The fourth gate
	 * of the arching rule, deciding whether a piece that lost a seat bridges the hole or
	 * cantilevers. It wants an intact head joint on the eccentric side (where the centre of mass
	 * sits, from the seat's centroid) to a neighbour that reaches ground and is not resting on
	 * this piece. FConnection::ArchingMomentScale owns the part one joint can answer alone.
	 *
	 * Four things matter. The direction: both eccentric fixtures have their supported neighbour
	 * on the seated side, so an undirected test halves every overhang. The neighbour must not
	 * hang from us, or two bricks propping each other over air read as an arch; the cheap
	 * one-step test is exact because a longer loop is what LoadReturnsToPiece strands. A spanned
	 * group supersedes that: a re-seated neighbour carries the PieceReseatedOnAnArch mark, so the
	 * far abutment gives the thrust somewhere to go. And an abutment the seat cannot push against
	 * is none: supplying the deleted couple `(1-k)*|M|` needs the seat to carry `dM/z` against
	 * `c + mu*sigma_n`, the same Mohr-Coulomb envelope; where it cannot, the relief is withheld
	 * (DESIGN.md §7 gap 4, so a mortared springing affords it while dry stone cannot).
	 *
	 * A spanned group is checked by being pushed instead (ApplyArchingThrust), so this gate
	 * leaves it alone — judging the same thrust twice by two rules.
	 *
	 * @param PieceIndex            The loaded piece, which must be placed.
	 * @param BedJoint              Its one seat, which must know its rectangle: kern, centroid and
	 *                              the plane the eccentricity is measured in all come off it.
	 * @param SeatForceUu           What that seat carries, oriented as stored: the squeeze the
	 *                              sliding capacity is bought with.
	 * @param DeletedCoupleUuCm     The couple the cap would delete, `(1-k)*|M|`, a magnitude.
	 * @param PieceJoints           Every joint touching each piece, in ascending index order.
	 * @param SupportConnections    What holds each piece up, before the reaching-ground filter.
	 * @param PieceReseatedOnAnArch Which pieces ReseatSpannedGroups re-seated (lean on an abutment
	 *                              only because a further one carries).
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
	 * Whether a piece on two or more compression-only supports overturns because its centre of
	 * mass projects outside the region they can push up through — the router's answer to global
	 * overturning, which no per-joint number expresses (each seat reads a comfortable split while
	 * the body has no admissible equilibrium).
	 *
	 * The tension clause is asked first and is data: any support whose strength carries tension
	 * holds the lifting side down, so the body has equilibrium however far the centre of mass
	 * reaches — read off the same FConnectionStrength the break sweep reads, so a new
	 * tension-capable connection type changes this with no code. That stands the porch overhang
	 * on its Screw-tied cleat. The region is the convex hull of the contact rectangles, tested by
	 * a conservative axis-aligned bound.
	 *
	 * @param PieceIndex The loaded piece; its centre of mass projects onto the bed plane.
	 * @param LoadPath   Its supports that reach ground — their contact rectangles and tension
	 *                   capability decide the answer.
	 * @return true only when the piece is on compression-only supports and its centre of mass is
	 *         certainly outside their union; false otherwise, including on missing or non-finite
	 *         geometry (fail closed: never spuriously overturn).
	 */
	bool PieceOverturnsOffItsSupports(int32 PieceIndex, const TArray<int32>& LoadPath) const;

	/**
	 * How the equilibrium gate disposed of a pass, so SolveAndBreak knows whether the LP
	 * answered authoritatively (skip the capacity sweep) or declined (the router sweep is the
	 * sole authority). Below the block cap the LP is the only break authority
	 * (PROMOTION_DESIGN.md §3.7); the strength sweep is demoted to an estimator that latches
	 * nothing. DeclinedToRouter is the zero enumerator, so a fail-closed value routes to the
	 * capacity sweep.
	 */
	enum class EEquilibriumGateDisposition : uint8
	{
		/** Over cap / no geometry / bridge refused / LP unanswerable: the capacity sweep decides. */
		DeclinedToRouter = 0,

		/** The LP answered and severed nothing this pass (it stood, or the mechanism's joints were already gone). */
		AuthoritativeNoBreak,

		/** The LP answered Falls and severed the mechanism's opening/sliding joints this pass. */
		AuthoritativeBroke,
	};

	/**
	 * The equilibrium gate (DESIGN.md §7 step 4, PROMOTION_DESIGN.md §6 Slice 3). Below the
	 * block cap it is the sole break authority: it asks the rigid-block LP whether the whole
	 * structure has any force system in equilibrium with self-weight, and on "no" extracts the
	 * phase-1 dual (the Farkas certificate = collapse mechanism) and severs the joints that
	 * mechanism opens or slides. Called from SolveAndBreak and nothing else.
	 *
	 * What no joint check catches: ComputeUtilisation reports a confident number for a joint on
	 * which no equilibrium exists (DESIGN.md §5.7) — a body on two bearings both to one side of
	 * its centroid reads a comfortable utilisation while its resultant has left the bearing. The
	 * LP reasons about the whole force system, so it also decides support, standing a structure
	 * the router only strands for want of a routing rule.
	 *
	 * The pose is feasibility at lambda = 1: the same Stands/Falls boolean as lambda* but
	 * cheaper. Scoped by EquilibriumGateBlockCap: above it the gate declines to the capacity
	 * sweep. A no-op without complete geometry and on any LP refusal (fail closed). On Stands or
	 * Falls it rewrites the support arrays so GetPieceSupport is LP-authoritative below the cap;
	 * on Falls it severs the mechanism's joints (via ConnectionOfJoint), stamped with this pass.
	 *
	 * @param Pass The cascade pass any break is stamped with.
	 * @return How the gate disposed of this pass (see EEquilibriumGateDisposition).
	 */
	EEquilibriumGateDisposition BreakByEquilibrium(int32 Pass);

	/**
	 * The regional collapse prover (REGIONAL_PROVER_PLAN.md §§1-4), shared by the isolated test
	 * entry and the real cascade. It floods a region from Seed by joint-hops up to RegionBlockCap,
	 * pins the one-hop frontier ring grounded, poses R ∪ boundary at feasibility through
	 * RigidBlockOracle::BuildRegionalProblem, and on a certified Falls marks the moved interior
	 * pieces Falling (never Supported — toward collapse only) and severs the mechanism's joints,
	 * stamping each with BreakPass.
	 *
	 * It does not solve loads: the caller has settled the graph, so re-solving would wipe the
	 * support state the stitch overrides. A bridge refusal or non-Falls outcome releases nothing.
	 *
	 * @param Seed           The pieces the region grows from (the disturbance neighbourhood).
	 * @param RegionBlockCap The largest region the flood may reach (the grounded boundary rings it).
	 * @param BreakPass      The pass any severed joint is stamped with (default 1, so the isolated
	 *                       slice-1/2/3 tests are byte-for-byte).
	 * @return the number of pieces the prover moved and marked Falling this call.
	 */
	int32 ProveRegionalCollapse(const TArray<int32>& Seed, int32 RegionBlockCap, int32 BreakPass = 1);

	/**
	 * Rewrite the per-piece support arrays from the LP verdict, so GetPieceSupport is
	 * LP-authoritative below the cap (PROMOTION_DESIGN.md §3b). Each included piece reads
	 * Supported/Grounded if the LP carries it, Falling if the mechanism moves it, overriding the
	 * router's Stranded. Called only from BreakByEquilibrium on an answered solve.
	 */
	void ApplyLimitAnalysisSupport(
		const RigidBlockOracle::FOracleProblem& Problem,
		const RigidBlockOracle::FOracleResult& Result);

	/**
	 * Solve and cache the min-violation strain readout, keyed back through ConnectionOfJoint
	 * (PROMOTION_DESIGN.md §3.5). Called only from BreakByEquilibrium on an answered below-cap
	 * solve, after the break authority: it poses a separate FOracleProblem with
	 * bMinViolationReadout, so it changes no break decision, support flag or force — purely
	 * additive.
	 */
	void CacheMinViolationReadout(const RigidBlockOracle::FOracleProblem& Problem);

	/**
	 * The per-joint capacity sweep — the router's break authority, stamping every over-capacity
	 * joint with this pass. The sole authority above the block cap and on any LP refusal; below
	 * the cap the gate answers and this does not run. Returns whether any joint gave. Called
	 * only from SolveAndBreak.
	 */
	bool BreakByCapacitySweep(int32 Pass);

	/**
	 * The equilibrium gate's block cap — the fail-closed boundary keeping synchronous LP
	 * authority off the flagship scenarios (D6-c). At or below it the gate is authoritative;
	 * above it it declines to the router. Set so the medium wall-catalogue fixtures (125-174
	 * blocks) are in scope while the 375-block flagship and giant corbels stay above. No
	 * production structure sits in that range today (the default wall is ~1200 blocks), so the
	 * cost is test-time for now — CURRENT_STATE carries the open latency item.
	 */
	int32 EquilibriumGateBlockCap = 200;

	/*
	 * The regional prover's block cap — the largest |region ∪ grounded boundary| the cascade's
	 * above-cap decline arm lets ProveRegionalCollapse pose (REGIONAL_PROVER_PLAN.md §4).
	 *
	 * Modest, not the gate's 200: the prover poses a region-cap-sized LP every above-cap pass,
	 * so the cap is the per-pass cost — 200 blocks per pass makes a flagship 3D collapse take
	 * tens of minutes. Over-holds are local, so a small region catches them cheaply and the
	 * grounded boundary keeps every verdict sound. 200 stays reachable through SetRegionBlockCap
	 * for tests.
	 *
	 * Named distinctly from the RegionBlockCap parameter so the -Werror shadow (C4458) never
	 * fires: the test entry drives the parameter, the cascade reads this member.
	 */
	int32 RegionalProverBlockCap = 200;

	/*
	 * Backs GetLastRegionalProblemBlockCount — blocks the last regional prove posed, i.e.
	 * |region ∪ grounded boundary|. Stamped when the pose is built; INDEX_NONE until then.
	 */
	int32 LastRegionalProblemBlockCount = INDEX_NONE;

	/*
	 * Backs GetLastEquilibriumProblemDim — 2 or 3 for the last equilibrium-gate pose's
	 * dimension, INDEX_NONE before any. Stamped in BreakByEquilibrium from Problem.Dim.
	 */
	int32 LastEquilibriumProblemDim = INDEX_NONE;

	/** See GetLastSolveAndBreakReport / GetLastSolveLoadsProfile: observability, never authority. */
	FSolveAndBreakReport LastSolveAndBreakReport;
	FSolveLoadsProfile LastSolveLoadsProfile;

	/**
	 * What the last ProveRegionalCollapse did, for the pass report: poses, pivots, LP time, whether
	 * its final pose fell, and how many joints it severed. Written by ProveRegionalCollapse only.
	 */
	int32 LastProverPoses = 0;
	int32 LastProverLpPivots = 0;
	double LastProverLpMs = 0.0;
	bool bLastProverFell = false;
	int32 LastProverJointsSevered = 0;

	/*
	 * Per-pose decomposition of the aggregates above — reset per ProveRegionalCollapse call, one
	 * entry per pose, so Num() equals LastProverPoses and the sums match. Surfaced as
	 * FBreakPassReport::RegionalPoseBreakdown; observability only.
	 */
	TArray<FProverPoseReport> LastProverPoseBreakdown;

	/*
	 * Whether SetThreeDimensional flagged this structure 3D (THREED_DESIGN.md E3). False by
	 * default, so every existing structure is 2D. The bridge reads it: true routes to the Dim3D
	 * pose and lifts the Y-normal refusal. See SetThreeDimensional.
	 */
	bool bThreeDimensional = false;

	TArray<FStructurePiece> Pieces;
	TArray<FConnection> Connections;

	/**
	 * Solver output, parallel to the arrays above and rebuilt by every solve. Kept beside the
	 * pieces, not inside them, so neither a piece nor a joint carries a cached answer that could
	 * be read before a solve produced it.
	 */

	/** Whether each piece reaches the earth through supports. */
	TArray<bool> PieceSupported;

	/**
	 * Whether each piece is in an unroutable knot — not merely resting on one. Computed by the
	 * SolveLoads fixpoint and kept so GetPieceSupport can say why a piece is not held up; both
	 * accessors read it and recompute nothing. Never both stranded and supported.
	 */
	TArray<bool> PieceStranded;

	/** What each connection carries, in Unreal force units. */
	TArray<FVector> ConnectionForces;

	/**
	 * The bending moment each connection carries about its own centroid, uu.cm. Beside
	 * ConnectionForces and rebuilt every solve, not folded into it: encoding it in the force
	 * vector would make GetConnectionForce report a number that no longer describes the load.
	 * Non-zero only where the statics is determinate — a placed piece whose load reaches ground
	 * through exactly one joint. See SolveLoads.
	 */
	TArray<FVector> ConnectionMoments;

	/**
	 * How deep the bonded masonry standing over each connection is, cm. The third parallel
	 * array, rebuilt every solve like the other two: a depth that survived a re-solve would
	 * describe a wall since cut. Beside the joints, not on them, so it cannot be read out of
	 * GetConnection before a solve produced it. Non-zero only where ConnectionMoments is, and on
	 * bed joints alone. See GetConnectionCompositeDepthCm.
	 */
	TArray<double> ConnectionCompositeDepthCm;

	/**
	 * Which breaking pass gave each connection, counted from 1, or INDEX_NONE. Grown by
	 * AddConnection, never cleared: joints never heal, so a stamp once written is history a
	 * re-run must not rewrite. The only record of the order a collapse happened in (phase 5
	 * plays it back); FConnection's latch says only whether a joint gave, never when.
	 */
	TArray<int32> ConnectionBreakPass;

	/**
	 * Which authority gave each connection, parallel to ConnectionBreakPass and grown beside it
	 * so the two stay index-aligned. Written once at the sever site by whichever authority ruled;
	 * INDEX_NONE until then. Pure observability — GetBreakAuthority reads it, the solve does not.
	 */
	TArray<int32> ConnectionBreakAuthority;

	/**
	 * The cached min-violation strain readout, one entry per connection, read by
	 * GetConnectionReadout (PROMOTION_DESIGN.md §3.5). Filled solve-on-settle by
	 * BreakByEquilibrium below the block cap and cleared by SolveAndBreak so a re-solve returns
	 * no stale readout; above the cap nothing fills it and the overlay falls back to the router.
	 * From a different solve than the verdict (min-violation, not maximise-lambda), so it is kept
	 * apart from ConnectionForces and never feeds a break decision.
	 */
	TArray<FConnectionReadout> ConnectionReadoutCache;

	/**
	 * Observability counter (test scaffolding) — min-violation readout LPs run since the last
	 * SolveAndBreak began. Reset at the top of SolveAndBreak (accumulates across the cascade),
	 * bumped per call. Read only by GetMinViolationReadoutSolveCount; drives no behaviour.
	 */
	int32 MinViolationReadoutSolves = 0;

	/**
	 * How many times SolveLoads has been entered, ever. See NumSolves. Not solver output — it
	 * sits below the arrays because it is the only field a solve accumulates rather than
	 * rebuilds. Nothing clears it and nothing branches on it.
	 */
	int32 SolveCount = 0;
};
