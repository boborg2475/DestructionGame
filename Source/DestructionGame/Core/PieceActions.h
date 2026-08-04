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
