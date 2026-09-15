// Copyright Epic Games, Inc. All Rights Reserved.

#include <limits>

#include "Misc/AutomationTest.h"

#include "Core/Connection.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/SessionToolbar.h"
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
		Subsystem.PlaceBuildPiece(StructureId, FVector(0.0, 0.0, 0.0), HalfBrick, ClayBrick);

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

	/*
	 * GROUNDED COMES FROM THE POSE, NOT FROM AN ARGUMENT (2026-09-15 DESIGN §8 ruling). This seed
	 * is centred at Z = 0, so its underside sits at -3.25 cm — at or below the ground plane, hence
	 * within one 1 cm joint of it — and the structure must record it grounded on that basis alone.
	 */
	TestTrue(
		TEXT("piece 0's snapped pose rests it on the earth, so the structure must record it grounded"),
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
		Subsystem.PlaceBuildPiece(StructureId, FVector(11.0, 0.0, 7.5), HalfBrick, ClayBrick);

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

	Subsystem.PlaceBuildPiece(StructureId, FVector(0.0, 0.0, 0.0), HalfBrick, ClayBrick);

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
		Subsystem.PlaceBuildPiece(StructureId, RequestedCentre, HalfBrick, ClayBrick);

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

/**
 * GROUNDED IS DERIVED FROM THE SNAPPED POSE, AND FROM NOTHING ELSE — the 2026-09-15 DESIGN §8
 * ruling, at the subsystem door where it is decided.
 *
 * PlaceBuildPiece no longer takes a bGrounded argument. A caller's guess is exactly what the
 * ruling forbids: the snap solver ranks by raw DISTANCE, so a cursor on the grounded course beside
 * a standing brick can be lifted a whole course onto its bed, and a piece 7.5 cm in the air that
 * was flagged grounded terminates load at the earth and CAN NEVER FALL. The committed piece's flag
 * must therefore come from where the piece actually ended up.
 *
 * THE RULE, SPELLED OUT HERE RATHER THAN IMPORTED. A piece is grounded when its BOTTOM FACE
 * (CentreCm.Z - ExtentCm.Z) is at or below one joint thickness above the ground plane Z = 0 —
 * i.e. `bottom <= 1.0`, with 1.0 cm being FSnapSettings::JointThicknessCm written out
 * independently. One joint of tolerance rather than zero because a brick laid on the earth beds
 * into its own mortar, and because both conventions this project has used must read grounded: the
 * rests-on-the-ground centre 3.25 (bottom exactly 0) and every pre-ruling harness's centre 0
 * (bottom -3.25, below the plane).
 *
 * AND IT IS DELIBERATELY NOT THE HOUSE `!(x > y)` FORM, which would be the WRONG POLARITY here.
 * The house form exists to make a NaN land inside the guard — but GROUNDED IS THE FAIL-OPEN SIDE
 * of this comparison: a grounded piece terminates load at the earth, absorbs whatever is stacked
 * on it and can never fall. Every comparison against NaN is false, so `!(bottom > 1.0)` answers
 * TRUE for a non-finite pose and mints exactly that immovable brick out of garbage arithmetic.
 * `bottom <= 1.0` answers FALSE for a NaN, so an unplaceable pose reads NOT grounded and the
 * solver stays free to drop it. DestructionGame.World.BuildMode.NonFinitePoseNeverCommitsGrounded
 * is the row that pins this polarity; production spells the same reasoning at the site
 * (ComputeBuildPlacement, World/DestructionStructureSubsystem.cpp).
 *
 * A TABLE, NOT FIVE TESTS, and every row is a HALF-BRICK of 3.25 so the arithmetic is one
 * subtraction the reader can do. The two rows either side of 4.25 are the whole point: 4.25 puts
 * the bottom at exactly 1.0 (grounded, the inclusive edge) and 4.5 puts it at 1.25 (not grounded),
 * so a `<` where the ruling says `<=`, or a tolerance retuned to 0 or to a course pitch, is caught.
 * A greater-than on the CENTRE rather than on the bottom face would also fail: centre 3.25 and
 * centre 4.25 are both grounded while 4.5 is not, which no centre threshold reproduces with a
 * brick-height gap between the rows.
 *
 * EACH ROW GETS ITS OWN EMPTY BUILD, so the piece is the structure's first and the snap is Free at
 * exactly the requested pose. That keeps the row about the grounded rule alone — a neighbour would
 * let the solver move the pose and confuse which Z was tested — and the placed box centre is
 * asserted to be the requested one so a row cannot silently be measuring some other pose.
 *
 * NEEDS A TICKING WORLD: a real world for the actor spawns, but it never ticks. Every assertion is
 * on the MECHANISM — the structure's own bIsGrounded flag and the placed box — never displacement.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlaceBuildPieceDerivesGroundedFromPoseTest,
	"DestructionGame.World.BuildMode.PlaceBuildPieceDerivesGroundedFromPose",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPlaceBuildPieceDerivesGroundedFromPoseTest::RunTest(const FString& Parameters)
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

	struct FGroundedCase
	{
		const TCHAR* Description;
		double CentreZCm;
		double BottomFaceZCm;
		bool bExpectGrounded;
	};

	const FGroundedCase Cases[] = {
		{ TEXT("the rests-on-the-ground course 0 centre"),      3.25,  0.0,   true  },
		{ TEXT("the legacy half-buried course 0 centre"),       0.0,  -3.25,  true  },
		{ TEXT("a bottom face exactly one joint up (the edge)"), 4.25, 1.0,   true  },
		{ TEXT("a bottom face a quarter centimetre past it"),   4.5,   1.25,  false },
		{ TEXT("a brick bedded a whole course in the air"),     10.75, 7.5,   false },
	};

	for (const FGroundedCase& Case : Cases)
	{
		const int32 StructureId = Subsystem.BeginBuild();

		const FVector RequestedCentre(0.0, 0.0, Case.CentreZCm);

		const FPieceRef Ref =
			Subsystem.PlaceBuildPiece(StructureId, RequestedCentre, HalfBrick, ClayBrick);

		FStructureBinding* Binding = Subsystem.Find(StructureId);

		if (Binding == nullptr || Ref.PieceIndex != 0)
		{
			AddError(FString::Printf(
				TEXT("fixture: %s did not place a first piece (ref {%d, %d})"),
				Case.Description, Ref.StructureId, Ref.PieceIndex));
			continue;
		}

		/* The lone piece is Free at the requested pose, so the row is about the Z it names. */
		const FVector PlacedCentre = Binding->GetBinding(0).Box.CentreCm;

		TestTrue(
			FString::Printf(
				TEXT("fixture: %s should land at its requested pose (0, 0, %g), got (%g, %g, %g)"),
				Case.Description, Case.CentreZCm, PlacedCentre.X, PlacedCentre.Y, PlacedCentre.Z),
			PlacedCentre.Equals(RequestedCentre, KINDA_SMALL_NUMBER));

		TestEqual(
			FString::Printf(
				TEXT("%s: centre Z %g minus the 3.25 half-height puts the bottom face at %g, which is %s one 1 cm joint of the ground, so the committed piece must read %s"),
				Case.Description, Case.CentreZCm, Case.BottomFaceZCm,
				Case.bExpectGrounded ? TEXT("WITHIN") : TEXT("BEYOND"),
				Case.bExpectGrounded ? TEXT("grounded") : TEXT("NOT grounded")),
			Binding->GetStructure().GetPiece(0).bIsGrounded, Case.bExpectGrounded);
	}

	return true;
}

/**
 * FREE PLACEMENT HONOURS THE REQUESTED POSE AND FORMS NO JOINTS — the deliberate escape from the
 * bond, selected by EPlacementMode rather than by luck.
 *
 * THE SOLVER ALREADY EMITS A FREE CANDIDATE, but it is appended LAST, so today it is reached only
 * when every snap is out of radius or occupancy-filtered. "Place it exactly where I am pointing"
 * is a mode the player chooses, so PreviewBuildPiece / PlaceBuildPiece take a
 * DestructionSession::EPlacementMode and Free selects that candidate however close a snap is.
 *
 * THE SAME REQUESTED POSE, TWICE, ONE PIECE OF STATE APART. Against the same one-brick structure
 * the cursor (11, 0, 10) is 0.79 cm from the running-bond next-course pose (11.25, 0, 10.75) — the
 * nearest snap by a wide margin over the same-course pose at (22.5, 0, 3.25), 13.33 cm away — so
 * Snap must move it there and form one bed joint, while Free must leave it at (11, 0, 10) forming
 * none. Asserting both from one fixture is what makes the mode demonstrably the cause: a
 * production that ignored the parameter would give the same answer twice, and one of the two legs
 * would fail whichever way it ignored it.
 *
 * THE NUMBERS ARE DERIVED, NOT IMPORTED. Brick 21.5 x 10.25 x 6.5 on 1 cm joints gives a
 * coordinating grid of 22.5 x 11.25 x 7.5, so the next-course pose over a seed centred at
 * (0, 0, 3.25) is (0 + 11.25, 0, 3.25 + 7.5) = (11.25, 0, 10.75) and the same-course pose is
 * (22.5, 0, 3.25). Offsets from (11, 0, 10): sqrt(0.25^2 + 0.75^2) = 0.79 and
 * sqrt(11.5^2 + 6.75^2) = 13.33.
 *
 * AND THE COMMIT AGREES WITH THE PREVIEW. A Free commit at the same pose lands its box at the
 * cursor exactly and leaves NumConnections at zero — the mechanism reading of "formed no joints",
 * exact and countable, never a position tolerance.
 *
 * NEEDS A TICKING WORLD: a real world for the actor spawns, but it never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPreviewBuildPieceFreeIgnoresSnapsTest,
	"DestructionGame.World.BuildMode.PreviewBuildPieceFreeIgnoresSnaps",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPreviewBuildPieceFreeIgnoresSnapsTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildPlacePieceTestSupport;
	using namespace DestructionProfiles;
	using namespace DestructionSession;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	/* One seed brick RESTING ON THE GROUND (centre 3.25, underside 0), the ruling's convention. */
	const int32 StructureId = Subsystem.BeginBuild();

	Subsystem.PlaceBuildPiece(StructureId, FVector(0.0, 0.0, 3.25), HalfBrick, ClayBrick);

	FStructureBinding* Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr)
	{
		AddError(TEXT("the structure vanished after seeding the first piece"));
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("fixture: the seeded structure holds exactly 1 piece, got %d"),
			Binding->NumPieces()),
		Binding->NumPieces(), 1);

	const FVector FreeCursorCm(11.0, 0.0, 10.0);
	const FVector NextCourseCm(11.25, 0.0, 10.75);

	/* SNAP: the nearest candidate wins and the cursor is moved onto the bond. */
	const FBuildPreview Snapped =
		Subsystem.PreviewBuildPiece(StructureId, FreeCursorCm, HalfBrick, ClayBrick, EPlacementMode::Snap);

	TestTrue(
		FString::Printf(TEXT("the Snap preview must take the next-course candidate, got kind %d"),
			static_cast<int32>(Snapped.Kind)),
		Snapped.Kind == BuildMode::ESnapKind::BrickNextCourse);

	TestTrue(
		FString::Printf(
			TEXT("the Snap preview centres on (11.25, 0, 10.75), got (%g, %g, %g)"),
			Snapped.CentreCm.X, Snapped.CentreCm.Y, Snapped.CentreCm.Z),
		Snapped.CentreCm.Equals(NextCourseCm, KINDA_SMALL_NUMBER));

	TestEqual(
		FString::Printf(TEXT("the Snap preview would form one bed joint, got %d"), Snapped.JointCount),
		Snapped.JointCount, 1);

	/* FREE: the same cursor, honoured verbatim, jointless. */
	const FBuildPreview Free =
		Subsystem.PreviewBuildPiece(StructureId, FreeCursorCm, HalfBrick, ClayBrick, EPlacementMode::Free);

	TestTrue(
		FString::Printf(TEXT("the Free preview must be the Free candidate, got kind %d"),
			static_cast<int32>(Free.Kind)),
		Free.Kind == BuildMode::ESnapKind::Free);

	TestTrue(
		FString::Printf(
			TEXT("the Free preview centres on the cursor (11, 0, 10) EXACTLY, got (%g, %g, %g)"),
			Free.CentreCm.X, Free.CentreCm.Y, Free.CentreCm.Z),
		Free.CentreCm.Equals(FreeCursorCm, KINDA_SMALL_NUMBER));

	TestEqual(
		FString::Printf(TEXT("a Free placement bonds to nothing, so 0 joints, got %d"), Free.JointCount),
		Free.JointCount, 0);

	/* Its bottom face is at 6.75 cm, most of a course in the air, so it is not grounded. */
	TestFalse(
		TEXT("the Free pose's bottom face sits 6.75 cm up, so the preview must not read it grounded"),
		Free.bGrounded);

	/* THE COMMIT AGREES: the box lands at the cursor and the structure gains no connection. */
	const FPieceRef Placed = Subsystem.PlaceBuildPiece(
		StructureId, FreeCursorCm, HalfBrick, ClayBrick, EPlacementMode::Free);

	TestTrue(
		FString::Printf(TEXT("the Free commit should be ref {%d, 1}, got {%d, %d}"),
			StructureId, Placed.StructureId, Placed.PieceIndex),
		Placed == FPieceRef{ StructureId, 1 });

	Binding = Subsystem.Find(StructureId);
	if (Binding == nullptr)
	{
		AddError(TEXT("the structure vanished after the Free placement"));
		return true;
	}

	const FVector PlacedCentre = Binding->GetBinding(1).Box.CentreCm;

	TestTrue(
		FString::Printf(
			TEXT("the Free commit lands its box on the cursor (11, 0, 10), got (%g, %g, %g)"),
			PlacedCentre.X, PlacedCentre.Y, PlacedCentre.Z),
		PlacedCentre.Equals(FreeCursorCm, KINDA_SMALL_NUMBER));

	TestEqual(
		FString::Printf(TEXT("a Free commit forms no joint, so the structure still holds %d connections"),
			Binding->GetStructure().NumConnections()),
		Binding->GetStructure().NumConnections(), 0);

	TestFalse(
		TEXT("the Free-committed piece is in the air, so the structure must not record it grounded"),
		Binding->GetStructure().GetPiece(1).bIsGrounded);

	return true;
}

/**
 * A NON-FINITE POSE NEVER READS GROUNDED — the POLARITY of the pose-derived rule, pinned so the
 * house style cannot be applied to it by reflex.
 *
 * WHAT IT DROVE, AND WHAT IT PINS NOW. Written as a regression pin on the polarity of the grounded
 * comparison (ComputeBuildPlacement writes `BottomFaceZCm <= Settings.JointThicknessCm`), its first
 * run found something else: the COMMIT leg tripped three engine ensures, because PlaceBuildPiece
 * called SpawnBrickForPiece BEFORE FStructure::AddPiece got to refuse the non-finite centre, so an
 * ABrickActor was spawned at a NaN transform and then destroyed (`NewTransform.IsValid()`,
 * `!NewTransform.ContainsNaN()`, `bIsValid` in SQVisitor). The fix that row drove is a guard in
 * ComputeBuildPlacement that leaves bDecided FALSE for a non-finite CHOSEN centre, so both doors
 * fail closed before anything is spawned: the ghost query returns a default FBuildPreview (bValid
 * false, bGrounded false — a pose no click can commit must not preview) and the commit returns a
 * default ref with no actor. THAT GUARD IS WHAT THIS TEST BITES ON NOW: remove it and the ensures
 * come back. The `<=` polarity beneath it is no longer reachable by a NaN pose (the guard refuses
 * the pose first), so it is defence in depth, protected by the reasoning here and in TRAPS rather
 * than by a discriminating row — a pose that is finite cannot tell `<=` from `!(>)`. Measured, not
 * assumed: with the guard in place, swapping the `<=` for `!(> )` leaves the whole BuildMode group
 * green (dev-expert, 2026-09-15).
 *
 * WHY THE HOUSE `!(x > y)` FORM IS WRONG HERE, and this is the whole point of the row. Everywhere
 * else in this codebase a guard is written `!(x > y)` precisely so a NaN lands INSIDE it — because
 * everywhere else, inside the guard is the safe side. Grounded inverts that: a grounded piece is
 * one the earth holds up, so it terminates load, absorbs anything stacked on it and CAN NEVER
 * FALL. That makes `true` the fail-OPEN answer. Since every comparison against NaN is false,
 * `!(bottom > 1.0)` evaluates to TRUE for a NaN pose — a brick nobody could place, credited with
 * the earth, immovable, and a lie inherited by every piece bonded above it. The `<=` production
 * uses evaluates to FALSE for a NaN, so garbage arithmetic yields a piece that is NOT grounded and
 * the solver may drop it. Fail closed means NOT GROUNDED here.
 *
 * BOTH DOORS, because both must answer the same thing about the same pose: the ghost query
 * (PreviewBuildPiece -> bGrounded) is what the player sees before the click, and the commit
 * (PlaceBuildPiece -> FStructurePiece::bIsGrounded) is what the solver reads afterwards. A preview
 * that says "not grounded" over a commit that stores grounded would be the worst of both.
 *
 * THE COMMIT LEG IS WRITTEN "IF IT LANDS A PIECE AT ALL". A NaN centre is refused deeper down —
 * FStructure::AddPiece rejects a non-finite centre of mass outright — so the outcome today is a
 * default ref and an EMPTY structure, and the assertion is the invariant that survives either
 * behaviour: no piece in the binding may read grounded. It is deliberately not "the place must
 * refuse", because refusing is AddPiece's guarantee (pinned by its own tests) and not this rule's;
 * if a later slice makes a non-finite pose placeable, this row must still hold.
 *
 * FREE PLACEMENT, ON AN EMPTY BUILD, so the requested pose is honoured verbatim and reaches the
 * grounded derivation unchanged. Under Snap with neighbours the solver could substitute a finite
 * candidate pose and the row would quietly stop testing a NaN at all.
 *
 * TWO NON-FINITE SHAPES. +Inf is the honest degenerate companion (a brick infinitely high is not
 * grounded, and the guard refuses it just as it refuses a NaN — `ContainsNaN` alone would let it
 * through, which is why the guard also tests `IsFinite` per component). The NaN row is the one
 * that would have discriminated the two comparison spellings had the pose reached the comparison.
 * -Inf is NOT swept: a bottom face infinitely far BELOW the earth is refused by the same guard, and
 * asserting it would only repeat the +Inf row.
 *
 * NEEDS A TICKING WORLD: a real world, because these are the world-layer subsystem doors and the
 * commit spawns an actor — but it never ticks. Every assertion is on the MECHANISM (the grounded
 * flag and the piece count), never displacement.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildModeNonFinitePoseNeverCommitsGroundedTest,
	"DestructionGame.World.BuildMode.NonFinitePoseNeverCommitsGrounded",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildModeNonFinitePoseNeverCommitsGroundedTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildPlacePieceTestSupport;
	using namespace DestructionProfiles;
	using namespace DestructionSession;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	struct FNonFiniteCase
	{
		const TCHAR* Description;
		double CentreZCm;
	};

	const FNonFiniteCase Cases[] = {
		{ TEXT("a NaN Z (the row that discriminates <= from !(> ))"),
			std::numeric_limits<double>::quiet_NaN() },
		{ TEXT("a +infinity Z"), std::numeric_limits<double>::infinity() },
	};

	for (const FNonFiniteCase& Case : Cases)
	{
		/* Its own EMPTY build, so the Free candidate is the requested pose and nothing else. */
		const int32 StructureId = Subsystem.BeginBuild();

		const FVector RequestedCentre(50.0, 0.0, Case.CentreZCm);

		/* THE GHOST QUERY. */
		const FBuildPreview Preview = Subsystem.PreviewBuildPiece(
			StructureId, RequestedCentre, HalfBrick, ClayBrick, EPlacementMode::Free);

		TestFalse(
			FString::Printf(
				TEXT("%s: the preview must NOT read grounded — a non-finite bottom face is not on the earth, and a grounded piece can never fall"),
				Case.Description),
			Preview.bGrounded);

		/* THE COMMIT. */
		const FPieceRef Ref = Subsystem.PlaceBuildPiece(
			StructureId, RequestedCentre, HalfBrick, ClayBrick, EPlacementMode::Free);

		const FStructureBinding* Binding = Subsystem.Find(StructureId);
		if (Binding == nullptr)
		{
			AddError(FString::Printf(
				TEXT("fixture: %s lost its structure across the commit"), Case.Description));
			continue;
		}

		/*
		 * NO PIECE IN THE BINDING MAY READ GROUNDED. The loop covers both outcomes: zero pieces
		 * (today's refusal, deep in AddPiece) and a landed piece, whose message names the ref so
		 * the log says which of the two happened.
		 */
		for (int32 PieceIndex = 0; PieceIndex < Binding->NumPieces(); ++PieceIndex)
		{
			TestFalse(
				FString::Printf(
					TEXT("%s: it committed piece %d (ref index %d) — a piece landed from a non-finite pose must NOT be recorded grounded"),
					Case.Description, PieceIndex, Ref.PieceIndex),
				Binding->GetStructure().GetPiece(PieceIndex).bIsGrounded);
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
