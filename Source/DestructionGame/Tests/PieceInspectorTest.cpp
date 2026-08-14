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

	/*
	 * THE WALL'S OWN GRID, transcribed from DestructionLayout::RunningBond rather than
	 * invented: a 21.5 x 10.25 x 6.5 cm brick on a 1.0 cm joint gives a 22.5 cm brick pitch
	 * and a 7.5 cm COURSE pitch, with the bottom course centred at half a brick height.
	 *
	 * IT MATTERS THAT THESE ARE THE REAL NUMBERS. Every course tolerance argued in this file
	 * is argued against the 7.5 cm a course actually rises, so a fixture on a made-up grid
	 * would make the tolerance look either absurdly tight or dangerously loose.
	 */
	constexpr double InspectorCoursePitchCm = 7.5;
	constexpr double InspectorFirstCourseZCm = 3.25;
	constexpr double InspectorBrickPitchCm = 22.5;

	/** Course c (0-based here; the READOUT counts from one) at brick position b. */
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
	 * WHERE EACH HANDLE OF THE WORKED FIXTURE ACTUALLY SITS, and it is now a real
	 * arrangement rather than one box per handle strung out along X.
	 *
	 *   course 4 (Z 25.75)                                    [4] Floater
	 *   course 3 (Z 18.25)   [2] Rider
	 *   course 2 (Z 10.75)   [1] Subject   [3] Spare
	 *   course 1 (Z  3.25)   [0] Pad       [6] Knot X   [5] Knot ground   [7] Knot Y
	 *                        X=0           X=22.5       X=45             X=67.5
	 *
	 * THE BOTTOM COURSE IS LAID OUT SO THAT X ORDER AND HANDLE ORDER DISAGREE. Handles 5, 6
	 * and 7 run left to right as 6, 5, 7 — so a derivation that numbered along a course by
	 * PIECE HANDLE would agree with this table on every other course in the file and be
	 * wrong about exactly these two. A wall is built bottom-up and left-to-right, so handle
	 * order is very nearly position order almost everywhere, which is precisely what makes
	 * the confusion survivable until somebody looks at a brick.
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
	 * WHAT AN ENTRY'S SUPPORT COLUMN READS WHEN THE REF NAMES NO BRICK AT ALL, SPELLED OUT HERE
	 * RATHER THAN IMPORTED FROM THE MODEL.
	 *
	 * A brick a removal took, a ref naming another wall and a ref missing a half all come back
	 * bIsPiece false, and the truthful union of the three is that the ref names nothing in the
	 * structure this panel is reading.
	 *
	 * THE OBVIOUS IMPLEMENTATION PRODUCES "not solved yet" AND THAT IS THE FAIL-OPEN ANSWER.
	 * PresenterWordForSupport asked of a default FPieceInspection sees bHasSupportAnswer false
	 * and says so — a sentence which promises the brick is there and that nobody has run a solve
	 * yet, beside a brick that is not there. It is also indistinguishable, on a freshly built
	 * wall, from every live row on the panel.
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
	 * WHAT EACH BUCKET MUST READ AS, SPELLED OUT HERE RATHER THAN ASKED OF THE MODEL.
	 *
	 * THIS IS THE PAIRING, AND THE PAIRING IS THE WHOLE CLAIM. The word is what a player reads and
	 * the bucket is what a dot beside it is coloured from, and the one thing that must never happen
	 * is a row saying "grounded" next to the colour this game uses for "falling". Two derivations
	 * of one question is exactly how that happens, so the two are held together everywhere rather
	 * than each being checked against its own column.
	 *
	 * WRITTEN OUT INDEPENDENTLY, so a model that derived the bucket from the word by comparing
	 * strings — the very policy the bucket exists to abolish — is not thereby endorsed: the words
	 * are pinned per row against a hand-worked diagram by the table in PieceMenuInspector, so this
	 * pins the bucket against those, not against production's own idea of either.
	 *
	 * IT IS INJECTIVE, AND THE NEW TEST ASSERTS THAT RATHER THAN ASSUMING IT. If two buckets ever
	 * shared a word, this sweep would stop distinguishing them and the per-row expectations would
	 * be the only thing left holding them apart.
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
	 * HOW SEVERE A BAND IS, ORDERED HERE RATHER THAN BY THE ENUMERATOR'S OWN VALUE.
	 *
	 * The numeric order of the enumerators is production's choice — zero has to be the one that
	 * promises least, which is a fail-closed argument and not a scale — so a sweep that compared
	 * the enumerators directly would be asserting against whatever order somebody happened to
	 * declare. This is the order a PLAYER reads: more load can never mean a calmer colour.
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

	/**
	 * THE ONE WORD THE PANEL CALLS ITSELF, SPELLED HERE RATHER THAN READ OFF THE MODEL.
	 *
	 * A header that is a constant in production is a constant a test may not import, for the
	 * reason every other expectation in this file is written out: a check that took the word
	 * from the thing it is checking agrees with it whatever it says, including nothing at all.
	 */
	const TCHAR* const InspectorHeaderWord = TEXT("Selection");

	/**
	 * AND THE LINE THAT STANDS IN FOR A READOUT THERE IS NOT ONE, also spelled out.
	 *
	 * It is a sentence rather than a blank because a fixed-size panel reserves the readout's
	 * space whether or not it has one, and an empty reserved region reads as a readout that
	 * failed. Which STATE it belongs to is the assertion — bricks listed, none singled out —
	 * and that is swept below rather than written per row.
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

		/**
		 * What each ENTRY's own support column reads, in the same order as ExpectedEntries.
		 *
		 * ONE PER ROW, WHICH IS THE WHOLE POINT OF THE FIELD. Eleven picked bricks are eleven
		 * identical strings today, so finding the one that is falling means hovering each of
		 * them in turn — the panel holds the answer for exactly one brick at a time while the
		 * model has it for all of them. Written out from the fixture diagram rather than read
		 * back off the binding, for the reason ExpectedLive is: a check derived the way the
		 * presenter derives it agrees with the presenter however wrong it is.
		 */
		TArray<const TCHAR*> ExpectedEntrySupport;
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

		/*
		 * AND THE PANEL SAYS WHAT IT IS, IN EVERY STATE THERE IS. It is the only line that
		 * cannot go quiet: a fixed panel is on screen while the selection is empty, and a
		 * heading that disappeared with the last brick would leave a bare count over a blank
		 * box. Swept rather than tabled because it is the same word in every row — a row-by-row
		 * expectation would be thirteen copies of one fact.
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
		 * How many rows said NOTHING about whether their brick is standing up.
		 *
		 * COUNTED AND ASSERTED ONCE RATHER THAN ROW BY ROW, because a field nothing fills is
		 * empty on every row at once and forty copies of one failure is a log nobody reads.
		 */
		int32 SilentEntries = 0;

		/**
		 * How many rows' DOT disagreed with their own WORD, and the first one that did.
		 *
		 * The bucket exists so a widget can colour a dot without comparing SupportText against
		 * string literals — a policy in the one place no test can reach, in the one file this
		 * project has a written no-logic exception for. The moment the two can disagree there are
		 * two answers to "is this brick standing up", and it is the quietest failure there is: a
		 * perfectly ordinary word beside the wrong colour.
		 *
		 * COUNTED AND ASSERTED ONCE, for the reason the silent entries are: a derivation that is
		 * wrong is wrong on every row of every case at once, and a hundred copies of one failure
		 * is a log nobody reads. The first offender is carried so the message still names one.
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
		 * AND EVERY ROW SAYS WHETHER ITS BRICK IS STANDING UP — IN EVERY STATE, INCLUDING FOR A
		 * REF THAT NAMES NOTHING AND ON A WALL NOBODY HAS SOLVED.
		 *
		 * A COLUMN THAT IS BLANK ON SOME ROWS AND FULL ON OTHERS IS THE ABSENCE THIS WHOLE STRUCT
		 * IS SHAPED AGAINST, one field further out: "No bricks selected" is a sentence rather than
		 * a blank for exactly this reason, and a widget left to notice an empty string and draw
		 * something else would be holding the branch where nothing can read it. WHICH word each
		 * state gets is the tables' job; that there is always one is swept here.
		 */
		Test.TestEqual(
			FString::Printf(
				TEXT("%s: every entry must say how its brick is held up, %d of %d say nothing %s"),
				Where, SilentEntries, Inspector.Pieces.Num(), *DescribeInspector(Inspector)),
			SilentEntries, 0);

		/*
		 * AND EVERY ROW'S DOT AND EVERY ROW'S WORD ARE ONE FACT. WHICH bucket each state gets is
		 * the tables' job; that the two halves of one row cannot contradict each other is swept
		 * here, over every readout this file builds — the knot, the unsolved wall, the position
		 * fixtures and the ladders included.
		 */
		Test.TestEqual(
			FString::Printf(
				TEXT("%s: every entry's bucket must match its own word, %d of %d do not — %s %s"),
				Where, MismatchedBands, Inspector.Pieces.Num(),
				FirstBandMismatch.IsEmpty() ? TEXT("none") : *FirstBandMismatch,
				*DescribeInspector(Inspector)),
			MismatchedBands, 0);

		/*
		 * AND THE ROW AND THE READOUT UNDER IT MAY NOT DISAGREE. Two inches apart on one panel,
		 * one brick must not read "supported" in the list and "stranded" over its joints — which
		 * is the same argument that already ties InspectedLabel to the marked entry's Label, and
		 * it is exactly the drift two independent derivations of one question produce. Held
		 * against the model's own other field rather than against a string written here, on top
		 * of the tables that pin what that word actually is.
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
			 * AND THE SAME FOR THE BUCKET, WHICH IS THE ASSERTION THE WHOLE COLOURED-DOT SLICE
			 * TURNS ON. The row and the readout are two inches apart on one panel and are drawn
			 * from two different fields; a green dot on the row above an amber one over the joints
			 * is the panel contradicting itself about one brick, and it is the direct analogue of
			 * the InspectedLabel check three rows down.
			 *
			 * HELD AGAINST THE MODEL'S OTHER FIELD RATHER THAN AGAINST A VALUE WRITTEN HERE, on
			 * top of the tables that pin what the bucket actually is — the same shape as the word
			 * check immediately above.
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

			/*
			 * AND THE BUCKET GOES WITH IT, TO THE ONE VALUE THAT CLAIMS NOTHING. An empty word
			 * beside a bucket that still says "grounded" is a dot drawn in the colour of a brick
			 * the readout is no longer about — the stale-field defect in the field a widget reads
			 * without reading any text at all. NotAPiece is the zero enumerator precisely so this
			 * is also what a default-constructed readout answers.
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

			/*
			 * AND IT MUST NOT HEAD THE READOUT WITH A BRICK EITHER. A brick named over an
			 * empty breakout is the stale-field defect in the one field a player reads FIRST
			 * — the name of the brick the numbers under it are about.
			 */
			Test.TestEqual(
				FString::Printf(
					TEXT("%s: nothing inspected must name no brick over the readout, it names '%s'"),
					Where, *Inspector.InspectedLabel),
				Inspector.InspectedLabel, FString());

			/*
			 * AND NO SCALE, FOR THE REASON THERE IS NO SUPPORT WORD. The headroom caption
			 * and its ticks label a BAR; a caption left standing over a breakout that has
			 * gone is the stale-field defect wearing the tidiest possible face.
			 */
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

			/*
			 * AND THE BREAKOUT NAMES THE BRICK IT IS ABOUT, IN THE LIST'S OWN WORDS.
			 *
			 * HELD AGAINST THE MARKED ENTRY'S LABEL RATHER THAN AGAINST A STRING WRITTEN OUT
			 * HERE, and that is not circular: the tables below pin every entry label character
			 * for character against a hand-worked diagram, so this pins the two halves of one
			 * panel to each other on top of that. Two inches apart, one brick may not be
			 * "course 2 · #1" in the list and something else over the joints — which is the same
			 * argument CheckFarEndsReadAsPositions makes for the far ends, one field up.
			 *
			 * The list scrolling is what turns this from tidy into necessary: the entry a
			 * breakout belongs to can be scrolled out of sight while the breakout stays.
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
		 * AND THE READOUT REGION SAYS SOMETHING IN EXACTLY ONE OF THE THREE STATES A PANEL CAN
		 * BE IN, WHICH IS WHY THIS IS ONE ASSERTION RATHER THAN A GUARD PER STATE.
		 *
		 * Bricks picked and none pointed at is the state a fixed panel has to fill: the space
		 * is reserved whether or not there is a breakout to put in it, and a blank region under
		 * a full list reads as a readout that failed rather than as one waiting. A brick pointed
		 * at must NOT carry it — a hint standing over a live breakout is the stale-field defect
		 * this whole struct is shaped against — and neither must an empty selection, where the
		 * player would be told to hover a list with nothing in it and CountText already speaks.
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
		 * AND EVERY NUMBER A HUMAN WILL READ IS FINITE. GetConnectionUtilisation fails closed
		 * to TNumericLimits<double>::Max() for a connection that does not exist, which times
		 * 100 is an infinity, and FMath::Max discards a NaN rather than propagating it — so
		 * "it looked like a number" is exactly how a degenerate answer reaches a screen.
		 */
		/**
		 * How many rows took a colour slot that is not their own row number, and how many took
		 * one after a row that had already run out. Counted rather than asserted per row, for
		 * the reason the silent entries are: one broken rule breaks every row at once.
		 */
		int32 MisplacedSlots = 0;
		int32 SlotsAfterTheEnd = 0;
		bool bSlotsHaveRunOut = false;

		for (int32 Index = 0; Index < Inspector.Joints.Num(); ++Index)
		{
			const FInspectorJointRow& Joint = Inspector.Joints[Index];

			/*
			 * THE SWATCH IS THE ROW'S NUMBER, IN A FIXED ORDER, UNTIL THE PALETTE RUNS OUT AND
			 * THEN IT IS NOTHING AT ALL.
			 *
			 * ROW i TAKES SLOT i is what makes the colours stable while a player scans: the first
			 * joint row is the first colour on every brick they point at. The alternative — a
			 * colour keyed on the brick at the far end — is what a reader assumes is happening,
			 * and it is unimplementable at this scale: a wall is 1,220 bricks and a palette is a
			 * handful of hues, so it must collide, and two rows in one colour is a lie about the
			 * one thing the swatch is for.
			 *
			 * AND PAST THE END IT IS INDEX_NONE, NEVER A WRAP. Wrapping is the tidy-looking answer
			 * and it reintroduces the collision it was chosen to avoid, on the brick with the most
			 * joints — the one being read hardest. No swatch is an absence; a repeated swatch is a
			 * wrong answer. Once out, out: a palette that resumed further down the list would put
			 * the same colour twice on one readout with rows in between.
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
			 * AND SO IS THE MARGIN, WHICH IS THE FIELD MOST LIKELY TO BE SILENTLY ABSENT.
			 * It is a reciprocal, so its degenerate inputs are the two ends of the range
			 * rather than something exotic: an unloaded joint divides by zero and a joint
			 * past its limit divides into less than one. Every state has to have a
			 * sentence, because the widget may not decide which one it is.
			 */
			Test.TestFalse(
				*FString::Printf(
					TEXT("%s: joint row %d must give a margin reading, it is empty %s"),
					Where, Index, *DescribeInspector(Inspector)),
				Joint.MarginText.IsEmpty());

			/*
			 * AND THE BAR'S FILL IS A FRACTION, ALWAYS. A log of a reciprocal is exactly
			 * the shape that produces an infinity for an unloaded joint and a NaN for a
			 * negative one, and both would draw as SOME bar — a Slate progress bar clamps
			 * internally, so a wrong number here looks entirely plausible on screen. This
			 * is the assertion that makes the bar's arithmetic falsifiable at all.
			 */
			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: joint row %d's headroom must be a finite fraction, it is %f"),
					Where, Index, Joint.HeadroomFraction),
				FMath::IsFinite(Joint.HeadroomFraction)
					&& Joint.HeadroomFraction >= 0.0
					&& Joint.HeadroomFraction <= 1.0);

			/*
			 * AND A JOINT THAT HAS GIVEN HAS NO HEADROOM AT ALL, which is the bar's copy
			 * of the rule the whole readout is built on: a given joint carries 0 N at 0 %,
			 * identical to an intact joint with nothing on it — and an intact unloaded
			 * joint has the MOST headroom there is. Drawing a full bar beside a hole in the
			 * wall is the single worst thing this panel could do.
			 */
			if (Joint.bHasGiven)
			{
				Test.TestEqual(
					FString::Printf(
						TEXT("%s: joint row %d has given, so its bar must be empty; it reads %f"),
						Where, Index, Joint.HeadroomFraction),
					Joint.HeadroomFraction, 0.0);

				/*
				 * AND IT IS COLOURED LIKE THE HOLE IT IS. A given joint is 0 N at 0 %, which is
				 * the arithmetic of a joint with nothing on it — the most comfortable state there
				 * is — so a band computed from the number alone paints a hole in the wall the same
				 * colour as a healthy bed joint on a pad. That is bHasGiven's whole reason for
				 * existing, reappearing one field further out.
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
		 * THE SCALE IS DRAWN EXACTLY WHEN THERE IS A BAR TO LABEL — the user's own
		 * instruction, which was "if it is log, then it needs labels". A log bar without
		 * its decades is unreadable by construction: the same visible fill means 1000x on
		 * one panel and 3x on another, and nothing on screen says which. So the ticks are
		 * not decoration, and they are supplied HERE rather than composed by the widget,
		 * for the reason every other string on this struct is.
		 *
		 * The other direction matters too: a brick with no joints draws no bar, so a
		 * caption beside it is a label on nothing.
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
		 * AND THE TICKS ARE A SCALE: strictly ascending fractions inside the bar, each
		 * with something written on it. A tick at a fraction outside [0,1] is off the end
		 * of the bar it labels, and two ticks at one fraction are two numbers claiming the
		 * same place — both draw perfectly and are simply lies about what the fill means.
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
	 * What the ENTRY list calls one handle of this binding.
	 *
	 * ASKED OF THE PRESENTER'S OTHER ENTRY POINT RATHER THAN DERIVED HERE. A second copy of
	 * "which course is this brick in" written in the test file would agree with a production
	 * derivation that had drifted, and this project has already paid twice for a duplicated
	 * derivation. The entry label itself is pinned against hand-written expectations by
	 * DestructionGame.Presenter.PieceMenuPositionLabel, so this is a known-good answer being
	 * read back rather than a circular one — the numbers a joint line must contain are still
	 * spelled out by hand in the tables below.
	 */
	FString InspectorEntryLabelFor(const FStructureBinding& Binding, int32 Handle)
	{
		const TArray<FPieceRef> JustThatBrick = { MakeRef(Binding.StructureId, Handle) };

		const FPieceMenuInspector Named =
			BuildPieceMenuInspector(Binding, JustThatBrick, FPieceRef());

		return Named.Pieces.Num() == 1 ? Named.Pieces[0].Label : FString();
	}

	/**
	 * EVERY JOINT LINE NAMES ITS FAR END THE WAY THE ENTRY LIST NAMES THAT BRICK.
	 *
	 * THE TWO LISTS ARE ONE PANEL, WHICH IS THE WHOLE POINT. A joint row exists so a player
	 * can find the brick on the OTHER side of the joint, and an array subscript is precisely
	 * what cannot help them do that — it is the same defect the entry labels already shed,
	 * sitting two inches below them, where "course 2 · #1" and "brick 12" would appear in one
	 * screenshot naming bricks in the same wall by two incompatible schemes.
	 *
	 * SWEPT RATHER THAN ARGUED, AND SEPARATELY FROM THE PINNED LINES. The tables pin what each
	 * line reads, character for character, from a hand-worked diagram; this says the two panels
	 * cannot drift apart afterwards, including for the far ends no table happens to name.
	 *
	 * Containment rather than equality because the far end is one field of a line that also
	 * carries the connection, the tier and the load — the exact wording is the tables' job.
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
			 * A FAR END WITH NO NAME AT ALL WOULD MAKE THE SWEEP BELOW VACUOUS — an empty
			 * string is contained in every line there is. The entry list is total, so this
			 * can only fire if the far end is not a handle of this binding at all.
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
 * AND EVERY ENTRY'S LABEL IS A POSITION — "course 2 · #1" — RATHER THAN AN ARRAY INDEX.
 *
 * "brick 4:282" is unambiguous to the code and means nothing whatsoever to a person; a player
 * asked what it meant, which IS the answer — it is a structure id and a subscript into a piece
 * array, and there is only ever one structure in the game today, so the "4:" half is noise a
 * hundred per cent of the time. A course and a place along it is the same brick named the way
 * a bricklayer would name it, and it is derivable here and only here: FStructure is
 * position-free on purpose, and FStructureBinding is the layer that has the boxes.
 *
 * THE OLD LABEL SURVIVES AS THE FALLBACK, and that is the point rather than a leftover. The
 * rule it replaced was justified entirely by TOTALITY — two selected bricks must never present
 * as the same string — so the new one has to keep that promise, including for the refs that
 * have no position in this binding at all: one naming another wall, and one missing a half.
 * Those still read "brick 9:1" and "brick 4:-1", a shape no positioned brick can collide with.
 * DestructionGame.Presenter.PieceMenuPositionLabel is where the derivation itself is pinned.
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
			 * THE INSPECTED BRICK WAS DESELECTED. It is still a perfectly live piece, so the
			 * only thing that disqualifies it is that it is no longer in the set — which is
			 * the rule: an anchor outside the set it anchors is a readout of somebody else's
			 * brick, and this is the row that stops "just call InspectPiece on it".
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
			{ TEXT("course 2 · #1"), TEXT("course 2 · #2") },
			{ true, false },
			TEXT("2 bricks selected"),
			INDEX_NONE,
			0,
			TEXT(""),
			TEXT(""),
			/*
			 * AND THE BRICK THAT IS GONE SAYS SO RATHER THAN QUOTING THE LAST SOLVE. FStructure
			 * legitimately keeps a removed piece's support answer until something re-solves —
			 * this one would still read "supported" — and drawing that beside a hole in the wall
			 * is a confident answer about nothing. InspectPiece is what fails closed on it, and
			 * this column is where that has to survive being turned into a word.
			 */
			{ TEXT("supported"), InspectorNoBrickSupportWord }
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
			 * THE CASE THAT DECIDES SIBLING-VERSUS-ROW. The menu for this selection is
			 * empty — asserted above — and the debugger must be full: both bricks listed,
			 * the live one singled out, all three of its joints broken out.
			 */
			TEXT("the menu is empty because one brick is released, and the debugger is not"),
			{ MakeRef(InspectorStructure, SubjectPiece), MakeRef(InspectorStructure, FloaterPiece) },
			MakeRef(InspectorStructure, SubjectPiece),
			{ MakeRef(InspectorStructure, SubjectPiece), MakeRef(InspectorStructure, FloaterPiece) },
			{ TEXT("course 2 · #1"), TEXT("course 4 · #1") },
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
			TEXT("3 joints"),
			/*
			 * AND THE RELEASED BRICK READS "falling" WHILE READING LIVE, which is the pair of
			 * fields saying two different true things about one brick: it is still a piece in the
			 * graph (so the list must not grey it out) and nothing is holding it up (so the list
			 * must say so). This is also the row where the support column earns its place — the
			 * menu is EMPTY here, so the panel's only explanation of why is this word.
			 */
			{ TEXT("supported"), TEXT("falling") }
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
			 * A DUPLICATE IS PRESENTED TWICE. FPieceSelection is a set and cannot produce
			 * this, so the point is what the presenter is: a PROJECTION of a list, not a
			 * second implementation of set semantics. A presenter built on a TSet would
			 * pass every other row here and lose both the order and this.
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
			{ TEXT("course 1 · #2") },
			{ true },
			TEXT("1 brick selected"),
			0,
			2,
			TEXT("stranded"),
			TEXT("2 joints"),
			/*
			 * AND THE ROW SAYS "stranded" TOO, WHICH IS THE WORD THE LIST CANNOT AFFORD TO LOSE.
			 * A stranded brick is one the solver could not route rather than one that is falling,
			 * and in a list of eleven picked bricks it is the only thing that would tell a player
			 * the numbers under the others may be worth doubting.
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
		 * AND THE TWO HALVES OF THE PANEL NAME BRICKS THE SAME WAY. The rows above assert
		 * what the ENTRY labels read; this asserts that the joint lines under them call the
		 * far end the same thing, on every case that breaks any joints out at all — which
		 * includes the knot, whose two head joints no line-by-line table in this file pins.
		 */
		CheckFarEndsReadAsPositions(*this, Binding, Inspector, Case.Description);

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
				 * THE LABEL IS "course <C> · #<N>" FOR A BRICK THIS BINDING CAN PLACE, AND
				 * "brick <StructureId>:<PieceIndex>" FOR ONE IT CANNOT. Both halves are
				 * total, and neither shape can be mistaken for the other.
				 *
				 * A POSITION, BECAUSE AN ARRAY SUBSCRIPT IS NOT A PLACE. "brick 4:282" tells
				 * a reader which slot of which array a brick is in, which is exactly the
				 * information a person standing in front of a wall does not have and cannot
				 * check. The course and the position along it is how the wall was built and
				 * how anybody would point at it.
				 *
				 * THE FALLBACK IS THE OLD RULE, KEPT WORD FOR WORD, because the old rule's
				 * whole justification was that two selected bricks must never present as one
				 * string — and a ref naming another wall, or one missing a half, has no
				 * position here to be named by. Qualifying those by structure is what tells
				 * "{4,1}" and "{9,1}" apart; the row above builds exactly that pair.
				 *
				 * Note WHICH bricks get a position: piece 3 was REMOVED and still reads
				 * "course 2 · #2", because FPieceBinding keeps the box of a piece that has
				 * gone deliberately — it is a record of where the brick WAS, and where it was
				 * is the single most useful thing to say about a hole in a wall.
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

				/*
				 * AND WHY THAT BRICK IS OR IS NOT STANDING UP, ON ITS OWN ROW.
				 *
				 * THE PANEL SHOWS ELEVEN IDENTICAL STRINGS WITHOUT IT. Every fact the list
				 * currently carries — the position, the liveness — is about IDENTITY, so a
				 * selection of eleven bricks says nothing whatsoever about what the wall is
				 * doing until the player hovers each row in turn and reads the breakout. The
				 * model has the answer for all of them already: BuildPieceMenuInspector calls
				 * InspectPiece once per entry to decide bIsLivePiece, and the support state
				 * comes back on the very same struct.
				 *
				 * READ BACK, NOT RE-DERIVED, WHICH IS WHY THE WORDS ARE THE READOUT'S OWN.
				 * "grounded", "supported", "stranded", "falling" and "not solved yet" are what
				 * PresenterWordForSupport already says for the singled-out brick, and a second
				 * vocabulary for the same five states would be two panels in one.
				 *
				 * EXPECTED VALUES COME FROM THE FIXTURE DIAGRAM, per row, never from the graph.
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

		/*
		 * AND A BRICK'S NEIGHBOURS ARE NAMED BY WHERE THEY ARE WHETHER OR NOT ANYBODY HAS
		 * SOLVED. A position is a fact about the BOXES, so it must not go quiet with the
		 * support word the way the loads legitimately do.
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

		/*
		 * AND THE ENTRY'S OWN COLUMN SAYS THE SAME THING THE READOUT DOES, WHICH IS THE STATE
		 * THE COLUMN IS MOST LIKELY TO GET WRONG. "not solved yet" is the sentence that exists
		 * because EPieceSupport::Falling is both a real collapse and an absent answer, and a
		 * per-row column that took the enumerator at face value would draw a freshly built wall
		 * as a list of falling bricks — which is the same catastrophe-that-has-not-happened one
		 * field out, and far louder in a list of forty rows than in one readout.
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

		/*
		 * AND THE MOMENT, ON THE SAME TERMS AND THROUGH THE SAME CONSTANT. A moment is uu.cm
		 * and length is already centimetres, so this is the identical unit change ForceN
		 * makes rather than a second boundary — MOMENTS_DESIGN.md says so loudly because
		 * "moments" sounds like it should introduce one. The magnitude, for the reason ForceN
		 * takes a magnitude: which way a joint is being levered open is not a thing a line of
		 * text says, and the worst corner is the worst corner either way.
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
	 *
	 * AND EACH INTACT LINE NOW CARRIES ITS MARGIN, WHICH IS THE HALF A PLAYER CAN READ.
	 * "0.049 %" is only meaningful to somebody who already knows that 100 % is failure and
	 * that masonry sits four orders of magnitude below it; "2041× margin" says the same
	 * number as a sentence — this joint could take two thousand times what it is carrying.
	 * 1 / 0.00049 is 2040.8, and the format is an integer at or above 100× because a tenth
	 * of a multiple that large is noise. The rest of the states are pinned on their own
	 * fixture in DestructionGame.Presenter.PieceMenuJointHeadroom.
	 *
	 * A GIVEN JOINT'S LINE IS UNCHANGED and carries no margin at all, because it is not
	 * carrying anything and never will again. Its margin READING is still asserted — as a
	 * field, in the headroom test — precisely so that it cannot quietly become the same
	 * sentence as an intact joint with nothing on it.
	 *
	 * AND THE FAR END IS NAMED BY WHERE IT IS, EXACTLY AS THE ENTRY LIST NAMES IT. "brick 0"
	 * was an array subscript in a panel whose other half already reads "course 2 · #1", and a
	 * joint row is the one place a subscript is least defensible: the row exists so a player
	 * can find the brick on the OTHER side of the joint, which is precisely what an index into
	 * FStructure's piece array cannot help anybody do. The three far ends here are the pad
	 * (course 1 · #1), the rider (course 3 · #1) and the spare (course 2 · #2), read off the
	 * fixture diagram above rather than out of the code.
	 *
	 * AND THE SEVERED JOINT'S FAR END IS STILL A POSITION, WHICH IS NOT AN ACCIDENT. The spare
	 * was REMOVED, and FPieceBinding keeps the box of a piece that has gone deliberately — a
	 * hole in the wall is somewhere, and where it was is the single most useful thing to say
	 * about it. A far end that fell back to a subscript the moment its brick was pulled would
	 * lose the name at exactly the moment a player is asking what just happened.
	 *
	 * WHAT IS *NOT* CHANGED IS THE "#<n>" PREFIX. That is the CONNECTION index, and it is not
	 * the same defect: a joint has no position of its own — no course, no place along one — so
	 * there is nothing else to call it, and it is the handle anybody would use to take a
	 * failure back to the graph (GetConnectionForce(11)). Naming a BRICK by subscript is
	 * useless to a player because the brick is a thing they can see; naming a joint by its
	 * connection index is the only name a joint has.
	 */
	const TArray<FString> ExpectedLines = {
		TEXT("#0  course 1 · #1  bed below  49.0 N  0.049 %  2041× margin"),
		TEXT("#1  course 3 · #1  bed above  29.4 N  0.029 %  3401× margin"),
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

/**
 * NAMED, AND NAMED DIFFERENTLY AGAIN — see the note on PieceInspectorTestSupport. A unity
 * build merges translation units, so two file-local names that collide are a hard compile
 * error between files that never refer to each other.
 */
namespace PiecePositionTestSupport
{
	using namespace PieceInspectorTestSupport;

	/** A structure id distinct from the worked fixture's, so a fallback label reads apart. */
	constexpr int32 PositionStructure = 12;

	/**
	 * HOW FAR APART TWO BRICKS' CENTRES MAY SIT AND STILL BE ONE COURSE — SPELLED OUT HERE
	 * RATHER THAN IMPORTED, so a production constant that moves fails this file instead of
	 * agreeing with it.
	 *
	 * HALF A CENTIMETRE, AND THE ARGUMENT IS THE GRID. A course rises by the brick's height
	 * plus one mortar joint — 7.5 cm for the standard 21.5 x 10.25 x 6.5 unit this game
	 * lays, and DestructionLayout::RunningBond computes exactly that. So 0.5 cm is one
	 * FIFTEENTH of the smallest real gap between two courses, which leaves an enormous
	 * amount of room to be wrong in without ever merging two courses into one, while still
	 * absorbing float noise and a brick modelled a couple of millimetres off nominal.
	 *
	 * THE FAILURE DIRECTIONS ARE NOT SYMMETRIC, WHICH IS WHY IT IS NOT SIMPLY EQUALITY.
	 * Too tight and one wall reads as eighty courses of one brick each — useless, but
	 * obviously useless. Too loose and two real courses merge, and then two DIFFERENT
	 * bricks compete for one position number: the readout still looks perfectly ordinary
	 * and is silently naming the wrong brick, which is the whole failure the label rule
	 * exists to prevent. So the tolerance is set far closer to the tight end than the
	 * middle.
	 *
	 * KNOWN SCALE ASSUMPTION, RECORDED RATHER THAN SOLVED: this is an absolute distance, so
	 * a structure built of pieces under about a centimetre tall would band its courses
	 * together. Nothing in the game builds one. The adaptive alternative — a fraction of
	 * each piece's own height — is not obviously better, because a mixed-size structure
	 * makes "same course as" non-transitive and the banding then depends on visiting order.
	 */
	constexpr double PositionCourseToleranceCm = 0.5;

	/**
	 * A real IEEE NaN and a real +infinity, produced the way LayoutTest.cpp produces
	 * theirs — through a volatile so no constant folding turns them into anything else.
	 *
	 * They must be the genuine articles rather than merely enormous numbers, because the
	 * two are caught by different guards: an IsFinite check rejects both of these and
	 * lets TNumericLimits<double>::Max() straight through.
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
 * A BRICK IS NAMED BY WHERE IT IS — "course 2 · #1" — AND TWO DIFFERENT BRICKS ARE NEVER
 * NAMED THE SAME THING.
 *
 * THE DERIVATION, STATED ONCE. Sort every piece the binding holds a usable box for by the
 * Z of its centre. Band them: a piece joins the course whose LOWEST member it is within
 * half a centimetre of, otherwise it starts a new one. Number the courses from the bottom
 * starting at ONE, because a person counting courses of brick starts at one and always has.
 * Within a course, order by X, then by Y, then by piece handle, and number from one again.
 * A piece the binding cannot place keeps the old "brick <StructureId>:<PieceIndex>".
 *
 * WHY BAND AGAINST THE COURSE'S FLOOR RATHER THAN THE PREVIOUS PIECE. Comparing each piece
 * to the one before it is the obvious loop and it is wrong in a way nothing would notice:
 * a run of pieces each 0.4 cm above the last CHAINS, so forty of them merge into a single
 * "course" spanning sixteen centimetres — two real courses of a wall, presented as one, with
 * their position numbers interleaved. Anchoring to the band's own floor bounds a course's
 * total spread at the tolerance, so the property holds however many pieces arrive. The row
 * "near misses must not chain into one course" below is the one that separates the two, and
 * it is the only row in this file that does.
 *
 * WHY (X, Y, HANDLE) AND NOT JUST X. The label replaced one whose entire justification was
 * that two selected bricks must never present as the same string, so the replacement has to
 * keep that promise TOTALLY rather than usually. X alone does not: a wall two leaves thick
 * puts two bricks of one course at the same X, differing only in depth — an ordinary piece
 * of masonry, not a pathological input. Y settles that. The handle then settles the genuinely
 * degenerate case of two pieces at exactly the same point, which is not something a wall
 * produces but IS something a caller can hand in, and "unambiguous" has to mean unambiguous.
 * The result is an ordinal in a TOTAL order over one course's pieces, so no two members of a
 * course can share one — and two pieces in different courses differ in the course number. The
 * property therefore holds by construction, and the sweep at the end of this test says so
 * over every case rather than trusting the argument.
 *
 * WHY REMOVED PIECES ARE STILL PLACED. FPieceBinding keeps the box of a piece that has gone,
 * deliberately, and this is what that is for: a hole in a wall is worth naming, and — more
 * importantly — a course that renumbered itself when a brick was pulled out of it would
 * change the name of every brick to its right at the exact moment a player is looking at
 * them. The label of a brick must not depend on what has been done to its neighbours.
 *
 * DEGENERATE POSITIONS FAIL CLOSED TO THE OLD LABEL. A NaN or infinite centre cannot be
 * banded — every comparison against NaN is false, so it would form a course of its own whose
 * NUMBER depends on where the sort happened to put it, which is both meaningless and
 * unstable. It is excluded from the banding entirely, so it neither gets a position nor
 * disturbs anybody else's.
 *
 * NEEDS A TICKING WORLD: no, and not even a world. This is arithmetic over a plain struct.
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
			 * THE ORDINARY CASE, AND THE ONE THAT SEPARATES POSITION ORDER FROM HANDLE
			 * ORDER. The boxes are added deliberately scrambled — handle 0 is in the upper
			 * course at the right — so a derivation that numbered along a course by handle
			 * would agree with a bottom-up left-to-right fixture forever and disagree here.
			 * A wall IS built bottom-up and left-to-right, which is exactly what would let
			 * that confusion survive to a player's screen.
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
			 * A COURSE IS A BAND, NOT A PLANE. Real bed joints are not identical thicknesses
			 * and a modelled brick need not be laid to the micrometre, so bricks a few
			 * millimetres apart in Z are one course. A bit-exact rule would present a wall
			 * as one course per brick the first time anything jittered a centre.
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
			 * AND THE BAND HAS AN EDGE. Without one every brick in a forty-course wall is
			 * in course 1, which is the failure that also destroys the uniqueness promise:
			 * forty bricks would then be competing for one set of position numbers.
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
			 * THE ROW THAT PINS *WHICH* PIECE THE TOLERANCE IS MEASURED FROM, and the only
			 * one in the file that does.
			 *
			 * Four pieces, each 0.4 cm above the last — every consecutive gap is inside the
			 * tolerance, and the total span is 1.2 cm, well over twice it. Measured against
			 * the course's own floor they are two courses of two. Measured against the
			 * PREVIOUS piece they chain into one course of four, which passes every other
			 * row in this file, renumbers half the wall and looks completely reasonable
			 * doing it. The two implementations differ on exactly this shape.
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
			 * TWO LEAVES: an entirely ordinary wall, and the one that breaks an X-only
			 * ordering. Both bricks are in course 1 at X 0 and differ only in depth, so an
			 * ordering that stopped at X would hand them the same position number — two
			 * different bricks presenting as one string, which is precisely what the label
			 * rule exists to prevent.
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
			 * AND THE DEGENERATE ONE: two pieces at exactly the same point. No wall
			 * produces this, but a caller can hand it in, and "unambiguous" that stops
			 * being true for inputs nobody expected is the same as not being true. The
			 * piece handle is the last resort and it is unique by construction.
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
			 * A POSITION NOBODY CAN COMPUTE FALLS BACK, AND DISTURBS NOTHING.
			 *
			 * The two good bricks must still read #1 and #2 — so the unplaceable ones take
			 * no position number at all rather than being sorted somewhere and consuming
			 * one. An implementation that let a NaN through would band it into a course of
			 * its own, whose NUMBER then depends on where the sort put a value that
			 * compares false against everything: unstable as well as meaningless.
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
			 * AND PULLING A BRICK OUT RENAMES NOTHING — not the brick, and not its
			 * neighbours.
			 *
			 * The middle brick is removed and everything reads exactly as it did. An
			 * implementation that skipped removed pieces would slide the third brick from
			 * #3 to #2, so the brick a player is looking at would change its name because
			 * something happened to a DIFFERENT brick. That is the one thing a label may
			 * never do, and it is invisible in every other row here.
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
		/* TABLE INTEGRITY: a short expectation list would silently skip the tail. */
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
		 * AND THE PROMISE ITSELF, SWEPT OVER EVERY ROW RATHER THAN ARGUED ONCE. This is the
		 * property the whole label rule exists for, and it is the one an example cannot
		 * establish: every case above is checked for a collision, including the ones where
		 * a collision is not the thing being demonstrated.
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

/** Named again, and named apart again — see the note on PieceInspectorTestSupport. */
namespace PieceHeadroomTestSupport
{
	using namespace PieceInspectorTestSupport;

	constexpr int32 HeadroomStructure = 21;

	/**
	 * THE LADDER'S ONE JOINT SIZE, CHOSEN SO THE ARITHMETIC IS DOABLE IN THE HEAD.
	 *
	 * General purpose mortar takes 10 MPa in compression, so 49 cm2 of it holds
	 * 10 N/mm2 x 4900 mm2 = 49,000 N, which is 4,900,000 Unreal force units. A brick of
	 * M kilograms weighs M x 980 uu. So a brick of M kg resting squarely on 49 cm2 of this
	 * mortar loads the joint to exactly
	 *
	 *     980 M / 4,900,000  =  M / 5000
	 *
	 * of its capacity, and its MARGIN is 5000 / M. Five kilograms is a thousandth,
	 * fifty is a hundredth, five hundred is a tenth, and five thousand is exactly the
	 * limit — four decades from four masses, with no rounding anywhere in the chain. Every
	 * expected number below is read off that one line.
	 *
	 * WHICH AXIS GOVERNS IS NOT LEFT TO CHANCE, for the reason CompressionOnlyUtilisation
	 * states: every load here is exactly antiparallel to an exactly vertical normal, so
	 * shear and tension are exactly zero and their ratios are exactly zero whatever their
	 * capacities are. COMPRESSION is necessarily the worst of the three, so ComputeUtilisation's
	 * worst-axis answer is the compression figure and not whichever axis happened to win.
	 */
	constexpr double LadderJointAreaSqCm = 49.0;
	constexpr double LadderMassPerFullLoadKg = 5000.0;

	/** M / 5000, worked above. Hand-derived, never read back off the graph. */
	double LadderUtilisation(double MassKg)
	{
		return MassKg / LadderMassPerFullLoadKg;
	}

	/*
	 * THE LOAD LADDER: ONE GROUNDED PAD WITH A ROW OF BRICKS SAT ON IT, EACH ON ITS OWN
	 * BED JOINT, EACH A DIFFERENT WEIGHT.
	 *
	 * IT IS A LADDER RATHER THAN A COLUMN ON PURPOSE. A column's joints carry the
	 * ACCUMULATED weight above them, so the loads are tied together and cannot be placed
	 * where a readout needs them; bricks side by side on one pad each send their whole
	 * weight down their own joint and nothing else, so each rung is an independent dial.
	 * The pad is grounded, which TERMINATES the flow — a grounded piece pushes nothing on —
	 * so the pad's own weight never appears on any of them.
	 *
	 * INSPECTING THE PAD THEREFORE BREAKS OUT THE WHOLE LADDER IN ONE READOUT, in ascending
	 * connection order, which is what makes this one table rather than twelve fixtures.
	 *
	 * IT IS NOT A WALL AND DOES NOT PRETEND TO BE. The geometry is synthetic — the joints
	 * are hand-written with explicit normals and areas exactly as the worked fixture's are,
	 * and no box here decides anything. What is real is the load path and the strengths.
	 */
	constexpr int32 LadderPadPiece = 0;

	/** Handle 11: a SECOND grounded pad, so the head joint between them carries nothing. */
	constexpr int32 LadderNoLoadPiece = 11;

	/** Handle 12: pulled out before the solve, so its joint is severed without ever failing. */
	constexpr int32 LadderRemovedPiece = 12;

	/**
	 * Handle 13: A REAL PIECE, CARRYING A REAL LOAD, THAT NO BINDING CAN PLACE.
	 *
	 * Its centre is a NaN, so it takes no course and no position along one — the same
	 * exclusion the entry labels already make, for the same reason: every comparison against
	 * a NaN is false, so banding it would put it in a course of its own whose NUMBER depends
	 * on where the sort happened to leave a value that orders against nothing.
	 *
	 * IT IS NOT A DEGENERATE PIECE ANYWHERE ELSE. FStructure is position-free on purpose, so
	 * the box reaches the solver not at all: this brick has a mass, a joint, a tier and a
	 * perfectly ordinary load, and the ONLY thing wrong with it is that nobody can say where
	 * it is. That is what makes it a clean test of the fallback rather than of the arithmetic
	 * — it carries exactly what rung 0 carries, so the two rows differ in one field.
	 *
	 * WHY IT IS WORTH A ROW AT ALL. The entry label's fallback exists because a REF can name
	 * another wall or be missing a half; a far end is a bare HANDLE in the inspected brick's
	 * own structure, so neither of those can happen to it and an unplaceable box is the only
	 * way the fallback is reachable here. If it were not reachable, the fallback would be
	 * untestable code — and a fallback nothing exercises is a fallback nobody has checked
	 * cannot collide with a position.
	 */
	constexpr int32 LadderUnplaceablePiece = 13;

	/** The same 5 kg as rung 0, so the two rows differ ONLY in how the far end is named. */
	constexpr double LadderUnplaceableMassKg = 5.0;

	/**
	 * HANDLE 14: THE RUNG WHOSE LOAD DOES NOT COME DOWN THE MIDDLE OF ITS JOINT.
	 *
	 * EVERY OTHER RUNG IS GEOMETRY-FREE AND THEREFORE UNBENT, which is exactly why this one
	 * has to exist. A line that printed the force and the percentage and nothing else agrees
	 * with all twelve of them forever, and it is precisely on a bent joint that the two stop
	 * explaining each other: this rung carries 548.8 N — barely more than rung 2's 490 N, a
	 * ninth of rung 3's — and sits at 49 % of capacity where rung 2 sits at 1 %. Nothing a
	 * reader can do with the two numbers printed beside each other gets from one to the
	 * other.
	 *
	 * THE ARITHMETIC, DERIVED HERE RATHER THAN READ BACK. (The mass moved 8 -> 56 kg at the
	 * 2026-08-14 mean re-anchor flip, exactly x7 with f_x1 0.10 -> 0.70, so every ratio below —
	 * the 49 %, the 2.0x margin, the Caution band, the bar fraction — is preserved
	 * bit-identically and the row keeps measuring what it always measured.)
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
	 * WHICH AXIS GOVERNS IS NOT LEFT TO CHANCE, and here it is a DIFFERENT axis from every
	 * other rung on the ladder. Peak compression is the SUM of the two stresses, 0.5716667
	 * against 10 MPa — 0.057 of capacity — and shear is exactly zero because the load is
	 * exactly antiparallel to an exactly vertical normal. TENSION governs, by 8.6 times.
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
	 * Every rung's mass, in handle order from handle 1. The four decades come first.
	 *
	 * Handles 9 and 10 are the KILONEWTON BOUNDARY PAIR and are the only two masses here
	 * that are not round: 100000/980 kg weighs exactly 100,000 uu, which is exactly
	 * 1000.0 N, and 99990/980 kg weighs exactly 999.9 N. They exist to pin which side of
	 * one thousand newtons switches unit, which is the one decision in the formatting that
	 * a hand-picked example either side of it would leave open.
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
	 * The one rung that knows the shape of its own face, and therefore the one that bends.
	 *
	 * A SEPARATE HELPER RATHER THAN AN ARGUMENT ON THE ONE ABOVE, because a rectangle and an
	 * area have to agree — AddConnection refuses them otherwise — and the twelve joints above
	 * are 49 cm2 with no rectangle at all. Giving them one would put a lever arm on every rung
	 * of a ladder whose entire point is that each dial moves independently.
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

		/*
		 * And the one nobody can place. Its X is ordinary and its Z is a NaN — the centre as
		 * a whole is unusable, which is the state the position table excludes, and one
		 * unusable component is enough to reach it.
		 */
		{
			DestructionLayout::FPieceBox Nowhere =
				InspectorBoxAt(InspectorBrickPitchCm * 12.0, InspectorCourseZ(1));

			Nowhere.CentreCm.Z = PiecePositionTestSupport::PositionNaN();

			Out.AddPiece(LadderUnplaceableMassKg, false, nullptr, Nowhere);
		}

		/*
		 * AND THE RUNG THAT LEANS. Its box centre is what the binding hands the solver as a
		 * centre of mass, so the four centimetres between it and the joint's centroid below
		 * are the whole lever arm — there is nothing else in the fixture that could produce
		 * one, and no other rung has a centroid to measure against at all.
		 *
		 * Placed a full pitch past the pulled brick so its POSITION is the twelfth of course
		 * two: the unplaceable rung between them takes no position, which is what makes
		 * "course 2 · #12" a real count of placeable bricks rather than a handle in a hat.
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

		/* Appended LAST, so every connection index the table below names stays where it was. */
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
		 * WHICH BAND THE BAR IS DRAWN IN — comfortable above 10x margin, cautious below it,
		 * critical at or below 2x and for a joint that has gone.
		 *
		 * A COLUMN HERE RATHER THAN A TABLE OF ITS OWN, because the band is a transform of the
		 * utilisation two columns to the left and this ladder is where those numbers already
		 * live. What this ladder CANNOT say is where the amber/red edge is — it holds nothing
		 * between 1x and 10x — so the two edge rows either side of 2x are a fixture of their own
		 * in Presenter.PieceMenuJointMarginBand.
		 */
		EJointMarginBand ExpectedBand = EJointMarginBand::Critical;

		/**
		 * WHAT THE JOINT IS BEING BENT BY, IN NEWTON-CENTIMETRES — AND ZERO ON TWELVE OF THE
		 * THIRTEEN ROWS, WHICH IS THE HALF OF THIS COLUMN THAT ACTUALLY BITES.
		 *
		 * A settled wall bends nowhere: a brick on two symmetric bed patches has its centre of
		 * mass at the area-weighted centroid of its supports, so the eccentricity is zero
		 * EXACTLY rather than nearly. Most joints a player ever looks at therefore have nothing
		 * to say here, and a line that carried a bending clause anyway would make the common
		 * case worse to read for the sake of the rare one. So the twelve zeroes below are an
		 * assertion that the sentence does NOT grow, held by their ExpectedLine being the line
		 * this panel already printed, word for word.
		 */
		double ExpectedMomentNCm = 0.0;
	};
}

/**
 * A JOINT SAYS HOW MANY TIMES ITS LOAD IT COULD TAKE, IN NEWTONS OR KILONEWTONS, AND HANDS
 * OVER A LOG-SCALED BAR FRACTION AND THE SCALE THAT MAKES IT READABLE.
 *
 * WHY MARGIN AT ALL, WHEN THE PERCENTAGE IS ALREADY THERE. "0.470 %" is only meaningful to
 * a reader who already knows two things: that 100 % is failure, and that masonry in
 * compression sits three or four orders of magnitude under it. Nothing on the panel says
 * either. "213× margin" says the whole thing in one phrase — this joint could take two
 * hundred times what it is carrying — and it is the reciprocal of a number already on the
 * row, so nothing is recomputed and the exact-equality sweep on ForceN and UtilisationPercent
 * in PieceMenuJointReadout keeps holding unchanged.
 *
 * THE THREE READINGS THAT ARE NOT A NUMBER ARE THE POINT OF THE TABLE, because each of them
 * is a state where the obvious formula produces something plausible and wrong:
 *
 *   - AN UNLOADED JOINT divides by zero. Infinite margin is true and useless, and printed it
 *     is either "inf× margin" or, once something clamps it, a large fabricated number. It
 *     reads "no load".
 *   - A JOINT AT OR PAST ITS LIMIT divides into something no bigger than one, so the formula
 *     goes on producing a perfectly well-formed answer: a joint at twice its capacity reads
 *     "0.5× margin", which contains the word MARGIN beside a joint that has none. That is the
 *     fail-open direction and it is the single most misleading line this panel could print,
 *     so it reads "no margin left" — which is true at exactly 1.0, where the break rule says
 *     the joint is fully loaded but still holding, and true above it as well.
 *   - A JOINT THAT HAS GIVEN carries nothing, so the formula puts it in the FIRST of those
 *     states: an unloaded joint and a hole in the wall reading identically, which is the
 *     exact defect FJointInspection::bHasGiven exists to prevent, reappearing one layer out
 *     in a new field. It reads "gone".
 *
 * THE BAR IS LOG-SCALED OVER THREE DECADES BECAUSE A LINEAR ONE IS EMPTY FOREVER. A settled
 * brick wall sits near 0.0005 of capacity — CURRENT_STATE.md records the worst joint of a
 * 30 x 40 wall at 0.00495 — so a bar drawn on utilisation directly is a flat zero at every
 * joint of every structure the game currently builds, which is a bar that conveys nothing at
 * all. Full is 1000× margin, empty is the joint giving, and everything between is a decade of
 * the log. The consequence is that MOST joints peg the bar full, and that is honest: they
 * genuinely are three orders of magnitude from failing.
 *
 * SO IT NEEDS LABELS, AND THE MODEL SUPPLIES THEM. A log axis with no ticks is unreadable by
 * construction — the same fill means 1000× on one panel and 3× on another and nothing says
 * which — and composing the ticks in the widget would put four strings and four numbers in
 * the one place no test can reach. They are asserted here BOTH as text and as positions, and
 * cross-checked against the curve: the joint whose margin is exactly 10× must fill the bar to
 * exactly where the "10×" tick is drawn. A caption promising a scale the arithmetic does not
 * follow is a plausible-looking picture over a wrong number, which is a thing this project has
 * already paid for once.
 *
 * AND EVERY FAR END IS NAMED BY WHERE IT IS. The ladder is the fixture that can say what the
 * worked one cannot: twelve far ends across two courses, one of them a brick that has been
 * pulled out and one of them a brick whose box says nowhere — which is the ONLY way a joint
 * row reaches the ref-shaped fallback, because a far end is a bare handle in the inspected
 * brick's own structure and can therefore be neither foreign nor half-missing.
 *
 * AND THE FORCE PICKS ITS UNIT. 91200.0 N is a number a reader has to count the digits of;
 * 91.2 kN is not. The switch is at a thousand newtons, and the pair of rungs at exactly
 * 1000.0 N and exactly 999.9 N is what pins which side of it changes unit. There is no new
 * conversion boundary here: newtons are already DestructionPresenter::ForceUnitsPerNewton's
 * job, and a kilonewton is a thousand newtons by definition of the prefix.
 *
 * AND A JOINT BEING LEVERED OPEN SAYS SO, WHILE THE TWELVE THAT ARE NOT SAY NOTHING EXTRA.
 * The last rung carries 548.8 N and sits at 49 % of capacity — barely more than rung 2's load
 * at fifty times its utilisation — and no arithmetic a reader can do on the two numbers printed
 * beside each other closes that gap, because the term joining them is a 2,195.2 N·cm bend the
 * line never mentions. That is the defect MOMENTS_DESIGN.md names as part of the moment work
 * rather than as a follow-up. The other twelve rows are the other half of the claim: a
 * settled wall bends nowhere — a brick on two symmetric patches has its centre of mass at the
 * area-weighted centroid of its supports, so its eccentricity is zero EXACTLY — and a line
 * that carried a bending clause anyway would make the common case worse to read for the sake
 * of the rare one. Their pinned sentences are the ones this panel already printed.
 *
 * NEEDS A TICKING WORLD: no, and not even a world.
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
	 * FIXTURE PRECONDITION, AND THIS ONE IS A DOOR RATHER THAN A NUMBER. AddConnection refuses
	 * a rectangle that disagrees with its area and one on a normal that names no separation
	 * axis, answering INDEX_NONE — at which point the ladder is one rung short, every index
	 * the table names past it is wrong, and the failures say nothing about the readout.
	 */
	TestEqual(
		FString::Printf(
			TEXT("fixture: the eccentric rung's joint must be accepted and land last; AddConnection returned %d"),
			EccentricJoint),
		EccentricJoint, 13);

	/*
	 * FIXTURE PRECONDITIONS, HAND-DERIVED AND ASKED OF THE GRAPH. Every reading below is
	 * worthless if the ladder underneath it is not carrying what this file thinks. These
	 * drive nothing and are green on arrival; they exist so that a ladder which stopped
	 * being a ladder says so, rather than silently redefining what is being presented.
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
	 * AND THE UNPLACEABLE BRICK'S JOINT IS AN ENTIRELY ORDINARY ONE. FStructure never sees a
	 * box, so a NaN centre reaches the solve not at all — this precondition is what says so,
	 * and it is what makes the row below a test of the LABEL rather than of the arithmetic.
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
	 * AND THE BENT RUNG, ASKED OF THE GRAPH DIRECTLY AND DERIVED IN THIS FILE. These are green
	 * on arrival — GetConnectionMoment already exists and already answers — and they drive
	 * nothing. What they buy is that the presented row below is being held against a joint that
	 * genuinely bends: a ladder whose eccentricity quietly went to zero would otherwise agree
	 * with a presenter that never fetched a moment at all, and the whole row would pass.
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
		 * AND IT IS TENSION THAT GOVERNS RATHER THAN COMPRESSION, WHICH IS NOT FREE AND IS THE
		 * ONE THING THAT WOULD MAKE THIS ROW MEASURE SOMETHING ELSE. ComputeUtilisation returns
		 * the WORST of three axes: peak compression here is the SUM of the two stresses over
		 * mortar's 10 MPa, and peak tension is their DIFFERENCE over its 0.1 MPa. Shear is
		 * exactly zero — the load is exactly antiparallel to an exactly vertical normal — so
		 * this comparison is the whole of the remaining question, and a retuned profile that
		 * flipped it would leave every number above unchanged while the row stopped being about
		 * a joint being peeled open.
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
	 * THE LADDER, RUNG BY RUNG. Force and per cent are restated here rather than taken on
	 * trust — PieceMenuJointReadout is what holds them against InspectPiece with exact
	 * equality, and this table is what says which NUMBERS those are, so the two together
	 * fail differently if the passthrough breaks than if the arithmetic does.
	 *
	 * AND EVERY FAR END IS NAMED BY WHERE IT IS, read off the ladder's own boxes rather than
	 * out of the code. The bottom course holds the two grounded pads — handle 11 at X -22.5
	 * is course 1 · #1 and the inspected pad at X 0 is course 1 · #2 — and the course above
	 * holds the ten rungs left to right as #1 to #10, with the pulled brick past the end of
	 * them at #11.
	 *
	 * NOTE WHICH ROWS WOULD SURVIVE A LAZY DERIVATION. Nine of the twelve far ends are
	 * "course 2 · #<handle>", so a presenter that printed the handle in a position's clothes
	 * would pass those and fail exactly three: the head joint to the second pad, which is in
	 * the OTHER course; the severed joint to handle 12, which is #11 of its course rather
	 * than #12; and the unplaceable brick, which has no position at all.
	 */
	const TArray<FHeadroomCase> Cases = {
		{
			TEXT("a thousandth of capacity: the top decade, and the bar is full"),
			0, 49.0, 0.1, TEXT("1000× margin"), 1.0,
			TEXT("#0  course 2 · #1  bed above  49.0 N  0.100 %  1000× margin"),
			EJointMarginBand::Comfortable
		},
		{
			/*
			 * A HUNDREDTH — and the row that pins the FORMAT boundary from above. 100× is
			 * an integer; the 98.04× rung below it keeps a decimal. A rule with no boundary
			 * row is a rule nothing checks.
			 */
			TEXT("a hundredth of capacity: two decades of bar, and a whole-number margin"),
			1, 490.0, 1.0, TEXT("100× margin"), 2.0 / 3.0,
			TEXT("#1  course 2 · #2  bed above  490.0 N  1.000 %  100× margin"),
			EJointMarginBand::Comfortable
		},
		{
			TEXT("a tenth of capacity: one decade of bar, and a margin worth a decimal"),
			2, 4900.0, 10.0, TEXT("10.0× margin"), 1.0 / 3.0,
			TEXT("#2  course 2 · #3  bed above  4.9 kN  10.000 %  10.0× margin"),
			EJointMarginBand::Caution
		},
		{
			/*
			 * EXACTLY AT THE LIMIT. FConnection's break rule holds that 1.0 is fully loaded
			 * but still holding, so this joint is intact, is carrying 49 kN, and has
			 * precisely nothing spare. The naive reading is "1.0× margin", which is
			 * arithmetically true and reads like a joint with room in it.
			 */
			TEXT("exactly at the limit: the bar is empty and there is no margin to quote"),
			3, 49000.0, 100.0, TEXT("no margin left"), 0.0,
			TEXT("#3  course 2 · #4  bed above  49.0 kN  100.000 %  no margin left"),
			EJointMarginBand::Critical
		},
		{
			/*
			 * OFF THE TOP OF THE BAR. Ten thousand times is four decades and the bar has
			 * three, so the fraction clamps at full — but the MARGIN is still quoted in
			 * full, because clamping a bar is a drawing decision and rounding the number
			 * beside it would be losing information the reader asked for.
			 */
			TEXT("four decades of margin: the bar pegs full and the number does not"),
			4, 4.9, 0.01, TEXT("10000× margin"), 1.0,
			TEXT("#4  course 2 · #5  bed above  4.9 N  0.010 %  10000× margin"),
			EJointMarginBand::Comfortable
		},
		{
			/*
			 * JUST UNDER THE FORMAT BOUNDARY: 5000/51 is 98.0392..., which keeps its
			 * decimal where the 100× rung above loses it. Pairing the two is the only way
			 * the "at or above 100×" half of the rule is falsifiable.
			 */
			TEXT("just under a hundred times: still a decimal"),
			5, 499.8, 1.02, TEXT("98.0× margin"), 0.66379994274602749,
			TEXT("#5  course 2 · #6  bed above  499.8 N  1.020 %  98.0× margin"),
			EJointMarginBand::Comfortable
		},
		{
			/*
			 * TWICE THE LIMIT, WHICH IS THE FAIL-OPEN ROW. The reciprocal is 0.5, so the
			 * naive line reads "0.5× margin" — a number, next to the word margin, beside a
			 * joint that is at two hundred per cent and gone the moment anything asks it to
			 * break. A joint past its capacity and a joint at exactly its capacity are the
			 * same sentence deliberately: both have nothing left, and inventing a second
			 * wording would be a branch whose only purpose is decoration.
			 */
			TEXT("past the limit: still no margin, never a fraction of one"),
			6, 98000.0, 200.0, TEXT("no margin left"), 0.0,
			TEXT("#6  course 2 · #7  bed above  98.0 kN  200.000 %  no margin left"),
			EJointMarginBand::Critical
		},
		{
			TEXT("twelve and a half times, between two decades"),
			7, 3920.0, 8.0, TEXT("12.5× margin"), 0.36563667100268549,
			TEXT("#7  course 2 · #8  bed above  3.9 kN  8.000 %  12.5× margin"),
			EJointMarginBand::Comfortable
		},
		{
			/*
			 * EXACTLY ONE THOUSAND NEWTONS — the unit switch, from above. 100000 uu over
			 * 100 uu per newton is 1000.0 N with no rounding anywhere, so this row pins the
			 * boundary itself rather than a value near it. At the boundary the reading is
			 * KILONEWTONS: "1.0 kN" is what a reader wants, and "1000.0 N" is the digit
			 * counting the switch exists to stop.
			 */
			TEXT("exactly one thousand newtons reads in kilonewtons"),
			8, 1000.0, 2.0408163265306123, TEXT("49.0× margin"), 0.56339869334283788,
			TEXT("#8  course 2 · #9  bed above  1.0 kN  2.041 %  49.0× margin"),
			EJointMarginBand::Comfortable
		},
		{
			/* And one tenth of a newton under it, which does not. */
			TEXT("a tenth of a newton below the switch still reads in newtons"),
			9, 999.9, 2.0406122448979592, TEXT("49.0× margin"), 0.5634131705494404,
			TEXT("#9  course 2 · #10  bed above  999.9 N  2.041 %  49.0× margin"),
			EJointMarginBand::Comfortable
		},
		{
			/*
			 * NOTHING ON IT. Two grounded pads share a head joint, and a grounded piece
			 * terminates the load flow, so this joint genuinely carries zero — an intact,
			 * healthy joint with no load, which is a completely ordinary thing for a wall
			 * to contain. Its margin is a division by zero and its bar is FULL, because
			 * unloaded is the most headroom there is.
			 *
			 * IT IS ALSO THE ONE ROW WHOSE FAR END IS IN A DIFFERENT COURSE FROM EVERY OTHER.
			 * The second pad sits beside the first, at the BOTTOM of the ladder, so this is
			 * "course 1 · #1" where its nine neighbours read course 2 — which is what separates
			 * a real position from a handle dressed up as one.
			 */
			TEXT("an intact joint carrying nothing has no margin figure and a full bar"),
			10, 0.0, 0.0, TEXT("no load"), 1.0,
			TEXT("#10  course 1 · #1  head  0.0 N  0.000 %  no load"),
			EJointMarginBand::Comfortable
		},
		{
			/*
			 * AND THE JOINT THAT IS NOT THERE ANY MORE. It reads 0 N at 0 % — bit for bit
			 * identical to the rung above it — and one of them is a hole in the wall. Its
			 * bar is EMPTY where the unloaded joint's is full, which is the whole distinction
			 * expressed in the one field a player actually looks at, and its margin reading
			 * is a different word rather than a different number.
			 *
			 * AND ITS FAR END IS STILL A PLACE, WHICH IS THE ROW THAT SAYS SO. The brick was
			 * pulled out and FPieceBinding kept its box deliberately, so the joint that went
			 * with it can still say WHERE the hole is — which is the single most useful thing
			 * to say about a hole, and exactly the sentence a player who just pulled a brick
			 * is reading. Note the number: handle 12 is the ELEVENTH brick of its course,
			 * because the ten rungs come first, so a handle printed as a position would read
			 * "#12" here and be wrong by one in the one row nobody would check.
			 */
			TEXT("a joint that has given reads apart from an intact joint with nothing on it"),
			11, 0.0, 0.0, TEXT("gone"), 0.0,
			TEXT("#11  course 2 · #11  bed above  broken (went with a removed piece)"),
			EJointMarginBand::Critical
		},
		{
			/*
			 * AND THE FAR END NOBODY CAN PLACE, WHICH IS THE ONLY WAY A JOINT ROW REACHES THE
			 * FALLBACK AT ALL.
			 *
			 * A far end is a bare HANDLE in the inspected brick's own structure, so the two
			 * things that send an ENTRY label to the fallback — a ref naming another wall, and
			 * a ref missing a half — cannot happen to one. What is left is a brick whose box
			 * says nowhere, and this row is it.
			 *
			 * WHAT IT SPELLS IS THE ENTRY LABEL'S FALLBACK, WORD FOR WORD: "brick 21:13", the
			 * binding's own structure id and the handle. Two properties decide that rather than
			 * taste. It cannot COLLIDE with a positioned far end, because no position contains
			 * a colon and none begins with "brick". And it is the SAME STRING the entry list
			 * would print for that brick, so a player reading "brick 21:13" in a joint row and
			 * "brick 21:13" in the list above knows they are looking at one brick — which the
			 * unqualified "brick 13" would leave them to work out, in the one panel that is
			 * supposed to have stopped naming bricks two different ways.
			 *
			 * The structure id is the BINDING'S because a far end is always in it: a joint row
			 * can never name another wall, which is the one way this fallback is narrower than
			 * the entry list's.
			 *
			 * ITS LOAD IS RUNG 0'S EXACTLY, so the two rows differ in one field and nothing
			 * else. A NaN centre is invisible to the solver — FStructure has no positions —
			 * so this is a perfectly healthy joint with an unnameable brick on the end of it.
			 */
			TEXT("a far end nobody can place falls back to the ref-shaped label"),
			12, 49.0, 0.1, TEXT("1000× margin"), 1.0,
			TEXT("#12  brick 21:13  bed above  49.0 N  0.100 %  1000× margin"),
			EJointMarginBand::Comfortable
		},
		{
			/*
			 * AND THE ONE JOINT ON THE LADDER THAT IS BEING LEVERED OPEN, WHICH IS THE ROW THE
			 * OTHER TWELVE CANNOT WRITE.
			 *
			 * PUT THE NUMBERS SIDE BY SIDE AND THE PROBLEM IS THE WHOLE POINT. This joint
			 * carries 548.8 N — barely more than rung 2's 490, a ninth of rung 3 — and it
			 * sits at 49 % of capacity where rung 2 sits at 1 %. Force and percentage are both
			 * true, both printed, and there is no arithmetic between them: the missing term is
			 * a 2,195.2 N·cm bend that nothing on the line mentions. That is this subsystem's
			 * recurring signature — a plausible number that does not describe what is
			 * happening — and MOMENTS_DESIGN.md names closing it as part of the moment work
			 * rather than as a follow-up, for exactly this reason.
			 *
			 * WHY THE MOMENT AND NOT THE ECCENTRICITY. M / F is a lever arm in centimetres and
			 * "4.0 cm off centre" is arguably friendlier than "313.6 N·cm". It is rejected on
			 * three grounds, and the third is decisive. It is DERIVED — a quotient of two
			 * numbers, computed in the presenter, which is precisely the second-copy shape
			 * Core/PieceInspection.h's whole discipline is written against. It divides by a
			 * force, and the force on a bent joint is free to be small. And MOMENTS_DESIGN.md
			 * slice 5 makes a joint RECEIVE moment along the load path from the wall above it,
			 * at which point M / F stops being that joint's lever arm and becomes a length in
			 * centimetres that describes nothing — a wrong answer wearing the units of a right
			 * one, which is the failure mode this line exists to remove rather than relocate.
			 * The moment is also the quantity ComputeUtilisation actually consumes, so printing
			 * it means the line names both inputs to the percentage beside it.
			 *
			 * AND N·cm RATHER THAN N·m, pinned once beside the formatting exactly as the
			 * kilonewton switch is. Centimetres are this game's length unit everywhere else —
			 * the brick is 21.5 cm, the kern of a bed patch is ±1.708 cm — and the interesting
			 * range collapses in metres: a 313.6 N·cm bend is 3.136 N·m, where one step of the
			 * last printed digit is a whole 10 N·cm. There is NO new conversion boundary either
			 * way: a moment is uu.cm, length is already centimetres, so this is
			 * DestructionPresenter::ForceUnitsPerNewton applied once and nothing else.
			 *
			 * THE CLAUSE TRAILS ITS NUMBER, matching every other clause on the line — "548.8 N",
			 * "49.000 %", "2.0× margin" are all <number> <word> — and it appears ONLY on a
			 * joint that has one. Twelve rows above assert the absence.
			 */
			TEXT("a joint levered open by an off-centre load says what is bending it"),
			13, 548.8, 49.0, TEXT("2.0× margin"), 0.1032679733238288,
			TEXT("#13  course 2 · #12  bed above  548.8 N  49.000 %  2.0× margin  2195.2 N·cm bending"),
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
	 * AND THE MOMENT ON EVERY ROW IS THE MODEL'S OWN NUMBER, EXACTLY, HELD ON THE ONE FIXTURE
	 * IN THIS FILE THAT HAS SOMETHING TO BE WRONG ABOUT.
	 *
	 * Presenter.PieceMenuJointReadout is where the whole row is swept against InspectPiece with
	 * exact equality, and it will go on passing this particular field whatever anybody does to
	 * it: its fixture supplies no joint geometry, so every moment there is zero on both sides
	 * and 0 == 0 forever. The ladder bends on exactly one rung, so the same claim is worth
	 * something here — a presenter that recomputed the moment from a lever arm rather than
	 * reading it back would agree to about fifteen places and differ in the last bit, which is
	 * precisely the drift a tolerance lets through.
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
		 * THE BAR'S FRACTION, TO TWELVE PLACES, AGAINST A NUMBER WORKED OUT BY HAND. A log
		 * curve is the shape where a wrong answer stays a plausible answer — a bar drawn on
		 * a natural log instead of a base-ten one, or over two decades instead of three, is
		 * still a bar that moves in the right direction and fills up for healthy joints. The
		 * decade rows are what separate those: 1000x, 100x, 10x and 1x must land on exactly
		 * 1, two thirds, one third and zero, and nothing but clamp(log10(margin)/3) does.
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
		 * AND THE BAND THE BAR IS DRAWN IN. Every bar on the panel is the same green today, so
		 * the joint at 200 % of capacity and the joint at a ten-thousandth of it are the same
		 * colour and differ only in a length nobody has a reference for. Which side of an edge a
		 * joint falls on is a decision, so it is the model's; the hue it maps to is the widget's.
		 */
		TestTrue(
			*FString::Printf(TEXT("%s: should be in the %s band, it is %s %s"),
				Case.Description, NameOfBand(Case.ExpectedBand), NameOfBand(Row.MarginBand),
				*DescribeInspector(Inspector)),
			Row.MarginBand == Case.ExpectedBand);
	}

	/*
	 * THE SCALE, PINNED AS TEXT AND AS POSITION — AND THEN TIED TO THE CURVE.
	 *
	 * The ticks matching the four decade rungs is what makes the labels TRUE rather than
	 * merely present: a caption saying "1000×" over a bar that actually fills at a hundred
	 * would draw perfectly and mislead completely, and it is exactly the plausible-picture-
	 * over-a-wrong-number failure this project has already paid for once. So each tick's
	 * fraction is held against the HEADROOM of the rung whose margin is that tick's number,
	 * which is a fact about the two halves agreeing rather than about either one alone.
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
	 * AND THE CURVE ONLY EVER GOES ONE WAY. Swept over every pair of intact rungs rather
	 * than checked at the four decades, because a sign slip or a reciprocal taken twice
	 * produces a bar that is smooth, bounded, correct at the ends and backwards in the
	 * middle — which no individual expected value in the table above would catch on its
	 * own. More load can never mean more headroom.
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
	 * AND THE BAND IS MONOTONE IN THE LOAD, SWEPT OVER EVERY PAIR OF INTACT RUNGS RATHER THAN
	 * CHECKED AT THE BOUNDARIES.
	 *
	 * The same argument the headroom sweep above makes, and it catches the same class of defect:
	 * a comparison written the wrong way round, or a band chain whose guards are in the wrong
	 * order, produces a colouring that is correct at the ends of the ladder and backwards in the
	 * middle — which no individual expected value can see. More load can never mean a calmer
	 * colour. Compared through SeverityOfBand rather than through the enumerators' own values,
	 * because their numeric order is a fail-closed decision rather than a scale.
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

/** Named again, and named apart again — see the note on PieceInspectorTestSupport. */
namespace PieceBandBoundaryTestSupport
{
	using namespace PieceInspectorTestSupport;

	constexpr int32 BandStructure = 33;

	/**
	 * THE SAME 49 cm2 OF GENERAL PURPOSE MORTAR THE HEADROOM LADDER USES, so the one line of
	 * arithmetic carries over unchanged: a brick of M kilograms on this joint loads it to
	 * exactly M / 5000 of capacity, and its margin is 5000 / M.
	 *
	 * A SECOND, SHORTER LADDER RATHER THAN FOUR MORE RUNGS ON THE FIRST, and the reason is
	 * arithmetic rather than taste: every expected LINE in the headroom table names its far end
	 * by position — "course 2 · #11" — and its joint by connection index, so inserting rungs
	 * renumbers rows that are pinned character for character. A boundary fixture that forced a
	 * rewrite of thirteen unrelated expectations would be a change nobody could review.
	 */
	constexpr double BandJointAreaSqCm = 49.0;
	constexpr double BandMassPerFullLoadKg = 5000.0;

	/**
	 * THE FOUR MASSES THE BOUNDARIES NEED, AND THE MAIN LADDER CANNOT SUPPLY.
	 *
	 * The load ladder holds 10x margin (exactly the green/amber edge) and 12.5x, and then nothing
	 * at all between 1x and 10x — so the amber/red edge at 2x is unpinned everywhere in the suite,
	 * and an implementation that split amber from red at 3x, or at 5x, would pass every existing
	 * row. These four are the two rungs either side of each edge:
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
 * A JOINT'S BAR IS COLOURED BY HOW MUCH ROOM IT HAS LEFT, AND WHICH SIDE OF EACH EDGE IT FALLS
 * ON IS DECIDED HERE RATHER THAN BY A WIDGET COMPARING NUMBERS.
 *
 * WHY THIS IS A MODEL FIELD AND NOT A SLATE TERNARY. Every bar on the panel is the same green
 * today, which makes the one joint that is nearly gone look exactly like the five that are three
 * orders of magnitude from failing — the bar's own fill says it, but a fill is a length and a
 * length has to be compared against its neighbours to mean anything. A colour does not. Choosing
 * WHERE the colour changes is a decision about what the game considers dangerous, and a widget
 * holding `Fraction > 0.5f ? Green : Red` is that decision written where no test can read it,
 * beside a constant nobody would ever revisit.
 *
 * THE EDGES ARE 10x AND 2x MARGIN, AND THE BOUNDARY ROWS ARE WHY THIS TEST EXISTS AT ALL. Any
 * two implementations agree about a joint at 1000x and a joint at 200 %; they differ at exactly
 * ten times and exactly twice, and those are the rows a hand-picked example never contains.
 *
 * AT AN EDGE THE JOINT TAKES THE WORSE BAND, UNIFORMLY, AND THAT IS THE FAIL-CLOSED DIRECTION.
 * A joint at exactly 10x is amber and a joint at exactly 2x is red — never the other way — for
 * the same reason PresenterMarginText says "no margin left" at exactly 1.0 rather than
 * "1.0x margin": over-promising is the expensive direction on a panel whose whole job is to say
 * what is about to fall down. Written as a chain of guards on the UTILISATION already on the row
 * (comfortable below 10 %, cautious below 50 %, critical otherwise), which puts a NaN in the
 * critical band by the same mechanism the margin text and the bar fill already use — every
 * comparison against a NaN is false, so it falls out of the bottom of the chain.
 *
 * AND IT IS A TRANSFORM OF UtilisationPercent, NEVER A THIRD TRIP TO THE GRAPH. The percentage
 * on the row is the number PieceMenuJointReadout holds against InspectPiece with exact equality;
 * a band derived from the connection again would be a fourth copy of the break decision, and this
 * project has paid twice for the second.
 *
 * NEEDS A TICKING WORLD: no, and not even a world.
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
			 * EXACTLY TEN TIMES, which is the row the whole "worse band at the edge" rule turns
			 * on. `Margin >= 10 ? green : amber` is the coin-flip alternative and it differs from
			 * the rule on exactly this joint and nowhere else in the suite.
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
			 * AND EXACTLY TWICE, the other edge, worked the same way. A joint that could take
			 * exactly one more of itself is not a joint to describe as having room.
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
		 * FIXTURE PRECONDITION, HAND-DERIVED: the rung is carrying what this file thinks it is.
		 * Green on arrival and driving nothing — but without it a fixture that stopped loading
		 * its joints would quietly retarget every band expectation onto some other number.
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
 * EVERY JOINT ROW CARRIES THE COLOUR SLOT OF ITS OWN POSITION IN THE LIST — ROW 0 IS ALWAYS THE
 * FIRST COLOUR — AND THE PALETTE RUNS OUT RATHER THAN REPEATING.
 *
 * WHAT THE SWATCH IS FOR. A joint row names the brick at the far end in words ("course 2 · #4"),
 * and in a wall of 1,220 identical bricks a word is not enough to find one by: the design ties
 * each row to its brick by COLOUR, so the row and the brick light up together. This slice is the
 * model deciding WHICH slot each row is; lighting the brick is the world half and is not here.
 *
 * PER SLOT, NOT PER BRICK, AND THE COST IS ACCEPTED RATHER THAN HIDDEN. Keying the colour on the
 * far-end brick is what a reader assumes is happening and it cannot be built: a palette is a
 * handful of legible hues and a wall is over a thousand bricks, so it must collide, and two rows
 * in one colour is a lie about the single thing the swatch says. Keyed on the ROW it never
 * collides and it is stable while a player scans down a readout — at the price that one brick is
 * the first colour in one readout and the second in another. THAT COST IS ASSERTED HERE, on the
 * one joint that appears in two readouts, so nobody can mistake it for a defect later.
 *
 * AND PAST THE END OF THE PALETTE, NOTHING. Wrapping is the tidy answer and it reintroduces
 * exactly the collision per-slot was chosen to avoid, on the brick with the most joints — which
 * is the brick being looked at hardest. An absent swatch is an absence; a repeated swatch is a
 * wrong answer. The load ladder is what reaches that end: thirteen joints on one pad, which no
 * wall a player builds will produce and no hand-written fixture would otherwise contain.
 *
 * THE STRUCTURAL PROPERTIES — row i takes slot i, and once out the palette stays out — are swept
 * over EVERY readout in this file by CheckInspectorInvariants rather than checked here, because
 * they must hold of the knot, the unsolved wall and the position fixtures too.
 *
 * NEEDS A TICKING WORLD: no, and not even a world.
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
	 * AN ORDINARY BRICK'S JOINTS ALL GET A COLOUR, IN LIST ORDER. Three rows, three slots, first
	 * to last — including the SEVERED one, which is the row a player who just pulled a brick is
	 * looking for and would be the cheapest one to quietly drop.
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
	 * THE SAME JOINT, IN TWO READOUTS, IN TWO DIFFERENT SLOTS — WHICH IS THE WHOLE PER-SLOT
	 * DECISION STATED AS A FACT RATHER THAN AS A COMMENT.
	 *
	 * Connection 1 joins the subject and the rider. It is the SECOND row of the subject's
	 * breakout and the FIRST of the rider's, so per-slot means slot 1 there and slot 0 here. Any
	 * implementation that keyed the colour on the connection, on the far-end handle, or on
	 * anything else about the brick would give it the same slot twice and fail exactly one of
	 * these two assertions — which is the only way to tell the two designs apart at all, since
	 * every other row in this file inspects one brick at a time.
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

	/*
	 * AND THE LADDER, WHICH IS THE ONLY FIXTURE WITH MORE JOINTS ON ONE PIECE THAN A PALETTE IS
	 * LIKELY TO HOLD. Fourteen rows off one pad.
	 */
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
	 * SIX ROWS IS THE FLOOR, AND IT IS THE WALL'S NUMBER RATHER THAN A ROUND ONE. A brick inside
	 * a running bond is spanned by two above, rests on two below and has a head joint either side
	 * — six, which is exactly what the panel showed on the capture this work came from. A palette
	 * that ran out before then would leave the ordinary case half-coloured.
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
	 * AND NO TWO ROWS OF ONE READOUT SHARE A COLOUR, WHICH IS THE PROPERTY WRAPPING BREAKS.
	 *
	 * It is implied by "row i takes slot i" on a list this length, and it is asserted anyway
	 * because it is the claim itself: the swatch exists to tell one row's brick from another's,
	 * and a modulo that made rows 0 and 6 the same colour would be a readout that quietly points
	 * at two bricks with one hue on the brick with the most joints.
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
 * EVERY BRICK ROW CARRIES ITS SUPPORT STATE AS A BUCKET AS WELL AS A WORD, SO A COLOURED DOT IS
 * THE MODEL'S DECISION AND ONLY THE HUE IS THE WIDGET'S.
 *
 * WHAT IS BROKEN TODAY, AND WHY IT IS A MODEL PROBLEM RATHER THAN A DRAWING ONE. FInspectorPieceEntry
 * carries SupportText and nothing else about support, so a widget asked for a dot per row has
 * exactly one way to choose its colour: compare that string against literals. That is a policy
 * written in the one place this project has a recorded exception saying there may be no logic at
 * all, and it fails silently in both directions — it stops colouring the day the wording is retuned
 * (and the wording is deliberately pinned in the model, which is an invitation to retune it), and
 * it colours the wrong dot the day a sixth word is added.
 *
 * THE SPLIT IS THE ONE EJointMarginBand ALREADY MADE, and it is made here for the same reason:
 * which side of a line a value falls on is a DECISION, and the palette is taste. The bucket is the
 * decision; which green, which amber, which grey stays out in the widget where nothing headless can
 * judge it anyway.
 *
 * SIX BUCKETS, NOT FOUR AND NOT FIVE, AND THE TWO EXTRA ONES ARE THE ARGUMENT. Grounded, supported,
 * stranded and falling are the four physical states. "not in this wall" is a fifth, and it is a
 * BUCKET rather than an absence because the alternative is either a second bool for "has a bucket"
 * — a field free to disagree with the one beside it, which is the defect this whole struct is
 * shaped against — or reusing Falling, which is exactly the fail-open conflation PresenterWordForSupport
 * exists to undo. And "not solved yet" is a sixth, because collapsing it into the fifth is a
 * ONE-WAY door: a widget handed two enumerators can paint them one colour with a lookup, and a
 * widget handed one can never tell them apart again.
 *
 * THE CROSS-CHECK IS WHERE THE VALUE IS, AND IT IS SWEPT RATHER THAN TABLED. CheckInspectorInvariants
 * now holds every row's bucket against that row's own word, and the MARKED row's bucket against the
 * readout's — the same pair of claims SupportText already carries. A panel that said "grounded" in
 * the list and drew the falling colour two inches below it would be the drift this project keeps
 * paying for, in the one field a player reads without reading any text.
 *
 * THE TABLE BELOW IS WHAT DRIVES IT RED. The sweep alone is satisfied by a model that never fills
 * the field at all — every row would bucket NotAPiece and every word would have to be "not in this
 * wall", which is false of five of them, so in practice the sweep bites too — but the per-case
 * expectations are what say WHICH bucket each of the six states is, hand-written from the fixture
 * diagram rather than read back off the binding.
 *
 * NEEDS A TICKING WORLD: no, and not even a world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceMenuSupportBandTest,
	"DestructionGame.Presenter.PieceMenuSupportBand",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceMenuSupportBandTest::RunTest(const FString& Parameters)
{
	using namespace PieceInspectorTestSupport;

	/*
	 * TABLE INTEGRITY FIRST, AND IT IS NOT BOOKKEEPING. The whole sweep in CheckInspectorInvariants
	 * is "the bucket determines the word"; if two buckets shared a word it would stop separating
	 * them and would go on passing over a model that had merged the two.
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
	 * FIXTURE PRECONDITIONS, ASKED OF THE SOLVER. Four of the six buckets are only reachable if
	 * the graph really is in the four states the diagram claims, and a fixture that stopped
	 * producing one of them would quietly retarget its row onto whatever state came instead.
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
			 * EVERY BUCKET THE SOLVED WALL CAN PRODUCE, IN ONE LIST, WITH NOTHING SINGLED OUT.
			 *
			 * THIS IS THE ROW THE COLUMN EXISTS FOR. Seven picked bricks are seven identical rows
			 * without it, so the one that is falling and the one the solver gave up on can only be
			 * found by hovering each in turn — and the three refs that name nothing at all present
			 * exactly like the four that do.
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
			 * A RELEASED BRICK IS A LIVE PIECE THAT NOTHING IS HOLDING UP, and this is the row where
			 * the dot earns its place: the menu for this selection is EMPTY, so the only thing on
			 * the panel explaining why is this row's word and this row's colour.
			 */
			TEXT("the released floater, singled out"),
			{ Floater }, Floater,
			{ EPieceSupportBand::Falling },
			EPieceSupportBand::Falling
		},
		{
			/*
			 * AND THE ONE BUCKET THAT IS NOT A PHYSICAL CLAIM AT ALL. Stranded means the solver
			 * could not route this brick, so the numbers beside it are worth doubting — and in a
			 * list of eleven rows it is the only thing that would say so. Painting it as grounded
			 * is the same fail-open direction Integration.PullingSupportBringsTheWallDown polices.
			 */
			TEXT("a brick the solver stranded in a knot, singled out"),
			{ KnotX }, KnotX,
			{ EPieceSupportBand::Stranded },
			EPieceSupportBand::Stranded
		},
		{
			/*
			 * AND A REF THAT NAMES NOTHING NEVER BECOMES THE READOUT'S SUBJECT, so the readout
			 * buckets as the value that claims nothing while the ROW still says what it is.
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
	 * AND THE SIXTH BUCKET, ON ITS OWN FIXTURE, BECAUSE IT IS THE ONE A SOLVED WALL CANNOT REACH.
	 *
	 * "Nobody has solved yet" is its own bucket for exactly the reason it is its own sentence:
	 * EPieceSupport::Falling is both a real collapse and an absent answer, so a bucket taken
	 * straight off the enumerator would paint a freshly built wall in the colour of a wall coming
	 * down. That is a catastrophe drawn that has not happened, and in a column of forty dots it is
	 * far louder than in one line of text.
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
