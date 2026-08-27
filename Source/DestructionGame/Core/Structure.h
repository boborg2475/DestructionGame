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
	 * Where this piece's weight acts, cm. The ONE position a piece carries.
	 *
	 * Not a transform and not a bounding box: the only thing the solver can do with a
	 * position is measure how far the load path misses a joint's centroid by, and a
	 * point is the whole of what that needs.
	 *
	 * Zero is not "at the origin", it is "nobody said" — see bHasCentreOfMass.
	 */
	FVector CentreOfMassCm = FVector::ZeroVector;

	/**
	 * Whether anyone supplied that centre of mass.
	 *
	 * FALSE BY DEFAULT, so a piece nobody placed carries no eccentricity rather than
	 * claiming to sit at the world origin — which for a wall laid off the origin would
	 * be a lever arm of metres, invented out of a defaulted field. A piece with no
	 * centre of mass loads its joints exactly as it did before moments existed.
	 */
	bool bHasCentreOfMass = false;

	/**
	 * Whether this piece rests on the earth.
	 *
	 * A grounded piece TERMINATES the flow of load: whatever reaches it is
	 * absorbed by the ground rather than passed on to another connection.
	 */
	bool bIsGrounded = false;

	/**
	 * Whether this slot still holds a piece, which is what makes a handle stable.
	 *
	 * A removed piece is TOMBSTONED: the slot stays, the handle stays valid forever,
	 * and nothing ever moves down to fill the gap. Handles are array indices and three
	 * arrays hang off them, so compacting would re-point every connection above the
	 * hole at the wrong piece — silently, since ConnectionBreakPass cannot be rebuilt
	 * from anything and would be wrong for good.
	 *
	 * DO NOT PUT A FREE LIST ON TOP OF THIS. Reusing a slot without a generation
	 * counter brings the same bug back in a worse form: a stale handle then names a
	 * DIFFERENT LIVE piece, which nothing can detect, where a dangling index can at
	 * least be range-checked. If slots ever have to be reused — persistent worlds,
	 * accumulating debris, anything where the piece count is not bounded per scenario
	 * — that needs generational handles (index plus a counter bumped on reuse), not a
	 * free list. Scenarios are bounded today, so the leak is irrelevant.
	 *
	 * FALSE BY DEFAULT so that the placeholder GetPiece hands back for an unknown
	 * handle reads as dead, matching every other accessor's fail-closed answer. A
	 * piece that has not been added to a structure is not in one.
	 */
	bool bIsInTheStructure = false;
};

/**
 * WHY a piece is or is not being held up, as opposed to merely whether it is.
 *
 * IsPieceSupported is the composite answer and stays exactly as it is; this is the
 * reason behind it. Two completely different things produce "this piece is falling",
 * and nothing outside the solver can currently tell them apart:
 *
 *   - real physics — nothing is holding it up, its supports are gone or falling;
 *   - a solver limitation — the piece is in an unroutable knot, which is reported
 *     conservatively rather than solved (DESIGN.md §3, and see IsPieceSupported).
 *
 * That distinction is what lets a collapse test claim a wall fell from LOAD rather
 * than from the solver giving up on a loop: assert no piece was Stranded when it went.
 *
 * FALLING IS FIRST SO THAT ZERO IS THE FAIL-CLOSED ANSWER. A default-constructed or
 * zero-filled entry then reads "nothing is holding this up", which is the same
 * direction every other accessor on FStructure fails in. Grounded at zero would make
 * an uninitialised array claim the whole structure was resting on the earth.
 */
enum class EPieceSupport : uint8
{
	/**
	 * Nothing is holding this piece up, and the solver is not the reason.
	 *
	 * Also the answer for a handle that names no piece, for a piece that has been
	 * removed, for a structure that has not been solved yet, and for a piece resting
	 * ONLY on a knot — that piece is not itself in the knot, it has simply lost the
	 * one thing that was carrying it, which is ordinary physics.
	 */
	Falling,

	/** Resting on the earth. Ground terminates the flow of load; nothing carries this. */
	Grounded,

	/** Reaches the earth through supports, and the solver could route its load there. */
	Supported,

	/**
	 * IN an unroutable knot: this piece is ultimately one of its own supports.
	 *
	 * NOT physics, and that is the whole point of telling it apart from Falling. The
	 * piece reaches the ground through the support relation, but dividing load round a
	 * loop needs a rule that does not exist yet, so it is reported unsupported —
	 * conservatively, deliberately, and now visibly.
	 */
	Stranded,
};

/**
 * What a connection is TO ONE OF ITS PIECES — the two-tier classification, and the
 * relation is DIRECTED.
 *
 * The same joint is a bed joint beneath the piece above it and a bed joint above the
 * piece below it, so a role only means anything once you say whose. DESIGN.md §3's
 * tiering is the whole of it: a substantially vertical interface normal bears in
 * compression, a substantially horizontal one can only carry in shear, and the line
 * sits at 45 degrees.
 *
 * NONE IS THE ZERO ENUMERATOR, so a zero-filled entry claims no tier at all rather
 * than claiming the strongest one — the same direction EPieceSupport::Falling fails in.
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
 * THE LOAD MODEL.
 *
 * Load flows downward. A piece transmits its own weight, plus everything it
 * receives from the pieces above it, into whatever connections support it. A
 * grounded piece terminates that flow. Where a piece is supported by more than
 * one connection the load splits weighted by interface area, so equal areas split
 * evenly and the degenerate cases stay sensible.
 *
 * SUPPORT IS TWO-TIERED, and it is the joint's own orientation that decides which
 * tier (DESIGN.md §3). A substantially vertical interface normal is a BED JOINT,
 * which can bear weight in compression; a substantially horizontal one is a HEAD
 * JOINT, which can only carry it in shear. So a piece's supports are the bed
 * joints BENEATH it, and only a piece with none of those falls back to its head
 * joints. A bed joint above a piece bears nothing — that is something resting on
 * it, not something holding it up.
 *
 * A JOINT THAT HAS GIVEN IS NOT IN THE SUPPORT RELATION AT ALL. It leaves the tier
 * decision as well as the load path, so a piece whose only bed joint has gone falls
 * back to its head joints rather than being left with no supports and reported
 * falling. That is what redistribution IS: the share moves onto the neighbours
 * instead of evaporating.
 *
 * A REMOVED PIECE IS NOT IN THE GRAPH AT ALL, and that is the same statement one
 * level up. Every joint that held it goes with it, so it supports nothing and
 * nothing supports it; and if it was grounded it has stopped resting on the earth,
 * so it conducts the ground to nothing either. Removal is the player's move — pull
 * a brick and what it was carrying has to go somewhere — and it differs from a
 * joint failing in one respect that is recorded rather than incidental: nothing was
 * overloaded, so nothing enters the collapse sequence. See GetBreakPass.
 *
 * Everything else is then computed over that SUPPORT relation rather than over raw
 * connectivity, which is the whole point: routing by graph distance to the ground
 * let a short sideways path exclude a bed joint entirely, so a brick spanning a gap
 * bore none of the wall stacked on it. Classification was direction-aware from the
 * start; routing was blind, and routing decides where the load ends up.
 *
 * A SUPPORT THAT IS ITSELF FALLING IS NOT A SUPPORT, so the split only ever uses
 * supports with their own path to the earth. A share handed to a piece that never
 * reaches the ground stops there — the load is lost, and the joint genuinely
 * carrying it under-reports by exactly that fraction.
 *
 * "Supported" therefore means REACHING THE GROUND THROUGH SUPPORTS, not through
 * connections, and not about world geometry — pieces have no position here. Being
 * joined to a neighbour is not support, and two pieces can each hang from the other
 * and neither be held up by anything. A piece with no path to a grounded piece is
 * unsupported, and that is what "the wall fell" will eventually mean. It also makes
 * DESIGN.md's worked example fall out rather than being special-cased: a piece with
 * no bed joint beneath it pushes its whole weight through its vertical head joints,
 * and FConnection resolves that same downward force as shear rather than
 * compression.
 *
 * UNITS. Unreal's gravity is -980 cm/s2 and mass is in kilograms, so a piece's
 * weight in Unreal force units is simply MassKg * 980 — a 2.72 kg brick weighs
 * 2666. That already IS the 1 N = 100 uu conversion (2.72 kg x 9.81 = 26.7 N,
 * x 100 = 2670); applying a factor of 100 a second time is the standard way to be
 * wrong by exactly 100x here.
 *
 * SCOPE. SolveLoads computes loads and does not break anything: solving must leave
 * every connection exactly as intact as it found it, so a solve can be re-run.
 * Breaking is a separate, deliberate step — SolveAndBreak — and keeping the two
 * apart is what lets anything ask what a structure carries without damaging it.
 *
 * A plain struct with no UObject and no world, because the whole point of owning
 * the load maths ourselves (DESIGN.md §3) is that it stays plain arithmetic over
 * a graph and therefore fast and deterministic to test.
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
	 * THE SAME DOOR, WITH ONE MORE FACT THROUGH IT. Every rejection the two-argument
	 * form makes it makes too; supplying a centre of mass buys a piece the ability to
	 * load a joint eccentrically and nothing else.
	 *
	 * A centre that is not finite is refused outright rather than stored and ignored: a
	 * NaN there launders into a NaN lever arm the moment anything subtracts a joint
	 * centroid from it, and the same argument AddConnection already makes for a joint's
	 * own centre applies unchanged one level up.
	 */
	int32 AddPiece(double MassKg, bool bIsGrounded, const FVector& CentreOfMassCm);

	/**
	 * Add a connection and return its handle, or INDEX_NONE if it is not a joint.
	 *
	 * The structure owns the graph and is the only place that can tell a valid
	 * piece handle from a nonsense one, so validation lives here. Rejected:
	 * handles outside the piece array (INDEX_NONE included), a connection from a
	 * piece to itself, an interface area that is not finite and positive, and a
	 * normal that describes no plane.
	 *
	 * Area in particular has to fail closed at construction rather than at solve
	 * time: the load split divides by the total supporting area, and a zero or
	 * NaN area leaves no sensible answer to divide by.
	 *
	 * AND THE JOINT'S OWN RECTANGLE, when one is supplied at all. A rectangle that does
	 * not describe the face its area describes is a plausible number with the wrong lever
	 * arm, which is the same class of fault as a normal inconsistent with its A/B pairing
	 * — Layout.h makes that one inexpressible by emitting the two together, and this door
	 * is the other half of it. Rejected: a rectangle whose 4 x h_u x h_v disagrees with
	 * the area, one with an extent on the normal's own axis (a box, not a face), a
	 * negative or non-finite half-extent, a non-finite centre, and any rectangle at all on
	 * a normal that is not axis-aligned, since the in-plane frame is "the two world axes
	 * that are not the separation axis" and a tilted normal names no such pair.
	 *
	 * ZERO EXTENTS ARE NOT A DEGENERATE JOINT, and none of that applies to them. They mean
	 * no bending capacity was ever measured, which is healthy — the area alone answers a
	 * centred load exactly — so a joint supplying no rectangle is exactly as valid as it
	 * ever was, on a tilted normal included.
	 *
	 * VALIDATES, NEVER NORMALISES. An accepted rectangle is stored bit for bit as given;
	 * quietly zeroing one that could not be verified would turn a refusal into a joint
	 * that reads as healthy.
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
	 * The valid handle RANGE, which never shrinks — NOT the number of pieces still in
	 * the structure.
	 *
	 * Callers iterate 0..NumPieces() and resolve each handle, so this has to keep
	 * meaning the array extent: made to return a live count it would silently skip real
	 * pieces in every such loop. Ask IsPieceRemoved whether a slot still holds anything,
	 * and NumLivePieces for a count.
	 */
	int32 NumPieces() const;

	/** How many pieces have not been removed. For counts, never for iteration. */
	int32 NumLivePieces() const;

	int32 NumConnections() const;

	/**
	 * How many times this structure has been solved, ever.
	 *
	 * THE ONLY OBSERVABLE A COST CLAIM CAN BE MADE AGAINST. Solving is deterministic and
	 * non-destructive, so solving twice is invisible in every other reading of the graph —
	 * which means "the batched commit solves ONCE rather than once per piece" is unfalsifiable
	 * without a counter, and an unfalsifiable claim in a comment is not a claim. A full solve
	 * is tens of milliseconds at scenario scale, so the difference between one and ten is the
	 * difference between a click and a stutter.
	 *
	 * IT COUNTS SOLVES, NOT CHANGES. SolveAndBreak runs several, and it is meant to; nothing
	 * here is a budget or a limit, and no production code branches on it.
	 */
	int32 NumSolves() const;

	/**
	 * Whether every piece and every joint still in this structure knows where it is.
	 *
	 * "NOBODY SUPPLIED POSITIONS, SO THERE ARE NO MOMENTS" HAS TO BE ASKABLE, and this is
	 * the only thing that can ask it. A load path with no eccentricity and a load path
	 * nobody measured produce the identical answer — that exactness is what lets every
	 * geometry-free fixture in the project go on working — so the two are otherwise
	 * indistinguishable from outside, and a readout that showed a moment of zero could not
	 * say which of them it was looking at. Same trick as HasSupportAnswer, one level up.
	 *
	 * A CONJUNCTION OF THE TWO HALVES, and neither half is a judgement call. A moment needs
	 * a point for the load to act at and a rectangle for the joint to resist it with; either
	 * one missing anywhere means some joint in this structure is answering a centred load
	 * because it has to, not because the load is centred.
	 *
	 * OVER WHAT IS STILL IN THE STRUCTURE, not over the arrays. A removed piece and a joint
	 * that has given are out of the graph entirely — they carry nothing and route nothing —
	 * so a tombstone left behind by a piece nobody ever placed must not condemn a structure
	 * whose live half is fully described.
	 *
	 * TRUE FOR AN EMPTY STRUCTURE, deliberately: an empty conjunction is true, and that is
	 * what keeps this composable — adding a fully-described piece to a complete structure
	 * leaves it complete, and there is no first-piece special case. The state that would
	 * actually mislead someone is a structure with pieces and no positions, and that reads
	 * false.
	 */
	bool HasCompleteGeometry() const;

	/** Out-of-range handles return a default-constructed placeholder. */
	const FStructurePiece& GetPiece(int32 PieceIndex) const;
	const FConnection& GetConnection(int32 ConnectionIndex) const;

	/**
	 * Recompute what every connection carries and which pieces reach the ground.
	 *
	 * Joints that have already given are skipped entirely: they are out of the
	 * structure, so they neither bear load nor conduct support to anything.
	 *
	 * Non-destructive: no connection may give as a result of solving, however
	 * overloaded it is.
	 */
	void SolveLoads();

	/**
	 * Solve, break every joint that is over capacity, re-solve so their load moves onto
	 * the neighbours, and repeat until nothing more gives.
	 *
	 * EVERY JOINT OVER CAPACITY GIVES IN THE SAME PASS, and each is stamped with the
	 * pass number that broke it. Ordering WITHIN a pass is arbitrary and nothing may
	 * depend on it; ordering BETWEEN passes is real, and is the sequence a collapse is
	 * played back in.
	 *
	 * Terminates because joints never heal: a pass that breaks nothing is the last one,
	 * so there can be no more passes than there are connections.
	 *
	 * A PASS BREAKS ON TWO GROUNDS, NOT ONE. Beside the per-joint capacity sweep, the
	 * equilibrium gate (BreakByEquilibrium — DESIGN.md §7 step 4) asks the rigid-block LP
	 * whether the whole structure has any admissible equilibrium with self-weight, and on
	 * "no" gives the bearings of the body that has lost the earth, in the same pass and with
	 * the same stamp. It is scoped by a block cap; see its own contract.
	 *
	 * PASS NUMBERS ARE GLOBAL TO THE STRUCTURE, NOT TO THE CALL. A second cascade
	 * continues from the highest stamp already written, so a joint that gives after a
	 * piece has been removed carries a strictly larger number than everything that gave
	 * before it. Nothing earlier is ever rewritten. The stamps are the sequence a
	 * collapse is played back in, and consumers read a shared number as "these gave
	 * simultaneously" — which is only true if numbering does not restart.
	 *
	 * @return the number of passes THIS CALL broke at least one joint in; zero for a
	 *         structure that stands as built. Per-call, unlike the stamps: it is what a
	 *         caller polls to find out whether its removal did anything, so it can be
	 *         smaller than the pass numbers that call wrote.
	 */
	int32 SolveAndBreak();

	/**
	 * COMPILE STUB FOR SLICE 2 (PROMOTION_DESIGN.md §12 D6-c) — the injectable block cap that
	 * scopes the equilibrium gate's authority by structure size. Written by test-expert to let
	 * the scope-by-size red compile; it stores the value and NOTHING reads it yet. dev-expert
	 * (Slice 2) makes the gate read it: authoritative when the block count is <= this cap, and
	 * fail-closed to the router (no overturning check) above it. The production default is the
	 * conservative end of the measured ~84-104-block band; a test sets a low cap to drive the
	 * decline arm without building an over-cap fixture.
	 */
	void SetEquilibriumGateBlockCap(int32 MaxBlocks);

	/**
	 * Which breaking pass gave this joint, counted from 1, or INDEX_NONE if no pass did
	 * — including for an out-of-range handle, which is not a joint that broke.
	 *
	 * INDEX_NONE DOES NOT MEAN "STILL INTACT", and reading it that way is a trap this
	 * contract used to set. It means "did not fail under load in a cascade pass", which
	 * is also true of a joint that went because a piece it held was REMOVED: that joint
	 * never snapped, it was deleted, and phase 5 replays these stamps as the sequence of
	 * a collapse, so it must not appear in that sequence at all. Whether a joint is still
	 * in the structure is a different question, and HasGiven is the accessor for it.
	 *
	 * The two together are a complete, unambiguous encoding, and there is no sentinel:
	 *
	 *     intact              HasGiven false, INDEX_NONE
	 *     went with a piece   HasGiven true,  INDEX_NONE
	 *     broke in pass N     HasGiven true,  N >= 1
	 */
	int32 GetBreakPass(int32 ConnectionIndex) const;

	/**
	 * What this connection is to this piece: the solver's own two-tier decision, exposed.
	 *
	 * THE SAME DECISION SolveLoads ROUTES BY, NOT A SECOND ONE. It is the single most
	 * load-bearing classification in the solver — a joint in the wrong tier gives a wall
	 * that stands there being wrong — so a readout that re-derived it would be a copy that
	 * can disagree with the routing it claims to explain. Same reason
	 * GetConnectionUtilisation is one line delegating to FConnection::UtilisationUnder.
	 *
	 * PURE GEOMETRY, AND IT NEEDS NO SOLVE. It reads the joint's normal and which end the
	 * piece is, so it answers before anything has been solved and it keeps answering for a
	 * joint that has GIVEN — a given joint leaves the support relation, but what it used to
	 * be is exactly what a debugger is looking at.
	 *
	 * None for a handle naming no connection, for a piece that is not on this connection,
	 * and for a normal that will not normalise.
	 */
	EJointRole GetJointRole(int32 ConnectionIndex, int32 PieceIndex) const;

	/**
	 * The force this connection carries, in Unreal force units, after SolveLoads.
	 *
	 * A vertical vector of the accumulated magnitude — gravity does not change
	 * direction because a joint is vertical. FConnection::ApplyForce is what
	 * resolves it against the joint's own normal, which is why the same vector is
	 * compression on a bed joint and shear on a head joint.
	 *
	 * EXCEPT AT THE SPRINGING OF AN ARCH, AND THAT IS THE ONE EXCEPTION. An opening
	 * spanned by the group rule pushes SIDEWAYS as well as down, equally and oppositely
	 * at its two ends, and that thrust is delivered into the abutment's own bed joint —
	 * so a springing's force is (H, 0, -V) rather than (0, 0, -V). It is still one
	 * force through one joint and it still resolves the same way: a horizontal component
	 * on a bed joint is shear by ClassifyForce's own definition, which is why the arch
	 * needs no new axis and no new strength. ARCHING_DESIGN.md.
	 *
	 * A PLAYER LOOKING AT A NON-VERTICAL FORCE ON A SPRINGING IS SEEING THE ARCH, and it
	 * needs saying in the readout: the joint is being pushed sideways by the masonry
	 * bridging the hole beside it, and the number that decides whether it holds is
	 * Mohr-Coulomb friction bought by the weight standing on the same patch.
	 *
	 * ITS SIGN DEPENDS ON WHICH END IS BEING HELD UP. Per ConnectionLoad.h the force
	 * belonging to a connection is the force acting on PieceB, so a joint that names
	 * the loaded piece second carries it downward and one that names it first
	 * carries the equal-and-opposite reaction, upward. The classification is the same
	 * either way — that invariance is what makes per-piece bookkeeping safe — but a
	 * force stored against the wrong end turns compression into tension.
	 *
	 * Zero for an out-of-range handle, and zero for a connection in a part of the
	 * structure that has no path to ground: nothing there is being held up, so
	 * there is no static load path to report.
	 */
	FVector GetConnectionForce(int32 ConnectionIndex) const;

	/**
	 * The bending moment this connection carries about its own centroid, uu.cm, after
	 * SolveLoads.
	 *
	 * ONE OF THE THREE THINGS GetConnectionUtilisation IS COMPUTED FROM, and it exists
	 * because without it that accessor cannot state its own contract. The identity a strain
	 * readout rests on is
	 *
	 *     GetConnectionUtilisation(I)
	 *         == GetConnection(I).UtilisationUnder(
	 *                GetConnectionForce(I), GetConnectionMoment(I),
	 *                GetConnectionCompositeDepthCm(I))
	 *
	 * and the moment parameter is DEFAULTED, so an assertion written without this accessor
	 * silently supplies zero and holds however far the two have drifted. A readout that showed
	 * a joint at 0.397 beside a force of 2667 uu with no arithmetic connecting them is this
	 * subsystem's recurring signature; the number that closes the gap has to be gettable.
	 *
	 * SEPARATE FROM THE FORCE, NEVER ENCODED IN IT. A moment folded into GetConnectionForce as
	 * extra length or as a tilt would make the one vector a readout uses to explain the load
	 * stop describing it.
	 *
	 * ZERO IS "NO ECCENTRICITY", AND IT IS NOT A TOLERANCE. It is what a centred load, a piece
	 * nobody placed and a joint whose rectangle nobody measured all produce, exactly — which is
	 * what lets every geometry-free fixture in the project go on reading what it always read.
	 * HasCompleteGeometry is what tells those apart; this accessor does not try to.
	 *
	 * Zero for an out-of-range handle and zero before anything has been solved, the same scope
	 * GetConnectionForce documents, because it is the same solver output rebuilt by the same
	 * solve.
	 */
	FVector GetConnectionMoment(int32 ConnectionIndex) const;

	/**
	 * How deep the bonded masonry standing over this connection is, cm, after SolveLoads.
	 *
	 * THE THIRD THING GetConnectionUtilisation IS COMPUTED FROM, and it is here for exactly the
	 * reason GetConnectionMoment is: without it that accessor cannot state its own contract.
	 * The identity a strain readout rests on is
	 *
	 *     GetConnectionUtilisation(I)
	 *         == GetConnection(I).UtilisationUnder(
	 *                GetConnectionForce(I), GetConnectionMoment(I),
	 *                GetConnectionCompositeDepthCm(I))
	 *
	 * and both trailing parameters are DEFAULTED, so an assertion written without this accessor
	 * silently supplies zero and holds however far the two have drifted.
	 *
	 * A LENGTH, NOT A SECTION, AND THAT IS THE SEAM. What the solver knows is how much wall is
	 * standing on the plane; which of the joint's in-plane extents that depth pairs with is the
	 * joint's own business. Handing out a modulus would put the pairing on this side of the
	 * seam, where it becomes a second copy of the one thing that is silent when swapped.
	 *
	 * NON-ZERO ONLY WHERE A JOINT IS ACTUALLY BENT: a BED joint beneath a placed piece whose
	 * load reaches the ground through it alone, carrying a moment that is not exactly zero.
	 * Everywhere else this is zero and the joint reads its own bed patch, bit for bit as it did
	 * before composite action existed — which is what keeps every geometry-free fixture, both
	 * fuzzes included, reading exactly what it always read.
	 *
	 * ZERO IS "NO MASONRY WAS MEASURED", and it is the fail-closed value rather than a
	 * tolerance: the relief is withheld and the joint reads as more heavily loaded, which is
	 * the safe direction for a quantity whose whole job is to make a joint read LESS.
	 *
	 * Zero for an out-of-range handle and zero before anything has been solved, the same scope
	 * GetConnectionForce documents, because it is the same solver output rebuilt by the same
	 * solve.
	 */
	double GetConnectionCompositeDepthCm(int32 ConnectionIndex) const;

	/**
	 * How close this connection is to failing under the load the last solve gave it.
	 *
	 * The strain readout's question, and it must be answerable without damaging
	 * anything: FConnection::UtilisationUnder is the non-mutating evaluator, and this
	 * is that evaluator applied to GetConnectionForce. Nothing here re-derives the
	 * break decision — a third hand-copy of it in production is exactly what this
	 * accessor exists to prevent.
	 *
	 * ZERO FOR A JOINT THAT HAS GIVEN, and the reason matters: a given joint carries
	 * no force, so there is genuinely nothing on it. The latch is NOT what produces
	 * that zero, and a caller must not read a low ratio as "intact" — HasGiven is the
	 * authoritative state, and GetBreakPass says which pass wrote it.
	 *
	 * FAILS CLOSED FOR A HANDLE THAT NAMES NO JOINT, returning
	 * TNumericLimits<double>::Max() rather than zero. Zero would read as "unloaded and
	 * perfectly healthy", which is the one answer that must never come back for
	 * something that is not a joint — the same reason ComputeUtilisation answers a
	 * zero interface area that way, and the same hole DESIGN.md §2's caller obligation
	 * describes for a degenerate normal.
	 *
	 * Zero before anything has been solved, because no load has been routed yet. Same
	 * scope as GetConnectionForce, which it reads.
	 */
	double GetConnectionUtilisation(int32 ConnectionIndex) const;

	/**
	 * Whether this piece has a path to a grounded piece through SUPPORTS, after
	 * SolveLoads. Grounded pieces are supported by definition.
	 *
	 * Through supports, not through connections: a piece glued underneath a grounded
	 * slab is joined to the earth and is not held up by it.
	 *
	 * AND A PATH THE SOLVER COULD ACTUALLY ROUTE. Pieces in a cycle of the support
	 * relation — a course of bricks spanning a two-brick gap, each falling back to
	 * its neighbours' head joints — cannot be put in an accumulation order, so their
	 * load never reaches the earth. Those are reported unsupported, which keeps this
	 * answer and GetConnectionForce telling one story rather than contradicting each
	 * other in silence. It is not a claim that the cycle has been solved: dividing
	 * load round a loop needs a rule that does not exist yet.
	 *
	 * ONLY THE PIECES CAUGHT IN THE KNOT, though. Un-orderability is a property of
	 * the solver, not of the structure, and DESIGN.md §3 is explicit: a piece is
	 * unsupported only when it genuinely has no load path to the ground. A piece
	 * BENEATH a knot keeps its support and carries everything except the unroutable
	 * contribution; a piece whose only support is in one loses its own.
	 *
	 * False for an out-of-range handle: an unknown piece is not being held up.
	 *
	 * THIS IS THE LAST SOLVE'S ANSWER, AND REMOVAL DOES NOT REWRITE IT. GetPiece,
	 * IsPieceRemoved and GetBreakPass all answer about a removed piece immediately; this
	 * one reads solver output, which only SolveLoads writes, so removal is not visible
	 * here until something has re-solved:
	 *
	 *     never solved      false, for every handle — there is no answer yet
	 *     removed           the LAST SOLVE'S answer, unchanged, until the next solve
	 *     after that solve  false, and a removed GROUNDED piece is no longer earth
	 *
	 * The middle row is stale rather than wrong, and it is deliberate. Clearing the one
	 * entry on removal would leave a HALF-STALE array — that piece current, every
	 * neighbour still describing a structure that no longer exists — which is a worse
	 * thing to hand a caller than a uniformly stale array with a documented scope. The
	 * gameplay shape that reaches it is the MVP's own: remove a piece on player
	 * interaction, then ask about its neighbours to decide what to release to dynamics.
	 * Re-solve first. Structure.RemovedPieceSupportNeedsASolve pins all three rows, so
	 * reconsidering this turns that test red rather than letting the two drift again.
	 */
	bool IsPieceSupported(int32 PieceIndex) const;

	/**
	 * WHY this piece is or is not being held up, after SolveLoads.
	 *
	 * IsPieceSupported is the composite answer and is unchanged; this refines it, and
	 * the two must never disagree:
	 *
	 *     IsPieceSupported(H)  ==  (GetPieceSupport(H) is Grounded or Supported)
	 *
	 * for every handle, at every moment, including before a solve and in the window
	 * between a removal and the next one. Two accessors reading one solve and telling
	 * different stories is the exact defect the stranding rule was written to close;
	 * reintroducing it one level up would be worse, because this one claims to explain.
	 *
	 * STRANDED IS ONLY FOR PIECES IN THE KNOT. A piece resting on one is Falling: it is
	 * not itself unroutable, it has simply lost the support that was carrying it, and
	 * that is ordinary physics rather than a solver limitation.
	 *
	 * SAME SCOPE AS IsPieceSupported, because it is the same solver output — see that
	 * contract for the three rows. A removed piece reads the last solve's answer until
	 * something re-solves and Falling after it, and there is deliberately no fifth
	 * enumerator for "removed": IsPieceRemoved answers that immediately and this one
	 * cannot, so a Removed value here would contradict a stale Supported from
	 * IsPieceSupported in exactly the window Structure.RemovedPieceSupportNeedsASolve
	 * pins.
	 *
	 * Falling for an out-of-range handle: nothing is holding up a piece that does not
	 * exist, and Stranded would be a positive claim about a knot that is not there.
	 */
	EPieceSupport GetPieceSupport(int32 PieceIndex) const;

	/**
	 * Whether the last solve actually computed a support state for this handle.
	 *
	 * THE ONE THING GetPieceSupport CANNOT SAY. Falling means both "nothing is holding
	 * this up" and "nobody has asked yet", deliberately: Falling sits at enumerator zero
	 * so an absent answer cannot claim the structure is resting on the earth. What that
	 * costs is the ability to tell the two apart, and this is where it is bought back —
	 * PieceSupported is sized by SolveLoads and by nothing else, so its extent IS the set
	 * of handles the last solve answered for.
	 *
	 * False before any solve, false for a handle added since the last one, and false for a
	 * handle that names no piece. TRUE for a removed piece inside that extent: the last
	 * solve did answer for it, and IsPieceSupported's contract already records that the
	 * answer is stale until something re-solves. Whether a piece is still there is
	 * IsPieceRemoved's question, and keeping the two apart is what makes this one purely
	 * about the extent.
	 *
	 * WHO NEEDS IT: anything that turns "not held up" into an IRREVERSIBLE action rather
	 * than into a readout, because the polarity inverts at that seam. As an answer, Falling
	 * by default is the cautious one; as a command it says "release the whole structure,
	 * foundation included", which is the fail-OPEN direction and is not undone by a later
	 * solve. A readout may take GetPieceSupport at face value; a command must ask this
	 * first. Same shape as DESIGN.md §2's caller obligation for a degenerate normal.
	 */
	bool HasSupportAnswer(int32 PieceIndex) const;

private:
	/**
	 * ONE SPANNED OPENING, AS THE GROUP PASS LEAVES IT: the seated pieces at its two ends.
	 *
	 * An arch is the only thing in this solver that is a fact about a RUN of pieces rather
	 * than about any one of them, and the thrust is the reason that has to be written down.
	 * H is one number for the whole opening, applied equally and oppositely at the two
	 * abutments — so the pass that computes it needs both ends at once, and a per-joint rule
	 * could not state it. That is ARCHING_DESIGN's trap 2: applying +H at one springing and
	 * forgetting the other gives the structure a net horizontal force out of nowhere while
	 * every joint still reads perfectly plausibly.
	 *
	 * THE TWO ENDS ARE AN ORDER, NOT A DIRECTION IN THE WORLD. Which end is 0 falls out of
	 * whichever abutment the group walk happened to meet first, and nothing may depend on it:
	 * the model has no reason to prefer one side of a hole, and inventing one would be a
	 * convention a later slice has to unpick.
	 */
	struct FSpannedArch
	{
		/**
		 * The seated pieces the group pushes against, split by which end of the opening they
		 * stand at. Both are non-empty — a group with nothing on one side does not span, and
		 * an arch with nothing to push against at one end is a cantilever.
		 */
		TArray<int32> Abutments[2];

		/**
		 * Unit direction toward end 0, which is the way the arch pushes end 0's abutments and
		 * is the exact negative of the way it pushes end 1's.
		 *
		 * ONE VECTOR RATHER THAN TWO, so that the two thrusts sum to exactly zero rather than
		 * to a rounding of it. +H*D and -H*D cancel bit for bit on every component; two
		 * separately normalised directions would not.
		 */
		FVector TowardEndZero = FVector::ZeroVector;

		/**
		 * L: how far apart the two ends of the opening stand, cm.
		 *
		 * MEASURED BETWEEN THE ABUTMENTS' OWN CENTRES, one mean per end, which is the clear
		 * opening to the centimetre for a running-bond wall: each springing keeps half a cell
		 * of bearing, so its centre sits half a cell outboard of the masonry that was taken
		 * out and the two half cells make up exactly the one that was not. Seat centroid to
		 * seat centroid is half a cell wider and reads 5% high on a ten-cell hole.
		 *
		 * RECORDED HERE BECAUSE THIS IS WHERE IT IS KNOWN. The thrust pass has the abutments
		 * but not the group they stand either side of, and the span is a fact about the
		 * opening rather than about either end of it — one number per arch, which is what
		 * keeps the two thrusts equal and opposite.
		 */
		double SpanCm = 0.0;
	};

	/**
	 * Re-seat the pieces a hole has left with NO seat at all onto the group they belong to.
	 *
	 * THE GROUP RULE, AND IT IS THE ONE THING IN THE SOLVER THAT READS GEOMETRY TO DECIDE A
	 * ROUTE. A hole one brick wide leaves two half-seated bricks that each keep their own seat,
	 * and HasArchingAbutment answers those alone. A hole WIDER than one brick leaves the bricks
	 * in the middle of it with no seat whatever: under the two-tier rule they fall back to their
	 * head joints, become each other's support, and the solver strands the pair as a two-node
	 * knot — so a wall that should span its opening loses its middle and everything above it.
	 *
	 * What those bricks are actually doing is spanning, and a span is a statement about a RUN of
	 * pieces rather than about any one of them: contiguous unseated pieces form a GROUP through
	 * their intact head joints, and the group carries only if something seated stands on BOTH
	 * SIDES of it. Each member is then re-seated toward its nearer abutment, which is acyclic by
	 * construction because every edge runs from a longer path to a shorter one.
	 *
	 * MOMENTS_DESIGN's discipline line is REVISED, not broken, and ARCHING_DESIGN carries the
	 * replacement: GetJointRole still may never read geometry — the tier of one joint stays a
	 * fact about one normal and one pairing — and what reads geometry is this GROUP rule sitting
	 * above the tier. It is gated on HasCompleteGeometry() and is a NO-OP without it, so a
	 * geometry-free structure routes exactly as it did, bit for bit. That gate is not politeness:
	 * both fuzz generators emit structures with no positions at all, 20,000 cases between them,
	 * and they are the only property tests over routing this project has.
	 *
	 * THE ABUTMENTS ARE THE GROUP'S OWN "INCOMPLETE SEAT" NEIGHBOURS. ARCHING_DESIGN counts them
	 * as members and asks for a live seat on both sides of the group's centre; the boundary is
	 * drawn one piece tighter here — the group is the unseated run and the seated neighbours it
	 * pushes against are its abutments — which is the same rule with the same seats deciding it,
	 * and it keeps every piece that merely lost one of two seats out of a group entirely.
	 *
	 * @param PieceJoints          Every joint touching each piece, in ascending index order.
	 * @param PieceHasNoSeat       Which live, ungrounded pieces have no intact bed joint beneath.
	 * @param SupportConnections   What holds each piece up. Rewritten in place for the members of
	 *                             an abutted group, and left exactly as found everywhere else.
	 * @param PieceReseatedOnAnArch Set for each piece this rewrote, and false everywhere else.
	 * @param Arches               One entry per group that actually spans, naming the seated
	 *                             pieces at its two ends. Emptied first, so a structure with no
	 *                             geometry and a structure with no hole both come back with none.
	 */
	void ReseatSpannedGroups(
		const TArray<TArray<int32>>& PieceJoints,
		const TArray<bool>& PieceHasNoSeat,
		TArray<TArray<int32>>& SupportConnections,
		TArray<bool>& PieceReseatedOnAnArch,
		TArray<FSpannedArch>& Arches) const;

	/**
	 * Push each spanned opening's two abutments apart by the thrust the arch develops.
	 *
	 * AN ARCH PUSHES SIDEWAYS, AND SOMETHING HAS TO RESIST IT. ARCHING_DESIGN.md:
	 *
	 *     d_e = min( cover above the span , 0.866 * L )      arching depth
	 *     r   = d_e / 3                                      thrust line rise, kern-limited
	 *     W   = the load the solver already accumulated      NOT a triangle
	 *     H   = W * L / (8r)      V = W / 2                  per abutment
	 *
	 * WHAT RESISTS IT IS ALREADY IN THE MODEL, which is the whole point: the thrust arrives
	 * horizontally at a bed joint, ClassifyForce calls a horizontal force on a bed joint shear,
	 * and shear capacity is already `c + mu*sigma_n` truncated at the profile's ceiling. No new
	 * axis, no new strength, no new profile data — dry stone's zero cohesion then refuses to
	 * arch at any span with no per-material branch anywhere.
	 *
	 * RUN AFTER THE ACCUMULATION AND NEVER INSIDE IT. W is what the solver has already routed
	 * to the abutments' seats, so this reads finished forces; and adding a shear component
	 * changes no split, no support list and no moment, which is why every vertical answer in
	 * the structure is bit-identical to one computed without it.
	 *
	 * AND THE COVER IS WHAT USUALLY DECIDES. `d_e` is the SMALLER of the masonry actually
	 * standing over the opening and the angle's `0.866*L`, so `H` grows as `1/cover` while `V`
	 * falls with it: thin the brickwork over a hole and the thrust ratio `3L/(4 d_e)` blows up,
	 * which is why a wide opening near the top of a wall cannot arch at all. One cover per
	 * arch, the thinnest of the two ends — see MasonryDepthAboveCm.
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
	 * ONE STEP OF THE UPWARD WALK, AND ALSO THE QUESTION "IS THERE A STACK HERE AT ALL". Two
	 * callers need it and neither may have its own copy: MasonryDepthAboveCm takes this step
	 * once per course, and SolveLoads asks it once before crediting a joint with any composite
	 * section at all. A piece with nothing standing on it is ONE UNIT, and one unit is not a
	 * composite of anything — see the composite gate in SolveLoads for why that distinction is
	 * the definition of the mechanism rather than a refinement of it.
	 *
	 * A joint that has GIVEN holds nothing up and is not a course of masonry either, and a
	 * removed piece is not standing anywhere. Both are excluded here, which is what makes the
	 * depth shorten as a wall comes apart.
	 */
	int32 PieceRestingOn(int32 Piece, const TArray<TArray<int32>>& PieceJoints) const;

	/**
	 * How much bonded masonry stands over one bed joint, cm, by a BOUNDED UPWARD WALK.
	 *
	 * ONE MEASUREMENT, TWO CALLERS, AND THEY WANT IT FOR DIFFERENT REASONS — which is exactly
	 * why there is one of it. ApplyArchingThrust wants the COVER over a springing, because
	 * ARCHING_DESIGN.md's `d_e = min(cover, 0.866*L)` is the difference between a ten-cell hole
	 * under one course of brickwork reading 0.058 of its springing's capacity and reading 1.51
	 * of it. SolveLoads wants the COMPOSITE DEPTH over a seat, because a stack of courses over
	 * a lost support resists its overturning moment as a deep beam of section `t*D^2/6` rather
	 * than as one bed patch, and `D` squared is the whole of that slice. Both are the same
	 * question — how much wall is standing on this plane — and two copies of the walk would
	 * agree until the day one of them was fixed.
	 *
	 * COUNTED IN COURSES, AND THE COURSE THE JOINT IS UNDER IS THE FIRST OF THEM. For the arch
	 * it is the spanning course, the first ring rather than something resting on one; for the
	 * composite section it is the corbelled brick itself. Either way the shallowest answer a
	 * wall can give is one course pitch and never zero. That first rise is measured at the
	 * joint itself, from the piece below it to the piece standing on it, and every course after
	 * it contributes the rise to the piece above.
	 *
	 * A WALK OVER BED JOINTS AND NEVER A SPATIAL QUERY. What is over the plane is what the
	 * connection graph says is over it: the joints already index the neighbourhood, so this
	 * costs a step per course and needs no broadphase, no octree and no world. A joint that has
	 * GIVEN conducts nothing and is not a course of anything either, so the walk stops at a
	 * break — which is what bounds the composite depth to masonry that is actually bonded.
	 *
	 * AND IT IS BOUNDED TWICE. `EnoughDepthCm` is the depth past which the caller's own answer
	 * cannot change — `0.866*L` for the arch, where the angle takes over, and nothing at all
	 * for the composite section, which passes the largest double there is and stops when the
	 * wall does. The piece count is the second bound and is pure defence against a graph whose
	 * normals claim A is above B and B above A.
	 *
	 * A CHAIN AND NOT A TRAVERSAL. Running bond puts two pieces above each brick, and this
	 * follows the first of them by ascending joint index rather than exploring both. For a wall
	 * of uniform height the two columns reach the same place, which is the case this measures;
	 * a stepped or gabled wall would have its depth decided by which column the walk happened
	 * to take, and nothing tests that yet.
	 *
	 * ZERO WHERE THERE IS NOTHING TO MEASURE — a joint naming no second piece, or a course with
	 * no measurable rise. Both callers guard on that rather than dividing by it, and for the
	 * composite section zero is already the no-relief answer.
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
	 * How deep the CORBELLING BODY standing on one bed joint is, cm — the FLOOR under the
	 * composite depth, and the one part of it no lever arm may trim away.
	 *
	 * THE CORBELLING COURSES ARE WHAT GENERATE THE MOMENT, so they cannot be refused the section
	 * they generate it with. They are bonded into one cantilevering body and need no shear
	 * transfer to be engaged: whatever the arm's tip does, the courses under it come with it.
	 * Masonry ABOVE the cut is a different thing entirely — it is not being bent by the corbel's
	 * moment and has to be dragged into the section by shear over a distance, which is exactly
	 * what `lambda*e` bounds. So the two are bounded differently and only the second one is:
	 *
	 *     D  =  min( masonry above , max( THIS , lambda*|M|/|F| ) )
	 *
	 * AND THE FLOOR CAN NEVER CREDIT A SINGLE COURSE OF THE WALL ABOVE THE CUT, BECAUSE THE FLOOR
	 * IS THE CUT. The walk stops at the first course that is not corbelling, which is by
	 * definition the first course of the wall standing over the body — an exact structural
	 * guarantee rather than a bound that happens to hold on the fixtures.
	 *
	 * CORBELLING IS "SEATED ON EXACTLY ONE COURSE", WHICH IS A FACT ABOUT THE GRAPH AND NOTHING
	 * ELSE. A brick laid in bond straddles two below it; a brick stepped out over a raking cut,
	 * or the outer face of a corbel, has one seat and hangs off it. That is the whole predicate,
	 * and it needs no geometry beyond the roles the graph already carries.
	 *
	 * ⚠ IT IS TRUE OF EVERY PIECE IN A STACK-BOND COLUMN OR A ONE-BRICK-WIDE WALL, where each
	 * unit sits squarely on exactly one below. There the walk runs to the top and the floor
	 * becomes the whole wall, which is the very defect `lambda*e` was added to remove. The
	 * direction test that would refuse it — the piece's centre of mass must also lie OUTBOARD of
	 * its seat, on the side the root joint is already eccentric to, which is exactly what
	 * HasArchingAbutment asks — is deliberately NOT built, because no fixture in this project can
	 * tell the two apart: a stack-bond wall has zero eccentricity at every seat, so no joint in
	 * one ever carries a moment, and this is never reached. Building it would be capability no
	 * test covers. See CURRENT_STATE.md for the fixture that would be needed first.
	 *
	 * WHICH PIECE THE WALK STEPS TO IS DECIDED BY THE PREDICATE, NOT BY PieceRestingOn. That
	 * chain takes the first bed joint above by ascending index, which on a filled corbel is the
	 * two-centimetre lap onto the course INBOARD rather than the eighteen-centimetre seat under
	 * the stepping front — measured, on the very fixture that drove this. Following it would end
	 * the body at its second course on every corbel in the suite. So this walks the corbelling
	 * chain: the first piece resting here that is itself corbelling.
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
	 * THE FOURTH GATE OF THE ARCHING RULE, and the one that decides whether a piece which
	 * has lost a seat bridges the hole or cantilevers over it. It asks for an INTACT HEAD
	 * JOINT ON THE ECCENTRIC SIDE — the side the centre of mass sits, measured in the plane
	 * of the seat from the seat's own centroid — to a neighbour that reaches the ground and
	 * is NOT resting on the piece asking. FConnection::ArchingMomentScale owns the other
	 * half, the part one joint can answer alone.
	 *
	 * THE DIRECTION IS THE WHOLE OF IT. Both of this subsystem's eccentric fixtures have a
	 * live, intact, supported neighbour — on the SEATED side. The staircase corbel's
	 * eccentric side is precisely where the cut removed the neighbour, and the waisted
	 * wall's two top bricks overhang OUTWARD so their shared head joint is on the seated
	 * side of each. Asking only "is there an intact head joint" arches both, which halves
	 * every overhang in the game and stands the photographed failure back up.
	 *
	 * AND THE NEIGHBOUR MUST NOT BE HANGING FROM US. Two unseated bricks propping each other
	 * over open air look locally identical to a real arch — each is Supported, each has an
	 * intact head joint to the other — so granting one would let a wall hang from nothing.
	 * The separating fact is one step away and this asks for it directly: the piece must not
	 * appear among the neighbour's own supports. Deliberately NOT a reachability query;
	 * SolveLoads runs once per cascade pass, and the cheap form is exact for this case
	 * because a longer loop is what LoadReturnsToPiece already strands.
	 *
	 * AND A GROUP THAT SPANS SUPERSEDES THAT REFUSAL WITHOUT WEAKENING IT. Widen the hole past
	 * one brick and the neighbour on the eccentric side is an unseated piece re-seated onto the
	 * group — so it does lean on this one, and the one-step test refuses correctly, since two
	 * bricks propping each other is precisely what it is looking at. What makes it an arch
	 * instead is the FAR abutment: ReseatSpannedGroups only re-seats a group with something
	 * seated on both sides of it, so a neighbour carrying that mark has a reaction beyond it and
	 * the thrust has somewhere to go. Without the mark nothing here changes at all.
	 *
	 * AND AN ABUTMENT THE SEAT CANNOT PUSH AGAINST IS NOT AN ABUTMENT. The cap moves the thrust
	 * line from the overhang to the kern edge, which DELETES a couple of `(1 - k)*|M|` from what
	 * the joint carries, and nothing on the half-seated brick's free body can supply it except a
	 * horizontal pair: a push through the head joint above, and its equal and opposite reaction
	 * as SHEAR in this seat's own bed plane. So the seat has to be able to carry `dM / z`, with z
	 * the head joint's own height above the bed plane — against `c + mu*sigma_n`, the same
	 * Mohr-Coulomb envelope ComputeUtilisation measures every other sliding demand against, and
	 * on the seat's own squeeze. Where it cannot, the relief is WITHHELD and the joint reads what
	 * it was actually carrying. DESIGN.md §7 gap 4; the ratio is a fact about the bond geometry
	 * alone — `(e - h/6)/z`, the load having cancelled — so a mortared springing affords it three
	 * times over at every wall height while dry stone cannot afford it at any.
	 *
	 * A SPANNED GROUP IS CHECKED BY BEING PUSHED INSTEAD, so this gate leaves it alone. Where
	 * ReseatSpannedGroups formed an arch, ApplyArchingThrust puts the real horizontal force on
	 * both springings once accumulation settles and the ordinary shear axis then measures it; a
	 * second, withholding check on the same joint would judge the same thrust twice and by two
	 * different rules. It is the ONE-CELL hole that has no thrust pass of its own — which is why
	 * the mark that separates the two cases is the same one the paragraph above turns on.
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
	 * HOW THE EQUILIBRIUM GATE DISPOSED OF A PASS, so SolveAndBreak knows whether the LP answered
	 * authoritatively (and it must therefore NOT run the per-joint capacity sweep) or declined (and
	 * the router sweep is the sole authority for this pass). Below the block cap the mechanism is
	 * the ONLY break authority (PROMOTION_DESIGN.md §12 D7's 3b section, §3.7); the strength sweep
	 * is demoted to an estimator that still populates the utilisation readout but latches nothing.
	 *
	 * DeclinedToRouter is the ZERO enumerator so a default or fail-closed value routes to the
	 * capacity sweep — the same fail-closed polarity every other refusal here has.
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
	 * THE EQUILIBRIUM GATE — DESIGN.md §7 evolution step 4, PROMOTION_DESIGN.md §6 Slice 3. Below
	 * the block cap it is the SOLE break authority: it asks the rigid-block LP whether the WHOLE
	 * structure has any admissible force system in equilibrium with self-weight, and on "no" it
	 * extracts the phase-1 dual (the Farkas certificate = the kinematic collapse mechanism, Slice
	 * 3a) and severs exactly the joints that mechanism opens or slides. One entry point, called
	 * from SolveAndBreak and from nothing else.
	 *
	 * WHAT IT CATCHES that no joint check can: ComputeUtilisation happily reports a confident
	 * number for a joint on which NO EQUILIBRIUM SOLUTION EXISTS (DESIGN.md §5.7) — a stack
	 * offset far enough per course, or a body on two bearings both to one side of its centroid,
	 * reads a comfortable per-joint utilisation while its resultant has long since left the
	 * bearing. The LP reasons about the whole admissible force system, so it sees the loss the
	 * per-joint sweep cannot; equally it STANDS a structure the router only strands for want of a
	 * routing rule (a knot, an opening with no abutment), which is why it also decides support.
	 *
	 * THE POSE IS FEASIBILITY AT lambda = 1 (bGravityIsLive = false, PROMOTION_DESIGN §12 D6-b):
	 * the identical Stands/Falls boolean as lambda* but far cheaper. SCOPED BY A BLOCK CAP
	 * (EquilibriumGateBlockCap, D6-c): above it the gate DECLINES and behaviour falls through to
	 * the per-joint capacity sweep, which then both breaks AND enumerates support exactly as
	 * production did before the gate. A NO-OP without complete geometry and on any LP refusal
	 * (fail closed to the router).
	 *
	 * ON STANDS OR FALLS it rewrites the support arrays from the LP (PieceSupported/PieceStranded)
	 * so GetPieceSupport is LP-authoritative below the cap: a piece the LP carries reads Supported,
	 * a piece the mechanism moves reads Falling, overriding the router's Stranded verdict. On Falls
	 * it severs the mechanism's joints (mapped to production connections through ConnectionOfJoint),
	 * latched and stamped with this pass exactly as an over-capacity joint is.
	 *
	 * @param Pass The cascade pass any break is stamped with.
	 * @return How the gate disposed of this pass (see EEquilibriumGateDisposition).
	 */
	EEquilibriumGateDisposition BreakByEquilibrium(int32 Pass);

	/**
	 * REWRITE THE PER-PIECE SUPPORT ARRAYS FROM THE LP VERDICT, so GetPieceSupport is
	 * LP-authoritative below the cap (PROMOTION_DESIGN.md §12 D7's 3b section). Every piece the
	 * bridge included reads Supported/Grounded if the LP carries it and Falling if the mechanism
	 * moves it; the router's Stranded verdict is overridden for the pieces the LP stands. Called
	 * only from BreakByEquilibrium, on an answered (Stands or Falls) solve. See its definition.
	 */
	void ApplyLimitAnalysisSupport(
		const RigidBlockOracle::FOracleProblem& Problem,
		const RigidBlockOracle::FOracleResult& Result);

	/**
	 * THE PER-JOINT CAPACITY SWEEP — the router's break authority, one sweep per pass, stamping
	 * every joint over its own capacity with this pass. The sole break authority above the block
	 * cap and on any LP refusal; below the cap the equilibrium gate answers instead and this does
	 * not run. Returns whether any joint gave. Called only from SolveAndBreak.
	 */
	bool BreakByCapacitySweep(int32 Pass);

	/**
	 * THE EQUILIBRIUM GATE'S BLOCK CAP — the fail-closed boundary that keeps synchronous LP
	 * authority off the flagship scenarios (D6-c). At or below it the gate is authoritative; above
	 * it the gate declines to the router. Raised from the Slice-2 latency band (84) at Slice 3b so
	 * the medium wall-catalogue fixtures (rows 10/19/20 at 125-174 blocks) are IN scope for the LP
	 * to stand or fell — the driving reds of 3b — while the 375-block flagship wall and the giant
	 * corbels stay above it on the router. This exceeds the measured ~84-104-block per-action
	 * latency band, so a structure of a few hundred blocks now solves the LP synchronously; no
	 * production structure sits in that range today (the default wall is ~1200 blocks), so the
	 * cost is a test-time one for now — CURRENT_STATE carries the open latency item.
	 */
	int32 EquilibriumGateBlockCap = 200;

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
	 * Whether each piece is IN an unroutable knot — not merely resting on one.
	 *
	 * The fixpoint in SolveLoads has always computed this, because it is what decides
	 * who conducts support on the next pass; it is kept rather than discarded so that
	 * GetPieceSupport can say WHY a piece is not held up. Both accessors read these two
	 * arrays and neither recomputes anything, which is what stops the reason drifting
	 * away from the answer it explains.
	 *
	 * A piece is never both stranded and supported: the reachability walk neither marks
	 * a stranded piece nor crosses one, and the pass that strands is the pass after
	 * which nothing is supported through it.
	 */
	TArray<bool> PieceStranded;

	/** What each connection carries, in Unreal force units. */
	TArray<FVector> ConnectionForces;

	/**
	 * The bending moment each connection carries about its own centroid, uu.cm.
	 *
	 * BESIDE ConnectionForces AND WITH ITS LIFETIME, not folded into it and not a third
	 * kind of state. Both are rebuilt from scratch by every solve and are self-healing
	 * for the same reason: a joint whose moment survived a re-solve would describe a
	 * structure that no longer exists, exactly as a stale force would.
	 *
	 * SEPARATE RATHER THAN ENCODED, deliberately. A moment folded into the force vector
	 * — as extra length, or as a tilt — would make GetConnectionForce report a number
	 * that no longer describes the load, which is this subsystem's recurring signature.
	 *
	 * NON-ZERO ONLY WHERE THE STATICS IS DETERMINATE: a piece whose load reaches the
	 * ground through exactly one joint, and which somebody actually placed. See
	 * SolveLoads for why the per-joint rule that looks obvious is wrong.
	 */
	TArray<FVector> ConnectionMoments;

	/**
	 * How deep the bonded masonry standing over each connection is, cm.
	 *
	 * THE THIRD PARALLEL ARRAY, with the lifetime of the other two and for the same reason:
	 * a depth that survived a re-solve would describe a wall that has since been cut, exactly
	 * as a stale force or a stale moment would. Rebuilt from scratch by every solve.
	 *
	 * BESIDE THE JOINTS RATHER THAN ON THEM, deliberately, because it is an ANSWER and not an
	 * interface. A joint stays a face and a strength; how much wall happens to be standing over
	 * it is a fact about the graph at one instant, and storing it on FConnection would put a
	 * quantity into the joint that could be read out of GetConnection before any solve had
	 * produced it.
	 *
	 * NON-ZERO ONLY WHERE ConnectionMoments IS, and on bed joints alone. See
	 * GetConnectionCompositeDepthCm.
	 */
	TArray<double> ConnectionCompositeDepthCm;

	/**
	 * Which breaking pass gave each connection, counted from 1, or INDEX_NONE.
	 *
	 * Grown by AddConnection rather than sized by a solve, and NEVER cleared: joints
	 * never heal, so a stamp once written is history and re-running a cascade must not
	 * rewrite it. It is the only record of the ORDER a collapse happened in, which is
	 * what phase 5's visualisation plays back — the latch on FConnection says only
	 * whether a joint gave, never when.
	 */
	TArray<int32> ConnectionBreakPass;

	/**
	 * How many times SolveLoads has been entered, ever. See NumSolves.
	 *
	 * NOT SOLVER OUTPUT, so it sits below the arrays rather than among them: every one of
	 * those is rebuilt by a solve, and this is the only field a solve accumulates. Nothing
	 * clears it and nothing branches on it.
	 */
	int32 SolveCount = 0;
};
