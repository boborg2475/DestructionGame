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
 * The realistic-brick shed as a playable scenario (realistic-shed slice 5). The `shedrealistic`
 * row (Lvl_ShedRealistic, LayStructure BuildRealistic) lays the 442-piece shed: single-wythe
 * ClayBrick walls, Timber lintels, stepped gables, a Timber roof and a porch. Flagged 3D; above the
 * 200-block cap, so the router decides. The row's nine-brick back-eaves cut is resolved but not
 * applied here (see ShedRealisticCollapseRow).
 *
 * Needs its own map because IndexOfMapName returns the first matching row. Stands means
 * SolveAndBreak leaves nothing Stranded and the lintels and ridge Supported; no displacement.
 * World-free. Named namespace for unity builds.
 */
namespace RealisticShedScenarioTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/** The `?Scenario=` name. */
	const TCHAR* const RealisticScenarioName = TEXT("shedrealistic");

	const TCHAR* const RealisticScenarioMapName = TEXT("Lvl_ShedRealistic");

	// 442 pieces (not the 24-piece toy or the fallback), above the 200-block cap.
	constexpr int32 ExpectedPieces = 442;
	constexpr int32 EquilibriumGateBlockCap = 200;

	// Points inside each Timber lintel, as in the RealisticBrickShed builder test.
	const FVector DoorLintelPt(90.0, 5.0, 93.0);
	const FVector WindowLintelPt(5.0, 75.0, 93.0);

	/** Just above the eaves (course 15 top, Z 119); only gable and roof pieces sit above it. */
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

	/** The highest Timber roof piece (the ridge), or INDEX_NONE. */
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

/** The shed row exists, builds the 442-piece 3D shed, and stands through SolveAndBreak. See the file header. */
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

	// The row exists and names the realistic shed.

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

	// Its own map, or it collides with the other sheds in the content guards.
	TestEqual(
		TEXT("the realistic shed row must name its own map 'Lvl_ShedRealistic' — it cannot ride another level"),
		FString(Scenario.MapName ? Scenario.MapName : TEXT("")), FString(RealisticScenarioMapName));

	// A LayStructure producer, not the running-bond fallback.
	TestTrue(
		TEXT("the realistic shed row must carry a LayStructure producer (it is not a running-bond wall)"),
		static_cast<bool>(Scenario.LayStructure));

	// Three-quarter framing, so the box's depth and roof are visible.
	TestEqual(
		TEXT("the realistic shed row is framed from a three-quarter angle (a closed box, not a flat wall)"),
		static_cast<int32>(Scenario.Framing), static_cast<int32>(EScenarioFraming::ThreeQuarter));

	// Build lays the 442-piece multi-material shed, flagged 3D.

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

	// The 3D flag must survive the build path, or the bridge poses the box in 2D.
	TestTrue(
		TEXT("the built structure must be flagged 3D — the closed box's corners face out of the X-Z plane"),
		Layout.Structure.IsThreeDimensional());

	TestEqual(TEXT("442 pieces — the committed real-brick shell + gables + roof + porch (not the 24-piece toy)"),
		Layout.Structure.NumPieces(), ExpectedPieces);
	TestEqual(TEXT("one box per piece, or AdoptLayout refuses the layout"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());
	TestTrue(TEXT("the shed is multi-material — clay bricks and timber boards"),
		CountMaterial(Layout.Structure, &ClayBrick) > 0 && CountMaterial(Layout.Structure, &Timber) > 0);
	TestTrue(TEXT("the shed knows where every piece and joint is (else every moment is silently zero)"),
		Layout.Structure.HasCompleteGeometry());

	// The nine-brick back-eaves cut is resolved but not applied.
	TestEqual(
		TEXT("the realistic shed row names the nine-brick back-eaves cut — resolved but not applied here"),
		Cut.Num(), 9);

	// The assembled shed stands: nothing Stranded, lintels and ridge Supported.
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

		// The same call the game makes.
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
