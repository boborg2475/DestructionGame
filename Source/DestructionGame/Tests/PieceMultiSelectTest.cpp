// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceActions.h"
#include "Core/PieceMenu.h"
#include "Core/PieceSelection.h"
#include "DestructionGamePlayerController.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges
 * many files into one. See CURRENT_STATE.md; the `using namespace` lives inside RunTest for the
 * same reason. The world harness is NOT redeclared here: it lives in
 * Tests/BrickWorldTestSupport.h and every World.* test shares it.
 */
namespace PieceMultiSelectTestSupport
{
	using namespace DestructionLayout;

	/**
	 * How far along Y a ray starts and ends, either side of the wall.
	 *
	 * A brick is 10.25 cm deep and a wall is centred on Y = 0, so +/- 100 cm is far outside it
	 * on both sides and the ray crosses the whole thickness. Along Y rather than X or Z so
	 * nothing else in the wall is in the way and the answer is unambiguous.
	 */
	constexpr double MultiSelectReachCm = 100.0;

	/** Far from the wall and above the slab, so a ray straight down hits absolutely nothing. */
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
		case EBrickHighlight::Hovered:  return TEXT("Hovered");
		case EBrickHighlight::Selected: return TEXT("Selected");
		default:                        return TEXT("None");
		}
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

	/** Every brick's highlight at once, so a trail left behind reads as one message. */
	void CheckMultiSelectHighlights(
		FAutomationTestBase& Test,
		const TArray<ABrickActor*>& Bricks,
		const TCHAR* Where,
		const TArray<EBrickHighlight>& Expected)
	{
		for (int32 Piece = 0; Piece < Bricks.Num() && Piece < Expected.Num(); ++Piece)
		{
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
	 * THE SELECTION AND THE MENU ARE ONE FACT, ASSERTED TOGETHER.
	 *
	 * A menu is up exactly when the selection is non-empty, its rows commit against exactly the
	 * selection, in order, and the count it reports is the count of the set. Checking these in
	 * one place is what makes "the presenter kept its own idea of the targets" impossible to
	 * pass: a menu that agreed with the selection when it was built and drifted afterwards fails
	 * the very next time this is called.
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

		/*
		 * THE MENU IS UP EXACTLY WHEN SOMETHING IS SELECTED. Both directions matter: a menu left
		 * up over an empty selection is a Delete button with no target, and a selection with no
		 * menu is a player who has picked six bricks and cannot do anything with them.
		 */
		Test.TestEqual(
			FString::Printf(TEXT("%s: a menu should%s be up, IsPieceMenuShown reports %s"),
				Where, ExpectedSelection.Num() > 0 ? TEXT("") : TEXT(" NOT"),
				Controller.IsPieceMenuShown() ? TEXT("true") : TEXT("false")),
			Controller.IsPieceMenuShown(), ExpectedSelection.Num() > 0);

		const TArrayView<const FPieceMenuRow> Shown = Controller.GetShownPieceMenuRows();

		for (int32 RowIndex = 0; RowIndex < Shown.Num(); ++RowIndex)
		{
			const FPieceMenuRow& Row = Shown[RowIndex];

			/*
			 * THE COUNT THE MENU REPORTS. It is read off the row rather than off the selection,
			 * because the row is what the widget draws from and what the commit runs against —
			 * a header that counted the selection while the rows carried something else would
			 * put an honest number over a dishonest button.
			 */
			Test.TestEqual(
				FString::Printf(
					TEXT("%s: row %d ('%s') should report %d selected brick(s), it carries %d [%s]"),
					Where, RowIndex, *Row.Label, ExpectedSelection.Num(), Row.Refs.Num(),
					*DescribeMultiSelectRefs(Row.Refs)),
				Row.Refs.Num(), ExpectedSelection.Num());

			/* Nothing below can be said about a row that came back the wrong size, or empty. */
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

			/*
			 * AND THE ANCHOR IS ONE OF THEM. FPieceMenuRow::Ref is derived from Refs the way
			 * Label is derived from the action's own text; a derived field that can disagree
			 * with what it came from is worse than no field, because it is what a per-brick
			 * readout will hang off.
			 */
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
 * HOVERING CALLS OUT THE BRICK UNDER THE CURSOR, CLICKING TOGGLES IT IN AND OUT OF A SELECTION
 * THAT STAYS CALLED OUT DISTINCTLY, AND CLICKING EMPTY SPACE CLEARS THE LOT.
 *
 * WHAT IS TESTED HERE AND WHAT DELIBERATELY IS NOT. That a brick LOOKS different is the same
 * untestable inch as the menu widget — a code-built world has no viewport and no renderer to
 * ask. So the highlight is a STATE, asserted here, and the material change is exactly one line
 * behind it: read the flag, set a material. Everything that can be wrong in a way a player would
 * notice — the wrong brick called out, a hover trail left behind, a selected brick downgraded to
 * merely-pointed-at, a selection that never empties — is a decision about which state, and every
 * one of those is here.
 *
 * HOVER AND SELECTED MUST BE DIFFERENT STATES, NOT ONE BOOLEAN, and that is what the enum is
 * for. They render differently and they mean different things: hover is "this is what you would
 * hit" and selected is "this is what the menu is about". Collapse them and the moment a player
 * points at a brick it looks chosen — and the one thing they must be able to check before
 * pressing Delete is which bricks are actually going.
 *
 * THE STRONGER STATE WINS WHEN THEY COINCIDE. A selected brick under the cursor stays Selected;
 * a hover that overwrote it would make a chosen brick flicker back to unchosen as the mouse
 * passed over it, which reads as the selection having been lost.
 *
 * A LONG SEQUENCE IN ONE WORLD, NOT A TEST PER CLAIM. A world costs tens of milliseconds and
 * this needs one configuration, so the file groups by world rather than by assertion — and the
 * sequence is where the value is anyway: every bug in this area is a state left behind by the
 * PREVIOUS step, which no single-step test can see.
 *
 * NEEDS A TICKING WORLD: it needs a WORLD — bricks to trace against and a subsystem to hold
 * them — but it never ticks one. Nothing here is about anything falling. The controller is
 * spawned BARE, with no ULocalPlayer, so hovering and selecting must work without an Enhanced
 * Input subsystem to take a context off.
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

	/*
	 * THE REFERENCE LAYOUT IS LAID SEPARATELY, so the points pointed at come from the producer
	 * rather than from whatever the subsystem happened to spawn. A spawner that put every brick
	 * at the origin would otherwise be pointed at the origin and agree with itself.
	 */
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

	/* A wall nobody has pointed at is a wall with nothing called out and no menu. */
	CheckMultiSelectHighlights(*this, Bricks, TEXT("as built"), MultiSelectNoHighlights());
	CheckMultiSelectMenu(*this, *Controller, TEXT("as built"), TArray<FPieceRef>());

	/*
	 * ONE: HOVERING CALLS OUT THE BRICK UNDER THE CURSOR AND NOTHING ELSE.
	 */
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

		/* AND HOVERING PICKS NOTHING. Pointing at a brick is not choosing it. */
		CheckMultiSelectMenu(*this, *Controller, TEXT("hovering brick 0"), TArray<FPieceRef>());
	}

	/*
	 * TWO: MOVING THE CURSOR LETS THE PREVIOUS BRICK GO. Without this every brick the cursor has
	 * ever crossed stays lit and the wall ends up entirely highlighted.
	 */
	{
		Controller->HoverAlongRay(
			MultiSelectRayStart(Reference.Boxes[1]), MultiSelectRayEnd(Reference.Boxes[1]));

		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[1] = EBrickHighlight::Hovered;

		CheckMultiSelectHighlights(*this, Bricks, TEXT("hovering brick 1 after brick 0"), Expected);
	}

	/* THREE: AND HOVERING NOTHING LETS GO OF EVERYTHING. */
	{
		const FPieceRef Hovered = Controller->HoverAlongRay(
			MultiSelectEmptyAirCm, MultiSelectEmptyAirCm + FVector(0.0, 0.0, -100.0));

		TestTrue(
			FString::Printf(TEXT("hovering empty air should answer a default ref, it answered {%d,%d}"),
				Hovered.StructureId, Hovered.PieceIndex),
			MultiSelectSameRef(Hovered, FPieceRef()));

		CheckMultiSelectHighlights(*this, Bricks, TEXT("hovering empty air"), MultiSelectNoHighlights());
	}

	/*
	 * FOUR: CLICKING A BRICK SELECTS IT, AND IT STAYS CALLED OUT — DISTINCTLY.
	 */
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

	/*
	 * FIVE: HOVERING A SELECTED BRICK LEAVES IT SELECTED. The stronger state wins, or a chosen
	 * brick reads as merely-pointed-at whenever the cursor is on it — which is exactly when the
	 * player is looking at it.
	 */
	{
		Controller->HoverAlongRay(
			MultiSelectRayStart(Reference.Boxes[1]), MultiSelectRayEnd(Reference.Boxes[1]));

		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[1] = EBrickHighlight::Selected;

		CheckMultiSelectHighlights(*this, Bricks, TEXT("hovering the selected brick 1"), Expected);

		/* AND HOVERING CHANGES NOTHING ELSE: no pick, no menu rebuild, no dismissal. */
		CheckMultiSelectMenu(
			*this, *Controller, TEXT("hovering the selected brick 1"), { MultiSelectRef(StructureId, 1) });
	}

	/* SIX: A SELECTED BRICK AND A HOVERED ONE CO-EXIST, WHICH IS WHAT THE TWO STATES ARE FOR. */
	{
		Controller->HoverAlongRay(
			MultiSelectRayStart(Reference.Boxes[2]), MultiSelectRayEnd(Reference.Boxes[2]));

		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[1] = EBrickHighlight::Selected;
		Expected[2] = EBrickHighlight::Hovered;

		CheckMultiSelectHighlights(*this, Bricks, TEXT("hovering brick 2 with brick 1 selected"), Expected);
	}

	/*
	 * SEVEN: A SECOND CLICK ADDS RATHER THAN REPLACING, AND THE MENU NOW SPEAKS FOR BOTH.
	 */
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
	 * EIGHT: CLICKING A SELECTED BRICK TAKES IT BACK OUT, AND LEAVES THE OTHER ALONE.
	 *
	 * WHAT BRICK 1 READS AFTERWARDS IS DELIBERATELY NOT PINNED. The ray that deselected it is
	 * also a ray that is pointing at it, so both None and Hovered are defensible and nothing
	 * depends on which. What must hold is that it is no longer SELECTED while brick 2 still is —
	 * the failure worth catching is a toggle that removes the wrong entry, or removes nothing.
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

	/*
	 * NINE: CLICKING EMPTY SPACE CLEARS THE SELECTION AND DISMISSES — and lets every brick go
	 * with it. A cleared set that left bricks lit tells the player they still have a selection.
	 */
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

	/*
	 * TEN: AND THE NEXT CLICK STARTS A FRESH SELECTION, so clearing left nothing wedged.
	 */
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

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
