// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/BuildMode/JointInference.h"
#include "Core/Connection.h"
#include "Core/Layout.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Profiles/ConnectionProfiles.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Joint inference from material pairing and interface normal (BUILD_MODE_PLAN.md, 2026-09-03).
 * World-free; asserts the returned profile's identity.
 *
 * FConnectionStrength has no operator==, so all five fields are compared. The three possible
 * profiles differ only across the whole row:
 *   GeneralPurposeMortar         C10 / coh0.9 / T0.7 / mu0.75 / maxS2.0
 *   GeneralPurposeMortarPerpend  C10 / coh0.2 / T0.1 / mu0.75 / maxS2.0
 *   DryStone                     C30 / coh0.0 / T0.0 / mu0.70 / maxS6.0
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FJointInferenceTest,
	"DestructionGame.Core.BuildMode.JointInference",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

// Uniquely named namespace, used inside RunTest: unity builds merge files (see ConnectionLoadTest.cpp).
namespace JointInferenceTestSupport
{
	struct FInferenceCase
	{
		const TCHAR* Description;
		const DestructionProfiles::FMaterialProfile* FaceA;
		const DestructionProfiles::FMaterialProfile* FaceB;
		FVector InterfaceNormalUnit;
		const FConnectionStrength* Expected;
		const TCHAR* ExpectedName;
	};

	/*
	 * Footprints as half-extents (FPieceBox stores half-sizes). A 21.5 x 10.25 x 6.5 cm brick
	 * along X, the same brick along Y, and a square block with no long axis.
	 */
	const FVector XLongBrick(10.75, 5.125, 3.25);
	const FVector YLongBrick(5.125, 10.75, 3.25);
	const FVector SquareBlock(5.0, 5.0, 3.0);

	/*
	 * Either side of the 0.5 cm "no long axis" tolerance: 0.2 apart (square, Head) and 0.6 apart
	 * (X-long, can make a corner).
	 */
	const FVector NearSquareBlock(5.2, 5.0, 3.0);
	const FVector BarelyXLongBlock(5.6, 5.0, 3.0);

	double MakeNaN()
	{
		return std::numeric_limits<double>::quiet_NaN();
	}

	double MakeInf()
	{
		return std::numeric_limits<double>::infinity();
	}

	const TCHAR* ContactName(BuildMode::EMasonryContact Contact)
	{
		switch (Contact)
		{
		case BuildMode::EMasonryContact::Bed:
			return TEXT("Bed");
		case BuildMode::EMasonryContact::Head:
			return TEXT("Head");
		case BuildMode::EMasonryContact::Corner:
			return TEXT("Corner");
		default:
			return TEXT("<unknown>");
		}
	}

	// Full-field profile identity; FConnectionStrength has no operator==.
	void CheckProfileIdentity(
		FAutomationTestBase& Test,
		const FString& Prefix,
		const FConnectionStrength& Got,
		const FConnectionStrength& Want)
	{
		Test.TestEqual(Prefix + TEXT("CompressiveStrengthMPa"),
			Got.CompressiveStrengthMPa, Want.CompressiveStrengthMPa);
		Test.TestEqual(Prefix + TEXT("ShearCohesionMPa"),
			Got.ShearCohesionMPa, Want.ShearCohesionMPa);
		Test.TestEqual(Prefix + TEXT("TensileStrengthMPa"),
			Got.TensileStrengthMPa, Want.TensileStrengthMPa);
		Test.TestEqual(Prefix + TEXT("FrictionCoefficient"),
			Got.FrictionCoefficient, Want.FrictionCoefficient);
		Test.TestEqual(Prefix + TEXT("MaxShearStrengthMPa"),
			Got.MaxShearStrengthMPa, Want.MaxShearStrengthMPa);
	}

	/** A row of the classification table: two boxes and a normal. */
	struct FClassifyCase
	{
		const TCHAR* Description;
		DestructionLayout::FPieceBox BoxA;
		DestructionLayout::FPieceBox BoxB;
		FVector InterfaceNormalUnit;
		BuildMode::EMasonryContact Expected;
	};

	/** A row of the boxed-overload table. */
	struct FBoxedInferenceCase
	{
		const TCHAR* Description;
		const DestructionProfiles::FMaterialProfile* FaceA;
		const DestructionProfiles::FMaterialProfile* FaceB;
		DestructionLayout::FPieceBox BoxA;
		DestructionLayout::FPieceBox BoxB;
		FVector InterfaceNormalUnit;
		const FConnectionStrength* Expected;
		const TCHAR* ExpectedName;
	};
}

bool FJointInferenceTest::RunTest(const FString& Parameters)
{
	using namespace JointInferenceTestSupport;
	using namespace DestructionProfiles;

	const TArray<FInferenceCase> Cases = {
		// Masonry, vertical normal: a bed joint → GeneralPurposeMortar. Both signs of Z.
		{
			TEXT("brick/brick, +Z bed joint -> GeneralPurposeMortar"),
			&ClayBrick, &ClayBrick, FVector(0.0, 0.0, 1.0),
			&GeneralPurposeMortar, TEXT("GeneralPurposeMortar")
		},
		{
			TEXT("brick/brick, -Z bed joint -> GeneralPurposeMortar"),
			&ClayBrick, &ClayBrick, FVector(0.0, 0.0, -1.0),
			&GeneralPurposeMortar, TEXT("GeneralPurposeMortar")
		},

		// Masonry, horizontal normal (the 3-arg cannot see a corner) → GeneralPurposeMortarPerpend.
		{
			TEXT("brick/brick, +X head joint -> GeneralPurposeMortarPerpend"),
			&ClayBrick, &ClayBrick, FVector(1.0, 0.0, 0.0),
			&GeneralPurposeMortarPerpend, TEXT("GeneralPurposeMortarPerpend")
		},
		{
			TEXT("brick/brick, +Y horizontal (3-arg: no corner) -> GeneralPurposeMortarPerpend"),
			&ClayBrick, &ClayBrick, FVector(0.0, 1.0, 0.0),
			&GeneralPurposeMortarPerpend, TEXT("GeneralPurposeMortarPerpend")
		},

		/*
		 * Negative horizontal normals. A missing Abs (0 >= -1) would read (-1,0,0) as a bed and
		 * return the 4.5x-too-strong mortar.
		 */
		{
			TEXT("brick/brick, -X head joint -> GeneralPurposeMortarPerpend"),
			&ClayBrick, &ClayBrick, FVector(-1.0, 0.0, 0.0),
			&GeneralPurposeMortarPerpend, TEXT("GeneralPurposeMortarPerpend")
		},
		{
			TEXT("brick/brick, -Y horizontal (3-arg: no corner) -> GeneralPurposeMortarPerpend"),
			&ClayBrick, &ClayBrick, FVector(0.0, -1.0, 0.0),
			&GeneralPurposeMortarPerpend, TEXT("GeneralPurposeMortarPerpend")
		},

		// Either face not compression-dominant (timber) → DryStone, for any normal and ordering.
		{
			TEXT("timber/brick, vertical normal -> DryStone"),
			&Timber, &ClayBrick, FVector(0.0, 0.0, 1.0),
			&DryStone, TEXT("DryStone")
		},
		{
			TEXT("brick/timber, vertical normal -> DryStone"),
			&ClayBrick, &Timber, FVector(0.0, 0.0, 1.0),
			&DryStone, TEXT("DryStone")
		},
		{
			TEXT("timber/brick, horizontal normal -> DryStone"),
			&Timber, &ClayBrick, FVector(1.0, 0.0, 0.0),
			&DryStone, TEXT("DryStone")
		},
		{
			TEXT("brick/timber, horizontal normal -> DryStone"),
			&ClayBrick, &Timber, FVector(0.0, 1.0, 0.0),
			&DryStone, TEXT("DryStone")
		},
	};

	for (const FInferenceCase& Case : Cases)
	{
		const FConnectionStrength Got = BuildMode::JointForContact(
			*Case.FaceA, *Case.FaceB, Case.InterfaceNormalUnit);
		const FConnectionStrength& Want = *Case.Expected;

		const FString Prefix = FString::Printf(
			TEXT("%s (expected %s): "), Case.Description, Case.ExpectedName);

		TestEqual(Prefix + TEXT("CompressiveStrengthMPa"),
			Got.CompressiveStrengthMPa, Want.CompressiveStrengthMPa);
		TestEqual(Prefix + TEXT("ShearCohesionMPa"),
			Got.ShearCohesionMPa, Want.ShearCohesionMPa);
		TestEqual(Prefix + TEXT("TensileStrengthMPa"),
			Got.TensileStrengthMPa, Want.TensileStrengthMPa);
		TestEqual(Prefix + TEXT("FrictionCoefficient"),
			Got.FrictionCoefficient, Want.FrictionCoefficient);
		TestEqual(Prefix + TEXT("MaxShearStrengthMPa"),
			Got.MaxShearStrengthMPa, Want.MaxShearStrengthMPa);
	}

	return true;
}

/**
 * ClassifyMasonryContact tells Bed from Head from Corner (quoin) using both boxes as well as the
 * normal: a head joint's normal is parallel to both long axes, a corner's to only one (DESIGN §8,
 * 2026-09-15). Asserts the enum; the profile mapping is tested by CornerContactGetsFullMortar.
 *
 * Every row's boxes genuinely abut, so a centre-difference implementation cannot pass by accident.
 * Under the crossing rule the poses matter: the old across-Y quoin and tolerance pair were
 * T-junctions and made the table contradictory, so both are now genuine L's. The closer and
 * thick-wall rows test the refinement; the quoin rows guard against it over-firing. No world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClassifyMasonryContactByOrientationTest,
	"DestructionGame.Core.BuildMode.ClassifyMasonryContactByOrientation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FClassifyMasonryContactByOrientationTest::RunTest(const FString& Parameters)
{
	using namespace JointInferenceTestSupport;
	using namespace DestructionLayout;
	using namespace BuildMode;

	const double NaNValue = MakeNaN();
	const double InfValue = MakeInf();

	// On the 22.5 x 11.25 x 7.5 coordinating grid (Core/Layout.h), bottom faces at Z = 0.
	const FPieceBox Stretcher{ FVector(0.0, 0.0, 3.25), XLongBrick };
	const FPieceBox SameCourseNeighbour{ FVector(22.5, 0.0, 3.25), XLongBrick };
	const FPieceBox NextCourseAbove{ FVector(11.25, 0.0, 10.75), XLongBrick };

	/*
	 * The quoin: a Y-long return across one 1 cm joint from the stretcher's +X end (faces at
	 * 10.75 and 11.75), outer Y faces flush at Y = -5.125.
	 */
	const FPieceBox QuoinReturnAcrossX{ FVector(16.875, 5.625, 3.25), YLongBrick };

	/*
	 * The quoin met across a Y face. The old pose (0, 16.875) was a T-junction: the closer row
	 * rotated -90 degrees with arguments swapped, so the table asked Head and Corner of one contact.
	 * This is a true L: the return's +X face is flush with the stretcher's end at X = 10.75 (X span
	 * [0.5, 10.75]) and it meets the stretcher's +Y face across one joint (Y span [6.125, 27.625]).
	 */
	const FPieceBox QuoinReturnAcrossY{ FVector(5.625, 16.875, 3.25), YLongBrick };

	/*
	 * Four ways a crossed brick can sit at a stretcher's end (DESIGN §8 KNOWN LIMIT). The
	 * stretcher's width planes are Y = +/-5.125:
	 *   FLUSH-NEAR (16.875, 5.625)  spans [-5.125, 16.375]: crosses +Y, flush with -Y.
	 *   FLUSH-FAR  (16.875, -5.625) spans [-16.375, 5.125]: crosses -Y, flush with +Y.
	 *   SET IN     (16.875, 8.0)    spans [-2.75, 18.75]: crosses +Y, flush with neither.
	 *   CLOSER     (16.875, 0)      spans [-10.75, 10.75]: crosses both.
	 * The first three are corners. The closer is a header through the wall line: a head joint.
	 */
	const FPieceBox QuoinReturnFlushFarFace{ FVector(16.875, -5.625, 3.25), YLongBrick };
	const FPieceBox QuoinReturnSetIn{ FVector(16.875, 8.0, 3.25), YLongBrick };
	const FPieceBox CloserInTheWallLine{ FVector(16.875, 0.0, 3.25), YLongBrick };

	/*
	 * A 40 x 22 cm wall (width planes Y = +/-11) with a header end-on. The header's 21.5 cm span
	 * lies inside both planes, so it crosses neither: a head joint.
	 */
	const FVector ThickWallHalfExtent(20.0, 11.0, 3.25);
	const FPieceBox ThickXLongWall{ FVector(0.0, 0.0, 3.25), ThickWallHalfExtent };
	const FPieceBox HeaderInsideTheThickWall{ FVector(26.125, 0.0, 3.25), YLongBrick };

	const FPieceBox YRunStretcher{ FVector(0.0, 0.0, 3.25), YLongBrick };
	const FPieceBox YRunNeighbour{ FVector(0.0, 22.5, 3.25), YLongBrick };
	const FPieceBox YRunAcrossX{ FVector(11.25, 0.0, 3.25), YLongBrick };

	const FPieceBox Square{ FVector(0.0, 0.0, 3.0), SquareBlock };
	const FPieceBox SquareNeighbour{ FVector(11.0, 0.0, 3.0), SquareBlock };
	const FPieceBox XLongBesideSquare{ FVector(16.75, 0.0, 3.0), XLongBrick };
	const FPieceBox XLongAheadOfSquare{ FVector(0.0, 11.125, 3.0), XLongBrick };

	/*
	 * Tolerance pair posed as a genuine L (earlier poses interpenetrated, then formed a T).
	 * The Y-long block has long faces at X = 6.0 and 16.25; both test blocks sit one joint off
	 * its +X face at Y centre 5.75 (Y span [0.75, 10.75], flush with its +Y end). The 0.4 cm
	 * extent is the only difference between the two rows.
	 */
	const FPieceBox YLongBesideBlock{ FVector(11.125, 0.0, 3.0), YLongBrick };
	const FPieceBox NearSquare{ FVector(22.45, 5.75, 3.0), NearSquareBlock };
	const FPieceBox BarelyXLong{ FVector(22.85, 5.75, 3.0), BarelyXLongBlock };

	const FPieceBox NaNBox{
		FVector(11.0, 0.0, 3.0), FVector(NaNValue, NaNValue, NaNValue) };

	const TArray<FClassifyCase> Cases = {
		// Vertical-dominant normal -> Bed for any footprints. Both signs (MakeInterface orients by B).
		{ TEXT("two X-long bricks stacked, +Z"),
			Stretcher, NextCourseAbove, FVector(0.0, 0.0, 1.0), EMasonryContact::Bed },
		{ TEXT("two X-long bricks stacked, -Z"),
			Stretcher, NextCourseAbove, FVector(0.0, 0.0, -1.0), EMasonryContact::Bed },

		// Horizontal normal, parallel long axes -> Head.
		{ TEXT("two X-long bricks side by side along X, +X"),
			Stretcher, SameCourseNeighbour, FVector(1.0, 0.0, 0.0), EMasonryContact::Head },
		{ TEXT("two X-long bricks side by side along X, -X"),
			Stretcher, SameCourseNeighbour, FVector(-1.0, 0.0, 0.0), EMasonryContact::Head },
		{ TEXT("two Y-long bricks along Y, +Y"),
			YRunStretcher, YRunNeighbour, FVector(0.0, 1.0, 0.0), EMasonryContact::Head },
		{ TEXT("two Y-long bricks along Y, -Y"),
			YRunStretcher, YRunNeighbour, FVector(0.0, -1.0, 0.0), EMasonryContact::Head },

		// Parallelism is about the pieces, not the normal: two Y-long bricks across an X face are Head.
		{ TEXT("two Y-long bricks meeting across an X face, +X"),
			YRunStretcher, YRunAcrossX, FVector(1.0, 0.0, 0.0), EMasonryContact::Head },

		/*
		 * Horizontal normal, perpendicular long axes -> Corner, across either axis and sign. Scoped to
		 * the interactive build; BuildRealistic's sweep deliberately does not bond corners.
		 */
		{ TEXT("X-long meets Y-long across an X face, +X (the quoin)"),
			Stretcher, QuoinReturnAcrossX, FVector(1.0, 0.0, 0.0), EMasonryContact::Corner },
		{ TEXT("X-long meets Y-long across an X face, -X"),
			Stretcher, QuoinReturnAcrossX, FVector(-1.0, 0.0, 0.0), EMasonryContact::Corner },
		{ TEXT("X-long meets Y-long across a Y face, +Y"),
			Stretcher, QuoinReturnAcrossY, FVector(0.0, 1.0, 0.0), EMasonryContact::Corner },
		{ TEXT("X-long meets Y-long across a Y face, -Y"),
			Stretcher, QuoinReturnAcrossY, FVector(0.0, -1.0, 0.0), EMasonryContact::Corner },

		/*
		 * Crossed long axes are necessary but not sufficient: the crossing rule (DESIGN §8 KNOWN
		 * LIMIT). Take one piece's width planes (the two faces across its long axis) and the other
		 * piece's span along that axis. It is a Corner when the span strictly crosses (low < plane
		 * < high) exactly one plane. Both is a closer, neither a buried header: both are Head, the
		 * fail-closed answer, instead of a 4.5x-too-strong bond. Flush is not a crossing; a loose
		 * comparison would turn a flush quoin into a closer. The rule is symmetric by OR (A's span
		 * against B's planes, or B's against A's); the across-Y quoin needs the second direction.
		 *
		 * Per row (N gives the planes, C the span):
		 *   FLUSH-NEAR: N = stretcher, planes Y +/-5.125; C [-5.125, 16.375] crosses one -> Corner.
		 *   FLUSH-FAR:  C [-16.375, 5.125] crosses one -> Corner.
		 *   SET IN:     C [-2.75, 18.75] crosses one -> Corner.
		 *   ACROSS Y:   N = stretcher gives zero; N = return, planes X 0.5 / 10.75, C [-10.75,
		 *               10.75] crosses 0.5 only -> Corner.
		 *   CLOSER:     N = stretcher gives two; N = closer, planes X 11.75 / 22.0, gives zero -> Head.
		 *   THICK WALL: planes Y +/-11 give zero; planes X 21 / 31.25 give zero -> Head.
		 *   TOLERANCE:  N = BarelyXLong, planes Y 0.75 / 10.75, C [-10.75, 10.75] crosses 0.75 only
		 *               -> Corner. The near-square block has no long axis, so it is Head.
		 */
		{ TEXT("return flush with the FAR width face (past the -Y face only) -> the quoin"),
			Stretcher, QuoinReturnFlushFarFace, FVector(1.0, 0.0, 0.0), EMasonryContact::Corner },
		{ TEXT("return SET IN, flush with neither face but past the +Y face only -> the quoin"),
			Stretcher, QuoinReturnSetIn, FVector(1.0, 0.0, 0.0), EMasonryContact::Corner },
		{ TEXT("closer in the wall line (past BOTH width faces) -> the weaker answer"),
			Stretcher, CloserInTheWallLine, FVector(1.0, 0.0, 0.0), EMasonryContact::Head },
		{ TEXT("header inside a wall thicker than one brick (past NEITHER) -> the weaker answer"),
			ThickXLongWall, HeaderInsideTheThickWall, FVector(1.0, 0.0, 0.0),
			EMasonryContact::Head },

		// No long axis -> Head, failing closed to the weaker masonry answer.
		{ TEXT("square block against an X-long brick, +X -> the weaker answer"),
			Square, XLongBesideSquare, FVector(1.0, 0.0, 0.0), EMasonryContact::Head },
		{ TEXT("square block against an X-long brick, +Y -> the weaker answer"),
			Square, XLongAheadOfSquare, FVector(0.0, 1.0, 0.0), EMasonryContact::Head },
		{ TEXT("two square blocks, +X -> the weaker answer"),
			Square, SquareNeighbour, FVector(1.0, 0.0, 0.0), EMasonryContact::Head },

		/*
		 * The 0.5 cm tolerance from both sides, on half-extents: 0.2 apart is square (Head), 0.6
		 * apart is X-long (Corner). Both blocks share one L pose so only the extent differs.
		 */
		{ TEXT("near-square block (0.2 cm apart) against a Y-long brick, +X"),
			YLongBesideBlock, NearSquare, FVector(1.0, 0.0, 0.0), EMasonryContact::Head },
		{ TEXT("barely X-long block (0.6 cm apart) against a Y-long brick, +X"),
			YLongBesideBlock, BarelyXLong, FVector(1.0, 0.0, 0.0), EMasonryContact::Corner },

		/*
		 * Tilted normals: the dominant component decides (CURRENT_STATE deferral (c)). (0.8, 0, 0.6)
		 * is horizontal, so orientation decides; (0.6, 0, 0.8) is a Bed for both pairs.
		 */
		{ TEXT("perpendicular pair, tilted (0.8,0,0.6) -> horizontal, so orientation decides"),
			Stretcher, QuoinReturnAcrossX, FVector(0.8, 0.0, 0.6), EMasonryContact::Corner },
		{ TEXT("parallel pair, tilted (0.8,0,0.6) -> horizontal, so orientation decides"),
			Stretcher, SameCourseNeighbour, FVector(0.8, 0.0, 0.6), EMasonryContact::Head },
		{ TEXT("perpendicular pair, tilted (0.6,0,0.8) -> Z dominates, a bed"),
			Stretcher, QuoinReturnAcrossX, FVector(0.6, 0.0, 0.8), EMasonryContact::Bed },
		{ TEXT("parallel pair, tilted (0.6,0,0.8) -> Z dominates, a bed"),
			Stretcher, SameCourseNeighbour, FVector(0.6, 0.0, 0.8), EMasonryContact::Bed },

		/*
		 * Degenerate normals fail closed to Head. Deliberately unlike the 3-arg overload, which reads
		 * a zero normal as a bed (fail-open, CURRENT_STATE build-mode item (b)). NaN makes every
		 * comparison false, so the `!(x > y)` guards land on Head.
		 */
		{ TEXT("zero normal -> the weakest masonry answer"),
			Stretcher, QuoinReturnAcrossX, FVector::ZeroVector, EMasonryContact::Head },
		{ TEXT("NaN X in the normal -> the weakest masonry answer"),
			Stretcher, QuoinReturnAcrossX, FVector(NaNValue, 0.0, 0.0), EMasonryContact::Head },
		{ TEXT("all-NaN normal -> the weakest masonry answer"),
			Stretcher, QuoinReturnAcrossX,
			FVector(NaNValue, NaNValue, NaNValue), EMasonryContact::Head },
		{ TEXT("NaN Z in an otherwise horizontal normal -> the weakest masonry answer"),
			Stretcher, QuoinReturnAcrossX, FVector(1.0, 0.0, NaNValue), EMasonryContact::Head },
		{ TEXT("infinite normal -> the weakest masonry answer"),
			Stretcher, QuoinReturnAcrossX, FVector(InfValue, 0.0, 0.0), EMasonryContact::Head },

		// A NaN-extent box has no readable long axis, so it fails closed to Head.
		{ TEXT("NaN-extent box against an X-long brick, +X -> the weakest masonry answer"),
			Stretcher, NaNBox, FVector(1.0, 0.0, 0.0), EMasonryContact::Head },
	};

	for (const FClassifyCase& Case : Cases)
	{
		const EMasonryContact Got =
			ClassifyMasonryContact(Case.BoxA, Case.BoxB, Case.InterfaceNormalUnit);

		TestEqual(
			FString::Printf(
				TEXT("%s: expected %s, got %s"),
				Case.Description, ContactName(Case.Expected), ContactName(Got)),
			static_cast<int32>(Got),
			static_cast<int32>(Case.Expected));

		// Swapping A and B must not change the answer; callers pass pieces in either order.
		const EMasonryContact Swapped =
			ClassifyMasonryContact(Case.BoxB, Case.BoxA, Case.InterfaceNormalUnit);

		TestEqual(
			FString::Printf(
				TEXT("%s, A and B SWAPPED: expected %s, got %s"),
				Case.Description, ContactName(Case.Expected), ContactName(Swapped)),
			static_cast<int32>(Swapped),
			static_cast<int32>(Case.Expected));
	}

	return true;
}

/**
 * The five-argument JointForContact gives full GeneralPurposeMortar at a masonry corner, the
 * perpend at a head joint, mortar at a bed, and DryStone if either face is not
 * compression-dominant (DESIGN §8, 2026-09-15). All five fields compared: mortar and perpend
 * differ only in cohesion and tension. No world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCornerContactGetsFullMortarTest,
	"DestructionGame.Core.BuildMode.CornerContactGetsFullMortar",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCornerContactGetsFullMortarTest::RunTest(const FString& Parameters)
{
	using namespace JointInferenceTestSupport;
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;

	const FPieceBox Stretcher{ FVector(0.0, 0.0, 3.25), XLongBrick };
	const FPieceBox SameCourseNeighbour{ FVector(22.5, 0.0, 3.25), XLongBrick };
	const FPieceBox NextCourseAbove{ FVector(11.25, 0.0, 10.75), XLongBrick };
	const FPieceBox QuoinReturnAcrossX{ FVector(16.875, 5.625, 3.25), YLongBrick };

	// The re-posed across-Y quoin, as in the classification test (the old pose was a T-junction).
	const FPieceBox QuoinReturnAcrossY{ FVector(5.625, 16.875, 3.25), YLongBrick };
	const FPieceBox QuoinReturnFlushFarFace{ FVector(16.875, -5.625, 3.25), YLongBrick };
	const FPieceBox CloserInTheWallLine{ FVector(16.875, 0.0, 3.25), YLongBrick };
	const FPieceBox Square{ FVector(0.0, 0.0, 3.0), SquareBlock };
	const FPieceBox XLongBesideSquare{ FVector(16.75, 0.0, 3.0), XLongBrick };

	const TArray<FBoxedInferenceCase> Cases = {
		// The corner: an interlocked quoin gets the same mortar as a bed joint.
		{ TEXT("brick/brick quoin across an X face, +X"),
			&ClayBrick, &ClayBrick, Stretcher, QuoinReturnAcrossX, FVector(1.0, 0.0, 0.0),
			&GeneralPurposeMortar, TEXT("GeneralPurposeMortar") },
		{ TEXT("brick/brick quoin across an X face, -X"),
			&ClayBrick, &ClayBrick, Stretcher, QuoinReturnAcrossX, FVector(-1.0, 0.0, 0.0),
			&GeneralPurposeMortar, TEXT("GeneralPurposeMortar") },
		{ TEXT("brick/brick quoin across a Y face, +Y"),
			&ClayBrick, &ClayBrick, Stretcher, QuoinReturnAcrossY, FVector(0.0, 1.0, 0.0),
			&GeneralPurposeMortar, TEXT("GeneralPurposeMortar") },
		{ TEXT("brick/brick quoin returning the OTHER way (flush with the far face), +X"),
			&ClayBrick, &ClayBrick, Stretcher, QuoinReturnFlushFarFace, FVector(1.0, 0.0, 0.0),
			&GeneralPurposeMortar, TEXT("GeneralPurposeMortar") },

		/*
		 * A closer in the wall line crosses long axes but turns no corner, so it gets the perpend.
		 * Full mortar would overstate the bond 4.5x (cohesion 0.9 vs 0.2).
		 */
		{ TEXT("brick/brick closer in the wall line, +X"),
			&ClayBrick, &ClayBrick, Stretcher, CloserInTheWallLine, FVector(1.0, 0.0, 0.0),
			&GeneralPurposeMortarPerpend, TEXT("GeneralPurposeMortarPerpend") },

		// Collinear head joint on the same normal stays the perpend: the upgrade is keyed on orientation.
		{ TEXT("brick/brick collinear head joint, +X"),
			&ClayBrick, &ClayBrick, Stretcher, SameCourseNeighbour, FVector(1.0, 0.0, 0.0),
			&GeneralPurposeMortarPerpend, TEXT("GeneralPurposeMortarPerpend") },
		{ TEXT("brick/brick collinear head joint, -X"),
			&ClayBrick, &ClayBrick, Stretcher, SameCourseNeighbour, FVector(-1.0, 0.0, 0.0),
			&GeneralPurposeMortarPerpend, TEXT("GeneralPurposeMortarPerpend") },

		// A square footprint has no long axis, so it fails closed to the weaker joint.
		{ TEXT("square block against an X-long brick, +X"),
			&ClayBrick, &ClayBrick, Square, XLongBesideSquare, FVector(1.0, 0.0, 0.0),
			&GeneralPurposeMortarPerpend, TEXT("GeneralPurposeMortarPerpend") },

		// The bed joint is unchanged.
		{ TEXT("brick/brick bed joint, +Z"),
			&ClayBrick, &ClayBrick, Stretcher, NextCourseAbove, FVector(0.0, 0.0, 1.0),
			&GeneralPurposeMortar, TEXT("GeneralPurposeMortar") },

		// Timber at a corner is still DryStone (mortar does not bond to a board). Both orderings.
		{ TEXT("brick/timber at a corner, +X"),
			&ClayBrick, &Timber, Stretcher, QuoinReturnAcrossX, FVector(1.0, 0.0, 0.0),
			&DryStone, TEXT("DryStone") },
		{ TEXT("timber/brick at a corner, +X"),
			&Timber, &ClayBrick, Stretcher, QuoinReturnAcrossX, FVector(1.0, 0.0, 0.0),
			&DryStone, TEXT("DryStone") },
	};

	for (const FBoxedInferenceCase& Case : Cases)
	{
		const FConnectionStrength Got = JointForContact(
			*Case.FaceA, *Case.FaceB, Case.InterfaceNormalUnit, Case.BoxA, Case.BoxB);

		CheckProfileIdentity(
			*this,
			FString::Printf(TEXT("%s (expected %s): "), Case.Description, Case.ExpectedName),
			Got,
			*Case.Expected);
	}

	return true;
}

/**
 * The five-argument JointForContact matches the three-argument one on every axis-aligned normal,
 * except the masonry corner, which it upgrades from the perpend to full mortar. Pins the size of
 * the physics change. Differing rows assert both the new and old answers.
 *
 * Axis-aligned normals only: on zero/non-finite normals the overloads are meant to disagree (the
 * 3-arg fails open, CURRENT_STATE build-mode item (b)); ClassifyMasonryContactByOrientation pins
 * that. No world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBoxedOverloadAgreesWithTheUnboxedOneOffCornersTest,
	"DestructionGame.Core.BuildMode.BoxedOverloadAgreesWithTheUnboxedOneOffCorners",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBoxedOverloadAgreesWithTheUnboxedOneOffCornersTest::RunTest(const FString& Parameters)
{
	using namespace JointInferenceTestSupport;
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;

	struct FPairing
	{
		const TCHAR* Description;
		FVector ExtentA;
		FVector ExtentB;
		bool bPerpendicular;
	};

	const TArray<FPairing> Pairings = {
		{ TEXT("X-long / X-long (collinear)"), XLongBrick, XLongBrick, false },
		{ TEXT("Y-long / Y-long (collinear)"), YLongBrick, YLongBrick, false },
		{ TEXT("square / X-long (no long axis)"), SquareBlock, XLongBrick, false },
		{ TEXT("X-long / Y-long (perpendicular)"), XLongBrick, YLongBrick, true },
		{ TEXT("Y-long / X-long (perpendicular, swapped)"), YLongBrick, XLongBrick, true },
	};

	const TArray<FVector> AxisNormals = {
		FVector(1.0, 0.0, 0.0), FVector(-1.0, 0.0, 0.0),
		FVector(0.0, 1.0, 0.0), FVector(0.0, -1.0, 0.0),
		FVector(0.0, 0.0, 1.0), FVector(0.0, 0.0, -1.0),
	};

	struct FMaterialPair
	{
		const TCHAR* Description;
		const FMaterialProfile* FaceA;
		const FMaterialProfile* FaceB;
		bool bBothMasonry;
	};

	const TArray<FMaterialPair> MaterialPairs = {
		{ TEXT("brick/brick"), &ClayBrick, &ClayBrick, true },
		{ TEXT("timber/brick"), &Timber, &ClayBrick, false },
		{ TEXT("brick/timber"), &ClayBrick, &Timber, false },
	};

	int32 DifferingRows = 0;

	for (const FMaterialPair& Materials : MaterialPairs)
	{
		for (const FPairing& Pairing : Pairings)
		{
			/*
			 * Boxes one 1 cm joint apart on X, overlapping on Y and Z; one pose for all normals.
			 * The Y offset aligns the -Y faces so each crossed pair is a canonical L (else it is a
			 * closer and stays Head). X-long/Y-long: B at (16.875, 5.625), span [-5.125, 16.375]
			 * crosses +5.125 only -> Corner. Y-long/X-long: B at (16.875, -5.625); B's planes
			 * Y = -10.75 / -0.5, A's span crosses -0.5 only -> Corner.
			 */
			const FPieceBox BoxA{ FVector(0.0, 0.0, 3.25), Pairing.ExtentA };
			const FPieceBox BoxB{
				FVector(
					Pairing.ExtentA.X + 1.0 + Pairing.ExtentB.X,
					Pairing.ExtentB.Y - Pairing.ExtentA.Y,
					3.25),
				Pairing.ExtentB };

			for (const FVector& Normal : AxisNormals)
			{
				const FConnectionStrength Boxed = JointForContact(
					*Materials.FaceA, *Materials.FaceB, Normal, BoxA, BoxB);
				const FConnectionStrength Unboxed = JointForContact(
					*Materials.FaceA, *Materials.FaceB, Normal);

				const bool bHorizontal = FMath::Abs(Normal.Z) < 0.5;
				const bool bIsTheCorner =
					Materials.bBothMasonry && Pairing.bPerpendicular && bHorizontal;

				const FString Prefix = FString::Printf(
					TEXT("%s, %s, normal (%g,%g,%g): "),
					Materials.Description, Pairing.Description, Normal.X, Normal.Y, Normal.Z);

				if (!bIsTheCorner)
				{
					CheckProfileIdentity(
						*this, Prefix + TEXT("boxed must equal unboxed, "), Boxed, Unboxed);
					continue;
				}

				++DifferingRows;

				// The corner: the new answer is full mortar...
				CheckProfileIdentity(
					*this, Prefix + TEXT("the CORNER's boxed answer (GeneralPurposeMortar), "),
					Boxed, GeneralPurposeMortar);

				// ...and the old answer was the perpend, so the difference cannot silently vanish.
				CheckProfileIdentity(
					*this, Prefix + TEXT("the CORNER's unboxed answer (GeneralPurposeMortarPerpend), "),
					Unboxed, GeneralPurposeMortarPerpend);
			}
		}
	}

	// 2 perpendicular pairings x 4 horizontal normals x 1 masonry pair = 8 of 90. Guards a vacuous sweep.
	TestEqual(TEXT("exactly the corner readings differ between the two overloads"),
		DifferingRows, 8);

	return true;
}

/**
 * MakeInterface fed JointForContact's inferred profile, as Placement.cpp's joint loop does, turns
 * a real quoin into a live FConnection with full GeneralPurposeMortar.
 *
 * Tests the composition, not the solver: no corner snap candidate exists yet (CR-2). Checks the
 * pose forms a face, the face is the 66.625 cm2 brick end, the normal is horizontal, and the
 * profile is mortar. No world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FQuoinJointComposedLikeTheSolverIsFullMortarTest,
	"DestructionGame.Core.BuildMode.QuoinJointComposedLikeTheSolverIsFullMortar",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FQuoinJointComposedLikeTheSolverIsFullMortarTest::RunTest(const FString& Parameters)
{
	using namespace JointInferenceTestSupport;
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;

	const double JointCm = 1.0;

	// Stretcher at the origin; the return abuts its +X end across one joint, outer Y faces flush.
	const FPieceBox Stretcher{ FVector(0.0, 0.0, 3.25), XLongBrick };
	const FPieceBox Returning{ FVector(16.875, 5.625, 3.25), YLongBrick };

	const int32 StretcherHandle = 0;
	const int32 ReturnHandle = 1;

	// Placed piece first, neighbour second, as the placement path does.
	const FConnectionStrength Inferred =
		JointForContact(ClayBrick, ClayBrick, FVector(1.0, 0.0, 0.0), Returning, Stretcher);

	FConnection Connection;
	const bool bFormed = MakeInterface(
		ReturnHandle, Returning, StretcherHandle, Stretcher, JointCm, Inferred, Connection);

	if (!TestTrue(
			TEXT("the quoin pose forms a face at all (one axis separated by the joint, "
				 "positive overlap on the other two)"),
			bFormed))
	{
		return true;
	}

	// Horizontal contact, so the mortar below comes from the corner ruling, not a bed joint.
	TestTrue(
		FString::Printf(
			TEXT("the interface normal is horizontal, got (%g,%g,%g)"),
			Connection.InterfaceNormal.X,
			Connection.InterfaceNormal.Y,
			Connection.InterfaceNormal.Z),
		FMath::Abs(Connection.InterfaceNormal.GetSafeNormal().Z) < 0.5);

	// The brick end face: 10.25 x 6.5 = 66.625 cm2.
	TestEqual(TEXT("interface area is the brick end face, cm2"),
		Connection.InterfaceAreaSqCm, 66.625);

	CheckProfileIdentity(
		*this,
		TEXT("the formed quoin connection (expected GeneralPurposeMortar): "),
		Connection.Strength,
		GeneralPurposeMortar);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
