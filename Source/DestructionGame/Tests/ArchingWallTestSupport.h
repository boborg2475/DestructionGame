// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

/**
 * THE ARCHING WALL, AS GEOMETRY AND AS BEAM THEORY — SHARED BY EVERY TEST THAT CUTS A HOLE IN IT.
 *
 * ARCHING_DESIGN.md slices the arch work into five, and every slice cuts a hole of a different
 * width into the SAME 7 x 30 flush wall so that its answer can be compared against the slice
 * before it. One brick out is slice 1, three bricks out is slice 2, and the reason they share a
 * wall is that the intact wall's 0.0036748258197270385 is the anchor both of them are measured
 * against. A second copy of a wall definition is two fixtures that drift.
 *
 * WORLD-FREE ON PURPOSE. Everything here is boxes and doubles, so the fast suite pays nothing.
 *
 * NAMED NAMESPACE, not anonymous, and named for what it holds. An anonymous namespace is private
 * to a TRANSLATION UNIT rather than to a file, and a unity build merges many files into one — at
 * which point two anonymous namespaces in the blob are the SAME namespace and identically-named
 * helpers in files that never refer to each other are a hard compile error. Free functions are
 * `inline` because more than one translation unit includes this and a non-unity build would
 * otherwise fail at link.
 */
namespace StructureArchingTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * --- everything below is spelled out here rather than imported ------------------------
	 *
	 * The point of a test is to disagree with production when production is wrong, and a test
	 * that reaches for the same named constant agrees with it instead. So the brick, gravity,
	 * the SI conversion, the section modulus and the whole beam-theory reading of a bed joint
	 * are written from first principles here; the strengths are asserted against the profile
	 * rather than copied out of it.
	 */

	/** DESIGN.md's standard UK metric clay brick, cm. */
	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickWidthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;

	/** The standard 1 cm mortar joint, which is what makes the coordinating grid 22.5 cm. */
	constexpr double MortarJointCm = 1.0;

	constexpr double BrickPitchCm = BrickLengthCm + MortarJointCm;
	constexpr double CoursePitchCm = BrickHeightCm + MortarJointCm;
	constexpr double BondOffsetCm = BrickPitchCm / 2.0;

	/**
	 * 1.9 g/cm3, DENSITY FIRST in the product to match Layout::PieceMassKg exactly.
	 *
	 * Volume-first lands one ulp low at 2.7216312499999997, which would move every figure in
	 * this file in the last bit and turn the bit-identical guards into tolerance questions.
	 */
	constexpr double BrickMassKg = 1.9 * BrickLengthCm * BrickWidthCm * BrickHeightCm / 1000.0;

	/** 980 cm/s2. In a world where 1 uu = 1 cm and mass is kg, MassKg * 980 IS a force in uu. */
	constexpr double BrickWeightUu = BrickMassKg * 980.0;

	/**
	 * Force, in Unreal units, that loads one square centimetre to one megapascal.
	 *
	 * 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa (= 1 N/mm2) over 1 cm2 is 10000 uu.
	 * DELIBERATELY NOT DestructionForce::ForceUnitsPerMPaSqCm: this test has to fail if that
	 * constant is wrong, not agree with it.
	 */
	constexpr double ForceUnitsPerMPaPerSqCm = 100.0 * 100.0;

	/** The half-brick bed patch a running-bond brick keeps when it loses one of its two seats. */
	constexpr double HalfSeatAreaSqCm = BrickWidthCm * BrickWidthCm;

	/** Half the bed patch's extent on each in-plane axis; it is square, so they are equal. */
	constexpr double HalfSeatHalfExtentCm = BrickWidthCm / 2.0;

	/**
	 * How far a half-seated brick's own weight acts from the centroid of the patch left to it.
	 *
	 * Running bond offsets alternate courses by half a cell, so the smallest step an edge can
	 * take is 11.25 cm, and the surviving overlap's centroid sits a quarter of a brick pitch
	 * from the brick's own centre. 22.5 / 4 = 5.625 cm.
	 */
	constexpr double HalfSeatEccentricityCm = BondOffsetCm / 2.0;

	/**
	 * THE MIDDLE THIRD, and it is beam theory rather than a code figure.
	 *
	 * A rectangular section of depth h keeps its whole face in compression while the load path
	 * stays inside +/- h/6 of the centroid; past that some part of the face is opening. For the
	 * 10.25 cm deep bed patch that is +/- 1.7083 cm, and 5.625 cm is more than three times out.
	 *
	 * Written from the HALF extent because that is what the joint carries: h/6 == half/3.
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

	/** Print a double so a comparison that failed in the last bit is readable as one. */
	inline FString Bits(double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	/** A box the size of a whole brick, centred where it is asked for. */
	inline FPieceBox BrickBoxAt(double CentreXCm, double CentreYCm, double CentreZCm)
	{
		FPieceBox Box;
		Box.CentreCm = FVector(CentreXCm, CentreYCm, CentreZCm);
		Box.ExtentCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm) * 0.5;
		return Box;
	}

	/**
	 * WHAT A BED JOINT READS UNDER A FORCE AND A MOMENT — the ORACLE, derived from published
	 * beam theory and not from the solver.
	 *
	 * The whole point of these tests is to say what a joint should read BEFORE and AFTER the
	 * arching cap, and both answers are stresses on a rectangle. Recomputing them here from
	 * FStructure's own reported force — one number the solver publishes and this file never
	 * derives — is what makes "the arch did not fire" a checkable claim on every negative row
	 * rather than a literal that has to be re-measured each time a fixture moves.
	 *
	 * THE MOMENT PASSED IN IS THE CALLER'S CHOICE AND THAT MATTERS. Handing over the moment
	 * the SOLVER published answers "is this joint's reading self-consistent with its own
	 * published moment", which is the right question for a negative row and the WRONG question
	 * for "would this joint have cantilevered": the cap changes the published moment, so an
	 * oracle built on it necessarily agrees with the cap. A row that wants the un-arched
	 * answer has to build the moment out of the fixture's own lever arm instead.
	 *
	 * A bed joint separates on Z, so its in-plane frame is X and Y: a moment about X is
	 * resisted by the depth along Y and vice versa. The pair is NOT interchangeable — on a
	 * brick's end face the two moduli differ by a factor of 2.5 — so both are written out.
	 *
	 * FRICTION IS BOUGHT BY THE MEAN COMPRESSIVE STRESS, NOT THE PEAK, which is what
	 * EN 1996-1-1 §3.6.2 defines f_vk against and what production does. Every bed joint in
	 * these files carries a purely vertical force until slice 3 lands the horizontal thrust,
	 * so shear is exactly zero and the term never decides anything — it is written out so
	 * that a fixture where it DID would be visible.
	 */
	struct FBedJointReading
	{
		/** Averaged normal stress, MPa, signed and positive in tension. */
		double NormalStressMPa = 0.0;

		/** Worst-corner bending stress, MPa: the two in-plane axes ADD. */
		double BendingStressMPa = 0.0;

		double TensionUtilisation = 0.0;
		double CompressionUtilisation = 0.0;
		double ShearUtilisation = 0.0;

		/** The worst of the three, which is what ComputeUtilisation returns. */
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
		 * NO MOMENT MEANS NO BENDING TERM, AND THE TEST IS ON THE MOMENT RATHER THAN ON THE
		 * MODULUS. A joint whose rectangle nobody measured has a modulus of zero on both axes,
		 * and it is perfectly healthy — the area alone answers a centred load exactly — so
		 * dividing anyway gives 0/0 and the NaN then propagates into every stress here. A
		 * moment that IS there against a section that is not is a different thing entirely and
		 * comes out infinite, which is the fail-closed answer.
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
	 * WHAT THE SAME JOINT READS ONCE THE THRUST LINE IS HELD AT THE KERN EDGE.
	 *
	 * The relief is a cap on the moment VECTOR, k = min(1, |sigma_n| / sigma_b), so at the cap
	 * sigma_b IS |sigma_n| and the two normal answers fall out exactly:
	 *
	 *     peak tension     = max(0, sigma_n + sigma_b) = 0            exactly
	 *     peak compression = max(0, sigma_b - sigma_n) = 2|sigma_n|   exactly
	 *
	 * Shear is untouched and is zero here. So an arched bed joint reads twice its own mean
	 * compressive stress against the compressive strength, and nothing else — which is
	 * DELIBERATELY NOT ZERO. Setting the moment to zero instead of capping it would read
	 * |sigma_n| and be wrong by exactly a factor of two.
	 */
	inline double ArchedUtilisation(
		const FBedJointReading& Reading, const FConnectionStrength& Strength)
	{
		return 2.0 * FMath::Abs(Reading.NormalStressMPa) / Strength.CompressiveStrengthMPa;
	}

	/**
	 * THE WALL THE PLAYER IS LOOKING AT, SHRUNK TO WHAT A WORLD-FREE TEST NEEDS.
	 *
	 * Thirty courses is what makes the defect bite: a brick in course j of an N-course wall
	 * carries about N - j brick weights, a half seat reaches capacity at 17.18 of them, so any
	 * deletion with 18 or more courses above it takes the joint above it out. Seven bricks per
	 * course is the narrowest wall that leaves a genuinely INTERIOR cell — one whose two
	 * neighbouring columns are themselves two columns clear of either end — so nothing here is
	 * a free-end effect.
	 *
	 * FLUSH RATHER THAN RAGGED, and it is not a style choice. A ragged wall's alternate courses
	 * step in, so the end brick of every even course rests on ONE brick below it and is already
	 * a half-seated cantilever before anyone has deleted anything. A flush end fills that half
	 * cell with a half bat, every brick has two seats, and the eccentricity of an intact wall
	 * is exactly zero — which is the only baseline against which "one deletion did this" means
	 * anything. ADestructionGameGameMode lays a flush wall for its scenario.
	 */
	inline FRunningBondSpec ArchWallSpec()
	{
		FRunningBondSpec Spec;
		Spec.BrickSizeCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm);
		Spec.JointThicknessCm = MortarJointCm;
		Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
		Spec.CoursesHigh = 30;
		Spec.BricksPerCourse = 7;
		Spec.End = EWallEnd::Flush;
		Spec.Strength = GeneralPurposeMortar;
		return Spec;
	}

	/** 15 even courses of 7 full bricks, 15 odd courses of 6 full bricks and 2 half bats. */
	constexpr int32 ArchWallPieceCount = 15 * 7 + 15 * 8;

	/** Centre height of a course, cm: half a brick up, then one course pitch per course. */
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

	/**
	 * THE ONE BRICK THE PLAYER DELETES: the third full brick of course 1, at x = 56.25.
	 *
	 * INTERIOR, and it has to be. Course 1 runs half bat, then full bricks at 11.25, 33.75,
	 * 56.25, 78.75, 101.25, 123.75, then half bat. The third of those has two full columns of
	 * wall either side of it, so neither of the two bricks left half-seated above it is
	 * anywhere near a free end and neither can borrow anything from one.
	 */
	constexpr double DeletedBrickXCm = ArchWallOddBrickXCm(2);
	constexpr int32 DeletedBrickCourse = 1;

	/**
	 * THE TWO BRICKS LEFT HALF-SEATED, in course 2, at x = 45 and x = 67.5.
	 *
	 * Each of them spanned the deleted brick and one other, so each keeps exactly one 10.25 x
	 * 10.25 seat and overhangs it by 5.625 cm — TOWARD THE OTHER. The head joint between them
	 * is therefore on each one's ECCENTRIC side, and each has its own independent seat on the
	 * course below, so neither is holding the other up. That is the arch.
	 */
	constexpr double LeftHalfSeatedXCm = ArchWallEvenBrickXCm(2);
	constexpr double RightHalfSeatedXCm = ArchWallEvenBrickXCm(3);
	constexpr int32 HalfSeatedCourse = 2;

	/** The seat each of them keeps: the course-1 full brick on its OUTBOARD side. */
	constexpr double LeftSurvivingSeatXCm = ArchWallOddBrickXCm(1);
	constexpr double RightSurvivingSeatXCm = ArchWallOddBrickXCm(3);
}
