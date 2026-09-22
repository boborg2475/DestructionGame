// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"
#include "Core/StructureBinding.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Named, not anonymous: a unity build merges translation units, so two anonymous namespaces
 * become one and identically-named helpers collide at link. See CURRENT_STATE.md.
 */
namespace StructureMomentTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * Everything below is spelled from first principles rather than imported: a test that
	 * reuses production's own constants cannot disagree with production. Strengths are
	 * asserted against the profile, not copied from it.
	 */

	/** DESIGN.md's standard UK metric clay brick, cm. */
	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickWidthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;

	/** The standard 1 cm mortar joint, which is what makes the coordinating grid 22.5 cm. */
	constexpr double MortarJointCm = 1.0;

	/**
	 * 1.9 g/cm3, density first in the product to match Layout::PieceMassKg bit for bit.
	 * Volume-first lands one ulp low and the exact-equality guard below would reject it.
	 */
	constexpr double BrickMassKg = 1.9 * BrickLengthCm * BrickWidthCm * BrickHeightCm / 1000.0;

	/** 980 cm/s2. With 1 uu = 1 cm and mass in kg, MassKg * 980 is a force in uu. */
	constexpr double BrickWeightUu = BrickMassKg * 980.0;

	/**
	 * Force, in uu, that loads 1 cm2 to 1 MPa. 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa
	 * over 1 cm2 is 10000 uu. Not DestructionForce::ForceUnitsPerMPaSqCm on purpose: this
	 * test must fail if that constant is wrong, not agree with it.
	 */
	constexpr double ForceUnitsPerMPaPerSqCm = 100.0 * 100.0;

	/**
	 * Elastic section modulus of a rectangle, cm3 — ordinary beam theory, not a code figure.
	 * Bent along the second axis, W = I/c = (4/3)*HalfAlong*HalfAcross^2.
	 */
	constexpr double SectionModulusCm3(double HalfAlongCm, double HalfAcrossCm)
	{
		return (4.0 / 3.0) * HalfAlongCm * HalfAcrossCm * HalfAcrossCm;
	}

	/** A box the size of a whole brick, centred where it is asked for. */
	FPieceBox BrickBoxAt(double CentreXCm, double CentreYCm, double CentreZCm)
	{
		FPieceBox Box;
		Box.CentreCm = FVector(CentreXCm, CentreYCm, CentreZCm);
		Box.ExtentCm = FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm) * 0.5;
		return Box;
	}

	/** Print a double so a comparison that failed in the last bit is readable as one. */
	FString Bits(double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	/**
	 * A chain of bricks hanging off one head joint, each free to sit anywhere along the wall.
	 * A grounded pad at the origin, then one brick per course, each jointed only to the one
	 * below — so every brick rests on exactly one joint (N = 1, the determinate case).
	 *
	 * Vertical, not horizontal: a horizontal row has every piece falling back on all its head
	 * joints, a cycle the solver strands rather than solves.
	 *
	 * Handle and box index match: 0 is the pad, brick k is handle k + 1. Joint k is under
	 * brick k, so joint 0 is the head joint.
	 */
	struct FHangingChain
	{
		FStructure Structure;

		/** One box per piece handle, the pad first. */
		TArray<FPieceBox> Boxes;

		/** X centre of each BRICK, index 0 being the brick on the head joint. */
		TArray<double> BrickCentreXCm;

		/** One joint handle per brick: joint k is the joint under brick k. */
		TArray<int32> Joints;

		/** Whether every piece and every joint of it was accepted. */
		bool bBuilt = true;
	};

	/**
	 * Build one from each brick's centre along the wall. The first brick must be one
	 * coordinating cell from the pad (+/- 22.5) or there is no head joint; bricks above may
	 * sit anywhere still overlapping the one beneath. Courses are a brick plus a joint apart,
	 * so consecutive bricks share a bed joint.
	 */
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

	/**
	 * Where joint k sits along the wall, derived rather than read off the joint. Both boxes
	 * are a brick long, so the shared span's midpoint is the midpoint of the two centres. The
	 * test asserts the emitted centroid against this, since every lever arm is measured from
	 * it and the two must fail together.
	 */
	double ChainJointCentreXCm(const FHangingChain& Chain, int32 Joint)
	{
		return (Chain.Boxes[Joint].CentreCm.X + Chain.Boxes[Joint + 1].CentreCm.X) * 0.5;
	}

	/**
	 * The oracle: the moment everything above joint k exerts about joint k's centroid, uu.cm.
	 * One sum over the bricks the joint carries, each weight times its own distance from the
	 * joint — no recursion, no load path. Derived differently from the accumulation it checks,
	 * so the two must meet in the middle rather than agree by construction.
	 *
	 * Signed by the distance. Only the magnitude reaches a stress, so assertions compare
	 * magnitudes; the sign makes cancellation real — a zig-zag chain sums to exactly zero,
	 * which summing magnitudes could not produce.
	 *
	 * Only X matters: gravity is vertical, so every moment here is about world Y.
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

	/** How many brick weights joint k of a chain carries: everything above it. */
	double ChainForceUu(const FHangingChain& Chain, int32 Joint)
	{
		return (Chain.BrickCentreXCm.Num() - Joint) * BrickWeightUu;
	}
}

/**
 * A brick held by one head joint peels off it rather than shearing through it — the whole
 * point of the moment work, first seen in a structure rather than a hand-built load.
 *
 * Gravity runs parallel to a head joint, so the mean normal stress across it is exactly zero
 * and the averaged reading (0.0044481, shear against cohesion) says the brick is fine. What
 * is physically happening is that its weight acts 11.25 cm to one side and levers the top of
 * the joint open, against a tensile strength a fourteenth of the compressive one.
 *
 * The eccentricity is 11.25 cm, not 10.75, and it is arbitrated against the emitted centroid.
 * MakeInterface places the centroid at the mid-plane of the mortar (a 1 cm bed has two contact
 * planes, so a face-based centroid would depend on which brick was named first). From a head
 * joint at X = 11.25 the brick's mass at X = 22.5 is 11.25 cm away, giving 0.0594. The joint
 * geometry is asserted below so the number and its reason fail together.
 *
 * The arithmetic, from published figures and brick dimensions (mean basis since the 2026-08-13
 * re-anchor; only the divisor changed, 0.10 -> 0.70):
 *
 *     W       = 1.9 * 21.5 * 10.25 * 6.5 / 1000 * 980  = 2667.198625 uu
 *     M       = 2667.198625 * 11.25                    = 30005.98453125 uu.cm
 *     W_sec   = (4/3) * 5.125 * 3.25^2                 = 72.177083... cm3
 *     sigma_b = M / W_sec                              = 415.7273077 uu/cm2 = 0.04157 MPa
 *     tension = sigma_b / mean f_x1 (0.70 MPa)         = 0.0593896154
 *
 * No new conversion boundary: length is cm, so M/W is uu/cm2, the same quantity a force over
 * an area is, divided by the same 10000 (from 1 N = 100 uu). A stray factor of 100 surfaces here.
 *
 * Which axis governs is the whole risk: ComputeUtilisation returns the worst of three, so a
 * test aimed at bending would silently measure shear if shear were higher. Every row asserts
 * its own axis comparison as a precondition.
 *
 * N = 1 only. A piece on several supports is statically indeterminate and splitting the moment
 * per joint would peel every bed joint in a standing wall; on one support it is determinate and
 * exact. Structure.SymmetricSupportsCarryNoMoment is the other half of this.
 *
 * No world, no tick, no gravity setting: the 980 is the solver's own constant. The assertion is
 * on the mechanism — a utilisation ratio — as DESIGN.md §4 asks of a unit test.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureHangingBrickPeelsTest,
	"DestructionGame.Core.Structure.HangingBrickPeelsRatherThanShears",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureHangingBrickPeelsTest::RunTest(const FString& Parameters)
{
	using namespace StructureMomentTestSupport;

	/*
	 * The expectations are ratios of published strengths, so they hold only while the profile
	 * carries the figures they were derived against; a retune must fail loudly here. Mean
	 * flexural bond f_x1 = 0.70 MPa (re-anchor 2026-08-13, Gooch et al. 2023, ConBuildMat
	 * 386:131578) is the divisor; the other two decide which axis governs. Friction gives shear
	 * no help here — the mean compressive stress on a head joint under gravity is zero.
	 */
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

	/**
	 * One grounded pad, one brick hanging off it, one head joint between. The pad
	 * terminates the load, so its own mass and centre never reach the answer. The hanging
	 * brick has no bed joint, so its single head joint is its support — the fallback
	 * DESIGN.md §3 describes, N = 1 by definition.
	 */
	struct FPeelCase
	{
		const TCHAR* Description;

		FPieceBox PadBox;
		FPieceBox HangingBox;

		/**
		 * Whether the pad is MakeInterface's first handle, i.e. whether the normal points at
		 * the hanging brick or away. ConnectionLoad.h's force belongs to PieceB, so naming the
		 * loaded piece first stores the reaction pointing up. The answer must not depend on
		 * declaration order — that is what this row checks.
		 */
		bool bPadIsPieceA;

		/** What Layout::MakeInterface must emit for this pair — the arbitration. */
		FVector ExpectedJointCentreCm;
		FVector ExpectedJointHalfExtentCm;
		double ExpectedAreaSqCm;

		/** The offset from the joint's own centroid to the brick's centre of mass, cm. */
		double ExpectedLeverArmCm;

		/** Section modulus about the axis the joint is actually bent about, cm3. */
		double ExpectedModulusCm3;
	};

	/*
	 * A brick's half-extents, so the joint rectangles below read as the faces they are:
	 * a head joint is the 10.25 x 6.5 end of a brick, half-extents 5.125 and 3.25.
	 */
	constexpr double HalfLengthCm = BrickLengthCm / 2.0;
	constexpr double HalfWidthCm = BrickWidthCm / 2.0;
	constexpr double HalfHeightCm = BrickHeightCm / 2.0;

	/** One coordinating cell along the wall: a brick plus a joint. */
	constexpr double BrickPitchCm = BrickLengthCm + MortarJointCm;

	const TArray<FPeelCase> Cases = {
		/*
		 * The headline row. Pad at the origin, brick one cell along, so the head joint's
		 * mid-plane sits at X = 11.25 and the brick's mass acts at X = 22.5: 11.25 cm of lever
		 * arm against 3.25 cm of depth. Not 10.75 — see the mid-plane note above.
		 */
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

		/*
		 * The same joint mirrored. The brick hangs left of the pad, so the normal is -X and
		 * the lever arm points the other way. The sign of a moment says which edge opens, not
		 * how hard, so the answer must be identical; letting the sign reach the stress would
		 * read zero or double here.
		 */
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

		/*
		 * The loaded piece declared first, which stores the force pointing up. Same wall, same
		 * brick, same joint; only the handle order changed, and nothing physical may.
		 */
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
		 * A joint both bent and twisted, where only the bending may count. The pad is
		 * displaced across the wall and along it, so the boxes separate on Y and overlap half
		 * on X: a 10.75 x 6.5 face centred at X = 5.375, mass acting at X = 10.75. That leaves
		 * two offsets — 5.625 cm along the normal (bending) and 5.375 cm across the face
		 * (torsion about the normal).
		 *
		 * (p - c) x F produces both, as components about world X and Y. For a Y-normal joint
		 * the Y component is torsion — second order for gravity, needing a modulus this face
		 * lacks, and out of scope. It must be dropped, not folded in; feeding it in reads 0.38
		 * rather than 0.198. This row also exercises a different separation axis and modulus,
		 * so an implementation assuming an X normal, or pairing the wrong in-plane modulus,
		 * fails here while passing every row above.
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
		/*
		 * Real geometry throughout: the joint comes from the producer, not hand-assembly,
		 * because the emitted centroid is what the expected number is arbitrated against. A
		 * hand-written rectangle could agree with a hand-written expectation and both be wrong.
		 */
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

		/*
		 * The arbitration, asserted rather than assumed. The lever arm is measured from this
		 * centroid, so if the producer moved it to a brick face the expected utilisation would
		 * silently stop describing this joint. Pinned so the two fail together.
		 */
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

		/*
		 * What this joint must read, and why tension says so. Three utilisations, each stress
		 * against its own capacity. Bending reaches the tension and compression edges equally
		 * (the joint pivots about its centroid) and the mean normal stress is zero, so peak
		 * tension and compression are both sigma_b; what separates them is that mortar resists
		 * crushing far better than being pulled open.
		 */
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

		/*
		 * And it must still stand. A single brick on a single head joint hangs there; it is
		 * now a seventeenth of the way off rather than a two-hundredth. A row expecting over
		 * 1.0 would assert a collapse this fixture does not have.
		 */
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

		/*
		 * The load path is what makes this N = 1, so it is asserted. The joint must be the
		 * brick's head joint (a bed joint would win the tier and leave no fallback), the brick
		 * must be held up by it, and the whole weight must flow through it — a share would mean
		 * a second support had appeared.
		 */
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

		// --- the claim ---------------------------------------------------------------

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
 * A brick sitting squarely on two supports loads them exactly as before, bit for bit.
 *
 * This matters more than the headline, and is the trap MOMENTS_DESIGN.md records. The obvious
 * rule — every supporting joint carries (p - c_j) x S_j — is wrong: on a symmetric running-bond
 * brick it gives each bed joint about 0.029 in tension, because the moments cancel across the
 * pair, not on either one. The correct statics: a piece on several supports is indeterminate,
 * determinate on exactly one. So N = 1 is exact and N >= 2 keeps the area split with zero
 * moment, which is unconservative only when the centre of mass is off the supports' centroid —
 * and in a symmetric bond it is not, so the intact wall does not move.
 *
 * The fixture is the producer's own flush wall and its twin, rebuilt from the same pieces and
 * joints but added through the two-argument door so they carry no centre of mass. The only
 * variable is whether anyone said where the weight acts. Flush, not ragged: half bats land on
 * one brick, so the wall has N = 1 paths too, all at zero eccentricity.
 *
 * Exact equality is the assertion. A 1e-9 tolerance would pass a rearrangement of a few ulps,
 * and the cascade fuzz has joints settling at exactly 1.0, so one ulp of drift is spurious
 * break failures.
 *
 * Red on arrival for one reason: its precondition. The comparison measures nothing until Layout
 * supplies the centres of mass, so HasCompleteGeometry is asserted first. Once green this is a
 * regression net, not a driver.
 *
 * No world, no tick.
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

	/*
	 * The precondition the test hangs on. Until the producer places its pieces both halves of
	 * the comparison are geometry-free and agreeing proves nothing. This is the one red step here.
	 */
	TestTrue(
		TEXT("FIXTURE PRECONDITION: a wall laid by the producer must know where every piece ")
		TEXT("and every joint is, or the comparison below is a structure against itself"),
		Laid.Structure.HasCompleteGeometry());

	/*
	 * The twin: same masses, grounded flags and joints (rectangles and all) but no centres of
	 * mass — the state every fixture in this suite is in.
	 */
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

	/*
	 * A comparison over joints carrying nothing would pass trivially, so the wall is checked to
	 * be doing something first — thousandths of capacity, since masonry is overbuilt in compression.
	 */
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

		/*
		 * The force must not move either, a separate claim. Moments ride alongside the routed
		 * force rather than changing it, so a change here would mean the routing itself was
		 * disturbed — a different and worse failure than a joint reading a stray moment.
		 */
		const bool bAgrees = Placed == Unplaced && PlacedForce == UnplacedForce;

		if (!bAgrees)
		{
			++Mismatches;

			// So a wall that is wrong everywhere does not print fifty failures.
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
 * "Nobody supplied positions" and "the load happens to be centred" are different states, and
 * HasCompleteGeometry is the one thing that tells them apart.
 *
 * They give the identical answer everywhere else on purpose: sigma = N/A +/- M/W collapses to
 * N/A exactly when the moment is zero, which is why every geometry-free fixture and both fuzz
 * generators still work. This buys back the ability to ask which one you are looking at — the
 * same trade HasSupportAnswer makes against EPieceSupport::Falling.
 *
 * A conjunction: a moment needs a point for the load and a rectangle for the joint, so both
 * must be present or some joint is answering a centred load with no choice. Slice 2 deferred
 * this deliberately — with rectangles but no centres of mass it could not be written honestly.
 *
 * Over what is still in the structure: a removed piece and a given joint are out of the graph,
 * so a tombstone must not condemn a live structure that is fully described. Same scoping every
 * other FStructure accessor uses.
 *
 * One decision worth flagging: an empty structure reads true, because an empty conjunction is
 * true and that keeps the predicate composable. The fail-closed reading is defensible and cheap
 * to swap; what is not defensible is pieces with no positions reading true — the row that matters.
 *
 * No world, no tick.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureCompleteGeometryTest,
	"DestructionGame.Core.Structure.GeometryIsCompleteOrItIsNot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureCompleteGeometryTest::RunTest(const FString& Parameters)
{
	using namespace StructureMomentTestSupport;

	/**
	 * Free function pointers rather than TFunction, matching PieceActions.h: these rows capture
	 * nothing, so no allocation.
	 */
	struct FGeometryCase
	{
		const TCHAR* Description;
		void (*Build)(FStructure&);
		bool bExpected;
	};

	const TArray<FGeometryCase> Cases = {
		/*
		 * The empty conjunction. See the decision note above — this is the row to change if
		 * the fail-closed reading is preferred, and nothing else here moves with it.
		 */
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

		/*
		 * The row that matters. Every fixture in the suite and both fuzz generators: a valid
		 * structure with no positions. It must be distinguishable from one whose loads happen
		 * to be centred, or the accessor has no purpose.
		 */
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

		/*
		 * Half a conjunction is not a weaker version of it. The pieces are placed and the joint
		 * is not, so its lever arm is unmeasurable and every load through it is answered as
		 * centred — the state slice 2 left behind, seen from the other side.
		 */
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

		/*
		 * What has left the structure cannot spoil it. Piece 2 was never placed and its joint
		 * never had a rectangle; removing it tombstones the piece and severs the joint, leaving
		 * a live half that is fully described. A predicate walking the raw arrays would read
		 * false forever — useless the first time a player pulls a brick.
		 */
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
 * A wall that went through the binding is still a placed wall — today it is not, so every wall
 * the game builds computes no moments at all.
 *
 * Where the geometry is dropped: RunningBond places every piece. AdoptLayout replays the
 * layout into an FStructureBinding, and FStructureBinding::AddPiece keeps the box but forwards
 * only mass and groundedness to the solver. The centre of mass falls on the floor between them.
 *
 * Which is invisible: an unplaced piece loads its joints exactly as a centred one does (that
 * exactness is deliberate, and lets every geometry-free fixture keep working), so a binding
 * that lost every centre of mass reports a wall standing happily, with the same forces and
 * support answers. Nothing throws, and slice 3 is simply absent from play.
 *
 * Two claims. First, HasCompleteGeometry on the binding-built structure: it fails for the
 * whole class of droppage — any piece, any field — not one number on one joint, and keeps
 * meaning the same when the fixture below is retired. Second, one joint of one real wall
 * worked end to end, deliberately a joint whose answer moves, since a predicate can pass
 * without anything downstream using what it promises.
 *
 * The fixture is the narrow-waist wall (BrickWorldTestSupport::NarrowWaistWallSpec(3), spelled
 * out here to avoid dragging in UWorld and the input subsystem); its shape is asserted below.
 *
 *      course 2         [ 3 ][ 4 ]
 *      course 1            [ 2 ]        THE WAIST
 *      course 0         [ 0 ][ 1 ]      grounded
 *
 * Ragged, two full bricks per course, so the odd course is a single brick. Bricks 3 and 4 each
 * land on the waist and nothing else — N = 1, MOMENTS_DESIGN.md case (c).
 *
 * The arithmetic, from the emitted rectangle and published strengths. The waist brick sits at
 * x = 11.25 spanning 0.5 .. 22.0; brick 3 at x = 0 spanning -10.75 .. 10.75, so the shared
 * patch is x 0.5 .. 10.75 by the full 10.25 of depth:
 *
 *     A       = 10.25 * 10.25                          = 105.0625 cm2
 *     centre  = ((0.5 + 10.75) / 2, 0, 14.5)             the mid-plane of the mortar
 *     e       = 5.625 - 0                              = 5.625 cm
 *     W_v     = (4/3) * 5.125 * 5.125^2                = 179.4817708 cm3
 *     W       = 1.9 * 21.5 * 10.25 * 6.5 / 1000 * 980  = 2667.198625 uu
 *     sigma_b = 2667.198625 * 5.625 / W_v              = 8.3590619e-3 MPa
 *     sigma_n = -2667.198625 / A                       = -2.5386781e-3 MPa, compression
 *     tension = max(0, sigma_n + sigma_b) / 0.70 MPa   = 0.0083148340
 *
 * against 2.5386781e-4 a centred load reads — a 33x change, and the joint still holds. (Mean
 * basis since the 2026-08-13 re-anchor; only the 0.10 -> 0.70 divisor moved.)
 *
 * Which axis governs is asserted, not assumed. ComputeUtilisation returns the worst of three,
 * and compression governs this joint today; bending in tension is the axis this test overtakes,
 * so both are compared before either is claimed.
 *
 * The laid wall is the control, asserted first: the producer already places its pieces, so its
 * own structure reads the eccentric figure and only the binding copy does not. Checking both
 * says where the geometry was lost, not just that it is missing.
 *
 * No world, no tick, no actors. AdoptLayout stores pointers into weak pointers and never
 * resolves one, so the stand-ins are null; the piece-to-actor half is covered in
 * StructureBinding.AdoptLayout.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureBindingAdoptedWallGeometryTest,
	"DestructionGame.Core.StructureBinding.AdoptedWallLoadsItsWaistEccentrically",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureBindingAdoptedWallGeometryTest::RunTest(const FString& Parameters)
{
	using namespace StructureMomentTestSupport;

	/*
	 * The expected numbers are ratios of published strengths, so they hold only while the
	 * profile carries the figures they were derived against.
	 */
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

	/*
	 * The shape, pinned. Five pieces and six joints is the wall drawn above; a spec that
	 * stopped producing a waist would make every claim below meaningless while still passing.
	 */
	constexpr int32 WaistedBrick = 3;

	TestEqual(
		FString::Printf(TEXT("FIXTURE: a ragged 3-course wall 2 bricks wide should be 5 pieces, got %d"),
			Laid.Structure.NumPieces()),
		Laid.Structure.NumPieces(), 5);

	TestEqual(
		FString::Printf(TEXT("FIXTURE: it should carry 6 joints, got %d"),
			Laid.Structure.NumConnections()),
		Laid.Structure.NumConnections(), 6);

	/*
	 * The fixture hangs on this: the producer must already place its pieces. If it did not,
	 * the binding would lose nothing and this would measure two geometry-free structures.
	 */
	TestTrue(
		TEXT("FIXTURE: the laid wall must already know where every piece and every joint is"),
		Laid.Structure.HasCompleteGeometry());

	// --- what the joint under the waisted brick must read, worked from the grid ---------

	constexpr double HalfLengthCm = BrickLengthCm / 2.0;
	constexpr double BrickPitchCm = BrickLengthCm + MortarJointCm;
	constexpr double CoursePitchCm = BrickHeightCm + MortarJointCm;

	/** Running bond offsets alternate courses by half a cell; that is where the waist sits. */
	constexpr double BondOffsetCm = BrickPitchCm / 2.0;

	/*
	 * The shared span along the wall: from the waist's left face to the overhanging brick's
	 * right face. Half a brick of overlap, which is what a running-bond bed patch is.
	 */
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
	 * A bed joint separates on Z, so its in-plane frame is X and Y; the moment is about Y,
	 * resisted by the depth along X. This patch is square, so the two moduli are equal and this
	 * fixture cannot tell a swapped pair apart — that order is pinned in
	 * Structure.HangingBrickPeelsRatherThanShears, on a face whose moduli differ by 2.5x.
	 */
	const double BedPatchModulusCm3 = SectionModulusCm3(BedPatchHalfYCm, BedPatchHalfXCm);

	const double BendingStressMPa =
		(BrickWeightUu * LeverArmCm) / (BedPatchModulusCm3 * ForceUnitsPerMPaPerSqCm);

	// Signed, positive in tension: a brick pressing down on a bed joint compresses it.
	const double NormalStressMPa =
		-BrickWeightUu / (BedPatchAreaSqCm * ForceUnitsPerMPaPerSqCm);

	const double PeakTensileStressMPa = FMath::Max(0.0, NormalStressMPa + BendingStressMPa);
	const double PeakCompressiveStressMPa = FMath::Max(0.0, BendingStressMPa - NormalStressMPa);

	const double ExpectedUtilisation =
		PeakTensileStressMPa / GeneralPurposeMortar.TensileStrengthMPa;
	const double CompressionUtilisation =
		PeakCompressiveStressMPa / GeneralPurposeMortar.CompressiveStrengthMPa;

	// What a joint reads when nobody said where the load acts: the averaged stress alone.
	const double CentredUtilisation =
		-NormalStressMPa / GeneralPurposeMortar.CompressiveStrengthMPa;

	/*
	 * The precondition that stops this measuring the wrong axis. Compression governs this joint
	 * today, so bending in tension must overtake it, or the number would not move across the
	 * fix. Shear is exactly zero: gravity is normal to a bed joint.
	 */
	TestTrue(
		FString::Printf(
			TEXT("FIXTURE PRECONDITION: bending in tension must govern — tension %.10f vs ")
			TEXT("compression %.10f vs centred %.10f"),
			ExpectedUtilisation, CompressionUtilisation, CentredUtilisation),
		ExpectedUtilisation > CompressionUtilisation && ExpectedUtilisation > CentredUtilisation);

	/*
	 * And the wall must still stand. One brick overhanging half its bed holds; it is now a
	 * hundred-and-twentieth of the way off rather than a four-thousandth.
	 */
	TestTrue(
		FString::Printf(TEXT("FIXTURE PRECONDITION: the wall must still stand, expected %.10f"),
			ExpectedUtilisation),
		ExpectedUtilisation < 1.0);

	AddInfo(FString::Printf(
		TEXT("the waisted brick's bed joint: %s eccentric against %s centred"),
		*Bits(ExpectedUtilisation), *Bits(CentredUtilisation)));

	/*
	 * The joint is found by role, not index: the handle is RunningBond's pairing detail and
	 * hard-coding it would fail for a reason unrelated to geometry. Finding exactly one bed
	 * joint beneath the brick is also the N = 1 precondition the moment rule needs.
	 */
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

	// --- the control: the producer's own structure already reads the eccentric figure ----

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

	/*
	 * Claim one, failing for the whole class. Whatever the binding drops — a centre of mass
	 * today, a field nobody has invented yet tomorrow — a wall that went through it must be as
	 * completely described as the wall that went in.
	 */
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

	/*
	 * The routing must be identical, a separate claim. Moments ride alongside the routed force
	 * rather than changing it, so the two structures must agree here exactly; a difference would
	 * mean the replay disturbed the load path — a worse fault than a lost centre of mass.
	 */
	const FVector LaidForce = Laid.Structure.GetConnectionForce(LaidJoint);
	const FVector BoundForce = Binding.GetStructure().GetConnectionForce(BoundJoint);

	TestTrue(
		FString::Printf(
			TEXT("FIXTURE: adoption must route the same load, laid (%s, %s, %s) vs adopted (%s, %s, %s)"),
			*Bits(LaidForce.X), *Bits(LaidForce.Y), *Bits(LaidForce.Z),
			*Bits(BoundForce.X), *Bits(BoundForce.Y), *Bits(BoundForce.Z)),
		LaidForce == BoundForce);

	/*
	 * Claim two. Same wall, same joint, same force, so the only thing that can move the answer
	 * is whether anybody said where the brick's weight acts.
	 */
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
 * A moment breaks the joint it overloads: under the first-crack brittleness model the head
 * joint past its elastic section-modulus capacity actually gives, and the chain hanging off it
 * comes down.
 *
 * This reading has swung twice and now restores its original premise (first-crack promotion,
 * 2026-08-28). (1) Before the equilibrium LP was the break authority, the per-joint sweep did
 * not see the eccentric moment, so an over-capacity joint held forever. (2) The LP became the
 * authority and reasoned in plastic limit analysis — a no-tension block carries ~2.78x the
 * demand — so it charitably stood the chain, and the test was re-pinned to "carried, not
 * broken". (3) First crack flips the LP's bonded-joint bending capacity to the uncracked
 * peak-fibre limit (3x stricter), which is exactly production's elastic readout (W = A*h/3), so
 * the LP converges back onto the readout, the head joint is found infeasible, and the chain falls.
 *
 * The fall boundary is the elastic-readout crossing, and that coincidence is the physics. First
 * crack is sigma = -N/A + |M|/W <= f_t; on a head joint under gravity N = 0, so it reduces to
 * |M|/W <= f_t — exactly what GetConnectionUtilisation reports. So lambda* = 1 / readout and the
 * chain falls exactly when the readout crosses 1.0. The test pins (readout > 1.0) == (chain falls).
 *
 * The arithmetic. Under slice 3's rule the moment on a determinate joint is this piece's own
 * weight plus what it received from above, acting at its own centre (the arriving load does not
 * yet carry its own lever arm, that is slice 5). So the head joint's moment is the chain weight
 * times 11.25 cm, linear in the number of bricks:
 *
 *     W       = 1.9 * 21.5 * 10.25 * 6.5 / 1000 * 980  = 2667.198625 uu per brick
 *     e       = 22.5 / 2                               = 11.25 cm to the mid-plane
 *     W_u     = (4/3) * 5.125 * 3.25^2                 = 72.1770833 cm3
 *     tension = n * 2667.198625 * 11.25 / W_u / 0.70   = n * 0.059389616  (sigma_n = 0)
 *
 * n = 16 gives 0.9502 (lambda* 1.0524 >= 1) and holds; n = 17 gives 1.0096 (lambda* 0.9905 < 1)
 * and gives. So sixteen stands and seventeen falls — the discriminating boundary. The sixteen
 * chain is not a red fixture; it is the control that stops an implementation which breaks
 * whatever it is shown.
 *
 * The fixture is a corbel, not a row: a horizontal chain is a cycle the solver strands. Stacked
 * vertically off one hanging brick, each upper brick gets a bed joint and the hanging brick has
 * exactly one head joint:
 *
 *              [ C ]
 *              [ B ]
 *     [ pad ]  [ A ]        one head joint, the whole chain hanging off it
 *     =======
 *
 * Which axis governs is asserted: on a head joint the mean normal stress is zero, so at n = 17
 * tension reads 1.0096 against shear 0.0756 and compression 0.0707. Bending in tension must take
 * the joint apart; a fixture where shear got there first would be a different test.
 *
 * Asserted on the mechanism and support state, never on movement: FStructure has no positions.
 * The falling row's claim is the head joint's own state (given, break-pass stamped) plus every
 * hanging brick reading Falling; the standing row's is the mirror (nothing given, zero passes,
 * every brick Supported).
 *
 * No world, no tick. The break decision is the equilibrium LP over a graph, not a simulation.
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

	/** A head joint is the 10.25 x 6.5 end of a brick, and its mid-plane is half a cell along. */
	constexpr double HeadJointAreaSqCm = BrickWidthCm * BrickHeightCm;
	constexpr double LeverArmCm = BrickPitchCm / 2.0;

	/*
	 * A head joint separates on X, so its in-plane frame is Y and Z; the moment is about Y,
	 * resisted by the joint's 3.25 cm of depth, not its 5.125 cm of width. The two moduli differ
	 * by 1.58x, so the pair being the right way round is load-bearing here.
	 */
	constexpr double HeadJointModulusCm3 = SectionModulusCm3(HalfWidthCm, HalfHeightCm);

	struct FChainCase
	{
		const TCHAR* Description;

		/** How many bricks hang off the one head joint. */
		int32 BricksInChain;

		/**
		 * Whether the head joint gives under first crack. On a zero-normal head joint first crack
		 * is the uncracked peak-fibre condition, so this equals "the elastic readout crosses 1.0";
		 * the test asserts that coincidence, (readout > 1.0) == bFallsAtFirstCrack. True: chain
		 * falls (given, stamped, bricks Falling). False: chain stands (bricks Supported).
		 */
		bool bFallsAtFirstCrack;
	};

	const TArray<FChainCase> Cases = {
		/*
		 * The control. Sixteen bricks put the joint at 95% of its elastic capacity (0.9502339)
		 * and it stands: lambda* 1.0524 >= 1, the cascade breaks nothing. (The mean re-anchor
		 * moved the crossing to seventeen brick weights.)
		 */
		{ TEXT("sixteen bricks hanging off one head joint"), 16, false },

		/*
		 * The claim, re-inverted by the first-crack promotion (2026-08-28). The seventeenth brick
		 * takes the elastic readout to 1.0096, and first crack makes the LP's bending capacity that
		 * same peak-fibre limit, so the head joint is infeasible (lambda* 0.9905 < 1) and gives.
		 * This retires the plastic-stands re-pin; the chain falls.
		 */
		{ TEXT("seventeen bricks hanging off one head joint"), 17, true },
	};

	constexpr double Tolerance = 1.0e-9;

	for (const FChainCase& Case : Cases)
	{
		/*
		 * A grounded pad, then a stack of bricks beside it; the lowest is joined to the pad by a
		 * head joint and nothing else. Each brick above sits squarely on the one below, so its
		 * lever arm is zero and it adds weight to the head joint without eccentricity of its own.
		 */
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

		// --- what the head joint carries, and which axis says so -------------------------

		const double ChainWeightUu = Case.BricksInChain * BrickWeightUu;

		const double BendingStressMPa =
			(ChainWeightUu * LeverArmCm) / (HeadJointModulusCm3 * ForceUnitsPerMPaPerSqCm);

		// Gravity runs parallel to a head joint, so the whole force is shear and sigma_n is 0.
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

		/*
		 * The coincidence that is the physics, restated against the arithmetic: first crack on a
		 * zero-normal head joint reduces to |M|/W <= f_t, exactly the elastic readout, so the
		 * joint falls when the readout crosses 1.0. Fall flag and readout-crossing are one boolean.
		 */
		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE PRECONDITION: the case says the head joint %s at first crack and ")
				TEXT("the ELASTIC readout arithmetic says %.10f (falls iff readout > 1.0)"),
				Case.Description, Case.bFallsAtFirstCrack ? TEXT("falls") : TEXT("stands"),
				ExpectedUtilisation),
			(ExpectedUtilisation > 1.0) == Case.bFallsAtFirstCrack);

		/*
		 * The readout already sees it, and asserting that first keeps the failure below
		 * unambiguous. Slice 3 landed the moment into GetConnectionUtilisation; if this goes red
		 * the fault is upstream of the break decision.
		 */
		Structure.SolveLoads();

		const double ReadUtilisation = Structure.GetConnectionUtilisation(HeadIndex);

		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE: the strain readout should say %.10f, it says %.10f"),
				Case.Description, ExpectedUtilisation, ReadUtilisation),
			FMath::IsNearlyEqual(ReadUtilisation, ExpectedUtilisation, Tolerance));

		// --- the claim: first crack breaks the joint the moment overloads ----------------

		/*
		 * The break decision is the first-crack LP. The eighteen-piece structure sits below the
		 * 200-block cap, so the equilibrium LP decides. The head joint's bending capacity is its
		 * peak-fibre limit — the display's section-modulus reading — so seventeen bricks are
		 * infeasible (lambda* 0.9905) and the joint gives; sixteen (lambda* 1.0524) is admissible.
		 */
		const int32 Passes = Structure.SolveAndBreak();

		if (Case.bFallsAtFirstCrack)
		{
			/*
			 * The falling row (seventeen bricks). Measured 2026-08-28: the cascade runs 2 passes,
			 * the head joint gives on pass 1, the whole hang comes down. The mechanism is the head
			 * joint parting; the outcome is no brick still held. The intermediate bed joints part
			 * too, so they are not asserted individually — which one gives on pass 2 is a detail.
			 */
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

			/*
			 * The outcome, on the support state and never on movement. Once the head joint parts
			 * the lowest brick loses its only support and the chain loses its path to the pad, so
			 * every hanging brick reads Falling.
			 */
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
			/*
			 * The standing row (sixteen bricks). The margin is 1.0524, so the LP finds an admissible
			 * force system and nothing breaks: zero passes, the head joint intact and unstamped,
			 * every brick held. The control that stops an implementation breaking whatever it sees.
			 */
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

			/*
			 * And every bed joint holds. Each brick sits squarely on the one below, so its lever
			 * arm is zero and its bed joint is at a few ten-thousandths of capacity; a cascade
			 * taking them too would be breaking on something other than the eccentricity.
			 */
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
		 * And the readout agrees with the verdict — the coincidence pinned as the point. Display
		 * reading and break decision cross 1.0 together: seventeen bricks read above and give,
		 * sixteen read below and hold. Dropping the moment from the readout, or using a plastic
		 * no-tension capacity for the break, would split them.
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
 * Load arriving through a joint must remember where it came from — today it forgets, so a
 * corbel cannot feel the wall hanging off the corbel above it.
 *
 * What is wrong today: the accumulation carries only a force down the load path.
 * ReceivedFromAboveUU is a scalar, so a brick's share arrives at the piece below with no lever
 * arm and is treated as acting at that piece's own centre. A brick carrying an eccentric brick
 * levers its joint as though the load above sat neatly on its middle. What is missing is a
 * moment travelling with the force.
 *
 * The accumulated quantity is a vector referenced to each joint's own centroid. ConnectionMoments
 * already stores each joint's moment about its own centroid, so passing one down means
 * re-referencing at every step by the moment-transfer relation:
 *
 *     M_about(c_to) = M_about(c_from) + (c_from - c_to) x F_transmitted
 *
 * which is Varignon. So a joint's moment is its own weight about its own centroid plus, for each
 * joint above, that joint's moment carried down and re-referenced with the share through it.
 *
 * The other two candidate reference points are both wrong, named so nobody relitigates. The
 * receiving piece's centre of mass is what the code effectively uses today — it makes the answer
 * depend on where the brick sits, and it is exactly the term that vanishes when a chain stacks
 * squarely, which is why the defect is invisible there. The world origin is valid but awful:
 * every stored value would be the whole wall's moment about a point kilometres away, recovered
 * as a difference of huge numbers. This is also the forward-compatibility slice: an applied
 * force at a point (r x F from an impact) is one more term in the same sum.
 *
 * The oracle is derived the other way round, which is its whole value.
 * ChainMomentAboutJointUuCm sums every brick above a joint times its own distance — no recursion,
 * no transfer term. The accumulation walks down; the oracle never walks.
 *
 *     SQUARE      CORBELLED        ZIG-ZAG
 *      [C]            [C]            [C]
 *      [B]          [B]            [B]
 *      [A]          [A]            [A]
 * [pad][ ]     [pad][ ]       [pad][ ]
 *
 * A squarely stacked chain is the control, precisely because the answer does not move: each
 * brick sits above the bed patch below, so the received load already acts on the line the old
 * rule assumed. MOMENTS_DESIGN.md's headline 1.626 is a chain laid horizontally, brick beside
 * brick — a cycle the solver strands — and does not describe this vertical stack, whose
 * two-brick case stays at 0.1188 before and after.
 *
 * The fixture that moves is a corbelled chain: step each brick half its length out and the load
 * path leaves by a patch nowhere near the one it arrived on. Two bricks take the head joint from
 * 0.1188 to 0.1755 — a 48% jump from identical bricks, purely because the moment now travels. At
 * mean f_x1 = 0.70 nothing here breaks, so the break arm lives in
 * MomentBreaksTheJointItOverloads; every row here asserts the reading and the moment vector.
 *
 * The zig-zag row pins the sign. A chain that steps out then back puts the upper weight on the
 * far side of the joint from the lower, so the two moments cancel — the middle joint carries two
 * bricks and zero moment. Summing magnitudes, or getting the transfer sign backwards, cannot
 * produce that.
 *
 * Which axis governs is asserted for the head joint: ComputeUtilisation returns the worst of
 * three, and on a head joint the mean normal stress is zero, so the whole force is shear (0.0089
 * for two bricks against 0.1755 in tension). The claim is against the worst axis.
 *
 * Asserted on the mechanism — the moment vector, the force, the utilisation — never on movement.
 * No world, no tick.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureMomentAccumulatesTest,
	"DestructionGame.Core.Structure.MomentAccumulatesAlongTheLoadPath",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureMomentAccumulatesTest::RunTest(const FString& Parameters)
{
	using namespace StructureMomentTestSupport;

	/*
	 * The expectations are ratios of published strengths, so they hold only while the profile
	 * carries the figures they were derived against.
	 */
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

	/** One corbel step: half a brick, which is the smallest step a running bond can take. */
	constexpr double CorbelStepCm = HalfLengthCm;

	/*
	 * A head joint separates on X, so its in-plane frame is Y and Z; the moment is about Y,
	 * resisted by the joint's 3.25 cm of depth rather than its 5.125 cm of width. The two moduli
	 * differ by 1.58x, so the pair being the right way round is load-bearing here.
	 */
	constexpr double HeadJointModulusCm3 = SectionModulusCm3(HalfWidthCm, HalfHeightCm);
	constexpr double HeadJointAreaSqCm = BrickWidthCm * BrickHeightCm;

	/**
	 * One chain, and both the number it must read and the number it reads without accumulation,
	 * so the failure message says how far the head joint has to move and which way.
	 */
	struct FChainCase
	{
		const TCHAR* Description;

		/** Where each brick's centre sits along the wall; the first must be +/- 22.5. */
		TArray<double> BrickCentreXCm;

		/** What the head joint reads before any moment is carried down the load path. */
		double UtilisationWithoutAccumulation;

		/** And what it must read once one is. */
		double ExpectedHeadUtilisation;

		/** Whether the head joint has to come apart under the chain. */
		bool bHeadMustGive;
	};

	const TArray<FChainCase> Cases = {
		/*
		 * The controls, first. A squarely stacked chain hands its load down the line already
		 * assumed, so nothing may move; without these rows every claim below is satisfied by an
		 * implementation that invents a lever arm for anything it sees.
		 */
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

		/*
		 * The claim. The upper brick steps half its length out, so the load leaves it 5.375 cm
		 * from its own weight and arrives on the head joint 16.625 cm out rather than 11.25:
		 * 33.25 brick-weight-cm where the old rule saw 22.5.
		 *
		 * No row here gives any more (mean re-anchor 2026-08-13): the worst chain reads 0.348, so
		 * every bHeadMustGive is false and the test asserts the reading and moment vector. The
		 * break arm is now MomentBreaksTheJointItOverloads' seventeen-brick chain.
		 */
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

		/*
		 * The sign. Step out then straight back, and the top brick's weight lands as far left of
		 * the middle bed joint as the middle brick's does right. They cancel exactly, so that
		 * joint carries two bricks with no moment.
		 */
		{
			TEXT("three bricks zig-zagging back over the joint below"),
			{ BrickPitchCm, BrickPitchCm + CorbelStepCm, BrickPitchCm },
			0.17816884615384615,
			0.23491892307692308,
			false
		},

		/*
		 * The same chain mirrored. Hanging left of the pad flips the normal, every lever arm and
		 * every moment sign; the magnitude reaching the stress may not notice. An implementation
		 * that dropped a sign in the transfer term reads differently here than the row above.
		 */
		{
			TEXT("the corbelled pair mirrored, hanging to the left"),
			{ -BrickPitchCm, -(BrickPitchCm + CorbelStepCm) },
			0.11877923076923077,
			0.17552930769230769,
			false
		},
	};

	constexpr double Tolerance = 1.0e-9;

	/*
	 * Absolute, not relative, because one row comes out at exactly zero and a relative bound
	 * around zero admits nothing. Moments run to 1.8e5 uu.cm where rounding is ~2e-11, so a
	 * millionth of a uu.cm is ample slack over rounding and far below anything the test cares about.
	 */
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

		/*
		 * The load path is what makes every piece here determinate, so it is asserted. The bottom
		 * brick must hang off its head joint (a bed joint would win the tier and leave no
		 * fallback), and every brick above must rest on the one below.
		 */
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

		// --- what every joint of the chain carries, against the statics oracle ------------

		for (int32 Joint = 0; Joint < Chain.Joints.Num(); ++Joint)
		{
			const int32 Index = Chain.Joints[Joint];
			const FConnection& Connection = Structure.GetConnection(Index);

			const double JointCentreXCm = ChainJointCentreXCm(Chain, Joint);

			/*
			 * The arbitration. Every lever arm below is measured from this centroid, so if the
			 * producer moved it the expectations would silently stop describing this joint.
			 * Pinned so the two fail together.
			 */
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
			 * The mechanism itself. Magnitude, not signed value: the stored sign flips with
			 * which end was named first (pinned in Structure.HangingBrickPeelsRatherThanShears)
			 * and only the magnitude reaches a stress. The axis is not free, though — gravity
			 * crossed with a lever arm along the wall is a moment about world Y and nothing else;
			 * an X or Z component would be a twist this rectangle has no modulus for.
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

			/*
			 * And what that does to the joint, worked from the rectangle rather than read off it.
			 * A bed joint's shared span shortens by how far the brick above is stepped out, and
			 * the bending is about Y, resisted by the depth along the wall.
			 */
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

			/*
			 * Signed, positive in tension. Gravity runs parallel to a head joint, so its mean
			 * normal stress is zero and the whole force is shear; it presses squarely on a bed
			 * joint, so there the whole force is compression and the shear is zero.
			 */
			const double NormalStressMPa =
				bHeadJoint ? 0.0 : -ExpectedForceUu / (AreaSqCm * ForceUnitsPerMPaPerSqCm);
			const double ShearStressMPa =
				bHeadJoint ? ExpectedForceUu / (AreaSqCm * ForceUnitsPerMPaPerSqCm) : 0.0;

			/*
			 * And the deep beam standing over a bed joint, which is why joint 1 of the three-brick
			 * corbel moved from 0.24206 to 0.14919 at slice 5. A stack of courses does not resist
			 * the overturning moment as one bed patch: the section taking it is a vertical one
			 * through the bonded masonry above, t*D^2/6, and the joint gives at whichever opens it
			 * less. ARCHING_DESIGN.md slice 5.
			 *
			 * The chain is where that is easiest to count: joint k is under brick k, so D is that
			 * many course pitches. A head joint has no masonry standing on it (which is why case
			 * (b) and the squarely-stacked chain did not move), and the top brick has nothing on
			 * it, so it keeps its own patch.
			 *
			 * Both edges move together: where the deep beam takes the moment the bed patch is not
			 * bending, so what is left is the bed plane under uniform N/A and the vertical plane
			 * under +-M/W_c, and the worst fibre is the larger of the two, not their sum.
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

			/*
			 * The head joint is where the headline claim lives, so its governing axis is asserted,
			 * not just computed. The bed joints may be governed by whichever axis the arithmetic
			 * says (the zig-zag row drives one to zero tension), so their three are printed and
			 * the worst is claimed.
			 */
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

		// --- and the break decision has to follow it -------------------------------------

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

		/*
		 * The bed joints are not collateral. The worst of them here is a quarter of capacity, so a
		 * cascade taking one with it would be breaking on something other than the eccentricity.
		 */
		for (int32 Joint = 1; Joint < Chain.Joints.Num(); ++Joint)
		{
			TestFalse(
				FString::Printf(TEXT("%s: bed joint %d is well under capacity and must hold"),
					Case.Description, Joint),
				Structure.GetConnection(Chain.Joints[Joint]).HasGiven());
		}
	}

	/*
	 * ================================================================================
	 * And the same claim over every chain a half-brick grid can make.
	 * ================================================================================
	 *
	 * Hand-written rows only cover shapes somebody thought of, and the ones above all step the
	 * same way or step back once. That leaves the combinatorics untested: three steps that
	 * partly cancel, a chain that reverses twice, a step landing a joint's centroid on the brick
	 * above's centre of mass — where a wrong sign or reference point gives a plausible number.
	 *
	 * Exhaustive rather than seeded: every combination of steps on a quarter-brick grid four
	 * bricks deep is 75 chains and runs in no measurable time. Each case prints its brick
	 * centres, so a failure is a fixture ready to paste into the table above.
	 *
	 * The first brick may only step away from the pad: stepping toward it would put a course-1
	 * brick over the pad with a mortar gap — a face MakeInterface would call a joint, wrong for a
	 * reason unrelated to moments. Higher bricks are nowhere near the pad and are free either way.
	 */
	constexpr double SweepOffsetsCm[] = {
		-CorbelStepCm, -CorbelStepCm / 2.0, 0.0, CorbelStepCm / 2.0, CorbelStepCm
	};

	constexpr int32 SweepOffsetCount = UE_ARRAY_COUNT(SweepOffsetsCm);

	int32 SweepChains = 0;
	int32 SweepMismatches = 0;

	for (int32 First = 0; First < SweepOffsetCount; ++First)
	{
		// Away from the pad only; the offsets are ordered, so this is the back half of them.
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

					/*
					 * Never NaN, always finite. A NaN moment would sail through the magnitude
					 * comparison below — every comparison against NaN is false — so the check is
					 * written to catch it rather than be satisfied by it.
					 */
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

					// So a sign error everywhere does not print three hundred failures.
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
