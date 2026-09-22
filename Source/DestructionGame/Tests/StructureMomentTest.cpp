// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"
#include "Core/StructureBinding.h"

#if WITH_DEV_AUTOMATION_TESTS

/** Named namespace: unity builds merge anonymous ones and helpers collide. */
namespace StructureMomentTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	// Constants are spelled out, not imported, so a wrong production constant disagrees.

	/** DESIGN.md's standard UK metric clay brick, cm. */
	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickWidthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;

	/** 1 cm mortar joint; makes the grid 22.5 cm. */
	constexpr double MortarJointCm = 1.0;

	/** Density first in the product to match Layout::PieceMassKg bit for bit; volume-first is one ulp low. */
	constexpr double BrickMassKg = 1.9 * BrickLengthCm * BrickWidthCm * BrickHeightCm / 1000.0;

	/** kg x 980 cm/s2 is already a force in uu. */
	constexpr double BrickWeightUu = BrickMassKg * 980.0;

	/** uu that load 1 cm2 to 1 MPa (1 N = 100 uu, 1 cm2 = 100 mm2). Not imported, so a wrong constant disagrees. */
	constexpr double ForceUnitsPerMPaPerSqCm = 100.0 * 100.0;

	/** Elastic section modulus of a rectangle bent along the second axis, cm3: (4/3)*HalfAlong*HalfAcross^2. */
	constexpr double SectionModulusCm3(double HalfAlongCm, double HalfAcrossCm)
	{
		return (4.0 / 3.0) * HalfAlongCm * HalfAcrossCm * HalfAcrossCm;
	}

	/** A whole-brick box at the given centre. */
	FPieceBox BrickBoxAt(double CentreXCm, double CentreYCm, double CentreZCm)
	{
		FPieceBox Box;
		Box.CentreCm = FVector(CentreXCm, CentreYCm, CentreZCm);
		Box.ExtentCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm) * 0.5;
		return Box;
	}

	/** Full-precision print, so a last-bit failure is readable. */
	FString Bits(double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	/**
	 * A grounded pad, then one brick per course, each jointed only to the one below, so every
	 * brick rests on exactly one joint (N = 1, determinate). Vertical because a horizontal row
	 * forms a cycle the solver strands. Brick k is handle k + 1; joint k is under brick k.
	 */
	struct FHangingChain
	{
		FStructure Structure;

		/** One box per piece handle, the pad first. */
		TArray<FPieceBox> Boxes;

		/** X centre of each brick; index 0 is on the head joint. */
		TArray<double> BrickCentreXCm;

		/** Joint k is under brick k. */
		TArray<int32> Joints;

		/** Whether every piece and joint was accepted. */
		bool bBuilt = true;
	};

	/** Build a chain. The first brick must be one cell (+/- 22.5) from the pad to form a head joint. */
	FHangingChain MakeHangingChain(const TArray<double>& BrickCentreXCm)
	{
		FHangingChain Chain;
		Chain.BrickCentreXCm = BrickCentreXCm;

		const FPieceBox PadBox = BrickBoxAt(0.0, 0.0, BrickHeightCm / 2.0);

		Chain.Boxes.Add(PadBox);
		Chain.bBuilt =
			Chain.bBuilt && Chain.Structure.AddPiece(BrickMassKg, true, PadBox.CentreCm) == 0;

		for (int32 Brick = 0; Brick < BrickCentreXCm.Num(); ++Brick)
		{
			const FPieceBox Box = BrickBoxAt(
				BrickCentreXCm[Brick],
				0.0,
				BrickHeightCm / 2.0 + Brick * (BrickHeightCm + MortarJointCm));

			Chain.Boxes.Add(Box);
			Chain.bBuilt = Chain.bBuilt
				&& Chain.Structure.AddPiece(BrickMassKg, false, Box.CentreCm) == Brick + 1;
		}

		for (int32 Lower = 0; Lower < BrickCentreXCm.Num(); ++Lower)
		{
			FConnection Joint;

			const bool bJointed = MakeInterface(
				Lower, Chain.Boxes[Lower],
				Lower + 1, Chain.Boxes[Lower + 1],
				MortarJointCm, GeneralPurposeMortar, Joint);

			const int32 Index = bJointed ? Chain.Structure.AddConnection(Joint) : INDEX_NONE;

			Chain.Joints.Add(Index);
			Chain.bBuilt = Chain.bBuilt && Index != INDEX_NONE;
		}

		return Chain;
	}

	/** Joint k's X centre, derived as the midpoint of the two brick centres, not read off the joint. */
	double ChainJointCentreXCm(const FHangingChain& Chain, int32 Joint)
	{
		return (Chain.Boxes[Joint].CentreCm.X + Chain.Boxes[Joint + 1].CentreCm.X) * 0.5;
	}

	/**
	 * Oracle: the signed moment about joint k's centroid from every brick above it, uu.cm. A flat
	 * sum, unlike the solver's accumulation, so they cannot agree by construction. Signed so a
	 * zig-zag chain cancels to zero. Gravity is vertical, so every moment is about world Y.
	 */
	double ChainMomentAboutJointUuCm(const FHangingChain& Chain, int32 Joint)
	{
		const double JointCentreXCm = ChainJointCentreXCm(Chain, Joint);

		double MomentUuCm = 0.0;

		for (int32 Brick = Joint; Brick < Chain.BrickCentreXCm.Num(); ++Brick)
		{
			MomentUuCm += BrickWeightUu * (Chain.BrickCentreXCm[Brick] - JointCentreXCm);
		}

		return MomentUuCm;
	}

	/** Force joint k carries: every brick above it. */
	double ChainForceUu(const FHangingChain& Chain, int32 Joint)
	{
		return (Chain.BrickCentreXCm.Num() - Joint) * BrickWeightUu;
	}
}

/**
 * A brick held by one head joint reads bending tension, not shear. Gravity is parallel to a head
 * joint, so the mean normal stress is zero, but the weight acts 11.25 cm from the joint's
 * mid-plane centroid and levers it open:
 *
 *     M       = 2667.198625 uu * 11.25 cm   = 30005.98453125 uu.cm
 *     W_sec   = (4/3) * 5.125 * 3.25^2      = 72.177083 cm3
 *     tension = M / W_sec / 10000 / 0.70   = 0.0593896154
 *
 * M/W is uu/cm2, so the same 10000 converts it; a stray 100x surfaces here. Each row asserts that
 * tension governs, since utilisation is the worst of three axes. N = 1 only: see
 * SymmetricSupportsCarryNoMoment for several supports.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureHangingBrickPeelsTest,
	"DestructionGame.Core.Structure.HangingBrickPeelsRatherThanShears",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureHangingBrickPeelsTest::RunTest(const FString& Parameters)
{
	using namespace StructureMomentTestSupport;

	// Expectations are derived from these strengths, so a profile retune must fail here.
	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against mean f_x1 = 0.7 MPa, the profile carries %g"),
			GeneralPurposeMortar.TensileStrengthMPa),
		GeneralPurposeMortar.TensileStrengthMPa == 0.7);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against mean cohesion 0.9 MPa, the profile carries %g"),
			GeneralPurposeMortar.ShearCohesionMPa),
		GeneralPurposeMortar.ShearCohesionMPa == 0.9);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against compressive 10 MPa, the profile carries %g"),
			GeneralPurposeMortar.CompressiveStrengthMPa),
		GeneralPurposeMortar.CompressiveStrengthMPa == 10.0);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against clay brick at 1.9 g/cm3, the profile carries %g"),
			ClayBrick.DensityGramsPerCubicCm),
		ClayBrick.DensityGramsPerCubicCm == 1.9);

	/** A grounded pad and one brick hanging off it by a head joint (N = 1, DESIGN.md §3). */
	struct FPeelCase
	{
		const TCHAR* Description;

		FPieceBox PadBox;
		FPieceBox HangingBox;

		/** Whether the pad is MakeInterface's first handle; the answer must not depend on it. */
		bool bPadIsPieceA;

		/** What Layout::MakeInterface must emit for this pair. */
		FVector ExpectedJointCentreCm;
		FVector ExpectedJointHalfExtentCm;
		double ExpectedAreaSqCm;

		/** Offset from the joint centroid to the brick's centre of mass, cm. */
		double ExpectedLeverArmCm;

		/** Section modulus about the bending axis, cm3. */
		double ExpectedModulusCm3;
	};

	constexpr double HalfLengthCm = BrickLengthCm / 2.0;
	constexpr double HalfWidthCm = BrickWidthCm / 2.0;
	constexpr double HalfHeightCm = BrickHeightCm / 2.0;

	constexpr double BrickPitchCm = BrickLengthCm + MortarJointCm;

	const TArray<FPeelCase> Cases = {
		// Joint mid-plane at X = 11.25, mass at X = 22.5: an 11.25 cm arm.
		{
			TEXT("a brick hanging off one head joint, pad on the left"),
			BrickBoxAt(0.0, 0.0, HalfHeightCm),
			BrickBoxAt(BrickPitchCm, 0.0, HalfHeightCm),
			true,
			FVector(BrickPitchCm / 2.0, 0.0, HalfHeightCm),
			FVector(0.0, HalfWidthCm, HalfHeightCm),
			BrickWidthCm * BrickHeightCm,
			BrickPitchCm / 2.0,
			SectionModulusCm3(HalfWidthCm, HalfHeightCm)
		},

		// Mirrored: the moment's sign says which edge opens, not how hard, so the reading must match.
		{
			TEXT("the same joint mirrored, pad on the right"),
			BrickBoxAt(BrickPitchCm, 0.0, HalfHeightCm),
			BrickBoxAt(0.0, 0.0, HalfHeightCm),
			true,
			FVector(BrickPitchCm / 2.0, 0.0, HalfHeightCm),
			FVector(0.0, HalfWidthCm, HalfHeightCm),
			BrickWidthCm * BrickHeightCm,
			BrickPitchCm / 2.0,
			SectionModulusCm3(HalfWidthCm, HalfHeightCm)
		},

		// Only the handle order changes.
		{
			TEXT("the same joint with the hanging brick named first"),
			BrickBoxAt(BrickPitchCm, 0.0, HalfHeightCm),
			BrickBoxAt(0.0, 0.0, HalfHeightCm),
			false,
			FVector(BrickPitchCm / 2.0, 0.0, HalfHeightCm),
			FVector(0.0, HalfWidthCm, HalfHeightCm),
			BrickWidthCm * BrickHeightCm,
			BrickPitchCm / 2.0,
			SectionModulusCm3(HalfWidthCm, HalfHeightCm)
		},

		/*
		 * A Y-normal joint, bent and twisted. Torsion about the normal must be dropped, not folded
		 * in. Also catches an implementation that assumes an X normal or pairs the wrong modulus.
		 */
		{
			TEXT("a Y-normal joint bent about X and twisted about its own normal"),
			BrickBoxAt(0.0, 0.0, HalfHeightCm),
			BrickBoxAt(HalfLengthCm, BrickWidthCm + MortarJointCm, HalfHeightCm),
			true,
			FVector(HalfLengthCm / 2.0, (BrickWidthCm + MortarJointCm) / 2.0, HalfHeightCm),
			FVector(HalfLengthCm / 2.0, 0.0, HalfHeightCm),
			HalfLengthCm * BrickHeightCm,
			(BrickWidthCm + MortarJointCm) / 2.0,
			SectionModulusCm3(HalfLengthCm / 2.0, HalfHeightCm)
		},
	};

	constexpr double Tolerance = 1.0e-9;

	for (const FPeelCase& Case : Cases)
	{
		// The joint comes from the producer, since the lever arm is measured from its emitted centroid.
		FConnection Joint;

		const bool bJointed = Case.bPadIsPieceA
			? MakeInterface(0, Case.PadBox, 1, Case.HangingBox, MortarJointCm, GeneralPurposeMortar, Joint)
			: MakeInterface(1, Case.HangingBox, 0, Case.PadBox, MortarJointCm, GeneralPurposeMortar, Joint);

		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE: the two boxes must form a face"), Case.Description),
			bJointed);

		if (!bJointed)
		{
			continue;
		}

		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE: the joint should be %g cm2, MakeInterface emitted %g"),
				Case.Description, Case.ExpectedAreaSqCm, Joint.InterfaceAreaSqCm),
			FMath::IsNearlyEqual(Joint.InterfaceAreaSqCm, Case.ExpectedAreaSqCm, Tolerance));

		// Pinned so a moved centroid and the expected number fail together.
		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE: the joint's centroid should be (%g, %g, %g), it is (%g, %g, %g)"),
				Case.Description,
				Case.ExpectedJointCentreCm.X, Case.ExpectedJointCentreCm.Y, Case.ExpectedJointCentreCm.Z,
				Joint.InterfaceCentreCm.X, Joint.InterfaceCentreCm.Y, Joint.InterfaceCentreCm.Z),
			Joint.InterfaceCentreCm.Equals(Case.ExpectedJointCentreCm, Tolerance));

		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE: the joint's half-extents should be (%g, %g, %g), they are (%g, %g, %g)"),
				Case.Description,
				Case.ExpectedJointHalfExtentCm.X, Case.ExpectedJointHalfExtentCm.Y, Case.ExpectedJointHalfExtentCm.Z,
				Joint.InterfaceHalfExtentCm.X, Joint.InterfaceHalfExtentCm.Y, Joint.InterfaceHalfExtentCm.Z),
			Joint.InterfaceHalfExtentCm.Equals(Case.ExpectedJointHalfExtentCm, Tolerance));

		// Zero mean normal stress, so both edges see sigma_b; tension governs because mortar is weaker in tension.
		const double BendingStressMPa =
			(BrickWeightUu * Case.ExpectedLeverArmCm)
			/ (Case.ExpectedModulusCm3 * ForceUnitsPerMPaPerSqCm);

		const double ShearStressMPa =
			BrickWeightUu / (Case.ExpectedAreaSqCm * ForceUnitsPerMPaPerSqCm);

		const double ExpectedUtilisation = BendingStressMPa / GeneralPurposeMortar.TensileStrengthMPa;
		const double ShearUtilisation = ShearStressMPa / GeneralPurposeMortar.ShearCohesionMPa;
		const double CompressionUtilisation =
			BendingStressMPa / GeneralPurposeMortar.CompressiveStrengthMPa;

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE PRECONDITION: bending in tension must be the governing axis — ")
				TEXT("tension %.10f vs shear %.10f vs compression %.10f"),
				Case.Description, ExpectedUtilisation, ShearUtilisation, CompressionUtilisation),
			ExpectedUtilisation > ShearUtilisation && ExpectedUtilisation > CompressionUtilisation);

		// One brick on one head joint still hangs.
		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE PRECONDITION: one brick must still hang, expected %.10f"),
				Case.Description, ExpectedUtilisation),
			ExpectedUtilisation < 1.0);

		FStructure Structure;

		const int32 Pad = Structure.AddPiece(BrickMassKg, true, Case.PadBox.CentreCm);
		const int32 Hanging = Structure.AddPiece(BrickMassKg, false, Case.HangingBox.CentreCm);

		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE: both pieces should be accepted, got %d and %d"),
				Case.Description, Pad, Hanging),
			Pad == 0 && Hanging == 1);

		const int32 JointIndex = Structure.AddConnection(Joint);

		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE: the joint should be accepted, got %d"),
				Case.Description, JointIndex),
			JointIndex == 0);

		if (JointIndex == INDEX_NONE)
		{
			continue;
		}

		Structure.SolveLoads();

		// The load path is what makes this N = 1: a head joint carrying the whole weight.
		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE: the joint must be a head joint on the hanging brick"),
				Case.Description),
			Structure.GetJointRole(JointIndex, Hanging) == EJointRole::Head);

		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE: the hanging brick must be held up by it"),
				Case.Description),
			Structure.GetPieceSupport(Hanging) == EPieceSupport::Supported);

		const FVector Force = Structure.GetConnectionForce(JointIndex);

		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE: the joint should carry the whole brick, %g uu, it carries (%g, %g, %g)"),
				Case.Description, BrickWeightUu, Force.X, Force.Y, Force.Z),
			FMath::IsNearlyEqual(FMath::Abs(Force.Z), BrickWeightUu, Tolerance)
				&& FMath::IsNearlyZero(Force.X, Tolerance)
				&& FMath::IsNearlyZero(Force.Y, Tolerance));

		const double Utilisation = Structure.GetConnectionUtilisation(JointIndex);

		TestTrue(
			FString::Printf(
				TEXT("%s: a load path %g cm off the joint's centroid should read %.10f at the opened ")
				TEXT("edge, it reads %.10f"),
				Case.Description, Case.ExpectedLeverArmCm, ExpectedUtilisation, Utilisation),
			FMath::IsNearlyEqual(Utilisation, ExpectedUtilisation, Tolerance));
	}

	return true;
}

/**
 * A symmetric wall loads its joints bit-identically with and without centres of mass. Giving each
 * supporting joint (p - c_j) x S_j is wrong (MOMENTS_DESIGN.md): the moments cancel across the
 * pair, not on either joint. A piece on several supports is indeterminate, so N >= 2 carries no
 * moment. Exact equality because the cascade fuzz has joints at exactly 1.0, where one ulp matters.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureSymmetricSupportsTest,
	"DestructionGame.Core.Structure.SymmetricSupportsCarryNoMoment",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureSymmetricSupportsTest::RunTest(const FString& Parameters)
{
	using namespace StructureMomentTestSupport;

	FRunningBondSpec Spec;
	Spec.BrickSizeCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm);
	Spec.JointThicknessCm = MortarJointCm;
	Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
	Spec.CoursesHigh = 4;
	Spec.BricksPerCourse = 4;
	Spec.End = EWallEnd::Flush;
	Spec.Strength = GeneralPurposeMortar;

	FBrickLayout Laid;
	const bool bLaid = RunningBond(Spec, Laid);

	TestTrue(TEXT("FIXTURE: RunningBond should lay a flush 4 x 4 wall"), bLaid);

	if (!bLaid)
	{
		return true;
	}

	// Without placed pieces both halves are geometry-free and agreeing proves nothing.
	TestTrue(
		TEXT("FIXTURE PRECONDITION: a wall laid by the producer must know where every piece ")
		TEXT("and every joint is, or the comparison below is a structure against itself"),
		Laid.Structure.HasCompleteGeometry());

	// The twin: same pieces and joints, no centres of mass.
	FStructure Bare;

	for (int32 Piece = 0; Piece < Laid.Structure.NumPieces(); ++Piece)
	{
		const FStructurePiece& Placed = Laid.Structure.GetPiece(Piece);
		Bare.AddPiece(Placed.MassKg, Placed.bIsGrounded);
	}

	for (int32 Joint = 0; Joint < Laid.Structure.NumConnections(); ++Joint)
	{
		Bare.AddConnection(Laid.Structure.GetConnection(Joint));
	}

	TestTrue(
		FString::Printf(TEXT("FIXTURE: the twin should be the same graph, %d/%d pieces and %d/%d joints"),
			Bare.NumPieces(), Laid.Structure.NumPieces(),
			Bare.NumConnections(), Laid.Structure.NumConnections()),
		Bare.NumPieces() == Laid.Structure.NumPieces()
			&& Bare.NumConnections() == Laid.Structure.NumConnections());

	TestFalse(
		TEXT("FIXTURE: the twin must NOT know where its pieces are — that is the only difference"),
		Bare.HasCompleteGeometry());

	Laid.Structure.SolveLoads();
	Bare.SolveLoads();

	// Check the wall carries something, or the comparison passes trivially.
	double WorstUtilisation = 0.0;
	int32 WorstJoint = INDEX_NONE;
	int32 Mismatches = 0;

	for (int32 Joint = 0; Joint < Laid.Structure.NumConnections(); ++Joint)
	{
		const double Placed = Laid.Structure.GetConnectionUtilisation(Joint);
		const double Unplaced = Bare.GetConnectionUtilisation(Joint);

		if (Placed > WorstUtilisation)
		{
			WorstUtilisation = Placed;
			WorstJoint = Joint;
		}

		const FVector PlacedForce = Laid.Structure.GetConnectionForce(Joint);
		const FVector UnplacedForce = Bare.GetConnectionForce(Joint);

		// Force must not move either; moments ride alongside routing and must not disturb it.
		const bool bAgrees = Placed == Unplaced && PlacedForce == UnplacedForce;

		if (!bAgrees)
		{
			++Mismatches;

			if (Mismatches <= 6)
			{
				AddError(FString::Printf(
					TEXT("joint %d moved when its pieces were placed: utilisation %s vs %s, ")
					TEXT("force (%s, %s, %s) vs (%s, %s, %s)"),
					Joint, *Bits(Placed), *Bits(Unplaced),
					*Bits(PlacedForce.X), *Bits(PlacedForce.Y), *Bits(PlacedForce.Z),
					*Bits(UnplacedForce.X), *Bits(UnplacedForce.Y), *Bits(UnplacedForce.Z)));
			}
		}
	}

	AddInfo(FString::Printf(
		TEXT("the worst-loaded joint of the flush 4 x 4 wall is joint %d at %s of its capacity"),
		WorstJoint, *Bits(WorstUtilisation)));

	TestTrue(
		FString::Printf(TEXT("FIXTURE: the wall must actually be carrying something, worst joint is %s"),
			*Bits(WorstUtilisation)),
		WorstUtilisation > 0.0);

	TestEqual(
		FString::Printf(
			TEXT("placing the pieces of a symmetric wall must change nothing at all; %d of %d joints moved"),
			Mismatches, Laid.Structure.NumConnections()),
		Mismatches, 0);

	return true;
}

/**
 * HasCompleteGeometry distinguishes "no positions supplied" from "load centred"; both give a zero
 * moment. It is a conjunction over live pieces (centre of mass) and live joints (rectangle), so
 * removed pieces cannot spoil it. An empty structure reads true (empty conjunction).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureCompleteGeometryTest,
	"DestructionGame.Core.Structure.GeometryIsCompleteOrItIsNot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureCompleteGeometryTest::RunTest(const FString& Parameters)
{
	using namespace StructureMomentTestSupport;

	/** Function pointers, not TFunction: rows capture nothing. */
	struct FGeometryCase
	{
		const TCHAR* Description;
		void (*Build)(FStructure&);
		bool bExpected;
	};

	const TArray<FGeometryCase> Cases = {
		// Change this row alone if a fail-closed empty reading is preferred.
		{
			TEXT("a structure with nothing in it"),
			[](FStructure&) {},
			true
		},

		{
			TEXT("one placed piece and no joints at all"),
			[](FStructure& Structure)
			{
				Structure.AddPiece(BrickMassKg, true, FVector(0.0, 0.0, BrickHeightCm / 2.0));
			},
			true
		},

		// The row that matters: a valid structure with no positions.
		{
			TEXT("one piece nobody placed"),
			[](FStructure& Structure)
			{
				Structure.AddPiece(BrickMassKg, true);
			},
			false
		},

		{
			TEXT("two placed pieces and a joint carrying its own rectangle"),
			[](FStructure& Structure)
			{
				const FPieceBox Pad = BrickBoxAt(0.0, 0.0, BrickHeightCm / 2.0);
				const FPieceBox Brick =
					BrickBoxAt(BrickLengthCm + MortarJointCm, 0.0, BrickHeightCm / 2.0);

				Structure.AddPiece(BrickMassKg, true, Pad.CentreCm);
				Structure.AddPiece(BrickMassKg, false, Brick.CentreCm);

				FConnection Joint;
				MakeInterface(0, Pad, 1, Brick, MortarJointCm, GeneralPurposeMortar, Joint);
				Structure.AddConnection(Joint);
			},
			true
		},

		// Placed pieces but an unplaced joint: its lever arm is unmeasurable.
		{
			TEXT("placed pieces, but a joint with no rectangle"),
			[](FStructure& Structure)
			{
				Structure.AddPiece(BrickMassKg, true, FVector(0.0, 0.0, BrickHeightCm / 2.0));
				Structure.AddPiece(
					BrickMassKg, false,
					FVector(BrickLengthCm + MortarJointCm, 0.0, BrickHeightCm / 2.0));

				FConnection Joint;
				Joint.PieceA = 0;
				Joint.PieceB = 1;
				Joint.InterfaceNormal = FVector::XAxisVector;
				Joint.InterfaceAreaSqCm = BrickWidthCm * BrickHeightCm;
				Joint.Strength = GeneralPurposeMortar;
				Structure.AddConnection(Joint);
			},
			false
		},

		{
			TEXT("a joint with its rectangle, but a piece nobody placed"),
			[](FStructure& Structure)
			{
				const FPieceBox Pad = BrickBoxAt(0.0, 0.0, BrickHeightCm / 2.0);
				const FPieceBox Brick =
					BrickBoxAt(BrickLengthCm + MortarJointCm, 0.0, BrickHeightCm / 2.0);

				Structure.AddPiece(BrickMassKg, true, Pad.CentreCm);
				Structure.AddPiece(BrickMassKg, false);

				FConnection Joint;
				MakeInterface(0, Pad, 1, Brick, MortarJointCm, GeneralPurposeMortar, Joint);
				Structure.AddConnection(Joint);
			},
			false
		},

		// Removing the undescribed piece severs its joint, leaving a fully described live structure.
		{
			TEXT("the undescribed piece and its joint have been removed"),
			[](FStructure& Structure)
			{
				const FPieceBox Pad = BrickBoxAt(0.0, 0.0, BrickHeightCm / 2.0);
				const FPieceBox Brick =
					BrickBoxAt(BrickLengthCm + MortarJointCm, 0.0, BrickHeightCm / 2.0);

				Structure.AddPiece(BrickMassKg, true, Pad.CentreCm);
				Structure.AddPiece(BrickMassKg, false, Brick.CentreCm);
				Structure.AddPiece(BrickMassKg, false);

				FConnection Placed;
				MakeInterface(0, Pad, 1, Brick, MortarJointCm, GeneralPurposeMortar, Placed);
				Structure.AddConnection(Placed);

				FConnection Unplaced;
				Unplaced.PieceA = 1;
				Unplaced.PieceB = 2;
				Unplaced.InterfaceNormal = FVector::XAxisVector;
				Unplaced.InterfaceAreaSqCm = BrickWidthCm * BrickHeightCm;
				Unplaced.Strength = GeneralPurposeMortar;
				Structure.AddConnection(Unplaced);

				Structure.RemovePiece(2);
			},
			true
		},
	};

	for (const FGeometryCase& Case : Cases)
	{
		FStructure Structure;
		Case.Build(Structure);

		const bool bComplete = Structure.HasCompleteGeometry();

		TestTrue(
			FString::Printf(TEXT("%s: HasCompleteGeometry should be %s, it is %s"),
				Case.Description,
				Case.bExpected ? TEXT("true") : TEXT("false"),
				bComplete ? TEXT("true") : TEXT("false")),
			bComplete == Case.bExpected);
	}

	return true;
}

/**
 * A wall adopted through FStructureBinding keeps its centres of mass, so its joints see moments.
 * Losing them is silent: an unplaced piece loads its joints exactly like a centred one.
 *
 * Fixture: the narrow-waist wall (ragged, 3 courses of 2); bricks 3 and 4 each rest only on the
 * waist brick (N = 1). Brick 3's bed patch is 10.25 x 10.25 cm with the load 5.625 cm off-centre:
 *
 *     sigma_b = 2667.198625 * 5.625 / 179.4817708 cm3  = 8.3590619e-3 MPa
 *     sigma_n = -2667.198625 / 105.0625 cm2             = -2.5386781e-3 MPa
 *     tension = (sigma_n + sigma_b) / 0.70 MPa          = 0.0083148340
 *
 * The laid wall is checked first as the control, so a failure says where the geometry was lost.
 * Actor stand-ins are null; AdoptLayout never resolves them.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureBindingAdoptedWallGeometryTest,
	"DestructionGame.Core.StructureBinding.AdoptedWallLoadsItsWaistEccentrically",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureBindingAdoptedWallGeometryTest::RunTest(const FString& Parameters)
{
	using namespace StructureMomentTestSupport;

	// Expectations are derived from these strengths.
	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against mean f_x1 = 0.7 MPa, the profile carries %g"),
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

	FRunningBondSpec Spec;
	Spec.BrickSizeCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm);
	Spec.JointThicknessCm = MortarJointCm;
	Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
	Spec.CoursesHigh = 3;
	Spec.BricksPerCourse = 2;
	Spec.End = EWallEnd::Ragged;
	Spec.Strength = GeneralPurposeMortar;

	FBrickLayout Laid;
	const bool bLaid = RunningBond(Spec, Laid);

	TestTrue(TEXT("FIXTURE: RunningBond should lay the narrow-waist wall"), bLaid);

	if (!bLaid)
	{
		return true;
	}

	// Pin the shape, or a spec without a waist would pass vacuously.
	constexpr int32 WaistedBrick = 3;

	TestEqual(
		FString::Printf(TEXT("FIXTURE: a ragged 3-course wall 2 bricks wide should be 5 pieces, got %d"),
			Laid.Structure.NumPieces()),
		Laid.Structure.NumPieces(), 5);

	TestEqual(
		FString::Printf(TEXT("FIXTURE: it should carry 6 joints, got %d"),
			Laid.Structure.NumConnections()),
		Laid.Structure.NumConnections(), 6);

	// Otherwise the binding has nothing to lose and the test is vacuous.
	TestTrue(
		TEXT("FIXTURE: the laid wall must already know where every piece and every joint is"),
		Laid.Structure.HasCompleteGeometry());

	// --- what the joint under the waisted brick must read, worked from the grid ---------

	constexpr double HalfLengthCm = BrickLengthCm / 2.0;
	constexpr double BrickPitchCm = BrickLengthCm + MortarJointCm;
	constexpr double CoursePitchCm = BrickHeightCm + MortarJointCm;

	/** Running bond offsets alternate courses by half a cell. */
	constexpr double BondOffsetCm = BrickPitchCm / 2.0;

	// Shared span: the waist's left face to the upper brick's right face.
	constexpr double SharedLowXCm = BondOffsetCm - HalfLengthCm;
	constexpr double SharedHighXCm = HalfLengthCm;

	constexpr double BedPatchHalfXCm = (SharedHighXCm - SharedLowXCm) / 2.0;
	constexpr double BedPatchHalfYCm = BrickWidthCm / 2.0;
	constexpr double BedPatchCentreXCm = (SharedLowXCm + SharedHighXCm) / 2.0;

	// Top of course 1, plus half the mortar bed above it: the mid-plane MakeInterface emits.
	constexpr double BedPatchCentreZCm =
		BrickHeightCm / 2.0 + CoursePitchCm + BrickHeightCm / 2.0 + MortarJointCm / 2.0;

	constexpr double BedPatchAreaSqCm = (2.0 * BedPatchHalfXCm) * (2.0 * BedPatchHalfYCm);

	// The brick's own centre is at x = 0; the patch it lands on is not.
	constexpr double LeverArmCm = BedPatchCentreXCm;

	/*
	 * Moment about Y, resisted by depth along X. The patch is square, so a swapped modulus pair is
	 * invisible here; HangingBrickPeelsRatherThanShears pins the order.
	 */
	const double BedPatchModulusCm3 = SectionModulusCm3(BedPatchHalfYCm, BedPatchHalfXCm);

	const double BendingStressMPa =
		(BrickWeightUu * LeverArmCm) / (BedPatchModulusCm3 * ForceUnitsPerMPaPerSqCm);

	// Positive in tension, so a brick pressing down is negative.
	const double NormalStressMPa =
		-BrickWeightUu / (BedPatchAreaSqCm * ForceUnitsPerMPaPerSqCm);

	const double PeakTensileStressMPa = FMath::Max(0.0, NormalStressMPa + BendingStressMPa);
	const double PeakCompressiveStressMPa = FMath::Max(0.0, BendingStressMPa - NormalStressMPa);

	const double ExpectedUtilisation =
		PeakTensileStressMPa / GeneralPurposeMortar.TensileStrengthMPa;
	const double CompressionUtilisation =
		PeakCompressiveStressMPa / GeneralPurposeMortar.CompressiveStrengthMPa;

	// The reading with no centre of mass: averaged stress only.
	const double CentredUtilisation =
		-NormalStressMPa / GeneralPurposeMortar.CompressiveStrengthMPa;

	// Tension must govern or the reading would not move. Shear is zero on a bed joint under gravity.
	TestTrue(
		FString::Printf(
			TEXT("FIXTURE PRECONDITION: bending in tension must govern — tension %.10f vs ")
			TEXT("compression %.10f vs centred %.10f"),
			ExpectedUtilisation, CompressionUtilisation, CentredUtilisation),
		ExpectedUtilisation > CompressionUtilisation && ExpectedUtilisation > CentredUtilisation);

	// And the wall must still stand.
	TestTrue(
		FString::Printf(TEXT("FIXTURE PRECONDITION: the wall must still stand, expected %.10f"),
			ExpectedUtilisation),
		ExpectedUtilisation < 1.0);

	AddInfo(FString::Printf(
		TEXT("the waisted brick's bed joint: %s eccentric against %s centred"),
		*Bits(ExpectedUtilisation), *Bits(CentredUtilisation)));

	// Found by role, not index. Exactly one bed joint beneath is also the N = 1 precondition.
	const auto FindOnlyBedJointBeneath =
		[this, WaistedBrick](const FStructure& Structure, const TCHAR* Which) -> int32
	{
		int32 Found = INDEX_NONE;
		int32 Count = 0;

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			if (Structure.GetJointRole(Joint, WaistedBrick) == EJointRole::BedBeneath)
			{
				Found = Joint;
				++Count;
			}
		}

		TestEqual(
			FString::Printf(
				TEXT("%s: FIXTURE: the waisted brick must rest on EXACTLY ONE bed joint — ")
				TEXT("N = 1 is what makes its moment determinate — got %d"),
				Which, Count),
			Count, 1);

		return Count == 1 ? Found : INDEX_NONE;
	};

	// --- control: the producer's own structure reads the eccentric figure ---

	Laid.Structure.SolveLoads();

	const int32 LaidJoint = FindOnlyBedJointBeneath(Laid.Structure, TEXT("the laid wall"));

	if (LaidJoint == INDEX_NONE)
	{
		return true;
	}

	constexpr double Tolerance = 1.0e-9;

	const FConnection& LaidConnection = Laid.Structure.GetConnection(LaidJoint);

	TestTrue(
		FString::Printf(
			TEXT("FIXTURE: the bed patch should be %g cm2 centred at (%g, %g, %g) with ")
			TEXT("half-extents (%g, %g, 0); MakeInterface emitted %g cm2 at (%g, %g, %g) with ")
			TEXT("(%g, %g, %g)"),
			BedPatchAreaSqCm, BedPatchCentreXCm, 0.0, BedPatchCentreZCm,
			BedPatchHalfXCm, BedPatchHalfYCm,
			LaidConnection.InterfaceAreaSqCm,
			LaidConnection.InterfaceCentreCm.X, LaidConnection.InterfaceCentreCm.Y,
			LaidConnection.InterfaceCentreCm.Z,
			LaidConnection.InterfaceHalfExtentCm.X, LaidConnection.InterfaceHalfExtentCm.Y,
			LaidConnection.InterfaceHalfExtentCm.Z),
		FMath::IsNearlyEqual(LaidConnection.InterfaceAreaSqCm, BedPatchAreaSqCm, Tolerance)
			&& LaidConnection.InterfaceCentreCm.Equals(
				FVector(BedPatchCentreXCm, 0.0, BedPatchCentreZCm), Tolerance)
			&& LaidConnection.InterfaceHalfExtentCm.Equals(
				FVector(BedPatchHalfXCm, BedPatchHalfYCm, 0.0), Tolerance));

	TestTrue(
		FString::Printf(
			TEXT("CONTROL: the LAID wall's waist joint should already read %.10f, it reads %.10f"),
			ExpectedUtilisation, Laid.Structure.GetConnectionUtilisation(LaidJoint)),
		FMath::IsNearlyEqual(
			Laid.Structure.GetConnectionUtilisation(LaidJoint), ExpectedUtilisation, Tolerance));

	// --- the claim: adoption must not lose any of that ----------------------------------

	TArray<UObject*> StandIns;
	StandIns.Init(nullptr, Laid.Structure.NumPieces());

	FStructureBinding Binding;
	Binding.StructureId = 3;

	const bool bAdopted = AdoptLayout(Laid, StandIns, Binding);

	TestTrue(TEXT("FIXTURE: AdoptLayout should take a well-formed wall"), bAdopted);

	if (!bAdopted)
	{
		return true;
	}

	// Claim one catches any dropped field, not just the centre of mass.
	TestTrue(
		TEXT("a wall adopted into a binding must know where every piece and every joint is; ")
		TEXT("the layout did, so anything missing here was dropped in the replay"),
		Binding.GetStructure().HasCompleteGeometry());

	Binding.SolveLoads();

	const int32 BoundJoint = FindOnlyBedJointBeneath(Binding.GetStructure(), TEXT("the adopted wall"));

	if (BoundJoint == INDEX_NONE)
	{
		return true;
	}

	// Routing must match exactly; a difference means the replay disturbed the load path.
	const FVector LaidForce = Laid.Structure.GetConnectionForce(LaidJoint);
	const FVector BoundForce = Binding.GetStructure().GetConnectionForce(BoundJoint);

	TestTrue(
		FString::Printf(
			TEXT("FIXTURE: adoption must route the same load, laid (%s, %s, %s) vs adopted (%s, %s, %s)"),
			*Bits(LaidForce.X), *Bits(LaidForce.Y), *Bits(LaidForce.Z),
			*Bits(BoundForce.X), *Bits(BoundForce.Y), *Bits(BoundForce.Z)),
		LaidForce == BoundForce);

	// Claim two: same joint and force, so only the centre of mass can move the answer.
	const double BoundUtilisation = Binding.GetStructure().GetConnectionUtilisation(BoundJoint);

	TestTrue(
		FString::Printf(
			TEXT("a brick overhanging its only bed patch by %g cm should read %.10f through the ")
			TEXT("binding as it does through the layout; it reads %.10f (a centred load reads %.10f)"),
			LeverArmCm, ExpectedUtilisation, BoundUtilisation, CentredUtilisation),
		FMath::IsNearlyEqual(BoundUtilisation, ExpectedUtilisation, Tolerance));

	return true;
}

/**
 * A head joint past its elastic capacity gives under first crack, and the stack hanging off it
 * falls. On a head joint N = 0, so first crack (|M|/W <= f_t) is exactly the elastic readout:
 * the chain falls iff the readout exceeds 1.0, and the test pins that coincidence.
 *
 *     tension = n * 2667.198625 uu * 11.25 cm / 72.1770833 cm3 / 10000 / 0.70 = n * 0.059389616
 *
 * Sixteen bricks read 0.9502 and stand (the control); seventeen read 1.0096 and fall. Tension
 * governs (shear 0.0756 at n = 17). The stack is vertical because a horizontal chain is a cycle
 * the solver strands. Asserted on joint and support state, never movement.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureMomentBreaksTheJointTest,
	"DestructionGame.Core.Structure.MomentBreaksTheJointItOverloads",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureMomentBreaksTheJointTest::RunTest(const FString& Parameters)
{
	using namespace StructureMomentTestSupport;

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against mean f_x1 = 0.7 MPa, the profile carries %g"),
			GeneralPurposeMortar.TensileStrengthMPa),
		GeneralPurposeMortar.TensileStrengthMPa == 0.7);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against mean cohesion 0.9 MPa, the profile carries %g"),
			GeneralPurposeMortar.ShearCohesionMPa),
		GeneralPurposeMortar.ShearCohesionMPa == 0.9);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against compressive 10 MPa, the profile carries %g"),
			GeneralPurposeMortar.CompressiveStrengthMPa),
		GeneralPurposeMortar.CompressiveStrengthMPa == 10.0);

	constexpr double HalfWidthCm = BrickWidthCm / 2.0;
	constexpr double HalfHeightCm = BrickHeightCm / 2.0;
	constexpr double BrickPitchCm = BrickLengthCm + MortarJointCm;
	constexpr double CoursePitchCm = BrickHeightCm + MortarJointCm;

	/** A head joint is a brick's end face; its mid-plane is half a cell along. */
	constexpr double HeadJointAreaSqCm = BrickWidthCm * BrickHeightCm;
	constexpr double LeverArmCm = BrickPitchCm / 2.0;

	// Moment about Y is resisted by the 3.25 cm depth, not the 5.125 cm width; order matters.
	constexpr double HeadJointModulusCm3 = SectionModulusCm3(HalfWidthCm, HalfHeightCm);

	struct FChainCase
	{
		const TCHAR* Description;

		/** How many bricks hang off the one head joint. */
		int32 BricksInChain;

		/** Whether the head joint gives under first crack; equals readout > 1.0. */
		bool bFallsAtFirstCrack;
	};

	const TArray<FChainCase> Cases = {
		// Control: 0.9502 of capacity, lambda* 1.0524, stands.
		{ TEXT("sixteen bricks hanging off one head joint"), 16, false },

		// The claim: 1.0096 of capacity, lambda* 0.9905, the head joint gives.
		{ TEXT("seventeen bricks hanging off one head joint"), 17, true },
	};

	constexpr double Tolerance = 1.0e-9;

	for (const FChainCase& Case : Cases)
	{
		// A pad, and a square stack beside it joined to the pad only by its lowest head joint.
		FStructure Structure;

		const FPieceBox PadBox = BrickBoxAt(0.0, 0.0, HalfHeightCm);
		const int32 Pad = Structure.AddPiece(BrickMassKg, true, PadBox.CentreCm);

		TArray<FPieceBox> ChainBoxes;
		TArray<int32> Chain;

		for (int32 Brick = 0; Brick < Case.BricksInChain; ++Brick)
		{
			const FPieceBox Box =
				BrickBoxAt(BrickPitchCm, 0.0, HalfHeightCm + Brick * CoursePitchCm);

			ChainBoxes.Add(Box);
			Chain.Add(Structure.AddPiece(BrickMassKg, false, Box.CentreCm));
		}

		FConnection HeadJoint;
		const bool bHeaded = MakeInterface(
			Pad, PadBox, Chain[0], ChainBoxes[0], MortarJointCm, GeneralPurposeMortar, HeadJoint);

		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE: the pad and the hanging brick must form a head joint"),
				Case.Description),
			bHeaded);

		const int32 HeadIndex = Structure.AddConnection(HeadJoint);

		for (int32 Brick = 1; Brick < Case.BricksInChain; ++Brick)
		{
			FConnection BedJoint;
			const bool bBedded = MakeInterface(
				Chain[Brick - 1], ChainBoxes[Brick - 1],
				Chain[Brick], ChainBoxes[Brick],
				MortarJointCm, GeneralPurposeMortar, BedJoint);

			TestTrue(
				FString::Printf(TEXT("%s: FIXTURE: brick %d must sit on a bed joint"),
					Case.Description, Brick),
				bBedded);

			Structure.AddConnection(BedJoint);
		}

		TestEqual(
			FString::Printf(TEXT("%s: FIXTURE: the chain should be %d joints, got %d"),
				Case.Description, Case.BricksInChain, Structure.NumConnections()),
			Structure.NumConnections(), Case.BricksInChain);

		if (HeadIndex == INDEX_NONE)
		{
			continue;
		}

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the joint's mid-plane should be %g cm from the brick's centre, ")
				TEXT("its centroid is (%g, %g, %g)"),
				Case.Description, LeverArmCm,
				HeadJoint.InterfaceCentreCm.X, HeadJoint.InterfaceCentreCm.Y,
				HeadJoint.InterfaceCentreCm.Z),
			HeadJoint.InterfaceCentreCm.Equals(FVector(LeverArmCm, 0.0, HalfHeightCm), Tolerance)
				&& HeadJoint.InterfaceHalfExtentCm.Equals(
					FVector(0.0, HalfWidthCm, HalfHeightCm), Tolerance));

		const double ChainWeightUu = Case.BricksInChain * BrickWeightUu;

		const double BendingStressMPa =
			(ChainWeightUu * LeverArmCm) / (HeadJointModulusCm3 * ForceUnitsPerMPaPerSqCm);

		// Gravity is parallel to a head joint: all shear, sigma_n = 0.
		const double ShearStressMPa =
			ChainWeightUu / (HeadJointAreaSqCm * ForceUnitsPerMPaPerSqCm);

		const double ExpectedUtilisation =
			BendingStressMPa / GeneralPurposeMortar.TensileStrengthMPa;
		const double ShearUtilisation = ShearStressMPa / GeneralPurposeMortar.ShearCohesionMPa;
		const double CompressionUtilisation =
			BendingStressMPa / GeneralPurposeMortar.CompressiveStrengthMPa;

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE PRECONDITION: bending in tension must govern — tension %.10f ")
				TEXT("vs shear %.10f vs compression %.10f"),
				Case.Description, ExpectedUtilisation, ShearUtilisation, CompressionUtilisation),
			ExpectedUtilisation > ShearUtilisation && ExpectedUtilisation > CompressionUtilisation);

		// The row's fall flag must match the arithmetic's readout crossing.
		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE PRECONDITION: the case says the head joint %s at first crack and ")
				TEXT("the ELASTIC readout arithmetic says %.10f (falls iff readout > 1.0)"),
				Case.Description, Case.bFallsAtFirstCrack ? TEXT("falls") : TEXT("stands"),
				ExpectedUtilisation),
			(ExpectedUtilisation > 1.0) == Case.bFallsAtFirstCrack);

		// Readout first, so a failure here is upstream of the break decision.
		Structure.SolveLoads();

		const double ReadUtilisation = Structure.GetConnectionUtilisation(HeadIndex);

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the strain readout should say %.10f, it says %.10f"),
				Case.Description, ExpectedUtilisation, ReadUtilisation),
			FMath::IsNearlyEqual(ReadUtilisation, ExpectedUtilisation, Tolerance));

		// Under the 200-block cap, so the first-crack equilibrium LP decides.
		const int32 Passes = Structure.SolveAndBreak();

		if (Case.bFallsAtFirstCrack)
		{
			// Measured: 2 passes, head joint on pass 1. Which bed joint parts on pass 2 is not asserted.
			TestEqual(
				FString::Printf(
					TEXT("%s: elastic reading %.10f >= 1, first crack fells the head joint, so the ")
					TEXT("cascade should run 2 pass(es), it ran %d"),
					Case.Description, ExpectedUtilisation, Passes),
				Passes, 2);

			TestTrue(
				FString::Printf(
					TEXT("%s: the head joint the moment overloads MUST have given at first crack ")
					TEXT("(elastic reading %.10f)"),
					Case.Description, ExpectedUtilisation),
				Structure.GetConnection(HeadIndex).HasGiven());

			TestEqual(
				FString::Printf(
					TEXT("%s: the head joint gives on the FIRST pass, its break pass is %d"),
					Case.Description, Structure.GetBreakPass(HeadIndex)),
				Structure.GetBreakPass(HeadIndex), 1);

			// With the head joint gone, no brick has a path to the pad.
			for (int32 Brick = 0; Brick < Chain.Num(); ++Brick)
			{
				const EPieceSupport Support = Structure.GetPieceSupport(Chain[Brick]);

				TestEqual(
					FString::Printf(
						TEXT("%s: chain brick %d must be Falling once the head joint gives, it is %d"),
						Case.Description, Brick, static_cast<int32>(Support)),
					Support, EPieceSupport::Falling);
			}
		}
		else
		{
			// The control: nothing breaks, so an implementation that breaks everything fails.
			TestEqual(
				FString::Printf(
					TEXT("%s: elastic reading %.10f < 1, first crack is admissible, so the cascade ")
					TEXT("should break in 0 pass(es), it ran %d"),
					Case.Description, ExpectedUtilisation, Passes),
				Passes, 0);

			TestFalse(
				FString::Printf(
					TEXT("%s: the head joint must NOT have given — first crack is admissible at an ")
					TEXT("elastic reading of %.10f"),
					Case.Description, ExpectedUtilisation),
				Structure.GetConnection(HeadIndex).HasGiven());

			TestEqual(
				FString::Printf(
					TEXT("%s: the head joint carries no break pass, it is %d"),
					Case.Description, Structure.GetBreakPass(HeadIndex)),
				Structure.GetBreakPass(HeadIndex), INDEX_NONE);

			for (int32 Brick = 0; Brick < Chain.Num(); ++Brick)
			{
				const EPieceSupport Support = Structure.GetPieceSupport(Chain[Brick]);

				TestEqual(
					FString::Printf(
						TEXT("%s: chain brick %d must stay Supported, it is %d"),
						Case.Description, Brick, static_cast<int32>(Support)),
					Support, EPieceSupport::Supported);
			}

			// Bed joints carry no eccentricity and must hold.
			for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
			{
				if (Joint == HeadIndex)
				{
					continue;
				}

				TestFalse(
					FString::Printf(TEXT("%s: bed joint %d carries no eccentricity and must hold"),
						Case.Description, Joint),
					Structure.GetConnection(Joint).HasGiven());

				const double Settled = Structure.GetConnectionUtilisation(Joint);

				TestTrue(
					FString::Printf(
						TEXT("%s: bed joint %d carries no eccentricity and must read under 1.0; it reads %s"),
						Case.Description, Joint, *Bits(Settled)),
					Settled <= 1.0);
			}
		}

		/*
		 * Readout and break decision cross 1.0 together. Dropping the moment from the readout, or a
		 * plastic capacity for the break, would split them.
		 */
		TestEqual(
			FString::Printf(
				TEXT("%s: the head joint's elastic readout must be %s 1.0, it reads %.10f"),
				Case.Description, Case.bFallsAtFirstCrack ? TEXT("above") : TEXT("below"),
				ReadUtilisation),
			ReadUtilisation > 1.0, Case.bFallsAtFirstCrack);
	}

	return true;
}

/**
 * A moment travels down the load path with the force, re-referenced to each joint's own centroid:
 *
 *     M_about(c_to) = M_about(c_from) + (c_from - c_to) x F_transmitted
 *
 * Checked against ChainMomentAboutJointUuCm, a flat sum that never walks the path. A square stack
 * is the control (nothing moves); a two-brick corbel takes the head joint from 0.1188 to 0.1755;
 * a zig-zag cancels to zero moment on the middle joint, which pins the sign. Nothing here breaks
 * (see MomentBreaksTheJointItOverloads). An exhaustive sweep of four-brick chains follows the rows.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureMomentAccumulatesTest,
	"DestructionGame.Core.Structure.MomentAccumulatesAlongTheLoadPath",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureMomentAccumulatesTest::RunTest(const FString& Parameters)
{
	using namespace StructureMomentTestSupport;

	// Expectations are derived from these strengths.
	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against mean f_x1 = 0.7 MPa, the profile carries %g"),
			GeneralPurposeMortar.TensileStrengthMPa),
		GeneralPurposeMortar.TensileStrengthMPa == 0.7);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against mean cohesion 0.9 MPa, the profile carries %g"),
			GeneralPurposeMortar.ShearCohesionMPa),
		GeneralPurposeMortar.ShearCohesionMPa == 0.9);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against compressive 10 MPa, the profile carries %g"),
			GeneralPurposeMortar.CompressiveStrengthMPa),
		GeneralPurposeMortar.CompressiveStrengthMPa == 10.0);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against clay brick at 1.9 g/cm3, the profile carries %g"),
			ClayBrick.DensityGramsPerCubicCm),
		ClayBrick.DensityGramsPerCubicCm == 1.9);

	constexpr double HalfLengthCm = BrickLengthCm / 2.0;
	constexpr double HalfWidthCm = BrickWidthCm / 2.0;
	constexpr double HalfHeightCm = BrickHeightCm / 2.0;
	constexpr double BrickPitchCm = BrickLengthCm + MortarJointCm;

	/** One corbel step: half a brick. */
	constexpr double CorbelStepCm = HalfLengthCm;

	// Moment about Y is resisted by the 3.25 cm depth, not the 5.125 cm width; order matters.
	constexpr double HeadJointModulusCm3 = SectionModulusCm3(HalfWidthCm, HalfHeightCm);
	constexpr double HeadJointAreaSqCm = BrickWidthCm * BrickHeightCm;

	/** A chain with its expected head reading, and the reading without accumulation for the message. */
	struct FChainCase
	{
		const TCHAR* Description;

		/** Brick centres along the wall; the first must be +/- 22.5. */
		TArray<double> BrickCentreXCm;

		/** Head reading without moment accumulation. */
		double UtilisationWithoutAccumulation;

		/** Head reading with it. */
		double ExpectedHeadUtilisation;

		/** Whether the head joint must give. */
		bool bHeadMustGive;
	};

	const TArray<FChainCase> Cases = {
		// Controls: a square stack must not move, or an implementation inventing lever arms passes.
		{
			TEXT("two bricks stacked squarely"),
			{ BrickPitchCm, BrickPitchCm },
			0.11877923076923077,
			0.11877923076923077,
			false
		},

		{
			TEXT("three bricks stacked squarely"),
			{ BrickPitchCm, BrickPitchCm, BrickPitchCm },
			0.17816884615384615,
			0.17816884615384615,
			false
		},

		// The claim: the upper brick's weight acts 16.625 cm from the head joint, not 11.25.
		{
			TEXT("two bricks, the upper one corbelled half a brick out"),
			{ BrickPitchCm, BrickPitchCm + CorbelStepCm },
			0.11877923076923077,
			0.17552930769230769,
			false
		},

		{
			TEXT("three bricks, each corbelled half a brick further out"),
			{ BrickPitchCm, BrickPitchCm + CorbelStepCm, BrickPitchCm + 2.0 * CorbelStepCm },
			0.17816884615384615,
			0.34841907692307692,
			false
		},

		// The sign: the middle bed joint carries two bricks and exactly zero moment.
		{
			TEXT("three bricks zig-zagging back over the joint below"),
			{ BrickPitchCm, BrickPitchCm + CorbelStepCm, BrickPitchCm },
			0.17816884615384615,
			0.23491892307692308,
			false
		},

		// Mirrored: every sign flips, the magnitude must not.
		{
			TEXT("the corbelled pair mirrored, hanging to the left"),
			{ -BrickPitchCm, -(BrickPitchCm + CorbelStepCm) },
			0.11877923076923077,
			0.17552930769230769,
			false
		},
	};

	constexpr double Tolerance = 1.0e-9;

	// Absolute, since one row is exactly zero. Moments reach 1.8e5 uu.cm with ~2e-11 rounding.
	constexpr double MomentToleranceUuCm = 1.0e-6;

	for (const FChainCase& Case : Cases)
	{
		const FHangingChain Chain = MakeHangingChain(Case.BrickCentreXCm);

		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE: every piece and joint of the chain must be accepted"),
				Case.Description),
			Chain.bBuilt);

		if (!Chain.bBuilt)
		{
			continue;
		}

		FStructure Structure = Chain.Structure;
		Structure.SolveLoads();

		// The load path makes every piece determinate: head joint at the bottom, bed joints above.
		TestTrue(
			FString::Printf(TEXT("%s: FIXTURE: joint 0 must be the bottom brick's HEAD joint"),
				Case.Description),
			Structure.GetJointRole(Chain.Joints[0], 1) == EJointRole::Head);

		for (int32 Brick = 1; Brick < Chain.BrickCentreXCm.Num(); ++Brick)
		{
			TestTrue(
				FString::Printf(TEXT("%s: FIXTURE: brick %d must rest on the bed joint beneath it"),
					Case.Description, Brick),
				Structure.GetJointRole(Chain.Joints[Brick], Brick + 1) == EJointRole::BedBeneath);
		}

		for (int32 Piece = 1; Piece <= Chain.BrickCentreXCm.Num(); ++Piece)
		{
			TestTrue(
				FString::Printf(TEXT("%s: FIXTURE: brick %d must be held up, not falling"),
					Case.Description, Piece - 1),
				Structure.GetPieceSupport(Piece) == EPieceSupport::Supported);
		}

		for (int32 Joint = 0; Joint < Chain.Joints.Num(); ++Joint)
		{
			const int32 Index = Chain.Joints[Joint];
			const FConnection& Connection = Structure.GetConnection(Index);

			const double JointCentreXCm = ChainJointCentreXCm(Chain, Joint);

			// Lever arms are measured from this centroid, so pin it.
			TestTrue(
				FString::Printf(
					TEXT("%s: FIXTURE: joint %d's centroid should sit at X = %g, it sits at %g"),
					Case.Description, Joint, JointCentreXCm, Connection.InterfaceCentreCm.X),
				FMath::IsNearlyEqual(Connection.InterfaceCentreCm.X, JointCentreXCm, Tolerance));

			const double ExpectedForceUu = ChainForceUu(Chain, Joint);
			const FVector Force = Structure.GetConnectionForce(Index);

			TestTrue(
				FString::Printf(
					TEXT("%s: FIXTURE: joint %d should carry the %d bricks above it, %g uu, ")
					TEXT("it carries (%g, %g, %g)"),
					Case.Description, Joint, Chain.BrickCentreXCm.Num() - Joint, ExpectedForceUu,
					Force.X, Force.Y, Force.Z),
				FMath::IsNearlyEqual(FMath::Abs(Force.Z), ExpectedForceUu, Tolerance)
					&& FMath::IsNearlyZero(Force.X, Tolerance)
					&& FMath::IsNearlyZero(Force.Y, Tolerance));

			/*
			 * Magnitude only, since the stored sign depends on handle order. The axis must be Y;
			 * an X or Z component would be a twist with no modulus.
			 */
			const double ExpectedMomentUuCm = ChainMomentAboutJointUuCm(Chain, Joint);
			const FVector Moment = Structure.GetConnectionMoment(Index);

			TestTrue(
				FString::Printf(
					TEXT("%s: joint %d carries %s bricks whose weight acts %s cm from its ")
					TEXT("centroid, so it should bend about Y by %s uu.cm; it reads ")
					TEXT("(%s, %s, %s)"),
					Case.Description, Joint,
					*Bits(ExpectedForceUu / BrickWeightUu),
					*Bits(ExpectedMomentUuCm / ExpectedForceUu),
					*Bits(FMath::Abs(ExpectedMomentUuCm)),
					*Bits(Moment.X), *Bits(Moment.Y), *Bits(Moment.Z)),
				FMath::Abs(FMath::Abs(Moment.Y) - FMath::Abs(ExpectedMomentUuCm))
						<= MomentToleranceUuCm
					&& FMath::Abs(Moment.X) <= MomentToleranceUuCm
					&& FMath::Abs(Moment.Z) <= MomentToleranceUuCm);

			// A bed joint's shared span shortens by the step of the brick above.
			const bool bHeadJoint = Joint == 0;

			const double SharedSpanCm = bHeadJoint
				? BrickWidthCm
				: BrickLengthCm
					- FMath::Abs(Chain.Boxes[Joint + 1].CentreCm.X - Chain.Boxes[Joint].CentreCm.X);

			const double AreaSqCm =
				bHeadJoint ? HeadJointAreaSqCm : SharedSpanCm * BrickWidthCm;
			const double ModulusCm3 = bHeadJoint
				? HeadJointModulusCm3
				: SectionModulusCm3(HalfWidthCm, SharedSpanCm / 2.0);

			const double BendingStressMPa =
				FMath::Abs(ExpectedMomentUuCm) / (ModulusCm3 * ForceUnitsPerMPaPerSqCm);

			// Positive in tension. Head joint: all shear. Bed joint: all compression.
			const double NormalStressMPa =
				bHeadJoint ? 0.0 : -ExpectedForceUu / (AreaSqCm * ForceUnitsPerMPaPerSqCm);
			const double ShearStressMPa =
				bHeadJoint ? ExpectedForceUu / (AreaSqCm * ForceUnitsPerMPaPerSqCm) : 0.0;

			/*
			 * Composite relief (ARCHING_DESIGN.md slice 5): the bonded masonry above a bed joint
			 * resists as a deep beam t*D^2/6, D = courses above x pitch, and the joint takes the
			 * lesser reading. Then the bed plane sees only N/A and the worst fibre is the larger of
			 * the two, not their sum. Head joints and the top brick get no relief.
			 */
			const int32 CoursesOfMasonryAbove =
				bHeadJoint ? 0 : Chain.BrickCentreXCm.Num() - Joint;

			const double CompositeDepthCm =
				CoursesOfMasonryAbove * (BrickHeightCm + MortarJointCm);

			const double CompositeModulusCm3 = CoursesOfMasonryAbove >= 2
				? BrickWidthCm * CompositeDepthCm * CompositeDepthCm / 6.0
				: 0.0;

			double PeakTensionMPa = FMath::Max(0.0, NormalStressMPa + BendingStressMPa);
			double PeakCompressionMPa = FMath::Max(0.0, BendingStressMPa - NormalStressMPa);

			if (CompositeModulusCm3 > 0.0)
			{
				const double CompositeStressMPa = FMath::Abs(ExpectedMomentUuCm)
					/ (CompositeModulusCm3 * ForceUnitsPerMPaPerSqCm);

				if (NormalStressMPa <= 0.0 && CompositeStressMPa < PeakTensionMPa)
				{
					PeakTensionMPa = CompositeStressMPa;
					PeakCompressionMPa = FMath::Max(CompositeStressMPa, -NormalStressMPa);
				}
			}

			const double TensionUtilisation =
				PeakTensionMPa / GeneralPurposeMortar.TensileStrengthMPa;
			const double CompressionUtilisation =
				PeakCompressionMPa / GeneralPurposeMortar.CompressiveStrengthMPa;
			const double ShearUtilisation = ShearStressMPa
				/ (GeneralPurposeMortar.ShearCohesionMPa
					+ GeneralPurposeMortar.FrictionCoefficient * FMath::Max(0.0, -NormalStressMPa));

			const double ExpectedUtilisation =
				FMath::Max3(TensionUtilisation, CompressionUtilisation, ShearUtilisation);

			// The head joint's axis is asserted; bed joints may be governed by any axis.
			if (bHeadJoint)
			{
				TestTrue(
					FString::Printf(
						TEXT("%s: FIXTURE PRECONDITION: bending in tension must govern the head ")
						TEXT("joint — tension %.10f vs shear %.10f vs compression %.10f"),
						Case.Description, TensionUtilisation, ShearUtilisation,
						CompressionUtilisation),
					TensionUtilisation > ShearUtilisation
						&& TensionUtilisation > CompressionUtilisation);

				TestTrue(
					FString::Printf(
						TEXT("%s: FIXTURE PRECONDITION: the row claims %.10f and the arithmetic ")
						TEXT("says %.10f"),
						Case.Description, Case.ExpectedHeadUtilisation, ExpectedUtilisation),
					FMath::IsNearlyEqual(
						ExpectedUtilisation, Case.ExpectedHeadUtilisation, Tolerance));
			}

			const double Utilisation = Structure.GetConnectionUtilisation(Index);

			AddInfo(FString::Printf(
				TEXT("%s: joint %d — %s uu.cm on a %s cm3 section, tension %.10f, compression ")
				TEXT("%.10f, shear %.10f"),
				Case.Description, Joint, *Bits(FMath::Abs(ExpectedMomentUuCm)), *Bits(ModulusCm3),
				TensionUtilisation, CompressionUtilisation, ShearUtilisation));

			TestTrue(
				FString::Printf(
					TEXT("%s: joint %d should read %.10f of capacity, it reads %.10f%s"),
					Case.Description, Joint, ExpectedUtilisation, Utilisation,
					bHeadJoint
						? *FString::Printf(TEXT(" (without the accumulation it reads %.10f)"),
							Case.UtilisationWithoutAccumulation)
						: TEXT("")),
				FMath::IsNearlyEqual(Utilisation, ExpectedUtilisation, Tolerance));
		}

		const int32 Passes = Structure.SolveAndBreak();

		TestEqual(
			FString::Printf(
				TEXT("%s: reading %.10f, the cascade should break in %d pass(es), it ran %d"),
				Case.Description, Case.ExpectedHeadUtilisation, Case.bHeadMustGive ? 1 : 0,
				Passes),
			Passes, Case.bHeadMustGive ? 1 : 0);

		TestTrue(
			FString::Printf(
				TEXT("%s: the head joint should%s have given at %.10f of capacity ")
				TEXT("(%.10f without the accumulation)"),
				Case.Description, Case.bHeadMustGive ? TEXT("") : TEXT(" NOT"),
				Case.ExpectedHeadUtilisation, Case.UtilisationWithoutAccumulation),
			Structure.GetConnection(Chain.Joints[0]).HasGiven() == Case.bHeadMustGive);

		// Bed joints are at most a quarter of capacity and must hold.
		for (int32 Joint = 1; Joint < Chain.Joints.Num(); ++Joint)
		{
			TestFalse(
				FString::Printf(TEXT("%s: bed joint %d is well under capacity and must hold"),
					Case.Description, Joint),
				Structure.GetConnection(Chain.Joints[Joint]).HasGiven());
		}
	}

	/*
	 * Exhaustive sweep: every four-brick chain on a quarter-brick step grid (75 chains), covering
	 * partial cancellation and double reversals. Failures print brick centres ready to paste as a
	 * row. The first step only goes away from the pad, or brick 1 would sit over the pad and form an
	 * extra joint.
	 */
	constexpr double SweepOffsetsCm[] = {
		-CorbelStepCm, -CorbelStepCm / 2.0, 0.0, CorbelStepCm / 2.0, CorbelStepCm
	};

	constexpr int32 SweepOffsetCount = UE_ARRAY_COUNT(SweepOffsetsCm);

	int32 SweepChains = 0;
	int32 SweepMismatches = 0;

	for (int32 First = 0; First < SweepOffsetCount; ++First)
	{
		if (SweepOffsetsCm[First] < 0.0)
		{
			continue;
		}

		for (int32 Second = 0; Second < SweepOffsetCount; ++Second)
		{
			for (int32 Third = 0; Third < SweepOffsetCount; ++Third)
			{
				TArray<double> BrickCentreXCm;
				BrickCentreXCm.Add(BrickPitchCm);
				BrickCentreXCm.Add(BrickCentreXCm.Last() + SweepOffsetsCm[First]);
				BrickCentreXCm.Add(BrickCentreXCm.Last() + SweepOffsetsCm[Second]);
				BrickCentreXCm.Add(BrickCentreXCm.Last() + SweepOffsetsCm[Third]);

				const FHangingChain Chain = MakeHangingChain(BrickCentreXCm);
				++SweepChains;

				if (!Chain.bBuilt)
				{
					AddError(FString::Printf(
						TEXT("sweep: a chain at X %g, %g, %g, %g should have built"),
						BrickCentreXCm[0], BrickCentreXCm[1], BrickCentreXCm[2],
						BrickCentreXCm[3]));

					continue;
				}

				FStructure Structure = Chain.Structure;
				Structure.SolveLoads();

				for (int32 Joint = 0; Joint < Chain.Joints.Num(); ++Joint)
				{
					const int32 Index = Chain.Joints[Joint];

					const double ExpectedMomentUuCm = ChainMomentAboutJointUuCm(Chain, Joint);
					const FVector Moment = Structure.GetConnectionMoment(Index);

					const double Utilisation = Structure.GetConnectionUtilisation(Index);

					// Checked explicitly: every comparison against NaN is false, so NaN would pass the bounds.
					const bool bFinite = FMath::IsFinite(Moment.X) && FMath::IsFinite(Moment.Y)
						&& FMath::IsFinite(Moment.Z) && FMath::IsFinite(Utilisation);

					const bool bAgrees = bFinite
						&& FMath::Abs(FMath::Abs(Moment.Y) - FMath::Abs(ExpectedMomentUuCm))
							<= MomentToleranceUuCm
						&& FMath::Abs(Moment.X) <= MomentToleranceUuCm
						&& FMath::Abs(Moment.Z) <= MomentToleranceUuCm;

					if (bAgrees)
					{
						continue;
					}

					++SweepMismatches;

					if (SweepMismatches <= 6)
					{
						AddError(FString::Printf(
							TEXT("sweep: a chain with bricks at X %g, %g, %g, %g carries %s ")
							TEXT("bricks over joint %d at X %g, whose weight acts %s cm from ")
							TEXT("it — the joint should bend about Y by %s uu.cm, it reads ")
							TEXT("(%s, %s, %s) and %s of capacity"),
							BrickCentreXCm[0], BrickCentreXCm[1], BrickCentreXCm[2],
							BrickCentreXCm[3],
							*Bits(ChainForceUu(Chain, Joint) / BrickWeightUu), Joint,
							ChainJointCentreXCm(Chain, Joint),
							*Bits(ExpectedMomentUuCm / ChainForceUu(Chain, Joint)),
							*Bits(FMath::Abs(ExpectedMomentUuCm)),
							*Bits(Moment.X), *Bits(Moment.Y), *Bits(Moment.Z),
							*Bits(Utilisation)));
					}
				}
			}
		}
	}

	AddInfo(FString::Printf(
		TEXT("the sweep walked %d chains of 4 bricks on a quarter-brick grid"), SweepChains));

	TestEqual(
		FString::Printf(
			TEXT("every joint of every chain must carry the moment of what is above it; ")
			TEXT("%d of %d joints across %d chains disagree"),
			SweepMismatches, SweepChains * 4, SweepChains),
		SweepMismatches, 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
