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
 * THE RECOGNIZABLE 3D SHED BUILDER — the v2 geometry rebuild. The 7-piece toy (four grey blocks and a
 * plank) is physics-valid but does not READ as a shed; this drives the builder that lays a shed with a
 * FORM someone recognises, while staying inside the axis-aligned envelope the 3D bridge requires.
 *
 * THE BEHAVIOUR, IN ONE SENTENCE. DestructionShed3D::BuildRecognizable lays a recognizable axis-aligned
 * brick shed — four walls closing a box, a DOOR opening in the front wall (two brick piers carrying a
 * Timber lintel), a WINDOW opening in the left wall (a sill course, two brick jambs, a Timber lintel),
 * STEPPED brick gables on the front and back walls rising in symmetric centred courses to a ridge, a
 * Timber roof of stepped purlins plus a ridge beam bearing on the gable shoulders, and a Timber porch
 * overhang over the door carried on two grounded Timber posts and a wall fixing — as a
 * SetThreeDimensional FBrickLayout in which every piece carries its authored MATERIAL and every contact
 * its authored CONNECTION, so that (bridged to the 3D LP) the assembled shed STANDS, its door and window
 * lintels read as carried by their piers/jambs, and its roof reads as carried by the gables.
 *
 * THIS SLICE IS SHAPE + STANDS ONLY (the RED decomposition — see the report). It asserts the builder
 * lays the expected recognizable geometry (piece/joint counts, the door and window openings as genuine
 * GAPS, the gable rising in steps to the ridge, per-piece materials and grounding), that the structure
 * is flagged 3D and bridges to the 3D oracle, and that the assembled shed STANDS (lambda* >= 1, nothing
 * stranded, the lintels and roof read Supported). The COLLAPSE ARMS — pull a porch post -> the porch
 * falls; pull a door pier -> the door lintel falls; pull a window jamb -> the window lintel falls — are
 * the follow-up slice, each hand-derived there. Because the builder is a bare `return false` stub, the
 * expected RED is "the builder lays nothing yet": Build returns false and Identify fails.
 *
 * =========================================================================================
 * THE FIXTURE — A RECOGNIZABLE AXIS-ALIGNED SHED. X IS WIDTH, Y IS DEPTH (INTO THE DOOR), Z IS HEIGHT.
 * 24 PIECES, 30 JOINTS. All dimensions cm; the numbers below are the canonical shed the builder lays,
 * spelled out here so the hand-derivation reads from the SAME numbers.
 * =========================================================================================
 *
 * Outer footprint X[0,300], Y[0,300]; walls 25 thick, eaves (wall top) at Z=200; joints 1 cm.
 *
 * FRONT is large Y (Y[275,300], the door faces +Y, the porch cantilevers +Y over it). BACK is Y[0,25].
 * The gable roof's ridge runs along Y, so the FRONT and BACK walls are the triangular GABLE ENDS; the
 * LEFT (X[0,25]) and RIGHT (X[275,300]) walls are the eaves walls. The door is in the front gable wall,
 * the window in the left wall.
 *
 * THE 24 PIECES (box = X[lo,hi] Y[lo,hi] Z[lo,hi]; G = grounded):
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
 *   PORCH (Timber, over the door, +Y) — a CANTILEVER whose back is tied by a narrow central cleat:
 *     PostL        Timber    G  X[55,85]   Y[328,352] Z[0,200]     grounded porch post, left  (Xc 70).
 *     PostR        Timber    G  X[215,245] Y[328,352] Z[0,200]     grounded porch post, right (Xc 230).
 *     Overhang     Timber       X[40,260]  Y[301,451] Z[201,221]   porch roof; centroid (150,376,211),
 *                                                                  FORWARD of the post line (Y=340).
 *     Cleat        Timber       X[140,160] Y[301,310] Z[176,200]   a 20 cm central bracket, 1 cm off the
 *                                                                  wall, projecting to Y=310; the overhang
 *                                                                  laps ONLY this (Z-normal tie, 180 cm2).
 *
 * THE 30 JOINTS (each through MakeInterface — one axis of separation, by the joint thickness):
 *   DOOR     (2): LeftPier-DoorHeader, RightPier-DoorHeader                          bed, DryStone.
 *   WINDOW   (4): Sill-WinJambBack, Sill-WinJambFront, WinJambBack-WinLintel,
 *                 WinJambFront-WinLintel                                             bed, DryStone.
 *   CORNERS  (4): Sill-BackWall, Sill-LeftPier, RightWall-BackWall, RightWall-RightPier
 *                 (all grounded-grounded, so the oracle SKIPS them — closure, not structure).
 *   GABLES   (6): FGableBase-DoorHeader, FGableBase-FGableMid, FGableMid-FGableApex,
 *                 BGableBase-BackWall, BGableBase-BGableMid, BGableMid-BGableApex     bed, mortar.
 *   ROOF    (10): each of the five purlins to the BACK and FRONT gable shoulder it rests on
 *                                                                                     bed, DryStone.
 *   PORCH    (4): PostL-Overhang, PostR-Overhang (bed, DryStone); Cleat-Overhang (the tie, Z-normal,
 *                 Screw, 180 cm2); DoorHeader-Cleat (the anchor, Y-normal, mortar, 480 cm2).
 *
 * =========================================================================================
 * THE ASSEMBLED STAND IS HAND-DERIVED (never mirrored from the LP). Every non-grounded piece has a
 * downward compression path to a grounded piece, and the two arrangements that could overturn are shown
 * not to:
 *   - THE STEPPED GABLE is a SYMMETRIC CENTRED stack: every course is centred at X=150 and narrows
 *     symmetrically (300 -> 150 -> 50 wide), so each course's centroid sits directly over the course
 *     below and the stack cannot overturn. The purlins load each gable symmetrically about X=150
 *     (EavesL/EavesR, MidL/MidR mirror; the Ridge is centred), so there is no net overturning moment.
 *   - THE DOOR LINTEL and WINDOW LINTEL are simply-supported beams whose centroid sits BETWEEN their
 *     two supports (door centroid X=150 between piers at X=55 and 245; window centroid Y=150 between
 *     jambs at Y=73 and 227), so both reactions are positive.
 *   - THE PORCH is a CANTILEVER: two posts (Y=340) carry the overhang, whose centroid (Y=376) sits
 *     FORWARD of them, so the narrow central cleat tie holds the back down in WITHDRAWAL (a small down
 *     force, comfortably within the Screw's 0.54 MPa over 180 cm2). Assembled the two posts are
 *     X-symmetric about the load line (X=150), so their X-moments cancel and no couple is demanded of
 *     the tie -> stands. The collapse arm (its own test) shows the mirror: remove a post and the load
 *     line's X-moment outruns everything the narrow tie's 10 cm-half-width couple can restore -> falls.
 *
 * =========================================================================================
 * UNITS — SPELLED OUT LOCALLY (DESIGN.md §3). 1 N = 100 uu, 1 cm2 = 100 mm2, so 1 MPa over 1 cm2 is
 * 10000 uu. DELIBERATELY not the production constant. Weight is MassKg * 980; masses come from the
 * published densities (Timber 0.42, ClayBrick 1.9) times the true volumes.
 *
 * NEEDS A TICKING WORLD: NO. The builder is arithmetic over boxes; the structure over a graph; the LP
 * over that; gravity is on (weight = mass*980); every assertion is on the laid layout, the bridged
 * oracle, or the solved outcome — no Chaos, no world tick. Same footing as the 7-piece toy.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges files into one translation unit.
 */
namespace RecognizableShed3DTestSupport
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
	 * THE CANONICAL SHED, spelled out so the derivation reads the SAME numbers the builder lays. The
	 * builder hardcodes these; the test reads back the laid layout and identifies pieces by position.
	 * ================================================================================ */

	constexpr int32 ExpectedPieces = 24;
	constexpr int32 ExpectedJoints = 30;
	constexpr int32 ExpectedGrounded = 7;    // BackWall, RightWall, LeftPier, RightPier, Sill, PostL, PostR
	constexpr int32 ExpectedBrick = 13;
	constexpr int32 ExpectedTimber = 11;     // + the Cleat (the wall tie); the cleat is NOT grounded

	/* Door opening and window opening — the GAPS. A point in the middle of each must contain NO piece. */
	constexpr double DoorGapX = 150.0, DoorGapY = 287.5, DoorGapZ = 75.0;
	constexpr double WindowGapX = 12.5, WindowGapY = 150.0, WindowGapZ = 130.0;

	/* Centroids of the pieces the assertions name, so identification is by POSITION not by handle. */
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

	/* ================================================================================
	 * THE HAND-DERIVED POSITIVE CONTROLS. Independent of the builder — they prove the CHOSEN dimensions
	 * produce the intended supported regimes, so the LP's "stands" has real controls behind it.
	 * ================================================================================ */

	double BoxWeightUu(const FVector& Ext /*half extents cm*/, double DensityGramsPerCubicCm)
	{
		const double VolumeCm3 = (2.0 * Ext.X) * (2.0 * Ext.Y) * (2.0 * Ext.Z);
		return DensityGramsPerCubicCm * VolumeCm3 / 1000.0 * GravityCmPerSecondSquared;
	}

	/* The door lintel: a simply-supported beam. Reactions of a point load w at position p on supports
	 * at a and b are R_a = w*(b-p)/(b-a), R_b = w*(p-a)/(b-a) — both positive iff a < p < b. */
	void BeamReactions(double p, double a, double b, double w, double& OutRa, double& OutRb)
	{
		OutRa = w * (b - p) / (b - a);
		OutRb = w * (p - a) / (b - a);
	}

	/* ================================================================================
	 * THE LAID SHED — pieces identified by MATERIAL, GROUNDING and POSITION, never by a handle order.
	 * ================================================================================ */

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

	/** The live piece whose box contains the point, or INDEX_NONE. Robust identity by position. */
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

	/** Name every asserted piece from its centroid, or fail if any is missing or ambiguous. */
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
 * THE RECOGNIZABLE 3D SHED BUILDER LAYS A SHED-SHAPED, MULTI-MATERIAL BOX WITH A DOOR, A WINDOW, STEPPED
 * GABLES AND A PORCH THAT STANDS AS BUILT — the shape-and-stands slice of the v2 rebuild.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRecognizableShed3DBuilderTest,
	"DestructionGame.Acceptance.Shed.ThreeD.RecognizableShedStandsAsBuilt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRecognizableShed3DBuilderTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace RecognizableShed3DTestSupport;

	/* ------------------------------------------------------------------ *
	 * PRECONDITIONS ON THE STRENGTH BASIS — pinned to the published figures rather than read from the
	 * profiles, so the sizing is derived against known numbers.
	 * ------------------------------------------------------------------ */

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

	/* ------------------------------------------------------------------ *
	 * THE HAND-DERIVED POSITIVE CONTROLS — the door lintel's reactions, the window lintel's reactions,
	 * and the porch's inboard centroid. Independent of the builder; a guard that the CHOSEN dimensions
	 * produce the intended supported regimes so the LP's STANDS has real controls.
	 * ------------------------------------------------------------------ */

	/* Door lintel (Timber): X[0,300] Y[275,300] Z[176,200], on piers at X-centroid 55 and 245. */
	const double WDoorHeader = BoxWeightUu(FVector(150.0, 12.5, 12.0), TimberDensityGramsPerCubicCm);
	double DoorRa = 0.0, DoorRb = 0.0;
	BeamReactions(150.0, 55.0, 245.0, WDoorHeader, DoorRa, DoorRb);

	/* Window lintel (Timber): X[0,25] Y[26,274] Z[171,200], on jambs at Y-centroid 73 and 227. */
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

	/* ================================================================================
	 * ARM 0 — THE BUILDER LAYS THE RECOGNIZABLE SHED. Counts, per-piece material and grounding, the 3D
	 * flag, the door and window OPENINGS as gaps, the gable rising in steps to the ridge, and the
	 * authored joints. This is where the stub is RED: it lays nothing, so Build returns false.
	 * ================================================================================ */

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

	/* --- THE DOOR AND WINDOW OPENINGS ARE GENUINE GAPS ---------------------------------------- */

	TestEqual(TEXT("OPENING: the doorway is a GAP — no piece spans it at ground level between the piers"),
		PieceContaining(Layout, FVector(DoorGapX, DoorGapY, DoorGapZ)), (int32)INDEX_NONE);
	TestEqual(TEXT("OPENING: the window is a GAP — no piece fills it between the jambs and under the lintel"),
		PieceContaining(Layout, FVector(WindowGapX, WindowGapY, WindowGapZ)), (int32)INDEX_NONE);

	/* And the openings are genuinely FLANKED — piers either side of the door, jambs either side of the
	 * window — so the gap is an opening in a wall, not just a missing wall. */
	TestTrue(TEXT("OPENING: brick piers flank the doorway left and right"),
		S.LeftPier != INDEX_NONE && S.RightPier != INDEX_NONE
			&& S.LeftPier != S.RightPier);
	TestTrue(TEXT("OPENING: brick jambs flank the window back and front"),
		S.WinJambBack != INDEX_NONE && S.WinJambFront != INDEX_NONE
			&& S.WinJambBack != S.WinJambFront);

	/* --- THE GABLE RISES IN SYMMETRIC CENTRED STEPS TO THE RIDGE ------------------------------- */

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

	/* --- MATERIALS: the shed is multi-material as authored ------------------------------------- */

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

	/* --- GROUNDING: the walls' feet and the posts stand on the earth; the spanning pieces do not -- */

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

	/* --- KEY JOINTS: the lintels are carried by their supports; the fixing is a tension tie ------ */

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

	/* The door lintel bears on the pier through a BED joint beneath it (+Z normal), and the porch fixing
	 * is now a Z-normal withdrawal tie: the overhang laps the central cleat, so the tie reads as a bed
	 * BENEATH the overhang. The cleat itself is anchored to the wall by a Y-normal mortar joint. The shed
	 * stays genuinely 3D through its four Y-normal CORNER joints — a contact out of the X-Z plane that a 2D
	 * oracle cannot express. */
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

	/* ================================================================================
	 * THE ASSEMBLED SHED STANDS. Mechanism via the 3D oracle (bridged, Dim3D, lambda* >= 1), outcome
	 * via production (0 stranded, the lintels and roof read Supported). No displacement anywhere.
	 * ================================================================================ */

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

	/* ---- THE OUTCOME, VIA PRODUCTION. Below the block cap the LP is the break authority. ---- */
	const int32 Passes = Layout.Structure.SolveAndBreak();
	const int32 Stranded = StrandedCount(Layout.Structure);

	AddInfo(FString::Printf(TEXT("STANDS: PRODUCTION ran %d pass(es); %d stranded"), Passes, Stranded));

	TestEqual(TEXT("STANDS: nothing may be Stranded — the verdict is about the shed, not the solver declining"),
		Stranded, 0);

	/* The carried spanning pieces read Supported — the lintels by their piers/jambs, the roof by the
	 * gables. This is the "genuinely supported, not floating" assertion for the openings and the roof. */
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
 * THE RECOGNIZABLE 3D SHED FALLS WHEN A PORCH POST IS PULLED — the collapse half of the goal (Slice 2).
 * Slice 1 proved BuildRecognizable STANDS; this proves the headline "take a post out and it falls":
 * remove EITHER porch post and the overhang drops, while the walls, the far post, the door lintel and the
 * roof all keep the earth. Read through the SAME production bridge + SolveAndBreak the toy 3D shed uses:
 * mechanism via the 3D oracle (Falls, lambda* < 1, the dead-load mechanism NAMES the overhang as a moving
 * block); outcome via production (the overhang loses the earth, the survivors keep it, nothing Stranded).
 * NEVER displacement — support state and the named mechanism only, per DESIGN §4.
 *
 * =========================================================================================
 * WHY THERE IS NO DOOR-PIER ARM (lead decision, 2026-08-30). An earlier draft also pulled a door pier
 * and expected the Timber lintel to drop. Measurement showed it STANDS at lambda* = 42.08: the mortar
 * bond over the doorway plus the internally-bonded gable act as a DEEP BEAM (COMPOSITE VERTICAL ACTION,
 * ConnectionStrength.h) that spans the missing pier and stands on the surviving one. That is CORRECT
 * PHYSICS — masonry arches over an opening when a jamb is lost — and desirable, so we do NOT weaken it to
 * force a collapse. The door is not a single point of failure, by design; only the porch is.
 *
 * =========================================================================================
 * THE PORCH IS A CANTILEVER TIED BY A NARROW CENTRAL CLEAT (Slice-2 REQUIRED PRODUCTION CHANGE — full
 * builder spec in the report). Two evolutions got here:
 *   1) The Slice-1 TABLE porch (posts front at Y=382.5, wide Y-normal shear fixing, centroid between)
 *      OVER-HELD one post pulled at lambda* = 78.71 — a wall-fixed porch surviving one of two posts is
 *      defensible physics, so we re-topologised to a genuine CANTILEVER: the overhang is carried PRIMARILY
 *      by its two posts (Y=340) and its centroid sits FORWARD of them (Y=376), so a WITHDRAWAL tie must
 *      hold its back down.
 *   2) A first cantilever tied the back down with a WIDE Z-normal Screw bed (the full X[40,260] overhang
 *      width lapping the DoorHeader ledge). That still OVER-HELD, at lambda* = 13.31 — dev found why, and
 *      it is the SAME root cause as the wide Y-normal fixing: a rigid contact carries a PRESSURE
 *      DISTRIBUTION across its whole area, so a full-width bed forms a COMPRESSION/TENSION COUPLE about the
 *      X axis (compression far side, withdrawal near side) whose arm is the contact's HALF-WIDTH. At 110 cm
 *      half-width that couple is enormous and swamps the X-torsion the collapse depends on.
 *
 * THE FIX (lead-endorsed): make the wall tie NARROW in X and CENTRAL so its X-couple arm (its half-width)
 * is small. THE TIE THEREFORE NEEDS A DEDICATED NARROW CENTRAL PIECE. MakeInterface builds a joint from
 * the GEOMETRIC OVERLAP of the two boxes (Layout.cpp: min(highs) - max(lows)), so a contact between the
 * wide overhang (X[40,260]) and any wide wall piece (X[0,300]) is ALWAYS full width — a narrow central
 * contact cannot be had by "just narrowing the lap". A small central CLEAT bracket, bonded to the wall and
 * lapped by the overhang, is the only single-box way. THIS ADDS ONE PIECE AND ONE JOINT (23 -> 24 pieces,
 * 29 -> 30 joints); "only the fixing lap changes" cannot be honoured, and the report says why.
 *
 * THE TARGET PORCH GEOMETRY the builder must lay (X width, Y depth = +Y forward over the door, Z up):
 *   DoorHeader  Timber       X[0,300]   Y[275,300] Z[176,200]   plain lintel again (drop the ledge; the
 *                                                               cleat now provides the shelf).
 *   Cleat (NEW) Timber       X[140,160] Y[300,310] Z[176,200]   a 20 cm-wide central bracket projecting
 *                                                               10 cm past the wall; bonded to the DoorHeader.
 *   PostL       Timber   G   X[55,85]   Y[328,352] Z[0,200]     grounded, centroid (70,340).
 *   PostR       Timber   G   X[215,245] Y[328,352] Z[0,200]     grounded, centroid (230,340).
 *   Overhang    Timber       X[40,260]  Y[301,451] Z[201,221]   centroid (150,376,211); cantilevers +Y
 *                                                               PAST the posts (376 > 340). It laps ONLY the
 *                                                               cleat (it clears the plain DoorHeader in Y).
 *   Tie      Overhang-Cleat  Z-normal SCREW  lap X[140,160] x Y[301,310] = 180 cm2  (half-width 10 cm).
 *   Anchor   Cleat-DoorHeader  Y-normal GeneralPurposeMortar  X[140,160] x Z[176,200] = 480 cm2.
 *   Bearings PostL-Overhang, PostR-Overhang  Z-normal DryStone  X30 x Y24 = 720 cm2 each.
 * The overhang must NOT keep any full-width bed on the wall — that bed would re-form the X-couple — so the
 * DoorHeader loses its projection and the overhang (Y[301,451]) clears it (DoorHeader ends at Y=300).
 *
 * =========================================================================================
 * SIZED BOTH WAYS, HAND-DERIVED against the TARGET geometry, WITH THE X-COUPLE EXPLICIT (the term the
 * earlier derivation omitted). Overhang weight W = 0.42 g/cm3 * (220*150*20) cm3 /1000 * 980 = 271,656 uu.
 * Tie lap centre Yf~=305.5, post line Yp=340, centroid Yc=376. Tie X-half-width h = 10 cm.
 *
 *   ASSEMBLED -> STANDS. Weight FORWARD of the posts, so the tie holds the back down in WITHDRAWAL,
 *     T = W*(Yc-Yp)/(Yp-Yf) = W*36/34.5 = 1.043 W = 283,378 uu, against withdrawal capacity 0.54 MPa *
 *     180 cm2 = 972,000 uu (3.4x margin). The two posts are X-SYMMETRIC about the load (X=150), so their
 *     X-moments cancel and NO couple is demanded of the tie -> stands (lambda* >= 1).
 *   EITHER POST REMOVED -> FALLS by X-TORSION. Say PostR goes. The Y-Z plane still balances (PostL takes
 *     P = W*(Yc-Yf)/(Yp-Yf) = 2.043 W up; the tie pulls DOWN 1.043 W in withdrawal). But the load at X=150
 *     now has only PostL at X=70 to resist the X-moment, DEMANDING P*|70-150| = 2.043 W * 80 = 163.4 W*cm.
 *     The MOST the tie can restore is its COUPLE: the withdrawal force acting at its half-width,
 *     (0.54 MPa * 180 cm2) * h = 972,000 * 10 = 9.72e6 uu*cm = 35.8 W*cm. Demand 163.4 W*cm outruns it by
 *     4.6x -> the overhang tips toward the missing post and drops. (Removing PostL is the mirror.) The
 *     narrow tie's half-width is what starves the couple — the very couple a full-width bed supplied in
 *     abundance, which is why THIS falls where the wide bed (lambda* = 13.31) held.
 *
 * *** RED STATUS (measured 2026-08-30). This test FAILS today because the builder lays the WIDE Z-normal
 * fixing (full 880 cm2 bed to the DoorHeader ledge), whose X-couple OVER-HOLDS one post pulled at
 * lambda* = 13.31. The toy 3D shed FALLS on the identical code path (post removed lambda* = 0.456), and
 * the controls below pass and the build is clean — so this is a real "tie not yet narrowed to a central
 * cleat" red, NOT a test-construction defect. dev closes it by laying the cleat tie above. The assertions
 * here identify PostR/Overhang by position (via the shared centroid table), so they hold across the change:
 * red on the wide bed, green on the narrow cleat. ***
 *
 * =========================================================================================
 * UNITS — SPELLED OUT LOCALLY (DESIGN.md §3): 1 MPa over 1 cm2 is 10000 uu; weight = MassKg * 980.
 * NEEDS A TICKING WORLD: NO — boxes, a graph, the LP; gravity on; support state and the named mechanism,
 * no Chaos, no world tick. Same footing as the toy 3D shed's collapse arms.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRecognizableShed3DCollapseTest,
	"DestructionGame.Acceptance.Shed.ThreeD.RecognizableShedCollapsesWhenAPostOrPierIsPulled",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRecognizableShed3DCollapseTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace RecognizableShed3DTestSupport;

	/* ------------------------------------------------------------------ *
	 * PRECONDITIONS ON THE STRENGTH BASIS — the verdict turns on these, so they are pinned to the
	 * published figures rather than read from the profiles. The Screw is a mechanical fastener: mu = 0,
	 * so its shear capacity is pure cohesion (no friction help), and a DryStone bed carries no tension.
	 * ------------------------------------------------------------------ */

	TestEqual(TEXT("FIXTURE: the porch fixing is a Screw, withdrawal 0.54 MPa (EN 1995-1-1 8.7.2)"),
		Screw.TensileStrengthMPa, 0.54);
	TestEqual(TEXT("FIXTURE: the Screw carries shear by cohesion 0.23 MPa with mu = 0 (no friction help)"),
		Screw.ShearCohesionMPa, 0.23);
	TestEqual(TEXT("FIXTURE: the Screw's friction coefficient is exactly zero (mechanical fastener)"),
		Screw.FrictionCoefficient, 0.0);
	TestEqual(TEXT("FIXTURE: the bearings are DryStone frictional contacts with NO tension"),
		DryStone.TensileStrengthMPa, 0.0);

	/* ------------------------------------------------------------------ *
	 * THE HAND-DERIVED POSITIVE CONTROLS — the TARGET cantilever porch, sized both ways, independent of
	 * the builder and the LP. They prove the CHOSEN dimensions produce the intended regimes: assembled
	 * the small withdrawal fixing comfortably holds the cantilever's back (stands), and with one post
	 * pulled the remaining post's X-moment demand outruns everything the fixing can restore (falls). A
	 * wrong dimension fails here rather than being mirrored from the solver.
	 * ------------------------------------------------------------------ */

	const double OverhangWeightUu =
		TimberDensityGramsPerCubicCm * (220.0 * 150.0 * 20.0) / 1000.0 * GravityCmPerSecondSquared;

	const double TieLapCentreYCm = 305.5;       // cleat lap Y[301,310]
	const double PostLineYCm = 340.0;           // posts Y[328,352]
	const double OverhangCentroidYCm = 376.0;   // overhang Y[301,451]

	const double TieAreaSqCm = 20.0 * 9.0;      // narrow central cleat lap X[140,160] x Y[301,310]
	const double TieHalfWidthXCm = 10.0;        // half of the cleat's 20 cm X-extent — the X-COUPLE ARM
	const double TieWithdrawalCapUu =
		Screw.TensileStrengthMPa * ForceUnitsPerMPaSqCmHere * TieAreaSqCm;   // 0.54 MPa * 180 cm2

	/* Assembled: the central tie holds the cantilever's back down in WITHDRAWAL; the two posts are
	 * X-symmetric about the load, so no X-couple is demanded of the tie. */
	const double TieTensionUu = OverhangWeightUu
		* (OverhangCentroidYCm - PostLineYCm) / (PostLineYCm - TieLapCentreYCm);   // 1.043 W

	/* One post removed: the surviving off-centre post's X-moment demand vs the MOST the tie's X-couple can
	 * restore. THE COUPLE IS THE TERM THE EARLIER DERIVATION OMITTED: a Z-normal tie's restoring moment
	 * about the X axis is its (withdrawal-limited) vertical force acting at its HALF-WIDTH, so a narrow
	 * central tie starves it. A full-width bed's half-width (110 cm) is what over-held; 10 cm cannot. */
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

	/* ================================================================================
	 * THE COLLAPSE ARM: build a fresh shed, identify by position, pull PostR, read the 3D oracle mechanism
	 * (Falls, lambda* < 1, the dead-load mechanism names the overhang) and the production outcome (the
	 * overhang loses the earth, the grounded survivors keep it, 0 stranded). Removing PostL is the mirror.
	 * ================================================================================ */

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
	/* Only the overhang hangs on the porch; the walls, the far post, the door lintel and the roof are all
	 * independent of it and keep the earth. */
	const TArray<int32> ExpectedStanding = { F.BackWall, F.RightWall, F.LeftPier, F.RightPier, F.Sill,
		F.PostL, F.DoorHeader, F.Ridge };

	Fresh.Structure.RemovePiece(F.PostR);

	/* ---- THE MECHANISM, VIA THE 3D ORACLE. ---- */
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

		/* The collapse mechanism (phase-1 dual, gravity dead) must NAME the overhang as a moving block, so
		 * the fall is a genuine loss of equilibrium and not a routing artefact. */
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

	/* ---- THE OUTCOME, VIA PRODUCTION. Below the block cap, the LP is the break authority. ---- */
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
