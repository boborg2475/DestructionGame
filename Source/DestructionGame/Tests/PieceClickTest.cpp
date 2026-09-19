// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceActions.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Named namespace, and named differently from every other one in this module — a unity
 * build merges many files into one translation unit. The world harness itself is not
 * redeclared here: it lives in Tests/BrickWorldTestSupport.h and is shared with
 * BrickActorTest.cpp and StructurePushTest.cpp, so a floor height and a tick length are
 * not two fixtures that can drift. Only what is specific to clicking is below.
 */
namespace PieceClickTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * This file uses BrickWorldTestSupport::NarrowWaistWallSpec(3), not ::WallSpec, because
	 * of the waist. The claim this test makes about committing is that the wall was
	 * re-solved behind the deletion, visible only as a piece whose support answer changes
	 * — and in the shared flush 2x3 wall nothing's answer changes when one brick goes (a
	 * full brick spans two below it and keeps the other; a half bat falls back on its head
	 * joint). Both are right physics and both make the re-solve invisible.
	 *
	 *      course 2         [ 3 ][ 4 ]      head joint 3-4 between them
	 *      course 1            [ 2 ]        THE WAIST — clicked and deleted
	 *      course 0         [ 0 ][ 1 ]      grounded
	 *
	 * Everything above course 0 reaches the ground only through piece 2, and deleting it
	 * leaves 3 and 4 with each other's head joint and nothing else, so both read Falling.
	 * Three courses rather than StructurePushTest.cpp's four, since nothing here ticks
	 * physics and a fifth and sixth piece would only cost spawn time.
	 */

	/**
	 * Fixture preconditions, asserted rather than assumed: 2 + 1 + 2 pieces, joints are the
	 * two head joints 0-1 and 3-4 plus the four bed joints 0-2, 1-2, 2-3, 2-4. If a producer
	 * change moves either number, this says so here rather than failing downstream with a
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
	 * How far along Y a trace starts and ends, either side of the wall. A brick is 10.25 cm
	 * deep and the wall is centred on Y = 0, so +/- 100 cm is far outside it on both sides
	 * and the ray crosses the whole thickness. Along Y, not X or Z, so nothing else in the
	 * wall is in the way.
	 */
	constexpr double ClickTraceReachCm = 100.0;

	/**
	 * A point over the floor and clear of the wall. BrickWorldTestSupport's slab is scaled
	 * 40x from a mesh whose local bounds run 0..100, so it spans X 0..4000 and Y 0..4000
	 * with its top at Z = -50 — a known misplacement (SM_Cube's pivot is a corner, so the
	 * slab does not straddle the origin and only the Y >= 0 half of any wall has floor
	 * beneath it). Nothing here needs a brick to land on it, so it is left as is; this
	 * point is simply chosen inside the half that genuinely has floor.
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
 * Clicking a brick resolves to that brick's piece, the menu is built from what the table
 * allows, committing deletes it and re-solves the wall, and the orphaned actor is
 * destroyed.
 *
 * One test because one world: a world test costs tens of milliseconds of setup, and the
 * four claims below share a single wall in a single world — splitting them by assertion
 * would pay for the world four times over to learn nothing extra.
 *
 * The trace is asserted per piece, not once. The defect that matters is a wall whose
 * actors were spawned in the right places but handed the wrong refs; a single trace
 * cannot see that, but five traces through five known box centres can, since the wrong
 * answer has to be wrong for a particular brick. BrickActorTest.cpp already traces its
 * own wall this way, checking the spawner (that a brick's ref agrees with its bounds);
 * this test checks the chain — that the subsystem turns a ray into a handle with every
 * step failing closed — so the two overlap in fixture, not in subject.
 *
 * Fail-closed is asserted on the things a player actually clicks: the floor, and thin
 * air. Both must produce no piece, no menu and no action — the "no action" half is the
 * one a bare "it returned INDEX_NONE" would miss, since a commit path that ignored its
 * ref could still delete something.
 *
 * The commit is where ActorToDestroy is finally consumed. RunPieceAction is world-free
 * and hands the orphan back rather than destroying it, so until something calls it from a
 * world the deleted brick's mesh stays standing with no piece naming it. Three things are
 * asserted separately because each fails on its own: the piece left the graph, the actor
 * left the world, and the wall was re-solved — visible only because the fixture has a
 * waist, and nothing here calls SolveLoads after the commit.
 *
 * Needs a world — actors to spawn into, a physics scene for the trace to query, somewhere
 * to destroy an actor — but deliberately never ticks one; nothing here is about anything
 * falling (that is StructurePushTest.cpp's subject).
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

	const FRunningBondSpec Spec = NarrowWaistWallSpec(3);

	/*
	 * The reference layout is laid separately, so the traced points come from the producer
	 * rather than from whatever the subsystem happened to spawn — a spawner that put every
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

	// One: a trace through brick k resolves to piece k, for every brick in the wall.
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
		 * The ref that comes back is the one a menu and a commit are built from, so it has
		 * to resolve against the binding on its own account, not just agree with the handle.
		 */
		TestEqual(
			FString::Printf(TEXT("the ref the trace at piece %d handed back should resolve to %d, got %d"),
				Piece, Piece, Binding->ResolvePiece(Hit.Ref)),
			Binding->ResolvePiece(Hit.Ref), Piece);
	}

	// Two: a click that missed every brick produces no piece, no menu and no action.
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
		 * And nothing commits either — "it returned INDEX_NONE" is not the same claim: a
		 * commit path that ignored its ref would satisfy every row above and still delete a
		 * brick, precisely the click-the-floor-lose-a-wall bug.
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

	// Three: the menu for a clicked, live brick offers Delete.
	const FPieceBox& WaistBox = Reference.Boxes[ClickWaistPiece];

	const FPieceHit WaistHit = TestWorld.Subsystem->TracePiece(
		FVector(WaistBox.CentreCm.X, WaistBox.CentreCm.Y - ClickTraceReachCm, WaistBox.CentreCm.Z),
		FVector(WaistBox.CentreCm.X, WaistBox.CentreCm.Y + ClickTraceReachCm, WaistBox.CentreCm.Z));

	/*
	 * Reported and then carried on, rather than bailed out of: everything below commits
	 * against the ref this click produced, so a run that stopped here would leave the far
	 * end of the chain silently unexercised and looking green.
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

	// Four: committing deletes that brick, destroys its actor and re-solves the wall.
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

		/* The positive control for the re-solve: these have to read held up now for reading
		 * Falling after the commit to mean the wall was solved again. */
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
	 * The orphan left the world. RunPieceAction hands the actor back and destroys nothing;
	 * without a caller that consumes it, a kinematic brick stays standing in the hole it
	 * was deleted from — a collider nothing in the model knows about.
	 */
	TestTrue(
		FString::Printf(TEXT("the deleted brick's actor must have been destroyed, it is %s"),
			IsValid(WaistBrick) ? TEXT("still valid") : TEXT("gone")),
		!IsValid(WaistBrick));

	// And only that one — a commit that tore down the whole wall would satisfy the row above.
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
	 * The re-solve, and nothing between the commit and here called SolveLoads. With the
	 * waist gone, pieces 3 and 4 have only each other's head joint and no path to the
	 * ground; a wall nobody re-solved would still report them held up by a brick that is
	 * no longer there.
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
	 * And clicking the hole the brick left finds nothing: the actor is gone, so the trace
	 * misses — the same ref-going-stale that RunPieceAction's re-resolve exists for.
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

	/* A second commit on the same ref did nothing and must say so — and must not hand a
	 * second actor to be destroyed, which would be destroying something already destroyed. */
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
