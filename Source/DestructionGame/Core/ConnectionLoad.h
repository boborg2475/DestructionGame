// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * A force resolved into compression, tension and shear relative to the interface plane. At most
 * one of Compression and Tension is non-zero; all are magnitudes. That holds for the resultant
 * only: with bending, the stress can have both peaks, which ComputeUtilisation splits out.
 */
struct FConnectionLoad
{
	/** Normal force pushing the faces together. */
	double Compression = 0.0;

	/** Normal force pulling the faces apart. */
	double Tension = 0.0;

	/** In-plane force sliding the faces. */
	double Shear = 0.0;

	/**
	 * Bending moment about the interface's two in-plane axes, uu.cm. Signed (which edge opens);
	 * only the magnitude reaches the stress. M/W is uu/cm2 like F/A, so the same
	 * ForceUnitsPerMPaSqCm applies. Zero means no eccentricity.
	 */
	double BendingMomentUUuCm = 0.0;
	double BendingMomentVUuCm = 0.0;
};

namespace DestructionForce
{
	/**
	 * Resolve a force into compression, tension and shear relative to the interface plane (so
	 * gravity is compression on a horizontal joint and shear on a vertical one).
	 *
	 * Pass the force acting on the piece the normal points toward: a component along +Normal is
	 * tension, against it compression. Viewing from the other piece means flipping the normal and
	 * negating the force. Flipping only the normal silently swaps compression and tension.
	 *
	 * @param Force            World-space force, Unreal units (1 N = 100 uu, DESIGN.md §3).
	 * @param InterfaceNormal  Points toward the piece Force acts on; normalised internally.
	 */
	FConnectionLoad ClassifyForce(const FVector& Force, const FVector& InterfaceNormal);
}
