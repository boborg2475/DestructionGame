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
 * File-local names carry a PieceMenu prefix: a unity build merges files into one TU, so two
 * files' colliding file-local names are a compile error. See CURRENT_STATE.md.
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
 * The priority the contexts are applied at, named because it is used twice. Prefixed for the
 * unity-build reason above (see CURRENT_STATE.md).
 */
static constexpr int32 PieceMenuMappingContextPriority = 0;

/*
 * The session's context goes one priority above the other two, which is what makes the camera
 * turn. IMC_MouseLook's look is chorded on IA_LookModifier (the right button, mapped by
 * IMC_Session); a chord reads the modifier's trigger state, which Enhanced Input resets at frame
 * end, so the modifier must evaluate earlier in the frame than the mapping chording off it or
 * free-look never triggers. Priority is the only lever across two contexts: at one shared priority
 * a probe measured 0° of yaw.
 */
static constexpr int32 SessionMappingContextPriority = PieceMenuMappingContextPriority + 1;

/*
 * How far a cursor ray reaches, cm (1 uu = 1 cm), i.e. 100 m. One reach for both handlers, since
 * click and hover are the same ray. A generous reach, not a tuned threshold: the trace either
 * hits a brick or misses, and a miss dismisses.
 */
static constexpr double PieceMenuCursorReachCm = 10000.0;

/*
 * What an entry row is drawn in, keyed on FInspectorPieceEntry::bIsLivePiece so a dead brick and
 * a live one do not look alike. Colour only: no filtering or dropping the entry.
 */
static const FLinearColor PieceMenuLivePieceColour(1.0f, 1.0f, 1.0f, 1.0f);
static const FLinearColor PieceMenuDeadPieceColour(0.5f, 0.5f, 0.5f, 0.6f);

/*
 * The panel's size is not here: it is a function of the detail mode (Compact drops the joint
 * table and headroom scale), so it lives in Core as PieceMenuPanelSizePx, beside the derivation
 * that Presenter.PieceMenuPanelSize and World.Menu.TheReadoutFitsInsideThePanel check.
 */

/**
 * How far in from the viewport's right edge the panel opens. Passed to PieceMenuHomeOffset as an
 * argument rather than hard-coded, so Presenter.PanelHomeOffset can sweep other values.
 */
static constexpr double PieceMenuPanelHomeMarginPx = 24.0;

/** The gap between the panel's background and anything drawn on it. */
static constexpr float PieceMenuPanelPaddingPx = 10.0f;

/**
 * Max height of the brick list, about eight rows; past that it scrolls rather than grows, so a
 * long selection pushes nothing off screen. A cap, not a fixed height, so a short selection takes
 * only the room it needs instead of stranding empty panel above the readout.
 */
static constexpr float PieceMenuBrickListMaxHeightPx = 190.0f;

/*
 * The headroom bar's track, one width for every joint so the bars form a column. The width is set
 * by the decade scale under it: the labels straddle fractions of it, so 140 px leaves 15 px
 * between the tightest pair (World.Menu.HeadroomTicksStayInsideTheBarTheyLabel wants 4). Bounded
 * above too — bar plus swatch take 166 px, leaving 22 px once
 * World.Menu.TheReadoutFitsInsideThePanel sweeps a corbelled wall.
 */
static constexpr float PieceMenuHeadroomBarWidthPx = 140.0f;
static constexpr float PieceMenuHeadroomBarHeightPx = 8.0f;

/*
 * The swatch tying a joint row to its neighbour brick, since a word ("course 2 · #4") is not
 * enough to find one in a wall of identical bricks. Size of the block plus the gap to the bar;
 * the scale row uses the same total as left padding so ticks stay under the fills, not the
 * swatches.
 */
static constexpr float PieceMenuJointSwatchWidthPx = 10.0f;
static constexpr float PieceMenuJointSwatchHeightPx = 10.0f;
static constexpr float PieceMenuJointSwatchGapPx = 6.0f;

/*
 * Width of the support-word column on brick rows, wide enough for the longest ("not in this
 * wall") with room to spare so the words align. Deliberately generous: a column that just fitted
 * would make an entry row the tightest line and retarget World.Menu.TheReadoutFitsInsideThePanel
 * away from the joint sentences it measures.
 */
static constexpr float PieceMenuEntrySupportWidthPx = 150.0f;

/** The row the bar's decade ticks are placed along, directly under that column. */
static constexpr float PieceMenuHeadroomScaleHeightPx = 14.0f;

/** The rule that separates the destructive row from everything describing what it destroys. */
static constexpr float PieceMenuRuleHeightPx = 1.0f;

/*
 * Width and top margin of the scenario banner. A stated width, not a fit to the text, so the
 * two-sentence expectation wraps to a few readable lines instead of one viewport-wide line.
 * Centred at the top, the one region the piece menu (right-homed) never covers.
 */
static constexpr float ScenarioBannerWidthPx = 760.0f;
static constexpr float ScenarioBannerTopMarginPx = 24.0f;

/*
 * The panel's colours. The near-opaque dark background is load-bearing: without a fill the readout
 * text is invisible against a bright sky, and no headless test can catch it. The rest are contrast
 * against it.
 */
static const FLinearColor PieceMenuPanelBackgroundColour(0.014f, 0.016f, 0.022f, 0.94f);
static const FLinearColor PieceMenuHeaderColour(1.0f, 1.0f, 1.0f, 1.0f);
static const FLinearColor PieceMenuCountColour(0.62f, 0.68f, 0.78f, 1.0f);
static const FLinearColor PieceMenuReadoutColour(0.82f, 0.86f, 0.92f, 1.0f);
static const FLinearColor PieceMenuHintColour(0.55f, 0.60f, 0.68f, 1.0f);
static const FLinearColor PieceMenuRuleColour(1.0f, 1.0f, 1.0f, 0.16f);
static const FLinearColor PieceMenuHeadroomTrackColour(0.0f, 0.0f, 0.0f, 0.55f);

/*
 * The title strip's tint, the only thing saying the panel can be moved. A lift off the panel's own
 * background so it reads as part of the panel and a separate strip at once. The grab cursor is the
 * other half of the affordance; no word for it, since words live in FPieceMenuInspector.
 */
static const FLinearColor PieceMenuGrabStripColour(0.16f, 0.18f, 0.24f, 0.75f);

/*
 * The three load-band colours are not here: the load overlay needs the same three hues in
 * materials (content, out of a widget's reach), so DestructionContent::BrickLoadSwatchColours
 * holds them beside the material paths, one row per band, with no literal left here to drift.
 */

/*
 * What each support-bucket dot is drawn in. EPieceSupportBand is the model's decision, swept
 * against the word beside it; this is only the hue, which nothing headless can judge. The alarm
 * colours (falling red, stranded amber) are the load bar's own, deliberately, so one panel keeps
 * one vocabulary; the calm buckets sit outside it so nothing calm reads as an alarm.
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
 * The neighbour palette is not here: it used to be a copy of six material emissives held together
 * only by a comment, and a repick drifted the two apart.
 * DestructionContent::BrickNeighbourSwatchColours now sits beside the material paths, held by
 * Content.NeighbourSwatchesMatchTheirMaterials. Six is the model's number (a running-bond brick
 * has six joints); a row past the end carries INDEX_NONE and gets the transparent entry below.
 */

/** What a row past the end of the palette is painted in: nothing at all. */
static const FLinearColor PieceMenuNoSwatchColour(0.0f, 0.0f, 0.0f, 0.0f);

/*
 * What a destructive row is drawn in, keyed on FPieceMenuRow::bIsDestructive so the colour follows
 * the action's own flag rather than a caption compared against "Delete".
 */
static const FLinearColor PieceMenuDestructiveRowColour(0.72f, 0.16f, 0.14f, 1.0f);
static const FLinearColor PieceMenuOrdinaryRowColour(1.0f, 1.0f, 1.0f, 1.0f);

/*
 * The session strip's measurements, from SESSION_UI_DESIGN.md §e. 48 px tall is an owner ruling
 * (72 was "way too big"); a 34 px chip inside leaves 7 px of air, so it reads as a bar with
 * buttons. None of these are test-measured — the model owns which buttons and their state; the
 * pixels are checked by a human against the screenshot proof.
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
 * The two piece swatches, two shapes as well as two colours: the plank longer and thinner than
 * the block, so the chips read apart without their words (§e's swatch "in place of a size
 * caption"). Proportions are roughly the pieces' own.
 */
static constexpr float SessionToolbarBrickSwatchWidthPx = 18.0f;
static constexpr float SessionToolbarBrickSwatchHeightPx = 11.0f;
static constexpr float SessionToolbarTimberSwatchWidthPx = 26.0f;
static constexpr float SessionToolbarTimberSwatchHeightPx = 8.0f;
static constexpr float SessionToolbarSwatchGapPx = 7.0f;

/*
 * What the strip is drawn in, one step lighter than the panel's fill so the two read as separate
 * objects rather than one dark shape with a seam. Linear triples, the form Slate takes; §e's sRGB
 * hexes are what the eye checks them against. Chip fills and accents are not here — they are
 * DestructionSession::ChipLookFor's answer now (swept by Core.SessionToolbar.ChipLook), since a
 * colour multiplied through FCoreStyle's grey brush was never the amber a player saw.
 */
static const FLinearColor SessionToolbarFillColour(0.020f, 0.023f, 0.030f, 0.96f);

/*
 * A second file-local namespace, below the constants it reads: everything here draws the panel and
 * needs a size or colour declared above, so the split is declaration order.
 */
namespace
{
	/*
	 * FCoreStyle, not FAppStyle: FAppStyle resolves to the running app's style (editor vs cooked
	 * game), so the panel would look different in each. FCoreStyle is the same in both and needs no
	 * content asset.
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
	 * The two caption faces; which a chip wears is FChipLook::bBoldCaption. Active shows in both the
	 * caption weight and the fill, since either alone is fragile — the fill reads from peripheral
	 * vision, the word head-on.
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
	 * The fill one step toward white: how a chip answers the cursor. Clamped at white, not lerped,
	 * so a full channel stays put. FMath::Min keeps a NaN a NaN rather than making it a plausible
	 * colour; ChipLook is swept for finiteness so none arrives.
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
	 * A chip's whole style, from the model's look. A rounded box brush, not FCoreStyle's grey
	 * button brush with amber multiplied through it (that yields mustard). All four states are
	 * rounded, since SButton swaps the border brush per state and a square one would change the
	 * chip's shape under the cursor.
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
	 * The block of colour that makes a piece chip look like what it lays. One widget: an empty
	 * SBorder's desired size is exactly its padding, so no sizing box is needed. The colour is
	 * SwatchColour(Kind), the material's own base colour, so chip and brick are one decision.
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
	 * The 1 px rule between two regions of the strip, drawn where the model's group changes (§b:
	 * "a destructive click is never adjacent to a setting click"). Full chip height, so it reads as
	 * a division rather than a tick.
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
	 * What a bar in this band is filled in: a lookup only. The band arrives decided
	 * (Presenter.PieceMenuJointMarginBand), so nothing here compares numbers; an index past the end
	 * answers with the most severe colour, since a bar wrong about its band must not look calm. The
	 * table is the overlay's own (RequiredContent.h), a subscript rather than a second palette.
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
	 * What a brick row's support dot is painted in: the same lookup. The bucket arrives decided
	 * (Presenter.PieceMenuSupportBand, held against each row's word by CheckInspectorInvariants);
	 * an unknown enumerator answers grey, which claims nothing, so a wrong dot asserts no state.
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
	 * The dot saying whether a picked brick is standing up. Inside the fixed-width support column,
	 * so it takes its space from that column's slack and moves nothing else; ahead of the entry
	 * button it would push every label right, out of World.Menu.TheReadoutFitsInsideThePanel's
	 * budget.
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
	 * What a joint row's swatch is painted in: the same lookup. The slot is the model's answer,
	 * swept over every readout; an out-of-range slot fails to the transparent entry, so it draws no
	 * swatch rather than borrowing another's colour.
	 */
	FLinearColor PieceMenuSwatchColour(int32 ColourSlot)
	{
		const TArrayView<const FLinearColor> Palette(DestructionContent::BrickNeighbourSwatchColours);

		return Palette.IsValidIndex(ColourSlot) ? Palette[ColourSlot] : PieceMenuNoSwatchColour;
	}

	/**
	 * The block of colour tying a joint row to its neighbour. Drawn even when transparent, so the
	 * bars stay in a column for the decade scale; a skipped swatch would slide its bar 16 px left.
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
	 * One joint's headroom bar, filled to the model's fraction. The fill is a laid-out child, not a
	 * painted rectangle (and not an SProgressBar, whose Percent has no getter): an anchored child's
	 * arranged width is HeadroomFraction times the track's, readable by a headless test. The colour
	 * comes from the band, not the fraction — how full and how alarming are two answers, and only
	 * the first is a length.
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
	 * The bar's decade ticks, each placed by the model's Fraction along the same curve the fill is
	 * drawn by, so a fill ending at 10x lands under the 10x label. A log axis with no decades is
	 * unreadable — the same fill means 1000x on one panel and 3x on another. Alignment is each
	 * tick's own Fraction, not 0.5, so end labels straddle their point and stay on the track
	 * instead of being half-clipped at the edges.
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
	PlayerCameraManagerClass = ADestructionGameCameraManager::StaticClass();

	/*
	 * Mapping contexts wired here rather than in a Blueprint, so the sandbox runs from C++ defaults
	 * alone, by the paths RequiredContent.h names so the two lists cannot disagree.
	 */
	static ConstructorHelpers::FObjectFinder<UInputMappingContext> DefaultContext(DestructionContent::DefaultMappingContextPath);
	static ConstructorHelpers::FObjectFinder<UInputMappingContext> MouseLookContext(DestructionContent::MouseLookMappingContextPath);

	/*
	 * The look context is remembered by name as well as applied, the same pointer in both places;
	 * its header says why it keeps a name of its own.
	 */
	MouseLookMappingContext = MouseLookContext.Object;

	DefaultMappingContexts.Add(DefaultContext.Object);
	DefaultMappingContexts.Add(MouseLookMappingContext);

	/*
	 * The session's keyboard, pushed in beside the others so the one apply loop covers it too.
	 * Named as well as listed: the loop asks "is this the session's?" to give it
	 * SessionMappingContextPriority instead of the shared one (see that priority's header).
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
	 * The eight session shortcuts. IA_LookModifier is not among them: it only feeds IMC_MouseLook's
	 * chord, so it has no handler and a reference here would be a UPROPERTY nothing reads.
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
	 * The build loop is a default subobject, not something a level attaches: without it, Build mode
	 * would fail every click closed with nothing on screen saying why.
	 */
	BuildComponent = CreateDefaultSubobject<UBuildModeComponent>(TEXT("BuildComponent"));

	/*
	 * The session opens in Destroy, not the model's Build default: most levels lay a structure to
	 * pull apart, and Build mode would hang a gold ghost over it. The game mode puts the build plot
	 * into Build mode, through the same door every click uses.
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
	 * The player's build wins, but only once something is live in it. BeginBuild spends an id on an
	 * empty binding the moment Build mode opens, so an empty build winning here would shadow the
	 * level's wall and let Run report success on an empty graph. SessionStructureIsLive also catches
	 * a build emptied down to a piece count.
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
	 * Otherwise the level's own wall, which is what makes a Destroy session anything but inert. The
	 * build plot built nothing and leaves INDEX_NONE, so the strip greys both commands until the
	 * first brick lands.
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
	 * What was tinted is remembered before recomputing: a brick loses its band untouched (overlay
	 * off, structure changed, piece removed), so telling only the new set would leave the old one
	 * coloured by a stale solve. Same shape as RefreshNeighbourHighlights' "were neighbours".
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
		 * A structure that already holds an answer is read, never re-solved: a bare SolveLoads has
		 * no equilibrium gate and would overwrite a settle's SolveAndBreak verdict, so looking at a
		 * wall would change what it does next. The rule is "solve when there is no answer" — a fresh
		 * plot or a piece placed after a settle carries none. SolveLoads is non-destructive;
		 * SolveAndBreak is never reached from here.
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
			 * A removed or released piece wears nothing. WorstJointBandForPiece fails both closed to
			 * Critical — right for a handle reading, wrong to paint: a gone or already-falling brick
			 * would spend the loudest red on something the player can see move, when the red is for
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
	 * Then old set and new set go back through the precedence, each brick asked afresh what state
	 * it should be in — a selected brick keeps its colour, an unclaimed one falls to its band or to
	 * None. Bricks in both are told twice, harmless because SetHighlighted is idempotent.
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
	 * What there is to command is refreshed first: bHasStructure is a fact about the world, and
	 * checked after the greying it would refuse Run on the first click after a build became real and
	 * accept it on the second — a dropped click.
	 */
	RefreshSessionHasStructure();

	/*
	 * The refusal is the model's, asked rather than re-decided: a greyed button must not run its
	 * side effect — a greyed Course down that still decremented would put the build plane under the
	 * earth with the readout saying course 0.
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
	 * Settings are pushed from the state the transition produced, not the button: CourseUp means
	 * "one more than the course was", and the state holds that sum, so the component is told the
	 * number, not the gesture — the reading that stays right if a transition starts clamping. The
	 * component derives the rest (material, extent, plane; grounded from the snapped pose, DESIGN §8).
	 */
	switch (Id)
	{
	case EToolbarButtonId::ModeBuild:
		/*
		 * A build is opened only if one is not already: Build mode with no structure fails every
		 * click closed, but opening unconditionally is worse — BeginBuild cancels, so a trip to
		 * Destroy and back would sweep the plot.
		 */
		if (BuildComponent != nullptr && BuildComponent->GetStructureId() == INDEX_NONE)
		{
			BuildComponent->BeginBuild();
		}

		/*
		 * Nothing is done about the cursor, a change not an omission: the pointer is the session's
		 * now (SetSessionControls raises it once in BeginPlay), so a mode that touched it would
		 * fight the mode that did not (SESSION_UI_DESIGN §d, S6).
		 */
		break;

	case EToolbarButtonId::ModeDestroy:
		/*
		 * No ghost survives into Destroy mode: a gold brick over a wall being demolished is the most
		 * confusing thing this UI can do. The build itself survives — treating "leaving Build mode"
		 * as a reason to CancelBuild would hand out a fresh empty plot on every look.
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
		 * The state's flag, read after the transition like piece and course: the model already
		 * flipped it, so the component is told which way the next piece lies, not that a chip was
		 * clicked, and derives the swapped footprint itself.
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
		 * The state's choice, read after the transition: the component is told which joint, never
		 * which chip, and looks up the profile at the door so nothing here holds a library address
		 * to get wrong.
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
		 * Clear is "start again", not "stop building", so a fresh plot is left open. BeginBuild
		 * cancels whatever is open first, so the two halves are one call.
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
		 * The overlay is recomputed against what is left: a settle breaks joints and releases
		 * pieces, so every band on screen now reads a structure that no longer exists.
		 */
		RefreshLoadOverlay();
		break;

	case EToolbarButtonId::ToggleLoadOverlay:
		/*
		 * The flag is already the transition's; this is the world catching up, both ways — the tint
		 * goes on, or comes off every brick wearing one.
		 */
		RefreshLoadOverlay();
		break;
	}

	/*
	 * And what there is to command is refreshed again, because a command changes it: Clear leaves an
	 * empty plot, so the strip has to grey Clear and Run on the way out of the click that emptied it.
	 */
	RefreshSessionHasStructure();
	RefreshSessionToolbar();

	return true;
}

bool ADestructionGamePlayerController::ToggleSessionMode()
{
	using namespace DestructionSession;

	/*
	 * The read is the whole function: a toggle that always dispatched one id would appear to jam in
	 * one mode. Everything else — drawn, greyed, effect on the build component — is the one door's.
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
	 * The same shape for Snap/Free, where the door earns its keep: neither chip is on the Destroy
	 * strip, so this must refuse there rather than flip a setting the mode does not draw.
	 * OnToolbarButton consults the list the strip greys from, so key and chip refuse together.
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
		 * The component is given a direction, not an end point, and intersects it with the build
		 * plane itself; an end point would place the ghost where the ray was cut off, not on the
		 * course.
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
	 * The pose is taken again from this ray before committing, not trusting the last pointer move: a
	 * preview predicts the commit only while the binding is unchanged, and the previous click
	 * changed it, so a stale preview would lay this brick at the last one's pose.
	 */
	const FVector Direction = (EndCm - StartCm).GetSafeNormal();

	BuildComponent->UpdatePreviewFromRay(StartCm, Direction);

	const FPieceRef Placed = BuildComponent->ConfirmPlace();

	/*
	 * And again afterwards, along the same ray: the commit spent the last preview, and with the
	 * mouse still the tick's refresh is throttled on cursor position and will not re-drive it. On
	 * the changed binding the solver drops the now-occupied pose and answers the next.
	 */
	BuildComponent->UpdatePreviewFromRay(StartCm, Direction);

	/* A brick landing is what turns an empty plot into something Clear and Run can act on. */
	RefreshSessionHasStructure();
	RefreshSessionToolbar();

	/*
	 * A new brick is a new load path: the overlay survives into Build mode, so a piece laid while it
	 * is on needs its own band and the pieces under it re-read.
	 */
	RefreshLoadOverlay();

	return Placed.StructureId != INDEX_NONE && Placed.PieceIndex != INDEX_NONE;
}

bool ADestructionGamePlayerController::RefreshBuildPreviewFromRay(
	const FVector& OriginCm,
	const FVector& Direction)
{
	/*
	 * Destroy mode is a no-op, checked first: this runs every frame the cursor moves, so leaking the
	 * mode would hang a ghost over the wall being demolished and re-arm a preview a stray confirm
	 * could commit. Reporting false keeps hover (IA_HoverPiece, via PointerAlongRay) separate.
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
	 * The same seam a pointer move uses, so a refreshed ghost and a hovered one agree on where the
	 * click lands. The component intersects the ray with the build plane and fails closed on a miss.
	 */
	return BuildComponent->UpdatePreviewFromRay(OriginCm, Direction).bValid;
}

TArray<FPieceMenuRow> ADestructionGamePlayerController::InspectAlongRay(
	const FVector& StartCm,
	const FVector& EndCm)
{
	TArray<FPieceMenuRow> Rows;

	/*
	 * Which bricks the readout points at, before a click moves anything: toggling a brick out can
	 * stop another being singled out, so the readout empties and its colours have to come back.
	 */
	const TArray<FPieceRef> WereNeighbours = NeighbourPieces();

	UDestructionStructureSubsystem* const Subsystem = PieceMenuSubsystemOf(*this);

	/*
	 * The chain is already written; this is only the wire. TracePiece fails closed, so a miss is a
	 * default ref, and PieceActionsFor re-resolves every ref and answers an empty menu for one that
	 * names nothing.
	 */
	if (Subsystem != nullptr)
	{
		const FPieceHit Hit = Subsystem->TracePiece(StartCm, EndCm);

		/*
		 * A click happens at the cursor, so this ray also answers what is under it. Setting it here
		 * rather than waiting for the next mouse-move stops a brick staying lit after a click moves
		 * off it.
		 */
		SetHoveredPiece(Hit.Ref);

		/*
		 * Clicking a brick toggles it, clicking past everything clears the lot. The selection is
		 * durable and the menu is a projection rebuilt below, so nothing here shows or dismisses: an
		 * empty selection builds no rows, which is how a menu comes down.
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
		 * One menu for the whole selection, against the structure its refs name: a selection is
		 * built by clicking one wall, so the first ref names it and PieceActionsFor refuses any that
		 * do not belong.
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
	 * Every route presents, including the ones that found nothing, so "the ray hit nothing" and
	 * "take the menu down" are one call. One ShowPieceMenu at the end, not an early return per guard:
	 * a bare return would leave the previous brick's menu on screen.
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
	 * Pointing is not choosing, so nothing here touches the selection or a menu, only which brick is
	 * called out. No world or subsystem answers the same as a miss: a default ref, releasing
	 * whatever was called out.
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
	 * The stronger state wins where they coincide: Inspected > Selected > Hovered. A selected brick
	 * under the cursor stays Selected, and the inspected brick (its forces on screen) beats the rest
	 * of the selection so its readout is unambiguous.
	 *
	 * Inspected also asks the selection, not just the ref: an anchor outside its set is a readout of
	 * another brick. This only makes a stale InspectedPiece inert while it is out of the selection;
	 * the ref survives, so DismissPieceMenu is where it is actually cleared.
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
	 * Then the readout's own colours, between selection and cursor. Selected beats neighbour: a
	 * player must be able to see which bricks are going before pressing Delete (irreversible), so a
	 * picked brick keeps its colour rather than a neighbour hue. Neighbour beats hovered because the
	 * cursor is on the panel then, so HoveredPiece is stale by construction.
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
	 * And the load overlay last: everything above says "this one", the overlay says something about
	 * all of them, so it must not beat them and hide the pre-Delete check. Asked here, not painted
	 * on — a per-brick SetHighlighted would fight the cursor and lose. One function decides where
	 * states coincide; the overlay is its last question.
	 */
	return LoadHighlightForPiece(Ref);
}

EBrickHighlight ADestructionGamePlayerController::LoadHighlightForPiece(const FPieceRef& Ref) const
{
	/*
	 * The structure is checked as well as the index: piece 4 of every wall on screen is not piece 4
	 * of the one the overlay solved.
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
	 * The rows belong to the inspected brick, so the far end belongs to its structure. Comparing the
	 * index alone would light brick 4 of every wall the moment brick 4 here became a neighbour.
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
	 * Old set first, new set second, which matters for bricks in both: one may have changed slot,
	 * and refreshing it twice is harmless because SetHighlighted is idempotent.
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
	 * The brick being left is refreshed too, not just cleared — it may be selected, so it stays
	 * called out. Without this every brick the cursor has crossed stays lit.
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
	 * And the readout's own bricks, which are not in that list: clearing the selection drops the
	 * whole neighbour set too, and the loop below would not reach them. Asked before the clear.
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
	 * The brick being left is refreshed, not cleared — it is almost always still selected, so it
	 * drops back to Selected rather than None. Same bug as SetHoveredPiece, sharper: a brick dropped
	 * to None would empty the selection on screen while the commit still deletes it. Reading is not
	 * choosing, so the selection and rows are untouched.
	 */
	const FPieceRef Previous = InspectedPiece;

	/*
	 * And the bricks the readout pointed at are collected before the ref moves, since afterwards
	 * there is nothing to ask — the neighbour set changes wholesale with the readout.
	 */
	const TArray<FPieceRef> WereNeighbours = NeighbourPieces();

	InspectedPiece = Ref;

	RefreshPieceHighlight(Previous);
	RefreshPieceHighlight(Ref);

	RefreshNeighbourHighlights(WereNeighbours);

	/*
	 * And the readout follows the brick it describes: the panel breaks out one brick's joints, so
	 * changing which brick is the whole point of running the cursor down the list.
	 */
	RefreshPieceMenuInspectorWidget();
}

bool ADestructionGamePlayerController::ShowPieceMenu(TArrayView<const FPieceMenuRow> Rows)
{
	/*
	 * Showing is dismiss-then-build, so there is one route out of "a menu is up": replacing, showing
	 * empty, and closing all take it and let go of the inspected brick without three copies. It also
	 * pairs every widget add with a remove, so an unmatched add cannot slip past a headless suite.
	 *
	 * Rows must not alias ShownPieceMenuRows: the dismiss below empties it. Nothing guards it; see
	 * CURRENT_STATE.md.
	 */
	DismissPieceMenu();

	if (Rows.Num() == 0)
	{
		return false;
	}

	ShownPieceMenuRows.Append(Rows.GetData(), Rows.Num());

	BuildPieceMenuWidget();

	/*
	 * The controls are not touched: the cursor is the session's now and the camera is chorded to a
	 * held right button, so there is nothing left to restore (SESSION_UI_DESIGN §d, S6).
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
	 * And nothing is read out any more, a clear not a disable: Slate sends no OnMouseLeave to a
	 * widget that left the tree, so a panel taken down under the cursor leaves InspectedPiece set and
	 * picking the brick again springs the readout back. Through SetInspectedPiece so the let-go brick
	 * drops back to Selected; after RemovePieceMenuWidget so its refresh finds no box.
	 */
	SetInspectedPiece(FPieceRef());

	return true;
}

bool ADestructionGamePlayerController::IsPieceMenuShown() const
{
	/*
	 * The rows are the record, no second flag: an empty list dismisses, so "holding rows" and "a
	 * menu is up" are one fact.
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
	 * An out-of-range index commits nothing, refused not clamped: a Clamp would turn every bad choice
	 * into a commit of row 0. IsValidIndex also refuses "choose row 0 when no menu is up", since an
	 * empty array has no valid index.
	 */
	if (!ShownPieceMenuRows.IsValidIndex(RowIndex))
	{
		return false;
	}

	/*
	 * The chosen row is copied out before the dismiss, load-bearing: DismissPieceMenu Reset()s the
	 * array the rows live in, so a reference would read destroyed elements by commit time. Both
	 * halves come from the row, not remembered state (Core/PieceMenu.h).
	 */
	const TArray<FPieceRef> Refs = ShownPieceMenuRows[RowIndex].Refs;
	const FPieceAction* const Action = ShownPieceMenuRows[RowIndex].Action;

	/*
	 * It comes down first, so the brick it was reading out is let go before the commit runs with
	 * nothing on screen naming the bricks it removes.
	 */
	DismissPieceMenu();

	/*
	 * And the pick goes with it, before the commit: leaving these bricks selected would carry them
	 * into the next click's menu where they no longer resolve, and a brick must still exist to be
	 * told it is no longer called out.
	 */
	ClearPieceSelection();

	UDestructionStructureSubsystem* const Subsystem = PieceMenuSubsystemOf(*this);

	if (Subsystem == nullptr)
	{
		return false;
	}

	/*
	 * One commit for the whole selection, which makes it one solve: looping the single-piece commit
	 * would push N times, each against an answer that saw only part of the batch.
	 */
	const bool bCommitted = Subsystem->CommitPieceActionForAll(Refs, *Action) > 0;

	/*
	 * And the session is asked again what is left to command — the only world-changing door that is
	 * not a toolbar click. Without it the strip would offer Run and Clear over a plot a delete just
	 * emptied. Only when something committed, since a refused action changed nothing.
	 */
	if (bCommitted)
	{
		RefreshSessionHasStructure();
		RefreshSessionToolbar();

		/*
		 * And the overlay is recomputed: a refresh only on the toggle would leave the wall coloured
		 * by the structure as it stood before the delete. Costs nothing while the overlay is off.
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
	 * No viewport means no widget, ordinary in a test not an error: a code-built world has no
	 * UGameViewportClient, so the presented rows stand alone and every menu assertion still holds.
	 */
	if (Viewport == nullptr)
	{
		return;
	}

	/*
	 * The panel takes its home the first time only: the menu is rebuilt on every click, and
	 * PieceMenuPanelOffsetPx is controller state so a corner the player dragged to survives; a home
	 * on every build would snap it back. Where it opens is Core's (Presenter.PanelHomeOffset), which
	 * composes the clamp so nothing here re-clamps.
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
	 * The screen in the units the panel's offset uses, before there is a panel to ask.
	 * PieceMenuViewportSizePx reads the laid-out root, but a widget built this frame has no geometry
	 * yet, so the viewport client's own size is used up front.
	 *
	 * That size is in screen pixels, wrong by the DPI scale: the canvas lays out under Slate's
	 * scaler, so a home in screen pixels applied in scaled ones would open past the right edge.
	 * Dividing by the scale is the conversion. The guard is `!(X > 0)` so a NaN or non-positive
	 * scale fails to the origin, on screen at every size.
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
	 * The readout is asked for once and every string taken as given: nothing here counts, formats,
	 * pluralises or resolves, since this function is the one place no test can reach (Core/PieceMenu.h).
	 * The detail is the panel's own, the one place the compact request may reach — the shared accessor
	 * would darken every neighbour highlight too (see PieceMenuInspectorForSelection).
	 */
	const FPieceMenuInspector Inspector = PieceMenuInspectorForSelection(PieceMenuPanelDetail);

	TSharedRef<SVerticalBox> Panel = SNew(SVerticalBox);

	/*
	 * Heading and count share a row; both strings are the model's, and only which is bold is decided
	 * here. The row is also the panel's drag handle — the widest thing holding nothing clickable, and
	 * present in every state. The border takes no padding of its own: six layout tests measure the
	 * rows below, so the strip is a background plus four event bindings that change no geometry.
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
	 * One row per selected brick; hovering singles it out. A button carries the hover events but no
	 * OnClicked — naming a brick is not acting on it. Each row shows why its brick stands up
	 * (FInspectorPieceEntry::SupportText), so a falling brick is not hidden among identical strings.
	 * The word sits beside the button, not inside: four layout tests find the row by the text under
	 * its button, so a second text block there would rename every row and take those assertions.
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
					 * Not focusable, like the toolbar chips: a focused SButton handles Enter and
					 * Space in OnKeyDown, so clicking a row would cost the player Run (Enter) and the
					 * pawn's jump (Space).
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
	 * The list is capped in height and scrolls inside it, so forty picked bricks do not run the
	 * action rows off screen. The cap keeps the panel's fixed size: the readout below is a fill slot,
	 * so three bricks or forty-five measure the same and lay the action rows from the same edge.
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
	 * The joint breakout gets the leftover space, the same whatever is in it: a fill slot's height is
	 * the panel minus the auto-height rows, so its content cannot move the other rows or the panel.
	 * A box whose content is swapped, not a panel rebuilt — rebuilding on hover would destroy the
	 * button the cursor is on and refire OnHovered on its replacement.
	 */
	Panel->AddSlot()
		.FillHeight(1.0f)
		.Padding(0.0f, 8.0f, 0.0f, 8.0f)
		[
			SAssignNew(PieceMenuInspectorBox, SBox)
		];

	/*
	 * The destructive row is last, behind a rule: releasing a brick is irreversible, so the button
	 * sits past everything describing what it destroys, with nothing clickable below it.
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
	 * A destructive row looks like one and says how much, both the model's: the colour keys on
	 * bIsDestructive, and TargetText comes from the refs the row commits against, so it cannot
	 * promise a different count than it acts on. The count is overlaid, not part of the button, since
	 * four layout tests find the row by the text under its button; HitTestInvisible so a click still
	 * lands on the button.
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
	 * A fixed size on a real background, placed where the player last put it. A panel that cannot
	 * change size cannot move a row out from under the cursor. A position, not an alignment, via a
	 * constraint canvas that takes the corner as a value, so a drag is a new value not a new layout.
	 * Anchored top-left so the offset means what ClampPanelOffset reasons about; AutoSize takes the
	 * size from the child, the presenter's answer for this mode.
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
	 * The corner, nothing else: the slot is AutoSize, so the margin's last two components are ignored
	 * for the child's own size; stating the panel's dimensions here would be a second copy to drift.
	 */
	return FMargin(PieceMenuPanelOffsetPx.X, PieceMenuPanelOffsetPx.Y, 0.0f, 0.0f);
}

FVector2D ADestructionGamePlayerController::PieceMenuViewportSizePx() const
{
	/*
	 * The panel's root is the viewport, so its local size is the screen in the offset's units — see
	 * the header for why GetViewportSize is wrong by the DPI scale. Zero with no panel clamps every
	 * offset to the origin, fail-closed.
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
	 * Where both were when the press landed, plus the capture: Slate stops sending moves once the
	 * pointer leaves the widget, so without it a fast drag drops the panel at the strip's edge. The
	 * corner is re-clamped on pickup, which survives a viewport resize (a corner inside 1920 px is
	 * outside 1280 px, and nothing signals the change). Clamping is idempotent, so this is free.
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
	 * A move that is not a drag is the cursor crossing the strip: Slate sends moves with no button
	 * down, and a panel following the pointer unpicked-up would be unusable.
	 */
	if (!bPieceMenuPanelIsHeld)
	{
		return FReply::Unhandled();
	}

	/*
	 * Measured from the press, not the last frame, so the corner tracks the cursor without
	 * accumulating rounding and returns to start after a trip into a corner. AbsoluteToLocal on both
	 * ends puts the delta in canvas units at any DPI scale; the translation cancels in the subtraction.
	 */
	const FVector2D DraggedToPx = PieceMenuPanelGrabbedFromPx
		+ FVector2D(Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition()))
		- PieceMenuCursorGrabbedAtPx;

	/*
	 * Where the panel may end up is Core's decision: it can strand the panel (a corner off the top
	 * leaves nothing to grab), and it is arithmetic, so Presenter.PanelOffsetClamp holds it.
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
	 * The panel's mode only: NeighbourHighlightForPiece and NeighbourPieces ask
	 * PieceMenuInspectorForSelection with no argument, so rolling the readout up cannot take the
	 * neighbour colours (see the header). The whole panel is rebuilt, not just the readout, since a
	 * mode change moves the action rows. Safe here because the cursor is on the title strip, not an
	 * entry button, so nothing torn down can refire a hover.
	 */
	PieceMenuPanelDetail = PieceMenuPanelDetail == EPieceMenuDetail::Compact
		? EPieceMenuDetail::Full
		: EPieceMenuDetail::Compact;

	/*
	 * And the drag is over, which the double-click's own press turned on: Slate sends a press before
	 * a double-click, so otherwise the strip holds a grab that never sees a release once the widget
	 * is replaced.
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
	 * Both handles live inside the panel, so they go with it: pointers into the tree, not viewport
	 * widgets, and a stale one would keep a torn-down panel alive to be captured to.
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
	 * The readout names its brick or says why it has nothing to say, on one row since the model
	 * guarantees at most one is present: InspectedLabel is empty when none is singled out,
	 * InspectedHintText when one is, so side by side draws whichever exists with no branch near Slate.
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
	 * What that brick is, directly under its name, since the two are one thought. No emptiness check,
	 * like the row above: the model leaves IdentityText empty in the same state as InspectedLabel, so
	 * the branch stays where a test can read it.
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
	 * The support word and the joint sentence, present whether or not there are joints, so no
	 * emptiness check: an isolated pad reads "No joints" from the model, not from a widget noticing
	 * Joints.Num() == 0.
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
	 * One row per joint: swatch, bar, then line. The bar before the words so the bars form a column
	 * for the decade scale below; the swatch before the bar so the colours form a column down the
	 * left edge, where an eye scanning for one neighbour looks.
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
	 * The scale the bars are read against; the left alignment is load-bearing. WidthOverride is a
	 * desired width, and a vertical box slot fills by default, so without HAlign_Left the scale
	 * stretched to the readout's width while the bars kept theirs, and the 10x tick stood where no
	 * fill could reach. The left padding is the swatch column, so the ticks sit under the bars.
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
	 * And the whole readout scrolls inside its space: a brick with more joints than fit would else
	 * run its last lines past the rule and under the delete row.
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
	 * One readout for the whole selection, against the structure its refs name — the same plumbing
	 * as InspectAlongRay: the first ref names the wall, and BuildPieceMenuInspector answers for any
	 * ref that does not belong.
	 */
	const TArrayView<const FPieceRef> Selected = PieceSelection.Refs();

	UDestructionStructureSubsystem* const Subsystem = PieceMenuSubsystemOf(*this);

	const FStructureBinding* const Binding = (Subsystem != nullptr && Selected.Num() > 0)
		? Subsystem->Find(Selected[0].StructureId)
		: nullptr;

	/*
	 * No binding is an empty binding, not an early return, so the model stays the only author of the
	 * readout: a default FPieceMenuInspector would carry an empty CountText where "No bricks selected"
	 * belongs. An empty structure words it through the one route, and keeps SelectedCount right on the
	 * fail-closed paths (Core/PieceMenu.h).
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
	 * The strip is the model's list in its own order — which buttons, their captions, lit or greyed
	 * are all SessionToolbarButtons' (Core/SessionToolbar.h). What is left here is the chip.
	 */
	const TArray<FToolbarButton> Buttons = SessionToolbarButtons(SessionToolbarState);

	/*
	 * The styles are rebuilt first, since the look follows the state: every chip's fill, edge and
	 * caption weight is ChipLookFor's, rebuilt on every click that changes it.
	 */
	RebuildSessionChipStyles(Buttons);

	TSharedRef<SHorizontalBox> Strip = SNew(SHorizontalBox);

	for (int32 Index = 0; Index < Buttons.Num(); ++Index)
	{
		const FToolbarButton& Button = Buttons[Index];

		/*
		 * A hairline where the region changes, and nowhere else. Compared against the neighbour's
		 * group, not counted out in slots, so a retuned strip keeps its rules without a change here.
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
		 * A chip's content is its swatch then its caption, swatch first (§e: "in place of a size
		 * caption") — after the word it would read as a status light, not the piece about to be laid.
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
					 * Not focusable, the one line here that is not cosmetic: a focused SButton stops
					 * the flying pawn answering W, which reads as the game freezing. The style is a
					 * pointer into the controller's own storage (SButton never copies it); the fill is
					 * the style's brush, since a colour multiplied through FCoreStyle's grey brush
					 * could never be the design's amber.
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
		 * The course reads out between its two arrows as a stepper, hung off the down arrow's slot,
		 * which makes it Build-only for free (the course pair is drawn only in Build mode). A text
		 * slot, not an eleventh button, since the model has no row for it; the wording is CourseLabel's.
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
	 * A bar across the bottom; the rest of the screen is not the toolbar's. The handed widget fills
	 * the viewport, so everything above the bar is an empty fill slot, and the root is
	 * SelfHitTestInvisible so it does not swallow a click aimed at a brick. The strip itself swallows
	 * the left button (down and up — a swallowed press with a leaked release is half a click), or
	 * else Slate bubbles an unbound press into PrimaryAlongRay and missing a chip lays a brick. Left
	 * only: the right button is the look chord (SESSION_UI_DESIGN §d), and swallowing it would make
	 * the strip a dead patch the view cannot drag across.
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
	 * The last slot is nobody's, so an id outside the enumeration lands there, not on another chip.
	 * RebuildSessionChipStyles fills it with the greyed look, so an undeclared button reads as
	 * unclickable rather than borrowing a live chip's amber.
	 */
	const bool bKnown = Index >= 0 && Index < SessionChipStyleCount - 1;

	return SessionChipStyles[bKnown ? Index : SessionChipStyleCount - 1];
}

void ADestructionGamePlayerController::RebuildSessionChipStyles(
	const TArray<DestructionSession::FToolbarButton>& Buttons)
{
	using namespace DestructionSession;

	/*
	 * Every slot is written, starting from the greyed look. A default FToolbarButton has bEnabled ==
	 * false, so this is the greyed answer itself — what the unknown slot needs, and what keeps a
	 * stale amber from surviving a mode switch.
	 */
	const FButtonStyle GreyedStyle =
		SessionToolbarChipStyle(ChipLookFor(FToolbarButton(), SessionToolbarState.Mode));

	for (FButtonStyle& Style : SessionChipStyles)
	{
		Style = GreyedStyle;
	}

	/*
	 * Then the strip's own, in place: the slots do not move, so a chip holding a pointer into one
	 * goes on reading a valid style, now the new look.
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
	 * No viewport means no strip, ordinary in a test not an error: a code-built world has no
	 * UGameViewportClient. The session state is the record and stands alone.
	 */
	if (Viewport == nullptr)
	{
		return;
	}

	/*
	 * Remove then add, keeping adds and removes paired: this is also the redraw path, so an unmatched
	 * add cannot slip onto a branch of its own.
	 */
	RemoveSessionToolbarWidget();

	SessionToolbarWidget = BuildSessionToolbarPanel();

	Viewport->AddViewportWidgetContent(SessionToolbarWidget.ToSharedRef());
}

void ADestructionGamePlayerController::RefreshSessionToolbar()
{
	/*
	 * Nothing on screen is nothing to redraw: every accepted click calls this, including headless
	 * ones before any strip is shown, and putting one up would turn a redraw into a show (the game
	 * mode's job).
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
	 * A controller with no local player has no viewport for an input mode, so this fails closed after
	 * the cursor flag, which needs nothing and is what headless assertions read.
	 */
	if (GetLocalPlayer() == nullptr)
	{
		return;
	}

	/*
	 * GameAndUI, since both halves are live at once: the same pointer clicks the strip and aims the
	 * ghost, so UI-only would stop the pawn flying and game-only would hide the cursor. Hidden during
	 * capture (the right-drag look chord), recentred on release (§d). Not locked to the viewport,
	 * since a lock the player did not ask for reads as the game hanging.
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

	if (IsLocalPlayerController())
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
		{
			/*
			 * One loop, two priorities: the session's context applies above the other two, for the
			 * chord-ordering reason in SessionMappingContextPriority's header. One loop, not a second
			 * apply call, so a later route cannot forget it.
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
	 * On Started, exactly once: with no explicit trigger, Triggered fires every frame held
	 * (re-tracing the menu 60/s) and Completed fires on release. A second binding would run the
	 * handler twice per click — now that a miss dismisses, open-then-close.
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
		 * Hover binds on Triggered, the opposite of above: hovering is a continuous axis that
		 * actuates on every frame its value is non-zero — the frames the mouse moved, when what is
		 * under the cursor can change. Started would fire only on the first frame, Completed only
		 * when the mouse stops. A still mouse costs no traces; a second binding would re-highlight
		 * twice per moved frame.
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
		 * The eight shortcuts, on Started, exactly once each — one-shot presses. Triggered would fire
		 * every frame held (walking the build plane at 60 courses/s on `]`, re-settling on held
		 * `Enter`); two bindings on `Tab` would toggle the mode twice per press and appear to do
		 * nothing. Six carry their id as a payload, since a shortcut is a toolbar click through the
		 * one door; the mode and placement keys stand for a pair of chips each, so they go through
		 * the toggles.
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
	 * No viewport means no ray, and the out params are left untouched, so this returns rather than
	 * tracing along stack garbage.
	 */
	if (!DeprojectMousePositionToWorld(StartCm, Direction))
	{
		return;
	}

	/*
	 * Through the session's dispatch, not straight at the inspect, since a click means two things
	 * now: lay a piece in Build, inspect in Destroy. Deprojection and reach are unchanged.
	 */
	PrimaryAlongRay(StartCm, StartCm + Direction * PieceMenuCursorReachCm);
}

void ADestructionGamePlayerController::OnHoverPiece()
{
	FVector StartCm;
	FVector Direction;

	/*
	 * Same untestable inch as OnInspectPiece, failing closed: no viewport means no ray, out params
	 * untouched, so this returns rather than tracing stack garbage.
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
	 * Not while the look chord is held: the right button turns the camera and hides the cursor, so
	 * there is nothing for the ghost to follow. Read directly, not through IA_LookModifier, since
	 * that action has no handler; the two are the same press.
	 */
	if (IsInputKeyDown(EKeys::RightMouseButton))
	{
		return;
	}

	/*
	 * No viewport, no cursor: GetMousePosition answers false with no local player or viewport (a
	 * headless run), out-params untouched, so this returns rather than deprojecting stack garbage.
	 */
	float CursorXPx = 0.0f;
	float CursorYPx = 0.0f;

	if (!GetMousePosition(CursorXPx, CursorYPx))
	{
		return;
	}

	/*
	 * And a still mouse costs nothing: an unmoved cursor names the same point and re-solves the same
	 * snap, and the settings half is carried by the component's setters.
	 */
	const FVector2D CursorPx(CursorXPx, CursorYPx);

	if (bHasBuildCursorPx && CursorPx == LastBuildCursorPx)
	{
		return;
	}

	LastBuildCursorPx = CursorPx;
	bHasBuildCursorPx = true;

	/*
	 * The same untestable inch as OnHoverPiece, failing closed. Everything a player would notice —
	 * mode, where the ghost lands, whether it shows — is behind RefreshBuildPreviewFromRay.
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
	 * The cursor comes up once here for the whole session: a toolbar on screen in both modes must be
	 * clickable in both, unlike the old raise-only-while-a-menu-is-up.
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
	 * Every string is an attribute, not a value, so the countdown runs: Slate re-asks it on every
	 * paint, turning "4.0 s" into a clock with no timer here. Bound through MakeAttributeUObject, not
	 * a lambda capturing `this`: a UObject delegate is not invoked once its object has gone, so a
	 * controller destroyed before its remove cannot be read through the widget the viewport still paints.
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
