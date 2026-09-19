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
 * width into the SAME 7 x 30 flush wall so its answer can be compared against the slice before
 * it — the intact wall's 0.0036748258197270385 is the anchor both are measured against. A second
 * copy of a wall definition is two fixtures that drift.
 *
 * WORLD-FREE ON PURPOSE. Everything here is boxes and doubles, so the fast suite pays nothing.
 *
 * NAMED NAMESPACE, not anonymous, and named for what it holds. An anonymous namespace is private
 * to a TRANSLATION UNIT rather than a file, and a unity build merges many files into one — at
 * which point two anonymous namespaces are the SAME namespace and identically-named helpers in
 * files that never refer to each other are a hard compile error. Free functions are `inline`
 * because more than one translation unit includes this and a non-unity build would fail at link.
 */
namespace StructureArchingTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * Everything below is spelled out here rather than imported: a test that reaches for the
	 * same named constant as production agrees with it instead of catching it wrong. So the
	 * brick, gravity, the SI conversion, the section modulus and the beam-theory reading of a
	 * bed joint are all written from first principles; strengths are asserted against the
	 * profile rather than copied out of it.
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
	 * Recomputing the stresses here from FStructure's own reported force — one number the
	 * solver publishes and this file never derives — is what makes "the arch did not fire" a
	 * checkable claim on every negative row rather than a literal re-measured each time a
	 * fixture moves.
	 *
	 * THE MOMENT PASSED IN IS THE CALLER'S CHOICE AND THAT MATTERS. Handing over the moment
	 * the SOLVER published answers "is this reading self-consistent with its own published
	 * moment", the right question for a negative row but the WRONG one for "would this joint
	 * have cantilevered": the cap changes the published moment, so an oracle built on it
	 * necessarily agrees with the cap. A row wanting the un-arched answer builds the moment
	 * from the fixture's own lever arm instead.
	 *
	 * A bed joint separates on Z, so its in-plane frame is X and Y: a moment about X is
	 * resisted by the depth along Y and vice versa. The pair is NOT interchangeable — on a
	 * brick's end face the two moduli differ by a factor of 2.5 — so both are written out.
	 *
	 * FRICTION IS BOUGHT BY THE MEAN COMPRESSIVE STRESS, NOT THE PEAK (EN 1996-1-1 §3.6.2,
	 * and what production does). Every bed joint here carries a purely vertical force until
	 * slice 3 lands the horizontal thrust, so shear is exactly zero and never decides
	 * anything — written out so a fixture where it DID would be visible.
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
		 * NO MOMENT MEANS NO BENDING TERM, tested on the moment rather than the modulus. A
		 * joint whose rectangle nobody measured has a modulus of zero on both axes and is
		 * perfectly healthy — the area alone answers a centred load exactly — so dividing
		 * anyway would give 0/0 and NaN would propagate. A moment that IS there against a
		 * section that is not instead comes out infinite, the fail-closed answer.
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
	 * Shear is untouched and zero here. So an arched bed joint reads twice its own mean
	 * compressive stress against the compressive strength — DELIBERATELY NOT ZERO. Setting the
	 * moment to zero instead of capping it would read |sigma_n|, wrong by a factor of two.
	 */
	inline double ArchedUtilisation(
		const FBedJointReading& Reading, const FConnectionStrength& Strength)
	{
		return 2.0 * FMath::Abs(Reading.NormalStressMPa) / Strength.CompressiveStrengthMPa;
	}

	/**
	 * THE WALL THE PLAYER IS LOOKING AT, SHRUNK TO WHAT A WORLD-FREE TEST NEEDS, AT ANY HEIGHT.
	 *
	 * Thirty courses (ArchWallSpec's default) is what makes the defect bite: a brick in course
	 * j of an N-course wall carries about N - j brick weights, a half seat reaches capacity at
	 * 17.18 of them, so any deletion with 18+ courses above it takes the joint above it out.
	 * Seven bricks per course is the narrowest wall that leaves a genuinely INTERIOR cell, so
	 * nothing here is a free-end effect.
	 *
	 * FLUSH RATHER THAN RAGGED, and it is not a style choice. A ragged wall's end brick already
	 * rests on ONE brick below it and is a half-seated cantilever before anyone deletes
	 * anything; a flush end fills that half cell with a half bat, so every brick has two seats
	 * and an intact wall's eccentricity is exactly zero — the only baseline against which "one
	 * deletion did this" means anything. ADestructionGameGameMode lays a flush wall for its
	 * scenario.
	 *
	 * THE HEIGHT PARAMETER exists for COMPOSITE_DEPTH_DESIGN.md's slice 2 row — a free-end
	 * deletion in a wall much taller than the cut, since a free-end joint's credited section is
	 * measured by walking UP, so wall height feeds the answer and thirty courses could never
	 * show that. Seven bricks per course at every height, deliberately: the claim is about
	 * depth, and widening the wall alongside it would move two things at once.
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
	 * WHAT THE FREE-END SEAT READS ON THE THIRTY-COURSE WALL — lives here because two files
	 * measure it and must not be allowed to disagree.
	 *
	 * Delete the outermost full brick of course 0 and the full brick above it keeps exactly one
	 * seat, overhanging it 5.625 cm toward the free end. `Core.Structure.ACorbelResistsWithItsWholeDepth`
	 * PART 2 reads that joint on `ArchWallSpec()`, and `Core.Structure.AFreeEndDeletionInATallWall`
	 * reads the SAME joint on `ArchWallSpecOfHeight(30)` — the same call, one measurement made
	 * twice, and if they ever print different numbers something has quietly stopped being the
	 * same fixture. Both assert against this, EXACTLY.
	 *
	 * AN AGREEMENT ANCHOR, NOT A DERIVATION. Neither file derives anything from it: PART 2
	 * derives its reading from the identity `util = K*F^2/M` and the free-end ladder derives it
	 * from the section the walk credited; this literal only says the two agree. IT MOVES WITH
	 * lambda, which COMPOSITE_DEPTH_DESIGN.md is explicit is a RULING, formally provisional
	 * until slice 3 — so when lambda is ruled on, this number changes in one place and both
	 * identities keep holding on their own terms.
	 *
	 * 0.0185878 * 43.2749^2 / 486.2129 at lambda = 3.464 on the mean basis (coefficient
	 * 0.130114883 / 7 since the 2026-08-14 re-anchor flip moved f_x1 0.10 -> 0.70), where
	 * 43.2749 brick weights and 486.2129 brick-weight-cm are what the joint carries under
	 * twenty-nine courses of column.
	 *
	 * RE-MEASURE AT THE FLIP: written as the old measured bits divided by 7.0, within an ulp of
	 * what production reads at f_x1 = 0.70 but not provably bit-identical (production divides
	 * the stress by 0.7 directly). Both consumers pin it with exact ==, so if either fails by
	 * the last bit when the profile flips, re-pin THIS constant to the measured value.
	 */
	constexpr double FreeEndThirtyCourseUtilisation = 0.50114032912317807 / 7.0;

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
	 * 56.25, 78.75, 101.25, 123.75, then half bat. The third has two full columns of wall
	 * either side of it, so neither brick left half-seated above it can borrow from a free end.
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

	/*
	 * THE SCENARIO WALL, AND THE GEOMETRY OF THE ARCH ITSELF. Slices 3 and 4 both need a wall
	 * wide enough for a twenty-cell opening and tall enough for the cover to govern the arching
	 * depth, which the 7 x 30 wall above is neither. This is that wall, kept beside the other
	 * one for the same reason this header exists: a second copy of a wall definition drifts.
	 */

	/**
	 * THE ARCHING DEPTH PER UNIT SPAN, WRITTEN HERE RATHER THAN IMPORTED.
	 *
	 * BS 5977-1 specifies the equilateral triangle of loading over an opening — 60 degree base
	 * angles, so a height of sqrt(3)/2 of the span. ARCHING_DESIGN.md uses that angle ONLY as a
	 * cap on how deep the arch may be, never to reduce the load, and records 0.866 as the one
	 * genuine modelling constant the work introduces.
	 *
	 * SPELLED AS THE THREE-DIGIT 0.866 rather than sqrt(3)/2 because that is what the design
	 * says, and the two differ by 3e-5 relative — inside every tolerance that uses it.
	 */
	constexpr double ArchingDepthPerSpan = 0.866;

	/**
	 * THE WALL THE PLAYER IS ACTUALLY LOOKING AT — 30 bricks by 40 courses, the game mode's own
	 * scenario wall, and the wall every figure in ARCHING_DESIGN's span table was worked against.
	 *
	 * NOT THE 7 x 30 WALL SLICES 1 AND 2 CUT, and it cannot be: a 20-cell opening needs at least
	 * 22 bricks in a course, and the thrust ratio depends on the COVER above the opening, which
	 * is what the 40 courses supply. Everything else — brick, joint, bond, flush end — is
	 * identical, so the two fixtures still share every derived quantity here.
	 *
	 * FLUSH RATHER THAN RAGGED, for the reason ArchWallSpec records: a ragged wall's end brick is
	 * already a half-seated cantilever before anyone has cut anything.
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

	/**
	 * Centre X of the Nth brick of a course, whichever way the bond ran that course.
	 *
	 * Running bond alternates, so a fixture that cuts course 1 and a fixture that cuts course 38
	 * index their bricks from different origins. Everything downstream is written in X rather
	 * than in indices precisely so that the two read the same.
	 */
	constexpr double ArchWallBrickXCm(int32 Course, int32 Index)
	{
		return Course % 2 == 0 ? ArchWallEvenBrickXCm(Index) : ArchWallOddBrickXCm(Index);
	}

	/**
	 * HOW MUCH MASONRY STANDS OVER A ONE-COURSE CUT AT `CutCourse`, cm.
	 *
	 * Counted as WHOLE COURSES AT ONE COURSE PITCH EACH, from the course that spans the opening
	 * up to the top of the wall — ARCHING_DESIGN's own 285cm figure: a 40-course wall cut at
	 * course 1 leaves courses 2-39, and 38 * 7.5 = 285. The SPANNING COURSE ITSELF COUNTS — it
	 * is the first ring of the arch — so the shallowest cover this wall can offer is one pitch,
	 * 7.5cm, not zero.
	 */
	constexpr double CoverAboveCutCm(int32 CoursesHigh, int32 CutCourse)
	{
		return (CoursesHigh - CutCourse - 1) * CoursePitchCm;
	}

	/**
	 * HOW HARD AN ARCH PUSHES SIDEWAYS, PER UNIT OF WHAT IT PUSHES DOWN WITH.
	 *
	 *     d_e = min( cover above the span , 0.866 * L )       arching depth
	 *     r   = d_e / 3                                       thrust line rise, kern-limited
	 *     H   = W * L / (8r)     V = W / 2                    per abutment
	 *
	 * Dividing the last two, H/V = L / (4r) = 3L / (4 d_e) — AND W CANCELS. That is why this is
	 * the quantity asserted rather than H itself: a statement about the thrust line's geometry
	 * alone, holding whatever the wall above the opening weighs.
	 *
	 * V IS THE ABUTMENT'S VERTICAL REACTION, exactly what the springing bed joint reports. The
	 * arch delivers its whole load W to two abutments, V each, and the springing brick's own
	 * column is part of that load because it leaves through the same patch. An implementation
	 * that took W as the re-seated GROUP's load only — excluding the two springings' own columns
	 * — reads about 0.78 where this expects 0.866 on a ten-cell case; if that one row is the
	 * only thing red, that is the difference.
	 */
	inline double ThrustPerReaction(double SpanCm, double CoverCm)
	{
		const double DepthCm = FMath::Min(CoverCm, ArchingDepthPerSpan * SpanCm);

		return 3.0 * SpanCm / (4.0 * DepthCm);
	}

	/** The end face of a brick: the whole of it, since two bricks in a course meet squarely. */
	constexpr double HeadJointAreaSqCm = BrickWidthCm * BrickHeightCm;

	/**
	 * WHAT A HEAD JOINT READS — the sibling of the bed-joint oracle above, derived the same way,
	 * from published beam theory rather than from the solver.
	 *
	 * A head joint separates on a HORIZONTAL axis, so gravity is entirely shear on it and the
	 * normal axis is whatever is pushing the two bricks together along the wall — today, nothing
	 * at all. Mohr-Coulomb then buys capacity from that compression: `c + mu * sigma_n`,
	 * truncated at the ceiling.
	 *
	 * NO BENDING TERM: a re-seated piece is statically indeterminate by slice 2's own rule, so
	 * its joints carry a moment of exactly zero. Asserted at the call site rather than assumed
	 * here, so a moment that appeared would fail rather than be silently dropped.
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

		/** The worst of the three, which is what ComputeUtilisation returns. */
		double Worst = 0.0;
	};

	inline FHeadJointReading ReadHeadJoint(
		const FVector& ForceUu,
		const FVector& UnitNormal,
		double AreaSqCm,
		const FConnectionStrength& Strength)
	{
		FHeadJointReading Out;

		/* ALONG THE NORMAL IS POSITIVE IN TENSION (ClassifyForce's convention): the force handed
		 * over acts on the piece the normal points toward, so +normal pulls the faces apart. */
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

	/** How many intact bed joints this piece is resting on RIGHT NOW. */
	inline int32 IntactSeatsBeneath(const FStructure& Structure, int32 Piece)
	{
		int32 Seats = 0;

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			/* A JOINT THAT HAS GIVEN IS NOT A SEAT. GetJointRole is pure geometry and keeps
			 * answering for a joint that has left the structure (deliberately, for a debugger),
			 * so the joints that went with the deleted bricks still report BedBeneath. */
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
