// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * A force resolved into the three load types a connection can experience,
 * measured relative to the connection's interface plane.
 *
 * At most one of Compression and Tension is non-zero (opposite signs of the
 * same axis), and all values are magnitudes, never negative. That invariant
 * is about the RESULTANT only — the resulting STRESS can carry both peak
 * tension and peak compression at once once moments bend the joint.
 * ComputeUtilisation does that split downstream, from these fields plus the
 * section geometry.
 */
struct FConnectionLoad
{
	/** Force squeezing the two faces together, perpendicular to the interface. */
	double Compression = 0.0;

	/** Force pulling the two faces apart, perpendicular to the interface. */
	double Tension = 0.0;

	/** Force sliding the two faces past each other, parallel to the interface. */
	double Shear = 0.0;

	/**
	 * Bending moment about the two in-plane axes of the interface, uu.cm.
	 *
	 * Signed, unlike the three forces above: the sign says which edge is being
	 * opened, though only the magnitude reaches the stress. No new conversion
	 * boundary — length is cm, so M/W with W in cm3 is uu/cm2, the same quantity
	 * a force over an area already is, divided by the same ForceUnitsPerMPaSqCm.
	 *
	 * Zero is not a special case; it is a load path with no eccentricity, so the
	 * bending term vanishes and the joint reads exactly what it read before
	 * moments existed.
	 */
	double BendingMomentUUuCm = 0.0;
	double BendingMomentVUuCm = 0.0;
};

namespace DestructionForce
{
	/**
	 * Resolve a force into compression, tension and shear relative to a connection.
	 *
	 * Classification is relative to the interface plane, not to world axes: the
	 * same downward gravity is compression on a horizontal joint and shear on a
	 * vertical one, depending entirely on InterfaceNormal.
	 *
	 * ORIENTATION CONVENTION. A connection owns one interface normal, fixed when
	 * made and pointing from one piece toward the other; pass the force acting on
	 * the piece the normal points TOWARD. Under that pairing a force component
	 * along +Normal is tension, against it is compression — unambiguous. Compression
	 * or tension is a fact about the joint, not about which piece you look from:
	 * describing it from the other piece means flipping the normal AND taking the
	 * equal-and-opposite reaction force, and doing both gives the identical answer.
	 *
	 * Flipping the normal WITHOUT negating the force silently swaps compression and
	 * tension — a misuse, not a second opinion, and the likeliest way to get
	 * structures failing in tension where they should be crushing.
	 *
	 * @param Force            The applied force vector, world space, Unreal force
	 *                         units (1 N = 100 uu — see DESIGN.md §3).
	 * @param InterfaceNormal  Normal of the plane where the two pieces meet,
	 *                         pointing toward the piece Force acts on. Need not be
	 *                         unit length; normalised internally.
	 * @return The force split into its three load types.
	 */
	FConnectionLoad ClassifyForce(const FVector& Force, const FVector& InterfaceNormal);
}
