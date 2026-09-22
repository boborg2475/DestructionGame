// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockBridge.h"
#include "Core/RigidBlock/RigidBlockOracle.h"

#include "World/DestructionScenarios.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The recognizable 3D shed as a playable scenario: the world-path counterpart of the
 * DestructionShed3D::BuildRecognizable builder tests (Acceptance.Shed.ThreeD.*).
 *
 * The catalogue's `shed3d` row (map `Lvl_Shed3D`) lays the 24-piece shed, flagged 3D: brick walls
 * with a door and window, stepped gables, a timber roof, and a timber porch cantilevered on two
 * grounded posts and tied back by a cleat. It stands through production SolveAndBreak (whose
 * below-cap authority is the 3D LP), and its cut pulls the right porch post so the overhang loses
 * the earth while the shed stands.
 *
 * Beyond the builder tests, this checks the row, that the 3D flag and materials survive
 * DestructionScenarios::Build, and that the production door decides the break. The row needs its
 * own map: IndexOfMapName returns the first row with a map name, and sharing one would also
 * collide in the ScenarioMapsExist and ScenarioMapsAreDistinctAssets checks.
 *
 * Assertions are on mechanism and outcome (LP feasibility, moving blocks, GetPieceSupport), never
 * displacement. Needs no world. Uniquely named namespace for unity builds.
 */
namespace ThreeDShedScenarioTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/** The row's scenario name (`?Scenario=` and IndexOfName). */
	const TCHAR* const Shed3DScenarioName = TEXT("shed3d");

	/** The row's own map. */
	const TCHAR* const Shed3DScenarioMapName = TEXT("Lvl_Shed3D");

	/*
	 * Expected shed, matching the builder. Pieces are identified by position, not handle, using the
	 * same centroid table as the builder tests.
	 */

	constexpr int32 ExpectedPieces = 24;
	constexpr int32 ExpectedJoints = 30;
	constexpr int32 ExpectedGrounded = 7;    // BackWall, RightWall, LeftPier, RightPier, Sill, PostL, PostR
	constexpr int32 ExpectedBrick = 13;
	constexpr int32 ExpectedTimber = 11;     // + the Cleat (the wall tie); the cleat is NOT grounded

	/*
	 * PostR's box centre (X[215,245] Y[328,352] Z[0,200]), the cut the row must name.
	 * ScenariosPieceAtCentre matches to 1e-6.
	 */
	const FVector CutCentreForPostR(230.0, 340.0, 100.0);

	// Centroids of the asserted pieces.
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

	/** Handles of the named shed pieces, found by position. */
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
	inline int32 PieceContaining(const FBrickLayout& Layout, const FVector& P)
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

	/** Name every asserted piece from its centroid, or fail if any is missing. */
	inline bool Identify(const FBrickLayout& Layout, FShed& Out)
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

	inline int32 CountMaterial(const FStructure& S, const FMaterialProfile* Material,
		bool bGroundedFilter, bool bWantGrounded)
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

	inline int32 StrandedCount(const FStructure& S)
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

	inline bool IsStanding(EPieceSupport Support)
	{
		return Support == EPieceSupport::Grounded || Support == EPieceSupport::Supported;
	}

	/** True when a live piece has lost every path to the earth. */
	inline bool HasLostTheEarth(const FStructure& S, int32 Piece)
	{
		if (S.IsPieceRemoved(Piece))
		{
			return false;
		}
		return !IsStanding(S.GetPieceSupport(Piece));
	}

	/** The oracle block that came from a given FStructure piece, via the bridge provenance. */
	inline int32 OracleBlockOfPiece(const RigidBlockOracle::FOracleProblem& Problem, int32 Piece)
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
 * The shed3d row exists, lays the 24-piece shed flagged 3D, stands through production
 * SolveAndBreak, and its cut post fells the porch overhang. Needs no world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FThreeDShedScenarioCatalogueTest,
	"DestructionGame.World.Scenarios.Shed3DRow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FThreeDShedScenarioCatalogueTest::RunTest(const FString& Parameters)
{
	using namespace ThreeDShedScenarioTestSupport;
	using namespace DestructionProfiles;
	using namespace DestructionLayout;
	using namespace DestructionScenarios;

	// Arm 0: the row exists and lays the 3D shed.

	const int32 Index = IndexOfName(FName(Shed3DScenarioName));

	if (!Catalogue().IsValidIndex(Index))
	{
		AddError(FString::Printf(
			TEXT("the catalogue must carry a row named '%s' — the 3D shed is not joinable until it does; "
				"IndexOfName returned %d against %d row(s)."),
			Shed3DScenarioName, Index, Catalogue().Num()));

		return false;
	}

	const FScenario& Scenario = Catalogue()[Index];

	// Its own map, or it is joinable only by ?Scenario= and collides in the content checks.
	TestEqual(
		TEXT("the 3D shed row must name its own map 'Lvl_Shed3D' — it cannot ride Lvl_Sandbox or Lvl_Shed"),
		FString(Scenario.MapName ? Scenario.MapName : TEXT("")), FString(Shed3DScenarioMapName));

	// A LayStructure producer, so Build doesn't fall back to a running-bond wall.
	TestTrue(
		TEXT("the 3D shed row must carry a LayStructure producer (the shed is not a running-bond wall)"),
		static_cast<bool>(Scenario.LayStructure));

	// Three-quarter framing, so the box's depth and the porch are visible.
	TestEqual(
		TEXT("the 3D shed row is framed from a three-quarter angle (a closed box, not a flat wall)"),
		static_cast<int32>(Scenario.Framing), static_cast<int32>(EScenarioFraming::ThreeQuarter));

	// Build lays the 24-piece shed; the 3D flag and materials survive AdoptLayout.

	FBrickLayout Layout;
	TArray<int32> Cut;

	const bool bBuilt = Build(Scenario, Layout, Cut);

	TestTrue(
		TEXT("the 3D shed row must build — a row that cannot be laid is a level that cannot be joined"),
		bBuilt);

	if (!bBuilt)
	{
		return false;
	}

	// Without the 3D flag the bridge poses it in 2D and silently drops the Y-normal corners.
	TestTrue(
		TEXT("the built structure must be flagged 3D — else the world bridge poses it in 2D and loses "
			"the out-of-plane corners"),
		Layout.Structure.IsThreeDimensional());

	TestEqual(TEXT("24 pieces — walls, door, window, gables, roof, porch"),
		Layout.Structure.NumPieces(), ExpectedPieces);
	TestEqual(TEXT("one box per piece, or AdoptLayout refuses the layout"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());
	TestEqual(TEXT("30 joints — door(2), window(4), corners(4), gables(6), roof(10), porch(4)"),
		Layout.Structure.NumConnections(), ExpectedJoints);

	TestEqual(TEXT("7 grounded pieces — the walls' feet, the door piers, the sill and the two posts"),
		CountMaterial(Layout.Structure, &ClayBrick, true, true)
			+ CountMaterial(Layout.Structure, &Timber, true, true), ExpectedGrounded);
	TestEqual(TEXT("13 ClayBrick pieces (walls, piers, sill, jambs, gables)"),
		CountMaterial(Layout.Structure, &ClayBrick, false, false), ExpectedBrick);
	TestEqual(TEXT("11 Timber pieces (lintels, purlins, ridge, cleat, posts, overhang)"),
		CountMaterial(Layout.Structure, &Timber, false, false), ExpectedTimber);

	FShed S;
	const bool bIdentified = Identify(Layout, S);

	if (!bIdentified)
	{
		AddError(TEXT(
			"the built recognizable 3D shed must identify its named pieces by POSITION. Failing this means "
			"either the wrong structure was laid or the builder's geometry did not survive "
			"DestructionScenarios::Build (the AdoptLayout material-carrying promise)."));

		return false;
	}

	// Materials as authored, after the catalogue Build path.
	TestTrue(TEXT("the back wall is ClayBrick"), Layout.Structure.GetPiece(S.BackWall).Material == &ClayBrick);
	TestTrue(TEXT("the right wall is ClayBrick"), Layout.Structure.GetPiece(S.RightWall).Material == &ClayBrick);
	TestTrue(TEXT("the door piers are ClayBrick"),
		Layout.Structure.GetPiece(S.LeftPier).Material == &ClayBrick
			&& Layout.Structure.GetPiece(S.RightPier).Material == &ClayBrick);
	TestTrue(TEXT("the sill course is ClayBrick"), Layout.Structure.GetPiece(S.Sill).Material == &ClayBrick);
	TestTrue(TEXT("the window jambs are ClayBrick"),
		Layout.Structure.GetPiece(S.WinJambBack).Material == &ClayBrick
			&& Layout.Structure.GetPiece(S.WinJambFront).Material == &ClayBrick);
	TestTrue(TEXT("the gable courses are ClayBrick"),
		Layout.Structure.GetPiece(S.FGableBase).Material == &ClayBrick
			&& Layout.Structure.GetPiece(S.FGableApex).Material == &ClayBrick);
	TestTrue(TEXT("the door lintel is Timber"), Layout.Structure.GetPiece(S.DoorHeader).Material == &Timber);
	TestTrue(TEXT("the window lintel is Timber"), Layout.Structure.GetPiece(S.WinLintel).Material == &Timber);
	TestTrue(TEXT("the ridge beam is Timber"), Layout.Structure.GetPiece(S.Ridge).Material == &Timber);
	TestTrue(TEXT("the overhang is Timber"), Layout.Structure.GetPiece(S.Overhang).Material == &Timber);
	TestTrue(TEXT("the left post is Timber and grounded"),
		Layout.Structure.GetPiece(S.PostL).Material == &Timber && Layout.Structure.GetPiece(S.PostL).bIsGrounded);
	TestTrue(TEXT("the right post is Timber and grounded"),
		Layout.Structure.GetPiece(S.PostR).Material == &Timber && Layout.Structure.GetPiece(S.PostR).bIsGrounded);

	TestTrue(TEXT("the laid 3D shed knows where every piece and joint is, or every moment is silently zero"),
		Layout.Structure.HasCompleteGeometry());

	/*
	 * Arm 1: the assembled shed stands. The bridge poses it in 3D, the LP says Stands, and
	 * SolveAndBreak leaves nothing Stranded with the spanning pieces held up.
	 */
	{
		FBrickLayout Assembled;
		TArray<int32> AssembledCut;

		if (!Build(Scenario, Assembled, AssembledCut))
		{
			AddError(TEXT("ARM 1: the 3D shed row must build a fresh copy"));
			return false;
		}

		RigidBlockOracle::FOracleProblem Problem;
		FString BridgeWhy;
		const bool bBridged =
			RigidBlockOracle::BuildRigidBlockProblem(Assembled.Structure, Problem, BridgeWhy);

		TestTrue(
			*FString::Printf(TEXT("ARM 1: the oracle bridge must accept this 3D shed (%s)"), *BridgeWhy),
			bBridged);

		if (bBridged)
		{
			// A 2D pose here means the 3D flag was lost in Build.
			TestEqual(TEXT("ARM 1: the bridged problem must be posed in 3D"),
				static_cast<int32>(Problem.Dim), static_cast<int32>(RigidBlockOracle::EOracleDim::Dim3D));

			const RigidBlockOracle::FOracleResult Live = RigidBlockOracle::SolveRigidBlock(Problem);
			const RigidBlockOracle::EOracleOutcome Outcome = RigidBlockOracle::OutcomeOf(Live);

			AddInfo(FString::Printf(
				TEXT("ARM 1: oracle answered %d, lambda* %.10g, outcome %d (2=Stands,1=Falls)"),
				Live.bAnswered ? 1 : 0, Live.Lambda, static_cast<int32>(Outcome)));

			TestTrue(TEXT("ARM 1: the oracle must ANSWER"), Live.bAnswered);
			TestEqual(TEXT("ARM 1: the assembled 3D shed's LP feasibility must be STANDS"),
				static_cast<int32>(Outcome),
				static_cast<int32>(RigidBlockOracle::EOracleOutcome::Stands));
			TestTrue(
				*FString::Printf(TEXT("ARM 1: lambda* %.10g must sit at or above 1"), Live.Lambda),
				Live.bAnswered && Live.Lambda >= 1.0);
		}

		// The production call; below the block cap it breaks via BreakByEquilibrium (the 3D LP).
		const int32 Passes = Assembled.Structure.SolveAndBreak();
		const int32 Stranded = StrandedCount(Assembled.Structure);

		AddInfo(FString::Printf(TEXT("ARM 1: PRODUCTION ran %d pass(es); %d stranded"), Passes, Stranded));

		TestEqual(TEXT("ARM 1: nothing may be Stranded — the verdict must be about the shed"),
			Stranded, 0);

		FShed AS;
		if (Identify(Assembled, AS))
		{
			// Spanning pieces stay held up: lintels, ridge, and the overhang on its posts and cleat.
			TestTrue(TEXT("ARM 1: the door lintel is held up as laid, through the world path"),
				IsStanding(Assembled.Structure.GetPieceSupport(AS.DoorHeader)));
			TestTrue(TEXT("ARM 1: the window lintel is held up as laid, through the world path"),
				IsStanding(Assembled.Structure.GetPieceSupport(AS.WinLintel)));
			TestTrue(TEXT("ARM 1: the ridge is held up as laid, through the world path"),
				IsStanding(Assembled.Structure.GetPieceSupport(AS.Ridge)));
			TestTrue(TEXT("ARM 1: the porch overhang is held up as laid, through the world path"),
				IsStanding(Assembled.Structure.GetPieceSupport(AS.Overhang)));
		}
	}

	/*
	 * Arm 2: the row's cut is PostR; pulling it makes the LP mechanism move the overhang and
	 * SolveAndBreak drop it, while the shed stays up.
	 */
	TestEqual(
		TEXT("ARM 2: the 3D shed row must name exactly one cut — the porch post to pull"),
		Cut.Num(), 1);

	TestTrue(
		TEXT("ARM 2: the shared cut centre (230,340,100) is PostR's box centre (the right porch post)"),
		Layout.Boxes.IsValidIndex(S.PostR)
			&& Layout.Boxes[S.PostR].CentreCm.Equals(CutCentreForPostR, 1.0e-6));

	if (Cut.Num() == 1)
	{
		const int32 CutPiece = Cut[0];

		TestTrue(
			*FString::Printf(TEXT("ARM 2: the cut must name the grounded Timber right porch post PostR "
				"(it named piece %d, PostR is piece %d)"), CutPiece, S.PostR),
			CutPiece == S.PostR);
	}

	{
		FBrickLayout Pulled;
		TArray<int32> PulledCut;

		if (!Build(Scenario, Pulled, PulledCut))
		{
			AddError(TEXT("ARM 2: the 3D shed row must build a fresh copy"));
			return false;
		}

		FShed PS;
		if (!Identify(Pulled, PS))
		{
			AddError(TEXT("ARM 2: the freshly built 3D shed must identify"));
			return false;
		}

		// Apply the row's own cut list.
		for (const int32 Piece : PulledCut)
		{
			Pulled.Structure.RemovePiece(Piece);
		}

		// If the row named no cut (already a failure above), pull PostR so this arm still tests something.
		if (PulledCut.Num() == 0)
		{
			Pulled.Structure.RemovePiece(PS.PostR);
		}

		RigidBlockOracle::FOracleProblem Problem;
		FString BridgeWhy;
		const bool bBridged =
			RigidBlockOracle::BuildRigidBlockProblem(Pulled.Structure, Problem, BridgeWhy);

		TestTrue(
			*FString::Printf(TEXT("ARM 2: the oracle bridge must accept the post-removed 3D shed (%s)"),
				*BridgeWhy),
			bBridged);

		if (bBridged)
		{
			TestEqual(TEXT("ARM 2: the post-removed problem must still be posed in 3D"),
				static_cast<int32>(Problem.Dim), static_cast<int32>(RigidBlockOracle::EOracleDim::Dim3D));

			const RigidBlockOracle::FOracleResult Live = RigidBlockOracle::SolveRigidBlock(Problem);
			const RigidBlockOracle::EOracleOutcome Outcome = RigidBlockOracle::OutcomeOf(Live);

			AddInfo(FString::Printf(
				TEXT("ARM 2: oracle answered %d, lambda* %.10g, outcome %d (2=Stands,1=Falls)"),
				Live.bAnswered ? 1 : 0, Live.Lambda, static_cast<int32>(Outcome)));

			TestTrue(TEXT("ARM 2: the oracle must ANSWER"), Live.bAnswered);
			TestEqual(TEXT("ARM 2: pulling the post must make the LP feasibility FALLS"),
				static_cast<int32>(Outcome),
				static_cast<int32>(RigidBlockOracle::EOracleOutcome::Falls));
			TestTrue(
				*FString::Printf(TEXT("ARM 2: lambda* %.10g must sit clearly below 1"), Live.Lambda),
				Live.bAnswered && Live.Lambda < 0.9);

			// The dead-gravity collapse mechanism must move the overhang, so the fall is a real loss of equilibrium.
			RigidBlockOracle::FOracleProblem Dead = Problem;
			Dead.bGravityIsLive = false;
			const RigidBlockOracle::FOracleResult DeadR = RigidBlockOracle::SolveRigidBlock(Dead);

			TestTrue(TEXT("ARM 2: the LP must extract a certified collapse mechanism"),
				DeadR.Mechanism.bPresent && DeadR.Mechanism.bIsCertified);

			const int32 Block = OracleBlockOfPiece(Dead, PS.Overhang);
			const bool bMoves = DeadR.Mechanism.bPresent
				&& DeadR.Mechanism.Blocks.IsValidIndex(Block)
				&& DeadR.Mechanism.Blocks[Block].bMoves;

			AddInfo(FString::Printf(TEXT("ARM 2: overhang is oracle block %d, moves %d"),
				Block, bMoves ? 1 : 0));

			TestTrue(TEXT("ARM 2: the mechanism must name the overhang as a moving block"), bMoves);
		}

		// Production SolveAndBreak must reach the same verdict as the oracle call above.
		const int32 Passes = Pulled.Structure.SolveAndBreak();
		const int32 Stranded = StrandedCount(Pulled.Structure);

		AddInfo(FString::Printf(TEXT("ARM 2: PRODUCTION ran %d pass(es); %d stranded"), Passes, Stranded));

		TestEqual(TEXT("ARM 2: nothing may be Stranded — the verdict must be about the shed"),
			Stranded, 0);

		// The shed itself, independent of the porch, stays up.
		const TArray<int32> ExpectedStanding = { PS.BackWall, PS.RightWall, PS.LeftPier, PS.RightPier,
			PS.Sill, PS.PostL, PS.DoorHeader, PS.Ridge };

		for (const int32 Piece : ExpectedStanding)
		{
			TestTrue(
				*FString::Printf(TEXT("ARM 2: piece %d must still be held up (support %d)"), Piece,
					static_cast<int32>(Pulled.Structure.GetPieceSupport(Piece))),
				IsStanding(Pulled.Structure.GetPieceSupport(Piece)));
		}

		TestTrue(
			*FString::Printf(TEXT("ARM 2: the overhang (piece %d) must lose the earth (support %d) — "
				"pulling the post drops the porch, decided by the world break authority"),
				PS.Overhang, static_cast<int32>(Pulled.Structure.GetPieceSupport(PS.Overhang))),
			HasLostTheEarth(Pulled.Structure, PS.Overhang));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
