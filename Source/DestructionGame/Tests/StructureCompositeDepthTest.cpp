// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"
#include "Tests/ArchingWallTestSupport.h"
#include "Tests/StaircaseWallTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

// Named namespace because unity builds merge TUs and anonymous helpers would collide.
namespace StructureCompositeDepthTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	// Constants derived locally and asserted against the profile, so a wrong production value fails.

	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickWidthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;
	constexpr double MortarJointCm = 1.0;

	constexpr double BrickPitchCm = BrickLengthCm + MortarJointCm;
	constexpr double CoursePitchCm = BrickHeightCm + MortarJointCm;
	constexpr double HalfStepCm = BrickPitchCm / 2.0;

	/** 1.9 g/cm3, density first in the product so it matches Layout::PieceMassKg to the last bit. */
	constexpr double BrickMassKg = 1.9 * BrickLengthCm * BrickWidthCm * BrickHeightCm / 1000.0;

	/** 980 cm/s2. With 1 uu = 1 cm and mass in kg, MassKg * 980 is a force in Unreal units. */
	constexpr double BrickWeightUu = BrickMassKg * 980.0;

	/**
	 * 1 MPa over 1 cm2 is 10000 uu (1 N = 100 uu, 1 cm2 = 100 mm2). Deliberately not
	 * ForceUnitsPerMPaSqCm, so this file fails if that constant is wrong.
	 */
	constexpr double ForceUnitsPerMPaPerSqCm = 100.0 * 100.0;

	// Mean flexural bond f_x1 for general-purpose mortar (characteristic 0.10 retired 2026-08-13).
	constexpr double MortarTensileMPa = 0.70;

	/** The half-brick bed patch a running-bond brick keeps when it loses one of its two seats. */
	constexpr double HalfSeatAreaSqCm = BrickWidthCm * BrickWidthCm;

	/** Half of that patch on each in-plane axis (square). */
	constexpr double HalfSeatHalfExtentCm = BrickWidthCm / 2.0;

	/** How far a half-seated brick's weight acts from the centroid of the patch it keeps. */
	constexpr double HalfSeatEccentricityCm = HalfStepCm / 2.0;

	/** Bed patch section modulus W = (4/3) * h_u * h_v^2, cm3: 179.4817708 for the square patch. */
	constexpr double PatchModulusCm3 =
		(4.0 / 3.0) * HalfSeatHalfExtentCm * HalfSeatHalfExtentCm * HalfSeatHalfExtentCm;

	/**
	 * Composite section W = t * D^2 / 6 for bonded masonry of depth D over a lost support, acting
	 * as a deep beam (ARCHING_DESIGN.md slice 5). At D = 82.5 cm that is 64.8x the patch. The patch
	 * is this formula at D = 10.25 cm, so the two models are nested.
	 */
	constexpr double CompositeModulusCm3(double WallThicknessCm, double DepthCm)
	{
		return WallThicknessCm * DepthCm * DepthCm / 6.0;
	}

	/** Print a double so a comparison that failed in the last bit is readable as one. */
	inline FString Bits(double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	/*
	 * The raking corbel, with the cut's height (CorbelSteps) separate from the wall's
	 * (CoursesHigh). CorbelSteps must be odd: running bond steps half a cell per course, so an even
	 * cut names bricks that are not there.
	 */

	/** The staircase edge: everything left of it in this course is cut. */
	constexpr double RakingVoidEdgeXCm(int32 CorbelSteps, int32 Course)
	{
		return (CorbelSteps + 1 - Course) * HalfStepCm;
	}

	/** The courses the void cuts. Course 0 and everything above stay whole. */
	constexpr int32 LowestVoidCourse = 1;

	constexpr int32 HighestVoidCourse(int32 CorbelSteps)
	{
		return CorbelSteps;
	}

	/**
	 * Corbelled courses: 2 up to one above the cut. Course 1's survivor still rests on two bricks;
	 * the course above the cut is corbelled because the course beneath starts half a step right.
	 */
	constexpr int32 LowestCorbelCourse = 2;

	constexpr int32 HighestCorbelCourse(int32 CorbelSteps)
	{
		return CorbelSteps + 1;
	}

	/** Centre height of a course, cm. */
	constexpr double CourseZCm(int32 Course)
	{
		return BrickHeightCm / 2.0 + Course * CoursePitchCm;
	}

	/**
	 * Force on one corbel step, in brick weights, s steps below the top: F(s) = 1 + s + s(s+1)/4.
	 * Matches StaircaseWallTestSupport's hand count (1, 2.5, 4.5, 7, ...).
	 */
	constexpr double LadderForceBrickWeights(int32 StepsBelowTop)
	{
		return 1.0 + StepsBelowTop + StepsBelowTop * (StepsBelowTop + 1) / 4.0;
	}

	/**
	 * Moment on that step, brick-weight-cm: M(0) = 5.625, M(s) = 5.625 + 11.25 * F(s-1) + M(s-1),
	 * in closed form. Unrolls to 5.625, 22.5, 56.25, ... 1608.75.
	 */
	constexpr double LadderMomentBrickWeightCm(int32 StepsBelowTop)
	{
		const double S = StepsBelowTop;

		const double SumOfForces =
			S + S * (S - 1.0) / 2.0 + (S - 1.0) * S * (S + 1.0) / 12.0;

		return HalfSeatEccentricityCm * (S + 1.0) + HalfStepCm * SumOfForces;
	}

	/**
	 * Courses of masonry over one step's bed joint (11 for the staircase's bottom step). This is
	 * the unbounded reading, the most any depth rule may credit, so it is asserted only where the
	 * wall stops at the top of the corbel.
	 */
	constexpr int32 CoursesOverCorbelJoint(int32 CoursesHigh, int32 Course)
	{
		return CoursesHigh - Course;
	}

	/** Patch reading: bending on the bed patch less the compression closing it, MPa. */
	inline double PatchTensionMPa(double MomentBrickWeightCm, double ForceBrickWeights)
	{
		const double BendingMPa = MomentBrickWeightCm * BrickWeightUu
			/ (PatchModulusCm3 * ForceUnitsPerMPaPerSqCm);

		const double NormalMPa = ForceBrickWeights * BrickWeightUu
			/ (HalfSeatAreaSqCm * ForceUnitsPerMPaPerSqCm);

		return FMath::Max(0.0, BendingMPa - NormalMPa);
	}

	/**
	 * Composite reading: pure bending on t*D^2/6, with no axial relief. The resisting plane is
	 * vertical, so there is no compression on it to subtract. Subtracting an axial term instead
	 * reads zero for every fixture here, making walls indestructible.
	 */
	inline double CompositeTensionMPa(
		double MomentBrickWeightCm, double DepthCm, double WallThicknessCm)
	{
		return MomentBrickWeightCm * BrickWeightUu
			/ (CompositeModulusCm3(WallThicknessCm, DepthCm) * ForceUnitsPerMPaPerSqCm);
	}

	/**
	 * Expected utilisation: min(patch, composite) / f_x1. Composite action may only help. At one
	 * course (D = 7.5 cm, W = 96.09 < 179.48) the min keeps the patch, which is why the top-course
	 * anchors in StructureBinding and StructurePushTest stay at 0.058203838.
	 */
	inline double CorbelUtilisation(
		double MomentBrickWeightCm,
		double ForceBrickWeights,
		int32 CoursesOfDepth,
		double WallThicknessCm,
		double TensileStrengthMPa)
	{
		const double Patch = PatchTensionMPa(MomentBrickWeightCm, ForceBrickWeights);

		const double Composite = CompositeTensionMPa(
			MomentBrickWeightCm, CoursesOfDepth * CoursePitchCm, WallThicknessCm);

		if (!(TensileStrengthMPa > 0.0))
		{
			// Zero tensile strength: match ComputeUtilisation's Max() rather than divide by zero.
			return FMath::Min(Patch, Composite) > 0.0 ? TNumericLimits<double>::Max() : 0.0;
		}

		return FMath::Min(Patch, Composite) / TensileStrengthMPa;
	}

	/** The flush running-bond wall this file cuts a raking void into. */
	inline FRunningBondSpec RakingWallSpec(
		int32 CoursesHigh, int32 BricksPerCourse, const FConnectionStrength& Strength)
	{
		FRunningBondSpec Spec;
		Spec.BrickSizeCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm);
		Spec.JointThicknessCm = MortarJointCm;
		Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
		Spec.CoursesHigh = CoursesHigh;
		Spec.BricksPerCourse = BricksPerCourse;
		Spec.End = EWallEnd::Flush;
		Spec.Strength = Strength;
		return Spec;
	}

	/** Which course a laid box is in, read back off its height. */
	inline int32 CourseOf(const FPieceBox& Box)
	{
		return FMath::RoundToInt32((Box.CentreCm.Z - BrickHeightCm / 2.0) / CoursePitchCm);
	}

	/** Every piece a raking void of this many steps takes out, in handle order. */
	inline TArray<int32> RakingVoidPieces(TArrayView<const FPieceBox> Boxes, int32 CorbelSteps)
	{
		TArray<int32> Cut;

		for (int32 Piece = 0; Piece < Boxes.Num(); ++Piece)
		{
			const int32 Course = CourseOf(Boxes[Piece]);

			if (Course < LowestVoidCourse || Course > HighestVoidCourse(CorbelSteps))
			{
				continue;
			}

			if (Boxes[Piece].CentreCm.X < RakingVoidEdgeXCm(CorbelSteps, Course) - 0.001)
			{
				Cut.Add(Piece);
			}
		}

		return Cut;
	}
}

// Aliased, not opened: it shares many names with this file's support namespace.
namespace ArchSupport = StructureArchingTestSupport;

/**
 * A corbel resists with the whole depth of masonry over it (the user's ruling: a brick deleted at
 * a free end must not bring the wall down). Masonry over a lost support acts as a deep beam,
 * W = t*D^2/6, against the bed patch's 179.48 cm3 (ARCHING_DESIGN.md slice 5).
 *
 * Asserts, in order: the load path is unchanged (force and moment still match the hand ladder, so
 * the utilisation claim is not circular); the utilisation is min(patch, composite) at 2%; and the
 * outcome by path to ground, never by displacement.
 *
 * At the mean bond no buildable mortared corbel fails by depth, so the dry row carries "something
 * must still come down" (and AHundredStepCorbelMustComeDown). Must not move the top-course anchors
 * in StructureBinding and StructurePushTest (0.058203838); omitting the min moves both to 0.08359.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureCompositeDepthTest,
	"DestructionGame.Core.Structure.ACorbelResistsWithItsWholeDepth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureCompositeDepthTest::RunTest(const FString& Parameters)
{
	using namespace StructureCompositeDepthTestSupport;
	using namespace StaircaseWallTestSupport;

	// Expectations hold only while the profile carries the strengths they were derived against.
	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against f_xk1 = %g MPa, the profile carries %g"),
			MortarTensileMPa, GeneralPurposeMortar.TensileStrengthMPa),
		GeneralPurposeMortar.TensileStrengthMPa == MortarTensileMPa);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against compressive 10 MPa, the profile carries %g"),
			GeneralPurposeMortar.CompressiveStrengthMPa),
		GeneralPurposeMortar.CompressiveStrengthMPa == 10.0);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: dry stone must have EXACTLY zero tensile strength, it has %g"),
			DryStone.TensileStrengthMPa),
		DryStone.TensileStrengthMPa == 0.0);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against clay brick at 1.9 g/cm3, the profile carries %g"),
			ClayBrick.DensityGramsPerCubicCm),
		ClayBrick.DensityGramsPerCubicCm == 1.9);

	/*
	 * PART 0: the two sections are nested and the ladder derivations agree.
	 *
	 * The patch equals t*D^2/6 at D = 10.25 algebraically but one ulp apart in floating point, so
	 * the min keeps the patch value verbatim where composite is no better.
	 */
	TestTrue(
		FString::Printf(
			TEXT("THE TWO SECTIONS ARE NESTED: the 10.25 x 10.25 bed patch is %s cm3 and ")
			TEXT("t*D^2/6 at D = 10.25 is %s — algebraically identical, %s apart relative"),
			*Bits(PatchModulusCm3), *Bits(CompositeModulusCm3(BrickWidthCm, BrickWidthCm)),
			*Bits(FMath::Abs(PatchModulusCm3 - CompositeModulusCm3(BrickWidthCm, BrickWidthCm))
				/ PatchModulusCm3)),
		FMath::Abs(PatchModulusCm3 - CompositeModulusCm3(BrickWidthCm, BrickWidthCm))
			<= 1.0e-15 * PatchModulusCm3);

	// The closed forms must reproduce StaircaseWallTestSupport's hand count exactly.
	for (int32 Course = StaircaseLowestCorbelCourse;
		Course <= StaircaseHighestCorbelCourse;
		++Course)
	{
		const int32 StepsBelowTop = StaircaseHighestCorbelCourse - Course;

		const double HandForce =
			StaircaseCorbelLoadBrickWeights[Course - StaircaseLowestCorbelCourse];
		const double HandMoment =
			StaircaseCorbelMomentBrickWeightCm[Course - StaircaseLowestCorbelCourse];

		TestTrue(
			FString::Printf(
				TEXT("THE LADDERS AGREE: course %d is %d steps below the top, so the hand count ")
				TEXT("says %s brick weights and %s brick-weight-cm; the closed forms say %s and %s"),
				Course, StepsBelowTop, *Bits(HandForce), *Bits(HandMoment),
				*Bits(LadderForceBrickWeights(StepsBelowTop)),
				*Bits(LadderMomentBrickWeightCm(StepsBelowTop))),
			LadderForceBrickWeights(StepsBelowTop) == HandForce
				&& LadderMomentBrickWeightCm(StepsBelowTop) == HandMoment);
	}

	// PART 1: raking corbels at several depths, one laid dry, two under more wall than they need.

	/**
	 * What the cascade must do to the mass over the void. Unasserted where the verdict depends on
	 * an open choice of depth bound.
	 */
	enum class EVoidOutcome
	{
		MustStand,
		MustComeDown,
		Unasserted,
	};

	/** One raking void case. */
	struct FCorbelCase
	{
		const TCHAR* Description;

		/** Number of corbelled steps; always odd (see RakingVoidEdgeXCm). */
		int32 CorbelSteps;

		/** At least CorbelSteps + 2. Extra courses add both load and depth (PART 1B). */
		int32 CoursesHigh;

		/** ~CorbelSteps + 1, so the load cone over the bottom step stays inside the wall. */
		int32 BricksPerCourse;

		const FConnectionStrength* Strength;

		EVoidOutcome Outcome;

		/** ARCHING_DESIGN's own figure for the bottom rung, or 0 where it published none. */
		double DesignUtilisation;
	};

	const TArray<FCorbelCase> Cases = {
		// Five steps: the smallest raking corbel where patch and composite readings disagree.
		{ TEXT("a FIVE-step raking corbel"), 5, 7, 7, &GeneralPurposeMortar,
			EVoidOutcome::MustStand, 0.0 },

		/*
		 * Eleven steps: the staircase void, the same 13 x 10 wall AStaircaseVoidCondemnsTheCorbel
		 * lays. ARCHING_DESIGN's 0.369 was at f_xk1 = 0.10; at the mean 0.70 it reads 0.0527.
		 */
		{ TEXT("ELEVEN steps: THE STAIRCASE VOID"), 11, 13, 10, &GeneralPurposeMortar,
			EVoidOutcome::MustStand, 0.369 / 7.0 },

		/*
		 * Forty-five steps: used to fail by depth at the characteristic bond; reads 0.1786 at the
		 * mean. Outcome unasserted because whether a 7.9 m overhang stands is unruled (CURRENT_STATE).
		 */
		{ TEXT("FORTY-FIVE steps: the depth runs out"), 45, 47, 47, &GeneralPurposeMortar,
			EVoidOutcome::Unasserted, 0.0 },

		/*
		 * The staircase laid dry: no bond, so no composite action, so it must come down. Guards
		 * against subtracting an axial term, which would stand a dry overhang up.
		 */
		{ TEXT("ELEVEN steps, laid DRY"), 11, 13, 10, &DryStone,
			EVoidOutcome::MustComeDown, 0.0 },

		/*
		 * The same cuts under more wall. Extra courses add load as well as depth, so a taller wall
		 * is not the same as crediting more depth. Forty-five under 67: outcome unasserted, as above.
		 */
		{ TEXT("FORTY-FIVE steps under SIXTY-SEVEN courses"), 45, 67, 47, &GeneralPurposeMortar,
			EVoidOutcome::Unasserted, 0.0 },

		/*
		 * Eleven steps under 40 courses: the wall the game renders, headless. Outcome unasserted
		 * (depends on the depth bound); PART 1B asserts the property that holds under every bound.
		 */
		{ TEXT("ELEVEN steps under FORTY courses: THE SCENARIO WALL"), 11, 40, 30,
			&GeneralPurposeMortar, EVoidOutcome::Unasserted, 0.0 },
	};

	/** Every case's bottom rung, kept for PART 1B's pairwise comparison. */
	struct FBottomRung
	{
		const TCHAR* Description;
		int32 CorbelSteps;
		int32 CoursesHigh;
		int32 BricksPerCourse;
		const FConnectionStrength* Strength;

		double ForceBrickWeights;
		double MomentBrickWeightCm;
		double Utilisation;

		/** Every course of the wall over this joint. */
		double UnboundedDepthCm;
	};

	TArray<FBottomRung> BottomRungs;

	for (const FCorbelCase& Case : Cases)
	{
		const int32 Steps = Case.CorbelSteps;

		// Only then is the hand ladder the whole load; otherwise per-rung checks are one-sided.
		const bool bWallStopsAtTheCorbel = Case.CoursesHigh == Case.CorbelSteps + 2;

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the STEP count must be ODD or the raking edge names bricks ")
				TEXT("that are not there; it is %d"),
				Case.Description, Case.CorbelSteps),
			Case.CorbelSteps % 2 == 1);

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: a %d-step cut needs at least %d courses to stand in, the wall ")
				TEXT("has %d"),
				Case.Description, Case.CorbelSteps, Case.CorbelSteps + 2, Case.CoursesHigh),
			Case.CoursesHigh >= Case.CorbelSteps + 2);

		AddInfo(FString::Printf(
			TEXT("%s: %d x %d flush wall, raking void through courses %d..%d, %d corbelled steps, ")
			TEXT("%g cm of masonry over the bottom one (%g cm of it is the corbel's own)"),
			Case.Description, Case.BricksPerCourse, Case.CoursesHigh, LowestVoidCourse,
			HighestVoidCourse(Case.CorbelSteps), Steps,
			CoursesOverCorbelJoint(Case.CoursesHigh, LowestCorbelCourse) * CoursePitchCm,
			Case.CorbelSteps * CoursePitchCm));

		FBrickLayout Laid;

		if (!RunningBond(
				RakingWallSpec(Case.CoursesHigh, Case.BricksPerCourse, *Case.Strength), Laid))
		{
			AddError(FString::Printf(
				TEXT("%s: FIXTURE: RunningBond should lay a %d x %d flush wall"),
				Case.Description, Case.BricksPerCourse, Case.CoursesHigh));

			continue;
		}

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the laid wall must know where every piece and every joint is, ")
				TEXT("or every moment below is silently zero"),
				Case.Description),
			Laid.Structure.HasCompleteGeometry());

		// Positive control: nothing over capacity before the cut (hence the flush end).
		Laid.Structure.SolveLoads();

		double WorstAsBuilt = 0.0;

		for (int32 Joint = 0; Joint < Laid.Structure.NumConnections(); ++Joint)
		{
			WorstAsBuilt =
				FMath::Max(WorstAsBuilt, Laid.Structure.GetConnectionUtilisation(Joint));
		}

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the wall as built must have nothing over capacity, its worst ")
				TEXT("joint reads %s"),
				Case.Description, *Bits(WorstAsBuilt)),
			WorstAsBuilt < 1.0);

		const TArray<int32> VoidPieces = RakingVoidPieces(Laid.Boxes, Case.CorbelSteps);

		bool bCutLaid = true;

		for (const int32 Piece : VoidPieces)
		{
			if (!Laid.Structure.RemovePiece(Piece))
			{
				AddError(FString::Printf(
					TEXT("%s: FIXTURE: piece %d should have been in the wall to remove"),
					Case.Description, Piece));

				bCutLaid = false;
				break;
			}
		}

		if (!bCutLaid || VoidPieces.Num() == 0)
		{
			AddError(FString::Printf(
				TEXT("%s: FIXTURE: the raking void should cut something, it cut %d pieces"),
				Case.Description, VoidPieces.Num()));

			continue;
		}

		Laid.Structure.SolveLoads();

		// The ladder, rung by rung.

		int32 CorbelPieces[2] = { INDEX_NONE, INDEX_NONE };
		int32 BottomRungJoint = INDEX_NONE;
		TArray<int32> AllCorbelPieces;

		int32 RungsOverCapacity = 0;
		int32 PredictedRungsOverCapacity = 0;
		bool bLadderRead = true;

		for (int32 Course = LowestCorbelCourse;
			Course <= HighestCorbelCourse(Case.CorbelSteps);
			++Course)
		{
			const int32 StepsBelowTop = HighestCorbelCourse(Case.CorbelSteps) - Course;

			const double HandForceBrickWeights = LadderForceBrickWeights(StepsBelowTop);
			const double HandMomentBrickWeightCm = LadderMomentBrickWeightCm(StepsBelowTop);

			const int32 CoursesOfDepth = CoursesOverCorbelJoint(Case.CoursesHigh, Course);

			const double Expected = CorbelUtilisation(
				HandMomentBrickWeightCm, HandForceBrickWeights, CoursesOfDepth,
				BrickWidthCm, Case.Strength->TensileStrengthMPa);

			if (Expected > 1.0)
			{
				++PredictedRungsOverCapacity;
			}

			const int32 Corbel = StaircasePieceAt(
				Laid.Boxes,
				RakingVoidEdgeXCm(Case.CorbelSteps, Course),
				CourseZCm(Course));

			const int32 Support = StaircasePieceAt(
				Laid.Boxes,
				RakingVoidEdgeXCm(Case.CorbelSteps, Course) + HalfStepCm,
				CourseZCm(Course - 1));

			const int32 Joint = Corbel != INDEX_NONE && Support != INDEX_NONE
				? JointBetweenPieces(Laid.Structure, Corbel, Support)
				: INDEX_NONE;

			if (Joint == INDEX_NONE)
			{
				AddError(FString::Printf(
					TEXT("%s: FIXTURE: course %d should have a brick at x %g jointed to one at ")
					TEXT("x %g; they are pieces %d and %d"),
					Case.Description, Course, RakingVoidEdgeXCm(Case.CorbelSteps, Course),
					RakingVoidEdgeXCm(Case.CorbelSteps, Course) + HalfStepCm, Corbel, Support));

				bLadderRead = false;
				break;
			}

			AllCorbelPieces.Add(Corbel);

			if (Course == LowestCorbelCourse)
			{
				CorbelPieces[0] = Corbel;
				BottomRungJoint = Joint;
			}

			if (Course == HighestCorbelCourse(Case.CorbelSteps))
			{
				CorbelPieces[1] = Corbel;
			}

			// Every expectation assumes a single 10.25 x 10.25 half seat; check the fixture presents one.
			const FConnection& Bed = Laid.Structure.GetConnection(Joint);

			const bool bOneSeat = ArchSupport::TheOneIntactSeatBeneath(Laid.Structure, Corbel) == Joint;

			TestTrue(
				FString::Printf(
					TEXT("%s, course %d: FIXTURE: the corbel must rest on EXACTLY ONE bed joint ")
					TEXT("(it rests on %d), of %g cm2 with half-extents (%g, %g); MakeInterface ")
					TEXT("emitted %g cm2 with (%g, %g)"),
					Case.Description, Course, ArchSupport::IntactSeatsBeneath(Laid.Structure, Corbel),
					HalfSeatAreaSqCm, HalfSeatHalfExtentCm, HalfSeatHalfExtentCm,
					Bed.InterfaceAreaSqCm, Bed.InterfaceHalfExtentCm.X, Bed.InterfaceHalfExtentCm.Y),
				bOneSeat
					&& FMath::IsNearlyEqual(Bed.InterfaceAreaSqCm, HalfSeatAreaSqCm, 1.0e-9)
					&& FMath::IsNearlyEqual(
						Bed.InterfaceHalfExtentCm.X, HalfSeatHalfExtentCm, 1.0e-9)
					&& FMath::IsNearlyEqual(
						Bed.InterfaceHalfExtentCm.Y, HalfSeatHalfExtentCm, 1.0e-9));

			const FVector ForceUu = Laid.Structure.GetConnectionForce(Joint);
			const FVector MomentUuCm = Laid.Structure.GetConnectionMoment(Joint);

			const double Utilisation = Laid.Structure.GetConnectionUtilisation(Joint);

			const double MeasuredForceBrickWeights = ForceUu.Size() / BrickWeightUu;
			const double MeasuredMomentBrickWeightCm = MomentUuCm.Size() / BrickWeightUu;

			if (Utilisation > 1.0)
			{
				++RungsOverCapacity;
			}

			const bool bWorthPrinting =
				Steps <= 11 || Course == LowestCorbelCourse
				|| Course == HighestCorbelCourse(Case.CorbelSteps);

			if (bWorthPrinting)
			{
				// Readings use the measured moment: the hand ladder misses load from courses above.
				AddInfo(FString::Printf(
					TEXT("%s, course %2d: joint %d carries %s brick weights (hand ladder %s) and ")
					TEXT("%s brick-weight-cm (hand ladder %s) over %d courses = %g cm of depth. ")
					TEXT("It reads %s; on the moment it carries the patch alone says %s, the ")
					TEXT("composite section at that depth says %s, and the corbel's OWN %d ")
					TEXT("courses would say %s"),
					Case.Description, Course, Joint, *Bits(MeasuredForceBrickWeights),
					*Bits(HandForceBrickWeights), *Bits(MeasuredMomentBrickWeightCm),
					*Bits(HandMomentBrickWeightCm), CoursesOfDepth,
					CoursesOfDepth * CoursePitchCm, *Bits(Utilisation),
					*Bits(PatchTensionMPa(MeasuredMomentBrickWeightCm, MeasuredForceBrickWeights)
						/ MortarTensileMPa),
					*Bits(CompositeTensionMPa(
						MeasuredMomentBrickWeightCm, CoursesOfDepth * CoursePitchCm, BrickWidthCm)
						/ MortarTensileMPa),
					FMath::Min(CoursesOfDepth, Case.CorbelSteps),
					*Bits(CompositeTensionMPa(
						MeasuredMomentBrickWeightCm,
						FMath::Min(CoursesOfDepth, Case.CorbelSteps) * CoursePitchCm,
						BrickWidthCm)
						/ MortarTensileMPa)));
			}

			/*
			 * Load path first: composite action changes the section, not the load, so force and
			 * moment must match the hand ladder (2%; ARCHING_DESIGN's cross-check agrees to 1.3%).
			 * In a taller wall the ladder can only grow, so the check is "at least".
			 */
			const double ForceFloor = HandForceBrickWeights * (1.0 - 0.02);
			const double MomentFloor = HandMomentBrickWeightCm * (1.0 - 0.02);

			TestTrue(
				FString::Printf(
					TEXT("%s, course %d: the load path is UNCHANGED — the joint must still carry ")
					TEXT("%s the hand ladder's %s brick weights, it carries %s"),
					Case.Description, Course, bWallStopsAtTheCorbel ? TEXT("") : TEXT("at least"),
					*Bits(HandForceBrickWeights), *Bits(MeasuredForceBrickWeights)),
				bWallStopsAtTheCorbel
					? FMath::Abs(MeasuredForceBrickWeights - HandForceBrickWeights)
						<= 0.02 * HandForceBrickWeights
					: MeasuredForceBrickWeights >= ForceFloor);

			TestTrue(
				FString::Printf(
					TEXT("%s, course %d: and %s the hand ladder's %s brick-weight-cm of moment, ")
					TEXT("it carries %s"),
					Case.Description, Course, bWallStopsAtTheCorbel ? TEXT("") : TEXT("at least"),
					*Bits(HandMomentBrickWeightCm), *Bits(MeasuredMomentBrickWeightCm)),
				bWallStopsAtTheCorbel
					? FMath::Abs(MeasuredMomentBrickWeightCm - HandMomentBrickWeightCm)
						<= 0.02 * HandMomentBrickWeightCm
					: MeasuredMomentBrickWeightCm >= MomentFloor);

			/*
			 * Then the section. A dry row is asserted as an ordering, since its answer is Max().
			 * Skipped in a taller wall, where both the ladder moment and the unbounded depth are wrong.
			 */
			if (Expected >= TNumericLimits<double>::Max())
			{
				TestTrue(
					FString::Printf(
						TEXT("%s, course %d: with f_xk1 exactly zero, ANY tension has already ")
						TEXT("gone — the joint must read past capacity, it reads %s"),
						Case.Description, Course, *Bits(Utilisation)),
					Utilisation > 1.0);
			}
			else if (bWallStopsAtTheCorbel)
			{
				TestTrue(
					FString::Printf(
						TEXT("%s, course %d: %d courses of bonded masonry stand over this joint, ")
						TEXT("so its section is t*D^2/6 = %s cm3 rather than the patch's %s, and ")
						TEXT("it must read %s — it reads %s"),
						Case.Description, Course, CoursesOfDepth,
						*Bits(CompositeModulusCm3(BrickWidthCm, CoursesOfDepth * CoursePitchCm)),
						*Bits(PatchModulusCm3), *Bits(Expected), *Bits(Utilisation)),
					FMath::Abs(Utilisation - Expected) <= 0.02 * FMath::Max(Expected, 1.0e-12));
			}

			if (Course == LowestCorbelCourse)
			{
				BottomRungs.Add({
					Case.Description, Case.CorbelSteps, Case.CoursesHigh, Case.BricksPerCourse,
					Case.Strength, MeasuredForceBrickWeights, MeasuredMomentBrickWeightCm,
					Utilisation, CoursesOfDepth * CoursePitchCm });
			}
		}

		if (!bLadderRead)
		{
			continue;
		}

		AddInfo(FString::Printf(
			TEXT("%s: %d of %d rungs read over capacity; the composite section over the corbel's ")
			TEXT("OWN %d courses predicts %d"),
			Case.Description, RungsOverCapacity, Steps, Case.CorbelSteps,
			PredictedRungsOverCapacity));

		if (bWallStopsAtTheCorbel)
		{
			TestEqual(
				FString::Printf(
					TEXT("%s: exactly the rungs the composite section condemns must be over ")
					TEXT("capacity"),
					Case.Description),
				RungsOverCapacity, PredictedRungsOverCapacity);
		}

		// ARCHING_DESIGN's figure within a factor of two: still catches a missing 100x or the 64.8.
		if (Case.DesignUtilisation > 0.0 && BottomRungJoint != INDEX_NONE)
		{
			const double Measured = Laid.Structure.GetConnectionUtilisation(BottomRungJoint);

			TestTrue(
				FString::Printf(
					TEXT("%s: ARCHING_DESIGN predicts %g for the bottom rung and it reads %s — a ")
					TEXT("cross-check, not a target, so it allows a factor of two"),
					Case.Description, Case.DesignUtilisation, *Bits(Measured)),
				Measured >= 0.5 * Case.DesignUtilisation
					&& Measured <= 2.0 * Case.DesignUtilisation);
		}

		// The outcome.

		const int32 BreakingPasses = Laid.Structure.SolveAndBreak();

		int32 Unrouted = 0;

		for (int32 Piece = 0; Piece < Laid.Structure.NumPieces(); ++Piece)
		{
			if (!Laid.Structure.IsPieceRemoved(Piece) && !Laid.Structure.IsPieceSupported(Piece))
			{
				++Unrouted;
			}
		}

		AddInfo(FString::Printf(
			TEXT("%s: the cascade ran %d passes and left %d of the %d pieces it did not delete ")
			TEXT("with no path to the ground"),
			Case.Description, BreakingPasses, Unrouted,
			Laid.Structure.NumPieces() - VoidPieces.Num()));

		if (Case.Outcome == EVoidOutcome::Unasserted)
		{
			int32 CorbelsLost = 0;

			for (const int32 Piece : AllCorbelPieces)
			{
				if (!Laid.Structure.IsPieceSupported(Piece))
				{
					++CorbelsLost;
				}
			}

			AddInfo(FString::Printf(
				TEXT("%s: OUTCOME NOT ASSERTED — %d of the %d corbelled bricks lost their path to ")
				TEXT("the ground. Which way this row should read follows from the depth bound and ")
				TEXT("is a ruling, not a threshold."),
				Case.Description, CorbelsLost, AllCorbelPieces.Num()));
		}
		else if (Case.Outcome == EVoidOutcome::MustStand)
		{
			/*
			 * Every corbelled brick must still reach the ground. Seatless survivors of the cut are
			 * local loss and are not corbels, so they are not counted.
			 */
			int32 CorbelsLost = 0;

			for (const int32 Piece : AllCorbelPieces)
			{
				if (!Laid.Structure.IsPieceSupported(Piece))
				{
					++CorbelsLost;
				}
			}

			TestEqual(
				FString::Printf(
					TEXT("%s: THE OVERHANG MUST STAND — all %d corbelled bricks should still ")
					TEXT("reach the ground, %d do not"),
					Case.Description, AllCorbelPieces.Num(), CorbelsLost),
				CorbelsLost, 0);
		}
		else
		{
			/*
			 * The corbel top loses its path, and the bottom rung failed under load (a joint that left
			 * with its piece has break pass INDEX_NONE).
			 */
			TestTrue(
				FString::Printf(
					TEXT("%s: THE CORBEL MUST COME DOWN — the top of it (piece %d) must lose its ")
					TEXT("path to the ground"),
					Case.Description, CorbelPieces[1]),
				CorbelPieces[1] != INDEX_NONE
					&& !Laid.Structure.IsPieceSupported(CorbelPieces[1]));

			TestTrue(
				FString::Printf(
					TEXT("%s: and the bottom rung (joint %d) must be one of the joints that ")
					TEXT("failed UNDER LOAD; it broke in pass %d"),
					Case.Description, BottomRungJoint,
					BottomRungJoint == INDEX_NONE
						? -2
						: Laid.Structure.GetBreakPass(BottomRungJoint)),
				BottomRungJoint != INDEX_NONE
					&& Laid.Structure.GetBreakPass(BottomRungJoint) != INDEX_NONE);
		}
	}

	/*
	 * PART 1B: adding masonry on top of a corbel must not make the corbel safer.
	 *
	 * Extra courses add load as well as depth, so the check is one-sided: the taller wall's bottom
	 * rung may not read lower. It holds under any depth bound tied to the cut, and fails for an
	 * unbounded walk up the whole wall. It does not rank the candidate bounds.
	 */
	{
		int32 PairsCompared = 0;

		for (const FBottomRung& Short : BottomRungs)
		{
			for (const FBottomRung& Tall : BottomRungs)
			{
				if (Short.CorbelSteps != Tall.CorbelSteps
					|| Short.Strength != Tall.Strength
					|| Short.CoursesHigh >= Tall.CoursesHigh)
				{
					continue;
				}

				++PairsCompared;

				// Candidate depth rules on the taller wall's measured moment (the hand ladder understates it).
				const double UnboundedMPa = CompositeTensionMPa(
					Tall.MomentBrickWeightCm, Tall.UnboundedDepthCm, BrickWidthCm);

				const double CorbelOwnDepthCm = Tall.CorbelSteps * CoursePitchCm;

				const double CorbelLimitedMPa = CompositeTensionMPa(
					Tall.MomentBrickWeightCm, CorbelOwnDepthCm, BrickWidthCm);

				const double PatchMPa = PatchTensionMPa(
					Tall.MomentBrickWeightCm, Tall.ForceBrickWeights);

				AddInfo(FString::Printf(
					TEXT("SAME CUT, MORE WALL: %d steps under %d courses against the same %d steps ")
					TEXT("under %d. The bottom rung carries %s brick weights against %s (x%s) and ")
					TEXT("%s brick-weight-cm against %s (x%s) — MORE, because the extra courses ")
					TEXT("stand on it. The walk credits %g cm of depth against %g (x%s), a section ")
					TEXT("x%s larger. It reads %s against %s (x%s)."),
					Tall.CorbelSteps, Tall.CoursesHigh, Short.CorbelSteps, Short.CoursesHigh,
					*Bits(Tall.ForceBrickWeights), *Bits(Short.ForceBrickWeights),
					*Bits(Tall.ForceBrickWeights / Short.ForceBrickWeights),
					*Bits(Tall.MomentBrickWeightCm), *Bits(Short.MomentBrickWeightCm),
					*Bits(Tall.MomentBrickWeightCm / Short.MomentBrickWeightCm),
					Tall.UnboundedDepthCm, Short.UnboundedDepthCm,
					*Bits(Tall.UnboundedDepthCm / Short.UnboundedDepthCm),
					*Bits((Tall.UnboundedDepthCm * Tall.UnboundedDepthCm)
						/ (Short.UnboundedDepthCm * Short.UnboundedDepthCm)),
					*Bits(Tall.Utilisation), *Bits(Short.Utilisation),
					*Bits(Tall.Utilisation / Short.Utilisation)));

				AddInfo(FString::Printf(
					TEXT("SAME CUT, MORE WALL: on the %d x %d wall's own moment the candidate ")
					TEXT("rules read — all the masonry there is (%g cm): %s; the corbel's own %d ")
					TEXT("courses (%g cm): %s; the bed patch alone: %s. The corbel-limited figure ")
					TEXT("is what a span-to-depth or bond-continuity bound would sit at or below, ")
					TEXT("and this fixture cannot tell those two apart."),
					Tall.BricksPerCourse, Tall.CoursesHigh,
					Tall.UnboundedDepthCm, *Bits(UnboundedMPa / MortarTensileMPa),
					Tall.CorbelSteps, CorbelOwnDepthCm,
					*Bits(CorbelLimitedMPa / MortarTensileMPa),
					*Bits(PatchMPa / MortarTensileMPa)));

				// The deepest section this pair permits: D_short * sqrt(M_tall / M_short). A ceiling only.
				const double DeepestPermittedCm = Short.UnboundedDepthCm
					* FMath::Sqrt(Tall.MomentBrickWeightCm / Short.MomentBrickWeightCm);

				AddInfo(FString::Printf(
					TEXT("SAME CUT, MORE WALL: this pair permits AT MOST %s cm of section on the ")
					TEXT("taller wall (%s courses) and the walk credits %g (%g courses); the ")
					TEXT("corbel's own depth is %g. Every rule at or under the ceiling passes and ")
					TEXT("this fixture ranks none of them."),
					*Bits(DeepestPermittedCm), *Bits(DeepestPermittedCm / CoursePitchCm),
					Tall.UnboundedDepthCm, Tall.UnboundedDepthCm / CoursePitchCm,
					CorbelOwnDepthCm));

				// Premise: the taller wall must load the corbel harder, or the property asserts nothing.
				TestTrue(
					FString::Printf(
						TEXT("FIXTURE: %d steps under %d courses must bend its bottom rung HARDER ")
						TEXT("than the same cut under %d — %s brick-weight-cm against %s"),
						Tall.CorbelSteps, Tall.CoursesHigh, Short.CoursesHigh,
						*Bits(Tall.MomentBrickWeightCm), *Bits(Short.MomentBrickWeightCm)),
					Tall.MomentBrickWeightCm > Short.MomentBrickWeightCm);

				// No tolerance: it is an ordering, and slack would admit the defect.
				TestTrue(
					FString::Printf(
						TEXT("MASONRY ABOVE A CORBEL MUST NOT MAKE IT SAFER — the same %d-step cut ")
						TEXT("under %d courses bends its bottom rung x%s harder than under %d, so ")
						TEXT("that joint may not read LESS; it reads %s against %s, x%s"),
						Tall.CorbelSteps, Tall.CoursesHigh,
						*Bits(Tall.MomentBrickWeightCm / Short.MomentBrickWeightCm),
						Short.CoursesHigh, *Bits(Tall.Utilisation), *Bits(Short.Utilisation),
						*Bits(Tall.Utilisation / Short.Utilisation)),
					Tall.Utilisation >= Short.Utilisation);
			}
		}

		// Deleting one row of a pair would silently drop the property.
		TestTrue(
			FString::Printf(
				TEXT("FIXTURE: the table must hold at least two cuts each laid in a wall of its ")
				TEXT("own height AND in a taller one, or this property asserts nothing; it made ")
				TEXT("%d comparison(s)"),
				PairsCompared),
			PairsCompared >= 2);
	}

	/*
	 * PART 2: the free end, the deletion the user made. Delete the end brick of the grounded course
	 * and the full brick above keeps one bed patch, overhanging toward the free end where there is
	 * no head joint to arch against. The same shape initiates the jamb failures in cases 7, 9, 11.
	 *
	 * Credited depth is min(masonry above, max(body depth, lambda*e)), e = |M|/|F|
	 * (COMPOSITE_DEPTH_DESIGN.md). Here lambda*e binds (~39 cm of 217.5), so the reading is linear
	 * in height instead of falling as 1/m^2, which would make free ends unbreakable. Where the arm
	 * governs the reading reduces to util = K * F^2 / M, K = 6*W_brick/(t * 10^4 * f_x1) / lambda^2.
	 *
	 * Asserted: the arm governs; the identity holds exactly on the solver's loads; it holds at 2% on
	 * the hand moment (the non-circular check); and the joint matches AFreeEndDeletionInATallWall.
	 */
	{
		// 5.625 cm of arm on one brick weight over a one-course-deep section.
		const double HalfSeatCoefficient = HalfSeatEccentricityCm * BrickWeightUu
			/ (CompositeModulusCm3(BrickWidthCm, CoursePitchCm) * ForceUnitsPerMPaPerSqCm)
			/ MortarTensileMPa;

		AddInfo(FString::Printf(
			TEXT("FREE END: over the WHOLE wall a half seat would read %s * n / m^2, against ")
			TEXT("%s * n on its own bed patch. ARCHING_DESIGN's published 0.0082 is the ")
			TEXT("n = m = 19 row of that, %s; no fixture presents it and the depth is not ")
			TEXT("unbounded any more, so both are printed and nothing is derived from them."),
			*Bits(HalfSeatCoefficient),
			*Bits(PatchTensionMPa(HalfSeatEccentricityCm, 1.0) / MortarTensileMPa),
			*Bits(HalfSeatCoefficient * 19.0 / (19.0 * 19.0))));

		// lambda = 2*sqrt(3), a ruling (COMPOSITE_DEPTH_DESIGN.md), deliberately not imported from the solver.
		constexpr double CompositeDepthPerArm = 3.464;

		// K: the section arithmetic over lambda^2 (the depth rule), kept as two visible factors.
		const double ArmIdentityCoefficient =
			6.0 * BrickWeightUu / (BrickWidthCm * ForceUnitsPerMPaPerSqCm * MortarTensileMPa)
			/ (CompositeDepthPerArm * CompositeDepthPerArm);

		FBrickLayout Laid;

		if (!RunningBond(ArchSupport::ArchWallSpec(), Laid) || Laid.Boxes.Num() != ArchSupport::ArchWallPieceCount)
		{
			AddError(FString::Printf(
				TEXT("FREE END: FIXTURE: a flush 7 x 30 wall should lay as %d pieces, got %d"),
				ArchSupport::ArchWallPieceCount, Laid.Boxes.Num()));

			return true;
		}

		// The deletion: the grounded course's outermost full brick, at x = 0.
		const int32 EndBrick = StaircasePieceAt(Laid.Boxes, ArchSupport::ArchWallEvenBrickXCm(0), ArchSupport::ArchWallCourseZCm(0));

		// Course 1's first full brick, left half-seated and overhanging outward.
		const int32 HalfSeated =
			StaircasePieceAt(Laid.Boxes, ArchSupport::ArchWallOddBrickXCm(0), ArchSupport::ArchWallCourseZCm(1));

		if (EndBrick == INDEX_NONE || HalfSeated == INDEX_NONE
			|| !Laid.Structure.RemovePiece(EndBrick))
		{
			AddError(TEXT("FREE END: FIXTURE: the wall should have an end brick to delete and a ")
				TEXT("full brick above it to leave half seated"));

			return true;
		}

		Laid.Structure.SolveLoads();

		const int32 Seat = ArchSupport::TheOneIntactSeatBeneath(Laid.Structure, HalfSeated);

		TestTrue(
			FString::Printf(
				TEXT("FREE END: FIXTURE: the brick above the deletion must keep EXACTLY ONE seat, ")
				TEXT("it rests on %d"),
				ArchSupport::IntactSeatsBeneath(Laid.Structure, HalfSeated)),
			Seat != INDEX_NONE);

		if (Seat != INDEX_NONE)
		{
			const FConnection& Bed = Laid.Structure.GetConnection(Seat);

			// It must overhang toward the free end; overhanging inward would measure the arch instead.
			const double EccentricityCm =
				Laid.Boxes[HalfSeated].CentreCm.X - Bed.InterfaceCentreCm.X;

			TestTrue(
				FString::Printf(
					TEXT("FREE END: FIXTURE: it must overhang its seat by %g cm TOWARD the free ")
					TEXT("end, it overhangs %s"),
					HalfSeatEccentricityCm, *Bits(EccentricityCm)),
				FMath::IsNearlyEqual(EccentricityCm, -HalfSeatEccentricityCm, 1.0e-9));

			const double ColumnBrickWeights =
				FMath::Abs(Laid.Structure.GetConnectionForce(Seat).Z) / BrickWeightUu;

			/*
			 * Two-arm walk: M = 5.625 * 1 + 11.25 * (n - 1). The solver reads 1.04% more (the half
			 * bat's own arm), so 2%. Derived from the force only, never read back off
			 * GetConnectionMoment (CURRENT_STATE).
			 */
			const double MeasuredMomentBrickWeightCm =
				Laid.Structure.GetConnectionMoment(Seat).Size() / BrickWeightUu;

			const double HandMomentBrickWeightCm = HalfSeatEccentricityCm * 1.0
				+ HalfStepCm * (ColumnBrickWeights - 1.0);

			TestTrue(
				FString::Printf(
					TEXT("FREE END: the moment is the two-arm walk — its own weight at 5.625 cm ")
					TEXT("and the other %s brick weights at 11.25 — so %s brick weights makes %s ")
					TEXT("brick-weight-cm; the joint publishes %s, which is %s%% out (one arm ")
					TEXT("alone would be %s)"),
					*Bits(ColumnBrickWeights - 1.0), *Bits(ColumnBrickWeights),
					*Bits(HandMomentBrickWeightCm), *Bits(MeasuredMomentBrickWeightCm),
					*Bits(100.0 * (MeasuredMomentBrickWeightCm / HandMomentBrickWeightCm - 1.0)),
					*Bits(HalfSeatEccentricityCm * ColumnBrickWeights)),
				FMath::Abs(MeasuredMomentBrickWeightCm - HandMomentBrickWeightCm)
					<= 0.02 * HandMomentBrickWeightCm);

			// Each term of min(above, max(body, lambda*e)), so a moved reading says which one moved.
			const int32 CoursesOfDepth = ArchSupport::ArchWallSpec().CoursesHigh - 1;

			const double MasonryStandingOverItCm = CoursesOfDepth * CoursePitchCm;

			const FVector SeatForceUu = Laid.Structure.GetConnectionForce(Seat);
			const FVector SeatMomentUuCm = Laid.Structure.GetConnectionMoment(Seat);

			const double EffectiveArmCm =
				SeatForceUu.Size() > 0.0 ? SeatMomentUuCm.Size() / SeatForceUu.Size() : 0.0;

			const double PermittedDepthCm = CompositeDepthPerArm * EffectiveArmCm;

			const double CreditedDepthCm = Laid.Structure.GetConnectionCompositeDepthCm(Seat);

			const double Utilisation = Laid.Structure.GetConnectionUtilisation(Seat);

			AddInfo(FString::Printf(
				TEXT("FREE END: the half seat carries %s brick weights and %s brick-weight-cm ")
				TEXT("(the two-arm walk says %s), so e = %s cm and lambda*e = %s. The wall offers ")
				TEXT("%s cm and the joint was credited %s. Unbounded it would have read %s; on ")
				TEXT("its own bed patch, %s. It reads %s."),
				*Bits(ColumnBrickWeights), *Bits(MeasuredMomentBrickWeightCm),
				*Bits(HandMomentBrickWeightCm), *Bits(EffectiveArmCm), *Bits(PermittedDepthCm),
				*Bits(MasonryStandingOverItCm), *Bits(CreditedDepthCm),
				*Bits(CompositeTensionMPa(
					HandMomentBrickWeightCm, MasonryStandingOverItCm, BrickWidthCm)
					/ MortarTensileMPa),
				*Bits(PatchTensionMPa(HandMomentBrickWeightCm, ColumnBrickWeights)
					/ MortarTensileMPa),
				*Bits(Utilisation)));

			// Row one: the arm governs, or the identity below would measure something else.
			TestTrue(
				FString::Printf(
					TEXT("FREE END: THE ARM MUST GOVERN — e = |M|/|F| = %s cm permits ")
					TEXT("lambda*e = %s cm, and that must be what the joint was credited; it was ")
					TEXT("credited %s"),
					*Bits(EffectiveArmCm), *Bits(PermittedDepthCm), *Bits(CreditedDepthCm)),
				FMath::IsNearlyEqual(CreditedDepthCm, PermittedDepthCm, 1.0e-9));

			TestTrue(
				FString::Printf(
					TEXT("FREE END: and it must be STRICTLY under the %s cm of masonry standing ")
					TEXT("over the joint and STRICTLY over the one course of corbelling body ")
					TEXT("beneath it (%s cm), or one of those is the term that bound it; it is %s"),
					*Bits(MasonryStandingOverItCm), *Bits(CoursePitchCm),
					*Bits(CreditedDepthCm)),
				CreditedDepthCm > CoursePitchCm && CreditedDepthCm < MasonryStandingOverItCm);

			// Row two: the identity on the solver's loads, 1e-12. Pins the section, not the moment.
			const double IdentityOnSolverLoads = SeatMomentUuCm.Size() > 0.0
				? ArmIdentityCoefficient * (SeatForceUu.Size() / BrickWeightUu)
					* (SeatForceUu.Size() / BrickWeightUu)
					/ (SeatMomentUuCm.Size() / BrickWeightUu)
				: 0.0;

			TestTrue(
				FString::Printf(
					TEXT("FREE END: WHERE THE ARM GOVERNS THE READING IS K*F^2/M — K = %s ")
					TEXT("(1.561287 / lambda^2 at lambda = %g), F = %s brick weights and ")
					TEXT("M = %s brick-weight-cm, so it must read %s; it reads %s"),
					*Bits(ArmIdentityCoefficient), CompositeDepthPerArm,
					*Bits(SeatForceUu.Size() / BrickWeightUu),
					*Bits(SeatMomentUuCm.Size() / BrickWeightUu),
					*Bits(IdentityOnSolverLoads), *Bits(Utilisation)),
				FMath::Abs(Utilisation - IdentityOnSolverLoads)
					<= 1.0e-12 * FMath::Max(IdentityOnSolverLoads, 1.0e-12));

			// Row three: the identity on the hand moment, the non-circular check, at 2%.
			const double IdentityOnTheHandWalk = ArmIdentityCoefficient
				* ColumnBrickWeights * ColumnBrickWeights / HandMomentBrickWeightCm;

			TestTrue(
				FString::Printf(
					TEXT("FREE END: on the HAND moment the same identity says %s (the design ")
					TEXT("publishes ~0.5064 for this row) and it reads %s, %s%% out — which must ")
					TEXT("be the %s%% the solver's moment exceeds the two-arm walk by, and 2%% is ")
					TEXT("twice that and slack for nothing else"),
					*Bits(IdentityOnTheHandWalk), *Bits(Utilisation),
					*Bits(100.0 * (Utilisation / IdentityOnTheHandWalk - 1.0)),
					*Bits(100.0 * (MeasuredMomentBrickWeightCm / HandMomentBrickWeightCm - 1.0))),
				FMath::Abs(Utilisation - IdentityOnTheHandWalk)
					<= 0.02 * FMath::Max(IdentityOnTheHandWalk, 1.0e-12));

			// Same joint as AFreeEndDeletionInATallWall at thirty courses; must agree to the last digit.
			TestTrue(
				FString::Printf(
					TEXT("FREE END: this is the same joint AFreeEndDeletionInATallWall reads at ")
					TEXT("thirty courses, so the two files must agree to the last digit — the ")
					TEXT("shared anchor is %s and this file reads %s"),
					*Bits(ArchSupport::FreeEndThirtyCourseUtilisation), *Bits(Utilisation)),
				Utilisation == ArchSupport::FreeEndThirtyCourseUtilisation);
		}

		/*
		 * The ruling: the wall does not come down. Only pieces the cut left with no bed patch at all
		 * (course 1's half bat) may be lost.
		 */
		int32 SeatlessAfterTheCut = 0;

		for (int32 Piece = 0; Piece < Laid.Structure.NumPieces(); ++Piece)
		{
			if (!Laid.Structure.IsPieceRemoved(Piece)
				&& Laid.Boxes[Piece].CentreCm.Z > ArchSupport::ArchWallCourseZCm(0)
				&& ArchSupport::IntactSeatsBeneath(Laid.Structure, Piece) == 0)
			{
				++SeatlessAfterTheCut;
			}
		}

		const int32 BreakingPasses = Laid.Structure.SolveAndBreak();

		int32 Unrouted = 0;

		for (int32 Piece = 0; Piece < Laid.Structure.NumPieces(); ++Piece)
		{
			if (!Laid.Structure.IsPieceRemoved(Piece) && !Laid.Structure.IsPieceSupported(Piece))
			{
				++Unrouted;
			}
		}

		AddInfo(FString::Printf(
			TEXT("FREE END: the cut left %d piece(s) with no bed patch at all; the cascade then ")
			TEXT("ran %d passes and left %d of %d pieces with no path to the ground"),
			SeatlessAfterTheCut, BreakingPasses, Unrouted, Laid.Structure.NumPieces() - 1));

		TestTrue(
			FString::Printf(
				TEXT("THE RULING: A BRICK DELETED AT A FREE END MUST NOT BRING THE WALL DOWN — ")
				TEXT("only the %d piece(s) the cut left with no bed patch may be lost, and %d ")
				TEXT("piece(s) of %d were"),
				SeatlessAfterTheCut, Unrouted, Laid.Structure.NumPieces() - 1),
			Unrouted <= SeatlessAfterTheCut);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
