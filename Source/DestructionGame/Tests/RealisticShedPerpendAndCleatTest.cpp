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
 * Item 6b: two owner-approved joint-authoring corrections to the realistic-brick shed, pinned as
 * data on the authored profiles rather than utilisations:
 *
 *   (1) The porch cleat's wall anchor is a Screw (withdrawal 0.54), since mortar does not bond to
 *       timber.
 *   (2) Vertical brick-brick joints (perpends and corners) use the weak-perpend row (cohesion 0.2,
 *       tensile 0.1; other fields as GeneralPurposeMortar). Real perpends are weak and EN 1996 does
 *       not credit them. Beds keep GeneralPurposeMortar; timber keeps DryStone.
 *
 * A brick-brick joint is a perpend when |normal.Z| < 0.5; MakeInterface normals are axis-aligned, so
 * |Z| is 1 for a bed and 0 otherwise. World-free.
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

	// A bed joint has a vertical normal.
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

	// Which wall a brick belongs to, from its thin-axis coordinate.
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

/** Vertical brick-brick joints use the weak-perpend row (0.2 / 0.1); beds keep GeneralPurposeMortar (0.9 / 0.7). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRealisticShedWeakPerpendTest,
	"DestructionGame.Acceptance.Shed.ThreeD.RealisticBrickShedUsesWeakPerpendMortarOnVerticalJoints",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRealisticShedWeakPerpendTest::RunTest(const FString& Parameters)
{
	using namespace RealisticShedPerpendAndCleatSupport;
	using namespace DestructionProfiles;

	// Pin the bed row's values.
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
			// Beds keep GeneralPurposeMortar; the perpend weakening must not leak into them.
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

			// Vertical joints use the weak-perpend row.
			if (!(Near(St.ShearCohesionMPa, 0.2) && Near(St.TensileStrengthMPa, 0.1)))
			{
				++VerticalWrong;
			}

			// The other three fields match the bed row (10.0 / 0.75 / 2.0).
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

	// Both sides of the predicate must be populated, or the checks are vacuous.
	TestTrue(TEXT("PRECONDITION: the shed has horizontal BED brick-brick joints"), BedCount > 0);
	TestTrue(TEXT("PRECONDITION: the shed has VERTICAL brick-brick joints (in-course perpends)"),
		VerticalCount > 0);
	TestTrue(TEXT("PRECONDITION: the predicate catches wall CORNERS too (vertical, cross-wall)"),
		CornerCount > 0);

	TestEqual(
		TEXT("RED: every VERTICAL brick-brick joint must be the weak-perpend row (cohesion 0.2, tensile 0.1) — "
			"they are GeneralPurposeMortar (0.9 / 0.7) today"),
		VerticalWrong, 0);

	TestEqual(
		TEXT("SPEC: a vertical joint keeps the bed row's compressive 10.0, friction 0.75, MaxShear 2.0"),
		VerticalBadCompanion, 0);

	TestEqual(
		TEXT("ANTI-REGRESSION: every horizontal BED brick-brick joint keeps GeneralPurposeMortar (0.9 / 0.7)"),
		BedWrong, 0);

	// Timber bearings are untouched; the rule is brick-brick only.
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
		// Skip the porch's explicit tension-capable joints; check only the sweep's DryStone bearings.
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
 * The porch cleat's wall anchor is a Screw (withdrawal 0.54), not mortar. The porch stands either way
 * (item 3's tension clause spares anything held by an f_t > 0 tie), so the standing check is an
 * anti-regression pin. World-free.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRealisticShedPorchCleatScrewedTest,
	"DestructionGame.Acceptance.Shed.ThreeD.RealisticBrickShedPorchCleatIsScrewedToTheWall",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRealisticShedPorchCleatScrewedTest::RunTest(const FString& Parameters)
{
	using namespace RealisticShedPerpendAndCleatSupport;
	using namespace DestructionProfiles;

	// Pin the Screw and mortar rows being distinguished.
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

	// The cleat, found by position (centre 90, -6, 100.75) rather than handle.
	const int32 Cleat = PieceContaining(Layout, FVector(90.0, -6.0, 100.75));
	TestTrue(TEXT("FIXTURE: the porch cleat (Timber, centre ~ (90, -6, 100.75)) must exist"),
		Cleat != INDEX_NONE && S.GetPiece(Cleat).Material == &Timber);
	if (Cleat == INDEX_NONE)
	{
		return false;
	}

	// The anchor is the cleat's only joint to a brick.
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

	TestTrue(
		TEXT("RED: the cleat-wall anchor must be a Screw (tensile 0.54, cohesion 0.23, mu 0) — it is "
			"GeneralPurposeMortar (tensile 0.7) today"),
		Near(Anchor.TensileStrengthMPa, 0.54) && Near(Anchor.ShearCohesionMPa, 0.23)
			&& Near(Anchor.FrictionCoefficient, 0.0));

	TestTrue(TEXT("SPEC: the anchor is tension-capable (a screwed cleat withdraws, it does not just rest)"),
		Anchor.TensileStrengthMPa > 0.0);

	// Anti-regression: the porch still stands with nothing stranded.
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
