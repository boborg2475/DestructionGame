// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Connection.h"

/*
 * Forward-declared so the equilibrium gate can take the LP problem and result by const
 * reference without Structure.h pulling in the whole oracle header — the full definitions
 * live in Core/RigidBlock, which Structure.cpp includes.
 */
namespace RigidBlockOracle
{
	struct FOracleProblem;
	struct FOracleResult;
}

/*
 * Forward-declared so a piece can carry a pointer to what it is made of without
 * Structure.h pulling in the material library — the concrete definition lives in
 * Core/Profiles/MaterialProfiles.h, which a caller that assigns a material includes.
 */
namespace DestructionProfiles
{
	struct FMaterialProfile;
}

/**
 * A piece of a structure: plain data, deliberately.
 *
 * No actor, no component, no transform. A piece is a mass that either rests on
 * the earth or does not, an identity that connections can refer to, and — once it
 * has been removed — a hole where all three used to be. Whoever owns the world
 * resolves the handle back to something visible; the solver never needs to.
 */
struct FStructurePiece
{
	/** Position of this piece in its structure's piece array. */
	int32 Index = INDEX_NONE;

	/** Mass in kilograms. Real published values, unconverted — see DESIGN.md §3. */
	double MassKg = 0.0;

	/**
	 * Where this piece's weight acts, cm — the one position a piece carries.
	 *
	 * Not a transform or bounding box: the solver only measures how far the load path
	 * misses a joint's centroid, and a point is all that needs. Zero is not "at the
	 * origin", it is "nobody said" — see bHasCentreOfMass.
	 */
	FVector CentreOfMassCm = FVector::ZeroVector;

	/**
	 * Whether anyone supplied that centre of mass.
	 *
	 * False by default, so a piece nobody placed carries no eccentricity rather than
	 * claiming to sit at the world origin — a lever arm of metres invented out of a
	 * defaulted field for a wall laid off-origin. Such a piece loads its joints
	 * exactly as it did before moments existed.
	 */
	bool bHasCentreOfMass = false;

	/**
	 * Whether this piece rests on the earth.
	 *
	 * A grounded piece terminates the flow of load: whatever reaches it is absorbed
	 * by the ground rather than passed on to another connection.
	 */
	bool bIsGrounded = false;

	/**
	 * Whether this slot still holds a piece, which is what makes a handle stable.
	 *
	 * A removed piece is tombstoned: the slot stays, the handle stays valid forever,
	 * and nothing moves down to fill the gap. Handles are array indices with three
	 * arrays hanging off them, so compacting would silently re-point every connection
	 * above the hole at the wrong piece.
	 *
	 * Do not add a free list on top of this. Reusing a slot without a generation
	 * counter brings the bug back worse: a stale handle then names a different live
	 * piece, undetectably, where a dangling index can at least be range-checked. If
	 * slots ever need reuse, that needs generational handles (index plus a counter
	 * bumped on reuse), not a free list. Scenarios are bounded today, so the leak is
	 * fine.
	 *
	 * False by default so GetPiece's placeholder for an unknown handle reads as dead,
	 * matching every other accessor's fail-closed answer.
	 */
	bool bIsInTheStructure = false;

	/**
	 * What this piece is made of, so a joint can pair its connection with its two
	 * faces' materials through DestructionForce::EffectiveBondedStrength.
	 *
	 * Nullptr means "nobody said what this is made of", the fail-safe default: no
	 * shipped fixture assigns a material today, so every joint keeps reading its bare
	 * connection bit-for-bit wherever a material is absent.
	 *
	 * Non-owning; must point at a static / program-lifetime profile — the `extern
	 * const` definitions in MaterialProfiles.cpp (via `AllMaterialProfiles()` or a
	 * named `DestructionProfiles::Xxx`). Never store the address of a temporary.
	 */
	const DestructionProfiles::FMaterialProfile* Material = nullptr;
};

/**
 * WHY a piece is or is not being held up, refining IsPieceSupported's composite
 * answer. Two different things produce "this piece is falling", and nothing outside
 * the solver can tell them apart otherwise:
 *
 *   - real physics — nothing is holding it up, its supports are gone or falling;
 *   - a solver limitation — the piece is in an unroutable knot, reported
 *     conservatively rather than solved (DESIGN.md §3, see IsPieceSupported).
 *
 * That distinction lets a collapse test claim a wall fell from load rather than from
 * the solver giving up on a loop: assert no piece was Stranded when it went.
 *
 * Falling is first so zero is the fail-closed answer: a default-constructed entry
 * reads "nothing is holding this up", the same direction every FStructure accessor
 * fails in. Grounded at zero would make an uninitialised array claim the whole
 * structure rests on the earth.
 */
enum class EPieceSupport : uint8
{
	/**
	 * Nothing is holding this piece up, and the solver is not the reason.
	 *
	 * Also the answer for a handle naming no piece, a removed piece, an unsolved
	 * structure, and a piece resting only on a knot — it isn't itself in the knot, it
	 * has simply lost what was carrying it, which is ordinary physics.
	 */
	Falling,

	/** Resting on the earth. Ground terminates the flow of load; nothing carries this. */
	Grounded,

	/** Reaches the earth through supports, and the solver could route its load there. */
	Supported,

	/**
	 * In an unroutable knot: this piece is ultimately one of its own supports.
	 *
	 * Not physics — that's the point of telling it apart from Falling. It reaches the
	 * ground through the support relation, but dividing load round a loop needs a
	 * rule that does not exist yet, so it is reported unsupported conservatively.
	 */
	Stranded,
};

/**
 * What a connection is to one of its pieces — the two-tier classification. The
 * relation is directed: the same joint is a bed joint beneath the piece above it and
 * a bed joint above the piece below it, so a role only means anything once you say
 * whose. DESIGN.md §3's tiering is the whole of it: a substantially vertical
 * interface normal bears in compression, a substantially horizontal one only in
 * shear, split at 45 degrees.
 *
 * None is the zero enumerator, so a zero-filled entry claims no tier rather than the
 * strongest one — same direction EPieceSupport::Falling fails in.
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
 * The graph that owns pieces and connections, and works out what each connection
 * carries.
 *
 * Load flows downward. A piece transmits its own weight, plus everything it
 * receives from above, into whatever connections support it. A grounded piece
 * terminates that flow, and where more than one connection supports a piece the
 * load splits weighted by interface area.
 *
 * Support is two-tiered by the joint's own orientation (DESIGN.md §3): a
 * substantially vertical normal is a bed joint, bearing in compression; a
 * substantially horizontal one is a head joint, carrying only in shear. A piece's
 * supports are the bed joints beneath it, falling back to head joints only when it
 * has none. A joint that has given leaves the support relation entirely, so a piece
 * whose only bed joint has gone falls back to its head joints instead of reading
 * falling — redistribution is that share moving onto the neighbours rather than
 * evaporating. A removed piece leaves the graph the same way, one level up: every
 * joint that held it goes with it, and removal differs from a joint failing in one
 * recorded respect — nothing was overloaded, so nothing enters the collapse
 * sequence (see GetBreakPass).
 *
 * Everything is computed over that support relation, not raw connectivity —
 * routing by graph distance let a short sideways path exclude a bed joint entirely,
 * so a brick spanning a gap bore none of the wall stacked on it. A support that is
 * itself falling is not a support, so the split only ever uses supports with their
 * own path to the earth; a share handed to one that never reaches ground is simply
 * lost. "Supported" therefore means reaching the ground through supports, not
 * connections, and pieces have no position here — two can each hang from the other
 * and neither be held up by anything. This is also what makes DESIGN.md's worked
 * example fall out rather than being special-cased: a piece with no bed joint
 * beneath it pushes its whole weight through its head joints, which FConnection
 * resolves as shear rather than compression.
 *
 * Units. Unreal's gravity is -980 cm/s2 and mass is in kilograms, so weight in
 * Unreal force units is MassKg * 980 — a 2.72 kg brick weighs 2666. That already is
 * the 1 N = 100 uu conversion (2.72 kg x 9.81 = 26.7 N, x 100 = 2670); applying the
 * factor a second time is the standard way to be wrong by exactly 100x here.
 *
 * Scope. SolveLoads computes loads without breaking anything, so it can be re-run;
 * breaking is the separate, deliberate SolveAndBreak.
 *
 * A plain struct with no UObject and no world, so owning the load maths ourselves
 * (DESIGN.md §3) stays plain arithmetic over a graph — fast and deterministic to test.
 */
struct FStructure
{
	/**
	 * Add a piece and return its handle.
	 *
	 * Rejects a mass that is negative or not finite, returning INDEX_NONE and
	 * adding nothing. Zero is allowed: a massless piece is meaningful.
	 */
	int32 AddPiece(double MassKg, bool bIsGrounded = false);

	/**
	 * Add a piece that knows where its weight acts, and return its handle.
	 *
	 * The same door, with one more fact through it: every rejection the two-argument
	 * form makes it makes too; a centre of mass only buys a piece the ability to load
	 * a joint eccentrically.
	 *
	 * A non-finite centre is refused rather than stored and ignored — a NaN there
	 * launders into a NaN lever arm the moment anything subtracts a joint centroid
	 * from it, the same reason AddConnection refuses a joint's own bad centre.
	 */
	int32 AddPiece(double MassKg, bool bIsGrounded, const FVector& CentreOfMassCm);

	/**
	 * Add a connection and return its handle, or INDEX_NONE if it is not a joint.
	 *
	 * The structure owns the graph and is the only place that can tell a valid piece
	 * handle from a nonsense one, so validation lives here. Rejected: handles outside
	 * the piece array (INDEX_NONE included), a self-connection, a non-finite or
	 * non-positive interface area (has to fail closed here — the load split divides
	 * by total supporting area, and a zero or NaN area leaves nothing sensible to
	 * divide by), and a normal that describes no plane.
	 *
	 * The joint's own rectangle, when supplied, is checked too — a rectangle that
	 * doesn't describe the face its area describes is a plausible number with the
	 * wrong lever arm, the same class of fault Layout.h makes inexpressible for the
	 * normal/A-B pairing. Rejected: a rectangle whose 4 x h_u x h_v disagrees with the
	 * area, one with an extent on the normal's own axis (a box, not a face), a
	 * negative or non-finite half-extent or centre, and any rectangle at all on a
	 * non-axis-aligned normal (the in-plane frame is "the two world axes that are not
	 * the separation axis", and a tilted normal names no such pair). Zero extents are
	 * not degenerate — they mean no bending capacity was ever measured, which is
	 * healthy, since the area alone answers a centred load exactly.
	 *
	 * Validates, never normalises: an accepted rectangle is stored bit for bit as
	 * given, since quietly zeroing one that could not be verified would turn a refusal
	 * into a joint that reads as healthy.
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
	 * The valid handle range, which never shrinks — not the number of live pieces.
	 *
	 * Callers iterate 0..NumPieces() and resolve each handle, so this has to keep
	 * meaning the array extent: a live count would silently skip real pieces in every
	 * such loop. Ask IsPieceRemoved whether a slot still holds anything, and
	 * NumLivePieces for a count.
	 */
	int32 NumPieces() const;

	/** How many pieces have not been removed. For counts, never for iteration. */
	int32 NumLivePieces() const;

	int32 NumConnections() const;

	/**
	 * How many times this structure has been solved, ever.
	 *
	 * The only observable a cost claim can be made against. Solving is deterministic
	 * and non-destructive, so solving twice is invisible in every other reading of the
	 * graph — "the batched commit solves once rather than once per piece" is
	 * unfalsifiable without a counter. A full solve is tens of milliseconds at
	 * scenario scale, so one vs. ten is a click vs. a stutter.
	 *
	 * Counts solves, not changes — SolveAndBreak runs several, deliberately. No budget
	 * or limit; no production code branches on it.
	 */
	int32 NumSolves() const;

	/**
	 * Whether every piece and every joint still in this structure knows where it is.
	 *
	 * "Nobody supplied positions, so there are no moments" has to be askable, and this
	 * is the only thing that can ask it: a load path with no eccentricity and one
	 * nobody measured produce the identical answer, so a readout showing zero can't
	 * say which it saw. A conjunction of two halves, neither a judgement call — a
	 * moment needs a point to act at and a rectangle to be resisted with; either
	 * missing anywhere means some joint answers a centred load because it has to.
	 *
	 * Over what is still in the structure, not the arrays: a removed piece or a given
	 * joint carries and routes nothing, so a tombstone left by a piece nobody ever
	 * placed must not condemn a structure whose live half is fully described.
	 *
	 * True for an empty structure — an empty conjunction is true, which keeps this
	 * composable (adding a fully-described piece to a complete structure leaves it
	 * complete, no first-piece special case). Pieces with no positions is the state
	 * that would actually mislead someone, and that reads false.
	 */
	bool HasCompleteGeometry() const;

	/** Out-of-range handles return a default-constructed placeholder. */
	const FStructurePiece& GetPiece(int32 PieceIndex) const;
	const FConnection& GetConnection(int32 ConnectionIndex) const;

	/**
	 * Mutable access to a built connection, for a diagnostic probe that re-authors a
	 * joint's strength on an already-built structure and re-solves (the
	 * mortar-sensitivity sweep). No logic — it hands back the reference; an
	 * out-of-range handle returns a throwaway placeholder so a bad index writes
	 * nowhere observable rather than reading out of bounds.
	 */
	FConnection& GetConnectionMutable(int32 ConnectionIndex);

	/**
	 * Recompute what every connection carries and which pieces reach the ground.
	 *
	 * Joints that have already given are skipped entirely — they are out of the
	 * structure, so they neither bear load nor conduct support.
	 *
	 * Non-destructive: no connection may give as a result of solving, however
	 * overloaded it is.
	 */
	void SolveLoads();

	/**
	 * Solve, break every joint that is over capacity, re-solve so their load moves onto
	 * the neighbours, and repeat until nothing more gives.
	 *
	 * Every joint over capacity gives in the same pass, each stamped with the pass
	 * number that broke it. Ordering within a pass is arbitrary; ordering between
	 * passes is real and is the sequence a collapse is played back in. Terminates
	 * because joints never heal: a pass that breaks nothing is the last one.
	 *
	 * A pass breaks on two grounds, not one. Beside the per-joint capacity sweep, the
	 * equilibrium gate (BreakByEquilibrium — DESIGN.md §7 step 4) asks the rigid-block
	 * LP whether the whole structure has any admissible equilibrium with self-weight,
	 * and on "no" gives the bearings of the body that has lost the earth, in the same
	 * pass and stamp. Scoped by a block cap; see its own contract.
	 *
	 * Pass numbers are global to the structure, not the call: a second cascade
	 * continues from the highest stamp already written, and nothing earlier is
	 * rewritten — consumers read a shared number as "these gave simultaneously",
	 * true only if numbering never restarts.
	 *
	 * @return the number of passes THIS CALL broke at least one joint in; zero for a
	 *         structure that stands as built. Per-call, unlike the stamps, so it can be
	 *         smaller than the pass numbers that call wrote.
	 */
	int32 SolveAndBreak();

	/**
	 * The injectable block cap that scopes the equilibrium gate's authority by
	 * structure size (PROMOTION_DESIGN.md §12 D6-c): authoritative when the block
	 * count is <= this cap, fail-closed to the router (no overturning check) above it.
	 * The production default is the conservative end of the measured ~84-104-block
	 * band; a test sets a low cap to drive the decline arm without an over-cap fixture.
	 */
	void SetEquilibriumGateBlockCap(int32 MaxBlocks);

	/**
	 * The injectable region cap the real SolveAndBreak cascade hands
	 * ProveRegionalCollapse in its above-cap decline arm (REGIONAL_PROVER_PLAN.md §4).
	 * Bounds |region ∪ grounded boundary| the flood may pose, mirroring
	 * SetEquilibriumGateBlockCap: a mechanism larger than the cap is an accepted miss
	 * (the router covers it, and the prover only ever moves toward collapse, so a
	 * miss never over-holds unsoundly).
	 *
	 * The default is the modest RegionalProverBlockCap (see its member), chosen for
	 * per-pass latency, not the ratified 200 — 200 is a settable ceiling reachable
	 * through here for tests and tuning. A bare store that drives no behaviour on its
	 * own; the cascade seam reads it.
	 */
	void SetRegionBlockCap(int32 MaxBlocks);

	/**
	 * Record what a piece is made of, so a cross-material joint can reach its two
	 * faces' materials. A bare store — no branch, no arithmetic — so it drives no
	 * behaviour on its own; the router and oracle bridge consult it via
	 * DestructionForce::EffectiveBondedStrength.
	 *
	 * Out-of-range handles are ignored. A null pointer clears the assignment back to
	 * "nobody said", the fail-safe default every piece starts at.
	 */
	void SetPieceMaterial(int32 PieceIndex, const DestructionProfiles::FMaterialProfile* Material);

	/**
	 * Flag this structure as genuinely 3D, so the oracle bridge poses it to the 3D LP
	 * (FOracleProblem::Dim = Dim3D) and stops refusing its out-of-plane (Y) joint
	 * normals, while a 2D structure's Y-normal stays loudly refused.
	 *
	 * A structure-level flag rather than "infer 3D from any Y-normal joint": a 2D
	 * structure that has accidentally acquired a Y-normal joint must still be refused
	 * (the 2D X-Z oracle cannot honestly project it), not silently promoted — so the
	 * intent to be 3D has to be stated rather than guessed. False by default, so every
	 * existing 2D fixture is unchanged and stays refused if it ever grows a Y-normal.
	 *
	 * When set, `BuildRigidBlockProblem` poses the structure as `Dim3D` — lifting the
	 * Y-normal refusal and posing the block's plan-Y and each joint's full 3D geometry
	 * (NormalY, CentreYCm, HalfUCm/HalfVCm) — instead of the 2D X-Z projection.
	 */
	void SetThreeDimensional(bool bIsThreeDimensional);

	/** Whether SetThreeDimensional flagged this structure 3D. False by default (2D). */
	bool IsThreeDimensional() const;

	/**
	 * The test entry for the regional collapse prover (REGIONAL_PROVER_PLAN.md slice
	 * 1) — region extraction -> grounded-boundary pose -> SolveRigidBlock ->
	 * Falling-only stitch, end to end, without touching the real SolveAndBreak cascade
	 * (so no committed scenario verdict changes). The real cascade wires the same
	 * machinery in behind a settable region cap; this entry is how it is driven and
	 * tested in isolation.
	 *
	 * Runs the router baseline (SolveLoads), then floods a region from Seed by
	 * joint-hops over PieceJoints up to RegionBlockCap, pins the one-hop frontier ring
	 * grounded, poses R + boundary at feasibility with first-crack rows through the
	 * grounded-boundary bridge overload RigidBlockOracle::BuildRegionalProblem, calls
	 * SolveRigidBlock, and on a certified Falls marks the moved interior pieces
	 * Falling (never Supported — one-directional, toward collapse only) and severs
	 * the intact joints the mechanism opens, mapped back through the problem's
	 * ConnectionOfJoint provenance. A bridge refusal or a non-Falls outcome releases
	 * nothing and defers to the router baseline.
	 *
	 * With RegionBlockCap >= the block count the region floods the whole structure, so
	 * the grounded boundary is the earth alone (no cut) and the released set equals
	 * the whole-structure ground-only LP's moving set.
	 *
	 * @param Seed           The pieces the region grows from (the disturbance neighbourhood).
	 * @param RegionBlockCap The largest region the flood may reach (the grounded boundary rings it).
	 * @return the number of pieces the prover released (marked Falling) this call.
	 */
	int32 SolveAndBreak_WithRegionalProver(const TArray<int32>& Seed, int32 RegionBlockCap);

	/**
	 * Instrumentation: the number of blocks the last SolveAndBreak_WithRegionalProver
	 * call posed to the oracle, i.e. |region ∪ grounded boundary|. Bounds "region ∪
	 * grounded boundary <= RegionBlockCap" (REGIONAL_PROVER_PLAN.md §1): the flood
	 * holds a candidate out as the grounded ring rather than admit it when doing so
	 * would push |R∪B| over the cap, so this count comes in at or under
	 * RegionBlockCap.
	 *
	 * Stamped only when a prove actually posed a problem, so a later call whose
	 * bridge pose is refused leaves the previous value in place. INDEX_NONE until
	 * the first regional prove has posed a problem.
	 */
	int32 GetLastRegionalProblemBlockCount() const;

	/**
	 * Instrumentation for which physics the last equilibrium-gate pose was posed in: 2
	 * for a 2D (X-Z) problem, 3 for a 3D one, INDEX_NONE until a pose has been built.
	 *
	 * `BuildRigidBlockProblem` poses the cheapest sound problem — 2D whenever every
	 * posed joint has an in-plane normal and every posed row sits at one common Y, 3D
	 * otherwise, with `IsThreeDimensional` staying the stated permission to pose 3D
	 * rather than the choice itself. The two poses are not the same physics (2D rows
	 * carry the exact Coulomb cone, 3D rows an inscribed octagon — DESIGN §8), which
	 * is why the reading is worth pinning. Stamped on pose, not on call, exactly as
	 * `GetLastRegionalProblemBlockCount`.
	 *
	 * An int, not `RigidBlockOracle::EOracleDim`: Structure.h forward-declares the
	 * oracle's two structs precisely so it never pulls in that header, and an enum
	 * cannot be forward-declared without fixing its underlying type here.
	 */
	int32 GetLastEquilibriumProblemDim() const;

	/**
	 * What one solve of the loads cost, phase by phase — observability only
	 * (2026-09-18, the warehouse course-37 experiment). Wall-clock milliseconds from
	 * FPlatformTime, so the numbers are about this machine on this run and are meant
	 * to be read, never asserted against a threshold.
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
		 * What each iteration found, one entry per iteration: pieces reached from the ground, pieces
		 * that overturned off their supports, pieces stranded in a load cycle, and refused-arch members
		 * released. Every iteration but the last changes at least one of the last three; the last is
		 * the one whose loads are the answer.
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
	 * What one prover pose cost — one LP the regional collapse prover posed while
	 * growing/re-flooding its region within a pass. A pass poses several of these
	 * (RegionalPoses of them), and the per-pass aggregates on FBreakPassReport
	 * (RegionalPoses, RegionalLpPivots, RegionalLpMs, RegionalLastBlocks,
	 * bRegionalFell) are this record summed/reduced over the pass — the faithful
	 * decomposition that says whether a slow pass is one big pose or several.
	 *
	 * Test-only observability, exactly like GetLastRegionalProblemBlockCount: no
	 * production code may branch on it. Wall-clock LpMs is meant to be read, never
	 * asserted against a threshold.
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
	 * WHAT ONE PASS OF THE BREAK CASCADE DID AND COST. A pass is one SolveLoads, then the equilibrium
	 * gate (which above the block cap DECLINES to the router without posing anything), then — only
	 * when it declined — the per-joint capacity sweep and the regional collapse prover.
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

		/** |region ∪ grounded boundary| of the prover's FINAL pose this pass, or INDEX_NONE. */
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
	 * The break-decision report: what the last SolveAndBreak did, pass by pass, and
	 * what it cost.
	 *
	 * Observability, not authority — filled as a side effect of SolveAndBreak and
	 * read by diagnostics and experiments; no production code branches on it. The
	 * terminal pass (the one that broke nothing, whose loads are the settled state)
	 * is included, so a structure that stands as built reports one pass with zero
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
	 * Which breaking pass gave this joint, counted from 1, or INDEX_NONE if no pass did
	 * — including for an out-of-range handle, which is not a joint that broke.
	 *
	 * INDEX_NONE does not mean "still intact" — that's a trap this contract used to
	 * set. It means "did not fail under load in a cascade pass", which is also true of
	 * a joint that went because a piece it held was removed: that joint never snapped,
	 * it was deleted, and phase 5 replays these stamps as the sequence of a collapse,
	 * so it must not appear in that sequence at all. Whether a joint is still in the
	 * structure is HasGiven's question, not this one.
	 *
	 * The two together are a complete, unambiguous encoding:
	 *
	 *     intact              HasGiven false, INDEX_NONE
	 *     went with a piece   HasGiven true,  INDEX_NONE
	 *     broke in pass N     HasGiven true,  N >= 1
	 */
	int32 GetBreakPass(int32 ConnectionIndex) const;

	/**
	 * Which authority severed a joint, for observability — the break decision has
	 * three, and an experiment that wants to see where a collapse came from needs to
	 * tell a below-cap gate ruling apart from a per-joint capacity failure (the sweep)
	 * and from a proven regional mechanism (the prover). The codes:
	 *
	 *     1  the below-cap equilibrium gate (BreakByEquilibrium)
	 *     2  the per-joint capacity sweep (BreakByCapacitySweep)
	 *     3  the regional collapse prover (ProveRegionalCollapse)
	 *
	 * INDEX_NONE means no authority severed it: it is either intact, or it went with a
	 * removed piece — RemovePiece severs without stamping, exactly as it leaves the
	 * break pass INDEX_NONE. Read-only companion to GetBreakPass; no production code
	 * may branch on it.
	 */
	int32 GetBreakAuthority(int32 ConnectionIndex) const;

	/**
	 * What this connection is to this piece: the solver's own two-tier decision,
	 * exposed.
	 *
	 * The same decision SolveLoads routes by, not a second one — it is the single
	 * most load-bearing classification in the solver, so a readout that re-derived it
	 * would be a copy that can disagree with the routing it claims to explain. Same
	 * reason GetConnectionUtilisation is one line delegating to
	 * FConnection::UtilisationUnder.
	 *
	 * Pure geometry, and needs no solve: it reads the joint's normal and which end the
	 * piece is, so it answers before anything has been solved and keeps answering for
	 * a joint that has given — a given joint leaves the support relation, but what it
	 * used to be is exactly what a debugger is looking at.
	 *
	 * None for a handle naming no connection, for a piece not on this connection, and
	 * for a normal that will not normalise.
	 */
	EJointRole GetJointRole(int32 ConnectionIndex, int32 PieceIndex) const;

	/**
	 * The force this connection carries, in Unreal force units, after SolveLoads.
	 *
	 * A vertical vector of the accumulated magnitude — gravity doesn't change
	 * direction because a joint is vertical. FConnection::ApplyForce resolves it
	 * against the joint's own normal, which is why the same vector is compression on
	 * a bed joint and shear on a head joint.
	 *
	 * Except at the springing of an arch: an opening spanned by the group rule pushes
	 * sideways as well as down, equally and oppositely at its two ends, so a
	 * springing's force is (H, 0, -V) rather than (0, 0, -V). It still resolves the
	 * same way — a horizontal component on a bed joint is shear by ClassifyForce's
	 * own definition, so the arch needs no new axis and no new strength
	 * (ARCHING_DESIGN.md). A player seeing a non-vertical force on a springing is
	 * seeing the arch: the joint is pushed sideways by masonry bridging the hole
	 * beside it, held by Mohr-Coulomb friction from the weight on the same patch.
	 *
	 * Its sign depends on which end is being held up: per ConnectionLoad.h the force
	 * belongs to PieceB, so a joint naming the loaded piece second carries it
	 * downward and one naming it first carries the upward reaction — a force stored
	 * against the wrong end turns compression into tension.
	 *
	 * Zero for an out-of-range handle, and zero for a connection with no path to
	 * ground: nothing there is being held up, so there is no static load path to
	 * report.
	 */
	FVector GetConnectionForce(int32 ConnectionIndex) const;

	/**
	 * The bending moment this connection carries about its own centroid, uu.cm, after
	 * SolveLoads.
	 *
	 * One of the three things GetConnectionUtilisation is computed from, and it
	 * exists because without it that accessor cannot state its own contract:
	 *
	 *     GetConnectionUtilisation(I)
	 *         == GetConnection(I).UtilisationUnder(
	 *                GetConnectionForce(I), GetConnectionMoment(I),
	 *                GetConnectionCompositeDepthCm(I))
	 *
	 * The moment parameter is defaulted, so an assertion written without this
	 * accessor silently supplies zero and holds however far the two have drifted.
	 * Separate from the force, never encoded in it — folding a moment in as extra
	 * length or a tilt would make the one vector a readout uses to explain the load
	 * stop describing it.
	 *
	 * Zero is "no eccentricity", not a tolerance: it is what a centred load, a piece
	 * nobody placed, and a joint whose rectangle nobody measured all produce exactly —
	 * which is what lets every geometry-free fixture keep reading what it always read.
	 * HasCompleteGeometry is what tells those apart; this accessor does not.
	 *
	 * Zero for an out-of-range handle and zero before anything has been solved, same
	 * scope as GetConnectionForce.
	 */
	FVector GetConnectionMoment(int32 ConnectionIndex) const;

	/**
	 * How deep the bonded masonry standing over this connection is, cm, after
	 * SolveLoads.
	 *
	 * The third thing GetConnectionUtilisation is computed from, here for the same
	 * reason GetConnectionMoment is (see the identity in its comment); both trailing
	 * parameters are defaulted, so an assertion written without this accessor
	 * silently supplies zero.
	 *
	 * A length, not a section, and that is the seam: what the solver knows is how
	 * much wall is standing on the plane, not which of the joint's in-plane extents
	 * that depth pairs with — that is the joint's own business.
	 *
	 * Non-zero only where a joint is actually bent: a bed joint beneath a placed piece
	 * whose load reaches the ground through it alone, carrying a non-zero moment.
	 * Everywhere else this is zero and the joint reads its own bed patch, bit for bit
	 * as before composite action existed.
	 *
	 * Zero is "no masonry was measured", the fail-closed value: the relief is
	 * withheld and the joint reads more heavily loaded, the safe direction for a
	 * quantity whose whole job is to make a joint read less.
	 *
	 * Zero for an out-of-range handle and zero before anything has been solved, same
	 * scope as GetConnectionForce.
	 */
	double GetConnectionCompositeDepthCm(int32 ConnectionIndex) const;

	/**
	 * How close this connection is to failing under the load the last solve gave it.
	 *
	 * The strain readout's question, answerable without damaging anything:
	 * FConnection::UtilisationUnder is the non-mutating evaluator, applied here to
	 * GetConnectionForce. Nothing re-derives the break decision — a third hand-copy
	 * of it in production is exactly what this accessor prevents.
	 *
	 * Zero for a joint that has given (it carries no force, and the latch is not what
	 * produces that zero — a caller must not read a low ratio as "intact"; HasGiven is
	 * the authoritative state). Fails closed for a handle naming no joint, returning
	 * TNumericLimits<double>::Max() rather than zero — zero would read as "unloaded
	 * and perfectly healthy", the one answer that must never come back for something
	 * that is not a joint.
	 *
	 * Zero before anything has been solved, because no load has been routed yet. Same
	 * scope as GetConnectionForce, which it reads.
	 */
	double GetConnectionUtilisation(int32 ConnectionIndex) const;

	/**
	 * The joint's effective strength — its bare connection paired with its two faces'
	 * materials through the weakest-link rule (SHED_PATH.md Phase B / B3). The single
	 * pairing point both production strength paths consult: the router reads it here
	 * in GetConnectionUtilisation, and the oracle bridge reads it to fill each LP
	 * strength row, so the two can never disagree about a cross-material joint.
	 *
	 * Gated on both faces carrying a material: only when the connection's two pieces
	 * each name one is DestructionForce::EffectiveBondedStrength consulted; otherwise
	 * the bare connection strength is returned unchanged. A null material is "nobody
	 * said what this is made of", and every fixture that assigns none — today, all of
	 * them — reads its bare connection bit for bit. Computed on demand: materials
	 * don't change mid-solve, so there's no cache to invalidate.
	 *
	 * The bare connection strength for an out-of-range handle, since GetConnection
	 * hands back a placeholder and neither of its (absent) pieces names a material.
	 */
	FConnectionStrength EffectiveJointStrength(int32 ConnectionIndex) const;

	/**
	 * One connection's strain readout from the cached min-violation LP (Slice 6b,
	 * PROMOTION_DESIGN.md §3.5). Below the block cap a settled structure solves the
	 * min-violation (goal-programming) LP once and caches its per-joint result, keyed
	 * back to production connections through the bridge's ConnectionOfJoint
	 * provenance; this accessor hands one connection's entry back so the strain
	 * overlay can show how loaded a joint is from the LP's own force distribution
	 * rather than the router's per-joint estimate.
	 *
	 * NormalUu is N = n1 + n2 (compression positive, so a tension joint reads
	 * negative); MomentUuCm is M about the joint centre; ViolationUu is the
	 * non-negative strength-row slack (0 within capacity, > 0 by the amount the
	 * least-infeasible force system had to exceed a capacity here); Utilisation maps
	 * that onto the same 0 -> 1 -> >1 scale GetConnectionUtilisation produces.
	 *
	 * bPresent is false until a below-cap settle solved and cached the readout. Above
	 * the cap the gate declines and this stays absent — the overlay falls back to
	 * GetConnectionUtilisation. Absent for an out-of-range handle, and before any
	 * solve, exactly as the other solver accessors are empty before they are filled.
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
	 * Observability only — how many times CacheMinViolationReadout ran the min-violation
	 * readout LP during the last SolveAndBreak. Incremented once per call and reset
	 * once at the top of SolveAndBreak, so it counts readout solves across a whole
	 * cascade rather than one pass. Test scaffolding for the "compute the readout once
	 * per settle" fix: it drives no behaviour, only lets a test watch how many
	 * separate readout LPs a below-cap cascade pays for.
	 */
	int32 GetMinViolationReadoutSolveCount() const;

	/**
	 * Whether this piece has a path to a grounded piece through supports, after
	 * SolveLoads. Grounded pieces are supported by definition.
	 *
	 * Through supports, not connections: a piece glued underneath a grounded slab is
	 * joined to the earth and is not held up by it. And a path the solver could
	 * actually route: pieces in a cycle of the support relation — a course of bricks
	 * spanning a two-brick gap, each falling back to its neighbours' head joints —
	 * cannot be put in an accumulation order, so their load never reaches the earth
	 * and they are reported unsupported. It is not a claim the cycle has been solved:
	 * dividing load round a loop needs a rule that does not exist yet.
	 *
	 * Only the pieces caught in the knot, though — DESIGN.md §3 is explicit that a
	 * piece is unsupported only when it genuinely has no load path to ground. A piece
	 * beneath a knot keeps its support and carries everything except the unroutable
	 * contribution.
	 *
	 * False for an out-of-range handle. This is the last solve's answer, and removal
	 * does not rewrite it — this reads solver output, which only SolveLoads writes:
	 *
	 *     never solved      false, for every handle — there is no answer yet
	 *     removed           the last solve's answer, unchanged, until the next solve
	 *     after that solve  false, and a removed grounded piece is no longer earth
	 *
	 * The middle row is stale rather than wrong, deliberately: clearing that one entry
	 * on removal would leave a half-stale array, worse than a uniformly stale one with
	 * a documented scope. The gameplay shape that reaches it: remove a piece on
	 * player interaction, then ask about its neighbours to decide what to release to
	 * dynamics — re-solve first. Structure.RemovedPieceSupportNeedsASolve pins all
	 * three rows.
	 */
	bool IsPieceSupported(int32 PieceIndex) const;

	/**
	 * Why this piece is or is not being held up, after SolveLoads.
	 *
	 * IsPieceSupported is the composite answer and is unchanged; this refines it, and
	 * the two must never disagree:
	 *
	 *     IsPieceSupported(H)  ==  (GetPieceSupport(H) is Grounded or Supported)
	 *
	 * for every handle, at every moment, including before a solve and in the window
	 * between a removal and the next one.
	 *
	 * Stranded is only for pieces in the knot. A piece resting on one is Falling: it
	 * is not itself unroutable, it has simply lost the support that was carrying it —
	 * ordinary physics rather than a solver limitation.
	 *
	 * Same scope as IsPieceSupported, since it is the same solver output. There is
	 * deliberately no fifth enumerator for "removed": IsPieceRemoved answers that
	 * immediately and this one cannot, so a Removed value here would contradict a
	 * stale Supported from IsPieceSupported in exactly the window
	 * Structure.RemovedPieceSupportNeedsASolve pins.
	 *
	 * Falling for an out-of-range handle: nothing is holding up a piece that does not
	 * exist, and Stranded would be a positive claim about a knot that is not there.
	 */
	EPieceSupport GetPieceSupport(int32 PieceIndex) const;

	/**
	 * Whether the last solve actually computed a support state for this handle.
	 *
	 * The one thing GetPieceSupport cannot say. Falling means both "nothing is
	 * holding this up" and "nobody has asked yet", deliberately: it sits at
	 * enumerator zero so an absent answer cannot claim the structure rests on the
	 * earth. This buys back the ability to tell the two apart — PieceSupported is
	 * sized by SolveLoads and nothing else, so its extent is the set of handles the
	 * last solve answered for.
	 *
	 * False before any solve, for a handle added since the last one, and for a handle
	 * naming no piece. True for a removed piece inside that extent: the last solve
	 * did answer for it, and IsPieceSupported's contract already records the answer
	 * as stale until something re-solves.
	 *
	 * Who needs it: anything that turns "not held up" into an irreversible action
	 * rather than a readout, because the polarity inverts at that seam — as a
	 * command, Falling by default says "release the whole structure, foundation
	 * included" (fail-open, not undone by a later solve). A readout may take
	 * GetPieceSupport at face value; a command must ask this first.
	 */
	bool HasSupportAnswer(int32 PieceIndex) const;

private:
	/**
	 * One spanned opening, as the group pass leaves it: the seated pieces at its two
	 * ends.
	 *
	 * An arch is the only thing in this solver that is a fact about a run of pieces
	 * rather than about any one of them, and the thrust is the reason that has to be
	 * written down. H is one number for the whole opening, applied equally and
	 * oppositely at the two abutments — so the pass that computes it needs both ends
	 * at once, and a per-joint rule could not state it. That is ARCHING_DESIGN's trap
	 * 2: applying +H at one springing and forgetting the other gives the structure a
	 * net horizontal force out of nowhere while every joint still reads plausibly.
	 *
	 * The two ends are an order, not a direction in the world: which end is 0 falls
	 * out of whichever abutment the group walk happened to meet first, and nothing
	 * may depend on it.
	 */
	struct FSpannedArch
	{
		/**
		 * The seated pieces the group pushes against, split by which end of the
		 * opening they stand at. Both are non-empty — a group with nothing on one
		 * side does not span, and an arch with nothing to push against at one end is
		 * a cantilever.
		 */
		TArray<int32> Abutments[2];

		/**
		 * Unit direction toward end 0 — the way the arch pushes end 0's abutments,
		 * the exact negative of how it pushes end 1's.
		 *
		 * One vector rather than two, so the two thrusts sum to exactly zero rather
		 * than a rounding of it: +H*D and -H*D cancel bit for bit on every
		 * component; two separately normalised directions would not.
		 */
		FVector TowardEndZero = FVector::ZeroVector;

		/**
		 * L: how far apart the two ends of the opening stand, cm.
		 *
		 * Measured between the abutments' own centres, one mean per end, which is
		 * the clear opening to the centimetre for a running-bond wall: each
		 * springing keeps half a cell of bearing, so its centre sits half a cell
		 * outboard of the masonry that was taken out, and the two half cells make
		 * up the one that was. Seat centroid to seat centroid is half a cell wider
		 * and reads 5% high on a ten-cell hole.
		 *
		 * Recorded here because this is where it is known: the thrust pass has the
		 * abutments but not the group they stand either side of, and the span is a
		 * fact about the opening rather than either end of it.
		 */
		double SpanCm = 0.0;
	};

	/**
	 * Re-seat the pieces a hole has left with no seat at all onto the group they
	 * belong to.
	 *
	 * The group rule, and the one thing in the solver that reads geometry to decide a
	 * route. A hole one brick wide leaves two half-seated bricks that each keep their
	 * own seat, and HasArchingAbutment answers those alone. A hole wider than one
	 * brick leaves the bricks in the middle with no seat whatever: under the two-tier
	 * rule they fall back to their head joints, become each other's support, and the
	 * solver strands the pair as a two-node knot — so a wall that should span its
	 * opening loses its middle and everything above it.
	 *
	 * What those bricks are doing is spanning, a statement about a run of pieces
	 * rather than any one of them: contiguous unseated pieces form a group through
	 * their intact head joints, carrying only if something seated stands on both
	 * sides of it. Each member is re-seated toward its nearer abutment, acyclic by
	 * construction since every edge runs from a longer path to a shorter one.
	 *
	 * MOMENTS_DESIGN's discipline line is revised, not broken: GetJointRole still may
	 * never read geometry — the tier of one joint stays a fact about one normal and
	 * one pairing — and what reads geometry is this group rule sitting above the
	 * tier. It is gated on HasCompleteGeometry() and a no-op without it, so a
	 * geometry-free structure routes exactly as it did; both fuzz generators emit
	 * structures with no positions at all, 20,000 cases between them, the only
	 * property tests over routing this project has.
	 *
	 * The abutments are the group's own "incomplete seat" neighbours, one piece
	 * tighter than ARCHING_DESIGN's boundary: the group is the unseated run and the
	 * seated neighbours it pushes against are its abutments.
	 *
	 * @param PieceJoints          Every joint touching each piece, in ascending index order.
	 * @param PieceHasNoSeat       Which live, ungrounded pieces have no intact bed joint beneath.
	 * @param SupportConnections   What holds each piece up. Rewritten in place for the members of
	 *                             an abutted group, and left exactly as found everywhere else.
	 * @param PieceReseatedOnAnArch Set for each piece this rewrote, and false everywhere else.
	 * @param PieceInRefusedArchGroup Set for each member of a group that formed but whose
	 *                             opposition gate refused it — a one-sided cantilever with
	 *                             nothing to thrust into. These pieces were not re-seated (the
	 *                             gate declined), and left on their sign-blind head joints they
	 *                             become a mutual-support knot the solver strands. Marking them
	 *                             lets SolveLoads exclude them from the reachability walk exactly
	 *                             as it excludes an overturned piece, so a refused cantilever
	 *                             reads Falling rather than the false-knot Stranded. A
	 *                             geometry-free mutually-propping pair forms no group, so it is
	 *                             never marked and keeps its genuine cycle-strand.
	 * @param Arches               One entry per group that actually spans, naming the seated
	 *                             pieces at its two ends. Emptied first, so a structure with no
	 *                             geometry and one with no hole both come back with none.
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
	 *
	 * An arch pushes sideways, and something has to resist it. ARCHING_DESIGN.md:
	 *
	 *     d_e = min( cover above the span , 0.866 * L )      arching depth
	 *     r   = d_e / 3                                      thrust line rise, kern-limited
	 *     W   = the load the solver already accumulated      NOT a triangle
	 *     H   = W * L / (8r)      V = W / 2                  per abutment
	 *
	 * What resists it is already in the model: the thrust arrives horizontally at a
	 * bed joint, ClassifyForce calls a horizontal force on a bed joint shear, and
	 * shear capacity is already `c + mu*sigma_n` truncated at the profile's ceiling —
	 * no new axis, no new strength, no new profile data, so dry stone's zero cohesion
	 * refuses to arch at any span with no per-material branch anywhere.
	 *
	 * Run after the accumulation and never inside it: W is what the solver has
	 * already routed to the abutments' seats, so adding a shear component changes no
	 * split, no support list and no moment — every vertical answer is bit-identical
	 * to one computed without it.
	 *
	 * The cover usually decides: `d_e` is the smaller of the masonry actually
	 * standing over the opening and the angle's `0.866*L`, so `H` grows as `1/cover`
	 * while `V` falls with it — thin the brickwork over a hole and the thrust ratio
	 * `3L/(4 d_e)` blows up, which is why a wide opening near the top of a wall cannot
	 * arch at all. One cover per arch, the thinnest of the two ends — see
	 * MasonryDepthAboveCm.
	 *
	 * @param PieceJoints Every joint touching each piece, in ascending index order.
	 * @param Arches      What ReseatSpannedGroups found. Empty is the ordinary case and costs
	 *                    nothing.
	 */
	void ApplyArchingThrust(
		const TArray<TArray<int32>>& PieceJoints,
		const TArray<FSpannedArch>& Arches);

	/**
	 * The live piece resting on this one through an intact bed joint, or INDEX_NONE.
	 *
	 * One step of the upward walk, and also the question "is there a stack here at
	 * all". Two callers need it and neither may have its own copy: MasonryDepthAboveCm
	 * takes this step once per course, and SolveLoads asks it once before crediting a
	 * joint with any composite section at all. A piece with nothing standing on it is
	 * one unit, not a composite of anything — see the composite gate in SolveLoads for
	 * why that is the definition of the mechanism rather than a refinement of it.
	 *
	 * A joint that has given holds nothing up and is not a course of masonry either,
	 * and a removed piece is not standing anywhere. Both are excluded here, which is
	 * what makes the depth shorten as a wall comes apart.
	 */
	int32 PieceRestingOn(int32 Piece, const TArray<TArray<int32>>& PieceJoints) const;

	/**
	 * How much bonded masonry stands over one bed joint, cm, by a bounded upward walk.
	 *
	 * One measurement, two callers, for different reasons. ApplyArchingThrust wants
	 * the cover over a springing, because ARCHING_DESIGN.md's `d_e = min(cover,
	 * 0.866*L)` is the difference between a ten-cell hole reading 0.058 of its
	 * springing's capacity and reading 1.51 of it. SolveLoads wants the composite
	 * depth over a seat, because a stack of courses over a lost support resists its
	 * overturning moment as a deep beam of section `t*D^2/6` rather than one bed
	 * patch. Both are the same question — how much wall is standing on this plane —
	 * and two copies of the walk would agree until the day one of them was fixed.
	 *
	 * Counted in courses, the course the joint is under being the first of them (the
	 * spanning course for the arch, the corbelled brick itself for the composite
	 * section) — the shallowest answer is one course pitch, never zero.
	 *
	 * A walk over bed joints, never a spatial query: what is over the plane is what
	 * the connection graph says is over it, so this costs a step per course and needs
	 * no broadphase, no octree, no world. A joint that has given conducts nothing and
	 * stops the walk, which bounds the depth to masonry that is actually bonded.
	 *
	 * Bounded twice: `EnoughDepthCm` is the depth past which the caller's own answer
	 * cannot change — `0.866*L` for the arch, and effectively unbounded for the
	 * composite section, which stops when the wall does. The piece count is the
	 * second bound, pure defence against a graph whose normals claim A is above B and
	 * B above A.
	 *
	 * A chain, not a traversal: running bond puts two pieces above each brick, and
	 * this follows the first of them by ascending joint index. For a uniform wall the
	 * two columns reach the same place; a stepped or gabled wall would have its depth
	 * decided by which column the walk took, and nothing tests that yet.
	 *
	 * Zero where there is nothing to measure. Both callers guard on that rather than
	 * dividing by it, and for the composite section zero is already the no-relief
	 * answer.
	 *
	 * @param Piece         The piece standing on the joint.
	 * @param SeatJointIndex Its bed joint, which is the plane the masonry stands on.
	 * @param PieceJoints   Every joint touching each piece, in ascending index order.
	 * @param EnoughDepthCm The depth past which more masonry cannot change the caller's answer.
	 */
	double MasonryDepthAboveCm(
		int32 Piece,
		int32 SeatJointIndex,
		const TArray<TArray<int32>>& PieceJoints,
		double EnoughDepthCm) const;

	/**
	 * How deep the corbelling body standing on one bed joint is, cm — the floor under
	 * the composite depth, and the one part of it no lever arm may trim away.
	 *
	 * The corbelling courses generate the moment, so they cannot be refused the
	 * section they generate it with — bonded into one cantilevering body, needing no
	 * shear transfer to be engaged. Masonry above the cut is different: it isn't bent
	 * by the corbel's moment and has to be dragged into the section by shear over a
	 * distance, exactly what `lambda*e` bounds. So the two are bounded differently
	 * and only the second one is:
	 *
	 *     D  =  min( masonry above , max( THIS , lambda*|M|/|F| ) )
	 *
	 * The floor can never credit a course above the cut, because the floor is the
	 * cut: the walk stops at the first course that is not corbelling, by definition
	 * the first course of the wall standing over the body — an exact structural
	 * guarantee rather than a bound that happens to hold on the fixtures.
	 *
	 * Corbelling is "seated on exactly one course", a fact about the graph and
	 * nothing else: a brick laid in bond straddles two below it, while a brick
	 * stepped out over a raking cut has one seat and hangs off it.
	 *
	 * ⚠ It is true of every piece in a stack-bond column or a one-brick-wide wall,
	 * where the walk runs to the top and the floor becomes the whole wall — the very
	 * defect `lambda*e` was added to remove. The direction test that would refuse it
	 * (the centre of mass must lie outboard of its seat, exactly what
	 * HasArchingAbutment asks) is deliberately not built, because no fixture here can
	 * tell the two apart: a stack-bond wall has zero eccentricity at every seat, so
	 * no joint in one ever carries a moment and this is never reached. See
	 * CURRENT_STATE.md for the fixture that would be needed first.
	 *
	 * Which piece the walk steps to is decided by the predicate, not by
	 * PieceRestingOn: that chain takes the first bed joint above by ascending index,
	 * which on a filled corbel is the two-centimetre lap onto the course inboard
	 * rather than the eighteen-centimetre seat under the stepping front — measured,
	 * on the very fixture that drove this. So this walks the corbelling chain
	 * instead: the first piece resting here that is itself corbelling.
	 *
	 * @param Piece          The piece standing on the joint — always the body's first course.
	 * @param SeatJointIndex Its bed joint, which is the plane the body stands on.
	 * @param PieceJoints    Every joint touching each piece, in ascending index order.
	 */
	double CorbellingBodyDepthCm(
		int32 Piece,
		int32 SeatJointIndex,
		const TArray<TArray<int32>>& PieceJoints) const;

	/**
	 * Is there something on the overhanging side for this piece to arch against?
	 *
	 * The fourth gate of the arching rule, deciding whether a piece that has lost a
	 * seat bridges the hole or cantilevers over it. It asks for an intact head joint
	 * on the eccentric side — where the centre of mass sits, measured from the seat's
	 * own centroid — to a neighbour that reaches the ground and is not resting on the
	 * piece asking. FConnection::ArchingMomentScale owns the other half, the part one
	 * joint can answer alone.
	 *
	 * The direction is the whole of it. Both of this subsystem's eccentric fixtures
	 * have a live, supported neighbour on the seated side, not the eccentric one —
	 * asking only "is there an intact head joint" arches both, which halves every
	 * overhang in the game and stands the photographed failure back up.
	 *
	 * And the neighbour must not be hanging from us. Two unseated bricks propping
	 * each other over open air look locally identical to a real arch, so granting one
	 * would let a wall hang from nothing; the separating fact is that the piece must
	 * not appear among the neighbour's own supports. Deliberately not a reachability
	 * query — SolveLoads runs once per pass, and the cheap form is exact here because
	 * a longer loop is what LoadReturnsToPiece already strands.
	 *
	 * A group that spans supersedes that refusal without weakening it: widen the hole
	 * past one brick and the neighbour on the eccentric side is an unseated piece
	 * re-seated onto the group, so it does lean on this one and the one-step test
	 * refuses correctly. What makes it an arch instead is the far abutment —
	 * ReseatSpannedGroups only re-seats a group with something seated on both sides,
	 * so a neighbour carrying that mark has a reaction beyond it and the thrust has
	 * somewhere to go. Without the mark nothing here changes.
	 *
	 * And an abutment the seat cannot push against is not an abutment. The cap moves
	 * the thrust line to the kern edge, deleting `(1 - k)*|M|` from what the joint
	 * carries, and nothing on the half-seated brick's free body can supply that
	 * except a horizontal pair: a push through the head joint above, and its
	 * reaction as shear in this seat's own bed plane. So the seat has to carry `dM /
	 * z` (z the head joint's height above the bed plane) against `c + mu*sigma_n`,
	 * the same Mohr-Coulomb envelope ComputeUtilisation measures every other sliding
	 * demand against. Where it cannot, the relief is withheld and the joint reads
	 * what it was actually carrying (DESIGN.md §7 gap 4; the ratio `(e - h/6)/z` is a
	 * fact about the bond geometry alone, so a mortared springing affords it three
	 * times over while dry stone cannot afford it at any height).
	 *
	 * A spanned group is checked by being pushed instead, so this gate leaves it
	 * alone: where ReseatSpannedGroups formed an arch, ApplyArchingThrust puts the
	 * real horizontal force on both springings and the ordinary shear axis measures
	 * it — a second, withholding check on the same joint would judge the same thrust
	 * twice by two different rules. The one-cell hole has no thrust pass of its own,
	 * which is why the same mark separates the two cases.
	 *
	 * @param PieceIndex            The loaded piece, which must be placed.
	 * @param BedJoint              Its one seat, which must know its own rectangle: the kern, the
	 *                              centroid and the plane the eccentricity is measured in all
	 *                              come off it.
	 * @param SeatForceUu           What that seat carries, oriented as the joint stores it: the
	 *                              squeeze the sliding capacity is bought with.
	 * @param DeletedCoupleUuCm     The couple the cap would delete, `(1 - k)*|M|`, which is what
	 *                              the thrust has to supply. A magnitude, so the caller's frame
	 *                              never enters.
	 * @param PieceJoints           Every joint touching each piece, in ascending index order.
	 * @param SupportConnections    What holds each piece up, before the reaching-the-ground
	 *                              filter — the relation "counts among its own supports" means.
	 * @param PieceReseatedOnAnArch Which pieces ReseatSpannedGroups re-seated, and therefore lean
	 *                              on their abutments only because a further one is carrying.
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
	 * Whether a piece on two or more compression-only supports overturns because its
	 * centre of mass projects outside the region those supports can push up through —
	 * the router's answer to global overturning, which no per-joint number can
	 * express (each seat reads a comfortable split while the body as a whole has no
	 * admissible equilibrium).
	 *
	 * The tension clause is asked first and is data, not code: any support whose
	 * strength can carry tension holds the lifting side down in withdrawal, so the
	 * body has an equilibrium however far its centre of mass reaches — read off the
	 * same FConnectionStrength the break sweep reads, so a new tension-capable
	 * connection type changes this with no code change. That is what stands the
	 * porch overhang on its Screw-tied cleat.
	 *
	 * The region is the convex hull of the contact rectangles on the bed plane, and
	 * this tests a conservative axis-aligned bound of it — see the definition for why
	 * that is the safe direction.
	 *
	 * @param PieceIndex The loaded piece; its centre of mass is the point projected onto the bed plane.
	 * @param LoadPath   Its supports that reach the ground — the connections whose contact rectangles
	 *                   and tension capability decide the answer.
	 * @return true only when the piece is on compression-only supports and its centre of mass is
	 *         certainly outside their union; false — keep today's behaviour — otherwise, including
	 *         on any missing or non-finite geometry (fail closed: never spuriously overturn).
	 */
	bool PieceOverturnsOffItsSupports(int32 PieceIndex, const TArray<int32>& LoadPath) const;

	/**
	 * How the equilibrium gate disposed of a pass, so SolveAndBreak knows whether the
	 * LP answered authoritatively (and must not run the per-joint capacity sweep) or
	 * declined (and the router sweep is the sole authority for this pass). Below the
	 * block cap the mechanism is the only break authority (PROMOTION_DESIGN.md §12
	 * D7's 3b section, §3.7); the strength sweep is demoted to an estimator that still
	 * populates the utilisation readout but latches nothing.
	 *
	 * DeclinedToRouter is the zero enumerator so a default or fail-closed value routes
	 * to the capacity sweep — the same fail-closed polarity every other refusal here
	 * has.
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
	 * The equilibrium gate — DESIGN.md §7 evolution step 4, PROMOTION_DESIGN.md §6
	 * Slice 3. Below the block cap it is the sole break authority: it asks the
	 * rigid-block LP whether the whole structure has any admissible force system in
	 * equilibrium with self-weight, and on "no" extracts the phase-1 dual (the Farkas
	 * certificate = the kinematic collapse mechanism) and severs exactly the joints
	 * that mechanism opens or slides. Called from SolveAndBreak and nothing else.
	 *
	 * What it catches that no joint check can: ComputeUtilisation happily reports a
	 * confident number for a joint on which no equilibrium solution exists
	 * (DESIGN.md §5.7) — a stack offset far enough per course, or a body on two
	 * bearings both to one side of its centroid, reads a comfortable per-joint
	 * utilisation while its resultant has long since left the bearing. The LP reasons
	 * about the whole admissible force system, so it sees the loss the per-joint
	 * sweep cannot; it equally stands a structure the router only strands for want of
	 * a routing rule (a knot, an opening with no abutment), which is why it also
	 * decides support.
	 *
	 * The pose is feasibility at lambda = 1 (bGravityIsLive = false): the identical
	 * Stands/Falls boolean as lambda* but far cheaper. Scoped by a block cap
	 * (EquilibriumGateBlockCap, D6-c): above it the gate declines and behaviour falls
	 * through to the per-joint capacity sweep, exactly as production did before the
	 * gate. A no-op without complete geometry and on any LP refusal (fail closed).
	 *
	 * On Stands or Falls it rewrites the support arrays from the LP
	 * (PieceSupported/PieceStranded) so GetPieceSupport is LP-authoritative below the
	 * cap, overriding the router's Stranded verdict. On Falls it severs the
	 * mechanism's joints (mapped to production connections through
	 * ConnectionOfJoint), latched and stamped with this pass.
	 *
	 * @param Pass The cascade pass any break is stamped with.
	 * @return How the gate disposed of this pass (see EEquilibriumGateDisposition).
	 */
	EEquilibriumGateDisposition BreakByEquilibrium(int32 Pass);

	/**
	 * The regional collapse prover (REGIONAL_PROVER_PLAN.md §§1-4) — factored out of
	 * SolveAndBreak_WithRegionalProver so both the isolated test entry and the real
	 * cascade drive the identical machinery. It floods a region from Seed by
	 * joint-hops over PieceJoints up to RegionBlockCap, pins the one-hop frontier
	 * ring grounded, poses R ∪ boundary at feasibility through the grounded-boundary
	 * bridge overload RigidBlockOracle::BuildRegionalProblem, calls SolveRigidBlock,
	 * and on a certified Falls marks the moved interior pieces Falling (never
	 * Supported — one-directional, toward collapse only) and severs the intact
	 * joints the mechanism opens, stamping each with BreakPass.
	 *
	 * It does not solve loads — the caller has already settled the graph, so
	 * re-solving here would waste work and wipe the support state the stitch
	 * overrides. A bridge refusal or a non-Falls outcome releases nothing.
	 *
	 * @param Seed           The pieces the region grows from (the disturbance neighbourhood).
	 * @param RegionBlockCap The largest region the flood may reach (the grounded boundary rings it).
	 * @param BreakPass      The cascade pass any severed joint is stamped with (default 1, the value
	 *                       the isolated slice-1/2/3 entry used, so those tests are byte-for-byte).
	 * @return the number of pieces the prover moved and marked Falling this call.
	 */
	int32 ProveRegionalCollapse(const TArray<int32>& Seed, int32 RegionBlockCap, int32 BreakPass = 1);

	/**
	 * Rewrite the per-piece support arrays from the LP verdict, so GetPieceSupport is
	 * LP-authoritative below the cap (PROMOTION_DESIGN.md §12 D7's 3b section). Every
	 * piece the bridge included reads Supported/Grounded if the LP carries it and
	 * Falling if the mechanism moves it; the router's Stranded verdict is overridden
	 * for the pieces the LP stands. Called only from BreakByEquilibrium, on an
	 * answered (Stands or Falls) solve. See its definition.
	 */
	void ApplyLimitAnalysisSupport(
		const RigidBlockOracle::FOracleProblem& Problem,
		const RigidBlockOracle::FOracleResult& Result);

	/**
	 * Solve and cache the min-violation strain readout, keyed back to production
	 * connections through the bridge's ConnectionOfJoint provenance
	 * (PROMOTION_DESIGN.md §3.5, SHED_PATH.md Phase A 6b). Called only from
	 * BreakByEquilibrium on an answered below-cap solve, after the break authority has
	 * run: it poses a separate FOracleProblem with bMinViolationReadout set, so it
	 * changes no break decision, support flag or ConnectionForces — the readout is
	 * purely additive. See its definition.
	 */
	void CacheMinViolationReadout(const RigidBlockOracle::FOracleProblem& Problem);

	/**
	 * The per-joint capacity sweep — the router's break authority, one sweep per
	 * pass, stamping every joint over its own capacity with this pass. The sole break
	 * authority above the block cap and on any LP refusal; below the cap the
	 * equilibrium gate answers instead and this does not run. Returns whether any
	 * joint gave. Called only from SolveAndBreak.
	 */
	bool BreakByCapacitySweep(int32 Pass);

	/**
	 * The equilibrium gate's block cap — the fail-closed boundary keeping synchronous
	 * LP authority off the flagship scenarios (D6-c). At or below it the gate is
	 * authoritative; above it it declines to the router. Set so the medium
	 * wall-catalogue fixtures (125-174 blocks) are in scope for the LP to stand or
	 * fell, while the 375-block flagship wall and the giant corbels stay above it.
	 * Exceeds the measured ~84-104-block per-action latency band, so a structure of a
	 * few hundred blocks now solves the LP synchronously; no production structure
	 * sits in that range today (the default wall is ~1200 blocks), so the cost is
	 * test-time only for now — CURRENT_STATE carries the open latency item.
	 */
	int32 EquilibriumGateBlockCap = 200;

	/*
	 * The regional prover's block cap — the largest |region ∪ grounded boundary| the
	 * cascade's above-cap decline arm lets ProveRegionalCollapse pose
	 * (REGIONAL_PROVER_PLAN.md §4).
	 *
	 * Deliberately modest, not the equilibrium gate's 200: the prover poses a
	 * region-cap-sized LP on every above-cap pass, so the cap is the per-pass solve
	 * cost — 200 blocks per pass makes a flagship 3D collapse (many passes) take tens
	 * of minutes. Over-holds are local, so a small region catches them cheaply and
	 * the grounded boundary keeps every verdict sound regardless of where the cut
	 * falls. 200 stays reachable through SetRegionBlockCap as a ceiling for tests.
	 * The proper fix for cheap large reach is grow-on-contact, deferred as
	 * REGIONAL_PROVER_PLAN.md slice 3.
	 *
	 * Named distinctly from the RegionBlockCap parameter SolveAndBreak_WithRegionalProver
	 * and ProveRegionalCollapse take, so the -Werror shadow (C4458) never fires: the
	 * isolated test entry drives the cap through its parameter, the real cascade reads
	 * this member.
	 */
	int32 RegionalProverBlockCap = 200;

	/*
	 * Instrumentation for GetLastRegionalProblemBlockCount — the number of oracle
	 * blocks the last SolveAndBreak_WithRegionalProver posed to BuildRegionalProblem,
	 * i.e. |region ∪ grounded boundary|. Stamped the moment the pose is built;
	 * INDEX_NONE until a regional prove has posed a problem.
	 */
	int32 LastRegionalProblemBlockCount = INDEX_NONE;

	/*
	 * Instrumentation for GetLastEquilibriumProblemDim — 2 or 3 for the dimension the
	 * last equilibrium-gate pose was built in, INDEX_NONE before any. Stamped in
	 * BreakByEquilibrium from Problem.Dim the moment BuildRigidBlockProblem accepts.
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
	 * The per-pose decomposition of the aggregates above — reset at the top of each
	 * ProveRegionalCollapse call, one entry appended per prover pose, so its Num()
	 * equals LastProverPoses and its Blocks/LpPivots/LpMs sum to the aggregates above.
	 * GetLastSolveAndBreakReport surfaces it as FBreakPassReport::RegionalPoseBreakdown;
	 * observability only, never authority.
	 */
	TArray<FProverPoseReport> LastProverPoseBreakdown;

	/*
	 * Whether SetThreeDimensional flagged this structure 3D (THREED_DESIGN.md E3).
	 * False by default so every existing structure is 2D and bridges exactly as
	 * before. The bridge reads it (E3): true routes to the Dim3D pose and lifts the
	 * Y-normal refusal. See SetThreeDimensional.
	 */
	bool bThreeDimensional = false;

	TArray<FStructurePiece> Pieces;
	TArray<FConnection> Connections;

	/**
	 * Solver output, parallel to the arrays above and rebuilt by every solve.
	 *
	 * Kept beside the pieces rather than inside FStructurePiece and FConnection so
	 * that a piece stays a mass and an identity, and a joint stays an interface and
	 * a strength — neither carries a cached answer that could be read before a
	 * solve has produced it.
	 */

	/** Whether each piece reaches the earth through supports. */
	TArray<bool> PieceSupported;

	/**
	 * Whether each piece is in an unroutable knot — not merely resting on one.
	 *
	 * The fixpoint in SolveLoads has always computed this, kept rather than discarded
	 * so GetPieceSupport can say why a piece is not held up. Both accessors read
	 * these two arrays and neither recomputes anything, which stops the reason
	 * drifting from the answer it explains.
	 *
	 * A piece is never both stranded and supported: the reachability walk neither
	 * marks a stranded piece nor crosses one, and the pass that strands is the pass
	 * after which nothing is supported through it.
	 */
	TArray<bool> PieceStranded;

	/** What each connection carries, in Unreal force units. */
	TArray<FVector> ConnectionForces;

	/**
	 * The bending moment each connection carries about its own centroid, uu.cm.
	 *
	 * Beside ConnectionForces and with its lifetime, not folded into it: both are
	 * rebuilt from scratch by every solve and self-heal the same way — a moment that
	 * survived a re-solve would describe a structure that no longer exists.
	 * Separate rather than encoded, since folding it into the force vector (as extra
	 * length or a tilt) would make GetConnectionForce report a number that no longer
	 * describes the load.
	 *
	 * Non-zero only where the statics is determinate: a piece whose load reaches the
	 * ground through exactly one joint, and which somebody actually placed. See
	 * SolveLoads for why the per-joint rule that looks obvious is wrong.
	 */
	TArray<FVector> ConnectionMoments;

	/**
	 * How deep the bonded masonry standing over each connection is, cm.
	 *
	 * The third parallel array, with the lifetime of the other two for the same
	 * reason: a depth that survived a re-solve would describe a wall since cut.
	 *
	 * Beside the joints rather than on them, deliberately, because it is an answer,
	 * not an interface: a joint stays a face and a strength, and storing this on
	 * FConnection would let it be read out of GetConnection before any solve
	 * produced it.
	 *
	 * Non-zero only where ConnectionMoments is, and on bed joints alone. See
	 * GetConnectionCompositeDepthCm.
	 */
	TArray<double> ConnectionCompositeDepthCm;

	/**
	 * Which breaking pass gave each connection, counted from 1, or INDEX_NONE.
	 *
	 * Grown by AddConnection rather than sized by a solve, and never cleared: joints
	 * never heal, so a stamp once written is history and re-running a cascade must not
	 * rewrite it. It is the only record of the order a collapse happened in, which is
	 * what phase 5's visualisation plays back — the latch on FConnection says only
	 * whether a joint gave, never when.
	 */
	TArray<int32> ConnectionBreakPass;

	/**
	 * Which authority gave each connection, parallel to ConnectionBreakPass and grown
	 * beside it in AddConnection so the two stay index-aligned by construction.
	 * Written once at the sever site by whichever authority ruled (gate, sweep or
	 * prover); INDEX_NONE until then. Pure observability — GetBreakAuthority reads
	 * it, and nothing in the solve consults it.
	 */
	TArray<int32> ConnectionBreakAuthority;

	/**
	 * The cached min-violation strain readout, one entry per connection, read by
	 * GetConnectionReadout (PROMOTION_DESIGN.md §3.5, SHED_PATH.md Phase A 6b). Filled
	 * solve-on-settle by BreakByEquilibrium below the block cap through
	 * CacheMinViolationReadout, and cleared by SolveAndBreak so a re-solve cannot
	 * return a stale readout. Above the cap the gate declines and nothing fills it, so
	 * every handle reads absent and the overlay falls back to the router.
	 *
	 * Solver output like the arrays above, but from a different solve than the
	 * verdict: the readout is the min-violation force distribution, not the
	 * maximise-lambda break authority, so it is kept apart from ConnectionForces and
	 * never feeds a break decision.
	 */
	TArray<FConnectionReadout> ConnectionReadoutCache;

	/**
	 * Observability counter (test scaffolding) — the number of min-violation readout
	 * LPs CacheMinViolationReadout has run since the last SolveAndBreak began. Reset
	 * once at the top of SolveAndBreak (not per pass, so it accumulates across a
	 * cascade) and bumped once per call. Nothing reads it but
	 * GetMinViolationReadoutSolveCount; it drives no behaviour.
	 */
	int32 MinViolationReadoutSolves = 0;

	/**
	 * How many times SolveLoads has been entered, ever. See NumSolves.
	 *
	 * Not solver output, so it sits below the arrays rather than among them: every
	 * one of those is rebuilt by a solve, and this is the only field a solve
	 * accumulates. Nothing clears it and nothing branches on it.
	 */
	int32 SolveCount = 0;
};
