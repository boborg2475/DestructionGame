// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * GetLastSolveAndBreakReport's counts agree with the cascade: its passes are SolveAndBreak's
 * return plus the terminal one, its joint counts are the structure's, and a settle that breaks
 * nothing reports one pass. Wall-clock is only checked finite and non-negative, since a threshold
 * would flake. World-free.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSolveAndBreakReportTest,
	"DestructionGame.Core.Structure.SolveAndBreakReportMatchesTheCascade",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSolveAndBreakReportTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	FRunningBondSpec Spec;
	Spec.BrickSizeCm = FVector(21.5, 10.25, 6.5);
	Spec.JointThicknessCm = 1.0;
	Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
	Spec.CoursesHigh = 8;
	Spec.BricksPerCourse = 6;
	Spec.Strength = GeneralPurposeMortar;

	FBrickLayout Layout;

	if (!TestTrue(TEXT("fixture: the wall must lay"), RunningBond(Spec, Layout)))
	{
		return false;
	}

	FStructure& S = Layout.Structure;

	/* As built: one solve, nothing gives. */
	const int32 PassesAsBuilt = S.SolveAndBreak();
	const FStructure::FSolveAndBreakReport& AsBuilt = S.GetLastSolveAndBreakReport();

	TestEqual(TEXT("as built, the wall breaks in no pass"), PassesAsBuilt, 0);
	TestEqual(TEXT("the report's breaking passes are the return value"), AsBuilt.BreakingPasses, PassesAsBuilt);
	TestEqual(TEXT("a settle that breaks nothing is one terminal pass"), AsBuilt.Passes.Num(), 1);
	TestEqual(TEXT("live pieces before"), AsBuilt.LivePiecesBefore, S.NumLivePieces());
	TestEqual(TEXT("every joint was intact before"), AsBuilt.IntactJointsBefore, S.NumConnections());
	TestEqual(TEXT("every joint is intact after"), AsBuilt.IntactJointsAfter, S.NumConnections());

	if (AsBuilt.Passes.Num() == 1)
	{
		const FStructure::FBreakPassReport& Pass = AsBuilt.Passes[0];
		TestEqual(TEXT("the terminal pass is stamped 1"), Pass.Pass, 1);
		TestEqual(TEXT("the terminal pass severed nothing by sweep"), Pass.JointsGivenToSweep, 0);
		TestEqual(TEXT("the terminal pass severed nothing by prover"), Pass.JointsSeveredByProver, 0);
		TestEqual(TEXT("the terminal pass left every joint intact"), Pass.IntactJointsAfter, S.NumConnections());
		TestTrue(TEXT("the solve ran at least one fixpoint iteration"), Pass.Solve.FixpointIterations >= 1);
		TestTrue(TEXT("the solve's clock is finite and non-negative"),
			FMath::IsFinite(Pass.Solve.TotalMs) && Pass.Solve.TotalMs >= 0.0);
		TestTrue(TEXT("the pass's clock is finite and non-negative"),
			FMath::IsFinite(Pass.PassMs) && Pass.PassMs >= 0.0);
	}

	TestTrue(TEXT("the total clock is finite and non-negative"),
		FMath::IsFinite(AsBuilt.TotalMs) && AsBuilt.TotalMs >= 0.0);
	TestTrue(TEXT("the last SolveLoads profile is the terminal pass's"),
		S.GetLastSolveLoadsProfile().FixpointIterations == AsBuilt.Passes.Last().Solve.FixpointIterations);

	// Remove the bottom course except its end bricks, so something must give.
	int32 Removed = 0;
	for (int32 Piece = 0; Piece < S.NumPieces(); ++Piece)
	{
		const FPieceBox& Box = Layout.Boxes[Piece];
		const bool bBottom = Box.CentreCm.Z - Box.ExtentCm.Z < 0.01;
		const bool bEnd = Box.CentreCm.X - Box.ExtentCm.X < 0.01
			|| Box.CentreCm.X + Box.ExtentCm.X > Spec.BricksPerCourse * (21.5 + 1.0) - 1.5;
		if (bBottom && !bEnd && S.RemovePiece(Piece))
		{
			++Removed;
		}
	}

	TestTrue(TEXT("fixture: the cut must remove something"), Removed > 0);

	const int32 PassesAfterCut = S.SolveAndBreak();
	const FStructure::FSolveAndBreakReport& AfterCut = S.GetLastSolveAndBreakReport();

	int32 IntactNow = 0;
	int32 SeveredNow = 0;
	for (int32 J = 0; J < S.NumConnections(); ++J)
	{
		if (S.GetConnection(J).HasGiven())
		{
			++SeveredNow;
		}
		else
		{
			++IntactNow;
		}
	}

	TestEqual(TEXT("the report's breaking passes are the return value"), AfterCut.BreakingPasses, PassesAfterCut);
	TestEqual(TEXT("the report lists every breaking pass and the terminal one"),
		AfterCut.Passes.Num(), PassesAfterCut + 1);
	TestEqual(TEXT("live pieces before the cascade"), AfterCut.LivePiecesBefore, S.NumLivePieces());
	TestEqual(TEXT("intact joints after the cascade"), AfterCut.IntactJointsAfter, IntactNow);

	/*
	 * Pass numbers are global stamps; nothing gave as built, so this call starts at 1. Each joint
	 * gives to exactly one authority (gate, sweep or prover), so the three counts sum to the total.
	 */
	int32 SeveredPerReport = 0;
	for (int32 Index = 0; Index < AfterCut.Passes.Num(); ++Index)
	{
		const FStructure::FBreakPassReport& Pass = AfterCut.Passes[Index];
		const int32 SeveredThisPass =
			Pass.JointsSeveredByGate + Pass.JointsGivenToSweep + Pass.JointsSeveredByProver;
		SeveredPerReport += SeveredThisPass;

		const bool bTerminal = Index == AfterCut.Passes.Num() - 1;
		TestEqual(
			*FString::Printf(TEXT("pass %d of the report is stamped in sequence"), Index),
			Pass.Pass, Index + 1);
		if (bTerminal)
		{
			TestEqual(TEXT("the terminal pass severs nothing"), SeveredThisPass, 0);
		}
		else
		{
			TestTrue(TEXT("a breaking pass severs at least one joint"), SeveredThisPass > 0);
		}
	}

	/*
	 * RemovePiece severs its joints before SolveAndBreak runs, so they appear in the structure's
	 * count but in no pass. The passes account for IntactJointsBefore - IntactJointsAfter.
	 */
	TestEqual(TEXT("the report's severed joints sum to the joints the cascade severed"),
		SeveredPerReport, AfterCut.IntactJointsBefore - AfterCut.IntactJointsAfter);
	TestEqual(TEXT("and the cascade's joints plus the cut's are the structure's severed count"),
		SeveredPerReport + (S.NumConnections() - AfterCut.IntactJointsBefore), SeveredNow);

	/*
	 * Below the 200-block gate cap, every cascade sever is stamped by the gate (code 1). Intact and
	 * cut-severed joints are unstamped (INDEX_NONE).
	 */
	int32 GateStamped = 0;
	int32 SweepStamped = 0;
	int32 ProverStamped = 0;
	int32 Unstamped = 0;
	for (int32 J = 0; J < S.NumConnections(); ++J)
	{
		switch (S.GetBreakAuthority(J))
		{
		case 1: ++GateStamped; break;
		case 2: ++SweepStamped; break;
		case 3: ++ProverStamped; break;
		default: ++Unstamped; break;
		}
	}

	const int32 CutSevered = S.NumConnections() - AfterCut.IntactJointsBefore;

	TestEqual(TEXT("below the cap, the gate authored every cascade sever"), GateStamped, SeveredPerReport);
	TestEqual(TEXT("no joint was given to the sweep below the cap"), SweepStamped, 0);
	TestEqual(TEXT("no joint was given to the prover below the cap"), ProverStamped, 0);
	TestEqual(TEXT("the unstamped joints are exactly the intact ones and the cut's own"),
		Unstamped, IntactNow + CutSevered);

	/*
	 * Below the cap the prover never runs, so every pass has zero regional poses and an empty
	 * breakdown (the converse of the cascade-seam test's one-entry-per-pose check).
	 */
	for (int32 Index = 0; Index < AfterCut.Passes.Num(); ++Index)
	{
		const FStructure::FBreakPassReport& Pass = AfterCut.Passes[Index];
		TestEqual(
			*FString::Printf(TEXT("below the cap, pass %d poses no prover LP"), Index),
			Pass.RegionalPoses, 0);
		TestEqual(
			*FString::Printf(TEXT("below the cap, pass %d has an empty per-pose breakdown"), Index),
			Pass.RegionalPoseBreakdown.Num(), 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
