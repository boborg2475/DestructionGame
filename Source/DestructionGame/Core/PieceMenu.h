// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Core/PieceActions.h"
#include "Core/StructureBinding.h"

/**
 * One entry of a presented piece menu: what it says, what it will run, and against what.
 *
 * THE ROW CARRIES ITS REF, and that is the whole reason this is a struct rather than the
 * bare TArray<const FPieceAction*> the table already hands back. A presented entry is a
 * COMMAND waiting to be chosen: whoever clicks it needs both halves of the commit call,
 * and a presenter that remembered the ref separately from the rows is one stale field away
 * from committing the right action against the wrong brick.
 *
 * THE ACTION POINTER IS A ROW OF AllPieceActions(), NEVER A COPY, so "which entry did they
 * choose" stays a pointer comparison — the same promise PieceActionsFor already makes, and
 * it survives being wrapped only if nothing here copies the row.
 */
struct FPieceMenuRow
{
	/** What the button reads. Taken from the action's own Label; never spelled here. */
	FString Label;

	/** The row of AllPieceActions() this entry commits. */
	const FPieceAction* Action = nullptr;

	/**
	 * EVERY piece this entry commits against, in the order they were selected.
	 *
	 * THE WHOLE SET LIVES ON THE ROW, exactly as the single ref did and for exactly the same
	 * reason: a presented entry is a COMMAND, and a presenter that remembered the targets
	 * beside the rows is one stale field away from running the right action against the wrong
	 * bricks. A selection is the durable state a menu is a projection of, so the projection
	 * has to carry it.
	 *
	 * NEVER EMPTY ON A ROW THAT EXISTS. A menu with no target is a button with nothing behind
	 * it, so a builder handed no refs builds no rows at all.
	 */
	TArray<FPieceRef> Refs;

	/**
	 * The last piece added to Refs — the one the player most recently picked.
	 *
	 * DERIVED, NOT A SECOND RECORD, and it is the same shape as Label: Label is a copy of the
	 * action's own text and this is a copy of Refs.Last(), both kept because almost every
	 * consumer wants exactly one of them and neither is allowed to disagree with what it came
	 * from. Tests/PieceMultiSelectTest.cpp asserts they agree on every presented row, so the
	 * pair cannot drift the way two independent fields would.
	 *
	 * IT IS ALWAYS A MEMBER OF Refs. A field naming a piece that is not in the selection would
	 * be worse than no field: it is the anchor a per-brick breakout will hang off, and an
	 * anchor outside the set it anchors is a readout of somebody else's brick.
	 */
	FPieceRef Ref;
};

/**
 * Turn a menu — a list of action rows and the piece they are for — into presentable rows.
 *
 * A PURE FUNCTION OF THE LIST. It takes no binding and consults no CanRun, because the
 * filtering already happened: PieceActionsFor is what decides which actions a piece may
 * offer, and a presenter that re-asked would be a second, quieter copy of that policy.
 * What is left is display and identity, which needs no world, no widget and no graph — so
 * this is where the presenter's testable half lives, and Slate is what the other half is.
 *
 * IT FAILS CLOSED ON A REF THAT NAMES NOTHING. A row is a command rather than a readout,
 * and CURRENT_STATE.md's polarity-inversion note is exactly about this seam: a value that
 * is safe as a diagnostic default is unsafe as an instruction. A click that hit the floor
 * arrives as a default FPieceRef, and offering a full menu against it would put a Delete
 * button on screen with nothing behind it.
 *
 * AND A MALFORMED ROW IS SKIPPED RATHER THAN DEREFERENCED. AllPieceActions() cannot
 * contain one — Core.PieceActions.TableIsWellFormed sweeps for it — but this takes an
 * array from anywhere, and dereferencing a null row here would abort the whole automation
 * run rather than failing one test. Skipping is also the same code path CanRun already
 * takes for an entry a piece may not have, which is the shape the table exists to keep.
 */
TArray<FPieceMenuRow> BuildPieceMenuRows(
	TArrayView<const FPieceAction* const> Actions,
	const FPieceRef& Ref);

/**
 * The same, for a whole selection: one row per action, each carrying every selected piece.
 *
 * THE SINGLE-REF OVERLOAD ABOVE IS THE ONE-ELEMENT CASE OF THIS ONE, which is the property
 * the multi-select slice turns on — a menu for one brick is not a different kind of menu.
 *
 * IT FAILS CLOSED ON AN EMPTY SELECTION and on a selection containing a ref that names
 * nothing, for the reason the single-ref version fails closed on a default ref: a row is a
 * command, and a command with a hole in its target list is worse than no command. The whole
 * list is refused rather than the bad entry silently dropped, because dropping one would put
 * a Delete button on screen that removes fewer bricks than the player picked, which is
 * exactly the class of quiet wrongness this codebase keeps closing.
 */
TArray<FPieceMenuRow> BuildPieceMenuRows(
	TArrayView<const FPieceAction* const> Actions,
	TArrayView<const FPieceRef> Refs);
