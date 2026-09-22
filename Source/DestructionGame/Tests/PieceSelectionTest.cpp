// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceSelection.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Uniquely named namespace, not anonymous: a unity build merges translation units. The
 * `using namespace` lives inside RunTest for the same reason.
 */
namespace PieceSelectionTestSupport
{
	FPieceRef SelectionRef(int32 StructureId, int32 PieceIndex)
	{
		FPieceRef Ref;
		Ref.StructureId = StructureId;
		Ref.PieceIndex = PieceIndex;

		return Ref;
	}

	bool SameRef(const FPieceRef& Left, const FPieceRef& Right)
	{
		return Left.StructureId == Right.StructureId && Left.PieceIndex == Right.PieceIndex;
	}

	/** What the set holds, so a failure reads without a debugger. */
	FString DescribeSelection(TArrayView<const FPieceRef> Refs)
	{
		if (Refs.Num() == 0)
		{
			return TEXT("<empty>");
		}

		FString Line;

		for (int32 Index = 0; Index < Refs.Num(); ++Index)
		{
			Line += FString::Printf(
				TEXT("%s{%d,%d}"),
				Index == 0 ? TEXT("") : TEXT(", "),
				Refs[Index].StructureId,
				Refs[Index].PieceIndex);
		}

		return Line;
	}

	enum class ESelectionOp : uint8
	{
		Toggle,
		Clear
	};

	/** One script step: the operation, its expected answer, and the expected set afterwards. */
	struct FSelectionStep
	{
		const TCHAR* Description;
		ESelectionOp Op;
		FPieceRef Ref;
		bool bExpectedAnswer;
		TArray<FPieceRef> ExpectedSet;
	};
}

/**
 * Toggle adds then removes a ref, Clear empties the set, and an invalid ref is refused. Written
 * as a script because bugs show in the state left behind; each step checks the answer, count and
 * contents. Order is asserted, since the batched commit returns results in selection order.
 * Toggle answers "selected now", so a refused ref answers false. {0,0} is a valid ref;
 * INDEX_NONE is the sentinel. No world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceSelectionTogglingBuildsTheSetTest,
	"DestructionGame.Core.PieceSelection.TogglingBuildsTheSet",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceSelectionTogglingBuildsTheSetTest::RunTest(const FString& Parameters)
{
	using namespace PieceSelectionTestSupport;

	const FPieceRef First = SelectionRef(0, 0);
	const FPieceRef Second = SelectionRef(7, 3);
	const FPieceRef Third = SelectionRef(7, 4);

	const TArray<FSelectionStep> Steps = {
		{
			TEXT("picking the first piece of the first structure"),
			ESelectionOp::Toggle, First, true, { First }
		},
		{
			TEXT("picking a second brick keeps the first"),
			ESelectionOp::Toggle, Second, true, { First, Second }
		},
		{
			// A different FPieceRef naming the same piece: membership is by value, since refs are copied.
			TEXT("clicking the first brick again deselects it, by value not identity"),
			ESelectionOp::Toggle, SelectionRef(0, 0), false, { Second }
		},
		{
			TEXT("picking a third brick appends it after the survivor"),
			ESelectionOp::Toggle, Third, true, { Second, Third }
		},
		{
			// Refusals run against a non-empty set, where they are distinguishable from add-then-remove.
			TEXT("a wholly default ref — what a click on the floor arrives as"),
			ESelectionOp::Toggle, FPieceRef(), false, { Second, Third }
		},
		{
			TEXT("a ref with no structure id"),
			ESelectionOp::Toggle, SelectionRef(INDEX_NONE, 3), false, { Second, Third }
		},
		{
			TEXT("a ref with no piece index"),
			ESelectionOp::Toggle, SelectionRef(7, INDEX_NONE), false, { Second, Third }
		},
		{
			TEXT("clearing empties the set"),
			ESelectionOp::Clear, FPieceRef(), true, TArray<FPieceRef>()
		},
		{
			// Clearing an empty set answers false, so callers can tell nothing was dismissed.
			TEXT("clearing again reports that there was nothing to clear"),
			ESelectionOp::Clear, FPieceRef(), false, TArray<FPieceRef>()
		},
		{
			TEXT("and the set is usable again afterwards"),
			ESelectionOp::Toggle, Second, true, { Second }
		},
	};

	FPieceSelection Selection;

	// A fresh selection is empty.
	TestEqual(
		FString::Printf(TEXT("a fresh selection should be empty, it holds %d [%s]"),
			Selection.Num(), *DescribeSelection(Selection.Refs())),
		Selection.Num(), 0);

	TestEqual(
		TEXT("a fresh selection should hand back no refs"),
		Selection.Refs().Num(), 0);

	TestTrue(
		TEXT("a fresh selection contains nothing, not even a real ref"),
		!Selection.Contains(First));

	for (const FSelectionStep& Step : Steps)
	{
		const bool bAnswer = Step.Op == ESelectionOp::Toggle
			? Selection.Toggle(Step.Ref)
			: Selection.Clear();

		TestTrue(
			*FString::Printf(
				TEXT("%s: the call should answer %s, it answered %s (the set is now [%s])"),
				Step.Description,
				Step.bExpectedAnswer ? TEXT("true") : TEXT("false"),
				bAnswer ? TEXT("true") : TEXT("false"),
				*DescribeSelection(Selection.Refs())),
			bAnswer == Step.bExpectedAnswer);

		TestEqual(
			FString::Printf(TEXT("%s: the set should hold %d, it holds %d [%s]"),
				Step.Description, Step.ExpectedSet.Num(), Selection.Num(),
				*DescribeSelection(Selection.Refs())),
			Selection.Num(), Step.ExpectedSet.Num());

		// Num and Refs must agree.
		TestEqual(
			FString::Printf(TEXT("%s: Num says %d and Refs hands back %d"),
				Step.Description, Selection.Num(), Selection.Refs().Num()),
			Selection.Refs().Num(), Selection.Num());

		if (Selection.Refs().Num() != Step.ExpectedSet.Num())
		{
			continue;
		}

		for (int32 Index = 0; Index < Step.ExpectedSet.Num(); ++Index)
		{
			TestTrue(
				*FString::Printf(
					TEXT("%s: entry %d should be {%d,%d}, the set reads [%s]"),
					Step.Description, Index,
					Step.ExpectedSet[Index].StructureId, Step.ExpectedSet[Index].PieceIndex,
					*DescribeSelection(Selection.Refs())),
				SameRef(Selection.Refs()[Index], Step.ExpectedSet[Index]));

			TestTrue(
				*FString::Printf(TEXT("%s: {%d,%d} is in the set, so Contains must say so"),
					Step.Description,
					Step.ExpectedSet[Index].StructureId, Step.ExpectedSet[Index].PieceIndex),
				Selection.Contains(Step.ExpectedSet[Index]));
		}

		// Contains is also false for refs not held, so an always-true Contains fails.
		for (const FPieceRef& Candidate : { First, Second, Third, FPieceRef() })
		{
			bool bInList = false;

			for (const FPieceRef& Held : Step.ExpectedSet)
			{
				bInList = bInList || SameRef(Held, Candidate);
			}

			TestTrue(
				*FString::Printf(
					TEXT("%s: Contains({%d,%d}) should be %s; the set reads [%s]"),
					Step.Description, Candidate.StructureId, Candidate.PieceIndex,
					bInList ? TEXT("true") : TEXT("false"),
					*DescribeSelection(Selection.Refs())),
				Selection.Contains(Candidate) == bInList);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
