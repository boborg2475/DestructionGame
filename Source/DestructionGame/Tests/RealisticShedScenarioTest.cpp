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
 * The realistic-brick shed as a playable scenario (slice (5) of the realistic-shed goal): where
 * DestructionShed3D::BuildRealistic stops being reachable only from a builder unit test and becomes
 * a level a human can join and watch hold.
 *
 * The catalogue's `shedrealistic` row (MapName Lvl_ShedRealistic, LayStructure BuildRealistic) lays
 * the 442-piece real-brick shed: four single-wythe running-bond ClayBrick walls of true
 * 21.5 x 10.25 x 6.5 cm bricks on 1 cm joints, a door and window each under a Timber lintel, stepped
 * gables to a ridge, a Timber gable roof, and a porch of two posts over the door. Flagged 3D, and
 * standing through SolveAndBreak (442 blocks is above the 200-block cap, so the router, not the 3D
 * LP, is the authority). The row names a nine-brick back-eaves collapse cut; this test proves it
 * resolves without applying it (the collapse is ShedRealisticCollapseRow's).
 *
 * The catalogue/world-path counterpart of the RealisticBrickShed builder tests: it proves the row
 * exists, names the realistic builder, that Build routes through it, that the 3D flag and materials
 * survive into Layout.Structure, and that the shed travels the same production door. The row needs
 * its own map because IndexOfMapName returns the first row spelling a name, so a distinct
 * Lvl_ShedRealistic keeps it from colliding with the other sheds in the content sweeps.
 *
 * Assertions are on mechanism and outcome, never displacement: stands is SolveAndBreak leaving
 * Stranded == 0 and the spanning pieces (two lintels, ridge) reading Supported. No collapse arm (the
 * cut is not applied) and no LP arm (above the cap the router is the break authority).
 *
 * No ticking world. Named namespace because unity builds merge translation units.
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
	 * The committed shed size, pinned so the row is proven to lay the real-brick shed and not the
	 * 24-piece toy or the running-bond fallback: 442 pieces, above the 200-block cap that puts the
	 * router in charge of the verdict.
	 */
	constexpr int32 ExpectedPieces = 442;
	constexpr int32 EquilibriumGateBlockCap = 200;

	/* Fixed points inside each Timber lintel, the same probe points the RealisticBrickShed builder
	 * test reads the openings back at. */
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
 * level. No ticking world (see the file header).
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

	/* Its own distinct map, or it collides with the other sheds in both content guards. */
	TestEqual(
		TEXT("the realistic shed row must name its own map 'Lvl_ShedRealistic' — it cannot ride another level"),
		FString(Scenario.MapName ? Scenario.MapName : TEXT("")), FString(RealisticScenarioMapName));

	/* A LayStructure producer, so Build routes the realistic builder, not the running-bond fallback. */
	TestTrue(
		TEXT("the realistic shed row must carry a LayStructure producer (it is not a running-bond wall)"),
		static_cast<bool>(Scenario.LayStructure));

	/* Framed three-quarter, so the box's depth and gable roof are visible, not a flat front face. */
	TestEqual(
		TEXT("the realistic shed row is framed from a three-quarter angle (a closed box, not a flat wall)"),
		static_cast<int32>(Scenario.Framing), static_cast<int32>(EScenarioFraming::ThreeQuarter));

	/* ARM 0 (cont.) — Build lays the 442-piece shed, flagged 3D (surviving Build/AdoptLayout), multi-material. */

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

	/* The 3D flag must survive the build path, or the world bridge poses the box in 2D and loses the
	 * out-of-plane corners. Asserted on the built Layout.Structure, not the builder. */
	TestTrue(
		TEXT("the built structure must be flagged 3D — the closed box's corners face out of the X-Z plane"),
		Layout.Structure.IsThreeDimensional());

	/* 442 pins the real-brick shed (not the 24-piece toy or the fallback), above the 200-block cap so
	 * the router, not the LP, is the break authority below. */
	TestEqual(TEXT("442 pieces — the committed real-brick shell + gables + roof + porch (not the 24-piece toy)"),
		Layout.Structure.NumPieces(), ExpectedPieces);
	TestEqual(TEXT("one box per piece, or AdoptLayout refuses the layout"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());
	TestTrue(TEXT("the shed is multi-material — clay bricks and timber boards"),
		CountMaterial(Layout.Structure, &ClayBrick) > 0 && CountMaterial(Layout.Structure, &Timber) > 0);
	TestTrue(TEXT("the shed knows where every piece and joint is (else every moment is silently zero)"),
		Layout.Structure.HasCompleteGeometry());

	/* The row names the nine-brick back-eaves cut (the footing under the back gable). Build resolves
	 * it but leaves the level to remove it, so this proves it named without applying it; the collapse
	 * is ShedRealisticCollapseRow. */
	TestEqual(
		TEXT("the realistic shed row names the nine-brick back-eaves cut — resolved but not applied here"),
		Cut.Num(), 9);

	/*
	 * ARM 1 — the assembled shed stands through the production world path. Above the cap, SolveAndBreak's
	 * authority is the router (the 3D LP is bypassed). A standing shed strands and drops nothing; the
	 * lintels and ridge read Supported. Support state only, never displacement.
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

		/* The production world door, the same call the game makes; above the cap its authority is the router. */
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
