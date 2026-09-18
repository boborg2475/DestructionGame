// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE BREAK-DECISION REPORT IS A FAITHFUL RECORD OF THE CASCADE IT DESCRIBES.
 *
 * `FStructure::GetLastSolveAndBreakReport` exists so an experiment can say how long a break decision
 * took and where the time went. A report is only worth reading if its counts agree with the cascade's
 * own answers, so this pins the agreement rather than any number: the passes it lists are the passes
 * SolveAndBreak returned plus the terminal one, its joint counts are the structure's, and a settle
 * that breaks nothing reports exactly one pass with no breaks. Wall-clock is asserted only to be
 * finite and non-negative — a threshold would flake on a shared machine.
 *
 * NEEDS A TICKING WORLD: NO.
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

	/* AS BUILT: one solve, nothing gives, and the report says exactly that. */
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

	/*
	 * A CUT THAT MAKES SOMETHING GIVE. Pull the whole bottom course out but its two end bricks, so
	 * the courses above span a hole wider than any of them can bridge; whatever the cascade decides,
	 * the report must count it the same way the structure does.
	 */
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
	 * PASS NUMBERS ARE GLOBAL STAMPS: this call continues from the highest stamp already written.
	 * Nothing gave as built, so this call's first pass is 1, and every pass after it is the next
	 * integer. A joint gives to exactly one of the three authorities — the gate below the cap, the
	 * sweep or the prover above it — so the three counts sum to the joints the structure severed.
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
	 * THE CUT ITSELF SEVERS TOO, AND IS NOT THE CASCADE'S. RemovePiece severs every joint of the
	 * piece it tombstones before SolveAndBreak is ever called, so those show up in the structure's
	 * severed count and in IntactJointsBefore, never in a pass. What the passes must account for is
	 * exactly the difference between the intact count the cascade started from and the one it ended on.
	 */
	TestEqual(TEXT("the report's severed joints sum to the joints the cascade severed"),
		SeveredPerReport, AfterCut.IntactJointsBefore - AfterCut.IntactJointsAfter);
	TestEqual(TEXT("and the cascade's joints plus the cut's are the structure's severed count"),
		SeveredPerReport + (S.NumConnections() - AfterCut.IntactJointsBefore), SeveredNow);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
