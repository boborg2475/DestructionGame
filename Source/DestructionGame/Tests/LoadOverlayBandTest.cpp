// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Connection.h"
#include "Core/ConnectionStrength.h"
#include "Core/PieceMenu.h"
#include "Core/Structure.h"
#include "World/BrickActor.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges many
 * files into one. Every name here carries a LoadOverlayBand prefix. See CURRENT_STATE.md, where the
 * collisions this has already caused are recorded.
 */
namespace LoadOverlayBandTestSupport
{
	/**
	 * HOW MANY UNREAL FORCE UNITS ONE MEGAPASCAL BUYS OVER ONE SQUARE CENTIMETRE — SPELLED OUT HERE
	 * RATHER THAN IMPORTED.
	 *
	 * 1 N = 100 uu and 1 MPa = 1 N/mm2 = 100 N/cm2, so one MPa over one cm2 is 100 N = 10,000 uu.
	 * `DestructionForce::ForceUnitsPerMPaSqCm` is the production constant and this file deliberately
	 * does NOT use it: importing it would make every expectation below agree with that constant
	 * however wrong it is, and a conversion that is out by exactly 100x is the one this project's
	 * units rule exists for (CLAUDE.md, "the easiest way to be wrong by 100x").
	 */
	constexpr double LoadOverlayForceUnitsPerMPaSqCm = 10000.0;

	/**
	 * WHAT A KILOGRAM WEIGHS IN UNREAL FORCE UNITS. Unreal's gravity is -980 cm/s2 and mass is in
	 * kilograms, so a piece's weight is simply MassKg * 980 — which already IS the 1 N = 100 uu
	 * conversion (1 kg x 9.81 = 9.81 N = 981 uu, to the 980 the engine actually uses).
	 */
	constexpr double LoadOverlayWeightUuPerKg = 980.0;

	/**
	 * THE TWO EDGES OF THE THREE BANDS, TRANSCRIBED FROM THE DESIGN RATHER THAN READ OUT OF
	 * Core/PieceMenu.cpp.
	 *
	 * Ten per cent of capacity is 10x margin and fifty per cent is 2x, and `EJointMarginBand`'s own
	 * header states them that way round: "orders of magnitude from failing, which is where a settled
	 * wall lives", "getting close", "at or past its limit". `PresenterMarginBand` is file-local, so
	 * there is nothing to import even if importing were allowed — and it must not be, because the
	 * claim this file makes is that the OVERLAY buckets a piece with the same two numbers the
	 * INSPECTOR buckets a joint with. Reading the production constants would make an overlay that had
	 * grown its own thresholds agree with itself.
	 *
	 * AT AN EDGE THE PIECE TAKES THE WORSE BAND, which is the same rule the rows follow and is why
	 * the guards below run worst-first and are written negated: over-promising is the expensive
	 * direction for an instrument whose whole job is to say what is about to fall down, and every
	 * comparison against a NaN is false so a degenerate number lands in the FIRST arm.
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
	 * THE ONE STRENGTH EVERY JOINT IN THIS FILE CARRIES, AND ITS ONE PURPOSE IS THAT COMPRESSION
	 * GOVERNS.
	 *
	 * `ComputeUtilisation` returns the WORST AXIS, so a fixture aimed at compression that happened to
	 * utilise shear or tension harder would be measuring a different number than the one the expected
	 * values below are computed from — the exact trap DESIGN §4 names. Every load here is a piece's
	 * own weight through a bed joint whose normal is +Z, so the force resolves to pure compression
	 * and the other two axes see a demand of exactly ZERO:
	 *
	 *   - shear demand 0 against a capacity of cohesion + mu.sigma, truncated at MaxShear. Positive,
	 *     so the ratio is 0 rather than 0/0.
	 *   - tension demand 0 against f_t.A. Also positive, for the same reason: a tensile strength of
	 *     zero would make an unloaded joint read 0/0.
	 *
	 * The compressive strength is ONE MEGAPASCAL, not clay brick's real figure, and that is the point:
	 * this file is measuring the BANDING of a utilisation, not the strength of masonry, and a round
	 * capacity is what lets the expected percentages below be read off by hand. The real strengths are
	 * pinned where they belong, in `ConnectionStrengthTest`.
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

	/** THE JOINT AREA EVERY BED JOINT HERE HAS, in cm2. Round, so 1 MPa is exactly 1,000,000 uu. */
	constexpr double LoadOverlayJointAreaSqCm = 100.0;

	/**
	 * A BED JOINT BENEATH `Above`, DECLARED THE WAY THE SOLVER READS ONE.
	 *
	 * Per `ConnectionLoad.h` the interface normal points toward PieceB, so a normal of +Z with the
	 * supported piece as B is a bed joint UNDERNEATH it, bearing it. Getting that round the other way
	 * turns compression into tension, and mortar's tensile limit is a hundredth of its compressive
	 * one — which is why the convention is stated here rather than assumed.
	 *
	 * NO CENTRE AND NO RECTANGLE, DELIBERATELY. `HasCompleteGeometry` then reads false, the joint
	 * carries no moment and no composite depth, and the utilisation is the bare force over the bare
	 * bed patch — which is what makes the arithmetic below a hand calculation rather than a solve.
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
	 * WHAT A BED JOINT CARRYING THIS MUCH MASS READS, AS A PERCENTAGE OF ITS CAPACITY.
	 *
	 * Worked independently and in one line so nothing is hidden:
	 *
	 *     percent = 100 * (MassKg * 980) / (1 MPa * 100 cm2 * 10,000 uu-per-MPa-cm2)
	 *             = 100 * MassKg * 980 / 1,000,000
	 *
	 * so 50 kg reads 4.9 %, 250 kg reads 24.5 % and 750 kg reads 73.5 %. Those three sit 2.0x, 2.45x
	 * and 1.47x clear of the nearest band edge, so none of them is a fixture balanced on its own
	 * boundary.
	 */
	double LoadOverlayPercentForMassKg(double MassAboveKg)
	{
		const double DemandUu = MassAboveKg * LoadOverlayWeightUuPerKg;

		const double CapacityUu = LoadOverlayJointStrength().CompressiveStrengthMPa
			* LoadOverlayJointAreaSqCm
			* LoadOverlayForceUnitsPerMPaSqCm;

		return 100.0 * DemandUu / CapacityUu;
	}

	/*
	 * THE THREE MASSES, ONE PER BAND, AND THEY ARE CHOSEN RATHER THAN DISCOVERED. See
	 * LoadOverlayPercentForMassKg for the arithmetic; the comments are the answers.
	 */
	constexpr double LoadOverlayComfortableMassKg = 50.0;   /* 4.9 % — under the 10 % edge */
	constexpr double LoadOverlayCautionMassKg = 250.0;      /* 24.5 % — between the two edges */
	constexpr double LoadOverlayCriticalTopMassKg = 700.0;  /* with the 50 kg above it: 73.5 % */

	/**
	 * THE FIXTURE, AND ITS SHAPE IS THE WHOLE OF WHAT SECTION TWO IS FOR.
	 *
	 * A THREE-HIGH STACK AND A TWO-HIGH STACK, side by side and unconnected:
	 *
	 *     piece 2   50 kg      ---- joint 0 (added FIRST) ...... carries 50 kg,  4.9 %  Comfortable
	 *     piece 1  700 kg      ---- joint 1 (added SECOND) ..... carries 750 kg, 73.5 % Critical
	 *     piece 0  grounded
	 *
	 *     piece 5    0 kg      ---- joint 3 ..................... carries 0 kg,    0.0 %  Comfortable
	 *     piece 4  250 kg      ---- joint 2 ..................... carries 250 kg, 24.5 % Caution
	 *     piece 3  grounded
	 *
	 *     piece 6  grounded, no joints at all ..... THE FIRST BRICK A PLAYER LAYS
	 *
	 *     piece 8    50 kg     ---- joint 4 ..................... carries 0-50 kg, Comfortable
	 *     piece 7   700 kg     nothing at all beneath it: BOTH are in mid-air
	 *
	 * PIECE 6 IS A GROUNDED PAD AND PIECES 7/8 ARE A FLOATING PAIR, and between them they are the
	 * whole of the support-first rule. Piece 6 is added grounded, with no joint anywhere at all —
	 * the first brick a player lays on the earth, which is held up by the earth and has
	 * nothing to report. Pieces 7 and 8 are joined to each other and to nothing else, so neither has
	 * any path to the ground at all, and the ONE joint between them is intact and lightly loaded —
	 * which is exactly the case where reading joints alone answers "comfortable" about two bricks
	 * that are falling.
	 *
	 * THE MASSLESS TOPPER IS FOR SECTION THREE'S SURVIVOR-BESIDE-A-HOLE ROW, and it is massless so
	 * that pulling it changes NOTHING about the joint below — 250 kg either way, to the bit. That is
	 * what makes the row a clean reading of the given-joint rule rather than a reading of a load that
	 * moved: piece 4 keeps two joints, one of which has given, and its live one still reads 24.5 %.
	 *
	 * PIECE 1 IS THE DISCRIMINATING ONE AND THE ORDER OF THE TWO JOINTS IS DELIBERATE. Its joints are
	 * a Comfortable one at the LOWER connection index and a Critical one at the higher, so an
	 * implementation that took the first joint it found, or the last, or the mean, or the MINIMUM
	 * comes back Comfortable — a brick carrying three quarters of its capacity drawn as the safest
	 * thing on the wall. Only "the worst of them" answers Critical.
	 *
	 * AND ALL THREE BANDS ARE REACHED, which is what stops the sweep passing because everything is
	 * comfortable. A settled wall is comfortable everywhere, so a fixture built out of real bricks
	 * would have exercised exactly one arm of the bucketing.
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

		/** The joint between the two floating pieces — the last connection added, so index 4. */
		int32 FloatingJoint = INDEX_NONE;

		void Build()
		{
			GroundedBase = Structure.AddPiece(10.0, true);
			CriticalMiddle = Structure.AddPiece(LoadOverlayCriticalTopMassKg, false);
			ComfortableTop = Structure.AddPiece(LoadOverlayComfortableMassKg, false);

			/* The top joint FIRST, so the piece in the middle meets its comfortable joint first. */
			Structure.AddConnection(LoadOverlayBedJoint(CriticalMiddle, ComfortableTop));
			Structure.AddConnection(LoadOverlayBedJoint(GroundedBase, CriticalMiddle));

			CautionBase = Structure.AddPiece(10.0, true);
			CautionTop = Structure.AddPiece(LoadOverlayCautionMassKg, false);

			Structure.AddConnection(LoadOverlayBedJoint(CautionBase, CautionTop));

			/* Zero mass is allowed and meaningful — FStructure::AddPiece says so in as many words. */
			MasslessTopper = Structure.AddPiece(0.0, false);

			Structure.AddConnection(LoadOverlayBedJoint(CautionTop, MasslessTopper));

			Jointless = Structure.AddPiece(5.0, true);

			/*
			 * THE FLOATING PAIR, APPENDED LAST SO NO EXISTING PIECE OR CONNECTION INDEX MOVES.
			 * Neither is grounded and nothing stands under either of them, so the solver can find no
			 * path to the earth for either — while the joint between them stays intact and lightly
			 * worked, which is the whole point of the row it drives.
			 */
			FloatingBase = Structure.AddPiece(LoadOverlayCriticalTopMassKg, false);
			FloatingTop = Structure.AddPiece(LoadOverlayComfortableMassKg, false);

			FloatingJoint = Structure.AddConnection(LoadOverlayBedJoint(FloatingBase, FloatingTop));
		}
	};
}

/**
 * A MARGIN BAND IS A BRICK HIGHLIGHT, ONE PER BAND, AND NOTHING ELSE IN THE ENUM WEARS ONE.
 *
 * =====================================================================================
 * THE BEHAVIOUR IN ONE SENTENCE
 * =====================================================================================
 *
 * `BrickHighlightForLoadBand` maps each of `EJointMarginBand`'s three bands onto its own
 * `EBrickHighlight` — Comfortable to `LoadComfortable`, Caution to `LoadCaution`, Critical to
 * `LoadCritical` — and answers `None` for a band this build has never heard of.
 *
 * =====================================================================================
 * WHY THIS IS ONE FUNCTION AND NOT A SWITCH AT THE CALL SITE
 * =====================================================================================
 *
 * It is `BrickHighlightForNeighbourSlot`'s argument repeated for the other half of the overlay
 * vocabulary, and that function's header already gives it: the presenter has a band and the brick has
 * a material, and two tables between them is an overlay drawing amber for a joint the details window
 * beside it calls comfortable. SESSION_UI_DESIGN §a principle 6 states the same split from the other
 * end — "the colour of a thing is the model's decision; the hue is the widget's".
 *
 * =====================================================================================
 * WHAT IS ASSERTED, AND THE ROW THAT IS NOT A RESTATEMENT
 * =====================================================================================
 *
 * The three rows are the mapping. THE ROW WORTH HAVING IS THE DISTINCTNESS SWEEP: every load state
 * must differ from every other load state AND from every one of the ten states that already exist.
 * A mapping that answered `Selected` for Critical would pass any per-row check written as "Critical
 * gets a state that is not None", and would then make an overlaid brick indistinguishable from a
 * picked one — which is the one thing a player must be able to check before pressing Delete.
 *
 * AND THE FAIL-CLOSED ROW IS `None` RATHER THAN A PLAUSIBLE GREEN. `EJointMarginBand` is a uint8 and
 * a cast is all it takes to make a value nobody declared; green is a POSITIVE claim that a piece is
 * orders of magnitude from failing, and a piece with no answer must draw plain instead.
 *
 * NEEDS A TICKING WORLD: no, and not even a world — it is one free function over two enums.
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

	/*
	 * EVERY STATE THE ENUM HAS, so the distinctness claim below is against the WHOLE vocabulary
	 * rather than against the handful this test happened to think of. A new state added without a
	 * row here is an omission somebody has to make on purpose.
	 */
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

	/*
	 * FAIL CLOSED. Both of these enums are uint8 and a cast is all it takes; a piece with no answer
	 * must draw PLAIN rather than draw a plausible green.
	 */
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
 * A PIECE'S OVERLAY BAND IS ITS WORST JOINT'S BAND, BUCKETED WITH THE SAME TWO NUMBERS THE INSPECTOR
 * BUCKETS A JOINT WITH.
 *
 * =====================================================================================
 * THE BEHAVIOUR IN ONE SENTENCE
 * =====================================================================================
 *
 * `WorstJointBandForPiece(Structure, PieceIndex)` asks FIRST whether the last solve is holding the
 * piece up at all — an unsupported piece is `Critical` whatever its joints read, and a supported piece
 * with no live joints is `Comfortable` — and otherwise takes the HIGHEST utilisation over every
 * connection the piece is an end of, buckets it at 10 % and 50 % of capacity exactly as
 * `FInspectorJointRow`'s band is bucketed, and fails closed to `Critical` for a piece it cannot answer
 * for.
 *
 * =====================================================================================
 * SUPPORT IS ASKED BEFORE THE JOINTS ARE, AND BOTH HALVES OF THAT ARE NEW
 * =====================================================================================
 *
 * THE FIRST BRICK A PLAYER LAYS IS THE CASE THAT FORCED IT. It rests on the earth, it touches nothing,
 * and a rule that answered from its joints alone had to pick a band for a piece with no joints —
 * `Critical` being the only fail-closed choice, which paints the one brick on the plot bright red the
 * instant it lands. But that brick is not in trouble: the earth is holding it up, and there is simply
 * nothing to report about how hard it is working. `IsPieceSupported` is what tells those two apart, and
 * it costs nothing extra — the solve the overlay already runs writes the support array on its way past.
 *
 * AND THE EXPENSIVE HALF IS THE OTHER WAY ROUND: A PIECE WITH NOTHING HOLDING IT UP READS Critical
 * EVEN WHEN EVERY JOINT IT HAS IS COMFORTABLE. That is not a degenerate case, it is the ordinary shape
 * of a wall coming down — the load path is severed and what is left is an unloaded head joint reading
 * a fraction of a per cent, which is precisely a brick about to fall drawn as the safest thing on the
 * wall. The floating pair in the fixture is that shape in its smallest form: one intact joint, 4.9 %
 * of capacity, and no path to the ground for either end of it.
 *
 * THE ORDER OF THE FOUR QUESTIONS IS THEREFORE PART OF THE CLAIM: removed or unknown, then unsolved,
 * then unsupported, then jointless — and only what survives all four is banded by its joints.
 *
 * =====================================================================================
 * WHY THE WORST AND NOT THE MEAN, AND WHY THE ORDER OF THE JOINTS IS THE FIXTURE
 * =====================================================================================
 *
 * A brick fails at the joint that fails first, so a brick with one bed joint at three quarters of
 * capacity and one head joint carrying nothing is a three-quarters brick. The mean of those two is
 * 37 % — comfortably inside Caution — and the minimum is 4.9 %, which draws the most heavily worked
 * piece on the wall as the safest thing on it. Both are the kind of wrong answer that still looks
 * like a plausible number, so the fixture is built to tell them apart: piece 1's COMFORTABLE joint is
 * at the LOWER connection index, so first-wins, last-wins, mean and minimum all answer Comfortable
 * and only "the worst" answers Critical.
 *
 * =====================================================================================
 * WHERE THE EXPECTED NUMBERS COME FROM
 * =====================================================================================
 *
 * Every joint here is a bed joint of 100 cm2 at 1 MPa compressive, loaded by the weight of what
 * stands on it, so its capacity is 1 MPa x 100 cm2 x 10,000 = 1,000,000 uu and its demand is
 * MassKg x 980. The unit conversion is spelled out in this file rather than imported, per CLAUDE.md:
 * importing `ForceUnitsPerMPaSqCm` would make these expectations agree with that constant however
 * wrong it is, and being out by exactly 100x is the failure the rule exists for.
 *
 * COMPRESSION GOVERNS, AND THAT IS A CONSTRAINT RATHER THAN AN OBSERVATION. `ComputeUtilisation`
 * returns the worst axis, so a fixture aimed at compression whose shear happened to utilise more
 * would be silently measuring shear. Every force here is vertical through a +Z normal, which resolves
 * to pure compression with a shear and tension demand of exactly zero — and both of those axes are
 * given a non-zero capacity so that an unloaded axis reads 0 rather than 0/0. Do not give any joint
 * in this file a horizontal normal without redoing the arithmetic.
 *
 * =====================================================================================
 * AND THE THRESHOLDS ARE TRANSCRIBED, NOT IMPORTED
 * =====================================================================================
 *
 * `PresenterMarginBand`'s two constants are file-local to Core/PieceMenu.cpp, so there is nothing to
 * import even if importing were allowed — and it must not be, because the claim is that the OVERLAY
 * buckets with the same numbers the ROWS do. 10 % of capacity is 10x margin and 50 % is 2x, which is
 * what `EJointMarginBand`'s own three sentences say. `World.Session.LoadOverlayTintsByWorstJoint` is
 * where the agreement is checked against the inspector's own rows on a real wall; this is where the
 * numbers themselves are pinned.
 *
 * NEEDS A TICKING WORLD: no, and not even a world. A plain `FStructure`, one `SolveLoads`, and a
 * free function.
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

	/* NON-DESTRUCTIVE, WHICH IS WHAT THE OVERLAY IS ALLOWED TO DO. Never SolveAndBreak. */
	Fixture.Structure.SolveLoads();

	/* --- ONE: the fixture really does read the three percentages it was designed to ---------- */

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

		/*
		 * THE FIXTURE'S OWN FLOOR. Every band claim below is computed from the mass standing on a
		 * joint, so a solver that routed the load somewhere else entirely would leave this file
		 * asserting the bands of numbers nothing in the structure actually carries. One part in ten
		 * thousand is far tighter than the 2x clearance the bands have and far looser than anything
		 * double precision could drift by over three multiplications.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("fixture: joint %d (%s) must carry the weight standing on it — %g kg x 980 over "
					 "1 MPa x 100 cm2 x 10,000 is %.4f %% of capacity; it reads %.4f %%"),
				Joint.ConnectionIndex, Joint.What, Joint.MassAboveKg, ExpectedPercent, ActualPercent),
			FMath::IsNearlyEqual(ActualPercent, ExpectedPercent, 1.0e-4));
	}

	/* --- ONE AND A HALF: what the solve says is holding what up ------------------------------ */

	/*
	 * THE PRECONDITIONS FOR THE SUPPORT-FIRST ROWS, AND THEY ARE ASSERTED RATHER THAN ASSUMED.
	 *
	 * Two of the rows below claim something about a piece BECAUSE of its support state, and each would
	 * pass for the wrong reason if the fixture were not the shape it is described as: the grounded pad
	 * would read Comfortable anyway if it turned out to have a joint, and the floating pair would read
	 * Critical anyway if its joint turned out to be heavily worked or to have given. So the shape is
	 * pinned here, in the mechanism's own currency, before anything is claimed from it.
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

		/* Every handle in play was answered for, so nothing below lands in the unsolved arm. */
		TestTrue(
			TEXT("fixture: the solve must have answered for the pad and for both floating pieces, or "
				 "they would read Critical for the unsolved reason instead"),
			Fixture.Structure.HasSupportAnswer(Fixture.Jointless)
				&& Fixture.Structure.HasSupportAnswer(Fixture.FloatingBase)
				&& Fixture.Structure.HasSupportAnswer(Fixture.FloatingTop));
	}

	/* --- TWO: and the piece takes the WORST of the joints it is an end of -------------------- */

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

	/*
	 * ALL THREE BANDS WERE ACTUALLY PRODUCED. A bucketing that collapsed two bands into one would
	 * still satisfy some of the rows above; this is the row that says the fixture exercised the whole
	 * of the vocabulary rather than the comfortable end of it, which is all a settled wall ever
	 * reaches.
	 */
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

	/* --- THREE: A SURVIVOR BESIDE A HOLE KEEPS ITS OWN BAND ---------------------------------- */

	/*
	 * THE ONE PLACE THE OVERLAY'S RULE DIFFERS FROM THE INSPECTOR ROW'S, AND IT IS A PRODUCT
	 * DECISION RATHER THAN AN ARITHMETIC ONE.
	 *
	 * `PresenterMarginBand` draws a joint that has GIVEN as Critical, and for a ROW that is exactly
	 * right: the row is saying "this joint is gone", and the number beside it is zero for the same
	 * reason a hole in a wall is unloaded. Carried into a PIECE aggregate the same rule says
	 * something quite different — that every brick which used to touch a deleted one is in trouble —
	 * and a destruction game whose wall turns red wherever the player has already pulled is an
	 * instrument reporting its own history instead of the load.
	 *
	 * MEASURED, AND THAT IS WHY THIS ROW EXISTS: a throwaway implementation that copied the row's
	 * given-joint rule painted three of the four survivors of a single delete `LoadCritical` on the
	 * five-brick wall in `World.Session.LoadOverlayRefreshesAfterDelete`. The literal reading of the
	 * spec — the max of `GetConnectionUtilisation` over the piece's connections — does the right
	 * thing on its own, because that accessor answers ZERO for a joint that has given.
	 *
	 * THE TOPPER IS MASSLESS SO THAT PULLING IT MOVES NO LOAD. Piece 4's live bed joint reads 24.5 %
	 * before and 24.5 % after, to the bit, so the only thing that changed is that one of its two
	 * joints is now out of the graph — which is precisely the variable under test.
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

	/* --- FOUR: fail closed, and Critical is the enum's own fail-closed zero ------------------ */

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

	/*
	 * AND A STRUCTURE NOTHING HAS SOLVED IS NOT A COMFORTABLE ONE.
	 *
	 * `GetConnectionUtilisation` answers ZERO before anything has been solved — its header says so —
	 * so an overlay that simply read it would paint an untouched wall entirely green the moment it was
	 * switched on, and a full green bar beside "not solved yet" is already a logged defect in the
	 * details window. At wall scale it is worse: the whole structure would read as three orders of
	 * magnitude from failing, and nothing on screen would say the number was never computed.
	 */
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
