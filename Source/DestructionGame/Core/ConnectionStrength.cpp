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
		 * Sibling of StressMPa, and deliberately the same shape, because THERE IS NO
		 * NEW CONVERSION BOUNDARY HERE — which is worth saying plainly, since
		 * "moments" sounds like it should introduce one. Length is cm, so a moment is
		 * uu.cm and M/W with W in cm3 is uu/cm2: the identical quantity a force over
		 * an area already is, divided by the identical named constant.
		 *
		 * Only the magnitude of the moment reaches the stress. The sign says which
		 * edge of the joint is being levered open, and the worst corner is the worst
		 * corner whichever way the piece leans.
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
		 * theoretical one — it genuinely has zero tensile strength and zero
		 * cohesion, so 0/0 would otherwise produce a NaN, and since NaN compares
		 * false against everything the joint would read as intact rather than
		 * failed. Failing open is the wrong direction to be wrong in.
		 */
		double AxisUtilisation(double Stress, double CapacityMPa)
		{
			/*
			 * Garbage in still has to fail closed. A NaN stress is worse than it
			 * looks: FMath::Max is `(B < A) ? A : B` (GenericPlatformMath.h) and
			 * every comparison against NaN is false, so Max DISCARDS a NaN in its
			 * FIRST argument and RETURNS one in its second. Max3 is
			 * Max(Max(A, B), C), so a NaN reaching the Max3 below is laundered
			 * whichever axis it lands on: a NaN compression is dropped and the
			 * answer becomes the larger of the other two, a NaN shear swallows the
			 * compression reading and the answer becomes the tension one, and a NaN
			 * tension comes straight back out — where it still reads as intact,
			 * because NaN > 1 is false. Every route ends in a confident number for
			 * an input nobody can interpret, which nothing downstream could detect.
			 * An infinite stress is genuinely failed.
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
		 * BRANCH ON THE MOMENT, NEVER ON THE MODULUS FIRST. A piece leans one way
		 * only, so a perfectly healthy joint routinely carries no moment about its
		 * second axis and has no extent there to resist one with either — 0/0, on the
		 * ordinary path, for a joint that is entirely fine. Testing the moment first
		 * makes that contribute nothing, while a joint genuinely being bent about an
		 * axis it has no section on fails closed. Reversing the two tests would turn
		 * every single-axis lean into a failed joint.
		 *
		 * Spelled as != 0.0 rather than as a magnitude test on purpose: it is true of
		 * a NaN or infinite moment as well as a real one, so garbage lands inside the
		 * guard instead of being waved through as "no bending".
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
		 * Biaxial bending is the WORST CORNER, so the two axes ADD rather than the
		 * larger governing: the fibre in the corner of the joint feels both leans at
		 * once. With no eccentricity both terms are zero and everything below
		 * collapses to the averaged stresses bit for bit — which is what lets every
		 * caller that has no geometry go on supplying none.
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
		 * ZERO IS THE FIRST ARGUMENT DELIBERATELY. Every comparison against NaN is
		 * false, so FMath::Max falls through to its SECOND argument — meaning
		 * Max(0.0, NaN) hands the NaN on to be caught downstream, while Max(NaN, 0.0)
		 * would silently discard it and leave a joint that was handed garbage
		 * reporting a confident zero.
		 */
		double PeakTensileStress = FMath::Max(0.0, NormalStress + BendingStress);
		double PeakCompressiveStress = FMath::Max(0.0, BendingStress - NormalStress);

		/*
		 * AND THE DEEP BEAM STANDING OVER THE JOINT CARRIES THE SAME MOMENT ON A VERTICAL
		 * SECTION, so the joint gives at the LESSER of the two demands. Composite action is an
		 * alternative path rather than an extra one; see the header for why the composite
		 * reading carries no axial term and why one course of masonry therefore helps nothing.
		 *
		 * EVERY AXIS THE JOINT IS BENT ABOUT MUST HAVE A COMPOSITE SECTION, or there is none:
		 * summing a relieved axis with an unrelieved one would understate the corner by
		 * whichever term was silently left out. Written as positive tests so a NaN or a
		 * negative modulus lands OUTSIDE the relief and the joint keeps its patch reading.
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
			 * THE COMPARISONS ARE BOTH WRITTEN SO THAT A NaN LEAVES THE JOINT UNRELIEVED, and
			 * here that means the plain spellings rather than the negated ones this codebase
			 * usually reaches for. Relief is the PERMISSIVE branch, so the guard has to be
			 * FALSE on garbage: a NaN composite stress fails `<` and a NaN normal stress fails
			 * `<=`, and either way the patch reading stands. FMath::Min would do the opposite —
			 * it is `(A < B) ? A : B` (GenericPlatformMath.h), so a NaN FIRST argument is
			 * silently discarded and a NaN SECOND one silently replaces a perfectly good
			 * answer. It also means both
			 * values below are known finite by the time the branch is entered, which is what
			 * entitles the Max inside it to be an ordinary Max.
			 *
			 * NET TENSION REFUSES THE RELIEF OUTRIGHT. sigma_n at or below zero is a bed plane
			 * in compression or carrying nothing, which is the only state a deep beam over it
			 * can speak for; a joint being pulled apart has a demand of its own that no masonry
			 * standing on it answers. Unreachable from gravity on a bed joint today — that is
			 * compression by construction — and guarded anyway, because it is the one way this
			 * could read a failed joint as intact.
			 */
			if (NormalStress <= 0.0 && CompositeBendingStress < PeakTensileStress)
			{
				/*
				 * BOTH EDGES MOVE TOGETHER, AND THEY HAVE TO. If the deep beam is what resists
				 * the moment then the bed patch is NOT bending — it carries the axial force and
				 * nothing else — so leaving its squeezed edge at sigma_b + |sigma_n| would read
				 * a joint against a section the same line has just said is not the one working.
				 * That is not a refinement: a forty-five course corbel relieved on one edge only
				 * reads 13.69 in COMPRESSION against the 1.25 in tension it is supposed to fail
				 * at, so the whole slice would be masked by the axis it does not touch.
				 *
				 * WHAT IS LEFT IS TWO PLANES, EACH WITH ONE STRESS ON IT: the bed plane under a
				 * uniform |sigma_n|, and the vertical plane under +-sigma_c. The worst squeezed
				 * fibre anywhere in that state is the larger of them, which is what is written —
				 * NOT their sum, because they are different planes and no fibre feels both.
				 *
				 * IT CAN ONLY EVER LOWER THE READING, which is what keeps every "worst joint in
				 * the wall" anchor in place. Composite governs only where sigma_c < sigma_b, so
				 * max(sigma_c, |sigma_n|) is at most max(sigma_b, |sigma_n|) and therefore at
				 * most the sigma_b + |sigma_n| it replaces.
				 */
				PeakTensileStress = CompositeBendingStress;
				PeakCompressiveStress =
					FMath::Max(CompositeBendingStress, -NormalStress);
			}
		}

		const double MeanCompressiveStress = StressMPa(Load.Compression, InterfaceAreaSqCm);
		const double ShearStress = StressMPa(Load.Shear, InterfaceAreaSqCm);

		/*
		 * Mohr-Coulomb: the bond, plus whatever friction the squeeze is worth.
		 * Only compression contributes — a joint being pulled open gains nothing,
		 * and FConnectionLoad guarantees Compression is zero whenever there is
		 * tension, so that falls out without a branch.
		 *
		 * FRICTION IS BOUGHT BY THE MEAN COMPRESSIVE STRESS, NOT THE PEAK, and that
		 * is a considered choice rather than an oversight. Using the peak could only
		 * ever make a bent joint look STRONGER in shear than an unbent one, which is
		 * the wrong direction to be wrong in; EN 1996-1-1 §3.6.2 defines
		 * f_vk = f_vk0 + 0.4*sigma_d with sigma_d an average; and the honest
		 * refinement — averaging over the part of the face still in contact — is a
		 * real improvement that deserves its own change rather than riding along.
		 *
		 * Truncated at the material's own ceiling, because friction cannot help
		 * forever: past a point the material gives rather than the faces sliding.
		 * Left unbounded, capacity would climb with depth and joints at the base
		 * of a tall structure would become effectively uncuttable.
		 */
		const double ShearCapacityMPa = FMath::Min(
			Strength.ShearCohesionMPa + Strength.FrictionCoefficient * MeanCompressiveStress,
			Strength.MaxShearStrengthMPa);

		/*
		 * A joint gives on whichever axis runs out first, so the worst governs.
		 * Each is measured against its own capacity — that separation is what makes
		 * stone crush-resistant but brittle in shear, and it is the whole reason
		 * this sits in front of Chaos's single strain threshold.
		 *
		 * The normal axes are measured at the outermost fibre rather than averaged,
		 * because the average is the wrong number the moment the load path misses the
		 * centroid, and it is wrong in the direction that leaves things standing.
		 */
		return FMath::Max3(
			AxisUtilisation(PeakCompressiveStress, Strength.CompressiveStrengthMPa),
			AxisUtilisation(ShearStress, ShearCapacityMPa),
			AxisUtilisation(PeakTensileStress, Strength.TensileStrengthMPa));
	}
}
