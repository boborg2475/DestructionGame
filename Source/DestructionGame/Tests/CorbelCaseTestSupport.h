// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "Core/Corbel.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

/**
 * THE STEPPED CORBEL OFF AN IMMOVABLE BASE — CASES A TO D, AND EVERY SWEEP OVER THEM.
 *
 * `claude_plans/CORBEL_CASES.html` draws four structures the user reviewed and
 * `claude_plans/CORBEL_CASES_EF.html` proposes six tests over them. All six want the SAME
 * family of structures with one thing varied — step count, step size, scale, material, how
 * much masonry stands opposite — so the family lives here and each test names the row it
 * wants. A second copy of a corbel definition is two fixtures that drift, which is the rule
 * ArchingWallTestSupport.h already states for the wall.
 *
 * WHAT THE FOUR CASES ARE, IN THIS FIXTURE'S PARAMETERS:
 *
 *     A   a bare stepped arm of single bricks              `bFilled` false
 *     B   BaseCells 2, Steps 4                             filled
 *     C   BaseCells 2, Steps 10                            filled
 *     D   BaseCells 5, LeftOrigin -3 cells, Steps 10       C plus three cells opposite
 *
 * THE BRICKS ARE NOT LAID HERE ANY MORE. `Core/Corbel.h` lays them, because the corbel family
 * is a set of PLAYABLE SCENARIOS and no level can reach into Tests/. What remains here is what
 * a test needs to name and production does not: which pieces are the arm, which joint is the
 * root, how far the tip reaches. `Core.Corbel.LaysTheFamilyOnItsGrid` derives the whole family
 * from the coordinating grid independently and is what arbitrates the builder.
 *
 * THE SPEC IS PRODUCTION'S OWN TYPE, for the reason this header has always given: a second copy
 * of a corbel definition is two fixtures that drift. Its one difference is the profile — the
 * production spec defaults to a strength of all zeros, so the two case functions below name
 * `GeneralPurposeMortar` explicitly rather than leaning on a default.
 *
 * D IS C PLUS THREE CELLS ON THE LEFT AND NOTHING ELSE, AND THAT IS ARRANGED RATHER THAN
 * ASSUMED. Both put their base's top-course rightmost brick at the same absolute X, so the
 * root joint of the two structures is the same joint in the same place carrying the same
 * column — which is exactly what test E is trying to find out about. Shifting the left origin
 * by the three cells the base gains is what keeps that true.
 *
 * WORLD-FREE ON PURPOSE, like the two headers beside it: boxes and doubles, no UWorld.
 *
 * NAMED NAMESPACE, not anonymous, and every free name inside it carries a `Corbel` prefix.
 * An anonymous namespace is private to a TRANSLATION UNIT rather than to a file, and a unity
 * build merges many files into one — at which point two anonymous namespaces in the blob are
 * the SAME namespace and identically-named helpers in files that never refer to each other
 * are a hard compile error. Free functions are `inline` because more than one translation
 * unit includes this.
 */
namespace CorbelCaseTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * --- everything below is spelled out here rather than imported ------------------------
	 *
	 * Same discipline as ArchingWallTestSupport and StructureCompositeDepthTest: the brick,
	 * the grid, the SI conversion and the section moduli are written from first principles,
	 * and the strengths are ASSERTED against the profile at the call site rather than read
	 * out of it. A test that reaches for production's own constant agrees with a wrong one.
	 */

	/** DESIGN.md's standard UK metric clay brick, cm, at unit scale. */
	constexpr double CorbelBrickLengthCm = 21.5;
	constexpr double CorbelBrickWidthCm = 10.25;
	constexpr double CorbelBrickHeightCm = 6.5;

	/** The standard 1 cm mortar joint, which is what makes the coordinating grid 22.5 cm. */
	constexpr double CorbelMortarJointCm = 1.0;

	/** Clay at 1.9 g/cm3, asserted against ClayBrick at the call site rather than read off it. */
	constexpr double CorbelBrickDensityGramsPerCubicCm = 1.9;

	/**
	 * Force, in Unreal units, that loads one square centimetre to one megapascal.
	 *
	 * 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa (= 1 N/mm2) over 1 cm2 is 10000 uu.
	 * DELIBERATELY NOT DestructionForce::ForceUnitsPerMPaSqCm: these tests have to fail if
	 * that constant is wrong, not agree with it.
	 */
	constexpr double CorbelForceUnitsPerMPaPerSqCm = 100.0 * 100.0;

	/** 980 cm/s2. In a world where 1 uu = 1 cm and mass is kg, MassKg * 980 IS a force in uu. */
	constexpr double CorbelGravityCmPerSecondSquared = 980.0;

	/**
	 * THE PUBLISHED CORBELLING LIMIT FOR THIS BRICK, AND IT IS A CALIBRATION REFERENCE RATHER
	 * THAN A CONSTRAINT.
	 *
	 * `claude_plans/REAL_WORLD_CHECK.md`: US model codes and UK practice both limit the
	 * projection per course to the LESSER of one-third the unit bed depth and one-half the
	 * unit height, and the total projection to the wall thickness. For 21.5 x 10.25 x 6.5 cm
	 * that is min(10.25/3, 6.5/2) = 3.25 cm per course and 10.25 cm in total.
	 *
	 * THIS PROJECT DELIBERATELY EXCEEDS BOTH, AND THE USER RULED THAT IT SHOULD. Every existing
	 * fixture steps half a cell — 11.25 cm, 3.46x the per-course limit — because the point of a
	 * destruction game is to find out what actually falls, and the interesting behaviour lives
	 * well past where an engineer stops guaranteeing anything. So these two numbers are here to
	 * be REPORTED AGAINST, as the safe anchor at one end of a deliberate ladder: the 3.25 cm
	 * step is the one geometry published practice unambiguously endorses, so it is the strongest
	 * "must stand" row available anywhere in this project, and where the model's own crossover
	 * sits as a MULTIPLE of these is the deliverable rather than any individual verdict.
	 */
	constexpr double CorbelCodeStepPerCourseCm = 3.25;
	constexpr double CorbelCodeTotalProjectionCm = 10.25;

	/** Print a double so a comparison that failed in the last bit is readable as one. */
	inline FString CorbelBits(double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	/** Elastic section modulus of a rectangle, cm3: W = I/c = (4/3)*HalfAlong*HalfAcross^2. */
	constexpr double CorbelSectionModulusCm3(double HalfAlongCm, double HalfAcrossCm)
	{
		return (4.0 / 3.0) * HalfAlongCm * HalfAcrossCm * HalfAcrossCm;
	}

	/** The DEEP BEAM's section, cm3: W = t*D^2/6 for a wall t thick and D deep. */
	constexpr double CorbelCompositeModulusCm3(double WallThicknessCm, double DepthCm)
	{
		return WallThicknessCm * DepthCm * DepthCm / 6.0;
	}

	/**
	 * ONE STRUCTURE IN THE FAMILY, AS DATA — AND IT IS PRODUCTION'S OWN.
	 *
	 * SCALE MULTIPLIES EVERY LENGTH AND NOTHING ELSE — brick, joint, step and origin — which is
	 * precisely the covariance test G asserts. Density is NOT a length and is left alone, so a
	 * structure at scale k has k^3 the mass, k^4 the moment about its root and k^3 the section:
	 * Galileo's square-cube law, and stress therefore grows LINEARLY with k.
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

	/**
	 * Case D: the same reach with three cells of masonry OPPOSITE.
	 *
	 * The origin moves left by exactly the three cells the base gains, so D's root joint sits at
	 * the same absolute X as C's and the two structures differ in the counterweight and in
	 * nothing else.
	 */
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

	/** A brick plus a joint: one coordinating cell along the wall. */
	inline double CorbelCellPitchOf(const FCorbelSpec& Spec)
	{
		return CorbelBrickLengthOf(Spec) + CorbelJointOf(Spec);
	}

	/** A brick plus a joint: one course up. */
	inline double CorbelCoursePitchOf(const FCorbelSpec& Spec)
	{
		return CorbelBrickHeightOf(Spec) + CorbelJointOf(Spec);
	}

	/** Centre height of a course, cm: half a brick up, then one course pitch per course. */
	inline double CorbelCourseZCm(const FCorbelSpec& Spec, int32 Course)
	{
		return CorbelBrickHeightOf(Spec) / 2.0 + Course * CorbelCoursePitchOf(Spec);
	}

	/** What one brick of this spec weighs, in Unreal force units. */
	inline double CorbelBrickWeightUu(const FCorbelSpec& Spec)
	{
		const double MassKg = CorbelBrickDensityGramsPerCubicCm
			* CorbelBrickLengthOf(Spec) * CorbelBrickWidthOf(Spec) * CorbelBrickHeightOf(Spec)
			/ 1000.0;

		return MassKg * CorbelGravityCmPerSecondSquared;
	}

	/** One built structure, and everything about it a test needs to name. */
	struct FCorbelStructure
	{
		FStructure Structure;

		/** One box per piece handle, parallel to the structure's piece array. */
		TArray<FPieceBox> Boxes;

		/** Piece handles by course, each sorted by ascending X, so `.Last()` is the outer face. */
		TArray<TArray<int32>> CoursePieces;

		/** Every piece standing ON the base — the filled stepped arm, base excluded. */
		TArray<int32> ArmPieces;

		/** The outermost brick of each arm course, bottom first: the stepping front itself. */
		TArray<int32> OuterFacePieces;

		/**
		 * THE ROOT JOINT: the bed joint under the arm's lowest outermost brick.
		 *
		 * This is the one place a corbel on an IMMOVABLE base can fail. A rigid body cannot
		 * rotate about a fixed base without separating from it, and separation on a bed plane IS
		 * that joint opening in tension — COMPOSITE_DEPTH_DESIGN.md's overturning section works
		 * that reduction through and it is why every reading below is taken here.
		 */
		int32 RootJoint = INDEX_NONE;

		/** The arm's lowest outermost brick, which stands on the root joint. */
		int32 RootPiece = INDEX_NONE;

		/** The base's top-course outermost brick, which the root joint stands on. */
		int32 RootSeatPiece = INDEX_NONE;

		/** The outermost brick of the TOP arm course: the mass hanging furthest out over nothing. */
		int32 TipPiece = INDEX_NONE;

		/** How far the tip's outer face reaches past the base's, cm — the total projection. */
		double ProjectionCm = 0.0;
	};

	/**
	 * LAY ONE OF THESE STRUCTURES, THROUGH PRODUCTION, AND NAME THE PARTS A TEST TALKS ABOUT.
	 *
	 * `DestructionCorbel::Build` places every brick and discovers every joint. What is added here
	 * is only INDEXING: which handles belong to which course, which of them are the arm, which
	 * joint is the root, and how far the tip reaches. None of that is a second opinion about the
	 * geometry — every value below is READ BACK OUT of the layout production handed over.
	 *
	 * THE COURSE INDEX IS READ FROM A BRICK'S OWN HEIGHT, which is the one thing here that could
	 * be a second construction, so it is a division by the course pitch and a round rather than a
	 * re-derivation of where a course goes. Within a course, handles arrive in ascending X — the
	 * base is laid left to right and the arm's fill is laid inboard-most first — so `.Last()` is
	 * the outer face without a sort.
	 *
	 * THE ROOT JOINT is the bed joint under the arm's lowest outermost brick: the one place a
	 * corbel on an IMMOVABLE base can fail, because a rigid body cannot rotate about a fixed base
	 * without separating from it.
	 *
	 * @return false if the spec could not describe a structure, writing nothing worth reading.
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
	 * WHAT A BED JOINT UNDER A CORBEL READS — the ORACLE, and what it is and is not for.
	 *
	 * Written from published beam theory rather than from the solver: a rectangle of area A and
	 * in-plane half-extents (h_u, h_v) under a normal force and a moment, plus the deep beam of
	 * section t*D^2/6 standing over it. It exists so that a printed number can be explained, so
	 * that the GOVERNING AXIS can be named rather than assumed, and so that a row which fails
	 * can say which of the three it failed on.
	 *
	 * IT IS NOT WHAT THE TESTS IN THIS FAMILY ASSERT ON, AND THAT IS DELIBERATE. It is built to
	 * the same shape as ComputeUtilisation's own arithmetic — the `min` against the composite
	 * section, both edges moving together — so agreeing with production proves little. What the
	 * tests assert is ORDERINGS and RATIOS between structures: a crossover that must move, a
	 * reading that must scale linearly, an ordering that must follow the profile numbers. Those
	 * survive a change of lambda, a change of section rule and a change of profile, and no
	 * oracle mirroring production can satisfy them by accident.
	 */
	struct FCorbelJointReading
	{
		/** Averaged normal stress, MPa, signed and positive in tension. */
		double NormalStressMPa = 0.0;

		/** Worst-corner bending stress on the joint's OWN patch, MPa. */
		double PatchBendingStressMPa = 0.0;

		/** The same moment read against the deep beam standing over the joint, MPa. */
		double CompositeBendingStressMPa = 0.0;

		/** Whether the composite section is the one that governs. */
		bool bCompositeGoverns = false;

		double TensionUtilisation = 0.0;
		double CompressionUtilisation = 0.0;
		double ShearUtilisation = 0.0;

		/** The worst of the three, which is what ComputeUtilisation returns. */
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
		 * A bed joint separates on Z, so its in-plane frame is X and Y: a moment about X is
		 * resisted by the depth along Y and vice versa. The pair is NOT interchangeable, and a
		 * corbel stepping along X is bent about Y, so it is the second of the two that decides
		 * everything here. Both are written out so that a fixture where the other one mattered
		 * would be visible rather than silently mis-sectioned.
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
