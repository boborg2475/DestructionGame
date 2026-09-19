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
 * The row carries its ref rather than the bare TArray<const FPieceAction*> the table
 * already hands back, because a presented entry is a COMMAND waiting to be chosen: a
 * presenter that kept the ref separately from the rows is one stale field away from
 * committing the right action against the wrong brick.
 *
 * The action pointer is a row of AllPieceActions(), never a copy, so "which entry did
 * they choose" stays a pointer comparison — the same promise PieceActionsFor makes.
 */
struct FPieceMenuRow
{
	/** What the button reads. Taken from the action's own Label; never spelled here. */
	FString Label;

	/** The row of AllPieceActions() this entry commits. */
	const FPieceAction* Action = nullptr;

	/**
	 * Every piece this entry commits against, in the order they were selected.
	 *
	 * Lives on the row for the same reason the ref does — a stale field away from running
	 * the right action against the wrong bricks. Never empty on a row that exists: a menu
	 * with no target is a button with nothing behind it, so no refs builds no rows at all.
	 */
	TArray<FPieceRef> Refs;

	/**
	 * The last piece added to Refs — the one the player most recently picked.
	 *
	 * Derived, not a second record: a copy of Refs.Last(), kept because most consumers
	 * want exactly one piece and neither field may disagree with what it came from
	 * (Tests/PieceMultiSelectTest.cpp asserts they agree on every presented row). Always
	 * a member of Refs — a field naming a piece outside the selection would be an anchor
	 * pointed at somebody else's brick.
	 */
	FPieceRef Ref;

	/**
	 * Whether choosing this row destroys something.
	 *
	 * The action's own flag, copied like Label is, never a comparison against the word
	 * "Delete" — a widget that styled a button by reading its caption would be a policy
	 * written in string literals no test can reach, and would silently stop styling the
	 * day the caption is retuned or a second destructive action lands.
	 */
	bool bIsDestructive = false;

	/**
	 * What this row will act on, as a noun phrase — "1 brick", "11 bricks".
	 *
	 * Singular/plural is decided here, as for CountText, so a branch in Slate doesn't have
	 * to be. Derived from Refs, the set the row actually commits against, so a button that
	 * says three bricks cannot delete four.
	 */
	FString TargetText;
};

/**
 * Turn a menu — a list of action rows and the piece they are for — into presentable rows.
 *
 * A pure function of the list: it takes no binding and consults no CanRun, because the
 * filtering already happened in PieceActionsFor and re-asking would be a second, quieter
 * copy of that policy.
 *
 * Fails closed on a ref that names nothing. A row is a command rather than a readout
 * (CURRENT_STATE.md's polarity-inversion note is exactly this seam: a value safe as a
 * diagnostic default is unsafe as an instruction) — a click that hit the floor arrives as
 * a default FPieceRef, and offering a full menu against it would put a Delete button on
 * screen with nothing behind it.
 *
 * A malformed row is skipped rather than dereferenced. AllPieceActions() cannot contain
 * one — Core.PieceActions.TableIsWellFormed sweeps for it — but this takes an array from
 * anywhere, and dereferencing a null row would abort the whole run rather than fail one test.
 */
TArray<FPieceMenuRow> BuildPieceMenuRows(
	TArrayView<const FPieceAction* const> Actions,
	const FPieceRef& Ref);

/**
 * The same, for a whole selection: one row per action, each carrying every selected piece.
 *
 * The single-ref overload above is the one-element case of this one — a menu for one
 * brick is not a different kind of menu.
 *
 * Fails closed on an empty selection and on a selection containing a ref that names
 * nothing, for the reason the single-ref version fails closed on a default ref: a command
 * with a hole in its target list is worse than no command. The whole list is refused
 * rather than the bad entry silently dropped, since dropping one would put a Delete
 * button on screen that removes fewer bricks than the player picked.
 */
TArray<FPieceMenuRow> BuildPieceMenuRows(
	TArrayView<const FPieceAction* const> Actions,
	TArrayView<const FPieceRef> Refs);

namespace DestructionPresenter
{
	/**
	 * How many Unreal force units make one newton — the OTHER conversion boundary.
	 *
	 * Not DestructionForce::ForceUnitsPerMPaSqCm, which is 10,000: that one is this factor
	 * times cm2-to-mm2, because a strength in MPa needs an area as well as a unit change.
	 * This is the bare unit change, named once here beside the formatting that is the only
	 * thing entitled to use it, rather than in Core's model layer where a divide by 100
	 * would be a second open-coded conversion.
	 */
	constexpr double ForceUnitsPerNewton = 100.0;
}

/**
 * How much room a joint has left, in the three bands a bar is drawn in.
 *
 * A bucket rather than a colour, and rather than a number the widget thresholds itself.
 * Which side of 10x margin a joint sits on is a decision, and a widget comparing
 * UtilisationPercent against two constants would be holding that decision where no test
 * can reach it. What the widget still owns is the palette: which green, amber, red.
 *
 * Critical is enumerator zero, for the reason EPieceSupport::Falling is: zero has to be
 * the value that promises least, so a default-constructed row cannot claim a joint is
 * healthy.
 */
enum class EJointMarginBand : uint8
{
	/** At or past its limit, or gone. Nothing left. */
	Critical,

	/** Getting close: worth reading before deleting anything under it. */
	Caution,

	/** Orders of magnitude from failing, which is where a settled wall lives. */
	Comfortable,
};

/**
 * One joint of the inspected brick, in the units and words a human reads.
 *
 * FJointInspection converted and worded, never recomputed. Every field below either comes
 * straight off InspectPiece's row or is that row's number in a human unit; nothing here
 * consults FConnection, a strength profile or a normal — the discipline Core/PieceInspection.h
 * states, unweakened one layer further out. A readout that re-derived a utilisation would
 * be a third copy of the break decision.
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
	 * What this joint carries, in newtons.
	 *
	 * The unit change happens here and only here. FJointInspection keeps raw uu on purpose
	 * so it can be held against the solver's own accessor with exact ==; a player reads
	 * newtons, and 1 N = 100 uu.
	 */
	double ForceN = 0.0;

	/**
	 * The bending moment this joint carries about its own centroid, in newton-centimetres.
	 *
	 * The other half of what the percentage beside it was computed from — a row showing
	 * only ForceN cannot explain its own utilisation once a load stops acting through the
	 * middle of a joint. No new conversion boundary despite "moments" sounding like one: a
	 * moment is uu.cm and length is already centimetres, so this is the same unit change
	 * ForceN makes (DestructionPresenter::ForceUnitsPerNewton).
	 *
	 * Zero on every joint of every structure the game builds today except the eccentric
	 * ones, which is why Text mentions it only when there is one to mention.
	 */
	double MomentNCm = 0.0;

	/** The same utilisation the break decision used, as a percentage. 100 % is the limit. */
	double UtilisationPercent = 0.0;

	/** How many times its load this joint could take, in words. See the tests for the states. */
	FString MarginText;

	/** The headroom bar's fill, 0 to 1. Log-scaled over three decades of margin. */
	double HeadroomFraction = 0.0;

	/** Which band that fill is in, so the bar's colour is decided here. See the tests. */
	EJointMarginBand MarginBand = EJointMarginBand::Critical;

	/**
	 * Which colour slot this ROW is, or INDEX_NONE when the palette has run out.
	 *
	 * Per slot, not per brick: the first joint row is always the first colour, whichever
	 * brick is on the end of it. A wall has 1,220 bricks and a palette a handful of hues,
	 * so a colour keyed on the far end runs out immediately; keyed on the row it never can,
	 * at the price that one brick changes colour when a different brick is inspected.
	 *
	 * INDEX_NONE past the end rather than wrapping — two rows sharing a colour is a lie
	 * about which brick is which, the one thing the swatch exists to say.
	 */
	int32 ColourSlot = INDEX_NONE;

	/** Whether this joint is still in the structure. A given joint carries nothing. */
	bool bHasGiven = false;

	/** Which cascade pass gave it, or INDEX_NONE. Only meaningful beside bHasGiven. */
	int32 BreakPass = INDEX_NONE;

	/**
	 * The whole line the widget prints, and the widget prints nothing else.
	 *
	 * The formatting is here because the widget may not have it: it was landed under a
	 * recorded exception to the TDD gate on the condition that it contains no logic, and
	 * choosing a unit, a precision, a tier word, or a different line for a joint that has
	 * given is all logic. A given joint must not read like an intact unloaded one — both
	 * are 0 N at 0 % — so the two states are different sentences here rather than a branch
	 * up there.
	 */
	FString Text;
};

/** One tick of the headroom bar's scale: what it reads, and where along the bar it sits. */
struct FHeadroomScaleTick
{
	FString Label;

	/** Position along the bar, 0 at the empty end and 1 at the full end. */
	double Fraction = 0.0;
};

/**
 * Why a brick is or is not being held up, as a BUCKET a dot can be coloured from.
 *
 * The same split EJointMarginBand already uses: the model decides which bucket, the widget
 * owns the hue. Deliberately not EPieceSupport: the solver's enumerator conflates a real
 * collapse with an absent answer on purpose (zero must promise least), and the presenter's
 * job is to undo that conflation before a human reads it — handing the enum straight out
 * would draw a freshly built wall as a column of falling bricks.
 *
 * One enumerator per word PresenterWordForSupport can say, including the two that are not
 * physical states — collapsing "not in this wall" and "not solved yet" into one grey bucket
 * is not recoverable once done. NotAPiece is enumerator zero, for the reason
 * EPieceSupport::Falling is, and is also the value PresenterWordForSupport answers for a
 * default FPieceInspection, so a default-constructed entry's bucket and word agree.
 */
enum class EPieceSupportBand : uint8
{
	/** This ref names no brick in this wall — removed, foreign, or missing a half. */
	NotAPiece,

	/** A real brick, but nobody has solved yet, so there is no answer to report. */
	NotSolved,

	/** Nothing is holding it up. */
	Falling,

	/** The solver could not route it: a knot, and the numbers beside it are not a physical claim. */
	Stranded,

	/** Held up by something that reaches the earth. */
	Supported,

	/** Resting on the earth itself. */
	Grounded,
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

	/**
	 * Whether this entry still names a piece that is in the graph.
	 *
	 * Decided here because a widget must not resolve a ref itself: without it, a brick a
	 * cascade removed, a ref naming another structure, and a live brick all present
	 * identically. It is FPieceInspection::bIsPiece, not "can the menu act on it" — a
	 * RELEASED brick is a live piece and reads true here even though Delete's CanRun
	 * refuses it; what the menu may do is PieceActionsFor's intersection, already answered
	 * by the rows going empty.
	 */
	bool bIsLivePiece = false;

	/**
	 * Why this brick is or is not being held up, in words — on EVERY row, not just the one
	 * whose joints are broken out.
	 *
	 * Eleven selected bricks are eleven identical rows without it, and finding the falling
	 * one means hovering each in turn. The same word FPieceMenuInspector::SupportText
	 * carries for the singled-out brick, asked the same way.
	 *
	 * Never empty, including for a ref that names nothing, like "No bricks selected" and
	 * "No joints" — the fail-closed word for a nameless ref is emphatically not "not
	 * solved yet", which claims the brick exists.
	 */
	FString SupportText;

	/**
	 * The same answer as a bucket, so the row's dot can be coloured without reading the word.
	 *
	 * A bucket beside the word, exactly as EJointMarginBand sits beside MarginText —
	 * otherwise a widget wanting a coloured dot has only one way to get one: comparing
	 * SupportText against string literals, a policy written where no test can reach it.
	 * May never disagree with SupportText; the tests hold the two together both ways.
	 */
	EPieceSupportBand SupportBand = EPieceSupportBand::NotAPiece;
};

/**
 * The DEBUGGER half of the presented piece menu: what is selected, and one brick's joints.
 *
 * A sibling of FPieceMenuRow rather than more fields on it, and not cosmetic: a row is a
 * COMMAND waiting to be chosen (why it carries Refs and derives Ref from them), while an
 * inspector is a READOUT. They have different lifetimes and different fail-closed
 * polarities — a menu with a hole in its target list must offer nothing, while a readout
 * with a hole in it must still tell the truth about the hole.
 *
 * The deciding case is a selection the menu cannot act on. When a cascade releases a
 * picked brick the intersection empties and BuildPieceMenuRows builds no rows at all, so
 * an inspector living on a row would vanish exactly when the player most needs to see why.
 * Built beside the rows, it survives — the surface CURRENT_STATE.md's open product
 * decision about that state would become legible on.
 */
struct FPieceMenuInspector
{
	/**
	 * What the panel calls itself, whatever state the panel is in.
	 *
	 * A constant string is still a string the widget may not spell, same as every other
	 * word on this struct. Never goes empty, including with nothing selected — a fixed
	 * panel is on screen while the selection is empty, and no heading over an empty list
	 * would read as a readout that failed.
	 */
	FString HeaderText;

	/**
	 * How many bricks are selected.
	 *
	 * The selection's own count, and it never lies. A ref that resolves to nothing, one
	 * naming another structure, one whose piece a cascade took — all still count, because
	 * the player picked that many bricks and a count that quietly shrank would be the
	 * presenter disagreeing with the highlights still on screen.
	 */
	int32 SelectedCount = 0;

	/** That count as a sentence, singular and plural decided here rather than in the widget. */
	FString CountText;

	/** One entry per selected brick, in selection order. Never reordered, never deduplicated. */
	TArray<FInspectorPieceEntry> Pieces;

	/**
	 * Whether one brick is currently singled out AND is a live piece worth breaking out.
	 *
	 * A field rather than Joints.Num() > 0, for the reason FPieceInspection::bIsPiece is a
	 * field: an isolated grounded pad is a real brick with no joints at all, and a ref
	 * naming a piece that has gone is not a brick. Drawing those the same way is the defect.
	 */
	bool bHasInspectedPiece = false;

	/** Which brick that is. Default when none — see bHasInspectedPiece. */
	FPieceRef InspectedRef;

	/**
	 * Which brick the readout under it is about, in the words the entry list uses.
	 *
	 * The readout must name its own subject: a support word and six joint lines with no
	 * brick named above them are numbers about whichever brick the player last pointed at,
	 * and a scrollable list makes it worse, since the entry the breakout belongs to can
	 * scroll out of sight while the breakout stays. The entry's own Label rather than a
	 * second derivation — two inches apart on one panel, one brick must not be "course 2 ·
	 * #1" above and anything else below.
	 *
	 * Empty exactly when no brick is singled out, like SupportText and JointsText.
	 */
	FString InspectedLabel;

	/**
	 * What that brick is — its material, its size and its mass — as one composed line:
	 * "ClayBrick · 21.5 × 10.25 × 6.5 cm · 2.7 kg".
	 *
	 * The label above says which brick and never says what it is: two pieces of one wall
	 * can differ by a whole material and a factor of four in weight and present identically,
	 * so a player asking why the timber held where the brick crushed has every number
	 * except the ones that answer it. One string, composed in the model, for the reason
	 * every other string here is — the widget that draws this was landed under a recorded
	 * exception to the TDD gate on the condition it holds no logic.
	 *
	 * The material is named by which library row it is, not by its numbers, and a piece
	 * whose material is not a row — including one nobody ever set, which is most of the
	 * pieces in this game — reads "Unknown material" rather than going blank; size and
	 * mass are known either way.
	 *
	 * Empty exactly when no brick is singled out, like SupportText, JointsText and InspectedLabel.
	 */
	FString IdentityText;

	/**
	 * What the readout region says when there is no brick to break out, and nothing when
	 * there is.
	 *
	 * A fixed panel reserves the readout's space whether or not it has one, the same rule
	 * "No bricks selected" and "No joints" already follow. Empty with nothing selected at
	 * all, a third state rather than a copy of the second: told to hover a brick when the
	 * list is empty, a player would hunt a list with nothing in it, so CountText speaks
	 * for that state instead.
	 */
	FString InspectedHintText;

	/**
	 * Why the inspected brick is or is not being held up, in words.
	 *
	 * "Nobody has solved yet" is its own sentence. EPieceSupport::Falling is deliberately
	 * both that and a real collapse — zero has to be the enumerator that promises least —
	 * so a readout taking it at face value would draw a freshly built wall as a column of
	 * falling bricks. Empty when no brick is inspected.
	 */
	FString SupportText;

	/**
	 * The same answer as a bucket, so the readout's own dot is coloured from the same fact
	 * the marked ENTRY's dot is.
	 *
	 * Must agree with the marked entry's band — the cross-check SupportText already
	 * carries. Two inches apart on one panel, a green dot on the row and an amber one over
	 * the joints would be the panel disagreeing with itself. NotAPiece when no brick is
	 * singled out, the bucket form of SupportText going empty.
	 */
	EPieceSupportBand SupportBand = EPieceSupportBand::NotAPiece;

	/** The inspected brick's joints, in ascending connection order. Empty when none is. */
	TArray<FInspectorJointRow> Joints;

	/**
	 * The joint list summed up in a sentence — including when there are none.
	 *
	 * An empty list gets a sentence, exactly as CountText's "No bricks selected" does: an
	 * isolated grounded pad is a real brick with nothing joined to it, a fact about the
	 * brick rather than an absence of data. Empty when no brick is inspected, like
	 * SupportText — there is nothing to summarise.
	 */
	FString JointsText;

	/** What the headroom bar means, in words. Empty when no bar is drawn. */
	FString HeadroomCaption;

	/** The bar's decade ticks, low to high. Empty when no bar is drawn. */
	TArray<FHeadroomScaleTick> HeadroomScale;
};

/**
 * How much of the readout the panel is asked for.
 *
 * A mode on the model rather than a collapsed slot in the widget: "which parts of the
 * readout are shown" is a decision, and a widget that hid its own joint box would hold it
 * where no test can reach it, while still paying for the rows it then threw away.
 *
 * Full is enumerator zero — the opposite reasoning from EPieceSupport's rather than a
 * departure from it. There, zero must claim least, since every other value asserts
 * something about a brick; here every enumerator is a request for a readout, and the
 * failure mode is a default request quietly suppressing numbers somebody asked for. So
 * zero is the mode that withholds nothing.
 */
enum class EPieceMenuDetail : uint8
{
	/** Everything: the per-joint table and the headroom scale that gives its bars meaning. */
	Full,

	/**
	 * The selection and why each brick is standing, without the per-joint table.
	 *
	 * What it drops is what makes the panel big: a joint row is the widest line the
	 * readout composes and there is one per joint, so the table sets both the panel's
	 * width and most of its height, while the entry rows are a fixed handful of short
	 * lines. This is the cut that buys back a third of the screen, and it costs a player
	 * nothing while they are picking bricks rather than reading numbers.
	 */
	Compact,
};

/**
 * Build the debugger for a selection, singling out one brick of it.
 *
 * World-free, like everything else the presenter decides: a binding is a plain struct, so
 * every string, number and ordering decision a player will read is reachable from a
 * headless test.
 *
 * The inspected brick must be a member of the selection and a live piece. Anything else —
 * a brick the player deselected, one a cascade removed, a ref naming another structure, a
 * ref missing either half — singles out and breaks out nothing rather than showing
 * somebody else's joints (Core/PieceMenu.h says why an anchor outside the set it anchors
 * is worse than no anchor).
 *
 * The joint breakout comes from InspectPiece and is not recomputed, including the
 * adjacency: a joint that has GIVEN is still one of the brick's joints and is exactly the
 * one a player who just pulled a brick is looking for.
 */
FPieceMenuInspector BuildPieceMenuInspector(
	const FStructureBinding& Binding,
	TArrayView<const FPieceRef> Selected,
	const FPieceRef& InspectedRef,
	EPieceMenuDetail Detail = EPieceMenuDetail::Full);

/**
 * How big the panel is, in viewport pixels, for the detail it is being drawn at.
 *
 * A function of the mode, in the presenter, rather than two file-static floats in the
 * widget: the readout sits in a slot that fills the panel, so a compact panel drawn into
 * the full-size rectangle would print fewer lines into exactly as much screen. Compact is
 * strictly smaller on both axes — narrower alone leaves a tall ribbon, shorter alone a band.
 *
 * Neither figure is picked. The full width is a measured floor plus a stated clearance:
 * 617.5 px is the longest line the readout can compose (measured by
 * World.Menu.TheReadoutFitsInsideThePanel on the ragged corbel wall), and 640 px clears
 * it; the compact width is derived the same way from the longest line COMPACT still shows.
 *
 * Fails closed to the size that hides nothing: an enumerator this does not know would have
 * the compact size suppress numbers somebody asked for, so the answer is always finite and
 * positive — a panel of no size draws no heading.
 */
FVector2D PieceMenuPanelSizePx(EPieceMenuDetail Detail);

/**
 * Where the panel opens: the top-left corner it takes when the menu is first shown.
 *
 * A pure function of the two sizes rather than an alignment on a box — a regression being
 * paid for. The panel used to be an SBox at HAlign_Right/VAlign_Center with a 24 px margin;
 * making it draggable replaced the alignment with an offset, and every corner but the
 * origin is a function of the viewport's size, unknown until layout has run — so this
 * arithmetic has to live here rather than in the widget, where a test can read it.
 *
 * The home is right-of-centre and vertically centred: the panel's right edge stands
 * MarginPx in from the viewport's right edge, and its height is centred down the screen.
 * The margin is horizontal only, since "centred" already answers the vertical question.
 *
 * Comes back already on screen: a viewport narrower than the panel, or a margin wider
 * than the room left over, would make the subtraction NEGATIVE — the panel's heading off
 * the top-left of the screen, the one corner that has to stay grabbable — so this composes
 * with ClampPanelOffset's own guard rather than duplicating its reasoning.
 *
 * Fails closed to the origin on anything degenerate: a non-finite component in either size
 * or the margin, a negative size, or a negative margin (which pushes the panel off the
 * edge it was measured from) — the same NaN-safety ClampPanelOffset states below.
 */
FVector2D PieceMenuHomeOffset(
	FVector2D PanelSizePx,
	FVector2D ViewportSizePx,
	double MarginPx);

/**
 * Where the piece menu panel's top-left corner may actually go, given where it was dragged to.
 *
 * In the presenter rather than in the Slate that anchors the panel: an SConstraintCanvas
 * offset is arithmetic, untested by construction in a widget. "The player dragged the
 * panel off the edge of the screen and can never get it back" is that recurring defect —
 * a correct layer with an untested join.
 *
 * The offset is the panel's top-left corner in viewport pixels, so the whole panel is
 * inside the viewport exactly when the offset is inside [0, Viewport - Panel] on both axes,
 * kept independent: a drag past the right edge must not also reset the vertical position.
 *
 * A panel bigger than the viewport on an axis pins to zero on that axis, the case this
 * function exists to get right: the permitted range Viewport - Panel goes negative there,
 * and an inverted range does not fail loudly. Max(Min(X, Range), 0) pins to zero while
 * Min(Max(X, 0), Range) hands back the negative bound instead — the two read identically
 * at a glance, and UE's own FMath::Clamp happens to be the first, luck rather than a
 * guarantee, so the case is pinned by a test rather than left to whichever spelling
 * somebody reaches for.
 *
 * Fails closed to the origin on anything degenerate — a non-finite component in any
 * argument, or a negative size. A NaN passes straight through FMath::Max and is replaced
 * by FMath::Min, becoming a plausible offset rather than an obvious fault.
 */
FVector2D ClampPanelOffset(
	FVector2D DesiredOffsetPx,
	FVector2D PanelSizePx,
	FVector2D ViewportSizePx);

/**
 * How hard the hardest-working joint of one piece is working, as the band a bar would be drawn in.
 *
 * The load overlay's whole model: tinting a wall by load is a decision per PIECE, and the
 * piece's own number is the WORST of its joints' — a brick whose bed joint is at 80 % and
 * head joint is unloaded is an 80 % brick, since the thing that fails is the thing that
 * fails first. The same band the inspector draws, not a second one:
 * `FInspectorJointRow::MarginBand` already decides which side of 10x and 2x margin a joint
 * sits on, so this reads `GetConnectionUtilisation` over the piece's own connections and
 * buckets the worst of them with the same function the rows use.
 *
 * A joint that has given is not counted, the one place the overlay's rule differs from the
 * row's: a row says "this joint is gone" and Critical is right for it, but a PIECE beside
 * a hole is not a piece in trouble — the joint that went carries nothing, and counting it
 * would paint every neighbour of every deleted brick red. What is left is the max over the
 * joints still IN the structure.
 *
 * Support is asked before the joints, and overrules them both ways. A piece the last solve
 * found no path to the earth for is Critical whatever its joints say — the ordinary shape
 * of a wall coming down, where all that's left on the falling brick is an unloaded joint
 * reading a fraction of a per cent. A piece that IS held up but has no live joint to read
 * — the first brick on an empty plot, or a survivor whose every joint has given — is
 * Comfortable instead.
 *
 * The order of the questions is part of the answer: no piece or a removed one, then a
 * structure nothing has solved, then support, then the joints. The first two are Critical
 * because that is EJointMarginBand's own fail-closed zero — over-promising is the
 * expensive direction on an overlay whose job is to say what is about to fall down.
 */
EJointMarginBand WorstJointBandForPiece(const FStructure& Structure, int32 PieceIndex);
