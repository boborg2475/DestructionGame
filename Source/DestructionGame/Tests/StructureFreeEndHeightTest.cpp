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
namespace StructureFreeEndHeightTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * --- everything below is spelled out here rather than imported ------------------------
	 *
	 * Same discipline as every other fixture in this directory: the brick, the grid and the SI
	 * conversion are written from first principles and the strengths are ASSERTED against the
	 * profile rather than read out of it. A test that reaches for production's own constant
	 * agrees with a wrong one.
	 */

	constexpr double FreeEndBrickLengthCm = 21.5;
	constexpr double FreeEndBrickWidthCm = 10.25;
	constexpr double FreeEndBrickHeightCm = 6.5;
	constexpr double FreeEndMortarJointCm = 1.0;

	constexpr double FreeEndBrickPitchCm = FreeEndBrickLengthCm + FreeEndMortarJointCm;
	constexpr double FreeEndCoursePitchCm = FreeEndBrickHeightCm + FreeEndMortarJointCm;

	/** How far a half-seated brick's own weight acts from the centroid of the patch it keeps. */
	constexpr double FreeEndHalfSeatEccentricityCm = FreeEndBrickPitchCm / 4.0;

	/** The half step the load from above arrives across. */
	constexpr double FreeEndHalfStepCm = FreeEndBrickPitchCm / 2.0;

	/** 1.9 g/cm3, DENSITY FIRST in the product so it matches Layout::PieceMassKg to the last bit. */
	constexpr double FreeEndBrickMassKg =
		1.9 * FreeEndBrickLengthCm * FreeEndBrickWidthCm * FreeEndBrickHeightCm / 1000.0;

	/** 980 cm/s2. With 1 uu = 1 cm and mass in kg, MassKg * 980 IS a force in Unreal units. */
	constexpr double FreeEndBrickWeightUu = FreeEndBrickMassKg * 980.0;

	/**
	 * Unreal force units that load one square centimetre to one megapascal.
	 *
	 * 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa over 1 cm2 is 10000 uu. DELIBERATELY NOT
	 * DestructionForce::ForceUnitsPerMPaSqCm: this file has to fail if that constant is wrong.
	 */
	constexpr double FreeEndForceUnitsPerMPaPerSqCm = 100.0 * 100.0;

	/*
	 * The MEAN flexural bond f_x1 for general-purpose mortar, asserted against the profile
	 * (re-anchor 2026-08-13; the retired characteristic f_xk1 was EN 1996-1-1's 0.10).
	 *
	 * WHAT THE RE-ANCHOR DOES TO THIS LADDER: every reading divides by 7, so the 20-60 course
	 * rungs fall to ~0.047-0.146 and the extrapolated capacity crossing moves from ~59 courses
	 * to ~412 — far beyond any buildable wall. The fixture keeps its monotone property and its
	 * agreement anchor but LOSES its above-1.0 discrimination at any real height; the
	 * replacement is owed and recorded in CURRENT_STATE (re-design the fixture, e.g. a deeper
	 * multi-course free-end cut, and MEASURE it — never re-tune this one).
	 */
	constexpr double FreeEndMortarTensileMPa = 0.70;

	/** The DEEP BEAM's section, cm3: t*D^2/6 for a wall t thick and D deep. */
	constexpr double FreeEndCompositeModulusCm3(double WallThicknessCm, double DepthCm)
	{
		return WallThicknessCm * DepthCm * DepthCm / 6.0;
	}

	/** Print a double so a comparison that failed in the last bit is readable as one. */
	inline FString FreeEndBits(double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	/** What one deletion at one wall height came to. */
	struct FFreeEndRow
	{
		bool bRead = false;

		int32 CoursesHigh = 0;
		int32 PieceCount = 0;

		/** Brick weights the surviving seat carries, and brick-weight-cm bending it. */
		double ColumnBrickWeights = 0.0;
		double MomentBrickWeightCm = 0.0;

		/** e = |M|/|F|, cm — the statical arm the credited depth is bounded by. */
		double EffectiveArmCm = 0.0;

		/** What the walk credited, cm, and how much masonry actually stands over the joint. */
		double CreditedDepthCm = 0.0;
		double MasonryStandingOverItCm = 0.0;

		double Utilisation = 0.0;

		/** How many pieces the cut left with no bed patch at all, and how many ended unrouted. */
		int32 SeatlessAfterTheCut = 0;
		int32 UnroutedAfterTheCascade = 0;
	};
}

/**
 * ONE BRICK DELETED AT A FREE END, IN A WALL MUCH TALLER THAN THE CUT.
 *
 * TEST J, and `COMPOSITE_DEPTH_DESIGN.md` says outright that this is the number that decides
 * lambda and that no fixture in the project presents it — which is exactly why the safe window
 * for lambda looked wider than it is. The existing free-end row lives in
 * `Core.Structure.ACorbelResistsWithItsWholeDepth` PART 2 and cuts a THIRTY-course wall. The game
 * builds FORTY. Those are different numbers for a reason that is the whole of this subsystem's
 * recurring defect: the moment bending the joint is set by the CUT and the section credited to it
 * is measured by walking UP the WALL, so the two stop cancelling the moment the wall is taller
 * than the cut.
 *
 * =========================================================================================
 * WHAT IS DELETED, AND WHY IT IS THE HARDEST CASE RATHER THAN THE EASIEST
 * =========================================================================================
 *
 * The OUTERMOST full brick of the grounded course. The half bat above it sat entirely on it and
 * has no bed patch at all afterwards — that piece is deliberately allowed to be lost and is
 * COUNTED rather than named. The full brick beside it keeps exactly ONE seat and overhangs it
 * 5.625 cm OUTWARD, toward the free end, where there is no head joint at all because a wall's
 * free vertical end simply has no edge in the graph. So the arching relief is refused, correctly,
 * and the joint cantilevers.
 *
 * IT IS THE SAME SHAPE AS THE JAMB REVEAL, which is why one fixture covers both, and it is the
 * user's own reported case: they deleted a brick and the wall came down in a stepping triangle.
 * The ruling that it must NOT is what composite vertical action exists to satisfy.
 *
 * =========================================================================================
 * WHAT IS ASSERTED, AND WHY EACH FORM WAS CHOSEN
 * =========================================================================================
 *
 *   - IT STANDS AT THE HEIGHT THE GAME ACTUALLY BUILDS. Forty courses, asserted as an OUTCOME
 *     rather than as a reading: only the pieces the cut left with no bed patch at all may be
 *     lost. Displacement is never a valid break assertion and neither is a single severed joint;
 *     a wall can shed one and stand.
 *
 *   - AND AT THIRTY, which is the existing fixture's wall, so that this file and PART 2 are
 *     talking about the same deletion and a divergence between them is visible.
 *
 *   - THE READING RISES WITH WALL HEIGHT AND MAY NEVER FALL. More courses over the joint means
 *     more load through it, so whatever section the model credits the reading may not go DOWN.
 *     That is the one-sided form of PART 1B's property said about a free end instead of a corbel,
 *     it needs no bound to have been chosen, and it is what refuses a rule that credits the whole
 *     wall's depth against a cut one course tall.
 *
 *   - AND THE LADDER IS REPORTED AT FIVE HEIGHTS SO THE TREND IS VISIBLE RATHER THAN INFERRED,
 *     with the height at which it would cross capacity extrapolated from the two tallest rows.
 *
 * DERIVED BEFORE IT WAS MEASURED. A half seat carries its own weight at 5.625 cm and everything
 * arriving from above at 11.25, so `M = 5.625 + 11.25*(n - 1)` and `e = M/F` tends to 11.25 cm as
 * the column grows. With `D = lambda*e` that is about 39 cm of section however tall the wall gets,
 * so the reading is LINEAR in the column and therefore in the wall's height:
 * `M*W_brick / (10.25*39^2/6 * 10^4 * f_x1)`, which on the mean basis (re-anchor 2026-08-13) is
 * about 0.00166 per brick weight — a seventh of the characteristic-basis 0.0116, so the thirty-
 * course rung reads ~0.072, forty ~0.096, and the capacity crossing sits near 412 courses. The
 * characteristic-basis history (0.50636 at thirty, crossing ~59) is what the lambda window was
 * measured against; the ladder now discriminates by trend, not by a crossing.
 *
 * IF IT DOES NOT STAND AT FORTY, LAMBDA IS INFEASIBLE AND THAT IS A RULING, NOT A TUNING. The
 * window `COMPOSITE_DEPTH_DESIGN.md` derives is bounded below by this ruling and above by the
 * one-sided corbel property, and a failure here means there is no value that satisfies both.
 *
 * NEEDS A TICKING WORLD: NO. FStructure is arithmetic over a graph and Layout is arithmetic over
 * boxes.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureFreeEndHeightTest,
	"DestructionGame.Core.Structure.AFreeEndDeletionInATallWall",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureFreeEndHeightTest::RunTest(const FString& Parameters)
{
	using namespace StructureFreeEndHeightTestSupport;
	using namespace StaircaseWallTestSupport;

	namespace ArchSupport = StructureArchingTestSupport;

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against f_xk1 = %g MPa, the profile carries %g"),
			FreeEndMortarTensileMPa, GeneralPurposeMortar.TensileStrengthMPa),
		GeneralPurposeMortar.TensileStrengthMPa == FreeEndMortarTensileMPa);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against clay brick at 1.9 g/cm3, the profile carries %g"),
			ClayBrick.DensityGramsPerCubicCm),
		ClayBrick.DensityGramsPerCubicCm == 1.9);

	/**
	 * THE HEIGHT THE GAME ACTUALLY BUILDS. `ADestructionGameGameMode` lays 30 bricks by 40
	 * courses, and forty is what makes this row the one that decides lambda rather than one more
	 * point on a curve. Named rather than repeated so the two uses below cannot drift.
	 */
	constexpr int32 TheHeightTheGameBuilds = 40;

	/** The wall PART 2 cuts, so the two files are talking about the same deletion. */
	constexpr int32 TheExistingFixturesHeight = 30;

	const int32 Heights[5] = { 20, TheExistingFixturesHeight, TheHeightTheGameBuilds, 50, 60 };

	TArray<FFreeEndRow> Ladder;

	for (const int32 CoursesHigh : Heights)
	{
		FFreeEndRow Row;
		Row.CoursesHigh = CoursesHigh;

		FBrickLayout Laid;

		if (!RunningBond(ArchSupport::ArchWallSpecOfHeight(CoursesHigh), Laid))
		{
			AddError(FString::Printf(
				TEXT("FIXTURE: a flush 7 x %d wall should lay"), CoursesHigh));
			continue;
		}

		Row.PieceCount = Laid.Boxes.Num();

		/*
		 * THE DELETION: the OUTERMOST full brick of the grounded course, at x = 0.
		 *
		 * Course 0 runs 0, 22.5, 45 … so x = 0 is the end of the wall and there is nothing
		 * outboard of it but the odd courses' half bats. That is the click the user made, and it
		 * is the same brick PART 2 deletes from its own thirty-course wall.
		 */
		const int32 EndBrick = StaircasePieceAt(
			Laid.Boxes, ArchSupport::ArchWallEvenBrickXCm(0), ArchSupport::ArchWallCourseZCm(0));

		/* The brick left half-seated by it: course 1's first full brick, overhanging outward. */
		const int32 HalfSeated = StaircasePieceAt(
			Laid.Boxes, ArchSupport::ArchWallOddBrickXCm(0), ArchSupport::ArchWallCourseZCm(1));

		if (EndBrick == INDEX_NONE || HalfSeated == INDEX_NONE
			|| !Laid.Structure.RemovePiece(EndBrick))
		{
			AddError(FString::Printf(
				TEXT("FIXTURE: the %d-course wall should have an end brick to delete and a full ")
				TEXT("brick above it to leave half seated"),
				CoursesHigh));
			continue;
		}

		Laid.Structure.SolveLoads();

		const int32 Seat = ArchSupport::TheOneIntactSeatBeneath(Laid.Structure, HalfSeated);

		if (Seat == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("FIXTURE: at %d courses the brick above the deletion must keep EXACTLY ONE ")
				TEXT("seat, it rests on %d"),
				CoursesHigh, ArchSupport::IntactSeatsBeneath(Laid.Structure, HalfSeated)));
			continue;
		}

		const FConnection& Bed = Laid.Structure.GetConnection(Seat);

		/*
		 * AND IT OVERHANGS OUTWARD, TOWARD THE FREE END, which is what refuses it the arch: the
		 * eccentric side is where the wall stops, so there is no head joint there to push against.
		 * A fixture that had drifted into overhanging INWARD would be measuring the arch instead,
		 * and would read about a hundredth of this.
		 */
		const double EccentricityCm = Laid.Boxes[HalfSeated].CentreCm.X - Bed.InterfaceCentreCm.X;

		TestTrue(
			FString::Printf(
				TEXT("FIXTURE: at %d courses it must overhang its seat by %g cm TOWARD the free ")
				TEXT("end, it overhangs %s"),
				CoursesHigh, FreeEndHalfSeatEccentricityCm, *FreeEndBits(EccentricityCm)),
			FMath::IsNearlyEqual(EccentricityCm, -FreeEndHalfSeatEccentricityCm, 1.0e-9));

		const FVector ForceUu = Laid.Structure.GetConnectionForce(Seat);
		const FVector MomentUuCm = Laid.Structure.GetConnectionMoment(Seat);

		Row.ColumnBrickWeights = FMath::Abs(ForceUu.Z) / FreeEndBrickWeightUu;
		Row.MomentBrickWeightCm = MomentUuCm.Size() / FreeEndBrickWeightUu;
		Row.EffectiveArmCm = ForceUu.Size() > 0.0 ? MomentUuCm.Size() / ForceUu.Size() : 0.0;
		Row.CreditedDepthCm = Laid.Structure.GetConnectionCompositeDepthCm(Seat);
		Row.MasonryStandingOverItCm = (CoursesHigh - 1) * FreeEndCoursePitchCm;
		Row.Utilisation = Laid.Structure.GetConnectionUtilisation(Seat);
		Row.bRead = true;

		/*
		 * THE MOMENT IS A TWO-ARM WALK, NOT ONE ARM, AND IT IS DERIVED RATHER THAN READ BACK.
		 * The brick's OWN weight acts at its centre, 5.625 cm from the centroid of the seat it
		 * keeps; everything arriving from above comes through a patch a further half step INBOARD,
		 * 11.25 cm from that seat. So M = 5.625 + 11.25*(n - 1). Building the expectation out of
		 * `GetConnectionMoment` instead is the trap CURRENT_STATE records: the section this
		 * subsystem changes also changes what that quantity is worth, so an oracle standing on it
		 * agrees with whatever landed. Only the FORCE comes from the solver.
		 *
		 * TWO PER CENT, because the half bat hanging off the head joint contributes its share on
		 * an arm of its own that the two-arm walk does not model — about one per cent on this
		 * fixture, and PART 2 measures the same residual on its own wall.
		 */
		const double HandMomentBrickWeightCm = FreeEndHalfSeatEccentricityCm
			+ FreeEndHalfStepCm * (Row.ColumnBrickWeights - 1.0);

		TestTrue(
			FString::Printf(
				TEXT("FIXTURE: at %d courses the moment must be the two-arm walk — %s brick ")
				TEXT("weights makes %s brick-weight-cm; the joint publishes %s, %s%% out"),
				CoursesHigh, *FreeEndBits(Row.ColumnBrickWeights),
				*FreeEndBits(HandMomentBrickWeightCm), *FreeEndBits(Row.MomentBrickWeightCm),
				*FreeEndBits(100.0 * (Row.MomentBrickWeightCm / HandMomentBrickWeightCm - 1.0))),
			FMath::Abs(Row.MomentBrickWeightCm - HandMomentBrickWeightCm)
				<= 0.02 * HandMomentBrickWeightCm);

		/*
		 * WHAT THE SECTION THE SOLVER CREDITED WOULD READ, WORKED HERE FROM THE HAND MOMENT AND
		 * THE PUBLISHED f_xk1 — so that a joint reading something else says so rather than being
		 * explained by whatever the solver decided. Five per cent, which is the two above plus
		 * three for a correct implementation reading its own moment rather than the hand walk's.
		 */
		if (Row.CreditedDepthCm > 0.0)
		{
			const double Expected = HandMomentBrickWeightCm * FreeEndBrickWeightUu
				/ (FreeEndCompositeModulusCm3(FreeEndBrickWidthCm, Row.CreditedDepthCm)
					* FreeEndForceUnitsPerMPaPerSqCm)
				/ FreeEndMortarTensileMPa;

			TestTrue(
				FString::Printf(
					TEXT("FIXTURE: at %d courses the walk credited %s cm of the %s cm standing over ")
					TEXT("the joint, so a section of t*D^2/6 = %s cm3 must read %s; it reads %s"),
					CoursesHigh, *FreeEndBits(Row.CreditedDepthCm),
					*FreeEndBits(Row.MasonryStandingOverItCm),
					*FreeEndBits(FreeEndCompositeModulusCm3(
						FreeEndBrickWidthCm, Row.CreditedDepthCm)),
					*FreeEndBits(Expected), *FreeEndBits(Row.Utilisation)),
				FMath::Abs(Row.Utilisation - Expected) <= 0.05 * FMath::Max(Expected, 1.0e-12));
		}

		/*
		 * AND THE OUTCOME. One brick is deliberately allowed to be lost and exactly one is
		 * available to lose: course 1's flush half bat sat entirely on the deleted brick, so it
		 * has no bed patch at all, and composite action has nothing to offer a piece that is not
		 * resting on anything. Counted rather than named, so a second unseated piece appearing
		 * anywhere fails here.
		 */
		for (int32 Piece = 0; Piece < Laid.Structure.NumPieces(); ++Piece)
		{
			if (!Laid.Structure.IsPieceRemoved(Piece)
				&& Laid.Boxes[Piece].CentreCm.Z > ArchSupport::ArchWallCourseZCm(0)
				&& ArchSupport::IntactSeatsBeneath(Laid.Structure, Piece) == 0)
			{
				++Row.SeatlessAfterTheCut;
			}
		}

		Laid.Structure.SolveAndBreak();

		for (int32 Piece = 0; Piece < Laid.Structure.NumPieces(); ++Piece)
		{
			if (!Laid.Structure.IsPieceRemoved(Piece) && !Laid.Structure.IsPieceSupported(Piece))
			{
				++Row.UnroutedAfterTheCascade;
			}
		}

		AddInfo(FString::Printf(
			TEXT("FREE END UNDER %d COURSES (%d pieces): the seat carries %s brick weights and %s ")
			TEXT("brick-weight-cm, so e = %s cm. The walk credits %s cm of the %s cm standing over ")
			TEXT("it (%s), and it reads %s. The cut left %d piece(s) with no bed patch; the cascade ")
			TEXT("left %d of %d with no path to the ground."),
			CoursesHigh, Row.PieceCount, *FreeEndBits(Row.ColumnBrickWeights),
			*FreeEndBits(Row.MomentBrickWeightCm), *FreeEndBits(Row.EffectiveArmCm),
			*FreeEndBits(Row.CreditedDepthCm), *FreeEndBits(Row.MasonryStandingOverItCm),
			Row.CreditedDepthCm < Row.MasonryStandingOverItCm - 1.0e-9
				? TEXT("the ARM caps it")
				: TEXT("the WALL caps it"),
			*FreeEndBits(Row.Utilisation), Row.SeatlessAfterTheCut,
			Row.UnroutedAfterTheCascade, Laid.Structure.NumPieces() - 1));

		Ladder.Add(Row);
	}

	if (Ladder.Num() != UE_ARRAY_COUNT(Heights))
	{
		AddError(FString::Printf(
			TEXT("FIXTURE: every one of the %d heights must be readable; %d were"),
			static_cast<int32>(UE_ARRAY_COUNT(Heights)), Ladder.Num()));

		return true;
	}

	/*
	 * THE PROPERTY: A TALLER WALL MAY NOT MAKE THE SAME DELETION SAFER.
	 *
	 * The premise first, because the one-sided claim rests entirely on it — if a taller wall did
	 * not load the joint harder, "must not read lower" would be an assertion about nothing.
	 */
	for (int32 Which = 1; Which < Ladder.Num(); ++Which)
	{
		const FFreeEndRow& Short = Ladder[Which - 1];
		const FFreeEndRow& Tall = Ladder[Which];

		TestTrue(
			FString::Printf(
				TEXT("FIXTURE: %d courses must bend the seat HARDER than %d — %s brick-weight-cm ")
				TEXT("against %s"),
				Tall.CoursesHigh, Short.CoursesHigh, *FreeEndBits(Tall.MomentBrickWeightCm),
				*FreeEndBits(Short.MomentBrickWeightCm)),
			Tall.MomentBrickWeightCm > Short.MomentBrickWeightCm);

		TestTrue(
			FString::Printf(
				TEXT("A TALLER WALL MUST NOT MAKE THE SAME DELETION SAFER — %d courses bends the ")
				TEXT("seat x%s harder than %d, so it may not read LESS; it reads %s against %s, x%s"),
				Tall.CoursesHigh,
				*FreeEndBits(Tall.MomentBrickWeightCm / Short.MomentBrickWeightCm),
				Short.CoursesHigh, *FreeEndBits(Tall.Utilisation),
				*FreeEndBits(Short.Utilisation),
				*FreeEndBits(Tall.Utilisation / Short.Utilisation)),
			Tall.Utilisation >= Short.Utilisation);
	}

	/*
	 * WHERE THE LADDER WOULD CROSS, EXTRAPOLATED FROM ITS TWO TALLEST ROWS AND STATED AS AN
	 * EXTRAPOLATION. The reading is linear in the column once the arm has settled at 11.25 cm, so
	 * a straight line through the top two rows is the honest estimate; it is printed because the
	 * question the user asked is "how much headroom is there", which no single verdict answers.
	 */
	{
		const FFreeEndRow& Second = Ladder[Ladder.Num() - 2];
		const FFreeEndRow& Last = Ladder.Last();

		const double SlopePerCourse = (Last.Utilisation - Second.Utilisation)
			/ static_cast<double>(Last.CoursesHigh - Second.CoursesHigh);

		AddInfo(FString::Printf(
			TEXT("FREE END: the ladder rises %s per course between %d and %d courses, so it would ")
			TEXT("reach capacity at about %s courses — %s m of wall. EXTRAPOLATED, not measured. ")
			TEXT("The game builds %d."),
			*FreeEndBits(SlopePerCourse), Second.CoursesHigh, Last.CoursesHigh,
			*FreeEndBits(SlopePerCourse > 0.0
				? Last.CoursesHigh + (1.0 - Last.Utilisation) / SlopePerCourse
				: 0.0),
			*FreeEndBits(SlopePerCourse > 0.0
				? (Last.CoursesHigh + (1.0 - Last.Utilisation) / SlopePerCourse)
					* FreeEndCoursePitchCm / 100.0
				: 0.0),
			TheHeightTheGameBuilds));
	}

	/*
	 * AND THE RULING, AS AN OUTCOME, AT THE TWO HEIGHTS IT HAS TO HOLD AT.
	 *
	 * Thirty is the wall the existing fixture cuts, so a divergence between this file and PART 2
	 * is visible; forty is the wall the game renders, and it is the row `COMPOSITE_DEPTH_DESIGN.md`
	 * says decides lambda. Stated on the PIECES rather than on the joint: a single severed joint
	 * is not a collapse, and displacement is never a valid break assertion either.
	 */
	for (const FFreeEndRow& Row : Ladder)
	{
		if (Row.CoursesHigh != TheHeightTheGameBuilds
			&& Row.CoursesHigh != TheExistingFixturesHeight)
		{
			continue;
		}

		TestTrue(
			FString::Printf(
				TEXT("THE RULING: A BRICK DELETED AT A FREE END MUST NOT BRING THE WALL DOWN — at ")
				TEXT("%d courses only the %d piece(s) the cut left with no bed patch may be lost, ")
				TEXT("and %d piece(s) of %d were. The seat reads %s. IF THIS FAILS AT %d COURSES, ")
				TEXT("LAMBDA IS INFEASIBLE: the window is bounded below by this ruling and above ")
				TEXT("by the one-sided corbel property, and no value satisfies both."),
				Row.CoursesHigh, Row.SeatlessAfterTheCut, Row.UnroutedAfterTheCascade,
				Row.PieceCount - 1, *FreeEndBits(Row.Utilisation), TheHeightTheGameBuilds),
			Row.UnroutedAfterTheCascade <= Row.SeatlessAfterTheCut);

		TestTrue(
			FString::Printf(
				TEXT("THE RULING, ON THE JOINT: at %d courses the seat must be under capacity, it ")
				TEXT("reads %s"),
				Row.CoursesHigh, *FreeEndBits(Row.Utilisation)),
			Row.Utilisation < 1.0);

		/*
		 * AND AT THIRTY, THE TWO FILES MUST AGREE TO THE LAST DIGIT.
		 *
		 * `Core.Structure.ACorbelResistsWithItsWholeDepth` PART 2 deletes the same brick out of
		 * `ArchWallSpec()`, which IS `ArchWallSpecOfHeight(30)`, and reads the same seat. So
		 * this row and that one are one measurement made twice, and the shared literal is the
		 * only thing standing between them and drifting apart in silence — the two derive the
		 * number differently on purpose (that file from the identity `K*F^2/M`, this one from
		 * whatever section the walk credited), so agreeing is a real claim rather than a
		 * tautology. EXACT, because there is nothing here for a tolerance to absorb: it is the
		 * same arithmetic on the same wall in the same process.
		 *
		 * It is an AGREEMENT anchor, not a derivation, and it moves with lambda. See its own
		 * comment in ArchingWallTestSupport.h.
		 */
		if (Row.CoursesHigh == TheExistingFixturesHeight)
		{
			TestTrue(
				FString::Printf(
					TEXT("THE TWO FILES MUST AGREE: ACorbelResistsWithItsWholeDepth PART 2 reads ")
					TEXT("this same joint on this same wall, and the shared anchor is %s; this ")
					TEXT("file reads %s"),
					*FreeEndBits(ArchSupport::FreeEndThirtyCourseUtilisation),
					*FreeEndBits(Row.Utilisation)),
				Row.Utilisation == ArchSupport::FreeEndThirtyCourseUtilisation);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
