// Copyright Epic Games, Inc. All Rights Reserved.


#include "DestructionGamePlayerController.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "InputTriggers.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Styling/CoreStyle.h"
#include "UObject/ConstructorHelpers.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SConstraintCanvas.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"
#include "Core/PieceActions.h"
#include "DestructionGameCameraManager.h"
#include "DestructionGameGameMode.h"
#include "RequiredContent.h"
#include "World/BrickActor.h"
#include "World/BuildModeComponent.h"
#include "World/DestructionStructureSubsystem.h"

/*
 * File-local names carry a PieceMenu prefix: an anonymous namespace is private to a
 * translation unit, not a file, and a unity build merges files into one, so two colliding
 * file-local names are a hard compile error between files that never refer to each other.
 * See CURRENT_STATE.md.
 */
namespace
{
	/** The subsystem holding this world's walls, or null. There is no world in a bare CDO. */
	UDestructionStructureSubsystem* PieceMenuSubsystemOf(const AActor& Actor)
	{
		UWorld* const World = Actor.GetWorld();

		return World != nullptr ? World->GetSubsystem<UDestructionStructureSubsystem>() : nullptr;
	}

	/**
	 * The brick standing for this ref, or null.
	 *
	 * Resolved rather than indexed, so a ref naming a piece that has gone (what every ref
	 * becomes once a commit runs) answers null instead of reaching a tombstoned slot;
	 * GetActor already handles INDEX_NONE and any other kind of gone, so the cast is the
	 * only check left.
	 */
	ABrickActor* PieceMenuBrickForRef(UDestructionStructureSubsystem* Subsystem, const FPieceRef& Ref)
	{
		if (Subsystem == nullptr)
		{
			return nullptr;
		}

		const FStructureBinding* const Binding = Subsystem->Find(Ref.StructureId);

		if (Binding == nullptr)
		{
			return nullptr;
		}

		return Cast<ABrickActor>(Binding->GetActor(Binding->ResolvePiece(Ref)));
	}

	/**
	 * Whether a structure has anything in it worth commanding — one reading, used twice.
	 *
	 * Live pieces rather than piece count, since RemovePiece tombstones instead of compacting
	 * and an emptied plot still answers a nonzero count. GetSessionStructureId and
	 * RefreshSessionHasStructure ask this from two places; without one shared answer, a build
	 * that has had pieces and has none left could get picked by the first and read as empty by
	 * the second, greying Run over a wall standing in front of the player.
	 */
	bool SessionStructureIsLive(const FStructureBinding* Binding)
	{
		return Binding != nullptr && Binding->GetStructure().NumLivePieces() > 0;
	}
}

/*
 * The priority the contexts are applied at, named because it is now used twice: a restore at a
 * different priority would silently change which context wins a shared key. Not a bare
 * `MappingContextPriority` because a unity build shares this file-scope name with every other
 * file in the module — see CURRENT_STATE.md.
 */
static constexpr int32 PieceMenuMappingContextPriority = 0;

/*
 * The session's context goes above the other two, which is what makes the camera turn at all.
 *
 * IMC_MouseLook's Mouse2D mapping is chorded on IA_LookModifier, which IMC_Session maps to the
 * right mouse button. A chord reads the modifier's trigger state, and
 * UEnhancedPlayerInput::EvaluateInputImpl resets that state at the end of the frame (its own
 * comment: "Delay ... reset ... to allow dependent triggers (e.g. chords) access to this tick's
 * values"), so the modifier must be evaluated earlier in the same frame than the mapping that
 * chords off it, or the chord reads last frame's cleared state and free-look never triggers.
 *
 * A priority is the only lever that reaches across two contexts — ReorderMappings only orders
 * chording-before-chorded within one context, and across contexts the sort decides nothing
 * between equals; at one shared priority a probe measured 0° of yaw. One priority above fixes it.
 */
static constexpr int32 SessionMappingContextPriority = PieceMenuMappingContextPriority + 1;

/*
 * How far a ray cast from the cursor reaches, in cm (1 uu = 1 cm), i.e. 100 m.
 *
 * One reach for both handlers, because they are the same ray: what a click would hit and what
 * the cursor is pointing at must be the same brick. It lives in the untested handlers' half, so
 * it is a reach rather than a tuned threshold — the game mode's wall is about 6.6 m across and
 * 3 m tall, and a flying observer is expected to be tens of metres off it. Nothing downstream
 * depends on the value: the trace either hits a brick or it does not, and a miss dismisses.
 */
static constexpr double PieceMenuCursorReachCm = 10000.0;

/*
 * What an entry row is drawn in, keyed on the one bool the model already decided.
 *
 * FInspectorPieceEntry::bIsLivePiece exists so a brick a cascade removed and a live one do not
 * present identically, and it lives on the model so nothing here has to resolve a ref to find
 * out. Reading it into a colour is the whole use of it — no filtering, no dropping the entry;
 * whether the menu can still act on it is PieceActionsFor's intersection, already said by the
 * action rows going empty.
 *
 * File-scope names carry the prefix for the unity-build reason noted above.
 */
static const FLinearColor PieceMenuLivePieceColour(1.0f, 1.0f, 1.0f, 1.0f);
static const FLinearColor PieceMenuDeadPieceColour(0.5f, 0.5f, 0.5f, 0.6f);

/*
 * The panel's size is not here any more, and its absence is the point.
 *
 * A panel that cannot change size cannot move anything, which every stillness claim about this
 * menu rests on: whatever is showing, every row stays where it was, so a click aimed at one
 * commits that one. The size is now a function of the detail mode — EPieceMenuDetail::Compact
 * drops the joint table and the headroom scale, and a mode that drew its remaining lines into
 * the same rectangle gave the player back no screen at all, which was their complaint.
 *
 * It lives in Core now, as PieceMenuPanelSizePx, beside the derivation of both figures:
 * Presenter.PieceMenuPanelSize holds the orderings and area budget, and
 * World.Menu.TheReadoutFitsInsideThePanel checks the rectangle it lands in against that answer.
 * Two constants here would be a second copy with no test between them.
 */

/**
 * How far in from the viewport's right edge the panel opens.
 *
 * The old figure, coming back as an argument. The panel used to be an SBox at HAlign_Right /
 * VAlign_Center with a 24 px margin, kept there to hold the readout off the wall the player is
 * pointing at; making it draggable replaced the alignment with an offset defaulting to the
 * origin. PieceMenuHomeOffset takes this as a parameter rather than a spelled-out constant, so
 * Presenter.PanelHomeOffset can sweep no margin, this one, one wider than the screen, and a
 * negative one — a value passed in is a value a test can vary.
 */
static constexpr double PieceMenuPanelHomeMarginPx = 24.0;

/** The gap between the panel's background and anything drawn on it. */
static constexpr float PieceMenuPanelPaddingPx = 10.0f;

/**
 * How tall the brick list may get, which is what makes a long selection survivable.
 *
 * About eight rows. Past that the list scrolls inside this height rather than growing, so forty
 * picked bricks push nothing off the bottom of the screen and move neither the readout nor the
 * row that deletes them.
 *
 * A cap rather than a fixed height, because a fixed height reserved all eight rows for three
 * bricks, stranding about 140 px of empty panel between the last brick and the readout — a fifth
 * of the panel, reading as a menu that had failed to finish drawing. Capping stops the list
 * growing past this figure while letting a short selection take only the room it needs.
 */
static constexpr float PieceMenuBrickListMaxHeightPx = 190.0f;

/*
 * The headroom bar's track: one size for every joint, so the bars read as a column — and the
 * width is set by the scale under it rather than the bar.
 *
 * The decade labels sit along this width, each straddling its own fraction of it, so the four
 * must fit side by side with air between them: at the scale's font, the two crowded ones at the
 * top end left a third of a pixel between them on the 96 px track this replaces — "100×1000×"
 * rendered as one string on the axis whose entire job is to say which decade a fill means.
 *
 * 140 px leaves 15 px between the tightest pair, against the 4 px
 * World.Menu.HeadroomTicksStayInsideTheBarTheyLabel asks for. It is bounded from the other side
 * too: the bar and swatch column together take 166 px off the front of the joint sentence beside
 * them, leaving only 22 px once World.Menu.TheReadoutFitsInsideThePanel sweeps a wall with a
 * corbel in it. A wider bar has to come out of that.
 */
static constexpr float PieceMenuHeadroomBarWidthPx = 140.0f;
static constexpr float PieceMenuHeadroomBarHeightPx = 8.0f;

/*
 * The swatch that ties a joint row to the brick on the far end of it.
 *
 * A joint row already names its neighbour in words — "course 2 · #4" — and in a wall of 1,220
 * identical bricks a word is not enough to find one by. FInspectorJointRow::ColourSlot is the
 * model's answer to which colour each row takes; this is the size of the block it is painted
 * in, and the gap to the bar. The scale row below carries the same total as a left padding, so
 * the ticks stay under the fills they label rather than under the swatches.
 */
static constexpr float PieceMenuJointSwatchWidthPx = 10.0f;
static constexpr float PieceMenuJointSwatchHeightPx = 10.0f;
static constexpr float PieceMenuJointSwatchGapPx = 6.0f;

/*
 * How wide the column of support words on the brick rows is.
 *
 * Wide enough for the longest of them — "not in this wall" — with room to spare, so the words
 * line up in a column instead of ragging off the ends of labels of different lengths. It is
 * deliberately generous: the entry rows are swept by World.Menu.TheReadoutFitsInsideThePanel
 * along with everything else the model supplies, and a column that just fitted would make an
 * entry row the tightest line on the panel, quietly retargeting that test's reported budget away
 * from the joint sentences it exists to measure.
 */
static constexpr float PieceMenuEntrySupportWidthPx = 150.0f;

/** The row the bar's decade ticks are placed along, directly under that column. */
static constexpr float PieceMenuHeadroomScaleHeightPx = 14.0f;

/** The rule that separates the destructive row from everything describing what it destroys. */
static constexpr float PieceMenuRuleHeightPx = 1.0f;

/*
 * How wide the scenario banner is, and how far off the top of the screen it sits.
 *
 * A stated width rather than a fit to the text, because the expectation lines run to two full
 * sentences and a banner sized to its content would stretch to viewport width and put a
 * two-hundred-character line across the top of the wall. Wrapped inside a fixed width, the same
 * text is three or four readable lines. Centred at the top is the one region the piece-menu panel
 * never opens into — it homes against the right edge — so the two readouts cannot overlap.
 */
static constexpr float ScenarioBannerWidthPx = 760.0f;
static constexpr float ScenarioBannerTopMarginPx = 24.0f;

/*
 * What the panel is drawn in, and the background is the one colour that is not decoration.
 *
 * Every line below the brick rows used to be a bare STextBlock over whatever the camera was
 * pointing at — legible against a wall, invisible against the sky, and no headless test can see
 * the difference because nothing here paints a pixel. A near-opaque dark fill behind the whole
 * panel is what makes the readout readable at all; the rest of these are contrast against it.
 */
static const FLinearColor PieceMenuPanelBackgroundColour(0.014f, 0.016f, 0.022f, 0.94f);
static const FLinearColor PieceMenuHeaderColour(1.0f, 1.0f, 1.0f, 1.0f);
static const FLinearColor PieceMenuCountColour(0.62f, 0.68f, 0.78f, 1.0f);
static const FLinearColor PieceMenuReadoutColour(0.82f, 0.86f, 0.92f, 1.0f);
static const FLinearColor PieceMenuHintColour(0.55f, 0.60f, 0.68f, 1.0f);
static const FLinearColor PieceMenuRuleColour(1.0f, 1.0f, 1.0f, 0.16f);
static const FLinearColor PieceMenuHeadroomTrackColour(0.0f, 0.0f, 0.0f, 0.55f);

/*
 * What the title strip is tinted, which is the only thing saying the panel can be moved.
 *
 * A lift off the panel's own background rather than a colour of its own: it has to read as part
 * of the panel and as a separate strip at the same time, which is what a title bar is. The other
 * half of the affordance is the grab cursor; there is deliberately no word for it, since a word
 * would be chosen in the one place no test can read it, and FPieceMenuInspector is where the
 * panel's words are decided.
 */
static const FLinearColor PieceMenuGrabStripColour(0.16f, 0.18f, 0.24f, 0.75f);

/*
 * The three band colours are not here any more, and their absence is the point — the same move
 * the neighbour palette made, for the same reason, one slice later.
 *
 * EJointMarginBand says where the colour changes — which side of 10x and of 2x margin a joint
 * falls on — because that is a decision about what this game calls dangerous, and belongs where
 * a test can read it. What was left here was the hue; the load overlay then needed the same three
 * hues in three materials, which are content and cannot reach a file-static in a widget. Two
 * copies of green would have been a brick tinted one green beside a bar drawn another.
 *
 * DestructionContent::BrickLoadSwatchColours now sits beside the three material paths it must
 * agree with, one row per band, with deliberately no colour literal left here for a bar to drift
 * back to.
 */

/*
 * And what each support bucket's dot is drawn in, on the same terms as the bar above.
 *
 * EPieceSupportBand says which bucket a brick is in — the model's decision, swept against the
 * word beside it — and this is the hue, the half nothing headless can judge. Forty picked bricks
 * are forty lines of small text without it, and the one that is falling reads exactly like the
 * thirty-nine that are not until somebody reads every word.
 *
 * The alarm colours are the bar's own, deliberately: a falling brick takes the same red a joint
 * past its limit does, and a stranded one the same amber as a joint running out of room, because
 * one panel wants one vocabulary rather than two. The other three — resting, held, and the two
 * that are not claims about a brick at all — sit outside that vocabulary, so nothing calm can be
 * mistaken for an alarm.
 */
static const FLinearColor PieceMenuSupportNotAPieceColour(0.36f, 0.37f, 0.40f, 1.0f);
static const FLinearColor PieceMenuSupportNotSolvedColour(0.45f, 0.55f, 0.78f, 1.0f);
static const FLinearColor PieceMenuSupportFallingColour(0.95f, 0.24f, 0.20f, 1.0f);
static const FLinearColor PieceMenuSupportStrandedColour(0.95f, 0.66f, 0.13f, 1.0f);
static const FLinearColor PieceMenuSupportSupportedColour(0.18f, 0.76f, 0.55f, 1.0f);
static const FLinearColor PieceMenuSupportGroundedColour(0.22f, 0.56f, 0.86f, 1.0f);

/** How big the dot on a brick row is, and how far the word beside it stands off. */
static constexpr float PieceMenuSupportDotSizePx = 8.0f;
static constexpr float PieceMenuSupportDotGapPx = 6.0f;

/*
 * The neighbour palette is not here any more, and its absence is the point.
 *
 * It used to be a file-static array of six colours here, with a comment claiming they were
 * "exactly" the emissives of Content/Materials/M_BrickNeighbour0..5 — a second copy with only
 * that sentence tying it to the first. A palette repick changed three of the assets and left
 * the sentence, so the swatch column drew amber, chartreuse and teal beside bricks lit green,
 * clay and sage. DestructionContent::BrickNeighbourSwatchColours now sits beside the material
 * paths it must agree with, one row per slot, held together by
 * Content.NeighbourSwatchesMatchTheirMaterials, with no colour literal left here to drift back to.
 *
 * Six is the model's number: a brick inside a running bond has six joints. A row past the end
 * carries INDEX_NONE and gets the transparent entry below, so the swatch is absent rather than
 * repeated — a repeated swatch would be a wrong answer about which brick is which.
 */

/** What a row past the end of the palette is painted in: nothing at all. */
static const FLinearColor PieceMenuNoSwatchColour(0.0f, 0.0f, 0.0f, 0.0f);

/*
 * And what a row that destroys something is drawn in, taken from FPieceMenuRow::bIsDestructive.
 *
 * The flag is the action's own, carried across by the presenter, so this is a colour keyed on
 * data rather than a widget comparing a caption against the word "Delete" — the policy-in-a-
 * string-literal that FPieceAction::bIsDestructive exists to make unnecessary.
 */
static const FLinearColor PieceMenuDestructiveRowColour(0.72f, 0.16f, 0.14f, 1.0f);
static const FLinearColor PieceMenuOrdinaryRowColour(1.0f, 1.0f, 1.0f, 1.0f);

/*
 * The session strip's own measurements, from SESSION_UI_DESIGN.md §e.
 *
 * 48 px tall is an owner ruling rather than a fit: the first cut was 72 and was "way too big". A
 * 34 px chip inside it leaves 7 px of air above and below, which is what makes the strip read as
 * a bar with buttons on it rather than a row of buttons.
 *
 * Nothing here is measured by any test — the panel tests already draw that division of labour:
 * which buttons, in what order, greyed or live, focusable or not is the model's and is asserted;
 * every pixel below is this file's and is checked by a human against the screenshot proof.
 */
static constexpr float SessionToolbarHeightPx = 48.0f;
static constexpr float SessionToolbarChipHeightPx = 34.0f;
static constexpr float SessionToolbarChipGapPx = 5.0f;
static constexpr float SessionToolbarChipPaddingPx = 12.0f;
static constexpr float SessionToolbarEdgePaddingPx = 10.0f;
static constexpr float SessionToolbarReadoutPaddingPx = 6.0f;

/*
 * The hairline between two groups, and the air either side of it. §e's 10 px between groups,
 * spent as the chip gap on the left of the rule and the rest on its right.
 */
static constexpr float SessionToolbarRuleWidthPx = 1.0f;
static constexpr float SessionToolbarRuleGapPx = 10.0f;

/*
 * And the two piece swatches, which are two shapes as well as two colours.
 *
 * The plank is longer and thinner than the block, which is what makes the three piece chips
 * readable from each other without reading the words — the whole reason §e draws a swatch "in
 * place of a size caption". The proportions are roughly the pieces' own: a brick is about twice
 * as long as it is tall in elevation and a board is three times that.
 */
static constexpr float SessionToolbarBrickSwatchWidthPx = 18.0f;
static constexpr float SessionToolbarBrickSwatchHeightPx = 11.0f;
static constexpr float SessionToolbarTimberSwatchWidthPx = 26.0f;
static constexpr float SessionToolbarTimberSwatchHeightPx = 8.0f;
static constexpr float SessionToolbarSwatchGapPx = 7.0f;

/*
 * What the strip is drawn in, and the fill is one step lighter than the panel's on purpose.
 *
 * The details window sits on PieceMenuPanelBackgroundColour and the strip sits on this, so the
 * two read as separate objects rather than one dark shape with a seam in it. Both are the
 * design's linear triples, the form Slate takes — the sRGB hexes in §e are what the eye checks
 * them against, and confusing the two is how a palette drifts.
 *
 * The accents and chip fills are not here any more, and their absence is the point. They used to
 * be two file-static colours and a pair of ternaries in the panel builder — "how a chip is drawn"
 * decided in the one place no test can reach — and the multiply through FCoreStyle's grey button
 * brush meant the amber this project chose was never the amber a player saw. The whole look is
 * DestructionSession::ChipLookFor's answer now, swept by Core.SessionToolbar.ChipLook, and this
 * file turns it into a brush.
 */
static const FLinearColor SessionToolbarFillColour(0.020f, 0.023f, 0.030f, 0.96f);

/*
 * A second file-local namespace, below the constants it reads rather than beside the one at the
 * top of the file. Everything here draws the panel and needs a size or colour declared above, so
 * the split is declaration order rather than a second grouping. The PieceMenu prefix is the same
 * unity-build rule noted above.
 */
namespace
{
	/*
	 * The style comes from FCoreStyle rather than FAppStyle, deliberately. FAppStyle resolves to
	 * whichever style the running application registered — the editor's in an editor binary, the
	 * core one in a cooked game — so a panel styled through it would look different in the two
	 * places this menu is seen. FCoreStyle is the same in both and needs no content asset, which
	 * keeps the background off RequiredContent's table.
	 */
	const FSlateBrush* PieceMenuFillBrush()
	{
		return FCoreStyle::Get().GetBrush("WhiteBrush");
	}

	FSlateFontInfo PieceMenuHeaderFont()
	{
		return FCoreStyle::GetDefaultFontStyle("Bold", 13);
	}

	FSlateFontInfo PieceMenuBodyFont()
	{
		return FCoreStyle::GetDefaultFontStyle("Regular", 9);
	}

	FSlateFontInfo PieceMenuSmallFont()
	{
		return FCoreStyle::GetDefaultFontStyle("Regular", 7);
	}

	/**
	 * The two caption faces; which one a chip wears is the model's answer.
	 *
	 * The chip says `bActive` twice — in the caption's weight and in the chip's fill — because
	 * they are two readings of one decision, and either alone is fragile: a player reading the
	 * strip from peripheral vision sees the fill, and one looking straight at it reads the word.
	 * Which face a chip gets is FChipLook::bBoldCaption rather than a ternary here, for the same
	 * reason the fill is FChipLook::Fill.
	 */
	FSlateFontInfo SessionToolbarBoldFont()
	{
		return FCoreStyle::GetDefaultFontStyle("Bold", 11);
	}

	FSlateFontInfo SessionToolbarRegularFont()
	{
		return FCoreStyle::GetDefaultFontStyle("Regular", 11);
	}

	/**
	 * The same fill one step toward white, which is how a chip answers the cursor.
	 *
	 * Clamped at white rather than lerped, so a channel already at full stays put instead of the
	 * whole colour drifting. The clamp is FMath::Min, which replaces a NaN rather than discarding
	 * it — the right direction here, since a look that is not a number must stay not a number
	 * rather than become a plausible colour; Core.SessionToolbar.ChipLook sweeps every look for
	 * finiteness so one can never arrive.
	 */
	FLinearColor SessionToolbarLiftedFill(const FLinearColor& Fill)
	{
		constexpr float LiftPerChannel = 0.12f;

		return FLinearColor(
			FMath::Min(1.0f, Fill.R + LiftPerChannel),
			FMath::Min(1.0f, Fill.G + LiftPerChannel),
			FMath::Min(1.0f, Fill.B + LiftPerChannel),
			Fill.A);
	}

	/** And the same fill pushed down, which is how it answers the press. */
	FLinearColor SessionToolbarPressedFill(const FLinearColor& Fill)
	{
		constexpr float PressScale = 0.82f;

		return FLinearColor(Fill.R * PressScale, Fill.G * PressScale, Fill.B * PressScale, Fill.A);
	}

	/**
	 * A chip's whole style, built from the look the model decided.
	 *
	 * A rounded box brush rather than FCoreStyle's button brush with a colour multiplied through
	 * it — this is the defect the slice exists to close: the stock brush is grey, so multiplying
	 * the design's amber into it produces dark mustard, and no retune of the constant could fix
	 * that because the thing being multiplied into is grey. The brush has to be the chip's own.
	 *
	 * All four states are rounded. SButton swaps its border brush for the state it is in, so a
	 * square disabled or pressed brush would be a chip that changed shape under the cursor.
	 */
	FButtonStyle SessionToolbarChipStyle(const DestructionSession::FChipLook& Look)
	{
		/* All four corners the same; FVector4 rather than FVector4f is FSlateBrushOutlineSettings' own type. */
		const FVector4 Radii(
			Look.CornerRadiusPx, Look.CornerRadiusPx, Look.CornerRadiusPx, Look.CornerRadiusPx);

		FButtonStyle Style;

		Style.SetNormal(
			FSlateRoundedBoxBrush(Look.Fill, Radii, Look.Outline, Look.OutlineWidthPx));

		Style.SetHovered(
			FSlateRoundedBoxBrush(
				SessionToolbarLiftedFill(Look.Fill), Radii, Look.Outline, Look.OutlineWidthPx));

		Style.SetPressed(
			FSlateRoundedBoxBrush(
				SessionToolbarPressedFill(Look.Fill), Radii, Look.Outline, Look.OutlineWidthPx));

		Style.SetDisabled(
			FSlateRoundedBoxBrush(Look.Fill, Radii, Look.Outline, Look.OutlineWidthPx));

		return Style;
	}

	/** How big a swatch of this kind is drawn: the plank longer and thinner than the block. */
	FVector2f SessionToolbarSwatchSizePx(DestructionSession::EToolbarSwatch Swatch)
	{
		return Swatch == DestructionSession::EToolbarSwatch::Timber
			? FVector2f(SessionToolbarTimberSwatchWidthPx, SessionToolbarTimberSwatchHeightPx)
			: FVector2f(SessionToolbarBrickSwatchWidthPx, SessionToolbarBrickSwatchHeightPx);
	}

	/**
	 * The little block of colour that makes a piece chip look like the thing it lays.
	 *
	 * One widget, and its size is its own padding. A border with nothing inside it is a filled
	 * rectangle whose desired size is exactly the padding around the nothing, which lets the
	 * swatch be a single widget rather than a sizing box wrapped around an image — a single
	 * widget is what a reader, human or test, can point at and call "the swatch".
	 *
	 * The colour is the model's: SwatchColour(Kind) is the shed material's own base colour, so the
	 * palette chip and the brick that lands are one decision rather than two people picking the
	 * same red.
	 */
	TSharedRef<SWidget> SessionToolbarSwatchBlock(DestructionSession::EToolbarSwatch Swatch)
	{
		const FVector2f SizePx = SessionToolbarSwatchSizePx(Swatch);

		return SNew(SBorder)
			.BorderImage(PieceMenuFillBrush())
			.BorderBackgroundColor(DestructionSession::SwatchColour(Swatch))
			.Padding(FMargin(0.5f * SizePx.X, 0.5f * SizePx.Y));
	}

	/**
	 * The 1 px rule that separates two regions of the strip.
	 *
	 * Drawn where the model's group changes and nowhere else, per §b: the commands sit past a rule
	 * "so that a destructive click is never adjacent to a setting click". Full chip height, so it
	 * reads as a division of the bar rather than a tick.
	 */
	TSharedRef<SWidget> SessionToolbarGroupRule()
	{
		return SNew(SImage)
			.Image(PieceMenuFillBrush())
			.ColorAndOpacity(PieceMenuRuleColour)
			.DesiredSizeOverride(
				FVector2D(SessionToolbarRuleWidthPx, SessionToolbarChipHeightPx));
	}

	/**
	 * What a bar in this band is filled in — a lookup, which is all a widget may do with it.
	 *
	 * The band arrived decided: Presenter.PieceMenuJointMarginBand pins which side of each edge
	 * every joint falls on. Nothing here compares a number against anything, so there is no
	 * second copy of that rule to drift, and the arm past the end of the enumeration answers
	 * with the most severe colour, since a bar wrong about its own band must not look calm.
	 *
	 * The table is the overlay's, which is what makes the bar and the brick one decision: the
	 * three colours live beside the three load-overlay material paths in RequiredContent.h,
	 * indexed by the band itself, a subscript rather than a second palette.
	 */
	FLinearColor PieceMenuBandColour(EJointMarginBand Band)
	{
		const int32 Index = static_cast<int32>(Band);

		const bool bKnown =
			Index >= 0 && Index < UE_ARRAY_COUNT(DestructionContent::BrickLoadSwatchColours);

		return DestructionContent::BrickLoadSwatchColours[
			bKnown ? Index : static_cast<int32>(EJointMarginBand::Critical)];
	}

	/**
	 * What a brick row's support dot is painted in — the same shape of lookup, on the same terms.
	 *
	 * The bucket arrived decided: Presenter.PieceMenuSupportBand pins which bucket every state of
	 * every brick falls in, and CheckInspectorInvariants holds each row's bucket against that
	 * row's own word over every readout the suite builds. Nothing here compares a string, a
	 * support enumerator or a live-piece flag against anything, so there is no second copy of that
	 * rule to drift, and the arm past the end of the enumeration answers with the grey that claims
	 * nothing, because a dot that is wrong about its own bucket must not assert a physical state.
	 */
	FLinearColor PieceMenuSupportColour(EPieceSupportBand Band)
	{
		switch (Band)
		{
		case EPieceSupportBand::NotAPiece: return PieceMenuSupportNotAPieceColour;
		case EPieceSupportBand::NotSolved: return PieceMenuSupportNotSolvedColour;
		case EPieceSupportBand::Falling:   return PieceMenuSupportFallingColour;
		case EPieceSupportBand::Stranded:  return PieceMenuSupportStrandedColour;
		case EPieceSupportBand::Supported: return PieceMenuSupportSupportedColour;
		case EPieceSupportBand::Grounded:  return PieceMenuSupportGroundedColour;
		}

		return PieceMenuSupportNotAPieceColour;
	}

	/**
	 * The dot that says whether a picked brick is standing up, without its row being read.
	 *
	 * It sits inside the support column rather than at the head of the row, a layout decision
	 * with a measured reason: the column is a fixed-width box, so a dot placed inside it takes its
	 * space out of that box's own slack and moves nothing else on the row — a dot ahead of the
	 * entry button would instead push every label and word right by its width, out of the budget
	 * World.Menu.TheReadoutFitsInsideThePanel measures.
	 */
	TSharedRef<SWidget> PieceMenuSupportDot(EPieceSupportBand Band)
	{
		return SNew(SBox)
			.WidthOverride(PieceMenuSupportDotSizePx)
			.HeightOverride(PieceMenuSupportDotSizePx)
			[
				SNew(SImage)
				.Image(PieceMenuFillBrush())
				.ColorAndOpacity(PieceMenuSupportColour(Band))
			];
	}

	/**
	 * What a joint row's swatch is painted in — the same shape of lookup, on the same terms.
	 *
	 * Which slot a row takes is the model's answer and is swept over every readout in the suite;
	 * a slot outside the palette is INDEX_NONE by that answer's own rule, and the bounds check
	 * here is the lookup's rather than a policy of its own. It fails to the transparent entry, so
	 * an unknown slot draws no swatch instead of borrowing somebody else's colour.
	 */
	FLinearColor PieceMenuSwatchColour(int32 ColourSlot)
	{
		const TArrayView<const FLinearColor> Palette(DestructionContent::BrickNeighbourSwatchColours);

		return Palette.IsValidIndex(ColourSlot) ? Palette[ColourSlot] : PieceMenuNoSwatchColour;
	}

	/**
	 * The block of colour that ties a joint row to its neighbouring brick.
	 *
	 * It is drawn even when invisible, which is why the transparent colour goes through the same
	 * widget rather than a slot that is not added. The bars must line up in a column for the
	 * decade scale under them to mean anything, and a row that skipped its swatch would slide its
	 * bar 16 px left of every other one.
	 */
	TSharedRef<SWidget> PieceMenuJointSwatch(int32 ColourSlot)
	{
		return SNew(SBox)
			.WidthOverride(PieceMenuJointSwatchWidthPx)
			.HeightOverride(PieceMenuJointSwatchHeightPx)
			[
				SNew(SImage)
				.Image(PieceMenuFillBrush())
				.ColorAndOpacity(PieceMenuSwatchColour(ColourSlot))
			];
	}

	/**
	 * One joint's headroom bar, filled to the fraction the model worked out.
	 *
	 * The fill is a laid-out child rather than a painted rectangle, the whole reason this is not
	 * an SProgressBar: SProgressBar keeps its Percent in a private slate attribute with no
	 * getter, so nothing could read back what it drew, while an anchored child's arranged width
	 * is `HeadroomFraction` times the track's, exactly, readable by a headless test with no
	 * renderer. The model's number goes straight into the anchor with no arithmetic and no
	 * clamp: FInspectorJointRow::HeadroomFraction is already a fraction, log-scaled over three
	 * decades, and swept for finiteness by Presenter.PieceMenuJointHeadroom.
	 *
	 * The colour comes from the band rather than the fraction, the same rule one field over: how
	 * full the bar is and how alarmed to be about it are two answers, and only the first is a
	 * length. Thresholding the fraction here would be the second answer written where nothing
	 * can read it.
	 */
	TSharedRef<SWidget> PieceMenuHeadroomBar(double HeadroomFraction, EJointMarginBand Band)
	{
		return SNew(SBox)
			.WidthOverride(PieceMenuHeadroomBarWidthPx)
			.HeightOverride(PieceMenuHeadroomBarHeightPx)
			[
				SNew(SBorder)
				.BorderImage(PieceMenuFillBrush())
				.BorderBackgroundColor(PieceMenuHeadroomTrackColour)
				.Padding(0.0f)
				[
					SNew(SConstraintCanvas)
					+ SConstraintCanvas::Slot()
					.Anchors(FAnchors(0.0f, 0.0f, static_cast<float>(HeadroomFraction), 1.0f))
					.Offset(FMargin(0.0f))
					.Alignment(FVector2D::ZeroVector)
					[
						SNew(SImage)
						.Image(PieceMenuFillBrush())
						.ColorAndOpacity(PieceMenuBandColour(Band))
					]
				]
			];
	}

	/**
	 * The bar's decade ticks, each standing where the model put it.
	 *
	 * A log axis with no decades on it is unreadable by construction — the same visible fill
	 * means 1000x on one panel and 3x on another. FHeadroomScaleTick carries a Fraction as well
	 * as a label so this can place each one by the same curve the fill is drawn by, rather than
	 * spreading four labels evenly and promising a linear scale it does not have.
	 *
	 * The anchor is that same arithmetic: PieceMenuHeadroomBar anchors its fill to Fraction of a
	 * track this wide, and a tick anchors its label to Fraction of a canvas the same width, so
	 * the joint whose margin is 10x has its fill end under the 10x label because both came out
	 * of one expression, not because two constants happen to agree.
	 *
	 * Alignment is the fraction rather than a half, which keeps the end labels on the bar.
	 * SConstraintCanvas reads Alignment as the pivot inside the child, so alignment 0.5 would
	 * centre the ticks at 0.0 and 1.0 on the track's two edges, half of each clipped off; the
	 * tick's own Fraction as pivot instead pins each label's near edge at its own end, so the
	 * label always straddles the point it names and stays within the track.
	 */
	TSharedRef<SWidget> PieceMenuHeadroomScale(const TArray<FHeadroomScaleTick>& Scale)
	{
		TSharedRef<SConstraintCanvas> Ticks = SNew(SConstraintCanvas);

		for (const FHeadroomScaleTick& Tick : Scale)
		{
			Ticks->AddSlot()
				.Anchors(FAnchors(static_cast<float>(Tick.Fraction), 0.0f))
				.Offset(FMargin(0.0f))
				.Alignment(FVector2D(Tick.Fraction, 0.0))
				.AutoSize(true)
				[
					SNew(STextBlock)
					.Font(PieceMenuSmallFont())
					.ColorAndOpacity(PieceMenuHintColour)
					.Text(FText::FromString(Tick.Label))
				];
		}

		return SNew(SBox)
			.WidthOverride(PieceMenuHeadroomBarWidthPx)
			.HeightOverride(PieceMenuHeadroomScaleHeightPx)
			[
				Ticks
			];
	}
}

ADestructionGamePlayerController::ADestructionGamePlayerController()
{
	// set the player camera manager class
	PlayerCameraManagerClass = ADestructionGameCameraManager::StaticClass();

	/*
	 * Wire up the mapping contexts here rather than in a Blueprint, so the sandbox runs from C++
	 * defaults alone — by the paths RequiredContent.h names, so this constructor and the
	 * required-content table cannot become two lists that disagree.
	 */
	static ConstructorHelpers::FObjectFinder<UInputMappingContext> DefaultContext(DestructionContent::DefaultMappingContextPath);
	static ConstructorHelpers::FObjectFinder<UInputMappingContext> MouseLookContext(DestructionContent::MouseLookMappingContextPath);

	/*
	 * The look context is remembered by name as well as applied — the same pointer in both
	 * places, so there is nothing to drift; its header says why it still has a name of its own.
	 */
	MouseLookMappingContext = MouseLookContext.Object;

	DefaultMappingContexts.Add(DefaultContext.Object);
	DefaultMappingContexts.Add(MouseLookMappingContext);

	/*
	 * And the session's own keyboard, pushed in beside them rather than applied separately, so
	 * the code that already applies the other two applies this one too — an apply written a
	 * second way is an apply a later route can forget. Named as well as listed, for the reason
	 * MouseLookMappingContext is, and here the name carries a second job: it is what the apply
	 * loop asks "is this the session's?" to give it SessionMappingContextPriority instead of the
	 * shared one (see that priority's own header for why it cannot be the shared one).
	 */
	static ConstructorHelpers::FObjectFinder<UInputMappingContext> SessionContext(DestructionContent::SessionMappingContextPath);

	SessionMappingContext = SessionContext.Object;

	DefaultMappingContexts.Add(SessionMappingContext);

	/* The piece menu's own input, by the same one spelling of its path. */
	static ConstructorHelpers::FObjectFinder<UInputAction> InspectPieceActionAsset(DestructionContent::InspectPieceActionPath);

	InspectPieceAction = InspectPieceActionAsset.Object;

	/* And the one that keeps the highlight under the cursor, mapped in IMC_Default beside it. */
	static ConstructorHelpers::FObjectFinder<UInputAction> HoverPieceActionAsset(DestructionContent::HoverPieceActionPath);

	HoverPieceAction = HoverPieceActionAsset.Object;

	/*
	 * And the eight session shortcuts, by the same one spelling of each path.
	 *
	 * IA_LookModifier is not among them, deliberately: it does nothing on its own, existing only
	 * to be the action IMC_MouseLook's chord watches, so there is no handler for it to reach and a
	 * reference here would be a UPROPERTY nothing ever reads.
	 */
	static ConstructorHelpers::FObjectFinder<UInputAction> SessionToggleModeAsset(DestructionContent::SessionToggleModeActionPath);
	static ConstructorHelpers::FObjectFinder<UInputAction> SessionPieceBrickAsset(DestructionContent::SessionPieceBrickActionPath);
	static ConstructorHelpers::FObjectFinder<UInputAction> SessionPiecePlateAsset(DestructionContent::SessionPiecePlateActionPath);
	static ConstructorHelpers::FObjectFinder<UInputAction> SessionPieceLintelAsset(DestructionContent::SessionPieceLintelActionPath);
	static ConstructorHelpers::FObjectFinder<UInputAction> SessionSnapToggleAsset(DestructionContent::SessionSnapToggleActionPath);
	static ConstructorHelpers::FObjectFinder<UInputAction> SessionCourseUpAsset(DestructionContent::SessionCourseUpActionPath);
	static ConstructorHelpers::FObjectFinder<UInputAction> SessionCourseDownAsset(DestructionContent::SessionCourseDownActionPath);
	static ConstructorHelpers::FObjectFinder<UInputAction> SessionRunAsset(DestructionContent::SessionRunActionPath);

	SessionToggleModeAction = SessionToggleModeAsset.Object;
	SessionPieceBrickAction = SessionPieceBrickAsset.Object;
	SessionPiecePlateAction = SessionPiecePlateAsset.Object;
	SessionPieceLintelAction = SessionPieceLintelAsset.Object;
	SessionSnapToggleAction = SessionSnapToggleAsset.Object;
	SessionCourseUpAction = SessionCourseUpAsset.Object;
	SessionCourseDownAction = SessionCourseDownAsset.Object;
	SessionRunAction = SessionRunAsset.Object;

	/*
	 * The build loop is part of what a controller is, so it is a default subobject rather than
	 * something a level or a Blueprint attaches. A session whose Build tab found no component
	 * would put the player in a mode where every click fails closed and nothing on screen says why.
	 */
	BuildComponent = CreateDefaultSubobject<UBuildModeComponent>(TEXT("BuildComponent"));

	/*
	 * And the session opens in Destroy, deliberately not the model's own default:
	 * FSessionToolbarState::Mode defaults to Build because a default-constructed session must be
	 * the one that cannot destroy anything, but a controller is a different question — most
	 * playable levels lay a structure and invite the player to pull it apart, and opening those
	 * in Build mode would hang a gold ghost over somebody else's wall. The one build plot is put
	 * into Build mode by the game mode, through the same single door every other click goes through.
	 */
	SessionToolbarState.Mode = DestructionSession::ESessionMode::Destroy;
}

UBuildModeComponent* ADestructionGamePlayerController::GetBuildComponent() const
{
	return BuildComponent;
}

const DestructionSession::FSessionToolbarState&
	ADestructionGamePlayerController::GetSessionToolbarState() const
{
	return SessionToolbarState;
}

int32 ADestructionGamePlayerController::GetSessionStructureId() const
{
	UDestructionStructureSubsystem* const Subsystem = PieceMenuSubsystemOf(*this);

	if (Subsystem == nullptr)
	{
		return INDEX_NONE;
	}

	/*
	 * The player's own build wins, but only once there is something live in it.
	 *
	 * BeginBuild spends an id on an empty binding the moment Build mode is entered, so "the
	 * component names a structure" is true long before there is anything to command. An empty
	 * build that won here would shadow the level's own wall with nothing — Run would solve an
	 * empty graph and report success on a level with a wall standing in front of the player. A
	 * build whose every brick has been deleted is the same emptiness wearing a piece count, which
	 * is why the question goes through SessionStructureIsLive rather than a second way here.
	 */
	if (BuildComponent != nullptr)
	{
		const int32 BuildId = BuildComponent->GetStructureId();

		if (SessionStructureIsLive(Subsystem->Find(BuildId)))
		{
			return BuildId;
		}
	}

	/*
	 * Otherwise the level's own wall, which is what makes a Destroy session on a scenario level
	 * anything but inert. A row that built nothing — the build plot — leaves INDEX_NONE here, and
	 * the strip greys both commands until the player's first brick lands.
	 */
	const UWorld* const World = GetWorld();

	const ADestructionGameGameMode* const GameMode =
		World != nullptr ? World->GetAuthGameMode<ADestructionGameGameMode>() : nullptr;

	return GameMode != nullptr ? GameMode->GetBuiltStructureId() : INDEX_NONE;
}

void ADestructionGamePlayerController::RefreshSessionHasStructure()
{
	UDestructionStructureSubsystem* const Subsystem = PieceMenuSubsystemOf(*this);

	const FStructureBinding* const Binding =
		Subsystem != nullptr ? Subsystem->Find(GetSessionStructureId()) : nullptr;

	SessionToolbarState.bHasStructure = SessionStructureIsLive(Binding);
}

void ADestructionGamePlayerController::RefreshLoadOverlay()
{
	/*
	 * What was tinted is remembered before anything is recomputed, and it is the half that has to
	 * be: a brick stops wearing a band without being touched at all — the overlay goes off, the
	 * session moves to another structure, a piece is pulled out — so a refresh that told only the
	 * new set would leave the old one coloured by a solve nobody can date. Same obligation, same
	 * shape, as RefreshNeighbourHighlights' "were neighbours" argument.
	 */
	const int32 WasStructureId = LoadOverlayStructureId;
	const int32 WasCount = LoadOverlayStates.Num();

	LoadOverlayStructureId = INDEX_NONE;
	LoadOverlayStates.Reset();

	UDestructionStructureSubsystem* const Subsystem = PieceMenuSubsystemOf(*this);

	const int32 StructureId = GetSessionStructureId();

	FStructureBinding* const Binding =
		SessionToolbarState.bLoadOverlay && Subsystem != nullptr ? Subsystem->Find(StructureId) : nullptr;

	if (Binding != nullptr)
	{
		/*
		 * A structure that already holds an answer is read, never re-solved — the second solve
		 * would be harmful, not merely wasteful. A settle runs SolveAndBreak, whose equilibrium
		 * gate makes the LP the support authority below the block cap; a bare SolveLoads has no
		 * gate and rebuilds weaker per-piece arrays from the router's flood alone, so an
		 * unconditional refresh would overwrite the settle's verdict — looking at a wall would
		 * change what it does next.
		 *
		 * So the rule is "solve when there is no answer", not "never solve": a freshly built plot
		 * has never been solved, and neither has a piece placed after a settle, since the old
		 * pieces carry the settle's answer and the new one carries none. Asking per live piece
		 * catches both, and it is non-destructive when it does solve — SolveLoads leaves every
		 * connection as intact as it found it; SolveAndBreak is the deliberate step, never
		 * reached from here.
		 */
		bool bAnyPieceWithoutAnAnswer = false;

		for (int32 Index = 0; Index < Binding->NumPieces(); ++Index)
		{
			if (Binding->IsPieceRemoved(Index) || Binding->IsReleased(Index))
			{
				continue;
			}

			if (!Binding->GetStructure().HasSupportAnswer(Index))
			{
				bAnyPieceWithoutAnAnswer = true;
				break;
			}
		}

		if (bAnyPieceWithoutAnAnswer)
		{
			Binding->SolveLoads();
		}

		LoadOverlayStructureId = StructureId;
		LoadOverlayStates.Reserve(Binding->NumPieces());

		for (int32 Index = 0; Index < Binding->NumPieces(); ++Index)
		{
			/*
			 * A hole wears nothing, and neither does a brick that has already gone. A removed piece
			 * has no actor to tint and WorstJointBandForPiece fails it closed to Critical, the right
			 * answer for a reading of a handle and the wrong one to paint: the brick is gone, so
			 * there is nothing there to be in trouble.
			 *
			 * A released piece is the same fact one step earlier — a rigid body falling through the
			 * air under Chaos, its joints saying nothing about it any more, so it too reads Critical
			 * for want of support. Painting that would spend the instrument's loudest signal on a
			 * brick the player can already see moving, when the whole point of the red is to pick out
			 * the one that has not moved yet.
			 */
			LoadOverlayStates.Add(
				Binding->IsPieceRemoved(Index) || Binding->IsReleased(Index)
					? EBrickHighlight::None
					: BrickHighlightForLoadBand(
						WorstJointBandForPiece(Binding->GetStructure(), Index)));
		}
	}

	/*
	 * Then the union of the old set and the new one is put back through the precedence. Nothing
	 * here paints: every brick is asked afresh what state it should be in, so a hovered or
	 * selected brick keeps what it had and an unclaimed one falls to its band — or to None, which
	 * is what taking the overlay off means.
	 *
	 * The old set first and the new set second, and the bricks in both are simply told twice.
	 * That is harmless for the same reason RefreshNeighbourHighlights' overlap is: SetHighlighted
	 * is idempotent and HighlightForPiece is asked afresh each time, so a second telling cannot
	 * say anything different from the first.
	 */
	for (int32 Index = 0; Index < WasCount; ++Index)
	{
		FPieceRef Ref;
		Ref.StructureId = WasStructureId;
		Ref.PieceIndex = Index;

		RefreshPieceHighlight(Ref);
	}

	for (int32 Index = 0; Index < LoadOverlayStates.Num(); ++Index)
	{
		FPieceRef Ref;
		Ref.StructureId = LoadOverlayStructureId;
		Ref.PieceIndex = Index;

		RefreshPieceHighlight(Ref);
	}
}

bool ADestructionGamePlayerController::OnToolbarButton(DestructionSession::EToolbarButtonId Id)
{
	using namespace DestructionSession;

	/*
	 * What there is to command is asked first, before the strip is asked what is live: `bHasStructure`
	 * is a fact about the world, not a player choice, and asked after the greying check, Run would
	 * be refused on the first click after the build became real and accepted on the second — a
	 * dropped click.
	 */
	RefreshSessionHasStructure();

	/*
	 * And the refusal is the model's, asked rather than re-decided: ApplyToolbarButton consults
	 * the same list for the same answer, so the only thing left to decide is whether the side
	 * effect runs, and it must not — a greyed `Course down` that still decremented would put the
	 * build plane under the earth with the readout saying course 0.
	 */
	const TArray<FToolbarButton> Buttons = SessionToolbarButtons(SessionToolbarState);

	const FToolbarButton* const Button = Buttons.FindByPredicate(
		[Id](const FToolbarButton& Candidate) { return Candidate.Id == Id; });

	if (Button == nullptr || !Button->bEnabled)
	{
		return false;
	}

	SessionToolbarState = ApplyToolbarButton(SessionToolbarState, Id);

	/*
	 * The settings are pushed from the state the transition produced, never from the button:
	 * `CourseUp` means "one more than whatever the course was", and the state is where that sum
	 * already lives, so the component is told the number, not the gesture — the only reading
	 * that stays right if a transition starts refusing or clamping something. The component
	 * derives the rest itself: SetPieceKind works out the material, half extent and plane;
	 * grounded is derived from the snapped pose inside the subsystem (DESIGN §8, 2026-09-15)
	 * rather than pushed.
	 */
	switch (Id)
	{
	case EToolbarButtonId::ModeBuild:
		/*
		 * A build is opened if one is not already. Build mode with no structure behind it is a
		 * mode in which every click fails closed against an unknown id — and opening one
		 * unconditionally would be worse, because BeginBuild cancels: a player who looked at a
		 * brick in Destroy mode and came back would find their plot swept.
		 */
		if (BuildComponent != nullptr && BuildComponent->GetStructureId() == INDEX_NONE)
		{
			BuildComponent->BeginBuild();
		}

		/*
		 * And nothing is done about the cursor, a change and not an omission. This used to raise
		 * it, since there is no aiming a ghost without a pointer. The pointer is the session's
		 * now — SetSessionControls raises it once in BeginPlay and nothing lowers it — so a mode
		 * that raised it would be a mode that owned it, and the mode that did not would take it
		 * away again (SESSION_UI_DESIGN §d, S6).
		 */
		break;

	case EToolbarButtonId::ModeDestroy:
		/*
		 * No ghost survives into Destroy mode: a gold brick hanging in the air over a wall the
		 * player is demolishing is the most confusing thing this UI can do. The build survives —
		 * CancelBuild is one call away, and "leaving Build mode" reads like a reason to make it,
		 * which would hand the player a fresh empty plot every time they looked at a brick.
		 */
		if (BuildComponent != nullptr)
		{
			BuildComponent->HidePreview();
		}

		/* The cursor stays where it is: the strip is on screen in Destroy mode too. */
		break;

	case EToolbarButtonId::PieceBrick:
	case EToolbarButtonId::PieceTimberPlate:
	case EToolbarButtonId::PieceTimberLintel:
		if (BuildComponent != nullptr)
		{
			BuildComponent->SetPieceKind(SessionToolbarState.Piece);
		}
		break;

	case EToolbarButtonId::RotatePiece:
		/*
		 * The state's flag, read after the transition, exactly as the piece and the course are.
		 * The chip is a toggle and the model is where that flip already happened, so the component
		 * is told which way the next piece lies rather than that a chip was clicked — and it
		 * derives the swapped footprint itself, keeping the rotation and the palette one answer.
		 */
		if (BuildComponent != nullptr)
		{
			BuildComponent->SetRotated(SessionToolbarState.bRotated);
		}
		break;

	case EToolbarButtonId::PlacementSnap:
	case EToolbarButtonId::PlacementFree:
		if (BuildComponent != nullptr)
		{
			BuildComponent->SetPlacementMode(SessionToolbarState.Placement);
		}
		break;

	case EToolbarButtonId::JointAuto:
	case EToolbarButtonId::JointMortar:
	case EToolbarButtonId::JointDry:
	case EToolbarButtonId::JointNail:
	case EToolbarButtonId::JointScrew:
	case EToolbarButtonId::JointBolt:
		/*
		 * The state's choice, read after the transition, exactly as the piece and the course are.
		 * The component is told which joint, never which chip — the profile is looked up at the
		 * door rather than here, so nothing between the strip and the placement holds a library
		 * address it could get wrong.
		 */
		if (BuildComponent != nullptr)
		{
			BuildComponent->SetJointChoice(SessionToolbarState.Joint);
		}
		break;

	case EToolbarButtonId::CourseDown:
	case EToolbarButtonId::CourseUp:
		if (BuildComponent != nullptr)
		{
			BuildComponent->SetCourse(SessionToolbarState.Course);
		}
		break;

	case EToolbarButtonId::ClearBuild:
		/*
		 * Clear is "start again" rather than "stop building", so a fresh plot is left open behind
		 * it. BeginBuild cancels whatever is open first — bricks, binding and all — so the two
		 * halves are one call rather than a cancel this function could forget to follow.
		 */
		if (BuildComponent != nullptr)
		{
			BuildComponent->BeginBuild();
		}
		break;

	case EToolbarButtonId::RunStructure:
		if (UDestructionStructureSubsystem* const Subsystem = PieceMenuSubsystemOf(*this))
		{
			Subsystem->SolveAndPush(GetSessionStructureId());
		}

		/*
		 * And the overlay is recomputed against what is left. A settle breaks joints and releases
		 * pieces, so every band on screen is a reading of a structure that no longer exists — a
		 * green brick over a gap is worse advice than none.
		 */
		RefreshLoadOverlay();
		break;

	case EToolbarButtonId::ToggleLoadOverlay:
		/*
		 * The flag is already the transition's; this is the world catching up with it, in both
		 * directions — the tint goes on, or it comes off every brick that was wearing one.
		 */
		RefreshLoadOverlay();
		break;
	}

	/*
	 * And what there is to command is asked again, because a command changes it. Clear leaves an
	 * empty plot, so the strip has to grey Clear and Run again on the way out of the very click
	 * that emptied it — a strip still offering them would be offering a command over nothing.
	 */
	RefreshSessionHasStructure();
	RefreshSessionToolbar();

	return true;
}

bool ADestructionGamePlayerController::ToggleSessionMode()
{
	using namespace DestructionSession;

	/*
	 * The read is the whole function, and it is the only decision in the session's keyboard. A
	 * toggle that always dispatched one id is a key that takes the player into whichever mode it
	 * favours and then appears to jam. Everything else — whether the button is drawn, whether it
	 * is greyed, what it does to the build component — is already the one door's.
	 */
	return OnToolbarButton(
		SessionToolbarState.Mode == ESessionMode::Build
			? EToolbarButtonId::ModeDestroy
			: EToolbarButtonId::ModeBuild);
}

bool ADestructionGamePlayerController::ToggleSessionPlacement()
{
	using namespace DestructionSession;

	/*
	 * And the same shape for Snap/Free, where the door earns its keep. Neither placement chip is
	 * on the Destroy strip, so this must be refused there — written as "set the other value" it
	 * would flip a setting in a mode that does not draw it, and the player would come back to
	 * Build to find bricks landing wherever the cursor is. OnToolbarButton consults the same list
	 * the strip greys from, so the key and the chip refuse together.
	 */
	return OnToolbarButton(
		SessionToolbarState.Placement == EPlacementMode::Snap
			? EToolbarButtonId::PlacementFree
			: EToolbarButtonId::PlacementSnap);
}

void ADestructionGamePlayerController::PointerAlongRay(const FVector& StartCm, const FVector& EndCm)
{
	if (SessionToolbarState.Mode == DestructionSession::ESessionMode::Build)
	{
		/*
		 * The component is given a direction, not an end point, and it intersects that with the
		 * build plane itself. Handing it the end point would place the ghost wherever the ray was
		 * cut off rather than where it meets the course the player is laying on.
		 */
		if (BuildComponent != nullptr)
		{
			BuildComponent->UpdatePreviewFromRay(StartCm, (EndCm - StartCm).GetSafeNormal());
		}

		return;
	}

	HoverAlongRay(StartCm, EndCm);
}

bool ADestructionGamePlayerController::PrimaryAlongRay(const FVector& StartCm, const FVector& EndCm)
{
	if (SessionToolbarState.Mode != DestructionSession::ESessionMode::Build)
	{
		/* Destroy mode's click is the one this controller has always had. */
		return InspectAlongRay(StartCm, EndCm).Num() > 0;
	}

	if (BuildComponent == nullptr)
	{
		return false;
	}

	/*
	 * The pose is taken again from this ray before it is committed, rather than trusting whatever
	 * the last pointer move held. A preview predicts the commit only while the binding is
	 * unchanged, and the previous click changed it — so a click that reused a stale preview would
	 * lay the second brick at the pose the first one was going to take.
	 */
	const FVector Direction = (EndCm - StartCm).GetSafeNormal();

	BuildComponent->UpdatePreviewFromRay(StartCm, Direction);

	const FPieceRef Placed = BuildComponent->ConfirmPlace();

	/*
	 * And again afterwards, along the same ray, because the commit spent the one it just used.
	 * With the mouse still there is no pointer event to re-drive it — the tick's refresh is
	 * throttled on the cursor's pixel position and skips until the player jogs the mouse — so
	 * without this the gold ghost stands inside the red brick it just laid with no preview behind
	 * it. Asked again on the changed binding, the solver drops the pose now occupied and answers
	 * the next one, which is what "show where the brick is going to go" means during a run of clicks.
	 */
	BuildComponent->UpdatePreviewFromRay(StartCm, Direction);

	/* A brick landing is what turns an empty plot into something Clear and Run can act on. */
	RefreshSessionHasStructure();
	RefreshSessionToolbar();

	/*
	 * And a new brick is a new load path. The overlay survives a trip through Build mode, so a
	 * piece laid while it is on has to be given a band of its own, and the pieces it now stands on
	 * have to be read again — that is the whole of what laying a brick does to a structure.
	 */
	RefreshLoadOverlay();

	return Placed.StructureId != INDEX_NONE && Placed.PieceIndex != INDEX_NONE;
}

bool ADestructionGamePlayerController::RefreshBuildPreviewFromRay(
	const FVector& OriginCm,
	const FVector& Direction)
{
	/*
	 * Destroy mode is a no-op, checked first. This runs every frame the cursor moves, so a refresh
	 * that leaked the mode would hang a gold ghost over the wall the player is demolishing and
	 * re-arm a preview a stray confirm could commit — exactly the ghost that switching to Destroy
	 * hides. Reporting false rather than falling through to the hover keeps the two cursor jobs
	 * apart: pointing in Destroy mode is IA_HoverPiece's, through PointerAlongRay.
	 */
	if (SessionToolbarState.Mode != DestructionSession::ESessionMode::Build)
	{
		return false;
	}

	if (BuildComponent == nullptr)
	{
		return false;
	}

	/*
	 * And the same seam a pointer move uses, so a refreshed ghost and a hovered one cannot come to
	 * different answers about where the click would land. The component intersects the ray with
	 * the build plane itself and fails closed on a miss, which is what makes the return value honest.
	 */
	return BuildComponent->UpdatePreviewFromRay(OriginCm, Direction).bValid;
}

TArray<FPieceMenuRow> ADestructionGamePlayerController::InspectAlongRay(
	const FVector& StartCm,
	const FVector& EndCm)
{
	TArray<FPieceMenuRow> Rows;

	/*
	 * Which bricks the readout is pointing at, before a click moves anything. Toggling a brick
	 * out of the selection can stop another brick being singled out — so the readout empties and
	 * every colour it handed out has to come back, and none of those bricks is otherwise touched
	 * by anything below.
	 */
	const TArray<FPieceRef> WereNeighbours = NeighbourPieces();

	UDestructionStructureSubsystem* const Subsystem = PieceMenuSubsystemOf(*this);

	/*
	 * The whole chain is already written and none of it is repeated here. TracePiece fails
	 * closed on every step from the trace to the re-resolve, so a miss arrives as a default ref;
	 * PieceActionsFor resolves every ref again against the binding and answers an empty menu for
	 * one that names nothing. This is the wire between them, not a third opinion.
	 */
	if (Subsystem != nullptr)
	{
		const FPieceHit Hit = Subsystem->TracePiece(StartCm, EndCm);

		/*
		 * A click happens at the cursor, so this ray is also the answer to what is under it.
		 * Saying so here rather than waiting for the next mouse-move is what stops a brick
		 * staying lit after it has been clicked away from, or a cleared selection leaving the
		 * last brick pointed at still called out.
		 */
		SetHoveredPiece(Hit.Ref);

		/*
		 * Clicking a brick toggles it, and clicking past everything clears the lot. The
		 * selection is the durable state and the menu is a projection of it rebuilt below, which
		 * is why there is no branch here that shows or dismisses anything: an empty selection
		 * builds no rows, and an empty row list is already how a menu comes down.
		 */
		if (Hit.PieceHandle != INDEX_NONE)
		{
			PieceSelection.Toggle(Hit.Ref);

			RefreshPieceHighlight(Hit.Ref);
		}
		else
		{
			ClearPieceSelection();
		}

		/*
		 * One menu for the whole selection, against the structure its refs name. A selection is
		 * built by clicking one wall, so the first ref names it and PieceActionsFor refuses the
		 * rest piece by piece if it ever does not.
		 */
		const TArrayView<const FPieceRef> Selected = PieceSelection.Refs();

		if (Selected.Num() > 0)
		{
			if (const FStructureBinding* const Binding = Subsystem->Find(Selected[0].StructureId))
			{
				Rows = BuildPieceMenuRows(PieceActionsFor(*Binding, Selected), Selected);
			}
		}
	}

	/*
	 * Every route out of here presents, including the ones that found nothing — no world, no
	 * subsystem, a ray that hit the floor, a brick standing for a piece that has gone. That is
	 * what makes "the ray hit nothing" and "take the menu down" the same call rather than two,
	 * and it is the whole reason this is one ShowPieceMenu at the end instead of an early return
	 * per guard: a route that simply returned would leave the previous brick's menu on screen
	 * naming a brick the player is no longer pointing at.
	 */
	ShowPieceMenu(Rows);

	/* After the menu has been shown, so the state this leaves the wall in is the final one. */
	RefreshNeighbourHighlights(WereNeighbours);

	return Rows;
}

FPieceRef ADestructionGamePlayerController::HoverAlongRay(
	const FVector& StartCm,
	const FVector& EndCm)
{
	UDestructionStructureSubsystem* const Subsystem = PieceMenuSubsystemOf(*this);

	/*
	 * Pointing at a brick is not choosing it, so nothing here touches the selection, opens a menu
	 * or closes one — the only thing that changes is which brick is called out. No world and no
	 * subsystem is the same answer as a ray that hit nothing: a default ref, which lets go of
	 * whatever was called out before.
	 */
	const FPieceHit Hit = Subsystem != nullptr ? Subsystem->TracePiece(StartCm, EndCm) : FPieceHit();

	SetHoveredPiece(Hit.Ref);

	return Hit.Ref;
}

const FPieceSelection& ADestructionGamePlayerController::GetPieceSelection() const
{
	return PieceSelection;
}

EBrickHighlight ADestructionGamePlayerController::HighlightForPiece(const FPieceRef& Ref) const
{
	/*
	 * The stronger state wins where they coincide, in the order Inspected > Selected > Hovered.
	 * A selected brick under the cursor stays Selected: a hover that overwrote it would make a
	 * chosen brick read as unchosen exactly when the player is looking at it. The one brick
	 * whose joint forces are on screen beats the rest of the selection, since a breakout of its
	 * numbers drawn beside five identical-looking bricks would otherwise be ambiguous about
	 * which one it is the breakout of.
	 *
	 * A brick that is not selected cannot be the one being read, so this asks the selection as
	 * well as the ref — the same rule BuildPieceMenuInspector applies, since an anchor outside
	 * the set it anchors is a readout of somebody else's brick, and the panel and the wall must
	 * agree which brick the numbers are about.
	 *
	 * It is not a substitute for clearing the ref: the conjunct makes a stale InspectedPiece
	 * inert only while the brick is out of the selection, and the ref itself survives, so
	 * picking that brick again would spring the readout back open on it for no visible reason.
	 * DismissPieceMenu is where it is actually let go of.
	 */
	if (InspectedPiece == Ref && PieceSelection.Contains(Ref))
	{
		return EBrickHighlight::Inspected;
	}

	if (PieceSelection.Contains(Ref))
	{
		return EBrickHighlight::Selected;
	}

	/*
	 * Then the readout's own colours, between the selection and the cursor.
	 *
	 * Selected beats neighbour: a picked brick on the far end of a joint row keeps its selection
	 * colour, even though the neighbour hue is the only thing tying a row of numbers to a brick
	 * in a wall of identical bricks. It loses because the one thing a player must be able to
	 * check before pressing Delete is which bricks are going, and a brick that quietly stops
	 * looking picked while the cursor runs down a list is that check taken away at the worst
	 * possible moment — deleting is irreversible, losing a hue is not.
	 *
	 * Neighbour beats hovered for the opposite reason: while the readout is open the cursor is
	 * on the panel, so HoveredPiece is stale by construction, and a stale answer overwriting a
	 * live one would turn the brick a row is pointing at back to the hover colour.
	 */
	const EBrickHighlight Neighbour = NeighbourHighlightForPiece(Ref);

	if (Neighbour != EBrickHighlight::None)
	{
		return Neighbour;
	}

	if (HoveredPiece == Ref)
	{
		return EBrickHighlight::Hovered;
	}

	/*
	 * And the load overlay last, where a state that covers every piece at once has to sit.
	 * Everything above says "this one" and the overlay says something about all of them, so an
	 * overlay that beat any of those would take away the one check a player must be able to make
	 * before pressing Delete.
	 *
	 * It is asked here rather than painted on: a refresh that called SetHighlighted on every
	 * brick would fight the cursor and lose — the next refresh would repaint over the hover and
	 * selection, and the hover's own refresh would leave a brick plain forever after. One
	 * function decides where states coincide; the overlay is one more question it asks, last.
	 */
	return LoadHighlightForPiece(Ref);
}

EBrickHighlight ADestructionGamePlayerController::LoadHighlightForPiece(const FPieceRef& Ref) const
{
	/*
	 * The structure is checked as well as the index, for the reason NeighbourHighlightForPiece
	 * checks it: piece 4 of every wall on screen is not piece 4 of the one the overlay solved.
	 */
	if (Ref.StructureId != LoadOverlayStructureId || !LoadOverlayStates.IsValidIndex(Ref.PieceIndex))
	{
		return EBrickHighlight::None;
	}

	return LoadOverlayStates[Ref.PieceIndex];
}

EBrickHighlight ADestructionGamePlayerController::NeighbourHighlightForPiece(
	const FPieceRef& Ref) const
{
	const FPieceMenuInspector Inspector = PieceMenuInspectorForSelection();

	/*
	 * The rows belong to the inspected brick, so the far end belongs to its structure. Comparing
	 * the piece index alone would light brick 4 of every wall on screen the moment brick 4 of this
	 * one became a neighbour, the same fail-open shape FStructureBinding::ResolvePiece refuses a
	 * foreign ref for.
	 */
	if (Ref.StructureId != Inspector.InspectedRef.StructureId)
	{
		return EBrickHighlight::None;
	}

	for (const FInspectorJointRow& Row : Inspector.Joints)
	{
		if (Row.OtherPieceIndex == Ref.PieceIndex)
		{
			return BrickHighlightForNeighbourSlot(Row.ColourSlot);
		}
	}

	return EBrickHighlight::None;
}

void ADestructionGamePlayerController::RefreshNeighbourHighlights(
	TArrayView<const FPieceRef> WereNeighbours)
{
	/*
	 * The old set first and the new set second, which matters for the bricks in both. A brick
	 * that is a neighbour before and after may have changed slot, and refreshing it twice is
	 * harmless only because SetHighlighted is idempotent and HighlightForPiece is asked afresh
	 * each time — the same property SetHoveredPiece's pair of refreshes already leans on.
	 */
	for (const FPieceRef& WasNeighbour : WereNeighbours)
	{
		RefreshPieceHighlight(WasNeighbour);
	}

	for (const FPieceRef& Neighbour : NeighbourPieces())
	{
		RefreshPieceHighlight(Neighbour);
	}
}

TArray<FPieceRef> ADestructionGamePlayerController::NeighbourPieces() const
{
	const FPieceMenuInspector Inspector = PieceMenuInspectorForSelection();

	TArray<FPieceRef> Neighbours;
	Neighbours.Reserve(Inspector.Joints.Num());

	for (const FInspectorJointRow& Row : Inspector.Joints)
	{
		FPieceRef& Neighbour = Neighbours.AddDefaulted_GetRef();
		Neighbour.StructureId = Inspector.InspectedRef.StructureId;
		Neighbour.PieceIndex = Row.OtherPieceIndex;
	}

	return Neighbours;
}

void ADestructionGamePlayerController::RefreshPieceHighlight(const FPieceRef& Ref)
{
	if (ABrickActor* const Brick = PieceMenuBrickForRef(PieceMenuSubsystemOf(*this), Ref))
	{
		Brick->SetHighlighted(HighlightForPiece(Ref));
	}
}

void ADestructionGamePlayerController::SetHoveredPiece(const FPieceRef& Ref)
{
	/*
	 * The brick being left is refreshed as well as the one being pointed at, and refreshed
	 * rather than simply cleared — it may be selected, in which case it stays called out.
	 * Without the first of the two, every brick the cursor has ever crossed stays lit and the
	 * wall ends up entirely highlighted.
	 */
	const FPieceRef Previous = HoveredPiece;

	HoveredPiece = Ref;

	RefreshPieceHighlight(Previous);
	RefreshPieceHighlight(Ref);
}

void ADestructionGamePlayerController::ClearPieceSelection()
{
	/* Copied out first: Clear empties the very array these live in. */
	const TArray<FPieceRef> WasSelected(PieceSelection.Refs());

	/*
	 * And the readout's own bricks, not in that list: a brick that leaves the selection stops
	 * being singled out, so the whole neighbour set goes too, and those bricks were never picked
	 * so the loop below would not reach them. Asked before the clear, since afterwards there is
	 * no readout left to ask.
	 */
	const TArray<FPieceRef> WereNeighbours = NeighbourPieces();

	PieceSelection.Clear();

	for (const FPieceRef& Ref : WasSelected)
	{
		RefreshPieceHighlight(Ref);
	}

	RefreshNeighbourHighlights(WereNeighbours);
}

void ADestructionGamePlayerController::SetInspectedPiece(const FPieceRef& Ref)
{
	/*
	 * The brick being left is refreshed as well as the one being taken up, and refreshed rather
	 * than cleared — it is almost always still selected, so it goes back to Selected rather than
	 * None. Same bug class as SetHoveredPiece, sharper here: a brick left Inspected means two
	 * bricks claim the one breakout, and a brick dropped to None would silently empty the
	 * selection on screen while the commit still deletes every one of them. Nothing else moves:
	 * reading a brick is not choosing it, so the selection and presented rows come through untouched.
	 */
	const FPieceRef Previous = InspectedPiece;

	/*
	 * And the bricks the readout was pointing at are collected before the ref moves, since
	 * afterwards there is nothing left to ask — the neighbour set changes wholesale when the
	 * readout does, the same left-behind-state bug Previous exists to close, one field out.
	 */
	const TArray<FPieceRef> WereNeighbours = NeighbourPieces();

	InspectedPiece = Ref;

	RefreshPieceHighlight(Previous);
	RefreshPieceHighlight(Ref);

	RefreshNeighbourHighlights(WereNeighbours);

	/*
	 * And the readout follows the brick it describes. The panel breaks out one brick's joints,
	 * so which brick that is changing is the whole of what a player asked for by running the
	 * cursor down the list.
	 */
	RefreshPieceMenuInspectorWidget();
}

bool ADestructionGamePlayerController::ShowPieceMenu(TArrayView<const FPieceMenuRow> Rows)
{
	/*
	 * Showing is defined as dismissing and then building, which is the point rather than an
	 * implementation detail. There is exactly one route out of "a menu is up", so replacing a
	 * menu, showing an empty one and closing one outright all take it — which lets go of the
	 * inspected brick on every one of them without three copies of that. It also keeps the
	 * widget half honest: the build below sits here and the removal sits beside the Reset in
	 * DismissPieceMenu, so a second add with no matching remove — invisible to any headless
	 * assertion — cannot happen while the model-level version of the same bug (holding two
	 * menus' rows) stays asserted.
	 *
	 * Rows must not alias ShownPieceMenuRows: the dismiss below empties it. No caller does that
	 * today and nothing guards it; see CURRENT_STATE.md.
	 */
	DismissPieceMenu();

	if (Rows.Num() == 0)
	{
		return false;
	}

	ShownPieceMenuRows.Append(Rows.GetData(), Rows.Num());

	BuildPieceMenuWidget();

	/*
	 * And the controls are not touched: a menu used to raise the cursor and remove the free-look
	 * context, giving both back on the way out, but the cursor is the session's now and the
	 * camera is chorded to a held right mouse button, so there is nothing left to restore
	 * (SESSION_UI_DESIGN §d, S6).
	 */
	return true;
}

bool ADestructionGamePlayerController::DismissPieceMenu()
{
	if (!IsPieceMenuShown())
	{
		return false;
	}

	ShownPieceMenuRows.Reset();

	RemovePieceMenuWidget();

	/*
	 * And nothing is being read out any more, a clear rather than a disable. Slate delivers no
	 * OnMouseLeave to a widget that has left the tree, so a panel taken down under the cursor
	 * left InspectedPiece set; HighlightForPiece made that ref merely inert rather than cleared,
	 * so deselecting the brick and picking it again brought the readout straight back on it with
	 * the player's cursor nowhere near the menu.
	 *
	 * It goes through SetInspectedPiece rather than assigning the field, since the brick being
	 * let go of has to be told — it is almost always still selected, so it drops back to
	 * Selected rather than being left wearing the readout's colour. It sits after
	 * RemovePieceMenuWidget so the readout refresh it triggers finds no box to draw into.
	 */
	SetInspectedPiece(FPieceRef());

	return true;
}

bool ADestructionGamePlayerController::IsPieceMenuShown() const
{
	/*
	 * The rows are the record, and there is no second flag. An empty list dismisses, so
	 * "holding rows" and "a menu is up" are the same fact; a bool beside them would be a
	 * second copy of it, free to disagree.
	 */
	return ShownPieceMenuRows.Num() > 0;
}

TArrayView<const FPieceMenuRow> ADestructionGamePlayerController::GetShownPieceMenuRows() const
{
	return ShownPieceMenuRows;
}

bool ADestructionGamePlayerController::ChoosePieceMenuRow(int32 RowIndex)
{
	/*
	 * An index that names no row commits nothing, and is refused rather than clamped. A
	 * FMath::Clamp here would turn every out-of-range choice into a commit of row 0 — the
	 * first entry of a menu run against a brick nobody clicked — which is the obvious wrong
	 * fix and is exactly what the refusal rows of World.Choose count entries into Run to
	 * catch. IsValidIndex is also what makes "choose row 0 when no menu is up" the same
	 * refusal, since an empty array has no valid index at all.
	 */
	if (!ShownPieceMenuRows.IsValidIndex(RowIndex))
	{
		return false;
	}

	/*
	 * The chosen row is copied out before the dismiss, load-bearing rather than tidy:
	 * DismissPieceMenu Reset()s the very array the rows live in, so a reference into
	 * ShownPieceMenuRows would read destroyed elements by commit time. Both halves come from
	 * the row rather than anything this controller remembered separately — Core/PieceMenu.h
	 * says why, and a presenter that committed against a remembered selection would act on the
	 * wrong bricks with everything else looking perfect.
	 */
	const TArray<FPieceRef> Refs = ShownPieceMenuRows[RowIndex].Refs;
	const FPieceAction* const Action = ShownPieceMenuRows[RowIndex].Action;

	/*
	 * It comes down first, by the one route out of "a menu is up", so the brick it was reading
	 * out is let go of before the commit below runs with nothing on screen naming the bricks it
	 * is about to remove.
	 */
	DismissPieceMenu();

	/*
	 * And the pick goes with it, before the commit rather than after: these bricks have just
	 * been acted on, so leaving them selected would carry them into the next click's menu where
	 * they no longer resolve. Before, because the commit is what destroys them, and a brick has
	 * to still exist to be told it is no longer called out.
	 */
	ClearPieceSelection();

	UDestructionStructureSubsystem* const Subsystem = PieceMenuSubsystemOf(*this);

	if (Subsystem == nullptr)
	{
		return false;
	}

	/*
	 * One commit for the whole selection, which is what makes it one solve. Looping the
	 * single-piece commit here would reach the same wall at N times the price, pushing N
	 * times, each against an answer that had seen only part of the batch.
	 */
	const bool bCommitted = Subsystem->CommitPieceActionForAll(Refs, *Action) > 0;

	/*
	 * And the session is asked again whether there is anything left to command — the only door
	 * that changes the world without being a toolbar click. OnToolbarButton refreshes
	 * `bHasStructure` at its own door, but a delete arrives here instead, and without this the
	 * strip would go on drawing from the answer of the click that laid the brick, offering Run
	 * and Clear over an empty plot, with Run then solving an empty graph and reporting success.
	 * Only when something actually committed, since a refused action changed nothing.
	 */
	if (bCommitted)
	{
		RefreshSessionHasStructure();
		RefreshSessionToolbar();

		/*
		 * And the overlay is recomputed, the claim the whole feature is for: a refresh that ran
		 * only on the toggle would leave the wall coloured by the structure as it stood before
		 * the delete. It costs nothing while the overlay is off.
		 */
		RefreshLoadOverlay();
	}

	return bCommitted;
}

void ADestructionGamePlayerController::BuildPieceMenuWidget()
{
	UWorld* const World = GetWorld();

	UGameViewportClient* const Viewport = World != nullptr ? World->GetGameViewport() : nullptr;

	/*
	 * No viewport means no widget, the ordinary case in a test rather than an error: a world
	 * built in code has no UGameViewportClient at all, so the presented rows — the record, not
	 * this — stand alone and everything asserted about a menu still holds with nothing drawn.
	 */
	if (Viewport == nullptr)
	{
		return;
	}

	/*
	 * The panel takes its home the first time it is shown, and never again — the menu is rebuilt
	 * on every click, and PieceMenuPanelOffsetPx is controller state precisely so a corner the
	 * player chose survives that; a home taken on every build would overwrite it and snap the
	 * panel back across the screen the next time they picked a brick.
	 *
	 * Where it opens is Core's decision, not this function's: it is arithmetic on two sizes and
	 * a margin, and a widget cannot be asked whether it got it right. Presenter.PanelHomeOffset
	 * holds it and composes with the clamp rather than restating it, so nothing here has to
	 * clamp what it hands back.
	 */
	if (!bPieceMenuPanelHasOpened)
	{
		PieceMenuPanelOffsetPx = PieceMenuHomeOffset(
			PieceMenuPanelSizePx(PieceMenuPanelDetail),
			PieceMenuViewportSizeAtOpenPx(*Viewport),
			PieceMenuPanelHomeMarginPx);

		bPieceMenuPanelHasOpened = true;
	}

	PieceMenuWidget = BuildPieceMenuPanel();

	Viewport->AddViewportWidgetContent(PieceMenuWidget.ToSharedRef());
}

FVector2D ADestructionGamePlayerController::PieceMenuViewportSizeAtOpenPx(
	const UGameViewportClient& Viewport) const
{
	/*
	 * The screen, in the units the panel's offset is stated in, before there is a panel to ask.
	 * PieceMenuViewportSizePx reads the panel's own laid-out root and is the right answer, but a
	 * widget built this frame has no cached geometry yet, so a home worked out from it would be
	 * the origin for the whole of the first menu. The viewport client knows its size up front.
	 *
	 * In screen pixels, the wrong unit by exactly the DPI scale: the constraint canvas lays out
	 * under Slate's scaler, so a home measured in screen pixels and applied in scaled ones would
	 * open the panel past the right edge by that factor, invisibly, since the clamp would then
	 * quietly pull it back. Dividing by the scale is the whole conversion.
	 *
	 * A scale that is not a positive number fails to no screen at all; the guard is written
	 * `!(X > 0)` so a NaN lands inside it, putting the home at the origin — the corner that is on
	 * screen at every size and every scale.
	 */
	const double ScaleFactor = Viewport.GetDPIScale();

	if (!(ScaleFactor > 0.0))
	{
		return FVector2D::ZeroVector;
	}

	FVector2D ScreenSizePx = FVector2D::ZeroVector;
	Viewport.GetViewportSize(ScreenSizePx);

	return ScreenSizePx / ScaleFactor;
}

TSharedRef<SWidget> ADestructionGamePlayerController::BuildPieceMenuPanel()
{
	/*
	 * The readout is asked for once and every string in it is taken as given. Nothing below
	 * counts, formats, pluralises, filters or resolves anything — Core/PieceMenu.h says at
	 * length why each of those decisions is already made in the model, and the short version is
	 * that this function is the one place no test can reach.
	 *
	 * The detail is the panel's own rather than the accessor's default, the one argument on this
	 * line and the one place the compact request is allowed to reach. The joint table is also the
	 * index NeighbourHighlightForPiece colours the wall from, so a compact answer handed to the
	 * shared accessor would darken every neighbour highlight along with the table — see
	 * PieceMenuInspectorForSelection.
	 */
	const FPieceMenuInspector Inspector = PieceMenuInspectorForSelection(PieceMenuPanelDetail);

	TSharedRef<SVerticalBox> Panel = SNew(SVerticalBox);

	/*
	 * The heading and the count share a row — one sentence about one thing, and a count wrapped
	 * onto its own line would spend a row of a fixed panel on nothing. Both strings are the
	 * model's; which is bold is the only thing decided here.
	 *
	 * That row is also the handle the panel is moved by: the widest thing on the panel that
	 * holds nothing clickable, and the one strip there in every state — a grab affordance that
	 * vanished when nothing was selected would strand a panel with nothing in it.
	 *
	 * The border takes no padding of its own, load-bearing rather than tidy: six layout tests
	 * measure where the rows below this one land, so the strip is a background and four event
	 * bindings that change no geometry. The grab cursor is what says it can be dragged, since a
	 * word saying so would be chosen in the one place no test can read it.
	 */
	Panel->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 6.0f)
		[
			SAssignNew(PieceMenuGrabStrip, SBorder)
			.BorderImage(PieceMenuFillBrush())
			.BorderBackgroundColor(PieceMenuGrabStripColour)
			.Padding(FMargin(0.0f))
			.Cursor(EMouseCursor::GrabHand)
			.OnMouseButtonDown(FPointerEventHandler::CreateUObject(
				this, &ADestructionGamePlayerController::OnPieceMenuPanelGrabbed))
			.OnMouseMove(FPointerEventHandler::CreateUObject(
				this, &ADestructionGamePlayerController::OnPieceMenuPanelDragged))
			.OnMouseButtonUp(FPointerEventHandler::CreateUObject(
				this, &ADestructionGamePlayerController::OnPieceMenuPanelReleased))
			.OnMouseDoubleClick(FPointerEventHandler::CreateUObject(
				this, &ADestructionGamePlayerController::OnPieceMenuPanelDetailToggled))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 10.0f, 0.0f)
				[
					SNew(STextBlock)
					.Font(PieceMenuHeaderFont())
					.ColorAndOpacity(PieceMenuHeaderColour)
					.Text(FText::FromString(Inspector.HeaderText))
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(PieceMenuBodyFont())
					.ColorAndOpacity(PieceMenuCountColour)
					.Text(FText::FromString(Inspector.CountText))
				]
			]
		];

	/*
	 * One row per selected brick, and hovering one is what singles it out. A button is used for
	 * the hover events rather than a click: naming a brick is not choosing to do anything to it,
	 * so it carries no OnClicked at all. Every row says why its brick is standing up, in the
	 * model's own word — FInspectorPieceEntry::SupportText — since eleven picked bricks used to
	 * be eleven identical strings, forcing a hover of each to find the falling one.
	 *
	 * The word sits beside the button rather than inside it, which is not cosmetic: four layout
	 * tests find an entry row by the text under its button and match it against the model's
	 * Label, so a second text block in there would rename every row to "course 2 · #1supported"
	 * and take those assertions with it. The button still fills the row, so the whole width of
	 * it is hover target.
	 */
	TSharedRef<SScrollBox> BrickList = SNew(SScrollBox);

	for (const FInspectorPieceEntry& Entry : Inspector.Pieces)
	{
		BrickList->AddSlot()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				[
					/*
					 * Not focusable, for the same reason the toolbar's chips are not. Slate gives
					 * user focus to a focusable widget on click and SButton::OnKeyDown then handles
					 * Enter and Space itself, so clicking a brick row would cost the player Run
					 * (Enter) and the pawn's jump (Space) until they click the viewport again.
					 */
					SNew(SButton)
					.IsFocusable(false)
					.OnHovered(FSimpleDelegate::CreateUObject(
						this, &ADestructionGamePlayerController::OnPieceMenuEntryHovered, Entry.Ref))
					.OnUnhovered(FSimpleDelegate::CreateUObject(
						this, &ADestructionGamePlayerController::OnPieceMenuEntryUnhovered))
					[
						SNew(STextBlock)
						.Font(PieceMenuBodyFont())
						.Text(FText::FromString(Entry.Label))
						.ColorAndOpacity(Entry.bIsLivePiece
							? PieceMenuLivePieceColour : PieceMenuDeadPieceColour)
					]
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(SBox)
					.WidthOverride(PieceMenuEntrySupportWidthPx)
					.HAlign(HAlign_Left)
					.Padding(FMargin(10.0f, 0.0f, 0.0f, 0.0f))
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						.Padding(0.0f, 0.0f, PieceMenuSupportDotGapPx, 0.0f)
						[
							PieceMenuSupportDot(Entry.SupportBand)
						]
						+ SHorizontalBox::Slot()
						.AutoWidth()
						.VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Font(PieceMenuBodyFont())
							.ColorAndOpacity(PieceMenuReadoutColour)
							.Text(FText::FromString(Entry.SupportText))
						]
					]
				]
			];
	}

	/*
	 * And the list is capped in height with the scrolling inside it. Forty picked bricks is an
	 * ordinary selection, and a list that grew with it would run the action rows off the bottom
	 * of the screen — a scroll box in a box that will not exceed a stated height cannot do that.
	 *
	 * The cap does not give up the panel's fixed size, the property every stillness claim rests
	 * on: the readout below is a fill slot absorbing whatever a short list leaves, and three
	 * bricks or forty-five measure the same 560 px and lay the action rows from the same bottom
	 * edge.
	 */
	Panel->AddSlot()
		.AutoHeight()
		[
			SNew(SBox)
			.MaxDesiredHeight(PieceMenuBrickListMaxHeightPx)
			[
				BrickList
			]
		];

	/*
	 * The joint breakout gets the space that is left, and the same space whatever is in it: a
	 * fill slot's height comes from the panel's size minus the auto-height rows above and below
	 * it, so the readout's content cannot move the entry rows, the action rows, or the panel —
	 * retiring the old geometry workaround where the readout had to be the last slot.
	 *
	 * It is still a box whose content is swapped rather than a panel rebuilt: rebuilding on
	 * hover would destroy the very button the cursor is on, and Slate would fire OnHovered on
	 * its replacement next frame and again on the one after that.
	 */
	Panel->AddSlot()
		.FillHeight(1.0f)
		.Padding(0.0f, 8.0f, 0.0f, 8.0f)
		[
			SAssignNew(PieceMenuInspectorBox, SBox)
		];

	/*
	 * And the destructive row is last, behind a rule. Releasing a brick is irreversible here, and
	 * the standing rule is that the commit door is never wider than the menu door; a button
	 * reached by reading past everything that describes what it will destroy is that rule stated
	 * as geometry, and nothing a player might click on the way to reading the panel is below it.
	 */
	Panel->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 8.0f)
		[
			SNew(SBox)
			.HeightOverride(PieceMenuRuleHeightPx)
			[
				SNew(SImage)
				.Image(PieceMenuFillBrush())
				.ColorAndOpacity(PieceMenuRuleColour)
			]
		];

	/*
	 * And a row that destroys something looks like one, and says how much of it — both facts the
	 * model's. bIsDestructive is the action's own flag, so the colour is keyed on data rather
	 * than a caption compared against the word "Delete"; TargetText comes from the very refs the
	 * row commits against, so a button cannot promise to act on a different count than it will.
	 *
	 * The count is overlaid on the button rather than set as part of it: four layout tests find
	 * an action row by the text under its button and match it against the model's Label, so a
	 * text block inside would rename the row to "Delete3 bricks", and a slot beside it would
	 * narrow the span World.Menu.TheReadoutFitsInsideThePanel measures. The count is
	 * HitTestInvisible so a click on it still lands on the button underneath.
	 */
	for (int32 RowIndex = 0; RowIndex < ShownPieceMenuRows.Num(); ++RowIndex)
	{
		const FPieceMenuRow& Row = ShownPieceMenuRows[RowIndex];

		Panel->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 2.0f, 0.0f, 0.0f)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()
				[
					/*
					 * Not focusable, and this site needs its own say because it is a second one: an
					 * action row is built here and an entry row is built above, so the fix applied to
					 * one leaves the other taking the keyboard — a focused SButton's OnKeyDown handles
					 * Enter and Space, which in this session are Run and the pawn's jump.
					 */
					SNew(SButton)
					.IsFocusable(false)
					.ButtonColorAndOpacity(Row.bIsDestructive
						? PieceMenuDestructiveRowColour : PieceMenuOrdinaryRowColour)
					.Text(FText::FromString(Row.Label))
					.OnClicked(FOnClicked::CreateUObject(
						this, &ADestructionGamePlayerController::OnPieceMenuRowClicked, RowIndex))
				]
				+ SOverlay::Slot()
				.HAlign(HAlign_Right)
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 12.0f, 0.0f)
				[
					SNew(STextBlock)
					.Visibility(EVisibility::HitTestInvisible)
					.Font(PieceMenuBodyFont())
					.ColorAndOpacity(PieceMenuCountColour)
					.Text(FText::FromString(Row.TargetText))
				]
			];
	}

	/*
	 * A fixed size on a real background, placed wherever the player last put it.
	 *
	 * A panel that cannot change size cannot move a row out from under a cursor, strictly
	 * stronger than the top anchor and last-slot readout it replaces. Every line under the brick
	 * rows used to be a bare text block over the sky, invisible against anything bright and
	 * unreachable by a headless suite that paints no pixels; a near-opaque fill is what makes the
	 * readout readable.
	 *
	 * A position rather than an alignment, what the player asked for: this used to be an SBox
	 * pinned to the right edge and centred down it, a placement nobody could argue with — being
	 * unable to argue with it was the complaint. A constraint canvas takes the corner as a value,
	 * so the same tree draws wherever PieceMenuPanelOffsetPx says, and a drag is then a new value
	 * rather than a new layout.
	 *
	 * Anchored and aligned to the top-left so the offset means what ClampPanelOffset says: the
	 * clamp reasons about the panel's top-left corner in viewport pixels, so an anchor anywhere
	 * else would make the stored number a distance from somewhere the clamp never heard of.
	 * AutoSize takes the size from the child, stated once rather than again in the slot's margin,
	 * and that size is the presenter's answer for this mode, measured by
	 * World.Menu.TheReadoutFitsInsideThePanel rather than argued.
	 */
	const FVector2D PanelSizePx = PieceMenuPanelSizePx(PieceMenuPanelDetail);

	TSharedRef<SWidget> Framed =
		SNew(SConstraintCanvas)
		+ SConstraintCanvas::Slot()
		.Anchors(FAnchors(0.0f, 0.0f))
		.Alignment(FVector2D(0.0f, 0.0f))
		.AutoSize(true)
		.Offset(TAttribute<FMargin>::Create(TAttribute<FMargin>::FGetter::CreateUObject(
			this, &ADestructionGamePlayerController::PieceMenuPanelSlotOffset)))
		[
			SNew(SBox)
			.WidthOverride(static_cast<float>(PanelSizePx.X))
			.HeightOverride(static_cast<float>(PanelSizePx.Y))
			[
				SNew(SBorder)
				.BorderImage(PieceMenuFillBrush())
				.BorderBackgroundColor(PieceMenuPanelBackgroundColour)
				.Padding(PieceMenuPanelPaddingPx)
				[
					Panel
				]
			]
		];

	RefreshPieceMenuInspectorWidget();

	return Framed;
}

FMargin ADestructionGamePlayerController::PieceMenuPanelSlotOffset() const
{
	/*
	 * The corner, and nothing else. The slot is AutoSize, so the last two components of the
	 * margin are the size the canvas ignores in favour of the child's own — stating the panel's
	 * dimensions here as well would be the second copy that eventually disagrees with the first.
	 */
	return FMargin(PieceMenuPanelOffsetPx.X, PieceMenuPanelOffsetPx.Y, 0.0f, 0.0f);
}

FVector2D ADestructionGamePlayerController::PieceMenuViewportSizePx() const
{
	/*
	 * The root of the panel is the viewport, so its own local size is the screen in the units the
	 * offset above is stated in — see the header for why UGameViewportClient::GetViewportSize is
	 * the wrong answer by exactly the DPI scale. Zero with no panel up clamps every offset to the
	 * origin, the fail-closed corner rather than a case needing its own handling.
	 */
	return PieceMenuWidget.IsValid()
		? FVector2D(PieceMenuWidget->GetTickSpaceGeometry().GetLocalSize())
		: FVector2D::ZeroVector;
}

FReply ADestructionGamePlayerController::OnPieceMenuPanelGrabbed(
	const FGeometry& Geometry,
	const FPointerEvent& Event)
{
	/*
	 * Where both things were when the press landed, and the capture that keeps them coming: Slate
	 * stops sending moves once the pointer leaves a widget, so without the capture a fast drag
	 * drops the panel wherever the cursor crossed the strip's edge.
	 *
	 * The corner is re-clamped as it is picked up, which is what survives a viewport resize: a
	 * corner inside a 1920 px screen is outside a 1280 px one, and nothing tells this class the
	 * window changed, so the stored value is held against the screen as it is now before a drag
	 * is measured from it. Clamping is idempotent, so doing this on every press is free.
	 */
	PieceMenuPanelOffsetPx = ClampPanelOffset(
		PieceMenuPanelOffsetPx,
		PieceMenuPanelSizePx(PieceMenuPanelDetail),
		PieceMenuViewportSizePx());

	PieceMenuPanelGrabbedFromPx = PieceMenuPanelOffsetPx;
	PieceMenuCursorGrabbedAtPx = FVector2D(Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition()));
	bPieceMenuPanelIsHeld = true;

	return PieceMenuGrabStrip.IsValid()
		? FReply::Handled().CaptureMouse(PieceMenuGrabStrip.ToSharedRef())
		: FReply::Handled();
}

FReply ADestructionGamePlayerController::OnPieceMenuPanelDragged(
	const FGeometry& Geometry,
	const FPointerEvent& Event)
{
	/*
	 * A move that is not a drag is somebody's cursor crossing the strip. Slate sends moves
	 * whether or not a button is down, and a panel that followed the pointer without being picked
	 * up would be unusable rather than draggable.
	 */
	if (!bPieceMenuPanelIsHeld)
	{
		return FReply::Unhandled();
	}

	/*
	 * The drag is measured from the press rather than the last frame, so the corner tracks the
	 * cursor exactly instead of accumulating a rounding per move, and dragging into a corner and
	 * back out returns to where it started rather than wherever the clamp pinned it on the way
	 * through. AbsoluteToLocal on both ends puts the delta in the canvas's own units whatever the
	 * DPI scale; the translation cancels in the subtraction.
	 */
	const FVector2D DraggedToPx = PieceMenuPanelGrabbedFromPx
		+ FVector2D(Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition()))
		- PieceMenuCursorGrabbedAtPx;

	/*
	 * And where that is allowed to leave the panel is Core's decision, not this function's. It is
	 * the half that can strand the panel — a corner off the top of the screen leaves nothing to
	 * grab — and it is arithmetic on six doubles, so Presenter.PanelOffsetClamp holds it.
	 */
	PieceMenuPanelOffsetPx = ClampPanelOffset(
		DraggedToPx,
		PieceMenuPanelSizePx(PieceMenuPanelDetail),
		PieceMenuViewportSizePx());

	return FReply::Handled();
}

FReply ADestructionGamePlayerController::OnPieceMenuPanelReleased(
	const FGeometry& Geometry,
	const FPointerEvent& Event)
{
	bPieceMenuPanelIsHeld = false;

	return FReply::Handled().ReleaseMouseCapture();
}

FReply ADestructionGamePlayerController::OnPieceMenuPanelDetailToggled(
	const FGeometry& Geometry,
	const FPointerEvent& Event)
{
	/*
	 * The panel's mode only, which is the whole care here. NeighbourHighlightForPiece and
	 * NeighbourPieces read the joint table to colour bricks in the wall, and they ask
	 * PieceMenuInspectorForSelection for themselves with no argument, so rolling the readout up
	 * cannot take the neighbour colours with it. See the header.
	 *
	 * The whole panel is rebuilt rather than the readout swapped, because a mode change moves the
	 * action rows: the readout is a fill slot, so what is in it decides nothing, but the mode is
	 * read while the heading, the brick list and every row is composed. It is safe to rebuild
	 * here for the reason RefreshPieceMenuInspectorWidget is not — the cursor is on the title
	 * strip, not on an entry button, so nothing being destroyed can fire a hover at its own
	 * replacement.
	 */
	PieceMenuPanelDetail = PieceMenuPanelDetail == EPieceMenuDetail::Compact
		? EPieceMenuDetail::Full
		: EPieceMenuDetail::Compact;

	/*
	 * And the drag is over, which the double-click's own press turned on. Slate sends a press
	 * before a double-click, so the strip would otherwise hold a grab that never sees a release
	 * once the widget under the cursor is torn down and replaced.
	 */
	bPieceMenuPanelIsHeld = false;

	RemovePieceMenuWidget();
	BuildPieceMenuWidget();

	return FReply::Handled().ReleaseMouseCapture();
}

void ADestructionGamePlayerController::RemovePieceMenuWidget()
{
	if (!PieceMenuWidget.IsValid())
	{
		return;
	}

	UWorld* const World = GetWorld();

	if (UGameViewportClient* const Viewport = World != nullptr ? World->GetGameViewport() : nullptr)
	{
		Viewport->RemoveViewportWidgetContent(PieceMenuWidget.ToSharedRef());
	}

	/*
	 * Both handles live inside the panel, so they go with it and never outlive it. The grab strip
	 * is released beside the readout box for the reason it was taken beside it: it is a pointer
	 * into the tree rather than a second viewport widget, and a stale one would keep a torn down
	 * panel alive to be captured to.
	 */
	PieceMenuInspectorBox.Reset();
	PieceMenuGrabStrip.Reset();

	PieceMenuWidget.Reset();
}

void ADestructionGamePlayerController::RefreshPieceMenuInspectorWidget()
{
	if (!PieceMenuInspectorBox.IsValid())
	{
		return;
	}

	/* The panel's own detail, for the reason BuildPieceMenuPanel states beside the same argument. */
	const FPieceMenuInspector Inspector = PieceMenuInspectorForSelection(PieceMenuPanelDetail);

	TSharedRef<SVerticalBox> Readout = SNew(SVerticalBox);

	/*
	 * The readout names the brick it is about, or says why it has nothing to say — the two
	 * share a row because the model guarantees at most one of them is there.
	 *
	 * InspectedLabel is empty exactly when no brick is singled out and InspectedHintText is
	 * empty exactly when one is, so laying them side by side draws whichever exists with no
	 * branch anywhere near Slate. That is the point rather than a trick: a fixed panel reserves
	 * this space in every state, and an empty reserved region reads as a readout that failed.
	 */
	Readout->AddSlot()
		.AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Font(PieceMenuHeaderFont())
				.ColorAndOpacity(PieceMenuHeaderColour)
				.Text(FText::FromString(Inspector.InspectedLabel))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Font(PieceMenuBodyFont())
				.ColorAndOpacity(PieceMenuHintColour)
				.Text(FText::FromString(Inspector.InspectedHintText))
			]
		];

	/*
	 * And what that brick is, directly under its name, because the two are one thought — which
	 * brick, and what is it. The identity line below the joint table would be the brick's weight
	 * printed where a player who has read the heading has already stopped looking.
	 *
	 * No emptiness check, exactly as the row above has none. The model leaves IdentityText empty
	 * in the same state it leaves InspectedLabel empty, so an unsingled-out panel draws an empty
	 * text block here and the branch stays where a test can read it.
	 */
	Readout->AddSlot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Font(PieceMenuBodyFont())
			.ColorAndOpacity(PieceMenuReadoutColour)
			.Text(FText::FromString(Inspector.IdentityText))
		];

	/*
	 * The support word and the joint list's sentence, which is there whether or not there are
	 * any joints. That is why there is no emptiness check here: an isolated grounded pad reads
	 * "No joints", and the model is what says so. A widget noticing Joints.Num() == 0 for itself
	 * would be the branch this whole arrangement exists to keep out.
	 */
	Readout->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 2.0f, 0.0f, 6.0f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 12.0f, 0.0f)
			[
				SNew(STextBlock)
				.Font(PieceMenuBodyFont())
				.ColorAndOpacity(PieceMenuReadoutColour)
				.Text(FText::FromString(Inspector.SupportText))
			]
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Font(PieceMenuBodyFont())
				.ColorAndOpacity(PieceMenuCountColour)
				.Text(FText::FromString(Inspector.JointsText))
			]
		];

	/*
	 * One row per joint: its swatch, its bar, then its line. The bar goes before the words so
	 * the bars form a column the decade scale below can be read against — a log axis with the
	 * ticks nowhere near the fills is the same as no ticks at all — and the swatch goes before
	 * the bar so the colours form a column of their own down the left edge of the readout, where
	 * an eye scanning for one neighbour will look.
	 */
	for (const FInspectorJointRow& Joint : Inspector.Joints)
	{
		Readout->AddSlot()
			.AutoHeight()
			.Padding(0.0f, 1.0f, 0.0f, 1.0f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, PieceMenuJointSwatchGapPx, 0.0f)
				[
					PieceMenuJointSwatch(Joint.ColourSlot)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, 10.0f, 0.0f)
				[
					PieceMenuHeadroomBar(Joint.HeadroomFraction, Joint.MarginBand)
				]
				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Font(PieceMenuBodyFont())
					.ColorAndOpacity(PieceMenuReadoutColour)
					.Text(FText::FromString(Joint.Text))
				]
			];
	}

	/*
	 * The scale the bars above are read against, and the left alignment is load-bearing.
	 *
	 * WidthOverride states a desired width, not an arranged one, and a vertical box's slot fills
	 * by default — so the scale's box was being stretched to the readout's full width while the
	 * bars kept their auto-width 96 px, and a canvas five times as wide annotates nothing: the
	 * 10x tick stood where no fill could ever reach. Aligning the slot left hands the box the
	 * bar's own width, so the tick strip and the column of bars are one span.
	 *
	 * The left padding is the swatch column, for the same reason: the bars start one swatch and
	 * one gap in from the readout's edge, so a scale flush with that edge would stand one swatch
	 * to the left of everything it labels.
	 */
	Readout->AddSlot()
		.AutoHeight()
		.HAlign(HAlign_Left)
		.Padding(PieceMenuJointSwatchWidthPx + PieceMenuJointSwatchGapPx, 6.0f, 0.0f, 0.0f)
		[
			PieceMenuHeadroomScale(Inspector.HeadroomScale)
		];

	Readout->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 2.0f, 0.0f, 0.0f)
		[
			SNew(STextBlock)
			.Font(PieceMenuSmallFont())
			.ColorAndOpacity(PieceMenuHintColour)
			.Text(FText::FromString(Inspector.HeadroomCaption))
		];

	/*
	 * And the whole readout scrolls inside the space it was given. A brick with more joints than
	 * fit would otherwise run its last lines out past the rule and under the row that deletes it
	 * — the same hazard the brick list's cap closes, one region down.
	 */
	PieceMenuInspectorBox->SetContent(
		SNew(SScrollBox)
		+ SScrollBox::Slot()
		[
			Readout
		]);
}

FPieceMenuInspector ADestructionGamePlayerController::PieceMenuInspectorForSelection(
	EPieceMenuDetail Detail) const
{
	/*
	 * One readout for the whole selection, against the structure its refs name — the same
	 * plumbing InspectAlongRay does for the rows, and for the same reason: a selection is built
	 * by clicking one wall, so the first ref names it, and BuildPieceMenuInspector answers for
	 * every ref that turns out not to belong to it.
	 */
	const TArrayView<const FPieceRef> Selected = PieceSelection.Refs();

	UDestructionStructureSubsystem* const Subsystem = PieceMenuSubsystemOf(*this);

	const FStructureBinding* const Binding = (Subsystem != nullptr && Selected.Num() > 0)
		? Subsystem->Find(Selected[0].StructureId)
		: nullptr;

	/*
	 * No binding is an empty binding rather than an early return, which keeps the model the only
	 * author of the readout. A default-constructed FPieceMenuInspector is not the same object
	 * BuildPieceMenuInspector answers for the same inputs: nothing picked has its own sentence,
	 * "No bricks selected", decided in the model so no widget has to choose one, and a returned
	 * default would carry an empty CountText instead, drawing as a blank line where a sentence
	 * belongs. Handing over an empty structure says the same thing by the one route that words
	 * it, and makes SelectedCount answer the selection's own size on the fail-closed paths too,
	 * the promise Core/PieceMenu.h makes for it.
	 */
	const FStructureBinding NoStructure;

	return BuildPieceMenuInspector(
		Binding != nullptr ? *Binding : NoStructure, Selected, InspectedPiece, Detail);
}

void ADestructionGamePlayerController::SetPieceMenuDetail(EPieceMenuDetail Detail)
{
	/* A field, and nothing else. See the header: this is a seam, not a behaviour. */
	PieceMenuPanelDetail = Detail;
}

FReply ADestructionGamePlayerController::OnPieceMenuRowClicked(int32 RowIndex)
{
	ChoosePieceMenuRow(RowIndex);

	return FReply::Handled();
}

void ADestructionGamePlayerController::OnPieceMenuEntryHovered(FPieceRef Ref)
{
	SetInspectedPiece(Ref);
}

void ADestructionGamePlayerController::OnPieceMenuEntryUnhovered()
{
	/* A default ref singles out nothing, how the cursor leaving the list is said. */
	SetInspectedPiece(FPieceRef());
}

TSharedRef<SWidget> ADestructionGamePlayerController::BuildSessionToolbarPanel()
{
	using namespace DestructionSession;

	/*
	 * The strip is the model's list, drawn in its own order — which buttons exist, what each
	 * reads, which is lit or greyed are all SessionToolbarButtons' answers (Core/SessionToolbar.h
	 * says why). What is left here is the chip: a size, two fonts and two fills.
	 */
	const TArray<FToolbarButton> Buttons = SessionToolbarButtons(SessionToolbarState);

	/*
	 * The styles are rebuilt here, before a single chip is made, since the look follows the
	 * state: every chip's fill, edge and caption weight is ChipLookFor's answer, and this
	 * function is called afresh on every click that changes it.
	 */
	RebuildSessionChipStyles(Buttons);

	TSharedRef<SHorizontalBox> Strip = SNew(SHorizontalBox);

	for (int32 Index = 0; Index < Buttons.Num(); ++Index)
	{
		const FToolbarButton& Button = Buttons[Index];

		/*
		 * A hairline where the region changes, and nowhere else.
		 *
		 * Compared against the neighbour rather than counted out in slots, the whole reason
		 * EToolbarGroup is on the row: the model says the three regions are contiguous and in
		 * order, so "the group changed" is all this needs to know, and a strip whose buttons are
		 * retuned keeps its rules without anything here being touched.
		 */
		if (Index > 0 && Buttons[Index - 1].Group != Button.Group)
		{
			Strip->AddSlot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, SessionToolbarRuleGapPx, 0.0f)
				[
					SessionToolbarGroupRule()
				];
		}

		const FChipLook Look = ChipLookFor(Button, SessionToolbarState.Mode);

		/*
		 * A chip's content is its swatch and its caption, and the swatch comes first. §e puts it
		 * "in place of a size caption" on the piece chips, and a block of brick red drawn after
		 * the word would read as a status light rather than the thing about to be laid.
		 */
		TSharedRef<SHorizontalBox> Content = SNew(SHorizontalBox);

		if (Button.Swatch != EToolbarSwatch::None)
		{
			Content->AddSlot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(0.0f, 0.0f, SessionToolbarSwatchGapPx, 0.0f)
				[
					SessionToolbarSwatchBlock(Button.Swatch)
				];
		}

		Content->AddSlot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Font(Look.bBoldCaption ? SessionToolbarBoldFont() : SessionToolbarRegularFont())
				.ColorAndOpacity(Look.Caption)
				.Text(FText::FromString(Button.Label))
			];

		Strip->AddSlot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, SessionToolbarChipGapPx, 0.0f)
			[
				SNew(SBox)
				.HeightOverride(SessionToolbarChipHeightPx)
				[
					/*
					 * Not focusable, and it is the one line on this chip that is not cosmetic: a
					 * focusable SButton takes user focus when it is clicked, and the flying pawn
					 * then stops answering W — a player reports that as the game freezing, and
					 * nothing but a headless arrange of this tree can see it.
					 *
					 * The style is a pointer into the controller's own storage, and has to be:
					 * SButton keeps what it is given and never copies it. There is no tint on the
					 * button any more — the fill is the style's brush, since a colour multiplied
					 * through FCoreStyle's grey brush could never be the design's amber.
					 */
					SNew(SButton)
					.IsFocusable(false)
					.IsEnabled(Button.bEnabled)
					.ButtonStyle(&SessionChipStyleFor(Button))
					.ContentPadding(FMargin(SessionToolbarChipPaddingPx, 0.0f))
					.HAlign(HAlign_Center)
					.VAlign(VAlign_Center)
					.OnClicked(FOnClicked::CreateUObject(
						this,
						&ADestructionGamePlayerController::OnSessionToolbarButtonClicked,
						Button.Id))
					[
						Content
					]
				]
			];

		if (Button.Id != EToolbarButtonId::CourseDown)
		{
			continue;
		}

		/*
		 * The course reads out between its own two arrows, and it is not a chip: arrow, value,
		 * arrow is what a stepper is, so it is hung off the down arrow's own slot rather than
		 * the mode — which also makes it Build-only for free, since SessionToolbarButtons draws
		 * the course pair only in Build mode. A text slot rather than an eleventh button, because
		 * the model has no row for it: a chip here would be a lit control that does nothing when
		 * pressed. The wording is CourseLabel's, already owned by Core.SessionToolbar.*.
		 */
		Strip->AddSlot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(SessionToolbarReadoutPaddingPx, 0.0f, SessionToolbarReadoutPaddingPx, 0.0f)
			[
				SNew(STextBlock)
				.Font(SessionToolbarRegularFont())
				.ColorAndOpacity(PieceMenuReadoutColour)
				.Text(FText::FromString(CourseLabel(SessionToolbarState.Course)))
			];
	}

	/*
	 * A bar across the bottom, and the rest of the screen is not the toolbar's.
	 *
	 * The widget the viewport is handed fills the viewport, so everything above the bar is a
	 * fill slot holding nothing, and the root is SelfHitTestInvisible so the empty part does not
	 * swallow the click the player is aiming at a brick.
	 *
	 * Hit-testable is not enough: Slate routes a press to a hit-testable widget and then, finding
	 * nothing bound, bubbles it on into the input stack and, in Build mode, into
	 * PrimaryAlongRay — so missing a chip by three pixels lays a brick. The two handlers below
	 * stop that fall-through, on the release too (a swallowed press with a leaked release is
	 * half a click), and on the left button only: the right button is the look chord
	 * (SESSION_UI_DESIGN §d, S6), and swallowing it would make the strip a dead patch a player
	 * cannot drag their view across.
	 *
	 * The middle third of the screen is where the wall is (SESSION_UI_DESIGN §a): the strip is
	 * at the bottom edge and the details window homes against the right, so neither is ever over
	 * the thing the player is pointing at.
	 */
	const auto SwallowLeftButton =
		[](const FGeometry& /*Geometry*/, const FPointerEvent& Event)
		{
			return Event.GetEffectingButton() == EKeys::LeftMouseButton
				? FReply::Handled()
				: FReply::Unhandled();
		};

	return SNew(SVerticalBox)
		.Visibility(EVisibility::SelfHitTestInvisible)
		+ SVerticalBox::Slot()
		.FillHeight(1.0f)
		[
			SNullWidget::NullWidget
		]
		+ SVerticalBox::Slot()
		.AutoHeight()
		[
			SNew(SBox)
			.HeightOverride(SessionToolbarHeightPx)
			[
				SNew(SBorder)
				.BorderImage(PieceMenuFillBrush())
				.BorderBackgroundColor(SessionToolbarFillColour)
				.Padding(FMargin(SessionToolbarEdgePaddingPx, 0.0f))
				.VAlign(VAlign_Center)
				.OnMouseButtonDown_Lambda(SwallowLeftButton)
				.OnMouseButtonUp_Lambda(SwallowLeftButton)
				[
					Strip
				]
			]
		];
}

const FButtonStyle& ADestructionGamePlayerController::SessionChipStyleFor(
	const DestructionSession::FToolbarButton& Button) const
{
	const int32 Index = static_cast<int32>(Button.Id);

	/*
	 * The last slot is the one nobody's button owns, and an id outside the enumeration lands
	 * there rather than on somebody else's chip. EToolbarButtonId is a uint8 and a cast is all it
	 * takes to make one; RebuildSessionChipStyles fills that slot with the greyed look, so an
	 * undeclared button reads as one that cannot be clicked instead of borrowing a live chip's amber.
	 */
	const bool bKnown = Index >= 0 && Index < SessionChipStyleCount - 1;

	return SessionChipStyles[bKnown ? Index : SessionChipStyleCount - 1];
}

void ADestructionGamePlayerController::RebuildSessionChipStyles(
	const TArray<DestructionSession::FToolbarButton>& Buttons)
{
	using namespace DestructionSession;

	/*
	 * Every slot is written, starting from the greyed look.
	 *
	 * A default FToolbarButton has bEnabled == false, so this is the greyed answer rather than a
	 * second spelling of it — what the unknown slot needs, and what a button not on this mode's
	 * strip should keep so a stale amber cannot survive a mode switch.
	 */
	const FButtonStyle GreyedStyle =
		SessionToolbarChipStyle(ChipLookFor(FToolbarButton(), SessionToolbarState.Mode));

	for (FButtonStyle& Style : SessionChipStyles)
	{
		Style = GreyedStyle;
	}

	/*
	 * Then the strip's own, in place. The array's slots do not move, so a chip already on screen
	 * holding a pointer into one of them goes on reading a valid style — it simply starts reading
	 * the new look, which is what a rebuilt strip wants.
	 */
	for (const FToolbarButton& Button : Buttons)
	{
		const int32 Index = static_cast<int32>(Button.Id);

		if (Index < 0 || Index >= SessionChipStyleCount - 1)
		{
			continue;
		}

		SessionChipStyles[Index] = SessionToolbarChipStyle(ChipLookFor(Button, SessionToolbarState.Mode));
	}
}

void ADestructionGamePlayerController::ShowSessionToolbar()
{
	UWorld* const World = GetWorld();

	UGameViewportClient* const Viewport = World != nullptr ? World->GetGameViewport() : nullptr;

	/*
	 * No viewport means no strip, the ordinary case in a test rather than an error — a world
	 * built in code has no UGameViewportClient at all. The session state is the record and
	 * stands alone, exactly as the presented rows do for the piece menu.
	 */
	if (Viewport == nullptr)
	{
		return;
	}

	/*
	 * Remove then add, which keeps the adds and the removes paired. This is also the redraw
	 * path, so a second add with no matching remove — the one leak that would go unseen — is on
	 * the same code path as the first show rather than a branch of its own.
	 */
	RemoveSessionToolbarWidget();

	SessionToolbarWidget = BuildSessionToolbarPanel();

	Viewport->AddViewportWidgetContent(SessionToolbarWidget.ToSharedRef());
}

void ADestructionGamePlayerController::RefreshSessionToolbar()
{
	/*
	 * Nothing on screen is nothing to redraw. Every accepted click calls this, including the ones
	 * a headless test makes before any strip has been shown, and a refresh that put one up would
	 * make a redraw into a show, a different thing that belongs to the game mode.
	 */
	if (!SessionToolbarWidget.IsValid())
	{
		return;
	}

	ShowSessionToolbar();
}

void ADestructionGamePlayerController::RemoveSessionToolbarWidget()
{
	if (!SessionToolbarWidget.IsValid())
	{
		return;
	}

	UWorld* const World = GetWorld();

	if (UGameViewportClient* const Viewport = World != nullptr ? World->GetGameViewport() : nullptr)
	{
		Viewport->RemoveViewportWidgetContent(SessionToolbarWidget.ToSharedRef());
	}

	SessionToolbarWidget.Reset();
}

FReply ADestructionGamePlayerController::OnSessionToolbarButtonClicked(
	DestructionSession::EToolbarButtonId Id)
{
	OnToolbarButton(Id);

	return FReply::Handled();
}

void ADestructionGamePlayerController::SetSessionControls()
{
	bShowMouseCursor = true;

	/*
	 * A controller with no local player has no viewport to set an input mode against, and a
	 * session must still work rather than merely not crash — so this fails closed after the
	 * cursor flag, which needs nothing and is what every headless assertion reads.
	 */
	if (GetLocalPlayer() == nullptr)
	{
		return;
	}

	/*
	 * GameAndUI, because both halves are live at once and neither may win outright. The strip and
	 * the piece menu are clicked with the same pointer the ghost is aimed with, so a UI-only mode
	 * would stop the pawn flying and a game-only mode would put the cursor away.
	 *
	 * Hidden during capture, which is the right-drag: the look chord takes capture, the pointer
	 * vanishes for the length of the drag and comes back where it was on release — §d's "cursor
	 * hidden, recentred on release". Not locked to the viewport, since a session is played in a
	 * window as often as not and a lock the player did not ask for reads as the game hanging.
	 */
	SetInputMode(
		FInputModeGameAndUI()
			.SetHideCursorDuringCapture(true)
			.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock));
}

void ADestructionGamePlayerController::OnSessionShortcut(DestructionSession::EToolbarButtonId Id)
{
	OnToolbarButton(Id);
}

void ADestructionGamePlayerController::OnSessionToggleMode()
{
	ToggleSessionMode();
}

void ADestructionGamePlayerController::OnSessionTogglePlacement()
{
	ToggleSessionPlacement();
}

void ADestructionGamePlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	// only add IMCs for local player controllers
	if (IsLocalPlayerController())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
		{
			/*
			 * One loop, two priorities: the session's context is applied above the other two, for
			 * the chord-ordering reason SessionMappingContextPriority's header gives. The special
			 * case is here rather than in a second AddMappingContext call because an apply written
			 * a second way is an apply a later route can forget.
			 */
			for (UInputMappingContext* CurrentContext : DefaultMappingContexts)
			{
				const int32 Priority = CurrentContext == SessionMappingContext
					? SessionMappingContextPriority
					: PieceMenuMappingContextPriority;

				Subsystem->AddMappingContext(CurrentContext, Priority);
			}
		}
	}

	/*
	 * On Started, and exactly once. With no explicit trigger on the action, Triggered fires
	 * every frame the button is held, so holding LMB would re-trace and re-present the menu
	 * sixty times a second; Completed is the release, so opening a menu on let-go is a one-shot
	 * press. A second binding for the same action would run the handler twice per click, which,
	 * now that a miss dismisses, is open-then-immediately-close.
	 */
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(InputComponent))
	{
		if (InspectPieceAction != nullptr)
		{
			EnhancedInputComponent->BindAction(
				InspectPieceAction,
				ETriggerEvent::Started,
				this,
				&ADestructionGamePlayerController::OnInspectPiece);
		}

		/*
		 * Hover binds on Triggered, the opposite of the line above and the point: hovering is a
		 * continuous axis, and with no explicit trigger asset Enhanced Input actuates it on every
		 * frame its value is non-zero — exactly the frames the mouse moved, when what is under
		 * the cursor can have changed. Started would fire only on the first frame of a gesture,
		 * leaving the highlight stale for the rest of a drag; Completed only when the mouse
		 * stops, always one brick behind. A still mouse costs no traces, since an unactuated axis
		 * fires nothing, and exactly once — a second binding would trace and re-highlight twice
		 * on every moved frame for an answer already correct.
		 */
		if (HoverPieceAction != nullptr)
		{
			EnhancedInputComponent->BindAction(
				HoverPieceAction,
				ETriggerEvent::Triggered,
				this,
				&ADestructionGamePlayerController::OnHoverPiece);
		}

		/*
		 * The session's eight shortcuts, on Started, exactly once each — one-shot presses of
		 * digital keys. Triggered would fire on every frame held, walking the build plane up the
		 * wall at sixty courses a second on `]` or re-settling (releasing pieces irreversibly) on
		 * a held `Enter`; Completed would run the command on let-go. Exactly once, not at least
		 * once: two bindings on `Tab` would toggle the mode twice per press, appearing to do
		 * nothing at all.
		 *
		 * Six of them carry their id as a bound payload, since a shortcut is a toolbar click and
		 * the model's greying and refusals are consulted at the one door either way. The mode and
		 * placement keys stand for a pair of chips each, so they go through the toggles, which
		 * read the session before they choose.
		 */
		const auto BindSessionShortcut =
			[this, EnhancedInputComponent](
				UInputAction* Action, DestructionSession::EToolbarButtonId Id)
			{
				if (Action != nullptr)
				{
					EnhancedInputComponent->BindAction(
						Action,
						ETriggerEvent::Started,
						this,
						&ADestructionGamePlayerController::OnSessionShortcut,
						Id);
				}
			};

		BindSessionShortcut(SessionPieceBrickAction, DestructionSession::EToolbarButtonId::PieceBrick);
		BindSessionShortcut(SessionPiecePlateAction, DestructionSession::EToolbarButtonId::PieceTimberPlate);
		BindSessionShortcut(SessionPieceLintelAction, DestructionSession::EToolbarButtonId::PieceTimberLintel);
		BindSessionShortcut(SessionCourseUpAction, DestructionSession::EToolbarButtonId::CourseUp);
		BindSessionShortcut(SessionCourseDownAction, DestructionSession::EToolbarButtonId::CourseDown);
		BindSessionShortcut(SessionRunAction, DestructionSession::EToolbarButtonId::RunStructure);

		if (SessionToggleModeAction != nullptr)
		{
			EnhancedInputComponent->BindAction(
				SessionToggleModeAction,
				ETriggerEvent::Started,
				this,
				&ADestructionGamePlayerController::OnSessionToggleMode);
		}

		if (SessionSnapToggleAction != nullptr)
		{
			EnhancedInputComponent->BindAction(
				SessionSnapToggleAction,
				ETriggerEvent::Started,
				this,
				&ADestructionGamePlayerController::OnSessionTogglePlacement);
		}
	}
}

void ADestructionGamePlayerController::OnInspectPiece()
{
	FVector StartCm;
	FVector Direction;

	/*
	 * No viewport means no ray at all, and the out parameters are left untouched — so this
	 * returns rather than tracing along whatever was on the stack.
	 */
	if (!DeprojectMousePositionToWorld(StartCm, Direction))
	{
		return;
	}

	/*
	 * Through the session's dispatch rather than straight at the inspect, because what a click
	 * means now depends on the mode: in Build it lays a piece, and in Destroy it is the inspect
	 * this handler has always made. The deprojection and the reach are unchanged — the untestable
	 * inch stays exactly as long as it was.
	 */
	PrimaryAlongRay(StartCm, StartCm + Direction * PieceMenuCursorReachCm);
}

void ADestructionGamePlayerController::OnHoverPiece()
{
	FVector StartCm;
	FVector Direction;

	/*
	 * Same untestable inch as OnInspectPiece, and the same failure closed: no viewport means no
	 * ray, and the out parameters are left untouched, so this returns rather than tracing along
	 * whatever happened to be on the stack.
	 */
	if (!DeprojectMousePositionToWorld(StartCm, Direction))
	{
		return;
	}

	/* The same dispatch as OnInspectPiece, for the same reason — pointing means two things now. */
	PointerAlongRay(StartCm, StartCm + Direction * PieceMenuCursorReachCm);
}

void ADestructionGamePlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

	RefreshBuildPreviewFromCursor();
}

void ADestructionGamePlayerController::RefreshBuildPreviewFromCursor()
{
	/*
	 * Not while the look chord is held. The right button turns the camera and the cursor is
	 * hidden for the length of the drag, so there is nothing on screen for the ghost to follow,
	 * and re-previewing every frame would drag it across the plot behind the player's back. The
	 * button is read directly rather than through IA_LookModifier because the action has no
	 * handler to reach; the two are the same press, and IMC_MouseLook's chord is what makes it
	 * mean "look".
	 */
	if (IsInputKeyDown(EKeys::RightMouseButton))
	{
		return;
	}

	/*
	 * No viewport, no cursor. GetMousePosition answers false when there is no local player or no
	 * viewport to read one from — a headless run, exactly — leaving its out-params untouched, so
	 * this returns rather than deprojecting whatever was on the stack.
	 */
	float CursorXPx = 0.0f;
	float CursorYPx = 0.0f;

	if (!GetMousePosition(CursorXPx, CursorYPx))
	{
		return;
	}

	/*
	 * And a still mouse costs nothing. A cursor that has not moved names the same point on the
	 * same plane and re-solves the same snap; the settings half of this slice is already carried
	 * by the component's own setters, so nothing needs the pointer re-read on a frame it did not
	 * move.
	 */
	const FVector2D CursorPx(CursorXPx, CursorYPx);

	if (bHasBuildCursorPx && CursorPx == LastBuildCursorPx)
	{
		return;
	}

	LastBuildCursorPx = CursorPx;
	bHasBuildCursorPx = true;

	/*
	 * The same untestable inch as OnHoverPiece, and the same failure closed. Everything that can
	 * be wrong in a way a player would notice — which mode this may run in, where the ghost
	 * lands, whether it shows — is behind RefreshBuildPreviewFromRay.
	 */
	FVector StartCm;
	FVector Direction;

	if (!DeprojectMousePositionToWorld(StartCm, Direction))
	{
		return;
	}

	RefreshBuildPreviewFromRay(StartCm, Direction);
}

void ADestructionGamePlayerController::BeginPlay()
{
	Super::BeginPlay();

	/*
	 * The cursor comes up once, here, for the whole session. A toolbar that is on screen in both
	 * modes has to be clickable in both, and the alternative this replaced — raising the pointer
	 * only while a piece menu was up — made the strip reachable only by first opening a menu over
	 * a brick.
	 */
	SetSessionControls();

	BuildScenarioLabelWidget();
}

void ADestructionGamePlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	RemoveScenarioLabelWidget();
	RemoveSessionToolbarWidget();

	Super::EndPlay(EndPlayReason);
}

void ADestructionGamePlayerController::BuildScenarioLabelWidget()
{
	UWorld* const World = GetWorld();

	UGameViewportClient* const Viewport = World != nullptr ? World->GetGameViewport() : nullptr;

	/* No viewport means no banner, the ordinary case in a test rather than an error. */
	if (Viewport == nullptr)
	{
		return;
	}

	/*
	 * Every string is an attribute and not a value, so the countdown runs. Slate asks an
	 * attribute again on every paint, which is what turns "4.0 s" into a clock without anything
	 * here holding a timer, a tick or a copy of the label.
	 *
	 * Bound through MakeAttributeUObject rather than a lambda capturing `this`. The banner is
	 * handed to the viewport, which holds a shared reference to it; a UObject delegate is not
	 * invoked once its object has gone, so a controller destroyed before its remove ran cannot be
	 * read through a widget the viewport is still painting.
	 */
	ScenarioLabelWidget = SNew(SBox)
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Top)
		.Padding(FMargin(0.0f, ScenarioBannerTopMarginPx, 0.0f, 0.0f))
		[
			SNew(SBox)
			.WidthOverride(ScenarioBannerWidthPx)
			[
				SNew(SBorder)
				.BorderImage(PieceMenuFillBrush())
				.BorderBackgroundColor(PieceMenuPanelBackgroundColour)
				.Padding(FMargin(14.0f, 10.0f))
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Font(PieceMenuHeaderFont())
						.ColorAndOpacity(PieceMenuHeaderColour)
						.AutoWrapText(true)
						.Text(MakeAttributeUObject(
							this, &ADestructionGamePlayerController::ScenarioLabelTitleText))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 6.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Font(PieceMenuBodyFont())
						.ColorAndOpacity(PieceMenuReadoutColour)
						.AutoWrapText(true)
						.Text(MakeAttributeUObject(
							this, &ADestructionGamePlayerController::ScenarioLabelExpectationText))
					]
					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 6.0f, 0.0f, 0.0f)
					[
						SNew(STextBlock)
						.Font(PieceMenuBodyFont())
						.ColorAndOpacity(PieceMenuCountColour)
						.AutoWrapText(true)
						.Text(MakeAttributeUObject(
							this, &ADestructionGamePlayerController::ScenarioLabelCutText))
					]
				]
			]
		];

	Viewport->AddViewportWidgetContent(ScenarioLabelWidget.ToSharedRef());
}

void ADestructionGamePlayerController::RemoveScenarioLabelWidget()
{
	if (!ScenarioLabelWidget.IsValid())
	{
		return;
	}

	UWorld* const World = GetWorld();

	if (UGameViewportClient* const Viewport = World != nullptr ? World->GetGameViewport() : nullptr)
	{
		Viewport->RemoveViewportWidgetContent(ScenarioLabelWidget.ToSharedRef());
	}

	ScenarioLabelWidget.Reset();
}

DestructionScenarios::FScenarioLabel ADestructionGamePlayerController::ScenarioLabelNow() const
{
	UWorld* const World = GetWorld();

	const ADestructionGameGameMode* const GameMode =
		World != nullptr ? World->GetAuthGameMode<ADestructionGameGameMode>() : nullptr;

	return GameMode != nullptr
		? GameMode->GetScenarioLabel()
		: DestructionScenarios::FScenarioLabel();
}

FText ADestructionGamePlayerController::ScenarioLabelTitleText() const
{
	return FText::FromString(ScenarioLabelNow().TitleText);
}

FText ADestructionGamePlayerController::ScenarioLabelExpectationText() const
{
	return FText::FromString(ScenarioLabelNow().ExpectationText);
}

FText ADestructionGamePlayerController::ScenarioLabelCutText() const
{
	return FText::FromString(ScenarioLabelNow().CutText);
}
