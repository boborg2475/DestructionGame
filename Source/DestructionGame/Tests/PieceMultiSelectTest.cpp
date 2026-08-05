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
		case EBrickHighlight::Hovered:   return TEXT("Hovered");
		case EBrickHighlight::Selected:  return TEXT("Selected");
		case EBrickHighlight::Inspected: return TEXT("Inspected");
		default:                         return TEXT("None");
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

	/**
	 * Every brick's highlight at once, so a trail left behind reads as one message.
	 *
	 * SkipPieces IS FOR BRICKS THAT HAVE BEEN DELETED, and they are NAMED rather than
	 * skipped by being invalid — a brick that vanished without being deleted is exactly the
	 * failure this file is about, so "gone" must fail everywhere it was not asked for.
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
	 * How many joints touch a piece, counted straight off the graph.
	 *
	 * AN INDEPENDENT ORACLE FOR THE READOUT'S SENTENCE, and it is derived the other way round
	 * on purpose: the presenter asks InspectPiece for a list and counts it, this sweeps every
	 * connection in the structure and tests both ends. A joint that has been SEVERED by a
	 * removal is still one of the brick's joints and is exactly the row a player who just
	 * pulled a brick is looking for, so nothing here filters on HasGiven.
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

	/**
	 * That count as the sentence a player reads, spelled out here rather than imported.
	 *
	 * SINGULAR AND PLURAL ARE A BRANCH, and a test that called the production function would
	 * agree with it however wrong it was. "1 joints" is the same defect wearing a smaller coat.
	 */
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
 * THE STRONGER STATE WINS WHEN THEY COINCIDE, AND THE ORDER IS Inspected > Selected > Hovered.
 * A selected brick under the cursor stays Selected; a hover that overwrote it would make a chosen
 * brick flicker back to unchosen as the mouse passed over it, which reads as the selection having
 * been lost. And ONE selected brick can be singled out above the rest as the one the joint
 * readout is describing — a fourth state rather than a reuse of Selected, because a breakout of
 * one brick's forces drawn beside five bricks that look identical to it is ambiguous about which
 * brick it is the breakout OF.
 *
 * THE TRANSITIONS ARE COVERED, NOT JUST THE STATES, which is what steps thirteen onwards are.
 * Every bug in this area is a state left BEHIND: a brick left Inspected when the readout moved on
 * means two bricks claim one breakout, and a brick dropped to None instead of back to Selected
 * means running the cursor down the list silently empties the selection on screen while the
 * commit still deletes every one of them. SetHoveredPiece already has exactly this bug class
 * recorded against it.
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

	/*
	 * ELEVEN: DELETING THE BRICK THE CURSOR IS ON LEAVES NOTHING POINTING AT A DEAD ACTOR.
	 *
	 * THIS ONE IS A NET RATHER THAN A DRIVER, AND IT IS GREEN ON ARRIVAL. Brick 3 is both hovered
	 * and selected when it goes — the ray that picked it is the ray that is pointing at it — so
	 * after the commit HoveredPiece names a piece that has been tombstoned and an actor that has
	 * been destroyed. Nothing observable should be able to tell: RefreshPieceHighlight resolves
	 * the ref before it reaches for an actor, so a ref naming a piece that has gone refreshes
	 * nothing rather than dereferencing a destroyed one. It is pinned here because the highlight
	 * is about to grow a MATERIAL as well as a flag, and a material set on a destroyed component
	 * is the shape of bug that costs an afternoon.
	 *
	 * A HALF BAT IN THE TOP COURSE, so the delete orphans nothing and every other brick's state
	 * is unambiguous — this is about the hover pointer, not about a cascade.
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

	/*
	 * TWELVE: AND THE CURSOR STILL WORKS AFTERWARDS. The hovered ref is left naming a piece that
	 * has gone, so the next mouse move has to let go of a ref that resolves to nothing and take
	 * hold of a live one — which is the step that would go wrong if letting go reached for the
	 * actor instead of resolving first.
	 */
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

	/*
	 * THIRTEEN: A FRESH PICK OF THREE, WITH NOTHING SINGLED OUT YET. Brick 3 is gone from here
	 * on, so every check below names it as skipped rather than letting "gone" pass silently.
	 */
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
	 * FOURTEEN: INSPECTING ONE OF THEM SINGLES IT OUT, AND LEAVES THE REST OF THE SELECTION
	 * LIT AS IT WAS.
	 *
	 * THIS IS THE WHOLE POINT OF THE FOURTH STATE. The joint breakout on screen describes ONE
	 * brick, and the player picked six; if that brick draws the same as the other five, the
	 * numbers beside it are ambiguous about which brick they are the numbers OF. So Inspected
	 * must beat Selected — and the other two must stay Selected rather than dimming, because a
	 * selection that appears to shrink when you read one of its members is the same lie the
	 * hover-overwrites-selected bug told.
	 *
	 * AND IT PICKS NOTHING AND OPENS NOTHING. Reading a brick is not choosing it, exactly as
	 * pointing at one is not: the selection and the presented rows must come through untouched,
	 * or hovering down a list of six entries would rewrite the very list being hovered.
	 */
	{
		Controller->SetInspectedPiece(MultiSelectRef(StructureId, 1));

		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[0] = EBrickHighlight::Selected;
		Expected[1] = EBrickHighlight::Inspected;
		Expected[2] = EBrickHighlight::Selected;

		CheckMultiSelectHighlights(*this, Bricks, TEXT("inspecting brick 1 of three"), Expected, Deleted3);

		CheckMultiSelectMenu(
			*this, *Controller, TEXT("inspecting brick 1 of three"),
			{ MultiSelectRef(StructureId, 0), MultiSelectRef(StructureId, 1),
			  MultiSelectRef(StructureId, 2) });
	}

	/*
	 * FIFTEEN: INSPECTED BEATS HOVER TOO, AND A HOVER ELSEWHERE DOES NOT DISTURB IT.
	 *
	 * The precedence is a total order — Inspected > Selected > Hovered — so the brick being
	 * read stays the brick being read while the cursor wanders, which is exactly what the
	 * cursor is doing when it moves off a menu entry and across the wall behind it.
	 */
	{
		Controller->HoverAlongRay(
			MultiSelectRayStart(Reference.Boxes[1]), MultiSelectRayEnd(Reference.Boxes[1]));

		TArray<EBrickHighlight> OnTheInspected = MultiSelectNoHighlights();
		OnTheInspected[0] = EBrickHighlight::Selected;
		OnTheInspected[1] = EBrickHighlight::Inspected;
		OnTheInspected[2] = EBrickHighlight::Selected;

		CheckMultiSelectHighlights(
			*this, Bricks, TEXT("hovering the inspected brick 1"), OnTheInspected, Deleted3);

		Controller->HoverAlongRay(
			MultiSelectRayStart(Reference.Boxes[4]), MultiSelectRayEnd(Reference.Boxes[4]));

		TArray<EBrickHighlight> Elsewhere = MultiSelectNoHighlights();
		Elsewhere[0] = EBrickHighlight::Selected;
		Elsewhere[1] = EBrickHighlight::Inspected;
		Elsewhere[2] = EBrickHighlight::Selected;
		Elsewhere[4] = EBrickHighlight::Hovered;

		CheckMultiSelectHighlights(
			*this, Bricks, TEXT("hovering brick 4 while brick 1 is inspected"), Elsewhere, Deleted3);
	}

	/*
	 * SIXTEEN: INSPECTING A DIFFERENT BRICK HANDS THE PREVIOUS ONE BACK TO Selected — NOT TO
	 * None.
	 *
	 * THE BRICK BEING LEFT HAS TO BE REFRESHED AS WELL AS THE ONE BEING TAKEN UP, and this is
	 * the same bug class SetHoveredPiece already has recorded against it: without the first of
	 * the two, every brick the cursor crossed stays lit and the wall ends up entirely
	 * highlighted. Here the failure is sharper in both directions — a brick left Inspected
	 * means two bricks claim the one breakout, and a brick dropped to None means running the
	 * cursor down the list silently empties the selection on screen while the commit still
	 * deletes all three.
	 */
	{
		Controller->SetInspectedPiece(MultiSelectRef(StructureId, 2));

		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[0] = EBrickHighlight::Selected;
		Expected[1] = EBrickHighlight::Selected;
		Expected[2] = EBrickHighlight::Inspected;
		Expected[4] = EBrickHighlight::Hovered;

		CheckMultiSelectHighlights(
			*this, Bricks, TEXT("inspecting brick 2 after brick 1"), Expected, Deleted3);
	}

	/*
	 * SEVENTEEN: AND INSPECTING NOTHING GIVES THE BRICK BACK TO THE SELECTION. This is the
	 * cursor leaving the list entirely, and it is the route that has to leave all three bricks
	 * looking exactly as they did before anything was read.
	 */
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
	 * EIGHTEEN: A BRICK THAT IS NOT SELECTED CANNOT BE THE ONE BEING READ.
	 *
	 * SAME RULE AS BuildPieceMenuInspector'S, AND IT HAS TO BE THE SAME RULE OR THE TWO HALVES
	 * OF THE READOUT DISAGREE: the model refuses to single out an anchor outside the set it
	 * anchors, so a controller that lit one anyway would put the strongest highlight in the
	 * scene on a brick the panel says nothing about. It is also what makes the two steps below
	 * fall out of ONE rule rather than three clean-up sites somebody has to remember.
	 *
	 * Brick 4 is hovered, so this asks for the state it already has for another reason — which
	 * is the sharp version: what must not happen is the hover being PROMOTED.
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
	 * NINETEEN: DESELECTING THE BRICK BEING READ TAKES THE READOUT OFF IT.
	 *
	 * A brick that has just left the selection is a brick the panel no longer lists, so it
	 * cannot go on wearing the strongest highlight in the scene. What it reads INSTEAD is
	 * deliberately not pinned, for the reason step EIGHT gives: the ray that deselected it is
	 * also a ray pointing at it, so None and Hovered are both defensible. What must hold is
	 * that it is neither Inspected nor Selected while the other two are still Selected.
	 */
	{
		Controller->SetInspectedPiece(MultiSelectRef(StructureId, 1));

		TArray<EBrickHighlight> WhileInspected = MultiSelectNoHighlights();
		WhileInspected[0] = EBrickHighlight::Selected;
		WhileInspected[1] = EBrickHighlight::Inspected;
		WhileInspected[2] = EBrickHighlight::Selected;

		/* Brick 4 is still where the cursor was left in step fifteen, and nothing has moved it. */
		WhileInspected[4] = EBrickHighlight::Hovered;

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

	/*
	 * TWENTY: AND CLEARING THE SELECTION LETS THE INSPECTED BRICK GO WITH IT. A cleared set
	 * that left one brick still wearing the readout's own highlight tells the player they are
	 * still reading a brick that is no longer in a list that no longer exists.
	 */
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
	 * TWENTY-ONE: AND DELETING THE BRICK BEING READ LEAVES NOTHING POINTING AT A DEAD ACTOR.
	 *
	 * The counterpart of step ELEVEN for the third ref. Brick 5 is selected, inspected and
	 * hovered when it goes, so after the commit all three of the controller's refs name a
	 * tombstoned piece and a destroyed actor — and the next click has to let go of them and
	 * take hold of a live brick. A full brick of the TOP course, so the delete orphans nothing
	 * and every other brick's state stays unambiguous.
	 */
	const TArray<int32> Deleted3And5 = { 3, 5 };

	{
		Controller->InspectAlongRay(
			MultiSelectRayStart(Reference.Boxes[5]), MultiSelectRayEnd(Reference.Boxes[5]));

		Controller->SetInspectedPiece(MultiSelectRef(StructureId, 5));

		TArray<EBrickHighlight> WhileInspected = MultiSelectNoHighlights();
		WhileInspected[5] = EBrickHighlight::Inspected;

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

		/*
		 * AND THE NEXT CLICK STILL WORKS, on a brick that is not the dead one: a stale
		 * inspected ref must neither crash the refresh nor promote the next brick picked.
		 */
		Controller->InspectAlongRay(
			MultiSelectRayStart(Reference.Boxes[6]), MultiSelectRayEnd(Reference.Boxes[6]));

		TArray<EBrickHighlight> Expected = MultiSelectNoHighlights();
		Expected[6] = EBrickHighlight::Selected;

		CheckMultiSelectHighlights(
			*this, Bricks, TEXT("picking brick 6 after the inspected brick was deleted"),
			Expected, Deleted3And5);
	}

	/*
	 * TWENTY-TWO ONWARDS: THE READOUT MODEL THE PANEL IS DRAWN FROM, ASKED FOR THROUGH THE
	 * CONTROLLER RATHER THAN THROUGH BuildPieceMenuInspector.
	 *
	 * WHY THIS IS HERE AT ALL. PieceMenuInspectorForSelection had ZERO executed lines. Every
	 * headless world is built in code and so has no UGameViewportClient, so BuildPieceMenuWidget
	 * returned early, RefreshPieceMenuInspectorWidget returned early because its box was never
	 * valid, and its only two callers therefore never reached it. Tests/PieceInspectorTest.cpp
	 * covers BuildPieceMenuInspector thoroughly and Tests/PieceSelectionTest.cpp covers the
	 * selection; what nothing covered is the WIRE between them — which selection is handed over,
	 * which structure it is read against, and whether the singled-out brick is passed at all.
	 * That is precisely the shape CURRENT_STATE.md's integration entry rule names as unreachable
	 * by testing the halves: a call nobody makes.
	 *
	 * THE MUTATION THAT SURVIVED EVERYTHING. Passing FPieceRef() instead of InspectedPiece
	 * leaves the readout permanently blank for every brick hovered — the headline behaviour of
	 * the whole slice deleted — while the brick still lights magenta correctly, because
	 * HighlightForPiece reads InspectedPiece down a completely separate path. Step TWENTY-THREE
	 * is the one that kills it.
	 *
	 * SAME WORLD, SAME WALL, NO NEW FIXTURE. Bricks 3 and 5 are gone from here on, which is a
	 * feature rather than a leftover: brick 1 keeps the severed joints they left behind, so the
	 * joint sentence is asserted against a graph that has actually been through a removal.
	 */
	{
		/*
		 * TWENTY-TWO: NOTHING PICKED READS AS NOTHING PICKED, and it says so in words. This is
		 * the fail-closed route — Selected.Num() == 0 never reaches the binding at all — so a
		 * default-constructed inspector is the whole expectation.
		 */
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
		 * TWENTY-THREE: TWO BRICKS PICKED, ONE SINGLED OUT, AND THE READOUT IS ABOUT THAT ONE.
		 *
		 * THE JOINT SENTENCE IS THE LOAD-BEARING ASSERTION. A blank readout and a correct one
		 * are the same panel to every other test in this suite, so the count of joints is what
		 * separates "the controller handed the inspected ref over" from "it handed over a
		 * default and the model politely answered nothing". The expected count is swept off the
		 * graph rather than taken from the model, and the sentence is spelled here rather than
		 * imported, so agreement is evidence.
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

		/*
		 * A FIXTURE FLOOR ON THE ORACLE ITSELF. If the sweep found no joints, "No joints" would
		 * be the expectation and a blank readout would satisfy it — the mutation would live.
		 */
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
		/*
		 * TWENTY-FOUR: A BRICK THAT IS NOT PICKED CANNOT BE THE ONE BEING READ, and the count
		 * does not shrink because of it.
		 *
		 * SAME RULE THE HIGHLIGHT APPLIES, PINNED ON THE OTHER HALF. Step EIGHTEEN already says
		 * the wall will not light an unpicked brick; this says the panel will not read one out.
		 * They have to be the same rule or the strongest highlight in the scene sits on a brick
		 * the panel says nothing about — or worse, the other way round.
		 */
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
		 * TWENTY-FIVE: A BRICK RE-PICKED AFTER BEING DESELECTED WHILE IT WAS THE ONE BEING READ
		 * COMES BACK MERELY Selected.
		 *
		 * INspectedPiece IS NEVER CLEARED, ONLY MADE INERT by HighlightForPiece's membership
		 * conjunct — so a ref survives the brick leaving the selection and springs back to life
		 * the moment it rejoins. The player's cursor is nowhere near the menu at that point: the
		 * only thing that ever singles a brick out is hovering an entry row, and Slate delivers
		 * no OnMouseLeave to a widget that has left the tree, so a panel destroyed under the
		 * cursor leaves the ref set. The brick then lights up in the readout's own colour with a
		 * joint breakout open beside it, for no reason the player can see or undo.
		 *
		 * Both halves are asserted, because they are the two ends of the same stale ref: the
		 * WALL must read Selected, and the PANEL must not be reading anything out.
		 */
		Controller->SetInspectedPiece(MultiSelectRef(StructureId, 0));

		TArray<EBrickHighlight> WhileInspected = MultiSelectNoHighlights();
		WhileInspected[0] = EBrickHighlight::Inspected;
		WhileInspected[1] = EBrickHighlight::Selected;

		CheckMultiSelectHighlights(
			*this, Bricks, TEXT("inspecting brick 0 of the pair"), WhileInspected, Deleted3And5);

		/* Out of the selection... */
		Controller->InspectAlongRay(
			MultiSelectRayStart(Reference.Boxes[0]), MultiSelectRayEnd(Reference.Boxes[0]));

		/* ...and straight back into it, which is two ordinary clicks and nothing else. */
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

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
