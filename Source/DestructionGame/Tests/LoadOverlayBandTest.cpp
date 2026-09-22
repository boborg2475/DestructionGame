// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Connection.h"
#include "Core/ConnectionStrength.h"
#include "Core/PieceMenu.h"
#include "Core/Structure.h"
#include "World/BrickActor.h"

#if WITH_DEV_AUTOMATION_TESTS

/** Uniquely named namespace with prefixed names: unity builds merge files (see CURRENT_STATE.md). */
namespace LoadOverlayBandTestSupport
{
	/**
	 * Force units per MPa over one cm2: 1 MPa = 100 N/cm2 = 10,000 uu. Deliberately not imported
	 * from ForceUnitsPerMPaSqCm, so a 100x error there cannot agree with itself here (CLAUDE.md units).
	 */
	constexpr double LoadOverlayForceUnitsPerMPaSqCm = 10000.0;

	/** Weight of 1 kg in force units: gravity is 980 cm/s2, which already includes 1 N = 100 uu. */
	constexpr double LoadOverlayWeightUuPerKg = 980.0;

	/**
	 * Band edges at 10 % (10x margin) and 50 % (2x) of capacity, transcribed from the design, not read
	 * from PieceMenu.cpp, so the overlay is checked against the inspector's numbers. At an edge the
	 * worse band wins; guards run worst-first and negated so NaN lands in Critical.
	 */
	constexpr double LoadOverlayCautionAtPercent = 10.0;
	constexpr double LoadOverlayCriticalAtPercent = 50.0;

	EJointMarginBand LoadOverlayBandForPercent(double UtilisationPercent)
	{
		if (!(UtilisationPercent < LoadOverlayCriticalAtPercent))
		{
			return EJointMarginBand::Critical;
		}

		if (!(UtilisationPercent < LoadOverlayCautionAtPercent))
		{
			return EJointMarginBand::Caution;
		}

		return EJointMarginBand::Comfortable;
	}

	const TCHAR* LoadOverlayBandName(EJointMarginBand Band)
	{
		switch (Band)
		{
		case EJointMarginBand::Comfortable: return TEXT("Comfortable");
		case EJointMarginBand::Caution:     return TEXT("Caution");
		case EJointMarginBand::Critical:    return TEXT("Critical");
		}

		return TEXT("<a band this build has never heard of>");
	}

	const TCHAR* LoadOverlayHighlightName(EBrickHighlight Highlight)
	{
		switch (Highlight)
		{
		case EBrickHighlight::None:            return TEXT("None");
		case EBrickHighlight::Hovered:         return TEXT("Hovered");
		case EBrickHighlight::Selected:        return TEXT("Selected");
		case EBrickHighlight::Inspected:       return TEXT("Inspected");
		case EBrickHighlight::Neighbour0:      return TEXT("Neighbour0");
		case EBrickHighlight::Neighbour1:      return TEXT("Neighbour1");
		case EBrickHighlight::Neighbour2:      return TEXT("Neighbour2");
		case EBrickHighlight::Neighbour3:      return TEXT("Neighbour3");
		case EBrickHighlight::Neighbour4:      return TEXT("Neighbour4");
		case EBrickHighlight::Neighbour5:      return TEXT("Neighbour5");
		case EBrickHighlight::LoadComfortable: return TEXT("LoadComfortable");
		case EBrickHighlight::LoadCaution:     return TEXT("LoadCaution");
		case EBrickHighlight::LoadCritical:    return TEXT("LoadCritical");
		}

		return TEXT("<a highlight this build has never heard of>");
	}

	/**
	 * The strength every joint here carries. Loads are vertical through +Z normals, so compression
	 * governs (ComputeUtilisation takes the worst axis; DESIGN §4). Shear and tension capacities are
	 * non-zero so unloaded axes read 0, not 0/0. 1 MPa is a round test value, not brick's real figure.
	 */
	FConnectionStrength LoadOverlayJointStrength()
	{
		FConnectionStrength Strength;
		Strength.CompressiveStrengthMPa = 1.0;
		Strength.ShearCohesionMPa = 1.0;
		Strength.TensileStrengthMPa = 1.0;
		Strength.FrictionCoefficient = 0.6;
		Strength.MaxShearStrengthMPa = 2.0;
		return Strength;
	}

	/** Bed joint area, cm2. At 1 MPa the capacity is 1,000,000 uu. */
	constexpr double LoadOverlayJointAreaSqCm = 100.0;

	/**
	 * A bed joint under Above. The normal points toward PieceB (ConnectionLoad.h), so +Z with the
	 * supported piece as B is compression; reversed it would be tension. No centre or rectangle, so
	 * there is no moment and utilisation is bare force over area.
	 */
	FConnection LoadOverlayBedJoint(int32 Below, int32 Above)
	{
		FConnection Connection;
		Connection.PieceA = Below;
		Connection.PieceB = Above;
		Connection.InterfaceNormal = FVector::ZAxisVector;
		Connection.InterfaceAreaSqCm = LoadOverlayJointAreaSqCm;
		Connection.Strength = LoadOverlayJointStrength();
		return Connection;
	}

	/**
	 * Percent of capacity for a bed joint carrying this mass: 100 * MassKg * 980 / 1,000,000. So 50 kg
	 * reads 4.9 %, 250 kg 24.5 %, 750 kg 73.5 %, each well clear of a band edge.
	 */
	double LoadOverlayPercentForMassKg(double MassAboveKg)
	{
		const double DemandUu = MassAboveKg * LoadOverlayWeightUuPerKg;

		const double CapacityUu = LoadOverlayJointStrength().CompressiveStrengthMPa
			* LoadOverlayJointAreaSqCm
			* LoadOverlayForceUnitsPerMPaSqCm;

		return 100.0 * DemandUu / CapacityUu;
	}

	// One mass per band (see LoadOverlayPercentForMassKg).
	constexpr double LoadOverlayComfortableMassKg = 50.0;   // 4.9 %
	constexpr double LoadOverlayCautionMassKg = 250.0;      // 24.5 %
	constexpr double LoadOverlayCriticalTopMassKg = 700.0;  // with the 50 kg above it: 73.5 %

	/**
	 * Unconnected stacks reaching all three bands:
	 *
	 *     piece 2   50 kg      ---- joint 0 (added first) ...... carries 50 kg,  4.9 %  Comfortable
	 *     piece 1  700 kg      ---- joint 1 (added second) ..... carries 750 kg, 73.5 % Critical
	 *     piece 0  grounded
	 *
	 *     piece 5    0 kg      ---- joint 3 ..................... carries 0 kg,    0.0 %  Comfortable
	 *     piece 4  250 kg      ---- joint 2 ..................... carries 250 kg, 24.5 % Caution
	 *     piece 3  grounded
	 *
	 *     piece 6  grounded, no joints (the first brick a player lays)
	 *
	 *     piece 8    50 kg     ---- joint 4 ..................... carries 0-50 kg, Comfortable
	 *     piece 7   700 kg     nothing beneath: both unsupported
	 *
	 * Piece 1's comfortable joint has the lower index, so first, last, mean and minimum all miss
	 * Critical; only the worst finds it. Piece 6 and pieces 7/8 test support-first: grounded and
	 * jointless, and floating with a lightly loaded intact joint. Piece 5 is massless so removing it
	 * leaves piece 4's live joint at exactly 24.5 %.
	 */
	struct FLoadOverlayFixture
	{
		FStructure Structure;

		int32 GroundedBase = INDEX_NONE;
		int32 CriticalMiddle = INDEX_NONE;
		int32 ComfortableTop = INDEX_NONE;
		int32 CautionBase = INDEX_NONE;
		int32 CautionTop = INDEX_NONE;
		int32 MasslessTopper = INDEX_NONE;
		int32 Jointless = INDEX_NONE;
		int32 FloatingBase = INDEX_NONE;
		int32 FloatingTop = INDEX_NONE;

		/** The joint between the floating pieces (index 4). */
		int32 FloatingJoint = INDEX_NONE;

		void Build()
		{
			GroundedBase = Structure.AddPiece(10.0, true);
			CriticalMiddle = Structure.AddPiece(LoadOverlayCriticalTopMassKg, false);
			ComfortableTop = Structure.AddPiece(LoadOverlayComfortableMassKg, false);

			// Top joint first, so the middle piece's comfortable joint has the lower index.
			Structure.AddConnection(LoadOverlayBedJoint(CriticalMiddle, ComfortableTop));
			Structure.AddConnection(LoadOverlayBedJoint(GroundedBase, CriticalMiddle));

			CautionBase = Structure.AddPiece(10.0, true);
			CautionTop = Structure.AddPiece(LoadOverlayCautionMassKg, false);

			Structure.AddConnection(LoadOverlayBedJoint(CautionBase, CautionTop));

			// Zero mass is allowed (see FStructure::AddPiece).
			MasslessTopper = Structure.AddPiece(0.0, false);

			Structure.AddConnection(LoadOverlayBedJoint(CautionTop, MasslessTopper));

			Jointless = Structure.AddPiece(5.0, true);

			// The floating pair, appended last so no existing index moves. No path to the ground.
			FloatingBase = Structure.AddPiece(LoadOverlayCriticalTopMassKg, false);
			FloatingTop = Structure.AddPiece(LoadOverlayComfortableMassKg, false);

			FloatingJoint = Structure.AddConnection(LoadOverlayBedJoint(FloatingBase, FloatingTop));
		}
	};
}

/**
 * BrickHighlightForLoadBand maps each EJointMarginBand to its own EBrickHighlight (LoadComfortable,
 * LoadCaution, LoadCritical) and an unknown band to None. One function keeps the overlay and the
 * inspector on one table (SESSION_UI_DESIGN §a principle 6).
 *
 * All highlight states must be distinct, so an overlaid brick never looks selected. An unknown band
 * fails closed to None: green would be a positive claim of safety. No world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLoadOverlayBandToHighlightTest,
	"DestructionGame.Core.LoadOverlay.BandToBrickHighlight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLoadOverlayBandToHighlightTest::RunTest(const FString& Parameters)
{
	using namespace LoadOverlayBandTestSupport;

	struct FBandRow
	{
		EJointMarginBand Band;
		EBrickHighlight Expected;
	};

	const FBandRow Rows[] = {
		{ EJointMarginBand::Comfortable, EBrickHighlight::LoadComfortable },
		{ EJointMarginBand::Caution,     EBrickHighlight::LoadCaution },
		{ EJointMarginBand::Critical,    EBrickHighlight::LoadCritical },
	};

	for (const FBandRow& Row : Rows)
	{
		const EBrickHighlight Actual = BrickHighlightForLoadBand(Row.Band);

		TestEqual(
			FString::Printf(
				TEXT("a %s joint must put its piece in the %s state; it answered %s"),
				LoadOverlayBandName(Row.Band), LoadOverlayHighlightName(Row.Expected),
				LoadOverlayHighlightName(Actual)),
			static_cast<int32>(Actual), static_cast<int32>(Row.Expected));
	}

	// Every state in the enum, so distinctness is checked against the whole vocabulary.
	const EBrickHighlight AllStates[] = {
		EBrickHighlight::None,
		EBrickHighlight::Hovered,
		EBrickHighlight::Selected,
		EBrickHighlight::Inspected,
		EBrickHighlight::Neighbour0,
		EBrickHighlight::Neighbour1,
		EBrickHighlight::Neighbour2,
		EBrickHighlight::Neighbour3,
		EBrickHighlight::Neighbour4,
		EBrickHighlight::Neighbour5,
		EBrickHighlight::LoadComfortable,
		EBrickHighlight::LoadCaution,
		EBrickHighlight::LoadCritical,
	};

	TSet<int32> Seen;

	for (const EBrickHighlight State : AllStates)
	{
		bool bAlreadyThere = false;
		Seen.Add(static_cast<int32>(State), &bAlreadyThere);

		TestFalse(
			*FString::Printf(
				TEXT("THE THIRTEEN HIGHLIGHT STATES MUST BE THIRTEEN DIFFERENT VALUES. %s shares a "
					 "value with a state declared before it, so a brick in one is indistinguishable "
					 "from a brick in the other — and one of the pairs this would collapse is "
					 "'the overlay says this is fine' against 'you have picked this to delete'"),
				LoadOverlayHighlightName(State)),
			bAlreadyThere);
	}

	// Fail closed: an out-of-range band draws plain, not green.
	const EBrickHighlight Unknown = BrickHighlightForLoadBand(static_cast<EJointMarginBand>(200));

	TestEqual(
		FString::Printf(
			TEXT("FAIL CLOSED: a band this build has never heard of must leave the piece plain — a "
				 "green brick is a positive claim that it is orders of magnitude from failing. It "
				 "answered %s"),
			LoadOverlayHighlightName(Unknown)),
		static_cast<int32>(Unknown), static_cast<int32>(EBrickHighlight::None));

	return true;
}

/**
 * WorstJointBandForPiece checks support first: an unsupported piece is Critical whatever its joints
 * read, and a supported piece with no live joints is Comfortable (the first brick laid should not
 * turn red). Otherwise it buckets the highest utilisation over the piece's connections at 10 % and
 * 50 %, as FInspectorJointRow does, and fails closed to Critical. Check order: removed/unknown,
 * unsolved, unsupported, jointless, then joints.
 *
 * Worst, not mean or minimum: a brick fails at its first joint to fail. Units and thresholds are
 * transcribed, not imported (CLAUDE.md 100x rule). Do not add a horizontal normal without redoing
 * the arithmetic. World.Session.LoadOverlayTintsByWorstJoint checks agreement on a real wall. No world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLoadOverlayWorstJointBandTest,
	"DestructionGame.Core.LoadOverlay.WorstJointBandForPiece",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLoadOverlayWorstJointBandTest::RunTest(const FString& Parameters)
{
	using namespace LoadOverlayBandTestSupport;

	FLoadOverlayFixture Fixture;
	Fixture.Build();

	if (Fixture.Structure.NumConnections() != 5 || Fixture.Structure.NumPieces() != 9)
	{
		AddError(FString::Printf(
			TEXT("fixture: the stacks must build as nine pieces and five joints; they built %d and %d"),
			Fixture.Structure.NumPieces(), Fixture.Structure.NumConnections()));

		return true;
	}

	// Non-destructive, as the overlay is; never SolveAndBreak.
	Fixture.Structure.SolveLoads();

	// One: the fixture reads the designed percentages.

	struct FJointExpectation
	{
		int32 ConnectionIndex;
		double MassAboveKg;
		const TCHAR* What;
	};

	const FJointExpectation Joints[] = {
		{ 0, LoadOverlayComfortableMassKg, TEXT("the top joint of the three-high stack") },
		{ 1, LoadOverlayCriticalTopMassKg + LoadOverlayComfortableMassKg,
			TEXT("the bottom joint of the three-high stack") },
		{ 2, LoadOverlayCautionMassKg, TEXT("the Caution stack's bed joint") },
		{ 3, 0.0, TEXT("the massless topper's joint, which carries nothing at all") },
	};

	for (const FJointExpectation& Joint : Joints)
	{
		const double ExpectedPercent = LoadOverlayPercentForMassKg(Joint.MassAboveKg);
		const double ActualPercent = 100.0 * Fixture.Structure.GetConnectionUtilisation(Joint.ConnectionIndex);

		AddInfo(FString::Printf(
			TEXT("joint %d (%s): %g kg above it, expected %.4f %% of capacity, reads %.4f %% — band %s"),
			Joint.ConnectionIndex, Joint.What, Joint.MassAboveKg, ExpectedPercent, ActualPercent,
			LoadOverlayBandName(LoadOverlayBandForPercent(ExpectedPercent))));

		// Each joint must carry the weight above it; 1e-4 is far inside the bands' 2x clearance.
		TestTrue(
			*FString::Printf(
				TEXT("fixture: joint %d (%s) must carry the weight standing on it — %g kg x 980 over "
					 "1 MPa x 100 cm2 x 10,000 is %.4f %% of capacity; it reads %.4f %%"),
				Joint.ConnectionIndex, Joint.What, Joint.MassAboveKg, ExpectedPercent, ActualPercent),
			FMath::IsNearlyEqual(ActualPercent, ExpectedPercent, 1.0e-4));
	}

	/*
	 * Fixture: the support state the support-first rows rely on, so they cannot pass for the wrong
	 * reason (a pad with a joint, or a floating joint that is heavily loaded or given).
	 */
	{
		TestTrue(
			*FString::Printf(
				TEXT("fixture: the grounded pad (piece %d) must be SUPPORTED after the solve — it rests on "
					 "the earth, which is the whole reason it is not in trouble. IsPieceSupported reports "
					 "%d"),
				Fixture.Jointless, Fixture.Structure.IsPieceSupported(Fixture.Jointless) ? 1 : 0),
			Fixture.Structure.IsPieceSupported(Fixture.Jointless));

		int32 PadJoints = 0;

		for (int32 Index = 0; Index < Fixture.Structure.NumConnections(); ++Index)
		{
			const FConnection& Connection = Fixture.Structure.GetConnection(Index);

			PadJoints += Connection.PieceA == Fixture.Jointless || Connection.PieceB == Fixture.Jointless
				? 1 : 0;
		}

		TestEqual(
			FString::Printf(
				TEXT("fixture: and it must have NO joints at all, or the row below is reading a joint "
					 "rather than the earth; it has %d"),
				PadJoints),
			PadJoints, 0);

		TestFalse(
			*FString::Printf(
				TEXT("fixture: the floating pair's upper piece (%d) must NOT be supported — nothing is "
					 "under either of them, so neither has a path to the earth. IsPieceSupported reports "
					 "%d"),
				Fixture.FloatingTop, Fixture.Structure.IsPieceSupported(Fixture.FloatingTop) ? 1 : 0),
			Fixture.Structure.IsPieceSupported(Fixture.FloatingTop));

		TestFalse(
			*FString::Printf(
				TEXT("fixture: and neither must its lower piece (%d); IsPieceSupported reports %d"),
				Fixture.FloatingBase, Fixture.Structure.IsPieceSupported(Fixture.FloatingBase) ? 1 : 0),
			Fixture.Structure.IsPieceSupported(Fixture.FloatingBase));

		TestFalse(
			TEXT("fixture: and the joint between them must be INTACT — a given joint is skipped, which "
				 "would make the pair read Critical for the jointless reason rather than for want of "
				 "support"),
			Fixture.Structure.GetConnection(Fixture.FloatingJoint).HasGiven());

		const double FloatingPercent =
			100.0 * Fixture.Structure.GetConnectionUtilisation(Fixture.FloatingJoint);

		TestTrue(
			*FString::Printf(
				TEXT("fixture: AND THAT JOINT MUST READ COMFORTABLE ON ITS OWN — this row's whole subject "
					 "is a piece whose joints say it is fine and whose support says it is falling, so a "
					 "joint over the 10 %% edge would let the row pass for the joints' reason. It reads "
					 "%.4f %% of capacity, and the band it buckets to is %s"),
				FloatingPercent, LoadOverlayBandName(LoadOverlayBandForPercent(FloatingPercent))),
			LoadOverlayBandForPercent(FloatingPercent) == EJointMarginBand::Comfortable);

		// Every handle was solved, so nothing below lands in the unsolved arm.
		TestTrue(
			TEXT("fixture: the solve must have answered for the pad and for both floating pieces, or "
				 "they would read Critical for the unsolved reason instead"),
			Fixture.Structure.HasSupportAnswer(Fixture.Jointless)
				&& Fixture.Structure.HasSupportAnswer(Fixture.FloatingBase)
				&& Fixture.Structure.HasSupportAnswer(Fixture.FloatingTop));
	}

	// Two: a piece takes the worst of its joints.

	struct FPieceExpectation
	{
		int32 PieceIndex;
		EJointMarginBand Expected;
		const TCHAR* Why;
	};

	const FPieceExpectation Pieces[] = {
		{
			Fixture.ComfortableTop, EJointMarginBand::Comfortable,
			TEXT("its one joint carries 50 kg, which is 4.9 % of capacity — under the 10 % edge")
		},
		{
			Fixture.CriticalMiddle, EJointMarginBand::Critical,
			TEXT("THE DISCRIMINATING PIECE. Its joints are connection 0 at 4.9 % and connection 1 at "
				 "73.5 %, in that order, so first-wins, last-wins, the mean (39.2 %) and the minimum "
				 "all answer something other than Critical. A brick carrying three quarters of its "
				 "capacity drawn green is the exact lie this overlay exists to end")
		},
		{
			Fixture.GroundedBase, EJointMarginBand::Critical,
			TEXT("the base is an end of the same 73.5 % joint, and grounded is not a reason to be "
				 "excused from reading it")
		},
		{
			Fixture.CautionTop, EJointMarginBand::Caution,
			TEXT("250 kg is 24.5 % of capacity — between the two edges, and the band no settled wall "
				 "ever reaches on its own")
		},
		{
			Fixture.CautionBase, EJointMarginBand::Caution,
			TEXT("the other end of the same joint reads the same band")
		},
		{
			Fixture.Jointless, EJointMarginBand::Comfortable,
			TEXT("THE FIRST BRICK A PLAYER LAYS. It rests on the earth and touches nothing else, so the "
				 "earth is holding it up and there is nothing at all to report about how hard it is "
				 "working. Answering Critical here — the only fail-closed band available to a rule that "
				 "reads joints alone — paints the single brick on an empty plot bright red the moment it "
				 "lands, which is the overlay telling a player their first act was a mistake")
		},
		{
			Fixture.FloatingTop, EJointMarginBand::Critical,
			TEXT("THE OTHER DIRECTION, AND THE EXPENSIVE ONE. Nothing is holding this piece up — the "
				 "solve says so — and its one joint is intact and reads 4.9 % of capacity, so a rule "
				 "that banded from joints alone draws a brick in mid-air as the safest thing on the wall. "
				 "That is the ordinary shape of a wall coming down: the load path is severed and what is "
				 "left is an unloaded joint")
		},
		{
			Fixture.FloatingBase, EJointMarginBand::Critical,
			TEXT("and the other end of that same joint is in exactly as much trouble, for the same "
				 "reason — support is a property of the piece, not of the joint it is read through")
		},
	};

	for (const FPieceExpectation& Piece : Pieces)
	{
		const EJointMarginBand Actual = WorstJointBandForPiece(Fixture.Structure, Piece.PieceIndex);

		TestEqual(
			FString::Printf(
				TEXT("piece %d must read %s: %s. It reads %s"),
				Piece.PieceIndex, LoadOverlayBandName(Piece.Expected), Piece.Why,
				LoadOverlayBandName(Actual)),
			static_cast<int32>(Actual), static_cast<int32>(Piece.Expected));
	}

	// All three bands must actually be produced.
	TSet<int32> BandsSeen;

	for (const FPieceExpectation& Piece : Pieces)
	{
		BandsSeen.Add(static_cast<int32>(WorstJointBandForPiece(Fixture.Structure, Piece.PieceIndex)));
	}

	TestEqual(
		FString::Printf(
			TEXT("the fixture must reach all three bands for the bucketing to be pinned at all; it "
				 "reached %d"),
			BandsSeen.Num()),
		BandsSeen.Num(), 3);

	// Three: a survivor beside a hole keeps its own band.

	/*
	 * Unlike an inspector row, the overlay does not treat a given joint as Critical; otherwise every
	 * neighbour of a deleted brick turns red (an early implementation did this to three of four
	 * survivors). GetConnectionUtilisation returns zero for a given joint, so the max handles it. The
	 * topper is massless, so piece 4's live joint stays at 24.5 %.
	 */
	Fixture.Structure.RemovePiece(Fixture.MasslessTopper);
	Fixture.Structure.SolveLoads();

	TestTrue(
		TEXT("fixture: pulling the topper must actually give its joint — otherwise this section is "
			 "measuring nothing"),
		Fixture.Structure.GetConnection(3).HasGiven());

	{
		const double SurvivorPercent = 100.0 * Fixture.Structure.GetConnectionUtilisation(2);

		TestTrue(
			*FString::Printf(
				TEXT("fixture: and it must have moved NO load — piece 4's live joint read %.4f %% "
					 "before and reads %.4f %% now"),
				LoadOverlayPercentForMassKg(LoadOverlayCautionMassKg), SurvivorPercent),
			FMath::IsNearlyEqual(
				SurvivorPercent, LoadOverlayPercentForMassKg(LoadOverlayCautionMassKg), 1.0e-4));

		const EJointMarginBand Survivor =
			WorstJointBandForPiece(Fixture.Structure, Fixture.CautionTop);

		TestEqual(
			FString::Printf(
				TEXT("A PIECE BESIDE A HOLE IS NOT A PIECE IN TROUBLE. Piece %d has lost one of its two "
					 "joints and its live one still reads %.4f %% of capacity, so it must still read "
					 "Caution. Counting the given joint as Critical — which is the right answer for an "
					 "INSPECTOR ROW and the wrong one here — paints every neighbour of every deleted "
					 "brick red, and a wall that turns red where the player has already pulled reports "
					 "its own history instead of the load. It reads %s"),
				Fixture.CautionTop, SurvivorPercent, LoadOverlayBandName(Survivor)),
			static_cast<int32>(Survivor), static_cast<int32>(EJointMarginBand::Caution));
	}

	// Four: fail closed to Critical, the enum's zero.

	struct FDegenerateCase
	{
		int32 PieceIndex;
		const TCHAR* What;
	};

	Fixture.Structure.RemovePiece(Fixture.CautionTop);

	const FDegenerateCase Degenerate[] = {
		{ INDEX_NONE, TEXT("a handle naming no piece at all") },
		{ -7, TEXT("a negative handle") },
		{ 9999, TEXT("a handle past the end of the piece array") },
		{ Fixture.CautionTop, TEXT("a piece that has been removed; its joint went with it") },
	};

	for (const FDegenerateCase& Case : Degenerate)
	{
		const EJointMarginBand Actual = WorstJointBandForPiece(Fixture.Structure, Case.PieceIndex);

		TestEqual(
			FString::Printf(
				TEXT("FAIL CLOSED on %s: the answer must be Critical, which is EJointMarginBand's own "
					 "zero and the band that promises least. Over-promising is the expensive direction "
					 "for an instrument whose whole job is to say what is about to fall down. It "
					 "answered %s"),
				Case.What, LoadOverlayBandName(Actual)),
			static_cast<int32>(Actual), static_cast<int32>(EJointMarginBand::Critical));
	}

	// An unsolved structure is not Comfortable: utilisation reads zero before a solve.
	FLoadOverlayFixture Unsolved;
	Unsolved.Build();

	const EJointMarginBand UnsolvedBand =
		WorstJointBandForPiece(Unsolved.Structure, Unsolved.CriticalMiddle);

	TestEqual(
		FString::Printf(
			TEXT("A STRUCTURE NOTHING HAS SOLVED MUST NOT READ COMFORTABLE. Utilisation is zero before "
				 "a solve, so reading it straight would paint an untouched wall green from end to end "
				 "— 'no data' drawn as 'three orders of magnitude of headroom'. Piece %d of an unsolved "
				 "copy reads %s"),
			Unsolved.CriticalMiddle, LoadOverlayBandName(UnsolvedBand)),
		static_cast<int32>(UnsolvedBand), static_cast<int32>(EJointMarginBand::Critical));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
