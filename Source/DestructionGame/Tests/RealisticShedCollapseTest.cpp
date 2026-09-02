// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/DestructionShed3D.h"
#include "Core/Layout.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#include "World/DestructionScenarios.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE REALISTIC-BRICK SHED FALLS BIG AND CAMERA-VISIBLY WHEN YOU TAKE ITS BACK EAVES COURSE OUT — slice 4
 * of the realistic shed, the COLLAPSE arm, driven through the production world path under the ROUTER (the
 * break authority at this scale). This is the world-path counterpart of "watch a big section fall if you
 * take bricks out": the `shedrealistic` row must name a CUT that removes the top (eaves) course of the
 * BACK wall — the footing under the back gable end — so that once the cut is applied the whole back gable
 * (its stepped courses, ~20 bricks) loses the earth, while the rest of the shed keeps it.
 *
 * =====================================================================================
 * WHY THE BACK WALL, AND WHY THE GABLE (the camera + the mechanism)
 * =====================================================================================
 *
 * THE CAMERA. The row is framed ThreeQuarter: ViewpointFor places the camera at
 * Centre + Standoff * (cosEl*sinAz, cosEl*cosAz, sinEl) with Az = 40, El = 30 — direction
 * (0.557, 0.663, 0.500), i.e. in the +X/+Y/+Z octant looking back at the box. It most directly faces the
 * +Y BACK wall (dot 0.663) and the +X right wall (dot 0.557), and looks DOWN onto the roof. The -Y door
 * face — where the old two-brick door-lintel cut lived — points AWAY from the camera, so that collapse
 * barely showed. The back wall is the widest camera-facing face AND a gable end, so its stepped triangular
 * top is the single most visible thing in frame; dropping it reads unmistakably.
 *
 * THE MECHANISM. Bonded masonry DEEP-BEAMS over a mid-span gap and stands (the acceptance walls 11, 12 and
 * 19 all keep their footing with holes cut in them), so cutting a low band out of the wall BODY just arches
 * over and shows nothing. The back GABLE is different: its stepped courses (16..19) bed on the eaves course
 * (course 15) and have NO lateral abutment — they are a free-standing triangle on top of the wall. Remove
 * the eaves course across the back wall and the gable's entire footing is gone at once; with nothing beneath
 * and nothing to arch to, all four stepped courses lose the earth together. A clean, big, visible fall.
 *
 * =====================================================================================
 * WHY THE ROUTER (the authority decision this slice records)
 * =====================================================================================
 *
 * At 442 blocks the shed is far above the equilibrium gate's 200-block cap, so SolveAndBreak's authority is
 * the ROUTER's per-joint capacity sweep — the 3D LP is bypassed (a single LP feasibility solve at 442 runs
 * for many minutes; the router solve is a few milliseconds). The router routes load down bed joints, so a
 * course whose footing is entirely removed and which has no lateral bed path to ground correctly loses the
 * earth. The back gable is exactly that: removing course 15 strands nothing (no knot), it simply falls.
 *
 * =====================================================================================
 * THE CUT, HAND-DERIVED — THE BACK-WALL EAVES COURSE (course 15)
 * =====================================================================================
 *
 * BuildRealistic lays the back wall along X in the Y band [123.75, 134] (Y centre 128.875), RunStart 0,
 * 8 full bricks per even course, NoOpening, 16 courses. Course 15 is odd, so it is a half bat, seven full
 * bricks, and a closing half bat — nine pieces, at X centres 5.125, 22.0, 44.5, 67.0, 89.5, 112.0, 134.5,
 * 157.0, 173.875, all at Z = 15 * 7.5 + 3.25 = 115.75. The back gable rises above it: course 16 (8 bricks),
 * 17 (6), 18 (4) and 19 (2) — 20 bricks in all — each bedding on the course below and the lowest on course
 * 15. Pull course 15 and the gable has no path down; MEASURED through the router, 24 pieces lose the earth
 * as a chunk — the twenty-brick back gable end plus the four roof purlins that bore on its shoulders — and
 * nothing is Stranded (the ridge, spanning to the intact front gable, keeps the earth). The far side of the
 * shed (door head, window lintel, front gable, back-wall body below the cut, porch posts) is untouched.
 *
 * =====================================================================================
 * THE RED, AND WHAT DEV BUILDS
 * =====================================================================================
 *
 * ARM 1 is the RED: before the cut is rewired the `shedrealistic` row names the OLD two-brick door-lintel
 * cut, so Build resolves two pieces that are NOT the nine back-eaves bricks and the cut-naming assertions
 * fail. Dev makes it green by replacing the row's CutCentresCm with the nine back-eaves centres and updating
 * the row's Title/Expectation. THAT ALSO SUPERSEDES the Cut.Num() == 2 pin in World.Scenarios.ShedRealisticRow
 * — dev relaxes it to nine as part of wiring the cut.
 *
 * ARM 2 (the collapse itself) is written to run whether or not the cut is wired: it applies the row's cut if
 * it names the nine back-eaves bricks, else pulls them itself, so the router-correct collapse is exercised now
 * and stays green once the cut lands. Assertions are on mechanism / support-state (HasLostTheEarth,
 * Stranded == 0), never displacement (DESIGN.md §4).
 *
 * NEEDS A TICKING WORLD: NO. The catalogue is world-free, BuildRealistic is arithmetic over boxes and a
 * graph, and SolveAndBreak is a synchronous router settle over that graph — no UWorld, no Chaos, no tick.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges files into one translation unit.
 */
namespace RealisticShedCollapseTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	const TCHAR* const RealisticScenarioName = TEXT("shedrealistic");

	/* THE BACK-WALL EAVES COURSE (course 15) — the nine bricks that are the back gable's footing, by exact
	 * box centre. The scenario resolves a cut centre to a piece with a 1e-6 exact match, so these doubles are
	 * the same literals dev adds to CutCentresCm. Course 15 is odd: a half bat, seven full bricks, a closing
	 * half bat, all at Y centre 128.875 and Z centre 115.75. */
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

	/* Probe points inside pieces the collapse must DROP (the back gable) and pieces it must SPARE. */
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

/**
 * PULLING THE BACK-WALL EAVES COURSE DROPS THE WHOLE BACK GABLE OF THE REALISTIC SHED, THROUGH THE PRODUCTION
 * WORLD PATH, WHILE THE REST OF THE SHED STANDS — the router-authority, camera-facing collapse the level shows.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
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

	/* ================================================================================
	 * ARM 0 — THE ROW EXISTS AND LAYS THE REALISTIC SHED. (Established by ShedRealisticRow; re-checked so
	 * the collapse arms have a structure to act on.)
	 * ================================================================================ */

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

	/* The nine back-eaves bricks the cut must name all exist where hand-derived — an independent geometry
	 * check, true regardless of whether the cut is wired, so a wrong centre fails visibly here. */
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

	/* ================================================================================
	 * ARM 1 — THE RED. The row must NAME the back-eaves cut: the nine course-15 bricks under the back gable.
	 * Before the rewire the row names the OLD two-brick door-lintel cut, so these assertions fail. Dev replaces
	 * the row's CutCentresCm with the nine centres (and relaxes ShedRealisticRow's Cut.Num() pin to nine).
	 * ================================================================================ */

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

	/* ================================================================================
	 * ARM 2 — THE COLLAPSE, THROUGH THE PRODUCTION WORLD PATH. Apply the row's cut if it named the nine
	 * back-eaves bricks, else pull them ourselves so the router-correct collapse is exercised now and the
	 * assertions are proven to fail (in ARM 1) only for want of the CUT DATA — not a wrong physics claim.
	 * At 442 blocks SolveAndBreak's authority is the ROUTER. Support-state only, never displacement.
	 * ================================================================================ */
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

		/* Apply the row's own cut, but only if it is the back-eaves cut this test drives. */
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

		/* FALLBACK so ARM 2 says something on an un-rewired row: pull the nine back-eaves bricks ourselves.
		 * This makes ARM 2 green now and, once the cut lands, the row performs the identical removal, so ARM 2
		 * stays green while ARM 1 flips green with the cut. */
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

		/* A CLEAN COLLAPSE, NOT A ROUTING KNOT. Nothing may be Stranded, or a routing limitation would be
		 * wearing the collapse's clothes (DESIGN.md §4). */
		TestEqual(TEXT("ARM 2: nothing may be Stranded — the fall must be about the shed, not the router declining"),
			Stranded, 0);

		/* THE BACK GABLE DROPS AS A BIG CHUNK — its footing is gone and it has no lateral abutment. A large,
		 * camera-facing fall (>= 15 pieces), mechanism / support-state, never displacement. */
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

		/* ================================================================================
		 * THE RIDGE GOES WITH THE BACK GABLE — the review-item-3 RED. The ridge board spans the full depth
		 * and bears on the apex bricks of BOTH gables through compression-only dry seats. Once the back gable
		 * is gone it keeps only its FRONT-gable bearings, so its centre of mass (Y ~ 67, mid-depth) sits ~62 cm
		 * BEHIND that surviving front bearing line (Y ~ 5): a rigid board on a single line of compression-only
		 * bearings whose CoM projects past the line, and no tension tie to hold it — it overturns.
		 *
		 * Production STANDS it today: with two front bearings the ridge has two load paths, SolveLoads zeroes
		 * its overturning moment (bLoadPathIsDeterminate is false for N >= 2), the seats read a comfortable
		 * split compression, nothing breaks, and it reads Supported — the "top board still standing" artefact,
		 * the reason the current collapse releases the four purlins (each on ONE bearing per gable, so N = 1
		 * determinate) but NOT the ridge. The fix is the CoM-in-support-union gate WITH a tension clause. */
		TestTrue(
			*FString::Printf(TEXT("RED: the ridge (piece %d, support %d) must lose the earth — its CoM is behind "
				"its surviving front-gable bearing line"),
				Ridge, Ridge == INDEX_NONE ? -1 : static_cast<int32>(Pulled.Structure.GetPieceSupport(Ridge))),
			HasLostTheEarth(Pulled.Structure, Ridge));

		/* ================================================================================
		 * THE PORCH OVERHANG STAYS UP — the ANTI-REGRESSION that forbids the naive per-axis fix. Its centre of
		 * mass (Y = -36) is outboard of its two compression-only posts (Y = -20), so a union-only gate would
		 * fell it — but it is genuinely held by the CLEAT, a Z-normal Screw withdrawal tie (Tensile > 0). It
		 * really does stand, and the tension clause is what spares it. It is independent of the back gable, so
		 * removing the back eaves course must leave it Supported. If this ever reads not-standing, the fix has
		 * over-reached and felled a body a tension tie holds. */
		TestTrue(
			*FString::Printf(TEXT("ANTI-REGRESSION: the porch overhang (piece %d, support %d) must keep the earth — "
				"its cleat is a tension-capable Screw tie"),
				PorchOverhang, static_cast<int32>(Pulled.Structure.GetPieceSupport(PorchOverhang))),
			IsStanding(Pulled.Structure.GetPieceSupport(PorchOverhang)));

		/* THE REST OF THE SHED KEEPS THE EARTH — the fall is local to the back gable. The door head and window
		 * lintel on the far walls, the FRONT gable, the back-wall body below the cut, and the porch posts are all
		 * independent of the back eaves course. */
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
