// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/DestructionShed3D.h"
#include "Core/Layout.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Progressive-failure diagnostic. From the realistic shed with the back wall's low band (courses 6
 * and 7, 17 bricks) removed, it pulls the left side wall's bricks nearest the back wall one at a
 * time, from arch-thrust height (Z ~ 59.5) down, running SolveAndBreak after each (442 blocks, above
 * the 200-block LP cap). The log shows how many steps the hold survives and whether failure is
 * gradual or sudden.
 *
 * Asserts only the baseline; read the SIDEUNDERMINE_ lines in Saved/Logs/DestructionGame.log. Its
 * name omits "DestructionGame" so the full suite skips it; run `Automation RunTests
 * ShedRealisticLoads`. Needs no world. Uniquely named namespace for unity builds.
 */
namespace ShedSideUndermineProbeSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	// Back-wall Y and the course grid, cm.
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

	// Live pieces that are neither Grounded nor Supported after the settle.
	int32 LostEarthCount(const FStructure& S)
	{
		int32 N = 0;
		for (int32 P = 0; P < S.NumPieces(); ++P)
		{
			if (S.IsPieceRemoved(P))
			{
				continue;
			}
			if (!IsStanding(S.GetPieceSupport(P)))
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

	// A removable side-wall brick and the geometry the removal order uses.
	struct FSideBrick
	{
		int32 Piece = INDEX_NONE;
		double Cx = 0.0;
		double Cy = 0.0;
		double Cz = 0.0;
		int32 Course = 0;
		int32 ColumnRank = 0;   // 0 = nearest the back wall (largest Y in its course)
	};
}

/** Progressively undermine the left back-corner abutment and log when it collapses. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FShedSideUndermineProbe,
	"ShedRealisticLoads.Probe.SideUndermine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FShedSideUndermineProbe::RunTest(const FString& Parameters)
{
	using namespace ShedSideUndermineProbeSupport;
	using namespace DestructionProfiles;
	using namespace DestructionLayout;

	FBrickLayout L;
	if (!DestructionShed3D::BuildRealistic(L))
	{
		AddError(TEXT("BuildRealistic returned false"));
		return false;
	}

	FStructure& S = L.Structure;

	/*
	 * Step 1: remove the back wall's low band, courses 6 and 7 (Z 48.25 / 55.75), selected as in
	 * ShedRealisticLoads.Dump.LowBandRemoved.
	 */
	TArray<int32> RemovedInOrder;
	{
		TArray<int32> Band;
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
				Band.Add(P);
			}
		}
		for (const int32 P : Band)
		{
			if (S.RemovePiece(P))
			{
				RemovedInOrder.Add(P);
			}
		}
	}

	const int32 LowBandRemoved = RemovedInOrder.Num();

	/*
	 * Baseline solve. Since item 6b (weak perpends: cohesion 0.2, tensile 0.1) the panel over the
	 * gap can't hang on the corners and falls: 93 pieces lose the earth, none stranded, matching
	 * RealisticBrickShedCornerHangFallsWithWeakPerpends. Before 6b this read 0.
	 */
	const int32 BaselinePasses = S.SolveAndBreak();
	const int32 BaselineLost = LostEarthCount(S);
	const int32 BaselineStranded = StrandedCount(S);

	UE_LOG(LogTemp, Display,
		TEXT("SIDEUNDERMINE_BASELINE,lowband=%d,passes=%d,lostearth=%d,stranded=%d"),
		LowBandRemoved, BaselinePasses, BaselineLost, BaselineStranded);

	TestEqual(TEXT("BASELINE: with weak perpends the low-band-removed panel comes down — the corner hang "
		"gives way and a whole section (93 live survivors) loses the earth"),
		BaselineLost, 93);
	TestEqual(TEXT("BASELINE: the fall strands nothing — the panel genuinely lost its load path"),
		BaselineStranded, 0);

	/*
	 * Step 2: the removal order. Left side-wall bricks are ClayBrick at cx ~ 5.125 with cy in
	 * (10, 123), between the corners; keep courses <= 8 (at or below the thrust height). Remove the
	 * column nearest the back wall first, top to bottom, then the next column in.
	 */
	TArray<FSideBrick> Side;
	for (int32 P = 0; P < S.NumPieces(); ++P)
	{
		if (S.IsPieceRemoved(P) || S.GetPiece(P).Material != &ClayBrick || !L.Boxes.IsValidIndex(P))
		{
			continue;
		}
		const FVector C = L.Boxes[P].CentreCm;
		const bool bLeftSideWall = FMath::Abs(C.X - 5.125) < 1.0 && C.Y > 10.0 && C.Y < 123.0;
		if (!bLeftSideWall)
		{
			continue;
		}
		const int32 Course = FMath::RoundToInt((C.Z - CourseBaseZCm) / CoursePitchZCm);
		if (Course > 8)
		{
			continue;
		}
		FSideBrick B;
		B.Piece = P;
		B.Cx = C.X;
		B.Cy = C.Y;
		B.Cz = C.Z;
		B.Course = Course;
		Side.Add(B);
	}

	// Column rank: bricks in the same course further back (greater Y). 0 = nearest the back wall.
	for (FSideBrick& B : Side)
	{
		int32 Rank = 0;
		for (const FSideBrick& Other : Side)
		{
			if (Other.Course == B.Course && Other.Cy > B.Cy + 1.0e-6)
			{
				++Rank;
			}
		}
		B.ColumnRank = Rank;
	}

	// Nearest column first; within a column, top down.
	Side.Sort([](const FSideBrick& A, const FSideBrick& B)
	{
		if (A.ColumnRank != B.ColumnRank)
		{
			return A.ColumnRank < B.ColumnRank;
		}
		return A.Course > B.Course;
	});

	UE_LOG(LogTemp, Display,
		TEXT("SIDEUNDERMINE_PLAN,left_side_bricks_at_or_below_course8=%d,cap=25,collapse_threshold_lostearth=5"),
		Side.Num());

	// Step 3: remove one brick per step and re-solve, until 5+ pieces lose the earth or 25 are pulled.
	const int32 SideCap = 25;
	const int32 CollapseThreshold = 5;

	int32 SideRemoved = 0;
	int32 CollapseAtStep = -1;
	int32 FinalLost = BaselineLost;
	int32 FinalStranded = BaselineStranded;

	for (const FSideBrick& B : Side)
	{
		if (SideRemoved >= SideCap)
		{
			break;
		}
		if (!S.RemovePiece(B.Piece))
		{
			continue;
		}
		RemovedInOrder.Add(B.Piece);
		++SideRemoved;

		const int32 Passes = S.SolveAndBreak();
		const int32 Lost = LostEarthCount(S);
		const int32 Stranded = StrandedCount(S);
		FinalLost = Lost;
		FinalStranded = Stranded;

		UE_LOG(LogTemp, Display,
			TEXT("SIDEUNDERMINE,step=%d,removed_cx=%.4g,removed_cy=%.4g,removed_cz=%.4g,removed_course=%d,"
				"removed_colrank=%d,cum_removed=%d,passes=%d,lostearth=%d,stranded=%d"),
			SideRemoved, B.Cx, B.Cy, B.Cz, B.Course, B.ColumnRank,
			LowBandRemoved + SideRemoved, Passes, Lost, Stranded);

		if (Lost >= CollapseThreshold)
		{
			CollapseAtStep = SideRemoved;
			break;
		}
	}

	// Step 4: tally fallen pieces by role: timber roof, back wall/gable, left side wall, other.
	int32 FellRoof = 0;
	int32 FellBackWall = 0;
	int32 FellLeftSide = 0;
	int32 FellOther = 0;
	for (int32 P = 0; P < S.NumPieces(); ++P)
	{
		if (S.IsPieceRemoved(P) || IsStanding(S.GetPieceSupport(P)) || !L.Boxes.IsValidIndex(P))
		{
			continue;
		}
		const FVector C = L.Boxes[P].CentreCm;
		if (S.GetPiece(P).Material == &Timber)
		{
			++FellRoof;
		}
		else if (FMath::Abs(C.Y - BackWallYCm) < 6.0 || C.Y > 123.0)
		{
			++FellBackWall;
		}
		else if (FMath::Abs(C.X - 5.125) < 1.0)
		{
			++FellLeftSide;
		}
		else
		{
			++FellOther;
		}
	}

	const int32 Collapsed = (CollapseAtStep > 0) ? 1 : 0;

	FString WhatFell;
	if (Collapsed == 0)
	{
		WhatFell = TEXT("nothing (arch held to the cap)");
	}
	else
	{
		WhatFell = FString::Printf(TEXT("roof=%d back/gable=%d leftside=%d other=%d"),
			FellRoof, FellBackWall, FellLeftSide, FellOther);
	}

	UE_LOG(LogTemp, Display,
		TEXT("SIDEUNDERMINE_RESULT,collapsed=%d,at_step=%d,side_bricks_removed=%d,total_lostearth=%d,"
			"stranded=%d,what_fell=<%s>"),
		Collapsed, CollapseAtStep, SideRemoved, FinalLost, FinalStranded, *WhatFell);

	// Step 5: log every removed brick in order as SIDEUNDERMINE_CUT, for reuse as a scenario cut.
	for (const int32 P : RemovedInOrder)
	{
		if (!L.Boxes.IsValidIndex(P))
		{
			continue;
		}
		const FVector C = L.Boxes[P].CentreCm;
		UE_LOG(LogTemp, Display, TEXT("SIDEUNDERMINE_CUT,%.4g,%.4g,%.4g"), C.X, C.Y, C.Z);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
