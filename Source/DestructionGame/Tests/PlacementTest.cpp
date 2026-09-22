// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/BuildMode/Placement.h"
#include "Core/BuildMode/SnapSolver.h"
#include "Core/Layout.h"
#include "Core/Structure.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Profiles/ConnectionProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * BuildMode::PlacePiece (BUILD_MODE_PLAN.md slice 2, 2a): snaps against existing pieces, adopts
 * the best pose with mass from geometry and density, and forms the candidate's joints via
 * MakeInterface. World-free; asserts on the resulting structure, never displacement. A
 * next-course snap toward +X lands at origin + (11.25, 0, 7.5), as in SnapSolverTest.
 */

// Named namespace, unique in the module, for unity builds.
namespace PlacementTestSupport
{
	const FVector HalfBrick(10.75, 5.125, 3.25);

	// The same brick turned 90 degrees, running along Y.
	const FVector HalfBrickRotated(5.125, 10.75, 3.25);

	/*
	 * Full-field profile check (FConnectionStrength has no operator==), so the joint is shown to
	 * be bed mortar rather than a sibling profile.
	 */
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
}

/**
 * Two placements grow a jointed running-bond pair: a grounded Free seed with no joints, then a
 * brick that snaps to the next-course pose with one GeneralPurposeMortar bed joint to the seed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlacementGrowsLiveRunningBondPairTest,
	"DestructionGame.Core.BuildMode.PlacementGrowsLiveRunningBondPair",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPlacementGrowsLiveRunningBondPairTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;
	using namespace PlacementTestSupport;

	const FSnapSettings Settings; // BrickSize 21.5x10.25x6.5, joint 1.0, radius 30.
	const double Tol = KINDA_SMALL_NUMBER;

	FBrickLayout Layout;

	// 1. A grounded seed at the origin; nothing to snap to, so Free with no joints.
	const FPlacementResult First = PlacePiece(
		Layout, FVector(0.0, 0.0, 0.0), HalfBrick, ClayBrick, /*bGrounded*/ true, Settings);

	TestEqual(TEXT("first placement returns handle 0"), First.PieceHandle, 0);
	TestEqual(TEXT("first placement is a Free placement"),
		static_cast<int32>(First.Kind), static_cast<int32>(ESnapKind::Free));
	TestEqual(TEXT("first placement forms no joints"), First.JointsFormed, 0);

	TestEqual(TEXT("structure has one piece"), Layout.Structure.NumPieces(), 1);
	TestEqual(TEXT("structure has no connections"), Layout.Structure.NumConnections(), 0);
	TestEqual(TEXT("boxes parallel to pieces"), Layout.Boxes.Num(), 1);

	if (Layout.Structure.NumPieces() == 1)
	{
		const FStructurePiece& P0 = Layout.Structure.GetPiece(0);
		TestTrue(TEXT("first piece is grounded"), P0.bIsGrounded);
		// The exact library pointer, which later joint inference reads.
		TestTrue(TEXT("first piece material is &ClayBrick"),
			P0.Material == &ClayBrick);
	}

	// 2. A brick requested above and toward +X snaps to the next course with one bed joint.
	const FVector Requested(11.0, 0.0, 7.5);
	const FPlacementResult Second = PlacePiece(
		Layout, Requested, HalfBrick, ClayBrick, /*bGrounded*/ false, Settings);

	TestEqual(TEXT("second placement returns handle 1"), Second.PieceHandle, 1);
	TestEqual(TEXT("second placement snaps to BrickNextCourse"),
		static_cast<int32>(Second.Kind), static_cast<int32>(ESnapKind::BrickNextCourse));
	TestEqual(TEXT("second placement forms one joint"), Second.JointsFormed, 1);

	TestEqual(TEXT("structure now has two pieces"), Layout.Structure.NumPieces(), 2);
	TestEqual(TEXT("structure now has one connection"), Layout.Structure.NumConnections(), 1);
	TestEqual(TEXT("boxes still parallel to pieces"), Layout.Boxes.Num(), 2);

	if (Layout.Boxes.Num() == 2)
	{
		const FVector ExpectedCentre(11.25, 0.0, 7.5);
		TestTrue(TEXT("adopted box centre is the running-bond pose (11.25,0,7.5)"),
			Layout.Boxes[1].CentreCm.Equals(ExpectedCentre, Tol));

		// Mass via the same call production uses; this checks the derivation happens.
		if (Layout.Structure.NumPieces() == 2)
		{
			const FPieceBox AdoptedBox{ Layout.Boxes[1].CentreCm, HalfBrick };
			const double ExpectedMassKg =
				PieceMassKg(AdoptedBox, ClayBrick.DensityGramsPerCubicCm);
			TestEqual(TEXT("second piece mass derived from geometry + density"),
				Layout.Structure.GetPiece(1).MassKg, ExpectedMassKg, 1.0e-9);

			const FStructurePiece& P1 = Layout.Structure.GetPiece(1);
			TestFalse(TEXT("second piece is not grounded"), P1.bIsGrounded);
			TestTrue(TEXT("second piece material is &ClayBrick"),
				P1.Material == &ClayBrick);
		}
	}

	// 3. The connection is bed mortar with a real interface, linking handles 0 and 1.
	if (Layout.Structure.NumConnections() == 1)
	{
		const FConnection& Conn = Layout.Structure.GetConnection(0);

		CheckProfileIdentity(
			*this,
			TEXT("bed joint profile == GeneralPurposeMortar: "),
			Conn.Strength,
			GeneralPurposeMortar);

		TestTrue(TEXT("MakeInterface accepted the solver pose (positive area)"),
			Conn.InterfaceAreaSqCm > 0.0);

		// Bed overlap 10.25 * 10.25 = 105.0625 cm2, normal +/-Z; stricter than area > 0.
		TestEqual(TEXT("bed joint interface area is the running-bond overlap 105.0625 cm2"),
			Conn.InterfaceAreaSqCm, 105.0625, 1.0e-6);
		TestEqual(TEXT("bed joint normal is on the Z axis (|Z| == 1)"),
			FMath::Abs(Conn.InterfaceNormal.Z), 1.0, static_cast<double>(KINDA_SMALL_NUMBER));

		const bool bLinksZeroAndOne =
			(Conn.PieceA == 0 && Conn.PieceB == 1) ||
			(Conn.PieceA == 1 && Conn.PieceB == 0);
		TestTrue(TEXT("connection links handles 0 and 1"), bLinksZeroAndOne);
	}

	return true;
}

/**
 * A refused placement keeps Boxes parallel to the pieces. A degenerate box gets NaN mass and
 * AddPiece refuses it; pushing the box anyway would let the next placement read past the piece
 * array. A follow-on valid placement proves the layout still grows.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlacementRefusedDegeneratePieceKeepsBoxesParallelTest,
	"DestructionGame.Core.BuildMode.PlacementRefusedDegeneratePieceKeepsBoxesParallel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPlacementRefusedDegeneratePieceKeepsBoxesParallelTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;
	using namespace PlacementTestSupport;

	const FSnapSettings Settings;

	FBrickLayout Layout;

	// 1. A valid grounded seed.
	const FPlacementResult Seed = PlacePiece(
		Layout, FVector(0.0, 0.0, 0.0), HalfBrick, ClayBrick, /*bGrounded*/ true, Settings);
	TestEqual(TEXT("seed placement returns handle 0"), Seed.PieceHandle, 0);
	TestEqual(TEXT("seed: one piece"), Layout.Structure.NumPieces(), 1);
	TestEqual(TEXT("seed: boxes parallel to pieces"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());

	// 2. Zero Z extent: NaN mass, so AddPiece refuses it.
	const FVector DegenerateExtent(10.75, 5.125, 0.0);
	const FPlacementResult Refused = PlacePiece(
		Layout, FVector(11.0, 0.0, 7.5), DegenerateExtent, ClayBrick, /*bGrounded*/ false, Settings);

	TestEqual(TEXT("refused placement returns INDEX_NONE"),
		Refused.PieceHandle, static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("refused placement forms no joints"), Refused.JointsFormed, 0);

	TestEqual(TEXT("refused placement keeps Boxes parallel to pieces"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());
	TestEqual(TEXT("refused placement adds no piece"), Layout.Structure.NumPieces(), 1);
	TestEqual(TEXT("refused placement adds no connection"), Layout.Structure.NumConnections(), 0);

	// 3. A valid next-course brick still succeeds.
	const FPlacementResult Valid = PlacePiece(
		Layout, FVector(11.0, 0.0, 7.5), HalfBrick, ClayBrick, /*bGrounded*/ false, Settings);

	TestEqual(TEXT("valid placement after refusal returns handle 1"), Valid.PieceHandle, 1);
	TestEqual(TEXT("valid placement snaps to BrickNextCourse"),
		static_cast<int32>(Valid.Kind), static_cast<int32>(ESnapKind::BrickNextCourse));
	TestEqual(TEXT("valid placement: two pieces"), Layout.Structure.NumPieces(), 2);
	TestEqual(TEXT("valid placement: one connection"), Layout.Structure.NumConnections(), 1);
	TestEqual(TEXT("valid placement: boxes parallel to pieces"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());

	return true;
}

/**
 * CR-2a: laying a rotated brick against a stretcher's end adopts the corner-return snap and forms
 * a full GeneralPurposeMortar joint over the 10.25 x 6.5 = 66.625 cm2 end face, not a weak
 * perpend. Checks the whole chain through MakeInterface: pose, area, X normal and profile. The
 * cursor (16.5, 5.5, 3.25) is 0.395 cm from the chosen return and 11.1 cm from the next. No
 * ticking world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlacementFormsTheCornerReturnJointTest,
	"DestructionGame.Core.BuildMode.PlacementFormsTheCornerReturnJoint",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPlacementFormsTheCornerReturnJointTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;
	using namespace PlacementTestSupport;

	const FSnapSettings Settings; // BrickSize 21.5x10.25x6.5, joint 1.0, radius 30.
	const double Tol = KINDA_SMALL_NUMBER;

	FBrickLayout Layout;

	// 1. A grounded stretcher along X, resting on the ground (centre Z = 3.25).
	const FPlacementResult Seed = PlacePiece(
		Layout, FVector(0.0, 0.0, 3.25), HalfBrick, ClayBrick, /*bGrounded*/ true, Settings);

	TestEqual(TEXT("seed placement returns handle 0"), Seed.PieceHandle, 0);
	TestEqual(TEXT("seed placement is a Free placement"),
		static_cast<int32>(Seed.Kind), static_cast<int32>(ESnapKind::Free));

	// 2. The rotated return snaps to X = 10.75 + 1 + 5.125 = 16.875, flush with the stretcher's -Y face.
	const FPlacementResult Return = PlacePiece(
		Layout, FVector(16.5, 5.5, 3.25), HalfBrickRotated, ClayBrick, /*bGrounded*/ true, Settings);

	TestEqual(TEXT("return placement returns handle 1"), Return.PieceHandle, 1);
	TestEqual(TEXT("return placement snaps to BrickCornerReturn"),
		static_cast<int32>(Return.Kind), static_cast<int32>(ESnapKind::BrickCornerReturn));
	TestEqual(TEXT("return placement forms one joint"), Return.JointsFormed, 1);

	TestEqual(TEXT("structure has two pieces"), Layout.Structure.NumPieces(), 2);
	TestEqual(TEXT("structure has one connection"), Layout.Structure.NumConnections(), 1);
	TestEqual(TEXT("boxes parallel to pieces"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());

	if (Layout.Boxes.Num() == 2)
	{
		TestTrue(TEXT("adopted box centre is the corner-return pose (16.875, 5.625, 3.25)"),
			Layout.Boxes[1].CentreCm.Equals(FVector(16.875, 5.625, 3.25), Tol));
	}

	if (Layout.Structure.NumConnections() == 1)
	{
		const FConnection& Conn = Layout.Structure.GetConnection(0);

		// Full-field: mortar and perpend differ only in cohesion (0.9 vs 0.2) and tension (0.7 vs 0.1).
		CheckProfileIdentity(
			*this,
			TEXT("corner joint profile == GeneralPurposeMortar: "),
			Conn.Strength,
			GeneralPurposeMortar);

		TestEqual(TEXT("corner joint interface area is the brick end face 66.625 cm2"),
			Conn.InterfaceAreaSqCm, 66.625, 1.0e-6);
		TestEqual(TEXT("corner joint normal is on the X axis (|X| == 1)"),
			FMath::Abs(Conn.InterfaceNormal.X), 1.0, static_cast<double>(KINDA_SMALL_NUMBER));

		const bool bLinksZeroAndOne =
			(Conn.PieceA == 0 && Conn.PieceB == 1) ||
			(Conn.PieceA == 1 && Conn.PieceB == 0);
		TestTrue(TEXT("connection links handles 0 and 1"), bLinksZeroAndOne);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
