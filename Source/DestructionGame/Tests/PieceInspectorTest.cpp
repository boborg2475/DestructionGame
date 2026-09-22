// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceActions.h"
#include "Core/PieceInspection.h"
#include "Core/PieceMenu.h"
#include "Core/Profiles/ConnectionProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

// Named and unique across the directory: unity builds merge files, so colliding file-local names fail to compile.
namespace PieceInspectorTestSupport
{
	using namespace DestructionProfiles;

	// Already includes the 1 N = 100 uu factor (5 kg weighs 4900 uu); applying 100 again is a 100x error.
	constexpr double InspectorGravityCmPerSecondSquared = 980.0;

	/**
	 * 1 N = 100 uu, spelled out independently of DestructionPresenter::ForceUnitsPerNewton (under
	 * test). Not ForceUnitsPerMPaSqCm (10,000); confusing the two is a 100x error.
	 */
	constexpr double InspectorForceUnitsPerNewton = 100.0;

	/** 1 MPa over 1 cm2 is 100 x 100 uu. Used only by fixture preconditions. */
	constexpr double InspectorForceUnitsPerMPaSqCm = 100.0 * 100.0;

	constexpr double InspectorJointAreaSqCm = 100.0;

	/** The fixture's structure id, and one it does not have. */
	constexpr int32 InspectorStructure = 4;
	constexpr int32 InspectorOtherStructure = 9;

	/*
	 * The worked fixture: the subject has all three joint roles, and one of its joints is severed.
	 *
	 *                        [2] Rider  3 kg
	 *                         |  conn 1   bed joint above the subject
	 *      Spare [3] ~ ~ ~ ~ [1] Subject 2 kg
	 *      (removed) conn 2   |  head joint, severed when the spare was pulled
	 *                         |  conn 0   bed joint beneath the subject
	 *                        [0] Pad    grounded, 10 kg
	 *
	 *      [4] Floater 4 kg — no joints, so ApplyResults releases it; that empties the menu
	 *                         (Delete's CanRun says no) while the debugger stays full.
	 *
	 * Piece 3 is removed before the solve, severing conn 2 (HasGiven, no break pass). The solver
	 * drops given joints from its support lists, so a presenter reading those would show two
	 * rows instead of three; this proves the breakout comes from InspectPiece.
	 *
	 * A disjoint component, the only way to reach EPieceSupport::Stranded:
	 *
	 *      [5] Knot ground —head— [6] Knot X —head— [7] Knot Y
	 *          (grounded)
	 *
	 * With no bed joints, X's supports are {ground, Y} and Y's are {X}: a cycle, so both are Stranded.
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

	const FVector InspectorBedNormal(0.0, 0.0, 1.0);

	const FVector InspectorHeadNormal(1.0, 0.0, 0.0);

	FPieceRef MakeRef(int32 StructureId, int32 PieceIndex)
	{
		FPieceRef Ref;
		Ref.StructureId = StructureId;
		Ref.PieceIndex = PieceIndex;
		return Ref;
	}

	/*
	 * The real RunningBond grid (21.5 x 10.25 x 6.5 cm brick, 1 cm joint). The course tolerances
	 * below are argued against the real 7.5 cm course pitch.
	 */
	constexpr double InspectorCoursePitchCm = 7.5;
	constexpr double InspectorFirstCourseZCm = 3.25;
	constexpr double InspectorBrickPitchCm = 22.5;

	/** Brick box at a centre. Courses are 0-based here; the readout counts from one. */
	DestructionLayout::FPieceBox InspectorBoxAt(double CentreXCm, double CentreZCm)
	{
		DestructionLayout::FPieceBox Box;
		Box.CentreCm = FVector(CentreXCm, 0.0, CentreZCm);
		Box.ExtentCm = FVector(10.75, 5.125, 3.25);
		return Box;
	}

	double InspectorCourseZ(int32 Course)
	{
		return InspectorFirstCourseZCm + Course * InspectorCoursePitchCm;
	}

	/*
	 * Where each handle of the worked fixture sits.
	 *
	 *   course 4 (Z 25.75)                                    [4] Floater
	 *   course 3 (Z 18.25)   [2] Rider
	 *   course 2 (Z 10.75)   [1] Subject   [3] Spare
	 *   course 1 (Z  3.25)   [0] Pad       [6] Knot X   [5] Knot ground   [7] Knot Y
	 *                        X=0           X=22.5       X=45             X=67.5
	 *
	 * The bottom course runs 6, 5, 7 left to right, so numbering by handle instead of X is caught.
	 */
	DestructionLayout::FPieceBox InspectorBoxFor(int32 Index)
	{
		switch (Index)
		{
		case 0: return InspectorBoxAt(0.0,                        InspectorCourseZ(0));
		case 1: return InspectorBoxAt(0.0,                        InspectorCourseZ(1));
		case 2: return InspectorBoxAt(0.0,                        InspectorCourseZ(2));
		case 3: return InspectorBoxAt(InspectorBrickPitchCm,      InspectorCourseZ(1));
		case 4: return InspectorBoxAt(InspectorBrickPitchCm * 2.0, InspectorCourseZ(3));
		case 5: return InspectorBoxAt(InspectorBrickPitchCm * 2.0, InspectorCourseZ(0));
		case 6: return InspectorBoxAt(InspectorBrickPitchCm,      InspectorCourseZ(0));
		case 7: return InspectorBoxAt(InspectorBrickPitchCm * 3.0, InspectorCourseZ(0));
		}

		return InspectorBoxAt(0.0, 0.0);
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

	/** Builds the diagram above. bSettle solves and applies results; false is an unsolved wall. */
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

		// The knot, appended last so connections #0-#2 keep their indices.
		AddJoint(Out, KnotGroundPiece, KnotXPiece, InspectorHeadNormal);
		AddJoint(Out, KnotXPiece, KnotYPiece, InspectorHeadNormal);

		Out.RemovePiece(SparePiece);

		if (bSettle)
		{
			Out.SolveLoads();
			Out.ApplyResults();
		}
	}

	/** Utilisation under a purely compressive force, so compression is the worst axis. */
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

	const TCHAR* NameOfBand(EJointMarginBand Band)
	{
		switch (Band)
		{
		case EJointMarginBand::Critical:    return TEXT("Critical");
		case EJointMarginBand::Caution:     return TEXT("Caution");
		case EJointMarginBand::Comfortable: return TEXT("Comfortable");
		}

		return TEXT("<not a band>");
	}

	/**
	 * Support word for a ref that names no brick. Spelled out here, not imported. The fail-open
	 * mistake would be "not solved yet", which looks like a live row on an unsolved wall.
	 */
	const TCHAR* const InspectorNoBrickSupportWord = TEXT("not in this wall");

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

	/** Every bucket, so a sweep covers the unused ones too. */
	const EPieceSupportBand AllSupportBands[] = {
		EPieceSupportBand::NotAPiece,
		EPieceSupportBand::NotSolved,
		EPieceSupportBand::Falling,
		EPieceSupportBand::Stranded,
		EPieceSupportBand::Supported,
		EPieceSupportBand::Grounded
	};

	/** The word each bucket must read as, spelled out here so dot colour and word stay paired. */
	FString InspectorWordForBand(EPieceSupportBand Band)
	{
		switch (Band)
		{
		case EPieceSupportBand::NotAPiece: return FString(InspectorNoBrickSupportWord);
		case EPieceSupportBand::NotSolved: return FString(TEXT("not solved yet"));
		case EPieceSupportBand::Falling:   return FString(TEXT("falling"));
		case EPieceSupportBand::Stranded:  return FString(TEXT("stranded"));
		case EPieceSupportBand::Supported: return FString(TEXT("supported"));
		case EPieceSupportBand::Grounded:  return FString(TEXT("grounded"));
		}

		return FString(TEXT("<no word for this band>"));
	}

	/** Band severity, ordered here; the enumerator values are not a scale. */
	int32 SeverityOfBand(EJointMarginBand Band)
	{
		switch (Band)
		{
		case EJointMarginBand::Comfortable: return 0;
		case EJointMarginBand::Caution:     return 1;
		case EJointMarginBand::Critical:    return 2;
		}

		return 3;
	}

	const TCHAR* const InspectorHeaderWord = TEXT("Selection");

	/** Shown instead of a readout; a blank region would look like a failed readout. */
	const TCHAR* const InspectorHintLine = TEXT("Hover a brick in the list to see its joints");

	/** Dumps the model for failure messages. */
	FString DescribeInspector(const FPieceMenuInspector& Inspector)
	{
		FString Line = FString::Printf(
			TEXT("{header:'%s' count:%d '%s' entries:"),
			*Inspector.HeaderText, Inspector.SelectedCount, *Inspector.CountText);

		if (Inspector.Pieces.Num() == 0)
		{
			Line += TEXT("<none>");
		}

		for (int32 Index = 0; Index < Inspector.Pieces.Num(); ++Index)
		{
			const FInspectorPieceEntry& Entry = Inspector.Pieces[Index];

			Line += FString::Printf(
				TEXT("%s'%s'(%s/%s){%d,%d}%s%s"),
				Index == 0 ? TEXT("") : TEXT(" "),
				*Entry.Label, *Entry.SupportText, NameOfSupportBand(Entry.SupportBand),
				Entry.Ref.StructureId, Entry.Ref.PieceIndex,
				Entry.bIsLivePiece ? TEXT("") : TEXT("[dead]"),
				Entry.bIsInspected ? TEXT("<==") : TEXT(""));
		}

		Line += FString::Printf(
			TEXT(" inspected:%s{%d,%d} '%s' hint:'%s' support:'%s'/%s jointstext:'%s' joints:"),
			Inspector.bHasInspectedPiece ? TEXT("yes") : TEXT("no"),
			Inspector.InspectedRef.StructureId, Inspector.InspectedRef.PieceIndex,
			*Inspector.InspectedLabel, *Inspector.InspectedHintText,
			*Inspector.SupportText, NameOfSupportBand(Inspector.SupportBand),
			*Inspector.JointsText);

		if (Inspector.Joints.Num() == 0)
		{
			Line += TEXT("<none>");
		}

		for (int32 Index = 0; Index < Inspector.Joints.Num(); ++Index)
		{
			const FInspectorJointRow& Joint = Inspector.Joints[Index];

			Line += FString::Printf(
				TEXT("%s[c%d->p%d %s %.6f N %.6f N.cm %.9f %% %s pass=%d margin:'%s' bar=%.9f %s slot=%d '%s']"),
				Index == 0 ? TEXT("") : TEXT(" "),
				Joint.ConnectionIndex, Joint.OtherPieceIndex, NameOfRole(Joint.Role),
				Joint.ForceN, Joint.MomentNCm, Joint.UtilisationPercent,
				Joint.bHasGiven ? TEXT("GIVEN") : TEXT("intact"),
				Joint.BreakPass, *Joint.MarginText, Joint.HeadroomFraction,
				NameOfBand(Joint.MarginBand), Joint.ColourSlot, *Joint.Text);
		}

		Line += FString::Printf(TEXT(" caption:'%s' scale:"), *Inspector.HeadroomCaption);

		for (int32 Index = 0; Index < Inspector.HeadroomScale.Num(); ++Index)
		{
			Line += FString::Printf(
				TEXT("%s['%s'@%.9f]"),
				Index == 0 ? TEXT("") : TEXT(" "),
				*Inspector.HeadroomScale[Index].Label,
				Inspector.HeadroomScale[Index].Fraction);
		}

		return Line + TEXT("}");
	}

	/** One row of the selection table below. */
	struct FInspectorCase
	{
		const TCHAR* Description = nullptr;

		/** In pick order. */
		TArray<FPieceRef> Selected;

		/** May be a ref outside Selected. */
		FPieceRef Inspected;

		/** One entry per selected ref, same order, whatever it resolves to. */
		TArray<FPieceRef> ExpectedEntries;

		TArray<FString> ExpectedLabels;

		/** Hand-written from the diagram: piece 3 is removed, structure 9 does not exist. */
		TArray<bool> ExpectedLive;

		const TCHAR* ExpectedCountText = nullptr;

		/** Index into ExpectedEntries, or INDEX_NONE. */
		int32 ExpectedInspectedEntry = INDEX_NONE;

		int32 ExpectedJointCount = 0;

		const TCHAR* ExpectedSupportText = TEXT("");

		/** Empty only when nothing is inspected; an inspected brick with no joints still gets a sentence. */
		const TCHAR* ExpectedJointsText = TEXT("");

		/** Each entry's support word, hand-written from the diagram. */
		TArray<const TCHAR*> ExpectedEntrySupport;
	};

	/**
	 * Properties every readout must have, swept over every case because they fail quietly: two
	 * inspected bricks, a NaN rendered as "nan %", stale joints.
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

		// The fixed panel is visible with an empty selection, so its heading is always present.
		Test.TestEqual(
			FString::Printf(
				TEXT("%s: the panel must always name itself '%s', it says '%s'"),
				Where, InspectorHeaderWord, *Inspector.HeaderText),
			Inspector.HeaderText, FString(InspectorHeaderWord));

		int32 InspectedEntries = 0;

		int32 MarkedEntry = INDEX_NONE;

		/** Rows with no support word; asserted once, not per row. */
		int32 SilentEntries = 0;

		/** Rows whose bucket disagrees with their word, and the first offender for the message. */
		int32 MismatchedBands = 0;
		FString FirstBandMismatch;

		for (int32 Index = 0; Index < Inspector.Pieces.Num(); ++Index)
		{
			const FInspectorPieceEntry& Entry = Inspector.Pieces[Index];

			SilentEntries += Entry.SupportText.IsEmpty() ? 1 : 0;

			if (Entry.SupportText != InspectorWordForBand(Entry.SupportBand))
			{
				++MismatchedBands;

				if (FirstBandMismatch.IsEmpty())
				{
					FirstBandMismatch = FString::Printf(
						TEXT("entry %d is bucketed %s, which must read '%s'; it reads '%s'"),
						Index, NameOfSupportBand(Entry.SupportBand),
						*InspectorWordForBand(Entry.SupportBand), *Entry.SupportText);
				}
			}

			if (Entry.bIsInspected && MarkedEntry == INDEX_NONE)
			{
				MarkedEntry = Index;
			}

			InspectedEntries += Entry.bIsInspected ? 1 : 0;

			// The inspected entry is always live; otherwise a greyed row would have a joint breakout.
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

		// Every row has a support word in every state; which word is the tables' job.
		Test.TestEqual(
			FString::Printf(
				TEXT("%s: every entry must say how its brick is held up, %d of %d say nothing %s"),
				Where, SilentEntries, Inspector.Pieces.Num(), *DescribeInspector(Inspector)),
			SilentEntries, 0);

		// Every row's bucket and word must agree.
		Test.TestEqual(
			FString::Printf(
				TEXT("%s: every entry's bucket must match its own word, %d of %d do not — %s %s"),
				Where, MismatchedBands, Inspector.Pieces.Num(),
				FirstBandMismatch.IsEmpty() ? TEXT("none") : *FirstBandMismatch,
				*DescribeInspector(Inspector)),
			MismatchedBands, 0);

		// The marked row and the readout describe one brick, so their words and buckets must match.
		if (Inspector.bHasInspectedPiece && Inspector.Pieces.IsValidIndex(MarkedEntry))
		{
			Test.TestEqual(
				FString::Printf(
					TEXT("%s: entry %d is the brick the readout is about, so its support word must match; the row says '%s' and the readout says '%s' %s"),
					Where, MarkedEntry, *Inspector.Pieces[MarkedEntry].SupportText,
					*Inspector.SupportText, *DescribeInspector(Inspector)),
				Inspector.Pieces[MarkedEntry].SupportText, Inspector.SupportText);

			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: entry %d is the brick the readout is about, so its bucket must match; the row is %s and the readout is %s %s"),
					Where, MarkedEntry, NameOfSupportBand(Inspector.Pieces[MarkedEntry].SupportBand),
					NameOfSupportBand(Inspector.SupportBand), *DescribeInspector(Inspector)),
				Inspector.Pieces[MarkedEntry].SupportBand == Inspector.SupportBand);

			Test.TestEqual(
				FString::Printf(
					TEXT("%s: the readout is bucketed %s, which must read '%s'; it reads '%s' %s"),
					Where, NameOfSupportBand(Inspector.SupportBand),
					*InspectorWordForBand(Inspector.SupportBand), *Inspector.SupportText,
					*DescribeInspector(Inspector)),
				Inspector.SupportText, InspectorWordForBand(Inspector.SupportBand));
		}

		// Nothing inspected means every inspected-brick field is empty; leftovers are stale.
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

			// NotAPiece is the zero enumerator, so a default readout satisfies this.
			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: nothing inspected must bucket as %s, it buckets as %s %s"),
					Where, NameOfSupportBand(EPieceSupportBand::NotAPiece),
					NameOfSupportBand(Inspector.SupportBand), *DescribeInspector(Inspector)),
				Inspector.SupportBand == EPieceSupportBand::NotAPiece);

			Test.TestTrue(
				*FString::Printf(TEXT("%s: nothing inspected must name no brick, it names {%d,%d}"),
					Where, Inspector.InspectedRef.StructureId, Inspector.InspectedRef.PieceIndex),
				Inspector.InspectedRef == FPieceRef());

			Test.TestEqual(
				FString::Printf(
					TEXT("%s: nothing inspected must say nothing about joints, it says '%s'"),
					Where, *Inspector.JointsText),
				Inspector.JointsText, FString());

			Test.TestEqual(
				FString::Printf(
					TEXT("%s: nothing inspected must name no brick over the readout, it names '%s'"),
					Where, *Inspector.InspectedLabel),
				Inspector.InspectedLabel, FString());

			Test.TestEqual(
				FString::Printf(
					TEXT("%s: nothing inspected must caption no bar, it says '%s'"),
					Where, *Inspector.HeadroomCaption),
				Inspector.HeadroomCaption, FString());

			Test.TestEqual(
				FString::Printf(
					TEXT("%s: nothing inspected must draw no scale, it offers %d tick(s)"),
					Where, Inspector.HeadroomScale.Num()),
				Inspector.HeadroomScale.Num(), 0);
		}

		// An inspected brick always gets a joints sentence, even with no joints.
		if (Inspector.bHasInspectedPiece)
		{
			Test.TestFalse(
				*FString::Printf(
					TEXT("%s: a brick is singled out, so its %d joint(s) must be summed up in words; the line is empty %s"),
					Where, Inspector.Joints.Num(), *DescribeInspector(Inspector)),
				Inspector.JointsText.IsEmpty());

			// The readout heading matches the marked entry's label; the entry may be scrolled out of view.
			if (Inspector.Pieces.IsValidIndex(MarkedEntry))
			{
				Test.TestEqual(
					FString::Printf(
						TEXT("%s: the readout must head itself with the brick it is about; entry %d reads '%s' and the readout says '%s' %s"),
						Where, MarkedEntry, *Inspector.Pieces[MarkedEntry].Label,
						*Inspector.InspectedLabel, *DescribeInspector(Inspector)),
					Inspector.InspectedLabel, Inspector.Pieces[MarkedEntry].Label);
			}

			Test.TestFalse(
				*FString::Printf(
					TEXT("%s: a brick is singled out, so the readout must name it; the line is empty %s"),
					Where, *DescribeInspector(Inspector)),
				Inspector.InspectedLabel.IsEmpty());
		}

		// The hint shows only when bricks are selected and none is inspected.
		const bool bShouldOfferTheHint =
			Inspector.SelectedCount > 0 && !Inspector.bHasInspectedPiece;

		Test.TestEqual(
			FString::Printf(
				TEXT("%s: %d brick(s) picked and %s singled out, so the readout region should read '%s'; it reads '%s'"),
				Where, Inspector.SelectedCount,
				Inspector.bHasInspectedPiece ? TEXT("one") : TEXT("none"),
				bShouldOfferTheHint ? InspectorHintLine : TEXT(""),
				*Inspector.InspectedHintText),
			Inspector.InspectedHintText,
			bShouldOfferTheHint ? FString(InspectorHintLine) : FString());

		/*
		 * Every displayed number is finite. GetConnectionUtilisation fails closed to double Max,
		 * which times 100 is infinity, and FMath::Max discards a NaN.
		 *
		 * Slot counters: rows whose colour slot is not their row number, and rows given a slot after
		 * the palette ran out.
		 */
		int32 MisplacedSlots = 0;
		int32 SlotsAfterTheEnd = 0;
		bool bSlotsHaveRunOut = false;

		for (int32 Index = 0; Index < Inspector.Joints.Num(); ++Index)
		{
			const FInspectorJointRow& Joint = Inspector.Joints[Index];

			/*
			 * Row i takes slot i; keying on the far-end brick would collide (1,220 bricks, a few hues).
			 * Past the palette's end it is INDEX_NONE, never a wrap, and stays out.
			 */
			if (Joint.ColourSlot != Index && Joint.ColourSlot != INDEX_NONE)
			{
				++MisplacedSlots;
			}

			if (Joint.ColourSlot == INDEX_NONE)
			{
				bSlotsHaveRunOut = true;
			}
			else if (bSlotsHaveRunOut)
			{
				++SlotsAfterTheEnd;
			}

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

			// The margin is a reciprocal, so unloaded and over-limit joints are its degenerate ends.
			Test.TestFalse(
				*FString::Printf(
					TEXT("%s: joint row %d must give a margin reading, it is empty %s"),
					Where, Index, *DescribeInspector(Inspector)),
				Joint.MarginText.IsEmpty());

			// Log of a reciprocal gives inf/NaN at the ends, which a Slate bar would clamp and draw plausibly.
			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: joint row %d's headroom must be a finite fraction, it is %f"),
					Where, Index, Joint.HeadroomFraction),
				FMath::IsFinite(Joint.HeadroomFraction)
					&& Joint.HeadroomFraction >= 0.0
					&& Joint.HeadroomFraction <= 1.0);

			// A given joint reads 0 N at 0% like an unloaded one, but must show an empty, Critical bar.
			if (Joint.bHasGiven)
			{
				Test.TestEqual(
					FString::Printf(
						TEXT("%s: joint row %d has given, so its bar must be empty; it reads %f"),
						Where, Index, Joint.HeadroomFraction),
					Joint.HeadroomFraction, 0.0);

				Test.TestTrue(
					*FString::Printf(
						TEXT("%s: joint row %d has given, so its bar must be in the most severe band, it is %s %s"),
						Where, Index, NameOfBand(Joint.MarginBand), *DescribeInspector(Inspector)),
					Joint.MarginBand == EJointMarginBand::Critical);
			}
		}

		Test.TestEqual(
			FString::Printf(
				TEXT("%s: every joint row's colour slot must be its own row number or nothing at all, %d of %d are neither %s"),
				Where, MisplacedSlots, Inspector.Joints.Num(), *DescribeInspector(Inspector)),
			MisplacedSlots, 0);

		Test.TestEqual(
			FString::Printf(
				TEXT("%s: once the palette has run out it must stay out, %d row(s) took a colour after one that did not %s"),
				Where, SlotsAfterTheEnd, *DescribeInspector(Inspector)),
			SlotsAfterTheEnd, 0);

		// A log bar is unreadable without its decade ticks, so the scale and caption exist exactly when there are joints.
		const int32 ExpectedTicks = Inspector.Joints.Num() > 0 ? 4 : 0;

		Test.TestEqual(
			FString::Printf(
				TEXT("%s: %d joint row(s) should come with %d scale tick(s), %d came %s"),
				Where, Inspector.Joints.Num(), ExpectedTicks, Inspector.HeadroomScale.Num(),
				*DescribeInspector(Inspector)),
			Inspector.HeadroomScale.Num(), ExpectedTicks);

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: the bar is %sdrawn, so it must%s be captioned; it says '%s'"),
				Where,
				Inspector.Joints.Num() > 0 ? TEXT("") : TEXT("not "),
				Inspector.Joints.Num() > 0 ? TEXT("") : TEXT(" NOT"),
				*Inspector.HeadroomCaption),
			Inspector.HeadroomCaption.IsEmpty() == (Inspector.Joints.Num() == 0));

		// Ticks are labelled, strictly ascending, and inside [0,1].
		double PreviousFraction = -1.0;

		for (int32 Index = 0; Index < Inspector.HeadroomScale.Num(); ++Index)
		{
			const FHeadroomScaleTick& Tick = Inspector.HeadroomScale[Index];

			Test.TestFalse(
				*FString::Printf(TEXT("%s: scale tick %d must say something, it is empty"),
					Where, Index),
				Tick.Label.IsEmpty());

			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: scale tick %d ('%s') must sit inside the bar and above tick %d, it is at %f"),
					Where, Index, *Tick.Label, Index - 1, Tick.Fraction),
				FMath::IsFinite(Tick.Fraction)
					&& Tick.Fraction >= 0.0
					&& Tick.Fraction <= 1.0
					&& Tick.Fraction > PreviousFraction);

			PreviousFraction = Tick.Fraction;
		}
	}

	/** The entry-list label for a handle, via the presenter (pinned by PieceMenuPositionLabel). */
	FString InspectorEntryLabelFor(const FStructureBinding& Binding, int32 Handle)
	{
		const TArray<FPieceRef> JustThatBrick = { MakeRef(Binding.StructureId, Handle) };

		const FPieceMenuInspector Named =
			BuildPieceMenuInspector(Binding, JustThatBrick, FPieceRef());

		return Named.Pieces.Num() == 1 ? Named.Pieces[0].Label : FString();
	}

	/** Every joint line contains its far end's entry-list label, so a player can find that brick. */
	void CheckFarEndsReadAsPositions(
		FAutomationTestBase& Test,
		const FStructureBinding& Binding,
		const FPieceMenuInspector& Inspector,
		const TCHAR* Where)
	{
		for (int32 Index = 0; Index < Inspector.Joints.Num(); ++Index)
		{
			const FInspectorJointRow& Row = Inspector.Joints[Index];
			const FString FarEndLabel = InspectorEntryLabelFor(Binding, Row.OtherPieceIndex);

			// An empty label is contained in every line, which would make the check vacuous.
			Test.TestFalse(
				*FString::Printf(
					TEXT("%s: joint row %d's far end is piece %d, which the entry list must be able to name at all"),
					Where, Index, Row.OtherPieceIndex),
				FarEndLabel.IsEmpty());

			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: joint row %d's far end is piece %d, which the entry list calls '%s'; the line must name it that way, it reads '%s'"),
					Where, Index, Row.OtherPieceIndex, *FarEndLabel, *Row.Text),
				!FarEndLabel.IsEmpty() && Row.Text.Contains(FarEndLabel, ESearchCase::CaseSensitive));
		}
	}
}

/**
 * The inspector counts the selection, lists every brick in pick order, and breaks out the joints
 * of the inspected one. Unlike FPieceMenuRow it does not fail closed on an unresolvable ref: a
 * selection holding a released brick empties the menu but still lists both bricks.
 *
 * The count never shrinks to what resolves; foreign, removed and malformed refs all count. Each
 * entry says whether it names a live brick. Labels are positions ("course 2 · #1"); refs with no
 * position fall back to "brick 9:1" / "brick 4:-1". No world needed.
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

	// Fixture preconditions: these check the fixture, not the feature.
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

	// Otherwise the "stranded" case below would pass while covering nothing.
	TestTrue(
		TEXT("fixture: knot X should be Stranded — the solver could not route it"),
		Binding.GetStructure().GetPieceSupport(KnotXPiece) == EPieceSupport::Stranded);

	TestTrue(
		TEXT("fixture: knot Y should be Stranded too — both ends are in the knot"),
		Binding.GetStructure().GetPieceSupport(KnotYPiece) == EPieceSupport::Stranded);

	// The menu is the intersection of CanRun, so one released brick empties it.
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
			TEXT(""),
			TArray<const TCHAR*>()
		},
		{
			TEXT("one brick, and it is the one being inspected"),
			{ MakeRef(InspectorStructure, SubjectPiece) },
			MakeRef(InspectorStructure, SubjectPiece),
			{ MakeRef(InspectorStructure, SubjectPiece) },
			{ TEXT("course 2 · #1") },
			{ true },
			TEXT("1 brick selected"),
			0,
			3,
			TEXT("supported"),
			TEXT("3 joints"),
			{ TEXT("supported") }
		},
		{
			// Pick order, not handle order: a sorting presenter would stop matching commit order.
			TEXT("three bricks in pick order, singling out the second"),
			{ MakeRef(InspectorStructure, RiderPiece),
			  MakeRef(InspectorStructure, PadPiece),
			  MakeRef(InspectorStructure, SubjectPiece) },
			MakeRef(InspectorStructure, PadPiece),
			{ MakeRef(InspectorStructure, RiderPiece),
			  MakeRef(InspectorStructure, PadPiece),
			  MakeRef(InspectorStructure, SubjectPiece) },
			{ TEXT("course 3 · #1"), TEXT("course 1 · #1"), TEXT("course 2 · #1") },
			{ true, true, true },
			TEXT("3 bricks selected"),
			1,
			1,
			TEXT("grounded"),
			TEXT("1 joint"),
			{ TEXT("supported"), TEXT("grounded"), TEXT("supported") }
		},
		{
			TEXT("three bricks and none singled out"),
			{ MakeRef(InspectorStructure, RiderPiece),
			  MakeRef(InspectorStructure, PadPiece),
			  MakeRef(InspectorStructure, SubjectPiece) },
			Nothing,
			{ MakeRef(InspectorStructure, RiderPiece),
			  MakeRef(InspectorStructure, PadPiece),
			  MakeRef(InspectorStructure, SubjectPiece) },
			{ TEXT("course 3 · #1"), TEXT("course 1 · #1"), TEXT("course 2 · #1") },
			{ true, true, true },
			TEXT("3 bricks selected"),
			INDEX_NONE,
			0,
			TEXT(""),
			TEXT(""),
			{ TEXT("supported"), TEXT("grounded"), TEXT("supported") }
		},
		{
			// Still live, so only set membership disqualifies it; calling InspectPiece alone would fail this.
			TEXT("the brick being inspected has been deselected"),
			{ MakeRef(InspectorStructure, RiderPiece), MakeRef(InspectorStructure, PadPiece) },
			MakeRef(InspectorStructure, SubjectPiece),
			{ MakeRef(InspectorStructure, RiderPiece), MakeRef(InspectorStructure, PadPiece) },
			{ TEXT("course 3 · #1"), TEXT("course 1 · #1") },
			{ true, true },
			TEXT("2 bricks selected"),
			INDEX_NONE,
			0,
			TEXT(""),
			TEXT(""),
			{ TEXT("supported"), TEXT("grounded") }
		},
		{
			// Counted and listed, but InspectPiece fails closed on a removed piece, so nothing is inspected.
			TEXT("the brick being inspected has been removed"),
			{ MakeRef(InspectorStructure, SubjectPiece), MakeRef(InspectorStructure, SparePiece) },
			MakeRef(InspectorStructure, SparePiece),
			{ MakeRef(InspectorStructure, SubjectPiece), MakeRef(InspectorStructure, SparePiece) },
			{ TEXT("course 2 · #1"), TEXT("course 2 · #2") },
			{ true, false },
			TEXT("2 bricks selected"),
			INDEX_NONE,
			0,
			TEXT(""),
			TEXT(""),
			// Not the removed piece's stale "supported".
			{ TEXT("supported"), InspectorNoBrickSupportWord }
		},
		{
			// Both refs have piece index 1, so labels must be qualified by structure to differ.
			TEXT("the brick being inspected belongs to another structure"),
			{ MakeRef(InspectorStructure, SubjectPiece),
			  MakeRef(InspectorOtherStructure, SubjectPiece) },
			MakeRef(InspectorOtherStructure, SubjectPiece),
			{ MakeRef(InspectorStructure, SubjectPiece),
			  MakeRef(InspectorOtherStructure, SubjectPiece) },
			{ TEXT("course 2 · #1"), TEXT("brick 9:1") },
			{ true, false },
			TEXT("2 bricks selected"),
			INDEX_NONE,
			0,
			TEXT(""),
			TEXT(""),
			{ TEXT("supported"), InspectorNoBrickSupportWord }
		},
		{
			/*
			 * A ref missing its piece index; unreachable via FPieceSelection, but must be counted,
			 * listed, never inspected. Its label is pinned as "brick 4:-1".
			 */
			TEXT("a ref missing its piece index"),
			{ MakeRef(InspectorStructure, SubjectPiece), MakeRef(InspectorStructure, INDEX_NONE) },
			MakeRef(InspectorStructure, INDEX_NONE),
			{ MakeRef(InspectorStructure, SubjectPiece), MakeRef(InspectorStructure, INDEX_NONE) },
			{ TEXT("course 2 · #1"), TEXT("brick 4:-1") },
			{ true, false },
			TEXT("2 bricks selected"),
			INDEX_NONE,
			0,
			TEXT(""),
			TEXT(""),
			{ TEXT("supported"), InspectorNoBrickSupportWord }
		},
		{
			// The menu is empty (asserted above) but the inspector is full.
			TEXT("the menu is empty because one brick is released, and the debugger is not"),
			{ MakeRef(InspectorStructure, SubjectPiece), MakeRef(InspectorStructure, FloaterPiece) },
			MakeRef(InspectorStructure, SubjectPiece),
			{ MakeRef(InspectorStructure, SubjectPiece), MakeRef(InspectorStructure, FloaterPiece) },
			{ TEXT("course 2 · #1"), TEXT("course 4 · #1") },
			// A released brick is still a live piece, though Delete refuses it.
			{ true, true },
			TEXT("2 bricks selected"),
			0,
			3,
			TEXT("supported"),
			TEXT("3 joints"),
			// Live but "falling": the only explanation of why the menu is empty.
			{ TEXT("supported"), TEXT("falling") }
		},
		{
			// Inspected with no joints: why bHasInspectedPiece is a field, not Joints.Num() > 0.
			TEXT("a released brick with no joints at all"),
			{ MakeRef(InspectorStructure, FloaterPiece) },
			MakeRef(InspectorStructure, FloaterPiece),
			{ MakeRef(InspectorStructure, FloaterPiece) },
			{ TEXT("course 4 · #1") },
			{ true },
			TEXT("1 brick selected"),
			0,
			0,
			TEXT("falling"),
			TEXT("No joints"),
			{ TEXT("falling") }
		},
		{
			// Unreachable via FPieceSelection; pins that the presenter projects a list, not a set.
			TEXT("the same brick twice"),
			{ MakeRef(InspectorStructure, PadPiece), MakeRef(InspectorStructure, PadPiece) },
			Nothing,
			{ MakeRef(InspectorStructure, PadPiece), MakeRef(InspectorStructure, PadPiece) },
			{ TEXT("course 1 · #1"), TEXT("course 1 · #1") },
			{ true, true },
			TEXT("2 bricks selected"),
			INDEX_NONE,
			0,
			TEXT(""),
			TEXT(""),
			{ TEXT("grounded"), TEXT("grounded") }
		},
		{
			// Only the first occurrence is inspected; comparing refs would mark both.
			TEXT("the same brick twice, and it is the one being inspected"),
			{ MakeRef(InspectorStructure, PadPiece), MakeRef(InspectorStructure, PadPiece) },
			MakeRef(InspectorStructure, PadPiece),
			{ MakeRef(InspectorStructure, PadPiece), MakeRef(InspectorStructure, PadPiece) },
			{ TEXT("course 1 · #1"), TEXT("course 1 · #1") },
			{ true, true },
			TEXT("2 bricks selected"),
			0,
			1,
			TEXT("grounded"),
			TEXT("1 joint"),
			{ TEXT("grounded"), TEXT("grounded") }
		},
		{
			// The only row that catches swapped "stranded"/"grounded" arms.
			TEXT("a brick the solver stranded in a knot"),
			{ MakeRef(InspectorStructure, KnotXPiece) },
			MakeRef(InspectorStructure, KnotXPiece),
			{ MakeRef(InspectorStructure, KnotXPiece) },
			{ TEXT("course 1 · #2") },
			{ true },
			TEXT("1 brick selected"),
			0,
			2,
			TEXT("stranded"),
			TEXT("2 joints"),
			{ TEXT("stranded") }
		},
	};

	for (const FInspectorCase& Case : Cases)
	{
		const FPieceMenuInspector Inspector =
			BuildPieceMenuInspector(Binding, Case.Selected, Case.Inspected);

		CheckInspectorInvariants(*this, Inspector, Case.Description);

		CheckFarEndsReadAsPositions(*this, Binding, Inspector, Case.Description);

		// The count as a number and as the printed sentence (singular/plural decided here).
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

		// Table integrity: a short list would silently skip entries.
		TestEqual(
			FString::Printf(TEXT("%s: the table row must name one label per expected entry"),
				Case.Description),
			Case.ExpectedLabels.Num(), Case.ExpectedEntries.Num());

		TestEqual(
			FString::Printf(TEXT("%s: the table row must name one liveness per expected entry"),
				Case.Description),
			Case.ExpectedLive.Num(), Case.ExpectedEntries.Num());

		TestEqual(
			FString::Printf(TEXT("%s: the table row must name one support word per expected entry"),
				Case.Description),
			Case.ExpectedEntrySupport.Num(), Case.ExpectedEntries.Num());

		if (Inspector.Pieces.Num() == Case.ExpectedEntries.Num())
		{
			for (int32 Index = 0; Index < Case.ExpectedEntries.Num(); ++Index)
			{
				const FInspectorPieceEntry& Entry = Inspector.Pieces[Index];

				/*
				 * "course <C> · #<N>" when placeable, else "brick <StructureId>:<PieceIndex>". Removed
				 * piece 3 still reads "course 2 · #2" because FPieceBinding keeps its box.
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

				// Liveness is decided by the model so the widget need not resolve refs to grey rows out.
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

				// Each row's support word, from the same InspectPiece call and vocabulary as the readout.
				if (Case.ExpectedEntrySupport.IsValidIndex(Index))
				{
					TestEqual(
						FString::Printf(
							TEXT("%s: entry %d {%d,%d} should read '%s', it reads '%s' %s"),
							Case.Description, Index,
							Case.ExpectedEntries[Index].StructureId,
							Case.ExpectedEntries[Index].PieceIndex,
							Case.ExpectedEntrySupport[Index], *Entry.SupportText,
							*DescribeInspector(Inspector)),
						Entry.SupportText, FString(Case.ExpectedEntrySupport[Index]));
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

		TestEqual(
			FString::Printf(TEXT("%s: the joint list should read '%s', it reads '%s' %s"),
				Case.Description, Case.ExpectedJointsText, *Inspector.JointsText,
				*DescribeInspector(Inspector)),
			Inspector.JointsText, FString(Case.ExpectedJointsText));
	}

	/*
	 * An unsolved wall reads "not solved yet", not "falling": EPieceSupport::Falling is also the
	 * zero default. Its joints are still listed.
	 */
	{
		FStructureBinding Unsolved;
		BuildWorkedFixture(Unsolved, /*bSettle*/ false);

		const TArray<FPieceRef> JustTheSubject = { MakeRef(InspectorStructure, SubjectPiece) };

		const FPieceMenuInspector Inspector = BuildPieceMenuInspector(
			Unsolved, JustTheSubject, MakeRef(InspectorStructure, SubjectPiece));

		CheckInspectorInvariants(*this, Inspector, TEXT("a wall nobody has solved"));

		// Positions come from the boxes, so they do not need a solve.
		CheckFarEndsReadAsPositions(*this, Unsolved, Inspector, TEXT("a wall nobody has solved"));

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

		TestEqual(
			FString::Printf(
				TEXT("a wall nobody has solved: its entry must NOT read 'falling', it reads '%s' %s"),
				Inspector.Pieces.Num() == 1 ? *Inspector.Pieces[0].SupportText : TEXT("<no entry>"),
				*DescribeInspector(Inspector)),
			Inspector.Pieces.Num() == 1 ? Inspector.Pieces[0].SupportText : FString(),
			FString(TEXT("not solved yet")));
	}

	return true;
}

/**
 * The joint breakout is InspectPiece's answer converted to newtons and per cent, same rows and
 * order, nothing recomputed. Asserted with exact equality against a live InspectPiece call. The
 * severed joint proves the rows come from InspectPiece (see the fixture). A given joint and an
 * intact unloaded one are both 0 N at 0%, so they must read differently.
 *
 * Units: 1 N = 100 uu; ForceUnitsPerMPaSqCm is 10,000, and using it here is a 100x error. No world needed.
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
	 * Fixture preconditions: conn 1 carries the rider's 2940 uu, conn 0 the subject plus rider's
	 * 4900 uu, both pure compression. Conn 2 is severed.
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

	// The model's own answer; asserting against it tests "not recomputed".
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
	CheckFarEndsReadAsPositions(*this, Binding, Inspector, TEXT("the subject's breakout"));

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

		// Ascending connection order, the only stable one.
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
		 * Exact equality, since a recomputation differs only in the last bit. The spelling must be
		 * `Force.Size() / ForceUnitsPerNewton` and `Utilisation * 100.0`; multiplying by 0.01 is an ulp out.
		 */
		const double ExpectedForceN = Model.ForceUu.Size() / InspectorForceUnitsPerNewton;

		TestTrue(
			*FString::Printf(
				TEXT("row %d should read %.9f N — %.6f uu at 100 uu per newton — it reads %.9f"),
				Index, ExpectedForceN, Model.ForceUu.Size(), Row.ForceN),
			Row.ForceN == ExpectedForceN);

		// Moment magnitude, uu.cm to N·cm through the same constant; length is already cm (MOMENTS_DESIGN.md).
		const double ExpectedMomentNCm = Model.MomentUuCm.Size() / InspectorForceUnitsPerNewton;

		TestTrue(
			*FString::Printf(
				TEXT("row %d should read %.9f N·cm — %.6f uu.cm at 100 uu per newton — it reads %.9f"),
				Index, ExpectedMomentNCm, Model.MomentUuCm.Size(), Row.MomentNCm),
			Row.MomentNCm == ExpectedMomentNCm);

		const double ExpectedPercent = Model.Utilisation * 100.0;

		TestTrue(
			*FString::Printf(
				TEXT("row %d should read %.12f %% — the solver's own ratio %.12f — it reads %.12f"),
				Index, ExpectedPercent, Model.Utilisation, Row.UtilisationPercent),
			Row.UtilisationPercent == ExpectedPercent);
	}

	/*
	 * The exact lines: connection index, far end by position, tier, then load or the reason there
	 * is none. Newtons to 1 decimal, per cent to 3. Margin is 1/utilisation, an integer at or above
	 * 100x (1 / 0.00049 = 2041). A given joint has no margin. Other states are in PieceMenuJointHeadroom.
	 */
	const TArray<FString> ExpectedLines = {
		TEXT("#0  course 1 · #1  bed below  generalpurposemortar  49.0 N  0.049 %  2041× margin"),
		TEXT("#1  course 3 · #1  bed above  generalpurposemortar  29.4 N  0.029 %  3401× margin"),
		TEXT("#2  course 2 · #2  head  broken (went with a removed piece)"),
	};

	for (int32 Index = 0; Index < ExpectedLines.Num() && Index < Inspector.Joints.Num(); ++Index)
	{
		TestEqual(
			FString::Printf(TEXT("joint line %d should read '%s', it reads '%s'"),
				Index, *ExpectedLines[Index], *Inspector.Joints[Index].Text),
			Inspector.Joints[Index].Text, ExpectedLines[Index]);
	}

	/*
	 * "broke in pass N" is not asserted: FStructureBinding does not expose SolveAndBreak yet
	 * (CURRENT_STATE.md). BreakPass itself is checked above and in PieceInspection.JointBreakout.
	 */

	return true;
}

/** Named and unique again; see the note on PieceInspectorTestSupport. */
namespace PiecePositionTestSupport
{
	using namespace PieceInspectorTestSupport;

	/** Distinct from the worked fixture's id, so fallback labels differ. */
	constexpr int32 PositionStructure = 12;

	/**
	 * Max Z distance between centres in one course, spelled out here. A course rises 7.5 cm, so
	 * 0.5 cm absorbs float noise without merging courses. Too loose merges courses, and two
	 * bricks share a label; too tight gives one course per brick.
	 */
	constexpr double PositionCourseToleranceCm = 0.5;

	/** A real NaN and +infinity via volatile, so nothing folds them; IsFinite passes double Max. */
	double PositionNaN()
	{
		volatile double Zero = 0.0;
		return Zero / Zero;
	}

	double PositionInfinity()
	{
		volatile double One = 1.0;
		volatile double Zero = 0.0;
		return One / Zero;
	}

	DestructionLayout::FPieceBox PositionBox(double XCm, double YCm, double ZCm)
	{
		DestructionLayout::FPieceBox Box;
		Box.CentreCm = FVector(XCm, YCm, ZCm);
		Box.ExtentCm = FVector(10.75, 5.125, 3.25);
		return Box;
	}

	struct FPositionCase
	{
		const TCHAR* Description = nullptr;

		/** One box per handle, in handle order. */
		TArray<DestructionLayout::FPieceBox> Boxes;

		/** Handles removed before the labels are read. */
		TArray<int32> Removed;

		/** In handle order. */
		TArray<FString> ExpectedLabels;
	};

	/** Boxes only: labels need no joints or solve. */
	void BuildPositionFixture(const FPositionCase& Case, FStructureBinding& Out)
	{
		Out.StructureId = PositionStructure;

		for (const DestructionLayout::FPieceBox& Box : Case.Boxes)
		{
			Out.AddPiece(/*MassKg*/ 1.0, /*bIsGrounded*/ false, nullptr, Box);
		}

		for (const int32 Handle : Case.Removed)
		{
			Out.RemovePiece(Handle);
		}
	}

	TArray<FPieceRef> AllRefsOf(const FStructureBinding& Binding)
	{
		TArray<FPieceRef> Refs;

		for (int32 Handle = 0; Handle < Binding.NumPieces(); ++Handle)
		{
			Refs.Add(MakeRef(PositionStructure, Handle));
		}

		return Refs;
	}
}

/**
 * A brick is labelled by position ("course 2 · #1"), and no two bricks share a label.
 *
 * Sort placeable pieces by centre Z and band them: a piece joins the course if within tolerance
 * of that course's lowest member (not the previous piece, which would let small steps chain two
 * courses into one). Courses number from the bottom; within one, order by X, then Y, then handle,
 * a total order. Removed pieces keep their place so neighbours are never renamed. A NaN or
 * infinite centre fails closed to "brick <StructureId>:<PieceIndex>" and takes no place. No world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuPositionLabelTest,
	"DestructionGame.Presenter.PieceMenuPositionLabel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuPositionLabelTest::RunTest(const FString& Parameters)
{
	using namespace PiecePositionTestSupport;

	const double CourseOne = InspectorCourseZ(0);
	const double CourseTwo = InspectorCourseZ(1);

	const double WellInside = PositionCourseToleranceCm * 0.8;
	const double WellOutside = PositionCourseToleranceCm * 4.0;

	const TArray<FPositionCase> Cases = {
		{
			// Handles are scrambled, so numbering by handle instead of position fails here.
			TEXT("two courses, numbered from the bottom and along by X rather than by handle"),
			{
				PositionBox(InspectorBrickPitchCm, 0.0, CourseTwo),
				PositionBox(0.0,                   0.0, CourseOne),
				PositionBox(InspectorBrickPitchCm, 0.0, CourseOne),
				PositionBox(0.0,                   0.0, CourseTwo),
			},
			{},
			{
				TEXT("course 2 · #2"),
				TEXT("course 1 · #1"),
				TEXT("course 1 · #2"),
				TEXT("course 2 · #1"),
			}
		},
		{
			// A course is a band, not a plane.
			TEXT("a course that is not perfectly level is still one course"),
			{
				PositionBox(0.0,                         0.0, CourseOne),
				PositionBox(InspectorBrickPitchCm,       0.0, CourseOne + WellInside),
				PositionBox(InspectorBrickPitchCm * 2.0, 0.0, CourseOne + WellInside * 0.5),
			},
			{},
			{
				TEXT("course 1 · #1"),
				TEXT("course 1 · #2"),
				TEXT("course 1 · #3"),
			}
		},
		{
			// The band has an edge.
			TEXT("a piece further than the tolerance above a course is a course of its own"),
			{
				PositionBox(0.0,                   0.0, CourseOne),
				PositionBox(InspectorBrickPitchCm, 0.0, CourseOne + WellOutside),
			},
			{},
			{
				TEXT("course 1 · #1"),
				TEXT("course 2 · #1"),
			}
		},
		{
			/*
			 * Each piece 0.4 cm above the last: measured from the course floor gives two courses of
			 * two; measured from the previous piece chains them into one. The only row that separates these.
			 */
			TEXT("near misses must not chain into one course"),
			{
				PositionBox(0.0,                         0.0, CourseOne),
				PositionBox(InspectorBrickPitchCm,       0.0, CourseOne + WellInside),
				PositionBox(InspectorBrickPitchCm * 2.0, 0.0, CourseOne + WellInside * 2.0),
				PositionBox(InspectorBrickPitchCm * 3.0, 0.0, CourseOne + WellInside * 3.0),
			},
			{},
			{
				TEXT("course 1 · #1"),
				TEXT("course 1 · #2"),
				TEXT("course 2 · #1"),
				TEXT("course 2 · #2"),
			}
		},
		{
			// Same course and X, different depth: X-only ordering fails.
			TEXT("two leaves of one course share an X and are still told apart"),
			{
				PositionBox(0.0,                   0.0,  CourseOne),
				PositionBox(0.0,                   11.25, CourseOne),
				PositionBox(InspectorBrickPitchCm, 0.0,  CourseOne),
			},
			{},
			{
				TEXT("course 1 · #1"),
				TEXT("course 1 · #2"),
				TEXT("course 1 · #3"),
			}
		},
		{
			// Coincident pieces; the handle is the final tie-break.
			TEXT("two pieces in exactly the same place still read apart"),
			{
				PositionBox(0.0,                   0.0, CourseOne),
				PositionBox(0.0,                   0.0, CourseOne),
				PositionBox(InspectorBrickPitchCm, 0.0, CourseOne),
			},
			{},
			{
				TEXT("course 1 · #1"),
				TEXT("course 1 · #2"),
				TEXT("course 1 · #3"),
			}
		},
		{
			// Unplaceable pieces fall back and consume no number: the good bricks still read #1 and #2.
			TEXT("a piece with no usable position falls back to its ref and takes no place"),
			{
				PositionBox(0.0,                   0.0, CourseOne),
				PositionBox(InspectorBrickPitchCm, 0.0, PositionNaN()),
				PositionBox(InspectorBrickPitchCm, 0.0, CourseOne),
				PositionBox(PositionInfinity(),    0.0, CourseOne),
			},
			{},
			{
				TEXT("course 1 · #1"),
				TEXT("brick 12:1"),
				TEXT("course 1 · #2"),
				TEXT("brick 12:3"),
			}
		},
		{
			// Skipping removed pieces would rename the third brick from #3 to #2.
			TEXT("removing a brick renames neither it nor the ones beside it"),
			{
				PositionBox(0.0,                         0.0, CourseOne),
				PositionBox(InspectorBrickPitchCm,       0.0, CourseOne),
				PositionBox(InspectorBrickPitchCm * 2.0, 0.0, CourseOne),
			},
			{ 1 },
			{
				TEXT("course 1 · #1"),
				TEXT("course 1 · #2"),
				TEXT("course 1 · #3"),
			}
		},
	};

	for (const FPositionCase& Case : Cases)
	{
		// Table integrity: a short list would silently skip the tail.
		TestEqual(
			FString::Printf(TEXT("%s: the table row must name one label per box"), Case.Description),
			Case.ExpectedLabels.Num(), Case.Boxes.Num());

		FStructureBinding Binding;
		BuildPositionFixture(Case, Binding);

		const TArray<FPieceRef> Refs = AllRefsOf(Binding);
		const FPieceMenuInspector Inspector = BuildPieceMenuInspector(Binding, Refs, FPieceRef());

		CheckInspectorInvariants(*this, Inspector, Case.Description);

		if (Inspector.Pieces.Num() != Case.ExpectedLabels.Num())
		{
			TestEqual(
				FString::Printf(TEXT("%s: should list %d entr(y/ies), it lists %d"),
					Case.Description, Case.ExpectedLabels.Num(), Inspector.Pieces.Num()),
				Inspector.Pieces.Num(), Case.ExpectedLabels.Num());

			continue;
		}

		for (int32 Index = 0; Index < Case.ExpectedLabels.Num(); ++Index)
		{
			TestEqual(
				FString::Printf(TEXT("%s: handle %d should read '%s', it reads '%s' %s"),
					Case.Description, Index, *Case.ExpectedLabels[Index],
					*Inspector.Pieces[Index].Label, *DescribeInspector(Inspector)),
				Inspector.Pieces[Index].Label, Case.ExpectedLabels[Index]);
		}

		// No two bricks share a label, checked on every case.
		for (int32 Left = 0; Left < Inspector.Pieces.Num(); ++Left)
		{
			for (int32 Right = Left + 1; Right < Inspector.Pieces.Num(); ++Right)
			{
				TestTrue(
					*FString::Printf(
						TEXT("%s: handles %d and %d are different bricks and must not share the label '%s' %s"),
						Case.Description, Left, Right, *Inspector.Pieces[Left].Label,
						*DescribeInspector(Inspector)),
					Inspector.Pieces[Left].Label != Inspector.Pieces[Right].Label);
			}
		}
	}

	return true;
}

/** Named and unique again; see the note on PieceInspectorTestSupport. */
namespace PieceHeadroomTestSupport
{
	using namespace PieceInspectorTestSupport;

	constexpr int32 HeadroomStructure = 21;

	/**
	 * 49 cm2 of 10 MPa mortar holds 4,900,000 uu, so a brick of M kg (M x 980 uu) sits at M / 5000
	 * of capacity: 5 kg is a thousandth, 5000 kg the limit. Compression governs throughout.
	 */
	constexpr double LadderJointAreaSqCm = 49.0;
	constexpr double LadderMassPerFullLoadKg = 5000.0;

	/** M / 5000, hand-derived. */
	double LadderUtilisation(double MassKg)
	{
		return MassKg / LadderMassPerFullLoadKg;
	}

	/*
	 * The load ladder: a grounded pad with a row of bricks, each on its own bed joint, so each
	 * rung's load is independent. Inspecting the pad breaks out every rung. Synthetic geometry;
	 * real load path and strengths.
	 */
	constexpr int32 LadderPadPiece = 0;

	/** A second grounded pad, so the head joint between them carries nothing. */
	constexpr int32 LadderNoLoadPiece = 11;

	/** Removed before the solve, so its joint is severed without failing. */
	constexpr int32 LadderRemovedPiece = 12;

	/**
	 * NaN centre, so it has no position; the solver never sees boxes, so its load is ordinary.
	 * The only way a joint row's far end reaches the fallback label.
	 */
	constexpr int32 LadderUnplaceablePiece = 13;

	/** Same as rung 0, so the rows differ only in the far-end label. */
	constexpr double LadderUnplaceableMassKg = 5.0;

	/**
	 * The one bent rung: 548.8 N at 49%, where rung 2 is 490 N at 1%. The difference is a moment
	 * only this row can show.
	 *
	 *   force        56 kg x 980 = 54,880 uu (548.8 N)
	 *   moment       4 cm lever arm along X: 219,520 uu.cm (2,195.2 N.cm), wholly about Y
	 *   section      6 x 8 cm, area 48 cm2, modulus b.d2/6 = 48 cm3 (the textbook form, not production's)
	 *   stresses     mean 0.1143333 MPa, edge 0.4573333 MPa
	 *   peak tension 0.343 MPa against mortar's 0.7, so 0.49 of capacity
	 *
	 * Tension governs by 8.6x over compression; shear is zero.
	 */
	constexpr int32 LadderEccentricPiece = 14;
	constexpr double LadderEccentricMassKg = 56.0;
	constexpr double LadderEccentricLeverArmCm = 4.0;
	constexpr double LadderEccentricHalfXCm = 3.0;
	constexpr double LadderEccentricHalfYCm = 4.0;
	constexpr double LadderEccentricAreaSqCm =
		4.0 * LadderEccentricHalfXCm * LadderEccentricHalfYCm;

	/** b.d2/6, with the depth taken along the lean. */
	constexpr double LadderEccentricModulusCm3 =
		(2.0 * LadderEccentricHalfYCm)
			* (2.0 * LadderEccentricHalfXCm) * (2.0 * LadderEccentricHalfXCm) / 6.0;

	/** Rung masses from handle 1. Handles 9 and 10 straddle the 1000 N unit switch exactly. */
	const TArray<double> LadderMassesKg = {
		5.0,                 // handle 1  — a thousandth of capacity, 1000x margin
		50.0,                // handle 2  — a hundredth,               100x
		500.0,               // handle 3  — a tenth,                   10x
		5000.0,              // handle 4  — exactly the limit,         1x
		0.5,                 // handle 5  — a ten-thousandth,          10000x, off the top of the bar
		51.0,                // handle 6  — 98.04x, just under the format's own boundary
		10000.0,             // handle 7  — twice the limit
		400.0,               // handle 8  — 12.5x
		100000.0 / 980.0,    // handle 9  — exactly 1000.0 N
		99990.0 / 980.0,     // handle 10 — exactly  999.9 N
	};

	void AddLadderJoint(FStructureBinding& Out, int32 PieceB, const FVector& Normal)
	{
		FConnection Connection;
		Connection.PieceA = LadderPadPiece;
		Connection.PieceB = PieceB;
		Connection.InterfaceNormal = Normal;
		Connection.InterfaceAreaSqCm = LadderJointAreaSqCm;
		Connection.Strength = GeneralPurposeMortar;
		Out.AddConnection(Connection);
	}

	/**
	 * The only rung with face geometry, so the only one that bends; AddConnection requires the
	 * rectangle to match the area.
	 *
	 * @return the connection index, or INDEX_NONE if refused.
	 */
	int32 AddEccentricLadderJoint(FStructureBinding& Out, int32 PieceB, double JointCentreXCm)
	{
		FConnection Connection;
		Connection.PieceA = LadderPadPiece;
		Connection.PieceB = PieceB;
		Connection.InterfaceNormal = InspectorBedNormal;
		Connection.InterfaceAreaSqCm = LadderEccentricAreaSqCm;
		Connection.InterfaceCentreCm = FVector(JointCentreXCm, 0.0, InspectorCourseZ(0) + 3.25);
		Connection.InterfaceHalfExtentCm =
			FVector(LadderEccentricHalfXCm, LadderEccentricHalfYCm, 0.0);
		Connection.Strength = GeneralPurposeMortar;
		return Out.AddConnection(Connection);
	}

	/** @return the connection index of the eccentric rung, or INDEX_NONE if it was refused. */
	int32 BuildLoadLadder(FStructureBinding& Out)
	{
		Out.StructureId = HeadroomStructure;

		Out.AddPiece(1.0, /*bIsGrounded*/ true, nullptr, InspectorBoxAt(0.0, InspectorCourseZ(0)));

		for (int32 Rung = 0; Rung < LadderMassesKg.Num(); ++Rung)
		{
			Out.AddPiece(
				LadderMassesKg[Rung], false, nullptr,
				InspectorBoxAt(Rung * InspectorBrickPitchCm, InspectorCourseZ(1)));
		}

		Out.AddPiece(
			1.0, /*bIsGrounded*/ true, nullptr,
			InspectorBoxAt(-InspectorBrickPitchCm, InspectorCourseZ(0)));

		Out.AddPiece(
			5.0, false, nullptr,
			InspectorBoxAt(InspectorBrickPitchCm * 11.0, InspectorCourseZ(1)));

		{
			DestructionLayout::FPieceBox Nowhere =
				InspectorBoxAt(InspectorBrickPitchCm * 12.0, InspectorCourseZ(1));

			Nowhere.CentreCm.Z = PiecePositionTestSupport::PositionNaN();

			Out.AddPiece(LadderUnplaceableMassKg, false, nullptr, Nowhere);
		}

		/*
		 * The bent rung. Its box centre is its centre of mass, so the 4 cm to the joint centroid is
		 * the lever arm. It reads course 2 · #12 since the unplaceable rung takes no position.
		 */
		Out.AddPiece(
			LadderEccentricMassKg, false, nullptr,
			InspectorBoxAt(InspectorBrickPitchCm * 13.0, InspectorCourseZ(1)));

		for (int32 Rung = 0; Rung < LadderMassesKg.Num(); ++Rung)
		{
			AddLadderJoint(Out, Rung + 1, InspectorBedNormal);
		}

		AddLadderJoint(Out, LadderNoLoadPiece, InspectorHeadNormal);
		AddLadderJoint(Out, LadderRemovedPiece, InspectorBedNormal);

		// Appended last so earlier connection indices stay put.
		AddLadderJoint(Out, LadderUnplaceablePiece, InspectorBedNormal);

		const int32 EccentricJoint = AddEccentricLadderJoint(
			Out,
			LadderEccentricPiece,
			InspectorBrickPitchCm * 13.0 - LadderEccentricLeverArmCm);

		Out.RemovePiece(LadderRemovedPiece);
		Out.SolveLoads();

		return EccentricJoint;
	}

	struct FHeadroomCase
	{
		const TCHAR* Description = nullptr;

		/** Also the row's position in the breakout. */
		int32 ConnectionIndex = INDEX_NONE;

		/** M x 980 uu / 100 uu per newton. */
		double ExpectedForceN = 0.0;

		double ExpectedUtilisationPercent = 0.0;

		const TCHAR* ExpectedMarginText = nullptr;

		/** clamp(log10(5000 / M) / 3, 0, 1), by hand. */
		double ExpectedHeadroom = 0.0;

		const TCHAR* ExpectedLine = nullptr;

		/**
		 * Comfortable above 10x margin, Caution below, Critical at or below 2x or given. The 2x edge
		 * is pinned in Presenter.PieceMenuJointMarginBand.
		 */
		EJointMarginBand ExpectedBand = EJointMarginBand::Critical;

		/** N.cm; zero on all but the bent rung, whose line alone gains a bending clause. */
		double ExpectedMomentNCm = 0.0;
	};
}

/**
 * Each joint row gives its margin (1 / utilisation, e.g. "213x margin"), a force in N or kN, and
 * a log-scaled headroom bar with labelled ticks.
 *
 * Special readings, where the formula would be wrong: an unloaded joint reads "no load" (not a
 * divide by zero); at or past the limit reads "no margin left" (not "0.5x margin"); a given joint
 * reads "gone" (not like an unloaded one).
 *
 * The bar spans three decades (full at 1000x, empty at failure), because a settled wall sits near
 * 0.0005 of capacity. Each tick must match the fill of the rung whose margin it names. The unit
 * switches to kN at exactly 1000 N. Only a bent joint gets a bending clause (MOMENTS_DESIGN.md).
 * No world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuJointHeadroomTest,
	"DestructionGame.Presenter.PieceMenuJointHeadroom",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuJointHeadroomTest::RunTest(const FString& Parameters)
{
	using namespace PieceHeadroomTestSupport;

	FStructureBinding Binding;
	const int32 EccentricJoint = BuildLoadLadder(Binding);

	// AddConnection returns INDEX_NONE for a rectangle that disagrees with its area.
	TestEqual(
		FString::Printf(
			TEXT("fixture: the eccentric rung's joint must be accepted and land last; AddConnection returned %d"),
			EccentricJoint),
		EccentricJoint, 13);

	// Fixture preconditions, hand-derived.
	for (int32 Rung = 0; Rung < LadderMassesKg.Num(); ++Rung)
	{
		const double MassKg = LadderMassesKg[Rung];
		const double ExpectedUu = MassKg * InspectorGravityCmPerSecondSquared;

		TestEqual(
			FString::Printf(TEXT("fixture: rung %d (%.4f kg) should load its joint to %.6f uu"),
				Rung, MassKg, ExpectedUu),
			Binding.GetStructure().GetConnectionForce(Rung).Size(), ExpectedUu);

		TestEqual(
			FString::Printf(TEXT("fixture: rung %d should sit at %.12f of capacity"),
				Rung, LadderUtilisation(MassKg)),
			Binding.GetStructure().GetConnectionUtilisation(Rung),
			LadderUtilisation(MassKg),
			1e-15);
	}

	TestEqual(
		TEXT("fixture: the head joint between two GROUNDED pads must carry nothing at all"),
		Binding.GetStructure().GetConnectionForce(LadderMassesKg.Num()).Size(), 0.0);

	TestTrue(
		TEXT("fixture: the pulled brick's joint went with it, so it must read as given"),
		Binding.GetStructure().GetConnection(LadderMassesKg.Num() + 1).HasGiven());

	// The NaN centre never reaches the solver, so this joint's load is ordinary.
	TestEqual(
		FString::Printf(
			TEXT("fixture: the unplaceable brick (%.4f kg) should load its joint to %.6f uu"),
			LadderUnplaceableMassKg,
			LadderUnplaceableMassKg * InspectorGravityCmPerSecondSquared),
		Binding.GetStructure().GetConnectionForce(LadderMassesKg.Num() + 2).Size(),
		LadderUnplaceableMassKg * InspectorGravityCmPerSecondSquared);

	TestFalse(
		TEXT("fixture: the unplaceable brick is still in the graph, so its joint is intact"),
		Binding.GetStructure().GetConnection(LadderMassesKg.Num() + 2).HasGiven());

	// The bent rung genuinely bends, so the moment assertions below are not vacuous.
	{
		const double EccentricForceUu =
			LadderEccentricMassKg * InspectorGravityCmPerSecondSquared;

		const double EccentricMomentUuCm = LadderEccentricLeverArmCm * EccentricForceUu;

		const double MeanStressMPa =
			EccentricForceUu / (LadderEccentricAreaSqCm * InspectorForceUnitsPerMPaSqCm);

		const double EdgeStressMPa =
			EccentricMomentUuCm / (LadderEccentricModulusCm3 * InspectorForceUnitsPerMPaSqCm);

		const double PeelUtilisation =
			(EdgeStressMPa - MeanStressMPa) / GeneralPurposeMortar.TensileStrengthMPa;

		TestEqual(
			FString::Printf(
				TEXT("fixture: the eccentric rung (%.1f kg) should load its joint to %.6f uu"),
				LadderEccentricMassKg, EccentricForceUu),
			Binding.GetStructure().GetConnectionForce(EccentricJoint).Size(), EccentricForceUu);

		TestEqual(
			FString::Printf(
				TEXT("fixture: the eccentric rung should bend its joint by %.6f uu.cm"),
				EccentricMomentUuCm),
			Binding.GetStructure().GetConnectionMoment(EccentricJoint).Size(),
			EccentricMomentUuCm,
			1e-9);

		TestEqual(
			FString::Printf(
				TEXT("fixture: the eccentric rung should sit at %.12f of capacity, in TENSION"),
				PeelUtilisation),
			Binding.GetStructure().GetConnectionUtilisation(EccentricJoint),
			PeelUtilisation,
			1e-15);

		// Tension must govern; a retuned profile could flip it without changing any number above.
		const double PeakCompressionUtilisation =
			(EdgeStressMPa + MeanStressMPa) / GeneralPurposeMortar.CompressiveStrengthMPa;

		TestTrue(
			FString::Printf(
				TEXT("fixture: the eccentric rung must be governed by TENSION (%.12f) rather than compression (%.12f)"),
				PeelUtilisation, PeakCompressionUtilisation),
			PeelUtilisation > PeakCompressionUtilisation);
	}

	const FPieceRef PadRef = MakeRef(HeadroomStructure, LadderPadPiece);
	const TArray<FPieceRef> JustThePad = { PadRef };

	const FPieceMenuInspector Inspector = BuildPieceMenuInspector(Binding, JustThePad, PadRef);

	CheckInspectorInvariants(*this, Inspector, TEXT("the load ladder"));
	CheckFarEndsReadAsPositions(*this, Binding, Inspector, TEXT("the load ladder"));

	/*
	 * Force and per cent are hand-derived here; PieceMenuJointReadout checks them against
	 * InspectPiece. Most far ends read "course 2 · #<handle>"; rows 10, 11 and 12 catch a handle
	 * printed as a position.
	 */
	const TArray<FHeadroomCase> Cases = {
		{
			TEXT("a thousandth of capacity: the top decade, and the bar is full"),
			0, 49.0, 0.1, TEXT("1000× margin"), 1.0,
			TEXT("#0  course 2 · #1  bed above  generalpurposemortar  49.0 N  0.100 %  1000× margin"),
			EJointMarginBand::Comfortable
		},
		{
			// 100x prints as an integer; 98.04x (row 5) keeps a decimal.
			TEXT("a hundredth of capacity: two decades of bar, and a whole-number margin"),
			1, 490.0, 1.0, TEXT("100× margin"), 2.0 / 3.0,
			TEXT("#1  course 2 · #2  bed above  generalpurposemortar  490.0 N  1.000 %  100× margin"),
			EJointMarginBand::Comfortable
		},
		{
			TEXT("a tenth of capacity: one decade of bar, and a margin worth a decimal"),
			2, 4900.0, 10.0, TEXT("10.0× margin"), 1.0 / 3.0,
			TEXT("#2  course 2 · #3  bed above  generalpurposemortar  4.9 kN  10.000 %  10.0× margin"),
			EJointMarginBand::Caution
		},
		{
			// 1.0 is intact but has nothing spare; "1.0x margin" would read like room.
			TEXT("exactly at the limit: the bar is empty and there is no margin to quote"),
			3, 49000.0, 100.0, TEXT("no margin left"), 0.0,
			TEXT("#3  course 2 · #4  bed above  generalpurposemortar  49.0 kN  100.000 %  no margin left"),
			EJointMarginBand::Critical
		},
		{
			// The bar clamps full; the number is not clamped.
			TEXT("four decades of margin: the bar pegs full and the number does not"),
			4, 4.9, 0.01, TEXT("10000× margin"), 1.0,
			TEXT("#4  course 2 · #5  bed above  generalpurposemortar  4.9 N  0.010 %  10000× margin"),
			EJointMarginBand::Comfortable
		},
		{
			// 5000/51 = 98.04, just under the 100× integer-format boundary.
			TEXT("just under a hundred times: still a decimal"),
			5, 499.8, 1.02, TEXT("98.0× margin"), 0.66379994274602749,
			TEXT("#5  course 2 · #6  bed above  generalpurposemortar  499.8 N  1.020 %  98.0× margin"),
			EJointMarginBand::Comfortable
		},
		{
			// The fail-open row: the naive reading is "0.5× margin" at 200%.
			TEXT("past the limit: still no margin, never a fraction of one"),
			6, 98000.0, 200.0, TEXT("no margin left"), 0.0,
			TEXT("#6  course 2 · #7  bed above  generalpurposemortar  98.0 kN  200.000 %  no margin left"),
			EJointMarginBand::Critical
		},
		{
			TEXT("twelve and a half times, between two decades"),
			7, 3920.0, 8.0, TEXT("12.5× margin"), 0.36563667100268549,
			TEXT("#7  course 2 · #8  bed above  generalpurposemortar  3.9 kN  8.000 %  12.5× margin"),
			EJointMarginBand::Comfortable
		},
		{
			// Exactly 1000.0 N, no rounding: the boundary reads kN.
			TEXT("exactly one thousand newtons reads in kilonewtons"),
			8, 1000.0, 2.0408163265306123, TEXT("49.0× margin"), 0.56339869334283788,
			TEXT("#8  course 2 · #9  bed above  generalpurposemortar  1.0 kN  2.041 %  49.0× margin"),
			EJointMarginBand::Comfortable
		},
		{
			TEXT("a tenth of a newton below the switch still reads in newtons"),
			9, 999.9, 2.0406122448979592, TEXT("49.0× margin"), 0.5634131705494404,
			TEXT("#9  course 2 · #10  bed above  generalpurposemortar  999.9 N  2.041 %  49.0× margin"),
			EJointMarginBand::Comfortable
		},
		{
			// Head joint between two grounded pads: zero load, full bar. Far end is in course 1.
			TEXT("an intact joint carrying nothing has no margin figure and a full bar"),
			10, 0.0, 0.0, TEXT("no load"), 1.0,
			TEXT("#10  course 1 · #1  head  generalpurposemortar  0.0 N  0.000 %  no load"),
			EJointMarginBand::Comfortable
		},
		{
			/*
			 * 0 N at 0% like the row above, but an empty bar and "gone". Handle 12 reads "#11"; a
			 * handle printed as a position would be off by one.
			 */
			TEXT("a joint that has given reads apart from an intact joint with nothing on it"),
			11, 0.0, 0.0, TEXT("gone"), 0.0,
			TEXT("#11  course 2 · #11  bed above  broken (went with a removed piece)"),
			EJointMarginBand::Critical
		},
		{
			// Same load as rung 0; only the far end differs, as the entry list's fallback "brick 21:13".
			TEXT("a far end nobody can place falls back to the ref-shaped label"),
			12, 49.0, 0.1, TEXT("1000× margin"), 1.0,
			TEXT("#12  brick 21:13  bed above  generalpurposemortar  49.0 N  0.100 %  1000× margin"),
			EJointMarginBand::Comfortable
		},
		{
			/*
			 * The bent rung: shows the moment (an input to the percentage), not the eccentricity M / F,
			 * which divides by a possibly small force. In N·cm, the game's length unit (MOMENTS_DESIGN.md).
			 */
			TEXT("a joint levered open by an off-centre load says what is bending it"),
			13, 548.8, 49.0, TEXT("2.0× margin"), 0.1032679733238288,
			TEXT("#13  course 2 · #12  bed above  generalpurposemortar  548.8 N  49.000 %  2.0× margin  2195.2 N·cm bending"),
			EJointMarginBand::Caution,
			2195.2
		},
	};

	TestEqual(
		FString::Printf(
			TEXT("the pad should break out one row per rung (%d), it broke out %d %s"),
			Cases.Num(), Inspector.Joints.Num(), *DescribeInspector(Inspector)),
		Inspector.Joints.Num(), Cases.Num());

	if (Inspector.Joints.Num() != Cases.Num())
	{
		return true;
	}

	/*
	 * Every row's moment equals the model's exactly. PieceMenuJointReadout's fixture has no joint
	 * geometry, so only this ladder makes the check non-trivial.
	 */
	{
		const FPieceInspection PadModel = InspectPiece(Binding, PadRef);

		for (int32 Index = 0;
			Index < PadModel.Joints.Num() && Index < Inspector.Joints.Num();
			++Index)
		{
			const double ExpectedMomentNCm =
				PadModel.Joints[Index].MomentUuCm.Size() / InspectorForceUnitsPerNewton;

			TestTrue(
				*FString::Printf(
					TEXT("row %d should read %.9f N·cm — the model's own %.6f uu.cm at 100 uu per newton — it reads %.9f"),
					Index, ExpectedMomentNCm,
					PadModel.Joints[Index].MomentUuCm.Size(),
					Inspector.Joints[Index].MomentNCm),
				Inspector.Joints[Index].MomentNCm == ExpectedMomentNCm);
		}
	}

	for (const FHeadroomCase& Case : Cases)
	{
		const FInspectorJointRow& Row = Inspector.Joints[Case.ConnectionIndex];

		TestEqual(
			FString::Printf(TEXT("%s: should be connection %d, it is %d"),
				Case.Description, Case.ConnectionIndex, Row.ConnectionIndex),
			Row.ConnectionIndex, Case.ConnectionIndex);

		TestEqual(
			FString::Printf(TEXT("%s: should carry %.6f N, it carries %.6f"),
				Case.Description, Case.ExpectedForceN, Row.ForceN),
			Row.ForceN, Case.ExpectedForceN, 1e-9);

		TestEqual(
			FString::Printf(TEXT("%s: should be bent by %.6f N·cm, it reads %.6f"),
				Case.Description, Case.ExpectedMomentNCm, Row.MomentNCm),
			Row.MomentNCm, Case.ExpectedMomentNCm, 1e-9);

		TestEqual(
			FString::Printf(TEXT("%s: should sit at %.12f %%, it sits at %.12f"),
				Case.Description, Case.ExpectedUtilisationPercent, Row.UtilisationPercent),
			Row.UtilisationPercent, Case.ExpectedUtilisationPercent, 1e-12);

		TestEqual(
			FString::Printf(TEXT("%s: margin should read '%s', it reads '%s' %s"),
				Case.Description, Case.ExpectedMarginText, *Row.MarginText,
				*DescribeInspector(Inspector)),
			Row.MarginText, FString(Case.ExpectedMarginText));

		// The decade rows (1, 2/3, 1/3, 0) rule out a natural log or the wrong decade count.
		TestEqual(
			FString::Printf(TEXT("%s: the bar should fill to %.12f, it fills to %.12f %s"),
				Case.Description, Case.ExpectedHeadroom, Row.HeadroomFraction,
				*DescribeInspector(Inspector)),
			Row.HeadroomFraction, Case.ExpectedHeadroom, 1e-12);

		TestEqual(
			FString::Printf(TEXT("%s: the line should read '%s', it reads '%s'"),
				Case.Description, Case.ExpectedLine, *Row.Text),
			Row.Text, FString(Case.ExpectedLine));

		// The model picks the band; the widget picks the hue.
		TestTrue(
			*FString::Printf(TEXT("%s: should be in the %s band, it is %s %s"),
				Case.Description, NameOfBand(Case.ExpectedBand), NameOfBand(Row.MarginBand),
				*DescribeInspector(Inspector)),
			Row.MarginBand == Case.ExpectedBand);
	}

	// Each tick's fraction must equal the fill of the rung whose margin it names.
	TestEqual(
		FString::Printf(TEXT("the bar should be captioned, it says '%s'"),
			*Inspector.HeadroomCaption),
		Inspector.HeadroomCaption,
		FString(TEXT("headroom — full is 1000× margin, empty is the joint giving")));

	struct FExpectedTick
	{
		const TCHAR* Label;
		double Fraction;

		/** The rung whose margin is exactly this tick. */
		int32 MatchingConnection;
	};

	const TArray<FExpectedTick> ExpectedTicks = {
		{ TEXT("1×"),    0.0,       3 },
		{ TEXT("10×"),   1.0 / 3.0, 2 },
		{ TEXT("100×"),  2.0 / 3.0, 1 },
		{ TEXT("1000×"), 1.0,       0 },
	};

	TestEqual(
		FString::Printf(TEXT("the scale should have %d ticks, it has %d %s"),
			ExpectedTicks.Num(), Inspector.HeadroomScale.Num(), *DescribeInspector(Inspector)),
		Inspector.HeadroomScale.Num(), ExpectedTicks.Num());

	for (int32 Index = 0; Index < ExpectedTicks.Num() && Index < Inspector.HeadroomScale.Num(); ++Index)
	{
		const FExpectedTick& Expected = ExpectedTicks[Index];
		const FHeadroomScaleTick& Tick = Inspector.HeadroomScale[Index];

		TestEqual(
			FString::Printf(TEXT("scale tick %d should read '%s', it reads '%s'"),
				Index, Expected.Label, *Tick.Label),
			Tick.Label, FString(Expected.Label));

		TestEqual(
			FString::Printf(TEXT("scale tick '%s' should sit at %.12f, it sits at %.12f"),
				Expected.Label, Expected.Fraction, Tick.Fraction),
			Tick.Fraction, Expected.Fraction, 1e-12);

		TestEqual(
			FString::Printf(
				TEXT("the joint whose margin IS %s must fill the bar to exactly where '%s' is drawn: %.12f against %.12f"),
				Expected.Label, Expected.Label,
				Inspector.Joints[Expected.MatchingConnection].HeadroomFraction, Tick.Fraction),
			Inspector.Joints[Expected.MatchingConnection].HeadroomFraction, Tick.Fraction, 1e-12);
	}

	// Monotonic over every pair of intact rungs: more load never means more headroom.
	for (int32 Left = 0; Left < Inspector.Joints.Num(); ++Left)
	{
		if (Inspector.Joints[Left].bHasGiven)
		{
			continue;
		}

		for (int32 Right = 0; Right < Inspector.Joints.Num(); ++Right)
		{
			if (Inspector.Joints[Right].bHasGiven
				|| Inspector.Joints[Left].UtilisationPercent
					>= Inspector.Joints[Right].UtilisationPercent)
			{
				continue;
			}

			TestTrue(
				*FString::Printf(
					TEXT("joint %d is at %.6f %% and joint %d at %.6f %%, so the lighter one cannot have LESS headroom: %.12f against %.12f"),
					Left, Inspector.Joints[Left].UtilisationPercent,
					Right, Inspector.Joints[Right].UtilisationPercent,
					Inspector.Joints[Left].HeadroomFraction,
					Inspector.Joints[Right].HeadroomFraction),
				Inspector.Joints[Left].HeadroomFraction
					>= Inspector.Joints[Right].HeadroomFraction);
		}
	}

	// The band is monotonic in load too, compared through SeverityOfBand, not enumerator values.
	for (int32 Left = 0; Left < Inspector.Joints.Num(); ++Left)
	{
		if (Inspector.Joints[Left].bHasGiven)
		{
			continue;
		}

		for (int32 Right = 0; Right < Inspector.Joints.Num(); ++Right)
		{
			if (Inspector.Joints[Right].bHasGiven
				|| Inspector.Joints[Left].UtilisationPercent
					>= Inspector.Joints[Right].UtilisationPercent)
			{
				continue;
			}

			TestTrue(
				*FString::Printf(
					TEXT("joint %d is at %.6f %% and joint %d at %.6f %%, so the lighter one cannot be in a WORSE band: %s against %s"),
					Left, Inspector.Joints[Left].UtilisationPercent,
					Right, Inspector.Joints[Right].UtilisationPercent,
					NameOfBand(Inspector.Joints[Left].MarginBand),
					NameOfBand(Inspector.Joints[Right].MarginBand)),
				SeverityOfBand(Inspector.Joints[Left].MarginBand)
					<= SeverityOfBand(Inspector.Joints[Right].MarginBand));
		}
	}

	return true;
}

/** Named and unique again; see the note on PieceInspectorTestSupport. */
namespace PieceBandBoundaryTestSupport
{
	using namespace PieceInspectorTestSupport;

	constexpr int32 BandStructure = 33;

	/**
	 * Same joint as the headroom ladder (M kg sits at M / 5000). A separate ladder so the headroom
	 * table's connection indices do not shift.
	 */
	constexpr double BandJointAreaSqCm = 49.0;
	constexpr double BandMassPerFullLoadKg = 5000.0;

	/**
	 * Either side of each band edge:
	 *
	 *     499 kg   9.98 %   10.02x margin   the last comfortable joint
	 *     500 kg  10.00 %   10.00x margin   EXACTLY the green/amber edge
	 *    2499 kg  49.98 %    2.0008x        the last cautious joint
	 *    2500 kg  50.00 %    2.00x          EXACTLY the amber/red edge
	 */
	const TArray<double> BandMassesKg = { 499.0, 500.0, 2499.0, 2500.0 };

	void BuildBandLadder(FStructureBinding& Out)
	{
		Out.StructureId = BandStructure;

		Out.AddPiece(1.0, /*bIsGrounded*/ true, nullptr, InspectorBoxAt(0.0, InspectorCourseZ(0)));

		for (int32 Rung = 0; Rung < BandMassesKg.Num(); ++Rung)
		{
			Out.AddPiece(
				BandMassesKg[Rung], false, nullptr,
				InspectorBoxAt(Rung * InspectorBrickPitchCm, InspectorCourseZ(1)));
		}

		for (int32 Rung = 0; Rung < BandMassesKg.Num(); ++Rung)
		{
			FConnection Connection;
			Connection.PieceA = 0;
			Connection.PieceB = Rung + 1;
			Connection.InterfaceNormal = InspectorBedNormal;
			Connection.InterfaceAreaSqCm = BandJointAreaSqCm;
			Connection.Strength = GeneralPurposeMortar;
			Out.AddConnection(Connection);
		}

		Out.SolveLoads();
	}
}

/**
 * A joint's margin band is a model field, so a widget never decides what counts as dangerous.
 * Edges are 10x and 2x margin; exactly at an edge the joint takes the worse band (fail closed).
 * Written as guards on UtilisationPercent (Comfortable below 10%, Caution below 50%, else
 * Critical), so a NaN is Critical. No world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuJointMarginBandTest,
	"DestructionGame.Presenter.PieceMenuJointMarginBand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuJointMarginBandTest::RunTest(const FString& Parameters)
{
	using namespace PieceBandBoundaryTestSupport;

	FStructureBinding Binding;
	BuildBandLadder(Binding);

	struct FBandCase
	{
		const TCHAR* Description = nullptr;
		int32 ConnectionIndex = INDEX_NONE;

		/** M / 5000 as a percentage, hand-derived. */
		double ExpectedUtilisationPercent = 0.0;

		EJointMarginBand ExpectedBand = EJointMarginBand::Critical;
	};

	const TArray<FBandCase> Cases = {
		{
			TEXT("just over ten times its load: still comfortable"),
			0, 9.98, EJointMarginBand::Comfortable
		},
		{
			// `Margin >= 10 ? green : amber` differs only on this row.
			TEXT("exactly ten times its load: the edge, and the edge is cautious"),
			1, 10.0, EJointMarginBand::Caution
		},
		{
			TEXT("just over twice its load: still cautious"),
			2, 49.98, EJointMarginBand::Caution
		},
		{
			TEXT("exactly twice its load: the edge, and the edge is critical"),
			3, 50.0, EJointMarginBand::Critical
		},
	};

	const FPieceRef PadRef = MakeRef(BandStructure, 0);
	const TArray<FPieceRef> JustThePad = { PadRef };

	const FPieceMenuInspector Inspector = BuildPieceMenuInspector(Binding, JustThePad, PadRef);

	CheckInspectorInvariants(*this, Inspector, TEXT("the band ladder"));

	TestEqual(
		FString::Printf(
			TEXT("the pad should break out one row per rung (%d), it broke out %d %s"),
			Cases.Num(), Inspector.Joints.Num(), *DescribeInspector(Inspector)),
		Inspector.Joints.Num(), Cases.Num());

	if (Inspector.Joints.Num() != Cases.Num())
	{
		return true;
	}

	for (const FBandCase& Case : Cases)
	{
		const FInspectorJointRow& Row = Inspector.Joints[Case.ConnectionIndex];

		// Fixture precondition: the rung carries the hand-derived load.
		TestEqual(
			FString::Printf(TEXT("%s: should sit at %.6f %% of capacity, it sits at %.6f"),
				Case.Description, Case.ExpectedUtilisationPercent, Row.UtilisationPercent),
			Row.UtilisationPercent, Case.ExpectedUtilisationPercent, 1e-12);

		TestTrue(
			*FString::Printf(TEXT("%s: should be in the %s band, it is %s %s"),
				Case.Description, NameOfBand(Case.ExpectedBand), NameOfBand(Row.MarginBand),
				*DescribeInspector(Inspector)),
			Row.MarginBand == Case.ExpectedBand);
	}

	return true;
}

/**
 * Joint row i takes colour slot i, and the palette runs out (INDEX_NONE) rather than wrapping.
 * Keyed on the row, not the far-end brick, so one joint can take different slots in different
 * readouts. The general rules are swept by CheckInspectorInvariants. No world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuJointColourSlotTest,
	"DestructionGame.Presenter.PieceMenuJointColourSlots",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuJointColourSlotTest::RunTest(const FString& Parameters)
{
	using namespace PieceHeadroomTestSupport;

	FStructureBinding Worked;
	BuildWorkedFixture(Worked, /*bSettle*/ true);

	const FPieceRef SubjectRef = MakeRef(InspectorStructure, SubjectPiece);
	const FPieceRef RiderRef = MakeRef(InspectorStructure, RiderPiece);

	const TArray<FPieceRef> JustTheSubject = { SubjectRef };
	const TArray<FPieceRef> JustTheRider = { RiderRef };

	const FPieceMenuInspector Subject =
		BuildPieceMenuInspector(Worked, JustTheSubject, SubjectRef);

	const FPieceMenuInspector Rider =
		BuildPieceMenuInspector(Worked, JustTheRider, RiderRef);

	CheckInspectorInvariants(*this, Subject, TEXT("the subject's swatches"));
	CheckInspectorInvariants(*this, Rider, TEXT("the rider's swatches"));

	TestEqual(
		FString::Printf(
			TEXT("fixture: the subject should break out its 3 joints, it broke out %d %s"),
			Subject.Joints.Num(), *DescribeInspector(Subject)),
		Subject.Joints.Num(), 3);

	TestEqual(
		FString::Printf(
			TEXT("fixture: the rider should break out its 1 joint, it broke out %d %s"),
			Rider.Joints.Num(), *DescribeInspector(Rider)),
		Rider.Joints.Num(), 1);

	if (Subject.Joints.Num() != 3 || Rider.Joints.Num() != 1)
	{
		return true;
	}

	// All three rows get a slot, including the severed one.
	for (int32 Index = 0; Index < Subject.Joints.Num(); ++Index)
	{
		TestEqual(
			FString::Printf(
				TEXT("the subject's joint row %d should take colour slot %d, it took %d %s"),
				Index, Index, Subject.Joints[Index].ColourSlot, *DescribeInspector(Subject)),
			Subject.Joints[Index].ColourSlot, Index);
	}

	// Connection 1 is the subject's row 1 and the rider's row 0, so it takes different slots.
	TestEqual(
		FString::Printf(
			TEXT("connection %d is the subject's second joint row, so it takes slot 1; it took %d %s"),
			Subject.Joints[1].ConnectionIndex, Subject.Joints[1].ColourSlot,
			*DescribeInspector(Subject)),
		Subject.Joints[1].ColourSlot, 1);

	TestEqual(
		FString::Printf(
			TEXT("the SAME connection %d is the rider's first joint row, so it takes slot 0; it took %d %s"),
			Rider.Joints[0].ConnectionIndex, Rider.Joints[0].ColourSlot,
			*DescribeInspector(Rider)),
		Rider.Joints[0].ColourSlot, 0);

	TestEqual(
		FString::Printf(
			TEXT("fixture: both readouts must be describing ONE joint for that to mean anything: %d against %d"),
			Subject.Joints[1].ConnectionIndex, Rider.Joints[0].ConnectionIndex),
		Rider.Joints[0].ConnectionIndex, Subject.Joints[1].ConnectionIndex);

	// The ladder is the only fixture with more joints on one piece than the palette holds.
	FStructureBinding Ladder;
	BuildLoadLadder(Ladder);

	const FPieceRef PadRef = MakeRef(HeadroomStructure, LadderPadPiece);
	const TArray<FPieceRef> JustThePad = { PadRef };

	const FPieceMenuInspector Long = BuildPieceMenuInspector(Ladder, JustThePad, PadRef);

	CheckInspectorInvariants(*this, Long, TEXT("the load ladder's swatches"));

	TestTrue(
		FString::Printf(
			TEXT("fixture: the ladder should break out more rows than a palette holds, it broke out %d"),
			Long.Joints.Num()),
		Long.Joints.Num() >= 13);

	// At least 6 slots: a running-bond brick has two joints above, two below, and two head joints.
	const int32 ColouredRows = Long.Joints.IndexOfByPredicate(
		[](const FInspectorJointRow& Row) { return Row.ColourSlot == INDEX_NONE; });

	TestTrue(
		FString::Printf(
			TEXT("the palette must reach at least the 6 joints an ordinary brick has, it ran out after %d row(s) %s"),
			ColouredRows == INDEX_NONE ? Long.Joints.Num() : ColouredRows,
			*DescribeInspector(Long)),
		ColouredRows == INDEX_NONE || ColouredRows >= 6);

	// No two rows share a slot; this is what wrapping would break.
	for (int32 Left = 0; Left < Long.Joints.Num(); ++Left)
	{
		if (Long.Joints[Left].ColourSlot == INDEX_NONE)
		{
			continue;
		}

		for (int32 Right = Left + 1; Right < Long.Joints.Num(); ++Right)
		{
			TestTrue(
				*FString::Printf(
					TEXT("joint rows %d and %d are different joints and must not share colour slot %d %s"),
					Left, Right, Long.Joints[Left].ColourSlot, *DescribeInspector(Long)),
				Long.Joints[Left].ColourSlot != Long.Joints[Right].ColourSlot);
		}
	}

	return true;
}

/**
 * Every brick row carries a support bucket as well as a word, so a widget colours a dot without
 * comparing strings. Six buckets: the four physical states, plus NotAPiece ("not in this wall")
 * and NotSolved, kept separate because two buckets can share a colour but one cannot be split
 * later. Bucket/word agreement is swept by CheckInspectorInvariants; this table pins which bucket
 * each state gets. No world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuSupportBandTest,
	"DestructionGame.Presenter.PieceMenuSupportBand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuSupportBandTest::RunTest(const FString& Parameters)
{
	using namespace PieceInspectorTestSupport;

	// Each bucket needs a distinct word, or the bucket/word sweep cannot tell them apart.
	for (int32 Left = 0; Left < UE_ARRAY_COUNT(AllSupportBands); ++Left)
	{
		TestFalse(
			*FString::Printf(TEXT("table: bucket %s must have a word of its own, it has none"),
				NameOfSupportBand(AllSupportBands[Left])),
			InspectorWordForBand(AllSupportBands[Left]).IsEmpty());

		for (int32 Right = Left + 1; Right < UE_ARRAY_COUNT(AllSupportBands); ++Right)
		{
			TestTrue(
				*FString::Printf(
					TEXT("table: buckets %s and %s must read differently or the sweep cannot tell them apart; both read '%s'"),
					NameOfSupportBand(AllSupportBands[Left]),
					NameOfSupportBand(AllSupportBands[Right]),
					*InspectorWordForBand(AllSupportBands[Left])),
				InspectorWordForBand(AllSupportBands[Left])
					!= InspectorWordForBand(AllSupportBands[Right]));
		}
	}

	FStructureBinding Binding;
	BuildWorkedFixture(Binding, /*bSettle*/ true);

	// Fixture preconditions: the four physical states are all present.
	TestTrue(
		TEXT("fixture: the pad should be Grounded"),
		Binding.GetStructure().GetPieceSupport(PadPiece) == EPieceSupport::Grounded);

	TestTrue(
		TEXT("fixture: the subject should be Supported"),
		Binding.GetStructure().GetPieceSupport(SubjectPiece) == EPieceSupport::Supported);

	TestTrue(
		TEXT("fixture: the floater should be Falling — nothing is joined to it at all"),
		Binding.GetStructure().GetPieceSupport(FloaterPiece) == EPieceSupport::Falling);

	TestTrue(
		TEXT("fixture: knot X should be Stranded — the solver could not route it"),
		Binding.GetStructure().GetPieceSupport(KnotXPiece) == EPieceSupport::Stranded);

	TestTrue(
		TEXT("fixture: the spare should be out of the graph, so its ref names no brick"),
		Binding.IsPieceRemoved(SparePiece));

	struct FBandCase
	{
		const TCHAR* Description = nullptr;

		TArray<FPieceRef> Selected;
		FPieceRef Inspected;

		/** One per selected ref, hand-written from the diagram. */
		TArray<EPieceSupportBand> ExpectedEntryBands;

		/** NotAPiece when nothing is inspected. */
		EPieceSupportBand ExpectedReadoutBand = EPieceSupportBand::NotAPiece;
	};

	const FPieceRef Nothing;

	const FPieceRef Pad = MakeRef(InspectorStructure, PadPiece);
	const FPieceRef Subject = MakeRef(InspectorStructure, SubjectPiece);
	const FPieceRef Floater = MakeRef(InspectorStructure, FloaterPiece);
	const FPieceRef KnotX = MakeRef(InspectorStructure, KnotXPiece);
	const FPieceRef Removed = MakeRef(InspectorStructure, SparePiece);
	const FPieceRef Foreign = MakeRef(InspectorOtherStructure, SubjectPiece);
	const FPieceRef Malformed = MakeRef(InspectorStructure, INDEX_NONE);

	const TArray<FBandCase> Cases = {
		{
			// Every bucket a solved wall can produce, in one list.
			TEXT("seven bricks in every state there is, none singled out"),
			{ Pad, Subject, Floater, KnotX, Removed, Foreign, Malformed },
			Nothing,
			{ EPieceSupportBand::Grounded, EPieceSupportBand::Supported,
			  EPieceSupportBand::Falling, EPieceSupportBand::Stranded,
			  EPieceSupportBand::NotAPiece, EPieceSupportBand::NotAPiece,
			  EPieceSupportBand::NotAPiece },
			EPieceSupportBand::NotAPiece
		},
		{
			TEXT("the same seven, singling out the supported one"),
			{ Pad, Subject, Floater, KnotX, Removed, Foreign, Malformed },
			Subject,
			{ EPieceSupportBand::Grounded, EPieceSupportBand::Supported,
			  EPieceSupportBand::Falling, EPieceSupportBand::Stranded,
			  EPieceSupportBand::NotAPiece, EPieceSupportBand::NotAPiece,
			  EPieceSupportBand::NotAPiece },
			EPieceSupportBand::Supported
		},
		{
			TEXT("the grounded pad, singled out"),
			{ Pad }, Pad,
			{ EPieceSupportBand::Grounded },
			EPieceSupportBand::Grounded
		},
		{
			TEXT("the released floater, singled out"),
			{ Floater }, Floater,
			{ EPieceSupportBand::Falling },
			EPieceSupportBand::Falling
		},
		{
			// Stranded means the solver could not route it; painting it grounded would fail open.
			TEXT("a brick the solver stranded in a knot, singled out"),
			{ KnotX }, KnotX,
			{ EPieceSupportBand::Stranded },
			EPieceSupportBand::Stranded
		},
		{
			// A ref naming nothing is never inspected, so the readout is NotAPiece.
			TEXT("a removed brick beside a live one, the removed one pointed at"),
			{ Subject, Removed }, Removed,
			{ EPieceSupportBand::Supported, EPieceSupportBand::NotAPiece },
			EPieceSupportBand::NotAPiece
		},
	};

	for (const FBandCase& Case : Cases)
	{
		const FPieceMenuInspector Inspector =
			BuildPieceMenuInspector(Binding, Case.Selected, Case.Inspected);

		CheckInspectorInvariants(*this, Inspector, Case.Description);

		TestEqual(
			FString::Printf(TEXT("%s: the table row must name one bucket per selected ref"),
				Case.Description),
			Case.ExpectedEntryBands.Num(), Case.Selected.Num());

		TestEqual(
			FString::Printf(TEXT("%s: should list %d entr(y/ies), it lists %d %s"),
				Case.Description, Case.Selected.Num(), Inspector.Pieces.Num(),
				*DescribeInspector(Inspector)),
			Inspector.Pieces.Num(), Case.Selected.Num());

		if (Inspector.Pieces.Num() == Case.ExpectedEntryBands.Num())
		{
			for (int32 Index = 0; Index < Case.ExpectedEntryBands.Num(); ++Index)
			{
				TestTrue(
					*FString::Printf(
						TEXT("%s: entry %d {%d,%d} should bucket as %s, it buckets as %s %s"),
						Case.Description, Index,
						Case.Selected[Index].StructureId, Case.Selected[Index].PieceIndex,
						NameOfSupportBand(Case.ExpectedEntryBands[Index]),
						NameOfSupportBand(Inspector.Pieces[Index].SupportBand),
						*DescribeInspector(Inspector)),
					Inspector.Pieces[Index].SupportBand == Case.ExpectedEntryBands[Index]);
			}
		}

		TestTrue(
			*FString::Printf(
				TEXT("%s: the readout should bucket as %s, it buckets as %s %s"),
				Case.Description,
				NameOfSupportBand(Case.ExpectedReadoutBand),
				NameOfSupportBand(Inspector.SupportBand),
				*DescribeInspector(Inspector)),
			Inspector.SupportBand == Case.ExpectedReadoutBand);
	}

	// NotSolved needs an unsolved wall; EPieceSupport::Falling is also the zero default.
	{
		FStructureBinding Unsolved;
		BuildWorkedFixture(Unsolved, /*bSettle*/ false);

		TestFalse(
			TEXT("fixture: nobody has solved this wall, so its subject must have no support answer"),
			Unsolved.GetStructure().HasSupportAnswer(SubjectPiece));

		const TArray<FPieceRef> JustTheSubject = { Subject };

		const FPieceMenuInspector Inspector =
			BuildPieceMenuInspector(Unsolved, JustTheSubject, Subject);

		CheckInspectorInvariants(*this, Inspector, TEXT("a wall nobody has solved"));

		TestTrue(
			*FString::Printf(
				TEXT("a wall nobody has solved: its entry must bucket as %s and NOT as %s, it buckets as %s %s"),
				NameOfSupportBand(EPieceSupportBand::NotSolved),
				NameOfSupportBand(EPieceSupportBand::Falling),
				Inspector.Pieces.Num() == 1
					? NameOfSupportBand(Inspector.Pieces[0].SupportBand) : TEXT("<no entry>"),
				*DescribeInspector(Inspector)),
			Inspector.Pieces.Num() == 1
				&& Inspector.Pieces[0].SupportBand == EPieceSupportBand::NotSolved);

		TestTrue(
			*FString::Printf(
				TEXT("a wall nobody has solved: the readout must bucket as %s, it buckets as %s %s"),
				NameOfSupportBand(EPieceSupportBand::NotSolved),
				NameOfSupportBand(Inspector.SupportBand),
				*DescribeInspector(Inspector)),
			Inspector.SupportBand == EPieceSupportBand::NotSolved);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
