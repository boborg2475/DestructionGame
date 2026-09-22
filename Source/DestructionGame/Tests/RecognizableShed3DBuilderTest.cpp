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
 * DestructionShed3D::BuildRecognizable lays an axis-aligned multi-material shed that reads as a shed:
 * four walls, a door (brick piers, Timber lintel), a window (sill, jambs, Timber lintel), stepped brick
 * gables to a ridge, a Timber purlin roof, and a Timber porch on two posts and a wall tie. As a 3D
 * FBrickLayout bridged to the 3D LP, the assembled shed stands. This test covers shape and standing;
 * the collapse test below covers pulling a porch post.
 *
 * The canonical shed, 24 pieces and 30 joints, cm. X width, Y depth (front is +Y), Z height.
 * Footprint X[0,300] Y[0,300]; walls 25 thick; eaves at Z=200; joints 1 cm. Front and back walls
 * are the gable ends; left and right are eaves walls. Door in the front wall, window in the left.
 *
 * Pieces (X[lo,hi] Y[lo,hi] Z[lo,hi]; G = grounded):
 *
 *   WALLS / DOOR (front gable base, Z < 200):
 *     BackWall     ClayBrick G  X[0,300]   Y[0,25]    Z[0,200]     the back gable end, solid.
 *     RightWall    ClayBrick G  X[275,300] Y[26,274]  Z[0,200]     the right eaves wall, solid.
 *     LeftPier     ClayBrick G  X[0,110]   Y[275,300] Z[0,175]     door jamb, left of the doorway.
 *     RightPier    ClayBrick G  X[190,300] Y[275,300] Z[0,175]     door jamb, right of the doorway.
 *     DoorHeader   Timber       X[0,300]   Y[275,300] Z[176,200]   the door LINTEL, bearing on both piers.
 *                                                                  DOOR GAP: X[110,190], Z[0,175].
 *
 *   WINDOW (left wall, X[0,25]):
 *     Sill         ClayBrick G  X[0,25]    Y[26,274]  Z[0,89]      the sill course, full length.
 *     WinJambBack  ClayBrick    X[0,25]    Y[26,120]  Z[90,170]    window jamb toward the back.
 *     WinJambFront ClayBrick    X[0,25]    Y[180,274] Z[90,170]    window jamb toward the front.
 *     WinLintel    Timber       X[0,25]    Y[26,274]  Z[171,200]   the window LINTEL, on both jambs.
 *                                                                  WINDOW GAP: Y[120,180], Z[90,170].
 *
 *   GABLES (brick, symmetric centred courses, Z >= 201):
 *     FGableBase   ClayBrick    X[0,300]   Y[275,300] Z[201,230]   front gable course 1 (full width).
 *     FGableMid    ClayBrick    X[75,225]  Y[275,300] Z[231,260]   front gable course 2 (narrower).
 *     FGableApex   ClayBrick    X[125,175] Y[275,300] Z[261,290]   front gable apex (ridge block).
 *     BGableBase   ClayBrick    X[0,300]   Y[0,25]    Z[201,230]   back gable course 1.
 *     BGableMid    ClayBrick    X[75,225]  Y[0,25]    Z[231,260]   back gable course 2.
 *     BGableApex   ClayBrick    X[125,175] Y[0,25]    Z[261,290]   back gable apex.
 *
 *   ROOF (Timber purlins + ridge, each spanning Y[0,300] on both gable shoulders):
 *     EavesPurlinL Timber       X[0,75]    Y[0,300]   Z[231,255]   on the Z=230 base shoulders.
 *     EavesPurlinR Timber       X[225,300] Y[0,300]   Z[231,255]
 *     MidPurlinL   Timber       X[75,125]  Y[0,300]   Z[261,285]   on the Z=260 mid shoulders.
 *     MidPurlinR   Timber       X[175,225] Y[0,300]   Z[261,285]
 *     Ridge        Timber       X[125,175] Y[0,300]   Z[291,315]   on the Z=290 apex tops.
 *
 *   PORCH (Timber, over the door, +Y), a cantilever tied at the back by a narrow central cleat:
 *     PostL        Timber    G  X[55,85]   Y[328,352] Z[0,200]     grounded porch post, left  (Xc 70).
 *     PostR        Timber    G  X[215,245] Y[328,352] Z[0,200]     grounded porch post, right (Xc 230).
 *     Overhang     Timber       X[40,260]  Y[301,451] Z[201,221]   porch roof; centroid (150,376,211),
 *                                                                  FORWARD of the post line (Y=340).
 *     Cleat        Timber       X[140,160] Y[301,310] Z[176,200]   a 20 cm central bracket, 1 cm off the
 *                                                                  wall, projecting to Y=310; the overhang
 *                                                                  laps ONLY this (Z-normal tie, 180 cm2).
 *
 * Joints (all via MakeInterface):
 *   DOOR     (2): LeftPier-DoorHeader, RightPier-DoorHeader                          bed, DryStone.
 *   WINDOW   (4): Sill-WinJambBack, Sill-WinJambFront, WinJambBack-WinLintel,
 *                 WinJambFront-WinLintel                                             bed, DryStone.
 *   CORNERS  (4): Sill-BackWall, Sill-LeftPier, RightWall-BackWall, RightWall-RightPier
 *                 (grounded-grounded, so the oracle skips them).
 *   GABLES   (6): FGableBase-DoorHeader, FGableBase-FGableMid, FGableMid-FGableApex,
 *                 BGableBase-BackWall, BGableBase-BGableMid, BGableMid-BGableApex     bed, mortar.
 *   ROOF    (10): each of the five purlins to the BACK and FRONT gable shoulder it rests on
 *                                                                                     bed, DryStone.
 *   PORCH    (4): PostL-Overhang, PostR-Overhang (bed, DryStone); Cleat-Overhang (the tie, Z-normal,
 *                 Screw, 180 cm2); DoorHeader-Cleat (the anchor, Y-normal, mortar, 480 cm2).
 *
 * Why it stands (hand-derived, not read from the LP):
 *   - Gables are centred at X=150 and narrow symmetrically (300 -> 150 -> 50); purlins load them
 *     symmetrically, so there is no net overturning moment.
 *   - Door and window lintels have their centroid between their supports, so both reactions are positive.
 *   - The porch overhang's centroid (Y=376) is forward of the posts (Y=340), so the cleat tie holds its
 *     back down in withdrawal, well inside the Screw's 0.54 MPa over 180 cm2. The posts are symmetric
 *     about X=150, so no couple is demanded of the tie.
 *
 * Units spelled out locally (DESIGN.md §3): 1 MPa over 1 cm2 is 10000 uu; weight is MassKg * 980.
 * World-free. Named namespace because unity builds merge translation units.
 */
namespace RecognizableShed3DTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/** Timber C24, EN 338 mean density. g/cm3: 0.42, never 420. */
	constexpr double TimberDensityGramsPerCubicCm = 0.42;

	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** MassKg * 980 is already a weight in uu; do not apply 1 N = 100 uu again. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** 1 MPa over 1 cm2 is 10000 uu. A local literal, not the production constant. */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	// The canonical shed's numbers; pieces are identified by position in the laid layout.

	constexpr int32 ExpectedPieces = 24;
	constexpr int32 ExpectedJoints = 30;
	constexpr int32 ExpectedGrounded = 7;    // BackWall, RightWall, LeftPier, RightPier, Sill, PostL, PostR
	constexpr int32 ExpectedBrick = 13;
	constexpr int32 ExpectedTimber = 11;     // + the Cleat (the wall tie); the cleat is NOT grounded

	// Points inside the door and window openings; no piece may contain them.
	constexpr double DoorGapX = 150.0, DoorGapY = 287.5, DoorGapZ = 75.0;
	constexpr double WindowGapX = 12.5, WindowGapY = 150.0, WindowGapZ = 130.0;

	// Centroids of the named pieces.
	const FVector CBackWall(150.0, 12.5, 100.0);
	const FVector CRightWall(287.5, 150.0, 100.0);
	const FVector CLeftPier(55.0, 287.5, 87.5);
	const FVector CRightPier(245.0, 287.5, 87.5);
	const FVector CDoorHeader(150.0, 287.5, 188.0);
	const FVector CCleat(150.0, 305.0, 188.0);
	const FVector CSill(12.5, 150.0, 44.5);
	const FVector CWinJambBack(12.5, 73.0, 130.0);
	const FVector CWinJambFront(12.5, 227.0, 130.0);
	const FVector CWinLintel(12.5, 150.0, 185.5);
	const FVector CFGableBase(150.0, 287.5, 215.5);
	const FVector CFGableMid(150.0, 287.5, 245.5);
	const FVector CFGableApex(150.0, 287.5, 275.5);
	const FVector CBGableApex(150.0, 12.5, 275.5);
	const FVector CRidge(150.0, 150.0, 303.0);
	const FVector CPostL(70.0, 340.0, 100.0);
	const FVector CPostR(230.0, 340.0, 100.0);
	const FVector COverhang(150.0, 376.0, 211.0);

	// Hand-derived controls, independent of the builder.

	double BoxWeightUu(const FVector& Ext /*half extents cm*/, double DensityGramsPerCubicCm)
	{
		const double VolumeCm3 = (2.0 * Ext.X) * (2.0 * Ext.Y) * (2.0 * Ext.Z);
		return DensityGramsPerCubicCm * VolumeCm3 / 1000.0 * GravityCmPerSecondSquared;
	}

	/*
	 * Simply-supported beam: point load w at p on supports a and b gives R_a = w*(b-p)/(b-a),
	 * R_b = w*(p-a)/(b-a), both positive iff a < p < b.
	 */
	void BeamReactions(double p, double a, double b, double w, double& OutRa, double& OutRb)
	{
		OutRa = w * (b - p) / (b - a);
		OutRb = w * (p - a) / (b - a);
	}

	// Named pieces of the laid shed, found by position rather than handle order.
	struct FShed
	{
		int32 BackWall = INDEX_NONE;
		int32 RightWall = INDEX_NONE;
		int32 LeftPier = INDEX_NONE;
		int32 RightPier = INDEX_NONE;
		int32 DoorHeader = INDEX_NONE;
		int32 Cleat = INDEX_NONE;
		int32 Sill = INDEX_NONE;
		int32 WinJambBack = INDEX_NONE;
		int32 WinJambFront = INDEX_NONE;
		int32 WinLintel = INDEX_NONE;
		int32 FGableBase = INDEX_NONE;
		int32 FGableMid = INDEX_NONE;
		int32 FGableApex = INDEX_NONE;
		int32 BGableApex = INDEX_NONE;
		int32 Ridge = INDEX_NONE;
		int32 PostL = INDEX_NONE;
		int32 PostR = INDEX_NONE;
		int32 Overhang = INDEX_NONE;
	};

	/** The live piece whose box contains the point, or INDEX_NONE. */
	int32 PieceContaining(const FBrickLayout& Layout, const FVector& P)
	{
		const FStructure& S = Layout.Structure;
		for (int32 Piece = 0; Piece < S.NumPieces(); ++Piece)
		{
			if (S.IsPieceRemoved(Piece) || !Layout.Boxes.IsValidIndex(Piece))
			{
				continue;
			}
			const FPieceBox& B = Layout.Boxes[Piece];
			const FVector Lo = B.CentreCm - B.ExtentCm;
			const FVector Hi = B.CentreCm + B.ExtentCm;
			if (P.X >= Lo.X && P.X <= Hi.X && P.Y >= Lo.Y && P.Y <= Hi.Y && P.Z >= Lo.Z && P.Z <= Hi.Z)
			{
				return Piece;
			}
		}
		return INDEX_NONE;
	}

	/** Finds every named piece by centroid; false if any is missing. */
	bool Identify(const FBrickLayout& Layout, FShed& Out)
	{
		Out.BackWall = PieceContaining(Layout, CBackWall);
		Out.RightWall = PieceContaining(Layout, CRightWall);
		Out.LeftPier = PieceContaining(Layout, CLeftPier);
		Out.RightPier = PieceContaining(Layout, CRightPier);
		Out.DoorHeader = PieceContaining(Layout, CDoorHeader);
		Out.Cleat = PieceContaining(Layout, CCleat);
		Out.Sill = PieceContaining(Layout, CSill);
		Out.WinJambBack = PieceContaining(Layout, CWinJambBack);
		Out.WinJambFront = PieceContaining(Layout, CWinJambFront);
		Out.WinLintel = PieceContaining(Layout, CWinLintel);
		Out.FGableBase = PieceContaining(Layout, CFGableBase);
		Out.FGableMid = PieceContaining(Layout, CFGableMid);
		Out.FGableApex = PieceContaining(Layout, CFGableApex);
		Out.BGableApex = PieceContaining(Layout, CBGableApex);
		Out.Ridge = PieceContaining(Layout, CRidge);
		Out.PostL = PieceContaining(Layout, CPostL);
		Out.PostR = PieceContaining(Layout, CPostR);
		Out.Overhang = PieceContaining(Layout, COverhang);

		const int32 All[] = {
			Out.BackWall, Out.RightWall, Out.LeftPier, Out.RightPier, Out.DoorHeader, Out.Cleat, Out.Sill,
			Out.WinJambBack, Out.WinJambFront, Out.WinLintel, Out.FGableBase, Out.FGableMid,
			Out.FGableApex, Out.BGableApex, Out.Ridge, Out.PostL, Out.PostR, Out.Overhang };

		for (const int32 P : All)
		{
			if (P == INDEX_NONE)
			{
				return false;
			}
		}
		return true;
	}

	int32 JointBetween(const FStructure& S, int32 PieceA, int32 PieceB)
	{
		for (int32 Joint = 0; Joint < S.NumConnections(); ++Joint)
		{
			const FConnection& C = S.GetConnection(Joint);
			if ((C.PieceA == PieceA && C.PieceB == PieceB) || (C.PieceA == PieceB && C.PieceB == PieceA))
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

	int32 CountMaterial(const FStructure& S, const FMaterialProfile* Material, bool bGroundedFilter, bool bWantGrounded)
	{
		int32 N = 0;
		for (int32 Piece = 0; Piece < S.NumPieces(); ++Piece)
		{
			if (S.IsPieceRemoved(Piece))
			{
				continue;
			}
			const FStructurePiece& P = S.GetPiece(Piece);
			if (P.Material != Material)
			{
				continue;
			}
			if (bGroundedFilter && P.bIsGrounded != bWantGrounded)
			{
				continue;
			}
			++N;
		}
		return N;
	}

	/** True when a live piece has lost every path to the ground. */
	bool HasLostTheEarth(const FStructure& S, int32 Piece)
	{
		if (S.IsPieceRemoved(Piece))
		{
			return false;
		}
		return !IsStanding(S.GetPieceSupport(Piece));
	}

	/** The oracle block built from a given FStructure piece. */
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

/** The recognizable shed lays with the expected shape and stands as built. See the file header. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRecognizableShed3DBuilderTest,
	"DestructionGame.Acceptance.Shed.ThreeD.RecognizableShedStandsAsBuilt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRecognizableShed3DBuilderTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace RecognizableShed3DTestSupport;

	// Strength basis pinned to published figures, not read from the profiles.

	TestEqual(TEXT("FIXTURE: the porch fixing is a Screw, withdrawal 0.54 MPa (EN 1995-1-1 8.7.2)"),
		Screw.TensileStrengthMPa, 0.54);
	TestEqual(TEXT("FIXTURE: the bearings are DryStone frictional contacts with NO tension"),
		DryStone.TensileStrengthMPa, 0.0);
	TestTrue(TEXT("FIXTURE: GeneralPurposeMortar is a bonded joint with cohesion and tension"),
		GeneralPurposeMortar.ShearCohesionMPa > 0.0 && GeneralPurposeMortar.TensileStrengthMPa > 0.0);
	TestEqual(TEXT("FIXTURE: Timber C24 crushes at 29 MPa (mean f_c,0)"),
		Timber.Strength.CompressiveStrengthMPa, 29.0);
	TestEqual(TEXT("FIXTURE: clay brick crushes at 20 MPa"),
		ClayBrick.Strength.CompressiveStrengthMPa, 20.0);

	// Hand-derived controls: lintel reactions and the porch centroid, independent of the builder.

	// Door lintel (Timber): X[0,300] Y[275,300] Z[176,200], on piers at X-centroid 55 and 245.
	const double WDoorHeader = BoxWeightUu(FVector(150.0, 12.5, 12.0), TimberDensityGramsPerCubicCm);
	double DoorRa = 0.0, DoorRb = 0.0;
	BeamReactions(150.0, 55.0, 245.0, WDoorHeader, DoorRa, DoorRb);

	// Window lintel (Timber): X[0,25] Y[26,274] Z[171,200], on jambs at Y-centroid 73 and 227.
	const double WWinLintel = BoxWeightUu(FVector(12.5, 124.0, 14.5), TimberDensityGramsPerCubicCm);
	double WinRa = 0.0, WinRb = 0.0;
	BeamReactions(150.0, 73.0, 227.0, WWinLintel, WinRa, WinRb);

	AddInfo(FString::Printf(
		TEXT("DERIVED: door lintel W %.6g uu, reactions (%.6g, %.6g); window lintel W %.6g uu, "
			 "reactions (%.6g, %.6g); porch overhang centroid Y 376 forward of the post line Y 340."),
		WDoorHeader, DoorRa, DoorRb, WWinLintel, WinRa, WinRb));

	TestTrue(TEXT("SIZING: the door lintel's two pier reactions are both positive (centroid between)"),
		DoorRa > 0.0 && DoorRb > 0.0);
	TestTrue(TEXT("SIZING: the window lintel's two jamb reactions are both positive (centroid between)"),
		WinRa > 0.0 && WinRb > 0.0);
	TestTrue(TEXT("SIZING: the overhang centroid (Y 376) is FORWARD of the posts (Y 340), so the porch is "
		"a cantilever whose Z-normal wall fixing holds its back down in withdrawal rather than a passenger"),
		376.0 > 340.0);

	// The builder lays the shed: counts, materials, grounding, 3D flag, openings, gables, joints.

	FBrickLayout Layout;
	const bool bBuilt = DestructionShed3D::BuildRecognizable(Layout);

	TestTrue(TEXT("BUILD: the builder must lay the recognizable shed (the stub returns false — the RED)"),
		bBuilt);

	TestTrue(TEXT("BUILD: the structure must be flagged 3D so the bridge poses its Y-normal joints"),
		Layout.Structure.IsThreeDimensional());

	TestEqual(TEXT("BUILD: 23 pieces — 4 walls (front split by the door), a window course, gables, roof, porch"),
		Layout.Structure.NumPieces(), ExpectedPieces);
	TestEqual(TEXT("BUILD: one box per piece, or AdoptLayout refuses the layout"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());
	TestEqual(TEXT("BUILD: 29 joints — door(2), window(4), corners(4), gables(6), roof(10), porch(3)"),
		Layout.Structure.NumConnections(), ExpectedJoints);

	TestEqual(TEXT("BUILD: 7 grounded pieces — the four walls' feet and the two posts"),
		CountMaterial(Layout.Structure, &ClayBrick, true, true)
			+ CountMaterial(Layout.Structure, &Timber, true, true), ExpectedGrounded);
	TestEqual(TEXT("BUILD: 13 ClayBrick pieces (walls, piers, sill, jambs, gables)"),
		CountMaterial(Layout.Structure, &ClayBrick, false, false), ExpectedBrick);
	TestEqual(TEXT("BUILD: 10 Timber pieces (lintels, purlins, ridge, posts, overhang)"),
		CountMaterial(Layout.Structure, &Timber, false, false), ExpectedTimber);

	FShed S;
	const bool bIdentified = bBuilt && Identify(Layout, S);

	if (!bIdentified)
	{
		AddError(TEXT("BUILD: the builder must lay a recognizable shed whose named pieces identify by "
			"position. Until BuildRecognizable is implemented this fails: the stub lays nothing."));
		return false;
	}

	// The door and window openings are gaps.

	TestEqual(TEXT("OPENING: the doorway is a GAP — no piece spans it at ground level between the piers"),
		PieceContaining(Layout, FVector(DoorGapX, DoorGapY, DoorGapZ)), (int32)INDEX_NONE);
	TestEqual(TEXT("OPENING: the window is a GAP — no piece fills it between the jambs and under the lintel"),
		PieceContaining(Layout, FVector(WindowGapX, WindowGapY, WindowGapZ)), (int32)INDEX_NONE);

	// Flanked by piers and jambs, so each gap is an opening in a wall, not a missing wall.
	TestTrue(TEXT("OPENING: brick piers flank the doorway left and right"),
		S.LeftPier != INDEX_NONE && S.RightPier != INDEX_NONE
			&& S.LeftPier != S.RightPier);
	TestTrue(TEXT("OPENING: brick jambs flank the window back and front"),
		S.WinJambBack != INDEX_NONE && S.WinJambFront != INDEX_NONE
			&& S.WinJambBack != S.WinJambFront);

	// The gable rises in centred steps to the ridge.

	const FPieceBox& GBase = Layout.Boxes[S.FGableBase];
	const FPieceBox& GMid = Layout.Boxes[S.FGableMid];
	const FPieceBox& GApex = Layout.Boxes[S.FGableApex];

	TestTrue(TEXT("GABLE: the three courses climb in Z (base < mid < apex)"),
		GBase.CentreCm.Z < GMid.CentreCm.Z && GMid.CentreCm.Z < GApex.CentreCm.Z);
	TestTrue(TEXT("GABLE: each higher course is NARROWER in X (a triangle, not a tower)"),
		GBase.ExtentCm.X > GMid.ExtentCm.X && GMid.ExtentCm.X > GApex.ExtentCm.X);
	TestTrue(TEXT("GABLE: every course is centred at X=150, so the stack cannot overturn"),
		FMath::IsNearlyEqual(GBase.CentreCm.X, 150.0, 1.0e-6)
			&& FMath::IsNearlyEqual(GMid.CentreCm.X, 150.0, 1.0e-6)
			&& FMath::IsNearlyEqual(GApex.CentreCm.X, 150.0, 1.0e-6));
	TestTrue(TEXT("GABLE: the ridge beam sits above the apex, spanning the ridge line"),
		Layout.Boxes[S.Ridge].CentreCm.Z > GApex.CentreCm.Z);
	TestTrue(TEXT("GABLE: the two gable ends (front and back) both rise to an apex under the ridge"),
		FMath::IsNearlyEqual(Layout.Boxes[S.FGableApex].CentreCm.Z,
			Layout.Boxes[S.BGableApex].CentreCm.Z, 1.0e-6));

	// Materials as authored.

	TestTrue(TEXT("BUILD: the back wall is ClayBrick"), Layout.Structure.GetPiece(S.BackWall).Material == &ClayBrick);
	TestTrue(TEXT("BUILD: the right wall is ClayBrick"), Layout.Structure.GetPiece(S.RightWall).Material == &ClayBrick);
	TestTrue(TEXT("BUILD: the left door pier is ClayBrick"), Layout.Structure.GetPiece(S.LeftPier).Material == &ClayBrick);
	TestTrue(TEXT("BUILD: the right door pier is ClayBrick"), Layout.Structure.GetPiece(S.RightPier).Material == &ClayBrick);
	TestTrue(TEXT("BUILD: the sill course is ClayBrick"), Layout.Structure.GetPiece(S.Sill).Material == &ClayBrick);
	TestTrue(TEXT("BUILD: the window jambs are ClayBrick"),
		Layout.Structure.GetPiece(S.WinJambBack).Material == &ClayBrick
			&& Layout.Structure.GetPiece(S.WinJambFront).Material == &ClayBrick);
	TestTrue(TEXT("BUILD: the gable courses are ClayBrick"),
		Layout.Structure.GetPiece(S.FGableBase).Material == &ClayBrick
			&& Layout.Structure.GetPiece(S.FGableApex).Material == &ClayBrick);
	TestTrue(TEXT("BUILD: the door lintel is Timber"), Layout.Structure.GetPiece(S.DoorHeader).Material == &Timber);
	TestTrue(TEXT("BUILD: the window lintel is Timber"), Layout.Structure.GetPiece(S.WinLintel).Material == &Timber);
	TestTrue(TEXT("BUILD: the ridge beam is Timber"), Layout.Structure.GetPiece(S.Ridge).Material == &Timber);
	TestTrue(TEXT("BUILD: the porch posts are Timber"),
		Layout.Structure.GetPiece(S.PostL).Material == &Timber
			&& Layout.Structure.GetPiece(S.PostR).Material == &Timber);
	TestTrue(TEXT("BUILD: the porch overhang is Timber"), Layout.Structure.GetPiece(S.Overhang).Material == &Timber);

	// Walls and posts are grounded; spanning pieces are not.

	TestTrue(TEXT("BUILD: the back wall is grounded"), Layout.Structure.GetPiece(S.BackWall).bIsGrounded);
	TestTrue(TEXT("BUILD: the right wall is grounded"), Layout.Structure.GetPiece(S.RightWall).bIsGrounded);
	TestTrue(TEXT("BUILD: the door piers are grounded"),
		Layout.Structure.GetPiece(S.LeftPier).bIsGrounded && Layout.Structure.GetPiece(S.RightPier).bIsGrounded);
	TestTrue(TEXT("BUILD: the sill course is grounded"), Layout.Structure.GetPiece(S.Sill).bIsGrounded);
	TestTrue(TEXT("BUILD: the porch posts are grounded"),
		Layout.Structure.GetPiece(S.PostL).bIsGrounded && Layout.Structure.GetPiece(S.PostR).bIsGrounded);
	TestFalse(TEXT("BUILD: the door lintel is not grounded (it is carried by the piers)"),
		Layout.Structure.GetPiece(S.DoorHeader).bIsGrounded);
	TestFalse(TEXT("BUILD: the window lintel is not grounded (it is carried by the jambs)"),
		Layout.Structure.GetPiece(S.WinLintel).bIsGrounded);
	TestFalse(TEXT("BUILD: the ridge is not grounded (it is carried by the gables)"),
		Layout.Structure.GetPiece(S.Ridge).bIsGrounded);
	TestFalse(TEXT("BUILD: the overhang is not grounded (it is carried by the posts + fixing)"),
		Layout.Structure.GetPiece(S.Overhang).bIsGrounded);

	TestTrue(TEXT("BUILD: the shed knows where every piece and joint is, or every moment is silently zero"),
		Layout.Structure.HasCompleteGeometry());

	// Key joints: lintels on their supports; the porch fixing is a tension tie.

	const int32 DoorOnLeft = JointBetween(Layout.Structure, S.LeftPier, S.DoorHeader);
	const int32 DoorOnRight = JointBetween(Layout.Structure, S.RightPier, S.DoorHeader);
	const int32 WinOnBack = JointBetween(Layout.Structure, S.WinJambBack, S.WinLintel);
	const int32 WinOnFront = JointBetween(Layout.Structure, S.WinJambFront, S.WinLintel);
	const int32 RidgeOnFront = JointBetween(Layout.Structure, S.FGableApex, S.Ridge);
	const int32 Fixing = JointBetween(Layout.Structure, S.Cleat, S.Overhang);
	const int32 Anchor = JointBetween(Layout.Structure, S.DoorHeader, S.Cleat);
	const int32 CornerRWBW = JointBetween(Layout.Structure, S.RightWall, S.BackWall);
	const int32 PostLOnOverhang = JointBetween(Layout.Structure, S.PostL, S.Overhang);
	const int32 PostROnOverhang = JointBetween(Layout.Structure, S.PostR, S.Overhang);

	TestTrue(TEXT("JOINT: the door lintel bears on the LEFT pier"), DoorOnLeft != INDEX_NONE);
	TestTrue(TEXT("JOINT: the door lintel bears on the RIGHT pier"), DoorOnRight != INDEX_NONE);
	TestTrue(TEXT("JOINT: the window lintel bears on the BACK jamb"), WinOnBack != INDEX_NONE);
	TestTrue(TEXT("JOINT: the window lintel bears on the FRONT jamb"), WinOnFront != INDEX_NONE);
	TestTrue(TEXT("JOINT: the ridge bears on the front gable apex"), RidgeOnFront != INDEX_NONE);
	TestTrue(TEXT("JOINT: the overhang is tied down to the central cleat"), Fixing != INDEX_NONE);
	TestTrue(TEXT("JOINT: the cleat is anchored to the door header"), Anchor != INDEX_NONE);
	TestTrue(TEXT("JOINT: the overhang bears on both posts"),
		PostLOnOverhang != INDEX_NONE && PostROnOverhang != INDEX_NONE);

	if (DoorOnLeft == INDEX_NONE || DoorOnRight == INDEX_NONE || RidgeOnFront == INDEX_NONE
		|| Fixing == INDEX_NONE)
	{
		AddError(TEXT("JOINT: the carrying joints must exist before their roles can be read"));
		return false;
	}

	/*
	 * The porch tie is a Z-normal bed beneath the overhang (withdrawal); the cleat anchor and the
	 * corners are Y-normal, out of the X-Z plane, which a 2D oracle cannot express.
	 */
	TestTrue(TEXT("JOINT: the door lintel bears on the left pier through a bed joint"),
		Layout.Structure.GetJointRole(DoorOnLeft, S.DoorHeader) == EJointRole::BedBeneath);
	TestTrue(TEXT("JOINT: the ridge bears on the apex through a bed joint"),
		Layout.Structure.GetJointRole(RidgeOnFront, S.Ridge) == EJointRole::BedBeneath);
	TestTrue(TEXT("JOINT: the porch fixing is a Z-normal bed beneath the overhang (a withdrawal tie, not shear)"),
		Layout.Structure.GetJointRole(Fixing, S.Overhang) == EJointRole::BedBeneath);
	TestTrue(TEXT("JOINT: the cleat-to-wall anchor is a Y-normal mortar joint, |normal.Y| ~ 1 (out of the X-Z plane)"),
		Anchor != INDEX_NONE
			&& FMath::IsNearlyEqual(FMath::Abs(Layout.Structure.GetConnection(Anchor).InterfaceNormal.Y), 1.0, 1.0e-9));
	TestTrue(TEXT("JOINT: a corner joint is a Y-normal head joint, |normal.Y| ~ 1 (out of the X-Z plane)"),
		CornerRWBW != INDEX_NONE
			&& FMath::IsNearlyEqual(FMath::Abs(Layout.Structure.GetConnection(CornerRWBW).InterfaceNormal.Y), 1.0, 1.0e-9));
	TestEqual(TEXT("JOINT: the fixing is a Screw — a tension-capable tie"),
		Layout.Structure.GetConnection(Fixing).Strength.TensileStrengthMPa, Screw.TensileStrengthMPa);

	/*
	 * The assembled shed stands: mechanism via the 3D oracle (lambda* >= 1), outcome via production
	 * (nothing Stranded, lintels and roof Supported). No displacement.
	 */

	RigidBlockOracle::FOracleProblem Problem;
	FString BridgeWhy;
	const bool bBridged = RigidBlockOracle::BuildRigidBlockProblem(Layout.Structure, Problem, BridgeWhy);

	TestTrue(*FString::Printf(TEXT("STANDS: the oracle bridge must accept this 3D shed (%s)"), *BridgeWhy),
		bBridged);

	if (bBridged)
	{
		TestEqual(TEXT("STANDS: the bridged problem is posed in 3D"),
			static_cast<int32>(Problem.Dim), static_cast<int32>(RigidBlockOracle::EOracleDim::Dim3D));

		const RigidBlockOracle::FOracleResult Live = RigidBlockOracle::SolveRigidBlock(Problem);
		const RigidBlockOracle::EOracleOutcome Outcome = RigidBlockOracle::OutcomeOf(Live);

		AddInfo(FString::Printf(
			TEXT("STANDS: oracle answered %d, lambda* %.10g, outcome %d (2=Stands,1=Falls,0=Unanswerable)"),
			Live.bAnswered ? 1 : 0, Live.Lambda, static_cast<int32>(Outcome)));

		TestTrue(TEXT("STANDS: the oracle must ANSWER"), Live.bAnswered);
		TestEqual(TEXT("STANDS: the assembled shed must STAND"),
			static_cast<int32>(Outcome), static_cast<int32>(RigidBlockOracle::EOracleOutcome::Stands));
		TestTrue(*FString::Printf(TEXT("STANDS: lambda* %.10g must sit at or above 1"), Live.Lambda),
			Live.bAnswered && Live.Lambda >= 1.0);
	}

	// Outcome via production; below the block cap the LP is the break authority.
	const int32 Passes = Layout.Structure.SolveAndBreak();
	const int32 Stranded = StrandedCount(Layout.Structure);

	AddInfo(FString::Printf(TEXT("STANDS: PRODUCTION ran %d pass(es); %d stranded"), Passes, Stranded));

	TestEqual(TEXT("STANDS: nothing may be Stranded — the verdict is about the shed, not the solver declining"),
		Stranded, 0);

	// The spanning pieces read Supported, not floating.
	TestTrue(TEXT("STANDS: the door lintel reads Supported (carried by its piers)"),
		IsStanding(Layout.Structure.GetPieceSupport(S.DoorHeader)));
	TestTrue(TEXT("STANDS: the window lintel reads Supported (carried by its jambs)"),
		IsStanding(Layout.Structure.GetPieceSupport(S.WinLintel)));
	TestTrue(TEXT("STANDS: the ridge reads Supported (carried by the gables)"),
		IsStanding(Layout.Structure.GetPieceSupport(S.Ridge)));
	TestTrue(TEXT("STANDS: the porch overhang reads Supported (carried by the posts + fixing)"),
		IsStanding(Layout.Structure.GetPieceSupport(S.Overhang)));

	return true;
}

/**
 * Pulling either porch post drops the overhang, while the walls, the other post, the door lintel and
 * the roof keep the ground. Mechanism via the 3D oracle (Falls, lambda* < 1, the mechanism names the
 * overhang); outcome via production SolveAndBreak. No displacement (DESIGN §4).
 *
 * No door-pier arm (decision 2026-08-30): pulling a pier stands at lambda* = 42.08 because the masonry
 * over the door acts as a deep beam (composite vertical action). That is correct physics.
 *
 * The porch is a cantilever tied by a narrow central cleat. A wide tie forms a compression/tension
 * couple about X with an arm of its half-width, which held one post pulled (lambda* = 13.31 for a
 * full-width bed). MakeInterface uses the boxes' overlap, so a narrow tie needs its own piece: the
 * 20 cm Cleat, X[140,160]. Tie is Z-normal Screw, 180 cm2, half-width 10 cm.
 *
 * Sizing: W = 0.42 * (220*150*20) / 1000 * 980 = 271,656 uu. Tie at Y 305.5, posts Y 340, centroid Y 376.
 *   Assembled: tie withdrawal T = W*36/34.5 = 1.043 W, against 0.54 MPa * 180 cm2 = 972,000 uu
 *     (3.4x margin). Posts are symmetric about X=150, so no couple is demanded. Stands.
 *   One post removed: the other takes 2.043 W at 80 cm from the load line, demanding 163.4 W*cm; the
 *     tie's couple is at most 972,000 * 10 = 35.8 W*cm. 4.6x short. Falls.
 *
 * Units spelled out locally (DESIGN.md §3). World-free.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRecognizableShed3DCollapseTest,
	"DestructionGame.Acceptance.Shed.ThreeD.RecognizableShedCollapsesWhenAPostOrPierIsPulled",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRecognizableShed3DCollapseTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace RecognizableShed3DTestSupport;

	/*
	 * Strength basis pinned to published figures. The Screw has mu = 0 (shear is pure cohesion); DryStone
	 * carries no tension.
	 */

	TestEqual(TEXT("FIXTURE: the porch fixing is a Screw, withdrawal 0.54 MPa (EN 1995-1-1 8.7.2)"),
		Screw.TensileStrengthMPa, 0.54);
	TestEqual(TEXT("FIXTURE: the Screw carries shear by cohesion 0.23 MPa with mu = 0 (no friction help)"),
		Screw.ShearCohesionMPa, 0.23);
	TestEqual(TEXT("FIXTURE: the Screw's friction coefficient is exactly zero (mechanical fastener)"),
		Screw.FrictionCoefficient, 0.0);
	TestEqual(TEXT("FIXTURE: the bearings are DryStone frictional contacts with NO tension"),
		DryStone.TensileStrengthMPa, 0.0);

	// Hand-derived controls for the porch, sized both ways, independent of the builder and the LP.

	const double OverhangWeightUu =
		TimberDensityGramsPerCubicCm * (220.0 * 150.0 * 20.0) / 1000.0 * GravityCmPerSecondSquared;

	const double TieLapCentreYCm = 305.5;       // cleat lap Y[301,310]
	const double PostLineYCm = 340.0;           // posts Y[328,352]
	const double OverhangCentroidYCm = 376.0;   // overhang Y[301,451]

	const double TieAreaSqCm = 20.0 * 9.0;      // narrow central cleat lap X[140,160] x Y[301,310]
	const double TieHalfWidthXCm = 10.0;        // half the cleat's 20 cm X-extent; the X-couple arm
	const double TieWithdrawalCapUu =
		Screw.TensileStrengthMPa * ForceUnitsPerMPaSqCmHere * TieAreaSqCm;   // 0.54 MPa * 180 cm2

	// Assembled: the tie holds the back down in withdrawal; symmetric posts demand no X-couple.
	const double TieTensionUu = OverhangWeightUu
		* (OverhangCentroidYCm - PostLineYCm) / (PostLineYCm - TieLapCentreYCm);   // 1.043 W

	/*
	 * One post removed: the surviving post's X-moment demand against the tie's maximum couple, its
	 * withdrawal capacity at its half-width (10 cm here; a full-width bed's 110 cm over-held).
	 */
	const double RemainingPostReactionUu = OverhangWeightUu
		* (OverhangCentroidYCm - TieLapCentreYCm) / (PostLineYCm - TieLapCentreYCm);  // 2.043 W up
	const double RemainingPostCentreXCm = 70.0;      // PostL when PostR is removed
	const double LoadCentreXCm = 150.0;

	const double XMomentDemandUuCm =
		RemainingPostReactionUu * FMath::Abs(RemainingPostCentreXCm - LoadCentreXCm);
	const double XCoupleRestoreMaxUuCm = TieWithdrawalCapUu * TieHalfWidthXCm;

	AddInfo(FString::Printf(
		TEXT("DERIVED (narrow central cleat): overhang W %.6g uu; assembled tie tension %.6g vs withdrawal "
			 "cap %.6g (%.3gx). One post gone: surviving post reaction %.6g demands X-moment %.6g uu*cm; the "
			 "tie's X-COUPLE restores at most cap * half-width = %.6g * %.4g = %.6g uu*cm (%.3gx short)."),
		OverhangWeightUu, TieTensionUu, TieWithdrawalCapUu, TieWithdrawalCapUu / TieTensionUu,
		RemainingPostReactionUu, XMomentDemandUuCm,
		TieWithdrawalCapUu, TieHalfWidthXCm, XCoupleRestoreMaxUuCm,
		XMomentDemandUuCm / XCoupleRestoreMaxUuCm));

	TestTrue(TEXT("CONTROL (assembled): the tie's withdrawal capacity must comfortably exceed the tension "
		"holding the cantilever's back down -> stands"),
		TieWithdrawalCapUu > 3.0 * TieTensionUu);
	TestTrue(TEXT("CONTROL (one post gone): the surviving post's X-moment demand must outrun the tie's "
		"X-couple (its withdrawal force at its half-width), so the overhang tips toward the gap -> falls"),
		XMomentDemandUuCm > 2.0 * XCoupleRestoreMaxUuCm);

	// Collapse arm: pull PostR (PostL is the mirror), then read the oracle mechanism and production outcome.

	const TCHAR* const Label = TEXT("porch post pulled");

	FBrickLayout Fresh;
	if (!DestructionShed3D::BuildRecognizable(Fresh))
	{
		AddError(TEXT("the builder must lay the recognizable shed"));
		return false;
	}

	FShed F;
	if (!Identify(Fresh, F))
	{
		AddError(TEXT("the laid recognizable shed must identify"));
		return false;
	}

	const int32 ExpectedFalling = F.Overhang;
	// Everything else is independent of the porch and keeps the ground.
	const TArray<int32> ExpectedStanding = { F.BackWall, F.RightWall, F.LeftPier, F.RightPier, F.Sill,
		F.PostL, F.DoorHeader, F.Ridge };

	Fresh.Structure.RemovePiece(F.PostR);

	// Mechanism, via the 3D oracle.
	RigidBlockOracle::FOracleProblem Problem;
	FString BridgeWhy;
	const bool bBridged = RigidBlockOracle::BuildRigidBlockProblem(Fresh.Structure, Problem, BridgeWhy);

	TestTrue(
		*FString::Printf(TEXT("%s: the oracle bridge must accept the 3D shed (%s)"), Label, *BridgeWhy),
		bBridged);

	if (bBridged)
	{
		TestEqual(*FString::Printf(TEXT("%s: the bridged problem is posed in 3D"), Label),
			static_cast<int32>(Problem.Dim), static_cast<int32>(RigidBlockOracle::EOracleDim::Dim3D));

		const RigidBlockOracle::FOracleResult Live = RigidBlockOracle::SolveRigidBlock(Problem);
		const RigidBlockOracle::EOracleOutcome Outcome = RigidBlockOracle::OutcomeOf(Live);

		AddInfo(FString::Printf(
			TEXT("%s: oracle answered %d, lambda* %.10g, outcome %d (2=Stands,1=Falls,0=Unanswerable)"),
			Label, Live.bAnswered ? 1 : 0, Live.Lambda, static_cast<int32>(Outcome)));

		TestTrue(*FString::Printf(TEXT("%s: the oracle must ANSWER"), Label), Live.bAnswered);

		TestEqual(
			*FString::Printf(TEXT("%s: pulling a post must make the overhang FALL"), Label),
			static_cast<int32>(Outcome),
			static_cast<int32>(RigidBlockOracle::EOracleOutcome::Falls));

		TestTrue(
			*FString::Printf(TEXT("%s: lambda* %.10g must sit clearly below 1"), Label, Live.Lambda),
			Live.bAnswered && Live.Lambda < 0.9);

		// The dead-load mechanism must name the overhang as moving, so the fall is real, not a routing artefact.
		RigidBlockOracle::FOracleProblem Dead = Problem;
		Dead.bGravityIsLive = false;
		const RigidBlockOracle::FOracleResult DeadR = RigidBlockOracle::SolveRigidBlock(Dead);

		TestTrue(
			*FString::Printf(TEXT("%s: the LP must extract a certified collapse mechanism"), Label),
			DeadR.Mechanism.bPresent && DeadR.Mechanism.bIsCertified);

		const int32 Block = OracleBlockOfPiece(Dead, ExpectedFalling);
		const bool bMoves = DeadR.Mechanism.bPresent
			&& DeadR.Mechanism.Blocks.IsValidIndex(Block)
			&& DeadR.Mechanism.Blocks[Block].bMoves;

		AddInfo(FString::Printf(TEXT("%s: the overhang is piece %d, oracle block %d, moves %d"),
			Label, ExpectedFalling, Block, bMoves ? 1 : 0));

		TestTrue(
			*FString::Printf(TEXT("%s: the mechanism must name the overhang (piece %d) as a moving block"),
				Label, ExpectedFalling),
			bMoves);
	}

	// Outcome via production; below the block cap the LP is the break authority.
	const int32 Passes = Fresh.Structure.SolveAndBreak();
	const int32 Stranded = StrandedCount(Fresh.Structure);

	AddInfo(FString::Printf(TEXT("%s: PRODUCTION ran %d pass(es); %d stranded"), Label, Passes, Stranded));

	TestEqual(
		*FString::Printf(TEXT("%s: nothing may be Stranded — the verdict must be about the shed, not the "
			"solver declining to route"), Label),
		Stranded, 0);

	TestTrue(
		*FString::Printf(TEXT("%s: the overhang (piece %d) must lose the earth (support %d) — the porch "
			"drops when a post is pulled"), Label, ExpectedFalling,
			static_cast<int32>(Fresh.Structure.GetPieceSupport(ExpectedFalling))),
		HasLostTheEarth(Fresh.Structure, ExpectedFalling));

	for (const int32 Piece : ExpectedStanding)
	{
		TestTrue(
			*FString::Printf(TEXT("%s: piece %d must still be held up (support %d)"), Label, Piece,
				static_cast<int32>(Fresh.Structure.GetPieceSupport(Piece))),
			IsStanding(Fresh.Structure.GetPieceSupport(Piece)));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
