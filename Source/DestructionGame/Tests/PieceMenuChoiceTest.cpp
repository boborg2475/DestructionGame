// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceActions.h"
#include "Core/PieceMenu.h"
#include "DestructionGamePlayerController.h"
#include "InputMappingContext.h"
#include "RequiredContent.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Uniquely named namespace: unity builds merge files, so anonymous namespaces can collide. The
 * world harness is shared from Tests/BrickWorldTestSupport.h.
 */
namespace PieceMenuChoiceTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/**
	 * A second action, distinct from Delete, that counts its runs and records the handle it got.
	 * Distinguishes "row N's action" from "the menu's action" and catches the right action run
	 * against the wrong brick.
	 *
	 * It really removes the piece: CommitPieceAction destroys the actor RunPieceAction returns on
	 * success, so reporting success without removing would tear a live brick out of the world.
	 */
	int32 ChoiceTripwireRunCount = 0;
	int32 ChoiceTripwireLastHandle = INDEX_NONE;

	bool ChoiceTripwireCanRun(const FStructureBinding&, int32)
	{
		return true;
	}

	bool ChoiceTripwireRun(FPieceActionContext& Context)
	{
		++ChoiceTripwireRunCount;
		ChoiceTripwireLastHandle = Context.PieceHandle;

		return Context.Binding.RemovePiece(Context.PieceHandle);
	}

	const FPieceAction ChoiceTripwireAction{
		TEXT("<recording delete>"), &ChoiceTripwireCanRun, &ChoiceTripwireRun };

	/** Find an action by label, so nothing hard-codes a table position. */
	const FPieceAction* FindChoiceAction(const TCHAR* Label)
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

	FPieceRef ChoiceRef(int32 StructureId, int32 PieceIndex)
	{
		FPieceRef Ref;
		Ref.StructureId = StructureId;
		Ref.PieceIndex = PieceIndex;

		return Ref;
	}

	/**
	 * A two-row menu whose rows name different bricks, from two production BuildPieceMenuRows calls.
	 * With one row, "commit the chosen row's ref" and "commit a remembered ref" are indistinguishable.
	 */
	TArray<FPieceMenuRow> ChoiceMenu(
		const FPieceAction& FirstAction,
		const FPieceRef& FirstRef,
		const FPieceAction& SecondAction,
		const FPieceRef& SecondRef)
	{
		const TArray<const FPieceAction*> First = { &FirstAction };
		const TArray<const FPieceAction*> Second = { &SecondAction };

		TArray<FPieceMenuRow> Rows = BuildPieceMenuRows(First, FirstRef);
		Rows.Append(BuildPieceMenuRows(Second, SecondRef));

		return Rows;
	}

	/** Rows as text, for failure messages. */
	FString DescribeChoiceRows(TArrayView<const FPieceMenuRow> Rows)
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

	/** Assert whether a menu is shown and that it presents exactly these rows (empty = dismissed). */
	void CheckChoiceMenu(
		FAutomationTestBase& Test,
		const ADestructionGamePlayerController& Controller,
		const TCHAR* Where,
		const TArray<FPieceMenuRow>& Expected)
	{
		const TArrayView<const FPieceMenuRow> Shown = Controller.GetShownPieceMenuRows();

		Test.TestEqual(
			FString::Printf(TEXT("%s: a menu should%s be shown, it shows [%s]"),
				Where, Expected.Num() > 0 ? TEXT("") : TEXT(" NOT"), *DescribeChoiceRows(Shown)),
			Controller.IsPieceMenuShown(), Expected.Num() > 0);

		Test.TestEqual(
			FString::Printf(TEXT("%s: should present %d row(s), it presents %d [%s]"),
				Where, Expected.Num(), Shown.Num(), *DescribeChoiceRows(Shown)),
			Shown.Num(), Expected.Num());

		if (Shown.Num() != Expected.Num())
		{
			return;
		}

		for (int32 Index = 0; Index < Shown.Num(); ++Index)
		{
			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: row %d should still be '%s'->{%d,%d}, it reads '%s'->{%d,%d}"),
					Where, Index,
					*Expected[Index].Label, Expected[Index].Ref.StructureId, Expected[Index].Ref.PieceIndex,
					*Shown[Index].Label, Shown[Index].Ref.StructureId, Shown[Index].Ref.PieceIndex),
				Shown[Index].Action == Expected[Index].Action
					&& Shown[Index].Ref.StructureId == Expected[Index].Ref.StructureId
					&& Shown[Index].Ref.PieceIndex == Expected[Index].Ref.PieceIndex);
		}
	}

	/**
	 * Assert the controls never move: cursor shown and IMC_MouseLook and IMC_Default applied, menu
	 * or not. Since SESSION_UI_DESIGN §d (S6) look is gated by the RMB chord, so no menu route
	 * should touch them. Tests/PieceMenuPresenterTest.cpp owns the contract; this covers the
	 * choose-a-row route.
	 */
	void CheckChoiceControls(
		FAutomationTestBase& Test,
		const ADestructionGamePlayerController& Controller,
		UEnhancedInputLocalPlayerSubsystem& Input,
		const UInputMappingContext& DefaultContext,
		const UInputMappingContext& MouseLookContext,
		const TCHAR* Where)
	{
		Test.TestTrue(
			*FString::Printf(TEXT("%s: the mouse cursor must be shown — it is the session's pointer, "
								  "up whether or not a menu is"), Where),
			Controller.bShowMouseCursor);

		Test.TestTrue(
			*FString::Printf(TEXT("%s: IMC_MouseLook must stay applied — look is gated by the RMB "
								  "chord, so nothing about a menu may remove it"), Where),
			Input.HasMappingContext(&MouseLookContext));

		// IMC_Default carries IA_InspectPiece, which closes a menu by clicking away.
		Test.TestTrue(
			*FString::Printf(TEXT("%s: IMC_Default must stay applied whatever the menu is doing"), Where),
			Input.HasMappingContext(&DefaultContext));
	}

	/** Whether a piece is still in the graph AND its brick is still in the world. */
	void CheckPieceUntouched(
		FAutomationTestBase& Test,
		FStructureBinding& Binding,
		const TArray<ABrickActor*>& Bricks,
		const TCHAR* Where,
		int32 Piece)
	{
		Test.TestTrue(
			*FString::Printf(TEXT("%s: piece %d must still be in the graph"), Where, Piece),
			!Binding.IsPieceRemoved(Piece));

		Test.TestTrue(
			*FString::Printf(TEXT("%s: brick %d must still be standing in the world, it is %s"),
				Where, Piece, IsValid(Bricks[Piece]) ? TEXT("valid") : TEXT("gone")),
			IsValid(Bricks[Piece]));
	}

	/*
	 * Three distinct live bricks of the shared flush 2 x 3 wall. Nothing calls ApplyResults, so
	 * removals release nothing and CanRun answers stay fixed.
	 */
	constexpr int32 ChoiceRecordedPiece = 6;
	constexpr int32 ChoiceDeletedPiece = 3;
	constexpr int32 ChoiceBystanderPiece = 4;

	/*
	 * The end-to-end test uses NarrowWaistWallSpec(3); removing the waist changes other pieces'
	 * support, which makes the re-solve observable (the flush 2 x 3 wall would not).
	 *
	 *      course 2         [ 3 ][ 4 ]
	 *      course 1            [ 2 ]        the waist, inspected and chosen
	 *      course 0         [ 0 ][ 1 ]      grounded
	 */

	constexpr int32 ChoiceWaistWallPieceCount = 5;

	/** The course-1 brick everything above hangs from. */
	constexpr int32 ChoiceWaistPiece = 2;

	/** The pieces that lose their path to the ground when the waist goes. */
	constexpr bool bChoiceOrphanedByTheWaist[ChoiceWaistWallPieceCount] =
	{
		false, false, false, true, true
	};

	/** Ray half-length along Y through the wall (centred on Y = 0, 10.25 cm deep), cm. */
	constexpr double ChoiceInspectReachCm = 100.0;

	const TCHAR* ChoiceSupportName(EPieceSupport Support)
	{
		switch (Support)
		{
		case EPieceSupport::Grounded:  return TEXT("Grounded");
		case EPieceSupport::Supported: return TEXT("Supported");
		case EPieceSupport::Stranded:  return TEXT("Stranded");
		default:                       return TEXT("Falling");
		}
	}
}

/**
 * Choosing row N commits row N's own action against row N's own ref and dismisses the menu,
 * without moving the controls. An index naming no row commits nothing and leaves the menu up.
 *
 * Two rows on different bricks, with row 0 a recording tripwire, separate "the row's ref/action"
 * from "the menu's". Refusals are checked by the tripwire's run count, not by absence of damage
 * (see Core.PieceActions.CommitRefFailsClosed); clamping the index is the likely wrong fix and
 * would run row 0.
 *
 * Not pinned: what to do when the chosen row's own commit refuses (e.g. its brick was removed by
 * a cascade). Needs a world and a real ULocalPlayer, but no ticking.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuChoiceCommitsThatRowTest,
	"DestructionGame.World.Choose.ChoosingARowCommitsThatRow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuChoiceCommitsThatRowTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;
	using namespace PieceMenuChoiceTestSupport;

	const FPieceAction* const Delete = FindChoiceAction(TEXT("Delete"));

	if (Delete == nullptr)
	{
		AddError(TEXT("fixture: the action table must contain a row labelled 'Delete'"));
		return true;
	}

	const UInputMappingContext* const DefaultContext = LoadObject<UInputMappingContext>(
		nullptr, DestructionContent::DefaultMappingContextPath);

	const UInputMappingContext* const MouseLookContext = LoadObject<UInputMappingContext>(
		nullptr, DestructionContent::MouseLookMappingContextPath);

	if (DefaultContext == nullptr || MouseLookContext == nullptr)
	{
		AddError(TEXT("fixture: IMC_Default and IMC_MouseLook should both resolve"));
		return true;
	}

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	const int32 StructureId = TestWorld.Subsystem->BuildRunningBond(WallSpec());
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
		SpawnControllerWithLocalPlayer(*this, TestWorld.World);

	UEnhancedInputLocalPlayerSubsystem* const Input = InputSubsystemOf(Controller);

	TestNotNull(
		TEXT("fixture: a controller with a local player should have an Enhanced Input subsystem"),
		Input);

	if (Controller == nullptr || Input == nullptr)
	{
		TestWorld.End();
		return true;
	}

	ChoiceTripwireRunCount = 0;
	ChoiceTripwireLastHandle = INDEX_NONE;

	// Row 0: the tripwire on one brick. Row 1: the shipped Delete on another.
	const TArray<FPieceMenuRow> Menu = ChoiceMenu(
		ChoiceTripwireAction, ChoiceRef(StructureId, ChoiceRecordedPiece),
		*Delete, ChoiceRef(StructureId, ChoiceDeletedPiece));

	TestEqual(
		FString::Printf(TEXT("fixture: the two-brick menu should build 2 rows, it built %d [%s]"),
			Menu.Num(), *DescribeChoiceRows(Menu)),
		Menu.Num(), 2);

	if (Menu.Num() != 2)
	{
		TestWorld.End();
		return true;
	}

	// Preconditions: distinct action pointers, and all three pieces live.
	TestTrue(
		TEXT("fixture: the two rows must carry DIFFERENT action pointers, or nothing here discriminates"),
		Menu[0].Action != Menu[1].Action);

	for (const int32 Piece : { ChoiceRecordedPiece, ChoiceDeletedPiece, ChoiceBystanderPiece })
	{
		CheckPieceUntouched(*this, *Binding, Bricks, TEXT("fixture"), Piece);
	}

	// 1. An index naming no row commits nothing and changes nothing.
	Controller->ShowPieceMenu(Menu);

	CheckChoiceMenu(*this, *Controller, TEXT("fixture, with the two-row menu up"), Menu);

	CheckChoiceControls(
		*this, *Controller, *Input, *DefaultContext, *MouseLookContext,
		TEXT("fixture, with the two-row menu up"));

	struct FRefusedIndexCase
	{
		const TCHAR* Description;
		int32 RowIndex;
	};

	const TArray<FRefusedIndexCase> Refused = {
		{ TEXT("a negative row index"), -1 },
		{ TEXT("one past the last row"), 2 },
		{ TEXT("INDEX_NONE as a row index"), INDEX_NONE },
		{ TEXT("a wildly out-of-range row index"), MAX_int32 },
		{ TEXT("MIN_int32, which must not be negated into range"), MIN_int32 },
	};

	for (const FRefusedIndexCase& Case : Refused)
	{
		const bool bChose = Controller->ChoosePieceMenuRow(Case.RowIndex);

		TestTrue(
			*FString::Printf(TEXT("%s: choosing row %d must report that it did nothing, it reported %d"),
				Case.Description, Case.RowIndex, bChose ? 1 : 0),
			!bChose);

		// Mechanism check: a clamped index would run row 0's tripwire.
		TestTrue(
			*FString::Printf(
				TEXT("%s: a refused index must not enter any row's Run, but the tripwire has been entered %d time(s)"),
				Case.Description, ChoiceTripwireRunCount),
			ChoiceTripwireRunCount == 0);

		TestEqual(
			FString::Printf(TEXT("%s: all %d pieces must still be live, got %d"),
				Case.Description, WallPieceCount, Binding->GetStructure().NumLivePieces()),
			Binding->GetStructure().NumLivePieces(), WallPieceCount);

		CheckPieceUntouched(*this, *Binding, Bricks, Case.Description, ChoiceRecordedPiece);
		CheckPieceUntouched(*this, *Binding, Bricks, Case.Description, ChoiceDeletedPiece);

		// The menu stays up, unchanged.
		CheckChoiceMenu(*this, *Controller, Case.Description, Menu);

		CheckChoiceControls(
			*this, *Controller, *Input, *DefaultContext, *MouseLookContext,
			Case.Description);
	}

	// 2. Choosing with no menu up is the same refusal.
	Controller->DismissPieceMenu();

	{
		const bool bChose = Controller->ChoosePieceMenuRow(0);

		TestTrue(
			FString::Printf(TEXT("choosing row 0 with no menu up must report that it did nothing, it reported %d"),
				bChose ? 1 : 0),
			!bChose);

		TestTrue(
			FString::Printf(
				TEXT("choosing with no menu up must not enter any row's Run, but the tripwire has been entered %d time(s)"),
				ChoiceTripwireRunCount),
			ChoiceTripwireRunCount == 0);

		TestEqual(
			FString::Printf(TEXT("choosing with no menu up must leave all %d pieces live, got %d"),
				WallPieceCount, Binding->GetStructure().NumLivePieces()),
			Binding->GetStructure().NumLivePieces(), WallPieceCount);

		CheckChoiceMenu(*this, *Controller, TEXT("after choosing with no menu up"), TArray<FPieceMenuRow>());
	}

	/*
	 * 3. Choosing row 1 runs row 1's action on row 1's brick. The tripwire count checks the action;
	 * row 0's brick still standing checks the ref.
	 */
	Controller->ShowPieceMenu(Menu);

	{
		const bool bChose = Controller->ChoosePieceMenuRow(1);

		TestTrue(
			FString::Printf(TEXT("choosing row 1 ('%s' on piece %d) should report that it committed, it reported %d"),
				*Menu[1].Label, ChoiceDeletedPiece, bChose ? 1 : 0),
			bChose);

		TestTrue(
			FString::Printf(TEXT("choosing row 1 must take piece %d out of the graph, IsPieceRemoved reports %d"),
				ChoiceDeletedPiece, Binding->IsPieceRemoved(ChoiceDeletedPiece) ? 1 : 0),
			Binding->IsPieceRemoved(ChoiceDeletedPiece));

		TestTrue(
			FString::Printf(TEXT("choosing row 1 must destroy brick %d's actor, it is %s"),
				ChoiceDeletedPiece, IsValid(Bricks[ChoiceDeletedPiece]) ? TEXT("still valid") : TEXT("gone")),
			!IsValid(Bricks[ChoiceDeletedPiece]));

		TestTrue(
			FString::Printf(
				TEXT("choosing row 1 must not run row 0's action, but the tripwire has been entered %d time(s)"),
				ChoiceTripwireRunCount),
			ChoiceTripwireRunCount == 0);

		CheckPieceUntouched(
			*this, *Binding, Bricks, TEXT("after choosing row 1"), ChoiceRecordedPiece);

		TestEqual(
			FString::Printf(TEXT("choosing row 1 must remove exactly one piece, so %d should be live, got %d"),
				WallPieceCount - 1, Binding->GetStructure().NumLivePieces()),
			Binding->GetStructure().NumLivePieces(), WallPieceCount - 1);

		// The menu is dismissed.
		CheckChoiceMenu(*this, *Controller, TEXT("after choosing row 1"), TArray<FPieceMenuRow>());

		CheckChoiceControls(
			*this, *Controller, *Input, *DefaultContext, *MouseLookContext,
			TEXT("after choosing row 1"));
	}

	/*
	 * 4. The reverse: choosing row 0 runs row 0's action on row 0's brick (checked by the recorded
	 * handle) and leaves row 1's brick alone.
	 */
	const TArray<FPieceMenuRow> SecondMenu = ChoiceMenu(
		ChoiceTripwireAction, ChoiceRef(StructureId, ChoiceRecordedPiece),
		*Delete, ChoiceRef(StructureId, ChoiceBystanderPiece));

	Controller->ShowPieceMenu(SecondMenu);

	{
		const bool bChose = Controller->ChoosePieceMenuRow(0);

		TestTrue(
			FString::Printf(TEXT("choosing row 0 ('%s' on piece %d) should report that it committed, it reported %d"),
				*SecondMenu[0].Label, ChoiceRecordedPiece, bChose ? 1 : 0),
			bChose);

		TestTrue(
			FString::Printf(TEXT("choosing row 0 must enter row 0's Run exactly once, it has been entered %d time(s)"),
				ChoiceTripwireRunCount),
			ChoiceTripwireRunCount == 1);

		TestEqual(
			FString::Printf(
				TEXT("row 0's action must have been handed row 0's OWN piece %d, it was handed handle %d"),
				ChoiceRecordedPiece, ChoiceTripwireLastHandle),
			ChoiceTripwireLastHandle, ChoiceRecordedPiece);

		TestTrue(
			FString::Printf(TEXT("choosing row 0 must take piece %d out of the graph, IsPieceRemoved reports %d"),
				ChoiceRecordedPiece, Binding->IsPieceRemoved(ChoiceRecordedPiece) ? 1 : 0),
			Binding->IsPieceRemoved(ChoiceRecordedPiece));

		TestTrue(
			FString::Printf(TEXT("choosing row 0 must destroy brick %d's actor, it is %s"),
				ChoiceRecordedPiece, IsValid(Bricks[ChoiceRecordedPiece]) ? TEXT("still valid") : TEXT("gone")),
			!IsValid(Bricks[ChoiceRecordedPiece]));

		// Row 1's brick is untouched.
		CheckPieceUntouched(
			*this, *Binding, Bricks, TEXT("after choosing row 0"), ChoiceBystanderPiece);

		TestEqual(
			FString::Printf(TEXT("two choices must have removed exactly two pieces, so %d should be live, got %d"),
				WallPieceCount - 2, Binding->GetStructure().NumLivePieces()),
			Binding->GetStructure().NumLivePieces(), WallPieceCount - 2);

		CheckChoiceMenu(*this, *Controller, TEXT("after choosing row 0"), TArray<FPieceMenuRow>());

		CheckChoiceControls(
			*this, *Controller, *Input, *DefaultContext, *MouseLookContext,
			TEXT("after choosing row 0"));
	}

	TestWorld.End();

	return true;
}

/**
 * End to end with production code only: a ray at the waist brick opens its menu, choosing the row
 * removes it from the graph and the world, the wall is re-solved, and the controls never move.
 *
 * Pieces 3 and 4 reach the ground only through the waist, so they read Falling only if the choice
 * itself re-solved (nothing else calls SolveLoads). Not covered: the widget and the cursor
 * deprojection, which need a viewport. Needs a world, but no ticking.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuChoiceDeletesTheBrickTest,
	"DestructionGame.World.Choose.ChoosingDeleteTakesTheBrickOutOfTheWall",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuChoiceDeletesTheBrickTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;
	using namespace PieceMenuChoiceTestSupport;

	const FPieceAction* const Delete = FindChoiceAction(TEXT("Delete"));

	if (Delete == nullptr)
	{
		AddError(TEXT("fixture: the action table must contain a row labelled 'Delete'"));
		return true;
	}

	const UInputMappingContext* const DefaultContext = LoadObject<UInputMappingContext>(
		nullptr, DestructionContent::DefaultMappingContextPath);

	const UInputMappingContext* const MouseLookContext = LoadObject<UInputMappingContext>(
		nullptr, DestructionContent::MouseLookMappingContextPath);

	if (DefaultContext == nullptr || MouseLookContext == nullptr)
	{
		AddError(TEXT("fixture: IMC_Default and IMC_MouseLook should both resolve"));
		return true;
	}

	const FRunningBondSpec Spec = NarrowWaistWallSpec(3);

	// Aim from a separately laid reference layout, so a buggy spawner can't agree with itself.
	FBrickLayout Reference;

	TestTrue(TEXT("fixture: RunningBond should lay the reference wall"), RunningBond(Spec, Reference));

	TestEqual(
		FString::Printf(TEXT("fixture: a ragged 3 x 2 wall should be %d pieces, got %d"),
			ChoiceWaistWallPieceCount, Reference.Structure.NumPieces()),
		Reference.Structure.NumPieces(), ChoiceWaistWallPieceCount);

	if (Reference.Boxes.Num() != ChoiceWaistWallPieceCount)
	{
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

	if (Binding == nullptr || Binding->NumPieces() != ChoiceWaistWallPieceCount)
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

	// Positive control: every piece is held up before the choice.
	for (int32 Piece = 0; Piece < ChoiceWaistWallPieceCount; ++Piece)
	{
		const EPieceSupport Support = Binding->GetStructure().GetPieceSupport(Piece);

		TestTrue(
			*FString::Printf(TEXT("fixture: piece %d should be held up before the waist goes, the solver says %s"),
				Piece, ChoiceSupportName(Support)),
			Support == EPieceSupport::Grounded || Support == EPieceSupport::Supported);
	}

	ADestructionGamePlayerController* const Controller =
		SpawnControllerWithLocalPlayer(*this, TestWorld.World);

	UEnhancedInputLocalPlayerSubsystem* const Input = InputSubsystemOf(Controller);

	TestNotNull(
		TEXT("fixture: a controller with a local player should have an Enhanced Input subsystem"),
		Input);

	if (Controller == nullptr || Input == nullptr)
	{
		TestWorld.End();
		return true;
	}

	CheckChoiceControls(
		*this, *Controller, *Input, *DefaultContext, *MouseLookContext,
		TEXT("fixture, before anything is clicked"));

	// 1. The ray opens the waist brick's menu.
	const FPieceBox& WaistBox = Reference.Boxes[ChoiceWaistPiece];

	const TArray<FPieceMenuRow> Rows = Controller->InspectAlongRay(
		FVector(WaistBox.CentreCm.X, WaistBox.CentreCm.Y - ChoiceInspectReachCm, WaistBox.CentreCm.Z),
		FVector(WaistBox.CentreCm.X, WaistBox.CentreCm.Y + ChoiceInspectReachCm, WaistBox.CentreCm.Z));

	const TArray<const FPieceAction*> Allowed =
		PieceActionsFor(*Binding, ChoiceRef(StructureId, ChoiceWaistPiece));

	TestEqual(
		FString::Printf(TEXT("inspecting the waist should offer the %d row(s) the table allows, got %d [%s]"),
			Allowed.Num(), Rows.Num(), *DescribeChoiceRows(Rows)),
		Rows.Num(), Allowed.Num());

	if (Rows.Num() == 0)
	{
		AddError(TEXT("inspecting the waist offered nothing, so the choice below cannot be made"));
		TestWorld.End();
		return true;
	}

	TestTrue(
		*FString::Printf(TEXT("fixture: the row offered should be Delete against piece %d, it reads '%s'->{%d,%d}"),
			ChoiceWaistPiece, *Rows[0].Label, Rows[0].Ref.StructureId, Rows[0].Ref.PieceIndex),
		Rows[0].Action == Delete
			&& Rows[0].Ref.StructureId == StructureId
			&& Rows[0].Ref.PieceIndex == ChoiceWaistPiece);

	CheckChoiceMenu(*this, *Controller, TEXT("after inspecting the waist"), Rows);

	// With the menu up, the controls have not moved.
	CheckChoiceControls(
		*this, *Controller, *Input, *DefaultContext, *MouseLookContext,
		TEXT("with the inspected brick's menu up"));

	// 2. Choosing the row removes the brick from the graph and the world.
	ABrickActor* const WaistBrick = Bricks[ChoiceWaistPiece];

	const bool bChose = Controller->ChoosePieceMenuRow(0);

	TestTrue(
		FString::Printf(TEXT("choosing the inspected brick's only row should report that it committed, it reported %d"),
			bChose ? 1 : 0),
		bChose);

	TestTrue(
		FString::Printf(TEXT("the chosen piece %d must be gone from the graph, IsPieceRemoved reports %d"),
			ChoiceWaistPiece, Binding->IsPieceRemoved(ChoiceWaistPiece) ? 1 : 0),
		Binding->IsPieceRemoved(ChoiceWaistPiece));

	// RunPieceAction is world-free; without the commit destroying its returned actor, a phantom collider stays.
	TestTrue(
		FString::Printf(TEXT("the chosen brick's actor must have been destroyed, it is %s"),
			IsValid(WaistBrick) ? TEXT("still valid") : TEXT("gone")),
		!IsValid(WaistBrick));

	// Only that one.
	for (int32 Piece = 0; Piece < Bricks.Num(); ++Piece)
	{
		if (Piece == ChoiceWaistPiece)
		{
			continue;
		}

		TestTrue(
			FString::Printf(TEXT("brick %d was not chosen and must still be in the world"), Piece),
			IsValid(Bricks[Piece]));
	}

	TestEqual(
		FString::Printf(TEXT("one piece should have left the structure, so %d should be live, got %d"),
			ChoiceWaistWallPieceCount - 1, Binding->GetStructure().NumLivePieces()),
		Binding->GetStructure().NumLivePieces(), ChoiceWaistWallPieceCount - 1);

	// 3. The choice re-solved the wall; nothing else calls SolveLoads.
	for (int32 Piece = 0; Piece < ChoiceWaistWallPieceCount; ++Piece)
	{
		if (Piece == ChoiceWaistPiece)
		{
			continue;
		}

		const EPieceSupport Support = Binding->GetStructure().GetPieceSupport(Piece);

		AddInfo(FString::Printf(TEXT("after the choice, piece %d reads %s"), Piece, ChoiceSupportName(Support)));

		if (bChoiceOrphanedByTheWaist[Piece])
		{
			TestTrue(
				*FString::Printf(
					TEXT("the choice must re-solve: piece %d lost its only path to the ground and should read Falling, got %s"),
					Piece, ChoiceSupportName(Support)),
				Support == EPieceSupport::Falling);
		}
		else
		{
			TestTrue(
				*FString::Printf(TEXT("grounded piece %d must still read Grounded after the re-solve, got %s"),
					Piece, ChoiceSupportName(Support)),
				Support == EPieceSupport::Grounded);
		}
	}

	// 4. The menu is dismissed and the controls are unchanged.
	CheckChoiceMenu(*this, *Controller, TEXT("after choosing the row"), TArray<FPieceMenuRow>());

	CheckChoiceControls(
		*this, *Controller, *Input, *DefaultContext, *MouseLookContext,
		TEXT("after choosing the row"));

	// A second choice with no menu commits nothing.
	TestTrue(
		TEXT("choosing again with the menu gone must report that it did nothing"),
		!Controller->ChoosePieceMenuRow(0));

	TestEqual(
		FString::Printf(TEXT("the second choice must leave %d pieces live, got %d"),
			ChoiceWaistWallPieceCount - 1, Binding->GetStructure().NumLivePieces()),
		Binding->GetStructure().NumLivePieces(), ChoiceWaistWallPieceCount - 1);

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
