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

// File-local names carry a prefix so they don't collide under a unity build.
namespace
{
	/** This world's structure subsystem, or null (a bare CDO has no world). */
	UDestructionStructureSubsystem* PieceMenuSubsystemOf(const AActor& Actor)
	{
		UWorld* const World = Actor.GetWorld();

		return World != nullptr ? World->GetSubsystem<UDestructionStructureSubsystem>() : nullptr;
	}

	/** The brick for this ref, or null. Resolved rather than indexed, so a stale ref answers null. */
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
	 * Whether a structure has any live pieces. Live, not NumPieces, since removal tombstones.
	 * Shared by GetSessionStructureId and RefreshSessionHasStructure so they agree.
	 */
	bool SessionStructureIsLive(const FStructureBinding* Binding)
	{
		return Binding != nullptr && Binding->GetStructure().NumLivePieces() > 0;
	}
}

static constexpr int32 PieceMenuMappingContextPriority = 0;

/*
 * One above the others so IA_LookModifier (in IMC_Session) evaluates before IMC_MouseLook's
 * chord reads it. At a shared priority free-look measured 0° of yaw.
 */
static constexpr int32 SessionMappingContextPriority = PieceMenuMappingContextPriority + 1;

// Cursor ray reach, 100 m (1 uu = 1 cm). Generous, not tuned.
static constexpr double PieceMenuCursorReachCm = 10000.0;

// Entry row colours, keyed on FInspectorPieceEntry::bIsLivePiece.
static const FLinearColor PieceMenuLivePieceColour(1.0f, 1.0f, 1.0f, 1.0f);
static const FLinearColor PieceMenuDeadPieceColour(0.5f, 0.5f, 0.5f, 0.6f);

// The panel's size depends on detail mode, so it lives in Core as PieceMenuPanelSizePx.

/** Inset from the viewport's right edge. An argument to PieceMenuHomeOffset so tests can sweep it. */
static constexpr double PieceMenuPanelHomeMarginPx = 24.0;

static constexpr float PieceMenuPanelPaddingPx = 10.0f;

/** Max height of the brick list, about eight rows; it scrolls past that. */
static constexpr float PieceMenuBrickListMaxHeightPx = 190.0f;

/*
 * One width for every bar so they form a column. 140 px leaves 15 px between the tightest
 * decade labels and still fits the panel (World.Menu.TheReadoutFitsInsideThePanel).
 */
static constexpr float PieceMenuHeadroomBarWidthPx = 140.0f;
static constexpr float PieceMenuHeadroomBarHeightPx = 8.0f;

/*
 * The swatch tying a joint row to its neighbour brick. The scale row uses width plus gap as
 * left padding so ticks stay under the bars.
 */
static constexpr float PieceMenuJointSwatchWidthPx = 10.0f;
static constexpr float PieceMenuJointSwatchHeightPx = 10.0f;
static constexpr float PieceMenuJointSwatchGapPx = 6.0f;

/** Support-word column width; generous so entry rows never become the panel's widest line. */
static constexpr float PieceMenuEntrySupportWidthPx = 150.0f;

static constexpr float PieceMenuHeadroomScaleHeightPx = 14.0f;

static constexpr float PieceMenuRuleHeightPx = 1.0f;

// Fixed banner width so the text wraps; centred at the top, clear of the right-homed menu.
static constexpr float ScenarioBannerWidthPx = 760.0f;
static constexpr float ScenarioBannerTopMarginPx = 24.0f;

// The near-opaque background keeps text readable against a bright sky; no headless test catches it.
static const FLinearColor PieceMenuPanelBackgroundColour(0.014f, 0.016f, 0.022f, 0.94f);
static const FLinearColor PieceMenuHeaderColour(1.0f, 1.0f, 1.0f, 1.0f);
static const FLinearColor PieceMenuCountColour(0.62f, 0.68f, 0.78f, 1.0f);
static const FLinearColor PieceMenuReadoutColour(0.82f, 0.86f, 0.92f, 1.0f);
static const FLinearColor PieceMenuHintColour(0.55f, 0.60f, 0.68f, 1.0f);
static const FLinearColor PieceMenuRuleColour(1.0f, 1.0f, 1.0f, 0.16f);
static const FLinearColor PieceMenuHeadroomTrackColour(0.0f, 0.0f, 0.0f, 0.55f);

// The title strip's tint, which (with the grab cursor) shows the panel can be dragged.
static const FLinearColor PieceMenuGrabStripColour(0.16f, 0.18f, 0.24f, 0.75f);

// Load-band colours live in DestructionContent::BrickLoadSwatchColours, shared with the overlay materials.

// Support-dot hues. Falling and stranded reuse the load bar's alarm colours; calm buckets avoid them.
static const FLinearColor PieceMenuSupportNotAPieceColour(0.36f, 0.37f, 0.40f, 1.0f);
static const FLinearColor PieceMenuSupportNotSolvedColour(0.45f, 0.55f, 0.78f, 1.0f);
static const FLinearColor PieceMenuSupportFallingColour(0.95f, 0.24f, 0.20f, 1.0f);
static const FLinearColor PieceMenuSupportStrandedColour(0.95f, 0.66f, 0.13f, 1.0f);
static const FLinearColor PieceMenuSupportSupportedColour(0.18f, 0.76f, 0.55f, 1.0f);
static const FLinearColor PieceMenuSupportGroundedColour(0.22f, 0.56f, 0.86f, 1.0f);

static constexpr float PieceMenuSupportDotSizePx = 8.0f;
static constexpr float PieceMenuSupportDotGapPx = 6.0f;

// The neighbour palette lives in DestructionContent::BrickNeighbourSwatchColours, beside its materials.

/** A row past the end of the palette draws no swatch. */
static const FLinearColor PieceMenuNoSwatchColour(0.0f, 0.0f, 0.0f, 0.0f);

// Row colours, keyed on FPieceMenuRow::bIsDestructive.
static const FLinearColor PieceMenuDestructiveRowColour(0.72f, 0.16f, 0.14f, 1.0f);
static const FLinearColor PieceMenuOrdinaryRowColour(1.0f, 1.0f, 1.0f, 1.0f);

// Session strip measurements from SESSION_UI_DESIGN.md §e (48 px is an owner ruling). Not test-measured.
static constexpr float SessionToolbarHeightPx = 48.0f;
static constexpr float SessionToolbarChipHeightPx = 34.0f;
static constexpr float SessionToolbarChipGapPx = 5.0f;
static constexpr float SessionToolbarChipPaddingPx = 12.0f;
static constexpr float SessionToolbarEdgePaddingPx = 10.0f;
static constexpr float SessionToolbarReadoutPaddingPx = 6.0f;

// The hairline between toolbar groups and the gap around it.
static constexpr float SessionToolbarRuleWidthPx = 1.0f;
static constexpr float SessionToolbarRuleGapPx = 10.0f;

// Piece swatches differ in shape as well as colour: the plank is longer and thinner than the brick.
static constexpr float SessionToolbarBrickSwatchWidthPx = 18.0f;
static constexpr float SessionToolbarBrickSwatchHeightPx = 11.0f;
static constexpr float SessionToolbarTimberSwatchWidthPx = 26.0f;
static constexpr float SessionToolbarTimberSwatchHeightPx = 8.0f;
static constexpr float SessionToolbarSwatchGapPx = 7.0f;

/*
 * Strip fill, linear, a step lighter than the panel so they read as separate. Chip colours
 * come from DestructionSession::ChipLookFor.
 */
static const FLinearColor SessionToolbarFillColour(0.020f, 0.023f, 0.030f, 0.96f);

// A second namespace, below the constants its drawing helpers read.
namespace
{
	// FCoreStyle, not FAppStyle: it looks the same in editor and cooked game.
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

	/** Chip caption faces, chosen by FChipLook::bBoldCaption. */
	FSlateFontInfo SessionToolbarBoldFont()
	{
		return FCoreStyle::GetDefaultFontStyle("Bold", 11);
	}

	FSlateFontInfo SessionToolbarRegularFont()
	{
		return FCoreStyle::GetDefaultFontStyle("Regular", 11);
	}

	/** The hover fill: one step toward white, clamped per channel. */
	FLinearColor SessionToolbarLiftedFill(const FLinearColor& Fill)
	{
		constexpr float LiftPerChannel = 0.12f;

		return FLinearColor(
			FMath::Min(1.0f, Fill.R + LiftPerChannel),
			FMath::Min(1.0f, Fill.G + LiftPerChannel),
			FMath::Min(1.0f, Fill.B + LiftPerChannel),
			Fill.A);
	}

	/** The pressed fill. */
	FLinearColor SessionToolbarPressedFill(const FLinearColor& Fill)
	{
		constexpr float PressScale = 0.82f;

		return FLinearColor(Fill.R * PressScale, Fill.G * PressScale, Fill.B * PressScale, Fill.A);
	}

	/**
	 * A chip's style from the model's look. Rounded brushes in all four states, since SButton
	 * swaps brush per state and a square one would change shape under the cursor.
	 */
	FButtonStyle SessionToolbarChipStyle(const DestructionSession::FChipLook& Look)
	{
		// FVector4, not FVector4f: FSlateBrushOutlineSettings' own type.
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

	/** Swatch size for a piece kind. */
	FVector2f SessionToolbarSwatchSizePx(DestructionSession::EToolbarSwatch Swatch)
	{
		return Swatch == DestructionSession::EToolbarSwatch::Timber
			? FVector2f(SessionToolbarTimberSwatchWidthPx, SessionToolbarTimberSwatchHeightPx)
			: FVector2f(SessionToolbarBrickSwatchWidthPx, SessionToolbarBrickSwatchHeightPx);
	}

	/** A piece chip's colour block. An empty SBorder's desired size is its padding. */
	TSharedRef<SWidget> SessionToolbarSwatchBlock(DestructionSession::EToolbarSwatch Swatch)
	{
		const FVector2f SizePx = SessionToolbarSwatchSizePx(Swatch);

		return SNew(SBorder)
			.BorderImage(PieceMenuFillBrush())
			.BorderBackgroundColor(DestructionSession::SwatchColour(Swatch))
			.Padding(FMargin(0.5f * SizePx.X, 0.5f * SizePx.Y));
	}

	/** The 1 px rule drawn where the toolbar group changes (SESSION_UI_DESIGN §b). */
	TSharedRef<SWidget> SessionToolbarGroupRule()
	{
		return SNew(SImage)
			.Image(PieceMenuFillBrush())
			.ColorAndOpacity(PieceMenuRuleColour)
			.DesiredSizeOverride(
				FVector2D(SessionToolbarRuleWidthPx, SessionToolbarChipHeightPx));
	}

	/** Bar colour for a band. An unknown band gets the most severe colour, never a calm one. */
	FLinearColor PieceMenuBandColour(EJointMarginBand Band)
	{
		const int32 Index = static_cast<int32>(Band);

		const bool bKnown =
			Index >= 0 && Index < UE_ARRAY_COUNT(DestructionContent::BrickLoadSwatchColours);

		return DestructionContent::BrickLoadSwatchColours[
			bKnown ? Index : static_cast<int32>(EJointMarginBand::Critical)];
	}

	/** Support-dot colour for a band. An unknown band gets grey, which claims nothing. */
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

	/** The support dot, drawn inside the fixed-width support column so it shifts nothing. */
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

	/** Swatch colour for a neighbour slot; out of range draws nothing. */
	FLinearColor PieceMenuSwatchColour(int32 ColourSlot)
	{
		const TArrayView<const FLinearColor> Palette(DestructionContent::BrickNeighbourSwatchColours);

		return Palette.IsValidIndex(ColourSlot) ? Palette[ColourSlot] : PieceMenuNoSwatchColour;
	}

	/** A joint row's swatch. Drawn even when transparent so the bars stay aligned. */
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
	 * A joint's headroom bar. The fill is an anchored child, not an SProgressBar, so a headless
	 * test can read its width. Colour comes from the band, length from the fraction.
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
	 * The bar's decade ticks, placed on the same curve as the fill. Each label aligns by its own
	 * fraction so the end labels stay on the track.
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

	// Wired in C++, not Blueprint, by the paths RequiredContent.h names.
	static ConstructorHelpers::FObjectFinder<UInputMappingContext> DefaultContext(DestructionContent::DefaultMappingContextPath);
	static ConstructorHelpers::FObjectFinder<UInputMappingContext> MouseLookContext(DestructionContent::MouseLookMappingContextPath);

	MouseLookMappingContext = MouseLookContext.Object;

	DefaultMappingContexts.Add(DefaultContext.Object);
	DefaultMappingContexts.Add(MouseLookMappingContext);

	// Also kept by name so the apply loop can give it SessionMappingContextPriority.
	static ConstructorHelpers::FObjectFinder<UInputMappingContext> SessionContext(DestructionContent::SessionMappingContextPath);

	SessionMappingContext = SessionContext.Object;

	DefaultMappingContexts.Add(SessionMappingContext);

	static ConstructorHelpers::FObjectFinder<UInputAction> InspectPieceActionAsset(DestructionContent::InspectPieceActionPath);

	InspectPieceAction = InspectPieceActionAsset.Object;

	static ConstructorHelpers::FObjectFinder<UInputAction> HoverPieceActionAsset(DestructionContent::HoverPieceActionPath);

	HoverPieceAction = HoverPieceActionAsset.Object;

	// The eight session shortcuts. IA_LookModifier only feeds a chord, so it has no handler here.
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

	BuildComponent = CreateDefaultSubobject<UBuildModeComponent>(TEXT("BuildComponent"));

	// Opens in Destroy since most levels have a structure to pull apart; the build plot switches to Build.
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

	// The player's build wins only once it has live pieces; BeginBuild creates an empty binding up front.
	if (BuildComponent != nullptr)
	{
		const int32 BuildId = BuildComponent->GetStructureId();

		if (SessionStructureIsLive(Subsystem->Find(BuildId)))
		{
			return BuildId;
		}
	}

	// Otherwise the level's own structure; INDEX_NONE on the build plot.
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
	// Remember the old tinted set so bricks that lose their band get refreshed too.
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
		 * Solve only if some piece has no answer yet. Re-solving would overwrite a settle's
		 * SolveAndBreak verdict, so looking at a wall would change it.
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
			// Removed or released pieces get no tint; WorstJointBandForPiece would paint them Critical.
			LoadOverlayStates.Add(
				Binding->IsPieceRemoved(Index) || Binding->IsReleased(Index)
					? EBrickHighlight::None
					: BrickHighlightForLoadBand(
						WorstJointBandForPiece(Binding->GetStructure(), Index)));
		}
	}

	// Refresh old and new sets through the highlight precedence; SetHighlighted is idempotent.
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

	// Refresh first, or the first click after a build becomes real is refused.
	RefreshSessionHasStructure();

	// A greyed button must not run its side effect; the model decides which are greyed.
	const TArray<FToolbarButton> Buttons = SessionToolbarButtons(SessionToolbarState);

	const FToolbarButton* const Button = Buttons.FindByPredicate(
		[Id](const FToolbarButton& Candidate) { return Candidate.Id == Id; });

	if (Button == nullptr || !Button->bEnabled)
	{
		return false;
	}

	SessionToolbarState = ApplyToolbarButton(SessionToolbarState, Id);

	// Settings are pushed from the resulting state, not the button, so clamping stays correct.
	switch (Id)
	{
	case EToolbarButtonId::ModeBuild:
		// Only if no build is open: BeginBuild cancels, which would wipe the plot on a mode round trip.
		if (BuildComponent != nullptr && BuildComponent->GetStructureId() == INDEX_NONE)
		{
			BuildComponent->BeginBuild();
		}

		// The cursor is the session's, not the mode's (SESSION_UI_DESIGN §d).
		break;

	case EToolbarButtonId::ModeDestroy:
		// Hide the ghost but keep the build.
		if (BuildComponent != nullptr)
		{
			BuildComponent->HidePreview();
		}

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
		// Clear means start again: BeginBuild cancels the old build and opens a fresh one.
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

		// The settle changed the structure, so the overlay is stale.
		RefreshLoadOverlay();
		break;

	case EToolbarButtonId::ToggleLoadOverlay:
		RefreshLoadOverlay();
		break;
	}

	// Refresh again: Clear may have emptied the plot.
	RefreshSessionHasStructure();
	RefreshSessionToolbar();

	return true;
}

bool ADestructionGamePlayerController::ToggleSessionMode()
{
	using namespace DestructionSession;

	// Dispatches the opposite mode through the same door as the chips.
	return OnToolbarButton(
		SessionToolbarState.Mode == ESessionMode::Build
			? EToolbarButtonId::ModeDestroy
			: EToolbarButtonId::ModeBuild);
}

bool ADestructionGamePlayerController::ToggleSessionPlacement()
{
	using namespace DestructionSession;

	// Through OnToolbarButton so the key is refused in Destroy mode, where neither chip exists.
	return OnToolbarButton(
		SessionToolbarState.Placement == EPlacementMode::Snap
			? EToolbarButtonId::PlacementFree
			: EToolbarButtonId::PlacementSnap);
}

void ADestructionGamePlayerController::PointerAlongRay(const FVector& StartCm, const FVector& EndCm)
{
	if (SessionToolbarState.Mode == DestructionSession::ESessionMode::Build)
	{
		// A direction, not an end point: the component intersects it with the build plane.
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
		return InspectAlongRay(StartCm, EndCm).Num() > 0;
	}

	if (BuildComponent == nullptr)
	{
		return false;
	}

	// Re-preview from this ray: the previous click changed the binding, so the old preview is stale.
	const FVector Direction = (EndCm - StartCm).GetSafeNormal();

	BuildComponent->UpdatePreviewFromRay(StartCm, Direction);

	const FPieceRef Placed = BuildComponent->ConfirmPlace();

	// And again after: the tick's refresh is throttled on cursor movement, so a still mouse won't redo it.
	BuildComponent->UpdatePreviewFromRay(StartCm, Direction);

	RefreshSessionHasStructure();
	RefreshSessionToolbar();

	// A new brick changes the load path.
	RefreshLoadOverlay();

	return Placed.StructureId != INDEX_NONE && Placed.PieceIndex != INDEX_NONE;
}

bool ADestructionGamePlayerController::RefreshBuildPreviewFromRay(
	const FVector& OriginCm,
	const FVector& Direction)
{
	// A no-op outside Build mode, or a ghost would appear over the wall being demolished.
	if (SessionToolbarState.Mode != DestructionSession::ESessionMode::Build)
	{
		return false;
	}

	if (BuildComponent == nullptr)
	{
		return false;
	}

	// Same path as a pointer move, so both agree on where the click lands.
	return BuildComponent->UpdatePreviewFromRay(OriginCm, Direction).bValid;
}

TArray<FPieceMenuRow> ADestructionGamePlayerController::InspectAlongRay(
	const FVector& StartCm,
	const FVector& EndCm)
{
	TArray<FPieceMenuRow> Rows;

	// Captured before the click so bricks that stop being neighbours get their colours back.
	const TArray<FPieceRef> WereNeighbours = NeighbourPieces();

	UDestructionStructureSubsystem* const Subsystem = PieceMenuSubsystemOf(*this);

	if (Subsystem != nullptr)
	{
		const FPieceHit Hit = Subsystem->TracePiece(StartCm, EndCm);

		// The click ray also updates hover, without waiting for a mouse move.
		SetHoveredPiece(Hit.Ref);

		// A hit toggles the brick; a miss clears the selection. The menu is rebuilt from it below.
		if (Hit.PieceHandle != INDEX_NONE)
		{
			PieceSelection.Toggle(Hit.Ref);

			RefreshPieceHighlight(Hit.Ref);
		}
		else
		{
			ClearPieceSelection();
		}

		// One menu for the selection; PieceActionsFor refuses refs from another structure.
		const TArrayView<const FPieceRef> Selected = PieceSelection.Refs();

		if (Selected.Num() > 0)
		{
			if (const FStructureBinding* const Binding = Subsystem->Find(Selected[0].StructureId))
			{
				Rows = BuildPieceMenuRows(PieceActionsFor(*Binding, Selected), Selected);
			}
		}
	}

	// Always present, even empty, so a miss takes the old menu down.
	ShowPieceMenu(Rows);

	RefreshNeighbourHighlights(WereNeighbours);

	return Rows;
}

FPieceRef ADestructionGamePlayerController::HoverAlongRay(
	const FVector& StartCm,
	const FVector& EndCm)
{
	UDestructionStructureSubsystem* const Subsystem = PieceMenuSubsystemOf(*this);

	// Hover only; the selection and menu are untouched. No subsystem behaves as a miss.
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
	 * Precedence: Inspected > Selected > Neighbour > Hovered > load overlay. Inspected
	 * also requires selection membership, so a stale InspectedPiece is inert.
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
	 * Selected beats neighbour so the player sees what Delete will remove. Neighbour beats
	 * hovered because the cursor is on the panel then.
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

	// The load overlay is lowest so it never hides a selection.
	return LoadHighlightForPiece(Ref);
}

EBrickHighlight ADestructionGamePlayerController::LoadHighlightForPiece(const FPieceRef& Ref) const
{
	// Check the structure as well as the index.
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

	// Check the structure as well as the index.
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
	// Old set then new set; SetHighlighted is idempotent.
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
	// Refresh, not clear, the brick being left: it may still be selected.
	const FPieceRef Previous = HoveredPiece;

	HoveredPiece = Ref;

	RefreshPieceHighlight(Previous);
	RefreshPieceHighlight(Ref);
}

void ADestructionGamePlayerController::ClearPieceSelection()
{
	// Copied out first: Clear empties the array these live in.
	const TArray<FPieceRef> WasSelected(PieceSelection.Refs());

	// Neighbours too, captured before the clear.
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
	 * Refresh, not clear, the brick being left: it is usually still selected and must not look
	 * unselected while Delete would still remove it.
	 */
	const FPieceRef Previous = InspectedPiece;

	// Captured before the ref moves; the neighbour set changes with it.
	const TArray<FPieceRef> WereNeighbours = NeighbourPieces();

	InspectedPiece = Ref;

	RefreshPieceHighlight(Previous);
	RefreshPieceHighlight(Ref);

	RefreshNeighbourHighlights(WereNeighbours);

	RefreshPieceMenuInspectorWidget();
}

bool ADestructionGamePlayerController::ShowPieceMenu(TArrayView<const FPieceMenuRow> Rows)
{
	/*
	 * Dismiss then build, so every add is paired with a remove. Rows must not alias
	 * ShownPieceMenuRows, which the dismiss empties; unguarded (CURRENT_STATE.md).
	 */
	DismissPieceMenu();

	if (Rows.Num() == 0)
	{
		return false;
	}

	ShownPieceMenuRows.Append(Rows.GetData(), Rows.Num());

	BuildPieceMenuWidget();

	// Controls are untouched: the cursor belongs to the session (SESSION_UI_DESIGN §d).
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
	 * Clear the inspected piece explicitly: Slate sends no OnMouseLeave to a removed widget.
	 * After RemovePieceMenuWidget so the refresh finds no box.
	 */
	SetInspectedPiece(FPieceRef());

	return true;
}

bool ADestructionGamePlayerController::IsPieceMenuShown() const
{
	return ShownPieceMenuRows.Num() > 0;
}

TArrayView<const FPieceMenuRow> ADestructionGamePlayerController::GetShownPieceMenuRows() const
{
	return ShownPieceMenuRows;
}

bool ADestructionGamePlayerController::ChoosePieceMenuRow(int32 RowIndex)
{
	// Refused, not clamped: clamping would commit row 0 for any bad index.
	if (!ShownPieceMenuRows.IsValidIndex(RowIndex))
	{
		return false;
	}

	// Copied, not referenced: DismissPieceMenu resets the array.
	const TArray<FPieceRef> Refs = ShownPieceMenuRows[RowIndex].Refs;
	const FPieceAction* const Action = ShownPieceMenuRows[RowIndex].Action;

	DismissPieceMenu();

	// Before the commit, while the bricks still exist to be un-highlighted.
	ClearPieceSelection();

	UDestructionStructureSubsystem* const Subsystem = PieceMenuSubsystemOf(*this);

	if (Subsystem == nullptr)
	{
		return false;
	}

	// One commit for the whole selection, so one solve.
	const bool bCommitted = Subsystem->CommitPieceActionForAll(Refs, *Action) > 0;

	// A delete may have emptied the plot or changed loads.
	if (bCommitted)
	{
		RefreshSessionHasStructure();
		RefreshSessionToolbar();

		RefreshLoadOverlay();
	}

	return bCommitted;
}

void ADestructionGamePlayerController::BuildPieceMenuWidget()
{
	UWorld* const World = GetWorld();

	UGameViewportClient* const Viewport = World != nullptr ? World->GetGameViewport() : nullptr;

	// No viewport in a code-built test world; the rows still stand alone.
	if (Viewport == nullptr)
	{
		return;
	}

	// Home position on first open only, so a dragged position survives rebuilds.
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
	 * Viewport size in Slate units, before the panel has geometry. Divided by the DPI scale;
	 * `!(X > 0)` sends a NaN or non-positive scale to the origin.
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
	 * Strings are taken from the model as given; no logic here, since tests can't reach it.
	 * The panel's own detail is passed only here, not to the shared neighbour accessor.
	 */
	const FPieceMenuInspector Inspector = PieceMenuInspectorForSelection(PieceMenuPanelDetail);

	TSharedRef<SVerticalBox> Panel = SNew(SVerticalBox);

	/*
	 * Heading and count, also the drag handle. No padding on the border, since layout tests
	 * measure the rows below.
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
	 * One row per selected brick; hovering inspects it, with no OnClicked. The support word sits
	 * beside the button, since layout tests find rows by the button's text.
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
					// Not focusable: a focused SButton would swallow Enter (Run) and Space (jump).
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

	// Height-capped and scrolling, so a long selection keeps the action rows on screen.
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
	 * The readout fills the leftover space. Its content is swapped on hover rather than the panel
	 * rebuilt, which would destroy the hovered button and refire OnHovered.
	 */
	Panel->AddSlot()
		.FillHeight(1.0f)
		.Padding(0.0f, 8.0f, 0.0f, 8.0f)
		[
			SAssignNew(PieceMenuInspectorBox, SBox)
		];

	// A rule, then the irreversible action rows last.
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
	 * The count is overlaid rather than inside the button, since tests find rows by button text;
	 * HitTestInvisible so clicks reach the button.
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
					// Not focusable, same reason as the entry rows.
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
	 * Fixed size so rows never move under the cursor. Positioned by a top-left anchored offset,
	 * matching ClampPanelOffset, so a drag just changes the value.
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
	// The slot is AutoSize, so only the corner matters.
	return FMargin(PieceMenuPanelOffsetPx.X, PieceMenuPanelOffsetPx.Y, 0.0f, 0.0f);
}

FVector2D ADestructionGamePlayerController::PieceMenuViewportSizePx() const
{
	// The root's local size is the screen in Slate units; zero with no panel.
	return PieceMenuWidget.IsValid()
		? FVector2D(PieceMenuWidget->GetTickSpaceGeometry().GetLocalSize())
		: FVector2D::ZeroVector;
}

FReply ADestructionGamePlayerController::OnPieceMenuPanelGrabbed(
	const FGeometry& Geometry,
	const FPointerEvent& Event)
{
	/*
	 * Re-clamp on pickup in case the viewport shrank. Capture the mouse so a fast drag keeps
	 * receiving moves after leaving the strip.
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
	if (!bPieceMenuPanelIsHeld)
	{
		return FReply::Unhandled();
	}

	// Measured from the press, not the last frame, so no rounding accumulates.
	const FVector2D DraggedToPx = PieceMenuPanelGrabbedFromPx
		+ FVector2D(Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition()))
		- PieceMenuCursorGrabbedAtPx;

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
	 * Changes the panel's mode only, not the neighbour highlights. The whole panel is rebuilt,
	 * safe because the cursor is on the title strip, not an entry button.
	 */
	PieceMenuPanelDetail = PieceMenuPanelDetail == EPieceMenuDetail::Compact
		? EPieceMenuDetail::Full
		: EPieceMenuDetail::Compact;

	// Slate sends a press before a double-click, so end the drag it started.
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

	// Handles into the panel's tree go with it.
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

	const FPieceMenuInspector Inspector = PieceMenuInspectorForSelection(PieceMenuPanelDetail);

	TSharedRef<SVerticalBox> Readout = SNew(SVerticalBox);

	// Label and hint side by side; the model leaves exactly one of them empty.
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

	// No emptiness checks here: the model decides what is empty.
	Readout->AddSlot()
		.AutoHeight()
		[
			SNew(STextBlock)
			.Font(PieceMenuBodyFont())
			.ColorAndOpacity(PieceMenuReadoutColour)
			.Text(FText::FromString(Inspector.IdentityText))
		];

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

	// One row per joint: swatch, bar, text, so swatches and bars each form a column.
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
	 * HAlign_Left is required: a filling slot would stretch the scale past the bars' width.
	 * The left padding skips the swatch column.
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

	// Scrolls, so many joints don't overflow onto the action rows.
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
	// The first ref names the structure; BuildPieceMenuInspector handles refs that don't belong.
	const TArrayView<const FPieceRef> Selected = PieceSelection.Refs();

	UDestructionStructureSubsystem* const Subsystem = PieceMenuSubsystemOf(*this);

	const FStructureBinding* const Binding = (Subsystem != nullptr && Selected.Num() > 0)
		? Subsystem->Find(Selected[0].StructureId)
		: nullptr;

	// No binding uses an empty one, so the model still words the readout (Core/PieceMenu.h).
	const FStructureBinding NoStructure;

	return BuildPieceMenuInspector(
		Binding != nullptr ? *Binding : NoStructure, Selected, InspectedPiece, Detail);
}

void ADestructionGamePlayerController::SetPieceMenuDetail(EPieceMenuDetail Detail)
{
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
	SetInspectedPiece(FPieceRef());
}

TSharedRef<SWidget> ADestructionGamePlayerController::BuildSessionToolbarPanel()
{
	using namespace DestructionSession;

	// Which buttons, captions and states are the model's (Core/SessionToolbar.h); this only draws them.
	const TArray<FToolbarButton> Buttons = SessionToolbarButtons(SessionToolbarState);

	RebuildSessionChipStyles(Buttons);

	TSharedRef<SHorizontalBox> Strip = SNew(SHorizontalBox);

	for (int32 Index = 0; Index < Buttons.Num(); ++Index)
	{
		const FToolbarButton& Button = Buttons[Index];

		// A rule wherever the group changes.
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

		// Swatch before caption (SESSION_UI_DESIGN §e).
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
					 * Not focusable, or the pawn stops answering W. The style points into the
					 * controller's storage; SButton does not copy it.
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

		// The course readout sits after the down arrow, so it appears only in Build mode.
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
	 * A bar across the bottom; the root is SelfHitTestInvisible so clicks above reach the world.
	 * The strip swallows left down and up so missing a chip doesn't lay a brick. Right button
	 * passes through for the look chord (SESSION_UI_DESIGN §d).
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

	// Unknown ids get the last slot, which holds the greyed look.
	const bool bKnown = Index >= 0 && Index < SessionChipStyleCount - 1;

	return SessionChipStyles[bKnown ? Index : SessionChipStyleCount - 1];
}

void ADestructionGamePlayerController::RebuildSessionChipStyles(
	const TArray<DestructionSession::FToolbarButton>& Buttons)
{
	using namespace DestructionSession;

	// Every slot starts greyed (a default FToolbarButton is disabled), so no stale look survives.
	const FButtonStyle GreyedStyle =
		SessionToolbarChipStyle(ChipLookFor(FToolbarButton(), SessionToolbarState.Mode));

	for (FButtonStyle& Style : SessionChipStyles)
	{
		Style = GreyedStyle;
	}

	// Updated in place, so pointers held by existing chips stay valid.
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

	// No viewport in a code-built test world.
	if (Viewport == nullptr)
	{
		return;
	}

	// Remove then add, keeping them paired; this is also the redraw path.
	RemoveSessionToolbarWidget();

	SessionToolbarWidget = BuildSessionToolbarPanel();

	Viewport->AddViewportWidgetContent(SessionToolbarWidget.ToSharedRef());
}

void ADestructionGamePlayerController::RefreshSessionToolbar()
{
	// Redraw only; showing the strip is the game mode's job.
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

	// No local player (headless): the cursor flag above is all tests read.
	if (GetLocalPlayer() == nullptr)
	{
		return;
	}

	/*
	 * GameAndUI: the pointer both clicks the strip and aims the ghost. Cursor hidden during the
	 * right-drag look (SESSION_UI_DESIGN §d); not locked to the viewport.
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
			// The session context gets the higher priority (see SessionMappingContextPriority).
			for (UInputMappingContext* CurrentContext : DefaultMappingContexts)
			{
				const int32 Priority = CurrentContext == SessionMappingContext
					? SessionMappingContextPriority
					: PieceMenuMappingContextPriority;

				Subsystem->AddMappingContext(CurrentContext, Priority);
			}
		}
	}

	// Started, bound once: Triggered would fire every held frame, and a second binding would open-then-close.
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

		// Hover binds on Triggered, which fires on every frame the mouse moves.
		if (HoverPieceAction != nullptr)
		{
			EnhancedInputComponent->BindAction(
				HoverPieceAction,
				ETriggerEvent::Triggered,
				this,
				&ADestructionGamePlayerController::OnHoverPiece);
		}

		/*
		 * Shortcuts are one-shot, on Started, bound once each. Six pass a button id to the toolbar
		 * door; mode and placement keys go through the toggles.
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

	// No viewport leaves the out params unset.
	if (!DeprojectMousePositionToWorld(StartCm, Direction))
	{
		return;
	}

	// Dispatched by mode: place in Build, inspect in Destroy.
	PrimaryAlongRay(StartCm, StartCm + Direction * PieceMenuCursorReachCm);
}

void ADestructionGamePlayerController::OnHoverPiece()
{
	FVector StartCm;
	FVector Direction;

	if (!DeprojectMousePositionToWorld(StartCm, Direction))
	{
		return;
	}

	PointerAlongRay(StartCm, StartCm + Direction * PieceMenuCursorReachCm);
}

void ADestructionGamePlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

	RefreshBuildPreviewFromCursor();
}

void ADestructionGamePlayerController::RefreshBuildPreviewFromCursor()
{
	// Not while the right-button look is held: the cursor is hidden.
	if (IsInputKeyDown(EKeys::RightMouseButton))
	{
		return;
	}

	float CursorXPx = 0.0f;
	float CursorYPx = 0.0f;

	if (!GetMousePosition(CursorXPx, CursorYPx))
	{
		return;
	}

	// A still mouse costs nothing; setting changes are pushed by the component's setters.
	const FVector2D CursorPx(CursorXPx, CursorYPx);

	if (bHasBuildCursorPx && CursorPx == LastBuildCursorPx)
	{
		return;
	}

	LastBuildCursorPx = CursorPx;
	bHasBuildCursorPx = true;

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

	// The cursor is shown once for the whole session, since the toolbar is always on screen.
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

	if (Viewport == nullptr)
	{
		return;
	}

	/*
	 * Text is bound as attributes so the countdown updates every paint. MakeAttributeUObject,
	 * not a `this` lambda, so a destroyed controller is never read.
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
