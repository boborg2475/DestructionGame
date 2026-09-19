// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceMenu.h"
#include "InputMappingContext.h"
#include "RequiredContent.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Named namespace, named differently from every other one in this module — an anonymous
 * namespace is private to a translation unit rather than to a file, and a unity build merges
 * many files into one. See CURRENT_STATE.md; the `using namespace` lives inside RunTest for
 * the same reason.
 */
namespace PieceMenuPresenterTestSupport
{
	/**
	 * Rows for a piece, built by the PRODUCTION producer rather than assembled by hand.
	 *
	 * BuildPieceMenuRows is what the presenter will actually be handed, and it is what keeps
	 * the action pointers identical to rows of AllPieceActions() — which is the whole basis of
	 * "which entry did they choose" being a pointer comparison. A hand-built FPieceMenuRow
	 * would let this test agree with itself about identity it never established.
	 */
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
	 * The whole presented state, asserted in one place: is a menu up, and is it exactly these
	 * rows — same count, same action rows, same refs, same order.
	 *
	 * The count is the anti-stack assertion: a presenter that appends instead of replacing still
	 * looks right on the row added last, and on a one-row action table the only observable
	 * difference is two rows instead of one, the first naming the previous brick. So the count and
	 * the per-row ref are checked together.
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
	 * The cursor and the two contexts, asserted together because they are one decision with the
	 * same answer in every state — which is why there are no parameters left.
	 *
	 * This used to take bExpectCursor and bExpectLook, and the menu flipped both. SESSION_UI_DESIGN
	 * §d (S6) replaces that scheme: IMC_MouseLook used to bind the raw Mouse2D axis with no held
	 * button, so a cursor drawn over a camera that follows every mouse movement was unusable, and
	 * taking the context away was the cheapest fix. What changed is not the hazard but what closes
	 * it — IA_MouseLook's mapping now carries a Chorded Action trigger on IA_LookModifier (the
	 * right mouse button), so the camera only turns while RMB is held. There is nothing left for a
	 * menu to take away, so the apply/restore pair that could fall out of step stops existing.
	 *
	 * The function's job is inverted rather than deleted: it used to say "the menu flips the
	 * controls", it now says nothing flips them — the cursor is up for the whole session and both
	 * contexts stay applied through every route a menu comes and goes by. That is strictly
	 * stronger than the old restore: a restore can be forgotten by a new route; nothing to restore
	 * cannot be.
	 */
	void CheckControls(
		FAutomationTestBase& Test,
		const ADestructionGamePlayerController& Controller,
		UEnhancedInputLocalPlayerSubsystem& Input,
		const UInputMappingContext& DefaultContext,
		const UInputMappingContext& MouseLookContext,
		const TCHAR* After)
	{
		/*
		 * The cursor is the session's, not the menu's. SetSessionControls raises it once in
		 * BeginPlay because the toolbar is on screen for the whole session — a strip a player can
		 * only click by first opening a piece menu is not a toolbar.
		 */
		Test.TestTrue(
			*FString::Printf(TEXT("%s: the mouse cursor must be shown — it is up for the whole "
								  "session, not only while a menu is"), After),
			Controller.bShowMouseCursor);

		/*
		 * IMC_MouseLook stays applied. With look chorded to a held RMB there is nothing for a menu
		 * to take away; taking it away anyway would be the old bug's mirror image — the camera dead
		 * for as long as a panel is up, restored only by a route somebody remembered.
		 */
		Test.TestTrue(
			*FString::Printf(TEXT("%s: IMC_MouseLook must stay applied — camera look is gated by the "
								  "RMB chord now, so a menu has nothing to remove"), After),
			Input.HasMappingContext(&MouseLookContext));

		/*
		 * IMC_Default stays too, and it is not a freebie row: it carries IA_InspectPiece, how the
		 * player dismisses the menu by clicking elsewhere. A presenter that cleared every context
		 * to stop the camera spinning would leave a menu nothing can close and a player who can no
		 * longer look around.
		 */
		Test.TestTrue(
			*FString::Printf(TEXT("%s: IMC_Default must stay applied whatever the menu is doing"),
				After),
			Input.HasMappingContext(&DefaultContext));
	}
}

/**
 * The piece menu is a presented state with one menu in it: showing replaces, showing nothing
 * dismisses, dismissing twice is harmless — and it takes nothing away from the player's controls,
 * because the cursor and the camera are the session's rather than the menu's.
 *
 * WHAT THIS COVERS. The presenter's state, not that a Slate widget appeared on screen, has one
 * button per row, or that clicking a button commits — a code-built test world has no
 * UGameViewportClient, so AddViewportWidgetContent has nothing to add to. Core/PieceMenu.h and
 * Tests/PieceMenuTest.cpp own what the rows are; this owns which of them are up.
 *
 * THE ONE REAL GAP THAT LEAVES: a second AddViewportWidgetContent without a matching remove leaks
 * the previous widget forever, invisible to a headless assertion. What can be seen is the
 * model-level version of the same bug — the presenter holding two menus' rows — and the two are
 * only the same code path if ShowPieceMenu is defined as DismissPieceMenu followed by a build.
 * Write it that way; it is the only removal path, and it also makes the cursor/context restore
 * correct for free.
 *
 * WHY THESE FOUR TRANSITIONS. They are where a menu actually goes wrong: showing an empty box
 * (a piece with no actions), stacking (clicking brick after brick without closing anything), a
 * non-idempotent dismiss (closed by two routes at once), and a menu that moves the controls
 * around, leaving the player unable to look — unrecoverable rather than merely untidy.
 *
 * THE CURSOR DECISION, REVERSED HONESTLY RATHER THAN QUIETLY. IMC_MouseLook used to bind the raw
 * Mouse2D axis with no held button, so the camera followed the mouse and there was no pointer; a
 * menu drawn over that is unusable, and removing the whole context while a menu was up was the
 * cheapest fix. The toolbar has since made the alternative necessary (SESSION_UI_DESIGN §d, S6):
 * a strip on screen for the whole session cannot be clickable only while a piece menu is up.
 *
 * So the claim is inverted rather than dropped. IA_MouseLook's mapping now carries a Chorded
 * Action trigger on IA_LookModifier (the right mouse button), the cursor is raised once in
 * BeginPlay by SetSessionControls, and nothing removes a context: while a menu is up, after it is
 * dismissed, and after the empty-list route every miss takes, bShowMouseCursor is true and both
 * contexts stay applied. The hazard the removal guarded — a cursor over a camera nobody can stop
 * spinning — is closed by the chord, at the asset, for every panel this game will draw.
 *
 * That is strictly stronger than the old restore: a restore is something a new route can forget
 * (`World.Choose.ChoosingARowCommitsThatRow` had to be told about it separately). With nothing to
 * restore, a route that forgets has nothing to forget.
 *
 * TWO CONTROLLERS, AND THE PLAIN ONE IS NOT AN OVERSIGHT. A controller with no ULocalPlayer has no
 * Enhanced Input subsystem to remove a context from, and Tests/PieceInspectTest.cpp spawns exactly
 * that, so the state half is asserted on a controller with no local player too.
 *
 * NEEDS A TICKING WORLD: it needs a world, because a player controller is an actor and only a real
 * ULocalPlayer makes the engine run SetupInputComponent. It never ticks one, lays no wall and
 * touches no physics.
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

	/* Fixture precondition: every assertion below about replacing rather than stacking reads the
	 * row count, so a producer that handed back nothing would make all of them vacuous. */
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

	/* One: the state machine, on a controller with no local player. */
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

	/* A second show replaces. If it appended, the count doubles and row 0 still names piece 3 —
	 * on screen that is two menus, and a Delete on the stale one removes the wrong brick. */
	TestTrue(
		TEXT("showing a second menu should report that one is up"),
		Plain->ShowPieceMenu(RowsForFive));

	CheckShown(*this, *Plain, TEXT("after showing piece 5's menu over piece 3's"), true, RowsForFive);

	/* An empty list dismisses rather than showing an empty box — the route InspectAlongRay takes
	 * on every miss, so it is the common case rather than a corner one. */
	TestTrue(
		TEXT("showing an empty menu must report that NO menu is up"),
		!Plain->ShowPieceMenu(NoRows));

	CheckShown(*this, *Plain, TEXT("after showing an empty menu"), false, NoRows);

	/* Dismiss is idempotent, and answers whether it actually took anything down. */
	Plain->ShowPieceMenu(RowsForThree);

	TestTrue(
		TEXT("dismissing a menu that is up should report that it took one down"),
		Plain->DismissPieceMenu());

	CheckShown(*this, *Plain, TEXT("after dismissing"), false, NoRows);

	TestTrue(
		TEXT("dismissing again should report that there was nothing to take down"),
		!Plain->DismissPieceMenu());

	CheckShown(*this, *Plain, TEXT("after dismissing twice"), false, NoRows);

	/* Two: the cursor and the look context, which need a real local player. */
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

	/* Fixture precondition: SetupInputComponent really did apply both contexts. Without this,
	 * "IMC_MouseLook is gone" would pass on a controller that never applied it. */
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

	/*
	 * The empty-list route leaves them alone too. It is a second way out of "a menu is up", the
	 * one every miss takes, and the one that used to get forgotten — a player who clicked a brick
	 * and then clicked the sky was left with a cursor and no camera. Kept here because a presenter
	 * that reached for SetPieceMenuControls out of habit would take the look context away on
	 * exactly this path.
	 */
	Local->ShowPieceMenu(RowsForFive);
	Local->ShowPieceMenu(NoRows);

	CheckControls(
		*this, *Local, *Input, *DefaultContext, *MouseLookContext,
		TEXT("after an empty menu dismissed the last one"));

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
