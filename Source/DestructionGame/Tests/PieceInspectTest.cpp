// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceMenu.h"
#include "DestructionGamePlayerController.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges
 * many files into one. The world harness is NOT redeclared here: it lives in
 * Tests/BrickWorldTestSupport.h and four test files share it, because a second copy of a
 * floor height and a tick length is two fixtures that drift.
 */
namespace PieceInspectTestSupport
{
	/**
	 * How far along Y a ray starts and ends, either side of the wall.
	 *
	 * A brick is 10.25 cm deep and a wall is centred on Y = 0, so +/- 100 cm is far outside it
	 * on both sides and the ray crosses the whole thickness. Along Y rather than X or Z so
	 * nothing else in the wall is in the way and the answer is unambiguous. Same choice, and
	 * the same reason, as Tests/PieceClickTest.cpp.
	 */
	constexpr double InspectReachCm = 100.0;

	/** Far from the wall and above the slab, so a ray straight down hits absolutely nothing. */
	const FVector InspectEmptyAirCm(5000.0, 5000.0, 300.0);

	/**
	 * A point over the floor and well clear of the wall.
	 *
	 * The slab now genuinely straddles the origin (see BrickWorldTestSupport::SpawnFloor —
	 * it used to sit entirely in the +X +Y quadrant), so this needs no side to be chosen and
	 * is simply 300 cm out in Y from a wall that is 10.25 cm deep.
	 */
	const FVector InspectFloorPointCm(0.0, 300.0, 0.0);

	/** The Delete row, looked up by label so nothing hard-codes a position in the table. */
	const FPieceAction* FindInspectAction(const TCHAR* Label)
	{
		for (const FPieceAction& Action : AllPieceActions())
		{
			if (Action.Label != nullptr && FCString::Strcmp(Action.Label, Label) == 0)
			{
				return &Action;
			}
		}

		return nullptr;
	}

	/** What came back, so a failure reads without a debugger. */
	FString DescribeInspectRows(TArrayView<const FPieceMenuRow> Rows)
	{
		if (Rows.Num() == 0)
		{
			return TEXT("<empty>");
		}

		FString Line;

		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			Line += FString::Printf(
				TEXT("%s'%s'->{%d,%d}"),
				Index == 0 ? TEXT("") : TEXT(", "),
				*Rows[Index].Label,
				Rows[Index].Ref.StructureId,
				Rows[Index].Ref.PieceIndex);
		}

		return Line;
	}
}

/**
 * THE PLAYER CONTROLLER TURNS A RAY INTO THE MENU FOR THE BRICK IT HIT, DISMISSES IT WHEN THE
 * RAY HITS NOTHING, AND THE ROW IT PRESENTED COMMITS AGAINST THAT VERY BRICK.
 *
 * ON THE CONTROLLER, NOT THE PAWN, and this is the test that pins it. The controller already
 * owns the mapping contexts, it outlives any pawn, and it is what carries the cursor and the
 * deprojection — a binding on the pawn goes away with the pawn, which for a spectator that
 * can be re-possessed is a menu that stops opening for no visible reason.
 *
 * A RAY IN RATHER THAN A CLICK, WHICH IS WHERE THE SEAM IS DRAWN AND WHY. Turning the cursor
 * into a world ray needs a viewport, and a headless run has none; everything after that —
 * trace, resolve, build the rows, present or dismiss — needs only a world. So THIS is the
 * half that is asserted, and the deprojection itself is deliberately untested. So is whether
 * a Slate widget appeared: that costs a viewport and a widget tree to learn nothing the rows
 * do not already say, and Tests/PieceMenuTest.cpp owns the rows.
 *
 * PER PIECE, NOT ONCE. The defect that matters here is a wall spawned in the right places
 * with the wrong refs — every brick present, every brick the right size, and inspecting one
 * offers a menu that deletes another. A single inspection cannot see it; one per brick can,
 * because the wrong answer has to be wrong for a particular brick. Tests/PieceClickTest.cpp
 * makes the same argument about TracePiece; the overlap is in the fixture, not the subject.
 *
 * DISMISSAL IS ASSERTED ON ITS OWN, and it is the half that is easy to leave out. A handler
 * that opens a menu on a hit and simply returns on a miss leaves the previous brick's menu on
 * screen naming a brick the player is no longer pointing at — and then a click on Delete
 * removes it. Nothing about that looks like a bug until a wall loses a brick nobody chose.
 *
 * AND THE MISS ROWS ARE NOW ASSERTED AGAINST THE PRESENTER, NOT ONLY AGAINST THE RETURN VALUE,
 * which is what makes them able to fail at all. CURRENT_STATE.md records that the two miss
 * rows below were structurally redundant: three independent guards each produce an empty list
 * for a miss, so no single mutation of the wire could make one return rows. What they were
 * written to guard against is a STATEFUL presenter — and the moment something remembers the
 * last menu, "open on a hit, return on a miss" becomes writable and is the obvious shape to
 * write. So a miss must now leave the presenter EMPTY as well as return nothing, and the
 * per-brick loop asserts the presenter is showing that brick's rows so that there is genuinely
 * a menu up for the miss to fail to dismiss.
 *
 * THE PRESENTING BELONGS TO InspectAlongRay RATHER THAN TO THE INPUT HANDLER, and that is the
 * placement this test pins. The handler is the one part of the chain no test can reach, so it
 * must stay a deprojection plus one call; putting "and then show it" in the handler would move
 * the dismiss-on-miss decision — the exact thing above — into the untestable half.
 *
 * THIS CONTROLLER HAS NO ULocalPlayer, DELIBERATELY. It is spawned bare, so there is no
 * Enhanced Input subsystem to remove a look context from and no cursor to show. Presenting
 * must therefore work rather than merely not crash without one;
 * DestructionGame.World.Menu.PieceMenuPresenterState owns the half that needs a local player.
 *
 * NEEDS A TICKING WORLD: it needs a WORLD — actors to spawn into, a physics scene for the
 * trace, a controller to spawn — but it deliberately never ticks one. Nothing here is about
 * anything falling.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceInspectOpensTheMenuTest,
	"DestructionGame.World.Inspect.InspectingABrickOpensItsMenu",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceInspectOpensTheMenuTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;
	using namespace PieceInspectTestSupport;

	const FPieceAction* const Delete = FindInspectAction(TEXT("Delete"));

	if (Delete == nullptr)
	{
		AddError(TEXT("fixture: the action table must contain a row labelled 'Delete'"));
		return true;
	}

	const FRunningBondSpec Spec = WallSpec();

	/*
	 * THE REFERENCE LAYOUT IS LAID SEPARATELY, so the points inspected at come from the
	 * producer rather than from whatever the subsystem happened to spawn. A spawner that put
	 * every brick at the origin would otherwise be inspected at the origin and agree with
	 * itself. Same reasoning as Tests/PieceClickTest.cpp.
	 */
	FBrickLayout Reference;

	TestTrue(TEXT("fixture: RunningBond should lay the reference wall"), RunningBond(Spec, Reference));

	TestEqual(
		FString::Printf(TEXT("fixture: the shared wall spec should be %d pieces, got %d"),
			WallPieceCount, Reference.Structure.NumPieces()),
		Reference.Structure.NumPieces(), WallPieceCount);

	if (Reference.Boxes.Num() != WallPieceCount)
	{
		return true;
	}

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	const int32 StructureId = TestWorld.Subsystem->BuildRunningBond(Spec);
	FStructureBinding* Binding = TestWorld.Subsystem->Find(StructureId);

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

	TestNotNull(TEXT("fixture: the test world should be able to spawn the game's player controller"), Controller);

	if (Controller == nullptr)
	{
		TestWorld.End();
		return true;
	}

	/*
	 * ONE: INSPECTING BRICK k OFFERS BRICK k'S MENU, FOR EVERY BRICK IN THE WALL.
	 */
	for (int32 Piece = 0; Piece < Reference.Boxes.Num(); ++Piece)
	{
		const FPieceBox& Box = Reference.Boxes[Piece];

		const TArray<FPieceMenuRow> Rows = Controller->InspectAlongRay(
			FVector(Box.CentreCm.X, Box.CentreCm.Y - InspectReachCm, Box.CentreCm.Z),
			FVector(Box.CentreCm.X, Box.CentreCm.Y + InspectReachCm, Box.CentreCm.Z));

		/*
		 * THE EXPECTATION IS WHAT THE TABLE ALLOWS, ASKED FOR SEPARATELY, so this stays true
		 * the day a second action lands rather than being a hard-coded one-row menu.
		 */
		FPieceRef ExpectedRef;
		ExpectedRef.StructureId = StructureId;
		ExpectedRef.PieceIndex = Piece;

		const TArray<const FPieceAction*> Allowed = PieceActionsFor(*Binding, ExpectedRef);

		TestEqual(
			FString::Printf(
				TEXT("inspecting piece %d should offer the %d row(s) the table allows, got %d [%s]"),
				Piece, Allowed.Num(), Rows.Num(), *DescribeInspectRows(Rows)),
			Rows.Num(), Allowed.Num());

		if (Rows.Num() != Allowed.Num())
		{
			continue;
		}

		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			TestTrue(
				*FString::Printf(TEXT("inspecting piece %d: row %d should be a row of the shipped table"),
					Piece, Index),
				Rows[Index].Action == Allowed[Index]);

			TestEqual(
				FString::Printf(TEXT("inspecting piece %d: row %d should name structure %d, got %d"),
					Piece, Index, StructureId, Rows[Index].Ref.StructureId),
				Rows[Index].Ref.StructureId, StructureId);

			/*
			 * THE ROW'S REF IS THE WHOLE POINT OF INSPECTING PER PIECE. A wall whose bricks
			 * carry each other's refs offers a perfect menu on every brick and deletes the
			 * wrong one, and 1,220 identical bricks make that undetectable by eye.
			 */
			TestEqual(
				FString::Printf(TEXT("inspecting piece %d: row %d should name piece %d, got %d"),
					Piece, Index, Piece, Rows[Index].Ref.PieceIndex),
				Rows[Index].Ref.PieceIndex, Piece);
		}

		/*
		 * AND THE ROWS ARE ACTUALLY PRESENTED, not merely returned. Handing them back is what
		 * makes this testable; putting them on screen is what makes it a menu.
		 */
		const TArrayView<const FPieceMenuRow> Shown = Controller->GetShownPieceMenuRows();

		TestTrue(
			*FString::Printf(TEXT("inspecting piece %d should leave its menu on screen, it shows [%s]"),
				Piece, *DescribeInspectRows(Shown)),
			Controller->IsPieceMenuShown());

		TestEqual(
			FString::Printf(
				TEXT("inspecting piece %d should present the %d row(s) it returned, it presents %d [%s]"),
				Piece, Rows.Num(), Shown.Num(), *DescribeInspectRows(Shown)),
			Shown.Num(), Rows.Num());

		if (Shown.Num() == Rows.Num())
		{
			for (int32 Index = 0; Index < Shown.Num(); ++Index)
			{
				TestTrue(
					*FString::Printf(
						TEXT("inspecting piece %d: presented row %d should be the row it returned, it names {%d,%d}"),
						Piece, Index, Shown[Index].Ref.StructureId, Shown[Index].Ref.PieceIndex),
					Shown[Index].Action == Rows[Index].Action
						&& Shown[Index].Ref.StructureId == Rows[Index].Ref.StructureId
						&& Shown[Index].Ref.PieceIndex == Rows[Index].Ref.PieceIndex);
			}
		}
	}

	/*
	 * TWO: A RAY THAT HIT NOTHING DISMISSES THE MENU RATHER THAN LEAVING THE LAST ONE UP.
	 */
	struct FMissCase
	{
		const TCHAR* Description;
		FVector Start;
		FVector End;
	};

	const TArray<FMissCase> Misses = {
		{
			TEXT("inspecting the floor"),
			InspectFloorPointCm + FVector(0.0, 0.0, 200.0),
			InspectFloorPointCm + FVector(0.0, 0.0, -200.0)
		},
		{
			TEXT("inspecting nothing at all"),
			InspectEmptyAirCm,
			InspectEmptyAirCm + FVector(0.0, 0.0, -100.0)
		},
	};

	for (const FMissCase& Miss : Misses)
	{
		/*
		 * A MENU IS UP GOING INTO EVERY MISS, and that is what gives the two rows below
		 * something to fail at. The loop above left one on screen; re-showing here makes the
		 * precondition explicit rather than depending on the order of the file, and it is
		 * asserted, because a miss that dismisses nothing is not evidence of anything.
		 */
		Controller->InspectAlongRay(
			FVector(Reference.Boxes[0].CentreCm.X, Reference.Boxes[0].CentreCm.Y - InspectReachCm, Reference.Boxes[0].CentreCm.Z),
			FVector(Reference.Boxes[0].CentreCm.X, Reference.Boxes[0].CentreCm.Y + InspectReachCm, Reference.Boxes[0].CentreCm.Z));

		TestTrue(
			*FString::Printf(TEXT("fixture: a menu should be on screen before %s"), Miss.Description),
			Controller->IsPieceMenuShown());

		const TArray<FPieceMenuRow> Rows = Controller->InspectAlongRay(Miss.Start, Miss.End);

		TestEqual(
			FString::Printf(TEXT("%s must dismiss the menu, it offered [%s]"),
				Miss.Description, *DescribeInspectRows(Rows)),
			Rows.Num(), 0);

		/*
		 * THE ROW THAT CAN ACTUALLY FAIL. Returning nothing is guarded three times over
		 * already; leaving the last brick's menu on screen is not guarded at all until
		 * something remembers it, which is what the widget half now does.
		 */
		TestTrue(
			*FString::Printf(TEXT("%s must take the previous brick's menu down, it still shows [%s]"),
				Miss.Description, *DescribeInspectRows(Controller->GetShownPieceMenuRows())),
			!Controller->IsPieceMenuShown()
				&& Controller->GetShownPieceMenuRows().Num() == 0);
	}

	/*
	 * THREE: THE ROW THAT WAS PRESENTED COMMITS AGAINST THE BRICK IT NAMED, AND ONLY THAT ONE.
	 *
	 * This is what makes a row worth presenting at all: the pair it carries is exactly the
	 * pair UDestructionStructureSubsystem::CommitPieceAction takes, so a menu built here and
	 * chosen later needs nothing remembered on the side.
	 */
	constexpr int32 InspectedPiece = 0;

	const FPieceBox& ChosenBox = Reference.Boxes[InspectedPiece];

	const TArray<FPieceMenuRow> ChosenRows = Controller->InspectAlongRay(
		FVector(ChosenBox.CentreCm.X, ChosenBox.CentreCm.Y - InspectReachCm, ChosenBox.CentreCm.Z),
		FVector(ChosenBox.CentreCm.X, ChosenBox.CentreCm.Y + InspectReachCm, ChosenBox.CentreCm.Z));

	if (ChosenRows.Num() == 0)
	{
		AddError(FString::Printf(
			TEXT("inspecting piece %d offered nothing, so the commit assertions below cannot run"),
			InspectedPiece));

		TestWorld.End();
		return true;
	}

	const FPieceMenuRow& DeleteRow = ChosenRows[0];

	TestTrue(
		*FString::Printf(TEXT("fixture: the first row offered should be Delete, it reads '%s'"),
			*DeleteRow.Label),
		DeleteRow.Action == Delete);

	ABrickActor* const ChosenBrick = Bricks[InspectedPiece];

	TestTrue(
		TEXT("committing the presented row should report that it ran"),
		TestWorld.Subsystem->CommitPieceAction(DeleteRow.Ref, *DeleteRow.Action));

	TestTrue(
		FString::Printf(TEXT("the inspected piece %d must be gone from the graph"), InspectedPiece),
		Binding->IsPieceRemoved(InspectedPiece));

	TestTrue(
		FString::Printf(TEXT("the inspected brick's actor must have been destroyed, it is %s"),
			IsValid(ChosenBrick) ? TEXT("still valid") : TEXT("gone")),
		!IsValid(ChosenBrick));

	for (int32 Piece = 0; Piece < Bricks.Num(); ++Piece)
	{
		if (Piece == InspectedPiece)
		{
			continue;
		}

		TestTrue(
			FString::Printf(TEXT("brick %d was not chosen and must still be in the world"), Piece),
			IsValid(Bricks[Piece]));
	}

	/*
	 * AND INSPECTING THE HOLE IT LEFT OFFERS NOTHING. The actor is gone, so the ray misses —
	 * the same staleness RunPieceAction's re-resolve exists for, seen from the presenter's end.
	 */
	{
		const TArray<FPieceMenuRow> Rows = Controller->InspectAlongRay(
			FVector(ChosenBox.CentreCm.X, ChosenBox.CentreCm.Y - InspectReachCm, ChosenBox.CentreCm.Z),
			FVector(ChosenBox.CentreCm.X, ChosenBox.CentreCm.Y + InspectReachCm, ChosenBox.CentreCm.Z));

		TestEqual(
			FString::Printf(TEXT("inspecting where the deleted brick was must offer nothing, got [%s]"),
				*DescribeInspectRows(Rows)),
			Rows.Num(), 0);
	}

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
