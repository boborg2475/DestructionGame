// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceMenu.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges
 * many files into one. See CURRENT_STATE.md; the `using namespace` lives inside RunTest for
 * the same reason.
 */
namespace PieceMenuTestSupport
{
	/**
	 * A TRIPWIRE ROW THAT MUST NEVER BE CALLED, and the counters are what say so.
	 *
	 * BuildPieceMenuRows takes a menu that PieceActionsFor has ALREADY filtered, so it has no
	 * business asking CanRun again — a second, quieter copy of the policy is exactly what the
	 * action table exists not to have. A presenter that re-filtered would look identical from
	 * outside on today's one-row table, because Delete's CanRun happens to say yes for a live
	 * piece; only a row whose CanRun says NO, offered anyway, can tell the two apart.
	 *
	 * Run is counted for the same reason in the other direction: building a menu must not run
	 * anything, and "it showed the right labels" would not notice if it had.
	 */
	int32 MenuCanRunEntries = 0;
	int32 MenuRunEntries = 0;

	bool PieceMenuTripwireCanRun(const FStructureBinding& Binding, int32 PieceHandle)
	{
		++MenuCanRunEntries;
		return false;
	}

	bool PieceMenuTripwireRun(FPieceActionContext& Context)
	{
		++MenuRunEntries;
		return false;
	}

	const FPieceAction MenuTripwire{
		TEXT("Tripwire"), &PieceMenuTripwireCanRun, &PieceMenuTripwireRun };

	const FPieceAction MenuSecondRow{
		TEXT("Second"), &PieceMenuTripwireCanRun, &PieceMenuTripwireRun };

	/** A row with no label at all — malformed, and must be skipped rather than shown blank. */
	const FPieceAction MenuUnlabelledRow{ nullptr, &PieceMenuTripwireCanRun, &PieceMenuTripwireRun };

	/** Every row of the shipped table, as pointers — exactly what PieceActionsFor hands back. */
	TArray<const FPieceAction*> ShippedMenu()
	{
		TArray<const FPieceAction*> Menu;

		for (const FPieceAction& Action : AllPieceActions())
		{
			Menu.Add(&Action);
		}

		return Menu;
	}

	FPieceRef MakeRef(int32 StructureId, int32 PieceIndex)
	{
		FPieceRef Ref;
		Ref.StructureId = StructureId;
		Ref.PieceIndex = PieceIndex;
		return Ref;
	}

	/** What came back, so a failure reads without a debugger. */
	FString DescribeRows(const TArray<FPieceMenuRow>& Rows)
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

	/** One row of the table below: a menu, a ref, and the labels that must come back in order. */
	struct FMenuCase
	{
		const TCHAR* Description;
		TArray<const FPieceAction*> Menu;
		FPieceRef Ref;
		TArray<const FPieceAction*> ExpectedRows;
	};
}

/**
 * THE PRESENTER TURNS A MENU AND A REF INTO ROWS: ONE ROW PER ACTION, IN THE TABLE'S ORDER,
 * EACH CARRYING THE ACTION'S OWN LABEL, THE ACTION ITSELF BY POINTER, AND THE REF IT COMMITS
 * AGAINST — AND NOTHING AT ALL FOR A REF THAT NAMES NOTHING.
 *
 * WHY THIS IS THE TESTABLE HALF OF A PRESENTER. What a player sees is a stack of Slate
 * buttons, and asserting that a widget appeared on screen is high cost for very little: it
 * needs a viewport, it fails for reasons that have nothing to do with the menu, and it cannot
 * see the failure that actually matters. The failure that matters is which ROWS were built —
 * an entry the piece may not have, an entry missing, entries in a shuffled order, or a row
 * carrying somebody else's ref so that clicking Delete deletes the wrong brick. All four are
 * a pure function of the list and the ref, and none of them needs a world. So this test owns
 * that function, and the Slate half is deliberately untested; the report says so out loud
 * rather than skipping it quietly.
 *
 * A PARAMETERISED TABLE, NOT A TEST PER CASE, so a second action is a row here as it is a row
 * there. The expectation is written as the POINTERS that must come back, in order, which
 * checks identity and ordering in one comparison — and pointer identity is the whole promise
 * PieceActionsFor makes ("never copies"), so a presenter that copied a row would break the
 * "which entry did they choose" comparison and this is where that shows up.
 *
 * FAIL-CLOSED HAS A DIFFERENT POLARITY HERE THAN IT DOES ONE LAYER DOWN, and CURRENT_STATE.md
 * names the trap. A row is a COMMAND waiting to be chosen, not a readout: a click that hit
 * the floor arrives as a default FPieceRef, and a default is also what "we have no answer"
 * looks like. A readout may take that at face value; a button may not. So a ref missing
 * either half produces no rows, however good the menu handed in was.
 *
 * NEEDS A TICKING WORLD: no, and not even a world. No binding, no actor, no solver — the
 * menu is a list of pointers and the ref is two integers, so this runs at arithmetic speed
 * alongside the solver tests.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuRowsTest,
	"DestructionGame.Presenter.PieceMenuRows",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuRowsTest::RunTest(const FString& Parameters)
{
	using namespace PieceMenuTestSupport;

	MenuCanRunEntries = 0;
	MenuRunEntries = 0;

	const TArray<const FPieceAction*> Shipped = ShippedMenu();

	/*
	 * FIXTURE PRECONDITION. Every "one row per action" claim below is free if the shipped
	 * table is empty, and the ordering claims need at least two rows to say anything — which
	 * is why the multi-row cases use test-local rows rather than waiting for a second action.
	 */
	TestTrue(
		FString::Printf(TEXT("fixture: the shipped action table should carry at least one row, it carries %d"),
			Shipped.Num()),
		Shipped.Num() >= 1);

	if (Shipped.Num() < 1)
	{
		return true;
	}

	const FPieceRef LiveRef = MakeRef(7, 3);

	const TArray<FMenuCase> Cases = {
		{
			TEXT("the shipped menu for a live piece"),
			Shipped,
			LiveRef,
			Shipped
		},
		{
			TEXT("an empty menu"),
			TArray<const FPieceAction*>(),
			LiveRef,
			TArray<const FPieceAction*>()
		},
		{
			TEXT("two rows keep the order they were offered in"),
			{ &MenuTripwire, &MenuSecondRow },
			LiveRef,
			{ &MenuTripwire, &MenuSecondRow }
		},
		{
			TEXT("the same two rows the other way round"),
			{ &MenuSecondRow, &MenuTripwire },
			LiveRef,
			{ &MenuSecondRow, &MenuTripwire }
		},
		{
			/*
			 * A ROW WHOSE CanRun SAYS NO IS STILL SHOWN. Filtering is PieceActionsFor's job
			 * and it has already happened; a presenter that asked again would be a second
			 * copy of the policy, and on today's one-row table it would be indistinguishable
			 * from this one on every other case in this file.
			 */
			TEXT("a row this presenter has no business re-filtering"),
			{ &MenuTripwire },
			LiveRef,
			{ &MenuTripwire }
		},
		{
			TEXT("a null row on its own"),
			{ nullptr },
			LiveRef,
			TArray<const FPieceAction*>()
		},
		{
			TEXT("a null row between two good ones"),
			{ &MenuTripwire, nullptr, &MenuSecondRow },
			LiveRef,
			{ &MenuTripwire, &MenuSecondRow }
		},
		{
			TEXT("a row with no label"),
			{ &MenuTripwire, &MenuUnlabelledRow },
			LiveRef,
			{ &MenuTripwire }
		},
		{
			TEXT("a wholly default ref"),
			Shipped,
			FPieceRef(),
			TArray<const FPieceAction*>()
		},
		{
			TEXT("a ref with no structure id"),
			Shipped,
			MakeRef(INDEX_NONE, 3),
			TArray<const FPieceAction*>()
		},
		{
			TEXT("a ref with no piece index"),
			Shipped,
			MakeRef(7, INDEX_NONE),
			TArray<const FPieceAction*>()
		},
		{
			/*
			 * STRUCTURE ZERO AND PIECE ZERO ARE REAL. INDEX_NONE is the sentinel and zero is
			 * not, so a guard written as `if (!Ref.StructureId)` would silently eat the first
			 * structure ever built — which is the one the game mode builds on Play.
			 */
			TEXT("the first piece of the first structure"),
			Shipped,
			MakeRef(0, 0),
			Shipped
		},
	};

	for (const FMenuCase& Case : Cases)
	{
		const TArray<FPieceMenuRow> Rows = BuildPieceMenuRows(Case.Menu, Case.Ref);

		TestEqual(
			FString::Printf(TEXT("%s should build %d row(s), got %d [%s]"),
				Case.Description, Case.ExpectedRows.Num(), Rows.Num(), *DescribeRows(Rows)),
			Rows.Num(), Case.ExpectedRows.Num());

		if (Rows.Num() != Case.ExpectedRows.Num())
		{
			continue;
		}

		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			const FPieceMenuRow& Row = Rows[Index];
			const FPieceAction* const Expected = Case.ExpectedRows[Index];

			/*
			 * THE POINTER, NOT THE LABEL. PieceActionsFor promises rows of the shipped table
			 * rather than copies precisely so that "which entry did they choose" is a pointer
			 * comparison; a presenter that copied the row would keep every label right and
			 * break that promise silently.
			 */
			TestTrue(
				*FString::Printf(
					TEXT("%s: row %d should name the very action it was offered ('%s'), got %s"),
					Case.Description, Index,
					Expected->Label != nullptr ? Expected->Label : TEXT("<null label>"),
					Row.Action == nullptr ? TEXT("nothing") : TEXT("a different row")),
				Row.Action == Expected);

			TestEqual(
				FString::Printf(TEXT("%s: row %d should read '%s', got '%s'"),
					Case.Description, Index,
					Expected->Label != nullptr ? Expected->Label : TEXT("<null label>"),
					*Row.Label),
				Row.Label, FString(Expected->Label));

			/*
			 * AND EVERY ROW CARRIES THE REF IT WAS BUILT FOR. A presenter that dropped the ref
			 * would show a perfect menu that committed against whatever the caller happened to
			 * remember — which for a wall of 1,220 identical bricks is undetectable by eye.
			 */
			TestEqual(
				FString::Printf(TEXT("%s: row %d should commit against structure %d, got %d"),
					Case.Description, Index, Case.Ref.StructureId, Row.Ref.StructureId),
				Row.Ref.StructureId, Case.Ref.StructureId);

			TestEqual(
				FString::Printf(TEXT("%s: row %d should commit against piece %d, got %d"),
					Case.Description, Index, Case.Ref.PieceIndex, Row.Ref.PieceIndex),
				Row.Ref.PieceIndex, Case.Ref.PieceIndex);
		}
	}

	/*
	 * AND BUILDING A MENU NEITHER FILTERS NOR RUNS ANYTHING. Both counters are the only thing
	 * that can tell "showed the row" from "asked the row's CanRun and showed it anyway", and
	 * the second is a different presenter with the same output on today's table.
	 */
	TestEqual(
		FString::Printf(TEXT("building rows must not consult CanRun; it was entered %d time(s)"),
			MenuCanRunEntries),
		MenuCanRunEntries, 0);

	TestEqual(
		FString::Printf(TEXT("building rows must not run anything; Run was entered %d time(s)"),
			MenuRunEntries),
		MenuRunEntries, 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
