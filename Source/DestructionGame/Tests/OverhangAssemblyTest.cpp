// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE FRONT-DOOR OVERHANG ASSEMBLY — SHED_PATH.md Phase C, slice C2, the first concrete piece of
 * the shed's shape, and the acceptance of R-Overhang.
 *
 * THE BEHAVIOUR, IN ONE SENTENCE (R-Overhang, the orchestrator ruling). A wooden overhang over the
 * front door is carried by WOODEN POSTS IN COMPRESSION at the front AND a WALL-FIXING IN TENSION at
 * the back, NEITHER SUFFICIENT ALONE — so it STANDS as assembled, FALLS when the posts are pulled
 * (the fixing alone cannot cantilever it), and FALLS when the wall-fixing (the anchor brick it hangs
 * from) is pulled (the post alone lets it topple off).
 *
 * EVERYTHING THIS NEEDS IS ALREADY BUILT AND DIMENSION-INDEPENDENT (2D LP): the Timber C24 material
 * (B1), the connection x material weakest-link bearing so wood-on-brick and post-under-beam contacts
 * carry the right crush (B3), and fastener withdrawal TENSION in the LP (Screw 0.54 MPa via
 * TensileStrengthMPa, confirmed by the hanging test C1). The toy overhang is ~4 pieces, far below the
 * 200-block cap, so the equilibrium LP is the break authority.
 *
 * =========================================================================================
 * THE FIXTURE — A 2D X-Z CROSS-SECTION. WALL ON THE LEFT (BACK), OVERHANG CANTILEVERING RIGHT.
 * =========================================================================================
 *
 *                                             beam centroid c = X 96
 *                    fixing (Screw, 4 cm)              |
 *                    X in [-4, 0], TENSION             v
 *        WallTop  ##=========================================================+   <- Timber BEAM
 *      +---------+ |                        [ B E A M ]                      |      X in [-4,196]
 *      | WallTop |=+                              |                          |      (over the "door")
 *      +---------+          door opening      +---+---+  post (DryStone
 *      | WallBase|                            | POST  |  bearing, compression)
 *      | (brick, |                            | (wood)|  X in [54,66]
 *      | grounded|                            |       |  grounded on the earth
 *      +=========+                            +=======+
 *         earth                                  earth
 *
 * FOUR PIECES:
 *   - WallBase: ClayBrick, GROUNDED, the shed wall (a pier). X in [-40,0], Z in [0,170].
 *   - WallTop:  ClayBrick, the ANCHOR brick the fixing hangs from; rests on WallBase through a
 *               mortar bed joint. X in [-40,0], Z in [171,181]. Removing it is "remove the wall
 *               bricks the fixing anchors to".
 *   - Beam:     Timber, the overhang. X in [-4,196] (length 200), Z in [182,194] (thick 12). Its
 *               back end LAPS 4 cm onto WallTop; the rest cantilevers out over the door.
 *   - Post:     Timber, GROUNDED, under the front of the beam. X in [54,66] (width 12), Z in [0,181].
 *
 * THREE LIVE JOINTS (all bed joints, normal +Z, single wythe in X-Z — the 2D LP's domain):
 *   - WallBedJoint: WallBase (below) - WallTop (above), GeneralPurposeMortar. Holds the anchor up.
 *   - FixingJoint:  WallTop (below) - Beam (above), SCREW. A 4 cm x 20 cm = 80 cm2 patch, the
 *                   tension tie. Withdrawal is min(Screw 0.54, Timber 14, ClayBrick 2) = 0.54 MPa
 *                   (the screw is the weakest link, so the cross-material pairing leaves the fixing
 *                   a fastener withdrawal, exactly as the hanging test isolates).
 *   - PostJoint:    Post (below) - Beam (above), DRYSTONE (a frictional bearing, compression only,
 *                   no tension). 12 cm x 20 cm = 240 cm2.
 *
 * =========================================================================================
 * THE SIZING IS THE DESIGN WORK (R-Overhang: neither support sufficient alone).
 * =========================================================================================
 *
 * The whole trick is the BEAM CENTROID SITS OUTBOARD OF THE POST (c = 96 > X_post = 60). That single
 * choice makes all three arms come out right, and it is what forces the assembled fixing into TENSION
 * rather than compression:
 *
 *   (a) ASSEMBLED. Two vertical supports, weight outboard of the post, so the beam wants to rotate
 *       about the post with its BACK END LIFTING. The fixing must hold the back down => it is in
 *       TENSION. Statics (fixing tie at X_f, post at X_p, weight W at c):
 *           T_fix = W * (c - X_p) / (X_p - X_f)       (net uplift the fixing resists)
 *           R_post = W + T_fix                         (compression, the post carries more than W)
 *       The fixing's WITHDRAWAL capacity f_t*Conv*A must exceed T_fix -> stands. It does, ~37x, so
 *       the assembled canopy is a comfortable stand, NOT a knife edge.
 *
 *   (b) POSTS REMOVED -> the fixing alone must CANTILEVER the beam. The 4 cm-wide fixing patch has a
 *       tiny lever, so its plastic moment capacity (a compression edge plus a withdrawal-limited
 *       tension edge) is FAR short of the cantilever moment W*(c - X_f):
 *           M_demand  = W * (c - X_f)
 *           M_capacity = d_fix * (W + 2 * f_t*Conv*(A_fix/2))     (d_fix = half the patch width)
 *       M_demand outruns M_capacity ~2.1x -> no admissible equilibrium -> FALLS. This is exactly why
 *       "pull the posts and it drops" is true: a couple of screws cannot cantilever a 2 m canopy.
 *
 *   (c) WALL-FIXING REMOVED (the anchor brick pulled) -> the beam is propped only at the post, weight
 *       outboard, so it TOPPLES off the post. The post bearing carries no tension (DryStone), so its
 *       max restoring moment is compression on the outboard edge only:
 *           M_demand  = W * (c - X_p)
 *           M_capacity = halfPost * W                 (compression-only, one edge)
 *       M_demand outruns it ~6x -> FALLS.
 *
 * NEITHER ALONE: (b) is the fixing alone (fails), (c) is the post alone (fails), (a) is both (stands).
 * That is R-Overhang, made into an equilibrium the LP judges.
 *
 * =========================================================================================
 * WHAT IS ASSERTED (DESIGN.md §4 — mechanism and outcome, never displacement).
 * =========================================================================================
 *
 *   - THE MECHANISM, VIA THE ORACLE. Bridged to the rigid-block LP: assembled must be FEASIBLE
 *     (lambda* >= 1); each removal must be INFEASIBLE (lambda* < 1). On the removals the collapse
 *     MECHANISM (phase-1 dual, gravity-dead) must be present and must NAME THE BEAM as a moving
 *     block — so the fall is a genuine loss of equilibrium of the overhang, not a routing artefact.
 *
 *   - THE OUTCOME, VIA PRODUCTION. After SolveAndBreak (below the cap, so the LP is the authority):
 *     assembled -> the beam reads Supported; each removal -> the beam has lost the earth. Nothing may
 *     be Stranded on a genuine fall. The grounded pieces keep the earth. Displacement is never read.
 *
 * =========================================================================================
 * UNITS — SPELLED OUT LOCALLY (DESIGN.md §3). 1 N = 100 uu, 1 cm2 = 100 mm2, so 1 MPa over 1 cm2 is
 * 100*100 = 10000 uu. DELIBERATELY not the production constant, so a wrong conversion fails here.
 * Weight is MassKg * 980 (the 1 N = 100 uu factor is already inside the 980); masses come from the
 * published densities (Timber 0.42 g/cm3, ClayBrick 1.9 g/cm3) times the true volumes.
 *
 * NEEDS A TICKING WORLD: NO. FStructure is arithmetic over a graph; gravity is on (weight = mass*980),
 * everything is connected, and every assertion is on the oracle, the outcome, or solver state. Same
 * footing as the two-load-path, cross-material-bearing and hanging acceptance tests.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges files into one translation unit.
 */
namespace OverhangAssemblyTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/* ================================================================================
	 * UNITS AND MATERIAL DENSITIES. Lengths in cm at Unreal's default 1 uu = 1 cm.
	 * ================================================================================ */

	/** Structural timber C24, EN 338 mean density. UNITS TRAP: 0.42, never 420. */
	constexpr double TimberDensityGramsPerCubicCm = 0.42;

	/** Fired clay, the figure every wall fixture uses. */
	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** MassKg * 980 IS a weight in uu — the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** 1 MPa over 1 cm2 is 10000 uu. DELIBERATELY a local literal, not the production constant. */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	/** The single wythe: every piece is this deep on Y, so every joint's Y overlap is full. */
	constexpr double WytheCm = 20.0;

	/** A 1 cm mortarless/mortar contact — the separation every bed joint is formed across. */
	constexpr double JointThicknessCm = 1.0;

	/* ================================================================================
	 * THE GEOMETRY, as named constants so the hand-derivation below reads from the SAME
	 * numbers the boxes are built from.
	 * ================================================================================ */

	/* WallBase: the grounded shed pier. */
	constexpr double WallLeftXCm = -40.0;
	constexpr double WallRightXCm = 0.0;
	constexpr double WallBaseBottomZCm = 0.0;
	constexpr double WallBaseTopZCm = 170.0;

	/* WallTop: the anchor brick, one bed joint above WallBase, same footprint. */
	constexpr double WallTopBottomZCm = WallBaseTopZCm + JointThicknessCm;   // 171
	constexpr double WallTopTopZCm = WallTopBottomZCm + 10.0;                // 181

	/* Beam: the overhang. Back end laps 4 cm onto WallTop; the rest cantilevers out. */
	constexpr double BeamLeftXCm = -4.0;
	constexpr double BeamRightXCm = 196.0;
	constexpr double BeamBottomZCm = WallTopTopZCm + JointThicknessCm;       // 182
	constexpr double BeamThicknessZCm = 12.0;
	constexpr double BeamTopZCm = BeamBottomZCm + BeamThicknessZCm;          // 194
	constexpr double BeamLengthXCm = BeamRightXCm - BeamLeftXCm;             // 200
	constexpr double BeamCentroidXCm = (BeamLeftXCm + BeamRightXCm) / 2.0;   // 96

	/* Post: under the FRONT of the beam, grounded on the earth. */
	constexpr double PostLeftXCm = 54.0;
	constexpr double PostRightXCm = 66.0;
	constexpr double PostWidthXCm = PostRightXCm - PostLeftXCm;              // 12
	constexpr double PostCentreXCm = (PostLeftXCm + PostRightXCm) / 2.0;     // 60
	constexpr double PostTopZCm = BeamBottomZCm - JointThicknessCm;          // 181
	constexpr double PostBottomZCm = 0.0;

	/* The fixing patch: the overlap of the beam's back end onto WallTop. */
	constexpr double FixingLeftXCm = BeamLeftXCm;                            // -4
	constexpr double FixingRightXCm = WallRightXCm;                          // 0
	constexpr double FixingWidthXCm = FixingRightXCm - FixingLeftXCm;        // 4
	constexpr double FixingCentreXCm = (FixingLeftXCm + FixingRightXCm) / 2.0; // -2
	constexpr double FixingAreaSqCm = FixingWidthXCm * WytheCm;              // 80

	constexpr double PostBearingAreaSqCm = PostWidthXCm * WytheCm;           // 240

	/* ================================================================================
	 * THE INDEPENDENT STATICS — one rigid beam, moments about a support, against the
	 * plastic joint capacity. Derived here, NOT mirrored from the LP; the bridged LP is
	 * the SECOND, independently derived confirmation, called against the built structure.
	 * ================================================================================ */

	/** The overhang beam's weight in uu (Timber density x true volume x g). */
	double BeamWeightUu()
	{
		const double MassKg =
			TimberDensityGramsPerCubicCm * BeamLengthXCm * BeamThicknessZCm * WytheCm / 1000.0;
		return MassKg * GravityCmPerSecondSquared;
	}

	/** The fixing's full withdrawal capacity, uu: the screw's f_t over the whole patch. */
	double FixingWithdrawalCapacityUu(double ScrewTensileMPa)
	{
		return ScrewTensileMPa * ForceUnitsPerMPaSqCmHere * FixingAreaSqCm;
	}

	/** (a) ASSEMBLED: the net TENSION the fixing must carry, weight outboard of the post. */
	double AssembledFixingTensionUu()
	{
		return BeamWeightUu() * (BeamCentroidXCm - PostCentreXCm) / (PostCentreXCm - FixingCentreXCm);
	}

	/** (b) POSTS REMOVED: the cantilever moment the fixing alone must resist, about its centre. */
	double CantileverDemandUuCm()
	{
		return BeamWeightUu() * (BeamCentroidXCm - FixingCentreXCm);
	}

	/**
	 * (b) The MOST the 4 cm fixing patch can restore: the fully-plastic couple the LP discretises
	 * it into — a compression edge (carrying W plus the tension edge's pull) and a tension edge
	 * limited to the screw withdrawal over its tributary half-area, each a half-width from centre.
	 * M = d * (n_comp + |n_ten|) with n_comp = W + Cap_half and |n_ten| = Cap_half.
	 */
	double CantileverCapacityUuCm(double ScrewTensileMPa)
	{
		const double HalfWidthCm = FixingWidthXCm / 2.0;
		const double CapHalfUu = ScrewTensileMPa * ForceUnitsPerMPaSqCmHere * (FixingAreaSqCm / 2.0);
		return HalfWidthCm * (BeamWeightUu() + 2.0 * CapHalfUu);
	}

	/** (c) WALL-FIXING REMOVED: the toppling moment about the post centre, weight outboard. */
	double ToppleDemandUuCm()
	{
		return BeamWeightUu() * (BeamCentroidXCm - PostCentreXCm);
	}

	/**
	 * (c) The MOST the post bearing can restore: DryStone carries NO tension, so the couple is
	 * compression on the outboard edge only, at most W at a half-post-width lever.
	 */
	double ToppleCapacityUuCm()
	{
		return (PostWidthXCm / 2.0) * BeamWeightUu();
	}

	/* ================================================================================
	 * THE FIXTURE.
	 * ================================================================================ */

	struct FOverhang
	{
		FStructure Structure;

		int32 WallBase = INDEX_NONE;
		int32 WallTop = INDEX_NONE;
		int32 Beam = INDEX_NONE;
		int32 Post = INDEX_NONE;

		int32 WallBedJoint = INDEX_NONE;
		int32 FixingJoint = INDEX_NONE;
		int32 PostJoint = INDEX_NONE;
	};

	FPieceBox MakeBox(double LeftX, double RightX, double BottomZ, double TopZ)
	{
		FPieceBox Box;
		Box.ExtentCm = FVector((RightX - LeftX) * 0.5, WytheCm * 0.5, (TopZ - BottomZ) * 0.5);
		Box.CentreCm = FVector((LeftX + RightX) * 0.5, 0.0, (BottomZ + TopZ) * 0.5);
		return Box;
	}

	double BoxMassKg(const FPieceBox& Box, double DensityGramsPerCubicCm)
	{
		return DensityGramsPerCubicCm
			* (Box.ExtentCm.X * 2.0) * (Box.ExtentCm.Y * 2.0) * (Box.ExtentCm.Z * 2.0) / 1000.0;
	}

	/** Lay the wall, anchor brick, overhang beam and post, tag materials, and join the three beds. */
	void Build(FOverhang& Out)
	{
		const FPieceBox WallBaseBox = MakeBox(WallLeftXCm, WallRightXCm, WallBaseBottomZCm, WallBaseTopZCm);
		const FPieceBox WallTopBox = MakeBox(WallLeftXCm, WallRightXCm, WallTopBottomZCm, WallTopTopZCm);
		const FPieceBox BeamBox = MakeBox(BeamLeftXCm, BeamRightXCm, BeamBottomZCm, BeamTopZCm);
		const FPieceBox PostBox = MakeBox(PostLeftXCm, PostRightXCm, PostBottomZCm, PostTopZCm);

		Out.WallBase = Out.Structure.AddPiece(
			BoxMassKg(WallBaseBox, ClayDensityGramsPerCubicCm), /*bIsGrounded*/ true, WallBaseBox.CentreCm);
		Out.WallTop = Out.Structure.AddPiece(
			BoxMassKg(WallTopBox, ClayDensityGramsPerCubicCm), /*bIsGrounded*/ false, WallTopBox.CentreCm);
		Out.Beam = Out.Structure.AddPiece(
			BoxMassKg(BeamBox, TimberDensityGramsPerCubicCm), /*bIsGrounded*/ false, BeamBox.CentreCm);
		Out.Post = Out.Structure.AddPiece(
			BoxMassKg(PostBox, TimberDensityGramsPerCubicCm), /*bIsGrounded*/ true, PostBox.CentreCm);

		Out.Structure.SetPieceMaterial(Out.WallBase, &ClayBrick);
		Out.Structure.SetPieceMaterial(Out.WallTop, &ClayBrick);
		Out.Structure.SetPieceMaterial(Out.Beam, &Timber);
		Out.Structure.SetPieceMaterial(Out.Post, &Timber);

		FConnection Joint;

		/* WallTop bears on WallBase — an ordinary mortar bed joint holding the anchor up. */
		if (MakeInterface(Out.WallBase, WallBaseBox, Out.WallTop, WallTopBox,
				JointThicknessCm, GeneralPurposeMortar, Joint))
		{
			Out.WallBedJoint = Out.Structure.AddConnection(Joint);
		}

		/* The fixing: the beam's back end screwed onto WallTop. Tension-capable (withdrawal). */
		if (MakeInterface(Out.WallTop, WallTopBox, Out.Beam, BeamBox,
				JointThicknessCm, Screw, Joint))
		{
			Out.FixingJoint = Out.Structure.AddConnection(Joint);
		}

		/* The post bears UNDER the front of the beam — a frictional compression bearing, no tension. */
		if (MakeInterface(Out.Post, PostBox, Out.Beam, BeamBox,
				JointThicknessCm, DryStone, Joint))
		{
			Out.PostJoint = Out.Structure.AddConnection(Joint);
		}
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

	/** True when a live piece has lost every path to the earth — the outcome a dropped overhang shows. */
	bool HasLostTheEarth(const FStructure& S, int32 Piece)
	{
		if (S.IsPieceRemoved(Piece))
		{
			return false;
		}
		const EPieceSupport Support = S.GetPieceSupport(Piece);
		return Support != EPieceSupport::Grounded && Support != EPieceSupport::Supported;
	}

	bool IsStanding(EPieceSupport Support)
	{
		return Support == EPieceSupport::Grounded || Support == EPieceSupport::Supported;
	}

	/** The oracle block that came from a given FStructure piece, via the bridge provenance. */
	int32 OracleBlockOfPiece(const RigidBlockOracle::FOracleProblem& Problem, int32 Piece)
	{
		for (int32 B = 0; B < Problem.PieceOfBlock.Num(); ++B)
		{
			if (Problem.PieceOfBlock[B] == Piece)
			{
				return B;
			}
		}
		return INDEX_NONE;
	}
}

/**
 * THE OVERHANG STANDS ON POSTS-PLUS-FIXING AND FALLS WHEN EITHER IS PULLED — NEITHER ALONE SUFFICIENT.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOverhangAssemblyTest,
	"DestructionGame.Acceptance.Overhang.CarriedByPostsInCompressionAndAWallFixingInTension",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOverhangAssemblyTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace OverhangAssemblyTestSupport;

	/* ------------------------------------------------------------------ *
	 * PRECONDITIONS ON THE STRENGTH BASIS — the whole verdict turns on
	 * these, so they are pinned to the published figures the sizing was
	 * derived against rather than read from the profiles.
	 * ------------------------------------------------------------------ */

	TestEqual(TEXT("FIXTURE: the wall-fixing is a Screw, withdrawal 0.54 MPa (EN 1995-1-1 8.7.2)"),
		Screw.TensileStrengthMPa, 0.54);
	TestEqual(TEXT("FIXTURE: the post bearing is DryStone — a frictional contact with NO tension"),
		DryStone.TensileStrengthMPa, 0.0);
	TestEqual(TEXT("FIXTURE: Timber C24 crushes at 21 MPa (f_c,0,k)"),
		Timber.Strength.CompressiveStrengthMPa, 21.0);
	TestEqual(TEXT("FIXTURE: Timber C24 pulls apart at 14 MPa (f_t,0,k) — genuinely tension-capable"),
		Timber.Strength.TensileStrengthMPa, 14.0);
	TestEqual(TEXT("FIXTURE: clay brick crushes at 20 MPa"),
		ClayBrick.Strength.CompressiveStrengthMPa, 20.0);

	/*
	 * The fixing is a fastener WITHDRAWAL: the weakest link on the tension axis is min(Screw 0.54,
	 * Timber 14, ClayBrick 2) = the screw's 0.54. So the cross-material wiring leaves the wall-fixing
	 * exactly the screw's withdrawal, which is the tension capability the hanging test isolates.
	 */
	const double FixingTensileMPa = FMath::Min3(
		Screw.TensileStrengthMPa, Timber.Strength.TensileStrengthMPa, ClayBrick.Strength.TensileStrengthMPa);
	TestEqual(TEXT("FIXTURE: the fixing's weakest-link withdrawal is the screw's 0.54 MPa"),
		FixingTensileMPa, 0.54);

	/* ------------------------------------------------------------------ *
	 * THE "NEITHER ALONE SUFFICIENT" SIZING, HAND-DERIVED. Assembled the
	 * fixing carries a comfortable tension; the fixing ALONE cannot
	 * cantilever the beam and the post ALONE cannot stop it toppling.
	 * ------------------------------------------------------------------ */

	const double W = BeamWeightUu();
	const double AssembledTension = AssembledFixingTensionUu();
	const double FixingCap = FixingWithdrawalCapacityUu(Screw.TensileStrengthMPa);

	const double CantDemand = CantileverDemandUuCm();
	const double CantCap = CantileverCapacityUuCm(Screw.TensileStrengthMPa);

	const double ToppleDemand = ToppleDemandUuCm();
	const double ToppleCap = ToppleCapacityUuCm();

	AddInfo(FString::Printf(
		TEXT("DERIVED: beam weight %.10g uu, centroid X %.10g, post X %.10g, fixing X %.10g. "
			 "(a) assembled fixing tension %.10g uu vs withdrawal capacity %.10g uu (stands, margin %.3gx). "
			 "(b) cantilever moment %.10g vs fixing capacity %.10g uu.cm (falls, %.3gx). "
			 "(c) topple moment %.10g vs post capacity %.10g uu.cm (falls, %.3gx)."),
		W, BeamCentroidXCm, PostCentreXCm, FixingCentreXCm,
		AssembledTension, FixingCap, FixingCap / AssembledTension,
		CantDemand, CantCap, CantDemand / CantCap,
		ToppleDemand, ToppleCap, ToppleDemand / ToppleCap));

	TestTrue(
		*FString::Printf(TEXT("SIZING (a): the fixing's withdrawal (%.10g) must comfortably exceed the "
			"assembled tension (%.10g) — a stand, not a knife edge"), FixingCap, AssembledTension),
		FixingCap > 3.0 * AssembledTension);

	TestTrue(
		*FString::Printf(TEXT("SIZING (b): the cantilever moment (%.10g) must outrun the fixing's plastic "
			"capacity (%.10g) — the fixing ALONE cannot cantilever the beam"), CantDemand, CantCap),
		CantDemand > 1.5 * CantCap);

	TestTrue(
		*FString::Printf(TEXT("SIZING (c): the topple moment (%.10g) must outrun the post's compression-only "
			"capacity (%.10g) — the post ALONE lets it topple"), ToppleDemand, ToppleCap),
		ToppleDemand > 1.5 * ToppleCap);

	/* ------------------------------------------------------------------ *
	 * THE THREE ARMS. Each builds a fresh overhang, removes the arm's
	 * piece(s), then reads the oracle mechanism and the production outcome.
	 * ------------------------------------------------------------------ */

	enum class EArm : uint8 { Assembled, PostsRemoved, FixingRemoved };

	struct FArm
	{
		EArm Arm;
		const TCHAR* Label;
		bool bExpectStands;
	};

	const FArm Arms[3] = {
		{ EArm::Assembled,     TEXT("(a) assembled"),        true },
		{ EArm::PostsRemoved,  TEXT("(b) posts removed"),    false },
		{ EArm::FixingRemoved, TEXT("(c) wall-fixing removed"), false },
	};

	for (const FArm& A : Arms)
	{
		FOverhang Fx;
		Build(Fx);

		if (Fx.WallBedJoint == INDEX_NONE || Fx.FixingJoint == INDEX_NONE || Fx.PostJoint == INDEX_NONE)
		{
			AddError(FString::Printf(TEXT("%s: FIXTURE: the producer must emit all three bed joints"), A.Label));
			return false;
		}

		/* Topology preconditions on the freshly-built (pre-removal) structure — only once. */
		if (A.Arm == EArm::Assembled)
		{
			TestEqual(TEXT("FIXTURE: four pieces — wall base, anchor brick, overhang beam, post"),
				Fx.Structure.NumPieces(), 4);
			TestEqual(TEXT("FIXTURE: three joints — wall bed, screw fixing, post bearing"),
				Fx.Structure.NumConnections(), 3);
			TestTrue(TEXT("FIXTURE: every piece and joint must know where it is, or there are no lever arms"),
				Fx.Structure.HasCompleteGeometry());
			TestTrue(TEXT("FIXTURE: the beam bears on the post through a BED joint (a compression bearing)"),
				Fx.Structure.GetJointRole(Fx.PostJoint, Fx.Beam) == EJointRole::BedBeneath);
			TestTrue(TEXT("FIXTURE: the fixing is a BED joint under the beam's back end (the tension tie)"),
				Fx.Structure.GetJointRole(Fx.FixingJoint, Fx.Beam) == EJointRole::BedBeneath);
			TestEqual(TEXT("FIXTURE: the fixing patch is the 4 cm x 20 cm lap, 80 cm2"),
				Fx.Structure.GetConnection(Fx.FixingJoint).InterfaceAreaSqCm, FixingAreaSqCm);
			TestEqual(TEXT("FIXTURE: the post bearing is the 12 cm x 20 cm face, 240 cm2"),
				Fx.Structure.GetConnection(Fx.PostJoint).InterfaceAreaSqCm, PostBearingAreaSqCm);
		}

		/* Pull the arm's piece(s). */
		if (A.Arm == EArm::PostsRemoved)
		{
			Fx.Structure.RemovePiece(Fx.Post);
		}
		else if (A.Arm == EArm::FixingRemoved)
		{
			Fx.Structure.RemovePiece(Fx.WallTop);
		}

		/* ---- THE MECHANISM, VIA THE ORACLE. ---- */
		RigidBlockOracle::FOracleProblem Problem;
		FString BridgeWhy;
		const bool bBridged = RigidBlockOracle::BuildRigidBlockProblem(Fx.Structure, Problem, BridgeWhy);

		TestTrue(
			*FString::Printf(TEXT("%s: the oracle bridge must accept this 2D overhang (%s)"), A.Label, *BridgeWhy),
			bBridged);

		if (bBridged)
		{
			const RigidBlockOracle::FOracleResult Live = RigidBlockOracle::SolveRigidBlock(Problem);
			const RigidBlockOracle::EOracleOutcome Outcome = RigidBlockOracle::OutcomeOf(Live);

			AddInfo(FString::Printf(
				TEXT("%s: oracle answered %d, lambda* %.10g, outcome %d (2=Stands,1=Falls,0=Unanswerable)"),
				A.Label, Live.bAnswered ? 1 : 0, Live.Lambda, static_cast<int32>(Outcome)));

			TestTrue(
				*FString::Printf(TEXT("%s: the oracle must ANSWER (a refusal proves nothing)"), A.Label),
				Live.bAnswered);

			TestEqual(
				*FString::Printf(TEXT("%s: the LP feasibility must match R-Overhang — %s"),
					A.Label, A.bExpectStands ? TEXT("assembled STANDS") : TEXT("the removal FALLS")),
				static_cast<int32>(Outcome),
				static_cast<int32>(A.bExpectStands
					? RigidBlockOracle::EOracleOutcome::Stands
					: RigidBlockOracle::EOracleOutcome::Falls));

			if (A.bExpectStands)
			{
				TestTrue(
					*FString::Printf(TEXT("%s: lambda* %.10g must sit at or above 1 — an admissible equilibrium"),
						A.Label, Live.Lambda),
					Live.bAnswered && Live.Lambda >= 1.0);
			}
			else
			{
				TestTrue(
					*FString::Printf(TEXT("%s: lambda* %.10g must sit clearly below 1 — no admissible equilibrium"),
						A.Label, Live.Lambda),
					Live.bAnswered && Live.Lambda < 0.9);

				/*
				 * THE COLLAPSE MECHANISM must NAME THE BEAM as a moving block, so the fall is a genuine
				 * loss of equilibrium of the overhang — not a routing artefact wearing its clothes. The
				 * mechanism lives on the infeasible arm of the FEASIBILITY formulation (gravity dead).
				 */
				RigidBlockOracle::FOracleProblem Dead = Problem;
				Dead.bGravityIsLive = false;
				const RigidBlockOracle::FOracleResult DeadR = RigidBlockOracle::SolveRigidBlock(Dead);

				const int32 BeamBlock = OracleBlockOfPiece(Dead, Fx.Beam);
				const bool bBeamMoves = DeadR.Mechanism.bPresent
					&& DeadR.Mechanism.Blocks.IsValidIndex(BeamBlock)
					&& DeadR.Mechanism.Blocks[BeamBlock].bMoves;

				AddInfo(FString::Printf(
					TEXT("%s: mechanism present %d, certified %d, beam is oracle block %d, beam moves %d"),
					A.Label, DeadR.Mechanism.bPresent ? 1 : 0, DeadR.Mechanism.bIsCertified ? 1 : 0,
					BeamBlock, bBeamMoves ? 1 : 0));

				TestTrue(
					*FString::Printf(TEXT("%s: the LP must extract a certified collapse mechanism"), A.Label),
					DeadR.Mechanism.bPresent && DeadR.Mechanism.bIsCertified);

				TestTrue(
					*FString::Printf(TEXT("%s: the mechanism must NAME THE BEAM as a moving block — the "
						"overhang is what loses equilibrium"), A.Label),
					bBeamMoves);
			}
		}

		/* ---- THE OUTCOME, VIA PRODUCTION. Below the 200-block cap, the LP is the break authority. ---- */
		const int32 Passes = Fx.Structure.SolveAndBreak();

		const EPieceSupport BeamSupport = Fx.Structure.GetPieceSupport(Fx.Beam);
		const int32 Stranded = StrandedCount(Fx.Structure);

		AddInfo(FString::Printf(
			TEXT("%s: PRODUCTION ran %d breaking pass(es); beam support %d "
				 "(1=Grounded,2=Supported,3=Stranded,0=Falling); %d stranded"),
			A.Label, Passes, static_cast<int32>(BeamSupport), Stranded));

		TestEqual(
			*FString::Printf(TEXT("%s: nothing may be Stranded — a verdict must be about the masonry, not the "
				"solver declining to route"), A.Label),
			Stranded, 0);

		TestTrue(
			*FString::Printf(TEXT("%s: the grounded wall base keeps the earth"), A.Label),
			Fx.Structure.GetPieceSupport(Fx.WallBase) == EPieceSupport::Grounded);

		if (A.bExpectStands)
		{
			TestEqual(
				*FString::Printf(TEXT("%s: the overhang STANDS — the beam reads Supported after the cascade, "
					"support %d"), A.Label, static_cast<int32>(BeamSupport)),
				static_cast<int32>(BeamSupport), static_cast<int32>(EPieceSupport::Supported));

			/* When it stands, the post is genuinely there and grounded, carrying the compression path. */
			TestTrue(
				*FString::Printf(TEXT("%s: the grounded post keeps the earth (the compression support)"), A.Label),
				Fx.Structure.GetPieceSupport(Fx.Post) == EPieceSupport::Grounded);
		}
		else
		{
			TestTrue(
				*FString::Printf(TEXT("%s: the overhang FALLS — the beam loses the earth (support %d), because "
					"R-Overhang says neither support holds it alone"), A.Label, static_cast<int32>(BeamSupport)),
				HasLostTheEarth(Fx.Structure, Fx.Beam));

			/* The remaining single support keeps the earth — only the overhang is ever at stake. */
			if (A.Arm == EArm::PostsRemoved)
			{
				TestTrue(
					*FString::Printf(TEXT("%s: the anchor brick it hung from keeps the earth"), A.Label),
					IsStanding(Fx.Structure.GetPieceSupport(Fx.WallTop)));
			}
			else
			{
				TestTrue(
					*FString::Printf(TEXT("%s: the grounded post keeps the earth"), A.Label),
					Fx.Structure.GetPieceSupport(Fx.Post) == EPieceSupport::Grounded);
			}
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
