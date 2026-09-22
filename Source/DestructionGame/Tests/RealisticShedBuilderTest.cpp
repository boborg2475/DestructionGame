// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/DestructionShed3D.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The realistic-brick shed: DestructionShed3D::BuildRealistic lays real 21.5 x 10.25 x 6.5 cm clay
 * bricks in running bond on 1 cm joints, single-wythe, closing a box, with Timber-board lintels
 * over a front door and a left-wall window. It is a SetThreeDimensional layout of hundreds of
 * blocks, so it is above the 200-block equilibrium-gate cap and the router (per-joint capacity
 * sweep) is the break authority.
 *
 * Canonical geometry (cm; X width, Y depth, Z height): grid 22.5 x 11.25 x 7.5; outer box
 * X[0,180] Y[0,140]; eaves at 16 courses. Side walls meet the front/back walls across a 1 cm
 * Y-normal corner joint, which is why the shell is 3D. Door gap X[57.5,122.5] Z[0,90]; window gap
 * Y[55,95] Z[45,90]. Tests find pieces by scanning the layout, never by handle order.
 *
 * Units (DESIGN.md §3): weight = MassKg * 980 already includes 1 N = 100 uu. No strength-vs-force
 * comparison here, so no MPa conversion. No ticking world. Named namespace for unity builds.
 */
namespace RealisticShedTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	// The real brick's half-extents and the coordinating grid.
	constexpr double BrickHalfXCm = 21.5 / 2.0;    // 10.75
	constexpr double BrickHalfYCm = 10.25 / 2.0;   // 5.125  — the single-wythe half-thickness
	constexpr double BrickHalfZCm = 6.5 / 2.0;     // 3.25   — the one-course half-height

	// Long half-extent: 10.75 for a full brick, 5.125 for a half bat ((21.5 - 1) / 2 long).
	constexpr double FullBrickLongHalfCm = 10.75;
	constexpr double HalfBatLongHalfCm = (21.5 - 1.0) / 2.0 / 2.0;   // (20.5)/2 /2 = 5.125

	constexpr double BrickPitchCm = 22.5;          // 21.5 + 1.0 perpend -> a 1 cm joint
	constexpr double CoursePitchCm = 7.5;          // 6.5 + 1.0 bed
	constexpr double BondOffsetCm = 11.25;         // half a cell — the running-bond offset

	constexpr double Tol = 0.05;

	// Probe points inside each gap, pier/sill and lintel, with margin for the bond layout.
	const FVector DoorGap(90.0, 5.0, 45.0);        // mid-doorway, must be EMPTY
	const FVector DoorPierLeft(30.0, 5.0, 45.0);   // brick pier left of the door
	const FVector DoorPierRight(150.0, 5.0, 45.0); // brick pier right of the door
	const FVector DoorLintelPt(90.0, 5.0, 93.0);   // inside the Timber door lintel

	const FVector WindowGap(5.0, 75.0, 68.0);      // mid-window, must be EMPTY
	const FVector WindowSillPt(5.0, 75.0, 20.0);   // brick sill below the window
	const FVector WindowLintelPt(5.0, 75.0, 93.0); // inside the Timber window lintel

	bool Near(double A, double B)
	{
		return FMath::IsNearlyEqual(A, B, Tol);
	}

	/** A box's half-extents ascending, so a brick reads the same however it is laid. */
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

	/** Whether the piece is a full ClayBrick brick. */
	bool IsFullBrickPiece(const FBrickLayout& Layout, int32 Piece)
	{
		return Layout.Structure.GetPiece(Piece).Material == &ClayBrick
			&& Layout.Boxes.IsValidIndex(Piece) && IsFullBrick(Layout.Boxes[Piece]);
	}

	/**
	 * Whether two full bricks in the same wall plane, one course apart, are offset half a cell
	 * (11.25): a running bond. A stack bond or a coarse block fails.
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

	/** Whether two full bricks in one course sit 22.5 cm apart, i.e. a 1 cm perpend. */
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

	/** Whether any joint has a Y normal (out of the X-Z plane). */
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

	/*
	 * Which wall a brick belongs to, by its thin coordinate: front Y=5.125, back Y=128.875, left
	 * X=5.125, right X=174.875. Front/back is tested first so a square corner half-bat is read as
	 * the front/back wall it is laid into.
	 */
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
	 * Whether a Y-normal joint links the named front/back wall to the named side wall. A side
	 * wall's own perpends join two pieces of the same wall, so they don't count; a detached back
	 * wall has no such joint.
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

	/*
	 * Gable and roof scanning (slice 2). Eaves top at Z = 119 (15 * 7.5 + 6.5). Stepped gables rise
	 * above that on the front and back walls, narrowing to an apex; the Timber roof bears on them.
	 */
	constexpr double EavesTopZCm = 119.0;    // course 15 (0-based) top: 15 * 7.5 + 6.5
	constexpr double GableFloorZCm = 119.5;  // gable bricks sit above this

	constexpr double FrontGableYCentreCm = 5.125;    // front wall band Y[0,10.25]
	constexpr double BackGableYCentreCm = 128.875;   // back wall band Y[123.75,134]

	/** The Z centre of a piece's box. */
	double CentreZ(const FBrickLayout& Layout, int32 Piece)
	{
		return Layout.Boxes[Piece].CentreCm.Z;
	}

	/** X-width of each gable course above the eaves on one gable end, ascending in Z. */
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

	/** Whether two gable bricks on this gable end share a Z-normal bed joint. */
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

	/** Whether a Timber piece above the eaves shares a Z-normal joint with a gable brick. */
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

	/** The highest Timber roof member (the ridge), or INDEX_NONE. */
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

	/*
	 * Porch scanning (slice 3): two grounded Timber posts flanking the door, an overhang board on
	 * both cantilevering out in -Y, and a Screw withdrawal tie holding its back to the wall (the
	 * tension a DryStone bearing can't give). The posts are the only grounded Timber in the shed,
	 * so they are found by material and grounding alone.
	 */
	constexpr double PostHalfSectionCm = 5.0;    // a 10 x 10 cm post

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

	/** The porch overhang: the free Timber piece jointed to every post. A cleat never is. */
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
	 * Whether a withdrawal tie (tension > 0, friction 0) touches this piece. DryStone bearings have
	 * no tension and mortar has friction, so only the fixing matches, direct or via a cleat.
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
 * The shell builds from real bricks in running bond, closes a box with door and window openings
 * under Timber lintels, and stands through the router. No ticking world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRealisticShedBuilderTest,
	"DestructionGame.Acceptance.Shed.ThreeD.RealisticBrickShedShellStandsAsBuilt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRealisticShedBuilderTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace RealisticShedTestSupport;

	// Fixture preconditions, pinned so a wrong constant fails visibly.
	TestEqual(TEXT("FIXTURE: clay brick density is 1.9 g/cm3 (UK metric standard brick)"),
		ClayBrick.DensityGramsPerCubicCm, 1.9);
	TestTrue(TEXT("FIXTURE: GeneralPurposeMortar is a bonded joint (cohesion and tension)"),
		GeneralPurposeMortar.ShearCohesionMPa > 0.0 && GeneralPurposeMortar.TensileStrengthMPa > 0.0);
	TestTrue(TEXT("FIXTURE: the real brick is 21.5 x 10.25 x 6.5 cm — half-extents (10.75, 5.125, 3.25)"),
		Near(BrickHalfXCm, 10.75) && Near(BrickHalfYCm, 5.125) && Near(BrickHalfZCm, 3.25));

	// Arm 0: build.
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

	// Arm 1: realistic sizing. A coarse per-wall-block build fails every check.
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

	const int32 DoorLintel = PieceContaining(Layout, DoorLintelPt);
	const int32 WindowLintel = PieceContaining(Layout, WindowLintelPt);

	TestTrue(TEXT("SIZING: a Timber-board door lintel sits over the doorway"),
		DoorLintel != INDEX_NONE && Layout.Structure.GetPiece(DoorLintel).Material == &Timber);
	TestTrue(TEXT("SIZING: a Timber-board window lintel sits over the window"),
		WindowLintel != INDEX_NONE && Layout.Structure.GetPiece(WindowLintel).Material == &Timber);

	if (DoorLintel != INDEX_NONE)
	{
		// A board: smallest half-extent <= 5 cm.
		double Lo = 0.0, Mid = 0.0, Hi = 0.0;
		SortedHalfExtents(Layout.Boxes[DoorLintel], Lo, Mid, Hi);
		AddInfo(FString::Printf(TEXT("SIZING: door lintel half-extents sorted (%.4g, %.4g, %.4g)"), Lo, Mid, Hi));
		TestTrue(TEXT("SIZING: the door lintel is a real BOARD section (<= 10 cm thick), not a coarse block"),
			Lo <= 5.0);
	}

	// Arm 2: openings, materials, and out-of-plane corner joints.
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

	// All four corners bonded; a detached back wall fails the back pair.
	TestTrue(TEXT("SHED: the FRONT-LEFT corner is a genuine wall-to-wall Y-normal mortar joint"),
		HasCornerJoint(Layout, EWall::Front, EWall::Left));
	TestTrue(TEXT("SHED: the FRONT-RIGHT corner is a genuine wall-to-wall Y-normal mortar joint"),
		HasCornerJoint(Layout, EWall::Front, EWall::Right));
	TestTrue(TEXT("SHED: the BACK-LEFT corner is a genuine wall-to-wall Y-normal mortar joint (open box fails here)"),
		HasCornerJoint(Layout, EWall::Back, EWall::Left));
	TestTrue(TEXT("SHED: the BACK-RIGHT corner is a genuine wall-to-wall Y-normal mortar joint (open box fails here)"),
		HasCornerJoint(Layout, EWall::Back, EWall::Right));

	/*
	 * Arm 3: it stands through SolveAndBreak (router, above the cap). Nothing stranded, lintels
	 * Supported. Support state only, never displacement (DESIGN §4).
	 */
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
 * Slice 2: stepped brick gables and a Timber roof on the shell. Courses 16..19 on each gable end
 * step in one brick pitch per side per course (8, 6, 4, 2 bricks), so each course's centroid stays
 * over the one below and the gable can't overturn. Full-depth Timber purlins bear on both gables
 * at stepped heights, capped by a ridge board. The whole shed stands through the router with
 * nothing stranded. No ticking world.
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

	// Arm 1: stepped gables narrowing to an apex, each course bedded on the one below.
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

	// Arm 2: Timber roof bearing on the gables, capped by a ridge.
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

	/*
	 * Arm 3: it stands (support state only, DESIGN §4). Checks are guarded so a missing gable or
	 * roof fails in arms 1/2 rather than crashing here.
	 */
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
 * Slice 3: a porch over the door. Two grounded 10 x 10 cm Timber posts flank the door at
 * Y[-25,-15]; a ~5 cm Timber overhang X[50,130] Y[-70,-2] bears on both (DryStone beds) and
 * cantilevers out in -Y; a narrow 10 cm Timber cleat, mortared to the wall, ties its back down
 * with a Screw in withdrawal.
 *
 * The overhang (11.4 kg) has its centroid at Y = -36, outboard of the posts at Y = -20, so it tips
 * about them and the Screw tie resists (0.54 MPa x ~90 cm2 = 4,860 N). The cleat is narrow so it
 * gives little X-couple: with one post gone the overhang should rotate and drop (slice 4). The
 * router sees three bed joints under the overhang, so the assembled porch stands. No ticking world.
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

	// Arm 1: two grounded 10 x 10 cm Timber posts flanking the door.
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

	// Arm 2: the overhang board, found by its joints to both posts, then checked.
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

		// A ~5 cm plank: smallest half-extent <= ~2.5 cm.
		TestTrue(TEXT("OVERHANG: the overhang is a real BOARD section (<= ~5 cm thick), not a coarse block"),
			Lo <= 2.5 + Tol);

		// Spans the door centre X = 90 with real width.
		const double LoX = Box.CentreCm.X - FMath::Abs(Box.ExtentCm.X);
		const double HiX = Box.CentreCm.X + FMath::Abs(Box.ExtentCm.X);
		TestTrue(TEXT("OVERHANG: the board spans OVER the doorway (covers the door centre X=90 with real width)"),
			LoX < 90.0 - 30.0 && HiX > 90.0 + 30.0);

		TestTrue(TEXT("OVERHANG: the board sits OUT over the door (centre Y < 0, the porch side of the wall)"),
			Box.CentreCm.Y < 0.0);

		bool bBearsBoth = true;
		for (const int32 Post : Posts)
		{
			if (!SharesJoint(S, Overhang, Post))
			{
				bBearsBoth = false;
			}
		}
		TestTrue(TEXT("OVERHANG: the board BEARS on both posts (a joint to each)"), bBearsBoth);

		// Design intent: the narrow tie only holds the back down; the posts are the real support.
		TestTrue(TEXT("FIXING: the overhang is tied back by a Screw-style WITHDRAWAL tie (Tensile>0, mu=0) — "
			"a wall fixing a compression-only bearing could not be"),
			HasWithdrawalTie(S, Overhang));
	}

	// Arm 3: the whole shed stands (support state only, DESIGN §4).
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

	// Spot-check that the porch didn't unsettle the shell.
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
 * The as-built shed settles with zero breaking passes and no joint given. Stricter than Stranded
 * == 0: the router used to sever five DryStone lintel bearings on settle (the door-lintel bearing
 * ~2.5% past its kern read infinite utilisation), yet nothing stranded because the masonry arched.
 * Asserts on pass count and HasGiven, never displacement (DESIGN §4). The fix is the partial-contact
 * bearing model; its unit driver is ConnectionStrength.DryJointBearsOnReducedContactPastTheKern.
 * No ticking world.
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

	// Only bites while the bearings are no-tension DryStone; re-derive if they are re-profiled.
	TestTrue(TEXT("FIXTURE: DryStone bearings are the no-tension joint under test (f_t = 0)"),
		DryStone.TensileStrengthMPa == 0.0);

	const int32 Passes = Layout.Structure.SolveAndBreak();

	// Name the joints that gave so a failure shows the mechanism.
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

	TestEqual(TEXT("SETTLE: a valid standing shed breaks nothing — zero breaking passes"),
		Passes, 0);
	TestEqual(TEXT("SETTLE: no bearing may give as the shed settles (the dry timber lintels must hold)"),
		GivenJoints, 0);

	return true;
}

#endif
