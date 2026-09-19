// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ConnectionLoad.h"
#include "Core/ConnectionStrength.h"

/**
 * A joint between two pieces: the thing that gives before a structure falls.
 *
 * Holds the interface normal ClassifyForce needs and the area and strength
 * ComputeUtilisation needs, so a caller supplies only a force. A plain struct
 * (no UObject/actor/world) so the load solver is testable without a ticking
 * world; pieces are opaque integer handles that whoever owns them resolves.
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
	 * See the orientation convention in ConnectionLoad.h: forces passed in must be
	 * the forces acting on PieceB. Flipping this without negating the force silently
	 * swaps compression and tension.
	 */
	FVector InterfaceNormal = FVector::ZAxisVector;

	/** Area of the face the two pieces meet across, cm2. */
	double InterfaceAreaSqCm = 0.0;

	/**
	 * Where the face sits in the world, cm; on the normal's own axis, the plane of
	 * the joint. Zero means "nobody supplied geometry", not the origin. See
	 * InterfaceHalfExtentCm.
	 */
	FVector InterfaceCentreCm = FVector::ZeroVector;

	/**
	 * Half the face's extent on each axis, cm — zero on the normal's own axis (the
	 * face is a rectangle, not a box). Zero on all three means no geometry supplied:
	 * no known bending capacity, not a degenerate joint, since the area alone answers
	 * a centred load exactly. AddConnection rejects geometry that disagrees with the
	 * area, so a plausible area with the wrong lever arm is inexpressible.
	 */
	FVector InterfaceHalfExtentCm = FVector::ZeroVector;

	/** Directional strengths of the joint itself — mortar, nail, bolt. */
	FConnectionStrength Strength;

	/**
	 * Evaluate a world-space force against this joint and return its utilisation.
	 *
	 * Latches: once utilisation exceeds 1 the joint has given and stays given. The
	 * discovering call reports the utilisation that broke it; every call after returns
	 * zero, since a given joint carries nothing.
	 *
	 * Moment and composite depth are the same parameters UtilisationUnder takes,
	 * defaulted the same way — this call is that query plus the latch, so the break
	 * decision and the strain readout can never disagree. Deciding on the force alone
	 * would drift: without the moment a joint at 1.25 capacity holds forever; without
	 * the composite-depth relief a joint the readout shows at 0.37 snaps at 22.9.
	 *
	 * @param MomentUuCm       Bending moment about this joint's centroid, uu.cm.
	 * @param CompositeDepthCm How deep the bonded masonry standing over this joint is.
	 */
	double ApplyForce(
		const FVector& Force,
		const FVector& MomentUuCm = FVector::ZeroVector,
		double CompositeDepthCm = 0.0);

	/**
	 * What this force WOULD do to this joint. Never latches, never mutates.
	 *
	 * The read-only half of ApplyForce, asked by the strain readout every frame (a
	 * display that called ApplyForce would take the wall apart by drawing it). Pure
	 * arithmetic: it ignores the latch, so a given joint still reports what a fresh
	 * joint of the same geometry would — latching stays known only in ApplyForce.
	 *
	 * The moment arrives as a world-space vector and the joint resolves it itself, as
	 * it does the force: the caller supplies (p - c) x F about this centroid and
	 * nothing more, since which components bend the face and which twist it is a fact
	 * about the interface. Torsion (the component about the normal) is dropped
	 * deliberately — it needs a polar modulus this face lacks and is second-order for
	 * gravity on a rectangular joint (MOMENTS_DESIGN.md).
	 *
	 * Composite depth is a length the joint pairs with its own rectangle: masonry
	 * standing over a lost support resists overturning as a deep beam, t*D^2/6, and
	 * the joint gives at whichever of that and its own bed patch reads less. How much
	 * is standing is a graph fact only the caller knows; which extent is t is an
	 * interface fact only the joint knows. See ComputeUtilisation for why the relief
	 * is a min with no axial term, and ARCHING_DESIGN.md slice 5.
	 *
	 * Both trailing params default to zero, which is exact, not a tolerance: an
	 * unmeasured or centred load gives bit-for-bit the pre-moments answer, the same
	 * way zero friction reduces Mohr-Coulomb exactly.
	 *
	 * @param MomentUuCm       Bending moment about this joint's centroid, uu.cm.
	 * @param CompositeDepthCm Vertical depth of bonded masonry over this joint, cm.
	 *                         Zero, negative and non-finite all mean no relief.
	 */
	double UtilisationUnder(
		const FVector& Force,
		const FVector& MomentUuCm = FVector::ZeroVector,
		double CompositeDepthCm = 0.0) const;

	/**
	 * How much of a moment this joint may keep before its thrust line leaves the kern:
	 * k = min(1, |sigma_n| / sigma_b), and exactly 1 when there is nothing to relieve.
	 *
	 * This is arching, and only half the rule. A brick that has lost one seat but still
	 * abuts a neighbour reaching the ground arches across the hole rather than
	 * cantilevering: the abutment supplies a horizontal thrust and the thrust line sits
	 * at the kern edge. Scaled by k the joint reads peak tension of exactly zero and
	 * peak compression of exactly 2|sigma_n| — what an arch is, and twice what deleting
	 * the moment would read. See ARCHING_DESIGN.md.
	 *
	 * This call decides only what one joint can see: that the normal force is
	 * compressive and the resultant is outside the kern. Whether the joint is a
	 * springing at all is a graph fact SolveLoads asks; a caller applying this without
	 * it caps head joints and corbels too, deleting MOMENTS_DESIGN case (b).
	 *
	 * Inert where it does not apply: returns 1.0 exactly, so a structure with no arch is
	 * bit-identical. No conversion boundary is crossed — the answer is a ratio of two
	 * uu/cm2 stresses, so the megapascals cancel. Degenerate input fails closed to no
	 * relief: a joint that cannot locate its kern keeps its whole moment and reads
	 * heavily loaded, the safe direction.
	 *
	 * @param Force      The force this joint carries, oriented as ClassifyForce requires:
	 *                   the force acting on the piece the normal points toward.
	 * @param MomentUuCm Bending moment about this joint's centroid, uu.cm. Only each
	 *                   in-plane component's magnitude is read, so either end agrees.
	 * @return k in (0, 1] — 1 exactly when no relief is available or warranted.
	 */
	double ArchingMomentScale(const FVector& Force, const FVector& MomentUuCm) const;

	/**
	 * Take this joint out of the structure without it having failed.
	 *
	 * A removed piece's joint did not snap — it was deleted — but it is just as absent,
	 * so it answers HasGiven the same way: out of the tier decision, out of the load
	 * paths, carrying nothing. It records no reason; FStructure keeps that by leaving
	 * the break-pass stamp alone. Idempotent and monotonic: joints never heal.
	 */
	void Sever();

	/** Whether this joint has given. Never returns to false. */
	bool HasGiven() const;

private:
	bool bHasGiven = false;
};
