// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceMenu.h"
#include "InputMappingContext.h"
#include "RequiredContent.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges
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
	 * The whole presented state, asserted in one place: is a menu up, and is it EXACTLY these
	 * rows — same count, same action rows, same refs, same order.
	 *
	 * THE COUNT IS THE ANTI-STACK ASSERTION. A presenter that appends instead of replacing
	 * still looks right on the row that was added last, and on a one-row action table the only
	 * observable difference is that there are two rows instead of one and the first names the
	 * previous brick. So the count and the per-row ref are checked together and neither is
	 * decoration.
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

	/** The cursor and the two contexts, asserted together because they are one decision. */
	void CheckControls(
		FAutomationTestBase& Test,
		const ADestructionGamePlayerController& Controller,
		UEnhancedInputLocalPlayerSubsystem& Input,
		const UInputMappingContext& DefaultContext,
		const UInputMappingContext& MouseLookContext,
		const TCHAR* After,
		bool bExpectCursor,
		bool bExpectLook)
	{
		Test.TestEqual(
			FString::Printf(TEXT("%s: the mouse cursor should%s be shown"),
				After, bExpectCursor ? TEXT("") : TEXT(" NOT")),
			Controller.bShowMouseCursor, bExpectCursor);

		Test.TestEqual(
			FString::Printf(TEXT("%s: IMC_MouseLook should%s be applied"),
				After, bExpectLook ? TEXT("") : TEXT(" NOT")),
			Input.HasMappingContext(&MouseLookContext), bExpectLook);

		/*
		 * IMC_Default STAYS, ALWAYS, AND THIS IS NOT A FREEBIE ROW. It is what carries
		 * IA_InspectPiece, which is how the player DISMISSES the menu by clicking somewhere
		 * else. A presenter that cleared every context to stop the camera spinning would leave
		 * a menu on screen that nothing can close and a player who can no longer look around —
		 * exactly the "dismissed by a route nobody thought of" hazard, reached by the front door.
		 */
		Test.TestTrue(
			*FString::Printf(TEXT("%s: IMC_Default must stay applied whatever the menu is doing"),
				After),
			Input.HasMappingContext(&DefaultContext));
	}
}

/**
 * THE PIECE MENU IS A PRESENTED STATE WITH ONE MENU IN IT: SHOWING REPLACES, SHOWING NOTHING
 * DISMISSES, DISMISSING TWICE IS HARMLESS — AND WHILE IT IS UP THE PLAYER HAS A CURSOR AND THE
 * CAMERA HAS STOPPED FOLLOWING THE MOUSE.
 *
 * WHAT THIS COVERS AND WHAT IT DOES NOT, PLAINLY. It covers the presenter's STATE. It does not
 * assert that a Slate widget appeared on screen, that it has one button per row, or that
 * clicking a button commits — a code-built test world has no UGameViewportClient at all, so
 * AddViewportWidgetContent has nothing to add to and there is nothing to look at. Standing a
 * viewport up would cost a great deal to learn something the rows already say; Core/PieceMenu.h
 * and Tests/PieceMenuTest.cpp own what the rows are, and this owns which of them are up.
 *
 * THE ONE REAL GAP THAT LEAVES: a second AddViewportWidgetContent without a matching remove
 * leaks the previous widget on screen forever, and no headless assertion can see it. What CAN
 * be seen is the model-level version of the same bug — the presenter holding two menus' rows —
 * and the two are only the same code path if ShowPieceMenu is DEFINED as DismissPieceMenu
 * followed by a build. Write it that way; it is the only removal path there is, and it is also
 * what makes the cursor and context restore correct for free.
 *
 * WHY THESE FOUR TRANSITIONS. They are where a menu actually goes wrong, and none of them is a
 * pixel question. Showing an empty box is what happens the day a piece offers no actions.
 * Stacking is what happens when a player clicks brick after brick without closing anything.
 * A non-idempotent dismiss is what happens when the menu is closed by two routes at once.
 * And a menu that does not restore the controls leaves the player unable to look around, which
 * is unrecoverable rather than merely untidy.
 *
 * THE CURSOR DECISION, RECORDED HERE BECAUSE THIS IS WHAT PINS IT. IMC_MouseLook binds the raw
 * Mouse2D AXIS unconditionally with no held button, so the camera follows the mouse all the
 * time and there is no pointer — a menu drawn on top of that is unusable. So while a menu is
 * up: bShowMouseCursor is true and IMC_MouseLook is REMOVED; when it goes down, both are put
 * back. IMC_Default is deliberately NOT removed, because it carries the very action that
 * closes the menu. The alternative — put free-look behind a held RMB and leave the cursor on
 * permanently — was considered and rejected for this slice: it changes how the entire game
 * controls, and it is a bigger decision than the menu that provoked it.
 *
 * TWO CONTROLLERS, AND THE PLAIN ONE IS NOT AN OVERSIGHT. A controller with no ULocalPlayer
 * has no Enhanced Input subsystem to remove a context from, and Tests/PieceInspectTest.cpp
 * spawns exactly that. So the state half must work — not merely not crash — with no local
 * player at all, and it is asserted on a controller that has none.
 *
 * NEEDS A TICKING WORLD: it needs a WORLD, because a player controller is an actor and only a
 * real ULocalPlayer makes the engine run SetupInputComponent. It never ticks one, lays no
 * wall and touches no physics.
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

	/*
	 * FIXTURE PRECONDITION. Every assertion below about replacing rather than stacking reads
	 * the row COUNT, so a producer that handed back nothing would make all of them vacuous.
	 */
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

	/*
	 * ONE: THE STATE MACHINE, ON A CONTROLLER WITH NO LOCAL PLAYER.
	 */
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

	/*
	 * A SECOND SHOW REPLACES. If it appended, the count doubles and row 0 still names piece 3
	 * — which on screen is two menus, and a Delete on the stale one removes a brick the player
	 * is no longer pointing at.
	 */
	TestTrue(
		TEXT("showing a second menu should report that one is up"),
		Plain->ShowPieceMenu(RowsForFive));

	CheckShown(*this, *Plain, TEXT("after showing piece 5's menu over piece 3's"), true, RowsForFive);

	/*
	 * AN EMPTY LIST DISMISSES RATHER THAN SHOWING AN EMPTY BOX. This is the route
	 * InspectAlongRay takes on every miss, so it is the common case rather than a corner one.
	 */
	TestTrue(
		TEXT("showing an empty menu must report that NO menu is up"),
		!Plain->ShowPieceMenu(NoRows));

	CheckShown(*this, *Plain, TEXT("after showing an empty menu"), false, NoRows);

	/* DISMISS IS IDEMPOTENT, and it answers whether it actually took anything down. */
	Plain->ShowPieceMenu(RowsForThree);

	TestTrue(
		TEXT("dismissing a menu that is up should report that it took one down"),
		Plain->DismissPieceMenu());

	CheckShown(*this, *Plain, TEXT("after dismissing"), false, NoRows);

	TestTrue(
		TEXT("dismissing again should report that there was nothing to take down"),
		!Plain->DismissPieceMenu());

	CheckShown(*this, *Plain, TEXT("after dismissing twice"), false, NoRows);

	/*
	 * TWO: THE CURSOR AND THE LOOK CONTEXT, WHICH NEED A REAL LOCAL PLAYER.
	 */
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

	/*
	 * FIXTURE PRECONDITION: SetupInputComponent really did apply both contexts, and the cursor
	 * really does start hidden. Without this, "IMC_MouseLook is gone" would pass on a
	 * controller that never applied it, which is the vacuous direction.
	 */
	CheckControls(
		*this, *Local, *Input, *DefaultContext, *MouseLookContext,
		TEXT("fixture, before any menu"), false, true);

	Local->ShowPieceMenu(RowsForThree);

	CheckControls(
		*this, *Local, *Input, *DefaultContext, *MouseLookContext,
		TEXT("while the menu is up"), true, false);

	Local->DismissPieceMenu();

	CheckControls(
		*this, *Local, *Input, *DefaultContext, *MouseLookContext,
		TEXT("after the menu is dismissed"), false, true);

	/*
	 * AND THE EMPTY-LIST ROUTE RESTORES THEM TOO. It is a second way out of "a menu is up",
	 * it is the one every miss takes, and it is the one that gets forgotten — a player who
	 * clicks a brick and then clicks the sky would be left with a cursor and no camera.
	 */
	Local->ShowPieceMenu(RowsForFive);
	Local->ShowPieceMenu(NoRows);

	CheckControls(
		*this, *Local, *Input, *DefaultContext, *MouseLookContext,
		TEXT("after an empty menu dismissed the last one"), false, true);

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
