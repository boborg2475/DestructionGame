// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/PieceSelection.h"

bool FPieceSelection::Toggle(const FPieceRef& Ref)
{
	/*
	 * A REF MISSING EITHER HALF NEVER GETS IN, and it is refused by NAME rather than by
	 * truthiness: structure 0 piece 0 is the first brick of the wall the game mode builds,
	 * so `if (!Ref.StructureId)` would silently refuse it forever. A click on the floor
	 * arrives as a wholly default ref, which is also what "we have no answer" looks like —
	 * and a selection is a command's target list, so it takes the cautious reading.
	 */
	if (Ref.StructureId == INDEX_NONE || Ref.PieceIndex == INDEX_NONE)
	{
		return false;
	}

	/*
	 * REMOVED BY VALUE, WHICH IS WHAT MAKES A SECOND CLICK A DESELECTION. The ref that
	 * comes back from the second click is a different object naming the same piece; an
	 * implementation comparing anything else removes nothing and the set grows without
	 * bound as the player clicks the same brick.
	 */
	if (Selected.Remove(Ref) > 0)
	{
		return false;
	}

	Selected.Add(Ref);

	return true;
}

bool FPieceSelection::Contains(const FPieceRef& Ref) const
{
	return Selected.Contains(Ref);
}

bool FPieceSelection::Clear()
{
	/*
	 * ANSWERS WHETHER THERE WAS ANYTHING TO CLEAR, so a presenter can tell "a click on
	 * empty space dismissed something" from "there was nothing up" — two different events
	 * that a call always reporting a change cannot distinguish.
	 */
	if (Selected.Num() == 0)
	{
		return false;
	}

	Selected.Reset();

	return true;
}

int32 FPieceSelection::Num() const
{
	return Selected.Num();
}

TArrayView<const FPieceRef> FPieceSelection::Refs() const
{
	return Selected;
}
