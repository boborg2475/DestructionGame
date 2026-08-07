// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/PieceMenu.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/StructureBinding.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges many
 * files into one. See CURRENT_STATE.md; the `using namespace` lives inside RunTest for the same
 * reason. The worked fixture is deliberately NOT shared with PieceInspectorTest.cpp: that one
 * lives in a .cpp rather than a header, and reaching into it would only work by accident of the
 * unity build.
 */
namespace PieceMenuCompactTestSupport
{
	using namespace DestructionProfiles;

	/** The structure this fixture identifies itself as, and one it does not. */
	constexpr int32 CompactStructure = 4;
	constexpr int32 CompactOtherStructure = 9;

	/** An ordinary bed- or head-joint face. */
	constexpr double CompactJointAreaSqCm = 100.0;

	/*
	 * THE FIXTURE, AND EVERY PIECE IN IT IS HERE TO PUT A DIFFERENT WORD IN THE SUPPORT COLUMN.
	 *
	 *                        [2] Rider  3 kg
	 *                         |  conn 1   bed joint ABOVE the subject
	 *      Spare [3] ~ ~ ~ ~ [1] Subject 2 kg          <- the brick singled out, three joints
	 *      (removed) conn 2   |  head joint, severed when the spare was pulled
	 *                         |  conn 0   bed joint BENEATH the subject
	 *                        [0] Pad    grounded, 10 kg
	 *
	 *      [4] Floater 4 kg — no joints at all, so the solve finds nothing holding it up.
	 *
	 * THE SUBJECT NEEDS THREE JOINTS AND THE REASON IS NOT DECORATION. A compact mode is only
	 * observably different from a full one where the full one has a joint table to drop, and the
	 * joint table is also what carries the colour slots the brick highlights are keyed on. One
	 * joint would make "the rows did not get renumbered" a claim about a single row.
	 *
	 * AND THE FIVE ENTRIES MUST NOT ALL READ THE SAME. If every selected brick came back
	 * "supported", an implementation that filled the compact entry list with copies of the first
	 * row would satisfy every equality below. The bands are asserted to be plural as a fixture
	 * precondition rather than assumed, because a retuned solver could quietly flatten them.
	 */
	constexpr int32 PadPiece = 0;
	constexpr int32 SubjectPiece = 1;
	constexpr int32 RiderPiece = 2;
	constexpr int32 SparePiece = 3;
	constexpr int32 FloaterPiece = 4;

	constexpr double PadMassKg = 10.0;
	constexpr double SubjectMassKg = 2.0;
	constexpr double RiderMassKg = 3.0;
	constexpr double SpareMassKg = 1.0;
	constexpr double FloaterMassKg = 4.0;

	/** Straight up: a bed joint, which bears in compression. */
	const FVector CompactBedNormal(0.0, 0.0, 1.0);

	/** Straight sideways: a head joint, which can only carry in shear. */
	const FVector CompactHeadNormal(1.0, 0.0, 0.0);

	/*
	 * THE WALL'S OWN GRID, transcribed from DestructionLayout::RunningBond rather than invented:
	 * a 21.5 x 10.25 x 6.5 cm brick on a 1.0 cm joint gives a 22.5 cm brick pitch and a 7.5 cm
	 * course pitch, with the bottom course centred at half a brick height. The entry labels the
	 * agreement claims compare are composed from these boxes, so a fixture on a made-up grid
	 * would be comparing two labels that no wall ever produces.
	 */
	constexpr double CompactCoursePitchCm = 7.5;
	constexpr double CompactFirstCourseZCm = 3.25;
	constexpr double CompactBrickPitchCm = 22.5;

	DestructionLayout::FPieceBox CompactBoxAt(double CentreXCm, double CentreZCm)
	{
		DestructionLayout::FPieceBox Box;
		Box.CentreCm = FVector(CentreXCm, 0.0, CentreZCm);
		Box.ExtentCm = FVector(10.75, 5.125, 3.25);
		return Box;
	}

	double CompactCourseZ(int32 Course)
	{
		return CompactFirstCourseZCm + Course * CompactCoursePitchCm;
	}

	DestructionLayout::FPieceBox CompactBoxFor(int32 Index)
	{
		switch (Index)
		{
		case PadPiece:     return CompactBoxAt(0.0, CompactCourseZ(0));
		case SubjectPiece: return CompactBoxAt(0.0, CompactCourseZ(1));
		case RiderPiece:   return CompactBoxAt(0.0, CompactCourseZ(2));
		case SparePiece:   return CompactBoxAt(CompactBrickPitchCm, CompactCourseZ(1));
		case FloaterPiece: return CompactBoxAt(CompactBrickPitchCm * 2.0, CompactCourseZ(3));
		}

		return CompactBoxAt(0.0, 0.0);
	}

	FPieceRef MakeRef(int32 StructureId, int32 PieceIndex)
	{
		FPieceRef Ref;
		Ref.StructureId = StructureId;
		Ref.PieceIndex = PieceIndex;
		return Ref;
	}

	void AddJoint(FStructureBinding& Out, int32 PieceA, int32 PieceB, const FVector& Normal)
	{
		FConnection Connection;
		Connection.PieceA = PieceA;
		Connection.PieceB = PieceB;
		Connection.InterfaceNormal = Normal;
		Connection.InterfaceAreaSqCm = CompactJointAreaSqCm;
		Connection.Strength = GeneralPurposeMortar;
		Out.AddConnection(Connection);
	}

	/**
	 * The diagram above, built and settled.
	 *
	 * A NULL ACTOR IS FINE HERE. Nothing in this file releases through an actor, resolves one or
	 * destroys one — ApplyResults latches a flag on the binding and needs no UObject.
	 */
	void BuildCompactFixture(FStructureBinding& Out)
	{
		Out.StructureId = CompactStructure;

		Out.AddPiece(PadMassKg, /*bIsGrounded*/ true, nullptr, CompactBoxFor(PadPiece));
		Out.AddPiece(SubjectMassKg, false, nullptr, CompactBoxFor(SubjectPiece));
		Out.AddPiece(RiderMassKg, false, nullptr, CompactBoxFor(RiderPiece));
		Out.AddPiece(SpareMassKg, false, nullptr, CompactBoxFor(SparePiece));
		Out.AddPiece(FloaterMassKg, false, nullptr, CompactBoxFor(FloaterPiece));

		AddJoint(Out, PadPiece, SubjectPiece, CompactBedNormal);
		AddJoint(Out, SubjectPiece, RiderPiece, CompactBedNormal);
		AddJoint(Out, SubjectPiece, SparePiece, CompactHeadNormal);

		/* Pulled before the solve, which SEVERS conn 2 without it ever having failed. */
		Out.RemovePiece(SparePiece);

		Out.SolveLoads();
		Out.ApplyResults();
	}

	const TCHAR* NameOfSupportBand(EPieceSupportBand Band)
	{
		switch (Band)
		{
		case EPieceSupportBand::NotAPiece: return TEXT("NotAPiece");
		case EPieceSupportBand::NotSolved: return TEXT("NotSolved");
		case EPieceSupportBand::Falling:   return TEXT("Falling");
		case EPieceSupportBand::Stranded:  return TEXT("Stranded");
		case EPieceSupportBand::Supported: return TEXT("Supported");
		case EPieceSupportBand::Grounded:  return TEXT("Grounded");
		}

		return TEXT("<not a support band>");
	}

	FString DescribeEntries(const FPieceMenuInspector& Inspector)
	{
		if (Inspector.Pieces.Num() == 0)
		{
			return TEXT("<no entries>");
		}

		FString Line;

		for (int32 Index = 0; Index < Inspector.Pieces.Num(); ++Index)
		{
			const FInspectorPieceEntry& Entry = Inspector.Pieces[Index];

			Line += FString::Printf(
				TEXT("%s{%d,%d '%s' %s%s %s}"),
				Index == 0 ? TEXT("") : TEXT(", "),
				Entry.Ref.StructureId, Entry.Ref.PieceIndex, *Entry.Label,
				Entry.bIsInspected ? TEXT("inspected ") : TEXT(""),
				Entry.bIsLivePiece ? TEXT("live") : TEXT("dead"),
				NameOfSupportBand(Entry.SupportBand));
		}

		return Line;
	}

	FString DescribeJoints(const FPieceMenuInspector& Inspector)
	{
		if (Inspector.Joints.Num() == 0)
		{
			return TEXT("<no joint rows>");
		}

		FString Line;

		for (int32 Index = 0; Index < Inspector.Joints.Num(); ++Index)
		{
			const FInspectorJointRow& Row = Inspector.Joints[Index];

			Line += FString::Printf(
				TEXT("%s#%d->piece %d slot %d '%s'"),
				Index == 0 ? TEXT("") : TEXT(", "),
				Row.ConnectionIndex, Row.OtherPieceIndex, Row.ColourSlot, *Row.Text);
		}

		return Line;
	}

	/** One row of the table: what is picked, which brick is singled out, and what to call it. */
	struct FCompactCase
	{
		const TCHAR* Description;
		TArray<FPieceRef> Selected;
		FPieceRef InspectedRef;
	};

	/**
	 * EVERYTHING THE COMPACT PANEL STILL SHOWS, HELD AGAINST THE FULL ONE FIELD BY FIELD.
	 *
	 * A FUNCTION RATHER THAN A BLOCK IN THE LOOP because the same comparison is made twice — once
	 * between the two DETAIL MODES, and once between two FULL builds taken either side of a
	 * compact one. The second is what says asking for compact does not disturb what the rest of
	 * the game reads: NeighbourHighlightForPiece walks Inspector.Joints and colours a brick in the
	 * wall from Row.ColourSlot, so a compact build that renumbered the slots, reordered the rows
	 * or shortened the entry list in place would move the highlights off the bricks they name.
	 */
	void CheckInspectorsAgree(
		FAutomationTestBase& Test,
		const FPieceMenuInspector& Expected,
		const FPieceMenuInspector& Actual,
		const TCHAR* Description,
		const TCHAR* What)
	{
		Test.TestEqual(
			FString::Printf(
				TEXT("%s: %s must keep the heading '%s', it reads '%s'"),
				Description, What, *Expected.HeaderText, *Actual.HeaderText),
			Actual.HeaderText, Expected.HeaderText);

		Test.TestEqual(
			FString::Printf(
				TEXT("%s: %s must count the same %d brick(s), it counts %d"),
				Description, What, Expected.SelectedCount, Actual.SelectedCount),
			Actual.SelectedCount, Expected.SelectedCount);

		Test.TestEqual(
			FString::Printf(
				TEXT("%s: %s must say '%s', it says '%s'"),
				Description, What, *Expected.CountText, *Actual.CountText),
			Actual.CountText, Expected.CountText);

		/*
		 * WHICH BRICK IS SINGLED OUT SURVIVES THE MODE. The entry row's own marker, the readout's
		 * heading and the ref itself are what tie the panel to the magenta brick in the wall, and
		 * a mode that dropped the table is not a mode that stopped pointing at a brick.
		 */
		Test.TestEqual(
			FString::Printf(
				TEXT("%s: %s must still %s a brick singled out"),
				Description, What,
				Expected.bHasInspectedPiece ? TEXT("have") : TEXT("have no")),
			Actual.bHasInspectedPiece, Expected.bHasInspectedPiece);

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: %s must single out {%d,%d}, it singles out {%d,%d}"),
				Description, What,
				Expected.InspectedRef.StructureId, Expected.InspectedRef.PieceIndex,
				Actual.InspectedRef.StructureId, Actual.InspectedRef.PieceIndex),
			Actual.InspectedRef == Expected.InspectedRef);

		Test.TestEqual(
			FString::Printf(
				TEXT("%s: %s must name its subject '%s', it names '%s'"),
				Description, What, *Expected.InspectedLabel, *Actual.InspectedLabel),
			Actual.InspectedLabel, Expected.InspectedLabel);

		Test.TestEqual(
			FString::Printf(
				TEXT("%s: %s must carry the same hint '%s', it carries '%s'"),
				Description, What, *Expected.InspectedHintText, *Actual.InspectedHintText),
			Actual.InspectedHintText, Expected.InspectedHintText);

		Test.TestEqual(
			FString::Printf(
				TEXT("%s: %s must still say why the brick stands ('%s'), it says '%s'"),
				Description, What, *Expected.SupportText, *Actual.SupportText),
			Actual.SupportText, Expected.SupportText);

		Test.TestEqual(
			FString::Printf(
				TEXT("%s: %s must keep the readout's support band %s, it is %s"),
				Description, What,
				NameOfSupportBand(Expected.SupportBand), NameOfSupportBand(Actual.SupportBand)),
			NameOfSupportBand(Actual.SupportBand), NameOfSupportBand(Expected.SupportBand));

		/*
		 * AND THE ONE-LINE SUMMARY OF THE JOINTS, WHICH IS NOT PART OF THE TABLE.
		 *
		 * "3 joints" IS A SENTENCE ABOUT THE BRICK RATHER THAN A ROW OF THE TABLE, and it is most
		 * of what a compact readout is for: it is the one line that says the table exists to be
		 * opened. It is also counted off a joint list, so a compact mode that trimmed the list the
		 * SENTENCE is counted from — rather than only the rows drawn under it — reports a brick
		 * with three neighbours as having none. That reads as a fact rather than as an absence,
		 * which is the class of quiet wrongness this presenter keeps closing. Proved to bite: a
		 * compact build counting the trimmed list turns this row red with "No joints".
		 */
		Test.TestEqual(
			FString::Printf(
				TEXT("%s: %s must still summarise the joints as '%s', it says '%s'"),
				Description, What, *Expected.JointsText, *Actual.JointsText),
			Actual.JointsText, Expected.JointsText);

		Test.TestEqual(
			FString::Printf(
				TEXT("%s: %s must list the same %d entr(y/ies), it lists %d [%s]"),
				Description, What, Expected.Pieces.Num(), Actual.Pieces.Num(),
				*DescribeEntries(Actual)),
			Actual.Pieces.Num(), Expected.Pieces.Num());

		if (Actual.Pieces.Num() != Expected.Pieces.Num())
		{
			return;
		}

		for (int32 Index = 0; Index < Expected.Pieces.Num(); ++Index)
		{
			const FInspectorPieceEntry& Want = Expected.Pieces[Index];
			const FInspectorPieceEntry& Got = Actual.Pieces[Index];

			/*
			 * THE REF AT THIS INDEX, WHICH IS THE ORDERING CLAIM. Entries are in selection order
			 * and are never reordered or deduplicated, and the row a player's cursor is on is
			 * identified by its position — a list that came back with the same members in a
			 * different order would hover a different brick under the same pointer.
			 */
			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: %s entry %d must still be {%d,%d}, it is {%d,%d} [%s]"),
					Description, What, Index,
					Want.Ref.StructureId, Want.Ref.PieceIndex,
					Got.Ref.StructureId, Got.Ref.PieceIndex, *DescribeEntries(Actual)),
				Got.Ref == Want.Ref);

			Test.TestEqual(
				FString::Printf(
					TEXT("%s: %s entry %d must still read '%s', it reads '%s'"),
					Description, What, Index, *Want.Label, *Got.Label),
				Got.Label, Want.Label);

			Test.TestEqual(
				FString::Printf(
					TEXT("%s: %s entry %d must be marked %s"),
					Description, What, Index,
					Want.bIsInspected ? TEXT("as the singled-out one") : TEXT("as an ordinary row")),
				Got.bIsInspected, Want.bIsInspected);

			Test.TestEqual(
				FString::Printf(
					TEXT("%s: %s entry %d must read as a %s piece"),
					Description, What, Index, Want.bIsLivePiece ? TEXT("live") : TEXT("dead")),
				Got.bIsLivePiece, Want.bIsLivePiece);

			Test.TestEqual(
				FString::Printf(
					TEXT("%s: %s entry %d must still say '%s', it says '%s'"),
					Description, What, Index, *Want.SupportText, *Got.SupportText),
				Got.SupportText, Want.SupportText);

			/*
			 * AND THE BUCKET BESIDE THE WORD. The row's dot is coloured from this and the word is
			 * read from the field above it; a mode that kept one and defaulted the other would
			 * draw a grounded brick with the colour this game uses for a falling one.
			 */
			Test.TestEqual(
				FString::Printf(
					TEXT("%s: %s entry %d must keep the band %s, it is %s"),
					Description, What, Index,
					NameOfSupportBand(Want.SupportBand), NameOfSupportBand(Got.SupportBand)),
				NameOfSupportBand(Got.SupportBand), NameOfSupportBand(Want.SupportBand));
		}
	}

	/** The joint table, row for row, including the colour slot the brick highlights key on. */
	void CheckJointsAgree(
		FAutomationTestBase& Test,
		const FPieceMenuInspector& Expected,
		const FPieceMenuInspector& Actual,
		const TCHAR* Description,
		const TCHAR* What)
	{
		Test.TestEqual(
			FString::Printf(
				TEXT("%s: %s must break out the same %d joint row(s), it broke out %d [%s]"),
				Description, What, Expected.Joints.Num(), Actual.Joints.Num(),
				*DescribeJoints(Actual)),
			Actual.Joints.Num(), Expected.Joints.Num());

		if (Actual.Joints.Num() != Expected.Joints.Num())
		{
			return;
		}

		for (int32 Index = 0; Index < Expected.Joints.Num(); ++Index)
		{
			const FInspectorJointRow& Want = Expected.Joints[Index];
			const FInspectorJointRow& Got = Actual.Joints[Index];

			Test.TestEqual(
				FString::Printf(
					TEXT("%s: %s joint row %d must still be connection #%d, it is #%d"),
					Description, What, Index, Want.ConnectionIndex, Got.ConnectionIndex),
				Got.ConnectionIndex, Want.ConnectionIndex);

			Test.TestEqual(
				FString::Printf(
					TEXT("%s: %s joint row %d must still name piece %d at the far end, it names %d"),
					Description, What, Index, Want.OtherPieceIndex, Got.OtherPieceIndex),
				Got.OtherPieceIndex, Want.OtherPieceIndex);

			Test.TestEqual(
				FString::Printf(
					TEXT("%s: %s joint row %d must keep colour slot %d, it has %d"),
					Description, What, Index, Want.ColourSlot, Got.ColourSlot),
				Got.ColourSlot, Want.ColourSlot);

			Test.TestEqual(
				FString::Printf(
					TEXT("%s: %s joint row %d must still read '%s', it reads '%s'"),
					Description, What, Index, *Want.Text, *Got.Text),
				Got.Text, Want.Text);
		}
	}
}

/**
 * A COMPACT READOUT DROPS THE PER-JOINT TABLE AND THE HEADROOM SCALE THAT LABELS IT, AND AGREES
 * WITH THE FULL READOUT ON EVERY OTHER THING IT SHOWS.
 *
 * WHY THIS IS A PRESENTER TEST RATHER THAN A LAYOUT ONE. The player's complaint is that the panel
 * "takes up so much of the screen" — 640 px of a 1920 px viewport, full height. Almost all of that
 * is the joint table: a joint line is the widest sentence the readout composes (the panel's width
 * was set BY the longest of them, and is asserted against it) and there is one per joint, while
 * the entries and their support words are a fixed handful of short lines. So the cut that buys the
 * screen back is dropping the table, and WHICH FIELDS ARE DROPPED is a decision — a decision that
 * in Slate would be a collapsed slot in the one place no test can reach, still paying to build
 * every row it then hid.
 *
 * THE ASSERTION IS THE PAIR, NOT THE ABSENCE. "Compact has no joint rows" alone is satisfied by a
 * function that returns a default-constructed inspector, which is a panel that has gone blank
 * rather than one that has got smaller. So each case builds both modes off the same binding and
 * holds them against each other field by field, and only the two arrays and the caption that
 * belongs to them are allowed to differ.
 *
 * AND THE FULL MODE IS MEASURED EITHER SIDE OF A COMPACT BUILD. NeighbourHighlightForPiece walks
 * Inspector.Joints and colours a brick in the wall from the row's ColourSlot, and NeighbourPieces
 * turns the same rows into refs — so the joint table is not only a readout, it is the index the
 * brick highlights are keyed on. A compact mode that renumbered a slot, reordered a row or
 * shortened the entry list would move a highlight onto a brick that no row names, and nothing on
 * screen would say so.
 *
 * A TABLE OF SELECTIONS RATHER THAN ONE SCENARIO, so the agreement claim covers the states the
 * panel actually sits in: a brick singled out with a table under it, a brick with one joint,
 * nothing singled out, nothing picked at all, and a singled-out ref the player has deselected.
 * The rows that have no joint table in EITHER mode still assert agreement — they are what says
 * compact is not quietly a different presenter.
 *
 * NEEDS A TICKING WORLD: no, and not even a world. A binding is a plain struct; there is no actor,
 * no viewport and no solver tick anywhere in this file.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuCompactTest,
	"DestructionGame.Presenter.PieceMenuCompact",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuCompactTest::RunTest(const FString& Parameters)
{
	using namespace PieceMenuCompactTestSupport;

	FStructureBinding Binding;
	BuildCompactFixture(Binding);

	const FPieceRef PadRef = MakeRef(CompactStructure, PadPiece);
	const FPieceRef SubjectRef = MakeRef(CompactStructure, SubjectPiece);
	const FPieceRef SpareRef = MakeRef(CompactStructure, SparePiece);
	const FPieceRef FloaterRef = MakeRef(CompactStructure, FloaterPiece);
	const FPieceRef ForeignRef = MakeRef(CompactOtherStructure, PadPiece);

	/*
	 * FIVE PICKED BRICKS THAT DO NOT ALL READ THE SAME: the subject is held up, the pad is on the
	 * earth, the floater is not held up at all, the spare was pulled out from under everything and
	 * the last one names another wall entirely.
	 */
	const TArray<FPieceRef> WorkedSelection = {
		SubjectRef, PadRef, FloaterRef, SpareRef, ForeignRef };

	const TArray<FCompactCase> Cases = {
		{
			TEXT("the worked selection with the subject singled out"),
			WorkedSelection,
			SubjectRef
		},
		{
			TEXT("the same selection with the grounded pad singled out"),
			WorkedSelection,
			PadRef
		},
		{
			TEXT("the same selection with nothing singled out"),
			WorkedSelection,
			FPieceRef()
		},
		{
			TEXT("nothing picked at all"),
			TArray<FPieceRef>(),
			FPieceRef()
		},
		{
			/* An anchor outside the set it anchors singles out nothing, in either mode. */
			TEXT("a singled-out brick the player has since deselected"),
			TArray<FPieceRef>{ PadRef },
			SubjectRef
		},
	};

	int32 WidestJointTable = 0;

	for (const FCompactCase& Case : Cases)
	{
		const FPieceMenuInspector Full = BuildPieceMenuInspector(
			Binding, Case.Selected, Case.InspectedRef, EPieceMenuDetail::Full);

		const FPieceMenuInspector Compact = BuildPieceMenuInspector(
			Binding, Case.Selected, Case.InspectedRef, EPieceMenuDetail::Compact);

		const FPieceMenuInspector FullAgain = BuildPieceMenuInspector(
			Binding, Case.Selected, Case.InspectedRef, EPieceMenuDetail::Full);

		WidestJointTable = FMath::Max(WidestJointTable, Full.Joints.Num());

		/*
		 * THE CUT ITSELF. The table goes, and so do the two fields that exist only to explain the
		 * bars in it — FPieceMenuInspector already promises the caption and its ticks are empty
		 * when no bar is drawn, so a caption surviving a table that did not is the panel labelling
		 * something it is no longer showing.
		 */
		TestEqual(
			FString::Printf(
				TEXT("%s: the compact readout must break out no joint rows, it broke out %d [%s]"),
				Case.Description, Compact.Joints.Num(), *DescribeJoints(Compact)),
			Compact.Joints.Num(), 0);

		TestEqual(
			FString::Printf(
				TEXT("%s: the compact readout must draw no headroom scale, it carries %d tick(s)"),
				Case.Description, Compact.HeadroomScale.Num()),
			Compact.HeadroomScale.Num(), 0);

		TestTrue(
			*FString::Printf(
				TEXT("%s: the compact readout must caption no bar, it says '%s'"),
				Case.Description, *Compact.HeadroomCaption),
			Compact.HeadroomCaption.IsEmpty());

		/* And everything compact still shows reads exactly as the full panel reads it. */
		CheckInspectorsAgree(
			*this, Full, Compact, Case.Description, TEXT("the compact readout"));

		/*
		 * AND ASKING FOR COMPACT CHANGED NOTHING ABOUT THE FULL ANSWER. Both halves are checked,
		 * because the highlight tie-in reads the entries AND the joint rows.
		 */
		CheckInspectorsAgree(
			*this, Full, FullAgain, Case.Description, TEXT("the full readout, rebuilt"));

		CheckJointsAgree(
			*this, Full, FullAgain, Case.Description, TEXT("the full readout, rebuilt"));
	}

	/*
	 * FIXTURE: THE FULL READOUT HAD A TABLE TO DROP. Every "compact has no joint rows" claim above
	 * is free on a fixture whose bricks have no joints, and a wall retuned so that the subject
	 * lost its neighbours would make this whole file pass over a compact mode that does nothing.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("fixture: some case must break out at least 3 joint rows in FULL detail for compact to be dropping anything, the most any case broke out is %d"),
			WidestJointTable),
		WidestJointTable >= 3);

	/*
	 * FIXTURE: AND THE ENTRY ROWS DO NOT ALL SAY THE SAME THING. The agreement sweep compares the
	 * compact list against the full one entry by entry; if every entry were identical, a compact
	 * build that filled the list with copies of one row would satisfy all of it.
	 */
	{
		const FPieceMenuInspector Full = BuildPieceMenuInspector(
			Binding, WorkedSelection, SubjectRef, EPieceMenuDetail::Full);

		TSet<uint8> Bands;
		TSet<FString> Labels;

		for (const FInspectorPieceEntry& Entry : Full.Pieces)
		{
			Bands.Add(static_cast<uint8>(Entry.SupportBand));
			Labels.Add(Entry.Label);
		}

		TestTrue(
			*FString::Printf(
				TEXT("fixture: the picked bricks must land in at least 3 different support bands for the band comparison to bite, they land in %d [%s]"),
				Bands.Num(), *DescribeEntries(Full)),
			Bands.Num() >= 3);

		TestEqual(
			FString::Printf(
				TEXT("fixture: the %d picked bricks must read as %d different labels for the ordering comparison to bite, they read as %d [%s]"),
				Full.Pieces.Num(), Full.Pieces.Num(), Labels.Num(), *DescribeEntries(Full)),
			Labels.Num(), Full.Pieces.Num());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
