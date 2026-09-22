// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Connection.h"

// Joint-prefixed names, so unity builds do not collide with other file-local helpers.
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

	/** Section modulus of the deep beam of masonry over the joint, cm3: width*depth^2/6. ARCHING_DESIGN.md slice 5. */
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
	 * The joint's two in-plane axes and the modulus resisting each, shared by the stress and the
	 * arching cap so the pairing cannot drift. Requires a valid separation axis.
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
	// A given joint never revives, so collapse stays monotonic.
	if (bHasGiven)
	{
		return 0.0;
	}

	/*
	 * One copy of the arithmetic, so the break decision and the readout agree to the bit. Moment
	 * and depth must pass through, or a joint shown at 0.37 breaks at 22.9.
	 */
	const double Utilisation = UtilisationUnder(Force, MomentUuCm, CompositeDepthCm);

	// Exactly 1 holds. Written !(x <= 1.0) so a NaN latches as given (fail closed).
	if (!(Utilisation <= 1.0))
	{
		bHasGiven = true;
	}

	return Utilisation;
}

double FConnection::UtilisationUnder(
	const FVector& Force, const FVector& MomentUuCm, double CompositeDepthCm) const
{
	// The latch is not consulted; a given joint still answers what the force would do.

	/*
	 * A normal that will not normalise (zero or NaN) must read as failed, so a zero area is
	 * substituted to hit the fail-closed guard; otherwise ClassifyForce would return a healthy zero.
	 */
	FVector UnitNormal = InterfaceNormal;
	const double EffectiveAreaSqCm = UnitNormal.Normalize() ? InterfaceAreaSqCm : 0.0;

	FConnectionLoad Load = DestructionForce::ClassifyForce(Force, UnitNormal);
	FJointSection Section(EffectiveAreaSqCm);

	/*
	 * Resolve the moment onto the face. Torsion is dropped (no polar modulus; MOMENTS_DESIGN.md).
	 * With no separation axis the whole moment goes onto U against a zero modulus: fail closed.
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
		 * Masonry over the joint is a second section; the caller supplies its depth. Zero,
		 * negative or non-finite depth withholds the relief.
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
	// Every early exit is 1.0, the identity, so a no-arch structure is bit-identical.
	constexpr double NoRelief = 1.0;

	// No valid normal or area means no kern. Written !(x > 0.0) so a NaN area is caught.
	FVector UnitNormal = InterfaceNormal;

	if (!UnitNormal.Normalize() || !(InterfaceAreaSqCm > 0.0))
	{
		return NoRelief;
	}

	/*
	 * Only a compressed joint has a thrust line. This also avoids 0/0 for a massless piece,
	 * which would give NaN.
	 */
	const FConnectionLoad Load = DestructionForce::ClassifyForce(Force, UnitNormal);

	if (!(Load.Compression > 0.0))
	{
		return NoRelief;
	}

	// Same frame helpers as UtilisationUnder, since k caps the stress it computes.
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

	// uu/cm2. The in-plane axes add at the worst corner, as in ComputeUtilisation.
	const double BendingStress = FMath::Abs(MomentUuCm[Frame.AxisU]) / Frame.ModulusUCm3
		+ FMath::Abs(MomentUuCm[Frame.AxisV]) / Frame.ModulusVCm3;

	const double NormalStress = Load.Compression / InterfaceAreaSqCm;

	if (!FMath::IsFinite(BendingStress) || !FMath::IsFinite(NormalStress))
	{
		return NoRelief;
	}

	/*
	 * Resultant outside the kern: |M_u|/W_u + |M_v|/W_v > |N|/A (the rectangle's rhombic core).
	 * This enforces min(1, ...); without it an in-kern joint's bending would be inflated.
	 * Written !(a > b) so a NaN is caught.
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
