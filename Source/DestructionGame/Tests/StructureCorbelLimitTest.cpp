// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "HAL/PlatformTime.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"
#include "Tests/CorbelCaseTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

// Named, not anonymous: a unity build merges files, so anonymous-namespace helpers would collide.
namespace StructureCorbelLimitTestSupport
{
	using namespace CorbelCaseTestSupport;

	/** Readings of one built structure's root joint. */
	struct FRootReading
	{
		bool bBuilt = false;

		double Utilisation = 0.0;

		/** Joint load in brick weights and brick-weight-cm of this spec. */
		double ForceBrickWeights = 0.0;
		double MomentBrickWeightCm = 0.0;

		/** e = |M|/|F|, cm. The lever arm that bounds the credited depth. */
		double EffectiveArmCm = 0.0;

		/** Depth the solver credited, and masonry height over the joint, cm. */
		double CreditedDepthCm = 0.0;
		double MasonryStandingOverItCm = 0.0;

		/** How far the arm's tip reaches past the base's outer face, cm. */
		double ProjectionCm = 0.0;

		int32 PieceCount = 0;

		FCorbelJointReading Axes;
	};

	/**
	 * Build, solve and read the root joint. Uses SolveLoads, not SolveAndBreak: tests E, G, H and
	 * I are about readings, not cascades.
	 */
	inline FRootReading CorbelReadRoot(const FCorbelSpec& Spec, FCorbelStructure& Built)
	{
		FRootReading Out;

		if (!CorbelBuild(Spec, Built))
		{
			return Out;
		}

		Built.Structure.SolveLoads();

		const FConnection& Root = Built.Structure.GetConnection(Built.RootJoint);

		const FVector ForceUu = Built.Structure.GetConnectionForce(Built.RootJoint);
		const FVector MomentUuCm = Built.Structure.GetConnectionMoment(Built.RootJoint);

		const double BrickWeightUu = CorbelBrickWeightUu(Spec);

		Out.bBuilt = true;
		Out.Utilisation = Built.Structure.GetConnectionUtilisation(Built.RootJoint);
		Out.ForceBrickWeights = ForceUu.Size() / BrickWeightUu;
		Out.MomentBrickWeightCm = MomentUuCm.Size() / BrickWeightUu;
		Out.EffectiveArmCm = ForceUu.Size() > 0.0 ? MomentUuCm.Size() / ForceUu.Size() : 0.0;
		Out.CreditedDepthCm = Built.Structure.GetConnectionCompositeDepthCm(Built.RootJoint);
		Out.MasonryStandingOverItCm = Spec.Steps * CorbelCoursePitchOf(Spec);
		Out.ProjectionCm = Built.ProjectionCm;
		Out.PieceCount = Built.Structure.NumPieces();

		Out.Axes = CorbelReadBedJoint(
			ForceUu, MomentUuCm, Root.InterfaceHalfExtentCm, Root.InterfaceAreaSqCm,
			Out.CreditedDepthCm, Spec.Strength);

		return Out;
	}

	/** Overload that discards the structure. */
	inline FRootReading CorbelReadRoot(const FCorbelSpec& Spec)
	{
		FCorbelStructure Built;

		return CorbelReadRoot(Spec, Built);
	}

	/** One-line description of a root reading, logged for every sweep row. */
	inline FString CorbelDescribe(const FCorbelSpec& Spec, const FRootReading& Reading)
	{
		return FString::Printf(
			TEXT("%d steps of %g cm at scale %g (%d pieces): reads %s. It carries %s brick weights ")
			TEXT("and %s brick-weight-cm, so e = %s cm; the walk credits %s cm of the %s cm ")
			TEXT("standing over it (%s), and the arm projects %s cm. Axes: tension %s, compression ")
			TEXT("%s, shear %s."),
			Spec.Steps, Spec.StepCm, Spec.Scale, Reading.PieceCount, *CorbelBits(Reading.Utilisation),
			*CorbelBits(Reading.ForceBrickWeights), *CorbelBits(Reading.MomentBrickWeightCm),
			*CorbelBits(Reading.EffectiveArmCm), *CorbelBits(Reading.CreditedDepthCm),
			*CorbelBits(Reading.MasonryStandingOverItCm),
			Reading.CreditedDepthCm < Reading.MasonryStandingOverItCm - 1.0e-9
				? TEXT("the ARM caps it")
				: TEXT("the WALL caps it"),
			*CorbelBits(Reading.ProjectionCm),
			*CorbelBits(Reading.Axes.TensionUtilisation),
			*CorbelBits(Reading.Axes.CompressionUtilisation),
			*CorbelBits(Reading.Axes.ShearUtilisation));
	}
}

/**
 * Test E (claude_plans/CORBEL_CASES_EF.html): the step count at which a corbel crosses capacity,
 * and whether masonry opposite (case D) delays it versus the bare base (case C). The D-vs-C row
 * is a predicted defect: the counterweight's weight goes down its own columns and never reaches
 * the root joint, so C and D may read identically.
 *
 * Asserted: a crossover exists within the cap; the reading is monotonic in step count (which
 * makes the bisection valid); D's crossover is strictly later than C's (integers, no tolerance).
 * A bisection gives the exact smallest failing step count, comparable between structures.
 *
 * Derived first (mean basis, 2026-08-13): F(s) = 1 + s + s(s+1)/4 brick weights. Compression now
 * crosses before tension: F(k-1) * 2667.198625 / (105.0625e4 * 10) = 1 near k ~ 124-125, while
 * tension at f_x1 = 0.70 stays under 1.0 past k ~ 250. The test name predates the axis change.
 * The lambda cap is not expected to fire (matched-corbel lemma). No world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureCorbelCrossoverTest,
	"DestructionGame.Core.Structure.CorbelStepsBeforeTensionWins",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureCorbelCrossoverTest::RunTest(const FString& Parameters)
{
	using namespace StructureCorbelLimitTestSupport;

	// Pin the profile figures the expectations were derived from; importing them would agree with a wrong profile.
	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against mean f_x1 = 0.70 MPa (re-anchor 2026-08-13), the profile carries %g"),
			GeneralPurposeMortar.TensileStrengthMPa),
		GeneralPurposeMortar.TensileStrengthMPa == 0.70);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against clay brick at 1.9 g/cm3, the profile carries %g"),
			ClayBrick.DensityGramsPerCubicCm),
		ClayBrick.DensityGramsPerCubicCm == CorbelBrickDensityGramsPerCubicCm);

	/*
	 * The fixture must match CORBEL_CASES.html's case C brick for brick (2 + floor(s/2) cells on a
	 * half-cell bond). The generalised builder agrees with that only at the half-cell step.
	 */
	{
		FCorbelStructure CaseC;

		if (CorbelBuild(CorbelCaseC(10), CaseC))
		{
			const int32 ExpectedCells[10] = { 2, 3, 3, 4, 4, 5, 5, 6, 6, 7 };

			bool bMatchesThePicture = true;

			for (int32 StepIndex = 1; StepIndex <= 10; ++StepIndex)
			{
				const int32 Course = 3 + StepIndex - 1;
				const double Offset = StepIndex % 2 == 1 ? 11.25 : 0.0;

				if (CaseC.CoursePieces[Course].Num() != ExpectedCells[StepIndex - 1])
				{
					bMatchesThePicture = false;
					AddError(FString::Printf(
						TEXT("FIXTURE: CORBEL_CASES.html gives case C's step %d %d cells, this ")
						TEXT("fixture laid %d"),
						StepIndex, ExpectedCells[StepIndex - 1],
						CaseC.CoursePieces[Course].Num()));
					continue;
				}

				for (int32 Cell = 0; Cell < ExpectedCells[StepIndex - 1]; ++Cell)
				{
					const double Expected = Offset + Cell * 22.5;
					const double Laid = CaseC.Boxes[CaseC.CoursePieces[Course][Cell]].CentreCm.X;

					if (!FMath::IsNearlyEqual(Expected, Laid, 1.0e-9))
					{
						bMatchesThePicture = false;
						AddError(FString::Printf(
							TEXT("FIXTURE: CORBEL_CASES.html puts case C's step %d cell %d at x %g, ")
							TEXT("this fixture laid it at %s"),
							StepIndex, Cell, Expected, *CorbelBits(Laid)));
					}
				}
			}

			TestTrue(
				TEXT("FIXTURE: the generalised fixture must reproduce CORBEL_CASES.html's case C "
					"exactly at the half-cell step"),
				bMatchesThePicture);

			TestTrue(
				FString::Printf(
					TEXT("FIXTURE: the base must be IMMOVABLE — all %d of its pieces grounded"),
					3 * 2),
				CaseC.Structure.GetPiece(CaseC.RootSeatPiece).bIsGrounded
					&& !CaseC.Structure.GetPiece(CaseC.RootPiece).bIsGrounded);

			TestTrue(
				TEXT("FIXTURE: the laid corbel must know where every piece and every joint is, or "
					"every moment below is silently zero"),
				CaseC.Structure.HasCompleteGeometry());

			TestEqual(
				TEXT("FIXTURE: the root joint must be a BED joint beneath the arm's lowest outer "
					"brick"),
				CaseC.Structure.GetJointRole(CaseC.RootJoint, CaseC.RootPiece),
				EJointRole::BedBeneath);
		}
		else
		{
			AddError(TEXT("FIXTURE: case C at ten steps should build"));
			return true;
		}
	}

	// D is C plus three cells opposite; both root joints must sit at the same X.
	{
		FCorbelStructure CaseC;
		FCorbelStructure CaseD;

		if (CorbelBuild(CorbelCaseC(10), CaseC) && CorbelBuild(CorbelCaseD(10), CaseD))
		{
			TestTrue(
				FString::Printf(
					TEXT("FIXTURE: C and D must present the SAME root joint at the same place — ")
					TEXT("C's seat is at x %s, D's at x %s; D has %d pieces to C's %d"),
					*CorbelBits(CaseC.Boxes[CaseC.RootSeatPiece].CentreCm.X),
					*CorbelBits(CaseD.Boxes[CaseD.RootSeatPiece].CentreCm.X),
					CaseD.Structure.NumPieces(), CaseC.Structure.NumPieces()),
				FMath::IsNearlyEqual(
					CaseC.Boxes[CaseC.RootSeatPiece].CentreCm.X,
					CaseD.Boxes[CaseD.RootSeatPiece].CentreCm.X, 1.0e-9)
					&& CaseD.Structure.NumPieces() > CaseC.Structure.NumPieces());
		}
	}

	// 150, not 100: at mean strengths the crossover is the ~124-step crushing limit.
	constexpr int32 MostStepsWorthTrying = 150;

	struct FSweep
	{
		const TCHAR* Description;
		FCorbelSpec (*Make)(int32);
	};

	const FSweep Sweeps[2] = {
		{ TEXT("CASE C, bare two-cell base"), &CorbelCaseC },
		{ TEXT("CASE D, three cells of masonry opposite"), &CorbelCaseD },
	};

	int32 Crossovers[2] = { INDEX_NONE, INDEX_NONE };

	for (int32 Which = 0; Which < 2; ++Which)
	{
		TMap<int32, double> Measured;

		auto RootUtilisation = [&](int32 Steps) -> double
		{
			if (const double* Cached = Measured.Find(Steps))
			{
				return *Cached;
			}

			const FCorbelSpec Spec = Sweeps[Which].Make(Steps);
			const FRootReading Reading = CorbelReadRoot(Spec);

			if (!Reading.bBuilt)
			{
				AddError(FString::Printf(
					TEXT("%s: FIXTURE: a %d-step corbel should build"),
					Sweeps[Which].Description, Steps));

				Measured.Add(Steps, 0.0);
				return 0.0;
			}

			AddInfo(FString::Printf(
				TEXT("%s: %s"), Sweeps[Which].Description, *CorbelDescribe(Spec, Reading)));

			Measured.Add(Steps, Reading.Utilisation);
			return Reading.Utilisation;
		};

		// Monotonicity is checked on this ladder; results are cached for the bisection.
		const int32 Ladder[] = { 1, 2, 4, 8, 16, 32, 64, MostStepsWorthTrying };

		double Previous = -1.0;
		bool bMonotonic = true;

		for (const int32 Steps : Ladder)
		{
			const double Utilisation = RootUtilisation(Steps);

			if (Utilisation < Previous)
			{
				bMonotonic = false;
				AddError(FString::Printf(
					TEXT("%s: ADDING A STEP MUST NEVER LOWER THE READING — the reading fell to %s ")
					TEXT("at %d steps"),
					Sweeps[Which].Description, *CorbelBits(Utilisation), Steps));
			}

			Previous = Utilisation;
		}

		TestTrue(
			FString::Printf(
				TEXT("%s: THE READING MUST BE MONOTONIC IN STEP COUNT — more mass outboard of the ")
				TEXT("root can only bend it harder"),
				Sweeps[Which].Description),
			bMonotonic);

		const double AtTheTop = RootUtilisation(MostStepsWorthTrying);

		TestTrue(
			FString::Printf(
				TEXT("%s: A CROSSOVER MUST EXIST BELOW %d STEPS — at %d the root joint reads %s, ")
				TEXT("and if that is under capacity nothing here has a limit at all"),
				Sweeps[Which].Description, MostStepsWorthTrying, MostStepsWorthTrying,
				*CorbelBits(AtTheTop)),
			AtTheTop > 1.0);

		if (!(AtTheTop > 1.0))
		{
			continue;
		}

		// Bisect for the smallest step count over capacity. Invariant: Low <= 1.0 < High; Low = 1 is checked.
		int32 Low = 1;
		int32 High = MostStepsWorthTrying;

		if (RootUtilisation(Low) > 1.0)
		{
			Crossovers[Which] = Low;
		}
		else
		{
			while (High - Low > 1)
			{
				const int32 Middle = Low + (High - Low) / 2;

				if (RootUtilisation(Middle) > 1.0)
				{
					High = Middle;
				}
				else
				{
					Low = Middle;
				}
			}

			Crossovers[Which] = High;
		}

		const FCorbelSpec AtCrossover = Sweeps[Which].Make(Crossovers[Which]);
		const FRootReading Reading = CorbelReadRoot(AtCrossover);

		AddInfo(FString::Printf(
			TEXT("%s: CROSSOVER AT %d STEPS. The step below reads %s and this one reads %s. The arm ")
			TEXT("projects %s cm, which is %s x the %g cm total projection published corbelling ")
			TEXT("practice allows, at %s x its per-course limit."),
			Sweeps[Which].Description, Crossovers[Which],
			*CorbelBits(RootUtilisation(Crossovers[Which] - 1)), *CorbelBits(Reading.Utilisation),
			*CorbelBits(Reading.ProjectionCm),
			*CorbelBits(Reading.ProjectionCm / CorbelCodeTotalProjectionCm),
			CorbelCodeTotalProjectionCm,
			*CorbelBits(AtCrossover.StepCm / CorbelCodeStepPerCourseCm)));
	}

	/*
	 * Masonry behind the root should add axial compression to the joint. If C and D cross at the
	 * same count, that weight went down its own columns instead. Strict, on integers.
	 */
	TestTrue(
		FString::Printf(
			TEXT("MASONRY OPPOSITE MUST BUY THE CORBEL SOMETHING — case D crosses at %d steps and ")
			TEXT("case C at %d, and D's must be STRICTLY LATER. If they are equal the counterweight ")
			TEXT("is carrying its own weight to the ground without ever reaching the root joint."),
			Crossovers[1], Crossovers[0]),
		Crossovers[0] != INDEX_NONE && Crossovers[1] != INDEX_NONE
			&& Crossovers[1] > Crossovers[0]);

	return true;
}

/**
 * Test F: a 150-step corbel (17 m of overhang, case D) must come down by a margin, by crushing
 * its root. A must-fail guard against any depth rule that credits too much.
 *
 * 150, not 100: at mean f_x1 = 0.70 the 100-step corbel reads under capacity on every axis.
 * Compression crosses first, at ~124 steps.
 *
 * Asserted: utilisation >= 1.2 (derived 1.457); compression is strictly the worst axis and the
 * moment is about Y; the tip loses its path to ground and the root joint has a break pass
 * (failed under load, not removed). Never a displacement.
 *
 * Derived: F(149) = 5737.5 brick weights, sigma_n = 5737.5 * 2667.198625 / 1.050625e6 = 14.566 MPa,
 * so 1.457 of 10 MPa. Composite-relieved tension reads 0.569 of 0.70. e = 562.6 cm, so the lambda
 * cap does not fire.
 *
 * Cost: the largest structure in the suite (~6,800 pieces). 130 steps would give only ~1.1 margin.
 * No world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureHundredStepCorbelTest,
	"DestructionGame.Core.Structure.AHundredStepCorbelMustComeDown",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureHundredStepCorbelTest::RunTest(const FString& Parameters)
{
	using namespace StructureCorbelLimitTestSupport;

	constexpr int32 Steps = 150;

	const FCorbelSpec Spec = CorbelCaseD(Steps);

	const double BuildStartedAt = FPlatformTime::Seconds();

	FCorbelStructure Built;
	const FRootReading Reading = CorbelReadRoot(Spec, Built);

	const double SolvedAt = FPlatformTime::Seconds();

	if (!Reading.bBuilt)
	{
		AddError(TEXT("FIXTURE: a hundred-step corbel on case D's base should build"));
		return true;
	}

	AddInfo(FString::Printf(TEXT("A HUNDRED STEPS: %s"), *CorbelDescribe(Spec, Reading)));

	TestTrue(
		TEXT("FIXTURE: the laid corbel must know where every piece and every joint is, or every "
			"moment below is silently zero"),
		Built.Structure.HasCompleteGeometry());

	TestEqual(
		TEXT("FIXTURE: the root must be a BED joint, or 'fails in tension on a bed joint' is a "
			"claim about something else"),
		Built.Structure.GetJointRole(Built.RootJoint, Built.RootPiece),
		EJointRole::BedBeneath);

	// 1.2 against a derived 1.457 on compression (see header).
	TestTrue(
		FString::Printf(
			TEXT("A GIANT CORBEL MUST COME DOWN BY A MARGIN — the root joint reads %s and must be ")
			TEXT("at least 1.2, so that a modest crediting error still fails it"),
			*CorbelBits(Reading.Utilisation)),
		Reading.Utilisation >= 1.2);

	// An arm projecting along X bends about Y, the axis the depth is paired with.
	const FVector MomentUuCm = Built.Structure.GetConnectionMoment(Built.RootJoint);

	TestTrue(
		FString::Printf(
			TEXT("A GIANT CORBEL: the root joint must be bent about the axis ACROSS the wall — its ")
			TEXT("moment is (%s, %s, %s) uu.cm"),
			*CorbelBits(MomentUuCm.X), *CorbelBits(MomentUuCm.Y), *CorbelBits(MomentUuCm.Z)),
		FMath::Abs(MomentUuCm.Y) > 0.0
			&& FMath::Abs(MomentUuCm.X) <= 1.0e-9 * FMath::Abs(MomentUuCm.Y)
			&& FMath::Abs(MomentUuCm.Z) <= 1.0e-9 * FMath::Abs(MomentUuCm.Y));

	// Fails by crushing: compression 1.457 against tension 0.569.
	TestTrue(
		FString::Printf(
			TEXT("A GIANT CORBEL MUST FAIL BY CRUSHING — compression must be STRICTLY the worst of ")
			TEXT("the three axes: tension %s, compression %s, shear %s"),
			*CorbelBits(Reading.Axes.TensionUtilisation),
			*CorbelBits(Reading.Axes.CompressionUtilisation),
			*CorbelBits(Reading.Axes.ShearUtilisation)),
		Reading.Axes.CompressionUtilisation > Reading.Axes.TensionUtilisation
			&& Reading.Axes.CompressionUtilisation > Reading.Axes.ShearUtilisation);

	// The test's own section arithmetic must match the solver's reading, within 2%.
	TestTrue(
		FString::Printf(
			TEXT("A GIANT CORBEL: the compression the section arithmetic predicts (%s) must be what ")
			TEXT("the joint actually reads (%s)"),
			*CorbelBits(Reading.Axes.CompressionUtilisation), *CorbelBits(Reading.Utilisation)),
		FMath::Abs(Reading.Axes.CompressionUtilisation - Reading.Utilisation)
			<= 0.02 * Reading.Utilisation);

	// The outcome.

	const double CascadeStartedAt = FPlatformTime::Seconds();

	const int32 BreakingPasses = Built.Structure.SolveAndBreak();

	const double FinishedAt = FPlatformTime::Seconds();

	AddInfo(FString::Printf(
		TEXT("A HUNDRED STEPS: %d pieces and %d joints; %.2f s to lay, %.2f s to solve, %.2f s for ")
		TEXT("%d cascade passes, %.2f s in total. Sixty steps is the fallback if that is ")
		TEXT("unreasonable."),
		Built.Structure.NumPieces(), Built.Structure.NumConnections(),
		SolvedAt - BuildStartedAt - (SolvedAt - CascadeStartedAt), CascadeStartedAt - BuildStartedAt,
		FinishedAt - CascadeStartedAt, BreakingPasses, FinishedAt - BuildStartedAt));

	AddInfo(FString::Printf(
		TEXT("A HUNDRED STEPS: %d of the %d pieces in the arm lost their path to the ground"),
		CorbelArmPiecesWithNoPath(Built), Built.ArmPieces.Num()));

	TestTrue(
		FString::Printf(
			TEXT("THE CORBEL MUST COME DOWN — the tip of it (piece %d, %s cm out over nothing) must ")
			TEXT("lose its path to the ground"),
			Built.TipPiece, *CorbelBits(Built.ProjectionCm)),
		!Built.Structure.IsPieceSupported(Built.TipPiece));

	TestTrue(
		FString::Printf(
			TEXT("and the root joint (joint %d) must be one of the joints that failed UNDER LOAD, ")
			TEXT("not one that went with a removed piece; it broke in pass %d"),
			Built.RootJoint, Built.Structure.GetBreakPass(Built.RootJoint)),
		Built.Structure.GetBreakPass(Built.RootJoint) != INDEX_NONE);

	return true;
}

/**
 * Test G: the same corbel at half and double size reads half and double. Per
 * COMPOSITE_DEPTH_DESIGN.md, e and D scale as lengths, M ~ k^4 and W ~ k^3, so sigma ~ k
 * (square-cube law). The ratio is asserted, not the value, so any absolute length in the rule
 * fails it. Factors 0.5 and 2.0 are exact in binary, so the ratio holds to the last few bits.
 * Credited depth and arm are checked as lengths first. No world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureCorbelScaleTest,
	"DestructionGame.Core.Structure.ACorbelReadsLinearlyInScale",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureCorbelScaleTest::RunTest(const FString& Parameters)
{
	using namespace StructureCorbelLimitTestSupport;

	constexpr int32 Steps = 10;

	const double Scales[3] = { 0.5, 1.0, 2.0 };

	FRootReading Readings[3];

	for (int32 Which = 0; Which < 3; ++Which)
	{
		FCorbelSpec Spec = CorbelCaseC(Steps);
		Spec.Scale = Scales[Which];

		Readings[Which] = CorbelReadRoot(Spec);

		if (!Readings[Which].bBuilt)
		{
			AddError(FString::Printf(
				TEXT("FIXTURE: case C at scale %g should build"), Scales[Which]));
			return true;
		}

		AddInfo(FString::Printf(TEXT("SCALE x%g: %s"), Scales[Which], *CorbelDescribe(Spec, Readings[Which])));
	}

	// Scaling must lay the same bricks, only bigger.
	TestTrue(
		FString::Printf(
			TEXT("FIXTURE: scaling must lay the SAME structure bigger — %d, %d and %d pieces"),
			Readings[0].PieceCount, Readings[1].PieceCount, Readings[2].PieceCount),
		Readings[0].PieceCount == Readings[1].PieceCount
			&& Readings[1].PieceCount == Readings[2].PieceCount);

	for (int32 Which = 0; Which < 3; ++Which)
	{
		if (Which == 1)
		{
			continue;
		}

		const double Factor = Scales[Which];

		// Credited depth and effective arm are lengths and scale by exactly the factor.
		TestTrue(
			FString::Printf(
				TEXT("SCALE x%g: THE CREDITED DEPTH IS A LENGTH AND MUST SCALE LIKE ONE — %s cm ")
				TEXT("against %s at unit scale, a ratio of %s"),
				Factor, *CorbelBits(Readings[Which].CreditedDepthCm),
				*CorbelBits(Readings[1].CreditedDepthCm),
				*CorbelBits(Readings[Which].CreditedDepthCm / Readings[1].CreditedDepthCm)),
			Readings[1].CreditedDepthCm > 0.0
				&& FMath::Abs(Readings[Which].CreditedDepthCm / Readings[1].CreditedDepthCm - Factor)
					<= 1.0e-12 * Factor);

		TestTrue(
			FString::Printf(
				TEXT("SCALE x%g: and so is the effective arm e = |M|/|F| that bounds it — %s cm ")
				TEXT("against %s, a ratio of %s"),
				Factor, *CorbelBits(Readings[Which].EffectiveArmCm),
				*CorbelBits(Readings[1].EffectiveArmCm),
				*CorbelBits(Readings[Which].EffectiveArmCm / Readings[1].EffectiveArmCm)),
			Readings[1].EffectiveArmCm > 0.0
				&& FMath::Abs(Readings[Which].EffectiveArmCm / Readings[1].EffectiveArmCm - Factor)
					<= 1.0e-12 * Factor);

		// M ~ k^4 over W ~ k^3, so the reading scales linearly.
		TestTrue(
			FString::Printf(
				TEXT("SCALE x%g: THE READING MUST GROW LINEARLY WITH SIZE — it reads %s against %s ")
				TEXT("at unit scale, a ratio of %s where the square-cube law demands %g"),
				Factor, *CorbelBits(Readings[Which].Utilisation),
				*CorbelBits(Readings[1].Utilisation),
				*CorbelBits(Readings[Which].Utilisation / Readings[1].Utilisation), Factor),
			Readings[1].Utilisation > 0.0
				&& FMath::Abs(Readings[Which].Utilisation / Readings[1].Utilisation - Factor)
					<= 1.0e-9 * Factor);
	}

	return true;
}

/**
 * Test H: the same corbel in three materials orders by the profile numbers (DESIGN.md §2,
 * materials are data). Where the joint fails in tension, utilisation * TensileStrengthMPa is the
 * same stress for all three; any per-material branch breaks that.
 *
 * The third row is CohesionlessBond, not DryStone: DryStone's tensile strength is exactly zero,
 * so it fails identically under any depth rule. CohesionlessBond has zero cohesion and a small
 * tensile bond, 0.40 MPa, placed between lime (0.20) and cement (0.70) so laying order alone
 * cannot explain the ordering.
 *
 * Not asserted: that the frictional row comes down. That needs the unbuilt slice 5 shear-transfer
 * gate, so it is logged instead. No world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureCorbelMaterialsTest,
	"DestructionGame.Core.Structure.ACorbelOrdersByItsProfileNumbers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureCorbelMaterialsTest::RunTest(const FString& Parameters)
{
	using namespace StructureCorbelLimitTestSupport;

	// Premise: three distinct tensile strengths in a known order, and DryStone still at zero.
	TestTrue(
		FString::Printf(
			TEXT("FIXTURE: the three profiles must have three DIFFERENT tensile strengths in a ")
			TEXT("known order — lime %g < cohesionless %g < cement %g"),
			LimeMortar.TensileStrengthMPa, CohesionlessBond.TensileStrengthMPa,
			GeneralPurposeMortar.TensileStrengthMPa),
		LimeMortar.TensileStrengthMPa < CohesionlessBond.TensileStrengthMPa
			&& CohesionlessBond.TensileStrengthMPa < GeneralPurposeMortar.TensileStrengthMPa);

	TestTrue(
		FString::Printf(
			TEXT("FIXTURE: the new row must be the one thing DryStone cannot be — zero cohesion ")
			TEXT("(%g) with a NON-ZERO tensile bond (%g), against dry stone's exact zero (%g)"),
			CohesionlessBond.ShearCohesionMPa, CohesionlessBond.TensileStrengthMPa,
			DryStone.TensileStrengthMPa),
		CohesionlessBond.ShearCohesionMPa == 0.0
			&& CohesionlessBond.TensileStrengthMPa > 0.0
			&& DryStone.TensileStrengthMPa == 0.0);

	constexpr int32 Steps = 10;

	struct FMaterialRow
	{
		const TCHAR* Name;
		const FConnectionStrength* Strength;
	};

	const FMaterialRow Rows[3] = {
		{ TEXT("GeneralPurposeMortar"), &GeneralPurposeMortar },
		{ TEXT("CohesionlessBond"), &CohesionlessBond },
		{ TEXT("LimeMortar"), &LimeMortar },
	};

	double Readings[3] = { 0.0, 0.0, 0.0 };
	double StressTimesStrength[3] = { 0.0, 0.0, 0.0 };

	for (int32 Which = 0; Which < 3; ++Which)
	{
		FCorbelSpec Spec = CorbelCaseC(Steps);
		Spec.Strength = *Rows[Which].Strength;

		FCorbelStructure Built;
		const FRootReading Reading = CorbelReadRoot(Spec, Built);

		if (!Reading.bBuilt)
		{
			AddError(FString::Printf(
				TEXT("FIXTURE: case C in %s should build"), Rows[Which].Name));
			return true;
		}

		AddInfo(FString::Printf(TEXT("%s: %s"), Rows[Which].Name, *CorbelDescribe(Spec, Reading)));

		/*
		 * Tension must govern in all three, or the identity below compares different axes.
		 * Compressive strengths span 2 to 30 MPa, so lime could switch to compression.
		 */
		TestTrue(
			FString::Printf(
				TEXT("%s: the root joint must be governed by TENSION for the ordering claim to ")
				TEXT("mean anything — tension %s, compression %s, shear %s"),
				Rows[Which].Name, *CorbelBits(Reading.Axes.TensionUtilisation),
				*CorbelBits(Reading.Axes.CompressionUtilisation),
				*CorbelBits(Reading.Axes.ShearUtilisation)),
			Reading.Axes.TensionUtilisation > Reading.Axes.CompressionUtilisation
				&& Reading.Axes.TensionUtilisation > Reading.Axes.ShearUtilisation);

		Readings[Which] = Reading.Utilisation;
		StressTimesStrength[Which] = Reading.Utilisation * Rows[Which].Strength->TensileStrengthMPa;

		AddInfo(FString::Printf(
			TEXT("%s: the walk credited %s cm of depth and the joint carries %s brick-weight-cm, ")
			TEXT("so the peak tension is %s MPa. UNASSERTED, because the shear-transfer gate that ")
			TEXT("would refuse a bondless joint its composite section has not been built: at this ")
			TEXT("section a bondless wall gets the same relief a mortared one does."),
			Rows[Which].Name, *CorbelBits(Reading.CreditedDepthCm),
			*CorbelBits(Reading.MomentBrickWeightCm),
			*CorbelBits(StressTimesStrength[Which])));
	}

	// One stress, three strengths: the product must be the same for all three.
	for (int32 Which = 1; Which < 3; ++Which)
	{
		TestTrue(
			FString::Printf(
				TEXT("THE ORDERING MUST FOLLOW THE PROFILE NUMBERS — %s reads %s against a %g MPa ")
				TEXT("bond and %s reads %s against %g, so the peak stress must be the SAME in both: ")
				TEXT("%s against %s"),
				Rows[Which].Name, *CorbelBits(Readings[Which]),
				Rows[Which].Strength->TensileStrengthMPa,
				Rows[0].Name, *CorbelBits(Readings[0]), Rows[0].Strength->TensileStrengthMPa,
				*CorbelBits(StressTimesStrength[Which]), *CorbelBits(StressTimesStrength[0])),
			StressTimesStrength[0] > 0.0
				&& FMath::Abs(StressTimesStrength[Which] - StressTimesStrength[0])
					<= 1.0e-9 * StressTimesStrength[0]);
	}

	// The explicit order; the identity alone would pass if all three read zero.
	TestTrue(
		FString::Printf(
			TEXT("A WEAKER BOND MUST READ HIGHER — cement %s, cohesionless %s, lime %s"),
			*CorbelBits(Readings[0]), *CorbelBits(Readings[1]), *CorbelBits(Readings[2])),
		Readings[0] > 0.0 && Readings[1] > Readings[0] && Readings[2] > Readings[1]);

	return true;
}

/**
 * Test I: a bigger step per course reads higher, with no projection term in the solver; the
 * measured lever arm produces it (COMPOSITE_DEPTH_DESIGN.md).
 *
 * The ladder runs from the published per-course limit (REAL_WORLD_CHECK.md: min(bed/3,
 * height/2) = 3.25 cm) to well past it, since players build recklessly:
 *
 *     3.25 cm    the published per-course limit    1.00x — must stand under any rule, forever
 *     5.375 cm   a quarter brick                   1.65x
 *     7.5 cm     one third of a cell               2.31x
 *     11.25 cm   half a cell                       3.46x — case 14, and every existing fixture
 *     16.125 cm  three quarters of a cell          4.96x
 *
 * RunningBond can only emit the 11.25 cm row; this fixture places its own boxes through
 * MakeInterface, so a quarter-bond scenario would need a producer change.
 *
 * Asserted at 10 and 20 steps: strictly increasing with step size; largest step reads at least
 * twice the smallest; the 3.25 cm corbel stands after SolveAndBreak.
 *
 * History: with depth capped only by lambda*e the ladder was U-shaped (3.25 cm read 0.1876, above
 * 5.375 cm's 0.1540), because the arm caps a matched corbel iff step < 22.5/lambda = 6.4954 cm.
 * The fix is the floor D = min(above, max(h_body, lambda*e)): the corbelling body resists with its
 * full depth. Now at ten steps: 0.046620, 0.099100, 0.168695, 0.344813, 0.667037.
 *
 * Each row's crossover is estimated from the slope and logged against published practice.
 * No world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureCorbelStepSizeTest,
	"DestructionGame.Core.Structure.ACorbelReadsItsOwnStepSize",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureCorbelStepSizeTest::RunTest(const FString& Parameters)
{
	using namespace StructureCorbelLimitTestSupport;

	TestTrue(
		FString::Printf(
			TEXT("FIXTURE: the code-compliant step for a %g x %g x %g cm brick is min(bed/3, ")
			TEXT("height/2) = %g cm, and this ladder's anchor must BE that"),
			CorbelBrickLengthCm, CorbelBrickWidthCm, CorbelBrickHeightCm,
			FMath::Min(CorbelBrickWidthCm / 3.0, CorbelBrickHeightCm / 2.0)),
		CorbelCodeStepPerCourseCm
			== FMath::Min(CorbelBrickWidthCm / 3.0, CorbelBrickHeightCm / 2.0));

	const double StepsCm[5] = { CorbelCodeStepPerCourseCm, 5.375, 7.5, 11.25, 16.125 };
	const int32 StepCounts[2] = { 10, 20 };

	double Reading[5][2] = {};
	double CreditedDepth[5][2] = {};
	double AvailableDepth[5][2] = {};
	double SlopePerStep[5] = {};

	for (int32 Which = 0; Which < 5; ++Which)
	{
		for (int32 Count = 0; Count < 2; ++Count)
		{
			FCorbelSpec Spec = CorbelCaseC(StepCounts[Count]);
			Spec.StepCm = StepsCm[Which];

			FCorbelStructure Built;
			const FRootReading Row = CorbelReadRoot(Spec, Built);

			if (!Row.bBuilt)
			{
				AddError(FString::Printf(
					TEXT("FIXTURE: a %d-step corbel stepping %g cm should build"),
					StepCounts[Count], StepsCm[Which]));
				return true;
			}

			AddInfo(FString::Printf(
				TEXT("STEP %g cm (%s x the %g cm published limit): %s"),
				StepsCm[Which], *CorbelBits(StepsCm[Which] / CorbelCodeStepPerCourseCm),
				CorbelCodeStepPerCourseCm, *CorbelDescribe(Spec, Row)));

			// A step of s cm leaves a seat (brick length - s) wide.
			const FConnection& Root = Built.Structure.GetConnection(Built.RootJoint);

			const double ExpectedSeatCm = CorbelBrickLengthCm - StepsCm[Which];

			TestTrue(
				FString::Printf(
					TEXT("STEP %g cm: the seat left by that step must be %g cm along the wall and ")
					TEXT("%g cm through it; MakeInterface emitted %g x %g (area %g cm2)"),
					StepsCm[Which], ExpectedSeatCm, CorbelBrickWidthCm,
					2.0 * Root.InterfaceHalfExtentCm.X, 2.0 * Root.InterfaceHalfExtentCm.Y,
					Root.InterfaceAreaSqCm),
				FMath::IsNearlyEqual(2.0 * Root.InterfaceHalfExtentCm.X, ExpectedSeatCm, 1.0e-9)
					&& FMath::IsNearlyEqual(
						2.0 * Root.InterfaceHalfExtentCm.Y, CorbelBrickWidthCm, 1.0e-9));

			Reading[Which][Count] = Row.Utilisation;
			CreditedDepth[Which][Count] = Row.CreditedDepthCm;
			AvailableDepth[Which][Count] = Row.MasonryStandingOverItCm;

			if (Count == 1)
			{
				SlopePerStep[Which] = Row.Utilisation / StepCounts[Count];
			}

			// The code-compliant row must stand: every arm piece still reaches the ground.
			if (Which == 0)
			{
				Built.Structure.SolveAndBreak();

				const int32 Lost = CorbelArmPiecesWithNoPath(Built);

				TestEqual(
					FString::Printf(
						TEXT("A CODE-COMPLIANT CORBEL MUST STAND — %d steps of %g cm is exactly what ")
						TEXT("published practice endorses per course, %s cm of total projection, and ")
						TEXT("all %d pieces of the arm must still reach the ground"),
						StepCounts[Count], CorbelCodeStepPerCourseCm,
						*CorbelBits(Row.ProjectionCm), Built.ArmPieces.Num()),
					Lost, 0);
			}
		}
	}

	for (int32 Count = 0; Count < 2; ++Count)
	{
		for (int32 Which = 1; Which < 5; ++Which)
		{
			/*
			 * The message names what capped the depth: a wall cap gives a monotone ordering, an
			 * arm cap reads K*F^2/M, which need not rise with the step.
			 */
			TestTrue(
				FString::Printf(
					TEXT("A BIGGER STEP MUST READ HIGHER — at %d steps, %g cm reads %s over %s cm ")
					TEXT("of credited depth (%s of %s available) and %g cm reads %s over %s cm (%s ")
					TEXT("of %s). That is x%s. Nothing in the solver has a projection term, so this ")
					TEXT("can only come from the arm it measures."),
					StepCounts[Count],
					StepsCm[Which], *CorbelBits(Reading[Which][Count]),
					*CorbelBits(CreditedDepth[Which][Count]),
					CreditedDepth[Which][Count] < AvailableDepth[Which][Count] - 1.0e-9
						? TEXT("the ARM capped it")
						: TEXT("the WALL capped it"),
					*CorbelBits(AvailableDepth[Which][Count]),
					StepsCm[Which - 1], *CorbelBits(Reading[Which - 1][Count]),
					*CorbelBits(CreditedDepth[Which - 1][Count]),
					CreditedDepth[Which - 1][Count] < AvailableDepth[Which - 1][Count] - 1.0e-9
						? TEXT("the ARM capped it")
						: TEXT("the WALL capped it"),
					*CorbelBits(AvailableDepth[Which - 1][Count]),
					*CorbelBits(Reading[Which][Count] / Reading[Which - 1][Count])),
				Reading[Which - 1][Count] > 0.0
					&& Reading[Which][Count] > Reading[Which - 1][Count]);
		}

		// A real margin end to end, which refuses a model with no projection term.
		TestTrue(
			FString::Printf(
				TEXT("AND BY A REAL MARGIN — at %d steps the %g cm step must read at least TWICE ")
				TEXT("the %g cm one; it reads %s against %s, x%s"),
				StepCounts[Count], StepsCm[4], StepsCm[0], *CorbelBits(Reading[4][Count]),
				*CorbelBits(Reading[0][Count]),
				*CorbelBits(Reading[4][Count] / Reading[0][Count])),
			Reading[0][Count] > 0.0 && Reading[4][Count] >= 2.0 * Reading[0][Count]);
	}

	// The reading is nearly linear in step count (M ~ k^3, W ~ k^2), so estimate the crossover from the slope.
	for (int32 Which = 0; Which < 5; ++Which)
	{
		const double EstimatedCrossoverSteps =
			SlopePerStep[Which] > 0.0 ? 1.0 / SlopePerStep[Which] : 0.0;

		AddInfo(FString::Printf(
			TEXT("THE CURVE: a %g cm step (%s x the %g cm per-course limit) reads %s per step, so ")
			TEXT("it crosses capacity at about %s steps — %s cm of projection, which is %s x the ")
			TEXT("%g cm total projection published practice allows. ESTIMATED FROM THE SLOPE AT %d ")
			TEXT("STEPS, not measured."),
			StepsCm[Which], *CorbelBits(StepsCm[Which] / CorbelCodeStepPerCourseCm),
			CorbelCodeStepPerCourseCm, *CorbelBits(SlopePerStep[Which]),
			*CorbelBits(EstimatedCrossoverSteps),
			*CorbelBits(EstimatedCrossoverSteps * StepsCm[Which]),
			*CorbelBits(EstimatedCrossoverSteps * StepsCm[Which] / CorbelCodeTotalProjectionCm),
			CorbelCodeTotalProjectionCm, StepCounts[1]));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
