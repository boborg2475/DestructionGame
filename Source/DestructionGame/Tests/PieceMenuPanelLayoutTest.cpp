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
	 * One drawn line of the panel: what it reads, and the rectangle it occupies.
	 *
	 * SEPARATE FROM FPanelButton BECAUSE MOST OF THE PANEL IS NOT CLICKABLE. The readout is all
	 * text — the support word, the joint lines, the headroom caption and its decade ticks — and
	 * every claim about the panel's ORDER is a claim about where those sit relative to the one
	 * row that deletes bricks. A collector that only saw buttons could say nothing about it,
	 * which is exactly why the readout's position was argued rather than measured until now.
	 */
	struct FPanelText
	{
		FString Text;
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

	/**
	 * Lay the tree out and record every line of text in it, with where it landed.
	 *
	 * THE SAME ARRANGE THE BUTTON COLLECTOR USES, for the same reason and with the same absence
	 * of any renderer. A text block that is arranged but never painted still has a position, and
	 * a position is the whole of what "the readout sits above Delete" means.
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
	 * THE SAME PANEL, OVER A WALL THAT ACTUALLY BENDS SOMEWHERE — AND A SIBLING OF FPanelFixture
	 * RATHER THAN A WIDENING OF IT.
	 *
	 * WHY THE SHARED ONE COULD NEVER SHOW THIS. FPanelFixture is a FLUSH running bond, and every
	 * joint in one is either a brick on two symmetric bed patches — statically indeterminate, so
	 * FStructure's moment rule leaves it at exactly zero — or a half bat sitting squarely on the
	 * one brick below it, whose patch centre lands exactly under its centre of mass. Neither can
	 * produce an eccentricity, so no line the fit sweep measures there ever carries the bending
	 * clause, and the sweep would report a bigger budget and pass identically at 560 px or 680 px.
	 * It cannot observe the panel's width either way.
	 *
	 * WHY A SIBLING AND NOT A WIDER SHARED FIXTURE. Five tests measure FPanelFixture — the two
	 * stillness claims, the no-growth cap, the Delete-is-last ordering and the tick containment —
	 * and every one of them is a number taken on that flush wall. Putting a bending joint into the
	 * shared fixture changes the readout's content, and so its width and its height, in all five
	 * at once: they would all move for a reason that has nothing to do with what any of them
	 * asserts. The cost of the sibling is one more world and one more panel build, inside one test.
	 *
	 * THE SHAPE IS THE PRODUCER'S OWN, NOT A HAND-WRITTEN JOINT. NarrowWaistWallSpec is a RAGGED
	 * running bond, so the courses above the waist step back OUT over it:
	 *
	 *      course 4            [ 5 ]
	 *      course 3         [ 3 ][ 4 ]      EACH ON ONE PATCH, AND OFF-CENTRE
	 *      course 2            [ 2 ]        THE WAIST — the brick singled out
	 *      course 1         [ 0 ][ 1 ]      grounded
	 *
	 * Brick 3 spans x -10.75..10.75 and the waist below it spans 0.5..22.0, so their bed patch
	 * runs 0.5..10.75 and is centred at x 5.625 — HALF THE BOND OFFSET from brick 3's own centre
	 * of mass at x 0. One support and a real lever arm is exactly the determinate case the moment
	 * rule answers, so that joint carries a bend and the line describing it grows the clause.
	 * Brick 4 is its mirror. The waist's two joints to the ground are unbent, so this fixture
	 * prints both kinds of line rather than only the interesting one.
	 *
	 * THE LEVER ARM IS ASSERTED FROM THE LAID BOXES rather than taken on trust, and the moment is
	 * then asked of the graph. A producer retuned so that the bond offset vanished would leave a
	 * wall that still stands, still draws, and quietly stops having anything to say about bending
	 * — at which point this fixture would be a second copy of the flush one and the sweep would go
	 * green over a panel that still cannot fit its own sentences.
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
		 * Half the coordinating grid's brick pitch, which is what a running bond offsets by.
		 *
		 * 21.5 cm of brick plus a 1 cm joint is a 22.5 cm cell, a bond offsets alternate courses
		 * by half of one, and the patch a stepped-out brick lands on is the half of its bed that
		 * overhangs — so its centre sits a quarter cell, 5.625 cm, from the brick's own middle.
		 * Spelled out here from the spec's own dimensions so that a wall laid to a different
		 * brick still states its own arm rather than inheriting this one.
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
			 * THE ECCENTRICITY, MEASURED OFF THE LAID BOXES AND NOT OFF THE SOLVER. This is the
			 * whole reason the wall bends: the corbel's bed patch is the overlap between it and
			 * the waist, and that overlap's middle is a quarter cell from the corbel's centre of
			 * mass. Asked of the geometry rather than of GetConnectionMoment so it says which of
			 * the two is wrong when they disagree.
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
			 * AND THE JOINT THE WHOLE FIXTURE EXISTS FOR IS CARRYING A BEND. The lever arm above
			 * says the geometry is eccentric; this says the solver answered it, which is the only
			 * state in which a line of the readout grows a clause at all.
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
	 * A WALL WITH MORE BRICKS IN IT THAN A LIST CAN SENSIBLY SHOW, which is the fixture the cap
	 * needs and the shared 7-brick WallSpec cannot be.
	 *
	 * TEN COURSES OF FOUR, FLUSH, so a flush course carries an extra half bat and the wall comes
	 * out around 45 pieces — the "40-brick selection" the panel is being rebuilt to survive. The
	 * count is ASSERTED rather than assumed below, because a layout change to RunningBond that
	 * quietly halved it would leave this test measuring a list short enough to fit and passing
	 * over the very thing it exists for.
	 *
	 * It stays well inside the floor the shared harness lays: four bricks reach X 78.25 against
	 * a slab that covers +/- 700, and ten courses stand 75 cm tall.
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
	 * ONE ARRANGED WIDGET OF ANY KIND, AND WHOEVER ARRANGED IT.
	 *
	 * THE PARENT LINK IS THE POINT, AND IT IS THERE SO A REGION CAN BE NAMED WITHOUT NAMING A
	 * SLOT. "The tick labels must lie inside the strip the bar is drawn in" needs that strip's
	 * arranged rectangle, and the strip is not a button and not a line of text — it is whatever
	 * widget the labels were placed on. Walking up from the labels to their nearest common
	 * ancestor finds it whatever it happens to be built from, so the claim survives a fix that
	 * wraps each label in a box to align it and still fails a fix that lifts them out of the
	 * bar's strip altogether.
	 */
	struct FPanelWidget
	{
		FString Type;

		/** What it reads, for an STextBlock. Empty for every other kind of widget. */
		FString Text;

		FVector2D TopLeftPx = FVector2D::ZeroVector;
		FVector2D SizePx = FVector2D::ZeroVector;

		/**
		 * How big it ASKED to be, which for a line of text is how much room the glyphs need.
		 *
		 * ARRANGED SIZE CANNOT ANSWER "IS THIS TEXT CUT OFF". A slot that fills hands its child
		 * the whole column whatever the child asked for, so a text block in one is arranged at
		 * the panel's width whether it holds four characters or four hundred — measuring that
		 * rectangle against the panel is a comparison of the panel with itself. The desired size
		 * is the measurement the renderer would have to honour to draw every glyph, and the gap
		 * between it and the space the line was given is exactly the room a wider bar could take.
		 *
		 * VALID BECAUSE THE PREPASS HAS RUN: SlatePrepass is what computes it, and MeasurePanel
		 * calls that before any of this walks the tree.
		 */
		FVector2D DesiredSizePx = FVector2D::ZeroVector;

		int32 ParentIndex = INDEX_NONE;
	};

	/**
	 * The same prepass-and-arrange walk the button and text collectors do, keeping everything.
	 *
	 * ARRANGED RATHER THAN PAINTED, for the reason the collectors above give: ArrangeChildren is
	 * the call the renderer makes to decide where a child goes, it is const, and it needs no
	 * device — so a whole tree's absolute geometry is reachable with no window and no RHI.
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
	 * THE PANEL PRINTS THIS LINE, AND THE MODEL HAD ONE TO PRINT.
	 *
	 * THE EMPTINESS CHECK IS THE HALF THAT MATTERS. An empty expected string would be looked for
	 * and found nowhere, or worse, matched against some other empty text block — either way the
	 * assertion would say nothing about a widget while appearing to. A string the model does not
	 * supply is a fault in the model, and it is reported as one rather than as a missing widget.
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
	 * How far apart two texts' vertical centres may be and still be "on one line".
	 *
	 * A ROW IS ABOUT 22 px TALL, so 8 px is comfortably under half a row: two texts this close
	 * cannot be in adjacent rows, and a header whose count was pushed onto its own line lands a
	 * whole row away. It is slack on a claim about ROWS rather than a measurement tolerance.
	 */
	constexpr double PanelSameLineSlackPx = 8.0;

	double PanelCentreYPx(const FPanelText& Text)
	{
		return Text.TopLeftPx.Y + Text.SizePx.Y * 0.5;
	}

	/**
	 * HOW MUCH CLEAR SPACE TWO ADJACENT DECADE LABELS NEED BETWEEN THEM.
	 *
	 * NOT A TOLERANCE — IT IS THE CLAIM. Two layouts of one tree in one geometry are
	 * bit-identical, so this is not slack on a measurement; it is the smallest gap at which
	 * "100×" and "1000×" read as two numbers rather than as "100×1000×". Four pixels is over
	 * half a digit at the scale's own font, which is the size of the space between words in the
	 * same run of text.
	 *
	 * THE MEASURED FAILURE IS 0.33 px, so the test is nowhere near its own boundary: the labels
	 * were placed on a track narrowed to the bar's own column without the labels being remeasured
	 * against it, and the two crowded ends of a decade scale are exactly the numbers a reader
	 * needs most. It is deliberately NOT expressed as a fraction of the track, because the thing
	 * that has to be readable is the glyphs, and glyphs do not scale with the bar.
	 */
	constexpr double PanelTickLabelGapPx = 4.0;

	/**
	 * SWEEP EVERY LINE THIS SELECTION'S READOUT SUPPLIES AND HOLD IT INSIDE THE PANEL'S COLUMN.
	 *
	 * A FUNCTION RATHER THAN A RunTest BODY BECAUSE THE CLAIM NOW HAS TWO WALLS TO MAKE IT ABOUT,
	 * and it is one claim. A flush running bond bends nowhere, so its readout never grows the
	 * bending clause and the sweep over it cannot see the panel's width at all; a wall with a
	 * corbel on it prints the longest line this panel can produce. Two copies of this body would
	 * be two chances for one of them to drift into measuring something else.
	 *
	 * @param WallDescription names which wall a failure came off, since both run in one test.
	 */
	void SweepReadoutFitsInsideThePanel(
		FAutomationTestBase& Test,
		ADestructionGamePlayerController* Controller,
		const FPieceRef& InspectedRef,
		const TCHAR* WallDescription)
	{
		const TSharedRef<SWidget> Panel = Controller->BuildPieceMenuPanel();
		const FGeometry Root = PanelRootGeometry();

		/* The readout is only full — and only as wide as it gets — with a brick singled out. */
		Controller->SetInspectedPiece(InspectedRef);

		const FPieceMenuInspector Inspector = Controller->PieceMenuInspectorForSelection();
		const FPanelLayoutState State = MeasurePanel(Panel, Root);

		const TArrayView<const FPieceMenuRow> Rows = Controller->GetShownPieceMenuRows();

		Test.TestTrue(
			FString::Printf(
				TEXT("fixture (%s): the selection should offer at least one action row to measure the panel by, the presenter shows %d"),
				WallDescription, Rows.Num()),
			Rows.Num() >= 1);

		Test.TestTrue(
			FString::Printf(
				TEXT("fixture (%s): the singled-out brick should break out joints for the readout to be wide, it broke out %d"),
				WallDescription, Inspector.Joints.Num()),
			Inspector.Joints.Num() >= 3);

		if (Rows.Num() == 0 || Inspector.Joints.Num() < 3)
		{
			return;
		}

		const FPanelButton* const ActionRow = FindPanelButton(State.Buttons, Rows.Last().Label);

		if (ActionRow == nullptr)
		{
			Test.AddError(FString::Printf(
				TEXT("the panel must draw the action row '%s' for its content width to be measurable; it drew %s"),
				*Rows.Last().Label, *DescribePanelButtons(State.Buttons)));

			return;
		}

		const double ContentLeftPx = ActionRow->TopLeftPx.X;
		const double ContentRightPx = ContentLeftPx + ActionRow->SizePx.X;

		/*
		 * FIXTURE: THE COLUMN IS A REAL ONE. A zero-width action row would make every containment
		 * claim below impossible for a reason that has nothing to do with the readout, and a row
		 * wider than the viewport would make them free.
		 */
		Test.TestTrue(
			*FString::Printf(
				TEXT("fixture (%s): the action row should span a real width, it spans x %.2f..%.2f in a %.0f px viewport"),
				WallDescription, ContentLeftPx, ContentRightPx, PanelViewportWidthPx),
			ActionRow->SizePx.X > 0.0 && ActionRow->SizePx.X < PanelViewportWidthPx);

		if (!(ActionRow->SizePx.X > 0.0))
		{
			return;
		}

		/*
		 * EVERY LINE THE MODEL SUPPLIED FOR THIS STATE, taken from the model rather than read back
		 * off the panel: a line the panel failed to draw is then a missing widget rather than an
		 * assertion that quietly skipped itself.
		 */
		TArray<FString> ReadoutLines;
		ReadoutLines.Add(Inspector.InspectedLabel);
		ReadoutLines.Add(Inspector.SupportText);
		ReadoutLines.Add(Inspector.JointsText);

		for (const FInspectorJointRow& Joint : Inspector.Joints)
		{
			ReadoutLines.Add(Joint.Text);
		}

		ReadoutLines.Add(Inspector.HeadroomCaption);

		for (const FHeadroomScaleTick& Tick : Inspector.HeadroomScale)
		{
			ReadoutLines.Add(Tick.Label);
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
			 * EVERY WIDGET THAT READS THIS, not the first one found: the brick a readout is about is
			 * drawn twice by construction — once as its entry row and once as the readout's own
			 * heading — and both of them have to fit.
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
				 * WHERE THE GLYPHS END, WHICH IS NOT WHERE THE SLOT ENDS. A text block in a filling
				 * slot is arranged at the whole column's width however short its text is, so its
				 * arranged rectangle says nothing at all about whether the words fit; the width it
				 * ASKED for does.
				 */
				const double LineRightPx = LineLeftPx + Text.DesiredSizePx.X;
				const double SlackPx = ContentRightPx - LineRightPx;

				if (SlackPx < TightestSlackPx)
				{
					TightestSlackPx = SlackPx;
					TightestLine = Line;
				}

				/*
				 * THE ASSERTION, BOTH EDGES. Past the right edge is the sentence running off the
				 * panel's own background and being cut; past the left is the same thing lying down,
				 * and it is what a bar wide enough to push its sentence out would produce if the row
				 * ever centred itself instead of overflowing.
				 */
				Test.TestTrue(
					*FString::Printf(
						TEXT("readout line '%s' must fit inside the panel (%s): it needs x %.2f..%.2f against a content column of %.2f..%.2f, overrunning by %.2f px"),
						*Line, WallDescription, LineLeftPx, LineRightPx, ContentLeftPx, ContentRightPx,
						LineRightPx - ContentRightPx),
					LineLeftPx >= ContentLeftPx - PanelSpanContainmentSlackPx
						&& LineRightPx <= ContentRightPx + PanelSpanContainmentSlackPx);
			}
		}

		/*
		 * AND THE NUMBER THE NEXT CHANGE NEEDS. This is how many pixels of the panel's width are
		 * spare beside the longest thing the readout prints, which is the budget a wider headroom bar
		 * has to come out of — reported rather than asserted, because how it is SPENT is a layout
		 * decision and this test has no business pinning one. When it is negative it is the other
		 * number the next change needs: how many pixels wider the panel has to be before the
		 * sentence it already prints fits inside it.
		 */
		Test.AddInfo(FString::Printf(
			TEXT("%s: the readout's tightest line is '%s', %s %.2f px inside a %.2f px content column"),
			WallDescription, *TightestLine,
			TightestSlackPx < 0.0 ? TEXT("OVERRUNNING BY") : TEXT("with spare of"),
			FMath::Abs(TightestSlackPx), ContentRightPx - ContentLeftPx));
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
 * AND THE PANEL'S OWN SIZE IS NOW PINNED TOO, WHICH IT DELIBERATELY WAS NOT BEFORE. The earlier
 * version allowed the panel to grow and asserted the growth as a fixture precondition, because
 * top-anchoring was a legal fix and top-anchoring leaves the height moving. The agreed rebuild
 * makes the panel a fixed size with the brick list scrolling inside it, and that is a strictly
 * stronger statement of the same property: a panel that cannot change size cannot move anything,
 * whatever it is anchored to and whatever order its slots are in. So the precondition that the
 * readout really did change is now made on the READOUT'S OWN TEXT rather than on the panel's
 * height — a joint line that is drawn in one state and not the other — which is the fact the
 * old height comparison was standing in for and cannot be satisfied by a panel that resizes.
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
	 * FIXTURE: THE READOUT ACTUALLY CHANGED. A panel that drew the same thing in both states
	 * would hold every row still for free, which is indistinguishable from the panel being
	 * right. It is asserted on the READOUT'S TEXT rather than on the panel's height because the
	 * height is now pinned STILL — see the header comment — and a joint line appearing where
	 * there was none is the change the height comparison was standing in for.
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
	 * AND THE PANEL ITSELF IS THE SAME SIZE, WHICH IS THE PROPERTY THE WHOLE REBUILD TURNS ON.
	 * Every row-by-row assertion below is a consequence of this one; it is asserted separately
	 * because it is the mechanism, and because a panel that resizes can satisfy the row
	 * assertions today by an arrangement of slots that the next change quietly invalidates.
	 * Both axes: a panel that widens with the longest joint line drags every row's left edge
	 * with it, which is the same hazard lying down.
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

/**
 * THE PANEL PRINTS EVERY LINE THE MODEL SUPPLIES FOR THE STATE IT IS IN, AND NOTHING FROM THE
 * STATE IT IS NOT IN.
 *
 * THIS IS THE TEST THE SCREENSHOT WOULD HAVE FAILED. HeadroomFraction, HeadroomCaption and
 * HeadroomScale have been on the model, worded, scaled and swept for finiteness by
 * Presenter.PieceMenuJointHeadroom for as long as the bar has existed, and NOTHING HAS EVER
 * CONSUMED THEM — the panel drew joint lines and stopped. A model field with no reader is
 * indistinguishable from a model field that is wrong, and every test that could see it was
 * looking at the model rather than at the widget. So the claim here is deliberately at the seam:
 * the strings the model decided are the strings the arranged widget tree contains.
 *
 * IT IS PRESENCE, NOT APPEARANCE. Colour, opacity, font and anchoring are what the screenshot is
 * for and are asserted nowhere: a headless world has no UGameViewportClient and nothing here
 * paints a pixel. What can be checked is that the text exists in the tree at all, which is
 * exactly the difference between a caption the model composed and a caption a player can read.
 *
 * THE ABSENT HALF IS THE OTHER HALF, AND IT IS TAKEN FROM THE OTHER STATE'S MODEL RATHER THAN
 * SPELLED HERE. The hint line that fills the readout region while no brick is singled out must
 * NOT be standing beside a live breakout — that is the stale-field defect the whole inspector is
 * shaped against — and a joint line must not survive the brick it describes being let go of.
 * Both strings come from whichever state supplies them, so this file pins no wording of its own
 * and Presenter.PieceMenuInspector remains the one place the words are decided.
 *
 * NEEDS A WORLD, NEVER TICKS ONE, AND NEEDS NO RHI, like every test in this file.
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
	 * THE READOUT NAMES THE BRICK IT IS ABOUT. It reads the same as that brick's entry row by
	 * construction — Presenter.PieceMenuInspector pins the two to each other — so this cannot
	 * distinguish the heading from the row above it, and does not try to. What it says is that
	 * the model's InspectedLabel is a string the panel has, which is the whole seam claim.
	 */
	CheckPanelDrawsLine(
		*this, InspectedState, Inspected.InspectedLabel,
		TEXT("the name of the brick the readout is about"));

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
	 * AND THE HEADROOM BAR'S WORDS, WHICH ARE THE ONES NOTHING HAS EVER DRAWN. A log axis with
	 * no decades on it is unreadable by construction — the same visible fill means 1000x on one
	 * panel and 3x on another — so the caption and the ticks are not decoration and the model
	 * has been supplying them to nobody.
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
	 * AND NEITHER STATE CARRIES THE OTHER'S LINES. A hint to point at a brick, drawn beside the
	 * breakout of the brick being pointed at, is the panel contradicting itself; a joint line
	 * left standing after the cursor leaves the list is somebody else's brick's numbers.
	 */
	CheckPanelDrawsNoLine(
		*this, InspectedState, Idle.InspectedHintText,
		TEXT("the hint line, now that a brick IS singled out"));

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
 * THE PANEL READS TOP TO BOTTOM IN THE AGREED ORDER, AND THE ROW THAT DESTROYS BRICKS IS LAST.
 *
 * WHY ORDER IS A TEST AND NOT A TASTE. The rebuild puts the readout BETWEEN the brick list and
 * Delete, which is the opposite of where it sits today — today's panel has the readout last
 * precisely because it was the only slot that could resize, and everything clickable had to be
 * above it. Once the panel is a fixed size that constraint is gone, and the ordering becomes a
 * legibility decision: a player reads what they picked, then what the singled-out brick is
 * doing, and only then meets the button that removes it. An implementation that quietly kept
 * the readout last would satisfy every stillness assertion in this file and would be the old
 * panel wearing the new one's clothes.
 *
 * AND DELETE BEING LAST IS THE SAFETY HALF OF IT. Releasing a brick is irreversible here, and
 * the standing rule is that the commit door is never wider than the menu door. A destructive
 * button reached by walking the cursor down PAST everything that describes what will be
 * destroyed is that rule expressed as geometry: nothing a player might click on the way to
 * reading the panel sits below it, so nothing can be committed by overshooting.
 *
 * THE HEADER IS ONE LINE, WHICH IS ASSERTED AS A LINE RATHER THAN AS TWO SLOTS. The word and
 * the count are two texts and belong on one row; a count that wrapped onto its own line is the
 * panel spending a row on nothing, and it is exactly the kind of thing that reads fine in code
 * and wrong on screen. Their vertical CENTRES are compared, because two texts on a row can be
 * different heights and still be on the row.
 *
 * NEEDS A WORLD, NEVER TICKS ONE, AND NEEDS NO RHI.
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
	 * THE LAST ACTION THE PRESENTER OFFERS IS THE ONE THAT MUST BE LAST ON SCREEN, asked of the
	 * presenter rather than spelled "Delete" here: the action table is data, and a second action
	 * landing beside Delete must not silently retarget this test onto whichever one it names.
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
	 * THE ORDER, ROW BY ROW: the heading over the list, the list over the readout, the readout
	 * over the button that deletes. Each entry row is checked against both ends rather than only
	 * against the next one, so a list that landed in the wrong region entirely cannot pass by
	 * being internally consistent.
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
	 * AND EVERY LINE OF THE READOUT SITS BETWEEN THEM. The support word, the joint summary, each
	 * joint line and the bar's caption are what a player is reading when they decide whether to
	 * delete, so all of them are above the button that does it.
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
	 * AND NOTHING CLICKABLE IS BELOW IT AT ALL, which is the claim in its strongest form: the
	 * per-row comparisons above are over the rows the presenter knows about, and this is over
	 * every button the panel actually drew, including one nothing accounted for.
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
 * PICKING FORTY BRICKS MUST NOT MAKE THE PANEL ANY BIGGER, AND MUST NOT MOVE DELETE.
 *
 * THIS IS THE CAP, STATED WITHOUT A MAGIC NUMBER. The agreed design gives the brick list a fixed
 * height and scrolls inside it, and the number of pixels that is belongs to whoever writes the
 * Slate. What a test can say is the CONSEQUENCE: the panel is the same panel with three bricks
 * picked and with forty, so a long selection cannot push anything off the bottom of the screen
 * and cannot move the row that deletes them. A test that asserted "the list is 300 px tall"
 * would break on every tuning pass and would still not say that.
 *
 * IT IS TWO ASSERTIONS BECAUSE THEY CAN FAIL SEPARATELY. The panel's own measured size is the
 * mechanism — a fixed panel cannot move anything — and Delete's arranged position is the
 * outcome a player's cursor experiences. A panel that grew but happened to keep Delete still
 * would be one slot rearrangement away from not doing so.
 *
 * WHY NOT THE SHARED 7-BRICK WALL. Seven entries fit anywhere, so it can distinguish nothing
 * about a list that has to scroll; this builds ten courses of four for about forty-five pieces
 * and asserts the count before believing the measurement.
 *
 * NEEDS A WORLD, NEVER TICKS ONE, AND NEEDS NO RHI. The world is there for a wall to point at
 * and forty bricks to pick out of; the panel is prepassed and arranged, which touches no
 * renderer.
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
	 * THE SHORT PANEL IS BUILT AND MEASURED BEFORE THE LONG ONE IS BUILT AT ALL. BuildPieceMenuPanel
	 * assigns the controller's readout box, so a second panel takes it over — measuring the first
	 * one afterwards would be measuring a panel the controller has stopped feeding.
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
	 * FIXTURE: THE MODEL REALLY IS LISTING THEM ALL. The cap is about what the panel DRAWS; a
	 * presenter that had quietly dropped entries would satisfy every size claim below by having
	 * nothing to fit in.
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

	/* THE MECHANISM: one panel, one size, however many bricks are in the list inside it. */
	TestTrue(
		*FString::Printf(
			TEXT("the panel must be the same size whatever is picked: %.2f x %.2f px with 3 bricks, %.2f x %.2f px with %d"),
			ShortState.DesiredSizePx.X, ShortState.DesiredSizePx.Y,
			LongState.DesiredSizePx.X, LongState.DesiredSizePx.Y, LongCount),
		FMath::IsNearlyEqual(ShortState.DesiredSizePx.X, LongState.DesiredSizePx.X, PanelRowMustNotMovePx)
			&& FMath::IsNearlyEqual(ShortState.DesiredSizePx.Y, LongState.DesiredSizePx.Y, PanelRowMustNotMovePx));

	/* AND THE OUTCOME: the row that deletes bricks is exactly where it was. */
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
	 * AND IT IS STILL ON SCREEN, which is the thing the player actually loses when a list runs
	 * away: a Delete button at y 1400 in a 1080-tall viewport is not reachable at all.
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
 * EVERY DECADE TICK IS WHOLLY INSIDE THE BAR IT LABELS, AND STILL MARKS ITS OWN PLACE ON IT.
 *
 * WHAT A CAPTURE SHOWED. The tick row reads `×` at the left end and a broken `10(` at the right:
 * each label is placed on a canvas the width of the bar, anchored at its own Fraction and aligned
 * on its own centre, so the labels for 0.0 and 1.0 are centred on the track's two EDGES and half
 * of each falls outside and is clipped away. A log axis with no readable decades on it is
 * unreadable by construction — the same visible fill means 1000× on one panel and 3× on another —
 * so a clipped label is worse than no label at all: it is a number the player half-reads.
 *
 * THE STRIP IS FOUND BY WALKING UP FROM THE LABELS RATHER THAN BY NAMING A SLOT. The nearest
 * widget that has all four ticks beneath it IS the region they were placed in, whatever it is
 * built out of, so a fix that wraps each label in a box to align it is still measured against the
 * same strip. Naming the canvas, or importing the width constant, would make the test agree with
 * the layout instead of checking it.
 *
 * AND THE SECOND HALF IS WHAT STOPS THE OBVIOUS WRONG FIX. Spacing four labels evenly across the
 * strip — four equal slots, each label centred in its own — removes the clipping and silently
 * breaks the single property the scale exists for: that the joint whose margin IS 10× lines its
 * fill up with the `10×` tick. The fill and the ticks are placed by the same curve today, on
 * purpose. So each label must COVER the point Fraction along the strip, which is exactly where
 * that decade's fill ends: centred labels cover it, an end label pushed inward to stop the
 * clipping still covers it, and a label sitting at 1/8 of the strip while its fraction is 0 does
 * not. The ordering claim rides along for the same money.
 *
 * WHICH OF THESE IS THE RED ONE. Containment is: the ticks hang off both ends today. Covering the
 * fraction and staying in order both hold today and are here as the guard rail the fix has to get
 * past — see the report accompanying this change for the mutation that shows they bite.
 *
 * NEEDS A WORLD, NEVER TICKS ONE, AND NEEDS NO RHI, like every test in this file. The world is
 * there for a wall to point at so the readout has joints to draw bars for; SlatePrepass and
 * ArrangeChildren are arithmetic.
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
	 * FIXTURE: THE SCALE REALLY DOES REACH BOTH ENDS. Every tick sitting somewhere in the middle
	 * would make containment free, and the two labels this defect clips are precisely the ones at
	 * 0.0 and 1.0 — so the ends are asserted rather than assumed.
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
	 * THE STRIP THE TICKS WERE PLACED IN, which is the bar's track: the lowest widget that has all
	 * four of them beneath it. See the header — this is deliberately not a named slot.
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
	 * FIXTURE: THE STRIP IS SOMETHING SMALLER THAN THE PANEL. A common ancestor that had walked all
	 * the way to the root would make every claim below a claim about the panel rather than about
	 * the bar, and it would be free.
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
	 * AND THE STRIP IS THE BAR'S COLUMN RATHER THAN THE WHOLE READOUT, WHICH IS THE OTHER HALF OF
	 * BEING READABLE AND IS MEASURED OFF THE JOINT LINES THEMSELVES.
	 *
	 * A joint row is a bar and then its sentence, so the bars end where the sentences begin, and
	 * the leftmost sentence is the right-hand edge of the column of bars without this test having
	 * to know how wide a bar is or how much padding sits between the two. A scale wider than that
	 * column is not a scale for those bars at all: its 10× tick stands where no bar's fill can ever
	 * reach, so every bar reads as far emptier than it is. It is stated as an inequality rather
	 * than an equality because the gap between a bar and its sentence is a spacing decision and
	 * this test has no business pinning it.
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
		 * THE ASSERTION THIS TEST IS FOR. A label whose rectangle leaves the strip is a label the
		 * player reads half of, which on a decade scale is a different number rather than a
		 * smaller one.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("the '%s' tick must lie inside the bar it labels: it spans x %.2f..%.2f against a track of %.2f..%.2f, overhanging by %.2f px left and %.2f px right"),
				*Scale[Index].Label, LabelLeftPx, LabelRightPx, TrackLeftPx, TrackRightPx,
				TrackLeftPx - LabelLeftPx, LabelRightPx - TrackRightPx),
			LabelLeftPx >= TrackLeftPx - PanelSpanContainmentSlackPx
				&& LabelRightPx <= TrackRightPx + PanelSpanContainmentSlackPx);

		/*
		 * AND IT STILL MARKS THE PLACE IT IS THE NAME OF. Fraction along the strip is exactly where
		 * a joint with that much margin fills the bar to, so a label that no longer covers that
		 * point is pointing at a different decade — which is what evenly spacing them does.
		 */
		const double MarkedPx = TrackLeftPx + Scale[Index].Fraction * TrackWidthPx;

		TestTrue(
			*FString::Printf(
				TEXT("the '%s' tick must cover the point the bar fills to at %.4f of its width, x %.2f: the label spans %.2f..%.2f"),
				*Scale[Index].Label, Scale[Index].Fraction, MarkedPx, LabelLeftPx, LabelRightPx),
			LabelLeftPx <= MarkedPx + PanelSpanContainmentSlackPx
				&& LabelRightPx >= MarkedPx - PanelSpanContainmentSlackPx);

		/*
		 * AND THE LABELS READ LOW TO HIGH ACROSS THE BAR. Cheap, and it is the claim that a fix
		 * which shuffles the ends inward has not shuffled one past its neighbour.
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
			 * AND THEY MUST NOT TOUCH, WHICH IS A DIFFERENT CLAIM FROM READING IN ORDER AND FROM
			 * LYING INSIDE THE TRACK — AND IS THE ONE THE CAPTURE FAILS.
			 *
			 * "100×" ends at x 1416.67 and "1000×" begins at x 1417.00: a third of a pixel apart,
			 * on a 96 px track, both of them in order and both of them wholly inside the bar. So
			 * every assertion above is satisfied by a row that reads "…100×1000×" — two numbers
			 * rendered as one string, on the axis whose entire purpose is to say which decade a
			 * fill means. A log bar with an unreadable scale is a log bar with no scale.
			 *
			 * IT IS ALSO WHAT BOUNDS THE FIX. The strip was narrowed to the bar's own column so
			 * that the ticks annotate the fills rather than empty panel — right, and it left the
			 * labels no room. Widening the bar is the obvious way out and it costs the joint
			 * sentences their width, which is why World.Menu.TheReadoutFitsInsideThePanel measures
			 * how much room those have: the two together say what a fix may spend.
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
 * EVERY LINE OF THE READOUT IS DRAWN INSIDE THE PANEL, AND THE SLACK IS REPORTED.
 *
 * WHY THIS EXISTS NOW, AND WHAT IT IS FOR. The decade ticks collide because their strip was
 * narrowed to the column of bars, and the obvious fix is to widen the bar — which nobody could
 * safely do, because the panel is a fixed 560 px and NOTHING MEASURED WHAT THAT COSTS the joint
 * sentences beside it. A joint line is a bar and then a sentence laid out at its own natural
 * width in a box that will not shrink it, so a sentence with no room does not wrap and does not
 * shrink: it runs out past the panel's own background and is cut off by whatever clips first.
 * "#3  course 2 · #4  bed above  49.0 kN  100.0" is the worst thing this panel could print,
 * because it is the joint that is about to fail and the reader cannot see that it is.
 *
 * THE PANEL'S CONTENT WIDTH IS MEASURED OFF THE ACTION ROW RATHER THAN IMPORTED. The row that
 * deletes bricks is a full-width slot of the same vertical box the readout is in, so its span IS
 * the column everything else has to fit in — no constant is read out of the controller, and a
 * panel retuned to a different width moves the claim with it rather than breaking it.
 *
 * IT SWEEPS EVERY LINE THE MODEL SUPPLIES, not just the long ones. Which line is longest depends
 * on the wall, the course numbers and the units a force happens to print in — 999.9 N and 1.0 kN
 * are different widths for a tenth of a newton — so naming one would be pinning today's fixture.
 *
 * AND IT SWEEPS TWO WALLS, BECAUSE ONE OF THEM CANNOT SEE THE PANEL'S WIDTH AT ALL. The flush
 * running bond every other test in this file measures bends NOWHERE: each of its bricks sits
 * either on two symmetric bed patches, which the moment rule leaves at exactly zero because the
 * statics is indeterminate, or squarely on the one brick below it. So no line of its readout ever
 * carries the bending clause, and the sweep over it reports a comfortable budget and would go on
 * reporting one at 560 px or at 680 px. The waist wall's corbels each land on ONE patch, half a
 * bond offset off their own centre of mass — the determinate case, where the moment is exact —
 * and the line describing such a joint is the longest thing this panel can print. Both walls run
 * through the same sweep so the claim stays one claim.
 *
 * AND THE SLACK IS LOGGED, WHICH IS HALF THE POINT OF THE TEST. The narrowest gap between a line
 * and the panel's edge is exactly how many pixels a wider bar may take, and it appears in the
 * run's output whether the test passes or fails — as an OVERRUN when it is negative, which is the
 * number a panel too narrow for its own sentence has to be widened by.
 *
 * NEEDS A WORLD, NEVER TICKS ONE, AND NEEDS NO RHI, like every test in this file.
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
	 * THE FLUSH WALL FIRST, WHICH IS THE PANEL AS FIVE OTHER TESTS IN THIS FILE MEASURE IT. It
	 * bends nowhere, so what it establishes is the budget the ORDINARY readout leaves — and it is
	 * kept rather than replaced precisely because it is the common case a player looks at.
	 */
	{
		FPanelFixture Fixture;

		if (Fixture.Begin(*this))
		{
			SweepReadoutFitsInsideThePanel(
				*this, Fixture.Controller, Fixture.InspectedRef,
				TEXT("a flush running bond, where nothing bends"));
		}

		Fixture.End();
	}

	/*
	 * AND THEN THE WALL WITH A CORBEL ON IT, WHICH IS THE ONE THAT CAN SEE THE PANEL'S WIDTH. A
	 * brick on a single off-centre patch is statically determinate, so its joint carries a real
	 * moment and its line grows the bending clause — the longest sentence this readout produces,
	 * and the only one against which "560 px" is a claim rather than an unexamined number.
	 */
	{
		FBendingPanelFixture Fixture;

		if (Fixture.Begin(*this))
		{
			SweepReadoutFitsInsideThePanel(
				*this, Fixture.Controller, Fixture.InspectedRef,
				TEXT("a ragged wall with a corbel, where one joint is levered open"));
		}

		Fixture.End();
	}

	return true;
}

/**
 * A SHORT SELECTION MUST NOT LEAVE A BAND OF EMPTY PANEL ABOVE THE READOUT.
 *
 * WHAT A CAPTURE SHOWED. Three picked bricks draw three rows into a list that reserves a fixed
 * height whatever it holds, so about 145 px of nothing stands between the last brick and the first
 * line of the readout — a fifth of the panel, reading as a menu that failed to finish drawing
 * rather than as a menu with room to spare. The height was made fixed to stop a forty-brick
 * selection running the action rows off the bottom of the screen, which is a real hazard and is
 * held by World.Menu.PanelDoesNotGrowWithTheSelection; a CAP does that job as well as a fixed
 * height does, and a short list then takes only the room it needs.
 *
 * THE MEASUREMENT IS THE DEAD SPACE, NOT THE LIST'S HEIGHT, AND THAT IS THE WHOLE CARE HERE. A
 * test that said "the list is 66 px tall with three bricks in it" would pin an implementation, be
 * wrong the moment the font changed, and still not say the thing a player sees. The gap between
 * the bottom of the last brick row and the top of the readout's first line is what they see, and
 * it is compared against ONE ROW'S OWN HEIGHT — measured from the row, not written down here — so
 * it stays true at any font and any panel size. A gap under a row is spacing; a gap of six rows
 * is a hole.
 *
 * THE PANEL'S FIXED SIZE IS NOT RELAXED BY THIS. The three stillness tests in this file and the
 * forty-brick cap all continue to hold with a natural-height-up-to-a-cap list, because the readout
 * takes the space the list does not — FillHeight(1.0) absorbs the difference — and the action rows
 * are laid from a panel bottom that has not moved. What DOES move as the selection count changes
 * is the readout's first line, and that is deliberately left unpinned: the readout carries nothing
 * clickable, so nothing can be committed by it moving, and the selection count only ever changes
 * on a click at a brick in the world with the cursor nowhere near the panel — there is no feedback
 * path and so no oscillation to catch. The brick rows themselves do not move at all, because the
 * list is the second slot from the top of a top-down box and its top is not a function of what is
 * below it.
 *
 * NEEDS A WORLD, NEVER TICKS ONE, AND NEEDS NO RHI.
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
	 * NOTHING SINGLED OUT, WHICH IS THE STATE THE HOLE IS WORST IN. With no brick pointed at the
	 * readout is one hint line, so the band of nothing above it is the only thing between the
	 * bricks and the rule. It is also the state whose first readout line has a text of its own:
	 * once a brick IS singled out the readout's first line reads the same as that brick's entry
	 * row, by construction, and could not be told apart from it by what it says.
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
	 * THE LOWEST ENTRY ROW, FOUND BY WHERE IT LANDED RATHER THAN BY WHICH BRICK IT NAMES. The
	 * order the presenter lists bricks in is its business; the row nearest the readout is whichever
	 * one the layout put there.
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
	 * FIXTURE: A ROW HAS A HEIGHT, AND THE READOUT IS BELOW THE LIST. A zero-height row would make
	 * the comparison below impossible to satisfy for a reason that has nothing to do with the
	 * defect, and a readout that had landed above the list would make the gap a negative number
	 * that passes while the panel is upside down.
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
	 * THE ASSERTION. Whatever the list is doing with the space it was given, what is between the
	 * bricks and the readout is a gap rather than a hole.
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

#endif // WITH_DEV_AUTOMATION_TESTS
