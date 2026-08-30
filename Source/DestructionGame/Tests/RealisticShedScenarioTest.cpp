// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/DestructionShed3D.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#include "World/DestructionScenarios.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE REALISTIC-BRICK SHED AS A PLAYABLE SCENARIO — the point where the true-masonry shed the
 * DestructionShed3D::BuildRealistic builder lays (Acceptance.Shed.ThreeD.RealisticBrickShedShellStandsAsBuilt
 * and .RealisticBrickShedGablesAndRoofStandAsBuilt) stops being reachable only from a builder unit test and
 * becomes a LEVEL a human can join and watch hold — the world-path counterpart of the realistic-shed builder
 * tests, and the scenario slice (5) of the realistic-shed goal.
 *
 * THE BEHAVIOUR, IN ONE SENTENCE. The catalogue carries a `shedrealistic` row whose MapName is
 * `Lvl_ShedRealistic` and whose LayStructure is DestructionShed3D::BuildRealistic, so that
 * DestructionScenarios::Build lays the 442-piece real-brick shed — four single-wythe running-bond ClayBrick
 * walls of true 21.5 x 10.25 x 6.5 cm bricks on 1 cm mortar joints closing a box, a DOOR and a WINDOW opening
 * each under a Timber-board lintel, stepped brick GABLES rising to a ridge, a Timber gable ROOF, and a PORCH
 * of two grounded Timber posts carrying a cantilevered overhang over the door — as a
 * structure FLAGGED 3D (IsThreeDimensional), which STANDS through the PRODUCTION world path (FStructure::
 * SolveAndBreak, whose above-cap authority is the ROUTER's per-joint capacity sweep — the shed is 442 blocks,
 * far above the 200-block equilibrium-gate cap), with NO cut yet: this slice makes the realistic shed a
 * joinable, standing, renderable level; the collapse cut that pulls a support is a LATER slice.
 *
 * =====================================================================================
 * WHY THIS IS THE CATALOGUE / WORLD PATH, NOT A SECOND COPY OF THE BUILDER TEST
 * =====================================================================================
 *
 * The two RealisticBrickShed builder tests prove the BUILDER — BuildRealistic called directly, then solved.
 * This test proves the SCENARIO AND THE WORLD PATH: that a catalogue row exists, names the realistic builder
 * as its LayStructure, that DestructionScenarios::Build routes through that lambda, that the builder's 3D FLAG
 * and its per-piece MATERIALS survive the Build path into Layout.Structure (the AdoptLayout promise), and that
 * the 442-block shed travels the SAME production door the game uses: FStructure::SolveAndBreak, whose above-cap
 * authority is the router. The shed stands, reached the way a human reaches it: by joining a level.
 *
 * WHY THE ROW NEEDS ITS OWN MAP. A scenario is selected back from its map by IndexOfMapName, which returns the
 * FIRST row spelling that map name. A distinct `Lvl_ShedRealistic` is mandatory so this row is joinable by
 * opening a level and does not collide with sandbox / the 2D shed / the recognizable 3D shed in either the
 * Content.ScenarioMapsExist distinctness sweep or the Content.ScenarioMapsAreDistinctAssets PrimaryAssetId
 * sweep. This test pins the row's MapName to it; the .umap itself is a content step for dev (New-ScenarioMap.ps1).
 *
 * WHY THE ASSERTIONS ARE ON MECHANISM AND OUTCOME, NEVER DISPLACEMENT. Stands is asserted as production's
 * SolveAndBreak leaving Stranded == 0 and the spanning pieces (the two lintels, the ridge) reading Supported
 * through the world path — not as any piece having moved. Two pieces can sever and rest exactly in place, so
 * no displacement is measured. There is no collapse arm in this slice because the row names no cut.
 *
 * WHY NO LP / ORACLE ARM (unlike the recognizable Shed3DRow). At 442 blocks the shed is far above the LP's
 * 200-block cap, so SolveAndBreak's authority is the router (BreakByCapacitySweep), the same path the flagship
 * ~1200-block wall uses — the 3D LP is bypassed. The world-path proof here is therefore the production
 * SolveAndBreak read, not a bridge-then-SolveRigidBlock feasibility call.
 *
 * NEEDS A TICKING WORLD: NO. The catalogue is world-free, BuildRealistic is arithmetic over boxes and a graph,
 * and SolveAndBreak is a synchronous settle over that graph — no UWorld, no Chaos, no tick. The 442-block
 * router solve is a few tens of milliseconds. Same footing as the 2D ShedScenarioTest and Shed3DRow.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges files into one translation unit.
 */
namespace RealisticShedScenarioTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/** What `?Scenario=` names on a URL, and what IndexOfName looks up. */
	const TCHAR* const RealisticScenarioName = TEXT("shedrealistic");

	/** The distinct map the row must select and be selected by. A content step for dev, not this test. */
	const TCHAR* const RealisticScenarioMapName = TEXT("Lvl_ShedRealistic");

	/*
	 * THE COMMITTED SIZE OF THE REALISTIC SHED, pinned so the row is proven to lay the REAL-brick shed and not
	 * the 24-piece recognizable toy or the running-bond fallback. BuildRealistic lays 442 pieces / 1131 joints
	 * (the shell's 393 + the gables, the roof and the porch); the count is far above the 200-block cap, which puts the
	 * router in charge of the verdict.
	 */
	constexpr int32 ExpectedPieces = 442;
	constexpr int32 EquilibriumGateBlockCap = 200;

	/* Fixed points the builder's canonical geometry places inside each Timber lintel — the same probe points
	 * the RealisticBrickShed builder test reads the openings back at. */
	const FVector DoorLintelPt(90.0, 5.0, 93.0);
	const FVector WindowLintelPt(5.0, 75.0, 93.0);

	/** A hair above the eaves (course 15 top, Z 119) — a gable/roof piece sits above this, the shell does not. */
	constexpr double GableFloorZCm = 119.5;

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
			const double Z = Layout.Boxes[P].CentreCm.Z;
			if (Z > GableFloorZCm && Z > BestZ)
			{
				BestZ = Z;
				Best = P;
			}
		}
		return Best;
	}
}

/**
 * THE REALISTIC-BRICK SHED ROW IS IN THE CATALOGUE, IT LAYS THE 442-PIECE REAL-BRICK SHED THROUGH THE BUILD
 * PATH FLAGGED 3D WITH NO CUT, AND IT STANDS THROUGH THE PRODUCTION WORLD PATH — the whole verdict decided by
 * the world break authority (the router, above the block cap), reached by joining a level.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRealisticShedScenarioCatalogueTest,
	"DestructionGame.World.Scenarios.ShedRealisticRow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRealisticShedScenarioCatalogueTest::RunTest(const FString& Parameters)
{
	using namespace RealisticShedScenarioTestSupport;
	using namespace DestructionProfiles;
	using namespace DestructionLayout;
	using namespace DestructionScenarios;

	/* ================================================================================
	 * ARM 0 — THE ROW EXISTS AND NAMES THE REALISTIC-BRICK SHED. This is the RED: no `shedrealistic` row is in
	 * the catalogue yet, so IndexOfName returns INDEX_NONE and the shed is not joinable.
	 * ================================================================================ */

	const int32 Index = IndexOfName(FName(RealisticScenarioName));

	if (!Catalogue().IsValidIndex(Index))
	{
		AddError(FString::Printf(
			TEXT("the catalogue must carry a row named '%s' — the realistic-brick shed is not joinable until it "
				"does; IndexOfName returned %d against %d row(s). This is the expected RED: dev-expert adds the "
				"FScenario row (MapName Lvl_ShedRealistic, LayStructure -> DestructionShed3D::BuildRealistic, "
				"Framing ThreeQuarter, no cut) and duplicates the map via New-ScenarioMap.ps1."),
			RealisticScenarioName, Index, Catalogue().Num()));

		return false;
	}

	const FScenario& Scenario = Catalogue()[Index];

	/* --- the row must carry its own distinct map, or it is joinable only by ?Scenario= and collides with the
	 * other sheds in both content guards. A distinct Lvl_ShedRealistic is mandatory. --- */
	TestEqual(
		TEXT("the realistic shed row must name its own map 'Lvl_ShedRealistic' — it cannot ride another level"),
		FString(Scenario.MapName ? Scenario.MapName : TEXT("")), FString(RealisticScenarioMapName));

	/* --- the row must lay its structure through a LayStructure producer, so Build routes the realistic builder
	 * rather than the running-bond fallback. --- */
	TestTrue(
		TEXT("the realistic shed row must carry a LayStructure producer (it is not a running-bond wall)"),
		static_cast<bool>(Scenario.LayStructure));

	/* --- and it is framed from a three-quarter angle, so the closed box's depth and its gable roof are both
	 * visible rather than foreshortened to a flat front face. --- */
	TestEqual(
		TEXT("the realistic shed row is framed from a three-quarter angle (a closed box, not a flat wall)"),
		static_cast<int32>(Scenario.Framing), static_cast<int32>(EScenarioFraming::ThreeQuarter));

	/* ================================================================================
	 * ARM 0 (cont.) — DestructionScenarios::Build lays the 442-piece realistic shed, the structure is FLAGGED
	 * 3D (the flag survives the Build/AdoptLayout path), it is multi-material, and it names NO cut in this slice.
	 * ================================================================================ */

	FBrickLayout Layout;
	TArray<int32> Cut;

	const bool bBuilt = Build(Scenario, Layout, Cut);

	TestTrue(
		TEXT("the realistic shed row must build — a row that cannot be laid is a level that cannot be joined"),
		bBuilt);

	if (!bBuilt)
	{
		return false;
	}

	AddInfo(FString::Printf(TEXT("BUILD: %d pieces, %d joints, %d ClayBrick, %d Timber; cut names %d piece(s)."),
		Layout.Structure.NumPieces(), Layout.Structure.NumConnections(),
		CountMaterial(Layout.Structure, &ClayBrick), CountMaterial(Layout.Structure, &Timber), Cut.Num()));

	/* THE 3D FLAG MUST SURVIVE THE WORLD BUILD PATH — else the world bridge would pose the box in 2D and lose
	 * the out-of-plane corners. Asserted on the built Layout.Structure, not on the builder. */
	TestTrue(
		TEXT("the built structure must be flagged 3D — the closed box's corners face out of the X-Z plane"),
		Layout.Structure.IsThreeDimensional());

	/* IT IS THE REAL-BRICK SHED, NOT THE 24-PIECE RECOGNIZABLE TOY NOR THE RUNNING-BOND FALLBACK. 442 pieces
	 * pins the exact committed realistic build; being above the 200-block cap is what makes the router — not
	 * the LP — the break authority below. */
	TestEqual(TEXT("442 pieces — the committed real-brick shell + gables + roof + porch (not the 24-piece toy)"),
		Layout.Structure.NumPieces(), ExpectedPieces);
	TestEqual(TEXT("one box per piece, or AdoptLayout refuses the layout"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());
	TestTrue(TEXT("the shed is multi-material — clay bricks and timber boards"),
		CountMaterial(Layout.Structure, &ClayBrick) > 0 && CountMaterial(Layout.Structure, &Timber) > 0);
	TestTrue(TEXT("the shed knows where every piece and joint is (else every moment is silently zero)"),
		Layout.Structure.HasCompleteGeometry());

	/* NO CUT IN THIS SLICE. The realistic shed is a joinable, standing level; the collapse cut that pulls a
	 * support is a later slice. A cut named here would be applied by the level and change the outcome. */
	TestEqual(
		TEXT("the realistic shed row names NO cut in this slice — it builds and stands, nothing is pulled"),
		Cut.Num(), 0);

	/* ================================================================================
	 * ARM 1 — THE ASSEMBLED REALISTIC SHED STANDS THROUGH THE PRODUCTION WORLD PATH. At 442 blocks it is above
	 * the 200-block equilibrium-gate cap, so SolveAndBreak's authority is the ROUTER (the per-joint capacity
	 * sweep) — the same path the flagship ~1200-block wall uses; the 3D LP is bypassed. A standing shed strands
	 * nothing and drops nothing; the lintels and the ridge read Supported. Support state only, never displacement.
	 * ================================================================================ */
	{
		FBrickLayout Assembled;
		TArray<int32> AssembledCut;

		if (!Build(Scenario, Assembled, AssembledCut))
		{
			AddError(TEXT("ARM 1: the realistic shed row must build a fresh copy"));
			return false;
		}

		TestTrue(*FString::Printf(TEXT("ARM 1: %d blocks is above the %d-block cap, so the ROUTER is the world "
			"break authority (the 3D LP is bypassed)"), Assembled.Structure.NumPieces(), EquilibriumGateBlockCap),
			Assembled.Structure.NumPieces() > EquilibriumGateBlockCap);

		/* THE PRODUCTION WORLD DOOR — the same call the game makes. Above the cap its authority is the router. */
		const int32 Passes = Assembled.Structure.SolveAndBreak();
		const int32 Stranded = StrandedCount(Assembled.Structure);

		AddInfo(FString::Printf(TEXT("ARM 1: PRODUCTION ran %d pass(es); %d stranded."), Passes, Stranded));

		TestEqual(TEXT("ARM 1: nothing may be Stranded — the verdict must be about the shed, not the solver declining"),
			Stranded, 0);

		const int32 DoorLintel = PieceContaining(Assembled, DoorLintelPt);
		const int32 WindowLintel = PieceContaining(Assembled, WindowLintelPt);
		const int32 Ridge = RidgePiece(Assembled);

		TestTrue(TEXT("ARM 1: a Timber door lintel exists over the doorway"),
			DoorLintel != INDEX_NONE && Assembled.Structure.GetPiece(DoorLintel).Material == &Timber);
		TestTrue(TEXT("ARM 1: a Timber window lintel exists over the window"),
			WindowLintel != INDEX_NONE && Assembled.Structure.GetPiece(WindowLintel).Material == &Timber);
		TestTrue(TEXT("ARM 1: a Timber ridge exists at the top of the gable roof"),
			Ridge != INDEX_NONE);

		if (DoorLintel != INDEX_NONE)
		{
			TestTrue(TEXT("ARM 1: the door lintel is held up as laid, through the world path (carried by its piers)"),
				IsStanding(Assembled.Structure.GetPieceSupport(DoorLintel)));
		}
		if (WindowLintel != INDEX_NONE)
		{
			TestTrue(TEXT("ARM 1: the window lintel is held up as laid, through the world path (carried by its jambs)"),
				IsStanding(Assembled.Structure.GetPieceSupport(WindowLintel)));
		}
		if (Ridge != INDEX_NONE)
		{
			TestTrue(TEXT("ARM 1: the ridge is held up as laid, through the world path (carried down to the gables)"),
				IsStanding(Assembled.Structure.GetPieceSupport(Ridge)));
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
