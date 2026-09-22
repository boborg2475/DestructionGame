// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"
#include "Tests/ArchingWallTestSupport.h"
#include "Tests/StaircaseWallTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/** Named, not anonymous: unity builds merge files, so anonymous-namespace helpers would collide. */
namespace StructureFreeEndHeightTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * Brick, grid and SI conversion are written out here, and strengths asserted against the profile,
	 * so a wrong production constant cannot agree with itself.
	 */

	constexpr double FreeEndBrickLengthCm = 21.5;
	constexpr double FreeEndBrickWidthCm = 10.25;
	constexpr double FreeEndBrickHeightCm = 6.5;
	constexpr double FreeEndMortarJointCm = 1.0;

	constexpr double FreeEndBrickPitchCm = FreeEndBrickLengthCm + FreeEndMortarJointCm;
	constexpr double FreeEndCoursePitchCm = FreeEndBrickHeightCm + FreeEndMortarJointCm;

	/** Arm of a half-seated brick's own weight from the centroid of its remaining patch. */
	constexpr double FreeEndHalfSeatEccentricityCm = FreeEndBrickPitchCm / 4.0;

	/** Arm of the load arriving from above. */
	constexpr double FreeEndHalfStepCm = FreeEndBrickPitchCm / 2.0;

	/** 1.9 g/cm3, density first so it matches Layout::PieceMassKg bit for bit. */
	constexpr double FreeEndBrickMassKg =
		1.9 * FreeEndBrickLengthCm * FreeEndBrickWidthCm * FreeEndBrickHeightCm / 1000.0;

	/** MassKg * 980 cm/s2 is a force in Unreal units. */
	constexpr double FreeEndBrickWeightUu = FreeEndBrickMassKg * 980.0;

	/**
	 * Force units per MPa over 1 cm2: 1 N = 100 uu and 1 cm2 = 100 mm2, so 10000 uu. Deliberately not
	 * ForceUnitsPerMPaSqCm, so this file fails if that constant is wrong.
	 */
	constexpr double FreeEndForceUnitsPerMPaPerSqCm = 100.0 * 100.0;

	/*
	 * Mean flexural bond f_x1 for general-purpose mortar, asserted against the profile (re-anchor
	 * 2026-08-13; the old characteristic f_xk1 was 0.10). Readings fell 7x, so the capacity crossing
	 * moved from ~59 to ~412 courses and the ladder no longer reaches 1.0 at real heights. A
	 * replacement fixture is owed (CURRENT_STATE); do not re-tune this one.
	 */
	constexpr double FreeEndMortarTensileMPa = 0.70;

	/** Deep-beam section modulus t*D^2/6, cm3. */
	constexpr double FreeEndCompositeModulusCm3(double WallThicknessCm, double DepthCm)
	{
		return WallThicknessCm * DepthCm * DepthCm / 6.0;
	}

	/** Print a double to full precision. */
	inline FString FreeEndBits(double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	/** Readings for one deletion at one wall height. */
	struct FFreeEndRow
	{
		bool bRead = false;

		int32 CoursesHigh = 0;
		int32 PieceCount = 0;

		/** Load on the surviving seat in brick weights, and its moment in brick-weight-cm. */
		double ColumnBrickWeights = 0.0;
		double MomentBrickWeightCm = 0.0;

		/** e = |M|/|F|, cm; bounds the credited depth. */
		double EffectiveArmCm = 0.0;

		/** Credited depth and the masonry actually standing over the joint, cm. */
		double CreditedDepthCm = 0.0;
		double MasonryStandingOverItCm = 0.0;

		double Utilisation = 0.0;

		/** Pieces left seatless by the cut, and pieces unsupported after the cascade. */
		int32 SeatlessAfterTheCut = 0;
		int32 UnroutedAfterTheCascade = 0;
	};
}

/**
 * Test J (COMPOSITE_DEPTH_DESIGN.md): one brick deleted at a free end of a wall much taller than
 * the cut. This decides lambda. The moment is set by the cut but the credited section is measured
 * up the wall, so the two stop cancelling once the wall is taller than the cut.
 *
 * Deletes the outermost grounded full brick. The half bat above loses its seat (allowed, counted).
 * The full brick beside it keeps one seat and overhangs 5.625 cm toward the free end, where there is
 * no head joint, so arching is refused and the joint cantilevers. Same shape as a jamb reveal, and
 * the user's reported stepping-triangle collapse.
 *
 * Asserted: at 40 courses (the game's height) and 30 (ACorbelResistsWithItsWholeDepth PART 2's
 * wall), only seatless pieces are lost; the reading never falls as the wall grows; five heights are
 * reported with an extrapolated capacity crossing.
 *
 * Derivation: M = 5.625 + 11.25*(n - 1), so e tends to 11.25 cm and D = lambda*e is about 39 cm at
 * any height. The reading is linear in height, about 0.00166 per brick weight on the mean basis
 * (~0.072 at 30 courses, crossing near 412). If it fails at 40, lambda is infeasible. No world.
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

	/** The height ADestructionGameGameMode builds (30 bricks by 40 courses). */
	constexpr int32 TheHeightTheGameBuilds = 40;

	/** The wall PART 2 cuts. */
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

		// Delete the outermost grounded full brick at x = 0 (the wall's end), as PART 2 does.
		const int32 EndBrick = StaircasePieceAt(
			Laid.Boxes, ArchSupport::ArchWallEvenBrickXCm(0), ArchSupport::ArchWallCourseZCm(0));

		// The brick left half-seated: course 1's first full brick.
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

		// It must overhang toward the free end, where there is no head joint to arch against.
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
		 * Hand-derived two-arm moment: own weight at 5.625 cm, load from above at 11.25 cm, so
		 * M = 5.625 + 11.25*(n - 1). Not read from GetConnectionMoment, which would agree with
		 * whatever the solver did; only the force comes from the solver. 2 % covers the unmodelled
		 * half bat (about 1 %).
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

		// Expected reading from the hand moment and the credited section. 5 % = 2 % above plus 3 % slack.
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

		// Count pieces with no bed patch (expected: course 1's half bat), so any extra one fails.
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

	// A taller wall may not make the same deletion safer. The premise (more moment) is checked first.
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

	// Extrapolated capacity crossing from the two tallest rows (the reading is linear once e settles).
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

	// The ruling at 30 and 40 courses, asserted on pieces: a single severed joint is not a collapse.
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
		 * At 30 courses this must equal PART 2's reading exactly: same brick, same wall, derived
		 * differently (K*F^2/M there). The shared anchor moves with lambda (ArchingWallTestSupport.h).
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
