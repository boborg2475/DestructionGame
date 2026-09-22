// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Corbel.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

// Named namespace: a unity build merges files into one translation unit.
namespace StructurePushTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * BrickWorldTestSupport::NarrowWaistWallSpec(4). Everything above course 0 reaches the ground
	 * only through the course-1 brick.
	 *
	 *      course 3            [ 5 ]              rests on 3 and 4
	 *      course 2         [ 3 ][ 4 ]            head joint 3-4 between them
	 *      course 1            [ 2 ]              the waist, removed by the test
	 *      course 0         [ 0 ][ 1 ]            grounded
	 */

	/** Fixture preconditions: 6 pieces; head joints 0-1, 3-4 and six bed joints. */
	constexpr int32 NarrowWallPieceCount = 6;
	constexpr int32 NarrowWallJointCount = 8;

	constexpr int32 WaistPiece = 2;

	/**
	 * Pieces that lose their path to the ground when the waist goes. 3 and 4 name each other
	 * across the head joint, but LoadPaths holds only supports that reach the ground, so both read
	 * Falling, not a cycle. 5 falls with them. None is Stranded, which would be a solver
	 * limitation rather than a collapse (see CURRENT_STATE.md).
	 */
	constexpr bool bOrphanedByRemovingTheWaist[NarrowWallPieceCount] =
	{
		false, false, false, true, true, true
	};

	constexpr int32 ExpectedReleaseCount = 3;

	/*
	 * The orphans drop one course, 8.5 cm, clear of FallenAtLeastCm (5 cm, five times the 1 cm
	 * mortar settle).
	 */

	/*
	 * Four walls that stand and one structure that cannot. Under the no-tension partial-contact rule
	 * (Core/ConnectionStrength.cpp) a dry ragged wall stands. Its top end brick is a corbel on a
	 * 10.25 x 10.25 cm patch carrying F = 1.5 brick weights at e = 3.75 cm, past the kern (1.71)
	 * but inside the face (5.125). It cracks and bears on L_c = 3(h/2 - e) = 4.125 cm, reading
	 * 0.000631 of dry stone's 30 MPa.
	 *
	 * An even-course finish leaves a top corbel at e = 5.625 > h/2, which the per-joint router reads
	 * as infinite, but the LP (authority below 200 blocks) finds equilibrium by leaning it on its
	 * neighbour. Measured: 22..26 course dry ragged walls shed nothing, so the over-capacity fixture
	 * is a bare corbel arm (DryCorbelArmSpec).
	 */

	inline FRunningBondSpec WallSpecOf(
		int32 CoursesHigh,
		int32 BricksPerCourse,
		EWallEnd End,
		const FConnectionStrength& Strength)
	{
		FRunningBondSpec Spec;
		Spec.BrickSizeCm = FVector(21.5, 10.25, 6.5);
		Spec.JointThicknessCm = 1.0;
		Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
		Spec.CoursesHigh = CoursesHigh;
		Spec.BricksPerCourse = BricksPerCourse;
		Spec.End = End;
		Spec.Strength = Strength;
		return Spec;
	}

	/*
	 * Six wide since the corbel is local to each end. 24 courses gives an odd finish, so each top
	 * corbel has one brick on it (F = 1.5, e = 3.75 cm). 132 pieces, so the LP is the authority.
	 */
	constexpr int32 OverCapacityWallCourses = 24;
	constexpr int32 OverCapacityWallBricksPerCourse = 6;

	/** 12 even courses of 6 plus 12 odd courses of 5. */
	constexpr int32 OverCapacityWallPieceCount = 12 * 6 + 12 * 5;

	/** Toothed ends, dry joints. It stands. */
	inline FRunningBondSpec DryRaggedWallSpec(int32 CoursesHigh, int32 BricksPerCourse)
	{
		return WallSpecOf(CoursesHigh, BricksPerCourse, EWallEnd::Ragged, DryStone);
	}

	/** The game mode's own scenario wall: 40 courses of 30, flush, mortared. */
	inline FRunningBondSpec ScenarioWallSpec()
	{
		return WallSpecOf(40, 30, EWallEnd::Flush, GeneralPurposeMortar);
	}

	// Brick plus joint; half the brick pitch is the bond offset.
	constexpr double CoursePitchCm = 6.5 + 1.0;
	constexpr double BrickPitchCm = 21.5 + 1.0;

	/** 1.9 g/cm3 x 21.5 x 10.25 x 6.5 / 1000 = 2.72163125 kg, x 980 cm/s2. */
	constexpr double FullBrickWeightUu = 1.9 * 21.5 * 10.25 * 6.5 / 1000.0 * 980.0;

	/** Force units per MPa per cm2, not imported, so a production factor off by 100x fails here (DESIGN.md 3). */
	constexpr double ForceUnitsPerMPaSqCmHere = 10000.0;

	/** The half-brick bed patch a corbel keeps, 10.25 x 10.25 cm. */
	constexpr double CorbelBedAreaSqCm = 10.25 * 10.25;

	/** Patch depth h in the bending direction, cm. */
	constexpr double CorbelBedDepthCm = 10.25;

	/** (4/3) x h_u x h_v^2, cm3; equals t h^2 / 6 for a square patch. */
	constexpr double CorbelSectionModulusCm3 =
		(4.0 / 3.0) * (10.25 / 2.0) * (10.25 / 2.0) * (10.25 / 2.0);

	/** The brick keeps half a cell, so its centre is 22.5 / 4 out. */
	constexpr double CorbelOwnWeightArmCm = 22.5 / 4.0;

	/** Dry stone: no bond, f_c = 30 MPa. */
	constexpr double DryStoneTensileMPa = 0.0;
	constexpr double DryStoneCompressiveMPa = 30.0;

	/** Mean flexural bond f_x1 for general-purpose mortar (Gooch et al. 2023). Bonded, so no no-tension relief. */
	constexpr double MortarTensileMPa = 0.70;

	/** Force and moment on the top corbel's one bed joint, in brick weights. */
	constexpr double TopCorbelForceBrickWeights = 1.5;
	constexpr double TopCorbelMomentBrickWeightCm = CorbelOwnWeightArmCm;

	/** e = |M|/|F| = 3.75 cm, past the kern (1.71), inside the face (5.125). */
	constexpr double TopCorbelArmCm =
		TopCorbelMomentBrickWeightCm / TopCorbelForceBrickWeights;

	// Cracked-and-bearing reading by hand. Equals the linear peak at e = h/6; diverges as e -> h/2.
	constexpr double TopCorbelContactLengthCm =
		3.0 * (CorbelBedDepthCm / 2.0 - TopCorbelArmCm);

	constexpr double TopCorbelReducedContactStressMPa =
		2.0 * TopCorbelForceBrickWeights * FullBrickWeightUu
		/ (CorbelBedDepthCm * TopCorbelContactLengthCm)
		/ ForceUnitsPerMPaSqCmHere;

	/** 0.00063082303: compression on reduced contact. */
	constexpr double DryRaggedTopCorbelUtilisation =
		TopCorbelReducedContactStressMPa / DryStoneCompressiveMPa;

	// The same joint in mortar, uncracked: peak tension = |M|/W_v - N/A.
	constexpr double TopCorbelTensileStressMPa =
		FullBrickWeightUu
		* (TopCorbelMomentBrickWeightCm / CorbelSectionModulusCm3
			- TopCorbelForceBrickWeights / CorbelBedAreaSqCm)
		/ ForceUnitsPerMPaSqCmHere;

	/** 0.0065014926: the mortared ragged wall's worst joint, tension over f_x1 = 0.70. */
	constexpr double MortarRaggedWorstAsBuilt = 0.0065014926;

	/*
	 * Scenario wall: flush, so e = 0 and compression governs. 1220 pieces is over the 200-block cap,
	 * so the router is its authority.
	 */
	constexpr double ScenarioWorstAsBuilt = 0.00495042219;
	constexpr int32 ScenarioWallPieceCount = 1220;

	/** A dry flush wall has no corbel: e = 0, no tension, it stands. */
	constexpr double DryFlushWorstAsBuilt = 0.000971218165;

	/** 12 courses of 6, plus 12 courses of 5 bricks and 2 half bats. */
	constexpr int32 DryFlushWallPieceCount = 12 * 6 + 12 * 7;

	/*
	 * Bare corbel arm: a grounded base, then single bricks each one half-cell out and one course up,
	 * with nothing inboard or beside to lean on. The bed resultant sits at e = 5.625 cm, past the face
	 * (5.125), so there is no equilibrium and the LP sheds the arm on spawn. Measured: 16 pieces,
	 * 19 joints, all 10 arm bed joints over capacity, none Stranded.
	 */
	constexpr int32 CorbelArmSteps = 10;
	constexpr int32 CorbelArmBaseCourses = 3;
	constexpr int32 CorbelArmBaseCells = 2;

	constexpr int32 CorbelArmBaseCount = CorbelArmBaseCells * CorbelArmBaseCourses;
	constexpr int32 CorbelArmPieceCount = CorbelArmBaseCount + CorbelArmSteps;

	/** The whole arm sheds; the base stands. */
	constexpr int32 CorbelArmSheddingCount = CorbelArmSteps;

	inline DestructionCorbel::FCorbelSpec DryCorbelArmSpec()
	{
		DestructionCorbel::FCorbelSpec Spec;
		Spec.Strength = DryStone;
		Spec.bFilled = false;
		Spec.Steps = CorbelArmSteps;
		Spec.BaseCourses = CorbelArmBaseCourses;
		Spec.BaseCells = CorbelArmBaseCells;
		return Spec;
	}

	/** Course index, from the box's height. */
	inline int32 CourseOf(const FPieceBox& Box)
	{
		return FMath::RoundToInt32((Box.CentreCm.Z - 6.5 * 0.5) / CoursePitchCm);
	}

	/** Oracle for the arm: a piece survives iff it is in the base courses. Independent of emit order. */
	inline bool CorbelArmPieceSurvives(const FPieceBox& Box)
	{
		return CourseOf(Box) < CorbelArmBaseCourses;
	}

	/**
	 * Minimum move to count as handed to physics, not a fall distance. A kinematic brick moves
	 * exactly 0 cm (measured); the smallest released move is logged each run.
	 */
	constexpr double ReleasedMustMoveCm = 0.01;

	/**
	 * Time for the 132-brick wall to finish falling, measured. The whole wall lets go at once, so it
	 * compacts then topples rather than free-falling: centre-of-mass Z reads 92.434, 75.480, 1.966,
	 * then 1.881 from 4 s on. The test ticks one more second and asserts nothing changed. A fixed
	 * duration, not a settle poll, so a failure is an assertion rather than a timeout.
	 */
	constexpr double CollapseSeconds = 3.0;

	/** World-space centre. SM_Cube's pivot is a corner, which moves when a brick rotates. */
	inline FVector BrickCentreCm(const ABrickActor& Brick)
	{
		return Brick.GetMesh()->Bounds.Origin;
	}

	/**
	 * Second push while bricks are still falling (0.05 s, about -49 cm/s). Idempotence is checked by
	 * velocity surviving the call, since SetSimulatePhysics(true) on a simulating body would recreate
	 * it at rest while still "simulating". By 0.25 s they have landed and velocity is 0 either way.
	 */
	constexpr double SecondPushAtSeconds = 0.05;
	constexpr double RemainingFallSeconds = 0.95;

	/** Well under the -49 cm/s three steps of gravity give, and well clear of zero. */
	constexpr double FallingFasterThanCmPerSecond = -20.0;

	const TCHAR* SupportName(EPieceSupport Support)
	{
		switch (Support)
		{
		case EPieceSupport::Grounded:  return TEXT("Grounded");
		case EPieceSupport::Supported: return TEXT("Supported");
		case EPieceSupport::Stranded:  return TEXT("Stranded");
		default:                       return TEXT("Falling");
		}
	}

	/** Logs every piece's support state on one line. */
	void ReportSupport(FAutomationTestBase& Test, const FStructureBinding& Binding, const TCHAR* When)
	{
		FString Line;

		for (int32 Piece = 0; Piece < Binding.NumPieces(); ++Piece)
		{
			Line += FString::Printf(
				TEXT("%s%d=%s%s"),
				Piece == 0 ? TEXT("") : TEXT(", "),
				Piece,
				SupportName(Binding.GetStructure().GetPieceSupport(Piece)),
				Binding.IsPieceRemoved(Piece) ? TEXT("(removed)") : TEXT(""));
		}

		Test.AddInfo(FString::Printf(TEXT("support %s: %s"), When, *Line));
	}
}

/**
 * A standing wall is solved and pushed and nothing comes down. The control for the collapse test:
 * "release everything" would pass that one but fails here (mutation-proved).
 *
 * Also pins that a push with no solve releases nothing: before any solve every piece reads Falling,
 * the same as an absent answer, so ApplyResults refuses to act. An unknown id releases nothing.
 * Needs a ticking world, since unticked kinematic actors stand regardless.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructurePushStandingWallReleasesNothingTest,
	"DestructionGame.World.Push.StandingWallReleasesNothing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructurePushStandingWallReleasesNothingTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace StructurePushTestSupport;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	// Nothing built yet, so every id is unknown, including the next one to be handed out.
	TestEqual(
		TEXT("SolveAndPush on a subsystem holding no structures at all must release nothing"),
		TestWorld.Subsystem->SolveAndPush(0), 0);

	const int32 StructureId = TestWorld.Subsystem->BuildRunningBond(NarrowWaistWallSpec(4));
	FStructureBinding* Binding = TestWorld.Subsystem->Find(StructureId);

	TestNotNull(
		*FString::Printf(TEXT("fixture: BuildRunningBond returned %d and Find should hand back its binding"),
			StructureId),
		Binding);

	if (Binding == nullptr)
	{
		TestWorld.End();
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("fixture: the narrow wall should span %d handles, got %d"),
			NarrowWallPieceCount, Binding->NumPieces()),
		Binding->NumPieces(), NarrowWallPieceCount);

	TestEqual(
		FString::Printf(TEXT("fixture: the narrow wall should carry %d joints, got %d"),
			NarrowWallJointCount, Binding->GetStructure().NumConnections()),
		Binding->GetStructure().NumConnections(), NarrowWallJointCount);

	if (Binding->NumPieces() != NarrowWallPieceCount)
	{
		TestWorld.End();
		return true;
	}

	TestEqual(
		TEXT("a push with no solve behind it must release nothing: no answer is not an instruction"),
		Binding->ApplyResults(), 0);

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
		TEXT("SolveAndPush on a wall that is standing must release nothing"),
		TestWorld.Subsystem->SolveAndPush(StructureId), 0);

	ReportSupport(*this, *Binding, TEXT("after the first push"));

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		const EPieceSupport Support = Binding->GetStructure().GetPieceSupport(Piece);

		TestTrue(
			FString::Printf(TEXT("piece %d in a standing wall should be held up, the solver says %s"),
				Piece, SupportName(Support)),
			Support == EPieceSupport::Grounded || Support == EPieceSupport::Supported);

		TestTrue(
			FString::Printf(TEXT("piece %d must not be released by a push on a standing wall"), Piece),
			!Binding->IsReleased(Piece));

		TestTrue(
			FString::Printf(TEXT("brick %d must still be kinematic after a push on a standing wall"), Piece),
			Bricks[Piece]->GetMesh() != nullptr && !Bricks[Piece]->GetMesh()->IsSimulatingPhysics());
	}

	// Re-check the wall afterwards: a push that ignored its id would return zero yet release the wall.
	TestEqual(
		TEXT("SolveAndPush(INDEX_NONE) must release nothing"),
		TestWorld.Subsystem->SolveAndPush(INDEX_NONE), 0);

	TestEqual(
		FString::Printf(TEXT("SolveAndPush(%d) names no structure and must release nothing"),
			StructureId + 1),
		TestWorld.Subsystem->SolveAndPush(StructureId + 1), 0);

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		TestTrue(
			FString::Printf(TEXT("piece %d must survive a push aimed at an id that names nothing"), Piece),
			!Binding->IsReleased(Piece));
	}

	TestWorld.TickSeconds(1.0);

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		const double DriftCm = FVector::Dist(Bricks[Piece]->GetActorLocation(), LaidAt[Piece]);

		TestTrue(
			FString::Printf(
				TEXT("brick %d must not move in a second of gravity after a push that released nothing; it moved %.6f cm"),
				Piece, DriftCm),
			DriftCm < DriftToleranceCm);

		TestTrue(
			FString::Printf(TEXT("brick %d must still be kinematic a second later"), Piece),
			Bricks[Piece]->GetMesh() != nullptr && !Bricks[Piece]->GetMesh()->IsSimulatingPhysics());
	}

	TestEqual(
		TEXT("a second SolveAndPush on the same standing wall must release nothing"),
		TestWorld.Subsystem->SolveAndPush(StructureId), 0);

	TestWorld.End();

	return true;
}

/**
 * Removing the waist releases exactly the orphaned pieces, and they fall. Asserted per handle, not
 * by count, so releasing the wrong three bricks fails. Both the release flag and physics simulation
 * are asserted (DESIGN.md §4), and supported bricks must not move, so a world falling through the
 * floor fails. The test destroys the waist's actor itself, since RemovePiece does not; left in
 * place the orphans would land on it after 1 cm.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructurePushOrphanedPiecesFallTest,
	"DestructionGame.World.Push.LosingASupportDropsExactlyTheOrphans",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructurePushOrphanedPiecesFallTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace StructurePushTestSupport;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	const int32 StructureId = TestWorld.Subsystem->BuildRunningBond(NarrowWaistWallSpec(4));
	FStructureBinding* Binding = TestWorld.Subsystem->Find(StructureId);

	TestNotNull(
		*FString::Printf(TEXT("fixture: BuildRunningBond returned %d and Find should hand back its binding"),
			StructureId),
		Binding);

	if (Binding == nullptr)
	{
		TestWorld.End();
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("fixture: the narrow wall should span %d handles, got %d"),
			NarrowWallPieceCount, Binding->NumPieces()),
		Binding->NumPieces(), NarrowWallPieceCount);

	TestEqual(
		FString::Printf(TEXT("fixture: the narrow wall should carry %d joints, got %d"),
			NarrowWallJointCount, Binding->GetStructure().NumConnections()),
		Binding->GetStructure().NumConnections(), NarrowWallJointCount);

	if (Binding->NumPieces() != NarrowWallPieceCount)
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
		TEXT("fixture: the wall as built should stand, so the first push releases nothing"),
		TestWorld.Subsystem->SolveAndPush(StructureId), 0);

	ReportSupport(*this, *Binding, TEXT("as built"));

	// Capture the actor first: RemovePiece clears the binding's pointer to it.
	ABrickActor* WaistBrick = Bricks[WaistPiece];

	TestTrue(
		FString::Printf(TEXT("fixture: removing piece %d should report a live piece removed"), WaistPiece),
		Binding->RemovePiece(WaistPiece));

	WaistBrick->Destroy();
	Bricks[WaistPiece] = nullptr;

	const int32 ReleasedCount = TestWorld.Subsystem->SolveAndPush(StructureId);

	ReportSupport(*this, *Binding, TEXT("after the waist was removed"));

	TestEqual(
		FString::Printf(
			TEXT("removing the waist should release the %d pieces above it, SolveAndPush released %d"),
			ExpectedReleaseCount, ReleasedCount),
		ReleasedCount, ExpectedReleaseCount);

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		if (Piece == WaistPiece)
		{
			// A removed piece has no actor, so it is never released.
			TestTrue(
				TEXT("the removed waist piece must not be released"),
				!Binding->IsReleased(Piece));

			TestNull(
				TEXT("the removed waist piece must have no actor left in its binding"),
				Binding->GetActor(Piece));

			continue;
		}

		const bool bExpected = bOrphanedByRemovingTheWaist[Piece];
		const EPieceSupport Support = Binding->GetStructure().GetPieceSupport(Piece);

		TestTrue(
			FString::Printf(
				TEXT("piece %d should%s be released once the waist has gone; the solver says %s and IsReleased is %s"),
				Piece, bExpected ? TEXT("") : TEXT(" not"), SupportName(Support),
				Binding->IsReleased(Piece) ? TEXT("true") : TEXT("false")),
			Binding->IsReleased(Piece) == bExpected);

		TestTrue(
			FString::Printf(
				TEXT("piece %d must not be Stranded: that would make this a solver limitation rather than a collapse"),
				Piece),
			Support != EPieceSupport::Stranded);

		if (bExpected)
		{
			TestTrue(
				FString::Printf(TEXT("orphaned piece %d should read Falling, the solver says %s"),
					Piece, SupportName(Support)),
				Support == EPieceSupport::Falling);
		}

		// The flag reached the actor: catches a push that set flags but never called physics.
		const bool bSimulating =
			Bricks[Piece]->GetMesh() != nullptr && Bricks[Piece]->GetMesh()->IsSimulatingPhysics();

		TestTrue(
			FString::Printf(
				TEXT("brick %d should%s be simulating physics once the waist has gone; it is %s"),
				Piece, bExpected ? TEXT("") : TEXT(" not"), bSimulating ? TEXT("simulating") : TEXT("kinematic")),
			bSimulating == bExpected);
	}

	TestWorld.TickSeconds(SecondPushAtSeconds);

	TArray<FVector> VelocityBefore;

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		VelocityBefore.Add(
			Bricks[Piece] != nullptr && Bricks[Piece]->GetMesh() != nullptr
				? Bricks[Piece]->GetMesh()->GetPhysicsLinearVelocity()
				: FVector::ZeroVector);

		if (Piece == WaistPiece || !bOrphanedByRemovingTheWaist[Piece])
		{
			continue;
		}

		// Precondition: 0 against 0 would prove nothing.
		TestTrue(
			FString::Printf(
				TEXT("fixture: released brick %d should be moving after %.2f s, velocity Z is %.3f cm/s"),
				Piece, SecondPushAtSeconds, VelocityBefore[Piece].Z),
			VelocityBefore[Piece].Z < FallingFasterThanCmPerSecond);
	}

	TestEqual(
		TEXT("a second SolveAndPush with nothing changed must release nothing"),
		TestWorld.Subsystem->SolveAndPush(StructureId), 0);

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		if (Piece == WaistPiece || !bOrphanedByRemovingTheWaist[Piece])
		{
			continue;
		}

		// Nothing ticked between reads, so a push that re-creates the body would zero this velocity.
		const FVector VelocityAfter = Bricks[Piece]->GetMesh()->GetPhysicsLinearVelocity();

		TestTrue(
			FString::Printf(
				TEXT("a second push must not disturb falling brick %d: velocity was (%.3f, %.3f, %.3f), now (%.3f, %.3f, %.3f)"),
				Piece,
				VelocityBefore[Piece].X, VelocityBefore[Piece].Y, VelocityBefore[Piece].Z,
				VelocityAfter.X, VelocityAfter.Y, VelocityAfter.Z),
			VelocityAfter.Equals(VelocityBefore[Piece], 1.0e-3));
	}

	TestWorld.TickSeconds(RemainingFallSeconds);

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		if (Piece == WaistPiece)
		{
			continue;
		}

		const double FellCm = LaidAt[Piece].Z - Bricks[Piece]->GetActorLocation().Z;
		const double MovedCm = FVector::Dist(Bricks[Piece]->GetActorLocation(), LaidAt[Piece]);

		if (bOrphanedByRemovingTheWaist[Piece])
		{
			AddInfo(FString::Printf(
				TEXT("released brick %d fell %.3f cm in one second, from Z %.3f to Z %.3f"),
				Piece, FellCm, LaidAt[Piece].Z, Bricks[Piece]->GetActorLocation().Z));

			TestTrue(
				FString::Printf(
					TEXT("released brick %d should have fallen more than %.1f cm in a second; it dropped %.3f cm"),
					Piece, FallenAtLeastCm, FellCm),
				FellCm > FallenAtLeastCm);
		}
		else
		{
			// Without this, a world that fell through the floor would pass the row above.
			TestTrue(
				FString::Printf(
					TEXT("brick %d is still held up and must not have moved; it drifted %.6f cm"),
					Piece, MovedCm),
				MovedCm < DriftToleranceCm);

			TestTrue(
				FString::Printf(TEXT("brick %d is still held up and must still be kinematic"), Piece),
				Bricks[Piece]->GetMesh() != nullptr && !Bricks[Piece]->GetMesh()->IsSimulatingPhysics());
		}
	}

	TestWorld.End();

	return true;
}

/**
 * An over-capacity structure comes down on build, not on the next unrelated click, and a standing
 * one is left alone. Guards the SolveAndBreak inside SolveAndPush. The subject is a bare corbel arm
 * since running-bond walls now shed nothing; the dry ragged wall is the standing control.
 *
 * Released arm bricks must move and the base and wall must not (DESIGN.md 4). The governing axis is
 * asserted (dry corbel: compression on reduced contact; mortared: tension). Solver answers are
 * established world-free first, so the rest tests only the wire.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FStructurePushOverCapacityWallSettlesOnBuildTest,
	"DestructionGame.World.Push.AWallOverCapacityDoesNotWaitForAClick",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FStructurePushOverCapacityWallSettlesOnBuildTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;
	using namespace StructurePushTestSupport;

	const auto WorstJointOf = [](const FStructure& Structure, int32& OutJoint)
	{
		double Worst = 0.0;
		OutJoint = INDEX_NONE;

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			const double Utilisation = Structure.GetConnectionUtilisation(Joint);

			if (Utilisation > Worst)
			{
				Worst = Utilisation;
				OutJoint = Joint;
			}
		}

		return Worst;
	};

	// World-free load model first.

	// The dry ragged wall stands.
	{
		FBrickLayout Dry;

		TestTrue(TEXT("fixture: the producer should lay the dry ragged wall"),
			RunningBond(
				DryRaggedWallSpec(OverCapacityWallCourses, OverCapacityWallBricksPerCourse), Dry));

		TestEqual(
			FString::Printf(TEXT("fixture: the dry ragged wall should be %d pieces"),
				OverCapacityWallPieceCount),
			Dry.Structure.NumPieces(), OverCapacityWallPieceCount);

		// The fixture depends on these profile values, so assert them.
		TestEqual(
			FString::Printf(TEXT("fixture: DryStone's tensile strength must be an exact zero, it is %g MPa"),
				DryStone.TensileStrengthMPa),
			DryStone.TensileStrengthMPa, DryStoneTensileMPa);

		TestEqual(
			FString::Printf(TEXT("fixture: DryStone's compressive strength must be %g MPa, it is %g"),
				DryStoneCompressiveMPa, DryStone.CompressiveStrengthMPa),
			DryStone.CompressiveStrengthMPa, DryStoneCompressiveMPa);

		TestEqual(
			FString::Printf(TEXT("fixture: GeneralPurposeMortar's f_x1 must be %g MPa, it is %g"),
				MortarTensileMPa, GeneralPurposeMortar.TensileStrengthMPa),
			GeneralPurposeMortar.TensileStrengthMPa, MortarTensileMPa);

		Dry.Structure.SolveLoads();

		// Find the top corbel by geometry: end brick of the highest even course and the brick under it.
		const int32 TopCorbelCourse =
			OverCapacityWallCourses % 2 == 0 ? OverCapacityWallCourses - 2 : OverCapacityWallCourses - 1;

		int32 TopCorbel = INDEX_NONE;
		int32 TopCorbelSupport = INDEX_NONE;

		for (int32 Piece = 0; Piece < Dry.Boxes.Num(); ++Piece)
		{
			const double CentreXCm = Dry.Boxes[Piece].CentreCm.X;
			const double CentreZCm = Dry.Boxes[Piece].CentreCm.Z;

			if (FMath::IsNearlyEqual(CentreZCm, 6.5 * 0.5 + TopCorbelCourse * CoursePitchCm, 0.01)
				&& FMath::IsNearlyEqual(CentreXCm, 0.0, 0.01))
			{
				TopCorbel = Piece;
			}

			if (FMath::IsNearlyEqual(CentreZCm, 6.5 * 0.5 + (TopCorbelCourse - 1) * CoursePitchCm, 0.01)
				&& FMath::IsNearlyEqual(CentreXCm, BrickPitchCm * 0.5, 0.01))
			{
				TopCorbelSupport = Piece;
			}
		}

		int32 TopCorbelJoint = INDEX_NONE;

		for (int32 Joint = 0; Joint < Dry.Structure.NumConnections(); ++Joint)
		{
			const FConnection& Candidate = Dry.Structure.GetConnection(Joint);

			if ((Candidate.PieceA == TopCorbel && Candidate.PieceB == TopCorbelSupport)
				|| (Candidate.PieceA == TopCorbelSupport && Candidate.PieceB == TopCorbel))
			{
				TopCorbelJoint = Joint;
			}
		}

		if (TopCorbelJoint == INDEX_NONE)
		{
			AddError(FString::Printf(
				TEXT("fixture: course %d should have an end brick at X 0 resting on one at X %g; the pieces resolved to %d and %d"),
				TopCorbelCourse, BrickPitchCm * 0.5, TopCorbel, TopCorbelSupport));

			return true;
		}

		// Only the corbel's own weight has a lever arm, giving M = 5.625 brick-weight-cm.
		const double CorbelForceUu = Dry.Structure.GetConnectionForce(TopCorbelJoint).Size();
		const double CorbelMomentUuCm = Dry.Structure.GetConnectionMoment(TopCorbelJoint).Size();
		const double CorbelArmCm = CorbelForceUu > 0.0 ? CorbelMomentUuCm / CorbelForceUu : 0.0;

		AddInfo(FString::Printf(
			TEXT("the dry top corbel (course %d, joint %d) carries %.6f W and %.6f W.cm, arm %.6f cm, and reads %.9g of capacity"),
			TopCorbelCourse, TopCorbelJoint,
			CorbelForceUu / FullBrickWeightUu, CorbelMomentUuCm / FullBrickWeightUu, CorbelArmCm,
			Dry.Structure.GetConnectionUtilisation(TopCorbelJoint)));

		TestTrue(
			FString::Printf(TEXT("the top corbel joint must carry %.6f brick weights; it carries %.6f"),
				TopCorbelForceBrickWeights, CorbelForceUu / FullBrickWeightUu),
			FMath::IsNearlyEqual(CorbelForceUu, TopCorbelForceBrickWeights * FullBrickWeightUu, 1.0e-6));

		TestTrue(
			FString::Printf(TEXT("and %.6f brick-weight-centimetres of moment; it carries %.6f"),
				TopCorbelMomentBrickWeightCm, CorbelMomentUuCm / FullBrickWeightUu),
			FMath::IsNearlyEqual(CorbelMomentUuCm, TopCorbelMomentBrickWeightCm * FullBrickWeightUu, 1.0e-6));

		TestTrue(
			FString::Printf(TEXT("so the effective arm is |M|/|F| = %.9g cm, past the kern (1.708) and inside the face (5.125); it reads %.9g"),
				TopCorbelArmCm, CorbelArmCm),
			FMath::IsNearlyEqual(CorbelArmCm, TopCorbelArmCm, 1.0e-9));

		// Axis check: a joint failed at the retired kern rule would read the ~1.8e308 sentinel.
		TestTrue(
			FString::Printf(
				TEXT("the reduced-contact hand reading must come to %.9g; it comes to %.9g"),
				DryRaggedTopCorbelUtilisation, TopCorbelReducedContactStressMPa / DryStoneCompressiveMPa),
			FMath::IsNearlyEqual(
				TopCorbelReducedContactStressMPa / DryStoneCompressiveMPa,
				DryRaggedTopCorbelUtilisation, 1.0e-12));

		TestTrue(
			FString::Printf(
				TEXT("THE DRY TOP CORBEL MUST STAND ON ITS REDUCED CONTACT at %.9g, not fail at the kern; it reads %.9g"),
				DryRaggedTopCorbelUtilisation, Dry.Structure.GetConnectionUtilisation(TopCorbelJoint)),
			FMath::IsNearlyEqual(
				Dry.Structure.GetConnectionUtilisation(TopCorbelJoint),
				DryRaggedTopCorbelUtilisation, DryRaggedTopCorbelUtilisation * 1.0e-6));

		// Lower corbels carry more compression at the same arm, so none is over capacity.
		int32 OverCapacity = 0;
		int32 WorstJoint = INDEX_NONE;
		const double Worst = WorstJointOf(Dry.Structure, WorstJoint);

		for (int32 Joint = 0; Joint < Dry.Structure.NumConnections(); ++Joint)
		{
			if (Dry.Structure.GetConnectionUtilisation(Joint) > 1.0)
			{
				++OverCapacity;
			}
		}

		AddInfo(FString::Printf(
			TEXT("the dry ragged wall has %d of %d joints over capacity as built; the worst reads %.9g at joint %d"),
			OverCapacity, Dry.Structure.NumConnections(), Worst, WorstJoint));

		TestEqual(
			FString::Printf(TEXT("NO joint of the dry ragged wall may be over capacity; %d are"), OverCapacity),
			OverCapacity, 0);

		TestTrue(
			FString::Printf(TEXT("and its worst joint must be far under capacity; it reads %.9g"), Worst),
			Worst < 0.01);

		TestEqual(
			TEXT("settling the dry ragged wall must break nothing: the LP stands it entire"),
			Dry.Structure.SolveAndBreak(), 0);
	}

	// Mortared control: same geometry, bonded joints.
	{
		FBrickLayout Mortared;

		TestTrue(TEXT("fixture: the producer should lay the same wall mortared"),
			RunningBond(
				WallSpecOf(OverCapacityWallCourses, OverCapacityWallBricksPerCourse,
					EWallEnd::Ragged, GeneralPurposeMortar),
				Mortared));

		Mortared.Structure.SolveLoads();

		int32 MortaredWorstJoint = INDEX_NONE;
		const double MortaredWorst = WorstJointOf(Mortared.Structure, MortaredWorstJoint);

		TestTrue(
			FString::Printf(
				TEXT("the mortared corbel's hand tension reading must come to %.9g; it comes to %.9g"),
				MortarRaggedWorstAsBuilt, TopCorbelTensileStressMPa / MortarTensileMPa),
			FMath::IsNearlyEqual(
				TopCorbelTensileStressMPa / MortarTensileMPa, MortarRaggedWorstAsBuilt, 1.0e-9));

		TestTrue(
			FString::Printf(
				TEXT("the mortared wall's worst joint must be the corbel's tension at %.9g; it reads %.9g"),
				MortarRaggedWorstAsBuilt, MortaredWorst),
			FMath::IsNearlyEqual(MortaredWorst, MortarRaggedWorstAsBuilt, 1.0e-9));

		TestTrue(
			FString::Printf(TEXT("so a mortared ragged wall stands as built at %.9g"), MortaredWorst),
			MortaredWorst < 1.0);

		TestEqual(
			TEXT("and settling the mortared wall must break nothing at all"),
			Mortared.Structure.SolveAndBreak(), 0);
	}

	// Dry flush control: half bats fill the ends, so e = 0 and there is no corbel.
	{
		FBrickLayout DryFlush;

		TestTrue(TEXT("fixture: the producer should lay a dry FLUSH wall"),
			RunningBond(
				WallSpecOf(OverCapacityWallCourses, OverCapacityWallBricksPerCourse,
					EWallEnd::Flush, DryStone),
				DryFlush));

		TestEqual(
			FString::Printf(TEXT("fixture: the dry flush wall should be %d pieces"), DryFlushWallPieceCount),
			DryFlush.Structure.NumPieces(), DryFlushWallPieceCount);

		DryFlush.Structure.SolveLoads();

		int32 WorstJoint = INDEX_NONE;
		const double Worst = WorstJointOf(DryFlush.Structure, WorstJoint);

		TestTrue(
			FString::Printf(
				TEXT("a dry wall with flush ends stands: its worst joint should read %.9g, it reads %.9g"),
				DryFlushWorstAsBuilt, Worst),
			FMath::IsNearlyEqual(Worst, DryFlushWorstAsBuilt, DryFlushWorstAsBuilt * 1.0e-6));

		TestEqual(
			TEXT("so settling the dry flush wall must break nothing at all"),
			DryFlush.Structure.SolveAndBreak(), 0);
	}

	// The game mode's 40 x 30 scenario wall; over the block cap, so the router is its authority.
	{
		FBrickLayout Scenario;

		TestTrue(TEXT("fixture: the producer should lay the game mode's own scenario wall"),
			RunningBond(ScenarioWallSpec(), Scenario));

		TestEqual(
			FString::Printf(TEXT("fixture: the scenario wall should be %d pieces"), ScenarioWallPieceCount),
			Scenario.Structure.NumPieces(), ScenarioWallPieceCount);

		Scenario.Structure.SolveLoads();

		int32 WorstJoint = INDEX_NONE;
		const double Worst = WorstJointOf(Scenario.Structure, WorstJoint);

		TestTrue(
			FString::Printf(TEXT("the scenario wall must read %.9g of capacity, it reads %.9g"),
				ScenarioWorstAsBuilt, Worst),
			FMath::IsNearlyEqual(Worst, ScenarioWorstAsBuilt, ScenarioWorstAsBuilt * 1.0e-6));

		TestEqual(
			TEXT("settling the scenario wall must break nothing"),
			Scenario.Structure.SolveAndBreak(), 0);
	}

	// The bare dry corbel arm cannot stand. Nothing may be Stranded (a solver stall, not a collapse).
	{
		FBrickLayout Arm;

		TestTrue(TEXT("fixture: the corbel producer should lay the bare dry arm"),
			DestructionCorbel::Build(DryCorbelArmSpec(), Arm));

		TestEqual(
			FString::Printf(TEXT("fixture: the bare corbel arm should be %d pieces"), CorbelArmPieceCount),
			Arm.Structure.NumPieces(), CorbelArmPieceCount);

		Arm.Structure.SolveLoads();

		int32 OverCapacity = 0;

		for (int32 Joint = 0; Joint < Arm.Structure.NumConnections(); ++Joint)
		{
			if (Arm.Structure.GetConnectionUtilisation(Joint) > 1.0)
			{
				++OverCapacity;
			}
		}

		TestEqual(
			FString::Printf(
				TEXT("the router must read all %d arm bed joints over capacity as built; %d are"),
				CorbelArmSteps, OverCapacity),
			OverCapacity, CorbelArmSteps);

		const int32 Broke = Arm.Structure.SolveAndBreak();

		int32 Shed = 0;
		int32 Stranded = 0;
		int32 WrongFate = 0;

		for (int32 Piece = 0; Piece < Arm.Structure.NumPieces(); ++Piece)
		{
			const EPieceSupport Support = Arm.Structure.GetPieceSupport(Piece);
			const bool bShouldSurvive = CorbelArmPieceSurvives(Arm.Boxes[Piece]);

			if (Support == EPieceSupport::Falling)
			{
				++Shed;
			}

			if (Support == EPieceSupport::Stranded)
			{
				++Stranded;
			}

			if ((Support == EPieceSupport::Falling) == bShouldSurvive)
			{
				++WrongFate;
			}
		}

		AddInfo(FString::Printf(
			TEXT("the bare corbel arm: %d joints over capacity, SolveAndBreak severed %d, %d pieces shed, %d Stranded"),
			OverCapacity, Broke, Shed, Stranded));

		TestEqual(
			FString::Printf(TEXT("the LP must shed exactly the %d arm bricks; %d fell"),
				CorbelArmSheddingCount, Shed),
			Shed, CorbelArmSheddingCount);

		TestEqual(
			FString::Printf(TEXT("and every piece must meet the base/arm fate the producer predicts; %d did not"),
				WrongFate),
			WrongFate, 0);

		TestEqual(
			FString::Printf(TEXT("and NONE may be Stranded: that would be a solver stall, not a collapse; %d was"),
				Stranded),
			Stranded, 0);
	}

	// Now the wire: build each in a world and push once, with no click.
	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	// Standing control: stops "settle at build time" being satisfied by settling everything.
	{
		const int32 WallId = TestWorld.Subsystem->BuildRunningBond(
			DryRaggedWallSpec(OverCapacityWallCourses, OverCapacityWallBricksPerCourse));

		FStructureBinding* WallBinding = TestWorld.Subsystem->Find(WallId);

		TestNotNull(TEXT("fixture: the dry ragged wall should build in the world"), WallBinding);

		if (WallBinding == nullptr)
		{
			TestWorld.End();
			return true;
		}

		TestEqual(
			TEXT("building a wall that STANDS must settle nothing on spawn: SolveAndPush releases 0"),
			TestWorld.Subsystem->SolveAndPush(WallId), 0);

		for (int32 Piece = 0; Piece < WallBinding->NumPieces(); ++Piece)
		{
			TestTrue(
				FString::Printf(TEXT("no piece of the standing wall may be released; piece %d was"), Piece),
				!WallBinding->IsReleased(Piece));
		}
	}

	// The over-capacity arm, pushed once with no click: the wire the game mode runs on BeginPlay.
	const int32 ArmId = TestWorld.Subsystem->BuildLayout([&]
	{
		FBrickLayout Arm;
		DestructionCorbel::Build(DryCorbelArmSpec(), Arm);
		return Arm;
	}());

	FStructureBinding* Binding = TestWorld.Subsystem->Find(ArmId);

	TestNotNull(TEXT("fixture: the bare corbel arm should build in the world"), Binding);

	if (Binding == nullptr)
	{
		TestWorld.End();
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("fixture: the arm should span %d handles, got %d"),
			CorbelArmPieceCount, Binding->NumPieces()),
		Binding->NumPieces(), CorbelArmPieceCount);

	if (Binding->NumPieces() != CorbelArmPieceCount)
	{
		TestWorld.End();
		return true;
	}

	TArray<ABrickActor*> Bricks;
	TArray<FVector> LaidAt;
	TArray<FVector> LaidCentreCm;
	TArray<FPieceBox> Boxes;
	TArray<bool> ShouldSurvive;

	int32 ExpectedSurvivors = 0;

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		ABrickActor* Brick = BrickAt(*this, *Binding, Piece);

		if (Brick == nullptr || Brick->GetMesh() == nullptr)
		{
			AddError(FString::Printf(TEXT("fixture: brick %d has no mesh to measure"), Piece));
			TestWorld.End();
			return true;
		}

		Bricks.Add(Brick);
		LaidAt.Add(Brick->GetActorLocation());
		LaidCentreCm.Add(BrickCentreCm(*Brick));

		const FPieceBox& Box = Binding->GetBinding(Piece).Box;
		Boxes.Add(Box);
		ShouldSurvive.Add(CorbelArmPieceSurvives(Box));

		if (ShouldSurvive.Last())
		{
			++ExpectedSurvivors;
		}
	}

	const int32 ExpectedReleased = Binding->NumPieces() - ExpectedSurvivors;

	AddInfo(FString::Printf(
		TEXT("the producer predicts %d of %d arm pieces survive, so %d settle on spawn"),
		ExpectedSurvivors, Binding->NumPieces(), ExpectedReleased));

	const int32 Released = TestWorld.Subsystem->SolveAndPush(ArmId);

	TestEqual(
		FString::Printf(
			TEXT("building an arm that cannot hold itself up must settle it there and then: %d should release, %d did"),
			ExpectedReleased, Released),
		Released, ExpectedReleased);

	// A break pass stamp proves joints gave, not merely read over capacity.
	int32 BrokenJoints = 0;

	for (int32 Joint = 0; Joint < Binding->GetStructure().NumConnections(); ++Joint)
	{
		if (Binding->GetStructure().GetBreakPass(Joint) != INDEX_NONE)
		{
			++BrokenJoints;
		}
	}

	TestTrue(
		FString::Printf(TEXT("joints must have GIVEN, not merely read over capacity; %d carry a break pass"),
			BrokenJoints),
		BrokenJoints > 0);

	// Per piece against the producer's oracle, and the flag must match the physics body.
	int32 WrongFate = 0;
	int32 WrongBody = 0;
	int32 StrandedPieces = 0;

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		const EPieceSupport Support = Binding->GetStructure().GetPieceSupport(Piece);

		if (Binding->IsReleased(Piece) == ShouldSurvive[Piece])
		{
			++WrongFate;

			AddError(FString::Printf(
				TEXT("arm piece %d (course %d) should%s have been released; IsReleased is %s"),
				Piece, CourseOf(Boxes[Piece]),
				ShouldSurvive[Piece] ? TEXT(" not") : TEXT(""),
				Binding->IsReleased(Piece) ? TEXT("true") : TEXT("false")));
		}

		if (Support == EPieceSupport::Stranded)
		{
			++StrandedPieces;
		}

		const bool bSimulating =
			Bricks[Piece]->GetMesh() != nullptr && Bricks[Piece]->GetMesh()->IsSimulatingPhysics();

		if (bSimulating == ShouldSurvive[Piece])
		{
			++WrongBody;
		}
	}

	TestEqual(
		FString::Printf(TEXT("every piece must meet the fate the producer predicts; %d did not"), WrongFate),
		WrongFate, 0);

	TestEqual(
		FString::Printf(TEXT("and no piece may be Stranded; %d was"), StrandedPieces),
		StrandedPieces, 0);

	TestEqual(
		FString::Printf(TEXT("and every brick's body must agree with its binding; %d did not"), WrongBody),
		WrongBody, 0);

	/*
	 * Outcome, relational since tumbling bricks land anywhere: the released centre of mass ends
	 * lower (a 0/0 NaN fails the strict test) but above the floor, every released brick moved, and
	 * no base brick moved.
	 */
	double ReleasedMassKg = 0.0;

	const auto ReleasedCentreOfMassZCm = [&](auto CentreOfPiece)
	{
		double MassKg = 0.0;
		double MomentKgCm = 0.0;

		for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
		{
			if (ShouldSurvive[Piece])
			{
				continue;
			}

			const double PieceMassKg = MassKgFromBox(Boxes[Piece], ClayBrick.DensityGramsPerCubicCm);

			MassKg += PieceMassKg;
			MomentKgCm += PieceMassKg * CentreOfPiece(Piece).Z;
		}

		ReleasedMassKg = MassKg;

		return MomentKgCm / MassKg;
	};

	const double LaidCentreOfMassZCm =
		ReleasedCentreOfMassZCm([&](int32 Piece) { return LaidCentreCm[Piece]; });

	TestWorld.TickSeconds(CollapseSeconds);

	const double RestingCentreOfMassZCm =
		ReleasedCentreOfMassZCm([&](int32 Piece) { return BrickCentreCm(*Bricks[Piece]); });

	int32 DidNotMove = 0;
	int32 Drifted = 0;
	double SmallestReleasedMoveCm = TNumericLimits<double>::Max();

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		const double MovedCm = FVector::Dist(Bricks[Piece]->GetActorLocation(), LaidAt[Piece]);

		if (ShouldSurvive[Piece])
		{
			if (MovedCm >= DriftToleranceCm)
			{
				++Drifted;

				AddError(FString::Printf(
					TEXT("base brick %d is grounded and must not move; it drifted %.6f cm"),
					Piece, MovedCm));
			}

			continue;
		}

		if (!(MovedCm > ReleasedMustMoveCm))
		{
			++DidNotMove;

			AddError(FString::Printf(
				TEXT("released arm brick %d was handed to physics and must have moved; it moved %.6f cm"),
				Piece, MovedCm));
		}

		SmallestReleasedMoveCm = FMath::Min(SmallestReleasedMoveCm, MovedCm);
	}

	AddInfo(FString::Printf(
		TEXT("the released %.1f kg of arm had its centre of mass at Z %.3f as laid and Z %.3f after %g s, a drop of %.3f cm; the floor is Z %g; smallest released move %.6f cm"),
		ReleasedMassKg, LaidCentreOfMassZCm, RestingCentreOfMassZCm, CollapseSeconds,
		LaidCentreOfMassZCm - RestingCentreOfMassZCm, FloorTopZCm, SmallestReleasedMoveCm));

	TestEqual(
		FString::Printf(TEXT("no grounded base brick may have moved; %d did"), Drifted),
		Drifted, 0);

	TestEqual(
		FString::Printf(TEXT("every released arm brick must have moved under gravity; %d of %d did not"),
			DidNotMove, ExpectedReleased),
		DidNotMove, 0);

	TestTrue(
		FString::Printf(
			TEXT("fixture: the released arm must have real mass to fall; %.1f kg over %d released"),
			ReleasedMassKg, ExpectedReleased),
		ReleasedMassKg > 0.0 && ExpectedReleased > 0);

	TestTrue(
		FString::Printf(
			TEXT("the arm must have COME DOWN: the released centre of mass should end below Z %.3f, it is at Z %.3f"),
			LaidCentreOfMassZCm, RestingCentreOfMassZCm),
		RestingCentreOfMassZCm < LaidCentreOfMassZCm);

	TestTrue(
		FString::Printf(
			TEXT("and it must have LANDED rather than left the world: it should stay above the floor at Z %g, it is at Z %.3f"),
			FloorTopZCm, RestingCentreOfMassZCm),
		RestingCentreOfMassZCm > FloorTopZCm);

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
