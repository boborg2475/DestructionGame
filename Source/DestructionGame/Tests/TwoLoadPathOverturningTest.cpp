// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"
#include "Core/StructureBinding.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE INTERIM OVERTURNING GUARD'S DOCUMENTED BLIND SPOT — the in-flight RED that opens the
 * first PRODUCTION slice of DESIGN.md §7 evolution step 4 (PROMOTION_DESIGN.md §6 "Slice 2",
 * §9.4 "the first production slice"). It was a behaviour red on production that WENT GREEN
 * 2026-08-27 when Slice 2 wired the equilibrium gate; it now stands as the regression test that
 * the gate fells a two-load-path body. Never one of the six standing deliberate reds.
 *
 * WHAT THE GUARD COULD NOT JUDGE (historical — the guard is deleted). The interim
 * FStructure::BreakOverturnedBodies (removed in Slice 2; DESIGN.md §5.7) was a SINGLE-BODY
 * overturning check. It fired only for a bonded body whose SOLE connection to the rest of the
 * structure was one bed-joint bridge: it flooded
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

	/* ================================================================================
	 * THE SAME FIXTURE, BUILT THROUGH THE PLAYER-FACING DOOR. FStructureBinding owns its
	 * FStructure privately and hands out only a const reference, so the scope-by-size test
	 * drives RemovePiece/SolveAndBreak/ApplyResults exactly as the acceptance tests do and
	 * reads results off Binding.GetStructure(). No removal is needed — as with the dry-stack
	 * acceptance fixture, the action is simply settling under gravity. Geometry, mass and the
	 * mortar bond are the SAME constants the FStructure fixture above uses; nothing is copied.
	 * ================================================================================ */

	struct FTwoPathBinding
	{
		FStructureBinding Binding;

		int32 Anchor = INDEX_NONE;
		int32 Pivot = INDEX_NONE;
		int32 Body = INDEX_NONE;

		int32 AnchorJoint = INDEX_NONE;
		int32 PivotJoint = INDEX_NONE;
	};

	void BuildBinding(FTwoPathBinding& Out)
	{
		const FPieceBox AnchorBox = MakeBox(AnchorCentreXCm, AnchorWidthXCm, SeatHeightCm / 2.0, SeatHeightCm);
		const FPieceBox PivotBox = MakeBox(PivotCentreXCm, PivotWidthXCm, SeatHeightCm / 2.0, SeatHeightCm);
		const FPieceBox BodyBox = MakeBox(BodyCentreXCm, BodyLengthXCm, BodyCentreZCm, BodyThicknessZCm);

		Out.Anchor = Out.Binding.AddPiece(BoxMassKg(AnchorBox), /*bIsGrounded*/ true, /*Actor*/ nullptr, AnchorBox);
		Out.Pivot = Out.Binding.AddPiece(BoxMassKg(PivotBox), /*bIsGrounded*/ true, /*Actor*/ nullptr, PivotBox);
		Out.Body = Out.Binding.AddPiece(BoxMassKg(BodyBox), /*bIsGrounded*/ false, /*Actor*/ nullptr, BodyBox);

		FConnection Joint;

		if (MakeInterface(Out.Anchor, AnchorBox, Out.Body, BodyBox,
				BedJointThicknessCm, GeneralPurposeMortar, Joint))
		{
			Out.AnchorJoint = Out.Binding.AddConnection(Joint);
		}

		if (MakeInterface(Out.Pivot, PivotBox, Out.Body, BodyBox,
				BedJointThicknessCm, GeneralPurposeMortar, Joint))
		{
			Out.PivotJoint = Out.Binding.AddConnection(Joint);
		}
	}

	/** True when a live piece has lost every path to the earth — the outcome a caught body shows. */
	bool HasLostTheEarth(const FStructure& S, int32 Piece)
	{
		if (S.IsPieceRemoved(Piece))
		{
			return false;
		}

		const EPieceSupport Support = S.GetPieceSupport(Piece);
		return Support != EPieceSupport::Grounded && Support != EPieceSupport::Supported;
	}

	int32 StrandedCount(const FStructure& S)
	{
		int32 Stranded = 0;

		for (int32 Piece = 0; Piece < S.NumPieces(); ++Piece)
		{
			if (!S.IsPieceRemoved(Piece) && S.GetPieceSupport(Piece) == EPieceSupport::Stranded)
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
			TEXT("REGRESSION: a body past its tipping point on two load paths must lose the earth; its "
				 "support reads %d after the cascade (want NOT Grounded/Supported), and the cascade ran %d "
				 "pass(es). The equilibrium gate fells it (LP infeasible at self-weight); the interim guard "
				 "it replaced could not, because N>=2 zeroed the moment and it excluded any second load path"),
			static_cast<int32>(Fixture.Structure.GetPieceSupport(Fixture.Body)), Passes),
		Fallen.Contains(Fixture.Body));

	TestTrue(TEXT("RED: the two grounded seats are the earth and must keep it — only the body falls"),
		Fixture.Structure.GetPieceSupport(Fixture.Anchor) == EPieceSupport::Grounded
			&& Fixture.Structure.GetPieceSupport(Fixture.Pivot) == EPieceSupport::Grounded);

	return true;
}

/**
 * THE EQUILIBRIUM GATE IS SCOPED BY A BLOCK CAP — the fail-closed seam Slice 2 must not drift.
 *
 * WHAT THIS PINS, AND WHY NEITHER EXISTING RED TOUCHES IT. The two-load-path red above proves
 * the gate must CATCH a body the LP falls. This proves the OTHER half of D6-c: the gate's
 * authority is bounded by structure size — authoritative at or below a block cap, and above it
 * it DECLINES, falling through to the router (the joint sweep, with no overturning check), which
 * is exactly today's behaviour. Without this pin a dev could wire a gate that runs unconditionally
 * and the >cap fail-closed boundary — the thing that keeps synchronous LP authority off the
 * flagship scenarios (§12 D2⁗) — would be untested and silently drift.
 *
 * HOW IT IS MADE TESTABLE WITHOUT AN 84-BLOCK FIXTURE. The cap exists precisely to avoid solving
 * large structures, so a fixture at the real ~84-104-block band would defeat the purpose. Instead
 * the cap is an INJECTABLE seam (SetEquilibriumGateBlockCap) and this test drives the SAME
 * 3-piece two-load-path body the LP falls, twice, changing ONLY the cap:
 *   - cap = 8  (>= the 3-piece block count): the gate is authoritative, so it must CATCH the
 *              body — the body loses the earth and ApplyResults releases it. This does NOT happen
 *              today (no gate exists), so THIS ARM IS THE RED that drives dev.
 *   - cap = 2  (<  the 3-piece block count): the gate DECLINES; behaviour falls through to the
 *              router with no overturning check, so the body is NOT caught — it stands, exactly as
 *              production does today. This arm passes today and GUARDS the seam once the gate
 *              exists: a capless gate would wrongly catch the body here and fail this arm.
 *
 * THE BLOCK-COUNT CONTRACT dev must implement to: the cap is compared against the structure's
 * LIVE BLOCK COUNT (NumPieces, pinned to 3 below). Authoritative when count <= cap; decline when
 * count > cap. If dev prefers to count only non-grounded blocks, the caps here must be re-derived
 * so this fixture is unambiguously over-cap on one arm and under-cap on the other — the test is
 * the spec, so state the change against it rather than around it.
 *
 * DRIVEN THROUGH THE PLAYER-FACING DOOR — FStructureBinding SolveAndBreak + ApplyResults, reading
 * results off GetStructure() — so "caught" is an actual release (what the player sees the brick
 * do), not just a support-state flag. No removal: like the dry-stack acceptance fixture the action
 * is settling under gravity. OUTCOME assertions per DESIGN.md §4 — lost-earth, released count,
 * Stranded == 0; displacement is never read.
 *
 * NEEDS A TICKING WORLD: NO. Same footing as the two-load-path red above.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FTwoLoadPathGateScopedByBlockCapTest,
	"DestructionGame.Acceptance.Overturning.TheEquilibriumGateIsScopedByBlockCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FTwoLoadPathGateScopedByBlockCapTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace TwoLoadPathOverturningTestSupport;

	/* ------------------------------------------------------------------ *
	 * PRECONDITIONS: the same past-tipping, LP-falls body as the red above.
	 * The cap only means anything if the gate SHOULD catch this body below it.
	 * ------------------------------------------------------------------ */

	TestEqual(TEXT("FIXTURE: the seats are bonded with the mean-basis 0.70 flexural bond"),
		GeneralPurposeMortar.TensileStrengthMPa, 0.7);

	const double OverturningRatio =
		OverturningMomentUuCm() / MaxPlasticRestoringMomentUuCm(GeneralPurposeMortar.TensileStrengthMPa);

	TestTrue(
		*FString::Printf(
			TEXT("FIXTURE: the body must be past tipping under the MOST charitable plastic bond; "
				 "overturning/restoring is %.10g and must exceed 2"),
			OverturningRatio),
		OverturningRatio > 2.0);

	/* Build once for the topology/oracle preconditions; the two cascade runs each build fresh. */
	FTwoPathBinding Probe;
	BuildBinding(Probe);

	if (Probe.AnchorJoint == INDEX_NONE || Probe.PivotJoint == INDEX_NONE)
	{
		AddError(TEXT("FIXTURE: the producer must emit both body-seat bed joints"));
		return false;
	}

	const FStructure& ProbeS = Probe.Binding.GetStructure();

	/*
	 * THE BLOCK COUNT THE CAP IS COMPARED AGAINST — pinned so the two caps below are
	 * unambiguously on either side of it. Three live pieces: two seats and one body.
	 */
	TestEqual(TEXT("FIXTURE: three live blocks — the count the cap gates on"), ProbeS.NumPieces(), 3);

	TestEqual(TEXT("FIXTURE: exactly two load paths beneath the body"),
		ProbeS.NumConnections(), 2);

	/* The cross-check that licenses catching the body: the LP finds NO equilibrium at self-weight. */
	RigidBlockOracle::FOracleProblem Problem;
	FString BridgeWhy;

	const bool bBridged = RigidBlockOracle::BuildRigidBlockProblem(ProbeS, Problem, BridgeWhy);

	if (TestTrue(
			*FString::Printf(TEXT("CROSS-CHECK: the oracle bridge must accept this 2D structure (%s)"), *BridgeWhy),
			bBridged))
	{
		const RigidBlockOracle::FOracleResult Oracle = RigidBlockOracle::SolveRigidBlock(Problem);

		AddInfo(FString::Printf(
			TEXT("CROSS-CHECK: oracle answered %d, lambda* %.10g — Falls means lambda* < 1"),
			Oracle.bAnswered ? 1 : 0, Oracle.Lambda));

		TestTrue(
			TEXT("CROSS-CHECK: the LP must find NO equilibrium at self-weight (lambda* < 1) — this is "
				 "the body the gate must catch when it is authoritative"),
			Oracle.bAnswered
				&& RigidBlockOracle::OutcomeOf(Oracle) == RigidBlockOracle::EOracleOutcome::Falls
				&& Oracle.Lambda < 0.9);
	}

	/* ------------------------------------------------------------------ *
	 * RUN THE PLAYER PIPELINE TWICE, CHANGING ONLY THE CAP. Fresh build each
	 * time — SolveAndBreak is destructive — so the only difference is the cap.
	 * ------------------------------------------------------------------ */

	struct FRun
	{
		int32 Passes = 0;
		int32 Released = 0;
		int32 Stranded = 0;
		bool bBodyLostEarth = false;
		EPieceSupport BodySupport = EPieceSupport::Falling;
		bool bSeatsGrounded = false;
	};

	auto RunAtCap = [](int32 Cap) -> FRun
	{
		FTwoPathBinding Fx;
		BuildBinding(Fx);
		Fx.Binding.SetEquilibriumGateBlockCap(Cap);

		FRun R;
		R.Passes = Fx.Binding.SolveAndBreak();
		R.Released = Fx.Binding.ApplyResults();

		const FStructure& S = Fx.Binding.GetStructure();
		R.Stranded = StrandedCount(S);
		R.bBodyLostEarth = HasLostTheEarth(S, Fx.Body);
		R.BodySupport = S.GetPieceSupport(Fx.Body);
		R.bSeatsGrounded = S.GetPieceSupport(Fx.Anchor) == EPieceSupport::Grounded
			&& S.GetPieceSupport(Fx.Pivot) == EPieceSupport::Grounded;
		return R;
	};

	/* AT OR BELOW THE CAP the gate is authoritative and MUST catch the body. */
	constexpr int32 AuthoritativeCap = 8;
	const FRun Auth = RunAtCap(AuthoritativeCap);

	/* ABOVE THE CAP the gate declines to the router — the body is NOT caught (today's behaviour). */
	constexpr int32 DeclineCap = 2;
	const FRun Decline = RunAtCap(DeclineCap);

	AddInfo(FString::Printf(
		TEXT("CAP=%d (>=3, authoritative): passes %d, released %d, body-lost-earth %d, body support %d, "
			 "stranded %d. CAP=%d (<3, declines): passes %d, released %d, body-lost-earth %d, body support "
			 "%d, stranded %d. (support 1=Grounded,2=Supported,3=Stranded,0=Falling)"),
		AuthoritativeCap, Auth.Passes, Auth.Released, Auth.bBodyLostEarth ? 1 : 0,
		static_cast<int32>(Auth.BodySupport), Auth.Stranded,
		DeclineCap, Decline.Passes, Decline.Released, Decline.bBodyLostEarth ? 1 : 0,
		static_cast<int32>(Decline.BodySupport), Decline.Stranded));

	/* No routing artefact may wear a verdict's clothes in either run. */
	TestEqual(TEXT("PRECONDITION: nothing Stranded at or below the cap"), Auth.Stranded, 0);
	TestEqual(TEXT("PRECONDITION: nothing Stranded above the cap"), Decline.Stranded, 0);

	TestTrue(TEXT("BOTH RUNS: the two grounded seats keep the earth — only the body is ever at stake"),
		Auth.bSeatsGrounded && Decline.bSeatsGrounded);

	/* ------------------------------------------------------------------ *
	 * THE DECLINE ARM — passes today, and guards the seam once the gate lands.
	 * Above the cap the gate must NOT run: the body stands, nothing is released.
	 * ------------------------------------------------------------------ */

	TestTrue(
		*FString::Printf(
			TEXT("ABOVE CAP: the gate must decline to the router (no overturning check), so the body is "
				 "NOT caught — it keeps the earth (support %d) and nothing is released (%d)"),
			static_cast<int32>(Decline.BodySupport), Decline.Released),
		!Decline.bBodyLostEarth && Decline.Released == 0);

	/* ------------------------------------------------------------------ *
	 * THE RED — below the cap the gate is authoritative and MUST catch the body.
	 * Today no gate exists, so the body stands here too and this arm fails.
	 * ------------------------------------------------------------------ */

	TestTrue(
		*FString::Printf(
			TEXT("RED, AT/BELOW CAP: with the gate authoritative the LP-infeasible body must lose the "
				 "earth and be released; it reads support %d, released %d, passes %d. Today production "
				 "stands it — no equilibrium gate exists — so this is the behaviour Slice 2 adds"),
			static_cast<int32>(Auth.BodySupport), Auth.Released, Auth.Passes),
		Auth.bBodyLostEarth && Auth.Released >= 1);

	/* THE SEAM ITSELF: the block cap ALONE flips the verdict on one and the same body. */
	TestTrue(
		TEXT("RED, THE SEAM: the block cap alone must decide the gate's authority — the same body is "
			 "caught at/below the cap and NOT caught above it"),
		Auth.bBodyLostEarth && !Decline.bBodyLostEarth);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
