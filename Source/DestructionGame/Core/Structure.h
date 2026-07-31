// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Connection.h"

/**
 * A piece of a structure: plain data, deliberately.
 *
 * No actor, no component, no transform. A piece is a mass that either rests on
 * the earth or does not, and an identity that connections can refer to. Whoever
 * owns the world resolves the handle back to something visible; the solver never
 * needs to.
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
 * SCOPE. This computes loads. It does not break anything: solving must leave
 * every connection exactly as intact as it found it, so a solve can be re-run,
 * and so breaking stays a separate, deliberate step.
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

	int32 NumPieces() const;
	int32 NumConnections() const;

	/** Out-of-range handles return a default-constructed placeholder. */
	const FStructurePiece& GetPiece(int32 PieceIndex) const;
	const FConnection& GetConnection(int32 ConnectionIndex) const;

	/**
	 * Recompute what every connection carries and which pieces reach the ground.
	 *
	 * Non-destructive: no connection may give as a result of solving, however
	 * overloaded it is.
	 */
	void SolveLoads();

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
	 */
	bool IsPieceSupported(int32 PieceIndex) const;

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

	/** What each connection carries, in Unreal force units. */
	TArray<FVector> ConnectionForces;
};
