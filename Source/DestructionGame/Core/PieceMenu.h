// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Core/PieceActions.h"
#include "Core/PieceInspection.h"
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

namespace DestructionPresenter
{
	/**
	 * How many Unreal force units make one newton — the OTHER conversion boundary.
	 *
	 * NOT DestructionForce::ForceUnitsPerMPaSqCm, WHICH IS 10,000: that one is this factor
	 * times cm2-to-mm2, because a strength in MPa needs an area as well as a unit change.
	 * This is the bare unit change, it exists only so a human reads newtons instead of raw
	 * uu, and it is named ONCE here — beside the formatting that is the only thing entitled
	 * to use it — rather than in Core's model layer, where a divide by 100 would be a second
	 * open-coded conversion in a struct whose fields are checked against the solver's own.
	 */
	constexpr double ForceUnitsPerNewton = 100.0;
}

/**
 * One joint of the inspected brick, in the units and words a human reads.
 *
 * IT IS FJointInspection CONVERTED AND WORDED, NEVER RECOMPUTED. Every field below either
 * comes straight off InspectPiece's row or is that row's number in a human unit; nothing
 * here consults FConnection, a strength profile or a normal. The discipline is the one
 * Core/PieceInspection.h states and it does not weaken by being one layer further out —
 * a readout that re-derived a utilisation would be a third copy of the break decision.
 */
struct FInspectorJointRow
{
	/** The connection this row is, so a failure can be taken back to the graph. */
	int32 ConnectionIndex = INDEX_NONE;

	/** The piece at the far end. Keeps naming a piece that has been removed. */
	int32 OtherPieceIndex = INDEX_NONE;

	/** What this joint is to the inspected brick. FStructure's own decision, passed through. */
	EJointRole Role = EJointRole::None;

	/**
	 * What this joint carries, in NEWTONS.
	 *
	 * THIS IS WHERE THE UNIT CHANGE HAPPENS AND THE ONLY PLACE IT MAY. FJointInspection
	 * keeps raw uu on purpose so it can be held against the solver's own accessor with
	 * exact ==; a player reads newtons, and 1 N = 100 uu.
	 */
	double ForceN = 0.0;

	/** The same utilisation the break decision used, as a percentage. 100 % is the limit. */
	double UtilisationPercent = 0.0;

	/** Whether this joint is still in the structure. A given joint carries nothing. */
	bool bHasGiven = false;

	/** Which cascade pass gave it, or INDEX_NONE. Only meaningful beside bHasGiven. */
	int32 BreakPass = INDEX_NONE;

	/**
	 * The whole line the widget prints, and the widget prints NOTHING ELSE.
	 *
	 * THE FORMATTING IS HERE BECAUSE THE WIDGET MAY NOT HAVE IT. The menu widget was landed
	 * under a recorded exception to the TDD gate on the condition that it contains no logic,
	 * and choosing a unit, a precision, a word for a tier, or a different line for a joint
	 * that has given is all logic. A joint that has given must not read like an intact
	 * unloaded one — both are 0 N at 0 % — so the two states are different sentences here
	 * rather than a branch up there.
	 */
	FString Text;
};

/** One selected brick's entry in the list. Identity only: the joints belong to the inspected one. */
struct FInspectorPieceEntry
{
	/** The piece this entry stands for, exactly as the selection holds it. */
	FPieceRef Ref;

	/** What the entry reads. Decided here so the widget does not compose it. */
	FString Label;

	/** Whether this is the entry whose joints are broken out below. At most one ever is. */
	bool bIsInspected = false;
};

/**
 * The DEBUGGER half of the presented piece menu: what is selected, and one brick's joints.
 *
 * A SIBLING OF FPieceMenuRow RATHER THAN MORE FIELDS ON IT, and the distinction is not
 * cosmetic. A row is a COMMAND waiting to be chosen — that is why it carries Refs and
 * derives Ref from them — and an inspector is a READOUT. They have different lifetimes and
 * different fail-closed polarities: a menu with a hole in its target list must offer
 * nothing, while a readout with a hole in it must still tell the truth about the hole.
 *
 * THE DECIDING CASE IS A SELECTION THE MENU CANNOT ACT ON. When a cascade releases a picked
 * brick the intersection empties and BuildPieceMenuRows builds no rows at all — so an
 * inspector living on a row would vanish at exactly the moment the player most needs to see
 * why. Built beside the rows it survives, which is also the surface CURRENT_STATE.md's open
 * product decision about that state would become legible on.
 */
struct FPieceMenuInspector
{
	/**
	 * How many bricks are selected.
	 *
	 * IT IS THE SELECTION'S OWN COUNT AND IT NEVER LIES. A ref that resolves to nothing, one
	 * naming another structure, one whose piece a cascade took — all of them still count,
	 * because the player picked that many bricks and a count that quietly shrank would be
	 * the presenter disagreeing with the highlights still on screen.
	 */
	int32 SelectedCount = 0;

	/** That count as a sentence, singular and plural decided here rather than in the widget. */
	FString CountText;

	/** One entry per selected brick, in selection order. Never reordered, never deduplicated. */
	TArray<FInspectorPieceEntry> Pieces;

	/**
	 * Whether one brick is currently singled out AND is a live piece worth breaking out.
	 *
	 * A FIELD RATHER THAN Joints.Num() > 0, for the reason FPieceInspection::bIsPiece is a
	 * field: an isolated grounded pad is a real brick with no joints at all, and a ref naming
	 * a piece that has gone is not a brick. Drawing those the same way is the defect.
	 */
	bool bHasInspectedPiece = false;

	/** Which brick that is. Default when none — see bHasInspectedPiece. */
	FPieceRef InspectedRef;

	/**
	 * Why the inspected brick is or is not being held up, in words.
	 *
	 * "NOBODY HAS SOLVED YET" IS ITS OWN SENTENCE. EPieceSupport::Falling is both that and a
	 * real collapse, deliberately, because zero has to be the enumerator that promises least
	 * — so a readout that took it at face value would draw a freshly built wall as a column
	 * of falling bricks. Empty when no brick is inspected.
	 */
	FString SupportText;

	/** The inspected brick's joints, in ascending connection order. Empty when none is. */
	TArray<FInspectorJointRow> Joints;
};

/**
 * Build the debugger for a selection, singling out one brick of it.
 *
 * WORLD-FREE, LIKE EVERYTHING ELSE THE PRESENTER DECIDES. A binding is a plain struct, so
 * every string, number and ordering decision a player will read is reachable from a headless
 * test — which is the condition the untested widget was landed under.
 *
 * THE INSPECTED BRICK MUST BE A MEMBER OF THE SELECTION AND MUST BE A LIVE PIECE. Anything
 * else — a brick the player deselected, one a cascade removed, a ref naming another
 * structure, a ref missing either half — singles out nothing and breaks out nothing, rather
 * than showing somebody else's joints. Core/PieceMenu.h already says why an anchor outside
 * the set it anchors is worse than no anchor.
 *
 * THE JOINT BREAKOUT COMES FROM InspectPiece AND IS NOT RECOMPUTED. That includes the
 * adjacency: a joint that has GIVEN is still one of the brick's joints and is exactly the
 * one a player who just pulled a brick is looking for, so walking the solver's support
 * lists instead would silently drop it.
 */
FPieceMenuInspector BuildPieceMenuInspector(
	const FStructureBinding& Binding,
	TArrayView<const FPieceRef> Selected,
	const FPieceRef& InspectedRef);
