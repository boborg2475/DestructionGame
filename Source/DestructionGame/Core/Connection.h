// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ConnectionLoad.h"
#include "Core/ConnectionStrength.h"

/**
 * A joint between two pieces: the thing that gives before a structure falls.
 *
 * It holds together the two halves of the pipeline that already exist — the
 * interface normal ClassifyForce needs, and the area and strength profile
 * ComputeUtilisation needs — so a caller supplies only a force.
 *
 * A plain struct on purpose: no UObject, no actor, no world. The load solver
 * built on top of it has to be testable without a ticking solver, and pieces are
 * therefore opaque integer handles rather than actor pointers — whoever owns the
 * pieces resolves them.
 */
struct FConnection
{
	/** Handle of the piece the interface normal points away from. */
	int32 PieceA = INDEX_NONE;

	/** Handle of the piece the interface normal points toward. */
	int32 PieceB = INDEX_NONE;

	/**
	 * Normal of the plane where the two pieces meet, pointing toward PieceB.
	 *
	 * See the orientation convention in ConnectionLoad.h: forces passed in must
	 * be the forces acting on PieceB. Flipping this without negating the force
	 * silently swaps compression and tension.
	 */
	FVector InterfaceNormal = FVector::ZAxisVector;

	/** Area of the face the two pieces meet across, cm2. */
	double InterfaceAreaSqCm = 0.0;

	/**
	 * Where the face sits in the world, cm. On the normal's own axis this is the
	 * plane of the joint itself.
	 *
	 * Zero for a joint whose producer supplied no geometry, which is not a position
	 * at the origin — it is "nobody said". See InterfaceHalfExtentCm.
	 */
	FVector InterfaceCentreCm = FVector::ZeroVector;

	/**
	 * Half the face's extent on each axis, cm, and therefore ZERO ON THE NORMAL'S
	 * OWN AXIS: the face is a rectangle, not a box.
	 *
	 * Zero on all three means no geometry was supplied, which is a joint with no
	 * bending capacity known rather than a degenerate one — the area alone still
	 * answers a centred load exactly. FStructure::AddConnection is what keeps the
	 * two states apart: supplied geometry must agree with the area it claims to
	 * describe, so a plausible area with the wrong lever arm is inexpressible.
	 */
	FVector InterfaceHalfExtentCm = FVector::ZeroVector;

	/** Directional strengths of the joint itself — mortar, nail, bolt. */
	FConnectionStrength Strength;

	/**
	 * Evaluate a world-space force against this joint and return its utilisation.
	 *
	 * Latches: once utilisation exceeds 1 the joint has given and stays given.
	 * The call that discovers the failure reports the utilisation that broke it;
	 * every call after that returns zero, because a joint that has given is out
	 * of the structure and carries nothing.
	 *
	 * THE MOMENT IS THE SAME PARAMETER UtilisationUnder TAKES, defaulted the same
	 * way, because this call IS that query plus the latch. A break decision made on
	 * the force alone would be a SECOND evaluation of the question a strain readout
	 * already answers — and the two agree exactly until a load path misses a joint's
	 * centroid, at which point a joint can be drawn at 1.25 of capacity and hold
	 * forever. That is the direction where nothing falls, which is the expensive one.
	 *
	 * @param MomentUuCm Bending moment about this joint's centroid, uu.cm.
	 */
	double ApplyForce(const FVector& Force, const FVector& MomentUuCm = FVector::ZeroVector);

	/**
	 * What this force WOULD do to this joint. Never latches, never mutates.
	 *
	 * The read-only half of ApplyForce, and the one a strain readout asks every
	 * frame: a display that had to call ApplyForce to find out how loaded a joint
	 * is would take the wall apart by drawing it.
	 *
	 * PURE ARITHMETIC — it does not consult the latch. A joint that has already
	 * given still answers what the force would have done to it, which is exactly
	 * what a fresh joint of the same geometry and profile would report. Latching
	 * therefore stays known in exactly one place, ApplyForce, and a caller that
	 * cares whether the joint is still in the structure asks HasGiven, which it
	 * wants to do anyway.
	 *
	 * THE MOMENT IS A WORLD-SPACE VECTOR AND THE JOINT RESOLVES IT ITSELF, exactly
	 * as it already does for the force. A caller supplies (p - c) x F about this
	 * joint's own centroid and nothing else: which of its components bend the face
	 * and which one merely twists it is a fact about the interface, and the
	 * interface is what knows its own normal and its own rectangle. Resolved
	 * outside, that decomposition would be a second copy of the geometry — the
	 * same argument that keeps a caller from supplying compression and shear
	 * directly.
	 *
	 * TORSION IS DROPPED, DELIBERATELY. The component about the joint's own normal
	 * twists it rather than opening an edge, which needs a polar modulus this face
	 * does not carry; it is second-order for gravity on a rectangular joint and is
	 * out of scope per MOMENTS_DESIGN.md. Folding it into either bending term
	 * would be a plausible number against the wrong section.
	 *
	 * DEFAULTED TO ZERO, WHICH IS NOT A TOLERANCE. A load path with no
	 * eccentricity and one nobody measured both give a moment of exactly zero, the
	 * bending term vanishes, and the answer is bit for bit what it was before
	 * moments existed — the same way a zero friction coefficient reduces
	 * Mohr-Coulomb exactly rather than approximately.
	 *
	 * @param MomentUuCm Bending moment about this joint's centroid, uu.cm.
	 */
	double UtilisationUnder(
		const FVector& Force, const FVector& MomentUuCm = FVector::ZeroVector) const;

	/**
	 * Take this joint out of the structure without it having failed.
	 *
	 * A joint whose piece was REMOVED did not snap — it was deleted — but it is just
	 * as absent, so it must answer HasGiven the same way: out of the tier decision,
	 * out of the load paths, carrying nothing. Nothing here records a reason, because
	 * the joint is not where that belongs; whoever severed it knows why, and
	 * FStructure keeps the record by leaving the break-pass stamp alone.
	 *
	 * Idempotent, and monotonic like the latch it sets: joints never heal.
	 */
	void Sever();

	/** Whether this joint has given. Never returns to false. */
	bool HasGiven() const;

private:
	bool bHasGiven = false;
};
