// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/StructureBinding.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Components/StaticMeshComponent.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include <type_traits>
#include <utility>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, not anonymous, and named differently from every other one in this
 * directory. An anonymous namespace is private to a TRANSLATION UNIT rather than to a
 * file, and a unity build merges many files into one — at which point two anonymous
 * BedJointNormal declarations are the same declaration twice. See CURRENT_STATE.md;
 * the `using namespace` for this one lives inside each RunTest body for the same
 * reason.
 */
namespace StructureBindingTestSupport
{
	using namespace DestructionProfiles;

	/** A standard UK metric brick, 215 x 102.5 x 65 mm, in cm3. */
	constexpr double BrickVolumeCubicCm = 21.5 * 10.25 * 6.5;

	/**
	 * A standard clay brick, derived rather than hand-set.
	 *
	 * NO EXPECTATION IN THIS FILE DEPENDS ON THE VALUE. Every joint below is the
	 * Unbreakable test fixture and every assertion is about support state, actor
	 * identity or the release latch — never about a force or a utilisation. A real
	 * number is simply less distracting than zero, and deriving it keeps this file
	 * from becoming a second place for the figure to drift, as
	 * StructureRemovalTest.cpp already records.
	 */
	const double BrickMassKg = ClayBrick.DensityGramsPerCubicCm * BrickVolumeCubicCm / 1000.0;

	/** Horizontal interface, normal pointing up at the piece above: a bed joint. */
	const FVector BedJointNormal(0.0, 0.0, 1.0);

	/** Vertical interface, normal pointing sideways at the neighbour: a head joint. */
	const FVector HeadJointNormal(1.0, 0.0, 0.0);

	constexpr double JointAreaSqCm = 100.0;

	/**
	 * A stand-in for a brick actor.
	 *
	 * A TRANSIENT-PACKAGE UObject IS ENOUGH, and spinning up a world to get a real
	 * AActor would be the wrong shape entirely: this slice is world-free, so a test for
	 * it that needed a ticking world would be testing the next slice by accident. All
	 * the binding does with an actor is hold it, hand it back and let it die, and any
	 * UObject does all three exactly as an actor does.
	 *
	 * NOT `NewObject<UObject>`, WHICH DOES NOT WORK: UObject itself is flagged abstract,
	 * so instantiating it directly trips an engine ensure ("Class which was marked
	 * abstract was trying to be loaded in Outer /Engine/Transient") and the automation
	 * framework counts that ensure as a test error. UStaticMeshComponent is the nearest
	 * concrete thing a brick will genuinely own, needs no world, and constructing one
	 * against the transient package is exactly what runtime component creation does.
	 *
	 * ROOTED, so the only thing that can collect a stand-in is a test that asks for it.
	 * The GC case below deliberately creates an unrooted one instead.
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

	/**
	 * Build a binding, one stand-in and one box per piece, and hand the stand-ins back
	 * so the caller can assert identity against them and un-root them afterwards.
	 */
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
	 * THE INVARIANT THE WHOLE TYPE EXISTS FOR, asserted after every operation that could
	 * break it rather than once at the end.
	 *
	 * The binding array and the piece array are index-parallel or the type is worthless:
	 * one entry out of step and every handle above the hole resolves to the wrong brick,
	 * silently. Handles are array indices into both, and neither ever compacts.
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
 * A MUTABLE BINDING MUST NOT HAND OUT A MUTABLE STRUCTURE, and this is checked at
 * COMPILE TIME because that is the only place it can be checked: the failure mode is
 * somebody calling Binding.GetStructure().RemovePiece(2) directly, which removes the
 * piece and leaves the binding's actor pointing at a brick that is no longer in the
 * graph. No runtime assertion can catch code that was never written; a static_assert
 * stops the accessor that would allow it from existing.
 *
 * decltype on a non-const lvalue picks the non-const overload if there is one, so
 * adding `FStructure& GetStructure()` alongside the const one fails here rather than
 * quietly reopening the hole.
 */
static_assert(
	std::is_same_v<
		decltype(std::declval<FStructureBinding&>().GetStructure()),
		const FStructure&>,
	"FStructureBinding::GetStructure must return a CONST reference even from a mutable "
	"binding: a mutable FStructure& lets a caller call RemovePiece on the graph without "
	"the binding array hearing about it, and the resulting desync is silent.");

/**
 * REMOVAL CANNOT DANGLE: one call takes the piece out of the graph and clears its
 * actor, and every other handle still resolves to the same stand-in it always did.
 *
 * WHY REMOVAL IS THE FIRST TEST OF THIS SLICE. FStructure already tombstones a removed
 * slot so handles stay stable, and the binding array is parallel to that array — so the
 * one way to get this wrong is to remove from one and not the other, or to remove from
 * one by compacting. Both produce a binding array off by one above the hole, at which
 * point the player shoots brick 5 and brick 6 falls. Nothing crashes and nothing logs.
 *
 * THE REMOVAL IS FROM THE MIDDLE, which is the only place that can prove anything:
 * removing the last piece is indistinguishable from compacting it away. The stand-ins
 * are distinct objects and the boxes are distinct values, so a shifted array reads back
 * the wrong one rather than an identical one.
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

	/* The positive control: every handle resolves to its own stand-in before anything. */
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

	/*
	 * THE HANDLE RANGE NEVER SHRINKS, in either array. Callers iterate 0..NumPieces and
	 * resolve each handle, so a shrinking answer silently skips real pieces.
	 */
	CheckArraysAreParallel(*this, Binding, PieceSpecs.Num(), TEXT("after removing piece 1"));

	for (int32 Index = 0; Index < PieceSpecs.Num(); ++Index)
	{
		const bool bShouldBeGone = Index == Removed;

		/*
		 * ONE FUNCTION DID BOTH. The graph knows the piece is gone and the binding has
		 * let go of the actor, and there was only ever one call — this is what stops the
		 * two halves being separately callable and therefore separately forgettable.
		 */
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
			/*
			 * A SURVIVOR IS UNTOUCHED, and pointer identity is what makes that checkable:
			 * under compaction handle 2 hands back the stand-in for the beam.
			 */
			TestTrue(
				FString::Printf(TEXT("handle %d should still resolve to its own stand-in, got %s"),
					Index, *GetNameSafe(Binding.GetActor(Index))),
				Binding.GetActor(Index) == StandIns[Index]);
		}

		/*
		 * THE BOX SURVIVES REMOVAL, INCLUDING THE REMOVED PIECE'S OWN. It records where
		 * the brick WAS, which is what debris and a collapse replay want; and a cleared
		 * box would be a zero-size box at the world origin, which reads as real geometry
		 * rather than as absent. Only the actor pointer has a lifetime problem.
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

	/* Removing the same piece twice is not a second removal, and must not report one. */
	TestTrue(TEXT("removing an already-removed piece must report that it removed nothing"),
		!Binding.RemovePiece(Removed));

	ReleaseStandIns(StandIns);

	return true;
}

/**
 * AN ACTOR DESTROYED BY SOME OTHER ROUTE READS AS NULL, not as a live pointer to
 * freed memory.
 *
 * The tombstone above covers the deliberate case — the player pulls a brick and
 * RemovePiece clears the binding. This is the other one: a level transition, an
 * expiring lifespan, anything else calling Destroy. Nothing tells the binding, so the
 * binding has to be unable to be wrong about it, which is what TWeakObjectPtr buys and
 * a raw UObject* does not.
 *
 * THE PIECE IS STILL LIVE IN THE GRAPH THROUGHOUT, which is the discriminating part: a
 * dead actor is not a removed piece, the solver still routes load through it, and only
 * the actor lookup changes its answer.
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

	/*
	 * UNROOTED ON PURPOSE, and the local pointer is cleared before the collect, so once
	 * the binding's weak pointer is the only thing that ever referred to it the object
	 * is genuinely unreferenced and genuinely collectable. A rooted stand-in would make
	 * the second half of this test vacuous.
	 */
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

	/* The positive control, without which every assertion below passes on a stub. */
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

	/*
	 * AND STILL NULL ONCE THE MEMORY IS ACTUALLY RECLAIMED. Marking garbage is what a
	 * destroyed actor does first; the collect is what turns a raw pointer into a dangle.
	 */
	CollectGarbage(RF_NoFlags, true);

	TestTrue(
		FString::Printf(TEXT("a collected actor must still read as null, got %s"),
			*GetNameSafe(Binding.GetActor(1))),
		Binding.GetActor(1) == nullptr);

	/*
	 * A DEAD ACTOR IS NOT A REMOVED PIECE. The graph never heard about any of this, so
	 * the piece is still in it, still bound to a handle and still carrying load — the
	 * world's half of the pair went away, and only that half changed its answer.
	 */
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
 * WHICH PIECES A SOLVE RELEASES: exactly the ones the solver is no longer holding up,
 * and the binding does not care WHY it is not holding them up.
 *
 * THE STRANDED-VERSUS-FALLING DECISION, MADE TESTABLE. GetPieceSupport draws four
 * states and two of them mean the brick comes down: Falling is physics, Stranded is the
 * solver declining to divide load round a knot. That distinction is real and worth
 * keeping — a collapse test asserts nothing was Stranded when the wall went, so it can
 * tell a wall that fell from load from one the solver gave up on — but it is a
 * diagnostic about the SOLVE. Nothing about the brick differs: either way no load path
 * reaches the earth, so either way it is not being held up. A binding that branched on
 * the reason would be claiming a stranded brick should hang in the air. Pieces 2 and 3
 * are Stranded and piece 4 is Falling, and this test requires all three to release
 * identically.
 *
 * AND THE PUSH IS EXPLICIT, NOT A CALLBACK. Solving is asserted to release nothing at
 * all, because a solve is re-runnable and non-destructive and must stay that way;
 * releasing is what ApplyResults does, when the caller says so.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureBindingReleaseFollowsSupportTest,
	"DestructionGame.Core.StructureBinding.ReleaseFollowsSupport",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureBindingReleaseFollowsSupportTest::RunTest(const FString& Parameters)
{
	using namespace StructureBindingTestSupport;

	/*
	 * All four support states in one structure, lifted from Structure.PieceSupportReason
	 * so the states themselves are already pinned elsewhere and this test is only about
	 * what the binding does with them.
	 *
	 *                       B          piece 4, resting on Y through a bed joint
	 *                       |
	 *      Z   —   X   —    Y          pieces 1, 2, 3
	 *      |
	 *   [ pier ]                       piece 0
	 *   ========
	 *
	 * The pier is Grounded, Z is Supported, X and Y are the knot and are Stranded, and B
	 * rests only on the knot and is Falling.
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

	/*
	 * SOLVING RELEASES NOTHING. If this ever fails, the push has become a callback from
	 * inside the solver — which drags the world back into the world-free layer and fires
	 * during a solve that anything is allowed to re-run for a strain readout.
	 */
	for (int32 Index = 0; Index < PieceSpecs.Num(); ++Index)
	{
		TestTrue(
			FString::Printf(TEXT("solving must release nothing, but piece %d came back released"), Index),
			!Binding.IsReleased(Index));
	}

	/* The fixture's own precondition: these are the states the expectations rest on. */
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

		/*
		 * THE RULE ITSELF, stated against the solver's own answer rather than against the
		 * expectation column, so a table row that got a state wrong is caught by the
		 * structure instead of agreeing with itself. Released is exactly "not held up",
		 * with Stranded and Falling on the same side of the line.
		 */
		const EPieceSupport State = Binding.GetStructure().GetPieceSupport(Index);
		const bool bHeldUp = State == EPieceSupport::Grounded || State == EPieceSupport::Supported;

		TestTrue(
			FString::Printf(TEXT("piece %d reads %s and IsReleased %d — release must be exactly 'not held up'"),
				Index, NameOfSupport(State), Binding.IsReleased(Index) ? 1 : 0),
			Binding.IsReleased(Index) == !bHeldUp);

		/* Releasing a piece does not take it out of the graph; it is still a brick. */
		TestTrue(
			FString::Printf(TEXT("releasing piece %d must not remove it from the graph"), Index),
			!Binding.IsPieceRemoved(Index));
	}

	/*
	 * A REMOVED PIECE IS NEVER RELEASED. Its actor was cleared when it was removed, so
	 * there is nothing left to hand to physics — and a binding that released it anyway
	 * would be reporting work done on an actor it no longer has.
	 */
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
 * RELEASE IS ONE WAY. A second push does not re-release a brick that has already
 * fallen, and — the sharp half — it does not FREEZE one either, however the solver's
 * answer has changed in the meantime.
 *
 * WHY RE-FREEZING IS THE DANGEROUS DIRECTION. A released brick has been handed to Chaos
 * and has MOVED. FStructurePiece has no position and neither does the graph, so nothing
 * in this layer knows where the brick went — freezing it back to kinematic pins it
 * wherever the physics happened to leave it, mid-air, mid-tumble, and the structure now
 * claims to be held up by a brick lying on the floor. Re-releasing is merely wasteful;
 * re-freezing is wrong.
 *
 * THE FIXTURE MAKES THE SOLVER GENUINELY CHANGE ITS MIND, which is what stops this
 * being a test of an unreachable branch:
 *
 *      [ G ] — X          pieces 0 and 2, head-jointed
 *      =====    |
 *               F         piece 1, floating beneath X on a bed joint
 *
 * A bed joint beneath wins the support tier outright, so while F is there X's only
 * support is F — and F is held up by nothing, so X is Falling and both are released.
 * Remove F and its bed joint leaves the relation entirely, X falls back to its head
 * joint, and the head joint's far end is the earth: X reads SUPPORTED on the very next
 * solve. Removal promoting a head joint is the one thing in this model that turns an
 * unsupported piece back into a supported one, and it is exactly the player's move.
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

	/* Fixture precondition: a bed joint beneath outranks the head joint to the earth. */
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

	/*
	 * A SECOND PUSH ON AN UNCHANGED STRUCTURE IS A NO-OP. Nothing has moved in the graph,
	 * so nothing may be released again — and the count is the only way to see that, since
	 * IsReleased reads true either way.
	 */
	const int32 SecondRelease = Binding.ApplyResults();

	TestTrue(
		FString::Printf(TEXT("pushing again with nothing changed must release nothing, got %d"),
			SecondRelease),
		SecondRelease == 0);

	TestTrue(TEXT("X must still be released after the second push"), Binding.IsReleased(2));

	/* Now change the solver's mind: pull the floating piece out from under X. */
	TestTrue(TEXT("removing the floating piece should report that it removed one"),
		Binding.RemovePiece(1));

	Binding.SolveLoads();

	TestTrue(
		FString::Printf(TEXT("fixture: with the bed joint gone X should fall back to its head joint and read Supported, got %s"),
			NameOfSupport(Binding.GetStructure().GetPieceSupport(2))),
		Binding.GetStructure().GetPieceSupport(2) == EPieceSupport::Supported);

	const int32 ThirdRelease = Binding.ApplyResults();

	/*
	 * THE ASSERTION THIS TEST EXISTS FOR. The solver now says X is held up, and X has
	 * already fallen. bReleased is a latch, not a mirror of the current support state:
	 * the push must leave it alone, release nothing, and above all not put X back.
	 */
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
 * A PIECE IS RELEASED ONLY IF THE LAST SOLVE ACTUALLY COMPUTED A SUPPORT STATE FOR IT.
 *
 * THE POLARITY INVERTS AT THIS SEAM, AND THAT IS THE WHOLE BUG. EPieceSupport::Falling
 * sits at enumerator zero deliberately, so that a zero-filled or absent solver answer
 * reads "nothing is holding this up" rather than claiming the structure rests on the
 * earth — fail-CLOSED, because down there Falling is a DIAGNOSTIC and the cautious answer
 * is the one that promises least. One layer up, Falling becomes the trigger for an
 * irreversible destructive action: hand the brick to physics and latch it forever. The
 * same default is now fail-OPEN. That is DESIGN.md §2's caller-obligation shape recurring
 * at the binding seam — a value that is safe as an ANSWER is unsafe as a COMMAND — and
 * the obligation is this layer's, not the solver's.
 *
 * THE ORACLE IS THE SOLVE'S OWN EXTENT. FStructure::PieceSupported is sized by SolveLoads
 * and by nothing else, so GetStructure().NumPieces() at the moment of the last solve
 * bounds exactly which handles have an answer; anything at or beyond it has none, and an
 * out-of-range read of that array is answered Falling by the same range check an unknown
 * handle takes. "Not held up" and "not yet asked" are therefore the same value coming out
 * of GetPieceSupport, and only this layer can tell them apart.
 *
 * TWO ROWS, BECAUSE THERE ARE TWO WAYS TO BE PAST THE EXTENT and a guard can close one
 * without closing the other. Row 1 is the array being EMPTY — nothing has solved at all,
 * so every handle including the grounded foundation reads Falling, which
 * Structure.PieceSupportDegenerateInputs already pins as correct at the solver level. Row
 * 2 is the array being SHORT — a piece added after the last solve, which is the one the
 * game will actually hit: a player lays a brick onto a settled wall and the brick goes
 * dynamic before anything has solved it. A guard written as "have we ever solved?" closes
 * row 1 and leaves row 2 wide open.
 *
 * AND THE DAMAGE IS PERMANENT, which is why the count is not the only assertion here.
 * bReleased is a one-way latch that ApplyResults skips forever after — ReleaseIsOneWay
 * requires exactly that — so no later solve can put the foundation back. A wrong release
 * is not a frame of flicker; it is a wall that can never stand again.
 *
 * Note FStructureBinding::RemovePiece already has this right and is the shape to copy: it
 * asks the graph whether a live piece went and acts only if the graph said yes. This acts
 * on a default.
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
	 *   [  pad 0  ]      a grounded pad and a brick on a bed joint: the smallest
	 *   ==========       structure that stands, lifted from PieceSupportDegenerateInputs.
	 */
	const TArray<FPieceSpec> PieceSpecs = {
		{ BrickMassKg, true },  // 0: pad, on the earth
		{ BrickMassKg, false }  // 1: brick, resting on the pad
	};

	const TArray<FConnectionSpec> ConnectionSpecs = {
		{ 0, 1, BedJointNormal }
	};

	/* ROW 1: nothing has been solved, so nothing may be released. */
	{
		FStructureBinding Binding;
		TArray<UObject*> StandIns;
		BuildBinding(Binding, 7, PieceSpecs, ConnectionSpecs, StandIns);

		CheckArraysAreParallel(*this, Binding, PieceSpecs.Num(), TEXT("as built"));

		/*
		 * THE FIXTURE'S PRECONDITION, AND THE REASON THE ROW EXISTS. Every handle reads
		 * Falling before a solve — the grounded pad included — because the support array
		 * has not been sized yet. That is the solver being correct, not the solver being
		 * broken; what must not happen is this layer treating it as an instruction.
		 */
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

		/*
		 * THE GROUNDED PIECE IS THE SHARP ONE. A wall whose foundation goes dynamic at
		 * spawn is not a wall, and the latch means it never comes back.
		 */
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

	/* ROW 2: a piece added AFTER the last solve is past the answer's extent. */
	{
		FStructureBinding Binding;
		TArray<UObject*> StandIns;
		BuildBinding(Binding, 7, PieceSpecs, ConnectionSpecs, StandIns);

		Binding.SolveLoads();

		/* The fixture stands, so a settled push is a no-op and the row starts from zero. */
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

		/*
		 * THE PLAYER LAYS A BRICK ON THE SETTLED WALL, and does not re-solve — because
		 * placing and solving are separate calls and nothing forces the order. The new
		 * handle is one past the extent the last solve sized, so the solver has no answer
		 * for it at all.
		 */
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

		/*
		 * THE ORACLE, STATED AS THE FIXTURE'S PRECONDITION. Two pieces had an answer
		 * computed for them; the third is past that extent and reads Falling for want of
		 * an entry rather than for want of support.
		 */
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

		/*
		 * AND THE LATCH IS WHY THE COUNT ALONE IS NOT ENOUGH. Once something does solve,
		 * the laid brick is plainly held up — but a push that released it a moment ago has
		 * already latched it, and ApplyResults skips a released piece forever after. So a
		 * wrong release here cannot be undone by the very next solve.
		 */
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
 * A PIECE REF FAILS CLOSED. A stale, foreign or out-of-range {StructureId, PieceIndex}
 * resolves to nothing rather than to a confident handle on somebody else's brick.
 *
 * THIS IS THE WHOLE OF ACTOR-TO-PIECE, and the reason there is no map: we spawn the
 * bricks, so each carries its own identity and there is no reverse hash to invalidate.
 * What is left is the validation on the way back in, which matters because a stale ref
 * is the NORMAL case — the actor outlives the piece by at least a frame, and a removed
 * piece's slot stays a valid array index forever, so a bounds check alone is not enough
 * to reject it.
 *
 * A MATRIX RATHER THAN EXAMPLES, because the wrong answer here is a plausible small
 * integer and every rejection route has to be separately closed.
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
		/*
		 * THE TWO ACCEPTING ROWS COME FIRST, and without them every rejection below is
		 * satisfied by a function that returns INDEX_NONE unconditionally.
		 */
		{ TEXT("a live piece of this structure resolves"), { ThisStructure, 0 }, 0 },
		{ TEXT("and so does the one above it"), { ThisStructure, 1 }, 1 },

		/*
		 * THE SHARP ONE. A tombstoned slot is still a valid array index, so a bounds
		 * check alone accepts it — and the caller then gets a handle to a piece that is
		 * not in the graph, which the solver will happily answer questions about.
		 */
		{ TEXT("a piece that has been removed does not resolve"), { ThisStructure, 2 }, INDEX_NONE },

		{ TEXT("a negative index does not resolve"), { ThisStructure, -1 }, INDEX_NONE },
		{ TEXT("INDEX_NONE as an index does not resolve"), { ThisStructure, INDEX_NONE }, INDEX_NONE },
		{ TEXT("one past the end does not resolve"), { ThisStructure, 3 }, INDEX_NONE },
		{ TEXT("a wildly out-of-range index does not resolve"), { ThisStructure, MAX_int32 }, INDEX_NONE },
		{ TEXT("MIN_int32 does not resolve, and must not be negated into range"), { ThisStructure, MIN_int32 }, INDEX_NONE },

		/*
		 * A ref from another structure names a piece that exists — somewhere else. This
		 * is what the StructureId is for, and it is the failure a bare index cannot see.
		 */
		{ TEXT("a valid index belonging to a DIFFERENT structure does not resolve"), { SomeOtherStructure, 0 }, INDEX_NONE },

		/*
		 * A DEFAULT REF MATCHES NOTHING, in either field. An actor that was never told
		 * who it is must not resolve to piece zero of whoever asked.
		 */
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

		/*
		 * AND THE ANSWER IS USABLE WITHOUT A SECOND CHECK. Whatever comes back is fed
		 * straight to GetActor, which must hand back the right stand-in for an accepted
		 * ref and null for every rejected one — never a crash and never somebody else's
		 * brick.
		 */
		UObject* const Expected = StandIns.IsValidIndex(Case.Expected) ? StandIns[Case.Expected] : nullptr;

		TestTrue(
			FString::Printf(TEXT("%s: the resolved handle %d should hand back %s, got %s"),
				Case.Description, Resolved, *GetNameSafe(Expected), *GetNameSafe(Binding.GetActor(Resolved))),
			Binding.GetActor(Resolved) == Expected);
	}

	/*
	 * IsReleased TAKES THE SAME HANDLES AND MUST FAIL CLOSED THE SAME WAY. GetActor is
	 * checked above against every rejected ref; this is the other accessor that reads
	 * GetBinding, and "not released" is its fail-closed answer — a true here says a brick
	 * has already been handed to physics, which makes ApplyResults skip it forever. The
	 * removed handle is in the list on purpose: its slot is still a valid array index, so
	 * a bounds check alone would let it through.
	 *
	 * That IsReleased can return true at all is established by ReleaseFollowsSupport and
	 * ReleaseIsOneWay in this file, so these rows are not satisfied by a stub.
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

	/*
	 * A BINDING THAT WAS NEVER GIVEN AN ID MATCHES NOTHING, including a ref that was
	 * never given one either. Two unidentified things must not find each other by both
	 * being INDEX_NONE — that is the fail-OPEN direction, and it is the one an equality
	 * check written without thinking produces.
	 */
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
 * A BUILT WALL CAN ENTER A BINDING, AND THE TWO ARRAYS COME OUT PARALLEL.
 *
 * THERE IS NO OTHER ROUTE, WHICH IS BOTH THE PROBLEM AND THE POINT. RunningBond produces
 * a finished FStructure and a Boxes array parallel to it; FStructureBinding owns its
 * FStructure privately and hands out no mutable reference, deliberately, because that is
 * the desync hole. So the only thing a caller can do today is write its own loop re-adding
 * every piece and every connection — which is exactly the two-arrays-in-lockstep code the
 * type exists to make inexpressible, moved out to the call site where nothing checks it.
 * AdoptLayout is that loop written once.
 *
 * IT IS A REPLAY, AND IT IS CORRECT ONLY BECAUSE A LAYOUT IS APPEND-ONLY. RunningBond adds
 * pieces and connections and never removes one, so handle i of the layout is handle i of
 * the binding by construction — index-by-index copying is sound. The moment a producer
 * removes a piece, or reuses a slot, that stops being true.
 *
 * THE ASSERTIONS ARE THE PARALLELISM ITSELF, per handle: mass, grounded, box and actor,
 * not merely a matching count. Two test-side mutations prove they bite, and both are free:
 * reverse the Actors array before adopting, and the actor identity check fails on every
 * piece; feed Layout.Boxes[i + 1] instead of Layout.Boxes[i], and the box check fails.
 * Counts alone survive both.
 *
 * AND IT REFUSES A LAYOUT WHOSE OWN ARRAYS ARE ALREADY OUT OF STEP. RunningBond can
 * produce one: an infinite brick dimension passes the !(x > 0.0) spec guard, LayBrick
 * computes an infinite mass, FStructure::AddPiece refuses it, and the box is appended
 * anyway — so Boxes ends up one longer than the piece array. Copying such a layout
 * index-by-index PROPAGATES that desync into the binding, and the binding-level guard
 * already queued (FStructureBinding::AddPiece ignoring what FStructure::AddPiece returns)
 * would not close the composed case, because the mass it would be handed is a real one
 * belonging to the wrong piece — or a zero, which AddPiece accepts. A type whose whole
 * contract is that its two arrays cannot desync must not be the thing that launders a
 * known-desynced input into that shape. So the check is at the door, and a refusal writes
 * nothing.
 *
 * World-free: stand-in UObjects in the transient package, no world and no actors.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructureBindingAdoptLayoutTest,
	"DestructionGame.Core.StructureBinding.AdoptLayout",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructureBindingAdoptLayoutTest::RunTest(const FString& Parameters)
{
	using namespace StructureBindingTestSupport;
	using namespace DestructionLayout;

	/*
	 * A FLUSH WALL, WHICH IS THE MIXED-SIZE CASE. Three courses of three gives 3 + 4 + 3 =
	 * 10 pieces, half of the middle course being half bats — so the per-piece mass check
	 * has two different masses to tell apart. A ragged wall would be ten copies of one
	 * number, and a replay that handed every piece the same mass would pass.
	 */
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

	/*
	 * FLOOR THE FIXTURE. A one-piece jointless layout would satisfy every assertion below
	 * while proving nothing about parallelism, and an empty one would satisfy them
	 * vacuously.
	 */
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

		/*
		 * BOTH COLUMNS HAVE TO VARY OR THEIR ASSERTIONS ARE FREE. Two piece sizes, and
		 * only the bottom course on the earth — otherwise a replay that hard-coded either
		 * field would go unnoticed.
		 */
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

	/* The identity is the caller's, not the layout's: adoption must leave it alone. */
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

	/*
	 * PER HANDLE, ALL FOUR COLUMNS. This is where a shifted array shows up: the counts
	 * above are identical under a reversed actor list and under an off-by-one box index,
	 * and both of those are silent in every other way — the wall stands, the loads are
	 * right, and the player shoots one brick and a different one falls.
	 */
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

	/* Every joint, whole: handles, normal, area and profile, all exact. */
	for (int32 Joint = 0; Joint < Layout.Structure.NumConnections(); ++Joint)
	{
		const FConnection& Original = Layout.Structure.GetConnection(Joint);
		const FConnection& Adopted = Binding.GetStructure().GetConnection(Joint);

		TestTrue(
			FString::Printf(
				TEXT("joint %d should join the same pieces, %d-%d, got %d-%d"),
				Joint, Original.PieceA, Original.PieceB, Adopted.PieceA, Adopted.PieceB),
			Adopted.PieceA == Original.PieceA && Adopted.PieceB == Original.PieceB);

		/*
		 * THE NORMAL IS COMPARED EXACTLY, and a flipped one would not be caught by the
		 * loads: RoleOf turns the normal toward whichever piece it is asked about, so a
		 * consistently flipped joint reports identical numbers. The pairing and the normal
		 * travel together or the replay can quietly transpose them.
		 */
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

	/*
	 * AND THE ADOPTED GRAPH SOLVES TO THE SAME ANSWER, which is the outcome-shaped check
	 * on top of the column-by-column ones. Solving is deterministic and the arithmetic is
	 * identical, so agreement is bit-for-bit; a replay that got the graph subtly wrong —
	 * a joint's ends transposed, a piece grounded that should not be — produces plausible
	 * numbers that differ here and nowhere else.
	 */
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

		/* The wall stands, so a support mismatch is a real difference and not both falling. */
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

	/*
	 * WHAT ADOPTION REFUSES, and every refusal must write NOTHING — a half-adopted binding
	 * is worse than none, because the pieces it did take would look like a real wall.
	 */
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
		 * THE DESYNC RUNNING BOND CAN ALREADY PRODUCE, reproduced by hand rather than by
		 * feeding an infinite brick dimension: that route ALSO drives JoinIfTouching into
		 * Layout.Boxes[INDEX_NONE], which aborts the whole automation run rather than
		 * failing a test. The state it reaches is what matters here, so the state is what
		 * is built.
		 */
		{
			FBrickLayout Extra;
			TestTrue(TEXT("fixture: the desync wall should be laid"), RunningBond(Spec, Extra));

			/*
			 * COPIED OUT FIRST. TArray::Add asserts on an argument that aliases the array's
			 * own storage, so Add(Boxes[0]) aborts the whole automation run rather than
			 * failing anything.
			 */
			const FPieceBox Duplicate = Extra.Boxes[0];
			Extra.Boxes.Add(Duplicate);

			CheckRefused(
				TEXT("a layout with one more box than pieces"),
				Extra,
				TArrayView<UObject* const>(Spares.GetData(), Extra.Boxes.Num()));
		}

		/* The same desync the other way round: a piece nothing has a box for. */
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

		/*
		 * THE ACTOR LIST IS THE THIRD PARALLEL ARRAY, and it comes from a different place
		 * again — whoever spawned the bricks. Too few and some handle binds to nothing;
		 * too many and there are actors in the world no piece will ever name, which is the
		 * direction that looks fine.
		 */
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

		/*
		 * AN EMPTY LAYOUT IS NOT A WALL. RunningBond refuses every spec that would produce
		 * one, so accepting it here would make AdoptLayout the more permissive of the two —
		 * and a caller that got true back would have a binding it can never see anything
		 * from and no way to tell that from success.
		 */
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

#endif // WITH_DEV_AUTOMATION_TESTS
