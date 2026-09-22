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
 * Named, and uniquely: an anonymous namespace is per-translation-unit, and a unity build merges
 * files into one (CURRENT_STATE.md). The `using namespace` sits inside RunTest for the same
 * reason. The world harness lives in Tests/BrickWorldTestSupport.h, shared by every World.* test.
 */
namespace PieceMenuPanelLayoutTestSupport
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;

	/**
	 * Y reach of the pick ray, either side of the wall. A brick is 10.25 cm deep and the wall is
	 * centred on Y = 0, so +/-100 cm clears it both sides. Along Y so nothing else is in the way.
	 */
	constexpr double PanelRayReachCm = 100.0;

	/**
	 * The space the panel is laid out in, px. A root geometry, not a real viewport: Slate layout is
	 * arithmetic on desired sizes, needing no renderer, so button positions are measurable headlessly.
	 * 1920x1080 is arbitrary; the assertions compare two states in the same space, so the size cancels.
	 */
	constexpr float PanelViewportWidthPx = 1920.0f;
	constexpr float PanelViewportHeightPx = 1080.0f;

	/**
	 * How far a menu entry may move when the readout changes. Slack on an exact equality, not a
	 * tolerance: two layouts of one tree in one geometry are bit-identical, and the defect moves a
	 * row tens of pixels.
	 */
	constexpr double PanelRowMustNotMovePx = 0.5;

	/**
	 * Slack on the horizontal containment claim, px. Same reasoning as the vertical slack: layouts
	 * are bit-identical, and the defect it catches is tens of pixels wide.
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
	 * One drawn button: its text, position and size. The size is kept for the horizontal claim:
	 * the panel is centred, so a row's left edge moves as the readout widens, harmless only while
	 * the narrower span stays inside the wider one. An origin alone cannot say that.
	 */
	struct FPanelButton
	{
		FString Label;
		FVector2D TopLeftPx = FVector2D::ZeroVector;
		FVector2D SizePx = FVector2D::ZeroVector;

		/**
		 * Whether clicking it takes user focus. Defaulted true, matching SButton's own default so a
		 * collector that failed to fill it in reports the hazard rather than clears it.
		 */
		bool bFocusable = true;
	};

	/**
	 * One drawn line of text and its rectangle. Separate from FPanelButton because most of the
	 * panel is not clickable: the readout is all text, and ordering claims are about where it sits
	 * relative to the Delete row.
	 */
	struct FPanelText
	{
		FString Text;
		FVector2D TopLeftPx = FVector2D::ZeroVector;
		FVector2D SizePx = FVector2D::ZeroVector;
	};

	/**
	 * Every STextBlock under a widget, concatenated — for a button, its caption. Buttons are
	 * identified by text, not slot index: the fix under test may change the structure, but not
	 * where a readable row lands.
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
	 * Arrange the tree in this geometry and record where every button landed. Arranged, not
	 * painted: ArrangeChildren is the renderer's own placement call, const and device-free, so it
	 * gives absolute positions with no RHI. Cached geometry would not do; nothing caches until paint.
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

			/*
			 * Asked of the widget, not an SButton cast: SupportsKeyboardFocus is SWidget's virtual and
			 * SButton's override answers it — the same answer Slate gets when a click puts focus.
			 */
			Button.bFocusable = Widget->SupportsKeyboardFocus();
		}

		FArrangedChildren Arranged(EVisibility::All);

		Widget->ArrangeChildren(Geometry, Arranged);

		for (int32 Index = 0; Index < Arranged.Num(); ++Index)
		{
			CollectPanelButtons(Arranged[Index].Widget, Arranged[Index].Geometry, Out);
		}
	}

	/**
	 * Arrange the tree and record every line of text and where it landed. The same arrange the
	 * button collector uses: an arranged-but-unpainted text block still has a position, which is
	 * all "the readout sits above Delete" means.
	 */
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

	/** How many drawn lines read exactly this. Zero is the assertion as often as one is. */
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

	/**
	 * How many joints touch a piece, counted off the graph. A fixture precondition, not a panel
	 * assertion: the readout grows one line per joint, so too few joints make the growth too small
	 * to move a row and the test passes over the defect.
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
	 * The shared fixture the tests measure, built once rather than in each RunTest. They differ only
	 * in which rows they hold still, which is only evidence while the wall, the three picked bricks
	 * and the singled-out brick's joint count stay identical; two copies could drift apart.
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
			 * Three bricks picked, so three entry rows. One row cannot show the defect: it is the rows
			 * above the readout that move, and one entry is barely a stack to shift.
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
			 * The singled-out brick needs enough joints to grow the readout. Brick 1 here is spanned by
			 * two above and has a neighbour each side; the count is asserted, not assumed, so a wall
			 * shape that changed it cannot make the growth too small and pass over the defect.
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

	/** How much of two boxes' spans overlap along one axis, and where the middle of that is. */
	double PanelOverlapCentreCm(const FPieceBox& A, const FPieceBox& B, int32 Axis)
	{
		const double LowCm = FMath::Max(
			A.CentreCm[Axis] - A.ExtentCm[Axis], B.CentreCm[Axis] - B.ExtentCm[Axis]);

		const double HighCm = FMath::Min(
			A.CentreCm[Axis] + A.ExtentCm[Axis], B.CentreCm[Axis] + B.ExtentCm[Axis]);

		return (LowCm + HighCm) * 0.5;
	}

	/**
	 * The same panel over a wall that actually bends, a sibling of FPanelFixture rather than a
	 * widening of it.
	 *
	 * Why the shared fixture cannot show this: FPanelFixture is a flush bond, where every joint is
	 * either a brick on two symmetric bed patches (indeterminate, so the moment rule leaves it at
	 * zero) or a half bat centred on the brick below. Neither is eccentric, so no line carries the
	 * bending clause and the sweep passes identically at 560 px or 680 px, blind to the panel's width.
	 *
	 * Why a sibling and not a wider shared fixture: five tests measure FPanelFixture, and a bending
	 * joint in it would change the readout's width and height in all five for a reason unrelated to
	 * what any of them asserts. The sibling costs one more world and panel build, in one test.
	 *
	 * The shape is the producer's own. NarrowWaistWallSpec is a ragged bond, so the courses above
	 * the waist step back out over it:
	 *
	 *      course 4            [ 5 ]
	 *      course 3         [ 3 ][ 4 ]      each on one patch, off-centre
	 *      course 2            [ 2 ]        the waist — the brick singled out
	 *      course 1         [ 0 ][ 1 ]      grounded
	 *
	 * Brick 3 spans x -10.75..10.75 and the waist below spans 0.5..22.0, so their bed patch runs
	 * 0.5..10.75, centred at x 5.625 — half the bond offset from brick 3's centre of mass at x 0.
	 * One off-centre support is the determinate case the moment rule answers, so that joint bends
	 * and its line grows the clause; brick 4 mirrors it. The waist's two joints to the ground are
	 * unbent, so this fixture prints both kinds of line. The lever arm is asserted from the laid
	 * boxes and the moment from the graph, so a producer retuned to lose the offset fails here
	 * rather than silently becoming a second flush fixture.
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

		/** Course 2's single brick: the one every corbel above it leans on. */
		static constexpr int32 WaistPiece = 2;

		/** Course 3's left-hand brick, which sits on the waist and hangs half off it. */
		static constexpr int32 CorbelPiece = 3;

		static constexpr int32 BendingWallPieceCount = 6;

		/**
		 * A quarter of the grid's brick pitch. A 21.5 cm brick plus a 1 cm joint is a 22.5 cm cell;
		 * a stepped-out brick's overhanging patch centre sits a quarter cell, 5.625 cm, from its
		 * own middle. Computed from the spec so a different brick states its own arm.
		 */
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

			/*
			 * The eccentricity, measured off the laid boxes not the solver: the corbel's bed patch
			 * is its overlap with the waist, whose middle is a quarter cell from the corbel's centre
			 * of mass. Off the geometry, not GetConnectionMoment, so a disagreement names the culprit.
			 */
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

			/* Three bricks picked, as the flush fixture picks three: the pair and the waist. */
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

			/*
			 * And the joint this fixture exists for is carrying a bend. The lever arm says the
			 * geometry is eccentric; this says the solver answered it — the only state where a
			 * readout line grows a clause.
			 */
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

	/**
	 * A wall with more bricks than a list can sensibly show, which the shared 7-brick WallSpec is
	 * not. Ten flush courses of four is about 45 pieces — the 40-brick selection the rebuild must
	 * survive; the count is asserted below so a halved RunningBond cannot pass over the defect. It
	 * stays inside the harness floor: four bricks reach X 78.25 on a +/-700 slab, ten courses 75 cm.
	 */
	FRunningBondSpec TallWallSpec()
	{
		FRunningBondSpec Spec = WallSpec();
		Spec.CoursesHigh = 10;
		Spec.BricksPerCourse = 4;

		return Spec;
	}

	/** How many bricks the cap test insists on picking before it believes it has a long list. */
	constexpr int32 PanelLongSelectionAtLeast = 40;

	/** A world holding that wall, and a controller to pick out of it. Nothing is picked yet. */
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

		/** Click bricks [First, Last] of the reference layout, which picks each of them once. */
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

	/**
	 * One arranged widget of any kind, plus its parent. The parent link lets a region be named
	 * without naming a slot: "the ticks lie inside the bar's strip" needs that strip's rectangle,
	 * found by walking the ticks up to their common ancestor whatever it is built from.
	 */
	struct FPanelWidget
	{
		FString Type;

		/** What it reads, for an STextBlock. Empty for every other kind of widget. */
		FString Text;

		FVector2D TopLeftPx = FVector2D::ZeroVector;
		FVector2D SizePx = FVector2D::ZeroVector;

		/**
		 * How big it asked to be; for text, the room the glyphs need. Arranged size cannot tell if
		 * text is cut off: a filling slot arranges its child at the whole column width whatever it
		 * asked for, so measuring that against the panel compares the panel with itself. Desired size
		 * is what the renderer must honour to draw every glyph. Valid because MeasurePanel's
		 * SlatePrepass runs before this walk.
		 */
		FVector2D DesiredSizePx = FVector2D::ZeroVector;

		int32 ParentIndex = INDEX_NONE;
	};

	/**
	 * The same arrange walk the button and text collectors do, keeping every widget. Arranged, not
	 * painted, for the reason they give: ArrangeChildren is the renderer's const, device-free
	 * placement call, so the whole tree's absolute geometry is reachable with no RHI.
	 */
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

	/** Every widget that reads exactly this, by index. A tick label should be found once. */
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
	 * The panel's own rectangle: the constraint canvas's one child, arranged. Not the canvas (which
	 * is arranged at the whole viewport) and not GetDesiredSize (which bakes in the drag offset, so
	 * it moves when the player drags). The child's arranged rectangle is the panel itself. Found by
	 * parentage, not index, so it survives a wrapper being added around the box.
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

	/** One laid-out state of the panel: what it measured to, and where everything landed. */
	struct FPanelLayoutState
	{
		FVector2D DesiredSizePx = FVector2D::ZeroVector;
		TArray<FPanelButton> Buttons;
		TArray<FPanelText> Texts;
		TArray<FPanelWidget> Widgets;
	};

	/**
	 * Prepass and arrange the tree, which is the whole measurement. The panel is built once and
	 * measured twice by calling this twice: SetInspectedPiece swaps only the readout's content, so
	 * both states are the same tree, which makes "the row moved" a layout claim, not two panels.
	 */
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

	/**
	 * The panel prints this line, and the model had one to print. The emptiness check matters: an
	 * empty expected string matches nothing, or some other empty block, asserting nothing while
	 * looking like it does — so a missing model string is reported as a model fault.
	 */
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

	/** And this one it must NOT print. Only ever called with a line some other state supplied. */
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

	/**
	 * How far apart two texts' vertical centres may be and still count as one line. A row is about
	 * 22 px, so 8 px is under half a row: texts this close cannot be in adjacent rows. Slack on a
	 * claim about rows, not a measurement tolerance.
	 */
	constexpr double PanelSameLineSlackPx = 8.0;

	double PanelCentreYPx(const FPanelText& Text)
	{
		return Text.TopLeftPx.Y + Text.SizePx.Y * 0.5;
	}

	/**
	 * Clear space two adjacent decade labels need between them. Not a tolerance but the claim: the
	 * smallest gap at which "100×" and "1000×" read as two numbers, not "100×1000×". 4 px is over
	 * half a digit at the scale's font. The measured failure is 0.33 px, so the test is far from
	 * its boundary. In pixels, not a fraction of the track: the glyphs must be readable and glyphs
	 * do not scale with the bar.
	 */
	constexpr double PanelTickLabelGapPx = 4.0;

	/**
	 * What one sweep measured, so the two detail modes can be compared. The panel's own rectangle
	 * is here because the player's complaint is about how much screen it covers, a separate claim
	 * from whether its sentences fit; a panel that shrank and clipped its text is the worse answer.
	 */
	struct FPanelFitMeasurement
	{
		bool bMeasured = false;

		/** The panel as arranged: where its corner is, and how much screen it covers. */
		FVector2D PanelTopLeftPx = FVector2D::ZeroVector;
		FVector2D PanelSizePx = FVector2D::ZeroVector;

		/** The column everything inside it has to fit in, measured off the action row. */
		double ContentLeftPx = 0.0;
		double ContentRightPx = 0.0;

		/** How much room the tightest line had left, and which line that was. */
		double TightestSlackPx = 0.0;
		FString TightestLine;
	};

	/**
	 * Sweep every line the selection's readout supplies and hold it inside the panel's column.
	 *
	 * A function, not a RunTest body, because the one claim now spans two walls and two detail modes:
	 * a flush bond bends nowhere so its sweep cannot see the panel's width, a corbel wall prints the
	 * longest line. The mode changes which line is longest, so compact needs its own sweep: full's
	 * width came from the longest joint line (617.5 px on the ragged wall), which compact drops,
	 * leaving an entry row widest — so entry rows are swept in both modes, or a compact panel could
	 * be narrowed to "3 bricks selected" while the brick list ran off the edge.
	 *
	 * @param WallDescription names which wall a failure came off, since both run in one test.
	 * @param Detail which mode to build and measure. The controller is left in it.
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

		/* The mode is set before the build, because the build reads it. */
		Controller->SetPieceMenuDetail(Detail);

		const TSharedRef<SWidget> Panel = Controller->BuildPieceMenuPanel();
		const FGeometry Root = PanelRootGeometry();

		/* The readout is only full — and only as wide as it gets — with a brick singled out. */
		Controller->SetInspectedPiece(InspectedRef);

		const FPieceMenuInspector Inspector = Controller->PieceMenuInspectorForSelection(Detail);
		const FPanelLayoutState State = MeasurePanel(Panel, Root);

		const TArrayView<const FPieceMenuRow> Rows = Controller->GetShownPieceMenuRows();

		Test.TestTrue(
			FString::Printf(
				TEXT("fixture (%s, %s): the selection should offer at least one action row to measure the panel by, the presenter shows %d"),
				WallDescription, DetailName, Rows.Num()),
			Rows.Num() >= 1);

		/*
		 * Fixture: the full readout has a joint table, the compact one has dropped it. Asked of the
		 * same controller in the same state, so a compact sweep over a jointless brick says so.
		 */
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

		/*
		 * Fixture: the column is real. A zero-width action row would make every containment claim
		 * below impossible for no readout reason; one wider than the viewport would make them free.
		 */
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
		 * The panel's rectangle, and the claim it is the presenter's answer. This join has been got
		 * wrong before: PieceMenuPanelSizePx sets how much screen a mode may take, but an SBox
		 * override is only a desired size and a filling slot hands its child any width — a 540 px
		 * canvas once sat over 96 px bars. Reading the arranged rectangle back makes "the widget
		 * honours the model" a measurement, not a reading of the source.
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

		/*
		 * Every line the model supplied, taken from the model not read back off the panel: a line the
		 * panel failed to draw is then a missing widget, not a silently skipped assertion.
		 */
		TArray<FString> ReadoutLines;
		ReadoutLines.AddUnique(Inspector.InspectedLabel);
		ReadoutLines.AddUnique(Inspector.SupportText);
		ReadoutLines.AddUnique(Inspector.JointsText);

		/*
		 * And the brick list, which sets a compact panel's width: with the joint table gone, an entry
		 * row (dot, position label, support word in a fixed column) is the widest thing left. Swept in
		 * both modes because a claim that changes shape with the mode is two claims. AddUnique
		 * throughout: the inspected brick's label is drawn twice (its entry row and the heading) and
		 * two bricks can share a support word, so duplicates would only assert the same widgets twice.
		 */
		for (const FInspectorPieceEntry& Entry : Inspector.Pieces)
		{
			ReadoutLines.AddUnique(Entry.Label);
			ReadoutLines.AddUnique(Entry.SupportText);
		}

		for (const FInspectorJointRow& Joint : Inspector.Joints)
		{
			ReadoutLines.AddUnique(Joint.Text);
		}

		/*
		 * The caption and ticks are asked for only when there is a bar. The model leaves both empty
		 * with no bar (the compact state), and the loop below reports an empty line as a fault, so
		 * asking unconditionally would turn the model keeping its promise into an error.
		 */
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

			/*
			 * Every widget that reads this, not the first: the readout's brick is drawn twice, as
			 * its entry row and as the heading, and both have to fit.
			 */
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

				/*
				 * Where the glyphs end, not where the slot ends: a text block in a filling slot is
				 * arranged at the whole column width however short its text, so the arranged rectangle
				 * says nothing about fit; the width it asked for does.
				 */
				const double LineRightPx = LineLeftPx + Text.DesiredSizePx.X;
				const double SlackPx = ContentRightPx - LineRightPx;

				if (SlackPx < TightestSlackPx)
				{
					TightestSlackPx = SlackPx;
					TightestLine = Line;
				}

				/*
				 * The assertion, both edges. Past the right edge the sentence runs off the panel and
				 * is cut; past the left is the same, what a bar wide enough to push its sentence out
				 * would produce if the row centred itself.
				 */
				Test.TestTrue(
					*FString::Printf(
						TEXT("readout line '%s' must fit inside the %s panel (%s): it needs x %.2f..%.2f against a content column of %.2f..%.2f, overrunning by %.2f px"),
						*Line, DetailName, WallDescription, LineLeftPx, LineRightPx,
						ContentLeftPx, ContentRightPx, LineRightPx - ContentRightPx),
					LineLeftPx >= ContentLeftPx - PanelSpanContainmentSlackPx
						&& LineRightPx <= ContentRightPx + PanelSpanContainmentSlackPx);
			}
		}

		/*
		 * And the number the next change needs: the spare pixels beside the longest readout line, the
		 * budget a wider bar comes out of. Reported not asserted, because how it is spent is a layout
		 * decision. Negative, it is how much wider the panel must be to fit its own sentence. In
		 * compact it is half the size derivation: a compact width chosen without reading it clips its
		 * own brick list.
		 */
		Test.AddInfo(FString::Printf(
			TEXT("%s, %s detail: the readout's tightest line is '%s', %s %.2f px inside a %.2f px content column"),
			WallDescription, DetailName, *TightestLine,
			TightestSlackPx < 0.0 ? TEXT("OVERRUNNING BY") : TEXT("with spare of"),
			FMath::Abs(TightestSlackPx), ContentRightPx - ContentLeftPx));

		/*
		 * And no two lines sharing a row may print on top of each other — the clipping the containment
		 * sweep cannot see, and the one a narrower panel actually hits. A brick row is a position label
		 * at the left and a support word in a fixed 150 px column pinned to the right; narrowing the
		 * panel walks the column towards the label while both stay inside it, so the span claims keep
		 * passing (the word's spare is a constant 71 px). What breaks first is one sentence running
		 * into another, which looks like neither. Same row is decided by vertical centre (within 8 px),
		 * not by walking the tree, and glyph spans not arranged ones — a filling slot arranges every
		 * line at the whole column width, so arranged spans would all "overlap".
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

				/* Whichever is to the left; the pair is unordered and the gap is not. */
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

		/*
		 * And the other number the compact width derives from. The containment budget is the room
		 * beside the longest line; this is the room between two lines sharing a row, which runs out
		 * first in compact. The smaller of the two is how much narrower the panel may be made.
		 */
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
	 * And the compact panel actually gave screen back, the player's complaint measured. Two
	 * arrangements of one panel over one wall with the same bricks, differing only in mode, so every
	 * difference is the mode's. Strict per axis: narrower alone leaves a ribbon down the side,
	 * shorter alone a band across, either a mode that dropped the table but kept its room. Height
	 * needed measuring: the readout is in a FillHeight slot, so fewer lines shrink its desired size
	 * but change the arranged panel by nothing — only the panel's own override shrinks it.
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

		/*
		 * And the column inside shrank with it, by the same amount. A panel that narrowed while its
		 * column did not has sentences hanging off its background; the chrome is equal in both modes,
		 * so the two differences are one number, and a mismatch means the readout is outside it.
		 */
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
 * The bug is a two-frame oscillation needing no mouse movement. Hovering an entry calls
 * SetInspectedPiece, which swaps a joint breakout (3-6 lines) into the readout; the panel is in a
 * vertically-centred box, so it grows about its centre and every entry above the readout moves up
 * by half the growth, tens of pixels against a ~20 px row. The cursor, unmoved, is now off the
 * row; Slate synthesises a cursor move each tick, so OnMouseLeave empties the readout, the panel
 * re-centres, and the brick strobes Inspected<->Selected at 60 Hz. Swapping the readout's content
 * rather than rebuilding the panel defends the wrong half: the loop needs the button moved, not
 * destroyed.
 *
 * The assertion is the entry row's arranged offset, not the panel's desired height: a top-aligning
 * fix leaves the height growing yet stops every entry moving, so pinning height would rule it out
 * for no visible reason. Nor the action rows, which sit below the readout and cannot feed back. The
 * property is the one the loop turns on: the row that changed the readout is still where it was.
 *
 * Needs a world (a wall to point at, a binding to read), never ticks one, needs no RHI: SlatePrepass
 * and ArrangeChildren are arithmetic, which is why extracting BuildPieceMenuPanel makes it reachable.
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
	 * Fixture: the layout actually happened. A tree that measured to nothing would put every button
	 * at the same place in both states and pass while asserting nothing.
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
	 * And the rows are at distinct heights. If every button stacked at one Y the comparison below
	 * would be free — the other way a zero-sized layout passes for nothing.
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
	 * The assertion. Every entry row is where it was, so the cursor that changed the readout is
	 * still on the row that caused it.
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
 * No row a player can click moves when the readout changes — entry rows and action rows.
 *
 * The sister test above is about an oscillation; this is about an irreversible commit. Action rows
 * cannot strobe, but walk the cursor down off the last entry toward Delete: OnUnhovered collapses
 * the readout next tick and Delete slides up 60 px, toward a cursor on its way down. A click in
 * that frame deletes a brick the player did not aim at, and releasing a brick is irreversible here.
 * The standing rule is that the commit door is never wider than the menu door; a button moving
 * between aim and click is that hazard in geometry.
 *
 * The property is over every presented row, taken from the model and matched by caption, not slot
 * index: where a row sits is what a fix may change, and a player finds a row by reading it. Both
 * halves come from the presenter (Inspector.Pieces and GetShownPieceMenuRows), so a drawn-but-
 * unplaced row is a missing button, not a skipped assertion.
 *
 * The panel's size is now pinned too. The rebuild makes it fixed with the brick list scrolling
 * inside, a strictly stronger statement: a panel that cannot resize cannot move anything. So the
 * precondition that the readout changed is now made on its own text — a joint line drawn in one
 * state and not the other — which the old height comparison stood in for. It also pins the
 * horizontal argument: the panel is centred, so a row's left edge moves ~59 px as the readout
 * widens, harmless only while the narrow span is contained in the wide one, which fails if a row
 * shrink-wraps. One comparison per row, so asserted not argued.
 *
 * Needs a world, never ticks one, needs no RHI — same fixture and two-state arrange as above.
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
	 * The clickable rows, asked of the presenter not read back off the panel: reading captions off
	 * the drawn buttons would make the list agree with whatever was drawn, so a missing row would
	 * take its own assertion with it.
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
	 * Every presented row is a drawn button and vice versa. An equality, not a floor: an extra
	 * unaccounted button is an unwatched clickable row, exactly the hazard.
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
	 * And the rows are at distinct heights. If every button stacked at one Y the comparison below
	 * would be free — how a zero-sized layout passes while asserting nothing.
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
	 * Fixture: the readout actually changed. A panel that drew the same thing in both states would
	 * hold every row still for free. Asserted on the readout's text, not the panel's height, because
	 * the height is now pinned still (see the header): a joint line appearing is what height stood in for.
	 */
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

	/*
	 * And the panel itself is the same size, the property the rebuild turns on. Every row assertion
	 * below follows from it; asserted separately because it is the mechanism, and a resizing panel
	 * can satisfy the rows today by a slot arrangement the next change invalidates. Both axes: a
	 * panel that widens with the longest joint line drags every row's left edge with it.
	 */
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

	/*
	 * The assertion. Every clickable row is exactly where it was, so a click aimed at one commits it.
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
		 * And sideways, where the claim is containment not stillness. The panel is centred, so a
		 * row's left edge moves as the readout widens, harmless while the narrower span lies inside
		 * the wider one. Either direction counts — the readout may grow or collapse.
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

/**
 * The panel prints every line the model supplies for the state it is in, and nothing from the
 * state it is not in.
 *
 * The test the screenshot would have failed. HeadroomFraction, HeadroomCaption and HeadroomScale
 * have been on the model, worded and swept, since the bar existed, and nothing ever consumed them —
 * the panel drew joint lines and stopped. A field with no reader is indistinguishable from a wrong
 * field, and every other test looked at the model, not the widget. So the claim is at the seam: the
 * strings the model decided are the strings the arranged tree contains.
 *
 * Presence, not appearance. Colour, font and anchoring are the screenshot's job and asserted
 * nowhere: a headless world paints no pixel. That the text exists in the tree is the difference
 * between a caption the model composed and one a player can read.
 *
 * The absent half is taken from the other state's model, not spelled here: the hint line shown with
 * no brick singled out must not stand beside a live breakout (the stale-field defect), and a joint
 * line must not survive its brick being let go. So this file pins no wording; Presenter.PieceMenuInspector
 * decides the words.
 *
 * The identity line (SESSION_UI_DESIGN §c, S7) joins the same sweep, with a position: a reader-less
 * field like the headroom scale, so it is drawn, absent with nothing singled out, and between the
 * brick's name and its joint summary, because "which brick, and what is it" is one thought.
 *
 * Needs a world, never ticks one, needs no RHI, like every test here.
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

	/* Bricks picked, none pointed at: the state the readout region has to fill with words. */
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

	/* And now one brick is pointed at, which is what a cursor on an entry row does. */
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

	/*
	 * The readout names its brick. It reads the same as that brick's entry row by construction, so
	 * this cannot distinguish the heading from the row and does not try: it says the model's
	 * InspectedLabel is a string the panel has, the whole seam claim.
	 */
	CheckPanelDrawsLine(
		*this, InspectedState, Inspected.InspectedLabel,
		TEXT("the name of the brick the readout is about"));

	/*
	 * And what that brick is — material, size, mass — directly under the name. SESSION_UI_DESIGN §c
	 * puts the identity line second because the two lines are one thought; a model that drew it
	 * elsewhere would put the weight below the joint table, where a reader has stopped looking. The
	 * fixture's RunningBond bricks carry no material, so the line reads "Unknown material · …" — fine,
	 * because this file owns only that the panel draws the model's string; Presenter.PieceIdentityText
	 * owns which string.
	 */
	TestFalse(
		TEXT("fixture: the singled-out brick must have an identity line for the panel to draw"),
		Inspected.IdentityText.IsEmpty());

	CheckPanelDrawsLine(
		*this, InspectedState, Inspected.IdentityText,
		TEXT("what the brick is made of, how big it is and what it weighs"));

	/*
	 * The lowest occurrence of the name, not the first. The heading reads the same as the brick's
	 * entry row, so the first match is the row up in the list; measuring against it would only say
	 * the identity line is below the brick list, which every readout line already is.
	 */
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

	/*
	 * And the headroom bar's words, which nothing has ever drawn. A log axis with no decades is
	 * unreadable — the same fill means 1000x on one panel and 3x on another — so the caption and
	 * ticks are not decoration, and the model has supplied them to nobody.
	 */
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

	/*
	 * And neither state carries the other's lines. A "point at a brick" hint beside a live breakout
	 * is the panel contradicting itself; a joint line left standing is another brick's numbers.
	 */
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
 * The panel reads top to bottom in the agreed order, and the row that destroys bricks is last.
 *
 * Why order is a test, not a taste. The rebuild puts the readout between the brick list and Delete,
 * the opposite of today — today the readout is last only because it was the one slot that could
 * resize. Once the panel is fixed-size, ordering is a legibility decision: read what you picked,
 * then what the brick is doing, then the button that removes it. Keeping the readout last would
 * pass every stillness assertion here and be the old panel in new clothes.
 *
 * Delete being last is the safety half. Releasing a brick is irreversible, and the commit door is
 * never wider than the menu door: a destructive button reached only by walking past everything that
 * describes what will be destroyed cannot be hit by overshooting.
 *
 * The header is one line, asserted as a line not two slots: the word and count belong on one row,
 * and a wrapped count spends a row on nothing. Vertical centres are compared, since two texts on a
 * row can differ in height.
 *
 * Needs a world, never ticks one, needs no RHI.
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

	/* The readout has to be full, or there is nothing for Delete to be underneath. */
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

	/*
	 * The presenter's last action is the one that must be last on screen, asked of the presenter
	 * not spelled "Delete": the action table is data, and a second action must not silently
	 * retarget this test.
	 */
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

	/* The heading, and the count beside it on the same row. */
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

	/*
	 * The order, row by row: heading over list, list over readout, readout over Delete. Each entry
	 * is checked against both ends, not just the next one, so a list in the wrong region cannot pass
	 * by being internally consistent.
	 */
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

	/*
	 * And every readout line sits between them. The support word, joint summary, joint lines and bar
	 * caption are what a player reads before deleting, so all are above the button that does it.
	 */
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

	/*
	 * And nothing clickable is below it at all, the claim at its strongest: the comparisons above
	 * are over the presenter's rows, this is over every button drawn, including an unaccounted one.
	 */
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
 * The cap, without a magic number. The design gives the brick list a fixed height and scrolls
 * inside it; the pixel count is the Slate's business. The test states the consequence: the panel is
 * the same with three bricks and with forty, so a long selection cannot push anything off screen or
 * move Delete. "The list is 300 px tall" would break on every tuning pass and not say that.
 *
 * Two assertions because they fail separately: the panel's measured size is the mechanism (a fixed
 * panel moves nothing), Delete's position is the outcome a cursor feels. A panel that grew but kept
 * Delete still is one rearrangement from not doing so.
 *
 * Not the shared 7-brick wall: seven entries fit anywhere. This builds ten courses of four (~45
 * pieces) and asserts the count before believing the measurement.
 *
 * Needs a world, never ticks one, needs no RHI.
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

	/*
	 * The short panel is built and measured before the long one exists. BuildPieceMenuPanel assigns
	 * the controller's readout box, so a second panel takes it over — measuring the first afterwards
	 * would be measuring a panel the controller has stopped feeding.
	 */
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

	/* Now pick the rest of the wall, which is the forty-brick selection the design names. */
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

	/*
	 * Fixture: the model really is listing them all. The cap is about what the panel draws; a
	 * presenter that dropped entries would satisfy every size claim by having nothing to fit in.
	 */
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

	/* The mechanism: one panel, one size, however many bricks in the list inside it. */
	TestTrue(
		*FString::Printf(
			TEXT("the panel must be the same size whatever is picked: %.2f x %.2f px with 3 bricks, %.2f x %.2f px with %d"),
			ShortState.DesiredSizePx.X, ShortState.DesiredSizePx.Y,
			LongState.DesiredSizePx.X, LongState.DesiredSizePx.Y, LongCount),
		FMath::IsNearlyEqual(ShortState.DesiredSizePx.X, LongState.DesiredSizePx.X, PanelRowMustNotMovePx)
			&& FMath::IsNearlyEqual(ShortState.DesiredSizePx.Y, LongState.DesiredSizePx.Y, PanelRowMustNotMovePx));

	/* And the outcome: the row that deletes bricks is exactly where it was. */
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

	/*
	 * And it is still on screen, which is what the player loses when a list runs away: a Delete
	 * button at y 1400 in a 1080-tall viewport is not reachable.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("'%s' must still be on screen with %d bricks picked: it is at y %.2f in a %.0f px viewport"),
			*LastActionLabel, LongCount, LongLastAction->TopLeftPx.Y, PanelViewportHeightPx),
		LongLastAction->TopLeftPx.Y + LongLastAction->SizePx.Y <= PanelViewportHeightPx);

	Fixture.End();

	return true;
}

/**
 * Every decade tick is wholly inside the bar it labels, and still marks its own place on it.
 *
 * What a capture showed: the tick row read `×` at the left and a broken `10(` at the right. Each
 * label is placed on a canvas the width of the bar, anchored at its Fraction and centred, so the
 * 0.0 and 1.0 labels are centred on the track's edges and half of each is clipped. A log axis with
 * unreadable decades is unreadable, so a clipped label is worse than none — a half-read number.
 *
 * The strip is found by walking up from the labels, not by naming a slot: the nearest widget with
 * all four ticks beneath it is the region they were placed in, whatever it is built from. Naming
 * the canvas or importing the width constant would make the test agree with the layout.
 *
 * The second half stops the obvious wrong fix. Spacing four labels evenly removes the clipping and
 * breaks the one property the scale exists for: that the joint whose margin is 10× lines its fill
 * up with the `10×` tick. Fill and ticks share one curve, so each label must cover the point
 * Fraction along the strip; a label at 1/8 while its fraction is 0 does not. Ordering rides along.
 *
 * Which is the red one: containment (the ticks hang off both ends today). Covering the fraction and
 * ordering both hold today, here as guard rails — see the accompanying report for the mutation.
 *
 * Needs a world (a wall to point at so the readout has joints), never ticks one, needs no RHI.
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

	/* There is no bar and no scale until a brick is singled out, which is what a hover does. */
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

	/*
	 * Fixture: the scale really reaches both ends. Ticks all in the middle would make containment
	 * free, and the clipped labels are exactly those at 0.0 and 1.0, so the ends are asserted.
	 */
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

	/* Where each tick's label was actually drawn, one widget per label or the claim says nothing. */
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

	/*
	 * The strip the ticks were placed in, the bar's track: the lowest widget with all four beneath
	 * it. See the header — deliberately not a named slot.
	 */
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

	/*
	 * Fixture: the strip is smaller than the panel. A common ancestor that walked to the root would
	 * make every claim below about the panel rather than the bar, and free.
	 */
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
	 * And the strip is the bar's column, not the whole readout — the other half of being readable,
	 * measured off the joint lines. A joint row is a bar then its sentence, so the leftmost sentence
	 * is the right edge of the column of bars without knowing a bar's width. A scale wider than that
	 * column has its 10× tick where no fill can reach, so every bar reads emptier than it is. An
	 * inequality, not an equality, because the bar-to-sentence gap is a spacing decision.
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

		/*
		 * The assertion this test is for. A label whose rectangle leaves the strip is half-read,
		 * which on a decade scale is a different number, not a smaller one.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("the '%s' tick must lie inside the bar it labels: it spans x %.2f..%.2f against a track of %.2f..%.2f, overhanging by %.2f px left and %.2f px right"),
				*Scale[Index].Label, LabelLeftPx, LabelRightPx, TrackLeftPx, TrackRightPx,
				TrackLeftPx - LabelLeftPx, LabelRightPx - TrackRightPx),
			LabelLeftPx >= TrackLeftPx - PanelSpanContainmentSlackPx
				&& LabelRightPx <= TrackRightPx + PanelSpanContainmentSlackPx);

		/*
		 * And it still marks the place it names. Fraction along the strip is where a joint with
		 * that margin fills to, so a label that no longer covers that point names a different
		 * decade — what evenly spacing them does.
		 */
		const double MarkedPx = TrackLeftPx + Scale[Index].Fraction * TrackWidthPx;

		TestTrue(
			*FString::Printf(
				TEXT("the '%s' tick must cover the point the bar fills to at %.4f of its width, x %.2f: the label spans %.2f..%.2f"),
				*Scale[Index].Label, Scale[Index].Fraction, MarkedPx, LabelLeftPx, LabelRightPx),
			LabelLeftPx <= MarkedPx + PanelSpanContainmentSlackPx
				&& LabelRightPx >= MarkedPx - PanelSpanContainmentSlackPx);

		/*
		 * And the labels read low to high across the bar. Cheap, and it catches a fix that shuffled
		 * an end past its neighbour.
		 */
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
			 * And they must not touch — a different claim from ordering and from lying inside the
			 * track, and the one the capture fails. "100×" ends at x 1416.67 and "1000×" begins at
			 * x 1417.00, a third of a pixel apart on a 96 px track, in order and wholly inside the
			 * bar. So every assertion above passes a row reading "…100×1000×", two numbers as one
			 * string on the axis meant to say which decade a fill means.
			 *
			 * It also bounds the fix. Narrowing the strip to the bar's column left the labels no
			 * room; widening the bar costs the joint sentences their width, which is why
			 * World.Menu.TheReadoutFitsInsideThePanel measures how much room they have.
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
 * Every line of the readout is drawn inside the panel, and the slack is reported.
 *
 * Why now. The decade ticks collide because their strip was narrowed to the column of bars, and the
 * obvious fix is to widen the bar — which nobody could safely do, because the panel is a fixed 560 px
 * and nothing measured what that costs the joint sentences. A joint line is a bar then a sentence at
 * its natural width in a box that will not shrink it, so a sentence with no room runs off the panel
 * and is clipped. "#3  course 2 · #4  bed above  49.0 kN  100.0" is the worst case: the joint about
 * to fail, and the reader cannot see it.
 *
 * The content width is measured off the action row, not imported: Delete is a full-width slot of the
 * readout's own box, so its span is the column everything must fit in, and a retuned width moves the
 * claim with it. It sweeps every line the model supplies, not just the long ones, because which is
 * longest depends on the wall and the units (999.9 N and 1.0 kN differ by a tenth of a newton).
 *
 * And it sweeps two walls, because one cannot see the panel's width. The flush bond bends nowhere —
 * every brick sits on two symmetric patches (moment zero, indeterminate) or squarely on the one
 * below — so its sweep reports a comfortable budget at 560 px or 680 px alike. The waist wall's
 * corbels land on one patch, half a bond offset off centre (determinate, exact moment), giving the
 * longest line this panel prints. Both run through one sweep so the claim stays one claim.
 *
 * The slack is logged, half the point: the narrowest gap between a line and the panel edge is how
 * many pixels a wider bar may take, printed pass or fail — as an overrun when negative, the amount a
 * too-narrow panel must be widened by.
 *
 * And it now sweeps both detail modes, where the player's second complaint lives. Compact drops the
 * joint table and headroom scale, but the panel is overridden to the same 640x560 px, so it gives
 * back no screen. Held together deliberately: the compact panel must cover strictly less viewport on
 * both axes, and every line it still shows must fit, since a compact panel that clips its brick list
 * is worse than one merely too big. The longest line changes with the mode — full's width came from
 * the longest joint line (617.5 px), which compact drops, leaving an entry row widest — so entry
 * rows are swept too and the compact budget logged the same way.
 *
 * Needs a world, never ticks one, needs no RHI.
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

	/*
	 * The flush wall first, the panel as five other tests measure it. It bends nowhere, so it
	 * establishes the budget the ordinary readout leaves — kept because it is the common case.
	 */
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

	/*
	 * And then the wall with a corbel, the one that can see the panel's width. A brick on a single
	 * off-centre patch is determinate, so its joint carries a real moment and its line grows the
	 * bending clause — the longest sentence, the only one against which "560 px" is a claim.
	 */
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
 * What a capture showed: three picked bricks draw three rows into a list that reserves a fixed
 * height, so ~145 px of nothing stands between the last brick and the readout — a fifth of the
 * panel, reading as a menu that failed to finish drawing. The height was fixed to stop a forty-brick
 * selection running the action rows off screen (World.Menu.PanelDoesNotGrowWithTheSelection); a cap
 * does that job too, and a short list then takes only the room it needs.
 *
 * The measurement is the dead space, not the list's height. "The list is 66 px tall" would pin an
 * implementation, break on a font change, and not say what a player sees. The gap between the last
 * brick row and the readout's first line is compared against one row's own height (measured, not
 * written here), so it holds at any font and panel size. A gap under a row is spacing; six rows is a hole.
 *
 * The panel's fixed size is not relaxed. The stillness tests and the cap still hold with a
 * natural-height-up-to-a-cap list: the readout absorbs the difference via FillHeight(1.0) and the
 * action rows lay from an unmoved bottom. The readout's first line does move with the count, left
 * unpinned deliberately: it carries nothing clickable, and the count changes only on a click in the
 * world with the cursor off the panel, so there is no feedback path. The brick rows do not move, the
 * list being the second slot of a top-down box.
 *
 * Needs a world, never ticks one, needs no RHI.
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
	 * Nothing singled out, the state the hole is worst in: the readout is one hint line, so the band
	 * of nothing is the only thing between the bricks and the rule. It is also the only state whose
	 * first readout line has its own text — once a brick is singled out that line reads the same as
	 * its entry row and could not be told apart.
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

	/*
	 * The lowest entry row, found by where it landed not which brick it names. The presenter's order
	 * is its business; the row nearest the readout is whichever the layout put there.
	 */
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

	/*
	 * Fixture: a row has a height, and the readout is below the list. A zero-height row would make
	 * the comparison unsatisfiable for no defect reason, and a readout above the list would make the
	 * gap negative and pass while the panel is upside down.
	 */
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

	/*
	 * The assertion. Whatever the list does with its space, what is between the bricks and the
	 * readout is a gap, not a hole.
	 */
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
 * Every SButton the panel draws — entry rows and action rows — reports SupportsKeyboardFocus()
 * false, so clicking one leaves the keyboard where it was.
 *
 * Why it is a defect, and why only since S6. SButton is focusable by default and Slate focuses it
 * on click; SButton::OnKeyDown then handles Enter and Space itself, so both stop reaching the game
 * while focus is held — Enter (Run structure) and Space (IA_Jump). A player who deletes a brick then
 * presses Space finds the pawn does not rise, with nothing in the log and no way back but clicking
 * the viewport. Before S6 the cursor existed only while a menu was up; with the permanent cursor the
 * menu is one surface among many and stolen focus outlives the click. BuildSessionToolbarPanel
 * already sets .IsFocusable(false) on every chip for this reason; this is the same hazard on the
 * older panel.
 *
 * Why the walk is over every button, and the count asserted. Buttons are built at two sites — one
 * per brick, one per action row — not the same code, so a fix to one leaves the other stealing the
 * keyboard; a sweep covers both, and a third site added later. And a sweep over an empty list passes,
 * so the count is asserted at two or more (the fixture's three bricks plus action rows exceed it).
 * Two, not tighter, because the exact count is the presenter's business and World.Menu.* owns it.
 *
 * Needs a ticking world (the controller is an actor, the selection a real ray at a real wall), never
 * ticks one, needs no RHI: focusability is read off the same arranged tree.
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

	/*
	 * A brick is singled out, the state a player is in when they click a row. It changes nothing
	 * about focusability, and draws the panel in its fullest configuration rather than its emptiest.
	 */
	Controller->SetInspectedPiece(Fixture.InspectedRef);

	const TSharedRef<SWidget> Panel = Controller->BuildPieceMenuPanel();

	const FPanelLayoutState State = MeasurePanel(Panel, PanelRootGeometry());

	AddInfo(FString::Printf(
		TEXT("the panel drew: %s"), *DescribePanelButtons(State.Buttons)));

	/* Anti-vacuity: there are buttons to make a claim about. */

	TestTrue(
		*FString::Printf(
			TEXT("fixture: the panel must draw at least 2 buttons — an entry row and an action row — "
				 "for a sweep over its buttons to mean anything; it drew %d [%s]"),
			State.Buttons.Num(), *DescribePanelButtons(State.Buttons)),
		State.Buttons.Num() >= 2);

	/* The claim: not one of them takes the keyboard. */

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
