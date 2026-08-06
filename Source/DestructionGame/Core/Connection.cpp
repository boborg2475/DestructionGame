// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Connection.h"

/*
 * EVERY NAME IN HERE CARRIES A Joint PREFIX, for the reason Structure.cpp's file-local
 * names carry a Solver one: an anonymous namespace is private to a TRANSLATION UNIT
 * rather than to a file, and a unity build merges many files into one — at which point
 * two identically-named file-local helpers in files that never refer to each other are a
 * hard compile error, decided by how UBT partitioned the blob that day.
 */
namespace
{
	/**
	 * Elastic section modulus of the joint's rectangle, cm3, about the in-plane axis
	 * it is measured ALONG.
	 *
	 * Ordinary beam theory rather than a code figure. For a face 2*HalfAlong wide and
	 * 2*HalfAcross deep, bent about the first axis so the stress varies along the
	 * second, I = (2*HalfAlong)*(2*HalfAcross)^3/12 and the outermost fibre sits at
	 * HalfAcross, so W = I/c = (4/3)*HalfAlong*HalfAcross^2.
	 *
	 * WHICH HALF-EXTENT IS WHICH IS THE WHOLE OF THE ARITHMETIC, and getting the pair
	 * the wrong way round is silent: the two moduli of a brick's end face are 72.2 and
	 * 179.5 cm3, so a swap is a factor of 2.5 on a joint that still reads perfectly
	 * plausible.
	 */
	double JointSectionModulusCm3(double HalfAlongCm, double HalfAcrossCm)
	{
		return (4.0 / 3.0) * HalfAlongCm * HalfAcrossCm * HalfAcrossCm;
	}

	/**
	 * The world axis this joint separates on, or INDEX_NONE if its normal names none.
	 *
	 * THE IN-PLANE FRAME IS "the two world axes that are not the separation axis", which
	 * only names a frame when there IS one. On a normal 40 degrees off vertical there are
	 * two candidates and choosing between them silently picks a section modulus, so a
	 * tilted normal is answered INDEX_NONE and the caller fails closed rather than
	 * guessing. FStructure::AddConnection refuses a rectangle on such a normal for the
	 * same reason and in the same words, so nothing that came through that door reaches
	 * here undecided.
	 *
	 * The RAW normal is read rather than the normalised one, so a non-unit (0, 0, 5) is
	 * still the same plane — and a normal a millionth off an axis was not produced by
	 * rounding an exact axis vector, it was produced by somebody meaning something else.
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
}

double FConnection::ApplyForce(const FVector& Force, const FVector& MomentUuCm)
{
	/*
	 * A joint that has given is out of the structure and carries nothing, so it
	 * is answered before anything else is even looked at. That is what makes the
	 * latch total: no later load, however large or however malformed, can revive
	 * it. Mortar does not re-bond, and a joint that healed itself when the load
	 * dropped would make collapse non-monotonic.
	 */
	if (bHasGiven)
	{
		return 0.0;
	}

	/*
	 * The evaluation itself lives in UtilisationUnder and is not repeated here.
	 * Breaking is that query plus the latch, so there is exactly one copy of the
	 * arithmetic the break decision is made on — a second copy agrees to 1e-9
	 * forever and still differs in the last bit, and one ulp of drift is enough
	 * to make joints sitting at exactly 1.0 report as over capacity.
	 *
	 * The moment is passed straight through for the same reason the force is: what
	 * a joint is being asked to carry is the caller's to say, and how it resolves
	 * onto the face is the joint's. Dropping it here would leave the decision made
	 * on a strictly smaller load than the one every readout is showing.
	 */
	const double Utilisation = UtilisationUnder(Force, MomentUuCm);

	/*
	 * Above 1 the joint gives; exactly 1 is fully loaded but still holding. The
	 * breaking call still returns the ratio that broke it rather than zero: that
	 * is the number a strain readout should show at the moment of failure, and it
	 * is what distinguishes "gave, at this strain" from "gave silently".
	 *
	 * Written !(x <= 1.0) rather than x > 1.0 so that a NaN latches as given
	 * instead of reading as intact. ComputeUtilisation currently guarantees it
	 * never returns NaN, so today the two forms are identical — but this is the
	 * one comparison that decides whether a joint breaks, and making it locally
	 * correct costs nothing rather than depending on a promise kept in another
	 * translation unit. See the coupling note in CURRENT_STATE.md.
	 */
	if (!(Utilisation <= 1.0))
	{
		bHasGiven = true;
	}

	return Utilisation;
}

double FConnection::UtilisationUnder(const FVector& Force, const FVector& MomentUuCm) const
{
	/*
	 * The latch is deliberately not consulted. This is pure arithmetic on the
	 * joint's geometry, its profile and the force, so a joint that has already
	 * given still answers what the force would have done to it — exactly what a
	 * fresh joint of the same shape reports. Knowing about latching stays in
	 * ApplyForce alone, which is what lets ApplyForce be built out of this call
	 * rather than beside it.
	 */

	/*
	 * A normal that will not normalise describes no interface plane at all, so
	 * this is not a joint and must read as failed.
	 *
	 * The hole only exists once the two halves are composed. ClassifyForce
	 * answers a degenerate normal with a ZERO load — correct in isolation, and
	 * documented there — but that clean zero arrives at ComputeUtilisation as a
	 * perfectly legitimate "unloaded, perfectly healthy", which neither its area
	 * guard nor its stress guard has any way to see through. Substituting a
	 * zero area routes the case through the guard that already fails closed
	 * rather than opening a second escape hatch beside it.
	 *
	 * Normalize returns false for a zero-length AND for a NaN normal — every
	 * comparison against NaN is false, so its length test rejects both — which
	 * is why one check covers both cases.
	 *
	 * Not exhaustive: a normal whose components are large enough to overflow the
	 * squared sum (around 1e154 each) normalises to true while leaving the vector
	 * at zero, which slips through. Unreachable from any plausible geometry, and
	 * noted only so the enumeration above is not mistaken for a complete set.
	 */
	FVector UnitNormal = InterfaceNormal;
	const double EffectiveAreaSqCm = UnitNormal.Normalize() ? InterfaceAreaSqCm : 0.0;

	FConnectionLoad Load = DestructionForce::ClassifyForce(Force, UnitNormal);
	FJointSection Section(EffectiveAreaSqCm);

	/*
	 * THE MOMENT IS RESOLVED ONTO THE FACE THE SAME WAY THE FORCE IS, and this is the
	 * one place it happens. A moment about the joint's own normal TWISTS it, which needs
	 * a polar modulus this rectangle does not carry and which MOMENTS_DESIGN.md puts out
	 * of scope; the other two components lever an edge open and are what the section
	 * resists. So the separation axis is read off, its component is dropped, and the two
	 * in-plane components are paired with the modulus that belongs to each.
	 *
	 * A moment about axis U is resisted by the depth on axis V, so W_u reads the extents
	 * in that order and W_v in the other. They are NOT interchangeable: the two moduli of
	 * a brick's end face differ by a factor of 2.5.
	 *
	 * A NORMAL WITH NO SEPARATION AXIS FAILS CLOSED RATHER THAN CHOOSING A FRAME. The
	 * whole moment goes onto U against a modulus of zero, which is the case
	 * ComputeUtilisation already answers with Max — and Size() carries a NaN or an
	 * infinite component through as one, so garbage lands there too. With no moment at
	 * all Size() is exactly zero, the branch costs nothing, and a tilted geometry-free
	 * joint reads what it has always read.
	 */
	const int32 SeparationAxis = JointSeparationAxis(InterfaceNormal);

	if (SeparationAxis == INDEX_NONE)
	{
		Load.BendingMomentUUuCm = MomentUuCm.Size();
	}
	else
	{
		const int32 AxisU = SeparationAxis == 0 ? 1 : 0;
		const int32 AxisV = SeparationAxis == 2 ? 1 : 2;

		Load.BendingMomentUUuCm = MomentUuCm[AxisU];
		Load.BendingMomentVUuCm = MomentUuCm[AxisV];

		Section.SectionModulusUCm3 = JointSectionModulusCm3(
			InterfaceHalfExtentCm[AxisU], InterfaceHalfExtentCm[AxisV]);
		Section.SectionModulusVCm3 = JointSectionModulusCm3(
			InterfaceHalfExtentCm[AxisV], InterfaceHalfExtentCm[AxisU]);
	}

	return DestructionForce::ComputeUtilisation(Load, Strength, Section);
}

void FConnection::Sever()
{
	bHasGiven = true;
}

bool FConnection::HasGiven() const
{
	return bHasGiven;
}
