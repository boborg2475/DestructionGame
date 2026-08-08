// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

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
	 * THE WALL THAT CANNOT HOLD ITSELF UP, AND THE THREE THAT CAN.
	 * ================================================================================
	 *
	 * Four walls the same producer lays from the same brick on the same grid, differing
	 * only in how they finish at their ends and in what is in the joint. Everything below
	 * is derived here rather than imported, and the numbers are measurements of the
	 * SOLVER, which is unchanged by any of this — the question is only whether the world
	 * wire asks it to settle.
	 *
	 * WHY A RAGGED END CORBELS AND A FLUSH ONE DOES NOT, WHICH IS HALF THE FIXTURE.
	 * Running bond offsets alternate courses by half a cell, 11.25 cm. Where a course
	 * steps in, the end brick of the course ABOVE it overhangs into thin air and keeps
	 * exactly ONE bed joint — the 10.25 cm strip it still shares with the brick below —
	 * whose centroid sits 5.625 cm to the inside of the brick's own centre of mass. That
	 * 5.625 cm is a genuine eccentricity, and it is the identical lever arm the staircase
	 * corbel has (StaircaseWallTestSupport works that one through). A flush end fills the
	 * half cell with a half bat, the end brick gets two supports, its centre of mass lands
	 * on the area-weighted centroid of them, and the eccentricity is exactly zero.
	 *
	 * AND WHY RAGGED ALONE IS NO LONGER ENOUGH, WHICH IS THE OTHER HALF AND IS CORRECT
	 * STATICS RATHER THAN A WEAKENING. A plain toothed end is the ZIG-ZAG case: the joint
	 * the course above arrives through has the SAME centroid as the joint the end brick
	 * leaves by, so the load handed down carries no lever arm across it, and the arm on a
	 * ragged end joint is pinned at its own 5.625 cm forever while the compression under
	 * it grows course by course. Bending therefore stays put and the compression that
	 * closes the joint only ever increases, so the ratio PEAKS NEAR THE TOP OF THE WALL
	 * AND FALLS BELOW IT: a mortared ragged wall reads 0.0455104479 at its worst joint at
	 * any height that finishes on an odd course, and 0.0582038382 at one finishing on an
	 * even course. NO MORTARED RAGGED WALL OF ANY HEIGHT IS OVER CAPACITY AS BUILT.
	 *
	 * THE TWO FIGURES ARE STILL READ AGAINST DIFFERENT LOADS, WHICH IS WHY THE SMALLER ONE
	 * IS THE ODD-COURSE WALL, BUT BOTH ARE NOW THE BED PATCH'S OWN. A wall finishing on an
	 * ODD course has one brick standing on its top corbel, so that joint carries 1.5 brick
	 * weights of closing compression against the same 5.625 brick-weight-cm of bending, and
	 * reads 0.0455104479. A wall finishing on an EVEN course has its worst joint under the
	 * topmost brick itself, with nothing resting on it at all, so it carries 1.0 and reads
	 * 0.0582038382. Neither gets any composite relief, and the odd-course one is where the
	 * whole of COMPOSITE_DEPTH_DESIGN.md's depth rule shows up in this file — see the block
	 * on TopCorbelCompositeDepthCm, which is a good deal more interesting than it looks.
	 *
	 * SO THE WALL THAT CANNOT HOLD ITSELF UP IS THE ONE WITH NOTHING IN THE JOINT. Laid
	 * DRY — DestructionProfiles::DryStone, whose cohesion and tensile strength are exact
	 * zeroes rather than small numbers, because there is no bond there at all — a corbel
	 * whose bed joint is in net tension anywhere on its face has no capacity to resist it
	 * and gives immediately. That is not a contrived wall either: dry-stone walling is a
	 * real technique, DryStone is a shipped profile with published figures, and a player
	 * who stacks bricks dry with toothed ends is entitled to watch the ends come off.
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

	/**
	 * TWENTY-FOUR COURSES OF SIX, AND BOTH NUMBERS ARE MEASURED RATHER THAN LIKED.
	 *
	 * SIX WIDE because the corbel is a LOCAL phenomenon at each end — the worst joint of a
	 * ragged wall reads the same 0.0455104479 at six bricks wide, at ten and at thirty —
	 * so width buys nothing but actors. What width DOES decide is the size of the triangle
	 * that survives the settle (see ShouldSurviveSettling): the collapse front retreats one
	 * brick position every two courses from each end, so the two fronts meet at course
	 * BricksPerCourse and the survivors are a triangle 6 + 5 + 4 + 3 + 2 + 1 = 21 pieces
	 * tall, whose apex tops out at Z 44 WHATEVER THE WALL'S HEIGHT.
	 *
	 * TWENTY-FOUR HIGH BECAUSE A SHORTER ONE DOES NOT ACTUALLY COME DOWN, and that was
	 * measured rather than reasoned about. The staircase leaves both ends of courses 2 to
	 * 5 propped on the two complete bottom courses, so a short wall's released set simply
	 * SITS DOWN on its own foot: at 16 courses the released centre of mass falls 72.8 to
	 * 66.5 cm and stops — 6 cm, with only 22 of 67 bricks moving a whole course — which is
	 * a correct outcome and a useless one to assert on. At 24 the released mass is five
	 * times the triangle it lands on, the pile cannot stand in a wall one brick thick, and
	 * the centre of mass goes 103.0 to 1.9 in three seconds with 70 of 111 bricks past a
	 * course. THE FIXTURE HAD TO BE BIG ENOUGH TO FALL OVER, and the failure mode of
	 * guessing here is a green test about a wall that shrugged.
	 *
	 * 132 ACTORS, against the 380 the fixture this replaces needed, and the test ticks
	 * every one of them for four simulated seconds.
	 */
	constexpr int32 OverCapacityWallCourses = 24;
	constexpr int32 OverCapacityWallBricksPerCourse = 6;

	/** 12 even courses of 6 plus 12 odd courses of 5. */
	constexpr int32 OverCapacityWallPieceCount = 12 * 6 + 12 * 5;

	/** The wall that cannot hold itself up: toothed ends, and nothing in the joint. */
	inline FRunningBondSpec DryRaggedWallSpec(int32 CoursesHigh, int32 BricksPerCourse)
	{
		return WallSpecOf(CoursesHigh, BricksPerCourse, EWallEnd::Ragged, DryStone);
	}

	/** The game mode's own scenario wall: 40 courses of 30, FLUSH, mortared. */
	inline FRunningBondSpec ScenarioWallSpec()
	{
		return WallSpecOf(40, 30, EWallEnd::Flush, GeneralPurposeMortar);
	}

	/*
	 * ================================================================================
	 * WHAT THE TOP CORBEL CARRIES, WORKED THROUGH HERE AND NOT READ OFF THE SOLVER.
	 * ================================================================================
	 *
	 * A full brick is 21.5 x 10.25 x 6.5 cm of clay at 1.9 g/cm3, so
	 * W = 2.72163125 kg x 980 cm/s2 = 2667.198625 Unreal force units — and DESIGN.md §3's
	 * 1 N = 100 uu is already inside that product and must not be applied again.
	 *
	 * THE TOP CORBEL IS THE END BRICK OF THE HIGHEST EVEN COURSE, and everything about it
	 * is countable off the bond rather than recursive:
	 *
	 *   - it keeps ONE bed joint, the 10.25 x 10.25 cm strip it still shares with the
	 *     single brick below, so A = 105.0625 cm2 and its own weight acts 5.625 cm from
	 *     that strip's centroid;
	 *   - the ONE brick above it — the end brick of the odd top course — sits on TWO equal
	 *     supports, so half its weight comes down here, and it arrives through a patch
	 *     with the SAME centroid, which is the zig-zag: force, no arm.
	 *
	 * So F = 1.5 W and M = 5.625 W exactly, with no series to sum. Bending about the
	 * joint's Y axis is resisted by W_v = (4/3) x 5.125 x 5.125^2 = 179.4817708 cm3, and
	 * beam theory on an uncracked rectangle puts the fibre stress at |M|/W_v -+ N/A:
	 *
	 *     tension     = 5.625 W / 179.4817708 - 1.5 W / 105.0625  = +0.00455104 MPa
	 *     compression = 5.625 W / 179.4817708 + 1.5 W / 105.0625  = +0.01216708 MPa
	 *
	 * both after dividing by the 10000 uu per MPa.cm2 spelled out below rather than
	 * imported.
	 *
	 * WHICH AXIS GOVERNS IS ASSERTED, NEVER ASSUMED, because ComputeUtilisation returns
	 * the WORST of three and a fixture aimed at tension that silently measured compression
	 * would be a green test about nothing. Against DryStone's 0.0 MPa tensile and 30 MPa
	 * compressive the two ratios are "infinite" and 0.000406; against general purpose
	 * mortar's 0.10 and 10.0 they are 0.0455104479 and 0.00121671. Tension governs by
	 * three orders of magnitude either way, and the shear axis carries nothing at all —
	 * gravity is normal to a bed joint.
	 *
	 * AND WHY ONLY THE TOP TWO CORBELS OF EACH END ARE OVER CAPACITY, WHICH IS THE FIXTURE
	 * PRECONDITION WITH THE MOST TEETH IN IT. The arm is the same 5.625 cm on every rung
	 * while the force grows downward, so the joint stops opening once the compression
	 * closes it: net tension survives only while F < M x A / W_v = 5.625 x 105.0625 /
	 * 179.4817708 = 3.29270 brick weights. Reading the ladder down from the top, the
	 * corbels carry 1.5, 2.75, 3.9375, 5.09375, 6.230469, 7.354492 and 8.470215 W — so
	 * exactly TWO rungs per end fall under 3.2927 and exactly four joints in the wall are
	 * over capacity as built. Everything below them comes down in the CASCADE, as the
	 * wall above them is shed and their own compression goes with it.
	 */

	/** 1.9 g/cm3 x 21.5 x 10.25 x 6.5 cm / 1000 = 2.72163125 kg, x 980 cm/s2. */
	constexpr double FullBrickWeightUu = 1.9 * 21.5 * 10.25 * 6.5 / 1000.0 * 980.0;

	/** The half-brick bed patch a corbel keeps: 10.25 along the wall by 10.25 through it. */
	constexpr double CorbelBedAreaSqCm = 10.25 * 10.25;

	/** (4/3) x h_u x h_v^2 for that square patch, cm3. */
	constexpr double CorbelSectionModulusCm3 =
		(4.0 / 3.0) * (10.25 / 2.0) * (10.25 / 2.0) * (10.25 / 2.0);

	/** Half of one half-step: the brick keeps half a cell, so its centre is 22.5 / 4 out. */
	constexpr double CorbelOwnWeightArmCm = 22.5 / 4.0;

	/**
	 * Unreal force units per MPa per cm2, SPELLED OUT RATHER THAN IMPORTED.
	 *
	 * DESIGN.md §3's whole units section exists because a missing or duplicated conversion
	 * is out by exactly 100x and tuned thresholds conceal it well. Production has one named
	 * boundary for this; a test that read it would agree with a wrong one.
	 */
	constexpr double ForceUnitsPerMPaSqCmHere = 10000.0;

	/** EN 1996-1-1 Table 3.2's f_xk1 for general purpose mortar, asserted against the profile. */
	constexpr double MortarTensileMPa = 0.10;

	/** Dry stone has no bond at all, so this is an EXACT zero rather than a small number. */
	constexpr double DryStoneTensileMPa = 0.0;

	/** Force and moment on the top corbel's one bed joint, in brick weights. */
	constexpr double TopCorbelForceBrickWeights = 1.5;
	constexpr double TopCorbelMomentBrickWeightCm = CorbelOwnWeightArmCm;

	/** Where the corbel stops opening: F x W_v / (M x A), in brick weights. */
	constexpr double CorbelClosesAboveBrickWeights =
		CorbelOwnWeightArmCm * CorbelBedAreaSqCm / CorbelSectionModulusCm3;

	/** Two rungs per end are under that figure, so four joints in the wall are over capacity. */
	constexpr int32 OverCapacityJointsAsBuilt = 4;

	/** The peak fibre stresses on that joint, MPa, from the block above. */
	constexpr double TopCorbelTensileStressMPa =
		FullBrickWeightUu
		* (TopCorbelMomentBrickWeightCm / CorbelSectionModulusCm3
			- TopCorbelForceBrickWeights / CorbelBedAreaSqCm)
		/ ForceUnitsPerMPaSqCmHere;

	/*
	 * ================================================================================
	 * HOW DEEP A SECTION THIS JOINT IS ALLOWED, AND WHY THE ANSWER IS "NOT DEEP ENOUGH
	 * TO HELP" — THE COUNTER-EXAMPLE THAT KILLED THE HALF-SEAT LEMMA.
	 * ================================================================================
	 *
	 * Two courses DO stand over this joint: the top corbel is the end brick of the highest
	 * EVEN course and the odd top course's end brick rests on it, so there is 15 cm of bonded
	 * masonry there and t*D^2/6 would be 384.375 cm3 against the patch's 179.4817708. That is
	 * the section this file credited through slice 4 of COMPOSITE_DEPTH_DESIGN.md, and it read
	 * 0.039032175.
	 *
	 * BUT THE MASONRY STANDING OVER A JOINT IS ONLY THE FIRST OF THREE TERMS. Slice 1 bounds
	 * the credited depth by the joint's own statical lever arm, because masonry above a cut is
	 * not being bent by the cut's moment and has to be dragged into the section by shear over
	 * a distance; and the corbelling body's own depth is a FLOOR under that, because the
	 * courses that GENERATE the moment are one cantilevering body and need no shear transfer
	 * to be engaged. The rule is
	 *
	 *     D  =  min( masonry above , max( the corbelling body's own depth , lambda*|M|/|F| ) )
	 *
	 * THE DESIGN PREDICTED THIS JOINT WAS UNTOUCHABLE, AND IT WAS WRONG, AND THE ARITHMETIC
	 * IS RIGHT HERE IN THE FIXTURE. Its "half-seat lemma" argued that any half seat carries
	 * its load at the half-seat eccentricity, so e >= 5.625 cm and lambda*e >= 19.49 cm, which
	 * is 2.6 courses — hence no joint with two or fewer courses over it could ever be trimmed.
	 * This joint refutes it: it carries 1.5 brick weights and 5.625 brick-weight-cm, so
	 *
	 *     e  =  |M| / |F|  =  5.625 / 1.5  =  3.75 cm,   NOT 5.625
	 *
	 * because THE EXTRA HALF BRICK ARRIVES CENTRED. The brick above the corbel hands its half
	 * share down through a patch with the SAME centroid — the zig-zag this file has always
	 * been about — so F grows from 1.0 to 1.5 and M does not grow at all. An increment with no
	 * arm DILUTES the arm, and the lemma assumed every increment arrives with one.
	 *
	 * SO THE ARM GOVERNS: lambda*e = 3.464 x 3.75 = 12.99 cm, against a corbelling body one
	 * course deep (7.5 cm — the brick above the corbel sits squarely on TWO supports, so the
	 * corbelling chain stops at the corbel itself) and 15 cm of masonry above. The floor does
	 * not reach it and the wall does not cap it, and 12.99 cm is what the joint is read over.
	 *
	 * AND THE NUMBER GOING UP IS THE RELIEF BEING DECLINED, NOT A REGRESSION. THIS IS THE
	 * PART THAT READS LIKE A DEFECT AND IS NOT. A 12.99 cm section is 288.2643375 cm3 — still
	 * DEEPER than the bed patch's 179.4817708 — but the composite reading carries NO AXIAL
	 * RELIEF, because the plane resisting a deep-beam moment is vertical and the wedge's
	 * weight is shear on it rather than load across it. So the two readings are
	 *
	 *     composite  =  5.625 W / 288.2643375                =  0.0052045953 MPa
	 *     patch      =  5.625 W / 179.4817708 - 1.5 W / 105.0625  =  0.0045510448 MPa
	 *
	 * and the composite one is WORSE. ComputeUtilisation takes the LESSER of the two demands —
	 * composite action is an alternative way of carrying the moment and may only ever help —
	 * so it refuses the relief and the joint keeps the bed patch's own 0.0455104479. The wall
	 * therefore reads HIGHER than it did at slice 4's 0.039032175, and that is the rule being
	 * applied faithfully rather than a load model that drifted.
	 *
	 * HOW MUCH DEPTH IT WOULD HAVE TAKEN, DERIVED BELOW RATHER THAN ASSERTED AS A FEELING.
	 * The composite section only undercuts the patch past
	 *
	 *     D_cross  =  sqrt( 6 M W / (t x 10^4 x sigma_patch) )  =  13.891434 cm  =  1.852 courses
	 *
	 * and lambda*e falls 6.5% short of it. Two courses (15 cm) clears it, which is exactly why
	 * the unbounded rule fired here and the bounded one does not.
	 *
	 * NOTHING ABOUT THE OUTCOME MOVED, THROUGH ANY OF IT. Every reading in sight is two orders
	 * of magnitude under capacity, so a mortared ragged wall still stands as built at any
	 * height, and the DRY wall is still condemned at the same joint — dry stone's f_xk1 is an
	 * exact zero, so any tension whatever has already gone, at any section modulus.
	 */

	/**
	 * lambda, THE COMPOSITE DEPTH PER UNIT OF EFFECTIVE ARM, SPELLED OUT RATHER THAN IMPORTED.
	 *
	 * 3.464 is 2*sqrt(3), and COMPOSITE_DEPTH_DESIGN.md is explicit that it is a RULING inside
	 * a window and not a derivation — slice 3 exists so somebody makes it knowingly. A test
	 * that reached for the solver's own constant would agree with a wrong one, and this one has
	 * to fail if it moves, because whether this joint gets composite relief at all turns on it.
	 */
	constexpr double CompositeDepthPerArm = 3.464;

	/** e = |M|/|F| = 3.75 cm. The half-seat lemma said this could not be under 5.625. */
	constexpr double TopCorbelArmCm =
		TopCorbelMomentBrickWeightCm / TopCorbelForceBrickWeights;

	/** What the arm permits: 12.99 cm. */
	constexpr double TopCorbelPermittedDepthCm = CompositeDepthPerArm * TopCorbelArmCm;

	/**
	 * The corbelling body under it is ONE course, and that is PROVEN by the reading rather
	 * than assumed here: a body is a whole number of course pitches, so a credited depth of
	 * 12.99 cm rules out two courses (15 cm would have floored it there) and one course is
	 * all that is left. Asserted below in exactly those terms.
	 */
	constexpr double TopCorbelBodyDepthCm = 6.5 + 1.0;

	/** And the masonry standing over it: the corbel and the one brick on it, 15 cm. */
	constexpr double TopCorbelMasonryAboveCm = 2.0 * (6.5 + 1.0);

	/* max(body, lambda*e), then min with the masonry above — the rule, written out. */
	constexpr double TopCorbelCreditableDepthCm =
		TopCorbelBodyDepthCm > TopCorbelPermittedDepthCm
			? TopCorbelBodyDepthCm
			: TopCorbelPermittedDepthCm;

	constexpr double TopCorbelCompositeDepthCm =
		TopCorbelMasonryAboveCm < TopCorbelCreditableDepthCm
			? TopCorbelMasonryAboveCm
			: TopCorbelCreditableDepthCm;

	constexpr double TopCorbelCompositeModulusCm3 =
		10.25 * TopCorbelCompositeDepthCm * TopCorbelCompositeDepthCm / 6.0;

	constexpr double TopCorbelCompositeStressMPa =
		FullBrickWeightUu * TopCorbelMomentBrickWeightCm
		/ (TopCorbelCompositeModulusCm3 * ForceUnitsPerMPaSqCmHere);

	/**
	 * WHAT THE UNBOUNDED RULE GAVE — the two-course section, kept only to be printed.
	 *
	 * 10.25 x 15^2 / 6 = 384.375 cm3 and 15003.0 / (384.375 x 10^4) / 0.10 = 0.039032175, the
	 * figure this file pinned through slice 4. Nothing is derived from it; it is here so that
	 * a reader who remembers the old number can see where it came from and that it did not
	 * drift, it stopped being reachable.
	 */
	constexpr double TwoCourseCompositeStressMPa =
		FullBrickWeightUu * TopCorbelMomentBrickWeightCm
		/ ((10.25 * TopCorbelMasonryAboveCm * TopCorbelMasonryAboveCm / 6.0)
			* ForceUnitsPerMPaSqCmHere);

	/**
	 * The SAME wall in mortar, at the same joint: 0.00455104 MPa against 0.10 MPa.
	 *
	 * PINNED AS A NUMBER RATHER THAN AS "under 1", so a load model that drifted is visible
	 * here rather than silently keeping the test green from the safe side of the line. It
	 * is the figure the ragged wall reads at ANY height finishing on an odd course.
	 *
	 * IT IS THE BED PATCH'S OWN READING, and it is the same figure this file pinned through
	 * slice 4 for the same reason it pins it now — the joint has never been able to use a
	 * section that helped it. Slice 5 of ARCHING_DESIGN briefly credited it 15 cm and
	 * 0.039032175; slice 1 of COMPOSITE_DEPTH_DESIGN took that back, by bounding the depth at
	 * lambda*e = 12.99 cm, where the composite demand is WORSE than the patch's and is
	 * declined. See the block above — this number going UP is the point, not a defect.
	 */
	constexpr double MortarRaggedWorstAsBuilt = 0.0455104479;

	/** The scenario wall's worst joint — MOMENTS_DESIGN.md's regression anchor, and exact. */
	constexpr double ScenarioWorstAsBuilt = 0.00495042219;

	constexpr int32 ScenarioWallPieceCount = 1220;

	/** A dry FLUSH wall of the same size has no corbel anywhere, and reads this. */
	constexpr double DryFlushWorstAsBuilt = 0.000971218165;

	/** 12 even courses of 6 full bricks, 12 odd courses of 5 full bricks and 2 half bats. */
	constexpr int32 DryFlushWallPieceCount = 12 * 6 + 12 * 7;

	/* The coordinating grid: a brick plus a joint, and half of it is the bond offset. */
	constexpr double CoursePitchCm = 6.5 + 1.0;
	constexpr double BrickPitchCm = 21.5 + 1.0;

	/** Which course a laid box is in, read back off its height. */
	inline int32 CourseOf(const FPieceBox& Box)
	{
		return FMath::RoundToInt32((Box.CentreCm.Z - 6.5 * 0.5) / CoursePitchCm);
	}

	/** How many brick positions in from the nearer end of its own course a box sits. */
	inline int32 PositionFromNearestEnd(const FPieceBox& Box, int32 Course, int32 BricksPerCourse)
	{
		/* Odd courses step in half a cell and are one brick short. */
		const int32 BricksInCourse = (Course % 2 == 0) ? BricksPerCourse : BricksPerCourse - 1;
		const double FirstXCm = (Course % 2 == 0) ? 0.0 : BrickPitchCm * 0.5;

		const int32 FromLeft = FMath::RoundToInt32((Box.CentreCm.X - FirstXCm) / BrickPitchCm);

		return FMath::Min(FromLeft, BricksInCourse - 1 - FromLeft);
	}

	/**
	 * WHICH BRICKS A RAGGED WALL SHEDS WHEN IT SETTLES — THE ORACLE, DERIVED FROM THE BOND
	 * RATHER THAN READ BACK OFF THE SOLVER.
	 *
	 * The collapse front is a staircase, and it is the same staircase a player cuts by hand.
	 * The outermost brick of a course is a corbel and gives; the brick that was resting on it
	 * loses a support and becomes the corbel of the course above; and because running bond
	 * offsets by HALF a cell, the front retreats 11.25 cm per course — one whole brick
	 * position every TWO courses. So course c loses floor(c / 2) bricks from each end.
	 *
	 * Courses 0 and 1 lose nothing: course 0 is grounded, and course 1's end brick still
	 * rests squarely on two bricks of it. The two fronts meet when floor(c / 2) x 2 reaches
	 * the number of bricks in the course, and everything above that has nothing left under it
	 * at all — at six wide that is course 6, so 21 of 132 pieces survive, in a triangle
	 * 6, 5, 4, 3, 2, 1 courses tall, and it tops out at Z 44 however tall the wall was.
	 *
	 * WHY THE TRIANGLE IS WHERE THE FRONT STOPS, AND IT IS GEOMETRY RATHER THAN A THRESHOLD.
	 * Every brick it leaves standing has TWO supports again: a surviving brick at position
	 * floor(c / 2) of course c sits at X = 11.25c, and the two survivors nearest it in course
	 * c - 1 sit at X = 11.25c -+ 11.25, so it keeps a 10.25 cm strip on each and its centre of
	 * mass lands exactly on the area-weighted centroid of the pair. Zero eccentricity, no
	 * moment, no tension — which is why the front halts there for a DRY wall with no tensile
	 * capacity at all, and not merely for one whose corbels happened to stay under a limit.
	 *
	 * THIS IS THE CONSEQUENCE OF SETTLING AT BUILD TIME, WRITTEN DOWN RATHER THAN DISCOVERED.
	 * It is a lot of wall, and it is correct: those bricks were never being held up, and the
	 * only thing the old wire did for them was decline to ask.
	 */
	inline bool ShouldSurviveSettling(const FPieceBox& Box, int32 BricksPerCourse)
	{
		const int32 Course = CourseOf(Box);

		return PositionFromNearestEnd(Box, Course, BricksPerCourse) >= Course / 2;
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
	 * How still "the collapse has finished" is, in cm of centre of mass over the last second.
	 *
	 * The rubble moves 0.087 cm between three seconds and four, having travelled 101 cm to
	 * get there; the seconds after that move two thousandths. 1 cm is eleven times the
	 * former and five hundred times the latter, so it is a floor under "at rest" rather than
	 * a fit to it — and it is still less than the nominal joint one brick could settle into.
	 */
	constexpr double RubbleAtRestCm = 1.0;

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
 * A WALL THAT CANNOT HOLD ITSELF UP COMES DOWN WHEN IT IS BUILT, NOT WHEN SOMEBODY
 * HAPPENS TO CLICK.
 *
 * WHAT WAS WRONG, AND WHAT THIS GUARDS NOW THAT IT IS NOT.
 * UDestructionStructureSubsystem::SolveAndPush used to call FStructureBinding::SolveLoads,
 * which is documented as NON-DESTRUCTIVE and breaks nothing however overloaded a joint is,
 * while the commit doors — RunPieceAction and RunPieceActions — called SolveAndBreak. So a
 * structure that was over capacity the moment it was laid stood there indefinitely, and
 * then shed the moment the player right-clicked ANY brick anywhere in it: this fixture's
 * wall has four joints past capacity before anybody touches it, and deleting one brick at
 * the far end of the far course takes 124 joints out in 23 passes. The collapse was real;
 * the attribution was a lie, and the player was told they had done something they had not.
 *
 * SO THIS TEST IS A REGRESSION NET RATHER THAN A DRIVER, AND IT IS SAID PLAINLY. The line
 * it exists for is the SolveAndBreak in SolveAndPush, and that line is already there. PROOF
 * THAT IT BITES, run rather than asserted: reverting that one call to SolveLoads leaves the
 * wall standing with 0 pieces released against 111 expected, 0 joints carrying a break pass,
 * every one of the 111 released bricks reading EXACTLY 0.000000 cm of movement, and the
 * released centre of mass sitting where it was laid at Z 103.047 against a triangle whose
 * top is Z 44. It is the fixture that had to be replaced, not the claim.
 *
 * THE DECISION IS TO SETTLE AT BUILD TIME. A wall that cannot hold itself up should not
 * stand waiting for a click. That is DESIGN.md §3's own rule — a joint over capacity gives,
 * and a pass that breaks nothing is the last one — applied at the only seam that had been
 * left out of it, and it removes the attribution problem rather than hiding it. The
 * rejected alternative was to make "nothing over capacity" a checked precondition of
 * BuildRunningBond, which would have the producer REFUSE geometry that is merely weak; a
 * weak wall is a real thing a player should be allowed to build and watch fall.
 *
 * THE WALL IS BUILT OVER CAPACITY BY ONE CALL TO THE PRODUCER, AND THAT IS DELIBERATE
 * RATHER THAN CONVENIENT. Nothing below removes a piece, cuts a void or resolves a menu
 * row: BuildRunningBond lays it, SolveAndPush is asked once, and the claim is that ONE
 * call settles it. A fixture that had to be carved into shape first would leave "built"
 * and "clicked" arguable, which is the whole distinction under test.
 *
 * THE CONSEQUENCE, STATED RATHER THAN DISCOVERED, AND IT IS LARGE. A dry-laid ragged wall
 * now collapses from its own ends on spawn, and the collapse front is a staircase retreating
 * half a brick per course — so at six bricks wide the two fronts meet at course 6 and 111
 * of 132 pieces come down, leaving a triangle. See ShouldSurviveSettling for the derivation.
 * That is correct: those bricks were never being held up. The ONLY thing making them look
 * held up was a wire that declined to ask.
 *
 * AND THREE WALLS MUST BE COMPLETELY UNAFFECTED, WHICH IS THE OTHER HALF AND NOT DECORATION.
 * Without them, "settle at build time" is satisfied by an implementation that knocks down
 * every wall in the game — and each of the three subtracts a different candidate
 * explanation. The SAME ragged wall MORTARED reads 0.0455104479, so it is not the toothed
 * end on its own. The same DRY wall finished FLUSH reads 0.000633860176, so it is not the
 * missing bond on its own. And the wall ADestructionGameGameMode actually lays — flush,
 * mortared, 40 courses of 30 — reads 0.00495042219, so nothing in the game's own scenario
 * moves. All three must settle in zero breaking passes.
 *
 * WHY BOTH HALVES OF THE OUTCOME ARE ASSERTED. DESIGN.md §4 is explicit that an integration
 * test measures the structure actually moving: a break stamp is a step, and a released flag
 * is a step, so the bricks are ticked for a second and the wall is required to have come
 * down. And "the ends came down" alone passes for a world that dropped through its floor, so
 * the surviving triangle is required not to have moved in the same second.
 *
 * THE OUTCOME IS MEASURED ON THE WHOLE RELEASED SET RATHER THAN BRICK BY BRICK, and the
 * block that does it says why at length: a staircase collapse has no clear air at its foot,
 * so a per-brick fall distance is a prediction about RUBBLE and not about the bond.
 *
 * AND NOTHING IS Stranded. A ragged wall is exactly the shape that could make an unroutable
 * knot, and a wall that came down because the solver declined to divide load round a loop
 * would be a model limitation wearing a collapse's clothes.
 *
 * AND WHICH AXIS GOVERNS IS ASSERTED RATHER THAN ASSUMED. GetConnectionUtilisation returns
 * the worst of compression, shear and tension, so a fixture aimed at a corbel levering its
 * joint OPEN would read exactly the same if compression happened to be the governing axis
 * — and would then be measuring nothing anybody wrote down. The worked arithmetic is on
 * TopCorbelTensileStressMPa, and the block below checks the force and the moment the joint
 * actually carries against it before claiming anything about capacity.
 *
 * NEEDS A TICKING WORLD: yes, and it is the point — this is about the wire between the
 * solver and the world, and the solver's own answer is unchanged by any of it.
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
	 * FIRST, THE FOUR WALLS AS ARITHMETIC, WITH NO WORLD IN THE WAY.
	 * ================================================================================
	 *
	 * Core/Layout and FStructure are world-free, so what each wall carries costs
	 * milliseconds and no actors. Establishing it here is what lets everything below be a
	 * statement about the WIRE rather than about the load model — the solver's answer is
	 * the same before and after this change, and this block is where that is pinned.
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

		/*
		 * THE PROFILE IS ASSERTED, NEVER TRUSTED. The whole fixture is "there is no bond in
		 * this joint", so a DryStone row that quietly grew a tensile strength would leave
		 * every claim below measuring something else. The figures are written here rather
		 * than read across, exactly as the conversion constant is.
		 */
		TestEqual(
			FString::Printf(TEXT("fixture: DryStone's tensile strength must be an exact zero, it is %g MPa"),
				DryStone.TensileStrengthMPa),
			DryStone.TensileStrengthMPa, DryStoneTensileMPa);

		TestEqual(
			FString::Printf(TEXT("fixture: GeneralPurposeMortar's f_xk1 must be %g MPa, it is %g"),
				MortarTensileMPa, GeneralPurposeMortar.TensileStrengthMPa),
			GeneralPurposeMortar.TensileStrengthMPa, MortarTensileMPa);

		Dry.Structure.SolveLoads();

		/*
		 * THE JOINT THE WHOLE FIXTURE RESTS ON, FOUND BY GEOMETRY RATHER THAN BY INDEX: the
		 * end brick of the highest EVEN course, and the one brick still under it half a step
		 * to its right. A joint handle is an artefact of the order the producer emits pairs
		 * in, and a fixture that hard-coded one would silently start watching a different
		 * joint the day that order changed.
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
		 * WHAT THAT JOINT CARRIES, AGAINST THE HAND ARITHMETIC AND NOT AGAINST ITSELF. The
		 * force is 1.5 brick weights because the corbel's own weight plus half of the one
		 * brick above it is all that reaches it; the moment is 5.625 brick-weight-centimetres
		 * because the corbel's own weight is the ONLY load with a lever arm — the half share
		 * from above arrives through a patch with the same centroid, which is the zig-zag.
		 *
		 * BOTH ARE ASSERTED BEFORE ANY CLAIM ABOUT CAPACITY, because a joint carrying a
		 * plausible force with the wrong moment reads as a plausible utilisation, and that is
		 * exactly the failure this project keeps finding.
		 */
		const double CorbelForceUu = Dry.Structure.GetConnectionForce(TopCorbelJoint).Size();
		const double CorbelMomentUuCm = Dry.Structure.GetConnectionMoment(TopCorbelJoint).Size();

		AddInfo(FString::Printf(
			TEXT("the top corbel (course %d, piece %d on piece %d, joint %d) carries %.6f uu (%.6f W) and %.6f uu.cm (%.6f W.cm)"),
			TopCorbelCourse, TopCorbel, TopCorbelSupport, TopCorbelJoint,
			CorbelForceUu, CorbelForceUu / FullBrickWeightUu,
			CorbelMomentUuCm, CorbelMomentUuCm / FullBrickWeightUu));

		TestTrue(
			FString::Printf(
				TEXT("the top corbel joint must carry %.6f brick weights, %.6f uu; it carries %.6f uu"),
				TopCorbelForceBrickWeights, TopCorbelForceBrickWeights * FullBrickWeightUu, CorbelForceUu),
			FMath::IsNearlyEqual(CorbelForceUu, TopCorbelForceBrickWeights * FullBrickWeightUu, 1.0e-6));

		TestTrue(
			FString::Printf(
				TEXT("and %.6f brick-weight-centimetres of moment, %.6f uu.cm; it carries %.6f uu.cm"),
				TopCorbelMomentBrickWeightCm, TopCorbelMomentBrickWeightCm * FullBrickWeightUu,
				CorbelMomentUuCm),
			FMath::IsNearlyEqual(
				CorbelMomentUuCm, TopCorbelMomentBrickWeightCm * FullBrickWeightUu, 1.0e-6));

		/*
		 * AND THE EFFECTIVE ARM THOSE TWO MAKE IS 3.75 cm AND NOT 5.625, WHICH IS THE WHOLE
		 * REASON THIS JOINT MOVED. It is asserted as its own row rather than left implicit in
		 * the two above, because it is the quantity COMPOSITE_DEPTH_DESIGN.md's half-seat
		 * lemma made a false claim about — "any half seat has e >= 5.625 cm, so no joint with
		 * two or fewer courses over it can be touched" — and this joint is the counter-example
		 * that killed it. The extra half brick arrives CENTRED, through a patch with the same
		 * centroid, so it grows |F| without growing |M| and DILUTES the arm. Any future "this
		 * shallow joint cannot be capped" claim has to be measured rather than argued, and
		 * this row is where that is written down.
		 */
		const double CorbelArmCm = CorbelForceUu > 0.0 ? CorbelMomentUuCm / CorbelForceUu : 0.0;

		TestTrue(
			FString::Printf(
				TEXT("the arm this joint carries its load at is |M|/|F| = %.9g cm, NOT the %.9g cm the half-seat lemma assumed; it reads %.9g"),
				TopCorbelArmCm, CorbelOwnWeightArmCm, CorbelArmCm),
			FMath::IsNearlyEqual(CorbelArmCm, TopCorbelArmCm, 1.0e-9));

		TestTrue(
			FString::Printf(
				TEXT("FIXTURE: and it must be STRICTLY SHORTER than the half-seat eccentricity, or this is not the counter-example it is documented as — %.9g against %.9g"),
				CorbelArmCm, CorbelOwnWeightArmCm),
			CorbelArmCm < CorbelOwnWeightArmCm);

		/*
		 * AND THE AXIS THAT GOVERNS IS TENSION, WHICH IS ASSERTED RATHER THAN ASSUMED. The
		 * same joint in MORTAR reads the tension ratio exactly — 0.00455104 MPa over f_xk1's
		 * 0.10 — so a run in which compression or shear had taken over would show up here as a
		 * number that no longer matched the hand arithmetic instead of as a silent pass.
		 */
		FBrickLayout Mortared;

		TestTrue(TEXT("fixture: the producer should lay the same wall mortared"),
			RunningBond(
				WallSpecOf(OverCapacityWallCourses, OverCapacityWallBricksPerCourse,
					EWallEnd::Ragged, GeneralPurposeMortar),
				Mortared));

		Mortared.Structure.SolveLoads();

		int32 MortaredWorstJoint = INDEX_NONE;
		const double MortaredWorst = WorstJointOf(Mortared.Structure, MortaredWorstJoint);

		/*
		 * WHAT SECTION THE JOINT WAS ALLOWED, AND IT IS THE ARM THAT DECIDES IT.
		 *
		 * The rule is D = min(masonry above, max(corbelling body, lambda*e)) and all three
		 * terms are worked out on this fixture rather than read back: 15 cm over it, one
		 * course of body under it, and lambda*e = 12.99 cm in the middle. The credited depth
		 * is asserted against lambda*e EXACTLY, and being strictly between 7.5 and 15 is what
		 * proves both of the other two terms are slack — a corbelling body is a whole number
		 * of course pitches, so 12.99 rules out a two-course body as firmly as it rules out
		 * the wall's own 15.
		 */
		const double CreditedDepthCm = MortaredWorstJoint != INDEX_NONE
			? Mortared.Structure.GetConnectionCompositeDepthCm(MortaredWorstJoint)
			: 0.0;

		TestTrue(
			FString::Printf(
				TEXT("THE ARM MUST GOVERN THE SECTION: lambda*e = %.9g x %.9g = %.9g cm must be the credited depth; the joint was credited %.9g"),
				CompositeDepthPerArm, TopCorbelArmCm, TopCorbelPermittedDepthCm,
				CreditedDepthCm),
			FMath::IsNearlyEqual(CreditedDepthCm, TopCorbelPermittedDepthCm, 1.0e-9));

		TestTrue(
			FString::Printf(
				TEXT("and it must sit STRICTLY between the %.9g cm of corbelling body under it and the %.9g cm of masonry over it, which is what says neither of those bound it; it is %.9g"),
				TopCorbelBodyDepthCm, TopCorbelMasonryAboveCm, CreditedDepthCm),
			CreditedDepthCm > TopCorbelBodyDepthCm && CreditedDepthCm < TopCorbelMasonryAboveCm);

		/*
		 * AND THE RELIEF IS DECLINED, WHICH IS THE ROW THAT READS LIKE A REGRESSION AND IS NOT.
		 *
		 * A 12.99 cm section is 288.264 cm3, DEEPER than the bed patch's 179.482 — but the
		 * composite reading carries no axial relief and the patch reading does, so the deeper
		 * section is the WORSE demand. ComputeUtilisation takes the lesser (composite action
		 * is an alternative way of carrying the moment, never an extra one), refuses it, and
		 * the joint keeps the patch's own number. The sense of this comparison is INVERTED
		 * from what this file asserted through slice 4, deliberately, and the crossover depth
		 * below is what makes it a statement about the arm rather than about a coincidence.
		 */
		const double CrossoverDepthCm = FMath::Sqrt(
			6.0 * TopCorbelMomentBrickWeightCm * FullBrickWeightUu
			/ (10.25 * ForceUnitsPerMPaSqCmHere * TopCorbelTensileStressMPa));

		AddInfo(FString::Printf(
			TEXT("the same ragged wall MORTARED reads %.9g at joint %d; over its credited %.9g cm the composite demand is %.9g and the bed patch's own is %.9g, so the deeper section is the WORSE one and the relief is refused (two whole courses, %.9g cm, would have given %.9g — the figure this file pinned through slice 4)"),
			MortaredWorst, MortaredWorstJoint, CreditedDepthCm,
			TopCorbelCompositeStressMPa / MortarTensileMPa,
			TopCorbelTensileStressMPa / MortarTensileMPa,
			TopCorbelMasonryAboveCm,
			TwoCourseCompositeStressMPa / MortarTensileMPa));

		TestTrue(
			FString::Printf(
				TEXT("THE RELIEF MUST BE DECLINED: over %.9g cm the composite demand %.9g EXCEEDS the patch's %.9g, so the joint may not be read against the deep beam"),
				CreditedDepthCm, TopCorbelCompositeStressMPa / MortarTensileMPa,
				TopCorbelTensileStressMPa / MortarTensileMPa),
			TopCorbelCompositeStressMPa > TopCorbelTensileStressMPa);

		TestTrue(
			FString::Printf(
				TEXT("and it is the ARM that costs it the relief, not the wall: the composite section only undercuts the patch past %.9g cm (%.9g courses) and lambda*e reaches %.9g, %.6g%% short — while the %.9g cm actually standing there clears it"),
				CrossoverDepthCm, CrossoverDepthCm / CoursePitchCm, TopCorbelPermittedDepthCm,
				100.0 * (1.0 - TopCorbelPermittedDepthCm / CrossoverDepthCm),
				TopCorbelMasonryAboveCm),
			TopCorbelPermittedDepthCm < CrossoverDepthCm
				&& CrossoverDepthCm < TopCorbelMasonryAboveCm);

		TestTrue(
			FString::Printf(
				TEXT("the hand arithmetic must come to the pinned %.9g, it comes to %.9g"),
				MortarRaggedWorstAsBuilt, TopCorbelTensileStressMPa / MortarTensileMPa),
			FMath::IsNearlyEqual(
				TopCorbelTensileStressMPa / MortarTensileMPa, MortarRaggedWorstAsBuilt, 1.0e-9));

		TestTrue(
			FString::Printf(
				TEXT("the mortared wall's worst joint must be the top corbel's tension, %.9g; it reads %.9g"),
				MortarRaggedWorstAsBuilt, MortaredWorst),
			FMath::IsNearlyEqual(MortaredWorst, MortarRaggedWorstAsBuilt, 1.0e-9));

		TestTrue(
			FString::Printf(
				TEXT("and it must be the SAME joint the dry wall is condemned at, joint %d; the mortared worst is joint %d"),
				TopCorbelJoint, MortaredWorstJoint),
			MortaredWorstJoint == TopCorbelJoint);

		TestTrue(
			FString::Printf(
				TEXT("so a mortared ragged wall stands as built at %.9g, and raggedness alone is not what condemns this one"),
				MortaredWorst),
			MortaredWorst < 1.0);

		TestEqual(
			TEXT("and settling the mortared wall must break nothing at all"),
			Mortared.Structure.SolveAndBreak(), 0);

		/*
		 * NOW THE DRY WALL: the identical geometry with nothing in the joint, and the SAME
		 * tension against a capacity of exactly zero.
		 *
		 * HOW MANY JOINTS, NOT MERELY THAT ONE IS. The arm is 5.625 cm on every rung of the
		 * corbel while the force grows downward, so a joint stays open only while it carries
		 * under 3.29270 brick weights — and the ladder reads 1.5, 2.75, 3.9375, ... so exactly
		 * two rungs per end qualify. A load model reading high or low would move that count
		 * long before it moved anything a "worst joint is over 1" row could see.
		 */
		int32 OverCapacity = 0;
		double WorstUnderCapacity = 0.0;

		for (int32 Joint = 0; Joint < Dry.Structure.NumConnections(); ++Joint)
		{
			const double Utilisation = Dry.Structure.GetConnectionUtilisation(Joint);

			if (Utilisation > 1.0)
			{
				++OverCapacity;

				const double ForceBrickWeights =
					Dry.Structure.GetConnectionForce(Joint).Size() / FullBrickWeightUu;

				TestTrue(
					FString::Printf(
						TEXT("every over-capacity joint must be a corbel still carrying under %.5f brick weights; joint %d carries %.6f"),
						CorbelClosesAboveBrickWeights, Joint, ForceBrickWeights),
					ForceBrickWeights < CorbelClosesAboveBrickWeights);
			}
			else
			{
				WorstUnderCapacity = FMath::Max(WorstUnderCapacity, Utilisation);
			}
		}

		AddInfo(FString::Printf(
			TEXT("the dry ragged wall has %d of %d joints over capacity as built; the worst of the rest reads %.9g"),
			OverCapacity, Dry.Structure.NumConnections(), WorstUnderCapacity));

		TestEqual(
			FString::Printf(
				TEXT("fixture: exactly %d joints — two corbels at each end — must be over capacity as built, %d are"),
				OverCapacityJointsAsBuilt, OverCapacity),
			OverCapacity, OverCapacityJointsAsBuilt);

		TestTrue(
			FString::Printf(
				TEXT("fixture: and the top corbel joint %d must be one of them; it reads %.9g"),
				TopCorbelJoint, Dry.Structure.GetConnectionUtilisation(TopCorbelJoint)),
			Dry.Structure.GetConnectionUtilisation(TopCorbelJoint) > 1.0);

		/*
		 * AND THE REST OF THE WALL IS NOWHERE NEAR ANYTHING. Three orders of magnitude under
		 * capacity says this is not a weak wall that would fall over whatever you did to it:
		 * its ENDS are unbuildable and the rest of it is fine, which is what makes the
		 * surviving triangle below a prediction rather than an accident.
		 */
		TestTrue(
			FString::Printf(
				TEXT("fixture: every other joint in the dry wall must be far under capacity; the worst reads %.9g"),
				WorstUnderCapacity),
			WorstUnderCapacity < 0.01);
	}

	/*
	 * AND A DRY FLUSH WALL OF THE SAME SIZE IS NOT OVER CAPACITY, which is what stops "no
	 * mortar" being read as "doomed". A half bat fills the half cell at each end, the end
	 * brick gets two supports, its centre of mass lands on the area-weighted centroid of them
	 * and the eccentricity is exactly zero — so there is no tension anywhere for a joint with
	 * no tensile capacity to fail at. This is the row that isolates the CORBEL as the cause.
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

		const int32 Passes = DryFlush.Structure.SolveAndBreak();

		TestEqual(
			FString::Printf(
				TEXT("so settling it must break nothing at all: it took %d breaking passes"), Passes),
			Passes, 0);
	}

	/*
	 * THE SCENARIO WALL, UNTOUCHED. The regression anchor is exact rather than approximate:
	 * a flush end brick's centre of mass sits ON the area-weighted centroid of its two
	 * supports, so its eccentricity is exactly zero and the moment term vanishes bit for bit.
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

		AddInfo(FString::Printf(
			TEXT("the flush scenario wall's worst joint is joint %d at %.9g of capacity"),
			WorstJoint, Worst));

		TestTrue(
			FString::Printf(
				TEXT("the scenario wall must read %.9g of capacity, it reads %.9g"),
				ScenarioWorstAsBuilt, Worst),
			FMath::IsNearlyEqual(Worst, ScenarioWorstAsBuilt, ScenarioWorstAsBuilt * 1.0e-6));

		const int32 Passes = Scenario.Structure.SolveAndBreak();

		TestEqual(
			FString::Printf(
				TEXT("settling the scenario wall must break nothing: it ran %d breaking passes"), Passes),
			Passes, 0);

		int32 BrokenJoints = 0;

		for (int32 Joint = 0; Joint < Scenario.Structure.NumConnections(); ++Joint)
		{
			if (Scenario.Structure.GetConnection(Joint).HasGiven())
			{
				++BrokenJoints;
			}
		}

		TestEqual(
			FString::Printf(TEXT("and not one of its joints may give; %d did"), BrokenJoints),
			BrokenJoints, 0);
	}

	/*
	 * ================================================================================
	 * NOW THE WIRE: BUILD THE DRY RAGGED WALL IN A WORLD AND SOLVE-AND-PUSH IT ONCE.
	 * ================================================================================
	 */
	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	const int32 StructureId = TestWorld.Subsystem->BuildRunningBond(
		DryRaggedWallSpec(OverCapacityWallCourses, OverCapacityWallBricksPerCourse));

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
		FString::Printf(TEXT("fixture: the ragged wall should span %d handles, got %d"),
			OverCapacityWallPieceCount, Binding->NumPieces()),
		Binding->NumPieces(), OverCapacityWallPieceCount);

	if (Binding->NumPieces() != OverCapacityWallPieceCount)
	{
		TestWorld.End();
		return true;
	}

	TArray<ABrickActor*> Bricks;
	TArray<FVector> LaidAt;
	TArray<FVector> LaidCentreCm;

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		ABrickActor* Brick = BrickAt(*this, *Binding, Piece);

		if (Brick == nullptr)
		{
			TestWorld.End();
			return true;
		}

		/*
		 * A BRICK WITH NO MESH HAS NO BOUNDS AND NO BODY, so it can neither be weighed into a
		 * centre of mass nor simulate — and every measurement below would quietly read the
		 * actor's pivot instead. Bail rather than measure something else.
		 */
		if (Brick->GetMesh() == nullptr)
		{
			AddError(FString::Printf(TEXT("fixture: brick %d has no mesh component to measure"), Piece));
			TestWorld.End();
			return true;
		}

		Bricks.Add(Brick);
		LaidAt.Add(Brick->GetActorLocation());
		LaidCentreCm.Add(BrickCentreCm(*Brick));
	}

	/*
	 * THE ORACLE, BUILT BEFORE THE PUSH AND FROM THE BOND RATHER THAN FROM THE ANSWER.
	 * Each piece's fate is decided by where it sits in its own course, which is read off
	 * the box the producer laid.
	 */
	TArray<bool> ShouldSurvive;
	TArray<FPieceBox> Boxes;

	int32 ExpectedSurvivors = 0;

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		const FPieceBox& Box = Binding->GetBinding(Piece).Box;

		Boxes.Add(Box);
		ShouldSurvive.Add(ShouldSurviveSettling(Box, OverCapacityWallBricksPerCourse));

		if (ShouldSurvive.Last())
		{
			++ExpectedSurvivors;
		}
	}

	const int32 ExpectedReleased = Binding->NumPieces() - ExpectedSurvivors;

	AddInfo(FString::Printf(
		TEXT("the bond predicts %d of %d pieces survive settling, so %d come down"),
		ExpectedSurvivors, Binding->NumPieces(), ExpectedReleased));

	/*
	 * THE ONLY CALL THIS TEST MAKES ON THE WALL. Nothing is removed, nothing is clicked,
	 * nobody chooses a menu row: this is the wire the game mode runs on BeginPlay.
	 */
	const int32 Released = TestWorld.Subsystem->SolveAndPush(StructureId);

	TestEqual(
		FString::Printf(
			TEXT("building a wall that cannot hold itself up must settle it there and then: %d pieces should have been released, SolveAndPush released %d"),
			ExpectedReleased, Released),
		Released, ExpectedReleased);

	/*
	 * AND THE MECHANISM BEHIND IT: JOINTS GAVE UNDER LOAD. GetBreakPass is the quantity
	 * that survives the breaking — a given joint carries nothing and reads zero
	 * utilisation, which is also what a joint nobody ever loaded reads. A stamp means a
	 * joint failed under load in a cascade pass, which a plain solve can never produce.
	 */
	int32 BrokenJoints = 0;
	int32 HighestPass = 0;

	for (int32 Joint = 0; Joint < Binding->GetStructure().NumConnections(); ++Joint)
	{
		const int32 Pass = Binding->GetStructure().GetBreakPass(Joint);

		if (Pass != INDEX_NONE)
		{
			++BrokenJoints;
			HighestPass = FMath::Max(HighestPass, Pass);
		}
	}

	AddInfo(FString::Printf(
		TEXT("settling the ragged wall broke %d of %d joints, over %d passes"),
		BrokenJoints, Binding->GetStructure().NumConnections(), HighestPass));

	TestTrue(
		FString::Printf(
			TEXT("joints must have GIVEN rather than merely been computed to be over capacity; %d carry a break pass"),
			BrokenJoints),
		BrokenJoints > 0);

	TestTrue(
		FString::Printf(
			TEXT("and it must have CASCADED rather than broken one sweep's worth: the highest pass is %d"),
			HighestPass),
		HighestPass > 1);

	/*
	 * WHICH BRICKS, NOT HOW MANY. A count is satisfied by releasing any 111 of them, and
	 * the defect that matters — a wall that came apart in the wrong place — leaves every
	 * count agreeing.
	 *
	 * COUNTED IN FULL AND REPORTED IN PART. A hundred and eleven pieces can each be wrong in
	 * three ways, and a failure that writes three hundred lines is a failure nobody reads. The first
	 * few are what a maintainer works from; the totals are the assertions, so nothing is
	 * hidden by the cap.
	 *
	 * THE CAP WAS 12 AND IT COST A DIAGNOSIS. A run in which 46 bricks failed one row printed
	 * 12 of them, and the other 34 had to be reconstructed by arithmetic before anyone could
	 * see that the failing set followed the collapse front — which was the whole answer. 48
	 * is four courses' worth of a wall this wide many times over, so a failure that follows
	 * the front shows its shape directly.
	 */
	constexpr int32 MaxReportedPieces = 48;

	int32 WrongFate = 0;
	int32 WrongBody = 0;
	int32 StrandedPieces = 0;

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		const EPieceSupport Support = Binding->GetStructure().GetPieceSupport(Piece);

		if (Binding->IsReleased(Piece) == ShouldSurvive[Piece])
		{
			++WrongFate;

			if (WrongFate <= MaxReportedPieces)
			{
				AddError(FString::Printf(
					TEXT("piece %d (course %d, %d in from the nearer end, at X %.3f Z %.3f) should%s have been released; the solver says %s and IsReleased is %s"),
					Piece,
					CourseOf(Boxes[Piece]),
					PositionFromNearestEnd(
						Boxes[Piece], CourseOf(Boxes[Piece]), OverCapacityWallBricksPerCourse),
					Boxes[Piece].CentreCm.X, Boxes[Piece].CentreCm.Z,
					ShouldSurvive[Piece] ? TEXT(" not") : TEXT(""),
					SupportName(Support),
					Binding->IsReleased(Piece) ? TEXT("true") : TEXT("false")));
			}
		}

		/*
		 * THIS IS A COLLAPSE, NOT A SOLVER STALL. Stranded means the solver declined to
		 * divide load round a knot, and a wall calibrated on one would come down looking
		 * exactly the same while measuring a limitation of the model.
		 */
		if (Support == EPieceSupport::Stranded)
		{
			++StrandedPieces;

			if (StrandedPieces <= MaxReportedPieces)
			{
				AddError(FString::Printf(
					TEXT("piece %d is Stranded: that would make this a solver limitation rather than a collapse"),
					Piece));
			}
		}

		/* And the flag reached the brick: ABrickActor keeps no second copy of the answer. */
		const bool bSimulating =
			Bricks[Piece]->GetMesh() != nullptr && Bricks[Piece]->GetMesh()->IsSimulatingPhysics();

		if (bSimulating == ShouldSurvive[Piece])
		{
			++WrongBody;

			if (WrongBody <= MaxReportedPieces)
			{
				AddError(FString::Printf(
					TEXT("brick %d should%s be simulating physics once the wall has settled; it is %s"),
					Piece, ShouldSurvive[Piece] ? TEXT(" not") : TEXT(""),
					bSimulating ? TEXT("simulating") : TEXT("kinematic")));
			}
		}
	}

	TestEqual(
		FString::Printf(
			TEXT("every piece must meet the fate the bond predicts for it; %d of %d did not"),
			WrongFate, Binding->NumPieces()),
		WrongFate, 0);

	TestEqual(
		FString::Printf(TEXT("and no piece may be Stranded; %d was"), StrandedPieces),
		StrandedPieces, 0);

	TestEqual(
		FString::Printf(
			TEXT("and every brick's body must agree with its binding; %d of %d did not"),
			WrongBody, Binding->NumPieces()),
		WrongBody, 0);

	/*
	 * ================================================================================
	 * THE OUTCOME. REAL GRAVITY ON A FIXED STEP, AND THE WALL ENDS UP ON THE FLOOR.
	 * ================================================================================
	 *
	 * WHY THIS IS NOT "EVERY RELEASED BRICK FELL A COURSE", WHICH IS WHAT IT ASSERTED FIRST
	 * AND WHICH IS FALSE OF THIS GEOMETRY. BrickWorldTestSupport::FallenAtLeastCm is derived
	 * from a fixture in which released bricks have CLEAR AIR beneath them — the narrow-waist
	 * wall, where taking the waist out leaves 8.5 cm of nothing under the orphans. THE FOOT
	 * OF A STAIRCASE COLLAPSE HAS NO SUCH AIR. The front retreats half a brick per course, so
	 * the lowest released brick — course 2, position 0, spanning X -10.75..10.75 — still has
	 * 10.25 of its 21.5 cm resting on the course 1 brick at X 0.5..22.0, which is STILL
	 * STANDING. Its underside is at Z 15.0 and that brick's top at Z 14.0, so it drops its 1
	 * cm gap and then tips about the support edge with its centre of mass 0.5 cm outboard —
	 * about a centimetre in total, and ticking for longer changes nothing, because the wall
	 * is holding it. That is a correct outcome of this collapse, and every course from 2 to 5
	 * has two of them.
	 *
	 * AND THE TWO CLASSES CANNOT BE TOLD APART FROM THE BOND, WHICH IS WHY THE OUTCOME CLAIM
	 * IS AN AGGREGATE. Both candidate oracles were worked out and both are wrong: "has a
	 * surviving brick directly beneath it" names only the two bricks per course that overhang
	 * the front, and misses everything the pile jams against; its transitive closure — resting
	 * on anything that is itself resting on something standing — names all 111, because every
	 * column of this wall eventually reaches the two complete bottom courses. The real
	 * boundary is where the rubble pile stops jamming and starts falling clear, which is a
	 * fact about a pile mid-collapse rather than about the bond, and an oracle fitted to it
	 * would be a number copied off one run. So the released set is asserted BRICK BY BRICK on
	 * the mechanism reaching the world, and AS A WHOLE on the collapse.
	 *
	 * THE AGGREGATE IS THE CENTRE OF MASS, AND ITS LANDMARK IS THE WALL THAT SURVIVED. The
	 * released set's centre of mass starts at Z 103.047 — asserted below, not assumed — and
	 * the highest thing still standing is the top face of the single course 5 brick at the
	 * apex of the triangle, Z 44, which is READ OFF the survivors rather than written down.
	 * For the released mass to end up UNDER that it has to come down the better part of a
	 * metre on average, which no settle (1 cm), no tip (a few cm) and no shedding of a
	 * handful of bricks can produce, and which reads exactly zero if nothing is released at
	 * all. That is DESIGN.md §4's "a measure of the whole structure actually moving/falling"
	 * taken literally: THE RUBBLE ENDS UP LOWER THAN THE WALL IT FELL OFF. It ends at Z 1.9,
	 * so the margin on the landmark is 42 cm rather than a fitted centimetre.
	 *
	 * AND ABOVE THE FLOOR, which is the other side of the same claim and not decoration. A
	 * centre of mass that dropped because bricks tunnelled out through the world would
	 * satisfy the first half perfectly. Both comparisons are strict, so a NaN anywhere in the
	 * set fails both rather than passing one.
	 *
	 * PLUS A BREADTH ROW, because a mass-weighted average is exactly the sort of number a few
	 * heavy outliers can carry. Requiring a MAJORITY of the released bricks to have fallen
	 * more than a whole course says the collapse is wide as well as deep, and a course pitch
	 * is the same landmark BrickWorldTestSupport::FallenAtLeastCm is derived from — seven and
	 * a half times what a brick settling into its own joint can move. 70 of the 111 clear it,
	 * and the 41 that do not are the propped foot the paragraph above describes.
	 */
	TestWorld.TickSeconds(CollapseSeconds);

	/*
	 * The released set's mass-weighted centre of mass, from wherever the caller says its
	 * pieces are — so the laid reading and the two later ones are the same arithmetic and
	 * cannot drift apart.
	 *
	 * MASS FROM THE LAID BOX, DERIVED HERE. Every brick of a ragged wall is a full brick, so
	 * the weighting cannot change today's answer — and it is written as a centre of mass
	 * anyway, because a bond that ever laid two sizes would otherwise turn this into a mean
	 * of positions that had quietly stopped being one.
	 *
	 * AND THE DIVISION FAILS CLOSED: no released mass at all makes it 0/0, which is a NaN,
	 * and a NaN fails every strict comparison below rather than reading as a plausible
	 * height. There is no FMath::Max to swallow it and no default to substitute.
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

	const double FallingCentreOfMassZCm =
		ReleasedCentreOfMassZCm([&](int32 Piece) { return BrickCentreCm(*Bricks[Piece]); });

	/* One more second, so the row below can say the fall had ALREADY finished by the last. */
	TestWorld.TickSeconds(1.0);

	const double RestingCentreOfMassZCm =
		ReleasedCentreOfMassZCm([&](int32 Piece) { return BrickCentreCm(*Bricks[Piece]); });

	int32 DidNotMove = 0;
	int32 Drifted = 0;
	int32 FellAtLeastACourse = 0;

	double SmallestReleasedMoveCm = TNumericLimits<double>::Max();
	double SurvivingTopZCm = -TNumericLimits<double>::Max();

	for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
	{
		const FVector NowAt = Bricks[Piece]->GetActorLocation();
		const double FellCm = LaidAt[Piece].Z - NowAt.Z;
		const double MovedCm = FVector::Dist(NowAt, LaidAt[Piece]);

		if (ShouldSurvive[Piece])
		{
			/*
			 * THE LANDMARK IS MEASURED OFF THE STANDING WALL RATHER THAN WRITTEN DOWN, and in
			 * the same world space as the centre of mass it will be compared against. That the
			 * survivors are still where they were laid is the row immediately below, so this is
			 * a reading of the triangle rather than a second opinion about it.
			 */
			const FBoxSphereBounds& BoundsCm = Bricks[Piece]->GetMesh()->Bounds;

			SurvivingTopZCm = FMath::Max(SurvivingTopZCm, BoundsCm.Origin.Z + BoundsCm.BoxExtent.Z);

			if (MovedCm >= DriftToleranceCm)
			{
				++Drifted;

				if (Drifted <= MaxReportedPieces)
				{
					AddError(FString::Printf(
						TEXT("brick %d (course %d) is still held up and must not move; it drifted %.6f cm"),
						Piece, CourseOf(Boxes[Piece]), MovedCm));
				}
			}

			continue;
		}

		if (FellCm > CoursePitchCm)
		{
			++FellAtLeastACourse;
		}

		/*
		 * NOT A CLAIM ABOUT DISTANCE, AND THAT IS THE POINT. A released brick has been handed
		 * to physics, so seconds of gravity have to have done SOMETHING to it — while a
		 * kinematic one reports exactly 0.000000, because nothing integrates it at all. That
		 * is what makes this discriminating without asserting a fall the wall is preventing.
		 * Written as a negated > so a NaN position counts as not having moved rather than
		 * sailing through a <= comparison every NaN passes.
		 */
		if (!(MovedCm > ReleasedMustMoveCm))
		{
			++DidNotMove;

			if (DidNotMove <= MaxReportedPieces)
			{
				AddError(FString::Printf(
					TEXT("released brick %d (course %d, %d in from the nearer end) was handed to physics and must have moved; it moved %.6f cm"),
					Piece,
					CourseOf(Boxes[Piece]),
					PositionFromNearestEnd(
						Boxes[Piece], CourseOf(Boxes[Piece]), OverCapacityWallBricksPerCourse),
					MovedCm));
			}
		}

		SmallestReleasedMoveCm = FMath::Min(SmallestReleasedMoveCm, MovedCm);
	}

	TestEqual(
		FString::Printf(TEXT("no brick the bond says is still held up may have moved; %d did"), Drifted),
		Drifted, 0);

	AddInfo(FString::Printf(
		TEXT("the smallest movement any of the %d released bricks made is %.6f cm, against the %g cm required and the exactly 0.000000 a kinematic brick reports"),
		ExpectedReleased, SmallestReleasedMoveCm, ReleasedMustMoveCm));

	TestEqual(
		FString::Printf(
			TEXT("every brick the settle released must have moved under gravity; %d of %d did not"),
			DidNotMove, ExpectedReleased),
		DidNotMove, 0);

	AddInfo(FString::Printf(
		TEXT("the released %.1f kg of wall had its centre of mass at Z %.3f cm as laid, Z %.3f after %g s and Z %.3f a second after that, a drop of %.3f cm; the standing triangle's top is Z %.3f and the floor is Z %g"),
		ReleasedMassKg, LaidCentreOfMassZCm, FallingCentreOfMassZCm, CollapseSeconds,
		RestingCentreOfMassZCm, LaidCentreOfMassZCm - RestingCentreOfMassZCm,
		SurvivingTopZCm, FloorTopZCm));

	/*
	 * THE FIXTURE PRECONDITION THAT STOPS THE OUTCOME CLAIM BEING FREE. If the released mass
	 * started BELOW the top of the triangle, "it ended up below it" would be true of a wall
	 * that never moved at all.
	 */
	TestTrue(
		FString::Printf(
			TEXT("fixture: the released mass must start ABOVE the wall that survives, at Z %.3f against a standing top of Z %.3f"),
			LaidCentreOfMassZCm, SurvivingTopZCm),
		LaidCentreOfMassZCm > SurvivingTopZCm);

	TestTrue(
		FString::Printf(
			TEXT("the wall must have COME DOWN: the released mass's centre of mass should end below the top of the triangle still standing at Z %.3f, it is at Z %.3f"),
			SurvivingTopZCm, RestingCentreOfMassZCm),
		RestingCentreOfMassZCm < SurvivingTopZCm);

	TestTrue(
		FString::Printf(
			TEXT("and it must have LANDED rather than left the world: the released mass's centre of mass should stay above the floor at Z %g, it is at Z %.3f"),
			FloorTopZCm, RestingCentreOfMassZCm),
		RestingCentreOfMassZCm > FloorTopZCm);

	/*
	 * AND THE COLLAPSE WAS OVER BEFORE THE LAST SECOND OF IT, which is what makes the tick
	 * length a measurement rather than a guess: the rubble moved 0.024 cm of centre of mass
	 * between three seconds and four, against the 180 cm it had travelled to get there.
	 */
	TestTrue(
		FString::Printf(
			TEXT("the collapse must have FINISHED inside %g s: the released centre of mass moved %.3f cm in the second after that, and may move no more than %g"),
			CollapseSeconds, FMath::Abs(RestingCentreOfMassZCm - FallingCentreOfMassZCm),
			RubbleAtRestCm),
		FMath::Abs(RestingCentreOfMassZCm - FallingCentreOfMassZCm) < RubbleAtRestCm);

	/*
	 * THE BREADTH ROW. A majority, so no small number of bricks falling a long way can carry
	 * it, and a whole course pitch, which is seven and a half times the mortar joint a brick
	 * can settle into and the same landmark the sister test's fall threshold is derived from.
	 */
	AddInfo(FString::Printf(
		TEXT("%d of the %d released bricks fell more than the %g cm course pitch"),
		FellAtLeastACourse, ExpectedReleased, CoursePitchCm));

	TestTrue(
		FString::Printf(
			TEXT("the collapse must be WIDE as well as deep: more than half of the %d released bricks should have fallen a whole course, %d did"),
			ExpectedReleased, FellAtLeastACourse),
		FellAtLeastACourse * 2 > ExpectedReleased);

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
