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

	/** The piece this entry commits against. */
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
