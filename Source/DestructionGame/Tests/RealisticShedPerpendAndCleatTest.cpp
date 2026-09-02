// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Connection.h"
#include "Core/ConnectionStrength.h"
#include "Core/DestructionShed3D.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * ITEM 6b — TWO OWNER-APPROVED JOINT-AUTHORING CORRECTIONS to the realistic-brick shed, each pinned as a
 * DATA assertion on the connection profile the builder authors (never on a utilisation, so no "which axis
 * governs" ambiguity can creep in — a profile field is exact and binary):
 *
 *   (1) THE PORCH CLEAT'S WALL ANCHOR IS A SCREW, NOT A MORTAR BOND. The cleat is a TIMBER piece; mortar
 *       does not bond to wood, so authoring its wall anchor as GeneralPurposeMortar (tensile 0.7 MPa) is
 *       unphysical. A plugged/screwed cleat is a Screw fastener (withdrawal tensile 0.54 MPa). The cleat's
 *       OTHER joint (to the overhang) is already a Screw — this brings the wall anchor into line.
 *
 *   (2) THE SHED'S VERTICAL BRICK-BRICK JOINTS ARE WEAK-PERPEND MORTAR. Real perpends (the vertical head
 *       joints inside a course, AND the vertical corner joints where perpendicular walls meet) are weak
 *       and often unfilled; EN 1996 declines to credit them. Today the builder's sweep authors ONE row —
 *       GeneralPurposeMortar (cohesion 0.9, tensile 0.7) — for every brick-brick joint, bed and vertical
 *       alike. The approved fix is a weaker head-joint row (ShearCohesion 0.2, Tensile 0.1; compressive
 *       10.0, friction 0.75, MaxShear 2.0 UNCHANGED from the bed row) authored for every VERTICAL
 *       brick-brick joint, while horizontal BED joints keep GeneralPurposeMortar and timber bearings keep
 *       DryStone.
 *
 * THE SELECTION PREDICATE (reported to dev). A brick-brick joint is a BED joint iff its interface normal
 * is substantially vertical (a Z-normal). MakeInterface only ever emits an axis-aligned normal (AddConnection
 * refuses anything else), so |normal.Z| is exactly 1 for a bed joint and exactly 0 for a perpend or a corner.
 * The clean rule is therefore:
 *
 *     for a brick-brick (both ClayBrick, non-timber) joint:
 *         weak-perpend row   iff   |InterfaceNormal.GetSafeNormal().Z| < 0.5   (normal is NOT Z-up)
 *         GeneralPurposeMortar otherwise (the bed joint beneath the piece)
 *
 * This catches in-course perpends (X- or Y-normal within a wall) AND the wall corners (Y- or X-normal at a
 * wall junction) — both vertical mortar — while leaving Z-normal beds and every timber bearing untouched.
 *
 * NEEDS A TICKING WORLD: NO. Boxes, doubles and the router; gravity on for the standing checks. No Chaos.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges files into one translation unit.
 */
namespace RealisticShedPerpendAndCleatSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	bool Near(double A, double B, double Tol = 1.0e-6)
	{
		return FMath::Abs(A - B) <= Tol;
	}

	bool IsStanding(EPieceSupport Support)
	{
		return Support == EPieceSupport::Grounded || Support == EPieceSupport::Supported;
	}

	int32 StrandedCount(const FStructure& S)
	{
		int32 N = 0;
		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (!S.IsPieceRemoved(P) && S.GetPieceSupport(P) == EPieceSupport::Stranded)
			{
				++N;
			}
		}
		return N;
	}

	/* A joint is a BED joint iff its (axis-aligned) normal is substantially vertical. */
	bool IsBedNormal(const FConnection& Cn)
	{
		return FMath::Abs(Cn.InterfaceNormal.GetSafeNormal().Z) >= 0.5;
	}

	bool IsBrickBrick(const FStructure& S, const FConnection& Cn)
	{
		return S.GetPiece(Cn.PieceA).Material == &ClayBrick
			&& S.GetPiece(Cn.PieceB).Material == &ClayBrick;
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
			const FVector Lo = Layout.Boxes[Piece].CentreCm - Layout.Boxes[Piece].ExtentCm;
			const FVector Hi = Layout.Boxes[Piece].CentreCm + Layout.Boxes[Piece].ExtentCm;
			if (P.X >= Lo.X && P.X <= Hi.X && P.Y >= Lo.Y && P.Y <= Hi.Y && P.Z >= Lo.Z && P.Z <= Hi.Z)
			{
				return Piece;
			}
		}
		return INDEX_NONE;
	}

	/* Which wall a ClayBrick belongs to, by the band its thin coordinate sits in (see the builder test). */
	enum class EWall { None, Front, Back, Left, Right };

	EWall WallOf(const FBrickLayout& Layout, int32 Piece)
	{
		if (!Layout.Boxes.IsValidIndex(Piece)
			|| Layout.Structure.GetPiece(Piece).Material != &ClayBrick)
		{
			return EWall::None;
		}
		const FVector C = Layout.Boxes[Piece].CentreCm;
		if (Near(C.Y, 5.125, 1.0))
		{
			return EWall::Front;
		}
		if (Near(C.Y, 128.875, 1.0))
		{
			return EWall::Back;
		}
		if (Near(C.X, 5.125, 1.0) && C.Y > 10.5 && C.Y < 123.5)
		{
			return EWall::Left;
		}
		if (Near(C.X, 174.875, 1.0) && C.Y > 10.5 && C.Y < 123.5)
		{
			return EWall::Right;
		}
		return EWall::None;
	}
}

/**
 * THE SHED'S VERTICAL BRICK-BRICK JOINTS (perpends + corners) ARE AUTHORED WITH THE WEAK-PERPEND ROW
 * (cohesion 0.2, tensile 0.1), WHILE ITS HORIZONTAL BED JOINTS KEEP GeneralPurposeMortar (0.9 / 0.7).
 *
 * RED TODAY: the builder authors GeneralPurposeMortar (0.9 / 0.7) for EVERY brick-brick joint, so the
 * vertical-joint tensile-0.1 / cohesion-0.2 assertions fail while every bed-joint pin passes unchanged.
 *
 * NEEDS A TICKING WORLD: NO.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRealisticShedWeakPerpendTest,
	"DestructionGame.Acceptance.Shed.ThreeD.RealisticBrickShedUsesWeakPerpendMortarOnVerticalJoints",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRealisticShedWeakPerpendTest::RunTest(const FString& Parameters)
{
	using namespace RealisticShedPerpendAndCleatSupport;
	using namespace DestructionProfiles;

	/* FIXTURE guards — the numbers this test pins trace to the two mortar rows, not to a coincidence. */
	TestTrue(TEXT("FIXTURE: the bed row GeneralPurposeMortar is cohesion 0.9 / tensile 0.7"),
		Near(GeneralPurposeMortar.ShearCohesionMPa, 0.9) && Near(GeneralPurposeMortar.TensileStrengthMPa, 0.7));

	FBrickLayout Layout;
	const bool bBuilt = DestructionShed3D::BuildRealistic(Layout);
	TestTrue(TEXT("BUILD: the realistic-brick shed must build"), bBuilt);
	if (!bBuilt)
	{
		return false;
	}

	const FStructure& S = Layout.Structure;

	int32 BedCount = 0;
	int32 VerticalCount = 0;
	int32 CornerCount = 0;

	int32 BedWrong = 0;        // a bed joint that is NOT the strong mortar row
	int32 VerticalWrong = 0;   // a vertical joint that is NOT the weak-perpend row
	int32 VerticalBadCompanion = 0;  // a vertical joint whose UNCHANGED fields drifted

	for (int32 J = 0; J < S.NumConnections(); ++J)
	{
		const FConnection& Cn = S.GetConnection(J);
		if (!IsBrickBrick(S, Cn))
		{
			continue;
		}
		const FConnectionStrength& St = Cn.Strength;

		if (IsBedNormal(Cn))
		{
			++BedCount;
			/*
			 * A BED joint must keep GeneralPurposeMortar — the perpend weakening must not leak into the
			 * horizontal beds that actually carry the wall down. Anti-regression: passes today, must stay.
			 */
			if (!(Near(St.ShearCohesionMPa, 0.9) && Near(St.TensileStrengthMPa, 0.7)))
			{
				++BedWrong;
			}
		}
		else
		{
			++VerticalCount;
			const EWall WA = WallOf(Layout, Cn.PieceA);
			const EWall WB = WallOf(Layout, Cn.PieceB);
			const bool bCorner = (WA != EWall::None && WB != EWall::None && WA != WB);
			if (bCorner)
			{
				++CornerCount;
			}

			/* THE RED: a vertical brick-brick joint must be the weak-perpend row (0.2 / 0.1). */
			if (!(Near(St.ShearCohesionMPa, 0.2) && Near(St.TensileStrengthMPa, 0.1)))
			{
				++VerticalWrong;
			}

			/* The three UNCHANGED fields must match the bed row exactly (10.0 / 0.75 / 2.0). */
			if (!(Near(St.CompressiveStrengthMPa, 10.0)
					&& Near(St.FrictionCoefficient, 0.75)
					&& Near(St.MaxShearStrengthMPa, 2.0)))
			{
				++VerticalBadCompanion;
			}
		}
	}

	AddInfo(FString::Printf(
		TEXT("PERPEND: brick-brick joints — %d bed (Z-normal), %d vertical (of which %d are wall corners)."),
		BedCount, VerticalCount, CornerCount));

	/* The predicate has to have something to catch on both sides, or the assertion is vacuous. */
	TestTrue(TEXT("PRECONDITION: the shed has horizontal BED brick-brick joints"), BedCount > 0);
	TestTrue(TEXT("PRECONDITION: the shed has VERTICAL brick-brick joints (in-course perpends)"),
		VerticalCount > 0);
	TestTrue(TEXT("PRECONDITION: the predicate catches wall CORNERS too (vertical, cross-wall)"),
		CornerCount > 0);

	/* THE RED — every vertical brick-brick joint (perpends AND corners) is the weak-perpend row. */
	TestEqual(
		TEXT("RED: every VERTICAL brick-brick joint must be the weak-perpend row (cohesion 0.2, tensile 0.1) — "
			"they are GeneralPurposeMortar (0.9 / 0.7) today"),
		VerticalWrong, 0);

	/* The unchanged companion fields — pins the exact spec so the perpend row is not a fresh guess. */
	TestEqual(
		TEXT("SPEC: a vertical joint keeps the bed row's compressive 10.0, friction 0.75, MaxShear 2.0"),
		VerticalBadCompanion, 0);

	/* Anti-regression — the bed joints that carry the wall down stay STRONG. */
	TestEqual(
		TEXT("ANTI-REGRESSION: every horizontal BED brick-brick joint keeps GeneralPurposeMortar (0.9 / 0.7)"),
		BedWrong, 0);

	/* Timber bearings must be untouched — the predicate is brick-brick only. */
	int32 TimberJoints = 0;
	int32 TimberWrong = 0;
	for (int32 J = 0; J < S.NumConnections(); ++J)
	{
		const FConnection& Cn = S.GetConnection(J);
		const bool bTimberTouch = S.GetPiece(Cn.PieceA).Material == &Timber
			|| S.GetPiece(Cn.PieceB).Material == &Timber;
		if (!bTimberTouch)
		{
			continue;
		}
		++TimberJoints;
		/*
		 * Every timber bearing the SWEEP authors is DryStone (compression-only, cohesion 0, tensile 0). The
		 * porch's four EXPLICIT joints (two DryStone bearings, a Screw tie, and the cleat-wall anchor this
		 * file's other test targets) are the only tension-capable timber joints, so a joint with f_t > 0 is
		 * a porch explicit and excluded — this pin measures only the sweep's own compression-only bearings.
		 */
		if (Cn.Strength.TensileStrengthMPa > 0.0)
		{
			continue;
		}
		if (!(Near(Cn.Strength.ShearCohesionMPa, 0.0) && Near(Cn.Strength.TensileStrengthMPa, 0.0)))
		{
			++TimberWrong;
		}
	}
	AddInfo(FString::Printf(TEXT("PERPEND: %d timber-bearing joints scanned."), TimberJoints));
	TestEqual(TEXT("SPEC: timber bearings are untouched by the perpend rule (DryStone, cohesion/tensile 0)"),
		TimberWrong, 0);

	return true;
}

/**
 * THE PORCH CLEAT IS SCREW-FIXED TO THE WALL, NOT MORTAR-BONDED — a tension-capable fastener (Screw,
 * withdrawal 0.54 MPa), because mortar does not bond to a timber cleat.
 *
 * RED TODAY: the builder authors the cleat-wall anchor as GeneralPurposeMortar (tensile 0.7), so the
 * Screw-tensile-0.54 assertion fails. The porch still STANDS both ways (the item-3 tension clause spares a
 * body held by any f_t > 0 tie), so the standing assertion is an anti-regression pin, not the red.
 *
 * NEEDS A TICKING WORLD: NO.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRealisticShedPorchCleatScrewedTest,
	"DestructionGame.Acceptance.Shed.ThreeD.RealisticBrickShedPorchCleatIsScrewedToTheWall",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRealisticShedPorchCleatScrewedTest::RunTest(const FString& Parameters)
{
	using namespace RealisticShedPerpendAndCleatSupport;
	using namespace DestructionProfiles;

	/* FIXTURE guards — the Screw and mortar rows this test discriminates. */
	TestTrue(TEXT("FIXTURE: Screw is the tension-capable fastener (withdrawal 0.54, cohesion 0.23, mu 0)"),
		Near(Screw.TensileStrengthMPa, 0.54) && Near(Screw.ShearCohesionMPa, 0.23)
			&& Near(Screw.FrictionCoefficient, 0.0));
	TestTrue(TEXT("FIXTURE: GeneralPurposeMortar is the mortar bond (tensile 0.7) the fix replaces"),
		Near(GeneralPurposeMortar.TensileStrengthMPa, 0.7));

	FBrickLayout Layout;
	const bool bBuilt = DestructionShed3D::BuildRealistic(Layout);
	TestTrue(TEXT("BUILD: the realistic-brick shed must build"), bBuilt);
	if (!bBuilt)
	{
		return false;
	}

	FStructure& S = Layout.Structure;

	/*
	 * THE CLEAT — the narrow central Timber piece over the door, box X[85,95], Y[-11,-1], Z[97.5,104]
	 * (centre 90, -6, 100.75). Found by its box, so the exact handle order does not matter.
	 */
	const int32 Cleat = PieceContaining(Layout, FVector(90.0, -6.0, 100.75));
	TestTrue(TEXT("FIXTURE: the porch cleat (Timber, centre ~ (90, -6, 100.75)) must exist"),
		Cleat != INDEX_NONE && S.GetPiece(Cleat).Material == &Timber);
	if (Cleat == INDEX_NONE)
	{
		return false;
	}

	/*
	 * THE WALL ANCHOR — the ONE joint from the cleat to a ClayBrick (the cleat's other joint is the
	 * Timber-to-Timber Screw tie to the overhang, so the cleat-to-brick joint is unambiguously the anchor).
	 */
	int32 AnchorJoint = INDEX_NONE;
	for (int32 J = 0; J < S.NumConnections(); ++J)
	{
		const FConnection& Cn = S.GetConnection(J);
		const bool bTouchesCleat = (Cn.PieceA == Cleat || Cn.PieceB == Cleat);
		if (!bTouchesCleat)
		{
			continue;
		}
		const int32 Other = (Cn.PieceA == Cleat) ? Cn.PieceB : Cn.PieceA;
		if (S.GetPiece(Other).Material == &ClayBrick)
		{
			AnchorJoint = J;
			break;
		}
	}

	TestTrue(TEXT("FIXTURE: the cleat has a wall anchor joint to a ClayBrick"), AnchorJoint != INDEX_NONE);
	if (AnchorJoint == INDEX_NONE)
	{
		return false;
	}

	const FConnectionStrength& Anchor = S.GetConnection(AnchorJoint).Strength;
	AddInfo(FString::Printf(
		TEXT("CLEAT: wall anchor is cohesion %.4g, tensile %.4g, friction %.4g, compressive %.4g."),
		Anchor.ShearCohesionMPa, Anchor.TensileStrengthMPa, Anchor.FrictionCoefficient,
		Anchor.CompressiveStrengthMPa));

	/* THE RED — the anchor is a Screw fastener, not a mortar bond. */
	TestTrue(
		TEXT("RED: the cleat-wall anchor must be a Screw (tensile 0.54, cohesion 0.23, mu 0) — it is "
			"GeneralPurposeMortar (tensile 0.7) today"),
		Near(Anchor.TensileStrengthMPa, 0.54) && Near(Anchor.ShearCohesionMPa, 0.23)
			&& Near(Anchor.FrictionCoefficient, 0.0));

	/* It is still tension-capable (f_t > 0) — so the tension clause spares the porch either way. */
	TestTrue(TEXT("SPEC: the anchor is tension-capable (a screwed cleat withdraws, it does not just rest)"),
		Anchor.TensileStrengthMPa > 0.0);

	/*
	 * ANTI-REGRESSION — THE PORCH STILL STANDS. Screw f_t > 0 keeps the item-3 tension clause sparing the
	 * cantilevered overhang, so nothing is stranded and the porch pieces keep the earth. Passes today; the
	 * point is that the mortar->Screw swap must not change that.
	 */
	const int32 PostL = PieceContaining(Layout, FVector(55.0, -20.0, 52.0));
	const int32 PostR = PieceContaining(Layout, FVector(125.0, -20.0, 52.0));
	const int32 Overhang = PieceContaining(Layout, FVector(90.0, -36.0, 107.5));

	TestTrue(TEXT("FIXTURE: the two posts and the overhang must be found"),
		PostL != INDEX_NONE && PostR != INDEX_NONE && Overhang != INDEX_NONE);

	const int32 Passes = S.SolveAndBreak();
	const int32 Stranded = StrandedCount(S);
	AddInfo(FString::Printf(TEXT("STANDS: production ran %d pass(es); %d stranded."), Passes, Stranded));

	TestEqual(TEXT("STANDS: nothing may be Stranded — the porch verdict is about the shed, not the solver"),
		Stranded, 0);
	if (Cleat != INDEX_NONE)
	{
		TestTrue(TEXT("STANDS: the cleat keeps the earth"), IsStanding(S.GetPieceSupport(Cleat)));
	}
	if (Overhang != INDEX_NONE)
	{
		TestTrue(TEXT("STANDS: the porch overhang keeps the earth (held by the posts and the tie)"),
			IsStanding(S.GetPieceSupport(Overhang)));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
