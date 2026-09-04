// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Connection.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"
#include "Tests/BrickWorldTestSupport.h"
#include "World/BrickActor.h"
#include "World/DestructionStructureSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * BUILD-MODE UI-1 — the incremental WORLD-LAYER place (BUILD_MODE_PLAN.md, "the second part").
 *
 * The core new seam: BeginBuild opens an EMPTY live structure, and PlaceBuildPiece runs the
 * proven snap brain (BuildMode::SolveSnapCandidates) against the pieces already in that
 * structure, adopts the piece at the best-ranked snapped pose, SPAWNS one ABrickActor for it,
 * and forms that candidate's joints as real FConnections — so a single call grows the live
 * world structure by one jointed, actor-backed piece.
 *
 * A WORLD TEST, NOT A CORE UNIT TEST, because it spawns an actor and reads it back off the
 * binding: FStructureBinding alone is world-free (StructureBindingTest.cpp uses UObject
 * stand-ins), but the whole point of this seam is that PlaceBuildPiece puts a real ABrickActor
 * in the world and binds it. So it rides the shared FBrickTestWorld harness under AGameModeBase
 * (no scenario, so the world starts brick-empty). It NEVER ticks physics: the assertions are on
 * the live binding/structure — piece count, actor identity, the formed joint, the snapped pose —
 * the MECHANISM, never displacement. Two severed pieces can rest exactly in place, and here
 * nothing is even released; a displacement assertion would measure nothing.
 *
 * THE RUNNING-BOND NUMBERS ARE READ, NOT RE-DERIVED, matching SnapSolverTest.cpp: a
 * 21.5 x 10.25 x 6.5 brick on 1 cm joints gives the 22.5 x 11.25 x 7.5 coordinating grid, so a
 * next-course brick biased toward +X snaps to +11.25 in X and +7.5 in Z. The bed overlap of two
 * half-staggered full bricks is (21.5 - 11.25) x 10.25 = 10.25 x 10.25 = 105.0625 cm2.
 */
namespace BuildPlacePieceTestSupport
{
	/* HALF sizes: this is the half-extent of a FULL 21.5 x 10.25 x 6.5 brick, as SnapSolverTest names it. */
	const FVector HalfBrick(10.75, 5.125, 3.25);

	/* The running-bond half-stagger pose one course up from the origin brick, on its +X side. */
	const FVector ExpectedRunningBondCentre(11.25, 0.0, 7.5);

	/*
	 * The two half-staggered full bricks overlap over (21.5 - 11.25) = 10.25 cm in X and the
	 * full 10.25 cm in Y, so the bed face is 10.25 x 10.25 = 105.0625 cm2. Spelled here rather
	 * than imported so the test fails if the solver's area math drifts.
	 */
	constexpr double ExpectedBedAreaSqCm = 105.0625;

	/*
	 * Full-field profile identity. FConnectionStrength has no operator==, so a joint profile is
	 * pinned by matching all five fields against the named library constant — the same discipline
	 * SnapSolverTest.cpp and JointInferenceTest.cpp use. This is what proves the auto-formed bed
	 * joint is the STRONG GeneralPurposeMortar and not the weak GeneralPurposeMortarPerpend or a
	 * dry-stone bearing, which differ on the two bond axes, on compression, on friction and on
	 * the shear ceiling. Asserting one field alone would let a sibling profile through.
	 */
	void CheckProfileIdentity(
		FAutomationTestBase& Test,
		const FString& Prefix,
		const FConnectionStrength& Got,
		const FConnectionStrength& Want)
	{
		Test.TestEqual(Prefix + TEXT("CompressiveStrengthMPa"),
			Got.CompressiveStrengthMPa, Want.CompressiveStrengthMPa);
		Test.TestEqual(Prefix + TEXT("ShearCohesionMPa"),
			Got.ShearCohesionMPa, Want.ShearCohesionMPa);
		Test.TestEqual(Prefix + TEXT("TensileStrengthMPa"),
			Got.TensileStrengthMPa, Want.TensileStrengthMPa);
		Test.TestEqual(Prefix + TEXT("FrictionCoefficient"),
			Got.FrictionCoefficient, Want.FrictionCoefficient);
		Test.TestEqual(Prefix + TEXT("MaxShearStrengthMPa"),
			Got.MaxShearStrengthMPa, Want.MaxShearStrengthMPa);
	}
}

/**
 * BeginBuild opens an EMPTY live structure, and PlaceBuildPiece grows it by one live, jointed,
 * actor-backed piece at the best-ranked snapped pose.
 *
 * STEP BY STEP:
 *  1. BeginBuild registers an empty structure — Find(id) is non-null and holds zero pieces.
 *  2. The FIRST grounded brick becomes piece 0: ref {id, 0}, one bound ABrickActor, grounded,
 *     ClayBrick, and NO connection (nothing to joint to yet).
 *  3. A SECOND brick requested near the +X next-course pose SNAPS to the running-bond stagger,
 *     spawns a distinct actor, and auto-forms exactly one real GeneralPurposeMortar bed joint
 *     between handles 0 and 1 — full-field, area 105.0625, |normal.Z| == 1 — and the placed box
 *     sits at (11.25, 0, 7.5), the snap pose, not the requested (11, 0, 7.5).
 *  4. A solve reads piece 0 Grounded and piece 1 Supported — held by the bed, not the earth:
 *     the "it's live and it stands" proof.
 *
 * NEEDS A TICKING WORLD: a real world for the actor spawn, but it never ticks — this is about
 * what the structure and its bound actors ARE, not about anything moving.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildPlacePieceGrowsALiveStructureTest,
	"DestructionGame.World.BuildMode.PlaceBuildPieceGrowsALiveStructure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildPlacePieceGrowsALiveStructureTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildPlacePieceTestSupport;
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	/* STEP 1: an empty build structure exists and holds nothing. */
	const int32 StructureId = Subsystem.BeginBuild();

	FStructureBinding* Binding = Subsystem.Find(StructureId);

	TestNotNull(
		FString::Printf(TEXT("BeginBuild should register a live structure, but Find(%d) is null"),
			StructureId),
		Binding);

	if (Binding == nullptr)
	{
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("a just-begun build structure holds no pieces, got %d"),
			Binding->NumPieces()),
		Binding->NumPieces(), 0);

	/* STEP 2: the first grounded brick lands as piece 0, actor-backed, no joint yet. */
	const FPieceRef Ref0 =
		Subsystem.PlaceBuildPiece(StructureId, FVector(0.0, 0.0, 0.0), HalfBrick, ClayBrick, /*bGrounded*/ true);

	TestTrue(
		FString::Printf(TEXT("the first placed piece should be ref {%d, 0}, got {%d, %d}"),
			StructureId, Ref0.StructureId, Ref0.PieceIndex),
		Ref0 == FPieceRef{ StructureId, 0 });

	/* Find again — the binding pointer is stable (TUniquePtr), but re-fetch to be exact. */
	Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr)
	{
		AddError(TEXT("the structure vanished after the first placement"));
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("one placement should grow the structure to 1 piece, got %d"),
			Binding->NumPieces()),
		Binding->NumPieces(), 1);

	ABrickActor* Actor0 = Cast<ABrickActor>(Binding->GetActor(0));
	TestNotNull(
		FString::Printf(TEXT("piece 0 should be backed by a spawned ABrickActor, got %s"),
			*GetNameSafe(Binding->GetActor(0))),
		Actor0);

	TestTrue(
		TEXT("piece 0 was placed grounded, so the structure must record it grounded"),
		Binding->GetStructure().GetPiece(0).bIsGrounded);

	TestTrue(
		TEXT("piece 0's material must be the ClayBrick profile it was placed with"),
		Binding->GetStructure().GetPiece(0).Material == &ClayBrick);

	TestEqual(
		FString::Printf(TEXT("the lone first piece has nothing to joint to, so 0 connections, got %d"),
			Binding->GetStructure().NumConnections()),
		Binding->GetStructure().NumConnections(), 0);

	/*
	 * STEP 3: a second brick requested near the +X next-course pose snaps to the running-bond
	 * stagger and auto-forms one real bed joint.
	 */
	const FPieceRef Ref1 =
		Subsystem.PlaceBuildPiece(StructureId, FVector(11.0, 0.0, 7.5), HalfBrick, ClayBrick, /*bGrounded*/ false);

	TestTrue(
		FString::Printf(TEXT("the second placed piece should be ref {%d, 1}, got {%d, %d}"),
			StructureId, Ref1.StructureId, Ref1.PieceIndex),
		Ref1 == FPieceRef{ StructureId, 1 });

	Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr)
	{
		AddError(TEXT("the structure vanished after the second placement"));
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("two placements should grow the structure to 2 pieces, got %d"),
			Binding->NumPieces()),
		Binding->NumPieces(), 2);

	ABrickActor* Actor1 = Cast<ABrickActor>(Binding->GetActor(1));
	TestNotNull(
		FString::Printf(TEXT("piece 1 should be backed by a spawned ABrickActor, got %s"),
			*GetNameSafe(Binding->GetActor(1))),
		Actor1);

	/* A NEW brick, not the same actor re-bound: the world must have grown by a distinct actor. */
	TestTrue(
		TEXT("piece 1's actor must be a DISTINCT actor from piece 0's"),
		Actor1 != nullptr && Actor1 != Actor0);

	/*
	 * THE PLACED BOX SITS AT THE SNAP POSE, not the requested one — the brain moved it onto the
	 * running-bond grid. Read the pose off the binding's own box for handle 1.
	 */
	const FVector PlacedCentre = Binding->GetBinding(1).Box.CentreCm;
	TestTrue(
		FString::Printf(
			TEXT("piece 1 should be snapped to the running-bond pose (11.25, 0, 7.5), got (%g, %g, %g)"),
			PlacedCentre.X, PlacedCentre.Y, PlacedCentre.Z),
		PlacedCentre.Equals(ExpectedRunningBondCentre, KINDA_SMALL_NUMBER));

	/* EXACTLY ONE real connection formed by the placement. */
	TestEqual(
		FString::Printf(TEXT("the running-bond placement should form exactly one bed joint, got %d"),
			Binding->GetStructure().NumConnections()),
		Binding->GetStructure().NumConnections(), 1);

	if (Binding->GetStructure().NumConnections() == 1)
	{
		const FConnection& Bed = Binding->GetStructure().GetConnection(0);

		/* It links the two pieces, in either order. */
		const bool bLinksZeroAndOne =
			(Bed.PieceA == 0 && Bed.PieceB == 1) || (Bed.PieceA == 1 && Bed.PieceB == 0);
		TestTrue(
			FString::Printf(TEXT("the bed joint must link handles 0 and 1, got {%d, %d}"),
				Bed.PieceA, Bed.PieceB),
			bLinksZeroAndOne);

		/* A real running-bond BED: horizontal face, so the normal is vertical. */
		TestEqual(
			FString::Printf(TEXT("a bed joint's normal is vertical, |Z| should be 1, got %g"),
				FMath::Abs(Bed.InterfaceNormal.Z)),
			FMath::Abs(Bed.InterfaceNormal.Z), 1.0, 1.0e-9);

		/* The half-stagger bed overlap, spelled independently above. */
		TestEqual(
			FString::Printf(TEXT("the bed overlap area should be 105.0625 cm2, got %g"),
				Bed.InterfaceAreaSqCm),
			Bed.InterfaceAreaSqCm, ExpectedBedAreaSqCm, 1.0e-6);

		/* Full-field GeneralPurposeMortar — the strong bed bond, inferred, not a perpend. */
		CheckProfileIdentity(
			*this,
			TEXT("bed joint profile == GeneralPurposeMortar: "),
			Bed.Strength,
			GeneralPurposeMortar);
	}

	/*
	 * STEP 4: it's live and it stands. A solve reads the grounded foot Grounded and the upper
	 * brick Supported — held up by the bed joint, not by the earth.
	 */
	Binding->SolveLoads();

	TestTrue(
		FString::Printf(TEXT("piece 0 rests on the earth, so it should solve Grounded, got state %d"),
			static_cast<int32>(Binding->GetStructure().GetPieceSupport(0))),
		Binding->GetStructure().GetPieceSupport(0) == EPieceSupport::Grounded);

	TestTrue(
		FString::Printf(TEXT("piece 1 is held by the bed joint, so it should solve Supported, got state %d"),
			static_cast<int32>(Binding->GetStructure().GetPieceSupport(1))),
		Binding->GetStructure().GetPieceSupport(1) == EPieceSupport::Supported);

	return true;
}

/**
 * BUILD-MODE UI-2, FIRST STEP — the NON-MUTATING preview query.
 *
 * PreviewBuildPiece answers what the best-ranked candidate WOULD be if placed here — the snapped
 * pose, the snap kind, and how many joints it would form — WITHOUT changing anything. It is the
 * exact snap decision PlaceBuildPiece makes, surfaced so the UI can draw a ghost before the click.
 *
 * A WORLD TEST for the same reason its sibling above is: PlaceBuildPiece (used to seed and then to
 * confirm) spawns a real ABrickActor and binds it, so the fixture needs the FBrickTestWorld
 * harness. It never ticks — every assertion is on the query result and on the structure's own
 * state, the MECHANISM, never displacement.
 *
 * THE ASSERTIONS ARE THREE THINGS AT ONCE:
 *  - PREDICTION: the preview's Kind is BrickNextCourse, its CentreCm is the running-bond snap
 *    (11.25, 0, 7.5) — not the requested (11, 0, 7.5) — and it would form exactly ONE joint (the
 *    bed). The numbers are read from SnapSolverTest's grid, not re-derived here.
 *  - NON-MUTATION: after the preview the structure still holds ONE piece, ZERO connections, and
 *    GetActor(1) is still null — no second actor was spawned. This is the entire point: a preview
 *    that added a piece, or spawned a ghost's actor into the live binding, would be a place, not a
 *    preview.
 *  - CONSISTENCY: a following PlaceBuildPiece at the SAME requested pose lands its box at exactly
 *    the previewed CentreCm and forms exactly the previewed JointCount — so the preview provably
 *    predicts the commit rather than merely being plausible.
 *
 * AND FAIL-CLOSED: PreviewBuildPiece against an unknown structure id returns bValid == false.
 *
 * NEEDS A TICKING WORLD: a real world for the seed/confirm actor spawns, but it never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPreviewBuildPieceIsNonMutatingAndPredictsTheCommitTest,
	"DestructionGame.World.BuildMode.PreviewBuildPieceIsNonMutatingAndPredictsTheCommit",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPreviewBuildPieceIsNonMutatingAndPredictsTheCommitTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildPlacePieceTestSupport;
	using namespace DestructionProfiles;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	/* One grounded brick, so there is something for a next-course preview to snap to. */
	const int32 StructureId = Subsystem.BeginBuild();

	Subsystem.PlaceBuildPiece(StructureId, FVector(0.0, 0.0, 0.0), HalfBrick, ClayBrick, /*bGrounded*/ true);

	FStructureBinding* Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr)
	{
		AddError(TEXT("the structure vanished after seeding the first piece"));
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("precondition: the seeded structure holds exactly 1 piece, got %d"),
			Binding->NumPieces()),
		Binding->NumPieces(), 1);

	/* The requested pose is off-grid on purpose — the snap must move it, and the preview must say so. */
	const FVector RequestedCentre(11.0, 0.0, 7.5);

	const FBuildPreview Preview =
		Subsystem.PreviewBuildPiece(StructureId, RequestedCentre, HalfBrick, ClayBrick);

	/* PREDICTION. */
	TestTrue(
		TEXT("a preview against a known structure is valid"),
		Preview.bValid);

	TestTrue(
		FString::Printf(TEXT("the +X next-course preview snaps to BrickNextCourse, got kind %d"),
			static_cast<int32>(Preview.Kind)),
		Preview.Kind == BuildMode::ESnapKind::BrickNextCourse);

	TestTrue(
		FString::Printf(
			TEXT("the preview centre is the running-bond snap (11.25, 0, 7.5), got (%g, %g, %g)"),
			Preview.CentreCm.X, Preview.CentreCm.Y, Preview.CentreCm.Z),
		Preview.CentreCm.Equals(ExpectedRunningBondCentre, KINDA_SMALL_NUMBER));

	TestEqual(
		FString::Printf(TEXT("the preview would form exactly one bed joint, got %d"),
			Preview.JointCount),
		Preview.JointCount, 1);

	/*
	 * NON-MUTATION — the whole point. Re-fetch and prove nothing grew: still one piece, still no
	 * connection, and no second actor was spawned into the binding (GetActor(1) is null for an
	 * unknown handle).
	 */
	Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr)
	{
		AddError(TEXT("the structure vanished across the preview call"));
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("preview must NOT add a piece: still 1 piece, got %d"),
			Binding->NumPieces()),
		Binding->NumPieces(), 1);

	TestEqual(
		FString::Printf(TEXT("preview must NOT form a connection: still 0 connections, got %d"),
			Binding->GetStructure().NumConnections()),
		Binding->GetStructure().NumConnections(), 0);

	TestNull(
		FString::Printf(TEXT("preview must NOT spawn a second actor: GetActor(1) should be null, got %s"),
			*GetNameSafe(Binding->GetActor(1))),
		Binding->GetActor(1));

	/*
	 * CONSISTENCY — the preview predicts the commit. Place the real piece at the SAME requested
	 * pose and read it back: the placed box sits at the previewed centre and forms the previewed
	 * number of joints.
	 */
	const FPieceRef Placed =
		Subsystem.PlaceBuildPiece(StructureId, RequestedCentre, HalfBrick, ClayBrick, /*bGrounded*/ false);

	TestTrue(
		FString::Printf(TEXT("the committed piece should be ref {%d, 1}, got {%d, %d}"),
			StructureId, Placed.StructureId, Placed.PieceIndex),
		Placed == FPieceRef{ StructureId, 1 });

	Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr)
	{
		AddError(TEXT("the structure vanished after the confirming placement"));
		return true;
	}

	const FVector PlacedCentre = Binding->GetBinding(1).Box.CentreCm;
	TestTrue(
		FString::Printf(
			TEXT("the committed box lands at the previewed centre (%g, %g, %g), got (%g, %g, %g)"),
			Preview.CentreCm.X, Preview.CentreCm.Y, Preview.CentreCm.Z,
			PlacedCentre.X, PlacedCentre.Y, PlacedCentre.Z),
		PlacedCentre.Equals(Preview.CentreCm, KINDA_SMALL_NUMBER));

	TestEqual(
		FString::Printf(
			TEXT("the commit forms exactly the previewed joint count %d, got %d connections"),
			Preview.JointCount, Binding->GetStructure().NumConnections()),
		Binding->GetStructure().NumConnections(), Preview.JointCount);

	/* FAIL-CLOSED — an unknown structure id previews nothing. */
	const FBuildPreview UnknownPreview =
		Subsystem.PreviewBuildPiece(9999, RequestedCentre, HalfBrick, ClayBrick);

	TestFalse(
		TEXT("a preview against an unknown structure id is invalid"),
		UnknownPreview.bValid);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
