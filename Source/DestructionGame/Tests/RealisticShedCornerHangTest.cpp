// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/DestructionShed3D.h"
#include "Core/Layout.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * ITEM 6b, THE INTENDED BEHAVIOUR CHANGE — THE DEEP-CUT CORNER-HANG NOW FAILS HONESTLY.
 *
 * THE BEHAVIOUR IN ONE SENTENCE. When the full-width low band of the realistic shed's back wall is knocked
 * out (courses 6 and 7, the 17 bricks the running-bond wall used to deep-beam / arch across into its two
 * back corners), the panel ABOVE the hole — which hangs on nothing but the two corner perpends — LOSES THE
 * EARTH once those vertical joints carry the weak-perpend row (cohesion 0.2, tensile 0.1) instead of full
 * bed mortar, and it does so with NOTHING stranded (item-5's fix holds).
 *
 * WHY THIS IS THE RIGHT ASSERTION, AND WHY IT IS NOT A STANDING-SHED REGRESSION. The FULLY INTACT shed
 * stands unchanged with weak perpends — its walls are independently grounded and its vertical joints carry
 * little (measured: RealisticBrickShedShellStandsAsBuilt and its siblings stay green). This fixture starts
 * from a DEEP CUT — 17 bricks already removed — so the panel above the hole hangs across a full-width gap on
 * the corner mortar alone. With STRONG mortar (0.9 / 0.7) that hang held at a comfortable margin (arch/deep-
 * beam action, 0 pieces lost — the state ShedRealisticLoads.Probe.SideUndermine's baseline pins today). With
 * the realistic WEAK perpend it can no longer hang, and the panel comes down. That is the honest fall the
 * item approves; a fully-supported wall would never reach this fixture's cut.
 *
 * THE ASSERTION IS SUPPORT STATE, NEVER DISPLACEMENT (DESIGN §4). A live survivor that reads neither
 * Grounded nor Supported after the router settle has "lost the earth"; a section coming down is many of them.
 * Stranded is asserted zero SEPARATELY so a solver-stranding artifact can never wear this collapse's clothes.
 *
 * RED TODAY: at HEAD every brick-brick joint is GeneralPurposeMortar (0.9 / 0.7), so the low-band-removed
 * panel still arches and 0 pieces lose the earth — the >= 5 assertion fails. It goes green when the vertical
 * brick-brick joints (perpends + corners) become the weak-perpend row.
 *
 * WHERE THIS LIVES, AND THE SIBLING PROBE. This is a FOCUSED default-suite fixture (world-free router solve,
 * ~100 ms — no ticking world, gravity on) rather than a mutation of the opt-in SideUndermine PROBE, because
 * the probe is a logging diagnostic that "asserts almost nothing". NOTE FOR DEV: the probe's own baseline
 * assertion `TestEqual(BaselineLost, 0)` (ShedSideUndermineProbe.cpp) pins the OLD arch-holds behaviour and
 * WILL flip to failing when this change lands — update it to the new "the arch now falls, N lost" reading in
 * the same slice (it is opt-in, so it does not touch the default suite, but it must not be left wrong).
 *
 * NEEDS A TICKING WORLD: NO.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges files into one translation unit.
 */
namespace RealisticShedCornerHangSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	const double BackWallYCm = 128.875;
	const double CoursePitchZCm = 7.5;
	const double CourseBaseZCm = 3.25;

	double CourseCentreZ(int32 Course)
	{
		return Course * CoursePitchZCm + CourseBaseZCm;
	}

	bool IsStanding(EPieceSupport Support)
	{
		return Support == EPieceSupport::Grounded || Support == EPieceSupport::Supported;
	}

	int32 LostEarthCount(const FStructure& S)
	{
		int32 N = 0;
		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (!S.IsPieceRemoved(P) && !IsStanding(S.GetPieceSupport(P)))
			{
				++N;
			}
		}
		return N;
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
}

/**
 * KNOCKING OUT THE FULL-WIDTH LOW BACK-WALL BAND DROPS THE PANEL THAT HUNG ON THE CORNER PERPENDS.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRealisticShedCornerHangTest,
	"DestructionGame.Acceptance.Shed.ThreeD.RealisticBrickShedCornerHangFallsWithWeakPerpends",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRealisticShedCornerHangTest::RunTest(const FString& Parameters)
{
	using namespace RealisticShedCornerHangSupport;
	using namespace DestructionProfiles;

	FBrickLayout L;
	const bool bBuilt = DestructionShed3D::BuildRealistic(L);
	TestTrue(TEXT("BUILD: the realistic-brick shed must build"), bBuilt);
	if (!bBuilt)
	{
		return false;
	}

	FStructure& S = L.Structure;

	/*
	 * THE DEEP CUT — the full-width low back-wall band: courses 6 and 7 of the +Y back wall (Z centres
	 * 48.25 / 55.75), the 17 bricks the running-bond wall deep-beams over by arching into its two back
	 * corners. Identified exactly as the SideUndermine probe does: ClayBrick, back-wall Y band, either
	 * course centre.
	 */
	int32 BandRemoved = 0;
	for (int32 P = 0; P < S.NumPieces(); ++P)
	{
		if (S.IsPieceRemoved(P) || S.GetPiece(P).Material != &ClayBrick || !L.Boxes.IsValidIndex(P))
		{
			continue;
		}
		const FVector C = L.Boxes[P].CentreCm;
		const bool bBackWall = FMath::Abs(C.Y - BackWallYCm) < 1.0;
		const bool bCourse6 = FMath::Abs(C.Z - CourseCentreZ(6)) < 1.0;
		const bool bCourse7 = FMath::Abs(C.Z - CourseCentreZ(7)) < 1.0;
		if (bBackWall && (bCourse6 || bCourse7))
		{
			if (S.RemovePiece(P))
			{
				++BandRemoved;
			}
		}
	}

	AddInfo(FString::Printf(TEXT("CUT: removed the %d-brick low back-wall band (courses 6-7)."), BandRemoved));
	TestTrue(TEXT("FIXTURE: the full-width low band is a real cut (the 17-brick back-wall band)"),
		BandRemoved >= 15);

	const int32 Passes = S.SolveAndBreak();
	const int32 Lost = LostEarthCount(S);
	const int32 Stranded = StrandedCount(S);

	AddInfo(FString::Printf(TEXT("HANG: router ran %d pass(es); %d live survivors lost the earth; %d stranded."),
		Passes, Lost, Stranded));

	/*
	 * THE RED — the panel above the hole comes down. At HEAD (strong perpends) it arches and 0 fall, so this
	 * fails; with the weak-perpend row the corner hang gives way and a whole section loses the earth. The
	 * threshold is a genuine SECTION (>= 5), not a single brick, so a one-brick edge effect cannot green it.
	 */
	TestTrue(
		*FString::Printf(TEXT("RED: with weak perpends the low-band-removed panel must LOSE THE EARTH — a "
			"section comes down (>= 5 live survivors). It arches today, so %d fall."), Lost),
		Lost >= 5);

	/*
	 * ITEM-5 PIN — the honest fall strands NOTHING. A stranded survivor would mean the solver declined rather
	 * than the structure genuinely giving; the corner-hang collapse must be a real loss of the earth.
	 */
	TestEqual(TEXT("HONEST: nothing is Stranded — the panel genuinely lost its load path, the solver did not "
		"decline"), Stranded, 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
