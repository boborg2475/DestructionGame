// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/DestructionShed3D.h"
#include "Core/Layout.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A PROGRESSIVE-FAILURE DIAGNOSTIC, NOT A PERMANENT RED. It starts from the ARCHED realistic shed — the
 * full-width low back-wall band (courses 6 and 7, 17 bricks) removed so the back wall stands only by arching
 * into its two back corners — then UNDERMINES the LEFT back corner abutment: it pulls the left side wall's
 * column of bricks nearest the back wall (largest Y), starting near the arch-thrust height (Z ~ 59.5, the
 * slot mid-height) and working DOWN toward the ground, ONE brick per step, re-solving with the ROUTER
 * (SolveAndBreak — the break authority at 442 blocks, well above the 200-block LP cap) after each removal.
 * It logs the whole progression so the question "is the arch's hold robust, or just barely propped by the
 * corner?" can be read off the step count and the shape of the failure (gradual vs sudden).
 *
 * It ASSERTS almost nothing — it exists to be grepped out of Saved/Logs/DestructionGame.log under the
 * SIDEUNDERMINE_ prefix. The name deliberately omits "DestructionGame" so the full suite never runs it;
 * invoke it with `Automation RunTests ShedRealisticLoads`.
 *
 * NEEDS A TICKING WORLD: NO. Boxes, doubles and the router; gravity on. No Chaos, no tick.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges files into one translation unit.
 */
namespace ShedSideUndermineProbeSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/* The two low-band courses of the back wall (the arch state) and the back-wall Y band. */
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

	/*
	 * A LIVE SURVIVOR THAT LOST THE EARTH — a piece that is still present (not one we deliberately removed)
	 * yet reads neither Grounded nor Supported after the router settle. This is the count that tells us the
	 * arch gave.
	 */
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

	/* One removable side-wall brick, with the geometry the removal order keys on. */
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

/**
 * PROGRESSIVELY UNDERMINE THE LEFT BACK-CORNER ABUTMENT OF THE ARCHED SHED AND WATCH FOR COLLAPSE.
 *
 * NEEDS A TICKING WORLD: NO. See the file header.
 */
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
	 * STEP 1 — THE ARCH STATE. Pull the full-width low back-wall band: courses 6 and 7 of the +Y back wall
	 * (Z centres 48.25 / 55.75), the 17-brick band the running-bond wall deep-beams over by arching into its
	 * two back corners. Identified exactly as ShedRealisticLoads.Dump.LowBandRemoved does: ClayBrick, in the
	 * back-wall Y band centred on 128.875, Z centre matching one of the two course centres.
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
	 * STEP 1 (cont.) — THE BASELINE SOLVE. The router settles the arched structure; with the low band gone the
	 * shed must still stand (0 live survivors lost the earth), or the experiment has no arch to undermine.
	 */
	const int32 BaselinePasses = S.SolveAndBreak();
	const int32 BaselineLost = LostEarthCount(S);
	const int32 BaselineStranded = StrandedCount(S);

	UE_LOG(LogTemp, Display,
		TEXT("SIDEUNDERMINE_BASELINE,lowband=%d,passes=%d,lostearth=%d,stranded=%d"),
		LowBandRemoved, BaselinePasses, BaselineLost, BaselineStranded);

	TestEqual(TEXT("BASELINE: the arched shed (low band removed) must stand — 0 live survivors lost the earth"),
		BaselineLost, 0);

	/*
	 * STEP 2 — BUILD THE UNDERMINING ORDER. The LEFT side wall runs along Y at X band [0, 10.25] (centre
	 * 5.125), thin in X. A left side-wall brick is a ClayBrick with cx ~ 5.125 whose Y centre sits strictly
	 * between the front corner (cy ~ 5.125) and the back wall (cy ~ 128.875) — cy in (10, 123). We keep only
	 * bricks at or below the arch-thrust height (course <= 8, Z centre <= ~63.75): undermining is about what
	 * carries the thrust DOWN to the ground, not the wall above it.
	 *
	 * The column NEAREST the back wall is, per course, the brick with the largest Y. We remove that column
	 * first, top (near the slot) to bottom (ground) — ColumnRank 0, course descending — then, only if the
	 * arch still holds, the next column in (ColumnRank 1), and so on up to the cap. That is "undermine the
	 * abutment from the load height down, one brick per step".
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

	/* Per-course column rank: how many bricks in the same course sit FURTHER back (greater Y). 0 = nearest. */
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

	/* Nearest column first (ColumnRank ascending); within a column, from the slot down (course descending). */
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

	/*
	 * STEP 3 — THE PROGRESSION. Remove one side brick, re-solve with the router, log the step. Continue until
	 * a live section comes down (lostearth >= 5) or we have pulled 25 side bricks without collapse.
	 */
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

	/*
	 * STEP 4 — CLASSIFY WHAT CAME DOWN. Tally the live survivors that lost the earth by rough role so the
	 * summary can name the failure: Timber roof members; back-wall / back-gable bricks (cy ~ 128.875); left
	 * side-wall bricks (cx ~ 5.125); and everything else.
	 */
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

	/*
	 * STEP 5 — THE CUT LIST. Every brick removed, in removal order — the 17 low-band bricks first, then the
	 * side bricks up to and including the collapse trigger — as SIDEUNDERMINE_CUT lines, so the exact cut can
	 * be grepped out and wired into a scenario for rendering.
	 */
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
