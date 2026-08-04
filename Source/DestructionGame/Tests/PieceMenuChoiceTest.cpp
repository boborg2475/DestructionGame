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
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges
 * many files into one. See CURRENT_STATE.md; the `using namespace` lives inside each RunTest
 * body for the same reason. The world harness is NOT redeclared here: it lives in
 * Tests/BrickWorldTestSupport.h and every World.* test shares it.
 */
namespace PieceMenuChoiceTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/**
	 * A SECOND ACTION THAT RECORDS WHICH HANDLE IT WAS RUN AGAINST, and it is the only thing
	 * that can tell "row N's action" from "the menu's action".
	 *
	 * The shipped table has exactly one row today, so a menu built from it twice over is two
	 * rows carrying the SAME action pointer — and a presenter that committed row 0's action
	 * whatever was clicked would be indistinguishable from a correct one. This row is a
	 * different pointer, so choosing the other row must leave its counter at zero; and it
	 * records Context.PieceHandle, so committing the right action against the wrong brick
	 * reads back as a handle that is not the one the chosen row named.
	 *
	 * IT REALLY REMOVES THE PIECE rather than merely counting, and that is deliberate.
	 * RunPieceAction hands back the actor it captured whenever Run reports success, and
	 * CommitPieceAction destroys whatever comes back — so a tripwire that reported success
	 * without removing anything would have a brick still standing in the wall torn out of the
	 * world. (CURRENT_STATE.md records that hand-back rule as undecided for a non-destroying
	 * action; this file deliberately does not take a position on it.)
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

	/** The Delete row, looked up by label so nothing hard-codes a position in the table. */
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
	 * A TWO-ROW MENU WHOSE ROWS NAME DIFFERENT BRICKS, built by the PRODUCTION producer.
	 *
	 * BuildPieceMenuRows takes one ref for the whole menu, which is right — a menu is for a
	 * piece — so two rows naming two bricks is two calls concatenated. That shape is exactly
	 * what makes the requirement falsifiable: with one row, "commit the chosen row's ref" and
	 * "commit the ref we remembered when the menu opened" produce identical output forever.
	 *
	 * The rows still come from the producer rather than being assembled field by field here,
	 * so the label/action/ref wiring under test is production's and not this file's.
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

	/** What is on screen, so a failure reads without a debugger. */
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

	/**
	 * The presented state, asserted in one place: is a menu up, and is it EXACTLY these rows.
	 *
	 * Used for both directions. After a REFUSED choice it is what says nothing changed — a
	 * refusal that also took the menu down is a menu that vanishes when a player clicks past
	 * the end of it. After a committed choice it is what says the menu came down.
	 */
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
	 * The cursor and the look context, which is the half a player cannot recover from alone.
	 *
	 * Tests/PieceMenuPresenterTest.cpp owns the controls CONTRACT; this checks it across the
	 * one route that file does not have — choosing a row — because a choice that committed
	 * perfectly and forgot to restore leaves a player deleting bricks with no camera.
	 */
	void CheckChoiceControls(
		FAutomationTestBase& Test,
		const ADestructionGamePlayerController& Controller,
		UEnhancedInputLocalPlayerSubsystem& Input,
		const UInputMappingContext& DefaultContext,
		const UInputMappingContext& MouseLookContext,
		const TCHAR* Where,
		bool bExpectCursor,
		bool bExpectLook)
	{
		Test.TestEqual(
			FString::Printf(TEXT("%s: the mouse cursor should%s be shown"),
				Where, bExpectCursor ? TEXT("") : TEXT(" NOT")),
			Controller.bShowMouseCursor, bExpectCursor);

		Test.TestEqual(
			FString::Printf(TEXT("%s: IMC_MouseLook should%s be applied"),
				Where, bExpectLook ? TEXT("") : TEXT(" NOT")),
			Input.HasMappingContext(&MouseLookContext), bExpectLook);

		/* IMC_Default carries IA_InspectPiece, which is how a menu is closed by clicking away. */
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
	 * THE THREE BRICKS OF THE SHARED FLUSH 2 x 3 WALL THIS TEST NAMES.
	 *
	 * They only have to be distinct and live: nothing here calls ApplyResults, so no removal
	 * releases anything and no CanRun answer changes for a piece that stays in the graph.
	 * Which of them the bond made a half bat is irrelevant to every assertion below.
	 */
	constexpr int32 ChoiceRecordedPiece = 6;
	constexpr int32 ChoiceDeletedPiece = 3;
	constexpr int32 ChoiceBystanderPiece = 4;

	/*
	 * THE WALL THIS TEST USES IS BrickWorldTestSupport::NarrowWaistWallSpec(3), the smallest
	 * one with a waist, and the waist is what makes the re-solve visible at all: the only thing
	 * that can SEE a re-solve from outside is a piece whose support answer changes, and in the
	 * shared flush 2 x 3 wall nothing's answer changes when one brick goes.
	 *
	 *      course 2         [ 3 ][ 4 ]      head joint between them
	 *      course 1            [ 2 ]        THE WAIST — inspected and chosen
	 *      course 0         [ 0 ][ 1 ]      grounded
	 *
	 * This file used to carry its own copy of that spec, with a note saying to promote it the
	 * day a third file needed a waist. Four now do, so it lives in the shared harness.
	 */

	constexpr int32 ChoiceWaistWallPieceCount = 5;

	/** The single course-1 brick everything above the bottom course hangs from. */
	constexpr int32 ChoiceWaistPiece = 2;

	/** Exactly the pieces that lose their path to the ground when the waist goes. */
	constexpr bool bChoiceOrphanedByTheWaist[ChoiceWaistWallPieceCount] =
	{
		false, false, false, true, true
	};

	/**
	 * How far along Y a ray starts and ends, either side of the wall.
	 *
	 * A brick is 10.25 cm deep and a wall is centred on Y = 0, so +/- 100 cm is far outside it
	 * on both sides and the ray crosses the whole thickness. Along Y rather than X or Z so
	 * nothing else in the wall is in the way and the answer is unambiguous.
	 */
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
 * CHOOSING ROW N COMMITS ROW N'S OWN ACTION AGAINST ROW N'S OWN REF, TAKES THE MENU DOWN AND
 * GIVES THE CONTROLS BACK — AND AN INDEX THAT NAMES NO ROW COMMITS NOTHING AND CHANGES NOTHING.
 *
 * THIS IS THE LAST ASSERTABLE STEP OF THE MVP LOOP. Everything before it exists: a click
 * reaches the chain, the chain builds the right brick's rows, and the controller presents and
 * holds them. What is missing is any route from a presented row to CommitPieceAction, so a
 * player who clicks a brick gets a cursor, loses free-look, and can do nothing with either.
 * The button that will call this is the untested inch — a code-built test world has no
 * UGameViewportClient at all, so AddViewportWidgetContent has nothing to add to — and that
 * inch is exactly one call wide, which is the smallest it can be made.
 *
 * TWO ROWS NAMING DIFFERENT BRICKS, WHICH IS WHAT MAKES THE REQUIREMENT FALSIFIABLE AT ALL.
 * FPieceMenuRow carries its ref precisely so a presenter cannot commit the right action
 * against the wrong brick (Core/PieceMenu.h says so), but with a ONE-row menu "commit the
 * chosen row's ref" and "commit the ref we remembered when the menu opened" are the same
 * output forever. Two rows separate them. And because the shipped table has one row today,
 * both rows would carry the SAME action pointer — so row 0 is a recording tripwire, a
 * different pointer with a counter and a handle, which is what separates "row N's action"
 * from "the menu's action" and, when it does run, says which brick it was handed.
 *
 * THE REFUSAL ROWS USE A COUNTER RATHER THAN AN ABSENCE, for the reason
 * Core.PieceActions.CommitRefFailsClosed already records: "refused before running" and "ran,
 * then reported false" are different bugs that look identical from outside, and only an entry
 * count into Run can tell them apart. A clamping implementation — FMath::Clamp on the index,
 * which is the obvious wrong fix — commits row 0 for every out-of-range choice, and the
 * tripwire sees that immediately.
 *
 * AND A REFUSAL CHANGES NOTHING, including the menu. A refused index that also dismissed would
 * make the menu vanish when a player clicks a millimetre past the last entry, with the cursor
 * and free-look flipping back at the same time; nothing about that reads as a bug.
 *
 * WHAT IS DELIBERATELY NOT PINNED: what a choice should do when the row's own commit REFUSES —
 * a row naming a brick a cascade removed between the menu opening and the click. Both readings
 * (dismiss anyway, or leave it up) are defensible and nothing depends on it yet, so no row
 * here exercises it and the return value is asserted only where the commit genuinely runs.
 *
 * NEEDS A TICKING WORLD: it needs a WORLD — bricks to spawn, a subsystem to hold them, and a
 * real ULocalPlayer so the contexts are actually applied — but it never ticks one. Nothing
 * here is about anything falling.
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

	/*
	 * ROW 0 IS THE RECORDING TRIPWIRE ON ONE BRICK; ROW 1 IS THE SHIPPED Delete ON ANOTHER.
	 */
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

	/*
	 * FIXTURE PRECONDITIONS. Every "nothing was committed" row below reads a piece that is
	 * live now, and the two rows must genuinely carry different action pointers, or the whole
	 * "row N's action" claim is vacuous.
	 */
	TestTrue(
		TEXT("fixture: the two rows must carry DIFFERENT action pointers, or nothing here discriminates"),
		Menu[0].Action != Menu[1].Action);

	for (const int32 Piece : { ChoiceRecordedPiece, ChoiceDeletedPiece, ChoiceBystanderPiece })
	{
		CheckPieceUntouched(*this, *Binding, Bricks, TEXT("fixture"), Piece);
	}

	/*
	 * ONE: AN INDEX THAT NAMES NO ROW COMMITS NOTHING AND CHANGES NOTHING.
	 */
	Controller->ShowPieceMenu(Menu);

	CheckChoiceMenu(*this, *Controller, TEXT("fixture, with the two-row menu up"), Menu);

	CheckChoiceControls(
		*this, *Controller, *Input, *DefaultContext, *MouseLookContext,
		TEXT("fixture, with the two-row menu up"), true, false);

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

		/*
		 * THE COUNTER IS THE MECHANISM ASSERTION. Nothing-was-deleted is also what a commit
		 * that ran and then reported false produces, and FMath::Clamp on the index — the
		 * obvious wrong fix — commits row 0 for every one of these.
		 */
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

		/* AND THE MENU IS STILL UP, unchanged, with the controls still handed to it. */
		CheckChoiceMenu(*this, *Controller, Case.Description, Menu);

		CheckChoiceControls(
			*this, *Controller, *Input, *DefaultContext, *MouseLookContext,
			Case.Description, true, false);
	}

	/*
	 * TWO: CHOOSING WHEN NO MENU IS UP IS THE SAME REFUSAL. Row 0 of nothing is still no row,
	 * and a keybind or a stale widget can ask for it at any time.
	 */
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
	 * THREE: CHOOSING ROW 1 COMMITS ROW 1 — ROW 1'S ACTION, AGAINST ROW 1'S OWN BRICK.
	 *
	 * The tripwire's counter is what says row 0's action did not run; row 0's brick still
	 * standing is what says row 0's ref was not the one committed against. A presenter that
	 * remembered "the ref this menu is for" and committed the chosen row's ACTION would pass
	 * the second and fail the first, and vice versa, which is why both are asserted.
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

		/* AND IT DISMISSES, by the one route out of "a menu is up" that restores the controls. */
		CheckChoiceMenu(*this, *Controller, TEXT("after choosing row 1"), TArray<FPieceMenuRow>());

		CheckChoiceControls(
			*this, *Controller, *Input, *DefaultContext, *MouseLookContext,
			TEXT("after choosing row 1"), false, true);
	}

	/*
	 * FOUR: AND THE OTHER WAY ROUND — CHOOSING ROW 0 RUNS ROW 0'S ACTION AGAINST ROW 0'S BRICK
	 * AND LEAVES ROW 1'S ALONE. The recorded handle is what makes this more than a count: a
	 * commit of the right action against the remembered ref reads back as the wrong handle.
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

		/* ROW 1'S BRICK IS UNTOUCHED — the same claim as row 1's test, from the other end. */
		CheckPieceUntouched(
			*this, *Binding, Bricks, TEXT("after choosing row 0"), ChoiceBystanderPiece);

		TestEqual(
			FString::Printf(TEXT("two choices must have removed exactly two pieces, so %d should be live, got %d"),
				WallPieceCount - 2, Binding->GetStructure().NumLivePieces()),
			Binding->GetStructure().NumLivePieces(), WallPieceCount - 2);

		CheckChoiceMenu(*this, *Controller, TEXT("after choosing row 0"), TArray<FPieceMenuRow>());

		CheckChoiceControls(
			*this, *Controller, *Input, *DefaultContext, *MouseLookContext,
			TEXT("after choosing row 0"), false, true);
	}

	TestWorld.End();

	return true;
}

/**
 * THE WHOLE LOOP, END TO END: A RAY AT A BRICK PUTS UP THAT BRICK'S MENU, CHOOSING ITS ONE ROW
 * TAKES THE BRICK OUT OF THE WALL, DESTROYS ITS ACTOR, RE-SOLVES WHAT IS LEFT, AND GIVES THE
 * PLAYER BACK THE CAMERA.
 *
 * WHY THIS EXISTS BESIDE THE ROW TEST ABOVE. That one drives the presenter directly with a
 * hand-built menu, which is what makes the row-versus-ref property falsifiable; this one
 * touches nothing by hand at all. Every step is the production one — TracePiece resolves the
 * ray, PieceActionsFor filters the table, BuildPieceMenuRows makes the rows, ShowPieceMenu
 * presents them, and the only input to the choice is an index. If any link is wired to the
 * wrong brick the wall loses a brick nobody chose, and that is a defect no single-link test
 * can see.
 *
 * THE WAIST IS WHAT MAKES THE RE-SOLVE VISIBLE, and nothing between the choice and the
 * assertion calls SolveLoads. Pieces 3 and 4 reach the ground only through piece 2, so a wall
 * nobody re-solved still reports them held up by a brick that is not there — the "the brick
 * vanishes and the wall stands there" failure, seen from the far end of the chain.
 *
 * AND THE CONTROLS ARE ASSERTED ON BOTH SIDES OF THE CHOICE, because the state this closes is
 * currently worse than nothing: today a click on a brick takes free-look away and puts up a
 * cursor with NO MENU behind it, recoverable only by clicking somewhere that misses. The
 * cursor-on/look-off row is that state, asserted deliberately rather than incidentally; the
 * row after the choice is the recovery that makes it stop mattering.
 *
 * WHAT IT STILL CANNOT REACH: the widget. A code-built test world has no UGameViewportClient,
 * so nothing here can assert a button appeared or that clicking it calls anything — and
 * DeprojectMousePositionToWorld needs a viewport too, which is why the ray is supplied rather
 * than derived. Those two are the whole remaining untested surface of the loop.
 *
 * NEEDS A TICKING WORLD: it needs a WORLD — a physics scene for the trace, actors to destroy,
 * a real ULocalPlayer for the contexts — but it never ticks one. Nothing here is about falling.
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

	/*
	 * THE REFERENCE LAYOUT IS LAID SEPARATELY, so the point inspected at comes from the
	 * producer rather than from whatever the subsystem happened to spawn. A spawner that put
	 * every brick at the origin would otherwise be inspected at the origin and agree with itself.
	 */
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

	/*
	 * THE POSITIVE CONTROL FOR THE RE-SOLVE. Pieces 3 and 4 have to read held up NOW for
	 * reading Falling afterwards to mean the wall was solved again rather than never solved.
	 */
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
		TEXT("fixture, before anything is clicked"), false, true);

	/*
	 * ONE: THE RAY PUTS UP THE WAIST BRICK'S MENU AND TAKES THE CONTROLS.
	 */
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

	/*
	 * THIS IS THE STATE THAT IS CURRENTLY WORSE THAN NOTHING, asserted deliberately: the
	 * cursor is up and free-look is gone, and until a row can be chosen there is nothing
	 * behind either of them.
	 */
	CheckChoiceControls(
		*this, *Controller, *Input, *DefaultContext, *MouseLookContext,
		TEXT("with the inspected brick's menu up"), true, false);

	/*
	 * TWO: CHOOSING THE ROW TAKES THE BRICK OUT OF THE WALL AND THE WORLD.
	 */
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

	/*
	 * THE ACTOR LEFT THE WORLD. RunPieceAction is world-free and hands the orphan back;
	 * without a caller that consumes it the deleted brick's mesh stays standing in the hole it
	 * was deleted from — a collider nothing in the model knows about.
	 */
	TestTrue(
		FString::Printf(TEXT("the chosen brick's actor must have been destroyed, it is %s"),
			IsValid(WaistBrick) ? TEXT("still valid") : TEXT("gone")),
		!IsValid(WaistBrick));

	/* AND ONLY THAT ONE. A choice that tore the wall down satisfies the row above perfectly. */
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

	/*
	 * THREE: THE WALL WAS RE-SOLVED BEHIND THE CHOICE, and nothing since the choice called
	 * SolveLoads.
	 */
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

	/*
	 * FOUR: AND THE PLAYER HAS THE CAMERA BACK. This is the whole reason the loop closing
	 * matters: a menu that commits and stays up leaves a cursor over a wall that cannot be
	 * looked at, which no click anywhere can undo.
	 */
	CheckChoiceMenu(*this, *Controller, TEXT("after choosing the row"), TArray<FPieceMenuRow>());

	CheckChoiceControls(
		*this, *Controller, *Input, *DefaultContext, *MouseLookContext,
		TEXT("after choosing the row"), false, true);

	/* And a second choice with nothing up commits nothing more. */
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
