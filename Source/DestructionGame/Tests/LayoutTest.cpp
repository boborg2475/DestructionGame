// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/Layout.h"
#include "Core/Structure.h"
#include "Core/ConnectionLoad.h"
#include "Core/ConnectionStrength.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Named, not anonymous: a unity build merges translation units, so two anonymous
 * MakeNaN would collide. The using-directives go inside each RunTest body, not at file
 * scope, to keep these names from leaking into another file in the same blob.
 */
namespace LayoutTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * Unreal's default gravity, 980 cm/s2, spelled out rather than imported so the test
	 * fails if production gets it wrong. Mass is kg and length cm, so MassKg * 980 is a
	 * force in uu: the 1 N = 100 uu conversion is baked into the 980, and applying it
	 * again is out by 100x.
	 */
	constexpr double GravityCmPerSecondSquared = 980.0;

	constexpr double WeightOf(double MassKg)
	{
		return MassKg * GravityCmPerSecondSquared;
	}

	/*
	 * UK metric brick and its coordinating grid, re-derived rather than quoted.
	 * 215 x 102.5 x 65 mm with a 10 mm joint gives a 225 x 112.5 x 75 mm cell; the
	 * running-bond offset is half a cell along the wall. Every value is an integer
	 * quarter-cm and so exact in binary, which is why the areas below use exact equality.
	 */
	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickDepthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;
	constexpr double MortarCm = 1.0;

	constexpr double BrickPitchCm = BrickLengthCm + MortarCm;
	constexpr double CoursePitchCm = BrickHeightCm + MortarCm;
	constexpr double BondOffsetCm = BrickPitchCm / 2.0;

	/** A half bat: what makes a FLUSH end, and the wall's second piece size. */
	constexpr double HalfBatLengthCm = (BrickLengthCm - MortarCm) / 2.0;

	/*
	 * A brick spans two below, so it has two bed joints of 105.0625 cm2, not one of
	 * 220.375. Overlap along the wall is 21.5 - 11.25 = 10.25 cm over the 10.25 cm depth.
	 * Geometry closes: 220.375 - 2 x 105.0625 = 10.25 cm2, the head-joint mortar gap.
	 */
	constexpr double BedJointOverlapCm = BrickLengthCm - BondOffsetCm;
	constexpr double BedJointAreaSqCm = BedJointOverlapCm * BrickDepthCm;
	constexpr double HeadJointAreaSqCm = BrickDepthCm * BrickHeightCm;
	constexpr double FullBedFaceAreaSqCm = BrickLengthCm * BrickDepthCm;

	/*
	 * The joint is a rectangle; the half-extents are halves of the same two overlaps the
	 * area is the product of, so the rectangle and the area describe one face (the loop
	 * below asserts 4 x h_u x h_v == area on the produced values). A bed joint is
	 * 10.25 x 10.25, so both halves 5.125; a head joint 10.25 x 6.5, so 5.125 and 3.25.
	 */
	constexpr double BedJointHalfAlongWallCm = BedJointOverlapCm / 2.0;
	constexpr double HalfBrickDepthCm = BrickDepthCm / 2.0;
	constexpr double HalfBrickHeightCm = BrickHeightCm / 2.0;
	constexpr double HalfBrickLengthCm = BrickLengthCm / 2.0;

	/*
	 * The joint plane is the mid-plane of the mortar, not either brick's face: a 1 cm bed
	 * has two faces, and taking one would make the geometry depend on which handle is A.
	 * The mid-plane makes swapping the handles swap only the normal, and degenerates onto
	 * the faces at zero thickness. Between courses that is 6.5 + 0.5 = 7.0 cm; within a
	 * course 10.75 + 0.5 = 11.25 cm, which coincides with the bond offset.
	 */
	constexpr double BedJointPlaneZCm = BrickHeightCm + MortarCm / 2.0;
	constexpr double HeadJointPlaneXCm = HalfBrickLengthCm + MortarCm / 2.0;

	/**
	 * 4 x h_u x h_v: the area the emitted rectangle claims, read back independently. The
	 * in-plane axes come from the normal, not from whichever components are non-zero, so
	 * an accidental zero on an in-plane axis reads as zero area rather than being skipped.
	 * Scaling halves back up by four is exact in binary, so == against the area holds.
	 */
	double RectangleAreaOf(const FVector& HalfExtentCm, const FVector& InterfaceNormal)
	{
		int32 SeparationAxis = INDEX_NONE;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (InterfaceNormal[Axis] != 0.0)
			{
				SeparationAxis = Axis;
			}
		}

		if (SeparationAxis == INDEX_NONE)
		{
			return 0.0;
		}

		double AreaSqCm = 4.0;
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (Axis != SeparationAxis)
			{
				AreaSqCm *= HalfExtentCm[Axis];
			}
		}

		return AreaSqCm;
	}

	/*
	 * Masses derived from the profile, following StructureTest.cpp: density comes from
	 * ClayBrick, the single anchor, not a second literal. 1432.4375 cm3 at 1.9 g/cm3 is
	 * 2.72163125 kg; 682.90625 cm3 is 1.29752188 kg. Safe at namespace scope only because
	 * ClayBrick is constant-initialised; a computed density could read zero here through
	 * static-init order, so every test opens with a guard that this derived to positive.
	 */
	const double BrickMassKg =
		ClayBrick.DensityGramsPerCubicCm * BrickLengthCm * BrickDepthCm * BrickHeightCm / 1000.0;

	const double HalfBatMassKg =
		ClayBrick.DensityGramsPerCubicCm * HalfBatLengthCm * BrickDepthCm * BrickHeightCm / 1000.0;

	/** 2667.198625 uu. */
	const double BrickWeightUU = WeightOf(BrickMassKg);

	/**
	 * The bed/head threshold, the same literal production uses. Transcribed from
	 * FStructure::GetJointRole, not re-derived: 1.0 / FMath::Sqrt(2.0) would land an ulp
	 * off the constexpr in Structure.cpp. If the production constant changes, match it.
	 */
	constexpr double BedJointCosine = 0.70710678118654752440;

	/**
	 * Stress in MPa a force produces over an area. Spelled out rather than importing
	 * ForceUnitsPerMPaSqCm, so the test fails if that constant is wrong. 1 N = 100 uu,
	 * 1 cm2 = 100 mm2, 1 MPa = 1 N/mm2, so 1 MPa over 1 cm2 is 10000 uu.
	 */
	constexpr double MPaForForce(double ForceUnits, double AreaSqCm)
	{
		return ForceUnits / (100.0 * 100.0 * AreaSqCm);
	}

	double MakeNaN()
	{
		volatile double Zero = 0.0;
		return Zero / Zero;
	}

	/**
	 * A real +infinity, the same way MakeNaN produces its NaN. Must be the actual IEEE
	 * infinity, not a huge finite number: +inf is caught by an IsFinite check on the
	 * extent, while DBL_MAX is finite and would need an overflow check on the area.
	 */
	double MakeInfinity()
	{
		volatile double One = 1.0;
		volatile double Zero = 0.0;
		return One / Zero;
	}

	/** A box from its centre and full size, since bricks are quoted full size. */
	FPieceBox BoxOfSize(const FVector& CentreCm, const FVector& FullSizeCm)
	{
		FPieceBox Box;
		Box.CentreCm = CentreCm;
		Box.ExtentCm = FullSizeCm * 0.5;
		return Box;
	}

	/** A full brick, centred along the wall at X and sitting in the given course. */
	FPieceBox FullBrickAt(double CentreXCm, int32 Course)
	{
		return BoxOfSize(
			FVector(CentreXCm, 0.0, BrickHeightCm * 0.5 + Course * CoursePitchCm),
			FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm));
	}

	FString Describe(const FPieceBox& Box)
	{
		return FString::Printf(
			TEXT("centre (%.4f, %.4f, %.4f) extent (%.4f, %.4f, %.4f)"),
			Box.CentreCm.X, Box.CentreCm.Y, Box.CentreCm.Z,
			Box.ExtentCm.X, Box.ExtentCm.Y, Box.ExtentCm.Z);
	}

	/**
	 * The tier a joint has for one piece, transcribed from FStructure::GetJointRole. The
	 * normal is first turned to point at this piece: substantially up, a bed joint
	 * beneath it that bears it; substantially down, a bed joint above it bearing nothing;
	 * otherwise a head joint. Only catches a transcription slip, but it answers what the
	 * load numbers cannot: how many joints beneath each brick actually bear it.
	 */
	enum class EJointRole : uint8
	{
		BedBeneath,
		BedAbove,
		Head,
		NotTouchingThisPiece,
	};

	EJointRole RoleFor(const FConnection& Connection, int32 PieceIndex)
	{
		FVector UnitNormal = Connection.InterfaceNormal;
		if (!UnitNormal.Normalize())
		{
			return EJointRole::NotTouchingThisPiece;
		}

		double NormalZTowardPiece = 0.0;
		if (Connection.PieceB == PieceIndex)
		{
			NormalZTowardPiece = UnitNormal.Z;
		}
		else if (Connection.PieceA == PieceIndex)
		{
			NormalZTowardPiece = -UnitNormal.Z;
		}
		else
		{
			return EJointRole::NotTouchingThisPiece;
		}

		if (FMath::Abs(NormalZTowardPiece) > BedJointCosine)
		{
			return NormalZTowardPiece > 0.0 ? EJointRole::BedBeneath : EJointRole::BedAbove;
		}

		return EJointRole::Head;
	}

	/**
	 * Independent oracle for which pairs touch: a brute-force O(n2) scan over the emitted
	 * boxes. Derived differently from the generative producer on purpose — it looks only
	 * at geometry, so it cannot agree with a wrong producer the way a course-walking
	 * oracle would. The rule: two boxes form a face when separated on exactly one axis by
	 * the joint thickness and overlapping positively on the other two. Two axes is an
	 * edge, three a corner; either emitted as a joint gets the tier wrong.
	 */
	struct FContact
	{
		int32 LowerIndex = INDEX_NONE;
		int32 HigherIndex = INDEX_NONE;

		/** 0 = X, 1 = Y, 2 = Z. */
		int32 SeparationAxis = INDEX_NONE;

		double AreaSqCm = 0.0;

		/**
		 * The face's own rectangle, from the box bounds rather than centre differences, so
		 * it is a second derivation. Intersects the [min, max] intervals per axis and takes
		 * the midpoint and half-width. On the separation axis the intersection is empty, but
		 * its midpoint is still the mid-plane of the mortar; the half-extent there is set to
		 * zero because a face has no thickness.
		 */
		FVector CentreCm = FVector::ZeroVector;
		FVector HalfExtentCm = FVector::ZeroVector;
	};

	TArray<FContact> ContactsOf(const TArray<FPieceBox>& Boxes, double JointThicknessCm)
	{
		constexpr double Slack = 1.0e-9;

		TArray<FContact> Contacts;

		for (int32 I = 0; I < Boxes.Num(); ++I)
		{
			for (int32 J = I + 1; J < Boxes.Num(); ++J)
			{
				double Gap[3];
				double Overlap[3];

				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					const double Distance =
						FMath::Abs(Boxes[J].CentreCm[Axis] - Boxes[I].CentreCm[Axis]);
					const double Reach = Boxes[I].ExtentCm[Axis] + Boxes[J].ExtentCm[Axis];
					Gap[Axis] = Distance - Reach;
					Overlap[Axis] = Reach - Distance;
				}

				int32 SeparationAxis = INDEX_NONE;
				int32 SeparationCount = 0;
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					if (Gap[Axis] > Slack)
					{
						SeparationAxis = Axis;
						++SeparationCount;
					}
				}

				if (SeparationCount != 1)
				{
					continue;
				}

				if (!FMath::IsNearlyEqual(Gap[SeparationAxis], JointThicknessCm, Slack))
				{
					continue;
				}

				double Area = 1.0;
				bool bIsAFace = true;
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					if (Axis == SeparationAxis)
					{
						continue;
					}

					if (Overlap[Axis] <= Slack)
					{
						bIsAFace = false;
						break;
					}

					Area *= Overlap[Axis];
				}

				if (!bIsAFace)
				{
					continue;
				}

				FVector CentreCm = FVector::ZeroVector;
				FVector HalfExtentCm = FVector::ZeroVector;

				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					const double Low = FMath::Max(
						Boxes[I].CentreCm[Axis] - Boxes[I].ExtentCm[Axis],
						Boxes[J].CentreCm[Axis] - Boxes[J].ExtentCm[Axis]);
					const double High = FMath::Min(
						Boxes[I].CentreCm[Axis] + Boxes[I].ExtentCm[Axis],
						Boxes[J].CentreCm[Axis] + Boxes[J].ExtentCm[Axis]);

					CentreCm[Axis] = (Low + High) * 0.5;
					HalfExtentCm[Axis] = Axis == SeparationAxis ? 0.0 : (High - Low) * 0.5;
				}

				Contacts.Add({ I, J, SeparationAxis, Area, CentreCm, HalfExtentCm });
			}
		}

		return Contacts;
	}
}

/**
 * The interface factory: areas, normals, orientation and validation, written once.
 * MakeInterface is the only way to build a joint, for the sake of the normal: it is the
 * axis of separation signed by which handle is B, never the centroid direction. For a
 * bed joint the centroid difference normalises to Z = 0.5547, below cos45, so a centroid
 * normal makes every bed joint classify as a head joint and gravity resolves as shear
 * against 0.2 MPa instead of compression against 10 MPa — the wall stands there being
 * wrong. So the expected normals below are exact unit axes.
 *
 * Rejection is the other half: a pair separated on two axes (edge) or three (corner)
 * emitted as a joint is a spurious diagonal that changes the tier. Degenerate boxes fail
 * closed with a zero interface area, which routes through ComputeUtilisation's area guard
 * so a caller that ignores the return value gets a joint that reads as failed.
 *
 * World-free: plain arithmetic over boxes.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLayoutInterfaceTest,
	"DestructionGame.Core.Layout.Interface",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLayoutInterfaceTest::RunTest(const FString& Parameters)
{
	using namespace LayoutTestSupport;

	/*
	 * The geometry closes: a bed face is 220.375 cm2, so the two bed joints plus the
	 * head-joint gap must account for all of it. If this fails, every area expectation in
	 * the file describes a different brick.
	 */
	TestEqual(TEXT("a bed joint is 105.0625 cm2"), BedJointAreaSqCm, 105.0625);
	TestEqual(TEXT("a head joint is 66.625 cm2"), HeadJointAreaSqCm, 66.625);
	TestEqual(TEXT("a full bed face is 220.375 cm2"), FullBedFaceAreaSqCm, 220.375);
	TestEqual(
		TEXT("two bed joints plus the head-joint gap must account for the whole bed face"),
		FullBedFaceAreaSqCm - 2.0 * BedJointAreaSqCm,
		MortarCm * BrickDepthCm);

	struct FInterfaceCase
	{
		const TCHAR* Description = nullptr;
		int32 HandleA = 0;
		FPieceBox BoxA;
		int32 HandleB = 1;
		FPieceBox BoxB;
		double JointThicknessCm = MortarCm;
		bool bExpectedAccepted = false;

		/** Exact, and only read when the case is expected to be accepted. */
		FVector ExpectedNormal = FVector::ZeroVector;
		double ExpectedAreaSqCm = 0.0;

		/**
		 * The face's own rectangle, hand-derived per row. Only read when the case is
		 * accepted. Centre is the overlap midpoint on each in-plane axis and the mid-plane
		 * of the joint on the separation axis; half-extent is half each in-plane overlap and
		 * exactly zero on the separation axis.
		 */
		FVector ExpectedCentreCm = FVector::ZeroVector;
		FVector ExpectedHalfExtentCm = FVector::ZeroVector;
	};

	const FPieceBox LowerLeft = FullBrickAt(0.0, 0);
	const FPieceBox LowerRight = FullBrickAt(BrickPitchCm, 0);
	const FPieceBox Spanning = FullBrickAt(BondOffsetCm, 1);

	/*
	 * A pier bearing wholly inside the beam it carries (a padstone, bearing plate, lintel
	 * on a wide pier) — the one shape a wall of same-sized bricks never makes. A 40 cm pier
	 * under a 220 cm beam, both 10 cm through the wall, one mortar joint apart on Z. The
	 * pier's whole top face bears, so the shared face is the pier's: 40 x 10 = 400 cm2.
	 *
	 * Two placements. The inboard pier at [160, 200] is strictly contained by the beam's
	 * [0, 220]; the flush pier slid out to [180, 220] sits exactly where containment begins
	 * (|d| = 90 = |eA - eB|). Same 400 cm2 either way, and the flush row shows the fix for
	 * the inboard one moves nothing that already works. Integer-cm sizes, exact in binary.
	 */
	constexpr double BeamLengthCm = 220.0;
	constexpr double BeamHeightCm = 30.0;
	constexpr double PierLengthCm = 40.0;
	constexpr double PierHeightCm = 20.0;

	/** Through the wall, shared by both, so Y overlaps fully and cannot govern the area. */
	constexpr double BearingDepthCm = 10.0;

	/** How far the inboard pier's outer face stands in from the beam's end. */
	constexpr double PierInsetCm = 20.0;

	constexpr double BeamCentreXCm = BeamLengthCm / 2.0;
	constexpr double InboardPierCentreXCm = BeamLengthCm - PierInsetCm - PierLengthCm / 2.0;
	constexpr double FlushPierCentreXCm = BeamLengthCm - PierLengthCm / 2.0;

	/*
	 * The beam sits one mortar joint above the pier's top face, so the joint plane is the
	 * mid-plane of that mortar, the same rule as the brick rows on a different fixture.
	 */
	constexpr double BeamCentreZCm = PierHeightCm + MortarCm + BeamHeightCm / 2.0;
	constexpr double BearingPlaneZCm = PierHeightCm + MortarCm / 2.0;

	/** The pier is wholly under the beam, so the shared face is the pier's own top face. */
	constexpr double BearingAreaSqCm = PierLengthCm * BearingDepthCm;

	const FPieceBox Beam = BoxOfSize(
		FVector(BeamCentreXCm, 0.0, BeamCentreZCm),
		FVector(BeamLengthCm, BearingDepthCm, BeamHeightCm));

	const FPieceBox InboardPier = BoxOfSize(
		FVector(InboardPierCentreXCm, 0.0, PierHeightCm / 2.0),
		FVector(PierLengthCm, BearingDepthCm, PierHeightCm));

	const FPieceBox FlushPier = BoxOfSize(
		FVector(FlushPierCentreXCm, 0.0, PierHeightCm / 2.0),
		FVector(PierLengthCm, BearingDepthCm, PierHeightCm));

	/*
	 * Fixture preconditions for the bearing rows: pure arithmetic on the test's own boxes,
	 * asserting this geometry can tell the two ways of measuring an overlap apart. Reach
	 * minus distance (eA + eB - |d|) equals the true intersection min(highs) - max(lows)
	 * only while |d| >= |eA - eB|; below that it over-reports by the shortfall. The inboard
	 * pier is strictly inside that boundary and the flush pier sits exactly on it. The half
	 * bat cannot do this job: it has |d| = 5.625 = |eA - eB| exactly, right where the two
	 * arithmetics agree.
	 */
	const double InboardCentreGapCm = FMath::Abs(InboardPierCentreXCm - BeamCentreXCm);
	const double FlushCentreGapCm = FMath::Abs(FlushPierCentreXCm - BeamCentreXCm);
	const double BearingExtentDifferenceCm = (BeamLengthCm - PierLengthCm) / 2.0;

	TestTrue(
		FString::Printf(
			TEXT("fixture precondition: the inboard pier must bear WHOLLY inside the beam, so |d| =")
			TEXT(" %.6f must be strictly less than |eA - eB| = %.6f"),
			InboardCentreGapCm, BearingExtentDifferenceCm),
		InboardCentreGapCm < BearingExtentDifferenceCm);

	TestTrue(
		FString::Printf(
			TEXT("fixture precondition: the flush pier must sit EXACTLY on the switchover, |d| =")
			TEXT(" %.17g against |eA - eB| = %.17g"),
			FlushCentreGapCm, BearingExtentDifferenceCm),
		FlushCentreGapCm == BearingExtentDifferenceCm);

	/*
	 * The two arithmetics, asserted to disagree by exactly the shortfall. Reach minus
	 * distance gives 110 + 20 - 70 = 60 cm where only the pier's 40 cm bears: 20 cm too
	 * much, 10 cm hanging off each end.
	 */
	const double InboardReachMinusDistanceCm =
		BeamLengthCm / 2.0 + PierLengthCm / 2.0 - InboardCentreGapCm;

	TestEqual(
		TEXT("fixture precondition: reach minus distance over-reports the inboard bearing by the")
		TEXT(" shortfall, |eA - eB| - |d|"),
		InboardReachMinusDistanceCm - PierLengthCm,
		BearingExtentDifferenceCm - InboardCentreGapCm);

	TestTrue(
		FString::Printf(
			TEXT("fixture precondition: this geometry must tell the two apart — reach minus distance")
			TEXT(" gives %.6f cm where the true intersection gives %.6f cm"),
			InboardReachMinusDistanceCm, PierLengthCm),
		InboardReachMinusDistanceCm > PierLengthCm);

	TestEqual(
		TEXT("fixture precondition: the true shared bearing is the pier's own 400 cm2 face"),
		BearingAreaSqCm, 400.0);

	const TArray<FInterfaceCase> Cases = {
		/*
		 * The case the factory exists for. A brick offset half a cell and one course up.
		 * Separation axis is Z, so the normal is +Z; the centroid direction normalises to
		 * Z = 0.5547, which would make this a head joint.
		 */
		{
			TEXT("a running-bond bed joint has an axis-of-separation normal, not a centroid one"),
			0, LowerLeft, 1, Spanning, MortarCm,
			true, FVector(0.0, 0.0, 1.0), BedJointAreaSqCm,
			/*
			 * The rectangle the moment model hangs off. Lower brick X [-10.75, 10.75],
			 * spanning brick X [0.5, 22.0], so the face is X [0.5, 10.75]: 10.25 wide,
			 * centred on 5.625, full depth in Y, mid-plane of the mortar at Z = 7.0, zero
			 * half-extent on Z. The 5.625 is the eccentricity a brick sees when it loses
			 * its other bed support; the assertion after the loop pins the symmetry.
			 */
			FVector(BondOffsetCm / 2.0, 0.0, BedJointPlaneZCm),
			FVector(BedJointHalfAlongWallCm, HalfBrickDepthCm, 0.0)
		},

		/*
		 * The same joint declared upper piece first. Swap the handles and the normal must
		 * swap with them. A consistently flipped joint reports identical loads, so only
		 * the pairing can catch this.
		 */
		{
			TEXT("swapping the handles swaps the normal, and nothing else"),
			0, Spanning, 1, LowerLeft, MortarCm,
			true, FVector(0.0, 0.0, -1.0), BedJointAreaSqCm,
			/*
			 * Identical geometry. Naming the upper brick first must not move the joint: a
			 * centroid that slid to whichever brick was A would be a lever arm dependent on
			 * declaration order. Asserted bit for bit against the row above.
			 */
			FVector(BondOffsetCm / 2.0, 0.0, BedJointPlaneZCm),
			FVector(BedJointHalfAlongWallCm, HalfBrickDepthCm, 0.0)
		},

		{
			TEXT("the other bed joint of the same spanning brick"),
			0, LowerRight, 1, Spanning, MortarCm,
			true, FVector(0.0, 0.0, 1.0), BedJointAreaSqCm,
			/*
			 * The mirror of the first row. Right brick X [11.75, 33.25], so the face is
			 * X [11.75, 22.0], centred on 16.875: 5.625 the other side of the spanning
			 * brick's centre at 11.25.
			 */
			FVector(BrickPitchCm - BondOffsetCm / 2.0, 0.0, BedJointPlaneZCm),
			FVector(BedJointHalfAlongWallCm, HalfBrickDepthCm, 0.0)
		},

		/*
		 * A head joint: separated along the wall, so the normal is +X. The centroid
		 * direction is +X here too, which is why a head joint cannot discriminate between
		 * the two rules and the bed joint above is the case that matters.
		 */
		{
			TEXT("a head joint between two bricks in one course"),
			0, LowerLeft, 1, LowerRight, MortarCm,
			true, FVector(1.0, 0.0, 0.0), HeadJointAreaSqCm,
			/*
			 * The head joint's rectangle, a different shape: 10.25 deep by 6.5 tall, so the
			 * halves are 5.125 and 3.25, not 5.125 twice. A producer emitting half the
			 * brick's footprint for every joint would pass the bed rows and miss here. The
			 * face is the mortar mid-plane at X = 11.25, spanning the full depth and height.
			 */
			FVector(HeadJointPlaneXCm, 0.0, HalfBrickHeightCm),
			FVector(0.0, HalfBrickDepthCm, HalfBrickHeightCm)
		},
		{
			TEXT("the same head joint declared the other way round"),
			0, LowerRight, 1, LowerLeft, MortarCm,
			true, FVector(-1.0, 0.0, 0.0), HeadJointAreaSqCm,
			// Same face, same rectangle; only the normal turns round.
			FVector(HeadJointPlaneXCm, 0.0, HalfBrickHeightCm),
			FVector(0.0, HalfBrickDepthCm, HalfBrickHeightCm)
		},

		/*
		 * Stack bond, no offset: the full bed face. Separates the area from the bond
		 * pattern; a producer returning 220.375 for every bed joint would pass the
		 * running-bond rows, and vice versa.
		 */
		{
			TEXT("a brick directly above another shares its whole bed face"),
			0, LowerLeft, 1, FullBrickAt(0.0, 1), MortarCm,
			true, FVector(0.0, 0.0, 1.0), FullBedFaceAreaSqCm,
			/*
			 * The half-extent varies with the bond and the area cannot say so. Stack bond
			 * shares the whole 21.5 cm face, so the half-extent along the wall is 10.75, not
			 * running bond's 5.125: twice the section modulus about the same axis. Centred
			 * on the brick, since neither is offset.
			 */
			FVector(0.0, 0.0, BedJointPlaneZCm),
			FVector(HalfBrickLengthCm, HalfBrickDepthCm, 0.0)
		},

		/*
		 * A half bat on a full brick: the flush wall's mixed-size case. The half bat sits
		 * wholly within the brick below, so the overlap is its own 10.25 cm and the area is
		 * the same 105.0625 cm2. Joint areas are identical in both end treatments; the piece
		 * differs.
		 */
		{
			TEXT("a half bat on a full brick is still a 105.0625 cm2 bed joint"),
			0, LowerLeft,
			1, BoxOfSize(
				FVector(
					-BrickLengthCm * 0.5 + HalfBatLengthCm * 0.5,
					0.0,
					BrickHeightCm * 0.5 + CoursePitchCm),
				FVector(HalfBatLengthCm, BrickDepthCm, BrickHeightCm)),
			MortarCm,
			true, FVector(0.0, 0.0, 1.0), BedJointAreaSqCm,
			/*
			 * The mixed-size row, the one no shortcut satisfies. For equal boxes the overlap
			 * midpoint is just the midpoint of the centres, and every other accepted row is
			 * equal boxes on the axis that matters, so averaging the two centres would pass
			 * them all. Not here: the half bat at -5.625 and the brick at 0 average to
			 * -2.8125, while the face is the half bat's footprint X [-10.75, -0.5], centred
			 * on -5.625. Same 105.0625 area as a bed joint, so only the rectangle tells them
			 * apart: same size, different place.
			 */
			FVector(-BrickLengthCm * 0.5 + HalfBatLengthCm * 0.5, 0.0, BedJointPlaneZCm),
			FVector(BedJointHalfAlongWallCm, HalfBrickDepthCm, 0.0)
		},

		/*
		 * A pier bearing wholly inside the beam above it, the row the half bat cannot stand
		 * in for. The half bat is the boundary case, |d| = |eA - eB| exactly, where reach
		 * minus distance is still right. Move a small piece inboard of that and the larger
		 * span contains the smaller, so the true face is the smaller piece's footprint and
		 * reach minus distance over-reports it.
		 *
		 * Beam X [0, 220], pier X [160, 200]: the pier's whole top bears, so 40 cm and
		 * 400 cm2. Reach minus distance gives 60 cm and 600 cm2 on X [150, 210], 10 cm
		 * hanging off each end. It survives every other check here: the centroid is built
		 * from the interval intersection so it lands on 180 either way, and 4 x h_u x h_v ==
		 * area still holds because the half-extent and the area are over-reported together.
		 * So the area, the rectangle and the centroid must all be named: two move, one does
		 * not. BeamAcceptanceTest.cpp sidesteps this by running its piers past the beam's end.
		 */
		{
			TEXT("a pier bearing wholly inside the beam above it shares only the pier's own face"),
			0, InboardPier, 1, Beam, MortarCm,
			true, FVector(0.0, 0.0, 1.0), BearingAreaSqCm,
			FVector(InboardPierCentreXCm, 0.0, BearingPlaneZCm),
			FVector(PierLengthCm / 2.0, BearingDepthCm / 2.0, 0.0)
		},

		/*
		 * The same contained pair declared beam first, not a duplicate of the swapped rows
		 * above: those swap equal boxes, where any rule is symmetric. Containment is the one
		 * arrangement where the boxes play different parts, so a fix reaching for "the smaller
		 * extent" or branching on which box is A could get the contained row green and this
		 * one wrong — a bearing size that depended on declaration order.
		 */
		{
			TEXT("the same contained bearing declared beam first"),
			0, Beam, 1, InboardPier, MortarCm,
			true, FVector(0.0, 0.0, -1.0), BearingAreaSqCm,
			FVector(InboardPierCentreXCm, 0.0, BearingPlaneZCm),
			FVector(PierLengthCm / 2.0, BearingDepthCm / 2.0, 0.0)
		},

		/*
		 * The same pier slid out to the beam's end, exactly on the switchover; correct today
		 * and must stay so. At X [180, 220] the outer faces meet, so |d| = 90 = |eA - eB| and
		 * the two arithmetics agree bit for bit. It pins the boundary from the outside, so a
		 * fix for the row above moves nothing that works and an over-correction that clamped
		 * one step too early would be caught. Same 400 cm2: same pier, same beam.
		 */
		{
			TEXT("a pier flush with the beam's end sits exactly on the switchover and is unchanged"),
			0, FlushPier, 1, Beam, MortarCm,
			true, FVector(0.0, 0.0, 1.0), BearingAreaSqCm,
			FVector(FlushPierCentreXCm, 0.0, BearingPlaneZCm),
			FVector(PierLengthCm / 2.0, BearingDepthCm / 2.0, 0.0)
		},

		/*
		 * A dry-stacked joint, thickness zero: the faces touch. Not degenerate (dry stone is
		 * a real profile), and it separates "gap equals joint thickness" from "gap is 1 cm".
		 */
		{
			TEXT("a zero-thickness joint is faces touching, and is a real joint"),
			0, LowerLeft,
			1, BoxOfSize(
				FVector(BondOffsetCm, 0.0, BrickHeightCm * 1.5),
				FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm)),
			0.0,
			true, FVector(0.0, 0.0, 1.0), BedJointAreaSqCm,
			/*
			 * Dry stacked: the row that shows the mid-plane is not a fudge. With no mortar
			 * the faces coincide at Z = 6.5, so mid-plane and contact face are the same
			 * number. The mortared rows sit half a joint off either face; this pins that the
			 * rule degenerates onto the faces rather than being offset by a constant.
			 */
			FVector(BondOffsetCm / 2.0, 0.0, BrickHeightCm),
			FVector(BedJointHalfAlongWallCm, HalfBrickDepthCm, 0.0)
		},

		/*
		 * The spurious diagonal pair, the worst thing this factory could emit. A brick up one
		 * course and along one cell is separated by the joint thickness on both X and Z: it
		 * touches at an edge, no face. Accepting it would hand the solver a joint whose normal
		 * is one axis or the other, a coin flip between the bed and head tiers.
		 */
		{
			TEXT("a diagonal neighbour touches at an edge and is not a joint"),
			0, LowerLeft, 1, FullBrickAt(BrickPitchCm, 1), MortarCm,
			false
		},

		/*
		 * The corner: the other half of the same guard, and the half that fails open. One
		 * cell along, one through the depth and one course up, so all three gaps are the joint
		 * thickness and the boxes meet at a point. Production refuses it with the same
		 * SeparationCount != 1 line that refuses the edge, but the two jobs fail in opposite
		 * directions. Weaken that guard to "reject 0 or 2" and 3 falls through: SeparationAxis
		 * ends as 2, the Z thickness check passes, and the area is (-1.0) x (-1.0) = +1.0 cm2
		 * — two negatives into a healthy positive, normal +Z, accepted. The graph gains a
		 * phantom 1 cm2 bed joint between two bricks touching at a point, a real support under
		 * the two-tier rule.
		 *
		 * The running-bond sweep cannot catch it: every RunningBond box shares one Y, so no
		 * pair is separated on three axes and none would yield a positive area under the
		 * weakened guard. This table is the only defence. The loop's area assertion does real
		 * work here: a weakened guard fails open, so "refused" and "zero area" differ.
		 */
		{
			TEXT("two bricks touching at a corner are separated on three axes and are not a joint"),
			0, LowerLeft,
			1, BoxOfSize(
				FVector(
					BrickPitchCm,
					BrickDepthCm + MortarCm,
					BrickHeightCm * 0.5 + CoursePitchCm),
				FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm)),
			MortarCm,
			false
		},

		{
			TEXT("two courses apart is not a joint"),
			0, LowerLeft, 1, FullBrickAt(BondOffsetCm, 2), MortarCm,
			false
		},

		{
			TEXT("a gap wider than the joint is not a joint"),
			0, LowerLeft,
			1, BoxOfSize(
				FVector(BondOffsetCm, 0.0, BrickHeightCm + 0.5 * MortarCm + BrickHeightCm * 0.5),
				FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm)),
			MortarCm,
			false
		},

		{
			TEXT("faces closer than the joint thickness are not a joint either"),
			0, LowerLeft,
			1, BoxOfSize(
				FVector(BondOffsetCm, 0.0, BrickHeightCm + 0.25 * MortarCm + BrickHeightCm * 0.5),
				FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm)),
			MortarCm,
			false
		},

		{
			TEXT("interpenetrating boxes are not a joint"),
			0, LowerLeft, 1, FullBrickAt(BrickLengthCm * 0.5, 0), MortarCm,
			false
		},

		{
			TEXT("a box against itself is not a joint"),
			0, LowerLeft, 1, LowerLeft, MortarCm,
			false
		},

		/*
		 * Flush on a second axis: a line contact, an edge, not a zero-area face. Offset a full
		 * brick depth in Y so the Y faces are flush and the Y overlap is exactly 0. Production
		 * counts axes that do not overlap, !(Overlap > Tol), so a zero overlap is a separated
		 * axis: this pair is separated on two (Y at zero gap, Z at the joint thickness) and is
		 * refused by SeparationCount != 1 before any area is computed, alongside the diagonal
		 * and the corner.
		 */
		{
			TEXT("faces flush on a second axis meet along a line, not on a face"),
			0, LowerLeft,
			1, BoxOfSize(
				FVector(BondOffsetCm, BrickDepthCm, BrickHeightCm * 0.5 + CoursePitchCm),
				FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm)),
			MortarCm,
			false
		},

		{
			TEXT("a NaN centre is not a joint"),
			0, LowerLeft,
			1, BoxOfSize(
				FVector(MakeNaN(), 0.0, BrickHeightCm * 0.5 + CoursePitchCm),
				FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm)),
			MortarCm,
			false
		},

		{
			TEXT("a NaN extent is not a joint"),
			0, LowerLeft,
			1, BoxOfSize(
				FVector(BondOffsetCm, 0.0, BrickHeightCm * 0.5 + CoursePitchCm),
				FVector(MakeNaN(), BrickDepthCm, BrickHeightCm)),
			MortarCm,
			false
		},

		/*
		 * An infinite extent, the same guard's other hole. IsUsableBox checks IsFinite on the
		 * centre but only !(Extent > 0.0) on the extent, and +inf > 0.0 is true, so it walks
		 * through the half written to fail closed. It cannot be caught downstream either: an
		 * infinite extent overlaps infinitely, so that axis is never the separation axis. Here
		 * Y is infinite while X overlaps by 10.25 and Z is separated by the joint thickness, so
		 * the pair looks like an ordinary bed joint and is accepted with area 10.25 x inf =
		 * +inf — a fail-open refusal. AddConnection would reject the non-finite area, so today
		 * this is a wrong return value rather than a corrupt graph, but the contract is that a
		 * refusal leaves zero area. Real +inf, not DBL_MAX: see MakeInfinity.
		 */
		{
			TEXT("an infinite extent is not a joint"),
			0, LowerLeft,
			1, BoxOfSize(
				FVector(BondOffsetCm, 0.0, BrickHeightCm * 0.5 + CoursePitchCm),
				FVector(BrickLengthCm, MakeInfinity(), BrickHeightCm)),
			MortarCm,
			false
		},

		{
			TEXT("a zero-sized box is not a joint"),
			0, LowerLeft,
			1, BoxOfSize(
				FVector(BondOffsetCm, 0.0, BrickHeightCm * 0.5 + CoursePitchCm),
				FVector::ZeroVector),
			MortarCm,
			false
		},

		{
			TEXT("a negative-sized box is not a joint"),
			0, LowerLeft,
			1, BoxOfSize(
				FVector(BondOffsetCm, 0.0, BrickHeightCm * 0.5 + CoursePitchCm),
				FVector(-BrickLengthCm, BrickDepthCm, BrickHeightCm)),
			MortarCm,
			false
		},

		{
			TEXT("a NaN joint thickness is not a joint"),
			0, LowerLeft, 1, Spanning, MakeNaN(),
			false
		},

		{
			TEXT("a negative joint thickness is not a joint"),
			0, LowerLeft, 1, Spanning, -MortarCm,
			false
		},

		/*
		 * Handle validation. AddConnection rejects these too, but a factory that produced them
		 * would move the failure away from its cause and emit a joint with meaningless pairing
		 * and perfect-looking geometry.
		 */
		{
			TEXT("a joint from a piece to itself is not a joint"),
			3, LowerLeft, 3, Spanning, MortarCm,
			false
		},
		{
			TEXT("an INDEX_NONE handle is not a joint"),
			INDEX_NONE, LowerLeft, 1, Spanning, MortarCm,
			false
		},
		{
			TEXT("a negative handle is not a joint"),
			0, LowerLeft, -7, Spanning, MortarCm,
			false
		},
	};

	for (const FInterfaceCase& Case : Cases)
	{
		/*
		 * Pre-filled with a bogus area on purpose. A refusal must zero the out connection, not
		 * leave it alone: zero area routes a degenerate joint through ComputeUtilisation's area
		 * guard, so a caller that ignores the return value is not handed a healthy-looking joint.
		 */
		FConnection Produced;
		Produced.InterfaceAreaSqCm = 999.0;
		Produced.InterfaceNormal = FVector(0.0, 1.0, 0.0);

		/*
		 * And a bogus rectangle, for the same reason. Zero extents mean "no bending capacity
		 * known", a healthy state, so leaving these would hand back a joint claiming a section
		 * modulus it never measured. A refusal must zero the geometry as well as the area.
		 */
		Produced.InterfaceCentreCm = FVector(111.0, 222.0, 333.0);
		Produced.InterfaceHalfExtentCm = FVector(7.0, 8.0, 9.0);

		const bool bAccepted = MakeInterface(
			Case.HandleA, Case.BoxA,
			Case.HandleB, Case.BoxB,
			Case.JointThicknessCm,
			GeneralPurposeMortar,
			Produced);

		TestEqual(
			FString::Printf(
				TEXT("%s: MakeInterface should return %s. A %s, B %s, joint %.4f cm"),
				Case.Description,
				Case.bExpectedAccepted ? TEXT("true") : TEXT("false"),
				*Describe(Case.BoxA), *Describe(Case.BoxB), Case.JointThicknessCm),
			bAccepted,
			Case.bExpectedAccepted);

		if (!Case.bExpectedAccepted)
		{
			TestTrue(
				FString::Printf(
					TEXT("%s: a refused joint must fail closed with a zero interface area, got %.6f"),
					Case.Description, Produced.InterfaceAreaSqCm),
				Produced.InterfaceAreaSqCm == 0.0);

			TestTrue(
				FString::Printf(
					TEXT("%s: a refused joint must fail closed with NO rectangle either, got centre")
					TEXT(" (%.4f, %.4f, %.4f) half-extent (%.4f, %.4f, %.4f)"),
					Case.Description,
					Produced.InterfaceCentreCm.X,
					Produced.InterfaceCentreCm.Y,
					Produced.InterfaceCentreCm.Z,
					Produced.InterfaceHalfExtentCm.X,
					Produced.InterfaceHalfExtentCm.Y,
					Produced.InterfaceHalfExtentCm.Z),
				Produced.InterfaceCentreCm.IsZero() && Produced.InterfaceHalfExtentCm.IsZero());

			continue;
		}

		if (!bAccepted)
		{
			continue;
		}

		/*
		 * The normal is compared exactly, the area to 1e-12. Every dimension is an integer
		 * quarter-cm and exact in binary, the normal is a unit axis vector however derived,
		 * and the producer is generative. A disagreement at these tolerances is real, not
		 * floating-point noise.
		 */
		TestEqual(
			FString::Printf(TEXT("%s: PieceA should be %d"), Case.Description, Case.HandleA),
			Produced.PieceA, Case.HandleA);
		TestEqual(
			FString::Printf(TEXT("%s: PieceB should be %d"), Case.Description, Case.HandleB),
			Produced.PieceB, Case.HandleB);

		TestTrue(
			FString::Printf(
				TEXT("%s: normal should be exactly (%.1f, %.1f, %.1f), got (%.10g, %.10g, %.10g)"),
				Case.Description,
				Case.ExpectedNormal.X, Case.ExpectedNormal.Y, Case.ExpectedNormal.Z,
				Produced.InterfaceNormal.X, Produced.InterfaceNormal.Y, Produced.InterfaceNormal.Z),
			Produced.InterfaceNormal == Case.ExpectedNormal);

		TestTrue(
			FString::Printf(
				TEXT("%s: interface area should be %.6f cm2, got %.6f"),
				Case.Description, Case.ExpectedAreaSqCm, Produced.InterfaceAreaSqCm),
			FMath::IsNearlyEqual(Produced.InterfaceAreaSqCm, Case.ExpectedAreaSqCm, 1.0e-12));

		TestTrue(
			FString::Printf(
				TEXT("%s: the strength profile should be carried through unchanged, %.6f MPa")
				TEXT(" compressive against %.6f"),
				Case.Description,
				Produced.Strength.CompressiveStrengthMPa,
				GeneralPurposeMortar.CompressiveStrengthMPa),
			Produced.Strength.CompressiveStrengthMPa == GeneralPurposeMortar.CompressiveStrengthMPa);

		/*
		 * The rectangle, to the same 1e-12 as the area. A centroid is a subtraction of world
		 * positions, not a unit axis, so it is not compared with ==; but every coordinate is an
		 * integer eighth-cm, so 1e-12 is far looser than the arithmetic and far tighter than
		 * the smallest disagreement that could matter, half a mortar joint (0.5 cm).
		 */
		TestTrue(
			FString::Printf(
				TEXT("%s: interface centre should be (%.6f, %.6f, %.6f), got (%.10g, %.10g, %.10g)"),
				Case.Description,
				Case.ExpectedCentreCm.X, Case.ExpectedCentreCm.Y, Case.ExpectedCentreCm.Z,
				Produced.InterfaceCentreCm.X,
				Produced.InterfaceCentreCm.Y,
				Produced.InterfaceCentreCm.Z),
			Produced.InterfaceCentreCm.Equals(Case.ExpectedCentreCm, 1.0e-12));

		TestTrue(
			FString::Printf(
				TEXT("%s: interface half-extent should be (%.6f, %.6f, %.6f), got (%.10g, %.10g, %.10g)"),
				Case.Description,
				Case.ExpectedHalfExtentCm.X,
				Case.ExpectedHalfExtentCm.Y,
				Case.ExpectedHalfExtentCm.Z,
				Produced.InterfaceHalfExtentCm.X,
				Produced.InterfaceHalfExtentCm.Y,
				Produced.InterfaceHalfExtentCm.Z),
			Produced.InterfaceHalfExtentCm.Equals(Case.ExpectedHalfExtentCm, 1.0e-12));

		/*
		 * The face cannot poke outside either piece, asserted as a property of every accepted
		 * row and derived from the box bounds. A shared face is an intersection, so the
		 * rectangle must lie inside both boxes on both in-plane axes. This is strictly stronger
		 * than 4 x h_u x h_v == area, which cannot see an over-reported face at all because the
		 * rectangle and area are then wrong together. In-plane axes only: on the separation
		 * axis the centre is the mortar mid-plane, deliberately outside both pieces. 1e-12 for
		 * the same reason as the rectangle above.
		 */
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (Produced.InterfaceNormal[Axis] != 0.0)
			{
				continue;
			}

			const double FaceLowCm =
				Produced.InterfaceCentreCm[Axis] - Produced.InterfaceHalfExtentCm[Axis];
			const double FaceHighCm =
				Produced.InterfaceCentreCm[Axis] + Produced.InterfaceHalfExtentCm[Axis];

			const double SharedLowCm = FMath::Max(
				Case.BoxA.CentreCm[Axis] - Case.BoxA.ExtentCm[Axis],
				Case.BoxB.CentreCm[Axis] - Case.BoxB.ExtentCm[Axis]);
			const double SharedHighCm = FMath::Min(
				Case.BoxA.CentreCm[Axis] + Case.BoxA.ExtentCm[Axis],
				Case.BoxB.CentreCm[Axis] + Case.BoxB.ExtentCm[Axis]);

			TestTrue(
				FString::Printf(
					TEXT("%s: on in-plane axis %d the emitted face [%.10g, %.10g] must lie inside")
					TEXT(" BOTH pieces, whose shared span is [%.10g, %.10g] — a bearing face cannot")
					TEXT(" hang off the end of the piece bearing on it"),
					Case.Description, Axis, FaceLowCm, FaceHighCm, SharedLowCm, SharedHighCm),
				FaceLowCm >= SharedLowCm - 1.0e-12 && FaceHighCm <= SharedHighCm + 1.0e-12);
		}

		/*
		 * Zero on the normal's own axis, exactly, a different claim from the vector above: the
		 * vector says which rectangle, this says the face is a rectangle at all. A near-zero
		 * thickness would give a section modulus about an axis with no extent, and the in-plane
		 * frame is "the two axes that are not the separation axis" only if the third is nothing.
		 */
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (Produced.InterfaceNormal[Axis] == 0.0)
			{
				continue;
			}

			TestTrue(
				FString::Printf(
					TEXT("%s: the half-extent on the separation axis %d must be EXACTLY zero — a face")
					TEXT(" is a rectangle, not a box — got %.17g"),
					Case.Description, Axis, Produced.InterfaceHalfExtentCm[Axis]),
				Produced.InterfaceHalfExtentCm[Axis] == 0.0);
		}

		/*
		 * The invariant that makes the pair atomic, on the produced values and exact. The area
		 * and the rectangle must be the same face; an extent that disagrees with its area is
		 * the same failure mode as a normal that disagrees with its pairing. AddConnection
		 * enforces this at the door, but only the door can refuse — this is the only place that
		 * says the producer never builds one. Exact because scaling halves back up by four is
		 * exact in binary.
		 */
		const double RectangleAreaSqCm =
			RectangleAreaOf(Produced.InterfaceHalfExtentCm, Produced.InterfaceNormal);

		TestTrue(
			FString::Printf(
				TEXT("%s: 4 x h_u x h_v must reproduce the interface area EXACTLY, got %.17g")
				TEXT(" against %.17g"),
				Case.Description, RectangleAreaSqCm, Produced.InterfaceAreaSqCm),
			RectangleAreaSqCm == Produced.InterfaceAreaSqCm);
	}

	/*
	 * The anti-centroid assertion, stated separately because it is why the factory exists.
	 * The first claim is a fixture precondition: pure arithmetic asserting this geometry can
	 * tell the two rules apart. If the brick format made the centroid direction vertical, the
	 * second claim would stop discriminating and this would say so.
	 */
	const FVector CentroidDirection = (Spanning.CentreCm - LowerLeft.CentreCm).GetSafeNormal();

	TestTrue(
		FString::Printf(
			TEXT("fixture precondition: the centroid direction of a running-bond bed joint must NOT")
			TEXT(" read as vertical, |Z| = %.10g against cos45 = %.10g"),
			FMath::Abs(CentroidDirection.Z), LayoutTestSupport::BedJointCosine),
		FMath::Abs(CentroidDirection.Z) < LayoutTestSupport::BedJointCosine);

	FConnection BedJoint;
	const bool bBedAccepted =
		MakeInterface(0, LowerLeft, 1, Spanning, MortarCm, GeneralPurposeMortar, BedJoint);

	TestTrue(TEXT("the running-bond bed joint must be produced at all"), bBedAccepted);

	if (bBedAccepted)
	{
		TestTrue(
			FString::Printf(
				TEXT("the produced bed normal must classify as a BED joint: |Z| = %.10g must exceed")
				TEXT(" cos45 = %.10g. A centroid normal gives 0.5547 and every bed joint in the wall")
				TEXT(" becomes a head joint"),
				FMath::Abs(BedJoint.InterfaceNormal.Z), LayoutTestSupport::BedJointCosine),
			FMath::Abs(BedJoint.InterfaceNormal.Z) > LayoutTestSupport::BedJointCosine);

		TestFalse(
			FString::Printf(
				TEXT("the produced bed normal must not be the centroid direction (%.10g, %.10g, %.10g)"),
				CentroidDirection.X, CentroidDirection.Y, CentroidDirection.Z),
			BedJoint.InterfaceNormal.Equals(CentroidDirection, 1.0e-6));
	}

	/*
	 * Swapping the handles swaps the normal and moves nothing else, bit for bit because it is
	 * a symmetry, not a computation. The table pins both orderings against the same rectangle,
	 * catching a disagreement over 1e-12; this catches one at all. It forbids the plausible
	 * alternative "contact plane" — the centroid on PieceA's face — which would make every
	 * joint's lever arm depend on which brick was named first: half a mortar joint of silent
	 * asymmetry in a quantity that multiplies a force.
	 */
	{
		FConnection Forward;
		FConnection Reversed;

		const bool bForward =
			MakeInterface(0, LowerLeft, 1, Spanning, MortarCm, GeneralPurposeMortar, Forward);
		const bool bReversed =
			MakeInterface(0, Spanning, 1, LowerLeft, MortarCm, GeneralPurposeMortar, Reversed);

		TestTrue(TEXT("both orderings of the bed joint must be produced"), bForward && bReversed);

		if (bForward && bReversed)
		{
			TestTrue(
				FString::Printf(
					TEXT("the two orderings must place the joint at the same point: (%.17g, %.17g,")
					TEXT(" %.17g) against (%.17g, %.17g, %.17g)"),
					Forward.InterfaceCentreCm.X,
					Forward.InterfaceCentreCm.Y,
					Forward.InterfaceCentreCm.Z,
					Reversed.InterfaceCentreCm.X,
					Reversed.InterfaceCentreCm.Y,
					Reversed.InterfaceCentreCm.Z),
				Forward.InterfaceCentreCm == Reversed.InterfaceCentreCm);

			TestTrue(
				TEXT("the two orderings must give the joint the same rectangle"),
				Forward.InterfaceHalfExtentCm == Reversed.InterfaceHalfExtentCm);

			TestTrue(
				TEXT("the two orderings must differ in the normal, and only in the normal"),
				Forward.InterfaceNormal == -Reversed.InterfaceNormal);
		}
	}

	/*
	 * The eccentricity a brick actually sees, cross-checked from the emitted centroids. A
	 * running-bond brick rests on two bed patches; the centroids must straddle it
	 * symmetrically, 5.625 cm each way, because that symmetry makes an intact wall's
	 * eccentricity exactly zero. 5.624 and 5.626 would give every bed joint a small spurious
	 * moment. And 5.625 is what a brick sees when it loses one patch (the corbelled case,
	 * roughly 200x change in governing stress).
	 */
	{
		FConnection LeftPatch;
		FConnection RightPatch;

		const bool bLeft =
			MakeInterface(0, LowerLeft, 1, Spanning, MortarCm, GeneralPurposeMortar, LeftPatch);
		const bool bRight =
			MakeInterface(0, LowerRight, 1, Spanning, MortarCm, GeneralPurposeMortar, RightPatch);

		TestTrue(TEXT("both bed patches under the spanning brick must be produced"), bLeft && bRight);

		if (bLeft && bRight)
		{
			const double LeftOffsetCm = LeftPatch.InterfaceCentreCm.X - Spanning.CentreCm.X;
			const double RightOffsetCm = RightPatch.InterfaceCentreCm.X - Spanning.CentreCm.X;

			TestTrue(
				FString::Printf(
					TEXT("the spanning brick's two bed patches must sit %.6f cm either side of it,")
					TEXT(" got %.10g and %.10g"),
					BondOffsetCm / 2.0, LeftOffsetCm, RightOffsetCm),
				FMath::IsNearlyEqual(LeftOffsetCm, -BondOffsetCm / 2.0, 1.0e-12)
					&& FMath::IsNearlyEqual(RightOffsetCm, BondOffsetCm / 2.0, 1.0e-12));

			TestTrue(
				FString::Printf(
					TEXT("an intact running bond must be eccentricity-free EXACTLY: the two patch")
					TEXT(" centroids must average to the brick's own centre, got %.17g against %.17g"),
					(LeftPatch.InterfaceCentreCm.X + RightPatch.InterfaceCentreCm.X) * 0.5,
					Spanning.CentreCm.X),
				(LeftPatch.InterfaceCentreCm.X + RightPatch.InterfaceCentreCm.X) * 0.5
					== Spanning.CentreCm.X);
		}
	}

	/*
	 * Sliding a pier along under a beam it is entirely beneath cannot change how much of it
	 * bears, asserted between two produced joints and needing no expected value: the same pier
	 * and beam moved 20 cm along must give the same area and rectangle size, with the face
	 * simply travelling with it. Bit for bit, because it is a translation: both faces are the
	 * pier's own footprint over integer-cm coordinates.
	 */
	{
		FConnection InboardBearing;
		FConnection FlushBearing;

		const bool bInboard =
			MakeInterface(0, InboardPier, 1, Beam, MortarCm, GeneralPurposeMortar, InboardBearing);
		const bool bFlush =
			MakeInterface(0, FlushPier, 1, Beam, MortarCm, GeneralPurposeMortar, FlushBearing);

		TestTrue(TEXT("both bearings of the pier under the beam must be produced"), bInboard && bFlush);

		if (bInboard && bFlush)
		{
			TestTrue(
				FString::Printf(
					TEXT("the same pier under the same beam must bear over the same area wherever it")
					TEXT(" stands, got %.17g cm2 inboard against %.17g cm2 flush"),
					InboardBearing.InterfaceAreaSqCm, FlushBearing.InterfaceAreaSqCm),
				InboardBearing.InterfaceAreaSqCm == FlushBearing.InterfaceAreaSqCm);

			TestTrue(
				FString::Printf(
					TEXT("and over the same shape, got half-extents (%.10g, %.10g, %.10g) against")
					TEXT(" (%.10g, %.10g, %.10g)"),
					InboardBearing.InterfaceHalfExtentCm.X,
					InboardBearing.InterfaceHalfExtentCm.Y,
					InboardBearing.InterfaceHalfExtentCm.Z,
					FlushBearing.InterfaceHalfExtentCm.X,
					FlushBearing.InterfaceHalfExtentCm.Y,
					FlushBearing.InterfaceHalfExtentCm.Z),
				InboardBearing.InterfaceHalfExtentCm == FlushBearing.InterfaceHalfExtentCm);

			TestTrue(
				FString::Printf(
					TEXT("and the face must travel with the pier: the two centroids must be %.6f cm")
					TEXT(" apart, got %.10g"),
					PierInsetCm,
					FlushBearing.InterfaceCentreCm.X - InboardBearing.InterfaceCentreCm.X),
				FMath::IsNearlyEqual(
					FlushBearing.InterfaceCentreCm.X - InboardBearing.InterfaceCentreCm.X,
					PierInsetCm,
					1.0e-12));
		}
	}

	return true;
}

/**
 * What a piece weighs, derived once. LayBrick turns a box and a density into a mass, and
 * the brick actor needs the same number; a second derivation is a second place to drift,
 * which this project has paid for twice. So the arithmetic moves into PieceMassKg and
 * LayBrick becomes a caller.
 *
 * Exact equality, and the multiplication order is part of the spec. Density first,
 * 1.9 x 21.5 x 10.25 x 6.5 / 1000, is exactly 2.72163125; volume first is one ulp low at
 * 2.7216312499999997. LayBrick multiplies density first, so PieceMassKg must too or the
 * refactor moves every full brick by an ulp. The full-brick row is the only one that can
 * tell the orders apart, with a precondition asserting it still can.
 *
 * Fail closed for a mass is not zero. AddPiece deliberately accepts a mass of zero, so
 * returning zero for a degenerate box would launder it into a weightless piece that routes
 * load and never breaks — fail-open at the only consumer. The refusal is a value the door
 * already turns away: AddPiece guards !(MassKg >= 0.0) || !IsFinite, so a NaN is caught by
 * the !(x >= 0.0) idiom (every comparison against NaN is false). The rows assert the
 * property, not the spelling.
 *
 * World-free: arithmetic over a box and a scalar.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLayoutPieceMassTest,
	"DestructionGame.Core.Layout.PieceMass",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLayoutPieceMassTest::RunTest(const FString& Parameters)
{
	using namespace LayoutTestSupport;

	/*
	 * The two densities the expectations are built on, pinned before anything reads them. A
	 * retune of either would make the whole table describe a different material while failing
	 * for what looks like an arithmetic reason.
	 */
	TestTrue(
		FString::Printf(
			TEXT("fixture precondition: the expectations are derived from clay at 1.9 g/cm3, profile says %.17g"),
			ClayBrick.DensityGramsPerCubicCm),
		ClayBrick.DensityGramsPerCubicCm == 1.9);

	TestTrue(
		FString::Printf(
			TEXT("fixture precondition: the expectations are derived from concrete at 2.4 g/cm3, profile says %.17g"),
			StructuralConcrete.DensityGramsPerCubicCm),
		StructuralConcrete.DensityGramsPerCubicCm == 2.4);

	/*
	 * The order really is observable, stated on the test's own literals before any
	 * expectation depends on it. If a future brick format made the two orders agree, the
	 * full-brick row would stop pinning the order and this says so.
	 */
	{
		const double DensityFirst = 1.9 * 21.5 * 10.25 * 6.5 / 1000.0;
		const double VolumeFirst = (21.5 * 10.25 * 6.5) * 1.9 / 1000.0;

		TestTrue(
			FString::Printf(
				TEXT("fixture precondition: density-first must be exactly 2.72163125, got %.17g"),
				DensityFirst),
			DensityFirst == 2.72163125);

		TestTrue(
			FString::Printf(
				TEXT("fixture precondition: volume-first must differ, so the brick row can tell the two")
				TEXT(" orders apart — got %.17g against %.17g"),
				VolumeFirst, DensityFirst),
			VolumeFirst != DensityFirst);
	}

	struct FMassCase
	{
		const TCHAR* Description = nullptr;

		/** Full size, cm, because materials and bricks are quoted full size. */
		FVector FullSizeCm = FVector::ZeroVector;

		double DensityGramsPerCubicCm = 0.0;

		bool bExpectedUsable = false;

		/** Hand-derived, compared exactly. Only read when the case is usable. */
		double ExpectedMassKg = 0.0;

		/** Off-origin and asymmetric: a mass that depended on position would read wrong. */
		FVector CentreCm = FVector(37.5, -12.25, 8.75);
	};

	const TArray<FMassCase> Cases = {
		/*
		 * The row the refactor turns on. 215 x 102.5 x 65 mm is 1432.4375 cm3, at 1.9 g/cm3
		 * that is 2721.63125 g. The only row whose two multiplication orders disagree, so it
		 * pins density-first.
		 */
		{
			TEXT("a UK metric brick in clay"),
			FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm),
			ClayBrick.DensityGramsPerCubicCm,
			true, 2.72163125
		},

		/*
		 * The half bat, and why mass comes from the box not the spec: 682.90625 cm3, so
		 * 1.297521875 kg, not a full brick's 2.72163125. Not half a brick: it is a brick less
		 * a mortar joint, halved, so 10.25 cm rather than 10.75.
		 */
		{
			TEXT("a half bat in clay"),
			FVector(HalfBatLengthCm, BrickDepthCm, BrickHeightCm),
			ClayBrick.DensityGramsPerCubicCm,
			true, 1.297521875
		},

		/*
		 * A cubic metre of concrete at 2400 kg, checkable against a table without arithmetic
		 * and catching a factor of 1000 either way. 100 x 100 x 100 cm is 1e6 cm3.
		 */
		{
			TEXT("a cubic metre of structural concrete"),
			FVector(100.0, 100.0, 100.0),
			StructuralConcrete.DensityGramsPerCubicCm,
			true, 2400.0
		},

		/* A 3 x 3 m floor slab 200 mm thick: 1.8e6 cm3, 4320 kg. */
		{
			TEXT("a 3 x 3 m concrete slab, 200 mm thick"),
			FVector(300.0, 300.0, 20.0),
			StructuralConcrete.DensityGramsPerCubicCm,
			true, 4320.0
		},

		/*
		 * One cubic centimetre, where the g-to-kg conversion shows on its own: 1.9 g is
		 * 0.0019 kg, and a missing /1000 would report a 1 cm cube weighing nearly two kilos.
		 */
		{
			TEXT("one cubic centimetre of clay is 1.9 grams"),
			FVector(1.0, 1.0, 1.0),
			ClayBrick.DensityGramsPerCubicCm,
			true, 0.0019
		},

		/*
		 * Three different dimensions, one sub-centimetre, so a function that cubed an axis or
		 * halved the wrong one lands nowhere near. 12.5 x 0.5 x 40 is 250 cm3, at 2.4 is 0.6 kg.
		 */
		{
			TEXT("a thin concrete plate, no two axes alike"),
			FVector(12.5, 0.5, 40.0),
			StructuralConcrete.DensityGramsPerCubicCm,
			true, 0.6
		},

		/*
		 * Degenerate boxes. Each must come back as something AddPiece refuses, which zero is
		 * not — see the note on this test.
		 */
		{
			TEXT("a box with no thickness is not a piece"),
			FVector(BrickLengthCm, BrickDepthCm, 0.0),
			ClayBrick.DensityGramsPerCubicCm,
			false
		},
		{
			TEXT("a zero-sized box is not a piece"),
			FVector::ZeroVector,
			ClayBrick.DensityGramsPerCubicCm,
			false
		},
		{
			TEXT("a negative dimension is not a piece"),
			FVector(-BrickLengthCm, BrickDepthCm, BrickHeightCm),
			ClayBrick.DensityGramsPerCubicCm,
			false
		},
		{
			/*
			 * Two negative axes is the one that fails open. The signs cancel, so a guard that
			 * only checked the sign of the result would return a plausible 2.72163125 kg for a
			 * box that is inside out.
			 */
			TEXT("two negative dimensions multiply into a plausible mass and are still not a piece"),
			FVector(-BrickLengthCm, -BrickDepthCm, BrickHeightCm),
			ClayBrick.DensityGramsPerCubicCm,
			false
		},
		{
			TEXT("a NaN dimension is not a piece"),
			FVector(BrickLengthCm, MakeNaN(), BrickHeightCm),
			ClayBrick.DensityGramsPerCubicCm,
			false
		},
		{
			TEXT("an infinite dimension is not a piece"),
			FVector(MakeInfinity(), BrickDepthCm, BrickHeightCm),
			ClayBrick.DensityGramsPerCubicCm,
			false
		},
		{
			/*
			 * Finite and still not a piece. DBL_MAX passes an IsFinite check and only overflows
			 * once multiplied out, so it is refused by a different route from the +inf row —
			 * a live hole in MakeInterface recorded in CURRENT_STATE.md. What must not happen is
			 * a finite plausible answer.
			 */
			TEXT("a DBL_MAX dimension is finite and still not a piece"),
			FVector(TNumericLimits<double>::Max(), BrickDepthCm, BrickHeightCm),
			ClayBrick.DensityGramsPerCubicCm,
			false
		},
		{
			/*
			 * A box nobody can place is not a piece either, though the mass does not depend on
			 * the centre. There is one notion of a usable box, IsUsableBox; a mass function with
			 * a narrower opinion would be a second, weaker definition.
			 */
			TEXT("a box at a NaN position is not a piece"),
			FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm),
			ClayBrick.DensityGramsPerCubicCm,
			false, 0.0,
			FVector(MakeNaN(), 0.0, 0.0)
		},

		/*
		 * Degenerate densities. RunningBond refuses all four in its spec, so a mass function
		 * that accepted them would be the more permissive of the two.
		 */
		{
			TEXT("a weightless material is not a material"),
			FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm),
			0.0,
			false
		},
		{
			TEXT("a negative density is not a material"),
			FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm),
			-1.9,
			false
		},
		{
			TEXT("a NaN density is not a material"),
			FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm),
			MakeNaN(),
			false
		},
		{
			TEXT("an infinite density is not a material"),
			FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm),
			MakeInfinity(),
			false
		},
	};

	for (const FMassCase& Case : Cases)
	{
		const FPieceBox Box = BoxOfSize(Case.CentreCm, Case.FullSizeCm);

		const double MassKg = PieceMassKg(Box, Case.DensityGramsPerCubicCm);

		/*
		 * The composed property, asserted both directions: whatever comes back must be usable
		 * at the one door it is handed to. A usable box must produce a mass the structure
		 * accepts, a degenerate one a mass it turns away — which is what makes "fail closed"
		 * mean something here.
		 */
		FStructure Door;
		const int32 Handle = Door.AddPiece(MassKg, /*bIsGrounded*/ false);

		TestTrue(
			FString::Printf(
				TEXT("%s: FStructure::AddPiece should %s a mass of %.17g"),
				Case.Description,
				Case.bExpectedUsable ? TEXT("accept") : TEXT("REFUSE"),
				MassKg),
			(Handle != INDEX_NONE) == Case.bExpectedUsable);

		if (!Case.bExpectedUsable)
		{
			/*
			 * And not zero, the trap. Zero passes AddPiece's guard by design, so a 0 here
			 * means the assertion above already failed. Stated separately to show zero was
			 * considered and rejected as the fail-closed value, not merely not chosen.
			 */
			TestTrue(
				FString::Printf(
					TEXT("%s: a degenerate box must not come back as a plausible mass, got %.17g")
					TEXT(" (zero is NOT fail-closed: AddPiece accepts a massless piece)"),
					Case.Description, MassKg),
				!(MassKg >= 0.0) || !FMath::IsFinite(MassKg));

			continue;
		}

		/*
		 * Exact, not IsNearlyEqual: every dimension and density is exact in binary and the
		 * expectations land exactly on the density-first product. A tolerance would accept the
		 * volume-first order, which is the one thing this assertion is for.
		 */
		TestTrue(
			FString::Printf(
				TEXT("%s: %g x %g x %g cm at %g g/cm3 should weigh EXACTLY %.17g kg, got %.17g"),
				Case.Description,
				Case.FullSizeCm.X, Case.FullSizeCm.Y, Case.FullSizeCm.Z,
				Case.DensityGramsPerCubicCm,
				Case.ExpectedMassKg, MassKg),
			MassKg == Case.ExpectedMassKg);
	}

	/*
	 * The brick's weight, anchoring the number the suite quotes to the mass. 2.72163125 kg x
	 * 980 cm/s2 is 2667.198625 uu, exact: the 1 N = 100 uu conversion is baked into the 980,
	 * and applying it again is out by 100x.
	 */
	{
		const double BrickFromTheBox = PieceMassKg(
			BoxOfSize(FVector::ZeroVector, FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm)),
			ClayBrick.DensityGramsPerCubicCm);

		TestTrue(
			FString::Printf(
				TEXT("a clay brick should weigh exactly 2667.198625 uu, got %.17g"),
				WeightOf(BrickFromTheBox)),
			WeightOf(BrickFromTheBox) == 2667.198625);
	}

	/*
	 * The refactor's safety net, written before the refactor. Moving the arithmetic out must
	 * change no wall, so this asserts bit-for-bit agreement against the masses RunningBond
	 * already produces, on both end treatments. Agreement is the required behaviour: an ulp
	 * here is an ulp of movement in every full brick in the suite.
	 */
	{
		const TArray<EWallEnd> Ends = { EWallEnd::Ragged, EWallEnd::Flush };

		int32 PiecesCompared = 0;
		int32 DistinctMasses = 0;
		TArray<double> MassesSeen;

		for (const EWallEnd End : Ends)
		{
			FRunningBondSpec Spec;
			Spec.BrickSizeCm = FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm);
			Spec.JointThicknessCm = MortarCm;
			Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
			Spec.CoursesHigh = 3;
			Spec.BricksPerCourse = 3;
			Spec.End = End;
			Spec.Strength = GeneralPurposeMortar;

			FBrickLayout Layout;

			const bool bLaid = RunningBond(Spec, Layout);

			TestTrue(
				FString::Printf(TEXT("the %s cross-check wall should be laid"),
					End == EWallEnd::Ragged ? TEXT("ragged") : TEXT("flush")),
				bLaid);

			if (!bLaid || Layout.Boxes.Num() != Layout.Structure.NumPieces())
			{
				continue;
			}

			for (int32 Piece = 0; Piece < Layout.Boxes.Num(); ++Piece)
			{
				const double FromLayBrick = Layout.Structure.GetPiece(Piece).MassKg;
				const double FromTheFunction =
					PieceMassKg(Layout.Boxes[Piece], Spec.DensityGramsPerCubicCm);

				TestTrue(
					FString::Printf(
						TEXT("%s wall piece %d: PieceMassKg must reproduce LayBrick BIT FOR BIT, got")
						TEXT(" %.17g against %.17g"),
						End == EWallEnd::Ragged ? TEXT("ragged") : TEXT("flush"),
						Piece, FromTheFunction, FromLayBrick),
					FromTheFunction == FromLayBrick);

				++PiecesCompared;

				if (!MassesSeen.Contains(FromLayBrick))
				{
					MassesSeen.Add(FromLayBrick);
					++DistinctMasses;
				}
			}
		}

		/*
		 * Floor the cross-check, so a producer that laid nothing does not pass in silence on an
		 * empty array. Two 3 x 3 walls: ragged is 3 + 2 + 3 = 8 pieces, flush 3 + 4 + 3 = 10.
		 */
		TestEqual(
			FString::Printf(TEXT("the cross-check should span 18 pieces, compared %d"), PiecesCompared),
			PiecesCompared, 18);

		/*
		 * And both piece sizes must appear, or the cross-check is 18 copies of one comparison
		 * and a half bat weighing a full brick would sail through.
		 */
		TestEqual(
			FString::Printf(
				TEXT("the cross-check should see two distinct masses, a full brick and a half bat, saw %d"),
				DistinctMasses),
			DistinctMasses, 2);
	}

	return true;
}

/**
 * Three boxes, one spanning brick: the smallest arrangement whose graph carries a load, and
 * the smallest where a centroid normal is catastrophic. Two grounded bricks and a third
 * spanning them: two bed joints of 105.0625 cm2 and one head joint of 66.625 cm2, and the
 * spanning brick's 2667.198625 uu splits evenly over the two bed joints, 1333.5993125 uu
 * each, in pure compression at 1.2693390e-4 of mortar's capacity. The head joint carries
 * nothing, because both pieces it joins are grounded.
 *
 * Compression must be the governing axis: ComputeUtilisation returns the worst of three, so
 * an expectation aimed at one axis can measure another. Here the force and the normal are
 * both vertical, so shear and tension are exactly zero and only compression can govern; the
 * assertions check all three loads so that stays visible.
 *
 * Under a centroid normal the bed joints classify as head joints, gravity resolves as shear
 * against 0.2 MPa instead of compression against 10 MPa, and the utilisation lands 41.5x
 * higher with nothing broken or moved. The normal (0.8320503, 0, 0.5547002) splits the same
 * 1333.5993125 uu into 739.7478 compression and 1109.6217 shear over 105.0625 cm2: shear
 * stress 1.056154e-3 MPa against a Mohr-Coulomb capacity of 0.2 + 0.6 x 7.041026e-4 =
 * 0.20042246 MPa, so 5.269638e-3 against the correct 1.269339e-4, a factor of 41.5.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLayoutSpanningBrickTest,
	"DestructionGame.Core.Layout.SpanningBrick",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLayoutSpanningBrickTest::RunTest(const FString& Parameters)
{
	using namespace LayoutTestSupport;

	/*
	 * The derivation ran. BrickMassKg is computed at namespace scope from ClayBrick's density;
	 * if static-init order made it read zero, every force expectation would become zero and
	 * the test would pass asserting nothing. Not a second anchor on what a brick weighs.
	 */
	TestTrue(
		FString::Printf(TEXT("the brick mass must derive to something positive, got %.10g kg"), BrickMassKg),
		BrickMassKg > 0.0);

	/*
	 * The hand-derived numbers, pinned before anything can return early, so the whole chain
	 * (1432.4375 cm3 at 1.9 g/cm3, x 980, halved over two bed joints of 105.0625 cm2, against
	 * mortar's 10 MPa) is anchored to something written out. A precondition that runs only
	 * when the fixture already worked is not a precondition.
	 */
	const double ShareUU = BrickWeightUU / 2.0;
	const double BedUtilisation =
		MPaForForce(ShareUU, BedJointAreaSqCm) / GeneralPurposeMortar.CompressiveStrengthMPa;

	TestTrue(
		FString::Printf(
			TEXT("fixture precondition: one brick on two bed joints derives to 1333.5993125 uu each")
			TEXT(" and 1.2693390e-4 utilisation, got %.10g uu and %.10g"),
			ShareUU, BedUtilisation),
		FMath::IsNearlyEqual(ShareUU, 1333.5993125, 1.0e-9)
			&& FMath::IsNearlyEqual(BedUtilisation / 1.2693390e-4, 1.0, 1.0e-6));

	const FPieceBox LeftBox = FullBrickAt(0.0, 0);
	const FPieceBox RightBox = FullBrickAt(BrickPitchCm, 0);
	const FPieceBox SpanBox = FullBrickAt(BondOffsetCm, 1);

	FStructure Structure;
	const int32 Left = Structure.AddPiece(BrickMassKg, /*bIsGrounded*/ true);
	const int32 Right = Structure.AddPiece(BrickMassKg, /*bIsGrounded*/ true);
	const int32 Span = Structure.AddPiece(BrickMassKg, /*bIsGrounded*/ false);

	struct FSpanJoint
	{
		const TCHAR* Description = nullptr;
		int32 HandleA = INDEX_NONE;
		FPieceBox BoxA;
		int32 HandleB = INDEX_NONE;
		FPieceBox BoxB;
		FVector ExpectedNormal = FVector::ZeroVector;
		double ExpectedAreaSqCm = 0.0;

		/*
		 * Signed Z of the force the solver must store. Per ConnectionLoad.h the force belongs
		 * to piece B, so a joint that names the loaded piece second carries it downward.
		 */
		double ExpectedForceZUU = 0.0;
		bool bExpectedBedJoint = false;
	};

	const TArray<FSpanJoint> Joints = {
		{
			TEXT("bed joint, left support"),
			Left, LeftBox, Span, SpanBox,
			FVector(0.0, 0.0, 1.0), BedJointAreaSqCm, -ShareUU, true
		},
		{
			TEXT("bed joint, right support"),
			Right, RightBox, Span, SpanBox,
			FVector(0.0, 0.0, 1.0), BedJointAreaSqCm, -ShareUU, true
		},
		{
			TEXT("head joint between the two grounded bricks"),
			Left, LeftBox, Right, RightBox,
			FVector(1.0, 0.0, 0.0), HeadJointAreaSqCm, 0.0, false
		},
	};

	TArray<int32> Handles;
	for (const FSpanJoint& Expected : Joints)
	{
		FConnection Produced;
		Produced.InterfaceAreaSqCm = 999.0;

		const bool bAccepted = MakeInterface(
			Expected.HandleA, Expected.BoxA,
			Expected.HandleB, Expected.BoxB,
			MortarCm,
			GeneralPurposeMortar,
			Produced);

		TestTrue(
			FString::Printf(TEXT("%s: MakeInterface should accept it"), Expected.Description),
			bAccepted);

		if (!bAccepted)
		{
			Handles.Add(INDEX_NONE);
			continue;
		}

		TestTrue(
			FString::Printf(
				TEXT("%s: normal should be exactly (%.1f, %.1f, %.1f), got (%.10g, %.10g, %.10g)"),
				Expected.Description,
				Expected.ExpectedNormal.X, Expected.ExpectedNormal.Y, Expected.ExpectedNormal.Z,
				Produced.InterfaceNormal.X, Produced.InterfaceNormal.Y, Produced.InterfaceNormal.Z),
			Produced.InterfaceNormal == Expected.ExpectedNormal);

		TestTrue(
			FString::Printf(
				TEXT("%s: interface area should be %.6f cm2, got %.6f"),
				Expected.Description, Expected.ExpectedAreaSqCm, Produced.InterfaceAreaSqCm),
			FMath::IsNearlyEqual(Produced.InterfaceAreaSqCm, Expected.ExpectedAreaSqCm, 1.0e-12));

		Handles.Add(Structure.AddConnection(Produced));
	}

	for (int32 Index = 0; Index < Handles.Num(); ++Index)
	{
		TestTrue(
			FString::Printf(
				TEXT("%s: the structure should accept the produced joint"),
				Joints[Index].Description),
			Handles[Index] != INDEX_NONE);
	}

	if (Handles.Contains(INDEX_NONE))
	{
		return false;
	}

	Structure.SolveLoads();

	TestTrue(TEXT("the spanning brick must be held up"), Structure.IsPieceSupported(Span));

	constexpr double Tolerance = 1.0e-6;

	for (int32 Index = 0; Index < Handles.Num(); ++Index)
	{
		const FSpanJoint& Expected = Joints[Index];
		const FConnection& Connection = Structure.GetConnection(Handles[Index]);
		const FVector Force = Structure.GetConnectionForce(Handles[Index]);

		TestTrue(
			FString::Printf(
				TEXT("%s: should carry Z = %.7f uu, got %.7f"),
				Expected.Description, Expected.ExpectedForceZUU, Force.Z),
			FMath::IsNearlyEqual(Force.Z, Expected.ExpectedForceZUU, Tolerance));

		TestTrue(
			FString::Printf(
				TEXT("%s: the load must be vertical, got (%.7f, %.7f, %.7f)"),
				Expected.Description, Force.X, Force.Y, Force.Z),
			FMath::IsNearlyZero(Force.X, Tolerance) && FMath::IsNearlyZero(Force.Y, Tolerance));

		/*
		 * ClassifyForce and ComputeUtilisation are called directly, not through ApplyForce
		 * (which DESIGN.md warns re-opens the degenerate-normal hole). Safe because no break is
		 * decided and every normal here is a real plane, and necessary because ApplyForce
		 * latches and this reads the same joint more than once.
		 */
		const FConnectionLoad Load =
			DestructionForce::ClassifyForce(Force, Connection.InterfaceNormal);

		const double Magnitude = FMath::Abs(Expected.ExpectedForceZUU);
		const double ExpectedCompression = Expected.bExpectedBedJoint ? Magnitude : 0.0;
		const double ExpectedShear = Expected.bExpectedBedJoint ? 0.0 : Magnitude;

		TestTrue(
			FString::Printf(
				TEXT("%s: should resolve to compression %.7f / shear %.7f / tension 0, got %.7f / %.7f / %.7f"),
				Expected.Description, ExpectedCompression, ExpectedShear,
				Load.Compression, Load.Shear, Load.Tension),
			FMath::IsNearlyEqual(Load.Compression, ExpectedCompression, Tolerance)
				&& FMath::IsNearlyEqual(Load.Shear, ExpectedShear, Tolerance)
				&& FMath::IsNearlyZero(Load.Tension, Tolerance));

		/*
		 * The utilisation, derived twice: from the expected load through a conversion spelled
		 * out independently of ForceUnitsPerMPaSqCm, and against a hand-written figure, so a
		 * slip in either derivation or production shows. Mortar's 10 MPa comes from the library.
		 */
		const double ExpectedUtilisation = Expected.bExpectedBedJoint
			? MPaForForce(Magnitude, Expected.ExpectedAreaSqCm)
				/ GeneralPurposeMortar.CompressiveStrengthMPa
			: 0.0;

		const double Utilisation = DestructionForce::ComputeUtilisation(
			Load, GeneralPurposeMortar, Connection.InterfaceAreaSqCm);

		TestTrue(
			FString::Printf(
				TEXT("%s: utilisation should be %.10g, got %.10g"),
				Expected.Description, ExpectedUtilisation, Utilisation),
			FMath::IsNearlyEqual(Utilisation, ExpectedUtilisation, 1.0e-12));
	}

	return true;
}

/**
 * A real wall, both end treatments, asserted on topology rather than only areas. A missed or
 * spurious contact is far worse than a wrong area: a spurious diagonal pair changes a joint's
 * tier. So the emitted pairs are checked against an independent oracle, a brute-force contact
 * scan derived differently from the generative producer, which a course-walking oracle could
 * not be. Deterministic and exhaustive rather than seeded: two ends x five heights x five
 * lengths enumerates completely and reproduces exactly, and each case names itself for lifting
 * into a regression test. Ragged uses one brick size; flush adds half bats at alternating
 * course ends, the mixed-size case. Joint areas are the same in both.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLayoutRunningBondTest,
	"DestructionGame.Core.Layout.RunningBond",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLayoutRunningBondTest::RunTest(const FString& Parameters)
{
	using namespace LayoutTestSupport;

	TestTrue(
		FString::Printf(TEXT("the brick mass must derive to something positive, got %.10g kg"), BrickMassKg),
		BrickMassKg > 0.0);

	/*
	 * Fixture precondition on the two piece sizes, re-derived. A full brick is
	 * 21.5 x 10.25 x 6.5 = 1432.4375 cm3 at 1.9 g/cm3 = 2.72163125 kg; a half bat is
	 * 682.90625 cm3 = 1.297521875 kg. The brief said 1.29777 kg for the half bat, which does
	 * not reproduce; the derived figure is used.
	 */
	TestTrue(
		FString::Printf(
			TEXT("fixture precondition: a full brick is 2.72163125 kg and a half bat 1.297521875 kg,")
			TEXT(" derived %.10g and %.10g"),
			BrickMassKg, HalfBatMassKg),
		FMath::IsNearlyEqual(BrickMassKg, 2.72163125, 1.0e-9)
			&& FMath::IsNearlyEqual(HalfBatMassKg, 1.297521875, 1.0e-9));

	const TArray<EWallEnd> Ends = { EWallEnd::Ragged, EWallEnd::Flush };

	int32 WallsChecked = 0;
	int32 JointsChecked = 0;

	for (const EWallEnd End : Ends)
	{
		for (int32 CoursesHigh = 1; CoursesHigh <= 5; ++CoursesHigh)
		{
			for (int32 BricksPerCourse = 2; BricksPerCourse <= 6; ++BricksPerCourse)
			{
				const FString Wall = FString::Printf(
					TEXT("%s wall, %d courses x %d bricks"),
					End == EWallEnd::Ragged ? TEXT("ragged") : TEXT("flush"),
					CoursesHigh, BricksPerCourse);

				FRunningBondSpec Spec;
				Spec.BrickSizeCm = FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm);
				Spec.JointThicknessCm = MortarCm;
				Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
				Spec.CoursesHigh = CoursesHigh;
				Spec.BricksPerCourse = BricksPerCourse;
				Spec.End = End;
				Spec.Strength = GeneralPurposeMortar;

				FBrickLayout Layout;
				const bool bLaid = RunningBond(Spec, Layout);

				TestTrue(FString::Printf(TEXT("%s: RunningBond should lay it"), *Wall), bLaid);
				if (!bLaid)
				{
					continue;
				}

				++WallsChecked;

				const TArray<FPieceBox>& Boxes = Layout.Boxes;
				const FStructure& Structure = Layout.Structure;

				TestEqual(
					FString::Printf(TEXT("%s: one box per piece handle"), *Wall),
					Boxes.Num(), Structure.NumPieces());

				if (Boxes.Num() != Structure.NumPieces() || Boxes.Num() == 0)
				{
					continue;
				}

				/*
				 * Courses from the boxes, not an assumed origin. Where the wall sits in space is
				 * not behaviour; how many pieces per course, and which touch, is.
				 */
				TArray<double> CourseHeights;
				for (const FPieceBox& Box : Boxes)
				{
					bool bKnown = false;
					for (const double Height : CourseHeights)
					{
						if (FMath::IsNearlyEqual(Height, Box.CentreCm.Z, 1.0e-9))
						{
							bKnown = true;
							break;
						}
					}
					if (!bKnown)
					{
						CourseHeights.Add(Box.CentreCm.Z);
					}
				}
				CourseHeights.Sort();

				TestEqual(
					FString::Printf(TEXT("%s: should be %d courses tall"), *Wall, CoursesHigh),
					CourseHeights.Num(), CoursesHigh);

				if (CourseHeights.Num() != CoursesHigh)
				{
					continue;
				}

				TArray<int32> CourseOf;
				CourseOf.SetNum(Boxes.Num());
				TArray<int32> PiecesInCourse;
				PiecesInCourse.SetNumZeroed(CoursesHigh);

				for (int32 Piece = 0; Piece < Boxes.Num(); ++Piece)
				{
					CourseOf[Piece] = CourseHeights.IndexOfByPredicate(
						[&](double Height)
						{
							return FMath::IsNearlyEqual(Height, Boxes[Piece].CentreCm.Z, 1.0e-9);
						});
					++PiecesInCourse[CourseOf[Piece]];
				}

				/*
				 * Piece counts per course. Ragged is full bricks only, so an odd course is one
				 * short; flush adds a half bat at each end of an odd course, so one longer.
				 */
				for (int32 Course = 0; Course < CoursesHigh; ++Course)
				{
					const bool bOdd = (Course % 2) == 1;
					int32 Expected = BricksPerCourse;
					if (bOdd)
					{
						Expected = End == EWallEnd::Ragged
							? BricksPerCourse - 1
							: BricksPerCourse + 1;
					}

					TestEqual(
						FString::Printf(TEXT("%s: course %d should hold %d pieces"), *Wall, Course, Expected),
						PiecesInCourse[Course], Expected);
				}

				/* Only the bottom course rests on the earth. */
				for (int32 Piece = 0; Piece < Boxes.Num(); ++Piece)
				{
					TestTrue(
						FString::Printf(
							TEXT("%s: piece %d in course %d should%s be grounded"),
							*Wall, Piece, CourseOf[Piece],
							CourseOf[Piece] == 0 ? TEXT("") : TEXT(" NOT")),
						Structure.GetPiece(Piece).bIsGrounded == (CourseOf[Piece] == 0));
				}

				/*
				 * The extreme piece centres per course, so "at a course end" is read off the
				 * boxes rather than an assumed handle ordering.
				 */
				TArray<double> LowestXInCourse;
				TArray<double> HighestXInCourse;
				LowestXInCourse.Init(TNumericLimits<double>::Max(), CoursesHigh);
				HighestXInCourse.Init(-TNumericLimits<double>::Max(), CoursesHigh);
				for (int32 Piece = 0; Piece < Boxes.Num(); ++Piece)
				{
					const int32 Course = CourseOf[Piece];
					LowestXInCourse[Course] =
						FMath::Min(LowestXInCourse[Course], Boxes[Piece].CentreCm.X);
					HighestXInCourse[Course] =
						FMath::Max(HighestXInCourse[Course], Boxes[Piece].CentreCm.X);
				}

				/*
				 * Mass from the box the producer emitted. A wall whose half bats weighed a full
				 * brick would route load correctly and report every number wrong.
				 */
				int32 FullBricks = 0;
				int32 HalfBats = 0;
				for (int32 Piece = 0; Piece < Boxes.Num(); ++Piece)
				{
					const FVector Size = Boxes[Piece].ExtentCm * 2.0;
					const double ExpectedMass =
						ClayBrick.DensityGramsPerCubicCm * Size.X * Size.Y * Size.Z / 1000.0;

					TestTrue(
						FString::Printf(
							TEXT("%s: piece %d mass should be %.10g kg from its own box, got %.10g"),
							*Wall, Piece, ExpectedMass, Structure.GetPiece(Piece).MassKg),
						FMath::IsNearlyEqual(Structure.GetPiece(Piece).MassKg, ExpectedMass, 1.0e-12));

					if (FMath::IsNearlyEqual(Size.X, BrickLengthCm, 1.0e-9))
					{
						++FullBricks;
					}
					else if (FMath::IsNearlyEqual(Size.X, HalfBatLengthCm, 1.0e-9))
					{
						++HalfBats;
					}
				}

				const int32 ExpectedHalfBats =
					End == EWallEnd::Flush ? 2 * (CoursesHigh / 2) : 0;

				TestEqual(
					FString::Printf(TEXT("%s: should contain %d half bats"), *Wall, ExpectedHalfBats),
					HalfBats, ExpectedHalfBats);

				TestEqual(
					FString::Printf(TEXT("%s: every piece should be a full brick or a half bat"), *Wall),
					FullBricks + HalfBats, Boxes.Num());

				/*
				 * The independent oracle: a brute-force contact scan over the emitted boxes
				 * versus the pairs the producer emitted. Catches a missing or spurious contact,
				 * which no count of areas can.
				 */
				const TArray<FContact> Contacts = ContactsOf(Boxes, MortarCm);

				TestEqual(
					FString::Printf(
						TEXT("%s: the producer should emit exactly the %d contacts a geometric scan finds"),
						*Wall, Contacts.Num()),
					Structure.NumConnections(), Contacts.Num());

				TArray<bool> ContactMatched;
				ContactMatched.SetNumZeroed(Contacts.Num());

				for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
				{
					const FConnection& Connection = Structure.GetConnection(Index);
					++JointsChecked;

					const int32 Lower = FMath::Min(Connection.PieceA, Connection.PieceB);
					const int32 Higher = FMath::Max(Connection.PieceA, Connection.PieceB);

					const int32 Match = Contacts.IndexOfByPredicate(
						[&](const FContact& Contact)
						{
							return Contact.LowerIndex == Lower && Contact.HigherIndex == Higher;
						});

					TestTrue(
						FString::Printf(
							TEXT("%s: joint %d joins pieces %d and %d, which a geometric scan says do not")
							TEXT(" share a face"),
							*Wall, Index, Connection.PieceA, Connection.PieceB),
						Match != INDEX_NONE);

					if (Match == INDEX_NONE)
					{
						continue;
					}

					TestFalse(
						FString::Printf(
							TEXT("%s: pieces %d and %d are joined more than once"),
							*Wall, Connection.PieceA, Connection.PieceB),
						ContactMatched[Match]);
					ContactMatched[Match] = true;

					const FContact& Contact = Contacts[Match];

					TestTrue(
						FString::Printf(
							TEXT("%s: joint %d (%d-%d) area should be %.6f cm2, got %.6f"),
							*Wall, Index, Connection.PieceA, Connection.PieceB,
							Contact.AreaSqCm, Connection.InterfaceAreaSqCm),
						FMath::IsNearlyEqual(Connection.InterfaceAreaSqCm, Contact.AreaSqCm, 1.0e-9));

					/*
					 * The normal is the separation axis, signed by which handle is B. Rebuilt from
					 * the boxes: the axis the scan found, and the sign of the centre difference on
					 * that axis only. Taking the sign from the whole difference gives the centroid
					 * normal.
					 */
					FVector ExpectedNormal = FVector::ZeroVector;
					ExpectedNormal[Contact.SeparationAxis] =
						Boxes[Connection.PieceB].CentreCm[Contact.SeparationAxis]
							> Boxes[Connection.PieceA].CentreCm[Contact.SeparationAxis]
						? 1.0
						: -1.0;

					TestTrue(
						FString::Printf(
							TEXT("%s: joint %d (%d-%d) normal should be exactly (%.1f, %.1f, %.1f),")
							TEXT(" got (%.10g, %.10g, %.10g)"),
							*Wall, Index, Connection.PieceA, Connection.PieceB,
							ExpectedNormal.X, ExpectedNormal.Y, ExpectedNormal.Z,
							Connection.InterfaceNormal.X,
							Connection.InterfaceNormal.Y,
							Connection.InterfaceNormal.Z),
						Connection.InterfaceNormal == ExpectedNormal);

					/*
					 * The rectangle, against the oracle's own. The scan intersects the [min, max]
					 * intervals; production works in centres and overlaps. Same face, two routes,
					 * which is why checking 1150 is worth more than the eight in the table above.
					 * 1e-9, matching the area beside it: the walls run to several metres and the
					 * oracle's sum-then-halve is a different association. A real disagreement is half
					 * a mortar joint, 0.5 cm.
					 */
					TestTrue(
						FString::Printf(
							TEXT("%s: joint %d (%d-%d) centre should be (%.6f, %.6f, %.6f), got")
							TEXT(" (%.10g, %.10g, %.10g)"),
							*Wall, Index, Connection.PieceA, Connection.PieceB,
							Contact.CentreCm.X, Contact.CentreCm.Y, Contact.CentreCm.Z,
							Connection.InterfaceCentreCm.X,
							Connection.InterfaceCentreCm.Y,
							Connection.InterfaceCentreCm.Z),
						Connection.InterfaceCentreCm.Equals(Contact.CentreCm, 1.0e-9));

					TestTrue(
						FString::Printf(
							TEXT("%s: joint %d (%d-%d) half-extent should be (%.6f, %.6f, %.6f), got")
							TEXT(" (%.10g, %.10g, %.10g)"),
							*Wall, Index, Connection.PieceA, Connection.PieceB,
							Contact.HalfExtentCm.X, Contact.HalfExtentCm.Y, Contact.HalfExtentCm.Z,
							Connection.InterfaceHalfExtentCm.X,
							Connection.InterfaceHalfExtentCm.Y,
							Connection.InterfaceHalfExtentCm.Z),
						Connection.InterfaceHalfExtentCm.Equals(Contact.HalfExtentCm, 1.0e-9));

					TestTrue(
						FString::Printf(
							TEXT("%s: joint %d (%d-%d) must have EXACTLY no thickness on its own")
							TEXT(" separation axis %d, got %.17g"),
							*Wall, Index, Connection.PieceA, Connection.PieceB,
							Contact.SeparationAxis,
							Connection.InterfaceHalfExtentCm[Contact.SeparationAxis]),
						Connection.InterfaceHalfExtentCm[Contact.SeparationAxis] == 0.0);

					/*
					 * And the rectangle and the area must be the same face, on all 1150. This is the
					 * invariant AddConnection refuses at the door: a violating joint would be dropped
					 * and the contact-count assertion would already fire. Both are worth having — a
					 * dropped joint says the count is wrong, this says which face is wrong.
					 */
					TestTrue(
						FString::Printf(
							TEXT("%s: joint %d (%d-%d) 4 x h_u x h_v must reproduce its area EXACTLY,")
							TEXT(" got %.17g against %.17g"),
							*Wall, Index, Connection.PieceA, Connection.PieceB,
							RectangleAreaOf(
								Connection.InterfaceHalfExtentCm, Connection.InterfaceNormal),
							Connection.InterfaceAreaSqCm),
						RectangleAreaOf(Connection.InterfaceHalfExtentCm, Connection.InterfaceNormal)
							== Connection.InterfaceAreaSqCm);

					const double ExpectedArea = Contact.SeparationAxis == 2
						? BedJointAreaSqCm
						: HeadJointAreaSqCm;

					TestTrue(
						FString::Printf(
							TEXT("%s: joint %d (%d-%d) should be one of the wall's two joint sizes,")
							TEXT(" %.6f cm2, got %.6f"),
							*Wall, Index, Connection.PieceA, Connection.PieceB,
							ExpectedArea, Connection.InterfaceAreaSqCm),
						FMath::IsNearlyEqual(Connection.InterfaceAreaSqCm, ExpectedArea, 1.0e-9));
				}

				for (int32 Index = 0; Index < Contacts.Num(); ++Index)
				{
					TestTrue(
						FString::Printf(
							TEXT("%s: pieces %d and %d share a face but the producer emitted no joint"),
							*Wall, Contacts[Index].LowerIndex, Contacts[Index].HigherIndex),
						ContactMatched[Index]);
				}

				/*
				 * The topology the brief names: every piece above the bottom course rests on two
				 * bed joints, except course-end pieces, which rest on one. Ragged puts those at
				 * both ends of every even course; flush at the two half bats of every odd course.
				 * Head joints: two with a neighbour each side, one at a course end, none in a
				 * course of one.
				 */
				int32 PiecesOnOneBedJoint = 0;

				for (int32 Piece = 0; Piece < Boxes.Num(); ++Piece)
				{
					int32 BedBeneath = 0;
					int32 HeadJoints = 0;

					for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
					{
						switch (RoleFor(Structure.GetConnection(Index), Piece))
						{
						case LayoutTestSupport::EJointRole::BedBeneath: ++BedBeneath; break;
						case LayoutTestSupport::EJointRole::Head:       ++HeadJoints; break;
						default: break;
						}
					}

					const int32 Course = CourseOf[Piece];

					if (Course == 0)
					{
						TestEqual(
							FString::Printf(
								TEXT("%s: piece %d is on the earth and rests on no bed joint"), *Wall, Piece),
							BedBeneath, 0);
					}
					else
					{
						TestTrue(
							FString::Printf(
								TEXT("%s: piece %d in course %d should rest on one or two bed joints, got %d"),
								*Wall, Piece, Course, BedBeneath),
							BedBeneath == 1 || BedBeneath == 2);

						if (BedBeneath == 1)
						{
							++PiecesOnOneBedJoint;
						}
					}

					const int32 InCourse = PiecesInCourse[Course];
					const bool bAtACourseEnd =
						FMath::IsNearlyEqual(Boxes[Piece].CentreCm.X, LowestXInCourse[Course], 1.0e-9)
						|| FMath::IsNearlyEqual(Boxes[Piece].CentreCm.X, HighestXInCourse[Course], 1.0e-9);

					const int32 ExpectedHeadJoints =
						InCourse == 1 ? 0 : (bAtACourseEnd ? 1 : 2);

					TestEqual(
						FString::Printf(
							TEXT("%s: piece %d in course %d (%d pieces) head joints"),
							*Wall, Piece, Course, InCourse),
						HeadJoints, ExpectedHeadJoints);
				}

				const int32 ExpectedOnOneBedJoint = End == EWallEnd::Ragged
					? 2 * ((CoursesHigh - 1) / 2)
					: 2 * (CoursesHigh / 2);

				TestEqual(
					FString::Printf(
						TEXT("%s: pieces resting on only one bed joint"), *Wall),
					PiecesOnOneBedJoint, ExpectedOnOneBedJoint);

				/*
				 * The outcome. Solve and check the wall stands and routes: nothing falling, every
				 * head joint carrying nothing (every brick has a bed joint beneath it), every bed
				 * joint in pure compression, and the whole weight above the bottom course arriving
				 * at the joints between courses 0 and 1.
				 */
				Layout.Structure.SolveLoads();

				double WeightAboveTheBottomCourse = 0.0;
				for (int32 Piece = 0; Piece < Boxes.Num(); ++Piece)
				{
					TestTrue(
						FString::Printf(TEXT("%s: piece %d should be held up"), *Wall, Piece),
						Layout.Structure.IsPieceSupported(Piece));

					if (CourseOf[Piece] > 0)
					{
						WeightAboveTheBottomCourse += WeightOf(Layout.Structure.GetPiece(Piece).MassKg);
					}
				}

				double ThroughTheBottomBedJoints = 0.0;

				for (int32 Index = 0; Index < Layout.Structure.NumConnections(); ++Index)
				{
					const FConnection& Connection = Layout.Structure.GetConnection(Index);
					const FVector Force = Layout.Structure.GetConnectionForce(Index);

					TestTrue(
						FString::Printf(
							TEXT("%s: joint %d force must be finite, got (%.7f, %.7f, %.7f)"),
							*Wall, Index, Force.X, Force.Y, Force.Z),
						FMath::IsFinite(Force.X) && FMath::IsFinite(Force.Y) && FMath::IsFinite(Force.Z));

					const FConnectionLoad Load =
						DestructionForce::ClassifyForce(Force, Connection.InterfaceNormal);

					const bool bBedJoint =
						FMath::Abs(Connection.InterfaceNormal.Z) > LayoutTestSupport::BedJointCosine;

					if (bBedJoint)
					{
						TestTrue(
							FString::Printf(
								TEXT("%s: bed joint %d (%d-%d) must be in pure compression, got")
								TEXT(" compression %.7f / shear %.7f / tension %.7f"),
								*Wall, Index, Connection.PieceA, Connection.PieceB,
								Load.Compression, Load.Shear, Load.Tension),
							FMath::IsNearlyZero(Load.Shear, 1.0e-6)
								&& FMath::IsNearlyZero(Load.Tension, 1.0e-6));

						const int32 LowerCourse =
							FMath::Min(CourseOf[Connection.PieceA], CourseOf[Connection.PieceB]);
						if (LowerCourse == 0)
						{
							ThroughTheBottomBedJoints += FMath::Abs(Force.Z);
						}
					}
					else
					{
						TestTrue(
							FString::Printf(
								TEXT("%s: head joint %d (%d-%d) should carry nothing — every brick above")
								TEXT(" the earth has a bed joint beneath it — got %.7f uu"),
								*Wall, Index, Connection.PieceA, Connection.PieceB, Force.Z),
							FMath::IsNearlyZero(Force.Z, 1.0e-6));
					}
				}

				TestTrue(
					FString::Printf(
						TEXT("%s: the bed joints onto the bottom course should carry the whole %.6f uu")
						TEXT(" above it, got %.6f"),
						*Wall, WeightAboveTheBottomCourse, ThroughTheBottomBedJoints),
					FMath::IsNearlyEqual(
						ThroughTheBottomBedJoints, WeightAboveTheBottomCourse, 1.0e-6));
			}
		}
	}

	/*
	 * Floor the sweep itself, so a producer that laid nothing, or a loop that stopped early,
	 * fails rather than passing in silence on an empty table.
	 */
	TestEqual(TEXT("the sweep should cover 50 walls"), WallsChecked, 50);
	TestEqual(
		FString::Printf(
			TEXT("the 50 walls hold 1150 joints between them, checked %d"), JointsChecked),
		JointsChecked, 1150);

	/*
	 * Specs that describe no wall. Rejected, writing nothing: a half-built layout is worse
	 * than none, because the pieces it did emit would look like a real structure.
	 */
	struct FRejectedSpec
	{
		const TCHAR* Description = nullptr;
		FRunningBondSpec Spec;
	};

	FRunningBondSpec Valid;
	Valid.BrickSizeCm = FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm);
	Valid.JointThicknessCm = MortarCm;
	Valid.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
	Valid.CoursesHigh = 3;
	Valid.BricksPerCourse = 4;
	Valid.End = EWallEnd::Ragged;
	Valid.Strength = GeneralPurposeMortar;

	auto With = [&Valid](auto&& Mutate)
	{
		FRunningBondSpec Copy = Valid;
		Mutate(Copy);
		return Copy;
	};

	const TArray<FRejectedSpec> Rejected = {
		{ TEXT("no courses"), With([](FRunningBondSpec& S) { S.CoursesHigh = 0; }) },
		{ TEXT("a negative course count"), With([](FRunningBondSpec& S) { S.CoursesHigh = -2; }) },

		/*
		 * A course one brick wide is not a bond. Ragged would leave every odd course empty,
		 * a stack of disconnected bricks with a gap where a course should be, and it would
		 * still solve and still look plausible.
		 */
		{ TEXT("one brick per course is not a bond"), With([](FRunningBondSpec& S) { S.BricksPerCourse = 1; }) },
		{ TEXT("no bricks per course"), With([](FRunningBondSpec& S) { S.BricksPerCourse = 0; }) },

		{ TEXT("a zero-sized brick"), With([](FRunningBondSpec& S) { S.BrickSizeCm = FVector::ZeroVector; }) },
		{ TEXT("a negative brick dimension"),
			With([](FRunningBondSpec& S) { S.BrickSizeCm.Y = -BrickDepthCm; }) },
		{ TEXT("a NaN brick dimension"),
			With([](FRunningBondSpec& S) { S.BrickSizeCm.X = MakeNaN(); }) },

		{ TEXT("a negative joint thickness"),
			With([](FRunningBondSpec& S) { S.JointThicknessCm = -MortarCm; }) },
		{ TEXT("a NaN joint thickness"),
			With([](FRunningBondSpec& S) { S.JointThicknessCm = MakeNaN(); }) },

		/*
		 * A joint thicker than half a brick makes the half bat that a flush end needs
		 * vanish or go negative, and the bond offset no longer lands inside the brick
		 * below.
		 */
		{ TEXT("a joint as long as the brick"),
			With([](FRunningBondSpec& S) { S.JointThicknessCm = BrickLengthCm; }) },

		{ TEXT("a zero density"),
			With([](FRunningBondSpec& S) { S.DensityGramsPerCubicCm = 0.0; }) },
		{ TEXT("a negative density"),
			With([](FRunningBondSpec& S) { S.DensityGramsPerCubicCm = -1.9; }) },
		{ TEXT("a NaN density"),
			With([](FRunningBondSpec& S) { S.DensityGramsPerCubicCm = MakeNaN(); }) },
	};

	for (const FRejectedSpec& Case : Rejected)
	{
		FBrickLayout Layout;
		const bool bLaid = RunningBond(Case.Spec, Layout);

		TestFalse(
			FString::Printf(TEXT("%s: RunningBond should refuse it"), Case.Description),
			bLaid);

		TestEqual(
			FString::Printf(
				TEXT("%s: a refused spec must leave no boxes behind, got %d"),
				Case.Description, Layout.Boxes.Num()),
			Layout.Boxes.Num(), 0);

		TestEqual(
			FString::Printf(
				TEXT("%s: a refused spec must leave no pieces behind, got %d"),
				Case.Description, Layout.Structure.NumPieces()),
			Layout.Structure.NumPieces(), 0);

		TestEqual(
			FString::Printf(
				TEXT("%s: a refused spec must leave no joints behind, got %d"),
				Case.Description, Layout.Structure.NumConnections()),
			Layout.Structure.NumConnections(), 0);
	}

	return true;
}

#endif
