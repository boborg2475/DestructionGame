// Copyright Epic Games, Inc. All Rights Reserved.


#include "DestructionGamePlayerController.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputTriggers.h"
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
#include "RequiredContent.h"
#include "World/BrickActor.h"
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
 * THE PANEL'S SIZE, FIXED IN BOTH AXES, AND IT IS THE MECHANISM RATHER THAN A TASTE IN LAYOUT.
 *
 * A PANEL THAT CANNOT CHANGE SIZE CANNOT MOVE ANYTHING, which is the single property every
 * stillness claim about this menu now rests on: whatever the readout is showing and however many
 * bricks are picked, every row is where it was, so a click aimed at one commits that one. It
 * replaces two earlier partial fixes — a top anchor, and putting the readout in the last slot —
 * each of which held only one direction still. World.Menu.PanelDoesNotGrowWithTheSelection and
 * World.Menu.InspectingAnEntryDoesNotMoveTheClickableRows both assert it directly.
 *
 * THE WIDTH IS A MEASUREMENT RATHER THAN A FIT, AND IT IS THE ONE FIGURE HERE THAT IS ASSERTED.
 * 560 px held the longest line the model composed when it was chosen, and then the joint sentence
 * grew a bending clause: "#4  course 3 · #1  bed above  40.0 N  4.551 %  22.0× margin  150.0 N·cm
 * bending" asks for 432 px of glyphs, and it does not start at the column's left edge — the bar
 * and its swatch take 166 px first — so it ran 58 px off the end of a 540 px content column and
 * was cut. A joint line is laid out at its natural width in a box that will not shrink it, so
 * there is no wrapping to absorb that. World.Menu.TheReadoutFitsInsideThePanel measures the
 * overrun on a ragged wall whose corbel sits on one off-centre patch — the only wall shape that
 * bends at all, and so the only one from which this number is visible — which puts the floor at
 * 617.5 px. 640 px clears it by 22 px, about four characters at this font, which is the room the
 * original figure claimed for a course number in the hundreds and the game's own 1,220-brick wall
 * still wants. The panel is right-anchored at PieceMenuPanelMarginPx, so widening it takes room
 * from the empty middle of the screen rather than moving it off the edge.
 *
 * THE HEIGHT IS STILL A FIT. 560 px is about half a 1080 viewport, so the whole panel is on
 * screen with the list, the readout and the action rows all reserved. Nothing asserts it, because
 * a test that pinned "the list is 190 px tall" would break on every visual pass and would still
 * not say the thing that matters.
 */
static constexpr float PieceMenuPanelWidthPx = 640.0f;
static constexpr float PieceMenuPanelHeightPx = 560.0f;

/** How far the panel sits off the right edge of the viewport. */
static constexpr float PieceMenuPanelMarginPx = 24.0f;

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
 * of that, or out of PieceMenuPanelWidthPx alongside it.
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
 * WHAT EACH BAND OF BAR IS FILLED IN, AND THE BAND ITSELF IS THE MODEL'S DECISION RATHER THAN
 * THIS FILE'S.
 *
 * EJointMarginBand says WHERE the colour changes — which side of 10x and of 2x margin a joint
 * falls on — because that is a decision about what this game calls dangerous and it belongs
 * where a test can read it. What is left here is the hue, which is exactly the half nothing
 * headless can judge. Every bar was this one green until now, so the joint at 200 % of capacity
 * and the joint at a ten-thousandth of it differed only by a length with nothing to compare it
 * against.
 */
static const FLinearColor PieceMenuHeadroomComfortableColour(0.18f, 0.76f, 0.55f, 1.0f);
static const FLinearColor PieceMenuHeadroomCautionColour(0.95f, 0.66f, 0.13f, 1.0f);
static const FLinearColor PieceMenuHeadroomCriticalColour(0.95f, 0.24f, 0.20f, 1.0f);

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
	 * What a bar in this band is filled in — A LOOKUP, WHICH IS ALL A WIDGET MAY DO WITH IT.
	 *
	 * The band arrived decided: Presenter.PieceMenuJointMarginBand pins which side of each edge
	 * every joint falls on, including the two boundary rows a hand-picked example never contains.
	 * Nothing here compares a number against anything, so there is no second copy of that rule to
	 * drift — and the arm past the end of the enumeration answers with the most severe colour,
	 * because a bar that is wrong about its own band must not look calm.
	 */
	FLinearColor PieceMenuBandColour(EJointMarginBand Band)
	{
		switch (Band)
		{
		case EJointMarginBand::Comfortable: return PieceMenuHeadroomComfortableColour;
		case EJointMarginBand::Caution:     return PieceMenuHeadroomCautionColour;
		case EJointMarginBand::Critical:    return PieceMenuHeadroomCriticalColour;
		}

		return PieceMenuHeadroomCriticalColour;
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
	 * the look context is remembered by name as well as applied, because the piece menu takes
	 * it away while it is up — the SAME pointer in both places, so there is nothing to drift
	 */
	MouseLookMappingContext = MouseLookContext.Object;

	DefaultMappingContexts.Add(DefaultContext.Object);
	DefaultMappingContexts.Add(MouseLookMappingContext);

	/* the piece menu's own input, by the same one spelling of its path */
	static ConstructorHelpers::FObjectFinder<UInputAction> InspectPieceActionAsset(DestructionContent::InspectPieceActionPath);

	InspectPieceAction = InspectPieceActionAsset.Object;

	/* and the one that keeps the highlight under the cursor, mapped in IMC_Default beside it */
	static ConstructorHelpers::FObjectFinder<UInputAction> HoverPieceActionAsset(DestructionContent::HoverPieceActionPath);

	HoverPieceAction = HoverPieceActionAsset.Object;
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

	return EBrickHighlight::None;
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
	 * controls come back on every one of them without three copies of the restore. It is also
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

	SetPieceMenuControls(true);

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

	SetPieceMenuControls(false);

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
	 * IT COMES DOWN FIRST, BY THE ONE ROUTE OUT OF "A MENU IS UP" — so the cursor and
	 * free-look are given back by the same restore every other route takes, and the commit
	 * below runs with nothing on screen naming the bricks it is about to remove.
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
	return Subsystem->CommitPieceActionForAll(Refs, *Action) > 0;
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

	PieceMenuWidget = BuildPieceMenuPanel();

	Viewport->AddViewportWidgetContent(PieceMenuWidget.ToSharedRef());
}

TSharedRef<SWidget> ADestructionGamePlayerController::BuildPieceMenuPanel()
{
	/*
	 * THE READOUT IS ASKED FOR ONCE AND EVERY STRING IN IT IS TAKEN AS GIVEN. Nothing below
	 * counts, formats, pluralises, filters or resolves anything — Core/PieceMenu.h says at
	 * length why each of those decisions is already made in the model, and the short version
	 * is that this function is the one place no test can reach.
	 */
	const FPieceMenuInspector Inspector = PieceMenuInspectorForSelection();

	TSharedRef<SVerticalBox> Panel = SNew(SVerticalBox);

	/*
	 * THE HEADING AND THE COUNT SHARE A ROW, because they are one sentence about one thing and a
	 * count wrapped onto its own line spends a row of a fixed panel on nothing. Both strings are
	 * the model's; which of them is bold is the only thing decided here.
	 */
	Panel->AddSlot()
		.AutoHeight()
		.Padding(0.0f, 0.0f, 0.0f, 6.0f)
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
					SNew(SButton)
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
					SNew(SButton)
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
	 * A FIXED SIZE ON A REAL BACKGROUND, ANCHORED TO THE RIGHT EDGE.
	 *
	 * THE SIZE IS THE MECHANISM AND THE BACKGROUND IS THE DEFECT NO TEST COULD SEE. A panel that
	 * cannot change size cannot move a row out from under a cursor, whatever it is anchored to
	 * and whatever order its slots are in — which is strictly stronger than the top anchor and
	 * the last-slot readout it replaces, and it is asserted directly rather than argued. And
	 * every line under the brick rows used to be a bare text block over the sky: legible against
	 * a wall, invisible against anything bright, and unreachable by a headless suite that paints
	 * no pixels. A near-opaque fill behind the whole panel is what makes the readout readable.
	 *
	 * THE RIGHT EDGE RATHER THAN THE MIDDLE, because the panel is now big enough to hang over the
	 * wall a player is pointing at, and the wall is the thing they are trying to look at.
	 */
	TSharedRef<SWidget> Framed =
		SNew(SBox)
		.HAlign(HAlign_Right)
		.VAlign(VAlign_Center)
		.Padding(FMargin(0.0f, 0.0f, PieceMenuPanelMarginPx, 0.0f))
		[
			SNew(SBox)
			.WidthOverride(PieceMenuPanelWidthPx)
			.HeightOverride(PieceMenuPanelHeightPx)
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

	/* The box lives inside the panel, so it goes with it and never outlives it. */
	PieceMenuInspectorBox.Reset();

	PieceMenuWidget.Reset();
}

void ADestructionGamePlayerController::RefreshPieceMenuInspectorWidget()
{
	if (!PieceMenuInspectorBox.IsValid())
	{
		return;
	}

	const FPieceMenuInspector Inspector = PieceMenuInspectorForSelection();

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

FPieceMenuInspector ADestructionGamePlayerController::PieceMenuInspectorForSelection() const
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
		Binding != nullptr ? *Binding : NoStructure, Selected, InspectedPiece);
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

void ADestructionGamePlayerController::SetPieceMenuControls(bool bMenuIsUp)
{
	bShowMouseCursor = bMenuIsUp;

	UEnhancedInputLocalPlayerSubsystem* const Subsystem =
		ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer());

	/*
	 * A CONTROLLER WITH NO LOCAL PLAYER HAS NO SUBSYSTEM TO TAKE A CONTEXT OFF, and presenting
	 * must still work rather than merely not crash — so this fails closed and leaves the
	 * cursor flag, which needs nothing, already set above.
	 */
	if (Subsystem == nullptr || MouseLookMappingContext == nullptr)
	{
		return;
	}

	if (bMenuIsUp)
	{
		Subsystem->RemoveMappingContext(MouseLookMappingContext);
	}
	else
	{
		Subsystem->AddMappingContext(MouseLookMappingContext, PieceMenuMappingContextPriority);
	}
}

void ADestructionGamePlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	// only add IMCs for local player controllers
	if (IsLocalPlayerController())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
		{
			for (UInputMappingContext* CurrentContext : DefaultMappingContexts)
			{
				Subsystem->AddMappingContext(CurrentContext, PieceMenuMappingContextPriority);
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

	InspectAlongRay(StartCm, StartCm + Direction * PieceMenuCursorReachCm);
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

	HoverAlongRay(StartCm, StartCm + Direction * PieceMenuCursorReachCm);
}
