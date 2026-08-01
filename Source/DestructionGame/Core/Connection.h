// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/ConnectionLoad.h"
#include "Core/ConnectionStrength.h"

/**
 * A joint between two pieces: the thing that gives before a structure falls.
 *
 * It holds together the two halves of the pipeline that already exist — the
 * interface normal ClassifyForce needs, and the area and strength profile
 * ComputeUtilisation needs — so a caller supplies only a force.
 *
 * A plain struct on purpose: no UObject, no actor, no world. The load solver
 * built on top of it has to be testable without a ticking solver, and pieces are
 * therefore opaque integer handles rather than actor pointers — whoever owns the
 * pieces resolves them.
 */
struct FConnection
{
	/** Handle of the piece the interface normal points away from. */
	int32 PieceA = INDEX_NONE;

	/** Handle of the piece the interface normal points toward. */
	int32 PieceB = INDEX_NONE;

	/**
	 * Normal of the plane where the two pieces meet, pointing toward PieceB.
	 *
	 * See the orientation convention in ConnectionLoad.h: forces passed in must
	 * be the forces acting on PieceB. Flipping this without negating the force
	 * silently swaps compression and tension.
	 */
	FVector InterfaceNormal = FVector::ZAxisVector;

	/** Area of the face the two pieces meet across, cm2. */
	double InterfaceAreaSqCm = 0.0;

	/** Directional strengths of the joint itself — mortar, nail, bolt. */
	FConnectionStrength Strength;

	/**
	 * Evaluate a world-space force against this joint and return its utilisation.
	 *
	 * Latches: once utilisation exceeds 1 the joint has given and stays given.
	 * The call that discovers the failure reports the utilisation that broke it;
	 * every call after that returns zero, because a joint that has given is out
	 * of the structure and carries nothing.
	 */
	double ApplyForce(const FVector& Force);

	/**
	 * Take this joint out of the structure without it having failed.
	 *
	 * A joint whose piece was REMOVED did not snap — it was deleted — but it is just
	 * as absent, so it must answer HasGiven the same way: out of the tier decision,
	 * out of the load paths, carrying nothing. Nothing here records a reason, because
	 * the joint is not where that belongs; whoever severed it knows why, and
	 * FStructure keeps the record by leaving the break-pass stamp alone.
	 *
	 * Idempotent, and monotonic like the latch it sets: joints never heal.
	 */
	void Sever();

	/** Whether this joint has given. Never returns to false. */
	bool HasGiven() const;

private:
	bool bHasGiven = false;
};
