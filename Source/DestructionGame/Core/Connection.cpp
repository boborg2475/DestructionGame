// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/Connection.h"

double FConnection::ApplyForce(const FVector& Force)
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
	 */
	const double Utilisation = UtilisationUnder(Force);

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

double FConnection::UtilisationUnder(const FVector& Force) const
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

	const FConnectionLoad Load = DestructionForce::ClassifyForce(Force, UnitNormal);

	return DestructionForce::ComputeUtilisation(Load, Strength, EffectiveAreaSqCm);
}

void FConnection::Sever()
{
	bHasGiven = true;
}

bool FConnection::HasGiven() const
{
	return bHasGiven;
}
