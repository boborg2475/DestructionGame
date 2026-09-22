// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceActions.h"
#include "Core/PieceMenu.h"
#include "DestructionGamePlayerController.h"
#include "Tests/BrickWorldTestSupport.h"
#include "Tests/StaircaseWallTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Integration tests: the player's click path end to end. Two rules for every test here:
 *   1. Enter only through InspectAlongRay and ChoosePieceMenuRow, never an internal step such as
 *      SolveAndPush or RemovePiece. The original bug lived in the join between two correct links.
 *   2. Assert a physical outcome (a brick moved, or did not, after fixed simulated time), not
 *      model state alone.
 * Graph arithmetic belongs in the world-free suite; a missing call between correct halves belongs
 * here. Group by world configuration, since each test pays for a world.
 *
 * Uniquely named namespace because unity builds merge translation units. The world harness lives
 * in Tests/BrickWorldTestSupport.h.
 */
namespace StructureIntegrationTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/** Ray half-length along Y; walls are centred on Y = 0 and 10.25 cm deep, so this crosses them. */
	constexpr double IntegrationReachCm = 100.0;

	/**
	 * kg x 980 cm/s2 is already the weight in Unreal force units; applying DESIGN.md §3's
	 * 1 N = 100 uu again is the 100x error. Stated here rather than imported from production.
	 */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** Bricks stop within a third of a second; a full second is margin, not a settle poll. */
	constexpr double FallSeconds = 1.0;

	/** Watch time for a wall that must not move. Half a second of free fall is 122.5 cm, far past DriftToleranceCm. */
	constexpr double StandSeconds = 0.5;

	const TCHAR* IntegrationSupportName(EPieceSupport Support)
	{
		switch (Support)
		{
		case EPieceSupport::Grounded:  return TEXT("Grounded");
		case EPieceSupport::Supported: return TEXT("Supported");
		case EPieceSupport::Stranded:  return TEXT("Stranded");
		default:                       return TEXT("Falling");
		}
	}

	/** The action row with this label, so nothing hard-codes a table position. */
	const FPieceAction* FindIntegrationAction(const TCHAR* Label)
	{
		for (const FPieceAction& Action : AllPieceActions())
		{
			if (Action.Label != nullptr && FCString::Strcmp(Action.Label, Label) == 0)
			{
				return &Action;
			}
		}

		return nullptr;
	}

	/** The joint between two pieces, or INDEX_NONE. Shared implementation in Tests/StaircaseWallTestSupport.h. */
	int32 FindIntegrationJoint(const FStructure& Structure, int32 FirstPiece, int32 SecondPiece)
	{
		return StaircaseWallTestSupport::JointBetweenPieces(Structure, FirstPiece, SecondPiece);
	}

	/**
	 * Logs each piece's graph support, binding release state and body physics state. The original
	 * bug read "Falling / held / kinematic": the graph knew, the world was never told.
	 */
	void ReportIntegrationState(
		FAutomationTestBase& Test,
		const FStructureBinding& Binding,
		const TArray<ABrickActor*>& Bricks,
		const TCHAR* When)
	{
		FString Line;

		for (int32 Piece = 0; Piece < Binding.NumPieces(); ++Piece)
		{
			const UStaticMeshComponent* const Mesh =
				IsValid(Bricks[Piece]) ? Bricks[Piece]->GetMesh() : nullptr;

			Line += FString::Printf(
				TEXT("%s%d=%s/%s/%s"),
				Piece == 0 ? TEXT("") : TEXT(", "),
				Piece,
				IntegrationSupportName(Binding.GetStructure().GetPieceSupport(Piece)),
				Binding.IsReleased(Piece) ? TEXT("released") : TEXT("held"),
				Mesh == nullptr
					? TEXT("no actor")
					: (Mesh->IsSimulatingPhysics() ? TEXT("simulating") : TEXT("kinematic")));
		}

		Test.AddInfo(FString::Printf(TEXT("state %s (support/binding/body): %s"), When, *Line));
	}

	/** The menu rows as text, for failure messages. */
	FString DescribeIntegrationRows(TArrayView<const FPieceMenuRow> Rows)
	{
		if (Rows.Num() == 0)
		{
			return TEXT("<empty>");
		}

		FString Line;

		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			Line += FString::Printf(
				TEXT("%s'%s'->{%d,%d}"),
				Index == 0 ? TEXT("") : TEXT(", "),
				*Rows[Index].Label,
				Rows[Index].Ref.StructureId,
				Rows[Index].Ref.PieceIndex);
		}

		return Line;
	}

	/**
	 * Points a ray at each listed brick, then chooses Delete once for the whole selection. The only
	 * way tests here change a wall; untested in headless runs are cursor deprojection and the button.
	 * The Delete row is found by action pointer after every click, since the menu is rebuilt.
	 */
	bool InspectAndChooseDelete(
		FAutomationTestBase& Test,
		ADestructionGamePlayerController& Controller,
		const FPieceAction& Delete,
		int32 StructureId,
		const TArray<FPieceBox>& Boxes,
		const TArray<int32>& Pieces)
	{
		int32 DeleteRow = INDEX_NONE;

		for (const int32 ExpectedPiece : Pieces)
		{
			const FPieceBox& Box = Boxes[ExpectedPiece];

			const TArray<FPieceMenuRow> Rows = Controller.InspectAlongRay(
				FVector(Box.CentreCm.X, Box.CentreCm.Y - IntegrationReachCm, Box.CentreCm.Z),
				FVector(Box.CentreCm.X, Box.CentreCm.Y + IntegrationReachCm, Box.CentreCm.Z));

			DeleteRow = INDEX_NONE;

			for (int32 Index = 0; Index < Rows.Num(); ++Index)
			{
				if (Rows[Index].Action == &Delete)
				{
					DeleteRow = Index;
					break;
				}
			}

			if (DeleteRow == INDEX_NONE)
			{
				Test.AddError(FString::Printf(
					TEXT("inspecting piece %d at (%g, %g, %g) offered no Delete row, it offered [%s]"),
					ExpectedPiece, Box.CentreCm.X, Box.CentreCm.Y, Box.CentreCm.Z,
					*DescribeIntegrationRows(Rows)));

				return false;
			}

			// The row must name the brick just picked; otherwise the wrong brick is deleted with every count agreeing.
			Test.TestTrue(
				*FString::Printf(
					TEXT("the Delete row offered after picking piece %d should name {%d,%d}, it names {%d,%d}"),
					ExpectedPiece, StructureId, ExpectedPiece,
					Rows[DeleteRow].Ref.StructureId, Rows[DeleteRow].Ref.PieceIndex),
				Rows[DeleteRow].Ref.StructureId == StructureId
					&& Rows[DeleteRow].Ref.PieceIndex == ExpectedPiece);
		}

		if (DeleteRow == INDEX_NONE)
		{
			Test.AddError(TEXT("InspectAndChooseDelete was asked to pick no bricks at all"));

			return false;
		}

		// One choice for the whole selection.
		const bool bChose = Controller.ChoosePieceMenuRow(DeleteRow);

		Test.TestTrue(
			*FString::Printf(TEXT("choosing Delete on %d picked brick(s) should report that it committed"),
				Pieces.Num()),
			bChose);

		return bChose;
	}

	// The narrow-waist wall is NarrowWaistWallSpec in Tests/BrickWorldTestSupport.h.

	/**
	 * A wall that survives losing a brick (redistribution control). 5 + 4 + 5 + 4 = 18 pieces:
	 *
	 *      course 3      [14][15][16][17]         X centres 11.25, 33.75, 56.25, 78.75
	 *      course 2    [ 9][10][11][12][13]       X centres 0, 22.5, 45, 67.5, 90
	 *      course 1      [ 5][ 6][ 7][ 8]         X centres 11.25, 33.75, 56.25, 78.75
	 *      course 0    [ 0][ 1][ 2][ 3][ 4]       grounded
	 *
	 * Piece 6 is removed: 10 and 11 each keep a bed joint on another course-1 brick, so nothing
	 * loses its path to the ground.
	 */
	FRunningBondSpec WideWallSpec()
	{
		FRunningBondSpec Spec;
		Spec.BrickSizeCm = FVector(21.5, 10.25, 6.5);
		Spec.JointThicknessCm = 1.0;
		Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
		Spec.CoursesHigh = 4;
		Spec.BricksPerCourse = 5;
		Spec.End = EWallEnd::Ragged;
		Spec.Strength = GeneralPurposeMortar;
		return Spec;
	}

	constexpr int32 WideWallPieceCount = 18;

	/** The interior course-1 brick that comes out, and the two course-2 bricks resting on it. */
	constexpr int32 WideWallRemovedPiece = 6;
	constexpr int32 WideWallLeftNeighbour = 5;
	constexpr int32 WideWallRightNeighbour = 7;
	constexpr int32 WideWallLeftCarrier = 10;
	constexpr int32 WideWallRightCarrier = 11;

	/**
	 * Two bricks the wide wall can lose together in one batch. Each carrier keeps one support:
	 * 10 keeps 5, 11 keeps 7, 16 keeps 11, 17 keeps 13.
	 */
	const TArray<int32> WideWallSurvivableBatch = { 6, 12 };

	/** The whole grounded course in one batch; everything left has no path to the ground and must fall. */
	const TArray<int32> WideWallGroundCourse = { 0, 1, 2, 3, 4 };

	/** Tolerance on joint forces of a few thousand uu; the expected values are exact. */
	constexpr double JointForceToleranceUnrealUnits = 0.01;

	// The staircase wall (from a real screenshot) lives in Tests/StaircaseWallTestSupport.h.
}

/**
 * Deleting a brick through the click path makes the bricks it held up fall, and leaves the rest
 * exactly in place. Catches the bug where the commit re-solved the model but never pushed the
 * result to the world: other tests covered the click path and the fall separately, never together
 * (DESIGN.md §4, mechanism vs. outcome).
 *
 * Both halves matter: "orphans fell" alone passes if everything fell through the floor, "nothing
 * else moved" alone passes if nothing moved. IsReleased (binding) and IsSimulatingPhysics (body)
 * are asserted alongside, as they fail for different reasons. No joint is claimed to break.
 * Ticks a fixed second, never a settle poll.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureIntegrationPlayerJourneyTest,
	"DestructionGame.Integration.DeletingABrickDropsTheBricksItOrphaned",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureIntegrationPlayerJourneyTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;
	using namespace StructureIntegrationTestSupport;

	const FPieceAction* const Delete = FindIntegrationAction(TEXT("Delete"));

	if (Delete == nullptr)
	{
		AddError(TEXT("fixture: the action table must contain a row labelled 'Delete'"));
		return true;
	}

	/*
	 * Three courses: the waist is piece 2, the orphans 3 and 4. They share a head joint but neither
	 * reaches the ground, so both read Falling rather than a cycle. 0 and 1 are grounded.
	 */
	const FRunningBondSpec Spec = NarrowWaistWallSpec(3);

	constexpr int32 PieceCount = 5;
	constexpr int32 WaistPiece = 2;
	constexpr bool bOrphaned[PieceCount] = { false, false, false, true, true };

	// Aim points come from an independent layout, not from what the subsystem spawned.
	FBrickLayout Reference;

	TestTrue(TEXT("fixture: RunningBond should lay the reference wall"), RunningBond(Spec, Reference));

	if (Reference.Boxes.Num() != PieceCount)
	{
		AddError(FString::Printf(TEXT("fixture: a ragged 3 x 2 wall should be %d pieces, got %d"),
			PieceCount, Reference.Boxes.Num()));

		return true;
	}

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	const int32 StructureId = TestWorld.Subsystem->BuildRunningBond(Spec);
	FStructureBinding* const Binding = TestWorld.Subsystem->Find(StructureId);

	TestNotNull(
		*FString::Printf(TEXT("fixture: BuildRunningBond returned %d and Find should hand back its binding"),
			StructureId),
		Binding);

	if (Binding == nullptr || Binding->NumPieces() != PieceCount)
	{
		TestWorld.End();
		return true;
	}

	TArray<ABrickActor*> Bricks;
	TArray<FVector> LaidAt;

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		ABrickActor* Brick = BrickAt(*this, *Binding, Piece);

		if (Brick == nullptr)
		{
			TestWorld.End();
			return true;
		}

		Bricks.Add(Brick);
		LaidAt.Add(Brick->GetActorLocation());
	}

	// Same state the game mode sets on BeginPlay; the wall must stand on its own.
	TestEqual(
		TEXT("fixture: the wall as built should stand, so starting it up releases nothing"),
		TestWorld.Subsystem->SolveAndPush(StructureId), 0);

	ReportIntegrationState(*this, *Binding, Bricks, TEXT("as built"));

	for (int32 Piece = 0; Piece < PieceCount; ++Piece)
	{
		// Positive control: every piece is held up before the deletion.
		const EPieceSupport Support = Binding->GetStructure().GetPieceSupport(Piece);

		TestTrue(
			*FString::Printf(TEXT("fixture: piece %d should be held up before the waist goes, the solver says %s"),
				Piece, IntegrationSupportName(Support)),
			Support == EPieceSupport::Grounded || Support == EPieceSupport::Supported);

		TestTrue(
			*FString::Printf(TEXT("fixture: brick %d should be kinematic in a standing wall"), Piece),
			Bricks[Piece]->GetMesh() != nullptr && !Bricks[Piece]->GetMesh()->IsSimulatingPhysics());
	}

	ADestructionGamePlayerController* const Controller =
		SpawnControllerWithLocalPlayer(*this, TestWorld.World);

	if (Controller == nullptr)
	{
		TestWorld.End();
		return true;
	}

	ABrickActor* const WaistBrick = Bricks[WaistPiece];

	if (!InspectAndChooseDelete(
			*this, *Controller, *Delete, StructureId, Reference.Boxes, { WaistPiece }))
	{
		TestWorld.End();
		return true;
	}

	TestTrue(
		FString::Printf(TEXT("the chosen brick's actor must have left the world, it is %s"),
			IsValid(WaistBrick) ? TEXT("still valid") : TEXT("gone")),
		!IsValid(WaistBrick));

	Bricks[WaistPiece] = nullptr;

	ReportIntegrationState(*this, *Binding, Bricks, TEXT("straight after the choice"));

	/*
	 * Model, binding, body in order: a wrong support means no re-solve; a wrong release or physics
	 * flag means no push.
	 */
	for (int32 Piece = 0; Piece < PieceCount; ++Piece)
	{
		if (Piece == WaistPiece)
		{
			continue;
		}

		const bool bExpected = bOrphaned[Piece];
		const EPieceSupport Support = Binding->GetStructure().GetPieceSupport(Piece);

		TestTrue(
			*FString::Printf(TEXT("piece %d should%s read Falling once the waist has gone, the solver says %s"),
				Piece, bExpected ? TEXT("") : TEXT(" not"), IntegrationSupportName(Support)),
			(Support == EPieceSupport::Falling) == bExpected);

		TestTrue(
			*FString::Printf(TEXT("piece %d should%s be released by the commit, IsReleased reports %s"),
				Piece, bExpected ? TEXT("") : TEXT(" not"),
				Binding->IsReleased(Piece) ? TEXT("true") : TEXT("false")),
			Binding->IsReleased(Piece) == bExpected);

		const bool bSimulating =
			Bricks[Piece]->GetMesh() != nullptr && Bricks[Piece]->GetMesh()->IsSimulatingPhysics();

		TestTrue(
			*FString::Printf(TEXT("brick %d should%s be simulating physics once the waist has gone, it is %s"),
				Piece, bExpected ? TEXT("") : TEXT(" not"),
				bSimulating ? TEXT("simulating") : TEXT("kinematic")),
			bSimulating == bExpected);
	}

	/*
	 * Course pitch is 7.5 cm, so pieces 3 and 4 drop from Z = 15 onto course 0's top at Z = 6.5:
	 * 8.5 cm, against a 5 cm threshold. That fall takes 0.13 s.
	 */
	TestWorld.TickSeconds(FallSeconds);

	ReportIntegrationState(*this, *Binding, Bricks, TEXT("after one second of gravity"));

	for (int32 Piece = 0; Piece < PieceCount; ++Piece)
	{
		if (Piece == WaistPiece)
		{
			continue;
		}

		const FVector NowAt = Bricks[Piece]->GetActorLocation();

		const double FellCm = LaidAt[Piece].Z - NowAt.Z;
		const double MovedCm = FVector::Dist(NowAt, LaidAt[Piece]);

		if (bOrphaned[Piece])
		{
			AddInfo(FString::Printf(
				TEXT("orphaned brick %d fell %.3f cm in one second, from Z %.3f to Z %.3f (the bond predicts 8.5 cm)"),
				Piece, FellCm, LaidAt[Piece].Z, NowAt.Z));

			TestTrue(
				*FString::Printf(
					TEXT("orphaned brick %d should have fallen more than %.1f cm in a second, it dropped %.3f cm"),
					Piece, FallenAtLeastCm, FellCm),
				FellCm > FallenAtLeastCm);

			// Landed on the wall, not through the world.
			TestTrue(
				*FString::Printf(TEXT("orphaned brick %d should have come to rest above the floor at Z %g, it is at Z %.3f"),
					Piece, FloorTopZCm, NowAt.Z),
				NowAt.Z > FloorTopZCm);
		}
		else
		{
			// Without this half, everything falling through the floor would pass.
			TestTrue(
				*FString::Printf(TEXT("brick %d is still held up and must not have moved, it drifted %.6f cm"),
					Piece, MovedCm),
				MovedCm < DriftToleranceCm);

			TestTrue(
				*FString::Printf(TEXT("brick %d is still held up and must still be kinematic"), Piece),
				Bricks[Piece]->GetMesh() != nullptr && !Bricks[Piece]->GetMesh()->IsSimulatingPhysics());
		}
	}

	TestWorld.End();

	return true;
}

/**
 * Pulling bricks through the click path: the wall stands after the first grounded brick goes and
 * falls after the second (DESIGN.md §4, "falls at the predicted number"). Piece 2's course is one
 * brick wide, so losing both 0 and 1 leaves it no joint at all. The standing half is the control.
 *
 * No piece may be Stranded (a solver limitation, not a lost support). No joint is claimed to
 * break: this wall sits at about 0.005 of capacity, so this is loss of support, not strength.
 * Fixed-step ticks: half a second standing, one second falling.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureIntegrationCollapseTest,
	"DestructionGame.Integration.PullingSupportBringsTheWallDown",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureIntegrationCollapseTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;
	using namespace StructureIntegrationTestSupport;

	const FPieceAction* const Delete = FindIntegrationAction(TEXT("Delete"));

	if (Delete == nullptr)
	{
		AddError(TEXT("fixture: the action table must contain a row labelled 'Delete'"));
		return true;
	}

	// Four courses, so a piece sits above the pair on the waist.
	const FRunningBondSpec Spec = NarrowWaistWallSpec(4);

	constexpr int32 PieceCount = 6;

	// The wall survives the first pull and not the second.
	constexpr int32 FirstPulled = 0;
	constexpr int32 LastStraw = 1;

	FBrickLayout Reference;

	TestTrue(TEXT("fixture: RunningBond should lay the reference wall"), RunningBond(Spec, Reference));

	if (Reference.Boxes.Num() != PieceCount)
	{
		AddError(FString::Printf(TEXT("fixture: a ragged 4 x 2 wall should be %d pieces, got %d"),
			PieceCount, Reference.Boxes.Num()));

		return true;
	}

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	const int32 StructureId = TestWorld.Subsystem->BuildRunningBond(Spec);
	FStructureBinding* const Binding = TestWorld.Subsystem->Find(StructureId);

	TestNotNull(
		*FString::Printf(TEXT("fixture: BuildRunningBond returned %d and Find should hand back its binding"),
			StructureId),
		Binding);

	if (Binding == nullptr || Binding->NumPieces() != PieceCount)
	{
		TestWorld.End();
		return true;
	}

	TArray<ABrickActor*> Bricks;
	TArray<FVector> LaidAt;

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		ABrickActor* Brick = BrickAt(*this, *Binding, Piece);

		if (Brick == nullptr)
		{
			TestWorld.End();
			return true;
		}

		Bricks.Add(Brick);
		LaidAt.Add(Brick->GetActorLocation());
	}

	TestEqual(
		TEXT("fixture: the wall as built should stand, so starting it up releases nothing"),
		TestWorld.Subsystem->SolveAndPush(StructureId), 0);

	ReportIntegrationState(*this, *Binding, Bricks, TEXT("as built"));

	ADestructionGamePlayerController* const Controller =
		SpawnControllerWithLocalPlayer(*this, TestWorld.World);

	if (Controller == nullptr)
	{
		TestWorld.End();
		return true;
	}

	// Piece 2 keeps its bed joint on piece 1, so nothing may move.
	ABrickActor* const FirstBrick = Bricks[FirstPulled];

	if (!InspectAndChooseDelete(
			*this, *Controller, *Delete, StructureId, Reference.Boxes, { FirstPulled }))
	{
		TestWorld.End();
		return true;
	}

	TestTrue(
		FString::Printf(TEXT("the first pulled brick's actor must have left the world, it is %s"),
			IsValid(FirstBrick) ? TEXT("still valid") : TEXT("gone")),
		!IsValid(FirstBrick));

	Bricks[FirstPulled] = nullptr;

	TestWorld.TickSeconds(StandSeconds);

	ReportIntegrationState(*this, *Binding, Bricks, TEXT("half a second after the first brick came out"));

	for (int32 Piece = 0; Piece < PieceCount; ++Piece)
	{
		if (Piece == FirstPulled)
		{
			continue;
		}

		const double MovedCm = FVector::Dist(Bricks[Piece]->GetActorLocation(), LaidAt[Piece]);

		TestTrue(
			*FString::Printf(
				TEXT("the wall survives losing brick %d, so brick %d must not move, it drifted %.6f cm"),
				FirstPulled, Piece, MovedCm),
			MovedCm < DriftToleranceCm);

		TestTrue(
			*FString::Printf(TEXT("brick %d must still be kinematic while the wall stands"), Piece),
			Bricks[Piece]->GetMesh() != nullptr && !Bricks[Piece]->GetMesh()->IsSimulatingPhysics());
	}

	// With both grounded bricks gone, piece 2 has no joint left; 3, 4 and 5 lose the ground with it.
	ABrickActor* const LastBrick = Bricks[LastStraw];

	if (!InspectAndChooseDelete(
			*this, *Controller, *Delete, StructureId, Reference.Boxes, { LastStraw }))
	{
		TestWorld.End();
		return true;
	}

	TestTrue(
		FString::Printf(TEXT("the last straw's actor must have left the world, it is %s"),
			IsValid(LastBrick) ? TEXT("still valid") : TEXT("gone")),
		!IsValid(LastBrick));

	Bricks[LastStraw] = nullptr;

	ReportIntegrationState(*this, *Binding, Bricks, TEXT("straight after the last straw"));

	for (int32 Piece = 0; Piece < PieceCount; ++Piece)
	{
		if (Piece == FirstPulled || Piece == LastStraw)
		{
			continue;
		}

		const EPieceSupport Support = Binding->GetStructure().GetPieceSupport(Piece);

		// Stranded would mean a solver limitation, not a collapse.
		TestTrue(
			*FString::Printf(
				TEXT("piece %d must not be Stranded: that would make this a solver limitation rather than a collapse"),
				Piece),
			Support != EPieceSupport::Stranded);

		TestTrue(
			*FString::Printf(TEXT("piece %d lost its last path to the ground and should read Falling, the solver says %s"),
				Piece, IntegrationSupportName(Support)),
			Support == EPieceSupport::Falling);

		const bool bSimulating =
			Bricks[Piece]->GetMesh() != nullptr && Bricks[Piece]->GetMesh()->IsSimulatingPhysics();

		TestTrue(
			*FString::Printf(TEXT("brick %d should have been handed to physics by the last straw, it is %s"),
				Piece, bSimulating ? TEXT("simulating") : TEXT("kinematic")),
			bSimulating);
	}

	TestWorld.TickSeconds(FallSeconds);

	ReportIntegrationState(*this, *Binding, Bricks, TEXT("one second after the last straw"));

	for (int32 Piece = 0; Piece < PieceCount; ++Piece)
	{
		if (Piece == FirstPulled || Piece == LastStraw)
		{
			continue;
		}

		const FVector NowAt = Bricks[Piece]->GetActorLocation();
		const double FellCm = LaidAt[Piece].Z - NowAt.Z;

		AddInfo(FString::Printf(
			TEXT("brick %d fell %.3f cm in the second after the last straw, from Z %.3f to Z %.3f"),
			Piece, FellCm, LaidAt[Piece].Z, NowAt.Z));

		TestTrue(
			*FString::Printf(
				TEXT("the wall must come down: brick %d should have fallen more than %.1f cm, it dropped %.3f cm"),
				Piece, FallenAtLeastCm, FellCm),
			FellCm > FallenAtLeastCm);

		// Rubble rests on the floor; below it means the brick fell through the world.
		TestTrue(
			*FString::Printf(TEXT("brick %d should have landed on the floor at Z %g, it is at Z %.3f"),
				Piece, FloorTopZCm, NowAt.Z),
			NowAt.Z > FloorTopZCm);
	}

	TestWorld.End();

	return true;
}

/**
 * Removing a brick the wall can spare moves nothing, and its load moves onto the neighbours
 * (DESIGN.md §4: any brick drifting is a hard fail). The control for the two tests above: "release
 * everything" passes their fall checks but fails this. Green on arrival; a regression net.
 *
 * Exact load claim: 10 and 11 each rest on two equal-area bed joints and carry 2W (own weight plus
 * half of each brick above), so each joint carries W before and the survivor 2W after.
 * W = 2.72163125 kg x 980 = 2667.198625 uu; do not apply 1 N = 100 uu again.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureIntegrationRedistributionTest,
	"DestructionGame.Integration.ARemovalTheWallSurvivesMovesNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureIntegrationRedistributionTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;
	using namespace StructureIntegrationTestSupport;

	const FPieceAction* const Delete = FindIntegrationAction(TEXT("Delete"));

	if (Delete == nullptr)
	{
		AddError(TEXT("fixture: the action table must contain a row labelled 'Delete'"));
		return true;
	}

	const FRunningBondSpec Spec = WideWallSpec();

	FBrickLayout Reference;

	TestTrue(TEXT("fixture: RunningBond should lay the reference wall"), RunningBond(Spec, Reference));

	if (Reference.Boxes.Num() != WideWallPieceCount)
	{
		AddError(FString::Printf(TEXT("fixture: a ragged 4 x 5 wall should be %d pieces, got %d"),
			WideWallPieceCount, Reference.Boxes.Num()));

		return true;
	}

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	const int32 StructureId = TestWorld.Subsystem->BuildRunningBond(Spec);
	FStructureBinding* const Binding = TestWorld.Subsystem->Find(StructureId);

	TestNotNull(
		*FString::Printf(TEXT("fixture: BuildRunningBond returned %d and Find should hand back its binding"),
			StructureId),
		Binding);

	if (Binding == nullptr || Binding->NumPieces() != WideWallPieceCount)
	{
		TestWorld.End();
		return true;
	}

	TArray<ABrickActor*> Bricks;
	TArray<FVector> LaidAt;

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		ABrickActor* Brick = BrickAt(*this, *Binding, Piece);

		if (Brick == nullptr)
		{
			TestWorld.End();
			return true;
		}

		Bricks.Add(Brick);
		LaidAt.Add(Brick->GetActorLocation());
	}

	TestEqual(
		TEXT("fixture: the wall as built should stand, so starting it up releases nothing"),
		TestWorld.Subsystem->SolveAndPush(StructureId), 0);

	// Topology preconditions: the expected forces assume these joints exist.
	const FStructure& Graph = Binding->GetStructure();

	const int32 LeftJoint = FindIntegrationJoint(Graph, WideWallLeftNeighbour, WideWallLeftCarrier);
	const int32 DoomedLeftJoint = FindIntegrationJoint(Graph, WideWallRemovedPiece, WideWallLeftCarrier);
	const int32 DoomedRightJoint = FindIntegrationJoint(Graph, WideWallRemovedPiece, WideWallRightCarrier);
	const int32 RightJoint = FindIntegrationJoint(Graph, WideWallRightNeighbour, WideWallRightCarrier);

	if (LeftJoint == INDEX_NONE || DoomedLeftJoint == INDEX_NONE
		|| DoomedRightJoint == INDEX_NONE || RightJoint == INDEX_NONE)
	{
		AddError(FString::Printf(
			TEXT("fixture: the wall must join %d-%d, %d-%d, %d-%d and %d-%d; they resolved to %d, %d, %d and %d"),
			WideWallLeftNeighbour, WideWallLeftCarrier,
			WideWallRemovedPiece, WideWallLeftCarrier,
			WideWallRemovedPiece, WideWallRightCarrier,
			WideWallRightNeighbour, WideWallRightCarrier,
			LeftJoint, DoomedLeftJoint, DoomedRightJoint, RightJoint));

		TestWorld.End();
		return true;
	}

	// Equal areas make the split even, hence exactly W and 2W.
	TestTrue(
		*FString::Printf(
			TEXT("fixture: piece %d's two bed joints must have equal areas for the split to be even, they are %.6f and %.6f cm2"),
			WideWallLeftCarrier,
			Graph.GetConnection(LeftJoint).InterfaceAreaSqCm,
			Graph.GetConnection(DoomedLeftJoint).InterfaceAreaSqCm),
		FMath::IsNearlyEqual(
			Graph.GetConnection(LeftJoint).InterfaceAreaSqCm,
			Graph.GetConnection(DoomedLeftJoint).InterfaceAreaSqCm,
			1.0e-9));

	const double FullBrickWeightUnrealUnits = FullBrickMassKg * GravityCmPerSecondSquared;

	const double LeftBeforeUnrealUnits = Graph.GetConnectionForce(LeftJoint).Size();
	const double RightBeforeUnrealUnits = Graph.GetConnectionForce(RightJoint).Size();

	AddInfo(FString::Printf(
		TEXT("before the removal, joint %d-%d carries %.6f uu and joint %d-%d carries %.6f uu (one full brick weighs %.6f uu)"),
		WideWallLeftNeighbour, WideWallLeftCarrier, LeftBeforeUnrealUnits,
		WideWallRightNeighbour, WideWallRightCarrier, RightBeforeUnrealUnits,
		FullBrickWeightUnrealUnits));

	TestTrue(
		*FString::Printf(
			TEXT("fixture: joint %d-%d should carry one full brick's weight, %.6f uu, before the removal; it carries %.6f uu"),
			WideWallLeftNeighbour, WideWallLeftCarrier,
			FullBrickWeightUnrealUnits, LeftBeforeUnrealUnits),
		FMath::IsNearlyEqual(
			LeftBeforeUnrealUnits, FullBrickWeightUnrealUnits, JointForceToleranceUnrealUnits));

	TestTrue(
		*FString::Printf(
			TEXT("fixture: joint %d-%d should carry one full brick's weight, %.6f uu, before the removal; it carries %.6f uu"),
			WideWallRightNeighbour, WideWallRightCarrier,
			FullBrickWeightUnrealUnits, RightBeforeUnrealUnits),
		FMath::IsNearlyEqual(
			RightBeforeUnrealUnits, FullBrickWeightUnrealUnits, JointForceToleranceUnrealUnits));

	ADestructionGamePlayerController* const Controller =
		SpawnControllerWithLocalPlayer(*this, TestWorld.World);

	if (Controller == nullptr)
	{
		TestWorld.End();
		return true;
	}

	ABrickActor* const RemovedBrick = Bricks[WideWallRemovedPiece];

	if (!InspectAndChooseDelete(
			*this, *Controller, *Delete, StructureId,
			Reference.Boxes, { WideWallRemovedPiece }))
	{
		TestWorld.End();
		return true;
	}

	TestTrue(
		FString::Printf(TEXT("the removed brick's actor must have left the world, it is %s"),
			IsValid(RemovedBrick) ? TEXT("still valid") : TEXT("gone")),
		!IsValid(RemovedBrick));

	Bricks[WideWallRemovedPiece] = nullptr;

	ReportIntegrationState(*this, *Binding, Bricks, TEXT("after the removal the wall survives"));

	// The surviving bed joint takes the carrier's whole 2W.
	const double LeftAfterUnrealUnits = Graph.GetConnectionForce(LeftJoint).Size();
	const double RightAfterUnrealUnits = Graph.GetConnectionForce(RightJoint).Size();

	AddInfo(FString::Printf(
		TEXT("after the removal, joint %d-%d carries %.6f uu and joint %d-%d carries %.6f uu"),
		WideWallLeftNeighbour, WideWallLeftCarrier, LeftAfterUnrealUnits,
		WideWallRightNeighbour, WideWallRightCarrier, RightAfterUnrealUnits));

	TestTrue(
		*FString::Printf(
			TEXT("joint %d-%d must take on everything piece %d was carrying, %.6f uu; it carries %.6f uu"),
			WideWallLeftNeighbour, WideWallLeftCarrier, WideWallRemovedPiece,
			2.0 * FullBrickWeightUnrealUnits, LeftAfterUnrealUnits),
		FMath::IsNearlyEqual(
			LeftAfterUnrealUnits, 2.0 * FullBrickWeightUnrealUnits, JointForceToleranceUnrealUnits));

	TestTrue(
		*FString::Printf(
			TEXT("joint %d-%d must take on everything piece %d was carrying, %.6f uu; it carries %.6f uu"),
			WideWallRightNeighbour, WideWallRightCarrier, WideWallRemovedPiece,
			2.0 * FullBrickWeightUnrealUnits, RightAfterUnrealUnits),
		FMath::IsNearlyEqual(
			RightAfterUnrealUnits, 2.0 * FullBrickWeightUnrealUnits, JointForceToleranceUnrealUnits));

	// No drift allowed; the kinematic model gives 0 cm where Chaos constraints gave 0.62-0.70 cm.
	TestWorld.TickSeconds(FallSeconds);

	ReportIntegrationState(*this, *Binding, Bricks, TEXT("a second after the removal"));

	for (int32 Piece = 0; Piece < WideWallPieceCount; ++Piece)
	{
		if (Piece == WideWallRemovedPiece)
		{
			continue;
		}

		const double MovedCm = FVector::Dist(Bricks[Piece]->GetActorLocation(), LaidAt[Piece]);

		TestTrue(
			*FString::Printf(
				TEXT("the wall survives losing brick %d, so brick %d must not move at all, it drifted %.6f cm"),
				WideWallRemovedPiece, Piece, MovedCm),
			MovedCm < DriftToleranceCm);

		TestTrue(
			*FString::Printf(TEXT("brick %d must still be kinematic in a wall that is still standing"), Piece),
			Bricks[Piece]->GetMesh() != nullptr && !Bricks[Piece]->GetMesh()->IsSimulatingPhysics());
	}

	TestWorld.End();

	return true;
}

/**
 * Deleting a multi-brick selection with one choice removes them all, drops what they held up,
 * moves nothing when the wall can spare them, and costs one solve. The world-free test pins the
 * single solve; this checks the result is pushed for every orphan (ApplyResults releases only
 * pieces the last solve answered, so a mistimed solve leaves bricks hanging silently).
 *
 * One world, two batches: first a survivable pair (nothing may move), then the grounded course
 * (everything must fall). NumSolves is the one mechanism row, since a cost is not an outcome.
 * Push count is not asserted (idempotent, unobservable), nor any joint break (loss of support).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureIntegrationBatchedDeleteTest,
	"DestructionGame.Integration.DeletingASelectionDropsWhatItOrphaned",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureIntegrationBatchedDeleteTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;
	using namespace StructureIntegrationTestSupport;

	const FPieceAction* const Delete = FindIntegrationAction(TEXT("Delete"));

	if (Delete == nullptr)
	{
		AddError(TEXT("fixture: the action table must contain a row labelled 'Delete'"));
		return true;
	}

	const FRunningBondSpec Spec = WideWallSpec();

	FBrickLayout Reference;

	TestTrue(TEXT("fixture: RunningBond should lay the reference wall"), RunningBond(Spec, Reference));

	if (Reference.Boxes.Num() != WideWallPieceCount)
	{
		AddError(FString::Printf(TEXT("fixture: a ragged 4 x 5 wall should be %d pieces, got %d"),
			WideWallPieceCount, Reference.Boxes.Num()));

		return true;
	}

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	const int32 StructureId = TestWorld.Subsystem->BuildRunningBond(Spec);
	FStructureBinding* const Binding = TestWorld.Subsystem->Find(StructureId);

	TestNotNull(
		*FString::Printf(TEXT("fixture: BuildRunningBond returned %d and Find should hand back its binding"),
			StructureId),
		Binding);

	if (Binding == nullptr || Binding->NumPieces() != WideWallPieceCount)
	{
		TestWorld.End();
		return true;
	}

	TArray<ABrickActor*> Bricks;
	TArray<FVector> LaidAt;

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		ABrickActor* Brick = BrickAt(*this, *Binding, Piece);

		if (Brick == nullptr)
		{
			TestWorld.End();
			return true;
		}

		Bricks.Add(Brick);
		LaidAt.Add(Brick->GetActorLocation());
	}

	TestEqual(
		TEXT("fixture: the wall as built should stand, so starting it up releases nothing"),
		TestWorld.Subsystem->SolveAndPush(StructureId), 0);

	ReportIntegrationState(*this, *Binding, Bricks, TEXT("as built"));

	ADestructionGamePlayerController* const Controller =
		SpawnControllerWithLocalPlayer(*this, TestWorld.World);

	if (Controller == nullptr)
	{
		TestWorld.End();
		return true;
	}

	// Batch one: survivable, nothing may move.
	const int32 SolvesBeforeFirstBatch = Binding->GetStructure().NumSolves();

	TArray<ABrickActor*> FirstBatchActors;

	for (const int32 Piece : WideWallSurvivableBatch)
	{
		FirstBatchActors.Add(Bricks[Piece]);
	}

	if (!InspectAndChooseDelete(
			*this, *Controller, *Delete, StructureId, Reference.Boxes, WideWallSurvivableBatch))
	{
		TestWorld.End();
		return true;
	}

	const int32 SolvesSpentOnFirstBatch =
		Binding->GetStructure().NumSolves() - SolvesBeforeFirstBatch;

	AddInfo(FString::Printf(
		TEXT("deleting %d bricks in one batch spent %d solve(s)"),
		WideWallSurvivableBatch.Num(), SolvesSpentOnFirstBatch));

	// One solve per batch, not per brick; nothing else observable distinguishes the two.
	TestEqual(
		FString::Printf(
			TEXT("deleting %d bricks in ONE batch must cost exactly one solve, not one per brick; it cost %d"),
			WideWallSurvivableBatch.Num(), SolvesSpentOnFirstBatch),
		SolvesSpentOnFirstBatch, 1);

	// Every picked brick went, not just the last.
	for (int32 Index = 0; Index < WideWallSurvivableBatch.Num(); ++Index)
	{
		const int32 Piece = WideWallSurvivableBatch[Index];

		TestTrue(
			FString::Printf(TEXT("brick %d was picked, so its actor must have left the world; it is %s"),
				Piece, IsValid(FirstBatchActors[Index]) ? TEXT("still valid") : TEXT("gone")),
			!IsValid(FirstBatchActors[Index]));

		TestTrue(
			FString::Printf(TEXT("piece %d was picked, so it must be gone from the graph"), Piece),
			Binding->IsPieceRemoved(Piece));

		Bricks[Piece] = nullptr;
	}

	TestEqual(
		FString::Printf(TEXT("the batch must remove exactly %d pieces, so %d should be live, got %d"),
			WideWallSurvivableBatch.Num(),
			WideWallPieceCount - WideWallSurvivableBatch.Num(),
			Binding->GetStructure().NumLivePieces()),
		Binding->GetStructure().NumLivePieces(),
		WideWallPieceCount - WideWallSurvivableBatch.Num());

	TestWorld.TickSeconds(FallSeconds);

	ReportIntegrationState(*this, *Binding, Bricks, TEXT("a second after the survivable batch"));

	for (int32 Piece = 0; Piece < WideWallPieceCount; ++Piece)
	{
		if (Bricks[Piece] == nullptr)
		{
			continue;
		}

		const double MovedCm = FVector::Dist(Bricks[Piece]->GetActorLocation(), LaidAt[Piece]);

		TestTrue(
			*FString::Printf(
				TEXT("the wall survives this batch, so brick %d must not move at all, it drifted %.6f cm"),
				Piece, MovedCm),
			MovedCm < DriftToleranceCm);

		TestTrue(
			*FString::Printf(TEXT("brick %d must still be kinematic while the wall stands"), Piece),
			Bricks[Piece]->GetMesh() != nullptr && !Bricks[Piece]->GetMesh()->IsSimulatingPhysics());
	}

	// Batch two: the whole grounded course, after which nothing touches the ground.
	const int32 SolvesBeforeSecondBatch = Binding->GetStructure().NumSolves();

	TArray<ABrickActor*> SecondBatchActors;

	for (const int32 Piece : WideWallGroundCourse)
	{
		SecondBatchActors.Add(Bricks[Piece]);
	}

	if (!InspectAndChooseDelete(
			*this, *Controller, *Delete, StructureId, Reference.Boxes, WideWallGroundCourse))
	{
		TestWorld.End();
		return true;
	}

	const int32 SolvesSpentOnSecondBatch =
		Binding->GetStructure().NumSolves() - SolvesBeforeSecondBatch;

	AddInfo(FString::Printf(
		TEXT("deleting the %d-brick grounded course in one batch spent %d solve(s)"),
		WideWallGroundCourse.Num(), SolvesSpentOnSecondBatch));

	TestEqual(
		FString::Printf(
			TEXT("deleting the whole %d-brick course in ONE batch must still cost exactly one solve; it cost %d"),
			WideWallGroundCourse.Num(), SolvesSpentOnSecondBatch),
		SolvesSpentOnSecondBatch, 1);

	for (int32 Index = 0; Index < WideWallGroundCourse.Num(); ++Index)
	{
		const int32 Piece = WideWallGroundCourse[Index];

		TestTrue(
			FString::Printf(TEXT("brick %d was picked, so its actor must have left the world; it is %s"),
				Piece, IsValid(SecondBatchActors[Index]) ? TEXT("still valid") : TEXT("gone")),
			!IsValid(SecondBatchActors[Index]));

		Bricks[Piece] = nullptr;
	}

	ReportIntegrationState(*this, *Binding, Bricks, TEXT("straight after the grounded course went"));

	/*
	 * IsReleased (binding applied the solve) and IsSimulatingPhysics (body told) catch a push behind a
	 * mistimed solve. Support only asks "not held up"; Falling vs. Stranded is pinned elsewhere.
	 */
	for (int32 Piece = 0; Piece < WideWallPieceCount; ++Piece)
	{
		if (Bricks[Piece] == nullptr)
		{
			continue;
		}

		const EPieceSupport Support = Binding->GetStructure().GetPieceSupport(Piece);

		TestTrue(
			*FString::Printf(
				TEXT("with every grounded brick gone, piece %d cannot be held up, the solver says %s"),
				Piece, IntegrationSupportName(Support)),
			Support != EPieceSupport::Grounded && Support != EPieceSupport::Supported);

		TestTrue(
			*FString::Printf(TEXT("piece %d should have been released by the batch, IsReleased reports %s"),
				Piece, Binding->IsReleased(Piece) ? TEXT("true") : TEXT("false")),
			Binding->IsReleased(Piece));

		const bool bSimulating =
			Bricks[Piece]->GetMesh() != nullptr && Bricks[Piece]->GetMesh()->IsSimulatingPhysics();

		TestTrue(
			*FString::Printf(TEXT("brick %d should have been handed to physics by the batch, it is %s"),
				Piece, bSimulating ? TEXT("simulating") : TEXT("kinematic")),
			bSimulating);
	}

	// Outcome: it comes down.
	TestWorld.TickSeconds(FallSeconds);

	ReportIntegrationState(*this, *Binding, Bricks, TEXT("one second after the grounded course went"));

	for (int32 Piece = 0; Piece < WideWallPieceCount; ++Piece)
	{
		if (Bricks[Piece] == nullptr)
		{
			continue;
		}

		const FVector NowAt = Bricks[Piece]->GetActorLocation();
		const double FellCm = LaidAt[Piece].Z - NowAt.Z;

		AddInfo(FString::Printf(
			TEXT("brick %d fell %.3f cm in the second after the batch, from Z %.3f to Z %.3f"),
			Piece, FellCm, LaidAt[Piece].Z, NowAt.Z));

		TestTrue(
			*FString::Printf(
				TEXT("the wall must come down: brick %d should have fallen more than %.1f cm, it dropped %.3f cm"),
				Piece, FallenAtLeastCm, FellCm),
			FellCm > FallenAtLeastCm);

		// Rubble rests on the floor; below it means the brick fell through the world.
		TestTrue(
			*FString::Printf(TEXT("brick %d should have landed on the floor at Z %g, it is at Z %.3f"),
				Piece, FloorTopZCm, NowAt.Z),
			NowAt.Z > FloorTopZCm);
	}

	TestWorld.End();

	return true;
}

/**
 * Cutting a stepped diagonal void leaves the brickwork above corbelled over nothing, and it must
 * stand. Inverted from "falls" by the user's ruling of 2026-08-06: a corbel is locally identical
 * to a wall's free end, and the free end must not unzip, so composite vertical action saves both.
 * The wall acts as a deep beam: 11,627 cm3 of section over eleven courses against one patch's
 * 179.48, so the bottom rung reads 0.369 of capacity rather than 22.93. Derivation in
 * Tests/StaircaseWallTestSupport.h; magnitudes pinned world-free in
 * Core.Structure.AStaircaseVoidCondemnsTheCorbel. Consistent with acceptance case 20 (local loss).
 *
 * Non-displacement is a valid assertion (an unmoved brick was not released); joint breaks are read
 * off break stamps, since a given joint carries nothing (DESIGN.md §3). Against vacuity: the 36 cut
 * bricks must be gone and the corbel joints must exist. No survivor may be Stranded. The wall is
 * flush because a ragged end is already eccentric (see StaircaseWallSpec).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureIntegrationStaircaseVoidTest,
	"DestructionGame.Integration.AStaircaseVoidLeavesTheOverhangStanding",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureIntegrationStaircaseVoidTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;
	using namespace StaircaseWallTestSupport;
	using namespace StructureIntegrationTestSupport;

	const FPieceAction* const Delete = FindIntegrationAction(TEXT("Delete"));

	if (Delete == nullptr)
	{
		AddError(TEXT("fixture: the action table must contain a row labelled 'Delete'"));
		return true;
	}

	const FRunningBondSpec Spec = StaircaseWallSpec();

	FBrickLayout Reference;

	TestTrue(TEXT("fixture: RunningBond should lay the reference wall"), RunningBond(Spec, Reference));

	if (Reference.Boxes.Num() != StaircaseWallPieceCount)
	{
		AddError(FString::Printf(TEXT("fixture: a flush 13 x 10 wall should be %d pieces, got %d"),
			StaircaseWallPieceCount, Reference.Boxes.Num()));

		return true;
	}

	/*
	 * The void, read off the laid wall. Course c keeps everything from (12 - c) x 11.25 cm rightward:
	 *
	 *     course 12  [][][][][][][][][][]              whole, and already corbelled
	 *     course 11    ..[][][][][][][][][]
	 *     course 10  ....[][][][][][][][][]
	 *      ...
	 *     course  2  ..........[][][][][][]
	 *     course  1    ..........[][][][][]
	 *     course  0  [][][][][][][][][][]              grounded, whole
	 *
	 * 36 bricks, deleted in one batch.
	 */
	const TArray<int32> VoidPieces = StaircaseVoidPieces(Reference.Boxes);

	if (VoidPieces.Num() != StaircaseVoidPieceCount)
	{
		AddError(FString::Printf(TEXT("fixture: the staircase should cut %d bricks, it names %d"),
			StaircaseVoidPieceCount, VoidPieces.Num()));

		return true;
	}

	// The eleven corbelled bricks and their single supporting bricks.
	TArray<int32> CorbelPieces;
	TArray<int32> CorbelSupports;

	for (int32 Course = StaircaseLowestCorbelCourse; Course <= StaircaseHighestCorbelCourse; ++Course)
	{
		const int32 Corbel = StaircaseCorbelPiece(Reference.Boxes, Course);
		const int32 Support = StaircaseCorbelSupportPiece(Reference.Boxes, Course);

		if (Corbel == INDEX_NONE || Support == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("fixture: course %d should have a brick at X %.2f resting on one at X %.2f"),
				Course, StaircaseVoidEdgeXCm(Course),
				StaircaseVoidEdgeXCm(Course) + StaircaseHalfStepCm));

			return true;
		}

		CorbelPieces.Add(Corbel);
		CorbelSupports.Add(Support);
	}

	// Three bricks at the far end, which the staircase never reaches.
	TArray<int32> FarSidePieces;

	for (const int32 Course : { 0, 6, 12 })
	{
		const int32 Piece =
			StaircasePieceAt(Reference.Boxes, StaircaseFarSideXCm, StaircaseCourseZCm(Course));

		if (Piece == INDEX_NONE)
		{
			AddError(FString::Printf(TEXT("fixture: course %d should have a brick at X %.2f"),
				Course, StaircaseFarSideXCm));

			return true;
		}

		FarSidePieces.Add(Piece);
	}

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	const int32 StructureId = TestWorld.Subsystem->BuildRunningBond(Spec);
	FStructureBinding* const Binding = TestWorld.Subsystem->Find(StructureId);

	TestNotNull(
		*FString::Printf(TEXT("fixture: BuildRunningBond returned %d and Find should hand back its binding"),
			StructureId),
		Binding);

	if (Binding == nullptr || Binding->NumPieces() != StaircaseWallPieceCount)
	{
		TestWorld.End();
		return true;
	}

	TArray<ABrickActor*> Bricks;
	TArray<FVector> LaidAt;

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		ABrickActor* Brick = BrickAt(*this, *Binding, Piece);

		if (Brick == nullptr)
		{
			TestWorld.End();
			return true;
		}

		Bricks.Add(Brick);
		LaidAt.Add(Brick->GetActorLocation());
	}

	// Without geometry every moment is silently zero and the corbels read healthy.
	TestTrue(
		TEXT("fixture: the laid wall must know where its pieces and its joints are, or every moment below is silently zero"),
		Binding->GetStructure().HasCompleteGeometry());

	TestEqual(
		TEXT("fixture: the wall as built should stand, so starting it up releases nothing"),
		TestWorld.Subsystem->SolveAndPush(StructureId), 0);

	// Positive control: nothing is over capacity before the cut (the flush end ensures it).
	double WorstAsBuilt = 0.0;
	int32 WorstAsBuiltJoint = INDEX_NONE;

	for (int32 Joint = 0; Joint < Binding->GetStructure().NumConnections(); ++Joint)
	{
		const double Utilisation = Binding->GetStructure().GetConnectionUtilisation(Joint);

		if (Utilisation > WorstAsBuilt)
		{
			WorstAsBuilt = Utilisation;
			WorstAsBuiltJoint = Joint;
		}
	}

	AddInfo(FString::Printf(
		TEXT("as built, the worst of %d joints is %d at %.6f of capacity"),
		Binding->GetStructure().NumConnections(), WorstAsBuiltJoint, WorstAsBuilt));

	TestTrue(
		*FString::Printf(
			TEXT("the wall as built must have nothing over capacity, joint %d reads %.6f"),
			WorstAsBuiltJoint, WorstAsBuilt),
		WorstAsBuilt < 1.0);

	ADestructionGamePlayerController* const Controller =
		SpawnControllerWithLocalPlayer(*this, TestWorld.World);

	if (Controller == nullptr)
	{
		TestWorld.End();
		return true;
	}

	if (!InspectAndChooseDelete(
			*this, *Controller, *Delete, StructureId, Reference.Boxes, VoidPieces))
	{
		TestWorld.End();
		return true;
	}

	for (const int32 Piece : VoidPieces)
	{
		TestTrue(
			*FString::Printf(TEXT("cut brick %d's actor must have left the world, it is %s"),
				Piece, IsValid(Bricks[Piece]) ? TEXT("still valid") : TEXT("gone")),
			!IsValid(Bricks[Piece]));

		Bricks[Piece] = nullptr;
	}

	/*
	 * Read break stamps, not utilisation: the cascade runs inside the commit and a given joint
	 * carries nothing (DESIGN.md §3). A stamped corbel joint failed under load; a deleted one has no
	 * stamp. Pass 1 is the first sweep after the cut (nothing was over capacity before it).
	 * Checked against StaircaseCorbelIsCondemned rather than a hard-coded "none", so this follows
	 * the shared oracle if it ever condemns a rung. Magnitudes are pinned world-free in
	 * Core.Structure.AStaircaseVoidCondemnsTheCorbel.
	 */
	int32 CorbelJointsBrokenInFirstPass = 0;

	for (int32 Step = 0; Step < CorbelPieces.Num(); ++Step)
	{
		const int32 Course = StaircaseLowestCorbelCourse + Step;

		const int32 Joint = FindIntegrationJoint(
			Binding->GetStructure(), CorbelPieces[Step], CorbelSupports[Step]);

		if (Joint == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("fixture: course %d's corbel (piece %d) should still be jointed to piece %d"),
				Course, CorbelPieces[Step], CorbelSupports[Step]));

			continue;
		}

		const int32 BreakPass = Binding->GetStructure().GetBreakPass(Joint);
		const bool bCondemned = StaircaseCorbelIsCondemned(Course);

		AddInfo(FString::Printf(
			TEXT("corbel course %2d: piece %3d on piece %3d, joint %3d %s, broke in pass %d (the arithmetic puts it at %.5f of capacity)"),
			Course, CorbelPieces[Step], CorbelSupports[Step], Joint,
			Binding->GetStructure().GetConnection(Joint).HasGiven() ? TEXT("has given") : TEXT("is intact"),
			BreakPass, StaircasePredictedCorbelUtilisation(Course)));

		if (BreakPass == 1)
		{
			++CorbelJointsBrokenInFirstPass;
		}

		if (bCondemned)
		{
			TestTrue(
				*FString::Printf(
					TEXT("the load model must condemn course %d's corbel: the arithmetic puts it at %.5f of capacity, so the first sweep after the cut must break it — it broke in pass %d"),
					Course, StaircasePredictedCorbelUtilisation(Course), BreakPass),
				BreakPass == 1);
		}
		else
		{
			TestTrue(
				*FString::Printf(
					TEXT("course %d's corbel is under capacity at %.5f, so the first sweep must NOT have broken it — it broke in pass %d"),
					Course, StaircasePredictedCorbelUtilisation(Course), BreakPass),
				BreakPass != 1);
		}
	}

	// The count also catches breaks the per-rung rows cannot see.
	AddInfo(FString::Printf(
		TEXT("the staircase's first sweep broke %d of %d corbel joints (the arithmetic predicts %d, worst rung %.8f)"),
		CorbelJointsBrokenInFirstPass, CorbelPieces.Num(),
		StaircasePredictedCorbelJointsOverCapacity, StaircasePredictedWorstCorbelUtilisation));

	TestEqual(
		*FString::Printf(
			TEXT("the first sweep after the cut must break exactly the %d corbel joints the arithmetic condemns"),
			StaircasePredictedCorbelJointsOverCapacity),
		CorbelJointsBrokenInFirstPass, StaircasePredictedCorbelJointsOverCapacity);

	// No survivor may be Stranded; a staircase void is the shape that makes unroutable knots.
	for (int32 Piece = 0; Piece < StaircaseWallPieceCount; ++Piece)
	{
		if (Bricks[Piece] == nullptr)
		{
			continue;
		}

		const EPieceSupport Support = Binding->GetStructure().GetPieceSupport(Piece);

		TestTrue(
			*FString::Printf(
				TEXT("piece %d must not be Stranded: that would make this a solver limitation rather than a collapse"),
				Piece),
			Support != EPieceSupport::Stranded);
	}

	// A second is ample for a released brick to leave, so an unmoved brick was never released.
	TestWorld.TickSeconds(FallSeconds);

	for (int32 Step = 0; Step < CorbelPieces.Num(); ++Step)
	{
		const int32 Piece = CorbelPieces[Step];
		const FVector NowAt = Bricks[Piece]->GetActorLocation();
		const double MovedCm = FVector::Dist(NowAt, LaidAt[Piece]);

		AddInfo(FString::Printf(
			TEXT("corbel course %2d (piece %3d) moved %.6f cm in one second and is at Z %.3f"),
			StaircaseLowestCorbelCourse + Step, Piece, MovedCm, NowAt.Z));

		TestTrue(
			*FString::Printf(
				TEXT("THE RULING: the overhang must STAND — corbelled brick %d should not have moved, it drifted %.6f cm"),
				Piece, MovedCm),
			MovedCm < DriftToleranceCm);

		// Kinematic rules out a released brick that merely jammed in place.
		TestTrue(
			*FString::Printf(TEXT("corbelled brick %d is still held up and must still be kinematic"), Piece),
			Bricks[Piece]->GetMesh() != nullptr && !Bricks[Piece]->GetMesh()->IsSimulatingPhysics());
	}

	// The far end must be untouched too.
	for (const int32 Piece : FarSidePieces)
	{
		const double MovedCm = FVector::Dist(Bricks[Piece]->GetActorLocation(), LaidAt[Piece]);

		TestTrue(
			*FString::Printf(
				TEXT("the far end of the wall is untouched by the staircase, so brick %d must not move, it drifted %.6f cm"),
				Piece, MovedCm),
			MovedCm < DriftToleranceCm);

		TestTrue(
			*FString::Printf(TEXT("brick %d is still held up and must still be kinematic"), Piece),
			Bricks[Piece]->GetMesh() != nullptr && !Bricks[Piece]->GetMesh()->IsSimulatingPhysics());
	}

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
