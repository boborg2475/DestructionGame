// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"
#include "Tests/ArchingWallTestSupport.h"
#include "Tests/StaircaseWallTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

// Named, not anonymous: anonymous namespaces from different files collide in a unity build.
namespace StructureThrustTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace StructureArchingTestSupport;

	// The wall, the 0.866 constant and the H/V oracle are shared via ArchingWallTestSupport.h.

	/**
	 * One course tall, so there is no reveal (a multi-course opening's jamb brick peels as a
	 * cantilever; see CURRENT_STATE).
	 */
	constexpr int32 CutCourse = 1;

	constexpr int32 SpannedCourse = CutCourse + 1;

	/**
	 * Masonry over the opening, cm (285 at course 1). It caps the arching depth, so d_e is
	 * angle-limited on the narrow case and cover-limited on the wide one.
	 */
	constexpr double CoverAboveTheCutCm = CoverAboveCutCm(ScenarioWallCourses, CutCourse);

	/** Total load an intact piece carries on its bed joints. */
	inline double TotalCarriedUu(const FStructure& Structure, int32 Piece)
	{
		double TotalUu = 0.0;

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			if (Structure.GetJointRole(Joint, Piece) == EJointRole::BedBeneath
				&& !Structure.GetConnection(Joint).HasGiven())
			{
				TotalUu += FMath::Abs(Structure.GetConnectionForce(Joint).Z);
			}
		}

		return TotalUu;
	}

	// Slice 2's three-cell fixture on the 7 x 30 wall, for the re-seat head joint.
	constexpr int32 SpannedHoleFirstCutBrickIndex = 1;
	constexpr int32 SpannedHoleCellCount = 3;
}

/**
 * An arch thrusts sideways and the springing carries it in shear on its bed joint, so an opening
 * too wide for its abutments comes down (ARCHING_DESIGN.md): d_e = min(cover, 0.866*L), r = d_e/3,
 * H = W*L/(8r), V = W/2 per abutment. No new axis or profile data.
 *
 * Asserted: each springing carries non-zero H; sigma H = 0 across the arch; H/V = 3L/(4 d_e)
 * (0.86605 when the angle governs); shear governs; dry stone never arches (0.866/0.7 = 1.237 at
 * any span). At mean strengths both widths stand; a too-wide replacement fixture is owed
 * (CURRENT_STATE).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureThrustTest,
	"DestructionGame.Core.Structure.AnArchThrustsAndTheSpringingMustCarryIt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureThrustTest::RunTest(const FString& Parameters)
{
	using namespace StructureArchingTestSupport;
	using namespace StructureThrustTestSupport;
	using namespace StaircaseWallTestSupport;

	// Pin the profile figures the expectations were derived against.
	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against the mean f_v0 = 0.9 MPa (re-anchor 2026-08-13; Gooch et al. 2023/2025), the profile carries %g"),
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
		FString::Printf(
			TEXT("FIXTURE: dry stone must have EXACTLY zero cohesion — it carries %g — or the ")
			TEXT("row that says it can never arch is measuring a weak bond instead of no bond"),
			DryStone.ShearCohesionMPa),
		DryStone.ShearCohesionMPa == 0.0);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against dry stone friction 0.7, the profile carries %g"),
			DryStone.FrictionCoefficient),
		DryStone.FrictionCoefficient == 0.7);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against clay brick at 1.9 g/cm3, the profile carries %g"),
			ClayBrick.DensityGramsPerCubicCm),
		ClayBrick.DensityGramsPerCubicCm == 1.9);

	AddInfo(FString::Printf(
		TEXT("FIXTURE: %d x %d flush wall, cut one course tall at course %d, so the cover above ")
		TEXT("the opening is %g cm"),
		ScenarioWallBricksPerCourse, ScenarioWallCourses, CutCourse, CoverAboveTheCutCm));

	/** One opening. Both are centred on x = 315, so they differ only in span. */
	struct FThrustCase
	{
		const TCHAR* Description;

		int32 CellCount;

		/** First full brick of course 1 to delete; the flush half bat precedes index 0. */
		int32 FirstCutBrickIndex;

		/** ARCHING_DESIGN.md's figure for the springing, a loose cross-check. */
		double DesignUtilisation;

		bool bMustComeDown;

		/**
		 * Whether cover (not the 0.866 angle) governs d_e. Sets the H/V tolerance: 2% when the angle
		 * governs (exact 3/(4 x 0.866)), 12% when cover does.
		 */
		bool bCoverGoverns;
	};

	/*
	 * Design cross-checks re-derived at mean strengths via the design's implied seat stress. At mean
	 * strengths no width that fits this wall comes down; a replacement collapse fixture is owed
	 * (CURRENT_STATE).
	 */
	const TArray<FThrustCase> Cases = {
		// 225 cm: 0.866*L = 194.85 < 285 cover, so the angle governs.
		{ TEXT("a 10-cell opening"), 10, 9, 0.274, false, false },

		// 450 cm: 0.866*L = 389.7 > 285, so the cover governs and H/V = 1.184.
		{ TEXT("a 20-cell opening"), 20, 4, 0.606, false, true },
	};

	for (const FThrustCase& Case : Cases)
	{
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

		for (int32 Cell = 0; Cell < Case.CellCount; ++Cell)
		{
			const double BrickXCm = ArchWallOddBrickXCm(Case.FirstCutBrickIndex + Cell);
			const int32 Piece = StaircasePieceAt(Cut.Boxes, BrickXCm, ArchWallCourseZCm(CutCourse));

			if (Piece == INDEX_NONE || !Cut.Structure.RemovePiece(Piece))
			{
				AddError(FString::Printf(
					TEXT("%s: FIXTURE: there should be a brick at x %g in course %d to delete"),
					Case.Description, BrickXCm, CutCourse));

				bCutLaid = false;
				break;
			}
		}

		if (!bCutLaid)
		{
			continue;
		}

		Cut.Structure.SolveLoads();

		// Seat-to-seat span is 11.25 cm wider than clear; the tolerances cover the difference.
		const double ClearSpanCm = Case.CellCount * BrickPitchCm;
		const double SeatToSeatSpanCm = ClearSpanCm + BondOffsetCm;

		const double ArchingDepthCm =
			FMath::Min(CoverAboveTheCutCm, ArchingDepthPerSpan * ClearSpanCm);

		const double ExpectedThrustPerReaction = ThrustPerReaction(ClearSpanCm, CoverAboveTheCutCm);

		AddInfo(FString::Printf(
			TEXT("%s: L = %g cm clear (%g seat to seat), cover %g cm, so d_e = %g, r = %g and ")
			TEXT("H/V must be %s (%s if the span is measured seat to seat, %s if the cover cap ")
			TEXT("is deferred to slice 4)"),
			Case.Description, ClearSpanCm, SeatToSeatSpanCm, CoverAboveTheCutCm,
			ArchingDepthCm, ArchingDepthCm / 3.0, *Bits(ExpectedThrustPerReaction),
			*Bits(ThrustPerReaction(SeatToSeatSpanCm, CoverAboveTheCutCm)),
			*Bits(3.0 / (4.0 * ArchingDepthPerSpan))));

		/** One end of the arch: the half-seated brick. */
		struct FSpringingCase
		{
			const TCHAR* Side;

			double BrickXCm;
			double SeatXCm;

			/** Overhang direction; +1 is toward +X. */
			double EccentricSign;
		};

		const FSpringingCase Springings[2] = {
			{
				TEXT("LEFT springing"),
				ArchWallEvenBrickXCm(Case.FirstCutBrickIndex),
				ArchWallOddBrickXCm(Case.FirstCutBrickIndex - 1),
				+1.0
			},
			{
				TEXT("RIGHT springing"),
				ArchWallEvenBrickXCm(Case.FirstCutBrickIndex + Case.CellCount),
				ArchWallOddBrickXCm(Case.FirstCutBrickIndex + Case.CellCount),
				-1.0
			},
		};

		int32 SpringingJoints[2] = { INDEX_NONE, INDEX_NONE };
		double HorizontalUu[2] = { 0.0, 0.0 };
		double VerticalUu[2] = { 0.0, 0.0 };
		bool bBothSpringingsRead = true;

		for (int32 End = 0; End < 2; ++End)
		{
			const FSpringingCase& Springing = Springings[End];

			const FString Where = FString::Printf(
				TEXT("%s, %s"), Case.Description, Springing.Side);

			const int32 Brick = StaircasePieceAt(
				Cut.Boxes, Springing.BrickXCm, ArchWallCourseZCm(SpannedCourse));
			const int32 Seat = StaircasePieceAt(
				Cut.Boxes, Springing.SeatXCm, ArchWallCourseZCm(CutCourse));

			if (Brick == INDEX_NONE || Seat == INDEX_NONE)
			{
				AddError(FString::Printf(
					TEXT("%s: FIXTURE: no brick at x %g in course %d, or no seat at x %g in course %d"),
					*Where, Springing.BrickXCm, SpannedCourse, Springing.SeatXCm, CutCourse));

				bBothSpringingsRead = false;
				continue;
			}

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

			// Half seat, 10.25 x 10.25, loaded 5.625 cm off centre.
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

			const double EccentricityCm =
				Cut.Boxes[Brick].CentreCm.X - Bed.InterfaceCentreCm.X;

			TestTrue(
				FString::Printf(
					TEXT("%s: FIXTURE: it should overhang its seat by %g cm INTO the opening, it ")
					TEXT("overhangs %g"),
					*Where, HalfSeatEccentricityCm, EccentricityCm),
				FMath::IsNearlyEqual(
					EccentricityCm, Springing.EccentricSign * HalfSeatEccentricityCm, 1.0e-9));

			// The opening must be spanned (neighbour re-seated and Supported), or there is no arch.
			const int32 Neighbour = StaircasePieceAt(
				Cut.Boxes,
				Springing.BrickXCm + Springing.EccentricSign * BrickPitchCm,
				ArchWallCourseZCm(SpannedCourse));

			TestTrue(
				FString::Printf(
					TEXT("%s: FIXTURE: the piece over the opening beside it must be re-seated and ")
					TEXT("Supported (it reads %d, Supported is %d) or there is no arch to thrust"),
					*Where,
					Neighbour == INDEX_NONE
						? -1
						: static_cast<int32>(Cut.Structure.GetPieceSupport(Neighbour)),
					static_cast<int32>(EPieceSupport::Supported)),
				Neighbour != INDEX_NONE
					&& Cut.Structure.GetPieceSupport(Neighbour) == EPieceSupport::Supported);

			/*
			 * Re-seat head joint beside it: reported, not asserted. It carries about half the group in
			 * pure shear, a slice 2 consequence rather than the thrust.
			 */
			if (Neighbour != INDEX_NONE)
			{
				const int32 ReseatJoint = JointBetweenPieces(Cut.Structure, Brick, Neighbour);

				if (ReseatJoint != INDEX_NONE)
				{
					AddInfo(FString::Printf(
						TEXT("%s: FOR INFORMATION ONLY — the re-seat head joint %d beside it ")
						TEXT("carries (%s, %s, %s) uu, %.2f brick weights, and reads %s"),
						*Where, ReseatJoint,
						*Bits(Cut.Structure.GetConnectionForce(ReseatJoint).X),
						*Bits(Cut.Structure.GetConnectionForce(ReseatJoint).Y),
						*Bits(Cut.Structure.GetConnectionForce(ReseatJoint).Z),
						FMath::Abs(Cut.Structure.GetConnectionForce(ReseatJoint).Z) / BrickWeightUu,
						*Bits(Cut.Structure.GetConnectionUtilisation(ReseatJoint))));
				}
			}

			const FVector ForceUu = Cut.Structure.GetConnectionForce(BedJoint);
			const FVector MomentUuCm = Cut.Structure.GetConnectionMoment(BedJoint);

			const double Utilisation = Cut.Structure.GetConnectionUtilisation(BedJoint);

			const FBedJointReading Published = ReadBedJoint(
				ForceUu, MomentUuCm, Bed.InterfaceHalfExtentCm, Bed.InterfaceAreaSqCm,
				GeneralPurposeMortar);

			/*
			 * Signed as stored (force on PieceB, the brick above on both ends), so an outward thrust gives
			 * opposite signs.
			 */
			HorizontalUu[End] = ForceUu.X;
			VerticalUu[End] = FMath::Abs(ForceUu.Z);

			const double ThrustRatio = VerticalUu[End] > 0.0
				? FMath::Abs(ForceUu.X) / VerticalUu[End]
				: 0.0;

			AddInfo(FString::Printf(
				TEXT("%s: joint %d carries (%s, %s, %s) uu — V = %.2f brick weights, H/V = %s — ")
				TEXT("and publishes %s uu.cm. sigma_n %s MPa, shear %s MPa against a capacity of ")
				TEXT("%s MPa"),
				*Where, BedJoint, *Bits(ForceUu.X), *Bits(ForceUu.Y), *Bits(ForceUu.Z),
				VerticalUu[End] / BrickWeightUu, *Bits(ThrustRatio), *Bits(MomentUuCm.Size()),
				*Bits(Published.NormalStressMPa),
				*Bits(FVector(ForceUu.X, ForceUu.Y, 0.0).Size()
					/ (Bed.InterfaceAreaSqCm * ForceUnitsPerMPaPerSqCm)),
				*Bits(GeneralPurposeMortar.ShearCohesionMPa
					+ GeneralPurposeMortar.FrictionCoefficient
						* FMath::Abs(Published.NormalStressMPa))));

			AddInfo(FString::Printf(
				TEXT("%s: reads %s — tension %s, compression %s, SHEAR %s; ARCHING_DESIGN says %g"),
				*Where, *Bits(Utilisation), *Bits(Published.TensionUtilisation),
				*Bits(Published.CompressionUtilisation), *Bits(Published.ShearUtilisation),
				Case.DesignUtilisation));

			// No compression, no thrust line.
			TestTrue(
				FString::Printf(TEXT("%s: FIXTURE: the seat must be in COMPRESSION, sigma_n is %s MPa"),
					*Where, *Bits(Published.NormalStressMPa)),
				Published.NormalStressMPa < 0.0);

			// The shear ceiling must not be what decides.
			const double UntruncatedCapacityMPa = GeneralPurposeMortar.ShearCohesionMPa
				+ GeneralPurposeMortar.FrictionCoefficient * FMath::Abs(Published.NormalStressMPa);

			TestTrue(
				FString::Printf(
					TEXT("%s: FIXTURE: the shear ceiling must NOT be reached — Mohr-Coulomb gives ")
					TEXT("%s MPa against the profile's %g MPa cap"),
					*Where, *Bits(UntruncatedCapacityMPa),
					GeneralPurposeMortar.MaxShearStrengthMPa),
				UntruncatedCapacityMPa < GeneralPurposeMortar.MaxShearStrengthMPa);

			TestTrue(
				FString::Printf(
					TEXT("%s: the springing must carry a HORIZONTAL thrust; its force is ")
					TEXT("(%s, %s, %s) uu"),
					*Where, *Bits(ForceUu.X), *Bits(ForceUu.Y), *Bits(ForceUu.Z)),
				FMath::Abs(ForceUu.X) > 0.0);

			// H/V = 3L/(4 d_e); W cancels, so this is geometry only.
			const double ThrustRatioTolerance = Case.bCoverGoverns ? 0.12 : 0.02;

			TestTrue(
				FString::Printf(
					TEXT("%s: the thrust must be H/V = 3L/(4 d_e) = %s; the joint reports H = %s uu ")
					TEXT("against V = %s uu, a ratio of %s. (A W that excluded the springings' own ")
					TEXT("columns would read about %s; a deferred cover cap would read %s.)"),
					*Where, *Bits(ExpectedThrustPerReaction), *Bits(ForceUu.X),
					*Bits(VerticalUu[End]), *Bits(ThrustRatio),
					*Bits(ExpectedThrustPerReaction * Case.CellCount
						/ static_cast<double>(Case.CellCount + 1)),
					*Bits(3.0 / (4.0 * ArchingDepthPerSpan))),
				FMath::Abs(ThrustRatio - ExpectedThrustPerReaction)
					<= ThrustRatioTolerance * ExpectedThrustPerReaction);

			// Shear must govern, or the rows below would silently measure compression.
			TestTrue(
				FString::Printf(
					TEXT("%s: SHEAR must be the governing axis — shear %s against compression %s ")
					TEXT("and tension %s — or this row is measuring the wrong thing"),
					*Where, *Bits(Published.ShearUtilisation),
					*Bits(Published.CompressionUtilisation), *Bits(Published.TensionUtilisation)),
				Published.ShearUtilisation > Published.CompressionUtilisation
					&& Published.ShearUtilisation > Published.TensionUtilisation);

			// The thrust is evaluated as ordinary Mohr-Coulomb shear, not a private rule.
			TestTrue(
				FString::Printf(
					TEXT("%s: it must read what beam theory and Mohr-Coulomb say, %s, and it reads %s"),
					*Where, *Bits(Published.Worst), *Bits(Utilisation)),
				FMath::Abs(Utilisation - Published.Worst)
					<= 1.0e-12 * FMath::Max(Published.Worst, 1.0e-12));

			if (Case.bMustComeDown)
			{
				TestTrue(
					FString::Printf(
						TEXT("%s: %d cells is too wide for its abutment — the springing must be ")
						TEXT("OVER capacity in shear, it reads %s"),
						*Where, Case.CellCount, *Bits(Published.ShearUtilisation)),
					Published.ShearUtilisation > 1.0);
			}
			else
			{
				TestTrue(
					FString::Printf(
						TEXT("%s: %d cells must still stand — the springing must be UNDER capacity ")
						TEXT("in shear, it reads %s"),
						*Where, Case.CellCount, *Bits(Published.ShearUtilisation)),
					Published.ShearUtilisation < 1.0);
			}

			// Within 2x of the design figure: catches a missing 100x or a wrong rise.
			TestTrue(
				FString::Printf(
					TEXT("%s: ARCHING_DESIGN predicts %g for this springing and it reads %s — a ")
					TEXT("cross-check, not a target, so it allows a factor of two"),
					*Where, Case.DesignUtilisation, *Bits(Published.ShearUtilisation)),
				Published.ShearUtilisation >= 0.5 * Case.DesignUtilisation
					&& Published.ShearUtilisation <= 2.0 * Case.DesignUtilisation);
		}

		if (!bBothSpringingsRead)
		{
			continue;
		}

		/*
		 * Sigma H = 0 across the arch. Joints are evaluated independently, so thrust applied at one end
		 * only would go unnoticed.
		 */
		const double HorizontalSumUu = HorizontalUu[0] + HorizontalUu[1];
		const double LargerThrustUu = FMath::Max(
			FMath::Abs(HorizontalUu[0]), FMath::Abs(HorizontalUu[1]));

		AddInfo(FString::Printf(
			TEXT("%s: the two springings carry H = %s and %s uu, summing to %s; V = %s and %s uu"),
			Case.Description, *Bits(HorizontalUu[0]), *Bits(HorizontalUu[1]),
			*Bits(HorizontalSumUu), *Bits(VerticalUu[0]), *Bits(VerticalUu[1])));

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
		 * Outcome: pieces with no path to ground, and the springings among joints broken under load.
		 * Counted by break pass, since a joint severed with a removed piece has no pass.
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
			if (!Cut.Structure.IsPieceRemoved(Piece)
				&& !Cut.Structure.IsPieceSupported(Piece))
			{
				++Unrouted;
			}
		}

		AddInfo(FString::Printf(
			TEXT("%s: the cascade ran %d passes, %d of %d joints failed under load, and %d of the ")
			TEXT("%d pieces it did not delete are left with no path to the ground"),
			Case.Description, BreakingPasses, JointsBrokenByLoad,
			Cut.Structure.NumConnections(), Unrouted,
			Cut.Structure.NumPieces() - Case.CellCount));

		if (Case.bMustComeDown)
		{
			// A floor: at least the CellCount + 1 bricks of the spanned course.
			TestTrue(
				FString::Printf(
					TEXT("%s: an opening too wide for its abutments must COME DOWN — at least the ")
					TEXT("%d bricks over it should lose their path to the ground; %d pieces did"),
					Case.Description, Case.CellCount + 1, Unrouted),
				Unrouted >= Case.CellCount + 1);

			for (int32 End = 0; End < 2; ++End)
			{
				TestTrue(
					FString::Printf(
						TEXT("%s, %s: and the springing itself must be one of the joints that ")
						TEXT("failed under load; joint %d broke in pass %d"),
						Case.Description, Springings[End].Side, SpringingJoints[End],
						Cut.Structure.GetBreakPass(SpringingJoints[End])),
					Cut.Structure.GetBreakPass(SpringingJoints[End]) != INDEX_NONE);
			}
		}
	}

	/*
	 * Part 2: the re-seat head joint on the three-cell fixture. Pins conservation (the hanger's one
	 * route to ground carries its whole column), and only that utilisation is under capacity and
	 * self-consistent, since a legitimate thrust implementation may change the number.
	 */
	{
		FBrickLayout Intact;
		FBrickLayout Cut;

		if (!RunningBond(ArchWallSpec(), Intact) || Intact.Boxes.Num() != ArchWallPieceCount
			|| !RunningBond(ArchWallSpec(), Cut) || Cut.Boxes.Num() != ArchWallPieceCount)
		{
			AddError(TEXT("HEAD JOINT: FIXTURE: the 7 x 30 wall did not lay twice"));
		}
		else
		{
			Intact.Structure.SolveLoads();

			bool bCutLaid = true;

			for (int32 Cell = 0; Cell < SpannedHoleCellCount; ++Cell)
			{
				const double BrickXCm =
					ArchWallOddBrickXCm(SpannedHoleFirstCutBrickIndex + Cell);
				const int32 Piece =
					StaircasePieceAt(Cut.Boxes, BrickXCm, ArchWallCourseZCm(CutCourse));

				if (Piece == INDEX_NONE || !Cut.Structure.RemovePiece(Piece))
				{
					AddError(TEXT("HEAD JOINT: FIXTURE: the three-cell cut could not be made"));
					bCutLaid = false;
					break;
				}
			}

			if (bCutLaid)
			{
				Cut.Structure.SolveLoads();

				// Four bricks over the hole: two springings, two seatless hangers. Subject: hanger-springing joint.
				const int32 Springing = StaircasePieceAt(
					Cut.Boxes, ArchWallEvenBrickXCm(SpannedHoleFirstCutBrickIndex),
					ArchWallCourseZCm(SpannedCourse));

				const int32 Hanger = StaircasePieceAt(
					Cut.Boxes, ArchWallEvenBrickXCm(SpannedHoleFirstCutBrickIndex + 1),
					ArchWallCourseZCm(SpannedCourse));

				const int32 IntactHanger = StaircasePieceAt(
					Intact.Boxes, ArchWallEvenBrickXCm(SpannedHoleFirstCutBrickIndex + 1),
					ArchWallCourseZCm(SpannedCourse));

				const int32 HeadJoint = Springing == INDEX_NONE || Hanger == INDEX_NONE
					? INDEX_NONE
					: JointBetweenPieces(Cut.Structure, Springing, Hanger);

				if (HeadJoint == INDEX_NONE || IntactHanger == INDEX_NONE)
				{
					AddError(TEXT("HEAD JOINT: FIXTURE: the springing, hanger or head joint is missing"));
				}
				else
				{
					const FConnection& Head = Cut.Structure.GetConnection(HeadJoint);

					TestTrue(
						FString::Printf(
							TEXT("HEAD JOINT: FIXTURE: it must be an intact HEAD joint of %g cm2; ")
							TEXT("it is role %d, area %g"),
							HeadJointAreaSqCm,
							static_cast<int32>(Cut.Structure.GetJointRole(HeadJoint, Hanger)),
							Head.InterfaceAreaSqCm),
						Cut.Structure.GetJointRole(HeadJoint, Hanger) == EJointRole::Head
							&& !Head.HasGiven()
							&& FMath::IsNearlyEqual(
								Head.InterfaceAreaSqCm, HeadJointAreaSqCm, 1.0e-9));

					TestTrue(
						FString::Printf(
							TEXT("HEAD JOINT: FIXTURE: the hanger must have NO seat at all (it has ")
							TEXT("%d) and must still be Supported (it reads %d)"),
							IntactSeatsBeneath(Cut.Structure, Hanger),
							static_cast<int32>(Cut.Structure.GetPieceSupport(Hanger))),
						IntactSeatsBeneath(Cut.Structure, Hanger) == 0
							&& Cut.Structure.GetPieceSupport(Hanger) == EPieceSupport::Supported);

					const double IntactColumnUu = TotalCarriedUu(Intact.Structure, IntactHanger);

					const FVector ForceUu = Cut.Structure.GetConnectionForce(HeadJoint);
					const FVector MomentUuCm = Cut.Structure.GetConnectionMoment(HeadJoint);

					FVector UnitNormal = Head.InterfaceNormal;
					UnitNormal.Normalize();

					const FHeadJointReading Published = ReadHeadJoint(
						ForceUu, UnitNormal, Head.InterfaceAreaSqCm, GeneralPurposeMortar);

					const double Utilisation = Cut.Structure.GetConnectionUtilisation(HeadJoint);

					AddInfo(FString::Printf(
						TEXT("HEAD JOINT: joint %d carries (%s, %s, %s) uu against the hanger's ")
						TEXT("intact column of %s uu (%.4f brick weights); sigma_n %s MPa, shear ")
						TEXT("%s MPa against a capacity of %s; it reads %s"),
						HeadJoint, *Bits(ForceUu.X), *Bits(ForceUu.Y), *Bits(ForceUu.Z),
						*Bits(IntactColumnUu), IntactColumnUu / BrickWeightUu,
						*Bits(Published.NormalStressMPa), *Bits(Published.ShearStressMPa),
						*Bits(Published.ShearCapacityMPa), *Bits(Utilisation)));

					// One route to ground, and the column above is unchanged by the cut.
					TestTrue(
						FString::Printf(
							TEXT("HEAD JOINT: the shear through it must be the hanger's whole ")
							TEXT("column, %s uu; it carries %s uu vertically"),
							*Bits(IntactColumnUu), *Bits(FMath::Abs(ForceUu.Z))),
						FMath::Abs(FMath::Abs(ForceUu.Z) - IntactColumnUu)
							<= 1.0e-9 * IntactColumnUu);

					/*
					 * A re-seated piece is indeterminate: the head joint is a bookkeeping route, not a cantilever.
					 * With a moment it would carry its column across 11.25 cm and snap.
					 */
					TestTrue(
						FString::Printf(
							TEXT("HEAD JOINT: it must publish NO moment — a re-seated piece is ")
							TEXT("indeterminate — and it publishes %s uu.cm"),
							*Bits(MomentUuCm.Size())),
						MomentUuCm == FVector::ZeroVector);

					TestTrue(
						FString::Printf(
							TEXT("HEAD JOINT: it must read what Mohr-Coulomb says on the force it ")
							TEXT("reports, %s, and it reads %s"),
							*Bits(Published.Worst), *Bits(Utilisation)),
						FMath::Abs(Utilisation - Published.Worst)
							<= 1.0e-12 * FMath::Max(Published.Worst, 1.0e-12));

					TestTrue(
						FString::Printf(
							TEXT("HEAD JOINT: and it must still be INTACT — the three-cell wall ")
							TEXT("stands, so nothing slice 3 adds here may take it over capacity; ")
							TEXT("it reads %s"),
							*Bits(Utilisation)),
						Utilisation < 1.0);
				}
			}
		}
	}

	/*
	 * Part 3: dry stone cannot arch at any span. Capacity is 0.7*sigma_n and H/V >= 0.866, so the
	 * springing reads 0.866/0.7 = 1.2372 whatever the load. Two and five cells (one cell leaves no
	 * seatless piece, so no arch) check the answer does not move with span.
	 */
	{
		FRunningBondSpec Spec;
		Spec.BrickSizeCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm);
		Spec.JointThicknessCm = MortarJointCm;
		Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;

		/**
		 * Twenty courses gives 135 cm of cover, clearing the five-cell opening's 0.866*112.5 = 97.425 cm
		 * by 39%, so the angle (not the cover) governs.
		 */
		Spec.CoursesHigh = 20;
		Spec.BricksPerCourse = 12;
		Spec.End = EWallEnd::Flush;
		Spec.Strength = DryStone;

		struct FDryStoneCase
		{
			const TCHAR* Description;
			int32 CellCount;
			int32 FirstCutBrickIndex;
		};

		const TArray<FDryStoneCase> DryStoneCases = {
			// Two cells is the narrowest arch: one seatless brick.
			{ TEXT("dry stone, a TWO-cell opening"), 2, 5 },
			{ TEXT("dry stone, a FIVE-cell opening"), 5, 3 },
		};

		// 0.866/0.7 = 1.237215440448697, sigma_n cancelled.
		const double DryStoneSpringingUtilisation =
			(3.0 / (4.0 * ArchingDepthPerSpan)) / DryStone.FrictionCoefficient;

		for (const FDryStoneCase& Case : DryStoneCases)
		{
			FBrickLayout Cut;

			if (!RunningBond(Spec, Cut))
			{
				AddError(FString::Printf(
					TEXT("%s: FIXTURE: the dry stone wall did not lay"), Case.Description));

				continue;
			}

			bool bCutLaid = true;

			for (int32 Cell = 0; Cell < Case.CellCount; ++Cell)
			{
				const double BrickXCm = ArchWallOddBrickXCm(Case.FirstCutBrickIndex + Cell);
				const int32 Piece =
					StaircasePieceAt(Cut.Boxes, BrickXCm, ArchWallCourseZCm(CutCourse));

				if (Piece == INDEX_NONE || !Cut.Structure.RemovePiece(Piece))
				{
					AddError(FString::Printf(
						TEXT("%s: FIXTURE: there should be a brick at x %g in course %d"),
						Case.Description, BrickXCm, CutCourse));

					bCutLaid = false;
					break;
				}
			}

			if (!bCutLaid)
			{
				continue;
			}

			Cut.Structure.SolveLoads();

			const int32 Brick = StaircasePieceAt(
				Cut.Boxes, ArchWallEvenBrickXCm(Case.FirstCutBrickIndex),
				ArchWallCourseZCm(SpannedCourse));

			const int32 BedJoint = Brick == INDEX_NONE
				? INDEX_NONE
				: TheOneIntactSeatBeneath(Cut.Structure, Brick);

			if (BedJoint == INDEX_NONE)
			{
				AddError(FString::Printf(
					TEXT("%s: FIXTURE: the left springing must rest on exactly one bed joint"),
					Case.Description));

				continue;
			}

			// The neighbour must be seatless and Supported, or there is no arch and shear reads zero.
			const int32 Neighbour = StaircasePieceAt(
				Cut.Boxes, ArchWallEvenBrickXCm(Case.FirstCutBrickIndex) + BrickPitchCm,
				ArchWallCourseZCm(SpannedCourse));

			TestTrue(
				FString::Printf(
					TEXT("%s: FIXTURE: the brick over the opening beside the springing must have ")
					TEXT("NO seat (it has %d) and must still be Supported (it reads %d, Supported ")
					TEXT("is %d) — otherwise there is no group, no arch and nothing to thrust"),
					Case.Description,
					Neighbour == INDEX_NONE
						? -1
						: IntactSeatsBeneath(Cut.Structure, Neighbour),
					Neighbour == INDEX_NONE
						? -1
						: static_cast<int32>(Cut.Structure.GetPieceSupport(Neighbour)),
					static_cast<int32>(EPieceSupport::Supported)),
				Neighbour != INDEX_NONE
					&& IntactSeatsBeneath(Cut.Structure, Neighbour) == 0
					&& Cut.Structure.GetPieceSupport(Neighbour) == EPieceSupport::Supported);

			const FConnection& Bed = Cut.Structure.GetConnection(BedJoint);

			const FVector ForceUu = Cut.Structure.GetConnectionForce(BedJoint);
			const FVector MomentUuCm = Cut.Structure.GetConnectionMoment(BedJoint);

			const FBedJointReading Published = ReadBedJoint(
				ForceUu, MomentUuCm, Bed.InterfaceHalfExtentCm, Bed.InterfaceAreaSqCm, DryStone);

			const double ThrustRatio = FMath::Abs(ForceUu.Z) > 0.0
				? FMath::Abs(ForceUu.X) / FMath::Abs(ForceUu.Z)
				: 0.0;

			AddInfo(FString::Printf(
				TEXT("%s: joint %d carries (%s, %s, %s) uu — H/V = %s — sigma_n %s MPa against a ")
				TEXT("capacity of %s MPa; shear %s, compression %s"),
				Case.Description, BedJoint, *Bits(ForceUu.X), *Bits(ForceUu.Y), *Bits(ForceUu.Z),
				*Bits(ThrustRatio), *Bits(Published.NormalStressMPa),
				*Bits(DryStone.FrictionCoefficient * FMath::Abs(Published.NormalStressMPa)),
				*Bits(Published.ShearUtilisation), *Bits(Published.CompressionUtilisation)));

			TestTrue(
				FString::Printf(
					TEXT("%s: FIXTURE: the seat must be in COMPRESSION or there is no friction to ")
					TEXT("borrow; sigma_n is %s MPa"),
					Case.Description, *Bits(Published.NormalStressMPa)),
				Published.NormalStressMPa < 0.0);

			TestTrue(
				FString::Printf(
					TEXT("%s: the springing must read %s in shear — 0.866/0.7, with sigma_n ")
					TEXT("cancelled out, so it is the same at every span and every load — and it ")
					TEXT("reads %s"),
					Case.Description, *Bits(DryStoneSpringingUtilisation),
					*Bits(Published.ShearUtilisation)),
				FMath::Abs(Published.ShearUtilisation - DryStoneSpringingUtilisation)
					<= 0.02 * DryStoneSpringingUtilisation);

			TestTrue(
				FString::Printf(
					TEXT("%s: so a dry-stone opening can NEVER arch — the springing must be over ")
					TEXT("capacity, it reads %s"),
					Case.Description, *Bits(Published.ShearUtilisation)),
				Published.ShearUtilisation > 1.0);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
