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
 * The realistic-brick shed as a playable scenario — the point where the true-masonry shed
 * DestructionShed3D::BuildRealistic lays (Acceptance.Shed.ThreeD.RealisticBrickShedShellStandsAsBuilt
 * and .RealisticBrickShedGablesAndRoofStandAsBuilt) stops being reachable only from a builder
 * unit test and becomes a level a human can join and watch hold — scenario slice (5) of the
 * realistic-shed goal.
 *
 * The catalogue carries a `shedrealistic` row (MapName `Lvl_ShedRealistic`, LayStructure
 * DestructionShed3D::BuildRealistic), so DestructionScenarios::Build lays the 442-piece
 * real-brick shed — four single-wythe running-bond ClayBrick walls of true
 * 21.5 x 10.25 x 6.5 cm bricks on 1 cm mortar joints closing a box, a door and a window each
 * under a Timber-board lintel, stepped brick gables rising to a ridge, a Timber gable roof,
 * and a porch of two grounded Timber posts carrying a cantilevered overhang over the door —
 * flagged 3D (IsThreeDimensional), and standing through the production world path
 * (FStructure::SolveAndBreak, whose above-cap authority is the router's per-joint capacity
 * sweep — 442 blocks is far above the 200-block equilibrium-gate cap, so the 3D LP is
 * bypassed). The row also names a collapse cut, the nine bricks of the back wall's eaves
 * course; this test only proves the cut resolves (nine pieces) without applying it, so the
 * shed still stands as built — applying it and watching the back gable end come down is
 * ShedRealisticCollapseRow.
 *
 * This is the catalogue/world-path counterpart of the RealisticBrickShed builder tests
 * (which prove BuildRealistic called directly): here the proof is that the catalogue row
 * exists, names the realistic builder, that Build routes through it, that the 3D flag and
 * per-piece materials survive into Layout.Structure (the AdoptLayout promise), and that the
 * shed travels the same production door the game uses — reached by joining a level. The row
 * needs its own map because a scenario is selected back from its map by IndexOfMapName,
 * which returns the first row spelling that name, so a distinct `Lvl_ShedRealistic` keeps
 * this row from colliding with the other sheds in the Content.ScenarioMapsExist and
 * Content.ScenarioMapsAreDistinctAssets sweeps (the .umap itself is a content step for dev,
 * New-ScenarioMap.ps1).
 *
 * Assertions are on mechanism and outcome, never displacement: stands is production's
 * SolveAndBreak leaving Stranded == 0 and the spanning pieces (the two lintels, the ridge)
 * reading Supported — a severed piece can rest exactly in place. No collapse arm here since
 * the cut is not applied (that is ShedRealisticCollapseRow's job), and no LP/oracle arm
 * (unlike the recognizable Shed3DRow) since above the 200-block cap the router, not the LP,
 * is the break authority.
 *
 * Needs a ticking world: no — the catalogue is world-free, BuildRealistic is arithmetic over
 * boxes and a graph, and SolveAndBreak is a synchronous settle over that graph. The 442-block
 * router solve is a few tens of milliseconds.
 *
 * Named namespace, not anonymous: a unity build merges files into one translation unit.
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
	 * The committed size of the realistic shed, pinned so the row is proven to lay the
	 * real-brick shed rather than the 24-piece recognizable toy or the running-bond
	 * fallback: 442 pieces / 1131 joints (the shell's 393 plus gables, roof and porch),
	 * far above the 200-block cap that puts the router in charge of the verdict.
	 */
	constexpr int32 ExpectedPieces = 442;
	constexpr int32 EquilibriumGateBlockCap = 200;

	/* Fixed points the builder's canonical geometry places inside each Timber lintel — the same
	 * probe points the RealisticBrickShed builder test reads the openings back at. */
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
 * The realistic-brick shed row is in the catalogue, lays the 442-piece real-brick shed through
 * the build path flagged 3D, and stands through the production world path — the whole verdict
 * decided by the world break authority (the router, above the block cap), reached by joining a
 * level.
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

	/* ARM 0 — the row exists and names the realistic-brick shed. */

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

	/* Its own distinct map, or it is joinable only by ?Scenario= and collides with the other
	 * sheds in both content guards. */
	TestEqual(
		TEXT("the realistic shed row must name its own map 'Lvl_ShedRealistic' — it cannot ride another level"),
		FString(Scenario.MapName ? Scenario.MapName : TEXT("")), FString(RealisticScenarioMapName));

	/* A LayStructure producer, so Build routes the realistic builder rather than the
	 * running-bond fallback. */
	TestTrue(
		TEXT("the realistic shed row must carry a LayStructure producer (it is not a running-bond wall)"),
		static_cast<bool>(Scenario.LayStructure));

	/* Framed from a three-quarter angle, so the box's depth and gable roof are visible rather
	 * than foreshortened to a flat front face. */
	TestEqual(
		TEXT("the realistic shed row is framed from a three-quarter angle (a closed box, not a flat wall)"),
		static_cast<int32>(Scenario.Framing), static_cast<int32>(EScenarioFraming::ThreeQuarter));

	/* ARM 0 (cont.) — Build lays the 442-piece shed, flagged 3D (the flag survives the
	 * Build/AdoptLayout path), and multi-material. */

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

	/* The 3D flag must survive the build path, or the world bridge would pose the box in 2D and
	 * lose the out-of-plane corners. Asserted on the built Layout.Structure, not the builder. */
	TestTrue(
		TEXT("the built structure must be flagged 3D — the closed box's corners face out of the X-Z plane"),
		Layout.Structure.IsThreeDimensional());

	/* 442 pins the real-brick shed, not the 24-piece toy or the running-bond fallback, and
	 * being above the 200-block cap makes the router, not the LP, the break authority below. */
	TestEqual(TEXT("442 pieces — the committed real-brick shell + gables + roof + porch (not the 24-piece toy)"),
		Layout.Structure.NumPieces(), ExpectedPieces);
	TestEqual(TEXT("one box per piece, or AdoptLayout refuses the layout"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());
	TestTrue(TEXT("the shed is multi-material — clay bricks and timber boards"),
		CountMaterial(Layout.Structure, &ClayBrick) > 0 && CountMaterial(Layout.Structure, &Timber) > 0);
	TestTrue(TEXT("the shed knows where every piece and joint is (else every moment is silently zero)"),
		Layout.Structure.HasCompleteGeometry());

	/* The row names the collapse cut — the nine bricks of the back wall's eaves course, the
	 * footing under the back gable. Build resolves it but leaves it to the level to remove, so
	 * this test proves it named (nine pieces) without applying it; the collapse itself is
	 * ShedRealisticCollapseRow. */
	TestEqual(
		TEXT("the realistic shed row names the nine-brick back-eaves cut — resolved but not applied here"),
		Cut.Num(), 9);

	/*
	 * ARM 1 — the assembled shed stands through the production world path. At 442 blocks it is
	 * above the cap, so SolveAndBreak's authority is the router (the flagship ~1200-block
	 * wall's path); the 3D LP is bypassed. A standing shed strands nothing and drops nothing;
	 * the lintels and ridge read Supported. Support state only, never displacement.
	 */
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

		/* The production world door — the same call the game makes. Above the cap, its authority is the router. */
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
