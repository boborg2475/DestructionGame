// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ConnectionLoad.h"
#include "Core/ConnectionStrength.h"

/**
 * A joint between two pieces.
 *
 * Holds the interface normal, area, and strength profile, so a caller passes only a
 * force. Plain struct (no UObject/actor/world) to keep the load solver testable without
 * a world; pieces are integer handles resolved by their owner.
 */
struct FConnection
{
	/** Handle of the piece the interface normal points away from. */
	int32 PieceA = INDEX_NONE;

	/** Handle of the piece the interface normal points toward. */
	int32 PieceB = INDEX_NONE;

	/**
	 * Normal of the plane where the two pieces meet, pointing toward PieceB. Forces passed
	 * in must be the forces acting on PieceB (see ConnectionLoad.h); flipping this without
	 * negating the force swaps compression and tension.
	 */
	FVector InterfaceNormal = FVector::ZAxisVector;

	/** Area of the face the two pieces meet across, cm2. */
	double InterfaceAreaSqCm = 0.0;

	/**
	 * Where the face sits in the world, cm; on the normal's own axis, the plane of the
	 * joint. Zero means no geometry was supplied, not a position at the origin. See
	 * InterfaceHalfExtentCm.
	 */
	FVector InterfaceCentreCm = FVector::ZeroVector;

	/**
	 * Half the face's extent on each axis, cm, and zero on the normal's own axis (the face
	 * is a rectangle, not a box). Zero on all three means no geometry was supplied: no known
	 * bending capacity, not a degenerate joint, since the area alone answers a centred load
	 * exactly. AddConnection rejects geometry that disagrees with the area, so an area with
	 * the wrong lever arm cannot be expressed.
	 */
	FVector InterfaceHalfExtentCm = FVector::ZeroVector;

	/** Directional strengths of the joint itself (mortar, nail, bolt). */
	FConnectionStrength Strength;

	/**
	 * Evaluate a world-space force against this joint and return its utilisation.
	 *
	 * Latches: once utilisation exceeds 1 the joint has given and stays given. The call that
	 * discovers the failure returns the utilisation that broke it; later calls return zero,
	 * since a given joint carries nothing.
	 *
	 * Moment and composite depth are the same parameters UtilisationUnder takes, defaulted
	 * the same way, so the break decision and the strain readout stay consistent. Without the
	 * moment, an eccentric joint at 1.25 capacity would never break; without the
	 * composite-depth relief, a joint the readout shows at 0.37 would break at 22.9.
	 *
	 * @param MomentUuCm       Bending moment about this joint's centroid, uu.cm.
	 * @param CompositeDepthCm Depth of bonded masonry standing over this joint, cm.
	 */
	double ApplyForce(
		const FVector& Force,
		const FVector& MomentUuCm = FVector::ZeroVector,
		double CompositeDepthCm = 0.0);

	/**
	 * Utilisation this force would produce, without latching or mutating.
	 *
	 * Read-only counterpart to ApplyForce, used by the strain readout each frame. Ignores the
	 * latch, so a given joint still reports what a fresh joint of the same geometry would;
	 * latching lives only in ApplyForce. A caller that needs the joint's live state calls
	 * HasGiven.
	 *
	 * The moment is a world-space vector the joint resolves itself, like the force: the caller
	 * passes (p - c) x F about this centroid and nothing more. Torsion (the component about the
	 * normal) is dropped: it needs a polar modulus this face lacks and is second-order for
	 * gravity on a rectangular joint (MOMENTS_DESIGN.md).
	 *
	 * Composite depth is a length the joint pairs with its own rectangle. Masonry standing over
	 * a lost support resists overturning as a deep beam, t*D^2/6, and the joint gives at
	 * whichever of that and its own bed patch is smaller. The depth is a graph fact only the
	 * caller knows; which extent is t is an interface fact only the joint knows. See
	 * ComputeUtilisation for why the relief is a min with no axial term, and ARCHING_DESIGN.md
	 * slice 5.
	 *
	 * Both trailing parameters default to zero, which is exact, not a tolerance: an unmeasured
	 * or centred load gives the same answer as before moments existed, the way zero friction
	 * reduces Mohr-Coulomb exactly.
	 *
	 * @param MomentUuCm       Bending moment about this joint's centroid, uu.cm.
	 * @param CompositeDepthCm Vertical depth of bonded masonry over this joint, cm. Zero,
	 *                         negative, and non-finite all mean no relief.
	 */
	double UtilisationUnder(
		const FVector& Force,
		const FVector& MomentUuCm = FVector::ZeroVector,
		double CompositeDepthCm = 0.0) const;

	/**
	 * Fraction of a moment this joint may keep before its thrust line leaves the kern:
	 * k = min(1, |sigma_n| / sigma_b), and exactly 1 when there is nothing to relieve.
	 *
	 * This is the arching rule, and only half of it. A brick that has lost one seat but still
	 * abuts a neighbour reaching the ground arches across the hole rather than cantilevering:
	 * the abutment supplies a horizontal thrust and the thrust line sits at the kern edge.
	 * Scaled by k the joint reads peak tension of exactly zero and peak compression of exactly
	 * 2|sigma_n|, twice what deleting the moment would give. See ARCHING_DESIGN.md.
	 *
	 * This call only decides what one joint can see: that the normal force is compressive and
	 * the resultant is outside the kern. Whether the joint is a springing at all is a graph
	 * fact SolveLoads supplies; applying this without it caps head joints and corbels too,
	 * deleting MOMENTS_DESIGN case (b).
	 *
	 * Returns 1.0 exactly where it does not apply, so a structure with no arch is unchanged. No
	 * conversion boundary is crossed: the result is a ratio of two uu/cm2 stresses, so the
	 * megapascals cancel. Degenerate input fails closed to no relief: a joint that cannot locate
	 * its kern keeps its whole moment and reads as heavily loaded.
	 *
	 * @param Force      The force this joint carries, oriented as ClassifyForce requires: the
	 *                   force acting on the piece the normal points toward.
	 * @param MomentUuCm Bending moment about this joint's centroid, uu.cm. Only each in-plane
	 *                   component's magnitude is read, so either end gives the same answer.
	 * @return k in (0, 1], and 1 exactly when no relief is available or warranted.
	 */
	double ArchingMomentScale(const FVector& Force, const FVector& MomentUuCm) const;

	/**
	 * Take this joint out of the structure without it having failed.
	 *
	 * A removed piece's joint did not break, but it is just as absent, so it answers HasGiven
	 * the same way: out of the tier decision, out of the load paths, carrying nothing. It
	 * records no reason; FStructure keeps that by leaving the break-pass stamp alone. Idempotent
	 * and monotonic: joints never heal.
	 */
	void Sever();

	/** Whether this joint has given. Never returns to false. */
	bool HasGiven() const;

private:
	bool bHasGiven = false;
};
