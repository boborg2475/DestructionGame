// Copyright Epic Games, Inc. All Rights Reserved.

#include <limits>

#include "Misc/AutomationTest.h"

#include "Core/Connection.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/SessionToolbar.h"
#include "Core/Structure.h"
#include "Tests/BrickWorldTestSupport.h"
#include "World/BrickActor.h"
#include "World/DestructionStructureSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Build mode UI-1: world-layer placement (BUILD_MODE_PLAN.md). BeginBuild opens an empty live
 * structure; PlaceBuildPiece snaps a piece against it, spawns an ABrickActor and forms real joints.
 * World tests because they spawn actors, but never ticked: assertions are on the structure (the
 * mechanism), never displacement. Grid: a 21.5 x 10.25 x 6.5 brick on 1 cm joints gives 22.5 x
 * 11.25 x 7.5, matching SnapSolverTest.cpp.
 */
namespace BuildPlacePieceTestSupport
{
	// Half-extent of a full 21.5 x 10.25 x 6.5 brick.
	const FVector HalfBrick(10.75, 5.125, 3.25);

	// The running-bond pose one course up from the origin brick, on its +X side.
	const FVector ExpectedRunningBondCentre(11.25, 0.0, 7.5);

	// Half-stagger bed overlap: (21.5 - 11.25) x 10.25 = 105.0625 cm2, spelled out independently.
	constexpr double ExpectedBedAreaSqCm = 105.0625;

	/*
	 * Match all five fields (no operator==), so a sibling profile such as GeneralPurposeMortarPerpend
	 * cannot pass.
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
 * BeginBuild opens an empty structure; the first brick lands as grounded piece 0 with no joint;
 * a second, requested at (11, 0, 7.5), snaps to (11.25, 0, 7.5) with its own actor and one
 * GeneralPurposeMortar bed joint; a solve then reads Grounded and Supported. Never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPlacePieceGrowsALiveStructureTest,
	"DestructionGame.World.BuildMode.PlaceBuildPieceGrowsALiveStructure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildPlacePieceGrowsALiveStructureTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildPlacePieceTestSupport;
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	// Step 1: an empty build structure.
	const int32 StructureId = Subsystem.BeginBuild();

	FStructureBinding* Binding = Subsystem.Find(StructureId);

	TestNotNull(
		FString::Printf(TEXT("BeginBuild should register a live structure, but Find(%d) is null"),
			StructureId),
		Binding);

	if (Binding == nullptr)
	{
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("a just-begun build structure holds no pieces, got %d"),
			Binding->NumPieces()),
		Binding->NumPieces(), 0);

	// Step 2: the first brick lands as piece 0, with no joint.
	const FPieceRef Ref0 =
		Subsystem.PlaceBuildPiece(StructureId, FVector(0.0, 0.0, 0.0), HalfBrick, ClayBrick);

	TestTrue(
		FString::Printf(TEXT("the first placed piece should be ref {%d, 0}, got {%d, %d}"),
			StructureId, Ref0.StructureId, Ref0.PieceIndex),
		Ref0 == FPieceRef{ StructureId, 0 });

	Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr)
	{
		AddError(TEXT("the structure vanished after the first placement"));
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("one placement should grow the structure to 1 piece, got %d"),
			Binding->NumPieces()),
		Binding->NumPieces(), 1);

	ABrickActor* Actor0 = Cast<ABrickActor>(Binding->GetActor(0));
	TestNotNull(
		FString::Printf(TEXT("piece 0 should be backed by a spawned ABrickActor, got %s"),
			*GetNameSafe(Binding->GetActor(0))),
		Actor0);

	// Grounded is derived from the pose (DESIGN §8): the underside at -3.25 cm is within a joint of Z = 0.
	TestTrue(
		TEXT("piece 0's snapped pose rests it on the earth, so the structure must record it grounded"),
		Binding->GetStructure().GetPiece(0).bIsGrounded);

	TestTrue(
		TEXT("piece 0's material must be the ClayBrick profile it was placed with"),
		Binding->GetStructure().GetPiece(0).Material == &ClayBrick);

	TestEqual(
		FString::Printf(TEXT("the lone first piece has nothing to joint to, so 0 connections, got %d"),
			Binding->GetStructure().NumConnections()),
		Binding->GetStructure().NumConnections(), 0);

	// Step 3: a second brick snaps to the running-bond pose and forms one bed joint.
	const FPieceRef Ref1 =
		Subsystem.PlaceBuildPiece(StructureId, FVector(11.0, 0.0, 7.5), HalfBrick, ClayBrick);

	TestTrue(
		FString::Printf(TEXT("the second placed piece should be ref {%d, 1}, got {%d, %d}"),
			StructureId, Ref1.StructureId, Ref1.PieceIndex),
		Ref1 == FPieceRef{ StructureId, 1 });

	Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr)
	{
		AddError(TEXT("the structure vanished after the second placement"));
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("two placements should grow the structure to 2 pieces, got %d"),
			Binding->NumPieces()),
		Binding->NumPieces(), 2);

	ABrickActor* Actor1 = Cast<ABrickActor>(Binding->GetActor(1));
	TestNotNull(
		FString::Printf(TEXT("piece 1 should be backed by a spawned ABrickActor, got %s"),
			*GetNameSafe(Binding->GetActor(1))),
		Actor1);

	TestTrue(
		TEXT("piece 1's actor must be a DISTINCT actor from piece 0's"),
		Actor1 != nullptr && Actor1 != Actor0);

	// The box sits at the snap pose, not the requested one.
	const FVector PlacedCentre = Binding->GetBinding(1).Box.CentreCm;
	TestTrue(
		FString::Printf(
			TEXT("piece 1 should be snapped to the running-bond pose (11.25, 0, 7.5), got (%g, %g, %g)"),
			PlacedCentre.X, PlacedCentre.Y, PlacedCentre.Z),
		PlacedCentre.Equals(ExpectedRunningBondCentre, KINDA_SMALL_NUMBER));

	TestEqual(
		FString::Printf(TEXT("the running-bond placement should form exactly one bed joint, got %d"),
			Binding->GetStructure().NumConnections()),
		Binding->GetStructure().NumConnections(), 1);

	if (Binding->GetStructure().NumConnections() == 1)
	{
		const FConnection& Bed = Binding->GetStructure().GetConnection(0);

		const bool bLinksZeroAndOne =
			(Bed.PieceA == 0 && Bed.PieceB == 1) || (Bed.PieceA == 1 && Bed.PieceB == 0);
		TestTrue(
			FString::Printf(TEXT("the bed joint must link handles 0 and 1, got {%d, %d}"),
				Bed.PieceA, Bed.PieceB),
			bLinksZeroAndOne);

		TestEqual(
			FString::Printf(TEXT("a bed joint's normal is vertical, |Z| should be 1, got %g"),
				FMath::Abs(Bed.InterfaceNormal.Z)),
			FMath::Abs(Bed.InterfaceNormal.Z), 1.0, 1.0e-9);

		TestEqual(
			FString::Printf(TEXT("the bed overlap area should be 105.0625 cm2, got %g"),
				Bed.InterfaceAreaSqCm),
			Bed.InterfaceAreaSqCm, ExpectedBedAreaSqCm, 1.0e-6);

		// The strong bed mortar, not the perpend.
		CheckProfileIdentity(
			*this,
			TEXT("bed joint profile == GeneralPurposeMortar: "),
			Bed.Strength,
			GeneralPurposeMortar);
	}

	// Step 4: a solve reads the foot Grounded and the upper brick Supported by the bed joint.
	Binding->SolveLoads();

	TestTrue(
		FString::Printf(TEXT("piece 0 rests on the earth, so it should solve Grounded, got state %d"),
			static_cast<int32>(Binding->GetStructure().GetPieceSupport(0))),
		Binding->GetStructure().GetPieceSupport(0) == EPieceSupport::Grounded);

	TestTrue(
		FString::Printf(TEXT("piece 1 is held by the bed joint, so it should solve Supported, got state %d"),
			static_cast<int32>(Binding->GetStructure().GetPieceSupport(1))),
		Binding->GetStructure().GetPieceSupport(1) == EPieceSupport::Supported);

	return true;
}

/**
 * Build mode UI-2: PreviewBuildPiece reports the snap PlaceBuildPiece would make (BrickNextCourse
 * at (11.25, 0, 7.5), one joint) without changing anything: still one piece, no connection, no
 * second actor. A following PlaceBuildPiece at the same pose matches the preview exactly. An
 * unknown structure id previews invalid. Never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPreviewBuildPieceIsNonMutatingAndPredictsTheCommitTest,
	"DestructionGame.World.BuildMode.PreviewBuildPieceIsNonMutatingAndPredictsTheCommit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPreviewBuildPieceIsNonMutatingAndPredictsTheCommitTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildPlacePieceTestSupport;
	using namespace DestructionProfiles;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	// One grounded brick for the preview to snap to.
	const int32 StructureId = Subsystem.BeginBuild();

	Subsystem.PlaceBuildPiece(StructureId, FVector(0.0, 0.0, 0.0), HalfBrick, ClayBrick);

	FStructureBinding* Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr)
	{
		AddError(TEXT("the structure vanished after seeding the first piece"));
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("precondition: the seeded structure holds exactly 1 piece, got %d"),
			Binding->NumPieces()),
		Binding->NumPieces(), 1);

	// Off-grid on purpose, so the snap must move it.
	const FVector RequestedCentre(11.0, 0.0, 7.5);

	const FBuildPreview Preview =
		Subsystem.PreviewBuildPiece(StructureId, RequestedCentre, HalfBrick, ClayBrick);

	TestTrue(
		TEXT("a preview against a known structure is valid"),
		Preview.bValid);

	TestTrue(
		FString::Printf(TEXT("the +X next-course preview snaps to BrickNextCourse, got kind %d"),
			static_cast<int32>(Preview.Kind)),
		Preview.Kind == BuildMode::ESnapKind::BrickNextCourse);

	TestTrue(
		FString::Printf(
			TEXT("the preview centre is the running-bond snap (11.25, 0, 7.5), got (%g, %g, %g)"),
			Preview.CentreCm.X, Preview.CentreCm.Y, Preview.CentreCm.Z),
		Preview.CentreCm.Equals(ExpectedRunningBondCentre, KINDA_SMALL_NUMBER));

	TestEqual(
		FString::Printf(TEXT("the preview would form exactly one bed joint, got %d"),
			Preview.JointCount),
		Preview.JointCount, 1);

	// Non-mutation: still one piece, no connection, no second actor.
	Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr)
	{
		AddError(TEXT("the structure vanished across the preview call"));
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("preview must NOT add a piece: still 1 piece, got %d"),
			Binding->NumPieces()),
		Binding->NumPieces(), 1);

	TestEqual(
		FString::Printf(TEXT("preview must NOT form a connection: still 0 connections, got %d"),
			Binding->GetStructure().NumConnections()),
		Binding->GetStructure().NumConnections(), 0);

	TestNull(
		FString::Printf(TEXT("preview must NOT spawn a second actor: GetActor(1) should be null, got %s"),
			*GetNameSafe(Binding->GetActor(1))),
		Binding->GetActor(1));

	// Consistency: a commit at the same pose matches the preview.
	const FPieceRef Placed =
		Subsystem.PlaceBuildPiece(StructureId, RequestedCentre, HalfBrick, ClayBrick);

	TestTrue(
		FString::Printf(TEXT("the committed piece should be ref {%d, 1}, got {%d, %d}"),
			StructureId, Placed.StructureId, Placed.PieceIndex),
		Placed == FPieceRef{ StructureId, 1 });

	Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr)
	{
		AddError(TEXT("the structure vanished after the confirming placement"));
		return true;
	}

	const FVector PlacedCentre = Binding->GetBinding(1).Box.CentreCm;
	TestTrue(
		FString::Printf(
			TEXT("the committed box lands at the previewed centre (%g, %g, %g), got (%g, %g, %g)"),
			Preview.CentreCm.X, Preview.CentreCm.Y, Preview.CentreCm.Z,
			PlacedCentre.X, PlacedCentre.Y, PlacedCentre.Z),
		PlacedCentre.Equals(Preview.CentreCm, KINDA_SMALL_NUMBER));

	TestEqual(
		FString::Printf(
			TEXT("the commit forms exactly the previewed joint count %d, got %d connections"),
			Preview.JointCount, Binding->GetStructure().NumConnections()),
		Binding->GetStructure().NumConnections(), Preview.JointCount);

	// Fail closed: an unknown structure id previews nothing.
	const FBuildPreview UnknownPreview =
		Subsystem.PreviewBuildPiece(9999, RequestedCentre, HalfBrick, ClayBrick);

	TestFalse(
		TEXT("a preview against an unknown structure id is invalid"),
		UnknownPreview.bValid);

	return true;
}

/**
 * Grounded is derived from the placed pose, not a caller argument (DESIGN §8, 2026-09-15): the
 * snap can lift a piece a course up, and a wrongly grounded piece can never fall. Rule: the bottom
 * face (centre Z - half height) is `<= 1.0` cm, one joint thickness. Written `<=`, not the house
 * `!(x > y)`, because here grounded is the fail-open side and `<=` is false for a NaN (pinned by
 * NonFinitePoseNeverCommitsGrounded).
 *
 * Rows at centre 4.25 (bottom 1.0, grounded) and 4.5 (bottom 1.25, not) catch a `<` or a retuned
 * tolerance. Each row uses its own empty build so the pose is placed as requested. Never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlaceBuildPieceDerivesGroundedFromPoseTest,
	"DestructionGame.World.BuildMode.PlaceBuildPieceDerivesGroundedFromPose",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPlaceBuildPieceDerivesGroundedFromPoseTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildPlacePieceTestSupport;
	using namespace DestructionProfiles;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	struct FGroundedCase
	{
		const TCHAR* Description;
		double CentreZCm;
		double BottomFaceZCm;
		bool bExpectGrounded;
	};

	const FGroundedCase Cases[] = {
		{ TEXT("the rests-on-the-ground course 0 centre"),      3.25,  0.0,   true  },
		{ TEXT("the legacy half-buried course 0 centre"),       0.0,  -3.25,  true  },
		{ TEXT("a bottom face exactly one joint up (the edge)"), 4.25, 1.0,   true  },
		{ TEXT("a bottom face a quarter centimetre past it"),   4.5,   1.25,  false },
		{ TEXT("a brick bedded a whole course in the air"),     10.75, 7.5,   false },
	};

	for (const FGroundedCase& Case : Cases)
	{
		const int32 StructureId = Subsystem.BeginBuild();

		const FVector RequestedCentre(0.0, 0.0, Case.CentreZCm);

		const FPieceRef Ref =
			Subsystem.PlaceBuildPiece(StructureId, RequestedCentre, HalfBrick, ClayBrick);

		FStructureBinding* Binding = Subsystem.Find(StructureId);

		if (Binding == nullptr || Ref.PieceIndex != 0)
		{
			AddError(FString::Printf(
				TEXT("fixture: %s did not place a first piece (ref {%d, %d})"),
				Case.Description, Ref.StructureId, Ref.PieceIndex));
			continue;
		}

		// The lone piece lands at the requested pose, so the row tests the Z it names.
		const FVector PlacedCentre = Binding->GetBinding(0).Box.CentreCm;

		TestTrue(
			FString::Printf(
				TEXT("fixture: %s should land at its requested pose (0, 0, %g), got (%g, %g, %g)"),
				Case.Description, Case.CentreZCm, PlacedCentre.X, PlacedCentre.Y, PlacedCentre.Z),
			PlacedCentre.Equals(RequestedCentre, KINDA_SMALL_NUMBER));

		TestEqual(
			FString::Printf(
				TEXT("%s: centre Z %g minus the 3.25 half-height puts the bottom face at %g, which is %s one 1 cm joint of the ground, so the committed piece must read %s"),
				Case.Description, Case.CentreZCm, Case.BottomFaceZCm,
				Case.bExpectGrounded ? TEXT("WITHIN") : TEXT("BEYOND"),
				Case.bExpectGrounded ? TEXT("grounded") : TEXT("NOT grounded")),
			Binding->GetStructure().GetPiece(0).bIsGrounded, Case.bExpectGrounded);
	}

	return true;
}

/**
 * EPlacementMode::Free places at the requested pose with no joints, however close a snap is. The
 * same cursor (11, 0, 10) is tested in both modes: Snap moves it 0.79 cm to the next-course pose
 * (11.25, 0, 10.75) with one bed joint; Free leaves it in place with none. Testing both means a
 * production that ignored the mode fails one leg. The Free commit agrees with its preview and
 * adds no connection. Never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPreviewBuildPieceFreeIgnoresSnapsTest,
	"DestructionGame.World.BuildMode.PreviewBuildPieceFreeIgnoresSnaps",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPreviewBuildPieceFreeIgnoresSnapsTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildPlacePieceTestSupport;
	using namespace DestructionProfiles;
	using namespace DestructionSession;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	// One seed brick resting on the ground (centre 3.25, underside 0).
	const int32 StructureId = Subsystem.BeginBuild();

	Subsystem.PlaceBuildPiece(StructureId, FVector(0.0, 0.0, 3.25), HalfBrick, ClayBrick);

	FStructureBinding* Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr)
	{
		AddError(TEXT("the structure vanished after seeding the first piece"));
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("fixture: the seeded structure holds exactly 1 piece, got %d"),
			Binding->NumPieces()),
		Binding->NumPieces(), 1);

	const FVector FreeCursorCm(11.0, 0.0, 10.0);
	const FVector NextCourseCm(11.25, 0.0, 10.75);

	// Snap: the nearest candidate wins.
	const FBuildPreview Snapped =
		Subsystem.PreviewBuildPiece(StructureId, FreeCursorCm, HalfBrick, ClayBrick, EPlacementMode::Snap);

	TestTrue(
		FString::Printf(TEXT("the Snap preview must take the next-course candidate, got kind %d"),
			static_cast<int32>(Snapped.Kind)),
		Snapped.Kind == BuildMode::ESnapKind::BrickNextCourse);

	TestTrue(
		FString::Printf(
			TEXT("the Snap preview centres on (11.25, 0, 10.75), got (%g, %g, %g)"),
			Snapped.CentreCm.X, Snapped.CentreCm.Y, Snapped.CentreCm.Z),
		Snapped.CentreCm.Equals(NextCourseCm, KINDA_SMALL_NUMBER));

	TestEqual(
		FString::Printf(TEXT("the Snap preview would form one bed joint, got %d"), Snapped.JointCount),
		Snapped.JointCount, 1);

	// Free: the same cursor, placed as is, with no joints.
	const FBuildPreview Free =
		Subsystem.PreviewBuildPiece(StructureId, FreeCursorCm, HalfBrick, ClayBrick, EPlacementMode::Free);

	TestTrue(
		FString::Printf(TEXT("the Free preview must be the Free candidate, got kind %d"),
			static_cast<int32>(Free.Kind)),
		Free.Kind == BuildMode::ESnapKind::Free);

	TestTrue(
		FString::Printf(
			TEXT("the Free preview centres on the cursor (11, 0, 10) EXACTLY, got (%g, %g, %g)"),
			Free.CentreCm.X, Free.CentreCm.Y, Free.CentreCm.Z),
		Free.CentreCm.Equals(FreeCursorCm, KINDA_SMALL_NUMBER));

	TestEqual(
		FString::Printf(TEXT("a Free placement bonds to nothing, so 0 joints, got %d"), Free.JointCount),
		Free.JointCount, 0);

	TestFalse(
		TEXT("the Free pose's bottom face sits 6.75 cm up, so the preview must not read it grounded"),
		Free.bGrounded);

	// The commit agrees: box at the cursor, no new connection.
	const FPieceRef Placed = Subsystem.PlaceBuildPiece(
		StructureId, FreeCursorCm, HalfBrick, ClayBrick, EPlacementMode::Free);

	TestTrue(
		FString::Printf(TEXT("the Free commit should be ref {%d, 1}, got {%d, %d}"),
			StructureId, Placed.StructureId, Placed.PieceIndex),
		Placed == FPieceRef{ StructureId, 1 });

	Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr)
	{
		AddError(TEXT("the structure vanished after the Free placement"));
		return true;
	}

	const FVector PlacedCentre = Binding->GetBinding(1).Box.CentreCm;

	TestTrue(
		FString::Printf(
			TEXT("the Free commit lands its box on the cursor (11, 0, 10), got (%g, %g, %g)"),
			PlacedCentre.X, PlacedCentre.Y, PlacedCentre.Z),
		PlacedCentre.Equals(FreeCursorCm, KINDA_SMALL_NUMBER));

	TestEqual(
		FString::Printf(TEXT("a Free commit forms no joint, so the structure still holds %d connections"),
			Binding->GetStructure().NumConnections()),
		Binding->GetStructure().NumConnections(), 0);

	TestFalse(
		TEXT("the Free-committed piece is in the air, so the structure must not record it grounded"),
		Binding->GetStructure().GetPiece(1).bIsGrounded);

	return true;
}

/**
 * A non-finite pose never reads grounded, in either the preview or the commit. Grounded is the
 * fail-open side (a grounded piece can never fall), so the rule is `<=`, false for a NaN, not the
 * house `!(x > y)`, which would be true.
 *
 * Its first run found that PlaceBuildPiece spawned an actor at a NaN transform (engine ensures).
 * The fix is a guard in ComputeBuildPlacement that refuses a non-finite chosen centre before any
 * spawn; that guard is what this test now bites on. With it, the `<=` polarity is defence in depth
 * (swapping it leaves the group green, measured 2026-09-15). The commit leg asserts only that no
 * landed piece reads grounded, since refusing the pose is AddPiece's guarantee. Free placement on
 * an empty build so the pose reaches the rule unchanged. NaN and +Inf rows (the guard checks
 * IsFinite, not just ContainsNaN). Never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildModeNonFinitePoseNeverCommitsGroundedTest,
	"DestructionGame.World.BuildMode.NonFinitePoseNeverCommitsGrounded",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildModeNonFinitePoseNeverCommitsGroundedTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildPlacePieceTestSupport;
	using namespace DestructionProfiles;
	using namespace DestructionSession;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	struct FNonFiniteCase
	{
		const TCHAR* Description;
		double CentreZCm;
	};

	const FNonFiniteCase Cases[] = {
		{ TEXT("a NaN Z (the row that discriminates <= from !(> ))"),
			std::numeric_limits<double>::quiet_NaN() },
		{ TEXT("a +infinity Z"), std::numeric_limits<double>::infinity() },
	};

	for (const FNonFiniteCase& Case : Cases)
	{
		// Its own empty build, so the Free candidate is exactly the requested pose.
		const int32 StructureId = Subsystem.BeginBuild();

		const FVector RequestedCentre(50.0, 0.0, Case.CentreZCm);

		const FBuildPreview Preview = Subsystem.PreviewBuildPiece(
			StructureId, RequestedCentre, HalfBrick, ClayBrick, EPlacementMode::Free);

		TestFalse(
			FString::Printf(
				TEXT("%s: the preview must NOT read grounded — a non-finite bottom face is not on the earth, and a grounded piece can never fall"),
				Case.Description),
			Preview.bGrounded);

		const FPieceRef Ref = Subsystem.PlaceBuildPiece(
			StructureId, RequestedCentre, HalfBrick, ClayBrick, EPlacementMode::Free);

		const FStructureBinding* Binding = Subsystem.Find(StructureId);
		if (Binding == nullptr)
		{
			AddError(FString::Printf(
				TEXT("fixture: %s lost its structure across the commit"), Case.Description));
			continue;
		}

		// No piece may read grounded; covers both a refusal (zero pieces) and a landed piece.
		for (int32 PieceIndex = 0; PieceIndex < Binding->NumPieces(); ++PieceIndex)
		{
			TestFalse(
				FString::Printf(
					TEXT("%s: it committed piece %d (ref index %d) — a piece landed from a non-finite pose must NOT be recorded grounded"),
					Case.Description, PieceIndex, Ref.PieceIndex),
				Binding->GetStructure().GetPiece(PieceIndex).bIsGrounded);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
