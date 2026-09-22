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
 * The 3D shed builder (THREED_DESIGN.md E1-E3). DestructionShed3D::Build lays a closed box of four
 * grounded brick walls with Y-normal mortar corners, a timber roof beam on the back and front
 * walls, and a timber overhang screwed to the front wall and carried on a grounded post. Flagged
 * 3D and bridged to the 3D LP: assembled it stands, pulling the post drops the overhang, and
 * pulling the back wall drops the roof while the other walls stand. No new physics; it composes
 * the proven 2D shed mechanisms (C2) into one 3D structure.
 *
 * Fixture: X width, Y depth, Z height, cm. Seven pieces, eight joints.
 *
 * Plan view (looking down -Z; X right, Y up the page):
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
 * Pieces: walls grounded brick, Z[0,180]; back Y[0,20], front Y[200,220], sides X[0,20] and
 * X[200,220] at Y[21,199]. Roof timber X[40,180] Y[10,210] Z[181,193], centroid Y 110. Overhang
 * timber X[100,120] Y[216,416], lapping the front wall 4 cm, centroid Y 316. Post grounded timber
 * Y[274,286].
 *
 * Joints: four Y-normal mortar corners (3600 cm2, out of the X-Z plane, hence the 3D flag); two
 * DryStone roof bearings (1400 cm2); a Screw fixing front wall to overhang (80 cm2); a DryStone
 * post bearing (240 cm2). Roof and overhang are 6 cm apart and never join.
 *
 * Statics, hand-derived (the LP is the independent second check):
 *   (a) Assembled overhang: fixing tension W*(cY-Yp)/(Yp-Yf), ~37x under withdrawal capacity.
 *   (b) Post removed: the fixing's plastic couple is ~2.1x short of the cantilever moment. Falls.
 *   (d) Assembled roof: centroid between the bearings (15 and 205). Stands.
 *   (e) Back wall removed: roof on the front bearing alone, centroid 95 cm outboard, ~19x over the
 *       compression-only couple. Falls; the other walls stand.
 *
 * Units restated locally (DESIGN.md §3): 1 MPa over 1 cm2 = 10000 uu, not the production constant,
 * so a wrong conversion fails. Weight = MassKg * 980. World-free.
 */
namespace ThreeDShedBuilderTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/** Timber C24 mean density, g/cm3. Units trap: 0.42, not 420. */
	constexpr double TimberDensityGramsPerCubicCm = 0.42;

	/** Fired clay density, g/cm3. */
	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** MassKg * 980 is a weight in uu; the 1 N = 100 uu factor is included. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** 1 MPa over 1 cm2 = 10000 uu. A local literal on purpose, not the production constant. */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	// The canonical spec, shared by the builder call and the hand derivation.

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

	// Derived coordinates.

	constexpr double WallTopZCm = WallHeightCm;                                       // 180
	constexpr double BeamBottomZCm = WallHeightCm + JointThicknessCm;                 // 181

	// Roof beam spanning Y between the back and front wall tops.
	constexpr double RoofWidthXCm = BoxWidthXCm - 2.0 * RoofInsetXCm;                 // 140
	constexpr double RoofBackYCm = WallThicknessCm - RoofBearingLapYCm;               // 10
	constexpr double RoofFrontYCm = BoxDepthYCm - WallThicknessCm + RoofBearingLapYCm;// 210
	constexpr double RoofLengthYCm = RoofFrontYCm - RoofBackYCm;                      // 200
	constexpr double RoofCentroidYCm = (RoofBackYCm + RoofFrontYCm) / 2.0;            // 110

	// Roof bearings: the roof's Y overlap with each wall top.
	constexpr double RoofBackBearingCentreYCm = (RoofBackYCm + WallThicknessCm) / 2.0;             // 15
	constexpr double RoofFrontBearingCentreYCm =
		((BoxDepthYCm - WallThicknessCm) + RoofFrontYCm) / 2.0;                                    // 205
	constexpr double RoofBearingWidthYCm = RoofBearingLapYCm;                                       // 10

	// Overhang lapping the front wall top and cantilevering in +Y.
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

	// Independent statics: the 2D shed's overhang and roof derivations with X swapped for Y.

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

	/** The fixing's withdrawal capacity, uu: f_t over the whole patch. */
	double FixingWithdrawalCapacityUu(double ScrewTensileMPa)
	{
		return ScrewTensileMPa * ForceUnitsPerMPaSqCmHere * FixingAreaSqCm;
	}

	/** (a) Assembled: tension in the fixing from the weight outboard of the post. */
	double AssembledFixingTensionUu()
	{
		return OverhangWeightUu()
			* (OverhangCentroidYCm - PostCentreYCm) / (PostCentreYCm - FixingCentreYCm);
	}

	/** (b) Post removed: cantilever moment about the fixing centre. */
	double CantileverDemandUuCm()
	{
		return OverhangWeightUu() * (OverhangCentroidYCm - FixingCentreYCm);
	}

	/** (b) The fixing patch's fully plastic couple (C2's derivation). */
	double CantileverCapacityUuCm(double ScrewTensileMPa)
	{
		const double HalfWidthCm = FixingLapYCm / 2.0;
		const double CapHalfUu = ScrewTensileMPa * ForceUnitsPerMPaSqCmHere * (FixingAreaSqCm / 2.0);
		return HalfWidthCm * (OverhangWeightUu() + 2.0 * CapHalfUu);
	}

	/** (e) Back wall removed: roof toppling moment about the remaining bearing. */
	double RoofToppleDemandUuCm(double RemainingBearingCentreYCm)
	{
		return RoofWeightUu() * FMath::Abs(RemainingBearingCentreYCm - RoofCentroidYCm);
	}

	/** (e) That bearing's compression-only restoring couple: halfWidth * W. */
	double RoofToppleCapacityUuCm(double BearingWidthYCm)
	{
		return (BearingWidthYCm / 2.0) * RoofWeightUu();
	}

	// Pieces are identified by material, grounding and position, not by handle order.

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
	 * Name the seven pieces, or fail. Needs four grounded bricks, one grounded timber and two free
	 * timbers; walls sorted by Y then X, beams by Y centroid.
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

		// Back is smallest Y, front largest.
		BrickGrounded.Sort([&S](const int32& A, const int32& B)
		{
			return S.GetPiece(A).CentreOfMassCm.Y < S.GetPiece(B).CentreOfMassCm.Y;
		});
		Out.BackWall = BrickGrounded[0];
		Out.FrontWall = BrickGrounded[3];

		// Side walls by X.
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

	/** True when a live piece has no path to the earth. */
	bool HasLostTheEarth(const FStructure& S, int32 Piece)
	{
		if (S.IsPieceRemoved(Piece))
		{
			return false;
		}
		return !IsStanding(S.GetPieceSupport(Piece));
	}

	/** The oracle block for an FStructure piece. */
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

/** The 3D shed stands as built; pulling the post drops the overhang, pulling the back wall drops the roof. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FThreeDShedBuilderTest,
	"DestructionGame.Acceptance.Shed.ThreeD.BuildsAClosedBoxThatStandsAndCollapsesCorrectly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FThreeDShedBuilderTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace ThreeDShedBuilderTestSupport;

	// Pin the published strengths the sizing was derived from.

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

	// Hand-derived sizing, independent of the builder: the dimensions give the intended regimes.

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

	// The builder lays the 3D shed: counts, materials, grounding, 3D flag, joints.

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

	// Materials.

	TestTrue(TEXT("BUILD: the back wall is ClayBrick"), Layout.Structure.GetPiece(S.BackWall).Material == &ClayBrick);
	TestTrue(TEXT("BUILD: the front wall is ClayBrick"), Layout.Structure.GetPiece(S.FrontWall).Material == &ClayBrick);
	TestTrue(TEXT("BUILD: the left wall is ClayBrick"), Layout.Structure.GetPiece(S.LeftWall).Material == &ClayBrick);
	TestTrue(TEXT("BUILD: the right wall is ClayBrick"), Layout.Structure.GetPiece(S.RightWall).Material == &ClayBrick);
	TestTrue(TEXT("BUILD: the roof beam is Timber"), Layout.Structure.GetPiece(S.Roof).Material == &Timber);
	TestTrue(TEXT("BUILD: the overhang is Timber"), Layout.Structure.GetPiece(S.Overhang).Material == &Timber);
	TestTrue(TEXT("BUILD: the post is Timber"), Layout.Structure.GetPiece(S.Post).Material == &Timber);

	// Grounding.

	TestTrue(TEXT("BUILD: the back wall is grounded"), Layout.Structure.GetPiece(S.BackWall).bIsGrounded);
	TestTrue(TEXT("BUILD: the front wall is grounded"), Layout.Structure.GetPiece(S.FrontWall).bIsGrounded);
	TestTrue(TEXT("BUILD: the left wall is grounded"), Layout.Structure.GetPiece(S.LeftWall).bIsGrounded);
	TestTrue(TEXT("BUILD: the right wall is grounded"), Layout.Structure.GetPiece(S.RightWall).bIsGrounded);
	TestTrue(TEXT("BUILD: the post is grounded"), Layout.Structure.GetPiece(S.Post).bIsGrounded);
	TestFalse(TEXT("BUILD: the roof is not grounded"), Layout.Structure.GetPiece(S.Roof).bIsGrounded);
	TestFalse(TEXT("BUILD: the overhang is not grounded"), Layout.Structure.GetPiece(S.Overhang).bIsGrounded);

	TestTrue(TEXT("BUILD: the laid shed knows where every piece and joint is, or every moment is silently zero"),
		Layout.Structure.HasCompleteGeometry());

	// Joints.

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

	// Corners mortar, bearings DryStone (no tension), fixing Screw.
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

	// Corner normals are out of the X-Z plane (|Y| ~ 1), which a 2D oracle cannot express.
	TestTrue(
		*FString::Printf(TEXT("BUILD: the back-left corner normal is out of plane, |Y| ~ 1 (got %g,%g,%g)"),
			Layout.Structure.GetConnection(CornerBackLeft).InterfaceNormal.X,
			Layout.Structure.GetConnection(CornerBackLeft).InterfaceNormal.Y,
			Layout.Structure.GetConnection(CornerBackLeft).InterfaceNormal.Z),
		FMath::IsNearlyEqual(FMath::Abs(Layout.Structure.GetConnection(CornerBackLeft).InterfaceNormal.Y), 1.0, 1.0e-9));
	TestTrue(TEXT("BUILD: each corner joint is the 20 x 180 = 3600 cm2 wall face"),
		FMath::IsNearlyEqual(Layout.Structure.GetConnection(CornerBackLeft).InterfaceAreaSqCm, CornerAreaSqCm, 1.0e-6));

	// Roof and overhang sit on bed joints beneath them.
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

	/*
	 * Each arm rebuilds the shed, pulls a piece, and checks the 3D oracle's mechanism and the
	 * production outcome (support, Stranded == 0). Never displacement.
	 */

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

		// Pieces expected to fall and to stand.
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
			// The other walls are grounded, so only the roof falls.
			ExpectedStanding = { F.Overhang, F.LeftWall, F.RightWall, F.FrontWall };
			break;
		}

		// Mechanism, via the 3D oracle.
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

				// The collapse mechanism (gravity dead) must name each falling piece as moving.
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

		// Outcome, via production. Below the block cap the LP decides breaks.
		const int32 Passes = Fresh.Structure.SolveAndBreak();
		const int32 Stranded = StrandedCount(Fresh.Structure);

		AddInfo(FString::Printf(TEXT("%s: PRODUCTION ran %d pass(es); %d stranded"), A.Label, Passes, Stranded));

		TestEqual(
			*FString::Printf(TEXT("%s: nothing may be Stranded — the verdict must be about the shed, not the "
				"solver declining to route"), A.Label),
			Stranded, 0);

		// Survivors stay held up.
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
