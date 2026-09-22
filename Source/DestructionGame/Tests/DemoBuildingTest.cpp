// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/BuildMode/DemoBuilding.h"
#include "Core/BuildMode/Placement.h"
#include "Core/BuildMode/SnapSolver.h"
#include "Core/Layout.h"
#include "Core/Structure.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Profiles/ConnectionProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The scripted demo building (BUILD_MODE_PLAN.md slice 2, 2c): BuildDemoBuilding lays a small
 * running-bond wall with a timber wall-plate entirely through PlacePiece, and it stands.
 *
 * Mechanism: per-step snap Kind and joint count, piece and connection counts, and the profile
 * mix. Outcome: after SolveLoads each piece has the exact expected support kind. No displacement.
 *
 * Geometry (requested centres passed to PlacePiece; running-bond grid 22.5 x 11.25 x 7.5):
 *
 *   idx  requested centre     grounded  material   half-extent            -> Kind             joints
 *   0    (0.00, 0, 0.00)      yes       ClayBrick  (10.75,5.125,3.25)     -> Free             0
 *   1    (22.50,0, 0.00)      yes       ClayBrick  (10.75,5.125,3.25)     -> BrickSameCourse  1  head->0
 *   2    (45.00,0, 0.00)      yes       ClayBrick  (10.75,5.125,3.25)     -> BrickSameCourse  1  head->1
 *   3    (67.50,0, 0.00)      yes       ClayBrick  (10.75,5.125,3.25)     -> BrickSameCourse  1  head->2
 *   4    (11.25,0, 7.50)      no        ClayBrick  (10.75,5.125,3.25)     -> BrickNextCourse  2  beds->0,1
 *   5    (33.75,0, 7.50)      no        ClayBrick  (10.75,5.125,3.25)     -> BrickNextCourse  3  beds->1,2 + head->4
 *   6    (56.25,0, 7.50)      no        ClayBrick  (10.75,5.125,3.25)     -> BrickNextCourse  3  beds->2,3 + head->5
 *   7    (33.75,0,16.75)      no        Timber     (33.75,5.125,5.00)     -> TimberCentered   3  bearings->4,5,6
 *
 * 8 pieces; 14 connections = 6 beds (GeneralPurposeMortar) + 5 heads
 * (GeneralPurposeMortarPerpend) + 3 timber bearings (DryStone). The 67.5 cm plate spans all
 * three course-1 bricks. All normals are X or Z, so the build is planar. World-free.
 */

// Named namespace, not anonymous, for unity builds.
namespace DemoBuildingTestSupport
{
	// FConnectionStrength has no operator==, so compare all five load-bearing fields.
	bool ProfileMatches(const FConnectionStrength& Got, const FConnectionStrength& Want)
	{
		return Got.CompressiveStrengthMPa == Want.CompressiveStrengthMPa
			&& Got.ShearCohesionMPa == Want.ShearCohesionMPa
			&& Got.TensileStrengthMPa == Want.TensileStrengthMPa
			&& Got.FrictionCoefficient == Want.FrictionCoefficient
			&& Got.MaxShearStrengthMPa == Want.MaxShearStrengthMPa;
	}

	struct FExpectedStep
	{
		BuildMode::ESnapKind Kind;
		int32 JointsFormed;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildDemoBuildingStandsTest,
	"DestructionGame.Core.BuildMode.BuildDemoBuildingStands",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildDemoBuildingStandsTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;
	using namespace DemoBuildingTestSupport;

	const FSnapSettings Settings; // BrickSize 21.5x10.25x6.5, joint 1.0, radius 30.

	FBrickLayout Layout; // empty; the builder fills it via PlacePiece

	const TArray<FPlacementResult> Steps = BuildDemoBuilding(Layout, Settings);

	// 1. Per-step snap Kind and joints formed.
	const FExpectedStep Expected[] = {
		{ ESnapKind::Free,            0 },
		{ ESnapKind::BrickSameCourse, 1 },
		{ ESnapKind::BrickSameCourse, 1 },
		{ ESnapKind::BrickSameCourse, 1 },
		{ ESnapKind::BrickNextCourse, 2 },
		{ ESnapKind::BrickNextCourse, 3 },
		{ ESnapKind::BrickNextCourse, 3 },
		{ ESnapKind::TimberCentered,  3 },
	};
	const int32 ExpectedPieces = UE_ARRAY_COUNT(Expected);

	TestEqual(TEXT("builder returns one result per placed piece"),
		Steps.Num(), ExpectedPieces);

	if (Steps.Num() == ExpectedPieces)
	{
		for (int32 i = 0; i < ExpectedPieces; ++i)
		{
			const FString Prefix = FString::Printf(TEXT("step %d "), i);
			TestEqual(*(Prefix + TEXT("snap Kind")),
				static_cast<int32>(Steps[i].Kind),
				static_cast<int32>(Expected[i].Kind));
			TestEqual(*(Prefix + TEXT("joints formed")),
				Steps[i].JointsFormed, Expected[i].JointsFormed);
			TestEqual(*(Prefix + TEXT("piece handle in placement order")),
				Steps[i].PieceHandle, i);
		}
	}

	// 2. Structure shape; boxes stay parallel to pieces (PlacePiece's invariant).
	TestEqual(TEXT("structure has eight pieces"), Layout.Structure.NumPieces(), 8);
	TestEqual(TEXT("structure has fourteen connections"), Layout.Structure.NumConnections(), 14);
	TestEqual(TEXT("boxes parallel to pieces"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());

	/*
	 * 3. Profile mix, each checked against its normal (beds and bearings on Z, heads on X), so
	 * two swapped profiles cannot cancel out in the totals.
	 */
	const double AxisTol = static_cast<double>(KINDA_SMALL_NUMBER);
	int32 BedMortarCount = 0;
	int32 PerpendCount = 0;
	int32 DryStoneCount = 0;
	int32 UnrecognisedCount = 0;
	for (int32 c = 0; c < Layout.Structure.NumConnections(); ++c)
	{
		const FConnection& Conn = Layout.Structure.GetConnection(c);
		const FConnectionStrength& S = Conn.Strength;
		const FVector N = Conn.InterfaceNormal;
		const FString CPrefix = FString::Printf(TEXT("connection %d "), c);
		if (ProfileMatches(S, GeneralPurposeMortar))
		{
			++BedMortarCount;
			TestEqual(*(CPrefix + TEXT("bed mortar has a Z-axis normal")),
				FMath::Abs(N.Z), 1.0, AxisTol);
		}
		else if (ProfileMatches(S, GeneralPurposeMortarPerpend))
		{
			++PerpendCount;
			TestEqual(*(CPrefix + TEXT("perpend head has an X-axis normal")),
				FMath::Abs(N.X), 1.0, AxisTol);
		}
		else if (ProfileMatches(S, DryStone))
		{
			++DryStoneCount;
			TestEqual(*(CPrefix + TEXT("dry-stone bearing has a Z-axis normal")),
				FMath::Abs(N.Z), 1.0, AxisTol);
		}
		else
		{
			++UnrecognisedCount;
		}
	}

	TestEqual(TEXT("6 bed joints are GeneralPurposeMortar"), BedMortarCount, 6);
	TestEqual(TEXT("5 head joints are GeneralPurposeMortarPerpend"), PerpendCount, 5);
	TestEqual(TEXT("3 timber bearings are DryStone"), DryStoneCount, 3);
	TestEqual(TEXT("no connection has an unexpected profile"), UnrecognisedCount, 0);

	/*
	 * 4. It stands through real joints. Exact support kind, not IsPieceSupported, which would
	 * pass if everything were grounded: pieces 0-3 Grounded, 4-7 Supported.
	 */
	Layout.Structure.SolveLoads();

	for (int32 p = 0; p < Layout.Structure.NumPieces(); ++p)
	{
		const EPieceSupport Want = (p <= 3) ? EPieceSupport::Grounded : EPieceSupport::Supported;
		const FString Prefix = FString::Printf(
			TEXT("piece %d support kind after SolveLoads (%s expected)"),
			p, (p <= 3) ? TEXT("Grounded") : TEXT("Supported"));
		TestEqual(*Prefix,
			static_cast<int32>(Layout.Structure.GetPieceSupport(p)),
			static_cast<int32>(Want));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
