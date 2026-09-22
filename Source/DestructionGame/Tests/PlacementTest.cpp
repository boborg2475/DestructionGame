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
 * Unit test for the placement API — BUILD_MODE_PLAN.md slice 2, behavior 2a.
 *
 * The mechanism under test: BuildMode::PlacePiece turns a snap into a live,
 * jointed piece of a growing DestructionLayout::FBrickLayout. It runs the
 * snap-candidate solver against the pieces already there, adopts the piece
 * at the best-ranked snapped pose (mass from geometry + density), and forms
 * that candidate's joints as real FConnections via MakeInterface.
 *
 * World-free and on the mechanism, never displacement: nothing ticks and
 * nothing is solved here, so the assertions are on the live structure the
 * placement produced — piece count, grounded/material state, connection
 * count, the snapped box centre, the derived mass, and the formed joint's
 * profile/interface area/endpoints. A subsequent SolveLoads could run
 * against it; a later slice proves it stands.
 *
 * The running-bond pose is read, not re-derived: Core/Layout.h documents that
 * a 21.5 x 10.25 x 6.5 brick on 1 cm joints gives the 22.5 x 11.25 x 7.5
 * coordinating grid, so a next-course snap toward +X lands at
 * origin + (11.25, 0, 7.5). Matches SnapSolverTest's fixture geometry exactly.
 */

/*
 * Named namespace, distinct from every other one in this module — an
 * anonymous namespace is private to a translation unit, not a file, and a
 * unity build merges files. See SnapSolverTest.cpp / JointInferenceTest.cpp
 * for the rule.
 */
namespace PlacementTestSupport
{
	const FVector HalfBrick(10.75, 5.125, 3.25);

	// The same brick turned through 90 degrees — the return a corner is made of. A UK metric brick is 21.5 x 10.25 x 6.5, so running along Y it is (5.125, 10.75, 3.25).
	const FVector HalfBrickRotated(5.125, 10.75, 3.25);

	/*
	 * Full-field profile identity. FConnectionStrength has no operator==, so
	 * a joint profile is pinned by matching all five fields against the
	 * named library constant — the same discipline SnapSolverTest /
	 * JointInferenceTest use. Proves the auto-formed joint is the intended
	 * strong bed mortar rather than a sibling profile (weak perpend or
	 * dry-stone bearing), which differ on compression, friction and the
	 * shear ceiling.
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
 * Two placements grow a live, jointed running-bond pair.
 *
 * First a grounded brick at the origin (a Free placement — nothing to snap to): one
 * piece, grounded, its material set, no connections. Then a second brick requested
 * just above and biased +X snaps to the running-bond next-course pose and forms one
 * live GeneralPurposeMortar bed joint back to the first — two pieces, one connection
 * with a positive interface area, linking handles 0 and 1.
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

	FBrickLayout Layout; // empty: no pieces, no boxes.

	// 1. First piece: a grounded ClayBrick at the origin. With nothing nearby the only candidate is the Free fallback, so it adopts at the requested pose and forms no joints.
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
		// The stored material must be the exact library pointer passed, or later NearbyMaterials inference has nothing to read.
		TestTrue(TEXT("first piece material is &ClayBrick"),
			P0.Material == &ClayBrick);
	}

	// 2. Second piece: a (non-grounded) ClayBrick requested just above the first and biased toward +X. It must snap to the running-bond next-course pose and form one live bed joint back to the first piece.
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
		// The adopted box sits at the running-bond half-stagger, one course up: origin + (11.25, 0, 7.5). Exact coordinate, tight tolerance.
		const FVector ExpectedCentre(11.25, 0.0, 7.5);
		TestTrue(TEXT("adopted box centre is the running-bond pose (11.25,0,7.5)"),
			Layout.Boxes[1].CentreCm.Equals(ExpectedCentre, Tol));

		// Mass is derived from the adopted box geometry and material density — computed here through the same call production uses, so the assertion is on the derivation being present, not a magic number.
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

	// 3. The formed connection is a real, live joint: strong bed mortar, an interface area MakeInterface actually computed (positive), linking handles 0 and 1 in either order.
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

		/*
		 * A real bed joint, asserted not inferred. The running-bond bed
		 * overlap is the in-plane product 10.25 * 10.25 = 105.0625 cm2
		 * (Core/Layout.h), and a bed joint's normal is +/-Z. Pinning both
		 * turns "not a degenerate joint" into an explicit fact — a face on
		 * the wrong axis or a slivered overlap would slip past a bare > 0
		 * area check.
		 */
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
 * A refused placement must not desync Boxes from the piece array.
 *
 * PlacePiece derives mass with PieceMassKg, which returns NaN for a
 * degenerate box (Core/Layout.cpp — fail-closed), and FStructure::AddPiece
 * then refuses a NaN mass with INDEX_NONE. If the placement pushes its box
 * regardless, Boxes.Num() runs one ahead of NumPieces() — and the next
 * placement, which walks Boxes and resolves each index as a piece handle,
 * reads off the end of the piece array.
 *
 * The invariant Boxes.Num() == Structure.NumPieces() is the mechanism: exact,
 * binary, immune to jitter. The follow-on valid placement is the outcome
 * control — it proves the layout wasn't corrupted, so the structure can
 * still grow.
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

	// 1. A valid grounded seed brick. Succeeds and leaves the invariant intact.
	const FPlacementResult Seed = PlacePiece(
		Layout, FVector(0.0, 0.0, 0.0), HalfBrick, ClayBrick, /*bGrounded*/ true, Settings);
	TestEqual(TEXT("seed placement returns handle 0"), Seed.PieceHandle, 0);
	TestEqual(TEXT("seed: one piece"), Layout.Structure.NumPieces(), 1);
	TestEqual(TEXT("seed: boxes parallel to pieces"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());

	// 2. A degenerate piece: zero Z extent, so PieceMassKg returns NaN and AddPiece refuses it. Not brick-sized either, so it offers only a Free candidate that forms no joints. The placement must refuse cleanly and leave the layout untouched.
	const FVector DegenerateExtent(10.75, 5.125, 0.0);
	const FPlacementResult Refused = PlacePiece(
		Layout, FVector(11.0, 0.0, 7.5), DegenerateExtent, ClayBrick, /*bGrounded*/ false, Settings);

	TestEqual(TEXT("refused placement returns INDEX_NONE"),
		Refused.PieceHandle, static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("refused placement forms no joints"), Refused.JointsFormed, 0);

	// THE INVARIANT: no orphan box. Boxes must stay parallel to the piece array.
	TestEqual(TEXT("refused placement keeps Boxes parallel to pieces"),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());
	TestEqual(TEXT("refused placement adds no piece"), Layout.Structure.NumPieces(), 1);
	TestEqual(TEXT("refused placement adds no connection"), Layout.Structure.NumConnections(), 0);

	// 3. A valid next-course brick onto the seed still succeeds — proving the refused placement didn't corrupt the layout. If an orphan box had desynced the arrays, this placement's NearbyMaterials walk would read past the piece array.
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
 * CR-2a, placement forms the quoin — behaviour in one sentence: PlacePiece
 * laying a rotated brick against the end of a stretcher adopts the
 * corner-return snap and forms a live connection carrying the quoin's full
 * GeneralPurposeMortar over the brick's 66.625 cm2 end face, not the head
 * joint's weak perpend.
 *
 * Why this and not only the solver test: the solver decides the profile; the
 * placement path is where it becomes a joint the collapse system can read,
 * joined by Placement.cpp's loop handing the candidate's profile to
 * DestructionLayout::MakeInterface. This pins the whole chain: the pose
 * survives MakeInterface's face test (one axis separated by exactly the
 * joint, positive overlap on the other two), the area it computes is the end
 * face rather than a bed or a sliver, the normal it derives is horizontal —
 * precisely the contact the pre-refinement rule called a perpend — and the
 * profile riding on the connection is the corner's mortar.
 *
 * The area is derived, not copied: the shared face is the brick's width by
 * its height, 10.25 x 6.5 = 66.625 cm2, the same end-face figure
 * Core/Layout.h documents.
 *
 * Why the corner snap must win: the cursor at (16.5, 5.5, 3.25) is 0.395 cm
 * from the +X-end return that finishes flush with the stretcher's -Y face,
 * 11.1 cm from the other flush choice at that end, and past the 30 cm radius
 * from both -X-end returns. Ranking is raw distance, so the nearest return is
 * Candidates[0] and PlacePiece adopts it.
 *
 * Needs a ticking world: no. Nothing ticks and nothing is solved — the
 * assertions are on the live structure the placement produced.
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

	// 1. The stretcher: a grounded ClayBrick running along X, resting on the ground (centre Z = 3.25, bottom face at 0 — the 2026-09-15 course convention).
	const FPlacementResult Seed = PlacePiece(
		Layout, FVector(0.0, 0.0, 3.25), HalfBrick, ClayBrick, /*bGrounded*/ true, Settings);

	TestEqual(TEXT("seed placement returns handle 0"), Seed.PieceHandle, 0);
	TestEqual(TEXT("seed placement is a Free placement"),
		static_cast<int32>(Seed.Kind), static_cast<int32>(ESnapKind::Free));

	/*
	 * 2. The return: the same brick turned to run along Y, asked for just
	 * off the stretcher's +X end. It must snap to (16.875, 5.625, 3.25) — the
	 * stretcher's end face at 10.75, one 1 cm joint, then the return's own
	 * half-width 5.125 — flush at Y = -5.125 against the stretcher's -Y
	 * width face.
	 */
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

		/*
		 * The assertion the slice exists for. Full-field, because mortar and
		 * perpend are separated only by cohesion (0.9 vs 0.2) and tension
		 * (0.7 vs 0.1) — a partial comparison would accept the answer the
		 * unmigrated three-argument inference gives for a horizontal normal.
		 */
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
