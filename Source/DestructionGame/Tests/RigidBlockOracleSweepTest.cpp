// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"
#include "Core/WallCases.h"
#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"
#include "World/DestructionScenarios.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Fixture-catalogue sweep (DESIGN.md §7): each affordable fixture is built by production's
 * producers, bridged, solved by the rigid-block LP and diffed against the production cascade
 * on the same structure. Each row pins lambda* and the relation.
 *
 * A relation is not a verdict on which is right: the oracle is rigid-plastic limit analysis
 * with one global lambda* (a local loss reads Falls), production an elastic router run to a
 * standstill. Strengths are mean-basis (TRAPS.md 2026-08-14); do not re-apply the old /6.
 * Slow rows live in OracleSweepFull. Named namespace because unity builds merge TUs.
 */
namespace RigidBlockSweepTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace RigidBlockOracle;

	// Brick constants defined locally so a wrong production constant disagrees with this file.

	constexpr double SweepBrickLengthCm = 21.5;
	constexpr double SweepBrickWidthCm = 10.25;
	constexpr double SweepBrickHeightCm = 6.5;
	constexpr double SweepClayDensityGramsPerCubicCm = 1.9;
	constexpr double SweepJointCm = 1.0;
	constexpr double SweepCoursePitchCm = SweepBrickHeightCm + SweepJointCm;

	/** Density-first multiplication order (the PieceMassKg contract); 2.72163125 kg. */
	constexpr double SweepBrickMassKg = SweepClayDensityGramsPerCubicCm
		* SweepBrickLengthCm * SweepBrickWidthCm * SweepBrickHeightCm / 1000.0;

	/** Tolerance for "the LP prices two fixtures identically". Pairs agree to ~1 ulp (2026-08-12). */
	constexpr double SameNumberRelativeTolerance = 1.0e-15;

	// Production's wall-16 / wall-15 worst-joint ratio, measured 4.5082330043756453 (mean basis).
	constexpr double SuperimposedReadingRatioLo = 4.5081;
	constexpr double SuperimposedReadingRatioHi = 4.5084;

	/** Production's 20-course / 10-course free-end ratio, measured 2.11985065259119. */
	constexpr double FreeEndReadingRatioLo = 2.119;
	constexpr double FreeEndReadingRatioHi = 2.121;

	/** How the two methods relate on one fixture. Unmeasured fails when asserted. */
	enum class ERelation : uint8
	{
		Unmeasured,

		/** lambda* >= 1 and the cascade dropped nothing. */
		AgreeStands,

		/** lambda* < 1 and the cascade dropped at least one piece. */
		AgreeFalls,

		/** The oracle finds an admissible equilibrium production's rules refuse. */
		OracleStandsProductionFalls,

		/** Production holds up a structure the limit theorem says has no equilibrium. */
		OracleFallsProductionStands,

		/** Post-solve verification fails at this scale. Canary; unused today. */
		OracleRefusesAtThisScale,

		/** Phase-2 simplex fails before any answer exists. Canary; unused today. */
		OracleRefusesPhaseTwo,
	};

	const TCHAR* RelationName(ERelation Relation)
	{
		switch (Relation)
		{
		case ERelation::AgreeStands:                 return TEXT("AGREE (stands)");
		case ERelation::AgreeFalls:                  return TEXT("AGREE (falls)");
		case ERelation::OracleStandsProductionFalls: return TEXT("ORACLE STANDS / PRODUCTION FALLS");
		case ERelation::OracleFallsProductionStands: return TEXT("ORACLE FALLS / PRODUCTION STANDS");
		case ERelation::OracleRefusesAtThisScale:    return TEXT("ORACLE REFUSES (at this scale)");
		case ERelation::OracleRefusesPhaseTwo:       return TEXT("ORACLE REFUSES (phase-2 simplex failed)");
		default:                                     return TEXT("UNMEASURED");
		}
	}

	struct FSweepRow
	{
		const TCHAR* Name = nullptr;

		/** The named mechanism behind the expected relation, printed on any failure. */
		const TCHAR* Mechanism = nullptr;

		/** Leaves the structure post-cut, pre-cascade. False is a fixture failure. */
		TFunction<bool(FStructure&, FString&)> Build;

		ERelation Expected = ERelation::Unmeasured;

		/** Inclusive lambda* window around the measured value; negative means unmeasured. */
		double LambdaLo = -1.0;
		double LambdaHi = -1.0;

		/** Production's drop count, pinned exactly. */
		int32 ProductionFallen = INDEX_NONE;

		/** Production's stranded count (a routing limit, DESIGN.md §5.1). INDEX_NONE: unasserted. */
		int32 ProductionStranded = INDEX_NONE;

		/**
		 * Pricing experiment: rewrites the oracle's problem only (production stays the control)
		 * and returns how many joints it changed, so a no-op override is not mistaken for a
		 * negative result.
		 */
		TFunction<int32(FOracleProblem&)> AdjustProblem;

		/** How many joints the override must have changed. INDEX_NONE: unasserted. */
		int32 OverriddenJoints = INDEX_NONE;
	};

	struct FSweepReading
	{
		bool bBuilt = false;
		FString BuildWhy;

		bool bBridged = false;
		FString BridgeWhy;
		int32 Blocks = 0;
		int32 Joints = 0;

		FOracleResult Oracle;
		double OracleSeconds = 0.0;

		int32 JointsOverridden = 0;

		// Production, on the same structure.
		double WorstUtilisation = 0.0;
		int32 Passes = 0;
		int32 Fallen = 0;
		int32 Stranded = 0;
	};

	/** Live pieces with no path to ground; stranded ones are also counted separately. */
	void CountFallen(const FStructure& Structure, int32& OutFallen, int32& OutStranded)
	{
		OutFallen = 0;
		OutStranded = 0;

		for (int32 Piece = 0; Piece < Structure.NumPieces(); ++Piece)
		{
			if (Structure.IsPieceRemoved(Piece))
			{
				continue;
			}

			const EPieceSupport Support = Structure.GetPieceSupport(Piece);

			if (Support != EPieceSupport::Grounded && Support != EPieceSupport::Supported)
			{
				++OutFallen;
			}

			if (Support == EPieceSupport::Stranded)
			{
				++OutStranded;
			}
		}
	}

	/** Run one fixture both ways. Oracle first, because SolveAndBreak mutates the structure. */
	void MeasureRow(const FSweepRow& Row, FSweepReading& Out)
	{
		FStructure Structure;

		Out.bBuilt = Row.Build(Structure, Out.BuildWhy);

		if (!Out.bBuilt)
		{
			return;
		}

		FOracleProblem Problem;
		Out.bBridged = BuildRigidBlockProblem(Structure, Problem, Out.BridgeWhy);

		if (Out.bBridged)
		{
			Out.Blocks = Problem.Blocks.Num();
			Out.Joints = Problem.Joints.Num();

			if (Row.AdjustProblem)
			{
				Out.JointsOverridden = Row.AdjustProblem(Problem);
			}

			const double Started = FPlatformTime::Seconds();
			Out.Oracle = SolveRigidBlock(Problem);
			Out.OracleSeconds = FPlatformTime::Seconds() - Started;
		}

		Structure.SolveLoads();

		for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
		{
			const double Utilisation = Structure.GetConnectionUtilisation(Index);

			if (FMath::IsFinite(Utilisation) && Utilisation > Out.WorstUtilisation)
			{
				Out.WorstUtilisation = Utilisation;
			}
		}

		Out.Passes = Structure.SolveAndBreak();
		CountFallen(Structure, Out.Fallen, Out.Stranded);
	}

	/** One line per fixture, logged immediately so a killed slow run keeps its readings. */
	void ReportRow(FAutomationTestBase& Test, const FSweepRow& Row, const FSweepReading& R)
	{
		// 17 digits so the tight cross-row pins can be re-derived from the log.
		const FString Line = FString::Printf(
			TEXT("SWEEP %s: lambda=%.17g answered=%d pivots=%d secs=%.3f blocks=%d joints=%d ")
			TEXT("overridden=%d ")
			TEXT("| production worstU=%.17g passes=%d fallen=%d stranded=%d | expected %s%s%s"),
			Row.Name, R.Oracle.Lambda, R.Oracle.bAnswered ? 1 : 0, R.Oracle.SimplexIterations,
			R.OracleSeconds, R.Blocks, R.Joints, R.JointsOverridden,
			R.WorstUtilisation, R.Passes, R.Fallen, R.Stranded, RelationName(Row.Expected),
			R.Oracle.WhyNot.IsEmpty() ? TEXT("") : TEXT(" | whynot: "),
			R.Oracle.WhyNot.IsEmpty() ? TEXT("") : *R.Oracle.WhyNot);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		Test.AddInfo(Line);
	}

	/** Production's half of the diff, pinned exactly; also run on refusal rows. */
	void CheckProductionCounts(
		FAutomationTestBase& Test, const FSweepRow& Row, const FSweepReading& R)
	{
		if (Row.ProductionFallen != INDEX_NONE)
		{
			Test.TestEqual(
				*FString::Printf(
					TEXT("%s: production's drop count is pinned at %d (the DropsToday ")
					TEXT("pattern) and was %d"),
					Row.Name, Row.ProductionFallen, R.Fallen),
				R.Fallen, Row.ProductionFallen);
		}

		if (Row.ProductionStranded != INDEX_NONE)
		{
			Test.TestEqual(
				*FString::Printf(
					TEXT("%s: production STRANDED %d piece(s), pinned at %d — a stranded ")
					TEXT("piece is a routing limitation, not a wall that could not hold, so ")
					TEXT("this moving is a different finding from the drop count moving"),
					Row.Name, R.Stranded, Row.ProductionStranded),
				R.Stranded, Row.ProductionStranded);
		}
	}

	/** Every assertion a measured row makes; an unmeasured row is a loud red. */
	void CheckRow(FAutomationTestBase& Test, const FSweepRow& Row, const FSweepReading& R)
	{
		if (!R.bBuilt)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: FIXTURE could not be laid: %s"), Row.Name, *R.BuildWhy));

			return;
		}

		// Also catches a Y-normal joint the 2D oracle would refuse.
		if (!Test.TestTrue(
				*FString::Printf(
					TEXT("%s: the bridge must represent this fixture (it said: %s)"),
					Row.Name, *R.BridgeWhy),
				R.bBridged))
		{
			return;
		}

		// Checked first: an override that matched nothing looks like a negative result.
		if (Row.OverriddenJoints != INDEX_NONE)
		{
			Test.TestEqual(
				*FString::Printf(
					TEXT("%s: the strength override must have rewritten %d joint(s) and ")
					TEXT("rewrote %d — an override that matched nothing measures nothing, and ")
					TEXT("looks exactly like a negative result"),
					Row.Name, Row.OverriddenJoints, R.JointsOverridden),
				R.JointsOverridden, Row.OverriddenJoints);
		}

		// A pinned refusal asserts the refusal and its reason. An answer here means: promote the row.
		if (Row.Expected == ERelation::OracleRefusesAtThisScale)
		{
			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: pinned as ORACLE REFUSES (mechanism: %s). It ANSWERED, ")
					TEXT("lambda* %.9g — the solver's envelope has moved; measure this row ")
					TEXT("and promote the pin to a relation, never delete this failure."),
					Row.Name, Row.Mechanism, R.Oracle.Lambda),
				!R.Oracle.bAnswered);

			if (!R.Oracle.bAnswered)
			{
				Test.TestTrue(
					*FString::Printf(
						TEXT("%s: the refusal must be the post-solve verification, and ")
						TEXT("was: %s"),
						Row.Name, *R.Oracle.WhyNot),
					R.Oracle.WhyNot.Contains(TEXT("failed verification")));
			}

			CheckProductionCounts(Test, Row, R);

			return;
		}

		// Separate branch: the two refusals mean opposite things about how far the solve got.
		if (Row.Expected == ERelation::OracleRefusesPhaseTwo)
		{
			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: pinned as ORACLE REFUSES IN PHASE 2 (mechanism: %s). It ")
					TEXT("ANSWERED, lambda* %.9g — the solver's numerics have moved; measure ")
					TEXT("this row and promote the pin to a relation, never delete this ")
					TEXT("failure."),
					Row.Name, Row.Mechanism, R.Oracle.Lambda),
				!R.Oracle.bAnswered);

			if (!R.Oracle.bAnswered)
			{
				Test.TestTrue(
					*FString::Printf(
						TEXT("%s: the refusal must be the phase-2 simplex, and was: %s"),
						Row.Name, *R.Oracle.WhyNot),
					R.Oracle.WhyNot.Contains(TEXT("phase-2 simplex failed")));
			}

			CheckProductionCounts(Test, Row, R);

			return;
		}

		if (!Test.TestTrue(
				*FString::Printf(
					TEXT("%s: the oracle must answer (it said: %s)"),
					Row.Name, *R.Oracle.WhyNot),
				R.Oracle.bAnswered))
		{
			return;
		}

		if (Row.Expected == ERelation::Unmeasured)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: UNMEASURED — this row has no pinned relation yet. Measured: ")
				TEXT("lambda* %.17g, production dropped %d (stranded %d, worst reading %.17g). ")
				TEXT("Pin the relation, the lambda window and the drop count."),
				Row.Name, R.Oracle.Lambda, R.Fallen, R.Stranded, R.WorstUtilisation));

			return;
		}

		const EOracleOutcome Outcome = OutcomeOf(R.Oracle);

		const bool bOracleStands = Outcome == EOracleOutcome::Stands;
		const bool bProductionStands = R.Fallen == 0;

		bool bRelationHolds = false;

		switch (Row.Expected)
		{
		case ERelation::AgreeStands:
			bRelationHolds = bOracleStands && bProductionStands;
			break;
		case ERelation::AgreeFalls:
			bRelationHolds = !bOracleStands && !bProductionStands;
			break;
		case ERelation::OracleStandsProductionFalls:
			bRelationHolds = bOracleStands && !bProductionStands;
			break;
		case ERelation::OracleFallsProductionStands:
			bRelationHolds = !bOracleStands && bProductionStands;
			break;
		default:
			break;
		}

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: the pinned relation is %s (mechanism: %s); the oracle read ")
				TEXT("lambda* %.9g (%s) and production dropped %d piece(s). If one side ")
				TEXT("changed, re-derive the classification rather than flipping it."),
				Row.Name, RelationName(Row.Expected), Row.Mechanism,
				R.Oracle.Lambda, bOracleStands ? TEXT("stands") : TEXT("falls"), R.Fallen),
			bRelationHolds);

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: lambda* must lie in [%.9g, %.9g] and was %.17g — the margin is ")
				TEXT("the measurement; a silent move is a change in one of the two models"),
				Row.Name, Row.LambdaLo, Row.LambdaHi, R.Oracle.Lambda),
			R.Oracle.Lambda >= Row.LambdaLo && R.Oracle.Lambda <= Row.LambdaHi);

		CheckProductionCounts(Test, Row, R);
	}

	/** Run every row, keeping the readings for cross-row pins. */
	void RunRows(
		FAutomationTestBase& Test,
		const TArray<FSweepRow>& Rows,
		TArray<FSweepReading>& OutReadings)
	{
		OutReadings.Reset();
		OutReadings.SetNum(Rows.Num());

		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			MeasureRow(Rows[Index], OutReadings[Index]);
			ReportRow(Test, Rows[Index], OutReadings[Index]);
			CheckRow(Test, Rows[Index], OutReadings[Index]);
		}
	}

	void RunRows(FAutomationTestBase& Test, const TArray<FSweepRow>& Rows)
	{
		TArray<FSweepReading> Unread;
		RunRows(Test, Rows, Unread);
	}

	/** A reading by row name. */
	const FSweepReading* ReadingNamed(
		FAutomationTestBase& Test,
		const TArray<FSweepRow>& Rows,
		const TArray<FSweepReading>& Readings,
		const TCHAR* Name)
	{
		for (int32 Index = 0; Index < Rows.Num() && Index < Readings.Num(); ++Index)
		{
			if (FCString::Strcmp(Rows[Index].Name, Name) == 0)
			{
				return &Readings[Index];
			}
		}

		Test.AddError(FString::Printf(
			TEXT("cross-row pin: no row named '%s' ran — a renamed row must take its ")
			TEXT("cross-row pins with it, never lose them silently"),
			Name));

		return nullptr;
	}

	/** Asserts two rows' lambda* are the same number, far tighter than either row's window. */
	void CheckSameLambda(
		FAutomationTestBase& Test,
		const TCHAR* What,
		const FSweepReading& First,
		const FSweepReading& Second,
		double RelativeTolerance)
	{
		const double Difference = FMath::Abs(First.Oracle.Lambda - Second.Oracle.Lambda);
		const double Scale = FMath::Max(FMath::Abs(First.Oracle.Lambda), 1.0);
		const double Relative = Difference / Scale;

		Test.AddInfo(FString::Printf(
			TEXT("SAME-NUMBER %s: %.17g vs %.17g, relative difference %.3g (tolerance %.3g)"),
			What, First.Oracle.Lambda, Second.Oracle.Lambda, Relative, RelativeTolerance));

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: the two lambda* must be THE SAME NUMBER to %.3g relative and read ")
				TEXT("%.17g vs %.17g (relative difference %.3g). This is a tighter claim than ")
				TEXT("either row's window on purpose — if one side moved, the identity is the ")
				TEXT("finding that broke, so re-derive it rather than widening the tolerance."),
				What, RelativeTolerance, First.Oracle.Lambda, Second.Oracle.Lambda, Relative),
			Relative <= RelativeTolerance);
	}

	/**
	 * Production's readings on a same-number pair must stay apart by the measured ratio, so
	 * two rows accidentally built from one fixture cannot pass the pin vacuously.
	 */
	void CheckReadingRatio(
		FAutomationTestBase& Test,
		const TCHAR* What,
		double Numerator,
		double Denominator,
		double RatioLo,
		double RatioHi)
	{
		const double Ratio = Denominator != 0.0
			? Numerator / Denominator
			: TNumericLimits<double>::Max();

		Test.AddInfo(FString::Printf(
			TEXT("READING RATIO %s: %.17g / %.17g = %.17g"),
			What, Numerator, Denominator, Ratio));

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: production's two readings must stay apart by a ratio in [%.9g, ")
				TEXT("%.9g] and read %.17g / %.17g = %.17g — this pins the discrimination ")
				TEXT("production DOES make, and refuses a pair that quietly became one fixture"),
				What, RatioLo, RatioHi, Numerator, Denominator, Ratio),
			Ratio >= RatioLo && Ratio <= RatioHi);
	}

	// Fixture builders. Hand-laid geometry is transcribed from the acceptance fixture it mirrors.

	/** Lay a scenario row's structure and apply its cut. */
	bool BuildScenarioStructure(const TCHAR* ScenarioName, FStructure& Out, FString& OutWhy)
	{
		using namespace DestructionScenarios;

		const int32 Index = IndexOfName(FName(ScenarioName));

		if (Index == INDEX_NONE)
		{
			OutWhy = FString::Printf(TEXT("no scenario row named %s"), ScenarioName);
			return false;
		}

		FBrickLayout Layout;
		TArray<int32> CutPieces;

		if (!Build(Catalogue()[Index], Layout, CutPieces))
		{
			OutWhy = FString::Printf(TEXT("the producer refused %s"), ScenarioName);
			return false;
		}

		for (const int32 Piece : CutPieces)
		{
			if (!Layout.Structure.RemovePiece(Piece))
			{
				OutWhy = FString::Printf(
					TEXT("%s: cut piece %d could not be removed"), ScenarioName, Piece);
				return false;
			}
		}

		Out = MoveTemp(Layout.Structure);
		return true;
	}

	/** LeaningStackAcceptanceTest's fixture: course i at (10*i, 0, 3.25 + 7.5*i), base grounded. */
	bool BuildLeaningStack(int32 Courses, FStructure& Out, FString& OutWhy)
	{
		TArray<FPieceBox> Boxes;

		for (int32 Course = 0; Course < Courses; ++Course)
		{
			FPieceBox Box;
			Box.ExtentCm = FVector(SweepBrickLengthCm, SweepBrickWidthCm, SweepBrickHeightCm) * 0.5;
			Box.CentreCm = FVector(
				double(Course) * 10.0, 0.0,
				SweepBrickHeightCm / 2.0 + double(Course) * SweepCoursePitchCm);

			Out.AddPiece(SweepBrickMassKg, /*bIsGrounded*/ Course == 0, Box.CentreCm);
			Boxes.Add(Box);
		}

		for (int32 First = 0; First < Boxes.Num(); ++First)
		{
			for (int32 Second = First + 1; Second < Boxes.Num(); ++Second)
			{
				FConnection Joint;

				if (MakeInterface(First, Boxes[First], Second, Boxes[Second],
						SweepJointCm, GeneralPurposeMortar, Joint))
				{
					Out.AddConnection(Joint);
				}
			}
		}

		if (Out.NumConnections() != Courses - 1)
		{
			OutWhy = FString::Printf(
				TEXT("stack fixture emitted %d joints for %d courses"),
				Out.NumConnections(), Courses);
			return false;
		}

		return true;
	}

	// The beam pair, transcribed from BeamAcceptanceTest.

	constexpr double BeamSectionCm = 10.0;
	constexpr double BeamSegmentLengthCm = 220.0;
	constexpr double BeamBearingLengthCm = 40.0;
	constexpr double BeamPierLengthCm = 60.0;
	constexpr double BeamPierHeightCm = 40.0;
	constexpr double BeamBlockLengthCm = 200.0;

	constexpr double BeamC24DensityGramsPerCubicCm = 0.42;

	// Mean basis, matching BeamAcceptanceTest (JCSS x1.50 on EN 338; JCSS static yield for S275).
	constexpr double BeamC24BendingMPa = 36.0;
	constexpr double BeamC24ShearMPa = 6.0;
	constexpr double BeamS275YieldMPa = 290.0;
	constexpr double BeamSteelDensityGramsPerCubicCm = 7.85;
	constexpr double BeamConcreteDensityGramsPerCubicCm = 2.4;

	/** The glue line's profile: member strengths, mu exactly 0 (a section, not an interface). */
	FConnectionStrength BeamMemberStrength(double BendingMPa, double ShearMPa)
	{
		FConnectionStrength Strength;
		Strength.CompressiveStrengthMPa = BendingMPa;
		Strength.ShearCohesionMPa = ShearMPa;
		Strength.TensileStrengthMPa = BendingMPa;
		Strength.FrictionCoefficient = 0.0;

		return Strength;
	}

	bool BuildBeam(
		double MemberDensity, double MemberBendingMPa, double MemberShearMPa,
		double BlockHeightCm, FStructure& Out, FString& OutWhy)
	{
		const double BeamCentreZCm = BeamPierHeightCm + BeamSectionCm / 2.0;
		const double BeamTopZCm = BeamPierHeightCm + BeamSectionCm;
		const double PierInnerCm = BeamSegmentLengthCm - BeamBearingLengthCm;
		const double PierCentreCm = PierInnerCm + BeamPierLengthCm / 2.0;

		TArray<FPieceBox> Boxes;

		const auto AddBox = [&Out, &Boxes](const FPieceBox& Box, double Density, bool bGrounded)
		{
			const double MassKg = Density
				* (Box.ExtentCm.X * 2.0) * (Box.ExtentCm.Y * 2.0) * (Box.ExtentCm.Z * 2.0)
				/ 1000.0;

			const int32 Handle = Out.AddPiece(MassKg, bGrounded, Box.CentreCm);
			Boxes.Add(Box);

			return Handle;
		};

		FPieceBox Pier;
		Pier.ExtentCm = FVector(BeamPierLengthCm, BeamSectionCm, BeamPierHeightCm) * 0.5;

		Pier.CentreCm = FVector(-PierCentreCm, 0.0, BeamPierHeightCm / 2.0);
		AddBox(Pier, BeamConcreteDensityGramsPerCubicCm, true);

		Pier.CentreCm = FVector(PierCentreCm, 0.0, BeamPierHeightCm / 2.0);
		AddBox(Pier, BeamConcreteDensityGramsPerCubicCm, true);

		FPieceBox Segment;
		Segment.ExtentCm = FVector(BeamSegmentLengthCm, BeamSectionCm, BeamSectionCm) * 0.5;

		Segment.CentreCm = FVector(-BeamSegmentLengthCm / 2.0, 0.0, BeamCentreZCm);
		const int32 LeftSegment = AddBox(Segment, MemberDensity, false);

		Segment.CentreCm = FVector(BeamSegmentLengthCm / 2.0, 0.0, BeamCentreZCm);
		const int32 RightSegment = AddBox(Segment, MemberDensity, false);

		FPieceBox Block;
		Block.ExtentCm = FVector(BeamBlockLengthCm, BeamSectionCm, BlockHeightCm) * 0.5;
		Block.CentreCm = FVector(0.0, 0.0, BeamTopZCm + BlockHeightCm / 2.0);
		AddBox(Block, BeamSteelDensityGramsPerCubicCm, false);

		const FConnectionStrength Member =
			BeamMemberStrength(MemberBendingMPa, MemberShearMPa);

		for (int32 First = 0; First < Boxes.Num(); ++First)
		{
			for (int32 Second = First + 1; Second < Boxes.Num(); ++Second)
			{
				const bool bIsTheMember =
					(First == LeftSegment && Second == RightSegment);

				FConnection Joint;

				if (MakeInterface(First, Boxes[First], Second, Boxes[Second],
						/*JointThicknessCm*/ 0.0, bIsTheMember ? Member : DryStone, Joint))
				{
					Out.AddConnection(Joint);
				}
			}
		}

		if (Out.NumConnections() != 5)
		{
			OutWhy = FString::Printf(
				TEXT("beam fixture emitted %d joints, wants the acceptance file's 5"),
				Out.NumConnections());
			return false;
		}

		return true;
	}

	/** One-cell dry jamming pair: grounded seat, half-seated brick, abutting neighbour on its own seat. */
	bool BuildOneCellDryPair(FStructure& Out, FString& OutWhy)
	{
		const FVector Extent =
			FVector(SweepBrickLengthCm, SweepBrickWidthCm, SweepBrickHeightCm) * 0.5;

		const FVector Centres[] = {
			FVector(-0.5, 0.0, SweepBrickHeightCm / 2.0),
			FVector(10.75, 0.0, SweepBrickHeightCm / 2.0 + SweepCoursePitchCm),
			FVector(33.25, 0.0, SweepBrickHeightCm / 2.0 + SweepCoursePitchCm),
			FVector(33.25, 0.0, SweepBrickHeightCm / 2.0),
		};
		const bool Grounded[] = { true, false, false, true };

		TArray<FPieceBox> Boxes;

		for (int32 Piece = 0; Piece < 4; ++Piece)
		{
			FPieceBox Box;
			Box.ExtentCm = Extent;
			Box.CentreCm = Centres[Piece];

			Out.AddPiece(SweepBrickMassKg, Grounded[Piece], Box.CentreCm);
			Boxes.Add(Box);
		}

		for (int32 First = 0; First < Boxes.Num(); ++First)
		{
			for (int32 Second = First + 1; Second < Boxes.Num(); ++Second)
			{
				FConnection Joint;

				if (MakeInterface(First, Boxes[First], Second, Boxes[Second],
						1.0, DryStone, Joint))
				{
					Out.AddConnection(Joint);
				}
			}
		}

		if (Out.NumConnections() != 3)
		{
			OutWhy = FString::Printf(
				TEXT("one-cell fixture emitted %d joints, wants 3"), Out.NumConnections());
			return false;
		}

		return true;
	}

	/** StructureFreeEndHeightTest's fixture: 7-cell wall, outermost grounded brick removed. */
	bool BuildFreeEnd(int32 Courses, FStructure& Out, FString& OutWhy)
	{
		using namespace DestructionWallCases;

		FWallSpec Spec;
		Spec.BrickSizeCm = FVector(SweepBrickLengthCm, SweepBrickWidthCm, SweepBrickHeightCm);
		Spec.JointThicknessCm = SweepJointCm;
		Spec.DensityGramsPerCubicCm = SweepClayDensityGramsPerCubicCm;
		Spec.CoursesHigh = Courses;
		Spec.Cells = 7;
		Spec.Bond = EWallBond::Running;
		Spec.Strength = DestructionProfiles::GeneralPurposeMortar;

		FWallLayout Laid;

		if (!Build(Spec, Laid))
		{
			OutWhy = TEXT("the wall producer refused the free-end spec");
			return false;
		}

		const FWallRegion Cut[] = { { 0, 0, -0.25, 0.25 } };

		TArray<int32> CutPieces;
		PiecesInRegions(Laid, Cut, CutPieces);

		if (CutPieces.Num() != 1)
		{
			OutWhy = FString::Printf(
				TEXT("the free-end cut named %d bricks, wants exactly 1"), CutPieces.Num());
			return false;
		}

		if (!Laid.Layout.Structure.RemovePiece(CutPieces[0]))
		{
			OutWhy = TEXT("the free-end brick could not be removed");
			return false;
		}

		Out = MoveTemp(Laid.Layout.Structure);
		return true;
	}

	// The case-21 opening family. Opening width is a per-rung parameter (case 21 is 18 cells).

	constexpr int32 LadderOpeningCourses = 3;

	constexpr int32 LadderCoverCourses = 2;

	/** Z of the cover's underside. Every bed joint below it is a jamb bed joint. */
	constexpr double LadderCoverUndersideZCm(int32 CoursesBelow)
	{
		return double(CoursesBelow + LadderOpeningCourses) * SweepCoursePitchCm;
	}

	/** A joint whose normal is Z is a bed joint; one whose normal is X is a head joint. */
	bool IsBedJoint(const FOracleJoint& Joint)
	{
		return FMath::Abs(Joint.NormalZ) > 0.5;
	}

	bool IsHeadJoint(const FOracleJoint& Joint)
	{
		return FMath::Abs(Joint.NormalX) > 0.5;
	}

	/**
	 * Zero cohesion in the jamb bed joints only. Tests case 21's hypothesis that the LP bleeds
	 * thrust to ground as bed-joint shear, where cohesion dominates (0.2 MPa vs ~0.011 MPa of
	 * friction). Returns the joints rewritten.
	 */
	int32 ZeroJambBedCohesion(int32 CoursesBelow, FOracleProblem& Problem)
	{
		int32 Touched = 0;

		for (FOracleJoint& Joint : Problem.Joints)
		{
			if (IsBedJoint(Joint) && Joint.CentreZCm < LadderCoverUndersideZCm(CoursesBelow))
			{
				Joint.Strength.ShearCohesionMPa = 0.0;
				++Touched;
			}
		}

		return Touched;
	}

	/**
	 * Scale tension in the cover's head joints only. A bending panel is linear in it; an arch
	 * (head-joint compression) ignores it, so this separates the two mechanisms.
	 */
	int32 ScaleCoverHeadTension(int32 CoursesBelow, double Factor, FOracleProblem& Problem)
	{
		int32 Touched = 0;

		for (FOracleJoint& Joint : Problem.Joints)
		{
			if (IsHeadJoint(Joint) && Joint.CentreZCm > LadderCoverUndersideZCm(CoursesBelow))
			{
				Joint.Strength.TensileStrengthMPa *= Factor;
				++Touched;
			}
		}

		return Touched;
	}

	/**
	 * Relaxed crushing cap: raised, not zeroed (zero forbids compression entirely). Finite
	 * because the oracle refuses infinity.
	 */
	constexpr double ResidualUncappedCrushingMPa = 1.0e6;

	/** Splits the jamb chain into its top two bearing courses and its run to ground. */
	constexpr double LadderChainBearingSplitZCm(int32 CoursesBelow)
	{
		return double(CoursesBelow + LadderOpeningCourses - 2) * SweepCoursePitchCm;
	}

	/**
	 * Slice 0c's residual-attribution knob: remove chosen mechanisms from jamb bed joints in a
	 * Z band [BandLo, BandHi) and re-solve. Returns the joints rewritten.
	 */
	int32 AdjustJambBedResidual(
		int32 CoursesBelow,
		bool bZeroCohesion,
		bool bZeroFriction,
		bool bRelaxCrushing,
		bool bZeroTension,
		double BandLoZCm,
		double BandHiZCm,
		FOracleProblem& Problem)
	{
		int32 Touched = 0;

		for (FOracleJoint& Joint : Problem.Joints)
		{
			if (IsBedJoint(Joint)
				&& Joint.CentreZCm < LadderCoverUndersideZCm(CoursesBelow)
				&& Joint.CentreZCm >= BandLoZCm
				&& Joint.CentreZCm < BandHiZCm)
			{
				if (bZeroCohesion)
				{
					Joint.Strength.ShearCohesionMPa = 0.0;
				}

				if (bZeroFriction)
				{
					Joint.Strength.FrictionCoefficient = 0.0;
				}

				if (bRelaxCrushing)
				{
					Joint.Strength.CompressiveStrengthMPa = ResidualUncappedCrushingMPa;
				}

				if (bZeroTension)
				{
					Joint.Strength.TensileStrengthMPa = 0.0;
				}

				++Touched;
			}
		}

		return Touched;
	}

	/**
	 * One ladder rung: case 21's wall (OpeningCells 18, CoursesBelow 1, JambCells 2) with one
	 * dimension moved. CoverTailCells is used only by the disambiguating rung (else INDEX_NONE).
	 * The ladder test pins the s=1/j=2 rung equal to Scenario("wall-21").
	 *
	 * Parity: an odd course loses one brick fewer than an even one, so an opening starting odd
	 * takes 52 bricks and one starting even 53.
	 */
	bool BuildOpeningLadderWall(
		int32 OpeningCells, int32 CoursesBelow, int32 JambCells, int32 CoverTailCells,
		FStructure& Out, FString& OutWhy)
	{
		if (CoursesBelow < 1 || JambCells < 1 || OpeningCells < 2)
		{
			OutWhy = FString::Printf(
				TEXT("a rung needs at least one course below the opening, one cell of jamb ")
				TEXT("and two cells of opening; asked for %d, %d and %d"),
				CoursesBelow, JambCells, OpeningCells);

			return false;
		}

		DestructionWallCases::FWallSpec Spec;
		Spec.BrickSizeCm = FVector(SweepBrickLengthCm, SweepBrickWidthCm, SweepBrickHeightCm);
		Spec.JointThicknessCm = SweepJointCm;
		Spec.DensityGramsPerCubicCm = SweepClayDensityGramsPerCubicCm;
		Spec.CoursesHigh = CoursesBelow + LadderOpeningCourses + LadderCoverCourses;
		Spec.Cells = OpeningCells + 2 * JambCells;
		Spec.Bond = DestructionWallCases::EWallBond::Running;
		Spec.Strength = DestructionProfiles::GeneralPurposeMortar;

		DestructionWallCases::FWallLayout Laid;

		if (!DestructionWallCases::Build(Spec, Laid))
		{
			OutWhy = FString::Printf(
				TEXT("the wall producer refused a %d-course, %d-cell ladder rung"),
				Spec.CoursesHigh, Spec.Cells);

			return false;
		}

		TArray<DestructionWallCases::FWallRegion> Cut;

		Cut.Add({ CoursesBelow,
			CoursesBelow + LadderOpeningCourses - 1,
			double(JambCells) - 0.25,
			double(JambCells + OpeningCells - 1) + 0.25 });

		/*
		 * Cover tail trim: separates wider bearing from longer cover, which widening the jamb
		 * changes together. Approximate by a half bat on odd courses.
		 */
		if (CoverTailCells > 0 && CoverTailCells < JambCells)
		{
			const int32 CoverLo = CoursesBelow + LadderOpeningCourses;
			const int32 CoverHi = CoverLo + LadderCoverCourses - 1;
			const int32 TrimCells = JambCells - CoverTailCells;

			Cut.Add({ CoverLo, CoverHi, -1.0, double(TrimCells) - 0.25 });
			Cut.Add({ CoverLo, CoverHi,
				double(Spec.Cells - TrimCells) - 0.75, double(Spec.Cells) + 1.0 });
		}

		TArray<int32> CutPieces;
		DestructionWallCases::PiecesInRegions(Laid, Cut, CutPieces);

		// Any other count means the region arithmetic and the bricklayer disagree.
		const int32 WholeCourses = LadderOpeningCourses / 2 + 1;
		const int32 HalfCourses = LadderOpeningCourses - WholeCourses;

		const int32 EvenFirst = OpeningCells * WholeCourses
			+ (OpeningCells - 1) * HalfCourses;
		const int32 OddFirst = OpeningCells * HalfCourses
			+ (OpeningCells - 1) * WholeCourses;

		int32 Wanted = (CoursesBelow % 2 == 0) ? EvenFirst : OddFirst;

		// Each end loses TrimCells off the even cover course and TrimCells + 1 off the odd.
		if (CoverTailCells > 0 && CoverTailCells < JambCells)
		{
			Wanted += 2 * (2 * (JambCells - CoverTailCells) + 1);
		}

		if (CutPieces.Num() != Wanted)
		{
			OutWhy = FString::Printf(
				TEXT("the rung's cut named %d bricks; a %s-course opening of %d cells over ")
				TEXT("%d courses (cover tail %d) wants %d"),
				CutPieces.Num(), (CoursesBelow % 2 == 0) ? TEXT("even") : TEXT("odd"),
				OpeningCells, LadderOpeningCourses, CoverTailCells, Wanted);

			return false;
		}

		for (const int32 Piece : CutPieces)
		{
			if (!Laid.Layout.Structure.RemovePiece(Piece))
			{
				OutWhy = FString::Printf(
					TEXT("the rung's cut piece %d could not be removed"), Piece);

				return false;
			}
		}

		Out = MoveTemp(Laid.Layout.Structure);
		return true;
	}

	/**
	 * Case 22's shape with variable cover depth: one grounded course, a three-course opening,
	 * CoverCourses above. Case 22 itself is 8 cover courses, 35 opening cells, 2-cell jambs.
	 */
	bool BuildCoveredOpeningWall(
		int32 CoverCourses, int32 OpeningCells, int32 JambCells,
		FStructure& Out, FString& OutWhy)
	{
		if (CoverCourses < 1 || JambCells < 1 || OpeningCells < 2)
		{
			OutWhy = FString::Printf(
				TEXT("this family needs at least one course of cover, one cell of jamb and ")
				TEXT("two cells of opening; asked for %d, %d and %d"),
				CoverCourses, JambCells, OpeningCells);

			return false;
		}

		DestructionWallCases::FWallSpec Spec;
		Spec.BrickSizeCm = FVector(SweepBrickLengthCm, SweepBrickWidthCm, SweepBrickHeightCm);
		Spec.JointThicknessCm = SweepJointCm;
		Spec.DensityGramsPerCubicCm = SweepClayDensityGramsPerCubicCm;
		Spec.CoursesHigh = 1 + LadderOpeningCourses + CoverCourses;
		Spec.Cells = OpeningCells + 2 * JambCells;
		Spec.Bond = DestructionWallCases::EWallBond::Running;
		Spec.Strength = DestructionProfiles::GeneralPurposeMortar;

		DestructionWallCases::FWallLayout Laid;

		if (!DestructionWallCases::Build(Spec, Laid))
		{
			OutWhy = FString::Printf(
				TEXT("the wall producer refused a %d-course, %d-cell wall"),
				Spec.CoursesHigh, Spec.Cells);

			return false;
		}

		TArray<DestructionWallCases::FWallRegion> Cut;

		Cut.Add({ 1, LadderOpeningCourses,
			double(JambCells) - 0.25,
			double(JambCells + OpeningCells - 1) + 0.25 });

		TArray<int32> CutPieces;
		DestructionWallCases::PiecesInRegions(Laid, Cut, CutPieces);

		const int32 Wanted = 3 * OpeningCells - 2;

		if (CutPieces.Num() != Wanted)
		{
			OutWhy = FString::Printf(
				TEXT("the cut named %d bricks; a three-course opening of %d cells starting ")
				TEXT("on an odd course wants %d"),
				CutPieces.Num(), OpeningCells, Wanted);

			return false;
		}

		for (const int32 Piece : CutPieces)
		{
			if (!Laid.Layout.Structure.RemovePiece(Piece))
			{
				OutWhy = FString::Printf(TEXT("cut piece %d could not be removed"), Piece);
				return false;
			}
		}

		Out = MoveTemp(Laid.Layout.Structure);
		return true;
	}

	/**
	 * Pins a ladder relation as a ratio of two lambda*, with the competing predictions printed.
	 * The ratio carries the physics; the per-rung windows only the arithmetic.
	 */
	void CheckLambdaLadderRatio(
		FAutomationTestBase& Test,
		const TCHAR* What,
		const TCHAR* Predictions,
		const FSweepReading& Top,
		const FSweepReading& Bottom,
		double RatioLo,
		double RatioHi)
	{
		const double Ratio = Bottom.Oracle.Lambda != 0.0
			? Top.Oracle.Lambda / Bottom.Oracle.Lambda
			: TNumericLimits<double>::Max();

		Test.AddInfo(FString::Printf(
			TEXT("LADDER %s: %.17g / %.17g = %.9g (%s)"),
			What, Top.Oracle.Lambda, Bottom.Oracle.Lambda, Ratio, Predictions));

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: the measured ratio must lie in [%.9g, %.9g] and was %.17g / ")
				TEXT("%.17g = %.9g. %s. This pin is the LADDER's finding rather than either ")
				TEXT("rung's value — if it moved, a mechanism changed, and the two ")
				TEXT("predictions beside it are what to re-read before touching the window"),
				What, RatioLo, RatioHi, Top.Oracle.Lambda, Bottom.Oracle.Lambda, Ratio,
				Predictions),
			Ratio >= RatioLo && Ratio <= RatioHi);
	}

	/**
	 * Checks a rung is the fixture its name claims, by block count. A recorded rung-flip passed
	 * both the window and the ratio pin; only this caught it (TRAPS).
	 */
	void CheckRungSize(
		FAutomationTestBase& Test,
		const TCHAR* What,
		const FSweepReading& Reading,
		int32 WantedBlocks)
	{
		Test.TestEqual(
			*FString::Printf(
				TEXT("%s: the rung must bridge to %d live blocks and bridged %d — a rung ")
				TEXT("that is not the wall its name claims measures nothing"),
				What, WantedBlocks, Reading.Blocks),
			Reading.Blocks, WantedBlocks);
	}

	/** Turns first-crack rows on and returns the bonded joint count (f_t > 0) for the row to pin. */
	int32 TurnOnFirstCrackRows(FOracleProblem& Problem)
	{
		Problem.bFirstCrackRows = true;

		int32 Bonded = 0;

		for (const FOracleJoint& Joint : Problem.Joints)
		{
			if (Joint.Strength.TensileStrengthMPa > 0.0)
			{
				++Bonded;
			}
		}

		return Bonded;
	}
}

/**
 * Leaning stack (sweep items (b), (e)). Production's worst joint reading is height-invariant
 * (0.138781067), overstating the 30-course margin ~115x; verdicts agree only because of
 * BreakOverturnedBodies (DESIGN.md §7 step 4).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockSweepLeaningStackTest,
	"DestructionGame.Oracle.RigidBlock.Sweep.LeaningStack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockSweepLeaningStackTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	TArray<FSweepRow> Rows;

	Rows.Add({ TEXT("leaning stack, 5 courses"),
		TEXT("bond holds the lean; guard leaves it alone"),
		[](FStructure& Out, FString& Why) { return BuildLeaningStack(5, Out, Why); },
		ERelation::AgreeStands, 31.2072, 31.2076, 0 });

	Rows.Add({ TEXT("leaning stack, 8 courses"),
		TEXT("bond holds the lean; guard leaves it alone"),
		[](FStructure& Out, FString& Why) { return BuildLeaningStack(8, Out, Why); },
		ERelation::AgreeStands, 8.68772, 8.68782, 0 });

	Rows.Add({ TEXT("leaning stack, 30 courses"),
		TEXT("no equilibrium at ~0.44 of gravity; production agrees via the interim guard"),
		[](FStructure& Out, FString& Why) { return BuildLeaningStack(30, Out, Why); },
		ERelation::AgreeFalls, 0.440491, 0.440495, 29, 0 });

	Rows.Add({ TEXT("leaning stack, 40 courses"),
		TEXT("no equilibrium; production agrees via the interim guard"),
		[](FStructure& Out, FString& Why) { return BuildLeaningStack(40, Out, Why); },
		ERelation::AgreeFalls, 0.241006, 0.241010, 39, 0 });

	RunRows(*this, Rows);

	return true;
}

/**
 * Beam pair (sweep item (c)). The glue line's tension bound is the member's bending strength,
 * so the oracle can express member failure; it discriminates steel from timber 6.9x. The
 * oracle reads the plastic stress block (3x first crack); for brittle timber first crack
 * remains the honest criterion.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockSweepBeamPairTest,
	"DestructionGame.Oracle.RigidBlock.Sweep.BeamPair",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockSweepBeamPairTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	TArray<FSweepRow> Rows;

	/*
	 * Production solves with first-crack rows below the 200-block cap, so the heavy C24 glue
	 * line binds at 0.88258 and falls; the oracle (flag off) still stands it.
	 */
	Rows.Add({ TEXT("C24 timber beam, heavy load"),
		TEXT("oracle (default-off, plastic) stands at 2.65; production now fells the beam ")
		TEXT("below the cap — first crack binds the bonded midspan glue line at 0.883 and the ")
		TEXT("two half-beams plus block come down"),
		[](FStructure& Out, FString& Why)
		{
			return BuildBeam(BeamC24DensityGramsPerCubicCm, BeamC24BendingMPa,
				BeamC24ShearMPa, 120.0, Out, Why);
		},
		ERelation::OracleStandsProductionFalls, 2.64609, 2.64612, 3, 0 });

	Rows.Add({ TEXT("C24 timber beam, light load"),
		TEXT("a sensibly loaded joist; production now stands it too below the cap"),
		[](FStructure& Out, FString& Why)
		{
			return BuildBeam(BeamC24DensityGramsPerCubicCm, BeamC24BendingMPa,
				BeamC24ShearMPa, 10.0, Out, Why);
		},
		ERelation::AgreeStands, 28.8010, 28.8013, 0, 0 });

	Rows.Add({ TEXT("S275 steel beam, heavy load"),
		TEXT("steel at 18.30 vs timber's 2.65: the oracle discriminates the member ")
		TEXT("material 6.9x; production stands all three below the cap"),
		[](FStructure& Out, FString& Why)
		{
			return BuildBeam(BeamSteelDensityGramsPerCubicCm, BeamS275YieldMPa,
				BeamS275YieldMPa / FMath::Sqrt(3.0), 120.0, Out, Why);
		},
		ERelation::AgreeStands, 18.2992, 18.2994, 0, 0 });

	RunRows(*this, Rows);

	return true;
}

/**
 * Corbel family (sweep item (a)): A and B here, C and D in the slow test. The oracle agrees
 * with every §8 corbel ruling; filling buys 16.4x (the filled corbel jams as a block). A and B
 * are governed differently, so no window here may be re-pinned by scaling.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockSweepCorbelFamilyTest,
	"DestructionGame.Oracle.RigidBlock.Sweep.CorbelFamily",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockSweepCorbelFamilyTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	TArray<FSweepRow> Rows;

	Rows.Add({ TEXT("corbel A, bare arm of four"),
		TEXT("a mortared 11.25 cm/course lean of four bricks; the mean bond covers it ")
		TEXT("20.3x (2.9x at the retired characteristic strength)"),
		[](FStructure& Out, FString& Why)
		{
			return BuildScenarioStructure(TEXT("corbel-a-bare-4"), Out, Why);
		},
		ERelation::AgreeStands, 20.3328, 20.3331, 0 });

	/*
	 * First crack cuts the bare arm to 0.2786 of control. That is below the single-joint 1/3
	 * floor because all four joints bind at once and the LP can no longer trade eccentricity
	 * between them. A mechanism finding, not a bug.
	 */
	Rows.Add({ TEXT("corbel A, bare arm of four — first crack"),
		TEXT("bonded bending across the four-joint arm, cut to 0.279 of control — below the ")
		TEXT("per-joint 1/3 floor by multi-joint redistribution, not per-joint net tension; ")
		TEXT("still stands 5.66x"),
		[](FStructure& Out, FString& Why)
		{
			return BuildScenarioStructure(TEXT("corbel-a-bare-4"), Out, Why);
		},
		ERelation::AgreeStands, 5.66385, 5.66390, 0, 0,
		TurnOnFirstCrackRows, 4 });

	Rows.Add({ TEXT("corbel B, filled four steps"),
		TEXT("the same reach filled solid jams as a block: 16.4x the bare arm's margin"),
		[](FStructure& Out, FString& Why)
		{
			return BuildScenarioStructure(TEXT("corbel-b-filled-4"), Out, Why);
		},
		ERelation::AgreeStands, 333.402, 333.405, 0 });

	/*
	 * Compression control, pinned as a characterisation: first crack moves corbel B by 8% because
	 * some bonded joints still reach net tension in bending.
	 */
	Rows.Add({ TEXT("corbel B, filled four steps — first crack"),
		TEXT("compression/jamming governs, so first crack moves it only 8% (0.9205 of ")
		TEXT("control) — APPROXIMATE invariance, pinned as a characterisation, NOT asserted ")
		TEXT("as unchanged"),
		[](FStructure& Out, FString& Why)
		{
			return BuildScenarioStructure(TEXT("corbel-b-filled-4"), Out, Why);
		},
		ERelation::AgreeStands, 306.883, 306.886, 0, 0,
		TurnOnFirstCrackRows, 26 });

	TArray<FSweepReading> Readings;
	RunRows(*this, Rows, Readings);

	const FSweepReading* AOff =
		ReadingNamed(*this, Rows, Readings, TEXT("corbel A, bare arm of four"));
	const FSweepReading* AOn = ReadingNamed(
		*this, Rows, Readings, TEXT("corbel A, bare arm of four — first crack"));
	const FSweepReading* BOff =
		ReadingNamed(*this, Rows, Readings, TEXT("corbel B, filled four steps"));
	const FSweepReading* BOn = ReadingNamed(
		*this, Rows, Readings, TEXT("corbel B, filled four steps — first crack"));

	if (AOff != nullptr && AOn != nullptr && BOff != nullptr && BOn != nullptr)
	{
		CheckLambdaLadderRatio(*this,
			TEXT("SLICE 0d: corbel A's four-joint arm under first crack"),
			TEXT("a single pure-bending joint reads /3 = 0.3333 and none reads below it; the ")
			TEXT("four-joint arm cuts to 0.2786 by multi-joint redistribution — the MOVE, not ")
			TEXT("either rung, is the finding"),
			*AOn, *AOff, 0.27854, 0.27858);

		CheckLambdaLadderRatio(*this,
			TEXT("SLICE 0d: corbel B's jamming block under first crack (approximate invariance)"),
			TEXT("compression governs so the prediction was ~none; measured 0.9205 — a bonded ")
			TEXT("block still writes rows that slightly bind, so the invariance is approximate"),
			*BOn, *BOff, 0.92045, 0.92047);
	}

	return true;
}

/** One-cell dry half seat with abutment: stands by edge-contact jamming at a 7 cm arm. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockSweepOneCellTest,
	"DestructionGame.Oracle.RigidBlock.Sweep.OneCellDisagreement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockSweepOneCellTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	TArray<FSweepRow> Rows;

	// Agree-stands since slice 3b/4: below the 200-block cap production's LP finds the jamming too.
	Rows.Add({ TEXT("one-cell dry half seat, with abutment"),
		TEXT("edge-contact jamming (limit theorem); production now stands both bricks below the ")
		TEXT("cap where the LP replaced the kern-and-centroid refusal"),
		[](FStructure& Out, FString& Why) { return BuildOneCellDryPair(Out, Why); },
		ERelation::AgreeStands, 9592.67, 9592.69, 0, 0 });

	RunRows(*this, Rows);

	return true;
}

/**
 * Slice 0d: with bFirstCrackRows set, the LP adds -(n1+n2) + 3|n1-n2| <= f_t*A for every bonded
 * joint only, cutting plastic bending capacity to a third (PROMOTION_DESIGN Sec 4.3/4.5).
 * Beam rows assert lambda* moves to ~control/3 (midspan N~=0); dry joints stay bit-identical.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockFirstCrackBitesTest,
	"DestructionGame.Oracle.RigidBlock.FirstCrack.BitesBondedBendingSparesDryJoints",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockFirstCrackBitesTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	// Joints the first-crack rule keys on: a real tensile bond.
	const auto CountBonded = [](const FOracleProblem& Problem)
	{
		int32 Bonded = 0;

		for (const FOracleJoint& Joint : Problem.Joints)
		{
			if (Joint.Strength.TensileStrengthMPa > 0.0)
			{
				++Bonded;
			}
		}

		return Bonded;
	};

	// Solve one bridged problem with the flag off, then on.
	struct FPair
	{
		bool bOk = false;
		int32 Bonded = 0;
		FOracleResult Off;
		FOracleResult On;
	};

	const auto SolveBothWays =
		[&CountBonded](FAutomationTestBase& Test, const TCHAR* Name,
			const TFunction<bool(FStructure&, FString&)>& Build) -> FPair
	{
		FPair Out;

		FStructure Structure;
		FString Why;

		if (!Build(Structure, Why))
		{
			Test.AddError(FString::Printf(TEXT("%s: fixture could not be laid: %s"), Name, *Why));
			return Out;
		}

		FOracleProblem Problem;

		if (!BuildRigidBlockProblem(Structure, Problem, Why))
		{
			Test.AddError(FString::Printf(TEXT("%s: bridge refused the fixture: %s"), Name, *Why));
			return Out;
		}

		Out.Bonded = CountBonded(Problem);

		Problem.bFirstCrackRows = false;
		Out.Off = SolveRigidBlock(Problem);

		Problem.bFirstCrackRows = true;
		Out.On = SolveRigidBlock(Problem);

		if (!Test.TestTrue(
				*FString::Printf(TEXT("%s: control solve (flag off) must answer"), Name),
				Out.Off.bAnswered)
			|| !Test.TestTrue(
				*FString::Printf(TEXT("%s: first-crack solve (flag on) must answer"), Name),
				Out.On.bAnswered))
		{
			return Out;
		}

		Out.bOk = true;

		UE_LOG(LogTemp, Display,
			TEXT("FIRSTCRACK %s: bonded=%d lambdaOff=%.17g lambdaOn=%.17g ratio=%.6g ")
			TEXT("pivotsOff=%d pivotsOn=%d"),
			Name, Out.Bonded, Out.Off.Lambda, Out.On.Lambda,
			Out.Off.Lambda > 0.0 ? Out.On.Lambda / Out.Off.Lambda : -1.0,
			Out.Off.SimplexIterations, Out.On.SimplexIterations);

		return Out;
	};

	// Beam rows: one bonded glue line each, bending governs.
	struct FBeamRow
	{
		const TCHAR* Name;
		double Density;
		double BendingMPa;
		double ShearMPa;
		double BlockHeightCm;
		double ControlLambda;
	};

	const FBeamRow BeamRows[] = {
		{ TEXT("C24 timber beam, heavy load"), BeamC24DensityGramsPerCubicCm,
			BeamC24BendingMPa, BeamC24ShearMPa, 120.0, 2.6461037357339725 },
		{ TEXT("C24 timber beam, light load"), BeamC24DensityGramsPerCubicCm,
			BeamC24BendingMPa, BeamC24ShearMPa, 10.0, 28.801133612618997 },
		{ TEXT("S275 steel beam, heavy load"), BeamSteelDensityGramsPerCubicCm,
			BeamS275YieldMPa, BeamS275YieldMPa / FMath::Sqrt(3.0), 120.0,
			18.299323934632291 },
	};

	for (const FBeamRow& Beam : BeamRows)
	{
		const FPair P = SolveBothWays(*this, Beam.Name,
			[&Beam](FStructure& Out, FString& Why)
			{
				return BuildBeam(Beam.Density, Beam.BendingMPa, Beam.ShearMPa,
					Beam.BlockHeightCm, Out, Why);
			});

		if (!P.bOk)
		{
			continue;
		}

		TestEqual(
			*FString::Printf(
				TEXT("%s: exactly one bonded joint (the glue line) carries a first-crack row"),
				Beam.Name),
			P.Bonded, 1);

		TestTrue(
			*FString::Printf(
				TEXT("%s: control lambda* %.9g must sit at the pinned %.9g (flag off is the ")
				TEXT("untouched wall)"),
				Beam.Name, P.Off.Lambda, Beam.ControlLambda),
			FMath::Abs(P.Off.Lambda - Beam.ControlLambda) <= 1.0e-4 * Beam.ControlLambda);

		// Absent rows leave lambda*(on) == lambda*(off), which fails here.
		TestTrue(
			*FString::Printf(
				TEXT("%s: FIRST-CRACK ROWS MUST BITE — lambda*(on)=%.9g must fall below ")
				TEXT("0.5x lambda*(off)=%.9g (predicted control/3=%.9g). It did not move, so ")
				TEXT("the rows are absent"),
				Beam.Name, P.On.Lambda, P.Off.Lambda, Beam.ControlLambda / 3.0),
			P.On.Lambda < 0.5 * P.Off.Lambda);

		// N~=0 at midspan puts first crack at the full /3; the window allows arching/shear wobble.
		TestTrue(
			*FString::Printf(
				TEXT("%s: lambda*(on)=%.9g must land near control/3=%.9g (window 0.28-0.42 of ")
				TEXT("control %.9g) — the first-crack factor is exactly 3 in pure bending"),
				Beam.Name, P.On.Lambda, Beam.ControlLambda / 3.0, Beam.ControlLambda),
			P.On.Lambda >= 0.28 * P.Off.Lambda && P.On.Lambda <= 0.42 * P.Off.Lambda);
	}

	// Reported, not asserted: the verdict flip on beam row 1 is a user decision.
	AddInfo(TEXT(
		"USER DECISION (do not bank): first-crack takes beam row 1 (C24 heavy) to ~0.882 "
		"< 1.0, an oracle verdict flip to Falls — correct verdict, glue-line-cracking route "
		"rather than member bending. Measure and REPORT for the user's ruling; do not encode "
		"it as settled (PROMOTION_DESIGN Sec 4.3, Sec 8)."));

	// Dry one-cell pair: no bond, so it must return bit-identical.
	{
		const FPair P = SolveBothWays(*this, TEXT("one-cell dry half seat"),
			[](FStructure& Out, FString& Why) { return BuildOneCellDryPair(Out, Why); });

		if (P.bOk)
		{
			TestEqual(
				TEXT("one-cell dry half seat: zero bonded joints (nothing to carry a "
					 "first-crack row)"),
				P.Bonded, 0);

			TestEqual(
				TEXT("one-cell dry half seat: lambda* is BIT-IDENTICAL with the flag on — dry "
					 "stone has no bond to crack, so the rule keyed on f_t > 0 writes no row"),
				P.On.Lambda, P.Off.Lambda);

			TestEqual(
				TEXT("one-cell dry half seat: pivot count is bit-identical with the flag on — "
					 "an identical problem is solved either way"),
				P.On.SimplexIterations, P.Off.SimplexIterations);
		}
	}

	return true;
}

/*
 * The opt-in oracle sweep: OracleSweepFast (iteration) and OracleSweepFull (verification).
 * Neither name contains "DestructionGame", so the default suite never runs them; see CLAUDE.md
 * for commands and counts. Placement is by cost alone.
 *
 * OracleSweepFull is mandatory before any commit touching the LP oracle or the solver; a green
 * fast tier verifies nothing at scale. An opt-in tier rots if not run (TRAPS).
 */

/**
 * Wall catalogue (sweep item (d)): every acceptance wall except the 30-course cases 1-5 (too
 * costly), ordered cheap to expensive so a killed run keeps its readings. Each row's ruling is
 * in its mechanism text. 15/16 is pinned as a cross-row identity.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockSlowWallSweepTest,
	"OracleSweepFull.RigidBlock.WallsAndLadders",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockSlowWallSweepTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	const auto Scenario = [](const TCHAR* Name)
	{
		return [Name](FStructure& Out, FString& Why)
		{
			return BuildScenarioStructure(Name, Out, Why);
		};
	};

	TArray<FSweepRow> Rows;

	// This measurement moved the catalogue ruling for wall-08 to stands (DESIGN §8).
	Rows.Add({ TEXT("wall-08 four-cell opening, one course over"),
		TEXT("the flat-arch row: the four-cell coverless course jams against its ")
		TEXT("abutments through head-joint compression, so cover is not what carries it ")
		TEXT("and at 325x own weight the verdict survives every charitable-reading ")
		TEXT("discount (/3 plastic, /6 characteristic-vs-mean, i.e. 18.0 discounted both ")
		TEXT("ways at once — and the characteristic-vs-mean slant runs the other way, so ")
		TEXT("that half is budget rather than correction). THIS MEASUREMENT IS WHAT MOVED ")
		TEXT("THE CATALOGUE: case 8 was re-ruled from LOCAL LOSS to STANDS on 2026-08-11, ")
		TEXT("costing the set its last 'no room to arch' discriminator"),
		Scenario(TEXT("wall-08")),
		ERelation::AgreeStands, 481.519, 481.539, 0 });

	Rows.Add({ TEXT("corbel C, ten steps"),
		TEXT("the §8 ruling's fixture: the limit theorem agrees it stands — 17.97x at the ")
		TEXT("mean bond (2.57x at the retired characteristic, an exact x7: this rung is ")
		TEXT("tension-bound)"),
		Scenario(TEXT("corbel-c-10")),
		ERelation::AgreeStands, 17.97037, 17.97109, 0 });

	Rows.Add({ TEXT("wall-14 corbel, half brick per course"),
		TEXT("the projection pair's far half; see wall-13 for the pair's cross-method ")
		TEXT("history"),
		Scenario(TEXT("wall-14")),
		ERelation::AgreeStands, 440.200, 440.218, 0 });

	Rows.Add({ TEXT("wall-13 corbel, quarter brick per course"),
		TEXT("the corbel projection pair. AT THE CHARACTERISTIC DATA the pair was the ")
		TEXT("suite's strongest cross-validation — production 2.7786x vs LP 2.7777x, ")
		TEXT("0.03% apart. AT THE MEAN DATA that agreement is GONE: production's ratio is ")
		TEXT("unchanged (both readings moved /7 together) while the LP's fell to ")
		TEXT("817.047/440.209 = 1.856 — the two rungs' binding constraints no longer move ")
		TEXT("together, so the LP-side projection term is an open re-derivation (logged in ")
		TEXT("CURRENT_STATE), and the windows are what this row still pins"),
		Scenario(TEXT("wall-13")),
		ERelation::AgreeStands, 817.030, 817.063, 0 });

	Rows.Add({ TEXT("corbel D, ten steps with counterweight"),
		TEXT("global equilibrium prices the counterweight at 8.97x on the mean basis ")
		TEXT("(C's 17.97 -> 161.14; it was 22.6x at the characteristic data) where ")
		TEXT("production reads C and D 0.4% apart and the deliberate red ")
		TEXT("CorbelStepsBeforeTensionWins records them crossing 1.0 at 124 steps ")
		TEXT("identically"),
		Scenario(TEXT("corbel-d-10-counterweight")),
		ERelation::AgreeStands, 161.1375, 161.1439, 0 });

	/*
	 * Stack-bond pair. Production's ratio is not pinned: the family is deliberately red
	 * (StackBondColumnShearIsHeightIndependent), so it would pin a known defect.
	 */
	Rows.Add({ TEXT("wall-18 stack bond, one brick out"),
		TEXT("stack bond has no bond to spread a loss, so the column over the hole hangs ")
		TEXT("on head-joint tension into its neighbours: 897.73x at the mean bond (649.46 ")
		TEXT("at the characteristic — this rung moved x1.382 while the intact wall-17's ")
		TEXT("918.05 did not move at all, so the pair's margin shrank 1.41x -> 1.023x: ")
		TEXT("the strength-governed rung closed most of the gap on the crush-governed one)"),
		Scenario(TEXT("wall-18")),
		ERelation::AgreeStands, 897.712, 897.749, 0, 0 });

	Rows.Add({ TEXT("wall-17 stack bond, intact"),
		TEXT("the highest lambda* in the swept set (918.05x) and the first absolute ")
		TEXT("number case 17 has ever carried; an intact stack-bond wall standing is not ")
		TEXT("news, the MARGIN against wall-18 is"),
		Scenario(TEXT("wall-17")),
		ERelation::AgreeStands, 918.04511, 918.04695, 0, 0 });

	/*
	 * Rows 9, 10, 19 and 20 were ruled 2026-08-12 (see each mechanism text). The hand figures
	 * are elastic tension-plane checks; lambda* finds the best compression path and runs 14-32x
	 * above them.
	 *
	 * At 120-150 blocks the certified optimum varies ~1e-5 relative across pivot paths, so these
	 * windows are +/-2e-5. Anything tighter pins the algorithm, not the answer.
	 */

	Rows.Add({ TEXT("wall-10 opening at a free end, no abutment"),
		TEXT("THIS MEASUREMENT IS WHAT MOVED THE CATALOGUE: case 10 was re-ruled from ")
		TEXT("COLLAPSE to STANDS on 2026-08-12, and unlike case 8 the model does NOT ")
		TEXT("follow — the acceptance row stays red in the inverted direction, expected to ")
		TEXT("stand and dropping 12. Production drops those 12, of which 3 ")
		TEXT("are STRANDED (unroutable, not unheld — the absent cycle rule, DESIGN §5.1); ")
		TEXT("the LP stands it at 35.82x. The panel over the opening is a 3.75-cell ")
		TEXT("(84 cm) cantilever 8 courses (60 cm) deep off a single jamb: 8 x 3.75 = 30 ")
		TEXT("brick weights = 800 N at a 42.2 cm lever is M = 337 N*m, over ")
		TEXT("t*D^2/6 = 6150 cm^3 that is 0.055 MPa — 0.55x characteristic f_xk1 (0.10) ")
		TEXT("and 0.14x of f_xk2 (0.40) on a STRAIGHT vertical plane, and the real crack path ")
		TEXT("is a toothed staircase carrying bed-joint cohesion the plane cannot see. ")
		TEXT("Same mechanism family as the 2026-08-06 free-end ruling, scaled up to 3.75 ")
		TEXT("cells, which is what the ruling credited. PRODUCTION NOW AGREES (Slice 3b/4, ")
		TEXT("2026-08-27): below the 200-block cap the equilibrium LP is the break authority ")
		TEXT("and carries the panel the downward-only router could only strand, so production ")
		TEXT("drops 0 (was 12) and strands 0 (was 3) — the absent-mechanism verdict is gone and ")
		TEXT("the row agrees with the oracle. The lambda* window is UNCHANGED; only production's ")
		TEXT("half of the diff moved"),
		Scenario(TEXT("wall-10")),
		ERelation::AgreeStands, 111.4952, 111.4997, 0, 0 });

	Rows.Add({ TEXT("wall-19 bottom course out under half the wall"),
		TEXT("THIS MEASUREMENT IS WHAT MOVED THE CATALOGUE, and it is the CLOSEST CALL of ")
		TEXT("the four ruled on 2026-08-12: case 19 re-ruled COLLAPSE to STANDS knowingly, ")
		TEXT("with the model still dropping 34 so the acceptance row stays red in the ")
		TEXT("inverted direction. The LOWEST lambda* of the fifteen walls (12.38x) ")
		TEXT("— still above 1. Production drops 34 with 6 STRANDED. ")
		TEXT("6.0 cells (135 cm) of base gone at the wall's END — course 0 is an EVEN course ")
		TEXT("of whole bricks, so the cut takes cells 0..5, six of them, and the run prints ")
		TEXT("cut 6 — so the 9 courses over ")
		TEXT("it CANTILEVER rather than span: 9 x 6 = 54 brick weights = 1440 N at a ")
		TEXT("67.5 cm lever is M = 972 N*m, over t*D^2/6 = 7784 cm^3 that is 0.125 MPa ")
		TEXT("— 1.25x characteristic f_xk1 (0.10) but ")
		TEXT("0.31x of f_xk2's 0.40 and ~0.21x of the mean basis, and f_xk2 is what a ")
		TEXT("toothed vertical crack path ")
		TEXT("costs in a real wall. The two strengths STRADDLE the verdict, so this was a ")
		TEXT("judgement and is recorded as one: it is an END CANTILEVER with no second ")
		TEXT("support to redistribute to, against the practice anchor of ~1 m underpinning ")
		TEXT("bays that a bonded wall is expected to bridge — and 135 cm is longer than ")
		TEXT("that, which the ruling knows. PRODUCTION NOW AGREES (Slice 3b/4, 2026-08-27): ")
		TEXT("below the cap the LP carries the wedge the base-less router could only strand, so ")
		TEXT("production drops 0 (was 34) and strands 0 (was 6). The lambda* window is UNCHANGED"),
		Scenario(TEXT("wall-19")),
		ERelation::AgreeStands, 47.96274, 47.96466, 0, 0 });

	// Pier pair (11 vs 12): DESIGN §8 handed this discrimination to the oracle.
	Rows.Add({ TEXT("wall-12 the same span on a one-brick pier"),
		TEXT("the pier pair's narrow half: 89.12x, 1.44x less margin than wall-11's ")
		TEXT("three cells of bearing, where production reads the two 0.03% apart ")
		TEXT("(0.362067 vs 0.362193) because springing shear carries no pier-width term"),
		Scenario(TEXT("wall-12")),
		ERelation::AgreeStands, 206.8525, 206.8608, 0, 0 });

	/*
	 * The superimposed-load pair reads one lambda*: gravity is the LP's only live load, so the
	 * surcharge scales demand and pre-compression together. Production separates them, so both
	 * sides are pinned below. A dead/live split is wanted (CURRENT_STATE).
	 */
	Rows.Add({ TEXT("wall-16 header at the top, nothing on it"),
		TEXT("the superimposed-load pair's bare half; the LP prices it identically to ")
		TEXT("wall-15 because self-weight is its only live load, pinned as a cross-row ")
		TEXT("identity below"),
		Scenario(TEXT("wall-16")),
		ERelation::AgreeStands, 868.62287, 868.62461, 0, 0 });

	Rows.Add({ TEXT("wall-15 header with six courses on top"),
		TEXT("the loaded half, and the SAME NUMBER: 868.623736 either way. Production ")
		TEXT("separates the pair 31.6x at the header's own bed joint (0.00184437 vs ")
		TEXT("0.058203838 — the surcharge drives tension to zero and leaves compression ")
		TEXT("governing), so the discrimination the catalogue wanted is production's ")
		TEXT("here and the LP's only under a dead/live split it does not yet use"),
		Scenario(TEXT("wall-15")),
		ERelation::AgreeStands, 868.62287, 868.62461, 0, 0 });

	Rows.Add({ TEXT("wall-20 staircase void"),
		TEXT("RE-RULED TO AGREE-STANDS AT SLICE 4 (2026-08-27), the standing doubt settled in ")
		TEXT("the LP's direction. Through 2026-08-12 the catalogue ruled a LOCAL LOSS of two ")
		TEXT("NAMED teeth (course 3 cell 4.5, course 5 cell 2.5) that production over-answered by ")
		TEXT("dropping 9; below the 200-block cap the equilibrium LP is now the break authority ")
		TEXT("and stands the whole wall INCLUDING both teeth, so production drops 0 (was 9) and ")
		TEXT("agrees with the oracle. Why hanging a tooth is cheap, by hand: 0.1 MPa over ")
		TEXT("two head joints (2 x 66.625 cm^2) plus the two bed patches above it (2 x ")
		TEXT("105.0625 cm^2) is 3434 N against a brick's 2667 uu = 26.67 N, about 129x, ")
		TEXT("so the teeth are not what governs and the LP is not merely tolerating them. The ")
		TEXT("catalogue row is re-ruled STANDS to match (a global feasible force path exists); a ")
		TEXT("genuinely-local two-tooth mechanism, if one exists, needs the per-region ")
		TEXT("interrogation the global solve cannot express (PROMOTION_DESIGN §3.5). The lambda* ")
		TEXT("window is UNCHANGED"),
		Scenario(TEXT("wall-20")),
		ERelation::AgreeStands, 218.4185, 218.4272, 0, 0 });

	Rows.Add({ TEXT("wall-11 wall on two piers, six-brick clear span"),
		TEXT("the pier pair's wide half at 128.12x; the §8 case-11 ruling worked this ")
		TEXT("fixture by hand at 0.03-0.04 MPa of deep-beam bending against mean bond, ")
		TEXT("and the LP's three-figure margin at CHARACTERISTIC bond is that ruling's ")
		TEXT("first independent confirmation"),
		Scenario(TEXT("wall-11")),
		ERelation::AgreeStands, 269.3169, 269.3277, 0, 0 });

	Rows.Add({ TEXT("wall-09 ten-brick opening, eight courses over"),
		TEXT("THIS MEASUREMENT IS WHAT MOVED THE CATALOGUE: case 9 was re-ruled from ")
		TEXT("COLLAPSE to STANDS on 2026-08-12 and, alone of the three moved that day, ")
		TEXT("the MODEL AGREES — the acceptance row went green and left the known-red ")
		TEXT("list. Still the least stable relation in this file: ")
		TEXT("production STANDS at worst reading 0.985 — ONE RETUNE ")
		TEXT("FROM FLAPPING, and the day it crosses 1.0 this row's AgreeStands becomes ")
		TEXT("AgreeFalls with nothing about the physics having changed, which is now ")
		TEXT("watched from the production side too by Acceptance.Wall.SpanIsReadInThe")
		TEXT("JointNotInTheOutcome — while the LP ")
		TEXT("stands it at 36.56x. By hand: a 9.5-cell (214 cm) opening under 8 courses ")
		TEXT("(60 cm) of cover is a deep beam at span/depth 3.6 carrying 8 x 9.5 = 76 ")
		TEXT("brick weights = 2027 N, W*L/8 = 542 N*m over t*D^2/6 = 6150 cm^3 = 0.088 ")
		TEXT("MPa — 0.88x characteristic f_xk1 (0.10), 0.22x of f_xk2 (0.40) and ~0.15x ")
		TEXT("of the mean basis. That ")
		TEXT("hand figure and production's 0.985 are the same answer to within the ")
		TEXT("routing — two methods agreeing against one, which is what decided it. The ")
		TEXT("catalogue's COLLAPSE came from the published arching ")
		TEXT("gate (no room for a 45 deg triangle over 2.1 m), and this is the THIRD ")
		TEXT("verdict that gate has lost after cases 11 and 8: the set now has no case ")
		TEXT("refusing a span for want of rise"),
		Scenario(TEXT("wall-09")),
		ERelation::AgreeStands, 105.6457, 105.6499, 0, 0 });

	/*
	 * Cover pair (7 vs 8). The routerless LP reads the same direction as production, so that
	 * direction says nothing about the router. Whether DESIGN §8 should move is an open user call.
	 */
	Rows.Add({ TEXT("wall-07 four-cell opening, eight courses over"),
		TEXT("296.22x against wall-08's 324.73x: the LP prices eight courses of cover ")
		TEXT("1.10x WORSE than one, the same direction production reads at the joint and ")
		TEXT("for a reason that is about the measure (lambda* is a multiplier on own ")
		TEXT("weight) rather than about the router"),
		Scenario(TEXT("wall-07")),
		ERelation::AgreeStands, 456.874, 456.893, 0, 0 });

	Rows.Add({ TEXT("wall-06 two-cell opening, deep cover"),
		TEXT("the span ladder's short end: half wall-07's span under the same cover ")
		TEXT("prices 2.10x its margin (622.03 vs 296.22), which is the discrimination the ")
		TEXT("outcome column never had — 06, 07 and 08 all stand and always did"),
		Scenario(TEXT("wall-06")),
		ERelation::AgreeStands, 634.570, 634.596, 0, 0 });

	/*
	 * Wall-21: both models stand what the catalogue rules Collapse. Which mechanism carries it
	 * is open (CURRENT_STATE); decompose over GetConnectionForce before trusting any attribution
	 * (TRAPS).
	 */
	Rows.Add({ TEXT("wall-21 eighteen-cell opening, two courses over"),
		TEXT("the COVER counter-case: two courses (15 cm) over a 4.06 m opening is ")
		TEXT("1.2014 MPa of deep-beam bending, 1.71x the coded mean f_x1 — the catalogue's ")
		TEXT("ruled Collapse. Since the mean re-anchor BOTH models here stand it: the LP at ")
		TEXT("17.24, production at a worst reading of 0.9354 with nothing dropped — so this ")
		TEXT("row now measures the two of them agreeing against the ruled verdict, which is ")
		TEXT("the catalogue's inverted deliberate red, not this file's"),
		Scenario(TEXT("wall-21")),
		ERelation::AgreeStands, 17.23854, 17.23923, 0, 0 });

	TArray<FSweepReading> Readings;
	RunRows(*this, Rows, Readings);

	const FSweepReading* Loaded =
		ReadingNamed(*this, Rows, Readings, TEXT("wall-15 header with six courses on top"));
	const FSweepReading* Bare =
		ReadingNamed(*this, Rows, Readings, TEXT("wall-16 header at the top, nothing on it"));

	if (Loaded != nullptr && Bare != nullptr)
	{
		CheckSameLambda(
			*this,
			TEXT("the superimposed-load pair (wall-15 vs wall-16) under one live load"),
			*Loaded, *Bare, SameNumberRelativeTolerance);

		CheckReadingRatio(
			*this,
			TEXT("production separates the superimposed-load pair where the LP does not"),
			Bare->WorstUtilisation, Loaded->WorstUtilisation,
			SuperimposedReadingRatioLo, SuperimposedReadingRatioHi);
	}

	return true;
}

/**
 * Free-end height ladder (DESIGN.md §7 gap 5): StructureFreeEndHeightTest's fixture at 10 and
 * 20 courses. Both models scale with height (LP 2.1114x, production 2.1198x) and both stand.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockSlowFreeEndLadderTest,
	"OracleSweepFast.RigidBlock.FreeEndHeightLadder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockSlowFreeEndLadderTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	TArray<FSweepRow> Rows;

	Rows.Add({ TEXT("free end, 7 cells x 10 courses"),
		TEXT("the promoted canary: the LP prices the ten-course free end at 629.2x own ")
		TEXT("weight at the mean strengths while production's worst joint reads 0.132 ")
		TEXT("and holds"),
		[](FStructure& Out, FString& Why) { return BuildFreeEnd(10, Out, Why); },
		ERelation::AgreeStands, 629.187, 629.212, 0, 0 });

	Rows.Add({ TEXT("free end, 7 cells x 20 courses"),
		TEXT("the same wall twice as tall: since the mean re-anchor BOTH sides scale ")
		TEXT("with height (the LP 2.1114x, production 2.1199x) and both stand — the ")
		TEXT("characteristic-era height-flat LP and the production crossing are history ")
		TEXT("(header)"),
		[](FStructure& Out, FString& Why) { return BuildFreeEnd(20, Out, Why); },
		ERelation::AgreeStands, 297.991, 298.004, 0, 0 });

	TArray<FSweepReading> Readings;
	RunRows(*this, Rows, Readings);

	const FSweepReading* Short =
		ReadingNamed(*this, Rows, Readings, TEXT("free end, 7 cells x 10 courses"));
	const FSweepReading* Tall =
		ReadingNamed(*this, Rows, Readings, TEXT("free end, 7 cells x 20 courses"));

	if (Short == nullptr || Tall == nullptr)
	{
		return true;
	}

	CheckReadingRatio(
		*this,
		TEXT("the LP's free-end lambda* now scales with height (7x10 over 7x20)"),
		Short->Oracle.Lambda, Tall->Oracle.Lambda,
		2.11122, 2.11163);

	CheckReadingRatio(
		*this,
		TEXT("production's free-end reading is height-linear"),
		Tall->WorstUtilisation, Short->WorstUtilisation,
		FreeEndReadingRatioLo, FreeEndReadingRatioHi);

	TestTrue(
		*FString::Printf(
			TEXT("production's free-end readings both sit under capacity on the mean basis ")
			TEXT("— 10 courses read %.17g and 20 courses read %.17g; the characteristic-era ")
			TEXT("crossing between these heights is retired, and either reading crossing 1.0 ")
			TEXT("again means the ladder's slope must be re-derived"),
			Short->WorstUtilisation, Tall->WorstUtilisation),
		Short->WorstUtilisation < 1.0 && Tall->WorstUtilisation < 1.0);

	return true;
}

/**
 * Case-21 mechanism ladders (DESIGN §8, 2026-08-13). The rise ladder varies courses below the
 * opening (an arch predicts growth, a cover-carried mechanism flat); the abutment ladder varies
 * jamb width (a jamb-reacting mechanism predicts ~2x). Measured: depth does nothing, so the arch
 * is refuted; the mechanism is span-dominated and nearly abutment-blind.
 *
 * Pins the measurements, never the hypotheses: each rung a lambda* window (+/-2e-5), each ladder
 * a ratio with both predictions printed. The first row must equal Scenario("wall-21"), or every
 * rung is void.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockSlowOpeningLaddersTest,
	"OracleSweepFast.RigidBlock.OpeningMechanismLadders",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockSlowOpeningLaddersTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	const auto Rung =
		[](int32 OpeningCells, int32 CoursesBelow, int32 JambCells, int32 CoverTailCells)
	{
		return [OpeningCells, CoursesBelow, JambCells, CoverTailCells]
			(FStructure& Out, FString& Why)
		{
			return BuildOpeningLadderWall(
				OpeningCells, CoursesBelow, JambCells, CoverTailCells, Out, Why);
		};
	};

	TArray<FSweepRow> Rows;

	// Production stands every rung; even-parity rungs break one joint but drop nothing.
	Rows.Add({ TEXT("wall-21 through the scenario catalogue"),
		TEXT("the equivalence control: the shipped case-21 fixture, laid the way every ")
		TEXT("verdict row in this file is laid, so the hand-parameterised rung below can be ")
		TEXT("held against it as the same number"),
		[](FStructure& Out, FString& Why)
		{
			return BuildScenarioStructure(TEXT("wall-21"), Out, Why);
		},
		ERelation::AgreeStands, 17.23854, 17.23923, 0, 0 });

	Rows.Add({ TEXT("rise s=1 / abutment j=2 (case 21 itself)"),
		TEXT("both ladders' bottom rung and the equivalence control's partner — the same ")
		TEXT("83 blocks and 133 joints as the catalogue row above, which is what the ")
		TEXT("same-number pin below turns into an assertion"),
		Rung(18, 1, 2, INDEX_NONE),
		ERelation::AgreeStands, 17.23854, 17.23923, 0, 0 });

	Rows.Add({ TEXT("rise s=2"),
		TEXT("one more course under the opening: an arch from the ground gains 7.5 cm of ")
		TEXT("rise, a cover-carried mechanism gains nothing. It gains nothing — what moves ")
		TEXT("here is the REVEAL PARITY (an even-first opening), and the depth pins below ")
		TEXT("compare s=3 against s=1 and s=4 against s=2 to keep the two apart"),
		Rung(18, 2, 2, INDEX_NONE),
		ERelation::AgreeStands, 15.29968, 15.30030, 0, 0 });

	Rows.Add({ TEXT("rise s=3"),
		TEXT("two more courses under the opening, back on case 21's odd-first parity: the ")
		TEXT("same lambda* to 6.6e-6 relative at the mean data (1.9e-7 at the ")
		TEXT("characteristic)"),
		Rung(18, 3, 2, INDEX_NONE),
		ERelation::AgreeStands, 17.23865, 17.23934, 0, 0 });

	Rows.Add({ TEXT("rise s=4"),
		TEXT("three more courses under the opening: the springing is 22.5 cm deeper than ")
		TEXT("case 21's, which is 1.75x the rise the priced arch was fitted at, and lambda* ")
		TEXT("reads its parity partner s=2 to the last bit at the mean data"),
		Rung(18, 4, 2, INDEX_NONE),
		ERelation::AgreeStands, 15.29968, 15.30030, 0, 0 });

	Rows.Add({ TEXT("abutment j=3"),
		TEXT("half again the jamb, same cut span, same wall height, same reveal parity — ")
		TEXT("1.34x at the characteristic data, and only 1.04x at the mean: the abutment ")
		TEXT("sensitivity all but vanished at the re-anchor (the family's open mechanism ")
		TEXT("question — see the 2026-08-14 note above)"),
		Rung(18, 1, 3, INDEX_NONE),
		ERelation::AgreeStands, 17.89086, 17.89158, 0, 0 });

	Rows.Add({ TEXT("abutment j=4"),
		TEXT("twice the jamb: twice the bearing and twice the weight over it, with the ")
		TEXT("opening, the cover and the courses below all unmoved. This rung REFUSED in ")
		TEXT("phase 2 until 2026-08-13 and was pinned as a canary; the ratio test gained ")
		TEXT("its relative pivot floor and the canary flipped. The 2x test's mean-basis ")
		TEXT("answer is 1.05x — the bearing-proportional reading died with the re-anchor"),
		Rung(18, 1, 4, INDEX_NONE),
		ERelation::AgreeStands, 18.12397, 18.12469, 0, 0 });

	Rows.Add({ TEXT("abutment j=3, cover tail trimmed to two cells"),
		TEXT("THE DISAMBIGUATOR the abutment ladder needs: widening a jamb widens the ")
		TEXT("bearing AND lengthens the cover built in over it, and j=3 alone cannot say ")
		TEXT("which paid. At the mean data it lands between its neighbours again — 17.654 ")
		TEXT("against 17.239 and 17.891 — but the whole spread is now 3.8%, so what it ")
		TEXT("disambiguates is proportionally small (see the 2026-08-14 note above)"),
		Rung(18, 1, 3, 2),
		ERelation::AgreeStands, 17.65379, 17.65449, 0, 0 });

	/*
	 * Matched-span pair: separates the rise ladder's "parity" step into reveal span and cover
	 * bond parity. A 17-cell even-first cut bears on case 21's own 383.50 cm reveal with the
	 * opposite parity; the 17-cell odd-first cut prices pure span.
	 */
	Rows.Add({ TEXT("matched span: 17 cells, s=1"),
		TEXT("the pure SPAN step at fixed odd-first parity — one cell of opening off case ")
		TEXT("21, so the cover bears on 361.00 cm instead of 383.50 with the toothing and ")
		TEXT("the cover's bond parity unchanged. Span-squared predicts 1.128x case 21; the ")
		TEXT("mean data measures 1.1385 (the characteristic data measured 1.156)"),
		Rung(17, 1, 2, INDEX_NONE),
		ERelation::AgreeStands, 19.62579, 19.62658, 0, 0 });

	Rows.Add({ TEXT("matched span: 17 cells, s=2"),
		TEXT("THE MATCHED-SPAN RUNG: a 17-cell cut starting on an even course bears its ")
		TEXT("cover on the SAME 383.50 cm clear reveal as case 21's 18-cell odd-first one, ")
		TEXT("with the cover's two courses on the opposite bond parity. The span hypothesis ")
		TEXT("predicts ~1.00 against case 21 and the cover-bond-parity hypothesis the ")
		TEXT("parity step; the mean data measures 1.00088 (0.990 at the characteristic), ")
		TEXT("so the step IS the bearing span on either basis"),
		Rung(17, 2, 2, INDEX_NONE),
		ERelation::AgreeStands, 17.25377, 17.25446, 0, 0 });

	TArray<FSweepReading> Readings;
	RunRows(*this, Rows, Readings);

	const FSweepReading* Catalogue =
		ReadingNamed(*this, Rows, Readings, TEXT("wall-21 through the scenario catalogue"));
	const FSweepReading* S1 =
		ReadingNamed(*this, Rows, Readings, TEXT("rise s=1 / abutment j=2 (case 21 itself)"));
	const FSweepReading* S2 = ReadingNamed(*this, Rows, Readings, TEXT("rise s=2"));
	const FSweepReading* S3 = ReadingNamed(*this, Rows, Readings, TEXT("rise s=3"));
	const FSweepReading* S4 = ReadingNamed(*this, Rows, Readings, TEXT("rise s=4"));
	const FSweepReading* J3 = ReadingNamed(*this, Rows, Readings, TEXT("abutment j=3"));
	const FSweepReading* J4 = ReadingNamed(*this, Rows, Readings, TEXT("abutment j=4"));
	const FSweepReading* J3Trimmed = ReadingNamed(
		*this, Rows, Readings, TEXT("abutment j=3, cover tail trimmed to two cells"));
	const FSweepReading* W17S1 =
		ReadingNamed(*this, Rows, Readings, TEXT("matched span: 17 cells, s=1"));
	const FSweepReading* W17S2 =
		ReadingNamed(*this, Rows, Readings, TEXT("matched span: 17 cells, s=2"));

	if (Catalogue == nullptr || S1 == nullptr || S2 == nullptr || S3 == nullptr
		|| S4 == nullptr || J3 == nullptr || J4 == nullptr || J3Trimmed == nullptr
		|| W17S1 == nullptr || W17S2 == nullptr)
	{
		return true;
	}

	CheckSameLambda(
		*this,
		TEXT("the ladder's bottom rung IS case 21 (hand-parameterised vs the catalogue)"),
		*S1, *Catalogue, SameNumberRelativeTolerance);

	CheckRungSize(*this, TEXT("rise s=1 / abutment j=2"), *S1, 83);
	CheckRungSize(*this, TEXT("rise s=2"), *S2, 104);
	CheckRungSize(*this, TEXT("rise s=3"), *S3, 128);
	CheckRungSize(*this, TEXT("rise s=4"), *S4, 149);
	CheckRungSize(*this, TEXT("abutment j=3"), *J3, 95);
	CheckRungSize(*this, TEXT("abutment j=4"), *J4, 107);
	CheckRungSize(*this, TEXT("abutment j=3, cover tail trimmed"), *J3Trimmed, 89);
	CheckRungSize(*this, TEXT("matched span: 17 cells, s=1"), *W17S1, 80);
	CheckRungSize(*this, TEXT("matched span: 17 cells, s=2"), *W17S2, 100);

	/*
	 * Rise ladder: depth at fixed parity (s=3/s=1, s=4/s=2) and parity at fixed depth. Flat depth
	 * refutes rise-priced mechanisms, not every jamb path.
	 */
	CheckLambdaLadderRatio(
		*this,
		TEXT("THE RISE LADDER's depth, odd-parity: three courses below against one"),
		TEXT("arch-from-the-grounded-course predicts 1.50 (r 45.0 against 30.0 cm), ")
		TEXT("cover-carried and jamb-reaction both predict 1.00"),
		*S3, *S1, 0.99990, 1.00010);

	CheckLambdaLadderRatio(
		*this,
		TEXT("THE RISE LADDER's depth, even-parity: four courses below against two"),
		TEXT("arch-from-the-grounded-course predicts 1.40 (r 52.5 against 37.5 cm), ")
		TEXT("cover-carried and jamb-reaction both predict 1.00"),
		*S4, *S2, 0.99990, 1.00010);

	CheckLambdaLadderRatio(
		*this,
		TEXT("THE RISE LADDER's parity step, even-first opening against odd-first (s=2/s=1)"),
		TEXT("the ONLY movement in the rise ladder — the two parities bear the cover on ")
		TEXT("different clear reveals (383.50 vs 406.00 cm, a full cell of span), and ")
		TEXT("span-squared accounts for most of it ((383.5/406)^2 = 0.892 vs this 0.873). ")
		TEXT("SETTLED 2026-08-13 by the matched-span pair below: with the spans matched the ")
		TEXT("step falls to 0.990, so this is the reveal SPAN and not the cover's parity"),
		*S2, *S1, 0.88748, 0.88758);

	/*
	 * With spans matched the parity step vanishes, so the step is the reveal span. Production
	 * agrees independently (reading pins below), but only these LP-side pins catch a rung-flip.
	 */
	CheckLambdaLadderRatio(
		*this,
		TEXT("THE MATCHED-SPAN PAIR: 17 cells at s=2 against case 21, both bearing 383.50 cm"),
		TEXT("the bearing-span hypothesis predicts 1.00 (parity buys nothing once the spans ")
		TEXT("match), the cover-bond-parity hypothesis predicts the 0.873 step survives"),
		*W17S2, *S1, 1.00083, 1.00094);

	CheckLambdaLadderRatio(
		*this,
		TEXT("THE PURE SPAN STEP at fixed odd-first parity: 361.00 cm against 383.50 cm"),
		TEXT("span-squared predicts 1.128 and a ladder that had simply stopped responding ")
		TEXT("predicts 1.00; without this rung a matched-span 1.00 could not be told from ")
		TEXT("an insensitive one"),
		*W17S1, *S1, 1.13843, 1.13854);

	CheckLambdaLadderRatio(
		*this,
		TEXT("THE RISE LADDER's parity step, repeated two courses deeper (s=4/s=3)"),
		TEXT("the same reveal-width step, measured again at a different depth: if the ")
		TEXT("parity step were really a depth effect in disguise, these two would differ"),
		*S4, *S3, 0.88747, 0.88757);

	/*
	 * Abutment ladder. The trimmed rung separates wider bearing from longer cover tail, which
	 * widening the jamb changes together.
	 */
	CheckLambdaLadderRatio(
		*this,
		TEXT("THE ABUTMENT LADDER's step, three cells of jamb against two"),
		TEXT("a jamb-width-proportional reaction predicts ~1.50, a mechanism carried ")
		TEXT("between the reveals predicts 1.00"),
		*J3, *S1, 1.03779, 1.03789);

	CheckLambdaLadderRatio(
		*this,
		TEXT("THE ABUTMENT LADDER's 2x TEST: four cells of jamb against two"),
		TEXT("a bearing-proportional reaction predicts 2.00 and a mechanism carried ")
		TEXT("between the reveals predicts 1.00 — measured 1.698, between them and nearer ")
		TEXT("the first, which is the j=3 step's verdict restated over a doubled jamb"),
		*J4, *S1, 1.05131, 1.05141);

	CheckLambdaLadderRatio(
		*this,
		TEXT("THE DISAMBIGUATOR: the three-cell jamb with its cover tail trimmed back"),
		TEXT("if the jamb's bearing bought the 1.34, this stays near 1.34; if the longer ")
		TEXT("cover tail bought it, this falls back toward 1.00 — measured 1.141, which is ")
		TEXT("NEITHER, so both are live and this is the pin that says so"),
		*J3Trimmed, *S1, 1.02404, 1.02414);

	/*
	 * How much of the wider jamb's gain the trim gives back. Trimming also removes jamb
	 * precompression, so this slightly understates the bearing's share.
	 */
	CheckLambdaLadderRatio(
		*this,
		TEXT("THE DISAMBIGUATOR against the untrimmed jamb (trimmed j=3 / j=3)"),
		TEXT("all-bearing predicts 1.00 (the trim would change nothing), all-tail predicts ")
		TEXT("0.747 (the whole gain given back)"),
		*J3Trimmed, *J3, 0.98670, 0.98680);

	// Production sees the same depth and parity effects, so they are fixture properties, not the LP's.
	CheckReadingRatio(
		*this,
		TEXT("production's worst reading does not move with depth either (s=3 / s=1)"),
		S3->WorstUtilisation, S1->WorstUtilisation, 0.999999999, 1.000000001);

	CheckReadingRatio(
		*this,
		TEXT("production sees the reveal parity the LP sees (s=2 / s=1)"),
		S2->WorstUtilisation, S1->WorstUtilisation, 1.11531, 1.11543);

	/*
	 * Production's worst reading is in the cover, so it is blind to the jamb (DESIGN §7 step 4).
	 * Pinned at 1e-9 so a production-side change fires even while every lambda pin stays green.
	 */
	CheckReadingRatio(
		*this,
		TEXT("production is blind to the jamb: three cells against two (j=3 / j=2)"),
		J3->WorstUtilisation, S1->WorstUtilisation, 0.999999999, 1.000000001);

	CheckReadingRatio(
		*this,
		TEXT("production is blind to the jamb: four cells against two (j=4 / j=2)"),
		J4->WorstUtilisation, S1->WorstUtilisation, 0.999999999, 1.000000001);

	CheckReadingRatio(
		*this,
		TEXT("production is blind to the cover tail as well (trimmed j=3 / j=2)"),
		J3Trimmed->WorstUtilisation, S1->WorstUtilisation, 0.999999999, 1.000000001);

	// Identity plus a stay-apart ratio: an identity alone passes if two rows became one fixture (TRAPS).
	CheckReadingRatio(
		*this,
		TEXT("production prices the matched-span pair identically (17@s=2 / case 21)"),
		W17S2->WorstUtilisation, S1->WorstUtilisation, 0.999999999, 1.000000001);

	CheckReadingRatio(
		*this,
		TEXT("and separates the span step it is matched against (17@s=1 / case 21)"),
		W17S1->WorstUtilisation, S1->WorstUtilisation, 0.89065, 0.89076);

	return true;
}

/**
 * Case-21 strength probes: hold the fixture fixed and change one strength at a time in the
 * oracle's problem only, so production stays a bit-identical control. Measured: jamb bed
 * cohesion is the dominant single term (72%) but not the whole capacity; the cover's tensile
 * bond and first-crack bending are minor.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockSlowOpeningProbesTest,
	"OracleSweepFast.RigidBlock.OpeningStrengthProbes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockSlowOpeningProbesTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	// Every row is case 21's own wall.
	const auto CaseTwentyOne = [](FStructure& Out, FString& Why)
	{
		return BuildOpeningLadderWall(18, 1, 2, INDEX_NONE, Out, Why);
	};

	TArray<FSweepRow> Rows;

	Rows.Add({ TEXT("probe control: case 21 with every strength as built"),
		TEXT("the baseline every probe below is read against, and the row that says the ")
		TEXT("override machinery is inert when nothing asks it to do anything — same wall, ")
		TEXT("same 83 blocks, the lambda* the mechanism ladders pin, and ZERO joints ")
		TEXT("overridden, pinned rather than implied"),
		CaseTwentyOne,
		ERelation::AgreeStands, 17.23854, 17.23923, 0, 0, nullptr, 0 });

	Rows.Add({ TEXT("probe: jamb bed joints, cohesion zeroed"),
		TEXT("THE CLOSING EXPERIMENT, re-derived at the mean basis. Every bed joint below ")
		TEXT("the cover loses its cohesion and keeps its friction and its tension. At the ")
		TEXT("characteristic data that collapsed lambda* past 1.0 (0.785 — the wall fell); ")
		TEXT("at the mean data it reads 4.768, a factor of 0.277 of the control: the ")
		TEXT("dominant single term, no longer the whole capacity, and the modified wall ")
		TEXT("stands"),
		CaseTwentyOne,
		ERelation::AgreeStands, 4.76789, 4.76808, 0, 0,
		[](FOracleProblem& Problem) { return ZeroJambBedCohesion(1, Problem); }, 36 });

	Rows.Add({ TEXT("probe: cover head-joint tension x0.5"),
		TEXT("half the tensile bond across the cover's own head joints, nothing else ")
		TEXT("touched — the knob a deep bending panel's capacity is linear in. Halving it ")
		TEXT("costs 2.7% at the mean data (4.8% at the characteristic), where a panel ")
		TEXT("would lose half"),
		CaseTwentyOne,
		ERelation::AgreeStands, 16.76947, 16.77014, 0, 0,
		[](FOracleProblem& Problem)
		{
			return ScaleCoverHeadTension(1, 0.5, Problem);
		}, 43 });

	Rows.Add({ TEXT("probe: cover head-joint tension x2"),
		TEXT("and twice it, so the pair spans a factor of four: a cover carrying its span ")
		TEXT("in bond tension reads 4.00x across the two, and anything else reads 1.00. It ")
		TEXT("reads 1.068 at the mean data (1.151 at the characteristic) — the bond ")
		TEXT("contributes, and it is not what holds the wall up"),
		CaseTwentyOne,
		ERelation::AgreeStands, 17.90343, 17.90415, 0, 0,
		[](FOracleProblem& Problem)
		{
			return ScaleCoverHeadTension(1, 2.0, Problem);
		}, 43 });

	Rows.Add({ TEXT("probe: first-crack rows on"),
		TEXT("Slice 0d's flag-on window: the whole wall re-solved with uncracked first-crack ")
		TEXT("rows on every bonded joint. lambda* falls only 12.3% (0.8769) to 15.117 and the ")
		TEXT("wall STANDS — brittle bond bending is a minor term here, the §4 confirmation; it ")
		TEXT("does not flip the verdict"),
		CaseTwentyOne,
		ERelation::AgreeStands, 15.11658, 15.11718, 0, 0,
		TurnOnFirstCrackRows, 133 });

	TArray<FSweepReading> Readings;
	RunRows(*this, Rows, Readings);

	const FSweepReading* Control = ReadingNamed(
		*this, Rows, Readings, TEXT("probe control: case 21 with every strength as built"));
	const FSweepReading* NoCohesion = ReadingNamed(
		*this, Rows, Readings, TEXT("probe: jamb bed joints, cohesion zeroed"));
	const FSweepReading* HalfTension = ReadingNamed(
		*this, Rows, Readings, TEXT("probe: cover head-joint tension x0.5"));
	const FSweepReading* DoubleTension = ReadingNamed(
		*this, Rows, Readings, TEXT("probe: cover head-joint tension x2"));
	const FSweepReading* FirstCrack = ReadingNamed(
		*this, Rows, Readings, TEXT("probe: first-crack rows on"));

	if (Control == nullptr || NoCohesion == nullptr || HalfTension == nullptr
		|| DoubleTension == nullptr || FirstCrack == nullptr)
	{
		return true;
	}

	CheckRungSize(*this, TEXT("probe: first-crack on"), *FirstCrack, 83);

	CheckReadingRatio(
		*this,
		TEXT("the first-crack flag changed the LP only (production reading vs control)"),
		FirstCrack->WorstUtilisation, Control->WorstUtilisation, 0.999999999, 1.000000001);

	CheckLambdaLadderRatio(
		*this,
		TEXT("SLICE 0d: case 21 under first crack STANDS and does not flip"),
		TEXT("a bond-bending panel predicts 0.3333 (/3); case 21 measures 0.8769 and stays ")
		TEXT("above 1.0 — bond bending is minor, the residual is friction+compression"),
		*FirstCrack, *Control, 0.87689, 0.87692);

	// Same wall and same production reading on every row, so a moved lambda* is a strength finding.
	CheckRungSize(*this, TEXT("probe control"), *Control, 83);
	CheckRungSize(*this, TEXT("probe: cohesion zeroed"), *NoCohesion, 83);
	CheckRungSize(*this, TEXT("probe: tension x0.5"), *HalfTension, 83);
	CheckRungSize(*this, TEXT("probe: tension x2"), *DoubleTension, 83);

	CheckReadingRatio(
		*this,
		TEXT("the cohesion probe changed the LP only (production reading vs control)"),
		NoCohesion->WorstUtilisation, Control->WorstUtilisation, 0.999999999, 1.000000001);

	CheckReadingRatio(
		*this,
		TEXT("the x0.5 tension probe changed the LP only (production reading vs control)"),
		HalfTension->WorstUtilisation, Control->WorstUtilisation, 0.999999999, 1.000000001);

	CheckReadingRatio(
		*this,
		TEXT("the x2 tension probe changed the LP only (production reading vs control)"),
		DoubleTension->WorstUtilisation, Control->WorstUtilisation, 0.999999999, 1.000000001);

	CheckLambdaLadderRatio(
		*this,
		TEXT("THE CLOSING EXPERIMENT: case 21 with its jamb bed cohesion taken away"),
		TEXT("the jamb-shear-chain hypothesis predicts a collapse toward the residue, ")
		TEXT("anything else predicts 1.00 — the mean data measures 0.2766 (0.1425 at the ")
		TEXT("characteristic, where it also crossed 1.0 absolute): the dominant single ")
		TEXT("term, no longer the whole capacity"),
		*NoCohesion, *Control, 0.27657, 0.27660);

	CheckLambdaLadderRatio(
		*this,
		TEXT("THE COVER'S OWN BOND, over a four-fold lever (tension x2 / tension x0.5)"),
		TEXT("a cover spanning in bond tension predicts 4.00, a cover whose tension is ")
		TEXT("incidental predicts 1.00 — the mean data measures 1.068 (1.151 at the ")
		TEXT("characteristic), so the bond contributes little of what carrying the span ")
		TEXT("would require"),
		*DoubleTension, *HalfTension, 1.06757, 1.06767);

	// Proves the knob is connected; the four-fold ratio alone would pass for an unresponsive solver.
	CheckLambdaLadderRatio(
		*this,
		TEXT("the low half of the tension lever (tension x0.5 / control)"),
		TEXT("a bond-governed cover predicts 0.50, an unaffected one 1.00 — the mean data ")
		TEXT("measures 0.9728 (0.952 at the characteristic)"),
		*HalfTension, *Control, 0.97274, 0.97284);

	return true;
}

/**
 * Slice 0c: attribute case 21's residual lambda* = 4.768 (jamb bed cohesion zeroed), a
 * precondition on promoting the LP (PROMOTION_DESIGN §4.4). Measured: crushing is inert (~500x
 * clear); zeroing friction raises lambda* to 9.935 because mu=0 unlocks the tensile bond, and
 * zeroing that too collapses it to 3.747; bearing-course cohesion is the tighter link.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockSlowCase21ResidualTest,
	"OracleSweepFast.RigidBlock.Case21ResidualAttribution",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockSlowCase21ResidualTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	// Every row is case 21's own wall.
	const auto CaseTwentyOne = [](FStructure& Out, FString& Why)
	{
		return BuildOpeningLadderWall(18, 1, 2, INDEX_NONE, Out, Why);
	};

	// The full below-cover band: all 36 jamb bed joints.
	const double WholeChainLoZCm = -1.0e9;
	const double WholeChainHiZCm = 1.0e9;
	const double BearingSplitZCm = LadderChainBearingSplitZCm(1);

	TArray<FSweepRow> Rows;

	// Residual control: case 21 with jamb bed cohesion gone (4.768). Every probe is read against it.
	Rows.Add({ TEXT("residual control: case 21 with jamb bed cohesion zeroed"),
		TEXT("the 4.768 residual itself — jamb bed cohesion gone, everything else as built. ")
		TEXT("The state the three probes strip further and the two split rows re-pose; its ")
		TEXT("lambda* is the value OpeningStrengthProbes already pins"),
		CaseTwentyOne,
		ERelation::AgreeStands, 4.76789, 4.76808, 0, 0,
		[WholeChainLoZCm, WholeChainHiZCm](FOracleProblem& Problem)
		{
			return AdjustJambBedResidual(
				1, true, false, false, false, WholeChainLoZCm, WholeChainHiZCm, Problem);
		}, 36 });

	Rows.Add({ TEXT("probe 1: friction also zeroed on the whole jamb chain"),
		TEXT("cohesion AND the Coulomb mu gone from every jamb bed joint. lambda* RISES from ")
		TEXT("4.768 to 9.935: disallowing bed-joint shear lifts the compression coupling and ")
		TEXT("unlocks the 0.70 MPa tensile bond (left intact here), so the wall re-forms on a ")
		TEXT("DISTINCT, higher bond-tension mechanism — friction did not cap a compression ")
		TEXT("arch. CONFIRMS PROMOTION_DESIGN §4.2's non-brittle residual; probe 5 proves the ")
		TEXT("9.935 is bond tension"),
		CaseTwentyOne,
		ERelation::AgreeStands, 9.93490, 9.93494, 0, 0,
		[WholeChainLoZCm, WholeChainHiZCm](FOracleProblem& Problem)
		{
			return AdjustJambBedResidual(
				1, true, true, false, false, WholeChainLoZCm, WholeChainHiZCm, Problem);
		}, 36 });

	Rows.Add({ TEXT("probe 2: crushing cap relaxed on the whole jamb chain"),
		TEXT("cohesion gone and the crushing cap lifted to effectively infinite, friction ")
		TEXT("left in place. If compression bearing is what the residual runs on and the ")
		TEXT("joints are crushing-limited, lambda* rises; PREDICTED UNCHANGED because ")
		TEXT("crushing sits ~500x clear of demand"),
		CaseTwentyOne,
		ERelation::AgreeStands, 4.76789, 4.76808, 0, 0,
		[WholeChainLoZCm, WholeChainHiZCm](FOracleProblem& Problem)
		{
			return AdjustJambBedResidual(
				1, true, false, true, false, WholeChainLoZCm, WholeChainHiZCm, Problem);
		}, 36 });

	Rows.Add({ TEXT("probe 3: friction zeroed AND crushing relaxed on the whole jamb chain"),
		TEXT("both non-cohesive mechanisms removed at once. Reads probe 1's 9.935 value — a ")
		TEXT("friction-freed arch does not care how high the crushing cap is; the identity ")
		TEXT("against probe 1 is the finding"),
		CaseTwentyOne,
		ERelation::AgreeStands, 9.93490, 9.93494, 0, 0,
		[WholeChainLoZCm, WholeChainHiZCm](FOracleProblem& Problem)
		{
			return AdjustJambBedResidual(
				1, true, true, true, false, WholeChainLoZCm, WholeChainHiZCm, Problem);
		}, 36 });

	// Chain-vs-bearing split: cohesion zeroed in each half of the 36-joint chain (18 each).
	Rows.Add({ TEXT("split: cohesion zeroed in the bearing courses only"),
		TEXT("cohesion removed only from the top of the jamb, where the cover bears. Reads ")
		TEXT("4.76816 — essentially the whole 4.768 residual — so the bearing courses are ")
		TEXT("the tighter link for cohesion, NOT the run to ground. Refutes 'thrust runs to ")
		TEXT("ground' for the cohesion contribution specifically"),
		CaseTwentyOne,
		ERelation::AgreeStands, 4.76814, 4.76818, 0, 0,
		[BearingSplitZCm, WholeChainHiZCm](FOracleProblem& Problem)
		{
			return AdjustJambBedResidual(
				1, true, false, false, false, BearingSplitZCm, WholeChainHiZCm, Problem);
		}, 18 });

	Rows.Add({ TEXT("split: cohesion zeroed in the run to ground only"),
		TEXT("cohesion removed only from the lower jamb, its run to the foundation. Reads ")
		TEXT("5.96237 — a WEAKER effect than the bearing band, refuting the prediction that ")
		TEXT("the run to ground is the tighter link for cohesion. Removing cohesion here ")
		TEXT("leaves more of the residual standing than removing it in the bearing courses"),
		CaseTwentyOne,
		ERelation::AgreeStands, 5.96235, 5.96239, 0, 0,
		[WholeChainLoZCm, BearingSplitZCm](FOracleProblem& Problem)
		{
			return AdjustJambBedResidual(
				1, true, false, false, false, WholeChainLoZCm, BearingSplitZCm, Problem);
		}, 18 });

	Rows.Add({ TEXT("probe 5: friction AND jamb-bed tension both zeroed on the whole jamb chain"),
		TEXT("cohesion, the Coulomb mu AND the 0.70 MPa tensile bond all gone from every jamb ")
		TEXT("bed joint — compression only. The bond-tension mechanism probe 1 unlocked cannot ")
		TEXT("form, so lambda* COLLAPSES from 9.935 to 3.74716 — below even the 4.768 frictional ")
		TEXT("residual (which kept its friction-shear). Empirical proof the 9.935 was bond tension"),
		CaseTwentyOne,
		ERelation::AgreeStands, 3.74714, 3.74718, 0, 0,
		[WholeChainLoZCm, WholeChainHiZCm](FOracleProblem& Problem)
		{
			return AdjustJambBedResidual(
				1, true, true, false, true, WholeChainLoZCm, WholeChainHiZCm, Problem);
		}, 36 });

	TArray<FSweepReading> Readings;
	RunRows(*this, Rows, Readings);

	const FSweepReading* Control = ReadingNamed(
		*this, Rows, Readings,
		TEXT("residual control: case 21 with jamb bed cohesion zeroed"));
	const FSweepReading* FrictionOff = ReadingNamed(
		*this, Rows, Readings,
		TEXT("probe 1: friction also zeroed on the whole jamb chain"));
	const FSweepReading* CrushingOff = ReadingNamed(
		*this, Rows, Readings,
		TEXT("probe 2: crushing cap relaxed on the whole jamb chain"));
	const FSweepReading* Both = ReadingNamed(
		*this, Rows, Readings,
		TEXT("probe 3: friction zeroed AND crushing relaxed on the whole jamb chain"));
	const FSweepReading* Bearing = ReadingNamed(
		*this, Rows, Readings,
		TEXT("split: cohesion zeroed in the bearing courses only"));
	const FSweepReading* RunToGround = ReadingNamed(
		*this, Rows, Readings,
		TEXT("split: cohesion zeroed in the run to ground only"));
	const FSweepReading* FrictionAndTensionOff = ReadingNamed(
		*this, Rows, Readings,
		TEXT("probe 5: friction AND jamb-bed tension both zeroed on the whole jamb chain"));

	if (Control == nullptr || FrictionOff == nullptr || CrushingOff == nullptr
		|| Both == nullptr || Bearing == nullptr || RunToGround == nullptr
		|| FrictionAndTensionOff == nullptr)
	{
		return true;
	}

	CheckRungSize(*this, TEXT("residual control"), *Control, 83);
	CheckRungSize(*this, TEXT("probe 1 friction off"), *FrictionOff, 83);
	CheckRungSize(*this, TEXT("probe 2 crushing relaxed"), *CrushingOff, 83);
	CheckRungSize(*this, TEXT("probe 3 both"), *Both, 83);
	CheckRungSize(*this, TEXT("split bearing"), *Bearing, 83);
	CheckRungSize(*this, TEXT("split run to ground"), *RunToGround, 83);
	CheckRungSize(*this, TEXT("probe 5 friction and tension off"), *FrictionAndTensionOff, 83);

	CheckReadingRatio(
		*this, TEXT("probe 1 changed the LP only (production vs control)"),
		FrictionOff->WorstUtilisation, Control->WorstUtilisation, 0.999999999, 1.000000001);
	CheckReadingRatio(
		*this, TEXT("probe 2 changed the LP only (production vs control)"),
		CrushingOff->WorstUtilisation, Control->WorstUtilisation, 0.999999999, 1.000000001);
	CheckReadingRatio(
		*this, TEXT("probe 5 changed the LP only (production vs control)"),
		FrictionAndTensionOff->WorstUtilisation, Control->WorstUtilisation,
		0.999999999, 1.000000001);
	CheckReadingRatio(
		*this, TEXT("the split rows changed the LP only (bearing vs run-to-ground reading)"),
		Bearing->WorstUtilisation, RunToGround->WorstUtilisation, 0.999999999, 1.000000001);

	TestEqual(
		TEXT("the two split bands must partition the whole jamb chain: bearing + run == 36"),
		Bearing->JointsOverridden + RunToGround->JointsOverridden, 36);
	TestTrue(
		TEXT("the bearing band must have matched at least one jamb bed joint"),
		Bearing->JointsOverridden > 0);
	TestTrue(
		TEXT("the run-to-ground band must have matched at least one jamb bed joint"),
		RunToGround->JointsOverridden > 0);

	// Finding 1: crushing does not bind (relaxing a binding cap would raise lambda*).
	CheckSameLambda(
		*this,
		TEXT("crushing relaxed vs the residual control"),
		*CrushingOff, *Control, 1.0e-6);

	// Finding 2: with friction gone, the crushing cap changes nothing.
	CheckSameLambda(
		*this,
		TEXT("both (friction off + crushing relaxed) vs friction off alone"),
		*Both, *FrictionOff, 1.0e-6);

	/*
	 * Finding 3: zeroing friction raises lambda*. A net-compression mu=0 optimum is bounded by the
	 * control, so a rise proves the tensile bond is mobilised (PROMOTION_DESIGN §4.2).
	 */
	AddInfo(FString::Printf(
		TEXT("FRICTION-OFF-UNLOCKS-BOND: residual control lambda* %.17g, friction-off %.17g, ")
		TEXT("ratio %.17g (mu=0 forbids shear and unlocks the tensile bond — a distinct, ")
		TEXT("higher mechanism, not friction capping an arch)"),
		Control->Oracle.Lambda, FrictionOff->Oracle.Lambda,
		FrictionOff->Oracle.Lambda / Control->Oracle.Lambda));
	TestTrue(
		*FString::Printf(
			TEXT("zeroing friction must UNLOCK a higher mechanism, not cap the residual: ")
			TEXT("friction-off lambda* (%.17g) must be STRICTLY GREATER than the residual ")
			TEXT("control (%.17g). With cohesion gone, mu=0 forbids bed-joint shear and lifts ")
			TEXT("the compression coupling, so the 0.70 MPa tensile bond becomes usable and the ")
			TEXT("wall re-forms on a DISTINCT bond-tension mechanism that stands higher — a ")
			TEXT("mu=0 optimum in net compression everywhere would be bounded by the control, ")
			TEXT("so a rise proves net tension is mobilised (probe 5 removes that bond)"),
			FrictionOff->Oracle.Lambda, Control->Oracle.Lambda),
		FrictionOff->Oracle.Lambda > Control->Oracle.Lambda);

	/*
	 * Finding 5: remove the bond too and the joints are compression-only, a subset of the control,
	 * so lambda* must fall to at or below it.
	 */
	AddInfo(FString::Printf(
		TEXT("BOND-TENSION-COLLAPSE: friction-off (bond intact) lambda* %.17g, friction+tension ")
		TEXT("off %.17g, residual control %.17g — removing the 0.70 MPa bond drops the mechanism"),
		FrictionOff->Oracle.Lambda, FrictionAndTensionOff->Oracle.Lambda, Control->Oracle.Lambda));
	TestTrue(
		*FString::Printf(
			TEXT("removing the jamb-bed tensile bond must COLLAPSE the 9.935 mechanism: ")
			TEXT("friction+tension-off lambda* (%.17g) must be STRICTLY LESS than friction-off ")
			TEXT("alone (%.17g). If it did not fall, the 9.935 was NOT carried by bond tension ")
			TEXT("and finding 3's attribution is overturned"),
			FrictionAndTensionOff->Oracle.Lambda, FrictionOff->Oracle.Lambda),
		FrictionAndTensionOff->Oracle.Lambda < FrictionOff->Oracle.Lambda);
	TestTrue(
		*FString::Printf(
			TEXT("with shear and tension both forbidden the residual must return to at-or-below ")
			TEXT("the frictional control: friction+tension-off lambda* (%.17g) must be <= the ")
			TEXT("control (%.17g) plus a small margin — a compression-only bed joint is a strict ")
			TEXT("capacity subset of the mu=0.75 control, whose optimum bounds it"),
			FrictionAndTensionOff->Oracle.Lambda, Control->Oracle.Lambda),
		FrictionAndTensionOff->Oracle.Lambda <= Control->Oracle.Lambda + 1.0e-6);

	// Finding 4: for cohesion, the bearing courses are the tighter link, not the run to ground.
	AddInfo(FString::Printf(
		TEXT("CHAIN-VS-BEARING: cohesion-off bearing lambda* %.17g, run-to-ground %.17g ")
		TEXT("(control 17.24 as built, residual 4.768 whole-chain)"),
		Bearing->Oracle.Lambda, RunToGround->Oracle.Lambda));
	TestTrue(
		*FString::Printf(
			TEXT("the bearing courses must be the tighter link for cohesion: bearing ")
			TEXT("lambda* (%.17g) < run-to-ground lambda* (%.17g). Removing cohesion at the ")
			TEXT("top collapses the residual more than removing it in the run to ground — ")
			TEXT("thrust does NOT run to ground for the cohesion term"),
			Bearing->Oracle.Lambda, RunToGround->Oracle.Lambda),
		Bearing->Oracle.Lambda < RunToGround->Oracle.Lambda);

	return true;
}

/**
 * A bounded, feasible problem must not come back "phase-2 simplex failed". The 99- and 107-block
 * rungs once refused because the ratio test had no sense of scale (it accepted a pivot 1.6e-16 of
 * its column); RelativePivotTol in RigidBlockOracle.cpp fixes it, and reverting it reproduces the
 * red.
 *
 * Each refuser must answer and land between its two ladder neighbours from the same run. Brackets,
 * not pinned numbers, because lambda* at 100+ blocks reproduces only to ~1e-5. Do not fix this by
 * loosening the verification gate, the iteration cap or the optimality tolerance: an early stop
 * reports a too-low lambda* that verification still certifies.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockPhaseTwoBoundedTest,
	"OracleSweepFast.RigidBlock.PhaseTwoMustNotRefuseABoundedProblem",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockPhaseTwoBoundedTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	struct FRung
	{
		const TCHAR* Name = nullptr;
		int32 OpeningCells = 0;
		int32 CoursesBelow = 0;
		int32 JambCells = 0;

		/** The fixture's size, pinned: only this catches a rung building another's wall. */
		int32 WantBlocks = 0;
		int32 WantJoints = 0;

		/** A loose sanity window on an answering neighbour; both zero on a rung under test. */
		double SanityLo = 0.0;
		double SanityHi = 0.0;

		/** A +/-2e-5 window around a certified reading; both zero where none exists. */
		double CertifiedLo = 0.0;
		double CertifiedHi = 0.0;
	};

	// The 99er's certified window rests on one reading; a second is owed with the next pivot-path change.
	const FRung Rungs[] =
	{
		{ TEXT("span ladder, 8-cell opening (the wide-lambda neighbour)"),
			8, 3, 3, 94, 206, 115.9, 118.2, 0.0, 0.0 },
		{ TEXT("span ladder, 9-cell opening (THE 99-BLOCK REFUSER)"),
			9, 3, 3, 99, 216, 0.0, 0.0, 89.8971, 89.9007 },
		{ TEXT("span ladder, 10-cell opening (the narrow-lambda neighbour)"),
			10, 3, 3, 104, 226, 68.0, 69.4, 0.0, 0.0 },
		{ TEXT("abutment ladder, 3-cell jamb (the narrow-lambda neighbour)"),
			18, 1, 3, 95, 163, 17.71, 18.07, 0.0, 0.0 },
		{ TEXT("abutment ladder, 4-cell jamb (THE 107-BLOCK REFUSER)"),
			18, 1, 4, 107, 193, 0.0, 0.0, 0.0, 0.0 },
		{ TEXT("abutment ladder, 5-cell jamb (the wide-lambda neighbour)"),
			18, 1, 5, 119, 223, 17.96, 18.33, 0.0, 0.0 },
	};

	constexpr int32 NumRungs = UE_ARRAY_COUNT(Rungs);

	FOracleResult Results[NumRungs];
	bool bMeasured[NumRungs] = {};

	for (int32 Index = 0; Index < NumRungs; ++Index)
	{
		const FRung& Rung = Rungs[Index];

		FStructure Structure;
		FString BuildWhy;

		if (!BuildOpeningLadderWall(
				Rung.OpeningCells, Rung.CoursesBelow, Rung.JambCells, INDEX_NONE,
				Structure, BuildWhy))
		{
			AddError(FString::Printf(
				TEXT("%s: FIXTURE could not be laid: %s"), Rung.Name, *BuildWhy));

			continue;
		}

		FOracleProblem Problem;
		FString BridgeWhy;

		// Call first: Printf argument order is unsequenced and could read the empty reason (TRAPS).
		const bool bBridged = BuildRigidBlockProblem(Structure, Problem, BridgeWhy);

		if (!TestTrue(
				*FString::Printf(
					TEXT("%s: the bridge must represent this fixture (it said: %s)"),
					Rung.Name, *BridgeWhy),
				bBridged))
		{
			continue;
		}

		const double Started = FPlatformTime::Seconds();
		Results[Index] = SolveRigidBlock(Problem);
		const double Seconds = FPlatformTime::Seconds() - Started;
		bMeasured[Index] = true;

		const FString Line = FString::Printf(
			TEXT("PHASE2 %s: blocks=%d joints=%d answered=%d lambda=%.17g pivots=%d ")
			TEXT("bland=%d secs=%.2f%s%s"),
			Rung.Name, Problem.Blocks.Num(), Problem.Joints.Num(),
			Results[Index].bAnswered ? 1 : 0, Results[Index].Lambda,
			Results[Index].SimplexIterations, Results[Index].BlandDegenerateEntries, Seconds,
			Results[Index].WhyNot.IsEmpty() ? TEXT("") : TEXT(" | whynot: "),
			Results[Index].WhyNot.IsEmpty() ? TEXT("") : *Results[Index].WhyNot);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);

		TestEqual(
			*FString::Printf(
				TEXT("%s: the rung must be %d blocks and was %d — a size pin is the only ")
				TEXT("thing that catches a rung quietly building its neighbour's wall"),
				Rung.Name, Rung.WantBlocks, Problem.Blocks.Num()),
			Problem.Blocks.Num(), Rung.WantBlocks);

		TestEqual(
			*FString::Printf(
				TEXT("%s: the rung must carry %d joints and carried %d"),
				Rung.Name, Rung.WantJoints, Problem.Joints.Num()),
			Problem.Joints.Num(), Rung.WantJoints);
	}

	// Neighbour sanity windows stop the brackets being satisfiable by garbage.
	for (int32 Index = 0; Index < NumRungs; ++Index)
	{
		const FRung& Rung = Rungs[Index];

		if (!bMeasured[Index] || Rung.SanityHi <= 0.0)
		{
			continue;
		}

		if (!TestTrue(
				*FString::Printf(
					TEXT("%s: this neighbour answers today and must keep answering (it said: ")
					TEXT("%s)"),
					Rung.Name, *Results[Index].WhyNot),
				Results[Index].bAnswered))
		{
			continue;
		}

		TestTrue(
			*FString::Printf(
				TEXT("%s: lambda* %.17g must stay inside the loose sanity window ")
				TEXT("[%.6g, %.6g] — a deliberately slack floor under the brackets, a ")
				TEXT("thousand times wider than the ~1e-5 a changed pivot path moves it"),
				Rung.Name, Results[Index].Lambda, Rung.SanityLo, Rung.SanityHi),
			Results[Index].Lambda >= Rung.SanityLo && Results[Index].Lambda <= Rung.SanityHi);
	}

	// A certified window sits beside the bracket: the bracket states the physics, this the number.
	for (int32 Index = 0; Index < NumRungs; ++Index)
	{
		const FRung& Rung = Rungs[Index];

		if (!bMeasured[Index] || !(Rung.CertifiedHi > 0.0) || !Results[Index].bAnswered)
		{
			continue;
		}

		TestTrue(
			*FString::Printf(
				TEXT("%s: lambda* %.17g must lie in [%.9g, %.9g] — the midpoint of TWO ")
				TEXT("certified readings taken by different pivot paths, +/-2e-5 relative, ")
				TEXT("which is ~1e4 times the two ulps those paths actually differed by. A ")
				TEXT("move inside this window is the algorithm; a move outside it is the ")
				TEXT("physics, and the bracket beside it says which direction is even legal"),
				Rung.Name, Results[Index].Lambda, Rung.CertifiedLo, Rung.CertifiedHi),
			Results[Index].Lambda >= Rung.CertifiedLo
				&& Results[Index].Lambda <= Rung.CertifiedHi);
	}

	// Each refuser must answer, and land between its two ladder neighbours.
	const auto CheckBracketed =
		[&](int32 Under, int32 Low, int32 High, const TCHAR* Why)
	{
		if (!bMeasured[Under] || !bMeasured[Low] || !bMeasured[High])
		{
			return;
		}

		if (!TestTrue(
				*FString::Printf(
					TEXT("%s: the oracle MUST ANSWER this fixture — its LP is bounded by the ")
					TEXT("lambda-cap row and an admissible optimum exists — reachable and ")
					TEXT("verifiable, the ratio test's missing sense of scale being what ")
					TEXT("stood between the solver and it. It refused after %d pivots (%d ")
					TEXT("of them in the Bland fallback), saying: %s"),
					Rungs[Under].Name, Results[Under].SimplexIterations,
					Results[Under].BlandDegenerateEntries, *Results[Under].WhyNot),
				Results[Under].bAnswered))
		{
			return;
		}

		if (!Results[Low].bAnswered || !Results[High].bAnswered)
		{
			return;
		}

		TestTrue(
			*FString::Printf(
				TEXT("%s: lambda* %.17g must lie strictly between its ladder neighbours ")
				TEXT("(%.17g and %.17g) — %s. Failing THIS while the answer arrives is a ")
				TEXT("different finding from refusing: it says the solver answered, wrongly."),
				Rungs[Under].Name, Results[Under].Lambda, Results[Low].Lambda,
				Results[High].Lambda, Why),
			Results[Under].Lambda > Results[Low].Lambda
				&& Results[Under].Lambda < Results[High].Lambda);
	};

	CheckBracketed(1, 2, 0,
		TEXT("lambda* falls with the clear reveal (158.5 / 181.0 / 203.5 cm across these "
			"three rungs), so a 9-cell opening cannot be stronger than an 8-cell one nor "
			"weaker than a 10-cell one"));

	CheckBracketed(4, 3, 5,
		TEXT("more jamb is more bearing and more weight over it, and this ladder reads "
			"7.379 at three cells and 11.115 at five"));

	return true;
}

/**
 * A refusal must name which termination produced it (slice 0a). Each reason is an enumerator with
 * its own phrase, so iteration cap, unbounded ray and numerical failure are distinguishable. The
 * taxonomy is checked pairwise-distinct; fixtures tie None and InvalidProblem to real solves.
 * No fixture reaches the phase-2 arms today, so a wrong-reason site there would go uncaught
 * (CURRENT_STATE).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockRefusalReasonTest,
	"OracleSweepFast.RigidBlock.RefusalNamesItsReason",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockRefusalReasonTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	// 1. Every refusing reason reads differently from every other.

	struct FReason
	{
		EOracleRefusal Value;
		const TCHAR* Name;
	};

	const FReason Reasons[] =
	{
		{ EOracleRefusal::InvalidProblem,           TEXT("InvalidProblem") },
		{ EOracleRefusal::PhaseOneFailure,          TEXT("PhaseOneFailure") },
		{ EOracleRefusal::PhaseTwoIterationCap,     TEXT("PhaseTwoIterationCap") },
		{ EOracleRefusal::PhaseTwoUnbounded,        TEXT("PhaseTwoUnbounded") },
		{ EOracleRefusal::PhaseTwoNumericalFailure, TEXT("PhaseTwoNumericalFailure") },
		{ EOracleRefusal::VerificationFailure,      TEXT("VerificationFailure") },
	};

	TestTrue(
		TEXT("an ANSWERED result has no reason, so RefusalText(None) is empty — the reason "
			"and bAnswered must be checkable against each other, which they cannot be if "
			"None carries a sentence of its own"),
		RefusalText(EOracleRefusal::None).IsEmpty());

	for (const FReason& Reason : Reasons)
	{
		TestTrue(
			*FString::Printf(
				TEXT("%s must have a reason phrase — an unnamed refusal is the state this "
					"test exists to end"),
				Reason.Name),
			!RefusalText(Reason.Value).IsEmpty());
	}

	constexpr int32 NumReasons = UE_ARRAY_COUNT(Reasons);

	for (int32 First = 0; First < NumReasons; ++First)
	{
		for (int32 Second = First + 1; Second < NumReasons; ++Second)
		{
			const FString FirstText = RefusalText(Reasons[First].Value);
			const FString SecondText = RefusalText(Reasons[Second].Value);

			TestTrue(
				*FString::Printf(
					TEXT("%s and %s must not read the same, and both read \"%s\" — two "
						"terminations with one sentence between them is exactly what forced "
						"an instrumented build twice this week"),
					Reasons[First].Name, Reasons[Second].Name, *FirstText),
				FirstText != SecondText);
		}
	}

	// 2. An answered fixture reports no reason.

	FStructure Standing;
	FString StandingWhy;

	// Call first: Printf argument order is unsequenced and could read the empty reason (TRAPS).
	const bool bStandingLaid = BuildCoveredOpeningWall(2, 18, 2, Standing, StandingWhy);

	if (TestTrue(
			*FString::Printf(
				TEXT("the answering control must be laid (it said: %s)"), *StandingWhy),
			bStandingLaid))
	{
		FOracleProblem Problem;
		FString BridgeWhy;

		const bool bBridged = BuildRigidBlockProblem(Standing, Problem, BridgeWhy);

		if (TestTrue(
				*FString::Printf(
					TEXT("the answering control must bridge (it said: %s)"), *BridgeWhy),
				bBridged))
		{
			const FOracleResult Answered = SolveRigidBlock(Problem);

			TestTrue(
				*FString::Printf(
					TEXT("CONTROL (case 21's own rung, 83 blocks): it answers today at "
						"lambda* ~17.24 and must keep answering, or every row below is "
						"asserting against a solver that refuses everything (it said: %s)"),
					*Answered.WhyNot),
				Answered.bAnswered);

			TestTrue(
				TEXT("an answered result carries EOracleRefusal::None — the reason field and "
					"bAnswered must agree, and this is the arm that catches a reason left "
					"set from a previous solve"),
				Answered.Refusal == EOracleRefusal::None);

			TestTrue(
				*FString::Printf(
					TEXT("an answered result carries no sentence either, and carried \"%s\""),
					*Answered.WhyNot),
				Answered.WhyNot.IsEmpty());
		}
	}

	// 3. A refusal before the simplex names validation, not a simplex arm.

	{
		FStructure Poisonable;
		FString PoisonWhy;

		const bool bPoisonableLaid =
			BuildCoveredOpeningWall(2, 18, 2, Poisonable, PoisonWhy);

		if (TestTrue(
				*FString::Printf(
					TEXT("the poisoned fixture must be laid (it said: %s)"), *PoisonWhy),
				bPoisonableLaid))
		{
			FOracleProblem Problem;
			FString BridgeWhy;

			if (BuildRigidBlockProblem(Poisonable, Problem, BridgeWhy)
				&& Problem.Joints.Num() > 0)
			{
				// A non-unit normal: refused by ValidateProblem.
				Problem.Joints[0].NormalX = 0.5;
				Problem.Joints[0].NormalZ = 0.5;

				const FOracleResult Refused = SolveRigidBlock(Problem);

				TestTrue(
					TEXT("a poisoned problem is still refused"),
					!Refused.bAnswered);

				TestTrue(
					*FString::Printf(
						TEXT("a validation refusal reports InvalidProblem and reported %d — "
							"the reason must separate 'the problem was never solvable' from "
							"'the solver could not finish', which is the coarsest split in "
							"the taxonomy and the one a caller branches on first"),
						int32(Refused.Refusal)),
					Refused.Refusal == EOracleRefusal::InvalidProblem);

				TestTrue(
					*FString::Printf(
						TEXT("and the sentence it prints carries that reason's phrase: "
							"\"%s\" must contain \"%s\""),
						*Refused.WhyNot, *RefusalText(EOracleRefusal::InvalidProblem)),
					Refused.WhyNot.Contains(RefusalText(EOracleRefusal::InvalidProblem)));
			}
		}
	}

	return true;
}

/**
 * The covered-opening family (case 22's shape) once refused at 128 and 200 blocks, isolated holes
 * in a smooth curve whose neighbours answer. Cause: the basis was driven LU-singular by the
 * solver's arithmetic (Factorise pivot ~4e-12). Same rules as PhaseTwoMustNotRefuseABoundedProblem:
 * do not fix by loosening the gate, the cap or the tolerance.
 *
 * Window +/-1e-4 (not the usual 2e-5) because the certified readings came from diagnostic solver
 * variants; the bracket states that lambda* falls with the opening. Case 22 answers at 88,810 of
 * 100,000 pivots, close to the cap (CURRENT_STATE).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRigidBlockCoveredOpeningRefusalTest,
	"OracleSweepFull.RigidBlock.PhaseTwoMustNotRefuseTheCoveredOpeningFamily",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRigidBlockCoveredOpeningRefusalTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockSweepTestSupport;

	struct FRung
	{
		const TCHAR* Name = nullptr;
		int32 OpeningCells = 0;

		int32 WantBlocks = 0;
		int32 WantJoints = 0;

		/** A loose sanity window on an answering neighbour; both zero on the rung under test. */
		double SanityLo = 0.0;
		double SanityHi = 0.0;

		/** +/-1e-4 around two certified readings; both zero where none exists. */
		double CertifiedLo = 0.0;
		double CertifiedHi = 0.0;
	};

	const FRung Rungs[] =
	{
		{ TEXT("covered opening, 7 cells (the strong-lambda neighbour)"),
			7, 119, 284, 188.6, 192.5, 0.0, 0.0 },
		{ TEXT("covered opening, 8 cells (THE 128-BLOCK REFUSER)"),
			8, 128, 306, 0.0, 0.0, 155.6164, 155.6476 },
		{ TEXT("covered opening, 9 cells (the weak-lambda neighbour)"),
			9, 137, 328, 126.4, 129.0, 0.0, 0.0 },
		{ TEXT("covered opening, 16 cells (THE 200-BLOCK REFUSER)"),
			16, 200, 482, 0.0, 0.0, 43.1284, 43.1371 },
	};

	constexpr int32 NumRungs = UE_ARRAY_COUNT(Rungs);

	FOracleResult Results[NumRungs];
	bool bMeasured[NumRungs] = {};

	for (int32 Index = 0; Index < NumRungs; ++Index)
	{
		const FRung& Rung = Rungs[Index];

		FStructure Structure;
		FString BuildWhy;

		if (!BuildCoveredOpeningWall(8, Rung.OpeningCells, 2, Structure, BuildWhy))
		{
			AddError(FString::Printf(
				TEXT("%s: FIXTURE could not be laid: %s"), Rung.Name, *BuildWhy));

			continue;
		}

		FOracleProblem Problem;
		FString BridgeWhy;

		// Call first: Printf argument order is unsequenced (TRAPS).
		const bool bBridged = BuildRigidBlockProblem(Structure, Problem, BridgeWhy);

		if (!TestTrue(
				*FString::Printf(
					TEXT("%s: the bridge must represent this fixture (it said: %s)"),
					Rung.Name, *BridgeWhy),
				bBridged))
		{
			continue;
		}

		const double Started = FPlatformTime::Seconds();
		Results[Index] = SolveRigidBlock(Problem);
		const double Seconds = FPlatformTime::Seconds() - Started;
		bMeasured[Index] = true;

		const FString Line = FString::Printf(
			TEXT("COVERED %s: blocks=%d joints=%d answered=%d lambda=%.17g pivots=%d ")
			TEXT("bland=%d secs=%.2f%s%s"),
			Rung.Name, Problem.Blocks.Num(), Problem.Joints.Num(),
			Results[Index].bAnswered ? 1 : 0, Results[Index].Lambda,
			Results[Index].SimplexIterations, Results[Index].BlandDegenerateEntries, Seconds,
			Results[Index].WhyNot.IsEmpty() ? TEXT("") : TEXT(" | whynot: "),
			Results[Index].WhyNot.IsEmpty() ? TEXT("") : *Results[Index].WhyNot);

		UE_LOG(LogTemp, Display, TEXT("%s"), *Line);
		AddInfo(Line);

		TestEqual(
			*FString::Printf(
				TEXT("%s: the rung must be %d blocks and was %d — a size pin is the only ")
				TEXT("thing that catches a rung quietly building its neighbour's wall"),
				Rung.Name, Rung.WantBlocks, Problem.Blocks.Num()),
			Problem.Blocks.Num(), Rung.WantBlocks);

		TestEqual(
			*FString::Printf(
				TEXT("%s: the rung must carry %d joints and carried %d"),
				Rung.Name, Rung.WantJoints, Problem.Joints.Num()),
			Problem.Joints.Num(), Rung.WantJoints);
	}

	// Neighbour sanity windows stop the bracket being satisfiable by garbage.
	for (int32 Index = 0; Index < NumRungs; ++Index)
	{
		const FRung& Rung = Rungs[Index];

		if (!bMeasured[Index] || Rung.SanityHi <= 0.0)
		{
			continue;
		}

		if (!TestTrue(
				*FString::Printf(
					TEXT("%s: this neighbour answers today and must keep answering (it said: ")
					TEXT("%s)"),
					Rung.Name, *Results[Index].WhyNot),
				Results[Index].bAnswered))
		{
			continue;
		}

		TestTrue(
			*FString::Printf(
				TEXT("%s: lambda* %.17g must stay inside the loose sanity window ")
				TEXT("[%.6g, %.6g] — a deliberately slack floor under the bracket, a thousand ")
				TEXT("times wider than the ~1e-5 a changed pivot path moves it"),
				Rung.Name, Results[Index].Lambda, Rung.SanityLo, Rung.SanityHi),
			Results[Index].Lambda >= Rung.SanityLo && Results[Index].Lambda <= Rung.SanityHi);
	}

	// Each refuser must answer, and with the certified number, not merely stop refusing.
	for (int32 Index = 0; Index < NumRungs; ++Index)
	{
		const FRung& Rung = Rungs[Index];

		if (!bMeasured[Index] || !(Rung.CertifiedHi > 0.0))
		{
			continue;
		}

		if (!TestTrue(
				*FString::Printf(
					TEXT("%s: the oracle MUST ANSWER this fixture — its LP is bounded by the ")
					TEXT("lambda-cap row, and it has been SOLVED TWICE during the diagnosis, ")
					TEXT("by two solver variants taking different pivot paths, both clearing ")
					TEXT("the post-solve verification gate. It refused after %d pivots (%d of ")
					TEXT("them in the Bland fallback), saying: %s"),
					Rung.Name, Results[Index].SimplexIterations,
					Results[Index].BlandDegenerateEntries, *Results[Index].WhyNot),
				Results[Index].bAnswered))
		{
			continue;
		}

		TestTrue(
			*FString::Printf(
				TEXT("%s: lambda* %.17g must lie in [%.9g, %.9g] — the midpoint of two ")
				TEXT("certified readings, +/-1e-4 relative. A move inside this window is the ")
				TEXT("algorithm; a move outside it is a different answer, and this fixture's ")
				TEXT("answer is not in dispute"),
				Rung.Name, Results[Index].Lambda, Rung.CertifiedLo, Rung.CertifiedHi),
			Results[Index].Lambda >= Rung.CertifiedLo
				&& Results[Index].Lambda <= Rung.CertifiedHi);
	}

	// The bracket: what the physics allows, closed by neighbours measured in this run.
	if (bMeasured[0] && bMeasured[1] && bMeasured[2])
	{
		if (Results[1].bAnswered && Results[0].bAnswered && Results[2].bAnswered)
		{
			TestTrue(
				*FString::Printf(
					TEXT("covered opening, 8 cells: lambda* %.17g must lie strictly between ")
					TEXT("its neighbours (%.17g at 7 cells and %.17g at 9 cells) — lambda* ")
					TEXT("falls monotonically with the opening across all twelve rungs of this ")
					TEXT("family, so an 8-cell opening cannot be stronger than a 7-cell one ")
					TEXT("nor weaker than a 9-cell one, and the family's L^-2.4 fit predicts ")
					TEXT("~156. Failing THIS while the answer arrives is a different finding ")
					TEXT("from refusing: it says the solver answered, wrongly"),
					Results[1].Lambda, Results[0].Lambda, Results[2].Lambda),
				Results[1].Lambda < Results[0].Lambda && Results[1].Lambda > Results[2].Lambda);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
