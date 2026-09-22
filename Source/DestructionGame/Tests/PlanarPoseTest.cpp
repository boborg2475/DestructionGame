// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/BuildMode/Placement.h"
#include "Core/BuildMode/SnapSolver.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"
#include "Core/WallCases.h"

#include "Core/RigidBlock/RigidBlockBridge.h"
#include "Core/RigidBlock/RigidBlockOracle.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The planar pose — a 3D-flagged structure whose posed problem is planar is posed in 2D.
 *
 * `RigidBlockOracle::BuildRigidBlockProblem` poses the cheapest sound problem: 2D whenever
 * both (a) every joint it actually poses has an in-plane normal and (b) every Y that
 * enters an equilibrium row — each ungrounded block's centroid, each posed patch centre —
 * is one common Y; 3D otherwise. `IsThreeDimensional()` is demoted from "which pose" to
 * the stated permission to pose 3D at all, so a 2D-flagged structure carrying an
 * out-of-plane joint is still loudly refused and never quietly promoted. The fourth test
 * below is condition (b)'s own pin.
 *
 * Why this exists — what the unconditional 3D flag cost. `UDestructionStructureSubsystem::
 * BeginBuild` now flags every player build 3D (the E3 ruling: 3D is stated at the door,
 * never inferred from a joint that happens to have landed — see
 * `FStructure::SetThreeDimensional`, Structure.h ~490), fixing a silent demotion where one
 * rotated brick used to move a whole build's break authority off the LP and onto the
 * router. It also made a straight wall pay a 3D solve it has no use for: measured on a
 * 100-brick / 261-joint session wall with nothing released, a cold `Run structure` went
 * from 2.5 s in the 2D pose to 94 s in the 3D pose, ~37x (CURRENT_STATE, corner entry
 * (xiii)). Timing is not asserted anywhere here — a wall-clock threshold on a shared
 * machine is a flake, and `OracleSweepFull` is where solver cost is verified; the numbers
 * are recorded because they are the whole reason for the slice.
 *
 * Why the planar problem is posed in 2D — and why the two poses are not interchangeable.
 * The implication runs one way only: 3D-feasible implies 2D-feasible (project a 3D force
 * system onto X-Z and sum each joint's out-of-plane pairs — every 3D equilibrium row
 * implies its 2D twin), but the converse fails by a known factor. The 3D friction pyramid
 * and shear ceiling are a k = 8 octagon inscribed in the true Coulomb cone
 * (`ThreeDPyramidInscribeFactor` = cos(pi/8) = 0.92387953..., RigidBlockOracle.cpp),
 * deliberately pulled in so no shear direction is admitted past the cone. Pure in-plane
 * shear lands on a facet rather than a vertex, so the 3D pose caps it at 0.924x the exact
 * Coulomb limit the 2D rows carry — a planar structure shear-critical with a collapse
 * multiplier lambda* in [1.0, 1/0.92387953 = 1.0824) falls posed in 3D and stands posed in
 * 2D. So the 2D pose is not merely the cheap one, it is the accurate one: it carries the
 * exact cone for a problem with no out-of-plane shear to approximate, and it is the pose
 * every pinned oracle-sweep reading in this project is anchored on. Choosing it for a
 * planar problem is a fidelity argument that happens also to be ~37x faster. What the
 * out-of-plane rows add nothing to is the equilibrium side — with every posed normal in
 * X-Z and every posed Y equal, the out-of-plane force row and the two out-of-plane moment
 * rows are linear combinations of the in-plane ones and carry no information. It is the
 * strength side, the inscribed octagon, where the two formulations part company.
 *
 * The discriminator that matters: skipped joints do not count. The rule is over the
 * joints the bridge poses, not `NumConnections()`. The bridge drops a joint that has given
 * and a joint whose two pieces are both grounded before it ever looks at a normal, so an L
 * laid entirely on the earth has out-of-plane head joints and still poses a planar
 * problem. Case three below is exactly that structure, the case an implementation that
 * scanned every connection would get wrong: it would pay the 3D solve for a wall whose LP
 * contains not one out-of-plane row. The four other cases pin the corners of the rule.
 *
 * What is asserted, and the observable: `FOracleProblem::Dim`, read straight off the
 * bridge's own output — the mechanism, the posed dimension itself, binary and exact, not a
 * proxy such as a solve time or a pivot count. The world-level pin
 * (`World.Session.StraightBuildRunsInThePlanarPose`) cannot reach the problem struct and
 * goes through `FStructure::GetLastEquilibriumProblemDim()` instead, stamped on the pose
 * by `BreakByEquilibrium`.
 *
 * No ticking world needed: hand-laid layouts, the bridge, and `FStructure::SolveAndBreak`
 * — a world-free structural cascade, not a Chaos tick.
 *
 * Units: nothing here compares a force against a strength, so the 1 N = 100 uu / 1 MPa
 * over 1 cm2 = 10000 uu boundary is not crossed — every reading is a dimension, a support
 * enum, a severed flag, or the same number computed twice and compared with itself.
 *
 * Named namespace, not anonymous — a unity build merges many files into one translation unit.
 */
namespace PlanarPoseTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace RigidBlockOracle;

	/*
	 * The coordinating grid, spelled out rather than imported. A brick is 21.5 x 10.25 x
	 * 6.5 cm on a 1 cm joint, so the grid is 22.5 x 11.25 x 7.5 and course n centres a
	 * brick at n * 7.5 + 3.25 (course 0 resting on the earth). A rotated brick is the same
	 * box with X and Y swapped.
	 */

	const FVector HalfBrick(10.75, 5.125, 3.25);
	const FVector HalfBrickRotated(5.125, 10.75, 3.25);

	constexpr double Course0ZCm = 3.25;
	constexpr double Course1ZCm = 10.75;

	/** Clay brick, 1.9 g/cm3 — the published density every wall in this project is laid at. */
	constexpr double BrickDensityGramsPerCubicCm = 1.9;

	/*
	 * The bridge's skip rule, transcribed deliberately rather than called: this predicate
	 * answers "would the bridge pose this joint at all", which is what the rule under test
	 * quantifies over. Written from the bridge's documented refusals (a given joint is out
	 * of the structure; a joint between two grounded pieces constrains nothing the earth
	 * does not already absorb) rather than by calling into the bridge — a fixture that
	 * asked the code under test which joints it posed could not then claim the fixture
	 * contains a posed out-of-plane joint.
	 */
	bool JointWouldBePosed(const FStructure& Structure, int32 Connection)
	{
		const FConnection& Joint = Structure.GetConnection(Connection);

		if (Joint.HasGiven())
		{
			return false;
		}

		return !(Structure.GetPiece(Joint.PieceA).bIsGrounded
			&& Structure.GetPiece(Joint.PieceB).bIsGrounded);
	}

	/**
	 * Whether this joint's normal leaves the X-Z plane, in the house NaN-safe form.
	 *
	 * `!(|Y| <= tol)` rather than `|Y| > tol` so a non-finite normal answers true — out of
	 * plane, the expensive-and-sound side. It cannot actually arrive here (`AddConnection`
	 * refuses a non-axis-aligned normal and the bridge's own `Normalize()` refuses a
	 * degenerate one first), which is why no fixture below exercises it; the form is here
	 * so the test's own bookkeeping cannot be the thing that swallows a NaN.
	 */
	bool NormalIsOutOfPlane(const FConnection& Joint)
	{
		return !(FMath::Abs(Joint.InterfaceNormal.Y) <= 1.0e-9);
	}

	/** How many joints the bridge would pose, and how many of those leave the plane. */
	void CountPosedJoints(const FStructure& Structure, int32& OutPosed, int32& OutPosedOutOfPlane)
	{
		OutPosed = 0;
		OutPosedOutOfPlane = 0;

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			if (!JointWouldBePosed(Structure, Joint))
			{
				continue;
			}

			++OutPosed;

			if (NormalIsOutOfPlane(Structure.GetConnection(Joint)))
			{
				++OutPosedOutOfPlane;
			}
		}
	}

	/** Every out-of-plane joint, posed or skipped — the count case THREE needs to be non-vacuous. */
	int32 CountOutOfPlaneJoints(const FStructure& Structure)
	{
		int32 Count = 0;

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			Count += NormalIsOutOfPlane(Structure.GetConnection(Joint)) ? 1 : 0;
		}

		return Count;
	}

	FString DescribeJoint(const FStructure& Structure, int32 Connection)
	{
		const FConnection& Joint = Structure.GetConnection(Connection);

		return FString::Printf(
			TEXT("joint %d: %d-%d, n (%g, %g, %g), %.4f cm2, grounded %d/%d, posed %d"),
			Connection, Joint.PieceA, Joint.PieceB,
			Joint.InterfaceNormal.X, Joint.InterfaceNormal.Y, Joint.InterfaceNormal.Z,
			Joint.InterfaceAreaSqCm,
			Structure.GetPiece(Joint.PieceA).bIsGrounded ? 1 : 0,
			Structure.GetPiece(Joint.PieceB).bIsGrounded ? 1 : 0,
			JointWouldBePosed(Structure, Connection) ? 1 : 0);
	}

	/** "2D"/"3D" for a posed problem's dimension, so a failure message reads as physics. */
	const TCHAR* DimName(EOracleDim Dim)
	{
		return Dim == EOracleDim::Dim3D ? TEXT("3D") : TEXT("2D");
	}

	/* The three fixtures. */

	/**
	 * A straight running-bond wall — the planar case the slice exists for.
	 *
	 * Laid by the existing producer (`DestructionLayout::RunningBond`), two courses of
	 * three bricks: bed joints normal +/-Z, head joints normal +/-X, not one Y component
	 * anywhere.
	 */
	bool LayStraightWall(FBrickLayout& OutLayout)
	{
		FRunningBondSpec Spec;
		Spec.DensityGramsPerCubicCm = BrickDensityGramsPerCubicCm;
		Spec.CoursesHigh = 2;
		Spec.BricksPerCourse = 3;
		Spec.Strength = GeneralPurposeMortar;

		return RunningBond(Spec, OutLayout);
	}

	/**
	 * The six-piece L with a posed out-of-plane head joint — the shape
	 * `World.Session.CornerBuildIsJudgedByTheLP` lays, laid here through `BuildMode::PlacePiece`
	 * instead of the player's clicks so the fixture needs no world.
	 *
	 * The seed at the origin; a rotated return one joint off its +X end (10.75 + 1 + 5.125
	 * = 16.875) finishing flush with its -Y face (-5.125 + 10.75 = 5.625); two more rotated
	 * bricks stepping 22.5 cm along Y; then two course-1 bricks, whose head joint 5-4 is
	 * between two pieces that reach the earth only through their beds. That one joint is
	 * the whole fixture: an L laid entirely on course 0 presents the bridge with no posed
	 * out-of-plane joint at all (see `LayEarthLevelCorner`).
	 *
	 * The cursors are offset exactly as the session's are — same-course bricks asked for
	 * 0.125 cm short of the pitch so the same-course pose beats the next-course pose on raw
	 * distance, course-1 bricks asked for at the running bond, where a zero offset cannot
	 * be outranked.
	 */
	bool LayCornerWithAPosedOutOfPlaneJoint(FBrickLayout& OutLayout)
	{
		using namespace BuildMode;

		struct FLay
		{
			FVector RequestedCentreCm;
			FVector ExtentCm;
			bool bGrounded;
		};

		const FLay Steps[] = {
			{ FVector( 0.000,  0.000, Course0ZCm), HalfBrick,        true  },
			{ FVector(16.875,  5.000, Course0ZCm), HalfBrickRotated, true  },
			{ FVector(16.875, 28.000, Course0ZCm), HalfBrickRotated, true  },
			{ FVector(16.875, 50.500, Course0ZCm), HalfBrickRotated, true  },
			{ FVector(16.875, 16.875, Course1ZCm), HalfBrickRotated, false },
			{ FVector(16.875, 39.375, Course1ZCm), HalfBrickRotated, false },
		};

		const FSnapSettings Settings;

		for (int32 Step = 0; Step < UE_ARRAY_COUNT(Steps); ++Step)
		{
			const FPlacementResult Result = PlacePiece(
				OutLayout, Steps[Step].RequestedCentreCm, Steps[Step].ExtentCm, ClayBrick,
				Steps[Step].bGrounded, Settings);

			if (Result.PieceHandle != Step)
			{
				return false;
			}
		}

		return true;
	}

	/**
	 * THE SAME CORNER LAID ENTIRELY ON THE EARTH, WITH ONE BRICK ON TOP OF THE STRAIGHT LEG — the
	 * case that separates "every joint is in-plane" from "every POSED joint is in-plane".
	 *
	 * Two stretchers along X, a rotated return off the second one's +X end (22.5 + 10.75 + 1 +
	 * 5.125 = 39.375), one more rotated brick along Y, and a course-1 stretcher bedded on the two
	 * X-leg bricks. The Y-leg head joint 3-2 has a +/-Y normal and joins two GROUNDED pieces, so
	 * the bridge skips it and the posed set — the two beds under piece 4 — is entirely in-plane.
	 * The beds are what keep the posed set non-empty: "every posed joint is in-plane" over an
	 * EMPTY set is vacuously true and would discriminate nothing.
	 */
	bool LayEarthLevelCorner(FBrickLayout& OutLayout)
	{
		using namespace BuildMode;

		struct FLay
		{
			FVector RequestedCentreCm;
			FVector ExtentCm;
			bool bGrounded;
		};

		const FLay Steps[] = {
			{ FVector( 0.000,  0.000, Course0ZCm), HalfBrick,        true  },
			{ FVector(22.500,  0.000, Course0ZCm), HalfBrick,        true  },
			{ FVector(39.375,  5.000, Course0ZCm), HalfBrickRotated, true  },
			{ FVector(39.375, 28.000, Course0ZCm), HalfBrickRotated, true  },
			{ FVector(11.250,  0.000, Course1ZCm), HalfBrick,        false },
		};

		const FSnapSettings Settings;

		for (int32 Step = 0; Step < UE_ARRAY_COUNT(Steps); ++Step)
		{
			const FPlacementResult Result = PlacePiece(
				OutLayout, Steps[Step].RequestedCentreCm, Steps[Step].ExtentCm, ClayBrick,
				Steps[Step].bGrounded, Settings);

			if (Result.PieceHandle != Step)
			{
				return false;
			}
		}

		return true;
	}

	/*
	 * The hand-laid fixtures, and why they cannot go through the producers: everything
	 * above is laid by `RunningBond` or `BuildMode::PlacePiece`, and both snap onto one
	 * coordinating grid, at one wythe — exactly the geometry the second half of the
	 * planarity rule is about, so a fixture built through them cannot express the case
	 * (the snap would pull the Y back onto the plane and the test would measure the
	 * producer instead of the bridge). So these two go through `AddPiece`/`AddConnection`
	 * directly, with `MakeInterface` still owning every normal, area and rectangle — the
	 * one place a joint may be built (Layout.h).
	 */

	/** A structure laid brick by brick, with the handles kept so a test can name them. */
	struct FHandLaidStructure
	{
		FStructure Structure;
		TArray<int32> Pieces;
		TArray<int32> Joints;
	};

	/** The 1 cm mortar joint the whole coordinating grid above is built on. */
	constexpr double JointThicknessCm = 1.0;

	/** C24 softwood, ~0.42 g/cm3 — the published density this project's timber is cut at. */
	constexpr double TimberDensityGramsPerCubicCm = 0.42;

	/**
	 * Lay one box as a piece and return its handle, or INDEX_NONE.
	 *
	 * Mass comes from `PieceMassKg`, the project's ONE derivation of mass from geometry, so
	 * nothing here re-derives a kilogram from a volume.
	 */
	int32 LayBox(
		FHandLaidStructure& Out,
		const FPieceBox& Box,
		bool bIsGrounded,
		double DensityGramsPerCubicCm,
		const FMaterialProfile& Material)
	{
		const int32 Handle = Out.Structure.AddPiece(
			PieceMassKg(Box, DensityGramsPerCubicCm), bIsGrounded, Box.CentreCm);

		if (Handle != INDEX_NONE)
		{
			Out.Structure.SetPieceMaterial(Handle, &Material);
			Out.Pieces.Add(Handle);
		}

		return Handle;
	}

	/** Join two laid boxes through `MakeInterface`, appending the joint handle. Fails closed. */
	bool JoinBoxes(
		FHandLaidStructure& Out,
		int32 HandleA, const FPieceBox& BoxA,
		int32 HandleB, const FPieceBox& BoxB,
		const FConnectionStrength& Strength)
	{
		FConnection Joint;

		if (!MakeInterface(HandleA, BoxA, HandleB, BoxB, JointThicknessCm, Strength, Joint))
		{
			return false;
		}

		const int32 Handle = Out.Structure.AddConnection(Joint);

		if (Handle == INDEX_NONE)
		{
			return false;
		}

		Out.Joints.Add(Handle);
		return true;
	}

	/**
	 * The Y-cantilever: one grounded stretcher, one brick bedded on it and shoved along Y.
	 *
	 * Both boxes are the standard 21.5 x 10.25 x 6.5 brick. The lower rests on the earth at
	 * (0, 0, 3.25); the upper sits one course up at (0, TopCentreYCm, 10.75), separated on
	 * Z alone by the 1 cm joint, so `MakeInterface` writes a bed joint with a +Z normal. At
	 * TopCentreYCm = 7.5 the two boxes overlap over only 2.75 cm of wythe, so the bed patch
	 * spans Y 2.375..5.125 and its centre lands at Y = 3.75 — 2.73 kern-widths outside a
	 * patch whose Y half-extent is 1.375.
	 *
	 * Every normal is still in-plane (the bed is +/-Z), the whole point: the normals alone
	 * say "planar" and the problem is not. `DryStone` (tensile 0.0 MPa, cohesion 0.0) is
	 * what makes the overhang decide something — a bonded joint could hang it off its
	 * tensile capacity and the fixture would stand for reasons unrelated to the pose.
	 *
	 * At TopCentreYCm = 0 the same call is the control: full 21.5 x 10.25 bed, patch centre
	 * at Y = 0, the one ungrounded centroid at Y = 0, and a genuinely planar problem.
	 */
	bool LayYCantilever(double TopCentreYCm, FHandLaidStructure& Out)
	{
		const FPieceBox Base{ FVector(0.0, 0.0, Course0ZCm), HalfBrick };
		const FPieceBox Top{ FVector(0.0, TopCentreYCm, Course1ZCm), HalfBrick };

		const int32 BaseHandle =
			LayBox(Out, Base, /*bIsGrounded*/ true, BrickDensityGramsPerCubicCm, ClayBrick);
		const int32 TopHandle =
			LayBox(Out, Top, /*bIsGrounded*/ false, BrickDensityGramsPerCubicCm, ClayBrick);

		if (BaseHandle == INDEX_NONE || TopHandle == INDEX_NONE)
		{
			return false;
		}

		return JoinBoxes(Out, BaseHandle, Base, TopHandle, Top, DryStone);
	}

	/**
	 * Two walls and one plank — two grounded bearings 40 cm apart with a timber plank
	 * spanning them, laid either along Y (the 3D case) or along X (the planar control).
	 *
	 * The plank is 21.5 x 50 x 5 cm, bottomed at Z = 7.5 so it clears each bearing's 6.5 cm
	 * top by the 1 cm joint; each bed patch is separated on Z alone and so carries a +/-Z
	 * normal in both arrangements — the second half of the rule in isolation: not one
	 * normal leaves the plane either way, and the only thing that changes is where in Y the
	 * rows sit.
	 *
	 *   Along Y: bearings at Y = 0 and Y = 40, plank centroid at Y = 20, bed patch centres
	 *   at Y = 0.0625 and Y = 39.9375. Three different Y values enter the equilibrium rows,
	 *   and projecting them onto one plane would turn a plank bearing at two ends into a
	 *   plank bearing twice at the same place.
	 *
	 *   Along X: bearings at X = 0 and X = 40, everything at Y = 0 — one plane, genuinely planar.
	 */
	bool LayTwoWallsOnePlank(bool bSpanAlongY, FHandLaidStructure& Out)
	{
		/* Bottomed at 7.5 = the bearings' 6.5 cm top plus the 1 cm joint; 5 cm deep. */
		constexpr double PlankHalfDepthCm = 2.5;
		constexpr double PlankCentreZCm = 7.5 + PlankHalfDepthCm;
		constexpr double PlankHalfSpanCm = 25.0;
		constexpr double BearingSpacingCm = 40.0;

		const FPieceBox BearingA{
			FVector(0.0, 0.0, Course0ZCm), HalfBrick };
		const FPieceBox BearingB{
			bSpanAlongY
				? FVector(0.0, BearingSpacingCm, Course0ZCm)
				: FVector(BearingSpacingCm, 0.0, Course0ZCm),
			HalfBrick };

		const FPieceBox Plank{
			bSpanAlongY
				? FVector(0.0, BearingSpacingCm * 0.5, PlankCentreZCm)
				: FVector(BearingSpacingCm * 0.5, 0.0, PlankCentreZCm),
			bSpanAlongY
				? FVector(HalfBrick.X, PlankHalfSpanCm, PlankHalfDepthCm)
				: FVector(PlankHalfSpanCm, HalfBrick.Y, PlankHalfDepthCm) };

		const int32 A = LayBox(Out, BearingA, true, BrickDensityGramsPerCubicCm, ClayBrick);
		const int32 B = LayBox(Out, BearingB, true, BrickDensityGramsPerCubicCm, ClayBrick);
		const int32 P = LayBox(Out, Plank, false, TimberDensityGramsPerCubicCm, Timber);

		if (A == INDEX_NONE || B == INDEX_NONE || P == INDEX_NONE)
		{
			return false;
		}

		return JoinBoxes(Out, A, BearingA, P, Plank, DryStone)
			&& JoinBoxes(Out, B, BearingB, P, Plank, DryStone);
	}
}

/*
 * Case by case: which pose the bridge builds.
 *
 * Cases one and three are the rule itself: a 3D-flagged wall whose posed joints are all
 * in-plane at one Y is posed in 2D, and an out-of-plane joint the bridge skips buys no 3D
 * pose. They drove the slice — `BuildRigidBlockProblem` used to set `OutProblem.Dim =
 * Dim3D` on the flag alone, before it had looked at a single joint, so both read 3D where
 * 2D is required.
 *
 * Cases two, four and five are the regression net around the change: the genuinely
 * out-of-plane corner must go on being posed 3D, a 2D-flagged planar wall must go on being
 * posed 2D, and a 2D-flagged corner must go on being refused rather than quietly promoted
 * to the pose that would carry it. Proven to bite by their twins — cases one and three are
 * the same assertion against the same field, and the mutation that flattens the pose
 * choice back to the flag flips exactly those two.
 *
 * No ticking world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlanarProblemUnderTheThreeDFlagPosesInTwoDTest,
	"DestructionGame.Core.Oracle.PlanarProblemUnderThe3DFlagPosesIn2D",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPlanarProblemUnderTheThreeDFlagPosesInTwoDTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace PlanarPoseTestSupport;
	using namespace RigidBlockOracle;

	/* --- ONE: A STRAIGHT WALL FLAGGED 3D IS POSED IN 2D --------------------------------------- */

	{
		FBrickLayout Wall;

		if (!LayStraightWall(Wall))
		{
			AddError(TEXT("fixture: RunningBond must lay the two-course wall"));
			return true;
		}

		Wall.Structure.SetThreeDimensional(true);

		int32 Posed = 0;
		int32 PosedOutOfPlane = 0;
		CountPosedJoints(Wall.Structure, Posed, PosedOutOfPlane);

		AddInfo(FString::Printf(
			TEXT("STRAIGHT: %d pieces, %d joints, %d posed, %d posed out of plane"),
			Wall.Structure.NumPieces(), Wall.Structure.NumConnections(), Posed, PosedOutOfPlane));

		TestTrue(TEXT("fixture: the wall must have posed joints at all, or the rule is vacuous"),
			Posed > 0);
		TestEqual(
			TEXT("fixture: a running-bond wall has NO out-of-plane normal anywhere — beds are Z, "
				 "heads are X"),
			CountOutOfPlaneJoints(Wall.Structure), 0);
		TestTrue(TEXT("fixture: and it is flagged 3D, which is what makes this the case under test"),
			Wall.Structure.IsThreeDimensional());

		FOracleProblem Problem;
		FString WhyNot;
		const bool bBridged = BuildRigidBlockProblem(Wall.Structure, Problem, WhyNot);

		AddInfo(FString::Printf(
			TEXT("STRAIGHT: bridged %d (\"%s\"), posed %s with %d blocks and %d joints"),
			bBridged ? 1 : 0, *WhyNot, DimName(Problem.Dim),
			Problem.Blocks.Num(), Problem.Joints.Num()));

		TestTrue(TEXT("fixture: a planar wall must bridge whatever it is flagged"), bBridged);

		TestEqual(
			*FString::Printf(
				TEXT("[PIN] A 3D-FLAGGED PLANAR WALL MUST BE POSED IN 2D. Every one of its %d posed "
					 "joints has an in-plane normal and sits at one Y, so the 3D pose's out-of-plane "
					 "equilibrium row and its two extra moment rows are linear combinations of the "
					 "in-plane ones and carry nothing. Its friction facets are NOT free, and that is "
					 "why 2D is the accurate pose rather than merely the cheap one: the k=8 pyramid is "
					 "INSCRIBED in the Coulomb cone, so it caps pure in-plane shear at cos(pi/8) = "
					 "0.924x the exact limit the 2D rows carry, and a shear-critical planar wall with "
					 "lambda* in [1.0, 1.0824) would FALL posed in 3D and STAND posed in 2D. The flag "
					 "is the PERMISSION to pose 3D, not the choice; 2D is also ~37x cheaper on a "
					 "100-brick wall. Posed %s"),
				Posed, DimName(Problem.Dim)),
			static_cast<int32>(Problem.Dim), static_cast<int32>(EOracleDim::Dim2D));
	}

	/* --- TWO: A GENUINE CORNER FLAGGED 3D IS STILL POSED IN 3D -------------------------------- */

	{
		FBrickLayout Corner;

		if (!LayCornerWithAPosedOutOfPlaneJoint(Corner))
		{
			AddError(TEXT("fixture: the six-piece L must lay through PlacePiece"));
			return true;
		}

		Corner.Structure.SetThreeDimensional(true);

		int32 Posed = 0;
		int32 PosedOutOfPlane = 0;
		CountPosedJoints(Corner.Structure, Posed, PosedOutOfPlane);

		for (int32 Joint = 0; Joint < Corner.Structure.NumConnections(); ++Joint)
		{
			AddInfo(DescribeJoint(Corner.Structure, Joint));
		}

		AddInfo(FString::Printf(
			TEXT("CORNER: %d pieces, %d joints, %d posed, %d posed out of plane"),
			Corner.Structure.NumPieces(), Corner.Structure.NumConnections(), Posed,
			PosedOutOfPlane));

		TestEqual(TEXT("fixture: the L is the session's six-piece shape"),
			Corner.Structure.NumPieces(), 6);
		TestEqual(
			TEXT("FIXTURE, AND THE WHOLE POINT OF THE TWO COURSE-1 BRICKS: exactly one POSED "
				 "out-of-plane joint — the head 5-4, between two pieces neither of which is grounded. "
				 "With every Y-normal joint earth-to-earth this case would be indistinguishable from "
				 "case THREE and would pin nothing"),
			PosedOutOfPlane, 1);

		FOracleProblem Problem;
		FString WhyNot;
		const bool bBridged = BuildRigidBlockProblem(Corner.Structure, Problem, WhyNot);

		AddInfo(FString::Printf(
			TEXT("CORNER: bridged %d (\"%s\"), posed %s with %d blocks and %d joints"),
			bBridged ? 1 : 0, *WhyNot, DimName(Problem.Dim),
			Problem.Blocks.Num(), Problem.Joints.Num()));

		TestTrue(TEXT("a 3D-flagged corner must bridge — its out-of-plane normal is permitted"),
			bBridged);

		TestEqual(
			*FString::Printf(
				TEXT("[NET] A CORNER WITH A POSED OUT-OF-PLANE JOINT MUST STILL BE POSED IN 3D. This is "
					 "the half the cheap pose must not buy: the 2D X-Z oracle cannot express a Y-facing "
					 "joint at all, so posing this planar would be a plausible number with wrong "
					 "statics. Posed %s"),
				DimName(Problem.Dim)),
			static_cast<int32>(Problem.Dim), static_cast<int32>(EOracleDim::Dim3D));
	}

	/* --- THREE: AN OUT-OF-PLANE JOINT THE BRIDGE SKIPS DOES NOT BUY A 3D POSE ----------------- */

	{
		FBrickLayout Corner;

		if (!LayEarthLevelCorner(Corner))
		{
			AddError(TEXT("fixture: the five-piece earth-level L must lay through PlacePiece"));
			return true;
		}

		Corner.Structure.SetThreeDimensional(true);

		int32 Posed = 0;
		int32 PosedOutOfPlane = 0;
		CountPosedJoints(Corner.Structure, Posed, PosedOutOfPlane);

		for (int32 Joint = 0; Joint < Corner.Structure.NumConnections(); ++Joint)
		{
			AddInfo(DescribeJoint(Corner.Structure, Joint));
		}

		AddInfo(FString::Printf(
			TEXT("EARTH-LEVEL CORNER: %d pieces, %d joints, %d posed, %d posed out of plane, %d out "
				 "of plane in total"),
			Corner.Structure.NumPieces(), Corner.Structure.NumConnections(), Posed, PosedOutOfPlane,
			CountOutOfPlaneJoints(Corner.Structure)));

		TestTrue(
			TEXT("FIXTURE: this L really does carry an out-of-plane joint — otherwise it is just "
				 "another straight wall and case ONE already covers it"),
			CountOutOfPlaneJoints(Corner.Structure) > 0);
		TestEqual(
			TEXT("FIXTURE: and NONE of them is posed — every one joins two grounded pieces, which the "
				 "bridge drops before it looks at a normal"),
			PosedOutOfPlane, 0);
		TestTrue(
			TEXT("FIXTURE: the course-1 brick's two beds keep the posed set NON-EMPTY, so 'every posed "
				 "joint is in-plane' is a real claim here rather than a vacuous one"),
			Posed > 0);

		FOracleProblem Problem;
		FString WhyNot;
		const bool bBridged = BuildRigidBlockProblem(Corner.Structure, Problem, WhyNot);

		AddInfo(FString::Printf(
			TEXT("EARTH-LEVEL CORNER: bridged %d (\"%s\"), posed %s with %d blocks and %d joints"),
			bBridged ? 1 : 0, *WhyNot, DimName(Problem.Dim),
			Problem.Blocks.Num(), Problem.Joints.Num()));

		TestTrue(TEXT("fixture: it must bridge"), bBridged);

		TestEqual(
			*FString::Printf(
				TEXT("[PIN] A SKIPPED OUT-OF-PLANE JOINT DOES NOT MAKE THE PROBLEM 3D. The rule is over "
					 "the joints the bridge POSES: this L's Y-normal heads all join two GROUNDED pieces "
					 "and never reach the LP, so the posed problem contains not one out-of-plane row "
					 "and must be posed in 2D. An implementation that scanned NumConnections() instead "
					 "would pay the 3D solve for a planar LP — and would also be wrong in the other "
					 "direction after a cascade, where a GIVEN out-of-plane joint is equally absent. "
					 "Posed %s"),
				DimName(Problem.Dim)),
			static_cast<int32>(Problem.Dim), static_cast<int32>(EOracleDim::Dim2D));
	}

	/* --- FOUR: A 2D-FLAGGED PLANAR WALL IS UNCHANGED ------------------------------------------ */

	{
		FBrickLayout Wall;

		if (!LayStraightWall(Wall))
		{
			AddError(TEXT("fixture: RunningBond must lay the two-course wall"));
			return true;
		}

		TestFalse(TEXT("fixture: unflagged is 2D, the default"), Wall.Structure.IsThreeDimensional());

		FOracleProblem Problem;
		FString WhyNot;
		const bool bBridged = BuildRigidBlockProblem(Wall.Structure, Problem, WhyNot);

		AddInfo(FString::Printf(
			TEXT("STRAIGHT 2D-FLAGGED: bridged %d (\"%s\"), posed %s"),
			bBridged ? 1 : 0, *WhyNot, DimName(Problem.Dim)));

		TestTrue(TEXT("[NET] a 2D-flagged planar wall still bridges"), bBridged);
		TestEqual(
			TEXT("[NET] and is still posed in 2D — the unflagged path must stay byte-for-byte what it "
				 "was, which is what keeps every pinned oracle-sweep reading valid"),
			static_cast<int32>(Problem.Dim), static_cast<int32>(EOracleDim::Dim2D));
	}

	/* --- FIVE: A 2D-FLAGGED CORNER IS STILL REFUSED, NEVER PROMOTED --------------------------- */

	{
		FBrickLayout Corner;

		if (!LayCornerWithAPosedOutOfPlaneJoint(Corner))
		{
			AddError(TEXT("fixture: the six-piece L must lay through PlacePiece"));
			return true;
		}

		TestFalse(TEXT("fixture: this copy is NOT flagged — no permission to pose 3D"),
			Corner.Structure.IsThreeDimensional());

		FOracleProblem Problem;
		FString WhyNot;
		const bool bBridged = BuildRigidBlockProblem(Corner.Structure, Problem, WhyNot);

		AddInfo(FString::Printf(
			TEXT("CORNER 2D-FLAGGED: bridged %d (\"%s\"), %d blocks, %d joints"),
			bBridged ? 1 : 0, *WhyNot, Problem.Blocks.Num(), Problem.Joints.Num()));

		TestFalse(
			TEXT("[NET] A 2D-FLAGGED CORNER MUST STILL BE REFUSED. The pose becomes a CHOICE between "
				 "two sound options, not an inference: picking the cheap pose where it is sound must "
				 "not become picking the expensive pose where it is forbidden. The E3 ruling is that a "
				 "2D structure which has accidentally acquired an out-of-plane joint stays loudly "
				 "refused rather than silently promoted"),
			bBridged);

		TestTrue(
			*FString::Printf(
				TEXT("[NET] and refused for the out-of-plane normal, by name. Got \"%s\""), *WhyNot),
			WhyNot.Contains(TEXT("out-of-plane")));

		TestEqual(TEXT("[NET] a refusal empties the problem, so a caller who ignores it solves nothing"),
			Problem.Blocks.Num(), 0);
	}

	return true;
}

/*
 * The self-comparison pin: flagging a planar structure 3D changes no answer.
 *
 * Read this first — what this test is now for. Since the bridge chooses the pose, a
 * planar fixture is posed in 2D whether or not it is flagged, so both copies below build
 * literally the same LP and every comparison here is a self-comparison. It is green by
 * construction, and that is the point: its job is to go red the day the poses are
 * re-split. Anything that makes a flagged planar structure take the 3D pose again — a
 * reverted pose choice, a predicate that stops skipping earth-to-earth joints, a new
 * 3D-only readout path — separates the two copies and this test fires on the verdict, the
 * severed set or the readout. It is a tripwire, not a demonstration that the two
 * formulations agree — they do not (see below).
 *
 * The two poses are not interchangeable. The true relation is one-directional:
 * 3D-feasible implies 2D-feasible (project onto X-Z, sum each joint's out-of-plane pairs,
 * every 3D row implies its 2D twin), but not the converse — the 3D friction pyramid and
 * shear ceiling are a k=8 octagon inscribed in the Coulomb cone
 * (`ThreeDPyramidInscribeFactor` = cos(pi/8) = 0.924), so pure in-plane shear is capped at
 * 0.924x the exact cone the 2D rows carry, and a shear-critical planar structure with
 * lambda* in [1.0, 1.0824) falls in 3D and stands in 2D. 2D is the more accurate
 * formulation for a planar problem as well as the cheaper one, and the one every
 * oracle-sweep pin is anchored on. No fixture in this corpus is shear-critical, which is
 * why the corpus could ever have been run both ways and looked like agreement.
 *
 * What is compared, in four currencies, because they fail independently:
 *
 *   - the verdict: `SolveAndBreak`'s pass count, and every piece's `GetPieceSupport` — the
 *     exact enum, so Stranded and Falling cannot be confused;
 *   - the severed set: `HasGiven` and `GetBreakPass` per joint, so a cascade that reached
 *     the same end state down a different sequence of passes is still caught;
 *   - the readout: `GetConnectionReadout` per joint — presence, N, M, violation and utilisation;
 *   - and the fixture's own shape, so a producer change cannot quietly empty the corpus.
 *
 * Displacement is nowhere in that list, and could not be: nothing here moves (DESIGN §4).
 *
 * What the two poses actually did disagree about, before the pose was chosen — both
 * readings kept because they are what the tripwire is watching for, and either would come
 * back if the poses were re-split.
 *
 *   The readout: the 3D min-violation readout (`SolveMinViolationReadoutThreeD`) never got
 *   the shear-cap and first-crack rows the 3D gate got — the item-8 residue in
 *   CURRENT_STATE — so it prices a bonded joint against an envelope the gate no longer
 *   allows. Measured on the 100-brick session wall: the same joint read 0.006 through the
 *   3D readout against 0.041 through the 2D one, 6.7x more comfortable. That residue is
 *   still there; posing a planar problem in 2D simply stops a planar structure from ever
 *   reaching it.
 *
 *   The severed set, on the collapse row only (measured 2026-09-16, a finding in its own
 *   right): with the whole bottom course pulled out, the two poses agreed completely on
 *   the verdict (one pass, all ten pieces lose the earth, every support enum identical)
 *   and disagreed on which joints the mechanism opens — the 3D pose severed joints 2, 4,
 *   7, 14, 15, 16, 17 and 18 in pass 1 where the 2D pose left them intact. That is not the
 *   readout residue, it is the collapse mechanism itself, what a free-falling body's LP
 *   looks like: the dual is degenerate, several force systems certify the same
 *   infeasibility, and the two formulations pick different ones. So even where the two
 *   formulations agree on standing-or-falling, they need not agree on the break sequence —
 *   the argument for choosing a pose rather than leaving two to agree. Once a planar
 *   problem is always posed in 2D, both copies here are literally the same problem and
 *   every assertion below holds exactly, by construction rather than luck.
 *
 * What it deliberately does not do: sweep the whole catalogue. Five small fixtures, all
 * below the gate's block cap, each solved twice. The exhaustive version is the LP oracle
 * sweep, and `OracleSweepFull` is mandatory before any commit that touches the bridge or
 * the solver — this test is the fast gate, not the verification.
 *
 * Tolerance: today both copies assemble the identical system and the readout doubles
 * agree bit for bit; the 1e-9 relative-or-absolute comparison is there for the re-split
 * case, where two different-sized systems would reach the same optimum by different
 * simplex paths and the last bits need not agree — four orders tighter than the 6.7x
 * readout gap above and far looser than one ulp. Every other reading is exact.
 *
 * No ticking world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlanarPoseChangesNoVerdictTest,
	"DestructionGame.Core.Oracle.PlanarPoseChangesNoVerdict",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPlanarPoseChangesNoVerdictTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace PlanarPoseTestSupport;

	/**
	 * One fixture of the corpus: how to lay it, and which pieces the player has pulled out.
	 *
	 * The removals are named by course rather than by handle, because a handle is a
	 * producer implementation detail and a course is what the cut means. `INDEX_NONE` cuts
	 * nothing.
	 */
	struct FParityCase
	{
		const TCHAR* Name;
		TFunction<bool(FBrickLayout&)> Lay;

		/** Every piece whose box centre sits at this Z is removed before the solve. */
		double RemoveCourseAtZCm;

		/** And of those, only the one nearest this X — or every one of them if not finite. */
		double RemoveNearestXCm;
	};

	auto LayWall = [](int32 Courses, int32 Cells, DestructionWallCases::EWallBond Bond,
		int32 CorbelFrom, int32 ProjectingCourse)
	{
		return [Courses, Cells, Bond, CorbelFrom, ProjectingCourse](FBrickLayout& Out)
		{
			DestructionWallCases::FWallSpec Spec;
			Spec.DensityGramsPerCubicCm = BrickDensityGramsPerCubicCm;
			Spec.CoursesHigh = Courses;
			Spec.Cells = Cells;
			Spec.Bond = Bond;
			Spec.CorbelFromCourse = CorbelFrom;
			Spec.CorbelStepCm = CorbelFrom == INDEX_NONE ? 0.0 : 3.25;
			Spec.ProjectingCourse = ProjectingCourse;
			Spec.Strength = GeneralPurposeMortar;

			DestructionWallCases::FWallLayout Wall;

			if (!DestructionWallCases::Build(Spec, Wall))
			{
				return false;
			}

			Out = MoveTemp(Wall.Layout);
			return true;
		};
	};

	const FParityCase Corpus[] = {
		/* A plain running-bond wall, intact — the everyday case a player lays. */
		{ TEXT("running 3x3, intact"),
			LayWall(3, 3, DestructionWallCases::EWallBond::Running, INDEX_NONE, INDEX_NONE),
			TNumericLimits<double>::Max(), TNumericLimits<double>::Max() },

		/* The same wall with one brick pulled from under it — load has to find another path. */
		{ TEXT("running 3x3, one brick out of the bottom course"),
			LayWall(3, 3, DestructionWallCases::EWallBond::Running, INDEX_NONE, INDEX_NONE),
			Course0ZCm, 22.5 },

		/* Stack bond: every head joint lines up, so the load paths are a different graph. */
		{ TEXT("stack 3x3, intact"),
			LayWall(3, 3, DestructionWallCases::EWallBond::Stack, INDEX_NONE, INDEX_NONE),
			TNumericLimits<double>::Max(), TNumericLimits<double>::Max() },

		/* A corbelled wall — eccentric load, the case where moments decide rather than force. */
		{ TEXT("running 3x3 corbelling from course 1"),
			LayWall(3, 3, DestructionWallCases::EWallBond::Running, 1, INDEX_NONE),
			TNumericLimits<double>::Max(), TNumericLimits<double>::Max() },

		/*
		 * And one that must fall: take the whole bottom course away and nothing left
		 * touches the earth, so the LP has no admissible equilibrium however it is posed. A
		 * corpus in which every fixture stands would compare two identical rows of "nothing
		 * happened" and pin nothing about a collapse agreeing across the two poses — the
		 * floor below asserts that at least one fixture genuinely fells pieces.
		 */
		{ TEXT("running 3x3 with its whole bottom course removed"),
			LayWall(3, 3, DestructionWallCases::EWallBond::Running, INDEX_NONE, INDEX_NONE),
			Course0ZCm, TNumericLimits<double>::Max() },
	};

	/** 1e-9, relative for large numbers and absolute for small ones. */
	auto AgreesToNineFigures = [](double A, double B)
	{
		const double Scale = FMath::Max(1.0, FMath::Max(FMath::Abs(A), FMath::Abs(B)));

		/* Written so a NaN on either side answers FALSE rather than slipping through a `>`. */
		return FMath::Abs(A - B) <= 1.0e-9 * Scale;
	};

	int32 CasesWithAFall = 0;
	int32 CasesWithAReadout = 0;

	for (int32 Case = 0; Case < UE_ARRAY_COUNT(Corpus); ++Case)
	{
		const FParityCase& Row = Corpus[Case];

		FBrickLayout Planar;
		FBrickLayout Volumetric;

		if (!Row.Lay(Planar) || !Row.Lay(Volumetric))
		{
			AddError(FString::Printf(TEXT("fixture: case '%s' must lay"), Row.Name));
			continue;
		}

		/* --- THE CUT, applied identically to both copies ------------------------------------- */

		int32 Removed = 0;

		if (Row.RemoveCourseAtZCm != TNumericLimits<double>::Max())
		{
			int32 Nearest = INDEX_NONE;
			double NearestGap = TNumericLimits<double>::Max();

			for (int32 Piece = 0; Piece < Planar.Boxes.Num(); ++Piece)
			{
				if (FMath::Abs(Planar.Boxes[Piece].CentreCm.Z - Row.RemoveCourseAtZCm) > 1.0e-6)
				{
					continue;
				}

				if (Row.RemoveNearestXCm == TNumericLimits<double>::Max())
				{
					Planar.Structure.RemovePiece(Piece);
					Volumetric.Structure.RemovePiece(Piece);
					++Removed;
					continue;
				}

				const double Gap = FMath::Abs(Planar.Boxes[Piece].CentreCm.X - Row.RemoveNearestXCm);

				if (Gap < NearestGap)
				{
					NearestGap = Gap;
					Nearest = Piece;
				}
			}

			if (Nearest != INDEX_NONE)
			{
				Planar.Structure.RemovePiece(Nearest);
				Volumetric.Structure.RemovePiece(Nearest);
				++Removed;
			}
		}

		/* --- THE FIXTURE GUARDS: planar, below the cap, and not empty ------------------------ */

		int32 Posed = 0;
		int32 PosedOutOfPlane = 0;
		CountPosedJoints(Planar.Structure, Posed, PosedOutOfPlane);

		AddInfo(FString::Printf(
			TEXT("case '%s': %d pieces (%d live, %d removed), %d joints, %d posed"),
			Row.Name, Planar.Structure.NumPieces(), Planar.Structure.NumLivePieces(), Removed,
			Planar.Structure.NumConnections(), Posed));

		TestEqual(
			*FString::Printf(
				TEXT("fixture '%s': every joint is in-plane — this corpus is about the PLANAR pose, "
					 "and a fixture with an out-of-plane joint would be comparing a 2D REFUSAL against "
					 "a 3D answer"),
				Row.Name),
			CountOutOfPlaneJoints(Planar.Structure), 0);

		TestTrue(
			*FString::Printf(TEXT("fixture '%s': posed joints exist"), Row.Name), Posed > 0);

		TestTrue(
			*FString::Printf(
				TEXT("fixture '%s': %d pieces is below the 200-block gate cap, so the LP is the break "
					 "authority here and not the router"),
				Row.Name, Planar.Structure.NumPieces()),
			Planar.Structure.NumPieces() <= 200);

		/* --- THE TWO SOLVES ------------------------------------------------------------------ */

		Volumetric.Structure.SetThreeDimensional(true);

		const int32 PlanarPasses = Planar.Structure.SolveAndBreak();
		const int32 VolumetricPasses = Volumetric.Structure.SolveAndBreak();

		TestEqual(
			*FString::Printf(
				TEXT("PARITY '%s': the cascade must break in the same number of passes. A pose that "
					 "changes WHEN a joint gives changes the order a collapse plays back in, which is "
					 "the sequence the stamps exist to record"),
				Row.Name),
			VolumetricPasses, PlanarPasses);

		/* --- THE VERDICT: every piece's support, the exact enum ------------------------------- */

		int32 FellPlanar = 0;

		for (int32 Piece = 0; Piece < Planar.Structure.NumPieces(); ++Piece)
		{
			const EPieceSupport Here = Planar.Structure.GetPieceSupport(Piece);
			const EPieceSupport There = Volumetric.Structure.GetPieceSupport(Piece);

			FellPlanar += (Here == EPieceSupport::Falling || Here == EPieceSupport::Stranded) ? 1 : 0;

			TestEqual(
				*FString::Printf(
					TEXT("PARITY '%s': piece %d's support must be the same whether or not the planar "
						 "structure is FLAGGED 3D — the bridge poses both in 2D, so this is one LP "
						 "compared with itself and a difference means the poses have been re-split"),
					Row.Name, Piece),
				static_cast<int32>(There), static_cast<int32>(Here));
		}

		CasesWithAFall += FellPlanar > 0 ? 1 : 0;

		/* --- THE SEVERED SET, and the pass each joint went in --------------------------------- */

		for (int32 Joint = 0; Joint < Planar.Structure.NumConnections(); ++Joint)
		{
			const bool bGaveHere = Planar.Structure.GetConnection(Joint).HasGiven();
			const bool bGaveThere = Volumetric.Structure.GetConnection(Joint).HasGiven();

			TestTrue(
				*FString::Printf(
					TEXT("PARITY '%s': joint %d must be severed in both poses or neither; 2D %d, "
						 "3D %d"),
					Row.Name, Joint, bGaveHere ? 1 : 0, bGaveThere ? 1 : 0),
				bGaveHere == bGaveThere);

			TestEqual(
				*FString::Printf(
					TEXT("PARITY '%s': joint %d must have gone in the same pass"), Row.Name, Joint),
				Volumetric.Structure.GetBreakPass(Joint),
				Planar.Structure.GetBreakPass(Joint));
		}

		/* --- THE READOUT — the currency the re-split would break first ------------------------- */

		int32 PresentHere = 0;

		for (int32 Joint = 0; Joint < Planar.Structure.NumConnections(); ++Joint)
		{
			const FStructure::FConnectionReadout Here =
				Planar.Structure.GetConnectionReadout(Joint);
			const FStructure::FConnectionReadout There =
				Volumetric.Structure.GetConnectionReadout(Joint);

			PresentHere += Here.bPresent ? 1 : 0;

			TestTrue(
				*FString::Printf(
					TEXT("PARITY '%s': joint %d's LP readout must be present in both poses or neither "
						 "— an absent readout means the gate DECLINED, and a pose that declines where "
						 "the other answers has taken the LP off this structure. 2D %d, 3D %d"),
					Row.Name, Joint, Here.bPresent ? 1 : 0, There.bPresent ? 1 : 0),
				Here.bPresent == There.bPresent);

			if (!Here.bPresent || !There.bPresent)
			{
				continue;
			}

			TestTrue(
				*FString::Printf(
					TEXT("[TRIPWIRE] PARITY '%s': joint %d's UTILISATION must be the same number "
						 "flagged or not. It is the strain the player reads off the overlay and off "
						 "the inspector rows, and the 3D readout is STILL missing the shear-cap and "
						 "first-crack rows the 3D GATE has (CURRENT_STATE item-8 residue), so it "
						 "prices a bonded joint against an envelope the gate no longer allows — "
						 "measured 6.7x more comfortable on the session wall. A planar structure "
						 "only stays clear of that because it is posed in 2D; this line is what "
						 "notices if it stops being. 2D %.12g, flagged %.12g"),
					Row.Name, Joint, Here.Utilisation, There.Utilisation),
				AgreesToNineFigures(Here.Utilisation, There.Utilisation));

			TestTrue(
				*FString::Printf(
					TEXT("[TRIPWIRE] PARITY '%s': joint %d's normal force must agree (compression "
						 "positive). 2D %.12g uu, flagged %.12g uu"),
					Row.Name, Joint, Here.NormalUu, There.NormalUu),
				AgreesToNineFigures(Here.NormalUu, There.NormalUu));

			TestTrue(
				*FString::Printf(
					TEXT("[TRIPWIRE] PARITY '%s': joint %d's violation slack must agree. 2D %.12g uu, "
						 "flagged %.12g uu"),
					Row.Name, Joint, Here.ViolationUu, There.ViolationUu),
				AgreesToNineFigures(Here.ViolationUu, There.ViolationUu));
		}

		CasesWithAReadout += PresentHere > 0 ? 1 : 0;

		AddInfo(FString::Printf(
			TEXT("case '%s': passes %d/%d, %d piece(s) lost the earth in the 2D pose, %d joint(s) "
				 "carry a 2D readout"),
			Row.Name, PlanarPasses, VolumetricPasses, FellPlanar, PresentHere));
	}

	/* --- THE FLOORS: this corpus must actually exercise both arms ----------------------------- */

	TestTrue(
		*FString::Printf(
			TEXT("FLOOR: at least one fixture must genuinely FELL pieces, or every comparison above "
				 "is two identical rows of 'nothing happened' and the collapse arm is untested. %d "
				 "of %d did"),
			CasesWithAFall, static_cast<int32>(UE_ARRAY_COUNT(Corpus))),
		CasesWithAFall > 0);

	TestTrue(
		*FString::Printf(
			TEXT("FLOOR: at least one fixture must carry an LP READOUT, or the readout comparison is "
				 "vacuously green on absent-vs-absent and the 6.7x discrepancy this test exists to "
				 "catch would never be looked at. %d of %d did"),
			CasesWithAReadout, static_cast<int32>(UE_ARRAY_COUNT(Corpus))),
		CasesWithAReadout > 0);

	return true;
}

/*
 * The regional bridge poses its region on the same rule — B2.
 *
 * `BuildRegionalProblem` chooses its pose from the joints it poses — the region, its
 * grounded boundary ring, and nothing else — so a planar neighbourhood carved out of a
 * 3D-flagged structure is posed in 2D, and a neighbourhood containing a posed out-of-plane
 * joint is posed in 3D.
 *
 * Why the regional bridge needs its own coverage: it is a separate function from
 * `BuildRigidBlockProblem` on purpose (RigidBlockBridge.h) — the shared bridge poses the
 * problems the flagship scenarios and the oracle sweep pin byte-for-byte, and forcing a
 * boundary set grounded inside it would shift them. Two functions means two copies of the
 * pose choice, and `Core.Oracle.PlanarProblemUnderThe3DFlagPosesIn2D` exercises only one:
 * reverting the regional copy alone leaves that test green while every regional prove on a
 * straight player wall silently pays the 3D solve. It matters more here than at the
 * whole-structure bridge, because the prover's whole reason for existing is latency — it
 * is what makes a cap-200 corner-hang 0.53 s instead of 25 minutes (CURRENT_STATE,
 * grow-from-modest), so a regional pose that goes 3D on a planar wall spends the saving
 * twice over. Measured, not assumed (2026-09-16): replacing the regional pose choice with
 * the flag alone turns cases one and three red here while leaving the whole-structure
 * planar-pose tests all green — the gap this test closes.
 *
 * The region is what decides, not the structure around it. Case three is the
 * discriminating one, the regional analogue of the whole-structure bridge's earth-level L:
 * the structure contains Y-normal joints, but the region does not pose any of them — one
 * end of each is a piece outside R u B, or both ends are the grounded boundary ring. An
 * implementation that asked the structure's flag, or scanned `NumConnections()`, or reused
 * the whole-structure predicate over the whole piece array, gets that case wrong in the
 * expensive direction. Case two is its mirror: a region whose interior contains a Y-normal
 * joint must go on being posed 3D, since the 2D X-Z oracle cannot express one.
 *
 * What is asserted, and the observable: `FOracleProblem::Dim` off `BuildRegionalProblem`'s
 * own output, plus the refusal string on the one case that must stay refused — the
 * mechanism, the posed dimension itself, binary and exact, never a solve time or pivot count.
 *
 * No ticking world needed: hand-laid layouts and one bridge call each.
 *
 * Units: no force is compared against a strength anywhere here, so the 1 N = 100 uu
 * boundary is never crossed. Every reading is a dimension, a count, or a string.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FRegionalBridgePosesThePlanarRegionInTwoDTest,
	"DestructionGame.Core.Oracle.RegionalBridgePosesThePlanarRegionIn2D",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FRegionalBridgePosesThePlanarRegionInTwoDTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace PlanarPoseTestSupport;
	using namespace RigidBlockOracle;

	/** Every piece joined to `Piece` by a live joint — "its bed and head neighbours", from the graph. */
	auto NeighboursOf = [](const FStructure& Structure, int32 Piece)
	{
		TSet<int32> Out;

		for (int32 Joint = 0; Joint < Structure.NumConnections(); ++Joint)
		{
			const FConnection& Conn = Structure.GetConnection(Joint);

			if (Conn.HasGiven())
			{
				continue;
			}

			if (Conn.PieceA == Piece)
			{
				Out.Add(Conn.PieceB);
			}
			else if (Conn.PieceB == Piece)
			{
				Out.Add(Conn.PieceA);
			}
		}

		return Out;
	};

	/* --- ONE: A COURSE-1 BRICK AND ITS RING, OUT OF A 3D-FLAGGED STRAIGHT WALL ---------------- */

	{
		FBrickLayout Wall;

		if (!LayStraightWall(Wall))
		{
			AddError(TEXT("fixture: RunningBond must lay the two-course wall"));
			return true;
		}

		Wall.Structure.SetThreeDimensional(true);

		/* The first ungrounded piece the producer laid: a course-1 brick, bedded on two below. */
		int32 Seed = INDEX_NONE;

		for (int32 Piece = 0; Piece < Wall.Structure.NumPieces(); ++Piece)
		{
			if (!Wall.Structure.GetPiece(Piece).bIsGrounded)
			{
				Seed = Piece;
				break;
			}
		}

		if (Seed == INDEX_NONE)
		{
			AddError(TEXT("fixture: a two-course wall must have an ungrounded course-1 brick"));
			return true;
		}

		const TSet<int32> Region{ Seed };
		const TSet<int32> Boundary = NeighboursOf(Wall.Structure, Seed);

		AddInfo(FString::Printf(
			TEXT("STRAIGHT REGION: seed piece %d at (%g, %g, %g), boundary ring of %d"),
			Seed,
			Wall.Structure.GetPiece(Seed).CentreOfMassCm.X,
			Wall.Structure.GetPiece(Seed).CentreOfMassCm.Y,
			Wall.Structure.GetPiece(Seed).CentreOfMassCm.Z,
			Boundary.Num()));

		TestEqual(
			TEXT("fixture: the seed sits on course 1 — an ungrounded brick with a bed under it is "
				 "what makes the region pose any equilibrium row at all"),
			Wall.Structure.GetPiece(Seed).CentreOfMassCm.Z, Course1ZCm);
		TestEqual(
			TEXT("fixture: its ring is two beds below and one head alongside, so the pose is not a "
				 "lone block with nothing to be held by"),
			Boundary.Num(), 3);
		TestEqual(
			TEXT("fixture: a running-bond wall has NO out-of-plane normal anywhere — beds are Z, "
				 "heads are X"),
			CountOutOfPlaneJoints(Wall.Structure), 0);

		FOracleProblem Problem;
		FString WhyNot;
		const bool bBridged = BuildRegionalProblem(Wall.Structure, Region, Boundary, Problem, WhyNot);

		AddInfo(FString::Printf(
			TEXT("STRAIGHT REGION: bridged %d (\"%s\"), posed %s with %d blocks and %d joints"),
			bBridged ? 1 : 0, *WhyNot, DimName(Problem.Dim),
			Problem.Blocks.Num(), Problem.Joints.Num()));

		TestTrue(TEXT("fixture: a planar region must bridge whatever the structure is flagged"),
			bBridged);
		TestTrue(
			*FString::Printf(
				TEXT("fixture: the region must POSE joints (%d), or 'every posed joint is in-plane' "
					 "is vacuously true and this case discriminates nothing"),
				Problem.Joints.Num()),
			Problem.Joints.Num() > 0);

		TestEqual(
			*FString::Printf(
				TEXT("A PLANAR REGION OF A 3D-FLAGGED WALL MUST BE POSED IN 2D. Every joint this "
					 "region poses has an in-plane normal at one Y, so the 3D pose's out-of-plane "
					 "force row and its two out-of-plane moment rows are linear combinations of the "
					 "in-plane ones. The regional bridge is a SECOND copy of the pose choice — "
					 "reverting it alone leaves the whole-structure test green while every regional "
					 "prove on a straight player wall pays the 3D solve, and the prover exists for "
					 "latency. Posed %s"),
				DimName(Problem.Dim)),
			static_cast<int32>(Problem.Dim), static_cast<int32>(EOracleDim::Dim2D));
	}

	/* --- TWO: A REGION WHOSE INTERIOR HOLDS A Y-NORMAL JOINT IS STILL POSED IN 3D ------------- */

	{
		FBrickLayout Corner;

		if (!LayCornerWithAPosedOutOfPlaneJoint(Corner))
		{
			AddError(TEXT("fixture: the six-piece L must lay through PlacePiece"));
			return true;
		}

		Corner.Structure.SetThreeDimensional(true);

		/* The two course-1 rotated bricks, and the three course-0 returns they bed onto. */
		const TSet<int32> Region{ 4, 5 };
		const TSet<int32> Boundary{ 1, 2, 3 };

		int32 InteriorOutOfPlane = 0;

		for (int32 Joint = 0; Joint < Corner.Structure.NumConnections(); ++Joint)
		{
			AddInfo(DescribeJoint(Corner.Structure, Joint));

			const FConnection& Conn = Corner.Structure.GetConnection(Joint);

			if (NormalIsOutOfPlane(Conn) && Region.Contains(Conn.PieceA) && Region.Contains(Conn.PieceB))
			{
				++InteriorOutOfPlane;
			}
		}

		TestEqual(
			TEXT("FIXTURE, AND THE WHOLE POINT OF PUTTING BOTH COURSE-1 BRICKS IN THE REGION: the "
				 "head joint 5-4 is Y-normal and both its ends are interior, so the region poses it. "
				 "With it boundary-to-boundary this case would be case THREE and pin nothing"),
			InteriorOutOfPlane, 1);

		FOracleProblem Problem;
		FString WhyNot;
		const bool bBridged =
			BuildRegionalProblem(Corner.Structure, Region, Boundary, Problem, WhyNot);

		AddInfo(FString::Printf(
			TEXT("CORNER REGION: bridged %d (\"%s\"), posed %s with %d blocks and %d joints"),
			bBridged ? 1 : 0, *WhyNot, DimName(Problem.Dim),
			Problem.Blocks.Num(), Problem.Joints.Num()));

		TestTrue(TEXT("a 3D-flagged corner region must bridge — its out-of-plane normal is permitted"),
			bBridged);

		TestEqual(
			*FString::Printf(
				TEXT("[NET] A REGION WITH A POSED OUT-OF-PLANE JOINT MUST STILL BE POSED IN 3D. The "
					 "2D X-Z oracle cannot express a Y-facing joint at all, so posing this region "
					 "planar would be a plausible number with wrong statics — and the prover's answer "
					 "is a COLLAPSE it stitches into the cascade. Posed %s"),
				DimName(Problem.Dim)),
			static_cast<int32>(Problem.Dim), static_cast<int32>(EOracleDim::Dim3D));
	}

	/* --- THREE: Y-NORMAL JOINTS THE REGION DOES NOT POSE BUY NO 3D POSE ----------------------- */

	{
		FBrickLayout Corner;

		if (!LayEarthLevelCorner(Corner))
		{
			AddError(TEXT("fixture: the five-piece earth-level L must lay through PlacePiece"));
			return true;
		}

		Corner.Structure.SetThreeDimensional(true);

		/* The course-1 stretcher alone, held by the two course-0 stretchers under it. Pieces 2 and
		 * 3 — the whole Y-leg — are outside R u B and therefore absent. */
		const TSet<int32> Region{ 4 };
		const TSet<int32> Boundary{ 0, 1 };

		int32 OutOfPlaneTotal = 0;
		int32 OutOfPlaneTheRegionWouldPose = 0;

		for (int32 Joint = 0; Joint < Corner.Structure.NumConnections(); ++Joint)
		{
			AddInfo(DescribeJoint(Corner.Structure, Joint));

			const FConnection& Conn = Corner.Structure.GetConnection(Joint);

			if (!NormalIsOutOfPlane(Conn))
			{
				continue;
			}

			++OutOfPlaneTotal;

			const bool bIncludedA = Region.Contains(Conn.PieceA) || Boundary.Contains(Conn.PieceA);
			const bool bIncludedB = Region.Contains(Conn.PieceB) || Boundary.Contains(Conn.PieceB);
			const bool bBothBoundary =
				Boundary.Contains(Conn.PieceA) && Boundary.Contains(Conn.PieceB);

			if (bIncludedA && bIncludedB && !bBothBoundary)
			{
				++OutOfPlaneTheRegionWouldPose;
			}
		}

		TestTrue(
			*FString::Printf(
				TEXT("FIXTURE: this L really does carry out-of-plane joints (%d) — otherwise it is "
					 "another straight wall and case ONE already covers it"),
				OutOfPlaneTotal),
			OutOfPlaneTotal > 0);
		TestEqual(
			TEXT("FIXTURE: and the region poses NONE of them — each one either names a piece outside "
				 "R u B, or joins two pieces of the grounded boundary ring"),
			OutOfPlaneTheRegionWouldPose, 0);

		FOracleProblem Problem;
		FString WhyNot;
		const bool bBridged =
			BuildRegionalProblem(Corner.Structure, Region, Boundary, Problem, WhyNot);

		AddInfo(FString::Printf(
			TEXT("EARTH-LEVEL REGION: bridged %d (\"%s\"), posed %s with %d blocks and %d joints"),
			bBridged ? 1 : 0, *WhyNot, DimName(Problem.Dim),
			Problem.Blocks.Num(), Problem.Joints.Num()));

		TestTrue(TEXT("fixture: it must bridge"), bBridged);
		TestTrue(
			*FString::Printf(
				TEXT("fixture: the region must POSE joints (%d) — the two beds under piece 4"),
				Problem.Joints.Num()),
			Problem.Joints.Num() > 0);

		TestEqual(
			*FString::Printf(
				TEXT("AN OUT-OF-PLANE JOINT THE REGION DOES NOT POSE DOES NOT MAKE IT 3D. The rule "
					 "is over what this PROBLEM contains: pieces 2 and 3 are outside R u B, so their "
					 "Y-normal heads never reach the LP and the posed region holds not one "
					 "out-of-plane row. An implementation that asked the STRUCTURE — its flag, its "
					 "NumConnections(), its whole-piece-array predicate — would pay the 3D solve for "
					 "a planar LP. Posed %s"),
				DimName(Problem.Dim)),
			static_cast<int32>(Problem.Dim), static_cast<int32>(EOracleDim::Dim2D));
	}

	/* --- FOUR: THE UNFLAGGED PATH IS UNCHANGED — 2D STAYS 2D, AND A CORNER STAYS REFUSED ------ */

	{
		FBrickLayout Wall;

		if (!LayStraightWall(Wall))
		{
			AddError(TEXT("fixture: RunningBond must lay the two-course wall"));
			return true;
		}

		TestFalse(TEXT("fixture: unflagged is 2D, the default"), Wall.Structure.IsThreeDimensional());

		int32 Seed = INDEX_NONE;

		for (int32 Piece = 0; Piece < Wall.Structure.NumPieces(); ++Piece)
		{
			if (!Wall.Structure.GetPiece(Piece).bIsGrounded)
			{
				Seed = Piece;
				break;
			}
		}

		const TSet<int32> Region{ Seed };
		const TSet<int32> Boundary = NeighboursOf(Wall.Structure, Seed);

		FOracleProblem Problem;
		FString WhyNot;
		const bool bBridged = BuildRegionalProblem(Wall.Structure, Region, Boundary, Problem, WhyNot);

		AddInfo(FString::Printf(
			TEXT("STRAIGHT REGION 2D-FLAGGED: bridged %d (\"%s\"), posed %s"),
			bBridged ? 1 : 0, *WhyNot, DimName(Problem.Dim)));

		TestTrue(TEXT("[NET] a 2D-flagged planar region still bridges"), bBridged);
		TestEqual(
			TEXT("[NET] and is still posed in 2D — the unflagged path must stay byte-for-byte what it "
				 "was, which is what keeps every pinned regional-prover reading valid"),
			static_cast<int32>(Problem.Dim), static_cast<int32>(EOracleDim::Dim2D));
	}

	{
		FBrickLayout Corner;

		if (!LayCornerWithAPosedOutOfPlaneJoint(Corner))
		{
			AddError(TEXT("fixture: the six-piece L must lay through PlacePiece"));
			return true;
		}

		TestFalse(TEXT("fixture: this copy is NOT flagged — no permission to pose 3D"),
			Corner.Structure.IsThreeDimensional());

		const TSet<int32> Region{ 4, 5 };
		const TSet<int32> Boundary{ 1, 2, 3 };

		FOracleProblem Problem;
		FString WhyNot;
		const bool bBridged =
			BuildRegionalProblem(Corner.Structure, Region, Boundary, Problem, WhyNot);

		AddInfo(FString::Printf(
			TEXT("CORNER REGION 2D-FLAGGED: bridged %d (\"%s\"), %d blocks, %d joints"),
			bBridged ? 1 : 0, *WhyNot, Problem.Blocks.Num(), Problem.Joints.Num()));

		TestFalse(
			TEXT("[NET] A 2D-FLAGGED CORNER REGION MUST STILL BE REFUSED. Choosing the cheap pose "
				 "where it is sound must not become choosing the expensive pose where it is "
				 "forbidden: a 2D structure that has accidentally acquired an out-of-plane joint "
				 "stays loudly refused rather than silently promoted (the E3 ruling)"),
			bBridged);

		TestTrue(
			*FString::Printf(
				TEXT("[NET] and refused for the out-of-plane normal, by name. Got \"%s\""), *WhyNot),
			WhyNot.Contains(TEXT("out-of-plane")));

		TestEqual(TEXT("[NET] a refusal empties the problem, so a caller who ignores it solves nothing"),
			Problem.Blocks.Num(), 0);
	}

	return true;
}

/*
 * Coplanar normals are not enough: rows at different Y must be posed in 3D — B3.
 *
 * A 3D-flagged structure whose posed joint normals all lie in X-Z is still posed in 3D
 * whenever the Y values entering its equilibrium rows — each ungrounded block's centroid
 * and each posed patch's centre — are not one common Y.
 *
 * Why the normals alone cannot decide it, the failure this test exists for: "every posed
 * normal is in X-Z" is the obvious half of the planarity test and is not sufficient.
 * Moments are taken about each block's own centroid with lever arms out to its contacts,
 * so a bed joint whose patch sits at one Y under a block whose weight acts at another
 * generates a genuine out-of-plane moment demand with a +Z normal and nothing else.
 * Project that onto a single plane and the lever arm vanishes: an overhang that topples
 * reads as a centred load and stands. Both fixtures here have zero Y-normal joints, so a
 * predicate that scanned only normals would call both planar, pose both in 2D, and be
 * wrong in the expensive direction, where the 2D answer is not conservative but a
 * different structure.
 *
 * The two fixtures, and why each is asserted differently. The Y-cantilever asserts the
 * pose and the consequence: one brick bedded on another and shoved 7.5 cm along Y leaves
 * 2.75 cm of the 10.25 cm wythe overlapping, so the bed patch spans Y 2.375..5.125 with
 * its centre at 3.75 and a Y half-extent of 1.375, while the brick's weight acts at
 * Y = 7.5 — 2.73 kern-widths outside the patch, on a `DryStone` joint with zero tensile
 * and cohesion. There is no admissible force system: it must fall. The control is the
 * same two bricks with the top one at Y = 0, a fully-bedded brick that must stand. The
 * verdict is asserted, not just the dimension, because a dimension reading alone would be
 * satisfied by a predicate that answers 3D for the wrong reason — `SolveAndBreak` felling
 * the brick is the outcome the pose exists to get right, and the two readings fail
 * independently. No displacement is asserted anywhere (DESIGN §4): the reading is
 * `GetPieceSupport`, an enum. The overturning gate is not what decides it either:
 * `PieceOverturnsOffItsSupports` is gated on `LoadPaths[Current].Num() >= 2`, and this
 * brick has exactly one joint, so the router's gate cannot reach it and the LP is the only
 * authority that can answer (two pieces is far below the 200-block `EquilibriumGateBlockCap`).
 *
 * Two walls and one plank asserts the dimension alone, deliberately: a timber plank
 * spanning two brick bearings 40 cm apart in Y, both bed patches carrying +/-Z normals,
 * the plank's centroid at Y = 20 and the two patch centres at Y = 0.0625 and Y = 39.9375
 * — the roof-on-two-walls shape the predicate's own header names. Only the pose is read
 * because a symmetric plank on two symmetric bearings stands either way; its verdict would
 * discriminate nothing. Its control is the identical plank and bearings rotated to span X,
 * where every row really is at one Y.
 *
 * Which fixture pins which half — measured by mutation 2026-09-16, not assumed.
 * `PosedProblemLeavesThePlane` asks its common-Y question about two kinds of row, and the
 * two fixtures do not cover one each: deleting the patch-centre check
 * (`SitsOffThePlane(Joint.InterfaceCentreCm.Y)`) turns both the cantilever (poses 2D,
 * solves in 0 passes, reads Supported) and the plank (poses 2D) red. Deleting the
 * ungrounded-block centroid check turns the cantilever red (poses 2D and stands) but
 * leaves the plank green, since its two bearing patches disagree with each other and the
 * patch check alone catches it. So the cantilever is the fixture that pins both halves —
 * the one to look at first if this test ever goes red — and the plank pins the
 * patch-centre half while giving the shape a reader recognises. Do not "simplify" by
 * dropping the cantilever.
 *
 * What is asserted, and the observable: `FOracleProblem::Dim` off `BuildRigidBlockProblem`,
 * and `FStructure::GetPieceSupport` after `SolveAndBreak` on the cantilever — mechanism
 * and outcome, never a proxy.
 *
 * No ticking world needed: `SolveAndBreak` is a world-free structural cascade, not a Chaos tick.
 *
 * Units: nothing compares a force against a strength, so the conversion boundary is not
 * crossed. Masses come from `PieceMassKg` at published densities (clay brick 1.9 g/cm3,
 * C24 softwood 0.42 g/cm3), which take no conversion, and every assertion reads a
 * dimension or a support enum.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCoplanarNormalsAtDifferentYPoseInThreeDTest,
	"DestructionGame.Core.Oracle.CoplanarNormalsAtDifferentYPoseIn3D",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCoplanarNormalsAtDifferentYPoseInThreeDTest::RunTest(const FString& Parameters)
{
	using namespace PlanarPoseTestSupport;
	using namespace RigidBlockOracle;

	/** The brick's Y shove: 7.5 cm of the 10.25 cm wythe, leaving 2.75 cm of bed. */
	constexpr double CantileverOffsetYCm = 7.5;

	/* --- ONE: THE Y-CANTILEVER IS POSED IN 3D, AND FALLS ------------------------------------- */

	{
		FHandLaidStructure Fixture;

		if (!LayYCantilever(CantileverOffsetYCm, Fixture) || Fixture.Joints.Num() != 1)
		{
			AddError(TEXT("fixture: the Y-cantilever must lay two bricks and one bed joint"));
			return true;
		}

		Fixture.Structure.SetThreeDimensional(true);

		const int32 Top = Fixture.Pieces[1];

		int32 Posed = 0;
		int32 PosedOutOfPlane = 0;
		CountPosedJoints(Fixture.Structure, Posed, PosedOutOfPlane);

		AddInfo(DescribeJoint(Fixture.Structure, Fixture.Joints[0]));
		AddInfo(FString::Printf(
			TEXT("Y-CANTILEVER: top brick centroid Y %g, patch centre Y %g, patch half-Y %g"),
			Fixture.Structure.GetPiece(Top).CentreOfMassCm.Y,
			Fixture.Structure.GetConnection(Fixture.Joints[0]).InterfaceCentreCm.Y,
			Fixture.Structure.GetConnection(Fixture.Joints[0]).InterfaceHalfExtentCm.Y));

		TestEqual(
			TEXT("FIXTURE, AND THE ENTIRE POINT: not one normal leaves the X-Z plane. A predicate "
				 "that read only normals would call this planar"),
			CountOutOfPlaneJoints(Fixture.Structure), 0);
		TestEqual(TEXT("fixture: the one bed joint is posed — one end is ungrounded"), Posed, 1);
		TestTrue(
			TEXT("FIXTURE: and the brick's weight really does act OUTSIDE its bed patch in Y, which "
				 "is the demand the 2D projection would erase"),
			FMath::Abs(
				Fixture.Structure.GetPiece(Top).CentreOfMassCm.Y
				- Fixture.Structure.GetConnection(Fixture.Joints[0]).InterfaceCentreCm.Y)
			> Fixture.Structure.GetConnection(Fixture.Joints[0]).InterfaceHalfExtentCm.Y);

		FOracleProblem Problem;
		FString WhyNot;
		const bool bBridged = BuildRigidBlockProblem(Fixture.Structure, Problem, WhyNot);

		AddInfo(FString::Printf(
			TEXT("Y-CANTILEVER: bridged %d (\"%s\"), posed %s with %d blocks and %d joints"),
			bBridged ? 1 : 0, *WhyNot, DimName(Problem.Dim),
			Problem.Blocks.Num(), Problem.Joints.Num()));

		TestTrue(TEXT("fixture: it must bridge"), bBridged);

		TestEqual(
			*FString::Printf(
				TEXT("COPLANAR NORMALS ARE NOT ENOUGH: A POSED PATCH AT A DIFFERENT Y FROM THE BLOCK "
					 "IT CARRIES MUST BE POSED IN 3D. Every normal here is +/-Z, yet the brick's "
					 "weight acts 3.75 cm from its bed patch's centre against a 1.375 cm half-extent "
					 "— a genuine out-of-plane moment demand with no Y-normal joint anywhere. "
					 "Projecting it onto one plane deletes the lever arm and stands the overhang "
					 "back up. Posed %s"),
				DimName(Problem.Dim)),
			static_cast<int32>(Problem.Dim), static_cast<int32>(EOracleDim::Dim3D));

		const int32 Passes = Fixture.Structure.SolveAndBreak();

		AddInfo(FString::Printf(
			TEXT("Y-CANTILEVER: %d pass(es), top brick support %d (0 Falling, 1 Grounded, "
				 "2 Supported, 3 Stranded)"),
			Passes, static_cast<int32>(Fixture.Structure.GetPieceSupport(Top))));

		TestEqual(
			*FString::Printf(
				TEXT("AND THE OVERHANG MUST ACTUALLY GO. A DryStone bed has tensile 0.0 MPa and "
					 "cohesion 0.0 MPa, and the brick's centre of mass sits 3.75 cm out along Y "
					 "against a 1.375 cm patch half-extent, so no admissible force system exists and "
					 "the LP — the break authority at 2 blocks, far below the 200-block cap — must "
					 "fell it. The router's overturning gate cannot be what does this: it needs "
					 "LoadPaths.Num() >= 2 and this brick has one joint. NO DISPLACEMENT IS READ; a "
					 "released brick and a brick resting where it was are the same picture. Support "
					 "reads %d, Falling is 0"),
				static_cast<int32>(Fixture.Structure.GetPieceSupport(Top))),
			static_cast<int32>(Fixture.Structure.GetPieceSupport(Top)),
			static_cast<int32>(EPieceSupport::Falling));
	}

	/* --- TWO: THE CONTROL — THE SAME TWO BRICKS FULLY BEDDED, POSED IN 2D, AND STANDING ------- */

	{
		FHandLaidStructure Fixture;

		if (!LayYCantilever(0.0, Fixture) || Fixture.Joints.Num() != 1)
		{
			AddError(TEXT("fixture: the fully-bedded control must lay two bricks and one bed joint"));
			return true;
		}

		Fixture.Structure.SetThreeDimensional(true);

		const int32 Top = Fixture.Pieces[1];

		AddInfo(DescribeJoint(Fixture.Structure, Fixture.Joints[0]));

		FOracleProblem Problem;
		FString WhyNot;
		const bool bBridged = BuildRigidBlockProblem(Fixture.Structure, Problem, WhyNot);

		AddInfo(FString::Printf(
			TEXT("BEDDED CONTROL: bridged %d (\"%s\"), posed %s; patch centre Y %g, centroid Y %g"),
			bBridged ? 1 : 0, *WhyNot, DimName(Problem.Dim),
			Fixture.Structure.GetConnection(Fixture.Joints[0]).InterfaceCentreCm.Y,
			Fixture.Structure.GetPiece(Top).CentreOfMassCm.Y));

		TestTrue(TEXT("fixture: it must bridge"), bBridged);

		TestEqual(
			*FString::Printf(
				TEXT("[CONTROL] THE SAME TWO BRICKS AT ONE Y ARE POSED IN 2D. Without this the "
					 "cantilever above is satisfied by 'always pose 3D', which would put every "
					 "straight player wall back on the 37x solve. Posed %s"),
				DimName(Problem.Dim)),
			static_cast<int32>(Problem.Dim), static_cast<int32>(EOracleDim::Dim2D));

		Fixture.Structure.SolveAndBreak();

		TestEqual(
			*FString::Printf(
				TEXT("[CONTROL] and it STANDS — the difference between the two cases is 7.5 cm of Y "
					 "and nothing else, so a cantilever that fell for some reason other than its "
					 "overhang would fell this one too. Support reads %d, Supported is 2"),
				static_cast<int32>(Fixture.Structure.GetPieceSupport(Top))),
			static_cast<int32>(Fixture.Structure.GetPieceSupport(Top)),
			static_cast<int32>(EPieceSupport::Supported));
	}

	/* --- THREE: TWO WALLS AND ONE PLANK — CENTROID Y BETWEEN TWO BEARING Ys ------------------- */

	{
		FHandLaidStructure Fixture;

		if (!LayTwoWallsOnePlank(/*bSpanAlongY*/ true, Fixture) || Fixture.Joints.Num() != 2)
		{
			AddError(TEXT("fixture: the Y-spanning plank must lay three pieces and two bed joints"));
			return true;
		}

		Fixture.Structure.SetThreeDimensional(true);

		int32 Posed = 0;
		int32 PosedOutOfPlane = 0;
		CountPosedJoints(Fixture.Structure, Posed, PosedOutOfPlane);

		for (int32 Joint = 0; Joint < Fixture.Structure.NumConnections(); ++Joint)
		{
			AddInfo(DescribeJoint(Fixture.Structure, Joint));
		}

		AddInfo(FString::Printf(
			TEXT("PLANK ON TWO WALLS: plank centroid Y %g, bearing patch centres Y %g and %g"),
			Fixture.Structure.GetPiece(Fixture.Pieces[2]).CentreOfMassCm.Y,
			Fixture.Structure.GetConnection(Fixture.Joints[0]).InterfaceCentreCm.Y,
			Fixture.Structure.GetConnection(Fixture.Joints[1]).InterfaceCentreCm.Y));

		TestEqual(
			TEXT("FIXTURE, AND THE ENTIRE POINT: both bearings are +/-Z bed joints and not one "
				 "normal leaves the X-Z plane"),
			CountOutOfPlaneJoints(Fixture.Structure), 0);
		TestEqual(TEXT("fixture: both bearings are posed — the plank is the ungrounded end of each"),
			Posed, 2);

		FOracleProblem Problem;
		FString WhyNot;
		const bool bBridged = BuildRigidBlockProblem(Fixture.Structure, Problem, WhyNot);

		AddInfo(FString::Printf(
			TEXT("PLANK ON TWO WALLS: bridged %d (\"%s\"), posed %s with %d blocks and %d joints"),
			bBridged ? 1 : 0, *WhyNot, DimName(Problem.Dim),
			Problem.Blocks.Num(), Problem.Joints.Num()));

		TestTrue(TEXT("fixture: it must bridge"), bBridged);

		TestEqual(
			*FString::Printf(
				TEXT("A ROOF BEARING ON TWO WALLS AT DIFFERENT Y MUST BE POSED IN 3D. This is the "
					 "shape the planarity predicate's own header names: every normal is +/-Z, the "
					 "plank's weight acts at Y = 20 and its two bearings at Y = 0.0625 and 39.9375, "
					 "so the two lever arms are real and opposite. Flattened onto one plane the "
					 "plank bears twice at the same place, and an overhang that topples reads as "
					 "standing. Posed %s"),
				DimName(Problem.Dim)),
			static_cast<int32>(Problem.Dim), static_cast<int32>(EOracleDim::Dim3D));
	}

	/* --- FOUR: THE CONTROL — THE SAME PLANK AND BEARINGS SPANNING X, ALL AT ONE Y ------------- */

	{
		FHandLaidStructure Fixture;

		if (!LayTwoWallsOnePlank(/*bSpanAlongY*/ false, Fixture) || Fixture.Joints.Num() != 2)
		{
			AddError(TEXT("fixture: the X-spanning control must lay three pieces and two bed joints"));
			return true;
		}

		Fixture.Structure.SetThreeDimensional(true);

		for (int32 Joint = 0; Joint < Fixture.Structure.NumConnections(); ++Joint)
		{
			AddInfo(DescribeJoint(Fixture.Structure, Joint));
		}

		FOracleProblem Problem;
		FString WhyNot;
		const bool bBridged = BuildRigidBlockProblem(Fixture.Structure, Problem, WhyNot);

		AddInfo(FString::Printf(
			TEXT("PLANK SPANNING X: bridged %d (\"%s\"), posed %s; centroid Y %g, patch centres Y "
				 "%g and %g"),
			bBridged ? 1 : 0, *WhyNot, DimName(Problem.Dim),
			Fixture.Structure.GetPiece(Fixture.Pieces[2]).CentreOfMassCm.Y,
			Fixture.Structure.GetConnection(Fixture.Joints[0]).InterfaceCentreCm.Y,
			Fixture.Structure.GetConnection(Fixture.Joints[1]).InterfaceCentreCm.Y));

		TestTrue(TEXT("fixture: it must bridge"), bBridged);

		TestEqual(
			*FString::Printf(
				TEXT("[CONTROL] THE SAME PLANK AND BEARINGS ROTATED TO SPAN X ARE POSED IN 2D. Same "
					 "three boxes, same two +/-Z bed joints, same masses — only the axis they are "
					 "arranged along differs, so this is the one reading that isolates 'the Y values "
					 "differ' from 'a plank on two supports'. Posed %s"),
				DimName(Problem.Dim)),
			static_cast<int32>(Problem.Dim), static_cast<int32>(EOracleDim::Dim2D));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
