// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceActions.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Uniquely named namespace (unity builds merge files). The world harness is shared in
 * Tests/BrickWorldTestSupport.h; only click-specific support is here.
 */
namespace PieceClickTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * Uses NarrowWaistWallSpec(3), not WallSpec: the re-solve after a delete is only visible if
	 * some piece's support changes, and in the flush wall none does.
	 *
	 *      course 2         [ 3 ][ 4 ]      head joint 3-4 between them
	 *      course 1            [ 2 ]        the waist, clicked and deleted
	 *      course 0         [ 0 ][ 1 ]      grounded
	 *
	 * Deleting piece 2 leaves 3 and 4 with only their shared head joint, so both read Falling.
	 */

	/** Fixture preconditions: 5 pieces; head joints 0-1, 3-4 and bed joints 0-2, 1-2, 2-3, 2-4. */
	constexpr int32 ClickWallPieceCount = 5;
	constexpr int32 ClickWallJointCount = 6;

	/** The single course-1 brick everything above the bottom course hangs from. */
	constexpr int32 ClickWaistPiece = 2;

	/** Exactly the pieces that lose their path to the ground when the waist goes. */
	constexpr bool bClickOrphanedByTheWaist[ClickWallPieceCount] =
	{
		false, false, false, true, true
	};

	/** Trace half-length along Y, well outside the 10.25 cm wall. Y so no other brick is in the way. */
	constexpr double ClickTraceReachCm = 100.0;

	/**
	 * A point over the floor, clear of the wall. The harness slab spans X and Y 0..4000 (SM_Cube's
	 * pivot is a corner, a known misplacement), so the point is chosen inside it.
	 */
	const FVector ClickFloorPointCm(100.0, 100.0, 0.0);

	/** Empty air, far from the wall and above the floor. */
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

	/** A menu's labels, for failure messages. */
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

	/** An action row looked up by label, not table position. */
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
 * Clicking a brick resolves to its piece, the menu offers Delete, and committing removes the
 * piece, destroys its actor and re-solves the wall. One test to share one world.
 *
 * Every brick is traced, since wrong refs on correctly placed actors only show per brick.
 * Clicks on the floor and empty air must give no piece, no menu and no commit (a commit that
 * ignored its ref could still delete). The commit's three effects are asserted separately: piece
 * removed, actor destroyed (RunPieceAction only hands it back), and wall re-solved, with no
 * SolveLoads call after the commit. Needs a world for actors and traces; never ticks.
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

	// Trace points come from a separately laid layout, so a wrong spawner cannot agree with itself.
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

		// The ref must resolve on its own, since menus and commits are built from it.
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

		// Nothing commits: a commit path that ignored its ref would pass the checks above.
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

	// Report and continue rather than return, so the rest of the chain still runs.
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

		// Positive control: held up now, so Falling later proves a re-solve.
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

	// RunPieceAction only hands the actor back; the commit must destroy it.
	TestTrue(
		FString::Printf(TEXT("the deleted brick's actor must have been destroyed, it is %s"),
			IsValid(WaistBrick) ? TEXT("still valid") : TEXT("gone")),
		!IsValid(WaistBrick));

	// And only that actor.
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

	// The commit's own re-solve: pieces 3 and 4 have no path to the ground and must read Falling.
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

	// Clicking the hole finds nothing.
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

	// A second commit on the same ref does nothing.
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
