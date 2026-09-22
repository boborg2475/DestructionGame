// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"
#include "Tests/ArchingWallTestSupport.h"
#include "Tests/StaircaseWallTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

// Named namespace: unity builds merge anonymous namespaces across files.
namespace StructureCoverTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace StructureArchingTestSupport;

	/** Ten cells, 225 cm. The span is fixed so rows differ only in `d_e`. */
	constexpr int32 CellCount = 10;
	constexpr double ClearSpanCm = CellCount * BrickPitchCm;

	/**
	 * First cut index, by course parity: odd courses are offset half a cell, so index 9 (x = 213.75)
	 * or 10 (x = 225) starts a near-centred cut with jambs on both sides.
	 */
	constexpr int32 FirstCutIndexForCourse(int32 Course)
	{
		return Course % 2 == 0 ? 10 : 9;
	}

	constexpr double FirstCutBrickXCm(int32 Course)
	{
		return ArchWallBrickXCm(Course, FirstCutIndexForCourse(Course));
	}

	constexpr double LastCutBrickXCm(int32 Course)
	{
		return FirstCutBrickXCm(Course) + (CellCount - 1) * BrickPitchCm;
	}

	/**
	 * The two springings: the half-seated bricks of the course above, half a cell outboard of the
	 * cut. Their centres are 9 * 22.5 + 22.5 = 225 cm apart, exactly `L` (asserted below).
	 */
	constexpr double LeftSpringingXCm(int32 CutCourse)
	{
		return FirstCutBrickXCm(CutCourse) - BondOffsetCm;
	}

	constexpr double RightSpringingXCm(int32 CutCourse)
	{
		return LastCutBrickXCm(CutCourse) + BondOffsetCm;
	}

	/** Each springing's one seat: the uncut brick just outboard of the cut. */
	constexpr double LeftSeatXCm(int32 CutCourse)
	{
		return FirstCutBrickXCm(CutCourse) - BrickPitchCm;
	}

	constexpr double RightSeatXCm(int32 CutCourse)
	{
		return LastCutBrickXCm(CutCourse) + BrickPitchCm;
	}
}

/**
 * An arch needs masonry over it: the arching depth is capped by the cover above the span
 * (ARCHING_DESIGN.md slice 4).
 *
 *     d_e = min( cover above the span , 0.866 * L )       arching depth
 *     r   = d_e / 3                                       thrust line rise, kern-limited
 *     H   = W * L / (8r)      V = W / 2                   per abutment
 *
 * so `H/V = 3L / (4 d_e)` and W cancels. Uncapped (slice 3) the ratio is 0.866051 at any cover; with
 * one course over it is 22.5. `d_e` comes from a bounded upward walk over bed joints, not a spatial
 * query.
 *
 * Every row cuts the same ten cells of the 30x40 flush wall at a different course, so only `d_e`
 * varies. Asserted:
 *
 *   - H/V = 3L/(4 d_e) at 2% on every row: 22.5, 11.25, 5.625, 2.25 for the cover-governed rows.
 *   - The two angle-governed rows (202.5 and 285 cm, both past 194.85) read 0.866051, proving cover
 *     is a `min`; `d_e = cover` would give 0.833 and 0.592. The 285 cm row is StructureThrustTest's.
 *   - Cover counts whole course pitches including the spanning course; the alternatives are printed.
 *   - The abutments are 225 cm apart, exactly L (seat to seat would be 236.25).
 *   - Shear is the governing axis, since the compression axis reads a plausible 2|sigma_n|/f_c.
 *
 * The outcome arm is retired since the 2026-08-13 mean re-anchor: every springing now affords its
 * thrust. bMustComeDown and the attribution check are kept for the owed 20-cell replacement
 * (CURRENT_STATE). Never a displacement assertion. No ticking world. One-course cuts have no reveal
 * (that is slice 5).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureCoverTest,
	"DestructionGame.Core.Structure.AnArchNeedsMasonryOverIt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureCoverTest::RunTest(const FString& Parameters)
{
	using namespace StructureArchingTestSupport;
	using namespace StructureCoverTestSupport;
	using namespace StaircaseWallTestSupport;

	// Pin the profile values the expected numbers were derived from.
	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against the mean f_v0 = 0.9 MPa (re-anchor 2026-08-13), the profile carries %g"),
			GeneralPurposeMortar.ShearCohesionMPa),
		GeneralPurposeMortar.ShearCohesionMPa == 0.9);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against mean friction 0.75, the profile carries %g"),
			GeneralPurposeMortar.FrictionCoefficient),
		GeneralPurposeMortar.FrictionCoefficient == 0.75);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against the mean-basis 2.0 MPa shear ceiling (0.1 x f_b), the profile carries %g"),
			GeneralPurposeMortar.MaxShearStrengthMPa),
		GeneralPurposeMortar.MaxShearStrengthMPa == 2.0);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against compressive 10 MPa, the profile carries %g"),
			GeneralPurposeMortar.CompressiveStrengthMPa),
		GeneralPurposeMortar.CompressiveStrengthMPa == 10.0);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against clay brick at 1.9 g/cm3, the profile carries %g"),
			ClayBrick.DensityGramsPerCubicCm),
		ClayBrick.DensityGramsPerCubicCm == 1.9);

	AddInfo(FString::Printf(
		TEXT("FIXTURE: %d x %d flush wall, one course of %d cells cut out of it, so L = %g cm and ")
		TEXT("the angle wants an arching depth of %g cm"),
		ScenarioWallBricksPerCourse, ScenarioWallCourses, CellCount, ClearSpanCm,
		ArchingDepthPerSpan * ClearSpanCm));

	/** One depth of cover. Ordered shallowest (worst) first. */
	struct FCoverCase
	{
		const TCHAR* Description;

		/** Which course of the 40 is cut. The course above it spans. */
		int32 CutCourse;

		/** ARCHING_DESIGN's springing shear figure, or 0 if none; a factor-of-two cross-check only. */
		double DesignUtilisation;

		/** Whether the row also asserts collapse; only valid where the re-seat head joint is under capacity. */
		bool bMustComeDown;
	};

	/*
	 * Since the 2026-08-13 mean re-anchor (capacity 0.9 + 0.75 sigma) every row affords its thrust:
	 * even H/V = 22.5 costs 0.315 MPa against 0.91. The H/V geometry still pins hard. The owed
	 * replacement (CURRENT_STATE) is a 20-cell cut at course 38: H/V = 45, ~1.26 MPa against ~0.92.
	 * Design cross-checks were re-derived through each row's seat stress (0.617 and 0.274).
	 */
	const TArray<FCoverCase> Cases = {
		// One course, 7.5 cm (ARCHING_DESIGN's worked case): H/V = 22.5, 25.98x the uncapped value.
		{ TEXT("ONE course of cover"), 38, 0.617, false },

		// Two courses, 15 cm: H/V = 11.25.
		{ TEXT("TWO courses of cover"), 37, 0.0, false },

		// Four courses, 30 cm: H/V = 5.625.
		{ TEXT("FOUR courses of cover"), 35, 0.0, false },

		// Ten courses, 75 cm: H/V = 2.25. The crossover is 194.85 cm, 25.98 courses.
		{ TEXT("TEN courses of cover"), 29, 0.0, false },

		/*
		 * 27 courses, 202.5 cm: just past the crossover, so 0.866051 (`d_e = cover` would give
		 * 0.833). Not 26 courses, which clears the crossover by only 0.15 cm.
		 */
		{ TEXT("TWENTY-SEVEN courses of cover, past the crossover"), 12, 0.0, false },

		// 38 courses, 285 cm: StructureThrustTest's fixture; 0.866051 (`d_e = cover` would give 0.592).
		{ TEXT("THIRTY-EIGHT courses of cover, the deepest this wall has"), 1, 0.274, false },
	};

	/** Each row's measured H/V, for comparing the two angle-governed rows. */
	TArray<double> MeasuredThrustPerReaction;
	MeasuredThrustPerReaction.Init(0.0, Cases.Num());

	for (int32 Row = 0; Row < Cases.Num(); ++Row)
	{
		const FCoverCase& Case = Cases[Row];

		const double CoverCm = CoverAboveCutCm(ScenarioWallCourses, Case.CutCourse);
		const double ArchingDepthCm = FMath::Min(CoverCm, ArchingDepthPerSpan * ClearSpanCm);
		const double ExpectedThrustPerReaction = ThrustPerReaction(ClearSpanCm, CoverCm);

		// Two alternative ways of counting the cover, printed for comparison.
		const double CoursesOfCover = CoverCm / CoursePitchCm;
		const double BrickOnlyCoverCm = CoursesOfCover * BrickHeightCm;
		const double ExcludingTheSpanCm = CoverCm - CoursePitchCm;

		AddInfo(FString::Printf(
			TEXT("%s: cut course %d, so %g courses stand over the opening — cover %g cm, d_e %g, ")
			TEXT("r %g, and H/V must be %s. (Counting brick height rather than course pitch would ")
			TEXT("give %s; excluding the spanning course would give %s; an uncapped d_e gives %s.)"),
			Case.Description, Case.CutCourse, CoursesOfCover, CoverCm, ArchingDepthCm,
			ArchingDepthCm / 3.0, *Bits(ExpectedThrustPerReaction),
			*Bits(ThrustPerReaction(ClearSpanCm, BrickOnlyCoverCm)),
			*Bits(ExcludingTheSpanCm > 0.0
				? ThrustPerReaction(ClearSpanCm, ExcludingTheSpanCm)
				: TNumericLimits<double>::Max()),
			*Bits(3.0 / (4.0 * ArchingDepthPerSpan))));

		FBrickLayout Cut;

		if (!RunningBond(ScenarioWallSpec(), Cut) || Cut.Boxes.Num() != ScenarioWallPieceCount)
		{
			AddError(FString::Printf(
				TEXT("%s: FIXTURE: a flush %d x %d wall should lay as %d pieces, got %d"),
				Case.Description, ScenarioWallBricksPerCourse, ScenarioWallCourses,
				ScenarioWallPieceCount, Cut.Boxes.Num()));

			continue;
		}

		bool bCutLaid = true;

		for (int32 Cell = 0; Cell < CellCount; ++Cell)
		{
			const double BrickXCm = FirstCutBrickXCm(Case.CutCourse) + Cell * BrickPitchCm;
			const int32 Piece =
				StaircasePieceAt(Cut.Boxes, BrickXCm, ArchWallCourseZCm(Case.CutCourse));

			if (Piece == INDEX_NONE || !Cut.Structure.RemovePiece(Piece))
			{
				AddError(FString::Printf(
					TEXT("%s: FIXTURE: there should be a brick at x %g in course %d to delete"),
					Case.Description, BrickXCm, Case.CutCourse));

				bCutLaid = false;
				break;
			}
		}

		if (!bCutLaid)
		{
			continue;
		}

		Cut.Structure.SolveLoads();

		const int32 SpannedCourse = Case.CutCourse + 1;

		/** One end of the arch: the half-seated brick carrying the thrust. */
		struct FSpringingCase
		{
			const TCHAR* Side;

			double BrickXCm;
			double SeatXCm;

			/** Overhang direction into the opening: +1 is toward increasing X. */
			double EccentricSign;
		};

		const FSpringingCase Springings[2] = {
			{
				TEXT("LEFT springing"),
				LeftSpringingXCm(Case.CutCourse),
				LeftSeatXCm(Case.CutCourse),
				+1.0
			},
			{
				TEXT("RIGHT springing"),
				RightSpringingXCm(Case.CutCourse),
				RightSeatXCm(Case.CutCourse),
				-1.0
			},
		};

		// The abutment centres must be exactly L apart; slice 4 takes the span from them.
		const double AbutmentSeparationCm =
			Springings[1].BrickXCm - Springings[0].BrickXCm;

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the two abutment centres must be exactly L = %g cm apart, they ")
				TEXT("are %g (seat to seat would be %g)"),
				Case.Description, ClearSpanCm, AbutmentSeparationCm, ClearSpanCm + BondOffsetCm),
			FMath::IsNearlyEqual(AbutmentSeparationCm, ClearSpanCm, 1.0e-9));

		int32 SpringingJoints[2] = { INDEX_NONE, INDEX_NONE };
		int32 SpringingBricks[2] = { INDEX_NONE, INDEX_NONE };
		int32 ReseatHeadJoints[2] = { INDEX_NONE, INDEX_NONE };
		double HorizontalUu[2] = { 0.0, 0.0 };
		double VerticalUu[2] = { 0.0, 0.0 };
		double ThrustRatio[2] = { 0.0, 0.0 };
		bool bBothSpringingsRead = true;

		for (int32 End = 0; End < 2; ++End)
		{
			const FSpringingCase& Springing = Springings[End];

			const FString Where =
				FString::Printf(TEXT("%s, %s"), Case.Description, Springing.Side);

			const int32 Brick = StaircasePieceAt(
				Cut.Boxes, Springing.BrickXCm, ArchWallCourseZCm(SpannedCourse));
			const int32 Seat = StaircasePieceAt(
				Cut.Boxes, Springing.SeatXCm, ArchWallCourseZCm(Case.CutCourse));

			if (Brick == INDEX_NONE || Seat == INDEX_NONE)
			{
				AddError(FString::Printf(
					TEXT("%s: FIXTURE: no brick at x %g in course %d, or no seat at x %g in course %d"),
					*Where, Springing.BrickXCm, SpannedCourse, Springing.SeatXCm, Case.CutCourse));

				bBothSpringingsRead = false;
				continue;
			}

			SpringingBricks[End] = Brick;

			const int32 BedJoint = TheOneIntactSeatBeneath(Cut.Structure, Brick);

			if (BedJoint == INDEX_NONE
				|| BedJoint != JointBetweenPieces(Cut.Structure, Brick, Seat))
			{
				AddError(FString::Printf(
					TEXT("%s: FIXTURE: it must rest on EXACTLY ONE bed joint (it rests on %d) and ")
					TEXT("that joint must be the one to the brick at x %g"),
					*Where, IntactSeatsBeneath(Cut.Structure, Brick), Springing.SeatXCm));

				bBothSpringingsRead = false;
				continue;
			}

			SpringingJoints[End] = BedJoint;

			const FConnection& Bed = Cut.Structure.GetConnection(BedJoint);

			// The springing is a 10.25 x 10.25 half seat loaded 5.625 cm off its centroid.
			TestTrue(
				FString::Printf(
					TEXT("%s: FIXTURE: the surviving seat should be %g cm2 with half-extents ")
					TEXT("(%g, %g); MakeInterface emitted %g cm2 with (%g, %g)"),
					*Where, HalfSeatAreaSqCm, HalfSeatHalfExtentCm, HalfSeatHalfExtentCm,
					Bed.InterfaceAreaSqCm,
					Bed.InterfaceHalfExtentCm.X, Bed.InterfaceHalfExtentCm.Y),
				FMath::IsNearlyEqual(Bed.InterfaceAreaSqCm, HalfSeatAreaSqCm, 1.0e-9)
					&& FMath::IsNearlyEqual(Bed.InterfaceHalfExtentCm.X, HalfSeatHalfExtentCm, 1.0e-9)
					&& FMath::IsNearlyEqual(Bed.InterfaceHalfExtentCm.Y, HalfSeatHalfExtentCm, 1.0e-9));

			const double EccentricityCm = Cut.Boxes[Brick].CentreCm.X - Bed.InterfaceCentreCm.X;

			TestTrue(
				FString::Printf(
					TEXT("%s: FIXTURE: it should overhang its seat by %g cm INTO the opening, it ")
					TEXT("overhangs %g"),
					*Where, HalfSeatEccentricityCm, EccentricityCm),
				FMath::IsNearlyEqual(
					EccentricityCm, Springing.EccentricSign * HalfSeatEccentricityCm, 1.0e-9));

			// The neighbour over the opening must be seatless but Supported, or there is no arch.
			const int32 Neighbour = StaircasePieceAt(
				Cut.Boxes,
				Springing.BrickXCm + Springing.EccentricSign * BrickPitchCm,
				ArchWallCourseZCm(SpannedCourse));

			TestTrue(
				FString::Printf(
					TEXT("%s: FIXTURE: the piece over the opening beside it must have NO seat (it ")
					TEXT("has %d) and must be Supported (it reads %d, Supported is %d) or there is ")
					TEXT("no arch to thrust"),
					*Where,
					Neighbour == INDEX_NONE ? -1 : IntactSeatsBeneath(Cut.Structure, Neighbour),
					Neighbour == INDEX_NONE
						? -1
						: static_cast<int32>(Cut.Structure.GetPieceSupport(Neighbour)),
					static_cast<int32>(EPieceSupport::Supported)),
				Neighbour != INDEX_NONE
					&& IntactSeatsBeneath(Cut.Structure, Neighbour) == 0
					&& Cut.Structure.GetPieceSupport(Neighbour) == EPieceSupport::Supported);

			if (Neighbour != INDEX_NONE)
			{
				ReseatHeadJoints[End] = JointBetweenPieces(Cut.Structure, Brick, Neighbour);
			}

			const FVector ForceUu = Cut.Structure.GetConnectionForce(BedJoint);
			const FVector MomentUuCm = Cut.Structure.GetConnectionMoment(BedJoint);

			const double Utilisation = Cut.Structure.GetConnectionUtilisation(BedJoint);

			const FBedJointReading Published = ReadBedJoint(
				ForceUu, MomentUuCm, Bed.InterfaceHalfExtentCm, Bed.InterfaceAreaSqCm,
				GeneralPurposeMortar);

			HorizontalUu[End] = ForceUu.X;
			VerticalUu[End] = FMath::Abs(ForceUu.Z);

			ThrustRatio[End] = VerticalUu[End] > 0.0
				? FMath::Abs(ForceUu.X) / VerticalUu[End]
				: 0.0;

			AddInfo(FString::Printf(
				TEXT("%s: joint %d carries (%s, %s, %s) uu — V = %.4f brick weights, H/V = %s — ")
				TEXT("sigma_n %s MPa; tension %s, compression %s, SHEAR %s; the joint reads %s"),
				*Where, BedJoint, *Bits(ForceUu.X), *Bits(ForceUu.Y), *Bits(ForceUu.Z),
				VerticalUu[End] / BrickWeightUu, *Bits(ThrustRatio[End]),
				*Bits(Published.NormalStressMPa), *Bits(Published.TensionUtilisation),
				*Bits(Published.CompressionUtilisation), *Bits(Published.ShearUtilisation),
				*Bits(Utilisation)));

			// No compression, no thrust line.
			TestTrue(
				FString::Printf(
					TEXT("%s: FIXTURE: the seat must be in COMPRESSION, sigma_n is %s MPa"),
					*Where, *Bits(Published.NormalStressMPa)),
				Published.NormalStressMPa < 0.0);

			// The Mohr-Coulomb ceiling must not be what decides.
			const double UntruncatedCapacityMPa = GeneralPurposeMortar.ShearCohesionMPa
				+ GeneralPurposeMortar.FrictionCoefficient * FMath::Abs(Published.NormalStressMPa);

			TestTrue(
				FString::Printf(
					TEXT("%s: FIXTURE: the shear ceiling must NOT be reached — Mohr-Coulomb gives ")
					TEXT("%s MPa against the profile's %g MPa cap"),
					*Where, *Bits(UntruncatedCapacityMPa),
					GeneralPurposeMortar.MaxShearStrengthMPa),
				UntruncatedCapacityMPa < GeneralPurposeMortar.MaxShearStrengthMPa);

			/*
			 * The mechanism: H/V = 3L/(4 d_e), independent of wall weight. 2% covers 0.866 vs
			 * sqrt(3)/2 and column differences, but not the alternatives (brick-height cover 15% out,
			 * seat-to-seat span 5%, uncapped depth 26x).
			 */
			TestTrue(
				FString::Printf(
					TEXT("%s: the arching depth is capped by the COVER — d_e = min(%g, %g) = %g, so ")
					TEXT("H/V must be 3L/(4 d_e) = %s; the joint reports H = %s uu against V = %s ")
					TEXT("uu, a ratio of %s"),
					*Where, CoverCm, ArchingDepthPerSpan * ClearSpanCm, ArchingDepthCm,
					*Bits(ExpectedThrustPerReaction), *Bits(ForceUu.X), *Bits(VerticalUu[End]),
					*Bits(ThrustRatio[End])),
				FMath::Abs(ThrustRatio[End] - ExpectedThrustPerReaction)
					<= 0.02 * ExpectedThrustPerReaction);

			// Shear must govern, or a higher compression reading would be measured instead.
			TestTrue(
				FString::Printf(
					TEXT("%s: SHEAR must be the governing axis — shear %s against compression %s ")
					TEXT("and tension %s — or this row is measuring the wrong thing"),
					*Where, *Bits(Published.ShearUtilisation),
					*Bits(Published.CompressionUtilisation), *Bits(Published.TensionUtilisation)),
				Published.ShearUtilisation > Published.CompressionUtilisation
					&& Published.ShearUtilisation > Published.TensionUtilisation);

			// The joint's reading matches beam theory and Mohr-Coulomb; no private thrust rule.
			TestTrue(
				FString::Printf(
					TEXT("%s: it must read what beam theory and Mohr-Coulomb say, %s, and it reads %s"),
					*Where, *Bits(Published.Worst), *Bits(Utilisation)),
				FMath::Abs(Utilisation - Published.Worst)
					<= 1.0e-12 * FMath::Max(Published.Worst, 1.0e-12));

			// At mean strengths every row is under capacity (see the re-anchor note above).
			TestTrue(
				FString::Printf(
					TEXT("%s: at mean strengths this springing must afford its thrust — UNDER ")
					TEXT("capacity in shear, it reads %s"),
					*Where, *Bits(Published.ShearUtilisation)),
				Published.ShearUtilisation < 1.0);

			/*
			 * Factor-of-two check against ARCHING_DESIGN's figure. Loose, but it still catches a
			 * missing 100x, a missing /3 in the rise, or a thrust of W instead of W*L/(8r).
			 */
			if (Case.DesignUtilisation > 0.0)
			{
				TestTrue(
					FString::Printf(
						TEXT("%s: ARCHING_DESIGN predicts %g for this springing and it reads %s — a ")
						TEXT("cross-check, not a target, so it allows a factor of two"),
						*Where, Case.DesignUtilisation, *Bits(Published.ShearUtilisation)),
					Published.ShearUtilisation >= 0.5 * Case.DesignUtilisation
						&& Published.ShearUtilisation <= 2.0 * Case.DesignUtilisation);
			}
		}

		if (!bBothSpringingsRead)
		{
			continue;
		}

		MeasuredThrustPerReaction[Row] = ThrustRatio[0];

		/*
		 * Trap 2: sigma H = 0 across the arch. Both ends have identical cover, so this catches an
		 * asymmetric application of d_e, not an asymmetric measurement.
		 */
		const double HorizontalSumUu = HorizontalUu[0] + HorizontalUu[1];
		const double LargerThrustUu =
			FMath::Max(FMath::Abs(HorizontalUu[0]), FMath::Abs(HorizontalUu[1]));

		TestTrue(
			FString::Printf(
				TEXT("%s: the two ends of the arch must push in OPPOSITE directions — they carry ")
				TEXT("%s and %s uu"),
				Case.Description, *Bits(HorizontalUu[0]), *Bits(HorizontalUu[1])),
			HorizontalUu[0] * HorizontalUu[1] < 0.0);

		TestTrue(
			FString::Printf(
				TEXT("%s: and SIGMA H MUST BE ZERO across the arch — %s + %s = %s uu, against a ")
				TEXT("thrust of %s"),
				Case.Description, *Bits(HorizontalUu[0]), *Bits(HorizontalUu[1]),
				*Bits(HorizontalSumUu), *Bits(LargerThrustUu)),
			FMath::Abs(HorizontalSumUu) <= 1.0e-9 * FMath::Max(LargerThrustUu, 1.0));

		/*
		 * Read the re-seat head joints before any outcome claim. Slice 2 routes the spanned load
		 * through them in pure shear against 0.2 MPa cohesion: far over capacity under deep cover,
		 * well under with one course, which is what lets the shallow row attribute a fall to thrust.
		 */
		double WorstHeadJointUtilisation = 0.0;

		for (int32 End = 0; End < 2; ++End)
		{
			if (ReseatHeadJoints[End] == INDEX_NONE)
			{
				continue;
			}

			const int32 Index = ReseatHeadJoints[End];
			const double HeadUtilisation = Cut.Structure.GetConnectionUtilisation(Index);

			WorstHeadJointUtilisation = FMath::Max(WorstHeadJointUtilisation, HeadUtilisation);

			AddInfo(FString::Printf(
				TEXT("%s, %s: the re-seat head joint %d beside it carries (%s, %s, %s) uu, ")
				TEXT("%.4f brick weights, and reads %s"),
				Case.Description, Springings[End].Side, Index,
				*Bits(Cut.Structure.GetConnectionForce(Index).X),
				*Bits(Cut.Structure.GetConnectionForce(Index).Y),
				*Bits(Cut.Structure.GetConnectionForce(Index).Z),
				FMath::Abs(Cut.Structure.GetConnectionForce(Index).Z) / BrickWeightUu,
				*Bits(HeadUtilisation)));
		}

		if (!Case.bMustComeDown)
		{
			continue;
		}

		// Outcome arm. Attribution first: the head joints must not be what decided.
		TestTrue(
			FString::Printf(
				TEXT("%s: ATTRIBUTION: the re-seat head joints must be well UNDER capacity — the ")
				TEXT("worst reads %s — or a collapse here is slice 2's and not the thrust's"),
				Case.Description, *Bits(WorstHeadJointUtilisation)),
			WorstHeadJointUtilisation < 0.5);

		/*
		 * Outcome: count unrouted pieces, and require both springings among the load failures.
		 * Counted by break pass, not HasGiven: joints of removed pieces have given with pass INDEX_NONE.
		 */
		const int32 BreakingPasses = Cut.Structure.SolveAndBreak();

		int32 JointsBrokenByLoad = 0;

		for (int32 Joint = 0; Joint < Cut.Structure.NumConnections(); ++Joint)
		{
			if (Cut.Structure.GetConnection(Joint).HasGiven()
				&& Cut.Structure.GetBreakPass(Joint) != INDEX_NONE)
			{
				++JointsBrokenByLoad;
			}
		}

		int32 Unrouted = 0;

		for (int32 Piece = 0; Piece < Cut.Structure.NumPieces(); ++Piece)
		{
			if (!Cut.Structure.IsPieceRemoved(Piece) && !Cut.Structure.IsPieceSupported(Piece))
			{
				++Unrouted;
			}
		}

		AddInfo(FString::Printf(
			TEXT("%s: the cascade ran %d passes, %d of %d joints failed under load, and %d of the ")
			TEXT("%d pieces it did not delete are left with no path to the ground"),
			Case.Description, BreakingPasses, JointsBrokenByLoad, Cut.Structure.NumConnections(),
			Unrouted, Cut.Structure.NumPieces() - CellCount));

		// A floor: at least the seatless bricks plus both springings.
		TestTrue(
			FString::Printf(
				TEXT("%s: a ten-cell hole under one course of brickwork must COME DOWN — at least ")
				TEXT("the %d bricks over it should lose their path to the ground; %d pieces did"),
				Case.Description, CellCount + 1, Unrouted),
			Unrouted >= CellCount + 1);

		for (int32 End = 0; End < 2; ++End)
		{
			TestTrue(
				FString::Printf(
					TEXT("%s, %s: and the springing itself must be one of the joints that failed ")
					TEXT("under load; joint %d broke in pass %d"),
					Case.Description, Springings[End].Side, SpringingJoints[End],
					Cut.Structure.GetBreakPass(SpringingJoints[End])),
				Cut.Structure.GetBreakPass(SpringingJoints[End]) != INDEX_NONE);
		}
	}

	/*
	 * The cap is a `min`: the 202.5 and 285 cm rows are both past the angle and must match
	 * (`d_e = cover` would put them 41% apart). 1% because the openings sit at different heights.
	 */
	{
		const double Shallower = MeasuredThrustPerReaction[Cases.Num() - 2];
		const double Deeper = MeasuredThrustPerReaction[Cases.Num() - 1];

		TestTrue(
			FString::Printf(
				TEXT("THE CAP IS A min: %g cm and %g cm of cover are both past the 0.866 angle, so ")
				TEXT("the same opening must thrust identically under both — they read %s and %s. ")
				TEXT("(An uncapped d_e = cover would put them %g%% apart.)"),
				CoverAboveCutCm(ScenarioWallCourses, Cases[Cases.Num() - 2].CutCourse),
				CoverAboveCutCm(ScenarioWallCourses, Cases[Cases.Num() - 1].CutCourse),
				*Bits(Shallower), *Bits(Deeper),
				100.0 * (CoverAboveCutCm(ScenarioWallCourses, Cases[Cases.Num() - 1].CutCourse)
					/ CoverAboveCutCm(ScenarioWallCourses, Cases[Cases.Num() - 2].CutCourse) - 1.0)),
			Deeper > 0.0 && FMath::Abs(Shallower - Deeper) <= 0.01 * Deeper);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
