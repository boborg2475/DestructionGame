// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceSelection.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges
 * many files into one. See CURRENT_STATE.md; the `using namespace` lives inside RunTest for
 * the same reason.
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

	/** One step of the script below: what was done, what it answered, and the set afterwards. */
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
 * TOGGLING A REF ADDS IT, TOGGLING IT AGAIN REMOVES IT, CLEARING EMPTIES THE SET, AND A REF
 * THAT NAMES NOTHING NEVER GETS IN.
 *
 * A SCRIPT RATHER THAN A TEST PER OPERATION, because a selection is a state machine and every
 * bug worth catching is about the state left BEHIND an operation rather than its return value.
 * Each step asserts the answer, the count, and the exact contents in order, so an add that
 * added twice, a remove that removed the wrong entry, and a clear that left one behind are all
 * different failures with different messages.
 *
 * ORDER IS ASSERTED, NOT JUST MEMBERSHIP. The batched commit runs the action against these refs
 * in this order and hands back one orphaned actor per piece in the same order, so a set that
 * quietly re-orders itself makes the commit's own results unreadable — and a TSet, which is the
 * obvious implementation, gives no order at all.
 *
 * TOGGLE ANSWERS "IS IT SELECTED NOW", which is what a caller that has just clicked wants and
 * is also why a REFUSED ref answers false rather than true: it is not in the set either way,
 * and a refusal reported as success would put a menu up for a brick nobody picked.
 *
 * ZERO IS A REAL STRUCTURE AND A REAL PIECE, so the first row picks {0,0} — the very piece the
 * game mode's wall starts with. INDEX_NONE is the sentinel and zero is not; a guard written as
 * `if (!Ref.StructureId)` would silently refuse the first brick of the first wall.
 *
 * NEEDS A TICKING WORLD: no, and not even a world. Two integers and a list.
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
			/*
			 * A DIFFERENT FPieceRef VALUE THAT NAMES THE SAME PIECE. A ref is copied through the
			 * trace, the brick actor and the menu row before it comes back here, so membership
			 * has to be by VALUE. An implementation comparing anything else deselects nothing
			 * and the set grows without bound as the player clicks the same brick.
			 */
			TEXT("clicking the first brick again deselects it, by value not identity"),
			ESelectionOp::Toggle, SelectionRef(0, 0), false, { Second }
		},
		{
			TEXT("picking a third brick appends it after the survivor"),
			ESelectionOp::Toggle, Third, true, { Second, Third }
		},
		{
			/*
			 * THE REFUSAL ROWS SIT IN THE MIDDLE OF THE SCRIPT, WITH A NON-EMPTY SET, on purpose:
			 * against an empty set "refused" and "added then removed" are the same observation.
			 */
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
			/*
			 * CLEARING AN EMPTY SET ANSWERS FALSE, which is what makes the answer worth having:
			 * "a click on empty space dismissed something" and "there was nothing up" are
			 * different events, and a presenter that always reports a change cannot tell them.
			 */
			TEXT("clearing again reports that there was nothing to clear"),
			ESelectionOp::Clear, FPieceRef(), false, TArray<FPieceRef>()
		},
		{
			TEXT("and the set is usable again afterwards"),
			ESelectionOp::Toggle, Second, true, { Second }
		},
	};

	FPieceSelection Selection;

	/* The empty set answers everything, before a single operation runs. */
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

		/* Num and Refs are two answers to one question and must never disagree. */
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

		/*
		 * AND Contains AGREES WITH THE LIST IN THE OTHER DIRECTION TOO. Without this a Contains
		 * that answered true for everything satisfies every row above.
		 */
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
