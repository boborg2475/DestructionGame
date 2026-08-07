// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceActions.h"
#include "Core/PieceMenu.h"
#include "DestructionGamePlayerController.h"
#include "Tests/BrickWorldTestSupport.h"
#include "Tests/StaircaseWallTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE INTEGRATION SET. Three tests, and they exist because 71 correct tests did not stop a
 * player finding a broken wall in ten seconds.
 *
 * EVERY TEST IN THIS FILE OBEYS TWO RULES, AND A TEST THAT BREAKS EITHER BELONGS SOMEWHERE ELSE.
 *
 *   1. IT ENTERS THROUGH THE CALL A PLAYER'S OWN ACTION ENTERS THROUGH — a ray into
 *      InspectAlongRay and an index into ChoosePieceMenuRow — and NEVER through an internal
 *      step such as SolveAndPush, ApplyResults, RemovePiece or Release. The defect this set was
 *      written for lived entirely in the JOIN between two links, both of which were themselves
 *      correct and separately tested; a test that calls the join's parts individually cannot see
 *      it however many assertions it makes.
 *
 *   2. IT ASSERTS A PHYSICAL OUTCOME — a brick moved, or provably did not, after a fixed number
 *      of simulated seconds — rather than a model state. IsPieceRemoved, GetPieceSupport and
 *      NumLivePieces are MECHANISM, and mechanism is exactly what stayed green while the wall
 *      stood still. Where the mechanism is already covered elsewhere it is deliberately not
 *      repeated here; what is repeated is only what a claim below would be unfalsifiable
 *      without.
 *
 * WHERE THE LINE IS, for whoever adds the fourth. If the thing that can be wrong is arithmetic
 * on a graph, it belongs in the fast world-free suite, which runs in milliseconds and can afford
 * twelve thousand cases. If the thing that can be wrong is A CALL THAT NOBODY MAKES — a result
 * computed and never pushed, a flag set and never read, a wire between two correct halves — no
 * amount of world-free testing can reach it, and it belongs here. The cost is a world per test,
 * so group by world CONFIGURATION rather than by assertion: one test running a long sequence
 * against one wall beats five tests paying for five worlds to say five things about it.
 *
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges
 * many files into one. See CURRENT_STATE.md; the `using namespace` lives inside each RunTest
 * body for the same reason. The world harness is NOT redeclared here: it lives in
 * Tests/BrickWorldTestSupport.h and every world test shares it, because a second copy of a floor
 * height, a fall threshold and a tick length is two fixtures that drift.
 */
namespace StructureIntegrationTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/**
	 * How far along Y a ray starts and ends, either side of the wall.
	 *
	 * A brick is 10.25 cm deep and every wall here is centred on Y = 0, so +/- 100 cm is far
	 * outside it on both sides and the ray crosses the whole thickness. Along Y rather than X or
	 * Z, so nothing else in the wall is ever in the way and the answer is unambiguous.
	 */
	constexpr double IntegrationReachCm = 100.0;

	/**
	 * WEIGHT FROM MASS, DERIVED HERE RATHER THAN IMPORTED.
	 *
	 * Unreal's gravity is 980 cm/s2 and mass is in kilograms, so kg x 980 IS the weight in
	 * Unreal force units — DESIGN.md §3's 1 N = 100 uu is already inside that number and applying
	 * it again is the 100x error the whole units section exists to prevent. Spelled out here
	 * rather than read off a production constant, so this file disagrees with a wrong constant
	 * instead of agreeing with it.
	 */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** Bricks stop in the first third of a second; a full one is margin, not a settle poll. */
	constexpr double FallSeconds = 1.0;

	/**
	 * How long a wall that must NOT move is watched for.
	 *
	 * Half a second of free fall is 122.5 cm, more than a thousand times
	 * BrickWorldTestSupport::DriftToleranceCm, so a brick that was wrongly handed to physics is
	 * caught with an enormous margin. Shorter than the fall watch because nothing has to travel,
	 * land and settle — only fail to start.
	 */
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

	/** The Delete row, looked up by label so nothing hard-codes a position in the table. */
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

	/**
	 * The joint between two named pieces, or INDEX_NONE.
	 *
	 * ONE IMPLEMENTATION, IN Tests/StaircaseWallTestSupport.h, because the world-free staircase
	 * test needs the same lookup and this file's header drags in a world it does not want. The
	 * reasoning for finding joints by PIECE PAIR rather than by index is written there.
	 */
	int32 FindIntegrationJoint(const FStructure& Structure, int32 FirstPiece, int32 SecondPiece)
	{
		return StaircaseWallTestSupport::JointBetweenPieces(Structure, FirstPiece, SecondPiece);
	}

	/**
	 * The whole structure's state in one log line: what the graph thinks, what the binding
	 * recorded, and what the body is actually doing.
	 *
	 * PRINTING ALL THREE IS THE DIAGNOSTIC THIS SET EXISTS FOR. The failure that reached a player
	 * reads as "Falling / held / kinematic" on every orphan — the graph knowing perfectly well
	 * that a brick has lost the ground while nothing ever told the world — and no one of the
	 * three columns says that on its own.
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

	/** What is on screen, so a failure reads without a debugger. */
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
	 * WHAT A PLAYER DOES, IN ONE CALL: point at each brick they want, and choose Delete off the
	 * menu that is up once they have picked them all.
	 *
	 * THIS IS THE ONLY ROUTE ANY TEST IN THIS FILE TAKES TO CHANGE A WALL, and that is the point
	 * of the helper rather than a convenience: it is impossible to write a test in this file that
	 * accidentally reaches past the presenter into RemovePiece, SolveAndPush or the batched commit
	 * itself, because there is nothing here that does. The two inches in front of it that a
	 * headless run cannot reach are the deprojection that turns a cursor into these rays, and the
	 * button that supplies the index.
	 *
	 * A LIST OF PIECES RATHER THAN ONE, BECAUSE ONE IS THE ONE-ELEMENT CASE. A single-brick
	 * delete is a selection of one taken through exactly the calls a six-brick delete takes, so
	 * the three tests that predate multi-select keep entering through the same door — and a
	 * second helper for "the multi one" would be a second door with its own way of being wrong.
	 *
	 * THE ROW IS FOUND BY ACTION POINTER, NOT ASSUMED TO BE ROW 0. PieceActionsFor hands back
	 * pointers into the shipped table precisely so a presenter can compare identities, and the
	 * day the table grows a second row this keeps choosing Delete instead of whatever sorted
	 * first. It is looked up again after EVERY click, because the menu is rebuilt each time.
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

			/*
			 * THE ROW MUST NAME THE BRICK THAT WAS JUST POINTED AT. A chain wired to the wrong
			 * brick puts up a perfect menu and deletes somebody else, and every count in the wall
			 * still agrees. FPieceMenuRow::Ref is the last piece picked, so this holds for the
			 * first click and for the sixth.
			 */
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

		/* ONE CHOICE FOR THE WHOLE SELECTION — one click of one button, however many are picked. */
		const bool bChose = Controller.ChoosePieceMenuRow(DeleteRow);

		Test.TestTrue(
			*FString::Printf(TEXT("choosing Delete on %d picked brick(s) should report that it committed"),
				Pieces.Num()),
			bChose);

		return bChose;
	}

	/*
	 * THE WAIST WALL ITSELF LIVES IN Tests/BrickWorldTestSupport.h, as
	 * NarrowWaistWallSpec(CoursesHigh) — four test files need one, and four copies of a fixture
	 * is four things that drift. Why a waist at all, and why ragged and two bricks per course,
	 * is written there.
	 */

	/**
	 * A WALL WIDE ENOUGH TO SURVIVE LOSING A BRICK, for the redistribution test.
	 *
	 * Five bricks per course and four courses, ragged, is 5 + 4 + 5 + 4 = 18 pieces:
	 *
	 *      course 3      [14][15][16][17]         X centres 11.25, 33.75, 56.25, 78.75
	 *      course 2    [ 9][10][11][12][13]       X centres 0, 22.5, 45, 67.5, 90
	 *      course 1      [ 5][ 6][ 7][ 8]         X centres 11.25, 33.75, 56.25, 78.75
	 *      course 0    [ 0][ 1][ 2][ 3][ 4]       grounded
	 *
	 * Piece 6 is the one taken out, and it is interior on purpose: pieces 10 and 11 each rest on
	 * it AND on one other course-1 brick, so both keep a bed joint and nothing loses its path to
	 * the ground. That is what makes this the CONTROL — the same player action, the same commit
	 * path, a wall that must not move.
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
	 * TWO BRICKS THE WIDE WALL CAN SPARE AT THE SAME TIME, deleted in ONE batch.
	 *
	 * 6 is a course-1 brick and 12 is a course-2 brick, and they are chosen so that nothing loses
	 * its last path to the ground when BOTH go — which is a stronger requirement than either
	 * being survivable alone, and is what makes this the control for a BATCH rather than for a
	 * removal. Reading the bond off the diagram above: 10 rests on 5 and 6 so it keeps 5; 11 rests
	 * on 6 and 7 so it keeps 7; 16 rests on 11 and 12 so it keeps 11; 17 rests on 12 and 13 so it
	 * keeps 13. Nothing else touches either of them.
	 */
	const TArray<int32> WideWallSurvivableBatch = { 6, 12 };

	/**
	 * THE WHOLE GROUNDED COURSE, DELETED IN ONE BATCH — five bricks, one click of one button.
	 *
	 * FIVE AT ONCE RATHER THAN A CLEVER SUBSET, deliberately: the outcome then needs no
	 * hand-reading of the bond at all. Only course 0 is grounded, so with all five gone NOTHING
	 * that is left has any path to the earth and the entire remainder of the wall must come down.
	 * A fixture whose expected outcome is "everything" cannot be quietly wrong about which pieces
	 * it named.
	 */
	const TArray<int32> WideWallGroundCourse = { 0, 1, 2, 3, 4 };

	/**
	 * Slack on a joint force of a few thousand Unreal force units, not signal.
	 *
	 * The expected values below are exact — a handful of doubles added and halved — so this is
	 * four decimal places of headroom on a quantity in the thousands, and it is loose enough that
	 * a reassociation cannot fail it and tight enough that a wrong SHARE cannot pass it.
	 */
	constexpr double JointForceToleranceUnrealUnits = 0.01;

	/*
	 * THE STAIRCASE WALL — the fixture for the fourth test, and it is a PHOTOGRAPH rather than a
	 * shape somebody invented to break. Its geometry and its arithmetic live in
	 * Tests/StaircaseWallTestSupport.h, because the world-free twin of that test and the visual
	 * harness that photographs the same cut both need them and three copies of a void definition
	 * is three fixtures that drift.
	 */
}

/**
 * ONE: THE PLAYER JOURNEY. POINTING AT A BRICK AND CHOOSING DELETE MAKES THE BRICKS IT WAS
 * HOLDING UP PHYSICALLY FALL, AND LEAVES THE ONES THAT ARE STILL HELD UP EXACTLY WHERE THEY WERE.
 *
 * THIS IS THE TEST THAT WOULD HAVE CAUGHT THE BUG A PLAYER FOUND. Every link of the click chain
 * was covered and green: the trace resolves, the menu filters, the commit removes the piece,
 * destroys the orphaned actor and re-solves the wall. The end-to-end tests over that chain assert
 * the MODEL — IsPieceRemoved, GetPieceSupport reading Falling, NumLivePieces — and
 * World.Choose.ChoosingDeleteTakesTheBrickOutOfTheWall asserts that the re-solve happened without
 * ever asserting that a brick MOVED. Meanwhile World.Push.LosingASupportDropsExactlyTheOrphans
 * does assert bricks fall, but reaches them by calling SolveAndPush directly rather than by going
 * through a commit. So the outcome assertion and the click path lived in different tests, and the
 * one line between them — the push that tells the world what the solver worked out — was never
 * composed by anything. That is DESIGN.md §4's mechanism-versus-outcome distinction reappearing
 * as a COMPOSITION gap rather than as a wrong assertion, and only a test that takes the real path
 * and then looks at where the bricks ended up can close it.
 *
 * BOTH HALVES OF THE MOVEMENT CLAIM, AND NEITHER IS DECORATION. "The orphans fell" alone passes in
 * a world that dropped through the floor, and "nothing else moved" alone passes in a world where
 * nothing moved at all. Both are read off the same second of the same simulation.
 *
 * AND THE WIRE IS ASSERTED BESIDE THE OUTCOME, because they are different failures with different
 * fixes: IsReleased is the binding's record that the solver's answer was APPLIED, and
 * IsSimulatingPhysics is the actor's own record that physics was TOLD. ABrickActor deliberately
 * keeps no second copy of that flag, so a commit that set every latch and called nothing is caught
 * on the body alone. Displacement is emphatically not being used as a break assertion anywhere
 * here: nothing in this test claims a JOINT gave, only that pieces with no remaining path to the
 * ground were handed to physics and then behaved like it.
 *
 * NEEDS A TICKING WORLD: YES, and that is the point of it. A fixed second of simulated time on a
 * fixed step, never a settle poll — settling is non-deterministic, and a poll turns a real failure
 * into a timeout, which reports far worse than an assertion.
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
	 * THREE COURSES, SO THE WAIST IS PIECE 2 AND THE ORPHANS ARE 3 AND 4.
	 *
	 * 3 and 4 lose their only bed joint beneath and fall back on the head joint between them, so
	 * each names the other as its support. That looks like a cycle and is not reported as one:
	 * the walk considers only supports that themselves reach the ground, and neither of these
	 * does, so both read plain Falling. 0 and 1 are grounded and stay where they were laid.
	 */
	const FRunningBondSpec Spec = NarrowWaistWallSpec(3);

	constexpr int32 PieceCount = 5;
	constexpr int32 WaistPiece = 2;
	constexpr bool bOrphaned[PieceCount] = { false, false, false, true, true };

	/*
	 * THE REFERENCE LAYOUT IS LAID SEPARATELY, so the point pointed at comes from the producer
	 * rather than from whatever the subsystem happened to spawn. A spawner that put every brick
	 * at the origin would otherwise be pointed at the origin and agree with itself.
	 */
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

	/*
	 * THE WALL IS PUT INTO THE STATE THE GAME MODE PUTS IT IN ON BEGIN-PLAY, which is the state a
	 * player's first click actually arrives at. It is also the fixture precondition: a wall that
	 * came apart on its own would make everything below measure something else entirely.
	 */
	TestEqual(
		TEXT("fixture: the wall as built should stand, so starting it up releases nothing"),
		TestWorld.Subsystem->SolveAndPush(StructureId), 0);

	ReportIntegrationState(*this, *Binding, Bricks, TEXT("as built"));

	for (int32 Piece = 0; Piece < PieceCount; ++Piece)
	{
		/*
		 * THE POSITIVE CONTROL. Pieces 3 and 4 have to be held up NOW for their falling later to
		 * be something the deletion caused rather than something that was always true.
		 */
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

	/* THE PLAYER'S MOVE, AND THE ONLY THING BELOW THAT TOUCHES THE WALL AT ALL. */
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
	 * THE WIRE: THE MODEL KNOWING IS NOT THE WORLD BEING TOLD, and these are the two records that
	 * say which of the two happened. GetPieceSupport is read first because if IT is wrong the two
	 * rows beneath are failing for a different reason entirely — a re-solve that did not happen
	 * rather than a push that did not happen.
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
	 * THE OUTCOME. The course pitch is 6.5 cm of brick plus a 1 cm joint, so course centres sit
	 * at Z = 3.25 + 7.5 x Course and course 1 spans Z 7.5..14. Deleting it leaves pieces 3 and 4
	 * to drop from an underside of Z = 15 onto the top of course 0 at Z = 6.5 — A DROP OF 8.5 cm,
	 * against a 5 cm threshold that is itself five times the 1 cm mortar joint a brick merely
	 * settling into its own gap could move. Free fall over 8.5 cm takes 0.13 s, so a second of
	 * simulated time lands and settles them with room to spare.
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

			/* And it landed on the wall rather than through the world. */
			TestTrue(
				*FString::Printf(TEXT("orphaned brick %d should have come to rest above the floor at Z %g, it is at Z %.3f"),
					Piece, FloorTopZCm, NowAt.Z),
				NowAt.Z > FloorTopZCm);
		}
		else
		{
			/*
			 * THE OTHER HALF, AND IT IS NOT DECORATION. Without it, a world in which everything
			 * fell through the floor passes the rows above perfectly well.
			 */
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
 * TWO: COLLAPSE. PULLING BRICKS OUT THROUGH THE REAL PATH LEAVES THE WALL STANDING UNTIL THE ONE
 * THAT TAKES ITS LAST PATH TO THE GROUND, AND THEN EVERYTHING ABOVE COMES DOWN.
 *
 * THIS IS DESIGN.md §4's HEADLINE INTEGRATION TEST — "pull bricks until it topples; confirm it
 * falls at the PREDICTED number" — and the prediction is what makes it more than the journey test
 * above. Piece 2 spans pieces 0 and 1, so taking 0 gives it a hole to span and nothing else: the
 * wall must stand, and be watched standing under real gravity rather than merely asserted about.
 * Taking 1 as well leaves piece 2 with no bed joint beneath it and no head joint at all, because
 * its course is one brick wide — and the whole remainder of the wall goes.
 *
 * STANDS AT 1 REMOVED, FALLS AT 2, AND BOTH HALVES ARE IN THE SAME WORLD. The standing half is the
 * control that stops "everything falls" being the passing answer, and it is also what proves the
 * floor is under the wall — a world that had dropped through it would fail there rather than
 * flattering the collapse.
 *
 * THE PRECONDITION THAT MAKES IT HONEST: NO PIECE MAY BE Stranded AT THE MOMENT IT GOES. Stranded
 * means the solver declined to divide load round a knot — a limitation of the model rather than a
 * statement that anything lost its support — and a collapse fixture calibrated on one comes down
 * looking exactly the same while measuring something else entirely. CURRENT_STATE.md records that
 * trap against this very test; three bricks with no load path at all is the shape that avoids it.
 *
 * AND NOTHING HERE ASSERTS THAT A JOINT GAVE, DELIBERATELY. A mortared brick wall this size sits
 * at roughly 0.005 of capacity, so gravity alone breaks nothing whatsoever — asserting HasGiven or
 * a break stamp would be asserting something the physics cannot produce, and any test that did
 * would be red for a reason no implementation should fix. THIS COLLAPSE IS LOSS OF SUPPORT. The
 * strength-driven collapse, where a joint is loaded past its own capacity and the cascade takes
 * over, is a different and later test with a different fixture.
 *
 * NEEDS A TICKING WORLD: YES. Half a second watching a wall not move, and a second watching it
 * come down, both on a fixed step.
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

	/* Four courses, so there is a piece above the pair that hangs off the waist. */
	const FRunningBondSpec Spec = NarrowWaistWallSpec(4);

	constexpr int32 PieceCount = 6;

	/** The two grounded bricks, pulled in this order: the wall survives the first and not the second. */
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

	/*
	 * ONE BRICK OUT, AND THE WALL IS STILL A WALL. Piece 2 keeps its bed joint onto piece 1, so
	 * everything above it keeps a path to the ground and NOTHING may move.
	 */
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

	/*
	 * AND THE SECOND ONE IS THE LAST STRAW. Piece 2's course is one brick wide, so with both
	 * grounded bricks gone it has neither a bed joint beneath it nor a head joint to hang from,
	 * and pieces 3, 4 and 5 lose the ground with it.
	 */
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

		/*
		 * THE PRECONDITION THAT MAKES THE COLLAPSE HONEST. A Stranded piece is one the solver
		 * declined to route load around; a wall that came down for that reason is a model
		 * limitation wearing a collapse's clothes.
		 */
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

		/*
		 * AND IT LANDED. The bottom course is gone, so the rubble comes to rest on the floor at
		 * Z = -50 rather than continuing forever; a brick still below that has fallen THROUGH the
		 * world, which the row above cannot tell from a collapse.
		 */
		TestTrue(
			*FString::Printf(TEXT("brick %d should have landed on the floor at Z %g, it is at Z %.3f"),
				Piece, FloorTopZCm, NowAt.Z),
			NowAt.Z > FloorTopZCm);
	}

	TestWorld.End();

	return true;
}

/**
 * THREE: REDISTRIBUTION. TAKING A BRICK OUT OF A WALL THAT CAN SPARE IT MOVES NOTHING AT ALL, AND
 * THE LOAD IT WAS CARRYING TURNS UP ON ITS NEIGHBOURS.
 *
 * THIS IS THE CONTROL, AND WITHOUT IT THE OTHER TWO ARE CHEAP. "Release everything the binding
 * knows about" satisfies every fall assertion in this file; only a wall that must NOT come apart,
 * reached by the same player action through the same commit path, can tell that implementation
 * from a correct one. It is DESIGN.md §4's second integration case — "remove one brick; read the
 * actual strain on surrounding connections; the wall doesn't move; ANY BRICK DRIFTING IS A HARD
 * FAIL" — and the kinematic model exists precisely so that "doesn't move" can mean zero rather
 * than "settles to within a few millimetres".
 *
 * GREEN ON ARRIVAL, AND SAID PLAINLY. Nothing loses its support here, so a commit path that never
 * pushed anything to the world passes this test — that is what makes it a control and a
 * regression net rather than a driver. It bites in the other direction: the mutation that makes
 * the two tests above pass for the wrong reason is exactly the one this fails.
 *
 * THE LOAD CLAIM IS EXACT, NOT AN INEQUALITY, and the arithmetic is worked out here rather than
 * read back off the solver. Pieces 10 and 11 each rest on two course-1 bricks; the bed joints are
 * the same 10.25 x 10.25 cm overlap either side, so DESIGN.md §3's area-weighted split is exactly
 * even and each joint carries half. Piece 10 carries its own weight plus half of piece 14's and
 * half of piece 15's, which is 2W for a wall of full bricks — so each of its two bed joints
 * carries exactly W before, and the survivor carries exactly 2W after. Same for piece 11 either
 * side of piece 7. W is 2.72163125 kg x 980 cm/s2 = 2667.198625 Unreal force units, and DESIGN.md
 * §3's 1 N = 100 uu is already inside that product and must not be applied again. An inequality
 * would pass for a joint that took on a THIRD of the load as happily as one that took all of it.
 *
 * NEEDS A TICKING WORLD: YES, and it is the half of the claim that a world-free test cannot make.
 * The redistribution arithmetic is already covered by Structure.RemovalRedistributesLoad; what is
 * here and nowhere else is that a real wall in a real physics scene, driven by a real click, sits
 * there afterwards.
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

	/*
	 * FIXTURE PRECONDITIONS ON THE TOPOLOGY, because every expected value below is derived from a
	 * hand-read of which brick lands on which. If the producer ever lays this wall differently,
	 * these say so in one line rather than letting the force assertions fail with a plausible
	 * wrong number and send somebody looking at the solver.
	 */
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

	/*
	 * AND THE SPLIT IS EVEN, WHICH IS WHY THE EXPECTED FORCES ARE EXACTLY W AND 2W. Equal areas
	 * either side is the whole basis of the arithmetic in this test's header; unequal ones would
	 * make both expected values wrong without changing a single count.
	 */
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

	/* THE SAME PLAYER ACTION AS THE OTHER TWO, ON A BRICK THE WALL CAN SPARE. */
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

	/*
	 * THE LOAD MOVED ONTO THE NEIGHBOURS. Each carrier's two bed joints split its 2W evenly, so
	 * the survivor takes the lot: exactly twice what it carried, not merely more than it.
	 */
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

	/*
	 * AND THE WALL DID NOT MOVE. A full second under real gravity, on a fixed step, and any brick
	 * drifting is a hard fail — DESIGN.md §4's words, and the reason the kinematic model was
	 * chosen over Chaos constraints in the first place (0.000000 cm against 0.62-0.70 cm).
	 */
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
 * FOUR: THE BATCH. PICKING SEVERAL BRICKS AND CHOOSING DELETE ONCE TAKES ALL OF THEM OUT, DROPS
 * EVERYTHING THEY WERE HOLDING UP, MOVES NOTHING WHEN THE WALL CAN SPARE THEM — AND COSTS ONE
 * SOLVE RATHER THAN ONE PER BRICK.
 *
 * WHY THIS IS IN THE INTEGRATION SET AND NOT BESIDE THE ARITHMETIC. What can be wrong here is a
 * CALL THAT NOBODY MAKES: Core.PieceActions.BatchRunsEveryPieceAndSolvesOnce already pins that
 * the batch runs every action and solves once, at the end — and that is a claim about a graph.
 * What no world-free test can reach is whether the answer that one solve produced is then PUSHED
 * onto the world, for every piece the batch orphaned rather than for the ones a mid-way solve
 * happened to know about. FStructureBinding::ApplyResults refuses to release any piece the LAST
 * solve has no answer for, so a batch that solved at the wrong moment does not crash, does not
 * fail a count, and leaves bricks hanging in the air exactly as the defect a player found in ten
 * seconds did. Only a real wall, entered through the player's own calls and then watched under
 * gravity, can tell that from a correct one.
 *
 * BOTH DIRECTIONS, IN ONE WORLD, IN THIS ORDER, AND NEITHER IS DECORATION. The first batch is one
 * the wall SURVIVES: two bricks that between them orphan nothing, so a full second of gravity
 * must move nothing at all — which is what stops "release everything the binding knows about"
 * being a passing implementation of the second half. The second batch is the whole grounded
 * course, after which nothing that is left touches the earth, so every remaining brick must
 * physically fall. Running them against the same wall is also what makes the first batch's
 * removals count: the second batch is solved on a wall with two holes already in it.
 *
 * THE ONE-SOLVE CLAIM IS ASSERTED HERE TOO, AND IT IS THE ONLY MECHANISM ROW IN IT. This set
 * asserts outcomes, but a cost cannot be an outcome: solving is deterministic, so five solves and
 * one produce identical bricks in identical places. FStructure::NumSolves is the seam, read
 * either side of the player's own click, and without it "the commit is batched" is a comment
 * rather than a requirement — which matters because the batched path is measurably the reason to
 * have multi-select at all: a full solve is tens of milliseconds at scenario scale, so five
 * deletes done one at a time is a visible stutter for an answer one pass already had.
 *
 * WHAT IS DELIBERATELY NOT ASSERTED. That the push happens exactly ONCE: a second push is
 * idempotent by construction (ApplyResults releases each piece once and ABrickActor::Release
 * returns early on an already-simulating body), so it is unobservable from outside, and an
 * assertion that cannot fail is worse than none. And no claim is made here about a joint GIVING —
 * a mortared wall this size sits at roughly 0.005 of capacity, so this collapse is loss of
 * support, exactly as the collapse test above.
 *
 * NEEDS A TICKING WORLD: YES. Half a second watching a wall not move and a second watching it
 * come down, both on a fixed step, never a settle poll.
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

	/*
	 * ONE: A BATCH THE WALL SURVIVES. Two bricks picked, one Delete chosen, and a second of real
	 * gravity in which nothing may move a millimetre.
	 */
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

	/*
	 * THE COST CLAIM, AND IT IS WHY THE BATCH EXISTS. One click of one button against a selection
	 * of N must re-solve the wall once, not N times — a per-piece commit reaches the same wall at
	 * N times the price, and nothing else about the wall can tell you which happened.
	 */
	TestEqual(
		FString::Printf(
			TEXT("deleting %d bricks in ONE batch must cost exactly one solve, not one per brick; it cost %d"),
			WideWallSurvivableBatch.Num(), SolvesSpentOnFirstBatch),
		SolvesSpentOnFirstBatch, 1);

	/* EVERY PICKED BRICK WENT, not just the last one picked. */
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

	/*
	 * TWO: AND A BATCH THE WALL CANNOT SURVIVE. The whole grounded course, picked one brick at a
	 * time and deleted with one choice — after which nothing left in the wall touches the earth.
	 */
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
	 * THE WIRE, BEFORE THE OUTCOME. The model knowing a brick has lost the ground is not the same
	 * as the world having been told, and these are the two records that say which of the two
	 * happened: IsReleased is the binding's record that the last solve's answer was APPLIED, and
	 * IsSimulatingPhysics is the body's own record that physics was told. A push that ran behind
	 * a mistimed solve fails exactly here, with every count in the wall still agreeing.
	 *
	 * The support row asks only whether the piece is HELD UP. Whether a mutually head-jointed pair
	 * with no ground reads Falling or Stranded is a question about the solver rather than about
	 * batching, and Integration.PullingSupportBringsTheWallDown is where it is pinned.
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

	/* AND THE OUTCOME: it actually comes down. */
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

		/*
		 * AND IT LANDED. The grounded course is gone, so the rubble comes to rest on the floor
		 * rather than continuing forever; a brick below that has fallen THROUGH the world, which
		 * the row above cannot tell from a collapse.
		 */
		TestTrue(
			*FString::Printf(TEXT("brick %d should have landed on the floor at Z %g, it is at Z %.3f"),
				Piece, FloorTopZCm, NowAt.Z),
			NowAt.Z > FloorTopZCm);
	}

	TestWorld.End();

	return true;
}

/**
 * FOUR: THE STAIRCASE. CUTTING A STEPPED DIAGONAL VOID THROUGH A WALL LEAVES THE BRICKWORK ABOVE IT
 * CORBELLED OUT OVER NOTHING, AND IT MUST STAND THERE.
 *
 * THIS TEST USED TO ASSERT THE OPPOSITE, AND THE INVERSION IS THE USER'S RULING OF 2026-08-06
 * RATHER THAN A CAPITULATION. It was named AStaircaseVoidBringsTheOverhangDown and it required all
 * eleven corbelled bricks to fall. The player was then shown what the same model does to a brick
 * deleted at the END of a wall — the failure walks up the wall in a stepping triangle and takes its
 * upper half — and ruled that this must not happen. The two are LOCALLY INDISTINGUISHABLE: a
 * half-seated brick overhanging outward with nothing to abut against is the same picture either
 * way, so any rule local enough to save the free end also saves this corbel. Composite vertical
 * action is that rule, and the ruling adopts it knowing exactly what it costs here.
 *
 * THE MECHANISM, so that "it stands" is a claim and not an absence of one. A stack of courses over
 * a raking cut does not resist its overturning moment as a sequence of independent bed patches: the
 * wall acts as a DEEP BEAM, and the plane taking the moment is a vertical section through the
 * bonded masonry standing over the joint — 11,627 cm3 through eleven courses against one patch's
 * 179.48. The bottom rung reads 0.369 of capacity rather than 22.93, and nothing in the ladder
 * reaches 1.0. Tests/StaircaseWallTestSupport.h works both readings out beside the geometry they
 * belong to, because three tests cut this same void and they must all read one derivation.
 *
 * THE RULING IS INTERNALLY CONSISTENT, WHICH IS EVIDENCE RATHER THAN PREFERENCE. The same user
 * independently agreed acceptance case 20 — this same staircase void — as LOCAL LOSS: the loose
 * teeth left with no bed patch at all drop, and the mass stands. Composite action produces exactly
 * that, and on THIS wall there are no such teeth, so nothing at all comes down.
 *
 * NON-DISPLACEMENT IS A LEGITIMATE ASSERTION WHERE DISPLACEMENT WOULD NOT BE. DESIGN.md §4 bans
 * displacement as evidence that a joint BROKE, because two pieces can sever and rest exactly where
 * they were. The converse is safe and is what is asserted here: a brick that has not moved has not
 * been released, whatever its joints did. The half that displacement cannot answer — that no joint
 * gave — is asserted separately off the break stamps.
 *
 * AND THAT IS READ OFF THE BREAK STAMPS RATHER THAN OFF THE UTILISATION, because the cascade runs
 * inside the commit and a given joint carries exactly nothing (DESIGN.md §3). Every one of the
 * eleven rungs must be unstamped; the ladder's MAGNITUDES are pinned world-free in
 * DestructionGame.Core.Structure.AStaircaseVoidCondemnsTheCorbel, which reads the same eleven
 * joints off SolveLoads, which breaks nothing. The long comment at that assertion says why.
 *
 * WHAT WOULD MAKE THIS TEST VACUOUS, AND WHAT STOPS IT. "Nothing fell" is also true of a wall that
 * was never in the load path, of a wall nobody cut, and of a subsystem that has stopped running at
 * all. So the cut is asserted to have removed its 36 bricks and their actors; the corbel joints are
 * asserted to exist and to be intact; and the world-free sibling asserts that those joints carry
 * the full eleven-rung ladder, 38.5 brick weights and 1608.75 brick-weight-centimetres at the
 * bottom. The mass is being carried, and it is being carried by the joints this test names.
 *
 * AND THE PRECONDITION THAT MAKES IT HONEST, EXACTLY AS THE COLLAPSE TEST ABOVE: NO SURVIVING PIECE
 * MAY BE Stranded. A staircase void is precisely the shape that produces unroutable knots — cut two
 * bricks in a row out from under a course and the pair above fall back on each other's head joints,
 * each naming the other as its support — and a wall that came down because the solver declined to
 * divide load round a loop is a model limitation wearing a collapse's clothes. The corbel is built
 * from single bed joints for that reason: every step of it is statically determinate.
 *
 * A FLUSH WALL, AND THAT IS LOAD-BEARING. A ragged wall's alternate courses step in, so the end
 * brick of every even course already rests on one brick instead of two — the same 5.625 cm
 * eccentricity, at the wall's own end, with nobody having done anything to it. A 13-course ragged
 * wall reads 0.058 of capacity as built, and a taller one reads something else again.
 * StaircaseWallSpec says the rest.
 *
 * NEEDS A TICKING WORLD: YES, and this one could not be anywhere else. That the corbel joint is at
 * 0.369 is arithmetic on a graph and belongs in the fast suite — and lives there; that a wall in a
 * real scene, cut by a real click, is then still standing a second later is the composition, and
 * the composition is what no world-free test can reach.
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
	 * THE STAIRCASE, READ OFF THE LAID WALL RATHER THAN LISTED. Course c keeps everything from
	 * (12 - c) x 11.25 cm rightward, so the surviving left edge steps out half a brick pitch per
	 * course and the void underneath it is the stepped diagonal in the picture:
	 *
	 *     course 12  [][][][][][][][][][]              whole, and already corbelled
	 *     course 11    ..[][][][][][][][][]
	 *     course 10  ....[][][][][][][][][]
	 *      ...
	 *     course  2  ..........[][][][][][]
	 *     course  1    ..........[][][][][]
	 *     course  0  [][][][][][][][][][]              grounded, whole
	 *
	 * 36 bricks, taken out in ONE batch: one selection, one click of one button, which is both the
	 * player's own move and the only route any test in this file takes to change a wall.
	 */
	const TArray<int32> VoidPieces = StaircaseVoidPieces(Reference.Boxes);

	if (VoidPieces.Num() != StaircaseVoidPieceCount)
	{
		AddError(FString::Printf(TEXT("fixture: the staircase should cut %d bricks, it names %d"),
			StaircaseVoidPieceCount, VoidPieces.Num()));

		return true;
	}

	/* The eleven corbelled bricks, and the eleven bed joints that are the only thing holding them. */
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

	/* Three bricks up the far end of the wall, which the staircase never reaches. */
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

	/*
	 * THE ONE FIXTURE CHECK WITHOUT WHICH THIS TEST MEASURES NOTHING. A piece nobody placed and a
	 * joint that never measured its own face both answer a centred load — correctly, and bit for
	 * bit as they did before moments existed — so a wall laid without geometry reads perfectly
	 * healthy while every corbel in it is levering its joint open. HasCompleteGeometry is the only
	 * thing that can tell "the load is centred" from "nobody said where it acts".
	 */
	TestTrue(
		TEXT("fixture: the laid wall must know where its pieces and its joints are, or every moment below is silently zero"),
		Binding->GetStructure().HasCompleteGeometry());

	TestEqual(
		TEXT("fixture: the wall as built should stand, so starting it up releases nothing"),
		TestWorld.Subsystem->SolveAndPush(StructureId), 0);

	/*
	 * AND NOTHING IS OVERLOADED BEFORE THE CUT. This is the positive control for the whole test: a
	 * wall that arrived with a joint past capacity would come down for a reason the staircase had
	 * nothing to do with, and the flush end is what buys this — see StaircaseWallSpec.
	 */
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

	/* THE PLAYER'S MOVE, AND THE ONLY THING BELOW THAT TOUCHES THE WALL AT ALL. */
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
	 * THE LADDER, READ OFF THE BREAK STAMPS RATHER THAN OFF THE UTILISATION — AND THE REASON IS
	 * THE CONTRACT RATHER THAN A WORKAROUND.
	 *
	 * This precondition used to read GetConnectionUtilisation on each corbel joint and require the
	 * bottom rung to be past 1.0. It cannot any more, and nothing production does is wrong: the
	 * cascade now runs INSIDE the commit, so by the time the click has returned those joints have
	 * already given, and DESIGN.md §3 is explicit that a given joint carries exactly nothing and
	 * that only the BREAKING call reports the ratio that broke it. The ladder therefore reads zero
	 * all the way up, and it would read zero for a wall that was never loaded at all. Any wire
	 * that makes the overhang fall breaks these joints before a test can look at them, so the
	 * question has to be asked of a quantity that survives the breaking.
	 *
	 * GetBreakPass IS THAT QUANTITY, AND IT IS A STRONGER CLAIM THAN THE ONE IT REPLACES. It says
	 * WHICH joints gave and IN WHAT ORDER, and it distinguishes all three states with no sentinel:
	 * a joint that went with a removed piece carries no pass number at all, so a corbel joint
	 * stamped with a pass is a joint that FAILED UNDER LOAD rather than one that was deleted.
	 *
	 * PASS 1 IS THE FIRST SWEEP AFTER THE CUT, so what it breaks is exactly what the first solve
	 * found over capacity — which is exactly the ladder StaircaseWallTestSupport works out by
	 * hand, and under composite action that ladder condemns NOTHING. The wall as built is asserted
	 * above to have nothing over capacity and to release nothing, so no pass can have been stamped
	 * before this one and the numbering starts at 1.
	 *
	 * THE LOOP IS WRITTEN AGAINST StaircaseCorbelIsCondemned RATHER THAN AGAINST "none of them",
	 * DELIBERATELY, AND IT IS NOT DEAD CODE. It reads the same oracle the world-free sibling reads,
	 * so the day that oracle condemns a rung again — a deeper wall, a shallower depth rule, a
	 * strength change — this test starts requiring that rung to break instead of quietly continuing
	 * to require that nothing does. A hard-coded zero would make the two files disagree in silence,
	 * which is the exact failure the shared header exists to prevent.
	 *
	 * THE MAGNITUDE — 0.369 at the bottom rung and the whole eleven-rung ladder — is asserted in
	 * DestructionGame.Core.Structure.AStaircaseVoidCondemnsTheCorbel, which cuts the same void into
	 * the same wall with no world at all and reads the ladder off SolveLoads, which breaks nothing.
	 * That is where the arithmetic belongs, and it is what stops "nothing broke" here from being
	 * true of a wall carrying nothing.
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

	/*
	 * AND THE COUNT, WHICH IS THE HALF THE PER-RUNG ROWS CANNOT MAKE. None of eleven is the
	 * fixture's own claim, and a ladder that crossed 1.0 somewhere else entirely would still
	 * satisfy every row above if the rungs it broke happened to be the ones it predicted.
	 */
	AddInfo(FString::Printf(
		TEXT("the staircase's first sweep broke %d of %d corbel joints (the arithmetic predicts %d, worst rung %.8f)"),
		CorbelJointsBrokenInFirstPass, CorbelPieces.Num(),
		StaircasePredictedCorbelJointsOverCapacity, StaircasePredictedWorstCorbelUtilisation));

	TestEqual(
		*FString::Printf(
			TEXT("the first sweep after the cut must break exactly the %d corbel joints the arithmetic condemns"),
			StaircasePredictedCorbelJointsOverCapacity),
		CorbelJointsBrokenInFirstPass, StaircasePredictedCorbelJointsOverCapacity);

	/*
	 * AND NO SURVIVOR IS Stranded. A staircase void is exactly the shape that makes unroutable
	 * knots, and a wall that comes down because the solver declined to divide load round a loop is
	 * a model limitation wearing a collapse's clothes.
	 */
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

	/*
	 * THE OUTCOME: the overhang is over open air, and a second later it must still be there.
	 *
	 * A SECOND OF REAL TIME IS THE POINT. The wall is ticked exactly as long as the version of this
	 * test that required the overhang to FALL ticked it, and that duration was chosen to be several
	 * times longer than the drop it was measuring — so a brick that has been released has ample
	 * time to leave, and one that has not moved in that second was never released. Asserting
	 * non-displacement is safe in the direction asserting displacement is not: DESIGN.md §4 bans
	 * reading a movement as evidence a joint BROKE, because two pieces can sever and stay put; the
	 * converse does not have that hole.
	 */
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

		/*
		 * AND IT IS STILL BEING HELD UP RATHER THAN MERELY RESTING WHERE IT LANDED. A released
		 * brick that happened to jam against its neighbours would satisfy the row above; a
		 * kinematic mesh is one the subsystem never let go of.
		 */
		TestTrue(
			*FString::Printf(TEXT("corbelled brick %d is still held up and must still be kinematic"), Piece),
			Bricks[Piece]->GetMesh() != nullptr && !Bricks[Piece]->GetMesh()->IsSimulatingPhysics());
	}

	/*
	 * THE FAR END, AND IT IS NOT DECORATION EVEN NOW. It was the guard against an implementation
	 * that released the WHOLE wall while the corbel rows only asked about the overhang; with those
	 * rows inverted it is the guard against the opposite failure — a subsystem that has stopped
	 * releasing anything at all would pass both halves, and only the cut's own 36 deleted actors
	 * and the eleven intact-joint rows above say that anything happened.
	 */
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
