// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/PieceInspection.h"

FPieceInspection InspectPiece(const FStructure& Structure, int32 PieceIndex)
{
	FPieceInspection Inspection;

	/*
	 * ONE QUESTION CLOSES EVERY FAIL-CLOSED CASE AT ONCE. IsPieceRemoved already answers
	 * true for a handle past the end, a negative one, INDEX_NONE and a piece the player
	 * pulled out, so a readout that asked its own range question would be a second, weaker
	 * copy of a rule that is already written down. Returning here rather than filling
	 * fields in is what keeps the stale support answer FStructure legitimately keeps for a
	 * removed piece from ever reaching a row: a solver accessor with a documented scope may
	 * hand it back, and a readout drawn beside a brick that is gone may not.
	 */
	if (Structure.IsPieceRemoved(PieceIndex))
	{
		return Inspection;
	}

	Inspection.bIsPiece = true;
	Inspection.PieceIndex = PieceIndex;
	Inspection.bHasSupportAnswer = Structure.HasSupportAnswer(PieceIndex);
	Inspection.Support = Structure.GetPieceSupport(PieceIndex);

	/*
	 * A SCAN IN CONNECTION ORDER, AND EVERY FIELD ON A ROW IS AN ACCESSOR CALL. Nothing
	 * here decides anything the solver has already decided — the tier is GetJointRole, the
	 * force is GetConnectionForce, the ratio is GetConnectionUtilisation, the state is the
	 * connection's own latch and stamp — because a second derivation agrees to nine decimal
	 * places forever and still differs in the last bit.
	 *
	 * The filter is raw connectivity rather than the solver's support lists, deliberately:
	 * those drop a joint that has GIVEN before the tier is even decided, and the joint a
	 * player just broke is precisely the one a breakout is being looked at for.
	 */
	for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
	{
		const FConnection& Connection = Structure.GetConnection(Index);

		if (Connection.PieceA != PieceIndex && Connection.PieceB != PieceIndex)
		{
			continue;
		}

		FJointInspection Joint;
		Joint.ConnectionIndex = Index;
		Joint.OtherPieceIndex =
			Connection.PieceA == PieceIndex ? Connection.PieceB : Connection.PieceA;
		Joint.Role = Structure.GetJointRole(Index, PieceIndex);
		Joint.ForceUu = Structure.GetConnectionForce(Index);
		Joint.Utilisation = Structure.GetConnectionUtilisation(Index);
		Joint.bHasGiven = Connection.HasGiven();
		Joint.BreakPass = Structure.GetBreakPass(Index);

		Inspection.Joints.Add(Joint);
	}

	return Inspection;
}

FPieceInspection InspectPiece(const FStructureBinding& Binding, const FPieceRef& Ref)
{
	/*
	 * A RESOLVE AND NOTHING ELSE. ResolvePiece already refuses a ref naming another
	 * structure, a ref carrying either default, an index outside the handle range and one
	 * naming a piece that has gone — answering INDEX_NONE, which the handle overload
	 * above fails closed on. A guard of its own here would be a second policy at a second
	 * door, and the two doors would eventually disagree about which bricks exist.
	 */
	return InspectPiece(Binding.GetStructure(), Binding.ResolvePiece(Ref));
}
