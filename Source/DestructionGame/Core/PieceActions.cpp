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

	/*
	 * Delete IS DESTRUCTIVE, AND THE ROW IS WHERE THAT IS SAID. It hands a brick to physics and
	 * takes it out of the graph, and nothing in this project puts one back — so a panel has to be
	 * able to draw its button differently from a harmless one. The only alternative anybody would
	 * write is a presenter or a widget comparing the caption against the word "Delete", which is a
	 * policy in a string literal in the one place no test can read it.
	 */
	const FPieceAction PieceActionRows[] = {
		{ TEXT("Delete"), &PieceActionDeleteCanRun, &PieceActionDeleteRun, /*bIsDestructive*/ true }
	};

	/**
	 * One action against one piece, WITHOUT the solve — and it is file-local for exactly
	 * that reason.
	 *
	 * THERE IS NO PUBLIC "RUN WITHOUT SOLVING", AND THAT ABSENCE IS THE POINT. A caller
	 * who forgot the solve and then pushed would release pieces against a stale answer,
	 * and releasing is irreversible. Keeping the solve inside the only entry points means
	 * no caller can end up on the wrong side of it — the same argument that put the solve
	 * in the commit path rather than in a row.
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
		 * THE REF IS RE-RESOLVED HERE, AND A STALE ONE STOPS THE COMMIT DEAD. The ref is
		 * the durable identity and a piece handle is a momentary answer: between the click
		 * that opened the menu and the click that chose an entry, the piece can have gone
		 * by another route entirely. ResolvePiece already fails closed on every route —
		 * another structure's id, either default, an index outside the range, and a
		 * tombstoned slot, which is the sharp one because it stays a valid array index
		 * forever.
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
		 * AND THE ROW'S OWN CanRun IS CONSULTED, WHICH IS A SECOND GUARD RATHER THAN THE
		 * SAME ONE TWICE. The re-resolve above refuses a REMOVED piece; CanRun additionally
		 * refuses a RELEASED one, so without this the commit door is strictly wider than
		 * the menu and an action can be committed against a brick already tumbling through
		 * the air — by a route the menu would never have offered. The window is ordinary: a
		 * cascade releasing a piece between the click that opens the menu and the click
		 * that chooses an entry.
		 *
		 * CanRun is the row's own statement of when it is meaningful, so running Run
		 * outside it runs the row outside its stated domain. If deleting falling debris is
		 * ever wanted, the answer is to widen Delete's CanRun — one statement of the rule,
		 * in one place, that both doors then agree on — not to leave this one open.
		 *
		 * Refused BEFORE anything runs, for the same reason as the re-resolve: running
		 * first and answering false afterwards leaves whatever the action did behind it.
		 */
		if (!Action.CanRun(Binding, PieceHandle))
		{
			return Result;
		}

		/*
		 * THE ACTOR IS CAPTURED BEFORE THE ACTION RUNS, and the ordering is the whole
		 * point. FStructureBinding::GetActor answers null for a removed piece, so looking
		 * afterwards can never find the orphan — the brick mesh would stay in the world
		 * forever with no piece naming it and nothing reporting anything wrong.
		 */
		UObject* const ActorBeforeRunning = Binding.GetActor(PieceHandle);

		FPieceActionContext Context{ Binding, PieceHandle };

		Result.bRan = Action.Run(Context);

		/*
		 * A COMMIT THAT DID NOTHING HANDS BACK NOTHING. The caller destroys what comes
		 * back, so an actor returned beside a false is a brick still standing in the wall
		 * being torn out of the world.
		 */
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
	/*
	 * ONE PIECE IS A SELECTION OF ONE, and it is spelled that way rather than duplicated:
	 * a second copy of the filter would be a second policy, free to answer differently for
	 * the menu a single click opens than for the menu two clicks open.
	 */
	return PieceActionsFor(Binding, TArrayView<const FPieceRef>(&Ref, 1));
}

TArray<const FPieceAction*> PieceActionsFor(
	const FStructureBinding& Binding,
	TArrayView<const FPieceRef> Refs)
{
	TArray<const FPieceAction*> Menu;

	/*
	 * AN EMPTY SELECTION OFFERS NOTHING, AND THAT IS NOT VACUOUS TRUTH. An intersection
	 * over an empty set is mathematically everything, so the loop below — "no piece said
	 * no" — would offer the whole table for a selection of nothing: a Delete button with
	 * no target. Refused here, before the question is even asked.
	 */
	if (Refs.Num() == 0)
	{
		return Menu;
	}

	/*
	 * EVERY REF IS RESOLVED HERE, AND ONE THAT NAMES NOTHING EMPTIES THE WHOLE MENU. What
	 * a click produces is the {StructureId, PieceIndex} the brick carries; a handle is what
	 * resolving that against THIS binding answers. A click that hit the floor, or nothing
	 * at all, arrives as a default ref — so building a menu without resolving would offer
	 * to delete piece zero of whatever structure happened to be asked.
	 *
	 * The bad entry is not silently dropped, because an action offered for a set it cannot
	 * fully name is an action that will do something other than what it says: a Delete
	 * button that removes fewer bricks than the player picked.
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
	 * THE INTERSECTION, NOT THE UNION. An action is offered only if EVERY selected piece's
	 * CanRun says yes, because choosing it runs it against all of them — "offered if any"
	 * puts a Delete button on a menu that then deletes some of the bricks and silently
	 * declines the rest, which a player has no way to tell from a bug in the commit path.
	 *
	 * POINTERS INTO THE SHIPPED TABLE, NEVER COPIES. The caller's next move is to hand one
	 * of these straight to RunPieceActions, and a presenter comparing what it showed
	 * against what was chosen compares pointers — a menu of copies is unequal to everything.
	 *
	 * And there is deliberately no branch here naming a particular action: the filter is
	 * the row's own CanRun, which is what makes adding the tenth action a row rather than
	 * an edit to the presenter as well.
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
	 * A REF THAT RESOLVES TO NOTHING SKIPS THAT PIECE AND NOTHING ELSE, which is the
	 * opposite polarity to the MENU above and deliberately so: an offer with a hole in it
	 * is a lie about what the button will do, whereas a commit with a hole in it has
	 * already been authorised for the pieces that are still there. A selection is built by
	 * clicking, and a brick can go between the click and the commit; one stale entry must
	 * not cost the other five their delete. The skipping is the shared guard's own, so
	 * this loop adds no second opinion about which pieces an action may run against.
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
	 * SETTLING THE WALL IS THE LAST STATEMENT, AND ALL THREE HALVES OF THAT ARE LOAD-BEARING.
	 *
	 * IT CASCADES RATHER THAN MERELY SOLVING, WHICH IS THE WIRE THE GAME WENT WITHOUT.
	 * Removing a piece is not the only way a wall comes down: the load the removed pieces
	 * were carrying moves onto whatever is left, and DESIGN.md §3 is explicit that what
	 * follows a removal is an ordinary cascade. A solve alone computes that the surviving
	 * joints are past what mortar can hold and then asks none of them to give — so a
	 * staircase cut through a wall leaves eleven corbelled bricks hanging over open air, five
	 * of their bed joints reading up to 2.24 of capacity, and the wall stands there. Nothing
	 * outside a test could reach FStructure::SolveAndBreak at all until FStructureBinding
	 * grew a door onto it.
	 *
	 * EXACTLY ONE SETTLE, so a batch of ten costs what a batch of one costs rather than ten
	 * times it — and UNCONDITIONALLY, including a call that ran nothing. A cascade over a
	 * settled structure is one solve that breaks nothing, which is what SolveLoads cost here
	 * before, so the no-op case is unchanged in price as well as in effect. A rule with no
	 * exceptions is one no caller can be on the wrong side of.
	 *
	 * AND LAST, so the answer whoever pushes next is pushing saw EVERY removal AND every
	 * joint that gave because of them. FStructureBinding::ApplyResults refuses to release a
	 * piece the last solve has no answer for, and FStructure::SolveAndBreak ends on a
	 * complete solve that broke nothing — so the settled answer is exactly what a push
	 * needs, and a settle placed anywhere but here leaves the pieces the batch orphaned
	 * hanging in the air.
	 *
	 * ONCE PER PLAYER ACTION, WHICH IS A CONSTRAINT ON CALLERS AS WELL. Breaking is
	 * irreversible and stamps pass numbers that phase 5 replays as the order a collapse
	 * happened in, so a caller that cascaded again on the way to the world would stamp a
	 * second time for one click. The subsystem pushes behind this rather than re-solving,
	 * which is why it may not use SolveAndPush.
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
	 * AND THE SETTLE BELONGS HERE RATHER THAN TO ANY ROW. A per-row obligation to remember
	 * it is exactly the switch this table exists not to have, relocated; and forgetting it
	 * is the failure where the brick vanishes and the wall stands there, every piece above
	 * it still reporting itself held up by something that has gone.
	 *
	 * THE SAME CASCADE THE BATCH RUNS, AND NOT A SOLVE, which is the one thing that must not
	 * differ between the two doors. A single-brick delete that only solved while a two-brick
	 * delete cascaded would make the physics depend on how many bricks the player happened
	 * to pick — the same class of defect as the commit door once being wider than the menu,
	 * and no test would name it because both doors reach the same wall by different rules.
	 *
	 * Unconditional, because a cascade over a settled structure is one solve that breaks
	 * nothing — so a run that changed nothing costs that rather than a branch. RunPieceActions
	 * carries the reasoning in full; this is its one-element case, not a second path.
	 */
	Binding.SolveAndBreak();

	return Result;
}
