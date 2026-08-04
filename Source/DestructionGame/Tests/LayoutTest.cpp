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
 * NAMED NAMESPACE, not anonymous, for the reason recorded in CURRENT_STATE.md: an
 * anonymous namespace is private to a TRANSLATION UNIT and a unity build merges many
 * files into one, at which point two anonymous MakeNaN are the same MakeNaN and the
 * build fails on a redefinition between files that never refer to each other. The
 * using-directive goes INSIDE each RunTest body rather than at file scope, so these
 * names cannot leak into a later file in the same blob.
 */
namespace LayoutTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * Unreal's default gravity, 980 cm/s2, spelled out here rather than imported so
	 * the test fails if production gets it wrong instead of agreeing with it. Mass is
	 * already kilograms and length already centimetres, so MassKg * 980 IS a force in
	 * Unreal units — the 1 N = 100 uu conversion is baked into the 980, and applying
	 * it a second time is the standard way to be wrong by exactly 100x.
	 */
	constexpr double GravityCmPerSecondSquared = 980.0;

	constexpr double WeightOf(double MassKg)
	{
		return MassKg * GravityCmPerSecondSquared;
	}

	/*
	 * THE UK METRIC BRICK AND ITS COORDINATING GRID, re-derived here rather than
	 * quoted. 215 x 102.5 x 65 mm laid with a 10 mm joint gives a 225 x 112.5 x 75 mm
	 * coordinating cell, and the running-bond offset is half a cell along the wall.
	 *
	 * Every one of these is exact in binary floating point — all of them are integer
	 * quarters — so the areas below are exact products and the tests can compare them
	 * without a tolerance. That is not an accident of the format; it is why exact
	 * equality is the right assertion for a generative producer.
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
	 * A BRICK SPANS TWO BRICKS BELOW, so it has TWO bed joints of 105.0625 cm2 each
	 * and not one of 220.375. The overlap along the wall is 21.5 - 11.25 = 10.25 cm,
	 * the full depth is 10.25 cm, so each bed joint is 105.0625 cm2.
	 *
	 * The cross-check that the geometry closes: 220.375 - 2 x 105.0625 = 10.25 cm2,
	 * which is exactly the head-joint gap below — 1 cm of mortar across the 10.25 cm
	 * depth. Asserted in BedAndHeadJointGeometry rather than left as a comment.
	 */
	constexpr double BedJointOverlapCm = BrickLengthCm - BondOffsetCm;
	constexpr double BedJointAreaSqCm = BedJointOverlapCm * BrickDepthCm;
	constexpr double HeadJointAreaSqCm = BrickDepthCm * BrickHeightCm;
	constexpr double FullBedFaceAreaSqCm = BrickLengthCm * BrickDepthCm;

	/*
	 * Masses DERIVED FROM THE PROFILE, following StructureTest.cpp: importing density
	 * is safe because Profiles.MaterialInvariants is the single external anchor for it
	 * and everything here is downstream of that anchor, whereas a hand-set literal
	 * would be a second place to be wrong. 1432.4375 cm3 at 1.9 g/cm3 is 2.72163125 kg
	 * and 682.90625 cm3 is 1.29752188 kg.
	 *
	 * SAFE AT NAMESPACE SCOPE only because ClayBrick is an aggregate of literals and
	 * therefore constant-initialised. Make it a computed ratio and static
	 * initialisation order makes this silently zero, at which point every force
	 * expectation here becomes zero too and the file passes while asserting nothing.
	 * Each test opens with a guard against exactly that.
	 */
	const double BrickMassKg =
		ClayBrick.DensityGramsPerCubicCm * BrickLengthCm * BrickDepthCm * BrickHeightCm / 1000.0;

	const double HalfBatMassKg =
		ClayBrick.DensityGramsPerCubicCm * HalfBatLengthCm * BrickDepthCm * BrickHeightCm / 1000.0;

	/** 2667.198625 uu. */
	const double BrickWeightUU = WeightOf(BrickMassKg);

	/**
	 * THE BED / HEAD THRESHOLD, spelled as the same literal production uses.
	 *
	 * Copied verbatim from StructureTest.cpp for the reason recorded there: this is a
	 * transcription of RoleOf's tier decision, not a second opinion about it, and
	 * written 1.0 / FMath::Sqrt(2.0) it would land an ulp away from the constexpr
	 * constant in Structure.cpp. If the production constant changes, change this to
	 * match; do not re-derive it.
	 */
	constexpr double BedJointCosine = 0.70710678118654752440;

	/**
	 * Force, in Unreal units, that loads the given area to the given stress.
	 *
	 * Spelled out rather than importing ForceUnitsPerMPaSqCm, so the test fails if
	 * that constant is wrong instead of agreeing with it. 1 N = 100 uu, 1 cm2 =
	 * 100 mm2, 1 MPa = 1 N/mm2, so 1 MPa over 1 cm2 is 10000 uu.
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
	 * A REAL +INFINITY, produced the same way MakeNaN produces its NaN.
	 *
	 * It has to be the actual IEEE infinity rather than a merely enormous finite
	 * number, because the two are refused by different guards: +inf is caught by an
	 * IsFinite check on the extent, whereas TNumericLimits<double>::Max() is finite,
	 * passes any such check, and would need the AREA to be checked for overflow
	 * instead. This file asserts the first; the second is recorded as a separate hole.
	 */
	double MakeInfinity()
	{
		volatile double One = 1.0;
		volatile double Zero = 0.0;
		return One / Zero;
	}

	/** A box from its CENTRE and its FULL size, since bricks are quoted full size. */
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
	 * The tier a joint has FOR ONE PIECE, transcribed from Structure.cpp's RoleOf.
	 *
	 * A connection is described by a normal pointing toward PieceB, so to ask what it
	 * means for THIS piece the normal is first turned to point at this piece. Pointing
	 * substantially UP at it, the joint is a bed joint beneath it and bears it.
	 * Pointing substantially DOWN, the bed joint is above it and bears nothing.
	 * Anything else is a head joint.
	 *
	 * HONEST ABOUT WHAT THIS IS WORTH: it is a transcription, so it catches a
	 * transcription slip and nothing conceptual. What it is FOR here is asking a
	 * question about the produced graph that the load numbers cannot answer — how many
	 * joints beneath each brick actually bear it — which is the topology a spurious or
	 * missing contact corrupts.
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
	 * THE INDEPENDENT ORACLE for which pairs touch: a brute-force O(n2) scan over the
	 * boxes the producer emitted.
	 *
	 * This is derived differently on purpose, and that is the entire value of it.
	 * Production is GENERATIVE — it knows it is laying running bond, so it emits the
	 * pairs it laid without looking at any geometry. This looks only at geometry and
	 * knows nothing about bond patterns. An oracle that walked the courses the same way
	 * production does would agree with a wrong producer enthusiastically.
	 *
	 * The rule: two boxes form a face when they are separated on EXACTLY ONE axis, by
	 * the joint thickness, and overlap positively on the other two. Separated on two
	 * axes they meet at an edge, on three at a corner — and a corner pair emitted as a
	 * joint would be diagonal, which changes the joint's TIER and therefore where the
	 * load ends up, which is far worse than a slightly wrong area.
	 */
	struct FContact
	{
		int32 LowerIndex = INDEX_NONE;
		int32 HigherIndex = INDEX_NONE;

		/** 0 = X, 1 = Y, 2 = Z. */
		int32 SeparationAxis = INDEX_NONE;

		double AreaSqCm = 0.0;
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

				Contacts.Add({ I, J, SeparationAxis, Area });
			}
		}

		return Contacts;
	}
}

/**
 * THE INTERFACE FACTORY: areas, normals, orientation and validation, written once.
 *
 * MakeInterface is the only way to build a joint, and the reason is the normal. The
 * interface normal is the AXIS OF SEPARATION oriented by which handle is B — never
 * the direction between the two centroids. For a running-bond bed joint the centroid
 * difference is (11.25, 0, 7.5) cm, whose normalised Z is 0.5547, BELOW cos 45
 * degrees; a centroid normal therefore makes every bed joint in a wall classify as a
 * head joint, the sign-blind head tier makes every brick treat the joints above it as
 * supports, and gravity resolves as shear against 0.2 MPa of cohesion instead of
 * compression against 10 MPa. Nothing crashes and nothing collapses — the wall just
 * stands there being wrong. So the expected normals below are exact unit axis
 * vectors, and CentroidB - CentroidA is asserted against explicitly.
 *
 * Rejection is the other half. A pair separated on TWO axes touches at an edge, on
 * three at a corner, and either emitted as a joint is a spurious diagonal contact
 * that changes the joint's tier. Degenerate boxes fail closed: the out connection is
 * left with a zero interface area, which routes through ComputeUtilisation's existing
 * area guard, so a caller that ignores the return value gets a joint that reads as
 * failed rather than one that reads as fine.
 *
 * World-free: plain arithmetic over boxes. No actors, no world, no ticking solver.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLayoutInterfaceTest,
	"DestructionGame.Core.Layout.Interface",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLayoutInterfaceTest::RunTest(const FString& Parameters)
{
	using namespace LayoutTestSupport;

	/*
	 * THE GEOMETRY CLOSES. A brick's bed face is 220.375 cm2 and it rests on two
	 * bricks, so the two bed joints plus the head-joint gap below must account for all
	 * of it. If this fails every area expectation in the file is describing a
	 * different brick.
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
	};

	const FPieceBox LowerLeft = FullBrickAt(0.0, 0);
	const FPieceBox LowerRight = FullBrickAt(BrickPitchCm, 0);
	const FPieceBox Spanning = FullBrickAt(BondOffsetCm, 1);

	const TArray<FInterfaceCase> Cases = {
		/*
		 * THE CASE THE WHOLE FACTORY EXISTS FOR. A brick offset half a cell along the
		 * wall and one course up. The axis of separation is Z, so the normal is exactly
		 * +Z pointing at PieceB; the centroid direction is (11.25, 0, 7.5) normalised,
		 * whose Z is 0.5547 and which would make this a head joint.
		 */
		{
			TEXT("a running-bond bed joint has an axis-of-separation normal, not a centroid one"),
			0, LowerLeft, 1, Spanning, MortarCm,
			true, FVector(0.0, 0.0, 1.0), BedJointAreaSqCm
		},

		/*
		 * THE SAME JOINT DECLARED UPPER PIECE FIRST. The pair and the normal are one
		 * atomic value: swap the handles and the normal must swap with them, which is
		 * what makes a normal inconsistent with its pairing inexpressible. A
		 * CONSISTENTLY flipped joint reports identical loads, so only the pairing can
		 * catch this.
		 */
		{
			TEXT("swapping the handles swaps the normal, and nothing else"),
			0, Spanning, 1, LowerLeft, MortarCm,
			true, FVector(0.0, 0.0, -1.0), BedJointAreaSqCm
		},

		{
			TEXT("the other bed joint of the same spanning brick"),
			0, LowerRight, 1, Spanning, MortarCm,
			true, FVector(0.0, 0.0, 1.0), BedJointAreaSqCm
		},

		/*
		 * A head joint: separated along the wall, so the normal is +X. Note the
		 * centroid direction happens to be +X here too, which is exactly why a head
		 * joint cannot discriminate between the two rules and the bed joint above is
		 * the case that matters.
		 */
		{
			TEXT("a head joint between two bricks in one course"),
			0, LowerLeft, 1, LowerRight, MortarCm,
			true, FVector(1.0, 0.0, 0.0), HeadJointAreaSqCm
		},
		{
			TEXT("the same head joint declared the other way round"),
			0, LowerRight, 1, LowerLeft, MortarCm,
			true, FVector(-1.0, 0.0, 0.0), HeadJointAreaSqCm
		},

		/*
		 * Stack bond, no offset at all: the full bed face. Separates the area
		 * calculation from the bond pattern — a producer that returned 220.375 for
		 * every bed joint would pass every running-bond row if this were the only shape
		 * it were checked on, and vice versa.
		 */
		{
			TEXT("a brick directly above another shares its whole bed face"),
			0, LowerLeft, 1, FullBrickAt(0.0, 1), MortarCm,
			true, FVector(0.0, 0.0, 1.0), FullBedFaceAreaSqCm
		},

		/*
		 * A HALF BAT ON A FULL BRICK — the flush wall's mixed-size case. The half bat
		 * sits wholly within the brick below along the wall, so the overlap is its own
		 * 10.25 cm length and the area comes out at the SAME 105.0625 cm2. The joint
		 * areas really are identical in both end treatments; what differs is the piece.
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
			true, FVector(0.0, 0.0, 1.0), BedJointAreaSqCm
		},

		/*
		 * A DRY-STACKED joint, thickness zero: the faces touch. Not degenerate — dry
		 * stone is a real profile in the library — and it separates "the gap equals the
		 * joint thickness" from "the gap is one centimetre".
		 */
		{
			TEXT("a zero-thickness joint is faces touching, and is a real joint"),
			0, LowerLeft,
			1, BoxOfSize(
				FVector(BondOffsetCm, 0.0, BrickHeightCm * 1.5),
				FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm)),
			0.0,
			true, FVector(0.0, 0.0, 1.0), BedJointAreaSqCm
		},

		/*
		 * THE SPURIOUS DIAGONAL PAIR, and the worst thing this factory could emit. The
		 * brick up one course and along one full cell is separated by exactly the joint
		 * thickness on BOTH X and Z — it touches at an edge, and there is no face. A
		 * producer that accepted it would hand the solver a joint whose normal is one
		 * axis or the other, which is a coin flip between the bed tier and the head
		 * tier, and the tier is what decides where load ends up.
		 */
		{
			TEXT("a diagonal neighbour touches at an edge and is not a joint"),
			0, LowerLeft, 1, FullBrickAt(BrickPitchCm, 1), MortarCm,
			false
		},

		/*
		 * THE CORNER: THE OTHER HALF OF THE SAME GUARD, AND THE HALF THAT FAILS OPEN.
		 *
		 * One cell along the wall, one cell through the depth AND one course up, so all
		 * THREE gaps are exactly the joint thickness and the boxes meet at a single
		 * point. Production refuses it with the same SeparationCount != 1 line that
		 * refuses the edge above — one line doing two jobs — and only the edge job had a
		 * row until now.
		 *
		 * THE TWO JOBS FAIL IN OPPOSITE DIRECTIONS, which is why the edge row cannot
		 * stand in for this one. Weaken the guard to an explicit "reject 0 or 2", a
		 * perfectly plausible be-clear-about-what-we-reject refactor, and 3 falls
		 * through: SeparationAxis ends as 2 because the last assignment wins, the
		 * thickness check on Z passes, and the area is the product of the OTHER two
		 * overlaps — (-1.0) x (-1.0) = +1.0 cm2. Two negatives multiply into a
		 * healthy-looking positive, the normal is +Z, and AddConnection accepts it. The
		 * graph gains a phantom 1 cm2 BED joint between two bricks touching at a point,
		 * which under the two-tier rule is a real support: load routes through it and
		 * the joint that should have carried that share under-reports.
		 *
		 * THE RUNNING-BOND SWEEP CANNOT CATCH IT. Every box RunningBond emits shares one
		 * Y, so no pair in the 50-wall sweep is separated on three axes at all — 2430
		 * pairs separated on two, zero on three — and none of those 2430 would yield a
		 * positive area under the weakened guard, so they would be silently dropped and
		 * the connection count would still match the oracle. This table is the only
		 * defence.
		 *
		 * The area assertion in the loop below is doing real work on this row: a
		 * weakened guard fails OPEN, so "refused" and "left with zero area" are two
		 * different claims here rather than one restated.
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
		 * FLUSH ON A SECOND AXIS: A LINE CONTACT, AND IT IS AN EDGE, NOT A ZERO-AREA
		 * FACE. Offset a full brick depth in Y, so the Y faces are exactly flush and the
		 * Y overlap is exactly 0.
		 *
		 * The comment here used to claim this reached the area calculation and asserted
		 * that a zero-area face is refused rather than emitted with an area of zero. It
		 * does not. Production counts the axes that do NOT overlap — !(Overlap > Tol) —
		 * so an overlap of exactly zero IS a separated axis, this pair is separated on
		 * two of them (Y at zero gap, Z at the joint thickness), and it is refused by the
		 * SeparationCount != 1 branch alongside the diagonal and the corner, before any
		 * area is computed. The assertion was always right; only the reasoning was wrong,
		 * and a wrong reason is what makes somebody think a case is covered when a
		 * different one is.
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
		 * AN INFINITE EXTENT, AND THE SAME GUARD'S OTHER HOLE. IsUsableBox checks
		 * IsFinite on the CENTRE but only !(Extent > 0.0) on the extent, and +inf > 0.0
		 * is true, so an infinite extent walks straight through the half of the guard
		 * that was written to fail closed.
		 *
		 * It then cannot be caught downstream either, and that is what makes it worth a
		 * row rather than a shrug. An infinite extent overlaps infinitely, so that axis
		 * is never the axis of separation; here Y is infinite while X still overlaps by
		 * 10.25 cm and Z is separated by exactly the joint thickness, so the pair looks
		 * like a perfectly ordinary running-bond bed joint on every test that runs. The
		 * area is then 10.25 x inf = +inf and the joint is ACCEPTED with an infinite
		 * interface area — a fail-OPEN refusal, in the one place in this producer whose
		 * documented job is to fail closed.
		 *
		 * FStructure::AddConnection would reject the non-finite area, so today this is a
		 * wrong return value rather than a corrupt graph — but MakeInterface's contract
		 * is that a refusal leaves a zero area, and a caller that ignores the return
		 * value is exactly who that contract is for. Both halves of the loop below fire
		 * on this row: the accept/refuse assertion, and the zero-area assertion.
		 *
		 * Deliberately a real +inf and not TNumericLimits<double>::Max(): see MakeInfinity.
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
		 * Handle validation. FStructure::AddConnection rejects these too, but a factory
		 * that produced them would push the failure a step away from where it happened
		 * and, worse, would emit a joint whose pairing is meaningless while its geometry
		 * looks perfect.
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
		 * PRE-FILLED WITH A BOGUS AREA on purpose. A refusal has to ZERO the out
		 * connection, not merely leave it alone: zero area is what routes a degenerate
		 * joint through ComputeUtilisation's existing area guard, and a caller that
		 * ignored the return value would otherwise be handed a joint that reads as
		 * perfectly healthy.
		 */
		FConnection Produced;
		Produced.InterfaceAreaSqCm = 999.0;
		Produced.InterfaceNormal = FVector(0.0, 1.0, 0.0);

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
			continue;
		}

		if (!bAccepted)
		{
			continue;
		}

		/*
		 * THE NORMAL IS COMPARED EXACTLY, and the area to 1e-12. Every dimension in this
		 * file is an integer quarter of a centimetre and so exact in binary, the normal
		 * is a unit axis vector whatever order it is derived in, and the producer is
		 * generative rather than searching for a near miss. A disagreement at these
		 * tolerances is a real disagreement rather than floating-point noise.
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
	}

	/*
	 * THE ANTI-CENTROID ASSERTION, stated separately because it is the reason the
	 * factory exists and a reader should not have to infer it from an expected vector.
	 *
	 * The first claim is a FIXTURE PRECONDITION — pure arithmetic on the test's own
	 * boxes, asserting only that this geometry can tell the two rules apart. If the
	 * brick format ever changed so that the centroid direction happened to be
	 * substantially vertical, the second claim would stop discriminating and this would
	 * say so rather than the suite quietly losing its coverage.
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

	return true;
}

/**
 * WHAT A PIECE WEIGHS, DERIVED ONCE.
 *
 * LayBrick already turns the box it just emitted plus a density into a mass, and the
 * brick actor needs the same number for the same piece. A second derivation is a second
 * place for it to drift, which this project has already paid for twice — the half-bat
 * mass that circulated as 1.29777 kg and does not reproduce, and the break decision that
 * needed GetConnectionUtilisation written specifically to stop a third hand-copy. So the
 * arithmetic moves out into PieceMassKg and LayBrick becomes a caller.
 *
 * EXACT EQUALITY, AND THE MULTIPLICATION ORDER IS PART OF THE SPECIFICATION. Every
 * expectation below is a decimal worked out by hand from published dimensions and
 * densities, compared with ==, not with a tolerance. That is affordable because these
 * quantities really are exact in binary — but only in one order. Density first,
 * 1.9 x 21.5 x 10.25 x 6.5 / 1000, lands exactly on 2.72163125; volume first,
 * (21.5 x 10.25 x 6.5) x 1.9 / 1000, lands one ulp low at 2.7216312499999997. LayBrick
 * multiplies density first today, so PieceMassKg must too or the refactor moves every
 * full brick in every wall by an ulp. The full-brick row below is the only one in the
 * table that can tell the two orders apart, and there is a precondition asserting it
 * still can.
 *
 * FAIL CLOSED FOR A MASS IS NOT ZERO, and that is the sharp decision here.
 * FStructure::AddPiece deliberately ACCEPTS a mass of zero — "a massless piece is
 * meaningful" — so returning zero for a degenerate box would launder it into a real
 * piece that weighs nothing, sits in the graph, routes load and never breaks anything.
 * That is fail-OPEN at the only consumer there is. The refusal therefore has to be a
 * value the door already turns away: AddPiece guards !(MassKg >= 0.0) || !IsFinite, so a
 * NaN is refused by a check that is already written, needs no second guard, and is
 * caught by the !(x >= 0.0) idiom used throughout this codebase precisely because every
 * comparison against NaN is false. The rows below assert the PROPERTY — the door refuses
 * it, and it is not a plausible mass — rather than the spelling.
 *
 * World-free: arithmetic over a box and a scalar. No structure, no world, no actors.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLayoutPieceMassTest,
	"DestructionGame.Core.Layout.PieceMass",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLayoutPieceMassTest::RunTest(const FString& Parameters)
{
	using namespace LayoutTestSupport;

	/*
	 * THE TWO DENSITIES THE EXPECTATIONS ARE BUILT ON, pinned before anything reads them.
	 * Every mass below is a decimal derived by hand from one of these two numbers, so a
	 * retune of either would make the whole table describe a different material while
	 * still failing for what looks like an arithmetic reason.
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
	 * THE ORDER REALLY IS OBSERVABLE, stated as arithmetic on the test's own literals
	 * before any expectation depends on it. If a future brick format made the two orders
	 * agree, the full-brick row would silently stop pinning the order and this says so
	 * rather than the suite quietly losing that coverage.
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

		/** FULL size, cm, because materials and bricks are quoted full size. */
		FVector FullSizeCm = FVector::ZeroVector;

		double DensityGramsPerCubicCm = 0.0;

		bool bExpectedUsable = false;

		/** Hand-derived, and compared EXACTLY. Only read when the case is usable. */
		double ExpectedMassKg = 0.0;

		/**
		 * Deliberately off the origin and deliberately asymmetric: a mass that depended
		 * on where the box sits would read wrong on every accepted row.
		 */
		FVector CentreCm = FVector(37.5, -12.25, 8.75);
	};

	const TArray<FMassCase> Cases = {
		/*
		 * THE ROW THE REFACTOR TURNS ON. 215 x 102.5 x 65 mm is 1432.4375 cm3, and at
		 * 1.9 g/cm3 that is 2721.63125 g. It is also the only row here whose two
		 * multiplication orders disagree, so it is the row that pins density-first.
		 */
		{
			TEXT("a UK metric brick in clay"),
			FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm),
			ClayBrick.DensityGramsPerCubicCm,
			true, 2.72163125
		},

		/*
		 * THE HALF BAT, and the reason mass comes from the box rather than the spec: it
		 * is 682.90625 cm3, so it must weigh 1.297521875 kg and not a full brick's
		 * 2.72163125. Note this is NOT half of a full brick — the half bat is what is
		 * left when a mortar joint is taken out of a brick and the remainder halved, so
		 * it is 10.25 cm rather than 10.75.
		 */
		{
			TEXT("a half bat in clay"),
			FVector(HalfBatLengthCm, BrickDepthCm, BrickHeightCm),
			ClayBrick.DensityGramsPerCubicCm,
			true, 1.297521875
		},

		/*
		 * A CUBIC METRE OF CONCRETE AT 2400 kg, which is the figure anybody can check
		 * against a table without doing any arithmetic at all, and the one that catches a
		 * factor of 1000 in either direction. 100 x 100 x 100 cm is 1e6 cm3.
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
		 * ONE CUBIC CENTIMETRE, which is where the g-to-kg conversion is visible on its
		 * own: 1.9 grams is 0.0019 kg, and a function that forgot the /1000 would report
		 * a 1 cm cube weighing nearly two kilos.
		 */
		{
			TEXT("one cubic centimetre of clay is 1.9 grams"),
			FVector(1.0, 1.0, 1.0),
			ClayBrick.DensityGramsPerCubicCm,
			true, 0.0019
		},

		/*
		 * THREE DIFFERENT DIMENSIONS INCLUDING A SUB-CENTIMETRE ONE, so a function that
		 * cubed one axis or halved the wrong one lands nowhere near. 12.5 x 0.5 x 40 is
		 * 250 cm3, at 2.4 that is 0.6 kg.
		 */
		{
			TEXT("a thin concrete plate, no two axes alike"),
			FVector(12.5, 0.5, 40.0),
			StructuralConcrete.DensityGramsPerCubicCm,
			true, 0.6
		},

		/*
		 * DEGENERATE BOXES. Each must come back as something FStructure::AddPiece
		 * refuses, which zero is NOT — see the note on this test.
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
			 * TWO NEGATIVE AXES IS THE ONE THAT FAILS OPEN. The signs cancel in the
			 * product, so a guard that only checked the sign of the RESULT would hand
			 * back a perfectly plausible 2.72163125 kg for a box that is inside out.
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
			 * FINITE, AND STILL NOT A PIECE. DBL_MAX passes any IsFinite check on the
			 * extent and only overflows once it is multiplied out, so this row is refused
			 * by a different route from the +inf row above — the same distinction
			 * IsUsableBox draws, recorded in CURRENT_STATE.md as a live hole in
			 * MakeInterface. Either route satisfies the assertions below; what must not
			 * happen is a finite plausible answer.
			 */
			TEXT("a DBL_MAX dimension is finite and still not a piece"),
			FVector(TNumericLimits<double>::Max(), BrickDepthCm, BrickHeightCm),
			ClayBrick.DensityGramsPerCubicCm,
			false
		},
		{
			/*
			 * A BOX NOBODY CAN PLACE IS NOT A PIECE EITHER, even though the mass does not
			 * depend on the centre. There is one notion of a usable box in this producer,
			 * IsUsableBox, and MakeInterface already refuses this box; a mass function
			 * with a narrower opinion would be a second, weaker definition of the same
			 * thing.
			 */
			TEXT("a box at a NaN position is not a piece"),
			FVector(BrickLengthCm, BrickDepthCm, BrickHeightCm),
			ClayBrick.DensityGramsPerCubicCm,
			false, 0.0,
			FVector(MakeNaN(), 0.0, 0.0)
		},

		/*
		 * DEGENERATE DENSITIES. RunningBond already refuses all four in its spec, so a
		 * mass function that accepted them would be the more permissive of the two.
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
		 * THE COMPOSED PROPERTY, asserted on every row in both directions: whatever comes
		 * back must be usable at the one door it is going to be handed to. A usable box
		 * has to produce a mass the structure accepts, and a degenerate one has to produce
		 * a mass the structure turns away — which is what makes "fail closed" mean
		 * something here rather than being a claim about a particular value.
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
			 * AND NOT ZERO, WHICH IS THE TRAP. Zero passes AddPiece's guard by design, so
			 * if this ever reads 0 the assertion above has already failed — but stating it
			 * separately is what tells the next reader that zero was considered and
			 * rejected as the fail-closed value rather than merely not chosen.
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
		 * EXACT. Not IsNearlyEqual: every dimension and density in this table is exact in
		 * binary and the expectations are hand-derived decimals that land exactly on the
		 * density-first product. A tolerance here would accept the volume-first order,
		 * which is the one thing this assertion is for.
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
	 * THE BRICK'S WEIGHT, so the number the rest of the suite quotes is anchored to the
	 * mass rather than only to itself. 2.72163125 kg x 980 cm/s2 is 2667.198625 uu, and
	 * it is exact — the 1 N = 100 uu conversion is baked into the 980 and applying it
	 * again is the standard way to be wrong by exactly 100x.
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
	 * THE REFACTOR'S SAFETY NET, WRITTEN BEFORE THE REFACTOR. LayBrick derives the same
	 * number today, and the point of moving it out is that nothing about any wall may
	 * change — so this asserts BIT-FOR-BIT agreement against the masses RunningBond is
	 * already producing, on both end treatments, which is where the two piece sizes are.
	 *
	 * It is deliberately an agrees-with-production assertion, because agreement is the
	 * required behaviour rather than a convenience: an ulp of disagreement here is an ulp
	 * of movement in every full brick in every wall in the suite.
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
		 * FLOOR THE CROSS-CHECK, so a producer that laid nothing leaves this block
		 * comparing an empty array and passing in silence. Two walls of 3 courses x 3
		 * bricks: ragged is 3 + 2 + 3 = 8 pieces, flush is 3 + 4 + 3 = 10.
		 */
		TestEqual(
			FString::Printf(TEXT("the cross-check should span 18 pieces, compared %d"), PiecesCompared),
			PiecesCompared, 18);

		/*
		 * AND BOTH PIECE SIZES MUST APPEAR, or the cross-check is 18 copies of the same
		 * comparison and a half bat weighing a full brick would sail through it.
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
 * THREE BOXES, ONE SPANNING BRICK: the smallest arrangement in which the produced
 * graph carries a load, and the smallest one where a centroid normal is catastrophic.
 *
 * Two grounded bricks side by side, a third spanning them. That is two bed joints of
 * 105.0625 cm2 and one head joint of 66.625 cm2, and the spanning brick's 2667.198625
 * uu splits evenly over the two equal bed joints: 1333.5993125 uu each, resolved as
 * PURE COMPRESSION, at 1.2693390e-4 of mortar's capacity. The head joint carries
 * exactly nothing, because both pieces it joins are already on the earth.
 *
 * WHY COMPRESSION IS THE GOVERNING AXIS AND CANNOT SILENTLY BE SOMETHING ELSE:
 * ComputeUtilisation returns the WORST of the three axes, so an expectation aimed at
 * one axis can quietly be measuring another. Here shear and tension are exactly zero —
 * the force is vertical and the interface normal is exactly vertical — so their
 * utilisations are exactly zero whatever the capacities are, and compression is the
 * only axis that can govern. The assertions below check all three loads, not just the
 * ratio, so that stays visible rather than assumed.
 *
 * Under a centroid normal the two bed joints classify as head joints instead, gravity
 * resolves as shear against 0.2 MPa of cohesion rather than compression against
 * 10 MPa, and the utilisation lands 41.5x higher — with nothing broken and nothing
 * moved. Displacement could never see it; the classification can.
 *
 * THE 41.5 IS WORKED THROUGH, because the brief this test was written from said
 * "roughly 79x" and that figure does not reproduce on this case. The centroid normal
 * here is (0.8320503, 0, 0.5547002), which splits the same 1333.5993125 uu into
 * 739.7478 uu of compression and 1109.6217 uu of shear over the same 105.0625 cm2.
 * That is 7.041026e-4 MPa of compressive stress and 1.056154e-3 MPa of shear, against
 * a Mohr-Coulomb shear capacity of 0.2 + 0.6 x 7.041026e-4 = 0.20042246 MPa — so the
 * worst axis is 5.269638e-3 against the correct 1.269339e-4, a factor of 41.5. (74.8
 * is obtainable, but only as shear utilisation over compression utilisation BOTH read
 * under the wrong normal, which is not the comparison this sentence makes.)
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLayoutSpanningBrickTest,
	"DestructionGame.Core.Layout.SpanningBrick",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLayoutSpanningBrickTest::RunTest(const FString& Parameters)
{
	using namespace LayoutTestSupport;

	/*
	 * THE DERIVATION RAN. BrickMassKg is computed at namespace scope from ClayBrick's
	 * density; if static initialisation order ever made that read zero, every force
	 * expectation below would become zero as well and the test would pass while
	 * asserting nothing. Not a second anchor on what a brick weighs.
	 */
	TestTrue(
		FString::Printf(TEXT("the brick mass must derive to something positive, got %.10g kg"), BrickMassKg),
		BrickMassKg > 0.0);

	/*
	 * THE HAND-DERIVED NUMBERS, pinned BEFORE anything can return early on them, so the
	 * whole chain — 1432.4375 cm3 at 1.9 g/cm3, x 980, halved over two equal bed joints,
	 * over 105.0625 cm2, against mortar's 10 MPa — is anchored to something written out
	 * rather than only to itself. A fixture precondition that only runs when the fixture
	 * already worked is not a precondition.
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
		 * SIGNED Z of the force the solver must store. Per ConnectionLoad.h the force
		 * belonging to a connection is the force acting on PIECE B, so a joint that
		 * names the loaded piece second carries it downward.
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
		 * ClassifyForce and ComputeUtilisation are called directly here rather than
		 * through FConnection::ApplyForce, which DESIGN.md warns re-opens the
		 * degenerate-normal hole. Safe because no break decision is made and every
		 * normal in this fixture is a real plane — and necessary, because ApplyForce
		 * LATCHES and this test wants to read the same joint more than once.
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
		 * THE UTILISATION, derived twice. Once from the load the test expects, through
		 * a conversion spelled out independently of ForceUnitsPerMPaSqCm; and once
		 * against a hand-written figure, so a slip in the derivation shows up as well as
		 * a slip in production. Mortar's 10 MPa is read from the library, which
		 * Profiles.ConnectionInvariants anchors.
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
 * A REAL WALL, both end treatments, asserted on TOPOLOGY rather than only on areas.
 *
 * Topology is discrete and needs no tolerance, and a missed or spurious contact is a
 * far worse failure than a slightly wrong area: a spurious diagonal pair changes a
 * joint's TIER, and routing is what decides where load ends up. So the pairs the
 * producer emits are checked against an INDEPENDENT ORACLE — a brute-force geometric
 * contact scan over the boxes it emitted — which is derived differently from the
 * generative producer on purpose. An oracle that walked the courses the way production
 * does would agree with a wrong producer.
 *
 * DETERMINISTIC AND EXHAUSTIVE RATHER THAN SEEDED. The parameter space that matters
 * here is small and enumerable — two end treatments x five heights x five lengths — so
 * a sweep covers it completely and reproduces exactly, which a seeded generator can
 * only approximate. Every case names itself in its failure message, so one case can be
 * lifted straight out into a named regression test.
 *
 * BOTH END TREATMENTS, per the decision recorded in CURRENT_STATE.md. Ragged is the
 * simpler default and uses one brick size; FLUSH puts half bats at alternating course
 * ends and is therefore the mixed-size case — a producer that only ever emits identical
 * pieces is not being tested very hard. The joint AREAS are the same in both.
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
	 * FIXTURE PRECONDITION on the two piece sizes, re-derived rather than quoted. A
	 * full brick is 21.5 x 10.25 x 6.5 = 1432.4375 cm3 at 1.9 g/cm3 = 2.72163125 kg; a
	 * half bat is 10.25 long, so 682.90625 cm3 = 1.297521875 kg. The brief this test
	 * was written from said 1.29777 kg for the half bat, which does not reproduce —
	 * 682.90625 x 1.9 is 1297.521875 g. The derived figure is the one used.
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
				 * COURSES FROM THE BOXES, not from an assumed origin. Where the producer
				 * puts the wall in space is not behaviour; how many pieces sit in each
				 * course, and which of them touch, is.
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
				 * PIECE COUNTS PER COURSE. Ragged uses full bricks only, so an odd course
				 * is one brick short at each end; flush inserts a half bat at each end of
				 * an odd course, so it is one piece LONGER.
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
				 * The extreme piece centres in each course, so "at a course end" is read
				 * off the boxes rather than assumed from a handle ordering the producer
				 * is free to choose.
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
				 * MASS FROM THE BOX THE PRODUCER EMITTED. A wall whose half bats weighed a
				 * full brick would route load correctly and report every number wrong.
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
				 * THE INDEPENDENT ORACLE. Brute-force contact scan over the emitted boxes
				 * versus the pairs the generative producer emitted. Catches a MISSING
				 * contact and a SPURIOUS one, which no count of areas can.
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
					 * THE NORMAL IS THE AXIS OF SEPARATION, SIGNED BY WHICH HANDLE IS B.
					 * Rebuilt here from the boxes alone — the axis the scan found, and the
					 * sign of the centre difference ON THAT AXIS ONLY. Taking the sign from
					 * the whole centre difference is what produces the centroid normal.
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
				 * THE TOPOLOGY THE BRIEF NAMES: every piece above the bottom course rests
				 * on TWO bed joints, except the pieces at the ends of a course, which rest
				 * on one. Ragged puts those at both ends of every EVEN course above the
				 * first; flush puts them at the two half bats of every ODD course.
				 *
				 * Head joints: two for a piece with a neighbour on each side, one at a
				 * course end, none in a course of one.
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
				 * THE OUTCOME. Solve and check the wall actually stands and actually
				 * routes: nothing falling, every head joint carrying exactly nothing
				 * because every brick has a bed joint beneath it, every bed joint in pure
				 * compression, and the whole weight above the bottom course arriving at
				 * the joints between courses 0 and 1.
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
	 * FLOOR THE SWEEP ITSELF, so a producer that laid nothing, or a loop that stopped
	 * generating, fails rather than passing in silence with an empty table.
	 */
	TestEqual(TEXT("the sweep should cover 50 walls"), WallsChecked, 50);
	TestEqual(
		FString::Printf(
			TEXT("the 50 walls hold 1150 joints between them, checked %d"), JointsChecked),
		JointsChecked, 1150);

	/*
	 * SPECS THAT DESCRIBE NO WALL. Rejected, writing nothing — a half-built layout is
	 * worse than none, because the pieces it did emit would look like a real structure.
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
		 * A COURSE ONE BRICK WIDE IS NOT A BOND. Ragged would leave every odd course
		 * empty, so the wall would be a stack of disconnected bricks with a gap where a
		 * course should be — and it would still solve, and still look plausible.
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
