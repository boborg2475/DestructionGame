// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceActions.h"
#include "Core/PieceInspection.h"
#include "Core/PieceMenu.h"
#include "Core/Profiles/ConnectionProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, not anonymous, and named differently from every other one in this
 * directory. An anonymous namespace is private to a TRANSLATION UNIT rather than to a file,
 * and a unity build merges many files into one — at which point two file-local names that
 * collide are a hard compile error between files that never refer to each other. See
 * CURRENT_STATE.md; the `using namespace` lives inside each RunTest body for the same reason.
 */
namespace PieceInspectorTestSupport
{
	using namespace DestructionProfiles;

	/**
	 * Unreal's gravity, transcribed rather than imported.
	 *
	 * The 1 N = 100 uu conversion is ALREADY INSIDE THIS NUMBER — a 5 kg load weighs
	 * 5 x 980 = 4900 uu, which is 49 N x 100 — so applying a factor of 100 on top of it
	 * is the standard way to be wrong by exactly 100x here.
	 */
	constexpr double InspectorGravityCmPerSecondSquared = 980.0;

	/**
	 * SPELLED OUT INDEPENDENTLY, NEVER IMPORTED FROM PRODUCTION.
	 *
	 * DestructionPresenter::ForceUnitsPerNewton is the constant under test; writing "1 N is
	 * 100 uu" here from the units table rather than reading that symbol is what makes this
	 * file fail if the production constant is wrong, instead of agreeing with it. It is
	 * deliberately NOT DestructionForce::ForceUnitsPerMPaSqCm, which is 10,000 — that one is
	 * this factor times cm2-to-mm2, and confusing the two is a 100x error in either
	 * direction that a tuned-looking readout would hide perfectly.
	 */
	constexpr double InspectorForceUnitsPerNewton = 100.0;

	/**
	 * And the strength boundary, also spelled out: 1 MPa (1 N/mm2) over 1 cm2 is
	 * 100 x 100 uu. Used only by the fixture preconditions, which check that the graph's
	 * own numbers are what this file thinks they are before anything is asserted about how
	 * they are presented.
	 */
	constexpr double InspectorForceUnitsPerMPaSqCm = 100.0 * 100.0;

	/** An ordinary bed- or head-joint face. */
	constexpr double InspectorJointAreaSqCm = 100.0;

	/** The structure the fixture identifies itself as, and one it does not. */
	constexpr int32 InspectorStructure = 4;
	constexpr int32 InspectorOtherStructure = 9;

	/*
	 * THE WORKED FIXTURE. Five pieces around one subject, so that the brick being inspected
	 * wears all three joint roles at once and one of its joints has GONE:
	 *
	 *                        [2] Rider  3 kg
	 *                         |  conn 1   bed joint ABOVE the subject
	 *      Spare [3] ~ ~ ~ ~ [1] Subject 2 kg
	 *      (removed) conn 2   |  head joint, severed when the spare was pulled
	 *                         |  conn 0   bed joint BENEATH the subject
	 *                        [0] Pad    grounded, 10 kg
	 *
	 *      [4] Floater 4 kg — no joints at all, nothing holding it up, so ApplyResults
	 *                         RELEASES it. That is what makes Delete's CanRun say no, which
	 *                         is what empties the menu while the debugger stays full.
	 *
	 * Piece 3 is pulled out before the solve, which SEVERS conn 2 without it ever having
	 * failed — HasGiven true with no break pass, the state DESIGN.md's table calls "went
	 * with a removed piece". That joint is the one a readout must not draw like an intact
	 * one, and it is also the discriminator for "the breakout came from InspectPiece": the
	 * solver's own support lists drop a given joint before the tier is decided, so a
	 * presenter that walked those instead would show two rows where three are due and look
	 * entirely reasonable doing it.
	 *
	 * AND A SECOND, DISJOINT COMPONENT: THE STRANDING KNOT, which is the only way to reach
	 * EPieceSupport::Stranded at all.
	 *
	 *      [5] Knot ground —head— [6] Knot X —head— [7] Knot Y
	 *          (grounded)
	 *
	 * It is Structure.PieceSupportReason's own minimum repro, transcribed: neither X nor Y
	 * has a bed joint, so each falls back to its head joints — X's supports are {ground, Y}
	 * and Y's are {X}, so each is ultimately its own support and BOTH are in the knot. The
	 * solver reports them STRANDED rather than merely falling, which is the state that means
	 * "the solver gave up on this", not "the earth is not under it".
	 *
	 * IT IS A SEPARATE COMPONENT ON PURPOSE. Nothing here touches pieces 0-4, so every
	 * expectation the diagram above carries — the subject's three joints, the pad's one, the
	 * floater's none, the severed conn 2 — is untouched, and the knot's own pieces are named
	 * by no other row in the table.
	 */
	constexpr int32 PadPiece = 0;
	constexpr int32 SubjectPiece = 1;
	constexpr int32 RiderPiece = 2;
	constexpr int32 SparePiece = 3;
	constexpr int32 FloaterPiece = 4;
	constexpr int32 KnotGroundPiece = 5;
	constexpr int32 KnotXPiece = 6;
	constexpr int32 KnotYPiece = 7;

	constexpr int32 PadJoint = 0;
	constexpr int32 RiderJoint = 1;
	constexpr int32 SpareJoint = 2;

	constexpr double PadMassKg = 10.0;
	constexpr double SubjectMassKg = 2.0;
	constexpr double RiderMassKg = 3.0;
	constexpr double SpareMassKg = 1.0;
	constexpr double FloaterMassKg = 4.0;
	constexpr double KnotMassKg = 5.0;

	/** Straight up: a bed joint, which bears in compression. */
	const FVector InspectorBedNormal(0.0, 0.0, 1.0);

	/** Straight sideways: a head joint, which can only carry in shear. */
	const FVector InspectorHeadNormal(1.0, 0.0, 0.0);

	FPieceRef MakeRef(int32 StructureId, int32 PieceIndex)
	{
		FPieceRef Ref;
		Ref.StructureId = StructureId;
		Ref.PieceIndex = PieceIndex;
		return Ref;
	}

	/** A distinct box per handle, so nothing here can pass on a shifted array. */
	DestructionLayout::FPieceBox InspectorBoxFor(int32 Index)
	{
		DestructionLayout::FPieceBox Box;
		Box.CentreCm = FVector(Index * 100.0 + 1.0, 2.0, 3.0);
		Box.ExtentCm = FVector(10.75, 5.125, 3.25);
		return Box;
	}

	void AddJoint(
		FStructureBinding& Out,
		int32 PieceA,
		int32 PieceB,
		const FVector& Normal)
	{
		FConnection Connection;
		Connection.PieceA = PieceA;
		Connection.PieceB = PieceB;
		Connection.InterfaceNormal = Normal;
		Connection.InterfaceAreaSqCm = InspectorJointAreaSqCm;
		Connection.Strength = GeneralPurposeMortar;
		Out.AddConnection(Connection);
	}

	/**
	 * The diagram above, built.
	 *
	 * bSettle drives the whole "has anybody asked yet" axis: solving and pushing gives the
	 * subject a support answer and releases the floater, while leaving it false is the
	 * freshly-built wall that must not read as a column of falling bricks.
	 *
	 * A NULL ACTOR IS FINE HERE. Nothing in this file releases through an actor, resolves
	 * one or destroys one — ApplyResults latches a flag on the binding and needs no UObject
	 * — and spawning stand-ins for a slice that never looks at them would be testing the
	 * next one by accident.
	 */
	void BuildWorkedFixture(FStructureBinding& Out, bool bSettle)
	{
		Out.StructureId = InspectorStructure;

		Out.AddPiece(PadMassKg, /*bIsGrounded*/ true, nullptr, InspectorBoxFor(PadPiece));
		Out.AddPiece(SubjectMassKg, false, nullptr, InspectorBoxFor(SubjectPiece));
		Out.AddPiece(RiderMassKg, false, nullptr, InspectorBoxFor(RiderPiece));
		Out.AddPiece(SpareMassKg, false, nullptr, InspectorBoxFor(SparePiece));
		Out.AddPiece(FloaterMassKg, false, nullptr, InspectorBoxFor(FloaterPiece));
		Out.AddPiece(KnotMassKg, /*bIsGrounded*/ true, nullptr, InspectorBoxFor(KnotGroundPiece));
		Out.AddPiece(KnotMassKg, false, nullptr, InspectorBoxFor(KnotXPiece));
		Out.AddPiece(KnotMassKg, false, nullptr, InspectorBoxFor(KnotYPiece));

		AddJoint(Out, PadPiece, SubjectPiece, InspectorBedNormal);
		AddJoint(Out, SubjectPiece, RiderPiece, InspectorBedNormal);
		AddJoint(Out, SubjectPiece, SparePiece, InspectorHeadNormal);

		/*
		 * The knot, appended AFTER the three joints above so no existing connection index
		 * moves — the pinned joint lines name #0, #1 and #2 by number.
		 */
		AddJoint(Out, KnotGroundPiece, KnotXPiece, InspectorHeadNormal);
		AddJoint(Out, KnotXPiece, KnotYPiece, InspectorHeadNormal);

		Out.RemovePiece(SparePiece);

		if (bSettle)
		{
			Out.SolveLoads();
			Out.ApplyResults();
		}
	}

	/**
	 * Utilisation of a joint whose ONLY loaded axis is compression.
	 *
	 * WHICH AXIS GOVERNS IS NOT FREE, and getting it wrong is how a test aimed at one thing
	 * silently measures another. Here the force is exactly antiparallel to an exactly
	 * vertical normal, so shear and tension are exactly zero and their ratios are exactly
	 * zero whatever their capacities are — compression is the only axis that can be the
	 * worst of the three, so ComputeUtilisation's "worst axis" answer is this one.
	 */
	double CompressionOnlyUtilisation(double ForceMagnitudeUu)
	{
		const double StressMPa =
			ForceMagnitudeUu / (InspectorJointAreaSqCm * InspectorForceUnitsPerMPaSqCm);

		return StressMPa / GeneralPurposeMortar.CompressiveStrengthMPa;
	}

	const TCHAR* NameOfRole(EJointRole Role)
	{
		switch (Role)
		{
		case EJointRole::None:       return TEXT("None");
		case EJointRole::BedBeneath: return TEXT("BedBeneath");
		case EJointRole::BedAbove:   return TEXT("BedAbove");
		case EJointRole::Head:       return TEXT("Head");
		}

		return TEXT("<not a role>");
	}

	/** What came back, so a failure reads without a debugger. */
	FString DescribeInspector(const FPieceMenuInspector& Inspector)
	{
		FString Line = FString::Printf(
			TEXT("{count:%d '%s' entries:"), Inspector.SelectedCount, *Inspector.CountText);

		if (Inspector.Pieces.Num() == 0)
		{
			Line += TEXT("<none>");
		}

		for (int32 Index = 0; Index < Inspector.Pieces.Num(); ++Index)
		{
			const FInspectorPieceEntry& Entry = Inspector.Pieces[Index];

			Line += FString::Printf(
				TEXT("%s'%s'{%d,%d}%s%s"),
				Index == 0 ? TEXT("") : TEXT(" "),
				*Entry.Label, Entry.Ref.StructureId, Entry.Ref.PieceIndex,
				Entry.bIsLivePiece ? TEXT("") : TEXT("[dead]"),
				Entry.bIsInspected ? TEXT("<==") : TEXT(""));
		}

		Line += FString::Printf(
			TEXT(" inspected:%s{%d,%d} support:'%s' jointstext:'%s' joints:"),
			Inspector.bHasInspectedPiece ? TEXT("yes") : TEXT("no"),
			Inspector.InspectedRef.StructureId, Inspector.InspectedRef.PieceIndex,
			*Inspector.SupportText, *Inspector.JointsText);

		if (Inspector.Joints.Num() == 0)
		{
			return Line + TEXT("<none>}");
		}

		for (int32 Index = 0; Index < Inspector.Joints.Num(); ++Index)
		{
			const FInspectorJointRow& Joint = Inspector.Joints[Index];

			Line += FString::Printf(
				TEXT("%s[c%d->p%d %s %.6f N %.9f %% %s pass=%d '%s']"),
				Index == 0 ? TEXT("") : TEXT(" "),
				Joint.ConnectionIndex, Joint.OtherPieceIndex, NameOfRole(Joint.Role),
				Joint.ForceN, Joint.UtilisationPercent,
				Joint.bHasGiven ? TEXT("GIVEN") : TEXT("intact"),
				Joint.BreakPass, *Joint.Text);
		}

		return Line + TEXT("}");
	}

	/** One row of the selection table below. */
	struct FInspectorCase
	{
		const TCHAR* Description = nullptr;

		/** What the player has picked, in pick order. */
		TArray<FPieceRef> Selected;

		/** Which of them is being singled out — or something that is not one of them. */
		FPieceRef Inspected;

		/** One entry per selected brick, in the same order, whatever the refs resolve to. */
		TArray<FPieceRef> ExpectedEntries;

		/**
		 * What each of those entries READS, in the same order. Same length as ExpectedEntries,
		 * and the table-integrity check below says so rather than letting a short row skip.
		 */
		TArray<FString> ExpectedLabels;

		/**
		 * Whether each of those entries still names a piece in the graph, in the same order.
		 *
		 * WRITTEN OUT PER ROW FROM THE DIAGRAM rather than derived from the binding here — a
		 * check derived the same way the production code derives it agrees with it whatever it
		 * does. Pieces 0, 1, 2 and 4-7 are live, piece 3 was pulled out, structure 9 does not
		 * exist, and a ref missing a half names nothing anywhere.
		 */
		TArray<bool> ExpectedLive;

		const TCHAR* ExpectedCountText = nullptr;

		/** Index into ExpectedEntries of the singled-out brick, or INDEX_NONE for none. */
		int32 ExpectedInspectedEntry = INDEX_NONE;

		/** How many joints the singled-out brick has. Meaningful only when one is. */
		int32 ExpectedJointCount = 0;

		/** Empty when nothing is being inspected. */
		const TCHAR* ExpectedSupportText = TEXT("");

		/**
		 * The joint list as a sentence. Empty when nothing is being inspected, and the whole
		 * point of the field is that an inspected brick with NO joints still gets one.
		 */
		const TCHAR* ExpectedJointsText = TEXT("");
	};

	/**
	 * THE PROPERTIES EVERY ANSWER MUST HAVE, whatever the case.
	 *
	 * Written once and swept over every row, because these are the ones that fail QUIETLY:
	 * a second brick reading as inspected draws two joint lists over one breakout, a NaN
	 * utilisation renders as a plausible "nan %", and joints left behind from a previous
	 * brick are the readout of somebody else's wall.
	 */
	void CheckInspectorInvariants(
		FAutomationTestBase& Test,
		const FPieceMenuInspector& Inspector,
		const TCHAR* Where)
	{
		Test.TestEqual(
			FString::Printf(
				TEXT("%s: the entry list and the count are one fact; %d entries against a count of %d %s"),
				Where, Inspector.Pieces.Num(), Inspector.SelectedCount,
				*DescribeInspector(Inspector)),
			Inspector.Pieces.Num(), Inspector.SelectedCount);

		int32 InspectedEntries = 0;

		for (int32 Index = 0; Index < Inspector.Pieces.Num(); ++Index)
		{
			const FInspectorPieceEntry& Entry = Inspector.Pieces[Index];

			InspectedEntries += Entry.bIsInspected ? 1 : 0;

			/*
			 * AND THE SINGLED-OUT ENTRY IS ALWAYS A LIVE ONE. bHasInspectedPiece is
			 * FPieceInspection::bIsPiece and bIsLivePiece must be the same question asked of
			 * the same ref, so an entry that read inspected while reading dead would be the
			 * model contradicting itself in the two fields a widget draws side by side —
			 * a greyed-out row with a joint breakout under it.
			 */
			if (Entry.bIsInspected)
			{
				Test.TestTrue(
					*FString::Printf(
						TEXT("%s: entry %d is singled out, so it must also read as a live piece %s"),
						Where, Index, *DescribeInspector(Inspector)),
					Entry.bIsLivePiece);
			}
		}

		Test.TestEqual(
			FString::Printf(
				TEXT("%s: at most ONE entry may read as inspected, %d do %s"),
				Where, InspectedEntries, *DescribeInspector(Inspector)),
			InspectedEntries, Inspector.bHasInspectedPiece ? 1 : 0);

		/*
		 * NOTHING INSPECTED MEANS NOTHING DRAWN ABOUT ONE. A breakout left over from the
		 * brick the player just deselected is the stale-field defect FPieceMenuRow was
		 * shaped to prevent, one layer out.
		 */
		if (!Inspector.bHasInspectedPiece)
		{
			Test.TestEqual(
				FString::Printf(TEXT("%s: nothing inspected must break out no joints, it broke out %d %s"),
					Where, Inspector.Joints.Num(), *DescribeInspector(Inspector)),
				Inspector.Joints.Num(), 0);

			Test.TestEqual(
				FString::Printf(TEXT("%s: nothing inspected must say nothing about support, it says '%s'"),
					Where, *Inspector.SupportText),
				Inspector.SupportText, FString());

			Test.TestTrue(
				*FString::Printf(TEXT("%s: nothing inspected must name no brick, it names {%d,%d}"),
					Where, Inspector.InspectedRef.StructureId, Inspector.InspectedRef.PieceIndex),
				Inspector.InspectedRef == FPieceRef());

			Test.TestEqual(
				FString::Printf(
					TEXT("%s: nothing inspected must say nothing about joints, it says '%s'"),
					Where, *Inspector.JointsText),
				Inspector.JointsText, FString());
		}

		/*
		 * AND A BRICK THAT IS INSPECTED ALWAYS GETS A SENTENCE ABOUT ITS JOINTS, INCLUDING
		 * WHEN IT HAS NONE. That is the whole reason the field exists: "no joints" is a fact
		 * about the brick, and a widget left to notice an empty array for itself would be
		 * holding a branch in the one place nothing can test. CountText's "No bricks selected"
		 * is the same rule one level up.
		 */
		if (Inspector.bHasInspectedPiece)
		{
			Test.TestFalse(
				*FString::Printf(
					TEXT("%s: a brick is singled out, so its %d joint(s) must be summed up in words; the line is empty %s"),
					Where, Inspector.Joints.Num(), *DescribeInspector(Inspector)),
				Inspector.JointsText.IsEmpty());
		}

		/*
		 * AND EVERY NUMBER A HUMAN WILL READ IS FINITE. GetConnectionUtilisation fails closed
		 * to TNumericLimits<double>::Max() for a connection that does not exist, which times
		 * 100 is an infinity, and FMath::Max discards a NaN rather than propagating it — so
		 * "it looked like a number" is exactly how a degenerate answer reaches a screen.
		 */
		for (int32 Index = 0; Index < Inspector.Joints.Num(); ++Index)
		{
			const FInspectorJointRow& Joint = Inspector.Joints[Index];

			Test.TestTrue(
				*FString::Printf(TEXT("%s: joint row %d must show a finite force, it shows %f"),
					Where, Index, Joint.ForceN),
				FMath::IsFinite(Joint.ForceN));

			Test.TestTrue(
				*FString::Printf(TEXT("%s: joint row %d must show a finite utilisation, it shows %f"),
					Where, Index, Joint.UtilisationPercent),
				FMath::IsFinite(Joint.UtilisationPercent));

			Test.TestFalse(
				*FString::Printf(TEXT("%s: joint row %d must say something, its line is empty"),
					Where, Index),
				Joint.Text.IsEmpty());
		}
	}
}

/**
 * THE PRESENTED MENU IS TWO THINGS: THE ACTIONS, AND A DEBUGGER THAT SAYS HOW MANY BRICKS
 * ARE SELECTED, LISTS EVERY ONE OF THEM IN PICK ORDER, AND BREAKS OUT THE JOINTS OF THE ONE
 * BRICK BEING SINGLED OUT — OR OF NONE.
 *
 * WHY A SIBLING OF FPieceMenuRow RATHER THAN MORE FIELDS ON IT, AND WHY THAT IS TESTED
 * RATHER THAN ARGUED. A row is a COMMAND waiting to be chosen and an inspector is a READOUT,
 * and the two have opposite fail-closed polarities: BuildPieceMenuRows refuses the WHOLE
 * list when one ref names nothing, because an offer with a hole in it is a lie about what
 * the button will do, whereas a readout with a hole in it must still report the hole. The
 * case that settles it is in the table below — a selection holding a RELEASED brick, where
 * PieceActionsFor's intersection empties, BuildPieceMenuRows builds nothing at all, and the
 * debugger must still list both bricks and break out the live one. An inspector carried on a
 * row would go dark at exactly the moment the player is asking why.
 *
 * THE COUNT IS ASSERTED ON THE PRESENTED MODEL, NOT ON THE SELECTION. FPieceMenuRow::Refs
 * has carried the whole selection for weeks while the screen showed one word, so a test that
 * read Refs.Num() back would have passed throughout and proved nothing about what a player
 * can see. Every assertion here is on the thing the widget draws from.
 *
 * THE COUNT NEVER SHRINKS TO WHAT RESOLVES. A ref naming another structure, a ref whose
 * piece a cascade removed and a malformed ref all still count, because the player picked
 * that many bricks and the highlights on screen are drawn off the same set — a count that
 * quietly disagreed with them would be the presenter contradicting itself.
 *
 * EVERY ENTRY ALSO SAYS WHETHER IT STILL NAMES A BRICK. Without that, a removed brick, a ref
 * naming another structure and a live brick present identically, so a widget wanting to grey
 * the dead one out would have to resolve the ref against the binding itself — model logic in
 * the one place the recorded widget exception says there may be none. It is the same question
 * FPieceInspection::bIsPiece answers, asked once per entry rather than only for the singled-out
 * one, and a RELEASED brick reads LIVE: what the menu may do about it is PieceActionsFor's
 * intersection and is already said by the rows going empty.
 *
 * AND AN INSPECTED BRICK WITH NO JOINTS GETS ITS OWN SENTENCE, for the reason "No bricks
 * selected" is a sentence rather than "0 bricks selected": an empty list is a fact about the
 * brick, and a widget left to notice Joints.Num() == 0 for itself is a branch nothing can test.
 *
 * AND EVERY ENTRY'S LABEL IS ASSERTED, BECAUSE IT IS THE ONE STRING IN THE MODEL THAT ONLY
 * EVER APPEARED IN FAILURE MESSAGES. DescribeInspector prints it, which is what made it look
 * covered; nothing read it back, so deleting the line that builds it left the suite green.
 * The rule is "brick <StructureId>:<PieceIndex>", total rather than conditional, and the
 * assertion site says why — the short version is that this is a debugger and two bricks in
 * two structures can share a piece index.
 *
 * NEEDS A TICKING WORLD: no, and not even a world. FStructureBinding is a plain struct and
 * the whole answer is arithmetic and formatting over it, which is exactly why the presenter
 * is allowed to own every decision the untested widget must not.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuInspectorTest,
	"DestructionGame.Presenter.PieceMenuInspector",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuInspectorTest::RunTest(const FString& Parameters)
{
	using namespace PieceInspectorTestSupport;

	FStructureBinding Binding;
	BuildWorkedFixture(Binding, /*bSettle*/ true);

	/*
	 * FIXTURE PRECONDITIONS, ASKED OF THE GRAPH DIRECTLY. Every claim below about what is
	 * presented is worthless if the wall underneath it is not the wall in the diagram, and
	 * these are green on arrival by construction — they check the fixture, not the feature.
	 */
	TestTrue(
		TEXT("fixture: the spare should be out of the graph, so its joint is severed"),
		Binding.IsPieceRemoved(SparePiece));

	TestTrue(
		TEXT("fixture: the floater has nothing holding it up, so ApplyResults should release it"),
		Binding.IsReleased(FloaterPiece));

	TestFalse(
		TEXT("fixture: the subject is held up, so it must NOT be released"),
		Binding.IsReleased(SubjectPiece));

	TestTrue(
		TEXT("fixture: the subject should be Supported"),
		Binding.GetStructure().GetPieceSupport(SubjectPiece) == EPieceSupport::Supported);

	TestTrue(
		TEXT("fixture: the pad should be Grounded"),
		Binding.GetStructure().GetPieceSupport(PadPiece) == EPieceSupport::Grounded);

	/*
	 * AND THE KNOT MUST ACTUALLY BE A KNOT, asked of the solver rather than assumed from the
	 * diagram. Stranded is the one support state with no other way in — it needs a cycle in
	 * the support relation that never reaches the earth — so if this fixture ever stops
	 * producing one, the "stranded" row below would quietly retarget onto whatever state the
	 * solver gives instead and go on passing while covering nothing.
	 */
	TestTrue(
		TEXT("fixture: knot X should be Stranded — the solver could not route it"),
		Binding.GetStructure().GetPieceSupport(KnotXPiece) == EPieceSupport::Stranded);

	TestTrue(
		TEXT("fixture: knot Y should be Stranded too — both ends are in the knot"),
		Binding.GetStructure().GetPieceSupport(KnotYPiece) == EPieceSupport::Stranded);

	/*
	 * AND THE MENU FOR A SELECTION HOLDING THE RELEASED FLOATER IS EMPTY. This is the whole
	 * sibling argument, stated as a fact about the OTHER half of the presenter before the
	 * inspector is asked anything: Delete's CanRun is !IsPieceRemoved && !IsReleased, the
	 * menu is the INTERSECTION, so one released brick empties it.
	 */
	const TArray<FPieceRef> SubjectAndFloater = {
		MakeRef(InspectorStructure, SubjectPiece), MakeRef(InspectorStructure, FloaterPiece) };

	const TArray<const FPieceAction*> OfferedForBoth = PieceActionsFor(Binding, SubjectAndFloater);
	const TArray<FPieceMenuRow> RowsForBoth = BuildPieceMenuRows(OfferedForBoth, SubjectAndFloater);

	TestEqual(
		FString::Printf(
			TEXT("fixture: a selection holding a released brick must offer NO menu rows, it offered %d"),
			RowsForBoth.Num()),
		RowsForBoth.Num(), 0);

	const FPieceRef Nothing;

	const TArray<FInspectorCase> Cases = {
		{
			TEXT("nothing picked"),
			TArray<FPieceRef>(),
			Nothing,
			TArray<FPieceRef>(),
			TArray<FString>(),
			TArray<bool>(),
			TEXT("No bricks selected"),
			INDEX_NONE,
			0,
			TEXT(""),
			TEXT("")
		},
		{
			/* One brick, singled out: the ordinary case, and the singular of the count. */
			TEXT("one brick, and it is the one being inspected"),
			{ MakeRef(InspectorStructure, SubjectPiece) },
			MakeRef(InspectorStructure, SubjectPiece),
			{ MakeRef(InspectorStructure, SubjectPiece) },
			{ TEXT("brick 4:1") },
			{ true },
			TEXT("1 brick selected"),
			0,
			3,
			TEXT("supported"),
			TEXT("3 joints")
		},
		{
			/*
			 * PICK ORDER, NOT HANDLE ORDER. FPieceSelection guarantees insertion order and
			 * asserts it; a presenter that sorted would look perfectly tidy and would stop
			 * the list agreeing with the order the batch commits in.
			 */
			TEXT("three bricks in pick order, singling out the second"),
			{ MakeRef(InspectorStructure, RiderPiece),
			  MakeRef(InspectorStructure, PadPiece),
			  MakeRef(InspectorStructure, SubjectPiece) },
			MakeRef(InspectorStructure, PadPiece),
			{ MakeRef(InspectorStructure, RiderPiece),
			  MakeRef(InspectorStructure, PadPiece),
			  MakeRef(InspectorStructure, SubjectPiece) },
			{ TEXT("brick 4:2"), TEXT("brick 4:0"), TEXT("brick 4:1") },
			{ true, true, true },
			TEXT("3 bricks selected"),
			1,
			1,
			TEXT("grounded"),
			TEXT("1 joint")
		},
		{
			/* Picked but not pointed at: a list with no breakout under it. */
			TEXT("three bricks and none singled out"),
			{ MakeRef(InspectorStructure, RiderPiece),
			  MakeRef(InspectorStructure, PadPiece),
			  MakeRef(InspectorStructure, SubjectPiece) },
			Nothing,
			{ MakeRef(InspectorStructure, RiderPiece),
			  MakeRef(InspectorStructure, PadPiece),
			  MakeRef(InspectorStructure, SubjectPiece) },
			{ TEXT("brick 4:2"), TEXT("brick 4:0"), TEXT("brick 4:1") },
			{ true, true, true },
			TEXT("3 bricks selected"),
			INDEX_NONE,
			0,
			TEXT(""),
			TEXT("")
		},
		{
			/*
			 * THE INSPECTED BRICK WAS DESELECTED. It is still a perfectly live piece, so the
			 * only thing that disqualifies it is that it is no longer in the set — which is
			 * the rule: an anchor outside the set it anchors is a readout of somebody else's
			 * brick, and this is the row that stops "just call InspectPiece on it".
			 */
			TEXT("the brick being inspected has been deselected"),
			{ MakeRef(InspectorStructure, RiderPiece), MakeRef(InspectorStructure, PadPiece) },
			MakeRef(InspectorStructure, SubjectPiece),
			{ MakeRef(InspectorStructure, RiderPiece), MakeRef(InspectorStructure, PadPiece) },
			{ TEXT("brick 4:2"), TEXT("brick 4:0") },
			{ true, true },
			TEXT("2 bricks selected"),
			INDEX_NONE,
			0,
			TEXT(""),
			TEXT("")
		},
		{
			/*
			 * THE INSPECTED BRICK WENT WITH A REMOVAL, which is the state a cascade or the
			 * player's own Delete leaves behind. It is still counted and still listed —
			 * that is what the player picked — but InspectPiece fails closed on it, so
			 * nothing is singled out and nothing is broken out. FStructure legitimately
			 * keeps the last solve's answer for a removed piece; drawing that beside a
			 * brick that is gone is a confident answer about nothing.
			 */
			TEXT("the brick being inspected has been removed"),
			{ MakeRef(InspectorStructure, SubjectPiece), MakeRef(InspectorStructure, SparePiece) },
			MakeRef(InspectorStructure, SparePiece),
			{ MakeRef(InspectorStructure, SubjectPiece), MakeRef(InspectorStructure, SparePiece) },
			{ TEXT("brick 4:1"), TEXT("brick 4:3") },
			{ true, false },
			TEXT("2 bricks selected"),
			INDEX_NONE,
			0,
			TEXT(""),
			TEXT("")
		},
		{
			/*
			 * A ref naming another wall entirely: well-formed, resolves to nothing.
			 *
			 * AND THE ROW THE LABEL RULE EXISTS FOR. Both refs carry piece index 1, so a label
			 * built from the piece index alone presents two DIFFERENT bricks — one of them in
			 * a structure that is not this one, and which singles out nothing when clicked —
			 * as the identical string. Qualified by structure they read apart.
			 */
			TEXT("the brick being inspected belongs to another structure"),
			{ MakeRef(InspectorStructure, SubjectPiece),
			  MakeRef(InspectorOtherStructure, SubjectPiece) },
			MakeRef(InspectorOtherStructure, SubjectPiece),
			{ MakeRef(InspectorStructure, SubjectPiece),
			  MakeRef(InspectorOtherStructure, SubjectPiece) },
			{ TEXT("brick 4:1"), TEXT("brick 9:1") },
			{ true, false },
			TEXT("2 bricks selected"),
			INDEX_NONE,
			0,
			TEXT(""),
			TEXT("")
		},
		{
			/*
			 * A REF MISSING ONE HALF. FPieceSelection refuses to store one, so this is not
			 * reachable through the controller today — it is here because a readout takes
			 * its list from wherever it is handed, and the fail-closed answer must be
			 * "counted, listed, never singled out" rather than a breakout of piece nothing.
			 * Zero is a real structure and a real piece, so the sentinel is named rather
			 * than tested for truthiness.
			 *
			 * ITS LABEL IS PINNED RATHER THAN LEFT TO FALL OUT — "brick 4:-1". The rule is
			 * TOTAL, so the missing half is printed as what it is: -1 is not a piece index
			 * any wall can have, so it reads as absent to anyone who can read the other rows,
			 * and it is a debugger. Anything friendlier — an empty half, a word — would be a
			 * BRANCH here, and the fail-open direction of that branch is a label that reads
			 * like a brick beside one that is not.
			 */
			TEXT("a ref missing its piece index"),
			{ MakeRef(InspectorStructure, SubjectPiece), MakeRef(InspectorStructure, INDEX_NONE) },
			MakeRef(InspectorStructure, INDEX_NONE),
			{ MakeRef(InspectorStructure, SubjectPiece), MakeRef(InspectorStructure, INDEX_NONE) },
			{ TEXT("brick 4:1"), TEXT("brick 4:-1") },
			{ true, false },
			TEXT("2 bricks selected"),
			INDEX_NONE,
			0,
			TEXT(""),
			TEXT("")
		},
		{
			/*
			 * THE CASE THAT DECIDES SIBLING-VERSUS-ROW. The menu for this selection is
			 * empty — asserted above — and the debugger must be full: both bricks listed,
			 * the live one singled out, all three of its joints broken out.
			 */
			TEXT("the menu is empty because one brick is released, and the debugger is not"),
			{ MakeRef(InspectorStructure, SubjectPiece), MakeRef(InspectorStructure, FloaterPiece) },
			MakeRef(InspectorStructure, SubjectPiece),
			{ MakeRef(InspectorStructure, SubjectPiece), MakeRef(InspectorStructure, FloaterPiece) },
			{ TEXT("brick 4:1"), TEXT("brick 4:4") },
			/*
			 * AND BOTH ENTRIES ARE LIVE, INCLUDING THE RELEASED ONE. This is the row that
			 * pins what bIsLivePiece MEANS: the floater has been handed to physics and Delete
			 * refuses it, which is why the menu above came back empty — but it is still a
			 * piece in the graph with a support state and a joint list, so a readout that
			 * greyed it out would be reporting it as gone. "What the menu may do" is
			 * PieceActionsFor's intersection and is answered by the rows, not here.
			 */
			{ true, true },
			TEXT("2 bricks selected"),
			0,
			3,
			TEXT("supported"),
			TEXT("3 joints")
		},
		{
			/*
			 * AND A BRICK WITH NO JOINTS IS STILL A BRICK. The floater is a live piece that
			 * nothing is joined to, so an empty breakout is the truth about it — which is
			 * exactly why "is one inspected" is a field and not Joints.Num() > 0.
			 *
			 * IT IS ALSO THE ONE ROW THAT NEEDS THE EMPTY SENTENCE, and the reason the
			 * sentence has to be in the model at all. Everything else here is a brick with
			 * joints, so a widget could print one line per row and look complete; on this
			 * brick it would print nothing whatsoever under a heading and a support word,
			 * which reads as a readout that failed rather than as a brick standing alone.
			 * The only way to say "no joints" without a branch up there is for the model to
			 * hand over the words, exactly as CountText does for an empty selection.
			 */
			TEXT("a released brick with no joints at all"),
			{ MakeRef(InspectorStructure, FloaterPiece) },
			MakeRef(InspectorStructure, FloaterPiece),
			{ MakeRef(InspectorStructure, FloaterPiece) },
			{ TEXT("brick 4:4") },
			{ true },
			TEXT("1 brick selected"),
			0,
			0,
			TEXT("falling"),
			TEXT("No joints")
		},
		{
			/*
			 * A DUPLICATE IS PRESENTED TWICE. FPieceSelection is a set and cannot produce
			 * this, so the point is what the presenter is: a PROJECTION of a list, not a
			 * second implementation of set semantics. A presenter built on a TSet would
			 * pass every other row here and lose both the order and this.
			 */
			TEXT("the same brick twice"),
			{ MakeRef(InspectorStructure, PadPiece), MakeRef(InspectorStructure, PadPiece) },
			Nothing,
			{ MakeRef(InspectorStructure, PadPiece), MakeRef(InspectorStructure, PadPiece) },
			{ TEXT("brick 4:0"), TEXT("brick 4:0") },
			{ true, true },
			TEXT("2 bricks selected"),
			INDEX_NONE,
			0,
			TEXT(""),
			TEXT("")
		},
		{
			/*
			 * THE DUPLICATE, POINTED AT — AND THIS IS THE ROW THE "BY CONSTRUCTION" CLAIM
			 * RESTS ON.
			 *
			 * BuildPieceMenuInspector finds membership as an INDEX and marks that one entry,
			 * and Core/PieceMenu.cpp says in as many words that this is what survives a
			 * duplicate ref. The row above cannot check it: it inspects nothing, so the
			 * invariant sweep compares 0 against 0, and every other row holds a duplicate-free
			 * selection where 1 against 1 is true of a per-entry comparison too.
			 *
			 * The discriminating implementation is the obvious one —
			 * `Entry.bIsInspected = (Entry.Ref == InspectedRef)` — which passes every other row
			 * in this file and marks BOTH entries here, drawing one joint breakout under two
			 * headings. So the assertion is two facts at once: exactly one entry reads as
			 * inspected, and it is the FIRST occurrence.
			 */
			TEXT("the same brick twice, and it is the one being inspected"),
			{ MakeRef(InspectorStructure, PadPiece), MakeRef(InspectorStructure, PadPiece) },
			MakeRef(InspectorStructure, PadPiece),
			{ MakeRef(InspectorStructure, PadPiece), MakeRef(InspectorStructure, PadPiece) },
			{ TEXT("brick 4:0"), TEXT("brick 4:0") },
			{ true, true },
			TEXT("2 bricks selected"),
			0,
			1,
			TEXT("grounded"),
			TEXT("1 joint")
		},
		{
			/*
			 * A BRICK THE SOLVER GAVE UP ON READS AS "stranded", AND IT MUST NOT READ AS
			 * "grounded".
			 *
			 * These are the two words in PresenterWordForSupport that are furthest apart in
			 * meaning: grounded is "this is resting on the earth", and stranded is "this is in
			 * a knot the solver could not route — the answer beside it is not a physical
			 * claim". Swapping the two arms is invisible to every other row here, and it makes
			 * a solver limitation wear a foundation's clothes. Integration.PullingSupportBrings-
			 * TheWallDown asserts no piece is Stranded at the moment of collapse for exactly
			 * this reason; a readout that cannot say the word is the same hole one layer out.
			 *
			 * The knot is a live piece with two real head joints, so this is a full readout
			 * rather than a fail-closed one — the support word is the only thing unusual
			 * about it.
			 */
			TEXT("a brick the solver stranded in a knot"),
			{ MakeRef(InspectorStructure, KnotXPiece) },
			MakeRef(InspectorStructure, KnotXPiece),
			{ MakeRef(InspectorStructure, KnotXPiece) },
			{ TEXT("brick 4:6") },
			{ true },
			TEXT("1 brick selected"),
			0,
			2,
			TEXT("stranded"),
			TEXT("2 joints")
		},
	};

	for (const FInspectorCase& Case : Cases)
	{
		const FPieceMenuInspector Inspector =
			BuildPieceMenuInspector(Binding, Case.Selected, Case.Inspected);

		CheckInspectorInvariants(*this, Inspector, Case.Description);

		/*
		 * THE COUNT, AND IT IS THE FIRST THING A PLAYER READS. It is asserted twice on
		 * purpose — as a number, which anything downstream can colour or threshold, and as
		 * the sentence the widget prints, because "1 brick" against "1 bricks" is a branch
		 * and a branch in the widget is untested by construction.
		 */
		TestEqual(
			FString::Printf(TEXT("%s: should report %d selected, it reports %d %s"),
				Case.Description, Case.Selected.Num(), Inspector.SelectedCount,
				*DescribeInspector(Inspector)),
			Inspector.SelectedCount, Case.Selected.Num());

		TestEqual(
			FString::Printf(TEXT("%s: should read '%s', it reads '%s'"),
				Case.Description, Case.ExpectedCountText, *Inspector.CountText),
			Inspector.CountText, FString(Case.ExpectedCountText));

		TestEqual(
			FString::Printf(TEXT("%s: should list %d entr(y/ies), it lists %d %s"),
				Case.Description, Case.ExpectedEntries.Num(), Inspector.Pieces.Num(),
				*DescribeInspector(Inspector)),
			Inspector.Pieces.Num(), Case.ExpectedEntries.Num());

		/*
		 * TABLE INTEGRITY, not a claim about the presenter: a row whose label list is short
		 * would silently skip the label assertion for the entries past its end.
		 */
		TestEqual(
			FString::Printf(TEXT("%s: the table row must name one label per expected entry"),
				Case.Description),
			Case.ExpectedLabels.Num(), Case.ExpectedEntries.Num());

		TestEqual(
			FString::Printf(TEXT("%s: the table row must name one liveness per expected entry"),
				Case.Description),
			Case.ExpectedLive.Num(), Case.ExpectedEntries.Num());

		if (Inspector.Pieces.Num() == Case.ExpectedEntries.Num())
		{
			for (int32 Index = 0; Index < Case.ExpectedEntries.Num(); ++Index)
			{
				const FInspectorPieceEntry& Entry = Inspector.Pieces[Index];

				/*
				 * THE LABEL IS "brick <StructureId>:<PieceIndex>", ALWAYS, AND THE RULE IS
				 * TOTAL RATHER THAN CONDITIONAL.
				 *
				 * This is a DEBUGGER, so unambiguous beats friendly. Two selected refs can
				 * carry the same piece index in different structures — the row above builds
				 * exactly that — and a label built from the piece index alone presents them as
				 * the identical string while one of them singles out nothing when clicked,
				 * which reads as a bug in the inspector rather than as two different bricks.
				 *
				 * Qualifying only the FOREIGN refs would need to know which structure is the
				 * home one, and the model has no such state: BuildPieceMenuInspector takes a
				 * binding and a list, and a selection may legitimately hold refs from several
				 * walls. Inventing that state to save four characters would be a branch at the
				 * exact seam whose whole job is telling two bricks apart.
				 *
				 * It is asserted per entry rather than through DescribeInspector, which prints
				 * the label in FAILURE MESSAGES ONLY — that is what made this string look
				 * covered while being the one string in the model nothing read back.
				 */
				if (Case.ExpectedLabels.IsValidIndex(Index))
				{
					TestEqual(
						FString::Printf(
							TEXT("%s: entry %d should read '%s', it reads '%s' %s"),
							Case.Description, Index, *Case.ExpectedLabels[Index], *Entry.Label,
							*DescribeInspector(Inspector)),
						Entry.Label, Case.ExpectedLabels[Index]);
				}

				TestTrue(
					*FString::Printf(
						TEXT("%s: entry %d should stand for {%d,%d}, it stands for {%d,%d} %s"),
						Case.Description, Index,
						Case.ExpectedEntries[Index].StructureId,
						Case.ExpectedEntries[Index].PieceIndex,
						Entry.Ref.StructureId, Entry.Ref.PieceIndex,
						*DescribeInspector(Inspector)),
					Entry.Ref == Case.ExpectedEntries[Index]);

				/*
				 * WHETHER THE ENTRY STILL NAMES A BRICK YOU CAN ACT ON, DECIDED HERE RATHER
				 * THAN BY WHOEVER DRAWS IT.
				 *
				 * Without this field a brick a cascade removed, a ref naming another wall and
				 * a perfectly live brick present identically — same label shape, same ref,
				 * same nothing — so a widget that wanted to grey the dead one out would have
				 * to resolve the ref against the binding itself. That is model logic in the
				 * one place the recorded widget exception says there may be none, at exactly
				 * the surface CURRENT_STATE.md's rank-0 product decision has to become legible
				 * on: a selection that outlived the bricks in it.
				 *
				 * NOTE WHERE THE EXPECTATIONS COME FROM: the fixture diagram, written out per
				 * row, NOT read back off the binding. Deriving them here the way the presenter
				 * derives them would agree with it however wrong it was.
				 */
				if (Case.ExpectedLive.IsValidIndex(Index))
				{
					TestTrue(
						*FString::Printf(
							TEXT("%s: entry %d {%d,%d} should read as %s, it reads as %s %s"),
							Case.Description, Index,
							Case.ExpectedEntries[Index].StructureId,
							Case.ExpectedEntries[Index].PieceIndex,
							Case.ExpectedLive[Index] ? TEXT("a live piece") : TEXT("naming nothing"),
							Entry.bIsLivePiece ? TEXT("a live piece") : TEXT("naming nothing"),
							*DescribeInspector(Inspector)),
						Entry.bIsLivePiece == Case.ExpectedLive[Index]);
				}

				TestTrue(
					*FString::Printf(
						TEXT("%s: entry %d should%s be the one singled out, it %s %s"),
						Case.Description, Index,
						Index == Case.ExpectedInspectedEntry ? TEXT("") : TEXT(" NOT"),
						Entry.bIsInspected ? TEXT("is") : TEXT("is not"),
						*DescribeInspector(Inspector)),
					Entry.bIsInspected == (Index == Case.ExpectedInspectedEntry));
			}
		}

		const bool bExpectInspected = Case.ExpectedInspectedEntry != INDEX_NONE;

		TestTrue(
			*FString::Printf(TEXT("%s: a brick should%s be singled out, it reports %s %s"),
				Case.Description, bExpectInspected ? TEXT("") : TEXT(" NOT"),
				Inspector.bHasInspectedPiece ? TEXT("yes") : TEXT("no"),
				*DescribeInspector(Inspector)),
			Inspector.bHasInspectedPiece == bExpectInspected);

		if (bExpectInspected)
		{
			TestTrue(
				*FString::Printf(TEXT("%s: it should single out {%d,%d}, it names {%d,%d}"),
					Case.Description,
					Case.Inspected.StructureId, Case.Inspected.PieceIndex,
					Inspector.InspectedRef.StructureId, Inspector.InspectedRef.PieceIndex),
				Inspector.InspectedRef == Case.Inspected);
		}

		TestEqual(
			FString::Printf(TEXT("%s: should break out %d joint(s), it broke out %d %s"),
				Case.Description, Case.ExpectedJointCount, Inspector.Joints.Num(),
				*DescribeInspector(Inspector)),
			Inspector.Joints.Num(), Case.ExpectedJointCount);

		TestEqual(
			FString::Printf(TEXT("%s: support should read '%s', it reads '%s'"),
				Case.Description, Case.ExpectedSupportText, *Inspector.SupportText),
			Inspector.SupportText, FString(Case.ExpectedSupportText));

		/*
		 * AND THE JOINT LIST GETS A SENTENCE, WHICH IS THE SAME RULE AS CountText ONE LEVEL
		 * DOWN: singular and plural are decided here, and an EMPTY list is its own wording
		 * rather than an absence. "No joints" against "0 joints" is the same choice as
		 * "No bricks selected" against "0 bricks selected", and both are branches — so both
		 * belong where a test can read them rather than in the widget.
		 */
		TestEqual(
			FString::Printf(TEXT("%s: the joint list should read '%s', it reads '%s' %s"),
				Case.Description, Case.ExpectedJointsText, *Inspector.JointsText,
				*DescribeInspector(Inspector)),
			Inspector.JointsText, FString(Case.ExpectedJointsText));
	}

	/*
	 * AND "NOBODY HAS SOLVED YET" IS ITS OWN SENTENCE, ON ITS OWN FIXTURE.
	 *
	 * EPieceSupport::Falling is both a real collapse and an absent answer, deliberately,
	 * because enumerator zero has to be the one that promises least. Up here the polarity
	 * inverts: a freshly built wall drawn as a column of falling bricks is a catastrophe
	 * reported that has not happened. The joints are still listed — they exist and they are
	 * carrying nothing yet — which is what keeps this different from "no brick inspected".
	 */
	{
		FStructureBinding Unsolved;
		BuildWorkedFixture(Unsolved, /*bSettle*/ false);

		const TArray<FPieceRef> JustTheSubject = { MakeRef(InspectorStructure, SubjectPiece) };

		const FPieceMenuInspector Inspector = BuildPieceMenuInspector(
			Unsolved, JustTheSubject, MakeRef(InspectorStructure, SubjectPiece));

		CheckInspectorInvariants(*this, Inspector, TEXT("a wall nobody has solved"));

		TestTrue(
			*FString::Printf(
				TEXT("a wall nobody has solved: its brick is still a brick and is singled out %s"),
				*DescribeInspector(Inspector)),
			Inspector.bHasInspectedPiece);

		TestEqual(
			FString::Printf(
				TEXT("a wall nobody has solved: support must NOT read 'falling', it reads '%s'"),
				*Inspector.SupportText),
			Inspector.SupportText, FString(TEXT("not solved yet")));

		TestEqual(
			FString::Printf(
				TEXT("a wall nobody has solved: its 3 joints still exist, %d were broken out %s"),
				Inspector.Joints.Num(), *DescribeInspector(Inspector)),
			Inspector.Joints.Num(), 3);

		/*
		 * AND THE JOINT COUNT IS A FACT ABOUT THE GRAPH, NOT ABOUT THE SOLVE. Nobody has
		 * asked what the wall is carrying, but the joints are there and there are three of
		 * them — so this sentence must not go quiet with the support word.
		 */
		TestEqual(
			FString::Printf(
				TEXT("a wall nobody has solved: its joint list should still read '3 joints', it reads '%s'"),
				*Inspector.JointsText),
			Inspector.JointsText, FString(TEXT("3 joints")));

		TestTrue(
			*FString::Printf(
				TEXT("a wall nobody has solved: its selected brick is still a live piece %s"),
				*DescribeInspector(Inspector)),
			Inspector.Pieces.Num() == 1 && Inspector.Pieces[0].bIsLivePiece);
	}

	return true;
}

/**
 * THE JOINT BREAKOUT IS InspectPiece'S ANSWER IN A HUMAN'S UNITS AND WORDS — SAME ROWS, SAME
 * ORDER, NEWTONS INSTEAD OF UNREAL FORCE UNITS, PER CENT INSTEAD OF A RATIO — AND NOTHING IN
 * IT IS RECOMPUTED.
 *
 * WHY EXACT EQUALITY AGAINST A LIVE InspectPiece CALL RATHER THAN AGAINST CONSTANTS. This
 * project has paid twice for a second derivation of one number (the half-bat mass, and the
 * break decision that needed GetConnectionUtilisation written specifically to stop a third
 * hand-copy), and a re-derivation agrees to nine decimal places forever and still differs in
 * the last bit. So the passthrough fields are held against the model's own answer with ==,
 * and the two converted ones against that answer times a factor this file spells out for
 * itself. Hand-derived physical values appear as fixture PRECONDITIONS on the graph, so a
 * wrong fixture says so instead of being absorbed into a wrong expectation.
 *
 * THE SEVERED JOINT IS THE DISCRIMINATOR, and it is why the fixture pulls a brick out. A
 * joint that has GIVEN is dropped from the solver's support lists before the tier is even
 * decided, so a presenter that built its own adjacency by walking those would show two rows
 * where three are due — every number on the two rows correct — and look entirely reasonable.
 * Only a brick with a gone joint separates "read InspectPiece" from "did it again".
 *
 * AND A GIVEN JOINT MUST NOT READ LIKE AN INTACT UNLOADED ONE. It carries nothing, so it is
 * 0 N at 0 % — identical to a healthy joint with nothing on it. One of those is a hole in
 * the wall. bHasGiven is carried for anything that wants to colour it, and the LINE the
 * widget prints says so in words, because a widget that had to branch on the flag would be
 * logic in the one place no test can reach.
 *
 * THE UNIT IS THE POINT OF HALF THIS TEST. 1 N = 100 uu, and the only named factor in Core
 * is ForceUnitsPerMPaSqCm = 10,000 — this factor times cm2-to-mm2 — so reaching for the
 * wrong one is a clean 100x that a tuned-looking readout hides perfectly.
 *
 * NEEDS A TICKING WORLD: no, and not even a world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuJointReadoutTest,
	"DestructionGame.Presenter.PieceMenuJointReadout",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuJointReadoutTest::RunTest(const FString& Parameters)
{
	using namespace PieceInspectorTestSupport;

	FStructureBinding Binding;
	BuildWorkedFixture(Binding, /*bSettle*/ true);

	/*
	 * FIXTURE PRECONDITIONS, HAND-DERIVED AND ASKED OF THE GRAPH.
	 *
	 * The rider weighs 3 kg, so conn 1 carries 3 x 980 = 2940 uu. The subject carries its
	 * own 2 kg plus the rider's 3, so conn 0 carries 5 x 980 = 4900 uu. Both are exactly
	 * antiparallel to an exactly vertical normal, so shear and tension are exactly zero and
	 * COMPRESSION is necessarily the worst axis — which is what makes the utilisation below
	 * a compression figure rather than whichever axis happened to win. Conn 2 was severed
	 * with the spare and carries nothing.
	 *
	 * These are green on arrival and drive nothing; they exist so a fixture that stopped
	 * being the diagram fails here rather than silently redefining what is being presented.
	 */
	const double PadJointUu = (SubjectMassKg + RiderMassKg) * InspectorGravityCmPerSecondSquared;
	const double RiderJointUu = RiderMassKg * InspectorGravityCmPerSecondSquared;

	TestEqual(
		FString::Printf(TEXT("fixture: the pad joint should carry %.6f uu"), PadJointUu),
		Binding.GetStructure().GetConnectionForce(PadJoint).Size(), PadJointUu);

	TestEqual(
		FString::Printf(TEXT("fixture: the rider joint should carry %.6f uu"), RiderJointUu),
		Binding.GetStructure().GetConnectionForce(RiderJoint).Size(), RiderJointUu);

	TestTrue(
		TEXT("fixture: the spare's joint went with it, so it must read as given"),
		Binding.GetStructure().GetConnection(SpareJoint).HasGiven());

	TestEqual(
		FString::Printf(TEXT("fixture: the pad joint's utilisation should be %.12f"),
			CompressionOnlyUtilisation(PadJointUu)),
		Binding.GetStructure().GetConnectionUtilisation(PadJoint),
		CompressionOnlyUtilisation(PadJointUu),
		1e-15);

	TestEqual(
		FString::Printf(TEXT("fixture: the rider joint's utilisation should be %.12f"),
			CompressionOnlyUtilisation(RiderJointUu)),
		Binding.GetStructure().GetConnectionUtilisation(RiderJoint),
		CompressionOnlyUtilisation(RiderJointUu),
		1e-15);

	/*
	 * THE MODEL'S OWN ANSWER, TAKEN ONCE. Everything below is asserted against THIS rather
	 * than against a constant, which is what makes "not recomputed" the claim under test.
	 */
	const FPieceRef SubjectRef = MakeRef(InspectorStructure, SubjectPiece);
	const FPieceInspection Inspection = InspectPiece(Binding, SubjectRef);

	TestEqual(
		FString::Printf(TEXT("fixture: InspectPiece should find 3 joints on the subject, it found %d"),
			Inspection.Joints.Num()),
		Inspection.Joints.Num(), 3);

	if (Inspection.Joints.Num() != 3)
	{
		return true;
	}

	const TArray<FPieceRef> JustTheSubject = { SubjectRef };

	const FPieceMenuInspector Inspector =
		BuildPieceMenuInspector(Binding, JustTheSubject, SubjectRef);

	CheckInspectorInvariants(*this, Inspector, TEXT("the subject's breakout"));

	TestEqual(
		FString::Printf(TEXT("the breakout should have one row per joint InspectPiece found (%d), it has %d %s"),
			Inspection.Joints.Num(), Inspector.Joints.Num(), *DescribeInspector(Inspector)),
		Inspector.Joints.Num(), Inspection.Joints.Num());

	if (Inspector.Joints.Num() != Inspection.Joints.Num())
	{
		return true;
	}

	for (int32 Index = 0; Index < Inspection.Joints.Num(); ++Index)
	{
		const FJointInspection& Model = Inspection.Joints[Index];
		const FInspectorJointRow& Row = Inspector.Joints[Index];

		/*
		 * ASCENDING CONNECTION ORDER, PASSED THROUGH. A debugger's list must not reshuffle
		 * between two looks at the same brick, and connection order is the only stable order
		 * there is — so this is one comparison for identity and ordering together.
		 */
		TestEqual(
			FString::Printf(TEXT("row %d should be connection %d, it is %d %s"),
				Index, Model.ConnectionIndex, Row.ConnectionIndex, *DescribeInspector(Inspector)),
			Row.ConnectionIndex, Model.ConnectionIndex);

		TestEqual(
			FString::Printf(TEXT("row %d's far end should be piece %d, it is %d"),
				Index, Model.OtherPieceIndex, Row.OtherPieceIndex),
			Row.OtherPieceIndex, Model.OtherPieceIndex);

		TestTrue(
			*FString::Printf(TEXT("row %d's tier should be %s, it is %s"),
				Index, NameOfRole(Model.Role), NameOfRole(Row.Role)),
			Row.Role == Model.Role);

		TestTrue(
			*FString::Printf(TEXT("row %d should be %s, it is %s"),
				Index,
				Model.bHasGiven ? TEXT("given") : TEXT("intact"),
				Row.bHasGiven ? TEXT("given") : TEXT("intact")),
			Row.bHasGiven == Model.bHasGiven);

		TestEqual(
			FString::Printf(TEXT("row %d's break pass should be %d, it is %d"),
				Index, Model.BreakPass, Row.BreakPass),
			Row.BreakPass, Model.BreakPass);

		/*
		 * THE TWO CONVERTED FIELDS, HELD WITH EXACT EQUALITY AGAINST THE MODEL'S OWN NUMBER.
		 *
		 * Exact rather than nearly-equal because that is the whole discipline: a presenter
		 * that recomputed the utilisation from the connection and the force would agree to
		 * about fifteen decimal places and differ in the last bit, and a tolerance is
		 * precisely what would let that through. The spelling this requires is
		 * `Force.Size() / ForceUnitsPerNewton` and `Utilisation * 100.0`; a multiply by
		 * 0.01 for the first would be an ulp out, because 0.01 is not representable while
		 * the correctly-rounded quotient is what a decimal reading of the answer gives.
		 */
		const double ExpectedForceN = Model.ForceUu.Size() / InspectorForceUnitsPerNewton;

		TestTrue(
			*FString::Printf(
				TEXT("row %d should read %.9f N — %.6f uu at 100 uu per newton — it reads %.9f"),
				Index, ExpectedForceN, Model.ForceUu.Size(), Row.ForceN),
			Row.ForceN == ExpectedForceN);

		const double ExpectedPercent = Model.Utilisation * 100.0;

		TestTrue(
			*FString::Printf(
				TEXT("row %d should read %.12f %% — the solver's own ratio %.12f — it reads %.12f"),
				Index, ExpectedPercent, Model.Utilisation, Row.UtilisationPercent),
			Row.UtilisationPercent == ExpectedPercent);
	}

	/*
	 * AND THE LINES THEMSELVES, WHICH ARE WHAT A PLAYER ACTUALLY READS.
	 *
	 * THE WORDING IS PINNED EXACTLY, AND THAT COST IS DELIBERATE. Every string a widget
	 * prints has to be decided where a test can reach it — the widget was landed under a
	 * recorded exception on precisely that condition — so retuning the wording is a test
	 * edit rather than an untested change. What each line must contain is not free: which
	 * joint, which neighbour, which tier, and then EITHER the load or the reason there is
	 * none — an intact joint and a gone one are 0 N at 0 % alike, so the two must be
	 * different sentences rather than a branch in the widget. The block below this one says
	 * why the third state of DESIGN.md's table is not pinned here.
	 *
	 * The numbers inside are the ones asserted above, rendered at 1 decimal place for
	 * newtons and 3 for per cent — 49 N reads as 49.0 N, and 0.049 % keeps a figure at the
	 * scale a settled wall actually sits at.
	 */
	const TArray<FString> ExpectedLines = {
		TEXT("#0  brick 0  bed below  49.0 N  0.049 %"),
		TEXT("#1  brick 2  bed above  29.4 N  0.029 %"),
		TEXT("#2  brick 3  head  broken (went with a removed piece)"),
	};

	for (int32 Index = 0; Index < ExpectedLines.Num() && Index < Inspector.Joints.Num(); ++Index)
	{
		TestEqual(
			FString::Printf(TEXT("joint line %d should read '%s', it reads '%s'"),
				Index, *ExpectedLines[Index], *Inspector.Joints[Index].Text),
			Inspector.Joints[Index].Text, ExpectedLines[Index]);
	}

	/*
	 * THE THIRD SENTENCE — "broke in pass N" — IS DELIBERATELY NOT ASSERTED HERE, BECAUSE IT
	 * IS NOT REACHABLE THROUGH A BINDING TODAY.
	 *
	 * DESIGN.md's table has three states and this file only reaches two of them. Breaking a
	 * joint under load needs FStructure::SolveAndBreak, which FStructureBinding does not
	 * expose — the cascade is not on the world wire at all yet, and that is CURRENT_STATE.md's
	 * item 1. So a binding cannot be brought into the state where a joint carries a pass
	 * number, and asserting the wording of a sentence nothing can produce would be writing
	 * the test for a later slice against a fixture that cannot exist.
	 *
	 * WHAT IS STILL COVERED IS THE FIELD: BreakPass is asserted above to be InspectPiece's
	 * own answer on every row, so when the cascade does reach a binding the number is already
	 * arriving at the presenter, and only the sentence is left to write. The distinction
	 * itself is pinned one layer down, by PieceInspection.JointBreakout.
	 */

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
