// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

/**
 * The arching wall as geometry and beam theory, shared by every ARCHING_DESIGN.md slice test so
 * they cut the same 7 x 30 flush wall (intact anchor 0.0036748258197270385). World-free.
 *
 * Named namespace, not anonymous: unity builds merge anonymous namespaces and collide helpers.
 * Functions are inline for non-unity builds.
 */
namespace StructureArchingTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * Everything below is written from first principles rather than imported, so a wrong
	 * production constant is caught rather than agreed with.
	 */

	/** DESIGN.md's standard UK metric clay brick, cm. */
	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickWidthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;

	constexpr double MortarJointCm = 1.0;

	constexpr double BrickPitchCm = BrickLengthCm + MortarJointCm;
	constexpr double CoursePitchCm = BrickHeightCm + MortarJointCm;
	constexpr double BondOffsetCm = BrickPitchCm / 2.0;

	/**
	 * 1.9 g/cm3, density first to match Layout::PieceMassKg bit for bit (volume first is one ulp
	 * low, which would break the bit-identical guards).
	 */
	constexpr double BrickMassKg = 1.9 * BrickLengthCm * BrickWidthCm * BrickHeightCm / 1000.0;

	/** With 1 uu = 1 cm and mass in kg, MassKg * 980 is a force in uu. */
	constexpr double BrickWeightUu = BrickMassKg * 980.0;

	/**
	 * uu per MPa.cm2: 1 N = 100 uu and 1 cm2 = 100 mm2, so 10000. Deliberately not the production
	 * constant, so this fails if that one is wrong.
	 */
	constexpr double ForceUnitsPerMPaPerSqCm = 100.0 * 100.0;

	/** The square half-brick bed patch a brick keeps after losing one of its two seats. */
	constexpr double HalfSeatAreaSqCm = BrickWidthCm * BrickWidthCm;

	constexpr double HalfSeatHalfExtentCm = BrickWidthCm / 2.0;

	/** Arm of a half-seated brick's own weight about its patch: a quarter pitch, 5.625 cm. */
	constexpr double HalfSeatEccentricityCm = BondOffsetCm / 2.0;

	/**
	 * Kern (middle third) half-width: h/6 == half-extent/3. For the 10.25 cm patch that is
	 * 1.7083 cm, so 5.625 cm is well outside it.
	 */
	constexpr double KernFromHalfExtentCm(double HalfExtentCm)
	{
		return HalfExtentCm / 3.0;
	}

	/** Elastic section modulus of a rectangle, cm3: W = I/c = (4/3)*HalfAlong*HalfAcross^2. */
	constexpr double SectionModulusCm3(double HalfAlongCm, double HalfAcrossCm)
	{
		return (4.0 / 3.0) * HalfAlongCm * HalfAcrossCm * HalfAcrossCm;
	}

	/** Full-precision print, so a last-bit mismatch is visible. */
	inline FString Bits(double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	/** A whole-brick box centred at the given point. */
	inline FPieceBox BrickBoxAt(double CentreXCm, double CentreYCm, double CentreZCm)
	{
		FPieceBox Box;
		Box.CentreCm = FVector(CentreXCm, CentreYCm, CentreZCm);
		Box.ExtentCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm) * 0.5;
		return Box;
	}

	/**
	 * Bed-joint oracle from beam theory, not the solver, taking the solver's reported force.
	 *
	 * The caller chooses the moment: the solver's published moment checks self-consistency, but it
	 * already includes the arching cap, so a row wanting the un-arched answer builds the moment
	 * from the fixture's lever arm.
	 *
	 * The two in-plane moduli differ (2.5x on an end face), so both are written out. Friction uses
	 * the mean compressive stress, not the peak (EN 1996-1-1 §3.6.2).
	 */
	struct FBedJointReading
	{
		/** Averaged normal stress, MPa, signed and positive in tension. */
		double NormalStressMPa = 0.0;

		/** Worst-corner bending stress, MPa: the two in-plane axes add. */
		double BendingStressMPa = 0.0;

		double TensionUtilisation = 0.0;
		double CompressionUtilisation = 0.0;
		double ShearUtilisation = 0.0;

		/** The worst of the three, as ComputeUtilisation returns. */
		double Worst = 0.0;
	};

	inline FBedJointReading ReadBedJoint(
		const FVector& ForceUu,
		const FVector& MomentUuCm,
		const FVector& HalfExtentCm,
		double AreaSqCm,
		const FConnectionStrength& Strength)
	{
		FBedJointReading Out;

		const double NormalUu = FMath::Abs(ForceUu.Z);
		const double ShearUu = FVector(ForceUu.X, ForceUu.Y, 0.0).Size();

		const double ModulusUCm3 = SectionModulusCm3(HalfExtentCm.X, HalfExtentCm.Y);
		const double ModulusVCm3 = SectionModulusCm3(HalfExtentCm.Y, HalfExtentCm.X);

		Out.NormalStressMPa = -NormalUu / (AreaSqCm * ForceUnitsPerMPaPerSqCm);

		/*
		 * Test the moment, not the modulus: zero moment over an unmeasured (zero) modulus would be
		 * 0/0 = NaN. A real moment over a zero modulus gives infinity, which fails closed.
		 */
		const double BendingAboutUMPa = MomentUuCm.X == 0.0
			? 0.0
			: FMath::Abs(MomentUuCm.X) / (ModulusUCm3 * ForceUnitsPerMPaPerSqCm);

		const double BendingAboutVMPa = MomentUuCm.Y == 0.0
			? 0.0
			: FMath::Abs(MomentUuCm.Y) / (ModulusVCm3 * ForceUnitsPerMPaPerSqCm);

		Out.BendingStressMPa = BendingAboutUMPa + BendingAboutVMPa;

		const double PeakTensionMPa = FMath::Max(0.0, Out.NormalStressMPa + Out.BendingStressMPa);
		const double PeakCompressionMPa = FMath::Max(0.0, Out.BendingStressMPa - Out.NormalStressMPa);

		const double ShearStressMPa = ShearUu / (AreaSqCm * ForceUnitsPerMPaPerSqCm);

		const double ShearCapacityMPa = FMath::Min(
			Strength.ShearCohesionMPa + Strength.FrictionCoefficient * (-Out.NormalStressMPa),
			Strength.MaxShearStrengthMPa);

		Out.TensionUtilisation = PeakTensionMPa / Strength.TensileStrengthMPa;
		Out.CompressionUtilisation = PeakCompressionMPa / Strength.CompressiveStrengthMPa;
		Out.ShearUtilisation = ShearStressMPa / ShearCapacityMPa;

		Out.Worst = FMath::Max3(
			Out.TensionUtilisation, Out.CompressionUtilisation, Out.ShearUtilisation);

		return Out;
	}

	/**
	 * The joint's reading with the thrust line held at the kern edge. The cap
	 * k = min(1, |sigma_n| / sigma_b) makes sigma_b = |sigma_n|, so:
	 *
	 *     peak tension     = max(0, sigma_n + sigma_b) = 0            exactly
	 *     peak compression = max(0, sigma_b - sigma_n) = 2|sigma_n|   exactly
	 *
	 * Not zero: dropping the moment instead of capping it would read |sigma_n|, off by 2x.
	 */
	inline double ArchedUtilisation(
		const FBedJointReading& Reading, const FConnectionStrength& Strength)
	{
		return 2.0 * FMath::Abs(Reading.NormalStressMPa) / Strength.CompressiveStrengthMPa;
	}

	/**
	 * A 7-brick-wide flush wall of any height. At 30 courses (the default) a deletion with 18+
	 * courses above takes out the half seat (capacity at 17.18 brick weights). Seven is the
	 * narrowest width with a truly interior cell. Flush so an intact wall has zero eccentricity.
	 *
	 * Height is a parameter for COMPOSITE_DEPTH_DESIGN.md slice 2, where a free-end joint's section
	 * is measured by walking up; width stays 7 so only depth varies.
	 */
	inline FRunningBondSpec ArchWallSpecOfHeight(int32 CoursesHigh)
	{
		FRunningBondSpec Spec;
		Spec.BrickSizeCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm);
		Spec.JointThicknessCm = MortarJointCm;
		Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
		Spec.CoursesHigh = CoursesHigh;
		Spec.BricksPerCourse = 7;
		Spec.End = EWallEnd::Flush;
		Spec.Strength = GeneralPurposeMortar;
		return Spec;
	}

	inline FRunningBondSpec ArchWallSpec()
	{
		return ArchWallSpecOfHeight(30);
	}

	/** 15 even courses of 7 full bricks, 15 odd courses of 6 full bricks and 2 half bats. */
	constexpr int32 ArchWallPieceCount = 15 * 7 + 15 * 8;

	/**
	 * The free-end seat reading on the 30-course wall (outermost course-0 brick deleted). An
	 * agreement anchor: ACorbelResistsWithItsWholeDepth PART 2 and AFreeEndDeletionInATallWall
	 * read the same joint and both assert exactly against this. It moves with lambda
	 * (COMPOSITE_DEPTH_DESIGN.md, provisional until slice 3).
	 *
	 * 0.0185878 * 43.2749^2 / 486.2129 at lambda = 3.464 on the mean basis. Written as the old
	 * measured bits / 7.0, within an ulp of production but not provably bit-identical; if either
	 * consumer fails by the last bit, re-pin this to the measured value.
	 */
	constexpr double FreeEndThirtyCourseUtilisation = 0.50114032912317807 / 7.0;

	/** Centre height of a course, cm. */
	constexpr double ArchWallCourseZCm(int32 Course)
	{
		return BrickHeightCm / 2.0 + Course * CoursePitchCm;
	}

	/** Centre of the Nth FULL brick of an odd course: the flush half bat comes first. */
	constexpr double ArchWallOddBrickXCm(int32 Index)
	{
		return BondOffsetCm + Index * BrickPitchCm;
	}

	/** Centre of the Nth brick of an even course. */
	constexpr double ArchWallEvenBrickXCm(int32 Index)
	{
		return Index * BrickPitchCm;
	}

	/** The deleted brick: third full brick of course 1 (x = 56.25), interior with two columns either side. */
	constexpr double DeletedBrickXCm = ArchWallOddBrickXCm(2);
	constexpr int32 DeletedBrickCourse = 1;

	/**
	 * The two course-2 bricks left half-seated, each overhanging its one seat by 5.625 cm toward the
	 * other. Their shared head joint is on each one's eccentric side: that is the arch.
	 */
	constexpr double LeftHalfSeatedXCm = ArchWallEvenBrickXCm(2);
	constexpr double RightHalfSeatedXCm = ArchWallEvenBrickXCm(3);
	constexpr int32 HalfSeatedCourse = 2;

	/** The seat each keeps: the course-1 brick on its outboard side. */
	constexpr double LeftSurvivingSeatXCm = ArchWallOddBrickXCm(1);
	constexpr double RightSurvivingSeatXCm = ArchWallOddBrickXCm(3);

	/*
	 * The scenario wall and arch geometry, for slices 3 and 4, which need a wall wide enough for a
	 * 20-cell opening and tall enough for cover to govern arching depth.
	 */

	/**
	 * Arching depth per unit span: BS 5977-1's equilateral load triangle, sqrt(3)/2, used only to
	 * cap arch depth (ARCHING_DESIGN.md). Written as the design's 0.866 (3e-5 relative difference).
	 */
	constexpr double ArchingDepthPerSpan = 0.866;

	/**
	 * The game mode's 30 x 40 scenario wall, which ARCHING_DESIGN's span table was worked against.
	 * Same brick, bond and flush end as ArchWallSpec; only size differs.
	 */
	constexpr int32 ScenarioWallCourses = 40;
	constexpr int32 ScenarioWallBricksPerCourse = 30;

	/** 20 even courses of 30 full bricks, 20 odd courses of 29 full bricks and 2 half bats. */
	constexpr int32 ScenarioWallPieceCount = 20 * 30 + 20 * 31;

	inline FRunningBondSpec ScenarioWallSpec()
	{
		FRunningBondSpec Spec;
		Spec.BrickSizeCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm);
		Spec.JointThicknessCm = MortarJointCm;
		Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
		Spec.CoursesHigh = ScenarioWallCourses;
		Spec.BricksPerCourse = ScenarioWallBricksPerCourse;
		Spec.End = EWallEnd::Flush;
		Spec.Strength = GeneralPurposeMortar;
		return Spec;
	}

	/** Centre X of the Nth brick of a course, for either bond parity. */
	constexpr double ArchWallBrickXCm(int32 Course, int32 Index)
	{
		return Course % 2 == 0 ? ArchWallEvenBrickXCm(Index) : ArchWallOddBrickXCm(Index);
	}

	/**
	 * Masonry over a one-course cut, cm, in whole course pitches including the spanning course
	 * (e.g. 40 courses cut at 1: 38 * 7.5 = 285, ARCHING_DESIGN's figure). Minimum is 7.5, not 0.
	 */
	constexpr double CoverAboveCutCm(int32 CoursesHigh, int32 CutCourse)
	{
		return (CoursesHigh - CutCourse - 1) * CoursePitchCm;
	}

	/**
	 * Arch thrust per vertical reaction:
	 *
	 *     d_e = min( cover above the span , 0.866 * L )       arching depth
	 *     r   = d_e / 3                                       thrust line rise, kern-limited
	 *     H   = W * L / (8r)     V = W / 2                    per abutment
	 *
	 * H/V = 3L / (4 d_e); W cancels, so it is pure geometry. V is what the springing bed joint
	 * reports, including the springing's own column. Excluding those columns reads ~0.78 instead of
	 * 0.866 on a ten-cell case.
	 */
	inline double ThrustPerReaction(double SpanCm, double CoverCm)
	{
		const double DepthCm = FMath::Min(CoverCm, ArchingDepthPerSpan * SpanCm);

		return 3.0 * SpanCm / (4.0 * DepthCm);
	}

	/** A brick's whole end face. */
	constexpr double HeadJointAreaSqCm = BrickWidthCm * BrickHeightCm;

	/**
	 * Head-joint oracle from beam theory. Gravity is pure shear on a head joint; capacity is
	 * Mohr-Coulomb c + mu * sigma_n, capped. No bending term: a re-seated piece is indeterminate,
	 * so its moment is zero (asserted at the call site).
	 */
	struct FHeadJointReading
	{
		/** Averaged normal stress, MPa, signed and positive in tension. */
		double NormalStressMPa = 0.0;

		double ShearStressMPa = 0.0;
		double ShearCapacityMPa = 0.0;

		double TensionUtilisation = 0.0;
		double CompressionUtilisation = 0.0;
		double ShearUtilisation = 0.0;

		/** The worst of the three, as ComputeUtilisation returns. */
		double Worst = 0.0;
	};

	inline FHeadJointReading ReadHeadJoint(
		const FVector& ForceUu,
		const FVector& UnitNormal,
		double AreaSqCm,
		const FConnectionStrength& Strength)
	{
		FHeadJointReading Out;

		// Positive along the normal is tension (ClassifyForce's convention).
		const double AlongNormalUu = FVector::DotProduct(ForceUu, UnitNormal);
		const double ShearUu = (ForceUu - AlongNormalUu * UnitNormal).Size();

		Out.NormalStressMPa = AlongNormalUu / (AreaSqCm * ForceUnitsPerMPaPerSqCm);
		Out.ShearStressMPa = ShearUu / (AreaSqCm * ForceUnitsPerMPaPerSqCm);

		Out.ShearCapacityMPa = FMath::Min(
			Strength.ShearCohesionMPa
				+ Strength.FrictionCoefficient * FMath::Max(0.0, -Out.NormalStressMPa),
			Strength.MaxShearStrengthMPa);

		Out.TensionUtilisation = FMath::Max(0.0, Out.NormalStressMPa) / Strength.TensileStrengthMPa;

		Out.CompressionUtilisation =
			FMath::Max(0.0, -Out.NormalStressMPa) / Strength.CompressiveStrengthMPa;

		Out.ShearUtilisation = Out.ShearCapacityMPa > 0.0
			? Out.ShearStressMPa / Out.ShearCapacityMPa
			: (Out.ShearStressMPa > 0.0 ? TNumericLimits<double>::Max() : 0.0);

		Out.Worst = FMath::Max3(
			Out.TensionUtilisation, Out.CompressionUtilisation, Out.ShearUtilisation);

		return Out;
	}

	/** How many intact bed joints this piece currently rests on. */
	inline int32 IntactSeatsBeneath(const FStructure& Structure, int32 Piece)
	{
		int32 Seats = 0;

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			// GetJointRole is pure geometry and still reports BedBeneath for given joints.
			if (Structure.GetJointRole(Joint, Piece) == EJointRole::BedBeneath
				&& !Structure.GetConnection(Joint).HasGiven())
			{
				++Seats;
			}
		}

		return Seats;
	}

	/** The one intact bed joint under a piece, or INDEX_NONE if it does not have exactly one. */
	inline int32 TheOneIntactSeatBeneath(const FStructure& Structure, int32 Piece)
	{
		int32 Seat = INDEX_NONE;
		int32 Seats = 0;

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			if (Structure.GetJointRole(Joint, Piece) == EJointRole::BedBeneath
				&& !Structure.GetConnection(Joint).HasGiven())
			{
				Seat = Joint;
				++Seats;
			}
		}

		return Seats == 1 ? Seat : INDEX_NONE;
	}
}
