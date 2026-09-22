// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceActions.h"
#include "Core/PieceInspection.h"
#include "Core/PieceMenu.h"
#include "Core/Profiles/ConnectionProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Named, not anonymous, and unique across this directory: a unity build merges files, so two
 * file-local names that collide are a hard compile error. See CURRENT_STATE.md.
 */
namespace PieceInspectorTestSupport
{
	using namespace DestructionProfiles;

	/**
	 * Unreal's gravity. The 1 N = 100 uu conversion is already inside it (5 kg weighs
	 * 5 x 980 = 4900 uu = 49 N x 100), so applying 100 again is a 100x error.
	 */
	constexpr double InspectorGravityCmPerSecondSquared = 980.0;

	/**
	 * 1 N = 100 uu, spelled out independently so this file fails if the production constant
	 * (DestructionPresenter::ForceUnitsPerNewton, under test) is wrong. Not
	 * ForceUnitsPerMPaSqCm (10,000); confusing the two is a 100x error.
	 */
	constexpr double InspectorForceUnitsPerNewton = 100.0;

	/**
	 * The strength boundary: 1 MPa (1 N/mm2) over 1 cm2 is 100 x 100 uu. Used only by the
	 * fixture preconditions.
	 */
	constexpr double InspectorForceUnitsPerMPaSqCm = 100.0 * 100.0;

	/** An ordinary bed- or head-joint face. */
	constexpr double InspectorJointAreaSqCm = 100.0;

	/** The structure the fixture identifies itself as, and one it does not. */
	constexpr int32 InspectorStructure = 4;
	constexpr int32 InspectorOtherStructure = 9;

	/*
	 * The worked fixture: five pieces around one subject, so the subject wears all three joint
	 * roles at once and one of its joints has gone.
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
	 * Piece 3 is pulled before the solve, severing conn 2 without it failing (HasGiven true, no
	 * break pass; DESIGN.md's "went with a removed piece"). That severed joint is also the
	 * discriminator for "the breakout came from InspectPiece": the solver drops a given joint
	 * from its support lists before the tier is decided, so a presenter walking those would show
	 * two rows where three are due.
	 *
	 * A second, disjoint component, the only way to reach EPieceSupport::Stranded:
	 *
	 *      [5] Knot ground —head— [6] Knot X —head— [7] Knot Y
	 *          (grounded)
	 *
	 * Neither X nor Y has a bed joint, so each falls back to head joints: X's supports are
	 * {ground, Y} and Y's are {X}, a cycle that never reaches earth, so the solver reports both
	 * Stranded. Nothing here touches pieces 0-4.
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

	/*
	 * The wall's real grid, from DestructionLayout::RunningBond: a 21.5 x 10.25 x 6.5 cm brick
	 * on a 1.0 cm joint gives a 22.5 cm brick pitch and a 7.5 cm course pitch, bottom course
	 * centred at half a brick height. Real numbers matter because the course tolerances below
	 * are argued against the 7.5 cm a course actually rises.
	 */
	constexpr double InspectorCoursePitchCm = 7.5;
	constexpr double InspectorFirstCourseZCm = 3.25;
	constexpr double InspectorBrickPitchCm = 22.5;

	/** Course c (0-based here; the readout counts from one) at brick position b. */
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
	 * The bottom course puts X order and handle order out of step (handles 5, 6, 7 run left to
	 * right as 6, 5, 7), so a derivation that numbered along a course by handle would agree with
	 * this table everywhere else and be wrong about exactly these two.
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

	/**
	 * The diagram above, built. bSettle drives the "has anybody solved yet" axis: true gives
	 * the subject a support answer and releases the floater, false is the freshly-built wall
	 * that must not read as falling. A null actor is fine: nothing here acts through one.
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

		/* The knot, appended after the three joints so no connection index moves (#0, #1, #2). */
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
	 * Utilisation of a joint whose only loaded axis is compression. The force is exactly
	 * antiparallel to an exactly vertical normal, so shear and tension are exactly zero and
	 * compression is necessarily ComputeUtilisation's worst axis.
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
	 * What an entry's support column reads when the ref names no brick (removed, foreign, or
	 * malformed all come back bIsPiece false). Spelled out here, not imported. The fail-open
	 * mistake is "not solved yet", which reads identically to a live row on an unsolved wall.
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

	/** Every bucket there is, so a sweep over them cannot quietly stop at the ones in use. */
	const EPieceSupportBand AllSupportBands[] = {
		EPieceSupportBand::NotAPiece,
		EPieceSupportBand::NotSolved,
		EPieceSupportBand::Falling,
		EPieceSupportBand::Stranded,
		EPieceSupportBand::Supported,
		EPieceSupportBand::Grounded
	};

	/**
	 * What each bucket must read as, spelled out here, not asked of the model. The bucket colours
	 * a dot and the word sits beside it; a row must never say "grounded" next to the "falling"
	 * colour, so the two are pinned together. Asserted injective too: if two buckets shared a
	 * word the sweep could no longer tell them apart.
	 */
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

	/**
	 * How severe a band is, ordered here rather than by the enumerator's value (which is a
	 * fail-closed choice, not a scale). More load can never mean a calmer colour.
	 */
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

	/** The word the panel calls itself, spelled here rather than imported from the model. */
	const TCHAR* const InspectorHeaderWord = TEXT("Selection");

	/**
	 * The line that stands in when there is no readout. A sentence, not a blank, because the
	 * fixed panel reserves the space and an empty region reads as a readout that failed.
	 */
	const TCHAR* const InspectorHintLine = TEXT("Hover a brick in the list to see its joints");

	/** What came back, so a failure reads without a debugger. */
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

		/** What the player has picked, in pick order. */
		TArray<FPieceRef> Selected;

		/** Which of them is being singled out — or something that is not one of them. */
		FPieceRef Inspected;

		/** One entry per selected brick, in the same order, whatever the refs resolve to. */
		TArray<FPieceRef> ExpectedEntries;

		/** What each entry reads, in order. Same length as ExpectedEntries (checked below). */
		TArray<FString> ExpectedLabels;

		/**
		 * Whether each entry still names a live piece, in order. Hand-written from the diagram,
		 * not derived from the binding. Pieces 0, 1, 2 and 4-7 are live, piece 3 was pulled out,
		 * structure 9 does not exist, and a ref missing a half names nothing.
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
		 * The joint list as a sentence. Empty when nothing is inspected; the point of the field
		 * is that an inspected brick with no joints still gets one.
		 */
		const TCHAR* ExpectedJointsText = TEXT("");

		/**
		 * What each entry's support column reads, in order. One per row so the list can show
		 * every brick's state at once instead of one at a time. Hand-written from the diagram,
		 * not read back off the binding.
		 */
		TArray<const TCHAR*> ExpectedEntrySupport;
	};

	/**
	 * The properties every answer must have, whatever the case. Swept over every row because
	 * these fail quietly: a second inspected brick draws two joint lists, a NaN utilisation
	 * renders as "nan %", and stale joints are the readout of somebody else's wall.
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

		/*
		 * The panel names itself in every state: a fixed panel is on screen even when the
		 * selection is empty, so the heading must not vanish with the last brick. Swept rather
		 * than tabled because it is the same word in every row.
		 */
		Test.TestEqual(
			FString::Printf(
				TEXT("%s: the panel must always name itself '%s', it says '%s'"),
				Where, InspectorHeaderWord, *Inspector.HeaderText),
			Inspector.HeaderText, FString(InspectorHeaderWord));

		int32 InspectedEntries = 0;

		/** Which entry the model marked, so the readout's own heading can be held against it. */
		int32 MarkedEntry = INDEX_NONE;

		/**
		 * How many rows said nothing about whether their brick is standing up. Counted and
		 * asserted once, because a field nothing fills is empty on every row at once.
		 */
		int32 SilentEntries = 0;

		/**
		 * How many rows' dot disagreed with their own word, and the first that did. The bucket
		 * lets a widget colour a dot without comparing SupportText against literals; if the two
		 * disagree, a row has an ordinary word beside the wrong colour. The first offender is
		 * carried so the message can name one.
		 */
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

			/*
			 * The singled-out entry is always live. bHasInspectedPiece and bIsLivePiece are the
			 * same question on the same ref, so an inspected-but-dead entry would be a greyed-out
			 * row with a joint breakout under it.
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
		 * Every row says whether its brick is standing up, in every state, including a ref that
		 * names nothing and a wall nobody has solved. A column blank on some rows is the absence
		 * this struct is shaped against. Which word each state gets is the tables' job; that
		 * there is always one is swept here.
		 */
		Test.TestEqual(
			FString::Printf(
				TEXT("%s: every entry must say how its brick is held up, %d of %d say nothing %s"),
				Where, SilentEntries, Inspector.Pieces.Num(), *DescribeInspector(Inspector)),
			SilentEntries, 0);

		/*
		 * Every row's dot and word are one fact. Which bucket each state gets is the tables' job;
		 * that the two halves of a row cannot contradict each other is swept here, over every
		 * readout this file builds.
		 */
		Test.TestEqual(
			FString::Printf(
				TEXT("%s: every entry's bucket must match its own word, %d of %d do not — %s %s"),
				Where, MismatchedBands, Inspector.Pieces.Num(),
				FirstBandMismatch.IsEmpty() ? TEXT("none") : *FirstBandMismatch,
				*DescribeInspector(Inspector)),
			MismatchedBands, 0);

		/*
		 * The row and the readout under it may not disagree: one brick must not read "supported"
		 * in the list and "stranded" over its joints. Held against the model's own other field,
		 * on top of the tables that pin what the word is.
		 */
		if (Inspector.bHasInspectedPiece && Inspector.Pieces.IsValidIndex(MarkedEntry))
		{
			Test.TestEqual(
				FString::Printf(
					TEXT("%s: entry %d is the brick the readout is about, so its support word must match; the row says '%s' and the readout says '%s' %s"),
					Where, MarkedEntry, *Inspector.Pieces[MarkedEntry].SupportText,
					*Inspector.SupportText, *DescribeInspector(Inspector)),
				Inspector.Pieces[MarkedEntry].SupportText, Inspector.SupportText);

			/*
			 * The same for the bucket, the assertion the coloured-dot slice turns on: a green dot
			 * on the row above an amber one over the joints is the panel contradicting itself
			 * about one brick. Held against the model's other field, like the word check above.
			 */
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

		/*
		 * Nothing inspected means nothing drawn about one. A breakout left over from a
		 * deselected brick is the stale-field defect FPieceMenuRow was shaped to prevent.
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

			/*
			 * The bucket goes with it, to the value that claims nothing: an empty word beside a
			 * bucket still saying "grounded" is a dot in the colour of a brick the readout is no
			 * longer about. NotAPiece is the zero enumerator, so a default readout answers this too.
			 */
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

			/* It must not head the readout with a brick either: a name over an empty breakout is stale. */
			Test.TestEqual(
				FString::Printf(
					TEXT("%s: nothing inspected must name no brick over the readout, it names '%s'"),
					Where, *Inspector.InspectedLabel),
				Inspector.InspectedLabel, FString());

			/* And no scale: a caption and ticks label a bar, and a caption over a gone breakout is stale. */
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

		/*
		 * An inspected brick always gets a sentence about its joints, even with none: "no joints"
		 * is a fact about the brick, and a widget noticing an empty array for itself is a branch
		 * nothing can test. CountText's "No bricks selected" is the same rule one level up.
		 */
		if (Inspector.bHasInspectedPiece)
		{
			Test.TestFalse(
				*FString::Printf(
					TEXT("%s: a brick is singled out, so its %d joint(s) must be summed up in words; the line is empty %s"),
					Where, Inspector.Joints.Num(), *DescribeInspector(Inspector)),
				Inspector.JointsText.IsEmpty());

			/*
			 * And the breakout names the brick it is about, in the list's own words. Held against
			 * the marked entry's Label, not a string here: the tables pin every label against the
			 * diagram, so this ties the two halves of the panel to each other on top of that.
			 * Necessary because the entry can be scrolled out of sight while the breakout stays.
			 */
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

		/*
		 * The readout region carries the hint in exactly one state: bricks picked and none
		 * pointed at, where the fixed panel would otherwise show a blank that reads as failed. A
		 * brick pointed at must not carry it (a hint over a live breakout is stale), and neither
		 * must an empty selection, where CountText already speaks.
		 */
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
		 * Every number a human reads is finite. GetConnectionUtilisation fails closed to
		 * TNumericLimits<double>::Max() for a missing connection, which times 100 is an infinity,
		 * and FMath::Max discards a NaN, so a degenerate answer can reach a screen looking real.
		 */
		/**
		 * How many rows took a colour slot other than their own row number, and how many took one
		 * after the palette ran out. Counted, not asserted per row: one broken rule breaks all.
		 */
		int32 MisplacedSlots = 0;
		int32 SlotsAfterTheEnd = 0;
		bool bSlotsHaveRunOut = false;

		for (int32 Index = 0; Index < Inspector.Joints.Num(); ++Index)
		{
			const FInspectorJointRow& Joint = Inspector.Joints[Index];

			/*
			 * The swatch is the row's number (row i takes slot i), which keeps colours stable
			 * while a player scans. Keying on the far-end brick would collide, since a wall has
			 * 1,220 bricks and a palette a handful of hues. Past the end it is INDEX_NONE, never a
			 * wrap: wrapping reintroduces that collision on the brick with the most joints, and
			 * once out the palette stays out.
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

			/*
			 * And the margin, the field most likely to be silently absent. It is a reciprocal, so
			 * its degenerate inputs are the ends of the range: an unloaded joint divides by zero, a
			 * joint past its limit divides into less than one. Every state needs a sentence.
			 */
			Test.TestFalse(
				*FString::Printf(
					TEXT("%s: joint row %d must give a margin reading, it is empty %s"),
					Where, Index, *DescribeInspector(Inspector)),
				Joint.MarginText.IsEmpty());

			/*
			 * And the bar's fill is always a fraction. A log of a reciprocal produces an infinity
			 * for an unloaded joint and a NaN for a negative one, both of which a Slate progress
			 * bar clamps and draws plausibly. This makes the bar's arithmetic falsifiable.
			 */
			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: joint row %d's headroom must be a finite fraction, it is %f"),
					Where, Index, Joint.HeadroomFraction),
				FMath::IsFinite(Joint.HeadroomFraction)
					&& Joint.HeadroomFraction >= 0.0
					&& Joint.HeadroomFraction <= 1.0);

			/*
			 * A given joint has no headroom: it carries 0 N at 0%, identical to an intact unloaded
			 * joint, which has the most headroom there is. Drawing a full bar beside a hole in the
			 * wall is the worst thing this panel could do.
			 */
			if (Joint.bHasGiven)
			{
				Test.TestEqual(
					FString::Printf(
						TEXT("%s: joint row %d has given, so its bar must be empty; it reads %f"),
						Where, Index, Joint.HeadroomFraction),
					Joint.HeadroomFraction, 0.0);

				/*
				 * And it is coloured like the hole it is. A given joint reads 0 N at 0%, the most
				 * comfortable state there is, so a band from the number alone would paint a hole the
				 * colour of a healthy joint. This is bHasGiven's whole reason to exist.
				 */
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

		/*
		 * The scale is drawn exactly when there is a bar to label. A log bar without its decades
		 * is unreadable: the same fill means 1000x on one panel and 3x on another. So the ticks
		 * are supplied here, not composed by the widget. And a brick with no joints draws no bar,
		 * so a caption beside it would label nothing.
		 */
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

		/*
		 * And the ticks are a scale: strictly ascending fractions inside the bar, each labelled. A
		 * tick outside [0,1] is off the end of the bar, and two ticks at one fraction claim the
		 * same place while drawing perfectly.
		 */
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

	/**
	 * What the entry list calls one handle of this binding. Asked of the presenter's other entry
	 * point, not re-derived here: the label is pinned against hand-written expectations by
	 * PieceMenuPositionLabel, so this reads back a known-good answer rather than a circular one.
	 */
	FString InspectorEntryLabelFor(const FStructureBinding& Binding, int32 Handle)
	{
		const TArray<FPieceRef> JustThatBrick = { MakeRef(Binding.StructureId, Handle) };

		const FPieceMenuInspector Named =
			BuildPieceMenuInspector(Binding, JustThatBrick, FPieceRef());

		return Named.Pieces.Num() == 1 ? Named.Pieces[0].Label : FString();
	}

	/**
	 * Every joint line names its far end the way the entry list names that brick. The two are one
	 * panel: a joint row exists so a player can find the brick on the other side, which an array
	 * subscript cannot help them do. Swept, not tabled, so the far ends no table names are covered
	 * too. Containment rather than equality, because the line also carries connection, tier and load.
	 */
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

			/*
			 * A nameless far end would make the sweep vacuous, since an empty string is contained
			 * in every line. The entry list is total, so this fires only for a non-handle.
			 */
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
 * The presented menu is two things: the actions, and a debugger that counts the selection, lists
 * every brick in pick order, and breaks out the joints of the one singled out, or none.
 *
 * A sibling of FPieceMenuRow, not more fields on it, because the two have opposite fail-closed
 * polarities: a row is an offer, so BuildPieceMenuRows refuses the whole list when one ref names
 * nothing, while a readout must still report the hole. The settling case is below: a selection
 * holding a released brick empties the menu but must still list both bricks and break out the
 * live one.
 *
 * The count is asserted on the presented model, not the selection, and never shrinks to what
 * resolves: a foreign ref, a removed piece and a malformed ref all still count, because the
 * player picked that many and the highlights are drawn off the same set.
 *
 * Every entry also says whether it still names a brick (FPieceInspection::bIsPiece per entry), so
 * a widget can grey the dead ones out without resolving refs itself. A released brick reads live:
 * what the menu may do about it is PieceActionsFor's intersection, said by the rows going empty.
 *
 * Every label is a position ("course 2 · #1"), not an array index: "brick 4:282" is a structure
 * id and a subscript, and with one structure in the game the "4:" is always noise. A course and
 * place along it is derivable here and only here, since FStructureBinding has the boxes and
 * FStructure does not. The old label survives as the fallback for refs with no position (another
 * wall, a missing half), which read "brick 9:1" and "brick 4:-1", shapes no position collides
 * with. PieceMenuPositionLabel pins the derivation.
 *
 * Needs a ticking world: no, and not even a world.
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
	 * Fixture preconditions, asked of the graph. Every claim below is worthless if the wall is
	 * not the diagram's; green on arrival, they check the fixture, not the feature.
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
	 * The knot must actually be a knot, asked of the solver. Stranded has no other way in (a
	 * support cycle that never reaches earth), so if this fixture stopped producing one the
	 * "stranded" row below would retarget onto whatever came instead and pass while covering nothing.
	 */
	TestTrue(
		TEXT("fixture: knot X should be Stranded — the solver could not route it"),
		Binding.GetStructure().GetPieceSupport(KnotXPiece) == EPieceSupport::Stranded);

	TestTrue(
		TEXT("fixture: knot Y should be Stranded too — both ends are in the knot"),
		Binding.GetStructure().GetPieceSupport(KnotYPiece) == EPieceSupport::Stranded);

	/*
	 * And the menu for a selection holding the released floater is empty: Delete's CanRun is
	 * !IsPieceRemoved && !IsReleased, the menu is the intersection, so one released brick empties it.
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
			TEXT(""),
			TArray<const TCHAR*>()
		},
		{
			/* One brick, singled out: the ordinary case, and the singular of the count. */
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
			/*
			 * Pick order, not handle order. FPieceSelection guarantees insertion order; a
			 * presenter that sorted would look tidy and stop matching the commit order.
			 */
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
			/* Picked but not pointed at: a list with no breakout under it. */
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
			/*
			 * The inspected brick was deselected. Still a live piece, so the only disqualifier is
			 * that it is no longer in the set. This is the row that stops "just call InspectPiece".
			 */
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
			/*
			 * The inspected brick went with a removal. Still counted and listed, but InspectPiece
			 * fails closed on it, so nothing is singled out or broken out. FStructure keeps a
			 * removed piece's last answer, and drawing that beside a gone brick claims nothing.
			 */
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
			/*
			 * And the gone brick says so rather than quoting the last solve (which still reads
			 * "supported"). This column is where InspectPiece's fail-closed answer becomes a word.
			 */
			{ TEXT("supported"), InspectorNoBrickSupportWord }
		},
		{
			/*
			 * A ref naming another wall: well-formed, resolves to nothing. Both refs carry piece
			 * index 1, so a label from the piece index alone would present two bricks as one
			 * string; qualified by structure they read apart.
			 */
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
			 * A ref missing one half. FPieceSelection refuses to store one, so it is unreachable
			 * through the controller, but a readout takes its list from wherever it is handed, and
			 * the fail-closed answer is "counted, listed, never singled out". Zero is a real
			 * structure and piece, so the sentinel is named rather than tested for truthiness.
			 *
			 * Its label is pinned rather than left to fall out: "brick 4:-1". The rule is total, so
			 * -1 is printed as what it is, a piece index no wall can have, reading as absent.
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
			/*
			 * The case that decides sibling-versus-row. The menu is empty (asserted above) and the
			 * debugger must be full: both bricks listed, the live one singled out, three joints broken out.
			 */
			TEXT("the menu is empty because one brick is released, and the debugger is not"),
			{ MakeRef(InspectorStructure, SubjectPiece), MakeRef(InspectorStructure, FloaterPiece) },
			MakeRef(InspectorStructure, SubjectPiece),
			{ MakeRef(InspectorStructure, SubjectPiece), MakeRef(InspectorStructure, FloaterPiece) },
			{ TEXT("course 2 · #1"), TEXT("course 4 · #1") },
			/*
			 * Both entries are live, including the released one. This pins what bIsLivePiece means:
			 * the floater is handed to physics and Delete refuses it (so the menu is empty), but it
			 * is still a piece with a support state and joints, so greying it out would report it gone.
			 */
			{ true, true },
			TEXT("2 bricks selected"),
			0,
			3,
			TEXT("supported"),
			TEXT("3 joints"),
			/*
			 * And the released brick reads "falling" while reading live: two true things at once,
			 * still a piece (do not grey it out) and unsupported (say so). The menu is empty here,
			 * so this word is the panel's only explanation of why.
			 */
			{ TEXT("supported"), TEXT("falling") }
		},
		{
			/*
			 * A brick with no joints is still a brick. The floater is live and joined to nothing,
			 * so an empty breakout is the truth about it, which is why "is one inspected" is a
			 * field and not Joints.Num() > 0. It is the row that needs the empty sentence: the model
			 * hands over the words so the widget needs no branch, exactly as CountText does.
			 */
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
			/*
			 * A duplicate is presented twice. FPieceSelection is a set and cannot produce this;
			 * the point is that the presenter is a projection of a list, not a second set. A
			 * presenter built on a TSet would pass every other row and lose the order and this.
			 */
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
			/*
			 * The duplicate, pointed at. BuildPieceMenuInspector marks membership by index, so the
			 * first occurrence is inspected. The obvious `Entry.bIsInspected = (Entry.Ref ==
			 * InspectedRef)` marks both, drawing one breakout under two headings. So the assertion
			 * is two facts: exactly one entry reads inspected, and it is the first occurrence.
			 */
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
			/*
			 * A brick the solver gave up on reads "stranded", not "grounded", the two words in
			 * PresenterWordForSupport furthest apart: grounded rests on earth, stranded is in a
			 * knot the solver could not route. Swapping the arms is invisible to every other row
			 * here. The knot is a live piece with two real head joints, so this is a full readout.
			 */
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
			/*
			 * And the row says "stranded" too, the word the list cannot lose: it tells the player
			 * the solver could not route this brick, so the numbers may be worth doubting.
			 */
			{ TEXT("stranded") }
		},
	};

	for (const FInspectorCase& Case : Cases)
	{
		const FPieceMenuInspector Inspector =
			BuildPieceMenuInspector(Binding, Case.Selected, Case.Inspected);

		CheckInspectorInvariants(*this, Inspector, Case.Description);

		/*
		 * And the two halves of the panel name bricks the same way: the joint lines call the far
		 * end what the entry labels do, on every case that breaks joints out, including the knot.
		 */
		CheckFarEndsReadAsPositions(*this, Binding, Inspector, Case.Description);

		/*
		 * The count, the first thing a player reads. Asserted twice: as a number anything can
		 * threshold, and as the sentence the widget prints, since "1 brick" vs "1 bricks" is an
		 * untestable branch in the widget.
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

		/* Table integrity: a short label list would silently skip entries past its end. */
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
				 * The label is "course <C> · #<N>" for a placeable brick and "brick
				 * <StructureId>:<PieceIndex>" for one this binding cannot place, both total and
				 * neither mistakable for the other. A position, because "brick 4:282" is an array
				 * subscript a person at a wall cannot check. The fallback is kept word for word
				 * because a ref naming another wall or missing a half has no position; qualifying
				 * by structure tells "{4,1}" and "{9,1}" apart. Removed piece 3 still reads "course
				 * 2 · #2", since FPieceBinding keeps its box. Asserted per entry, not through
				 * DescribeInspector, which prints the label in failure messages only.
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
				 * Whether the entry still names a brick you can act on, decided here, not by
				 * whoever draws it. Without this field a removed brick, a foreign ref and a live
				 * brick present identically, so greying the dead one out would need the widget to
				 * resolve refs itself. Expectations come from the diagram per row, not the binding.
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

				/*
				 * And why that brick is or is not standing up, on its own row. Position and
				 * liveness are about identity, so without this a selection says nothing about what
				 * the wall is doing until the player hovers each row. The model already has the
				 * answer: InspectPiece runs once per entry to decide bIsLivePiece and the support
				 * state comes back on the same struct. Read back, so the words are the readout's own
				 * ("grounded", "supported", "stranded", "falling", "not solved yet"); a second
				 * vocabulary would be two panels in one.
				 */
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

		/*
		 * And the joint list gets a sentence, the same rule as CountText: singular, plural and
		 * the empty case are decided here, not left as a branch in the widget.
		 */
		TestEqual(
			FString::Printf(TEXT("%s: the joint list should read '%s', it reads '%s' %s"),
				Case.Description, Case.ExpectedJointsText, *Inspector.JointsText,
				*DescribeInspector(Inspector)),
			Inspector.JointsText, FString(Case.ExpectedJointsText));
	}

	/*
	 * "Nobody has solved yet" is its own sentence, on its own fixture. EPieceSupport::Falling is
	 * both a real collapse and an absent answer (enumerator zero promises least), so a freshly
	 * built wall drawn as falling bricks reports a catastrophe that has not happened. The joints
	 * are still listed, which keeps this different from "no brick inspected".
	 */
	{
		FStructureBinding Unsolved;
		BuildWorkedFixture(Unsolved, /*bSettle*/ false);

		const TArray<FPieceRef> JustTheSubject = { MakeRef(InspectorStructure, SubjectPiece) };

		const FPieceMenuInspector Inspector = BuildPieceMenuInspector(
			Unsolved, JustTheSubject, MakeRef(InspectorStructure, SubjectPiece));

		CheckInspectorInvariants(*this, Inspector, TEXT("a wall nobody has solved"));

		/*
		 * And a brick's neighbours are named by position whether or not anybody has solved: a
		 * position is a fact about the boxes, so it must not go quiet the way the loads do.
		 */
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

		/*
		 * And the joint count is a fact about the graph, not the solve: three joints exist
		 * whether or not the wall has been solved, so this sentence must not go quiet.
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

		/*
		 * And the entry's column says the same as the readout, the state it is most likely to get
		 * wrong: a column taking the enumerator at face value would draw a freshly built wall as a
		 * list of falling bricks, far louder across forty rows than in one readout.
		 */
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
 * The joint breakout is InspectPiece's answer in a human's units and words: same rows, same
 * order, newtons instead of uu, per cent instead of a ratio, nothing recomputed.
 *
 * Exact equality against a live InspectPiece call, not constants, because this project has paid
 * twice for a re-derivation that agrees to nine places and differs in the last bit. Passthrough
 * fields are held with ==; the two converted ones against that answer times a factor spelled out
 * here. Hand-derived physical values are fixture preconditions, so a wrong fixture says so.
 *
 * The severed joint is the discriminator, which is why the fixture pulls a brick out: a given
 * joint is dropped from the solver's support lists before the tier is decided, so a presenter
 * building its own adjacency would show two rows where three are due, every number correct. Only
 * a brick with a gone joint separates "read InspectPiece" from "did it again".
 *
 * A given joint must not read like an intact unloaded one: both are 0 N at 0%, but one is a hole
 * in the wall. bHasGiven is carried for colour and the line says so in words, so the widget needs
 * no branch.
 *
 * Units: 1 N = 100 uu, and the only named Core factor is ForceUnitsPerMPaSqCm = 10,000, so
 * reaching for the wrong one is a clean 100x a tuned readout hides.
 *
 * Needs a ticking world: no, and not even a world.
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
	 * Fixture preconditions, hand-derived and asked of the graph. Conn 1 carries the rider's
	 * 3 x 980 = 2940 uu; conn 0 carries the subject's 2 kg plus the rider's 3, 5 x 980 = 4900 uu.
	 * Both antiparallel to a vertical normal, so shear and tension are zero and compression
	 * governs. Conn 2 was severed with the spare and carries nothing. Green on arrival, driving nothing.
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

	/* The model's own answer, taken once; asserting against it is what tests "not recomputed". */
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

		/*
		 * Ascending connection order, passed through: the list must not reshuffle between two
		 * looks, and connection order is the only stable one. Identity and ordering in one check.
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
		 * The two converted fields, held with exact equality against the model's number. Exact,
		 * not near, because a recomputed utilisation would agree to fifteen places and differ in
		 * the last bit, which a tolerance lets through. The spelling must be `Force.Size() /
		 * ForceUnitsPerNewton` and `Utilisation * 100.0`; a multiply by 0.01 would be an ulp out,
		 * 0.01 not being representable while the rounded quotient is the decimal reading.
		 */
		const double ExpectedForceN = Model.ForceUu.Size() / InspectorForceUnitsPerNewton;

		TestTrue(
			*FString::Printf(
				TEXT("row %d should read %.9f N — %.6f uu at 100 uu per newton — it reads %.9f"),
				Index, ExpectedForceN, Model.ForceUu.Size(), Row.ForceN),
			Row.ForceN == ExpectedForceN);

		/*
		 * And the moment, through the same constant. A moment is uu.cm and length is already
		 * centimetres, so this is ForceN's unit change, not a second boundary (MOMENTS_DESIGN.md).
		 * The magnitude, since which way a joint is levered open is not something a line of text says.
		 */
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
	 * And the lines themselves, which are what a player reads. Wording is pinned exactly, since
	 * the widget landed under a recorded exception that every string be decided where a test can
	 * reach it. Each line names the joint, neighbour and tier, then either the load or the reason
	 * there is none: an intact and a gone joint are both 0 N at 0%, so they must be different
	 * sentences, not a widget branch. Numbers render at 1 decimal for newtons and 3 for per cent.
	 *
	 * Each intact line carries its margin, the half a player can read: "2041x margin" says
	 * "0.049%" as a sentence (1 / 0.00049 = 2040.8), an integer at or above 100x because a tenth
	 * of a multiple that large is noise. Other states are pinned in PieceMenuJointHeadroom.
	 *
	 * A given joint carries no margin, because it carries nothing. Its margin reading is still
	 * asserted as a field in the headroom test, so it cannot become the intact-unloaded sentence.
	 *
	 * The far end is named by position, exactly as the entry list names it: the row exists so a
	 * player can find the brick across the joint, which a subscript cannot help. The three far
	 * ends are the pad (course 1 · #1), rider (course 3 · #1) and spare (course 2 · #2). The
	 * severed joint's far end is still a position, since FPieceBinding keeps the removed brick's
	 * box. The "#<n>" prefix stays: a joint has no position, and the connection index is its only
	 * name and the handle for GetConnectionForce.
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
	 * The third sentence, "broke in pass N", is not asserted here: it needs
	 * FStructure::SolveAndBreak, which FStructureBinding does not expose yet (CURRENT_STATE.md's
	 * item 1), so no binding can reach that state. The field is still covered: BreakPass is
	 * asserted above as InspectPiece's own answer, so only the sentence is left to write. The
	 * distinction is pinned one layer down by PieceInspection.JointBreakout.
	 */

	return true;
}

/** Named and unique again; see the note on PieceInspectorTestSupport. */
namespace PiecePositionTestSupport
{
	using namespace PieceInspectorTestSupport;

	/** A structure id distinct from the worked fixture's, so a fallback label reads apart. */
	constexpr int32 PositionStructure = 12;

	/**
	 * How far apart two bricks' centres may sit and still be one course. Spelled out here, not
	 * imported. Half a centimetre: a course rises 7.5 cm (the 21.5 x 10.25 x 6.5 brick plus a
	 * mortar joint), so 0.5 cm is a fifteenth of that, leaving room for float noise without
	 * merging courses. Set near the tight end because the directions are asymmetric: too tight
	 * gives one course per brick, too loose merges courses and two bricks share a position
	 * number, the failure the label rule exists to prevent. Absolute distance, so pieces under
	 * ~1 cm tall would band together; nothing in the game builds one.
	 */
	constexpr double PositionCourseToleranceCm = 0.5;

	/**
	 * A real IEEE NaN and +infinity, made through a volatile so nothing folds them away. They
	 * must be genuine, not merely enormous: IsFinite rejects both but passes
	 * TNumericLimits<double>::Max().
	 */
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

	/** One arrangement of bricks, and what every handle in it should read. */
	struct FPositionCase
	{
		const TCHAR* Description = nullptr;

		/** One box per handle, in handle order. */
		TArray<DestructionLayout::FPieceBox> Boxes;

		/** Handles taken back out before the labels are read. */
		TArray<int32> Removed;

		/** What each handle should read, in handle order. */
		TArray<FString> ExpectedLabels;
	};

	/** Build a binding of boxes alone: labels need no joints, no solve and no world. */
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

	/** Every handle of the fixture, selected in handle order. */
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
 * A brick is named by where it is ("course 2 · #1"), and two different bricks are never named
 * the same thing.
 *
 * The derivation: sort placeable pieces by centre Z. Band them (a piece joins the course whose
 * lowest member it is within half a centimetre of, else it starts a new one). Number courses
 * from the bottom, from one. Within a course, order by X, then Y, then handle, and number from
 * one. An unplaceable piece keeps the old "brick <StructureId>:<PieceIndex>".
 *
 * Band against the course's floor, not the previous piece: chaining each piece to the last lets
 * a run of 0.4 cm steps merge two real courses into one. Anchoring to the floor bounds a course's
 * spread at the tolerance. "Near misses must not chain into one course" below is the only row
 * that separates the two.
 *
 * Order by (X, Y, handle), not just X: two selected bricks must never share a string, and X
 * alone fails on a two-leaf wall (two bricks of one course at one X, differing in depth). Y
 * settles that; the handle settles two pieces at exactly one point. The result is a total order,
 * so no two share an ordinal, swept at the end of the test.
 *
 * Removed pieces are still placed: FPieceBinding keeps their box, and a course that renumbered
 * when a brick was pulled would rename every brick to its right. A label must not depend on what
 * happened to its neighbours.
 *
 * Degenerate positions fail closed to the old label: a NaN or infinite centre cannot be banded
 * (every comparison against NaN is false), so it is excluded entirely, taking no position and
 * disturbing no one else's.
 *
 * Needs a ticking world: no, and not even a world.
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

	/** Comfortably inside the tolerance, and comfortably outside it. */
	const double WellInside = PositionCourseToleranceCm * 0.8;
	const double WellOutside = PositionCourseToleranceCm * 4.0;

	const TArray<FPositionCase> Cases = {
		{
			/*
			 * The ordinary case, and the one separating position order from handle order. The
			 * boxes are scrambled (handle 0 is upper-right), so numbering along a course by handle
			 * would agree with a bottom-up left-to-right fixture and disagree here.
			 */
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
			/*
			 * A course is a band, not a plane: bed joints vary, so bricks a few millimetres apart
			 * in Z are one course. A bit-exact rule would give one course per brick on any jitter.
			 */
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
			/*
			 * And the band has an edge: without one every brick in a forty-course wall is course
			 * 1, which also breaks uniqueness as forty bricks compete for one set of numbers.
			 */
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
			 * The only row that pins which piece the tolerance is measured from. Four pieces, each
			 * 0.4 cm above the last: every gap is inside the tolerance but the total span is 1.2 cm.
			 * Against the floor they are two courses of two; against the previous piece they chain
			 * into one course of four. The two implementations differ on exactly this shape.
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
			/*
			 * Two leaves: an ordinary wall that breaks an X-only ordering. Both bricks are course
			 * 1 at X 0, differing only in depth, so ordering by X alone gives them one number.
			 */
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
			/*
			 * The degenerate one: two pieces at exactly the same point. No wall produces it, but a
			 * caller can hand it in. The piece handle is the last resort, unique by construction.
			 */
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
			/*
			 * An uncomputable position falls back and disturbs nothing: the two good bricks still
			 * read #1 and #2, so the unplaceable ones take no number rather than consuming one. A
			 * NaN let through would band into its own course wherever the sort left it.
			 */
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
			/*
			 * And pulling a brick out renames nothing, not the brick nor its neighbours. Removing
			 * the middle brick leaves everything reading as it did; skipping removed pieces would
			 * slide the third from #3 to #2, renaming a brick because a different one changed.
			 */
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
		/* Table integrity: a short expectation list would silently skip the tail. */
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

		/*
		 * And the promise itself, swept over every row: the property the label rule exists for,
		 * checked for a collision on every case, including where none is being demonstrated.
		 */
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
	 * The ladder's one joint size, chosen so the arithmetic is doable in the head. Mortar takes
	 * 10 MPa in compression, so 49 cm2 holds 49,000 N = 4,900,000 uu, and a brick of M kg
	 * (M x 980 uu) loads the joint to 980 M / 4,900,000 = M / 5000 of capacity, margin 5000 / M.
	 * 5 kg is a thousandth, 50 a hundredth, 500 a tenth, 5000 the limit: four decades, no
	 * rounding. Every load is antiparallel to a vertical normal, so compression governs.
	 */
	constexpr double LadderJointAreaSqCm = 49.0;
	constexpr double LadderMassPerFullLoadKg = 5000.0;

	/** M / 5000, worked above. Hand-derived, never read back off the graph. */
	double LadderUtilisation(double MassKg)
	{
		return MassKg / LadderMassPerFullLoadKg;
	}

	/*
	 * The load ladder: one grounded pad with a row of bricks on it, each on its own bed joint,
	 * each a different weight. A ladder, not a column, so each rung is an independent dial:
	 * bricks side by side send their whole weight down their own joint, where a column ties the
	 * loads together. The pad is grounded, so its own weight never appears. Inspecting the pad
	 * breaks out the whole ladder in one readout, in ascending connection order. Not a wall: the
	 * geometry is synthetic and no box decides anything; the load path and strengths are real.
	 */
	constexpr int32 LadderPadPiece = 0;

	/** Handle 11: a SECOND grounded pad, so the head joint between them carries nothing. */
	constexpr int32 LadderNoLoadPiece = 11;

	/** Handle 12: pulled out before the solve, so its joint is severed without ever failing. */
	constexpr int32 LadderRemovedPiece = 12;

	/**
	 * Handle 13: a real piece carrying a real load that no binding can place. Its centre is a
	 * NaN, so it takes no position (every comparison against NaN is false). FStructure is
	 * position-free, so the box never reaches the solver: it has a mass, joint, tier and an
	 * ordinary load, and only its location is unknown. That makes it a clean test of the
	 * fallback, carrying exactly what rung 0 does so the two rows differ in one field. A far end
	 * is a bare handle in the inspected brick's own structure, so an unplaceable box is the only
	 * way a joint row reaches the fallback.
	 */
	constexpr int32 LadderUnplaceablePiece = 13;

	/** The same 5 kg as rung 0, so the two rows differ ONLY in how the far end is named. */
	constexpr double LadderUnplaceableMassKg = 5.0;

	/**
	 * Handle 14: the rung whose load does not come down the middle of its joint. Every other rung
	 * is geometry-free and unbent, which is why this one exists: a line printing only force and
	 * per cent agrees with all twelve, and only on a bent joint do the two stop explaining each
	 * other. This rung carries 548.8 N, barely more than rung 2's 490 N, but sits at 49% where
	 * rung 2 sits at 1%.
	 *
	 * The arithmetic, derived here rather than read back:
	 *
	 *   force        56 kg x 980 = 54,880 uu, which is 548.8 N
	 *   lever arm    the brick's centre of mass sits 4 cm along X from the joint's centroid
	 *   moment       r x F, and a vertical force crossed with a lever arm along X lands
	 *                wholly on Y: 4 x 54,880 = 219,520 uu.cm, which is 2,195.2 N.cm
	 *   section      6 cm along X by 8 cm along Y: area 48 cm2, and the modulus resisting a
	 *                lean along X is the textbook b.d2/6 = 8 x 36 / 6 = 48 cm3 — deliberately
	 *                NOT production's (4/3).h_along.h_across2, so the two agreeing is evidence
	 *   stresses     mean  54,880 / (48 x 10,000) = 0.1143333 MPa, compressive
	 *                edge 219,520 / (48 x 10,000) = 0.4573333 MPa
	 *   peak tension 0.343 MPa against mortar's mean 0.7, so 0.49 of capacity
	 *
	 * Tension governs by 8.6x: peak compression is the sum, 0.5716667 against 10 MPa, and shear
	 * is zero (load antiparallel to a vertical normal).
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

	/**
	 * Every rung's mass, in handle order from handle 1, four decades first. Handles 9 and 10 are
	 * the kilonewton boundary pair: 100000/980 kg is exactly 1000.0 N and 99990/980 kg exactly
	 * 999.9 N, pinning which side of a thousand newtons switches unit.
	 */
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
	 * The one rung that knows the shape of its own face, so the one that bends. A separate helper
	 * because a rectangle and area must agree (AddConnection refuses otherwise), and the twelve
	 * joints above are 49 cm2 with no rectangle; giving them one would bend every rung.
	 *
	 * @return the connection index, or INDEX_NONE if the door refused it.
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

		/* The second grounded pad, beside the first: a head joint with nothing to carry. */
		Out.AddPiece(
			1.0, /*bIsGrounded*/ true, nullptr,
			InspectorBoxAt(-InspectorBrickPitchCm, InspectorCourseZ(0)));

		/* And the brick that gets pulled out, severing its joint without it ever failing. */
		Out.AddPiece(
			5.0, false, nullptr,
			InspectorBoxAt(InspectorBrickPitchCm * 11.0, InspectorCourseZ(1)));

		/* And the one nobody can place: ordinary X, NaN Z, so the whole centre is unusable. */
		{
			DestructionLayout::FPieceBox Nowhere =
				InspectorBoxAt(InspectorBrickPitchCm * 12.0, InspectorCourseZ(1));

			Nowhere.CentreCm.Z = PiecePositionTestSupport::PositionNaN();

			Out.AddPiece(LadderUnplaceableMassKg, false, nullptr, Nowhere);
		}

		/*
		 * And the rung that leans. Its box centre is the centre of mass the binding hands the
		 * solver, so the 4 cm to the joint's centroid is the whole lever arm. Placed a full pitch
		 * past the pulled brick so its position is course 2 · #12, the unplaceable rung between
		 * them taking no position.
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

		/* Appended last, so every connection index the table below names stays put. */
		AddLadderJoint(Out, LadderUnplaceablePiece, InspectorBedNormal);

		const int32 EccentricJoint = AddEccentricLadderJoint(
			Out,
			LadderEccentricPiece,
			InspectorBrickPitchCm * 13.0 - LadderEccentricLeverArmCm);

		Out.RemovePiece(LadderRemovedPiece);
		Out.SolveLoads();

		return EccentricJoint;
	}

	/** One rung of the ladder, as it should read. */
	struct FHeadroomCase
	{
		const TCHAR* Description = nullptr;

		/** Which connection, which is also its position in the breakout. */
		int32 ConnectionIndex = INDEX_NONE;

		/** What the joint carries, hand-derived: M x 980 uu, over 100 uu per newton. */
		double ExpectedForceN = 0.0;

		/** M / 5000, as a percentage. */
		double ExpectedUtilisationPercent = 0.0;

		/** The reading beside it. */
		const TCHAR* ExpectedMarginText = nullptr;

		/** clamp(log10(5000 / M) / 3, 0, 1), worked out by hand per row. */
		double ExpectedHeadroom = 0.0;

		/** The whole line. */
		const TCHAR* ExpectedLine = nullptr;

		/**
		 * Which band the bar is drawn in: comfortable above 10x margin, cautious below, critical
		 * at or below 2x and for a gone joint. A column here because the band is a transform of
		 * the utilisation two columns left. This ladder holds nothing between 1x and 10x, so the
		 * amber/red edge at 2x is pinned in Presenter.PieceMenuJointMarginBand.
		 */
		EJointMarginBand ExpectedBand = EJointMarginBand::Critical;

		/**
		 * What the joint is being bent by, in newton-centimetres, and zero on twelve of the
		 * thirteen rows. A settled wall bends nowhere (centre of mass at the supports' centroid,
		 * eccentricity exactly zero), so a line carrying a bending clause anyway would burden the
		 * common case. The twelve zeroes assert the sentence does not grow.
		 */
		double ExpectedMomentNCm = 0.0;
	};
}

/**
 * A joint says how many times its load it could take, in newtons or kilonewtons, plus a
 * log-scaled bar fraction and the scale that makes it readable.
 *
 * Margin, because "0.470%" means nothing without knowing 100% is failure and masonry sits orders
 * of magnitude under it. "213x margin" says it in a phrase, and it is the reciprocal of a number
 * already on the row, so nothing is recomputed.
 *
 * The three non-number readings are the point of the table, each a state where the obvious
 * formula is plausible and wrong:
 *
 *   - An unloaded joint divides by zero: infinite margin, printed as junk. It reads "no load".
 *   - A joint at or past its limit reads "0.5x margin", the word margin beside a joint that has
 *     none, the fail-open direction. It reads "no margin left", at 1.0 and above.
 *   - A given joint carries nothing, so the formula reads it like an unloaded one. It reads "gone".
 *
 * The bar is log-scaled over three decades because a linear one is empty forever: a settled wall
 * sits near 0.0005 of capacity. Full is 1000x margin, empty is the joint giving, most joints peg
 * full and honestly so.
 *
 * So it needs labels, and the model supplies them, asserted as text and position and cross-checked
 * against the curve: the joint whose margin is exactly 10x must fill the bar to the "10x" tick.
 *
 * Every far end is named by position. The ladder says what the worked fixture cannot: twelve far
 * ends across two courses, one pulled out and one whose box says nowhere, the only way a joint row
 * reaches the ref-shaped fallback.
 *
 * The force picks its unit at a thousand newtons; the rungs at exactly 1000.0 N and 999.9 N pin
 * which side switches. A joint being levered open says so while the twelve unbent ones say nothing
 * extra: the last rung carries 548.8 N at 49% where rung 2 is at 1%, the missing term a 2,195.2
 * N.cm bend (MOMENTS_DESIGN.md).
 *
 * Needs a ticking world: no, and not even a world.
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

	/*
	 * Fixture precondition, a door not a number: AddConnection refuses a rectangle that
	 * disagrees with its area and answers INDEX_NONE, leaving the ladder a rung short.
	 */
	TestEqual(
		FString::Printf(
			TEXT("fixture: the eccentric rung's joint must be accepted and land last; AddConnection returned %d"),
			EccentricJoint),
		EccentricJoint, 13);

	/*
	 * Fixture preconditions, hand-derived and asked of the graph. Green on arrival, driving
	 * nothing; they exist so a ladder that stopped being a ladder says so.
	 */
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

	/*
	 * And the unplaceable brick's joint is ordinary: FStructure never sees a box, so the NaN
	 * centre never reaches the solve, making the row below a test of the label, not the arithmetic.
	 */
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

	/*
	 * And the bent rung, asked of the graph and derived here. Green on arrival; they buy that
	 * the row below is held against a joint that genuinely bends, so a ladder whose eccentricity
	 * went to zero would not agree with a presenter that never fetched a moment.
	 */
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

		/*
		 * And tension governs, not compression, which is what makes this row about a joint being
		 * peeled open. ComputeUtilisation returns the worst of three axes: peak compression is the
		 * sum of the two stresses over 10 MPa, peak tension their difference over 0.1 MPa, and
		 * shear is zero. A retuned profile that flipped it would leave every number above unchanged.
		 */
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
	 * The ladder, rung by rung. Force and per cent are restated here, not taken on trust:
	 * PieceMenuJointReadout holds them against InspectPiece with exact equality, so the two fail
	 * differently for a passthrough break than an arithmetic one.
	 *
	 * Far ends are named by position, off the ladder's boxes. The bottom course holds the two
	 * grounded pads (handle 11 at X -22.5 is course 1 · #1, the inspected pad at X 0 is #2); the
	 * course above holds the ten rungs #1 to #10, the pulled brick at #11. Nine of the twelve far
	 * ends are "course 2 · #<handle>", so a handle in a position's clothes passes those and fails
	 * three: the head joint to the second pad, the severed joint to handle 12, and the unplaceable
	 * brick.
	 */
	const TArray<FHeadroomCase> Cases = {
		{
			TEXT("a thousandth of capacity: the top decade, and the bar is full"),
			0, 49.0, 0.1, TEXT("1000× margin"), 1.0,
			TEXT("#0  course 2 · #1  bed above  generalpurposemortar  49.0 N  0.100 %  1000× margin"),
			EJointMarginBand::Comfortable
		},
		{
			/* A hundredth, pinning the format boundary from above: 100x is an integer, 98.04x below keeps a decimal. */
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
			/*
			 * Exactly at the limit. FConnection holds 1.0 as fully loaded but still holding, so
			 * this joint is intact at 49 kN with nothing spare. The naive "1.0x margin" reads like room.
			 */
			TEXT("exactly at the limit: the bar is empty and there is no margin to quote"),
			3, 49000.0, 100.0, TEXT("no margin left"), 0.0,
			TEXT("#3  course 2 · #4  bed above  generalpurposemortar  49.0 kN  100.000 %  no margin left"),
			EJointMarginBand::Critical
		},
		{
			/*
			 * Off the top of the bar. Four decades against the bar's three, so the fraction clamps
			 * full, but the margin is quoted in full: clamping the bar is drawing, rounding the
			 * number would lose information.
			 */
			TEXT("four decades of margin: the bar pegs full and the number does not"),
			4, 4.9, 0.01, TEXT("10000× margin"), 1.0,
			TEXT("#4  course 2 · #5  bed above  generalpurposemortar  4.9 N  0.010 %  10000× margin"),
			EJointMarginBand::Comfortable
		},
		{
			/*
			 * Just under the format boundary: 5000/51 is 98.0392..., which keeps its decimal
			 * where the 100× rung loses it. The pair makes the "at or above 100×" rule falsifiable.
			 */
			TEXT("just under a hundred times: still a decimal"),
			5, 499.8, 1.02, TEXT("98.0× margin"), 0.66379994274602749,
			TEXT("#5  course 2 · #6  bed above  generalpurposemortar  499.8 N  1.020 %  98.0× margin"),
			EJointMarginBand::Comfortable
		},
		{
			/*
			 * Twice the limit, the fail-open row. The reciprocal is 0.5, so the naive line reads
			 * "0.5× margin" beside a joint at 200%. Past capacity and at capacity are the same
			 * sentence: both have nothing left, and a second wording would be a decorative branch.
			 */
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
			/*
			 * Exactly one thousand newtons, the unit switch from above: 100000 uu / 100 = 1000.0 N
			 * with no rounding, so this pins the boundary itself. At the boundary it reads
			 * kilonewtons: "1.0 kN", not the digit-counting "1000.0 N".
			 */
			TEXT("exactly one thousand newtons reads in kilonewtons"),
			8, 1000.0, 2.0408163265306123, TEXT("49.0× margin"), 0.56339869334283788,
			TEXT("#8  course 2 · #9  bed above  generalpurposemortar  1.0 kN  2.041 %  49.0× margin"),
			EJointMarginBand::Comfortable
		},
		{
			/* And one tenth of a newton under it, which does not. */
			TEXT("a tenth of a newton below the switch still reads in newtons"),
			9, 999.9, 2.0406122448979592, TEXT("49.0× margin"), 0.5634131705494404,
			TEXT("#9  course 2 · #10  bed above  generalpurposemortar  999.9 N  2.041 %  49.0× margin"),
			EJointMarginBand::Comfortable
		},
		{
			/*
			 * Nothing on it. Two grounded pads share a head joint, and grounded terminates the
			 * flow, so this joint carries zero: intact and unloaded, an ordinary thing. Margin
			 * divides by zero and the bar is full, unloaded being the most headroom there is. It
			 * is also the one row whose far end is in a different course, "course 1 · #1" where its
			 * neighbours read course 2, separating a real position from a handle dressed as one.
			 */
			TEXT("an intact joint carrying nothing has no margin figure and a full bar"),
			10, 0.0, 0.0, TEXT("no load"), 1.0,
			TEXT("#10  course 1 · #1  head  generalpurposemortar  0.0 N  0.000 %  no load"),
			EJointMarginBand::Comfortable
		},
		{
			/*
			 * And the joint that is not there any more. It reads 0 N at 0%, bit for bit identical
			 * to the rung above, but one is a hole in the wall. Its bar is empty where the unloaded
			 * joint's is full, the distinction in the one field a player looks at, and its margin
			 * is a different word, not number. Its far end is still a place: the brick was pulled
			 * out and FPieceBinding kept its box, so the joint says where the hole is. Handle 12 is
			 * the eleventh brick of its course (the ten rungs come first), so a handle printed as a
			 * position would read "#12" and be wrong by one.
			 */
			TEXT("a joint that has given reads apart from an intact joint with nothing on it"),
			11, 0.0, 0.0, TEXT("gone"), 0.0,
			TEXT("#11  course 2 · #11  bed above  broken (went with a removed piece)"),
			EJointMarginBand::Critical
		},
		{
			/*
			 * And the far end nobody can place, the only way a joint row reaches the fallback. A
			 * far end is a bare handle in the inspected brick's own structure, so it can be neither
			 * foreign nor half-missing; what is left is a brick whose box says nowhere. It spells
			 * the entry label's fallback word for word: "brick 21:13", the binding's structure id
			 * and the handle. That cannot collide with a positioned far end (no colon, no "brick"),
			 * and it is the same string the entry list prints, so the two panels agree on one brick.
			 * Its load is rung 0's exactly, so the rows differ in one field: a NaN centre is
			 * invisible to the solver, so this is a healthy joint with an unnameable brick on it.
			 */
			TEXT("a far end nobody can place falls back to the ref-shaped label"),
			12, 49.0, 0.1, TEXT("1000× margin"), 1.0,
			TEXT("#12  brick 21:13  bed above  generalpurposemortar  49.0 N  0.100 %  1000× margin"),
			EJointMarginBand::Comfortable
		},
		{
			/*
			 * And the one joint being levered open, the row the other twelve cannot write. It
			 * carries 548.8 N, barely more than rung 2's 490, yet sits at 49% where rung 2 is at
			 * 1%. Both numbers are true and there is no arithmetic between them: the missing term is
			 * a 2,195.2 N·cm bend nothing on the line mentions (MOMENTS_DESIGN.md).
			 *
			 * The moment, not the eccentricity: M / F is a lever arm, but it is derived (the
			 * second-copy shape PieceInspection.h is written against), it divides by a force free to
			 * be small, and MOMENTS_DESIGN.md slice 5 makes a joint receive moment from above, at
			 * which point M / F describes nothing. The moment is also what ComputeUtilisation
			 * consumes, so the line names both inputs to the percentage.
			 *
			 * And N·cm, not N·m, pinned like the kilonewton switch: centimetres are this game's
			 * length unit, and the range collapses in metres (a 313.6 N·cm bend is 3.136 N·m). No
			 * new boundary: a moment is uu.cm, so this is ForceUnitsPerNewton applied once. The
			 * clause trails its number like every other and appears only on a bent joint; the
			 * twelve rows above assert the absence.
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
	 * And the moment on every row is the model's own number, exactly. PieceMenuJointReadout
	 * sweeps against InspectPiece too, but its fixture has no joint geometry, so every moment
	 * there is 0 == 0. The ladder bends on one rung, so the claim is worth something here: a
	 * recomputed moment would agree to fifteen places and differ in the last bit.
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

		/*
		 * The bar's fraction, to twelve places, against a hand-worked number. A log curve keeps a
		 * wrong answer plausible: a natural log, or two decades instead of three, still moves the
		 * right way. The decade rows separate those: 1000x, 100x, 10x and 1x must land on 1, two
		 * thirds, one third and zero, and only clamp(log10(margin)/3) does.
		 */
		TestEqual(
			FString::Printf(TEXT("%s: the bar should fill to %.12f, it fills to %.12f %s"),
				Case.Description, Case.ExpectedHeadroom, Row.HeadroomFraction,
				*DescribeInspector(Inspector)),
			Row.HeadroomFraction, Case.ExpectedHeadroom, 1e-12);

		TestEqual(
			FString::Printf(TEXT("%s: the line should read '%s', it reads '%s'"),
				Case.Description, Case.ExpectedLine, *Row.Text),
			Row.Text, FString(Case.ExpectedLine));

		/*
		 * And the band the bar is drawn in. Every bar is the same green today, so a joint at 200%
		 * and one at a ten-thousandth differ only in a length nobody has a reference for. Which
		 * side of an edge a joint falls on is the model's decision; the hue is the widget's.
		 */
		TestTrue(
			*FString::Printf(TEXT("%s: should be in the %s band, it is %s %s"),
				Case.Description, NameOfBand(Case.ExpectedBand), NameOfBand(Row.MarginBand),
				*DescribeInspector(Inspector)),
			Row.MarginBand == Case.ExpectedBand);
	}

	/*
	 * The scale, pinned as text and position and tied to the curve. Ticks matching the four
	 * decade rungs is what makes the labels true, not merely present: "1000x" over a bar that
	 * fills at a hundred draws perfectly and misleads. So each tick's fraction is held against
	 * the headroom of the rung whose margin is that tick's number.
	 */
	TestEqual(
		FString::Printf(TEXT("the bar should be captioned, it says '%s'"),
			*Inspector.HeadroomCaption),
		Inspector.HeadroomCaption,
		FString(TEXT("headroom — full is 1000× margin, empty is the joint giving")));

	struct FExpectedTick
	{
		const TCHAR* Label;
		double Fraction;

		/** The rung whose margin is exactly this tick, so the two can be held together. */
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

	/*
	 * And the curve only goes one way, swept over every pair of intact rungs. A sign slip or a
	 * doubled reciprocal gives a bar smooth, bounded, correct at the ends and backwards in the
	 * middle, which no single expected value catches. More load can never mean more headroom.
	 */
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

	/*
	 * And the band is monotone in the load, swept over every pair of intact rungs like the
	 * headroom sweep: a reversed comparison or a mis-ordered guard chain colours correctly at the
	 * ends and backwards in the middle. Compared through SeverityOfBand, not the enumerators'
	 * values, whose order is fail-closed rather than a scale.
	 */
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
	 * The same 49 cm2 of mortar the headroom ladder uses, so a brick of M kg loads the joint to
	 * M / 5000 of capacity, margin 5000 / M. A second, shorter ladder rather than more rungs on
	 * the first, because the headroom table pins each line by position and connection index, so
	 * inserting rungs renumbers rows pinned character for character.
	 */
	constexpr double BandJointAreaSqCm = 49.0;
	constexpr double BandMassPerFullLoadKg = 5000.0;

	/**
	 * The four masses the boundaries need, which the main ladder cannot supply: it holds nothing
	 * between 1x and 10x, so the amber/red edge at 2x is unpinned and a split at 3x or 5x would
	 * pass every existing row. These are the two rungs either side of each edge:
	 *
	 *     499 kg   9.98 %   10.02x margin   the last comfortable joint
	 *     500 kg  10.00 %   10.00x margin   EXACTLY the green/amber edge
	 *    2499 kg  49.98 %    2.0008x        the last cautious joint
	 *    2500 kg  50.00 %    2.00x          EXACTLY the amber/red edge
	 */
	const TArray<double> BandMassesKg = { 499.0, 500.0, 2499.0, 2500.0 };

	/** A grounded pad with those four bricks sat on it, each on its own bed joint. */
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
 * A joint's bar is coloured by how much room it has left, and which side of each edge it falls
 * on is decided here, not by a widget comparing numbers.
 *
 * A model field, not a Slate ternary, because a fill is a length that means nothing without its
 * neighbours while a colour does not, and `Fraction > 0.5f ? Green : Red` writes what the game
 * considers dangerous where no test can read it.
 *
 * The edges are 10x and 2x margin, and the boundary rows are why this test exists: any two
 * implementations agree at 1000x and 200% and differ at exactly ten times and twice, rows a
 * hand-picked example never contains.
 *
 * At an edge the joint takes the worse band, the fail-closed direction (10x is amber, 2x is red),
 * for the same reason margin text says "no margin left" at 1.0: over-promising is the expensive
 * direction. Written as guards on the utilisation (comfortable below 10%, cautious below 50%,
 * critical otherwise), so a NaN lands in critical.
 *
 * And it is a transform of UtilisationPercent, never a third trip to the graph: that percentage
 * is held against InspectPiece with exact equality, and a band from the connection again would
 * be a fourth copy of the break decision.
 *
 * Needs a ticking world: no, and not even a world.
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

		/** M / 5000 as a percentage, hand-derived and asked of the graph as a precondition. */
		double ExpectedUtilisationPercent = 0.0;

		EJointMarginBand ExpectedBand = EJointMarginBand::Critical;
	};

	const TArray<FBandCase> Cases = {
		{
			TEXT("just over ten times its load: still comfortable"),
			0, 9.98, EJointMarginBand::Comfortable
		},
		{
			/*
			 * Exactly ten times, the row the "worse band at the edge" rule turns on. `Margin >= 10
			 * ? green : amber` is the coin-flip alternative and differs only on this joint.
			 */
			TEXT("exactly ten times its load: the edge, and the edge is cautious"),
			1, 10.0, EJointMarginBand::Caution
		},
		{
			TEXT("just over twice its load: still cautious"),
			2, 49.98, EJointMarginBand::Caution
		},
		{
			/*
			 * And exactly twice, the other edge: a joint that could take one more of itself is
			 * not one to describe as having room.
			 */
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

		/*
		 * Fixture precondition, hand-derived: the rung carries what this file thinks. Green on
		 * arrival, but without it a fixture that stopped loading its joints would retarget every
		 * band expectation.
		 */
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
 * Every joint row carries the colour slot of its position in the list (row 0 is always the first
 * colour), and the palette runs out rather than repeating.
 *
 * The swatch ties each row to its brick by colour, since a word ("course 2 · #4") is not enough
 * to find one in a wall of 1,220. This slice decides the slot; lighting the brick is the world half.
 *
 * Per slot, not per brick: keying on the far-end brick is what a reader assumes but cannot be
 * built, a palette being a handful of hues against a thousand bricks. Keyed on the row it never
 * collides and stays stable while scanning, at the price that one brick is the first colour in one
 * readout and the second in another, asserted here on the joint in two readouts.
 *
 * Past the end, nothing: wrapping reintroduces the collision on the brick with the most joints.
 * The load ladder reaches that end with thirteen joints on one pad.
 *
 * The structural properties (row i takes slot i, once out stays out) are swept over every readout
 * by CheckInspectorInvariants, since they must hold of the knot, unsolved wall and position fixtures too.
 *
 * Needs a ticking world: no, and not even a world.
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

	/*
	 * An ordinary brick's joints all get a colour, in list order: three rows, three slots,
	 * including the severed one, the row that would be cheapest to quietly drop.
	 */
	for (int32 Index = 0; Index < Subject.Joints.Num(); ++Index)
	{
		TestEqual(
			FString::Printf(
				TEXT("the subject's joint row %d should take colour slot %d, it took %d %s"),
				Index, Index, Subject.Joints[Index].ColourSlot, *DescribeInspector(Subject)),
			Subject.Joints[Index].ColourSlot, Index);
	}

	/*
	 * The same joint, in two readouts, in two slots: the per-slot decision as a fact. Connection 1
	 * joins subject and rider, the subject's second row and the rider's first, so slot 1 there and
	 * slot 0 here. Keying on the connection or far-end handle would give it one slot twice.
	 */
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

	/* And the ladder, the only fixture with more joints on one piece than a palette holds: fourteen off one pad. */
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

	/*
	 * Six rows is the floor, the wall's number: a brick in a running bond is spanned by two
	 * above, rests on two below and has a head joint either side. A palette running out sooner
	 * would leave the ordinary case half-coloured.
	 */
	const int32 ColouredRows = Long.Joints.IndexOfByPredicate(
		[](const FInspectorJointRow& Row) { return Row.ColourSlot == INDEX_NONE; });

	TestTrue(
		FString::Printf(
			TEXT("the palette must reach at least the 6 joints an ordinary brick has, it ran out after %d row(s) %s"),
			ColouredRows == INDEX_NONE ? Long.Joints.Num() : ColouredRows,
			*DescribeInspector(Long)),
		ColouredRows == INDEX_NONE || ColouredRows >= 6);

	/*
	 * And no two rows of one readout share a colour, the property wrapping breaks. Implied by "row
	 * i takes slot i" but asserted anyway: a modulo making rows 0 and 6 one colour points at two
	 * bricks with one hue.
	 */
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
 * Every brick row carries its support state as a bucket as well as a word, so a coloured dot is
 * the model's decision and only the hue is the widget's.
 *
 * Today FInspectorPieceEntry carries only SupportText, so a widget wanting a dot per row must
 * compare the string against literals: logic in the one place a recorded exception says there may
 * be none, failing silently both ways (stops colouring when the wording changes, colours wrong
 * when a word is added). The split is EJointMarginBand's: the bucket is the decision, the hue is taste.
 *
 * Six buckets, and the two extra are the argument. Grounded, supported, stranded and falling are
 * the four physical states. "not in this wall" is a fifth, a bucket not an absence, because the
 * alternatives are a second bool free to disagree or reusing Falling. "not solved yet" is a sixth,
 * because merging it into the fifth is a one-way door: two enumerators can share a colour by
 * lookup, one can never be split again.
 *
 * The cross-check is swept by CheckInspectorInvariants (every row's bucket against its word, the
 * marked row's against the readout's). The table below drives it red: the sweep alone passes a
 * model that never fills the field, but the per-case expectations say which bucket each state is,
 * hand-written from the diagram.
 *
 * Needs a ticking world: no, and not even a world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuSupportBandTest,
	"DestructionGame.Presenter.PieceMenuSupportBand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuSupportBandTest::RunTest(const FString& Parameters)
{
	using namespace PieceInspectorTestSupport;

	/*
	 * Table integrity first, and not bookkeeping: the sweep is "the bucket determines the word",
	 * so if two buckets shared a word it would stop separating them and pass over a merged model.
	 */
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

	/*
	 * Fixture preconditions, asked of the solver. Four of the six buckets are reachable only if
	 * the graph is in the four states the diagram claims; a fixture that stopped producing one
	 * would retarget its row.
	 */
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

	/** One row of the bucket table. */
	struct FBandCase
	{
		const TCHAR* Description = nullptr;

		TArray<FPieceRef> Selected;
		FPieceRef Inspected;

		/** One bucket per selected ref, in the same order. Hand-written from the diagram. */
		TArray<EPieceSupportBand> ExpectedEntryBands;

		/** What the readout under the list buckets as. NotAPiece when nothing is singled out. */
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
			/*
			 * Every bucket the solved wall can produce, in one list, nothing singled out: the row
			 * the column exists for. Without it seven picked bricks are seven identical rows, and
			 * the three refs that name nothing present exactly like the four that do.
			 */
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
			/* The same list with the supported brick singled out: the readout takes its bucket. */
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
			/*
			 * A released brick is a live piece nothing holds up, the row where the dot earns its
			 * place: the menu is empty, so this row's word and colour are the only explanation.
			 */
			TEXT("the released floater, singled out"),
			{ Floater }, Floater,
			{ EPieceSupportBand::Falling },
			EPieceSupportBand::Falling
		},
		{
			/*
			 * The one bucket that is not a physical claim: Stranded means the solver could not
			 * route this brick, so the numbers are worth doubting. Painting it grounded is the
			 * fail-open direction Integration.PullingSupportBringsTheWallDown polices.
			 */
			TEXT("a brick the solver stranded in a knot, singled out"),
			{ KnotX }, KnotX,
			{ EPieceSupportBand::Stranded },
			EPieceSupportBand::Stranded
		},
		{
			/*
			 * And a ref naming nothing never becomes the readout's subject, so the readout buckets
			 * as the value that claims nothing while the row still says what it is.
			 */
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

	/*
	 * And the sixth bucket, on its own fixture, the one a solved wall cannot reach.
	 * EPieceSupport::Falling is both a real collapse and an absent answer, so a bucket from the
	 * enumerator would paint a freshly built wall as one coming down, far louder across forty
	 * dots than one line of text.
	 */
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
