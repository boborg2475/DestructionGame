// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"
#include "Tests/ArchingWallTestSupport.h"
#include "Tests/StaircaseWallTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Named namespace, not anonymous: a unity build merges files into one translation unit, so two
 * anonymous namespaces would collide and identically-named helpers be a hard compile error.
 */
namespace StructureCoverTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace StructureArchingTestSupport;

	/**
	 * Ten cells, 225 cm — one span, six depths of cover, nothing else varies. Holding the span
	 * fixed makes the table a statement about cover alone: `L` is identical in every row of
	 * `H/V = 3L/(4 d_e)`, so any difference between rows is a difference in `d_e`.
	 */
	constexpr int32 CellCount = 10;
	constexpr double ClearSpanCm = CellCount * BrickPitchCm;

	/**
	 * Where the cut starts, and why it depends on course parity. Running bond alternates, so odd
	 * courses sit half a cell along from even ones: index 9 of an odd course is x = 213.75 and
	 * index 10 of an even is x = 225 — the two nearest-centre starts, leaving 9 cells of jamb left
	 * and 10 right, so no row is near a free end. Downstream is written in X, not indices, so one
	 * table cuts course 1 and course 38 alike.
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
	 * The two springings, exactly `L` apart. A one-course cut leaves the course above with
	 * `CellCount - 1` seatless bricks and two keeping half a bed patch each, half a cell outboard.
	 * Those two are the springings, so their centres are
	 *
	 *     (last cut brick + 11.25) - (first cut brick - 11.25)  =  9 * 22.5 + 22.5  =  225
	 *
	 * apart, the clear opening to the centimetre — the hook slice 4 needs, asserted below.
	 */
	constexpr double LeftSpringingXCm(int32 CutCourse)
	{
		return FirstCutBrickXCm(CutCourse) - BondOffsetCm;
	}

	constexpr double RightSpringingXCm(int32 CutCourse)
	{
		return LastCutBrickXCm(CutCourse) + BondOffsetCm;
	}

	/** The one seat each springing keeps: the uncut brick of the cut course just outboard of it. */
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
 * An arch needs masonry over it: the arching depth is capped by the cover above the span, so an
 * opening that stands under deep cover cannot arch with one course on top.
 *
 * The rule, from ARCHING_DESIGN.md:
 *
 *     d_e = min( cover above the span , 0.866 * L )       arching depth
 *     r   = d_e / 3                                       thrust line rise, kern-limited
 *     H   = W * L / (8r)      V = W / 2                   per abutment
 *
 * so `H/V = 3L / (4 d_e)`, W cancels. Slice 3 assumed `d_e = 0.866*L`, at which `L` cancels too
 * and the ratio is the constant 3/(4*0.866) = 0.866051. Slice 4 is the cover: `d_e` is found by a
 * bounded upward walk over bed joints (at most `ceil(0.866L / pitch)` steps, never a spatial
 * query) and capped by the angle, not replaced by it.
 *
 * It is the permissive direction: with `r = d_e/3`, `H` grows as `1/cover` while `V` falls, so the
 * thrust ratio blows up as cover thins. Here the uncapped answer is 0.866051 at any cover; the
 * capped answer is 22.5 with one course over. A ten-cell hole under one course stands today and
 * cannot: one course is a beam in flexure over ten bricks, and they hang in mid-air.
 *
 * One span, six depths, table is the test: every row cuts the same ten cells of the same 30x40
 * flush wall, differing only in which course, so `L` is identical and any difference is `d_e`.
 *
 * What is asserted, and why:
 *
 *   - H/V = 3L/(4 d_e) on every row, at 2% — the mechanism a red run reads against. W cancels, so
 *     it depends on no wall weight or load distribution. The four cover-governed rows want 22.5,
 *     11.25, 5.625, 2.25; strengths cannot move them (the 2026-08-13 mean re-anchor did not).
 *
 *   - The two angle-governed rows are green on arrival and must stay so. 202.5 and 285 cm of cover
 *     both exceed 0.866 * 225 = 194.85, so the angle caps and both read 0.866051 (slice 3's own
 *     figure). They guard that cover is a `min`, not a replacement: `d_e = cover` gives 0.833 and
 *     0.592 here. The 285 cm row is StructureThrustTest's fixture (shear ~0.27 at mean), so it
 *     also states slice 4 moves nothing slice 3 pinned.
 *
 *   - Cover is counted in whole course pitches, spanning course first — behind ARCHING_DESIGN's
 *     285 cm (a 40-course wall cut at course 1 leaves 38 * 7.5). Asserted, not derived: brick
 *     height reads 25.96 vs 22.5, excluding the spanning course zeroes the shallowest row. Both
 *     alternatives are printed per row.
 *
 *   - The span is the abutment separation, a fixture fact: 225 cm apart, clear opening 225 cm, so
 *     no new query reintroduces `L`. Seat-centroid to seat-centroid is 236.25, 5% wider.
 *
 *   - The shear axis governs, asserted first: ComputeUtilisation returns the worst of three, and an
 *     arched springing's compression axis reads 2|sigma_n|/f_c, a plausible number beside the one
 *     asserted, so a thrust-aimed fixture would silently measure compression if it were higher.
 *
 *   - The outcome arm is retired at the 2026-08-13 mean re-anchor. On the characteristic basis the
 *     one-course row came down; at mean strengths every springing affords its thrust (shallowest
 *     ~0.35) and the wall stands under every cover. The machinery (bMustComeDown, the attribution
 *     precondition) is kept for the owed replacement — a 20-cell cut under one course, ~1.4x over,
 *     specified in CURRENT_STATE, to be measured in green.
 *
 * Never a displacement: two pieces can sever and stay resting where they were. Needs no ticking
 * world: FStructure and Layout are plain arithmetic.
 *
 * No reveal here — every cut is one course tall. A multi-course opening has a jamb brick that
 * overhangs with no head joint on its eccentric side, a cantilever that peels and takes the
 * springing's seat. That is slice 5's; a one-course cut has none.
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

	/*
	 * The expected numbers are ratios of published strengths, so they hold only while the profile
	 * carries the figures they were derived against. Asserted, not imported: a test that read the
	 * profile would agree with a wrong one.
	 */
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

	/**
	 * One depth of cover. Ordered shallowest first, which is worst-first: the thrust ratio falls
	 * as cover deepens until the angle takes over, then stops moving.
	 */
	struct FCoverCase
	{
		const TCHAR* Description;

		/** Which course of the 40 the ten cells come out of. The course above it spans. */
		int32 CutCourse;

		/**
		 * ARCHING_DESIGN's figure for the springing's shear, or 0 where it published none — a
		 * factor-of-two cross-check only, since the design calls its span limit its least
		 * trustworthy number and asks for the ordering to be pinned instead.
		 */
		double DesignUtilisation;

		/** Whether this row also makes the outcome claim: true only where the re-seat head joint
		 * beside the springing is under capacity, so the thrust is unambiguously what decided. */
		bool bMustComeDown;
	};

	/*
	 * MEAN RE-ANCHOR (2026-08-13), which inverted this table's verdict layer. Springing capacity
	 * moved from 0.2 + 0.6 sigma to 0.9 + 0.75 sigma, and the seat stress under thin cover is tiny
	 * (~0.014 MPa), so every row now affords its thrust: even H/V = 22.5 costs 0.315 MPa against
	 * 0.91. The four cover-governed rows flip to under-capacity, the one-course outcome arm is
	 * retired, and the H/V = 3L/(4 d_e) geometry is what still pins hard.
	 *
	 * The lost falls-for-thin-cover discriminator is owed a replacement (CURRENT_STATE): demand is
	 * linear in spanned load, capacity nearly constant, so a wider opening under one course fails
	 * again — a 20-cell cut at course 38 puts ~11 bricks on each springing (sigma ~ 0.028) under
	 * H/V = 45, ~1.26 MPa against ~0.92, ~1.4x over. Measured in the green phase, never tuned.
	 *
	 * The design cross-checks are re-derived through each row's implied seat stress: 2.635 implies
	 * sigma = 0.0252, re-reads 0.617; 0.763 implies sigma = 0.3738, re-reads 0.274.
	 */
	const TArray<FCoverCase> Cases = {
		/*
		 * One course over — 7.5 cm — ARCHING_DESIGN's worked case. d_e = 7.5, r = 2.5, so
		 * H/V = 3*225/(4*7.5) = 22.5 against an uncapped 0.866051: a factor of 25.98 (= 0.866*L/
		 * cover). Characteristic basis read ~1.51 capped and was the outcome row; at mean it reads
		 * ~0.35 and stands.
		 */
		{ TEXT("ONE course of cover"), 38, 0.617, false },

		/* Two courses, 15 cm: H/V = 11.25. */
		{ TEXT("TWO courses of cover"), 37, 0.0, false },

		/* Four courses, 30 cm: H/V = 5.625. */
		{ TEXT("FOUR courses of cover"), 35, 0.0, false },

		/*
		 * Ten courses, 75 cm: H/V = 2.25, the last row where cover still governs by a margin.
		 * 0.866 * 225 = 194.85, so the crossover is at 25.98 courses.
		 */
		{ TEXT("TEN courses of cover"), 29, 0.0, false },

		/*
		 * Twenty-seven courses, 202.5 cm — just past the crossover, so the angle governs and the
		 * answer is 0.866051. Must stay green: the row saying cover is a `min`, not a replacement.
		 * `d_e = cover` reads 3*225/(4*202.5) = 0.833 here, 4% low. Not 26 courses (195 cm), which
		 * clears 194.85 by 0.15 cm — a coincidence, not a margin.
		 */
		{ TEXT("TWENTY-SEVEN courses of cover, past the crossover"), 12, 0.0, false },

		/*
		 * Thirty-eight courses, 285 cm — the deepest this wall has, StructureThrustTest's ten-cell
		 * fixture. The angle governs by 46%, so slice 4 may not move it: 0.866051 of H/V before and
		 * after (the shear reading moved with the mean re-anchor to ~0.27). `d_e = cover` reads
		 * 0.592, 32% low.
		 */
		{ TEXT("THIRTY-EIGHT courses of cover, the deepest this wall has"), 1, 0.274, false },
	};

	/** Each row's measured H/V, kept so the two angle-governed rows can be compared to each other. */
	TArray<double> MeasuredThrustPerReaction;
	MeasuredThrustPerReaction.Init(0.0, Cases.Num());

	for (int32 Row = 0; Row < Cases.Num(); ++Row)
	{
		const FCoverCase& Case = Cases[Row];

		const double CoverCm = CoverAboveCutCm(ScenarioWallCourses, Case.CutCourse);
		const double ArchingDepthCm = FMath::Min(CoverCm, ArchingDepthPerSpan * ClearSpanCm);
		const double ExpectedThrustPerReaction = ThrustPerReaction(ClearSpanCm, CoverCm);

		/* The cover, counted the two other ways it could reasonably be counted. */
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

		/** One end of the arch: the half-seated brick the thrust is delivered to the ground through. */
		struct FSpringingCase
		{
			const TCHAR* Side;

			double BrickXCm;
			double SeatXCm;

			/** Which way it overhangs: +1 is toward increasing X, i.e. INTO the opening. */
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

		/*
		 * The hook ARCHING_DESIGN measured, asserted not trusted: the two abutment centres are `L`
		 * apart to the centimetre, so slice 4 reintroduces the span from the abutments' positions,
		 * no new query. Seat-centroid to seat-centroid is BondOffsetCm wider (236.25), 5% high.
		 */
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

			/*
			 * The springing is the half seat slices 1 and 2 arch — 10.25 x 10.25, loaded 5.625 cm
			 * off its centroid. Checked against the producer, not assumed.
			 */
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

			/*
			 * And the opening is genuinely spanned, which makes it an arch. The mid-hole bricks
			 * have no seat and are re-seated onto the abutments by slice 2; if any were Stranded or
			 * Falling, this row would measure a collapse, not a thrust.
			 */
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

			// --- what the joint carries -------------------------------------------------------

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

			/* The seat is in compression, the gate the whole thrust line depends on: no
			 * compression, no thrust line, no arch. */
			TestTrue(
				FString::Printf(
					TEXT("%s: FIXTURE: the seat must be in COMPRESSION, sigma_n is %s MPa"),
					*Where, *Bits(Published.NormalStressMPa)),
				Published.NormalStressMPa < 0.0);

			/*
			 * And the Mohr-Coulomb ceiling is not what is deciding. ARCHING_DESIGN says the
			 * 1.3 MPa truncation is never reached in this regime, so if it ever were, the answer
			 * would be governed by the cap rather than by friction and would mean something else.
			 */
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
			 * The mechanism, and the row a red run should be read against.
			 *
			 * H/V = 3L/(4 d_e) with d_e = min(cover, 0.866*L). W cancels out of it, so this is a
			 * statement about the geometry of the thrust line and nothing else — it does not
			 * depend on what the wall above weighs or on how the solver divided it.
			 *
			 * 2% everywhere. There is nothing here for a tolerance to absorb: L is 225 exactly,
			 * the cover is a whole number of course pitches, and 2% is slack for 0.866 against
			 * sqrt(3)/2 and the small difference between the two ends' columns. It does not span
			 * brick-height cover (15% out), seat-to-seat span (5% out), or an uncapped depth (a
			 * factor of 26 out on the shallowest row) — all three are printed above instead.
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

			/*
			 * The axis, before any claim about whether it stands. On an arched springing the
			 * compression axis reads 2|sigma_n|/f_c — a plausible small number sitting right
			 * beside the one being asserted — so a fixture aimed at the thrust would silently
			 * measure compression instead the moment compression happened to be higher.
			 */
			TestTrue(
				FString::Printf(
					TEXT("%s: SHEAR must be the governing axis — shear %s against compression %s ")
					TEXT("and tension %s — or this row is measuring the wrong thing"),
					*Where, *Bits(Published.ShearUtilisation),
					*Bits(Published.CompressionUtilisation), *Bits(Published.TensionUtilisation)),
				Published.ShearUtilisation > Published.CompressionUtilisation
					&& Published.ShearUtilisation > Published.TensionUtilisation);

			/*
			 * And the joint's own reading agrees with beam theory and Mohr-Coulomb on the force
			 * and moment it publishes. This pins that the thrust is evaluated as shear on the bed
			 * joint against `c + mu*sigma_n` rather than through some second, private rule — the
			 * whole design claim is that no new axis and no new strength are needed.
			 */
			TestTrue(
				FString::Printf(
					TEXT("%s: it must read what beam theory and Mohr-Coulomb say, %s, and it reads %s"),
					*Where, *Bits(Published.Worst), *Bits(Utilisation)),
				FMath::Abs(Utilisation - Published.Worst)
					<= 1.0e-12 * FMath::Max(Published.Worst, 1.0e-12));

			/*
			 * Every row of this ten-cell table is under capacity at mean strengths — the
			 * cover-governed rows included (see the re-anchor note at the case table; on the
			 * characteristic basis the four cover-governed rows read over and the shallowest
			 * carried the outcome claim). The cover still moves the demand — the H/V pins above
			 * are the mechanism — but this wall's springings now afford all of it, and the
			 * falls-for-thin-cover row is owed as a wider replacement fixture.
			 */
			TestTrue(
				FString::Printf(
					TEXT("%s: at mean strengths this springing must afford its thrust — UNDER ")
					TEXT("capacity in shear, it reads %s"),
					*Where, *Bits(Published.ShearUtilisation)),
				Published.ShearUtilisation < 1.0);

			/*
			 * And loosely against the design's own number, where it published one — a factor of
			 * two either way, an order-of-magnitude cross-check and nothing more. ARCHING_DESIGN
			 * calls its own span limit the least trustworthy figure it contains and asks for the
			 * ordering to be pinned instead; a factor of two still catches a missing 100x, a
			 * missing division by three in the rise, or a thrust taken as W rather than W*L/(8r).
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
		 * Trap 2 — sigma H = 0 across the arch, worth repeating here rather than leaving to
		 * slice 3's file. Measuring the cover invites measuring it per abutment, at which point
		 * the two ends of one arch can disagree about d_e and push each other by different amounts
		 * — a net horizontal force out of nowhere, with every joint still reading plausibly. Both
		 * ends of this fixture stand under identical cover, so the row cannot catch an asymmetric
		 * measurement; what it catches is an asymmetric application of the answer.
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
		 * What the re-seat head joint beside each springing is carrying, and why it has to be
		 * read before any outcome is claimed.
		 *
		 * Slice 2 routes the whole spanned group's load outward through these head joints, in pure
		 * shear against 0.2 MPa of cohesion with no normal force to buy friction with. Under deep
		 * cover that joint carries about four and a half columns of a forty-course wall and is far
		 * past capacity — why the deep ten-cell wall does not in fact stand today, a slice 2
		 * consequence rather than a missing thrust. Under one course of cover it carries four and
		 * a half bricks and is nowhere near it, which is what lets the shallow row say the thrust
		 * is what decided.
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

		/*
		 * And the outcome: a ten-cell hole under one course of brickwork comes down.
		 *
		 * Attribution first. The claim is that the thrust brought it down, so everything else in
		 * the load path has to be demonstrably not what decided — otherwise the row is green for
		 * slice 2's reasons and says nothing about the cover.
		 */
		TestTrue(
			FString::Printf(
				TEXT("%s: ATTRIBUTION: the re-seat head joints must be well UNDER capacity — the ")
				TEXT("worst reads %s — or a collapse here is slice 2's and not the thrust's"),
				Case.Description, *Bits(WorstHeadJointUtilisation)),
			WorstHeadJointUtilisation < 0.5);

		/*
		 * A single severed joint is not a collapse, so the outcome is a count of pieces left with
		 * no path to the ground, plus the fact that the two springings are among the joints that
		 * failed under load — the mechanism this slice adds rather than any old way of a wall
		 * falling over.
		 *
		 * Counted by break pass and not by HasGiven. GetBreakPass's contract spells the encoding
		 * out: a joint that went with a removed piece has HasGiven true and a pass of INDEX_NONE,
		 * because it never snapped.
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

		/* At least the masonry over the opening — the CellCount - 1 bricks with no seat and the
		 * two springings that were carrying them. Stated as a floor rather than an exact count
		 * because how far along the course the loss travels is not something this slice claims. */
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
	 * And the cap is a `min`: past the angle, more cover changes nothing.
	 *
	 * The last two rows stand under 202.5 cm and 285 cm of cover — 41% apart — and both are past
	 * 0.866 * 225 = 194.85, so both must read the same 3/(4*0.866). This is the row that separates
	 * `d_e = min(cover, 0.866L)` from `d_e = cover`: under the latter the two would differ by
	 * exactly the ratio of their covers, the whole 41%.
	 *
	 * 1% rather than exact: the two openings sit at different heights in a flush wall, so their
	 * springings' columns are not the same number of bits and H/V is H over one end's V.
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
