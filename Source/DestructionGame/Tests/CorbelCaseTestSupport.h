// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Core/Corbel.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

/**
 * The stepped corbel off an immovable base: cases A to D (claude_plans/CORBEL_CASES.html) and
 * the sweeps over them (CORBEL_CASES_EF.html). One shared family so fixtures cannot drift.
 *
 *     A   a bare stepped arm of single bricks              `bFilled` false
 *     B   BaseCells 2, Steps 4                             filled
 *     C   BaseCells 2, Steps 10                            filled
 *     D   BaseCells 5, LeftOrigin -3 cells, Steps 10       C plus three cells opposite
 *
 * Core/Corbel.h lays the bricks (the family is also playable); this header only indexes what a
 * test names: arm pieces, root joint, tip reach. Core.Corbel.LaysTheFamilyOnItsGrid checks the
 * builder. The spec is production's own type; the case functions set GeneralPurposeMortar
 * explicitly because the production default strength is all zeros.
 *
 * D's left origin shifts by the three cells it gains, so C and D share the same root joint at
 * the same X and differ only in counterweight (what test E probes).
 *
 * World-free. Named namespace with `Corbel`-prefixed names because unity builds merge
 * translation units; functions are `inline` because several files include this.
 */
namespace CorbelCaseTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * Constants below are written from first principles, not imported, and strengths are asserted
	 * against the profile at the call site, so a wrong production constant fails.
	 */

	/** DESIGN.md's standard UK metric clay brick, cm, at unit scale. */
	constexpr double CorbelBrickLengthCm = 21.5;
	constexpr double CorbelBrickWidthCm = 10.25;
	constexpr double CorbelBrickHeightCm = 6.5;

	/** 1 cm mortar joint, giving a 22.5 cm grid. */
	constexpr double CorbelMortarJointCm = 1.0;

	/** Clay at 1.9 g/cm3. */
	constexpr double CorbelBrickDensityGramsPerCubicCm = 1.9;

	/**
	 * Force in uu that loads 1 cm2 to 1 MPa: 1 N = 100 uu and 1 cm2 = 100 mm2, so 10000 uu.
	 * Deliberately not DestructionForce::ForceUnitsPerMPaSqCm, so a wrong constant fails.
	 */
	constexpr double CorbelForceUnitsPerMPaPerSqCm = 100.0 * 100.0;

	/** 980 cm/s2; with 1 uu = 1 cm and mass in kg, MassKg * 980 is a force in uu. */
	constexpr double CorbelGravityCmPerSecondSquared = 980.0;

	/**
	 * Published corbelling limit for this brick (claude_plans/REAL_WORLD_CHECK.md): per course
	 * min(bed depth / 3, height / 2) = 3.25 cm, total 10.25 cm. A reference to report against,
	 * not a constraint: fixtures deliberately step 11.25 cm (3.46x), as the user ruled, to find
	 * what actually falls. The 3.25 cm step is the strongest "must stand" row available.
	 */
	constexpr double CorbelCodeStepPerCourseCm = 3.25;
	constexpr double CorbelCodeTotalProjectionCm = 10.25;

	/** Print a double at full precision. */
	inline FString CorbelBits(double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	/** Elastic section modulus of a rectangle, cm3: W = I/c = (4/3)*HalfAlong*HalfAcross^2. */
	constexpr double CorbelSectionModulusCm3(double HalfAlongCm, double HalfAcrossCm)
	{
		return (4.0 / 3.0) * HalfAlongCm * HalfAcrossCm * HalfAcrossCm;
	}

	/** Deep-beam section, cm3: W = t*D^2/6 for a wall t thick and D deep. */
	constexpr double CorbelCompositeModulusCm3(double WallThicknessCm, double DepthCm)
	{
		return WallThicknessCm * DepthCm * DepthCm / 6.0;
	}

	/**
	 * One structure in the family. Scale multiplies every length but not density, so at scale k
	 * mass goes as k^3, root moment k^4, section k^3, and stress grows linearly (test G).
	 */
	using FCorbelSpec = DestructionCorbel::FCorbelSpec;

	/** Case C: the two-cell base, filled, reaching as far as it is asked to. */
	inline FCorbelSpec CorbelCaseC(int32 Steps)
	{
		FCorbelSpec Spec;
		Spec.BaseCells = 2;
		Spec.LeftOriginCm = 0.0;
		Spec.Steps = Steps;
		Spec.Strength = GeneralPurposeMortar;
		return Spec;
	}

	/** Case D: case C plus three cells of masonry opposite, with the root joint at C's X. */
	inline FCorbelSpec CorbelCaseD(int32 Steps)
	{
		FCorbelSpec Spec;
		Spec.BaseCells = 5;
		Spec.LeftOriginCm = -3.0 * (CorbelBrickLengthCm + CorbelMortarJointCm);
		Spec.Steps = Steps;
		Spec.Strength = GeneralPurposeMortar;
		return Spec;
	}

	inline double CorbelBrickLengthOf(const FCorbelSpec& Spec) { return CorbelBrickLengthCm * Spec.Scale; }
	inline double CorbelBrickWidthOf(const FCorbelSpec& Spec) { return CorbelBrickWidthCm * Spec.Scale; }
	inline double CorbelBrickHeightOf(const FCorbelSpec& Spec) { return CorbelBrickHeightCm * Spec.Scale; }
	inline double CorbelJointOf(const FCorbelSpec& Spec) { return CorbelMortarJointCm * Spec.Scale; }
	inline double CorbelStepOf(const FCorbelSpec& Spec) { return Spec.StepCm * Spec.Scale; }

	/** One coordinating cell along the wall. */
	inline double CorbelCellPitchOf(const FCorbelSpec& Spec)
	{
		return CorbelBrickLengthOf(Spec) + CorbelJointOf(Spec);
	}

	/** One course up. */
	inline double CorbelCoursePitchOf(const FCorbelSpec& Spec)
	{
		return CorbelBrickHeightOf(Spec) + CorbelJointOf(Spec);
	}

	/** Centre height of a course, cm. */
	inline double CorbelCourseZCm(const FCorbelSpec& Spec, int32 Course)
	{
		return CorbelBrickHeightOf(Spec) / 2.0 + Course * CorbelCoursePitchOf(Spec);
	}

	/** One brick's weight, uu. */
	inline double CorbelBrickWeightUu(const FCorbelSpec& Spec)
	{
		const double MassKg = CorbelBrickDensityGramsPerCubicCm
			* CorbelBrickLengthOf(Spec) * CorbelBrickWidthOf(Spec) * CorbelBrickHeightOf(Spec)
			/ 1000.0;

		return MassKg * CorbelGravityCmPerSecondSquared;
	}

	/** One built structure, with the parts a test names. */
	struct FCorbelStructure
	{
		FStructure Structure;

		/** One box per piece handle. */
		TArray<FPieceBox> Boxes;

		/** Piece handles by course, ascending X, so `.Last()` is the outer face. */
		TArray<TArray<int32>> CoursePieces;

		/** Every piece above the base (the arm). */
		TArray<int32> ArmPieces;

		/** The outermost brick of each arm course, bottom first. */
		TArray<int32> OuterFacePieces;

		/**
		 * The bed joint under the arm's lowest outermost brick: the only place a corbel on an
		 * immovable base can fail, by opening in tension (COMPOSITE_DEPTH_DESIGN.md, overturning).
		 */
		int32 RootJoint = INDEX_NONE;

		/** The arm's lowest outermost brick, which stands on the root joint. */
		int32 RootPiece = INDEX_NONE;

		/** The base's top-course outermost brick, which the root joint stands on. */
		int32 RootSeatPiece = INDEX_NONE;

		/** The outermost brick of the top arm course. */
		int32 TipPiece = INDEX_NONE;

		/** How far the tip reaches past the base's outer brick, cm. */
		double ProjectionCm = 0.0;
	};

	/**
	 * Build via DestructionCorbel::Build and index the result; all values are read back from
	 * the layout. Course comes from each brick's height / course pitch. Handles arrive in
	 * ascending X within a course, so `.Last()` is the outer face without a sort.
	 *
	 * @return false if the spec could not describe a structure.
	 */
	inline bool CorbelBuild(const FCorbelSpec& Spec, FCorbelStructure& Out)
	{
		Out = FCorbelStructure();

		FBrickLayout Laid;

		if (!DestructionCorbel::Build(Spec, Laid))
		{
			return false;
		}

		Out.Structure = MoveTemp(Laid.Structure);
		Out.Boxes = MoveTemp(Laid.Boxes);

		const int32 TotalCourses = Spec.BaseCourses + Spec.Steps;

		Out.CoursePieces.SetNum(TotalCourses);

		for (int32 Piece = 0; Piece < Out.Boxes.Num(); ++Piece)
		{
			const int32 Course = FMath::RoundToInt32(
				(Out.Boxes[Piece].CentreCm.Z - CorbelBrickHeightOf(Spec) / 2.0)
					/ CorbelCoursePitchOf(Spec));

			if (!Out.CoursePieces.IsValidIndex(Course))
			{
				return false;
			}

			Out.CoursePieces[Course].Add(Piece);

			if (Course >= Spec.BaseCourses)
			{
				Out.ArmPieces.Add(Piece);
			}
		}

		for (int32 Course = Spec.BaseCourses; Course < TotalCourses; ++Course)
		{
			Out.OuterFacePieces.Add(Out.CoursePieces[Course].Last());
		}

		Out.RootSeatPiece = Out.CoursePieces[Spec.BaseCourses - 1].Last();
		Out.RootPiece = Out.CoursePieces[Spec.BaseCourses].Last();
		Out.TipPiece = Out.CoursePieces[TotalCourses - 1].Last();

		for (int32 Joint = 0; Joint < Out.Structure.NumConnections(); ++Joint)
		{
			const FConnection& Connection = Out.Structure.GetConnection(Joint);

			if ((Connection.PieceA == Out.RootPiece && Connection.PieceB == Out.RootSeatPiece)
				|| (Connection.PieceA == Out.RootSeatPiece && Connection.PieceB == Out.RootPiece))
			{
				Out.RootJoint = Joint;
				break;
			}
		}

		Out.ProjectionCm =
			Out.Boxes[Out.TipPiece].CentreCm.X - Out.Boxes[Out.RootSeatPiece].CentreCm.X;

		return Out.RootJoint != INDEX_NONE;
	}

	/**
	 * Beam-theory reading of a bed joint (patch section plus the deep beam t*D^2/6 over it), for
	 * explaining printed numbers and naming the governing axis. Not asserted on: it mirrors
	 * ComputeUtilisation's shape, so agreement proves little. Tests assert orderings and ratios
	 * between structures instead.
	 */
	struct FCorbelJointReading
	{
		/** Mean normal stress, MPa, positive in tension. */
		double NormalStressMPa = 0.0;

		/** Worst-corner bending stress on the joint's own patch, MPa. */
		double PatchBendingStressMPa = 0.0;

		/** The same moment against the deep beam over the joint, MPa. */
		double CompositeBendingStressMPa = 0.0;

		bool bCompositeGoverns = false;

		double TensionUtilisation = 0.0;
		double CompressionUtilisation = 0.0;
		double ShearUtilisation = 0.0;

		/** The worst of the three (what ComputeUtilisation returns). */
		double Worst = 0.0;
	};

	inline FCorbelJointReading CorbelReadBedJoint(
		const FVector& ForceUu,
		const FVector& MomentUuCm,
		const FVector& HalfExtentCm,
		double AreaSqCm,
		double CompositeDepthCm,
		const FConnectionStrength& Strength)
	{
		FCorbelJointReading Out;

		const double NormalUu = FMath::Abs(ForceUu.Z);
		const double ShearUu = FVector(ForceUu.X, ForceUu.Y, 0.0).Size();

		Out.NormalStressMPa = -NormalUu / (AreaSqCm * CorbelForceUnitsPerMPaPerSqCm);

		/*
		 * A moment about X is resisted by depth along Y and vice versa. A corbel stepping along X
		 * bends about Y; both are computed so neither is silently mis-sectioned.
		 */
		const double PatchModulusUCm3 = CorbelSectionModulusCm3(HalfExtentCm.X, HalfExtentCm.Y);
		const double PatchModulusVCm3 = CorbelSectionModulusCm3(HalfExtentCm.Y, HalfExtentCm.X);

		const double BendingAboutUMPa = MomentUuCm.X == 0.0
			? 0.0
			: FMath::Abs(MomentUuCm.X) / (PatchModulusUCm3 * CorbelForceUnitsPerMPaPerSqCm);

		const double BendingAboutVMPa = MomentUuCm.Y == 0.0
			? 0.0
			: FMath::Abs(MomentUuCm.Y) / (PatchModulusVCm3 * CorbelForceUnitsPerMPaPerSqCm);

		Out.PatchBendingStressMPa = BendingAboutUMPa + BendingAboutVMPa;

		double PeakTensionMPa = FMath::Max(0.0, Out.NormalStressMPa + Out.PatchBendingStressMPa);
		double PeakCompressionMPa =
			FMath::Max(0.0, Out.PatchBendingStressMPa - Out.NormalStressMPa);

		if (CompositeDepthCm > 0.0 && FMath::IsFinite(CompositeDepthCm))
		{
			const double CompositeUCm3 =
				CorbelCompositeModulusCm3(2.0 * HalfExtentCm.X, CompositeDepthCm);
			const double CompositeVCm3 =
				CorbelCompositeModulusCm3(2.0 * HalfExtentCm.Y, CompositeDepthCm);

			Out.CompositeBendingStressMPa =
				(MomentUuCm.X == 0.0
					? 0.0
					: FMath::Abs(MomentUuCm.X) / (CompositeUCm3 * CorbelForceUnitsPerMPaPerSqCm))
				+ (MomentUuCm.Y == 0.0
					? 0.0
					: FMath::Abs(MomentUuCm.Y) / (CompositeVCm3 * CorbelForceUnitsPerMPaPerSqCm));

			if (Out.NormalStressMPa <= 0.0 && Out.CompositeBendingStressMPa < PeakTensionMPa)
			{
				Out.bCompositeGoverns = true;
				PeakTensionMPa = Out.CompositeBendingStressMPa;
				PeakCompressionMPa = FMath::Max(
					Out.CompositeBendingStressMPa, -Out.NormalStressMPa);
			}
		}

		const double ShearStressMPa = ShearUu / (AreaSqCm * CorbelForceUnitsPerMPaPerSqCm);

		const double ShearCapacityMPa = FMath::Min(
			Strength.ShearCohesionMPa + Strength.FrictionCoefficient * (-Out.NormalStressMPa),
			Strength.MaxShearStrengthMPa);

		Out.TensionUtilisation = Strength.TensileStrengthMPa > 0.0
			? PeakTensionMPa / Strength.TensileStrengthMPa
			: (PeakTensionMPa > 0.0 ? TNumericLimits<double>::Max() : 0.0);

		Out.CompressionUtilisation = PeakCompressionMPa / Strength.CompressiveStrengthMPa;

		Out.ShearUtilisation = ShearCapacityMPa > 0.0
			? ShearStressMPa / ShearCapacityMPa
			: (ShearStressMPa > 0.0 ? TNumericLimits<double>::Max() : 0.0);

		Out.Worst = FMath::Max3(
			Out.TensionUtilisation, Out.CompressionUtilisation, Out.ShearUtilisation);

		return Out;
	}

	/** How many of the arm's pieces have lost their path to the ground. */
	inline int32 CorbelArmPiecesWithNoPath(const FCorbelStructure& Built)
	{
		int32 Lost = 0;

		for (const int32 Piece : Built.ArmPieces)
		{
			if (!Built.Structure.IsPieceRemoved(Piece) && !Built.Structure.IsPieceSupported(Piece))
			{
				++Lost;
			}
		}

		return Lost;
	}

	/** The worst utilisation anywhere in the structure, and the joint that reads it. */
	inline double CorbelWorstUtilisation(const FCorbelStructure& Built, int32& OutJoint)
	{
		double Worst = 0.0;
		OutJoint = INDEX_NONE;

		for (int32 Joint = 0; Joint < Built.Structure.NumConnections(); ++Joint)
		{
			const double Utilisation = Built.Structure.GetConnectionUtilisation(Joint);

			if (FMath::IsFinite(Utilisation) && Utilisation > Worst)
			{
				Worst = Utilisation;
				OutJoint = Joint;
			}
		}

		return Worst;
	}
}
