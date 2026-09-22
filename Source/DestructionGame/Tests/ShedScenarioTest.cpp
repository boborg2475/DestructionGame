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
 * The shed as a playable scenario (SHED_PATH.md Phase F, slice F2). The catalogue's `shed` row
 * maps to `Lvl_Shed` and lays the seven-piece shed (two ClayBrick piers, Timber roof, overhang and
 * post) through DestructionScenarios::Build. It stands under the rigid-block LP; its cut pulls the
 * post, after which the overhang loses the earth and the piers keep it.
 *
 * The F1 test covers the builder directly; this covers the catalogue path, including that
 * per-piece materials survive Build. The row needs its own map: IndexOfMapName returns the first
 * row for a map name, so sharing Lvl_Sandbox would make it unreachable by map and collide in the
 * content guards. Make maps with New-ScenarioMap.ps1, never a file copy.
 *
 * Assertions are on mechanism, never displacement. No world needed. Named namespace for unity builds.
 */
namespace ShedScenarioTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/** What `?Scenario=` names on a URL, and what IndexOfName looks up. */
	const TCHAR* const ShedScenarioName = TEXT("shed");

	/** The row's own map. */
	const TCHAR* const ShedScenarioMapName = TEXT("Lvl_Shed");

	/** The seven shed pieces, identified by material, grounding and X order, never by handle. */
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
	 * Names the seven pieces, or fails. Requires two grounded and two free bricks, one grounded and
	 * two free timbers, so it also checks that materials survived Build.
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

	/** True when a live piece has lost every path to the earth. */
	inline bool HasLostTheEarth(const FStructure& S, int32 Piece)
	{
		if (S.IsPieceRemoved(Piece))
		{
			return false;
		}
		return !IsStanding(S.GetPieceSupport(Piece));
	}

	/** The oracle block mapped from a structure piece. */
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

/** The shed row lays the shed through Build, it stands, and pulling its named post fells the overhang. */
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

	// Arm 0: the row exists.

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

	// The row has its own map (see the file header).
	TestEqual(
		TEXT("the shed row must name its own map 'Lvl_Shed' — a scenario cannot ride Lvl_Sandbox"),
		FString(Scenario.MapName ? Scenario.MapName : TEXT("")), FString(ShedScenarioMapName));

	// A LayStructure producer, so Build uses the shed builder, not the running-bond fallback.
	TestTrue(
		TEXT("the shed row must carry a LayStructure producer (the shed is not a running-bond wall)"),
		static_cast<bool>(Scenario.LayStructure));

	// Build lays the seven-piece shed with its materials intact.

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

	// Arm 1: the assembled shed stands, in the oracle (lambda* >= 1) and in production.
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

	/*
	 * Arm 2: the row's cut pulls the post; the mechanism must name the overhang as moving and
	 * production must drop it, while the piers stay grounded.
	 */
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

		for (const int32 Piece : PulledCut)
		{
			Pulled.Structure.RemovePiece(Piece);
		}

		// If the row named no cut, pull the post here so this arm does not pass on an intact shed.
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

			// The gravity-dead mechanism must name the overhang as moving, so the fall is real.
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
