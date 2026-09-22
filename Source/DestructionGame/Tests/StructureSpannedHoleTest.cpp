// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"
#include "Tests/ArchingWallTestSupport.h"
#include "Tests/StaircaseWallTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

// Named namespace: unity builds merge translation units, so anonymous-namespace helpers can collide.
namespace StructureSpannedHoleTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace StructureArchingTestSupport;

	/**
	 * The cut: full bricks 1-3 of course 1 (x = 33.75 to 78.75), leaving a 67.5 cm void from
	 * x = 22.5 to 90 with wall standing either side, so there are no free-end effects.
	 */
	constexpr int32 CutCourse = 1;
	constexpr int32 FirstCutBrickIndex = 1;
	constexpr int32 CutCellCount = 3;

	/** The course above the cut, whose bricks lose their seats. */
	constexpr int32 SpannedCourse = CutCourse + 1;

	/**
	 * The four bricks left without a complete seat (the group):
	 *
	 *      course 2      [ 22.5 ][  45  ][ 67.5 ][  90  ]      the group
	 *      course 1   [11.25][ ---- ][ ---- ][ ---- ][101.25]  the cut
	 *
	 * The outer two (springings) keep one 10.25 x 10.25 half seat each. The middle two keep none
	 * and fall back to their head joints.
	 */
	constexpr double LeftSpringingBrickXCm = ArchWallEvenBrickXCm(1);
	constexpr double RightSpringingBrickXCm = ArchWallEvenBrickXCm(4);

	constexpr double LeftHangingBrickXCm = ArchWallEvenBrickXCm(2);
	constexpr double RightHangingBrickXCm = ArchWallEvenBrickXCm(3);

	/** The one seat each springing keeps: the course-1 brick on its outboard side. */
	constexpr double LeftSpringingSeatXCm = ArchWallOddBrickXCm(0);
	constexpr double RightSpringingSeatXCm = ArchWallOddBrickXCm(4);

	/**
	 * Joints removed with the cut, counted by hand: 4 heads along course 1, 6 beds down and 6 up.
	 * 4 + 6 + 6 = 16. Pinned so an extra joint lost to the deletion cannot hide.
	 */
	constexpr int32 JointsLostWithTheCut = 16;

	/** How many intact bed joints this piece currently rests on. */
	inline int32 IntactSeatsUnder(const FStructure& Structure, int32 Piece)
	{
		int32 Seats = 0;

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			// GetJointRole is pure geometry and still answers for given joints, so check HasGiven too.
			if (Structure.GetJointRole(Joint, Piece) == EJointRole::BedBeneath
				&& !Structure.GetConnection(Joint).HasGiven())
			{
				++Seats;
			}
		}

		return Seats;
	}

	/** The one intact bed joint under a piece, or INDEX_NONE if it does not have exactly one. */
	inline int32 TheOneSeatUnder(const FStructure& Structure, int32 Piece)
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

	/** Everything an intact piece is carrying, read off the joints it rests on. */
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

	/** What ARCHING_DESIGN.md predicts for the springing of a three-cell hole. */
	constexpr double DesignSpringingUtilisation = 0.036;
}

/**
 * A hole wider than one brick is spanned: bricks left with no seat are re-seated onto the
 * group's abutment joints, so nothing comes down (ARCHING_DESIGN.md slice 2).
 *
 * With a three-brick cut, the two middle bricks above hang off their head joints and prop each
 * other (a two-node cycle SolveLoads strands), and slice 1's trap-3 gate correctly refuses the
 * arch. Slice 2 groups contiguous incomplete-seat pieces through head joints; a group with a
 * live seat on both sides of its centre re-seats onto joints outside itself, acyclically.
 *
 * Asserts routing (nothing Stranded/Falling) and joints failed under load, never displacement.
 * Springing loads are derived from the intact wall: all four columns must leave through the two
 * seats (sum exact to 1e-9, each within 2% for mild asymmetry). The derived utilisation is
 * 0.0284; ARCHING_DESIGN's 0.036 is the arithmetic for five columns, i.e. a one-cell-wider hole.
 * The derived figure is asserted.
 *
 * Kern-edge moment, zero tension and beam-theory agreement survive slice 3's thrust; "compression
 * governs" would not, so it is only printed. Gate 1 (no geometry, as the fuzzers emit) must route
 * bit-identically to today. Gate 3 (inside the kern) uses an independently built moment so it
 * catches a cap missing its min. No ticking world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureSpannedHoleTest,
	"DestructionGame.Core.Structure.AHoleWiderThanOneBrickIsSpanned",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureSpannedHoleTest::RunTest(const FString& Parameters)
{
	using namespace StructureArchingTestSupport;
	using namespace StructureSpannedHoleTestSupport;
	using namespace StaircaseWallTestSupport;

	// Pin the profile values the expectations were derived against.
	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against mean f_x1 = 0.7 MPa (re-anchor 2026-08-13), the profile carries %g"),
			GeneralPurposeMortar.TensileStrengthMPa),
		GeneralPurposeMortar.TensileStrengthMPa == 0.7);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against compressive 10 MPa, the profile carries %g"),
			GeneralPurposeMortar.CompressiveStrengthMPa),
		GeneralPurposeMortar.CompressiveStrengthMPa == 10.0);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against clay brick at 1.9 g/cm3, the profile carries %g"),
			ClayBrick.DensityGramsPerCubicCm),
		ClayBrick.DensityGramsPerCubicCm == 1.9);

	/*
	 * Part 1: the intact wall. Measure the column on each group brick before the cut; the bond
	 * above is unchanged by it, so these columns are what the springings must carry.
	 */
	FBrickLayout Intact;

	if (!RunningBond(ArchWallSpec(), Intact) || Intact.Boxes.Num() != ArchWallPieceCount)
	{
		AddError(FString::Printf(
			TEXT("FIXTURE: a flush 7 x 30 wall should lay as %d pieces, got %d"),
			ArchWallPieceCount, Intact.Boxes.Num()));

		return true;
	}

	TestTrue(
		TEXT("FIXTURE: the laid wall must know where every piece and every joint is, or every "
			 "moment below is silently zero and this measures nothing"),
		Intact.Structure.HasCompleteGeometry());

	Intact.Structure.SolveLoads();

	const double GroupBrickXCm[4] =
	{
		LeftSpringingBrickXCm, LeftHangingBrickXCm, RightHangingBrickXCm, RightSpringingBrickXCm
	};

	double ExpectedSpringingSumUu = 0.0;
	bool bColumnsMeasured = true;

	for (const double BrickXCm : GroupBrickXCm)
	{
		const int32 Brick = StaircasePieceAt(
			Intact.Boxes, BrickXCm, ArchWallCourseZCm(SpannedCourse));

		if (Brick == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("FIXTURE: the intact wall has no brick at x %g in course %d"),
				BrickXCm, SpannedCourse));

			bColumnsMeasured = false;
			continue;
		}

		// Two seats centred under the brick, so e = 0 and no moment.
		const int32 Seats = IntactSeatsUnder(Intact.Structure, Brick);

		TestEqual(
			FString::Printf(
				TEXT("FIXTURE: the intact brick at x %g must rest on two seats, it rests on %d"),
				BrickXCm, Seats),
			Seats, 2);

		const double ColumnUu = TotalCarriedUu(Intact.Structure, Brick);

		AddInfo(FString::Printf(
			TEXT("INTACT: the column standing on the brick at x %g is %s uu (%.4f brick weights)"),
			BrickXCm, *Bits(ColumnUu), ColumnUu / BrickWeightUu));

		ExpectedSpringingSumUu += ColumnUu;
	}

	if (!bColumnsMeasured)
	{
		return true;
	}

	/*
	 * Each springing: half the group's load on a 105.0625 cm2 patch at the kern edge, so peak
	 * compression is twice the mean: 2 * (F / (105.0625 * 10000)) / 10 MPa. Halving follows the
	 * fixture's symmetry; the 2% tolerance covers mild asymmetry. The sum is exact.
	 */
	const double ExpectedSpringingForceUu = ExpectedSpringingSumUu / 2.0;

	const double ExpectedSpringingUtilisation =
		2.0 * (ExpectedSpringingForceUu / (HalfSeatAreaSqCm * ForceUnitsPerMPaPerSqCm))
			/ GeneralPurposeMortar.CompressiveStrengthMPa;

	constexpr double SpringingTolerance = 0.02;

	AddInfo(FString::Printf(
		TEXT("DERIVED: the group's four columns come to %s uu, so each springing carries %s uu ")
		TEXT("(%.4f brick weights) and reads %s. ARCHING_DESIGN.md predicts %g, which is the ")
		TEXT("arithmetic for FIVE columns through two seats — an N-cell cut leaves N-1 unseated ")
		TEXT("bricks and 2 half-seated ones, not N and 2. THE DERIVED FIGURE IS WHAT IS ASSERTED."),
		*Bits(ExpectedSpringingSumUu), *Bits(ExpectedSpringingForceUu),
		ExpectedSpringingForceUu / BrickWeightUu, *Bits(ExpectedSpringingUtilisation),
		DesignSpringingUtilisation));

	// Part 2: the three-cell cut.

	FBrickLayout Cut;

	if (!RunningBond(ArchWallSpec(), Cut) || Cut.Boxes.Num() != ArchWallPieceCount)
	{
		AddError(TEXT("FIXTURE: the second copy of the wall did not lay"));
		return true;
	}

	TArray<int32> DeletedPieces;

	for (int32 Cell = 0; Cell < CutCellCount; ++Cell)
	{
		const double BrickXCm = ArchWallOddBrickXCm(FirstCutBrickIndex + Cell);
		const int32 Piece = StaircasePieceAt(Cut.Boxes, BrickXCm, ArchWallCourseZCm(CutCourse));

		if (Piece == INDEX_NONE || !Cut.Structure.RemovePiece(Piece))
		{
			AddError(FString::Printf(
				TEXT("FIXTURE: there should be a brick at x %g in course %d to delete"),
				BrickXCm, CutCourse));

			return true;
		}

		DeletedPieces.Add(Piece);
	}

	Cut.Structure.SolveLoads();

	// The shape the cut leaves, checked against the producer.

	const int32 LeftSpringingBrick = StaircasePieceAt(
		Cut.Boxes, LeftSpringingBrickXCm, ArchWallCourseZCm(SpannedCourse));
	const int32 RightSpringingBrick = StaircasePieceAt(
		Cut.Boxes, RightSpringingBrickXCm, ArchWallCourseZCm(SpannedCourse));
	const int32 LeftHangingBrick = StaircasePieceAt(
		Cut.Boxes, LeftHangingBrickXCm, ArchWallCourseZCm(SpannedCourse));
	const int32 RightHangingBrick = StaircasePieceAt(
		Cut.Boxes, RightHangingBrickXCm, ArchWallCourseZCm(SpannedCourse));

	if (LeftSpringingBrick == INDEX_NONE || RightSpringingBrick == INDEX_NONE
		|| LeftHangingBrick == INDEX_NONE || RightHangingBrick == INDEX_NONE)
	{
		AddError(TEXT("FIXTURE: the four bricks over the hole were not all found"));
		return true;
	}

	// The middle pair have no seat at all; this is what differs from a one-brick hole.
	for (const int32 Hanger : { LeftHangingBrick, RightHangingBrick })
	{
		const int32 Seats = IntactSeatsUnder(Cut.Structure, Hanger);

		TestEqual(
			FString::Printf(
				TEXT("FIXTURE: the brick at x %g must be left with NO seat at all, it has %d"),
				Cut.Boxes[Hanger].CentreCm.X, Seats),
			Seats, 0);
	}

	// The four are chained by intact head joints, forming one group.
	const int32 GroupChain[4] =
	{
		LeftSpringingBrick, LeftHangingBrick, RightHangingBrick, RightSpringingBrick
	};

	for (int32 Link = 0; Link + 1 < 4; ++Link)
	{
		const int32 Head = JointBetweenPieces(
			Cut.Structure, GroupChain[Link], GroupChain[Link + 1]);

		TestTrue(
			FString::Printf(
				TEXT("FIXTURE: pieces %d and %d must share an INTACT head joint or they are not ")
				TEXT("one group"),
				GroupChain[Link], GroupChain[Link + 1]),
			Head != INDEX_NONE
				&& Cut.Structure.GetJointRole(Head, GroupChain[Link]) == EJointRole::Head
				&& !Cut.Structure.GetConnection(Head).HasGiven());
	}

	/*
	 * Trap 3 is live: the springing's eccentric-side neighbour is a hanger resting on it, which
	 * is why slice 1 refuses the arch and slice 2's grouping is needed.
	 */
	{
		int32 SpringingsTheHangerRestsOn = 0;

		for (int32 Joint = 0; Joint < Cut.Structure.NumConnections(); ++Joint)
		{
			const FConnection& Connection = Cut.Structure.GetConnection(Joint);

			if (Connection.HasGiven())
			{
				continue;
			}

			const bool bTouchesHanger =
				Connection.PieceA == LeftHangingBrick || Connection.PieceB == LeftHangingBrick;
			const bool bTouchesSpringing =
				Connection.PieceA == LeftSpringingBrick || Connection.PieceB == LeftSpringingBrick;

			if (bTouchesHanger && bTouchesSpringing)
			{
				++SpringingsTheHangerRestsOn;
			}
		}

		TestEqual(
			FString::Printf(
				TEXT("FIXTURE: the hanger and the springing must share exactly one intact joint — ")
				TEXT("the head joint slice 1 refuses to arch against; they share %d"),
				SpringingsTheHangerRestsOn),
			SpringingsTheHangerRestsOn, 1);
	}

	/*
	 * Part 3: nothing is left unrouted. Without grouping the hangers are stranded as a knot and
	 * the bricks above read Falling. Removed pieces are skipped, since GetPieceSupport reports
	 * them as Falling.
	 */
	{
		int32 Stranded = 0;
		int32 Falling = 0;
		int32 FirstUnrouted = INDEX_NONE;

		for (int32 Piece = 0; Piece < Cut.Structure.NumPieces(); ++Piece)
		{
			if (Cut.Structure.IsPieceRemoved(Piece))
			{
				continue;
			}

			const EPieceSupport Support = Cut.Structure.GetPieceSupport(Piece);

			if (Support == EPieceSupport::Stranded)
			{
				++Stranded;
			}
			else if (Support == EPieceSupport::Falling)
			{
				++Falling;
			}
			else
			{
				continue;
			}

			if (FirstUnrouted == INDEX_NONE)
			{
				FirstUnrouted = Piece;
			}
		}

		AddInfo(FString::Printf(
			TEXT("after a three-cell cut %d of %d pieces are Stranded and %d are Falling; the ")
			TEXT("first is piece %d"),
			Stranded, Cut.Structure.NumPieces() - DeletedPieces.Num(), Falling, FirstUnrouted));

		TestEqual(
			FString::Printf(
				TEXT("a spanned hole leaves NO piece stranded in a knot; %d are"), Stranded),
			Stranded, 0);

		TestEqual(
			FString::Printf(
				TEXT("and none of the wall it did not delete falling; %d are"), Falling),
			Falling, 0);

		TestTrue(
			FString::Printf(
				TEXT("the two bricks with no seat must be routed to the ground through the group, ")
				TEXT("not left hanging — they read %d and %d (Supported is %d)"),
				static_cast<int32>(Cut.Structure.GetPieceSupport(LeftHangingBrick)),
				static_cast<int32>(Cut.Structure.GetPieceSupport(RightHangingBrick)),
				static_cast<int32>(EPieceSupport::Supported)),
			Cut.Structure.GetPieceSupport(LeftHangingBrick) == EPieceSupport::Supported
				&& Cut.Structure.GetPieceSupport(RightHangingBrick) == EPieceSupport::Supported);
	}

	// Part 4: the two springings, where the group's load leaves.

	struct FSpringingCase
	{
		const TCHAR* Description;
		int32 Brick;
		double SeatXCm;

		/** Overhang direction into the hole: +1 is toward increasing X. */
		double EccentricSign;
	};

	const TArray<FSpringingCase> Springings = {
		{ TEXT("the LEFT springing"), LeftSpringingBrick, LeftSpringingSeatXCm, +1.0 },
		{ TEXT("the RIGHT springing"), RightSpringingBrick, RightSpringingSeatXCm, -1.0 },
	};

	double MeasuredSpringingSumUu = 0.0;

	for (const FSpringingCase& Case : Springings)
	{
		const int32 Seat = StaircasePieceAt(
			Cut.Boxes, Case.SeatXCm, ArchWallCourseZCm(CutCourse));

		const int32 Springing = TheOneSeatUnder(Cut.Structure, Case.Brick);

		if (Seat == INDEX_NONE || Springing == INDEX_NONE
			|| Springing != JointBetweenPieces(Cut.Structure, Case.Brick, Seat))
		{
			AddError(FString::Printf(
				TEXT("%s: FIXTURE: it must rest on EXACTLY ONE bed joint, and on the brick at x %g"),
				Case.Description, Case.SeatXCm));

			continue;
		}

		const FConnection& Bed = Cut.Structure.GetConnection(Springing);

		// Slice 1's half seat: 10.25 x 10.25, loaded 5.625 cm off centre against a 1.7083 cm kern.
		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the surviving seat should be %g cm2 with half-extents (%g, %g); ")
				TEXT("MakeInterface emitted %g cm2 with (%g, %g)"),
				Case.Description, HalfSeatAreaSqCm, HalfSeatHalfExtentCm, HalfSeatHalfExtentCm,
				Bed.InterfaceAreaSqCm,
				Bed.InterfaceHalfExtentCm.X, Bed.InterfaceHalfExtentCm.Y),
			FMath::IsNearlyEqual(Bed.InterfaceAreaSqCm, HalfSeatAreaSqCm, 1.0e-9)
				&& FMath::IsNearlyEqual(Bed.InterfaceHalfExtentCm.X, HalfSeatHalfExtentCm, 1.0e-9)
				&& FMath::IsNearlyEqual(Bed.InterfaceHalfExtentCm.Y, HalfSeatHalfExtentCm, 1.0e-9));

		const double EccentricityCm =
			Cut.Boxes[Case.Brick].CentreCm.X - Bed.InterfaceCentreCm.X;

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: it should overhang its seat by %g cm INTO the hole, it ")
				TEXT("overhangs %g"),
				Case.Description, HalfSeatEccentricityCm, EccentricityCm),
			FMath::IsNearlyEqual(
				EccentricityCm, Case.EccentricSign * HalfSeatEccentricityCm, 1.0e-9));

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: %g cm of eccentricity must be OUTSIDE the kern's %g cm, or no ")
				TEXT("part of the face is opening and there is nothing to relieve"),
				Case.Description, FMath::Abs(EccentricityCm),
				KernFromHalfExtentCm(HalfSeatHalfExtentCm)),
			FMath::Abs(EccentricityCm) > KernFromHalfExtentCm(HalfSeatHalfExtentCm));

		const FVector ForceUu = Cut.Structure.GetConnectionForce(Springing);
		const FVector MomentUuCm = Cut.Structure.GetConnectionMoment(Springing);

		const double Utilisation = Cut.Structure.GetConnectionUtilisation(Springing);

		const FBedJointReading Published = ReadBedJoint(
			ForceUu, MomentUuCm, Bed.InterfaceHalfExtentCm, Bed.InterfaceAreaSqCm,
			GeneralPurposeMortar);

		const double Arched = ArchedUtilisation(Published, GeneralPurposeMortar);

		MeasuredSpringingSumUu += FMath::Abs(ForceUu.Z);

		AddInfo(FString::Printf(
			TEXT("%s: joint %d carries %s uu (%.4f brick weights) and publishes %s uu.cm; reads ")
			TEXT("%s (tension %s, compression %s, shear %s); an arch reads %s, the derivation ")
			TEXT("above says %s"),
			Case.Description, Springing, *Bits(FMath::Abs(ForceUu.Z)),
			FMath::Abs(ForceUu.Z) / BrickWeightUu, *Bits(MomentUuCm.Size()), *Bits(Utilisation),
			*Bits(Published.TensionUtilisation), *Bits(Published.CompressionUtilisation),
			*Bits(Published.ShearUtilisation), *Bits(Arched),
			*Bits(ExpectedSpringingUtilisation)));

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the seat must be in COMPRESSION, sigma_n is %s MPa"),
				Case.Description, *Bits(Published.NormalStressMPa)),
			Published.NormalStressMPa < 0.0);

		/*
		 * The published moment sits on the kern edge, so the capped moment is both what travels
		 * and what the joint reads. Slice 3's thrust is shear and does not change this.
		 */
		TestTrue(
			FString::Printf(
				TEXT("%s: the moment it publishes must sit on the kern edge — sigma_b %s against ")
				TEXT("|sigma_n| %s"),
				Case.Description, *Bits(Published.BendingStressMPa),
				*Bits(FMath::Abs(Published.NormalStressMPa))),
			FMath::Abs(Published.BendingStressMPa - FMath::Abs(Published.NormalStressMPa))
				<= 1.0e-12 * FMath::Abs(Published.NormalStressMPa));

		TestTrue(
			FString::Printf(
				TEXT("%s: at the cap the opened edge carries exactly nothing; tension reads %s"),
				Case.Description, *Bits(Published.TensionUtilisation)),
			Published.TensionUtilisation == 0.0);

		// Compared with the oracle's worst axis, which survives shear governing at slice 3.
		TestTrue(
			FString::Printf(
				TEXT("%s: it must read what beam theory says, %s, and it reads %s"),
				Case.Description, *Bits(Published.Worst), *Bits(Utilisation)),
			FMath::Abs(Utilisation - Published.Worst)
				<= 1.0e-12 * FMath::Max(Published.Worst, 1.0e-12));

		// Not the moment zeroed, which would read |sigma_n| instead of 2|sigma_n|.
		TestTrue(
			FString::Printf(
				TEXT("%s: the compression axis must read 2|sigma_n|/f_c = %s, not the %s a deleted ")
				TEXT("moment gives; it reads %s"),
				Case.Description, *Bits(Arched),
				*Bits(FMath::Abs(Published.NormalStressMPa)
					/ GeneralPurposeMortar.CompressiveStrengthMPa),
				*Bits(Published.CompressionUtilisation)),
			FMath::Abs(Published.CompressionUtilisation - Arched) <= 1.0e-12 * Arched);

		// Against the load the intact wall says must pass here.
		TestTrue(
			FString::Printf(
				TEXT("%s: the group's four columns through two seats put %s on this joint, it ")
				TEXT("reads %s"),
				Case.Description, *Bits(ExpectedSpringingUtilisation),
				*Bits(Published.CompressionUtilisation)),
			FMath::Abs(Published.CompressionUtilisation - ExpectedSpringingUtilisation)
				<= SpringingTolerance * ExpectedSpringingUtilisation);
	}

	/*
	 * Conservation, independent of how slice 2 divides the load: the two springing forces must sum
	 * to the intact wall's four columns (same arithmetic above the course, so exact).
	 */
	AddInfo(FString::Printf(
		TEXT("the two springings carry %s uu between them; the intact wall's four columns come to ")
		TEXT("%s uu"),
		*Bits(MeasuredSpringingSumUu), *Bits(ExpectedSpringingSumUu)));

	TestTrue(
		FString::Printf(
			TEXT("everything over the hole must leave through the two springings: %s uu against ")
			TEXT("the intact wall's %s uu"),
			*Bits(MeasuredSpringingSumUu), *Bits(ExpectedSpringingSumUu)),
		FMath::Abs(MeasuredSpringingSumUu - ExpectedSpringingSumUu)
			<= 1.0e-9 * ExpectedSpringingSumUu);

	/*
	 * Part 5: the wall stands. Counted by break pass, not HasGiven: joints removed with a piece
	 * have HasGiven true and pass INDEX_NONE.
	 */
	const int32 BreakingPasses = Cut.Structure.SolveAndBreak();

	int32 JointsBrokenByLoad = 0;
	int32 JointsGoneWithTheCut = 0;

	for (int32 Joint = 0; Joint < Cut.Structure.NumConnections(); ++Joint)
	{
		if (!Cut.Structure.GetConnection(Joint).HasGiven())
		{
			continue;
		}

		if (Cut.Structure.GetBreakPass(Joint) != INDEX_NONE)
		{
			++JointsBrokenByLoad;
		}
		else
		{
			++JointsGoneWithTheCut;
		}
	}

	AddInfo(FString::Printf(
		TEXT("after a three-cell cut the cascade ran %d passes; %d of %d joints failed under load ")
		TEXT("and %d went with the three deleted bricks"),
		BreakingPasses, JointsBrokenByLoad, Cut.Structure.NumConnections(), JointsGoneWithTheCut));

	TestEqual(
		FString::Printf(
			TEXT("a three-cell interior cut must break NOTHING; the cascade ran %d passes"),
			BreakingPasses),
		BreakingPasses, 0);

	TestEqual(
		FString::Printf(
			TEXT("and must leave every joint it did not delete intact; %d failed under load"),
			JointsBrokenByLoad),
		JointsBrokenByLoad, 0);

	TestEqual(
		FString::Printf(
			TEXT("only the three deleted bricks' own %d joints may leave the graph; %d did"),
			JointsLostWithTheCut, JointsGoneWithTheCut),
		JointsGoneWithTheCut, JointsLostWithTheCut);

	/*
	 * Part 6, gate 1: a geometry-free structure routes bit-identically.
	 *
	 *      [ A ][ B ][ C ][ D ]      four pieces joined head to head
	 *      [pad]          [pad]      A and D bedded to the earth
	 *
	 * The spanned-hole topology with no positions. B and C prop each other and stay Stranded,
	 * carrying nothing. The fuzz generators emit geometry-free structures against an oracle with
	 * no arches, so grouping must not fire here. Exact equality throughout.
	 */
	{
		FStructure Structure;

		const int32 LeftPad = Structure.AddPiece(BrickMassKg, true);
		const int32 A = Structure.AddPiece(BrickMassKg, false);
		const int32 B = Structure.AddPiece(BrickMassKg, false);
		const int32 C = Structure.AddPiece(BrickMassKg, false);
		const int32 D = Structure.AddPiece(BrickMassKg, false);
		const int32 RightPad = Structure.AddPiece(BrickMassKg, true);

		const auto AddBed = [&Structure](int32 Below, int32 Above) -> int32
		{
			FConnection Bed;
			Bed.PieceA = Below;
			Bed.PieceB = Above;
			Bed.InterfaceNormal = FVector::ZAxisVector;
			Bed.InterfaceAreaSqCm = HalfSeatAreaSqCm;
			Bed.Strength = GeneralPurposeMortar;
			return Structure.AddConnection(Bed);
		};

		const auto AddHead = [&Structure](int32 First, int32 Second) -> int32
		{
			FConnection Head;
			Head.PieceA = First;
			Head.PieceB = Second;
			Head.InterfaceNormal = FVector::XAxisVector;
			Head.InterfaceAreaSqCm = BrickWidthCm * BrickHeightCm;
			Head.Strength = GeneralPurposeMortar;
			return Structure.AddConnection(Head);
		};

		const int32 LeftSeat = AddBed(LeftPad, A);
		const int32 RightSeat = AddBed(RightPad, D);

		const int32 HeadAB = AddHead(A, B);
		const int32 HeadBC = AddHead(B, C);
		const int32 HeadCD = AddHead(C, D);

		TestTrue(
			TEXT("GATE 1: FIXTURE: nobody's position is known, so HasCompleteGeometry must be false"),
			!Structure.HasCompleteGeometry());

		Structure.SolveLoads();

		AddInfo(FString::Printf(
			TEXT("GATE 1: A %d, B %d, C %d, D %d; the seats carry %s and %s uu, the head joints ")
			TEXT("%s, %s and %s uu"),
			static_cast<int32>(Structure.GetPieceSupport(A)),
			static_cast<int32>(Structure.GetPieceSupport(B)),
			static_cast<int32>(Structure.GetPieceSupport(C)),
			static_cast<int32>(Structure.GetPieceSupport(D)),
			*Bits(Structure.GetConnectionForce(LeftSeat).Size()),
			*Bits(Structure.GetConnectionForce(RightSeat).Size()),
			*Bits(Structure.GetConnectionForce(HeadAB).Size()),
			*Bits(Structure.GetConnectionForce(HeadBC).Size()),
			*Bits(Structure.GetConnectionForce(HeadCD).Size())));

		TestTrue(
			TEXT("GATE 1: the two seated pieces must still be Supported"),
			Structure.GetPieceSupport(A) == EPieceSupport::Supported
				&& Structure.GetPieceSupport(D) == EPieceSupport::Supported);

		TestTrue(
			FString::Printf(
				TEXT("GATE 1: with no geometry there is no group, so the mutually-propping pair ")
				TEXT("must STILL be Stranded — they read %d and %d (Stranded is %d)"),
				static_cast<int32>(Structure.GetPieceSupport(B)),
				static_cast<int32>(Structure.GetPieceSupport(C)),
				static_cast<int32>(EPieceSupport::Stranded)),
			Structure.GetPieceSupport(B) == EPieceSupport::Stranded
				&& Structure.GetPieceSupport(C) == EPieceSupport::Stranded);

		// Each seat carries exactly its own brick; head joints carry exactly zero.
		TestTrue(
			FString::Printf(
				TEXT("GATE 1: each seat must carry exactly one brick weight, %s uu; they carry %s ")
				TEXT("and %s"),
				*Bits(BrickWeightUu),
				*Bits(FMath::Abs(Structure.GetConnectionForce(LeftSeat).Z)),
				*Bits(FMath::Abs(Structure.GetConnectionForce(RightSeat).Z))),
			FMath::Abs(Structure.GetConnectionForce(LeftSeat).Z) == BrickWeightUu
				&& FMath::Abs(Structure.GetConnectionForce(RightSeat).Z) == BrickWeightUu);

		for (const int32 Head : { HeadAB, HeadBC, HeadCD })
		{
			TestTrue(
				FString::Printf(
					TEXT("GATE 1: head joint %d must carry exactly nothing, it carries %s uu"),
					Head, *Bits(Structure.GetConnectionForce(Head).Size())),
				Structure.GetConnectionForce(Head) == FVector::ZeroVector);

			TestTrue(
				FString::Printf(
					TEXT("GATE 1: head joint %d must carry no moment either, it carries %s uu.cm"),
					Head, *Bits(Structure.GetConnectionMoment(Head).Size())),
				Structure.GetConnectionMoment(Head) == FVector::ZeroVector);
		}

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			const double Utilisation = Structure.GetConnectionUtilisation(Joint);

			TestTrue(
				FString::Printf(
					TEXT("GATE 1: joint %d must read a finite utilisation, it reads %s"),
					Joint, *Bits(Utilisation)),
				FMath::IsFinite(Utilisation));
		}
	}

	/*
	 * Part 7, gate 3: inside the kern. A brick 1 cm off its pad loads a 20.5 cm patch 0.5 cm off
	 * centre against a 3.4167 cm kern, so min(1, |sigma_n|/sigma_b) = 1 and the arch must not
	 * fire. The expected moment is rebuilt from the 0.5 cm lever arm, not GetConnectionMoment, so
	 * the oracle cannot move with a defect. A cap missing its min would read 2.5387e-4 instead of
	 * 1.4551e-4.
	 */
	{
		FStructure Structure;

		constexpr double PadZCm = BrickHeightCm / 2.0;
		constexpr double UpperZCm = PadZCm + CoursePitchCm;
		constexpr double OffsetCm = 1.0;

		const FPieceBox PadBox = BrickBoxAt(0.0, 0.0, PadZCm);
		const FPieceBox LoadedBox = BrickBoxAt(OffsetCm, 0.0, UpperZCm);
		const FPieceBox NeighbourBox = BrickBoxAt(OffsetCm + BrickPitchCm, 0.0, UpperZCm);
		const FPieceBox FarPadBox = BrickBoxAt(OffsetCm + BrickPitchCm, 0.0, PadZCm);

		const int32 Pad = Structure.AddPiece(BrickMassKg, true, PadBox.CentreCm);
		const int32 Loaded = Structure.AddPiece(BrickMassKg, false, LoadedBox.CentreCm);
		const int32 FarPad = Structure.AddPiece(BrickMassKg, true, FarPadBox.CentreCm);
		const int32 Neighbour = Structure.AddPiece(BrickMassKg, false, NeighbourBox.CentreCm);

		FConnection Bed;
		MakeInterface(Pad, PadBox, Loaded, LoadedBox, MortarJointCm, GeneralPurposeMortar, Bed);
		const int32 Subject = Structure.AddConnection(Bed);

		FConnection FarBed;
		MakeInterface(
			FarPad, FarPadBox, Neighbour, NeighbourBox,
			MortarJointCm, GeneralPurposeMortar, FarBed);
		Structure.AddConnection(FarBed);

		FConnection Head;
		MakeInterface(
			Loaded, LoadedBox, Neighbour, NeighbourBox,
			MortarJointCm, GeneralPurposeMortar, Head);
		Structure.AddConnection(Head);

		if (Subject == INDEX_NONE)
		{
			AddError(TEXT("GATE 3: FIXTURE: the bed joint was refused"));
			return true;
		}

		Structure.SolveLoads();

		const FConnection& Joint = Structure.GetConnection(Subject);
		const FVector ForceUu = Structure.GetConnectionForce(Subject);

		const double EccentricityCm = LoadedBox.CentreCm.X - Joint.InterfaceCentreCm.X;

		TestTrue(
			FString::Printf(
				TEXT("GATE 3: FIXTURE: %g cm of eccentricity must be INSIDE the kern's %g cm, or ")
				TEXT("this row is measuring the arch instead of refusing it"),
				EccentricityCm, KernFromHalfExtentCm(Joint.InterfaceHalfExtentCm.X)),
			FMath::Abs(EccentricityCm) < KernFromHalfExtentCm(Joint.InterfaceHalfExtentCm.X));

		/*
		 * One brick weight through the eccentricity, about Y. The 20.5 x 10.25 patch's moduli
		 * differ by 4x, so a wrong axis would show.
		 */
		const FVector ExpectedMomentUuCm(
			0.0, FMath::Abs(ForceUu.Z) * FMath::Abs(EccentricityCm), 0.0);

		const FBedJointReading Expected = ReadBedJoint(
			ForceUu, ExpectedMomentUuCm, Joint.InterfaceHalfExtentCm, Joint.InterfaceAreaSqCm,
			GeneralPurposeMortar);

		const double Utilisation = Structure.GetConnectionUtilisation(Subject);
		const double IfScaledWithoutTheMin = ArchedUtilisation(Expected, GeneralPurposeMortar);

		AddInfo(FString::Printf(
			TEXT("GATE 3: the joint carries %s uu at %g cm, so sigma_n %s and sigma_b %s MPa — a ")
			TEXT("ratio of %s. It must read %s; scaling without the min reads %s. It reads %s"),
			*Bits(FMath::Abs(ForceUu.Z)), EccentricityCm,
			*Bits(Expected.NormalStressMPa), *Bits(Expected.BendingStressMPa),
			*Bits(FMath::Abs(Expected.NormalStressMPa) / Expected.BendingStressMPa),
			*Bits(Expected.Worst), *Bits(IfScaledWithoutTheMin), *Bits(Utilisation)));

		TestTrue(
			FString::Printf(
				TEXT("GATE 3: FIXTURE: inside the kern |sigma_n| exceeds sigma_b, so an ")
				TEXT("unconditional scale would MULTIPLY the bending stress — %s against %s"),
				*Bits(FMath::Abs(Expected.NormalStressMPa)), *Bits(Expected.BendingStressMPa)),
			FMath::Abs(Expected.NormalStressMPa) > Expected.BendingStressMPa);

		TestTrue(
			FString::Printf(
				TEXT("GATE 3: the arch must NOT fire — the fixture's own %g cm arm says %s, the ")
				TEXT("joint reads %s"),
				EccentricityCm, *Bits(Expected.Worst), *Bits(Utilisation)),
			FMath::Abs(Utilisation - Expected.Worst)
				<= 1.0e-12 * FMath::Max(Expected.Worst, 1.0e-12));

		TestTrue(
			FString::Printf(
				TEXT("GATE 3: and specifically it must not read the %s an unconditional scale ")
				TEXT("gives; it reads %s"),
				*Bits(IfScaledWithoutTheMin), *Bits(Utilisation)),
			FMath::Abs(Utilisation - IfScaledWithoutTheMin)
				> 1.0e-6 * IfScaledWithoutTheMin);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
