// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Connection.h"

/*
 * Every name in here carries a Joint prefix, for the reason Structure.cpp's file-local
 * names carry a Solver one: an anonymous namespace is private to a translation unit
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
	 * Which half-extent is which is the whole of the arithmetic, and getting the pair
	 * backwards is silent: the two moduli of a brick's end face are 72.2 and 179.5
	 * cm3, so a swap is a factor of 2.5 on a joint that still reads plausible.
	 */
	double JointSectionModulusCm3(double HalfAlongCm, double HalfAcrossCm)
	{
		return (4.0 / 3.0) * HalfAlongCm * HalfAcrossCm * HalfAcrossCm;
	}

	/**
	 * Elastic section modulus of the DEEP BEAM standing over the joint, cm3, about the
	 * in-plane axis it is measured ALONG.
	 *
	 * The same rectangle formula, `W = width * depth^2 / 6`, at a different depth: the
	 * sibling above is that at the joint's own face depth, `2*HalfAcross`; this is it at
	 * the depth of masonry standing over the joint. The two nest exactly, so a joint with
	 * one course above it gets a shallower section than its own patch — no help, by
	 * arithmetic and not by a special case. ARCHING_DESIGN.md slice 5.
	 *
	 * The width is the same half-extent either way — a vertical plane resisting a moment
	 * about axis U is as wide as the wall is along U, same extent the patch's own W_u
	 * reads first — so the two moduli differ only in depth and the pairing cannot drift.
	 */
	double JointCompositeModulusCm3(double HalfAlongCm, double DepthCm)
	{
		return (2.0 * HalfAlongCm) * DepthCm * DepthCm / 6.0;
	}

	/**
	 * The world axis this joint separates on, or INDEX_NONE if its normal names none.
	 *
	 * The in-plane frame is "the two world axes that are not the separation axis", which
	 * only names a frame when there is one. A normal 40 degrees off vertical has two
	 * candidates, and choosing between them would silently pick a section modulus, so a
	 * tilted normal fails closed to INDEX_NONE — the same refusal FStructure::AddConnection
	 * applies to a rectangle on such a normal.
	 *
	 * The raw normal is read rather than the normalised one, so a non-unit (0, 0, 5) is
	 * still the same plane, and a normal a millionth off an axis was produced by somebody
	 * meaning something else, not by rounding an exact one.
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
	 * Written once because the pairing is easy to get wrong: a moment about axis U is
	 * resisted by the depth on axis V, so the moduli read the extents in opposite orders.
	 * Two callers need this — the stress evaluation and the arching cap that has to agree
	 * with it — and two transcriptions would agree until the day one of them did not.
	 *
	 * The caller must have a separation axis already: a normal naming none has no in-plane
	 * frame to choose, and choosing one anyway is the guess this deliberately cannot make.
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
	 * is answered before anything else is looked at. That makes the latch total:
	 * no later load, however large or malformed, can revive it. Mortar does not
	 * re-bond, and a joint that healed when the load dropped would make collapse
	 * non-monotonic.
	 */
	if (bHasGiven)
	{
		return 0.0;
	}

	/*
	 * The evaluation lives in UtilisationUnder and is not repeated here, so there
	 * is exactly one copy of the arithmetic the break decision is made on — a
	 * second copy agreeing to 1e-9 still differs in the last bit, enough to make
	 * a joint at exactly 1.0 report over capacity.
	 *
	 * The moment and depth pass straight through for the reason the force does:
	 * what a joint carries is the caller's to say, how it resolves onto the face
	 * is the joint's. Dropping the moment would break on a smaller load than
	 * every readout shows; dropping the depth is the mirror error — it is a
	 * relief, not a load, so omitting it makes the break decision STRICTER than
	 * the readout (a joint drawn at 0.37 of capacity the cascade then snaps at 22.9).
	 */
	const double Utilisation = UtilisationUnder(Force, MomentUuCm, CompositeDepthCm);

	/*
	 * Above 1 the joint gives; exactly 1 is fully loaded but still holding. The
	 * breaking call still returns the ratio that broke it, distinguishing "gave,
	 * at this strain" from "gave silently".
	 *
	 * Written !(x <= 1.0) rather than x > 1.0 so a NaN latches as given instead
	 * of reading as intact. ComputeUtilisation currently guarantees it never
	 * returns NaN, so the two forms are identical today — but this comparison
	 * decides whether a joint breaks, and being locally correct costs nothing
	 * rather than depending on a promise kept elsewhere. See CURRENT_STATE.md.
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
	 * ApplyForce alone, which is what lets ApplyForce be built out of this call.
	 */

	/*
	 * A normal that will not normalise describes no interface plane, so this is not a
	 * joint and must read as failed. ClassifyForce answers a degenerate normal with a
	 * zero load — correct in isolation — but that clean zero arrives at
	 * ComputeUtilisation as a legitimate "unloaded, perfectly healthy". Substituting a
	 * zero area instead routes the case through the guard that already fails closed.
	 *
	 * Normalize returns false for a zero-length and for a NaN normal alike, since every
	 * comparison against NaN is false. Not exhaustive — a normal whose components
	 * overflow the squared sum (around 1e154 each) normalises to true while leaving the
	 * vector at zero — but unreachable from any plausible geometry.
	 */
	FVector UnitNormal = InterfaceNormal;
	const double EffectiveAreaSqCm = UnitNormal.Normalize() ? InterfaceAreaSqCm : 0.0;

	FConnectionLoad Load = DestructionForce::ClassifyForce(Force, UnitNormal);
	FJointSection Section(EffectiveAreaSqCm);

	/*
	 * The moment is resolved onto the face the same way the force is, and this is the one
	 * place it happens. A moment about the joint's own normal twists it, which needs a
	 * polar modulus this rectangle does not carry (MOMENTS_DESIGN.md puts it out of
	 * scope); the other two components lever an edge open and are what the section
	 * resists. So the separation axis is read off, its component dropped, and the two
	 * in-plane components paired with the modulus that belongs to each — W_u and W_v read
	 * the extents in opposite orders and are NOT interchangeable.
	 *
	 * A normal with no separation axis fails closed rather than choosing a frame: the
	 * whole moment goes onto U against a modulus of zero, the case ComputeUtilisation
	 * already answers with Max, and Size() carries a NaN or infinite component through as
	 * one too. With no moment at all Size() is exactly zero, so a tilted geometry-free
	 * joint reads what it always has.
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
		 * The masonry standing over the joint is a second section for the same moment. How
		 * much of it there is comes from the caller — a fact about the graph one joint
		 * cannot see — but which extent goes with which axis is this object's business, so
		 * the depth arrives as a bare length and is paired here.
		 *
		 * Only a real depth buys anything: zero is the ordinary case (nobody measured one),
		 * and a negative or non-finite depth is refused the same way, leaving the section
		 * at zero and ComputeUtilisation withholding the relief.
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
	 * Every exit below is 1.0, the identity and not a default: the relief is a multiplier
	 * on a moment, so "this joint cannot arch" and "nothing to relieve" are the same
	 * answer, and a structure with no arch is bit-identical to one compiled before this
	 * existed.
	 */
	constexpr double NoRelief = 1.0;

	/*
	 * A normal that will not normalise describes no interface plane, and a non-positive
	 * area describes no face — neither has a kern for a thrust line to sit on.
	 * UtilisationUnder answers both by reading as failed, the same direction this takes.
	 *
	 * Written !(x > 0.0) rather than x <= 0.0 so a NaN area lands inside the guard.
	 */
	FVector UnitNormal = InterfaceNormal;

	if (!UnitNormal.Normalize() || !(InterfaceAreaSqCm > 0.0))
	{
		return NoRelief;
	}

	/*
	 * The second gate: the normal force is compressive. An arch is a thrust line, and a
	 * joint being pulled apart — or carrying nothing — has none for the abutment to push
	 * against. It also keeps the arithmetic below out of 0/0: a massless piece has sigma_n
	 * and sigma_b both exactly zero, and min(1, 0/0) is a NaN that would come back reading
	 * as a failed joint.
	 */
	const FConnectionLoad Load = DestructionForce::ClassifyForce(Force, UnitNormal);

	if (!(Load.Compression > 0.0))
	{
		return NoRelief;
	}

	/*
	 * The frame and the two moduli are resolved exactly as UtilisationUnder resolves them,
	 * through the same two helpers, since k has to cap the stress that call will go on to
	 * compute. Neither a normal with no separation axis nor a non-positive modulus may be
	 * relieved out of existence here.
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
	 * Both in uu/cm2; the two in-plane axes ADD because the worst corner is worst
	 * on both at once — the same sigma_b ComputeUtilisation forms, spelled the same way.
	 */
	const double BendingStress = FMath::Abs(MomentUuCm[Frame.AxisU]) / Frame.ModulusUCm3
		+ FMath::Abs(MomentUuCm[Frame.AxisV]) / Frame.ModulusVCm3;

	const double NormalStress = Load.Compression / InterfaceAreaSqCm;

	if (!FMath::IsFinite(BendingStress) || !FMath::IsFinite(NormalStress))
	{
		return NoRelief;
	}

	/*
	 * The third gate: the resultant is outside the kern — and this comparison IS that
	 * statement, not an approximation of it. For one axis, e > W/A rearranges term for
	 * term into M/W > N/A; across both it is the rhombic core of a rectangle,
	 * |M_u|/W_u + |M_v|/W_v > |N|/A. On the 10.25 cm deep bed patch of a half-seated
	 * brick that boundary sits at 1.7083 cm and the load arrives 5.625 cm out.
	 *
	 * Inside the kern no part of the face is opening, so the ratio below would be
	 * greater than one — this guard is where min(1, ...) is actually enforced. Dropping
	 * it multiplies the bending stress of an ordinary slightly-off-centre joint by
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
