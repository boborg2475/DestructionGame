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
 * File-local names carry a PieceMenu prefix. An anonymous namespace is private to a
 * TRANSLATION UNIT rather than to a file, and a unity build merges many files into one —
 * so two file-local names that collide are a hard compile error between files that never
 * refer to each other. See CURRENT_STATE.md.
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
	 * THE REF IS RESOLVED RATHER THAN INDEXED, so a ref naming a piece that has gone — which
	 * is what every ref becomes the moment a commit runs — answers null instead of reaching
	 * a tombstoned slot. GetActor already answers null for INDEX_NONE, for a removed piece
	 * and for an actor destroyed by any route, so the cast is the only check left.
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
	 * WHETHER A STRUCTURE HAS ANYTHING IN IT WORTH COMMANDING — ONE READING, USED TWICE.
	 *
	 * LIVE PIECES RATHER THAN PIECES, because RemovePiece TOMBSTONES instead of compacting: a plot
	 * whose every brick has been deleted still answers a piece count. Which structure the session
	 * names and whether that structure has anything in it are the same question asked from two
	 * places — GetSessionStructureId choosing between the player's build and the level's wall, and
	 * RefreshSessionHasStructure setting the flag both commands are greyed on — and they agree for
	 * every state except exactly one: a build that has had pieces and has none left. Asked two ways
	 * there, the choice picks the emptied build and the flag then reads zero off it, so one deleted
	 * brick greys Run over a wall standing in front of the player. One function, so they cannot
	 * drift again.
	 */
	bool SessionStructureIsLive(const FStructureBinding* Binding)
	{
		return Binding != nullptr && Binding->GetStructure().NumLivePieces() > 0;
	}
}

/*
 * THE PRIORITY THE CONTEXTS ARE APPLIED AT, NAMED BECAUSE IT IS NOW USED TWICE. The menu
 * removes IMC_MouseLook and puts it back, and a restore at a different priority would change
 * which context wins a shared key without changing anything visible at the call site.
 *
 * The name is deliberately not a bare `MappingContextPriority`: a unity build merges many
 * .cpp files into one translation unit, so a file-scope constant here shares a namespace with
 * every other file in the module. See CURRENT_STATE.md.
 */
static constexpr int32 PieceMenuMappingContextPriority = 0;

/*
 * AND THE SESSION'S CONTEXT GOES ABOVE THEM, WHICH IS WHAT MAKES THE CAMERA TURN AT ALL.
 *
 * IMC_MouseLook's one Mouse2D mapping is CHORDED on IA_LookModifier, and IMC_Session is where the
 * modifier is mapped to the right mouse button. UInputTriggerChordAction::UpdateState answers by
 * reading the chord action's TriggerStateTracker off the player input, and
 * UEnhancedPlayerInput::EvaluateInputImpl resets every mapping's trigger state at the END of the
 * frame rather than the start — its own comment says why: "Delay MappingTriggerState reset until
 * here to allow dependent triggers (e.g. chords) access to this tick's values". So the modifier's
 * mapping has to be evaluated EARLIER IN THE SAME FRAME than the mapping that chords off it, or the
 * chord reads a state cleared last frame, answers "not held", and free-look never triggers however
 * hard the button is held.
 *
 * A PRIORITY IS THE ONLY LEVER THAT REACHES ACROSS TWO CONTEXTS.
 * IEnhancedInputSubsystemInterface::ReorderMappings puts chording mappings before chorded ones
 * WITHIN ONE CONTEXT, and these two are in different assets, so that reorder never sees the pair.
 * Across contexts RebuildControlMappings orders by a priority-descending ValueSort, which decides
 * nothing between equals — at one shared priority the list came out with the chorded axis first and
 * a probe measured 0° of yaw. One above is what puts IMC_Session's mappings in front.
 */
static constexpr int32 SessionMappingContextPriority = PieceMenuMappingContextPriority + 1;

/*
 * How far a ray cast from the cursor reaches, in cm (1 uu = 1 cm), i.e. 100 m.
 *
 * ONE REACH FOR BOTH HANDLERS, because they are the same ray: what a click would hit and what
 * the cursor is pointing at must be the same brick, and two constants is two ways for them to
 * stop being.
 *
 * IT LIVES IN THE HANDLERS' HALF, WHICH IS THE UNTESTED ONE, so it is a reach rather than a
 * tuned threshold: the game mode's wall is about 6.6 m across and 3 m tall, and a flying
 * observer is expected to be tens of metres off it. Nothing downstream depends on the value —
 * the trace either hits a brick or it does not, and a miss dismisses.
 */
static constexpr double PieceMenuCursorReachCm = 10000.0;

/*
 * WHAT AN ENTRY ROW IS DRAWN IN, ACCORDING TO THE ONE BOOL THE MODEL ALREADY DECIDED.
 *
 * FInspectorPieceEntry::bIsLivePiece exists so that a brick a cascade removed and a perfectly
 * live one do not present identically — and it exists ON THE MODEL so that nothing here has to
 * resolve a ref to find out. Reading it into a colour is the whole use of it: no filtering, no
 * dropping the entry, and no second opinion about what the menu may then do about it, which is
 * PieceActionsFor's intersection and is already said by the action rows going empty.
 *
 * These are file-scope names in a unity build, hence the prefix; see the note above.
 */
static const FLinearColor PieceMenuLivePieceColour(1.0f, 1.0f, 1.0f, 1.0f);
static const FLinearColor PieceMenuDeadPieceColour(0.5f, 0.5f, 0.5f, 0.6f);

/*
 * THE PANEL'S SIZE IS NOT HERE ANY MORE, AND ITS ABSENCE IS THE POINT.
 *
 * A PANEL THAT CANNOT CHANGE SIZE CANNOT MOVE ANYTHING, which is still the single property every
 * stillness claim about this menu rests on: whatever the readout is showing and however many bricks
 * are picked, every row is where it was, so a click aimed at one commits that one. What changed is
 * that the size is now a function of the DETAIL MODE — EPieceMenuDetail::Compact drops the joint
 * table and the headroom scale, and a mode that drew the lines it has left into the same rectangle
 * gave the player back no screen at all, which was their complaint.
 *
 * PieceMenuPanelSizePx IS WHERE IT LIVES, IN Core, BESIDE THE DERIVATION OF BOTH FIGURES.
 * Presenter.PieceMenuPanelSize holds the orderings and the area budget, and
 * World.Menu.TheReadoutFitsInsideThePanel arranges this very panel in both modes and holds the
 * rectangle it lands in against what that function said. Two constants here would be a second copy
 * of the answer with no test between them.
 */

/**
 * HOW FAR IN FROM THE VIEWPORT'S RIGHT EDGE THE PANEL OPENS.
 *
 * THE OLD FIGURE, COMING BACK AS AN ARGUMENT. The panel used to be an SBox at HAlign_Right /
 * VAlign_Center with a 24 px margin, argued on the record as keeping the readout off the wall the
 * player is pointing at; making it draggable replaced the alignment with an offset and the home
 * defaulted to the origin. PieceMenuHomeOffset takes this as a parameter rather than spelling it,
 * which is what lets Presenter.PanelHomeOffset sweep no margin, this one, one wider than the screen
 * and a negative one — a value passed in is a value a test can vary.
 */
static constexpr double PieceMenuPanelHomeMarginPx = 24.0;

/** The gap between the panel's background and anything drawn on it. */
static constexpr float PieceMenuPanelPaddingPx = 10.0f;

/**
 * HOW TALL THE BRICK LIST MAY GET, WHICH IS WHAT MAKES A LONG SELECTION SURVIVABLE.
 *
 * About eight rows. Past that the list scrolls INSIDE this height rather than growing, so forty
 * picked bricks push nothing off the bottom of the screen and move neither the readout nor the
 * row that deletes them.
 *
 * A CAP RATHER THAN A HEIGHT, AND THE DIFFERENCE IS WHAT A SHORT SELECTION LOOKS LIKE. Stated as a
 * fixed height this reserved all eight rows for three bricks, stranding about 140 px of empty
 * panel between the last brick and the readout — a fifth of the panel, reading as a menu that had
 * failed to finish drawing. Capping does the same job for the long selection, because the hazard
 * there is the list growing PAST this figure, and lets a short one take only the room it needs.
 */
static constexpr float PieceMenuBrickListMaxHeightPx = 190.0f;

/*
 * THE HEADROOM BAR'S TRACK: ONE SIZE FOR EVERY JOINT, SO THE BARS READ AS A COLUMN — AND THE
 * WIDTH IS SET BY THE SCALE UNDER IT RATHER THAN BY THE BAR.
 *
 * The decade labels are placed along this same width, each straddling its own fraction of it, so
 * the four of them have to fit side by side with air between them: "1×", "10×", "100×" and
 * "1000×" measure 70 px between them at the scale's font, and the two crowded ones at the top end
 * left a third of a pixel between them on the 96 px track this replaces — "100×1000×" rendered as
 * one string on the axis whose entire job is to say which decade a fill means.
 *
 * 140 px LEAVES 15 px BETWEEN THE TIGHTEST PAIR, against the 4 px World.Menu.HeadroomTicksStay-
 * InsideTheBarTheyLabel asks for, and it is bounded from the other side: every pixel here pushes
 * the joint sentence beside it further right, and the bar and the swatch column below together
 * take 166 px off the front of every one of those.
 *
 * WHAT IS LEFT TO SPEND IS 22 px, not the 112 this once read. That earlier figure was taken on a
 * flush wall, which bends nowhere and so never prints the bending clause; World.Menu.TheReadout-
 * FitsInsideThePanel now sweeps a wall with a corbel in it too, and the longest sentence that one
 * produces clears the column by 22 px at the panel's present width. A wider bar has to come out
 * of that, or out of PieceMenuPanelSizePx's full width alongside it.
 */
static constexpr float PieceMenuHeadroomBarWidthPx = 140.0f;
static constexpr float PieceMenuHeadroomBarHeightPx = 8.0f;

/*
 * THE SWATCH THAT TIES A JOINT ROW TO THE BRICK ON THE FAR END OF IT.
 *
 * A joint row already names its neighbour in words — "course 2 · #4" — and in a wall of 1,220
 * identical bricks a word is not enough to find one by. FInspectorJointRow::ColourSlot is the
 * model's answer to WHICH colour each row takes; this is the size of the block it is painted in,
 * and the gap between it and the bar. The scale row below the bars carries the same total as a
 * left padding, so the ticks stay under the fills they label rather than under the swatches.
 */
static constexpr float PieceMenuJointSwatchWidthPx = 10.0f;
static constexpr float PieceMenuJointSwatchHeightPx = 10.0f;
static constexpr float PieceMenuJointSwatchGapPx = 6.0f;

/*
 * HOW WIDE THE COLUMN OF SUPPORT WORDS ON THE BRICK ROWS IS.
 *
 * Wide enough for the longest of them — "not in this wall" — with room to spare, so the words line
 * up in a column instead of ragging off the ends of labels of different lengths. It is deliberately
 * generous: the entry rows are swept by World.Menu.TheReadoutFitsInsideThePanel along with
 * everything else the model supplies, and a column that just fitted would make an entry row the
 * tightest line on the panel and quietly retarget that test's reported budget away from the joint
 * sentences it exists to measure.
 */
static constexpr float PieceMenuEntrySupportWidthPx = 150.0f;

/** The row the bar's decade ticks are placed along, directly under that column. */
static constexpr float PieceMenuHeadroomScaleHeightPx = 14.0f;

/** The rule that separates the destructive row from everything describing what it destroys. */
static constexpr float PieceMenuRuleHeightPx = 1.0f;

/*
 * HOW WIDE THE SCENARIO BANNER IS, AND HOW FAR OFF THE TOP OF THE SCREEN IT SITS.
 *
 * A stated width rather than a fit to the text, because the expectation lines run to two full
 * sentences and a banner sized to its content would stretch to the width of the viewport and put a
 * two-hundred-character line across the top of the wall. Wrapped inside a fixed width, the same
 * text is three or four readable lines. Centred at the top is the one region the piece-menu panel
 * never opens into — it homes against the right edge — so the two readouts cannot overlap.
 */
static constexpr float ScenarioBannerWidthPx = 760.0f;
static constexpr float ScenarioBannerTopMarginPx = 24.0f;

/*
 * WHAT THE PANEL IS DRAWN IN, AND THE BACKGROUND IS THE ONE THAT IS NOT DECORATION.
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
 * WHAT THE TITLE STRIP IS TINTED, WHICH IS THE ONLY THING SAYING THE PANEL CAN BE MOVED.
 *
 * A lift off the panel's own background rather than a colour of its own: it has to read as part of
 * the panel and as a separate strip at the same time, which is what a title bar is. The other half
 * of the affordance is the grab cursor, and between them there is no word — a word would be a word
 * chosen in the one place no test can read it, and FPieceMenuInspector is where the panel's words
 * are decided.
 */
static const FLinearColor PieceMenuGrabStripColour(0.16f, 0.18f, 0.24f, 0.75f);

/*
 * THE THREE BAND COLOURS ARE NOT HERE ANY MORE, AND THEIR ABSENCE IS THE POINT — the same move the
 * neighbour palette made, for the same reason and one slice later.
 *
 * EJointMarginBand says WHERE the colour changes — which side of 10x and of 2x margin a joint falls
 * on — because that is a decision about what this game calls dangerous, and it belongs where a test
 * can read it. What was left here was the hue; the load overlay then needed the SAME three hues in
 * three MATERIALS, which are content and cannot reach a file-static in a widget. Two copies of green
 * would have been a brick tinted one green beside a bar drawn another, two inches apart.
 *
 * DestructionContent::BrickLoadSwatchColours now sits beside the three material paths it has to
 * agree with, one row per band, and there is deliberately no colour literal left in this file for a
 * bar to drift back to.
 */

/*
 * AND WHAT EACH SUPPORT BUCKET'S DOT IS DRAWN IN, ON THE SAME TERMS AS THE BAR ABOVE.
 *
 * EPieceSupportBand says WHICH bucket a brick is in — the model's decision, swept against the
 * word beside it — and this is the hue, which is the half nothing headless can judge. Forty
 * picked bricks are forty lines of small text without it, and the one that is falling reads
 * exactly like the thirty-nine that are not until somebody reads every word.
 *
 * THE ALARM COLOURS ARE THE BAR'S OWN, DELIBERATELY. A falling brick takes the same red a joint
 * past its limit does and a stranded one the same amber as a joint running out of room, because
 * one panel wants one vocabulary: a colour that means "look at this" in the top half and
 * something else in the bottom half is two vocabularies to learn. The other three are outside
 * that vocabulary on purpose — resting, held, and the two that are not claims about a brick at
 * all — so nothing calm can be mistaken for an alarm.
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
 * THE NEIGHBOUR PALETTE IS NOT HERE ANY MORE, AND ITS ABSENCE IS THE POINT.
 *
 * It used to be a file-static array of six colours in this file, with a comment claiming they were
 * "exactly" the emissives of Content/Materials/M_BrickNeighbour0..5 — a second copy of six numbers
 * whose only tie to the first was that sentence. A palette repick changed three of the assets and
 * left the sentence, so the swatch column drew amber, chartreuse and teal beside bricks lit green,
 * clay and sage, with the whole suite green.
 *
 * DestructionContent::BrickNeighbourSwatchColours now sits beside the material paths it must agree
 * with, one row per slot, and Content.NeighbourSwatchesMatchTheirMaterials holds the two together.
 * There is deliberately no colour literal left in this file for a swatch to drift back to.
 *
 * SIX, WHICH IS THE MODEL'S NUMBER: a brick inside a running bond has six joints. A row past the
 * end carries INDEX_NONE and gets the transparent entry below, so the swatch is ABSENT rather
 * than repeated — a repeated swatch is a wrong answer about which brick is which, and an absent
 * one is merely an absence.
 */

/** What a row past the end of the palette is painted in: nothing at all. */
static const FLinearColor PieceMenuNoSwatchColour(0.0f, 0.0f, 0.0f, 0.0f);

/*
 * AND WHAT A ROW THAT DESTROYS SOMETHING IS DRAWN IN, TAKEN FROM FPieceMenuRow::bIsDestructive.
 *
 * The flag is the ACTION'S OWN, carried across by the presenter, so this is a colour keyed on
 * data rather than a widget comparing a caption against the word "Delete" — which is the policy
 * in a string literal that FPieceAction::bIsDestructive exists to make unnecessary.
 */
static const FLinearColor PieceMenuDestructiveRowColour(0.72f, 0.16f, 0.14f, 1.0f);
static const FLinearColor PieceMenuOrdinaryRowColour(1.0f, 1.0f, 1.0f, 1.0f);

/*
 * THE SESSION STRIP'S OWN MEASUREMENTS, FROM SESSION_UI_DESIGN.md §e.
 *
 * 48 px TALL IS AN OWNER RULING RATHER THAN A FIT: the first cut was 72 and was "way too big". A
 * 34 px chip inside it leaves 7 px of air above and below, which is what makes the strip read as a
 * bar with buttons on it rather than as a row of buttons.
 *
 * NOTHING HERE IS MEASURED BY ANY TEST, and that is the division of labour the panel tests already
 * draw: which buttons, in what order, greyed or live, focusable or not is the model's and is
 * asserted; every pixel below is this file's and is looked at by a human on the screenshot proof.
 */
static constexpr float SessionToolbarHeightPx = 48.0f;
static constexpr float SessionToolbarChipHeightPx = 34.0f;
static constexpr float SessionToolbarChipGapPx = 5.0f;
static constexpr float SessionToolbarChipPaddingPx = 12.0f;
static constexpr float SessionToolbarEdgePaddingPx = 10.0f;
static constexpr float SessionToolbarReadoutPaddingPx = 6.0f;

/*
 * THE HAIRLINE BETWEEN TWO GROUPS, AND THE AIR EITHER SIDE OF IT. §e's 10 px between groups, spent
 * as the chip gap on the left of the rule and the rest on its right.
 */
static constexpr float SessionToolbarRuleWidthPx = 1.0f;
static constexpr float SessionToolbarRuleGapPx = 10.0f;

/*
 * AND THE TWO PIECE SWATCHES, WHICH ARE TWO SHAPES AS WELL AS TWO COLOURS.
 *
 * THE PLANK IS LONGER AND THINNER THAN THE BLOCK, and that is what makes the three piece chips
 * readable from each other without reading the words — which is the whole of why §e draws a swatch
 * "in place of a size caption". The proportions are the pieces' own, roughly: a brick is about twice
 * as long as it is tall in elevation and a board is three times that.
 */
static constexpr float SessionToolbarBrickSwatchWidthPx = 18.0f;
static constexpr float SessionToolbarBrickSwatchHeightPx = 11.0f;
static constexpr float SessionToolbarTimberSwatchWidthPx = 26.0f;
static constexpr float SessionToolbarTimberSwatchHeightPx = 8.0f;
static constexpr float SessionToolbarSwatchGapPx = 7.0f;

/*
 * WHAT THE STRIP IS DRAWN IN, AND THE FILL IS ONE STEP LIGHTER THAN THE PANEL'S ON PURPOSE.
 *
 * The details window sits on PieceMenuPanelBackgroundColour and the strip sits on this, so the two
 * read as separate objects rather than as one dark shape with a seam in it. Both are the design's
 * LINEAR triples, which is the number Slate takes — the sRGB hexes in §e are what the eye checks
 * them against, and confusing the two is how a palette drifts.
 *
 * THE ACCENTS AND THE CHIP FILLS ARE NOT HERE ANY MORE, AND THEIR ABSENCE IS THE POINT. They were
 * two file-static colours and a pair of ternaries in the panel builder, which made "how a chip is
 * drawn" a decision in the one place no test can reach — and the multiply through FCoreStyle's grey
 * button brush meant the amber this project chose was never the amber a player saw. The whole look
 * is DestructionSession::ChipLookFor's answer now, swept by Core.SessionToolbar.ChipLook, and this
 * file turns it into a brush.
 */
static const FLinearColor SessionToolbarFillColour(0.020f, 0.023f, 0.030f, 0.96f);

/*
 * A SECOND FILE-LOCAL NAMESPACE, BELOW THE CONSTANTS IT READS RATHER THAN BESIDE THE ONE AT THE
 * TOP OF THE FILE. Everything in here draws the panel and every one of them needs a size or a
 * colour declared above, so the split is declaration order rather than a second grouping. The
 * PieceMenu prefix is the same unity-build rule the note above states.
 */
namespace
{
	/*
	 * THE STYLE COMES FROM FCoreStyle RATHER THAN FAppStyle, AND THAT IS DELIBERATE. FAppStyle
	 * resolves to whichever style the running application registered — the editor's, in an editor
	 * binary, and the core one in a cooked game — so a panel styled through it looks different in
	 * the two places this menu is looked at. FCoreStyle is the same in both and needs no content
	 * asset, which is what keeps the background off RequiredContent's table.
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
	 * THE TWO CAPTION FACES, AND WHICH ONE A CHIP WEARS IS THE MODEL'S ANSWER.
	 *
	 * The chip says `bActive` twice — in the caption's weight and in the chip's fill — because they
	 * are two readings of one decision, and either alone is fragile: a player reading the strip from
	 * peripheral vision sees the fill, and a player looking straight at it reads the word. Which of
	 * the two a chip gets is FChipLook::bBoldCaption rather than a ternary here, for the reason the
	 * fill is FChipLook::Fill.
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
	 * CLAMPED AT WHITE RATHER THAN LERPED, so a channel already at full stays put instead of the
	 * whole colour drifting. The clamp is FMath::Min, which REPLACES a NaN rather than discarding it
	 * — the right direction here: a look that is not a number must stay not a number rather than
	 * becoming a plausible colour, and Core.SessionToolbar.ChipLook sweeps every look for finiteness
	 * so one can never arrive.
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
	 * A CHIP'S WHOLE STYLE, BUILT FROM THE LOOK THE MODEL DECIDED.
	 *
	 * A ROUNDED BOX BRUSH RATHER THAN FCoreStyle'S BUTTON BRUSH WITH A COLOUR MULTIPLIED THROUGH IT,
	 * and this is the defect the slice exists to close: the stock brush is GREY, so multiplying the
	 * design's amber into it produces dark mustard — the accent this project chose was never the
	 * accent on screen, and no retune of the constant could fix it because the thing being multiplied
	 * into is grey. The brush has to be the chip's own.
	 *
	 * ALL FOUR STATES ARE ROUNDED. SButton swaps its border brush for the state it is in, so a square
	 * disabled or pressed brush would be a chip that changed shape under the cursor.
	 */
	FButtonStyle SessionToolbarChipStyle(const DestructionSession::FChipLook& Look)
	{
		/* All four corners the same. FVector4 rather than FVector4f: FSlateBrushOutlineSettings' own. */
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
	 * THE LITTLE BLOCK OF COLOUR THAT MAKES A PIECE CHIP LOOK LIKE THE THING IT LAYS.
	 *
	 * ONE WIDGET, AND ITS SIZE IS ITS OWN PADDING. A border with nothing inside it is a filled
	 * rectangle whose desired size is exactly the padding around the nothing, which is what lets the
	 * swatch be a single widget rather than a sizing box wrapped around an image — and a single
	 * widget is what a reader, human or test, can point at and call "the swatch".
	 *
	 * THE COLOUR IS THE MODEL'S. SwatchColour(Kind) is the shed material's own base colour, so the
	 * palette chip and the brick that lands are one decision rather than two people picking the same
	 * red.
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
	 * THE 1 px RULE THAT SEPARATES TWO REGIONS OF THE STRIP.
	 *
	 * IT IS DRAWN WHERE THE MODEL'S GROUP CHANGES AND NOWHERE ELSE, which is §b's reason rather than
	 * a decoration: the commands sit past a rule "so that a destructive click is never adjacent to a
	 * setting click". Full chip height, so it reads as a division of the bar rather than as a tick.
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
	 * What a bar in this band is filled in — A LOOKUP, WHICH IS ALL A WIDGET MAY DO WITH IT.
	 *
	 * The band arrived decided: Presenter.PieceMenuJointMarginBand pins which side of each edge
	 * every joint falls on, including the two boundary rows a hand-picked example never contains.
	 * Nothing here compares a number against anything, so there is no second copy of that rule to
	 * drift — and the arm past the end of the enumeration answers with the most severe colour,
	 * because a bar that is wrong about its own band must not look calm.
	 *
	 * THE TABLE IS THE OVERLAY'S, AND THAT IS WHAT MAKES THE BAR AND THE BRICK ONE DECISION. The
	 * three colours live beside the three load-overlay material paths in RequiredContent.h, indexed
	 * by the band itself, so this is a subscript rather than a second palette.
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
	 * What a brick row's support dot is painted in — THE SAME SHAPE OF LOOKUP, ON THE SAME TERMS.
	 *
	 * The bucket arrived decided: Presenter.PieceMenuSupportBand pins which bucket every state of
	 * every brick falls in, and CheckInspectorInvariants holds each row's bucket against that row's
	 * own word over every readout the suite builds. Nothing here compares a string, a support
	 * enumerator or a live-piece flag against anything, so there is no second copy of that rule to
	 * drift — and the arm past the end of the enumeration answers with the grey that claims nothing,
	 * because a dot that is wrong about its own bucket must not assert a physical state.
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
	 * THE DOT THAT SAYS WHETHER A PICKED BRICK IS STANDING UP, WITHOUT ITS ROW BEING READ.
	 *
	 * IT SITS INSIDE THE SUPPORT COLUMN RATHER THAN AT THE HEAD OF THE ROW, which is a layout
	 * decision with a measured reason: the column is a fixed-width box, so a dot placed inside it
	 * takes its space out of that box's own slack and moves nothing else on the row — while a dot
	 * ahead of the entry button would push every label and every word right by its width, out of
	 * the budget World.Menu.TheReadoutFitsInsideThePanel measures.
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
	 * WHICH slot a row takes is the model's answer and is swept over every readout in the suite;
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
	 * THE BLOCK OF COLOUR THAT TIES A JOINT ROW TO ITS NEIGHBOURING BRICK.
	 *
	 * IT IS DRAWN EVEN WHEN IT IS INVISIBLE, which is why the transparent colour goes through the
	 * same widget rather than through a slot that is not added. The bars have to line up in a
	 * column for the decade scale under them to mean anything, and a row that skipped its swatch
	 * would slide its bar 16 px left of every other one.
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
	 * ONE JOINT'S HEADROOM BAR, FILLED TO THE FRACTION THE MODEL WORKED OUT.
	 *
	 * THE FILL IS A LAID-OUT CHILD RATHER THAN A PAINTED RECTANGLE, WHICH IS THE WHOLE REASON
	 * THIS IS NOT AN SProgressBar. A bar is the one thing on this panel that can be wrong while
	 * every word beside it is right — a constant fill under a correct caption looks entirely
	 * plausible — and SProgressBar keeps its Percent in a private slate attribute with no getter,
	 * so nothing could ever read back what it drew. An anchored child's ARRANGED WIDTH is
	 * `HeadroomFraction` times the track's, exactly, and ArrangeChildren hands that to a headless
	 * test with no renderer and no accessor at all.
	 *
	 * THE MODEL'S NUMBER GOES STRAIGHT INTO THE ANCHOR. There is no arithmetic here and no clamp:
	 * FInspectorJointRow::HeadroomFraction is already a fraction, already log-scaled over three
	 * decades, and already swept for finiteness by Presenter.PieceMenuJointHeadroom.
	 *
	 * AND THE COLOUR COMES FROM THE BAND RATHER THAN FROM THE FRACTION, which is the same rule one
	 * field over: how full the bar is and how alarmed to be about it are two answers, and only the
	 * first is a length. Thresholding the fraction here would be the second answer written where
	 * nothing can read it.
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
	 * THE BAR'S DECADE TICKS, EACH STANDING WHERE THE MODEL PUT IT.
	 *
	 * A LOG AXIS WITH NO DECADES ON IT IS UNREADABLE BY CONSTRUCTION — the same visible fill means
	 * 1000x on one panel and 3x on another. FHeadroomScaleTick carries a Fraction as well as a
	 * label precisely so this can place each one by the same curve the fill is drawn by, rather
	 * than spreading four labels evenly and quietly promising a linear scale.
	 *
	 * THE ANCHOR IS THE SAME ARITHMETIC THE FILL IS DRAWN BY, AND THAT IS THE POINT OF PLACING THE
	 * TICKS ON A CANVAS AT ALL. PieceMenuHeadroomBar anchors its fill to Fraction of a track this
	 * wide; a tick anchors its label to Fraction of a canvas the same width, so the joint whose
	 * margin IS 10x has its fill end under the 10x label because both came out of one expression,
	 * not because two constants happen to agree.
	 *
	 * ALIGNMENT IS THE FRACTION RATHER THAN A HALF, WHICH IS WHAT KEEPS THE END LABELS ON THE BAR.
	 * SConstraintCanvas reads Alignment as the pivot INSIDE the child, so an alignment of 0.5 puts
	 * the label's middle on the anchor — and for the ticks at 0.0 and 1.0 that centres them on the
	 * track's two EDGES, with half of each hanging off and clipped away by the scroll box. Setting
	 * the pivot to the tick's own Fraction pins the label's left edge at the low end, its right
	 * edge at the high end and its middle in the middle: the label always straddles the point it
	 * names, and it lies wholly within the track for any label no wider than the track, because its
	 * left edge lands at Fraction * (TrackWidth - LabelWidth).
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
	 * wire up the mapping contexts here rather than in a Blueprint, so the sandbox
	 * runs from C++ defaults alone — by the paths RequiredContent.h names, so this
	 * constructor and the required-content table cannot become two lists that disagree
	 */
	static ConstructorHelpers::FObjectFinder<UInputMappingContext> DefaultContext(DestructionContent::DefaultMappingContextPath);
	static ConstructorHelpers::FObjectFinder<UInputMappingContext> MouseLookContext(DestructionContent::MouseLookMappingContextPath);

	/*
	 * the look context is remembered by name as well as applied — the SAME pointer in both
	 * places, so there is nothing to drift; its header says why it still has a name of its own
	 */
	MouseLookMappingContext = MouseLookContext.Object;

	DefaultMappingContexts.Add(DefaultContext.Object);
	DefaultMappingContexts.Add(MouseLookMappingContext);

	/*
	 * AND THE SESSION'S OWN KEYBOARD, PUSHED IN BESIDE THEM RATHER THAN APPLIED SEPARATELY.
	 *
	 * SetupInputComponent adds every context in this list, so a third one here is applied for the
	 * whole session by the code that already applies the other two — and an apply written a second
	 * way is an apply that can be forgotten on a route somebody adds later. Nine mappings in a
	 * context nothing applies is nine dead keys, and from the player's chair that looks exactly like
	 * eight missing BindAction calls.
	 *
	 * IT IS NAMED AS WELL AS LISTED, THE SAME POINTER IN BOTH PLACES, for the same reason
	 * MouseLookMappingContext is — and here the name carries a second job: it is what the one apply
	 * loop asks "is this the session's?" to give it SessionMappingContextPriority rather than the
	 * shared one. The priority's own header says why it cannot be the shared one.
	 */
	static ConstructorHelpers::FObjectFinder<UInputMappingContext> SessionContext(DestructionContent::SessionMappingContextPath);

	SessionMappingContext = SessionContext.Object;

	DefaultMappingContexts.Add(SessionMappingContext);

	/* the piece menu's own input, by the same one spelling of its path */
	static ConstructorHelpers::FObjectFinder<UInputAction> InspectPieceActionAsset(DestructionContent::InspectPieceActionPath);

	InspectPieceAction = InspectPieceActionAsset.Object;

	/* and the one that keeps the highlight under the cursor, mapped in IMC_Default beside it */
	static ConstructorHelpers::FObjectFinder<UInputAction> HoverPieceActionAsset(DestructionContent::HoverPieceActionPath);

	HoverPieceAction = HoverPieceActionAsset.Object;

	/*
	 * AND THE EIGHT SESSION SHORTCUTS, BY THE SAME ONE SPELLING OF EACH PATH.
	 *
	 * IA_LookModifier IS NOT AMONG THEM, DELIBERATELY. It does nothing on its own: it exists only
	 * to be the action IMC_MouseLook's chord watches, so there is no handler for it to reach and
	 * a reference here would be a UPROPERTY nothing ever reads.
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
	 * THE BUILD LOOP IS PART OF WHAT A CONTROLLER IS, so it is a default subobject rather than
	 * something a level or a Blueprint attaches. A session whose Build tab found no component would
	 * put the player in a mode where every click fails closed and nothing on screen says why.
	 */
	BuildComponent = CreateDefaultSubobject<UBuildModeComponent>(TEXT("BuildComponent"));

	/*
	 * AND THE SESSION OPENS IN DESTROY, WHICH IS DELIBERATELY NOT THE MODEL'S OWN DEFAULT.
	 *
	 * FSessionToolbarState::Mode defaults to Build and Core/SessionToolbar.h argues for it: a
	 * default-constructed session must be the one that cannot destroy anything. A CONTROLLER is a
	 * different question. Twenty-eight of the twenty-nine playable levels lay a structure and invite
	 * the player to pull it apart, and opening those in Build mode would hang a gold ghost over
	 * somebody else's wall and swallow the first click on it. The one build plot is put into Build
	 * mode by the game mode, through the same single door every other click goes through.
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
	 * THE PLAYER'S OWN BUILD WINS, BUT ONLY ONCE THERE IS SOMETHING LIVE IN IT.
	 *
	 * BeginBuild spends an id on an EMPTY binding the moment Build mode is entered, so "the
	 * component names a structure" is true long before there is anything to command. An empty build
	 * that won here would shadow the level's own wall with nothing — Run would solve an empty graph
	 * and report success, on a level with a wall standing in front of the player. A build whose
	 * every brick has been DELETED is the same emptiness wearing a piece count, which is why the
	 * question goes through SessionStructureIsLive rather than being asked a second way here.
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
	 * OTHERWISE THE LEVEL'S OWN WALL, WHICH IS WHAT MAKES A DESTROY SESSION ON A SCENARIO LEVEL
	 * ANYTHING BUT INERT. A row that built nothing — the build plot — leaves INDEX_NONE here, and
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
	 * WHAT WAS TINTED IS REMEMBERED BEFORE ANYTHING IS RECOMPUTED, and it is the half that has to be:
	 * a brick stops wearing a band without being touched at all — the overlay goes off, the session
	 * moves to another structure, a piece is pulled out — so a refresh that told only the NEW set
	 * would leave the old one coloured by a solve nobody can date. Same obligation, same shape, as
	 * RefreshNeighbourHighlights' "were neighbours" argument.
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
		 * A STRUCTURE THAT ALREADY HOLDS AN ANSWER IS READ, NEVER RE-SOLVED, AND THE SECOND SOLVE
		 * WOULD BE HARMFUL RATHER THAN MERELY WASTEFUL.
		 *
		 * A settle runs SolveAndBreak, whose equilibrium gate calls ApplyLimitAnalysisSupport and
		 * makes the LP the support authority below the block cap. A bare SolveLoads has no gate: it
		 * rebuilds the same per-piece arrays from the router's downward flood alone. So a refresh
		 * that solved unconditionally would overwrite the settle's verdict with a worse one on the
		 * very array ApplyResults releases from — looking at a wall would change what it does next.
		 *
		 * SO THE RULE IS "SOLVE WHEN THERE IS NO ANSWER", NOT "NEVER SOLVE". A freshly built plot has
		 * never been solved at all (laying a brick deliberately does not solve), and so has a
		 * structure that HAS been settled and then had a piece placed on it — the old pieces carry
		 * the settle's answer and the new one carries none. Asking per LIVE piece catches both:
		 * HasSupportAnswer is false for a handle added since the last solve, and a removed or
		 * released piece is not part of what the overlay is describing.
		 *
		 * AND WHEN IT DOES SOLVE IT IS NON-DESTRUCTIVE, which is the first thing in this game to lean
		 * on that sentence in anger. SolveLoads is documented as leaving every connection exactly as
		 * intact as it found it; SolveAndBreak is the deliberate step and is never reached from here.
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
			 * A HOLE WEARS NOTHING, AND NEITHER DOES A BRICK THAT HAS ALREADY GONE. A removed piece
			 * has no actor to tint and WorstJointBandForPiece fails it closed to Critical, which is
			 * the right answer for a READING of a handle and the wrong one to paint: the brick is
			 * gone, so there is nothing there to be in trouble.
			 *
			 * A RELEASED PIECE IS THE SAME FACT ONE STEP EARLIER — it is a rigid body falling through
			 * the air under Chaos, its joints say nothing about it any more, and it too reads
			 * Critical for want of support. Painting that is the instrument spending its loudest
			 * signal on a brick the player can already see moving, when the whole point of the red is
			 * to pick out the one that has not moved yet.
			 */
			LoadOverlayStates.Add(
				Binding->IsPieceRemoved(Index) || Binding->IsReleased(Index)
					? EBrickHighlight::None
					: BrickHighlightForLoadBand(
						WorstJointBandForPiece(Binding->GetStructure(), Index)));
		}
	}

	/*
	 * THEN THE UNION OF THE OLD SET AND THE NEW ONE IS PUT BACK THROUGH THE PRECEDENCE. Nothing here
	 * paints: every brick is asked afresh what state it should be in, so a hovered or selected brick
	 * keeps what it had and an unclaimed one falls to its band — or to None, which is what taking the
	 * overlay off means.
	 *
	 * THE OLD SET FIRST AND THE NEW SET SECOND, AND THE BRICKS IN BOTH ARE SIMPLY TOLD TWICE. That is
	 * harmless for the same reason RefreshNeighbourHighlights' overlap is: SetHighlighted is
	 * idempotent and HighlightForPiece is asked afresh each time, so a second telling cannot say
	 * anything different from the first.
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
	 * WHAT THERE IS TO COMMAND IS ASKED FIRST, BEFORE THE STRIP IS ASKED WHAT IS LIVE.
	 *
	 * `bHasStructure` is the precondition on both commands, and it is a fact about the world rather
	 * than a choice the player made — the brick they just laid, the cascade that just took six, the
	 * wall the level built. Asked after the greying check, Run would be refused on the first click
	 * after the build became real and accepted on the second, which reads as a dropped click.
	 */
	RefreshSessionHasStructure();

	/*
	 * AND THE REFUSAL IS THE MODEL'S, ASKED RATHER THAN RE-DECIDED. ApplyToolbarButton consults the
	 * same list for the same answer, so the only thing left to decide here is whether the SIDE
	 * EFFECT runs — and it must not. A greyed `Course down` that still pushed its own decrement onto
	 * the component would put the build plane under the earth with the readout saying course 0.
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
	 * THE SETTINGS ARE PUSHED FROM THE STATE THE TRANSITION PRODUCED, NEVER FROM THE BUTTON.
	 *
	 * `CourseUp` means "one more than whatever the course was", and the state is where that sum
	 * already lives — the component is told the number, not the gesture. It is also the only reading
	 * that stays right when a transition starts refusing or clamping something: whatever the model
	 * decided the course is, that is the course the build plane is derived from.
	 *
	 * AND THE COMPONENT DERIVES THE REST ITSELF. SetPieceKind takes the kind and works out the
	 * material, the half extent and the plane; grounded is never pushed at all — it is derived from
	 * the snapped pose inside the subsystem (DESIGN §8, 2026-09-15), and IsCourseGrounded is only
	 * what the readout intends.
	 */
	switch (Id)
	{
	case EToolbarButtonId::ModeBuild:
		/*
		 * A BUILD IS OPENED IF ONE IS NOT ALREADY. Build mode with no structure behind it is a mode
		 * in which every click fails closed against an unknown id — and opening one unconditionally
		 * would be worse, because BeginBuild cancels: a player who looked at a brick in Destroy mode
		 * and came back would find their plot swept.
		 */
		if (BuildComponent != nullptr && BuildComponent->GetStructureId() == INDEX_NONE)
		{
			BuildComponent->BeginBuild();
		}

		/*
		 * AND NOTHING IS DONE ABOUT THE CURSOR, WHICH IS A CHANGE AND NOT AN OMISSION. This used
		 * to raise it, because there is no aiming a ghost without a pointer. The pointer is the
		 * SESSION's now — SetSessionControls raises it once in BeginPlay and nothing lowers it —
		 * so a mode that raised it would be a mode that owned it, and the mode that did not would
		 * take it away again (SESSION_UI_DESIGN §d, S6).
		 */
		break;

	case EToolbarButtonId::ModeDestroy:
		/*
		 * NO GHOST SURVIVES INTO DESTROY MODE. A gold brick hanging in the air over a wall the
		 * player is demolishing is the most confusing thing this UI can do. The BUILD survives —
		 * CancelBuild is one call away and "leaving Build mode" reads like a reason to make it,
		 * which would hand the player a fresh empty plot every time they looked at a brick.
		 */
		if (BuildComponent != nullptr)
		{
			BuildComponent->HidePreview();
		}

		/* And the cursor stays where it is: the strip is on screen in Destroy mode too. */
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
		 * THE STATE'S FLAG, READ AFTER THE TRANSITION, exactly as the piece and the course are. The
		 * chip is a toggle and the model is where that flip already happened, so the component is
		 * told WHICH WAY the next piece lies rather than that a chip was clicked — and it derives
		 * the swapped footprint itself, which is what keeps the rotation and the palette one answer.
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
			BuildComponent->PlacementMode = SessionToolbarState.Placement;
		}
		break;

	case EToolbarButtonId::JointAuto:
	case EToolbarButtonId::JointMortar:
	case EToolbarButtonId::JointDry:
	case EToolbarButtonId::JointNail:
	case EToolbarButtonId::JointScrew:
	case EToolbarButtonId::JointBolt:
		/*
		 * THE STATE'S CHOICE, READ AFTER THE TRANSITION, exactly as the piece and the course are.
		 * The component is told WHICH joint, never which chip — and the profile is looked up at the
		 * door rather than here, so nothing between the strip and the placement holds a library
		 * address it could get wrong.
		 */
		if (BuildComponent != nullptr)
		{
			BuildComponent->JointChoice = SessionToolbarState.Joint;
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
		 * CLEAR IS "START AGAIN" RATHER THAN "STOP BUILDING", so a fresh plot is left open behind
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
		 * AND THE OVERLAY IS RECOMPUTED AGAINST WHAT IS LEFT. A settle breaks joints and releases
		 * pieces, so every band on screen is a reading of a structure that no longer exists — and a
		 * green brick over a gap is worse advice than none.
		 */
		RefreshLoadOverlay();
		break;

	case EToolbarButtonId::ToggleLoadOverlay:
		/*
		 * THE FLAG IS ALREADY THE TRANSITION'S; this is the world catching up with it, in both
		 * directions — the tint goes on, or it comes off every brick that was wearing one.
		 */
		RefreshLoadOverlay();
		break;
	}

	/*
	 * AND WHAT THERE IS TO COMMAND IS ASKED AGAIN, BECAUSE A COMMAND CHANGES IT. Clear leaves an
	 * empty plot, so the strip has to grey Clear and Run again on the way out of the very click that
	 * emptied it — a strip still offering them would be offering a command over nothing.
	 */
	RefreshSessionHasStructure();
	RefreshSessionToolbar();

	return true;
}

bool ADestructionGamePlayerController::ToggleSessionMode()
{
	using namespace DestructionSession;

	/*
	 * THE READ IS THE WHOLE FUNCTION, AND IT IS THE ONLY DECISION IN THE SESSION'S KEYBOARD. A
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
	 * AND THE SAME SHAPE FOR Snap/Free, WHERE THE DOOR EARNS ITS KEEP. Neither placement chip is
	 * on the Destroy strip, so this must be REFUSED there — written as "set the other value" it
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
		 * THE COMPONENT IS GIVEN A DIRECTION, NOT AN END POINT, and it intersects that with the
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
	 * THE POSE IS TAKEN AGAIN FROM THIS RAY BEFORE IT IS COMMITTED, rather than trusting whatever
	 * the last pointer move held. A preview predicts the commit only while the binding is unchanged,
	 * and the previous click changed it — so a click that reused a stale preview would lay the
	 * second brick at the pose the first one was going to take.
	 */
	BuildComponent->UpdatePreviewFromRay(StartCm, (EndCm - StartCm).GetSafeNormal());

	const FPieceRef Placed = BuildComponent->ConfirmPlace();

	/* A brick landing is what turns an empty plot into something Clear and Run can act on. */
	RefreshSessionHasStructure();
	RefreshSessionToolbar();

	/*
	 * AND A NEW BRICK IS A NEW LOAD PATH. The overlay survives a trip through Build mode, so a piece
	 * laid while it is on has to be given a band of its own — and the pieces it now stands on have to
	 * be read again, because that is the whole of what laying a brick does to a structure.
	 */
	RefreshLoadOverlay();

	return Placed.StructureId != INDEX_NONE && Placed.PieceIndex != INDEX_NONE;
}

TArray<FPieceMenuRow> ADestructionGamePlayerController::InspectAlongRay(
	const FVector& StartCm,
	const FVector& EndCm)
{
	TArray<FPieceMenuRow> Rows;

	/*
	 * WHICH BRICKS THE READOUT IS POINTING AT, BEFORE A CLICK MOVES ANYTHING. Toggling a brick
	 * out of the selection can stop another brick being singled out — so the readout empties and
	 * every colour it handed out has to come back, and none of those bricks is otherwise touched
	 * by anything below.
	 */
	const TArray<FPieceRef> WereNeighbours = NeighbourPieces();

	UDestructionStructureSubsystem* const Subsystem = PieceMenuSubsystemOf(*this);

	/*
	 * THE WHOLE CHAIN IS ALREADY WRITTEN AND NONE OF IT IS REPEATED HERE. TracePiece fails
	 * closed on every step from the trace to the re-resolve, so a miss arrives as a default
	 * ref; PieceActionsFor resolves every ref again against the binding and answers an empty
	 * menu for one that names nothing. This is the wire between them, not a third opinion.
	 */
	if (Subsystem != nullptr)
	{
		const FPieceHit Hit = Subsystem->TracePiece(StartCm, EndCm);

		/*
		 * A CLICK HAPPENS AT THE CURSOR, so this ray is also the answer to what is under it.
		 * Saying so here rather than waiting for the next mouse-move is what stops a brick
		 * staying lit after it has been clicked away from, or a cleared selection leaving
		 * the last brick pointed at still called out.
		 */
		SetHoveredPiece(Hit.Ref);

		/*
		 * CLICKING A BRICK TOGGLES IT, AND CLICKING PAST EVERYTHING CLEARS THE LOT. The
		 * selection is the durable state and the menu is a projection of it rebuilt below,
		 * which is why there is no branch here that shows or dismisses anything: an empty
		 * selection builds no rows, and an empty row list is already how a menu comes down.
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
		 * ONE MENU FOR THE WHOLE SELECTION, AGAINST THE STRUCTURE ITS REFS NAME. A selection
		 * is built by clicking one wall, so the first ref names it and PieceActionsFor
		 * refuses the rest piece by piece if it ever does not.
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
	 * EVERY ROUTE OUT OF HERE PRESENTS, INCLUDING THE ONES THAT FOUND NOTHING — no world, no
	 * subsystem, a ray that hit the floor, a brick standing for a piece that has gone. That is
	 * what makes "the ray hit nothing" and "take the menu down" the same call rather than two,
	 * and it is the whole reason this is one ShowPieceMenu at the end instead of an early
	 * return per guard: a route that simply returned would leave the previous brick's menu on
	 * screen naming a brick the player is no longer pointing at, and a Delete on it removes it.
	 */
	ShowPieceMenu(Rows);

	/* AFTER the menu has been shown, so the state this leaves the wall in is the final one. */
	RefreshNeighbourHighlights(WereNeighbours);

	return Rows;
}

FPieceRef ADestructionGamePlayerController::HoverAlongRay(
	const FVector& StartCm,
	const FVector& EndCm)
{
	UDestructionStructureSubsystem* const Subsystem = PieceMenuSubsystemOf(*this);

	/*
	 * POINTING AT A BRICK IS NOT CHOOSING IT, so nothing here touches the selection, opens
	 * a menu or closes one — the only thing that changes is which brick is called out. No
	 * world and no subsystem is the same answer as a ray that hit nothing: a default ref,
	 * which lets go of whatever was called out before.
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
	 * THE STRONGER STATE WINS WHERE THEY COINCIDE, AND THE ORDER IS Inspected > Selected >
	 * Hovered. A selected brick under the cursor stays Selected: a hover that overwrote it
	 * would make a chosen brick read as unchosen exactly when the player is looking at it,
	 * which is indistinguishable from having lost the selection. And the one brick whose
	 * joint forces are on screen beats the rest of the selection, because a breakout of one
	 * brick's numbers drawn beside five bricks that look identical to it is ambiguous about
	 * which brick it is the breakout OF.
	 *
	 * A BRICK THAT IS NOT SELECTED CANNOT BE THE ONE BEING READ, which is why this asks the
	 * selection as well as the ref. It is the same rule BuildPieceMenuInspector applies — an
	 * anchor outside the set it anchors is a readout of somebody else's brick — and it has to
	 * be the same rule, or the panel and the wall disagree about which brick the numbers are
	 * about.
	 *
	 * IT IS NOT A SUBSTITUTE FOR CLEARING THE REF, AND THAT DISTINCTION COST A ROUND. The
	 * conjunct makes a stale InspectedPiece inert only for as long as the brick is out of the
	 * selection; the ref itself survives, so picking that brick again springs the readout back
	 * open on it for no reason the player can see. DismissPieceMenu is where it is actually
	 * let go of, because the panel is the only thing that can ever single a brick out.
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
	 * THEN THE READOUT'S OWN COLOURS, WHICH SIT BETWEEN THE SELECTION AND THE CURSOR — AND BOTH
	 * SIDES OF THAT ARE JUDGEMENTS RATHER THAN DEDUCTIONS.
	 *
	 * SELECTED BEATS NEIGHBOUR. A picked brick that is also on the far end of a joint row keeps
	 * its selection colour, and the argument the other way is real: the neighbour hue is the only
	 * thing tying a row of numbers to a brick in a wall of identical bricks, so a picked neighbour
	 * weakens that tie. It loses to the rule this project has already stated three times — the one
	 * thing a player must be able to check before pressing Delete is which bricks are going, and a
	 * brick that quietly stops looking picked while the cursor runs down a list is that check being
	 * taken away at the worst possible moment. Deleting is irreversible; losing a hue is not, and
	 * the row still names the brick in words either way.
	 *
	 * NEIGHBOUR BEATS HOVERED, FOR THE OPPOSITE REASON. While the readout is open the cursor is on
	 * the PANEL, so HoveredPiece is whatever the last ray into the world happened to hit and is
	 * STALE BY CONSTRUCTION. A stale answer overwriting a live one would turn the brick a row is
	 * pointing at back to the hover colour the moment the player last looked at it.
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
	 * AND THE LOAD OVERLAY LAST, WHICH IS WHERE A STATE THAT COVERS EVERY PIECE AT ONCE HAS TO SIT.
	 *
	 * Everything above says "this one" — the brick being read, the bricks picked, the bricks a row
	 * points at, the brick under the cursor — and the overlay says something about all of them. An
	 * overlay that beat any of those would take away the reading the player is actually making,
	 * which for the selection is the one thing they must be able to check before pressing Delete.
	 *
	 * IT IS ASKED HERE RATHER THAN PAINTED ON, and that is the whole shape of the feature. A refresh
	 * that called SetHighlighted on every brick would be in a fight with the cursor it wins: the next
	 * refresh repaints over the hover and the selection, and the hover's own refresh would leave a
	 * brick plain forever afterwards. One function decides where states coincide; the overlay is one
	 * more question it asks, at the bottom of the order.
	 */
	return LoadHighlightForPiece(Ref);
}

EBrickHighlight ADestructionGamePlayerController::LoadHighlightForPiece(const FPieceRef& Ref) const
{
	/*
	 * THE STRUCTURE IS CHECKED AS WELL AS THE INDEX, for the reason NeighbourHighlightForPiece checks
	 * it: piece 4 of every wall on screen is not piece 4 of the one the overlay solved.
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
	 * THE ROWS BELONG TO THE INSPECTED BRICK, SO THE FAR END BELONGS TO ITS STRUCTURE. Comparing
	 * the piece index alone would light brick 4 of every wall on screen the moment brick 4 of this
	 * one became a neighbour, which is the same fail-open shape FStructureBinding::ResolvePiece
	 * refuses a foreign ref for.
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
	 * THE OLD SET FIRST AND THE NEW SET SECOND, WHICH MATTERS FOR THE BRICKS IN BOTH. A brick
	 * that is a neighbour before and after may have changed SLOT, and refreshing it twice is
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
	 * THE BRICK BEING LEFT IS REFRESHED AS WELL AS THE ONE BEING POINTED AT, and it is
	 * refreshed rather than simply cleared — it may be selected, in which case it stays
	 * called out. Without the first of the two, every brick the cursor has ever crossed
	 * stays lit and the wall ends up entirely highlighted.
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
	 * AND THE READOUT'S OWN BRICKS, WHICH ARE NOT IN THAT LIST. A brick that leaves the selection
	 * stops being singled out, so the whole neighbour set goes with it — and those bricks are
	 * precisely the ones that were never picked, so the loop below would not reach them. Asked
	 * before the clear, because afterwards there is no readout left to ask.
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
	 * THE BRICK BEING LEFT IS REFRESHED AS WELL AS THE ONE BEING TAKEN UP, AND IT IS
	 * REFRESHED RATHER THAN CLEARED — it is almost always still selected, so it goes back to
	 * Selected rather than to None. This is exactly the bug class already recorded against
	 * SetHoveredPiece, and here it is sharper in both directions: a brick left Inspected means
	 * two bricks claim the one breakout, and a brick dropped to None means running the cursor
	 * down the menu silently empties the selection on screen while the commit still deletes
	 * every one of them.
	 *
	 * NOTHING ELSE MOVES. Reading a brick is not choosing it, exactly as pointing at one is
	 * not: the selection and the presented rows come through untouched, or hovering down a
	 * list of six entries would rewrite the very list being hovered.
	 */
	const FPieceRef Previous = InspectedPiece;

	/*
	 * AND THE BRICKS THE READOUT WAS POINTING AT ARE COLLECTED BEFORE THE REF MOVES, BECAUSE
	 * AFTERWARDS THERE IS NOTHING LEFT TO ASK. The neighbour set changes wholesale when the
	 * readout does, so a refresh that told only the NEW neighbours would leave the old ones lit
	 * and running the cursor down a list of six entries would colour the whole wall — the same
	 * left-behind-state bug this function's own Previous ref exists to close, one field out.
	 */
	const TArray<FPieceRef> WereNeighbours = NeighbourPieces();

	InspectedPiece = Ref;

	RefreshPieceHighlight(Previous);
	RefreshPieceHighlight(Ref);

	RefreshNeighbourHighlights(WereNeighbours);

	/*
	 * AND THE READOUT FOLLOWS THE BRICK IT DESCRIBES. The panel breaks out ONE brick's joints,
	 * so which brick that is changing is the whole of what a player asked for by running the
	 * cursor down the list.
	 */
	RefreshPieceMenuInspectorWidget();
}

bool ADestructionGamePlayerController::ShowPieceMenu(TArrayView<const FPieceMenuRow> Rows)
{
	/*
	 * SHOWING IS DEFINED AS DISMISSING AND THEN BUILDING, WHICH IS THE POINT RATHER THAN AN
	 * IMPLEMENTATION DETAIL. There is exactly one route out of "a menu is up", so replacing a
	 * menu, showing an empty one and closing one outright all take it — which is what makes the
	 * inspected brick let go of on every one of them without three copies of that. It is also
	 * what keeps the widget half honest, which is why the build below sits here and the removal
	 * sits beside the Reset in DismissPieceMenu: a second add with no matching remove leaks the
	 * previous menu on screen forever and no headless assertion can see that, but the
	 * model-level version of the same bug — holding two menus' rows — is asserted, and the two
	 * are only the same code path while show is written this way.
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
	 * AND THE CONTROLS ARE NOT TOUCHED. A menu used to raise the cursor and remove the free-look
	 * context here, and give both back on the way out; the cursor is the session's now and the
	 * camera is chorded to a held right mouse button, so there is nothing to take away and nothing
	 * a route out of "a menu is up" could forget to restore (SESSION_UI_DESIGN §d, S6).
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
	 * AND NOTHING IS BEING READ OUT ANY MORE, WHICH IS A CLEAR RATHER THAN A DISABLE. The only
	 * thing that ever singles a brick out is the cursor resting on an entry row of this panel,
	 * and Slate delivers no OnMouseLeave to a widget that has left the tree — so a panel taken
	 * down under the cursor left InspectedPiece set. HighlightForPiece made that ref merely
	 * INERT, by also asking whether the brick is still in the selection, and inert is not
	 * cleared: deselecting the brick and picking it again brought the readout straight back on
	 * it, joint breakout and all, with the player's cursor nowhere near the menu.
	 *
	 * It goes through SetInspectedPiece rather than assigning the field, because the brick being
	 * let go of has to be told: it is almost always still selected, so it drops back to Selected
	 * rather than being left wearing the readout's own colour. And it sits AFTER
	 * RemovePieceMenuWidget so the readout refresh it triggers finds no box and does nothing —
	 * there is no panel left to draw into by this point.
	 */
	SetInspectedPiece(FPieceRef());

	return true;
}

bool ADestructionGamePlayerController::IsPieceMenuShown() const
{
	/*
	 * THE ROWS ARE THE RECORD, AND THERE IS NO SECOND FLAG. An empty list dismisses, so
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
	 * AN INDEX THAT NAMES NO ROW COMMITS NOTHING, AND IT IS REFUSED RATHER THAN CLAMPED. A
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
	 * THE CHOSEN ROW IS COPIED OUT BEFORE THE DISMISS, and that is load-bearing rather than
	 * tidy: DismissPieceMenu Reset()s the very array the rows live in, so a reference into
	 * ShownPieceMenuRows would be reading destroyed elements by the time it was committed.
	 * Both halves come from the ROW rather than from anything this controller remembered
	 * separately — Core/PieceMenu.h says why the row carries its own targets, and a
	 * presenter that committed the chosen row's action against a remembered selection would
	 * act on the wrong bricks with everything else looking perfect.
	 */
	const TArray<FPieceRef> Refs = ShownPieceMenuRows[RowIndex].Refs;
	const FPieceAction* const Action = ShownPieceMenuRows[RowIndex].Action;

	/*
	 * IT COMES DOWN FIRST, BY THE ONE ROUTE OUT OF "A MENU IS UP" — so the brick it was
	 * reading out is let go of by the same call every other route makes, and the commit below
	 * runs with nothing on screen naming the bricks it is about to remove.
	 */
	DismissPieceMenu();

	/*
	 * AND THE PICK GOES WITH IT, BEFORE THE COMMIT RATHER THAN AFTER. These bricks have just
	 * been acted on, so leaving them selected would carry them into the next click's menu —
	 * where they no longer resolve, and the intersection then offers nothing at all. Before,
	 * because the commit is what destroys them, and a brick has to still exist to be told it
	 * is no longer called out.
	 */
	ClearPieceSelection();

	UDestructionStructureSubsystem* const Subsystem = PieceMenuSubsystemOf(*this);

	if (Subsystem == nullptr)
	{
		return false;
	}

	/*
	 * ONE COMMIT FOR THE WHOLE SELECTION, WHICH IS WHAT MAKES IT ONE SOLVE. Looping the
	 * single-piece commit here would reach the same wall at N times the price — and would
	 * push N times, each against an answer that had seen only part of the batch.
	 */
	const bool bCommitted = Subsystem->CommitPieceActionForAll(Refs, *Action) > 0;

	/*
	 * AND THE SESSION IS ASKED AGAIN WHETHER THERE IS ANYTHING LEFT TO COMMAND.
	 *
	 * THIS IS THE ONLY DOOR THAT CHANGES THE WORLD WITHOUT BEING A TOOLBAR CLICK. OnToolbarButton
	 * refreshes `bHasStructure` at its own door, so every route through the strip keeps the chips
	 * honest; a delete arrives here instead, and without this the state — which is what the strip
	 * on screen is drawn from — goes on carrying the answer from the click that LAID the brick.
	 * The player would be offered Run and Clear over an empty plot until they happened to press
	 * something else, and pressing Run would solve an empty graph and report success.
	 *
	 * ONLY WHEN SOMETHING ACTUALLY COMMITTED, because a refused action changed nothing and a
	 * refresh is a solve-free question with a widget rebuild behind it.
	 */
	if (bCommitted)
	{
		RefreshSessionHasStructure();
		RefreshSessionToolbar();

		/*
		 * AND THE OVERLAY IS RECOMPUTED, WHICH IS THE CLAIM THE WHOLE FEATURE IS FOR. "See where the
		 * load is, then pull THAT one" is worth nothing if the picture does not move when the player
		 * pulls: a refresh that ran only on the toggle would leave the wall coloured by the structure
		 * as it stood before the delete. It costs nothing while the overlay is off.
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
	 * NO VIEWPORT MEANS NO WIDGET, AND THAT IS THE ORDINARY CASE IN A TEST rather than an
	 * error: a world built in code has no UGameViewportClient at all, so the presented rows —
	 * which are the record, not this — stand alone and everything asserted about a menu still
	 * holds with nothing drawn.
	 */
	if (Viewport == nullptr)
	{
		return;
	}

	/*
	 * THE PANEL TAKES ITS HOME THE FIRST TIME IT IS SHOWN, AND NEVER AGAIN.
	 *
	 * ONCE, BECAUSE THE MENU IS REBUILT ON EVERY CLICK. PieceMenuPanelOffsetPx is controller state
	 * precisely so a corner the player chose survives the panel being torn down and put back; a home
	 * taken on every build would overwrite it, and the panel would snap back across the screen the
	 * next time they picked a brick — which is the complaint this whole seam answers, arrived at
	 * from the other direction.
	 *
	 * WHERE it opens is Core's decision and not this function's, for the reason ClampPanelOffset is:
	 * it is arithmetic on two sizes and a margin, the failure is a panel that opens somewhere
	 * unusable, and a widget cannot be asked whether it got it right. Presenter.PanelHomeOffset holds
	 * it, including the rows where the answer is the origin — and it composes with the clamp rather
	 * than restating it, so nothing here has to clamp what it hands back.
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
	 * THE SCREEN, IN THE UNITS THE PANEL'S OFFSET IS STATED IN, BEFORE THERE IS A PANEL TO ASK.
	 *
	 * PieceMenuViewportSizePx READS THE PANEL'S OWN LAID-OUT ROOT AND IS THE RIGHT ANSWER, and it is
	 * not available yet: a widget built this frame has no cached geometry, so a home worked out from
	 * it would be the origin for the whole of the first menu — which is exactly the corner nobody
	 * chose that this is here to stop. The viewport client knows its size before anything is laid
	 * out.
	 *
	 * IN SCREEN PIXELS, WHICH IS THE WRONG UNIT BY EXACTLY THE DPI SCALE. The constraint canvas is
	 * viewport content and so lays out under Slate's scaler; a home measured in screen pixels and
	 * applied in scaled ones would open the panel past the right edge by that factor, and the clamp
	 * would then quietly pull it back — a fault that is invisible rather than absent, and the same
	 * one PieceMenuViewportSizePx's own comment names. Dividing by the scale is the whole conversion.
	 *
	 * A SCALE THAT IS NOT A POSITIVE NUMBER FAILS TO NO SCREEN AT ALL, AND THE GUARD IS WRITTEN
	 * `!(X > 0)` SO A NaN LANDS INSIDE IT. A viewport of no size puts the home at the origin, which
	 * is the corner that is on screen at every size and every scale.
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
	 * THE READOUT IS ASKED FOR ONCE AND EVERY STRING IN IT IS TAKEN AS GIVEN. Nothing below
	 * counts, formats, pluralises, filters or resolves anything — Core/PieceMenu.h says at
	 * length why each of those decisions is already made in the model, and the short version
	 * is that this function is the one place no test can reach.
	 *
	 * AND THE DETAIL IS THE PANEL'S OWN RATHER THAN THE ACCESSOR'S DEFAULT, which is the one
	 * argument on this line and the one place the compact request is allowed to reach. The joint
	 * table is also the index NeighbourHighlightForPiece colours the wall from, so a compact
	 * answer handed to the shared accessor would darken every neighbour highlight along with the
	 * table — see PieceMenuInspectorForSelection.
	 */
	const FPieceMenuInspector Inspector = PieceMenuInspectorForSelection(PieceMenuPanelDetail);

	TSharedRef<SVerticalBox> Panel = SNew(SVerticalBox);

	/*
	 * THE HEADING AND THE COUNT SHARE A ROW, because they are one sentence about one thing and a
	 * count wrapped onto its own line spends a row of a fixed panel on nothing. Both strings are
	 * the model's; which of them is bold is the only thing decided here.
	 *
	 * AND THAT ROW IS THE HANDLE THE PANEL IS MOVED BY. A title bar is where every desktop already
	 * puts one, it is the widest thing on the panel that holds nothing clickable, and it is the one
	 * strip that is there in every state — a grab affordance that vanished when nothing was
	 * selected would strand a panel with nothing in it.
	 *
	 * THE BORDER TAKES NO PADDING OF ITS OWN, WHICH IS LOAD-BEARING RATHER THAN TIDY. Six layout
	 * tests measure where the rows below this one land; a border that inset its content would push
	 * every one of them down by however much it took, so the strip is a background and four event
	 * bindings and changes no geometry at all. The grab cursor is what says it can be dragged,
	 * because a word saying so would be a word chosen in the one place no test can read it.
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
	 * ONE ROW PER SELECTED BRICK, AND HOVERING ONE IS WHAT SINGLES IT OUT. A button is used
	 * for the hover events rather than for a click: an entry names a brick, and naming one is
	 * not choosing to do anything to it, so it carries no OnClicked at all.
	 *
	 * AND EVERY ROW SAYS WHY ITS BRICK IS STANDING UP, IN THE MODEL'S OWN WORD. Eleven picked
	 * bricks were eleven identical strings until now, so finding the falling one meant hovering
	 * each of them in turn while FInspectorPieceEntry::SupportText held the answer for all of
	 * them at once.
	 *
	 * THE WORD SITS BESIDE THE BUTTON RATHER THAN INSIDE IT, WHICH IS NOT COSMETIC. Four layout
	 * tests find an entry row by the text under its button and match it against the model's
	 * Label, so a second text block in there would rename every row to "course 2 · #1supported"
	 * and take those assertions with it. The button still fills the row, so the whole width of it
	 * is hover target.
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
					 * NOT FOCUSABLE, FOR THE SAME REASON THE TOOLBAR'S CHIPS ARE NOT. Slate gives user
					 * focus to a focusable widget on click and SButton::OnKeyDown then handles Enter
					 * and Space itself, so clicking a brick row costs the player Run (Enter) and the
					 * pawn's jump (Space) until they click the viewport again — with the cursor now
					 * permanent this menu is one clickable surface among several, so that stolen focus
					 * outlives whatever they opened it for.
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
	 * AND THE LIST IS CAPPED IN HEIGHT WITH THE SCROLLING INSIDE IT. Forty picked bricks is an
	 * ordinary selection in this game, and a list that grew with it would run the action rows off
	 * the bottom of the screen — measured at y 1005 in a 1080 viewport, i.e. unreachable. A
	 * scroll box in a box that will not exceed a stated height cannot do that whatever it holds.
	 *
	 * THE CAP DOES NOT GIVE UP THE PANEL'S FIXED SIZE, WHICH IS THE PROPERTY EVERY STILLNESS CLAIM
	 * RESTS ON. The readout below is a fill slot, so it absorbs exactly what a short list leaves,
	 * and the panel's own height is overridden outright — three bricks and forty-five measure the
	 * same 560 px and lay the action rows from the same bottom edge.
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
	 * THE JOINT BREAKOUT GETS THE SPACE THAT IS LEFT, AND IT GETS THE SAME SPACE WHATEVER IS IN
	 * IT. A fill slot's height comes from the panel's own size minus the auto-height rows above
	 * and below it, so the readout's CONTENT cannot move anything: not the entry rows over it,
	 * not the action rows under it, and not the panel. That is what retired both halves of the
	 * old geometry workaround — the readout no longer has to be the last slot, and the top anchor
	 * is no longer load-bearing.
	 *
	 * IT IS STILL A BOX WHOSE CONTENT IS SWAPPED RATHER THAN A PANEL REBUILT, AND THAT PART DOES
	 * NOT RELAX. Rebuilding on hover destroys the very button the cursor is on, so Slate fires
	 * OnHovered on its replacement next frame and again on the one after that.
	 */
	Panel->AddSlot()
		.FillHeight(1.0f)
		.Padding(0.0f, 8.0f, 0.0f, 8.0f)
		[
			SAssignNew(PieceMenuInspectorBox, SBox)
		];

	/*
	 * AND THE DESTRUCTIVE ROW IS LAST, BEHIND A RULE. Releasing a brick is irreversible here, and
	 * the standing rule is that the commit door is never wider than the menu door; a button
	 * reached by reading PAST everything that describes what it will destroy is that rule stated
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
	 * AND A ROW THAT DESTROYS SOMETHING LOOKS LIKE ONE, AND SAYS HOW MUCH OF IT.
	 *
	 * BOTH FACTS ARE THE MODEL'S. bIsDestructive is the ACTION'S own flag carried across by the
	 * presenter, so the colour below is keyed on data rather than on a caption compared against
	 * the word "Delete"; TargetText is derived from the very refs the row commits against, so a
	 * button cannot promise to act on a different number of bricks than it will.
	 *
	 * THE COUNT IS OVERLAID ON THE BUTTON RATHER THAN SET AS PART OF IT, AND BOTH HALVES OF THAT
	 * ARE LOAD-BEARING. Four layout tests find an action row by the text under its button and
	 * match it against the model's Label, so a second text block INSIDE it renames the row they
	 * are looking for to "Delete3 bricks" — and a second SLOT beside it narrows the button, which
	 * is the span World.Menu.TheReadoutFitsInsideThePanel measures the whole panel's content
	 * column by. An overlay leaves the button full width and the caption alone, and the count is
	 * HitTestInvisible so a click on it still lands on the button underneath rather than dying
	 * quietly two pixels from the thing the player aimed at.
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
					 * NOT FOCUSABLE, AND THIS SITE NEEDS ITS OWN SAY BECAUSE IT IS A SECOND ONE. An
					 * action row is built here and an entry row is built above, so the fix applied to
					 * one leaves the other taking the keyboard: a focused SButton's OnKeyDown handles
					 * Enter and Space, which in this session are Run and the pawn's jump, and the only
					 * way back is a click on the viewport.
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
	 * A FIXED SIZE ON A REAL BACKGROUND, PLACED WHEREVER THE PLAYER LAST PUT IT.
	 *
	 * THE SIZE IS THE MECHANISM AND THE BACKGROUND IS THE DEFECT NO TEST COULD SEE. A panel that
	 * cannot change size cannot move a row out from under a cursor, whatever it is anchored to
	 * and whatever order its slots are in — which is strictly stronger than the top anchor and
	 * the last-slot readout it replaces, and it is asserted directly rather than argued. And
	 * every line under the brick rows used to be a bare text block over the sky: legible against
	 * a wall, invisible against anything bright, and unreachable by a headless suite that paints
	 * no pixels. A near-opaque fill behind the whole panel is what makes the readout readable.
	 *
	 * A POSITION RATHER THAN AN ALIGNMENT, WHICH IS WHAT THE PLAYER ASKED FOR. This used to be an
	 * SBox pinned to the right edge and centred down it — a placement nobody could argue with, and
	 * being unable to argue with it is the complaint. A constraint canvas takes the corner as a
	 * value, so the same tree draws wherever PieceMenuPanelOffsetPx says, and a drag is then a new
	 * value rather than a new layout.
	 *
	 * ANCHORED AND ALIGNED TO THE TOP-LEFT SO THE OFFSET MEANS WHAT ClampPanelOffset SAYS IT MEANS.
	 * The clamp reasons about the panel's top-left corner in viewport pixels and about nothing
	 * else; an anchor anywhere but the origin would make the stored number a distance from
	 * somewhere the clamp has never heard of, which is the correct-layer-joined-wrongly defect this
	 * codebase keeps paying for. AutoSize takes the size from the child, so the panel's dimensions
	 * are stated once, on the box that overrides them, instead of again in the slot's margin.
	 *
	 * AND THE SIZE IS THE PRESENTER'S ANSWER FOR THIS MODE, NOT A CONSTANT BESIDE THIS SLATE. The
	 * override is a DESIRED size and a filling slot would hand its child whatever width it liked
	 * regardless, so the join is measured rather than argued: World.Menu.TheReadoutFitsInsideThe-
	 * Panel arranges this tree in both modes and reads the rectangle back.
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
	 * THE CORNER, AND NOTHING ELSE. The slot is AutoSize, so the last two components of the margin
	 * are the size the canvas ignores in favour of the child's own — stating the panel's dimensions
	 * here as well would be the second copy that eventually disagrees with the first.
	 */
	return FMargin(PieceMenuPanelOffsetPx.X, PieceMenuPanelOffsetPx.Y, 0.0f, 0.0f);
}

FVector2D ADestructionGamePlayerController::PieceMenuViewportSizePx() const
{
	/*
	 * THE ROOT OF THE PANEL IS THE VIEWPORT, so its own local size is the screen in the units the
	 * offset above is stated in — see the header for why UGameViewportClient::GetViewportSize is
	 * the wrong answer by exactly the DPI scale. Zero with no panel up clamps every offset to the
	 * origin, which is the fail-closed corner rather than a case needing its own handling.
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
	 * WHERE BOTH THINGS WERE WHEN THE PRESS LANDED, AND THE CAPTURE THAT KEEPS THEM COMING. Slate
	 * stops sending moves the moment the pointer leaves a widget, so without the capture a drag
	 * faster than the strip is wide drops the panel wherever the cursor crossed the edge.
	 */
	/*
	 * THE CORNER IS RE-CLAMPED AS IT IS PICKED UP, WHICH IS WHAT SURVIVES A VIEWPORT RESIZE. A
	 * corner that was inside a 1920 px screen is outside a 1280 px one, and nothing tells this
	 * class the window changed — so the stored value is held against the screen as it is NOW
	 * before a drag is measured from it. Clamping is idempotent, which is what makes doing this on
	 * every press free: an offset already in range comes back untouched.
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
	 * A MOVE THAT IS NOT A DRAG IS SOMEBODY'S CURSOR CROSSING THE STRIP. Slate sends moves whether
	 * or not a button is down, and a panel that followed the pointer without being picked up would
	 * be unusable rather than draggable.
	 */
	if (!bPieceMenuPanelIsHeld)
	{
		return FReply::Unhandled();
	}

	/*
	 * THE DRAG IS MEASURED FROM THE PRESS RATHER THAN FROM THE LAST FRAME, so the corner tracks the
	 * cursor exactly instead of accumulating a rounding per move — and so that dragging into a
	 * corner and back out returns to where it started rather than to wherever the clamp pinned it
	 * on the way through. AbsoluteToLocal on both ends puts the delta in the canvas's own units
	 * whatever the DPI scale; the translation cancels in the subtraction.
	 */
	const FVector2D DraggedToPx = PieceMenuPanelGrabbedFromPx
		+ FVector2D(Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition()))
		- PieceMenuCursorGrabbedAtPx;

	/*
	 * AND WHERE THAT IS ALLOWED TO LEAVE THE PANEL IS Core'S DECISION, NOT THIS FUNCTION'S. It is
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
	 * THE PANEL'S MODE ONLY, WHICH IS THE WHOLE CARE HERE. NeighbourHighlightForPiece and
	 * NeighbourPieces read the joint table to colour bricks in the wall, and they ask
	 * PieceMenuInspectorForSelection for themselves with no argument — so rolling the readout up
	 * cannot take the neighbour colours with it. See the header.
	 *
	 * THE WHOLE PANEL IS REBUILT RATHER THAN THE READOUT SWAPPED, because a mode change moves the
	 * action rows: the readout is a fill slot, so what is in it decides nothing, but the mode is
	 * read while the heading, the brick list and every row is composed. It is safe to rebuild here
	 * for the reason RefreshPieceMenuInspectorWidget is not — the cursor is on the title strip, not
	 * on an entry button, so nothing being destroyed can fire a hover at its own replacement.
	 */
	PieceMenuPanelDetail = PieceMenuPanelDetail == EPieceMenuDetail::Compact
		? EPieceMenuDetail::Full
		: EPieceMenuDetail::Compact;

	/*
	 * AND THE DRAG IS OVER, WHICH THE DOUBLE-CLICK'S OWN PRESS TURNED ON. Slate sends a press
	 * before a double-click, so the strip is holding a grab that will never see a release once the
	 * widget under the cursor is torn down and replaced.
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
	 * BOTH HANDLES LIVE INSIDE THE PANEL, SO THEY GO WITH IT AND NEVER OUTLIVE IT. The grab strip
	 * is released beside the readout box for the same reason it was taken beside it: it is a
	 * pointer into the tree rather than a second viewport widget, and a stale one would keep a torn
	 * down panel alive to be captured to.
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
	 * THE READOUT NAMES THE BRICK IT IS ABOUT, OR SAYS WHY IT HAS NOTHING TO SAY — AND THE TWO
	 * SHARE A ROW BECAUSE THE MODEL GUARANTEES AT MOST ONE OF THEM IS THERE.
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
	 * AND WHAT THAT BRICK IS, DIRECTLY UNDER ITS NAME, BECAUSE THE TWO ARE ONE THOUGHT — which
	 * brick, and what is it. The identity line below the joint table would be the brick's weight
	 * printed where a player who has read the heading has already stopped looking.
	 *
	 * NO EMPTINESS CHECK, EXACTLY AS THE ROW ABOVE HAS NONE. The model leaves IdentityText empty
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
	 * THE SUPPORT WORD AND THE JOINT LIST'S SENTENCE, WHICH IS THERE WHETHER OR NOT THERE ARE
	 * ANY JOINTS. That is why there is no emptiness check here: an isolated grounded pad reads
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
	 * ONE ROW PER JOINT: ITS SWATCH, ITS BAR, THEN ITS LINE. The bar goes before the words so the
	 * bars form a column the decade scale below can be read against — a log axis with the ticks
	 * nowhere near the fills is the same as no ticks at all — and the swatch goes before the bar
	 * so the colours form a column of their own down the left edge of the readout, which is where
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
	 * THE SCALE THE BARS ABOVE ARE READ AGAINST, AND THE LEFT ALIGNMENT IS LOAD-BEARING.
	 *
	 * WidthOverride states a DESIRED width, not an arranged one. A vertical box's slot fills by
	 * default, so the scale's box was being stretched to the readout's full width while the bars
	 * above kept their auto-width 96 px — and a canvas five times as wide as the bars does not
	 * merely clip its end labels, it annotates nothing: the 10x tick stood where no fill could ever
	 * reach, so every bar read as far emptier than it was. Aligning the slot left hands the box the
	 * width it asked for, which is the bar's, so the tick strip and the column of bars are one span.
	 *
	 * AND THE LEFT PADDING IS THE SWATCH COLUMN, FOR THE SAME REASON. The bars start one swatch and
	 * one gap in from the readout's edge, so a scale flush with that edge would stand one swatch to
	 * the left of everything it labels — the same "annotates nothing" defect the alignment above
	 * closed, wearing a smaller coat.
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
	 * AND THE WHOLE READOUT SCROLLS INSIDE THE SPACE IT WAS GIVEN. A brick with more joints than
	 * fit would otherwise run its last lines out past the rule and under the row that deletes it
	 * — which is the same hazard the brick list's cap closes, one region down.
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
	 * ONE READOUT FOR THE WHOLE SELECTION, AGAINST THE STRUCTURE ITS REFS NAME — the same
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
	 * NO BINDING IS AN EMPTY BINDING RATHER THAN AN EARLY RETURN, AND THAT IS WHAT KEEPS THE
	 * MODEL THE ONLY AUTHOR OF THE READOUT. A default-constructed FPieceMenuInspector is not the
	 * same object BuildPieceMenuInspector answers for the same inputs: nothing picked has its own
	 * sentence, "No bricks selected", decided in the model precisely so that no widget has to
	 * choose one — and a returned default carries an empty CountText instead, which draws as a
	 * blank line where a sentence belongs. Handing over an empty structure says the same thing by
	 * the one route that words it, and it makes SelectedCount answer the selection's own size on
	 * the fail-closed paths too, which is the promise Core/PieceMenu.h makes for it.
	 */
	const FStructureBinding NoStructure;

	return BuildPieceMenuInspector(
		Binding != nullptr ? *Binding : NoStructure, Selected, InspectedPiece, Detail);
}

void ADestructionGamePlayerController::SetPieceMenuDetail(EPieceMenuDetail Detail)
{
	/* A FIELD, AND NOTHING ELSE. See the header: this is a seam, not a behaviour. */
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
	/* A default ref singles out nothing, which is how the cursor leaving the list is said. */
	SetInspectedPiece(FPieceRef());
}

TSharedRef<SWidget> ADestructionGamePlayerController::BuildSessionToolbarPanel()
{
	using namespace DestructionSession;

	/*
	 * THE STRIP IS THE MODEL'S LIST, DRAWN IN ITS OWN ORDER, AND NOTHING BELOW DECIDES ANYTHING
	 * ELSE. Which buttons exist in this mode, what each reads, which one is lit and which are greyed
	 * are all SessionToolbarButtons' answers — Core/SessionToolbar.h says at length why a strip of
	 * buttons spelled as a run of AddSlot calls is a list of decisions in the one place no test can
	 * reach. What is left here is the chip: a size, two fonts and two fills.
	 */
	const TArray<FToolbarButton> Buttons = SessionToolbarButtons(SessionToolbarState);

	/*
	 * THE STYLES ARE REBUILT HERE, BEFORE A SINGLE CHIP IS MADE, BECAUSE THE LOOK FOLLOWS THE STATE.
	 * Every chip's fill, edge and caption weight is ChipLookFor's answer for the state the strip is
	 * being drawn for, and this function is called afresh on every click that changes it — so the
	 * styles are written in place first and the chips are then pointed at them.
	 */
	RebuildSessionChipStyles(Buttons);

	TSharedRef<SHorizontalBox> Strip = SNew(SHorizontalBox);

	for (int32 Index = 0; Index < Buttons.Num(); ++Index)
	{
		const FToolbarButton& Button = Buttons[Index];

		/*
		 * A HAIRLINE WHERE THE REGION CHANGES, AND NOWHERE ELSE.
		 *
		 * COMPARED AGAINST THE NEIGHBOUR RATHER THAN COUNTED OUT IN SLOTS, which is the whole reason
		 * EToolbarGroup is on the row: the model says the three regions are contiguous and in order,
		 * so "the group changed" is all this needs to know, and a strip whose buttons are retuned
		 * keeps its rules without anything here being touched.
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
		 * A CHIP'S CONTENT IS ITS SWATCH AND ITS CAPTION, and the swatch comes FIRST. §e puts it "in
		 * place of a size caption" on the piece chips, and a block of brick red drawn after the word
		 * would read as a status light rather than as the thing about to be laid.
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
					 * NOT FOCUSABLE, AND IT IS THE ONE LINE ON THIS CHIP THAT IS NOT COSMETIC. A
					 * focusable SButton takes user focus when it is clicked, and the flying pawn
					 * then stops answering W — a player reports that as the game freezing, and
					 * nothing but a headless arrange of this tree can see it.
					 *
					 * THE STYLE IS A POINTER INTO THE CONTROLLER'S OWN STORAGE, and it has to be:
					 * SButton keeps what it is given and never copies it. There is no tint on the
					 * button any more — the fill IS the style's brush, because a colour multiplied
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
		 * THE COURSE READS OUT BETWEEN ITS OWN TWO ARROWS, AND IT IS NOT A CHIP.
		 *
		 * ARROW, VALUE, ARROW IS WHAT A STEPPER IS — a value tacked onto the end of the strip would
		 * be a different control, and the two arrows would go on reading as acting on nothing. So it
		 * is hung off the DOWN arrow's own slot rather than off the mode, which also makes it
		 * Build-only for free: SessionToolbarButtons draws the course pair in Build mode and nowhere
		 * else, and there is no build plane in Destroy mode for a number to be about.
		 *
		 * AND A TEXT SLOT RATHER THAN AN ELEVENTH BUTTON, because the model has no row for it: a
		 * chip here would be a lit control that does nothing when it is pressed, and would put the
		 * drawn strip out of step with the list that is supposed to be its single source of truth.
		 * The wording is CourseLabel's, which Core.SessionToolbar.* already owns.
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
	 * A BAR ACROSS THE BOTTOM, AND THE REST OF THE SCREEN IS NOT THE TOOLBAR'S.
	 *
	 * The widget the viewport is handed fills the viewport, so everything above the bar is a fill
	 * slot holding nothing — and the root is SelfHitTestInvisible so that the empty part of it does
	 * not swallow the click the player is aiming at a brick.
	 *
	 * HIT-TESTABLE IS NOT ENOUGH, AND BELIEVING IT WAS IS THE DEFECT. Slate routes a press to a
	 * hit-testable widget and then, finding NOTHING BOUND, bubbles it on — to the SViewport, into
	 * the input stack, into IA_InspectPiece, and in Build mode into PrimaryAlongRay. So missing a
	 * chip by three pixels LAYS A BRICK where that pixel's ray meets the build plane. The two
	 * handlers below are what actually stops the fall-through: being routed to is the precondition,
	 * answering Handled is the act.
	 *
	 * THE RELEASE TOO, BECAUSE A SWALLOWED PRESS WITH A LEAKED RELEASE IS HALF A CLICK. Enhanced
	 * Input reads key-up as well as key-down, so a bar that ate only the press would deliver the end
	 * of a gesture to the world with nothing having started it.
	 *
	 * AND THE LEFT BUTTON ONLY. The right button is the look chord (SESSION_UI_DESIGN §d, S6):
	 * held-RMB turns the camera, and a bar that swallowed it would make the strip a dead patch a
	 * player cannot drag their view across.
	 *
	 * THE MIDDLE THIRD OF THE SCREEN IS WHERE THE WALL IS (SESSION_UI_DESIGN §a): the strip is at
	 * the bottom edge and the details window homes against the right, so neither is ever over the
	 * thing the player is pointing at.
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
	 * THE LAST SLOT IS THE ONE NOBODY'S BUTTON OWNS, and an id outside the enumeration lands there
	 * rather than on somebody else's chip. EToolbarButtonId is a uint8 and a cast is all it takes to
	 * make one; RebuildSessionChipStyles fills that slot with the GREYED look, so an undeclared
	 * button reads as one that cannot be clicked instead of borrowing a live chip's amber.
	 */
	const bool bKnown = Index >= 0 && Index < SessionChipStyleCount - 1;

	return SessionChipStyles[bKnown ? Index : SessionChipStyleCount - 1];
}

void ADestructionGamePlayerController::RebuildSessionChipStyles(
	const TArray<DestructionSession::FToolbarButton>& Buttons)
{
	using namespace DestructionSession;

	/*
	 * EVERY SLOT IS WRITTEN, STARTING FROM THE GREYED LOOK.
	 *
	 * A DEFAULT FToolbarButton IS bEnabled == false, so this IS the greyed answer rather than a
	 * second spelling of it — which is what the unknown slot needs, and what a button that is not on
	 * THIS mode's strip should keep so that a stale amber cannot survive a mode switch.
	 */
	const FButtonStyle GreyedStyle =
		SessionToolbarChipStyle(ChipLookFor(FToolbarButton(), SessionToolbarState.Mode));

	for (FButtonStyle& Style : SessionChipStyles)
	{
		Style = GreyedStyle;
	}

	/*
	 * THEN THE STRIP'S OWN, IN PLACE. The array's slots do not move, so a chip already on screen
	 * holding a pointer into one of them goes on reading a valid style — it simply starts reading the
	 * new look, which is what a rebuilt strip wants.
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
	 * NO VIEWPORT MEANS NO STRIP, AND THAT IS THE ORDINARY CASE IN A TEST rather than an error — a
	 * world built in code has no UGameViewportClient at all. The session state is the record and it
	 * stands alone, exactly as the presented rows do for the piece menu.
	 */
	if (Viewport == nullptr)
	{
		return;
	}

	/*
	 * REMOVE THEN ADD, WHICH IS WHAT KEEPS THE ADDS AND THE REMOVES PAIRED. This is also the redraw
	 * path, so a second add with no matching remove — the one leak that would go unseen — is on the
	 * same code path as the first show rather than on a branch of its own.
	 */
	RemoveSessionToolbarWidget();

	SessionToolbarWidget = BuildSessionToolbarPanel();

	Viewport->AddViewportWidgetContent(SessionToolbarWidget.ToSharedRef());
}

void ADestructionGamePlayerController::RefreshSessionToolbar()
{
	/*
	 * NOTHING ON SCREEN IS NOTHING TO REDRAW. Every accepted click calls this, including the ones a
	 * headless test makes before any strip has been shown — and a refresh that put one up would
	 * make a redraw into a show, which is a different thing and belongs to the game mode.
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
	 * A CONTROLLER WITH NO LOCAL PLAYER HAS NO VIEWPORT TO SET AN INPUT MODE AGAINST, and a
	 * session must still work rather than merely not crash — so this fails closed after the
	 * cursor flag, which needs nothing and is what every headless assertion reads.
	 */
	if (GetLocalPlayer() == nullptr)
	{
		return;
	}

	/*
	 * GameAndUI, BECAUSE BOTH HALVES ARE LIVE AT ONCE AND NEITHER MAY WIN OUTRIGHT. The strip and
	 * the piece menu are clicked with the same pointer the ghost is aimed with, so a UI-only mode
	 * would stop the pawn flying and a game-only mode would put the cursor away.
	 *
	 * HIDDEN DURING CAPTURE, WHICH IS THE RIGHT-DRAG: the look chord takes capture, the pointer
	 * vanishes for the length of the drag and comes back where it was on release — §d's "cursor
	 * hidden, recentred on release". NOT LOCKED to the viewport, because a session is played in a
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
			 * ONE LOOP, TWO PRIORITIES: THE SESSION'S CONTEXT IS APPLIED ABOVE THE OTHER TWO.
			 *
			 * IMC_MouseLook's Mouse2D mapping is chorded on IA_LookModifier, and IA_LookModifier is
			 * mapped in IMC_Session. A chord reads the modifier's TriggerStateTracker, which
			 * EvaluateInputImpl resets at the END of the frame, so the modifier's mapping must be
			 * evaluated EARLIER in the same frame or the chord reads last frame's cleared state and
			 * the camera never turns. ReorderMappings only orders chording-before-chorded within ONE
			 * context; across contexts the order is a priority-descending sort that decides nothing
			 * between equals. Hence the priority — see SessionMappingContextPriority's header.
			 *
			 * The special case is here rather than in a second AddMappingContext call because an
			 * apply written a second way is an apply a later route can forget.
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
	 * ON Started, AND EXACTLY ONCE. With no explicit trigger on the action, Triggered fires
	 * every frame the button is held, so holding LMB would re-trace and re-present the menu
	 * sixty times a second; Completed is the release, which opens a menu on let-go. Opening a
	 * menu is a one-shot press. And a second binding for the same action runs the handler twice
	 * per click — which, now that a miss dismisses, is open-then-immediately-close.
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
		 * HOVER BINDS ON Triggered, WHICH IS THE OPPOSITE OF THE LINE ABOVE AND IS THE POINT.
		 * Inspecting is a one-shot press; hovering is a continuous axis, and with no explicit
		 * trigger asset Enhanced Input actuates an axis action on every frame its value is
		 * non-zero — i.e. on exactly the frames the mouse moved, which are exactly the frames
		 * on which what is under the cursor can have changed. Started fires on the first frame
		 * of a gesture and not again until the mouse stops and restarts, so the highlight would
		 * update once per drag and be stale for the rest of it; Completed fires when the mouse
		 * STOPS, so the brick called out would always be the previous one. A still mouse costs
		 * no traces at all, because an unactuated axis fires nothing.
		 *
		 * AND EXACTLY ONCE, for the same reason as above: a second binding traces and
		 * re-highlights twice on every moved frame for an answer that was already correct.
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
		 * THE SESSION'S EIGHT SHORTCUTS, ON Started, EXACTLY ONCE EACH.
		 *
		 * Started FOR THE REASON IA_InspectPiece USES IT AND IA_HoverPiece DOES NOT: these are
		 * one-shot presses of digital keys. With no explicit trigger asset, Triggered fires on
		 * every frame the key is HELD — holding `]` would walk the build plane up the wall at
		 * sixty courses a second, and holding `Enter` would re-settle the structure on every
		 * frame, which releases pieces irreversibly. Completed is the release, which would run
		 * the command on let-go.
		 *
		 * AND EXACTLY ONCE, NOT AT LEAST ONCE. Two bindings on `Tab` toggle the mode twice per
		 * press, which is a mode switch that appears to do nothing at all.
		 *
		 * SIX OF THEM CARRY THEIR ID AS A BOUND PAYLOAD, because a shortcut IS a toolbar click:
		 * the model's greying and refusals are consulted at the one door either way. The mode and
		 * placement keys stand for a PAIR of chips each, so they go through the toggles, which
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
	 * THROUGH THE SESSION'S DISPATCH RATHER THAN STRAIGHT AT THE INSPECT, because what a click means
	 * now depends on the mode: in Build it lays a piece and in Destroy it is the inspect this
	 * handler has always made. The deprojection and the reach are unchanged — the untestable inch
	 * stays exactly as long as it was.
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

	/* The same dispatch as OnInspectPiece, for the same reason: pointing means two things now. */
	PointerAlongRay(StartCm, StartCm + Direction * PieceMenuCursorReachCm);
}

void ADestructionGamePlayerController::BeginPlay()
{
	Super::BeginPlay();

	/*
	 * THE CURSOR COMES UP ONCE, HERE, AND FOR THE WHOLE SESSION. A toolbar that is on screen in
	 * both modes has to be clickable in both, and the alternative this replaced — raising the
	 * pointer only while a piece menu was up — made the strip reachable only by first opening a
	 * menu over a brick.
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

	/* No viewport means no banner, which is the ordinary case in a test rather than an error. */
	if (Viewport == nullptr)
	{
		return;
	}

	/*
	 * EVERY STRING IS AN ATTRIBUTE AND NOT A VALUE, so the countdown runs. Slate asks an attribute
	 * again on every paint, which is what turns "4.0 s" into a clock without anything here holding
	 * a timer, a tick or a copy of the label.
	 *
	 * BOUND THROUGH MakeAttributeUObject RATHER THAN A LAMBDA CAPTURING `this`. The banner is handed to
	 * the viewport, which holds a shared reference to it; a UObject delegate is not invoked once
	 * its object has gone, so a controller destroyed before its remove ran cannot be read through
	 * a widget the viewport is still painting.
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
