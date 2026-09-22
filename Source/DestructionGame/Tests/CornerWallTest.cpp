// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/BuildMode/Placement.h"
#include "Core/BuildMode/SnapSolver.h"
#include "Core/Connection.h"
#include "Core/Layout.h"
#include "Core/Structure.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Profiles/ConnectionProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Corner-return slice CR-2a (DESIGN §8, 2026-09-15 corner ruling): an L-shaped wall laid through
 * BuildMode::PlacePiece (an X run, a rotated return, a Y run, and a course lapped over the corner)
 * snaps to the intended pose at every step and stands. Needs the corner return, long-axis-aware
 * running bond and boxed joint inference together; a straight wall exercises none of them.
 *
 * Mechanism and outcome, never displacement (DESIGN §4). Mechanism: per-step Kind and joints,
 * counts, profiles, and each key joint's profile, area and normal. Outcome: after SolveLoads the
 * six ground bricks are Grounded and the five above Supported.
 *
 * The quoin (2-3) is vertical, so it carries no vertical load and the support check cannot see
 * it; it is pinned by the mechanism sections only. The corner brick's bed onto the return (9-3)
 * is what makes the return load-bearing from above (section 7).
 *
 * The build (HalfBrick = (10.75,5.125,3.25) along X, HalfBrickRotated along Y; the Core/Layout.h
 * grid; course 0 rests on the ground at Z = 3.25):
 *
 *   idx  requested centre        extent    grounded  -> Kind               joints
 *   0    ( 0.000,  0.000, 3.25)  X-long    yes       -> Free               0
 *   1    (22.500,  0.000, 3.25)  X-long    yes       -> BrickSameCourse    1  head ->0
 *   2    (45.000,  0.000, 3.25)  X-long    yes       -> BrickSameCourse    1  head ->1
 *   3    (61.875,  5.000, 3.25)  Y-long    yes       -> BrickCornerReturn  1  quoin->2
 *   4    (61.875, 28.000, 3.25)  Y-long    yes       -> BrickSameCourse    1  head ->3
 *   5    (61.875, 50.500, 3.25)  Y-long    yes       -> BrickSameCourse    1  head ->4
 *   6    (11.250,  0.000,10.75)  X-long    no        -> BrickNextCourse    2  beds ->0,1
 *   7    (33.750,  0.000,10.75)  X-long    no        -> BrickNextCourse    3  beds ->1,2 + head->6
 *   8    (61.875, 16.875,10.75)  Y-long    no        -> BrickNextCourse    2  beds ->3,4
 *   9    (56.250,  0.000,10.75)  X-long    no        -> BrickNextCourse    4  beds ->2,3
 *                                                                            + head->7
 *                                                                            + quoin->8
 *  10    (61.875, 39.375,10.75)  Y-long    no        -> BrickNextCourse    3  beds ->4,5
 *                                                                            + head->8
 *
 * Cursors are chosen against the solver's raw-distance ranking:
 *   - step 3 at Y = 5.0 is 0.625 cm from the intended return (61.875, 5.625); competing returns
 *     are occupied or outside the 30 cm radius.
 *   - steps 4 and 5 sit 0.125 cm short of the Y pitch, so same-course beats next-course.
 *   - steps 6 to 10 sit exactly on their running-bond poses, where neighbours' poses merge
 *     into one candidate carrying all their joints.
 *
 * Step 9 is the corner brick (review finding B2), lapping the course-1 stretcher over the return.
 * At (56.25, 0, 10.75) it beds on brick 2 and on the return, brick 3, each a 10.25 x 10.25 =
 * 105.0625 cm2 lap; heads against brick 7; and meets brick 8 across a 66.625 cm2 face that the
 * L-footprint rule reads as a corner.
 *
 * 19 connections: 10 beds + 2 quoins (GeneralPurposeMortar) + 7 heads (GeneralPurposeMortarPerpend).
 * World-free. Regression guard for B2, where step 9 never bedded on the crossed return.
 */

// Named namespace: unity builds merge translation units.
namespace CornerWallTestSupport
{
	const FVector HalfBrick(10.75, 5.125, 3.25);
	const FVector HalfBrickRotated(5.125, 10.75, 3.25);

	/*
	 * Compares all five fields; FConnectionStrength has no operator==. Mortar and its perpend
	 * differ only on cohesion and tension.
	 */
	bool ProfileMatches(const FConnectionStrength& Got, const FConnectionStrength& Want)
	{
		return Got.CompressiveStrengthMPa == Want.CompressiveStrengthMPa
			&& Got.ShearCohesionMPa == Want.ShearCohesionMPa
			&& Got.TensileStrengthMPa == Want.TensileStrengthMPa
			&& Got.FrictionCoefficient == Want.FrictionCoefficient
			&& Got.MaxShearStrengthMPa == Want.MaxShearStrengthMPa;
	}

	/** One placement: the request and the expected snap answer. */
	struct FStep
	{
		FVector RequestedCentreCm;
		FVector ExtentCm;
		bool bGrounded;
		BuildMode::ESnapKind ExpectedKind;
		int32 ExpectedJoints;
		FVector ExpectedCentreCm;
	};

	/** Index of the connection joining two pieces, either order, or INDEX_NONE. */
	int32 FindConnectionIndex(const FStructure& Structure, int32 A, int32 B)
	{
		for (int32 c = 0; c < Structure.NumConnections(); ++c)
		{
			const FConnection& Conn = Structure.GetConnection(c);
			if ((Conn.PieceA == A && Conn.PieceB == B) || (Conn.PieceA == B && Conn.PieceB == A))
			{
				return c;
			}
		}
		return INDEX_NONE;
	}

	/** The connection joining two pieces, either order, or nullptr. */
	const FConnection* FindConnection(const FStructure& Structure, int32 A, int32 B)
	{
		const int32 Index = FindConnectionIndex(Structure, A, B);
		return Index == INDEX_NONE ? nullptr : &Structure.GetConnection(Index);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCornerWallStandsTest,
	"DestructionGame.Core.BuildMode.CornerWallStands",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCornerWallStandsTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;
	using namespace CornerWallTestSupport;

	const FSnapSettings Settings; // BrickSize 21.5x10.25x6.5, joint 1.0, radius 30.
	const double Tol = KINDA_SMALL_NUMBER;

	const FStep Steps[] = {
		// Course 0, the X leg.
		{ FVector(0.0, 0.0, 3.25), HalfBrick, true,
			ESnapKind::Free, 0, FVector(0.0, 0.0, 3.25) },
		{ FVector(22.5, 0.0, 3.25), HalfBrick, true,
			ESnapKind::BrickSameCourse, 1, FVector(22.5, 0.0, 3.25) },
		{ FVector(45.0, 0.0, 3.25), HalfBrick, true,
			ESnapKind::BrickSameCourse, 1, FVector(45.0, 0.0, 3.25) },

		// The corner: a rotated brick returning off the X leg's +X end.
		{ FVector(61.875, 5.0, 3.25), HalfBrickRotated, true,
			ESnapKind::BrickCornerReturn, 1, FVector(61.875, 5.625, 3.25) },

		// Course 0, the Y leg.
		{ FVector(61.875, 28.0, 3.25), HalfBrickRotated, true,
			ESnapKind::BrickSameCourse, 1, FVector(61.875, 28.125, 3.25) },
		{ FVector(61.875, 50.5, 3.25), HalfBrickRotated, true,
			ESnapKind::BrickSameCourse, 1, FVector(61.875, 50.625, 3.25) },

		// Course 1, staggered over both legs.
		{ FVector(11.25, 0.0, 10.75), HalfBrick, false,
			ESnapKind::BrickNextCourse, 2, FVector(11.25, 0.0, 10.75) },
		{ FVector(33.75, 0.0, 10.75), HalfBrick, false,
			ESnapKind::BrickNextCourse, 3, FVector(33.75, 0.0, 10.75) },
		{ FVector(61.875, 16.875, 10.75), HalfBrickRotated, false,
			ESnapKind::BrickNextCourse, 2, FVector(61.875, 16.875, 10.75) },

		// The corner brick: beds on 2 and 3, head to 7, corner face to 8.
		{ FVector(56.25, 0.0, 10.75), HalfBrick, false,
			ESnapKind::BrickNextCourse, 4, FVector(56.25, 0.0, 10.75) },

		// The Y leg's second course-1 brick, bedding on bricks 4 and 5 and abutting 8.
		{ FVector(61.875, 39.375, 10.75), HalfBrickRotated, false,
			ESnapKind::BrickNextCourse, 3, FVector(61.875, 39.375, 10.75) },
	};
	const int32 ExpectedPieces = UE_ARRAY_COUNT(Steps);

	FBrickLayout Layout; // grown from empty, one PlacePiece at a time

	/*
	 * 1. Per step: Kind, joints formed, and adopted centre. The centre matters because the
	 * corner return has four poses and only one turns this corner.
	 */
	for (int32 i = 0; i < ExpectedPieces; ++i)
	{
		const FStep& Step = Steps[i];
		const FPlacementResult Result = PlacePiece(
			Layout, Step.RequestedCentreCm, Step.ExtentCm, ClayBrick, Step.bGrounded, Settings);

		const FString Prefix = FString::Printf(TEXT("step %d "), i);

		TestEqual(*(Prefix + TEXT("piece handle in placement order")), Result.PieceHandle, i);
		TestEqual(*(Prefix + TEXT("snap Kind")),
			static_cast<int32>(Result.Kind), static_cast<int32>(Step.ExpectedKind));
		TestEqual(*(Prefix + TEXT("joints formed")), Result.JointsFormed, Step.ExpectedJoints);

		if (Layout.Boxes.Num() == i + 1)
		{
			TestTrue(
				*FString::Printf(
					TEXT("%sadopted centre is (%g,%g,%g), got (%g,%g,%g)"),
					*Prefix,
					Step.ExpectedCentreCm.X, Step.ExpectedCentreCm.Y, Step.ExpectedCentreCm.Z,
					Layout.Boxes[i].CentreCm.X, Layout.Boxes[i].CentreCm.Y,
					Layout.Boxes[i].CentreCm.Z),
				Layout.Boxes[i].CentreCm.Equals(Step.ExpectedCentreCm, Tol));
		}
	}

	// 2. Eleven pieces, nineteen connections, boxes parallel to pieces.
	TestEqual(TEXT("the L-wall has eleven pieces"), Layout.Structure.NumPieces(), ExpectedPieces);
	TestEqual(TEXT("the L-wall has nineteen connections"), Layout.Structure.NumConnections(), 19);
	TestEqual(TEXT("boxes parallel to pieces"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());

	/*
	 * 3. The quoin 2-3: full mortar over the 10.25 x 6.5 = 66.625 cm2 end face, horizontal normal
	 * (the contact the pre-ruling inference called a perpend). Only pinned here; section 6 cannot see it.
	 */
	const FConnection* Quoin = FindConnection(Layout.Structure, 2, 3);
	TestNotNull(TEXT("a connection joins the X leg's end brick to the return"), Quoin);
	if (Quoin != nullptr)
	{
		TestTrue(TEXT("the quoin joint is full GeneralPurposeMortar"),
			ProfileMatches(Quoin->Strength, GeneralPurposeMortar));
		TestEqual(TEXT("the quoin's interface area is the brick end face 66.625 cm2"),
			Quoin->InterfaceAreaSqCm, 66.625, 1.0e-6);
		TestEqual(TEXT("the quoin's normal is horizontal, on the X axis (|X| == 1)"),
			FMath::Abs(Quoin->InterfaceNormal.X), 1.0, Tol);
	}

	// 4. Control: same-direction head joints stay the weak perpend.
	const FConnection* HeadZeroOne = FindConnection(Layout.Structure, 0, 1);
	const FConnection* HeadOneTwo = FindConnection(Layout.Structure, 1, 2);
	TestNotNull(TEXT("a connection joins X-leg bricks 0 and 1"), HeadZeroOne);
	TestNotNull(TEXT("a connection joins X-leg bricks 1 and 2"), HeadOneTwo);
	if (HeadZeroOne != nullptr)
	{
		TestTrue(TEXT("the head joint 0-1 is the weak GeneralPurposeMortarPerpend"),
			ProfileMatches(HeadZeroOne->Strength, GeneralPurposeMortarPerpend));
	}
	if (HeadOneTwo != nullptr)
	{
		TestTrue(TEXT("the head joint 1-2 is the weak GeneralPurposeMortarPerpend"),
			ProfileMatches(HeadOneTwo->Strength, GeneralPurposeMortarPerpend));
	}

	/*
	 * 5. Profile mix: 12 mortar, 7 perpend, nothing unrecognised. The second quoin is 9-8: the
	 * corner brick's X span crosses exactly one of brick 8's width-face planes, so the
	 * L-footprint rule reads Corner.
	 */
	int32 MortarCount = 0;
	int32 PerpendCount = 0;
	int32 UnrecognisedCount = 0;
	for (int32 c = 0; c < Layout.Structure.NumConnections(); ++c)
	{
		const FConnectionStrength& S = Layout.Structure.GetConnection(c).Strength;
		if (ProfileMatches(S, GeneralPurposeMortar))
		{
			++MortarCount;
		}
		else if (ProfileMatches(S, GeneralPurposeMortarPerpend))
		{
			++PerpendCount;
		}
		else
		{
			++UnrecognisedCount;
		}
	}

	TestEqual(TEXT("12 bonded joints are GeneralPurposeMortar (10 beds + 2 quoins)"),
		MortarCount, 12);
	TestEqual(TEXT("7 head joints are GeneralPurposeMortarPerpend"), PerpendCount, 7);
	TestEqual(TEXT("no connection has an unexpected profile"), UnrecognisedCount, 0);

	/*
	 * 6. Exact support kind per piece (GetPieceSupport, not IsPieceSupported, which is also true
	 * for grounded pieces). The five course-1 bricks reach the ground only through formed beds.
	 * SetThreeDimensional is the caller's job per Placement.h (Y-normal joints); SolveLoads ignores
	 * it, only the LP bridge reads it.
	 */
	Layout.Structure.SetThreeDimensional(true);
	Layout.Structure.SolveLoads();

	for (int32 p = 0; p < Layout.Structure.NumPieces(); ++p)
	{
		const EPieceSupport Want = (p <= 5) ? EPieceSupport::Grounded : EPieceSupport::Supported;
		TestEqual(
			*FString::Printf(
				TEXT("piece %d support kind after SolveLoads (%s expected)"),
				p, (p <= 5) ? TEXT("Grounded") : TEXT("Supported")),
			static_cast<int32>(Layout.Structure.GetPieceSupport(p)),
			static_cast<int32>(Want));
	}

	/*
	 * 7. The corner lap 9-3 (finding B2): the corner brick beds on the return over
	 * 10.25 x 10.25 = 105.0625 cm2 with a vertical normal, as full mortar (a vertical contact is
	 * a Bed regardless of orientation). Utilisation > 0 shows it carries load; the value is not
	 * pinned, as that would pin router arithmetic.
	 */
	const int32 CornerLapIndex = FindConnectionIndex(Layout.Structure, 9, 3);
	TestTrue(
		TEXT("the course-1 corner brick beds onto the return (a connection 9-3 exists)"),
		CornerLapIndex != INDEX_NONE);
	if (CornerLapIndex != INDEX_NONE)
	{
		const FConnection& Lap = Layout.Structure.GetConnection(CornerLapIndex);
		TestTrue(TEXT("the corner lap 9-3 is full GeneralPurposeMortar"),
			ProfileMatches(Lap.Strength, GeneralPurposeMortar));
		TestEqual(TEXT("the corner lap's interface area is the 10.25 x 10.25 lap, 105.0625 cm2"),
			Lap.InterfaceAreaSqCm, 105.0625, 1.0e-6);
		TestEqual(TEXT("the corner lap's normal is VERTICAL (|Z| == 1)"),
			FMath::Abs(Lap.InterfaceNormal.Z), 1.0, Tol);

		const double LapUtilisation = Layout.Structure.GetConnectionUtilisation(CornerLapIndex);
		TestTrue(
			*FString::Printf(
				TEXT("the corner lap carries load after SolveLoads (utilisation %g > 0)"),
				LapUtilisation),
			LapUtilisation > 0.0);
	}

	// Control: its other bed, onto brick 2, so it laps both legs rather than swapping one.
	const FConnection* LegLap = FindConnection(Layout.Structure, 9, 2);
	TestNotNull(TEXT("the course-1 corner brick also beds onto the X leg's end brick (9-2)"), LegLap);
	if (LegLap != nullptr)
	{
		TestTrue(TEXT("the leg lap 9-2 is full GeneralPurposeMortar"),
			ProfileMatches(LegLap->Strength, GeneralPurposeMortar));
		TestEqual(TEXT("the leg lap's interface area is the 10.25 x 10.25 lap, 105.0625 cm2"),
			LegLap->InterfaceAreaSqCm, 105.0625, 1.0e-6);
		TestEqual(TEXT("the leg lap's normal is VERTICAL (|Z| == 1)"),
			FMath::Abs(LegLap->InterfaceNormal.Z), 1.0, Tol);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
