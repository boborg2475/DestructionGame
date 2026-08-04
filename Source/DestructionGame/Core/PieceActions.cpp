// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/PieceActions.h"

/**
 * THE PIECE ACTION TABLE — one row per thing a context menu offers on a brick.
 *
 * Modelled on the profile libraries in Core/Profiles, and for the same reason: adding the
 * second and the tenth action must be adding a ROW rather than editing a switch. Nothing
 * in this file may grow a branch that names a particular action.
 *
 * FILE-LOCAL NAMES CARRY A PieceAction PREFIX. An anonymous namespace is private to a
 * TRANSLATION UNIT rather than to a file, and a unity build merges many files into one —
 * at which point two anonymous CanRun declarations are the same declaration twice. See
 * CURRENT_STATE.md, where the collisions this has already caused are recorded.
 */
namespace
{
	/**
	 * A brick that is still in the wall and has not already been handed to physics.
	 *
	 * BOTH HALVES ARE NEEDED AND NEITHER IMPLIES THE OTHER. Removed and released are
	 * separate states with separate records — a tombstone on the graph and a one-way latch
	 * on the binding — so a released piece is still very much in the graph, and offering
	 * Delete on it would be offering it on a brick already tumbling through the air.
	 *
	 * Out-of-range handles are closed by the first call alone: FStructure::IsPieceRemoved
	 * answers true for a handle that names nothing, which is the fail-closed direction and
	 * the reason there is no bounds check of its own here.
	 */
	bool PieceActionDeleteCanRun(const FStructureBinding& Binding, int32 PieceHandle)
	{
		return !Binding.IsPieceRemoved(PieceHandle) && !Binding.IsReleased(PieceHandle);
	}

	/**
	 * Take the brick out of the structure.
	 *
	 * FStructureBinding::RemovePiece is the whole action: it asks the graph first and
	 * clears the binding's actor only if the graph said a live piece went, so its answer
	 * is already "did this do anything" and there is nothing here to add to it. No solve —
	 * that belongs to the commit path, so no row can forget it.
	 */
	bool PieceActionDeleteRun(FPieceActionContext& Context)
	{
		return Context.Binding.RemovePiece(Context.PieceHandle);
	}

	const FPieceAction PieceActionRows[] = {
		{ TEXT("Delete"), &PieceActionDeleteCanRun, &PieceActionDeleteRun }
	};
}

TArrayView<const FPieceAction> AllPieceActions()
{
	return TArrayView<const FPieceAction>(PieceActionRows);
}

FPieceActionResult RunPieceAction(
	FStructureBinding& Binding,
	const FPieceRef& Ref,
	const FPieceAction& Action)
{
	FPieceActionResult Result;

	/*
	 * THE REF IS RE-RESOLVED HERE, AND A STALE ONE STOPS THE COMMIT DEAD. The ref is the
	 * durable identity and a piece handle is a momentary answer: between the click that
	 * opened the menu and the click that chose an entry, the piece can have gone by another
	 * route entirely. ResolvePiece already fails closed on every route — another
	 * structure's id, either default, an index outside the range, and a tombstoned slot,
	 * which is the sharp one because it stays a valid array index forever.
	 *
	 * Refusing BEFORE the action runs is what makes this a guard rather than a report.
	 * Running first and then answering false leaves whatever the action did behind it,
	 * which for an action that is not Delete need not be visible from outside at all.
	 */
	const int32 PieceHandle = Binding.ResolvePiece(Ref);

	if (PieceHandle == INDEX_NONE)
	{
		return Result;
	}

	/*
	 * THE ACTOR IS CAPTURED BEFORE THE ACTION RUNS, and the ordering is the whole point.
	 * FStructureBinding::GetActor answers null for a removed piece, so looking afterwards
	 * can never find the orphan — the brick mesh would stay in the world forever with no
	 * piece naming it and nothing reporting anything wrong.
	 */
	UObject* const ActorBeforeRunning = Binding.GetActor(PieceHandle);

	FPieceActionContext Context{ Binding, PieceHandle };

	Result.bRan = Action.Run(Context);

	/*
	 * A COMMIT THAT DID NOTHING HANDS BACK NOTHING. The caller destroys what comes back, so
	 * an actor returned beside a false is a brick still standing in the wall being torn out
	 * of the world.
	 */
	if (Result.bRan)
	{
		Result.ActorToDestroy = ActorBeforeRunning;
	}

	/*
	 * AND THE RE-SOLVE BELONGS HERE RATHER THAN TO ANY ROW. A per-row obligation to
	 * remember it is exactly the switch this table exists not to have, relocated; and
	 * forgetting it is the failure where the brick vanishes and the wall stands there,
	 * every piece above it still reporting itself held up by something that has gone.
	 *
	 * Unconditional past this point because solving is non-destructive and re-runnable, so
	 * a run that changed nothing costs a solve rather than a branch.
	 */
	Binding.SolveLoads();

	return Result;
}
