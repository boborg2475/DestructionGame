// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/PieceActions.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Components/StaticMeshComponent.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, not anonymous, and named differently from every other one in this
 * directory. An anonymous namespace is private to a TRANSLATION UNIT rather than to a
 * file, and a unity build merges many files into one — at which point two anonymous
 * BedJointNormal declarations are the same declaration twice. See CURRENT_STATE.md; the
 * `using namespace` for this one lives inside each RunTest body for the same reason.
 */
namespace PieceActionsTestSupport
{
	using namespace DestructionProfiles;

	/** A standard UK metric brick, 215 x 102.5 x 65 mm, in cm3. */
	constexpr double PieceActionBrickVolumeCubicCm = 21.5 * 10.25 * 6.5;

	/**
	 * A standard clay brick, derived rather than hand-set.
	 *
	 * NO EXPECTATION IN THIS FILE DEPENDS ON THE VALUE. Every joint below is the
	 * Unbreakable test fixture and every assertion is about a handle, an actor, a
	 * removal flag or a support state — never about a force or a utilisation. A real
	 * number is simply less distracting than zero, and deriving it keeps this file from
	 * becoming a second place for the figure to drift.
	 */
	const double PieceActionBrickMassKg =
		ClayBrick.DensityGramsPerCubicCm * PieceActionBrickVolumeCubicCm / 1000.0;

	/** Horizontal interface, normal pointing up at the piece above: a bed joint. */
	const FVector PieceActionBedJointNormal(0.0, 0.0, 1.0);

	constexpr double PieceActionJointAreaSqCm = 100.0;

	/** The structure the fixtures below identify themselves as. */
	constexpr int32 ThisStructure = 7;

	/** A different one, for a ref that names somebody else's brick. */
	constexpr int32 SomeOtherStructure = 8;

	/**
	 * A stand-in for a brick actor.
	 *
	 * NOT `NewObject<UObject>`, WHICH DOES NOT WORK: UObject itself is flagged abstract,
	 * so instantiating it directly trips an engine ensure and the automation framework
	 * counts that ensure as a test error. UStaticMeshComponent is the nearest concrete
	 * thing a brick will genuinely own and needs no world — and this whole slice is
	 * world-free, so a test for it that spun up a world would be testing the next slice
	 * by accident.
	 */
	UObject* MakePieceActionStandIn()
	{
		UObject* StandIn = NewObject<UStaticMeshComponent>(GetTransientPackage());
		StandIn->AddToRoot();
		return StandIn;
	}

	void ReleasePieceActionStandIns(const TArray<UObject*>& StandIns)
	{
		for (UObject* StandIn : StandIns)
		{
			if (StandIn != nullptr)
			{
				StandIn->RemoveFromRoot();
			}
		}
	}

	/** A distinct, recognisable box per handle, so a shifted array reads back wrong. */
	DestructionLayout::FPieceBox PieceActionBoxFor(int32 Index)
	{
		DestructionLayout::FPieceBox Box;
		Box.CentreCm = FVector(Index * 100.0 + 1.0, Index * 10.0 + 2.0, Index + 3.0);
		Box.ExtentCm = FVector(10.75, 5.125, 3.25);
		return Box;
	}

	struct FPieceActionPieceSpec
	{
		double MassKg = 0.0;
		bool bIsGrounded = false;
	};

	struct FPieceActionJointSpec
	{
		int32 PieceA = INDEX_NONE;
		int32 PieceB = INDEX_NONE;
		FVector Normal = FVector::ZAxisVector;
	};

	void BuildPieceActionBinding(
		FStructureBinding& Out,
		const TArray<FPieceActionPieceSpec>& PieceSpecs,
		const TArray<FPieceActionJointSpec>& JointSpecs,
		TArray<UObject*>& OutStandIns)
	{
		Out.StructureId = ThisStructure;

		for (int32 Index = 0; Index < PieceSpecs.Num(); ++Index)
		{
			UObject* StandIn = MakePieceActionStandIn();
			OutStandIns.Add(StandIn);
			Out.AddPiece(
				PieceSpecs[Index].MassKg,
				PieceSpecs[Index].bIsGrounded,
				StandIn,
				PieceActionBoxFor(Index));
		}

		for (const FPieceActionJointSpec& Joint : JointSpecs)
		{
			FConnection Connection;
			Connection.PieceA = Joint.PieceA;
			Connection.PieceB = Joint.PieceB;
			Connection.InterfaceNormal = Joint.Normal;
			Connection.InterfaceAreaSqCm = PieceActionJointAreaSqCm;
			Connection.Strength = Unbreakable;
			Out.AddConnection(Connection);
		}
	}

	const TCHAR* NameOfPieceSupport(EPieceSupport State)
	{
		switch (State)
		{
		case EPieceSupport::Falling:   return TEXT("Falling");
		case EPieceSupport::Grounded:  return TEXT("Grounded");
		case EPieceSupport::Supported: return TEXT("Supported");
		case EPieceSupport::Stranded:  return TEXT("Stranded");
		}

		return TEXT("<not a state>");
	}

	/** Look a row up by label, so no test hard-codes a position in the table. */
	const FPieceAction* FindPieceAction(const TCHAR* Label)
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
	 * Bail out loudly rather than dereferencing a null row.
	 *
	 * A crash inside a test does not fail that test — it aborts the whole automation run,
	 * and every test queued after it silently never reports at all. See CURRENT_STATE.md.
	 */
	bool RequireAction(FAutomationTestBase& Test, const FPieceAction* Action, const TCHAR* Label)
	{
		if (Action == nullptr)
		{
			Test.AddError(FString::Printf(
				TEXT("the action table must contain a row labelled '%s'; it does not (table has %d rows)"),
				Label, AllPieceActions().Num()));
			return false;
		}

		if (Action->CanRun == nullptr || Action->Run == nullptr)
		{
			Test.AddError(FString::Printf(
				TEXT("the '%s' row must carry both a CanRun and a Run; CanRun is %s and Run is %s"),
				Label,
				Action->CanRun == nullptr ? TEXT("NULL") : TEXT("set"),
				Action->Run == nullptr ? TEXT("NULL") : TEXT("set")));
			return false;
		}

		return true;
	}

	/**
	 * A TEST-SIDE ACTION THAT RECORDS BEING RUN, and nothing else.
	 *
	 * This is what makes "the commit path re-resolves the ref BEFORE it runs anything" a
	 * mechanism assertion rather than an inference. Against the Delete row a rejected ref
	 * shows up only as an absence — nothing was removed — which a commit path that ran the
	 * action and then reported false would also produce. The counter cannot be fooled that
	 * way: it says whether Run was entered at all.
	 */
	int32 TripwireRunCount = 0;

	bool TripwireCanRun(const FStructureBinding&, int32)
	{
		return true;
	}

	bool TripwireRun(FPieceActionContext&)
	{
		++TripwireRunCount;
		return true;
	}

	const FPieceAction TripwireAction{ TEXT("<tripwire>"), &TripwireCanRun, &TripwireRun };

	/**
	 * THE SAME TRIPWIRE WITH ITS CanRun SAYING NO, and it is the only thing that can see
	 * whether the commit path consults CanRun at all.
	 *
	 * Against the shipped Delete row the two guards overlap almost everywhere: a ref that
	 * ResolvePiece refuses is usually a piece CanRun would refuse too, so a commit path that
	 * consults neither, one, or both looks identical. This row separates them — the ref is
	 * live and resolves perfectly, and the ONLY reason to refuse is CanRun. The counter is
	 * what says the refusal happened before Run rather than after it.
	 */
	int32 RefusedTripwireRunCount = 0;

	bool RefusedTripwireCanRun(const FStructureBinding&, int32)
	{
		return false;
	}

	bool RefusedTripwireRun(FPieceActionContext&)
	{
		++RefusedTripwireRunCount;
		return true;
	}

	const FPieceAction RefusedTripwireAction{
		TEXT("<tripwire that cannot run>"), &RefusedTripwireCanRun, &RefusedTripwireRun };

	/** Whether a menu returned a pointer to a particular row of the shipped table. */
	bool MenuOffers(const TArray<const FPieceAction*>& Menu, const FPieceAction* Action)
	{
		return Menu.Contains(Action);
	}

	/** The labels a menu came back with, so a failure reads without a debugger. */
	FString DescribeMenu(const TArray<const FPieceAction*>& Menu)
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
}

/**
 * EVERY ROW OF THE ACTION TABLE IS WELL-FORMED, whichever rows there turn out to be.
 *
 * A sweep, modelled on Profiles.ConnectionInvariants, and for the same reason: the whole
 * requirement here is that adding the second and the tenth action is adding a ROW. A test
 * that named each action would have to be edited alongside every one of them, which is the
 * switch statement the design is trying not to have, relocated into the test suite.
 *
 * DELIBERATELY NO EXACT COUNT. A second action should be a row, not a test edit. What is
 * asserted is that the table is not EMPTY — a sweep over nothing checks nothing, and a
 * context menu with no entries is not a menu — and that every row carries a usable label
 * and both function pointers, with labels unique so a menu cannot show the same word twice
 * and a lookup cannot silently pick whichever came first.
 *
 * GREEN-ON-ARRIVAL WARNING: this goes green the moment one well-formed row exists, so it
 * is not evidence of anything on its own. Prove it bites by temporarily adding a row with
 * a null Run — that is a test-side, free mutation.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceActionsTableIsWellFormedTest,
	"DestructionGame.Core.PieceActions.TableIsWellFormed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceActionsTableIsWellFormedTest::RunTest(const FString& Parameters)
{
	using namespace PieceActionsTestSupport;

	const TArrayView<const FPieceAction> Actions = AllPieceActions();

	TestTrue(
		FString::Printf(
			TEXT("the action table must not be empty — a sweep over nothing checks nothing, and a menu with no entries is not a menu; got %d rows"),
			Actions.Num()),
		Actions.Num() > 0);

	for (int32 Index = 0; Index < Actions.Num(); ++Index)
	{
		const FPieceAction& Action = Actions[Index];

		const bool bHasLabel = Action.Label != nullptr;

		TestTrue(
			FString::Printf(TEXT("row %d must carry a label, got null"), Index),
			bHasLabel);

		if (bHasLabel)
		{
			TestTrue(
				FString::Printf(TEXT("row %d's label must not be empty"), Index),
				FCString::Strlen(Action.Label) > 0);
		}

		const FString Where = FString::Printf(
			TEXT("row %d (%s)"), Index, bHasLabel ? Action.Label : TEXT("<null label>"));

		/*
		 * BOTH POINTERS, ALWAYS. CanRun is what lets the menu filter itself without any
		 * branch naming a specific action, so a row without one has no answer to give and
		 * the menu has to special-case it — which is the switch again.
		 */
		TestTrue(
			*FString::Printf(TEXT("%s must carry a CanRun, got null"), *Where),
			Action.CanRun != nullptr);

		TestTrue(
			*FString::Printf(TEXT("%s must carry a Run, got null"), *Where),
			Action.Run != nullptr);

		/*
		 * LABELS ARE UNIQUE. Two rows sharing a word show the player the same entry twice
		 * and make a lookup by label answer whichever happens to come first — silently, and
		 * differently after a reorder.
		 */
		for (int32 Earlier = 0; Earlier < Index; ++Earlier)
		{
			const FPieceAction& Other = Actions[Earlier];

			if (!bHasLabel || Other.Label == nullptr)
			{
				continue;
			}

			TestTrue(
				*FString::Printf(TEXT("%s duplicates the label of row %d — labels must be unique"),
					*Where, Earlier),
				FCString::Strcmp(Action.Label, Other.Label) != 0);
		}
	}

	return true;
}

/**
 * DELETE TAKES THE BRICK OUT OF THE STRUCTURE, HANDS BACK ITS ORPHANED ACTOR, AND LEAVES
 * THE WALL RE-SOLVED.
 *
 * FOUR THINGS ARE PINNED HERE AND EACH HAS ITS OWN FAILURE MODE.
 *
 * THE ACTOR IS CAPTURED BEFORE THE ACTION RUNS. GetActor answers null for a removed piece,
 * so a commit path that looks for the actor AFTERWARDS hands back nothing and the brick
 * mesh stays in the world forever with no piece naming it. The assertion is pointer
 * identity against the stand-in, taken at a point where GetActor itself is asserted null.
 *
 * THE HANDLE RANGE DOES NOT SHRINK. FStructure tombstones the slot, deliberately, because
 * handles are a model-level promise: renumbering would silently re-point every joint above
 * the hole and there is no recovering the break stamps afterwards. So NumPieces is
 * unchanged and NumLivePieces is one lower, which is exactly what distinguishes a
 * tombstone from a compaction.
 *
 * THE COMMIT PATH RE-SOLVES, AND NO ROW HAD TO REMEMBER TO. The piece above the deleted one
 * loses its only support, and this test never calls SolveLoads after the commit — so if the
 * re-solve is missing, the structure still reports the brick Supported by something that is
 * no longer there. That is the "the brick vanishes and the wall stands there" failure, and
 * it is invisible to every other assertion in this file.
 *
 * A SECOND DELETE DID NOTHING, AND SAYS SO. The ref is stale the instant the first one
 * commits; a silent no-op reporting success is the case this project keeps finding.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceActionsDeleteTest,
	"DestructionGame.Core.PieceActions.Delete",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceActionsDeleteTest::RunTest(const FString& Parameters)
{
	using namespace PieceActionsTestSupport;

	const FPieceAction* Delete = FindPieceAction(TEXT("Delete"));

	if (!RequireAction(*this, Delete, TEXT("Delete")))
	{
		return true;
	}

	/*
	 *      [ roof 2 ]
	 *          |          bed joint
	 *    [ column 1 ]     <- DELETED, and it is a MIDDLE handle on purpose:
	 *          |             deleting the last one is indistinguishable from
	 *    [  pad 0  ]         compacting it away.
	 *    ==========      [ bystander 3 ]
	 *                    ===============
	 */
	const TArray<FPieceActionPieceSpec> PieceSpecs = {
		{ PieceActionBrickMassKg, true },  // 0: pad, on the earth
		{ PieceActionBrickMassKg, false }, // 1: column, DELETED
		{ PieceActionBrickMassKg, false }, // 2: roof, resting on the column and nothing else
		{ PieceActionBrickMassKg, true }   // 3: an unrelated grounded pad, which must be untouched
	};

	const TArray<FPieceActionJointSpec> JointSpecs = {
		{ 0, 1, PieceActionBedJointNormal },
		{ 1, 2, PieceActionBedJointNormal }
	};

	FStructureBinding Binding;
	TArray<UObject*> StandIns;
	BuildPieceActionBinding(Binding, PieceSpecs, JointSpecs, StandIns);

	Binding.SolveLoads();

	/*
	 * THE FIXTURE'S PRECONDITIONS, and the roof one is the positive control for the
	 * re-solve assertion below: it has to read Supported NOW for reading Falling later to
	 * mean anything.
	 */
	TestTrue(
		FString::Printf(TEXT("fixture: the column should solve as Supported, got %s"),
			NameOfPieceSupport(Binding.GetStructure().GetPieceSupport(1))),
		Binding.GetStructure().GetPieceSupport(1) == EPieceSupport::Supported);

	TestTrue(
		FString::Printf(TEXT("fixture: the roof should solve as Supported before the column goes, got %s"),
			NameOfPieceSupport(Binding.GetStructure().GetPieceSupport(2))),
		Binding.GetStructure().GetPieceSupport(2) == EPieceSupport::Supported);

	TestTrue(
		FString::Printf(TEXT("fixture: handle 1 should be bound to its own stand-in, got %s"),
			*GetNameSafe(Binding.GetActor(1))),
		Binding.GetActor(1) == StandIns[1]);

	/* A live, unreleased brick is exactly what the menu should be offering Delete on. */
	TestTrue(
		TEXT("Delete should be offered on a live, unreleased brick"),
		Delete->CanRun(Binding, 1));

	const FPieceRef Ref{ ThisStructure, 1 };

	const FPieceActionResult Result = RunPieceAction(Binding, Ref, *Delete);

	TestTrue(
		FString::Printf(TEXT("deleting a live brick should report that it did something, bRan reports %d"),
			Result.bRan ? 1 : 0),
		Result.bRan);

	/*
	 * THE ORPHAN COMES BACK. Captured before the action ran, because a moment later there
	 * is nothing left to capture — the very next assertion is that GetActor answers null.
	 */
	TestTrue(
		FString::Printf(
			TEXT("the commit path should hand back the deleted brick's own actor (%s) so the caller can destroy it, got %s"),
			*GetNameSafe(StandIns[1]), *GetNameSafe(Result.ActorToDestroy)),
		Result.ActorToDestroy == StandIns[1]);

	TestTrue(
		FString::Printf(TEXT("the deleted piece must be gone from the graph, IsPieceRemoved reports %d"),
			Binding.IsPieceRemoved(1) ? 1 : 0),
		Binding.IsPieceRemoved(1));

	TestTrue(
		FString::Printf(TEXT("the deleted piece must have let go of its actor, got %s"),
			*GetNameSafe(Binding.GetActor(1))),
		Binding.GetActor(1) == nullptr);

	/*
	 * THE TOMBSTONE IS THE WHOLE POINT. NumPieces is the handle RANGE and never a live
	 * count; a shrinking answer means the slot was compacted away, at which point every
	 * handle above the hole names a different brick and nothing says so.
	 */
	TestTrue(
		FString::Printf(TEXT("the handle range must not shrink when a piece is deleted, got %d of an expected %d"),
			Binding.NumPieces(), PieceSpecs.Num()),
		Binding.NumPieces() == PieceSpecs.Num());

	TestTrue(
		FString::Printf(TEXT("the binding spans %d handles and the structure it owns spans %d — the arrays have desynced"),
			Binding.NumPieces(), Binding.GetStructure().NumPieces()),
		Binding.NumPieces() == Binding.GetStructure().NumPieces());

	TestTrue(
		FString::Printf(TEXT("one piece should have left the structure, so 3 should be live, got %d"),
			Binding.GetStructure().NumLivePieces()),
		Binding.GetStructure().NumLivePieces() == 3);

	/* Every survivor is untouched, and pointer identity is what makes that checkable. */
	for (const int32 Survivor : { 0, 2, 3 })
	{
		TestTrue(
			FString::Printf(TEXT("piece %d must still be in the graph after piece 1 was deleted"), Survivor),
			!Binding.IsPieceRemoved(Survivor));

		TestTrue(
			FString::Printf(TEXT("piece %d must still resolve to its own stand-in, got %s"),
				Survivor, *GetNameSafe(Binding.GetActor(Survivor))),
			Binding.GetActor(Survivor) == StandIns[Survivor]);
	}

	/*
	 * THE ASSERTION THE COMMIT PATH EXISTS FOR, and note that NOTHING between the
	 * RunPieceAction above and this line called SolveLoads. The roof's only support went
	 * with the column, so a re-solved structure says Falling; a structure nobody re-solved
	 * still says Supported, held up by a brick that is not there any more.
	 */
	TestTrue(
		FString::Printf(
			TEXT("the commit path must re-solve: with its only support deleted the roof should read Falling, got %s"),
			NameOfPieceSupport(Binding.GetStructure().GetPieceSupport(2))),
		Binding.GetStructure().GetPieceSupport(2) == EPieceSupport::Falling);

	TestTrue(
		FString::Printf(TEXT("the grounded pad must still read Grounded after the re-solve, got %s"),
			NameOfPieceSupport(Binding.GetStructure().GetPieceSupport(0))),
		Binding.GetStructure().GetPieceSupport(0) == EPieceSupport::Grounded);

	/*
	 * A SECOND COMMIT ON THE SAME REF DID NOTHING AND MUST SAY SO. The ref went stale the
	 * instant the first one committed, and handing back an actor a second time would have
	 * the caller destroy something already destroyed.
	 */
	const FPieceActionResult Again = RunPieceAction(Binding, Ref, *Delete);

	TestTrue(
		FString::Printf(TEXT("deleting an already-deleted piece must report that it did nothing, bRan reports %d"),
			Again.bRan ? 1 : 0),
		!Again.bRan);

	TestTrue(
		FString::Printf(TEXT("a commit that did nothing must hand back no actor, got %s"),
			*GetNameSafe(Again.ActorToDestroy)),
		Again.ActorToDestroy == nullptr);

	TestTrue(
		FString::Printf(TEXT("the second commit must not change the handle range, got %d"), Binding.NumPieces()),
		Binding.NumPieces() == PieceSpecs.Num());

	TestTrue(
		FString::Printf(TEXT("the second commit must leave 3 pieces live, got %d"),
			Binding.GetStructure().NumLivePieces()),
		Binding.GetStructure().NumLivePieces() == 3);

	ReleasePieceActionStandIns(StandIns);

	return true;
}

/**
 * CanRun IS WHAT FILTERS THE MENU, and it says no to a brick that is not there and to one
 * that has already fallen.
 *
 * WHY THE FILTER IS A COLUMN ON THE ROW rather than a check in the menu: the menu must be
 * able to decide what to show without any branch naming a specific action, or every new
 * action is an edit to the menu as well as a row in the table.
 *
 * A MATRIX, BECAUSE THE ROWS CLOSE DIFFERENT HOLES. Removed and released are separate
 * states with separate records — a tombstone on the graph and a one-way latch on the
 * binding — and a filter written against one of them passes this test while leaving the
 * other wide open. The fixture asserts the released brick is NOT removed, precisely so its
 * row cannot be satisfied by the removal check.
 *
 * THE ACCEPTING ROWS COME FIRST, and without them every rejection here is satisfied by a
 * CanRun that returns false unconditionally — which would hide Delete from the menu
 * forever and pass.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceActionsCanRunFiltersTheMenuTest,
	"DestructionGame.Core.PieceActions.CanRunFiltersTheMenu",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceActionsCanRunFiltersTheMenuTest::RunTest(const FString& Parameters)
{
	using namespace PieceActionsTestSupport;

	const FPieceAction* Delete = FindPieceAction(TEXT("Delete"));

	if (!RequireAction(*this, Delete, TEXT("Delete")))
	{
		return true;
	}

	/*
	 *   [ 2 ] floater, connected to nothing: Falling, so a push RELEASES it
	 *
	 *   [ 1 ]     [ 3 ]        both resting on the pad
	 *      \       /
	 *      [  pad 0  ]
	 *      ==========
	 *
	 * Piece 1 is then REMOVED, so the fixture holds a removed piece, a released piece and
	 * two ordinary live ones at once.
	 */
	const TArray<FPieceActionPieceSpec> PieceSpecs = {
		{ PieceActionBrickMassKg, true },  // 0: pad
		{ PieceActionBrickMassKg, false }, // 1: brick on the pad, REMOVED below
		{ PieceActionBrickMassKg, false }, // 2: floater, RELEASED below
		{ PieceActionBrickMassKg, false }  // 3: brick on the pad, live throughout
	};

	const TArray<FPieceActionJointSpec> JointSpecs = {
		{ 0, 1, PieceActionBedJointNormal },
		{ 0, 3, PieceActionBedJointNormal }
	};

	FStructureBinding Binding;
	TArray<UObject*> StandIns;
	BuildPieceActionBinding(Binding, PieceSpecs, JointSpecs, StandIns);

	Binding.SolveLoads();

	TestTrue(
		FString::Printf(TEXT("fixture: the floater should solve as Falling, got %s"),
			NameOfPieceSupport(Binding.GetStructure().GetPieceSupport(2))),
		Binding.GetStructure().GetPieceSupport(2) == EPieceSupport::Falling);

	const int32 ReleasedCount = Binding.ApplyResults();

	TestTrue(
		FString::Printf(TEXT("fixture: the push should release exactly the floater, got %d"), ReleasedCount),
		ReleasedCount == 1);

	TestTrue(TEXT("fixture: the floater should now read released"), Binding.IsReleased(2));

	/*
	 * AND IT IS NOT REMOVED, which is what makes its row discriminating: a CanRun that
	 * only consulted IsPieceRemoved would happily offer Delete on a brick already tumbling
	 * through the air.
	 */
	TestTrue(TEXT("fixture: a released piece is still in the graph, so this row is not the removal row"),
		!Binding.IsPieceRemoved(2));

	TestTrue(TEXT("fixture: the brick on the pad should remove cleanly"), Binding.RemovePiece(1));

	struct FCanRunCase
	{
		const TCHAR* Description;
		int32 Handle;
		bool bExpected;
	};

	const TArray<FCanRunCase> Cases = {
		/* The accepting rows, without which every rejection below is free. */
		{ TEXT("the grounded pad is a live, unreleased brick"), 0, true },
		{ TEXT("the second brick on the pad is a live, unreleased brick"), 3, true },

		{ TEXT("a REMOVED piece is not there to delete"), 1, false },
		{ TEXT("a RELEASED piece has already been handed to physics"), 2, false },

		/*
		 * Unknown handles fail closed too. FStructure::IsPieceRemoved already answers true
		 * for an out-of-range handle, so a filter spelled the obvious way gets these for
		 * free — but free is not the same as asserted, and the menu must not offer an
		 * action on a handle that names nothing.
		 */
		{ TEXT("INDEX_NONE names no piece"), INDEX_NONE, false },
		{ TEXT("a negative handle names no piece"), -7, false },
		{ TEXT("one past the last piece names no piece"), 4, false },
		{ TEXT("a wildly out-of-range handle names no piece"), MAX_int32, false },
		{ TEXT("MIN_int32 names no piece, and must not be negated into range"), MIN_int32, false },
	};

	for (const FCanRunCase& Case : Cases)
	{
		const bool bCanRun = Delete->CanRun(Binding, Case.Handle);

		TestTrue(
			FString::Printf(TEXT("%s: CanRun(%d) should be %s, got %s"),
				Case.Description, Case.Handle,
				Case.bExpected ? TEXT("true") : TEXT("false"),
				bCanRun ? TEXT("true") : TEXT("false")),
			bCanRun == Case.bExpected);
	}

	ReleasePieceActionStandIns(StandIns);

	return true;
}

/**
 * THE MENU FOR A CLICKED BRICK IS THE ACTION TABLE FILTERED BY CanRun, AND A CLICK THAT
 * RESOLVED TO NOTHING GETS NO MENU AT ALL.
 *
 * THE INPUT IS A REF, NOT A HANDLE, AND THAT IS THE REQUIREMENT RATHER THAN A CONVENIENCE.
 * What a click produces is the {StructureId, PieceIndex} the brick actor carries; a handle
 * is what resolving that ref against THIS binding answers. A menu built from a ref that
 * names another structure, or a piece that has since been removed, would be a menu offering
 * to delete somebody else's brick — so an unresolvable ref must produce an EMPTY menu, and
 * "the trace hit the floor" arrives here as a default ref and gets exactly that.
 *
 * THE PROPERTY IS ASSERTED AS A SET, NOT AS A LIST OF LABELS. What the menu offers is
 * "every row whose CanRun says yes, and no others" — expressed over AllPieceActions() so a
 * second action is a row and not an edit to this test. One explicit row names Delete, so the
 * sweep cannot be satisfied by an empty table agreeing with an empty menu.
 *
 * AND THE POINTERS NAME ROWS OF THE SHIPPED TABLE. A menu of copies would compare unequal to
 * everything, and the caller's next step is to pass one of these straight to RunPieceAction —
 * so identity, not equality, is what has to hold. Asserted as pointer identity against
 * AllPieceActions(), which a copied row cannot satisfy.
 *
 * NEEDS A TICKING WORLD: no. This is the whole reason the menu question is asked of a
 * binding rather than of a world — it is arithmetic on a graph, and the world-needing half
 * of a click is only the trace.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceActionsMenuOffersWhatCanRunTest,
	"DestructionGame.Core.PieceActions.MenuOffersWhatCanRun",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceActionsMenuOffersWhatCanRunTest::RunTest(const FString& Parameters)
{
	using namespace PieceActionsTestSupport;

	const FPieceAction* Delete = FindPieceAction(TEXT("Delete"));

	if (!RequireAction(*this, Delete, TEXT("Delete")))
	{
		return true;
	}

	/*
	 *   [ 2 ] floater, connected to nothing: Falling, so a push RELEASES it
	 *
	 *   [ 1 ]     [ 3 ]        both resting on the pad
	 *      \       /
	 *      [  pad 0  ]
	 *      ==========
	 *
	 * Piece 1 is then REMOVED, so one binding holds a removed piece, a released piece and two
	 * ordinary live ones at once — the same shape CanRunFiltersTheMenu uses, because the menu
	 * has to answer for all four states and this is the smallest fixture that has them.
	 */
	const TArray<FPieceActionPieceSpec> PieceSpecs = {
		{ PieceActionBrickMassKg, true },  // 0: pad
		{ PieceActionBrickMassKg, false }, // 1: brick on the pad, REMOVED below
		{ PieceActionBrickMassKg, false }, // 2: floater, RELEASED below
		{ PieceActionBrickMassKg, false }  // 3: brick on the pad, live throughout
	};

	const TArray<FPieceActionJointSpec> JointSpecs = {
		{ 0, 1, PieceActionBedJointNormal },
		{ 0, 3, PieceActionBedJointNormal }
	};

	FStructureBinding Binding;
	TArray<UObject*> StandIns;
	BuildPieceActionBinding(Binding, PieceSpecs, JointSpecs, StandIns);

	Binding.SolveLoads();

	TestTrue(
		FString::Printf(TEXT("fixture: the floater should solve as Falling, got %s"),
			NameOfPieceSupport(Binding.GetStructure().GetPieceSupport(2))),
		Binding.GetStructure().GetPieceSupport(2) == EPieceSupport::Falling);

	/*
	 * THE PUSH IS ITS OWN STATEMENT, and that is not a style point: folding it into the
	 * Printf of the assertion it sets up makes the two argument evaluations UNSEQUENCED, and
	 * MSVC evaluates the condition first — so the fixture read the flag before the call that
	 * sets it and failed for a reason that had nothing to do with the code under test. (It
	 * really did: this was written that way first.)
	 */
	const int32 ReleasedCount = Binding.ApplyResults();

	TestTrue(
		FString::Printf(TEXT("fixture: the push should release exactly the floater, got %d released"),
			ReleasedCount),
		ReleasedCount == 1 && Binding.IsReleased(2));

	TestTrue(TEXT("fixture: a released piece is still in the graph, so its row is not the removal row"),
		!Binding.IsPieceRemoved(2));

	TestTrue(TEXT("fixture: the brick on the pad should remove cleanly"), Binding.RemovePiece(1));

	struct FMenuCase
	{
		const TCHAR* Description;
		FPieceRef Ref;
		bool bExpectDelete;
	};

	const TArray<FMenuCase> Cases = {
		/* The offering rows, without which an implementation returning nothing passes. */
		{ TEXT("a live grounded brick"), { ThisStructure, 0 }, true },
		{ TEXT("a live brick resting on the pad"), { ThisStructure, 3 }, true },

		{ TEXT("a REMOVED piece is not there to act on"), { ThisStructure, 1 }, false },
		{ TEXT("a RELEASED piece is already tumbling"), { ThisStructure, 2 }, false },

		/*
		 * AND EVERY WAY A CLICK CAN RESOLVE TO NOTHING. A default ref is what a trace that
		 * hit the floor, or hit nothing at all, produces — so it is the fail-closed case the
		 * click chain actually depends on rather than a synthetic one.
		 */
		{ TEXT("a click that hit the floor, or nothing: a wholly default ref"), { }, false },
		{ TEXT("a brick belonging to a DIFFERENT structure"), { SomeOtherStructure, 0 }, false },
		{ TEXT("a ref with no structure id"), { INDEX_NONE, 0 }, false },
		{ TEXT("a ref with no piece index"), { ThisStructure, INDEX_NONE }, false },
		{ TEXT("a negative index"), { ThisStructure, -1 }, false },
		{ TEXT("one past the end"), { ThisStructure, 4 }, false },
		{ TEXT("a wildly out-of-range index"), { ThisStructure, MAX_int32 }, false },
		{ TEXT("MIN_int32, which must not be negated into range"), { ThisStructure, MIN_int32 }, false },
	};

	const TArrayView<const FPieceAction> Table = AllPieceActions();

	for (const FMenuCase& Case : Cases)
	{
		const TArray<const FPieceAction*> Menu = PieceActionsFor(Binding, Case.Ref);

		TestTrue(
			*FString::Printf(
				TEXT("%s: the menu for {%d, %d} should%s offer Delete; it offered [%s]"),
				Case.Description, Case.Ref.StructureId, Case.Ref.PieceIndex,
				Case.bExpectDelete ? TEXT("") : TEXT(" not"), *DescribeMenu(Menu)),
			MenuOffers(Menu, Delete) == Case.bExpectDelete);

		/*
		 * THE GENERAL PROPERTY, WRITTEN OVER THE TABLE RATHER THAN OVER Delete. The handle is
		 * re-derived here from the ref, because that is what the menu itself has to do, and a
		 * ref that resolves to nothing must leave the menu empty whatever the table says.
		 */
		const int32 Handle = Binding.ResolvePiece(Case.Ref);

		int32 ExpectedRows = 0;

		for (const FPieceAction& Action : Table)
		{
			if (Handle != INDEX_NONE && Action.CanRun != nullptr && Action.CanRun(Binding, Handle))
			{
				++ExpectedRows;
			}
		}

		TestTrue(
			*FString::Printf(
				TEXT("%s: the menu for {%d, %d} should hold exactly the %d row(s) whose CanRun says yes; it held %d [%s]"),
				Case.Description, Case.Ref.StructureId, Case.Ref.PieceIndex,
				ExpectedRows, Menu.Num(), *DescribeMenu(Menu)),
			Menu.Num() == ExpectedRows);

		for (const FPieceAction* Offered : Menu)
		{
			TestTrue(
				*FString::Printf(TEXT("%s: the menu must not offer a null row"), Case.Description),
				Offered != nullptr);

			if (Offered == nullptr)
			{
				continue;
			}

			/*
			 * IDENTITY AGAINST THE SHIPPED TABLE, NOT A COPY OF IT. The caller's next move is
			 * to hand this straight to RunPieceAction, and a presenter comparing what it
			 * showed against what was chosen compares pointers.
			 */
			const bool bIsATableRow = Offered >= Table.GetData() && Offered < Table.GetData() + Table.Num();

			TestTrue(
				*FString::Printf(
					TEXT("%s: the menu must name a row of AllPieceActions() rather than a copy of one ('%s')"),
					Case.Description,
					Offered->Label != nullptr ? Offered->Label : TEXT("<null label>")),
				bIsATableRow);

			if (bIsATableRow)
			{
				TestTrue(
					*FString::Printf(
						TEXT("%s: the menu offered '%s', whose CanRun says no"),
						Case.Description,
						Offered->Label != nullptr ? Offered->Label : TEXT("<null label>")),
					Offered->CanRun != nullptr && Handle != INDEX_NONE && Offered->CanRun(Binding, Handle));
			}
		}
	}

	ReleasePieceActionStandIns(StandIns);

	return true;
}

/**
 * THE COMMIT PATH CONSULTS CanRun, AND REFUSES BEFORE IT RUNS ANYTHING.
 *
 * THE DECISION THIS PINS, STATED SO IT IS DELIBERATE. RunPieceAction re-resolves the ref,
 * which refuses a REMOVED piece; CanRun additionally refuses a RELEASED one. Those two
 * guards do not overlap, so the commit path has been strictly MORE PERMISSIVE than the menu:
 * an action could be committed against a brick already tumbling through the air, by a route
 * the menu would never have offered. That is closed here, and the reasoning matters more than
 * the outcome: CanRun is the row's own statement of when it is meaningful, so running Run
 * outside it runs a row outside its stated domain. If deleting falling debris is wanted, the
 * answer is to widen Delete's CanRun — one statement of the rule, in one place, that the menu
 * and the commit path then agree on — rather than to leave the commit door wider than the
 * menu and depend on nobody using it. The window this closes is real and ordinary: the
 * cascade that releases a brick between the click that opens the menu and the click that
 * chooses an entry.
 *
 * THE REFUSING TRIPWIRE IS THE ONLY THING THAT CAN SEE IT. Against Delete the two guards
 * agree on almost every ref, so a commit path consulting neither, one or both looks the same.
 * The refusing tripwire's ref resolves perfectly and its CanRun is the sole reason to refuse,
 * and its counter is what distinguishes "refused before running" from "ran, then reported
 * false" — different bugs, and the second leaves whatever the action did behind it.
 *
 * THE RELEASED-BRICK ROW IS THE SAME CLAIM AGAINST THE SHIPPED ROW, which is what says the
 * guard reaches real actions and not just a test fixture.
 *
 * NEEDS A TICKING WORLD: no.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceActionsCommitRespectsCanRunTest,
	"DestructionGame.Core.PieceActions.CommitRespectsCanRun",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceActionsCommitRespectsCanRunTest::RunTest(const FString& Parameters)
{
	using namespace PieceActionsTestSupport;

	const FPieceAction* Delete = FindPieceAction(TEXT("Delete"));

	if (!RequireAction(*this, Delete, TEXT("Delete")))
	{
		return true;
	}

	/*
	 *   [ 2 ] floater, connected to nothing: Falling, so a push RELEASES it
	 *
	 *   [ 1 ]                  resting on the pad, live throughout
	 *      \
	 *      [  pad 0  ]
	 *      ==========
	 */
	const TArray<FPieceActionPieceSpec> PieceSpecs = {
		{ PieceActionBrickMassKg, true },  // 0: pad
		{ PieceActionBrickMassKg, false }, // 1: brick on the pad
		{ PieceActionBrickMassKg, false }  // 2: floater, RELEASED below
	};

	const TArray<FPieceActionJointSpec> JointSpecs = {
		{ 0, 1, PieceActionBedJointNormal }
	};

	FStructureBinding Binding;
	TArray<UObject*> StandIns;
	BuildPieceActionBinding(Binding, PieceSpecs, JointSpecs, StandIns);

	Binding.SolveLoads();
	Binding.ApplyResults();

	TestTrue(TEXT("fixture: the floater should have been released"), Binding.IsReleased(2));

	/*
	 * AND IT IS NOT REMOVED. This is what makes the row discriminating: the re-resolve alone
	 * accepts it happily, because a released piece is very much still in the graph.
	 */
	TestTrue(TEXT("fixture: the released floater is still in the graph, so the re-resolve accepts it"),
		!Binding.IsPieceRemoved(2));

	TestTrue(
		FString::Printf(TEXT("fixture: the released floater's ref must still resolve to handle 2, got %d"),
			Binding.ResolvePiece({ ThisStructure, 2 })),
		Binding.ResolvePiece({ ThisStructure, 2 }) == 2);

	TestTrue(TEXT("fixture: Delete's own CanRun already says no to a released brick"),
		!Delete->CanRun(Binding, 2));

	TripwireRunCount = 0;
	RefusedTripwireRunCount = 0;

	/*
	 * THE SHARP ROW: the ref resolves, and CanRun is the only reason to refuse. A commit path
	 * that never asks runs this action against a live piece and the counter says so.
	 */
	{
		const FPieceActionResult Refused =
			RunPieceAction(Binding, { ThisStructure, 1 }, RefusedTripwireAction);

		TestTrue(
			FString::Printf(
				TEXT("committing an action whose CanRun says no must report that it did nothing, bRan reports %d"),
				Refused.bRan ? 1 : 0),
			!Refused.bRan);

		TestTrue(
			FString::Printf(TEXT("a refused action must hand back no actor, got %s"),
				*GetNameSafe(Refused.ActorToDestroy)),
			Refused.ActorToDestroy == nullptr);

		TestTrue(
			FString::Printf(
				TEXT("an action whose CanRun says no must never be ENTERED, but Run has been entered %d time(s)"),
				RefusedTripwireRunCount),
			RefusedTripwireRunCount == 0);
	}

	/* The same claim against the shipped row, so the guard is not a fixture-only affair. */
	{
		const FPieceActionResult Refused = RunPieceAction(Binding, { ThisStructure, 2 }, *Delete);

		TestTrue(
			FString::Printf(
				TEXT("deleting a RELEASED brick must report that it did nothing, bRan reports %d"),
				Refused.bRan ? 1 : 0),
			!Refused.bRan);

		TestTrue(
			FString::Printf(TEXT("a refused delete must hand back no actor, got %s"),
				*GetNameSafe(Refused.ActorToDestroy)),
			Refused.ActorToDestroy == nullptr);

		TestTrue(
			FString::Printf(
				TEXT("a refused delete must leave the released brick in the graph, IsPieceRemoved reports %d"),
				Binding.IsPieceRemoved(2) ? 1 : 0),
			!Binding.IsPieceRemoved(2));

		TestTrue(
			FString::Printf(TEXT("a refused delete must leave the released brick holding its own actor, got %s"),
				*GetNameSafe(Binding.GetActor(2))),
			Binding.GetActor(2) == StandIns[2]);

		TestTrue(
			FString::Printf(TEXT("a refused delete must leave all 3 pieces live, got %d"),
				Binding.GetStructure().NumLivePieces()),
			Binding.GetStructure().NumLivePieces() == 3);
	}

	/*
	 * THE POSITIVE CONTROLS. Without them a commit path that refuses everything passes both
	 * rows above, which would hide every action from every brick forever.
	 */
	{
		const FPieceActionResult Ran = RunPieceAction(Binding, { ThisStructure, 1 }, TripwireAction);

		TestTrue(
			FString::Printf(
				TEXT("control: an action whose CanRun says yes must run against a live piece, bRan reports %d"),
				Ran.bRan ? 1 : 0),
			Ran.bRan);

		TestTrue(
			FString::Printf(TEXT("control: the accepting tripwire's Run should have been entered exactly once, got %d"),
				TripwireRunCount),
			TripwireRunCount == 1);
	}

	{
		const FPieceActionResult Ran = RunPieceAction(Binding, { ThisStructure, 1 }, *Delete);

		TestTrue(
			FString::Printf(TEXT("control: deleting a live, unreleased brick must still run, bRan reports %d"),
				Ran.bRan ? 1 : 0),
			Ran.bRan);

		TestTrue(
			FString::Printf(TEXT("control: deleting a live brick should hand back its own actor (%s), got %s"),
				*GetNameSafe(StandIns[1]), *GetNameSafe(Ran.ActorToDestroy)),
			Ran.ActorToDestroy == StandIns[1]);

		TestTrue(TEXT("control: the deleted piece must actually be gone from the graph"),
			Binding.IsPieceRemoved(1));
	}

	ReleasePieceActionStandIns(StandIns);

	return true;
}

/**
 * THE COMMIT PATH FAILS CLOSED ON A REF THAT NO LONGER NAMES A LIVE PIECE OF THIS
 * STRUCTURE: nothing runs, nothing changes, and no actor is handed back to be destroyed.
 *
 * WHY THE REF IS RE-RESOLVED AT COMMIT AT ALL. The ref is the durable identity and the
 * handle is a momentary answer. Between the click that opened the menu and the click that
 * chose an entry, the piece can have been removed by another route entirely — a cascade, a
 * second player, the previous entry on the same menu. A handle captured when the menu
 * opened is exactly the stale value this re-resolution exists to reject, and the wrong
 * answer is a confident handle to somebody else's brick.
 *
 * A MATRIX RATHER THAN AN EXAMPLE, because every rejection route has to be separately
 * closed and the wrong answer in each case is a plausible small integer. The sharp row is
 * the REMOVED piece: its slot stays a valid array index forever, so a bounds check alone
 * accepts it.
 *
 * TWO ACTIONS PER ROW, AND THE SECOND IS THE MECHANISM. Against Delete a rejected ref shows
 * up only as an absence — nothing was removed — which a commit path that ran the action and
 * THEN reported false would produce just as well. The tripwire counts entries into Run, so
 * it distinguishes "refused before running" from "ran and reported nothing", and those are
 * different bugs.
 *
 * AND THE POSITIVE CONTROLS ARE AT THE END, because without them a RunPieceAction that
 * returns a default-constructed result for everything passes the entire matrix.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceActionsCommitRefFailsClosedTest,
	"DestructionGame.Core.PieceActions.CommitRefFailsClosed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceActionsCommitRefFailsClosedTest::RunTest(const FString& Parameters)
{
	using namespace PieceActionsTestSupport;

	const FPieceAction* Delete = FindPieceAction(TEXT("Delete"));

	if (!RequireAction(*this, Delete, TEXT("Delete")))
	{
		return true;
	}

	/*
	 *   [ 2 ]   REMOVED before the matrix runs
	 *   [ 1 ]
	 *   [ 0 ]   grounded pad
	 *   =====
	 */
	const TArray<FPieceActionPieceSpec> PieceSpecs = {
		{ PieceActionBrickMassKg, true },  // 0: pad
		{ PieceActionBrickMassKg, false }, // 1: brick on the pad
		{ PieceActionBrickMassKg, false }  // 2: top brick, REMOVED below
	};

	const TArray<FPieceActionJointSpec> JointSpecs = {
		{ 0, 1, PieceActionBedJointNormal },
		{ 1, 2, PieceActionBedJointNormal }
	};

	FStructureBinding Binding;
	TArray<UObject*> StandIns;
	BuildPieceActionBinding(Binding, PieceSpecs, JointSpecs, StandIns);

	Binding.SolveLoads();

	TestTrue(TEXT("fixture: the top brick should remove cleanly"), Binding.RemovePiece(2));

	TripwireRunCount = 0;

	struct FRefCase
	{
		const TCHAR* Description;
		FPieceRef Ref;
	};

	const TArray<FRefCase> Cases = {
		/*
		 * THE SHARP ONE. A tombstoned slot is still a valid array index, so a bounds check
		 * alone accepts it and the action commits against a piece that is not in the graph.
		 */
		{ TEXT("a piece that has already been removed"), { ThisStructure, 2 } },

		{ TEXT("a valid index belonging to a DIFFERENT structure"), { SomeOtherStructure, 0 } },
		{ TEXT("a ref with no structure id"), { INDEX_NONE, 0 } },
		{ TEXT("a ref with no piece index"), { ThisStructure, INDEX_NONE } },
		{ TEXT("a wholly default ref"), { } },

		{ TEXT("a negative index"), { ThisStructure, -1 } },
		{ TEXT("one past the end"), { ThisStructure, 3 } },
		{ TEXT("a wildly out-of-range index"), { ThisStructure, MAX_int32 } },
		{ TEXT("MIN_int32, which must not be negated into range"), { ThisStructure, MIN_int32 } },
	};

	for (const FRefCase& Case : Cases)
	{
		for (const FPieceAction* Action : { Delete, &TripwireAction })
		{
			const FPieceActionResult Result = RunPieceAction(Binding, Case.Ref, *Action);

			TestTrue(
				FString::Printf(TEXT("%s: committing '%s' against {%d, %d} must report that it did nothing, bRan reports %d"),
					Case.Description, Action->Label, Case.Ref.StructureId, Case.Ref.PieceIndex,
					Result.bRan ? 1 : 0),
				!Result.bRan);

			/*
			 * NO ORPHAN, EITHER. A commit that did nothing has produced no actor to destroy,
			 * and handing one back has the caller destroy a brick that is still in the wall.
			 */
			TestTrue(
				FString::Printf(TEXT("%s: committing '%s' against {%d, %d} must hand back no actor, got %s"),
					Case.Description, Action->Label, Case.Ref.StructureId, Case.Ref.PieceIndex,
					*GetNameSafe(Result.ActorToDestroy)),
				Result.ActorToDestroy == nullptr);
		}

		/* NOTHING WAS TOUCHED — the graph, the tombstones and the actors are all as they were. */
		TestTrue(
			FString::Printf(TEXT("%s: the handle range must be unchanged, got %d"),
				Case.Description, Binding.NumPieces()),
			Binding.NumPieces() == PieceSpecs.Num());

		TestTrue(
			FString::Printf(TEXT("%s: the two live pieces must still be live, got %d"),
				Case.Description, Binding.GetStructure().NumLivePieces()),
			Binding.GetStructure().NumLivePieces() == 2);

		for (const int32 Survivor : { 0, 1 })
		{
			TestTrue(
				FString::Printf(TEXT("%s: piece %d must still be in the graph"), Case.Description, Survivor),
				!Binding.IsPieceRemoved(Survivor));

			TestTrue(
				FString::Printf(TEXT("%s: piece %d must still resolve to its own stand-in, got %s"),
					Case.Description, Survivor, *GetNameSafe(Binding.GetActor(Survivor))),
				Binding.GetActor(Survivor) == StandIns[Survivor]);
		}

		/*
		 * AND Run WAS NEVER ENTERED. This is the difference between refusing before running
		 * and running before reporting nothing — the second leaves whatever the action did
		 * behind it, which for an action that is not Delete need not be visible above at all.
		 */
		TestTrue(
			FString::Printf(TEXT("%s: a refused ref must not enter Run at all, but Run has been entered %d time(s)"),
				Case.Description, TripwireRunCount),
			TripwireRunCount == 0);
	}

	/*
	 * THE POSITIVE CONTROLS. Without these the whole matrix is satisfied by a commit path
	 * that returns a default result for everything, and the tripwire's counter is satisfied
	 * by one that never runs anything.
	 */
	{
		const FPieceActionResult Ran = RunPieceAction(Binding, { ThisStructure, 0 }, TripwireAction);

		TestTrue(
			FString::Printf(TEXT("control: committing the tripwire against a LIVE piece must report that it ran, bRan reports %d"),
				Ran.bRan ? 1 : 0),
			Ran.bRan);

		/*
		 * The counter, not the return value, is what says Run was entered — and nothing is
		 * asserted here about ActorToDestroy, because the tripwire removes nothing and what
		 * a non-destroying action should hand back is not part of this requirement.
		 */
		TestTrue(
			FString::Printf(TEXT("control: the tripwire's Run should have been entered exactly once, got %d"),
				TripwireRunCount),
			TripwireRunCount == 1);
	}

	{
		const FPieceActionResult Ran = RunPieceAction(Binding, { ThisStructure, 1 }, *Delete);

		TestTrue(
			FString::Printf(TEXT("control: deleting a LIVE piece must report that it ran, bRan reports %d"),
				Ran.bRan ? 1 : 0),
			Ran.bRan);

		TestTrue(
			FString::Printf(TEXT("control: deleting a live piece should hand back its own actor (%s), got %s"),
				*GetNameSafe(StandIns[1]), *GetNameSafe(Ran.ActorToDestroy)),
			Ran.ActorToDestroy == StandIns[1]);

		TestTrue(TEXT("control: the deleted piece must actually be gone from the graph"),
			Binding.IsPieceRemoved(1));
	}

	ReleasePieceActionStandIns(StandIns);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
