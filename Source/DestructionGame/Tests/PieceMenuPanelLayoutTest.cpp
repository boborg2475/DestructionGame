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

// Named namespace: a unity build merges anonymous namespaces across files.
namespace PieceMenuPanelLayoutTestSupport
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;

	/** Y reach of the pick ray either side of the wall; clears a 10.25 cm brick. */
	constexpr double PanelRayReachCm = 100.0;

	/** Root geometry the panel is laid out in, px. Arbitrary; comparisons are within one space. */
	constexpr float PanelViewportWidthPx = 1920.0f;
	constexpr float PanelViewportHeightPx = 1080.0f;

	/** Slack on "the row did not move": layouts are bit-identical, the defect is tens of px. */
	constexpr double PanelRowMustNotMovePx = 0.5;

	/** Slack on horizontal containment, px, for the same reason. */
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

	/** One drawn button. Size is kept for the horizontal containment claim. */
	struct FPanelButton
	{
		FString Label;
		FVector2D TopLeftPx = FVector2D::ZeroVector;
		FVector2D SizePx = FVector2D::ZeroVector;

		/** Whether clicking takes focus. Defaults true (SButton's default) so a missed fill fails. */
		bool bFocusable = true;
	};

	/** One drawn line of text and its rectangle. */
	struct FPanelText
	{
		FString Text;
		FVector2D TopLeftPx = FVector2D::ZeroVector;
		FVector2D SizePx = FVector2D::ZeroVector;
	};

	/** Every STextBlock under a widget, concatenated; a button's caption. Buttons are found by text. */
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
	 * Arrange the tree and record every button's position. ArrangeChildren needs no RHI; cached
	 * geometry would be empty since nothing is painted.
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

			// The same virtual Slate asks when a click would move focus.
			Button.bFocusable = Widget->SupportsKeyboardFocus();
		}

		FArrangedChildren Arranged(EVisibility::All);

		Widget->ArrangeChildren(Geometry, Arranged);

		for (int32 Index = 0; Index < Arranged.Num(); ++Index)
		{
			CollectPanelButtons(Arranged[Index].Widget, Arranged[Index].Geometry, Out);
		}
	}

	/** Arrange the tree and record every line of text and its position. */
	void CollectPanelTexts(
		const TSharedRef<SWidget>& Widget,
		const FGeometry& Geometry,
		TArray<FPanelText>& Out)
	{
		if (Widget->GetType() == TEXT("STextBlock"))
		{
			FPanelText& Line = Out.AddDefaulted_GetRef();
			Line.Text = StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString();
			Line.TopLeftPx = FVector2D(Geometry.GetAbsolutePosition());
			Line.SizePx = FVector2D(Geometry.GetAbsoluteSize());
		}

		FArrangedChildren Arranged(EVisibility::All);

		Widget->ArrangeChildren(Geometry, Arranged);

		for (int32 Index = 0; Index < Arranged.Num(); ++Index)
		{
			CollectPanelTexts(Arranged[Index].Widget, Arranged[Index].Geometry, Out);
		}
	}

	FString DescribePanelTexts(const TArray<FPanelText>& Texts)
	{
		if (Texts.Num() == 0)
		{
			return TEXT("<no text>");
		}

		FString Line;

		for (int32 Index = 0; Index < Texts.Num(); ++Index)
		{
			Line += FString::Printf(
				TEXT("%s'%s'@y%.2f"),
				Index == 0 ? TEXT("") : TEXT(", "),
				*Texts[Index].Text,
				Texts[Index].TopLeftPx.Y);
		}

		return Line;
	}

	/** How many drawn lines read exactly this. */
	int32 CountPanelLines(const TArray<FPanelText>& Texts, const FString& Line)
	{
		int32 Count = 0;

		for (const FPanelText& Text : Texts)
		{
			Count += Text.Text == Line ? 1 : 0;
		}

		return Count;
	}

	const FPanelText* FindPanelLine(const TArray<FPanelText>& Texts, const FString& Line)
	{
		return Texts.FindByPredicate(
			[&Line](const FPanelText& Text) { return Text.Text == Line; });
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
				TEXT("%s'%s'@(%.2f, %.2f) %.2f x %.2f px%s"),
				Index == 0 ? TEXT("") : TEXT(", "),
				*Buttons[Index].Label,
				Buttons[Index].TopLeftPx.X,
				Buttons[Index].TopLeftPx.Y,
				Buttons[Index].SizePx.X,
				Buttons[Index].SizePx.Y,
				Buttons[Index].bFocusable ? TEXT(" FOCUSABLE") : TEXT(""));
		}

		return Line;
	}

	const FPanelButton* FindPanelButton(const TArray<FPanelButton>& Buttons, const FString& Label)
	{
		return Buttons.FindByPredicate(
			[&Label](const FPanelButton& Button) { return Button.Label == Label; });
	}

	/** Joints touching a piece. The readout grows a line per joint, so too few hide the defect. */
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

	/** The shared fixture, defined once so the tests cannot drift apart. */
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

			// Laid separately so the pick points come from the producer.
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

			// Three entry rows, enough of a stack above the readout to shift visibly.
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

			// Brick 1 has enough joints to grow the readout; asserted, not assumed.
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

	/** Which connection joins these two pieces, or INDEX_NONE if none does. */
	int32 FindJointBetween(const FStructure& Structure, int32 HandleA, int32 HandleB)
	{
		for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
		{
			const FConnection& Connection = Structure.GetConnection(Index);

			if ((Connection.PieceA == HandleA && Connection.PieceB == HandleB)
				|| (Connection.PieceA == HandleB && Connection.PieceB == HandleA))
			{
				return Index;
			}
		}

		return INDEX_NONE;
	}

	/** The middle of two boxes' overlap along one axis. */
	double PanelOverlapCentreCm(const FPieceBox& A, const FPieceBox& B, int32 Axis)
	{
		const double LowCm = FMath::Max(
			A.CentreCm[Axis] - A.ExtentCm[Axis], B.CentreCm[Axis] - B.ExtentCm[Axis]);

		const double HighCm = FMath::Min(
			A.CentreCm[Axis] + A.ExtentCm[Axis], B.CentreCm[Axis] + B.ExtentCm[Axis]);

		return (LowCm + HighCm) * 0.5;
	}

	/**
	 * The panel over a wall that bends. The flush FPanelFixture has no eccentric joint, so no line
	 * carries the bending clause and its sweep is blind to the panel's width.
	 *
	 *      course 4            [ 5 ]
	 *      course 3         [ 3 ][ 4 ]      each on one off-centre patch
	 *      course 2            [ 2 ]        the waist, singled out
	 *      course 1         [ 0 ][ 1 ]      grounded
	 *
	 * Brick 3's bed patch on the waist is centred 5.625 cm off its centre of mass, so that joint
	 * bends; the waist's ground joints do not. Lever arm and moment are both asserted.
	 */
	struct FBendingPanelFixture
	{
		FBrickTestWorld TestWorld;
		FBrickLayout Reference;
		FStructureBinding* Binding = nullptr;
		ADestructionGamePlayerController* Controller = nullptr;

		FPieceRef InspectedRef;
		int32 InspectedJoints = 0;

		bool bWorldBegun = false;

		/** Course 2's single brick. */
		static constexpr int32 WaistPiece = 2;

		/** Course 3's left brick, hanging half off the waist. */
		static constexpr int32 CorbelPiece = 3;

		static constexpr int32 BendingWallPieceCount = 6;

		/** A quarter of the brick pitch (5.625 cm for a 22.5 cm cell), computed from the spec. */
		double ExpectedLeverArmCm = 0.0;

		bool Begin(FAutomationTestBase& Test)
		{
			const FRunningBondSpec Spec = NarrowWaistWallSpec(4);

			ExpectedLeverArmCm = (Spec.BrickSizeCm.X + Spec.JointThicknessCm) * 0.25;

			Test.TestTrue(
				TEXT("fixture: RunningBond should lay the reference waist wall"),
				RunningBond(Spec, Reference));

			if (Reference.Boxes.Num() != BendingWallPieceCount)
			{
				Test.AddError(FString::Printf(
					TEXT("fixture: the waist wall should be %d pieces, got %d"),
					BendingWallPieceCount, Reference.Boxes.Num()));

				return false;
			}

			// Eccentricity from the laid boxes, not the solver, so a disagreement names the culprit.
			const double PatchCentreXCm = PanelOverlapCentreCm(
				Reference.Boxes[WaistPiece], Reference.Boxes[CorbelPiece], /*Axis*/ 0);

			const double LeverArmCm =
				FMath::Abs(PatchCentreXCm - Reference.Boxes[CorbelPiece].CentreCm.X);

			Test.TestEqual(
				FString::Printf(
					TEXT("fixture: the corbel's bed patch must be off-centre by %.4f cm for anything to bend, it is %.4f cm"),
					ExpectedLeverArmCm, LeverArmCm),
				LeverArmCm, ExpectedLeverArmCm, 1e-9);

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

			if (Binding == nullptr || Binding->NumPieces() != BendingWallPieceCount)
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

			// Three picks, as in the flush fixture: the grounded pair and the waist.
			for (int32 Piece = 0; Piece <= WaistPiece; ++Piece)
			{
				Controller->InspectAlongRay(
					PanelRayStart(Reference.Boxes[Piece]), PanelRayEnd(Reference.Boxes[Piece]));
			}

			Test.TestEqual(
				FString::Printf(
					TEXT("fixture: three clicks should have picked three bricks, the selection holds %d"),
					Controller->GetPieceSelection().Num()),
				Controller->GetPieceSelection().Num(), 3);

			InspectedRef = PanelRef(StructureId, WaistPiece);

			const int32 InspectedHandle = Binding->ResolvePiece(InspectedRef);

			InspectedJoints = JointsTouchingPiece(Binding->GetStructure(), InspectedHandle);

			Test.TestTrue(
				FString::Printf(
					TEXT("fixture: the brick singled out must have at least 3 joints for the readout to grow by, it has %d"),
					InspectedJoints),
				InspectedJoints >= 3);

			// And the solver must actually report a moment on that joint.
			const int32 BendingJoint =
				FindJointBetween(Binding->GetStructure(), InspectedHandle, CorbelPiece);

			if (BendingJoint == INDEX_NONE)
			{
				Test.AddError(TEXT("fixture: the waist and the corbel above it must share a joint"));
				return false;
			}

			const double MomentUuCm =
				Binding->GetStructure().GetConnectionMoment(BendingJoint).Size();

			Test.TestTrue(
				FString::Printf(
					TEXT("fixture: joint #%d must be carrying a bend for a readout line to describe one, it carries %.6f uu.cm"),
					BendingJoint, MomentUuCm),
				MomentUuCm > 0.0);

			return InspectedJoints >= 3 && MomentUuCm > 0.0;
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

	/** Ten courses of four (~45 pieces): enough for a 40-brick selection. Fits the harness floor. */
	FRunningBondSpec TallWallSpec()
	{
		FRunningBondSpec Spec = WallSpec();
		Spec.CoursesHigh = 10;
		Spec.BricksPerCourse = 4;

		return Spec;
	}

	/** Minimum picks for the cap test to count as a long list. */
	constexpr int32 PanelLongSelectionAtLeast = 40;

	/** A world holding that wall and a controller. Nothing is picked yet. */
	struct FTallWallFixture
	{
		FBrickTestWorld TestWorld;
		FBrickLayout Reference;
		ADestructionGamePlayerController* Controller = nullptr;
		int32 StructureId = INDEX_NONE;
		bool bWorldBegun = false;

		bool Begin(FAutomationTestBase& Test)
		{
			const FRunningBondSpec Spec = TallWallSpec();

			Test.TestTrue(
				TEXT("fixture: RunningBond should lay the tall reference wall"),
				RunningBond(Spec, Reference));

			Test.TestTrue(
				*FString::Printf(
					TEXT("fixture: the tall wall must hold at least %d bricks for a list to be worth capping, it holds %d"),
					PanelLongSelectionAtLeast, Reference.Boxes.Num()),
				Reference.Boxes.Num() >= PanelLongSelectionAtLeast);

			if (Reference.Boxes.Num() < PanelLongSelectionAtLeast || !TestWorld.Begin(Test))
			{
				return false;
			}

			bWorldBegun = true;

			StructureId = TestWorld.Subsystem->BuildRunningBond(Spec);

			FStructureBinding* const Binding = TestWorld.Subsystem->Find(StructureId);

			Test.TestNotNull(
				TEXT("fixture: Find should hand back the tall wall's binding"), Binding);

			if (Binding == nullptr || Binding->NumPieces() != Reference.Boxes.Num())
			{
				return false;
			}

			Binding->SolveLoads();

			Controller = TestWorld.World->SpawnActor<ADestructionGamePlayerController>();

			Test.TestNotNull(
				TEXT("fixture: the test world should spawn the game's player controller"), Controller);

			return Controller != nullptr;
		}

		/** Click bricks [First, Last] of the reference layout once each. */
		void Pick(int32 First, int32 Last)
		{
			for (int32 Piece = First; Piece <= Last && Reference.Boxes.IsValidIndex(Piece); ++Piece)
			{
				Controller->InspectAlongRay(
					PanelRayStart(Reference.Boxes[Piece]), PanelRayEnd(Reference.Boxes[Piece]));
			}
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

	/** One arranged widget of any kind, with a parent link for finding common ancestors. */
	struct FPanelWidget
	{
		FString Type;

		/** Text, for an STextBlock only. */
		FString Text;

		FVector2D TopLeftPx = FVector2D::ZeroVector;
		FVector2D SizePx = FVector2D::ZeroVector;

		/**
		 * Desired size, the room the glyphs need. Arranged size cannot show clipping: a filling slot
		 * arranges at the column width regardless. Valid after MeasurePanel's prepass.
		 */
		FVector2D DesiredSizePx = FVector2D::ZeroVector;

		int32 ParentIndex = INDEX_NONE;
	};

	/** The same arrange walk as the other collectors, keeping every widget. */
	void CollectPanelWidgets(
		const TSharedRef<SWidget>& Widget,
		const FGeometry& Geometry,
		int32 ParentIndex,
		TArray<FPanelWidget>& Out)
	{
		const int32 Index = Out.AddDefaulted();

		{
			FPanelWidget& Entry = Out[Index];
			Entry.Type = Widget->GetType().ToString();
			Entry.TopLeftPx = FVector2D(Geometry.GetAbsolutePosition());
			Entry.SizePx = FVector2D(Geometry.GetAbsoluteSize());
			Entry.DesiredSizePx = FVector2D(Widget->GetDesiredSize());
			Entry.ParentIndex = ParentIndex;

			if (Widget->GetType() == TEXT("STextBlock"))
			{
				Entry.Text = StaticCastSharedRef<STextBlock>(Widget)->GetText().ToString();
			}
		}

		FArrangedChildren Arranged(EVisibility::All);

		Widget->ArrangeChildren(Geometry, Arranged);

		for (int32 Child = 0; Child < Arranged.Num(); ++Child)
		{
			CollectPanelWidgets(Arranged[Child].Widget, Arranged[Child].Geometry, Index, Out);
		}
	}

	/** Indices of every widget that reads exactly this. */
	TArray<int32> FindPanelWidgetsByText(const TArray<FPanelWidget>& Widgets, const FString& Text)
	{
		TArray<int32> Found;

		for (int32 Index = 0; Index < Widgets.Num(); ++Index)
		{
			if (Widgets[Index].Text == Text)
			{
				Found.Add(Index);
			}
		}

		return Found;
	}

	/** The lowest widget that has both of these somewhere beneath it. INDEX_NONE if there is none. */
	int32 PanelCommonAncestor(const TArray<FPanelWidget>& Widgets, int32 A, int32 B)
	{
		TSet<int32> AboveA;

		for (int32 Node = A; Node != INDEX_NONE; Node = Widgets[Node].ParentIndex)
		{
			AboveA.Add(Node);
		}

		for (int32 Node = B; Node != INDEX_NONE; Node = Widgets[Node].ParentIndex)
		{
			if (AboveA.Contains(Node))
			{
				return Node;
			}
		}

		return INDEX_NONE;
	}

	int32 PanelCommonAncestor(const TArray<FPanelWidget>& Widgets, const TArray<int32>& Nodes)
	{
		if (Nodes.Num() == 0)
		{
			return INDEX_NONE;
		}

		int32 Ancestor = Nodes[0];

		for (int32 Index = 1; Index < Nodes.Num() && Ancestor != INDEX_NONE; ++Index)
		{
			Ancestor = PanelCommonAncestor(Widgets, Ancestor, Nodes[Index]);
		}

		return Ancestor;
	}

	/**
	 * The panel's rectangle: the root canvas's child. The canvas spans the viewport and
	 * GetDesiredSize includes the drag offset, so neither will do.
	 */
	const FPanelWidget* PanelRectangle(const TArray<FPanelWidget>& Widgets)
	{
		return Widgets.FindByPredicate(
			[](const FPanelWidget& Widget) { return Widget.ParentIndex == 0; });
	}

	FString DescribePanelWidget(const FPanelWidget& Widget)
	{
		return FString::Printf(
			TEXT("%s%s x %.2f..%.2f, y %.2f..%.2f"),
			*Widget.Type,
			Widget.Text.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" '%s'"), *Widget.Text),
			Widget.TopLeftPx.X, Widget.TopLeftPx.X + Widget.SizePx.X,
			Widget.TopLeftPx.Y, Widget.TopLeftPx.Y + Widget.SizePx.Y);
	}

	/** One laid-out state of the panel. */
	struct FPanelLayoutState
	{
		FVector2D DesiredSizePx = FVector2D::ZeroVector;
		TArray<FPanelButton> Buttons;
		TArray<FPanelText> Texts;
		TArray<FPanelWidget> Widgets;
	};

	/** Prepass and arrange the tree. Called twice on one panel to compare two states. */
	FPanelLayoutState MeasurePanel(const TSharedRef<SWidget>& Panel, const FGeometry& Root)
	{
		Panel->SlatePrepass(1.0f);

		FPanelLayoutState State;
		State.DesiredSizePx = FVector2D(Panel->GetDesiredSize());

		CollectPanelButtons(Panel, Root, State.Buttons);
		CollectPanelTexts(Panel, Root, State.Texts);
		CollectPanelWidgets(Panel, Root, INDEX_NONE, State.Widgets);

		return State;
	}

	FGeometry PanelRootGeometry()
	{
		return FGeometry::MakeRoot(
			FVector2f(PanelViewportWidthPx, PanelViewportHeightPx), FSlateLayoutTransform());
	}

	/** The panel draws this line. An empty expected line is a model fault, not a vacuous pass. */
	void CheckPanelDrawsLine(
		FAutomationTestBase& Test,
		const FPanelLayoutState& State,
		const FString& Line,
		const FString& What)
	{
		if (Line.IsEmpty())
		{
			Test.AddError(FString::Printf(
				TEXT("the model must supply %s for the panel to have anything to draw; it is empty"),
				*What));

			return;
		}

		Test.TestTrue(
			*FString::Printf(
				TEXT("the panel must draw %s ('%s'), it draws none of it: %s"),
				*What, *Line, *DescribePanelTexts(State.Texts)),
			CountPanelLines(State.Texts, Line) >= 1);
	}

	/** The panel does not draw this line. */
	void CheckPanelDrawsNoLine(
		FAutomationTestBase& Test,
		const FPanelLayoutState& State,
		const FString& Line,
		const FString& What)
	{
		if (Line.IsEmpty())
		{
			Test.AddError(FString::Printf(
				TEXT("nothing can be shown absent without a line to look for; %s is empty"), *What));

			return;
		}

		Test.TestEqual(
			FString::Printf(
				TEXT("the panel must NOT draw %s ('%s'), it draws it %d time(s): %s"),
				*What, *Line, CountPanelLines(State.Texts, Line), *DescribePanelTexts(State.Texts)),
			CountPanelLines(State.Texts, Line), 0);
	}

	/** Max centre-to-centre gap for two texts on one line: under half a ~22 px row. */
	constexpr double PanelSameLineSlackPx = 8.0;

	double PanelCentreYPx(const FPanelText& Text)
	{
		return Text.TopLeftPx.Y + Text.SizePx.Y * 0.5;
	}

	/** Min gap between adjacent decade labels, px (over half a digit); the defect measured 0.33 px. */
	constexpr double PanelTickLabelGapPx = 4.0;

	/** What one sweep measured, so the two detail modes can be compared. */
	struct FPanelFitMeasurement
	{
		bool bMeasured = false;

		FVector2D PanelTopLeftPx = FVector2D::ZeroVector;
		FVector2D PanelSizePx = FVector2D::ZeroVector;

		/** The content column, measured off the action row. */
		double ContentLeftPx = 0.0;
		double ContentRightPx = 0.0;

		/** The tightest line's remaining room, and which line it was. */
		double TightestSlackPx = 0.0;
		FString TightestLine;
	};

	/**
	 * Sweep every line the readout supplies and hold it inside the panel's column. Run on two walls
	 * and both detail modes; entry rows are swept in both, since compact drops the joint lines that
	 * set full's width.
	 *
	 * @param WallDescription names the wall in failure messages.
	 * @param Detail the mode to build and measure; the controller is left in it.
	 */
	FPanelFitMeasurement SweepReadoutFitsInsideThePanel(
		FAutomationTestBase& Test,
		ADestructionGamePlayerController* Controller,
		const FPieceRef& InspectedRef,
		const TCHAR* WallDescription,
		EPieceMenuDetail Detail)
	{
		const bool bIsCompact = Detail == EPieceMenuDetail::Compact;
		const TCHAR* const DetailName = bIsCompact ? TEXT("compact") : TEXT("full");

		FPanelFitMeasurement Measured;

		// Set before the build, which reads it.
		Controller->SetPieceMenuDetail(Detail);

		const TSharedRef<SWidget> Panel = Controller->BuildPieceMenuPanel();
		const FGeometry Root = PanelRootGeometry();

		// The readout is widest with a brick singled out.
		Controller->SetInspectedPiece(InspectedRef);

		const FPieceMenuInspector Inspector = Controller->PieceMenuInspectorForSelection(Detail);
		const FPanelLayoutState State = MeasurePanel(Panel, Root);

		const TArrayView<const FPieceMenuRow> Rows = Controller->GetShownPieceMenuRows();

		Test.TestTrue(
			FString::Printf(
				TEXT("fixture (%s, %s): the selection should offer at least one action row to measure the panel by, the presenter shows %d"),
				WallDescription, DetailName, Rows.Num()),
			Rows.Num() >= 1);

		// Fixture: full detail has a joint table, compact has dropped it.
		const int32 FullJointCount =
			Controller->PieceMenuInspectorForSelection(EPieceMenuDetail::Full).Joints.Num();

		Test.TestTrue(
			FString::Printf(
				TEXT("fixture (%s, %s): the singled-out brick should break out joints in FULL detail for the width to be about anything, it broke out %d"),
				WallDescription, DetailName, FullJointCount),
			FullJointCount >= 3);

		Test.TestEqual(
			FString::Printf(
				TEXT("fixture (%s, %s): this mode should break out %d joint row(s), it broke out %d"),
				WallDescription, DetailName, bIsCompact ? 0 : FullJointCount, Inspector.Joints.Num()),
			Inspector.Joints.Num(), bIsCompact ? 0 : FullJointCount);

		if (Rows.Num() == 0 || FullJointCount < 3)
		{
			return Measured;
		}

		const FPanelButton* const ActionRow = FindPanelButton(State.Buttons, Rows.Last().Label);

		if (ActionRow == nullptr)
		{
			Test.AddError(FString::Printf(
				TEXT("the panel must draw the action row '%s' for its content width to be measurable; it drew %s"),
				*Rows.Last().Label, *DescribePanelButtons(State.Buttons)));

			return Measured;
		}

		const double ContentLeftPx = ActionRow->TopLeftPx.X;
		const double ContentRightPx = ContentLeftPx + ActionRow->SizePx.X;

		// Fixture: the column has a real, sub-viewport width, or containment is impossible or free.
		Test.TestTrue(
			*FString::Printf(
				TEXT("fixture (%s, %s): the action row should span a real width, it spans x %.2f..%.2f in a %.0f px viewport"),
				WallDescription, DetailName, ContentLeftPx, ContentRightPx, PanelViewportWidthPx),
			ActionRow->SizePx.X > 0.0 && ActionRow->SizePx.X < PanelViewportWidthPx);

		if (!(ActionRow->SizePx.X > 0.0))
		{
			return Measured;
		}

		/*
		 * The arranged panel must match PieceMenuPanelSizePx. An SBox override is only a desired
		 * size, and a filling slot can hand its child any width, so this is measured.
		 */
		const FPanelWidget* const PanelRect = PanelRectangle(State.Widgets);

		if (PanelRect == nullptr)
		{
			Test.AddError(FString::Printf(
				TEXT("fixture (%s, %s): the canvas should place exactly one panel for its rectangle to be measurable"),
				WallDescription, DetailName));

			return Measured;
		}

		Test.AddInfo(FString::Printf(
			TEXT("%s, %s detail: the panel is arranged %s"),
			WallDescription, DetailName, *DescribePanelWidget(*PanelRect)));

		const FVector2D SaidSizePx = PieceMenuPanelSizePx(Detail);

		Test.TestTrue(
			*FString::Printf(
				TEXT("the %s panel must be drawn at the size the presenter gives for that mode (%s, %.2f x %.2f px): it is arranged %.2f x %.2f px, out by %.2f x %.2f px (%s)"),
				DetailName, DetailName, SaidSizePx.X, SaidSizePx.Y,
				PanelRect->SizePx.X, PanelRect->SizePx.Y,
				PanelRect->SizePx.X - SaidSizePx.X, PanelRect->SizePx.Y - SaidSizePx.Y,
				WallDescription),
			FMath::IsNearlyEqual(PanelRect->SizePx.X, SaidSizePx.X, PanelSpanContainmentSlackPx)
				&& FMath::IsNearlyEqual(PanelRect->SizePx.Y, SaidSizePx.Y, PanelSpanContainmentSlackPx));

		Measured.bMeasured = true;
		Measured.PanelTopLeftPx = PanelRect->TopLeftPx;
		Measured.PanelSizePx = PanelRect->SizePx;
		Measured.ContentLeftPx = ContentLeftPx;
		Measured.ContentRightPx = ContentRightPx;

		// Lines come from the model, so an undrawn line fails rather than being skipped.
		TArray<FString> ReadoutLines;
		ReadoutLines.AddUnique(Inspector.InspectedLabel);
		ReadoutLines.AddUnique(Inspector.SupportText);
		ReadoutLines.AddUnique(Inspector.JointsText);

		// The brick list, which sets compact's width once the joint table is gone.
		for (const FInspectorPieceEntry& Entry : Inspector.Pieces)
		{
			ReadoutLines.AddUnique(Entry.Label);
			ReadoutLines.AddUnique(Entry.SupportText);
		}

		for (const FInspectorJointRow& Joint : Inspector.Joints)
		{
			ReadoutLines.AddUnique(Joint.Text);
		}

		// Caption and ticks only exist with a bar; empty ones would be reported as faults below.
		if (Inspector.Joints.Num() > 0)
		{
			ReadoutLines.AddUnique(Inspector.HeadroomCaption);

			for (const FHeadroomScaleTick& Tick : Inspector.HeadroomScale)
			{
				ReadoutLines.AddUnique(Tick.Label);
			}
		}

		double TightestSlackPx = TNumericLimits<double>::Max();
		FString TightestLine;

		for (const FString& Line : ReadoutLines)
		{
			if (Line.IsEmpty())
			{
				Test.AddError(TEXT("the model must supply every readout line for the panel to have anything to place"));
				continue;
			}

			// Every match: the inspected brick is drawn twice (entry row and heading).
			const TArray<int32> Drawn = FindPanelWidgetsByText(State.Widgets, Line);

			if (Drawn.Num() == 0)
			{
				Test.AddError(FString::Printf(
					TEXT("readout line '%s' should be drawn; the panel drew %s"),
					*Line, *DescribePanelTexts(State.Texts)));

				continue;
			}

			for (const int32 Index : Drawn)
			{
				const FPanelWidget& Text = State.Widgets[Index];

				const double LineLeftPx = Text.TopLeftPx.X;

				// Where the glyphs end (desired size), not the filling slot's arranged width.
				const double LineRightPx = LineLeftPx + Text.DesiredSizePx.X;
				const double SlackPx = ContentRightPx - LineRightPx;

				if (SlackPx < TightestSlackPx)
				{
					TightestSlackPx = SlackPx;
					TightestLine = Line;
				}

				Test.TestTrue(
					*FString::Printf(
						TEXT("readout line '%s' must fit inside the %s panel (%s): it needs x %.2f..%.2f against a content column of %.2f..%.2f, overrunning by %.2f px"),
						*Line, DetailName, WallDescription, LineLeftPx, LineRightPx,
						ContentLeftPx, ContentRightPx, LineRightPx - ContentRightPx),
					LineLeftPx >= ContentLeftPx - PanelSpanContainmentSlackPx
						&& LineRightPx <= ContentRightPx + PanelSpanContainmentSlackPx);
			}
		}

		// Spare width beside the longest line: reported, not asserted, as a sizing input.
		Test.AddInfo(FString::Printf(
			TEXT("%s, %s detail: the readout's tightest line is '%s', %s %.2f px inside a %.2f px content column"),
			WallDescription, DetailName, *TightestLine,
			TightestSlackPx < 0.0 ? TEXT("OVERRUNNING BY") : TEXT("with spare of"),
			FMath::Abs(TightestSlackPx), ContentRightPx - ContentLeftPx));

		/*
		 * No two lines on one row may overlap. Narrowing the panel pushes the fixed support-word
		 * column into the position label while both stay contained, so containment alone misses it.
		 * Rows are matched by vertical centre, spans by glyph (desired) width.
		 */
		double TightestGapPx = TNumericLimits<double>::Max();
		FString TightestPair;

		for (int32 Left = 0; Left < State.Widgets.Num(); ++Left)
		{
			const FPanelWidget& A = State.Widgets[Left];

			if (A.Text.IsEmpty() || !(A.DesiredSizePx.X > 0.0) || !(A.SizePx.Y > 0.0))
			{
				continue;
			}

			for (int32 Right = Left + 1; Right < State.Widgets.Num(); ++Right)
			{
				const FPanelWidget& B = State.Widgets[Right];

				if (B.Text.IsEmpty() || !(B.DesiredSizePx.X > 0.0) || !(B.SizePx.Y > 0.0))
				{
					continue;
				}

				const double ACentreYPx = A.TopLeftPx.Y + A.SizePx.Y * 0.5;
				const double BCentreYPx = B.TopLeftPx.Y + B.SizePx.Y * 0.5;

				if (FMath::Abs(ACentreYPx - BCentreYPx) > PanelSameLineSlackPx)
				{
					continue;
				}

				const FPanelWidget& First = A.TopLeftPx.X <= B.TopLeftPx.X ? A : B;
				const FPanelWidget& Second = A.TopLeftPx.X <= B.TopLeftPx.X ? B : A;

				const double GapPx =
					Second.TopLeftPx.X - (First.TopLeftPx.X + First.DesiredSizePx.X);

				if (GapPx < TightestGapPx)
				{
					TightestGapPx = GapPx;
					TightestPair = FString::Printf(
						TEXT("'%s' then '%s'"), *First.Text, *Second.Text);
				}

				Test.TestTrue(
					*FString::Printf(
						TEXT("two lines sharing a row in the %s panel must not print over each other (%s): '%s' needs x %.2f..%.2f and '%s' begins at x %.2f, overlapping by %.2f px"),
						DetailName, WallDescription,
						*First.Text, First.TopLeftPx.X, First.TopLeftPx.X + First.DesiredSizePx.X,
						*Second.Text, Second.TopLeftPx.X, -GapPx),
					GapPx >= -PanelSpanContainmentSlackPx);
			}
		}

		// The other sizing input: the tightest gap between two lines on one row.
		if (TightestGapPx < TNumericLimits<double>::Max())
		{
			Test.AddInfo(FString::Printf(
				TEXT("%s, %s detail: the tightest pair sharing a row is %s, %s %.2f px apart"),
				WallDescription, DetailName, *TightestPair,
				TightestGapPx < 0.0 ? TEXT("OVERLAPPING BY") : TEXT("with"),
				FMath::Abs(TightestGapPx)));
		}

		Measured.TightestSlackPx = TightestSlackPx;
		Measured.TightestLine = TightestLine;

		return Measured;
	}

	/**
	 * The compact panel is strictly smaller on both axes than full, on the same wall and bricks.
	 * Height is measured because the readout's FillHeight slot hides fewer lines from the panel.
	 */
	void CheckCompactPanelGivesScreenBack(
		FAutomationTestBase& Test,
		const FPanelFitMeasurement& Full,
		const FPanelFitMeasurement& Compact,
		const TCHAR* WallDescription)
	{
		if (!Full.bMeasured || !Compact.bMeasured)
		{
			Test.AddError(FString::Printf(
				TEXT("fixture (%s): both detail modes must have been measured to be compared"),
				WallDescription));

			return;
		}

		Test.TestTrue(
			*FString::Printf(
				TEXT("the compact panel must cover less of the screen ACROSS (%s): it is arranged %.2f px wide against the full panel's %.2f px, giving back %.2f px"),
				WallDescription, Compact.PanelSizePx.X, Full.PanelSizePx.X,
				Full.PanelSizePx.X - Compact.PanelSizePx.X),
			Compact.PanelSizePx.X < Full.PanelSizePx.X);

		Test.TestTrue(
			*FString::Printf(
				TEXT("the compact panel must cover less of the screen DOWN (%s): it is arranged %.2f px tall against the full panel's %.2f px, giving back %.2f px"),
				WallDescription, Compact.PanelSizePx.Y, Full.PanelSizePx.Y,
				Full.PanelSizePx.Y - Compact.PanelSizePx.Y),
			Compact.PanelSizePx.Y < Full.PanelSizePx.Y);

		// The content column must shrink by the same amount; the chrome is equal in both modes.
		const double FullColumnPx = Full.ContentRightPx - Full.ContentLeftPx;
		const double CompactColumnPx = Compact.ContentRightPx - Compact.ContentLeftPx;

		Test.TestTrue(
			*FString::Printf(
				TEXT("the compact panel's content column must shrink with the panel (%s): the panel gave back %.2f px and the column gave back %.2f px"),
				WallDescription,
				Full.PanelSizePx.X - Compact.PanelSizePx.X, FullColumnPx - CompactColumnPx),
			FMath::IsNearlyEqual(
				FullColumnPx - CompactColumnPx,
				Full.PanelSizePx.X - Compact.PanelSizePx.X,
				PanelSpanContainmentSlackPx));
	}
}

/**
 * Singling out a brick must not move the menu entry the cursor is on.
 *
 * Hovering an entry adds joint lines to the readout; a vertically centred panel then moves every
 * entry above it by half the growth, the cursor leaves the row, the readout empties, and the brick
 * strobes Inspected/Selected at 60 Hz. The assertion is each entry row's arranged Y. Needs no RHI.
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

	// Fixture: a zero-size layout would pass vacuously.
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

	// Rows at distinct heights, or the comparison below is free.
	TestTrue(
		FString::Printf(TEXT("fixture: the entry rows should sit at different heights, they sit at %s"),
			*DescribePanelButtons(Before)),
		!FMath::IsNearlyEqual(Before[0].TopLeftPx.Y, Before[1].TopLeftPx.Y, PanelRowMustNotMovePx));

	// What hovering entry row 1 does.
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
 * No clickable row (entry or action) moves when the readout changes.
 *
 * Moving the cursor off the last entry toward Delete collapses the readout and slides Delete up
 * toward the cursor, so a click could delete an unaimed brick, irreversibly. Rows come from the
 * presenter and are matched by caption. The panel size is also pinned; sideways, each row's
 * narrower span must lie inside its wider one. Needs no RHI.
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

	// From the presenter, not the drawn buttons, so a missing row still gets asserted.
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

	// Equality, not a floor: an extra button would be an unwatched clickable row.
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

	// Rows at distinct heights, or the comparison below is free.
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

	// What hovering entry row 1 does.
	Controller->SetInspectedPiece(Fixture.InspectedRef);

	const FPanelLayoutState AfterState = MeasurePanel(Panel, Root);

	AddInfo(FString::Printf(
		TEXT("brick %d:%d inspected (%d joints): panel desired size %.2f x %.2f px, buttons: %s"),
		Fixture.InspectedRef.StructureId, Fixture.InspectedRef.PieceIndex, Fixture.InspectedJoints,
		AfterState.DesiredSizePx.X, AfterState.DesiredSizePx.Y,
		*DescribePanelButtons(AfterState.Buttons)));

	// Fixture: the readout text actually changed (the panel's height no longer can).
	const FPieceMenuInspector Inspected = Controller->PieceMenuInspectorForSelection();

	TestTrue(
		FString::Printf(
			TEXT("fixture: the brick singled out should break out %d joint(s), the model broke out %d"),
			Fixture.InspectedJoints, Inspected.Joints.Num()),
		Inspected.Joints.Num() >= 3);

	if (Inspected.Joints.Num() >= 1)
	{
		const FString& FirstJointLine = Inspected.Joints[0].Text;

		TestEqual(
			FString::Printf(
				TEXT("fixture: joint line '%s' must be drawn once a brick is singled out; drawn %d time(s): %s"),
				*FirstJointLine, CountPanelLines(AfterState.Texts, FirstJointLine),
				*DescribePanelTexts(AfterState.Texts)),
			CountPanelLines(AfterState.Texts, FirstJointLine), 1);

		TestEqual(
			FString::Printf(
				TEXT("fixture: joint line '%s' must NOT be drawn with nothing singled out; drawn %d time(s): %s"),
				*FirstJointLine, CountPanelLines(BeforeState.Texts, FirstJointLine),
				*DescribePanelTexts(BeforeState.Texts)),
			CountPanelLines(BeforeState.Texts, FirstJointLine), 0);
	}

	// The panel keeps its size on both axes: the mechanism behind every row assertion below.
	TestTrue(
		*FString::Printf(
			TEXT("the panel must be the same size whatever is singled out: %.2f x %.2f px before, %.2f x %.2f px after"),
			BeforeState.DesiredSizePx.X, BeforeState.DesiredSizePx.Y,
			AfterState.DesiredSizePx.X, AfterState.DesiredSizePx.Y),
		FMath::IsNearlyEqual(BeforeState.DesiredSizePx.X, AfterState.DesiredSizePx.X, PanelRowMustNotMovePx)
			&& FMath::IsNearlyEqual(BeforeState.DesiredSizePx.Y, AfterState.DesiredSizePx.Y, PanelRowMustNotMovePx));

	TestEqual(
		FString::Printf(
			TEXT("singling a brick out must not add or remove buttons: %d before, %d after (%s)"),
			BeforeState.Buttons.Num(), AfterState.Buttons.Num(), *DescribePanelButtons(AfterState.Buttons)),
		AfterState.Buttons.Num(), BeforeState.Buttons.Num());

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

		// Sideways the claim is containment: the narrower span lies inside the wider, either way.
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

/**
 * The panel draws every line the model supplies for its current state, and none from the other.
 *
 * Model fields (the headroom caption and scale) once went undrawn with every model test green, so
 * this checks the arranged tree. Presence only; appearance is the screenshot's job. Expected and
 * absent lines both come from the model, so no wording is pinned here. The identity line
 * (SESSION_UI_DESIGN §c, S7) must sit between the brick's name and its joint summary.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuPanelPrintsTheModelTest,
	"DestructionGame.World.Menu.PanelPrintsEveryLineTheModelSupplies",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuPanelPrintsTheModelTest::RunTest(const FString& Parameters)
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

	// Bricks picked, none pointed at.
	const FPieceMenuInspector Idle = Controller->PieceMenuInspectorForSelection();
	const FPanelLayoutState IdleState = MeasurePanel(Panel, Root);

	AddInfo(FString::Printf(
		TEXT("nothing inspected: %s"), *DescribePanelTexts(IdleState.Texts)));

	CheckPanelDrawsLine(*this, IdleState, Idle.HeaderText, TEXT("the panel's own heading"));
	CheckPanelDrawsLine(*this, IdleState, Idle.CountText, TEXT("the selection count"));

	for (const FInspectorPieceEntry& Entry : Idle.Pieces)
	{
		CheckPanelDrawsLine(
			*this, IdleState, Entry.Label, FString::Printf(TEXT("entry row '%s'"), *Entry.Label));
	}

	CheckPanelDrawsLine(
		*this, IdleState, Idle.InspectedHintText,
		TEXT("the line standing in for a readout there is not"));

	// Now one brick is pointed at.
	Controller->SetInspectedPiece(Fixture.InspectedRef);

	const FPieceMenuInspector Inspected = Controller->PieceMenuInspectorForSelection();
	const FPanelLayoutState InspectedState = MeasurePanel(Panel, Root);

	AddInfo(FString::Printf(
		TEXT("brick %d:%d inspected: %s"),
		Fixture.InspectedRef.StructureId, Fixture.InspectedRef.PieceIndex,
		*DescribePanelTexts(InspectedState.Texts)));

	TestTrue(
		FString::Printf(
			TEXT("fixture: the singled-out brick should break out joints for the readout to describe, it broke out %d"),
			Inspected.Joints.Num()),
		Inspected.Joints.Num() >= 3);

	CheckPanelDrawsLine(*this, InspectedState, Inspected.HeaderText, TEXT("the panel's own heading"));
	CheckPanelDrawsLine(*this, InspectedState, Inspected.CountText, TEXT("the selection count"));

	// Matches the entry row too; this only claims the panel has the model's string.
	CheckPanelDrawsLine(
		*this, InspectedState, Inspected.InspectedLabel,
		TEXT("the name of the brick the readout is about"));

	/*
	 * The identity line, directly under the name (SESSION_UI_DESIGN §c). Its wording is
	 * Presenter.PieceIdentityText's concern; the fixture's bricks read "Unknown material".
	 */
	TestFalse(
		TEXT("fixture: the singled-out brick must have an identity line for the panel to draw"),
		Inspected.IdentityText.IsEmpty());

	CheckPanelDrawsLine(
		*this, InspectedState, Inspected.IdentityText,
		TEXT("what the brick is made of, how big it is and what it weighs"));

	// The lowest occurrence of the name: the first is the entry row up in the list.
	const FPanelText* NameLine = nullptr;
	const FPanelText* IdentityLine = FindPanelLine(InspectedState.Texts, Inspected.IdentityText);

	for (const FPanelText& Line : InspectedState.Texts)
	{
		if (Line.Text == Inspected.InspectedLabel
			&& (NameLine == nullptr || Line.TopLeftPx.Y > NameLine->TopLeftPx.Y))
		{
			NameLine = &Line;
		}
	}

	const FPanelText* const JointsLine = FindPanelLine(InspectedState.Texts, Inspected.JointsText);

	if (NameLine != nullptr && IdentityLine != nullptr)
	{
		TestTrue(
			*FString::Printf(
				TEXT("the identity line '%s' must sit UNDER the name '%s' — it is at y%.2f and the name is at y%.2f"),
				*Inspected.IdentityText, *Inspected.InspectedLabel,
				IdentityLine->TopLeftPx.Y, NameLine->TopLeftPx.Y),
			IdentityLine->TopLeftPx.Y > NameLine->TopLeftPx.Y);
	}

	if (JointsLine != nullptr && IdentityLine != nullptr)
	{
		TestTrue(
			*FString::Printf(
				TEXT("and ABOVE the joint summary '%s' — what a brick IS belongs with its name, not "
					 "below its joint table; identity at y%.2f, summary at y%.2f"),
				*Inspected.JointsText, IdentityLine->TopLeftPx.Y, JointsLine->TopLeftPx.Y),
			IdentityLine->TopLeftPx.Y < JointsLine->TopLeftPx.Y);
	}

	CheckPanelDrawsLine(
		*this, InspectedState, Inspected.SupportText, TEXT("why the brick is or is not held up"));

	CheckPanelDrawsLine(
		*this, InspectedState, Inspected.JointsText, TEXT("how many joints the brick has"));

	for (const FInspectorJointRow& Joint : Inspected.Joints)
	{
		CheckPanelDrawsLine(
			*this, InspectedState, Joint.Text,
			FString::Printf(TEXT("the line for joint #%d"), Joint.ConnectionIndex));
	}

	// The headroom bar's caption and ticks; a log axis without decades is unreadable.
	CheckPanelDrawsLine(
		*this, InspectedState, Inspected.HeadroomCaption, TEXT("the headroom bar's caption"));

	TestEqual(
		FString::Printf(
			TEXT("fixture: a bar over three decades should come with 4 ticks, the model offers %d"),
			Inspected.HeadroomScale.Num()),
		Inspected.HeadroomScale.Num(), 4);

	for (const FHeadroomScaleTick& Tick : Inspected.HeadroomScale)
	{
		CheckPanelDrawsLine(
			*this, InspectedState, Tick.Label,
			FString::Printf(TEXT("the headroom scale's '%s' tick"), *Tick.Label));
	}

	// Neither state may carry the other's lines.
	CheckPanelDrawsNoLine(
		*this, InspectedState, Idle.InspectedHintText,
		TEXT("the hint line, now that a brick IS singled out"));

	CheckPanelDrawsNoLine(
		*this, IdleState, Inspected.IdentityText,
		TEXT("the identity line, with no brick singled out to have an identity"));

	if (Inspected.Joints.Num() > 0)
	{
		CheckPanelDrawsNoLine(
			*this, IdleState, Inspected.Joints[0].Text,
			TEXT("a joint line, with nothing singled out"));

		CheckPanelDrawsNoLine(
			*this, IdleState, Inspected.HeadroomCaption,
			TEXT("the headroom caption, with no bar under it"));
	}

	Fixture.End();

	return true;
}

/**
 * The panel reads heading, brick list, readout, then the destructive action last.
 *
 * Delete is last so it is reached only past the description of what it destroys, and cannot be
 * hit by overshooting. Heading and count must share one row (compared by vertical centre).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuPanelOrdersItsRowsTest,
	"DestructionGame.World.Menu.DeleteIsTheLastRowUnderTheReadout",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuPanelOrdersItsRowsTest::RunTest(const FString& Parameters)
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

	// A full readout, so there is something for Delete to be underneath.
	Controller->SetInspectedPiece(Fixture.InspectedRef);

	const FPieceMenuInspector Inspector = Controller->PieceMenuInspectorForSelection();
	const FPanelLayoutState State = MeasurePanel(Panel, Root);

	AddInfo(FString::Printf(
		TEXT("buttons: %s"), *DescribePanelButtons(State.Buttons)));

	AddInfo(FString::Printf(
		TEXT("texts: %s"), *DescribePanelTexts(State.Texts)));

	const TArrayView<const FPieceMenuRow> Rows = Controller->GetShownPieceMenuRows();

	TestTrue(
		FString::Printf(
			TEXT("fixture: the selection should offer at least one action row, the presenter shows %d"),
			Rows.Num()),
		Rows.Num() >= 1);

	TestTrue(
		FString::Printf(
			TEXT("fixture: the singled-out brick should break out joints, it broke out %d"),
			Inspector.Joints.Num()),
		Inspector.Joints.Num() >= 3);

	if (Rows.Num() == 0 || Inspector.Pieces.Num() == 0)
	{
		Fixture.End();
		return true;
	}

	// The presenter's last action, not a hard-coded "Delete"; the action table is data.
	const FString& LastActionLabel = Rows.Last().Label;

	const FPanelButton* const LastAction = FindPanelButton(State.Buttons, LastActionLabel);

	if (LastAction == nullptr)
	{
		AddError(FString::Printf(
			TEXT("the panel must draw the action row '%s'; it drew %s"),
			*LastActionLabel, *DescribePanelButtons(State.Buttons)));

		Fixture.End();
		return true;
	}

	const double LastActionTopPx = LastAction->TopLeftPx.Y;

	const FPanelText* const Header = FindPanelLine(State.Texts, Inspector.HeaderText);
	const FPanelText* const Count = FindPanelLine(State.Texts, Inspector.CountText);

	if (Header == nullptr || Count == nullptr)
	{
		AddError(FString::Printf(
			TEXT("the panel must draw its heading ('%s') and its count ('%s'); it drew %s"),
			*Inspector.HeaderText, *Inspector.CountText, *DescribePanelTexts(State.Texts)));
	}
	else
	{
		TestTrue(
			*FString::Printf(
				TEXT("the heading and the count belong on one row: '%s' is centred at y %.2f and '%s' at y %.2f"),
				*Inspector.HeaderText, PanelCentreYPx(*Header),
				*Inspector.CountText, PanelCentreYPx(*Count)),
			FMath::Abs(PanelCentreYPx(*Header) - PanelCentreYPx(*Count)) <= PanelSameLineSlackPx);
	}

	// Each entry is checked against both ends, not just its neighbour.
	for (const FInspectorPieceEntry& Entry : Inspector.Pieces)
	{
		const FPanelButton* const Row = FindPanelButton(State.Buttons, Entry.Label);

		if (Row == nullptr)
		{
			AddError(FString::Printf(
				TEXT("entry row '%s' should be drawn; the panel drew %s"),
				*Entry.Label, *DescribePanelButtons(State.Buttons)));

			continue;
		}

		if (Header != nullptr)
		{
			TestTrue(
				*FString::Printf(
					TEXT("the heading must sit above entry row '%s': the heading is at y %.2f and the row at y %.2f"),
					*Entry.Label, Header->TopLeftPx.Y, Row->TopLeftPx.Y),
				Header->TopLeftPx.Y < Row->TopLeftPx.Y);
		}

		TestTrue(
			*FString::Printf(
				TEXT("'%s' must be the last row: entry row '%s' is at y %.2f and '%s' is at y %.2f"),
				*LastActionLabel, *Entry.Label, Row->TopLeftPx.Y, *LastActionLabel, LastActionTopPx),
			Row->TopLeftPx.Y < LastActionTopPx);
	}

	// Every readout line sits above the last action.
	TArray<FString> ReadoutLines;
	ReadoutLines.Add(Inspector.SupportText);
	ReadoutLines.Add(Inspector.JointsText);

	for (const FInspectorJointRow& Joint : Inspector.Joints)
	{
		ReadoutLines.Add(Joint.Text);
	}

	ReadoutLines.Add(Inspector.HeadroomCaption);

	for (const FString& Line : ReadoutLines)
	{
		if (Line.IsEmpty())
		{
			AddError(TEXT("the model must supply every readout line for the panel to place it"));
			continue;
		}

		const FPanelText* const Drawn = FindPanelLine(State.Texts, Line);

		if (Drawn == nullptr)
		{
			AddError(FString::Printf(
				TEXT("readout line '%s' should be drawn; the panel drew %s"),
				*Line, *DescribePanelTexts(State.Texts)));

			continue;
		}

		TestTrue(
			*FString::Printf(
				TEXT("'%s' must sit below readout line '%s': the line is at y %.2f and the row at y %.2f"),
				*LastActionLabel, *Line, Drawn->TopLeftPx.Y, LastActionTopPx),
			Drawn->TopLeftPx.Y < LastActionTopPx);
	}

	// No drawn button at all, accounted for or not, sits below it.
	for (const FPanelButton& Button : State.Buttons)
	{
		if (Button.Label == LastActionLabel)
		{
			continue;
		}

		TestTrue(
			*FString::Printf(
				TEXT("no clickable row may sit below '%s': '%s' is at y %.2f against y %.2f (%s)"),
				*LastActionLabel, *Button.Label, Button.TopLeftPx.Y, LastActionTopPx,
				*DescribePanelButtons(State.Buttons)),
			Button.TopLeftPx.Y < LastActionTopPx);
	}

	Fixture.End();

	return true;
}

/**
 * Picking forty bricks must not grow the panel or move Delete.
 *
 * States the consequence of the list cap rather than a pixel height: panel size (the mechanism)
 * and Delete's position (the outcome) are the same with three bricks and with forty.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuPanelCapsItsBrickListTest,
	"DestructionGame.World.Menu.PanelDoesNotGrowWithTheSelection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuPanelCapsItsBrickListTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;
	using namespace PieceMenuPanelLayoutTestSupport;

	FTallWallFixture Fixture;

	if (!Fixture.Begin(*this))
	{
		Fixture.End();
		return true;
	}

	ADestructionGamePlayerController* const Controller = Fixture.Controller;

	const FGeometry Root = PanelRootGeometry();

	Fixture.Pick(0, 2);

	TestEqual(
		FString::Printf(
			TEXT("fixture: three clicks should pick three bricks, the selection holds %d"),
			Controller->GetPieceSelection().Num()),
		Controller->GetPieceSelection().Num(), 3);

	// Measured before the long panel is built, which takes over the controller's readout box.
	const TSharedRef<SWidget> ShortPanel = Controller->BuildPieceMenuPanel();
	const FPanelLayoutState ShortState = MeasurePanel(ShortPanel, Root);

	AddInfo(FString::Printf(
		TEXT("3 bricks picked: panel desired size %.2f x %.2f px, buttons: %s"),
		ShortState.DesiredSizePx.X, ShortState.DesiredSizePx.Y,
		*DescribePanelButtons(ShortState.Buttons)));

	const TArrayView<const FPieceMenuRow> ShortRows = Controller->GetShownPieceMenuRows();

	TestTrue(
		FString::Printf(
			TEXT("fixture: three picked bricks should offer at least one action row, the presenter shows %d"),
			ShortRows.Num()),
		ShortRows.Num() >= 1);

	if (ShortRows.Num() == 0)
	{
		Fixture.End();
		return true;
	}

	const FString LastActionLabel = ShortRows.Last().Label;

	const FPanelButton* const ShortLastAction = FindPanelButton(ShortState.Buttons, LastActionLabel);

	// Pick the rest of the wall.
	Fixture.Pick(3, Fixture.Reference.Boxes.Num() - 1);

	const int32 LongCount = Controller->GetPieceSelection().Num();

	TestEqual(
		FString::Printf(
			TEXT("fixture: every brick of the wall should be picked, %d of %d are"),
			LongCount, Fixture.Reference.Boxes.Num()),
		LongCount, Fixture.Reference.Boxes.Num());

	const TSharedRef<SWidget> LongPanel = Controller->BuildPieceMenuPanel();
	const FPanelLayoutState LongState = MeasurePanel(LongPanel, Root);

	AddInfo(FString::Printf(
		TEXT("%d bricks picked: panel desired size %.2f x %.2f px, %d button(s)"),
		LongCount, LongState.DesiredSizePx.X, LongState.DesiredSizePx.Y, LongState.Buttons.Num()));

	// Fixture: the model lists them all, or the size claims pass vacuously.
	const FPieceMenuInspector LongInspector = Controller->PieceMenuInspectorForSelection();

	TestEqual(
		FString::Printf(
			TEXT("fixture: the readout model should list all %d picked bricks, it lists %d"),
			LongCount, LongInspector.Pieces.Num()),
		LongInspector.Pieces.Num(), LongCount);

	TestTrue(
		FString::Printf(
			TEXT("fixture: the long selection must actually be long, it holds %d against the %d this test is about"),
			LongCount, PanelLongSelectionAtLeast),
		LongCount >= PanelLongSelectionAtLeast);

	TestTrue(
		*FString::Printf(
			TEXT("the panel must be the same size whatever is picked: %.2f x %.2f px with 3 bricks, %.2f x %.2f px with %d"),
			ShortState.DesiredSizePx.X, ShortState.DesiredSizePx.Y,
			LongState.DesiredSizePx.X, LongState.DesiredSizePx.Y, LongCount),
		FMath::IsNearlyEqual(ShortState.DesiredSizePx.X, LongState.DesiredSizePx.X, PanelRowMustNotMovePx)
			&& FMath::IsNearlyEqual(ShortState.DesiredSizePx.Y, LongState.DesiredSizePx.Y, PanelRowMustNotMovePx));

	const FPanelButton* const LongLastAction = FindPanelButton(LongState.Buttons, LastActionLabel);

	if (ShortLastAction == nullptr || LongLastAction == nullptr)
	{
		AddError(FString::Printf(
			TEXT("the action row '%s' should be drawn for both selections; 3 bricks: %s / %d bricks: %s"),
			*LastActionLabel, *DescribePanelButtons(ShortState.Buttons), LongCount,
			*DescribePanelButtons(LongState.Buttons)));

		Fixture.End();
		return true;
	}

	TestTrue(
		*FString::Printf(
			TEXT("'%s' must not move when the list grows: it was at y %.2f with 3 bricks and is at y %.2f with %d, a shift of %.2f px"),
			*LastActionLabel, ShortLastAction->TopLeftPx.Y, LongLastAction->TopLeftPx.Y, LongCount,
			LongLastAction->TopLeftPx.Y - ShortLastAction->TopLeftPx.Y),
		FMath::IsNearlyEqual(
			ShortLastAction->TopLeftPx.Y, LongLastAction->TopLeftPx.Y, PanelRowMustNotMovePx));

	TestTrue(
		*FString::Printf(
			TEXT("'%s' must still be on screen with %d bricks picked: it is at y %.2f in a %.0f px viewport"),
			*LastActionLabel, LongCount, LongLastAction->TopLeftPx.Y, PanelViewportHeightPx),
		LongLastAction->TopLeftPx.Y + LongLastAction->SizePx.Y <= PanelViewportHeightPx);

	Fixture.End();

	return true;
}

/**
 * Every decade tick lies inside the bar it labels and still covers its own fraction.
 *
 * The end labels were centred on the track's edges and half-clipped. The strip is found as the
 * ticks' common ancestor, not a named slot. Covering the fraction rules out the wrong fix of
 * spacing labels evenly, which would misalign ticks and fill.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuPanelKeepsItsTicksOnTheBarTest,
	"DestructionGame.World.Menu.HeadroomTicksStayInsideTheBarTheyLabel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuPanelKeepsItsTicksOnTheBarTest::RunTest(const FString& Parameters)
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

	// No bar or scale until a brick is singled out.
	Controller->SetInspectedPiece(Fixture.InspectedRef);

	const FPieceMenuInspector Inspector = Controller->PieceMenuInspectorForSelection();
	const FPanelLayoutState State = MeasurePanel(Panel, Root);

	const TArray<FHeadroomScaleTick>& Scale = Inspector.HeadroomScale;

	TestTrue(
		FString::Printf(
			TEXT("fixture: a bar over three decades should come with 4 ticks, the model offers %d"),
			Scale.Num()),
		Scale.Num() == 4);

	if (Scale.Num() != 4)
	{
		Fixture.End();
		return true;
	}

	// Fixture: the scale reaches both ends, where the clipping happens.
	TestEqual(
		FString::Printf(
			TEXT("fixture: the lowest tick should sit at the empty end of the bar, it sits at %.4f"),
			Scale[0].Fraction),
		Scale[0].Fraction, 0.0);

	TestEqual(
		FString::Printf(
			TEXT("fixture: the highest tick should sit at the full end of the bar, it sits at %.4f"),
			Scale.Last().Fraction),
		Scale.Last().Fraction, 1.0);

	for (int32 Index = 1; Index < Scale.Num(); ++Index)
	{
		TestTrue(
			*FString::Printf(
				TEXT("fixture: the ticks should climb the bar in order, #%d is at %.4f and #%d at %.4f"),
				Index - 1, Scale[Index - 1].Fraction, Index, Scale[Index].Fraction),
			Scale[Index].Fraction > Scale[Index - 1].Fraction);
	}

	// Exactly one drawn widget per tick label.
	TArray<int32> TickWidgets;

	for (const FHeadroomScaleTick& Tick : Scale)
	{
		const TArray<int32> Drawn = FindPanelWidgetsByText(State.Widgets, Tick.Label);

		TestEqual(
			FString::Printf(
				TEXT("the panel must draw the '%s' tick exactly once, it drew it %d time(s): %s"),
				*Tick.Label, Drawn.Num(), *DescribePanelTexts(State.Texts)),
			Drawn.Num(), 1);

		if (Drawn.Num() != 1)
		{
			Fixture.End();
			return true;
		}

		TickWidgets.Add(Drawn[0]);
	}

	// The track: the lowest widget with all four ticks beneath it.
	const int32 TrackIndex = PanelCommonAncestor(State.Widgets, TickWidgets);

	if (!State.Widgets.IsValidIndex(TrackIndex))
	{
		AddError(TEXT("the tick labels should share a region of the panel; no common parent was found"));

		Fixture.End();
		return true;
	}

	const FPanelWidget& Track = State.Widgets[TrackIndex];

	const double TrackLeftPx = Track.TopLeftPx.X;
	const double TrackWidthPx = Track.SizePx.X;
	const double TrackRightPx = TrackLeftPx + TrackWidthPx;

	AddInfo(FString::Printf(TEXT("the tick strip: %s"), *DescribePanelWidget(Track)));

	for (int32 Index = 0; Index < TickWidgets.Num(); ++Index)
	{
		AddInfo(FString::Printf(
			TEXT("tick '%s' at fraction %.4f: %s"),
			*Scale[Index].Label, Scale[Index].Fraction,
			*DescribePanelWidget(State.Widgets[TickWidgets[Index]])));
	}

	// Fixture: the strip is narrower than the panel, not the root.
	TestTrue(
		*FString::Printf(
			TEXT("fixture: the ticks should share a region of the panel, theirs is %.2f px wide against a %.2f px panel"),
			TrackWidthPx, State.DesiredSizePx.X),
		TrackWidthPx > 0.0 && TrackWidthPx < State.DesiredSizePx.X);

	if (!(TrackWidthPx > 0.0))
	{
		Fixture.End();
		return true;
	}

	/*
	 * The strip must not extend past the bars, whose right edge is where the joint sentences
	 * begin; otherwise the top tick sits where no fill reaches.
	 */
	double JointTextLeftPx = TNumericLimits<double>::Max();

	for (const FInspectorJointRow& Joint : Inspector.Joints)
	{
		if (const FPanelText* const Line = FindPanelLine(State.Texts, Joint.Text))
		{
			JointTextLeftPx = FMath::Min(JointTextLeftPx, Line->TopLeftPx.X);
		}
	}

	TestTrue(
		FString::Printf(
			TEXT("fixture: the singled-out brick should draw joint lines for the scale to sit under, it broke out %d"),
			Inspector.Joints.Num()),
		Inspector.Joints.Num() >= 3 && JointTextLeftPx < TNumericLimits<double>::Max());

	if (JointTextLeftPx < TNumericLimits<double>::Max())
	{
		TestTrue(
			*FString::Printf(
				TEXT("the decade scale must stay under the column of bars: it spans x %.2f..%.2f, and the bars end where their lines begin at x %.2f"),
				TrackLeftPx, TrackRightPx, JointTextLeftPx),
			TrackRightPx <= JointTextLeftPx + PanelSpanContainmentSlackPx);
	}

	for (int32 Index = 0; Index < TickWidgets.Num(); ++Index)
	{
		const FPanelWidget& Label = State.Widgets[TickWidgets[Index]];

		const double LabelLeftPx = Label.TopLeftPx.X;
		const double LabelRightPx = LabelLeftPx + Label.SizePx.X;

		TestTrue(
			*FString::Printf(
				TEXT("fixture: the '%s' tick should be drawn wide enough to read, it is %.2f px wide"),
				*Scale[Index].Label, Label.SizePx.X),
			Label.SizePx.X > 0.0);

		// A half-clipped decade label reads as a different number.
		TestTrue(
			*FString::Printf(
				TEXT("the '%s' tick must lie inside the bar it labels: it spans x %.2f..%.2f against a track of %.2f..%.2f, overhanging by %.2f px left and %.2f px right"),
				*Scale[Index].Label, LabelLeftPx, LabelRightPx, TrackLeftPx, TrackRightPx,
				TrackLeftPx - LabelLeftPx, LabelRightPx - TrackRightPx),
			LabelLeftPx >= TrackLeftPx - PanelSpanContainmentSlackPx
				&& LabelRightPx <= TrackRightPx + PanelSpanContainmentSlackPx);

		// And it still covers the point a fill with that margin reaches.
		const double MarkedPx = TrackLeftPx + Scale[Index].Fraction * TrackWidthPx;

		TestTrue(
			*FString::Printf(
				TEXT("the '%s' tick must cover the point the bar fills to at %.4f of its width, x %.2f: the label spans %.2f..%.2f"),
				*Scale[Index].Label, Scale[Index].Fraction, MarkedPx, LabelLeftPx, LabelRightPx),
			LabelLeftPx <= MarkedPx + PanelSpanContainmentSlackPx
				&& LabelRightPx >= MarkedPx - PanelSpanContainmentSlackPx);

		// Labels read low to high across the bar.
		if (Index > 0)
		{
			const FPanelWidget& Previous = State.Widgets[TickWidgets[Index - 1]];

			const double PreviousCentrePx = Previous.TopLeftPx.X + Previous.SizePx.X * 0.5;
			const double CentrePx = LabelLeftPx + Label.SizePx.X * 0.5;

			TestTrue(
				*FString::Printf(
					TEXT("the ticks must read low to high across the bar: '%s' is centred at x %.2f and '%s' at x %.2f"),
					*Scale[Index - 1].Label, PreviousCentrePx, *Scale[Index].Label, CentrePx),
				CentrePx > PreviousCentrePx);

			/*
			 * And they must not touch: "100×" and "1000×" were 0.33 px apart, in order and inside the
			 * bar, reading as one number. Widening the bar costs the joint sentences their width
			 * (see World.Menu.TheReadoutFitsInsideThePanel).
			 */
			const double PreviousRightPx = Previous.TopLeftPx.X + Previous.SizePx.X;
			const double GapPx = LabelLeftPx - PreviousRightPx;

			TestTrue(
				*FString::Printf(
					TEXT("adjacent tick labels must be readable apart: '%s' ends at x %.2f and '%s' begins at x %.2f, a gap of %.2f px against the %.2f px this needs"),
					*Scale[Index - 1].Label, PreviousRightPx, *Scale[Index].Label, LabelLeftPx,
					GapPx, PanelTickLabelGapPx),
				GapPx >= PanelTickLabelGapPx);
		}
	}

	Fixture.End();

	return true;
}

/**
 * Every readout line is drawn inside the panel, and the spare width is reported.
 *
 * The content column is measured off the full-width action row. Every model line is swept, since
 * which is longest depends on wall and units. Two walls: the flush bond bends nowhere so cannot
 * see the panel's width; the corbel wall prints the longest (bending) line. Both detail modes:
 * compact must be strictly smaller on both axes and still fit every line it shows.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuPanelFitsItsReadoutTest,
	"DestructionGame.World.Menu.TheReadoutFitsInsideThePanel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuPanelFitsItsReadoutTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;
	using namespace PieceMenuPanelLayoutTestSupport;

	// The flush wall: the common case.
	{
		FPanelFixture Fixture;

		if (Fixture.Begin(*this))
		{
			const TCHAR* const Wall = TEXT("a flush running bond, where nothing bends");

			const FPanelFitMeasurement Full = SweepReadoutFitsInsideThePanel(
				*this, Fixture.Controller, Fixture.InspectedRef, Wall, EPieceMenuDetail::Full);

			const FPanelFitMeasurement Compact = SweepReadoutFitsInsideThePanel(
				*this, Fixture.Controller, Fixture.InspectedRef, Wall, EPieceMenuDetail::Compact);

			CheckCompactPanelGivesScreenBack(*this, Full, Compact, Wall);
		}

		Fixture.End();
	}

	// The corbel wall, whose bending line is the longest sentence.
	{
		FBendingPanelFixture Fixture;

		if (Fixture.Begin(*this))
		{
			const TCHAR* const Wall =
				TEXT("a ragged wall with a corbel, where one joint is levered open");

			const FPanelFitMeasurement Full = SweepReadoutFitsInsideThePanel(
				*this, Fixture.Controller, Fixture.InspectedRef, Wall, EPieceMenuDetail::Full);

			const FPanelFitMeasurement Compact = SweepReadoutFitsInsideThePanel(
				*this, Fixture.Controller, Fixture.InspectedRef, Wall, EPieceMenuDetail::Compact);

			CheckCompactPanelGivesScreenBack(*this, Full, Compact, Wall);
		}

		Fixture.End();
	}

	return true;
}

/**
 * A short selection must not leave a band of empty panel above the readout.
 *
 * A fixed-height list left ~145 px of nothing under three bricks; a capped list avoids it. The gap
 * from the last brick row to the readout's first line must be at most one measured row height.
 * The readout's first line may move with the count; it is not clickable, so there is no feedback.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuPanelFitsItsBrickListTest,
	"DestructionGame.World.Menu.AShortListLeavesNoDeadSpaceAboveTheReadout",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuPanelFitsItsBrickListTest::RunTest(const FString& Parameters)
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

	/*
	 * Nothing singled out: the worst case, and the only state whose first readout line (the hint)
	 * is distinguishable from an entry row.
	 */
	const FPieceMenuInspector Inspector = Controller->PieceMenuInspectorForSelection();
	const FPanelLayoutState State = MeasurePanel(Panel, Root);

	AddInfo(FString::Printf(
		TEXT("3 bricks picked, none singled out: buttons %s"), *DescribePanelButtons(State.Buttons)));

	TestEqual(
		FString::Printf(
			TEXT("fixture: the readout model should list 3 entries, it lists %d"),
			Inspector.Pieces.Num()),
		Inspector.Pieces.Num(), 3);

	if (Inspector.Pieces.Num() != 3)
	{
		Fixture.End();
		return true;
	}

	// The lowest entry row by position, whichever brick it names.
	const FPanelButton* Last = nullptr;

	for (const FInspectorPieceEntry& Entry : Inspector.Pieces)
	{
		const FPanelButton* const Row = FindPanelButton(State.Buttons, Entry.Label);

		if (Row == nullptr)
		{
			AddError(FString::Printf(
				TEXT("entry row '%s' should be drawn; the panel drew %s"),
				*Entry.Label, *DescribePanelButtons(State.Buttons)));

			continue;
		}

		if (Last == nullptr || Row->TopLeftPx.Y > Last->TopLeftPx.Y)
		{
			Last = Row;
		}
	}

	const FPanelText* const FirstReadoutLine =
		FindPanelLine(State.Texts, Inspector.InspectedHintText);

	if (Last == nullptr || FirstReadoutLine == nullptr)
	{
		AddError(FString::Printf(
			TEXT("the panel must draw the brick rows and the readout's first line ('%s'); it drew %s"),
			*Inspector.InspectedHintText, *DescribePanelTexts(State.Texts)));

		Fixture.End();
		return true;
	}

	// Fixture: a real row height, and the readout below the list (else the gap passes negative).
	const double RowHeightPx = Last->SizePx.Y;

	TestTrue(
		*FString::Printf(
			TEXT("fixture: an entry row should have a real height, it measured %.2f px"), RowHeightPx),
		RowHeightPx > 0.0);

	const double LastRowBottomPx = Last->TopLeftPx.Y + RowHeightPx;
	const double ReadoutTopPx = FirstReadoutLine->TopLeftPx.Y;

	TestTrue(
		*FString::Printf(
			TEXT("fixture: the readout should sit below the brick list: the last row ends at y %.2f and the readout starts at y %.2f"),
			LastRowBottomPx, ReadoutTopPx),
		ReadoutTopPx >= LastRowBottomPx);

	if (!(RowHeightPx > 0.0) || ReadoutTopPx < LastRowBottomPx)
	{
		Fixture.End();
		return true;
	}

	const double DeadSpacePx = ReadoutTopPx - LastRowBottomPx;

	TestTrue(
		*FString::Printf(
			TEXT("with 3 bricks picked the panel must not strand the readout: %.2f px of nothing between the last brick row (ending y %.2f) and '%s' (starting y %.2f), against a row %.2f px tall"),
			DeadSpacePx, LastRowBottomPx, *Inspector.InspectedHintText, ReadoutTopPx, RowHeightPx),
		DeadSpacePx <= RowHeightPx);

	Fixture.End();

	return true;
}

/**
 * No button in the piece menu may take keyboard focus.
 *
 * A focused SButton swallows Enter (Run) and Space (jump) until the viewport is clicked again,
 * which matters since S6's permanent cursor. The session toolbar's chips already opt out. Buttons
 * are built at two sites, so every drawn button is swept, with at least two required.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuButtonsNeverTakeFocusTest,
	"DestructionGame.World.Menu.PieceMenuButtonsNeverTakeFocus",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuButtonsNeverTakeFocusTest::RunTest(const FString& Parameters)
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

	// Singled out, so the panel is in its fullest configuration.
	Controller->SetInspectedPiece(Fixture.InspectedRef);

	const TSharedRef<SWidget> Panel = Controller->BuildPieceMenuPanel();

	const FPanelLayoutState State = MeasurePanel(Panel, PanelRootGeometry());

	AddInfo(FString::Printf(
		TEXT("the panel drew: %s"), *DescribePanelButtons(State.Buttons)));

	TestTrue(
		*FString::Printf(
			TEXT("fixture: the panel must draw at least 2 buttons — an entry row and an action row — "
				 "for a sweep over its buttons to mean anything; it drew %d [%s]"),
			State.Buttons.Num(), *DescribePanelButtons(State.Buttons)),
		State.Buttons.Num() >= 2);

	int32 Focusable = 0;

	for (const FPanelButton& Button : State.Buttons)
	{
		Focusable += Button.bFocusable ? 1 : 0;

		TestTrue(
			*FString::Printf(
				TEXT("THE PIECE MENU'S '%s' MUST NOT TAKE KEYBOARD FOCUS. Slate focuses a focusable "
					 "widget on click and SButton::OnKeyDown then swallows Enter and Space, so a player "
					 "who clicks this row loses Run (Enter) and the pawn's jump (Space) until they "
					 "click the viewport again — with nothing on screen and nothing in the log to say "
					 "why. SupportsKeyboardFocus reports %d"),
				*Button.Label, Button.bFocusable ? 1 : 0),
			!Button.bFocusable);
	}

	TestEqual(
		FString::Printf(
			TEXT("and NONE of the panel's %d button(s) may, not merely most of them — the entry rows "
				 "and the action rows are built at two different sites and a fix applied to one leaves "
				 "the other stealing the keyboard. %d still do [%s]"),
			State.Buttons.Num(), Focusable, *DescribePanelButtons(State.Buttons)),
		Focusable, 0);

	Fixture.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
