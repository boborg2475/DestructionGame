// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Corbel.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * NAMED NAMESPACE, and named differently from every other one in this module — an
 * anonymous namespace is private to a TRANSLATION UNIT, not to a file, and a unity build
 * merges many files into one. The world harness itself is NOT redeclared here: it lives
 * in Tests/BrickWorldTestSupport.h and is shared with Tests/BrickActorTest.cpp, because a
 * second copy of a floor height, a settle threshold and a tick length is two fixtures that
 * drift. Only what is specific to the push tests is below.
 */
namespace StructurePushTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/*
	 * THE WALL THIS FILE USES IS BrickWorldTestSupport::NarrowWaistWallSpec(4), and the
	 * narrowness is the whole fixture — everything above course 0 reaches the ground only
	 * through the single brick of course 1. Four courses, so there are two pieces above the
	 * waist with each other as their only head joint and one more piece above those:
	 *
	 *      course 3            [ 5 ]              rests on 3 and 4
	 *      course 2         [ 3 ][ 4 ]            head joint 3-4 between them
	 *      course 1            [ 2 ]              THE WAIST - removed by the test
	 *      course 0         [ 0 ][ 1 ]            grounded
	 *
	 * Why a waist is the only shape that can see any of this, and why the wall is ragged and
	 * two bricks per course, is written where the spec now lives.
	 */

	/**
	 * FIXTURE PRECONDITIONS, asserted rather than assumed.
	 *
	 * 2 + 1 + 2 + 1 pieces, and the joints are: the two head joints 0-1 and 3-4, the four
	 * bed joints into the waist (0-2, 1-2, 2-3, 2-4) and the two carrying the top brick
	 * (3-5, 4-5). If a producer change moves either number, the arrangement this test
	 * reasons about is no longer the arrangement it built, and it should say so here rather
	 * than fail somewhere downstream with a plausible-looking wrong count.
	 */
	constexpr int32 NarrowWallPieceCount = 6;
	constexpr int32 NarrowWallJointCount = 8;

	/** The single course-1 brick everything above the bottom course hangs from. */
	constexpr int32 WaistPiece = 2;

	/**
	 * Exactly the pieces that lose their path to the ground when the waist goes.
	 *
	 * 3 and 4 lose their only bed joint beneath and fall back on the head joint between
	 * them, so each names the other as its support. That looks like a cycle, and it is NOT
	 * reported as one: LoadReturnsToPiece walks LoadPaths, which holds only supports that
	 * themselves reach the ground, and neither of these does — so the walk finds nothing and
	 * both read plain Falling. 5 keeps two intact bed joints, onto 3 and 4, and is Falling
	 * for the ordinary reason that what was carrying it is not being carried.
	 *
	 * SO NOTHING HERE IS Stranded, AND THAT IS ASSERTED RATHER THAN ASSUMED. Stranded means
	 * the solver declined to divide load round a knot — a limitation of the model, not a
	 * statement that anything fell — and a collapse fixture calibrated on one would look
	 * identical while measuring something else entirely. CURRENT_STATE.md flags exactly this
	 * trap for the collapse test, and three bricks with no load path at all is the shape that
	 * avoids it.
	 *
	 * 0 and 1 are grounded and stay exactly where they were laid. Piece 2 is neither: it
	 * has been removed, so it is never released and has no actor left to release.
	 */
	constexpr bool bOrphanedByRemovingTheWaist[NarrowWallPieceCount] =
	{
		false, false, false, true, true, true
	};

	constexpr int32 ExpectedReleaseCount = 3;

	/**
	 * How far the orphans have to fall, and why it is NOT the 50 cm of clear air the brick
	 * actor test gets.
	 *
	 * Orphaning a piece means taking away what was under it, so the hole is one course. The
	 * course pitch is 7.5 cm and a brick is 6.5 cm tall, so pieces 3 and 4 start with their
	 * undersides at Z = 15 and land on the top of course 0 at Z = 6.5: a drop of 8.5 cm.
	 * Piece 5 lands on top of them, about 9.5 cm below where it was laid.
	 *
	 * That is comfortably clear of BrickWorldTestSupport::FallenAtLeastCm, which is 5 cm and
	 * is itself five times the 1 cm mortar joint a brick settling into its own gap would
	 * move. It is a smaller margin than a brick dropped over open floor, and it is the best
	 * available in a wall where removing ONE brick genuinely orphans something — which is
	 * the property this fixture is chosen for.
	 */

	/*
	 * ================================================================================
	 * FOUR WALLS THE SAME PRODUCER LAYS, AND WHY EVERY ONE OF THEM STANDS — PLUS THE ONE
	 * STRUCTURE THAT GENUINELY CANNOT HOLD ITSELF UP.
	 * ================================================================================
	 *
	 * THIS FILE USED TO OPEN ON A DRY RAGGED WALL THAT "CANNOT HOLD ITSELF UP". Under the
	 * no-tension partial-contact edge rule (Core/ConnectionStrength.cpp) that is no longer
	 * true, and the correction is the whole point of the arithmetic below.
	 *
	 * A ragged end brick is a corbel that keeps ONE 10.25 x 10.25 cm bed patch and stands its
	 * own weight 5.625 cm to the inside of that patch's centroid. The brick RESTING on it hands
	 * its half share straight down through a patch with the SAME centroid (the zig-zag), so the
	 * corbel carries F = 1.5 brick weights at an effective eccentricity of e = |M|/|F| =
	 * 5.625 / 1.5 = 3.75 cm. The bed is 10.25 cm deep, so the kern is h/6 = 1.71 cm and the face
	 * half-depth is h/2 = 5.125 cm: the resultant is PAST THE KERN but WELL INSIDE THE FACE. A
	 * dry joint cannot pull, so it CRACKS and bears on the part still in contact rather than
	 * failing — a triangular block L_c = 3(h/2 - e) = 4.125 cm long — and the squeezed fibre
	 * reads sigma = 2N / (t x L_c) = 0.0189 MPa, which is 0.000631 of dry stone's 30 MPa. The
	 * corbel STANDS, and so does every corbel below it (they carry more, so e only shrinks). The
	 * retired kern rule condemned this joint at the first whisker of tension; it was
	 * over-conservative, and the wall genuinely stands.
	 *
	 * AND IT IS NOT MERELY THE PER-JOINT READING THAT STANDS IT. Below the 200-block cap the
	 * equilibrium LP is the break authority, and even where the per-joint router DOES condemn a
	 * corbel — an even-course finish leaves a top corbel carrying only its own weight, F = 1.0,
	 * e = 5.625 > h/2, so its bed resultant genuinely leaves the face and the router reads it
	 * infinite — the LP STILL stands the wall: the corbel leans on its in-course neighbour
	 * across the head joint, and a valid whole-structure equilibrium exists. Measured: a dry
	 * ragged wall of 22..26 courses sheds NOTHING. A running-bond wall therefore cannot be the
	 * "over capacity as built" fixture any more.
	 *
	 * THE GENUINELY-OVER-CAPACITY STRUCTURE THAT KEEPS THE "SETTLES WITHOUT A CLICK" BEHAVIOUR
	 * UNDER TEST IS A BARE CORBEL ARM (DryCorbelArmSpec, below): a single-brick cantilever with
	 * no neighbour to lean on. The LP has no admissible equilibrium for its overhang and sheds
	 * the whole arm on spawn — which is the only honest way left to build a dry structure that
	 * settles the instant it is laid.
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
	 * TWENTY-FOUR COURSES OF SIX, dry and ragged — the wall the old fixture wrongly condemned.
	 * SIX WIDE because the corbel is a LOCAL phenomenon at each end, so width buys only actors.
	 * TWENTY-FOUR (an odd finish, since course 23 is the top) so every top corbel has one brick
	 * resting on it and reads the F = 1.5, e = 3.75 cm case worked through above. 132 pieces,
	 * comfortably under the 200-block cap, so the LP is its break authority.
	 */
	constexpr int32 OverCapacityWallCourses = 24;
	constexpr int32 OverCapacityWallBricksPerCourse = 6;

	/** 12 even courses of 6 plus 12 odd courses of 5. */
	constexpr int32 OverCapacityWallPieceCount = 12 * 6 + 12 * 5;

	/** The dry ragged wall: toothed ends, nothing in the joint. It STANDS. */
	inline FRunningBondSpec DryRaggedWallSpec(int32 CoursesHigh, int32 BricksPerCourse)
	{
		return WallSpecOf(CoursesHigh, BricksPerCourse, EWallEnd::Ragged, DryStone);
	}

	/** The game mode's own scenario wall: 40 courses of 30, FLUSH, mortared. */
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
	 * Unreal force units per MPa per cm2, SPELLED OUT RATHER THAN IMPORTED.
	 *
	 * DESIGN.md 3's whole units section exists because a missing or duplicated conversion is
	 * out by exactly 100x and tuned thresholds conceal it well. A test that read production's
	 * one named boundary would agree with a wrong one.
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

	/** Dry stone: no bond at all, so tensile and cohesion are EXACT zeroes; f_c = 30 MPa. */
	constexpr double DryStoneTensileMPa = 0.0;
	constexpr double DryStoneCompressiveMPa = 30.0;

	/**
	 * The MEAN flexural bond f_x1 for general-purpose mortar (re-anchor 2026-08-13; Gooch et al.
	 * 2023 batch means with the UK NA inversion). The mortared corbel is BONDED, so the
	 * no-tension relief never fires on it and it keeps its uncracked tension reading.
	 */
	constexpr double MortarTensileMPa = 0.70;

	/** Force and moment on the top corbel's one bed joint, in brick weights. */
	constexpr double TopCorbelForceBrickWeights = 1.5;
	constexpr double TopCorbelMomentBrickWeightCm = CorbelOwnWeightArmCm;

	/** e = |M|/|F| = 3.75 cm — past the kern (1.71), well inside the face (h/2 = 5.125). */
	constexpr double TopCorbelArmCm =
		TopCorbelMomentBrickWeightCm / TopCorbelForceBrickWeights;

	/*
	 * ================================================================================
	 * WHAT THE DRY TOP CORBEL READS NOW IT IS ALLOWED TO CRACK AND BEAR — THE FLIPPED
	 * EXPECTATION, DERIVED HERE AND NOT READ OFF THE SOLVER.
	 * ================================================================================
	 *
	 * The resultant is past the kern, so the dry (f_t = 0) bed cannot carry the linear +-sigma
	 * picture: the opened edge cannot pull, the contact shrinks to a triangular block of length
	 * L_c = 3(h/2 - e), and force balance over it concentrates the squeezed fibre to
	 * sigma_max = 2N / (t L_c). N is the 1.5 brick weights the joint carries, t = 10.25 cm is
	 * the patch width, and dividing by the 10000 uu per MPa.cm2 gives MPa. Against dry stone's
	 * 30 MPa compressive that is 0.000631 of capacity — the joint STANDS, governed by
	 * compression on its reduced contact, with zero tension. Continuous with the kern (at
	 * e = h/6 this returns 2 sigma_mean, the linear peak) and blowing up only as e -> h/2.
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
	 * THE SAME JOINT IN MORTAR, worked the OLD (uncracked, linear) way because a bonded joint
	 * keeps its tension: peak tension = |M|/W_v - N/A. The mortared wall's worst joint is this
	 * corbel's tension, and it is UNCHANGED by the dry edge rule — the control that says
	 * raggedness alone does not condemn a wall.
	 */
	constexpr double TopCorbelTensileStressMPa =
		FullBrickWeightUu
		* (TopCorbelMomentBrickWeightCm / CorbelSectionModulusCm3
			- TopCorbelForceBrickWeights / CorbelBedAreaSqCm)
		/ ForceUnitsPerMPaSqCmHere;

	/** 0.0065014926: the mortared ragged wall's worst joint, tension over f_x1 = 0.70. */
	constexpr double MortarRaggedWorstAsBuilt = 0.0065014926;

	/*
	 * The scenario wall's worst joint — a flush wall has e = 0 at every seat, so this is
	 * COMPRESSION-governed (0.00495 x 10 MPa of bed stress). 1220 pieces is over the 200-block
	 * cap, so the ROUTER is its break authority, and it stands.
	 */
	constexpr double ScenarioWorstAsBuilt = 0.00495042219;
	constexpr int32 ScenarioWallPieceCount = 1220;

	/** A dry FLUSH wall of the same brick has no corbel anywhere: e = 0, no tension, it stands. */
	constexpr double DryFlushWorstAsBuilt = 0.000971218165;

	/** 12 even courses of 6 full bricks, 12 odd courses of 5 full bricks and 2 half bats. */
	constexpr int32 DryFlushWallPieceCount = 12 * 6 + 12 * 7;

	/*
	 * ================================================================================
	 * THE BARE CORBEL ARM — A DRY CANTILEVER THE LP GENUINELY CANNOT STAND.
	 * ================================================================================
	 *
	 * DestructionCorbel lays a stepped arm: BaseCourses of immovable base, then Steps single
	 * bricks each advancing one half-cell (11.25 cm) outward and one course up. UNFILLED
	 * (bFilled = false) there is one brick per stepped course, so every arm brick rests on the
	 * ONE below it, overhanging it by 11.25 cm with nothing inboard to counterweight it and no
	 * neighbour to lean on. That is the difference from the running-bond corbel: the arm brick's
	 * bed resultant sits at e = 5.625 cm, past the face (h/2 = 5.125), and there is no admissible
	 * equilibrium for the overhang. Below the cap the LP is the authority, and it sheds the
	 * WHOLE arm on spawn while the grounded base stands.
	 *
	 * Measured against the current tree: 16 pieces, 19 joints, the router reads all 10 arm bed
	 * joints over capacity, and SolveAndBreak (the LP) releases exactly the 10 arm bricks with
	 * none Stranded. This is the fixture the "settles the instant it is laid, without a click"
	 * behaviour now rides on.
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
	 * THE ORACLE FOR THE CORBEL ARM, DERIVED FROM THE PRODUCER RATHER THAN THE RUN. A piece is
	 * in the grounded base — and so SURVIVES — exactly when its course is below BaseCourses;
	 * every stepped arm brick above that sheds. Read off the box's own height, so it does not
	 * depend on the order the producer emits pieces in.
	 */
	inline bool CorbelArmPieceSurvives(const FPieceBox& Box)
	{
		return CourseOf(Box) < CorbelArmBaseCourses;
	}

	/**
	 * HOW FAR A RELEASED BRICK MUST HAVE MOVED TO COUNT AS HAVING BEEN HANDED TO PHYSICS.
	 *
	 * NOT A FALL DISTANCE, AND DELIBERATELY TINY. A kinematic brick moves EXACTLY zero —
	 * 0.000000 cm, measured, in the two-brick spike and again in this suite's own survivors,
	 * because nothing integrates it at all. So any positive number discriminates, and the
	 * only job of this one is to be far enough above the zero a kinematic body reports and
	 * far enough below anything a real body does in a second of gravity. A tenth of a
	 * millimetre is a hundredth of the nominal joint and a thousandth of a course, and the
	 * smallest movement any of the 111 released bricks makes is reported on every run so the
	 * margin is visible rather than assumed.
	 *
	 * WHY THIS IS THE PER-BRICK CLAIM AND A FALL DISTANCE IS NOT, see the outcome block.
	 */
	constexpr double ReleasedMustMoveCm = 0.01;

	/**
	 * HOW LONG A 132-BRICK WALL TAKES TO FALL DOWN, MEASURED RATHER THAN GUESSED.
	 *
	 * A released brick in this collapse is NOT in free fall, which is the thing that caught
	 * the first version of this test out. The whole wall lets go at the same instant, so
	 * every brick is resting on another that is also falling and the stack compacts from the
	 * bottom rather than dropping: nothing outruns what is beneath it, and the pile then has
	 * to topple sideways out of a wall one brick thick before it can go anywhere. The
	 * released centre of mass reads Z 92.434 after one second, 75.480 after two, 1.966 after
	 * three, and then 1.879, 1.881, 1.881, 1.881, 1.881 for seconds four through eight — the
	 * pile at rest, to a thousandth of a centimetre, for five straight seconds.
	 *
	 * SO ONE SECOND WAS A COLLAPSE A TENTH OF THE WAY THROUGH, and two was barely a quarter.
	 * Three is where it finishes, and the test then ticks ONE MORE and asserts the difference
	 * is nothing, so this number is a claim the run checks rather than a duration somebody
	 * liked.
	 *
	 * A FIXED COUNT OF SECONDS, NEVER A "HAS IT SETTLED" POLL — the reason is written on
	 * BrickWorldTestSupport::PhysicsStepSeconds, and a poll would turn this failure into a
	 * timeout, which reports far worse than an assertion.
	 */
	constexpr double CollapseSeconds = 3.0;

	/**
	 * A brick's centre IN WORLD SPACE, which is not its actor location.
	 *
	 * SM_Cube's pivot is a CORNER, so GetActorLocation returns a corner of the brick, and
	 * once a falling brick has rotated that corner has swung somewhere else entirely — the
	 * pivot-to-centre offset turns with the body. FBoxSphereBounds::Origin is the component
	 * transform applied to the mesh's local bounds centre, so it is the centre whatever the
	 * rotation: the axis-aligned box of a rotated symmetric box is still centred on it.
	 *
	 * DISPLACEMENT may be measured from the pivot, because the same material point is being
	 * compared with itself. A CENTRE OF MASS may not, because it is compared against
	 * landmarks in the world.
	 */
	inline FVector BrickCentreCm(const ABrickActor& Brick)
	{
		return Brick.GetMesh()->Bounds.Origin;
	}

	/**
	 * When the second push happens, and why it is EARLY.
	 *
	 * The idempotence claim is that a second SolveAndPush does not disturb bricks already
	 * falling, and the assertion that can fail is the linear velocity surviving the call —
	 * "still simulating" is unfalsifiable, because SetSimulatePhysics(true) on an
	 * already-simulating body satisfies it either way while quietly recreating the body at
	 * rest. So the comparison has to be made while the bricks are genuinely moving. Three
	 * physics steps is 0.05 s, about -49 cm/s and 1.2 cm of travel, so nothing has landed
	 * yet; by 0.25 s these bricks have already hit the course below and the comparison would
	 * be 0 against 0.
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
 * A WALL THAT IS STANDING IS SOLVED AND PUSHED AND NOTHING COMES DOWN.
 *
 * THIS IS THE CONTROL, AND WITHOUT IT THE COLLAPSE TEST IS FREE. "Release everything the
 * binding knows about" passes an assertion that the orphans fell; only an untouched wall
 * that stays up can tell that implementation from the real one. Proved to bite by exactly
 * that mutation.
 *
 * IT ALSO PINS THAT A PUSH WITH NO SOLVE BEHIND IT RELEASES NOTHING. FStructureBinding
 * ::ApplyResults refuses to act on a piece the last solve never answered for, because
 * EPieceSupport::Falling is also what an ABSENT answer reads as — and before any solve
 * that is every piece in the wall, foundation included. SolveAndPush inherits that
 * obligation the moment it is written, and its solve is what discharges it, so the order
 * of the two halves is load-bearing rather than stylistic. That row is GREEN ON ARRIVAL
 * (the guard already exists and is pinned by Core.StructureBinding.ReleaseNeedsASolve); it
 * is here as a regression net so nobody optimises the solve out of SolveAndPush later.
 *
 * AND AN UNKNOWN STRUCTURE ID FAILS CLOSED — no crash, and nothing released anywhere,
 * which is the second half and the one a bare "it did not crash" would miss.
 *
 * NEEDS A TICKING WORLD: yes. A full second of real gravity with nothing released is the
 * only way "the wall stands" means anything; a wall of kinematic actors that was never
 * ticked would stand however wrong the answer was.
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

	/*
	 * THE OBLIGATION SolveAndPush INHERITS, ASSERTED BEFORE ANYTHING SOLVES. Green on
	 * arrival — see the note on the test — and it is the reason a push may never be run
	 * without the solve that precedes it.
	 */
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
		 * THE MECHANISM BEHIND THE OUTCOME. Every piece is held up, so nothing SHOULD be
		 * released — which is what makes "nothing moved" a statement about the push rather
		 * than about a wall that had no way of moving.
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
	 * FAIL CLOSED ON AN ID THAT NAMES NOTHING, and the wall is re-checked afterwards: a
	 * push that ignored its argument and swept every binding it owns would answer zero
	 * here quite happily while releasing the whole wall.
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

	/* ONE SECOND OF REAL GRAVITY, ON A FIXED STEP, AND THE WALL IS STILL A WALL. */
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
 * TAKE OUT THE ONE BRICK EVERYTHING ABOVE RESTS ON, AND EXACTLY THE PIECES THAT LOST
 * THEIR PATH TO THE GROUND ARE RELEASED AND FALL.
 *
 * WHICH HANDLES, NOT HOW MANY. A count is satisfied by releasing any three bricks, and
 * the defect that matters here — a push that walked the wrong array, or released by
 * position rather than by the solver's answer — produces a wall that comes apart in the
 * wrong place while every count agrees. So the assertion is per handle, against a set
 * derived from the tier rule rather than from what the code did.
 *
 * AND THE MECHANISM IS NOT THE OUTCOME, so both are asserted. DESIGN.md §4 is explicit
 * that an integration test has to measure the structure actually moving: a binding flag
 * flipping is a step, and a piece can be flagged released while nothing ever hands it to
 * physics. BOTH HALVES OF THE MOVEMENT CLAIM MATTER TOO — "the orphans fell" alone passes
 * when the entire world drops through the floor, so the still-supported bricks are
 * asserted not to have moved in the same second.
 *
 * THE REMOVED BRICK'S ACTOR IS DESTROYED BY THE TEST. FStructureBinding::RemovePiece takes
 * the piece out of the graph and clears the binding's actor, but nothing destroys the actor
 * itself yet — that belongs to the piece context menu's Delete action, which does not exist.
 * Left in place it would be a kinematic brick still filling the hole, the orphans would land
 * on it after 1 cm and the fall assertion would be measuring the mortar settle it is
 * specifically chosen to be distinguishable from.
 *
 * NEEDS A TICKING WORLD: yes, and it is the point of the test.
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
	 * FIXTURE PRECONDITION: THE WALL STANDS AS BUILT. If this wall came apart on its own,
	 * every assertion below about what the removal caused would be measuring something else.
	 */
	TestEqual(
		TEXT("fixture: the wall as built should stand, so the first push releases nothing"),
		TestWorld.Subsystem->SolveAndPush(StructureId), 0);

	ReportSupport(*this, *Binding, TEXT("as built"));

	/*
	 * OUT COMES THE WAIST. The actor is captured first because RemovePiece clears the
	 * binding's pointer to it, and destroyed afterwards because the graph is what decides
	 * whether a removal happened at all.
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
		 * THIS IS A COLLAPSE, NOT A SOLVER STALL. A Stranded piece is one the solver
		 * declined to route load around rather than one that lost its support, and a
		 * fixture calibrated on a knot would come down looking exactly the same. See the
		 * note on bOrphanedByRemovingTheWaist.
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
		 * AND THE FLAG REACHED THE BRICK. IsReleased is the binding's record; simulating is
		 * the actor's, and ABrickActor deliberately keeps no second copy of the answer — so
		 * a push that flipped every flag and called nothing is caught right here.
		 */
		const bool bSimulating =
			Bricks[Piece]->GetMesh() != nullptr && Bricks[Piece]->GetMesh()->IsSimulatingPhysics();

		TestTrue(
			FString::Printf(
				TEXT("brick %d should%s be simulating physics once the waist has gone; it is %s"),
				Piece, bExpected ? TEXT("") : TEXT(" not"), bSimulating ? TEXT("simulating") : TEXT("kinematic")),
			bSimulating == bExpected);
	}

	/*
	 * A SHORT TICK, THEN THE SECOND PUSH WHILE THE BRICKS ARE STILL IN THE AIR — see
	 * SecondPushAtSeconds for why it cannot wait until they have landed.
	 */
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
		 * THE ROW THE IDEMPOTENCE CLAIM RESTS ON. Nothing ticked between the two reads, so a
		 * push whose release derives its state from the body and returns early leaves this
		 * exactly equal; one that re-creates the body loses the velocity entirely and a brick
		 * a twentieth of a second into its fall is suddenly hanging still. "Still simulating"
		 * cannot tell those apart.
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
			/*
			 * THE OTHER HALF, AND IT IS NOT DECORATION. Without it, a world in which
			 * everything fell through the floor passes the row above.
			 */
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
 * A STRUCTURE THAT CANNOT HOLD ITSELF UP COMES DOWN WHEN IT IS BUILT, NOT WHEN SOMEBODY
 * HAPPENS TO CLICK — AND ONE THAT CAN IS LEFT EXACTLY WHERE IT WAS LAID.
 *
 * WHAT THIS GUARDS. UDestructionStructureSubsystem::SolveAndPush runs SolveAndBreak, so a
 * structure over capacity the instant it is laid settles there and then rather than standing
 * intact until the player right-clicks some unrelated brick and is told they caused a collapse
 * they did not. The line it exists for is the SolveAndBreak inside SolveAndPush; this is a
 * regression net over that line, and over the wire that carries its answer to the bodies.
 *
 * WHY THE SUBJECT IS A BARE CORBEL ARM AND NOT A RAGGED WALL — THE 2026-09 RE-DERIVATION.
 * This test used to build a dry ragged wall and watch four corner corbels shed on spawn. The
 * no-tension partial-contact edge rule retired that: a dry corbel whose bed resultant sits past
 * the kern but inside the face (e = 3.75 cm on a 10.25 cm bed) CRACKS AND BEARS rather than
 * failing, and below the 200-block cap the equilibrium LP — the break authority — stands the
 * whole wall anyway, because even a genuinely-overhanging corbel leans on its in-course
 * neighbour across the head joint. A running-bond wall of any height now sheds NOTHING (see the
 * support block), so it can no longer carry the "settles without a click" behaviour.
 *
 * A BARE, UNFILLED CORBEL ARM CAN, and honestly: a single-brick cantilever has no neighbour to
 * lean on, the LP finds no admissible equilibrium for its overhang, and it sheds the whole arm
 * the instant it is laid. So this test now asserts TWO things against the LP as its oracle —
 * the dry ragged wall STANDS untouched (the control that keeps "settle at build time" from
 * being satisfied by an implementation that knocks down every structure), and the bare corbel
 * arm SETTLES on spawn with no click, no removal, no menu row.
 *
 * BOTH HALVES OF THE OUTCOME ARE ASSERTED, per DESIGN.md 4: the released arm bricks must have
 * MOVED under gravity (a break stamp and a released flag are only steps), and the standing base
 * and the standing wall must NOT have moved in the same second (or "it fell" is satisfied by a
 * world that dropped through its own floor). AND WHICH AXIS GOVERNS IS ASSERTED RATHER THAN
 * ASSUMED — the dry corbel is compression-on-reduced-contact (zero tension), the mortared one
 * is tension, and the hand arithmetic pins each so a run in which the wrong axis had taken over
 * would show as a number that no longer matched rather than a silent pass.
 *
 * NEEDS A TICKING WORLD: yes, for the wire and the movement — the solver's own answer is
 * world-free and is established first, with no actors, so everything after is about the wire.
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
	 * ================================================================================
	 * FIRST, THE LOAD MODEL AS ARITHMETIC, WITH NO WORLD IN THE WAY.
	 * ================================================================================
	 *
	 * Core/Layout and FStructure are world-free, so what each structure carries costs
	 * milliseconds and no actors. Establishing it here is what lets everything after be a
	 * statement about the WIRE rather than the load model.
	 */

	/*
	 * THE DRY RAGGED WALL STANDS — the flipped expectation, and the heart of the re-derivation.
	 */
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
		 * THE TOP CORBEL, FOUND BY GEOMETRY RATHER THAN BY INDEX: the end brick of the highest
		 * EVEN course, and the one brick still under it half a step to its right.
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
		 * WHAT THAT JOINT CARRIES, against the hand arithmetic: F = 1.5 brick weights (its own
		 * weight plus the centred half share from the brick above), M = 5.625 brick-weight-cm
		 * (its own weight is the only load with a lever arm), so the effective arm is 3.75 cm.
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
		 * AND IT STANDS ON ITS REDUCED CONTACT, WHICH IS THE FLIPPED READING. A dry bed cannot
		 * pull, so the joint cracks and bears on L_c = 3(h/2 - e) = 4.125 cm; the squeezed fibre
		 * is compression-governed at 0.000631 of dry stone's 30 MPa, with zero tension. The hand
		 * value and the solver's must agree, which is the axis check — a joint that had failed at
		 * the retired kern would read the sentinel (~1.8e308) here instead.
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
		 * AND NOT ONE JOINT IN THE WHOLE WALL IS OVER CAPACITY. Every corbel below the top carries
		 * MORE closing compression at the same 5.625 cm arm, so its resultant sits even further
		 * inside the face; the dry ragged wall stands at every joint.
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

		/*
		 * SO THE LP, THE BREAK AUTHORITY BELOW THE CAP, STANDS THE WHOLE THING: settling it breaks
		 * nothing at all. This is the authority whose verdict the wire reproduces further down.
		 */
		TestEqual(
			TEXT("settling the dry ragged wall must break nothing: the LP stands it entire"),
			Dry.Structure.SolveAndBreak(), 0);
	}

	/*
	 * THE MORTARED CONTROL: identical geometry, bonded joints. The no-tension relief never fires
	 * on a bonded joint, so the corbel keeps its uncracked TENSION reading and the wall's worst
	 * joint is 0.0065014926 — under capacity, tension-governed, and unchanged by the dry edge
	 * rule. This is what says raggedness alone does not condemn a wall.
	 */
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
	 * THE DRY FLUSH CONTROL: no bond AND no corbel. A half bat fills each end, the end brick gets
	 * two supports, its centre of mass lands on their area-weighted centroid, e = 0, and there is
	 * no tension anywhere for a joint with no tensile capacity to fail at. This isolates the
	 * corbel, not the missing bond, as what an over-capacity dry structure needs.
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
	 * THE SCENARIO WALL, UNTOUCHED — the game mode's own 40 x 30 flush mortared wall, 1220 pieces
	 * over the block cap, so the ROUTER is its authority. e = 0 at every flush seat, so its worst
	 * joint is compression-governed and exactly 0.00495042219, and it stands.
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
	 * NOW THE STRUCTURE THAT GENUINELY CANNOT HOLD ITSELF UP: the bare dry corbel arm. The router
	 * reads all 10 arm bed joints over capacity (each arm brick's resultant is at e = 5.625 cm,
	 * past the face), and the LP — the authority below the cap — sheds exactly the 10 arm bricks
	 * while the grounded base stands. Nothing is Stranded: this is a collapse, not a solver stall.
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

	/*
	 * ================================================================================
	 * NOW THE WIRE: BUILD EACH IN A WORLD AND SOLVE-AND-PUSH IT ONCE, WITH NO CLICK.
	 * ================================================================================
	 */
	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	/*
	 * THE STANDING CONTROL FIRST. A dry ragged wall built in the world and pushed once must
	 * release NOTHING — the half of "settles at build time" that stops it being satisfied by an
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
	 * THE OVER-CAPACITY SUBJECT: build the bare corbel arm and ask ONCE. Nothing is removed,
	 * nobody clicks — this is the wire the game mode runs on BeginPlay, and the claim is that a
	 * structure which cannot hold itself up settles the instant it is built.
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

	/* THE ONLY CALL THIS TEST MAKES ON THE ARM — one push, no click. */
	const int32 Released = TestWorld.Subsystem->SolveAndPush(ArmId);

	TestEqual(
		FString::Printf(
			TEXT("building an arm that cannot hold itself up must settle it there and then: %d should release, %d did"),
			ExpectedReleased, Released),
		Released, ExpectedReleased);

	/*
	 * THE MECHANISM BEHIND IT: JOINTS GAVE UNDER LOAD. A break pass is a stamp a plain solve can
	 * never produce, so this separates a genuine settle from a wall that merely reads over
	 * capacity.
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
	 * WHICH BRICKS, NOT HOW MANY: the base must survive and the arm must go, each checked against
	 * the producer's own oracle, so a push that came apart in the wrong place is caught even
	 * though the count would agree. And the flag must have reached the body — ABrickActor keeps
	 * no second copy of the answer.
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
	 * ================================================================================
	 * THE OUTCOME. REAL GRAVITY ON A FIXED STEP, AND THE ARM COMES DOWN WHILE THE BASE DOES NOT.
	 * ================================================================================
	 *
	 * The claim is RELATIONAL rather than a fitted landmark, because released arm bricks tumble
	 * off a cantilever and pile where they may: the released centre of mass must END BELOW where
	 * it started (real downward motion, and a 0/0 NaN failing the strict test if nothing was
	 * released) and STAY ABOVE THE FLOOR (it settled rather than tunnelling out of the world),
	 * every released brick must have moved more than the kinematic-vs-dynamic threshold (the wire
	 * handed it to physics), and every surviving base brick must NOT have moved (the world did
	 * not simply drop through its floor).
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
