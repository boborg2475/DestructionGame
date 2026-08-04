// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceActions.h"
#include "Core/PieceMenu.h"
#include "DestructionGamePlayerController.h"
#include "Tests/BrickWorldTestSupport.h"

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
	 * BY PIECE PAIR RATHER THAN BY INDEX, because a joint index is an artefact of the order the
	 * producer happened to emit pairs in, and a test that hard-coded one would silently start
	 * watching a different joint the day that order changed. Both orientations are accepted:
	 * which piece is A and which is B is the producer's business, and DESIGN.md §3 is explicit
	 * that a consistently oriented joint reports identical loads either way.
	 */
	int32 FindIntegrationJoint(const FStructure& Structure, int32 FirstPiece, int32 SecondPiece)
	{
		for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
		{
			const FConnection& Joint = Structure.GetConnection(Index);

			if ((Joint.PieceA == FirstPiece && Joint.PieceB == SecondPiece)
				|| (Joint.PieceA == SecondPiece && Joint.PieceB == FirstPiece))
			{
				return Index;
			}
		}

		return INDEX_NONE;
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
	 * WHAT A PLAYER DOES, IN ONE CALL: point at a brick, and choose Delete off the menu that
	 * comes up.
	 *
	 * THIS IS THE ONLY ROUTE ANY TEST IN THIS FILE TAKES TO CHANGE A WALL, and that is the point
	 * of the helper rather than a convenience: it is impossible to write a test in this file that
	 * accidentally reaches past the presenter into RemovePiece or SolveAndPush, because there is
	 * nothing here that does. The two inches in front of it that a headless run cannot reach are
	 * the deprojection that turns a cursor into this ray, and the button that supplies this index.
	 *
	 * THE ROW IS FOUND BY ACTION POINTER, NOT ASSUMED TO BE ROW 0. PieceActionsFor hands back
	 * pointers into the shipped table precisely so a presenter can compare identities, and the
	 * day the table grows a second row this keeps choosing Delete instead of whatever sorted
	 * first.
	 */
	bool InspectAndChooseDelete(
		FAutomationTestBase& Test,
		ADestructionGamePlayerController& Controller,
		const FPieceAction& Delete,
		int32 StructureId,
		const FPieceBox& Box,
		int32 ExpectedPiece)
	{
		const TArray<FPieceMenuRow> Rows = Controller.InspectAlongRay(
			FVector(Box.CentreCm.X, Box.CentreCm.Y - IntegrationReachCm, Box.CentreCm.Z),
			FVector(Box.CentreCm.X, Box.CentreCm.Y + IntegrationReachCm, Box.CentreCm.Z));

		int32 DeleteRow = INDEX_NONE;

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
		 * THE ROW MUST NAME THE BRICK THAT WAS POINTED AT. A chain wired to the wrong brick puts
		 * up a perfect menu and deletes somebody else, and every count in the wall still agrees.
		 */
		Test.TestTrue(
			*FString::Printf(
				TEXT("the Delete row offered for piece %d should name {%d,%d}, it names {%d,%d}"),
				ExpectedPiece, StructureId, ExpectedPiece,
				Rows[DeleteRow].Ref.StructureId, Rows[DeleteRow].Ref.PieceIndex),
			Rows[DeleteRow].Ref.StructureId == StructureId
				&& Rows[DeleteRow].Ref.PieceIndex == ExpectedPiece);

		const bool bChose = Controller.ChoosePieceMenuRow(DeleteRow);

		Test.TestTrue(
			*FString::Printf(TEXT("choosing Delete on piece %d should report that it committed"),
				ExpectedPiece),
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
	 * Slack on a joint force of a few thousand Unreal force units, not signal.
	 *
	 * The expected values below are exact — a handful of doubles added and halved — so this is
	 * four decimal places of headroom on a quantity in the thousands, and it is loose enough that
	 * a reassociation cannot fail it and tight enough that a wrong SHARE cannot pass it.
	 */
	constexpr double JointForceToleranceUnrealUnits = 0.01;
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
			*this, *Controller, *Delete, StructureId, Reference.Boxes[WaistPiece], WaistPiece))
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
			*this, *Controller, *Delete, StructureId, Reference.Boxes[FirstPulled], FirstPulled))
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
			*this, *Controller, *Delete, StructureId, Reference.Boxes[LastStraw], LastStraw))
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
			Reference.Boxes[WideWallRemovedPiece], WideWallRemovedPiece))
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

#endif // WITH_DEV_AUTOMATION_TESTS
