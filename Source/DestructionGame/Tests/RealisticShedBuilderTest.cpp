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

#endif
