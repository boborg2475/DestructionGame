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
	 * Elastic section modulus of the DEEP BEAM standing over the joint, cm3, about the
	 * in-plane axis it is measured ALONG.
	 *
	 * THE SAME RECTANGLE FORMULA AT A DIFFERENT DEPTH, and that is the whole of composite
	 * vertical action. `W = width * depth^2 / 6`: the sibling above is that at the depth of
	 * the joint's own face, `2*HalfAcross`, and this is it at the depth of the masonry
	 * standing over the joint. So the two models are nested exactly and a joint with one
	 * course above it gets a SHALLOWER section than its own patch — no help, by arithmetic
	 * and not by a special case. ARCHING_DESIGN.md slice 5.
	 *
	 * THE WIDTH IS THE SAME HALF-EXTENT EITHER WAY, and that is what pairs a composite
	 * modulus with the right moment. A vertical plane resisting a moment about axis U is
	 * as wide as the wall is ALONG U — the identical extent the patch's own W_u reads
	 * first — so the two moduli differ in the depth and in nothing else, and the pairing
	 * cannot drift between them.
	 */
	double JointCompositeModulusCm3(double HalfAlongCm, double DepthCm)
	{
		return (2.0 * HalfAlongCm) * DepthCm * DepthCm / 6.0;
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

	/**
	 * The two in-plane axes of a joint, and the section modulus each of them is resisted by.
	 *
	 * WRITTEN ONCE BECAUSE THE PAIRING IS WHAT IS EASY TO GET WRONG. A moment about axis U
	 * is resisted by the DEPTH on axis V, so the moduli read the extents in opposite orders;
	 * the two moduli of a brick's end face are 72.2 and 179.5 cm3, so a swap is a factor of
	 * 2.5 on a joint that goes on reading perfectly plausible. Two callers need this — the
	 * stress evaluation and the arching cap that has to agree with it — and two transcriptions
	 * of it would agree until the day one of them did not.
	 *
	 * The caller must have a separation axis already: a normal that names none has no
	 * in-plane frame to choose, and choosing one anyway is the guess this deliberately
	 * cannot make.
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
	 *
	 * AND THE DEPTH TRAVELS WITH THEM, for the mirror-image reason. It is a relief
	 * rather than a load, so omitting it makes the break decision STRICTER than the
	 * readout — a joint drawn at 0.37 of capacity that the cascade then snaps at 22.9,
	 * which is the same drift the moment parameter's own note describes, pointing the
	 * other way.
	 */
	const double Utilisation = UtilisationUnder(Force, MomentUuCm, CompositeDepthCm);

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

double FConnection::UtilisationUnder(
	const FVector& Force, const FVector& MomentUuCm, double CompositeDepthCm) const
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
		const FJointBendingFrame Frame = JointBendingFrame(SeparationAxis, InterfaceHalfExtentCm);

		Load.BendingMomentUUuCm = MomentUuCm[Frame.AxisU];
		Load.BendingMomentVUuCm = MomentUuCm[Frame.AxisV];

		Section.SectionModulusUCm3 = Frame.ModulusUCm3;
		Section.SectionModulusVCm3 = Frame.ModulusVCm3;

		/*
		 * AND THE MASONRY STANDING OVER THE JOINT IS A SECOND SECTION FOR THE SAME MOMENT.
		 * How much of it there is comes from the caller — it is a fact about the graph, and
		 * one joint cannot see it — but which extent goes with which axis is this object's
		 * business, exactly as it is for the patch's own two moduli, so the depth arrives as
		 * a bare LENGTH and is paired here. Handing in a modulus instead would put the
		 * pairing outside, where it becomes a second copy of the thing that is easy to swap.
		 *
		 * ONLY A REAL DEPTH BUYS ANYTHING. Zero is the ordinary case — nobody measured one —
		 * and a negative or non-finite depth is refused by the same positive test, which
		 * leaves the section at zero and ComputeUtilisation withholding the relief.
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
	 * EVERY EXIT BELOW IS 1.0, WHICH IS THE IDENTITY AND NOT A DEFAULT. The relief is a
	 * multiplier on a moment, so "this joint cannot arch" and "this joint has nothing to
	 * relieve" are the same answer, and a structure that contains no arch multiplies every
	 * moment it has by exactly one and is bit-identical to one compiled before this existed.
	 */
	constexpr double NoRelief = 1.0;

	/*
	 * A normal that will not normalise describes no interface plane, and an area that is
	 * not positive describes no face — neither has a kern for a thrust line to sit on.
	 * UtilisationUnder answers both by reading as failed, which is the same direction this
	 * takes: no relief, so whatever the joint is carrying it carries in full.
	 *
	 * Written !(x > 0.0) rather than x <= 0.0 so a NaN area lands inside the guard.
	 */
	FVector UnitNormal = InterfaceNormal;

	if (!UnitNormal.Normalize() || !(InterfaceAreaSqCm > 0.0))
	{
		return NoRelief;
	}

	/*
	 * THE SECOND GATE: THE NORMAL FORCE IS COMPRESSIVE. An arch is a thrust line, and a
	 * joint being pulled apart — or carrying nothing at all — has none for the abutment to
	 * push against. It is also what keeps the arithmetic below out of 0/0: a massless piece
	 * has sigma_n and sigma_b both exactly zero, and min(1, 0/0) is a NaN that would reach
	 * ComputeUtilisation as a bending moment and come back reading as a FAILED joint.
	 */
	const FConnectionLoad Load = DestructionForce::ClassifyForce(Force, UnitNormal);

	if (!(Load.Compression > 0.0))
	{
		return NoRelief;
	}

	/*
	 * The frame and the two moduli are resolved exactly as UtilisationUnder resolves them,
	 * through the same two helpers, because k has to be the cap on the stress THAT call
	 * will go on to compute. A normal naming no separation axis has no in-plane frame to
	 * choose, and a modulus that is not positive is the case ComputeUtilisation already
	 * fails closed on; neither may be relieved out of existence here.
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
	 * Both in uu/cm2, and the two in-plane axes ADD because the worst corner is worst on
	 * both at once — the same sigma_b ComputeUtilisation forms, spelled the same way.
	 */
	const double BendingStress = FMath::Abs(MomentUuCm[Frame.AxisU]) / Frame.ModulusUCm3
		+ FMath::Abs(MomentUuCm[Frame.AxisV]) / Frame.ModulusVCm3;

	const double NormalStress = Load.Compression / InterfaceAreaSqCm;

	if (!FMath::IsFinite(BendingStress) || !FMath::IsFinite(NormalStress))
	{
		return NoRelief;
	}

	/*
	 * THE THIRD GATE: THE RESULTANT IS OUTSIDE THE KERN — and this comparison IS that
	 * statement rather than an approximation of it. For one axis, e > W/A rearranges term
	 * for term into M/W > N/A, which is what is written; across both it is the rhombic core
	 * of a rectangle, |M_u|/W_u + |M_v|/W_v > |N|/A. On the 10.25 cm deep bed patch of a
	 * half-seated brick that boundary sits at 1.7083 cm and the load arrives 5.625 cm out.
	 *
	 * INSIDE THE KERN NO PART OF THE FACE IS OPENING, so there is nothing to relieve, and
	 * the ratio below would be GREATER than one — this guard is where min(1, ...) is
	 * actually enforced. Dropping it and scaling unconditionally does not merely fail to
	 * help, it multiplies the bending stress of an ordinary slightly-off-centre joint by
	 * nearly seven.
	 *
	 * Written !(a > b), so a NaN that survived the finiteness checks still lands here.
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
