// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceMenu.h"
#include "DestructionGamePlayerController.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

// Named, not anonymous: anonymous namespaces from different files collide in a unity build.
namespace PieceInspectTestSupport
{
	/**
	 * Ray half-length along Y either side of the wall (10.25 cm deep, centred on Y 0). Along Y so
	 * nothing else in the wall is in the way, as in Tests/PieceClickTest.cpp.
	 */
	constexpr double InspectReachCm = 100.0;

	/** Far from the wall and off the slab, so a ray down hits nothing. */
	const FVector InspectEmptyAirCm(5000.0, 5000.0, 300.0);

	/** Over the floor, 300 cm clear of the wall. */
	const FVector InspectFloorPointCm(0.0, 300.0, 0.0);

	/** An action row looked up by label, not table position. */
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

	/** Rows as text, for failure messages. */
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
 * The player controller turns a ray into the menu for the brick it hit, dismisses it on a miss,
 * and the presented row commits against that brick. On the controller, not the pawn, since a pawn
 * binding disappears with the pawn.
 *
 * A ray rather than a click: deprojecting the cursor needs a viewport, so only trace-onwards is
 * tested. Per piece, because a wall with swapped refs offers a perfect menu that deletes the wrong
 * brick. A miss must also clear the presenter, or the last brick's menu stays up and Delete removes
 * a brick nobody is pointing at. Presenting lives in InspectAlongRay, not the untestable input
 * handler. No ULocalPlayer (PieceMenuPresenterState covers that half). Needs a world, never ticks.
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

	// Inspect points come from a separately laid reference, so a bad spawner cannot agree with itself.
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

	// Inspecting brick k offers brick k's menu, for every brick.
	for (int32 Piece = 0; Piece < Reference.Boxes.Num(); ++Piece)
	{
		const FPieceBox& Box = Reference.Boxes[Piece];

		const TArray<FPieceMenuRow> Rows = Controller->InspectAlongRay(
			FVector(Box.CentreCm.X, Box.CentreCm.Y - InspectReachCm, Box.CentreCm.Z),
			FVector(Box.CentreCm.X, Box.CentreCm.Y + InspectReachCm, Box.CentreCm.Z));

		// Expect what the table allows, so a new action does not break this.
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

			TestEqual(
				FString::Printf(TEXT("inspecting piece %d: row %d should name piece %d, got %d"),
					Piece, Index, Piece, Rows[Index].Ref.PieceIndex),
				Rows[Index].Ref.PieceIndex, Piece);
		}

		// Presented, not merely returned.
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

	// A ray that hits nothing dismisses the menu.
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
		// Put a menu up first, so the miss has something to dismiss.
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

		// The return value is guarded three times over; the presenter is what can actually fail.
		TestTrue(
			*FString::Printf(TEXT("%s must take the previous brick's menu down, it still shows [%s]"),
				Miss.Description, *DescribeInspectRows(Controller->GetShownPieceMenuRows())),
			!Controller->IsPieceMenuShown()
				&& Controller->GetShownPieceMenuRows().Num() == 0);
	}

	/*
	 * The presented row commits against the brick it named, and only that one. A row carries exactly
	 * what CommitPieceAction takes.
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

	// Inspecting the hole it left offers nothing.
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
