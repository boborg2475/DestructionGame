// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/Layout.h"
#include "Core/Structure.h"
#include "Core/ConnectionLoad.h"
#include "Core/ConnectionStrength.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/** Named, not anonymous, so a unity build cannot collide these helpers with another file's. */
namespace LayoutTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * Gravity, 980 cm/s2, spelled out so the test catches a production error. MassKg * 980 is
	 * already uu; applying 1 N = 100 uu again is out by 100x.
	 */
	constexpr double GravityCmPerSecondSquared = 980.0;

	constexpr double WeightOf(double MassKg)
	{
		return MassKg * GravityCmPerSecondSquared;
	}

	/*
	 * UK metric brick, 215 x 102.5 x 65 mm with a 10 mm joint; running-bond offset is half a
	 * cell. All values are exact in binary, so areas use exact equality.
	 */
	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickDepthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;
	constexpr double MortarCm = 1.0;

	constexpr double BrickPitchCm = BrickLengthCm + MortarCm;
	constexpr double CoursePitchCm = BrickHeightCm + MortarCm;
	constexpr double BondOffsetCm = BrickPitchCm / 2.0;

	/** A half bat: makes a flush end. */
	constexpr double HalfBatLengthCm = (BrickLengthCm - MortarCm) / 2.0;

	/*
	 * A brick spans two below: two bed joints of 105.0625 cm2 (10.25 x 10.25), not one of
	 * 220.375. The remaining 10.25 cm2 is the head-joint gap.
	 */
	constexpr double BedJointOverlapCm = BrickLengthCm - BondOffsetCm;
	constexpr double BedJointAreaSqCm = BedJointOverlapCm * BrickDepthCm;
	constexpr double HeadJointAreaSqCm = BrickDepthCm * BrickHeightCm;
	constexpr double FullBedFaceAreaSqCm = BrickLengthCm * BrickDepthCm;

	// Half-extents of the same overlaps the areas multiply, so rectangle and area agree.
	constexpr double BedJointHalfAlongWallCm = BedJointOverlapCm / 2.0;
	constexpr double HalfBrickDepthCm = BrickDepthCm / 2.0;
	constexpr double HalfBrickHeightCm = BrickHeightCm / 2.0;
	constexpr double HalfBrickLengthCm = BrickLengthCm / 2.0;

	/*
	 * The joint plane is the mortar's mid-plane, so swapping handles only flips the normal.
	 * 7.0 cm between courses, 11.25 cm within one.
	 */
	constexpr double BedJointPlaneZCm = BrickHeightCm + MortarCm / 2.0;
	constexpr double HeadJointPlaneXCm = HalfBrickLengthCm + MortarCm / 2.0;

	/**
	 * 4 x h_u x h_v over the two in-plane axes named by the normal, so a stray zero extent reads
	 * as zero area. Exact in binary.
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
	 * Masses from ClayBrick's density (1.9 g/cm3): 2.72163125 kg and 1.29752188 kg. Tests
	 * guard these are positive, against static-init order.
	 */
	const double BrickMassKg =
		ClayBrick.DensityGramsPerCubicCm * BrickLengthCm * BrickDepthCm * BrickHeightCm / 1000.0;

	const double HalfBatMassKg =
		ClayBrick.DensityGramsPerCubicCm * HalfBatLengthCm * BrickDepthCm * BrickHeightCm / 1000.0;

	/** 2667.198625 uu. */
	const double BrickWeightUU = WeightOf(BrickMassKg);

	/** Production's bed/head literal, copied exactly (1/sqrt(2) computed would be an ulp off). */
	constexpr double BedJointCosine = 0.70710678118654752440;

	/**
	 * Stress in MPa. Spelled out rather than importing ForceUnitsPerMPaSqCm, so a wrong constant
	 * is caught: 1 MPa over 1 cm2 is 10000 uu.
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

	/** A real IEEE +infinity (DBL_MAX is finite and would test something else). */
	double MakeInfinity()
	{
		volatile double One = 1.0;
		volatile double Zero = 0.0;
		return One / Zero;
	}

	/** A box from its centre and full size. */
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
	 * A joint's tier for one piece, transcribed from FStructure::GetJointRole. Answers what the
	 * load numbers cannot: how many joints beneath each brick bear it.
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
	 * Independent oracle for which pairs touch: a brute-force O(n2) scan over box geometry
	 * alone. A face is separated on exactly one axis by the joint thickness and overlaps on
	 * the other two; edges and corners are not joints.
	 */
	struct FContact
	{
		int32 LowerIndex = INDEX_NONE;
		int32 HigherIndex = INDEX_NONE;

		/** 0 = X, 1 = Y, 2 = Z. */
		int32 SeparationAxis = INDEX_NONE;

		double AreaSqCm = 0.0;

		/**
		 * The face rectangle from interval intersection (a second derivation). On the separation
		 * axis the midpoint is the mortar mid-plane and the half-extent is zero.
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
 * MakeInterface: areas, normals, orientation and validation. The normal is the separation
 * axis, never the centroid direction: a bed joint's centroid normal has Z = 0.5547, below
 * cos45, so every bed joint would read as a head joint. Edge and corner contacts are refused;
 * degenerate boxes fail closed with zero area.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLayoutInterfaceTest,
	"DestructionGame.Core.Layout.Interface",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLayoutInterfaceTest::RunTest(const FString& Parameters)
{
	using namespace LayoutTestSupport;

	// If the geometry does not close, every area expectation below describes a different brick.
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

		/** Exact; read only for accepted cases. */
		FVector ExpectedNormal = FVector::ZeroVector;
		double ExpectedAreaSqCm = 0.0;

		/** Hand-derived face rectangle; half-extent zero on the separation axis. Accepted cases only. */
		FVector ExpectedCentreCm = FVector::ZeroVector;
		FVector ExpectedHalfExtentCm = FVector::ZeroVector;
	};

	const FPieceBox LowerLeft = FullBrickAt(0.0, 0);
	const FPieceBox LowerRight = FullBrickAt(BrickPitchCm, 0);
	const FPieceBox Spanning = FullBrickAt(BondOffsetCm, 1);

	/*
	 * A 40 cm pier wholly under a 220 cm beam, a shape same-sized bricks never make. Shared
	 * face 40 x 10 = 400 cm2. Inboard pier at [160, 200] is strictly contained; flush pier at
	 * [180, 220] sits exactly where containment begins.
	 */
	constexpr double BeamLengthCm = 220.0;
	constexpr double BeamHeightCm = 30.0;
	constexpr double PierLengthCm = 40.0;
	constexpr double PierHeightCm = 20.0;

	/** Shared depth, so Y cannot govern the area. */
	constexpr double BearingDepthCm = 10.0;

	constexpr double PierInsetCm = 20.0;

	constexpr double BeamCentreXCm = BeamLengthCm / 2.0;
	constexpr double InboardPierCentreXCm = BeamLengthCm - PierInsetCm - PierLengthCm / 2.0;
	constexpr double FlushPierCentreXCm = BeamLengthCm - PierLengthCm / 2.0;

	constexpr double BeamCentreZCm = PierHeightCm + MortarCm + BeamHeightCm / 2.0;
	constexpr double BearingPlaneZCm = PierHeightCm + MortarCm / 2.0;

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
	 * Preconditions: eA + eB - |d| equals the true overlap only while |d| >= |eA - eB|. The
	 * inboard pier must be strictly inside that boundary, the flush pier exactly on it.
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

	// Reach minus distance gives 110 + 20 - 70 = 60 cm where only 40 cm bears.
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
		// The key case: the centroid direction has Z = 0.5547 and would read as a head joint.
		{
			TEXT("a running-bond bed joint has an axis-of-separation normal, not a centroid one"),
			0, LowerLeft, 1, Spanning, MortarCm,
			true, FVector(0.0, 0.0, 1.0), BedJointAreaSqCm,
			// Face X [0.5, 10.75], centred 5.625: the eccentricity if the other seat is lost.
			FVector(BondOffsetCm / 2.0, 0.0, BedJointPlaneZCm),
			FVector(BedJointHalfAlongWallCm, HalfBrickDepthCm, 0.0)
		},

		// Swapped handles flip the normal; only the pairing can catch a consistent flip.
		{
			TEXT("swapping the handles swaps the normal, and nothing else"),
			0, Spanning, 1, LowerLeft, MortarCm,
			true, FVector(0.0, 0.0, -1.0), BedJointAreaSqCm,
			// Identical geometry: the centroid must not depend on declaration order.
			FVector(BondOffsetCm / 2.0, 0.0, BedJointPlaneZCm),
			FVector(BedJointHalfAlongWallCm, HalfBrickDepthCm, 0.0)
		},

		{
			TEXT("the other bed joint of the same spanning brick"),
			0, LowerRight, 1, Spanning, MortarCm,
			true, FVector(0.0, 0.0, 1.0), BedJointAreaSqCm,
			// Mirror of the first row: face centred on 16.875.
			FVector(BrickPitchCm - BondOffsetCm / 2.0, 0.0, BedJointPlaneZCm),
			FVector(BedJointHalfAlongWallCm, HalfBrickDepthCm, 0.0)
		},

		// Head joint: the centroid direction is also +X here, so it cannot tell the rules apart.
		{
			TEXT("a head joint between two bricks in one course"),
			0, LowerLeft, 1, LowerRight, MortarCm,
			true, FVector(1.0, 0.0, 0.0), HeadJointAreaSqCm,
			// A different shape: halves 5.125 and 3.25, not 5.125 twice.
			FVector(HeadJointPlaneXCm, 0.0, HalfBrickHeightCm),
			FVector(0.0, HalfBrickDepthCm, HalfBrickHeightCm)
		},
		{
			TEXT("the same head joint declared the other way round"),
			0, LowerRight, 1, LowerLeft, MortarCm,
			true, FVector(-1.0, 0.0, 0.0), HeadJointAreaSqCm,
			FVector(HeadJointPlaneXCm, 0.0, HalfBrickHeightCm),
			FVector(0.0, HalfBrickDepthCm, HalfBrickHeightCm)
		},

		// Stack bond shares the full bed face, separating area from bond pattern.
		{
			TEXT("a brick directly above another shares its whole bed face"),
			0, LowerLeft, 1, FullBrickAt(0.0, 1), MortarCm,
			true, FVector(0.0, 0.0, 1.0), FullBedFaceAreaSqCm,
			// Half-extent 10.75 along the wall, not running bond's 5.125.
			FVector(0.0, 0.0, BedJointPlaneZCm),
			FVector(HalfBrickLengthCm, HalfBrickDepthCm, 0.0)
		},

		// A half bat on a full brick: same 105.0625 cm2 as a running-bond bed joint.
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
			 * Unequal boxes: averaging centres gives -2.8125, but the face is the half bat's
			 * footprint, centred on -5.625. Only the rectangle tells it from a bed joint.
			 */
			FVector(-BrickLengthCm * 0.5 + HalfBatLengthCm * 0.5, 0.0, BedJointPlaneZCm),
			FVector(BedJointHalfAlongWallCm, HalfBrickDepthCm, 0.0)
		},

		/*
		 * Contained bearing: the true face is the pier's 400 cm2; reach minus distance says
		 * 600 cm2. Centroid lands on 180 either way and 4 x h_u x h_v == area still holds, so
		 * area, rectangle and centroid are all asserted.
		 */
		{
			TEXT("a pier bearing wholly inside the beam above it shares only the pier's own face"),
			0, InboardPier, 1, Beam, MortarCm,
			true, FVector(0.0, 0.0, 1.0), BearingAreaSqCm,
			FVector(InboardPierCentreXCm, 0.0, BearingPlaneZCm),
			FVector(PierLengthCm / 2.0, BearingDepthCm / 2.0, 0.0)
		},

		// Beam first: containment is where declaration order could change the answer.
		{
			TEXT("the same contained bearing declared beam first"),
			0, Beam, 1, InboardPier, MortarCm,
			true, FVector(0.0, 0.0, -1.0), BearingAreaSqCm,
			FVector(InboardPierCentreXCm, 0.0, BearingPlaneZCm),
			FVector(PierLengthCm / 2.0, BearingDepthCm / 2.0, 0.0)
		},

		// Flush pier, exactly on the switchover: pins the boundary from outside.
		{
			TEXT("a pier flush with the beam's end sits exactly on the switchover and is unchanged"),
			0, FlushPier, 1, Beam, MortarCm,
			true, FVector(0.0, 0.0, 1.0), BearingAreaSqCm,
			FVector(FlushPierCentreXCm, 0.0, BearingPlaneZCm),
			FVector(PierLengthCm / 2.0, BearingDepthCm / 2.0, 0.0)
		},

		// Dry stack (zero thickness) is a real joint: gap equals joint thickness, not 1 cm.
		{
			TEXT("a zero-thickness joint is faces touching, and is a real joint"),
			0, LowerLeft,
			1, BoxOfSize(
				FVector(BondOffsetCm, 0.0, BrickHeightCm * 1.5),
				FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm)),
			0.0,
			true, FVector(0.0, 0.0, 1.0), BedJointAreaSqCm,
			// The mid-plane collapses onto the touching faces at Z = 6.5.
			FVector(BondOffsetCm / 2.0, 0.0, BrickHeightCm),
			FVector(BedJointHalfAlongWallCm, HalfBrickDepthCm, 0.0)
		},

		// Edge contact: accepting it would give a coin-flip between bed and head tiers.
		{
			TEXT("a diagonal neighbour touches at an edge and is not a joint"),
			0, LowerLeft, 1, FullBrickAt(BrickPitchCm, 1), MortarCm,
			false
		},

		/*
		 * Corner contact: a guard weakened to "reject 0 or 2 axes" would accept it with area
		 * (-1) x (-1) = +1 cm2, a phantom bed joint. The running-bond sweep cannot catch this;
		 * only this row does.
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

		// Zero overlap on Y counts as separated, so this line contact is refused as an edge.
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
		 * +inf passes !(Extent > 0) and would be accepted as an ordinary bed joint with area
		 * +inf. The extent needs its own IsFinite check.
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

		// Handle validation, so the failure stays at its cause rather than in AddConnection.
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
		// Pre-filled with junk: a refusal must zero area and rectangle, not leave them.
		FConnection Produced;
		Produced.InterfaceAreaSqCm = 999.0;
		Produced.InterfaceNormal = FVector(0.0, 1.0, 0.0);

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

		// All inputs are exact in binary, so any disagreement at these tolerances is real.
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

		// 1e-12: far tighter than the smallest error that matters, half a mortar joint.
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
		 * The face must lie inside both boxes on the in-plane axes. Stronger than 4 x h_u x h_v ==
		 * area, which cannot see a face over-reported together with its area.
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

		// Exactly zero on the separation axis: a face is a rectangle, not a box.
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

		// Area and rectangle describe one face. AddConnection refuses a mismatch; this says none is built.
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
	 * The anti-centroid assertion, the reason the factory exists. The precondition checks this
	 * geometry still tells the two rules apart.
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
	 * Swapping handles flips the normal and nothing else, bit for bit. Forbids placing the
	 * centroid on PieceA's face, which would make lever arms depend on declaration order.
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
	 * A running-bond brick's two bed patches straddle it at exactly +/-5.625 cm, so an intact
	 * wall has zero eccentricity; any asymmetry gives every bed joint a spurious moment.
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

	// Sliding a contained pier 20 cm changes only where the face is, bit for bit.
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
 * PieceMassKg, the single mass derivation LayBrick and the brick actor share. Exact equality:
 * density-first gives exactly 2.72163125 for a brick, volume-first is an ulp low, so the
 * order is part of the spec.
 *
 * Fail closed is not zero: AddPiece accepts zero mass, so a degenerate box must return
 * something AddPiece refuses (NaN).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLayoutPieceMassTest,
	"DestructionGame.Core.Layout.PieceMass",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLayoutPieceMassTest::RunTest(const FString& Parameters)
{
	using namespace LayoutTestSupport;

	// Pin the densities, so a retune fails here rather than as an arithmetic mismatch.
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

	// Precondition: the two multiplication orders really differ for this brick.
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

		/** Full size, cm. */
		FVector FullSizeCm = FVector::ZeroVector;

		double DensityGramsPerCubicCm = 0.0;

		bool bExpectedUsable = false;

		/** Hand-derived, compared exactly; usable cases only. */
		double ExpectedMassKg = 0.0;

		/** Off-origin, so a position-dependent mass would show. */
		FVector CentreCm = FVector(37.5, -12.25, 8.75);
	};

	const TArray<FMassCase> Cases = {
		// 1432.4375 cm3 at 1.9 g/cm3. The only row that pins density-first order.
		{
			TEXT("a UK metric brick in clay"),
			FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm),
			ClayBrick.DensityGramsPerCubicCm,
			true, 2.72163125
		},

		// 682.90625 cm3: a brick less a mortar joint, halved (10.25 cm, not 10.75).
		{
			TEXT("a half bat in clay"),
			FVector(HalfBatLengthCm, BrickDepthCm, BrickHeightCm),
			ClayBrick.DensityGramsPerCubicCm,
			true, 1.297521875
		},

		// 1 m3 of concrete is 2400 kg; catches a factor of 1000 either way.
		{
			TEXT("a cubic metre of structural concrete"),
			FVector(100.0, 100.0, 100.0),
			StructuralConcrete.DensityGramsPerCubicCm,
			true, 2400.0
		},

		// 1.8e6 cm3, 4320 kg.
		{
			TEXT("a 3 x 3 m concrete slab, 200 mm thick"),
			FVector(300.0, 300.0, 20.0),
			StructuralConcrete.DensityGramsPerCubicCm,
			true, 4320.0
		},

		// Isolates the g-to-kg conversion: 1.9 g is 0.0019 kg.
		{
			TEXT("one cubic centimetre of clay is 1.9 grams"),
			FVector(1.0, 1.0, 1.0),
			ClayBrick.DensityGramsPerCubicCm,
			true, 0.0019
		},

		// Three unequal axes (250 cm3, 0.6 kg), so a wrong-axis error shows.
		{
			TEXT("a thin concrete plate, no two axes alike"),
			FVector(12.5, 0.5, 40.0),
			StructuralConcrete.DensityGramsPerCubicCm,
			true, 0.6
		},

		// Degenerate boxes must return something AddPiece refuses; zero is not.
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
			// The signs cancel, so a result-sign check alone would accept this.
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
			 * DBL_MAX is finite and overflows only when multiplied out (still a live hole in
			 * MakeInterface; CURRENT_STATE.md).
			 */
			TEXT("a DBL_MAX dimension is finite and still not a piece"),
			FVector(TNumericLimits<double>::Max(), BrickDepthCm, BrickHeightCm),
			ClayBrick.DensityGramsPerCubicCm,
			false
		},
		{
			// One definition of a usable box (IsUsableBox), even though mass ignores the centre.
			TEXT("a box at a NaN position is not a piece"),
			FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm),
			ClayBrick.DensityGramsPerCubicCm,
			false, 0.0,
			FVector(MakeNaN(), 0.0, 0.0)
		},

		// Degenerate densities, refused as RunningBond refuses them.
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

		// Usable boxes give a mass AddPiece accepts; degenerate ones, a mass it refuses.
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
			// Stated separately: zero is explicitly not the fail-closed value.
			TestTrue(
				FString::Printf(
					TEXT("%s: a degenerate box must not come back as a plausible mass, got %.17g")
					TEXT(" (zero is NOT fail-closed: AddPiece accepts a massless piece)"),
					Case.Description, MassKg),
				!(MassKg >= 0.0) || !FMath::IsFinite(MassKg));

			continue;
		}

		// Exact: a tolerance would accept the volume-first order.
		TestTrue(
			FString::Printf(
				TEXT("%s: %g x %g x %g cm at %g g/cm3 should weigh EXACTLY %.17g kg, got %.17g"),
				Case.Description,
				Case.FullSizeCm.X, Case.FullSizeCm.Y, Case.FullSizeCm.Z,
				Case.DensityGramsPerCubicCm,
				Case.ExpectedMassKg, MassKg),
			MassKg == Case.ExpectedMassKg);
	}

	// 2.72163125 kg x 980 = 2667.198625 uu exactly; no further 100x conversion.
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

	// PieceMassKg must match RunningBond's masses bit for bit, on both end treatments.
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

		// Guards against an empty pass: ragged 3 + 2 + 3 = 8 pieces, flush 3 + 4 + 3 = 10.
		TestEqual(
			FString::Printf(TEXT("the cross-check should span 18 pieces, compared %d"), PiecesCompared),
			PiecesCompared, 18);

		// Both piece sizes must appear, or a half bat weighing a full brick would pass.
		TestEqual(
			FString::Printf(
				TEXT("the cross-check should see two distinct masses, a full brick and a half bat, saw %d"),
				DistinctMasses),
			DistinctMasses, 2);
	}

	return true;
}

/**
 * One brick spanning two grounded ones: the smallest loaded graph. Its 2667.198625 uu splits
 * to 1333.5993125 uu per bed joint in pure compression, utilisation 1.2693390e-4; the head
 * joint between grounded bricks carries nothing. All three load axes are checked so
 * compression visibly governs.
 *
 * A centroid normal would read the bed joints as head joints in shear, 41.5x higher
 * utilisation with nothing moved.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLayoutSpanningBrickTest,
	"DestructionGame.Core.Layout.SpanningBrick",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLayoutSpanningBrickTest::RunTest(const FString& Parameters)
{
	using namespace LayoutTestSupport;

	// Static-init guard: a zero mass would make every expectation zero and vacuous.
	TestTrue(
		FString::Printf(TEXT("the brick mass must derive to something positive, got %.10g kg"), BrickMassKg),
		BrickMassKg > 0.0);

	// Hand-derived numbers pinned before any early return.
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

		// Signed Z of the stored force, which acts on piece B (ConnectionLoad.h).
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

		// Called directly, not via ApplyForce, which latches; every normal here is valid.
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

		// Expected utilisation via MPaForForce, independent of ForceUnitsPerMPaSqCm.
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
 * Real walls checked on topology against the brute-force contact oracle, since a spurious
 * contact changes a joint's tier. Exhaustive, not seeded: two ends x five heights x five
 * lengths, each case self-named. Flush adds half bats at alternating course ends.
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

	// Re-derived piece masses (the brief's 1.29777 kg half bat does not reproduce).
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

				// Courses read from the boxes, not an assumed origin.
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

				// Odd courses: ragged is one short, flush one longer (half bats at both ends).
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

				for (int32 Piece = 0; Piece < Boxes.Num(); ++Piece)
				{
					TestTrue(
						FString::Printf(
							TEXT("%s: piece %d in course %d should%s be grounded"),
							*Wall, Piece, CourseOf[Piece],
							CourseOf[Piece] == 0 ? TEXT("") : TEXT(" NOT")),
						Structure.GetPiece(Piece).bIsGrounded == (CourseOf[Piece] == 0));
				}

				// Course ends read from the boxes, not handle order.
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

				// Each piece's mass must match its own box (half bats are lighter).
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

				// The independent oracle catches missing or spurious contacts.
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

					// Separation axis, signed by B's side on that axis only.
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
					 * Rectangle against the oracle's interval-intersection route. 1e-9 allows for
					 * association differences on metre-long walls; a real error is 0.5 cm.
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

					// Rectangle and area agree; names the bad face where the count check would not.
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
				 * Pieces above course 0 rest on two bed joints, course-end pieces on one. Head
				 * joints: two mid-course, one at an end, none in a course of one.
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
				 * Solved: everything held, head joints unloaded, bed joints in pure compression,
				 * and all weight above course 0 arriving at its bed joints.
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

	// Guards against an empty or truncated sweep.
	TestEqual(TEXT("the sweep should cover 50 walls"), WallsChecked, 50);
	TestEqual(
		FString::Printf(
			TEXT("the 50 walls hold 1150 joints between them, checked %d"), JointsChecked),
		JointsChecked, 1150);

	// Invalid specs are refused and write nothing; a partial layout would look real.
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

		// Ragged would leave every odd course empty yet still solve plausibly.
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

		// Too thick a joint makes the half bat vanish or go negative.
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
