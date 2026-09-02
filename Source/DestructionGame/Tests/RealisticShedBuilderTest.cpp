// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/DestructionShed3D.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE REALISTIC-BRICK SHED SHELL — the v3 rebuild at TRUE MASONRY RESOLUTION. The recognizable v2 shed
 * (24 pieces, one coarse block per wall face) is physics-valid but is built of 3-metre "bricks"; the
 * user's ask is "rebuild with bricks that are the size of actual bricks. Wood that is the size of boards
 * and joints to be a realistic size and build." This drives the builder that lays REAL 21.5 x 10.25 x 6.5
 * cm clay bricks in a running (stretcher) bond on 1 cm mortar joints, single-brick-thick, closing a box,
 * with a real Timber-board lintel over a door opening and over a window opening.
 *
 * THE BEHAVIOUR, IN ONE SENTENCE. DestructionShed3D::BuildRealistic lays a realistic-brick shed SHELL —
 * four single-wythe running-bond ClayBrick walls of true-sized bricks on 1 cm joints closing a box, a
 * DOOR opening in the front wall and a WINDOW opening in the left wall, each a genuine gap spanned by a
 * real Timber-board lintel — as a SetThreeDimensional FBrickLayout whose hundreds of blocks put it above
 * the equilibrium gate's 200-block cap, so that (broken by the ROUTER, the per-joint capacity sweep that
 * is the authority above the cap) the assembled shell STANDS with nothing stranded.
 *
 * =========================================================================================
 * SCOPE — THIS SLICE IS THE REALISTIC-BRICK SHELL THAT STANDS. Deferred to later slices (see the report):
 *   - the stepped brick GABLES rising to a ridge, the timber gable ROOF (rafters / ridge / roof boards),
 *     and the PORCH overhang on two timber posts;
 *   - the COLLAPSE arms at scale (pull a pier / a post -> the right thing falls) — above the cap the
 *     router's per-joint sweep is the break authority, with its known limitations, so collapse gets its
 *     own slice with hand-derived expectations;
 *   - the scenario re-wire (a `shedrealistic` catalogue row + a Lvl_ map) and the in-engine render.
 *
 * =========================================================================================
 * THE CANONICAL SHELL — X IS WIDTH, Y IS DEPTH (INTO THE DOOR), Z IS HEIGHT. All dimensions cm at
 * Unreal's default 1 uu = 1 cm. The builder hardcodes these; the test PINS the sizing by searching the
 * laid layout (origin-independent) and reads the openings back at fixed points.
 *
 *   BRICK: 21.5 (X) x 10.25 (Y) x 6.5 (Z), ClayBrick 1.9 g/cm3. Half-extents (10.75, 5.125, 3.25).
 *   JOINT: 1.0 cm bed AND perpend, so the coordinating grid is 22.5 (pitch X) x 11.25 (half-cell bond
 *          offset) x 7.5 (course pitch Z). A brick + a perpend sit on a 22.5 cm pitch, so two bricks along
 *          a course whose centres are 22.5 apart prove a 1 cm perpend (22.5 - 21.5). Alternate courses are
 *          offset half a cell (11.25 cm) — a real running bond, so perpends do not line up.
 *
 *   FOOTPRINT: outer box X[0,180], Y[0,140]; every wall ONE brick (10.25) thick; eaves at Z=120 (16
 *   courses of 7.5). FRONT wall Y[0,10.25], BACK wall Y[129.75,140], LEFT wall X[0,10.25], RIGHT wall
 *   X[169.75,180]; the side walls run between the front and back walls with a 1 cm gap at each corner, so
 *   each corner is a genuine Y-normal mortar joint out of the X-Z plane — the one fact a 2D section cannot
 *   hold, which is why the shell is flagged 3D.
 *
 *   DOOR (front wall): a gap X[57.5,122.5] (width 65, ~3 pitches), Z[0,90] (12 courses), flanked by brick
 *   piers; a Timber-board LINTEL X[50,130] Y[0,10.25] Z[90,97] (a 7 cm board) bears on the two piers.
 *   WINDOW (left wall): a gap Y[55,95] (width 40), Z[45,90], on a brick sill with brick jambs either side;
 *   a Timber-board LINTEL X[0,10.25] Y[50,100] Z[90,97] bears on the two jambs.
 *
 * =========================================================================================
 * WHY THE ROUTER, NOT THE LP. At real brick resolution the shell is hundreds of blocks; the equilibrium
 * gate declines above its 200-block cap (FStructure::EquilibriumGateBlockCap, default 200) and the
 * per-joint capacity sweep breaks AND enumerates support — the same path the flagship ~1200-block wall
 * uses. The router is dimension-agnostic (it routes load down bed joints and reads joint normals
 * directly), so a genuinely-3D box is carried without ever reaching the 3D LP: every brick has a downward
 * bed-joint path to a grounded base course, the lintels bear on their piers/jambs, and a fully-intact
 * standing wall strands nothing. STANDS is therefore a router verdict here; the LP's 3D machinery is
 * bypassed until a below-cap (coarser) slice.
 *
 * =========================================================================================
 * UNITS — SPELLED OUT LOCALLY (DESIGN.md §3): 1 uu = 1 cm; mass (kg) and density (g/cm3) go in
 * unconverted; weight = MassKg * 980 already contains 1 N = 100 uu. No strength-vs-force comparison is
 * made in this slice (STANDS is a support-state question), so no MPa->uu factor is needed here.
 *
 * NEEDS A TICKING WORLD: NO. The builder is arithmetic over boxes; the structure over a graph; the router
 * over that; gravity is on (weight = mass*980); every assertion is on the laid layout or the solved
 * support state — no Chaos, no world tick. Same footing as the other shed builder tests.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges files into one translation unit.
 */
namespace RealisticShedTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/* ================================================================================
	 * THE REAL BRICK, as half-extents, and the coordinating grid it sits on.
	 * ================================================================================ */

	constexpr double BrickHalfXCm = 21.5 / 2.0;    // 10.75
	constexpr double BrickHalfYCm = 10.25 / 2.0;   // 5.125  — the single-wythe half-thickness
	constexpr double BrickHalfZCm = 6.5 / 2.0;     // 3.25   — the one-course half-height

	/* A half bat is (BrickX - joint)/2 = 10.25 long, so its long HALF-extent is 5.125. A brick's short
	 * half-extents (5.125 wythe, 3.25 course) are shared by every piece; the long half-extent is 10.75 for
	 * a full brick or 5.125 for a half bat. */
	constexpr double FullBrickLongHalfCm = 10.75;
	constexpr double HalfBatLongHalfCm = (21.5 - 1.0) / 2.0 / 2.0;   // (20.5)/2 /2 = 5.125

	constexpr double BrickPitchCm = 22.5;          // 21.5 + 1.0 perpend -> a 1 cm joint
	constexpr double CoursePitchCm = 7.5;          // 6.5 + 1.0 bed
	constexpr double BondOffsetCm = 11.25;         // half a cell — the running-bond offset

	constexpr double Tol = 0.05;

	/* ================================================================================
	 * OPENING PROBE POINTS — fixed points the builder's canonical geometry places inside each gap, inside
	 * the flanking piers/jambs, and inside each lintel. Interior to the openings with margin, so the
	 * builder has latitude in exactly how it lays the running bond around them.
	 * ================================================================================ */

	const FVector DoorGap(90.0, 5.0, 45.0);        // mid-doorway, must be EMPTY
	const FVector DoorPierLeft(30.0, 5.0, 45.0);   // brick pier left of the door
	const FVector DoorPierRight(150.0, 5.0, 45.0); // brick pier right of the door
	const FVector DoorLintelPt(90.0, 5.0, 93.0);   // inside the Timber door lintel

	const FVector WindowGap(5.0, 75.0, 68.0);      // mid-window, must be EMPTY
	const FVector WindowSillPt(5.0, 75.0, 20.0);   // brick sill below the window
	const FVector WindowLintelPt(5.0, 75.0, 93.0); // inside the Timber window lintel

	/* ================================================================================
	 * SEARCH HELPERS — origin-independent identity by SIZE, MATERIAL and POSITION, never by handle order.
	 * ================================================================================ */

	bool Near(double A, double B)
	{
		return FMath::IsNearlyEqual(A, B, Tol);
	}

	/** The three half-extents of a box, ascending — so a brick reads the same whichever way it is laid. */
	void SortedHalfExtents(const FPieceBox& Box, double& OutLo, double& OutMid, double& OutHi)
	{
		double E[3] = { FMath::Abs(Box.ExtentCm.X), FMath::Abs(Box.ExtentCm.Y), FMath::Abs(Box.ExtentCm.Z) };
		for (int32 I = 0; I < 3; ++I)
		{
			for (int32 J = I + 1; J < 3; ++J)
			{
				if (E[J] < E[I])
				{
					Swap(E[I], E[J]);
				}
			}
		}
		OutLo = E[0];
		OutMid = E[1];
		OutHi = E[2];
	}

	/** A real brick section: one course high (3.25), one wythe thick (5.125), full brick or half bat long. */
	bool IsRealBrickSection(const FPieceBox& Box)
	{
		double Lo = 0.0, Mid = 0.0, Hi = 0.0;
		SortedHalfExtents(Box, Lo, Mid, Hi);

		const bool bCourseHigh = Near(Lo, BrickHalfZCm);
		const bool bOneWythe = Near(Mid, BrickHalfYCm);
		const bool bBrickLong = Near(Hi, FullBrickLongHalfCm) || Near(Hi, HalfBatLongHalfCm);

		return bCourseHigh && bOneWythe && bBrickLong;
	}

	/** A full (not half-bat) brick, 21.5 x 10.25 x 6.5 in some axis permutation. */
	bool IsFullBrick(const FPieceBox& Box)
	{
		double Lo = 0.0, Mid = 0.0, Hi = 0.0;
		SortedHalfExtents(Box, Lo, Mid, Hi);
		return Near(Lo, BrickHalfZCm) && Near(Mid, BrickHalfYCm) && Near(Hi, FullBrickLongHalfCm);
	}

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

	int32 CountMaterial(const FStructure& S, const FMaterialProfile* Material)
	{
		int32 N = 0;
		for (int32 Piece = 0; Piece < S.NumPieces(); ++Piece)
		{
			if (!S.IsPieceRemoved(Piece) && S.GetPiece(Piece).Material == Material)
			{
				++N;
			}
		}
		return N;
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

	/** Is P a ClayBrick full brick? (mass-material and size together). */
	bool IsFullBrickPiece(const FBrickLayout& Layout, int32 Piece)
	{
		return Layout.Structure.GetPiece(Piece).Material == &ClayBrick
			&& Layout.Boxes.IsValidIndex(Piece) && IsFullBrick(Layout.Boxes[Piece]);
	}

	/**
	 * Does the wall show a RUNNING BOND — a pair of full bricks in the SAME plane, one course apart in Z
	 * (7.5), sharing the wall's thin coordinate, whose long-axis centres are offset half a cell (11.25)?
	 * A stack bond (offset 0) and a coarse single block both fail this.
	 */
	bool HasRunningBondOffset(const FBrickLayout& Layout)
	{
		const FStructure& S = Layout.Structure;
		for (int32 A = 0; A < S.NumPieces(); ++A)
		{
			if (!IsFullBrickPiece(Layout, A))
			{
				continue;
			}
			const FVector CA = Layout.Boxes[A].CentreCm;
			const bool bRunsX = Near(FMath::Abs(Layout.Boxes[A].ExtentCm.X), FullBrickLongHalfCm);

			for (int32 B = 0; B < S.NumPieces(); ++B)
			{
				if (B == A || !IsFullBrickPiece(Layout, B))
				{
					continue;
				}
				const FVector CB = Layout.Boxes[B].CentreCm;

				if (!Near(FMath::Abs(CA.Z - CB.Z), CoursePitchCm))
				{
					continue;
				}
				if (bRunsX)
				{
					if (Near(CA.Y, CB.Y) && Near(FMath::Abs(CA.X - CB.X), BondOffsetCm))
					{
						return true;
					}
				}
				else
				{
					if (Near(CA.X, CB.X) && Near(FMath::Abs(CA.Y - CB.Y), BondOffsetCm))
					{
						return true;
					}
				}
			}
		}
		return false;
	}

	/**
	 * Do two full bricks sit on the 22.5 cm coordinating PITCH along a course (same Z, same thin
	 * coordinate)? Given a 21.5 cm brick, a 22.5 cm pitch is a 1 cm perpend — the realistic joint.
	 */
	bool HasOneCentimetrePerpend(const FBrickLayout& Layout)
	{
		const FStructure& S = Layout.Structure;
		for (int32 A = 0; A < S.NumPieces(); ++A)
		{
			if (!IsFullBrickPiece(Layout, A))
			{
				continue;
			}
			const FVector CA = Layout.Boxes[A].CentreCm;
			const bool bRunsX = Near(FMath::Abs(Layout.Boxes[A].ExtentCm.X), FullBrickLongHalfCm);

			for (int32 B = 0; B < S.NumPieces(); ++B)
			{
				if (B == A || !IsFullBrickPiece(Layout, B))
				{
					continue;
				}
				const FVector CB = Layout.Boxes[B].CentreCm;
				if (!Near(CA.Z, CB.Z))
				{
					continue;
				}
				if (bRunsX)
				{
					if (Near(CA.Y, CB.Y) && Near(FMath::Abs(CA.X - CB.X), BrickPitchCm))
					{
						return true;
					}
				}
				else
				{
					if (Near(CA.X, CB.X) && Near(FMath::Abs(CA.Y - CB.Y), BrickPitchCm))
					{
						return true;
					}
				}
			}
		}
		return false;
	}

	/** Does any joint face out of the X-Z plane (a Y-normal corner), the mark of a genuinely-3D box? */
	bool HasYNormalCorner(const FStructure& S)
	{
		for (int32 J = 0; J < S.NumConnections(); ++J)
		{
			if (FMath::IsNearlyEqual(FMath::Abs(S.GetConnection(J).InterfaceNormal.Y), 1.0, 1.0e-9))
			{
				return true;
			}
		}
		return false;
	}

	/* ================================================================================
	 * WHICH WALL A BRICK BELONGS TO, by the band its thin coordinate sits in. The canonical shell
	 * closes on Y[10.25] (front inner face), Y[123.75] (back inner face), X[10.25] (left inner face)
	 * and X[169.75] (right inner face), so the wall centres are Y=5.125 (front), Y=128.875 (back),
	 * X=5.125 (left) and X=174.875 (right). The front/back test is tried FIRST so a corner half-bat —
	 * which is square in plan (5.125 x 5.125) and so sits on both a front centre and a left centre — is
	 * read as the front/back wall it is laid into, not mistaken for a side wall.
	 * ================================================================================ */

	enum class EWall { None, Front, Back, Left, Right };

	EWall WallOf(const FBrickLayout& Layout, int32 Piece)
	{
		if (!Layout.Boxes.IsValidIndex(Piece)
			|| Layout.Structure.GetPiece(Piece).Material != &ClayBrick)
		{
			return EWall::None;
		}
		const FVector C = Layout.Boxes[Piece].CentreCm;
		if (Near(C.Y, 5.125))
		{
			return EWall::Front;
		}
		if (Near(C.Y, 128.875))
		{
			return EWall::Back;
		}
		if (Near(C.X, 5.125) && C.Y > 10.5 && C.Y < 123.5)
		{
			return EWall::Left;
		}
		if (Near(C.X, 174.875) && C.Y > 10.5 && C.Y < 123.5)
		{
			return EWall::Right;
		}
		return EWall::None;
	}

	/**
	 * Is there a GENUINE wall-to-wall corner joint between the named front/back wall and the named side
	 * wall? A corner is a Y-normal MakeInterface joint whose two pieces belong to DIFFERENT walls — the
	 * side wall's end face meeting the front or back wall across the 1 cm corner mortar joint. A side
	 * wall's own in-course Y-normal perpends never satisfy this: both their pieces sit in the SAME side
	 * wall, so the pair is (Left, Left), never (Front, Left). A detached back wall makes no such joint
	 * at all, so the back corners simply do not exist — which is the open box this assertion catches.
	 */
	bool HasCornerJoint(const FBrickLayout& Layout, EWall FrontOrBack, EWall Side)
	{
		const FStructure& S = Layout.Structure;
		for (int32 J = 0; J < S.NumConnections(); ++J)
		{
			const FConnection& Cn = S.GetConnection(J);
			if (!FMath::IsNearlyEqual(FMath::Abs(Cn.InterfaceNormal.Y), 1.0, 1.0e-9))
			{
				continue;
			}
			const EWall WA = WallOf(Layout, Cn.PieceA);
			const EWall WB = WallOf(Layout, Cn.PieceB);
			if ((WA == FrontOrBack && WB == Side) || (WA == Side && WB == FrontOrBack))
			{
				return true;
			}
		}
		return false;
	}

	/* ================================================================================
	 * GABLE + ROOF SCANNING (slice 2). The shell's 16 courses reach the eaves at Z = 119 (course 15 top,
	 * 15 * 7.5 + 6.5). The stepped gables continue real-brick courses ABOVE that on the two GABLE-END
	 * walls (front, Y-centre 5.125, which carries the door; back, Y-centre 128.875), each course stepping
	 * IN toward the box centre X = 90 so the gable narrows to an apex under the ridge. The Timber roof
	 * (stepped purlins + a ridge board) bears on the gable shoulders. Everything is found by scanning the
	 * laid layout — origin-independent, robust to the exact bond the builder chooses inside each gable.
	 * ================================================================================ */

	constexpr double EavesTopZCm = 119.0;    // course 15 (0-based) top: 15 * 7.5 + 6.5
	constexpr double GableFloorZCm = 119.5;  // a hair above the eaves — a gable brick sits above this

	constexpr double FrontGableYCentreCm = 5.125;    // front wall band Y[0,10.25]
	constexpr double BackGableYCentreCm = 128.875;   // back wall band Y[123.75,134]

	/** The Z centre of a piece's box. */
	double CentreZ(const FBrickLayout& Layout, int32 Piece)
	{
		return Layout.Boxes[Piece].CentreCm.Z;
	}

	/**
	 * The X-width of every gable brick COURSE above the eaves on one gable end (front or back), returned
	 * ascending in Z. A stepped gable NARROWS: each higher course is strictly less wide, reaching a narrow
	 * apex. A course is one Z band of ClayBrick bricks in the gable end's thin Y band, above the eaves.
	 */
	void GableCourseWidths(const FBrickLayout& Layout, double GableYCentreCm,
		TArray<double>& OutZ, TArray<double>& OutWidth)
	{
		const FStructure& S = Layout.Structure;

		TArray<double> Zs;
		TArray<double> LoX;
		TArray<double> HiX;

		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (S.IsPieceRemoved(P) || S.GetPiece(P).Material != &ClayBrick || !Layout.Boxes.IsValidIndex(P))
			{
				continue;
			}
			const FVector C = Layout.Boxes[P].CentreCm;
			if (C.Z <= GableFloorZCm || !Near(C.Y, GableYCentreCm))
			{
				continue;
			}

			const double Lo = C.X - FMath::Abs(Layout.Boxes[P].ExtentCm.X);
			const double Hi = C.X + FMath::Abs(Layout.Boxes[P].ExtentCm.X);

			int32 Bucket = INDEX_NONE;
			for (int32 K = 0; K < Zs.Num(); ++K)
			{
				if (Near(Zs[K], C.Z))
				{
					Bucket = K;
					break;
				}
			}
			if (Bucket == INDEX_NONE)
			{
				Zs.Add(C.Z);
				LoX.Add(Lo);
				HiX.Add(Hi);
			}
			else
			{
				LoX[Bucket] = FMath::Min(LoX[Bucket], Lo);
				HiX[Bucket] = FMath::Max(HiX[Bucket], Hi);
			}
		}

		TArray<int32> Order;
		for (int32 I = 0; I < Zs.Num(); ++I)
		{
			Order.Add(I);
		}
		Order.Sort([&](int32 A, int32 B) { return Zs[A] < Zs[B]; });

		for (const int32 I : Order)
		{
			OutZ.Add(Zs[I]);
			OutWidth.Add(HiX[I] - LoX[I]);
		}
	}

	/**
	 * Do two GABLE bricks (both ClayBrick, both above the eaves, both in the named gable end's thin Y band)
	 * bed on one another — a Z-normal MakeInterface joint between stepped gable courses? This is the
	 * "bedded — a joint to the course below" property: the stepped courses are genuinely bonded, not
	 * floating shelves.
	 */
	bool HasGableStepBed(const FBrickLayout& Layout, double GableYCentreCm)
	{
		const FStructure& S = Layout.Structure;
		for (int32 J = 0; J < S.NumConnections(); ++J)
		{
			const FConnection& Cn = S.GetConnection(J);
			if (!FMath::IsNearlyEqual(FMath::Abs(Cn.InterfaceNormal.Z), 1.0, 1.0e-9))
			{
				continue;
			}
			const int32 A = Cn.PieceA;
			const int32 B = Cn.PieceB;
			if (!Layout.Boxes.IsValidIndex(A) || !Layout.Boxes.IsValidIndex(B)
				|| S.GetPiece(A).Material != &ClayBrick || S.GetPiece(B).Material != &ClayBrick)
			{
				continue;
			}
			const FVector CA = Layout.Boxes[A].CentreCm;
			const FVector CB = Layout.Boxes[B].CentreCm;
			if (Near(CA.Y, GableYCentreCm) && Near(CB.Y, GableYCentreCm)
				&& CA.Z > GableFloorZCm && CB.Z > GableFloorZCm)
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * Does a Timber ROOF member bear on a gable brick — a Z-normal joint between a Timber piece above the
	 * eaves and a ClayBrick gable brick? This is the roof "bearing on the gable shoulders" property: the
	 * roof is carried by the masonry, not floating.
	 */
	bool HasRoofBearingOnGable(const FBrickLayout& Layout)
	{
		const FStructure& S = Layout.Structure;
		for (int32 J = 0; J < S.NumConnections(); ++J)
		{
			const FConnection& Cn = S.GetConnection(J);
			if (!FMath::IsNearlyEqual(FMath::Abs(Cn.InterfaceNormal.Z), 1.0, 1.0e-9))
			{
				continue;
			}
			const int32 A = Cn.PieceA;
			const int32 B = Cn.PieceB;
			if (!Layout.Boxes.IsValidIndex(A) || !Layout.Boxes.IsValidIndex(B))
			{
				continue;
			}
			const FMaterialProfile* MA = S.GetPiece(A).Material;
			const FMaterialProfile* MB = S.GetPiece(B).Material;
			const bool bTimberAbove = (MA == &Timber && CentreZ(Layout, A) > GableFloorZCm)
				|| (MB == &Timber && CentreZ(Layout, B) > GableFloorZCm);
			const bool bGableBrick = (MA == &ClayBrick && CentreZ(Layout, A) > GableFloorZCm)
				|| (MB == &ClayBrick && CentreZ(Layout, B) > GableFloorZCm);
			if (bTimberAbove && bGableBrick)
			{
				return true;
			}
		}
		return false;
	}

	/** The Timber roof member with the greatest Z centre — the ridge — or INDEX_NONE if the roof is absent. */
	int32 RidgePiece(const FBrickLayout& Layout)
	{
		const FStructure& S = Layout.Structure;
		int32 Best = INDEX_NONE;
		double BestZ = -DBL_MAX;
		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (S.IsPieceRemoved(P) || S.GetPiece(P).Material != &Timber || !Layout.Boxes.IsValidIndex(P))
			{
				continue;
			}
			const double Z = CentreZ(Layout, P);
			if (Z > GableFloorZCm && Z > BestZ)
			{
				BestZ = Z;
				Best = P;
			}
		}
		return Best;
	}

	/** The topmost (apex) ClayBrick gable brick on one gable end, or INDEX_NONE. */
	int32 GableApexPiece(const FBrickLayout& Layout, double GableYCentreCm)
	{
		const FStructure& S = Layout.Structure;
		int32 Best = INDEX_NONE;
		double BestZ = -DBL_MAX;
		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (S.IsPieceRemoved(P) || S.GetPiece(P).Material != &ClayBrick || !Layout.Boxes.IsValidIndex(P))
			{
				continue;
			}
			const FVector C = Layout.Boxes[P].CentreCm;
			if (C.Z > GableFloorZCm && Near(C.Y, GableYCentreCm) && C.Z > BestZ)
			{
				BestZ = C.Z;
				Best = P;
			}
		}
		return Best;
	}

	/* ================================================================================
	 * PORCH SCANNING (slice 3). A canopy over the DOOR: two grounded Timber POSTS flanking the
	 * doorway, a Timber OVERHANG board bearing on both posts and cantilevering OUT over the door,
	 * and a narrow wall FIXING tying the overhang's back down — a tension-capable fastener (a Screw
	 * WITHDRAWAL tie), the one joint a compression-only DryStone bearing cannot be.
	 *
	 * WHICH WAY IS "OUT". The front wall (which carries the door) runs along X in the thin Y band
	 * Y[0,10.25]; its OUTER face is Y = 0 and the box interior is +Y (toward the back wall at
	 * Y[123.75,134]). So the porch cantilevers over the door in NEGATIVE Y, and every porch piece
	 * has a centre at Y < 0. The door gap is X[57.5,122.5] (centre X = 90), so the two posts flank
	 * it: one centre X < 90, one X > 90.
	 *
	 * A POST NEEDS NO COORDINATE TO FIND: the porch posts are the ONLY grounded Timber pieces in the
	 * whole shed. Every shell / gable / roof Timber — the two lintels, the five roof members — is
	 * free; only course-0 ClayBricks are grounded. So a grounded Timber piece IS a porch post,
	 * identified by material and grounding alone, robust to the exact coordinates the builder picks.
	 * ================================================================================ */

	constexpr double PostHalfSectionCm = 5.0;    // a 10 x 10 cm post section (a real 4x4 timber)

	bool IsGroundedTimber(const FStructure& S, int32 Piece)
	{
		return !S.IsPieceRemoved(Piece)
			&& S.GetPiece(Piece).Material == &Timber
			&& S.GetPiece(Piece).bIsGrounded;
	}

	/** A 10 x 10 cm post section: the two smallest half-extents ~ 5, the tall (Z) one larger. */
	bool IsTenByTenSection(const FPieceBox& Box)
	{
		double Lo = 0.0, Mid = 0.0, Hi = 0.0;
		SortedHalfExtents(Box, Lo, Mid, Hi);
		return Near(Lo, PostHalfSectionCm) && Near(Mid, PostHalfSectionCm) && Hi > PostHalfSectionCm + Tol;
	}

	void FindPosts(const FBrickLayout& Layout, TArray<int32>& OutPosts)
	{
		const FStructure& S = Layout.Structure;
		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (IsGroundedTimber(S, P) && Layout.Boxes.IsValidIndex(P))
			{
				OutPosts.Add(P);
			}
		}
	}

	/** Do two pieces share a joint, in either A/B order? */
	bool SharesJoint(const FStructure& S, int32 P, int32 Q)
	{
		for (int32 J = 0; J < S.NumConnections(); ++J)
		{
			const FConnection& Cn = S.GetConnection(J);
			if ((Cn.PieceA == P && Cn.PieceB == Q) || (Cn.PieceA == Q && Cn.PieceB == P))
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * The porch OVERHANG: the free Timber board that bears on BOTH posts. Found by its joints, not
	 * its coordinates — the one free Timber piece jointed to both grounded-Timber posts. A cleat (if
	 * the builder uses one) is jointed to the overhang and the wall, never to both posts, so it is
	 * never mistaken for the overhang.
	 */
	int32 FindOverhang(const FBrickLayout& Layout, const TArray<int32>& Posts)
	{
		if (Posts.Num() < 2)
		{
			return INDEX_NONE;
		}
		const FStructure& S = Layout.Structure;
		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (S.IsPieceRemoved(P) || S.GetPiece(P).Material != &Timber
				|| S.GetPiece(P).bIsGrounded || !Layout.Boxes.IsValidIndex(P))
			{
				continue;
			}
			bool bBearsOnAll = true;
			for (const int32 Post : Posts)
			{
				if (!SharesJoint(S, P, Post))
				{
					bBearsOnAll = false;
					break;
				}
			}
			if (bBearsOnAll)
			{
				return P;
			}
		}
		return INDEX_NONE;
	}

	/**
	 * Is there a tension-capable FASTENER — a Screw-style WITHDRAWAL tie — incident to this piece? A
	 * withdrawal tie carries real tension AND has zero friction (a mechanical fastener). The post
	 * bearings are compression-only DryStone (Tensile 0) and every mortar joint has friction 0.75, so
	 * this predicate reads the fixing and nothing else. It is asserted on the OVERHANG, so it fires
	 * whether the builder ties the overhang straight to the wall or laps it onto a wall-anchored cleat
	 * — either way the tie is a joint incident to the overhang.
	 */
	bool HasWithdrawalTie(const FStructure& S, int32 Piece)
	{
		for (int32 J = 0; J < S.NumConnections(); ++J)
		{
			const FConnection& Cn = S.GetConnection(J);
			if (Cn.PieceA != Piece && Cn.PieceB != Piece)
			{
				continue;
			}
			if (Cn.Strength.TensileStrengthMPa > 0.0
				&& FMath::IsNearlyEqual(Cn.Strength.FrictionCoefficient, 0.0))
			{
				return true;
			}
		}
		return false;
	}
}

/**
 * THE REALISTIC-BRICK SHED SHELL BUILDS FROM TRUE-SIZED BRICKS IN RUNNING BOND, CLOSES A BOX WITH A DOOR
 * AND A WINDOW OPENING UNDER TIMBER-BOARD LINTELS, AND STANDS THROUGH THE ROUTER AT SCALE.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRealisticShedBuilderTest,
	"DestructionGame.Acceptance.Shed.ThreeD.RealisticBrickShedShellStandsAsBuilt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRealisticShedBuilderTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace RealisticShedTestSupport;

	/* ------------------------------------------------------------------ *
	 * FIXTURE PRECONDITIONS — the sizing is derived against the published real-brick dimensions and the
	 * material identities, pinned here rather than read from a spec so a wrong constant fails visibly.
	 * ------------------------------------------------------------------ */

	TestEqual(TEXT("FIXTURE: clay brick density is 1.9 g/cm3 (UK metric standard brick)"),
		ClayBrick.DensityGramsPerCubicCm, 1.9);
	TestTrue(TEXT("FIXTURE: GeneralPurposeMortar is a bonded joint (cohesion and tension)"),
		GeneralPurposeMortar.ShearCohesionMPa > 0.0 && GeneralPurposeMortar.TensileStrengthMPa > 0.0);
	TestTrue(TEXT("FIXTURE: the real brick is 21.5 x 10.25 x 6.5 cm — half-extents (10.75, 5.125, 3.25)"),
		Near(BrickHalfXCm, 10.75) && Near(BrickHalfYCm, 5.125) && Near(BrickHalfZCm, 3.25));

	/* ================================================================================
	 * ARM 0 — THE BUILDER LAYS THE REALISTIC-BRICK SHELL. This is where the stub is RED: it lays nothing,
	 * so BuildRealistic returns false and the shell cannot be examined.
	 * ================================================================================ */

	FBrickLayout Layout;
	const bool bBuilt = DestructionShed3D::BuildRealistic(Layout);

	TestTrue(TEXT("BUILD: the builder must lay the realistic-brick shed shell (the stub returns false — the RED)"),
		bBuilt);

	if (!bBuilt)
	{
		AddError(TEXT("BUILD: DestructionShed3D::BuildRealistic is a fail-closed stub — it lays nothing yet. "
			"This is the expected RED: dev-expert lays the real running-bond shell to the canonical dimensions "
			"in the file header. Everything below is unreachable until then."));
		return false;
	}

	TestTrue(TEXT("BUILD: the shell must be flagged 3D so its Y-normal corner joints are genuine (below cap)"),
		Layout.Structure.IsThreeDimensional());
	TestEqual(TEXT("BUILD: one box per piece, or AdoptLayout refuses the layout"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());

	/* ================================================================================
	 * ARM 1 — REALISTIC SIZING (the heart of the goal). Real bricks, a running-bond half-cell offset, a
	 * 1 cm perpend, single-wythe walls, and a real Timber board over each opening. A coarse build (one
	 * 3-metre block per wall) fails every one of these.
	 * ================================================================================ */

	int32 NumBricks = 0;
	int32 NumFullBricks = 0;
	bool bEveryBrickIsRealSection = true;
	for (int32 Piece = 0; Piece < Layout.Structure.NumPieces(); ++Piece)
	{
		if (Layout.Structure.IsPieceRemoved(Piece)
			|| Layout.Structure.GetPiece(Piece).Material != &ClayBrick)
		{
			continue;
		}
		++NumBricks;
		if (IsFullBrick(Layout.Boxes[Piece]))
		{
			++NumFullBricks;
		}
		if (!IsRealBrickSection(Layout.Boxes[Piece]))
		{
			bEveryBrickIsRealSection = false;
		}
	}

	AddInfo(FString::Printf(TEXT("SIZING: %d ClayBrick pieces (%d full bricks); %d pieces total, %d joints."),
		NumBricks, NumFullBricks, Layout.Structure.NumPieces(), Layout.Structure.NumConnections()));

	TestTrue(TEXT("SIZING: a real full brick (21.5 x 10.25 x 6.5) exists in the shell"),
		NumFullBricks > 0);
	TestTrue(TEXT("SIZING: EVERY clay brick is a real brick section — one wythe (10.25) thick, one course "
		"(6.5) high (a coarse per-wall block fails here)"),
		bEveryBrickIsRealSection);
	TestTrue(TEXT("SIZING: the shell is HUNDREDS of real bricks, not a handful of coarse blocks"),
		NumBricks > 200);
	TestTrue(TEXT("SIZING: alternate courses are offset half a brick (11.25) — a running bond, not a stack"),
		HasRunningBondOffset(Layout));
	TestTrue(TEXT("SIZING: bricks sit on a 22.5 cm course pitch — a 1 cm perpend joint (22.5 - 21.5)"),
		HasOneCentimetrePerpend(Layout));

	/* --- TIMBER BOARD SECTIONS OVER THE OPENINGS ---------------------------------------------- */

	const int32 DoorLintel = PieceContaining(Layout, DoorLintelPt);
	const int32 WindowLintel = PieceContaining(Layout, WindowLintelPt);

	TestTrue(TEXT("SIZING: a Timber-board door lintel sits over the doorway"),
		DoorLintel != INDEX_NONE && Layout.Structure.GetPiece(DoorLintel).Material == &Timber);
	TestTrue(TEXT("SIZING: a Timber-board window lintel sits over the window"),
		WindowLintel != INDEX_NONE && Layout.Structure.GetPiece(WindowLintel).Material == &Timber);

	if (DoorLintel != INDEX_NONE)
	{
		/* A board, not a chunk: its smallest half-extent is <= 5 cm (a <=10 cm-thick section). */
		double Lo = 0.0, Mid = 0.0, Hi = 0.0;
		SortedHalfExtents(Layout.Boxes[DoorLintel], Lo, Mid, Hi);
		AddInfo(FString::Printf(TEXT("SIZING: door lintel half-extents sorted (%.4g, %.4g, %.4g)"), Lo, Mid, Hi));
		TestTrue(TEXT("SIZING: the door lintel is a real BOARD section (<= 10 cm thick), not a coarse block"),
			Lo <= 5.0);
	}

	/* ================================================================================
	 * ARM 2 — IT IS A SHED. Genuine door and window GAPS flanked by masonry, a multi-material box, and the
	 * out-of-plane corner joints that make the closed box genuinely 3D.
	 * ================================================================================ */

	TestEqual(TEXT("OPENING: the doorway is a GAP — no piece fills it between the piers"),
		PieceContaining(Layout, DoorGap), (int32)INDEX_NONE);
	TestEqual(TEXT("OPENING: the window is a GAP — no piece fills it between the jambs"),
		PieceContaining(Layout, WindowGap), (int32)INDEX_NONE);

	TestTrue(TEXT("OPENING: brick piers flank the doorway left and right"),
		PieceContaining(Layout, DoorPierLeft) != INDEX_NONE
			&& PieceContaining(Layout, DoorPierRight) != INDEX_NONE);
	TestTrue(TEXT("OPENING: a brick sill sits below the window"),
		PieceContaining(Layout, WindowSillPt) != INDEX_NONE);

	TestTrue(TEXT("BUILD: the shell is multi-material — clay bricks and timber boards"),
		CountMaterial(Layout.Structure, &ClayBrick) > 0 && CountMaterial(Layout.Structure, &Timber) > 0);
	TestTrue(TEXT("BUILD: the shed knows where every piece and joint is (else every moment is silently zero)"),
		Layout.Structure.HasCompleteGeometry());
	TestTrue(TEXT("SHED: a corner joint faces out of the X-Z plane (|normal.Y| ~ 1) — the box is genuinely 3D"),
		HasYNormalCorner(Layout.Structure));

	/*
	 * THE BOX ACTUALLY CLOSES — all four corners bonded, not just the front pair. Each corner must be a
	 * genuine Y-normal mortar joint between a front/back wall brick and a side wall brick (different
	 * walls), so a detached back wall — whose bricks make no corner joint to either side wall — fails
	 * the back-left and back-right assertions rather than passing on the side walls' own perpends.
	 */
	TestTrue(TEXT("SHED: the FRONT-LEFT corner is a genuine wall-to-wall Y-normal mortar joint"),
		HasCornerJoint(Layout, EWall::Front, EWall::Left));
	TestTrue(TEXT("SHED: the FRONT-RIGHT corner is a genuine wall-to-wall Y-normal mortar joint"),
		HasCornerJoint(Layout, EWall::Front, EWall::Right));
	TestTrue(TEXT("SHED: the BACK-LEFT corner is a genuine wall-to-wall Y-normal mortar joint (open box fails here)"),
		HasCornerJoint(Layout, EWall::Back, EWall::Left));
	TestTrue(TEXT("SHED: the BACK-RIGHT corner is a genuine wall-to-wall Y-normal mortar joint (open box fails here)"),
		HasCornerJoint(Layout, EWall::Back, EWall::Right));

	/* ================================================================================
	 * ARM 3 — IT STANDS, THROUGH PRODUCTION (SolveAndBreak). At this resolution the shell is over the
	 * 200-block cap, so the ROUTER (the per-joint capacity sweep) is the break authority — the same path
	 * the flagship wall uses. A standing shell strands nothing and drops nothing; the lintels read
	 * Supported. NEVER displacement — support state only (DESIGN §4).
	 * ================================================================================ */

	const int32 PiecesBefore = Layout.Structure.NumPieces();

	TestTrue(*FString::Printf(TEXT("SCALE: %d blocks is above the 200-block equilibrium-gate cap, so the "
		"ROUTER is the break authority (the 3D LP is bypassed)"), PiecesBefore),
		PiecesBefore > 200);

	const int32 Passes = Layout.Structure.SolveAndBreak();
	const int32 Stranded = StrandedCount(Layout.Structure);

	AddInfo(FString::Printf(TEXT("STANDS: production ran %d pass(es); %d stranded."), Passes, Stranded));

	TestEqual(TEXT("STANDS: nothing may be Stranded — the verdict is about the shed, not the solver declining"),
		Stranded, 0);

	if (DoorLintel != INDEX_NONE)
	{
		TestTrue(TEXT("STANDS: the door lintel reads Supported (carried by its piers)"),
			IsStanding(Layout.Structure.GetPieceSupport(DoorLintel)));
	}
	if (WindowLintel != INDEX_NONE)
	{
		TestTrue(TEXT("STANDS: the window lintel reads Supported (carried by its jambs)"),
			IsStanding(Layout.Structure.GetPieceSupport(WindowLintel)));
	}

	return true;
}

/**
 * SLICE 2 — THE STEPPED BRICK GABLES AND THE TIMBER GABLE ROOF, on top of the standing realistic shell.
 *
 * THE BEHAVIOUR, IN ONE SENTENCE. On top of the real-brick shell, DestructionShed3D::BuildRealistic
 * continues real ClayBrick courses ABOVE the eaves on the two gable-end walls (front, which carries the
 * door, and back), each course stepping IN toward the box centre so the gable narrows symmetrically to an
 * apex under the ridge and beds on the course below, and lays a Timber roof of stepped board members
 * (purlins rising to a ridge) bearing on the gable shoulders — so that the whole shed (shell + gables +
 * roof) STANDS through production (SolveAndBreak / the router, above the 200-block cap) with nothing
 * stranded, the roof members and the gable apex reading Supported.
 *
 * THE GABLE, HAND-DERIVED. Eaves top Z = 119 (course 15). Continue courses 16..19 on each gable end, thin
 * Y band unchanged (front Y[0,10.25], back Y[123.75,134]), each course a band of real bricks centred on
 * the box's X centre (~89.5) and stepping IN one brick pitch (22.5) per side per course: course 16 spans
 * ~X[0,179] (8 bricks), 17 ~X[22.5,156.5] (6), 18 ~X[45,134] (4), 19 ~X[67.5,111.5] (2, the apex). The
 * symmetric narrowing keeps each course's centroid over the course below, so the corbelled gable cannot
 * overturn; each gable brick beds (1 cm) on a full-overlap brick below, so MakeInterface forms the beds.
 *
 * THE ROOF, HAND-DERIVED. Timber purlins run the full depth Y[0,134], bearing on BOTH gable shoulders
 * (a simply-supported beam between the two gable ends — its centroid sits between its two bearings, so it
 * cannot overturn). They step UP toward the centre onto successively higher shoulders — eaves purlins on
 * the course-16 shoulder (Z ~127.5), mid purlins on the course-18 shoulder (Z ~142.5) — and a ridge board
 * caps the apex course-19 shoulder (Z ~150). The stepped Z-levels read as a pitch; the ridge is the top.
 * Each member is a real board section (~5 cm thick). Load path: purlin -> gable shoulder -> gable courses
 * -> eaves wall -> ground. Symmetric, hand-derivably stable.
 *
 * THE RED. The builder lays the shell but NO gables and NO roof yet (its top course is 15, Z 119; the only
 * Timber is the two lintels at Z ~93). So every gable and roof scan below finds nothing: no courses above
 * the eaves, no ridge, no roof bearing. That is the expected RED — dev lays the stepped gables and the
 * roof to the geometry above. The shell arm re-checks that slice 1 still holds.
 *
 * NEEDS A TICKING WORLD: NO. Same footing as the shell test — boxes, doubles, the router; gravity on.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRealisticShedGablesAndRoofTest,
	"DestructionGame.Acceptance.Shed.ThreeD.RealisticBrickShedGablesAndRoofStandAsBuilt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRealisticShedGablesAndRoofTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace RealisticShedTestSupport;

	FBrickLayout Layout;
	const bool bBuilt = DestructionShed3D::BuildRealistic(Layout);

	TestTrue(TEXT("BUILD: the builder must lay the realistic-brick shed (the stub returns false — the RED)"),
		bBuilt);
	if (!bBuilt)
	{
		AddError(TEXT("BUILD: DestructionShed3D::BuildRealistic laid nothing — cannot examine gables/roof."));
		return false;
	}

	AddInfo(FString::Printf(TEXT("SCALE: %d pieces, %d joints total (shell + gables + roof)."),
		Layout.Structure.NumPieces(), Layout.Structure.NumConnections()));

	/* ================================================================================
	 * ARM 1 — STEPPED BRICK GABLES. Real-brick courses climb above the eaves on BOTH gable ends, narrowing
	 * strictly with height to an apex, each course bedded to the one below. The shell (top course Z 119)
	 * has no such courses, so these scans are the RED.
	 * ================================================================================ */

	for (int32 End = 0; End < 2; ++End)
	{
		const bool bFront = (End == 0);
		const double GableYCentre = bFront ? FrontGableYCentreCm : BackGableYCentreCm;
		const TCHAR* Name = bFront ? TEXT("front") : TEXT("back");

		TArray<double> CourseZ;
		TArray<double> CourseWidth;
		GableCourseWidths(Layout, GableYCentre, CourseZ, CourseWidth);

		FString Widths;
		for (int32 I = 0; I < CourseWidth.Num(); ++I)
		{
			Widths += FString::Printf(TEXT("[Z%.4g w%.4g]"), CourseZ[I], CourseWidth[I]);
		}
		AddInfo(FString::Printf(TEXT("GABLE(%s): %d courses above the eaves: %s"),
			Name, CourseWidth.Num(), *Widths));

		TestTrue(*FString::Printf(TEXT("GABLE(%s): real-brick courses rise above the eaves in >= 3 steps"), Name),
			CourseWidth.Num() >= 3);

		bool bNarrows = CourseWidth.Num() >= 2;
		for (int32 I = 1; I < CourseWidth.Num(); ++I)
		{
			if (!(CourseWidth[I] < CourseWidth[I - 1] - Tol))
			{
				bNarrows = false;
			}
		}
		TestTrue(*FString::Printf(TEXT("GABLE(%s): each course is strictly narrower than the one below "
			"(steps IN, symmetric, cannot overturn)"), Name), bNarrows);

		if (CourseWidth.Num() > 0)
		{
			TestTrue(*FString::Printf(TEXT("GABLE(%s): the apex course is narrow (<= ~2 bricks) — it reaches a ridge"),
				Name), CourseWidth.Last() <= 50.0);
		}

		TestTrue(*FString::Printf(TEXT("GABLE(%s): the stepped courses bed on one another (a Z-normal joint "
			"between two gable bricks)"), Name), HasGableStepBed(Layout, GableYCentre));
	}

	/* ================================================================================
	 * ARM 2 — TIMBER GABLE ROOF. Real board members span/bear on the gables, step up to read as a pitch,
	 * and are capped by a ridge at the top. All absent in the shell — the RED.
	 * ================================================================================ */

	int32 NumRoofMembers = 0;
	for (int32 P = 0; P < Layout.Structure.NumPieces(); ++P)
	{
		if (!Layout.Structure.IsPieceRemoved(P)
			&& Layout.Structure.GetPiece(P).Material == &Timber
			&& Layout.Boxes.IsValidIndex(P)
			&& CentreZ(Layout, P) > GableFloorZCm)
		{
			++NumRoofMembers;
		}
	}
	AddInfo(FString::Printf(TEXT("ROOF: %d Timber members above the eaves."), NumRoofMembers));

	TestTrue(TEXT("ROOF: Timber roof members sit above the eaves (a ridge board plus stepped purlins)"),
		NumRoofMembers >= 3);
	TestTrue(TEXT("ROOF: a Timber member bears on a gable brick (the roof is carried by the masonry)"),
		HasRoofBearingOnGable(Layout));

	const int32 Ridge = RidgePiece(Layout);
	TestTrue(TEXT("ROOF: a Timber ridge member exists at the top of the roof"), Ridge != INDEX_NONE);

	if (Ridge != INDEX_NONE)
	{
		double Lo = 0.0, Mid = 0.0, Hi = 0.0;
		SortedHalfExtents(Layout.Boxes[Ridge], Lo, Mid, Hi);
		AddInfo(FString::Printf(TEXT("ROOF: ridge half-extents sorted (%.4g, %.4g, %.4g), Z centre %.4g"),
			Lo, Mid, Hi, CentreZ(Layout, Ridge)));

		TestTrue(TEXT("ROOF: the ridge is a real BOARD section — smallest half-extent small (<= 5 cm)"),
			Lo <= 5.0);

		const int32 FrontApex = GableApexPiece(Layout, FrontGableYCentreCm);
		const int32 BackApex = GableApexPiece(Layout, BackGableYCentreCm);
		if (FrontApex != INDEX_NONE && BackApex != INDEX_NONE)
		{
			TestTrue(TEXT("ROOF: the ridge sits ABOVE the gable apex bricks (it is the top of the roof)"),
				CentreZ(Layout, Ridge) > CentreZ(Layout, FrontApex)
					&& CentreZ(Layout, Ridge) > CentreZ(Layout, BackApex));
		}
	}

	/* ================================================================================
	 * ARM 3 — IT STANDS, through production (SolveAndBreak / the router above the cap). Support-state only,
	 * never displacement (DESIGN §4): 0 stranded, the roof members read Supported, the gable apex bricks
	 * read Supported. The gable/apex/roof checks are guarded so that when they are ABSENT the RED lands on
	 * ARM 1/2 rather than crashing here.
	 * ================================================================================ */

	TestTrue(*FString::Printf(TEXT("SCALE: %d blocks is above the 200-block cap, so the router is authority"),
		Layout.Structure.NumPieces()), Layout.Structure.NumPieces() > 200);

	const int32 Passes = Layout.Structure.SolveAndBreak();
	const int32 Stranded = StrandedCount(Layout.Structure);
	AddInfo(FString::Printf(TEXT("STANDS: production ran %d pass(es); %d stranded."), Passes, Stranded));

	TestEqual(TEXT("STANDS: nothing may be Stranded — the whole shed (shell + gables + roof) stands"),
		Stranded, 0);

	if (Ridge != INDEX_NONE)
	{
		TestTrue(TEXT("STANDS: the ridge reads Supported (carried down the roof to the gables)"),
			IsStanding(Layout.Structure.GetPieceSupport(Ridge)));
	}

	int32 RoofChecked = 0;
	for (int32 P = 0; P < Layout.Structure.NumPieces(); ++P)
	{
		if (Layout.Structure.IsPieceRemoved(P)
			|| Layout.Structure.GetPiece(P).Material != &Timber
			|| !Layout.Boxes.IsValidIndex(P)
			|| CentreZ(Layout, P) <= GableFloorZCm)
		{
			continue;
		}
		++RoofChecked;
		TestTrue(*FString::Printf(TEXT("STANDS: roof member %d reads Supported (bears on the gables)"), P),
			IsStanding(Layout.Structure.GetPieceSupport(P)));
	}
	AddInfo(FString::Printf(TEXT("STANDS: checked %d roof member(s) for support."), RoofChecked));

	for (int32 End = 0; End < 2; ++End)
	{
		const bool bFront = (End == 0);
		const int32 Apex = GableApexPiece(Layout, bFront ? FrontGableYCentreCm : BackGableYCentreCm);
		if (Apex != INDEX_NONE)
		{
			TestTrue(*FString::Printf(TEXT("STANDS: the %s gable apex brick reads Supported"),
				bFront ? TEXT("front") : TEXT("back")),
				IsStanding(Layout.Structure.GetPieceSupport(Apex)));
		}
	}

	return true;
}

/**
 * SLICE 3 — THE PORCH: A DOOR CANOPY ON TWO REAL TIMBER POSTS, on top of the standing shell + gables
 * + roof. The SAME builder (DestructionShed3D::BuildRealistic) grows a porch over the door.
 *
 * THE BEHAVIOUR, IN ONE SENTENCE. On top of the realistic shed, DestructionShed3D::BuildRealistic lays
 * a porch over the DOOR — two GROUNDED Timber posts of a real 10 x 10 cm (4x4) section standing in front
 * of the door and flanking it, a Timber OVERHANG board of a real ~5 cm plank section bearing on both
 * posts and cantilevering OUT over the door away from the box interior, and a minimal wall FIXING (a
 * Screw withdrawal tie) tying the overhang back to the front wall — so that the whole shed (shell +
 * gables + roof + porch) STANDS through production (SolveAndBreak / the router, above the 200-block cap)
 * with nothing stranded, and the overhang and both posts read Supported / Grounded.
 *
 * =========================================================================================
 * THE PORCH, HAND-DERIVED — X IS WIDTH, Y IS DEPTH (INTO THE DOOR), Z IS HEIGHT, cm. The front wall's
 * OUTER face is Y = 0 and the box interior is +Y, so "OUT over the door" is NEGATIVE Y. The door gap is
 * X[57.5,122.5] (centre X = 90); the lintel footprint is X[50,130].
 *
 *   POSTS (2), Timber, GROUNDED, a 10 x 10 cm section rising from the ground to just under the overhang:
 *     PostL X[50,60] (centre 55), PostR X[120,130] (centre 125) — flanking the door gap; both Y[-25,-15]
 *     (15-25 cm out in front of the wall); Z[0,104].
 *   OVERHANG (1), Timber, free, a real ~5 cm plank: X[50,130] (spans the whole door), Y[-70,-2]
 *     (cantilevers 70 cm out, its back a hair off the wall so no spurious wall bearing forms), Z[105,110].
 *     Bottom Z = 105 sits one 1 cm joint above the post tops (Z = 104) — MakeInterface reads a DryStone
 *     bed BEARING on each post. Centroid Y = -36.
 *   FIXING — a narrow central Timber CLEAT, X[85,95] (10 cm wide, centred on the door), Y[-11,-1]
 *     (its back one 1 cm joint off the wall face Y = 0), Z[97.5,104]: a Y-normal GeneralPurposeMortar
 *     ANCHOR bonds it to the front-wall masonry over the door, and a Z-normal SCREW tie (cleat top 104,
 *     overhang bottom 105) holds the overhang's back DOWN in WITHDRAWAL. The narrow 10 cm X-width is the
 *     whole point (below).
 *
 * WHY IT STANDS (both posts present). The overhang weighs 80 x 68 x 5 = 27,200 cm3 x 0.42 g/cm3 =
 * 11.424 kg, W = 11,196 uu. Its weight centroid Y = -36 sits OUTBOARD (more -Y) of the post line
 * Y = -20, so the board tips front-down / back-up about the posts; the back-up is held by the cleat's
 * Z-normal Screw in withdrawal at Y ~ -6. Two support lines (posts at Y = -20 pushing up, fixing at
 * Y ~ -6 pulling down) straddle the load, and the Screw's mean tensile strength 0.54 MPa over the
 * ~90 cm2 tie carries 0.54 x 90 x 100 = 4,860 N = far more than the modest withdrawal reaction. The
 * ROUTER (authority above the 200-block cap) is even gentler: it sees three bed joints BENEATH the
 * overhang (two posts + the cleat) and splits the weight in compression among them, so the assembled
 * porch strands nothing. STANDS is a router verdict here; the tension mechanism is DESIGN readiness.
 *
 * WHY A LOST POST DROPS IT (slice 4, NOT built here — the arithmetic for dev). Remove one post and the
 * overhang keeps only ONE post (on one X-side) plus the narrow central cleat. The cleat's tie resists
 * the Y-tip well (its 90 cm2 acts at the full Y lever), but its restoring couple about the Y axis is the
 * withdrawal force acting over a HALF-WIDTH of just 5 cm in X — far too small to answer the X-torsion the
 * now-asymmetric load throws at the overhang, which rotates about the surviving post toward the gap and
 * drops. A WIDE fixing would have held (its half-width supplies a large X-couple); the tie is a narrow
 * central cleat precisely so the POSTS are the genuine support and the porch is post-dependent under
 * either break authority. THIS SLICE ONLY ASSERTS STANDS; slice 4 pulls a post.
 *
 * =========================================================================================
 * THE RED. BuildRealistic lays the shell + gables + roof but NO porch: there is no grounded Timber piece
 * (every Timber it lays is free), so FindPosts returns none, the overhang and fixing scans find nothing,
 * and the porch assertions fail. That is the expected RED — dev lays the posts, the overhang and the
 * fixing to the geometry above. The stands arm (0 stranded) already holds for the porch-less shed, so the
 * RED lands squarely on the missing porch and not on the solver.
 *
 * NEEDS A TICKING WORLD: NO. Same footing as the shell and gables tests — boxes, doubles, the router;
 * gravity on; every assertion on the laid layout or the solved support state, never on displacement.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRealisticShedPorchTest,
	"DestructionGame.Acceptance.Shed.ThreeD.RealisticBrickShedPorchStandsAsBuilt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRealisticShedPorchTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace RealisticShedTestSupport;

	FBrickLayout Layout;
	const bool bBuilt = DestructionShed3D::BuildRealistic(Layout);

	TestTrue(TEXT("BUILD: the builder must lay the realistic-brick shed (returns false only for a stub)"),
		bBuilt);
	if (!bBuilt)
	{
		AddError(TEXT("BUILD: DestructionShed3D::BuildRealistic laid nothing — cannot examine the porch."));
		return false;
	}

	const FStructure& S = Layout.Structure;

	AddInfo(FString::Printf(TEXT("PORCH: %d pieces, %d joints total (shell + gables + roof + porch)."),
		S.NumPieces(), S.NumConnections()));

	/* ================================================================================
	 * ARM 1 — TWO GROUNDED TIMBER POSTS, 10 x 10 cm, FLANKING THE DOOR. The porch posts are the only
	 * grounded Timber in the shed, so a coordinate-free material+grounding scan finds them. The shell
	 * lays no grounded Timber, so this arm is the RED.
	 * ================================================================================ */

	TArray<int32> Posts;
	FindPosts(Layout, Posts);

	AddInfo(FString::Printf(TEXT("PORCH: found %d grounded Timber post(s)."), Posts.Num()));

	TestEqual(TEXT("POST: exactly two grounded Timber posts stand in front of the door"),
		Posts.Num(), 2);

	bool bBothTenByTen = Posts.Num() == 2;
	bool bBothInFront = Posts.Num() == 2;
	bool bFlankDoor = false;
	if (Posts.Num() == 2)
	{
		for (const int32 Post : Posts)
		{
			const FPieceBox& Box = Layout.Boxes[Post];
			AddInfo(FString::Printf(TEXT("POST %d: centre (%.4g, %.4g, %.4g), half-extents (%.4g, %.4g, %.4g)"),
				Post, Box.CentreCm.X, Box.CentreCm.Y, Box.CentreCm.Z,
				FMath::Abs(Box.ExtentCm.X), FMath::Abs(Box.ExtentCm.Y), FMath::Abs(Box.ExtentCm.Z)));
			if (!IsTenByTenSection(Box))
			{
				bBothTenByTen = false;
			}
			if (!(Box.CentreCm.Y < 0.0))
			{
				bBothInFront = false;
			}
		}
		const double X0 = Layout.Boxes[Posts[0]].CentreCm.X;
		const double X1 = Layout.Boxes[Posts[1]].CentreCm.X;
		bFlankDoor = (FMath::Min(X0, X1) < 90.0) && (FMath::Max(X0, X1) > 90.0);
	}

	TestTrue(TEXT("POST: both posts are a real 10 x 10 cm section (a 4x4 timber), tall in Z"),
		bBothTenByTen);
	TestTrue(TEXT("POST: both posts stand IN FRONT of the door (centre Y < 0, the porch side of the wall)"),
		bBothInFront);
	TestTrue(TEXT("POST: the two posts FLANK the doorway — one left of the door centre X=90, one right"),
		bFlankDoor);

	/* ================================================================================
	 * ARM 2 — THE TIMBER OVERHANG BOARD, OVER THE DOOR, BEARING ON BOTH POSTS, TIED BACK. Found by its
	 * joints (bears on both posts), then checked for a board section, a door-spanning width, a porch-side
	 * position, and a tension-capable withdrawal tie. All absent in the shell — the RED.
	 * ================================================================================ */

	const int32 Overhang = FindOverhang(Layout, Posts);

	TestTrue(TEXT("OVERHANG: a free Timber board bears on BOTH posts (the porch overhang)"),
		Overhang != INDEX_NONE);

	if (Overhang != INDEX_NONE)
	{
		const FPieceBox& Box = Layout.Boxes[Overhang];
		double Lo = 0.0, Mid = 0.0, Hi = 0.0;
		SortedHalfExtents(Box, Lo, Mid, Hi);
		AddInfo(FString::Printf(TEXT("OVERHANG %d: centre (%.4g, %.4g, %.4g), half-extents sorted (%.4g, %.4g, %.4g)"),
			Overhang, Box.CentreCm.X, Box.CentreCm.Y, Box.CentreCm.Z, Lo, Mid, Hi));

		/* A real ~5 cm plank: smallest half-extent <= ~2.5 cm — a coarse block fails this. */
		TestTrue(TEXT("OVERHANG: the overhang is a real BOARD section (<= ~5 cm thick), not a coarse block"),
			Lo <= 2.5 + Tol);

		/* Over the door: its X span covers the door centre X = 90 and is a real span (>= ~half the door). */
		const double LoX = Box.CentreCm.X - FMath::Abs(Box.ExtentCm.X);
		const double HiX = Box.CentreCm.X + FMath::Abs(Box.ExtentCm.X);
		TestTrue(TEXT("OVERHANG: the board spans OVER the doorway (covers the door centre X=90 with real width)"),
			LoX < 90.0 - 30.0 && HiX > 90.0 + 30.0);

		/* On the porch side of the wall, cantilevering out over the door (centre Y < 0). */
		TestTrue(TEXT("OVERHANG: the board sits OUT over the door (centre Y < 0, the porch side of the wall)"),
			Box.CentreCm.Y < 0.0);

		/* Bears on both posts individually (the FindOverhang property, re-asserted per post for clarity). */
		bool bBearsBoth = true;
		for (const int32 Post : Posts)
		{
			if (!SharesJoint(S, Overhang, Post))
			{
				bBearsBoth = false;
			}
		}
		TestTrue(TEXT("OVERHANG: the board BEARS on both posts (a joint to each)"), bBearsBoth);

		/*
		 * THE FIXING is a tension-capable Screw WITHDRAWAL tie incident to the overhang — the one joint a
		 * compression-only DryStone post bearing cannot be. This is the design-intent assertion: the posts
		 * are the genuine support and the narrow tie only holds the back down, so a lost post drops the
		 * porch (slice 4). Above the cap the router stands it either way; the tie is future readiness, and
		 * it is a STRUCTURAL fact this slice pins now.
		 */
		TestTrue(TEXT("FIXING: the overhang is tied back by a Screw-style WITHDRAWAL tie (Tensile>0, mu=0) — "
			"a wall fixing a compression-only bearing could not be"),
			HasWithdrawalTie(S, Overhang));
	}

	/* ================================================================================
	 * ARM 3 — THE WHOLE SHED (shell + gables + roof + porch) STANDS, through production (SolveAndBreak /
	 * the router above the cap). Support-state only, never displacement (DESIGN §4): 0 stranded, the
	 * overhang and both posts read Supported / Grounded, and the shell's lintels and the ridge still read
	 * Supported so the porch did not disturb what already stood.
	 * ================================================================================ */

	TestTrue(*FString::Printf(TEXT("SCALE: %d blocks is above the 200-block cap, so the router is authority"),
		S.NumPieces()), S.NumPieces() > 200);

	const int32 Passes = Layout.Structure.SolveAndBreak();
	const int32 Stranded = StrandedCount(Layout.Structure);
	AddInfo(FString::Printf(TEXT("STANDS: production ran %d pass(es); %d stranded."), Passes, Stranded));

	TestEqual(TEXT("STANDS: nothing may be Stranded — the whole shed (shell + gables + roof + porch) stands"),
		Stranded, 0);

	for (const int32 Post : Posts)
	{
		TestTrue(*FString::Printf(TEXT("STANDS: post %d reads Grounded/Supported"), Post),
			IsStanding(Layout.Structure.GetPieceSupport(Post)));
	}

	if (Overhang != INDEX_NONE)
	{
		TestTrue(TEXT("STANDS: the overhang reads Supported (carried by its posts and the fixing)"),
			IsStanding(Layout.Structure.GetPieceSupport(Overhang)));
	}

	/* The porch must not have unsettled the shell — spot-check the door lintel and the ridge. */
	const int32 DoorLintel = PieceContaining(Layout, DoorLintelPt);
	if (DoorLintel != INDEX_NONE)
	{
		TestTrue(TEXT("STANDS: the door lintel still reads Supported (the porch did not disturb the shell)"),
			IsStanding(Layout.Structure.GetPieceSupport(DoorLintel)));
	}
	const int32 Ridge = RidgePiece(Layout);
	if (Ridge != INDEX_NONE)
	{
		TestTrue(TEXT("STANDS: the ridge still reads Supported"),
			IsStanding(Layout.Structure.GetPieceSupport(Ridge)));
	}

	return true;
}

/**
 * THE AS-BUILT REALISTIC BRICK SHED SETTLES WITHOUT BREAKING A SINGLE BEARING —
 * zero breaking passes, no joint given.
 *
 * THE BEHAVIOUR, IN ONE SENTENCE. DestructionShed3D::BuildRealistic lays a shed
 * that is a valid, standing structure, so settling it through production
 * (FStructure::SolveAndBreak, the router above the 200-block cap) must break
 * NOTHING: SolveAndBreak returns 0 breaking passes and no connection has given.
 *
 * WHY THIS IS SEPARATE FROM (AND STRICTER THAN) THE StandsAsBuilt TESTS ABOVE.
 * Those assert only Stranded == 0. That is TOO WEAK: the shed's timber lintels
 * bear on DryStone joints, and the router breaks five of them the instant it
 * settles (the door-lintel top bearing sits ~2.5% past its kern, reads
 * utilisation Max(), and severs on pass 1; its arch thrust then slides two more
 * dry springings on passes 2-3). None of those broken bearings STRAND a piece —
 * the timber lintel is left carrying only itself and the masonry arches over the
 * gap — so Stranded stays 0 and those tests stay green while the shed quietly
 * self-destructs five joints on the first settle. The pass count and HasGiven are
 * the mechanism that exposes it. Assert on those, NEVER on displacement
 * (DESIGN §4): a joint can sever and leave every piece resting exactly in place.
 *
 * THE DEFECT AND THE FIX IT DRIVES. A DryStone (no-tension) bearing whose
 * resultant crosses the kern reads infinite utilisation, because
 * ComputeUtilisation treats any positive peak tension against f_t = 0 as failure.
 * The correct masonry model is a partial-contact (no-tension) bearing — the bed
 * opens and carries the load on the reduced compressed contact, failing only off
 * the face or by crushing. The focused mechanism driver for that is
 * DestructionGame.Core.ConnectionStrength.DryJointBearsOnReducedContactPastTheKern.
 * This test is the acceptance-level outcome: teach the router that model and the
 * whole shed settles untouched.
 *
 * NEEDS A TICKING WORLD: NO. SolveAndBreak is the world-free settle on the
 * structure the builder returns — the same static solve the StandsAsBuilt tests
 * use — so there is no Chaos, no tick, and gravity is the solver's own self-weight
 * (an INTEGRATION-shaped outcome assertion on a world-free structure).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRealisticShedSettlesUntouchedTest,
	"DestructionGame.Acceptance.Shed.ThreeD.RealisticBrickShedSettlesWithoutBreakingABearing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRealisticShedSettlesUntouchedTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace RealisticShedTestSupport;

	FBrickLayout Layout;
	const bool bBuilt = DestructionShed3D::BuildRealistic(Layout);

	TestTrue(TEXT("BUILD: the builder must lay the realistic-brick shed"), bBuilt);
	if (!bBuilt)
	{
		AddError(TEXT("BUILD: DestructionShed3D::BuildRealistic laid nothing — cannot settle the shed."));
		return false;
	}

	/*
	 * FIXTURE PRECONDITION — this test only bites while the timber bearings are
	 * DryStone (the no-tension joint whose kern break is the defect). If the
	 * builder re-profiles them to a bonded joint the mechanism changes and this
	 * fixture must be re-derived rather than silently still passing.
	 */
	TestTrue(TEXT("FIXTURE: DryStone bearings are the no-tension joint under test (f_t = 0)"),
		DryStone.TensileStrengthMPa == 0.0);

	const int32 Passes = Layout.Structure.SolveAndBreak();

	/*
	 * Count and name the joints that gave, so the RED prints the mechanism rather
	 * than just a number: which joints severed and on which pass.
	 */
	int32 GivenJoints = 0;
	FString GivenDetail;
	for (int32 J = 0; J < Layout.Structure.NumConnections(); ++J)
	{
		if (Layout.Structure.GetConnection(J).HasGiven())
		{
			++GivenJoints;
			if (GivenJoints <= 8)
			{
				GivenDetail += FString::Printf(TEXT(" [joint %d gave on pass %d]"),
					J, Layout.Structure.GetBreakPass(J));
			}
		}
	}

	AddInfo(FString::Printf(TEXT("SETTLE: production ran %d breaking pass(es); %d joint(s) gave.%s"),
		Passes, GivenJoints, *GivenDetail));

	/*
	 * THE TWO NEW REDS. A valid standing shed settles in ZERO breaking passes with
	 * NO joint given. Today the router severs the dry timber bearings at the kern,
	 * so Passes == 3 and five joints have given.
	 */
	TestEqual(TEXT("SETTLE: a valid standing shed breaks nothing — zero breaking passes"),
		Passes, 0);
	TestEqual(TEXT("SETTLE: no bearing may give as the shed settles (the dry timber lintels must hold)"),
		GivenJoints, 0);

	return true;
}

#endif
