// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ConnectionLoad.h"

/**
 * How much stress a connection can carry before it gives, per load direction.
 *
 * Stored in real SI megapascals so values stay checkable against published
 * material data. Materials and connection types differ only in these numbers —
 * adding one is data, never code.
 *
 * The three are deliberately independent: stone and concrete are strong in
 * compression and brittle in shear and tension, and collapsing them to a single
 * strength is exactly the behaviour Chaos gives out of the box and that this
 * project exists to improve on.
 */
struct FConnectionStrength
{
	/** Resistance to being crushed, MPa. */
	double CompressiveStrengthMPa = 0.0;

	/**
	 * Resistance to the faces sliding past each other at ZERO normal stress, MPa.
	 *
	 * This is the bond alone — cohesion, not the whole shear strength, because
	 * capacity also grows with how hard the joint is being squeezed. Mortar has
	 * real cohesion; dry stone has none and holds purely by friction.
	 */
	double ShearCohesionMPa = 0.0;

	/** Resistance to being pulled apart, MPa. */
	double TensileStrengthMPa = 0.0;

	/**
	 * Coulomb friction coefficient (mu): how much compressive stress on the joint
	 * adds to its shear capacity.
	 *
	 * Friction is why a squeezed joint resists sliding, and why a masonry wall
	 * grows weaker in shear as the load above it is removed. Zero reduces the
	 * model to independent axes exactly, which is the right answer for mechanical
	 * fasteners — a bolt does not care how hard the pieces are pressed together.
	 * That is what keeps connection types data rather than separate code paths.
	 */
	double FrictionCoefficient = 0.0;
};

namespace DestructionForce
{
	/**
	 * Force, in Unreal units, that loads one square centimetre to one megapascal.
	 *
	 * The single conversion boundary between Unreal's cm/kg/s world and the SI
	 * units strengths are published in: 1 N = 100 uu of force and 1 cm2 = 100 mm2,
	 * so 1 MPa (= 1 N/mm2) over 1 cm2 is 100 * 100 = 10000 uu. Keeping this in one
	 * named place is deliberate — scattered, it makes everything wrong by exactly
	 * 100x in a way tuned thresholds will happily hide.
	 */
	constexpr double ForceUnitsPerMPaSqCm = 10000.0;

	/**
	 * How close a connection is to failing under a given load.
	 *
	 * Each load direction is converted to stress and compared against its own
	 * strength; the worst of the three governs, because a joint fails on the axis
	 * that gives first. Returned as a ratio rather than a bool so it can drive the
	 * on-screen strain readouts as well as the break decision.
	 *
	 * Shear capacity is not fixed. Following Mohr-Coulomb it is
	 *
	 *     cohesion + FrictionCoefficient * compressive stress
	 *
	 * so squeezing a joint makes it harder to slide. Only COMPRESSION contributes;
	 * a joint in tension is being pulled open and gets no friction benefit at all.
	 * This is what makes a wall shed shear capacity as the load above it is removed,
	 * rather than each joint holding a fixed strength until its own limit is hit.
	 *
	 * @param Load             Force already resolved into compression, tension and
	 *                         shear by ClassifyForce. Unreal force units.
	 * @param Strength         The connection's directional strengths, in MPa.
	 * @param InterfaceAreaSqCm  Area of the face the two pieces meet across, cm2.
	 * @return 0 when unloaded, 1 exactly at the limit, above 1 when the joint gives.
	 */
	double ComputeUtilisation(
		const FConnectionLoad& Load,
		const FConnectionStrength& Strength,
		double InterfaceAreaSqCm);
}
