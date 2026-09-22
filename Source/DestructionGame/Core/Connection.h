// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ConnectionLoad.h"
#include "Core/ConnectionStrength.h"

/**
 * A joint between two pieces. Holds the interface normal, area, and strength profile, so a
 * caller passes only a force. Plain struct (no UObject/world) to keep the load solver
 * testable; pieces are integer handles resolved by their owner.
 */
struct FConnection
{
	/** Handle of the piece the interface normal points away from. */
	int32 PieceA = INDEX_NONE;

	/** Handle of the piece the interface normal points toward. */
	int32 PieceB = INDEX_NONE;

	/**
	 * Normal of the meeting plane, pointing toward PieceB. Forces passed in must act on PieceB
	 * (see ConnectionLoad.h); flipping this without negating the force swaps compression and tension.
	 */
	FVector InterfaceNormal = FVector::ZAxisVector;

	/** Area of the shared face, cm2. */
	double InterfaceAreaSqCm = 0.0;

	/** Where the face sits in the world, cm. Zero means no geometry was supplied. */
	FVector InterfaceCentreCm = FVector::ZeroVector;

	/**
	 * Half-extents of the face, cm; zero on the normal's axis (a rectangle, not a box). All-zero
	 * means no geometry supplied, so no bending capacity is known (the area alone answers a centred
	 * load). AddConnection rejects geometry that disagrees with the area.
	 */
	FVector InterfaceHalfExtentCm = FVector::ZeroVector;

	/** Directional strengths of the joint (mortar, nail, bolt). */
	FConnectionStrength Strength;

	/**
	 * Evaluate a world-space force and return utilisation. Latches: once utilisation exceeds 1 the
	 * joint has given and stays given; the discovering call returns the breaking utilisation, later
	 * calls return zero. Moment and composite depth match UtilisationUnder's parameters so the break
	 * decision and the strain readout agree: without the moment an eccentric joint at 1.25 never
	 * breaks, without the depth relief a joint shown at 0.37 breaks at 22.9.
	 *
	 * @param MomentUuCm       Bending moment about the joint centroid, uu.cm.
	 * @param CompositeDepthCm Depth of bonded masonry over the joint, cm.
	 */
	double ApplyForce(
		const FVector& Force,
		const FVector& MomentUuCm = FVector::ZeroVector,
		double CompositeDepthCm = 0.0);

	/**
	 * Utilisation this force would produce, without latching or mutating. Read-only counterpart to
	 * ApplyForce, used by the strain readout. The moment is a world-space vector the joint resolves
	 * itself: the caller passes (p - c) x F. Torsion (the component about the normal) is dropped: no
	 * polar modulus, and second-order for gravity (MOMENTS_DESIGN.md). Composite depth is a length
	 * paired with the joint's rectangle: masonry over a lost support resists as a deep beam, t*D^2/6,
	 * and the joint gives at whichever of that and its bed patch is smaller (ARCHING_DESIGN.md slice
	 * 5). Both trailing params default to zero, which is exact: an unmeasured or centred load gives
	 * the pre-moments answer.
	 *
	 * @param MomentUuCm       Bending moment about the joint centroid, uu.cm.
	 * @param CompositeDepthCm Depth of bonded masonry over the joint, cm. Zero/negative/non-finite mean no relief.
	 */
	double UtilisationUnder(
		const FVector& Force,
		const FVector& MomentUuCm = FVector::ZeroVector,
		double CompositeDepthCm = 0.0) const;

	/**
	 * Moment fraction the joint keeps before its thrust line leaves the kern: k = min(1, |sigma_n| /
	 * sigma_b), and 1 when there is nothing to relieve. Models arching: a brick that lost one seat
	 * but abuts a grounded neighbour arches across the hole, thrust line at the kern edge, giving
	 * peak tension 0 and peak compression 2|sigma_n|. Decides only what one joint sees (compressive
	 * normal force, resultant outside the kern); whether the joint is a springing is SolveLoads'
	 * call, and applying this without that check wrongly caps head joints and corbels. Returns 1.0
	 * where it does not apply. No conversion boundary: the result is a stress ratio. Degenerate input
	 * fails closed to no relief.
	 *
	 * @param Force      Force on the joint, oriented as ClassifyForce requires (acting on the piece the normal points to).
	 * @param MomentUuCm Bending moment about the joint centroid, uu.cm; only in-plane magnitudes are read.
	 * @return k in (0, 1], and 1 when no relief is warranted.
	 */
	double ArchingMomentScale(const FVector& Force, const FVector& MomentUuCm) const;

	/**
	 * Take the joint out of the structure without failing it (its piece was removed). Answers
	 * HasGiven like a broken joint: out of the load paths, carrying nothing. Idempotent; joints
	 * never heal.
	 */
	void Sever();

	/** Whether the joint has given. Never returns to false. */
	bool HasGiven() const;

private:
	bool bHasGiven = false;
};
