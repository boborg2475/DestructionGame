// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceMenu.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Named namespace, unique to this module: a unity build merges files, so an anonymous namespace
 * would collide with another file's (see CURRENT_STATE.md). The `using namespace` lives inside
 * RunTest for the same reason.
 */
namespace PieceMenuTestSupport
{
	/**
	 * Tripwire rows that must never be called; the counters say so. BuildPieceMenuRows takes a menu
	 * PieceActionsFor already filtered, so re-asking CanRun would be a second copy of the policy —
	 * invisible on today's one-row table, since Delete's CanRun says yes for a live piece. Run is
	 * counted the other way: building a menu must run nothing.
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

	/**
	 * A row that declares itself destructive, the only way the passthrough is visible: if every
	 * shipped action shared one polarity, a presenter that dropped the flag would still agree. So
	 * the fixture supplies both, and the sweep compares each row against the action it came from.
	 */
	const FPieceAction MenuDestructiveRow{
		TEXT("Wreck"), &PieceMenuTripwireCanRun, &PieceMenuTripwireRun, /*bIsDestructive*/ true };

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
 * The presenter turns a menu and a ref into rows: one per action, in table order, each carrying
 * the action's label, the action by pointer, and the ref it commits against — and nothing for a
 * ref that names nothing.
 *
 * This is the testable half of a presenter. Asserting a Slate button appeared needs a viewport and
 * cannot see the failure that matters: which rows were built (an extra entry, a missing one, a
 * shuffled order, or a row carrying the wrong ref). All four are a pure function of the list and
 * the ref, needing no world, so this owns that function and the Slate half is deliberately untested.
 *
 * A parameterised table, so a second action is a row here as it is there. The expectation is the
 * pointers that must come back in order, checking identity and ordering at once — pointer identity
 * is PieceActionsFor's whole promise, and a presenter that copied a row would break it silently.
 *
 * Fail-closed has a different polarity here than one layer down (CURRENT_STATE.md names the trap).
 * A row is a command, not a readout: a click on the floor arrives as a default FPieceRef, which is
 * also what "no answer" looks like. A readout may take that at face value; a button may not, so a
 * ref missing either half produces no rows.
 *
 * No ticking world, not even a world: a list of pointers and two integers.
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
	 * FIXTURE PRECONDITION. The "one row per action" claims are vacuous on an empty table, so the
	 * ordering cases use test-local rows rather than waiting for a second shipped action.
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
			 * A row whose CanRun says no is still shown. Filtering is PieceActionsFor's job and has
			 * happened; a presenter that asked again would be a second copy of the policy.
			 */
			TEXT("a row this presenter has no business re-filtering"),
			{ &MenuTripwire },
			LiveRef,
			{ &MenuTripwire }
		},
		{
			/*
			 * A destructive row beside an ordinary one, the pair the flag needs to tell "carried
			 * across" from "defaulted" — the table's shape once a second action lands beside Delete.
			 */
			TEXT("a destructive row and an ordinary one in one menu"),
			{ &MenuTripwire, &MenuDestructiveRow },
			LiveRef,
			{ &MenuTripwire, &MenuDestructiveRow }
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
			 * Structure zero and piece zero are real. INDEX_NONE is the sentinel, not zero, so a
			 * guard like `if (!Ref.StructureId)` would eat the first structure ever built.
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
			 * The pointer, not the label. PieceActionsFor promises rows of the shipped table, not
			 * copies, so "which entry did they choose" is a pointer comparison; a copy keeps the
			 * label right and breaks that promise silently.
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
			 * And every row carries the ref it was built for. A dropped ref shows a perfect menu
			 * that commits against whatever the caller remembered — invisible on a wall of bricks.
			 */
			TestEqual(
				FString::Printf(TEXT("%s: row %d should commit against structure %d, got %d"),
					Case.Description, Index, Case.Ref.StructureId, Row.Ref.StructureId),
				Row.Ref.StructureId, Case.Ref.StructureId);

			TestEqual(
				FString::Printf(TEXT("%s: row %d should commit against piece %d, got %d"),
					Case.Description, Index, Case.Ref.PieceIndex, Row.Ref.PieceIndex),
				Row.Ref.PieceIndex, Case.Ref.PieceIndex);

			/*
			 * And whether choosing it destroys something, carried across from the action and never
			 * re-decided here. The flag is the only way a widget knows which button is destructive
			 * without a decision of its own; the alternative, `Label == TEXT("Delete")` in Slate,
			 * is a policy in a string literal no test can read. Held against the action's own flag,
			 * which makes it a passthrough claim rather than a second copy of the data.
			 */
			TestEqual(
				FString::Printf(
					TEXT("%s: row %d ('%s') should read as %s, it reads as %s"),
					Case.Description, Index, *Row.Label,
					Expected->bIsDestructive ? TEXT("destructive") : TEXT("harmless"),
					Row.bIsDestructive ? TEXT("destructive") : TEXT("harmless")),
				Row.bIsDestructive, Expected->bIsDestructive);

			/*
			 * And what it will act on, in words. A row against one brick says "1 brick", singular
			 * and plural derived from Refs so a button cannot promise a different count than it
			 * acts on. The plural is exercised below, on a selection.
			 */
			TestEqual(
				FString::Printf(TEXT("%s: row %d should say what it acts on, it says '%s'"),
					Case.Description, Index, *Row.TargetText),
				Row.TargetText, FString(TEXT("1 brick")));
		}
	}

	/*
	 * And the plural, which only the selection overload reaches. Three picked bricks make one row
	 * committing against three, and it must say "3 bricks" — a widget choosing the suffix itself is
	 * the untestable branch that yields "1 bricks".
	 */
	{
		const TArray<FPieceRef> ThreeBricks = { MakeRef(7, 3), MakeRef(7, 4), MakeRef(7, 5) };

		const TArray<FPieceMenuRow> Rows = BuildPieceMenuRows(Shipped, ThreeBricks);

		TestEqual(
			FString::Printf(
				TEXT("fixture: three bricks should still build one row per shipped action (%d), got %d [%s]"),
				Shipped.Num(), Rows.Num(), *DescribeRows(Rows)),
			Rows.Num(), Shipped.Num());

		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			TestEqual(
				FString::Printf(
					TEXT("a row committing against 3 bricks should say '3 bricks', row %d says '%s'"),
					Index, *Rows[Index].TargetText),
				Rows[Index].TargetText, FString(TEXT("3 bricks")));
		}
	}

	/*
	 * And building a menu neither filters nor runs anything. The counters are the only thing that
	 * tells "showed the row" from "asked CanRun and showed it anyway".
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
