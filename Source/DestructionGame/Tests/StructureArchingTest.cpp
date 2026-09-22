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
 * A brick that lost one of its two bed seats but abuts a neighbour that reaches the ground on its
 * own arches over the hole rather than cantilevering (ARCHING_DESIGN.md). Without this, deleting
 * one brick started a stepping-triangle failure up the wall.
 *
 * With one seat (N = 1) the moment is computed: area halves, section modulus quarters, and
 * e = 5.625 cm. Per brick weight the joint reads 0.0083148340 of mean f_x1 (1.0 at 120.27 brick
 * weights). Arching applies when the joint is BedBeneath with complete geometry, compressive,
 * eccentric beyond the kern (1.7083 cm on a 10.25 cm patch), with an intact head joint on the
 * eccentric side to a neighbour that reaches the ground without this piece. The cap is
 * k = min(1, |sigma_n|/sigma_b): peak tension 0, peak compression 2|sigma_n|.
 *
 * Asserts utilisation and break counts, never displacement. The arched answer is checked against
 * 2|sigma_n|/f_c from the solver's force and against the design's 0.0142166. The cantilever
 * figure is rebuilt from the fixture's 5.625 cm arm, since GetConnectionMoment publishes the
 * capped moment. Governing axis moves from tension to compression under the cap; shear is zero.
 * Each of the four gates has a negative row, plus regression anchors (the staircase is the
 * direction check). World-free.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureArchingTest,
	"DestructionGame.Core.Structure.AMissingBrickIsBridgedNotCantilevered",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureArchingTest::RunTest(const FString& Parameters)
{
	using namespace StructureArchingTestSupport;
	using namespace StaircaseWallTestSupport;

	// Expected numbers were derived against these profile values; asserted rather than imported.
	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against mean f_x1 = 0.7 MPa, the profile carries %g"),
			GeneralPurposeMortar.TensileStrengthMPa),
		GeneralPurposeMortar.TensileStrengthMPa == 0.7);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against compressive 10 MPa, the profile carries %g"),
			GeneralPurposeMortar.CompressiveStrengthMPa),
		GeneralPurposeMortar.CompressiveStrengthMPa == 10.0);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against mean cohesion 0.9 MPa, the profile carries %g"),
			GeneralPurposeMortar.ShearCohesionMPa),
		GeneralPurposeMortar.ShearCohesionMPa == 0.9);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against mean friction 0.75, the profile carries %g"),
			GeneralPurposeMortar.FrictionCoefficient),
		GeneralPurposeMortar.FrictionCoefficient == 0.75);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against clay brick at 1.9 g/cm3, the profile carries %g"),
			ClayBrick.DensityGramsPerCubicCm),
		ClayBrick.DensityGramsPerCubicCm == 1.9);

	// Part 1: the intact wall must not change by one bit.

	/*
	 * Exact ==, not a tolerance: the cascade fuzz has joints settling at 1 - 1ulp, so a last-bit
	 * drift matters. No gate is true in an intact wall, so the arch code must be unreached here.
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

	double IntactWorst = 0.0;
	int32 IntactWorstJoint = INDEX_NONE;

	for (int32 Joint = 0; Joint < Intact.Structure.NumConnections(); ++Joint)
	{
		const double Utilisation = Intact.Structure.GetConnectionUtilisation(Joint);

		if (Utilisation > IntactWorst)
		{
			IntactWorst = Utilisation;
			IntactWorstJoint = Joint;
		}
	}

	AddInfo(FString::Printf(
		TEXT("ANCHOR 1: intact 7 x 30 flush wall, %d pieces and %d joints, worst joint %d at %s"),
		Intact.Structure.NumPieces(), Intact.Structure.NumConnections(),
		IntactWorstJoint, *Bits(IntactWorst)));

	/**
	 * Measured and pinned bit for bit. Lower than ARCHING_DESIGN's 0.00495, which is for the
	 * 40-course wall. With e = 0 everywhere it is compression-governed, so it is invariant under
	 * the mean re-anchor. If it fires, re-measure and re-pin; do not weaken the ==.
	 */
	constexpr double IntactWallWorstUtilisation = 0.0036748258197270385;

	TestTrue(
		FString::Printf(
			TEXT("ANCHOR 1: the intact wall's worst joint must be BIT-IDENTICAL at %s, it reads %s"),
			*Bits(IntactWallWorstUtilisation), *Bits(IntactWorst)),
		IntactWorst == IntactWallWorstUtilisation);

	TestTrue(
		FString::Printf(TEXT("ANCHOR 1: and it must be nowhere near capacity, it reads %s"),
			*Bits(IntactWorst)),
		IntactWorst < 0.01);

	// Part 2: one interior deletion and the two joints it leaves half-seated.

	FBrickLayout Cut;

	if (!RunningBond(ArchWallSpec(), Cut) || Cut.Boxes.Num() != ArchWallPieceCount)
	{
		AddError(TEXT("FIXTURE: the second copy of the wall did not lay"));
		return true;
	}

	const int32 DeletedPiece = StaircasePieceAt(
		Cut.Boxes, DeletedBrickXCm, ArchWallCourseZCm(DeletedBrickCourse));

	if (DeletedPiece == INDEX_NONE || !Cut.Structure.RemovePiece(DeletedPiece))
	{
		AddError(FString::Printf(
			TEXT("FIXTURE: there should be a brick at x %.2f in course %d to delete"),
			DeletedBrickXCm, DeletedBrickCourse));

		return true;
	}

	Cut.Structure.SolveLoads();

	/** One of the two bricks left on half a seat. */
	struct FHalfSeatedCase
	{
		const TCHAR* Description;

		/** X of the half-seated brick and of the seat it kept. */
		double BrickXCm;
		double SeatXCm;

		/** Overhang direction (the eccentric side): +1 is toward increasing X. */
		double EccentricSign;

		/** X of the abutment it leans on through the head joint. */
		double AbutmentXCm;
	};

	const TArray<FHalfSeatedCase> HalfSeated = {
		{
			TEXT("the brick on the LEFT of the hole, overhanging RIGHT into it"),
			LeftHalfSeatedXCm, LeftSurvivingSeatXCm, +1.0, RightHalfSeatedXCm
		},
		{
			TEXT("the brick on the RIGHT of the hole, overhanging LEFT into it"),
			RightHalfSeatedXCm, RightSurvivingSeatXCm, -1.0, LeftHalfSeatedXCm
		},
	};

	/**
	 * ARCHING_DESIGN.md's prediction for 28 brick weights on one 105.0625 cm2 patch:
	 * 2 * (28 * 2667.198625 / (105.0625 * 10000)) / 10 MPa = 0.0142166. The ratio to the
	 * cantilever reading depends on strengths (tension vs. compression); the arched reading is
	 * the invariant. 2% slack covers the wall's real load distribution only; a wrong arm,
	 * modulus, 100x or M = 0 (off by 2x) fall well outside. The tight check is 2|sigma_n|.
	 */
	constexpr double PredictedArchedUtilisation = 0.0142166;
	constexpr double PredictedArchedTolerance = 0.02;

	/** The same joint without the arch, rebuilt from the 5.625 cm arm (not GetConnectionMoment, which is capped). */
	constexpr double PredictedCantileverUtilisation = 0.2328157;

	for (const FHalfSeatedCase& Case : HalfSeated)
	{
		const int32 Brick = StaircasePieceAt(
			Cut.Boxes, Case.BrickXCm, ArchWallCourseZCm(HalfSeatedCourse));
		const int32 Seat = StaircasePieceAt(
			Cut.Boxes, Case.SeatXCm, ArchWallCourseZCm(HalfSeatedCourse - 1));
		const int32 Abutment = StaircasePieceAt(
			Cut.Boxes, Case.AbutmentXCm, ArchWallCourseZCm(HalfSeatedCourse));

		if (Brick == INDEX_NONE || Seat == INDEX_NONE || Abutment == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("FIXTURE: %s — brick %d, seat %d, abutment %d"),
				Case.Description, Brick, Seat, Abutment));

			continue;
		}

		/*
		 * Gate one precondition: exactly one bed joint beneath (N = 1 makes the moment computed).
		 * Skip given joints: GetJointRole still reports BedBeneath for the deleted brick's joint.
		 */
		int32 BedBeneath = INDEX_NONE;
		int32 BedBeneathCount = 0;

		for (int32 Joint = 0; Joint < Cut.Structure.NumConnections(); ++Joint)
		{
			if (Cut.Structure.GetJointRole(Joint, Brick) == EJointRole::BedBeneath
				&& !Cut.Structure.GetConnection(Joint).HasGiven())
			{
				BedBeneath = Joint;
				++BedBeneathCount;
			}
		}

		TestEqual(
			FString::Printf(
				TEXT("%s: FIXTURE: it must rest on EXACTLY ONE bed joint after the deletion, got %d"),
				Case.Description, BedBeneathCount),
			BedBeneathCount, 1);

		if (BedBeneathCount != 1 || BedBeneath != JointBetweenPieces(Cut.Structure, Brick, Seat))
		{
			AddError(FString::Printf(
				TEXT("%s: FIXTURE: the one bed joint beneath should be the one to piece %d"),
				Case.Description, Seat));

			continue;
		}

		const FConnection& Bed = Cut.Structure.GetConnection(BedBeneath);

		// Gate one, other half: the joint's rectangle is the 10.25 x 10.25 half seat.
		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the surviving seat should be %g cm2 with half-extents (%g, %g, 0); ")
				TEXT("MakeInterface emitted %g cm2 with (%g, %g, %g)"),
				Case.Description, HalfSeatAreaSqCm,
				HalfSeatHalfExtentCm, HalfSeatHalfExtentCm,
				Bed.InterfaceAreaSqCm,
				Bed.InterfaceHalfExtentCm.X, Bed.InterfaceHalfExtentCm.Y, Bed.InterfaceHalfExtentCm.Z),
			FMath::IsNearlyEqual(Bed.InterfaceAreaSqCm, HalfSeatAreaSqCm, 1.0e-9)
				&& FMath::IsNearlyEqual(Bed.InterfaceHalfExtentCm.X, HalfSeatHalfExtentCm, 1.0e-9)
				&& FMath::IsNearlyEqual(Bed.InterfaceHalfExtentCm.Y, HalfSeatHalfExtentCm, 1.0e-9)
				&& Bed.InterfaceHalfExtentCm.Z == 0.0);

		// Gate three: 5.625 cm eccentricity on the expected side, beyond the 1.7083 cm kern.
		const double EccentricityCm =
			Cut.Boxes[Brick].CentreCm.X - Bed.InterfaceCentreCm.X;

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: it should overhang its seat by %g cm toward %s, it overhangs %g"),
				Case.Description, HalfSeatEccentricityCm,
				Case.EccentricSign > 0.0 ? TEXT("+X") : TEXT("-X"), EccentricityCm),
			FMath::IsNearlyEqual(EccentricityCm, Case.EccentricSign * HalfSeatEccentricityCm, 1.0e-9));

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: %g cm of eccentricity must be OUTSIDE the kern's %g cm, or ")
				TEXT("no part of the face is opening and there is nothing to relieve"),
				Case.Description, FMath::Abs(EccentricityCm),
				KernFromHalfExtentCm(HalfSeatHalfExtentCm)),
			FMath::Abs(EccentricityCm) > KernFromHalfExtentCm(HalfSeatHalfExtentCm));

		/*
		 * Gate four: an intact head joint on the eccentric side to a neighbour with its own bed
		 * joint, not resting on this piece (trap 3: two unseated bricks propping each other).
		 */
		const int32 HeadJoint = JointBetweenPieces(Cut.Structure, Brick, Abutment);

		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE: there must be a head joint to piece %d"),
				Case.Description, Abutment),
			HeadJoint != INDEX_NONE
				&& Cut.Structure.GetJointRole(HeadJoint, Brick) == EJointRole::Head
				&& !Cut.Structure.GetConnection(HeadJoint).HasGiven());

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the abutment must lie on the ECCENTRIC side — brick at x %g, ")
				TEXT("abutment at x %g"),
				Case.Description, Cut.Boxes[Brick].CentreCm.X, Cut.Boxes[Abutment].CentreCm.X),
			(Cut.Boxes[Abutment].CentreCm.X - Cut.Boxes[Brick].CentreCm.X) * Case.EccentricSign > 0.0);

		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE: the abutment must itself reach the ground"),
				Case.Description),
			Cut.Structure.GetPieceSupport(Abutment) == EPieceSupport::Supported
				|| Cut.Structure.GetPieceSupport(Abutment) == EPieceSupport::Grounded);

		int32 AbutmentSeats = 0;
		bool bAbutmentLeansOnUs = false;

		for (int32 Joint = 0; Joint < Cut.Structure.NumConnections(); ++Joint)
		{
			if (Cut.Structure.GetJointRole(Joint, Abutment) != EJointRole::BedBeneath
				|| Cut.Structure.GetConnection(Joint).HasGiven())
			{
				continue;
			}

			++AbutmentSeats;

			const FConnection& Other = Cut.Structure.GetConnection(Joint);

			if (Other.PieceA == Brick || Other.PieceB == Brick)
			{
				bAbutmentLeansOnUs = true;
			}
		}

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the abutment must have a seat of its own (it has %d) and must ")
				TEXT("NOT be resting on the piece asking for the arch (it %s)"),
				Case.Description, AbutmentSeats,
				bAbutmentLeansOnUs ? TEXT("is") : TEXT("is not")),
			AbutmentSeats >= 1 && !bAbutmentLeansOnUs);

		// Stranded would make every number below a solver limitation.
		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE: the half-seated brick must still be Supported"),
				Case.Description),
			Cut.Structure.GetPieceSupport(Brick) == EPieceSupport::Supported);

		// What the joint reads, and what it must read.

		const FVector ForceUu = Cut.Structure.GetConnectionForce(BedBeneath);
		const FVector MomentUuCm = Cut.Structure.GetConnectionMoment(BedBeneath);

		const double Utilisation = Cut.Structure.GetConnectionUtilisation(BedBeneath);

		// Beam theory on the solver's own published force and moment.
		const FBedJointReading Published = ReadBedJoint(
			ForceUu, MomentUuCm, Bed.InterfaceHalfExtentCm, Bed.InterfaceAreaSqCm,
			GeneralPurposeMortar);

		/*
		 * The cantilever reading, rebuilt from the fixture's 5.625 cm arm, not from
		 * GetConnectionMoment: the joint publishes the capped moment once the arch fires. Only the
		 * force comes from the solver (the cap does not touch it). Moment is about Y for an X
		 * eccentricity. Ignores moment from the courses above, about 0.1% off, inside the 2%.
		 */
		const FVector CantileverMomentUuCm(
			0.0, FMath::Abs(ForceUu.Z) * HalfSeatEccentricityCm, 0.0);

		const FBedJointReading Cantilever = ReadBedJoint(
			ForceUu, CantileverMomentUuCm, Bed.InterfaceHalfExtentCm, Bed.InterfaceAreaSqCm,
			GeneralPurposeMortar);

		const double Arched = ArchedUtilisation(Published, GeneralPurposeMortar);

		AddInfo(FString::Printf(
			TEXT("%s: piece %3d on %3d through joint %3d carries %s uu (%.4f brick weights) and ")
			TEXT("publishes %s uu.cm; sigma_n %s MPa, sigma_b %s MPa (a cantilever's %s uu.cm ")
			TEXT("would be sigma_b %s MPa)"),
			Case.Description, Brick, Seat, BedBeneath,
			*Bits(ForceUu.Size()), ForceUu.Size() / BrickWeightUu,
			*Bits(MomentUuCm.Size()),
			*Bits(Published.NormalStressMPa), *Bits(Published.BendingStressMPa),
			*Bits(CantileverMomentUuCm.Size()), *Bits(Cantilever.BendingStressMPa)));

		AddInfo(FString::Printf(
			TEXT("%s: reads %s; as a cantilever %s (tension %s, compression %s, shear %s); ")
			TEXT("arched %s"),
			Case.Description, *Bits(Utilisation), *Bits(Cantilever.Worst),
			*Bits(Cantilever.TensionUtilisation), *Bits(Cantilever.CompressionUtilisation),
			*Bits(Cantilever.ShearUtilisation), *Bits(Arched)));

		// Gate two: the seat is in compression.
		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE: the seat must be in COMPRESSION, sigma_n is %s MPa"),
				Case.Description, *Bits(Published.NormalStressMPa)),
			Published.NormalStressMPa < 0.0);

		// Shear is zero; tension governs before the cap and compression after.
		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE: shear must be exactly zero, it is %s"),
				Case.Description, *Bits(Published.ShearUtilisation)),
			Published.ShearUtilisation == 0.0);

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: as a cantilever TENSION must govern (%s against compression %s), ")
				TEXT("or the change this test asserts is invisible"),
				Case.Description, *Bits(Cantilever.TensionUtilisation),
				*Bits(Cantilever.CompressionUtilisation)),
			Cantilever.TensionUtilisation > Cantilever.CompressionUtilisation);

		/*
		 * On mean strengths the cantilever no longer breaks (0.2328), so the precondition is the
		 * relief margin: 16.376x, clear of 10x.
		 */
		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the cantilever answer (%s) must dwarf the arched one (%s) or ")
				TEXT("nothing was wrong in the first place"),
				Case.Description, *Bits(Cantilever.Worst), *Bits(Arched)),
			Cantilever.Worst > 10.0 * Arched);

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: ARCHING_DESIGN predicts the cantilever reads %g; beam theory on ")
				TEXT("the solver's own force through the fixture's own 5.625 cm arm says %s"),
				Case.Description, PredictedCantileverUtilisation, *Bits(Cantilever.Worst)),
			FMath::Abs(Cantilever.Worst - PredictedCantileverUtilisation)
				<= PredictedArchedTolerance * PredictedCantileverUtilisation);

		// The claim, tightly: the thrust line sits on the kern edge.
		TestTrue(
			FString::Printf(
				TEXT("%s: the half-seated joint must ARCH — 2|sigma_n|/f_c = %s — and it reads %s ")
				TEXT("(the cantilever answer is %s)"),
				Case.Description, *Bits(Arched), *Bits(Utilisation), *Bits(Cantilever.Worst)),
			FMath::Abs(Utilisation - Arched) <= 1.0e-12 * Arched);

		/*
		 * The published moment is the capped one (sigma_b == |sigma_n|). Catches a solver that
		 * relieves the evaluated stress but passes an unrelieved moment down.
		 */
		TestTrue(
			FString::Printf(
				TEXT("%s: the moment it PUBLISHES must sit on the kern edge — sigma_b %s against ")
				TEXT("|sigma_n| %s"),
				Case.Description, *Bits(Published.BendingStressMPa),
				*Bits(FMath::Abs(Published.NormalStressMPa))),
			FMath::Abs(Published.BendingStressMPa - FMath::Abs(Published.NormalStressMPa))
				<= 1.0e-12 * FMath::Abs(Published.NormalStressMPa));

		// Loosely, against the design document's independent figure.
		TestTrue(
			FString::Printf(
				TEXT("%s: ARCHING_DESIGN predicts %g for this joint, it reads %s"),
				Case.Description, PredictedArchedUtilisation, *Bits(Utilisation)),
			FMath::Abs(Utilisation - PredictedArchedUtilisation)
				<= PredictedArchedTolerance * PredictedArchedUtilisation);

		// Not the moment simply zeroed, which reads |sigma_n|, half the arch's 2|sigma_n|.
		TestTrue(
			FString::Printf(
				TEXT("%s: and it must NOT be the moment simply zeroed — that reads %s, half of ")
				TEXT("the %s an arch reads"),
				Case.Description,
				*Bits(FMath::Abs(Published.NormalStressMPa)
					/ GeneralPurposeMortar.CompressiveStrengthMPa),
				*Bits(Arched)),
			FMath::Abs(Utilisation
				- FMath::Abs(Published.NormalStressMPa)
					/ GeneralPurposeMortar.CompressiveStrengthMPa)
				> 1.0e-6 * Arched);
	}

	/*
	 * The wall stands: one interior deletion must break nothing. SolveLoads is non-destructive, so
	 * the same structure is reused. Counted by break pass, not HasGiven: joints removed with the
	 * brick have HasGiven true but pass INDEX_NONE.
	 */
	const int32 BreakingPasses = Cut.Structure.SolveAndBreak();

	int32 JointsBrokenByLoad = 0;
	int32 JointsGoneWithTheBrick = 0;

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
			++JointsGoneWithTheBrick;
		}
	}

	AddInfo(FString::Printf(
		TEXT("after one interior deletion the cascade ran %d passes; %d of %d joints failed under ")
		TEXT("load and %d went with the deleted brick"),
		BreakingPasses, JointsBrokenByLoad, Cut.Structure.NumConnections(), JointsGoneWithTheBrick));

	TestEqual(
		FString::Printf(
			TEXT("one interior deletion must break NOTHING; the cascade ran %d passes"),
			BreakingPasses),
		BreakingPasses, 0);

	TestEqual(
		FString::Printf(
			TEXT("one interior deletion must leave every joint it did not delete intact; %d failed ")
			TEXT("under load"),
			JointsBrokenByLoad),
		JointsBrokenByLoad, 0);

	// The deleted brick's own six joints: two beneath, two above, two head joints.
	TestEqual(
		FString::Printf(
			TEXT("only the deleted brick's own six joints may leave the graph; %d did"),
			JointsGoneWithTheBrick),
		JointsGoneWithTheBrick, 6);

	// Part 3: each of the four gates, with a case that must not arch.

	/**
	 * A fixture that fails exactly one gate. Each row asserts the joint reads plain beam theory on
	 * the solver's force and moment; a wrongly fired arch reads 2|sigma_n|/f_c instead.
	 */
	struct FGateCase
	{
		const TCHAR* Description;

		/** Builds the structure and returns the subject bed joint. */
		TFunction<int32(FStructure&)> Build;

		bool bExpectCompleteGeometry;
	};

	// Shared shape: grounded pad, a piece on it, and an intact Supported neighbour on the eccentric side.
	constexpr double PadZCm = BrickHeightCm / 2.0;
	constexpr double UpperZCm = PadZCm + CoursePitchCm;

	const TArray<FGateCase> Gates = {
		/*
		 * Gate one: no geometry. The fuzz generators emit geometry-free structures, which must read
		 * as before. Arch shape without positions, so skipping the HasCompleteGeometry check fires here.
		 */
		{
			TEXT("GATE 1: no geometry — the shape of an arch with nobody's position known"),
			[](FStructure& Structure) -> int32
			{
				const int32 Pad = Structure.AddPiece(BrickMassKg, true);
				const int32 Loaded = Structure.AddPiece(BrickMassKg, false);
				const int32 FarPad = Structure.AddPiece(BrickMassKg, true);
				const int32 Neighbour = Structure.AddPiece(BrickMassKg, false);

				FConnection Bed;
				Bed.PieceA = Pad;
				Bed.PieceB = Loaded;
				Bed.InterfaceNormal = FVector::ZAxisVector;
				Bed.InterfaceAreaSqCm = HalfSeatAreaSqCm;
				Bed.Strength = GeneralPurposeMortar;

				const int32 Subject = Structure.AddConnection(Bed);

				FConnection FarBed;
				FarBed.PieceA = FarPad;
				FarBed.PieceB = Neighbour;
				FarBed.InterfaceNormal = FVector::ZAxisVector;
				FarBed.InterfaceAreaSqCm = HalfSeatAreaSqCm;
				FarBed.Strength = GeneralPurposeMortar;
				Structure.AddConnection(FarBed);

				FConnection Head;
				Head.PieceA = Loaded;
				Head.PieceB = Neighbour;
				Head.InterfaceNormal = FVector::XAxisVector;
				Head.InterfaceAreaSqCm = BrickWidthCm * BrickHeightCm;
				Head.Strength = GeneralPurposeMortar;
				Structure.AddConnection(Head);

				return Subject;
			},
			false
		},

		/*
		 * Gate two: no normal force. A massless piece gives k = min(1, 0/0); a NaN moment would
		 * read as a failed joint. The answer must be exactly zero and finite.
		 */
		{
			TEXT("GATE 2: a massless piece — no compression, no thrust line, and no 0/0"),
			[](FStructure& Structure) -> int32
			{
				const FPieceBox PadBox = BrickBoxAt(0.0, 0.0, PadZCm);
				const FPieceBox LoadedBox = BrickBoxAt(BondOffsetCm, 0.0, UpperZCm);
				const FPieceBox NeighbourBox = BrickBoxAt(BondOffsetCm + BrickPitchCm, 0.0, UpperZCm);
				const FPieceBox FarPadBox = BrickBoxAt(BrickPitchCm, 0.0, PadZCm);

				const int32 Pad = Structure.AddPiece(BrickMassKg, true, PadBox.CentreCm);
				const int32 Loaded = Structure.AddPiece(0.0, false, LoadedBox.CentreCm);
				const int32 FarPad = Structure.AddPiece(BrickMassKg, true, FarPadBox.CentreCm);
				const int32 Neighbour = Structure.AddPiece(0.0, false, NeighbourBox.CentreCm);

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

				return Subject;
			},
			true
		},

		/*
		 * Gate three: 0.5 cm off centre on a 20.5 cm patch, inside the 3.4167 cm kern. Catches a
		 * cap without the min, which would raise bending stress about seven-fold.
		 */
		{
			TEXT("GATE 3: eccentric but INSIDE the kern — nothing is opening"),
			[](FStructure& Structure) -> int32
			{
				const FPieceBox PadBox = BrickBoxAt(0.0, 0.0, PadZCm);
				const FPieceBox LoadedBox = BrickBoxAt(1.0, 0.0, UpperZCm);
				const FPieceBox NeighbourBox = BrickBoxAt(1.0 + BrickPitchCm, 0.0, UpperZCm);
				const FPieceBox FarPadBox = BrickBoxAt(1.0 + BrickPitchCm, 0.0, PadZCm);

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

				return Subject;
			},
			true
		},

		/*
		 * Gate four (trap 3): the neighbour is Supported only through its head joint to the piece
		 * asking. Arching here would let a wall hang from nothing. The seat sees 33.75
		 * brick-weight-cm on two brick weights, about 0.45 of capacity.
		 */
		{
			TEXT("GATE 4: trap 3 — the neighbour is hanging FROM the piece asking for the arch"),
			[](FStructure& Structure) -> int32
			{
				const FPieceBox PadBox = BrickBoxAt(0.0, 0.0, PadZCm);
				const FPieceBox LoadedBox = BrickBoxAt(BondOffsetCm, 0.0, UpperZCm);
				const FPieceBox HangerBox = BrickBoxAt(BondOffsetCm + BrickPitchCm, 0.0, UpperZCm);

				const int32 Pad = Structure.AddPiece(BrickMassKg, true, PadBox.CentreCm);
				const int32 Loaded = Structure.AddPiece(BrickMassKg, false, LoadedBox.CentreCm);
				const int32 Hanger = Structure.AddPiece(BrickMassKg, false, HangerBox.CentreCm);

				FConnection Bed;
				MakeInterface(Pad, PadBox, Loaded, LoadedBox, MortarJointCm, GeneralPurposeMortar, Bed);
				const int32 Subject = Structure.AddConnection(Bed);

				FConnection Head;
				MakeInterface(
					Loaded, LoadedBox, Hanger, HangerBox,
					MortarJointCm, GeneralPurposeMortar, Head);
				Structure.AddConnection(Head);

				return Subject;
			},
			true
		},
	};

	for (const FGateCase& Gate : Gates)
	{
		FStructure Structure;
		const int32 Subject = Gate.Build(Structure);

		if (Subject == INDEX_NONE)
		{
			AddError(FString::Printf(TEXT("%s: FIXTURE: the bed joint was refused"), Gate.Description));
			continue;
		}

		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE: HasCompleteGeometry should be %s"),
				Gate.Description, Gate.bExpectCompleteGeometry ? TEXT("true") : TEXT("false")),
			Structure.HasCompleteGeometry() == Gate.bExpectCompleteGeometry);

		Structure.SolveLoads();

		const FConnection& Bed = Structure.GetConnection(Subject);
		const FVector ForceUu = Structure.GetConnectionForce(Subject);
		const FVector MomentUuCm = Structure.GetConnectionMoment(Subject);

		const double Utilisation = Structure.GetConnectionUtilisation(Subject);

		const FBedJointReading Expected = ReadBedJoint(
			ForceUu, MomentUuCm, Bed.InterfaceHalfExtentCm, Bed.InterfaceAreaSqCm,
			GeneralPurposeMortar);

		const double Arched = ArchedUtilisation(Expected, GeneralPurposeMortar);

		AddInfo(FString::Printf(
			TEXT("%s: joint carries %s uu and %s uu.cm; reads %s, beam theory says %s, an arch ")
			TEXT("would read %s"),
			Gate.Description, *Bits(ForceUu.Size()), *Bits(MomentUuCm.Size()),
			*Bits(Utilisation), *Bits(Expected.Worst), *Bits(Arched)));

		// Finite first: comparisons against NaN are false, so the next row could pass for one.
		TestTrue(
			FString::Printf(TEXT("%s: the utilisation must be finite, it is %s"),
				Gate.Description, *Bits(Utilisation)),
			FMath::IsFinite(Utilisation));

		TestTrue(
			FString::Printf(
				TEXT("%s: the arch must NOT fire — beam theory on the solver's own force and ")
				TEXT("moment says %s, the joint reads %s"),
				Gate.Description, *Bits(Expected.Worst), *Bits(Utilisation)),
			FMath::Abs(Utilisation - Expected.Worst) <= 1.0e-12 * FMath::Max(Expected.Worst, 1.0e-12));
	}

	// Part 4: regression anchors this slice could break.

	/*
	 * Anchor 2: the staircase corbel, the direction check. Its eccentric side has no neighbour,
	 * so it must not arch. It reads the composite section's 0.0527187818 (StaircaseWallTestSupport.h);
	 * an ungated arch would read about 0.0195, 2.7x lower. The rung count cannot separate the two
	 * (both give zero over capacity); the number can. 1e-5 relative tolerance, as in the staircase test.
	 */
	{
		FBrickLayout Staircase;

		if (RunningBond(StaircaseWallSpec(), Staircase)
			&& Staircase.Boxes.Num() == StaircaseWallPieceCount)
		{
			for (const int32 Piece : StaircaseVoidPieces(Staircase.Boxes))
			{
				Staircase.Structure.RemovePiece(Piece);
			}

			Staircase.Structure.SolveLoads();

			int32 OverCapacity = 0;
			double Worst = 0.0;

			for (int32 Course = StaircaseLowestCorbelCourse;
				Course <= StaircaseHighestCorbelCourse; ++Course)
			{
				const int32 Corbel = StaircaseCorbelPiece(Staircase.Boxes, Course);
				const int32 Support = StaircaseCorbelSupportPiece(Staircase.Boxes, Course);
				const int32 Joint = JointBetweenPieces(Staircase.Structure, Corbel, Support);

				if (Joint == INDEX_NONE)
				{
					AddError(FString::Printf(
						TEXT("ANCHOR 2: FIXTURE: course %d has no corbel joint"), Course));

					continue;
				}

				const double Utilisation = Staircase.Structure.GetConnectionUtilisation(Joint);

				Worst = FMath::Max(Worst, Utilisation);

				if (Utilisation > 1.0)
				{
					++OverCapacity;
				}
			}

			AddInfo(FString::Printf(
				TEXT("ANCHOR 2: the staircase corbel reads %s at its bottom rung with %d of %d ")
				TEXT("over capacity (the hand ladder says %.8f and %d)"),
				*Bits(Worst), OverCapacity, StaircaseCorbelStepCount,
				StaircasePredictedWorstCorbelUtilisation,
				StaircasePredictedCorbelJointsOverCapacity));

			TestTrue(
				FString::Printf(
					TEXT("ANCHOR 2: the corbel's eccentric side has NO neighbour, so it must NOT ")
					TEXT("arch — it must read the composite section's %.8f and not an arched ")
					TEXT("0.0195, it reads %s"),
					StaircasePredictedWorstCorbelUtilisation, *Bits(Worst)),
				FMath::Abs(Worst - StaircasePredictedWorstCorbelUtilisation)
					<= 1.0e-5 * StaircasePredictedWorstCorbelUtilisation);

			TestEqual(
				FString::Printf(
					TEXT("ANCHOR 2: and exactly %d of its eleven rungs must be over capacity"),
					StaircasePredictedCorbelJointsOverCapacity),
				OverCapacity, StaircasePredictedCorbelJointsOverCapacity);
		}
		else
		{
			AddError(TEXT("ANCHOR 2: FIXTURE: the staircase wall did not lay"));
		}
	}

	/*
	 * Anchor 3: the narrow waist, the direction check for the abutment rule.
	 *
	 *      course 2         [ 3 ][ 4 ]
	 *      course 1            [ 2 ]        the waist
	 *      course 0         [ 0 ][ 1 ]      grounded
	 *
	 * 3 and 4 each rest only on the waist and overhang outward; their shared head joint is on the
	 * seated side, so neither may arch. Catches a rule that only asks "is there a head joint?".
	 */
	{
		FRunningBondSpec WaistSpec;
		WaistSpec.BrickSizeCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm);
		WaistSpec.JointThicknessCm = MortarJointCm;
		WaistSpec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
		WaistSpec.CoursesHigh = 3;
		WaistSpec.BricksPerCourse = 2;
		WaistSpec.End = EWallEnd::Ragged;
		WaistSpec.Strength = GeneralPurposeMortar;

		FBrickLayout Waist;

		if (RunningBond(WaistSpec, Waist) && Waist.Structure.NumPieces() == 5)
		{
			Waist.Structure.SolveLoads();

			constexpr int32 WaistedBrick = 3;

			int32 Subject = INDEX_NONE;

			for (int32 Joint = 0; Joint < Waist.Structure.NumConnections(); ++Joint)
			{
				if (Waist.Structure.GetJointRole(Joint, WaistedBrick) == EJointRole::BedBeneath)
				{
					Subject = Joint;
				}
			}

			if (Subject != INDEX_NONE)
			{
				const double Utilisation = Waist.Structure.GetConnectionUtilisation(Subject);

				AddInfo(FString::Printf(
					TEXT("ANCHOR 3: the waisted brick's bed joint reads %s"), *Bits(Utilisation)));

				/*
				 * (2667.198625 * 5.625 / 179.4817708 - 2667.198625 / 105.0625) / 10000 / 0.70 MPa
				 * (MOMENTS_DESIGN case (c), mean basis).
				 */
				constexpr double WaistUtilisation = 0.0083148340;

				TestTrue(
					FString::Printf(
						TEXT("ANCHOR 3: the waisted brick's head joint is on its SEATED side, so ")
						TEXT("it must NOT arch — it must still read %g, it reads %s"),
						WaistUtilisation, *Bits(Utilisation)),
					FMath::Abs(Utilisation - WaistUtilisation) <= 1.0e-8);
			}
			else
			{
				AddError(TEXT("ANCHOR 3: FIXTURE: the waisted brick has no bed joint beneath it"));
			}
		}
		else
		{
			AddError(TEXT("ANCHOR 3: FIXTURE: the narrow-waist wall did not lay"));
		}
	}

	/*
	 * Anchor 4: the cap is BedBeneath-only. A brick on one head joint (MOMENTS_DESIGN case (b))
	 * has sigma_n = 0 and reads 0.0594 in tension. Capping it gives k = 0, deleting the moment and
	 * dropping back to 0.0044 shear.
	 */
	{
		struct FChainCase
		{
			const TCHAR* Description;
			int32 BricksInChain;
			double Expected;
		};

		const TArray<FChainCase> Chains = {
			{ TEXT("ANCHOR 4: one brick on one head joint"), 1, 0.0593896154 },
			{ TEXT("ANCHOR 4: two bricks on the same head joint"), 2, 0.1187792308 },
		};

		for (const FChainCase& Chain : Chains)
		{
			FStructure Structure;

			const FPieceBox PadBox = BrickBoxAt(0.0, 0.0, PadZCm);
			const int32 Pad = Structure.AddPiece(BrickMassKg, true, PadBox.CentreCm);

			TArray<FPieceBox> Boxes;
			TArray<int32> Handles;

			for (int32 Brick = 0; Brick < Chain.BricksInChain; ++Brick)
			{
				const FPieceBox Box =
					BrickBoxAt(BrickPitchCm, 0.0, PadZCm + Brick * CoursePitchCm);

				Boxes.Add(Box);
				Handles.Add(Structure.AddPiece(BrickMassKg, false, Box.CentreCm));
			}

			FConnection Head;
			MakeInterface(
				Pad, PadBox, Handles[0], Boxes[0], MortarJointCm, GeneralPurposeMortar, Head);
			const int32 Subject = Structure.AddConnection(Head);

			for (int32 Brick = 1; Brick < Chain.BricksInChain; ++Brick)
			{
				FConnection Bed;
				MakeInterface(
					Handles[Brick - 1], Boxes[Brick - 1],
					Handles[Brick], Boxes[Brick],
					MortarJointCm, GeneralPurposeMortar, Bed);
				Structure.AddConnection(Bed);
			}

			Structure.SolveLoads();

			const double Utilisation = Structure.GetConnectionUtilisation(Subject);

			AddInfo(FString::Printf(TEXT("%s: reads %s, expected %.10f"),
				Chain.Description, *Bits(Utilisation), Chain.Expected));

			TestTrue(
				FString::Printf(
					TEXT("%s: a head joint is not a springing, so it must still read %.10f; it ")
					TEXT("reads %s"),
					Chain.Description, Chain.Expected, *Bits(Utilisation)),
				FMath::Abs(Utilisation - Chain.Expected) <= 1.0e-9 * Chain.Expected);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
