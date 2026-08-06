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
}

/**
 * A BRICK HELD BY ONE HEAD JOINT PEELS OFF IT RATHER THAN SHEARING THROUGH IT — the whole
 * point of the moment work, and the first slice where it is visible in a structure rather
 * than in a hand-built load.
 *
 * WHAT IS WRONG TODAY. Gravity runs parallel to a head joint, so the mean normal stress
 * across it is EXACTLY zero and the only axis carrying anything is shear against mortar's
 * cohesion. The joint reads 0.0200165 — about fifty bricks from failure — while what is
 * physically happening is that the brick's weight acts 11.25 cm to one side of the joint
 * and levers the top of it open, against a tensile strength one hundredth of the
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
 * the answer is 0.4157, and the joint geometry is asserted below so that the number and
 * the reason for it fail together.
 *
 * THE ARITHMETIC, spelled from published figures and brick dimensions:
 *
 *     W       = 1.9 * 21.5 * 10.25 * 6.5 / 1000 * 980  = 2667.198625 uu
 *     M       = 2667.198625 * 11.25                    = 30005.98453125 uu.cm
 *     W_sec   = (4/3) * 5.125 * 3.25^2                 = 72.177083... cm3
 *     sigma_b = M / W_sec                              = 415.7273077 uu/cm2
 *                                                      = 0.04157273077 MPa
 *     tension = sigma_b / f_xk1 (0.10 MPa)             = 0.4157273077
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
	 * to fail loudly here rather than quietly moving every number below. EN 1996-1-1 Table
	 * 3.2's f_xk1 = 0.10 N/mm2 is the one the answer is divided by; the other two decide
	 * which axis governs, and the friction coefficient decides whether shear gets any help
	 * (it does not — the mean compressive stress on a head joint under gravity is zero).
	 */
	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against f_xk1 = 0.1 MPa, the profile carries %g"),
			GeneralPurposeMortar.TensileStrengthMPa),
		GeneralPurposeMortar.TensileStrengthMPa == 0.1);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against cohesion 0.2 MPa, the profile carries %g"),
			GeneralPurposeMortar.ShearCohesionMPa),
		GeneralPurposeMortar.ShearCohesionMPa == 0.2);

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
		 * there; what changes is that it is now a fifth of the way to coming off rather
		 * than a fiftieth, so two or three of them in a chain part. A row that expected
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
 *             / f_xk1 (0.10 MPa)                       = 0.058203838
 *
 * against the 2.5386781e-4 a centred load reads — a 229x change, and the joint still holds,
 * which is right: one overhanging brick does not come off.
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
		FString::Printf(TEXT("FIXTURE: derived against f_xk1 = 0.1 MPa, the profile carries %g"),
			GeneralPurposeMortar.TensileStrengthMPa),
		GeneralPurposeMortar.TensileStrengthMPa == 0.1);

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
	 * change is that it is a twentieth of the way off rather than a four-thousandth.
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
 * A JOINT THE READOUT CALLS OVER CAPACITY MUST ACTUALLY GIVE — today the strain display and
 * the break decision disagree, and they disagree in the direction where nothing falls.
 *
 * WHERE THEY PART. GetConnectionUtilisation composes FConnection::UtilisationUnder over the
 * routed force AND the routed moment, so it sees the eccentricity. SolveAndBreak's sweep
 * calls FConnection::ApplyForce with the force alone, and ApplyForce has no moment to give
 * it, so the decision is made on the averaged stress the whole of slice 3 exists to replace.
 * A joint can therefore sit at 1.25 of capacity, be drawn at 1.25 of capacity, and hold
 * forever.
 *
 * THIS IS THE PROJECT'S RECURRING SIGNATURE — a second evaluation of the same question that
 * agrees until it does not — and the reason Connection.h insists ApplyForce be built OUT OF
 * UtilisationUnder rather than beside it. The composition is what has come apart, not the
 * arithmetic.
 *
 * WHY A CHAIN OF THREE, AND WHY TWO IS NOT ENOUGH. Under slice 3's rule the moment on a
 * determinate joint is this piece's own weight PLUS what it received from above, acting at
 * its own centre; the load arriving from above does not yet carry its own lever arm, which
 * is slice 5. So the head joint's moment is simply the chain weight times 11.25 cm, and the
 * utilisation is linear in the number of bricks:
 *
 *     W       = 1.9 * 21.5 * 10.25 * 6.5 / 1000 * 980  = 2667.198625 uu per brick
 *     e       = 22.5 / 2                               = 11.25 cm to the joint's mid-plane
 *     W_u     = (4/3) * 5.125 * 3.25^2                 = 72.1770833 cm3
 *     sigma_b = n * 2667.198625 * 11.25 / W_u          = n * 0.041572731 MPa
 *     sigma_n = 0                                        gravity is parallel to a head joint
 *     tension = n * 0.041572731 / 0.10                 = n * 0.41572731
 *
 * n = 2 gives 0.8314546 and holds; n = 3 gives 1.2471819 and gives. MOMENTS_DESIGN.md's
 * 1.626 for a two-brick chain is the slice-5 figure, where the moment accumulates along the
 * load path and the upper brick's own lever arm survives the trip down. Two bricks is
 * therefore NOT a red fixture today, which is exactly why it is in the table: it is the
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
 * axes are close enough in kind to be worth comparing: at n = 3 tension reads 1.2471819
 * against shear at 0.0600495 and compression at 0.0124718. Bending in tension is what has to
 * take the joint apart, and a fixture where shear got there first would be a different test.
 *
 * ASSERTED ON THE MECHANISM, NEVER ON MOVEMENT. FStructure has no positions to move and a
 * severed joint moves nothing anyway — two pieces can part and stay exactly where they are —
 * so the claim is the joint's own state: it has given, it carries a break-pass stamp, and
 * the cascade reports the pass. The closing invariant is the general form of the same thing:
 * once a cascade has settled, NO joint still in the structure may read above 1. That is the
 * agreement between the readout and the decision, stated as a property rather than as a
 * number, and it is what actually goes red here.
 *
 * NO WORLD, NO TICK. The break decision is arithmetic over a graph.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureMomentBreaksTheJointTest,
	"DestructionGame.Core.Structure.MomentBreaksTheJointItOverloads",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureMomentBreaksTheJointTest::RunTest(const FString& Parameters)
{
	using namespace StructureMomentTestSupport;

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against f_xk1 = 0.1 MPa, the profile carries %g"),
			GeneralPurposeMortar.TensileStrengthMPa),
		GeneralPurposeMortar.TensileStrengthMPa == 0.1);

	TestTrue(
		FString::Printf(TEXT("FIXTURE: derived against cohesion 0.2 MPa, the profile carries %g"),
			GeneralPurposeMortar.ShearCohesionMPa),
		GeneralPurposeMortar.ShearCohesionMPa == 0.2);

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

		/** Whether the head joint has to come apart under them. */
		bool bMustGive;
	};

	const TArray<FChainCase> Cases = {
		/*
		 * THE CONTROL, AND IT COMES FIRST. Two bricks put the joint at five sixths of
		 * capacity — genuinely, visibly loaded, and still holding. Without it every claim
		 * below is satisfied by an implementation that breaks whatever it is shown.
		 */
		{ TEXT("two bricks hanging off one head joint"), 2, false },

		/* THE CLAIM. The third brick takes it past capacity and it has to give. */
		{ TEXT("three bricks hanging off one head joint"), 3, true },
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

		/* The table's own claim, restated against the arithmetic so the two cannot drift. */
		TestTrue(
			FString::Printf(
				TEXT("%s: FIXTURE PRECONDITION: the case says it %s give and the arithmetic says %.10f"),
				Case.Description, Case.bMustGive ? TEXT("must") : TEXT("must not"),
				ExpectedUtilisation),
			(ExpectedUtilisation > 1.0) == Case.bMustGive);

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

		// --- the claim: the break decision has to agree with it --------------------------

		const int32 Passes = Structure.SolveAndBreak();

		TestEqual(
			FString::Printf(
				TEXT("%s: reading %.10f, the cascade should break in %d pass(es), it ran %d"),
				Case.Description, ExpectedUtilisation, Case.bMustGive ? 1 : 0, Passes),
			Passes, Case.bMustGive ? 1 : 0);

		TestTrue(
			FString::Printf(
				TEXT("%s: the head joint should%s have given at %.10f of capacity"),
				Case.Description, Case.bMustGive ? TEXT("") : TEXT(" NOT"), ExpectedUtilisation),
			Structure.GetConnection(HeadIndex).HasGiven() == Case.bMustGive);

		TestEqual(
			FString::Printf(
				TEXT("%s: the head joint's break pass should be %d, it is %d"),
				Case.Description, Case.bMustGive ? 1 : INDEX_NONE,
				Structure.GetBreakPass(HeadIndex)),
			Structure.GetBreakPass(HeadIndex), Case.bMustGive ? 1 : INDEX_NONE);

		/*
		 * THE BED JOINTS ARE NOT COLLATERAL. Each brick sits squarely on the one below, so
		 * its lever arm is exactly zero and its bed joint is at a few ten-thousandths of
		 * capacity — a cascade that took them with it would be breaking on something other
		 * than the eccentricity this test is about.
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
		}

		/*
		 * THE CLOSING INVARIANT, and the general form of the whole claim: a settled cascade
		 * may leave NO joint that is still in the structure reading above 1. The readout and
		 * the break decision are two evaluations of one question, and this is them agreeing.
		 */
		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			if (Structure.GetConnection(Joint).HasGiven())
			{
				continue;
			}

			const double Settled = Structure.GetConnectionUtilisation(Joint);

			TestTrue(
				FString::Printf(
					TEXT("%s: joint %d is still intact after the cascade settled and reads %s of ")
					TEXT("its capacity — the readout and the break decision must not disagree"),
					Case.Description, Joint, *Bits(Settled)),
				Settled <= 1.0);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
