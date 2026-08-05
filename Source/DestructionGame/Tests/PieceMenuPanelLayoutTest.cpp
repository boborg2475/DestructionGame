// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceMenu.h"
#include "DestructionGamePlayerController.h"
#include "Layout/ArrangedChildren.h"
#include "Layout/Geometry.h"
#include "Tests/BrickWorldTestSupport.h"
#include "Widgets/SWidget.h"
#include "Widgets/Text/STextBlock.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges
 * many files into one. See CURRENT_STATE.md; the `using namespace` lives inside RunTest for the
 * same reason. The world harness is NOT redeclared here: it lives in
 * Tests/BrickWorldTestSupport.h and every World.* test shares it.
 */
namespace PieceMenuPanelLayoutTestSupport
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;

	/**
	 * How far along Y a ray starts and ends, either side of the wall.
	 *
	 * A brick is 10.25 cm deep and a wall is centred on Y = 0, so +/- 100 cm is far outside it
	 * on both sides and the ray crosses the whole thickness. Along Y rather than X or Z so
	 * nothing else in the wall is in the way and the answer is unambiguous.
	 */
	constexpr double PanelRayReachCm = 100.0;

	/**
	 * The size of the space the panel is laid out in, in pixels.
	 *
	 * A ROOT GEOMETRY RATHER THAN A REAL VIEWPORT, WHICH IS THE WHOLE POINT OF THIS FILE. Slate
	 * layout is arithmetic on desired sizes and alignments; it needs no renderer, no window and
	 * no RHI, so the one inch of the piece menu that used to be unreachable — where on screen
	 * each button ends up — can be measured headlessly by prepassing the tree and arranging it
	 * in a geometry of a stated size.
	 *
	 * 1920 x 1080 is an ordinary viewport. NOTHING BELOW DEPENDS ON THE NUMBERS: the assertion
	 * is that a row does not MOVE between two states laid out in the same space, so a different
	 * size moves both measurements together.
	 */
	constexpr float PanelViewportWidthPx = 1920.0f;
	constexpr float PanelViewportHeightPx = 1080.0f;

	/**
	 * How far a menu entry is allowed to move when the readout beside it changes. It is slack
	 * on an exact equality rather than a threshold: two layouts of the same tree in the same
	 * geometry produce bit-identical offsets, and the defect this file is about moves an entry
	 * by half the readout's growth — tens of pixels against a row about 20 px tall.
	 */
	constexpr double PanelRowMustNotMovePx = 0.5;

	/**
	 * Slack on the horizontal containment claim, in pixels.
	 *
	 * Same reasoning as the vertical slack above and not a measurement tolerance: two layouts of
	 * one tree in one geometry are bit-identical, and the shape this would catch — a row that
	 * shrink-wraps or re-anchors and slides out from under the cursor sideways — is tens of
	 * pixels wide.
	 */
	constexpr double PanelSpanContainmentSlackPx = 0.5;

	FVector PanelRayStart(const FPieceBox& Box)
	{
		return FVector(Box.CentreCm.X, Box.CentreCm.Y - PanelRayReachCm, Box.CentreCm.Z);
	}

	FVector PanelRayEnd(const FPieceBox& Box)
	{
		return FVector(Box.CentreCm.X, Box.CentreCm.Y + PanelRayReachCm, Box.CentreCm.Z);
	}

	FPieceRef PanelRef(int32 StructureId, int32 PieceIndex)
	{
		FPieceRef Ref;
		Ref.StructureId = StructureId;
		Ref.PieceIndex = PieceIndex;

		return Ref;
	}

	/**
	 * One button of the drawn panel: what it reads, where it ended up, and how big it is.
	 *
	 * THE SIZE IS HERE FOR THE HORIZONTAL CLAIM, WHICH IS ABOUT SPANS RATHER THAN ABOUT ORIGINS.
	 * The panel is centred horizontally, so a row's left edge moves whenever the readout widens;
	 * that is harmless only for as long as the narrower span stays INSIDE the wider one, because
	 * then the cursor is still over the same button. An origin on its own cannot say that.
	 */
	struct FPanelButton
	{
		FString Label;
		FVector2D TopLeftPx = FVector2D::ZeroVector;
		FVector2D SizePx = FVector2D::ZeroVector;
	};

	/**
	 * Every STextBlock under a widget, run together — which for a button is its caption.
	 *
	 * THE BUTTON IS IDENTIFIED BY WHAT IT SAYS RATHER THAN BY ITS SLOT INDEX, because a slot
	 * index is a claim about the panel's structure and the fix under test is allowed to change
	 * that structure. What must not change is where the row a player's cursor is resting on
	 * ends up, and the row a cursor rests on is the one they can read.
	 */
	FString PanelWidgetText(const TSharedRef<SWidget>& Widget)
	{
		FString Text;

		if (Widget->GetType() == TEXT("STextBlock"))
		{
			Text += StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString();
		}

		FChildren* const Children = Widget->GetChildren();

		for (int32 Index = 0; Children != nullptr && Index < Children->Num(); ++Index)
		{
			Text += PanelWidgetText(Children->GetChildAt(Index));
		}

		return Text;
	}

	/**
	 * Lay the tree out in this geometry and record where every button landed.
	 *
	 * ARRANGED RATHER THAN PAINTED. ArrangeChildren is the same call the renderer makes to
	 * decide where a child goes, and it is const and free of any device — so walking it gives
	 * the absolute position of every button with no window and no RHI. Cached geometry would
	 * not do: nothing caches until something paints.
	 */
	void CollectPanelButtons(
		const TSharedRef<SWidget>& Widget,
		const FGeometry& Geometry,
		TArray<FPanelButton>& Out)
	{
		if (Widget->GetType() == TEXT("SButton"))
		{
			FPanelButton& Button = Out.AddDefaulted_GetRef();
			Button.Label = PanelWidgetText(Widget);
			Button.TopLeftPx = FVector2D(Geometry.GetAbsolutePosition());
			Button.SizePx = FVector2D(Geometry.GetAbsoluteSize());
		}

		FArrangedChildren Arranged(EVisibility::All);

		Widget->ArrangeChildren(Geometry, Arranged);

		for (int32 Index = 0; Index < Arranged.Num(); ++Index)
		{
			CollectPanelButtons(Arranged[Index].Widget, Arranged[Index].Geometry, Out);
		}
	}

	FString DescribePanelButtons(const TArray<FPanelButton>& Buttons)
	{
		if (Buttons.Num() == 0)
		{
			return TEXT("<no buttons>");
		}

		FString Line;

		for (int32 Index = 0; Index < Buttons.Num(); ++Index)
		{
			Line += FString::Printf(
				TEXT("%s'%s'@(%.2f, %.2f) %.2f x %.2f px"),
				Index == 0 ? TEXT("") : TEXT(", "),
				*Buttons[Index].Label,
				Buttons[Index].TopLeftPx.X,
				Buttons[Index].TopLeftPx.Y,
				Buttons[Index].SizePx.X,
				Buttons[Index].SizePx.Y);
		}

		return Line;
	}

	const FPanelButton* FindPanelButton(const TArray<FPanelButton>& Buttons, const FString& Label)
	{
		return Buttons.FindByPredicate(
			[&Label](const FPanelButton& Button) { return Button.Label == Label; });
	}

	/**
	 * How many joints touch a piece, counted straight off the graph.
	 *
	 * A FIXTURE PRECONDITION, NOT AN ASSERTION ABOUT THE PANEL. The readout grows by one line
	 * per joint, so a brick with too few joints would make the growth too small to move a row
	 * out from under a cursor — and the test would pass while the defect stood.
	 */
	int32 JointsTouchingPiece(const FStructure& Structure, int32 Handle)
	{
		int32 Count = 0;

		for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
		{
			const FConnection& Connection = Structure.GetConnection(Index);

			if (Connection.PieceA == Handle || Connection.PieceB == Handle)
			{
				++Count;
			}
		}

		return Count;
	}

	/**
	 * THE FIXTURE BOTH TESTS IN THIS FILE MEASURE, BUILT IN ONE PLACE RATHER THAN IN TWO RunTest
	 * BODIES. The pair differs only in WHICH rows it holds still, and the pair is only evidence
	 * about that difference for as long as everything else is the same wall, the same three
	 * bricks picked and the same answer to "does the brick singled out have enough joints to
	 * grow the readout by". Two copies of that setup are two chances for one of them to drift
	 * into being a wall the defect cannot show up in.
	 */
	struct FPanelFixture
	{
		FBrickTestWorld TestWorld;
		FBrickLayout Reference;
		FStructureBinding* Binding = nullptr;
		ADestructionGamePlayerController* Controller = nullptr;

		/** The brick singled out, and the joint count the readout grows by. */
		FPieceRef InspectedRef;
		int32 InspectedJoints = 0;

		bool bWorldBegun = false;

		bool Begin(FAutomationTestBase& Test)
		{
			const FRunningBondSpec Spec = WallSpec();

			/* The reference layout is laid separately, so the points pointed at come from the producer. */
			Test.TestTrue(
				TEXT("fixture: RunningBond should lay the reference wall"), RunningBond(Spec, Reference));

			if (Reference.Boxes.Num() != WallPieceCount)
			{
				Test.AddError(FString::Printf(
					TEXT("fixture: the shared wall spec should be %d pieces, got %d"),
					WallPieceCount, Reference.Boxes.Num()));

				return false;
			}

			if (!TestWorld.Begin(Test))
			{
				return false;
			}

			bWorldBegun = true;

			const int32 StructureId = TestWorld.Subsystem->BuildRunningBond(Spec);
			Binding = TestWorld.Subsystem->Find(StructureId);

			Test.TestNotNull(
				*FString::Printf(
					TEXT("fixture: BuildRunningBond returned %d and Find should hand back its binding"),
					StructureId),
				Binding);

			if (Binding == nullptr || Binding->NumPieces() != WallPieceCount)
			{
				return false;
			}

			Binding->SolveLoads();

			Controller = TestWorld.World->SpawnActor<ADestructionGamePlayerController>();

			Test.TestNotNull(
				TEXT("fixture: the test world should spawn the game's player controller"), Controller);

			if (Controller == nullptr)
			{
				return false;
			}

			/*
			 * THREE BRICKS PICKED, WHICH IS THREE ENTRY ROWS. One row cannot show the defect: it
			 * is the rows ABOVE the readout that move, and with a single entry there is barely a
			 * stack to shift.
			 */
			for (int32 Piece = 0; Piece <= 2; ++Piece)
			{
				Controller->InspectAlongRay(
					PanelRayStart(Reference.Boxes[Piece]), PanelRayEnd(Reference.Boxes[Piece]));
			}

			Test.TestEqual(
				FString::Printf(
					TEXT("fixture: three clicks should have picked three bricks, the selection holds %d"),
					Controller->GetPieceSelection().Num()),
				Controller->GetPieceSelection().Num(), 3);

			/*
			 * THE BRICK SINGLED OUT HAS TO HAVE ENOUGH JOINTS TO MAKE THE READOUT GROW. Brick 1 of
			 * this flush wall is spanned by two bricks above and has a neighbour either side, so
			 * the count is asserted rather than assumed — a wall shape that changed it would
			 * otherwise quietly make the growth too small to move a row and both tests would pass
			 * over a live defect.
			 */
			InspectedRef = PanelRef(StructureId, 1);

			InspectedJoints = JointsTouchingPiece(Binding->GetStructure(), Binding->ResolvePiece(InspectedRef));

			Test.TestTrue(
				FString::Printf(
					TEXT("fixture: the brick singled out must have at least 3 joints for the readout to grow by, it has %d"),
					InspectedJoints),
				InspectedJoints >= 3);

			return InspectedJoints >= 3;
		}

		void End()
		{
			if (bWorldBegun)
			{
				TestWorld.End();
				bWorldBegun = false;
			}
		}
	};

	/** One laid-out state of the panel: what it measured to, and where every button landed. */
	struct FPanelLayoutState
	{
		FVector2D DesiredSizePx = FVector2D::ZeroVector;
		TArray<FPanelButton> Buttons;
	};

	/**
	 * Prepass the tree and arrange it, which is the whole measurement.
	 *
	 * THE PANEL IS BUILT ONCE AND MEASURED TWICE BY CALLING THIS TWICE. SetInspectedPiece swaps
	 * only the readout box's content, so both states are the very same widget tree — which is
	 * what makes "the row moved" a claim about layout rather than about two different panels.
	 */
	FPanelLayoutState MeasurePanel(const TSharedRef<SWidget>& Panel, const FGeometry& Root)
	{
		Panel->SlatePrepass(1.0f);

		FPanelLayoutState State;
		State.DesiredSizePx = FVector2D(Panel->GetDesiredSize());

		CollectPanelButtons(Panel, Root, State.Buttons);

		return State;
	}

	FGeometry PanelRootGeometry()
	{
		return FGeometry::MakeRoot(
			FVector2f(PanelViewportWidthPx, PanelViewportHeightPx), FSlateLayoutTransform());
	}
}

/**
 * SINGLING OUT A BRICK MUST NOT MOVE THE MENU ENTRY THE CURSOR IS SITTING ON.
 *
 * THE BUG THIS IS ABOUT IS A TWO-FRAME OSCILLATION, AND IT NEEDS NO MOUSE MOVEMENT AT ALL.
 * Hovering an entry row calls SetInspectedPiece, which swaps a joint breakout into the readout
 * box — three to six extra lines for a running-bond brick. The panel is wrapped in a
 * vertically-CENTRED box, so it grows about its centre and every entry row above the readout
 * moves UP by half the growth: tens of pixels, against a default SButton row about 20 px tall.
 * The cursor, which has not moved, is now off the row. Slate then routes an OnMouseLeave — it
 * synthesises a cursor move every non-sleeping tick precisely because "the UI can change even
 * if the mouse doesn't move" — the readout empties, the panel re-centres, the row comes back
 * under the cursor, and the next tick starts again. The readout strobes and the brick strobes
 * Inspected <-> Selected at 60 Hz.
 *
 * SWAPPING THE READOUT'S CONTENT RATHER THAN REBUILDING THE PANEL DEFENDS THE WRONG HALF. That
 * defence stops the hovered BUTTON being destroyed; the loop does not need it destroyed, only
 * moved.
 *
 * THE ASSERTION IS THE ENTRY ROW'S ARRANGED OFFSET, NOT THE PANEL'S DESIRED HEIGHT. Desired
 * height is deliberately NOT asserted: a fix that top-aligns the panel leaves the height growing
 * and still stops every entry moving, and a test that demanded a constant height would rule that
 * fix out for no reason a player could see. Nor is it the ACTION rows: those sit below the
 * readout, nothing about hovering one changes the readout, and a row that moves without feeding
 * back into what moved it cannot oscillate. The property is exactly the one the loop turns on —
 * THE ROW THAT CHANGED THE READOUT IS STILL WHERE IT WAS.
 *
 * NEEDS A WORLD, NEVER TICKS ONE, AND NEEDS NO RHI. The world is there for a wall to point at
 * and a subsystem to read a binding out of, which is what makes the readout have any content to
 * grow by. The layout itself is arithmetic: SlatePrepass computes desired sizes and
 * ArrangeChildren places them, neither of which touches a renderer, a window or a viewport —
 * which is why extracting BuildPieceMenuPanel makes the whole surface reachable.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuPanelKeepsItsEntriesStillTest,
	"DestructionGame.World.Menu.InspectingAnEntryDoesNotMoveTheEntries",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuPanelKeepsItsEntriesStillTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;
	using namespace PieceMenuPanelLayoutTestSupport;

	FPanelFixture Fixture;

	if (!Fixture.Begin(*this))
	{
		Fixture.End();
		return true;
	}

	ADestructionGamePlayerController* const Controller = Fixture.Controller;
	const FPieceRef InspectedRef = Fixture.InspectedRef;

	const TSharedRef<SWidget> Panel = Controller->BuildPieceMenuPanel();

	const FGeometry Root = PanelRootGeometry();

	const FPanelLayoutState BeforeState = MeasurePanel(Panel, Root);

	const FVector2D SizeBeforePx = BeforeState.DesiredSizePx;
	const TArray<FPanelButton>& Before = BeforeState.Buttons;

	AddInfo(FString::Printf(
		TEXT("nothing inspected: panel desired size %.2f x %.2f px, buttons: %s"),
		SizeBeforePx.X, SizeBeforePx.Y, *DescribePanelButtons(Before)));

	/*
	 * FIXTURE: THE LAYOUT ACTUALLY HAPPENED. A tree that measured to nothing would put every
	 * button at the same place in both states and pass while asserting nothing, which is
	 * indistinguishable from the panel being correct.
	 */
	TestTrue(
		FString::Printf(TEXT("fixture: the panel should measure to a real height, it measured %.2f px"),
			SizeBeforePx.Y),
		SizeBeforePx.Y > 0.0);

	TestTrue(
		FString::Printf(TEXT("fixture: the panel should hold 3 entry rows plus at least one action row, it holds %d button(s): %s"),
			Before.Num(), *DescribePanelButtons(Before)),
		Before.Num() >= 4);

	const FPieceMenuInspector Inspector = Controller->PieceMenuInspectorForSelection();

	TestEqual(
		FString::Printf(TEXT("fixture: the readout model should list 3 entries, it lists %d"),
			Inspector.Pieces.Num()),
		Inspector.Pieces.Num(), 3);

	if (Inspector.Pieces.Num() != 3 || Before.Num() < 4)
	{
		Fixture.End();
		return true;
	}

	/*
	 * AND THE ROWS ARE AT DISTINCT HEIGHTS. If every button stacked at one Y the comparison
	 * below would be free, which is the other way a zero-sized layout passes for nothing.
	 */
	TestTrue(
		FString::Printf(TEXT("fixture: the entry rows should sit at different heights, they sit at %s"),
			*DescribePanelButtons(Before)),
		!FMath::IsNearlyEqual(Before[0].TopLeftPx.Y, Before[1].TopLeftPx.Y, PanelRowMustNotMovePx));

	/* Hovering entry row 1 — which is all this is — singles that brick out. */
	Controller->SetInspectedPiece(InspectedRef);

	const FPanelLayoutState AfterState = MeasurePanel(Panel, Root);

	const FVector2D SizeAfterPx = AfterState.DesiredSizePx;
	const TArray<FPanelButton>& After = AfterState.Buttons;

	AddInfo(FString::Printf(
		TEXT("brick %d:%d inspected (%d joints): panel desired size %.2f x %.2f px, buttons: %s"),
		InspectedRef.StructureId, InspectedRef.PieceIndex, Fixture.InspectedJoints,
		SizeAfterPx.X, SizeAfterPx.Y, *DescribePanelButtons(After)));

	TestEqual(
		FString::Printf(
			TEXT("singling a brick out must not add or remove buttons: %d before, %d after (%s)"),
			Before.Num(), After.Num(), *DescribePanelButtons(After)),
		After.Num(), Before.Num());

	/*
	 * THE ASSERTION. Every entry row is where it was, so the cursor that caused the readout to
	 * change is still on the row that caused it.
	 */
	for (const FInspectorPieceEntry& Entry : Inspector.Pieces)
	{
		const FPanelButton* const WasAt = FindPanelButton(Before, Entry.Label);
		const FPanelButton* const IsAt = FindPanelButton(After, Entry.Label);

		if (WasAt == nullptr || IsAt == nullptr)
		{
			AddError(FString::Printf(
				TEXT("entry row '%s' should be drawn in both states; before: %s / after: %s"),
				*Entry.Label, *DescribePanelButtons(Before), *DescribePanelButtons(After)));

			continue;
		}

		TestTrue(
			*FString::Printf(
				TEXT("entry row '%s' must not move when the readout changes: it was at y %.2f and is now at y %.2f, a shift of %.2f px (panel height %.2f -> %.2f px)"),
				*Entry.Label, WasAt->TopLeftPx.Y, IsAt->TopLeftPx.Y,
				IsAt->TopLeftPx.Y - WasAt->TopLeftPx.Y, SizeBeforePx.Y, SizeAfterPx.Y),
			FMath::IsNearlyEqual(WasAt->TopLeftPx.Y, IsAt->TopLeftPx.Y, PanelRowMustNotMovePx));
	}

	Fixture.End();

	return true;
}

/**
 * NO ROW A PLAYER CAN CLICK MOVES WHEN THE READOUT CHANGES — ENTRY ROWS *AND* ACTION ROWS.
 *
 * THE SISTER TEST ABOVE IS ABOUT AN OSCILLATION; THIS ONE IS ABOUT AN IRREVERSIBLE COMMIT, AND
 * THE ACTION ROWS ARE ONLY SAFE UNDER THE FIRST ARGUMENT. Hovering an action row changes nothing
 * that moves it, so it cannot feed back and cannot strobe — all true, and not enough. Walk the
 * cursor down off the last entry row toward Delete: leaving the entries fires OnUnhovered, the
 * readout collapses on the next tick, and Delete slides UP 60 px, toward a cursor already on its
 * way down. A click landing in that frame commits a deletion the player did not aim at, and this
 * project treats releasing a brick as irreversible — the standing rule is that the COMMIT door is
 * never wider than the menu door, and a button that moves under the cursor between aim and click
 * is that same hazard expressed in geometry rather than in a guard.
 *
 * THE PROPERTY IS OVER EVERY PRESENTED ROW, TAKEN FROM THE MODEL AND MATCHED BY CAPTION. Slot
 * index is deliberately not used: where a row sits in the panel is exactly what a fix is allowed
 * to change, and a player finds a row by reading it. Both halves of the list come from what the
 * presenter says it is showing — Inspector.Pieces and GetShownPieceMenuRows — so a fix that drew
 * a row and then failed to place it is a missing button rather than a silently skipped assertion.
 *
 * WHAT IT DELIBERATELY DOES NOT CONSTRAIN, BECAUSE TWO CANDIDATE FIXES MUST BOTH SURVIVE IT.
 * Nothing here reads the readout's slot position, the panel's total height or the order the rows
 * come out in. Moving the readout BELOW the action rows passes, because then everything clickable
 * sits above the only thing that resizes; so does reserving the readout's height with a
 * MinDesiredHeight, because then nothing resizes at all. The panel GROWING is explicitly allowed
 * and is asserted as a FIXTURE PRECONDITION rather than forbidden: a readout that cannot grow
 * cannot show a brick with many joints, and a test that pinned the height would rule out the
 * feature along with the defect.
 *
 * AND IT PINS THE HORIZONTAL ARGUMENT, WHICH UNTIL NOW WAS REASONING RATHER THAN MEASUREMENT. The
 * panel is centred, so every row's left edge moves ~59 px left as the readout widens, and the
 * claim that this is harmless is that the narrow span is CONTAINED in the wide one — the cursor
 * is over the same button either way. That holds for a full-width row under a centred panel and
 * fails the moment a row shrink-wraps or re-anchors, which is a real thing a layout fix might do
 * on its way past. It costs one comparison per row, so it is asserted rather than argued.
 *
 * NEEDS A WORLD, NEVER TICKS ONE, AND NEEDS NO RHI — same fixture, same two-state prepass and
 * arrange as the test above, and for the same reasons.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuPanelKeepsEveryClickableRowStillTest,
	"DestructionGame.World.Menu.InspectingAnEntryDoesNotMoveTheClickableRows",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuPanelKeepsEveryClickableRowStillTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;
	using namespace PieceMenuPanelLayoutTestSupport;

	FPanelFixture Fixture;

	if (!Fixture.Begin(*this))
	{
		Fixture.End();
		return true;
	}

	ADestructionGamePlayerController* const Controller = Fixture.Controller;

	const TSharedRef<SWidget> Panel = Controller->BuildPieceMenuPanel();

	const FGeometry Root = PanelRootGeometry();

	const FPanelLayoutState BeforeState = MeasurePanel(Panel, Root);

	AddInfo(FString::Printf(
		TEXT("nothing inspected: panel desired size %.2f x %.2f px, buttons: %s"),
		BeforeState.DesiredSizePx.X, BeforeState.DesiredSizePx.Y,
		*DescribePanelButtons(BeforeState.Buttons)));

	/*
	 * THE ROWS THE PLAYER CAN PUT A CURSOR ON, ASKED OF THE PRESENTER RATHER THAN READ BACK OFF
	 * THE PANEL. Reading the captions out of the drawn buttons would make the list agree with
	 * whatever was drawn, so a row that went missing would take its own assertion with it.
	 */
	struct FClickableRow
	{
		FString Label;
		const TCHAR* Kind;
	};

	TArray<FClickableRow> Clickable;

	const FPieceMenuInspector Inspector = Controller->PieceMenuInspectorForSelection();

	for (const FInspectorPieceEntry& Entry : Inspector.Pieces)
	{
		Clickable.Add({ Entry.Label, TEXT("entry") });
	}

	for (const FPieceMenuRow& Row : Controller->GetShownPieceMenuRows())
	{
		Clickable.Add({ Row.Label, TEXT("action") });
	}

	TestEqual(
		FString::Printf(TEXT("fixture: the readout model should list 3 entries, it lists %d"),
			Inspector.Pieces.Num()),
		Inspector.Pieces.Num(), 3);

	TestTrue(
		FString::Printf(
			TEXT("fixture: three picked bricks should offer at least one action row, the presenter shows %d"),
			Controller->GetShownPieceMenuRows().Num()),
		Controller->GetShownPieceMenuRows().Num() >= 1);

	/*
	 * EVERY PRESENTED ROW IS A DRAWN BUTTON AND EVERY DRAWN BUTTON IS A PRESENTED ROW. An equality
	 * rather than a floor, because a panel with an extra unaccounted button is a row this property
	 * says nothing about — and an unwatched clickable row is exactly the hazard.
	 */
	TestEqual(
		FString::Printf(
			TEXT("fixture: the panel should draw one button per presented row: %d rows, %d button(s) (%s)"),
			Clickable.Num(), BeforeState.Buttons.Num(), *DescribePanelButtons(BeforeState.Buttons)),
		BeforeState.Buttons.Num(), Clickable.Num());

	if (Inspector.Pieces.Num() != 3 || Clickable.Num() != BeforeState.Buttons.Num())
	{
		Fixture.End();
		return true;
	}

	/*
	 * AND THE ROWS ARE AT DISTINCT HEIGHTS. If every button stacked at one Y the comparison below
	 * would be free, which is how a zero-sized layout passes while asserting nothing.
	 */
	for (int32 Index = 1; Index < BeforeState.Buttons.Num(); ++Index)
	{
		TestTrue(
			FString::Printf(
				TEXT("fixture: every drawn row should sit at its own height, they sit at %s"),
				*DescribePanelButtons(BeforeState.Buttons)),
			!FMath::IsNearlyEqual(
				BeforeState.Buttons[Index - 1].TopLeftPx.Y,
				BeforeState.Buttons[Index].TopLeftPx.Y,
				PanelRowMustNotMovePx));
	}

	/* Hovering entry row 1 — which is all this is — singles that brick out. */
	Controller->SetInspectedPiece(Fixture.InspectedRef);

	const FPanelLayoutState AfterState = MeasurePanel(Panel, Root);

	AddInfo(FString::Printf(
		TEXT("brick %d:%d inspected (%d joints): panel desired size %.2f x %.2f px, buttons: %s"),
		Fixture.InspectedRef.StructureId, Fixture.InspectedRef.PieceIndex, Fixture.InspectedJoints,
		AfterState.DesiredSizePx.X, AfterState.DesiredSizePx.Y,
		*DescribePanelButtons(AfterState.Buttons)));

	/*
	 * FIXTURE: THE READOUT ACTUALLY CHANGED SIZE. A panel that measured the same in both states
	 * would hold every row still for free. THE GROWTH IS PERMITTED AND ASSERTED, NOT FORBIDDEN —
	 * see the header comment: pinning the height would outlaw a readout long enough to describe a
	 * brick with many joints.
	 */
	TestTrue(
		FString::Printf(
			TEXT("fixture: singling a brick out must grow the panel, or nothing has been held still: %.2f -> %.2f px"),
			BeforeState.DesiredSizePx.Y, AfterState.DesiredSizePx.Y),
		AfterState.DesiredSizePx.Y > BeforeState.DesiredSizePx.Y + PanelRowMustNotMovePx);

	TestEqual(
		FString::Printf(
			TEXT("singling a brick out must not add or remove buttons: %d before, %d after (%s)"),
			BeforeState.Buttons.Num(), AfterState.Buttons.Num(), *DescribePanelButtons(AfterState.Buttons)),
		AfterState.Buttons.Num(), BeforeState.Buttons.Num());

	/*
	 * THE ASSERTION. Every row the player can click is exactly where it was, so a click aimed at
	 * one commits that one.
	 */
	for (const FClickableRow& Row : Clickable)
	{
		const FPanelButton* const WasAt = FindPanelButton(BeforeState.Buttons, Row.Label);
		const FPanelButton* const IsAt = FindPanelButton(AfterState.Buttons, Row.Label);

		if (WasAt == nullptr || IsAt == nullptr)
		{
			AddError(FString::Printf(
				TEXT("%s row '%s' should be drawn in both states; before: %s / after: %s"),
				Row.Kind, *Row.Label,
				*DescribePanelButtons(BeforeState.Buttons), *DescribePanelButtons(AfterState.Buttons)));

			continue;
		}

		TestTrue(
			*FString::Printf(
				TEXT("%s row '%s' must not move when the readout changes: it was at y %.2f and is now at y %.2f, a shift of %.2f px (panel height %.2f -> %.2f px)"),
				Row.Kind, *Row.Label, WasAt->TopLeftPx.Y, IsAt->TopLeftPx.Y,
				IsAt->TopLeftPx.Y - WasAt->TopLeftPx.Y,
				BeforeState.DesiredSizePx.Y, AfterState.DesiredSizePx.Y),
			FMath::IsNearlyEqual(WasAt->TopLeftPx.Y, IsAt->TopLeftPx.Y, PanelRowMustNotMovePx));

		/*
		 * AND SIDEWAYS, WHERE THE CLAIM IS CONTAINMENT RATHER THAN STILLNESS. The panel is
		 * centred, so a row's left edge moves as the readout widens; that is harmless exactly
		 * while the narrower span lies inside the wider one, because the cursor is then over the
		 * same button in both states. Either direction counts — the readout may grow or collapse.
		 */
		const double WasLeftPx = WasAt->TopLeftPx.X;
		const double WasRightPx = WasLeftPx + WasAt->SizePx.X;
		const double IsLeftPx = IsAt->TopLeftPx.X;
		const double IsRightPx = IsLeftPx + IsAt->SizePx.X;

		const bool bNarrowerSpanIsInsideTheWider =
			(IsLeftPx >= WasLeftPx - PanelSpanContainmentSlackPx
				&& IsRightPx <= WasRightPx + PanelSpanContainmentSlackPx)
			|| (WasLeftPx >= IsLeftPx - PanelSpanContainmentSlackPx
				&& WasRightPx <= IsRightPx + PanelSpanContainmentSlackPx);

		TestTrue(
			*FString::Printf(
				TEXT("%s row '%s' must stay under a cursor sideways: it spanned x %.2f..%.2f and now spans %.2f..%.2f, neither containing the other"),
				Row.Kind, *Row.Label, WasLeftPx, WasRightPx, IsLeftPx, IsRightPx),
			bNarrowerSpanIsInsideTheWider);
	}

	Fixture.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
