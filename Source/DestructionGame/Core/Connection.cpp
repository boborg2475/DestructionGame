// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Connection.h"

/*
 * Names here carry a Joint prefix: a unity build merges files into one translation unit,
 * so two identically-named file-local helpers would collide (as in Structure.cpp).
 */
namespace
{
	/**
	 * Elastic section modulus of the joint's rectangle, cm3, about the in-plane axis it is
	 * measured ALONG: W = (4/3)*HalfAlong*HalfAcross^2 (beam theory, I/c). The half-extent
	 * order matters and a swap is silent: a brick end face reads 72.2 vs 179.5 cm3, a 2.5x error.
	 */
	double JointSectionModulusCm3(double HalfAlongCm, double HalfAcrossCm)
	{
		return (4.0 / 3.0) * HalfAlongCm * HalfAcrossCm * HalfAcrossCm;
	}

	/**
	 * Elastic section modulus of the deep beam standing over the joint, cm3: W = width*depth^2/6
	 * at the depth of masonry above, versus the sibling above at the joint's own face depth.
	 * Same width half-extent, so the two differ only in depth. ARCHING_DESIGN.md slice 5.
	 */
	double JointCompositeModulusCm3(double HalfAlongCm, double DepthCm)
	{
		return (2.0 * HalfAlongCm) * DepthCm * DepthCm / 6.0;
	}

	/**
	 * The world axis this joint separates on, or INDEX_NONE if its normal names none. A tilted
	 * normal has two in-plane candidates and choosing would silently pick a modulus, so it fails
	 * closed (as AddConnection does). Reads the raw normal: a millionth off an axis is intent, not rounding.
	 */
	int32 JointSeparationAxis(const FVector& InterfaceNormal)
	{
		int32 SeparationAxis = INDEX_NONE;
		int32 AxisCount = 0;

		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (InterfaceNormal[Axis] != 0.0)
			{
				SeparationAxis = Axis;
				++AxisCount;
			}
		}

		return AxisCount == 1 ? SeparationAxis : INDEX_NONE;
	}

	/**
	 * The two in-plane axes of a joint and the section modulus resisting each. Shared by the
	 * stress evaluation and the arching cap so the pairing cannot drift between them: a moment
	 * about U is resisted by the depth on V, so the moduli read the extents in opposite orders.
	 * Caller must already have a separation axis.
	 */
	struct FJointBendingFrame
	{
		int32 AxisU = INDEX_NONE;
		int32 AxisV = INDEX_NONE;

		double ModulusUCm3 = 0.0;
		double ModulusVCm3 = 0.0;
	};

	FJointBendingFrame JointBendingFrame(int32 SeparationAxis, const FVector& InterfaceHalfExtentCm)
	{
		FJointBendingFrame Frame;

		Frame.AxisU = SeparationAxis == 0 ? 1 : 0;
		Frame.AxisV = SeparationAxis == 2 ? 1 : 2;

		Frame.ModulusUCm3 = JointSectionModulusCm3(
			InterfaceHalfExtentCm[Frame.AxisU], InterfaceHalfExtentCm[Frame.AxisV]);
		Frame.ModulusVCm3 = JointSectionModulusCm3(
			InterfaceHalfExtentCm[Frame.AxisV], InterfaceHalfExtentCm[Frame.AxisU]);

		return Frame;
	}
}

double FConnection::ApplyForce(
	const FVector& Force, const FVector& MomentUuCm, double CompositeDepthCm)
{
	/*
	 * A given joint carries nothing and never revives: mortar does not re-bond, and a joint
	 * that healed when the load dropped would make collapse non-monotonic.
	 */
	if (bHasGiven)
	{
		return 0.0;
	}

	/*
	 * One copy of the break arithmetic, in UtilisationUnder: a second copy differing in the
	 * last bit could make a joint at exactly 1.0 read over capacity. Moment and depth pass
	 * through so the break decision matches the readout — dropping either desyncs them (a
	 * joint drawn at 0.37 the cascade then snaps at 22.9).
	 */
	const double Utilisation = UtilisationUnder(Force, MomentUuCm, CompositeDepthCm);

	/*
	 * Above 1 the joint gives; exactly 1 holds. Written !(x <= 1.0) so a NaN latches as given
	 * rather than reading intact — locally correct even though ComputeUtilisation never
	 * returns NaN today. See CURRENT_STATE.md.
	 */
	if (!(Utilisation <= 1.0))
	{
		bHasGiven = true;
	}

	return Utilisation;
}

double FConnection::UtilisationUnder(
	const FVector& Force, const FVector& MomentUuCm, double CompositeDepthCm) const
{
	/*
	 * The latch is not consulted: pure arithmetic on geometry, profile and force, so a given
	 * joint still answers what the force would have done. Latching stays in ApplyForce alone.
	 */

	/*
	 * A normal that will not normalise is no interface plane and must read as failed.
	 * ClassifyForce would return a clean zero load that ComputeUtilisation reads as healthy,
	 * so a zero area is substituted to route it through the fail-closed guard. Normalize
	 * returns false for zero-length and NaN alike (every NaN comparison is false).
	 */
	FVector UnitNormal = InterfaceNormal;
	const double EffectiveAreaSqCm = UnitNormal.Normalize() ? InterfaceAreaSqCm : 0.0;

	FConnectionLoad Load = DestructionForce::ClassifyForce(Force, UnitNormal);
	FJointSection Section(EffectiveAreaSqCm);

	/*
	 * The moment is resolved onto the face here, once. The component about the normal twists
	 * the joint and needs a polar modulus this rectangle lacks (MOMENTS_DESIGN.md, out of
	 * scope), so it is dropped; the two in-plane components pair with W_u and W_v, which read
	 * the extents in opposite orders and are NOT interchangeable. A normal with no separation
	 * axis fails closed: the whole moment goes onto U against a zero modulus.
	 */
	const int32 SeparationAxis = JointSeparationAxis(InterfaceNormal);

	if (SeparationAxis == INDEX_NONE)
	{
		Load.BendingMomentUUuCm = MomentUuCm.Size();
	}
	else
	{
		const FJointBendingFrame Frame = JointBendingFrame(SeparationAxis, InterfaceHalfExtentCm);

		Load.BendingMomentUUuCm = MomentUuCm[Frame.AxisU];
		Load.BendingMomentVUuCm = MomentUuCm[Frame.AxisV];

		Section.SectionModulusUCm3 = Frame.ModulusUCm3;
		Section.SectionModulusVCm3 = Frame.ModulusVCm3;

		/*
		 * Masonry over the joint is a second section for the same moment. The caller supplies
		 * the depth (a graph fact one joint cannot see); pairing it with the axes is done here.
		 * Zero, negative or non-finite depth leaves the section at zero and withholds relief.
		 */
		if (CompositeDepthCm > 0.0 && FMath::IsFinite(CompositeDepthCm))
		{
			Section.CompositeSectionModulusUCm3 = JointCompositeModulusCm3(
				InterfaceHalfExtentCm[Frame.AxisU], CompositeDepthCm);
			Section.CompositeSectionModulusVCm3 = JointCompositeModulusCm3(
				InterfaceHalfExtentCm[Frame.AxisV], CompositeDepthCm);
		}
	}

	return DestructionForce::ComputeUtilisation(Load, Strength, Section);
}

double FConnection::ArchingMomentScale(const FVector& Force, const FVector& MomentUuCm) const
{
	/*
	 * Every exit is 1.0, the identity not a default: relief multiplies a moment, so "cannot
	 * arch" and "nothing to relieve" coincide and a no-arch structure is bit-identical to before.
	 */
	constexpr double NoRelief = 1.0;

	/*
	 * No normalisable normal and no positive area both mean no kern for a thrust line.
	 * Written !(x > 0.0) so a NaN area lands inside the guard.
	 */
	FVector UnitNormal = InterfaceNormal;

	if (!UnitNormal.Normalize() || !(InterfaceAreaSqCm > 0.0))
	{
		return NoRelief;
	}

	/*
	 * Second gate: the normal force must be compressive. A joint in tension or carrying
	 * nothing has no thrust line, and this also keeps the arithmetic out of 0/0 (a massless
	 * piece has sigma_n and sigma_b both zero, and min(1, 0/0) is a NaN reading as failed).
	 */
	const FConnectionLoad Load = DestructionForce::ClassifyForce(Force, UnitNormal);

	if (!(Load.Compression > 0.0))
	{
		return NoRelief;
	}

	/*
	 * Frame and moduli resolved through the same helpers as UtilisationUnder, since k caps
	 * the stress that call computes. No separation axis or non-positive modulus means no relief.
	 */
	const int32 SeparationAxis = JointSeparationAxis(InterfaceNormal);

	if (SeparationAxis == INDEX_NONE)
	{
		return NoRelief;
	}

	const FJointBendingFrame Frame = JointBendingFrame(SeparationAxis, InterfaceHalfExtentCm);

	if (!(Frame.ModulusUCm3 > 0.0) || !(Frame.ModulusVCm3 > 0.0))
	{
		return NoRelief;
	}

	/*
	 * Both in uu/cm2; the in-plane axes add because the worst corner is worst on both at
	 * once — the same sigma_b ComputeUtilisation forms.
	 */
	const double BendingStress = FMath::Abs(MomentUuCm[Frame.AxisU]) / Frame.ModulusUCm3
		+ FMath::Abs(MomentUuCm[Frame.AxisV]) / Frame.ModulusVCm3;

	const double NormalStress = Load.Compression / InterfaceAreaSqCm;

	if (!FMath::IsFinite(BendingStress) || !FMath::IsFinite(NormalStress))
	{
		return NoRelief;
	}

	/*
	 * Third gate: the resultant is outside the kern. This comparison IS that statement:
	 * |M_u|/W_u + |M_v|/W_v > |N|/A, the rhombic core of a rectangle. Inside the kern the
	 * ratio would exceed one, so this is where min(1, ...) is enforced — dropping it inflates
	 * an ordinary off-centre joint's bending stress by nearly seven. Written !(a > b) so a
	 * surviving NaN lands here.
	 */
	if (!(BendingStress > NormalStress))
	{
		return NoRelief;
	}

	return NormalStress / BendingStress;
}

void FConnection::Sever()
{
	bHasGiven = true;
}

bool FConnection::HasGiven() const
{
	return bHasGiven;
}
