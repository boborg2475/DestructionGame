// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/DestructionShed3D.h"
#include "Core/Layout.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#include "World/DestructionScenarios.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE REALISTIC-BRICK SHED FALLS CORRECTLY WHEN YOU TAKE BRICKS OUT — slice 4 of the realistic shed,
 * the COLLAPSE arm, driven through the production world path under the ROUTER (the break authority at
 * this scale). This is the world-path counterpart of "watch it fall if you take bricks out": the
 * `shedrealistic` row must name a CUT that pulls the two bricks the door lintel bears on, and once that
 * cut is applied the door head — the lintel and the masonry coursing above it — must lose the earth,
 * while the rest of the shed keeps it.
 *
 * THE BEHAVIOUR, IN ONE SENTENCE. The `shedrealistic` catalogue row names a cut that removes the two
 * course-11 pier-top bricks the Timber door lintel bears on, so that DestructionScenarios::Build resolves
 * that cut and — after the level applies it (RemovePiece) and settles through the production door
 * (FStructure::SolveAndBreak, whose authority at 442 blocks is the ROUTER, above the 200-block cap) — the
 * door lintel and the door-head masonry above it lose the earth (a genuine collapse, nothing Stranded),
 * while the window lintel on the far wall, the ridge, the back wall and the porch posts keep the earth.
 *
 * =====================================================================================
 * WHY THE ROUTER, AND WHY ONLY THE "TAKE BRICKS OUT" CASE (the authority decision this slice records)
 * =====================================================================================
 *
 * At 442 blocks the shed is far above the equilibrium gate's 200-block cap, so SolveAndBreak's authority
 * is the ROUTER's per-joint capacity sweep — the same path the flagship ~1200-block wall uses; the 3D LP
 * is bypassed. That authority was CHOSEN for this shed on measured cost (test-expert slice-4 report): a
 * router solve at 442 blocks is ~3 ms, whereas a single LP feasibility solve at 442 runs for MANY MINUTES
 * (measured > 10 min, still unfinished — the LP is super-linear and the promotable band was ~84-104
 * blocks). The cascade does several solves per action, so raising the cap to keep the accurate 3D LP is
 * impractical here; the router is the only tractable authority at this scale.
 *
 * THE ROUTER GETS "TAKE BRICKS OUT" RIGHT AND "PULL A POST" WRONG — an honest, recorded scale limit.
 *   - TAKE BRICKS OUT (this test): the router routes load down bed joints, so pulling the two bricks the
 *     lintel bears on correctly strands the lintel and the door head above it — a clean fall (0 stranded).
 *   - PULL A POST (NOT tested here): measured, the router OVER-HOLDS the porch overhang — with one post
 *     gone it still reads Supported (the surviving post gives a downward path, and the router has no
 *     overturning / X-torsion bracket). The accurate post-collapse needs the 3D LP, which is impractical
 *     at 442 blocks, so that case is a documented scale limitation, NOT forced green here.
 *
 * =====================================================================================
 * THE CASE-A CUT, HAND-DERIVED — WHICH TWO BRICKS, AND WHY THEY STRAND THE DOOR HEAD
 * =====================================================================================
 *
 * The door gap is X[57.5,122.5], rising courses 0..11 (Z[0,89]); the Timber lintel occupies course 12
 * (Z[90,96.5]) with footprint X[50,130]. In the running bond, the ONLY masonry the lintel's bottom (Z=90)
 * beds on one joint below (Z=89) is the two course-11 (odd course) pier-top bricks:
 *     LEFT  brick X[33.75,55.25], centre (44.5,  5.125, 85.75) — bears the lintel over X[50,55.25];
 *     RIGHT brick X[123.75,145.25], centre (134.5, 5.125, 85.75) — bears the lintel over X[123.75,130].
 * No course-11 brick sits under the lintel between them (that span is the door gap), and the course-12
 * masonry beside the lintel stands off its ends by more than a joint (a 6 cm gap, X=44 to X=50), so the
 * lintel is carried by exactly those two bearings. Pull both and the lintel — and the spandrel bricks that
 * bed down onto it — have no downward path left. MEASURED through the router: 8 pieces lose the earth
 * (the lintel + 7 above), and NOTHING is stranded — a clean collapse of the door head. The far side of
 * the shed (window lintel, ridge, back wall, posts) is untouched and stands.
 *
 * =====================================================================================
 * THE RED, AND WHAT DEV BUILDS
 * =====================================================================================
 *
 * ARM 1 is the RED: the `shedrealistic` row names NO cut today (slice 5 left CutCentresCm empty), so
 * Build resolves an EMPTY cut and the cut-naming assertions fail. Dev makes it green by adding the two
 * bearing centres (44.5, 5.125, 85.75) and (134.5, 5.125, 85.75) to the row's CutCentresCm and updating
 * the row's Title/Expectation. THAT ALSO SUPERSEDES the slice-5 placeholder `Cut.Num() == 0` assertion in
 * World.Scenarios.ShedRealisticRow — dev must relax that to the new cut count as part of wiring the cut.
 *
 * ARM 2 (the collapse itself) is written to run whether or not the cut is wired: it applies the row's cut
 * if present, else pulls the two bearing bricks itself, so the router-correct collapse is exercised now
 * (proving the assertions are right and fail for the intended reason — the missing CUT DATA, not a wrong
 * physics expectation) and stays green once the cut lands. Assertions are on mechanism / support-state
 * (HasLostTheEarth, Stranded == 0), never displacement (DESIGN.md §4).
 *
 * NEEDS A TICKING WORLD: NO. The catalogue is world-free, BuildRealistic is arithmetic over boxes and a
 * graph, and SolveAndBreak is a synchronous router settle over that graph — no UWorld, no Chaos, no tick.
 * The 442-block router solve is a few milliseconds. Same footing as ShedRealisticRow and Shed3DRow.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges files into one translation unit.
 */
namespace RealisticShedCollapseTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	const TCHAR* const RealisticScenarioName = TEXT("shedrealistic");

	/* THE CASE-A CUT — the two course-11 bricks the door lintel bears on, by exact box centre. The
	 * scenario resolves a cut centre to a piece with a 1e-6 exact match, so these doubles are the same
	 * literals dev adds to CutCentresCm. */
	const FVector LintelBearingLeft(44.5, 5.125, 85.75);
	const FVector LintelBearingRight(134.5, 5.125, 85.75);

	/* Probe points inside the far-side pieces that must KEEP the earth, and inside the door lintel. */
	const FVector DoorLintelPt(90.0, 5.0, 93.0);       // inside the Timber door lintel
	const FVector WindowLintelPt(5.0, 75.0, 93.0);     // inside the Timber window lintel (far wall)
	const FVector BackWallPt(90.0, 128.875, 40.0);     // a back-wall brick, mid-height

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
 * PULLING THE TWO BRICKS THE DOOR LINTEL BEARS ON DROPS THE DOOR HEAD OF THE REALISTIC SHED, THROUGH THE
 * PRODUCTION WORLD PATH, WHILE THE REST OF THE SHED STANDS — the router-authority collapse the level shows.
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

	/* The two bearing bricks the cut must name exist where hand-derived — an independent geometry check,
	 * true regardless of whether the cut is wired, so a wrong centre fails visibly here. */
	const int32 BearingLeft = PieceContaining(Layout, LintelBearingLeft);
	const int32 BearingRight = PieceContaining(Layout, LintelBearingRight);

	TestTrue(TEXT("ARM 0: a ClayBrick sits at the LEFT door-lintel bearing (44.5, 5.125, 85.75)"),
		BearingLeft != INDEX_NONE && Layout.Structure.GetPiece(BearingLeft).Material == &ClayBrick);
	TestTrue(TEXT("ARM 0: a ClayBrick sits at the RIGHT door-lintel bearing (134.5, 5.125, 85.75)"),
		BearingRight != INDEX_NONE && Layout.Structure.GetPiece(BearingRight).Material == &ClayBrick);

	/* ================================================================================
	 * ARM 1 — THE RED. The row must NAME the case-A cut: the two bricks the door lintel bears on. Today the
	 * row's CutCentresCm is empty (slice 5), so Build resolves an empty cut and both assertions fail. Dev
	 * adds the two bearing centres to CutCentresCm (and relaxes ShedRealisticRow's Cut.Num()==0 placeholder).
	 * ================================================================================ */

	TestTrue(
		*FString::Printf(TEXT("RED: the '%s' row must NAME a cut so the level pulls bricks — it names %d "
			"today. Dev adds the two door-lintel bearing centres to the row's CutCentresCm."),
			RealisticScenarioName, Cut.Num()),
		Cut.Num() >= 1);

	const bool bCutNamesLeft = BearingLeft != INDEX_NONE && Cut.Contains(BearingLeft);
	const bool bCutNamesRight = BearingRight != INDEX_NONE && Cut.Contains(BearingRight);

	TestTrue(
		TEXT("RED: the cut must name the LEFT door-lintel bearing brick (44.5, 5.125, 85.75)"),
		bCutNamesLeft);
	TestTrue(
		TEXT("RED: the cut must name the RIGHT door-lintel bearing brick (134.5, 5.125, 85.75)"),
		bCutNamesRight);

	/* ================================================================================
	 * ARM 2 — THE COLLAPSE, THROUGH THE PRODUCTION WORLD PATH. Apply the row's cut if it named one, else
	 * pull the two bearing bricks ourselves so the router-correct collapse is exercised now and the
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

		const int32 DoorLintel = PieceContaining(Pulled, DoorLintelPt);
		const int32 WindowLintel = PieceContaining(Pulled, WindowLintelPt);
		const int32 Ridge = RidgePiece(Pulled);
		const int32 BackWall = PieceContaining(Pulled, BackWallPt);

		TestTrue(TEXT("ARM 2: the door lintel, window lintel, ridge and a back-wall brick must all be found"),
			DoorLintel != INDEX_NONE && WindowLintel != INDEX_NONE && Ridge != INDEX_NONE
				&& BackWall != INDEX_NONE);

		/* Apply the row's own cut — the removal the level performs. */
		int32 Applied = 0;
		for (const int32 Piece : PulledCut)
		{
			if (Pulled.Structure.RemovePiece(Piece))
			{
				++Applied;
			}
		}

		/* FALLBACK so ARM 2 says something on an un-wired row: pull the two bearing bricks ourselves. This
		 * is the case-A removal hand-derived above; it makes ARM 2 green now and once the cut lands the row
		 * performs the identical removal, so ARM 2 stays green while ARM 1 flips green with the cut. */
		if (Applied == 0)
		{
			const int32 L = PieceContaining(Pulled, LintelBearingLeft);
			const int32 R = PieceContaining(Pulled, LintelBearingRight);
			if (L != INDEX_NONE)
			{
				Pulled.Structure.RemovePiece(L);
			}
			if (R != INDEX_NONE)
			{
				Pulled.Structure.RemovePiece(R);
			}
			AddInfo(TEXT("ARM 2: row named no cut yet — pulled the two bearing bricks via fallback."));
		}

		const int32 Passes = Pulled.Structure.SolveAndBreak();
		const int32 Stranded = StrandedCount(Pulled.Structure);
		const int32 LostEarth = LostEarthCount(Pulled.Structure);

		AddInfo(FString::Printf(
			TEXT("ARM 2: production ran %d pass(es); %d stranded; %d lost the earth."),
			Passes, Stranded, LostEarth));

		/* A CLEAN COLLAPSE, NOT A ROUTING KNOT. Nothing may be Stranded, or a routing limitation would be
		 * wearing the collapse's clothes (DESIGN.md §4). */
		TestEqual(TEXT("ARM 2: nothing may be Stranded — the fall must be about the shed, not the router declining"),
			Stranded, 0);

		/* THE DOOR HEAD DROPS. The lintel lost both its bearings, so it loses the earth; the door-head
		 * masonry above it comes down with it — a chunk (>= 3 pieces), not one loose brick. Mechanism /
		 * support-state, never displacement. */
		TestTrue(
			*FString::Printf(TEXT("ARM 2: the door lintel (piece %d, support %d) must lose the earth — its two "
				"bearings are gone"), DoorLintel, static_cast<int32>(Pulled.Structure.GetPieceSupport(DoorLintel))),
			HasLostTheEarth(Pulled.Structure, DoorLintel));

		TestTrue(
			*FString::Printf(TEXT("ARM 2: the door head comes down as a chunk — >= 3 pieces lose the earth (got %d)"),
				LostEarth),
			LostEarth >= 3);

		/* THE REST OF THE SHED KEEPS THE EARTH — the fall is local to the door head. The window lintel on the
		 * far (left) wall, the ridge, and a back-wall brick are all independent of the door piers. */
		TestTrue(TEXT("ARM 2: the window lintel on the far wall keeps the earth"),
			IsStanding(Pulled.Structure.GetPieceSupport(WindowLintel)));
		TestTrue(TEXT("ARM 2: the ridge keeps the earth"),
			IsStanding(Pulled.Structure.GetPieceSupport(Ridge)));
		TestTrue(TEXT("ARM 2: a back-wall brick keeps the earth"),
			IsStanding(Pulled.Structure.GetPieceSupport(BackWall)));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
