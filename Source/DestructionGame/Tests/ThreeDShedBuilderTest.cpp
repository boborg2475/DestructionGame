// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/DestructionShed3D.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE 3D SHED BUILDER — the point where the shed stops being a flat cross-section and becomes a
 * genuinely-3D closed box (THREED_DESIGN.md E1-E3).
 *
 * THE BEHAVIOUR, IN ONE SENTENCE. DestructionShed3D::Build lays a minimal toy 3D shed — four
 * grounded ClayBrick walls forming a closed box braced at the corners by out-of-plane (Y-normal)
 * mortar joints, a Timber roof beam bearing on the back and front walls, and a Timber overhang
 * screwed to the front wall and carried on a grounded Timber post — as a SetThreeDimensional
 * FBrickLayout in which every piece carries its authored MATERIAL and every contact its authored
 * CONNECTION, so that (bridged to the 3D LP) the assembled shed STANDS, pulling the POST drops the
 * overhang, and pulling the BACK wall drops the roof while the other three corner-braced walls
 * still stand.
 *
 * The physics this needs is already proven and reachable (THREED_DESIGN.md E1-E3): the 3D
 * six-equilibrium-row assembly + inscribed friction pyramid (E1), the deterministic 3D collapse
 * mechanism (E2), and the production bridge that poses a SetThreeDimensional FStructure to the 3D
 * LP and stops refusing its Y-normal joints (E3). The 2D shed's overhang and roof mechanisms are
 * proven (Acceptance.Shed / Acceptance.Overhang, C2); this test authors a builder that composes
 * those into one 3D structure rather than adding new physics.
 *
 * =========================================================================================
 * THE FIXTURE — A CLOSED AXIS-ALIGNED BOX. X IS WIDTH, Y IS DEPTH (INTO THE DOOR), Z IS HEIGHT.
 * SEVEN PIECES, EIGHT JOINTS. All dimensions cm; the numbers below are worked from the spec.
 * =========================================================================================
 *
 * PLAN VIEW (looking down -Z; X right, Y up the page):
 *
 *        X=0                                   X=220
 *     Y=220 +----------- FRONT WALL -----------+   front wall Y[200,220]  (grounded ClayBrick)
 *           |  fixing (Screw) + post out in +Y |
 *           | L                              R |   left  wall X[0,20]  Y[21,199] (grounded)
 *           | E                              I |   right wall X[200,220] Y[21,199] (grounded)
 *           | F         [ roof beam ]        G |   roof X[40,180] Y[10,210] Z[181,193] (Timber)
 *           | T                              H |
 *      Y=0  +------------ BACK WALL -----------+   back wall Y[0,20]   (grounded ClayBrick)
 *
 * SEVEN PIECES:
 *   - BackWall  : ClayBrick, GROUNDED. X[0,220],   Y[0,20],    Z[0,180].  Carries the roof's back end.
 *   - FrontWall : ClayBrick, GROUNDED. X[0,220],   Y[200,220], Z[0,180].  Carries the roof's front end
 *                 AND anchors the overhang's screw fixing.
 *   - LeftWall  : ClayBrick, GROUNDED. X[0,20],    Y[21,199],  Z[0,180].  Closes+braces the box.
 *   - RightWall : ClayBrick, GROUNDED. X[200,220], Y[21,199],  Z[0,180].  Closes+braces the box.
 *   - Roof      : Timber.              X[40,180],  Y[10,210],  Z[181,193]. Simply supported on the
 *                 back and front wall tops; centroid Y 110 sits BETWEEN the two bearings.
 *   - Overhang  : Timber.              X[100,120], Y[216,416], Z[181,193]. Laps the front wall top
 *                 4 cm in Y (the fixing patch) and cantilevers out over the door. Centroid Y 316.
 *   - Post      : Timber, GROUNDED.    X[100,120], Y[274,286], Z[0,180].  Under the front of the
 *                 overhang.
 *
 * EIGHT JOINTS:
 *   - four CORNER joints (each a wall's Y-facing face against the side wall between them), normal
 *     +/-Y, GeneralPurposeMortar (a brick corner bond) — the genuinely-3D feature, area 20 x 180 =
 *     3600 cm2. THESE ARE OUT OF THE X-Z PLANE: a 2D oracle cannot express them at all, which is
 *     why the structure is flagged 3D and the E3 bridge is what lets them through.
 *   - two roof BEARINGS: Roof-BackWall and Roof-FrontWall, normal +Z, DryStone (wood-on-brick,
 *     compression only), 140 x 10 = 1400 cm2.
 *   - the FIXING: FrontWall-Overhang, normal +Z, SCREW (the 4 cm x 20 cm = 80 cm2 tension tie).
 *   - the POST bearing: Post-Overhang, normal +Z, DryStone (12 x 20 = 240 cm2 bearing, compression).
 *
 * The roof (Y[10,210]) and the overhang (Y[216,416]) leave a 6 cm gap, so they never join; the
 * front wall is what they share. The roof (X[40,180]) clears the side walls (X[0,20], X[200,220]).
 *
 * =========================================================================================
 * THE STATICS ARE HAND-DERIVED (never mirrored from the LP; the bridged LP is the SECOND,
 * independently-derived confirmation). Every arm below is a proven 2D mechanism with the in-plane
 * axis swapped X->Y, laid on a genuinely-3D closed box.
 * =========================================================================================
 *
 * THE OVERHANG is C2 verbatim with X->Y — same length 200, same 4 cm fixing lap, same post 62 cm
 * outboard of the fixing and 36 cm inboard of the centroid — so its arms come out as C2's:
 *   (a) ASSEMBLED: the fixing carries a comfortable TENSION T = W*(cY-Yp)/(Yp-Yf), withdrawal
 *       capacity ~37x over -> stands.
 *   (b) POST REMOVED: the 4 cm fixing alone must cantilever the beam; its plastic couple is ~2.1x
 *       short of the cantilever moment W*(cY-Yf) -> falls.
 *
 * THE ROOF is a simply-supported beam on two compression-only (DryStone) bearings in Y:
 *   (d) ASSEMBLED: centroid Y 110 lies BETWEEN the back bearing (centre 15) and the front bearing
 *       (centre 205), so both vertical reactions are positive -> stands.
 *   (e) BACK WALL REMOVED: the roof is left on the FRONT compression-only bearing (centre 205) with
 *       its centroid 95 cm outboard, so the toppling moment W*95 far exceeds the bearing's
 *       compression-only couple (halfWidth 5)*W -> ~19x -> falls. The other three walls are each
 *       independently grounded, so the box does NOT fully collapse (corner bracing shown by their
 *       survival, and the roof loses a bearing rather than a whole-box failure).
 *
 * UNITS — spelled out locally (DESIGN.md §3). 1 N = 100 uu, 1 cm2 = 100 mm2, so 1 MPa over 1 cm2
 * is 100*100 = 10000 uu, deliberately not the production constant, so a wrong conversion fails
 * here. Weight is MassKg * 980 (the 1 N = 100 uu factor is already inside the 980); masses come
 * from the published densities (Timber 0.42, ClayBrick 1.9) times the true volumes.
 *
 * NEEDS A TICKING WORLD: no. The builder is arithmetic over boxes, the structure over a graph,
 * the LP over that; gravity is on (weight = mass*980); every assertion is on the laid layout, the
 * bridged oracle, or the solved outcome — no Chaos, no world tick.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges files into one translation unit.
 */
namespace ThreeDShedBuilderTestSupport
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
	 * THE SPEC — the canonical toy box, spelled out here so the hand-derivation below reads from the
	 * SAME numbers the builder is handed. The builder's contract is to honour this spec; the sizing
	 * is derived from it independently, and the bridged LP is called against whatever the builder
	 * actually lays.
	 * ================================================================================ */

	constexpr double WallThicknessCm = 20.0;
	constexpr double WallHeightCm = 180.0;
	constexpr double JointThicknessCm = 1.0;
	constexpr double BoxWidthXCm = 220.0;
	constexpr double BoxDepthYCm = 220.0;

	constexpr double RoofInsetXCm = 40.0;
	constexpr double RoofBearingLapYCm = 10.0;
	constexpr double RoofThicknessCm = 12.0;

	constexpr double OverhangCentreXCm = 110.0;
	constexpr double OverhangWidthXCm = 20.0;
	constexpr double OverhangLengthYCm = 200.0;
	constexpr double OverhangThicknessCm = 12.0;
	constexpr double FixingLapYCm = 4.0;

	constexpr double PostWidthYCm = 12.0;
	constexpr double PostCentreYCm = 280.0;

	/* --- coordinates the above imply, worked once so the derivation and the picture agree -------- */

	constexpr double WallTopZCm = WallHeightCm;                                       // 180
	constexpr double BeamBottomZCm = WallHeightCm + JointThicknessCm;                 // 181

	/* The roof beam, spanning Y between the back and front wall tops. */
	constexpr double RoofWidthXCm = BoxWidthXCm - 2.0 * RoofInsetXCm;                 // 140
	constexpr double RoofBackYCm = WallThicknessCm - RoofBearingLapYCm;               // 10
	constexpr double RoofFrontYCm = BoxDepthYCm - WallThicknessCm + RoofBearingLapYCm;// 210
	constexpr double RoofLengthYCm = RoofFrontYCm - RoofBackYCm;                      // 200
	constexpr double RoofCentroidYCm = (RoofBackYCm + RoofFrontYCm) / 2.0;            // 110

	/* Roof bearings: the Y overlap of the roof with each wall top. */
	constexpr double RoofBackBearingCentreYCm = (RoofBackYCm + WallThicknessCm) / 2.0;             // 15
	constexpr double RoofFrontBearingCentreYCm =
		((BoxDepthYCm - WallThicknessCm) + RoofFrontYCm) / 2.0;                                    // 205
	constexpr double RoofBearingWidthYCm = RoofBearingLapYCm;                                       // 10

	/* The overhang, lapping the front wall top and cantilevering in +Y. */
	constexpr double OverhangBackYCm = BoxDepthYCm - FixingLapYCm;                    // 216
	constexpr double OverhangFrontYCm = OverhangBackYCm + OverhangLengthYCm;          // 416
	constexpr double OverhangCentroidYCm = (OverhangBackYCm + OverhangFrontYCm) / 2.0;// 316

	constexpr double FixingCentreYCm = BoxDepthYCm - FixingLapYCm / 2.0;              // 218
	constexpr double FixingAreaSqCm = FixingLapYCm * OverhangWidthXCm;                // 80

	constexpr double PostBearingAreaSqCm = PostWidthYCm * OverhangWidthXCm;           // 240

	constexpr double CornerAreaSqCm = WallThicknessCm * WallHeightCm;                 // 3600

	/** The production spec, built from those constants. */
	DestructionShed3D::FShed3DSpec CanonicalSpec()
	{
		DestructionShed3D::FShed3DSpec Spec;

		Spec.WallThicknessCm = WallThicknessCm;
		Spec.WallHeightCm = WallHeightCm;
		Spec.JointThicknessCm = JointThicknessCm;
		Spec.BoxWidthXCm = BoxWidthXCm;
		Spec.BoxDepthYCm = BoxDepthYCm;
		Spec.RoofInsetXCm = RoofInsetXCm;
		Spec.RoofBearingLapYCm = RoofBearingLapYCm;
		Spec.RoofThicknessCm = RoofThicknessCm;
		Spec.OverhangCentreXCm = OverhangCentreXCm;
		Spec.OverhangWidthXCm = OverhangWidthXCm;
		Spec.OverhangLengthYCm = OverhangLengthYCm;
		Spec.OverhangThicknessCm = OverhangThicknessCm;
		Spec.FixingLapYCm = FixingLapYCm;
		Spec.PostWidthYCm = PostWidthYCm;
		Spec.PostCentreYCm = PostCentreYCm;

		return Spec;
	}

	/* ================================================================================
	 * THE INDEPENDENT STATICS — moments about a support against the plastic joint capacity, the
	 * in-plane axis swapped X->Y from the 2D shed's overhang and roof.
	 * ================================================================================ */

	double BeamWeightUu(double LengthYCm, double ThicknessZCm, double WidthXCm)
	{
		const double MassKg =
			TimberDensityGramsPerCubicCm * LengthYCm * ThicknessZCm * WidthXCm / 1000.0;
		return MassKg * GravityCmPerSecondSquared;
	}

	double OverhangWeightUu()
	{
		return BeamWeightUu(OverhangLengthYCm, OverhangThicknessCm, OverhangWidthXCm);
	}
	double RoofWeightUu()
	{
		return BeamWeightUu(RoofLengthYCm, RoofThicknessCm, RoofWidthXCm);
	}

	/** The fixing's full withdrawal capacity, uu: the screw's f_t over the whole patch. */
	double FixingWithdrawalCapacityUu(double ScrewTensileMPa)
	{
		return ScrewTensileMPa * ForceUnitsPerMPaSqCmHere * FixingAreaSqCm;
	}

	/** (a) ASSEMBLED overhang: the net TENSION the fixing must carry, weight outboard of the post. */
	double AssembledFixingTensionUu()
	{
		return OverhangWeightUu()
			* (OverhangCentroidYCm - PostCentreYCm) / (PostCentreYCm - FixingCentreYCm);
	}

	/** (b) POST REMOVED: the cantilever moment the fixing alone must resist, about its centre. */
	double CantileverDemandUuCm()
	{
		return OverhangWeightUu() * (OverhangCentroidYCm - FixingCentreYCm);
	}

	/** (b) The MOST the 4 cm fixing patch can restore — its fully-plastic couple (C2's derivation). */
	double CantileverCapacityUuCm(double ScrewTensileMPa)
	{
		const double HalfWidthCm = FixingLapYCm / 2.0;
		const double CapHalfUu = ScrewTensileMPa * ForceUnitsPerMPaSqCmHere * (FixingAreaSqCm / 2.0);
		return HalfWidthCm * (OverhangWeightUu() + 2.0 * CapHalfUu);
	}

	/** (e) BACK WALL REMOVED: the roof's toppling moment about the remaining bearing's centre. */
	double RoofToppleDemandUuCm(double RemainingBearingCentreYCm)
	{
		return RoofWeightUu() * FMath::Abs(RemainingBearingCentreYCm - RoofCentroidYCm);
	}

	/** (e) The MOST that bearing can restore: DryStone compression-only, one edge at halfWidth. */
	double RoofToppleCapacityUuCm(double BearingWidthYCm)
	{
		return (BearingWidthYCm / 2.0) * RoofWeightUu();
	}

	/* ================================================================================
	 * THE LAID SHED — identified by MATERIAL, GROUNDING and position, never by a handle the builder
	 * happened to hand back in a particular order. That keeps these assertions about the shed's
	 * SHAPE rather than about the builder's internal piece numbering.
	 * ================================================================================ */

	struct FShed
	{
		int32 BackWall = INDEX_NONE;
		int32 FrontWall = INDEX_NONE;
		int32 LeftWall = INDEX_NONE;
		int32 RightWall = INDEX_NONE;
		int32 Roof = INDEX_NONE;
		int32 Overhang = INDEX_NONE;
		int32 Post = INDEX_NONE;
	};

	/**
	 * Name the seven pieces from a laid layout, or fail. Four grounded bricks (walls), one
	 * grounded timber (post) and two free timbers (roof, overhang) is the only shape that
	 * identifies; the walls are told apart by position — back smallest Y, front largest Y, then
	 * left smallest X and right largest X — and the free beams by Y centroid (roof 110 < overhang
	 * 316).
	 */
	bool Identify(const FBrickLayout& Layout, FShed& Out)
	{
		const FStructure& S = Layout.Structure;

		TArray<int32> BrickGrounded, TimberGrounded, TimberFree;

		for (int32 Piece = 0; Piece < S.NumPieces(); ++Piece)
		{
			if (S.IsPieceRemoved(Piece))
			{
				continue;
			}

			const FStructurePiece& P = S.GetPiece(Piece);

			if (P.Material == &ClayBrick && P.bIsGrounded)
			{
				BrickGrounded.Add(Piece);
			}
			else if (P.Material == &Timber)
			{
				(P.bIsGrounded ? TimberGrounded : TimberFree).Add(Piece);
			}
		}

		if (BrickGrounded.Num() != 4 || TimberGrounded.Num() != 1 || TimberFree.Num() != 2)
		{
			return false;
		}

		/* Back = smallest Y centroid; front = largest Y. */
		BrickGrounded.Sort([&S](const int32& A, const int32& B)
		{
			return S.GetPiece(A).CentreOfMassCm.Y < S.GetPiece(B).CentreOfMassCm.Y;
		});
		Out.BackWall = BrickGrounded[0];
		Out.FrontWall = BrickGrounded[3];

		/* The middle two (side walls) are told apart by X: left smallest, right largest. */
		TArray<int32> Sides = { BrickGrounded[1], BrickGrounded[2] };
		Sides.Sort([&S](const int32& A, const int32& B)
		{
			return S.GetPiece(A).CentreOfMassCm.X < S.GetPiece(B).CentreOfMassCm.X;
		});
		Out.LeftWall = Sides[0];
		Out.RightWall = Sides[1];

		Out.Post = TimberGrounded[0];

		TimberFree.Sort([&S](const int32& A, const int32& B)
		{
			return S.GetPiece(A).CentreOfMassCm.Y < S.GetPiece(B).CentreOfMassCm.Y;
		});
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
 * THE 3D SHED BUILDER LAYS A CLOSED, MULTI-MATERIAL BOX THAT STANDS AS BUILT AND DROPS THE RIGHT
 * PIECE WHEN THE POST OR A WALL IS PULLED — the headline "pull the post, the overhang drops; pull
 * a wall, the roof drops and the box stands on its other three corners."
 *
 * NEEDS A TICKING WORLD: no. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FThreeDShedBuilderTest,
	"DestructionGame.Acceptance.Shed.ThreeD.BuildsAClosedBoxThatStandsAndCollapsesCorrectly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FThreeDShedBuilderTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace ThreeDShedBuilderTestSupport;

	/* Preconditions on the strength basis — the verdicts turn on these, so they are pinned to the
	 * published figures the sizing was derived against rather than read from the profiles. */

	TestEqual(TEXT("FIXTURE: the fixing is a Screw, withdrawal 0.54 MPa (EN 1995-1-1 8.7.2)"),
		Screw.TensileStrengthMPa, 0.54);
	TestEqual(TEXT("FIXTURE: the DryStone bearings are frictional contacts with NO tension"),
		DryStone.TensileStrengthMPa, 0.0);
	TestTrue(TEXT("FIXTURE: GeneralPurposeMortar is a bonded corner bond with cohesion and tension"),
		GeneralPurposeMortar.ShearCohesionMPa > 0.0 && GeneralPurposeMortar.TensileStrengthMPa > 0.0);
	TestEqual(TEXT("FIXTURE: Timber C24 crushes at 29 MPa (mean f_c,0)"),
		Timber.Strength.CompressiveStrengthMPa, 29.0);
	TestEqual(TEXT("FIXTURE: clay brick crushes at 20 MPa"),
		ClayBrick.Strength.CompressiveStrengthMPa, 20.0);

	/* The "neither alone sufficient" sizing, hand-derived: the overhang is C2 with X->Y, the roof
	 * a two-support beam whose centroid sits between its bearings — independent of the builder, a
	 * guard that these chosen dimensions actually produce the intended regimes. */

	const double Wover = OverhangWeightUu();
	const double AssembledTension = AssembledFixingTensionUu();
	const double FixingCap = FixingWithdrawalCapacityUu(Screw.TensileStrengthMPa);
	const double CantDemand = CantileverDemandUuCm();
	const double CantCap = CantileverCapacityUuCm(Screw.TensileStrengthMPa);

	const double Wroof = RoofWeightUu();
	const double RoofTopBackGone = RoofToppleDemandUuCm(RoofFrontBearingCentreYCm);
	const double RoofCapBackGone = RoofToppleCapacityUuCm(RoofBearingWidthYCm);

	AddInfo(FString::Printf(
		TEXT("DERIVED: overhang W %.10g uu (centroid Y %.10g, post %.10g, fixing %.10g); assembled "
			 "tension %.10g vs cap %.10g (%.3gx). cantilever %.10g vs %.10g (%.3gx). roof W %.10g uu "
			 "(centroid Y %.10g); back-gone topple %.10g vs %.10g (%.3gx)."),
		Wover, OverhangCentroidYCm, PostCentreYCm, FixingCentreYCm,
		AssembledTension, FixingCap, FixingCap / AssembledTension,
		CantDemand, CantCap, CantDemand / CantCap,
		Wroof, RoofCentroidYCm,
		RoofTopBackGone, RoofCapBackGone, RoofTopBackGone / RoofCapBackGone));

	TestTrue(TEXT("SIZING (a): the fixing's withdrawal must comfortably exceed the assembled tension"),
		FixingCap > 3.0 * AssembledTension);
	TestTrue(TEXT("SIZING (b): the cantilever moment must outrun the fixing's plastic capacity"),
		CantDemand > 1.5 * CantCap);
	TestTrue(TEXT("SIZING (d): the roof's centroid must sit BETWEEN its two bearings (it stands)"),
		RoofCentroidYCm > RoofBackBearingCentreYCm && RoofCentroidYCm < RoofFrontBearingCentreYCm);
	TestTrue(TEXT("SIZING (e): with the back wall gone the roof's topple moment must outrun its bearing"),
		RoofTopBackGone > 1.5 * RoofCapBackGone);

	/* Arm 0 — the builder lays the 3D shed. Piece counts, per-piece material and grounding, the 3D
	 * flag, and the eight authored joints with their connection profiles. */

	FBrickLayout Layout;
	const bool bBuilt = DestructionShed3D::Build(CanonicalSpec(), Layout);

	TestTrue(TEXT("BUILD: the builder must lay the 3D shed (the stub returns false — this is the RED)"),
		bBuilt);

	TestTrue(TEXT("BUILD: the structure must be flagged 3D so the bridge poses its Y-normal corners"),
		Layout.Structure.IsThreeDimensional());

	TestEqual(TEXT("BUILD: seven pieces — four walls, roof, overhang, post"),
		Layout.Structure.NumPieces(), 7);
	TestEqual(TEXT("BUILD: one box per piece, or AdoptLayout refuses the layout"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());
	TestEqual(TEXT("BUILD: eight joints — four corners, two roof bearings, the fixing, the post bearing"),
		Layout.Structure.NumConnections(), 8);

	FShed S;
	const bool bIdentified = bBuilt && Identify(Layout, S);

	if (!bIdentified)
	{
		AddError(TEXT("BUILD: the builder must lay a 3D shed whose pieces identify by material, "
			"grounding and position — four grounded bricks, one grounded timber post, two free timber "
			"beams. Until the builder is implemented this fails: the stub lays nothing."));
		return false;
	}

	/* --- MATERIALS: the box is multi-material as authored ------------------------------------- */

	TestTrue(TEXT("BUILD: the back wall is ClayBrick"), Layout.Structure.GetPiece(S.BackWall).Material == &ClayBrick);
	TestTrue(TEXT("BUILD: the front wall is ClayBrick"), Layout.Structure.GetPiece(S.FrontWall).Material == &ClayBrick);
	TestTrue(TEXT("BUILD: the left wall is ClayBrick"), Layout.Structure.GetPiece(S.LeftWall).Material == &ClayBrick);
	TestTrue(TEXT("BUILD: the right wall is ClayBrick"), Layout.Structure.GetPiece(S.RightWall).Material == &ClayBrick);
	TestTrue(TEXT("BUILD: the roof beam is Timber"), Layout.Structure.GetPiece(S.Roof).Material == &Timber);
	TestTrue(TEXT("BUILD: the overhang is Timber"), Layout.Structure.GetPiece(S.Overhang).Material == &Timber);
	TestTrue(TEXT("BUILD: the post is Timber"), Layout.Structure.GetPiece(S.Post).Material == &Timber);

	/* --- GROUNDING: the four walls and the post stand on the earth; the two beams do not ------- */

	TestTrue(TEXT("BUILD: the back wall is grounded"), Layout.Structure.GetPiece(S.BackWall).bIsGrounded);
	TestTrue(TEXT("BUILD: the front wall is grounded"), Layout.Structure.GetPiece(S.FrontWall).bIsGrounded);
	TestTrue(TEXT("BUILD: the left wall is grounded"), Layout.Structure.GetPiece(S.LeftWall).bIsGrounded);
	TestTrue(TEXT("BUILD: the right wall is grounded"), Layout.Structure.GetPiece(S.RightWall).bIsGrounded);
	TestTrue(TEXT("BUILD: the post is grounded"), Layout.Structure.GetPiece(S.Post).bIsGrounded);
	TestFalse(TEXT("BUILD: the roof is not grounded"), Layout.Structure.GetPiece(S.Roof).bIsGrounded);
	TestFalse(TEXT("BUILD: the overhang is not grounded"), Layout.Structure.GetPiece(S.Overhang).bIsGrounded);

	TestTrue(TEXT("BUILD: the laid shed knows where every piece and joint is, or every moment is silently zero"),
		Layout.Structure.HasCompleteGeometry());

	/* --- JOINTS: the eight contacts exist, each with its authored connection ------------------ */

	const int32 CornerBackLeft = JointBetween(Layout.Structure, S.BackWall, S.LeftWall);
	const int32 CornerBackRight = JointBetween(Layout.Structure, S.BackWall, S.RightWall);
	const int32 CornerFrontLeft = JointBetween(Layout.Structure, S.FrontWall, S.LeftWall);
	const int32 CornerFrontRight = JointBetween(Layout.Structure, S.FrontWall, S.RightWall);
	const int32 RoofBack = JointBetween(Layout.Structure, S.BackWall, S.Roof);
	const int32 RoofFront = JointBetween(Layout.Structure, S.FrontWall, S.Roof);
	const int32 Fixing = JointBetween(Layout.Structure, S.FrontWall, S.Overhang);
	const int32 PostBrg = JointBetween(Layout.Structure, S.Post, S.Overhang);

	TestTrue(TEXT("BUILD: BackWall - LeftWall is a corner joint"), CornerBackLeft != INDEX_NONE);
	TestTrue(TEXT("BUILD: BackWall - RightWall is a corner joint"), CornerBackRight != INDEX_NONE);
	TestTrue(TEXT("BUILD: FrontWall - LeftWall is a corner joint"), CornerFrontLeft != INDEX_NONE);
	TestTrue(TEXT("BUILD: FrontWall - RightWall is a corner joint"), CornerFrontRight != INDEX_NONE);
	TestTrue(TEXT("BUILD: BackWall - Roof is a bearing"), RoofBack != INDEX_NONE);
	TestTrue(TEXT("BUILD: FrontWall - Roof is a bearing"), RoofFront != INDEX_NONE);
	TestTrue(TEXT("BUILD: FrontWall - Overhang is the fixing"), Fixing != INDEX_NONE);
	TestTrue(TEXT("BUILD: Post - Overhang is the post bearing"), PostBrg != INDEX_NONE);

	if (CornerBackLeft == INDEX_NONE || CornerBackRight == INDEX_NONE || CornerFrontLeft == INDEX_NONE
		|| CornerFrontRight == INDEX_NONE || RoofBack == INDEX_NONE || RoofFront == INDEX_NONE
		|| Fixing == INDEX_NONE || PostBrg == INDEX_NONE)
	{
		AddError(TEXT("BUILD: the eight authored joints must all exist before their roles can be read"));
		return false;
	}

	/* The corners are bonded mortar; the roof/post bearings are compression-only DryStone; the
	 * fixing is a tension-capable screw. That distinction is the whole cross-material authoring. */
	TestEqual(TEXT("BUILD: the back-left corner is GeneralPurposeMortar"),
		Layout.Structure.GetConnection(CornerBackLeft).Strength.CompressiveStrengthMPa,
		GeneralPurposeMortar.CompressiveStrengthMPa);
	TestEqual(TEXT("BUILD: the front-right corner is GeneralPurposeMortar"),
		Layout.Structure.GetConnection(CornerFrontRight).Strength.CompressiveStrengthMPa,
		GeneralPurposeMortar.CompressiveStrengthMPa);
	TestEqual(TEXT("BUILD: the back roof bearing is DryStone (no tension)"),
		Layout.Structure.GetConnection(RoofBack).Strength.TensileStrengthMPa, 0.0);
	TestEqual(TEXT("BUILD: the front roof bearing is DryStone (no tension)"),
		Layout.Structure.GetConnection(RoofFront).Strength.TensileStrengthMPa, 0.0);
	TestEqual(TEXT("BUILD: the post bearing is DryStone (no tension)"),
		Layout.Structure.GetConnection(PostBrg).Strength.TensileStrengthMPa, 0.0);
	TestEqual(TEXT("BUILD: the fixing is a Screw — a tension tie"),
		Layout.Structure.GetConnection(Fixing).Strength.TensileStrengthMPa, Screw.TensileStrengthMPa);

	/* THE GENUINELY-3D FEATURE: the corner joint faces are OUT OF THE X-Z PLANE, |normal.Y| ~ 1.
	 * A 2D X-Z oracle cannot express these; the whole point of the 3D flag is to pose them. */
	TestTrue(
		*FString::Printf(TEXT("BUILD: the back-left corner normal is out of plane, |Y| ~ 1 (got %g,%g,%g)"),
			Layout.Structure.GetConnection(CornerBackLeft).InterfaceNormal.X,
			Layout.Structure.GetConnection(CornerBackLeft).InterfaceNormal.Y,
			Layout.Structure.GetConnection(CornerBackLeft).InterfaceNormal.Z),
		FMath::IsNearlyEqual(FMath::Abs(Layout.Structure.GetConnection(CornerBackLeft).InterfaceNormal.Y), 1.0, 1.0e-9));
	TestTrue(TEXT("BUILD: each corner joint is the 20 x 180 = 3600 cm2 wall face"),
		FMath::IsNearlyEqual(Layout.Structure.GetConnection(CornerBackLeft).InterfaceAreaSqCm, CornerAreaSqCm, 1.0e-6));

	/* The roof and overhang bear/hang through BED joints beneath them (+Z normal). */
	TestTrue(TEXT("BUILD: the roof bears on the front wall through a bed joint"),
		Layout.Structure.GetJointRole(RoofFront, S.Roof) == EJointRole::BedBeneath);
	TestTrue(TEXT("BUILD: the fixing is a bed joint under the overhang's back end"),
		Layout.Structure.GetJointRole(Fixing, S.Overhang) == EJointRole::BedBeneath);
	TestTrue(TEXT("BUILD: the post bears under the overhang through a bed joint"),
		Layout.Structure.GetJointRole(PostBrg, S.Overhang) == EJointRole::BedBeneath);

	TestEqual(TEXT("BUILD: the fixing patch is the 4 cm x 20 cm lap, 80 cm2"),
		Layout.Structure.GetConnection(Fixing).InterfaceAreaSqCm, FixingAreaSqCm);
	TestEqual(TEXT("BUILD: the post bearing is the 12 cm x 20 cm face, 240 cm2"),
		Layout.Structure.GetConnection(PostBrg).InterfaceAreaSqCm, PostBearingAreaSqCm);

	/* The stands-and-falls arms. Each rebuilds a fresh shed, pulls the arm's piece, then reads the
	 * 3D oracle mechanism and the production outcome — mechanism (feasibility, the mechanism's
	 * moving blocks) and outcome (Supported vs Falling, Stranded == 0), never displacement. */

	enum class EArm : uint8 { Assembled, PostRemoved, BackWallRemoved };

	struct FArm
	{
		EArm Arm;
		const TCHAR* Label;
		bool bExpectStands;
	};

	const FArm Arms[3] = {
		{ EArm::Assembled,       TEXT("(0) assembled"),          true },
		{ EArm::PostRemoved,     TEXT("(1) post removed"),       false },
		{ EArm::BackWallRemoved, TEXT("(2) back wall removed"),  false },
	};

	for (const FArm& A : Arms)
	{
		FBrickLayout Fresh;
		if (!DestructionShed3D::Build(CanonicalSpec(), Fresh))
		{
			AddError(FString::Printf(TEXT("%s: the builder must lay the 3D shed"), A.Label));
			return false;
		}

		FShed F;
		if (!Identify(Fresh, F))
		{
			AddError(FString::Printf(TEXT("%s: the laid 3D shed must identify"), A.Label));
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
		case EArm::BackWallRemoved:
			Fresh.Structure.RemovePiece(F.BackWall);
			ExpectedFalling = { F.Roof };
			/* Corner bracing: the other three walls are each independently grounded, so the box does
			 * NOT fully collapse — the roof loses a bearing, the surviving walls stand. */
			ExpectedStanding = { F.Overhang, F.LeftWall, F.RightWall, F.FrontWall };
			break;
		}

		/* ---- THE MECHANISM, VIA THE 3D ORACLE. ---- */
		RigidBlockOracle::FOracleProblem Problem;
		FString BridgeWhy;
		const bool bBridged = RigidBlockOracle::BuildRigidBlockProblem(Fresh.Structure, Problem, BridgeWhy);

		TestTrue(
			*FString::Printf(TEXT("%s: the oracle bridge must accept this 3D shed (%s)"), A.Label, *BridgeWhy),
			bBridged);

		if (bBridged)
		{
			TestEqual(
				*FString::Printf(TEXT("%s: the bridged problem is posed in 3D"), A.Label),
				static_cast<int32>(Problem.Dim), static_cast<int32>(RigidBlockOracle::EOracleDim::Dim3D));

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

		/* ---- THE OUTCOME, VIA PRODUCTION. Below the block cap, the LP is the break authority. ---- */
		const int32 Passes = Fresh.Structure.SolveAndBreak();
		const int32 Stranded = StrandedCount(Fresh.Structure);

		AddInfo(FString::Printf(TEXT("%s: PRODUCTION ran %d pass(es); %d stranded"), A.Label, Passes, Stranded));

		TestEqual(
			*FString::Printf(TEXT("%s: nothing may be Stranded — the verdict must be about the shed, not the "
				"solver declining to route"), A.Label),
			Stranded, 0);

		/* The grounded survivors keep the earth in every arm — the box's corner bracing. */
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
