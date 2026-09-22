// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "InputCoreTypes.h"
#include "Input/Events.h"
#include "Layout/ArrangedChildren.h"
#include "Layout/ArrangedWidget.h"
#include "Layout/Geometry.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateColor.h"
#include "Styling/SlateTypes.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/SWidget.h"
#include "Widgets/Text/STextBlock.h"

#include "Core/SessionToolbar.h"
#include "DestructionGamePlayerController.h"
#include "Tests/BrickWorldTestSupport.h"
#include "World/BuildModeComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Session S4: the on-screen toolbar strip draws the model, honours bActive and bEnabled, never
 * takes keyboard focus, and routes a click to OnToolbarButton.
 *
 * Slate layout is arithmetic on desired sizes, so the tree can be prepassed and arranged with no
 * window or RHI (as in PieceMenuPanelLayoutTest). Asserted:
 *   - Captions match SessionToolbarButtons in order (the mode pair must not move under a still cursor).
 *   - IsEnabled() matches bEnabled; a lit button that does nothing is a defect.
 *   - SupportsKeyboardFocus() is false on every button. A focusable SButton takes focus on click and
 *     the pawn stops answering W (design §d item 5).
 *   - bActive changes the caption font or the chip fill (either; the visual design is not pinned).
 *   - SimulateClick reaches OnToolbarButton.
 * No pixel sizes are pinned. Needs a world (the controller is an actor) but never ticks it.
 *
 * Namespace is named uniquely because unity builds merge files into one translation unit.
 */
namespace SessionToolbarPanelTestSupport
{
	using namespace DestructionSession;

	/** Layout space in pixels. No claim depends on these values; ArrangeChildren just needs a geometry. */
	constexpr float SessionPanelWidthPx = 1920.0f;
	constexpr float SessionPanelHeightPx = 1080.0f;

	FGeometry SessionPanelRootGeometry()
	{
		return FGeometry::MakeRoot(
			FVector2f(SessionPanelWidthPx, SessionPanelHeightPx), FSlateLayoutTransform());
	}

	/** All STextBlock text under a widget, concatenated. For a button, its caption. */
	FString SessionWidgetText(const TSharedRef<SWidget>& Widget)
	{
		FString Text;

		if (Widget->GetType() == TEXT("STextBlock"))
		{
			Text += StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString();
		}

		FChildren* const Children = Widget->GetChildren();

		for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
		{
			Text += SessionWidgetText(Children->GetChildAt(Index));
		}

		return Text;
	}

	/** The first STextBlock under a widget (a button's caption widget). */
	TSharedPtr<STextBlock> SessionFirstText(const TSharedRef<SWidget>& Widget)
	{
		if (Widget->GetType() == TEXT("STextBlock"))
		{
			return StaticCastSharedRef<STextBlock>(Widget);
		}

		FChildren* const Children = Widget->GetChildren();

		for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
		{
			if (TSharedPtr<STextBlock> Found = SessionFirstText(Children->GetChildAt(Index)))
			{
				return Found;
			}
		}

		return nullptr;
	}

	/** A font as a comparable string. Includes the typeface name because active vs idle is a weight (Bold/Regular). */
	FString SessionFontBits(const FSlateFontInfo& Font)
	{
		return FString::Printf(
			TEXT("%s/%s/%s/%g"),
			*GetNameSafe(Font.FontObject),
			*Font.TypefaceFontName.ToString(),
			*Font.FontMaterial.GetName(),
			Font.Size);
	}

	/**
	 * A chip's fill as a string, read from the brush SButton paints. GetBorderBackgroundColor no longer
	 * works: the fill moved into the brush and the tint stays white on every chip. An unspecified tint
	 * reads the same for every chip, so the font must then carry bActive.
	 */
	FString SessionChipFillBits(const FSlateBrush* Brush)
	{
		if (Brush == nullptr)
		{
			return FString(TEXT("<no brush>"));
		}

		return Brush->TintColor.IsColorSpecified()
			? Brush->TintColor.GetSpecifiedColor().ToString()
			: FString(TEXT("<from the style, unreadable>"));
	}

	/** One button of the drawn strip: what it reads, whether it is live, and how it looks. */
	struct FSessionChip
	{
		FString Caption;
		bool bEnabled = false;
		bool bFocusable = true;

		/** Font and fill joined, so "looks different" is one comparison; bActive may use either. */
		FString Look;
	};

	/** Arrange the tree (no device needed) and record every button depth-first, i.e. left to right. */
	void SessionCollectChips(
		const TSharedRef<SWidget>& Widget,
		const FGeometry& Geometry,
		TArray<FSessionChip>& Out)
	{
		if (Widget->GetType() == TEXT("SButton"))
		{
			const TSharedRef<SButton> Button = StaticCastSharedRef<SButton>(Widget);

			FSessionChip& Chip = Out.AddDefaulted_GetRef();
			Chip.Caption = SessionWidgetText(Widget);
			Chip.bEnabled = Widget->IsEnabled();
			Chip.bFocusable = Button->SupportsKeyboardFocus();

			const TSharedPtr<STextBlock> Caption = SessionFirstText(Widget);

			Chip.Look = FString::Printf(
				TEXT("font %s, fill %s"),
				Caption.IsValid() ? *SessionFontBits(Caption->GetFont()) : TEXT("<no caption>"),
				*SessionChipFillBits(Button->GetBorderImage()));
		}

		FArrangedChildren Arranged(EVisibility::All);

		Widget->ArrangeChildren(Geometry, Arranged);

		for (int32 Index = 0; Index < Arranged.Num(); ++Index)
		{
			SessionCollectChips(Arranged[Index].Widget, Arranged[Index].Geometry, Out);
		}
	}

	/** Build the strip for the controller's current state and read every chip. */
	TArray<FSessionChip> SessionMeasureStrip(ADestructionGamePlayerController& Controller)
	{
		const TSharedRef<SWidget> Panel = Controller.BuildSessionToolbarPanel();

		Panel->SlatePrepass(1.0f);

		TArray<FSessionChip> Chips;

		SessionCollectChips(Panel, SessionPanelRootGeometry(), Chips);

		return Chips;
	}

	FString SessionDescribeChips(const TArray<FSessionChip>& Chips)
	{
		if (Chips.Num() == 0)
		{
			return TEXT("<no buttons>");
		}

		FString Line;

		for (int32 Index = 0; Index < Chips.Num(); ++Index)
		{
			Line += FString::Printf(
				TEXT("%s'%s'[%s%s]"),
				Index == 0 ? TEXT("") : TEXT(", "),
				*Chips[Index].Caption,
				Chips[Index].bEnabled ? TEXT("live") : TEXT("greyed"),
				Chips[Index].bFocusable ? TEXT(", FOCUSABLE") : TEXT(""));
		}

		return Line;
	}

	FString SessionDescribeModel(const TArray<FToolbarButton>& Buttons)
	{
		FString Line;

		for (int32 Index = 0; Index < Buttons.Num(); ++Index)
		{
			Line += FString::Printf(
				TEXT("%s'%s'[%s%s]"),
				Index == 0 ? TEXT("") : TEXT(", "),
				*Buttons[Index].Label,
				Buttons[Index].bEnabled ? TEXT("live") : TEXT("greyed"),
				Buttons[Index].bActive ? TEXT(", lit") : TEXT(""));
		}

		return Line;
	}

	/** The chip reading exactly this caption, or null. */
	const FSessionChip* SessionFindChip(const TArray<FSessionChip>& Chips, const FString& Caption)
	{
		return Chips.FindByPredicate(
			[&Caption](const FSessionChip& Chip) { return Chip.Caption == Caption; });
	}

	/** The button the model gives this id, or null. */
	const FToolbarButton* SessionFindModelButton(
		const TArray<FToolbarButton>& Buttons, EToolbarButtonId Id)
	{
		return Buttons.FindByPredicate(
			[Id](const FToolbarButton& Button) { return Button.Id == Id; });
	}

	/** Check the drawn strip matches the model: same buttons, order and greying, none focusable. */
	void SessionCheckStripMatchesModel(
		FAutomationTestBase& Test,
		const TCHAR* Where,
		const TArray<FSessionChip>& Chips,
		const TArray<FToolbarButton>& Model)
	{
		Test.TestEqual(
			FString::Printf(
				TEXT("%s: the strip must draw one button per model row — %d, not %d. It drew [%s] "
					 "against [%s]"),
				Where, Model.Num(), Chips.Num(),
				*SessionDescribeChips(Chips), *SessionDescribeModel(Model)),
			Chips.Num(), Model.Num());

		if (Chips.Num() != Model.Num())
		{
			return;
		}

		for (int32 Index = 0; Index < Model.Num(); ++Index)
		{
			Test.TestEqual(
				FString::Printf(
					TEXT("%s: button %d must read the model's own caption '%s'; it reads '%s'. The "
						 "order is a player-facing promise — the mode pair may not move under a "
						 "stationary cursor. Drawn [%s]"),
					Where, Index, *Model[Index].Label, *Chips[Index].Caption,
					*SessionDescribeChips(Chips)),
				Chips[Index].Caption, Model[Index].Label);

			Test.TestEqual(
				FString::Printf(
					TEXT("%s: button %d ('%s') must be drawn %s, because that is what the model says "
						 "about it; it is drawn %s"),
					Where, Index, *Model[Index].Label,
					Model[Index].bEnabled ? TEXT("LIVE") : TEXT("GREYED"),
					Chips[Index].bEnabled ? TEXT("live") : TEXT("greyed")),
				Chips[Index].bEnabled, Model[Index].bEnabled);

			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: button %d ('%s') MUST NOT TAKE KEYBOARD FOCUS. A focusable SButton takes "
						 "user focus on click and the flying pawn stops answering W — a player would "
						 "report that as the game freezing. SupportsKeyboardFocus reports %d"),
					Where, Index, *Model[Index].Label, Chips[Index].bFocusable ? 1 : 0),
				!Chips[Index].bFocusable);
		}
	}

	/** Every widget in the tree, pre-order (left to right), with its geometry. Mouse events need the geometry. */
	void SessionCollectArranged(
		const TSharedRef<SWidget>& Widget,
		const FGeometry& Geometry,
		TArray<FArrangedWidget>& Out)
	{
		Out.Emplace(Widget, Geometry);

		FArrangedChildren Arranged(EVisibility::All);

		Widget->ArrangeChildren(Geometry, Arranged);

		for (int32 Index = 0; Index < Arranged.Num(); ++Index)
		{
			SessionCollectArranged(Arranged[Index].Widget, Arranged[Index].Geometry, Out);
		}
	}

	/** Lay the controller's current strip out and record every widget in it with its geometry. */
	TArray<FArrangedWidget> SessionArrangeStrip(ADestructionGamePlayerController& Controller)
	{
		const TSharedRef<SWidget> Panel = Controller.BuildSessionToolbarPanel();

		Panel->SlatePrepass(1.0f);

		TArray<FArrangedWidget> Arranged;

		SessionCollectArranged(Panel, SessionPanelRootGeometry(), Arranged);

		return Arranged;
	}

	/** Whether any SButton is under this widget. */
	bool SessionHasButtonDescendant(const TSharedRef<SWidget>& Widget)
	{
		if (Widget->GetType() == TEXT("SButton"))
		{
			return true;
		}

		FChildren* const Children = Widget->GetChildren();

		for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
		{
			if (SessionHasButtonDescendant(Children->GetChildAt(Index)))
			{
				return true;
			}
		}

		return false;
	}

	/** The bar: the outermost SBorder containing the chips. Found by tree, not pixels; pre-order gives the outermost. */
	const FArrangedWidget* SessionFindBar(const TArray<FArrangedWidget>& Arranged)
	{
		return Arranged.FindByPredicate(
			[](const FArrangedWidget& Entry)
			{
				return Entry.Widget->GetType() == TEXT("SBorder")
					&& SessionHasButtonDescendant(Entry.Widget);
			});
	}

	/** A left-button event at an absolute point. The pressed set is the buttons held after the transition, as Slate sends it. */
	FPointerEvent SessionMouseEvent(const FVector2f& AbsolutePositionPx, bool bIsDown)
	{
		TSet<FKey> Pressed;

		if (bIsDown)
		{
			Pressed.Add(EKeys::LeftMouseButton);
		}

		return FPointerEvent(
			0,
			AbsolutePositionPx,
			AbsolutePositionPx,
			Pressed,
			EKeys::LeftMouseButton,
			0.0f,
			FModifierKeysState());
	}

	/** Every STextBlock in the strip in slot order, captions and non-captions. */
	TArray<FString> SessionStripTexts(const TArray<FArrangedWidget>& Arranged)
	{
		TArray<FString> Texts;

		for (const FArrangedWidget& Entry : Arranged)
		{
			if (Entry.Widget->GetType() == TEXT("STextBlock"))
			{
				Texts.Add(StaticCastSharedRef<STextBlock>(Entry.Widget)->GetText().ToString());
			}
		}

		return Texts;
	}

	FString SessionDescribeTexts(const TArray<FString>& Texts)
	{
		if (Texts.Num() == 0)
		{
			return TEXT("<no text at all>");
		}

		FString Line;

		for (int32 Index = 0; Index < Texts.Num(); ++Index)
		{
			Line += FString::Printf(
				TEXT("%s%d:'%s'"), Index == 0 ? TEXT("") : TEXT(", "), Index, *Texts[Index]);
		}

		return Line;
	}

	/** One item of the strip: a chip or something between chips. The walk stops at chips so a swatch is not counted as a divider. */
	struct FSessionStripItem
	{
		FString Type;
		bool bIsChip = false;
		float WidthPx = 0.0f;
		float HeightPx = 0.0f;
		TSharedPtr<SWidget> Widget;
		FGeometry Geometry;
	};

	void SessionCollectStripRun(
		const TSharedRef<SWidget>& Widget,
		const FGeometry& Geometry,
		TArray<FSessionStripItem>& Out)
	{
		const bool bIsChip = Widget->GetType() == TEXT("SButton");

		FSessionStripItem& Item = Out.AddDefaulted_GetRef();
		Item.Type = Widget->GetType().ToString();
		Item.bIsChip = bIsChip;
		Item.WidthPx = static_cast<float>(Geometry.GetLocalSize().X);
		Item.HeightPx = static_cast<float>(Geometry.GetLocalSize().Y);
		Item.Widget = Widget;
		Item.Geometry = Geometry;

		if (bIsChip)
		{
			return;
		}

		FArrangedChildren Arranged(EVisibility::All);

		Widget->ArrangeChildren(Geometry, Arranged);

		for (int32 Index = 0; Index < Arranged.Num(); ++Index)
		{
			SessionCollectStripRun(Arranged[Index].Widget, Arranged[Index].Geometry, Out);
		}
	}

	/** Whether an item is a divider: a non-chip under 3 px wide and at least 2 px tall. Measured, not matched by class. */
	bool SessionItemIsARule(const FSessionStripItem& Item)
	{
		return !Item.bIsChip && Item.WidthPx > 0.0f && Item.WidthPx < 3.0f && Item.HeightPx >= 2.0f;
	}

	/** Whether any STextBlock is under this widget. */
	bool SessionHasTextDescendant(const TSharedRef<SWidget>& Widget)
	{
		if (Widget->GetType() == TEXT("STextBlock"))
		{
			return true;
		}

		FChildren* const Children = Widget->GetChildren();

		for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
		{
			if (SessionHasTextDescendant(Children->GetChildAt(Index)))
			{
				return true;
			}
		}

		return false;
	}

	/** A colour block drawn on a chip before its caption. */
	struct FSessionSwatch
	{
		FString Type;
		float WidthPx = 0.0f;
		float HeightPx = 0.0f;
		bool bColourSpecified = false;
		FLinearColor Colour = FLinearColor::Transparent;
	};

	/**
	 * Swatches inside one chip, before its caption (§e; after the word it reads as a status light).
	 * A swatch is a text-free box at most 20 px tall, which excludes the chip's full-height wrappers.
	 */
	TArray<FSessionSwatch> SessionChipSwatches(const FSessionStripItem& Chip)
	{
		TArray<FArrangedWidget> Inside;

		SessionCollectArranged(Chip.Widget.ToSharedRef(), Chip.Geometry, Inside);

		int32 FirstText = Inside.Num();

		for (int32 Index = 0; Index < Inside.Num(); ++Index)
		{
			if (Inside[Index].Widget->GetType() == TEXT("STextBlock"))
			{
				FirstText = Index;
				break;
			}
		}

		TArray<FSessionSwatch> Swatches;

		for (int32 Index = 0; Index < FirstText; ++Index)
		{
			const TSharedRef<SWidget> Widget = Inside[Index].Widget;

			if (Widget == Chip.Widget || SessionHasTextDescendant(Widget))
			{
				continue;
			}

			const FVector2f SizePx = FVector2f(Inside[Index].Geometry.GetLocalSize());

			if (SizePx.X < 3.0f || SizePx.Y < 2.0f || SizePx.Y > 20.0f)
			{
				continue;
			}

			FSessionSwatch& Swatch = Swatches.AddDefaulted_GetRef();
			Swatch.Type = Widget->GetType().ToString();
			Swatch.WidthPx = SizePx.X;
			Swatch.HeightPx = SizePx.Y;

			if (Widget->GetType() == TEXT("SBorder"))
			{
				const FSlateColor Fill =
					StaticCastSharedRef<SBorder>(Widget)->GetBorderBackgroundColor();

				Swatch.bColourSpecified = Fill.IsColorSpecified();
				Swatch.Colour = Fill.IsColorSpecified()
					? Fill.GetSpecifiedColor() : FLinearColor::Transparent;
			}
		}

		return Swatches;
	}

	bool SessionColoursExactlyEqual(const FLinearColor& A, const FLinearColor& B)
	{
		return A.R == B.R && A.G == B.G && A.B == B.B && A.A == B.A;
	}

	FString SessionDescribeColour(const FLinearColor& C)
	{
		return FString::Printf(TEXT("(%g, %g, %g, a %g)"), C.R, C.G, C.B, C.A);
	}

	/** A brush's tint, or a note that it has none. */
	FString SessionDescribeTint(const FSlateBrush& Brush)
	{
		return Brush.TintColor.IsColorSpecified()
			? SessionDescribeColour(Brush.TintColor.GetSpecifiedColor())
			: FString(TEXT("<from the style, unreadable>"));
	}

	/** The SButton with exactly this caption, from the live tree so it can be pressed. */
	TSharedPtr<SButton> SessionFindButtonWidget(
		const TSharedRef<SWidget>& Widget, const FString& Caption)
	{
		if (Widget->GetType() == TEXT("SButton") && SessionWidgetText(Widget) == Caption)
		{
			return StaticCastSharedRef<SButton>(Widget);
		}

		FChildren* const Children = Widget->GetChildren();

		for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
		{
			if (TSharedPtr<SButton> Found =
					SessionFindButtonWidget(Children->GetChildAt(Index), Caption))
			{
				return Found;
			}
		}

		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarPanelDrawsTheModelTest,
	"DestructionGame.World.Session.ToolbarPanelDrawsTheModel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarPanelDrawsTheModelTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionSession;
	using namespace SessionToolbarPanelTestSupport;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	ADestructionGamePlayerController* const Controller =
		TestWorld.World->SpawnActor<ADestructionGamePlayerController>();

	TestNotNull(TEXT("fixture: the test world should spawn the game's player controller"), Controller);

	if (Controller == nullptr)
	{
		TestWorld.End();
		return true;
	}

	// One: the Destroy strip, which every scenario level opens with.

	TArray<FSessionChip> DestroyChips;

	{
		const FSessionToolbarState& State = Controller->GetSessionToolbarState();

		TestTrue(
			TEXT("fixture: a fresh controller must be in Destroy mode for this to be the Destroy "
				 "strip"),
			State.Mode == ESessionMode::Destroy);

		const TArray<FToolbarButton> Model = SessionToolbarButtons(State);

		DestroyChips = SessionMeasureStrip(*Controller);

		AddInfo(FString::Printf(
			TEXT("the Destroy strip drew [%s]"), *SessionDescribeChips(DestroyChips)));

		SessionCheckStripMatchesModel(*this, TEXT("Destroy mode"), DestroyChips, Model);

		// Guard against a vacuous match: with nothing built, the model must grey Run structure.
		const FToolbarButton* const Run =
			SessionFindModelButton(Model, EToolbarButtonId::RunStructure);

		TestTrue(
			*FString::Printf(
				TEXT("fixture: with nothing built the model must be greying Run structure, or the "
					 "enabled-state comparison above proves nothing. The model is [%s]"),
				*SessionDescribeModel(Model)),
			Run != nullptr && !Run->bEnabled);

		const FSessionChip* const RunChip = SessionFindChip(DestroyChips, TEXT("Run structure"));

		TestTrue(
			*FString::Printf(
				TEXT("and the strip must DRAW it greyed — a lit button that does nothing is worse than "
					 "an absent one. It drew [%s]"),
				*SessionDescribeChips(DestroyChips)),
			RunChip != nullptr && !RunChip->bEnabled);
	}

	// Two: the Build strip.

	TArray<FSessionChip> BuildChips;

	{
		TestTrue(
			TEXT("fixture: the Build tab must be clickable"),
			Controller->OnToolbarButton(EToolbarButtonId::ModeBuild));

		const FSessionToolbarState& State = Controller->GetSessionToolbarState();

		const TArray<FToolbarButton> Model = SessionToolbarButtons(State);

		BuildChips = SessionMeasureStrip(*Controller);

		AddInfo(FString::Printf(
			TEXT("the Build strip drew [%s]"), *SessionDescribeChips(BuildChips)));

		// Also checked against a literal, so a strip that never re-reads state cannot match in both modes.
		TestTrue(
			*FString::Printf(
				TEXT("fixture: the Build strip is the seventeen-chip configuration; the model offers %d "
					 "[%s]"),
				Model.Num(), *SessionDescribeModel(Model)),
			Model.Num() == 17);

		SessionCheckStripMatchesModel(*this, TEXT("Build mode"), BuildChips, Model);
	}

	// Three: bActive is visible in the widget.

	{
		const FSessionChip* const BuildTabIdle = SessionFindChip(DestroyChips, TEXT("Build"));
		const FSessionChip* const DestroyTabLit = SessionFindChip(DestroyChips, TEXT("Destroy"));

		const FSessionChip* const BuildTabLit = SessionFindChip(BuildChips, TEXT("Build"));
		const FSessionChip* const DestroyTabIdle = SessionFindChip(BuildChips, TEXT("Destroy"));

		if (BuildTabIdle == nullptr || DestroyTabLit == nullptr
			|| BuildTabLit == nullptr || DestroyTabIdle == nullptr)
		{
			AddError(FString::Printf(
				TEXT("the strip must draw both mode tabs in both modes for the lit/idle claim to be "
					 "sayable; Destroy mode drew [%s] and Build mode drew [%s]"),
				*SessionDescribeChips(DestroyChips), *SessionDescribeChips(BuildChips)));

			TestWorld.End();
			return true;
		}

		// Within one strip, the active mode tab must look different from the inactive one.
		TestTrue(
			*FString::Printf(
				TEXT("IN DESTROY MODE THE DESTROY TAB IS LIT AND THE BUILD TAB IS NOT, and they must "
					 "be drawn differently — bActive is a decision the model already made and the "
					 "widget has to show it. Destroy reads (%s); Build reads (%s)"),
				*DestroyTabLit->Look, *BuildTabIdle->Look),
			DestroyTabLit->Look != BuildTabIdle->Look);

		TestTrue(
			*FString::Printf(
				TEXT("AND IN BUILD MODE, THE OTHER WAY ROUND. Build reads (%s); Destroy reads (%s)"),
				*BuildTabLit->Look, *DestroyTabIdle->Look),
			BuildTabLit->Look != DestroyTabIdle->Look);

		/*
		 * Across the two strips, each tab's look must change when it becomes active. This ties the
		 * checks above to bActive rather than slot position. Not a swap: each mode has its own accent.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("the Build tab must LOOK DIFFERENT when it is the mode you are in: lit it reads "
					 "(%s), idle it reads (%s)"),
				*BuildTabLit->Look, *BuildTabIdle->Look),
			BuildTabLit->Look != BuildTabIdle->Look);

		TestTrue(
			*FString::Printf(
				TEXT("and so must the Destroy tab: lit it reads (%s), idle it reads (%s)"),
				*DestroyTabLit->Look, *DestroyTabIdle->Look),
			DestroyTabLit->Look != DestroyTabIdle->Look);
	}

	// Four: a real click on the widget reaches OnToolbarButton.

	{
		const TSharedRef<SWidget> Panel = Controller->BuildSessionToolbarPanel();

		Panel->SlatePrepass(1.0f);

		const TSharedPtr<SButton> DestroyButton =
			SessionFindButtonWidget(Panel, FString(TEXT("Destroy")));

		TestTrue(
			TEXT("fixture: the Build strip must carry a pressable Destroy tab"),
			DestroyButton.IsValid());

		TestTrue(
			*FString::Printf(
				TEXT("fixture: the session must be in Build mode before the click, or the assertion "
					 "after it is free; it is in mode %d"),
				static_cast<int32>(Controller->GetSessionToolbarState().Mode)),
			Controller->GetSessionToolbarState().Mode == ESessionMode::Build);

		if (DestroyButton.IsValid())
		{
			DestroyButton->SimulateClick();

			TestTrue(
				*FString::Printf(
					TEXT("CLICKING THE DESTROY TAB MUST SWITCH THE SESSION TO DESTROY MODE. The strip "
						 "is the only input the player has; a button wired to nothing is a UI that "
						 "draws perfectly and does nothing. The session is in mode %d"),
					static_cast<int32>(Controller->GetSessionToolbarState().Mode)),
				Controller->GetSessionToolbarState().Mode == ESessionMode::Destroy);
		}
	}

	TestWorld.End();

	return true;
}

/**
 * The bar handles a left press and release on its own background, so a click beside a chip does not
 * reach the world.
 *
 * An unbound SBorder returns Unhandled, so the press bubbles to the SViewport and fires
 * IA_InspectPiece; in Build mode PrimaryAlongRay then lays a brick. SButtons handle their own
 * presses; the gaps and the bar past the last chip leak.
 *
 * Left button only: right-drag is the look chord (IMC_MouseLook, S6 permanent cursor) and must still
 * work over the bar.
 *
 * The press point is 4 px inside the bar's right edge. Fixture checks confirm it is on the bar, on no
 * chip, and that the bar is hit-testable. Needs a world (the controller is an actor); events are
 * delivered directly to the widget.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarBarSwallowsClicksTest,
	"DestructionGame.World.Session.ToolbarBarSwallowsClicks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarBarSwallowsClicksTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionSession;
	using namespace SessionToolbarPanelTestSupport;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	ADestructionGamePlayerController* const Controller =
		TestWorld.World->SpawnActor<ADestructionGamePlayerController>();

	TestNotNull(TEXT("fixture: the test world should spawn the game's player controller"), Controller);

	if (Controller == nullptr)
	{
		TestWorld.End();
		return true;
	}

	// Build mode, where a leaked click lays a brick rather than selecting one.
	TestTrue(
		TEXT("fixture: the Build tab must be clickable"),
		Controller->OnToolbarButton(EToolbarButtonId::ModeBuild));

	const TArray<FArrangedWidget> Arranged = SessionArrangeStrip(*Controller);

	const FArrangedWidget* const Bar = SessionFindBar(Arranged);

	if (Bar == nullptr)
	{
		AddError(FString::Printf(
			TEXT("fixture: the strip must be drawn as a border with the chips inside it; the tree "
				 "holds %d widgets and none of them is one"),
			Arranged.Num()));

		TestWorld.End();
		return true;
	}

	const FVector2f BarSizePx = FVector2f(Bar->Geometry.GetLocalSize());

	// Four pixels inside the right edge, vertically centred: on the bar, on no chip.
	const FVector2f PressAtPx =
		FVector2f(Bar->Geometry.LocalToAbsolute(FVector2f(BarSizePx.X - 4.0f, BarSizePx.Y * 0.5f)));

	AddInfo(FString::Printf(
		TEXT("the bar is %g x %g at absolute (%g, %g); the press is at (%g, %g)"),
		BarSizePx.X, BarSizePx.Y,
		Bar->Geometry.GetAbsolutePosition().X, Bar->Geometry.GetAbsolutePosition().Y,
		PressAtPx.X, PressAtPx.Y));

	// Fixture: on the bar, and on none of its chips.

	TestTrue(
		*FString::Printf(
			TEXT("fixture: the press must land ON the bar, or this says nothing about the bar. It is "
				 "at (%g, %g)"),
			PressAtPx.X, PressAtPx.Y),
		Bar->Geometry.IsUnderLocation(PressAtPx));

	{
		int32 Chips = 0;
		int32 ChipsUnderThePress = 0;

		for (const FArrangedWidget& Entry : Arranged)
		{
			if (Entry.Widget->GetType() != TEXT("SButton"))
			{
				continue;
			}

			++Chips;

			if (Entry.Geometry.IsUnderLocation(PressAtPx))
			{
				++ChipsUnderThePress;
			}
		}

		TestTrue(
			*FString::Printf(
				TEXT("fixture: the strip must have drawn its chips for this to be a claim about the "
					 "GAPS between them; it drew %d"),
				Chips),
			Chips > 0);

		TestEqual(
			FString::Printf(
				TEXT("fixture: AND THE PRESS MUST BE OVER NONE OF THEM — a press on a chip is handled "
					 "by SButton and would prove nothing about the background. %d chip(s) are under it"),
				ChipsUnderThePress),
			ChipsUnderThePress, 0);
	}

	// The bar must be hit-testable, or Slate never routes to it.

	TestTrue(
		*FString::Printf(
			TEXT("the bar must be HIT-TESTABLE: a handler bound to a widget Slate routes nothing to "
				 "swallows nothing. Its visibility reads %s"),
			*Bar->Widget->GetVisibility().ToString()),
		Bar->Widget->GetVisibility().IsHitTestVisible());

	// The claim: press and release on the background are handled.

	{
		const FPointerEvent Down = SessionMouseEvent(PressAtPx, true);

		const FReply DownReply = Bar->Widget->OnMouseButtonDown(Bar->Geometry, Down);

		TestTrue(
			*FString::Printf(
				TEXT("A LEFT PRESS ON THE STRIP'S OWN BACKGROUND MUST BE HANDLED. Unhandled bubbles to "
					 "the SViewport, into the input stack, into IA_InspectPiece and — in Build mode — "
					 "into PrimaryAlongRay, so missing a chip by three pixels LAYS A BRICK where that "
					 "pixel's ray meets the build plane. The bar replied %s"),
				DownReply.IsEventHandled() ? TEXT("Handled") : TEXT("UNHANDLED")),
			DownReply.IsEventHandled());

		const FPointerEvent Up = SessionMouseEvent(PressAtPx, false);

		const FReply UpReply = Bar->Widget->OnMouseButtonUp(Bar->Geometry, Up);

		// The release too: Enhanced Input reads key-up, so a leaked release is half a click.
		TestTrue(
			*FString::Printf(
				TEXT("AND SO MUST THE RELEASE: a swallowed press with a leaked release delivers half a "
					 "click to the world. The bar replied %s"),
				UpReply.IsEventHandled() ? TEXT("Handled") : TEXT("UNHANDLED")),
			UpReply.IsEventHandled());
	}

	TestWorld.End();

	return true;
}

/**
 * The Build strip draws CourseLabel(State.Course) as text between the Course down and Course up
 * captions; the Destroy strip draws none; two CourseUp clicks make it read "Course 2". Nothing else
 * on screen shows the build plane's height.
 *
 * The position is asserted by index in the strip's text list, strictly between the two arrows. The
 * readout must not be an SButton: the chip list must still match the model exactly. Needs a world
 * (the controller is an actor) but never ticks it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarShowsTheCourseReadoutTest,
	"DestructionGame.World.Session.ToolbarShowsTheCourseReadout",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarShowsTheCourseReadoutTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionSession;
	using namespace SessionToolbarPanelTestSupport;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	ADestructionGamePlayerController* const Controller =
		TestWorld.World->SpawnActor<ADestructionGamePlayerController>();

	TestNotNull(TEXT("fixture: the test world should spawn the game's player controller"), Controller);

	if (Controller == nullptr)
	{
		TestWorld.End();
		return true;
	}

	// Wording comes from the model, not literals; Core.SessionToolbar.* owns the wording.
	const FString CourseZero = CourseLabel(0);
	const FString CourseTwo = CourseLabel(2);

	// The arrow captions also come from the model, so a caption retune does not break this test.
	FSessionToolbarState BuildState;
	BuildState.Mode = ESessionMode::Build;

	const TArray<FToolbarButton> BuildModel = SessionToolbarButtons(BuildState);

	const FToolbarButton* const DownButton =
		SessionFindModelButton(BuildModel, EToolbarButtonId::CourseDown);

	const FToolbarButton* const UpButton =
		SessionFindModelButton(BuildModel, EToolbarButtonId::CourseUp);

	if (DownButton == nullptr || UpButton == nullptr)
	{
		AddError(FString::Printf(
			TEXT("fixture: the Build strip's model must carry both course arrows; it is [%s]"),
			*SessionDescribeModel(BuildModel)));

		TestWorld.End();
		return true;
	}

	const FString DownCaption = DownButton->Label;
	const FString UpCaption = UpButton->Label;

	// One: the Destroy strip has no course readout.

	{
		TestTrue(
			TEXT("fixture: a fresh controller must be in Destroy mode for this arm to be the Destroy "
				 "strip"),
			Controller->GetSessionToolbarState().Mode == ESessionMode::Destroy);

		const TArray<FString> Texts = SessionStripTexts(SessionArrangeStrip(*Controller));

		AddInfo(FString::Printf(TEXT("the Destroy strip reads [%s]"), *SessionDescribeTexts(Texts)));

		// Destroy mode has no build plane, so a course readout would describe nothing.
		TestEqual(
			FString::Printf(
				TEXT("THE DESTROY STRIP MUST NOT CARRY THE COURSE READOUT: it has no course stepper to "
					 "read out. It draws [%s]"),
				*SessionDescribeTexts(Texts)),
			Texts.IndexOfByKey(CourseZero), static_cast<int32>(INDEX_NONE));
	}

	// Two: the Build strip draws it between the two arrows.

	{
		TestTrue(
			TEXT("fixture: the Build tab must be clickable"),
			Controller->OnToolbarButton(EToolbarButtonId::ModeBuild));

		TestEqual(
			FString::Printf(
				TEXT("fixture: a fresh session starts on course 0; it reads %d"),
				Controller->GetSessionToolbarState().Course),
			Controller->GetSessionToolbarState().Course, 0);

		const TArray<FArrangedWidget> Arranged = SessionArrangeStrip(*Controller);

		const TArray<FString> Texts = SessionStripTexts(Arranged);

		AddInfo(FString::Printf(TEXT("the Build strip reads [%s]"), *SessionDescribeTexts(Texts)));

		const int32 DownIndex = Texts.IndexOfByKey(DownCaption);
		const int32 ReadoutIndex = Texts.IndexOfByKey(CourseZero);
		const int32 UpIndex = Texts.IndexOfByKey(UpCaption);

		TestTrue(
			*FString::Printf(
				TEXT("fixture: the Build strip must draw both course arrows; '%s' is at %d and '%s' at "
					 "%d in [%s]"),
				*DownCaption, DownIndex, *UpCaption, UpIndex, *SessionDescribeTexts(Texts)),
			DownIndex != INDEX_NONE && UpIndex != INDEX_NONE);

		TestTrue(
			*FString::Printf(
				TEXT("THE BUILD STRIP MUST SAY WHICH COURSE YOU ARE ON. '%s' is the model's own wording "
					 "and nothing on screen draws it — the ghost moves, but course 4 and course 5 look "
					 "identical from a flying camera, so a player who has lost count can only click down "
					 "until the button greys out. The strip draws [%s]"),
				*CourseZero, *SessionDescribeTexts(Texts)),
			ReadoutIndex != INDEX_NONE);

		if (ReadoutIndex != INDEX_NONE && DownIndex != INDEX_NONE && UpIndex != INDEX_NONE)
		{
			TestTrue(
				*FString::Printf(
					TEXT("AND IT MUST SIT BETWEEN THE TWO ARROWS — arrow, value, arrow is what a stepper "
						 "is. '%s' is at %d, '%s' at %d, '%s' at %d in [%s]"),
					*DownCaption, DownIndex, *CourseZero, ReadoutIndex, *UpCaption, UpIndex,
					*SessionDescribeTexts(Texts)),
				DownIndex < ReadoutIndex && ReadoutIndex < UpIndex);
		}

		// The readout is a text slot, not a chip: the model has no row for it.
		const TArray<FSessionChip> Chips = SessionMeasureStrip(*Controller);

		const TArray<FToolbarButton> Model =
			SessionToolbarButtons(Controller->GetSessionToolbarState());

		TestEqual(
			FString::Printf(
				TEXT("THE READOUT IS NOT A BUTTON: the strip must still draw exactly one chip per model "
					 "row — %d, not %d. It drew [%s]"),
				Model.Num(), Chips.Num(), *SessionDescribeChips(Chips)),
			Chips.Num(), Model.Num());

		TestNull(
			*FString::Printf(
				TEXT("and no chip may READ the readout — a pressable 'Course 0' is a control that does "
					 "nothing. The chips are [%s]"),
				*SessionDescribeChips(Chips)),
			SessionFindChip(Chips, CourseZero));
	}

	// Three: the readout follows the clicks.

	{
		TestTrue(
			TEXT("Course up is always live, so the first step must land"),
			Controller->OnToolbarButton(EToolbarButtonId::CourseUp));

		TestTrue(
			TEXT("and so must the second"),
			Controller->OnToolbarButton(EToolbarButtonId::CourseUp));

		TestEqual(
			FString::Printf(
				TEXT("fixture: two steps up put the session on course 2; it reads %d"),
				Controller->GetSessionToolbarState().Course),
			Controller->GetSessionToolbarState().Course, 2);

		const TArray<FString> Texts = SessionStripTexts(SessionArrangeStrip(*Controller));

		AddInfo(FString::Printf(
			TEXT("after two steps up the Build strip reads [%s]"), *SessionDescribeTexts(Texts)));

		// A stale readout is worse than none. Assert the new reading is present and the old one gone.
		TestTrue(
			*FString::Printf(
				TEXT("TWO STEPS UP MUST READ '%s'. A readout that never changes is worse than no "
					 "readout: it names a course the build plane is not on. The strip draws [%s]"),
				*CourseTwo, *SessionDescribeTexts(Texts)),
			Texts.Contains(CourseTwo));

		TestFalse(
			*FString::Printf(
				TEXT("and '%s' must be gone from it. The strip draws [%s]"),
				*CourseZero, *SessionDescribeTexts(Texts)),
			Texts.Contains(CourseZero));
	}

	TestWorld.End();

	return true;
}

/**
 * Chips are rounded boxes at the look model's radius, edge width and fill; hover brightens and press
 * darkens; a 1 px rule sits between chips whose EToolbarGroup differs and nowhere else; each piece
 * chip carries one swatch of its material's colour before its caption.
 *
 * Multiplying a colour into FCoreStyle's grey brush cannot produce the design's amber, so each chip
 * needs its own brush. The §b regions keep destructive clicks away from setting clicks.
 *
 * SButton has no style getter, so the style is read via SessionChipStyleFor and the chip's border
 * brush must be one of that style's four by address. SButton stores a raw const FButtonStyle*, so
 * that storage must outlive the widget and never move. Swatch colour is the model's SwatchColour;
 * the plank swatch must be longer and thinner than the brick. Hues and paddings are not pinned.
 * Needs a world but never ticks it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarChipsAreRoundedAndGroupedTest,
	"DestructionGame.World.Session.ToolbarChipsAreRoundedAndGrouped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarChipsAreRoundedAndGroupedTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionSession;
	using namespace SessionToolbarPanelTestSupport;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	ADestructionGamePlayerController* const Controller =
		TestWorld.World->SpawnActor<ADestructionGamePlayerController>();

	TestNotNull(TEXT("fixture: the test world should spawn the game's player controller"), Controller);

	if (Controller == nullptr)
	{
		TestWorld.End();
		return true;
	}

	// Build mode: the only strip with all three regions and all swatches.
	TestTrue(
		TEXT("fixture: the Build tab must be clickable"),
		Controller->OnToolbarButton(EToolbarButtonId::ModeBuild));

	const FSessionToolbarState& State = Controller->GetSessionToolbarState();
	const TArray<FToolbarButton> Model = SessionToolbarButtons(State);

	// One panel for every claim: the style rows compare brush pointers against this tree.
	const TSharedRef<SWidget> Panel = Controller->BuildSessionToolbarPanel();

	Panel->SlatePrepass(1.0f);

	TArray<FSessionStripItem> Run;

	SessionCollectStripRun(Panel, SessionPanelRootGeometry(), Run);

	TArray<int32> ChipItems;

	for (int32 Index = 0; Index < Run.Num(); ++Index)
	{
		if (Run[Index].bIsChip)
		{
			ChipItems.Add(Index);
		}
	}

	{
		FString RunLine;

		for (const FSessionStripItem& Item : Run)
		{
			if (Item.bIsChip || SessionItemIsARule(Item))
			{
				RunLine += FString::Printf(
					TEXT("%s%s(%gx%g)"),
					RunLine.IsEmpty() ? TEXT("") : TEXT(", "),
					Item.bIsChip ? TEXT("CHIP") : *Item.Type, Item.WidthPx, Item.HeightPx);
			}
		}

		AddInfo(FString::Printf(
			TEXT("the Build strip's top-level run holds %d widgets; its chips and hairlines are [%s]"),
			Run.Num(), *RunLine));
	}

	if (ChipItems.Num() != Model.Num())
	{
		AddError(FString::Printf(
			TEXT("fixture: the strip must draw one chip per model row — %d, not %d. "
				 "ToolbarPanelDrawsTheModel owns that claim; nothing below can be said without it"),
			Model.Num(), ChipItems.Num()));

		TestWorld.End();
		return true;
	}

	// One: every chip is a rounded box in the model's style.

	for (int32 Index = 0; Index < Model.Num(); ++Index)
	{
		const FToolbarButton& Button = Model[Index];
		const FSessionStripItem& Item = Run[ChipItems[Index]];
		const FChipLook Look = ChipLookFor(Button, State.Mode);

		const FButtonStyle& Style = Controller->SessionChipStyleFor(Button);

		const FString Where = FString::Printf(TEXT("the chip reading '%s'"), *Button.Label);

		TestTrue(
			*FString::Printf(
				TEXT("%s must be drawn as a ROUNDED BOX. FCoreStyle's grey button brush with a colour "
					 "multiplied through it cannot produce the design's amber — the accent a player "
					 "sees today is dark mustard. Its Normal brush draws as %d, RoundedBox is %d"),
				*Where, static_cast<int32>(Style.Normal.DrawAs),
				static_cast<int32>(ESlateBrushDrawType::RoundedBox)),
			Style.Normal.DrawAs == ESlateBrushDrawType::RoundedBox);

		TestTrue(
			*FString::Printf(
				TEXT("%s must be rounded at the look's own %g px, its brush says %g"),
				*Where, Look.CornerRadiusPx, Style.Normal.OutlineSettings.CornerRadii.X),
			Style.Normal.OutlineSettings.CornerRadii.X == Look.CornerRadiusPx);

		TestTrue(
			*FString::Printf(
				TEXT("%s must carry the look's own %g px drop edge, its brush says %g"),
				*Where, Look.OutlineWidthPx, Style.Normal.OutlineSettings.Width),
			Style.Normal.OutlineSettings.Width == Look.OutlineWidthPx);

		TestTrue(
			*FString::Printf(
				TEXT("%s must be FILLED with the look's own %s — the model decides the fill and the "
					 "widget draws it; the brush tints %s"),
				*Where, *SessionDescribeColour(Look.Fill), *SessionDescribeTint(Style.Normal)),
			Style.Normal.TintColor.IsColorSpecified()
				&& SessionColoursExactlyEqual(Style.Normal.TintColor.GetSpecifiedColor(), Look.Fill));

		if (Style.Normal.TintColor.IsColorSpecified()
			&& Style.Hovered.TintColor.IsColorSpecified()
			&& Style.Pressed.TintColor.IsColorSpecified())
		{
			const FLinearColor Normal = Style.Normal.TintColor.GetSpecifiedColor();
			const FLinearColor Hovered = Style.Hovered.TintColor.GetSpecifiedColor();
			const FLinearColor Pressed = Style.Pressed.TintColor.GetSpecifiedColor();

			TestTrue(
				*FString::Printf(
					TEXT("%s must LIFT under the cursor: every channel of its hovered fill %s must be "
						 "at least its normal %s, and at least one brighter. A chip that does not "
						 "answer the cursor reads as scenery"),
					*Where, *SessionDescribeColour(Hovered), *SessionDescribeColour(Normal)),
				Hovered.R >= Normal.R && Hovered.G >= Normal.G && Hovered.B >= Normal.B
					&& (Hovered.R > Normal.R || Hovered.G > Normal.G || Hovered.B > Normal.B));

			TestTrue(
				*FString::Printf(
					TEXT("%s must PRESS DOWN on click: every channel of its pressed fill %s must be at "
						 "most its normal %s, and at least one darker"),
					*Where, *SessionDescribeColour(Pressed), *SessionDescribeColour(Normal)),
				Pressed.R <= Normal.R && Pressed.G <= Normal.G && Pressed.B <= Normal.B
					&& (Pressed.R < Normal.R || Pressed.G < Normal.G || Pressed.B < Normal.B));
		}

		// The chip must wear that style object by address, or the checks above prove nothing about the widget.
		const TSharedRef<SButton> Chip = StaticCastSharedRef<SButton>(Item.Widget.ToSharedRef());

		const FSlateBrush* const Worn = Chip->GetBorderImage();

		TestTrue(
			*FString::Printf(
				TEXT("%s must actually BE WEARING the style SessionChipStyleFor reports — its border "
					 "brush must be one of that style's four. SButton stores a raw const FButtonStyle* "
					 "and never copies it, so the storage behind that reference must outlive the "
					 "widget and must not move"),
				*Where),
			Worn == &Style.Normal || Worn == &Style.Hovered || Worn == &Style.Pressed
				|| Worn == &Style.Disabled);

		TestTrue(
			*FString::Printf(
				TEXT("%s draws its border with a brush whose DrawAs is %d; a chip is rounded in "
					 "whichever state it is in"),
				*Where, Worn != nullptr ? static_cast<int32>(Worn->DrawAs) : -1),
			Worn != nullptr && Worn->DrawAs == ESlateBrushDrawType::RoundedBox);
	}

	// Two: a hairline rule where the group changes, and nowhere else.

	for (int32 Index = 0; Index + 1 < Model.Num(); ++Index)
	{
		const bool bGroupChanges = Model[Index].Group != Model[Index + 1].Group;

		int32 Rules = 0;

		for (int32 Between = ChipItems[Index] + 1; Between < ChipItems[Index + 1]; ++Between)
		{
			Rules += SessionItemIsARule(Run[Between]) ? 1 : 0;
		}

		if (bGroupChanges)
		{
			TestTrue(
				*FString::Printf(
					TEXT("A RULE MUST SEPARATE '%s' FROM '%s' — they are in different regions of the "
						 "strip, and §b puts the commands past a rule so that a destructive click is "
						 "never adjacent to a setting click. %d hairline(s) were drawn between them"),
					*Model[Index].Label, *Model[Index + 1].Label, Rules),
				Rules >= 1);
		}
		else
		{
			TestEqual(
				FString::Printf(
					TEXT("AND NO RULE MAY SPLIT ONE REGION: '%s' and '%s' are both in the same group, "
						 "and %d hairline(s) were drawn between them. A divider between every pair is "
						 "a different design and reads as noise"),
					*Model[Index].Label, *Model[Index + 1].Label, Rules),
				Rules, 0);
		}
	}

	// Three: piece chips carry their material's colour.

	{
		FSessionSwatch BrickSwatch;
		FSessionSwatch TimberSwatch;
		bool bHaveBrick = false;
		bool bHaveTimber = false;

		for (int32 Index = 0; Index < Model.Num(); ++Index)
		{
			const FToolbarButton& Button = Model[Index];

			const TArray<FSessionSwatch> Swatches = SessionChipSwatches(Run[ChipItems[Index]]);

			const int32 Expected = Button.Swatch == EToolbarSwatch::None ? 0 : 1;

			TestEqual(
				FString::Printf(
					TEXT("the chip reading '%s' must carry %d swatch box(es) before its caption, it "
						 "carries %d — a brick chip that looks like a word is a chip a player has to "
						 "READ while flying a camera"),
					*Button.Label, Expected, Swatches.Num()),
				Swatches.Num(), Expected);

			if (Swatches.Num() != 1 || Expected != 1)
			{
				continue;
			}

			const FSessionSwatch& Swatch = Swatches[0];

			AddInfo(FString::Printf(
				TEXT("'%s' carries a %s swatch of %g x %g px, filled %s"),
				*Button.Label, *Swatch.Type, Swatch.WidthPx, Swatch.HeightPx,
				Swatch.bColourSpecified ? *SessionDescribeColour(Swatch.Colour)
					: TEXT("<no readable colour>")));

			TestTrue(
				*FString::Printf(
					TEXT("'%s' must fill its swatch with the model's own %s — the palette chip and the "
						 "piece it lays are one colour, decided once. It is filled %s"),
					*Button.Label, *SessionDescribeColour(SwatchColour(Button.Swatch)),
					Swatch.bColourSpecified ? *SessionDescribeColour(Swatch.Colour)
						: TEXT("<no readable colour>")),
				Swatch.bColourSpecified
					&& SessionColoursExactlyEqual(Swatch.Colour, SwatchColour(Button.Swatch)));

			if (Button.Swatch == EToolbarSwatch::Brick)
			{
				BrickSwatch = Swatch;
				bHaveBrick = true;
			}
			else if (Button.Swatch == EToolbarSwatch::Timber)
			{
				TimberSwatch = Swatch;
				bHaveTimber = true;
			}
		}

		// Different shapes too: the plank is longer and thinner than the brick (a relation, not pixels).
		if (bHaveBrick && bHaveTimber)
		{
			TestTrue(
				*FString::Printf(
					TEXT("THE TIMBER SWATCH IS A PLANK AND THE BRICK'S IS A BLOCK: %g x %g against "
						 "%g x %g. The plank must be longer and thinner"),
					TimberSwatch.WidthPx, TimberSwatch.HeightPx,
					BrickSwatch.WidthPx, BrickSwatch.HeightPx),
				TimberSwatch.WidthPx > BrickSwatch.WidthPx
					&& TimberSwatch.HeightPx < BrickSwatch.HeightPx);
		}
		else
		{
			AddError(FString::Printf(
				TEXT("the Build strip must draw both a brick swatch and a timber one for the shape "
					 "claim to be sayable; brick %d, timber %d"),
				bHaveBrick ? 1 : 0, bHaveTimber ? 1 : 0));
		}
	}

	TestWorld.End();

	return true;
}

/**
 * At Slate scale 1, the last chip of the Build strip (17 chips, the widest configuration) and of the
 * Destroy strip ends within the §b reference width of 1280 px less the bar's 10 px padding, and
 * every chip still has a caption.
 *
 * §b: the strip never scrolls or wraps. AutoWidth slots simply run off the bar; the 16-chip strip
 * once spanned x = 10..1439, putting the course stepper and Clear build off a 1280 px screen. The
 * fix is shorter captions in the model; no caption is pinned here. The non-empty caption check
 * blocks the cheap fix of blanking them.
 *
 * Measured on a 1920 px surface so nothing is clamped (a fixture check confirms the run fits it).
 * The width and padding are transcribed from §b, not read from the widget. Needs a world but never
 * ticks it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarBuildStripFitsTheReferenceWidthTest,
	"DestructionGame.World.Session.BuildStripFitsTheReferenceWidth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarBuildStripFitsTheReferenceWidthTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionSession;
	using namespace SessionToolbarPanelTestSupport;

	// §b reference width and bar edge padding.
	constexpr float ReferenceViewportWidthPx = 1280.0f;
	constexpr float BarEdgePaddingPx = 10.0f;
	constexpr float RightmostAllowedEdgePx = ReferenceViewportWidthPx - BarEdgePaddingPx;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	ADestructionGamePlayerController* const Controller =
		TestWorld.World->SpawnActor<ADestructionGamePlayerController>();

	TestNotNull(TEXT("fixture: the test world should spawn the game's player controller"), Controller);

	if (Controller == nullptr)
	{
		TestWorld.End();
		return true;
	}

	/** A chip's caption and horizontal span. */
	struct FMeasuredChip
	{
		FString Caption;
		float LeftPx = 0.0f;
		float RightPx = 0.0f;
	};

	const auto MeasureStripChips = [](ADestructionGamePlayerController& Player)
	{
		TArray<FMeasuredChip> Measured;

		for (const FArrangedWidget& Entry : SessionArrangeStrip(Player))
		{
			if (Entry.Widget->GetType() != TEXT("SButton"))
			{
				continue;
			}

			const FVector2f PositionPx = FVector2f(Entry.Geometry.GetAbsolutePosition());
			const FVector2f SizePx = FVector2f(Entry.Geometry.GetAbsoluteSize());

			FMeasuredChip& Chip = Measured.AddDefaulted_GetRef();
			Chip.Caption = SessionWidgetText(Entry.Widget);
			Chip.LeftPx = PositionPx.X;
			Chip.RightPx = PositionPx.X + SizePx.X;
		}

		return Measured;
	};

	const auto DescribeMeasured = [](const TArray<FMeasuredChip>& Measured)
	{
		if (Measured.Num() == 0)
		{
			return FString(TEXT("<no chips>"));
		}

		FString Line;

		for (int32 Index = 0; Index < Measured.Num(); ++Index)
		{
			Line += FString::Printf(
				TEXT("%s'%s'[%g..%g]"),
				Index == 0 ? TEXT("") : TEXT(", "),
				*Measured[Index].Caption, Measured[Index].LeftPx, Measured[Index].RightPx);
		}

		return Line;
	};

	// Both strips: the claim is about the widget's layout, not one mode's list.
	struct FStripCase
	{
		const TCHAR* Description;
		ESessionMode Mode;
	};

	const FStripCase Cases[] = {
		{
			TEXT("the BUILD strip — the seventeen-chip configuration, the widest the model ever draws, "
				 "and the one §b names as the measurement"),
			ESessionMode::Build,
		},
		{
			TEXT("and the DESTROY strip, which is four chips and should already fit — here so the "
				 "claim is about the strip rather than about one mode's list"),
			ESessionMode::Destroy,
		},
	};

	for (const FStripCase& Case : Cases)
	{
		const EToolbarButtonId Tab = Case.Mode == ESessionMode::Build
			? EToolbarButtonId::ModeBuild
			: EToolbarButtonId::ModeDestroy;

		if (!Controller->OnToolbarButton(Tab))
		{
			AddError(FString::Printf(
				TEXT("fixture: %s — the mode tab must be clickable for the strip to be drawn"),
				Case.Description));

			continue;
		}

		const TArray<FToolbarButton> Model =
			SessionToolbarButtons(Controller->GetSessionToolbarState());

		const TArray<FMeasuredChip> Measured = MeasureStripChips(*Controller);

		AddInfo(FString::Printf(
			TEXT("%s: %d chip(s) — %s"),
			Case.Description, Measured.Num(), *DescribeMeasured(Measured)));

		if (Measured.Num() != Model.Num() || Measured.Num() == 0)
		{
			AddError(FString::Printf(
				TEXT("fixture: %s — the strip must draw one chip per model row (%d) to be measured; it "
					 "drew %d. ToolbarPanelDrawsTheModel owns that claim"),
				Case.Description, Model.Num(), Measured.Num()));

			continue;
		}

		const FMeasuredChip& Last = Measured.Last();

		// The surface must exceed the run, or the reading is a clamp rather than a width.
		TestTrue(
			*FString::Printf(
				TEXT("fixture: %s — the run must fit inside the %g px arrange surface for its width to "
					 "be a measurement at all; its last chip ends at %g"),
				Case.Description, SessionPanelWidthPx, Last.RightPx),
			Last.RightPx < SessionPanelWidthPx - BarEdgePaddingPx);

		TestTrue(
			*FString::Printf(
				TEXT("%s: THE LAST CHIP ('%s') MUST END BY %g px — the design's %g px reference width "
					 "less the bar's %g px edge padding. It ends at %g, which puts it %g px off the "
					 "right-hand side of a %g-wide viewport, where it can never be clicked and nothing "
					 "on screen says why. The fix is SHORTER CAPTIONS in the model (§b's own -/+ and "
					 "Plate/Lintel), never a narrower test. The strip measured [%s]"),
				Case.Description, *Last.Caption, RightmostAllowedEdgePx, ReferenceViewportWidthPx,
				BarEdgePaddingPx, Last.RightPx, Last.RightPx - RightmostAllowedEdgePx,
				ReferenceViewportWidthPx, *DescribeMeasured(Measured)),
			Last.RightPx <= RightmostAllowedEdgePx);

		// Every chip must still have a caption; blanking them all would pass the width check.
		for (int32 Index = 0; Index < Measured.Num(); ++Index)
		{
			TestFalse(
				*FString::Printf(
					TEXT("%s: chip %d (the model calls it '%s') MUST STILL READ SOMETHING — a strip that "
						 "fits because its captions were emptied is a row of identical lozenges, which "
						 "is a worse UI than one that overflows. The strip measured [%s]"),
					Case.Description, Index, *Model[Index].Label, *DescribeMeasured(Measured)),
				Measured[Index].Caption.TrimStartAndEnd().IsEmpty());
		}
	}

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
