// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Core/StructureBinding.h"

/**
 * What an action is handed when it runs.
 *
 * A struct rather than loose parameters, the whole requirement rather than a stylistic
 * preference: adding the second and tenth action must be adding a row, not editing a
 * switch, and a signature that has to grow to admit action #2 fails that outright.
 * Growing a struct's fields touches no existing row; growing a function's parameters
 * touches every one of them.
 */
struct FPieceActionContext
{
	/** The structure the piece belongs to, mutable because an action is a mutation. */
	FStructureBinding& Binding;

	/** Already re-resolved by the commit path: a live handle, or nothing ran. */
	int32 PieceHandle = INDEX_NONE;
};

/**
 * One row of the piece action table — what a context menu offers on a brick.
 *
 * Free function pointers, not TFunction: constant-initialised, no allocation, and
 * "adding a row" is literal — the same shape as FNamedConnectionProfile in
 * Core/Profiles/ConnectionProfiles.h, the library this is modelled on.
 *
 * CanRun is separate from Run so the menu can filter itself without any branch naming a
 * specific action: a greyed-out entry and a dead entry are the same code path.
 *
 * Run returns bool — did it do anything. This project has repeatedly found the silent
 * no-op to be the dangerous case; an action that quietly did nothing is indistinguishable
 * from one that worked.
 */
struct FPieceAction
{
	const TCHAR* Label = nullptr;
	bool (*CanRun)(const FStructureBinding&, int32 PieceHandle) = nullptr;
	bool (*Run)(FPieceActionContext&) = nullptr;

	/**
	 * Whether running this row destroys something a player cannot get back.
	 *
	 * Data on the row, the only place it can be: a presenter deciding it by comparing the
	 * label against "Delete" is the switch statement this table exists not to have. False by
	 * default, so a new action is harmless until it says otherwise, and saying otherwise is
	 * one field.
	 */
	bool bIsDestructive = false;
};

/**
 * Every piece action, so a menu can be built and a sweep can check the whole table
 * rather than whichever entries a test remembered to name.
 */
TArrayView<const FPieceAction> AllPieceActions();

/**
 * Which actions a menu should offer for the piece a ref names.
 *
 * The menu is built from the table, filtered by CanRun, with deliberately no branch here
 * naming a particular action — what makes adding the tenth action a row rather than an
 * edit to the presenter too.
 *
 * It takes the ref, not a handle, for the reason RunPieceAction does: the ref is the
 * durable identity a clicked brick carries, and resolving it here means a stale, foreign
 * or removed ref produces an empty menu rather than one built against somebody else's
 * brick. A click that hit the floor arrives as a default ref and must get nothing back.
 *
 * The pointers name rows of AllPieceActions(), never copies, so what the presenter shows
 * and what the caller commits are the same object.
 */
TArray<const FPieceAction*> PieceActionsFor(const FStructureBinding& Binding, const FPieceRef& Ref);

/**
 * Which actions a menu should offer for a whole SELECTION of pieces.
 *
 * The intersection, not the union — the whole requirement rather than a preference. An
 * action is offered only if every selected piece's CanRun says yes, because choosing it
 * runs it against all of them; "offered if any" puts a Delete button on a menu that then
 * deletes bricks nobody could have chosen individually.
 *
 * One piece is the one-element case, so the single-ref overload above is this function
 * with a list of one, never a second policy. An empty selection offers nothing: an empty
 * intersection over an empty set is vacuously everything, the fail-open direction.
 *
 * A single unresolvable ref empties the whole menu rather than being skipped. The refs
 * come from bricks that may have gone between the click that selected them and the click
 * that opens the menu, and an action offered for a set it cannot fully name will do
 * something other than what it says.
 */
TArray<const FPieceAction*> PieceActionsFor(
	const FStructureBinding& Binding,
	TArrayView<const FPieceRef> Refs);

/**
 * What running an action produced.
 *
 * The actor comes back rather than being destroyed here, which keeps the commit path
 * world-free: it names a UObject, it does not touch a world. The caller — the subsystem,
 * which already has one — does the destroying.
 */
struct FPieceActionResult
{
	bool bRan = false;
	UObject* ActorToDestroy = nullptr;
};

/**
 * Commit an action against a piece named by its durable ref.
 *
 * The ref is re-resolved here, which is why this is a function rather than a call to
 * Action.Run at the menu's call site: a piece handle is a momentary answer, and between
 * the click and the commit the piece can have gone by another route entirely. A stale ref
 * must do nothing at all.
 *
 * The row's CanRun is consulted too, so the commit door is never wider than the menu: the
 * re-resolve refuses a removed piece and CanRun refuses a released one, and a commit path
 * that asked only the first would let a keybind, an RPC or a replay act on a brick the
 * menu would not have offered.
 *
 * The actor is captured before the action runs, because FStructureBinding::GetActor
 * answers null for a removed piece, so looking afterwards can never find the orphan.
 *
 * And settling the wall belongs here, not to any row — a per-row obligation to remember
 * it is exactly the "editing a switch" smell the table exists to avoid.
 *
 * Settling is a cascade, not a solve, and it is the same one RunPieceActions runs; the
 * two doors must not differ, because a wall may not behave differently for a player who
 * picked one brick.
 */
FPieceActionResult RunPieceAction(
	FStructureBinding& Binding,
	const FPieceRef& Ref,
	const FPieceAction& Action);

/**
 * What running an action across a whole selection produced.
 *
 * One orphan per piece that ran, in the order they ran, so the caller destroys exactly
 * what the batch took out of the graph. Same contract as the single result's
 * ActorToDestroy, and the same reason it is handed back rather than destroyed here.
 */
struct FPieceBatchActionResult
{
	/** How many of the selected pieces the action actually ran against. */
	int32 RanCount = 0;

	TArray<UObject*> ActorsToDestroy;
};

/**
 * Commit one action against every piece of a selection, then let the wall settle ONCE.
 *
 * An action stays single-piece, and this is what loops: no row understands sets, because
 * the moment one does, adding an action stops being adding a row. FPieceActionContext is
 * shaped to grow by field if something genuinely needs the whole set later.
 *
 * Settling is the point, and it lives here because there is nowhere else it can. Each
 * solve costs a full pass over the graph — tens of milliseconds at scenario scale — so a
 * batch that called RunPieceAction per piece would settle N times to reach a state one
 * settle describes. Running everything first and settling after is what makes
 * multi-select faster per brick rather than slower.
 *
 * Settling means FStructureBinding::SolveAndBreak, not SolveLoads: removal is not the only
 * way a wall comes down, and DESIGN.md §3 is explicit that what follows a removal is an
 * ordinary cascade. A commit that only solved would compute a surviving joint at 2.24 of
 * what mortar holds and ask none of them to give — the wall a player cut a staircase
 * through and watched stand, its topology intact and its overhang not.
 *
 * Exactly one settle per call, unconditionally, including a call where nothing ran: a
 * cascade over a settled structure is one solve that breaks nothing, so the no-op case
 * costs what the plain solve used to. A rule with no exceptions is one no caller can be on
 * the wrong side of.
 *
 * It must see every removal, an ordering obligation rather than a count:
 * FStructureBinding::ApplyResults refuses to release a piece the last solve has no answer
 * for, so whoever pushes after this must be pushing an answer computed after the last
 * action ran. A settle placed anywhere but the end is still one settle and still wrong.
 *
 * One cascade per player action, which binds callers too: breaking is irreversible and
 * stamps the pass numbers a collapse is replayed in, so a caller that cascaded again on
 * its way to the world would stamp twice for one click.
 *
 * RunPieceAction is the one-element case of this. There is deliberately no public "run
 * without settling": a caller who forgot it and then pushed would release pieces against a
 * stale answer, and releasing is irreversible.
 */
FPieceBatchActionResult RunPieceActions(
	FStructureBinding& Binding,
	TArrayView<const FPieceRef> Refs,
	const FPieceAction& Action);
