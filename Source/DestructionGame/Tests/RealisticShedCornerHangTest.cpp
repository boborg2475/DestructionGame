// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/DestructionShed3D.h"
#include "Core/Layout.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Item 6b: with the realistic shed's full-width low back-wall band removed (courses 6-7, 17 bricks),
 * the panel above hangs only on the two corner perpends. With the weak-perpend row (cohesion 0.2,
 * tensile 0.1) it loses the earth, with nothing Stranded. Strong mortar (0.9 / 0.7) held it by
 * arching. The intact shed still stands with weak perpends (RealisticBrickShedShellStandsAsBuilt).
 *
 * Asserts support state, never displacement (DESIGN §4); Stranded is asserted zero separately.
 * ShedSideUndermineProbe's baseline `TestEqual(BaselineLost, 0)` pins the old arch-holds behaviour
 * and must be updated with this change. World-free router solve. Named namespace for unity builds.
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

/** Removing the low back-wall band drops the panel that hung on the corner perpends. See the file header. */
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

	// Courses 6 and 7 of the +Y back wall, identified as the SideUndermine probe does.
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

	// A section (>= 5), so a one-brick edge effect cannot pass it.
	TestTrue(
		*FString::Printf(TEXT("RED: with weak perpends the low-band-removed panel must LOSE THE EARTH — a "
			"section comes down (>= 5 live survivors). It arches today, so %d fall."), Lost),
		Lost >= 5);

	// Item-5 pin: Stranded would mean the solver declined rather than the structure giving.
	TestEqual(TEXT("HONEST: nothing is Stranded — the panel genuinely lost its load path, the solver did not "
		"decline"), Stranded, 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
