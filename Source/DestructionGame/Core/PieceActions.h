// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Core/StructureBinding.h"

/**
 * What an action is handed when it runs.
 *
 * A STRUCT RATHER THAN LOOSE PARAMETERS, and that is the whole requirement rather than
 * a stylistic preference. The point of this table is that adding the second and the
 * tenth action is adding a ROW, not editing a switch — and a signature that has to grow
 * to admit action #2 fails that outright. Growing a struct's FIELDS touches no existing
 * row; growing a function's PARAMETERS touches every one of them.
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
 * FREE FUNCTION POINTERS, NOT TFunction. Constant-initialised, no allocation, and
 * "adding a row" is literal: the same shape as FNamedConnectionProfile in
 * Core/Profiles/ConnectionProfiles.h, which is the library this is modelled on.
 *
 * CanRun IS SEPARATE FROM Run so the menu can filter itself without any branch naming
 * a specific action. A greyed-out entry and a dead entry are the same code path.
 *
 * Run RETURNS bool — did it do anything. This project has repeatedly found the silent
 * no-op to be the dangerous case; RemovePiece, ApplyResults and SolveAndPush all
 * already answer that question, and an action that quietly did nothing is
 * indistinguishable from one that worked.
 */
struct FPieceAction
{
	const TCHAR* Label = nullptr;
	bool (*CanRun)(const FStructureBinding&, int32 PieceHandle) = nullptr;
	bool (*Run)(FPieceActionContext&) = nullptr;

	/**
	 * Whether running this row destroys something a player cannot get back.
	 *
	 * DATA ON THE ROW, WHICH IS THE ONLY PLACE IT CAN BE. A presenter or a widget deciding it
	 * by comparing the label against "Delete" is the switch statement this table exists not to
	 * have, written in string literals and hidden where no test can read it. It defaults to
	 * FALSE so that adding a row is still adding a row — a new action is harmless until it says
	 * otherwise, and saying otherwise is one field.
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
 * THE MENU IS BUILT FROM THE TABLE, FILTERED BY CanRun, and there is deliberately no
 * branch here naming a particular action — that is what makes adding the tenth action a
 * row rather than an edit to the presenter as well.
 *
 * IT TAKES THE REF, NOT A HANDLE, for the same reason RunPieceAction does: the ref is the
 * durable identity a clicked brick carries, and resolving it here means a stale, foreign
 * or removed ref produces an EMPTY menu rather than a menu built against somebody else's
 * brick. A click that hit the floor, or nothing, arrives as a default ref and must get
 * nothing back.
 *
 * THE POINTERS NAME ROWS OF AllPieceActions(), never copies, so what the presenter shows
 * and what the caller commits are the same object.
 */
TArray<const FPieceAction*> PieceActionsFor(const FStructureBinding& Binding, const FPieceRef& Ref);

/**
 * Which actions a menu should offer for a whole SELECTION of pieces.
 *
 * THE INTERSECTION, NOT THE UNION, and that is the whole requirement rather than a
 * preference. An action is offered only if every selected piece's CanRun says yes, because
 * choosing it runs it against all of them — "offered if any" is the plausible wrong answer
 * and it puts a Delete button on a menu that then deletes bricks nobody could have chosen
 * individually, silently doing less (or more) than the button said.
 *
 * ONE PIECE IS THE ONE-ELEMENT CASE, so the single-ref overload above is this function with a
 * list of one and never a second policy. An EMPTY selection offers nothing: there is no
 * piece for a row to be meaningful about, and an empty intersection over an empty set is
 * vacuously everything, which is the fail-OPEN direction.
 *
 * A SINGLE UNRESOLVABLE REF EMPTIES THE WHOLE MENU rather than being skipped. The refs come
 * from bricks that may have gone between the click that selected them and the click that
 * opens the menu, and an action offered for a set it cannot fully name is an action that will
 * do something other than what it says.
 */
TArray<const FPieceAction*> PieceActionsFor(
	const FStructureBinding& Binding,
	TArrayView<const FPieceRef> Refs);

/**
 * What running an action produced.
 *
 * THE ACTOR COMES BACK RATHER THAN BEING DESTROYED HERE, which is what keeps the commit
 * path world-free: it names a UObject, it does not touch a world. The caller — the
 * subsystem, which already has one — does the destroying.
 */
struct FPieceActionResult
{
	bool bRan = false;
	UObject* ActorToDestroy = nullptr;
};

/**
 * Commit an action against a piece named by its durable ref.
 *
 * THE REF IS RE-RESOLVED HERE, and that is the reason this is a function rather than a
 * call to Action.Run at the menu's call site. The ref is the durable identity; a piece
 * handle is a momentary answer, and between the click and the commit the piece can have
 * gone by another route entirely. A stale ref must do nothing at all.
 *
 * AND THE ROW'S CanRun IS CONSULTED, so the commit door is never wider than the menu.
 * The re-resolve refuses a removed piece and CanRun refuses a released one; the two do
 * not overlap, and a commit path that asked only the first would let a keybind, an RPC or
 * a replay act on a brick the menu would not have offered. Widening what an action accepts
 * is a change to that action's CanRun, which both doors read.
 *
 * THE ACTOR IS CAPTURED BEFORE THE ACTION RUNS. FStructureBinding::GetActor answers null
 * for a removed piece, so an implementation that looks afterwards can never find the
 * orphan it was supposed to hand back.
 *
 * AND THE RE-SOLVE BELONGS HERE, NOT TO ANY ROW. A per-row obligation to remember to
 * re-solve is exactly the "editing a switch" smell the table exists to avoid — and
 * forgetting it means the brick vanishes and the wall stands there.
 */
FPieceActionResult RunPieceAction(
	FStructureBinding& Binding,
	const FPieceRef& Ref,
	const FPieceAction& Action);

/**
 * What running an action across a whole selection produced.
 *
 * ONE ORPHAN PER PIECE THAT RAN, in the order they ran, so the caller destroys exactly what
 * the batch took out of the graph and nothing else. Same contract as the single result's
 * ActorToDestroy, and the same reason it is handed back rather than destroyed here.
 */
struct FPieceBatchActionResult
{
	/** How many of the selected pieces the action actually ran against. */
	int32 RanCount = 0;

	TArray<UObject*> ActorsToDestroy;
};

/**
 * Commit one action against every piece of a selection, then solve ONCE.
 *
 * AN ACTION STAYS SINGLE-PIECE AND THIS IS WHAT LOOPS. No row understands sets, because the
 * moment one does, adding an action stops being adding a row — which is the only property the
 * table exists for. FPieceActionContext is shaped to grow by FIELD if something genuinely
 * needs the whole set later.
 *
 * THE SOLVE IS THE POINT, AND IT LIVES HERE BECAUSE THERE IS NOWHERE ELSE IT CAN. Each solve
 * costs a full pass over the graph — tens of milliseconds at scenario scale — so a batch that
 * called RunPieceAction per piece would solve N times to reach a state one solve describes,
 * and deleting ten bricks would cost ten times what deleting one does. Running everything
 * first and solving after is what makes multi-select FASTER per brick rather than slower.
 *
 * EXACTLY ONE SOLVE PER CALL, UNCONDITIONALLY — including a call where nothing ran. That is
 * the same rule the single-piece path already follows (solving is non-destructive and
 * re-runnable, so a run that changed nothing costs a solve rather than a branch), and a rule
 * with no exceptions is one no caller can be on the wrong side of.
 *
 * AND THE SOLVE MUST SEE EVERY REMOVAL, WHICH IS AN ORDERING OBLIGATION RATHER THAN A COUNT.
 * FStructureBinding::ApplyResults refuses to release a piece the last solve has no answer for,
 * so whoever pushes after this must be pushing an answer computed after the LAST action ran.
 * A solve placed anywhere but the end is one solve and still wrong.
 *
 * RunPieceAction IS THE ONE-ELEMENT CASE OF THIS. There is deliberately no public
 * "run without solving": a caller who forgot the solve and then pushed would release pieces
 * against a stale answer, and releasing is irreversible.
 */
FPieceBatchActionResult RunPieceActions(
	FStructureBinding& Binding,
	TArrayView<const FPieceRef> Refs,
	const FPieceAction& Action);
