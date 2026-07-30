// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * A force resolved into the three load types a connection can experience,
 * measured relative to the connection's interface plane.
 *
 * At most one of Compression and Tension is non-zero: they are opposite
 * signs of the same axis. All values are magnitudes and are never negative.
 */
struct FConnectionLoad
{
	/** Force squeezing the two faces together, perpendicular to the interface. */
	double Compression = 0.0;

	/** Force pulling the two faces apart, perpendicular to the interface. */
	double Tension = 0.0;

	/** Force sliding the two faces past each other, parallel to the interface. */
	double Shear = 0.0;
};

namespace DestructionForce
{
	/**
	 * Resolve a force into compression, tension and shear relative to a connection.
	 *
	 * Classification is relative to the interface plane, NOT to world axes. The same
	 * downward gravity is compression on a horizontal joint and shear on a vertical
	 * one — which joint it is depends entirely on InterfaceNormal.
	 *
	 * ORIENTATION CONVENTION — callers must get this right.
	 *
	 * A connection owns ONE interface normal, fixed when the connection is made and
	 * pointing from one piece toward the other. Pass the force acting on the piece
	 * the normal points TOWARD. Under that pairing the sign is unambiguous: a force
	 * component along +Normal pulls the faces apart (tension), against it pushes them
	 * together (compression).
	 *
	 * A joint is in compression or tension as a fact about the joint, not about which
	 * piece you are looking from. Describing it from the other piece means flipping
	 * the normal AND taking the equal-and-opposite reaction force; do both and the
	 * classification is identical, which is the invariant that makes per-piece
	 * bookkeeping safe.
	 *
	 * Flipping the normal WITHOUT negating the force silently swaps compression and
	 * tension. That is a misuse, not a second opinion, and it is the likeliest way to
	 * get structures failing in tension where they should be crushing. Both properties
	 * are pinned down by ConnectionLoad.NormalOrientation.
	 *
	 * @param Force            The applied force vector, in world space. Unreal force
	 *                         units, where 1 N = 100 uu — see DESIGN.md §3.
	 * @param InterfaceNormal  Normal of the plane where the two pieces meet, pointing
	 *                         toward the piece Force acts on. Need not be unit length;
	 *                         it is normalised internally.
	 * @return The force split into its three load types.
	 */
	FConnectionLoad ClassifyForce(const FVector& Force, const FVector& InterfaceNormal);
}
