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

	/**
	 * Ceiling on shear capacity however hard the joint is squeezed, MPa.
	 *
	 * Friction cannot help forever: real Mohr-Coulomb envelopes are truncated,
	 * since past a point the material itself gives rather than the joint
	 * sliding — Eurocode 6 caps it near 0.065 of the unit's compressive
	 * strength, roughly 1.3-2.0 MPa for clay brick. Without a cap, joints deep
	 * inside a tall structure become effectively unbreakable in shear, which is
	 * backwards: the base of a building is exactly where cutting it should work.
	 *
	 * Defaults to unbounded so profiles with no meaningful ceiling are
	 * unaffected, rather than making zero mean "no cap" and risking an
	 * accidentally rigid joint.
	 */
	double MaxShearStrengthMPa = TNumericLimits<double>::Max();
};

/**
 * The geometry of the face two pieces meet across: how much of it there is, and
 * how far the outermost fibre sits from its centroid on each in-plane axis.
 *
 * The area alone answers a centred load; a moment also needs the leverage the
 * section has against it, which is what a section modulus is.
 *
 * Implicitly constructible from a bare area on purpose: a caller with no
 * geometry gets zero moduli, which is exactly right — no moment means no
 * bending term to divide, and a moment against a zero modulus fails closed.
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
	 * The section a deep beam of bonded masonry standing over this joint resists the same
	 * moment with, about the first in-plane axis, cm3. Zero means none was measured.
	 *
	 * Composite vertical action is a second limit state rather than a bigger section. A
	 * stack of courses over a lost support does not resist its overturning moment as a
	 * sequence of independent bed patches: the wall acts as a deep beam, and the plane
	 * resisting the moment is a VERTICAL one through the masonry, `t*D^2/6` for a wall t
	 * thick and D deep. Eleven courses over a corbelled brick is 11,627 cm3 against the bed
	 * patch's 179.48 — a factor of 65, deciding whether a brick deleted at a free end takes
	 * the wall with it. ARCHING_DESIGN.md slice 5.
	 *
	 * The two sections are the same formula — `width * depth^2 / 6` — at different depths:
	 * the bed patch at its own depth, this at the depth of masonry standing over the joint.
	 * They nest, so a wall with only one course over a joint gets no help, by arithmetic
	 * rather than by a special case.
	 *
	 * Zero is "nobody measured one", not a degenerate section — the fail-closed value: with
	 * no composite section the joint reads its own patch exactly as before this existed.
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
	 * strength; the worst of the three governs, since a joint fails on the axis
	 * that gives first. Returned as a ratio rather than a bool so it can drive
	 * on-screen strain readouts as well as the break decision.
	 *
	 * Shear capacity is not fixed. Following Mohr-Coulomb it is
	 *
	 *     cohesion + FrictionCoefficient * compressive stress
	 *
	 * so squeezing a joint makes it harder to slide. Only compression
	 * contributes — tension gets no friction benefit — which is what makes a
	 * wall shed shear capacity as the load above it is removed, rather than
	 * each joint holding a fixed strength until its own limit is hit.
	 *
	 * A load path that misses the joint's centroid also bends it, and the
	 * normal axes are then measured at the outermost fibre rather than averaged:
	 *
	 *     sigma_n = (Tension - Compression) / A       signed, positive in tension
	 *     sigma_b = |M_u|/W_u + |M_v|/W_v             worst corner, so biaxial adds
	 *     peak tension     = max(0, sigma_n + sigma_b)
	 *     peak compression = max(0, sigma_b - sigma_n)
	 *
	 * Where a deep beam of masonry stands over the joint, that is a second way of
	 * carrying the same moment, so the joint is read against whichever of the two
	 * opens it less:
	 *
	 *     sigma_c = |M_u|/W_cu + |M_v|/W_cv           the same moment, vertical section
	 *
	 *     while  sigma_c < max(0, sigma_n + sigma_b)  and the joint is not in tension:
	 *         peak tension     = sigma_c
	 *         peak compression = max(sigma_c, -sigma_n)
	 *
	 * An alternative path, not an extra one — the structure takes the stiffer of
	 * the two, so this may only ever help, and the sections nest rather than
	 * bolting on a second model: one course of masonry over a joint gives a
	 * shallower section than the bed patch, so the composite reading is worse
	 * and discarded there.
	 *
	 * Both edges move together: if the deep beam resists the moment the bed
	 * patch is not bending at all, so the state is two planes with one stress
	 * each — the bed plane under uniform sigma_n, the vertical plane under
	 * +-sigma_c — and no axial term is subtracted from sigma_c, since the
	 * weight over the joint is shear on that vertical plane, not load across
	 * it. For the same reason the relief is refused in net tension: no masonry
	 * standing over a bed joint answers a demand to pull it apart. See the
	 * .cpp for the worked corbel number this protects against.
	 *
	 * Shear is untouched either way: the force sliding the bed plane is the
	 * same whichever section takes the moment, and the friction resisting it
	 * is bought by the MEAN compressive stress, never the bending term — the
	 * safe direction to be wrong in when the load path is eccentric; see the
	 * .cpp.
	 *
	 * Zero eccentricity is e = 0, not a tolerated special case: the bending
	 * term vanishes and the answer is bit for bit what it was before moments
	 * existed, letting a caller with no geometry at all go on passing a bare
	 * area.
	 *
	 * DEGENERATE INPUT fails closed. A zero-or-less interface area is not a
	 * joint, so the result reads as failed — the alternative is worse than it
	 * looks, since dividing by zero yields NaN, which compares false against
	 * everything including `> 1.0`. A non-zero moment against a zero or
	 * negative modulus fails closed the same way; no moment against a zero
	 * modulus does not, since a piece leaning one way only is healthy, not
	 * degenerate. A composite modulus is the other polarity — a relief, not a
	 * capacity — so one that is missing, zero, negative or non-finite on a
	 * bent axis leaves the joint reading its own patch. The return value is
	 * guaranteed never NaN and always finite, for any input.
	 *
	 * @param Load     Force already resolved into compression, tension and
	 *                 shear by ClassifyForce. Unreal force units.
	 * @param Strength The connection's directional strengths, in MPa.
	 * @param Section  Geometry of the face the two pieces meet across. A zero or
	 *                 negative area is treated as a failed joint, as is a zero
	 *                 or negative modulus about a bent axis.
	 * @return 0 when unloaded, 1 exactly at the limit, above 1 when the joint gives.
	 */
	double ComputeUtilisation(
		const FConnectionLoad& Load,
		const FConnectionStrength& Strength,
		const FJointSection& Section);
}
