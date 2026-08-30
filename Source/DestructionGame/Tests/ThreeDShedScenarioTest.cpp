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
 * THE 3D SHED AS A PLAYABLE SCENARIO — the point where the genuinely-3D closed-box shed the
 * DestructionShed3D::Build builder lays (Acceptance.Shed.ThreeD.BuildsAClosedBoxThatStandsAndCollapsesCorrectly)
 * stops being reachable only from a unit test and becomes a LEVEL a human can join, watch hold, and
 * watch fall — the next step of the active /goal "finish the 3d solution".
 *
 * THE BEHAVIOUR, IN ONE SENTENCE. The catalogue carries a `shed3d` row whose MapName is `Lvl_Shed3D`
 * and whose LayStructure is the 3D shed builder, so that DestructionScenarios::Build lays the
 * seven-piece genuinely-3D closed-box shed — four grounded ClayBrick walls, a Timber roof, a Timber
 * overhang and a grounded Timber post — as a structure FLAGGED 3D, which STANDS through the
 * PRODUCTION world path (AdoptLayout -> the rigid-block bridge -> SolveRigidBlock / BreakByEquilibrium
 * inside SolveAndBreak), and whose named cut PULLS THE POST so that, once applied, the WORLD/production
 * break authority (the 3D LP mechanism, not merely an oracle unit call) makes the overhang lose the
 * earth while the four grounded walls keep it.
 *
 * =====================================================================================
 * WHY THIS IS THE CATALOGUE / WORLD PATH, NOT A SECOND COPY OF THE 3D BUILDER TEST
 * =====================================================================================
 *
 * `Acceptance.Shed.ThreeD.BuildsAClosedBoxThatStandsAndCollapsesCorrectly` proves the BUILDER —
 * DestructionShed3D::Build called directly, then bridged and solved. This test proves the SCENARIO
 * AND THE WORLD PATH: that a catalogue row exists, names the 3D shed builder as its LayStructure,
 * that DestructionScenarios::Build routes through that lambda, that the builder's 3D FLAG and its
 * per-piece MATERIALS survive the Build path into Layout.Structure (the AdoptLayout promise), and —
 * the part no oracle unit test reaches — that the 3D structure travels the SAME production door the
 * game uses: FStructure::SolveAndBreak, whose below-cap authority is BreakByEquilibrium (the LP).
 * The shed stands and the post fells the overhang exactly as the builder test's arms do, but reached
 * the way a human reaches it: by joining a level.
 *
 * THE RED IS THE MISSING ROW. The `shed3d` row is not in the catalogue yet, so IndexOfName answers
 * INDEX_NONE and this test stops at the first assertion with a clear message — not a type error, a
 * compile stub, or a wrong-axis reading. dev-expert's green step is one catalogue row (a MapName, a
 * LayStructure calling DestructionShed3D::Build{}, and a one-piece cut list naming the post) plus the
 * duplicated `Lvl_Shed3D.umap` that Content.ScenarioMapsExist and Content.ScenarioMapsAreDistinctAssets
 * will then require. THE MAP IS A CONTENT STEP, made with Scripts/New-ScenarioMap.ps1, NEVER a file
 * copy: a copied .umap keeps the inner UWorld object's original name, so every copy claims the same
 * `Map:<name>` PrimaryAssetId and the editor refuses to open any of them.
 *
 * WHY THE ROW NEEDS ITS OWN MAP AND CANNOT RIDE `Lvl_Sandbox` OR `Lvl_Shed`. A scenario is selected
 * back from its map by IndexOfMapName, which returns the FIRST row spelling that map name. A row with
 * MapName `Lvl_Sandbox` (row 0) or `Lvl_Shed` (the 2D shed) would be selectable only by
 * `?Scenario=shed3d`, never by opening a level, and it would collide with that other row in both the
 * ScenarioMapsExist distinctness sweep and the ScenarioMapsAreDistinctAssets PrimaryAssetId sweep. A
 * distinct `Lvl_Shed3D` is mandatory; this test pins the row's MapName to it so the two content
 * guards then bite on the absent .umap.
 *
 * WHY THE ASSERTIONS ARE ON MECHANISM AND OUTCOME, NEVER DISPLACEMENT. Stands is asserted as LP
 * feasibility plus Stranded == 0 and the beams held up; Falls is asserted as the LP mechanism NAMING
 * the overhang a moving block AND production's GetPieceSupport reading it Fallen — two pieces can
 * sever and rest exactly in place, so no displacement is measured. The production reads (SolveAndBreak
 * then GetPieceSupport) are the load-bearing half: they prove the 3D LP is the break authority in the
 * actual game world, not just in the builder's own oracle call.
 *
 * NEEDS A TICKING WORLD: NO. The catalogue is world-free, Build is arithmetic over boxes and a graph,
 * the bridge and the LP are pure, and SolveAndBreak is a synchronous settle over that graph — no
 * UWorld, no Chaos, no tick. Same footing as the 2D ShedScenarioTest and the 3D builder test.
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

	/** The seven pieces of the laid 3D shed, named by material, grounding and position — never by handle. */
	struct FShed
	{
		int32 BackWall = INDEX_NONE;
		int32 FrontWall = INDEX_NONE;
		int32 LeftWall = INDEX_NONE;
		int32 RightWall = INDEX_NONE;
		int32 Roof = INDEX_NONE;
		int32 Overhang = INDEX_NONE;
		int32 Post = INDEX_NONE;
	};

	/**
	 * Name the seven pieces from a laid layout, or fail. Four grounded ClayBrick walls, one grounded
	 * Timber post and two free Timber beams is the only shape that identifies — which is also the
	 * assertion that the builder's MATERIALS and the 3D grounding survived the Build path. The four
	 * walls are told apart by position (back smallest Y, front largest Y, then left smallest X and
	 * right largest X); the two free beams by Y centroid (roof inboard < overhang cantilevered).
	 */
	inline bool Identify(const FBrickLayout& Layout, FShed& Out)
	{
		const FStructure& S = Layout.Structure;

		TArray<int32> BrickGrounded, TimberGrounded, TimberFree;

		for (int32 Piece = 0; Piece < S.NumPieces(); ++Piece)
		{
			if (S.IsPieceRemoved(Piece))
			{
				continue;
			}

			const FStructurePiece& P = S.GetPiece(Piece);

			if (P.Material == &ClayBrick && P.bIsGrounded)
			{
				BrickGrounded.Add(Piece);
			}
			else if (P.Material == &Timber)
			{
				(P.bIsGrounded ? TimberGrounded : TimberFree).Add(Piece);
			}
		}

		if (BrickGrounded.Num() != 4 || TimberGrounded.Num() != 1 || TimberFree.Num() != 2)
		{
			return false;
		}

		/* Back = smallest Y centroid; front = largest Y. */
		BrickGrounded.Sort([&S](const int32& A, const int32& B)
		{
			return S.GetPiece(A).CentreOfMassCm.Y < S.GetPiece(B).CentreOfMassCm.Y;
		});
		Out.BackWall = BrickGrounded[0];
		Out.FrontWall = BrickGrounded[3];

		/* The middle two (side walls) are told apart by X: left smallest, right largest. */
		TArray<int32> Sides = { BrickGrounded[1], BrickGrounded[2] };
		Sides.Sort([&S](const int32& A, const int32& B)
		{
			return S.GetPiece(A).CentreOfMassCm.X < S.GetPiece(B).CentreOfMassCm.X;
		});
		Out.LeftWall = Sides[0];
		Out.RightWall = Sides[1];

		Out.Post = TimberGrounded[0];

		TimberFree.Sort([&S](const int32& A, const int32& B)
		{
			return S.GetPiece(A).CentreOfMassCm.Y < S.GetPiece(B).CentreOfMassCm.Y;
		});
		Out.Roof = TimberFree[0];
		Out.Overhang = TimberFree[1];

		return true;
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
 * THE 3D SHED ROW IS IN THE CATALOGUE, IT LAYS THE 3D SHED THROUGH THE BUILD PATH FLAGGED 3D, IT
 * STANDS THROUGH THE PRODUCTION WORLD PATH, AND THE POST IT NAMES FELLS THE OVERHANG WHEN PULLED — the
 * whole loss decided by the world break authority (the 3D LP), not by an oracle unit call.
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
	 * ARM 0 — THE ROW EXISTS AND NAMES THE 3D SHED. This is where the scenario is RED: no `shed3d`
	 * row yet, so IndexOfName answers INDEX_NONE and there is nothing to build or solve.
	 * ================================================================================ */

	const int32 Index = IndexOfName(FName(Shed3DScenarioName));

	if (!Catalogue().IsValidIndex(Index))
	{
		AddError(FString::Printf(
			TEXT("the catalogue must carry a row named '%s' — the 3D shed is not joinable until it "
				"does; IndexOfName returned %d against %d row(s). This is the RED: dev adds the row (a "
				"MapName '%s', a LayStructure calling DestructionShed3D::Build, and a one-piece cut "
				"naming the post) plus the duplicated Lvl_Shed3D.umap the content guards then require "
				"(made with New-ScenarioMap.ps1, never a file copy — a copy collides on PrimaryAssetId)."),
			Shed3DScenarioName, Index, Catalogue().Num(), Shed3DScenarioMapName));

		return false;
	}

	const FScenario& Scenario = Catalogue()[Index];

	/* --- the row must carry its own distinct map, or it is joinable only by ?Scenario= and collides
	 * with sandbox / the 2D shed in both content guards. A distinct Lvl_Shed3D is mandatory. --- */
	TestEqual(
		TEXT("the 3D shed row must name its own map 'Lvl_Shed3D' — it cannot ride Lvl_Sandbox or Lvl_Shed"),
		FString(Scenario.MapName ? Scenario.MapName : TEXT("")), FString(Shed3DScenarioMapName));

	/* --- the row must lay its structure through a LayStructure producer, so Build routes the 3D shed
	 * builder rather than the running-bond fallback. --- */
	TestTrue(
		TEXT("the 3D shed row must carry a LayStructure producer (the shed is not a running-bond wall)"),
		static_cast<bool>(Scenario.LayStructure));

	/* ================================================================================
	 * ARM 0 (cont.) — DestructionScenarios::Build lays the seven-piece 3D shed, the structure is
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

	TestEqual(TEXT("seven pieces — four walls, roof, overhang, post"),
		Layout.Structure.NumPieces(), 7);
	TestEqual(TEXT("one box per piece, or AdoptLayout refuses the layout"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());
	TestEqual(TEXT("eight joints — four corners, two roof bearings, the fixing, the post bearing"),
		Layout.Structure.NumConnections(), 8);

	FShed S;
	const bool bIdentified = Identify(Layout, S);

	if (!bIdentified)
	{
		AddError(TEXT(
			"the built 3D shed must identify by MATERIAL and grounding — four grounded ClayBrick walls, "
			"one grounded Timber post, two free Timber beams. Failing this means either the wrong "
			"structure was laid or the builder's materials/grounding did not survive "
			"DestructionScenarios::Build (the AdoptLayout material-carrying promise)."));

		return false;
	}

	/* The multi-material 3D shape, as authored, having come through the catalogue Build path. */
	TestTrue(TEXT("the back wall is ClayBrick"), Layout.Structure.GetPiece(S.BackWall).Material == &ClayBrick);
	TestTrue(TEXT("the front wall is ClayBrick"), Layout.Structure.GetPiece(S.FrontWall).Material == &ClayBrick);
	TestTrue(TEXT("the left wall is ClayBrick"), Layout.Structure.GetPiece(S.LeftWall).Material == &ClayBrick);
	TestTrue(TEXT("the right wall is ClayBrick"), Layout.Structure.GetPiece(S.RightWall).Material == &ClayBrick);
	TestTrue(TEXT("the roof beam is Timber"), Layout.Structure.GetPiece(S.Roof).Material == &Timber);
	TestTrue(TEXT("the overhang is Timber"), Layout.Structure.GetPiece(S.Overhang).Material == &Timber);
	TestTrue(TEXT("the post is Timber and grounded"),
		Layout.Structure.GetPiece(S.Post).Material == &Timber && Layout.Structure.GetPiece(S.Post).bIsGrounded);

	TestTrue(TEXT("the laid 3D shed knows where every piece and joint is, or every moment is silently zero"),
		Layout.Structure.HasCompleteGeometry());

	/* ================================================================================
	 * ARM 1 — THE ASSEMBLED 3D SHED STANDS THROUGH THE PRODUCTION WORLD PATH. The bridge poses it in
	 * 3D and the LP finds equilibrium (Stands); production's SolveAndBreak leaves nothing Stranded and
	 * holds the roof and overhang up. The catalogue-built, world-path version of the builder's arm 0.
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
			TestTrue(TEXT("ARM 1: the roof is held up as laid, through the world path"),
				IsStanding(Assembled.Structure.GetPieceSupport(AS.Roof)));
			TestTrue(TEXT("ARM 1: the overhang is held up as laid, through the world path"),
				IsStanding(Assembled.Structure.GetPieceSupport(AS.Overhang)));
		}
	}

	/* ================================================================================
	 * ARM 2 — THE ROW'S CUT PULLS THE POST, AND THE WORLD BREAK AUTHORITY FELLS THE OVERHANG. The
	 * level's headline "pull the post, it drops". The cut is the row's own, resolved by Build; the 3D
	 * LP mechanism must name the overhang a moving block AND production's SolveAndBreak must drop it,
	 * while the four grounded walls keep the earth. Assert on the mechanism and the outcome, never
	 * displacement.
	 * ================================================================================ */

	/* The row must name exactly the post as its cut — the watchable event this level exists for. */
	TestEqual(
		TEXT("ARM 2: the 3D shed row must name exactly one cut — the post to pull"),
		Cut.Num(), 1);

	if (Cut.Num() == 1)
	{
		const int32 CutPiece = Cut[0];

		TestTrue(
			*FString::Printf(TEXT("ARM 2: the cut must name the grounded Timber POST (it named piece %d)"),
				CutPiece),
			CutPiece == S.Post);
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

		/* If the row named no cut (the RED-adjacent case dev must fix), pull the post ourselves so the
		 * mechanism arm still says something rather than passing on an intact shed. */
		if (PulledCut.Num() == 0)
		{
			Pulled.Structure.RemovePiece(PS.Post);
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

		/* Corner bracing: the four grounded walls keep the earth when the post goes. */
		TestTrue(TEXT("ARM 2: the back wall keeps the earth"),
			Pulled.Structure.GetPieceSupport(PS.BackWall) == EPieceSupport::Grounded);
		TestTrue(TEXT("ARM 2: the front wall keeps the earth"),
			Pulled.Structure.GetPieceSupport(PS.FrontWall) == EPieceSupport::Grounded);
		TestTrue(TEXT("ARM 2: the left wall keeps the earth"),
			Pulled.Structure.GetPieceSupport(PS.LeftWall) == EPieceSupport::Grounded);
		TestTrue(TEXT("ARM 2: the right wall keeps the earth"),
			Pulled.Structure.GetPieceSupport(PS.RightWall) == EPieceSupport::Grounded);

		TestTrue(
			*FString::Printf(TEXT("ARM 2: the overhang must lose the earth (support %d) — pulling the "
				"post drops it, decided by the world break authority"),
				static_cast<int32>(Pulled.Structure.GetPieceSupport(PS.Overhang))),
			HasLostTheEarth(Pulled.Structure, PS.Overhang));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
