// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Core/StructureBinding.h"

/**
 * What an action is handed when it runs. A struct so a new action can add a field without
 * changing every existing row's signature.
 */
struct FPieceActionContext
{
	FStructureBinding& Binding;

	/** Already re-resolved by the commit path: a live handle, or nothing ran. */
	int32 PieceHandle = INDEX_NONE;
};

/**
 * One row of the piece action table (a context-menu entry). Plain function pointers, like
 * FNamedConnectionProfile, so rows are constant-initialised. CanRun is separate from Run so the
 * menu filters without naming any action. Run returns whether it did anything, since a silent
 * no-op is indistinguishable from success.
 */
struct FPieceAction
{
	const TCHAR* Label = nullptr;
	bool (*CanRun)(const FStructureBinding&, int32 PieceHandle) = nullptr;
	bool (*Run)(FPieceActionContext&) = nullptr;

	/** Whether this row destroys something the player cannot get back. Data, so no presenter compares labels. */
	bool bIsDestructive = false;
};

/** Every piece action, for building menus and for table-wide checks. */
TArrayView<const FPieceAction> AllPieceActions();

/**
 * Actions to offer for the piece a ref names: the table filtered by CanRun. Takes the ref so a
 * stale, foreign, removed or default (floor click) ref gives an empty menu. Returns pointers to
 * the table's rows, not copies.
 */
TArray<const FPieceAction*> PieceActionsFor(const FStructureBinding& Binding, const FPieceRef& Ref);

/**
 * Actions to offer for a selection: the intersection, since choosing one runs it on every piece.
 * The single-ref overload is this with one element. An empty selection offers nothing (the
 * vacuous intersection would fail open), and any unresolvable ref empties the menu.
 */
TArray<const FPieceAction*> PieceActionsFor(
	const FStructureBinding& Binding,
	TArrayView<const FPieceRef> Refs);

/** What running an action produced. The actor is returned for the caller to destroy, keeping this world-free. */
struct FPieceActionResult
{
	bool bRan = false;
	UObject* ActorToDestroy = nullptr;
};

/**
 * Commits an action against a piece named by its durable ref. Re-resolves the ref (the piece may
 * have gone since the click) and checks CanRun, so a keybind or replay can never do more than the
 * menu offered. Captures the actor first, since GetActor returns null once removed. Settles with
 * the same cascade as RunPieceActions, so one brick behaves as a selection of one.
 */
FPieceActionResult RunPieceAction(
	FStructureBinding& Binding,
	const FPieceRef& Ref,
	const FPieceAction& Action);

/** Result across a selection: one orphan actor per piece that ran, in run order. */
struct FPieceBatchActionResult
{
	/** How many of the selected pieces the action actually ran against. */
	int32 RanCount = 0;

	TArray<UObject*> ActorsToDestroy;
};

/**
 * Runs one action on every piece of a selection, then settles once. Actions stay single-piece;
 * this loops. One settle instead of N matters because each solve costs tens of ms at scenario
 * scale.
 *
 * Settling is SolveAndBreak, not SolveLoads: a removal starts an ordinary cascade (DESIGN.md §3).
 * Solving alone would leave a joint at 2.24 of capacity standing. Exactly one settle per call,
 * always last, even when nothing ran: ApplyResults releases only against the latest answer, and
 * a second cascade by a caller would stamp break passes twice for one click. There is
 * deliberately no public "run without settling", since releasing against a stale answer is
 * irreversible.
 */
FPieceBatchActionResult RunPieceActions(
	FStructureBinding& Binding,
	TArrayView<const FPieceRef> Refs,
	const FPieceAction& Action);
