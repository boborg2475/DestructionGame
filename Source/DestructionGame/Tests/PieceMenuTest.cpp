// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceMenu.h"

#if WITH_DEV_AUTOMATION_TESTS

// Named namespace for unity builds; `using namespace` stays inside RunTest for the same reason.
namespace PieceMenuTestSupport
{
	/**
	 * Tripwire rows that count calls. The menu is already filtered by PieceActionsFor, so the
	 * presenter must not call CanRun again, and building a menu must run nothing.
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

	/** A destructive row, so a presenter that drops the flag is caught. */
	const FPieceAction MenuDestructiveRow{
		TEXT("Wreck"), &PieceMenuTripwireCanRun, &PieceMenuTripwireRun, /*bIsDestructive*/ true };

	/** A malformed row with no label; must be skipped. */
	const FPieceAction MenuUnlabelledRow{ nullptr, &PieceMenuTripwireCanRun, &PieceMenuTripwireRun };

	/** The shipped table as pointers, as PieceActionsFor returns it. */
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

	/** A menu, a ref, and the actions expected back in order. */
	struct FMenuCase
	{
		const TCHAR* Description;
		TArray<const FPieceAction*> Menu;
		FPieceRef Ref;
		TArray<const FPieceAction*> ExpectedRows;
	};
}

/**
 * BuildPieceMenuRows turns a menu and a ref into one row per action, in order, each carrying the
 * action by pointer, its label, and the ref. This is the testable half of the presenter; the
 * Slate half is untested. Expected rows are pointers, checking identity and order together.
 *
 * A row is a command, so a ref missing either half (e.g. a click on the floor) produces no rows.
 * No world.
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

	// The shipped-table cases are vacuous if it is empty; ordering cases use local rows.
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
			// Shown even though CanRun says no: filtering is PieceActionsFor's job.
			TEXT("a row this presenter has no business re-filtering"),
			{ &MenuTripwire },
			LiveRef,
			{ &MenuTripwire }
		},
		{
			// Both polarities, so a defaulted flag is caught.
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
			// Zero is a real id; the sentinel is INDEX_NONE.
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

			// Pointer identity, since a copied row would keep the right label.
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

			// Every row carries the ref it was built for.
			TestEqual(
				FString::Printf(TEXT("%s: row %d should commit against structure %d, got %d"),
					Case.Description, Index, Case.Ref.StructureId, Row.Ref.StructureId),
				Row.Ref.StructureId, Case.Ref.StructureId);

			TestEqual(
				FString::Printf(TEXT("%s: row %d should commit against piece %d, got %d"),
					Case.Description, Index, Case.Ref.PieceIndex, Row.Ref.PieceIndex),
				Row.Ref.PieceIndex, Case.Ref.PieceIndex);

			// bIsDestructive passes through from the action, so Slate never matches on a label.
			TestEqual(
				FString::Printf(
					TEXT("%s: row %d ('%s') should read as %s, it reads as %s"),
					Case.Description, Index, *Row.Label,
					Expected->bIsDestructive ? TEXT("destructive") : TEXT("harmless"),
					Row.bIsDestructive ? TEXT("destructive") : TEXT("harmless")),
				Row.bIsDestructive, Expected->bIsDestructive);

			// Target text derived from Refs; the plural is tested below.
			TestEqual(
				FString::Printf(TEXT("%s: row %d should say what it acts on, it says '%s'"),
					Case.Description, Index, *Row.TargetText),
				Row.TargetText, FString(TEXT("1 brick")));
		}
	}

	// The plural, via the selection overload: three bricks say "3 bricks".
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

	// Building a menu neither filters nor runs anything.
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
