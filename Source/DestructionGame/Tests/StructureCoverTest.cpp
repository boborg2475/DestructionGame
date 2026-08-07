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
 * NAMED NAMESPACE, not anonymous, and named for what it holds. An anonymous namespace is private
 * to a TRANSLATION UNIT rather than to a file, and a unity build merges many files into one — at
 * which point two anonymous namespaces in the blob are the SAME namespace and identically-named
 * helpers in files that never refer to each other are a hard compile error.
 */
namespace StructureCoverTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace StructureArchingTestSupport;

	/**
	 * TEN CELLS, 225 cm — ONE SPAN, SIX DEPTHS OF COVER, AND NOTHING ELSE VARIES.
	 *
	 * ARCHING_DESIGN works its cover table for exactly this opening in exactly this wall, and
	 * holding the span fixed is what makes the file a statement about the cover alone: `L` appears
	 * identically in every row of `H/V = 3L/(4 d_e)`, so any difference between two rows is a
	 * difference in `d_e` and can be nothing else.
	 */
	constexpr int32 CellCount = 10;
	constexpr double ClearSpanCm = CellCount * BrickPitchCm;

	/**
	 * WHERE THE CUT STARTS, AND WHY IT DEPENDS ON THE PARITY OF THE COURSE.
	 *
	 * Running bond alternates, so the bricks of an odd course sit half a cell along from those of
	 * an even one. Index 9 of an odd course is x = 213.75 and index 10 of an even course is
	 * x = 225 — the two nearest-to-centre starts available, leaving nine cells of jamb on the left
	 * and ten on the right of a thirty-cell wall, so no row is anywhere near a free end.
	 *
	 * EVERYTHING DOWNSTREAM IS WRITTEN IN X RATHER THAN IN INDICES, which is what lets one table
	 * cut course 1 and course 38 and mean the same thing by it.
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
	 * THE TWO SPRINGINGS, AND THEY ARE EXACTLY `L` APART.
	 *
	 * A one-course cut leaves the course above it with `CellCount - 1` bricks that have no seat at
	 * all and two that keep half a bed patch each, half a cell outboard of the cut. Those two are
	 * the springings, so the distance between their centres is
	 *
	 *     (last cut brick + 11.25) - (first cut brick - 11.25)  =  9 * 22.5 + 22.5  =  225
	 *
	 * which is the clear opening to the centimetre. ARCHING_DESIGN records that measurement as the
	 * hook slice 4 needs — the span is already available from the abutments' own positions and no
	 * new query is required to get it — and the fixture asserts it below rather than assuming it.
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
 * AN ARCH NEEDS MASONRY OVER IT: THE ARCHING DEPTH IS CAPPED BY THE COVER ACTUALLY FOUND ABOVE THE
 * SPAN, SO THE SAME OPENING THAT STANDS UNDER DEEP COVER CANNOT ARCH WITH ONE COURSE ON TOP.
 *
 * THE RULE, FROM ARCHING_DESIGN.md:
 *
 *     d_e = min( cover above the span , 0.866 * L )       arching depth
 *     r   = d_e / 3                                       thrust line rise, kern-limited
 *     H   = W * L / (8r)      V = W / 2                   per abutment
 *
 * so `H/V = 3L / (4 d_e)`, and W cancels. Slice 3 built that with `d_e` ASSUMED to be `0.866*L`,
 * at which point `L` cancels too and the ratio is the constant 3/(4*0.866) = 0.866051 at every
 * span and every depth. Slice 4 is the cover: `d_e` must be found by a BOUNDED UPWARD WALK OVER
 * BED JOINTS — at most `ceil(0.866L / course pitch)` steps, never a spatial query — and capped
 * with the angle rather than replaced by it.
 *
 * WHY IT MATTERS, AND IT IS THE PERMISSIVE DIRECTION. With `r = d_e/3` and `d_e` capped by the
 * cover, `H` grows as `1/cover` while `V` falls with it, so the thrust ratio blows up as the
 * masonry over an opening thins. On this fixture the uncapped answer is 0.866051 whatever the
 * cover; the capped answer is 22.5 with one course over. A ten-cell hole under a single course of
 * brickwork stands today and cannot: one course is not an arch ring, it is a beam in flexure over
 * ten bricks, and ten bricks hang in mid-air.
 *
 * ONE SPAN, SIX DEPTHS, AND THE TABLE IS THE TEST. Every row cuts the SAME ten cells out of the
 * SAME 30 x 40 flush wall and differs only in which course the cut is made in, so `L` is identical
 * everywhere and any difference between two rows is a difference in `d_e`. Adding a depth of cover
 * is adding a row.
 *
 * WHAT IS ASSERTED, AND WHY EACH FORM WAS CHOSEN.
 *
 *   - H/V = 3L/(4 d_e) ON EVERY ROW, AT 2%. This is the mechanism and it is what a red run should
 *     be read against. W cancels out of the ratio, so it depends on no wall weight, no load
 *     distribution and no definition of which columns count — it is a statement about the geometry
 *     of the thrust line and nothing else. THE FOUR COVER-GOVERNED ROWS ARE THE RED ONES: they
 *     want 22.5, 11.25, 5.625 and 2.25 and they all read 0.866051 today.
 *
 *   - AND THE TWO ANGLE-GOVERNED ROWS ARE GREEN ON ARRIVAL AND MUST STAY THAT WAY. 202.5 cm and
 *     285 cm of cover both exceed 0.866 * 225 = 194.85, so the angle caps the depth and the answer
 *     is 0.866051 in both — which is exactly what slice 3 already computes. They are the guard
 *     that says the cover is a `min` and not a replacement: an implementation that took `d_e` as
 *     the cover outright would read 0.833 and 0.592 here and take both rows red. The 285 cm row is
 *     the identical fixture `StructureThrustTest` measures at 0.88649 of shear, so it is also the
 *     statement that slice 4 moves nothing slice 3 pinned.
 *
 *   - THE COVER IS COUNTED IN WHOLE COURSE PITCHES, AND THE SPANNING COURSE IS THE FIRST OF THEM.
 *     That is the definition behind ARCHING_DESIGN's own 285 cm — a 40-course wall cut at course 1
 *     leaves courses 2 through 39, and 38 * 7.5 = 285 — and it is asserted here rather than
 *     derived, because the alternatives are real and differ by more than the tolerance: counting
 *     6.5 cm of brick instead of 7.5 cm of pitch would read 25.96 where this expects 22.5, and
 *     EXCLUDING the spanning course would make the shallowest row's cover zero and its thrust
 *     infinite. Both alternatives are printed on every row so a red run says which one happened.
 *
 *   - THE SPAN IS THE DISTANCE BETWEEN THE TWO ABUTMENTS, ASSERTED AS A FIXTURE FACT. They are
 *     225 cm apart and the clear opening is 225 cm, so the hook ARCHING_DESIGN measured is real
 *     and no new query is needed to reintroduce `L`. Seat-centroid to seat-centroid is 236.25 —
 *     5% wider, outside the 2% tolerance — so a row that used it fails and says so.
 *
 *   - THE SHEAR AXIS GOVERNS, ASSERTED BEFORE ANY NUMBER IS CLAIMED. ComputeUtilisation returns
 *     the worst of three, so a fixture aimed at the thrust would silently measure compression the
 *     moment compression happened to be higher — and an arched springing's compression axis reads
 *     2|sigma_n|/f_c, a plausible small number sitting right beside the one being asserted.
 *
 *   - AND THE OUTCOME, ON THE ONE ROW WHERE THE THRUST IS UNAMBIGUOUSLY WHAT DECIDES. A single
 *     severed joint is not a collapse, so the outcome claim is that the two springings are among
 *     the joints that FAILED UNDER LOAD and that at least the eleven bricks over the opening are
 *     left with no path to the ground. It is made only for the one-course row, and only after
 *     asserting that the re-seat head joint beside the springing is UNDER capacity there — on
 *     deeper rows that head joint is slice 2's own limit and is far past capacity, so a collapse
 *     there would prove nothing about the cover. Where the thing under test does not govern the
 *     result, the row does not claim the result.
 *
 * NEVER A DISPLACEMENT, ANYWHERE. Two pieces can sever and stay resting exactly where they were,
 * so how far anything moved would say nothing.
 *
 * NEEDS A TICKING WORLD: NO. FStructure is plain arithmetic over a graph and Layout is plain
 * arithmetic over boxes; nothing here needs an actor, a tick or a renderer, and slices 1 to 3
 * needed none either.
 *
 * NO REVEAL IS BEING MEASURED. Every cut here is ONE COURSE TALL. A multi-course opening has a
 * jamb brick one course below the spanning course which keeps a patch on the jamb and overhangs
 * into the opening with no head joint on its eccentric side — a genuine cantilever that peels and
 * takes the springing's seat with it. That is slice 5's, and a one-course cut has none.
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
	 * THE EXPECTED NUMBERS ARE RATIOS OF PUBLISHED STRENGTHS, so they mean what they say only
	 * while the profile still carries the figures they were derived against. Asserted rather than
	 * imported: a test that read the profile would agree with a wrong profile.
	 */
	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against f_vk0 = 0.2 MPa, the profile carries %g"),
			GeneralPurposeMortar.ShearCohesionMPa),
		GeneralPurposeMortar.ShearCohesionMPa == 0.2);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against friction 0.6, the profile carries %g"),
			GeneralPurposeMortar.FrictionCoefficient),
		GeneralPurposeMortar.FrictionCoefficient == 0.6);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against a 1.3 MPa shear ceiling, the profile carries %g"),
			GeneralPurposeMortar.MaxShearStrengthMPa),
		GeneralPurposeMortar.MaxShearStrengthMPa == 1.3);

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
	 * One depth of cover, and everything about it this file has an opinion on.
	 *
	 * THE ROWS ARE ORDERED SHALLOWEST FIRST, which is also worst-first: the thrust ratio falls
	 * monotonically as the cover deepens until the angle takes over, and then it stops moving.
	 */
	struct FCoverCase
	{
		const TCHAR* Description;

		/** Which course of the 40 the ten cells come out of. The course above it spans. */
		int32 CutCourse;

		/**
		 * ARCHING_DESIGN's own figure for the springing's shear, or 0 where it published none.
		 * A factor-of-two cross-check and nothing more — the design says its own span limit is
		 * the least trustworthy number it contains, and asks for the ordering to be pinned.
		 */
		double DesignUtilisation;

		/**
		 * Whether this row also makes the OUTCOME claim. True only where the re-seat head joint
		 * beside the springing is under capacity, so the thrust is unambiguously what decided.
		 */
		bool bMustComeDown;
	};

	const TArray<FCoverCase> Cases = {
		/*
		 * ONE COURSE OVER — 7.5 cm — AND IT IS ARCHING_DESIGN'S OWN WORKED CASE. d_e = 7.5,
		 * r = 2.5, so H/V = 3*225/(4*7.5) = 22.5 against the 0.866051 an uncapped depth gives:
		 * a factor of 25.98, which is 0.866*L/cover exactly. The design records the springing at
		 * 0.101 uncapped and 2.635 capped; this fixture reads about 0.058 and about 1.51, the
		 * same 26-fold step from a lighter springing.
		 */
		{ TEXT("ONE course of cover"), 38, 2.635, true },

		/* Two courses, 15 cm: H/V = 11.25. */
		{ TEXT("TWO courses of cover"), 37, 0.0, false },

		/* Four courses, 30 cm: H/V = 5.625. */
		{ TEXT("FOUR courses of cover"), 35, 0.0, false },

		/*
		 * Ten courses, 75 cm: H/V = 2.25, and this is the last row where the cover still governs
		 * by a margin. 0.866 * 225 = 194.85, so the crossover is at 25.98 courses.
		 */
		{ TEXT("TEN courses of cover"), 29, 0.0, false },

		/*
		 * TWENTY-SEVEN COURSES, 202.5 cm — JUST PAST THE CROSSOVER, so the ANGLE governs and the
		 * answer is 0.866051. Green today and it must stay green: this is the row that says the
		 * cover is a `min` rather than a replacement. An implementation that used the cover
		 * outright reads 3*225/(4*202.5) = 0.833 here, 4% low, and fails.
		 *
		 * Deliberately not 26 courses (195 cm), which clears 194.85 by 0.15 cm. That is not a
		 * margin, it is a coincidence.
		 */
		{ TEXT("TWENTY-SEVEN courses of cover, past the crossover"), 12, 0.0, false },

		/*
		 * THIRTY-EIGHT COURSES, 285 cm — the deepest this wall has, and the IDENTICAL FIXTURE
		 * StructureThrustTest measures its ten-cell case on. The angle governs by 46%, so slice 4
		 * may not move it at all: it reads 0.866051 of H/V and 0.88649 of shear before and after.
		 * An implementation that used the cover outright reads 0.592, 32% low.
		 */
		{ TEXT("THIRTY-EIGHT courses of cover, the deepest this wall has"), 1, 0.763, false },
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
		 * THE HOOK ARCHING_DESIGN MEASURED, ASSERTED RATHER THAN TAKEN ON TRUST: the two abutment
		 * centres are `L` apart to the centimetre, so slice 4 can reintroduce the span it needs
		 * from the abutments' own positions and no new query is required. Seat-centroid to
		 * seat-centroid is BondOffsetCm wider — 236.25 — and would read 5% high.
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
			 * THE SPRINGING IS THE SAME HALF SEAT SLICES 1 AND 2 ALREADY ARCH — 10.25 x 10.25,
			 * loaded 5.625 cm off its own centroid. Arbitrated against the producer rather than
			 * assumed, so that the number and the reason for it fail together.
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
			 * AND THE OPENING IS GENUINELY SPANNED, WHICH IS WHAT MAKES THIS AN ARCH AT ALL. The
			 * bricks in the middle of the hole have no seat whatever and are re-seated onto the
			 * group's abutments by slice 2; if any were Stranded or Falling instead, this row
			 * would be measuring a collapse rather than a thrust.
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

			/*
			 * THE SEAT IS IN COMPRESSION, which is the gate the whole thrust line depends on: no
			 * compression, no thrust line, no arch.
			 */
			TestTrue(
				FString::Printf(
					TEXT("%s: FIXTURE: the seat must be in COMPRESSION, sigma_n is %s MPa"),
					*Where, *Bits(Published.NormalStressMPa)),
				Published.NormalStressMPa < 0.0);

			/*
			 * AND THE MOHR-COULOMB CEILING IS NOT WHAT IS DECIDING. ARCHING_DESIGN says the
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
			 * THE MECHANISM, AND IT IS THE ROW A RED RUN SHOULD BE READ AGAINST.
			 *
			 * H/V = 3L/(4 d_e) with d_e = min(cover, 0.866*L). W cancels out of it, so this is a
			 * statement about the geometry of the thrust line and nothing else — it does not
			 * depend on what the wall above weighs or on how the solver divided it.
			 *
			 * 2% EVERYWHERE. There is nothing here for a tolerance to absorb: L is 225 exactly,
			 * the cover is a whole number of course pitches, and 2% is slack for 0.866 against
			 * sqrt(3)/2 and for the small difference between the two ends' columns. It does NOT
			 * span brick-height cover (15% out), seat-to-seat span (5% out), or an uncapped depth
			 * (a factor of 26 out on the shallowest row) — all three are printed above rather than
			 * tolerated.
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
			 * THE AXIS, BEFORE ANY CLAIM ABOUT WHETHER IT STANDS. On an arched springing the
			 * compression axis reads 2|sigma_n|/f_c — a plausible small number sitting right
			 * beside the one being asserted — so a fixture aimed at the thrust would measure
			 * compression instead the moment compression happened to be higher, and silently.
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
			 * AND THE JOINT'S OWN READING AGREES WITH BEAM THEORY AND MOHR-COULOMB ON THE FORCE
			 * AND MOMENT IT PUBLISHES. This is what pins that the thrust is evaluated AS SHEAR ON
			 * THE BED JOINT against `c + mu*sigma_n` rather than through some second, private
			 * rule — the whole design claim is that no new axis and no new strength are needed.
			 */
			TestTrue(
				FString::Printf(
					TEXT("%s: it must read what beam theory and Mohr-Coulomb say, %s, and it reads %s"),
					*Where, *Bits(Published.Worst), *Bits(Utilisation)),
				FMath::Abs(Utilisation - Published.Worst)
					<= 1.0e-12 * FMath::Max(Published.Worst, 1.0e-12));

			/*
			 * THE ORDERING. Where the cover governs, the thrust ratio is at least 2.25 against a
			 * capacity that friction can only lift so far, and the springing is over capacity;
			 * where the angle governs it is 0.866 and the springing holds. That the crossover
			 * between the two lands where it does is a consequence rather than a target — see the
			 * loose cross-check below.
			 */
			const bool bCoverGoverns = CoverCm < ArchingDepthPerSpan * ClearSpanCm;

			if (bCoverGoverns)
			{
				TestTrue(
					FString::Printf(
						TEXT("%s: %g cm of cover cannot carry a %g cm span — the springing must be ")
						TEXT("OVER capacity in shear, it reads %s"),
						*Where, CoverCm, ClearSpanCm, *Bits(Published.ShearUtilisation)),
					Published.ShearUtilisation > 1.0);
			}
			else
			{
				TestTrue(
					FString::Printf(
						TEXT("%s: %g cm of cover is past the 0.866 angle, so this opening arches — ")
						TEXT("the springing must be UNDER capacity in shear, it reads %s"),
						*Where, CoverCm, *Bits(Published.ShearUtilisation)),
					Published.ShearUtilisation < 1.0);
			}

			/*
			 * AND LOOSELY AGAINST THE DESIGN'S OWN NUMBER, where it published one — a factor of two
			 * either way, which is an order-of-magnitude cross-check and nothing more.
			 * ARCHING_DESIGN says its own span limit is the least trustworthy figure it contains
			 * and asks for the ordering to be pinned instead; a factor of two still catches a
			 * missing 100x, a missing division by three in the rise, or a thrust taken as W rather
			 * than as W*L/(8r).
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
		 * TRAP 2 — SIGMA H = 0 ACROSS THE ARCH, AND IT IS WORTH REPEATING HERE RATHER THAN LEAVING
		 * TO SLICE 3'S FILE. Measuring the cover invites measuring it per abutment, at which point
		 * the two ends of one arch can disagree about d_e and push each other by different amounts
		 * — a net horizontal force out of nowhere, with every joint still reading plausibly. Both
		 * ends of this fixture stand under identical cover, so the row cannot catch an asymmetric
		 * MEASUREMENT; what it does catch is an asymmetric APPLICATION of the answer.
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
		 * WHAT THE RE-SEAT HEAD JOINT BESIDE EACH SPRINGING IS CARRYING, AND WHY IT HAS TO BE
		 * READ BEFORE ANY OUTCOME IS CLAIMED.
		 *
		 * Slice 2 routes the whole spanned group's load outward through these head joints, in pure
		 * shear against 0.2 MPa of cohesion with no normal force to buy friction with. Under DEEP
		 * cover that joint carries about four and a half columns of a forty-course wall and is far
		 * past capacity — which is why the deep ten-cell wall does not in fact stand today, and it
		 * is a slice 2 consequence rather than a missing thrust. Under ONE course of cover it
		 * carries four and a half BRICKS and is nowhere near it, which is precisely what makes the
		 * shallow row able to say that the thrust is what decided.
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
		 * ===========================================================================
		 * AND THE OUTCOME: A TEN-CELL HOLE UNDER ONE COURSE OF BRICKWORK COMES DOWN.
		 * ===========================================================================
		 *
		 * ATTRIBUTION FIRST. The claim is that the THRUST brought it down, so everything else in
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
		 * A SINGLE SEVERED JOINT IS NOT A COLLAPSE, so the outcome is a count of pieces left with
		 * no path to the ground, plus the fact that the two springings are among the joints that
		 * FAILED UNDER LOAD — which is the mechanism this slice adds rather than any old way of a
		 * wall falling over.
		 *
		 * COUNTED BY BREAK PASS AND NOT BY HasGiven. GetBreakPass's contract spells the encoding
		 * out: a joint that went WITH A REMOVED PIECE has HasGiven true and a pass of INDEX_NONE,
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

		/*
		 * AT LEAST THE MASONRY OVER THE OPENING — the CellCount - 1 bricks with no seat and the
		 * two springings that were carrying them. Stated as a floor rather than an exact count
		 * because how far along the course the loss travels is not something this slice claims.
		 */
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
	 * ===================================================================================
	 * AND THE CAP IS A `min`: PAST THE ANGLE, MORE COVER CHANGES NOTHING.
	 * ===================================================================================
	 *
	 * The last two rows stand under 202.5 cm and 285 cm of cover — 41% apart — and both are past
	 * 0.866 * 225 = 194.85, so both must read the same 3/(4*0.866). This is the row that separates
	 * `d_e = min(cover, 0.866L)` from `d_e = cover`: under the latter the two would differ by
	 * exactly the ratio of their covers, which is the whole 41%.
	 *
	 * 1% RATHER THAN EXACT. The two openings sit at different heights in a flush wall, so their
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
