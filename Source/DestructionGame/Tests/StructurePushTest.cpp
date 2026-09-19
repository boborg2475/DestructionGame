// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Corbel.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Named, and differently from every other namespace here: an anonymous namespace is private
 * to a translation unit, not a file, and a unity build merges many files into one. The world
 * harness stays in Tests/BrickWorldTestSupport.h, shared with Tests/BrickActorTest.cpp — a
 * second copy of a floor height, settle threshold and tick length is two fixtures that drift.
 */
namespace StructurePushTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * BrickWorldTestSupport::NarrowWaistWallSpec(4). The narrowness is the whole fixture:
	 * everything above course 0 reaches the ground only through the single brick of course 1.
	 *
	 *      course 3            [ 5 ]              rests on 3 and 4
	 *      course 2         [ 3 ][ 4 ]            head joint 3-4 between them
	 *      course 1            [ 2 ]              THE WAIST - removed by the test
	 *      course 0         [ 0 ][ 1 ]            grounded
	 *
	 * Why a waist is the only shape that sees any of this is written where the spec lives.
	 */

	/**
	 * Fixture preconditions, asserted rather than assumed: 2 + 1 + 2 + 1 pieces, joined by the
	 * head joints 0-1 and 3-4, the four bed joints into the waist (0-2, 1-2, 2-3, 2-4) and the
	 * two carrying the top brick (3-5, 4-5). A producer change that moves either number means
	 * the arrangement reasoned about is no longer the one built, and should fail here rather
	 * than downstream with a plausible wrong count.
	 */
	constexpr int32 NarrowWallPieceCount = 6;
	constexpr int32 NarrowWallJointCount = 8;

	/** The single course-1 brick everything above the bottom course hangs from. */
	constexpr int32 WaistPiece = 2;

	/**
	 * Exactly the pieces that lose their path to the ground when the waist goes.
	 *
	 * 3 and 4 fall back on the head joint between them, each naming the other as its support.
	 * That looks like a cycle but is not reported as one: LoadReturnsToPiece walks LoadPaths,
	 * which holds only supports that themselves reach the ground, so the walk finds nothing and
	 * both read plain Falling. 5 keeps two bed joints onto 3 and 4 and falls with them.
	 *
	 * Nothing here is Stranded, and the test asserts it: Stranded means the solver declined to
	 * divide load round a knot — a model limitation, not a statement that anything fell — and a
	 * fixture calibrated on one would look identical while measuring something else
	 * (CURRENT_STATE.md flags this trap). Three bricks with no load path at all avoids it.
	 */
	constexpr bool bOrphanedByRemovingTheWaist[NarrowWallPieceCount] =
	{
		false, false, false, true, true, true
	};

	constexpr int32 ExpectedReleaseCount = 3;

	/**
	 * How far the orphans fall, and why it is not the 50 cm of clear air the brick actor test
	 * gets: orphaning takes away what was under a piece, so the hole is one course. Pieces 3 and
	 * 4 drop 8.5 cm, clear of BrickWorldTestSupport::FallenAtLeastCm (5 cm, itself five times
	 * the 1 cm mortar joint a brick settling into its own gap would move).
	 */

	/*
	 * Four walls this producer lays all stand — and the one structure that cannot.
	 *
	 * Under the no-tension partial-contact edge rule (Core/ConnectionStrength.cpp) a dry ragged
	 * wall holds itself up. A ragged end brick is a corbel keeping one 10.25 x 10.25 cm bed patch
	 * and standing its own weight 5.625 cm inside that patch's centroid; the brick above hands its
	 * half share down through the same centroid, so it carries F = 1.5 brick weights at
	 * e = |M|/|F| = 3.75 cm — past the kern (h/6 = 1.71) but well inside the face (h/2 = 5.125).
	 * The dry joint cannot pull, so it cracks and bears on a triangular block
	 * L_c = 3(h/2 - e) = 4.125 cm whose squeezed fibre reads 2N / (t x L_c) = 0.0189 MPa, 0.000631
	 * of dry stone's 30 MPa. Corbels below carry more, so e only shrinks. The retired kern rule
	 * condemned this joint at the first whisker of tension.
	 *
	 * The LP agrees even where the per-joint router does not: an even-course finish leaves a top
	 * corbel carrying only its own weight (F = 1.0, e = 5.625 > h/2), whose resultant leaves the
	 * face and reads infinite — but below the 200-block cap the LP is the authority, the corbel
	 * leans on its in-course neighbour across the head joint, and a whole-structure equilibrium
	 * exists. Measured: a dry ragged wall of 22..26 courses sheds nothing, so the "over capacity
	 * as built" fixture is now a bare corbel arm (DryCorbelArmSpec) instead.
	 */

	/** The one producer call every wall below is a set of arguments to. */
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
	 * Six wide because the corbel is a local phenomenon at each end, so width buys only actors;
	 * twenty-four (an odd finish, course 23 being the top) so every top corbel has one brick on
	 * it and reads the F = 1.5, e = 3.75 cm case above. 132 pieces, so the LP is the authority.
	 */
	constexpr int32 OverCapacityWallCourses = 24;
	constexpr int32 OverCapacityWallBricksPerCourse = 6;

	/** 12 even courses of 6 plus 12 odd courses of 5. */
	constexpr int32 OverCapacityWallPieceCount = 12 * 6 + 12 * 5;

	/** The dry ragged wall: toothed ends, nothing in the joint. It stands. */
	inline FRunningBondSpec DryRaggedWallSpec(int32 CoursesHigh, int32 BricksPerCourse)
	{
		return WallSpecOf(CoursesHigh, BricksPerCourse, EWallEnd::Ragged, DryStone);
	}

	/** The game mode's own scenario wall: 40 courses of 30, flush, mortared. */
	inline FRunningBondSpec ScenarioWallSpec()
	{
		return WallSpecOf(40, 30, EWallEnd::Flush, GeneralPurposeMortar);
	}

	/* The coordinating grid: a brick plus a joint, and half of it is the bond offset. */
	constexpr double CoursePitchCm = 6.5 + 1.0;
	constexpr double BrickPitchCm = 21.5 + 1.0;

	/** 1.9 g/cm3 x 21.5 x 10.25 x 6.5 / 1000 = 2.72163125 kg, x 980 cm/s2. */
	constexpr double FullBrickWeightUu = 1.9 * 21.5 * 10.25 * 6.5 / 1000.0 * 980.0;

	/**
	 * Unreal force units per MPa per cm2, spelled out rather than imported: a missing or
	 * duplicated conversion is out by exactly 100x and tuned thresholds conceal it well
	 * (DESIGN.md 3), so a test reading production's named boundary would agree with a wrong one.
	 */
	constexpr double ForceUnitsPerMPaSqCmHere = 10000.0;

	/** The half-brick bed patch a corbel keeps: 10.25 along the wall by 10.25 through it. */
	constexpr double CorbelBedAreaSqCm = 10.25 * 10.25;

	/** The bed patch's depth in the bending direction, cm: h. */
	constexpr double CorbelBedDepthCm = 10.25;

	/** (4/3) x h_u x h_v^2 for that square patch, cm3 — equal to t h^2 / 6 since it is square. */
	constexpr double CorbelSectionModulusCm3 =
		(4.0 / 3.0) * (10.25 / 2.0) * (10.25 / 2.0) * (10.25 / 2.0);

	/** Half of one half-step: the brick keeps half a cell, so its centre is 22.5 / 4 out. */
	constexpr double CorbelOwnWeightArmCm = 22.5 / 4.0;

	/** Dry stone: no bond at all, so tensile and cohesion are exact zeroes; f_c = 30 MPa. */
	constexpr double DryStoneTensileMPa = 0.0;
	constexpr double DryStoneCompressiveMPa = 30.0;

	/**
	 * Mean flexural bond f_x1 for general-purpose mortar (re-anchor 2026-08-13; Gooch et al. 2023
	 * batch means, UK NA inversion). A bonded corbel never fires the no-tension relief, so it
	 * keeps its uncracked tension reading.
	 */
	constexpr double MortarTensileMPa = 0.70;

	/** Force and moment on the top corbel's one bed joint, in brick weights. */
	constexpr double TopCorbelForceBrickWeights = 1.5;
	constexpr double TopCorbelMomentBrickWeightCm = CorbelOwnWeightArmCm;

	/** e = |M|/|F| = 3.75 cm — past the kern (1.71), well inside the face (h/2 = 5.125). */
	constexpr double TopCorbelArmCm =
		TopCorbelMomentBrickWeightCm / TopCorbelForceBrickWeights;

	/*
	 * The cracked-and-bearing reading derived above, spelled out here rather than read off the
	 * solver. Continuous with the kern (at e = h/6 it returns 2 sigma_mean, the linear peak) and
	 * blowing up only as e -> h/2.
	 */
	constexpr double TopCorbelContactLengthCm =
		3.0 * (CorbelBedDepthCm / 2.0 - TopCorbelArmCm);

	constexpr double TopCorbelReducedContactStressMPa =
		2.0 * TopCorbelForceBrickWeights * FullBrickWeightUu
		/ (CorbelBedDepthCm * TopCorbelContactLengthCm)
		/ ForceUnitsPerMPaSqCmHere;

	/** 0.00063082303: the dry ragged wall's top corbels, compression on reduced contact. */
	constexpr double DryRaggedTopCorbelUtilisation =
		TopCorbelReducedContactStressMPa / DryStoneCompressiveMPa;

	/*
	 * The same joint in mortar, worked the uncracked linear way because a bonded joint keeps its
	 * tension: peak tension = |M|/W_v - N/A. The mortared wall's worst joint, left unchanged by
	 * the dry edge rule — the control saying raggedness alone does not condemn a wall.
	 */
	constexpr double TopCorbelTensileStressMPa =
		FullBrickWeightUu
		* (TopCorbelMomentBrickWeightCm / CorbelSectionModulusCm3
			- TopCorbelForceBrickWeights / CorbelBedAreaSqCm)
		/ ForceUnitsPerMPaSqCmHere;

	/** 0.0065014926: the mortared ragged wall's worst joint, tension over f_x1 = 0.70. */
	constexpr double MortarRaggedWorstAsBuilt = 0.0065014926;

	/*
	 * The scenario wall's worst joint: flush, so e = 0 at every seat and this is
	 * compression-governed. 1220 pieces is over the 200-block cap, so the router is its authority.
	 */
	constexpr double ScenarioWorstAsBuilt = 0.00495042219;
	constexpr int32 ScenarioWallPieceCount = 1220;

	/** A dry flush wall of the same brick has no corbel anywhere: e = 0, no tension, it stands. */
	constexpr double DryFlushWorstAsBuilt = 0.000971218165;

	/** 12 even courses of 6 full bricks, 12 odd courses of 5 full bricks and 2 half bats. */
	constexpr int32 DryFlushWallPieceCount = 12 * 6 + 12 * 7;

	/*
	 * The bare corbel arm — a dry cantilever the LP genuinely cannot stand. DestructionCorbel lays
	 * BaseCourses of immovable base, then Steps single bricks each advancing one half-cell out and
	 * one course up. Unfilled there is one brick per stepped course, so each rests on the one below
	 * with nothing inboard to counterweight it and no neighbour to lean on — the difference from
	 * the running-bond corbel. Its bed resultant sits at e = 5.625 cm, past the face (h/2 = 5.125),
	 * so the overhang has no admissible equilibrium and the LP sheds the whole arm on spawn.
	 * Measured: 16 pieces, 19 joints, all 10 arm bed joints over capacity, none Stranded.
	 */
	constexpr int32 CorbelArmSteps = 10;
	constexpr int32 CorbelArmBaseCourses = 3;
	constexpr int32 CorbelArmBaseCells = 2;

	/** BaseCells x BaseCourses grounded base, then one brick per step. */
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

	/** Which course a laid box is in, read back off its height. */
	inline int32 CourseOf(const FPieceBox& Box)
	{
		return FMath::RoundToInt32((Box.CentreCm.Z - 6.5 * 0.5) / CoursePitchCm);
	}

	/**
	 * The oracle for the corbel arm, derived from the producer rather than the run: a piece is in
	 * the grounded base, and so survives, exactly when its course is below BaseCourses. Read off
	 * the box's height, so it does not depend on the order the producer emits pieces in.
	 */
	inline bool CorbelArmPieceSurvives(const FPieceBox& Box)
	{
		return CourseOf(Box) < CorbelArmBaseCourses;
	}

	/**
	 * How far a released brick must have moved to count as handed to physics — not a fall
	 * distance, and deliberately tiny. A kinematic brick moves exactly 0.000000 cm (measured)
	 * because nothing integrates it at all, so this only has to sit above that zero and below
	 * anything a real body does in a second of gravity. The smallest released move is logged
	 * every run so the margin is visible rather than assumed.
	 */
	constexpr double ReleasedMustMoveCm = 0.01;

	/**
	 * How long a 132-brick wall takes to fall down, measured rather than guessed.
	 *
	 * A released brick here is not in free fall, which caught the first version of this test out:
	 * the whole wall lets go at once, so every brick rests on another that is also falling, the
	 * stack compacts from the bottom, and the pile must then topple sideways out of a wall one
	 * brick thick. The released centre of mass reads Z 92.434 after one second, 75.480 after two,
	 * 1.966 after three, and 1.881 from four seconds on — so three is where it finishes, and the
	 * test ticks one more and asserts nothing changed. A fixed count of seconds, never a settle
	 * poll (see BrickWorldTestSupport::PhysicsStepSeconds): a poll turns this failure into a
	 * timeout, which reports far worse than an assertion.
	 */
	constexpr double CollapseSeconds = 3.0;

	/**
	 * A brick's centre in world space, which is not its actor location: SM_Cube's pivot is a
	 * corner, and once a falling brick has rotated that corner has swung elsewhere, whereas
	 * FBoxSphereBounds::Origin is the centre whatever the rotation. Displacement may be measured
	 * from the pivot, since the same material point is compared with itself; a centre of mass may
	 * not, being compared against landmarks in the world.
	 */
	inline FVector BrickCentreCm(const ABrickActor& Brick)
	{
		return Brick.GetMesh()->Bounds.Origin;
	}

	/**
	 * When the second push happens, and why it is early. The idempotence claim rests on the linear
	 * velocity surviving the call, because "still simulating" is unfalsifiable —
	 * SetSimulatePhysics(true) on an already-simulating body satisfies it either way while quietly
	 * recreating the body at rest. Three physics steps is 0.05 s, about -49 cm/s and 1.2 cm of
	 * travel; by 0.25 s the bricks have landed and the comparison is 0 against 0.
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

	/** The whole structure's answer in one log line, so a failure reads without a debugger. */
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
 * A wall that is standing is solved and pushed and nothing comes down.
 *
 * The control, without which the collapse test is free: "release everything the binding knows
 * about" passes an assertion that the orphans fell, and only an untouched wall that stays up
 * tells that implementation from the real one. Proved to bite by that mutation.
 *
 * It also pins that a push with no solve behind it releases nothing. FStructureBinding
 * ::ApplyResults refuses to act on a piece the last solve never answered for, because
 * EPieceSupport::Falling is also what an absent answer reads as — before any solve that is every
 * piece in the wall, foundation included. SolveAndPush's solve discharges that obligation, so the
 * order of the two halves is load-bearing. An unknown id fails closed: no crash, nothing released.
 *
 * Needs a ticking world: unticked kinematic actors stand however wrong the answer was.
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

	/* Nothing built yet, so every id is unknown, including the one about to be handed out. */
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

	/* The obligation SolveAndPush inherits, asserted before anything solves. */
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

		/*
		 * The mechanism behind the outcome: every piece is held up, so nothing should be released
		 * — which makes "nothing moved" a statement about the push rather than about a wall with
		 * no way of moving.
		 */
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

	/*
	 * Fail closed on an id that names nothing, and re-check the wall afterwards: a push that
	 * ignored its argument and swept every binding it owns would answer zero here while releasing
	 * the whole wall.
	 */
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

	/* One second of real gravity, on a fixed step, and the wall is still a wall. */
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

	/* And nothing about a settled wall changes on being asked again. */
	TestEqual(
		TEXT("a second SolveAndPush on the same standing wall must release nothing"),
		TestWorld.Subsystem->SolveAndPush(StructureId), 0);

	TestWorld.End();

	return true;
}

/**
 * Take out the one brick everything above rests on, and exactly the pieces that lost their
 * path to the ground are released and fall.
 *
 * Which handles, not how many: a count is satisfied by releasing any three bricks, and the defect
 * that matters — a push that walked the wrong array, or released by position rather than by the
 * solver's answer — comes apart in the wrong place while every count agrees. So the assertion is
 * per handle, against a set derived from the tier rule.
 *
 * The mechanism is not the outcome, so both are asserted (DESIGN.md §4): a piece can be flagged
 * released while nothing ever hands it to physics. Both halves of the movement claim matter too —
 * "the orphans fell" alone passes when the entire world drops through the floor, so the
 * still-supported bricks are asserted not to have moved in the same second.
 *
 * The removed brick's actor is destroyed by the test. RemovePiece takes the piece out of the graph
 * and clears the binding's actor, but nothing destroys the actor yet — that belongs to a Delete
 * action which does not exist. Left in place it would be a kinematic brick still filling the hole,
 * the orphans would land on it after 1 cm, and the fall assertion would be measuring the mortar
 * settle it is chosen to distinguish from.
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

	/*
	 * Fixture precondition: the wall stands as built. If it came apart on its own, every assertion
	 * below about what the removal caused would measure something else.
	 */
	TestEqual(
		TEXT("fixture: the wall as built should stand, so the first push releases nothing"),
		TestWorld.Subsystem->SolveAndPush(StructureId), 0);

	ReportSupport(*this, *Binding, TEXT("as built"));

	/*
	 * Out comes the waist. The actor is captured first because RemovePiece clears the binding's
	 * pointer to it, and destroyed afterwards because the graph decides whether a removal happened.
	 */
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
			/* A removed piece has no actor left, so it is never released whatever it reads as. */
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

		/*
		 * A collapse, not a solver stall: a Stranded piece is one the solver declined to route load
		 * around rather than one that lost its support, and a fixture calibrated on a knot would
		 * come down looking the same. See bOrphanedByRemovingTheWaist.
		 */
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

		/*
		 * And the flag reached the brick. IsReleased is the binding's record, simulating is the
		 * actor's, and ABrickActor keeps no second copy — so a push that flipped every flag and
		 * called nothing is caught here.
		 */
		const bool bSimulating =
			Bricks[Piece]->GetMesh() != nullptr && Bricks[Piece]->GetMesh()->IsSimulatingPhysics();

		TestTrue(
			FString::Printf(
				TEXT("brick %d should%s be simulating physics once the waist has gone; it is %s"),
				Piece, bExpected ? TEXT("") : TEXT(" not"), bSimulating ? TEXT("simulating") : TEXT("kinematic")),
			bSimulating == bExpected);
	}

	/* A short tick, then the second push while the bricks are still in the air. */
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

		/* The precondition for the idempotence row: 0 against 0 would prove nothing. */
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

		/*
		 * The row the idempotence claim rests on. Nothing ticked between the two reads, so a push
		 * that derives its state from the body and returns early leaves this exactly equal; one
		 * that re-creates the body loses the velocity, and a brick a twentieth of a second into its
		 * fall is suddenly hanging still.
		 */
		const FVector VelocityAfter = Bricks[Piece]->GetMesh()->GetPhysicsLinearVelocity();

		TestTrue(
			FString::Printf(
				TEXT("a second push must not disturb falling brick %d: velocity was (%.3f, %.3f, %.3f), now (%.3f, %.3f, %.3f)"),
				Piece,
				VelocityBefore[Piece].X, VelocityBefore[Piece].Y, VelocityBefore[Piece].Z,
				VelocityAfter.X, VelocityAfter.Y, VelocityAfter.Z),
			VelocityAfter.Equals(VelocityBefore[Piece], 1.0e-3));
	}

	/* One second of simulated time in total, on a fixed step, never on a settle poll. */
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
			/* The other half: without it, a world that fell through the floor passes the row above. */
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
 * A structure that cannot hold itself up comes down when it is built, not when somebody
 * happens to click — and one that can is left exactly where it was laid.
 *
 * The line this guards is the SolveAndBreak inside UDestructionStructureSubsystem::SolveAndPush,
 * so a structure over capacity the instant it is laid settles there and then rather than standing
 * intact until the player right-clicks an unrelated brick and is blamed for a collapse they did
 * not cause. The subject is a bare unfilled corbel arm rather than a ragged wall because, per the
 * derivation on the support block above, a running-bond wall of any height now sheds nothing. So
 * the LP is the oracle for two claims — the dry ragged wall stands untouched (the control stopping
 * "settle at build time" being satisfied by knocking everything down), and the arm settles on
 * spawn.
 *
 * Both halves of the outcome are asserted, per DESIGN.md 4: released arm bricks must have moved
 * under gravity (a break stamp and a released flag are only steps), and the standing base and
 * wall must not have moved in the same second. Which axis governs is asserted rather than assumed
 * — the dry corbel compression-on-reduced-contact, the mortared one tension — so a run where the
 * wrong axis took over shows as a mismatched number. The solver's own answer is world-free and
 * established first with no actors, so everything after is about the wire.
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

	/*
	 * First the load model as arithmetic, with no world in the way: Core/Layout and FStructure are
	 * world-free, so establishing what each structure carries here lets everything after be a
	 * statement about the wire.
	 */

	/* The dry ragged wall stands. */
	{
		FBrickLayout Dry;

		TestTrue(TEXT("fixture: the producer should lay the dry ragged wall"),
			RunningBond(
				DryRaggedWallSpec(OverCapacityWallCourses, OverCapacityWallBricksPerCourse), Dry));

		TestEqual(
			FString::Printf(TEXT("fixture: the dry ragged wall should be %d pieces"),
				OverCapacityWallPieceCount),
			Dry.Structure.NumPieces(), OverCapacityWallPieceCount);

		/* The profiles are asserted, never trusted: the whole fixture turns on their zeroes. */
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

		/*
		 * The top corbel, found by geometry rather than index: the end brick of the highest even
		 * course, and the one brick still under it half a step to its right.
		 */
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

		/*
		 * What that joint carries, against the hand arithmetic — its own weight is the only load
		 * with a lever arm, which is where M = 5.625 brick-weight-cm comes from.
		 */
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

		/*
		 * And it stands on its reduced contact. The hand value and the solver's must agree,
		 * which is the axis check: a joint that had failed at the retired kern would read the
		 * sentinel (~1.8e308) here instead.
		 */
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

		/*
		 * And not one joint in the wall is over capacity: every corbel below the top carries more
		 * closing compression at the same arm, so its resultant sits further inside the face.
		 */
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

		/* So the LP, the authority below the cap, stands it — the verdict the wire reproduces. */
		TestEqual(
			TEXT("settling the dry ragged wall must break nothing: the LP stands it entire"),
			Dry.Structure.SolveAndBreak(), 0);
	}

	/* The mortared control: identical geometry, bonded joints — isolates raggedness from bond. */
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

	/*
	 * The dry flush control: no bond and no corbel. A half bat fills each end, so the end brick
	 * gets two supports and e = 0 — isolating the corbel, not the missing bond, as what an
	 * over-capacity dry structure needs.
	 */
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

	/*
	 * The scenario wall, untouched — the game mode's own 40 x 30 flush mortared wall. At 1220
	 * pieces it is over the block cap, so the router rather than the LP is its authority.
	 */
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

	/*
	 * Now the structure that genuinely cannot hold itself up: the bare dry corbel arm. Nothing
	 * may be Stranded — that would be a solver stall rather than a collapse.
	 */
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

	/* Now the wire: build each in a world and solve-and-push it once, with no click. */
	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	/*
	 * The standing control first: a dry ragged wall built in the world and pushed once must
	 * release nothing — the half of "settles at build time" that stops it being satisfied by an
	 * implementation that settles everything.
	 */
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

	/*
	 * The over-capacity subject: build the bare corbel arm and ask once. Nothing is removed and
	 * nobody clicks — this is the wire the game mode runs on BeginPlay.
	 */
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

	/* The only call this test makes on the arm — one push, no click. */
	const int32 Released = TestWorld.Subsystem->SolveAndPush(ArmId);

	TestEqual(
		FString::Printf(
			TEXT("building an arm that cannot hold itself up must settle it there and then: %d should release, %d did"),
			ExpectedReleased, Released),
		Released, ExpectedReleased);

	/*
	 * The mechanism behind it: joints gave under load. A break pass is a stamp a plain solve can
	 * never produce, separating a genuine settle from a wall that merely reads over capacity.
	 */
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

	/*
	 * Which bricks, not how many: base survives and arm goes, each checked against the producer's
	 * own oracle, so a push that came apart in the wrong place is caught even though the count
	 * agrees. And the flag must have reached the body — ABrickActor keeps no second copy.
	 */
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
	 * The outcome: real gravity on a fixed step, and the arm comes down while the base does not.
	 * The claim is relational rather than a fitted landmark, because released arm bricks tumble off
	 * a cantilever and pile where they may. The released centre of mass must end below where it
	 * started (a 0/0 NaN fails the strict test if nothing was released) and stay above the floor
	 * (it settled rather than tunnelling out of the world); every released brick must have moved
	 * more than the kinematic-vs-dynamic threshold; and every surviving base brick must not have
	 * moved, or the world simply dropped through its floor.
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
