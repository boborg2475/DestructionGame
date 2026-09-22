// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceActions.h"
#include "Core/PieceMenu.h"
#include "Core/PieceSelection.h"
#include "DestructionGamePlayerController.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Uniquely named namespace: unity builds merge translation units, so an anonymous one can
 * collide (CURRENT_STATE.md). The world harness lives in Tests/BrickWorldTestSupport.h.
 */
namespace PieceMultiSelectTestSupport
{
	using namespace DestructionLayout;

	/** Ray half-length along Y, cm. Well past the 10.25 cm wall on both sides, and along Y so nothing else is hit. */
	constexpr double MultiSelectReachCm = 100.0;

	/** Far from the wall and above the slab, so a ray straight down hits nothing. */
	const FVector MultiSelectEmptyAirCm(5000.0, 5000.0, 300.0);

	FVector MultiSelectRayStart(const FPieceBox& Box)
	{
		return FVector(Box.CentreCm.X, Box.CentreCm.Y - MultiSelectReachCm, Box.CentreCm.Z);
	}

	FVector MultiSelectRayEnd(const FPieceBox& Box)
	{
		return FVector(Box.CentreCm.X, Box.CentreCm.Y + MultiSelectReachCm, Box.CentreCm.Z);
	}

	FPieceRef MultiSelectRef(int32 StructureId, int32 PieceIndex)
	{
		FPieceRef Ref;
		Ref.StructureId = StructureId;
		Ref.PieceIndex = PieceIndex;

		return Ref;
	}

	bool MultiSelectSameRef(const FPieceRef& Left, const FPieceRef& Right)
	{
		return Left.StructureId == Right.StructureId && Left.PieceIndex == Right.PieceIndex;
	}

	const TCHAR* MultiSelectHighlightName(EBrickHighlight Highlight)
	{
		switch (Highlight)
		{
		case EBrickHighlight::Hovered:    return TEXT("Hovered");
		case EBrickHighlight::Selected:   return TEXT("Selected");
		case EBrickHighlight::Inspected:  return TEXT("Inspected");
		case EBrickHighlight::Neighbour0: return TEXT("Neighbour0");
		case EBrickHighlight::Neighbour1: return TEXT("Neighbour1");
		case EBrickHighlight::Neighbour2: return TEXT("Neighbour2");
		case EBrickHighlight::Neighbour3: return TEXT("Neighbour3");
		case EBrickHighlight::Neighbour4: return TEXT("Neighbour4");
		case EBrickHighlight::Neighbour5: return TEXT("Neighbour5");
		default:                          return TEXT("None");
		}
	}

	/**
	 * Neighbour colour slots, transcribed rather than imported so a shrunken palette fails. Six
	 * because a brick inside a running bond has six joints.
	 */
	constexpr int32 MultiSelectNeighbourSlotCount = 6;

	/**
	 * Highlight for the far end of joint row N. A switch rather than `Neighbour0 + Slot`, since
	 * EBrickHighlight does not promise its enumerator layout.
	 */
	EBrickHighlight MultiSelectNeighbourState(int32 Slot)
	{
		switch (Slot)
		{
		case 0: return EBrickHighlight::Neighbour0;
		case 1: return EBrickHighlight::Neighbour1;
		case 2: return EBrickHighlight::Neighbour2;
		case 3: return EBrickHighlight::Neighbour3;
		case 4: return EBrickHighlight::Neighbour4;
		case 5: return EBrickHighlight::Neighbour5;
		}

		return EBrickHighlight::None;
	}

	bool MultiSelectIsNeighbourState(EBrickHighlight Highlight)
	{
		for (int32 Slot = 0; Slot < MultiSelectNeighbourSlotCount; ++Slot)
		{
			if (MultiSelectNeighbourState(Slot) == Highlight)
			{
				return true;
			}
		}

		return false;
	}

	FString DescribeMultiSelectRefs(TArrayView<const FPieceRef> Refs)
	{
		if (Refs.Num() == 0)
		{
			return TEXT("<empty>");
		}

		FString Line;

		for (int32 Index = 0; Index < Refs.Num(); ++Index)
		{
			Line += FString::Printf(
				TEXT("%s{%d,%d}"),
				Index == 0 ? TEXT("") : TEXT(", "),
				Refs[Index].StructureId,
				Refs[Index].PieceIndex);
		}

		return Line;
	}

	FString DescribeMultiSelectHighlights(const TArray<ABrickActor*>& Bricks)
	{
		FString Line;

		for (int32 Piece = 0; Piece < Bricks.Num(); ++Piece)
		{
			Line += FString::Printf(
				TEXT("%s%d=%s"),
				Piece == 0 ? TEXT("") : TEXT(", "),
				Piece,
				IsValid(Bricks[Piece])
					? MultiSelectHighlightName(Bricks[Piece]->GetHighlight())
					: TEXT("<gone>"));
		}

		return Line;
	}

	/**
	 * Checks every brick's highlight. SkipPieces names deleted bricks explicitly, so a brick that
	 * vanished without being deleted still fails.
	 */
	void CheckMultiSelectHighlights(
		FAutomationTestBase& Test,
		const TArray<ABrickActor*>& Bricks,
		const TCHAR* Where,
		const TArray<EBrickHighlight>& Expected,
		const TArray<int32>& SkipPieces = TArray<int32>())
	{
		for (int32 Piece = 0; Piece < Bricks.Num() && Piece < Expected.Num(); ++Piece)
		{
			if (SkipPieces.Contains(Piece))
			{
				continue;
			}

			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: brick %d should read %s, it reads %s (all bricks: %s)"),
					Where, Piece,
					MultiSelectHighlightName(Expected[Piece]),
					IsValid(Bricks[Piece])
						? MultiSelectHighlightName(Bricks[Piece]->GetHighlight())
						: TEXT("<gone>"),
					*DescribeMultiSelectHighlights(Bricks)),
				IsValid(Bricks[Piece]) && Bricks[Piece]->GetHighlight() == Expected[Piece]);
		}
	}

	TArray<EBrickHighlight> MultiSelectNoHighlights()
	{
		TArray<EBrickHighlight> Expected;
		Expected.Init(EBrickHighlight::None, BrickWorldTestSupport::WallPieceCount);

		return Expected;
	}

	/**
	 * Joints touching a piece, counted off the graph: an independent oracle for the readout, which
	 * counts InspectPiece's list instead. Severed joints still count, so no HasGiven filter.
	 */
	int32 MultiSelectJointsTouching(const FStructure& Structure, int32 Handle)
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

	/** The joint-count sentence, written out rather than imported so the singular/plural branch is tested. */
	FString MultiSelectJointsSentence(int32 JointCount)
	{
		if (JointCount == 0)
		{
			return TEXT("No joints");
		}

		if (JointCount == 1)
		{
			return TEXT("1 joint");
		}

		return FString::Printf(TEXT("%d joints"), JointCount);
	}

	FString DescribeMultiSelectInspector(const FPieceMenuInspector& Inspector)
	{
		FString Line = FString::Printf(
			TEXT("count %d '%s', inspected %s {%d,%d}, support '%s', joints '%s' x%d, entries ["),
			Inspector.SelectedCount, *Inspector.CountText,
			Inspector.bHasInspectedPiece ? TEXT("yes") : TEXT("no"),
			Inspector.InspectedRef.StructureId, Inspector.InspectedRef.PieceIndex,
			*Inspector.SupportText, *Inspector.JointsText, Inspector.Joints.Num());

		for (int32 Index = 0; Index < Inspector.Pieces.Num(); ++Index)
		{
			Line += FString::Printf(
				TEXT("%s'%s'%s%s"),
				Index == 0 ? TEXT("") : TEXT(", "),
				*Inspector.Pieces[Index].Label,
				Inspector.Pieces[Index].bIsInspected ? TEXT(" (inspected)") : TEXT(""),
				Inspector.Pieces[Index].bIsLivePiece ? TEXT("") : TEXT(" (dead)"));
		}

		return Line + TEXT("]");
	}

	/** The readout's joint rows, so a neighbour failure names the panel it disagreed with. */
	FString DescribeMultiSelectJointRows(const FPieceMenuInspector& Inspector)
	{
		if (Inspector.Joints.Num() == 0)
		{
			return TEXT("<no joint rows>");
		}

		FString Line;

		for (int32 Index = 0; Index < Inspector.Joints.Num(); ++Index)
		{
			const FInspectorJointRow& Row = Inspector.Joints[Index];

			Line += FString::Printf(
				TEXT("%srow %d: conn %d -> piece %d, slot %d"),
				Index == 0 ? TEXT("") : TEXT("; "),
				Index, Row.ConnectionIndex, Row.OtherPieceIndex, Row.ColourSlot);
		}

		return Line;
	}

	/**
	 * Every brick a joint row names wears that row's colour slot, read off the panel's rows rather
	 * than re-derived, so a second derivation in the world fails. Slot assignment itself is pinned
	 * by DestructionGame.Presenter.PieceMenuJointColourSlots.
	 *
	 * Picked bricks are skipped (precedence is tested separately). Deleted bricks are skipped too:
	 * a severed joint still takes a slot, so the readout can name a brick no longer in the world.
	 */
	void CheckMultiSelectNeighbourSlots(
		FAutomationTestBase& Test,
		const TArray<ABrickActor*>& Bricks,
		const ADestructionGamePlayerController& Controller,
		const FPieceMenuInspector& Inspector,
		const TCHAR* Where)
	{
		const FPieceSelection& Selection = Controller.GetPieceSelection();

		int32 Checked = 0;

		for (int32 Index = 0; Index < Inspector.Joints.Num(); ++Index)
		{
			const FInspectorJointRow& Row = Inspector.Joints[Index];

			if (Row.ColourSlot == INDEX_NONE
				|| !Bricks.IsValidIndex(Row.OtherPieceIndex)
				|| !IsValid(Bricks[Row.OtherPieceIndex]))
			{
				continue;
			}

			const FPieceRef FarEnd =
				MultiSelectRef(Inspector.InspectedRef.StructureId, Row.OtherPieceIndex);

			if (Selection.Contains(FarEnd))
			{
				continue;
			}

			++Checked;

			const EBrickHighlight Wanted = MultiSelectNeighbourState(Row.ColourSlot);

			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: joint row %d names brick %d in colour slot %d, so that brick must read %s; it reads %s (all bricks: %s) (panel: %s)"),
					Where, Index, Row.OtherPieceIndex, Row.ColourSlot,
					MultiSelectHighlightName(Wanted),
					MultiSelectHighlightName(Bricks[Row.OtherPieceIndex]->GetHighlight()),
					*DescribeMultiSelectHighlights(Bricks),
					*DescribeMultiSelectJointRows(Inspector)),
				Bricks[Row.OtherPieceIndex]->GetHighlight() == Wanted);
		}

		// Floor on the sweep, so skipping everything cannot pass vacuously.
		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: at least one joint row must name a live unpicked brick for this to mean anything; %d did (panel: %s)"),
				Where, Checked, *DescribeMultiSelectJointRows(Inspector)),
			Checked > 0);
	}

	/** No brick wears a neighbour colour. Catches old neighbours left lit when the readout moves. */
	void CheckMultiSelectNoNeighboursLit(
		FAutomationTestBase& Test,
		const TArray<ABrickActor*>& Bricks,
		const TCHAR* Where,
		const TArray<int32>& SkipPieces = TArray<int32>())
	{
		for (int32 Piece = 0; Piece < Bricks.Num(); ++Piece)
		{
			if (SkipPieces.Contains(Piece) || !IsValid(Bricks[Piece]))
			{
				continue;
			}

			Test.TestFalse(
				*FString::Printf(
					TEXT("%s: brick %d must not be wearing a neighbour colour, it reads %s (all bricks: %s)"),
					Where, Piece, MultiSelectHighlightName(Bricks[Piece]->GetHighlight()),
					*DescribeMultiSelectHighlights(Bricks)),
				MultiSelectIsNeighbourState(Bricks[Piece]->GetHighlight()));
		}
	}

	/**
	 * Selection and menu checked together: the menu is up exactly when the selection is non-empty,
	 * and every row targets exactly the selection, in order. Catches a menu that drifts after build.
	 */
	void CheckMultiSelectMenu(
		FAutomationTestBase& Test,
		const ADestructionGamePlayerController& Controller,
		const TCHAR* Where,
		const TArray<FPieceRef>& ExpectedSelection)
	{
		const FPieceSelection& Selection = Controller.GetPieceSelection();

		Test.TestEqual(
			FString::Printf(TEXT("%s: %d brick(s) should be selected, the controller reports %d [%s]"),
				Where, ExpectedSelection.Num(), Selection.Num(),
				*DescribeMultiSelectRefs(Selection.Refs())),
			Selection.Num(), ExpectedSelection.Num());

		if (Selection.Refs().Num() == ExpectedSelection.Num())
		{
			for (int32 Index = 0; Index < ExpectedSelection.Num(); ++Index)
			{
				Test.TestTrue(
					*FString::Printf(
						TEXT("%s: selected brick %d should be {%d,%d}, the selection reads [%s]"),
						Where, Index,
						ExpectedSelection[Index].StructureId, ExpectedSelection[Index].PieceIndex,
						*DescribeMultiSelectRefs(Selection.Refs())),
					MultiSelectSameRef(Selection.Refs()[Index], ExpectedSelection[Index]));
			}
		}

		Test.TestEqual(
			FString::Printf(TEXT("%s: a menu should%s be up, IsPieceMenuShown reports %s"),
				Where, ExpectedSelection.Num() > 0 ? TEXT("") : TEXT(" NOT"),
				Controller.IsPieceMenuShown() ? TEXT("true") : TEXT("false")),
			Controller.IsPieceMenuShown(), ExpectedSelection.Num() > 0);

		const TArrayView<const FPieceMenuRow> Shown = Controller.GetShownPieceMenuRows();

		for (int32 RowIndex = 0; RowIndex < Shown.Num(); ++RowIndex)
		{
			const FPieceMenuRow& Row = Shown[RowIndex];

			// Counted off the row, since the row is what the widget draws and the commit runs against.
			Test.TestEqual(
				FString::Printf(
					TEXT("%s: row %d ('%s') should report %d selected brick(s), it carries %d [%s]"),
					Where, RowIndex, *Row.Label, ExpectedSelection.Num(), Row.Refs.Num(),
					*DescribeMultiSelectRefs(Row.Refs)),
				Row.Refs.Num(), ExpectedSelection.Num());

			if (Row.Refs.Num() != ExpectedSelection.Num() || ExpectedSelection.Num() == 0)
			{
				continue;
			}

			for (int32 Index = 0; Index < ExpectedSelection.Num(); ++Index)
			{
				Test.TestTrue(
					*FString::Printf(
						TEXT("%s: row %d target %d should be {%d,%d}, the row carries [%s]"),
						Where, RowIndex, Index,
						ExpectedSelection[Index].StructureId, ExpectedSelection[Index].PieceIndex,
						*DescribeMultiSelectRefs(Row.Refs)),
					MultiSelectSameRef(Row.Refs[Index], ExpectedSelection[Index]));
			}

			// The anchor (Ref) is derived from Refs and must agree with it.
			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: row %d's anchor should be the last selected brick {%d,%d}, it is {%d,%d}"),
					Where, RowIndex,
					ExpectedSelection.Last().StructureId, ExpectedSelection.Last().PieceIndex,
					Row.Ref.StructureId, Row.Ref.PieceIndex),
				MultiSelectSameRef(Row.Ref, ExpectedSelection.Last()));
		}
	}
}

/**
 * Hover highlights the brick under the cursor, clicking toggles it in and out of the selection,
 * and clicking empty space clears it. Highlight is asserted as a state; the material follows it
 * one line later and cannot be checked without a renderer.
 *
 * Precedence is Inspected > Selected > Neighbour > Hovered. Hover and Selected are separate
 * states so pointing at a brick never looks like choosing it. Inspected singles out the brick the
 * joint readout describes. Neighbour0-5 colour the far end of each joint row (steps 26+); slot
 * choice is the model's, and this checks the world reads it back and rebuilds rather than
 * accumulates the set.
 *
 * One long sequence in one world, because the bugs here are states left behind by the previous
 * step. Needs a world but never ticks it. The controller is spawned bare (no ULocalPlayer), so
 * hover and selection must work without an Enhanced Input subsystem.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMultiSelectTogglesAndHighlightsTest,
	"DestructionGame.World.Select.ClickingTogglesTheSelectionAndHoverHighlights",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMultiSelectTogglesAndHighlightsTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;
	using namespace PieceMultiSelectTestSupport;

	const FRunningBondSpec Spec = WallSpec();

	// Rays aim at a separately laid reference, so a spawner placing bricks wrongly cannot agree with itself.
	FBrickLayout Reference;

	TestTrue(TEXT("fixture: RunningBond should lay the reference wall"), RunningBond(Spec, Reference));

	if (Reference.Boxes.Num() != WallPieceCount)
	{
		AddError(FString::Printf(TEXT("fixture: the shared wall spec should be %d pieces, got %d"),
			WallPieceCount, Reference.Boxes.Num()));

		return true;
	}

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	const int32 StructureId = TestWorld.Subsystem->BuildRunningBond(Spec);
	FStructureBinding* const Binding = TestWorld.Subsystem->Find(StructureId);

	TestNotNull(
		*FString::Printf(TEXT("fixture: BuildRunningBond returned %d and Find should hand back its binding"),
			StructureId),
		Binding);

	if (Binding == nullptr || Binding->NumPieces() != WallPieceCount)
	{
		TestWorld.End();
		return true;
	}

	Binding->SolveLoads();

	TArray<ABrickActor*> Bricks;

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		ABrickActor* Brick = BrickAt(*this, *Binding, Piece);

		if (Brick == nullptr)
		{
			TestWorld.End();
			return true;
		}

		Bricks.Add(Brick);
	}

	ADestructionGamePlayerController* const Controller =
		TestWorld.World->SpawnActor<ADestructionGamePlayerController>();

	TestNotNull(TEXT("fixture: the test world should spawn the game's player controller"), Controller);

	if (Controller == nullptr)
	{
		TestWorld.End();
		return true;
	}

	// As built: nothing highlighted, no menu.
	CheckMultiSelectHighlights(*this, Bricks, TEXT("as built"), MultiSelectNoHighlights());
	CheckMultiSelectMenu(*this, *Controller, TEXT("as built"), TArray<FPieceRef>());

	// 1: hover highlights only the brick under the cursor.
	{
		const FPieceRef Hovered = Controller->HoverAlongRay(
			MultiSelectRayStart(Reference.Boxes[0]), MultiSelectRayEnd(Reference.Boxes[0]));

		TestTrue(
			FString::Printf(TEXT("hovering brick 0 should answer {%d,0}, it answered {%d,%d}"),
				StructureId, Hovered.StructureId, Hovered.PieceIndex),
			MultiSelectSameRef(Hovered, MultiSelectRef(StructureId, 0)));

		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[0] = EBrickHighlight::Hovered;

		CheckMultiSelectHighlights(*this, Bricks, TEXT("hovering brick 0"), Expected);

		// Hover selects nothing.
		CheckMultiSelectMenu(*this, *Controller, TEXT("hovering brick 0"), TArray<FPieceRef>());
	}

	// 2: moving the cursor un-highlights the previous brick.
	{
		Controller->HoverAlongRay(
			MultiSelectRayStart(Reference.Boxes[1]), MultiSelectRayEnd(Reference.Boxes[1]));

		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[1] = EBrickHighlight::Hovered;

		CheckMultiSelectHighlights(*this, Bricks, TEXT("hovering brick 1 after brick 0"), Expected);
	}

	// 3: hovering empty air clears the hover.
	{
		const FPieceRef Hovered = Controller->HoverAlongRay(
			MultiSelectEmptyAirCm, MultiSelectEmptyAirCm + FVector(0.0, 0.0, -100.0));

		TestTrue(
			FString::Printf(TEXT("hovering empty air should answer a default ref, it answered {%d,%d}"),
				Hovered.StructureId, Hovered.PieceIndex),
			MultiSelectSameRef(Hovered, FPieceRef()));

		CheckMultiSelectHighlights(*this, Bricks, TEXT("hovering empty air"), MultiSelectNoHighlights());
	}

	// 4: clicking a brick selects it.
	{
		const TArray<FPieceMenuRow> Rows = Controller->InspectAlongRay(
			MultiSelectRayStart(Reference.Boxes[1]), MultiSelectRayEnd(Reference.Boxes[1]));

		TestTrue(
			FString::Printf(TEXT("clicking a live brick should offer at least one row, it offered %d"),
				Rows.Num()),
			Rows.Num() > 0);

		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[1] = EBrickHighlight::Selected;

		CheckMultiSelectHighlights(*this, Bricks, TEXT("clicking brick 1"), Expected);

		CheckMultiSelectMenu(
			*this, *Controller, TEXT("clicking brick 1"), { MultiSelectRef(StructureId, 1) });
	}

	// 5: hovering a selected brick leaves it Selected.
	{
		Controller->HoverAlongRay(
			MultiSelectRayStart(Reference.Boxes[1]), MultiSelectRayEnd(Reference.Boxes[1]));

		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[1] = EBrickHighlight::Selected;

		CheckMultiSelectHighlights(*this, Bricks, TEXT("hovering the selected brick 1"), Expected);

		CheckMultiSelectMenu(
			*this, *Controller, TEXT("hovering the selected brick 1"), { MultiSelectRef(StructureId, 1) });
	}

	// 6: a selected brick and a hovered one co-exist.
	{
		Controller->HoverAlongRay(
			MultiSelectRayStart(Reference.Boxes[2]), MultiSelectRayEnd(Reference.Boxes[2]));

		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[1] = EBrickHighlight::Selected;
		Expected[2] = EBrickHighlight::Hovered;

		CheckMultiSelectHighlights(*this, Bricks, TEXT("hovering brick 2 with brick 1 selected"), Expected);
	}

	// 7: a second click adds to the selection; the menu targets both.
	{
		Controller->InspectAlongRay(
			MultiSelectRayStart(Reference.Boxes[2]), MultiSelectRayEnd(Reference.Boxes[2]));

		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[1] = EBrickHighlight::Selected;
		Expected[2] = EBrickHighlight::Selected;

		CheckMultiSelectHighlights(*this, Bricks, TEXT("clicking brick 2 as well"), Expected);

		CheckMultiSelectMenu(
			*this, *Controller, TEXT("clicking brick 2 as well"),
			{ MultiSelectRef(StructureId, 1), MultiSelectRef(StructureId, 2) });
	}

	/*
	 * 8: clicking a selected brick deselects it and leaves the other selected. Brick 1's
	 * resulting state is not pinned: the same ray points at it, so None or Hovered are both fine.
	 */
	{
		Controller->InspectAlongRay(
			MultiSelectRayStart(Reference.Boxes[1]), MultiSelectRayEnd(Reference.Boxes[1]));

		TestTrue(
			FString::Printf(
				TEXT("clicking the selected brick 1 again must leave it not-Selected, it reads %s (all bricks: %s)"),
				MultiSelectHighlightName(Bricks[1]->GetHighlight()),
				*DescribeMultiSelectHighlights(Bricks)),
			Bricks[1]->GetHighlight() != EBrickHighlight::Selected);

		TestTrue(
			FString::Printf(
				TEXT("deselecting brick 1 must leave brick 2 Selected, it reads %s (all bricks: %s)"),
				MultiSelectHighlightName(Bricks[2]->GetHighlight()),
				*DescribeMultiSelectHighlights(Bricks)),
			Bricks[2]->GetHighlight() == EBrickHighlight::Selected);

		CheckMultiSelectMenu(
			*this, *Controller, TEXT("clicking brick 1 a second time"),
			{ MultiSelectRef(StructureId, 2) });
	}

	// 9: clicking empty space clears the selection, the menu and every highlight.
	{
		const TArray<FPieceMenuRow> Rows = Controller->InspectAlongRay(
			MultiSelectEmptyAirCm, MultiSelectEmptyAirCm + FVector(0.0, 0.0, -100.0));

		TestEqual(
			FString::Printf(TEXT("clicking empty space must offer nothing, it offered %d row(s)"),
				Rows.Num()),
			Rows.Num(), 0);

		CheckMultiSelectHighlights(
			*this, Bricks, TEXT("clicking empty space"), MultiSelectNoHighlights());

		CheckMultiSelectMenu(*this, *Controller, TEXT("clicking empty space"), TArray<FPieceRef>());
	}

	// 10: the next click starts a fresh selection.
	{
		Controller->InspectAlongRay(
			MultiSelectRayStart(Reference.Boxes[3]), MultiSelectRayEnd(Reference.Boxes[3]));

		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[3] = EBrickHighlight::Selected;

		CheckMultiSelectHighlights(*this, Bricks, TEXT("clicking brick 3 after the clear"), Expected);

		CheckMultiSelectMenu(
			*this, *Controller, TEXT("clicking brick 3 after the clear"),
			{ MultiSelectRef(StructureId, 3) });
	}

	/*
	 * 11: deleting the hovered, selected brick leaves nothing touching a destroyed actor.
	 * HoveredPiece still names the tombstoned piece; RefreshPieceHighlight must resolve the ref
	 * before reaching for an actor. A regression net (green on arrival). Brick 3 is a top-course
	 * half bat, so the delete orphans nothing.
	 */
	{
		const bool bCommitted = Controller->ChoosePieceMenuRow(0);

		TestTrue(
			TEXT("choosing the only row of brick 3's menu should commit the delete"),
			bCommitted);

		TestFalse(
			TEXT("deleting brick 3 should have destroyed its actor"),
			IsValid(Bricks[3]));

		for (int32 Piece = 0; Piece < Bricks.Num(); ++Piece)
		{
			if (Piece == 3)
			{
				continue;
			}

			TestTrue(
				*FString::Printf(
					TEXT("deleting the hovered brick 3 must leave brick %d alone at None, it reads %s (all bricks: %s)"),
					Piece,
					IsValid(Bricks[Piece])
						? MultiSelectHighlightName(Bricks[Piece]->GetHighlight())
						: TEXT("<gone>"),
					*DescribeMultiSelectHighlights(Bricks)),
				IsValid(Bricks[Piece]) && Bricks[Piece]->GetHighlight() == EBrickHighlight::None);
		}

		CheckMultiSelectMenu(*this, *Controller, TEXT("after deleting brick 3"), TArray<FPieceRef>());
	}

	// 12: hover still works after the delete, releasing a ref that resolves to nothing.
	{
		const FPieceRef OverTheHole = Controller->HoverAlongRay(
			MultiSelectRayStart(Reference.Boxes[3]), MultiSelectRayEnd(Reference.Boxes[3]));

		TestTrue(
			FString::Printf(
				TEXT("hovering where brick 3 was should answer a default ref, it answered {%d,%d}"),
				OverTheHole.StructureId, OverTheHole.PieceIndex),
			MultiSelectSameRef(OverTheHole, FPieceRef()));

		const FPieceRef Hovered = Controller->HoverAlongRay(
			MultiSelectRayStart(Reference.Boxes[0]), MultiSelectRayEnd(Reference.Boxes[0]));

		TestTrue(
			FString::Printf(
				TEXT("hovering brick 0 after the delete should answer {%d,0}, it answered {%d,%d}"),
				StructureId, Hovered.StructureId, Hovered.PieceIndex),
			MultiSelectSameRef(Hovered, MultiSelectRef(StructureId, 0)));

		for (int32 Piece = 0; Piece < Bricks.Num(); ++Piece)
		{
			if (Piece == 3)
			{
				continue;
			}

			const EBrickHighlight Wanted =
				Piece == 0 ? EBrickHighlight::Hovered : EBrickHighlight::None;

			TestTrue(
				*FString::Printf(
					TEXT("after the delete, brick %d should read %s, it reads %s (all bricks: %s)"),
					Piece, MultiSelectHighlightName(Wanted),
					IsValid(Bricks[Piece])
						? MultiSelectHighlightName(Bricks[Piece]->GetHighlight())
						: TEXT("<gone>"),
					*DescribeMultiSelectHighlights(Bricks)),
				IsValid(Bricks[Piece]) && Bricks[Piece]->GetHighlight() == Wanted);
		}
	}

	// 13: pick three, nothing inspected. Brick 3 is gone from here on and is skipped explicitly.
	const TArray<int32> Deleted3 = { 3 };

	{
		for (int32 Piece = 0; Piece <= 2; ++Piece)
		{
			Controller->InspectAlongRay(
				MultiSelectRayStart(Reference.Boxes[Piece]), MultiSelectRayEnd(Reference.Boxes[Piece]));
		}

		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[0] = EBrickHighlight::Selected;
		Expected[1] = EBrickHighlight::Selected;
		Expected[2] = EBrickHighlight::Selected;

		CheckMultiSelectHighlights(*this, Bricks, TEXT("picking bricks 0, 1 and 2"), Expected, Deleted3);

		CheckMultiSelectMenu(
			*this, *Controller, TEXT("picking bricks 0, 1 and 2"),
			{ MultiSelectRef(StructureId, 0), MultiSelectRef(StructureId, 1),
			  MultiSelectRef(StructureId, 2) });
	}

	/*
	 * 14: inspecting one brick makes it Inspected; the others stay Selected, and the selection
	 * and menu are unchanged.
	 *
	 * Its neighbours light too. Brick 1's joints run to 0, 2, 4 and 5 (slots 0-3); 0 and 2 stay
	 * Selected, so 4 and 5 show slots 2 and 3. Steps 26+ sweep the neighbour rule.
	 */
	{
		Controller->SetInspectedPiece(MultiSelectRef(StructureId, 1));

		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[0] = EBrickHighlight::Selected;
		Expected[1] = EBrickHighlight::Inspected;
		Expected[2] = EBrickHighlight::Selected;
		Expected[4] = MultiSelectNeighbourState(2);
		Expected[5] = MultiSelectNeighbourState(3);

		CheckMultiSelectHighlights(*this, Bricks, TEXT("inspecting brick 1 of three"), Expected, Deleted3);

		CheckMultiSelectMenu(
			*this, *Controller, TEXT("inspecting brick 1 of three"),
			{ MultiSelectRef(StructureId, 0), MultiSelectRef(StructureId, 1),
			  MultiSelectRef(StructureId, 2) });
	}

	/*
	 * 15: Inspected beats Hovered, and hovering elsewhere does not disturb it. Brick 4 is a
	 * neighbour, so it keeps its neighbour colour under the cursor (Neighbour > Hovered; see step
	 * 28). The hover is masked, not cleared; step 16 proves it.
	 */
	{
		Controller->HoverAlongRay(
			MultiSelectRayStart(Reference.Boxes[1]), MultiSelectRayEnd(Reference.Boxes[1]));

		TArray<EBrickHighlight> OnTheInspected = MultiSelectNoHighlights();
		OnTheInspected[0] = EBrickHighlight::Selected;
		OnTheInspected[1] = EBrickHighlight::Inspected;
		OnTheInspected[2] = EBrickHighlight::Selected;
		OnTheInspected[4] = MultiSelectNeighbourState(2);
		OnTheInspected[5] = MultiSelectNeighbourState(3);

		CheckMultiSelectHighlights(
			*this, Bricks, TEXT("hovering the inspected brick 1"), OnTheInspected, Deleted3);

		Controller->HoverAlongRay(
			MultiSelectRayStart(Reference.Boxes[4]), MultiSelectRayEnd(Reference.Boxes[4]));

		TArray<EBrickHighlight> Elsewhere = MultiSelectNoHighlights();
		Elsewhere[0] = EBrickHighlight::Selected;
		Elsewhere[1] = EBrickHighlight::Inspected;
		Elsewhere[2] = EBrickHighlight::Selected;
		Elsewhere[4] = MultiSelectNeighbourState(2);
		Elsewhere[5] = MultiSelectNeighbourState(3);

		CheckMultiSelectHighlights(
			*this, Bricks, TEXT("hovering brick 4 while brick 1 is inspected"), Elsewhere, Deleted3);
	}

	/*
	 * 16: inspecting a different brick returns the previous one to Selected, not None. Both the
	 * old and new brick must be refreshed.
	 */
	{
		Controller->SetInspectedPiece(MultiSelectRef(StructureId, 2));

		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[0] = EBrickHighlight::Selected;
		Expected[1] = EBrickHighlight::Selected;
		Expected[2] = EBrickHighlight::Inspected;

		// Brick 4 is no longer a neighbour (brick 2's joints run to 1, 5, 6), so its masked hover returns.
		Expected[4] = EBrickHighlight::Hovered;
		Expected[5] = MultiSelectNeighbourState(1);
		Expected[6] = MultiSelectNeighbourState(2);

		CheckMultiSelectHighlights(
			*this, Bricks, TEXT("inspecting brick 2 after brick 1"), Expected, Deleted3);
	}

	// 17: inspecting nothing returns all three to Selected.
	{
		Controller->SetInspectedPiece(FPieceRef());

		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[0] = EBrickHighlight::Selected;
		Expected[1] = EBrickHighlight::Selected;
		Expected[2] = EBrickHighlight::Selected;
		Expected[4] = EBrickHighlight::Hovered;

		CheckMultiSelectHighlights(*this, Bricks, TEXT("inspecting nothing"), Expected, Deleted3);
	}

	/*
	 * 18: an unselected brick cannot be inspected, matching BuildPieceMenuInspector's rule so the
	 * world and panel agree. Brick 4 is hovered; the hover must not be promoted.
	 */
	{
		Controller->SetInspectedPiece(MultiSelectRef(StructureId, 4));

		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[0] = EBrickHighlight::Selected;
		Expected[1] = EBrickHighlight::Selected;
		Expected[2] = EBrickHighlight::Selected;
		Expected[4] = EBrickHighlight::Hovered;

		CheckMultiSelectHighlights(
			*this, Bricks, TEXT("inspecting a brick that was never picked"), Expected, Deleted3);
	}

	/*
	 * 19: deselecting the inspected brick leaves it neither Inspected nor Selected; the others stay
	 * Selected. None vs Hovered is not pinned, as in step 8.
	 */
	{
		Controller->SetInspectedPiece(MultiSelectRef(StructureId, 1));

		TArray<EBrickHighlight> WhileInspected = MultiSelectNoHighlights();
		WhileInspected[0] = EBrickHighlight::Selected;
		WhileInspected[1] = EBrickHighlight::Inspected;
		WhileInspected[2] = EBrickHighlight::Selected;

		// Brick 4 is hovered but is brick 1's neighbour again, so the neighbour colour wins; brick 5 is slot 3.
		WhileInspected[4] = MultiSelectNeighbourState(2);
		WhileInspected[5] = MultiSelectNeighbourState(3);

		CheckMultiSelectHighlights(
			*this, Bricks, TEXT("inspecting brick 1 again before deselecting it"),
			WhileInspected, Deleted3);

		Controller->InspectAlongRay(
			MultiSelectRayStart(Reference.Boxes[1]), MultiSelectRayEnd(Reference.Boxes[1]));

		TestTrue(
			*FString::Printf(
				TEXT("deselecting the INSPECTED brick 1 must leave it neither Inspected nor Selected, it reads %s (all bricks: %s)"),
				MultiSelectHighlightName(Bricks[1]->GetHighlight()),
				*DescribeMultiSelectHighlights(Bricks)),
			Bricks[1]->GetHighlight() != EBrickHighlight::Inspected
				&& Bricks[1]->GetHighlight() != EBrickHighlight::Selected);

		TestTrue(
			*FString::Printf(
				TEXT("deselecting the inspected brick 1 must leave brick 0 Selected, it reads %s (all bricks: %s)"),
				MultiSelectHighlightName(Bricks[0]->GetHighlight()),
				*DescribeMultiSelectHighlights(Bricks)),
			Bricks[0]->GetHighlight() == EBrickHighlight::Selected);

		TestTrue(
			*FString::Printf(
				TEXT("deselecting the inspected brick 1 must leave brick 2 Selected, it reads %s (all bricks: %s)"),
				MultiSelectHighlightName(Bricks[2]->GetHighlight()),
				*DescribeMultiSelectHighlights(Bricks)),
			Bricks[2]->GetHighlight() == EBrickHighlight::Selected);

		CheckMultiSelectMenu(
			*this, *Controller, TEXT("deselecting the inspected brick 1"),
			{ MultiSelectRef(StructureId, 0), MultiSelectRef(StructureId, 2) });
	}

	// 20: clearing the selection also clears the inspected highlight.
	{
		Controller->SetInspectedPiece(MultiSelectRef(StructureId, 2));

		Controller->InspectAlongRay(
			MultiSelectEmptyAirCm, MultiSelectEmptyAirCm + FVector(0.0, 0.0, -100.0));

		CheckMultiSelectHighlights(
			*this, Bricks, TEXT("clearing the selection while a brick is inspected"),
			MultiSelectNoHighlights(), Deleted3);

		CheckMultiSelectMenu(
			*this, *Controller, TEXT("clearing the selection while a brick is inspected"),
			TArray<FPieceRef>());
	}

	/*
	 * 21: deleting the inspected brick (step 11 for the third ref). Brick 5 is selected, inspected
	 * and hovered, so all three refs go stale; the next click must still work. It is a top-course
	 * brick, so the delete orphans nothing.
	 */
	const TArray<int32> Deleted3And5 = { 3, 5 };

	{
		Controller->InspectAlongRay(
			MultiSelectRayStart(Reference.Boxes[5]), MultiSelectRayEnd(Reference.Boxes[5]));

		Controller->SetInspectedPiece(MultiSelectRef(StructureId, 5));

		TArray<EBrickHighlight> WhileInspected = MultiSelectNoHighlights();
		WhileInspected[5] = EBrickHighlight::Inspected;

		// Brick 5's joints run to 4, 6, 1, 2 (slots 0-3), none picked.
		WhileInspected[4] = MultiSelectNeighbourState(0);
		WhileInspected[6] = MultiSelectNeighbourState(1);
		WhileInspected[1] = MultiSelectNeighbourState(2);
		WhileInspected[2] = MultiSelectNeighbourState(3);

		CheckMultiSelectHighlights(
			*this, Bricks, TEXT("inspecting the one brick picked"), WhileInspected, Deleted3);

		TestTrue(
			TEXT("choosing the only row of brick 5's menu should commit the delete"),
			Controller->ChoosePieceMenuRow(0));

		TestFalse(
			TEXT("deleting brick 5 should have destroyed its actor"),
			IsValid(Bricks[5]));

		CheckMultiSelectHighlights(
			*this, Bricks, TEXT("after deleting the inspected brick 5"),
			MultiSelectNoHighlights(), Deleted3And5);

		CheckMultiSelectMenu(
			*this, *Controller, TEXT("after deleting the inspected brick 5"), TArray<FPieceRef>());

		// The stale inspected ref must neither crash nor promote the next brick picked.
		Controller->InspectAlongRay(
			MultiSelectRayStart(Reference.Boxes[6]), MultiSelectRayEnd(Reference.Boxes[6]));

		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[6] = EBrickHighlight::Selected;

		CheckMultiSelectHighlights(
			*this, Bricks, TEXT("picking brick 6 after the inspected brick was deleted"),
			Expected, Deleted3And5);
	}

	/*
	 * 22+: the readout model via the controller (PieceMenuInspectorForSelection). Headless worlds
	 * have no viewport, so the widget path never called it; this covers the wiring between
	 * selection and BuildPieceMenuInspector. Step 23 catches passing FPieceRef() instead of
	 * InspectedPiece, which blanks the readout while the highlight still looks right. Bricks 3 and
	 * 5 are gone, so brick 1 carries severed joints.
	 */
	{
		// 22: nothing picked gives a default inspector; an empty selection never reaches the binding.
		Controller->InspectAlongRay(
			MultiSelectEmptyAirCm, MultiSelectEmptyAirCm + FVector(0.0, 0.0, -100.0));

		const FPieceMenuInspector Empty = Controller->PieceMenuInspectorForSelection();

		TestEqual(
			FString::Printf(TEXT("with nothing picked the readout should say so, it says '%s' [%s]"),
				*Empty.CountText, *DescribeMultiSelectInspector(Empty)),
			Empty.CountText, FString(TEXT("No bricks selected")));

		TestEqual(
			FString::Printf(TEXT("with nothing picked the readout should list no entries, it lists %d [%s]"),
				Empty.Pieces.Num(), *DescribeMultiSelectInspector(Empty)),
			Empty.Pieces.Num(), 0);

		TestFalse(
			*FString::Printf(TEXT("with nothing picked no brick can be singled out [%s]"),
				*DescribeMultiSelectInspector(Empty)),
			Empty.bHasInspectedPiece);
	}

	{
		/*
		 * 23: two picked, one inspected; the readout describes that one. The joint sentence is the
		 * key assertion, with the count swept off the graph rather than taken from the model.
		 */
		Controller->InspectAlongRay(
			MultiSelectRayStart(Reference.Boxes[0]), MultiSelectRayEnd(Reference.Boxes[0]));

		Controller->InspectAlongRay(
			MultiSelectRayStart(Reference.Boxes[1]), MultiSelectRayEnd(Reference.Boxes[1]));

		Controller->SetInspectedPiece(MultiSelectRef(StructureId, 1));

		const FPieceMenuInspector Two = Controller->PieceMenuInspectorForSelection();

		TestEqual(
			FString::Printf(TEXT("two bricks picked should list two entries, it lists %d [%s]"),
				Two.Pieces.Num(), *DescribeMultiSelectInspector(Two)),
			Two.Pieces.Num(), 2);

		TestTrue(
			*FString::Printf(TEXT("singling brick 1 out should be reported as such [%s]"),
				*DescribeMultiSelectInspector(Two)),
			Two.bHasInspectedPiece);

		TestTrue(
			*FString::Printf(
				TEXT("the readout should be about brick {%d,1}, it names {%d,%d} [%s]"),
				StructureId, Two.InspectedRef.StructureId, Two.InspectedRef.PieceIndex,
				*DescribeMultiSelectInspector(Two)),
			MultiSelectSameRef(Two.InspectedRef, MultiSelectRef(StructureId, 1)));

		const int32 JointsOnBrick1 = MultiSelectJointsTouching(
			Binding->GetStructure(), Binding->ResolvePiece(MultiSelectRef(StructureId, 1)));

		// With zero joints a blank readout would pass, so require some.
		TestTrue(
			FString::Printf(TEXT("fixture: brick 1 should still have joints to read out, the graph has %d"),
				JointsOnBrick1),
			JointsOnBrick1 > 0);

		TestEqual(
			FString::Printf(
				TEXT("the readout should break out brick 1's %d joint(s), it says '%s' [%s]"),
				JointsOnBrick1, *Two.JointsText, *DescribeMultiSelectInspector(Two)),
			Two.JointsText, MultiSelectJointsSentence(JointsOnBrick1));

		TestEqual(
			FString::Printf(
				TEXT("and it should carry that many joint rows, it carries %d [%s]"),
				Two.Joints.Num(), *DescribeMultiSelectInspector(Two)),
			Two.Joints.Num(), JointsOnBrick1);

		if (Two.Pieces.Num() == 2)
		{
			TestFalse(
				*FString::Printf(TEXT("entry 0 (brick 0) is not the one being read [%s]"),
					*DescribeMultiSelectInspector(Two)),
				Two.Pieces[0].bIsInspected);

			TestTrue(
				*FString::Printf(TEXT("entry 1 (brick 1) is the one being read [%s]"),
					*DescribeMultiSelectInspector(Two)),
				Two.Pieces[1].bIsInspected);
		}
	}

	{
		// 24: the panel will not read out an unpicked brick (step 18's rule, panel side).
		Controller->SetInspectedPiece(MultiSelectRef(StructureId, 2));

		const FPieceMenuInspector Outside = Controller->PieceMenuInspectorForSelection();

		TestFalse(
			*FString::Printf(TEXT("a brick that was never picked cannot be singled out [%s]"),
				*DescribeMultiSelectInspector(Outside)),
			Outside.bHasInspectedPiece);

		TestEqual(
			FString::Printf(
				TEXT("and the two picked bricks are still listed, it lists %d [%s]"),
				Outside.Pieces.Num(), *DescribeMultiSelectInspector(Outside)),
			Outside.Pieces.Num(), 2);

		TestEqual(
			FString::Printf(TEXT("with nothing singled out there is no joint sentence, it says '%s' [%s]"),
				*Outside.JointsText, *DescribeMultiSelectInspector(Outside)),
			Outside.JointsText, FString());
	}

	{
		/*
		 * 25: a brick deselected while inspected and then re-picked comes back merely Selected.
		 * Guards a stale InspectedPiece reviving on rejoin: Slate sends no OnMouseLeave to a
		 * removed widget, so the ref can outlive the panel. Checks both wall and panel.
		 */
		Controller->SetInspectedPiece(MultiSelectRef(StructureId, 0));

		TArray<EBrickHighlight> WhileInspected = MultiSelectNoHighlights();
		WhileInspected[0] = EBrickHighlight::Inspected;

		/*
		 * Brick 1 is picked and slot 0, so stays Selected. Slot 1 is the joint to deleted brick 3;
		 * brick 4 is slot 2.
		 */
		WhileInspected[1] = EBrickHighlight::Selected;
		WhileInspected[4] = MultiSelectNeighbourState(2);

		CheckMultiSelectHighlights(
			*this, Bricks, TEXT("inspecting brick 0 of the pair"), WhileInspected, Deleted3And5);

		// Deselect, then re-select.
		Controller->InspectAlongRay(
			MultiSelectRayStart(Reference.Boxes[0]), MultiSelectRayEnd(Reference.Boxes[0]));

		Controller->InspectAlongRay(
			MultiSelectRayStart(Reference.Boxes[0]), MultiSelectRayEnd(Reference.Boxes[0]));

		TestTrue(
			*FString::Printf(
				TEXT("re-picking brick 0 must leave it merely Selected, it reads %s (all bricks: %s)"),
				MultiSelectHighlightName(Bricks[0]->GetHighlight()),
				*DescribeMultiSelectHighlights(Bricks)),
			Bricks[0]->GetHighlight() == EBrickHighlight::Selected);

		const FPieceMenuInspector Rejoined = Controller->PieceMenuInspectorForSelection();

		TestFalse(
			*FString::Printf(
				TEXT("and the readout must not have re-opened on it by itself [%s]"),
				*DescribeMultiSelectInspector(Rejoined)),
			Rejoined.bHasInspectedPiece);
	}

	/*
	 * 26+: joint-row colours reach the bricks, so a row's swatch identifies its brick in the wall.
	 * Slots are read off the panel (CheckMultiSelectNeighbourSlots). Bricks 3 and 5 are gone, so
	 * some rows name deleted bricks, which must be resolved before touching an actor.
	 *
	 * Selected beats Neighbour so a player can always see which bricks Delete will remove.
	 * Neighbour beats Hovered because while the readout is open the cursor is on the panel, so
	 * the last world hover is stale.
	 */

	// 26: nothing inspected, no neighbour colours. Selection is bricks 1 and 0, cursor on brick 0.
	{
		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[0] = EBrickHighlight::Selected;
		Expected[1] = EBrickHighlight::Selected;

		CheckMultiSelectHighlights(
			*this, Bricks, TEXT("nothing singled out"), Expected, Deleted3And5);

		CheckMultiSelectNoNeighboursLit(
			*this, Bricks, TEXT("nothing singled out"), Deleted3And5);
	}

	/*
	 * 27: inspecting a brick lights its neighbours in the panel's slots. Brick 1's joints reach 0
	 * (picked), 2 and 4 (unpicked) and 5 (deleted): every case at once.
	 */
	{
		Controller->SetInspectedPiece(MultiSelectRef(StructureId, 1));

		const FPieceMenuInspector Inspector = Controller->PieceMenuInspectorForSelection();

		/*
		 * Hand-derived from the bond: course 0 is bricks 0-2, course 1 is half bat 3, bricks 4-5,
		 * half bat 6. Brick 1 has head joints to 0, 2 and bed joints to 4, 5: four rows, slots 0-3.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("fixture: brick 1 should be singled out with 4 joint rows, the panel says %s with %d (panel: %s)"),
				Inspector.bHasInspectedPiece ? TEXT("yes") : TEXT("no"), Inspector.Joints.Num(),
				*DescribeMultiSelectJointRows(Inspector)),
			Inspector.bHasInspectedPiece && Inspector.Joints.Num() == 4);

		// At least one row must name a deleted brick, or the resolve-before-actor case is untested.
		const bool bNamesADeadBrick = Inspector.Joints.ContainsByPredicate(
			[&Bricks](const FInspectorJointRow& Row)
			{
				return !Bricks.IsValidIndex(Row.OtherPieceIndex) || !IsValid(Bricks[Row.OtherPieceIndex]);
			});

		TestTrue(
			*FString::Printf(
				TEXT("fixture: at least one of brick 1's joint rows must name a brick that has been deleted (panel: %s)"),
				*DescribeMultiSelectJointRows(Inspector)),
			bNamesADeadBrick);

		CheckMultiSelectNeighbourSlots(
			*this, Bricks, *Controller, Inspector, TEXT("singling out brick 1"));

		// Whole-wall check, so a colour also landing on extra bricks fails.
		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[0] = EBrickHighlight::Selected;
		Expected[1] = EBrickHighlight::Inspected;
		Expected[2] = MultiSelectNeighbourState(1);
		Expected[4] = MultiSelectNeighbourState(2);
		Expected[6] = EBrickHighlight::None;

		CheckMultiSelectHighlights(
			*this, Bricks, TEXT("singling out brick 1"), Expected, Deleted3And5);
	}

	// 28: a hovered neighbour keeps its neighbour colour (the hover is stale while the panel is open).
	{
		Controller->HoverAlongRay(
			MultiSelectRayStart(Reference.Boxes[2]), MultiSelectRayEnd(Reference.Boxes[2]));

		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[0] = EBrickHighlight::Selected;
		Expected[1] = EBrickHighlight::Inspected;
		Expected[2] = MultiSelectNeighbourState(1);
		Expected[4] = MultiSelectNeighbourState(2);

		CheckMultiSelectHighlights(
			*this, Bricks, TEXT("hovering a neighbour of the inspected brick"), Expected, Deleted3And5);

		// Brick 0 is picked, a neighbour (slot 0) and was last hovered; Selected wins.
		TestTrue(
			*FString::Printf(
				TEXT("brick 0 is a picked brick AND a neighbour of the inspected brick; picked must win, it reads %s (all bricks: %s)"),
				MultiSelectHighlightName(Bricks[0]->GetHighlight()),
				*DescribeMultiSelectHighlights(Bricks)),
			Bricks[0]->GetHighlight() == EBrickHighlight::Selected);
	}

	/*
	 * 29: moving the readout moves the colours with no trail. Brick 2 stops being a neighbour and
	 * falls back to Hovered, where the cursor still is.
	 */
	{
		Controller->SetInspectedPiece(MultiSelectRef(StructureId, 0));

		const FPieceMenuInspector Inspector = Controller->PieceMenuInspectorForSelection();

		TestTrue(
			*FString::Printf(
				TEXT("fixture: brick 0 should be singled out with 3 joint rows, the panel says %s with %d (panel: %s)"),
				Inspector.bHasInspectedPiece ? TEXT("yes") : TEXT("no"), Inspector.Joints.Num(),
				*DescribeMultiSelectJointRows(Inspector)),
			Inspector.bHasInspectedPiece && Inspector.Joints.Num() == 3);

		CheckMultiSelectNeighbourSlots(
			*this, Bricks, *Controller, Inspector, TEXT("moving the readout to brick 0"));

		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[0] = EBrickHighlight::Inspected;
		Expected[1] = EBrickHighlight::Selected;
		Expected[2] = EBrickHighlight::Hovered;
		Expected[4] = MultiSelectNeighbourState(2);
		Expected[6] = EBrickHighlight::None;

		CheckMultiSelectHighlights(
			*this, Bricks, TEXT("moving the readout to brick 0"), Expected, Deleted3And5);
	}

	/*
	 * 30: a brick that is no longer anyone's neighbour goes dark. Brick 4 neighbours 0 and 1 but
	 * not 2, so inspecting brick 2 shows the set is rebuilt; brick 6 lights for the first time.
	 */
	{
		// Picking brick 2 dismisses the menu and clears the inspected brick, so set it again.
		Controller->InspectAlongRay(
			MultiSelectRayStart(Reference.Boxes[2]), MultiSelectRayEnd(Reference.Boxes[2]));

		Controller->SetInspectedPiece(MultiSelectRef(StructureId, 2));

		const FPieceMenuInspector Inspector = Controller->PieceMenuInspectorForSelection();

		TestTrue(
			*FString::Printf(
				TEXT("fixture: brick 2 should be singled out with 3 joint rows, the panel says %s with %d (panel: %s)"),
				Inspector.bHasInspectedPiece ? TEXT("yes") : TEXT("no"), Inspector.Joints.Num(),
				*DescribeMultiSelectJointRows(Inspector)),
			Inspector.bHasInspectedPiece && Inspector.Joints.Num() == 3);

		CheckMultiSelectNeighbourSlots(
			*this, Bricks, *Controller, Inspector, TEXT("moving the readout to brick 2"));

		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[0] = EBrickHighlight::Selected;
		Expected[1] = EBrickHighlight::Selected;
		Expected[2] = EBrickHighlight::Inspected;
		Expected[4] = EBrickHighlight::None;
		Expected[6] = MultiSelectNeighbourState(2);

		CheckMultiSelectHighlights(
			*this, Bricks, TEXT("moving the readout to brick 2"), Expected, Deleted3And5);
	}

	// 31: inspecting nothing clears every neighbour colour.
	{
		Controller->SetInspectedPiece(FPieceRef());

		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[0] = EBrickHighlight::Selected;
		Expected[1] = EBrickHighlight::Selected;
		Expected[2] = EBrickHighlight::Selected;

		CheckMultiSelectHighlights(
			*this, Bricks, TEXT("singling out nothing again"), Expected, Deleted3And5);

		CheckMultiSelectNoNeighboursLit(
			*this, Bricks, TEXT("singling out nothing again"), Deleted3And5);
	}

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
