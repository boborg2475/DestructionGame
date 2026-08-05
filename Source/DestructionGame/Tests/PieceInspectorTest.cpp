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
			Line += TEXT("<none>");
		}

		for (int32 Index = 0; Index < Inspector.Joints.Num(); ++Index)
		{
			const FInspectorJointRow& Joint = Inspector.Joints[Index];

			Line += FString::Printf(
				TEXT("%s[c%d->p%d %s %.6f N %.9f %% %s pass=%d margin:'%s' bar=%.9f '%s']"),
				Index == 0 ? TEXT("") : TEXT(" "),
				Joint.ConnectionIndex, Joint.OtherPieceIndex, NameOfRole(Joint.Role),
				Joint.ForceN, Joint.UtilisationPercent,
				Joint.bHasGiven ? TEXT("GIVEN") : TEXT("intact"),
				Joint.BreakPass, *Joint.MarginText, Joint.HeadroomFraction, *Joint.Text);
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
			}
		}

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
			TEXT("")
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
			{ TEXT("course 3 · #1"), TEXT("course 1 · #1"), TEXT("course 2 · #1") },
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
			{ TEXT("course 3 · #1"), TEXT("course 1 · #1"), TEXT("course 2 · #1") },
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
			{ TEXT("course 3 · #1"), TEXT("course 1 · #1") },
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
			{ TEXT("course 2 · #1"), TEXT("course 2 · #2") },
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
			{ TEXT("course 2 · #1"), TEXT("brick 9:1") },
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
			{ TEXT("course 2 · #1"), TEXT("brick 4:-1") },
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
			{ TEXT("course 4 · #1") },
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
			{ TEXT("course 1 · #1"), TEXT("course 1 · #1") },
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
			{ TEXT("course 1 · #1"), TEXT("course 1 · #1") },
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
			{ TEXT("course 1 · #2") },
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

	void BuildLoadLadder(FStructureBinding& Out)
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

		for (int32 Rung = 0; Rung < LadderMassesKg.Num(); ++Rung)
		{
			AddLadderJoint(Out, Rung + 1, InspectorBedNormal);
		}

		AddLadderJoint(Out, LadderNoLoadPiece, InspectorHeadNormal);
		AddLadderJoint(Out, LadderRemovedPiece, InspectorBedNormal);

		/* Appended LAST, so every connection index the table below names stays where it was. */
		AddLadderJoint(Out, LadderUnplaceablePiece, InspectorBedNormal);

		Out.RemovePiece(LadderRemovedPiece);
		Out.SolveLoads();
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
	BuildLoadLadder(Binding);

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
			TEXT("#0  course 2 · #1  bed above  49.0 N  0.100 %  1000× margin")
		},
		{
			/*
			 * A HUNDREDTH — and the row that pins the FORMAT boundary from above. 100× is
			 * an integer; the 98.04× rung below it keeps a decimal. A rule with no boundary
			 * row is a rule nothing checks.
			 */
			TEXT("a hundredth of capacity: two decades of bar, and a whole-number margin"),
			1, 490.0, 1.0, TEXT("100× margin"), 2.0 / 3.0,
			TEXT("#1  course 2 · #2  bed above  490.0 N  1.000 %  100× margin")
		},
		{
			TEXT("a tenth of capacity: one decade of bar, and a margin worth a decimal"),
			2, 4900.0, 10.0, TEXT("10.0× margin"), 1.0 / 3.0,
			TEXT("#2  course 2 · #3  bed above  4.9 kN  10.000 %  10.0× margin")
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
			TEXT("#3  course 2 · #4  bed above  49.0 kN  100.000 %  no margin left")
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
			TEXT("#4  course 2 · #5  bed above  4.9 N  0.010 %  10000× margin")
		},
		{
			/*
			 * JUST UNDER THE FORMAT BOUNDARY: 5000/51 is 98.0392..., which keeps its
			 * decimal where the 100× rung above loses it. Pairing the two is the only way
			 * the "at or above 100×" half of the rule is falsifiable.
			 */
			TEXT("just under a hundred times: still a decimal"),
			5, 499.8, 1.02, TEXT("98.0× margin"), 0.66379994274602749,
			TEXT("#5  course 2 · #6  bed above  499.8 N  1.020 %  98.0× margin")
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
			TEXT("#6  course 2 · #7  bed above  98.0 kN  200.000 %  no margin left")
		},
		{
			TEXT("twelve and a half times, between two decades"),
			7, 3920.0, 8.0, TEXT("12.5× margin"), 0.36563667100268549,
			TEXT("#7  course 2 · #8  bed above  3.9 kN  8.000 %  12.5× margin")
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
			TEXT("#8  course 2 · #9  bed above  1.0 kN  2.041 %  49.0× margin")
		},
		{
			/* And one tenth of a newton under it, which does not. */
			TEXT("a tenth of a newton below the switch still reads in newtons"),
			9, 999.9, 2.0406122448979592, TEXT("49.0× margin"), 0.5634131705494404,
			TEXT("#9  course 2 · #10  bed above  999.9 N  2.041 %  49.0× margin")
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
			TEXT("#10  course 1 · #1  head  0.0 N  0.000 %  no load")
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
			TEXT("#11  course 2 · #11  bed above  broken (went with a removed piece)")
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
			TEXT("#12  brick 21:13  bed above  49.0 N  0.100 %  1000× margin")
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

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
