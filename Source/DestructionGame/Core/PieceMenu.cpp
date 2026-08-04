// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/PieceMenu.h"

TArray<FPieceMenuRow> BuildPieceMenuRows(
	TArrayView<const FPieceAction* const> Actions,
	const FPieceRef& Ref)
{
	/*
	 * A MENU FOR ONE BRICK IS A MENU FOR A SELECTION OF ONE, spelled that way rather than
	 * duplicated: a menu for one brick is not a different kind of menu, and a second copy
	 * of the fail-closed rule below is a second policy free to drift from it.
	 */
	return BuildPieceMenuRows(Actions, TArrayView<const FPieceRef>(&Ref, 1));
}

TArray<FPieceMenuRow> BuildPieceMenuRows(
	TArrayView<const FPieceAction* const> Actions,
	TArrayView<const FPieceRef> Refs)
{
	TArray<FPieceMenuRow> Rows;

	/*
	 * NO SELECTION BUILDS NO ROWS. A menu with no target is a button with nothing behind
	 * it, and a row that exists must always carry at least one piece to commit against —
	 * which is also what lets everything below take Refs.Last() without a second guard.
	 */
	if (Refs.Num() == 0)
	{
		return Rows;
	}

	/*
	 * AND A REF MISSING EITHER HALF BUILDS NOTHING, however good the menu handed in was,
	 * and however many of the other refs were perfect.
	 *
	 * A row is a COMMAND waiting to be chosen rather than a readout, and a default FPieceRef
	 * is both what a click on the floor arrives as and what "we have no answer" looks like.
	 * Offering the full menu against one would put a Delete button on screen with nothing
	 * behind it. Zero is a real structure and a real piece — the first one the game mode
	 * builds — so the sentinel is tested for by name rather than by truthiness.
	 *
	 * The whole list goes rather than the bad entry, for the reason PieceActionsFor gives:
	 * dropping one would put a button on screen that acts on fewer bricks than the player
	 * picked, and nothing about that reads as a bug.
	 */
	for (const FPieceRef& Ref : Refs)
	{
		if (Ref.StructureId == INDEX_NONE || Ref.PieceIndex == INDEX_NONE)
		{
			return Rows;
		}
	}

	Rows.Reserve(Actions.Num());

	/*
	 * ONE ROW PER ACTION, IN THE ORDER THEY WERE OFFERED, AND CanRun IS NOT CONSULTED.
	 * PieceActionsFor is what decides which actions a piece may offer and it has already
	 * run; asking again here would be a second, quieter copy of that policy.
	 */
	for (const FPieceAction* const Action : Actions)
	{
		/*
		 * A MALFORMED ROW IS SKIPPED RATHER THAN DEREFERENCED. AllPieceActions() cannot hold
		 * one, but this takes an array from anywhere, and dereferencing a null row would
		 * abort the whole run rather than dropping one entry.
		 */
		if (Action == nullptr || Action->Label == nullptr)
		{
			continue;
		}

		FPieceMenuRow& Row = Rows.AddDefaulted_GetRef();

		/*
		 * THE ACTION IS CARRIED BY POINTER AND THE LABEL IS TAKEN FROM IT, never spelled
		 * here: "which entry did they choose" is a pointer comparison against the shipped
		 * table, which a copied row would break while keeping every label right.
		 */
		Row.Label = FString(Action->Label);
		Row.Action = Action;
		Row.Refs.Append(Refs.GetData(), Refs.Num());

		/*
		 * THE ANCHOR IS DERIVED FROM THE SET, NEVER STORED BESIDE IT — the same shape as
		 * Label, which is a copy of the action's own text. A field naming a piece that is
		 * not in the selection would be worse than no field, because it is what a per-brick
		 * readout hangs off.
		 */
		Row.Ref = Row.Refs.Last();
	}

	return Rows;
}
