// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/PieceMenu.h"

/*
 * EVERY NAME IN HERE CARRIES A Presenter PREFIX, for the reason Structure.cpp's header
 * comment sets out at length: an anonymous namespace is private to a TRANSLATION UNIT
 * rather than to a file, a unity build merges many files into one, and two file-local
 * names that collide are then a hard compile error between files that never refer to
 * each other.
 */
namespace
{
	/**
	 * A joint's tier, in the words a player reads.
	 *
	 * THE WORDING LIVES HERE BECAUSE THE WIDGET MAY NOT HAVE IT. Choosing a word for a
	 * tier is a decision, and the menu widget was landed under a recorded exception to
	 * the TDD gate on the condition that it holds no decisions at all.
	 *
	 * None has a word of its own rather than sharing one. InspectPiece only ever lists
	 * real joints on a real piece and AddConnection refuses a normal that will not
	 * normalise, so no row can carry it today — but a tier that reads as one of the
	 * three real ones would be the fail-open direction the whole GetJointRole contract
	 * is written against.
	 */
	const TCHAR* PresenterWordForJointRole(EJointRole Role)
	{
		switch (Role)
		{
		case EJointRole::BedBeneath: return TEXT("bed below");
		case EJointRole::BedAbove:   return TEXT("bed above");
		case EJointRole::Head:       return TEXT("head");
		case EJointRole::None:       return TEXT("no tier");
		}

		return TEXT("no tier");
	}

	/**
	 * Why a brick is or is not being held up, in words.
	 *
	 * "NOBODY HAS SOLVED YET" IS ASKED FIRST AND IS ITS OWN SENTENCE, and that ordering
	 * is the whole point of the function. EPieceSupport::Falling is both a real collapse
	 * and an absent answer — deliberately, because enumerator zero has to promise least
	 * — so a readout that went straight to the enumerator would draw a freshly built,
	 * never-solved wall as a column of falling bricks: a catastrophe reported that has
	 * not happened.
	 */
	FString PresenterWordForSupport(const FPieceInspection& Inspection)
	{
		if (!Inspection.bHasSupportAnswer)
		{
			return FString(TEXT("not solved yet"));
		}

		switch (Inspection.Support)
		{
		case EPieceSupport::Grounded:  return FString(TEXT("grounded"));
		case EPieceSupport::Supported: return FString(TEXT("supported"));
		case EPieceSupport::Stranded:  return FString(TEXT("stranded"));
		case EPieceSupport::Falling:   return FString(TEXT("falling"));
		}

		return FString(TEXT("falling"));
	}
}

TArray<FPieceMenuRow> BuildPieceMenuRows(
	TArrayView<const FPieceAction* const> Actions,
	const FPieceRef& Ref)
{
	/*
	 * A MENU FOR ONE BRICK IS A MENU FOR A SELECTION OF ONE, spelled that way rather than
	 * duplicated: a menu for one brick is not a different kind of menu, and a second copy
	 * of the fail-closed rule below is a second policy free to drift from it.
	 */
	return BuildPieceMenuRows(Actions, TArrayView<const FPieceRef>(&Ref, 1));
}

TArray<FPieceMenuRow> BuildPieceMenuRows(
	TArrayView<const FPieceAction* const> Actions,
	TArrayView<const FPieceRef> Refs)
{
	TArray<FPieceMenuRow> Rows;

	/*
	 * NO SELECTION BUILDS NO ROWS. A menu with no target is a button with nothing behind
	 * it, and a row that exists must always carry at least one piece to commit against —
	 * which is also what lets everything below take Refs.Last() without a second guard.
	 */
	if (Refs.Num() == 0)
	{
		return Rows;
	}

	/*
	 * AND A REF MISSING EITHER HALF BUILDS NOTHING, however good the menu handed in was,
	 * and however many of the other refs were perfect.
	 *
	 * A row is a COMMAND waiting to be chosen rather than a readout, and a default FPieceRef
	 * is both what a click on the floor arrives as and what "we have no answer" looks like.
	 * Offering the full menu against one would put a Delete button on screen with nothing
	 * behind it. Zero is a real structure and a real piece — the first one the game mode
	 * builds — so the sentinel is tested for by name rather than by truthiness.
	 *
	 * The whole list goes rather than the bad entry, for the reason PieceActionsFor gives:
	 * dropping one would put a button on screen that acts on fewer bricks than the player
	 * picked, and nothing about that reads as a bug.
	 */
	for (const FPieceRef& Ref : Refs)
	{
		if (Ref.StructureId == INDEX_NONE || Ref.PieceIndex == INDEX_NONE)
		{
			return Rows;
		}
	}

	Rows.Reserve(Actions.Num());

	/*
	 * ONE ROW PER ACTION, IN THE ORDER THEY WERE OFFERED, AND CanRun IS NOT CONSULTED.
	 * PieceActionsFor is what decides which actions a piece may offer and it has already
	 * run; asking again here would be a second, quieter copy of that policy.
	 */
	for (const FPieceAction* const Action : Actions)
	{
		/*
		 * A MALFORMED ROW IS SKIPPED RATHER THAN DEREFERENCED. AllPieceActions() cannot hold
		 * one, but this takes an array from anywhere, and dereferencing a null row would
		 * abort the whole run rather than dropping one entry.
		 */
		if (Action == nullptr || Action->Label == nullptr)
		{
			continue;
		}

		FPieceMenuRow& Row = Rows.AddDefaulted_GetRef();

		/*
		 * THE ACTION IS CARRIED BY POINTER AND THE LABEL IS TAKEN FROM IT, never spelled
		 * here: "which entry did they choose" is a pointer comparison against the shipped
		 * table, which a copied row would break while keeping every label right.
		 */
		Row.Label = FString(Action->Label);
		Row.Action = Action;
		Row.Refs.Append(Refs.GetData(), Refs.Num());

		/*
		 * THE ANCHOR IS DERIVED FROM THE SET, NEVER STORED BESIDE IT — the same shape as
		 * Label, which is a copy of the action's own text. A field naming a piece that is
		 * not in the selection would be worse than no field, because it is what a per-brick
		 * readout hangs off.
		 */
		Row.Ref = Row.Refs.Last();
	}

	return Rows;
}

FPieceMenuInspector BuildPieceMenuInspector(
	const FStructureBinding& Binding,
	TArrayView<const FPieceRef> Selected,
	const FPieceRef& InspectedRef)
{
	FPieceMenuInspector Inspector;

	/*
	 * THE COUNT IS THE SELECTION'S OWN AND IT NEVER SHRINKS TO WHAT RESOLVES. A ref
	 * naming another wall, one whose piece a cascade took and one missing a half all
	 * still count: the player picked that many bricks and the highlights on screen are
	 * drawn off the same set, so a count that quietly disagreed with them would be the
	 * presenter contradicting itself.
	 */
	Inspector.SelectedCount = Selected.Num();

	/*
	 * SINGULAR AND PLURAL ARE DECIDED HERE, because "1 brick" against "1 bricks" is a
	 * branch and a branch in the widget is untested by construction. Nothing picked is
	 * its own sentence rather than "0 bricks selected", which reads as a fault.
	 */
	if (Inspector.SelectedCount == 0)
	{
		Inspector.CountText = FString(TEXT("No bricks selected"));
	}
	else if (Inspector.SelectedCount == 1)
	{
		Inspector.CountText = FString(TEXT("1 brick selected"));
	}
	else
	{
		Inspector.CountText =
			FString::Printf(TEXT("%d bricks selected"), Inspector.SelectedCount);
	}

	/*
	 * ONE ENTRY PER SELECTED BRICK, IN PICK ORDER, NEVER REORDERED AND NEVER
	 * DEDUPLICATED. This is a PROJECTION of a list rather than a second implementation
	 * of set semantics: FPieceSelection is what guarantees the set, and a presenter that
	 * sorted or de-duped would look perfectly tidy while ceasing to agree with the order
	 * the batched commit runs in.
	 */
	Inspector.Pieces.Reserve(Selected.Num());

	/*
	 * AND EVERY LABEL NAMES BOTH HALVES OF THE REF — "brick 4:1", ALWAYS. A piece index on
	 * its own is not an identity: two walls both have a brick 1, and a selection can hold
	 * refs from more than one of them, so an unqualified label presents two different
	 * bricks as one string — one of which singles out nothing when it is clicked.
	 *
	 * THE RULE IS TOTAL, AND THAT IS THE POINT RATHER THAN AN OVERSIGHT. Qualifying only
	 * the "foreign" refs would need to know which structure is the home one, which is state
	 * this model does not have and a branch it should not grow — and this is the one seam
	 * whose whole job is telling two bricks apart. So a ref missing a half prints its
	 * absence as -1, which is not a piece index any wall can have: it is a debugger, and
	 * unambiguous beats friendly. Anything prettier would read like a real brick beside
	 * ones that are.
	 */
	for (const FPieceRef& Ref : Selected)
	{
		FInspectorPieceEntry& Entry = Inspector.Pieces.AddDefaulted_GetRef();
		Entry.Ref = Ref;
		Entry.Label = FString::Printf(TEXT("brick %d:%d"), Ref.StructureId, Ref.PieceIndex);
	}

	/*
	 * THE INSPECTED BRICK MUST BE A MEMBER OF THE SELECTION, AND THE MEMBERSHIP IS FOUND
	 * AS AN INDEX RATHER THAN AS A BOOL. Marking by index marks exactly one entry however
	 * many times the same brick was picked, so "at most one entry reads as inspected"
	 * holds by construction instead of by a guard somebody has to remember. An anchor
	 * outside the set it anchors is a readout of somebody else's brick — Core/PieceMenu.h
	 * says why on FPieceMenuRow::Ref, and it does not weaken here.
	 */
	const int32 InspectedEntry = Selected.IndexOfByKey(InspectedRef);

	/*
	 * AND IT MUST BE A LIVE PIECE, WHICH IS InspectPiece'S QUESTION AND NOT A SECOND ONE.
	 * A ref naming another structure, a ref missing a half and a piece a removal took all
	 * come back bIsPiece false, so the default-constructed inspection below IS the
	 * fail-closed answer: nothing singled out, nothing broken out, and no support word.
	 */
	FPieceInspection Inspection;

	if (InspectedEntry != INDEX_NONE)
	{
		Inspection = InspectPiece(Binding, InspectedRef);
	}

	Inspector.bHasInspectedPiece = Inspection.bIsPiece;

	if (!Inspector.bHasInspectedPiece)
	{
		return Inspector;
	}

	Inspector.Pieces[InspectedEntry].bIsInspected = true;
	Inspector.InspectedRef = InspectedRef;
	Inspector.SupportText = PresenterWordForSupport(Inspection);

	/*
	 * THE BREAKOUT IS InspectPiece'S ANSWER CONVERTED AND WORDED, ROW FOR ROW, IN ITS
	 * ORDER. Nothing here consults FConnection, a normal or a strength profile: the tier,
	 * the force, the ratio and the two break fields are read straight off the model,
	 * because a second derivation agrees to nine decimal places forever and still differs
	 * in the last bit. That includes the ADJACENCY — a joint that has given is dropped
	 * from the solver's support lists before the tier is even decided, and it is exactly
	 * the row a player who just pulled a brick is looking for.
	 */
	Inspector.Joints.Reserve(Inspection.Joints.Num());

	for (const FJointInspection& Joint : Inspection.Joints)
	{
		FInspectorJointRow& Row = Inspector.Joints.AddDefaulted_GetRef();

		Row.ConnectionIndex = Joint.ConnectionIndex;
		Row.OtherPieceIndex = Joint.OtherPieceIndex;
		Row.Role = Joint.Role;
		Row.bHasGiven = Joint.bHasGiven;
		Row.BreakPass = Joint.BreakPass;

		/*
		 * THE ONE UNIT CHANGE, AND THE ONLY PLACE ENTITLED TO IT. 1 N = 100 uu, named
		 * once as DestructionPresenter::ForceUnitsPerNewton — NOT ForceUnitsPerMPaSqCm,
		 * which is 10,000, this factor times cm2-to-mm2, and reaching for it would be a
		 * clean 100x that a tuned-looking readout hides perfectly. Divided rather than
		 * multiplied by 0.01, because 0.01 is not representable and the quotient is what
		 * a decimal reading of the model's own number gives.
		 */
		Row.ForceN = Joint.ForceUu.Size() / DestructionPresenter::ForceUnitsPerNewton;
		Row.UtilisationPercent = Joint.Utilisation * 100.0;

		/*
		 * AND A JOINT THAT HAS GIVEN IS A DIFFERENT SENTENCE, NOT A DIFFERENT NUMBER. It
		 * carries nothing, so it reads 0 N at 0 % — identical to an intact joint with
		 * nothing on it, and one of those is a hole in the wall. Deciding that here is
		 * what keeps the widget free of the branch.
		 */
		Row.Text = Row.bHasGiven
			? FString::Printf(
				TEXT("#%d  brick %d  %s  broken (went with a removed piece)"),
				Row.ConnectionIndex, Row.OtherPieceIndex,
				PresenterWordForJointRole(Row.Role))
			: FString::Printf(
				TEXT("#%d  brick %d  %s  %.1f N  %.3f %%"),
				Row.ConnectionIndex, Row.OtherPieceIndex,
				PresenterWordForJointRole(Row.Role),
				Row.ForceN, Row.UtilisationPercent);
	}

	return Inspector;
}
