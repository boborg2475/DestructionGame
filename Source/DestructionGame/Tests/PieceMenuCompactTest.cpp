// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/PieceMenu.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/StructureBinding.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Uniquely named, not anonymous: a unity build merges files. The fixture is not shared with
 * PieceInspectorTest.cpp, whose copy would only be reachable by unity accident.
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
	 * The fixture: every piece puts a different word in the support column.
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
	 * The subject has three joints so full mode has a joint table to drop, and the five entries read
	 * differently so a list of copies cannot pass.
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
	 * RunningBond's grid (22.5 cm brick pitch, 7.5 cm course pitch), so the labels composed from
	 * these boxes are ones a real wall produces.
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

	/** The diagram above, built and settled. Null actors suffice: ApplyResults only latches a flag. */
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

		// Pulled before the solve, severing conn 2 without it failing.
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
	 * Everything the compact panel still shows, compared field by field. Also used between two full
	 * builds either side of a compact one, since highlights key on the full readout.
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

		// Which brick is singled out survives the mode.
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
		 * The one-line joint summary is not part of the table. Trimming the list it counts from would
		 * report "No joints" for a brick with three.
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

			// Order matters: the cursor identifies a row by position.
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

			// The band colours the row's dot; it must match the word above.
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
 * A compact readout drops the per-joint table and its headroom scale, and agrees with the full
 * readout on everything else. A presenter test: the joint table is most of the panel's width.
 *
 * Both modes are built from the same binding and compared field by field, since "no joint rows"
 * alone would pass on a blank inspector. The full mode is also rebuilt after a compact build, since
 * highlights key on its joint rows and colour slots. No world needed.
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

	// Five picks that read differently: supported, grounded, falling, removed, foreign.
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
			// An anchor outside the selection singles out nothing, in either mode.
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

		// The table goes, and so do the scale and caption that only label it.
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

		CheckInspectorsAgree(
			*this, Full, Compact, Case.Description, TEXT("the compact readout"));

		// Asking for compact changed nothing about the full answer.
		CheckInspectorsAgree(
			*this, Full, FullAgain, Case.Description, TEXT("the full readout, rebuilt"));

		CheckJointsAgree(
			*this, Full, FullAgain, Case.Description, TEXT("the full readout, rebuilt"));
	}

	// Fixture: the full readout must have had a table to drop.
	TestTrue(
		*FString::Printf(
			TEXT("fixture: some case must break out at least 3 joint rows in FULL detail for compact to be dropping anything, the most any case broke out is %d"),
			WidestJointTable),
		WidestJointTable >= 3);

	// Fixture: the entries must differ, or a list of copies would pass.
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
