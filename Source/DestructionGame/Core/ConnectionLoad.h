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
	 * @param Force            The applied force vector, in world space.
	 * @param InterfaceNormal  Normal of the plane where the two pieces meet. Need not
	 *                         be unit length; it is normalised internally. A force
	 *                         component along +Normal pulls the faces apart (tension),
	 *                         against it pushes them together (compression).
	 * @return The force split into its three load types.
	 */
	FConnectionLoad ClassifyForce(const FVector& Force, const FVector& InterfaceNormal);
}
