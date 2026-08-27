// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE INTERIM OVERTURNING GUARD'S DOCUMENTED BLIND SPOT — the in-flight RED that opens the
 * first PRODUCTION slice of DESIGN.md §7 evolution step 4 (PROMOTION_DESIGN.md §6 "Slice 2",
 * §9.4 "the first production slice"). NOT one of the six standing deliberate reds: it is a
 * behaviour red on production that persists through the oracle-relocation commit and goes green
 * when dev wires the equilibrium gate.
 *
 * WHAT THE GUARD CANNOT JUDGE, RESTATED FROM ITS OWN CONTRACT. FStructure::BreakOverturnedBodies
 * (DESIGN.md §5.7, Structure.cpp) is a SINGLE-BODY overturning check. It fires only for a bonded
 * body whose SOLE connection to the rest of the structure is one bed-joint bridge: it floods
 * outward from the body over every intact joint except the candidate bearing, and stands aside
 * the moment that flood reaches the bearing's seat OR the earth by any other route. Its own
 * comment says so — "everything with a second load path — walls, filled corbels, beams, spanned
 * holes — is deliberately outside its reach". A body held by TWO load paths therefore reads SAFE
 * today however far past its tipping point it is, because no per-joint number and no single-body
 * free-body check can express "this body, on these two seats, has no admissible equilibrium".
 * The equilibrium LP — which reasons about the whole structure's admissible force system — CAN:
 * it finds no force system in equilibrium with self-weight that violates no strength constraint,
 * so lambda* < 1 and the body falls. That gap is what licenses relocating the oracle out of
 * Tests/ (Slice 1) so the gate (Slice 2) can call it in production.
 *
 * THE FIXTURE — ONE TOPOLOGY, THE SIMPLEST THAT EXHIBITS THE BLIND SPOT. A single heavy
 * overhanging body resting on TWO grounded seats, both of them on the SAME side of the body's
 * centre of mass, so the body races out past both bearings:
 *
 *      body centroid (X = 147.5)  ---------------------------------------+
 *      +------------------------------------------------------------------+   <- one rigid body,
 *      |  [S2] [S1]                                                       |      300 x 40 x 10.25
 *      +--#----#----------------------------------------------------------+      overhang -->
 *         ||   ||
 *      ===||===||=====  the earth        S2 anchor at X=0 (w=5), S1 pivot at X=10 (w=10)
 *
 * TWO LOAD PATHS. The body has two bed joints beneath it, one to each grounded seat. The guard,
 * asked about either bearing, floods from the body and reaches the OTHER grounded seat in one
 * hop — a second path around the bearing under test — so it stands aside for BOTH. This is
 * exactly the abort the guard documents; no fixture in the suite exercises it until this one.
 *
 * WHY IT IS PAST TIPPING WITH NO EQUILIBRIUM. Both seats sit to the left of the centroid, so
 * gravity rotates the body clockwise about the rightmost bearing edge (the right edge of the
 * pivot S1, at X = 15). Everything left of that fulcrum LIFTS, and only the mortar BOND can hold
 * it down. The overturning moment about the fulcrum is W * (X_centroid - X_fulcrum); the most a
 * bonded joint can ever restore is the fully-plastic tension block, every left contact point at
 * f_t over its tributary half-area, times its lever from the fulcrum. Worked below, the
 * overturning outruns even that most-charitable plastic bond by ~3.4x — so there is no
 * admissible force system at self-weight and the body must come down. (Height in Z is irrelevant
 * to overturning under vertical gravity; only the horizontal lever and the weight matter, which
 * is why the body is made THICK rather than TALL to buy margin without a taller lever arm.)
 *
 * WHAT PRODUCTION READS TODAY (the red). The body has TWO supports (N = 2), so the N >= 2 rule
 * (DESIGN.md §5.3) keeps the area split and ZEROES the moment — "unconservative otherwise, and
 * recorded as such". Each bearing then reads pure split compression, a few percent of mortar
 * crushing, nothing breaks, and the guard is blind. The body reads Supported; SolveAndBreak runs
 * ZERO breaking passes; nothing loses the earth. That is the wrong answer this red pins.
 *
 * THE CROSS-CHECK THAT MAKES THE RED GREENABLE. The RigidBlockOracle (test support today,
 * relocating in Slice 1) is asked the SAME structure through BuildRigidBlockProblem and must
 * report it INFEASIBLE at self-weight (Falls, lambda* < 1). If the oracle stood it, the Slice 2
 * gate could never green this red and the fixture would be wrong — so the oracle verdict is
 * asserted here as a precondition, not merely reported. Production stands / oracle falls is the
 * whole licence.
 *
 * ASSERTIONS, per DESIGN.md §4. This is an OUTCOME test: the overhanging body must lose its path
 * to the earth (Falling or Stranded), and the two seats must keep theirs, with zero pieces
 * Stranded so a routing limitation cannot wear the collapse's clothes. Displacement is never
 * read. No assertion names a specific joint's HasGiven: how the gate expresses "this body has no
 * equilibrium" is implementation the outcome must not dictate.
 *
 * NOTHING IS IMPORTED FROM THE CODE UNDER TEST EXCEPT THE PRODUCER (MakeInterface) and the
 * mortar profile the seats are bonded with. The statics, the section, the unit conversion and
 * the plastic-bond bound are derived in this file, so a wrong production constant DISAGREES with
 * it rather than agreeing. The oracle likewise derives its own conversion (its header's rule).
 *
 * NEEDS A TICKING WORLD: NO. Gravity is on (weight is mass x 980), everything is connected, and
 * every assertion is on solver state, outcome, or the oracle. Same footing as the leaning-stack
 * and beam acceptance tests.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges many files into one translation unit.
 */
namespace TwoLoadPathOverturningTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/* ================================================================================
	 * THE GEOMETRY. Every length is centimetres, at Unreal's default 1 uu = 1 cm.
	 * ================================================================================ */

	/** Fired clay, 1.9 g/cm3 — the same figure every wall fixture uses. */
	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** The single wythe: the body and both seats are this wide on Y, so every joint's Y overlap is full. */
	constexpr double WytheWidthCm = 10.25;

	/** A 1 cm mortar bed under the body: the separation the two bed joints are formed across. */
	constexpr double BedJointThicknessCm = 1.0;

	/* --- The two grounded seats: both to the LEFT of the body's centroid. --- */

	/** Grounded from Z = 0 to this; the body's bed joints form 1 cm above their tops. */
	constexpr double SeatHeightCm = 20.0;

	/** The ANCHOR: the leftmost seat, whose bond must hold the lifting side down. */
	constexpr double AnchorCentreXCm = 0.0;
	constexpr double AnchorWidthXCm = 5.0;

	/** The PIVOT: the rightmost seat; the body tips clockwise about its right edge. */
	constexpr double PivotCentreXCm = 10.0;
	constexpr double PivotWidthXCm = 10.0;

	/** The right edge of the pivot bearing — the fulcrum the whole free body rotates about. */
	constexpr double FulcrumXCm = PivotCentreXCm + PivotWidthXCm / 2.0;

	/* --- The overhanging body: one rigid piece, made THICK (not tall) to buy margin. --- */

	/** Flush with the anchor's left edge, so both seats sit within the footprint. */
	constexpr double BodyLeftXCm = AnchorCentreXCm - AnchorWidthXCm / 2.0;
	constexpr double BodyLengthXCm = 300.0;
	constexpr double BodyRightXCm = BodyLeftXCm + BodyLengthXCm;
	constexpr double BodyCentreXCm = (BodyLeftXCm + BodyRightXCm) / 2.0;

	/** Thickness on Z. Weight scales with it and so does the overturning; the lever does not. */
	constexpr double BodyThicknessZCm = 40.0;

	/** The body floats one bed above the seat tops. */
	constexpr double BodyBottomZCm = SeatHeightCm + BedJointThicknessCm;
	constexpr double BodyCentreZCm = BodyBottomZCm + BodyThicknessZCm / 2.0;

	/* ================================================================================
	 * UNITS AND STRENGTH BASIS. Every figure cited; none imported from production.
	 * ================================================================================ */

	/** MassKg * 980 IS a weight in uu — the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/**
	 * 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa over 1 cm2 is 10000 uu. DELIBERATELY NOT
	 * DestructionForce::ForceUnitsPerMPaSqCm nor the oracle's own copy: this file has to FAIL
	 * if that constant is wrong rather than agree with it.
	 */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	/* ================================================================================
	 * THE INDEPENDENT ORACLE (statics), used to CHOOSE the geometry and to assert the
	 * fixture is genuinely past tipping. None of it mirrors production's routing: it is
	 * moments of one rigid body about one fulcrum against the most generous plastic bond.
	 * The RigidBlockOracle LP is the SECOND, independently derived confirmation, called
	 * against the built structure in the test below.
	 * ================================================================================ */

	constexpr double BodyMassKg =
		ClayDensityGramsPerCubicCm * BodyLengthXCm * WytheWidthCm * BodyThicknessZCm / 1000.0;

	constexpr double BodyWeightUu = BodyMassKg * GravityCmPerSecondSquared;

	/** The two bed joints' true face areas: the seat width across the full wythe. */
	constexpr double AnchorAreaSqCm = AnchorWidthXCm * WytheWidthCm;
	constexpr double PivotAreaSqCm = PivotWidthXCm * WytheWidthCm;

	/**
	 * Overturning moment about the fulcrum (uu.cm): the whole weight acts at the centroid,
	 * a horizontal lever (X_centroid - X_fulcrum) out past the rightmost bearing edge.
	 */
	double OverturningMomentUuCm()
	{
		return BodyWeightUu * (BodyCentreXCm - FulcrumXCm);
	}

	/**
	 * The MOST CHARITABLE restoring moment a bonded joint can ever offer: the fully-plastic
	 * tension block. Each joint is discretised (exactly as the rigid-block oracle discretises
	 * it) into two contact points at the ends of its in-plane segment; every point LEFT of the
	 * fulcrum may pull down at f_t over its tributary half-area, times its lever from the
	 * fulcrum. The pivot's right contact sits ON the fulcrum (zero lever, and it is the
	 * compression fulcrum anyway), so it restores nothing. This reads UP TO 3x the uncracked
	 * first-crack capacity, so a body this bound overturns cannot be argued back up.
	 */
	double MaxPlasticRestoringMomentUuCm(double BondMPa)
	{
		const double AnchorPerContactUu = BondMPa * ForceUnitsPerMPaSqCmHere * (AnchorAreaSqCm / 2.0);
		const double PivotPerContactUu = BondMPa * ForceUnitsPerMPaSqCmHere * (PivotAreaSqCm / 2.0);

		const double AnchorLeftLeverCm = FulcrumXCm - (AnchorCentreXCm - AnchorWidthXCm / 2.0);
		const double AnchorRightLeverCm = FulcrumXCm - (AnchorCentreXCm + AnchorWidthXCm / 2.0);
		const double PivotLeftLeverCm = FulcrumXCm - (PivotCentreXCm - PivotWidthXCm / 2.0);

		return AnchorPerContactUu * AnchorLeftLeverCm
			+ AnchorPerContactUu * AnchorRightLeverCm
			+ PivotPerContactUu * PivotLeftLeverCm;
	}

	/* ================================================================================
	 * THE FIXTURE.
	 * ================================================================================ */

	struct FTwoPathBody
	{
		FStructure Structure;

		int32 Anchor = INDEX_NONE;
		int32 Pivot = INDEX_NONE;
		int32 Body = INDEX_NONE;

		int32 AnchorJoint = INDEX_NONE;
		int32 PivotJoint = INDEX_NONE;
	};

	FPieceBox MakeBox(double CentreX, double WidthX, double CentreZ, double ThicknessZ)
	{
		FPieceBox Box;
		Box.ExtentCm = FVector(WidthX, WytheWidthCm, ThicknessZ) * 0.5;
		Box.CentreCm = FVector(CentreX, 0.0, CentreZ);
		return Box;
	}

	double BoxMassKg(const FPieceBox& Box)
	{
		return ClayDensityGramsPerCubicCm
			* (Box.ExtentCm.X * 2.0) * (Box.ExtentCm.Y * 2.0) * (Box.ExtentCm.Z * 2.0) / 1000.0;
	}

	/** Lay the two grounded seats and the one overhanging body; join only the two body-seat pairs. */
	void Build(FTwoPathBody& Out)
	{
		const FPieceBox AnchorBox = MakeBox(AnchorCentreXCm, AnchorWidthXCm, SeatHeightCm / 2.0, SeatHeightCm);
		const FPieceBox PivotBox = MakeBox(PivotCentreXCm, PivotWidthXCm, SeatHeightCm / 2.0, SeatHeightCm);
		const FPieceBox BodyBox = MakeBox(BodyCentreXCm, BodyLengthXCm, BodyCentreZCm, BodyThicknessZCm);

		Out.Anchor = Out.Structure.AddPiece(BoxMassKg(AnchorBox), /*bIsGrounded*/ true, AnchorBox.CentreCm);
		Out.Pivot = Out.Structure.AddPiece(BoxMassKg(PivotBox), /*bIsGrounded*/ true, PivotBox.CentreCm);
		Out.Body = Out.Structure.AddPiece(BoxMassKg(BodyBox), /*bIsGrounded*/ false, BodyBox.CentreCm);

		FConnection Joint;

		if (MakeInterface(Out.Anchor, AnchorBox, Out.Body, BodyBox,
				BedJointThicknessCm, GeneralPurposeMortar, Joint))
		{
			Out.AnchorJoint = Out.Structure.AddConnection(Joint);
		}

		if (MakeInterface(Out.Pivot, PivotBox, Out.Body, BodyBox,
				BedJointThicknessCm, GeneralPurposeMortar, Joint))
		{
			Out.PivotJoint = Out.Structure.AddConnection(Joint);
		}
	}

	/** Which live pieces have lost their path to the earth. Stranded counts as fallen. */
	TArray<int32> FallenPieces(const FTwoPathBody& Fixture)
	{
		TArray<int32> Fallen;

		for (int32 Piece = 0; Piece < Fixture.Structure.NumPieces(); ++Piece)
		{
			if (Fixture.Structure.IsPieceRemoved(Piece))
			{
				continue;
			}

			const EPieceSupport Support = Fixture.Structure.GetPieceSupport(Piece);

			if (Support != EPieceSupport::Grounded && Support != EPieceSupport::Supported)
			{
				Fallen.Add(Piece);
			}
		}

		return Fallen;
	}

	int32 StrandedCount(const FTwoPathBody& Fixture)
	{
		int32 Stranded = 0;

		for (int32 Piece = 0; Piece < Fixture.Structure.NumPieces(); ++Piece)
		{
			if (!Fixture.Structure.IsPieceRemoved(Piece)
				&& Fixture.Structure.GetPieceSupport(Piece) == EPieceSupport::Stranded)
			{
				++Stranded;
			}
		}

		return Stranded;
	}
}

/**
 * A BODY ON TWO LOAD PATHS, PAST ITS TIPPING POINT, MUST FALL.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTwoLoadPathOverturningTest,
	"DestructionGame.Acceptance.Overturning.ABodyOnTwoLoadPathsPastTippingMustFall",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FTwoLoadPathOverturningTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace TwoLoadPathOverturningTestSupport;

	/* ------------------------------------------------------------------ *
	 * PRECONDITIONS ON THE STRENGTH BASIS — the verdict turns on these.
	 * ------------------------------------------------------------------ */

	TestEqual(TEXT("FIXTURE: the seats are bonded with the mean-basis 0.70 flexural bond"),
		GeneralPurposeMortar.TensileStrengthMPa, 0.7);

	TestEqual(TEXT("FIXTURE: the mortar's coded crushing strength is M10's 10 MPa"),
		GeneralPurposeMortar.CompressiveStrengthMPa, 10.0);

	/* ------------------------------------------------------------------ *
	 * PRECONDITION: THE FIXTURE IS GENUINELY PAST TIPPING. Overturning must
	 * outrun even the most charitable plastic bond, or it is not a fall.
	 * ------------------------------------------------------------------ */

	const double OverturningUuCm = OverturningMomentUuCm();
	const double RestoringUuCm = MaxPlasticRestoringMomentUuCm(GeneralPurposeMortar.TensileStrengthMPa);
	const double OverturningRatio = OverturningUuCm / RestoringUuCm;

	AddInfo(FString::Printf(
		TEXT("DERIVED: body weight %.10g uu, centroid X %.10g, fulcrum X %.10g; overturning %.10g "
			 "uu.cm against the MOST plastic restoring %.10g uu.cm at f_t = %g MPa => ratio %.10g (>1 falls)"),
		BodyWeightUu, BodyCentreXCm, FulcrumXCm, OverturningUuCm, RestoringUuCm,
		GeneralPurposeMortar.TensileStrengthMPa, OverturningRatio));

	TestTrue(
		*FString::Printf(
			TEXT("FIXTURE: the body must be past tipping under the MOST charitable plastic bond by a "
				 "clear margin; overturning/restoring is %.10g and must exceed 2"),
			OverturningRatio),
		OverturningRatio > 2.0);

	/* ------------------------------------------------------------------ *
	 * BUILD, AND CHECK THE TOPOLOGY IS THE ONE CLAIMED: three pieces, two bed
	 * joints beneath the body, complete geometry — the guard's blind spot.
	 * ------------------------------------------------------------------ */

	FTwoPathBody Fixture;
	Build(Fixture);

	if (Fixture.AnchorJoint == INDEX_NONE || Fixture.PivotJoint == INDEX_NONE)
	{
		AddError(TEXT("FIXTURE: the producer must emit both body-seat bed joints"));
		return false;
	}

	TestEqual(TEXT("FIXTURE: three pieces — two seats and one body"),
		Fixture.Structure.NumPieces(), 3);

	TestEqual(TEXT("FIXTURE: exactly two joints — the two load paths, and no seat-seat joint"),
		Fixture.Structure.NumConnections(), 2);

	TestTrue(TEXT("FIXTURE: every piece and joint must know where it is, or there is no eccentricity"),
		Fixture.Structure.HasCompleteGeometry());

	TestEqual(TEXT("FIXTURE: the anchor bed joint is the seat's face across the full wythe"),
		Fixture.Structure.GetConnection(Fixture.AnchorJoint).InterfaceAreaSqCm, AnchorAreaSqCm);

	TestEqual(TEXT("FIXTURE: the pivot bed joint is the seat's face across the full wythe"),
		Fixture.Structure.GetConnection(Fixture.PivotJoint).InterfaceAreaSqCm, PivotAreaSqCm);

	TestTrue(TEXT("FIXTURE: the anchor joint BEARS the body (a bed joint beneath it) — two load paths, not shear"),
		Fixture.Structure.GetJointRole(Fixture.AnchorJoint, Fixture.Body) == EJointRole::BedBeneath);

	TestTrue(TEXT("FIXTURE: the pivot joint BEARS the body (a bed joint beneath it) — the guard's blind spot"),
		Fixture.Structure.GetJointRole(Fixture.PivotJoint, Fixture.Body) == EJointRole::BedBeneath);

	/* ------------------------------------------------------------------ *
	 * THE CROSS-CHECK THAT LICENSES THE GATE: the RigidBlockOracle, asked the
	 * SAME structure, must find NO admissible equilibrium at self-weight.
	 * If it stood the body, the Slice 2 gate could never green this red.
	 * ------------------------------------------------------------------ */

	RigidBlockOracle::FOracleProblem Problem;
	FString BridgeWhy;

	const bool bBridged = RigidBlockOracle::BuildRigidBlockProblem(Fixture.Structure, Problem, BridgeWhy);

	TestTrue(
		*FString::Printf(TEXT("CROSS-CHECK: the oracle bridge must accept this 2D structure (%s)"), *BridgeWhy),
		bBridged);

	if (bBridged)
	{
		const RigidBlockOracle::FOracleResult Oracle = RigidBlockOracle::SolveRigidBlock(Problem);
		const RigidBlockOracle::EOracleOutcome Outcome = RigidBlockOracle::OutcomeOf(Oracle);

		AddInfo(FString::Printf(
			TEXT("CROSS-CHECK: oracle answered %d, lambda* %.10g, %d pivots — Falls means lambda* < 1"),
			Oracle.bAnswered ? 1 : 0, Oracle.Lambda, Oracle.SimplexIterations));

		TestTrue(TEXT("CROSS-CHECK: the oracle must ANSWER this fixture (a refusal cannot license the gate)"),
			Oracle.bAnswered);

		TestEqual(
			TEXT("CROSS-CHECK: the oracle must find NO equilibrium at self-weight (Falls) — this is what "
				 "licenses the Slice 2 gate to bring the body down"),
			static_cast<int32>(Outcome), static_cast<int32>(RigidBlockOracle::EOracleOutcome::Falls));

		TestTrue(
			*FString::Printf(TEXT("CROSS-CHECK: lambda* %.10g must sit clearly below 1"), Oracle.Lambda),
			Oracle.bAnswered && Oracle.Lambda < 0.9);
	}

	/* ------------------------------------------------------------------ *
	 * THE STATE AS BUILT, AND THEN AFTER THE CASCADE. Solve once
	 * non-destructively so the today-verdict is visible, then cascade.
	 * ------------------------------------------------------------------ */

	Fixture.Structure.SolveLoads();

	const EPieceSupport BodyBefore = Fixture.Structure.GetPieceSupport(Fixture.Body);

	const int32 Passes = Fixture.Structure.SolveAndBreak();

	const TArray<int32> Fallen = FallenPieces(Fixture);
	const int32 Stranded = StrandedCount(Fixture);

	AddInfo(FString::Printf(
		TEXT("PRODUCTION today: body support-as-built %d (2=Supported); cascade ran %d breaking pass(es); "
			 "%d piece(s) fell; %d stranded"),
		static_cast<int32>(BodyBefore), Passes, Fallen.Num(), Stranded));

	/* ------------------------------------------------------------------ *
	 * PRECONDITION: no piece may be Stranded, so a collapse verdict cannot be
	 * a routing limitation wearing a collapse's clothes.
	 * ------------------------------------------------------------------ */

	TestEqual(
		TEXT("PRECONDITION: no piece may be Stranded — a verdict decided by the solver declining to route "
			 "is not a verdict about a leaning body"),
		Stranded, 0);

	/* ------------------------------------------------------------------ *
	 * THE RED. The body has no admissible equilibrium at self-weight, so it must
	 * lose the earth. Production stands it today — N >= 2 zeroes the moment and the
	 * interim guard cannot judge a two-load-path body — which is DESIGN.md §7 gap 1
	 * in one fixture, and the behaviour Slice 2's gate exists to fix.
	 * ------------------------------------------------------------------ */

	TestTrue(
		*FString::Printf(
			TEXT("RED: a body past its tipping point on two load paths must lose the earth; its support "
				 "reads %d after the cascade (want NOT Grounded/Supported), and the cascade ran %d pass(es). "
				 "Today production stands it: N>=2 zeroes the moment and BreakOverturnedBodies is documented "
				 "to exclude everything with a second load path"),
			static_cast<int32>(Fixture.Structure.GetPieceSupport(Fixture.Body)), Passes),
		Fallen.Contains(Fixture.Body));

	TestTrue(TEXT("RED: the two grounded seats are the earth and must keep it — only the body falls"),
		Fixture.Structure.GetPieceSupport(Fixture.Anchor) == EPieceSupport::Grounded
			&& Fixture.Structure.GetPieceSupport(Fixture.Pivot) == EPieceSupport::Grounded);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
