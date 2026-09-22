// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ConnectionLoad.h"

/**
 * Stress a connection carries before it gives, per load direction, in SI MPa so values match
 * published data. Connection types differ only in these numbers. The directions are independent:
 * masonry is strong in compression and weak in shear and tension.
 */
struct FConnectionStrength
{
	/** Resistance to being crushed, MPa. */
	double CompressiveStrengthMPa = 0.0;

	/**
	 * Shear resistance at zero normal stress (cohesion only), MPa. Friction adds to it under
	 * compression. Dry stone has none.
	 */
	double ShearCohesionMPa = 0.0;

	/** Resistance to being pulled apart, MPa. */
	double TensileStrengthMPa = 0.0;

	/**
	 * Coulomb friction coefficient (mu): shear capacity added per unit compressive stress. Zero
	 * gives independent axes, correct for mechanical fasteners.
	 */
	double FrictionCoefficient = 0.0;

	/**
	 * Ceiling on shear capacity however hard the joint is squeezed, MPa. Eurocode 6 truncates
	 * Mohr-Coulomb near 0.065 of unit compressive strength (1.3-2.0 MPa for clay brick); without
	 * it, joints low in a tall structure become unbreakable in shear. Defaults to unbounded rather
	 * than using zero for "no cap".
	 */
	double MaxShearStrengthMPa = TNumericLimits<double>::Max();
};

/**
 * Geometry of the face two pieces meet across: area for centred load, section moduli for
 * bending. Implicitly constructible from a bare area on purpose: zero moduli are harmless without
 * a moment, and a moment against a zero modulus fails closed.
 */
struct FJointSection
{
	/** Area of the face the two pieces meet across, cm2. */
	double AreaSqCm = 0.0;

	/** Section modulus about the first in-plane axis, cm3. */
	double SectionModulusUCm3 = 0.0;

	/** Section modulus about the second in-plane axis, cm3. */
	double SectionModulusVCm3 = 0.0;

	/**
	 * Section modulus of the bonded masonry standing over this joint acting as a deep beam, about
	 * the first in-plane axis, cm3: `t*D^2/6` on a vertical plane (ARCHING_DESIGN.md slice 5). A
	 * second limit state, not a bigger section. Eleven courses over a corbel give 11,627 cm3
	 * against the bed patch's 179.48. One course gives less than the patch, so it never helps.
	 * Zero means not measured (fail-closed): the joint reads its own patch.
	 */
	double CompositeSectionModulusUCm3 = 0.0;

	/** The same deep-beam section about the second in-plane axis, cm3. */
	double CompositeSectionModulusVCm3 = 0.0;

	FJointSection() = default;

	FJointSection(double InAreaSqCm)
		: AreaSqCm(InAreaSqCm)
	{
	}

	FJointSection(double InAreaSqCm, double InSectionModulusUCm3, double InSectionModulusVCm3)
		: AreaSqCm(InAreaSqCm)
		, SectionModulusUCm3(InSectionModulusUCm3)
		, SectionModulusVCm3(InSectionModulusVCm3)
	{
	}
};

namespace DestructionForce
{
	/**
	 * Force in Unreal units that loads 1 cm2 to 1 MPa: 1 N = 100 uu and 1 cm2 = 100 mm2, so
	 * 10000 uu. The single conversion boundary; open-coding it risks a 100x error that tuned
	 * thresholds hide.
	 */
	constexpr double ForceUnitsPerMPaSqCm = 10000.0;

	/**
	 * How close a connection is to failing: each direction's stress over its strength, worst
	 * governs. A ratio so it drives both readouts and the break decision.
	 *
	 * Shear capacity is Mohr-Coulomb, cohesion + FrictionCoefficient * compressive stress; tension
	 * gets no friction benefit, so a wall loses shear capacity as load above it is removed.
	 *
	 * An eccentric load also bends the joint; normal stresses are then read at the outer fibre:
	 *
	 *     sigma_n = (Tension - Compression) / A       signed, positive in tension
	 *     sigma_b = |M_u|/W_u + |M_v|/W_v             worst corner, so biaxial adds
	 *     peak tension     = max(0, sigma_n + sigma_b)
	 *     peak compression = max(0, sigma_b - sigma_n)
	 *
	 * Where a deep beam of masonry stands over the joint, the joint is read against whichever
	 * section opens it less:
	 *
	 *     sigma_c = |M_u|/W_cu + |M_v|/W_cv           the same moment, vertical section
	 *
	 *     while  sigma_c < max(0, sigma_n + sigma_b)  and the joint is not in tension:
	 *         peak tension     = sigma_c
	 *         peak compression = max(sigma_c, -sigma_n)
	 *
	 * This can only help. If the deep beam takes the moment the bed patch does not bend, so no
	 * axial term is subtracted from sigma_c, and the relief is refused in net tension. Shear is
	 * unchanged, and friction uses the mean compressive stress, never the bending term (the safe
	 * side; see the .cpp). Zero eccentricity gives exactly the pre-moments answer.
	 *
	 * Degenerate input fails closed: area <= 0, or a non-zero moment against a modulus <= 0, reads
	 * as failed (a division would give NaN, which compares false to `> 1.0`). A bad composite
	 * modulus is a missing relief, so the joint reads its own patch. Never NaN, always finite.
	 *
	 * @param Load     Force resolved by ClassifyForce, Unreal force units.
	 * @param Strength Directional strengths, MPa.
	 * @return 0 when unloaded, 1 exactly at the limit, above 1 when the joint gives.
	 */
	double ComputeUtilisation(
		const FConnectionLoad& Load,
		const FConnectionStrength& Strength,
		const FJointSection& Section);
}
