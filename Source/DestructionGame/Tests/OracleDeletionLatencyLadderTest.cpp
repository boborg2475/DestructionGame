// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"
#include "Core/WallCases.h"
#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Block-size threshold for size-scoped LP authority (PROMOTION_DESIGN §12 D2⁗, §5.6; DESIGN §8,
 * 2026-08-18). A measurement, not production code: below what size does the deletion re-solve
 * (one brick removed, feasibility pose at λ = 1, bGravityIsLive = false) fit the ~50 ms per-solve
 * budget? Each rung deletes the non-grounded brick nearest the centroid-box centre (lowest index
 * on ties) and must stay feasible.
 *
 * Pinned: blocks, joints, pivots, rows, structural and total columns, and 1 + 8·joints as an
 * independent check on structural columns. Wall-clock is reported, not pinned; it varies ~5%.
 *
 * Measured 2026-08-21: median 47.1 ms at 83 blocks / 357 pivots, 101.7 ms at 104 / 552. So the
 * 50 ms crossing is ~84 blocks and the 100 ms crossing ~103. Ms grows much faster than pivots
 * (174x vs 11.9x across the ladder), so a deterministic budget is safer as a block cap than a
 * single pivot cap.
 *
 * In OracleSweepFull because pivot drift is a solver change; ~15 s. No ticking world. Named
 * namespace, Ladder-prefixed names, for unity builds.
 */
namespace OracleDeletionLatencySupport
{
	using namespace DestructionProfiles;
	using namespace RigidBlockOracle;

	// Brick and grid written out, so a wrong production constant disagrees with this file.
	constexpr double LadderBrickLengthCm = 21.5;
	constexpr double LadderBrickWidthCm = 10.25;
	constexpr double LadderBrickHeightCm = 6.5;
	constexpr double LadderClayDensityGramsPerCubicCm = 1.9;
	constexpr double LadderJointCm = 1.0;

	/** Timing samples per rung; pivots are identical every run. */
	constexpr int32 LadderTimingSamples = 7;

	/** Sentinel for an unmeasured pin; the solver never emits a negative count. */
	constexpr int32 LadderUnmeasured = -2;

	bool LadderBuildIntactWall(
		int32 Courses, int32 Cells, DestructionWallCases::FWallLayout& Out, FString& OutWhy)
	{
		using namespace DestructionWallCases;

		FWallSpec Spec;
		Spec.BrickSizeCm = FVector(LadderBrickLengthCm, LadderBrickWidthCm, LadderBrickHeightCm);
		Spec.JointThicknessCm = LadderJointCm;
		Spec.DensityGramsPerCubicCm = LadderClayDensityGramsPerCubicCm;
		Spec.CoursesHigh = Courses;
		Spec.Cells = Cells;
		Spec.Bond = EWallBond::Running;
		Spec.Strength = GeneralPurposeMortar;

		if (!Build(Spec, Out))
		{
			OutWhy = FString::Printf(
				TEXT("the wall producer refused %d courses x %d cells"), Courses, Cells);
			return false;
		}

		return true;
	}

	/** The non-grounded live piece nearest a point in X-Z; the lowest index wins a tie. */
	int32 LadderNearestLoadBearing(const FStructure& S, double TargetXCm, double TargetZCm)
	{
		int32 Best = INDEX_NONE;
		double BestDistSq = TNumericLimits<double>::Max();

		for (int32 I = 0; I < S.NumPieces(); ++I)
		{
			if (S.IsPieceRemoved(I))
			{
				continue;
			}

			const FStructurePiece& P = S.GetPiece(I);

			if (P.bIsGrounded)
			{
				continue;
			}

			const double Dx = P.CentreOfMassCm.X - TargetXCm;
			const double Dz = P.CentreOfMassCm.Z - TargetZCm;
			const double DistSq = Dx * Dx + Dz * Dz;

			if (DistSq < BestDistSq)
			{
				BestDistSq = DistSq;
				Best = I;
			}
		}

		return Best;
	}

	/** The centroid bounding box over the wall's live pieces. */
	void LadderCentroidBounds(
		const FStructure& S,
		double& OutMinX, double& OutMaxX, double& OutMinZ, double& OutMaxZ)
	{
		OutMinX = OutMinZ = TNumericLimits<double>::Max();
		OutMaxX = OutMaxZ = -TNumericLimits<double>::Max();

		for (int32 I = 0; I < S.NumPieces(); ++I)
		{
			if (S.IsPieceRemoved(I))
			{
				continue;
			}

			const FVector& C = S.GetPiece(I).CentreOfMassCm;

			OutMinX = FMath::Min(OutMinX, C.X);
			OutMaxX = FMath::Max(OutMaxX, C.X);
			OutMinZ = FMath::Min(OutMinZ, C.Z);
			OutMaxZ = FMath::Max(OutMaxZ, C.Z);
		}
	}

	/** The mid-span, mid-height load-bearing brick: nearest the centroid box's centre. */
	int32 LadderSelectMidSpan(const FStructure& S)
	{
		double MinX, MaxX, MinZ, MaxZ;
		LadderCentroidBounds(S, MinX, MaxX, MinZ, MaxZ);

		return LadderNearestLoadBearing(S, 0.5 * (MinX + MaxX), 0.5 * (MinZ + MaxZ));
	}

	/** The contrast brick: mid-height at the wall's left end. */
	int32 LadderSelectEdge(const FStructure& S)
	{
		double MinX, MaxX, MinZ, MaxZ;
		LadderCentroidBounds(S, MinX, MaxX, MinZ, MaxZ);

		return LadderNearestLoadBearing(S, MinX, 0.5 * (MinZ + MaxZ));
	}

	struct FResolveReading
	{
		bool bBuilt = false;
		FString Why;

		int32 Blocks = 0;
		int32 Joints = 0;

		bool bAnswered = false;
		double Lambda = 0.0;

		// Deterministic, pinned.
		int32 Pivots = 0;
		int32 Rows = 0;
		int32 StructCols = 0;
		int32 TotalCols = 0;

		// Reported only.
		double BestSeconds = 0.0;
		double MedianSeconds = 0.0;
	};

	/** Delete one brick from a fresh wall and time the feasibility re-solve LadderTimingSamples times. */
	FResolveReading LadderResolveAfterDeletion(int32 Courses, int32 Cells, bool bEdgeBrick)
	{
		FResolveReading Out;

		DestructionWallCases::FWallLayout Laid;

		if (!LadderBuildIntactWall(Courses, Cells, Laid, Out.Why))
		{
			return Out;
		}

		FStructure& Structure = Laid.Layout.Structure;

		const int32 Victim =
			bEdgeBrick ? LadderSelectEdge(Structure) : LadderSelectMidSpan(Structure);

		if (Victim == INDEX_NONE)
		{
			Out.Why = TEXT("no non-grounded brick to delete");
			return Out;
		}

		if (!Structure.RemovePiece(Victim))
		{
			Out.Why = FString::Printf(TEXT("could not remove piece %d"), Victim);
			return Out;
		}

		FOracleProblem Problem;

		if (!BuildRigidBlockProblem(Structure, Problem, Out.Why))
		{
			return Out;
		}

		// Feasibility pose: gravity is a constant RHS.
		Problem.bGravityIsLive = false;

		Out.Blocks = Problem.Blocks.Num();
		Out.Joints = Problem.Joints.Num();

		TArray<double> Samples;
		Samples.Reserve(LadderTimingSamples);

		FOracleResult Result;

		for (int32 Sample = 0; Sample < LadderTimingSamples; ++Sample)
		{
			const double Started = FPlatformTime::Seconds();
			Result = SolveRigidBlock(Problem);
			Samples.Add(FPlatformTime::Seconds() - Started);
		}

		Out.bBuilt = true;
		Out.bAnswered = Result.bAnswered;
		Out.Lambda = Result.Lambda;
		Out.Pivots = Result.SimplexIterations;
		Out.Rows = Result.FinalBasis.Columns.Num();
		Out.StructCols = Result.FinalBasis.NumStructCols;
		Out.TotalCols = Result.FinalBasis.ArtificialStart + Out.Rows;

		Samples.Sort();
		Out.BestSeconds = Samples[0];
		Out.MedianSeconds = Samples[Samples.Num() / 2];

		return Out;
	}
}

/**
 * Each rung lays a wall, deletes its mid-span brick and times the feasibility re-solve.
 * Deterministic quantities are pinned; ms is reported.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOracleDeletionResolveLatencyLadderTest,
	"OracleSweepFull.RigidBlock.DeletionResolveLatencyLadder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOracleDeletionResolveLatencyLadderTest::RunTest(const FString& Parameters)
{
	using namespace OracleDeletionLatencySupport;

	struct FRung
	{
		const TCHAR* Name = nullptr;
		int32 Courses = 0;
		int32 Cells = 0;

		// Derived from the bond rule: C·Cells + C/2 - 1 after the deletion.
		int32 WantBlocks = 0;

		int32 WantPivots = 0;
		int32 WantJoints = 0;
		int32 WantRows = 0;
		int32 WantStructCols = 0;
		int32 WantTotalCols = 0;
	};

	/*
	 * Measured 2026-08-21. WantBlocks is derived; the rest are measured.
	 *          {  name,    C, Cells, blocks, pivots, joints,  rows, structCols, totalCols }
	 */
	FRung Rungs[] =
	{
		{ TEXT("4x10"),  4, 10,  41,  122,  83, 1090,  665,  2752 },
		{ TEXT("6x10"),  6, 10,  62,  248, 142, 1861, 1137,  4703 },
		{ TEXT("8x10"),  8, 10,  83,  357, 201, 2632, 1609,  6654 },
		{ TEXT("10x10"), 10, 10, 104,  552, 260, 3403, 2081,  8605 },
		{ TEXT("12x10"), 12, 10, 125,  744, 319, 4174, 2553, 10556 },
		{ TEXT("12x12"), 12, 12, 149,  873, 385, 5032, 3081, 12734 },
		{ TEXT("14x12"), 14, 12, 174, 1274, 456, 5959, 3649, 15081 },
		{ TEXT("16x12"), 16, 12, 199, 1456, 527, 6886, 4217, 17428 },
	};

	constexpr int32 NumRungs = UE_ARRAY_COUNT(Rungs);

	// (blocks, pivots, median ms) per answered rung, for the threshold readout.
	TArray<int32> CrossBlocks;
	TArray<int32> CrossPivots;
	TArray<double> CrossMillis;

	for (int32 Index = 0; Index < NumRungs; ++Index)
	{
		const FRung& Rung = Rungs[Index];

		const FResolveReading R =
			LadderResolveAfterDeletion(Rung.Courses, Rung.Cells, /*bEdgeBrick*/ false);

		if (!TestTrue(
				*FString::Printf(TEXT("%s: the reduced problem must build and solve (it said: %s)"),
					Rung.Name, *R.Why),
				R.bBuilt))
		{
			continue;
		}

		const double MedianMs = R.MedianSeconds * 1000.0;
		const double BestMs = R.BestSeconds * 1000.0;

		const FString Line = FString::Printf(
			TEXT("LADDER %-5s: blocks=%d joints=%d answered=%d lambda=%.17g pivots=%d ")
			TEXT("rows=%d structCols=%d totalCols=%d  ms(best=%.1f median=%.1f)"),
			Rung.Name, R.Blocks, R.Joints, R.bAnswered ? 1 : 0, R.Lambda, R.Pivots,
			R.Rows, R.StructCols, R.TotalCols, BestMs, MedianMs);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);

		// The wall must survive the deletion; otherwise this measures collapse.
		TestTrue(
			*FString::Printf(TEXT("%s: the re-solve must ANSWER (a refusal is not an infeasibility)"),
				Rung.Name),
			R.bAnswered);

		TestTrue(
			*FString::Printf(
				TEXT("%s: the wall must still stand with one mid-span brick out (feasible, ")
				TEXT("lambda=%.17g >= 1) — an infeasible rung would be measuring collapse, not a ")
				TEXT("survived deletion"),
				Rung.Name, R.Lambda),
			R.bAnswered && R.Lambda >= 1.0);

		// Catches a rung solving the wrong wall.
		TestEqual(
			*FString::Printf(TEXT("%s: post-deletion block count"), Rung.Name),
			R.Blocks, Rung.WantBlocks);

		TestEqual(
			*FString::Printf(TEXT("%s: post-deletion joint count"), Rung.Name),
			R.Joints, Rung.WantJoints);

		// 1 lambda column + 4 per contact x 2 contacts per joint.
		TestEqual(
			*FString::Printf(
				TEXT("%s: structural columns must be 1 + 8*joints = %d (an independent ")
				TEXT("derivation of the problem width)"),
				Rung.Name, 1 + 8 * R.Joints),
			R.StructCols, 1 + 8 * R.Joints);

		TestEqual(
			*FString::Printf(TEXT("%s: feasibility-pose deletion re-solve PIVOTS"), Rung.Name),
			R.Pivots, Rung.WantPivots);

		TestEqual(
			*FString::Printf(TEXT("%s: standard-form ROWS"), Rung.Name),
			R.Rows, Rung.WantRows);

		TestEqual(
			*FString::Printf(TEXT("%s: structural COLUMNS"), Rung.Name),
			R.StructCols, Rung.WantStructCols);

		TestEqual(
			*FString::Printf(TEXT("%s: total COLUMNS"), Rung.Name),
			R.TotalCols, Rung.WantTotalCols);

		if (R.bAnswered)
		{
			CrossBlocks.Add(R.Blocks);
			CrossPivots.Add(R.Pivots);
			CrossMillis.Add(MedianMs);
		}
	}

	// Reported, not asserted: the crossing rests on wall-clock.
	auto ReportCrossing = [&](double BudgetMs)
	{
		int32 CrossedIndex = INDEX_NONE;

		for (int32 I = 0; I < CrossMillis.Num(); ++I)
		{
			if (CrossMillis[I] > BudgetMs)
			{
				CrossedIndex = I;
				break;
			}
		}

		if (CrossedIndex == INDEX_NONE)
		{
			AddInfo(FString::Printf(
				TEXT("THRESHOLD @ %.0f ms: every laddered rung is UNDER budget (largest is ")
				TEXT("%d blocks / %d pivots at %.1f ms) — the crossing is above the ladder"),
				BudgetMs,
				CrossBlocks.Num() ? CrossBlocks.Last() : 0,
				CrossPivots.Num() ? CrossPivots.Last() : 0,
				CrossMillis.Num() ? CrossMillis.Last() : 0.0));
			return;
		}

		if (CrossedIndex == 0)
		{
			AddInfo(FString::Printf(
				TEXT("THRESHOLD @ %.0f ms: even the smallest rung (%d blocks / %d pivots) is ")
				TEXT("OVER budget at %.1f ms — the crossing is below the ladder"),
				BudgetMs, CrossBlocks[0], CrossPivots[0], CrossMillis[0]));
			return;
		}

		// Interpolate between the last rung under budget and the first over.
		const int32 Lo = CrossedIndex - 1;
		const int32 Hi = CrossedIndex;
		const double T =
			(BudgetMs - CrossMillis[Lo]) / (CrossMillis[Hi] - CrossMillis[Lo]);
		const double Blocks = CrossBlocks[Lo] + T * (CrossBlocks[Hi] - CrossBlocks[Lo]);
		const double Pivots = CrossPivots[Lo] + T * (CrossPivots[Hi] - CrossPivots[Lo]);

		AddInfo(FString::Printf(
			TEXT("THRESHOLD @ %.0f ms: crosses between %d blocks (%.1f ms) and %d blocks ")
			TEXT("(%.1f ms) -> ~%.0f blocks / ~%.0f pivots"),
			BudgetMs, CrossBlocks[Lo], CrossMillis[Lo], CrossBlocks[Hi], CrossMillis[Hi],
			Blocks, Pivots));
	};

	ReportCrossing(50.0);
	ReportCrossing(100.0);

	// Contrast: the same 8×10 wall with an edge brick deleted, as a control on the mid-span rule.
	{
		const FResolveReading Edge =
			LadderResolveAfterDeletion(8, 10, /*bEdgeBrick*/ true);

		constexpr int32 WantEdgeBlocks = 83;

		// Measured 2026-08-21: ~7% more than the mid-span brick's 357, so which brick matters modestly.
		constexpr int32 WantEdgePivots = 381;

		if (TestTrue(
				*FString::Printf(TEXT("edge contrast: the reduced problem must build (it said: %s)"),
					*Edge.Why),
				Edge.bBuilt))
		{
			const FString Line = FString::Printf(
				TEXT("CONTRAST 8x10 EDGE brick out: blocks=%d joints=%d answered=%d ")
				TEXT("lambda=%.17g pivots=%d  ms(best=%.1f median=%.1f)"),
				Edge.Blocks, Edge.Joints, Edge.bAnswered ? 1 : 0, Edge.Lambda, Edge.Pivots,
				Edge.BestSeconds * 1000.0, Edge.MedianSeconds * 1000.0);

			UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
			AddInfo(Line);

			TestEqual(TEXT("edge contrast: post-deletion block count"),
				Edge.Blocks, WantEdgeBlocks);

			TestEqual(TEXT("edge contrast: feasibility-pose PIVOTS (which brick can matter)"),
				Edge.Pivots, WantEdgePivots);

			// The only ms assertion: a ceiling ~30x above the ~51 ms anchor, so it can't flake.
			TestTrue(
				*FString::Printf(
					TEXT("edge contrast: the 83-block re-solve must finish under a 1.5 s ")
					TEXT("catastrophe ceiling (best was %.1f ms) — the ONLY ms assertion here"),
					Edge.BestSeconds * 1000.0),
				Edge.BestSeconds < 1.5);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
