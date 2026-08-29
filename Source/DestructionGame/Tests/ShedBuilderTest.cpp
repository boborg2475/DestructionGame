// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/DestructionShed.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE SHED BUILDER — SHED_PATH.md Phase F, slice F1, the first authored, catalogue-buildable shed,
 * and the point where the shed stops being a pile of proven mechanisms and becomes a thing you can
 * build through the scenario/binding pipeline.
 *
 * THE BEHAVIOUR, IN ONE SENTENCE. DestructionShed::Build lays a minimal 2D X-Z shed cross-section
 * — two grounded ClayBrick piers, a Timber roof beam bearing on both wall heads, and a Timber
 * overhang screwed to the front pier and carried on a grounded Timber post — as an FBrickLayout in
 * which every piece carries its authored MATERIAL and every contact its authored CONNECTION, so
 * that the assembled shed STANDS under the rigid-block LP, pulling the POST drops the overhang,
 * pulling the BACK head drops the roof, and pulling the FRONT head drops both.
 *
 * EVERYTHING THE PHYSICS NEEDS IS ALREADY PROVEN AND DIMENSION-INDEPENDENT (2D LP): the Timber C24
 * material (B1), the connection x material weakest-link bearing (B2/B3), fastener withdrawal tension
 * (C1), and the whole posts-plus-fixing overhang mechanism (C2,
 * Acceptance.Overhang.CarriedByPostsInCompressionAndAWallFixingInTension). This slice does NOT add
 * physics; it authors a builder that composes those mechanisms into one structure. So the RED is
 * expected to be "the builder lays nothing yet" — the F1 stub is a bare `return false` — and NOT a
 * physics gap. Every arm below is either the C2 overhang mechanism verbatim or a simply-supported
 * beam on two compression bearings, both of which the suite already stands and fells correctly.
 *
 * =========================================================================================
 * THE FIXTURE — A 2D X-Z CROSS-SECTION THROUGH THE DOORWAY. X IS DEPTH (BACK -> FRONT -> DOOR),
 * Z IS HEIGHT. SEVEN PIECES, SIX BED JOINTS.
 * =========================================================================================
 *
 *                                                          overhang centroid c = X 296
 *                              roof beam (Timber)                 |
 *          ##=============================##  fixing (Screw,       v
 *          |          [ R O O F ]          | 4 cm lap, TENSION)   ##=================================+
 *          ##                             ##==+  <- Timber OVERHANG, X in [196,396], over the "door"
 *   BackHead ##                    FrontHead ##                    |                                 |
 *   +------+ ##                    +------+  ##                +---+---+  post (DryStone bearing,
 *   |BackHd|=##                    |FrntHd|=+                  | POST  |  compression) X in [254,266]
 *   +------+                       +------+                    | (wood)|  grounded on the earth
 *   |BackBase|                     |FrntBase|                  |       |
 *   | brick  |  <-- shed depth --> | brick  |                  +=======+
 *   | grnded |    (120 cm door)    | grnded |                     earth
 *   +========+                     +========+
 *      earth                          earth
 *
 * SEVEN PIECES (a masonry pier is a grounded BASE, several fused courses, plus one removable HEAD):
 *   - BackBase  : ClayBrick, GROUNDED. X in [0,40],   Z in [0,170].
 *   - BackHead  : ClayBrick.           X in [0,40],   Z in [171,181]. Carries the roof's back end.
 *   - FrontBase : ClayBrick, GROUNDED. X in [160,200],Z in [0,170].
 *   - FrontHead : ClayBrick.           X in [160,200],Z in [171,181]. Carries the roof's front end
 *                 AND anchors the overhang's screw fixing.
 *   - Roof      : Timber.              X in [0,190],  Z in [182,194] (thick 12). Simply supported
 *                 on the two heads; centroid X 95 sits BETWEEN the two bearings.
 *   - Overhang  : Timber.              X in [196,396],Z in [182,194]. Laps the front head 4 cm
 *                 (the fixing patch) and cantilevers out over the door. Centroid X 296.
 *   - Post      : Timber, GROUNDED.    X in [254,266],Z in [0,181]. Under the front of the overhang.
 *
 * SIX BED JOINTS (all normal +Z, single wythe in X-Z — the 2D LP's domain; no head/Y-normal joints):
 *   - BackBed   : BackBase  - BackHead,  GeneralPurposeMortar (a brick mortar bed).
 *   - FrontBed  : FrontBase - FrontHead, GeneralPurposeMortar.
 *   - RoofBack  : BackHead  - Roof,      DRYSTONE (wood-on-brick frictional bearing, compression).
 *   - RoofFront : FrontHead - Roof,      DRYSTONE (30 cm x 20 cm lap).
 *   - Fixing    : FrontHead - Overhang,  SCREW (the 4 cm x 20 cm = 80 cm2 tension tie).
 *   - PostBrg   : Post      - Overhang,  DRYSTONE (12 cm x 20 cm = 240 cm2 bearing, compression).
 *
 * The roof (X in [0,190]) and the overhang (X in [196,396]) leave a 6 cm gap between their ends, so
 * they never form a joint with each other — the roof spans the shed, the overhang cantilevers the
 * door, and the front head is what they share.
 *
 * =========================================================================================
 * THE STATICS ARE HAND-DERIVED (never mirrored from the LP; the bridged LP is the SECOND,
 * independently-derived confirmation).
 * =========================================================================================
 *
 * THE OVERHANG is C2 verbatim — same length 200, same 4 cm fixing lap, same post 62 cm outboard of
 * the fixing and 36 cm inboard of the centroid — so its three arms come out exactly as C2's:
 *   (a) ASSEMBLED: the fixing carries a comfortable TENSION T = W*(c-Xp)/(Xp-Xf), its withdrawal
 *       capacity ~37x over -> stands.
 *   (b) POST REMOVED: the 4 cm fixing alone must cantilever the beam; its plastic couple is ~2.1x
 *       short of the cantilever moment W*(c-Xf) -> falls.
 *   (c) FIXING ANCHOR REMOVED: the post alone (DryStone, no tension) cannot stop the beam toppling
 *       off it; the topple moment W*(c-Xp) is ~6x the post's compression-only couple -> falls.
 *
 * THE ROOF is a simply-supported beam on two compression-only (DryStone) bearings:
 *   (d) ASSEMBLED: centroid X 95 lies BETWEEN the back bearing (centre 20) and the front bearing
 *       (centre 175), so both vertical reactions are positive -> stands.
 *   (e) EITHER HEAD REMOVED: the roof is left on ONE compression-only bearing with its centroid
 *       well outboard, so the toppling moment W*(centre - centroid) far exceeds the bearing's
 *       compression-only couple halfWidth*W -> falls. (Back head gone: ~5.3x. Front head gone:
 *       ~3.8x.)
 *
 * =========================================================================================
 * UNITS — SPELLED OUT LOCALLY (DESIGN.md §3). 1 N = 100 uu, 1 cm2 = 100 mm2, so 1 MPa over 1 cm2 is
 * 100*100 = 10000 uu. DELIBERATELY not the production constant, so a wrong conversion fails here.
 * Weight is MassKg * 980 (the 1 N = 100 uu factor is already inside the 980); masses come from the
 * published densities (Timber 0.42, ClayBrick 1.9) times the true volumes.
 *
 * NEEDS A TICKING WORLD: NO. The builder is arithmetic over boxes; the structure is arithmetic over
 * a graph; gravity is on (weight = mass*980); every assertion is on the laid layout, the oracle, or
 * the solved outcome. Same footing as the overhang (C2), cross-material bearing (B3) and F0 tests.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges files into one translation unit.
 */
namespace ShedBuilderTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/* ================================================================================
	 * UNITS AND DENSITIES. Lengths in cm at Unreal's default 1 uu = 1 cm.
	 * ================================================================================ */

	/** Structural timber C24, EN 338 mean density. UNITS TRAP: 0.42, never 420. */
	constexpr double TimberDensityGramsPerCubicCm = 0.42;

	/** Fired clay, the figure every wall fixture uses. */
	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** MassKg * 980 IS a weight in uu — the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** 1 MPa over 1 cm2 is 10000 uu. DELIBERATELY a local literal, not the production constant. */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	/* ================================================================================
	 * THE SPEC — the canonical shed, spelled out here so the hand-derivation below reads from the
	 * SAME numbers the builder is handed. The builder's contract is to honour this spec; the sizing
	 * is derived from it independently, and the bridged LP is called against whatever the builder
	 * actually lays.
	 * ================================================================================ */

	constexpr double WytheCm = 20.0;
	constexpr double JointThicknessCm = 1.0;

	constexpr double PierWidthCm = 40.0;
	constexpr double BaseHeightCm = 170.0;
	constexpr double HeadHeightCm = 10.0;
	constexpr double BackPierLeftCm = 0.0;
	constexpr double PierSeparationCm = 160.0;

	constexpr double RoofThicknessCm = 12.0;
	constexpr double RoofFrontCm = 190.0;

	constexpr double OverhangBackCm = 196.0;
	constexpr double OverhangLengthCm = 200.0;
	constexpr double OverhangThicknessCm = 12.0;

	constexpr double PostWidthCm = 12.0;
	constexpr double PostCentreCm = 260.0;

	/* --- coordinates the above imply, worked once so the derivation and the picture agree -------- */

	constexpr double FrontPierLeftCm = BackPierLeftCm + PierSeparationCm;             // 160
	constexpr double FrontPierRightCm = FrontPierLeftCm + PierWidthCm;                // 200
	constexpr double HeadTopZCm = BaseHeightCm + JointThicknessCm + HeadHeightCm;     // 181
	constexpr double BeamBottomZCm = HeadTopZCm + JointThicknessCm;                   // 182

	constexpr double RoofCentroidXCm = (BackPierLeftCm + RoofFrontCm) / 2.0;          // 95
	constexpr double OverhangRightCm = OverhangBackCm + OverhangLengthCm;             // 396
	constexpr double OverhangCentroidXCm = (OverhangBackCm + OverhangRightCm) / 2.0;  // 296

	/* The fixing patch is the overhang's lap onto the front head: [OverhangBackCm, FrontPierRightCm]. */
	constexpr double FixingWidthCm = FrontPierRightCm - OverhangBackCm;               // 4
	constexpr double FixingCentreXCm = (OverhangBackCm + FrontPierRightCm) / 2.0;     // 198
	constexpr double FixingAreaSqCm = FixingWidthCm * WytheCm;                        // 80

	constexpr double PostBearingAreaSqCm = PostWidthCm * WytheCm;                     // 240

	/* Roof bearings: full back head, and the front head as far as the roof reaches. */
	constexpr double RoofBackBearingCentreXCm = (BackPierLeftCm + PierWidthCm) / 2.0; // 20
	constexpr double RoofFrontBearingCentreXCm = (FrontPierLeftCm + RoofFrontCm) / 2.0; // 175
	constexpr double RoofFrontBearingWidthCm = RoofFrontCm - FrontPierLeftCm;         // 30

	/** The production spec, built from those constants. */
	DestructionShed::FShedSpec CanonicalSpec()
	{
		DestructionShed::FShedSpec Spec;

		Spec.WytheCm = WytheCm;
		Spec.JointThicknessCm = JointThicknessCm;
		Spec.PierWidthCm = PierWidthCm;
		Spec.BaseHeightCm = BaseHeightCm;
		Spec.HeadHeightCm = HeadHeightCm;
		Spec.BackPierLeftCm = BackPierLeftCm;
		Spec.PierSeparationCm = PierSeparationCm;
		Spec.RoofThicknessCm = RoofThicknessCm;
		Spec.RoofFrontCm = RoofFrontCm;
		Spec.OverhangBackCm = OverhangBackCm;
		Spec.OverhangLengthCm = OverhangLengthCm;
		Spec.OverhangThicknessCm = OverhangThicknessCm;
		Spec.PostWidthCm = PostWidthCm;
		Spec.PostCentreCm = PostCentreCm;

		return Spec;
	}

	/* ================================================================================
	 * THE INDEPENDENT STATICS — moments about a support against the plastic joint capacity.
	 * ================================================================================ */

	double BeamWeightUu(double LengthXCm, double ThicknessZCm)
	{
		const double MassKg =
			TimberDensityGramsPerCubicCm * LengthXCm * ThicknessZCm * WytheCm / 1000.0;
		return MassKg * GravityCmPerSecondSquared;
	}

	double OverhangWeightUu() { return BeamWeightUu(OverhangLengthCm, OverhangThicknessCm); }
	double RoofWeightUu() { return BeamWeightUu(RoofFrontCm - BackPierLeftCm, RoofThicknessCm); }

	/** The fixing's full withdrawal capacity, uu: the screw's f_t over the whole patch. */
	double FixingWithdrawalCapacityUu(double ScrewTensileMPa)
	{
		return ScrewTensileMPa * ForceUnitsPerMPaSqCmHere * FixingAreaSqCm;
	}

	/** (a) ASSEMBLED overhang: the net TENSION the fixing must carry, weight outboard of the post. */
	double AssembledFixingTensionUu()
	{
		return OverhangWeightUu()
			* (OverhangCentroidXCm - PostCentreCm) / (PostCentreCm - FixingCentreXCm);
	}

	/** (b) POST REMOVED: the cantilever moment the fixing alone must resist, about its centre. */
	double CantileverDemandUuCm()
	{
		return OverhangWeightUu() * (OverhangCentroidXCm - FixingCentreXCm);
	}

	/** (b) The MOST the 4 cm fixing patch can restore — its fully-plastic couple (C2's derivation). */
	double CantileverCapacityUuCm(double ScrewTensileMPa)
	{
		const double HalfWidthCm = FixingWidthCm / 2.0;
		const double CapHalfUu = ScrewTensileMPa * ForceUnitsPerMPaSqCmHere * (FixingAreaSqCm / 2.0);
		return HalfWidthCm * (OverhangWeightUu() + 2.0 * CapHalfUu);
	}

	/** (c) FIXING ANCHOR REMOVED: the overhang's toppling moment about the post centre. */
	double OverhangToppleDemandUuCm()
	{
		return OverhangWeightUu() * (OverhangCentroidXCm - PostCentreCm);
	}

	/** (c) The MOST the post bearing can restore: DryStone is compression-only, one outboard edge. */
	double OverhangToppleCapacityUuCm()
	{
		return (PostWidthCm / 2.0) * OverhangWeightUu();
	}

	/** (e) ONE HEAD REMOVED: the roof's toppling moment about the remaining bearing's centre. */
	double RoofToppleDemandUuCm(double RemainingBearingCentreXCm)
	{
		return RoofWeightUu() * FMath::Abs(RemainingBearingCentreXCm - RoofCentroidXCm);
	}

	/** (e) The MOST that bearing can restore: DryStone compression-only, one edge at halfWidth. */
	double RoofToppleCapacityUuCm(double BearingWidthCm)
	{
		return (BearingWidthCm / 2.0) * RoofWeightUu();
	}

	/* ================================================================================
	 * THE LAID SHED — identified by MATERIAL, GROUNDING and RELATIVE X, never by a handle the
	 * builder happened to hand back in a particular order. That keeps these assertions about the
	 * shed's SHAPE rather than about the builder's internal piece numbering.
	 * ================================================================================ */

	struct FShed
	{
		int32 BackBase = INDEX_NONE;
		int32 BackHead = INDEX_NONE;
		int32 FrontBase = INDEX_NONE;
		int32 FrontHead = INDEX_NONE;
		int32 Roof = INDEX_NONE;
		int32 Overhang = INDEX_NONE;
		int32 Post = INDEX_NONE;
	};

	/**
	 * Sort a handle list by piece centroid X, ascending. Back pier is smaller X than front; the
	 * roof (centroid 95) is smaller than the overhang (centroid 296).
	 */
	void SortByCentroidX(const FStructure& S, TArray<int32>& Handles)
	{
		Handles.Sort([&S](const int32& A, const int32& B)
		{
			return S.GetPiece(A).CentreOfMassCm.X < S.GetPiece(B).CentreOfMassCm.X;
		});
	}

	/**
	 * Name the seven pieces from a laid layout, or fail. Two grounded bricks (bases), two free
	 * bricks (heads), one grounded timber (post) and two free timbers (roof, overhang) is the only
	 * shape that identifies; anything else is a wrongly-built shed and the caller reports it.
	 */
	bool Identify(const FBrickLayout& Layout, FShed& Out)
	{
		const FStructure& S = Layout.Structure;

		TArray<int32> BrickGrounded, BrickFree, TimberGrounded, TimberFree;

		for (int32 Piece = 0; Piece < S.NumPieces(); ++Piece)
		{
			if (S.IsPieceRemoved(Piece))
			{
				continue;
			}

			const FStructurePiece& P = S.GetPiece(Piece);

			if (P.Material == &ClayBrick)
			{
				(P.bIsGrounded ? BrickGrounded : BrickFree).Add(Piece);
			}
			else if (P.Material == &Timber)
			{
				(P.bIsGrounded ? TimberGrounded : TimberFree).Add(Piece);
			}
		}

		if (BrickGrounded.Num() != 2 || BrickFree.Num() != 2
			|| TimberGrounded.Num() != 1 || TimberFree.Num() != 2)
		{
			return false;
		}

		SortByCentroidX(S, BrickGrounded);
		SortByCentroidX(S, BrickFree);
		SortByCentroidX(S, TimberFree);

		Out.BackBase = BrickGrounded[0];
		Out.FrontBase = BrickGrounded[1];
		Out.BackHead = BrickFree[0];
		Out.FrontHead = BrickFree[1];
		Out.Post = TimberGrounded[0];
		Out.Roof = TimberFree[0];
		Out.Overhang = TimberFree[1];

		return true;
	}

	int32 JointBetween(const FStructure& S, int32 PieceA, int32 PieceB)
	{
		for (int32 Joint = 0; Joint < S.NumConnections(); ++Joint)
		{
			const FConnection& C = S.GetConnection(Joint);

			if ((C.PieceA == PieceA && C.PieceB == PieceB)
				|| (C.PieceA == PieceB && C.PieceB == PieceA))
			{
				return Joint;
			}
		}

		return INDEX_NONE;
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

	bool IsStanding(EPieceSupport Support)
	{
		return Support == EPieceSupport::Grounded || Support == EPieceSupport::Supported;
	}

	/** True when a live piece has lost every path to the earth — the outcome a dropped piece shows. */
	bool HasLostTheEarth(const FStructure& S, int32 Piece)
	{
		if (S.IsPieceRemoved(Piece))
		{
			return false;
		}
		return !IsStanding(S.GetPieceSupport(Piece));
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
 * THE BUILDER LAYS A MULTI-MATERIAL SHED THAT STANDS AS BUILT AND DROPS WHEN THE POST OR A PIER IS
 * PULLED — the headline "pull the posts, it drops; pull the wall, it drops."
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShedBuilderTest,
	"DestructionGame.Acceptance.Shed.BuildsAMultiMaterialShedThatStandsAndCollapsesCorrectly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FShedBuilderTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace ShedBuilderTestSupport;

	/* ------------------------------------------------------------------ *
	 * PRECONDITIONS ON THE STRENGTH BASIS — the verdicts turn on these,
	 * so they are pinned to the published figures the sizing was derived
	 * against rather than read from the profiles.
	 * ------------------------------------------------------------------ */

	TestEqual(TEXT("FIXTURE: the fixing is a Screw, withdrawal 0.54 MPa (EN 1995-1-1 8.7.2)"),
		Screw.TensileStrengthMPa, 0.54);
	TestEqual(TEXT("FIXTURE: the DryStone bearings are frictional contacts with NO tension"),
		DryStone.TensileStrengthMPa, 0.0);
	TestTrue(TEXT("FIXTURE: GeneralPurposeMortar is a bonded bed with real cohesion and tension"),
		GeneralPurposeMortar.ShearCohesionMPa > 0.0 && GeneralPurposeMortar.TensileStrengthMPa > 0.0);
	TestEqual(TEXT("FIXTURE: Timber C24 crushes at 21 MPa (f_c,0,k)"),
		Timber.Strength.CompressiveStrengthMPa, 21.0);
	TestEqual(TEXT("FIXTURE: clay brick crushes at 20 MPa"),
		ClayBrick.Strength.CompressiveStrengthMPa, 20.0);

	/* ------------------------------------------------------------------ *
	 * THE "NEITHER ALONE SUFFICIENT" SIZING, HAND-DERIVED. The overhang is
	 * C2 verbatim; the roof is a two-support beam whose centroid sits
	 * between its bearings. Independent of the builder — a guard that these
	 * chosen dimensions actually produce the intended regimes.
	 * ------------------------------------------------------------------ */

	const double Wover = OverhangWeightUu();
	const double AssembledTension = AssembledFixingTensionUu();
	const double FixingCap = FixingWithdrawalCapacityUu(Screw.TensileStrengthMPa);
	const double CantDemand = CantileverDemandUuCm();
	const double CantCap = CantileverCapacityUuCm(Screw.TensileStrengthMPa);
	const double OverhangTopDemand = OverhangToppleDemandUuCm();
	const double OverhangTopCap = OverhangToppleCapacityUuCm();

	const double Wroof = RoofWeightUu();
	const double RoofTopBackGone = RoofToppleDemandUuCm(RoofFrontBearingCentreXCm);
	const double RoofCapBackGone = RoofToppleCapacityUuCm(RoofFrontBearingWidthCm);
	const double RoofTopFrontGone = RoofToppleDemandUuCm(RoofBackBearingCentreXCm);
	const double RoofCapFrontGone = RoofToppleCapacityUuCm(PierWidthCm);

	AddInfo(FString::Printf(
		TEXT("DERIVED: overhang W %.10g uu (centroid %.10g, post %.10g, fixing %.10g); "
			 "assembled tension %.10g vs cap %.10g (%.3gx). cantilever %.10g vs %.10g (%.3gx). "
			 "topple %.10g vs %.10g (%.3gx). roof W %.10g uu (centroid %.10g); back-gone topple "
			 "%.10g vs %.10g (%.3gx); front-gone topple %.10g vs %.10g (%.3gx)."),
		Wover, OverhangCentroidXCm, PostCentreCm, FixingCentreXCm,
		AssembledTension, FixingCap, FixingCap / AssembledTension,
		CantDemand, CantCap, CantDemand / CantCap,
		OverhangTopDemand, OverhangTopCap, OverhangTopDemand / OverhangTopCap,
		Wroof, RoofCentroidXCm,
		RoofTopBackGone, RoofCapBackGone, RoofTopBackGone / RoofCapBackGone,
		RoofTopFrontGone, RoofCapFrontGone, RoofTopFrontGone / RoofCapFrontGone));

	TestTrue(TEXT("SIZING (a): the fixing's withdrawal must comfortably exceed the assembled tension"),
		FixingCap > 3.0 * AssembledTension);
	TestTrue(TEXT("SIZING (b): the cantilever moment must outrun the fixing's plastic capacity"),
		CantDemand > 1.5 * CantCap);
	TestTrue(TEXT("SIZING (c): the topple moment must outrun the post's compression-only capacity"),
		OverhangTopDemand > 1.5 * OverhangTopCap);
	TestTrue(TEXT("SIZING (d): the roof's centroid must sit BETWEEN its two bearings (it stands)"),
		RoofCentroidXCm > RoofBackBearingCentreXCm && RoofCentroidXCm < RoofFrontBearingCentreXCm);
	TestTrue(TEXT("SIZING (e): with the back head gone the roof's topple moment must outrun its bearing"),
		RoofTopBackGone > 1.5 * RoofCapBackGone);
	TestTrue(TEXT("SIZING (e): with the front head gone the roof's topple moment must outrun its bearing"),
		RoofTopFrontGone > 1.5 * RoofCapFrontGone);

	/* ================================================================================
	 * ARM 0 — THE BUILDER LAYS THE SHED. Piece counts, per-piece MATERIAL and grounding, and the
	 * six authored joints with their connection profiles. This is where the F1 stub is RED: it lays
	 * nothing, so Build returns false and Identify fails.
	 * ================================================================================ */

	FBrickLayout Layout;
	const bool bBuilt = DestructionShed::Build(CanonicalSpec(), Layout);

	TestTrue(TEXT("F1: the builder must lay the shed (the stub returns false — this is the RED)"), bBuilt);

	TestEqual(TEXT("F1: seven pieces — two pier bases, two heads, roof, overhang, post"),
		Layout.Structure.NumPieces(), 7);
	TestEqual(TEXT("F1: one box per piece, or AdoptLayout refuses the layout"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());
	TestEqual(TEXT("F1: six bed joints — two mortar beds, two roof bearings, the fixing, the post bearing"),
		Layout.Structure.NumConnections(), 6);

	FShed S;
	const bool bIdentified = bBuilt && Identify(Layout, S);

	if (!bIdentified)
	{
		AddError(TEXT("F1: the builder must lay a shed whose pieces identify by material, grounding "
			"and position — two grounded bricks, two free bricks, one grounded timber post, two free "
			"timber beams. Until the builder is implemented this fails: the stub lays nothing."));
		return false;
	}

	/* --- MATERIALS: the shed is multi-material as authored ------------------------------------ */

	TestTrue(TEXT("F1: the back base is ClayBrick"), Layout.Structure.GetPiece(S.BackBase).Material == &ClayBrick);
	TestTrue(TEXT("F1: the back head is ClayBrick"), Layout.Structure.GetPiece(S.BackHead).Material == &ClayBrick);
	TestTrue(TEXT("F1: the front base is ClayBrick"), Layout.Structure.GetPiece(S.FrontBase).Material == &ClayBrick);
	TestTrue(TEXT("F1: the front head is ClayBrick"), Layout.Structure.GetPiece(S.FrontHead).Material == &ClayBrick);
	TestTrue(TEXT("F1: the roof beam is Timber"), Layout.Structure.GetPiece(S.Roof).Material == &Timber);
	TestTrue(TEXT("F1: the overhang is Timber"), Layout.Structure.GetPiece(S.Overhang).Material == &Timber);
	TestTrue(TEXT("F1: the post is Timber"), Layout.Structure.GetPiece(S.Post).Material == &Timber);

	/* --- GROUNDING: the two pier bases and the post stand on the earth; nothing else does ----- */

	TestTrue(TEXT("F1: the back base is grounded"), Layout.Structure.GetPiece(S.BackBase).bIsGrounded);
	TestTrue(TEXT("F1: the front base is grounded"), Layout.Structure.GetPiece(S.FrontBase).bIsGrounded);
	TestTrue(TEXT("F1: the post is grounded"), Layout.Structure.GetPiece(S.Post).bIsGrounded);
	TestFalse(TEXT("F1: the back head is not grounded"), Layout.Structure.GetPiece(S.BackHead).bIsGrounded);
	TestFalse(TEXT("F1: the front head is not grounded"), Layout.Structure.GetPiece(S.FrontHead).bIsGrounded);
	TestFalse(TEXT("F1: the roof is not grounded"), Layout.Structure.GetPiece(S.Roof).bIsGrounded);
	TestFalse(TEXT("F1: the overhang is not grounded"), Layout.Structure.GetPiece(S.Overhang).bIsGrounded);

	TestTrue(TEXT("F1: the laid shed knows where every piece and joint is, or every moment is silently zero"),
		Layout.Structure.HasCompleteGeometry());

	/* --- JOINTS: the six contacts exist, each with its authored connection ------------------- */

	const int32 BackBed = JointBetween(Layout.Structure, S.BackBase, S.BackHead);
	const int32 FrontBed = JointBetween(Layout.Structure, S.FrontBase, S.FrontHead);
	const int32 RoofBack = JointBetween(Layout.Structure, S.BackHead, S.Roof);
	const int32 RoofFront = JointBetween(Layout.Structure, S.FrontHead, S.Roof);
	const int32 Fixing = JointBetween(Layout.Structure, S.FrontHead, S.Overhang);
	const int32 PostBrg = JointBetween(Layout.Structure, S.Post, S.Overhang);

	TestTrue(TEXT("F1: BackBase - BackHead is a mortar bed"), BackBed != INDEX_NONE);
	TestTrue(TEXT("F1: FrontBase - FrontHead is a mortar bed"), FrontBed != INDEX_NONE);
	TestTrue(TEXT("F1: BackHead - Roof is a bearing"), RoofBack != INDEX_NONE);
	TestTrue(TEXT("F1: FrontHead - Roof is a bearing"), RoofFront != INDEX_NONE);
	TestTrue(TEXT("F1: FrontHead - Overhang is the fixing"), Fixing != INDEX_NONE);
	TestTrue(TEXT("F1: Post - Overhang is the post bearing"), PostBrg != INDEX_NONE);

	if (BackBed == INDEX_NONE || FrontBed == INDEX_NONE || RoofBack == INDEX_NONE
		|| RoofFront == INDEX_NONE || Fixing == INDEX_NONE || PostBrg == INDEX_NONE)
	{
		AddError(TEXT("F1: the six authored joints must all exist before their roles can be read"));
		return false;
	}

	/* The brick beds are bonded mortar; the roof/post bearings are compression-only DryStone; the
	 * fixing is a tension-capable screw. That distinction is the whole cross-material authoring. */
	TestEqual(TEXT("F1: the back bed is GeneralPurposeMortar"),
		Layout.Structure.GetConnection(BackBed).Strength.CompressiveStrengthMPa,
		GeneralPurposeMortar.CompressiveStrengthMPa);
	TestEqual(TEXT("F1: the front bed is GeneralPurposeMortar"),
		Layout.Structure.GetConnection(FrontBed).Strength.CompressiveStrengthMPa,
		GeneralPurposeMortar.CompressiveStrengthMPa);
	TestEqual(TEXT("F1: the back roof bearing is DryStone (no tension)"),
		Layout.Structure.GetConnection(RoofBack).Strength.TensileStrengthMPa, 0.0);
	TestEqual(TEXT("F1: the front roof bearing is DryStone (no tension)"),
		Layout.Structure.GetConnection(RoofFront).Strength.TensileStrengthMPa, 0.0);
	TestEqual(TEXT("F1: the post bearing is DryStone (no tension)"),
		Layout.Structure.GetConnection(PostBrg).Strength.TensileStrengthMPa, 0.0);
	TestEqual(TEXT("F1: the fixing is a Screw — a tension tie"),
		Layout.Structure.GetConnection(Fixing).Strength.TensileStrengthMPa, Screw.TensileStrengthMPa);

	/* The roof and overhang bear/hang through BED joints beneath them (not head joints). */
	TestTrue(TEXT("F1: the roof bears on the front head through a bed joint"),
		Layout.Structure.GetJointRole(RoofFront, S.Roof) == EJointRole::BedBeneath);
	TestTrue(TEXT("F1: the fixing is a bed joint under the overhang's back end"),
		Layout.Structure.GetJointRole(Fixing, S.Overhang) == EJointRole::BedBeneath);
	TestTrue(TEXT("F1: the post bears under the overhang through a bed joint"),
		Layout.Structure.GetJointRole(PostBrg, S.Overhang) == EJointRole::BedBeneath);

	TestEqual(TEXT("F1: the fixing patch is the 4 cm x 20 cm lap, 80 cm2"),
		Layout.Structure.GetConnection(Fixing).InterfaceAreaSqCm, FixingAreaSqCm);
	TestEqual(TEXT("F1: the post bearing is the 12 cm x 20 cm face, 240 cm2"),
		Layout.Structure.GetConnection(PostBrg).InterfaceAreaSqCm, PostBearingAreaSqCm);

	/* ================================================================================
	 * THE STANDS-AND-FALLS ARMS. Each rebuilds a fresh shed, pulls the arm's piece, then reads the
	 * oracle mechanism and the production outcome — mechanism (feasibility, the mechanism's moving
	 * blocks) and outcome (Supported vs Falling, Stranded == 0), never displacement.
	 * ================================================================================ */

	enum class EArm : uint8 { Assembled, PostRemoved, BackHeadRemoved, FrontHeadRemoved };

	struct FArm
	{
		EArm Arm;
		const TCHAR* Label;
		bool bExpectStands;
	};

	const FArm Arms[4] = {
		{ EArm::Assembled,        TEXT("(0) assembled"),          true },
		{ EArm::PostRemoved,      TEXT("(1) post removed"),       false },
		{ EArm::BackHeadRemoved,  TEXT("(2) back head removed"),  false },
		{ EArm::FrontHeadRemoved, TEXT("(3) front head removed"), false },
	};

	for (const FArm& A : Arms)
	{
		FBrickLayout Fresh;
		if (!DestructionShed::Build(CanonicalSpec(), Fresh))
		{
			AddError(FString::Printf(TEXT("%s: the builder must lay the shed"), A.Label));
			return false;
		}

		FShed F;
		if (!Identify(Fresh, F))
		{
			AddError(FString::Printf(TEXT("%s: the laid shed must identify"), A.Label));
			return false;
		}

		/* The pieces this arm expects to lose the earth, and the standing survivors we check. */
		TArray<int32> ExpectedFalling;
		TArray<int32> ExpectedStanding;

		switch (A.Arm)
		{
		case EArm::Assembled:
			ExpectedStanding = { F.Roof, F.Overhang };
			break;
		case EArm::PostRemoved:
			Fresh.Structure.RemovePiece(F.Post);
			ExpectedFalling = { F.Overhang };
			ExpectedStanding = { F.Roof };
			break;
		case EArm::BackHeadRemoved:
			Fresh.Structure.RemovePiece(F.BackHead);
			ExpectedFalling = { F.Roof };
			ExpectedStanding = { F.Overhang };
			break;
		case EArm::FrontHeadRemoved:
			Fresh.Structure.RemovePiece(F.FrontHead);
			ExpectedFalling = { F.Roof, F.Overhang };
			break;
		}

		/* ---- THE MECHANISM, VIA THE ORACLE. ---- */
		RigidBlockOracle::FOracleProblem Problem;
		FString BridgeWhy;
		const bool bBridged = RigidBlockOracle::BuildRigidBlockProblem(Fresh.Structure, Problem, BridgeWhy);

		TestTrue(
			*FString::Printf(TEXT("%s: the oracle bridge must accept this 2D shed (%s)"), A.Label, *BridgeWhy),
			bBridged);

		if (bBridged)
		{
			const RigidBlockOracle::FOracleResult Live = RigidBlockOracle::SolveRigidBlock(Problem);
			const RigidBlockOracle::EOracleOutcome Outcome = RigidBlockOracle::OutcomeOf(Live);

			AddInfo(FString::Printf(
				TEXT("%s: oracle answered %d, lambda* %.10g, outcome %d (2=Stands,1=Falls,0=Unanswerable)"),
				A.Label, Live.bAnswered ? 1 : 0, Live.Lambda, static_cast<int32>(Outcome)));

			TestTrue(*FString::Printf(TEXT("%s: the oracle must ANSWER"), A.Label), Live.bAnswered);

			TestEqual(
				*FString::Printf(TEXT("%s: the LP feasibility must match — %s"), A.Label,
					A.bExpectStands ? TEXT("assembled STANDS") : TEXT("the removal FALLS")),
				static_cast<int32>(Outcome),
				static_cast<int32>(A.bExpectStands
					? RigidBlockOracle::EOracleOutcome::Stands
					: RigidBlockOracle::EOracleOutcome::Falls));

			if (A.bExpectStands)
			{
				TestTrue(
					*FString::Printf(TEXT("%s: lambda* %.10g must sit at or above 1"), A.Label, Live.Lambda),
					Live.bAnswered && Live.Lambda >= 1.0);
			}
			else
			{
				TestTrue(
					*FString::Printf(TEXT("%s: lambda* %.10g must sit clearly below 1"), A.Label, Live.Lambda),
					Live.bAnswered && Live.Lambda < 0.9);

				/* The collapse mechanism (phase-1 dual, gravity dead) must NAME each falling piece as a
				 * moving block, so the fall is a genuine loss of equilibrium and not a routing artefact. */
				RigidBlockOracle::FOracleProblem Dead = Problem;
				Dead.bGravityIsLive = false;
				const RigidBlockOracle::FOracleResult DeadR = RigidBlockOracle::SolveRigidBlock(Dead);

				TestTrue(
					*FString::Printf(TEXT("%s: the LP must extract a certified collapse mechanism"), A.Label),
					DeadR.Mechanism.bPresent && DeadR.Mechanism.bIsCertified);

				for (const int32 Piece : ExpectedFalling)
				{
					const int32 Block = OracleBlockOfPiece(Dead, Piece);
					const bool bMoves = DeadR.Mechanism.bPresent
						&& DeadR.Mechanism.Blocks.IsValidIndex(Block)
						&& DeadR.Mechanism.Blocks[Block].bMoves;

					AddInfo(FString::Printf(TEXT("%s: piece %d is oracle block %d, moves %d"),
						A.Label, Piece, Block, bMoves ? 1 : 0));

					TestTrue(
						*FString::Printf(TEXT("%s: the mechanism must name piece %d as a moving block"),
							A.Label, Piece),
						bMoves);
				}
			}
		}

		/* ---- THE OUTCOME, VIA PRODUCTION. Below the 200-block cap, the LP is the break authority. ---- */
		const int32 Passes = Fresh.Structure.SolveAndBreak();
		const int32 Stranded = StrandedCount(Fresh.Structure);

		AddInfo(FString::Printf(TEXT("%s: PRODUCTION ran %d pass(es); %d stranded"), A.Label, Passes, Stranded));

		TestEqual(
			*FString::Printf(TEXT("%s: nothing may be Stranded — the verdict must be about the shed, not the "
				"solver declining to route"), A.Label),
			Stranded, 0);

		/* The grounded survivors keep the earth in every arm. */
		TestTrue(*FString::Printf(TEXT("%s: the back base keeps the earth"), A.Label),
			Fresh.Structure.GetPieceSupport(F.BackBase) == EPieceSupport::Grounded);
		TestTrue(*FString::Printf(TEXT("%s: the front base keeps the earth"), A.Label),
			Fresh.Structure.GetPieceSupport(F.FrontBase) == EPieceSupport::Grounded);

		for (const int32 Piece : ExpectedStanding)
		{
			TestTrue(
				*FString::Printf(TEXT("%s: piece %d must still be held up (support %d)"), A.Label, Piece,
					static_cast<int32>(Fresh.Structure.GetPieceSupport(Piece))),
				IsStanding(Fresh.Structure.GetPieceSupport(Piece)));
		}

		for (const int32 Piece : ExpectedFalling)
		{
			TestTrue(
				*FString::Printf(TEXT("%s: piece %d must lose the earth (support %d) — the shed drops "
					"correctly when its support is pulled"), A.Label, Piece,
					static_cast<int32>(Fresh.Structure.GetPieceSupport(Piece))),
				HasLostTheEarth(Fresh.Structure, Piece));
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
