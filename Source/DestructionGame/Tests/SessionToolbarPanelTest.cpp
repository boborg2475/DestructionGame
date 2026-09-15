// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "InputCoreTypes.h"
#include "Input/Events.h"
#include "Layout/ArrangedChildren.h"
#include "Layout/ArrangedWidget.h"
#include "Layout/Geometry.h"
#include "Styling/SlateColor.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SWidget.h"
#include "Widgets/Text/STextBlock.h"

#include "Core/SessionToolbar.h"
#include "DestructionGamePlayerController.h"
#include "Tests/BrickWorldTestSupport.h"
#include "World/BuildModeComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * SESSION S4 — THE STRIP ON SCREEN DRAWS THE MODEL, HONOURS bActive AND bEnabled, KEEPS ITS HANDS
 * OFF THE KEYBOARD, AND ROUTES A CLICK BACK THROUGH THE ONE DOOR.
 *
 * =====================================================================================
 * WHY A HEADLESS SLATE TEST IS POSSIBLE AT ALL
 * =====================================================================================
 *
 * `BuildPieceMenuPanel`'s header already sets this out and `Tests/PieceMenuPanelLayoutTest.cpp`
 * already relies on it: Slate layout is arithmetic on desired sizes and alignments, so a widget tree
 * can be prepassed and arranged with no window, no renderer and no RHI. The whole value of making
 * `BuildSessionToolbarPanel()` PUBLIC is that the strip's wiring — which buttons, in what order,
 * greyed or not, focusable or not, and what a click calls — becomes reachable. Kept private behind
 * `AddViewportWidgetContent` it would be the one surface nothing can see, which is exactly where the
 * two most likely defects in this design live.
 *
 * =====================================================================================
 * WHAT IS ASSERTED, AND WHY EACH ROW IS THE ONE THAT BITES
 * =====================================================================================
 *
 *   - THE CAPTIONS, IN ORDER, AGAINST `SessionToolbarButtons`. Not against literals: the model owns
 *     the wording and `Core.SessionToolbar.*` owns whether the wording is right. What this owns is
 *     that the widget draws THAT list, in THAT order — the ordering rule is a player-facing promise
 *     (the mode pair may not move under a stationary cursor) and a run of `AddSlot` calls is exactly
 *     where it would quietly stop being true.
 *
 *   - `IsEnabled()` AGAINST `bEnabled`. The model already refuses a greyed button, so a strip that
 *     drew everything live would still be SAFE — and would tell the player that clicking Run on an
 *     empty plot is going to do something. A lit button that does nothing is the failure
 *     `FToolbarButton::bEnabled`'s own header names.
 *
 *   - `SupportsKeyboardFocus() == false` ON EVERY BUTTON. This is the design's own nomination for
 *     the most likely thing to get wrong (§d, item 5), and it is invisible in every other kind of
 *     test: a focusable `SButton` takes user focus on click and the flying pawn stops answering `W`.
 *     A human would report it as "the game froze". It is one line per button and nothing else in the
 *     suite can see it.
 *
 *   - `bActive` IS HONOURED BY SOMETHING A WIDGET CAN READ. Which green, which glow and which
 *     gradient are the widget's business, so the claim is deliberately a DISJUNCTION: the caption's
 *     font or the chip's background colour must differ between the lit button and an idle one. What
 *     it is not free to do is draw them identically, which would make the mode the player is in
 *     unreadable — and `EBrickHighlight`'s ten enumerators are the precedent for why `bActive` and
 *     `bEnabled` must not be drawn alike either.
 *
 *   - AND A REAL CLICK REACHES `OnToolbarButton`. `SButton::SimulateClick` runs the actual OnClicked
 *     delegate, so this is the widget→controller wire itself rather than a second call to the
 *     controller's own method. Without it every claim in this file is about a tree nobody can press.
 *
 * WHAT IS DELIBERATELY NOT PINNED: any pixel. Sizes, paddings, the 48 px height, the group rules and
 * the accent hues are all the widget's, and `Presenter.SessionSafeArea` is where the one layout
 * number that matters to another surface will live.
 *
 * NEEDS A TICKING WORLD: a world, because the controller is an actor and `OnToolbarButton(ModeBuild)`
 * opens a real structure on the subsystem. It never ticks one, and it never needs an RHI — this runs
 * green under `-nullrhi` like every other test in the suite.
 *
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges many
 * files into one. See CURRENT_STATE.md.
 */
namespace SessionToolbarPanelTestSupport
{
	using namespace DestructionSession;

	/**
	 * The space the strip is laid out in, in pixels.
	 *
	 * 1920 x 1080 is an ordinary viewport, and NOTHING HERE DEPENDS ON THE NUMBERS. No claim in this
	 * file is about where anything landed — the geometry exists only because `ArrangeChildren` needs
	 * one to hand its children, and it is `ArrangeChildren` that walks the tree in SLOT ORDER, which
	 * is the order claim.
	 */
	constexpr float SessionPanelWidthPx = 1920.0f;
	constexpr float SessionPanelHeightPx = 1080.0f;

	FGeometry SessionPanelRootGeometry()
	{
		return FGeometry::MakeRoot(
			FVector2f(SessionPanelWidthPx, SessionPanelHeightPx), FSlateLayoutTransform());
	}

	/** Every STextBlock under a widget, run together — which for a button is its caption. */
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

	/** The first STextBlock under a widget, which for a button is the widget its caption is set on. */
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

	/**
	 * A font, as a string nothing can accidentally compare equal across two different fonts.
	 *
	 * THE FACE, THE TYPEFACE NAME AND THE SIZE, because the design's own distinction between an
	 * active caption and an idle one is a WEIGHT (`Bold` against `Regular`), which lives in the
	 * typeface name rather than in the size.
	 */
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
	 * A chip's fill, as a string.
	 *
	 * AN UNSPECIFIED COLOUR IS ITS OWN ANSWER rather than a read of `GetSpecifiedColor`, which is not
	 * meaningful for a colour that defers to the style. Two buttons both deferring to the style read
	 * the same, which is the honest answer: they look the same, and if that is how the strip draws
	 * `bActive` then the font had better be doing the work instead.
	 */
	FString SessionColourBits(const FSlateColor& Colour)
	{
		return Colour.IsColorSpecified()
			? Colour.GetSpecifiedColor().ToString()
			: FString(TEXT("<from the style>"));
	}

	/** One button of the drawn strip: what it reads, whether it is live, and how it looks. */
	struct FSessionChip
	{
		FString Caption;
		bool bEnabled = false;
		bool bFocusable = true;

		/**
		 * EVERYTHING A TEST CAN READ ABOUT HOW IT IS DRAWN, in one string.
		 *
		 * THE FONT AND THE FILL TOGETHER, because the claim is a DISJUNCTION — the widget may draw
		 * `bActive` with either, and pinning one would be choosing its visual design for it. Joined
		 * rather than compared field by field so "these two look different" is one comparison.
		 */
		FString Look;
	};

	/**
	 * Lay the tree out and record every button in SLOT ORDER.
	 *
	 * ARRANGED RATHER THAN PAINTED, exactly as `Tests/PieceMenuPanelLayoutTest.cpp` does it:
	 * `ArrangeChildren` is the call the renderer makes to decide where a child goes, it is const, and
	 * it needs no device. Walking it depth-first visits an `SHorizontalBox`'s slots in the order they
	 * were added, which is the left-to-right order a player sees.
	 */
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
				*SessionColourBits(Button->GetBorderBackgroundColor()));
		}

		FArrangedChildren Arranged(EVisibility::All);

		Widget->ArrangeChildren(Geometry, Arranged);

		for (int32 Index = 0; Index < Arranged.Num(); ++Index)
		{
			SessionCollectChips(Arranged[Index].Widget, Arranged[Index].Geometry, Out);
		}
	}

	/** Build the strip for the controller's CURRENT state and read every chip off it. */
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

	/**
	 * Hold the drawn strip against the model that produced it: same buttons, same order, same greying,
	 * and none of them stealing the keyboard.
	 */
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

	/**
	 * EVERY WIDGET IN THE TREE, DEPTH FIRST, EACH WITH THE GEOMETRY IT WAS LAID OUT IN.
	 *
	 * THE GEOMETRY IS THE HALF SessionCollectChips THROWS AWAY, and two of the claims below need it:
	 * a mouse event has to be handed the geometry of the widget it is being delivered to, and "a
	 * point on the bar that is not on any chip" is a question about where the chips actually landed.
	 * Pre-order is the same walk, so the order of this list is still the left-to-right order a
	 * player sees.
	 */
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

	/** Whether any SButton lives under this widget — which is what makes a border THE BAR. */
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

	/**
	 * THE BAR ITSELF: the outermost SBorder that has the chips inside it.
	 *
	 * FOUND BY THE TREE RATHER THAN BY A PIXEL. The bar is the border filling the strip's box at the
	 * bottom of the screen, and this file pins no sizes — so "the border the buttons are inside" is
	 * the description that stays true when the height, the padding or the accent changes. Pre-order
	 * makes the first match the OUTERMOST one, which is the bar rather than any decorative border
	 * that might one day sit around a group of chips inside it.
	 */
	const FArrangedWidget* SessionFindBar(const TArray<FArrangedWidget>& Arranged)
	{
		return Arranged.FindByPredicate(
			[](const FArrangedWidget& Entry)
			{
				return Entry.Widget->GetType() == TEXT("SBorder")
					&& SessionHasButtonDescendant(Entry.Widget);
			});
	}

	/**
	 * A LEFT-BUTTON EVENT AT AN ABSOLUTE SCREEN POINT.
	 *
	 * THE PRESSED SET CARRIES THE BUTTON ON THE WAY DOWN AND IS EMPTY ON THE WAY UP, which is what
	 * Slate itself delivers: the set is the buttons held AFTER the transition. Nothing under test
	 * reads it, and it is spelled correctly anyway so that a handler which ever does starts from a
	 * real event rather than from a convenient one.
	 */
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

	/** Every STextBlock in the strip, in slot order — captions AND anything that is not one. */
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

	/** The SButton reading exactly this caption, found in the live tree so it can be pressed. */
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

	/* --- ONE: the Destroy strip, which is what every scenario level opens with ------------- */

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

		/*
		 * AND THE GREYING IS NOT VACUOUS. The sweep above compares the strip against the model, which
		 * is satisfied by a model with nothing greyed and a strip with nothing greyed. With nothing
		 * built, Run structure must be one of the greyed ones — asserted of the MODEL so that a
		 * failure says which of the two halves is wrong.
		 */
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

	/* --- TWO: the Build strip, which is the ten-chip configuration ------------------------- */

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

		/*
		 * THE COUNT IS ASSERTED AGAINST A LITERAL AS WELL AS AGAINST THE MODEL, because the two
		 * configurations differing at all is the claim: a strip rebuilt from a state it never re-read
		 * would draw the same three buttons in both modes and match the model in neither.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("fixture: the Build strip is the ten-chip configuration; the model offers %d "
					 "[%s]"),
				Model.Num(), *SessionDescribeModel(Model)),
			Model.Num() == 10);

		SessionCheckStripMatchesModel(*this, TEXT("Build mode"), BuildChips, Model);
	}

	/* --- THREE: bActive is honoured by something a widget can read ------------------------- */

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

		/*
		 * WITHIN ONE STRIP: the mode you are in must not look like the mode you are not in. This is
		 * the whole of what "which mode am I in" means from peripheral vision.
		 */
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
		 * ACROSS THE TWO STRIPS: each tab's own appearance CHANGED when it became the active one.
		 *
		 * THIS IS THE ROW THAT MAKES THE TWO ABOVE ABOUT bActive RATHER THAN ABOUT POSITION. A strip
		 * that drew its FIRST chip one way and its second another — a decorative alternation, or a
		 * "first slot is special" style — would satisfy both of them forever while telling the player
		 * nothing about which mode they are in.
		 *
		 * IT IS DELIBERATELY NOT A SWAP. The design gives each mode its own accent (build amber,
		 * destroy red), so the Build tab's lit look is not required to equal the Destroy tab's lit
		 * look; insisting on that would be choosing the palette here.
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

	/* --- FOUR: and a real click on the widget reaches the one door ------------------------- */

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

			/*
			 * THE WIRE ITSELF. Every other claim in this file is about a tree nobody pressed; this is
			 * the one that says pressing it does anything at all — and that what it does goes through
			 * the controller's single door rather than setting a field beside it.
			 */
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
 * THE BAR SWALLOWS THE CLICK. A press on the strip's own background, three pixels beside a chip, is
 * the toolbar's — not the world's.
 *
 * =====================================================================================
 * THE BEHAVIOUR IN ONE SENTENCE
 * =====================================================================================
 *
 * The border that carries the chips must report a left-button press and release on itself as
 * HANDLED, so that a click landing on the strip's background goes no further.
 *
 * =====================================================================================
 * WHY THIS IS A DEFECT AND NOT A NICETY
 * =====================================================================================
 *
 * An SBorder with nothing bound answers `FReply::Unhandled()` — SWidget::OnMouseButtonDown returns
 * exactly that unless a handler has been set on it. Slate then bubbles the press on up to the
 * `SViewport`, which is where the game's input stack lives, and `IA_InspectPiece` fires. In Destroy
 * mode that is a ray at a brick the player did not mean to select; in BUILD mode the same click
 * reaches `PrimaryAlongRay`, which LAYS A BRICK wherever that pixel's ray happens to meet the build
 * plane — a piece of masonry appearing on the far side of the plot because the player missed
 * `Course up` by three pixels.
 *
 * The chips themselves are already safe: `SButton` handles its own press. It is the GAPS that leak —
 * the 5 px between every pair of chips, the 10 px edge padding, and the whole right-hand run of the
 * bar past the last chip, which on a 1920-wide viewport is most of its width.
 *
 * =====================================================================================
 * LEFT BUTTON ONLY, DELIBERATELY
 * =====================================================================================
 *
 * The right button is NOT asserted, and that is a decision rather than an omission. Right-drag is
 * the look chord: `IMC_MouseLook` is what the camera is flown with, and SESSION_UI_DESIGN's S6
 * permanent-cursor scheme keeps the cursor up for the whole session — so a player who starts a
 * look-drag with the pointer resting over the bar must still get their camera. A bar that swallowed
 * every button would take that away, and "the strip ate my mouse look" is a worse bug than the one
 * this test is about. Left is the click that commits things, and left is what is claimed.
 *
 * =====================================================================================
 * THE POINT IS CHOSEN, THEN PROVED TO BE THE RIGHT KIND OF POINT
 * =====================================================================================
 *
 * Four pixels inside the bar's right edge, vertically centred. That is inside the bar (asserted
 * against the bar's own geometry) and over NO chip (asserted against every arranged SButton), which
 * is what makes the claim about the BACKGROUND rather than about a button that would have handled it
 * anyway. Both are fixture preconditions rather than the claim, so a strip that one day filled its
 * whole width with chips fails here — saying "pick a different point" — instead of going green for
 * the wrong reason.
 *
 * AND THE BAR MUST STILL BE HIT-TESTABLE, which is the other half of the same promise: a handler
 * bound to a widget Slate never routes to swallows nothing at all. That row passes today and is
 * here so the pair cannot drift apart.
 *
 * NEEDS A TICKING WORLD: a world, because the controller is an actor. It never ticks one, and the
 * events are delivered by calling the widget directly — no SlateApplication, no window, no RHI.
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

	/*
	 * IN BUILD MODE, BECAUSE BUILD IS WHERE THE LEAK COSTS THE MOST — a stray click lays a brick
	 * rather than merely selecting one. It is also the ten-chip strip, so there is more bar to miss.
	 */
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

	/* Four pixels inside the right edge, vertically centred: bar, and no chip. */
	const FVector2f PressAtPx =
		FVector2f(Bar->Geometry.LocalToAbsolute(FVector2f(BarSizePx.X - 4.0f, BarSizePx.Y * 0.5f)));

	AddInfo(FString::Printf(
		TEXT("the bar is %g x %g at absolute (%g, %g); the press is at (%g, %g)"),
		BarSizePx.X, BarSizePx.Y,
		Bar->Geometry.GetAbsolutePosition().X, Bar->Geometry.GetAbsolutePosition().Y,
		PressAtPx.X, PressAtPx.Y));

	/* --- THE FIXTURE PRECONDITIONS: on the bar, and on none of its chips ------------------- */

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

	/* --- AND THE BAR IS SOMETHING SLATE WOULD ROUTE TO AT ALL ------------------------------ */

	TestTrue(
		*FString::Printf(
			TEXT("the bar must be HIT-TESTABLE: a handler bound to a widget Slate routes nothing to "
				 "swallows nothing. Its visibility reads %s"),
			*Bar->Widget->GetVisibility().ToString()),
		Bar->Widget->GetVisibility().IsHitTestVisible());

	/* --- THE CLAIM: press and release on the background are the toolbar's ------------------ */

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

		/*
		 * AND THE RELEASE TOO, BECAUSE A SWALLOWED PRESS IS ONLY HALF A CLICK. Enhanced Input reads
		 * key-up as well as key-down — a released action, a chord ending — so a bar that ate the press
		 * and let the release through would deliver half a click to the world with nothing having
		 * started it.
		 */
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
 * THE COURSE STEPPER SAYS WHICH COURSE YOU ARE ON — a readout between the two arrows, in Build mode
 * only, that follows the clicks.
 *
 * =====================================================================================
 * THE BEHAVIOUR IN ONE SENTENCE
 * =====================================================================================
 *
 * The Build strip draws `DestructionSession::CourseLabel(State.Course)` as its own text, positioned
 * between the `Course down` and `Course up` captions; the Destroy strip draws no such thing; and two
 * accepted `CourseUp` clicks make it read "Course 2".
 *
 * =====================================================================================
 * WHY A PAIR OF UNLABELLED ARROWS IS A DEFECT
 * =====================================================================================
 *
 * The course is the build plane's height, and NOTHING ELSE ON SCREEN SAYS WHAT IT IS. The ghost
 * moves, but a brick at course 4 and a brick at course 5 look identical from a flying camera at any
 * distance, and `Course down` greys out only at the very bottom — so a player who has stepped up
 * five times and lost count has no way to get back except to click down until the button dies. The
 * model already words it: `CourseLabel` exists, is swept by `Core.SessionToolbar.*`, and is drawn
 * by nobody.
 *
 * =====================================================================================
 * THE ORDER IS THE CLAIM, NOT MERELY THE PRESENCE
 * =====================================================================================
 *
 * A readout tacked onto the end of the strip would satisfy "the text is there" and would be a
 * different control: a stepper is an arrow, a value and an arrow, and the value belongs BETWEEN its
 * two arrows because that is what makes the two arrows read as acting on it. So the assertion is on
 * the readout's INDEX in the strip's text list, strictly between the two captions'.
 *
 * =====================================================================================
 * AND IT IS NOT A BUTTON
 * =====================================================================================
 *
 * `ToolbarPanelDrawsTheModel` holds the drawn chips against `SessionToolbarButtons` one for one, and
 * the model has no row for a readout — so a readout drawn as an eleventh SButton would break that
 * test AND would be a lit control that does nothing when pressed. This test pins the other side of
 * that: the chip list is still exactly the model's, and none of the chips reads the readout. The
 * caption walk already reads text only from under an SButton, so a text slot of its own is invisible
 * to it, which is exactly the design the readout wants.
 *
 * NEEDS A TICKING WORLD: a world, because the controller is an actor and entering Build mode opens a
 * structure on the subsystem. It never ticks one.
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

	/*
	 * THE WORDS COME OFF THE MODEL, NEVER OFF A LITERAL. Core.SessionToolbar.* owns whether "Course 0"
	 * is the right wording; what this file owns is that the strip draws THAT string.
	 */
	const FString CourseZero = CourseLabel(0);
	const FString CourseTwo = CourseLabel(2);

	/*
	 * AND SO DO THE TWO ARROWS' CAPTIONS, asked of a Build-mode state rather than transcribed — the
	 * strip this file is about is the model's list, so the two ends of the "between" claim have to be
	 * the model's words too or a retune of either caption would leave this test looking for a button
	 * that no longer exists.
	 */
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

	/* --- ONE: the Destroy strip has no course readout on it ------------------------------- */

	{
		TestTrue(
			TEXT("fixture: a fresh controller must be in Destroy mode for this arm to be the Destroy "
				 "strip"),
			Controller->GetSessionToolbarState().Mode == ESessionMode::Destroy);

		const TArray<FString> Texts = SessionStripTexts(SessionArrangeStrip(*Controller));

		AddInfo(FString::Printf(TEXT("the Destroy strip reads [%s]"), *SessionDescribeTexts(Texts)));

		/*
		 * A READOUT IN DESTROY MODE WOULD BE A LIE ABOUT WHAT THE SESSION IS DOING. There is no build
		 * plane in Destroy mode and no arrows to move it, so a number reading "Course 0" beside a Run
		 * button describes a thing the player cannot see or change.
		 */
		TestEqual(
			FString::Printf(
				TEXT("THE DESTROY STRIP MUST NOT CARRY THE COURSE READOUT: it has no course stepper to "
					 "read out. It draws [%s]"),
				*SessionDescribeTexts(Texts)),
			Texts.IndexOfByKey(CourseZero), static_cast<int32>(INDEX_NONE));
	}

	/* --- TWO: the Build strip carries it, BETWEEN the two arrows --------------------------- */

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
			/*
			 * BETWEEN THE ARROWS, WHICH IS WHAT MAKES THEM READ AS ACTING ON IT. A value at the end of
			 * the strip is a different control altogether.
			 */
			TestTrue(
				*FString::Printf(
					TEXT("AND IT MUST SIT BETWEEN THE TWO ARROWS — arrow, value, arrow is what a stepper "
						 "is. '%s' is at %d, '%s' at %d, '%s' at %d in [%s]"),
					*DownCaption, DownIndex, *CourseZero, ReadoutIndex, *UpCaption, UpIndex,
					*SessionDescribeTexts(Texts)),
				DownIndex < ReadoutIndex && ReadoutIndex < UpIndex);
		}

		/*
		 * AND IT IS A TEXT SLOT OF ITS OWN RATHER THAN AN ELEVENTH CHIP. The model has no row for it,
		 * so a readout drawn as a button is a lit control that does nothing when it is pressed — and it
		 * would put the drawn strip out of step with SessionToolbarButtons, which is the one-for-one
		 * claim ToolbarPanelDrawsTheModel makes.
		 */
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

	/* --- THREE: and it FOLLOWS the clicks -------------------------------------------------- */

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

		/*
		 * A READOUT BUILT ONCE AND NEVER REBUILT IS WORSE THAN NONE — it would tell the player they are
		 * on course 0 while the build plane sat two courses up, and the brick would land somewhere they
		 * were told it would not. Both halves are asserted: the new reading is there AND the old one
		 * is gone.
		 */
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

#endif // WITH_DEV_AUTOMATION_TESTS
