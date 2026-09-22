// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Core/PieceActions.h"
#include "Core/PieceInspection.h"
#include "Core/StructureBinding.h"

/**
 * One entry of a presented piece menu: label, action, and the pieces it acts on. The row
 * carries its refs so the action cannot be committed against a stale target. Action points
 * into AllPieceActions(), so identity is a pointer comparison.
 */
struct FPieceMenuRow
{
	/** Button text, from the action's own Label. */
	FString Label;

	/** The row of AllPieceActions() this entry commits. */
	const FPieceAction* Action = nullptr;

	/** Pieces this entry commits against, in selection order. Never empty on an existing row. */
	TArray<FPieceRef> Refs;

	/** Copy of Refs.Last(), the most recently picked piece. Always a member of Refs. */
	FPieceRef Ref;

	/**
	 * Whether choosing this row destroys something. Copied from the action's flag so a widget
	 * never styles by comparing the caption to "Delete".
	 */
	bool bIsDestructive = false;

	/** What this row acts on, e.g. "1 brick", "11 bricks". Derived from Refs. */
	FString TargetText;
};

/**
 * Turn a list of actions and a piece into presentable rows. Consults no CanRun; filtering
 * already happened in PieceActionsFor.
 *
 * Fails closed on a ref that names nothing: a click on the floor arrives as a default
 * FPieceRef, and a menu against it would show a Delete with nothing behind it. A null action
 * row is skipped rather than dereferenced.
 */
TArray<FPieceMenuRow> BuildPieceMenuRows(
	TArrayView<const FPieceAction* const> Actions,
	const FPieceRef& Ref);

/**
 * The same for a whole selection: one row per action, each carrying every selected piece.
 * Fails closed on an empty selection or any ref that names nothing; the whole list is refused
 * rather than dropping the bad entry, so Delete never removes fewer bricks than were picked.
 */
TArray<FPieceMenuRow> BuildPieceMenuRows(
	TArrayView<const FPieceAction* const> Actions,
	TArrayView<const FPieceRef> Refs);

namespace DestructionPresenter
{
	/**
	 * Unreal force units per newton, for display only. Not ForceUnitsPerMPaSqCm (10,000), which
	 * also includes the cm2-to-mm2 area factor for strengths in MPa.
	 */
	constexpr double ForceUnitsPerNewton = 100.0;
}

/**
 * A joint's remaining margin as one of three bar bands. Thresholds are decided here, not in
 * the widget; the widget owns only the colours. Critical is zero so a default promises least.
 */
enum class EJointMarginBand : uint8
{
	/** At or past its limit, or gone. */
	Critical,

	/** Getting close. */
	Caution,

	/** Orders of magnitude from failing. */
	Comfortable,
};

/**
 * One joint of the inspected brick in human units and words. Converted from FJointInspection,
 * never recomputed, so it cannot become another copy of the break decision.
 */
struct FInspectorJointRow
{
	/** The connection this row is, so a failure can be taken back to the graph. */
	int32 ConnectionIndex = INDEX_NONE;

	/** The piece at the far end. Keeps naming a piece that has been removed. */
	int32 OtherPieceIndex = INDEX_NONE;

	/** What this joint is to the inspected brick, from FStructure. */
	EJointRole Role = EJointRole::None;

	/**
	 * Force carried, in newtons (1 N = 100 uu). The only place the unit changes; FJointInspection
	 * keeps raw uu so it can be compared exactly against the solver.
	 */
	double ForceN = 0.0;

	/**
	 * Bending moment about the joint centroid, N.cm. Same unit change as ForceN, since length is
	 * already cm. Nonzero only on eccentric joints, so Text mentions it only then.
	 */
	double MomentNCm = 0.0;

	/** The break decision's utilisation as a percentage. 100 % is the limit. */
	double UtilisationPercent = 0.0;

	/** How many times its load this joint could take, in words. */
	FString MarginText;

	/** Headroom bar fill, 0 to 1, log-scaled over three decades of margin. */
	double HeadroomFraction = 0.0;

	/** Band of that fill, so the bar colour is decided here. */
	EJointMarginBand MarginBand = EJointMarginBand::Critical;

	/**
	 * Colour slot for this row, or INDEX_NONE when the palette runs out. Keyed on row, not brick,
	 * so it cannot run out on a large wall. No wrapping: two rows sharing a colour would be wrong.
	 */
	int32 ColourSlot = INDEX_NONE;

	/** Whether this joint has given. A given joint carries nothing. */
	bool bHasGiven = false;

	/** Which cascade pass gave it, or INDEX_NONE. */
	int32 BreakPass = INDEX_NONE;

	/**
	 * The whole line the widget prints. Formatting lives here because the widget is exempt from
	 * TDD only while logic-free. A given joint reads differently from an intact unloaded one,
	 * though both are 0 N at 0 %.
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
 * Why a brick is or is not held up, as a bucket for colouring a dot. Not EPieceSupport, which
 * deliberately conflates collapse with "not solved"; this separates them so a fresh wall does
 * not read as falling. One value per word PresenterWordForSupport can say. NotAPiece is zero and
 * matches the word for a default FPieceInspection.
 */
enum class EPieceSupportBand : uint8
{
	/** The ref names no brick in this wall (removed, foreign, or half-empty). */
	NotAPiece,

	/** A real brick, but nothing has been solved yet. */
	NotSolved,

	/** Nothing is holding it up. */
	Falling,

	/** The solver could not route it; its numbers are not a physical claim. */
	Stranded,

	/** Held up by something that reaches the earth. */
	Supported,

	/** Resting on the earth itself. */
	Grounded,
};

/** One selected brick's entry in the list. Identity only; joints belong to the inspected one. */
struct FInspectorPieceEntry
{
	/** The piece exactly as the selection holds it. */
	FPieceRef Ref;

	/** What the entry reads. */
	FString Label;

	/** Whether this entry's joints are broken out below. At most one is. */
	bool bIsInspected = false;

	/**
	 * Whether the ref still names a piece in the graph (FPieceInspection::bIsPiece). A released
	 * brick is still live here even though Delete refuses it.
	 */
	bool bIsLivePiece = false;

	/**
	 * Support word for this brick, on every row so a falling brick is findable without hovering.
	 * Never empty; a ref naming nothing does not read "not solved yet", which implies it exists.
	 */
	FString SupportText;

	/** SupportText as a bucket, for the dot colour. Always agrees with SupportText. */
	EPieceSupportBand SupportBand = EPieceSupportBand::NotAPiece;
};

/**
 * The inspector half of the piece menu: the selection and one brick's joints.
 *
 * Separate from FPieceMenuRow because a row is a command and this is a readout, with opposite
 * fail-closed rules: a command with a bad target offers nothing, a readout still reports it.
 * When a cascade releases a picked brick the menu has no rows, but the inspector survives to
 * show why.
 */
struct FPieceMenuInspector
{
	/** Panel heading. Never empty, including with nothing selected. */
	FString HeaderText;

	/**
	 * Number of selected bricks, including refs that no longer resolve, so it matches the
	 * highlights on screen.
	 */
	int32 SelectedCount = 0;

	/** That count as a sentence. */
	FString CountText;

	/** One entry per selected brick, in selection order. Never reordered or deduplicated. */
	TArray<FInspectorPieceEntry> Pieces;

	/**
	 * Whether a live brick is singled out. A field rather than Joints.Num() > 0, since an
	 * isolated grounded pad is a real brick with no joints.
	 */
	bool bHasInspectedPiece = false;

	/** Which brick that is. Default when none. */
	FPieceRef InspectedRef;

	/**
	 * The inspected brick's entry Label, so the readout names its subject even when the entry
	 * scrolls out of view. Empty when none is singled out.
	 */
	FString InspectedLabel;

	/**
	 * Material, size and mass as one line: "ClayBrick · 21.5 × 10.25 × 6.5 cm · 2.7 kg". A
	 * material that is not a library row (including unset) reads "Unknown material". Empty when
	 * none is singled out.
	 */
	FString IdentityText;

	/**
	 * Hint shown in the readout area when nothing is singled out. Empty when a brick is, and
	 * also when nothing is selected (CountText covers that).
	 */
	FString InspectedHintText;

	/**
	 * Support word for the inspected brick. "Not solved yet" is distinct from falling, since
	 * EPieceSupport::Falling means both. Empty when none is inspected.
	 */
	FString SupportText;

	/** SupportText as a bucket; agrees with the marked entry's band. NotAPiece when none. */
	EPieceSupportBand SupportBand = EPieceSupportBand::NotAPiece;

	/** The inspected brick's joints, in ascending connection order. Empty when none is. */
	TArray<FInspectorJointRow> Joints;

	/**
	 * The joint list as a sentence, including when there are no joints (a real fact about the
	 * brick). Empty when no brick is inspected.
	 */
	FString JointsText;

	/** What the headroom bar means, in words. Empty when no bar is drawn. */
	FString HeadroomCaption;

	/** The bar's decade ticks, low to high. Empty when no bar is drawn. */
	TArray<FHeadroomScaleTick> HeadroomScale;
};

/**
 * How much of the readout to build. Decided in the model so the widget holds no logic. Full is
 * zero so a default request never suppresses numbers.
 */
enum class EPieceMenuDetail : uint8
{
	/** Everything, including the per-joint table and headroom scale. */
	Full,

	/**
	 * Selection and support only, no per-joint table. The joint rows set most of the panel's
	 * width and height, so this saves about a third of the screen.
	 */
	Compact,
};

/**
 * Build the inspector for a selection, singling out one brick. World-free.
 *
 * The inspected brick must be in the selection and live; otherwise nothing is broken out. Joints
 * come from InspectPiece unchanged, including given ones, which are what a player looks for
 * after pulling a brick.
 */
FPieceMenuInspector BuildPieceMenuInspector(
	const FStructureBinding& Binding,
	TArrayView<const FPieceRef> Selected,
	const FPieceRef& InspectedRef,
	EPieceMenuDetail Detail = EPieceMenuDetail::Full);

/**
 * Panel size in viewport pixels for a detail mode. Compact is smaller on both axes. The full
 * width is 640 px, clearing the longest composed line (617.5 px, measured by
 * World.Menu.TheReadoutFitsInsideThePanel); compact is derived the same way. An unknown mode
 * fails closed to the full size.
 */
FVector2D PieceMenuPanelSizePx(EPieceMenuDetail Detail);

/**
 * Top-left offset where the panel first opens: right edge MarginPx in from the viewport's
 * right, vertically centred. Passed through ClampPanelOffset so it is always on screen. Fails
 * closed to the origin on non-finite input, a negative size or a negative margin.
 */
FVector2D PieceMenuHomeOffset(
	FVector2D PanelSizePx,
	FVector2D ViewportSizePx,
	double MarginPx);

/**
 * Clamp a dragged panel's top-left offset so the panel stays on screen: [0, Viewport - Panel]
 * per axis, independently.
 *
 * A panel larger than the viewport pins to zero on that axis. The range is negative there, and
 * Max(Min(X, Range), 0) gives zero while Min(Max(X, 0), Range) gives the negative bound, so the
 * order is pinned by a test. Fails closed to the origin on non-finite input or a negative size,
 * since a NaN through Max/Min becomes a plausible-looking offset.
 */
FVector2D ClampPanelOffset(
	FVector2D DesiredOffsetPx,
	FVector2D PanelSizePx,
	FVector2D ViewportSizePx);

/**
 * The load overlay's band for one piece: the worst band of its live joints, using the same
 * bucketing as the inspector rows. Given joints are skipped, or every neighbour of a deleted
 * brick would turn red.
 *
 * Checked in order: missing/removed piece or unsolved structure is Critical (fail closed);
 * a piece with no path to earth is Critical; a supported piece with no live joints is
 * Comfortable; otherwise the worst joint.
 */
EJointMarginBand WorstJointBandForPiece(const FStructure& Structure, int32 PieceIndex);
