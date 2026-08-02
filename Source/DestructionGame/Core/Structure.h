// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Connection.h"

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
	 * The force this connection carries, in Unreal force units, after SolveLoads.
	 *
	 * A vertical vector of the accumulated magnitude — gravity does not change
	 * direction because a joint is vertical. FConnection::ApplyForce is what
	 * resolves it against the joint's own normal, which is why the same vector is
	 * compression on a bed joint and shear on a head joint.
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

private:
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
	 * Which breaking pass gave each connection, counted from 1, or INDEX_NONE.
	 *
	 * Grown by AddConnection rather than sized by a solve, and NEVER cleared: joints
	 * never heal, so a stamp once written is history and re-running a cascade must not
	 * rewrite it. It is the only record of the ORDER a collapse happened in, which is
	 * what phase 5's visualisation plays back — the latch on FConnection says only
	 * whether a joint gave, never when.
	 */
	TArray<int32> ConnectionBreakPass;
};
