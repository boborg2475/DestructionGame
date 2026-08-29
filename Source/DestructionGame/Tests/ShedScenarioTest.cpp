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
 * THE SHED AS A PLAYABLE SCENARIO — SHED_PATH.md Phase F, slice F2, the point where the shed the F1
 * builder lays stops being reachable only from a unit test and becomes a LEVEL a human can join,
 * watch hold, and watch fall.
 *
 * THE BEHAVIOUR, IN ONE SENTENCE. The catalogue carries a `shed` row whose MapName is `Lvl_Shed`
 * and whose LayStructure is the F1 shed builder, so that DestructionScenarios::Build lays the
 * seven-piece multi-material shed — two ClayBrick piers, a Timber roof and overhang, a Timber post
 * — which STANDS under the rigid-block LP, and whose named cut PULLS THE POST so that, once
 * applied, the overhang loses the earth (the LP mechanism names it) while the two piers keep it.
 *
 * =====================================================================================
 * WHY THIS IS THE CATALOGUE PATH, NOT A SECOND COPY OF THE F1 BUILDER TEST
 * =====================================================================================
 *
 * `Acceptance.Shed.BuildsAMultiMaterialShedThatStandsAndCollapsesCorrectly` (F1) proves the BUILDER
 * — DestructionShed::Build called directly. This test proves the SCENARIO: that a catalogue row
 * exists, names the shed builder as its LayStructure, that DestructionScenarios::Build routes
 * through that lambda, and — the F0/AdoptLayout promise — that the per-piece MATERIALS the builder
 * authored survive the Build path into Layout.Structure so the cross-material physics is still
 * there when the level is laid. F1 is the builder; this is the level. The shed stands and the post
 * fells the overhang exactly as F1's arm 0 and arm 2 do, but reached the way a human reaches it.
 *
 * THE RED IS THE MISSING ROW. The `shed` row is not in the catalogue yet, so IndexOfName answers
 * INDEX_NONE and this test stops at the first assertion with a clear message — not a type error, a
 * compile stub, or a wrong-axis reading. dev-expert's green step is one catalogue row (a MapName, a
 * LayStructure calling DestructionShed::Build{}, and a one-piece cut list naming the post) plus the
 * duplicated `Lvl_Shed.umap` that Content.ScenarioMapsExist and Content.ScenarioMapsAreDistinctAssets
 * will then require — the map is a content step, made with New-ScenarioMap.ps1, never a file copy.
 *
 * WHY THE ROW NEEDS ITS OWN MAP AND CANNOT RIDE `Lvl_Sandbox`. A scenario is selected back from its
 * map by IndexOfMapName, which returns the FIRST row spelling that map name — `sandbox`, row 0. So a
 * shed row with MapName `Lvl_Sandbox` would be selectable only by `?Scenario=shed`, never by opening
 * a level, and it would collide with `sandbox` in both the ScenarioMapsExist distinctness sweep and
 * the ScenarioMapsAreDistinctAssets PrimaryAssetId sweep. A distinct `Lvl_Shed` is mandatory; this
 * test pins the row's MapName to it so the two content guards then bite on the absent .umap.
 *
 * NEEDS A TICKING WORLD: NO. The catalogue is world-free, Build is arithmetic over boxes and a
 * graph, the oracle and SolveAndBreak are pure — every assertion is on mechanism (material identity,
 * feasibility, the mechanism's moving blocks, Supported-vs-Falling, Stranded == 0), never on
 * displacement. Same footing as CorbelScenarioTest and the F1 builder test.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges files into one translation unit.
 */
namespace ShedScenarioTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/** What `?Scenario=` names on a URL, and what IndexOfName looks up. */
	const TCHAR* const ShedScenarioName = TEXT("shed");

	/** The distinct map the row must select and be selected by. A content step for dev, not this test. */
	const TCHAR* const ShedScenarioMapName = TEXT("Lvl_Shed");

	/** The seven pieces of the laid shed, named by material, grounding and relative X — never by handle. */
	struct FShed
	{
		int32 BackBase = INDEX_NONE;
		int32 FrontBase = INDEX_NONE;
		int32 BackHead = INDEX_NONE;
		int32 FrontHead = INDEX_NONE;
		int32 Roof = INDEX_NONE;
		int32 Overhang = INDEX_NONE;
		int32 Post = INDEX_NONE;
	};

	inline void SortByCentroidX(const FStructure& S, TArray<int32>& Handles)
	{
		Handles.Sort([&S](const int32& A, const int32& B)
		{
			return S.GetPiece(A).CentreOfMassCm.X < S.GetPiece(B).CentreOfMassCm.X;
		});
	}

	/**
	 * Name the seven pieces from a laid layout, or fail. Two grounded bricks (bases), two free bricks
	 * (heads), one grounded timber (post) and two free timbers (roof, overhang) is the only shape that
	 * identifies — which is also the assertion that the builder's MATERIALS survived the Build path.
	 */
	inline bool Identify(const FBrickLayout& Layout, FShed& Out)
	{
		const FStructure& S = Layout.Structure;

		TArray<int32> BrickGrounded, BrickFree, TimberGrounded, TimberFree;

		for (int32 Piece = 0; Piece < S.NumPieces(); ++Piece)
		{
			if (S.IsPieceRemoved(Piece))
			{
				continue;
			}

			const FStructurePiece& P = S.GetPiece(Piece);

			if (P.Material == &ClayBrick)
			{
				(P.bIsGrounded ? BrickGrounded : BrickFree).Add(Piece);
			}
			else if (P.Material == &Timber)
			{
				(P.bIsGrounded ? TimberGrounded : TimberFree).Add(Piece);
			}
		}

		if (BrickGrounded.Num() != 2 || BrickFree.Num() != 2
			|| TimberGrounded.Num() != 1 || TimberFree.Num() != 2)
		{
			return false;
		}

		SortByCentroidX(S, BrickGrounded);
		SortByCentroidX(S, BrickFree);
		SortByCentroidX(S, TimberFree);

		Out.BackBase = BrickGrounded[0];
		Out.FrontBase = BrickGrounded[1];
		Out.BackHead = BrickFree[0];
		Out.FrontHead = BrickFree[1];
		Out.Post = TimberGrounded[0];
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
 * THE SHED ROW IS IN THE CATALOGUE, IT LAYS THE F1 SHED THROUGH THE BUILD PATH, IT STANDS AS LAID,
 * AND THE POST IT NAMES FELLS THE OVERHANG WHEN PULLED.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShedScenarioCatalogueTest,
	"DestructionGame.World.Scenarios.ShedRow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FShedScenarioCatalogueTest::RunTest(const FString& Parameters)
{
	using namespace ShedScenarioTestSupport;
	using namespace DestructionProfiles;
	using namespace DestructionLayout;
	using namespace DestructionScenarios;

	/* ================================================================================
	 * ARM 0 — THE ROW EXISTS AND NAMES THE SHED. This is where F2 is RED: no `shed` row yet, so
	 * IndexOfName answers INDEX_NONE and there is nothing to build or solve.
	 * ================================================================================ */

	const int32 Index = IndexOfName(FName(ShedScenarioName));

	if (!Catalogue().IsValidIndex(Index))
	{
		AddError(FString::Printf(
			TEXT("the catalogue must carry a row named '%s' — the shed is not joinable until it does; "
				"IndexOfName returned %d against %d row(s). This is the F2 RED: dev adds the row (a "
				"MapName '%s', a LayStructure calling DestructionShed::Build, and a one-piece cut "
				"naming the post) plus the duplicated Lvl_Shed.umap the content guards then require."),
			ShedScenarioName, Index, Catalogue().Num(), ShedScenarioMapName));

		return false;
	}

	const FScenario& Scenario = Catalogue()[Index];

	/* --- the row must carry its own distinct map, or it is joinable only by ?Scenario= and collides
	 * with sandbox in both content guards. A distinct Lvl_Shed is mandatory (see the file header). --- */
	TestEqual(
		TEXT("the shed row must name its own map 'Lvl_Shed' — a scenario cannot ride Lvl_Sandbox"),
		FString(Scenario.MapName ? Scenario.MapName : TEXT("")), FString(ShedScenarioMapName));

	/* --- the row must lay its structure through a LayStructure producer, exactly as the corbels do,
	 * so that Build routes the shed builder rather than the running-bond fallback. --- */
	TestTrue(
		TEXT("the shed row must carry a LayStructure producer (the shed is not a running-bond wall)"),
		static_cast<bool>(Scenario.LayStructure));

	/* ================================================================================
	 * ARM 0 (cont.) — DestructionScenarios::Build lays the seven-piece multi-material shed, and the
	 * builder's MATERIALS survive the Build path (the F0/AdoptLayout promise, checked through Identify).
	 * ================================================================================ */

	FBrickLayout Layout;
	TArray<int32> Cut;

	const bool bBuilt = Build(Scenario, Layout, Cut);

	TestTrue(
		TEXT("the shed row must build — a row that cannot be laid is a level that cannot be joined"),
		bBuilt);

	if (!bBuilt)
	{
		return false;
	}

	TestEqual(TEXT("seven pieces — two pier bases, two heads, roof, overhang, post"),
		Layout.Structure.NumPieces(), 7);
	TestEqual(TEXT("one box per piece, or AdoptLayout refuses the layout"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());
	TestEqual(TEXT("six bed joints — two mortar beds, two roof bearings, the fixing, the post bearing"),
		Layout.Structure.NumConnections(), 6);

	FShed S;
	const bool bIdentified = Identify(Layout, S);

	if (!bIdentified)
	{
		AddError(TEXT(
			"the built shed must identify by MATERIAL and grounding — two grounded ClayBrick bases, "
			"two free ClayBrick heads, one grounded Timber post, two free Timber beams. Failing this "
			"means either the wrong structure was laid or the builder's materials did not survive "
			"DestructionScenarios::Build (the F0/AdoptLayout material-carrying promise)."));

		return false;
	}

	/* The multi-material shape, as authored, having come through the catalogue Build path. */
	TestTrue(TEXT("the back base is ClayBrick"), Layout.Structure.GetPiece(S.BackBase).Material == &ClayBrick);
	TestTrue(TEXT("the front base is ClayBrick"), Layout.Structure.GetPiece(S.FrontBase).Material == &ClayBrick);
	TestTrue(TEXT("the back head is ClayBrick"), Layout.Structure.GetPiece(S.BackHead).Material == &ClayBrick);
	TestTrue(TEXT("the front head is ClayBrick"), Layout.Structure.GetPiece(S.FrontHead).Material == &ClayBrick);
	TestTrue(TEXT("the roof beam is Timber"), Layout.Structure.GetPiece(S.Roof).Material == &Timber);
	TestTrue(TEXT("the overhang is Timber"), Layout.Structure.GetPiece(S.Overhang).Material == &Timber);
	TestTrue(TEXT("the post is Timber and grounded"),
		Layout.Structure.GetPiece(S.Post).Material == &Timber && Layout.Structure.GetPiece(S.Post).bIsGrounded);

	TestTrue(TEXT("the laid shed knows where every piece and joint is, or every moment is silently zero"),
		Layout.Structure.HasCompleteGeometry());

	/* ================================================================================
	 * ARM 1 — THE ASSEMBLED SHED STANDS. Oracle feasibility (Stands, lambda* at or above 1) and the
	 * production outcome (Stranded == 0, roof and overhang held up). The catalogue-built F1 arm 0.
	 * ================================================================================ */
	{
		FBrickLayout Assembled;
		TArray<int32> AssembledCut;

		if (!Build(Scenario, Assembled, AssembledCut))
		{
			AddError(TEXT("ARM 1: the shed row must build a fresh copy"));
			return false;
		}

		RigidBlockOracle::FOracleProblem Problem;
		FString BridgeWhy;
		const bool bBridged =
			RigidBlockOracle::BuildRigidBlockProblem(Assembled.Structure, Problem, BridgeWhy);

		TestTrue(
			*FString::Printf(TEXT("ARM 1: the oracle bridge must accept this 2D shed (%s)"), *BridgeWhy),
			bBridged);

		if (bBridged)
		{
			const RigidBlockOracle::FOracleResult Live = RigidBlockOracle::SolveRigidBlock(Problem);
			const RigidBlockOracle::EOracleOutcome Outcome = RigidBlockOracle::OutcomeOf(Live);

			AddInfo(FString::Printf(
				TEXT("ARM 1: oracle answered %d, lambda* %.10g, outcome %d (2=Stands,1=Falls)"),
				Live.bAnswered ? 1 : 0, Live.Lambda, static_cast<int32>(Outcome)));

			TestTrue(TEXT("ARM 1: the oracle must ANSWER"), Live.bAnswered);
			TestEqual(TEXT("ARM 1: the assembled shed's LP feasibility must be STANDS"),
				static_cast<int32>(Outcome),
				static_cast<int32>(RigidBlockOracle::EOracleOutcome::Stands));
			TestTrue(
				*FString::Printf(TEXT("ARM 1: lambda* %.10g must sit at or above 1"), Live.Lambda),
				Live.bAnswered && Live.Lambda >= 1.0);
		}

		const int32 Passes = Assembled.Structure.SolveAndBreak();
		const int32 Stranded = StrandedCount(Assembled.Structure);

		AddInfo(FString::Printf(TEXT("ARM 1: PRODUCTION ran %d pass(es); %d stranded"), Passes, Stranded));

		TestEqual(TEXT("ARM 1: nothing may be Stranded — the verdict must be about the shed"),
			Stranded, 0);

		FShed AS;
		if (Identify(Assembled, AS))
		{
			TestTrue(TEXT("ARM 1: the roof is held up as laid"),
				IsStanding(Assembled.Structure.GetPieceSupport(AS.Roof)));
			TestTrue(TEXT("ARM 1: the overhang is held up as laid"),
				IsStanding(Assembled.Structure.GetPieceSupport(AS.Overhang)));
		}
	}

	/* ================================================================================
	 * ARM 2 — THE ROW'S CUT PULLS THE POST, AND THAT FELLS THE OVERHANG. The catalogue-built F1
	 * arm 2: the level's headline "pull the post, it drops". The cut is the row's own, resolved by
	 * Build; the mechanism must name the overhang a moving block and production must drop it, while
	 * the two piers keep the earth. Assert on the mechanism, never on displacement.
	 * ================================================================================ */

	/* The row must name exactly the post as its cut — the watchable event this level exists for. */
	TestEqual(
		TEXT("ARM 2: the shed row must name exactly one cut — the post to pull"),
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
			AddError(TEXT("ARM 2: the shed row must build a fresh copy"));
			return false;
		}

		FShed PS;
		if (!Identify(Pulled, PS))
		{
			AddError(TEXT("ARM 2: the freshly built shed must identify"));
			return false;
		}

		/* Apply the row's own cut list — the removal the level performs after the hold. */
		for (const int32 Piece : PulledCut)
		{
			Pulled.Structure.RemovePiece(Piece);
		}

		/* If the row named no cut (the RED-adjacent case dev must fix), pull the post ourselves so
		 * the mechanism arm still says something rather than passing on an intact shed. */
		if (PulledCut.Num() == 0)
		{
			Pulled.Structure.RemovePiece(PS.Post);
		}

		RigidBlockOracle::FOracleProblem Problem;
		FString BridgeWhy;
		const bool bBridged =
			RigidBlockOracle::BuildRigidBlockProblem(Pulled.Structure, Problem, BridgeWhy);

		TestTrue(
			*FString::Printf(TEXT("ARM 2: the oracle bridge must accept the post-removed shed (%s)"),
				*BridgeWhy),
			bBridged);

		if (bBridged)
		{
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

			/* The collapse mechanism (gravity dead) must NAME the overhang a moving block, so the
			 * fall is a genuine loss of equilibrium and not a routing artefact. */
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

		const int32 Passes = Pulled.Structure.SolveAndBreak();
		const int32 Stranded = StrandedCount(Pulled.Structure);

		AddInfo(FString::Printf(TEXT("ARM 2: PRODUCTION ran %d pass(es); %d stranded"), Passes, Stranded));

		TestEqual(TEXT("ARM 2: nothing may be Stranded — the verdict must be about the shed"),
			Stranded, 0);

		TestTrue(TEXT("ARM 2: the back base keeps the earth"),
			Pulled.Structure.GetPieceSupport(PS.BackBase) == EPieceSupport::Grounded);
		TestTrue(TEXT("ARM 2: the front base keeps the earth"),
			Pulled.Structure.GetPieceSupport(PS.FrontBase) == EPieceSupport::Grounded);

		TestTrue(
			*FString::Printf(TEXT("ARM 2: the overhang must lose the earth (support %d) — pulling the "
				"post drops it"), static_cast<int32>(Pulled.Structure.GetPieceSupport(PS.Overhang))),
			HasLostTheEarth(Pulled.Structure, PS.Overhang));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
