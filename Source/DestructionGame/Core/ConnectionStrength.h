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
	 * Friction cannot help forever. Real Mohr-Coulomb envelopes are truncated,
	 * because past a point the material itself gives rather than the joint
	 * sliding, and the masonry codes cap the figure accordingly — Eurocode 6
	 * limits it to roughly 0.065 of the unit's compressive strength, which lands
	 * near 1.3-2.0 MPa for clay brick.
	 *
	 * Without a cap, joints deep inside a tall structure become effectively
	 * unbreakable in shear, which is backwards: the base of a building is exactly
	 * where cutting it should work.
	 *
	 * Defaults to unbounded so profiles that have no meaningful ceiling are
	 * unaffected, rather than making zero mean "no cap" and inviting an
	 * accidentally rigid joint.
	 */
	double MaxShearStrengthMPa = TNumericLimits<double>::Max();
};

/**
 * The geometry of the face two pieces meet across: how much of it there is, and
 * how far the outermost fibre sits from its centroid on each in-plane axis.
 *
 * The area alone answers a centred load; a moment also needs to know how much
 * leverage the section has against it, which is what a section modulus is.
 *
 * IMPLICITLY CONSTRUCTIBLE FROM A BARE AREA on purpose. A caller with no
 * geometry gets zero moduli, which is exactly right: with no moment there is no
 * bending term to divide, and with a moment a zero modulus fails closed.
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
	 * The section a DEEP BEAM of bonded masonry standing over this joint resists the same
	 * moment with, about the first in-plane axis, cm3. Zero means none was measured.
	 *
	 * COMPOSITE VERTICAL ACTION, AND IT IS A SECOND LIMIT STATE RATHER THAN A BIGGER
	 * SECTION. A stack of courses over a lost support does not resist its overturning
	 * moment as a sequence of independent bed patches: the wall acts as a deep beam and
	 * the plane resisting the moment is a VERTICAL one through the masonry, `t*D^2/6` for
	 * a wall t thick and D deep. Eleven courses over a corbelled brick is 11,627 cm3
	 * against the bed patch's 179.48 — a factor of 65, and it is what decides whether a
	 * brick deleted at a free end takes the wall with it. ARCHING_DESIGN.md slice 5.
	 *
	 * THE TWO SECTIONS ARE THE SAME FORMULA, WHICH IS WHY THIS IS ONE MODEL AND NOT TWO.
	 * A rectangle's modulus is `width * depth^2 / 6` either way; the bed patch is that at
	 * the depth of the patch itself, and this is it at the depth of the masonry standing
	 * over the joint. So the two are nested and a wall with only one course over a joint
	 * gets no help, by arithmetic rather than by a special case.
	 *
	 * ZERO IS "NOBODY MEASURED ONE", NOT A DEGENERATE SECTION, and it is the fail-closed
	 * value: with no composite section the joint reads its own patch exactly as it did
	 * before this existed. Every geometry-free caller keeps supplying a bare area and
	 * keeps reading bit for bit what it always read.
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
	 * A load path that misses the joint's centroid also bends it, and the normal
	 * axes are then measured AT THE OUTERMOST FIBRE rather than averaged:
	 *
	 *     sigma_n = (Tension - Compression) / A       signed, positive in tension
	 *     sigma_b = |M_u|/W_u + |M_v|/W_v             worst corner, so biaxial adds
	 *     peak tension     = max(0, sigma_n + sigma_b)
	 *     peak compression = max(0, sigma_b - sigma_n)
	 *
	 * AND WHERE A DEEP BEAM OF MASONRY STANDS OVER THE JOINT, THAT IS A SECOND WAY OF
	 * CARRYING THE SAME MOMENT, so the joint is read against whichever of the two opens
	 * it less:
	 *
	 *     sigma_c = |M_u|/W_cu + |M_v|/W_cv           the same moment, vertical section
	 *
	 *     while  sigma_c < max(0, sigma_n + sigma_b)  and the joint is not in tension:
	 *         peak tension     = sigma_c
	 *         peak compression = max(sigma_c, -sigma_n)
	 *
	 * AN ALTERNATIVE PATH AND NOT AN EXTRA ONE — the structure finds the stiffer of the
	 * two, so this may only ever help, and it is what makes the two sections nested
	 * rather than a second model bolted on: one course of masonry over a joint is a
	 * SHALLOWER section than the bed patch, so the composite reading is worse there and
	 * is discarded, and the joint keeps its own value verbatim rather than having it
	 * recomputed through another expression that agrees only to the last bit.
	 *
	 * BOTH EDGES MOVE TOGETHER OR NEITHER DOES. If the deep beam is what resists the
	 * moment then the bed patch is not bending at all, so what remains is TWO PLANES with
	 * one stress each: the bed plane under a uniform sigma_n, and the vertical plane
	 * under +-sigma_c. Relieving the opened edge while leaving the squeezed one at
	 * sigma_b + |sigma_n| reads a joint against a section that is no longer working —
	 * and it hides the whole mechanism, because a deep enough corbel then fails in
	 * COMPRESSION at a number the tension axis was supposed to decide.
	 *
	 * NO AXIAL TERM IS SUBTRACTED FROM sigma_c, and that is the model rather than an
	 * omission. The plane resisting a deep-beam moment is VERTICAL; the weight standing
	 * over the joint is shear on that plane rather than load across it, so there is no
	 * compression there to close it. Subtracting the bed patch's own sigma_n from a
	 * stress on a different plane reads a raking corbel as EXACTLY ZERO and stops bending
	 * governing anywhere at all.
	 *
	 * WHICH IS WHY THE RELIEF IS REFUSED WHILE THE JOINT IS IN NET TENSION. Pulling a bed
	 * joint apart is a demand on the bed plane that no amount of masonry standing over it
	 * answers, so composite action must not be allowed to delete it — the one direction
	 * this could fail open, and it is guarded rather than argued about.
	 *
	 * SHEAR IS UNTOUCHED, on both counts. The force sliding the bed plane is the same
	 * whichever section takes the moment, and the friction that resists it is bought by
	 * the MEAN compressive stress, which no bending term ever entered.
	 *
	 * The average is the wrong number as soon as the load path is eccentric, and it
	 * is wrong in the direction that leaves things standing — a brick hanging off a
	 * single head joint has a mean normal stress of exactly zero while the fibre at
	 * the top of that joint is being levered open. Shear is untouched, and its
	 * friction term keeps using the MEAN compressive stress; see the .cpp for why.
	 *
	 * Zero eccentricity is not a special case being tolerated, it is e = 0: the
	 * bending term vanishes and the answer is bit for bit what it was before moments
	 * existed, the same way a zero friction coefficient reduces Mohr-Coulomb exactly
	 * rather than approximately. That is what lets a caller with no geometry at all
	 * go on passing a bare area.
	 *
	 * @param Load             Force already resolved into compression, tension and
	 *                         shear by ClassifyForce. Unreal force units.
	 * @param Strength         The connection's directional strengths, in MPa.
	 * DEGENERATE INPUT — fails closed, deliberately.
	 *
	 * An interface area of zero or less is not a joint at all, so the result reads
	 * as failed rather than intact. The alternative is worse than it looks: dividing
	 * by a zero area yields NaN, NaN compares false against everything including
	 * `> 1.0`, and the joint would silently report itself as fine. Uninitialised
	 * areas would then hide until a structure mysteriously refused to collapse.
	 * Breaking loudly and immediately is the cheaper failure.
	 *
	 * A non-zero moment against a zero or negative section modulus fails closed the
	 * same way, for the same reason. NO moment against a zero modulus does not: a
	 * piece leans one way only, so a joint with no extent on its second axis and no
	 * moment about it either is perfectly healthy, not degenerate.
	 *
	 * A COMPOSITE MODULUS IS THE OTHER POLARITY, and deliberately so: it is a RELIEF
	 * rather than a capacity, so the fail-closed answer is to withhold it. A composite
	 * section that is missing, zero, negative or not finite on any axis the joint is
	 * actually bent about leaves the joint reading its own patch — heavily loaded, which
	 * is the expensive-to-be-wrong direction the guard has to land on.
	 *
	 * The return value is guaranteed never NaN and always finite, for any input.
	 *
	 * @param Section          Geometry of the face the two pieces meet across. A
	 *                         zero or negative area is treated as a failed joint, as
	 *                         is a zero or negative modulus about a bent axis.
	 * @return 0 when unloaded, 1 exactly at the limit, above 1 when the joint gives.
	 */
	double ComputeUtilisation(
		const FConnectionLoad& Load,
		const FConnectionStrength& Strength,
		const FJointSection& Section);
}
