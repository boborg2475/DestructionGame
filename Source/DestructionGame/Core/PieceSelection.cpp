// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/PieceSelection.h"

bool FPieceSelection::Toggle(const FPieceRef& Ref)
{
	/*
	 * A ref missing either half never gets in, refused by name rather than by truthiness:
	 * structure 0 piece 0 is the first brick of the wall the game mode builds, so
	 * `if (!Ref.StructureId)` would silently refuse it forever. A click on the floor arrives
	 * as a wholly default ref, which is also what "we have no answer" looks like — and a
	 * selection is a command's target list, so it takes the cautious reading.
	 */
	if (Ref.StructureId == INDEX_NONE || Ref.PieceIndex == INDEX_NONE)
	{
		return false;
	}

	/* Removed by value, which is what makes a second click a deselection: the ref from
	 * the second click is a different object naming the same piece. */
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
	/* Answers whether there was anything to clear, so a presenter can tell "a click on
	 * empty space dismissed something" from "there was nothing up". */
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
