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
 * Named uniquely, not anonymous: a unity build merges files into one translation unit, so
 * two anonymous namespaces would collide. The `using namespace` sits in each RunTest for the same reason.
 */
namespace PieceActionsTestSupport
{
	using namespace DestructionProfiles;

	/** A standard UK metric brick, 215 x 102.5 x 65 mm, in cm3. */
	constexpr double PieceActionBrickVolumeCubicCm = 21.5 * 10.25 * 6.5;

	/** A standard clay brick's mass, derived. No assertion here depends on the value. */
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
	 * A stand-in for a brick actor. Not NewObject<UObject>: UObject is abstract and trips an
	 * ensure that counts as a test error. UStaticMeshComponent is concrete and needs no world.
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

	/**
	 * Build a binding with joints of the given strength. A parameter because Unbreakable joints
	 * make SolveLoads and SolveAndBreak indistinguishable; only real mortar can tell them apart.
	 */
	void BuildPieceActionBinding(
		FStructureBinding& Out,
		const TArray<FPieceActionPieceSpec>& PieceSpecs,
		const TArray<FPieceActionJointSpec>& JointSpecs,
		TArray<UObject*>& OutStandIns,
		const FConnectionStrength& Strength)
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
			Connection.Strength = Strength;
			Out.AddConnection(Connection);
		}
	}

	/** Overload with Unbreakable joints. */
	void BuildPieceActionBinding(
		FStructureBinding& Out,
		const TArray<FPieceActionPieceSpec>& PieceSpecs,
		const TArray<FPieceActionJointSpec>& JointSpecs,
		TArray<UObject*>& OutStandIns)
	{
		BuildPieceActionBinding(Out, PieceSpecs, JointSpecs, OutStandIns, Unbreakable);
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

	/** Fail loudly rather than dereference a null row; a crash aborts the whole automation run. */
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
	 * A test-side action that counts entries into Run. Distinguishes "refused before running"
	 * from "ran, then reported false", which look the same against Delete.
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
	 * The tripwire with CanRun always false. Against Delete, ResolvePiece and CanRun refuse
	 * nearly the same refs; here the ref resolves and CanRun is the only reason to refuse.
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

	/** A selection as text, for failure messages. */
	FString DescribePieceActionRefs(TArrayView<const FPieceRef> Refs)
	{
		if (Refs.Num() == 0)
		{
			return TEXT("<empty>");
		}

		FString Line;

		for (int32 Index = 0; Index < Refs.Num(); ++Index)
		{
			Line += FString::Printf(
				TEXT("%s{%d,%d}"),
				Index == 0 ? TEXT("") : TEXT(", "),
				Refs[Index].StructureId,
				Refs[Index].PieceIndex);
		}

		return Line;
	}

	/** A menu's labels as text, for failure messages. */
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
 * Every row of the action table is well-formed: the table is non-empty, and each row has a
 * non-empty unique label and both function pointers. A sweep with no exact count, so a new
 * action is a row, not a test edit. Green on arrival; prove it bites by adding a row with a null Run.
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

		// CanRun lets the menu filter without naming a specific action.
		TestTrue(
			*FString::Printf(TEXT("%s must carry a CanRun, got null"), *Where),
			Action.CanRun != nullptr);

		TestTrue(
			*FString::Printf(TEXT("%s must carry a Run, got null"), *Where),
			Action.Run != nullptr);

		// Duplicate labels would show twice and make lookup by label order-dependent.
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
 * Delete removes the brick, returns its actor, and leaves the structure re-solved.
 *
 * - The actor is captured before the action runs; GetActor is null afterwards.
 * - The slot is tombstoned, not compacted: NumPieces unchanged, NumLivePieces one lower.
 *   Compacting would re-point every joint above the hole.
 * - The commit path re-solves: the test never calls SolveLoads after it, so the roof reads
 *   Falling only if the commit re-solved.
 * - A second delete on the now-stale ref reports that it did nothing.
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
	 * Delete is irreversible, so its row carries bIsDestructive as data; a panel can then draw
	 * it differently without comparing captions. Checked on this row only, not swept: the flag
	 * defaults to false.
	 */
	TestTrue(
		TEXT("Delete removes a brick and nothing puts it back, so its row must declare itself destructive"),
		Delete->bIsDestructive);

	/*
	 *      [ roof 2 ]
	 *          |          bed joint
	 *    [ column 1 ]     <- deleted; a middle handle, so a tombstone differs
	 *          |             from compaction.
	 *    [  pad 0  ]
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

	// Preconditions. The roof reading Supported now is the control for the re-solve check below.
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

	TestTrue(
		TEXT("Delete should be offered on a live, unreleased brick"),
		Delete->CanRun(Binding, 1));

	const FPieceRef Ref{ ThisStructure, 1 };

	const FPieceActionResult Result = RunPieceAction(Binding, Ref, *Delete);

	TestTrue(
		FString::Printf(TEXT("deleting a live brick should report that it did something, bRan reports %d"),
			Result.bRan ? 1 : 0),
		Result.bRan);

	// The actor must be captured before the action runs; GetActor is null afterwards.
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

	// NumPieces is the handle range; shrinking means compaction re-pointed every handle above the hole.
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

	// Nothing since RunPieceAction called SolveLoads, so Falling here proves the commit re-solved.
	TestTrue(
		FString::Printf(
			TEXT("the commit path must re-solve: with its only support deleted the roof should read Falling, got %s"),
			NameOfPieceSupport(Binding.GetStructure().GetPieceSupport(2))),
		Binding.GetStructure().GetPieceSupport(2) == EPieceSupport::Falling);

	TestTrue(
		FString::Printf(TEXT("the grounded pad must still read Grounded after the re-solve, got %s"),
			NameOfPieceSupport(Binding.GetStructure().GetPieceSupport(0))),
		Binding.GetStructure().GetPieceSupport(0) == EPieceSupport::Grounded);

	// The ref is now stale; returning the actor again would have the caller destroy it twice.
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
 * Delete's CanRun refuses removed, released and out-of-range handles. Removed and released
 * are separate records (graph tombstone vs binding latch), so each needs its own row; the
 * released brick is asserted not removed. The accepting rows stop an always-false CanRun passing.
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

	// Not removed, so a CanRun that only checks IsPieceRemoved fails the released row.
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
		{ TEXT("the grounded pad is a live, unreleased brick"), 0, true },
		{ TEXT("the second brick on the pad is a live, unreleased brick"), 3, true },

		{ TEXT("a REMOVED piece is not there to delete"), 1, false },
		{ TEXT("a RELEASED piece has already been handed to physics"), 2, false },

		// Unknown handles fail closed.
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
 * The menu for a clicked ref is exactly the table rows whose CanRun says yes; an unresolvable
 * ref (another structure, removed piece, or a default ref from a trace that hit nothing) gets
 * an empty menu. Asserted over AllPieceActions() so a new action needs no test edit, plus one
 * explicit Delete row so an empty table cannot pass. Offered pointers must be rows of the
 * shipped table, not copies. No world needed.
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
	 * Piece 1 is then removed, giving removed, released and live pieces in one binding (same
	 * shape as CanRunFiltersTheMenu).
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
	 * Kept as its own statement: inside the Printf the argument evaluations are unsequenced,
	 * and MSVC read IsReleased before ApplyResults ran.
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
		{ TEXT("a live grounded brick"), { ThisStructure, 0 }, true },
		{ TEXT("a live brick resting on the pad"), { ThisStructure, 3 }, true },

		{ TEXT("a REMOVED piece is not there to act on"), { ThisStructure, 1 }, false },
		{ TEXT("a RELEASED piece is already tumbling"), { ThisStructure, 2 }, false },

		// Refs that resolve to nothing. A default ref is what a trace that hit no brick produces.
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

		// The general property, over the whole table. An unresolved ref must give an empty menu.
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

			// Must be a row of the shipped table, not a copy; callers compare pointers.
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
 * The commit path consults CanRun and refuses before entering Run. Re-resolving the ref only
 * refuses removed pieces; CanRun also refuses released ones, e.g. a brick released by a
 * cascade between opening the menu and choosing an entry. To allow deleting debris, widen
 * Delete's CanRun rather than the commit path. The refusing tripwire isolates CanRun as the
 * only reason to refuse; the released-brick row checks the shipped Delete. No world needed.
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

	// Not removed, so the re-resolve alone accepts it.
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

	// The ref resolves; CanRun is the only reason to refuse.
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

	// The same claim against the shipped Delete row.
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

	// Positive controls: a commit path that refuses everything would pass the rows above.
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
 * The commit path fails closed on a ref that no longer names a live piece of this structure:
 * nothing runs, nothing changes, no actor is returned. The ref is re-resolved at commit
 * because the piece may have been removed since the menu opened. The sharp row is a removed
 * piece, whose slot is still a valid index. Each row runs Delete and the tripwire, which
 * counts entries into Run. Positive controls at the end stop a default result passing.
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
		// A tombstoned slot is still a valid index, so a bounds check alone accepts it.
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

			// Returning an actor here would have the caller destroy a brick still in the wall.
			TestTrue(
				FString::Printf(TEXT("%s: committing '%s' against {%d, %d} must hand back no actor, got %s"),
					Case.Description, Action->Label, Case.Ref.StructureId, Case.Ref.PieceIndex,
					*GetNameSafe(Result.ActorToDestroy)),
				Result.ActorToDestroy == nullptr);
		}

		// Graph, tombstones and actors unchanged.
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

		// Run was never entered: refused before running, not ran then reported nothing.
		TestTrue(
			FString::Printf(TEXT("%s: a refused ref must not enter Run at all, but Run has been entered %d time(s)"),
				Case.Description, TripwireRunCount),
			TripwireRunCount == 0);
	}

	// Positive controls: otherwise a path returning a default result for everything passes.
	{
		const FPieceActionResult Ran = RunPieceAction(Binding, { ThisStructure, 0 }, TripwireAction);

		TestTrue(
			FString::Printf(TEXT("control: committing the tripwire against a LIVE piece must report that it ran, bRan reports %d"),
				Ran.bRan ? 1 : 0),
			Ran.bRan);

		// ActorToDestroy is not asserted: the tripwire removes nothing.
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

/**
 * The menu for a selection is the intersection: an action is offered only if every selected
 * piece's CanRun says yes. The union would offer Delete and then silently skip some bricks.
 * The {live, released} row is the one that separates them. An empty selection offers nothing
 * (a naive "no piece said no" offers everything). Asserted over AllPieceActions(); offered
 * pointers must be rows of the shipped table. No world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceActionsMenuIntersectsTheSelectionTest,
	"DestructionGame.Core.PieceActions.MenuOffersWhatEveryPieceCanRun",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceActionsMenuIntersectsTheSelectionTest::RunTest(const FString& Parameters)
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
	 * Piece 1 is then removed. Same fixture as the single-piece menu test.
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

	const int32 ReleasedCount = Binding.ApplyResults();

	TestTrue(
		FString::Printf(TEXT("fixture: the push should release exactly the floater, got %d released"),
			ReleasedCount),
		ReleasedCount == 1 && Binding.IsReleased(2));

	TestTrue(TEXT("fixture: the released floater is still in the graph, so it is not the removal row"),
		!Binding.IsPieceRemoved(2));

	TestTrue(TEXT("fixture: the brick on the pad should remove cleanly"), Binding.RemovePiece(1));

	struct FSelectionMenuCase
	{
		const TCHAR* Description;
		TArray<FPieceRef> Refs;
		bool bExpectDelete;
	};

	const TArray<FSelectionMenuCase> Cases = {
		// Offering rows first, so a menu that offers nothing cannot pass.
		{
			TEXT("one live brick — the single-piece flow, as the one-element case"),
			{ { ThisStructure, 0 } }, true
		},
		{
			TEXT("two live bricks"),
			{ { ThisStructure, 0 }, { ThisStructure, 3 } }, true
		},
		{
			TEXT("the same two the other way round"),
			{ { ThisStructure, 3 }, { ThisStructure, 0 } }, true
		},
		{
			// The union fails this row: CanRun says no to released piece 2 and yes to piece 0.
			TEXT("a live brick and a RELEASED one: the intersection offers nothing"),
			{ { ThisStructure, 0 }, { ThisStructure, 2 } }, false
		},
		{
			TEXT("a RELEASED brick and a live one, the other way round"),
			{ { ThisStructure, 2 }, { ThisStructure, 0 } }, false
		},
		{
			TEXT("two live bricks and a REMOVED one"),
			{ { ThisStructure, 0 }, { ThisStructure, 3 }, { ThisStructure, 1 } }, false
		},
		{
			// The empty intersection is vacuously everything; the menu must still be empty.
			TEXT("no selection at all"),
			TArray<FPieceRef>(), false
		},
		{
			TEXT("a live brick plus a click that hit the floor"),
			{ { ThisStructure, 0 }, { } }, false
		},
		{
			TEXT("a live brick plus somebody else's structure"),
			{ { ThisStructure, 0 }, { SomeOtherStructure, 0 } }, false
		},
		{
			TEXT("a live brick plus an index one past the end"),
			{ { ThisStructure, 0 }, { ThisStructure, 4 } }, false
		},
		{
			TEXT("a live brick plus MIN_int32, which must not be negated into range"),
			{ { ThisStructure, 0 }, { ThisStructure, MIN_int32 } }, false
		},
		{
			TEXT("nothing but unresolvable refs"),
			{ { }, { SomeOtherStructure, 0 } }, false
		},
	};

	const TArrayView<const FPieceAction> Table = AllPieceActions();

	for (const FSelectionMenuCase& Case : Cases)
	{
		const TArray<const FPieceAction*> Menu = PieceActionsFor(Binding, Case.Refs);

		TestTrue(
			*FString::Printf(
				TEXT("%s: the menu for [%s] should%s offer Delete; it offered [%s]"),
				Case.Description, *DescribePieceActionRefs(Case.Refs),
				Case.bExpectDelete ? TEXT("") : TEXT(" not"), *DescribeMenu(Menu)),
			MenuOffers(Menu, Delete) == Case.bExpectDelete);

		/*
		 * An action is expected iff the selection is non-empty, every ref resolves, and every
		 * handle's CanRun says yes.
		 */
		bool bEveryRefResolves = Case.Refs.Num() > 0;

		for (const FPieceRef& Ref : Case.Refs)
		{
			bEveryRefResolves = bEveryRefResolves && Binding.ResolvePiece(Ref) != INDEX_NONE;
		}

		int32 ExpectedRows = 0;

		for (const FPieceAction& Action : Table)
		{
			bool bEveryPieceAllows = bEveryRefResolves && Action.CanRun != nullptr;

			for (const FPieceRef& Ref : Case.Refs)
			{
				bEveryPieceAllows = bEveryPieceAllows
					&& Action.CanRun(Binding, Binding.ResolvePiece(Ref));
			}

			ExpectedRows += bEveryPieceAllows ? 1 : 0;
		}

		TestTrue(
			*FString::Printf(
				TEXT("%s: the menu for [%s] should hold exactly the %d row(s) EVERY piece allows; it held %d [%s]"),
				Case.Description, *DescribePieceActionRefs(Case.Refs),
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

			// Must be a row of the shipped table, not a copy; the caller commits this pointer.
			TestTrue(
				*FString::Printf(
					TEXT("%s: the menu must name a row of AllPieceActions() rather than a copy of one ('%s')"),
					Case.Description,
					Offered->Label != nullptr ? Offered->Label : TEXT("<null label>")),
				Offered >= Table.GetData() && Offered < Table.GetData() + Table.Num());

			for (const FPieceRef& Ref : Case.Refs)
			{
				const int32 Handle = Binding.ResolvePiece(Ref);

				TestTrue(
					*FString::Printf(
						TEXT("%s: the menu offered '%s', which piece {%d,%d} (handle %d) does not allow"),
						Case.Description,
						Offered->Label != nullptr ? Offered->Label : TEXT("<null label>"),
						Ref.StructureId, Ref.PieceIndex, Handle),
					Offered->CanRun != nullptr && Handle != INDEX_NONE
						&& Offered->CanRun(Binding, Handle));
			}
		}
	}

	ReleasePieceActionStandIns(StandIns);

	return true;
}

/**
 * A batched commit runs the action on every piece, returns every orphan, and solves exactly
 * once, after the last one ran. Extra solves are invisible except through NumSolves (tens of
 * ms each at scenario scale), so cost is asserted as a contrast: two single commits cost 2,
 * one batch of two costs 1.
 *
 * Ordering is checked separately: piece 2 spans two pads, so only a solve after both
 * removals reads Falling; a mid-batch solve passes the count but reads Supported, and
 * ApplyResults then refuses to release the orphaned pieces.
 *
 * A stale ref skips only that piece, unlike the menu: the commit was already authorised for
 * the pieces still there. World-free; orphans are returned, not destroyed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceActionsBatchSolvesOnceTest,
	"DestructionGame.Core.PieceActions.BatchRunsEveryPieceAndSolvesOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceActionsBatchSolvesOnceTest::RunTest(const FString& Parameters)
{
	using namespace PieceActionsTestSupport;

	const FPieceAction* Delete = FindPieceAction(TEXT("Delete"));

	if (!RequireAction(*this, Delete, TEXT("Delete")))
	{
		return true;
	}

	/*
	 *                 [ 3 ]  roof, resting on the spanner and nothing else
	 *                 [ 2 ]  spanner, resting on BOTH pads
	 *              [ 1 ]  [ 4 ]   two grounded pads, both deleted in one batch
	 *              =====  =====
	 *
	 *   [ 0 ]  an unrelated grounded pad, which must come through untouched
	 *   =====
	 *
	 * Two supports make the ordering falsifiable: one pad gone still reads Supported, both gone
	 * reads Falling, and the solve count is the same either way.
	 */
	const TArray<FPieceActionPieceSpec> PieceSpecs = {
		{ PieceActionBrickMassKg, true },  // 0: bystander pad
		{ PieceActionBrickMassKg, true },  // 1: pad A, DELETED in the batch
		{ PieceActionBrickMassKg, false }, // 2: spanner
		{ PieceActionBrickMassKg, false }, // 3: roof
		{ PieceActionBrickMassKg, true }   // 4: pad B, DELETED in the batch
	};

	const TArray<FPieceActionJointSpec> JointSpecs = {
		{ 1, 2, PieceActionBedJointNormal },
		{ 4, 2, PieceActionBedJointNormal },
		{ 2, 3, PieceActionBedJointNormal }
	};

	FStructureBinding Binding;
	TArray<UObject*> StandIns;
	BuildPieceActionBinding(Binding, PieceSpecs, JointSpecs, StandIns);

	Binding.SolveLoads();

	// Controls for the re-solve: both must read Supported now.
	for (const int32 Piece : { 2, 3 })
	{
		TestTrue(
			*FString::Printf(TEXT("fixture: piece %d should solve as Supported before the pads go, got %s"),
				Piece, NameOfPieceSupport(Binding.GetStructure().GetPieceSupport(Piece))),
			Binding.GetStructure().GetPieceSupport(Piece) == EPieceSupport::Supported);
	}

	// 1: N single commits cost N solves, on an identical twin, as the baseline for the contrast.
	{
		FStructureBinding Twin;
		TArray<UObject*> TwinStandIns;
		BuildPieceActionBinding(Twin, PieceSpecs, JointSpecs, TwinStandIns);

		Twin.SolveLoads();

		const int32 SolvesBefore = Twin.GetStructure().NumSolves();

		RunPieceAction(Twin, { ThisStructure, 1 }, *Delete);
		RunPieceAction(Twin, { ThisStructure, 4 }, *Delete);

		const int32 SolvesSpent = Twin.GetStructure().NumSolves() - SolvesBefore;

		TestEqual(
			FString::Printf(
				TEXT("baseline: two SEPARATE commits should cost two solves (that is the cost the batch removes), they cost %d"),
				SolvesSpent),
			SolvesSpent, 2);

		ReleasePieceActionStandIns(TwinStandIns);
	}

	// 2: one batched commit of the same two pieces costs one solve, run last.
	const TArray<FPieceRef> Selection = { { ThisStructure, 1 }, { ThisStructure, 4 } };

	const int32 SolvesBefore = Binding.GetStructure().NumSolves();

	const FPieceBatchActionResult Result = RunPieceActions(Binding, Selection, *Delete);

	const int32 SolvesSpent = Binding.GetStructure().NumSolves() - SolvesBefore;

	AddInfo(FString::Printf(
		TEXT("the batch for [%s] ran %d action(s), handed back %d orphan(s) and spent %d solve(s)"),
		*DescribePieceActionRefs(Selection), Result.RanCount,
		Result.ActorsToDestroy.Num(), SolvesSpent));

	TestEqual(
		FString::Printf(TEXT("the batch should have run against both selected pieces, it ran %d"),
			Result.RanCount),
		Result.RanCount, 2);

	TestEqual(
		FString::Printf(
			TEXT("committing %d pieces at once must cost exactly ONE solve, not one per piece; it cost %d"),
			Selection.Num(), SolvesSpent),
		SolvesSpent, 1);

	// One orphan per piece that ran, in order; each must be captured before its removal.
	TestEqual(
		FString::Printf(TEXT("the batch should hand back one orphan per piece that ran, it handed back %d"),
			Result.ActorsToDestroy.Num()),
		Result.ActorsToDestroy.Num(), 2);

	if (Result.ActorsToDestroy.Num() == 2)
	{
		TestTrue(
			FString::Printf(TEXT("orphan 0 should be piece 1's own actor (%s), got %s"),
				*GetNameSafe(StandIns[1]), *GetNameSafe(Result.ActorsToDestroy[0])),
			Result.ActorsToDestroy[0] == StandIns[1]);

		TestTrue(
			FString::Printf(TEXT("orphan 1 should be piece 4's own actor (%s), got %s"),
				*GetNameSafe(StandIns[4]), *GetNameSafe(Result.ActorsToDestroy[1])),
			Result.ActorsToDestroy[1] == StandIns[4]);
	}

	for (const int32 Deleted : { 1, 4 })
	{
		TestTrue(
			FString::Printf(TEXT("piece %d must be gone from the graph, IsPieceRemoved reports %d"),
				Deleted, Binding.IsPieceRemoved(Deleted) ? 1 : 0),
			Binding.IsPieceRemoved(Deleted));
	}

	TestEqual(
		FString::Printf(TEXT("two pieces should have left the structure, so 3 should be live, got %d"),
			Binding.GetStructure().NumLivePieces()),
		Binding.GetStructure().NumLivePieces(), 3);

	// Ordering: nothing since the batch called SolveLoads, and a mid-batch solve would read Supported.
	for (const int32 Piece : { 2, 3 })
	{
		TestTrue(
			*FString::Printf(
				TEXT("the batch's ONE solve must come after the LAST removal: piece %d has no path to the ground and should read Falling, got %s"),
				Piece, NameOfPieceSupport(Binding.GetStructure().GetPieceSupport(Piece))),
			Binding.GetStructure().GetPieceSupport(Piece) == EPieceSupport::Falling);
	}

	TestTrue(
		FString::Printf(TEXT("the bystander pad must still read Grounded after the batch, got %s"),
			NameOfPieceSupport(Binding.GetStructure().GetPieceSupport(0))),
		Binding.GetStructure().GetPieceSupport(0) == EPieceSupport::Grounded);

	TestTrue(
		FString::Printf(TEXT("the bystander must still hold its own actor, got %s"),
			*GetNameSafe(Binding.GetActor(0))),
		Binding.GetActor(0) == StandIns[0]);

	// 3: a selection with stale entries runs the rest and still solves exactly once.
	TripwireRunCount = 0;

	{
		const TArray<FPieceRef> Mixed = {
			{ ThisStructure, 1 },          // already deleted by the batch above
			{ SomeOtherStructure, 0 },     // somebody else's wall
			{ },                           // a click that hit the floor
			{ ThisStructure, MAX_int32 },  // names nothing
			{ ThisStructure, 3 }           // live, and the only one that should run
		};

		const int32 Before = Binding.GetStructure().NumSolves();

		const FPieceBatchActionResult Partial = RunPieceActions(Binding, Mixed, *Delete);

		const int32 Spent = Binding.GetStructure().NumSolves() - Before;

		TestEqual(
			FString::Printf(
				TEXT("a selection of five with one live entry should run once, it ran %d [%s]"),
				Partial.RanCount, *DescribePieceActionRefs(Mixed)),
			Partial.RanCount, 1);

		TestEqual(
			FString::Printf(TEXT("and hand back exactly one orphan, it handed back %d"),
				Partial.ActorsToDestroy.Num()),
			Partial.ActorsToDestroy.Num(), 1);

		if (Partial.ActorsToDestroy.Num() == 1)
		{
			TestTrue(
				FString::Printf(TEXT("the orphan should be piece 3's own actor (%s), got %s"),
					*GetNameSafe(StandIns[3]), *GetNameSafe(Partial.ActorsToDestroy[0])),
				Partial.ActorsToDestroy[0] == StandIns[3]);
		}

		TestEqual(
			FString::Printf(TEXT("a partly stale selection must still cost exactly one solve, it cost %d"),
				Spent),
			Spent, 1);

		TestTrue(
			FString::Printf(TEXT("piece 3 should have gone, IsPieceRemoved reports %d"),
				Binding.IsPieceRemoved(3) ? 1 : 0),
			Binding.IsPieceRemoved(3));
	}

	// 4: a wholly stale selection runs nothing; the tripwire counter proves Run was not entered.
	{
		const TArray<FPieceRef> AllStale = {
			{ SomeOtherStructure, 0 },
			{ },
			{ ThisStructure, MIN_int32 }
		};

		const int32 Before = Binding.GetStructure().NumSolves();

		const FPieceBatchActionResult Nothing = RunPieceActions(Binding, AllStale, TripwireAction);

		const int32 Spent = Binding.GetStructure().NumSolves() - Before;

		TestEqual(
			FString::Printf(TEXT("a wholly stale selection must run nothing, it ran %d"), Nothing.RanCount),
			Nothing.RanCount, 0);

		TestEqual(
			FString::Printf(TEXT("a wholly stale selection must hand back no orphans, it handed back %d"),
				Nothing.ActorsToDestroy.Num()),
			Nothing.ActorsToDestroy.Num(), 0);

		TestEqual(
			FString::Printf(
				TEXT("a wholly stale selection must not ENTER any action's Run, it was entered %d time(s)"),
				TripwireRunCount),
			TripwireRunCount, 0);

		// Still one solve: the solve is unconditional, like the single-piece path, by design.
		TestEqual(
			FString::Printf(TEXT("every batched commit costs exactly one solve, even one that ran nothing; it cost %d"),
				Spent),
			Spent, 1);
	}

	// 5: an empty selection follows the same rule.
	{
		const int32 Before = Binding.GetStructure().NumSolves();

		const FPieceBatchActionResult Empty =
			RunPieceActions(Binding, TArrayView<const FPieceRef>(), *Delete);

		const int32 Spent = Binding.GetStructure().NumSolves() - Before;

		TestEqual(
			FString::Printf(TEXT("an empty selection must run nothing, it ran %d"), Empty.RanCount),
			Empty.RanCount, 0);

		TestEqual(
			FString::Printf(TEXT("an empty selection must cost exactly one solve, it cost %d"), Spent),
			Spent, 1);
	}

	ReleasePieceActionStandIns(StandIns);

	return true;
}

/**
 * Deleting one brick through the single-piece door (RunPieceAction) cascades exactly as the
 * batch door does. Both end in SolveAndBreak, but with Unbreakable joints the singular one
 * could revert to SolveLoads unnoticed. Two identical mortar walls, one per door, and the
 * break stamps are compared handle for handle.
 *
 *        [2]  15,000 kg slab            resting on two 100 cm2 bed joints
 *       /   \
 *     [0]   [1]  grounded pads          piece 0 is the one the player deletes
 *
 *   weight       = 15000 kg x 980 cm/s2            = 1.47e7 uu
 *   as built     = 1.47e7 / 2 / 100 cm2 / 10000    = 7.35 MPa   -> 0.735 of mortar's 10
 *   one pad gone = 1.47e7     / 100 cm2 / 10000    = 14.7 MPa   -> 1.47, and it gives
 *
 * The 10,000 uu per MPa.cm2 is written out, not imported from ForceUnitsPerMPaSqCm, so a wrong
 * production constant fails here. Normals are +Z and the load vertical, so compression is the
 * only axis; the joints have no rectangle, so no moment. Asserts on the break stamp (DESIGN.md
 * §4): a given joint reads zero utilisation, like an unloaded one. No world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceActionsSingleDeleteCascadesTest,
	"DestructionGame.Core.PieceActions.OneBrickDeleteSettlesTheWallLikeABatch",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceActionsSingleDeleteCascadesTest::RunTest(const FString& Parameters)
{
	using namespace PieceActionsTestSupport;

	const FPieceAction* const Delete = FindPieceAction(TEXT("Delete"));

	if (!RequireAction(*this, Delete, TEXT("Delete")))
	{
		return true;
	}

	// Held on two pads, over capacity on one.
	constexpr double SlabMassKg = 15000.0;

	// 1 N = 100 uu and 1 cm2 = 100 mm2, so 1 MPa over 1 cm2 is 10,000 uu. Deliberately not imported.
	constexpr double ForceUnitsPerMPaSqCm = 100.0 * 100.0;

	constexpr double SlabWeightUu = SlabMassKg * 980.0;

	const double MortarCompressiveMPa = GeneralPurposeMortar.CompressiveStrengthMPa;

	const double AsBuiltUtilisation =
		SlabWeightUu / 2.0 / PieceActionJointAreaSqCm / ForceUnitsPerMPaSqCm / MortarCompressiveMPa;

	const double OnOnePadUtilisation =
		SlabWeightUu / PieceActionJointAreaSqCm / ForceUnitsPerMPaSqCm / MortarCompressiveMPa;

	constexpr int32 LeftPad = 0;
	constexpr int32 RightPad = 1;
	constexpr int32 Slab = 2;

	// The remaining pad's joint, which must break.
	constexpr int32 SurvivingJoint = 1;

	// The deleted pad's joint, removed rather than broken.
	constexpr int32 DeletedJoint = 0;

	const TArray<FPieceActionPieceSpec> Pieces = {
		{ PieceActionBrickMassKg, /*bIsGrounded*/ true },
		{ PieceActionBrickMassKg, /*bIsGrounded*/ true },
		{ SlabMassKg },
	};

	const TArray<FPieceActionJointSpec> Joints = {
		{ LeftPad, Slab, PieceActionBedJointNormal },
		{ RightPad, Slab, PieceActionBedJointNormal },
	};

	TArray<UObject*> StandIns;

	// Two identical walls, differing only in which door deletes from them.
	FStructureBinding ThroughOneDoor;
	FStructureBinding ThroughTheBatchDoor;

	BuildPieceActionBinding(ThroughOneDoor, Pieces, Joints, StandIns, GeneralPurposeMortar);
	BuildPieceActionBinding(ThroughTheBatchDoor, Pieces, Joints, StandIns, GeneralPurposeMortar);

	// Precondition: the wall stands as built, so any break below is caused by the delete.
	ThroughOneDoor.SolveLoads();

	for (int32 Joint = 0; Joint < ThroughOneDoor.GetStructure().NumConnections(); ++Joint)
	{
		const double Utilisation = ThroughOneDoor.GetStructure().GetConnectionUtilisation(Joint);

		TestTrue(
			FString::Printf(
				TEXT("fixture: as built, joint %d should carry %.9g of capacity — half the slab over 100 cm2 against mortar's %g MPa — it reads %.9g"),
				Joint, AsBuiltUtilisation, MortarCompressiveMPa, Utilisation),
			FMath::IsNearlyEqual(Utilisation, AsBuiltUtilisation, AsBuiltUtilisation * 1.0e-9));
	}

	TestTrue(
		FString::Printf(TEXT("fixture: as built the slab is held, at %.9g of capacity"), AsBuiltUtilisation),
		AsBuiltUtilisation < 1.0);

	TestTrue(
		FString::Printf(TEXT("fixture: on one pad the slab is over capacity, at %.9g"), OnOnePadUtilisation),
		OnOnePadUtilisation > 1.0);

	FPieceRef Ref;
	Ref.StructureId = ThisStructure;
	Ref.PieceIndex = LeftPad;

	const FPieceActionResult Result = RunPieceAction(ThroughOneDoor, Ref, *Delete);

	TestTrue(TEXT("fixture: deleting the left pad should have run"), Result.bRan);

	TestTrue(
		FString::Printf(TEXT("fixture: the left pad should be out of the graph")),
		ThroughOneDoor.IsPieceRemoved(LeftPad));

	const int32 SingleDoorPass = ThroughOneDoor.GetStructure().GetBreakPass(SurvivingJoint);

	AddInfo(FString::Printf(
		TEXT("after one right-click delete: joint %d has %s and its break pass is %d"),
		SurvivingJoint,
		ThroughOneDoor.GetStructure().GetConnection(SurvivingJoint).HasGiven()
			? TEXT("given") : TEXT("NOT given"),
		SingleDoorPass));

	/*
	 * Assert the break stamp, not utilisation: a given joint reads zero, like an unloaded one.
	 * A joint removed with its piece has no pass number, so a stamp means it failed under load.
	 */
	TestEqual(
		FString::Printf(
			TEXT("the single-piece delete must settle the wall: joint %d is at %.9g of capacity once the left pad has gone, so the first sweep after the delete must break it — its break pass is %d"),
			SurvivingJoint, OnOnePadUtilisation, SingleDoorPass),
		SingleDoorPass, 1);

	TestTrue(
		FString::Printf(TEXT("joint %d must have given, not merely been condemned"), SurvivingJoint),
		ThroughOneDoor.GetStructure().GetConnection(SurvivingJoint).HasGiven());

	// The deleted pad's joint is out of the structure but carries no pass number.
	TestTrue(
		FString::Printf(TEXT("the deleted pad's own joint %d must be out of the structure"), DeletedJoint),
		ThroughOneDoor.GetStructure().GetConnection(DeletedJoint).HasGiven());

	TestEqual(
		FString::Printf(
			TEXT("joint %d went with a removed piece rather than failing, so it must carry no pass number"),
			DeletedJoint),
		ThroughOneDoor.GetStructure().GetBreakPass(DeletedJoint), (int32)INDEX_NONE);

	// Outcome: nothing holds the slab up.
	TestFalse(
		TEXT("with its last bed joint given, the slab must have no path to the ground"),
		ThroughOneDoor.GetStructure().IsPieceSupported(Slab));

	TestTrue(
		TEXT("the surviving pad is resting on the earth and must be unmoved by any of this"),
		ThroughOneDoor.GetStructure().IsPieceSupported(RightPad));

	// The same delete through the batch door; the break stamps must match.
	const FPieceRef BatchRefs[] = { Ref };

	const FPieceBatchActionResult BatchResult =
		RunPieceActions(ThroughTheBatchDoor, BatchRefs, *Delete);

	TestEqual(TEXT("fixture: the batch door should have run against exactly one piece"),
		BatchResult.RanCount, 1);

	for (int32 Joint = 0; Joint < ThroughTheBatchDoor.GetStructure().NumConnections(); ++Joint)
	{
		const int32 OneDoor = ThroughOneDoor.GetStructure().GetBreakPass(Joint);
		const int32 BatchDoor = ThroughTheBatchDoor.GetStructure().GetBreakPass(Joint);

		TestEqual(
			FString::Printf(
				TEXT("joint %d must give in the same pass whichever door the player came through: one brick says %d, a batch of one says %d"),
				Joint, OneDoor, BatchDoor),
			OneDoor, BatchDoor);
	}

	for (int32 Piece = 0; Piece < ThroughTheBatchDoor.NumPieces(); ++Piece)
	{
		TestEqual(
			FString::Printf(
				TEXT("piece %d must end up in the same state whichever door the player came through"),
				Piece),
			ThroughOneDoor.GetStructure().IsPieceSupported(Piece),
			ThroughTheBatchDoor.GetStructure().IsPieceSupported(Piece));
	}

	ReleasePieceActionStandIns(StandIns);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
