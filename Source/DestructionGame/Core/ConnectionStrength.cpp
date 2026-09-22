// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/ConnectionStrength.h"

namespace DestructionForce
{
	namespace
	{
		/** Stress carried by a force over an area, in MPa. */
		double StressMPa(double ForceUnits, double InterfaceAreaSqCm)
		{
			return ForceUnits / (InterfaceAreaSqCm * ForceUnitsPerMPaSqCm);
		}

		/**
		 * Outermost-fibre stress from a bending moment, in MPa. No new conversion: M/W in
		 * uu.cm / cm3 is uu/cm2, the same as force over area. Only the magnitude matters.
		 */
		double BendingStressMPa(double MomentUuCm, double SectionModulusCm3)
		{
			return FMath::Abs(MomentUuCm) / (SectionModulusCm3 * ForceUnitsPerMPaSqCm);
		}

		/**
		 * Stress over capacity for one axis. A zero-capacity axis (dry stone's tension and
		 * cohesion) is 0 when unstressed and failed when stressed, rather than 0/0 = NaN,
		 * which would read as intact.
		 */
		double AxisUtilisation(double Stress, double CapacityMPa)
		{
			/*
			 * A non-finite stress fails closed. FMath::Max is `(B < A) ? A : B`, so Max3 can
			 * launder a NaN into a finite number, and NaN > 1 is false.
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
		 * No interface area fails closed; a zero area would give NaN, which reads as intact.
		 * !(> 0) rather than <= 0 so a NaN area is caught too.
		 */
		if (!(InterfaceAreaSqCm > 0.0))
		{
			return TNumericLimits<double>::Max();
		}

		/*
		 * Test the moment before the modulus: a healthy joint often has no moment and no
		 * section about its second axis (0/0), which must contribute nothing. != 0.0 is true
		 * for NaN and infinity, so garbage lands inside the guard.
		 */
		const bool bBendsAboutU = Load.BendingMomentUUuCm != 0.0;
		const bool bBendsAboutV = Load.BendingMomentVUuCm != 0.0;

		// A moment with no section to resist it fails closed; !(> 0) also catches a NaN modulus.
		if ((bBendsAboutU && !(Section.SectionModulusUCm3 > 0.0))
			|| (bBendsAboutV && !(Section.SectionModulusVCm3 > 0.0)))
		{
			return TNumericLimits<double>::Max();
		}

		/*
		 * Biaxial bending adds: the corner fibre feels both. With no eccentricity this is
		 * zero and the result matches the averaged stresses exactly.
		 */
		const double BendingStress =
			(bBendsAboutU ? BendingStressMPa(Load.BendingMomentUUuCm, Section.SectionModulusUCm3) : 0.0)
			+ (bBendsAboutV ? BendingStressMPa(Load.BendingMomentVUuCm, Section.SectionModulusVCm3) : 0.0);

		// Mean normal stress, positive in tension; at most one of Compression and Tension is non-zero.
		const double NormalStress = StressMPa(Load.Tension - Load.Compression, InterfaceAreaSqCm);

		/*
		 * Edge stresses about the centroid, clamped at zero. Zero is the first argument on
		 * purpose: Max(0.0, NaN) passes the NaN on to be caught, Max(NaN, 0.0) would drop it.
		 */
		double PeakTensileStress = FMath::Max(0.0, NormalStress + BendingStress);
		double PeakCompressiveStress = FMath::Max(0.0, BendingStress - NormalStress);

		/*
		 * No-tension partial contact (DESIGN §3). A dry joint (f_t <= 0) in net compression
		 * whose resultant leaves the kern cracks and bears on a triangular block of length
		 * L_c = 3(h/2 - e), so sigma_max = 2*sigma_mean*h / (3(h/2 - e)). This equals the linear
		 * peak at the kern (e = h/6) and goes to infinity at the face. A NaN fails the entry
		 * tests, leaving the failed reading.
		 *
		 * The block is uniaxial, so the relief applies only when exactly one axis is cracked
		 * (sigma_b_i > |sigma_n|); the other axis adds linearly. Both cracked stays fail-closed.
		 * It runs before the composite relief and zeroes the peak tension, so the two never
		 * both apply.
		 */
		if (Strength.TensileStrengthMPa <= 0.0 && NormalStress < 0.0 && PeakTensileStress > 0.0)
		{
			const double MeanCompressiveMagnitude = -NormalStress;

			// Per-axis bending stresses; the moment flags keep an unbent axis at 0.0, not 0/0.
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

				// Depth from the rectangle identity h = 6*S/A.
				const double DepthHCm = 6.0 * CrackedModulusCm3 / InterfaceAreaSqCm;

				/*
				 * Only while the resultant is on the face: e < h/2, i.e. sigma_b < 3*|sigma_n|.
				 * The plain `<` makes NaN, or a resultant at or past the edge, keep the failed reading.
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
		 * Composite relief: the deep beam over the joint carries the same moment on a vertical
		 * section, so the joint gives at the lesser demand (see the header). Every bent axis
		 * must have a composite section, or mixing relieved and unrelieved axes understates the
		 * corner. Positive tests so a NaN or negative modulus gets no relief.
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
			 * Relief is the permissive branch, so the guard uses plain comparisons that are false
			 * on NaN (not FMath::Min, which can let a NaN through). Net tension gets no relief:
			 * a deep beam only speaks for a bed in compression or unloaded.
			 */
			if (NormalStress <= 0.0 && CompositeBendingStress < PeakTensileStress)
			{
				/*
				 * Relieve both edges: if the deep beam takes the moment, the bed carries only axial
				 * load. Relieving tension alone left a 45-course corbel reading 13.69 in compression
				 * instead of failing at 1.25 in tension. The worst compressive fibre is the larger of
				 * the bed's |sigma_n| and the beam's sigma_c, never their sum, so this only lowers
				 * the reading.
				 */
				PeakTensileStress = CompositeBendingStress;
				PeakCompressiveStress =
					FMath::Max(CompositeBendingStress, -NormalStress);
			}
		}

		const double MeanCompressiveStress = StressMPa(Load.Compression, InterfaceAreaSqCm);
		const double ShearStress = StressMPa(Load.Shear, InterfaceAreaSqCm);

		/*
		 * Mohr-Coulomb: cohesion plus friction on the mean (not peak) compressive stress, as
		 * EN 1996-1-1 §3.6.2 does; the peak would make a bent joint stronger in shear. Capped at
		 * the material's ceiling, or deep joints in tall structures become uncuttable.
		 */
		const double ShearCapacityMPa = FMath::Min(
			Strength.ShearCohesionMPa + Strength.FrictionCoefficient * MeanCompressiveStress,
			Strength.MaxShearStrengthMPa);

		/*
		 * The worst axis governs, each against its own capacity (why this sits in front of
		 * Chaos's single strain threshold). Normal axes use the outermost fibre, not the mean.
		 */
		return FMath::Max3(
			AxisUtilisation(PeakCompressiveStress, Strength.CompressiveStrengthMPa),
			AxisUtilisation(ShearStress, ShearCapacityMPa),
			AxisUtilisation(PeakTensileStress, Strength.TensileStrengthMPa));
	}
}
