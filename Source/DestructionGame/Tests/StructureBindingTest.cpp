// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/StructureBinding.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Components/StaticMeshComponent.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include <limits>
#include <type_traits>
#include <utility>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Named namespace, unique to this directory: a unity build merges files, so two anonymous
 * BedJointNormal declarations would collide (see CURRENT_STATE.md). The `using namespace` lives
 * inside each RunTest body for the same reason.
 */
namespace StructureBindingTestSupport
{
	using namespace DestructionProfiles;

	/** A standard UK metric brick, 215 x 102.5 x 65 mm, in cm3. */
	constexpr double BrickVolumeCubicCm = 21.5 * 10.25 * 6.5;

	/** A standard clay brick, derived from the profile. No expectation here depends on the value. */
	const double BrickMassKg = ClayBrick.DensityGramsPerCubicCm * BrickVolumeCubicCm / 1000.0;

	/** Horizontal interface, normal pointing up at the piece above: a bed joint. */
	const FVector BedJointNormal(0.0, 0.0, 1.0);

	/** Vertical interface, normal pointing sideways at the neighbour: a head joint. */
	const FVector HeadJointNormal(1.0, 0.0, 0.0);

	constexpr double JointAreaSqCm = 100.0;

	/**
	 * A rooted, world-free stand-in for a brick actor. UStaticMeshComponent, not UObject: UObject is
	 * abstract and instantiating it trips an ensure the automation framework counts as an error.
	 */
	UObject* MakeStandIn()
	{
		UObject* StandIn = NewObject<UStaticMeshComponent>(GetTransientPackage());
		StandIn->AddToRoot();
		return StandIn;
	}

	void ReleaseStandIns(const TArray<UObject*>& StandIns)
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
	DestructionLayout::FPieceBox BoxFor(int32 Index)
	{
		DestructionLayout::FPieceBox Box;
		Box.CentreCm = FVector(Index * 100.0 + 1.0, Index * 10.0 + 2.0, Index + 3.0);
		Box.ExtentCm = FVector(10.75 + Index, 5.125, 3.25);
		return Box;
	}

	bool BoxesMatch(const DestructionLayout::FPieceBox& A, const DestructionLayout::FPieceBox& B)
	{
		return A.CentreCm.Equals(B.CentreCm, 0.0) && A.ExtentCm.Equals(B.ExtentCm, 0.0);
	}

	struct FPieceSpec
	{
		double MassKg = 0.0;
		bool bIsGrounded = false;
	};

	struct FConnectionSpec
	{
		int32 PieceA = INDEX_NONE;
		int32 PieceB = INDEX_NONE;
		FVector Normal = FVector::ZAxisVector;
	};

	/** Build a binding with one stand-in and one box per piece; returns the stand-ins for identity checks and cleanup. */
	void BuildBinding(
		FStructureBinding& Out,
		int32 StructureId,
		const TArray<FPieceSpec>& PieceSpecs,
		const TArray<FConnectionSpec>& ConnectionSpecs,
		TArray<UObject*>& OutStandIns)
	{
		Out.StructureId = StructureId;

		for (int32 Index = 0; Index < PieceSpecs.Num(); ++Index)
		{
			UObject* StandIn = MakeStandIn();
			OutStandIns.Add(StandIn);
			Out.AddPiece(PieceSpecs[Index].MassKg, PieceSpecs[Index].bIsGrounded, StandIn, BoxFor(Index));
		}

		for (const FConnectionSpec& Joint : ConnectionSpecs)
		{
			FConnection Connection;
			Connection.PieceA = Joint.PieceA;
			Connection.PieceB = Joint.PieceB;
			Connection.InterfaceNormal = Joint.Normal;
			Connection.InterfaceAreaSqCm = JointAreaSqCm;
			Connection.Strength = Unbreakable;
			Out.AddConnection(Connection);
		}
	}

	const TCHAR* NameOfSupport(EPieceSupport State)
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

	/**
	 * The type's core invariant: the binding and piece arrays are index-parallel. One entry out of
	 * step and every handle above it resolves to the wrong brick.
	 */
	void CheckArraysAreParallel(
		FAutomationTestBase& Test,
		const FStructureBinding& Binding,
		int32 ExpectedPieces,
		const TCHAR* What)
	{
		Test.TestTrue(
			FString::Printf(TEXT("%s: the binding should span %d handles, got %d"),
				What, ExpectedPieces, Binding.NumPieces()),
			Binding.NumPieces() == ExpectedPieces);

		Test.TestTrue(
			FString::Printf(
				TEXT("%s: the binding spans %d handles and the structure it owns spans %d — the arrays have desynced"),
				What, Binding.NumPieces(), Binding.GetStructure().NumPieces()),
			Binding.NumPieces() == Binding.GetStructure().NumPieces());
	}
}

/*
 * A mutable binding must not hand out a mutable structure: GetStructure().RemovePiece would desync
 * the arrays. decltype on a non-const lvalue picks a non-const overload if one exists, so adding one
 * fails here.
 */
static_assert(
	std::is_same_v<
		decltype(std::declval<FStructureBinding&>().GetStructure()),
		const FStructure&>,
	"FStructureBinding::GetStructure must return a CONST reference even from a mutable "
	"binding: a mutable FStructure& lets a caller call RemovePiece on the graph without "
	"the binding array hearing about it, and the resulting desync is silent.");

/**
 * Removal cannot dangle: one call removes the piece from the graph and clears its actor, and every
 * other handle still resolves to its own stand-in. Removes from the middle (removing the last piece
 * looks like compaction) with distinct stand-ins and boxes so a shifted array reads back wrong.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureBindingRemovalCannotDangleTest,
	"DestructionGame.Core.StructureBinding.RemovalCannotDangle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureBindingRemovalCannotDangleTest::RunTest(const FString& Parameters)
{
	using namespace StructureBindingTestSupport;

	/*
	 *                [ block 4 ]
	 *                [ beam 3  ]
	 *   [pad 0]      [pad 1]      [pad 2]
	 *   ======       ======       ======
	 *
	 * Pad 1 goes, from the middle of the array.
	 */
	const TArray<FPieceSpec> PieceSpecs = {
		{ BrickMassKg, true },  // 0: left pad
		{ BrickMassKg, true },  // 1: middle pad, REMOVED
		{ BrickMassKg, true },  // 2: right pad
		{ BrickMassKg, false }, // 3: beam across all three
		{ BrickMassKg, false }  // 4: block on the beam
	};

	const TArray<FConnectionSpec> ConnectionSpecs = {
		{ 0, 3, BedJointNormal },
		{ 1, 3, BedJointNormal },
		{ 2, 3, BedJointNormal },
		{ 3, 4, BedJointNormal }
	};

	FStructureBinding Binding;
	TArray<UObject*> StandIns;
	BuildBinding(Binding, 7, PieceSpecs, ConnectionSpecs, StandIns);

	CheckArraysAreParallel(*this, Binding, PieceSpecs.Num(), TEXT("as built"));

	// The positive control: every handle resolves to its own stand-in before anything.
	for (int32 Index = 0; Index < PieceSpecs.Num(); ++Index)
	{
		TestTrue(
			FString::Printf(TEXT("as built: handle %d should resolve to its own stand-in, got %s"),
				Index, *GetNameSafe(Binding.GetActor(Index))),
			Binding.GetActor(Index) == StandIns[Index]);

		TestTrue(
			FString::Printf(TEXT("as built: handle %d should carry the box it was laid with"), Index),
			BoxesMatch(Binding.GetBinding(Index).Box, BoxFor(Index)));
	}

	constexpr int32 Removed = 1;

	TestTrue(TEXT("removing a live middle piece should report that it removed one"),
		Binding.RemovePiece(Removed));

	// The handle range never shrinks; callers iterate 0..NumPieces.
	CheckArraysAreParallel(*this, Binding, PieceSpecs.Num(), TEXT("after removing piece 1"));

	for (int32 Index = 0; Index < PieceSpecs.Num(); ++Index)
	{
		const bool bShouldBeGone = Index == Removed;

		TestTrue(
			FString::Printf(TEXT("piece %d should be %s in the graph, IsPieceRemoved reports %d"),
				Index, bShouldBeGone ? TEXT("REMOVED") : TEXT("live"),
				Binding.IsPieceRemoved(Index) ? 1 : 0),
			Binding.IsPieceRemoved(Index) == bShouldBeGone);

		TestTrue(
			FString::Printf(TEXT("the graph and the binding must agree that piece %d is %s"),
				Index, bShouldBeGone ? TEXT("REMOVED") : TEXT("live")),
			Binding.GetStructure().IsPieceRemoved(Index) == bShouldBeGone);

		if (bShouldBeGone)
		{
			TestTrue(
				FString::Printf(TEXT("removed piece %d must let go of its actor, got %s"),
					Index, *GetNameSafe(Binding.GetActor(Index))),
				Binding.GetActor(Index) == nullptr);
		}
		else
		{
			// Under compaction handle 2 would return the beam's stand-in.
			TestTrue(
				FString::Printf(TEXT("handle %d should still resolve to its own stand-in, got %s"),
					Index, *GetNameSafe(Binding.GetActor(Index))),
				Binding.GetActor(Index) == StandIns[Index]);
		}

		/*
		 * The box survives removal, even the removed piece's: debris and replay want it, and a
		 * cleared box would read as real geometry at the origin.
		 */
		TestTrue(
			FString::Printf(
				TEXT("piece %d should keep the box it was laid with, removed or not — got centre (%g, %g, %g) extent (%g, %g, %g)"),
				Index,
				Binding.GetBinding(Index).Box.CentreCm.X,
				Binding.GetBinding(Index).Box.CentreCm.Y,
				Binding.GetBinding(Index).Box.CentreCm.Z,
				Binding.GetBinding(Index).Box.ExtentCm.X,
				Binding.GetBinding(Index).Box.ExtentCm.Y,
				Binding.GetBinding(Index).Box.ExtentCm.Z),
			BoxesMatch(Binding.GetBinding(Index).Box, BoxFor(Index)));
	}

	TestTrue(TEXT("removing an already-removed piece must report that it removed nothing"),
		!Binding.RemovePiece(Removed));

	ReleaseStandIns(StandIns);

	return true;
}

/**
 * An actor destroyed by another route (level transition, lifespan) reads as null, not a dangling
 * pointer; TWeakObjectPtr guarantees that where a raw UObject* would not. The piece stays live in
 * the graph: a dead actor is not a removed piece.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureBindingActorLifetimeIsWeakTest,
	"DestructionGame.Core.StructureBinding.ActorLifetimeIsWeak",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureBindingActorLifetimeIsWeakTest::RunTest(const FString& Parameters)
{
	using namespace StructureBindingTestSupport;

	FStructureBinding Binding;
	Binding.StructureId = 7;

	UObject* Kept = MakeStandIn();

	// Unrooted on purpose so the binding's weak pointer is the only reference and GC can collect it.
	UObject* Doomed = NewObject<UStaticMeshComponent>(GetTransientPackage());

	Binding.AddPiece(BrickMassKg, true, Kept, BoxFor(0));
	Binding.AddPiece(BrickMassKg, false, Doomed, BoxFor(1));

	{
		FConnection Connection;
		Connection.PieceA = 0;
		Connection.PieceB = 1;
		Connection.InterfaceNormal = BedJointNormal;
		Connection.InterfaceAreaSqCm = JointAreaSqCm;
		Connection.Strength = Unbreakable;
		Binding.AddConnection(Connection);
	}

	CheckArraysAreParallel(*this, Binding, 2, TEXT("as built"));

	// Positive control; without it the assertions below pass on a stub.
	TestTrue(
		FString::Printf(TEXT("as built: handle 1 should resolve to the doomed stand-in, got %s"),
			*GetNameSafe(Binding.GetActor(1))),
		Binding.GetActor(1) == Doomed);

	Doomed->MarkAsGarbage();
	Doomed = nullptr;

	TestTrue(
		FString::Printf(TEXT("a destroyed actor must read as null, got %s"),
			*GetNameSafe(Binding.GetActor(1))),
		Binding.GetActor(1) == nullptr);

	// Still null after collection, which is what would turn a raw pointer into a dangle.
	CollectGarbage(RF_NoFlags, true);

	TestTrue(
		FString::Printf(TEXT("a collected actor must still read as null, got %s"),
			*GetNameSafe(Binding.GetActor(1))),
		Binding.GetActor(1) == nullptr);

	TestTrue(TEXT("losing the actor must not remove the piece from the graph"),
		!Binding.IsPieceRemoved(1));

	CheckArraysAreParallel(*this, Binding, 2, TEXT("after the actor was destroyed"));

	TestTrue(
		FString::Printf(TEXT("the surviving handle 0 must be unaffected, got %s"),
			*GetNameSafe(Binding.GetActor(0))),
		Binding.GetActor(0) == Kept);

	Binding.SolveLoads();

	TestTrue(TEXT("a piece whose actor died is still held up by the graph"),
		Binding.GetStructure().IsPieceSupported(1));

	Kept->RemoveFromRoot();

	return true;
}

/**
 * A push releases exactly the pieces the solver no longer holds up. Stranded and Falling differ
 * only as diagnostics, so both release identically. Solving itself releases nothing; only an
 * explicit ApplyResults does.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureBindingReleaseFollowsSupportTest,
	"DestructionGame.Core.StructureBinding.ReleaseFollowsSupport",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureBindingReleaseFollowsSupportTest::RunTest(const FString& Parameters)
{
	using namespace StructureBindingTestSupport;

	/*
	 * All four support states, from Structure.PieceSupportReason (which pins them).
	 *
	 *                       B          piece 4, resting on Y through a bed joint
	 *                       |
	 *      Z   —   X   —    Y          pieces 1, 2, 3
	 *      |
	 *   [ pier ]                       piece 0
	 *   ========
	 *
	 * Pier Grounded, Z Supported, X and Y the knot and Stranded, B on the knot and Falling.
	 */
	const TArray<FPieceSpec> PieceSpecs = {
		{ BrickMassKg, true },  // 0: pier
		{ BrickMassKg, false }, // 1: Z, bed-jointed to the pier
		{ BrickMassKg, false }, // 2: X, in the knot
		{ BrickMassKg, false }, // 3: Y, in the knot
		{ BrickMassKg, false }  // 4: B, resting on Y
	};

	const TArray<FConnectionSpec> ConnectionSpecs = {
		{ 0, 1, BedJointNormal },
		{ 1, 2, HeadJointNormal },
		{ 2, 3, HeadJointNormal },
		{ 3, 4, BedJointNormal }
	};

	const TArray<EPieceSupport> ExpectedSupport = {
		EPieceSupport::Grounded, EPieceSupport::Supported, EPieceSupport::Stranded,
		EPieceSupport::Stranded, EPieceSupport::Falling
	};

	const TArray<bool> ExpectedReleased = { false, false, true, true, true };

	FStructureBinding Binding;
	TArray<UObject*> StandIns;
	BuildBinding(Binding, 7, PieceSpecs, ConnectionSpecs, StandIns);

	CheckArraysAreParallel(*this, Binding, PieceSpecs.Num(), TEXT("as built"));

	for (int32 Index = 0; Index < PieceSpecs.Num(); ++Index)
	{
		TestTrue(
			FString::Printf(TEXT("as built: piece %d must not be released before anything is solved"), Index),
			!Binding.IsReleased(Index));
	}

	Binding.SolveLoads();

	// Solving releases nothing; the push must not become a callback from inside the solver.
	for (int32 Index = 0; Index < PieceSpecs.Num(); ++Index)
	{
		TestTrue(
			FString::Printf(TEXT("solving must release nothing, but piece %d came back released"), Index),
			!Binding.IsReleased(Index));
	}

	// Fixture precondition.
	for (int32 Index = 0; Index < ExpectedSupport.Num(); ++Index)
	{
		TestTrue(
			FString::Printf(TEXT("fixture: piece %d should solve as %s, got %s"),
				Index, NameOfSupport(ExpectedSupport[Index]),
				NameOfSupport(Binding.GetStructure().GetPieceSupport(Index))),
			Binding.GetStructure().GetPieceSupport(Index) == ExpectedSupport[Index]);
	}

	const int32 ReleasedCount = Binding.ApplyResults();

	TestTrue(
		FString::Printf(TEXT("the push should release the 3 pieces nothing is holding up, got %d"),
			ReleasedCount),
		ReleasedCount == 3);

	for (int32 Index = 0; Index < ExpectedReleased.Num(); ++Index)
	{
		TestTrue(
			FString::Printf(TEXT("piece %d is %s, so it should be %s; IsReleased reports %d"),
				Index, NameOfSupport(ExpectedSupport[Index]),
				ExpectedReleased[Index] ? TEXT("RELEASED") : TEXT("left kinematic"),
				Binding.IsReleased(Index) ? 1 : 0),
			Binding.IsReleased(Index) == ExpectedReleased[Index]);

		// The rule against the solver's own answer, so a wrong table row is caught too.
		const EPieceSupport State = Binding.GetStructure().GetPieceSupport(Index);
		const bool bHeldUp = State == EPieceSupport::Grounded || State == EPieceSupport::Supported;

		TestTrue(
			FString::Printf(TEXT("piece %d reads %s and IsReleased %d — release must be exactly 'not held up'"),
				Index, NameOfSupport(State), Binding.IsReleased(Index) ? 1 : 0),
			Binding.IsReleased(Index) == !bHeldUp);

		TestTrue(
			FString::Printf(TEXT("releasing piece %d must not remove it from the graph"), Index),
			!Binding.IsPieceRemoved(Index));
	}

	// A removed piece is never released: its actor was cleared on removal.
	{
		FStructureBinding Removed;
		TArray<UObject*> RemovedStandIns;

		BuildBinding(
			Removed, 7,
			{ { BrickMassKg, true }, { BrickMassKg, false } },
			{ { 0, 1, BedJointNormal } },
			RemovedStandIns);

		TestTrue(TEXT("the piece resting on the pad should remove cleanly"), Removed.RemovePiece(1));

		Removed.SolveLoads();

		const int32 RemovedRelease = Removed.ApplyResults();

		TestTrue(
			FString::Printf(TEXT("a removed piece has no actor to release, so the push should report 0, got %d"),
				RemovedRelease),
			RemovedRelease == 0);

		TestTrue(TEXT("a removed piece must not come back reported as released"),
			!Removed.IsReleased(1));

		ReleaseStandIns(RemovedStandIns);
	}

	ReleaseStandIns(StandIns);

	return true;
}

/**
 * Release is one way: a second push neither re-releases nor re-freezes a fallen brick. Re-freezing
 * is the danger: Chaos has moved the brick and this layer does not know where it went. The solver
 * genuinely changes its mind here:
 *
 *      [ G ] — X          pieces 0 and 2, head-jointed
 *      =====    |
 *               F         piece 1, floating beneath X on a bed joint
 *
 * The bed joint outranks the head joint, so X rests only on F and both are Falling and released.
 * Remove F and X falls back to its head joint and reads Supported.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureBindingReleaseIsOneWayTest,
	"DestructionGame.Core.StructureBinding.ReleaseIsOneWay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureBindingReleaseIsOneWayTest::RunTest(const FString& Parameters)
{
	using namespace StructureBindingTestSupport;

	const TArray<FPieceSpec> PieceSpecs = {
		{ BrickMassKg, true },  // 0: G, on the earth
		{ BrickMassKg, false }, // 1: F, floating beneath X, REMOVED later
		{ BrickMassKg, false }  // 2: X
	};

	const TArray<FConnectionSpec> ConnectionSpecs = {
		{ 1, 2, BedJointNormal },  // F beneath X: outranks the head joint while it is there
		{ 0, 2, HeadJointNormal }  // G beside X: X's fallback once the bed joint has gone
	};

	FStructureBinding Binding;
	TArray<UObject*> StandIns;
	BuildBinding(Binding, 7, PieceSpecs, ConnectionSpecs, StandIns);

	CheckArraysAreParallel(*this, Binding, PieceSpecs.Num(), TEXT("as built"));

	Binding.SolveLoads();

	// Fixture precondition.
	TestTrue(
		FString::Printf(TEXT("fixture: X should be Falling while the floating piece beneath it is its only support, got %s"),
			NameOfSupport(Binding.GetStructure().GetPieceSupport(2))),
		Binding.GetStructure().GetPieceSupport(2) == EPieceSupport::Falling);

	const int32 FirstRelease = Binding.ApplyResults();

	TestTrue(
		FString::Printf(TEXT("the first push should release the two pieces nothing holds up, got %d"),
			FirstRelease),
		FirstRelease == 2);

	TestTrue(TEXT("X should have been released by the first push"), Binding.IsReleased(2));
	TestTrue(TEXT("F should have been released by the first push"), Binding.IsReleased(1));
	TestTrue(TEXT("the grounded pier must not have been released"), !Binding.IsReleased(0));

	// A second push on an unchanged structure is a no-op; only the count shows it.
	const int32 SecondRelease = Binding.ApplyResults();

	TestTrue(
		FString::Printf(TEXT("pushing again with nothing changed must release nothing, got %d"),
			SecondRelease),
		SecondRelease == 0);

	TestTrue(TEXT("X must still be released after the second push"), Binding.IsReleased(2));

	TestTrue(TEXT("removing the floating piece should report that it removed one"),
		Binding.RemovePiece(1));

	Binding.SolveLoads();

	TestTrue(
		FString::Printf(TEXT("fixture: with the bed joint gone X should fall back to its head joint and read Supported, got %s"),
			NameOfSupport(Binding.GetStructure().GetPieceSupport(2))),
		Binding.GetStructure().GetPieceSupport(2) == EPieceSupport::Supported);

	const int32 ThirdRelease = Binding.ApplyResults();

	// The solver now holds X up, but X has fallen: bReleased is a latch, so X stays released.
	TestTrue(
		FString::Printf(TEXT("a push after a piece has fallen must release nothing, got %d"),
			ThirdRelease),
		ThirdRelease == 0);

	TestTrue(
		FString::Printf(TEXT("X has fallen and the solver now calls it %s — it must STAY released, IsReleased reports %d"),
			NameOfSupport(Binding.GetStructure().GetPieceSupport(2)),
			Binding.IsReleased(2) ? 1 : 0),
		Binding.IsReleased(2));

	CheckArraysAreParallel(*this, Binding, PieceSpecs.Num(), TEXT("after the removal"));

	ReleaseStandIns(StandIns);

	return true;
}

/**
 * A piece is released only if the last solve computed a support state for it. Falling is the
 * solver's fail-closed default, but at this seam it triggers an irreversible release, so the
 * default becomes fail-open (DESIGN.md §2). Handles at or past the last solve's extent have no
 * answer.
 *
 * Row 1: nothing solved, so every handle, the foundation included, reads Falling. Row 2: a piece
 * added after the last solve. A "have we ever solved?" guard closes row 1 only. bReleased latches,
 * so a wrong release is permanent.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureBindingReleaseNeedsASolveTest,
	"DestructionGame.Core.StructureBinding.ReleaseNeedsASolve",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureBindingReleaseNeedsASolveTest::RunTest(const FString& Parameters)
{
	using namespace StructureBindingTestSupport;

	/*
	 *   [ brick 1 ]
	 *   [  pad 0  ]      the smallest structure that stands
	 *   ==========
	 */
	const TArray<FPieceSpec> PieceSpecs = {
		{ BrickMassKg, true },  // 0: pad, on the earth
		{ BrickMassKg, false }  // 1: brick, resting on the pad
	};

	const TArray<FConnectionSpec> ConnectionSpecs = {
		{ 0, 1, BedJointNormal }
	};

	// ROW 1: nothing has been solved, so nothing may be released.
	{
		FStructureBinding Binding;
		TArray<UObject*> StandIns;
		BuildBinding(Binding, 7, PieceSpecs, ConnectionSpecs, StandIns);

		CheckArraysAreParallel(*this, Binding, PieceSpecs.Num(), TEXT("as built"));

		// Precondition: before a solve every handle reads Falling, the grounded pad included.
		for (int32 Index = 0; Index < Binding.NumPieces(); ++Index)
		{
			TestTrue(
				FString::Printf(
					TEXT("row 1 fixture: before any solve piece %d has no answer and must read Falling, got %s"),
					Index, NameOfSupport(Binding.GetStructure().GetPieceSupport(Index))),
				Binding.GetStructure().GetPieceSupport(Index) == EPieceSupport::Falling);
		}

		const int32 Released = Binding.ApplyResults();

		TestTrue(
			FString::Printf(
				TEXT("row 1: pushing before anything has been solved must release nothing, got %d"),
				Released),
			Released == 0);

		TestTrue(
			FString::Printf(
				TEXT("row 1: the GROUNDED pad must not be released by a push that had nothing to read, IsReleased reports %d"),
				Binding.IsReleased(0) ? 1 : 0),
			!Binding.IsReleased(0));

		TestTrue(
			FString::Printf(
				TEXT("row 1: the brick must not be released by a push that had nothing to read, IsReleased reports %d"),
				Binding.IsReleased(1) ? 1 : 0),
			!Binding.IsReleased(1));

		ReleaseStandIns(StandIns);
	}

	// ROW 2: a piece added AFTER the last solve is past the answer's extent.
	{
		FStructureBinding Binding;
		TArray<UObject*> StandIns;
		BuildBinding(Binding, 7, PieceSpecs, ConnectionSpecs, StandIns);

		Binding.SolveLoads();

		TestTrue(
			FString::Printf(TEXT("row 2 fixture: the pad should solve as Grounded, got %s"),
				NameOfSupport(Binding.GetStructure().GetPieceSupport(0))),
			Binding.GetStructure().GetPieceSupport(0) == EPieceSupport::Grounded);

		TestTrue(
			FString::Printf(TEXT("row 2 fixture: the brick should solve as Supported, got %s"),
				NameOfSupport(Binding.GetStructure().GetPieceSupport(1))),
			Binding.GetStructure().GetPieceSupport(1) == EPieceSupport::Supported);

		const int32 SettledRelease = Binding.ApplyResults();

		TestTrue(
			FString::Printf(TEXT("row 2: a solved, standing structure must release nothing, got %d"),
				SettledRelease),
			SettledRelease == 0);

		// Lay a brick without re-solving: its handle is past the last solve's extent.
		UObject* Placed = MakeStandIn();
		StandIns.Add(Placed);

		const int32 PlacedHandle = Binding.AddPiece(BrickMassKg, false, Placed, BoxFor(2));

		TestTrue(
			FString::Printf(TEXT("row 2 fixture: the laid brick should take handle 2, got %d"), PlacedHandle),
			PlacedHandle == 2);

		{
			FConnection Connection;
			Connection.PieceA = 1;
			Connection.PieceB = 2;
			Connection.InterfaceNormal = BedJointNormal;
			Connection.InterfaceAreaSqCm = JointAreaSqCm;
			Connection.Strength = Unbreakable;

			TestTrue(TEXT("row 2 fixture: the bed joint under the laid brick should be accepted"),
				Binding.AddConnection(Connection) != INDEX_NONE);
		}

		CheckArraysAreParallel(*this, Binding, 3, TEXT("after the brick was laid"));

		TestTrue(
			FString::Printf(
				TEXT("row 2 fixture: handle 2 is past the last solve's extent of 2 pieces, so it reads %s with no answer behind it"),
				NameOfSupport(Binding.GetStructure().GetPieceSupport(2))),
			Binding.GetStructure().GetPieceSupport(2) == EPieceSupport::Falling);

		const int32 PlacedRelease = Binding.ApplyResults();

		TestTrue(
			FString::Printf(
				TEXT("row 2: a piece added since the last solve has no answer, so the push must release nothing, got %d"),
				PlacedRelease),
			PlacedRelease == 0);

		TestTrue(
			FString::Printf(
				TEXT("row 2: the newly laid brick must not go dynamic before anything solved it, IsReleased reports %d"),
				Binding.IsReleased(2) ? 1 : 0),
			!Binding.IsReleased(2));

		for (int32 Index = 0; Index < 2; ++Index)
		{
			TestTrue(
				FString::Printf(TEXT("row 2: the standing piece %d must be unaffected by the placement, IsReleased reports %d"),
					Index, Binding.IsReleased(Index) ? 1 : 0),
				!Binding.IsReleased(Index));
		}

		// After a solve the brick is Supported; a wrong earlier release would still be latched.
		Binding.SolveLoads();

		TestTrue(
			FString::Printf(TEXT("row 2 fixture: once solved, the laid brick rests on the wall and should read Supported, got %s"),
				NameOfSupport(Binding.GetStructure().GetPieceSupport(2))),
			Binding.GetStructure().GetPieceSupport(2) == EPieceSupport::Supported);

		const int32 AfterSolveRelease = Binding.ApplyResults();

		TestTrue(
			FString::Printf(TEXT("row 2: the solved structure stands, so this push must release nothing either, got %d"),
				AfterSolveRelease),
			AfterSolveRelease == 0);

		TestTrue(
			FString::Printf(
				TEXT("row 2: the laid brick is Supported and must still be kinematic — a release latched earlier is permanent, IsReleased reports %d"),
				Binding.IsReleased(2) ? 1 : 0),
			!Binding.IsReleased(2));

		ReleaseStandIns(StandIns);
	}

	return true;
}

/**
 * A piece ref fails closed: a stale, foreign or out-of-range {StructureId, PieceIndex} resolves to
 * nothing. Stale refs are normal (a removed slot stays a valid index), so a bounds check alone is
 * not enough.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureBindingPieceRefFailsClosedTest,
	"DestructionGame.Core.StructureBinding.PieceRefFailsClosed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureBindingPieceRefFailsClosedTest::RunTest(const FString& Parameters)
{
	using namespace StructureBindingTestSupport;

	constexpr int32 ThisStructure = 7;
	constexpr int32 SomeOtherStructure = 8;

	const TArray<FPieceSpec> PieceSpecs = {
		{ BrickMassKg, true },  // 0: pad
		{ BrickMassKg, false }, // 1: brick on the pad
		{ BrickMassKg, false }  // 2: REMOVED below
	};

	const TArray<FConnectionSpec> ConnectionSpecs = {
		{ 0, 1, BedJointNormal },
		{ 1, 2, BedJointNormal }
	};

	FStructureBinding Binding;
	TArray<UObject*> StandIns;
	BuildBinding(Binding, ThisStructure, PieceSpecs, ConnectionSpecs, StandIns);

	CheckArraysAreParallel(*this, Binding, PieceSpecs.Num(), TEXT("as built"));

	TestTrue(TEXT("the top brick should remove cleanly"), Binding.RemovePiece(2));

	struct FRefCase
	{
		const TCHAR* Description;
		FPieceRef Ref;
		int32 Expected;
	};

	const TArray<FRefCase> Cases = {
		// Accepting rows first, so an always-INDEX_NONE stub fails.
		{ TEXT("a live piece of this structure resolves"), { ThisStructure, 0 }, 0 },
		{ TEXT("and so does the one above it"), { ThisStructure, 1 }, 1 },

		// A tombstoned slot is still a valid index; a bounds check alone would accept it.
		{ TEXT("a piece that has been removed does not resolve"), { ThisStructure, 2 }, INDEX_NONE },

		{ TEXT("a negative index does not resolve"), { ThisStructure, -1 }, INDEX_NONE },
		{ TEXT("INDEX_NONE as an index does not resolve"), { ThisStructure, INDEX_NONE }, INDEX_NONE },
		{ TEXT("one past the end does not resolve"), { ThisStructure, 3 }, INDEX_NONE },
		{ TEXT("a wildly out-of-range index does not resolve"), { ThisStructure, MAX_int32 }, INDEX_NONE },
		{ TEXT("MIN_int32 does not resolve, and must not be negated into range"), { ThisStructure, MIN_int32 }, INDEX_NONE },

		// A ref from another structure: what StructureId exists to catch.
		{ TEXT("a valid index belonging to a DIFFERENT structure does not resolve"), { SomeOtherStructure, 0 }, INDEX_NONE },

		// A default ref matches nothing, so an unidentified actor cannot resolve to piece zero.
		{ TEXT("a ref with no structure id does not resolve"), { INDEX_NONE, 0 }, INDEX_NONE },
		{ TEXT("a wholly default ref does not resolve"), { }, INDEX_NONE },
	};

	for (const FRefCase& Case : Cases)
	{
		const int32 Resolved = Binding.ResolvePiece(Case.Ref);

		TestTrue(
			FString::Printf(TEXT("%s: {%d, %d} should resolve to %d, got %d"),
				Case.Description, Case.Ref.StructureId, Case.Ref.PieceIndex,
				Case.Expected, Resolved),
			Resolved == Case.Expected);

		// The result feeds GetActor directly: right stand-in if accepted, null if rejected.
		UObject* const Expected = StandIns.IsValidIndex(Case.Expected) ? StandIns[Case.Expected] : nullptr;

		TestTrue(
			FString::Printf(TEXT("%s: the resolved handle %d should hand back %s, got %s"),
				Case.Description, Resolved, *GetNameSafe(Expected), *GetNameSafe(Binding.GetActor(Resolved))),
			Binding.GetActor(Resolved) == Expected);
	}

	/*
	 * IsReleased fails closed to false on unknown handles, including the removed slot. Other tests
	 * show it can return true, so a stub cannot pass these rows.
	 */
	{
		struct FUnknownHandleCase
		{
			const TCHAR* Description;
			int32 Handle;
		};

		const TArray<FUnknownHandleCase> UnknownHandles = {
			{ TEXT("INDEX_NONE"), INDEX_NONE },
			{ TEXT("a negative handle"), -7 },
			{ TEXT("one past the last piece"), 3 },
			{ TEXT("a wildly out-of-range handle"), MAX_int32 },
			{ TEXT("MIN_int32"), MIN_int32 },
			{ TEXT("the removed piece, whose slot is still a valid index"), 2 },
		};

		for (const FUnknownHandleCase& Case : UnknownHandles)
		{
			TestTrue(
				FString::Printf(
					TEXT("%s (%d) names no live piece, so it cannot have been released, IsReleased reports %d"),
					Case.Description, Case.Handle, Binding.IsReleased(Case.Handle) ? 1 : 0),
				!Binding.IsReleased(Case.Handle));
		}
	}

	// An unidentified binding matches nothing, even a ref that is also INDEX_NONE.
	{
		FStructureBinding Anonymous;
		TArray<UObject*> AnonymousStandIns;

		BuildBinding(
			Anonymous, INDEX_NONE,
			{ { BrickMassKg, true }, { BrickMassKg, false } },
			{ { 0, 1, BedJointNormal } },
			AnonymousStandIns);

		const TArray<FPieceRef> MustAllFail = {
			{ INDEX_NONE, 0 },
			{ INDEX_NONE, 1 },
			{ ThisStructure, 0 },
			{ }
		};

		for (const FPieceRef& Ref : MustAllFail)
		{
			TestTrue(
				FString::Printf(TEXT("an unidentified binding must resolve nothing, but {%d, %d} came back as %d"),
					Ref.StructureId, Ref.PieceIndex, Anonymous.ResolvePiece(Ref)),
				Anonymous.ResolvePiece(Ref) == INDEX_NONE);
		}

		ReleaseStandIns(AnonymousStandIns);
	}

	ReleaseStandIns(StandIns);

	return true;
}

/**
 * AdoptLayout brings a built wall into a binding with parallel arrays, the one sanctioned way to do
 * it. It replays the layout, correct only because layouts are append-only (handle i of the layout is
 * handle i of the binding).
 *
 * Asserted per handle (mass, grounded, box, actor), not by count: a reversed actor list or an
 * off-by-one box index keeps every count right.
 *
 * It refuses a layout whose arrays are already out of step (an infinite brick dimension makes
 * AddPiece refuse the mass while the box is still appended), and a refusal writes nothing.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureBindingAdoptLayoutTest,
	"DestructionGame.Core.StructureBinding.AdoptLayout",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureBindingAdoptLayoutTest::RunTest(const FString& Parameters)
{
	using namespace StructureBindingTestSupport;
	using namespace DestructionLayout;

	// A flush 3 x 3 wall: 10 pieces with half bats, so there are two masses to tell apart.
	FRunningBondSpec Spec;
	Spec.BrickSizeCm = FVector(21.5, 10.25, 6.5);
	Spec.JointThicknessCm = 1.0;
	Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
	Spec.CoursesHigh = 3;
	Spec.BricksPerCourse = 3;
	Spec.End = EWallEnd::Flush;
	Spec.Strength = GeneralPurposeMortar;

	FBrickLayout Layout;

	TestTrue(TEXT("fixture: RunningBond should lay the wall"), RunningBond(Spec, Layout));

	// Floor the fixture: a tiny or empty layout would pass vacuously.
	TestEqual(
		FString::Printf(TEXT("fixture: a flush 3 x 3 wall should be 10 pieces, got %d"),
			Layout.Structure.NumPieces()),
		Layout.Structure.NumPieces(), 10);

	TestEqual(
		FString::Printf(TEXT("fixture: the layout must carry one box per piece, got %d boxes"),
			Layout.Boxes.Num()),
		Layout.Boxes.Num(), Layout.Structure.NumPieces());

	TestTrue(
		FString::Printf(TEXT("fixture: the wall should have joints to replay, got %d"),
			Layout.Structure.NumConnections()),
		Layout.Structure.NumConnections() >= 8);

	{
		TArray<double> MassesSeen;
		int32 GroundedPieces = 0;

		for (int32 Piece = 0; Piece < Layout.Structure.NumPieces(); ++Piece)
		{
			MassesSeen.AddUnique(Layout.Structure.GetPiece(Piece).MassKg);

			if (Layout.Structure.GetPiece(Piece).bIsGrounded)
			{
				++GroundedPieces;
			}
		}

		// Both columns must vary, or a replay that hard-coded either would pass.
		TestEqual(
			FString::Printf(TEXT("fixture: the flush wall should hold two distinct masses, got %d"),
				MassesSeen.Num()),
			MassesSeen.Num(), 2);

		TestEqual(
			FString::Printf(TEXT("fixture: only the bottom course of 3 should be grounded, got %d"),
				GroundedPieces),
			GroundedPieces, 3);
	}

	TArray<UObject*> StandIns;
	for (int32 Piece = 0; Piece < Layout.Structure.NumPieces(); ++Piece)
	{
		StandIns.Add(MakeStandIn());
	}

	FStructureBinding Binding;
	Binding.StructureId = 7;

	const bool bAdopted = AdoptLayout(Layout, StandIns, Binding);

	TestTrue(TEXT("AdoptLayout should adopt a well-formed layout"), bAdopted);

	TestEqual(
		FString::Printf(TEXT("adoption must not change the binding's structure id, got %d"),
			Binding.StructureId),
		Binding.StructureId, 7);

	CheckArraysAreParallel(*this, Binding, Layout.Structure.NumPieces(), TEXT("after adoption"));

	TestEqual(
		FString::Printf(
			TEXT("the binding should carry all %d of the layout's joints, got %d"),
			Layout.Structure.NumConnections(), Binding.GetStructure().NumConnections()),
		Binding.GetStructure().NumConnections(), Layout.Structure.NumConnections());

	// Per handle, all four columns: where a shifted array shows up.
	for (int32 Piece = 0; Piece < Layout.Structure.NumPieces(); ++Piece)
	{
		const FStructurePiece& Original = Layout.Structure.GetPiece(Piece);
		const FStructurePiece& Adopted = Binding.GetStructure().GetPiece(Piece);

		TestTrue(
			FString::Printf(
				TEXT("piece %d should weigh what the layout laid, %.17g kg, got %.17g"),
				Piece, Original.MassKg, Adopted.MassKg),
			Adopted.MassKg == Original.MassKg);

		TestTrue(
			FString::Printf(
				TEXT("piece %d should%s be grounded, as the layout laid it"),
				Piece, Original.bIsGrounded ? TEXT("") : TEXT(" NOT")),
			Adopted.bIsGrounded == Original.bIsGrounded);

		TestTrue(
			FString::Printf(
				TEXT("piece %d should carry the layout's own box, centre (%g, %g, %g) extent")
				TEXT(" (%g, %g, %g); got centre (%g, %g, %g) extent (%g, %g, %g)"),
				Piece,
				Layout.Boxes[Piece].CentreCm.X, Layout.Boxes[Piece].CentreCm.Y,
				Layout.Boxes[Piece].CentreCm.Z,
				Layout.Boxes[Piece].ExtentCm.X, Layout.Boxes[Piece].ExtentCm.Y,
				Layout.Boxes[Piece].ExtentCm.Z,
				Binding.GetBinding(Piece).Box.CentreCm.X, Binding.GetBinding(Piece).Box.CentreCm.Y,
				Binding.GetBinding(Piece).Box.CentreCm.Z,
				Binding.GetBinding(Piece).Box.ExtentCm.X, Binding.GetBinding(Piece).Box.ExtentCm.Y,
				Binding.GetBinding(Piece).Box.ExtentCm.Z),
			BoxesMatch(Binding.GetBinding(Piece).Box, Layout.Boxes[Piece]));

		TestTrue(
			FString::Printf(
				TEXT("piece %d should be bound to its own stand-in %s, got %s"),
				Piece, *GetNameSafe(StandIns[Piece]), *GetNameSafe(Binding.GetActor(Piece))),
			Binding.GetActor(Piece) == StandIns[Piece]);

		TestTrue(
			FString::Printf(TEXT("an adopted piece %d is in the graph, not a tombstone"), Piece),
			!Binding.IsPieceRemoved(Piece));

		TestTrue(
			FString::Printf(TEXT("an adopted piece %d has not been handed to physics"), Piece),
			!Binding.IsReleased(Piece));
	}

	// Every joint, whole: handles, normal, area and profile, all exact.
	for (int32 Joint = 0; Joint < Layout.Structure.NumConnections(); ++Joint)
	{
		const FConnection& Original = Layout.Structure.GetConnection(Joint);
		const FConnection& Adopted = Binding.GetStructure().GetConnection(Joint);

		TestTrue(
			FString::Printf(
				TEXT("joint %d should join the same pieces, %d-%d, got %d-%d"),
				Joint, Original.PieceA, Original.PieceB, Adopted.PieceA, Adopted.PieceB),
			Adopted.PieceA == Original.PieceA && Adopted.PieceB == Original.PieceB);

		// Compared exactly: GetJointRole reorients the normal, so a flipped one would not show in the loads.
		TestTrue(
			FString::Printf(
				TEXT("joint %d normal should be exactly (%g, %g, %g), got (%g, %g, %g)"),
				Joint,
				Original.InterfaceNormal.X, Original.InterfaceNormal.Y, Original.InterfaceNormal.Z,
				Adopted.InterfaceNormal.X, Adopted.InterfaceNormal.Y, Adopted.InterfaceNormal.Z),
			Adopted.InterfaceNormal == Original.InterfaceNormal);

		TestTrue(
			FString::Printf(
				TEXT("joint %d area should be exactly %.17g cm2, got %.17g"),
				Joint, Original.InterfaceAreaSqCm, Adopted.InterfaceAreaSqCm),
			Adopted.InterfaceAreaSqCm == Original.InterfaceAreaSqCm);

		TestTrue(
			FString::Printf(
				TEXT("joint %d should keep its strength profile, %.17g / %.17g / %.17g MPa"),
				Joint, Original.Strength.CompressiveStrengthMPa,
				Original.Strength.ShearCohesionMPa, Original.Strength.TensileStrengthMPa),
			Adopted.Strength.CompressiveStrengthMPa == Original.Strength.CompressiveStrengthMPa
				&& Adopted.Strength.ShearCohesionMPa == Original.Strength.ShearCohesionMPa
				&& Adopted.Strength.TensileStrengthMPa == Original.Strength.TensileStrengthMPa
				&& Adopted.Strength.FrictionCoefficient == Original.Strength.FrictionCoefficient
				&& Adopted.Strength.MaxShearStrengthMPa == Original.Strength.MaxShearStrengthMPa);
	}

	// The adopted graph solves bit-for-bit the same as the layout (solving is deterministic).
	Layout.Structure.SolveLoads();
	Binding.SolveLoads();

	for (int32 Piece = 0; Piece < Layout.Structure.NumPieces(); ++Piece)
	{
		TestTrue(
			FString::Printf(
				TEXT("solved piece %d should read %s as it does in the layout, got %s"),
				Piece,
				NameOfSupport(Layout.Structure.GetPieceSupport(Piece)),
				NameOfSupport(Binding.GetStructure().GetPieceSupport(Piece))),
			Binding.GetStructure().GetPieceSupport(Piece)
				== Layout.Structure.GetPieceSupport(Piece));

		TestTrue(
			FString::Printf(TEXT("fixture: the adopted wall should stand — piece %d must be held up"), Piece),
			Binding.GetStructure().IsPieceSupported(Piece));
	}

	for (int32 Joint = 0; Joint < Layout.Structure.NumConnections(); ++Joint)
	{
		const FVector Expected = Layout.Structure.GetConnectionForce(Joint);
		const FVector Actual = Binding.GetStructure().GetConnectionForce(Joint);

		TestTrue(
			FString::Printf(
				TEXT("solved joint %d should carry (%.10g, %.10g, %.10g) as it does in the layout,")
				TEXT(" got (%.10g, %.10g, %.10g)"),
				Joint, Expected.X, Expected.Y, Expected.Z, Actual.X, Actual.Y, Actual.Z),
			Actual == Expected);
	}

	ReleaseStandIns(StandIns);

	// Refusals must write nothing: a half-adopted binding would look like a real wall.
	{
		auto CheckRefused =
			[this](const TCHAR* Description, const FBrickLayout& Refused, TArrayView<UObject* const> Actors)
		{
			FStructureBinding Fresh;
			Fresh.StructureId = 11;

			const bool bTaken = AdoptLayout(Refused, Actors, Fresh);

			TestFalse(
				FString::Printf(TEXT("%s: AdoptLayout should refuse it"), Description),
				bTaken);

			TestEqual(
				FString::Printf(TEXT("%s: a refused adoption must leave no bindings behind, got %d"),
					Description, Fresh.NumPieces()),
				Fresh.NumPieces(), 0);

			TestEqual(
				FString::Printf(TEXT("%s: a refused adoption must leave no pieces behind, got %d"),
					Description, Fresh.GetStructure().NumPieces()),
				Fresh.GetStructure().NumPieces(), 0);

			TestEqual(
				FString::Printf(TEXT("%s: a refused adoption must leave no joints behind, got %d"),
					Description, Fresh.GetStructure().NumConnections()),
				Fresh.GetStructure().NumConnections(), 0);
		};

		TArray<UObject*> Spares;
		for (int32 Index = 0; Index < 12; ++Index)
		{
			Spares.Add(MakeStandIn());
		}

		/*
		 * The desync RunningBond can produce, built by hand: the real route (an infinite brick
		 * dimension) also indexes Boxes[INDEX_NONE] and aborts the run.
		 */
		{
			FBrickLayout Extra;
			TestTrue(TEXT("fixture: the desync wall should be laid"), RunningBond(Spec, Extra));

			// Copied first: TArray::Add asserts on an argument aliasing its own storage.
			const FPieceBox Duplicate = Extra.Boxes[0];
			Extra.Boxes.Add(Duplicate);

			CheckRefused(
				TEXT("a layout with one more box than pieces"),
				Extra,
				TArrayView<UObject* const>(Spares.GetData(), Extra.Boxes.Num()));
		}

		// The same desync the other way round: a piece nothing has a box for.
		{
			FBrickLayout Extra;
			TestTrue(TEXT("fixture: the second desync wall should be laid"), RunningBond(Spec, Extra));

			TestTrue(
				TEXT("fixture: the extra piece should be accepted by the structure"),
				Extra.Structure.AddPiece(2.72163125, /*bIsGrounded*/ false) != INDEX_NONE);

			CheckRefused(
				TEXT("a layout with one more piece than boxes"),
				Extra,
				TArrayView<UObject* const>(Spares.GetData(), Extra.Structure.NumPieces()));
		}

		// The actor list is the third parallel array; too few or too many are both refused.
		{
			FBrickLayout Good;
			TestTrue(TEXT("fixture: the short-actor wall should be laid"), RunningBond(Spec, Good));

			CheckRefused(
				TEXT("fewer actors than pieces"),
				Good,
				TArrayView<UObject* const>(Spares.GetData(), Good.Structure.NumPieces() - 1));

			CheckRefused(
				TEXT("more actors than pieces"),
				Good,
				TArrayView<UObject* const>(Spares.GetData(), Good.Structure.NumPieces() + 1));

			CheckRefused(
				TEXT("no actors at all"),
				Good,
				TArrayView<UObject* const>());
		}

		// An empty layout is not a wall; RunningBond never produces one either.
		{
			FBrickLayout Nothing;

			CheckRefused(
				TEXT("an empty layout"),
				Nothing,
				TArrayView<UObject* const>());
		}

		ReleaseStandIns(Spares);
	}

	return true;
}

/**
 * A piece the structure refuses (negative or non-finite mass) must not land in the binding either,
 * or the arrays desync permanently. A good piece is added first so the guard cannot pass by
 * refusing everything.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureBindingAddPieceHonoursRefusalTest,
	"DestructionGame.Core.StructureBinding.AddPieceHonoursRefusal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureBindingAddPieceHonoursRefusalTest::RunTest(const FString& Parameters)
{
	using namespace StructureBindingTestSupport;

	FStructureBinding Binding;
	Binding.StructureId = 3;

	TArray<UObject*> StandIns;

	// One good piece first, handle 0 throughout.
	UObject* const Good = MakeStandIn();
	StandIns.Add(Good);

	const int32 GoodHandle = Binding.AddPiece(BrickMassKg, /*bIsGrounded*/ true, Good, BoxFor(0));

	TestEqual(
		FString::Printf(TEXT("a well-formed piece should take handle 0, got %d"), GoodHandle),
		GoodHandle, 0);

	CheckArraysAreParallel(*this, Binding, 1, TEXT("after one good piece"));

	// Each refused mass uses a fresh stand-in, so a mistaken append shows as an extra handle.
	struct FRefusedCase
	{
		const TCHAR* Description;
		double MassKg;
	};

	const TArray<FRefusedCase> Refused = {
		{ TEXT("a NaN mass"), std::numeric_limits<double>::quiet_NaN() },
		{ TEXT("an infinite mass"), std::numeric_limits<double>::infinity() },
		{ TEXT("a negative mass"), -BrickMassKg },
	};

	for (const FRefusedCase& Case : Refused)
	{
		UObject* const Reject = MakeStandIn();
		StandIns.Add(Reject);

		const int32 Handle = Binding.AddPiece(Case.MassKg, /*bIsGrounded*/ false, Reject, BoxFor(9));

		TestEqual(
			FString::Printf(
				TEXT("%s must be refused: AddPiece should relay INDEX_NONE, got %d"),
				Case.Description, Handle),
			Handle, static_cast<int32>(INDEX_NONE));

		CheckArraysAreParallel(*this, Binding, 1,
			*FString::Printf(TEXT("after refusing %s"), Case.Description));
	}

	TestTrue(
		TEXT("handle 0 must still be the good stand-in after every refusal"),
		Binding.GetActor(0) == Good);

	ReleaseStandIns(StandIns);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
