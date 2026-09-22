// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/PieceActions.h"

/**
 * The piece action table: one row per context-menu action on a brick. Like the profile
 * libraries, a new action is a new row; nothing here may branch on a particular action.
 *
 * File-local names carry a PieceAction prefix because unity builds merge anonymous namespaces
 * across files (see CURRENT_STATE.md).
 */
namespace
{
	/**
	 * A brick still in the graph and not yet released to physics. Removed and released are
	 * separate states, so both are checked. IsPieceRemoved returns true for an out-of-range
	 * handle, which covers the bounds check.
	 */
	bool PieceActionDeleteCanRun(const FStructureBinding& Binding, int32 PieceHandle)
	{
		return !Binding.IsPieceRemoved(PieceHandle) && !Binding.IsReleased(PieceHandle);
	}

	/** Remove the brick from the structure. Returns whether a live piece went. No solve; the commit path does that. */
	bool PieceActionDeleteRun(FPieceActionContext& Context)
	{
		return Context.Binding.RemovePiece(Context.PieceHandle);
	}

	// Delete is irreversible, so the row marks it destructive rather than a widget matching the caption.
	const FPieceAction PieceActionRows[] = {
		{ TEXT("Delete"), &PieceActionDeleteCanRun, &PieceActionDeleteRun, /*bIsDestructive*/ true }
	};

	/**
	 * Run one action on one piece without solving. File-local so no caller can skip the solve
	 * and then release against a stale answer. Holds every guard, so the single and batch paths
	 * agree: re-resolve the ref, check CanRun, capture the actor, run.
	 */
	FPieceActionResult RunOnePieceActionWithoutSolving(
		FStructureBinding& Binding,
		const FPieceRef& Ref,
		const FPieceAction& Action)
	{
		FPieceActionResult Result;

		/*
		 * Re-resolve the ref before running: the piece may have gone since the menu opened.
		 * ResolvePiece fails closed on a wrong structure id, defaults, out-of-range and
		 * tombstoned slots.
		 */
		const int32 PieceHandle = Binding.ResolvePiece(Ref);

		if (PieceHandle == INDEX_NONE)
		{
			return Result;
		}

		/*
		 * Also check CanRun, which additionally refuses released pieces (e.g. released by a cascade
		 * after the menu opened), so the commit is never wider than the menu. To allow deleting
		 * falling debris, widen Delete's CanRun rather than removing this.
		 */
		if (!Action.CanRun(Binding, PieceHandle))
		{
			return Result;
		}

		// Capture the actor before running: GetActor returns null once the piece is removed.
		UObject* const ActorBeforeRunning = Binding.GetActor(PieceHandle);

		FPieceActionContext Context{ Binding, PieceHandle };

		Result.bRan = Action.Run(Context);

		// Return an actor only on success: the caller destroys it, which would tear out a standing brick.
		if (Result.bRan)
		{
			Result.ActorToDestroy = ActorBeforeRunning;
		}

		return Result;
	}
}

TArrayView<const FPieceAction> AllPieceActions()
{
	return TArrayView<const FPieceAction>(PieceActionRows);
}

TArray<const FPieceAction*> PieceActionsFor(const FStructureBinding& Binding, const FPieceRef& Ref)
{
	// A selection of one, so single and multi-select share one filter.
	return PieceActionsFor(Binding, TArrayView<const FPieceRef>(&Ref, 1));
}

TArray<const FPieceAction*> PieceActionsFor(
	const FStructureBinding& Binding,
	TArrayView<const FPieceRef> Refs)
{
	TArray<const FPieceAction*> Menu;

	// An empty selection offers nothing (the intersection below would vacuously offer everything).
	if (Refs.Num() == 0)
	{
		return Menu;
	}

	/*
	 * Any unresolvable ref (e.g. a floor click's default ref) empties the menu rather than being
	 * dropped, so an offered action always covers the whole selection.
	 */
	TArray<int32> PieceHandles;
	PieceHandles.Reserve(Refs.Num());

	for (const FPieceRef& Ref : Refs)
	{
		const int32 PieceHandle = Binding.ResolvePiece(Ref);

		if (PieceHandle == INDEX_NONE)
		{
			return TArray<const FPieceAction*>();
		}

		PieceHandles.Add(PieceHandle);
	}

	/*
	 * Offer an action only if every selected piece's CanRun allows it, since choosing it runs
	 * on all of them. Returns pointers into the shipped table, which presenters compare by
	 * identity.
	 */
	for (const FPieceAction& Action : AllPieceActions())
	{
		bool bEveryPieceAllows = true;

		for (const int32 PieceHandle : PieceHandles)
		{
			bEveryPieceAllows = bEveryPieceAllows && Action.CanRun(Binding, PieceHandle);
		}

		if (bEveryPieceAllows)
		{
			Menu.Add(&Action);
		}
	}

	return Menu;
}

FPieceBatchActionResult RunPieceActions(
	FStructureBinding& Binding,
	TArrayView<const FPieceRef> Refs,
	const FPieceAction& Action)
{
	FPieceBatchActionResult Result;

	/*
	 * Unlike the menu, a stale ref here skips only that piece: the commit was already authorised
	 * for the rest. The skip comes from the shared guard.
	 */
	for (const FPieceRef& Ref : Refs)
	{
		const FPieceActionResult One = RunOnePieceActionWithoutSolving(Binding, Ref, Action);

		if (!One.bRan)
		{
			continue;
		}

		++Result.RanCount;

		Result.ActorsToDestroy.Add(One.ActorToDestroy);
	}

	/*
	 * Settle once, unconditionally, and last.
	 * - SolveAndBreak, not just a solve: a removal is followed by an ordinary cascade (DESIGN.md
	 *   §3); a solve alone would leave overloaded joints unbroken.
	 * - Once per batch, and even when nothing ran (a settled structure breaks nothing).
	 * - Last, so ApplyResults sees every removal and break; it won't release pieces the last
	 *   solve didn't cover.
	 * Callers must not cascade again for the same action: breaks are irreversible and stamp
	 * pass numbers.
	 */
	Binding.SolveAndBreak();

	return Result;
}

FPieceActionResult RunPieceAction(
	FStructureBinding& Binding,
	const FPieceRef& Ref,
	const FPieceAction& Action)
{
	const FPieceActionResult Result = RunOnePieceActionWithoutSolving(Binding, Ref, Action);

	/*
	 * Same unconditional cascade as RunPieceActions (see there), here rather than in each row
	 * so no row can forget it, and identical so physics doesn't depend on selection size.
	 */
	Binding.SolveAndBreak();

	return Result;
}
