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
 * 23 PIECES, 29 JOINTS. All dimensions cm; the numbers below are the canonical shed the builder lays,
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
 * THE 23 PIECES (box = X[lo,hi] Y[lo,hi] Z[lo,hi]; G = grounded):
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
 *   PORCH (Timber, over the door, +Y):
 *     PostL        Timber    G  X[45,75]   Y[370,395] Z[0,200]     grounded porch post, left.
 *     PostR        Timber    G  X[225,255] Y[370,395] Z[0,200]     grounded porch post, right.
 *     Overhang     Timber       X[40,260]  Y[301,401] Z[201,221]   porch roof, on the two posts + a
 *                                                                  Y-normal Screw fixing to FGableBase.
 *
 * THE 29 JOINTS (each through MakeInterface — one axis of separation, by the joint thickness):
 *   DOOR     (2): LeftPier-DoorHeader, RightPier-DoorHeader                          bed, DryStone.
 *   WINDOW   (4): Sill-WinJambBack, Sill-WinJambFront, WinJambBack-WinLintel,
 *                 WinJambFront-WinLintel                                             bed, DryStone.
 *   CORNERS  (4): Sill-BackWall, Sill-LeftPier, RightWall-BackWall, RightWall-RightPier
 *                 (all grounded-grounded, so the oracle SKIPS them — closure, not structure).
 *   GABLES   (6): FGableBase-DoorHeader, FGableBase-FGableMid, FGableMid-FGableApex,
 *                 BGableBase-BackWall, BGableBase-BGableMid, BGableMid-BGableApex     bed, mortar.
 *   ROOF    (10): each of the five purlins to the BACK and FRONT gable shoulder it rests on
 *                                                                                     bed, DryStone.
 *   PORCH    (3): PostL-Overhang, PostR-Overhang (bed, DryStone), Overhang-FGableBase (the fixing,
 *                 Y-normal, Screw).
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
 *   - THE PORCH is a tripod: two posts (Y[370,395]) and a wall fixing (Y=300). The overhang centroid
 *     Y=351 is INBOARD of even the posts' inner edge (Y=370), so the Y-normal fixing is genuinely
 *     LOAD-BEARING (it stops rotation about the post line), while the plan centroid (150,351) sits
 *     inside the support triangle. Assembled: fixing in shear, posts in compression -> stands.
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

	constexpr int32 ExpectedPieces = 23;
	constexpr int32 ExpectedJoints = 29;
	constexpr int32 ExpectedGrounded = 7;    // BackWall, RightWall, LeftPier, RightPier, Sill, PostL, PostR
	constexpr int32 ExpectedBrick = 13;
	constexpr int32 ExpectedTimber = 10;

	/* Door opening and window opening — the GAPS. A point in the middle of each must contain NO piece. */
	constexpr double DoorGapX = 150.0, DoorGapY = 287.5, DoorGapZ = 75.0;
	constexpr double WindowGapX = 12.5, WindowGapY = 150.0, WindowGapZ = 130.0;

	/* Centroids of the pieces the assertions name, so identification is by POSITION not by handle. */
	const FVector CBackWall(150.0, 12.5, 100.0);
	const FVector CRightWall(287.5, 150.0, 100.0);
	const FVector CLeftPier(55.0, 287.5, 87.5);
	const FVector CRightPier(245.0, 287.5, 87.5);
	const FVector CDoorHeader(150.0, 287.5, 188.0);
	const FVector CSill(12.5, 150.0, 44.5);
	const FVector CWinJambBack(12.5, 73.0, 130.0);
	const FVector CWinJambFront(12.5, 227.0, 130.0);
	const FVector CWinLintel(12.5, 150.0, 185.5);
	const FVector CFGableBase(150.0, 287.5, 215.5);
	const FVector CFGableMid(150.0, 287.5, 245.5);
	const FVector CFGableApex(150.0, 287.5, 275.5);
	const FVector CBGableApex(150.0, 12.5, 275.5);
	const FVector CRidge(150.0, 150.0, 303.0);
	const FVector CPostL(60.0, 382.5, 100.0);
	const FVector CPostR(240.0, 382.5, 100.0);
	const FVector COverhang(150.0, 351.0, 211.0);

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
			Out.BackWall, Out.RightWall, Out.LeftPier, Out.RightPier, Out.DoorHeader, Out.Sill,
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
	TestEqual(TEXT("FIXTURE: Timber C24 crushes at 21 MPa (f_c,0,k)"),
		Timber.Strength.CompressiveStrengthMPa, 21.0);
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
			 "reactions (%.6g, %.6g); porch overhang centroid Y 351 vs post inner edge Y 370."),
		WDoorHeader, DoorRa, DoorRb, WWinLintel, WinRa, WinRb));

	TestTrue(TEXT("SIZING: the door lintel's two pier reactions are both positive (centroid between)"),
		DoorRa > 0.0 && DoorRb > 0.0);
	TestTrue(TEXT("SIZING: the window lintel's two jamb reactions are both positive (centroid between)"),
		WinRa > 0.0 && WinRb > 0.0);
	TestTrue(TEXT("SIZING: the overhang centroid (Y 351) is inboard of the posts (Y 370), so the "
		"Y-normal wall fixing is genuinely load-bearing rather than a passenger"),
		351.0 < 370.0);

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
	const int32 Fixing = JointBetween(Layout.Structure, S.Overhang, S.FGableBase);
	const int32 PostLOnOverhang = JointBetween(Layout.Structure, S.PostL, S.Overhang);
	const int32 PostROnOverhang = JointBetween(Layout.Structure, S.PostR, S.Overhang);

	TestTrue(TEXT("JOINT: the door lintel bears on the LEFT pier"), DoorOnLeft != INDEX_NONE);
	TestTrue(TEXT("JOINT: the door lintel bears on the RIGHT pier"), DoorOnRight != INDEX_NONE);
	TestTrue(TEXT("JOINT: the window lintel bears on the BACK jamb"), WinOnBack != INDEX_NONE);
	TestTrue(TEXT("JOINT: the window lintel bears on the FRONT jamb"), WinOnFront != INDEX_NONE);
	TestTrue(TEXT("JOINT: the ridge bears on the front gable apex"), RidgeOnFront != INDEX_NONE);
	TestTrue(TEXT("JOINT: the overhang is fixed to the front gable"), Fixing != INDEX_NONE);
	TestTrue(TEXT("JOINT: the overhang bears on both posts"),
		PostLOnOverhang != INDEX_NONE && PostROnOverhang != INDEX_NONE);

	if (DoorOnLeft == INDEX_NONE || DoorOnRight == INDEX_NONE || RidgeOnFront == INDEX_NONE
		|| Fixing == INDEX_NONE)
	{
		AddError(TEXT("JOINT: the carrying joints must exist before their roles can be read"));
		return false;
	}

	/* The door lintel bears on the pier through a BED joint beneath it (+Z normal), and the porch
	 * fixing is a Y-normal HEAD joint — a genuinely out-of-plane 3D contact a 2D oracle cannot express. */
	TestTrue(TEXT("JOINT: the door lintel bears on the left pier through a bed joint"),
		Layout.Structure.GetJointRole(DoorOnLeft, S.DoorHeader) == EJointRole::BedBeneath);
	TestTrue(TEXT("JOINT: the ridge bears on the apex through a bed joint"),
		Layout.Structure.GetJointRole(RidgeOnFront, S.Ridge) == EJointRole::BedBeneath);
	TestTrue(TEXT("JOINT: the porch fixing is a Y-normal head joint, |normal.Y| ~ 1 (out of the X-Z plane)"),
		FMath::IsNearlyEqual(FMath::Abs(Layout.Structure.GetConnection(Fixing).InterfaceNormal.Y), 1.0, 1.0e-9));
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

#endif // WITH_DEV_AUTOMATION_TESTS
