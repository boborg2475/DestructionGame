// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "HAL/PlatformTime.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"
#include "Tests/CorbelCaseTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, not anonymous, and named for what it holds. An anonymous namespace is private
 * to a TRANSLATION UNIT rather than to a file, and a unity build merges many files into one — at
 * which point two anonymous namespaces in the blob are the SAME namespace and identically-named
 * helpers in files that never refer to each other are a hard compile error.
 */
namespace StructureCorbelLimitTestSupport
{
	using namespace CorbelCaseTestSupport;

	/** Everything one built structure's root joint says, in one value a test can compare. */
	struct FRootReading
	{
		bool bBuilt = false;

		double Utilisation = 0.0;

		/** What the joint carries, in brick weights and brick-weight-centimetres of THIS spec. */
		double ForceBrickWeights = 0.0;
		double MomentBrickWeightCm = 0.0;

		/** e = |M|/|F|, cm. The statical lever arm, which is what bounds the credited depth. */
		double EffectiveArmCm = 0.0;

		/** What the solver credited, cm, and how much masonry actually stands over the joint. */
		double CreditedDepthCm = 0.0;
		double MasonryStandingOverItCm = 0.0;

		/** How far the arm's tip reaches past the base's outer face, cm. */
		double ProjectionCm = 0.0;

		int32 PieceCount = 0;

		FCorbelJointReading Axes;
	};

	/**
	 * BUILD ONE, SOLVE IT, AND READ ITS ROOT JOINT. `SolveLoads` and never `SolveAndBreak`,
	 * because everything in E, G, H and I is a claim about what a joint READS rather than about
	 * what a cascade does to it — and solving is non-destructive, so the same structure can be
	 * asked twice and answer the same.
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

	/** The same, when the structure itself is not wanted afterwards. */
	inline FRootReading CorbelReadRoot(const FCorbelSpec& Spec)
	{
		FCorbelStructure Built;

		return CorbelReadRoot(Spec, Built);
	}

	/** One line describing what a root joint is doing, printed for every row of every sweep. */
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
 * HOW MANY STEPS A CORBEL TAKES BEFORE TENSION WINS, AND WHETHER MASONRY OPPOSITE BUYS ANY.
 *
 * TEST E of `claude_plans/CORBEL_CASES_EF.html`, and its third row is a PREDICTED DEFECT rather
 * than a guard. The user's prediction, in their words: the model reads C and D identically,
 * because the counterweight carries its own weight down its own columns to the ground and none
 * of it passes through the corbel's root joint — same axial compression, same section, same
 * reading. If that holds, "more bricks opposite" buys nothing, and it is the third face of the
 * downward-only routing defect after the free end and the jamb reveal.
 *
 * =========================================================================================
 * WHY A BISECTION RATHER THAN A TABLE OF STEP COUNTS
 * =========================================================================================
 *
 * A table answers "does 35 fail" and a crossover answers "where does it fail", and only the
 * second can be COMPARED BETWEEN TWO STRUCTURES. A fixed table would also make the answer an
 * artefact of which counts somebody happened to try: pick 30 and 40 and the crossover is "between
 * 30 and 40" for any rule that puts it anywhere in there. The bisection returns the SMALLEST step
 * count over capacity, exactly, and the monotonicity row below is what entitles it to.
 *
 * =========================================================================================
 * WHAT IS ASSERTED, AND WHY EACH FORM WAS CHOSEN
 * =========================================================================================
 *
 *   - A CROSSOVER EXISTS BELOW A HUNDRED STEPS. Reach grows the moment faster than depth grows
 *     the section, so the reading must cross 1.0 somewhere; without this row nothing here has a
 *     limit at all, and a depth rule that credited too much would make every overhang stand.
 *
 *   - THE READING IS MONOTONIC IN STEP COUNT. Adding a step adds mass outboard of the root and
 *     may never lower the reading. This is the same one-sided property `ACorbelResistsWithItsWholeDepth`
 *     PART 1B states for wall height, said about the CUT instead, and it is what makes the
 *     bisection well defined rather than a guess at a step function's shape.
 *
 *   - AND D'S CROSSOVER IS STRICTLY LATER THAN C'S. Masonry behind the root should put weight on
 *     the back of the joint, which is what closes it. THIS IS THE ROW EXPECTED TO FAIL AND ITS
 *     FAILURE IS THE FINDING; it is written as a strict inequality on integers, so it cannot pass
 *     by a tolerance.
 *
 * DERIVED BEFORE IT WAS MEASURED, so that a disagreement is visible — RE-DERIVED AT THE MEAN
 * BASIS 2026-08-13. Taking the raking corbel's own closed forms — F(s) = 1 + s + s(s+1)/4 brick
 * weights and M(s) = 5.625(s+1) + 11.25*SUM F — a k-step arm's root carries M(k-1) against a
 * section of t*(7.5k)^2/6 in TENSION and F(k-1) against its 105.0625 cm2 patch in COMPRESSION.
 * On the characteristic basis tension crossed first, at k = 36 (0.99029 at 35, 1.01625 at 36);
 * at the mean f_x1 = 0.70 the tension ladder divides by 7 and does not reach 1.0 until past
 * k ~ 250, while the compression ladder — against the UNMOVED 10 MPa — crosses first:
 * F(k-1) * 2667.198625 / (105.0625e4 * 10) = 1 at F ~ 3939 brick weights, i.e. k ~ 124
 * (F(122) = 3874.5 reads 0.983, F(123) = 3937 reads 0.99937, F(124) = 4000 reads 1.0154 — so
 * the closed form puts the crossover at 125 steps, knife-edge at 124; the bisection is the
 * measurement and the green phase pins what it returns). The crossover the test finds is
 * therefore a CRUSHING limit now, and the test's name keeps its history rather than its axis.
 * The lambda cap is NOT expected to fire on any row here — e ~ 3.75k for a half-cell step, so
 * lambda*e is about 1.73 times the arm's own depth against a wall that stops at its top — which
 * is COMPOSITE_DEPTH_DESIGN's matched-corbel lemma, and it is printed per row rather than
 * assumed.
 *
 * NEEDS A TICKING WORLD: NO. FStructure is arithmetic over a graph and the fixture is boxes.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureCorbelCrossoverTest,
	"DestructionGame.Core.Structure.CorbelStepsBeforeTensionWins",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureCorbelCrossoverTest::RunTest(const FString& Parameters)
{
	using namespace StructureCorbelLimitTestSupport;

	/*
	 * THE EXPECTED NUMBERS ARE RATIOS OF PUBLISHED STRENGTHS, so they mean what they say only
	 * while the profile still carries the figures they were derived against. Asserted rather
	 * than imported: a test that read the profile would agree with a wrong profile.
	 */
	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against mean f_x1 = 0.70 MPa (re-anchor 2026-08-13), the profile carries %g"),
			GeneralPurposeMortar.TensileStrengthMPa),
		GeneralPurposeMortar.TensileStrengthMPa == 0.70);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against clay brick at 1.9 g/cm3, the profile carries %g"),
			ClayBrick.DensityGramsPerCubicCm),
		ClayBrick.DensityGramsPerCubicCm == CorbelBrickDensityGramsPerCubicCm);

	/*
	 * THE FIXTURE MUST REPRODUCE THE PICTURE THE USER REVIEWED, BRICK FOR BRICK.
	 *
	 * CORBEL_CASES.html builds case C's ten-step fill as `2 + floor(s/2)` cells on an alternating
	 * half-cell bond; this fixture builds it as an outer face advancing one step per course with
	 * as many whole cells inboard as fit. The two are the same statement only while the step IS
	 * half a cell, which is the whole reason the second spelling exists — and if they had drifted,
	 * every row below would be about a structure nobody agreed to.
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

	/*
	 * AND D IS C PLUS THREE CELLS OPPOSITE AND NOTHING ELSE. If the two root joints sat at
	 * different X the comparison below would be between two different corbels and the answer
	 * would mean nothing.
	 */
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

	/*
	 * 150 rather than the original 100: at mean strengths the worst-axis crossover is the
	 * ~124-step CRUSHING limit (see the header), and a 100-step cap would sit under it and
	 * report that no crossover exists at all.
	 */
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

		/*
		 * THE LADDER FIRST, THEN THE BISECTION OVER IT. The ladder is what the monotonicity claim
		 * is made on and it doubles as the bisection's cache, so the sweep pays for each step
		 * count once however many times it is asked about.
		 */
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

		/*
		 * THE SMALLEST STEP COUNT OVER CAPACITY, BY BISECTION. The invariant is that Low is at or
		 * under capacity and High is over it; a one-step corbel is a single half-seated brick and
		 * is nowhere near, which is the base case and is checked rather than assumed.
		 */
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
	 * AND THE ROW THE TEST EXISTS FOR.
	 *
	 * Masonry behind the root puts weight on the back of the joint, which is what closes it —
	 * in this model that is the axial compression term, the same one that separated acceptance
	 * cases 15 and 16 by a factor of 32. If C and D cross at the same step count then that weight
	 * never reaches the joint at all: it has gone down its own columns to the ground, which is
	 * the routing the free end and the jamb reveal already fail on.
	 *
	 * STRICT, AND ON INTEGERS, so it cannot pass by a tolerance and cannot pass by the two being
	 * equal. One step later would be enough to prove the counterweight is read at all.
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
 * A GIANT CORBEL MUST COME DOWN, BY A MARGIN — AND SINCE THE 2026-08-14 MEAN RE-ANCHOR FLIP IT
 * COMES DOWN BY CRUSHING ITS ROOT, NOT BY OPENING IT.
 *
 * TEST F. The guarantee at the far end of the same curve E walks: seventeen metres of overhang
 * off a one-metre base, with the counterweight in place. The point is not the number but that no
 * future change to the depth rule can quietly make this stand — every candidate bound proposed so
 * far has been checked against a case that must fail, and this is the one absurd enough that
 * failing it is unarguable.
 *
 * WHY 150 STEPS AND NOT THE ORIGINAL 100. At the mean f_x1 = 0.70 the hundred-step tension
 * reading falls to 2.6805 / 7 = 0.383 while its compression stays at 0.654 — BOTH under
 * capacity, so the hundred-step corbel would STAND and the family's only far-above-1.0 guarantee
 * would be gone. The first axis to cross capacity as steps grow is now COMPRESSION (the unmoved
 * 10 MPa), at ~124 steps by the closed form below, so the fixture grows to 150 where crushing
 * condemns it with a real margin. That is honest physics — a seventeen-metre corbel crushes the
 * mortar at its root — and it keeps the must-fail guarantee alive without touching a strength.
 *
 * =========================================================================================
 * WHAT IS ASSERTED, AND WHY EACH FORM WAS CHOSEN
 * =========================================================================================
 *
 *   - OVER CAPACITY BY A MARGIN, NOT MARGINALLY. Asserted at 1.2, against a derived 1.457, so a
 *     modest crediting error still fails here. A bare `> 1.0` would be one tuning away from
 *     flipping.
 *
 *   - AND IT FAILS BY CRUSHING, WITH THE AXIS NAMED. `ComputeUtilisation` returns the WORST of
 *     three axes, so a row that only checked the number could pass on the wrong mechanism. The
 *     root carries F(149) = 5737.5 brick weights through a 105 cm2 patch — 14.57 MPa against the
 *     10 MPa mortar, 1.457 of capacity — while the composite-relieved tension reads 0.569
 *     against the mean 0.70 basis. Compression is asserted to be strictly the largest of the
 *     three, and the moment is still asserted to be about the axis ACROSS the wall.
 *     (On the retired characteristic basis tension governed — 3.98 here, 2.68 at a hundred
 *     steps — which is why this block is red until the profile rows flip.)
 *
 *   - AND THE OUTCOME, NEVER A DISPLACEMENT. Two pieces can sever and stay resting exactly where
 *     they were. What is asserted is that the TIP — the outermost brick of the top course, the
 *     mass hanging furthest out over nothing — has lost its path to the ground, and that the root
 *     joint is one of the joints that failed UNDER LOAD rather than one that went with a removed
 *     piece. `GetBreakPass`'s contract spells that encoding out.
 *
 * DERIVED BEFORE IT WAS MEASURED, at 150 steps. F(149) = 1 + 149 + 149*150/4 = 5737.5 brick
 * weights, so sigma_n = 5737.5 * 2667.198625 / (105.0625 * 10^4) = 14.566 MPa and the
 * compression axis reads 1.457. The crushing crossover: F(k) * 2667.198625 / 1.050625e6 = 10
 * at F(k) ~ 3939 brick weights, i.e. k ~ 124. The tension side: M(149) = 5.625*150 +
 * 11.25*SUM F(0..148) = 3,227,625 brick-weight-cm against 10.25 * 1125^2 / 6 = 2,162,109.4 cm3,
 * which is 0.398 MPa = 0.569 of the mean 0.70. e = M/F = 562.6 cm, so lambda*e is far past the
 * 1125 cm arm depth: the cap does not fire, the matched-corbel lemma again.
 *
 * COST: this is the largest structure in the suite (~6,800 pieces, up from the hundred-step
 * 3,015). The piece count and the wall-clock time are both printed; if either becomes
 * unreasonable the same claim is available at 130 steps — margin ~1.1, which would need the
 * threshold lowered with it and is one retune from flapping, so shrink only under protest.
 *
 * NEEDS A TICKING WORLD: NO.
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

	/*
	 * BY A MARGIN, NOT MARGINALLY. 1.2, against a derived 1.457 on the compression axis
	 * (mean re-anchor 2026-08-13 — see the header for the arithmetic).
	 */
	TestTrue(
		FString::Printf(
			TEXT("A GIANT CORBEL MUST COME DOWN BY A MARGIN — the root joint reads %s and must be ")
			TEXT("at least 1.2, so that a modest crediting error still fails it"),
			*CorbelBits(Reading.Utilisation)),
		Reading.Utilisation >= 1.2);

	/*
	 * THE MOMENT AXIS, STILL NAMED. The moment must be about Y — the axis ACROSS the wall —
	 * because that is the one an arm projecting along X bends its seat about, and it is the
	 * one the depth is paired with. A fixture that had drifted into bending about X would be
	 * read against a completely different section modulus and would agree with none of the
	 * arithmetic above.
	 */
	const FVector MomentUuCm = Built.Structure.GetConnectionMoment(Built.RootJoint);

	TestTrue(
		FString::Printf(
			TEXT("A GIANT CORBEL: the root joint must be bent about the axis ACROSS the wall — its ")
			TEXT("moment is (%s, %s, %s) uu.cm"),
			*CorbelBits(MomentUuCm.X), *CorbelBits(MomentUuCm.Y), *CorbelBits(MomentUuCm.Z)),
		FMath::Abs(MomentUuCm.Y) > 0.0
			&& FMath::Abs(MomentUuCm.X) <= 1.0e-9 * FMath::Abs(MomentUuCm.Y)
			&& FMath::Abs(MomentUuCm.Z) <= 1.0e-9 * FMath::Abs(MomentUuCm.Y));

	/*
	 * AND IT FAILS BY CRUSHING, WITH THE AXIS NAMED. At mean strengths the composite-relieved
	 * tension reads 0.569 while 5737.5 brick weights through one 105 cm2 patch read 1.457 of
	 * the unmoved 10 MPa — the root CRUSHES. (Red until the profile flips: on the
	 * characteristic basis tension still governs at 3.98.)
	 */
	TestTrue(
		FString::Printf(
			TEXT("A GIANT CORBEL MUST FAIL BY CRUSHING — compression must be STRICTLY the worst of ")
			TEXT("the three axes: tension %s, compression %s, shear %s"),
			*CorbelBits(Reading.Axes.TensionUtilisation),
			*CorbelBits(Reading.Axes.CompressionUtilisation),
			*CorbelBits(Reading.Axes.ShearUtilisation)),
		Reading.Axes.CompressionUtilisation > Reading.Axes.TensionUtilisation
			&& Reading.Axes.CompressionUtilisation > Reading.Axes.ShearUtilisation);

	/*
	 * AND THE ORACLE MUST AGREE WITH THE SOLVER ON WHICH NUMBER THAT IS. Without this the axis
	 * claim above would be a statement about this file's own arithmetic rather than about what
	 * `GetConnectionUtilisation` returned. Two per cent.
	 */
	TestTrue(
		FString::Printf(
			TEXT("A GIANT CORBEL: the compression the section arithmetic predicts (%s) must be what ")
			TEXT("the joint actually reads (%s)"),
			*CorbelBits(Reading.Axes.CompressionUtilisation), *CorbelBits(Reading.Utilisation)),
		FMath::Abs(Reading.Axes.CompressionUtilisation - Reading.Utilisation)
			<= 0.02 * Reading.Utilisation);

	// --- and the outcome ---------------------------------------------------------------------

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
 * THE SAME CORBEL AT HALF AND DOUBLE SIZE READS HALF AND DOUBLE.
 *
 * TEST G. `COMPOSITE_DEPTH_DESIGN.md`'s scale section claims the bound is COVARIANT: `e` and the
 * credited depth `D` are both LENGTHS, so both scale with the structure, while `M ~ k^4`,
 * `F ~ k^3` and `W_c ~ k^3` — leaving `sigma ~ k`. Stress therefore grows LINEARLY with size,
 * which is Galileo's square-cube law and is CORRECT rather than a defect: a cathedral built to a
 * cottage's proportions really does fall down.
 *
 * =========================================================================================
 * WHY THE RATIO AND NOT THE VALUE
 * =========================================================================================
 *
 * A rule that only works at 21.5 x 10.25 x 6.5 cm is a tuned constant in disguise, and asserting
 * the VALUE at three scales would pin three tuned constants instead of one. The ratio is the
 * claim: the reading at double size must be exactly twice the reading at unit size, and at half
 * size exactly half, whatever those readings happen to be. It survives a change of lambda, a
 * change of section rule and a retune of f_xk1, and it fails immediately for any length that got
 * into the arithmetic as an absolute.
 *
 * HALF AND DOUBLE RATHER THAN ARBITRARY FACTORS, and the reason is floating point rather than
 * taste. 0.5 and 2.0 are exact in binary and every brick dimension in this project has an exact
 * double representation, so every length, mass, moment and section in the scaled structures is
 * the unit one times an exact power of two — the summations run in the same order and the ratio
 * comes out to the last few bits rather than to a tolerance. A factor of 1.3 would make this a
 * question about rounding.
 *
 * THE CREDITED DEPTH IS ASSERTED SEPARATELY AND FIRST, because it is the quantity the design
 * makes the covariance claim about and it is the one that could be wrong on its own: a bound with
 * an absolute length in it would still produce a linear reading over some range while quietly
 * crediting the wrong section.
 *
 * NEEDS A TICKING WORLD: NO.
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

	/*
	 * THE PIECE COUNT MUST NOT MOVE. Scaling every length scales the grid with it, so the same
	 * spec lays the same bricks in the same places, only bigger. A structure that gained or lost
	 * a brick would make every ratio below a comparison between two different corbels.
	 */
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

		/*
		 * A LENGTH BEHAVING LIKE A LENGTH. The credited depth, the effective arm and the
		 * projection are all lengths and all three must scale by exactly the factor.
		 */
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

		/*
		 * AND THE READING GROWS LINEARLY WITH SIZE. `M ~ k^4` over `W ~ k^3`, so a wall twice the
		 * size is twice as stressed. This is the row that would go red for any absolute length
		 * hidden in the rule.
		 */
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
 * THE SAME CORBEL IN THREE MATERIALS ORDERS ITSELF BY THE PROFILE NUMBERS.
 *
 * TEST H. Adding a material must stay adding NUMBERS rather than adding code — DESIGN.md §2's
 * "materials and connection types are data, not code" — so the claim is an identity rather than
 * three expected values: where a bed joint fails by opening, its reading is the same stress over
 * each profile's own f_xk1, and `utilisation * TensileStrengthMPa` is therefore the SAME NUMBER
 * for all three. Any per-material branch anywhere in the path breaks that immediately.
 *
 * =========================================================================================
 * WHY THIS NEEDED A NEW PROFILE ROW, AND WHY IT COULD NOT BE `DryStone`
 * =========================================================================================
 *
 * `COMPOSITE_DEPTH_DESIGN.md` slice 5 wants a wall with NO BOND to get little or no composite
 * depth — you cannot corbel a dry-stone wall — and derives that from cohesion and friction with
 * no per-material branch. But `DryStone.TensileStrengthMPa` is an EXACT zero, so any tension at
 * all has already gone at ANY section modulus: a dry-laid corbel is condemned identically whether
 * the depth rule credits eleven courses, one course, or has not been written. No existing fixture
 * can tell those apart. `CohesionlessBond` is `DryStone` with that one blinding field changed —
 * zero cohesion so the frictional behaviour is real, a small tensile bond so the reading is a
 * number. It is data: one row in `ConnectionProfiles.cpp`, precedented by `Unbreakable`.
 *
 * 0.40 SITS BETWEEN LIME'S 0.20 AND CEMENT'S 0.70 DELIBERATELY (re-placed at the 2026-08-13
 * mean re-anchor — it was 0.08 between the characteristic 0.05 and 0.10; the property is the
 * POSITION, not the number). A row at either end could be satisfied by a model that ignored the
 * profile and simply read them in the order they were laid; a row in the MIDDLE cannot.
 *
 * WHAT IS DELIBERATELY NOT ASSERTED: that the frictional row comes DOWN. That is slice 5's shear
 * transfer gate, which has not been built, and asserting an outcome that depends on an unwritten
 * rule would pin whichever way it happens to fall today. What the gate WOULD say is printed
 * beside the measurement instead.
 *
 * NEEDS A TICKING WORLD: NO.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureCorbelMaterialsTest,
	"DestructionGame.Core.Structure.ACorbelOrdersByItsProfileNumbers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureCorbelMaterialsTest::RunTest(const FString& Parameters)
{
	using namespace StructureCorbelLimitTestSupport;

	/*
	 * THE FIXTURE'S OWN PREMISE, ASSERTED RATHER THAN IMPORTED. Three profiles with three
	 * DIFFERENT tensile strengths, in a known order, and dry stone still at an exact zero — which
	 * is the whole reason the third row is not dry stone.
	 */
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
		 * THE JOINT MUST BE FAILING BY OPENING IN ALL THREE, or the identity below is a comparison
		 * between two axes. `ComputeUtilisation` returns the WORST of three, and these three
		 * profiles have compressive strengths of 10, 30 and 2 MPa — a fifteen-fold spread — so
		 * "lime reads twice cement" would be quietly false the moment lime's 2 MPa let compression
		 * take over.
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

		/*
		 * AND THE CREDITED DEPTH MUST BE THE SAME IN ALL THREE. The depth is a fact about the
		 * geometry and the load path, not about the binder, so a rule that had grown a per-material
		 * term would show up here rather than being absorbed into the reading.
		 */
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

	/*
	 * THE IDENTITY. One stress, three strengths — so the product is one number three times, and
	 * "adding a material is adding numbers" is exactly that statement.
	 */
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

	/*
	 * AND THE ORDER ITSELF, STATED AS AN ORDER. The identity above would still hold if all three
	 * read zero; this is what refuses that.
	 */
	TestTrue(
		FString::Printf(
			TEXT("A WEAKER BOND MUST READ HIGHER — cement %s, cohesionless %s, lime %s"),
			*CorbelBits(Readings[0]), *CorbelBits(Readings[1]), *CorbelBits(Readings[2])),
		Readings[0] > 0.0 && Readings[1] > Readings[0] && Readings[2] > Readings[1]);

	return true;
}

/**
 * A BIGGER STEP READS HIGHER, AUTOMATICALLY, BECAUSE THE ARM IS MEASURED RATHER THAN ASSUMED.
 *
 * TEST I, and the only genuinely emergent property in the whole composite-depth design. A corbel
 * that steps further per course has a longer measured lever arm, which means a bigger moment and
 * a smaller credited depth, which means a higher reading — and nothing in the solver has a
 * projection term. `COMPOSITE_DEPTH_DESIGN.md`: "change the bond and `e` follows automatically,
 * because it is measured rather than assumed". A model with no projection term reads every step
 * size the same, so this is the row that refuses one.
 *
 * =========================================================================================
 * THE LADDER SPANS FROM CODE-COMPLIANT TO ABSURD, AND THAT IS THE POINT
 * =========================================================================================
 *
 * `REAL_WORLD_CHECK.md` establishes that published corbelling limits allow the lesser of one-third
 * the unit bed depth and one-half the unit height — 3.25 cm for this brick — against the 11.25 cm
 * half-cell every fixture in this project uses, which is 3.46x over. THAT EXCESS IS DELIBERATE.
 * This is a destruction game; players build reckless things and the interesting behaviour lives
 * well past where a real engineer stops guaranteeing anything. So the code limit is not a target
 * the other rows should approach — it is the SAFE ANCHOR at one end of a deliberate ladder:
 *
 *     3.25 cm    the published per-course limit    1.00x — must stand under any rule, forever
 *     5.375 cm   a quarter brick                   1.65x
 *     7.5 cm     one third of a cell               2.31x
 *     11.25 cm   half a cell                       3.46x — case 14, and every existing fixture
 *     16.125 cm  three quarters of a cell          4.96x
 *
 * `RunningBond` can emit the 11.25 cm row and no other, because it offsets alternate courses by
 * exactly half a brick pitch. The other four need a bond the layout producer cannot currently
 * produce; this fixture places its own boxes through the same `MakeInterface` door, so nothing is
 * approximated — but a scenario wanting a quarter-bond wall would need a producer change, and that
 * is worth saying rather than leaving implied.
 *
 * =========================================================================================
 * WHAT IS ASSERTED, AND WHY EACH FORM WAS CHOSEN
 * =========================================================================================
 *
 *   - STRICTLY INCREASING WITH STEP SIZE, at equal step count, at two step counts.
 *
 *   - AND BY A REAL MARGIN ACROSS THE LADDER, not the last bit. Stated end to end — the largest
 *     step must read at least twice the smallest — rather than as a per-neighbour threshold,
 *     which would have been a tuned constant picked before anything was measured. The end-to-end
 *     form is what refuses a model with no projection term at all, and it is independent of where
 *     the per-neighbour ordering happens to be tight.
 *
 *   - AND THE CODE-COMPLIANT ROW STANDS, AS AN OUTCOME. Every piece of the arm keeps a path to
 *     the ground after `SolveAndBreak`. It is the strongest "must stand" row in the project: if a
 *     3.25 cm step ever fails, the model is broken in a way no ruling can excuse.
 *
 * =========================================================================================
 * GREEN SINCE THE FLOOR LANDED — AND THIS ROW IS WHAT DROVE IT
 * =========================================================================================
 *
 * The ladder at ten steps reads 0.046620, 0.099100, 0.168695, 0.344813, 0.667037: strictly
 * increasing, every row capped by the WALL, which is what the design claims and what a bigger
 * overhang should do. Twenty steps behaves the same.
 *
 * IT WAS U-SHAPED FOR ONE DAY AND THE HISTORY IS WORTH KEEPING, because it is why the rule has
 * two terms rather than one. With the depth capped only by `lambda*e`, the ladder read 0.1876,
 * 0.1540, 0.1687, 0.3448, 0.6670 — the 3.25 cm row HIGHER than the 5.375 cm one. The two
 * smallest steps were the only rows where the ARM capped the credited depth rather than the wall
 * (37.4 cm of an available 75, and 60.2 of 75), and where the arm caps, the reading collapses to
 * `K*F^2/M = K*F/e` with F/e at 1.442 against 1.183 — the load a corbel column collects grows
 * more slowly with the step than its lever arm does, so the shallower corbel read higher.
 *
 * AND THE CONDITION IS EXACT: the arm caps a MATCHED corbel iff the step is under `22.5/lambda`
 * = 6.4954 cm, with the step count cancelling out — which is why the U-shape looked identical at
 * ten steps and twenty. That killed the design's matched-corbel lemma, which had silently assumed
 * the half-cell step. 6.4954 cm is also 2.00x the published per-course corbelling limit, so the
 * arm-only rule penalised precisely the geometries closest to code-compliant.
 *
 * THE FIX WAS THE FLOOR, NOT A WEAKER CLAIM. `D = min(above, max(h_body, lambda*e))`: the
 * corbelling body generates the moment and is bonded into one cantilevering mass, so it resists
 * with its full depth unconditionally; only the wall ABOVE the cut has to be dragged in by shear,
 * and that is what `lambda*e` bounds. For a matched corbel the floor IS the wall, so every row
 * here is read against the same section and the ordering is the moment's alone.
 *
 * THE DELIVERABLE IS THE CURVE, NOT THE VERDICTS. Where the model's own crossover sits relative to
 * published practice — as a multiple — is what says how permissive it is, and it is printed for
 * every row from the measured slope rather than searched for, because a code-compliant corbel
 * needs somewhere around a hundred and fifty courses to fail and that is not a structure worth
 * laying twenty times.
 *
 * NEEDS A TICKING WORLD: NO.
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

			/*
			 * THE SEAT THE STEP LEAVES, ARBITRATED AGAINST THE PRODUCER. A corbel stepping s cm
			 * keeps a seat (brick length - s) wide and carries its own weight s/2 outboard of that
			 * seat's centroid — which is the mechanism this whole test is about. A fixture that had
			 * drifted into presenting a different seat would agree with none of the arithmetic and
			 * nothing else would say so out loud.
			 */
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

			/*
			 * THE CODE-COMPLIANT ROW MUST STAND, AND AS AN OUTCOME RATHER THAN AS A NUMBER. A
			 * single severed joint is not a collapse and a corbel can shed one and stand; what is
			 * asserted is that every piece of the arm still reaches the ground.
			 */
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
			 * THE CREDITED DEPTH AND WHAT CAPPED IT ARE PRINTED IN THE FAILURE ITSELF, because
			 * that is where the answer is if this row goes red rather than somewhere it has to be
			 * hunted for. Where the WALL caps the depth every step size gets the same section and
			 * the ordering is the moment's alone, which is monotone; where the ARM caps it the
			 * section shrinks with the step as well and the reading is `K*F^2/M`, whose ordering
			 * is F/e's rather than M's, and F/e need not rise with the step at all.
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

		/*
		 * AND BY A REAL MARGIN END TO END. A model with no projection term reads all five the
		 * same, so the claim that refuses one is a factor rather than a strict inequality.
		 */
		TestTrue(
			FString::Printf(
				TEXT("AND BY A REAL MARGIN — at %d steps the %g cm step must read at least TWICE ")
				TEXT("the %g cm one; it reads %s against %s, x%s"),
				StepCounts[Count], StepsCm[4], StepsCm[0], *CorbelBits(Reading[4][Count]),
				*CorbelBits(Reading[0][Count]),
				*CorbelBits(Reading[4][Count] / Reading[0][Count])),
			Reading[0][Count] > 0.0 && Reading[4][Count] >= 2.0 * Reading[0][Count]);
	}

	/*
	 * THE CURVE, WHICH IS THE DELIVERABLE. The reading is very nearly linear in step count once
	 * the arm is more than a few courses tall — the moment goes as k^3 and the section as k^2 —
	 * so the crossover is estimated from the measured slope rather than searched for, and the
	 * estimate is stated as an estimate.
	 */
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
