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
 * THE RECOGNIZABLE 3D SHED AS A PLAYABLE SCENARIO — the point where the genuinely-3D shed the
 * DestructionShed3D::BuildRecognizable builder lays (Acceptance.Shed.ThreeD.RecognizableShedStandsAsBuilt)
 * stops being reachable only from a unit test and becomes a LEVEL a human can join, watch hold, and
 * watch fall — the world-path counterpart of the recognizable-shed builder tests.
 *
 * THE BEHAVIOUR, IN ONE SENTENCE. The catalogue carries a `shed3d` row whose MapName is `Lvl_Shed3D`
 * and whose LayStructure is the recognizable 3D shed builder, so that DestructionScenarios::Build lays
 * the 24-piece genuinely-3D shed — four ClayBrick walls closing a box with a DOOR (two piers carrying a
 * Timber lintel) and a WINDOW (a sill, two jambs, a Timber lintel), STEPPED brick gables rising to a
 * ridge, a Timber roof of purlins bearing on the gable shoulders, and over the door a Timber porch that
 * CANTILEVERS out on two grounded posts, its back tied to the wall by a narrow central cleat — as a
 * structure FLAGGED 3D, which STANDS through the PRODUCTION world path (AdoptLayout -> the rigid-block
 * bridge -> SolveRigidBlock / BreakByEquilibrium inside SolveAndBreak), and whose named cut PULLS THE
 * RIGHT-HAND PORCH POST so that, once applied, the WORLD/production break authority (the 3D LP mechanism,
 * not merely an oracle unit call) makes the overhang lose the earth while the shed keeps it.
 *
 * =====================================================================================
 * WHY THIS IS THE CATALOGUE / WORLD PATH, NOT A SECOND COPY OF THE BUILDER TEST
 * =====================================================================================
 *
 * `Acceptance.Shed.ThreeD.RecognizableShedStandsAsBuilt` and
 * `Acceptance.Shed.ThreeD.RecognizableShedCollapsesWhenAPostOrPierIsPulled` prove the BUILDER —
 * DestructionShed3D::BuildRecognizable called directly, then bridged and solved. This test proves the
 * SCENARIO AND THE WORLD PATH: that a catalogue row exists, names the recognizable 3D shed builder as its
 * LayStructure, that DestructionScenarios::Build routes through that lambda, that the builder's 3D FLAG
 * and its per-piece MATERIALS survive the Build path into Layout.Structure (the AdoptLayout promise), and —
 * the part no oracle unit test reaches — that the 3D structure travels the SAME production door the game
 * uses: FStructure::SolveAndBreak, whose below-cap authority is BreakByEquilibrium (the LP). The shed
 * stands and pulling the right post fells the porch overhang exactly as the builder tests' arms do, but
 * reached the way a human reaches it: by joining a level.
 *
 * WHY THE ROW NEEDS ITS OWN MAP AND CANNOT RIDE `Lvl_Sandbox` OR `Lvl_Shed`. A scenario is selected back
 * from its map by IndexOfMapName, which returns the FIRST row spelling that map name. A row with MapName
 * `Lvl_Sandbox` (row 0) or `Lvl_Shed` (the 2D shed) would be selectable only by `?Scenario=shed3d`, never
 * by opening a level, and it would collide with that other row in both the ScenarioMapsExist distinctness
 * sweep and the ScenarioMapsAreDistinctAssets PrimaryAssetId sweep. A distinct `Lvl_Shed3D` is mandatory;
 * this test pins the row's MapName to it.
 *
 * WHY THE ASSERTIONS ARE ON MECHANISM AND OUTCOME, NEVER DISPLACEMENT. Stands is asserted as LP
 * feasibility plus Stranded == 0 and the spanning pieces held up; Falls is asserted as the LP mechanism
 * NAMING the overhang a moving block AND production's GetPieceSupport reading it having lost the earth —
 * two pieces can sever and rest exactly in place, so no displacement is measured. The production reads
 * (SolveAndBreak then GetPieceSupport) are the load-bearing half: they prove the 3D LP is the break
 * authority in the actual game world, not just in the builder's own oracle call.
 *
 * NEEDS A TICKING WORLD: NO. The catalogue is world-free, BuildRecognizable is arithmetic over boxes and
 * a graph, the bridge and the LP are pure, and SolveAndBreak is a synchronous settle over that graph — no
 * UWorld, no Chaos, no tick. Same footing as the 2D ShedScenarioTest and the recognizable builder tests.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges files into one translation unit.
 */
namespace ThreeDShedScenarioTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/** What `?Scenario=` names on a URL, and what IndexOfName looks up. */
	const TCHAR* const Shed3DScenarioName = TEXT("shed3d");

	/** The distinct map the row must select and be selected by. A content step for dev, not this test. */
	const TCHAR* const Shed3DScenarioMapName = TEXT("Lvl_Shed3D");

	/* ================================================================================
	 * THE CANONICAL SHED, spelled out so the identification reads the SAME numbers the builder lays. The
	 * builder hardcodes these; the test reads back the laid layout and identifies pieces by POSITION, so
	 * the assertions survive any re-handle of the pieces. These centroids are the same table the
	 * recognizable builder tests use.
	 * ================================================================================ */

	constexpr int32 ExpectedPieces = 24;
	constexpr int32 ExpectedJoints = 30;
	constexpr int32 ExpectedGrounded = 7;    // BackWall, RightWall, LeftPier, RightPier, Sill, PostL, PostR
	constexpr int32 ExpectedBrick = 13;
	constexpr int32 ExpectedTimber = 11;     // + the Cleat (the wall tie); the cleat is NOT grounded

	/* The right porch post's box centre — the cut the row must name. BuildRecognizable lays PostR over
	 * X[215,245] Y[328,352] Z[0,200], so its box centre is (230, 340, 100). The scenario's cut centre must
	 * equal this to the ulp (ScenariosPieceAtCentre uses an exact-to-1e-6 match), so this doubles as the
	 * independent check that (230, 340, 100) names PostR and not some other piece. */
	const FVector CutCentreForPostR(230.0, 340.0, 100.0);

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

	/** The pieces of the laid recognizable 3D shed, named by POSITION, never by handle. */
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

	/** True when a live piece has lost every path to the earth — the outcome a dropped piece shows. */
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
 * THE RECOGNIZABLE 3D SHED ROW IS IN THE CATALOGUE, IT LAYS THE 24-PIECE SHED THROUGH THE BUILD PATH
 * FLAGGED 3D, IT STANDS THROUGH THE PRODUCTION WORLD PATH, AND THE POST IT NAMES FELLS THE PORCH OVERHANG
 * WHEN PULLED — the whole loss decided by the world break authority (the 3D LP), not by an oracle unit
 * call.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
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

	/* ================================================================================
	 * ARM 0 — THE ROW EXISTS AND NAMES THE RECOGNIZABLE 3D SHED.
	 * ================================================================================ */

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

	/* --- the row must carry its own distinct map, or it is joinable only by ?Scenario= and collides
	 * with sandbox / the 2D shed in both content guards. A distinct Lvl_Shed3D is mandatory. --- */
	TestEqual(
		TEXT("the 3D shed row must name its own map 'Lvl_Shed3D' — it cannot ride Lvl_Sandbox or Lvl_Shed"),
		FString(Scenario.MapName ? Scenario.MapName : TEXT("")), FString(Shed3DScenarioMapName));

	/* --- the row must lay its structure through a LayStructure producer, so Build routes the recognizable
	 * 3D shed builder rather than the running-bond fallback. --- */
	TestTrue(
		TEXT("the 3D shed row must carry a LayStructure producer (the shed is not a running-bond wall)"),
		static_cast<bool>(Scenario.LayStructure));

	/* --- and it is the one row framed from a three-quarter angle, so the box's depth and the overhang's
	 * fall off its front are both visible. --- */
	TestEqual(
		TEXT("the 3D shed row is framed from a three-quarter angle (a closed box, not a flat wall)"),
		static_cast<int32>(Scenario.Framing), static_cast<int32>(EScenarioFraming::ThreeQuarter));

	/* ================================================================================
	 * ARM 0 (cont.) — DestructionScenarios::Build lays the 24-piece recognizable 3D shed, the structure is
	 * FLAGGED 3D (the flag survives the Build/AdoptLayout path), and the builder's MATERIALS survive.
	 * ================================================================================ */

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

	/* THE 3D FLAG MUST SURVIVE THE WORLD BUILD PATH. If it does not, the bridge poses the structure in
	 * 2D and silently drops the Y-normal corners — the exact "world path doesn't carry the 3D flag" gap
	 * the scenario is meant to expose. Asserted on the built Layout.Structure, not on the builder. */
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

	/* The multi-material 3D shape, as authored, having come through the catalogue Build path. */
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

	/* ================================================================================
	 * ARM 1 — THE ASSEMBLED 3D SHED STANDS THROUGH THE PRODUCTION WORLD PATH. The bridge poses it in 3D
	 * and the LP finds equilibrium (Stands); production's SolveAndBreak leaves nothing Stranded and holds
	 * the spanning pieces up. The catalogue-built, world-path version of the builder test's stand arm.
	 * ================================================================================ */
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
			/* THE BRIDGE POSES IT IN 3D — the whole reason the row carries the 3D flag. A 2D pose here
			 * would mean the flag was lost between the builder and the world Build path. */
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

		/* THE PRODUCTION WORLD DOOR — the same call the game makes. Below the block cap its authority
		 * is BreakByEquilibrium (the 3D LP), so this is the world path, not an oracle unit call. */
		const int32 Passes = Assembled.Structure.SolveAndBreak();
		const int32 Stranded = StrandedCount(Assembled.Structure);

		AddInfo(FString::Printf(TEXT("ARM 1: PRODUCTION ran %d pass(es); %d stranded"), Passes, Stranded));

		TestEqual(TEXT("ARM 1: nothing may be Stranded — the verdict must be about the shed"),
			Stranded, 0);

		FShed AS;
		if (Identify(Assembled, AS))
		{
			/* The carried spanning pieces read Supported through the world path — the lintels by their
			 * piers/jambs, the ridge by the gables, the overhang by the posts + cleat tie. */
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

	/* ================================================================================
	 * ARM 2 — THE ROW'S CUT PULLS THE RIGHT PORCH POST, AND THE WORLD BREAK AUTHORITY FELLS THE OVERHANG.
	 * The level's headline "pull a post, the porch drops". The cut is the row's own, resolved by Build; it
	 * must name PostR (the 3D LP mechanism must name the overhang a moving block AND production's
	 * SolveAndBreak must drop it), while the shed keeps the earth. Assert on the mechanism and the outcome,
	 * never displacement.
	 * ================================================================================ */

	/* The row must name exactly the right porch post as its cut — the watchable event this level exists
	 * for. The centre (230,340,100) is PostR's box centre; ScenariosPieceAtCentre resolves it exactly, so
	 * asserting the resolved piece IS PostR is the independent check that the centre names the post. */
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

		/* Apply the row's own cut list — the removal the level performs after the hold. */
		for (const int32 Piece : PulledCut)
		{
			Pulled.Structure.RemovePiece(Piece);
		}

		/* If the row named no cut (a RED-adjacent case) pull PostR ourselves so the mechanism arm still
		 * says something rather than passing on an intact shed. */
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

			/* The collapse mechanism (gravity dead) must NAME the overhang a moving block, so the fall
			 * is a genuine loss of equilibrium and not a routing artefact. */
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

		/* THE WORLD BREAK AUTHORITY. SolveAndBreak below the cap IS the 3D LP mechanism, so this
		 * production read is the proof that the loss happens in the actual game world, not merely in
		 * the oracle unit call above. */
		const int32 Passes = Pulled.Structure.SolveAndBreak();
		const int32 Stranded = StrandedCount(Pulled.Structure);

		AddInfo(FString::Printf(TEXT("ARM 2: PRODUCTION ran %d pass(es); %d stranded"), Passes, Stranded));

		TestEqual(TEXT("ARM 2: nothing may be Stranded — the verdict must be about the shed"),
			Stranded, 0);

		/* The shed keeps the earth when the right post goes: the four walls' feet, the surviving left
		 * post, the door lintel and the ridge are all independent of the porch. */
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
