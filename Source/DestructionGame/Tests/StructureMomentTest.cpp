// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"
#include "Core/StructureBinding.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, not anonymous, and named for what it holds. An anonymous namespace is
 * private to a TRANSLATION UNIT rather than to a file, and a unity build merges many files
 * into one — at which point two anonymous namespaces in the blob are the SAME namespace and
 * identically-named helpers in files that never refer to each other are a hard compile
 * error. See CURRENT_STATE.md; this is the third time it has bitten.
 */
namespace StructureMomentTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * --- everything below is spelled out here rather than imported --------------------
	 *
	 * The point of a test is to disagree with production when production is wrong, and a
	 * test that reaches for the same named constant agrees with it instead. So the brick,
	 * gravity, the SI conversion and the section modulus are all written from first
	 * principles, and the strengths are asserted against the profile rather than copied
	 * out of it.
	 */

	/** DESIGN.md's standard UK metric clay brick, cm. */
	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickWidthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;

	/** The standard 1 cm mortar joint, which is what makes the coordinating grid 22.5 cm. */
	constexpr double MortarJointCm = 1.0;

	/**
	 * 1.9 g/cm3, DENSITY FIRST in the product to match Layout::PieceMassKg exactly.
	 *
	 * Volume-first lands one ulp low at 2.7216312499999997, and the exact-equality guard
	 * below compares against a mass the producer derived, so the association matters.
	 */
	constexpr double BrickMassKg = 1.9 * BrickLengthCm * BrickWidthCm * BrickHeightCm / 1000.0;

	/** 980 cm/s2. In a world where 1 uu = 1 cm and mass is kg, MassKg * 980 IS a force in uu. */
	constexpr double BrickWeightUu = BrickMassKg * 980.0;

	/**
	 * Force, in Unreal units, that loads one square centimetre to one megapascal.
	 *
	 * 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa (= 1 N/mm2) over 1 cm2 is 10000 uu.
	 * DELIBERATELY NOT DestructionForce::ForceUnitsPerMPaSqCm: this test has to fail if
	 * that constant is wrong, not agree with it.
	 */
	constexpr double ForceUnitsPerMPaPerSqCm = 100.0 * 100.0;

	/**
	 * Elastic section modulus of a rectangle, cm3 — ordinary beam theory, not a code figure.
	 *
	 * For a face 2*HalfAlong wide and 2*HalfAcross deep, bent so the stress varies along the
	 * second axis: I = (2*HalfAlong)*(2*HalfAcross)^3/12 and the outermost fibre sits at
	 * HalfAcross, so W = I/c = (4/3)*HalfAlong*HalfAcross^2.
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
	 * A CHAIN OF BRICKS HANGING OFF ONE HEAD JOINT, EACH FREE TO SIT ANYWHERE ALONG THE WALL.
	 *
	 * A grounded pad at the origin, then one brick per course going up, each jointed ONLY to
	 * the one below it. Every brick therefore rests on exactly one joint — N = 1 all the way
	 * down, which is the determinate case, so the whole chain carries a moment and nothing in
	 * it is subject to the indeterminate rule.
	 *
	 * THE CHAIN IS VERTICAL RATHER THAN HORIZONTAL, and that is forced rather than chosen. A
	 * row of bricks side by side has every piece falling back on ALL of its head joints, so
	 * the middle of the row supports its neighbour and is supported by it — a cycle the solver
	 * correctly strands rather than solving.
	 *
	 * Piece handles and box indices are the same number: 0 is the pad and brick k is handle
	 * k + 1. Joint k is the joint UNDER brick k, so joint 0 is the head joint.
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
	 * Build one, from nothing but where each brick's centre sits along the wall.
	 *
	 * The first brick has to be one coordinating cell from the pad — +/- 22.5 — or there is
	 * no head joint and MakeInterface refuses; every brick above may sit anywhere that still
	 * overlaps the one beneath it. Courses are one brick plus one joint apart, so consecutive
	 * bricks are separated on Z by exactly the mortar thickness and their shared face is a
	 * bed joint.
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
	 * Where joint k sits along the wall, DERIVED RATHER THAN READ OFF THE JOINT.
	 *
	 * Both boxes are a brick long, so the shared span runs from the further left face to the
	 * nearer right face and its midpoint is simply the midpoint of the two centres — which is
	 * also, on the separation axis of the head joint, the mid-plane of the mortar. One formula
	 * covers both because it is interval arithmetic on equal-length boxes either way.
	 *
	 * The test asserts the emitted centroid against this rather than assuming it: the lever
	 * arms below are all measured from here, so the two have to fail together.
	 */
	double ChainJointCentreXCm(const FHangingChain& Chain, int32 Joint)
	{
		return (Chain.Boxes[Joint].CentreCm.X + Chain.Boxes[Joint + 1].CentreCm.X) * 0.5;
	}

	/**
	 * THE ORACLE: the moment everything above joint k exerts about joint k's centroid, uu.cm.
	 *
	 * ONE SUM OVER THE BRICKS THE JOINT CARRIES, each weight times its own distance from the
	 * joint — first-principles statics, with no recursion, no re-referencing, no load path and
	 * no notion of a joint passing anything to another joint. That is the entire point of it:
	 * an oracle that walked the chain the way the accumulation does would agree with a wrong
	 * accumulation, and the value here is that the two are derived differently and have to
	 * meet in the middle.
	 *
	 * SIGNED, and the sign is the distance's. Only the magnitude reaches a stress — and the
	 * STORED sign additionally depends on which end of the joint was declared first — so the
	 * assertions compare magnitudes. What the sign does here is make cancellation real: a
	 * chain that zig-zags back over its own support has two contributions that cancel to
	 * exactly zero, and an accumulation that summed magnitudes could not produce that.
	 *
	 * Only X matters. Gravity is vertical, so a brick's height above a joint crosses into
	 * nothing at all and every moment in this fixture is about world Y.
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
 * A BRICK HELD BY ONE HEAD JOINT PEELS OFF IT RATHER THAN SHEARING THROUGH IT — the whole
 * point of the moment work, and the first slice where it is visible in a structure rather
 * than in a hand-built load.
 *
 * WHAT IS WRONG TODAY. Gravity runs parallel to a head joint, so the mean normal stress
 * across it is EXACTLY zero and the only axis carrying anything is shear against mortar's
 * cohesion. The joint reads 0.0044481 — over two hundred bricks from failure — while what
 * is physically happening is that the brick's weight acts 11.25 cm to one side of the
 * joint and levers the top of it open, against a tensile strength a fourteenth of the
 * compressive one. The averaged stress is the wrong number the moment the load path misses
 * the centroid, and it is wrong in the direction that leaves things standing.
 *
 * THE ECCENTRICITY IS 11.25 cm AND NOT 10.75, AND THAT IS ARBITRATED AGAINST THE EMITTED
 * CENTROID RATHER THAN AGAINST THE DESIGN NOTE. MOMENTS_DESIGN.md worked this case with
 * e = 10.75 — half a brick length, measured to the brick's own FACE — and got 0.39725.
 * Layout::MakeInterface emits the centroid at the MID-PLANE OF THE MORTAR, which is a
 * taken decision and a good one: a 1 cm bed has two contact planes, so "the contact plane"
 * is ambiguous for every mortared joint in a wall, and a face-based centroid would make
 * every lever arm depend on which brick the producer happened to name first. From a head
 * joint centred at X = 11.25 the brick's centre of mass at X = 22.5 is 11.25 cm away, so
 * the answer is 0.0594 (0.4157 on the retired characteristic basis), and the joint
 * geometry is asserted below so that the number and the reason for it fail together.
 *
 * THE ARITHMETIC, spelled from published figures and brick dimensions (mean basis since
 * the 2026-08-13 re-anchor — the stress side is pure statics and does not move; only the
 * divisor changed, 0.10 -> 0.70, a straight /7 on this and every chain multiple):
 *
 *     W       = 1.9 * 21.5 * 10.25 * 6.5 / 1000 * 980  = 2667.198625 uu
 *     M       = 2667.198625 * 11.25                    = 30005.98453125 uu.cm
 *     W_sec   = (4/3) * 5.125 * 3.25^2                 = 72.177083... cm3
 *     sigma_b = M / W_sec                              = 415.7273077 uu/cm2
 *                                                      = 0.04157273077 MPa
 *     tension = sigma_b / mean f_x1 (0.70 MPa)         = 0.0593896154
 *
 * NO NEW CONVERSION BOUNDARY, and it is worth checking rather than assuming because
 * "moments" sounds like it should introduce one. Length is cm, so M is uu.cm and M/W with
 * W in cm3 is uu/cm2 — the identical quantity a force over an area already is, divided by
 * the identical 10000. That 10000 is spelled out above from 1 N = 100 uu, so a factor of
 * 100 in the wrong place surfaces here instead of being absorbed.
 *
 * WHICH AXIS GOVERNS IS THE ENTIRE RISK, because ComputeUtilisation returns the WORST of
 * the three and a test aimed at bending would silently measure shear if shear happened to
 * be higher. Every row carries its own worked comparison as a fixture precondition, and
 * every row is checked to be measuring what it claims before it is allowed to claim it.
 *
 * N = 1 ONLY, AND THAT IS THE WHOLE CLAIM. A piece resting on several supports is
 * statically INDETERMINATE — the reactions rearrange to satisfy moment equilibrium — and
 * splitting the moment per joint is not merely approximate but wrong in a way that would
 * peel every bed joint in a standing wall. On exactly one support the problem is
 * determinate and the moment is exact. Structure.SymmetricSupportsCarryNoMoment is the
 * other half of this statement and matters more than this one does.
 *
 * NO WORLD, NO TICK, NO GRAVITY SETTING. FStructure is plain arithmetic over a graph and
 * Layout is plain arithmetic over boxes; the 980 above is the solver's own constant, not
 * a physics scene's. The assertion is on the MECHANISM — a utilisation ratio — exactly as
 * DESIGN.md §4 asks of a unit test.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureHangingBrickPeelsTest,
	"DestructionGame.Core.Structure.HangingBrickPeelsRatherThanShears",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureHangingBrickPeelsTest::RunTest(const FString& Parameters)
{
	using namespace StructureMomentTestSupport;

	/*
	 * The expectations are ratios of published strengths, so they only mean what they say
	 * while the profile still carries the figures they were derived against. A retune has
	 * to fail loudly here rather than quietly moving every number below. The MEAN flexural
	 * bond f_x1 = 0.70 MPa (re-anchor 2026-08-13: Gooch et al. 2023, ConBuildMat
	 * 386:131578 — twelve M4/M6 batch means averaging 0.571, and UK NA Table NA.6's 0.4
	 * x the campaign's mean/characteristic ratio 1.89 = 0.76; 0.70 is the centre) is the
	 * one the answer is divided by; the other two decide which axis governs, and the
	 * friction coefficient decides whether shear gets any help (it does not — the mean
	 * compressive stress on a head joint under gravity is zero).
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
	 * One grounded pad, one brick hanging off it, and one head joint between them.
	 *
	 * The pad is a brick-shaped block resting on the earth; it terminates the flow of
	 * load, so nothing about its own mass or its own centre reaches the answer. The
	 * hanging brick has no bed joint beneath it at all, which is what makes its single
	 * head joint its support — the fallback DESIGN.md §3 describes, and N = 1 by
	 * definition.
	 */
	struct FPeelCase
	{
		const TCHAR* Description;

		FPieceBox PadBox;
		FPieceBox HangingBox;

		/**
		 * Whether the PAD is the handle MakeInterface is given first, and therefore
		 * whether the joint's normal points at the hanging brick or away from it.
		 *
		 * ConnectionLoad.h's convention is that the force belonging to a joint is the
		 * force acting on PieceB, so a joint naming the loaded piece FIRST stores the
		 * equal-and-opposite reaction, pointing UP. A lever arm crossed with the stored
		 * vector rather than with the load path would then come out with the same
		 * magnitude, which is the point of having a row for it: the answer must not
		 * depend on declaration order at all, exactly as the classification does not.
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
		 * THE ROW THAT CARRIES THE WHOLE POINT. Pad at the origin, brick one cell along,
		 * so the head joint's mid-plane sits at X = 11.25 and the brick's mass acts at
		 * X = 22.5 — 11.25 cm of lever arm, and the joint has 3.25 cm of depth to resist
		 * it with. Not 10.75: see the note on the mid-plane above.
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
		 * THE SAME JOINT SEEN FROM THE OTHER SIDE. The brick hangs to the LEFT of the pad,
		 * so MakeInterface emits a normal of -X instead of +X and the lever arm points the
		 * other way. The sign of a moment says which edge of the joint opens, never how
		 * hard, so this must answer identically — an implementation that let the sign of
		 * the offset reach the stress would read zero or double here.
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
		 * THE LOADED PIECE DECLARED FIRST, which is the orientation that stores the force
		 * pointing UP rather than down. Same wall, same brick, same joint; only the order
		 * the producer named the two handles in has changed, and nothing physical may.
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
		 * A JOINT THAT IS BOTH BENT AND TWISTED, AND ONLY THE BENDING MAY COUNT.
		 *
		 * The pad is displaced across the wall AND along it, so the two boxes are
		 * separated on Y and overlap only half of each other on X. The shared face is
		 * therefore 10.75 x 6.5, centred at X = 5.375 — while the brick's mass acts at
		 * X = 10.75. That leaves TWO offsets: 5.625 cm along the joint's own normal, which
		 * levers it open, and 5.375 cm across the face, which twists it about that normal.
		 *
		 * (p - c) x F produces both, as components about world X and world Y respectively.
		 * The in-plane frame of a Y-normal joint is the two world axes that are not Y, so
		 * the X component is bending and the Y component is TORSION — second order for
		 * gravity on a rectangular joint, needing a modulus this face does not carry, and
		 * explicitly out of scope. It must be dropped rather than folded into either
		 * in-plane term, and a row that fed it in would read 0.38 rather than 0.198.
		 *
		 * This row also exercises a different separation axis and a different section
		 * modulus, which nothing else here does: an implementation that assumed the normal
		 * was X, or that paired a moment with the wrong one of the two in-plane moduli,
		 * agrees with every row above and fails this one.
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
		 * REAL GEOMETRY THROUGHOUT: the joint comes out of the producer rather than being
		 * hand-assembled, because the emitted centroid is the thing the expected number is
		 * arbitrated against. A rectangle written by hand here could agree with an
		 * expectation written by hand beside it and both be wrong together.
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
		 * THE ARBITRATION, AND IT IS AN ASSERTION RATHER THAN AN ASSUMPTION. The lever arm
		 * is measured from this centroid, so if the producer ever moved it to a brick face
		 * the expected utilisation below would silently stop describing this joint. Pinned
		 * here so the two fail together and the message says which moved.
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
		 * --- what this joint must read, and why it is the tension axis that says so ----
		 *
		 * Three utilisations, each stress against its own capacity. The bending term
		 * reaches the tension edge and the compression edge equally — the joint pivots
		 * about its centroid, so one side opens by exactly as much as the other closes —
		 * and the mean normal stress is zero because gravity runs parallel to the face,
		 * so peak tension and peak compression are both simply sigma_b. What separates
		 * them is that mortar resists crushing a hundred times better than it resists
		 * being pulled open.
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
		 * AND IT MUST STILL STAND. A single brick on a single head joint really does hang
		 * there; what changes is that it is now a seventeenth of the way to coming off
		 * rather than a two-hundredth, so a chain of seventeen parts. A row that expected
		 * something over 1.0 would be asserting a collapse this fixture does not have.
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
		 * THE LOAD PATH IS WHAT MAKES THIS N = 1, so it is asserted rather than assumed.
		 * The joint has to be the brick's HEAD joint (a bed joint would win the tier
		 * outright and there would be no fallback), the brick has to be held up by it,
		 * and the whole of the brick's weight has to be flowing through it — a share of
		 * it would mean the fixture had grown a second support somewhere.
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
 * A BRICK SITTING SQUARELY ON TWO SUPPORTS LOADS THEM EXACTLY AS IT DID BEFORE ANY OF THIS
 * EXISTED — bit for bit, not nearly.
 *
 * THIS MATTERS MORE THAN THE HEADLINE DOES, and it is the trap MOMENTS_DESIGN.md exists to
 * record. The obvious rule — every supporting joint carries (p - c_j) x S_j, its own share
 * crossed with its own lever arm — is WRONG, and wrong in the direction nobody would find
 * until a wall was on screen. On an ordinary running-bond brick with two symmetric bed
 * patches it gives each joint sigma_b = 4.18e-3 against sigma_n = 1.269e-3, so every bed
 * joint in a standing wall would read about 0.029 IN TENSION. The moments cancel across the
 * pair and not on either one of them.
 *
 * The correct statics is that a piece on several supports is statically INDETERMINATE — the
 * reactions rearrange until moment equilibrium is satisfied — and determinate on exactly
 * one. So N = 1 is exact, N >= 2 keeps the area split with the moment at zero, and that is
 * unconservative only when the centre of mass is NOT at the area-weighted centroid of the
 * supports. In a symmetric running bond it is, exactly, which is why the intact wall does
 * not move at all.
 *
 * THE FIXTURE IS THE PRODUCER'S OWN WALL, AND ITS TWIN. RunningBond lays a small flush
 * wall; the twin is rebuilt from that structure's own pieces and joints with ONE difference
 * — its pieces are added through the two-argument door, so they carry no centre of mass.
 * Every joint keeps its rectangle, every mass and every grounded flag is copied across, so
 * the only variable in the whole comparison is whether anybody said where the weight acts.
 *
 * FLUSH RATHER THAN RAGGED, deliberately: a flush wall finishes with half bats at
 * alternating course ends, and a half bat lands on exactly ONE brick below it. So this wall
 * contains N = 1 load paths as well as N = 2 ones — and their eccentricity is zero too,
 * because a half bat's bed patch is its own footprint. Getting N = 1 right must not mean
 * moving a wall that is standing perfectly well.
 *
 * EXACT EQUALITY IS THE ASSERTION. A 1e-9 tolerance would pass for a rearrangement that
 * moved every joint in the project by a few ulps, and the cascade fuzz has five joints
 * settling at utilisation exactly 1.0 and one at 1 - 1 ulp, so a single ulp of drift is
 * five spurious break-decision failures. "Does not move" has to mean does not move.
 *
 * RED ON ARRIVAL FOR ONE REASON ONLY: its fixture precondition. The comparison itself
 * cannot measure anything until Layout supplies the centres of mass — with none supplied
 * the twin is not a twin, it is the same structure twice — so HasCompleteGeometry is
 * asserted first and the rest of the test is worthless without it. Once that is green this
 * becomes a regression net rather than a driver, and it says so.
 *
 * NO WORLD, NO TICK.
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
	 * THE PRECONDITION THE WHOLE TEST HANGS ON. Until the producer places its pieces, both
	 * halves of the comparison below are geometry-free and agreeing with each other proves
	 * nothing whatsoever. This is the one assertion here that is a red step.
	 */
	TestTrue(
		TEXT("FIXTURE PRECONDITION: a wall laid by the producer must know where every piece ")
		TEXT("and every joint is, or the comparison below is a structure against itself"),
		Laid.Structure.HasCompleteGeometry());

	/*
	 * The twin. Same masses, same grounded flags, same joints — rectangles and all — and
	 * no centres of mass, which is exactly the state every fixture in this suite is in.
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
	 * A comparison over joints that all carry nothing would pass trivially, so the wall is
	 * checked to be doing something first. Masonry is enormously overbuilt in compression,
	 * so "something" here is thousandths of capacity rather than anything dramatic.
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
		 * THE FORCE MUST NOT MOVE EITHER, and it is a separate claim. Moments ride
		 * ALONGSIDE the routed force rather than changing it — the load split is still
		 * area-weighted and gravity still points straight down — so a change here would
		 * mean the routing itself had been disturbed, which is a different and worse
		 * failure than a joint reading a moment it should not.
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
 * "NOBODY SUPPLIED POSITIONS" AND "THE LOAD HAPPENS TO BE CENTRED" ARE DIFFERENT STATES,
 * AND EXACTLY ONE THING CAN TELL THEM APART.
 *
 * They produce the identical answer everywhere else, on purpose: sigma = N/A +/- M/W
 * collapses to N/A EXACTLY when the moment is zero, which is the only reason every
 * geometry-free fixture and both fuzz generators still work. What that costs is the
 * ability to ask which one you are looking at, and this is where it is bought back — the
 * same trade HasSupportAnswer makes against EPieceSupport::Falling one level down.
 *
 * A CONJUNCTION, NOT A JUDGEMENT CALL. A moment needs a point for the load to act at and a
 * rectangle for the joint to resist it with, so both halves have to be present or some
 * joint somewhere is answering a centred load because it has no choice. Slice 2 deferred
 * this deliberately: with rectangles landed and centres of mass not, it could not have been
 * written honestly, because half of a conjunction is not a weaker version of it.
 *
 * OVER WHAT IS STILL IN THE STRUCTURE. A removed piece and a joint that has given are out
 * of the graph — they carry nothing and route nothing — so a tombstone must not condemn a
 * live structure that is fully described. That is the same scoping every other accessor on
 * FStructure already uses, not a special case invented here.
 *
 * THE ONE DECISION WORTH FLAGGING: an empty structure reads TRUE, because an empty
 * conjunction is true and that is what keeps the predicate composable — adding a fully
 * described piece to a complete structure leaves it complete, with no first-piece special
 * case. The opposite reading (a default-constructed structure has supplied nothing, so it
 * should read false) is defensible and cheap to swap; what is NOT defensible is a structure
 * with pieces and no positions reading true, and that is the row below that matters.
 *
 * NO WORLD, NO TICK.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureCompleteGeometryTest,
	"DestructionGame.Core.Structure.GeometryIsCompleteOrItIsNot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureCompleteGeometryTest::RunTest(const FString& Parameters)
{
	using namespace StructureMomentTestSupport;

	/**
	 * FREE FUNCTION POINTERS RATHER THAN TFunction, matching PieceActions.h: these rows
	 * capture nothing, so there is no allocation and no indirection worth the name.
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
		 * THE ROW THAT MATTERS. This is every fixture in the suite and both fuzz
		 * generators: a perfectly valid structure that simply has no positions in it. It
		 * must be distinguishable from one whose loads happen to be centred, or the
		 * distinction this accessor exists for does not exist.
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
		 * HALF OF A CONJUNCTION IS NOT A WEAKER VERSION OF IT. The pieces know where they
		 * are and the joint does not, so its lever arm is unmeasurable and every load
		 * through it is answered as though it were centred. That is the state slice 2 left
		 * behind when it landed rectangles without centres of mass, seen from the other
		 * side.
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
		 * AND WHAT HAS LEFT THE STRUCTURE CANNOT SPOIL IT. Piece 2 was never placed and its
		 * joint never carried a rectangle; removing it takes both out of the graph at once
		 * — the piece is tombstoned and the joint is severed — leaving a live half that is
		 * completely described. A predicate that walked the raw arrays would say false here
		 * for ever, which would make it useless the first time a player pulled a brick.
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
 * A WALL THAT WENT THROUGH THE BINDING IS STILL A PLACED WALL — and today it is not, so
 * every wall the game itself builds computes no moments at all.
 *
 * WHERE THE GEOMETRY IS DROPPED. RunningBond places every piece it lays: it derives the
 * mass and the centre of mass from one box, so a piece cannot weigh one brick and act
 * through a different one. AdoptLayout then replays that layout into an FStructureBinding
 * — the only route there is from a laid wall to the thing the world subsystem owns — and
 * FStructureBinding::AddPiece takes the box, keeps it for itself, and forwards MASS AND
 * GROUNDEDNESS ONLY to the solver. The centre falls on the floor between the two.
 *
 * WHICH IS INVISIBLE, AND THAT IS THE WHOLE PROBLEM. An unplaced piece loads its joints
 * EXACTLY as a centred one does — that exactness is deliberate and is what lets every
 * geometry-free fixture in the project keep working — so a binding that lost every centre
 * of mass reports a wall that is standing perfectly happily, with the same forces, the same
 * support answers and the same piece count as one that never lost anything. Nothing throws,
 * nothing looks wrong, and slice 3 is simply absent from play.
 *
 * TWO CLAIMS, AND THE FIRST IS THE SHARPER ONE.
 *
 * HasCompleteGeometry exists precisely so that "nobody supplied positions" is askable rather
 * than silently identical to "the load happens to be centred". Asserting it on a
 * binding-built structure fails for the whole CLASS of droppage — any piece, any field, any
 * future producer — rather than for one number on one joint, and it keeps meaning the same
 * thing when the fixture below is eventually retired.
 *
 * The second claim is that number, because a predicate can be satisfied without anything
 * downstream using what it promises. So one joint of one real wall is worked through end to
 * end, and it is deliberately a joint whose answer MOVES.
 *
 * THE FIXTURE IS THE NARROW-WAIST WALL, which is BrickWorldTestSupport::NarrowWaistWallSpec(3)
 * spelled out here rather than included: that header drags in UWorld, the player controller
 * and the input subsystem, and this test needs none of them. The shape is asserted below so
 * that the two cannot drift apart in silence.
 *
 *      course 2         [ 3 ][ 4 ]
 *      course 1            [ 2 ]        THE WAIST
 *      course 0         [ 0 ][ 1 ]      grounded
 *
 * Ragged, two full bricks per course, so the odd course is a single brick. Bricks 3 and 4
 * each land on the waist and on NOTHING ELSE — N = 1, the determinate case — and each one
 * overhangs it, which is exactly MOMENTS_DESIGN.md's case (c): a running-bond brick that has
 * only one of its two bed supports.
 *
 * THE ARITHMETIC, from the emitted rectangle and published strengths.
 *
 * The waist brick sits half a cell along at x = 11.25 and spans x 0.5 .. 22.0; brick 3 sits
 * at x = 0 and spans -10.75 .. 10.75. The shared patch is therefore x 0.5 .. 10.75 by the
 * full 10.25 of depth:
 *
 *     A       = 10.25 * 10.25                          = 105.0625 cm2
 *     centre  = ((0.5 + 10.75) / 2, 0, 14.5)             the mid-plane of the mortar
 *     e       = 5.625 - 0                              = 5.625 cm
 *     W_v     = (4/3) * 5.125 * 5.125^2                = 179.4817708 cm3
 *     W       = 1.9 * 21.5 * 10.25 * 6.5 / 1000 * 980  = 2667.198625 uu
 *     sigma_b = 2667.198625 * 5.625 / W_v              = 8.3590619e-3 MPa
 *     sigma_n = -2667.198625 / A                       = -2.5386781e-3 MPa, compression
 *     tension = max(0, sigma_n + sigma_b)              = 5.8203838e-3 MPa
 *             / mean f_x1 (0.70 MPa)                   = 0.0083148340
 *
 * against the 2.5386781e-4 a centred load reads — a 33x change, and the joint still holds,
 * which is right: one overhanging brick does not come off. (Mean basis since the
 * 2026-08-13 re-anchor; the characteristic-basis figure was 0.058203838 and the change
 * 229x. The stress side is untouched — only the 0.10 -> 0.70 divisor moved.)
 *
 * WHICH AXIS GOVERNS IS ASSERTED, NOT ASSUMED. ComputeUtilisation returns the WORST of the
 * three, so a test aimed at bending measures compression the moment compression happens to
 * be higher — and on this joint compression is what governs TODAY. It is the axis this test
 * has to overtake, so both are worked out and compared before either is claimed.
 *
 * THE LAID WALL IS THE CONTROL, and it is asserted first. The producer already places its
 * pieces, so the layout's own structure reads the eccentric figure; only the copy that went
 * through the binding does not. Checking both is what makes the failure say WHERE the
 * geometry was lost rather than merely that it is missing.
 *
 * NO WORLD, NO TICK, NO ACTORS. AdoptLayout stores whatever pointers it is handed into weak
 * pointers and never resolves one, and nothing here asks for an actor, so the stand-ins are
 * null. That is not a shortcut around a fixture: the piece-to-actor half of the binding is
 * StructureBinding.AdoptLayout's subject and is fully covered there.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureBindingAdoptedWallGeometryTest,
	"DestructionGame.Core.StructureBinding.AdoptedWallLoadsItsWaistEccentrically",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureBindingAdoptedWallGeometryTest::RunTest(const FString& Parameters)
{
	using namespace StructureMomentTestSupport;

	/*
	 * The expected numbers are ratios of published strengths, so they mean what they say
	 * only while the profile still carries the figures they were derived against.
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
	 * THE SHAPE, PINNED. Five pieces and six joints is the wall drawn above and nothing
	 * else; a spec that stopped producing a waist would make every claim below meaningless
	 * while still passing most of them.
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
	 * ONE PLACE THE WHOLE FIXTURE HANGS ON: the producer must already place its pieces. If
	 * it did not, the binding would be losing nothing and this test would be measuring two
	 * geometry-free structures against each other.
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

	/* Top of course 1, plus half the mortar bed above it — the mid-plane MakeInterface emits. */
	constexpr double BedPatchCentreZCm =
		BrickHeightCm / 2.0 + CoursePitchCm + BrickHeightCm / 2.0 + MortarJointCm / 2.0;

	constexpr double BedPatchAreaSqCm = (2.0 * BedPatchHalfXCm) * (2.0 * BedPatchHalfYCm);

	/* The brick's own centre is at x = 0; the patch it lands on is not. */
	constexpr double LeverArmCm = BedPatchCentreXCm;

	/*
	 * A bed joint separates on Z, so its in-plane frame is X and Y. The lever arm runs along
	 * X and gravity along Z, so (p - c) x F is a moment about Y — resisted by the depth the
	 * face has along X. THIS PATCH IS SQUARE, so the two moduli are equal and this fixture
	 * cannot tell a swapped pair apart; Structure.HangingBrickPeelsRatherThanShears is where
	 * that order is pinned, on a face whose two moduli differ by a factor of 2.5.
	 */
	const double BedPatchModulusCm3 = SectionModulusCm3(BedPatchHalfYCm, BedPatchHalfXCm);

	const double BendingStressMPa =
		(BrickWeightUu * LeverArmCm) / (BedPatchModulusCm3 * ForceUnitsPerMPaPerSqCm);

	/* Signed, positive in tension: a brick pressing down on a bed joint compresses it. */
	const double NormalStressMPa =
		-BrickWeightUu / (BedPatchAreaSqCm * ForceUnitsPerMPaPerSqCm);

	const double PeakTensileStressMPa = FMath::Max(0.0, NormalStressMPa + BendingStressMPa);
	const double PeakCompressiveStressMPa = FMath::Max(0.0, BendingStressMPa - NormalStressMPa);

	const double ExpectedUtilisation =
		PeakTensileStressMPa / GeneralPurposeMortar.TensileStrengthMPa;
	const double CompressionUtilisation =
		PeakCompressiveStressMPa / GeneralPurposeMortar.CompressiveStrengthMPa;

	/* What a joint reads when nobody said where the load acts: the averaged stress alone. */
	const double CentredUtilisation =
		-NormalStressMPa / GeneralPurposeMortar.CompressiveStrengthMPa;

	/*
	 * THE PRECONDITION THAT STOPS THIS MEASURING THE WRONG AXIS. Compression is what governs
	 * this joint today, so bending in tension has to overtake it — a fixture where it did not
	 * would read the same number before and after the fix and prove nothing at all. Shear is
	 * exactly zero: gravity is normal to a bed joint.
	 */
	TestTrue(
		FString::Printf(
			TEXT("FIXTURE PRECONDITION: bending in tension must govern — tension %.10f vs ")
			TEXT("compression %.10f vs centred %.10f"),
			ExpectedUtilisation, CompressionUtilisation, CentredUtilisation),
		ExpectedUtilisation > CompressionUtilisation && ExpectedUtilisation > CentredUtilisation);

	/*
	 * AND THE WALL MUST STILL STAND. One brick overhanging half its bed really does hold; the
	 * change is that it is a hundred-and-twentieth of the way off rather than a four-thousandth.
	 */
	TestTrue(
		FString::Printf(TEXT("FIXTURE PRECONDITION: the wall must still stand, expected %.10f"),
			ExpectedUtilisation),
		ExpectedUtilisation < 1.0);

	AddInfo(FString::Printf(
		TEXT("the waisted brick's bed joint: %s eccentric against %s centred"),
		*Bits(ExpectedUtilisation), *Bits(CentredUtilisation)));

	/*
	 * THE JOINT IS FOUND BY ROLE, NOT BY INDEX. Which handle the producer happened to give
	 * it is an implementation detail of RunningBond's pairing order, and hard-coding it would
	 * make this test fail for a reason that has nothing to do with geometry. Finding exactly
	 * one bed joint beneath the brick is also the N = 1 precondition the moment rule needs.
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
	 * CLAIM ONE, AND IT FAILS FOR THE WHOLE CLASS. Whatever the binding drops — a centre of
	 * mass today, a rectangle or a field nobody has invented yet tomorrow — a wall that went
	 * through it has to be as completely described as the wall that went in.
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
	 * THE ROUTING MUST BE IDENTICAL, and it is a separate claim from the one below. Moments
	 * ride alongside the routed force rather than changing it, so the two structures must
	 * agree here exactly — a difference would mean the replay had disturbed the load path,
	 * which is a different and worse fault than a lost centre of mass.
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
	 * CLAIM TWO. Same wall, same joint, same force — so the only thing that can move the
	 * answer is whether anybody said where the brick's weight acts.
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
 * A MOMENT BREAKS THE JOINT IT OVERLOADS — under the first-crack brittleness model the head
 * joint that reads past its elastic section-modulus capacity actually GIVES, and the chain
 * hanging off it comes down.
 *
 * WHAT THIS TEST SAYS NOW, AND THE TWO INVERSIONS BEHIND IT (first-crack promotion, 2026-08-28).
 * This test has swung twice, and the current reading restores its ORIGINAL premise. (1) Before
 * the equilibrium LP became the break authority it pinned "a joint the readout calls over
 * capacity must actually give", and the bug was that the per-joint sweep did not see the
 * eccentric moment, so the joint held forever. (2) Slice 3b/4 made the LP the authority and it
 * reasoned in PLASTIC limit analysis — a no-tension f_t/f_c stress block carries about 2.78x the
 * seventeen-brick demand — so the LP CHARITABLY STOOD the chain and the test was re-pinned to
 * "the joint over its elastic capacity is carried, not broken." (3) The first-crack promotion
 * flips the LP's bonded-joint bending capacity to the UNCRACKED peak-fibre limit (3x stricter
 * than plastic no-tension): a bonded joint now cracks when its peak fibre reaches f_t, which is
 * EXACTLY production's elastic section-modulus readout (W = A*h/3). So the LP verdict converges
 * back onto the elastic readout, the seventeen-brick head joint is found infeasible, and the
 * chain FALLS. The plastic-stands re-pin is retired; "a moment breaks the joint it overloads" is
 * true again, this time because the ruled brittleness model, not the elastic display, condemns it.
 *
 * THE FALL BOUNDARY IS THE ELASTIC-READOUT CROSSING, AND THAT COINCIDENCE IS THE PHYSICS. First
 * crack is the uncracked peak-fibre condition sigma = -N/A + |M|/W <= f_t, and on a head joint
 * under gravity N = 0, so it reduces to |M|/W <= f_t — which is precisely what
 * GetConnectionUtilisation composes and reports. The first-crack load factor is therefore
 * lambda* = 1 / readout, and the chain falls exactly when the readout crosses 1.0. The test
 * pins that identity: (readout > 1.0) == (the chain falls), for every row.
 *
 * THE READOUT AND THE BREAK DECISION NOW AGREE, where under the plastic re-pin they diverged.
 * GetConnectionUtilisation still composes FConnection::UtilisationUnder over the routed force AND
 * moment, still sees the eccentricity, and still crosses 1.0 between sixteen and seventeen
 * bricks — and under first crack the break authority follows it rather than overriding it.
 *
 * THE ARITHMETIC. Under slice 3's rule the moment on a determinate joint is this piece's own
 * weight PLUS what it received from above, acting at its own centre; the load arriving from above
 * does not yet carry its own lever arm, which is slice 5. So the head joint's moment is simply
 * the chain weight times 11.25 cm, and the utilisation is linear in the number of bricks:
 *
 *     W       = 1.9 * 21.5 * 10.25 * 6.5 / 1000 * 980  = 2667.198625 uu per brick
 *     e       = 22.5 / 2                               = 11.25 cm to the joint's mid-plane
 *     W_u     = (4/3) * 5.125 * 3.25^2                 = 72.1770833 cm3
 *     sigma_b = n * 2667.198625 * 11.25 / W_u          = n * 0.041572731 MPa
 *     sigma_n = 0                                        gravity is parallel to a head joint
 *     tension = n * 0.041572731 / 0.70                 = n * 0.059389616
 *
 * n = 16 gives 0.9502339 (first-crack lambda* = 1/0.9502339 = 1.0524 >= 1) and HOLDS; n = 17
 * gives 1.0096235 (first-crack lambda* = 1/1.0096235 = 0.9905 < 1) and GIVES. So sixteen stands
 * and seventeen falls, and that 16/17 pair is the discriminating boundary. (Mean basis since the
 * 2026-08-13 re-anchor: the divisor moved 0.10 -> 0.70, so the crossing moved from n = 3 — the
 * characteristic-basis pair was 0.8314546 / 1.2471819 at n = 2 / 3.) The just-below-capacity
 * sixteen-brick chain is NOT a red fixture, which is exactly why it is in the table: it is the
 * control that stops an implementation which breaks whatever it is shown from passing.
 *
 * THE FIXTURE IS A CORBEL, NOT A ROW. A horizontal chain of bricks does not work at all:
 * every piece with no bed joint beneath it falls back to ALL of its head joints, so the
 * middle of a row supports its neighbour and is supported by it — a cycle, which the solver
 * correctly strands rather than solving. Stacking the chain vertically off one hanging brick
 * gives each upper brick a bed joint beneath it, and leaves the hanging brick with exactly
 * one head joint and nothing else:
 *
 *              [ C ]
 *              [ B ]
 *     [ pad ]  [ A ]        one head joint, the whole chain hanging off it
 *     =======
 *
 * WHICH AXIS GOVERNS IS ASSERTED. On a head joint under gravity the mean normal stress is
 * exactly zero and shear carries the whole force against mortar's cohesion, so the three
 * axes are close enough in kind to be worth comparing: at n = 17 tension reads 1.0096235
 * against shear at 0.0756179 (n x 0.0044481) and compression at 0.0706736 (n x 0.0041573).
 * Bending in tension is what has to take the joint apart, and a fixture where shear got
 * there first would be a different test.
 *
 * ASSERTED ON THE MECHANISM AND THE SUPPORT STATE, NEVER ON MOVEMENT. FStructure has no
 * positions to move and a severed joint moves nothing anyway — two pieces can part and stay
 * exactly where they are — so the falling row's claim is the head joint's own state (it HAS
 * given, and carries the break-pass stamp of the pass it gave on) plus the outcome that the
 * chain is no longer held (every hanging brick reads Falling, not Supported). The standing row's
 * claim is the mirror: nothing gave, the cascade ran zero passes, and every brick stays
 * Supported.
 *
 * NO WORLD, NO TICK. The break decision is the equilibrium LP over a graph, not a simulation.
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
	 * A head joint separates on X, so its in-plane frame is Y and Z. The lever arm runs along
	 * X and gravity along Z, so the moment is about Y — resisted by the joint's 3.25 cm of
	 * DEPTH, not by its 5.125 cm of width. The two moduli here differ by 1.58x, so the pair
	 * being the right way round is load-bearing in this fixture.
	 */
	constexpr double HeadJointModulusCm3 = SectionModulusCm3(HalfWidthCm, HalfHeightCm);

	struct FChainCase
	{
		const TCHAR* Description;

		/** How many bricks hang off the one head joint. */
		int32 BricksInChain;

		/**
		 * Whether the head joint gives under the FIRST-CRACK model — and, because first crack IS
		 * the uncracked peak-fibre condition on a zero-normal head joint, this is the SAME flag as
		 * "the elastic section-modulus readout crosses 1.0". The test asserts that coincidence
		 * rather than assuming it: (readout > 1.0) == bFallsAtFirstCrack, for every row. When it
		 * holds the chain falls (head joint given, break-pass stamped, bricks Falling); when it
		 * does not the chain stands (nothing given, zero passes, bricks Supported).
		 */
		bool bFallsAtFirstCrack;
	};

	const TArray<FChainCase> Cases = {
		/*
		 * THE CONTROL. Sixteen bricks put the joint at 95% of its ELASTIC section-modulus
		 * capacity (0.9502339) — genuinely, visibly loaded, and the readout says so — and it
		 * STANDS: first-crack lambda* = 1/0.9502339 = 1.0524 >= 1, no admissibility is lost, and
		 * the cascade breaks nothing. (Two and three on the characteristic basis; the mean
		 * re-anchor moved the elastic crossing to seventeen brick weights, 1/0.059389616.)
		 */
		{ TEXT("sixteen bricks hanging off one head joint"), 16, false },

		/*
		 * THE CLAIM, RE-INVERTED BY THE FIRST-CRACK PROMOTION (2026-08-28). The seventeenth brick
		 * takes the ELASTIC readout to 1.0096, just past its section-modulus capacity — and under
		 * first crack the LP's bonded-joint bending capacity is that same peak-fibre limit, so the
		 * head joint is found INFEASIBLE (first-crack lambda* = 1/1.0096235 = 0.9905 < 1) and
		 * GIVES. This retires the Slice-3b/4 plastic-stands re-pin, whose ~2.78x no-tension margin
		 * charitably held the chain; the ruled brittleness model condemns the joint the moment
		 * overloads, and the chain hanging off it falls.
		 */
		{ TEXT("seventeen bricks hanging off one head joint"), 17, true },
	};

	constexpr double Tolerance = 1.0e-9;

	for (const FChainCase& Case : Cases)
	{
		/*
		 * A grounded pad, then a stack of bricks beside it, the lowest of which is joined to
		 * the pad by a head joint and to nothing else. Each brick above sits squarely on the
		 * one below — its own bed patch, so its lever arm is exactly zero and it contributes
		 * weight to the head joint without contributing any eccentricity of its own.
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

		/* Gravity runs parallel to a head joint, so the whole force is shear and sigma_n is 0. */
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
		 * THE COINCIDENCE THAT IS THE PHYSICS, restated against the arithmetic so the two cannot
		 * drift: first crack on a zero-normal head joint reduces to |M|/W <= f_t, which is exactly
		 * the elastic readout, so the joint falls precisely when the readout crosses 1.0. The
		 * fall flag and the readout-crossing must therefore be the same boolean.
		 */
		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE PRECONDITION: the case says the head joint %s at first crack and ")
				TEXT("the ELASTIC readout arithmetic says %.10f (falls iff readout > 1.0)"),
				Case.Description, Case.bFallsAtFirstCrack ? TEXT("falls") : TEXT("stands"),
				ExpectedUtilisation),
			(ExpectedUtilisation > 1.0) == Case.bFallsAtFirstCrack);

		/*
		 * THE READOUT ALREADY SEES IT, and asserting that first is what makes the failure
		 * below unambiguous. Slice 3 landed the moment into GetConnectionUtilisation; if this
		 * line ever goes red the fault is upstream of the break decision and the rest of this
		 * test is measuring the wrong thing.
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
		 * THE BREAK DECISION IS THE FIRST-CRACK LP. The eighteen-piece structure (pad plus
		 * seventeen bricks) sits well below the 200-block cap, so the equilibrium LP decides. With
		 * the first-crack rows live, the head joint's bending capacity is its uncracked peak-fibre
		 * limit — the same section-modulus reading the display shows — so the seventeen-brick chain
		 * is found infeasible (lambda* 0.9905 < 1) and the head joint gives, while the sixteen-brick
		 * chain (lambda* 1.0524 >= 1) is admissible and nothing breaks.
		 */
		const int32 Passes = Structure.SolveAndBreak();

		if (Case.bFallsAtFirstCrack)
		{
			/*
			 * THE FALLING ROW (seventeen bricks). MEASURED 2026-08-28: the cascade runs 2 passes,
			 * the head joint gives on pass 1, and the whole hang comes down. The mechanism is the
			 * head joint parting (the joint the moment overloads); the outcome is that no brick in
			 * the chain is still held. The intermediate bed joints part too as the severed chain
			 * comes apart, so they are NOT asserted individually — enumerating which of them gives
			 * on pass 2 would be pinning an implementation detail, not the claim.
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
			 * THE OUTCOME, ON THE SUPPORT STATE AND NEVER ON MOVEMENT. Once the head joint parts
			 * the lowest brick loses its only support and the chain above loses its path to the
			 * pad, so every hanging brick reads Falling — the structure genuinely came down.
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
			 * THE STANDING ROW (sixteen bricks). The first-crack margin is 1.0524, so the LP finds
			 * an admissible force system and nothing breaks: zero passes, the head joint intact and
			 * unstamped, and every brick still held. This is the control that stops an
			 * implementation which breaks whatever it is shown.
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
			 * AND EVERY BED JOINT HOLDS. Each brick sits squarely on the one below, so its lever
			 * arm is exactly zero and its bed joint is at a few ten-thousandths of capacity — a
			 * cascade that took them with it would be breaking on something other than the
			 * eccentricity this test is about.
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
		 * AND THE READOUT AGREES WITH THE VERDICT — the coincidence pinned as the point. Under
		 * first crack the display's section-modulus reading and the break decision cross 1.0
		 * together: the seventeen-brick head joint reads above 1.0 AND gives, the sixteen-brick
		 * one reads below AND holds. An implementation that stopped composing the moment into the
		 * readout, or that used a plastic no-tension capacity for the break, would split them.
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
 * LOAD ARRIVING THROUGH A JOINT MUST REMEMBER WHERE IT CAME FROM — today it forgets, so a
 * corbel cannot feel the wall hanging off the corbel above it.
 *
 * WHAT IS WRONG TODAY. The accumulation carries a FORCE down the load path and nothing else:
 * ReceivedFromAboveUU is a scalar, so every brick's share arrives at the piece below with no
 * lever arm attached and is then treated as though it acted at THAT piece's own centre of
 * mass. A brick whose own weight is eccentric levers its joint correctly; a brick carrying
 * another brick that is eccentric levers it as though the load above had been placed neatly
 * on its middle. What is missing is a moment travelling with the force.
 *
 * THE ACCUMULATED QUANTITY IS A VECTOR AND ITS REFERENCE POINT IS EACH JOINT'S OWN CENTROID.
 * A moment is only a number about a point, and ConnectionMoments already stores each joint's
 * moment about ITS OWN centroid — which is what the readout explains and what the section
 * modulus resists. Passing one down a chain therefore means re-referencing it at every step,
 * by the ordinary moment-transfer relation:
 *
 *     M_about(c_to) = M_about(c_from) + (c_from - c_to) x F_transmitted
 *
 * which is just Varignon: M_B = sum (r_i - B) x F_i = M_A + (A - B) x R. So a joint's moment
 * is its own piece's weight about its own centroid, PLUS, for every joint above it, that
 * joint's moment carried down and re-referenced with the share that came through it.
 *
 * THE OTHER TWO CANDIDATE REFERENCE POINTS ARE BOTH WRONG, and worth naming so nobody
 * relitigates. The receiving PIECE'S centre of mass is what the code effectively uses today —
 * it makes the answer depend on where the brick happens to be rather than on where the load
 * came through, and it is exactly the term that vanishes when a chain is stacked squarely,
 * which is why the defect is invisible in every fixture that stacks squarely. The WORLD ORIGIN
 * would be arithmetically valid and practically awful: every stored value would be the moment
 * of the whole wall about a point kilometres away, the joint's own moment would be recovered
 * as a difference of two huge numbers, and slice 4's readout would stop describing the joint
 * it is drawn beside.
 *
 * AND IT IS THE FORWARD-COMPATIBILITY SLICE. Once the thing being carried is a moment vector
 * about a named point, an applied force at a point — r x F from an explosion or an impact —
 * is one more term in the same sum, needing no rule of its own. Nothing here builds that.
 *
 * THE ORACLE IS DERIVED THE OTHER WAY ROUND, and that is the whole of its value.
 * ChainMomentAboutJointUuCm sums every brick above a joint times its own distance from that
 * joint: no recursion, no transfer term, no load path. The accumulation walks down one joint
 * at a time; the oracle never walks at all. Two derivations that agree are evidence; an oracle
 * that mirrored the algorithm would be a tautology.
 *
 * WHAT THE FIXTURES ARE, AND WHY THE FIRST TWO ROWS MUST NOT MOVE.
 *
 *     SQUARE      CORBELLED        ZIG-ZAG
 *      [C]            [C]            [C]
 *      [B]          [B]            [B]
 *      [A]          [A]            [A]
 * [pad][ ]     [pad][ ]       [pad][ ]
 *
 * A chain stacked SQUARELY is the control, and it is a control precisely because the answer
 * does not move: every brick sits directly above the bed patch below it, so the received load
 * already acts on the line the old rule assumed, and re-referencing it changes nothing at all.
 * MOMENTS_DESIGN.md's headline "two in a chain reads 0.8315 today and 1.626 with the
 * accumulation" does NOT describe this fixture and does not reproduce on it — 1.626 is a
 * chain laid HORIZONTALLY, brick beside brick, each hanging off the last by a head joint, and
 * that shape is a cycle the solver strands rather than a structure it solves. Reproduced here
 * from the design's own figures: arms of 10.75 and 33.25 cm against a 72.1771 cm3 modulus give
 * 4.0930 x 0.39725 = 1.626 exactly, which is the horizontal chain and nothing else. Against
 * the emitted MID-PLANE centroid the same horizontal chain would read 4 x 0.4157273077 =
 * 1.6629. (Both are characteristic-basis figures, /7 on the mean basis.) Neither number
 * belongs to a vertical stack, whose two-brick case stays at 0.1187792 before and after.
 *
 * SO THE FIXTURE THAT MOVES IS A CORBELLED CHAIN. Step each brick half its length further out
 * and the load path leaves the piece by a patch that is nowhere near the patch it arrived on,
 * which is the whole of the effect: two bricks take the head joint from 0.1187792 to 0.1755293
 * — a 48% jump from the identical bricks, purely because the moment now travels. (On the
 * retired characteristic basis those read 0.8314546 and 1.2287052 and the jump crossed 1.0;
 * at the mean f_x1 = 0.70 nothing in this table breaks any more, so the BREAK arm of the
 * claim lives in MomentBreaksTheJointItOverloads' seventeen-brick chain, and every row here
 * asserts the reading and the moment vector.)
 *
 * THE ZIG-ZAG ROW IS THE ONE THAT PINS THE SIGN, and it is not decoration. A chain that steps
 * out and then steps back puts the brick above's weight on the far side of the joint from the
 * one below's, so the two moments CANCEL — the middle bed joint carries exactly zero moment
 * while carrying two bricks. An accumulation that summed magnitudes, or that got the sign of
 * the transfer term backwards, cannot produce that. It is also the shape a plain toothed wall
 * end has, where the load funnels straight down onto the patch it will leave by.
 *
 * WHICH AXIS GOVERNS IS ASSERTED FOR THE HEAD JOINT, because ComputeUtilisation returns the
 * WORST of three and a test aimed at bending measures shear the moment shear is higher. On a
 * head joint under gravity the mean normal stress is exactly zero, so the whole force is shear
 * against mortar's cohesion — 0.0089 for two bricks against 0.1755 in tension. Every joint's
 * three axes are worked out and printed, and the claim is against the worst of them, so a
 * fixture whose governing axis moved would fail here rather than quietly measure the other one.
 *
 * ASSERTED ON THE MECHANISM. The moment vector each joint carries, the force beside it and the
 * utilisation that comes out — never on anything moving, since FStructure has no positions and
 * two pieces can part without going anywhere.
 *
 * NO WORLD, NO TICK. Boxes and doubles.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureMomentAccumulatesTest,
	"DestructionGame.Core.Structure.MomentAccumulatesAlongTheLoadPath",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureMomentAccumulatesTest::RunTest(const FString& Parameters)
{
	using namespace StructureMomentTestSupport;

	/*
	 * The expectations are ratios of published strengths, so they only mean what they say
	 * while the profile still carries the figures they were derived against.
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
	 * A HEAD JOINT SEPARATES ON X, so its in-plane frame is Y and Z; the lever arm runs along
	 * X and gravity along Z, so the moment is about Y and is resisted by the joint's 3.25 cm
	 * of DEPTH rather than by its 5.125 cm of width. The two moduli differ by 1.58x, so the
	 * pair being the right way round is load-bearing here.
	 */
	constexpr double HeadJointModulusCm3 = SectionModulusCm3(HalfWidthCm, HalfHeightCm);
	constexpr double HeadJointAreaSqCm = BrickWidthCm * BrickHeightCm;

	/**
	 * ONE CHAIN, ONE HEADLINE NUMBER, AND WHAT THAT NUMBER IS TODAY.
	 *
	 * Both are carried so the failure message says how far the head joint has to move and in
	 * which direction, rather than only that it is wrong. The two agreeing is what makes a row
	 * a control.
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
		 * THE CONTROLS, AND THEY COME FIRST. A squarely stacked chain hands its load down the
		 * line it was already assumed to act on, so nothing may move — and without these rows
		 * every claim below is satisfied by an implementation that invents a lever arm for
		 * anything it is shown.
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
		 * THE CLAIM. The upper brick steps half its length out over open air, so the load
		 * leaves it 5.375 cm from where its own weight acts and arrives on the head joint
		 * 16.625 cm out rather than 11.25. Arms of 11.25 and 22.0 against a 72.1771 cm3
		 * modulus: 33.25 brick-weight-centimetres where the old rule saw 22.5.
		 *
		 * NO ROW IN THIS TABLE GIVES ANY MORE (mean re-anchor 2026-08-13): at the mean
		 * f_x1 = 0.70 the worst chain here reads 0.348, so every bHeadMustGive is false
		 * and what this test asserts is the reading and the moment vector. The break
		 * decision agreeing with an over-capacity moment is MomentBreaksTheJointIt-
		 * Overloads' seventeen-brick chain now.
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
		 * THE SIGN. Step out and then straight back, and the top brick's weight lands as far
		 * to the LEFT of the middle bed joint as the middle brick's does to the right. The two
		 * cancel exactly and that joint carries a chain of two bricks with no moment at all.
		 */
		{
			TEXT("three bricks zig-zagging back over the joint below"),
			{ BrickPitchCm, BrickPitchCm + CorbelStepCm, BrickPitchCm },
			0.17816884615384615,
			0.23491892307692308,
			false
		},

		/*
		 * AND THE SAME CHAIN MIRRORED. Hanging to the LEFT of the pad flips the joint normal,
		 * flips every lever arm and flips the sign of every moment in the chain; the magnitude
		 * that reaches the stress may not notice. An implementation that dropped a sign
		 * somewhere in the transfer term reads a different number here from the row above it.
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
	 * ABSOLUTE RATHER THAN RELATIVE, because one of these rows is meant to come out at exactly
	 * zero and a relative bound around zero admits nothing. The moments here run to 1.8e5
	 * uu.cm, where double rounding is worth about 2e-11, so a millionth of a uu.cm is five
	 * orders of magnitude of slack over rounding and eleven below anything this test could
	 * care about.
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
		 * THE LOAD PATH IS WHAT MAKES EVERY PIECE HERE DETERMINATE, so it is asserted rather
		 * than assumed. The bottom brick has to hang off its head joint — a bed joint would
		 * win the tier outright and there would be no fallback — and every brick above it has
		 * to be resting on the one below.
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
			 * THE ARBITRATION. Every lever arm below is measured from this centroid, so if the
			 * producer ever moved it the expectations would silently stop describing this
			 * joint. Pinned here so the two fail together and the message says which moved.
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
			 * THE MECHANISM ITSELF. Magnitude rather than signed value, for one reason only:
			 * the stored sign flips with which end of the joint the producer named first — the
			 * invariance Structure.HangingBrickPeelsRatherThanShears already pins — and only
			 * the magnitude reaches a stress. The AXIS is not free, though: gravity crossed
			 * with a lever arm along the wall is a moment about world Y and nothing else, and
			 * a component about X or Z would be a twist this rectangle has no modulus for.
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
			 * AND WHAT THAT DOES TO THE JOINT, worked from the rectangle rather than read off
			 * it. A bed joint's shared span shortens by however far the brick above is stepped
			 * out, and the bending is about Y so it is resisted by the depth ALONG the wall.
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
			 * Signed, positive in tension. Gravity runs PARALLEL to a head joint, so its mean
			 * normal stress is exactly zero and the whole force is shear; it presses squarely
			 * on a bed joint, so there the whole force is compression and the shear is zero.
			 */
			const double NormalStressMPa =
				bHeadJoint ? 0.0 : -ExpectedForceUu / (AreaSqCm * ForceUnitsPerMPaPerSqCm);
			const double ShearStressMPa =
				bHeadJoint ? ExpectedForceUu / (AreaSqCm * ForceUnitsPerMPaPerSqCm) : 0.0;

			/*
			 * AND THE DEEP BEAM STANDING OVER A BED JOINT, WHICH IS WHY JOINT 1 OF THE
			 * THREE-BRICK CORBEL MOVED FROM 0.2420600000 TO 0.1491896467 AT SLICE 5.
			 *
			 * A stack of courses over a joint does not resist the overturning moment as one bed
			 * patch: the section taking it is a VERTICAL one through the bonded masonry standing
			 * above, t*D^2/6, and the joint gives at whichever of the two opens it less.
			 * ARCHING_DESIGN.md slice 5, and the user's ruling of 2026-08-06.
			 *
			 * THE CHAIN IS WHERE THAT IS EASIEST TO COUNT. Joint k is under brick k, so the
			 * masonry over it is bricks k upward and D is that many course pitches. A HEAD joint
			 * has no bed plane and no masonry standing on it in this sense — which is the whole
			 * reason MOMENTS_DESIGN case (b) and the squarely-stacked chain above did not move
			 * — and the TOP brick of any chain has nothing resting on it, so it is one unit
			 * rather than a composite of several and keeps its own patch.
			 *
			 * BOTH EDGES MOVE TOGETHER. Where the deep beam takes the moment the bed patch is not
			 * bending at all, so what is left is the bed plane under a uniform N/A and the
			 * vertical plane under +-M/W_c, and the worst squeezed fibre is the larger of the two
			 * rather than their sum.
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
			 * THE HEAD JOINT IS WHERE THE HEADLINE CLAIM LIVES, so the axis carrying it is
			 * asserted rather than merely computed. The bed joints are allowed to be governed
			 * by whichever axis the arithmetic says — the zig-zag row deliberately drives one
			 * of them to zero tension — so their three are printed and the worst is claimed.
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
		 * THE BED JOINTS ARE NOT COLLATERAL. The worst of them in any row here is a quarter of
		 * capacity, so a cascade that took one with it would be breaking on something other
		 * than the eccentricity this test is about.
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
	 * AND THE SAME CLAIM OVER EVERY CHAIN A HALF-BRICK GRID CAN MAKE.
	 * ================================================================================
	 *
	 * HAND-WRITTEN ROWS ONLY COVER THE SHAPES SOMEBODY THOUGHT OF, and the shapes above were
	 * chosen to make a point — every one of them steps in the same direction, or steps back
	 * exactly once. What that leaves untested is the combinatorics: three steps whose moments
	 * partly cancel, a chain that reverses twice, a step that lands a joint's centroid exactly
	 * on the brick above's centre of mass. Those are where an accumulation with the wrong sign
	 * or the wrong reference point produces a plausible number rather than an obvious one.
	 *
	 * EXHAUSTIVE RATHER THAN SEEDED, which is stronger than a repeatable random draw and needs
	 * no seed to print: every combination of steps on a quarter-brick grid, four bricks deep,
	 * is 75 chains and they run in no measurable time. Each case prints its own brick centres,
	 * so a failure is already a fixture somebody can paste into the table above as a named
	 * regression row.
	 *
	 * THE FIRST BRICK MAY ONLY STEP AWAY FROM THE PAD. Stepping toward it would put a course-1
	 * brick over the pad with a mortar gap between them — a face MakeInterface would call a
	 * joint if anybody asked it to, and a fixture that relies on nobody asking is a fixture
	 * that is wrong for a reason unrelated to moments. Bricks two courses up and higher are
	 * nowhere near the pad and are free either way.
	 */
	constexpr double SweepOffsetsCm[] = {
		-CorbelStepCm, -CorbelStepCm / 2.0, 0.0, CorbelStepCm / 2.0, CorbelStepCm
	};

	constexpr int32 SweepOffsetCount = UE_ARRAY_COUNT(SweepOffsetsCm);

	int32 SweepChains = 0;
	int32 SweepMismatches = 0;

	for (int32 First = 0; First < SweepOffsetCount; ++First)
	{
		/* Away from the pad only; the offsets are ordered, so this is the back half of them. */
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
					 * NEVER NaN, ALWAYS FINITE, AND NEVER READING INTACT WHEN IT IS NOT. A NaN
					 * moment would sail through the magnitude comparison below — every
					 * comparison against a NaN is false, so the check is written to catch it
					 * rather than to be satisfied by it.
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
