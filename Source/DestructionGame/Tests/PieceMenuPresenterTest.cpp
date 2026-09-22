// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceMenu.h"
#include "InputMappingContext.h"
#include "RequiredContent.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/** Uniquely named namespace (not anonymous) to avoid unity-build collisions. */
namespace PieceMenuPresenterTestSupport
{
	/** Rows for a piece from the production BuildPieceMenuRows, so action pointers are real table rows. */
	TArray<FPieceMenuRow> RowsForPiece(int32 StructureId, int32 PieceIndex)
	{
		TArray<const FPieceAction*> Menu;

		for (const FPieceAction& Action : AllPieceActions())
		{
			Menu.Add(&Action);
		}

		FPieceRef Ref;
		Ref.StructureId = StructureId;
		Ref.PieceIndex = PieceIndex;

		return BuildPieceMenuRows(Menu, Ref);
	}

	/** What is on screen, so a failure reads without a debugger. */
	FString DescribeShown(TArrayView<const FPieceMenuRow> Rows)
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

	/**
	 * Assert whether a menu is up and that it shows exactly these rows in order. The count catches
	 * a presenter that appends instead of replacing.
	 */
	void CheckShown(
		FAutomationTestBase& Test,
		const ADestructionGamePlayerController& Controller,
		const TCHAR* After,
		bool bExpectShown,
		const TArray<FPieceMenuRow>& Expected)
	{
		const TArrayView<const FPieceMenuRow> Shown = Controller.GetShownPieceMenuRows();

		Test.TestEqual(
			FString::Printf(TEXT("%s: a menu should%s be shown, it shows [%s]"),
				After, bExpectShown ? TEXT("") : TEXT(" NOT"), *DescribeShown(Shown)),
			Controller.IsPieceMenuShown(), bExpectShown);

		Test.TestEqual(
			FString::Printf(TEXT("%s: should present %d row(s), it presents %d [%s]"),
				After, Expected.Num(), Shown.Num(), *DescribeShown(Shown)),
			Shown.Num(), Expected.Num());

		if (Shown.Num() != Expected.Num())
		{
			return;
		}

		for (int32 Index = 0; Index < Shown.Num(); ++Index)
		{
			Test.TestTrue(
				*FString::Printf(TEXT("%s: row %d should be the very action row it was handed"),
					After, Index),
				Shown[Index].Action == Expected[Index].Action);

			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: row %d should still name {%d,%d}, it names {%d,%d}"),
					After, Index,
					Expected[Index].Ref.StructureId, Expected[Index].Ref.PieceIndex,
					Shown[Index].Ref.StructureId, Shown[Index].Ref.PieceIndex),
				Shown[Index].Ref.StructureId == Expected[Index].Ref.StructureId
					&& Shown[Index].Ref.PieceIndex == Expected[Index].Ref.PieceIndex);
		}
	}

	/**
	 * The cursor is shown and both input contexts stay applied, in every menu state. Look is now
	 * chorded to a held RMB (SESSION_UI_DESIGN §d, S6), so a menu no longer removes anything.
	 */
	void CheckControls(
		FAutomationTestBase& Test,
		const ADestructionGamePlayerController& Controller,
		UEnhancedInputLocalPlayerSubsystem& Input,
		const UInputMappingContext& DefaultContext,
		const UInputMappingContext& MouseLookContext,
		const TCHAR* After)
	{
		// The cursor is raised once in BeginPlay for the session-long toolbar.
		Test.TestTrue(
			*FString::Printf(TEXT("%s: the mouse cursor must be shown — it is up for the whole "
								  "session, not only while a menu is"), After),
			Controller.bShowMouseCursor);

		// Removing IMC_MouseLook would leave the camera dead while a panel is up.
		Test.TestTrue(
			*FString::Printf(TEXT("%s: IMC_MouseLook must stay applied — camera look is gated by the "
								  "RMB chord now, so a menu has nothing to remove"), After),
			Input.HasMappingContext(&MouseLookContext));

		// IMC_Default carries IA_InspectPiece, which is how a click elsewhere dismisses the menu.
		Test.TestTrue(
			*FString::Printf(TEXT("%s: IMC_Default must stay applied whatever the menu is doing"),
				After),
			Input.HasMappingContext(&DefaultContext));
	}
}

/**
 * The presenter holds one menu: showing replaces, showing nothing dismisses, dismissing twice is
 * harmless, and no menu state changes the cursor or input contexts.
 *
 * Covers presenter state only; a code-built world has no viewport, so no Slate widget is checked.
 * A leaked widget from a second AddViewportWidgetContent is invisible here, so ShowPieceMenu should
 * be written as DismissPieceMenu followed by a build, making the model-level check cover it.
 *
 * Look is chorded to a held RMB (SESSION_UI_DESIGN §d, S6) and the cursor is raised once in
 * BeginPlay, so nothing needs restoring when a menu closes by any route.
 *
 * Two controllers: one with no local player (as Tests/PieceInspectTest.cpp spawns) for state, one
 * with a local player for input. Needs a world for the actors but never ticks it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuPresenterStateTest,
	"DestructionGame.World.Menu.PieceMenuPresenterState",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuPresenterStateTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace PieceMenuPresenterTestSupport;

	constexpr int32 StructureId = 0;

	const TArray<FPieceMenuRow> RowsForThree = RowsForPiece(StructureId, 3);
	const TArray<FPieceMenuRow> RowsForFive = RowsForPiece(StructureId, 5);
	const TArray<FPieceMenuRow> NoRows;

	// Precondition: with no rows, the replace-vs-stack checks would be vacuous.
	TestTrue(
		FString::Printf(
			TEXT("fixture: the shipped table should build at least one row per piece, it built %d and %d"),
			RowsForThree.Num(), RowsForFive.Num()),
		RowsForThree.Num() >= 1 && RowsForThree.Num() == RowsForFive.Num());

	if (RowsForThree.Num() < 1)
	{
		return true;
	}

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	// State machine, on a controller with no local player.
	ADestructionGamePlayerController* const Plain =
		TestWorld.World->SpawnActor<ADestructionGamePlayerController>();

	TestNotNull(TEXT("fixture: the test world should be able to spawn the game's player controller"), Plain);

	if (Plain == nullptr)
	{
		TestWorld.End();
		return true;
	}

	CheckShown(*this, *Plain, TEXT("before anything is shown"), false, NoRows);

	TestTrue(
		TEXT("showing a menu with rows should report that one is now up"),
		Plain->ShowPieceMenu(RowsForThree));

	CheckShown(*this, *Plain, TEXT("after showing piece 3's menu"), true, RowsForThree);

	// A second show replaces; appending would leave a stale Delete for the wrong brick.
	TestTrue(
		TEXT("showing a second menu should report that one is up"),
		Plain->ShowPieceMenu(RowsForFive));

	CheckShown(*this, *Plain, TEXT("after showing piece 5's menu over piece 3's"), true, RowsForFive);

	// An empty list dismisses; InspectAlongRay takes this route on every miss.
	TestTrue(
		TEXT("showing an empty menu must report that NO menu is up"),
		!Plain->ShowPieceMenu(NoRows));

	CheckShown(*this, *Plain, TEXT("after showing an empty menu"), false, NoRows);

	// Dismiss is idempotent and reports whether it closed anything.
	Plain->ShowPieceMenu(RowsForThree);

	TestTrue(
		TEXT("dismissing a menu that is up should report that it took one down"),
		Plain->DismissPieceMenu());

	CheckShown(*this, *Plain, TEXT("after dismissing"), false, NoRows);

	TestTrue(
		TEXT("dismissing again should report that there was nothing to take down"),
		!Plain->DismissPieceMenu());

	CheckShown(*this, *Plain, TEXT("after dismissing twice"), false, NoRows);

	// Cursor and input contexts, which need a real local player.
	const UInputMappingContext* const DefaultContext = LoadObject<UInputMappingContext>(
		nullptr, DestructionContent::DefaultMappingContextPath);

	const UInputMappingContext* const MouseLookContext = LoadObject<UInputMappingContext>(
		nullptr, DestructionContent::MouseLookMappingContextPath);

	TestNotNull(TEXT("fixture: IMC_Default should resolve"), DefaultContext);
	TestNotNull(TEXT("fixture: IMC_MouseLook should resolve"), MouseLookContext);

	ADestructionGamePlayerController* const Local =
		SpawnControllerWithLocalPlayer(*this, TestWorld.World);

	if (Local == nullptr || DefaultContext == nullptr || MouseLookContext == nullptr)
	{
		TestWorld.End();
		return true;
	}

	UEnhancedInputLocalPlayerSubsystem* const Input = InputSubsystemOf(Local);

	TestNotNull(
		TEXT("fixture: a controller with a local player should have an Enhanced Input subsystem"),
		Input);

	if (Input == nullptr)
	{
		TestWorld.End();
		return true;
	}

	// Precondition: SetupInputComponent applied both contexts.
	CheckControls(
		*this, *Local, *Input, *DefaultContext, *MouseLookContext,
		TEXT("fixture, before any menu"));

	Local->ShowPieceMenu(RowsForThree);

	CheckControls(
		*this, *Local, *Input, *DefaultContext, *MouseLookContext,
		TEXT("while the menu is up"));

	Local->DismissPieceMenu();

	CheckControls(
		*this, *Local, *Input, *DefaultContext, *MouseLookContext,
		TEXT("after the menu is dismissed"));

	// The empty-list route (every miss) must also leave the controls alone; it was once forgotten.
	Local->ShowPieceMenu(RowsForFive);
	Local->ShowPieceMenu(NoRows);

	CheckControls(
		*this, *Local, *Input, *DefaultContext, *MouseLookContext,
		TEXT("after an empty menu dismissed the last one"));

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
