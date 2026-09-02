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
		 * A NO-TENSION BED BEARING ECCENTRICALLY PAST ITS KERN DOES NOT FAIL AT THE KERN — it
		 * cracks and stands on the part of the face still in contact. A dry joint (f_t = 0) has
		 * no tension to carry, so the linear +-sigma_b picture is wrong the moment the resultant
		 * leaves the kern: the opened edge cannot pull, the contact shrinks to a triangular block
		 * of length L_c = 3(h/2 - e), and force balance over that block concentrates the squeezed
		 * fibre to sigma_max = 2N/(W*L_c) = 2*sigma_mean*h/(3(h/2 - e)). It is continuous with the
		 * linear regime at the kern (e = h/6 gives 2*sigma_mean, exactly the linear peak there) and
		 * climbs to infinity as the resultant nears the face (e -> h/2), so the joint fails only by
		 * CRUSHING that reduced contact or by the resultant leaving the face entirely. This is the
		 * same no-tension partial-contact model the LP oracle already uses below the router's cap;
		 * teaching it here lets a dry timber bearing stand on its reduced contact instead of the
		 * router severing it at the kern. See the header and DESIGN §3.
		 *
		 * ENTERED ONLY FOR A GENUINE NO-TENSION JOINT IN NET COMPRESSION WHOSE RESULTANT HAS LEFT
		 * THE KERN. f_t <= 0 is the dry joint; NormalStress < 0 is net compression (the only state
		 * a contact bearing can be in); PeakTensileStress > 0 is the resultant past the (rhombic)
		 * kern. A NaN on any of the three fails all three tests and skips the relief, leaving the
		 * joint its own (failed) kern reading — the expensive direction to be wrong in, so garbage
		 * stays conservative.
		 *
		 * THE TRIANGULAR BLOCK IS UNIAXIAL, SO EXACTLY ONE AXIS MAY BE CRACKED. An axis is past its
		 * OWN kern when its bending stress alone exceeds the mean compression (sigma_b_i > |sigma_n|,
		 * the per-axis form of M/W > N/A). When exactly one axis is cracked the contact is the
		 * triangular block this formula owns and the relief fires about that axis; the other axis is
		 * still inside its kern, so its bending is an ordinary linear compression added to the worst
		 * corner exactly as the biaxial peak everywhere else in this file adds. When BOTH axes are
		 * cracked the contact is a cut corner, and when NEITHER alone cracks yet the summed bending
		 * still opens the corner (the only other way to enter here) that corner tension is likewise a
		 * two-dimensional state — either way the 1-D formula does not apply and the joint is left
		 * FAIL-CLOSED, its positive peak tension reading Max against f_t = 0. A genuinely biaxial dry
		 * joint therefore stays conservative rather than being laundered into a bearing.
		 *
		 * PLACED BEFORE THE COMPOSITE RELIEF DELIBERATELY. A dry bearing may still carry a composite
		 * depth (masonry standing over it), so the block below could otherwise fire on the same
		 * joint; running the no-tension relief first and zeroing the peak tension leaves the
		 * composite guard's CompositeBendingStress < PeakTensileStress false, so the two never both
		 * apply. The no-tension contact model is the correct one for a dry bed and can only lower the
		 * reading, so taking precedence is safe.
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

			/*
			 * Exactly one cracked axis is the case the uniaxial block describes; both-cracked and
			 * corner-tension are left fail-closed above.
			 */
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
