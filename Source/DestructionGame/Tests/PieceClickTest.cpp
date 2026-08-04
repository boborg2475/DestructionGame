// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceActions.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges
 * many files into one. The world harness itself is NOT redeclared here: it lives in
 * Tests/BrickWorldTestSupport.h and is shared with Tests/BrickActorTest.cpp and
 * Tests/StructurePushTest.cpp, because a second copy of a floor height and a tick length is
 * two fixtures that drift. Only what is specific to clicking is below.
 */
namespace PieceClickTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/**
	 * THE SMALLEST WALL WITH A WAIST, and the waist is the whole reason for a fixture of its
	 * own rather than BrickWorldTestSupport::WallSpec.
	 *
	 * The claim this test has to make about committing is that the wall was RE-SOLVED behind
	 * the deletion, and the only thing that can see a re-solve from outside is a piece whose
	 * support answer changes. In the shared flush 2 x 3 wall nothing's answer changes when one
	 * brick goes: a full brick spans two below it and keeps the other, and a half bat with no
	 * bed joint left falls back on the head joint to its neighbour. Both are right physics and
	 * both make the re-solve invisible.
	 *
	 * Two bricks per course is the narrowest bond RunningBond will lay, and ragged means the
	 * odd course is one brick short — so three courses give
	 *
	 *      course 2         [ 3 ][ 4 ]      head joint 3-4 between them
	 *      course 1            [ 2 ]        THE WAIST — clicked and deleted
	 *      course 0         [ 0 ][ 1 ]      grounded
	 *
	 * and everything above course 0 reaches the ground only through piece 2. Deleting it
	 * leaves 3 and 4 with each other's head joint and nothing else, so both read Falling.
	 * Three courses rather than the four Tests/StructurePushTest.cpp uses, because nothing
	 * here ticks physics and a fifth and sixth piece would only cost spawn time.
	 */
	FRunningBondSpec ClickWallSpec()
	{
		FRunningBondSpec Spec;
		Spec.BrickSizeCm = FVector(21.5, 10.25, 6.5);
		Spec.JointThicknessCm = 1.0;
		Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
		Spec.CoursesHigh = 3;
		Spec.BricksPerCourse = 2;
		Spec.End = EWallEnd::Ragged;
		Spec.Strength = GeneralPurposeMortar;
		return Spec;
	}

	/**
	 * FIXTURE PRECONDITIONS, asserted rather than assumed. 2 + 1 + 2 pieces, and the joints
	 * are the two head joints 0-1 and 3-4, and the four bed joints 0-2, 1-2, 2-3, 2-4. If a
	 * producer change moves either number, the arrangement this test reasons about is not the
	 * arrangement it built, and it should say so here rather than fail downstream with a
	 * plausible wrong answer.
	 */
	constexpr int32 ClickWallPieceCount = 5;
	constexpr int32 ClickWallJointCount = 6;

	/** The single course-1 brick everything above the bottom course hangs from. */
	constexpr int32 ClickWaistPiece = 2;

	/** Exactly the pieces that lose their path to the ground when the waist goes. */
	constexpr bool bClickOrphanedByTheWaist[ClickWallPieceCount] =
	{
		false, false, false, true, true
	};

	/**
	 * How far along Y a trace starts and ends, either side of the wall.
	 *
	 * A brick is 10.25 cm deep and the wall is centred on Y = 0, so +/- 100 cm is far outside
	 * it on both sides and the ray crosses the whole thickness. Along Y rather than along X or
	 * Z, so that nothing else in the wall is in the way and the answer is unambiguous.
	 */
	constexpr double ClickTraceReachCm = 100.0;

	/**
	 * A POINT OVER THE FLOOR AND CLEAR OF THE WALL.
	 *
	 * BrickWorldTestSupport's slab is scaled 40x from a mesh whose local bounds run 0..100, so
	 * it spans X 0..4000 and Y 0..4000 with its top at Z = -50. That is a KNOWN MISPLACEMENT —
	 * SM_Cube's pivot is a corner, so the slab does not straddle the origin and only the
	 * Y >= 0 half of any wall has floor beneath it. Nothing here needs a brick to land on it,
	 * so it is left exactly as it is; this point is simply chosen inside the half that
	 * genuinely has floor, and 100 cm out in Y from a wall that is 5.125 cm deep.
	 */
	const FVector ClickFloorPointCm(100.0, 100.0, 0.0);

	/** Far from the wall, above the floor's top face and above the slab's own footprint. */
	const FVector ClickEmptyAirPointCm(500.0, 500.0, 300.0);

	const TCHAR* ClickSupportName(EPieceSupport Support)
	{
		switch (Support)
		{
		case EPieceSupport::Grounded:  return TEXT("Grounded");
		case EPieceSupport::Supported: return TEXT("Supported");
		case EPieceSupport::Stranded:  return TEXT("Stranded");
		default:                       return TEXT("Falling");
		}
	}

	/** The labels a menu came back with, so a failure reads without a debugger. */
	FString DescribeClickMenu(const TArray<const FPieceAction*>& Menu)
	{
		if (Menu.Num() == 0)
		{
			return TEXT("<empty>");
		}

		FString Line;

		for (int32 Index = 0; Index < Menu.Num(); ++Index)
		{
			Line += FString::Printf(
				TEXT("%s%s"),
				Index == 0 ? TEXT("") : TEXT(", "),
				Menu[Index] != nullptr && Menu[Index]->Label != nullptr
					? Menu[Index]->Label
					: TEXT("<null row>"));
		}

		return Line;
	}

	/** The Delete row, looked up by label so nothing hard-codes a position in the table. */
	const FPieceAction* FindClickAction(const TCHAR* Label)
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
}

/**
 * CLICKING A BRICK RESOLVES TO THAT BRICK'S PIECE, THE MENU IS BUILT FROM WHAT THE TABLE
 * ALLOWS, COMMITTING DELETES IT AND RE-SOLVES THE WALL, AND THE ORPHANED ACTOR IS DESTROYED.
 *
 * ONE TEST BECAUSE ONE WORLD. A world test costs tens of milliseconds of setup here, and the
 * four claims below share a single wall in a single world — splitting them by assertion would
 * pay for the world four times over to learn nothing extra.
 *
 * THE TRACE IS ASSERTED PER PIECE, NOT ONCE. The defect that matters is a wall whose actors
 * were spawned in the right places but handed the wrong refs — every brick present, every
 * brick the right size, and clicking one deletes another. A single trace cannot see it; five
 * traces through five known box centres can, because the wrong answer has to be wrong for a
 * particular brick. Tests/BrickActorTest.cpp already traces its own wall this way and that
 * assertion was deliberately LEFT WHERE IT IS: it is checking the SPAWNER, that a brick's ref
 * agrees with its bounds, and it does its own trace and its own ResolvePiece. This test is
 * checking the CHAIN — that the subsystem turns a ray into a handle with every step failing
 * closed — so the two overlap in their fixture rather than in their subject.
 *
 * FAIL CLOSED IS ASSERTED ON THE THINGS A PLAYER ACTUALLY CLICKS: the floor, and thin air.
 * Both must produce no piece, no menu and no action — and the "no action" half is the one a
 * bare "it returned INDEX_NONE" would miss, because a commit path that ignored its ref could
 * still delete something.
 *
 * AND THE COMMIT IS WHERE ActorToDestroy IS FINALLY CONSUMED. RunPieceAction is world-free
 * and deliberately hands the orphan back rather than destroying it, so until something calls
 * it from a world the deleted brick's mesh stays standing with no piece naming it. Three
 * separate things are asserted about that, because each fails on its own: the piece left the
 * graph, the ACTOR left the world, and the wall was RE-SOLVED — that last one visible only
 * because the fixture has a waist, and nothing in this test calls SolveLoads after the commit.
 *
 * NEEDS A TICKING WORLD: it needs a WORLD — actors to spawn into, a physics scene for the
 * trace to query, and somewhere to destroy an actor — but it deliberately never ticks one.
 * Nothing here is about anything falling; that is Tests/StructurePushTest.cpp's subject, and
 * ticking would make this test slower and no more discriminating.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceClickResolvesAndCommitsTest,
	"DestructionGame.World.Click.ClickingABrickResolvesAndCommits",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceClickResolvesAndCommitsTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;
	using namespace PieceClickTestSupport;

	const FPieceAction* Delete = FindClickAction(TEXT("Delete"));

	if (Delete == nullptr || Delete->CanRun == nullptr || Delete->Run == nullptr)
	{
		AddError(TEXT("fixture: the action table must contain a well-formed row labelled 'Delete'"));
		return true;
	}

	const FRunningBondSpec Spec = ClickWallSpec();

	/*
	 * THE REFERENCE LAYOUT IS LAID SEPARATELY, so the points traced at come from the producer
	 * rather than from whatever the subsystem happened to spawn. A spawner that put every
	 * brick at the origin would otherwise be traced at the origin and agree with itself.
	 */
	FBrickLayout Reference;

	TestTrue(TEXT("fixture: RunningBond should lay the reference wall"), RunningBond(Spec, Reference));

	TestEqual(
		FString::Printf(TEXT("fixture: a ragged 3 x 2 wall should be %d pieces, got %d"),
			ClickWallPieceCount, Reference.Structure.NumPieces()),
		Reference.Structure.NumPieces(), ClickWallPieceCount);

	TestEqual(
		FString::Printf(TEXT("fixture: a ragged 3 x 2 wall should carry %d joints, got %d"),
			ClickWallJointCount, Reference.Structure.NumConnections()),
		Reference.Structure.NumConnections(), ClickWallJointCount);

	if (Reference.Boxes.Num() != ClickWallPieceCount)
	{
		return true;
	}

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	const int32 StructureId = TestWorld.Subsystem->BuildRunningBond(Spec);
	FStructureBinding* Binding = TestWorld.Subsystem->Find(StructureId);

	TestNotNull(
		*FString::Printf(TEXT("fixture: BuildRunningBond returned %d and Find should hand back its binding"),
			StructureId),
		Binding);

	if (Binding == nullptr || Binding->NumPieces() != ClickWallPieceCount)
	{
		TestWorld.End();
		return true;
	}

	TArray<ABrickActor*> Bricks;

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		ABrickActor* Brick = BrickAt(*this, *Binding, Piece);

		if (Brick == nullptr)
		{
			TestWorld.End();
			return true;
		}

		Bricks.Add(Brick);
	}

	/* The wall has to be solved before the fixture can claim anything about what holds it up. */
	Binding->SolveLoads();

	/*
	 * ONE: A TRACE THROUGH BRICK k RESOLVES TO PIECE k, FOR EVERY BRICK IN THE WALL.
	 */
	for (int32 Piece = 0; Piece < Reference.Boxes.Num(); ++Piece)
	{
		const FPieceBox& Box = Reference.Boxes[Piece];

		const FVector Start(Box.CentreCm.X, Box.CentreCm.Y - ClickTraceReachCm, Box.CentreCm.Z);
		const FVector End(Box.CentreCm.X, Box.CentreCm.Y + ClickTraceReachCm, Box.CentreCm.Z);

		const FPieceHit Hit = TestWorld.Subsystem->TracePiece(Start, End);

		TestEqual(
			FString::Printf(
				TEXT("a trace through piece %d's own box centre (%g, %g, %g) should resolve to handle %d, got %d"),
				Piece, Box.CentreCm.X, Box.CentreCm.Y, Box.CentreCm.Z, Piece, Hit.PieceHandle),
			Hit.PieceHandle, Piece);

		TestEqual(
			FString::Printf(TEXT("the trace at piece %d should carry this structure's id %d, got %d"),
				Piece, StructureId, Hit.Ref.StructureId),
			Hit.Ref.StructureId, StructureId);

		TestEqual(
			FString::Printf(TEXT("the trace at piece %d should carry piece index %d, got %d"),
				Piece, Piece, Hit.Ref.PieceIndex),
			Hit.Ref.PieceIndex, Piece);

		/*
		 * AND THE REF THAT COMES BACK IS THE ONE A MENU AND A COMMIT ARE BUILT FROM, so it
		 * has to resolve against the binding on its own account rather than only agreeing
		 * with the handle the subsystem reported.
		 */
		TestEqual(
			FString::Printf(TEXT("the ref the trace at piece %d handed back should resolve to %d, got %d"),
				Piece, Piece, Binding->ResolvePiece(Hit.Ref)),
			Binding->ResolvePiece(Hit.Ref), Piece);
	}

	/*
	 * TWO: A CLICK THAT MISSED EVERY BRICK PRODUCES NO PIECE, NO MENU AND NO ACTION.
	 */
	struct FMissCase
	{
		const TCHAR* Description;
		FVector Start;
		FVector End;
	};

	const TArray<FMissCase> Misses = {
		{
			TEXT("a click on the floor"),
			ClickFloorPointCm + FVector(0.0, 0.0, 200.0),
			ClickFloorPointCm + FVector(0.0, 0.0, -200.0)
		},
		{
			TEXT("a click on nothing at all"),
			ClickEmptyAirPointCm,
			ClickEmptyAirPointCm + FVector(0.0, 0.0, -100.0)
		},
	};

	for (const FMissCase& Miss : Misses)
	{
		const FPieceHit Hit = TestWorld.Subsystem->TracePiece(Miss.Start, Miss.End);

		TestEqual(
			FString::Printf(TEXT("%s must resolve to no piece, got handle %d"),
				Miss.Description, Hit.PieceHandle),
			Hit.PieceHandle, static_cast<int32>(INDEX_NONE));

		TestEqual(
			FString::Printf(TEXT("%s must carry no structure id, got %d"),
				Miss.Description, Hit.Ref.StructureId),
			Hit.Ref.StructureId, static_cast<int32>(INDEX_NONE));

		TestEqual(
			FString::Printf(TEXT("%s must carry no piece index, got %d"),
				Miss.Description, Hit.Ref.PieceIndex),
			Hit.Ref.PieceIndex, static_cast<int32>(INDEX_NONE));

		const TArray<const FPieceAction*> Menu = PieceActionsFor(*Binding, Hit.Ref);

		TestEqual(
			FString::Printf(TEXT("%s must offer no menu, got [%s]"),
				Miss.Description, *DescribeClickMenu(Menu)),
			Menu.Num(), 0);

		/*
		 * AND NOTHING COMMITS EITHER. "It returned INDEX_NONE" is not the same claim: a
		 * commit path that ignored its ref would satisfy every row above and still delete a
		 * brick, which is precisely the click-the-floor-lose-a-wall bug.
		 */
		TestTrue(
			FString::Printf(TEXT("%s must commit nothing"), Miss.Description),
			!TestWorld.Subsystem->CommitPieceAction(Hit.Ref, *Delete));

		TestEqual(
			FString::Printf(TEXT("%s must leave all %d pieces live, got %d"),
				Miss.Description, ClickWallPieceCount, Binding->GetStructure().NumLivePieces()),
			Binding->GetStructure().NumLivePieces(), ClickWallPieceCount);

		for (int32 Piece = 0; Piece < Bricks.Num(); ++Piece)
		{
			TestTrue(
				FString::Printf(TEXT("%s must leave brick %d standing in the world"),
					Miss.Description, Piece),
				IsValid(Bricks[Piece]));
		}
	}

	/*
	 * THREE: THE MENU FOR A CLICKED, LIVE BRICK OFFERS DELETE.
	 */
	const FPieceBox& WaistBox = Reference.Boxes[ClickWaistPiece];

	const FPieceHit WaistHit = TestWorld.Subsystem->TracePiece(
		FVector(WaistBox.CentreCm.X, WaistBox.CentreCm.Y - ClickTraceReachCm, WaistBox.CentreCm.Z),
		FVector(WaistBox.CentreCm.X, WaistBox.CentreCm.Y + ClickTraceReachCm, WaistBox.CentreCm.Z));

	/*
	 * REPORTED AND THEN CARRIED ON, RATHER THAN BAILED OUT OF. Everything below commits
	 * against the ref this click produced, so a click that resolved to nothing takes the
	 * commit half down with it — which is the point: the claim is the CHAIN, and a run that
	 * stopped here would leave the far end of it silently unexercised and looking green.
	 */
	if (WaistHit.PieceHandle != ClickWaistPiece)
	{
		AddError(FString::Printf(
			TEXT("the click on the waist resolved to %d rather than %d, so every commit assertion below is failing for that reason"),
			WaistHit.PieceHandle, ClickWaistPiece));
	}

	{
		const TArray<const FPieceAction*> Menu = PieceActionsFor(*Binding, WaistHit.Ref);

		TestTrue(
			*FString::Printf(TEXT("the menu for a clicked live brick should offer Delete, got [%s]"),
				*DescribeClickMenu(Menu)),
			Menu.Contains(Delete));
	}

	/*
	 * FOUR: COMMITTING DELETES THAT BRICK, DESTROYS ITS ACTOR AND RE-SOLVES THE WALL.
	 */
	ABrickActor* const WaistBrick = Bricks[ClickWaistPiece];

	TestTrue(
		FString::Printf(TEXT("fixture: the waist brick should be in the world before the commit, it is %s"),
			IsValid(WaistBrick) ? TEXT("valid") : TEXT("already gone")),
		IsValid(WaistBrick));

	TestTrue(
		FString::Printf(
			TEXT("fixture: piece %d should be Supported before the commit, the solver says %s"),
			ClickWaistPiece, ClickSupportName(Binding->GetStructure().GetPieceSupport(ClickWaistPiece))),
		Binding->GetStructure().GetPieceSupport(ClickWaistPiece) == EPieceSupport::Supported);

	for (int32 Piece = 0; Piece < ClickWallPieceCount; ++Piece)
	{
		if (!bClickOrphanedByTheWaist[Piece])
		{
			continue;
		}

		/*
		 * THE POSITIVE CONTROL FOR THE RE-SOLVE. These have to read held up NOW for reading
		 * Falling after the commit to mean the wall was solved again.
		 */
		const EPieceSupport Support = Binding->GetStructure().GetPieceSupport(Piece);

		TestTrue(
			FString::Printf(TEXT("fixture: piece %d should be held up before the waist goes, the solver says %s"),
				Piece, ClickSupportName(Support)),
			Support == EPieceSupport::Grounded || Support == EPieceSupport::Supported);
	}

	TestTrue(
		TEXT("committing Delete against the clicked brick should report that it ran"),
		TestWorld.Subsystem->CommitPieceAction(WaistHit.Ref, *Delete));

	TestTrue(
		FString::Printf(TEXT("the clicked piece must be gone from the graph, IsPieceRemoved reports %d"),
			Binding->IsPieceRemoved(ClickWaistPiece) ? 1 : 0),
		Binding->IsPieceRemoved(ClickWaistPiece));

	TestNull(
		TEXT("the deleted piece must have let go of its actor"),
		Binding->GetActor(ClickWaistPiece));

	/*
	 * THE ORPHAN LEFT THE WORLD, AND THIS IS THE ASSERTION THE WHOLE SLICE EXISTS FOR.
	 * RunPieceAction hands the actor back and destroys nothing; without a caller that
	 * consumes it, a kinematic brick stays standing in the hole it was deleted from — which
	 * is not merely untidy, it is a collider nothing in the model knows about.
	 */
	TestTrue(
		FString::Printf(TEXT("the deleted brick's actor must have been destroyed, it is %s"),
			IsValid(WaistBrick) ? TEXT("still valid") : TEXT("gone")),
		!IsValid(WaistBrick));

	/*
	 * AND ONLY THAT ONE. A commit that tore down the whole wall would satisfy the row above
	 * perfectly well.
	 */
	for (int32 Piece = 0; Piece < Bricks.Num(); ++Piece)
	{
		if (Piece == ClickWaistPiece)
		{
			continue;
		}

		TestTrue(
			FString::Printf(TEXT("brick %d was not deleted and must still be in the world"), Piece),
			IsValid(Bricks[Piece]));
	}

	TestEqual(
		FString::Printf(TEXT("the handle range must not shrink when a piece is deleted, got %d"),
			Binding->NumPieces()),
		Binding->NumPieces(), ClickWallPieceCount);

	TestEqual(
		FString::Printf(TEXT("one piece should have left the structure, so %d should be live, got %d"),
			ClickWallPieceCount - 1, Binding->GetStructure().NumLivePieces()),
		Binding->GetStructure().NumLivePieces(), ClickWallPieceCount - 1);

	/*
	 * THE RE-SOLVE, AND NOTHING BETWEEN THE COMMIT AND HERE CALLED SolveLoads. With the waist
	 * gone, pieces 3 and 4 have only each other's head joint and no path to the ground; a wall
	 * nobody re-solved still reports them held up by a brick that is not there any more.
	 */
	for (int32 Piece = 0; Piece < ClickWallPieceCount; ++Piece)
	{
		if (Piece == ClickWaistPiece)
		{
			continue;
		}

		const EPieceSupport Support = Binding->GetStructure().GetPieceSupport(Piece);

		AddInfo(FString::Printf(TEXT("after the commit, piece %d reads %s"), Piece, ClickSupportName(Support)));

		if (bClickOrphanedByTheWaist[Piece])
		{
			TestTrue(
				FString::Printf(
					TEXT("the commit must re-solve: piece %d lost its only path to the ground and should read Falling, got %s"),
					Piece, ClickSupportName(Support)),
				Support == EPieceSupport::Falling);
		}
		else
		{
			TestTrue(
				FString::Printf(TEXT("grounded piece %d must still read Grounded after the re-solve, got %s"),
					Piece, ClickSupportName(Support)),
				Support == EPieceSupport::Grounded);
		}
	}

	/*
	 * AND CLICKING THE HOLE THE BRICK LEFT FINDS NOTHING. The actor is gone, so the trace
	 * misses; this is the same ref going stale that RunPieceAction's re-resolve exists for,
	 * observed from the far end of the chain.
	 */
	{
		const FPieceHit Again = TestWorld.Subsystem->TracePiece(
			FVector(WaistBox.CentreCm.X, WaistBox.CentreCm.Y - ClickTraceReachCm, WaistBox.CentreCm.Z),
			FVector(WaistBox.CentreCm.X, WaistBox.CentreCm.Y + ClickTraceReachCm, WaistBox.CentreCm.Z));

		TestEqual(
			FString::Printf(TEXT("clicking where the deleted brick was must find no piece, got handle %d"),
				Again.PieceHandle),
			Again.PieceHandle, static_cast<int32>(INDEX_NONE));

		TestEqual(
			FString::Printf(TEXT("the stale click must offer no menu, got [%s]"),
				*DescribeClickMenu(PieceActionsFor(*Binding, Again.Ref))),
			PieceActionsFor(*Binding, Again.Ref).Num(), 0);
	}

	/*
	 * A SECOND COMMIT ON THE SAME REF DID NOTHING AND MUST SAY SO — and must not hand a
	 * second actor to be destroyed, which would be destroying something already destroyed.
	 */
	TestTrue(
		TEXT("committing the same click a second time must report that it did nothing"),
		!TestWorld.Subsystem->CommitPieceAction(WaistHit.Ref, *Delete));

	TestEqual(
		FString::Printf(TEXT("the second commit must leave %d pieces live, got %d"),
			ClickWallPieceCount - 1, Binding->GetStructure().NumLivePieces()),
		Binding->GetStructure().NumLivePieces(), ClickWallPieceCount - 1);

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
