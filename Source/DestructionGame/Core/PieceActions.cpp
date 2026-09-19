// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/PieceActions.h"

/**
 * The piece action table — one row per thing a context menu offers on a brick.
 *
 * Modelled on the profile libraries in Core/Profiles, for the same reason: adding the
 * second and tenth action must be adding a row rather than editing a switch. Nothing in
 * this file may grow a branch that names a particular action.
 *
 * File-local names carry a PieceAction prefix: an anonymous namespace is private to a
 * translation unit, not a file, and a unity build merges many files into one, so two
 * anonymous CanRun declarations become the same declaration twice. See CURRENT_STATE.md,
 * where the collisions this has already caused are recorded.
 */
namespace
{
	/**
	 * A brick that is still in the wall and has not already been handed to physics.
	 *
	 * Both halves are needed and neither implies the other: removed and released are
	 * separate states with separate records — a tombstone on the graph and a one-way latch
	 * on the binding — so a released piece is still very much in the graph, and offering
	 * Delete on it would be offering it on a brick already tumbling through the air.
	 *
	 * Out-of-range handles are closed by the first call alone: FStructure::IsPieceRemoved
	 * answers true for a handle that names nothing, the fail-closed direction and the
	 * reason there is no bounds check of its own here.
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
	 * is already "did this do anything". No solve — that belongs to the commit path, so no
	 * row can forget it.
	 */
	bool PieceActionDeleteRun(FPieceActionContext& Context)
	{
		return Context.Binding.RemovePiece(Context.PieceHandle);
	}

	/*
	 * Delete is destructive, and the row is where that is said: it hands a brick to physics
	 * and takes it out of the graph, and nothing in this project puts one back. The only
	 * alternative is a widget comparing the caption against the word "Delete", a policy in a
	 * string literal in the one place no test can read it.
	 */
	const FPieceAction PieceActionRows[] = {
		{ TEXT("Delete"), &PieceActionDeleteCanRun, &PieceActionDeleteRun, /*bIsDestructive*/ true }
	};

	/**
	 * One action against one piece, WITHOUT the solve — and it is file-local for exactly
	 * that reason.
	 *
	 * There is no public "run without solving", and that absence is the point: a caller
	 * who forgot the solve and then pushed would release pieces against a stale answer,
	 * and releasing is irreversible. Keeping the solve inside the only entry points means
	 * no caller can end up on the wrong side of it.
	 *
	 * Every guard the commit path has lives here, so RunPieceAction and RunPieceActions
	 * cannot disagree about which pieces an action may run against: re-resolve the ref,
	 * consult the row's own CanRun, capture the actor, then run.
	 */
	FPieceActionResult RunOnePieceActionWithoutSolving(
		FStructureBinding& Binding,
		const FPieceRef& Ref,
		const FPieceAction& Action)
	{
		FPieceActionResult Result;

		/*
		 * The ref is re-resolved here, and a stale one stops the commit dead. The ref is the
		 * durable identity and a piece handle is a momentary answer: between the click that
		 * opened the menu and the click that chose an entry, the piece can have gone by
		 * another route entirely. ResolvePiece already fails closed on every route — another
		 * structure's id, either default, an out-of-range index, and a tombstoned slot, the
		 * sharp one because it stays a valid array index forever.
		 *
		 * Refusing before the action runs is what makes this a guard rather than a report:
		 * running first and answering false afterwards leaves whatever the action did behind
		 * it.
		 */
		const int32 PieceHandle = Binding.ResolvePiece(Ref);

		if (PieceHandle == INDEX_NONE)
		{
			return Result;
		}

		/*
		 * And the row's own CanRun is consulted, a second guard rather than the same one
		 * twice: the re-resolve above refuses a removed piece, CanRun additionally refuses a
		 * released one, and without this the commit door would be strictly wider than the
		 * menu — an action committed against a brick already tumbling through the air, by a
		 * route the menu would never have offered. The window is ordinary: a cascade
		 * releasing a piece between the click that opens the menu and the click that chooses
		 * an entry.
		 *
		 * If deleting falling debris is ever wanted, the answer is to widen Delete's CanRun
		 * — one statement of the rule that both doors then agree on — not to leave this open.
		 *
		 * Refused before anything runs, for the same reason as the re-resolve.
		 */
		if (!Action.CanRun(Binding, PieceHandle))
		{
			return Result;
		}

		/*
		 * The actor is captured before the action runs, and the ordering is the whole point:
		 * FStructureBinding::GetActor answers null for a removed piece, so looking afterwards
		 * can never find the orphan — the brick mesh would stay in the world forever with no
		 * piece naming it.
		 */
		UObject* const ActorBeforeRunning = Binding.GetActor(PieceHandle);

		FPieceActionContext Context{ Binding, PieceHandle };

		Result.bRan = Action.Run(Context);

		/* A commit that did nothing hands back nothing: the caller destroys what comes
		 * back, so an actor returned beside a false tears a standing brick out of the world. */
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
	/* One piece is a selection of one, spelled that way rather than duplicated: a second
	 * copy of the filter would be a second policy, free to answer differently for the menu
	 * a single click opens than for the menu two clicks open. */
	return PieceActionsFor(Binding, TArrayView<const FPieceRef>(&Ref, 1));
}

TArray<const FPieceAction*> PieceActionsFor(
	const FStructureBinding& Binding,
	TArrayView<const FPieceRef> Refs)
{
	TArray<const FPieceAction*> Menu;

	/*
	 * An empty selection offers nothing, and that is not vacuous truth: an intersection
	 * over an empty set is mathematically everything, so the loop below — "no piece said
	 * no" — would offer the whole table for a selection of nothing. Refused here, before
	 * the question is even asked.
	 */
	if (Refs.Num() == 0)
	{
		return Menu;
	}

	/*
	 * Every ref is resolved here, and one that names nothing empties the whole menu. A
	 * click that hit the floor arrives as a default ref, so building a menu without
	 * resolving would offer to delete piece zero of whatever structure happened to be
	 * asked.
	 *
	 * The bad entry is not silently dropped: an action offered for a set it cannot fully
	 * name will do something other than what it says — a Delete button that removes fewer
	 * bricks than the player picked.
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
	 * The intersection, not the union: an action is offered only if every selected piece's
	 * CanRun says yes, because choosing it runs it against all of them — "offered if any"
	 * puts a Delete button on a menu that then deletes some bricks and silently declines
	 * the rest.
	 *
	 * Pointers into the shipped table, never copies: the caller hands one of these straight
	 * to RunPieceActions, and a presenter comparing what it showed against what was chosen
	 * compares pointers.
	 *
	 * And there is deliberately no branch here naming a particular action: the filter is
	 * the row's own CanRun, what makes adding the tenth action a row rather than an edit to
	 * the presenter too.
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
	 * A ref that resolves to nothing skips that piece and nothing else, the opposite
	 * polarity to the menu above and deliberately so: an offer with a hole in it is a lie
	 * about what the button will do, whereas a commit with a hole in it has already been
	 * authorised for the pieces that are still there. One stale entry must not cost the
	 * other five their delete. The skipping is the shared guard's own, so this loop adds no
	 * second opinion about which pieces an action may run against.
	 */
	for (const FPieceRef& Ref : Refs)
	{
		const FPieceActionResult One = RunOnePieceActionWithoutSolving(Binding, Ref, Action);

		if (!One.bRan)
		{
			continue;
		}

		++Result.RanCount;

		/* One orphan per piece that ran, in the order they ran. */

		Result.ActorsToDestroy.Add(One.ActorToDestroy);
	}

	/*
	 * Settling the wall is the last statement, and all three halves of that are
	 * load-bearing.
	 *
	 * It cascades rather than merely solving, the wire the game went without: removing a
	 * piece is not the only way a wall comes down, and DESIGN.md §3 is explicit that what
	 * follows a removal is an ordinary cascade. A solve alone computes that the surviving
	 * joints are past what mortar can hold and asks none of them to give — a staircase cut
	 * through a wall would leave corbelled bricks hanging over open air with the wall
	 * standing there.
	 *
	 * Exactly one settle, so a batch of ten costs what a batch of one costs — and
	 * unconditionally, including a call that ran nothing, since a cascade over a settled
	 * structure is one solve that breaks nothing. A rule with no exceptions is one no
	 * caller can be on the wrong side of.
	 *
	 * And last, so the answer whoever pushes next is pushing saw every removal and every
	 * joint that gave because of them: FStructureBinding::ApplyResults refuses to release a
	 * piece the last solve has no answer for, and a settle placed anywhere but here leaves
	 * the pieces the batch orphaned hanging in the air.
	 *
	 * Once per player action, which is a constraint on callers too: breaking is
	 * irreversible and stamps pass numbers a collapse is replayed in, so a caller that
	 * cascaded again on the way to the world would stamp twice for one click.
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
	 * And the settle belongs here rather than to any row: a per-row obligation to remember
	 * it is exactly the switch this table exists not to have, relocated, and forgetting it
	 * is the failure where the brick vanishes and the wall stands there.
	 *
	 * The same cascade the batch runs, and not a solve, which must not differ between the
	 * two doors: a single-brick delete that only solved while a two-brick delete cascaded
	 * would make the physics depend on how many bricks the player happened to pick.
	 *
	 * Unconditional, because a cascade over a settled structure is one solve that breaks
	 * nothing, so a run that changed nothing costs that rather than a branch.
	 * RunPieceActions carries the reasoning in full; this is its one-element case.
	 */
	Binding.SolveAndBreak();

	return Result;
}
