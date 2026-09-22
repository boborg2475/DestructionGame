// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/DestructionShed3D.h"
#include "Core/Layout.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#include "World/DestructionScenarios.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Realistic shed slice 4, the collapse: the `shedrealistic` row's cut removes the back wall's eaves
 * course (course 15), and the whole back gable above it loses the earth while the rest stands.
 *
 * The back wall faces the ThreeQuarter camera (Az 40, El 30), so its gable is the most visible
 * thing in frame. A cut in the wall body would just arch over; the gable (courses 16-19, 20
 * bricks) has no lateral abutment, so removing its footing drops it. At 442 blocks the shed is
 * above the LP's 200-block cap, so the router is the break authority.
 *
 * Course 15 is odd: a half bat, seven full bricks and a closing half bat at Y 128.875,
 * Z = 15 * 7.5 + 3.25 = 115.75. Measured through the router, 24 pieces lose the earth (the gable
 * plus four purlins) and nothing is Stranded.
 *
 * Arm 1 (the red) checks the row names these nine bricks; arm 2 applies the row's cut, or pulls
 * the bricks itself if the row is not yet rewired. Assertions are on support state, never
 * displacement (DESIGN.md §4). No world: catalogue, layout and router are all world-free.
 * Named namespace: a unity build merges files.
 */
namespace RealisticShedCollapseTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	const TCHAR* const RealisticScenarioName = TEXT("shedrealistic");

	/*
	 * Box centres of the nine back-eaves bricks (course 15). Cut centres resolve with a 1e-6 match,
	 * so these are the same literals as the row's CutCentresCm.
	 */
	TArray<FVector> BackEavesCutCentres()
	{
		const double YCm = 128.875;
		const double ZCm = 115.75;
		const double XsCm[] = { 5.125, 22.0, 44.5, 67.0, 89.5, 112.0, 134.5, 157.0, 173.875 };

		TArray<FVector> Centres;
		for (const double XCm : XsCm)
		{
			Centres.Add(FVector(XCm, YCm, ZCm));
		}
		return Centres;
	}

	// Probe points inside pieces the collapse must drop (the back gable) and pieces it must spare.
	const FVector BackGableBasePt(100.75, 128.875, 123.25);   // back gable course 16 — bottom step
	const FVector BackGableApexPt(100.75, 128.875, 145.75);   // back gable course 19 — apex step

	const FVector DoorLintelPt(90.0, 5.0, 93.0);       // the Timber door lintel (front wall, far side)
	const FVector WindowLintelPt(5.0, 75.0, 93.0);     // the Timber window lintel (left wall)
	const FVector FrontGableApexPt(100.75, 5.125, 145.75);  // the FRONT gable apex — independent of the back
	const FVector BackWallBodyPt(100.75, 128.875, 63.25);   // back wall course 8 (even) — below the cut, still footed
	const FVector PostLPt(55.0, -20.0, 52.0);          // the left porch post
	const FVector PorchOverhangPt(90.0, -36.0, 107.5); // the porch overhang board — CoM Y=-36, outboard of the posts

	int32 PieceContaining(const FBrickLayout& L, const FVector& Pt)
	{
		const FStructure& S = L.Structure;
		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (S.IsPieceRemoved(P) || !L.Boxes.IsValidIndex(P))
			{
				continue;
			}
			const FVector Lo = L.Boxes[P].CentreCm - L.Boxes[P].ExtentCm;
			const FVector Hi = L.Boxes[P].CentreCm + L.Boxes[P].ExtentCm;
			if (Pt.X >= Lo.X && Pt.X <= Hi.X && Pt.Y >= Lo.Y && Pt.Y <= Hi.Y && Pt.Z >= Lo.Z && Pt.Z <= Hi.Z)
			{
				return P;
			}
		}
		return INDEX_NONE;
	}

	int32 StrandedCount(const FStructure& S)
	{
		int32 N = 0;
		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (!S.IsPieceRemoved(P) && S.GetPieceSupport(P) == EPieceSupport::Stranded)
			{
				++N;
			}
		}
		return N;
	}

	int32 LostEarthCount(const FStructure& S)
	{
		int32 N = 0;
		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (S.IsPieceRemoved(P))
			{
				continue;
			}
			const EPieceSupport Sup = S.GetPieceSupport(P);
			if (Sup != EPieceSupport::Grounded && Sup != EPieceSupport::Supported)
			{
				++N;
			}
		}
		return N;
	}

	bool IsStanding(EPieceSupport Support)
	{
		return Support == EPieceSupport::Grounded || Support == EPieceSupport::Supported;
	}

	bool HasLostTheEarth(const FStructure& S, int32 Piece)
	{
		return Piece != INDEX_NONE && !S.IsPieceRemoved(Piece) && !IsStanding(S.GetPieceSupport(Piece));
	}

	/** The Timber roof member with the greatest Z centre — the ridge — or INDEX_NONE. */
	int32 RidgePiece(const FBrickLayout& L)
	{
		const FStructure& S = L.Structure;
		int32 Best = INDEX_NONE;
		double BestZ = -DBL_MAX;
		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (S.IsPieceRemoved(P) || S.GetPiece(P).Material != &Timber || !L.Boxes.IsValidIndex(P))
			{
				continue;
			}
			const double Z = L.Boxes[P].CentreCm.Z;
			if (Z > 119.5 && Z > BestZ)
			{
				BestZ = Z;
				Best = P;
			}
		}
		return Best;
	}
}

/** Pulling the back eaves course drops the back gable while the rest of the shed stands. No world. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRealisticShedCollapseRowTest,
	"DestructionGame.World.Scenarios.ShedRealisticCollapseRow",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRealisticShedCollapseRowTest::RunTest(const FString& Parameters)
{
	using namespace RealisticShedCollapseTestSupport;
	using namespace DestructionProfiles;
	using namespace DestructionLayout;
	using namespace DestructionScenarios;

	// Arm 0: the row exists and builds (owned by ShedRealisticRow; re-checked here).

	const int32 Index = IndexOfName(FName(RealisticScenarioName));

	if (!Catalogue().IsValidIndex(Index))
	{
		AddError(FString::Printf(
			TEXT("the catalogue must carry a '%s' row — established by ShedRealisticRow. IndexOfName=%d."),
			RealisticScenarioName, Index));
		return false;
	}

	const FScenario& Scenario = Catalogue()[Index];

	FBrickLayout Layout;
	TArray<int32> Cut;
	const bool bBuilt = Build(Scenario, Layout, Cut);

	TestTrue(TEXT("ARM 0: the realistic shed row must build"), bBuilt);
	if (!bBuilt)
	{
		return false;
	}

	AddInfo(FString::Printf(TEXT("BUILD: %d pieces, %d joints; cut names %d piece(s)."),
		Layout.Structure.NumPieces(), Layout.Structure.NumConnections(), Cut.Num()));

	TestTrue(TEXT("ARM 0: the built shed is above the 200-block cap, so the ROUTER is the break authority"),
		Layout.Structure.NumPieces() > 200);

	// Independent geometry check: a brick exists at each hand-derived back-eaves centre.
	const TArray<FVector> EavesCentres = BackEavesCutCentres();
	TArray<int32> EavesPieces;
	for (const FVector& CentreCm : EavesCentres)
	{
		const int32 P = PieceContaining(Layout, CentreCm);
		TestTrue(*FString::Printf(TEXT("ARM 0: a ClayBrick sits at back-eaves centre (%.3f, %.3f, %.3f)"),
			CentreCm.X, CentreCm.Y, CentreCm.Z),
			P != INDEX_NONE && Layout.Structure.GetPiece(P).Material == &ClayBrick);
		if (P != INDEX_NONE)
		{
			EavesPieces.Add(P);
		}
	}

	// Arm 1: the row's cut names exactly the nine back-eaves bricks.

	TestEqual(
		*FString::Printf(TEXT("RED: the '%s' row must NAME the nine back-eaves bricks — it names %d today"),
			RealisticScenarioName, Cut.Num()),
		Cut.Num(), EavesCentres.Num());

	int32 NamedEaves = 0;
	for (const int32 P : EavesPieces)
	{
		if (Cut.Contains(P))
		{
			++NamedEaves;
		}
	}
	TestEqual(
		TEXT("RED: the cut must name every one of the nine back-eaves bricks"),
		NamedEaves, EavesPieces.Num());

	/*
	 * Arm 2: the collapse. Apply the row's cut if it is the back-eaves cut, else pull the bricks
	 * directly, so the physics is exercised independently of the cut data.
	 */
	{
		FBrickLayout Pulled;
		TArray<int32> PulledCut;
		if (!Build(Scenario, Pulled, PulledCut))
		{
			AddError(TEXT("ARM 2: the realistic shed row must build a fresh copy"));
			return false;
		}

		const int32 BackGableBase = PieceContaining(Pulled, BackGableBasePt);
		const int32 BackGableApex = PieceContaining(Pulled, BackGableApexPt);
		const int32 DoorLintel = PieceContaining(Pulled, DoorLintelPt);
		const int32 WindowLintel = PieceContaining(Pulled, WindowLintelPt);
		const int32 FrontGableApex = PieceContaining(Pulled, FrontGableApexPt);
		const int32 BackWallBody = PieceContaining(Pulled, BackWallBodyPt);
		const int32 PostL = PieceContaining(Pulled, PostLPt);
		const int32 PorchOverhang = PieceContaining(Pulled, PorchOverhangPt);
		const int32 Ridge = RidgePiece(Pulled);

		TestTrue(TEXT("ARM 2: the back gable base and apex, and the spared probe pieces, must all be found"),
			BackGableBase != INDEX_NONE && BackGableApex != INDEX_NONE && DoorLintel != INDEX_NONE
				&& WindowLintel != INDEX_NONE && FrontGableApex != INDEX_NONE && BackWallBody != INDEX_NONE
				&& PostL != INDEX_NONE && PorchOverhang != INDEX_NONE);

		TestTrue(TEXT("ARM 2: a Timber ridge board exists at the top of the roof"), Ridge != INDEX_NONE);

		int32 Applied = 0;
		bool bCutIsBackEaves = PulledCut.Num() == EavesCentres.Num();
		if (bCutIsBackEaves)
		{
			for (const FVector& CentreCm : EavesCentres)
			{
				const int32 P = PieceContaining(Pulled, CentreCm);
				bCutIsBackEaves = bCutIsBackEaves && P != INDEX_NONE && PulledCut.Contains(P);
			}
		}
		if (bCutIsBackEaves)
		{
			for (const int32 Piece : PulledCut)
			{
				if (Pulled.Structure.RemovePiece(Piece))
				{
					++Applied;
				}
			}
		}

		// Fallback for an un-rewired row: the same removal the cut would perform.
		if (Applied == 0)
		{
			for (const FVector& CentreCm : EavesCentres)
			{
				const int32 P = PieceContaining(Pulled, CentreCm);
				if (P != INDEX_NONE && Pulled.Structure.RemovePiece(P))
				{
					++Applied;
				}
			}
			AddInfo(FString::Printf(
				TEXT("ARM 2: row did not name the back-eaves cut yet — pulled %d bricks via fallback."), Applied));
		}

		const int32 Passes = Pulled.Structure.SolveAndBreak();
		const int32 Stranded = StrandedCount(Pulled.Structure);
		const int32 LostEarth = LostEarthCount(Pulled.Structure);

		AddInfo(FString::Printf(
			TEXT("ARM 2: removed %d brick(s); production ran %d pass(es); %d stranded; %d lost the earth."),
			Applied, Passes, Stranded, LostEarth));
		AddInfo(FString::Printf(
			TEXT("ARM 2: support — backGableBase %d, backGableApex %d, ridge %d, doorLintel %d, "
				"windowLintel %d, frontGableApex %d, backWallBody %d, postL %d."),
			static_cast<int32>(Pulled.Structure.GetPieceSupport(BackGableBase)),
			static_cast<int32>(Pulled.Structure.GetPieceSupport(BackGableApex)),
			Ridge == INDEX_NONE ? -1 : static_cast<int32>(Pulled.Structure.GetPieceSupport(Ridge)),
			static_cast<int32>(Pulled.Structure.GetPieceSupport(DoorLintel)),
			static_cast<int32>(Pulled.Structure.GetPieceSupport(WindowLintel)),
			static_cast<int32>(Pulled.Structure.GetPieceSupport(FrontGableApex)),
			static_cast<int32>(Pulled.Structure.GetPieceSupport(BackWallBody)),
			static_cast<int32>(Pulled.Structure.GetPieceSupport(PostL))));

		// Nothing Stranded, so the fall is not a routing limitation (DESIGN.md §4).
		TestEqual(TEXT("ARM 2: nothing may be Stranded — the fall must be about the shed, not the router declining"),
			Stranded, 0);

		// The back gable drops as a chunk of at least 15 pieces.
		TestTrue(
			*FString::Printf(TEXT("ARM 2: the back gable base (piece %d, support %d) must lose the earth"),
				BackGableBase, static_cast<int32>(Pulled.Structure.GetPieceSupport(BackGableBase))),
			HasLostTheEarth(Pulled.Structure, BackGableBase));
		TestTrue(
			*FString::Printf(TEXT("ARM 2: the back gable apex (piece %d, support %d) must lose the earth"),
				BackGableApex, static_cast<int32>(Pulled.Structure.GetPieceSupport(BackGableApex))),
			HasLostTheEarth(Pulled.Structure, BackGableApex));

		TestTrue(
			*FString::Printf(TEXT("ARM 2: a big section comes down — >= 15 pieces lose the earth (got %d)"), LostEarth),
			LostEarth >= 15);

		/*
		 * Review item 3 (red): the ridge must fall too. With the back gable gone it rests only on
		 * compression-only front-gable seats at Y ~ 5, while its CoM is at Y ~ 67, so it overturns.
		 * Production stands it because with two bearings SolveLoads zeroes the overturning moment
		 * (bLoadPathIsDeterminate false for N >= 2). Fix: a CoM-in-support-union gate with a tension
		 * clause.
		 */
		TestTrue(
			*FString::Printf(TEXT("RED: the ridge (piece %d, support %d) must lose the earth — its CoM is behind "
				"its surviving front-gable bearing line"),
				Ridge, Ridge == INDEX_NONE ? -1 : static_cast<int32>(Pulled.Structure.GetPieceSupport(Ridge))),
			HasLostTheEarth(Pulled.Structure, Ridge));

		/*
		 * Anti-regression: the porch overhang must stand. Its CoM (Y = -36) is outboard of its posts
		 * (Y = -20), so a union-only gate would fell it, but its Screw cleat is a tension tie that
		 * holds it. The tension clause is what spares it.
		 */
		TestTrue(
			*FString::Printf(TEXT("ANTI-REGRESSION: the porch overhang (piece %d, support %d) must keep the earth — "
				"its cleat is a tension-capable Screw tie"),
				PorchOverhang, static_cast<int32>(Pulled.Structure.GetPieceSupport(PorchOverhang))),
			IsStanding(Pulled.Structure.GetPieceSupport(PorchOverhang)));

		// The rest of the shed, independent of the back eaves course, keeps the earth.
		TestTrue(TEXT("ARM 2: the door lintel on the front wall keeps the earth"),
			IsStanding(Pulled.Structure.GetPieceSupport(DoorLintel)));
		TestTrue(TEXT("ARM 2: the window lintel on the left wall keeps the earth"),
			IsStanding(Pulled.Structure.GetPieceSupport(WindowLintel)));
		TestTrue(TEXT("ARM 2: the FRONT gable apex keeps the earth (independent of the back gable)"),
			IsStanding(Pulled.Structure.GetPieceSupport(FrontGableApex)));
		TestTrue(TEXT("ARM 2: a back-wall brick below the cut keeps the earth"),
			IsStanding(Pulled.Structure.GetPieceSupport(BackWallBody)));
		TestTrue(TEXT("ARM 2: the left porch post keeps the earth"),
			IsStanding(Pulled.Structure.GetPieceSupport(PostL)));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
