// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ConnectionStrength.h"

namespace DestructionForce
{
	namespace
	{
		/**
		 * Stress carried by a force, in MPa.
		 *
		 * Stress rather than raw force is the point. The same force through half
		 * the area is twice as punishing, so a force-only comparison would let
		 * thin joints survive loads that should part them.
		 */
		double StressMPa(double ForceUnits, double InterfaceAreaSqCm)
		{
			return ForceUnits / (InterfaceAreaSqCm * ForceUnitsPerMPaSqCm);
		}

		/**
		 * Stress at the outermost fibre from a bending moment, in MPa.
		 *
		 * Sibling of StressMPa, deliberately the same shape: no new conversion
		 * boundary here, worth saying since "moments" sounds like it should
		 * introduce one. Length is cm, so a moment is uu.cm and M/W with W in cm3
		 * is uu/cm2 — the identical quantity a force over an area already is,
		 * divided by the identical named constant.
		 *
		 * Only the magnitude of the moment reaches the stress; the sign says which
		 * edge is being levered open, and the worst corner is the worst corner
		 * whichever way the piece leans.
		 */
		double BendingStressMPa(double MomentUuCm, double SectionModulusCm3)
		{
			return FMath::Abs(MomentUuCm) / (SectionModulusCm3 * ForceUnitsPerMPaSqCm);
		}

		/**
		 * How hard one axis is working: its stress over the capacity resisting it.
		 *
		 * An axis with no capacity is handled explicitly rather than left to divide
		 * by zero. Unstressed it contributes nothing; carrying anything at all it
		 * has already gone. Dry stone makes this a live case rather than a
		 * theoretical one — zero tensile strength and zero cohesion would
		 * otherwise produce 0/0, and since NaN compares false against everything
		 * the joint would read as intact rather than failed. Failing open is the
		 * wrong direction to be wrong in.
		 */
		double AxisUtilisation(double Stress, double CapacityMPa)
		{
			/*
			 * Garbage in still has to fail closed. A NaN stress is worse than it
			 * looks: FMath::Max is `(B < A) ? A : B` (GenericPlatformMath.h), and
			 * since every comparison against NaN is false, Max discards a NaN in
			 * its first argument but returns one in its second. Max3 below is
			 * Max(Max(A, B), C), so a NaN on any axis is laundered into a
			 * confident finite number whichever way it lands, and a tension NaN
			 * comes straight back out reading as intact (NaN > 1 is false). An
			 * infinite stress, by contrast, is genuinely failed.
			 */
			if (!FMath::IsFinite(Stress))
			{
				return TNumericLimits<double>::Max();
			}

			if (CapacityMPa > 0.0)
			{
				return Stress / CapacityMPa;
			}

			return Stress > 0.0 ? TNumericLimits<double>::Max() : 0.0;
		}
	}

	double ComputeUtilisation(
		const FConnectionLoad& Load,
		const FConnectionStrength& Strength,
		const FJointSection& Section)
	{
		const double InterfaceAreaSqCm = Section.AreaSqCm;

		/*
		 * Fail closed on a joint that has no interface to carry anything across.
		 * Dividing by a zero area produces NaN, and NaN compares false against
		 * everything — including the > 1 test for failure — so the joint would
		 * report itself intact. Written as !(> 0) rather than <= 0 so that a NaN
		 * area is caught by the same branch instead of slipping past it.
		 */
		if (!(InterfaceAreaSqCm > 0.0))
		{
			return TNumericLimits<double>::Max();
		}

		/*
		 * Branch on the moment, never the modulus, first. A piece leans one way
		 * only, so a healthy joint routinely carries no moment about its second
		 * axis and has no extent there either — 0/0 on the ordinary path.
		 * Testing the moment first makes that contribute nothing, while a joint
		 * genuinely bent about an axis with no section still fails closed;
		 * reversing the two tests would turn every single-axis lean into a
		 * failed joint.
		 *
		 * Spelled as != 0.0 rather than a magnitude test on purpose: it is true
		 * of a NaN or infinite moment too, so garbage lands inside the guard
		 * instead of being waved through as "no bending".
		 */
		const bool bBendsAboutU = Load.BendingMomentUUuCm != 0.0;
		const bool bBendsAboutV = Load.BendingMomentVUuCm != 0.0;

		/*
		 * A moment with nothing to resist it is the area guard's failure mode again,
		 * one axis further in, so it fails closed the same way and with the same
		 * sentinel. Written as !(> 0) rather than <= 0 so a NaN modulus is caught by
		 * the branch instead of slipping past it.
		 */
		if ((bBendsAboutU && !(Section.SectionModulusUCm3 > 0.0))
			|| (bBendsAboutV && !(Section.SectionModulusVCm3 > 0.0)))
		{
			return TNumericLimits<double>::Max();
		}

		/*
		 * Biaxial bending is the worst corner, so the two axes ADD rather than the
		 * larger governing: the fibre in the corner feels both leans at once. With
		 * no eccentricity both terms are zero and everything below collapses to
		 * the averaged stresses bit for bit, which is what lets a geometry-free
		 * caller go on supplying none.
		 */
		const double BendingStress =
			(bBendsAboutU ? BendingStressMPa(Load.BendingMomentUUuCm, Section.SectionModulusUCm3) : 0.0)
			+ (bBendsAboutV ? BendingStressMPa(Load.BendingMomentVUuCm, Section.SectionModulusVCm3) : 0.0);

		/*
		 * The averaged normal stress, signed, positive in tension. FConnectionLoad
		 * guarantees at most one of Compression and Tension is non-zero, so this is
		 * simply whichever one is loaded with its sign attached.
		 */
		const double NormalStress = StressMPa(Load.Tension - Load.Compression, InterfaceAreaSqCm);

		/*
		 * The joint pivots about its centroid, so one edge opens by exactly as much
		 * as the other closes. Clamped at zero rather than taken as a magnitude,
		 * because a face that stays entirely in compression is carrying no tension at
		 * all rather than a negative amount of it.
		 *
		 * Zero is the first argument deliberately. Every comparison against NaN is
		 * false, so FMath::Max falls through to its second argument — Max(0.0, NaN)
		 * hands the NaN on to be caught downstream, while Max(NaN, 0.0) would
		 * silently discard it and report a confident zero for garbage input.
		 */
		double PeakTensileStress = FMath::Max(0.0, NormalStress + BendingStress);
		double PeakCompressiveStress = FMath::Max(0.0, BendingStress - NormalStress);

		/*
		 * A no-tension bed bearing eccentrically past its kern does not fail at the kern — it
		 * cracks and stands on the part of the face still in contact. A dry joint (f_t = 0) has
		 * no tension to carry, so the linear +-sigma_b picture breaks once the resultant leaves
		 * the kern: the opened edge cannot pull, the contact shrinks to a triangular block of
		 * length L_c = 3(h/2 - e), and force balance over that block concentrates the squeezed
		 * fibre to sigma_max = 2N/(W*L_c) = 2*sigma_mean*h/(3(h/2 - e)) — continuous with the
		 * linear regime at the kern (e = h/6 gives 2*sigma_mean, the linear peak there) and
		 * climbing to infinity as the resultant nears the face (e -> h/2). Same no-tension
		 * partial-contact model the LP oracle already uses below the router's cap. See the
		 * header and DESIGN §3.
		 *
		 * Entered only for a genuine no-tension joint in net compression whose resultant has left
		 * the kern: f_t <= 0 (dry joint), NormalStress < 0 (net compression), PeakTensileStress > 0
		 * (past the rhombic kern). A NaN on any of the three fails all three tests and skips the
		 * relief, leaving the joint its own failed kern reading — the expensive direction to be
		 * wrong in.
		 *
		 * The triangular block is uniaxial, so exactly one axis may be cracked — past its own
		 * kern when its bending stress alone exceeds the mean compression (sigma_b_i > |sigma_n|,
		 * the per-axis form of M/W > N/A). With exactly one axis cracked the relief fires about
		 * that axis, the other staying ordinary linear compression added to the worst corner as
		 * elsewhere in this file; both-cracked or corner-tension states are two-dimensional, so
		 * the 1-D formula does not apply and the joint is left fail-closed against f_t = 0.
		 *
		 * Placed before the composite relief deliberately: zeroing the peak tension here first
		 * leaves the composite guard's CompositeBendingStress < PeakTensileStress false, so the
		 * two never both apply. The no-tension model can only lower the reading, so taking
		 * precedence is safe.
		 */
		if (Strength.TensileStrengthMPa <= 0.0 && NormalStress < 0.0 && PeakTensileStress > 0.0)
		{
			const double MeanCompressiveMagnitude = -NormalStress;

			/*
			 * Per-axis bending stresses, guarded by the moment flags so a non-bent axis with a zero
			 * modulus contributes a clean 0.0 rather than a 0/0 NaN. These sum to the BendingStress
			 * already formed above.
			 */
			const double BendingStressU = bBendsAboutU
				? BendingStressMPa(Load.BendingMomentUUuCm, Section.SectionModulusUCm3) : 0.0;
			const double BendingStressV = bBendsAboutV
				? BendingStressMPa(Load.BendingMomentVUuCm, Section.SectionModulusVCm3) : 0.0;

			const bool bCracksU = BendingStressU > MeanCompressiveMagnitude;
			const bool bCracksV = BendingStressV > MeanCompressiveMagnitude;

			if (bCracksU != bCracksV)
			{
				const double CrackedModulusCm3 =
					bCracksU ? Section.SectionModulusUCm3 : Section.SectionModulusVCm3;
				const double CrackedBendingStress = bCracksU ? BendingStressU : BendingStressV;
				const double OtherBendingStress = bCracksU ? BendingStressV : BendingStressU;

				/*
				 * Recover the cracked axis's depth from the rectangle identity h = 6*S/A. Its
				 * modulus was already proven positive by the moment/modulus guard above, so a
				 * non-positive or NaN depth means garbage — written as the positive test so it, and
				 * only it, gates the relief on.
				 */
				const double DepthHCm = 6.0 * CrackedModulusCm3 / InterfaceAreaSqCm;

				/*
				 * The relief applies only while the resultant is still on the face, e < h/2 —
				 * equivalently sigma_b < 3*|sigma_n|. Written as the plain `<` so a NaN bending
				 * stress fails the test and the joint keeps its (failed) kern reading, and so that a
				 * resultant AT or past the face edge falls through with its peak tension intact and
				 * fails against f_t = 0. Only when the contact genuinely survives do we swap the
				 * linear picture for the reduced-contact one.
				 */
				if (DepthHCm > 0.0 && CrackedBendingStress < 3.0 * MeanCompressiveMagnitude)
				{
					const double EccentricityCm =
						CrackedBendingStress * DepthHCm / (6.0 * MeanCompressiveMagnitude);

					PeakTensileStress = 0.0;
					PeakCompressiveStress =
						2.0 * MeanCompressiveMagnitude * DepthHCm
							/ (3.0 * (0.5 * DepthHCm - EccentricityCm))
						+ OtherBendingStress;
				}
			}
		}

		/*
		 * The deep beam standing over the joint carries the same moment on a vertical
		 * section, so the joint gives at the lesser of the two demands; see the header for
		 * why the composite reading carries no axial term.
		 *
		 * Every axis the joint is bent about must have a composite section, or there is
		 * none: summing a relieved axis with an unrelieved one would understate the corner
		 * by whichever term was left out. Written as positive tests so a NaN or negative
		 * modulus lands outside the relief and the joint keeps its patch reading.
		 */
		const bool bHasCompositeSection = (bBendsAboutU || bBendsAboutV)
			&& (!bBendsAboutU || Section.CompositeSectionModulusUCm3 > 0.0)
			&& (!bBendsAboutV || Section.CompositeSectionModulusVCm3 > 0.0);

		if (bHasCompositeSection)
		{
			const double CompositeBendingStress =
				(bBendsAboutU
					? BendingStressMPa(Load.BendingMomentUUuCm, Section.CompositeSectionModulusUCm3)
					: 0.0)
				+ (bBendsAboutV
					? BendingStressMPa(Load.BendingMomentVUuCm, Section.CompositeSectionModulusVCm3)
					: 0.0);

			/*
			 * Both comparisons are written so a NaN leaves the joint unrelieved — the plain
			 * spellings rather than the negated ones this codebase usually reaches for, since
			 * relief is the permissive branch and the guard has to be FALSE on garbage: a NaN
			 * composite stress fails `<` and a NaN normal stress fails `<=`. FMath::Min would do
			 * the opposite — `(A < B) ? A : B` (GenericPlatformMath.h) silently discards a NaN
			 * first argument and lets a NaN second one replace a good answer. This also
			 * guarantees both values are finite by the time the branch is entered, entitling the
			 * Max inside it to be an ordinary Max.
			 *
			 * Net tension refuses the relief outright: sigma_n at or below zero is a bed plane in
			 * compression or carrying nothing, the only state a deep beam over it can speak for.
			 * Unreachable from gravity on a bed joint today, and guarded anyway as the one way
			 * this could read a failed joint as intact.
			 */
			if (NormalStress <= 0.0 && CompositeBendingStress < PeakTensileStress)
			{
				/*
				 * Both edges move together, and they have to: if the deep beam resists the
				 * moment, the bed patch is not bending — it carries only the axial force — so
				 * leaving its squeezed edge at sigma_b + |sigma_n| would read the joint against a
				 * section that is not the one working. Not a refinement: a forty-five course
				 * corbel relieved on one edge only reads 13.69 in compression against the 1.25 in
				 * tension it is supposed to fail at, masking the whole slice.
				 *
				 * What is left is two planes, each with one stress: the bed plane under uniform
				 * |sigma_n|, the vertical plane under +-sigma_c. The worst squeezed fibre is the
				 * larger of them, not their sum, because no fibre feels both planes at once.
				 *
				 * This can only ever lower the reading — composite governs only where sigma_c <
				 * sigma_b, so max(sigma_c, |sigma_n|) is at most max(sigma_b, |sigma_n|), the
				 * sigma_b + |sigma_n| it replaces — which is what keeps every "worst joint in the
				 * wall" anchor in place.
				 */
				PeakTensileStress = CompositeBendingStress;
				PeakCompressiveStress =
					FMath::Max(CompositeBendingStress, -NormalStress);
			}
		}

		const double MeanCompressiveStress = StressMPa(Load.Compression, InterfaceAreaSqCm);
		const double ShearStress = StressMPa(Load.Shear, InterfaceAreaSqCm);

		/*
		 * Mohr-Coulomb: the bond, plus whatever friction the squeeze is worth. Only
		 * compression contributes — FConnectionLoad guarantees Compression is zero
		 * whenever there is tension, so that falls out without a branch.
		 *
		 * Friction is bought by the mean compressive stress, not the peak — a
		 * considered choice, since using the peak could only make a bent joint look
		 * stronger in shear than an unbent one, the wrong direction to be wrong in;
		 * EN 1996-1-1 §3.6.2 defines f_vk = f_vk0 + 0.4*sigma_d with sigma_d an
		 * average. Averaging over just the part of the face still in contact would
		 * be a real improvement, but deserves its own change rather than riding along.
		 *
		 * Truncated at the material's own ceiling, since friction cannot help
		 * forever: left unbounded, capacity would climb with depth and joints at
		 * the base of a tall structure would become effectively uncuttable.
		 */
		const double ShearCapacityMPa = FMath::Min(
			Strength.ShearCohesionMPa + Strength.FrictionCoefficient * MeanCompressiveStress,
			Strength.MaxShearStrengthMPa);

		/*
		 * A joint gives on whichever axis runs out first, so the worst governs. Each
		 * is measured against its own capacity — the separation that makes stone
		 * crush-resistant but brittle in shear, and the whole reason this sits in
		 * front of Chaos's single strain threshold. The normal axes are measured at
		 * the outermost fibre rather than averaged, since the average is wrong the
		 * moment the load path misses the centroid, and wrong in the direction that
		 * leaves things standing.
		 */
		return FMath::Max3(
			AxisUtilisation(PeakCompressiveStress, Strength.CompressiveStrengthMPa),
			AxisUtilisation(ShearStress, ShearCapacityMPa),
			AxisUtilisation(PeakTensileStress, Strength.TensileStrengthMPa));
	}
}
