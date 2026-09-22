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
 * Planar pose: a 3D-flagged structure whose posed problem is planar is posed in 2D.
 *
 * `BuildRigidBlockProblem` poses 2D when (a) every posed joint has an in-plane normal and
 * (b) every Y entering an equilibrium row (ungrounded centroids, posed patch centres) is one
 * common Y; otherwise 3D. `IsThreeDimensional()` is only the permission to pose 3D, so a
 * 2D-flagged structure with an out-of-plane joint is still refused, never promoted.
 *
 * Why: BeginBuild flags every player build 3D (the E3 ruling, Structure.h
 * SetThreeDimensional), which made a straight 100-brick session wall solve in 94 s instead of
 * 2.5 s (~37x; CURRENT_STATE corner entry xiii). Timing is not asserted; OracleSweepFull owns
 * solver cost.
 *
 * 2D is also the more accurate pose. 3D-feasible implies 2D-feasible, not the converse: the 3D
 * friction pyramid is a k=8 octagon inscribed in the Coulomb cone (cos(pi/8) = 0.924), so a
 * shear-critical planar structure with lambda* in [1.0, 1.0824) falls posed 3D and stands
 * posed 2D. Every oracle-sweep pin is anchored on the 2D pose.
 *
 * Only posed joints count: the bridge skips given joints and grounded-to-grounded joints, so
 * an earth-level L poses planar (case three). The observable is `FOracleProblem::Dim` itself.
 * No ticking world, and no force-vs-strength comparison, so no unit conversion.
 *
 * Named namespace: unity builds merge translation units.
 */
namespace PlanarPoseTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace RigidBlockOracle;

	/*
	 * Coordinating grid, spelled out: a 21.5 x 10.25 x 6.5 cm brick on a 1 cm joint, so course n
	 * centres at n * 7.5 + 3.25. A rotated brick swaps X and Y.
	 */

	const FVector HalfBrick(10.75, 5.125, 3.25);
	const FVector HalfBrickRotated(5.125, 10.75, 3.25);

	constexpr double Course0ZCm = 3.25;
	constexpr double Course1ZCm = 10.75;

	/** Clay brick, 1.9 g/cm3. */
	constexpr double BrickDensityGramsPerCubicCm = 1.9;

	/*
	 * Whether the bridge would pose this joint: not given, and not grounded-to-grounded.
	 * Transcribed rather than called, so the fixture does not ask the code under test.
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

	/** Whether the normal leaves the X-Z plane. Written `!(|Y| <= tol)` so a NaN answers true (the sound side). */
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

	/** Every out-of-plane joint, posed or skipped. */
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

	const TCHAR* DimName(EOracleDim Dim)
	{
		return Dim == EOracleDim::Dim3D ? TEXT("3D") : TEXT("2D");
	}

	/** Two courses of three in running bond: beds +/-Z, heads +/-X, no Y normal anywhere. */
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
	 * The six-piece L of `World.Session.CornerBuildIsJudgedByTheLP`, laid via PlacePiece.
	 * A rotated return off the seed's +X end (x = 10.75 + 1 + 5.125 = 16.875), two more rotated
	 * bricks along Y, then two course-1 bricks whose head joint 5-4 is the one posed
	 * out-of-plane joint (neither piece is grounded).
	 *
	 * Same-course cursors sit 0.125 cm short of the pitch so the same-course pose wins, as in the
	 * session.
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
	 * An L laid on the earth plus one course-1 brick on the X leg. Separates "every joint is
	 * in-plane" from "every posed joint is in-plane": the Y-normal head joint 3-2 joins two
	 * grounded pieces and is skipped, and the course-1 brick's two beds keep the posed set
	 * non-empty. Return at x = 22.5 + 10.75 + 1 + 5.125 = 39.375.
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
	 * Hand-laid fixtures for condition (b). The producers snap to one wythe and would pull Y back
	 * onto the plane, so these use AddPiece/AddConnection directly, with MakeInterface still
	 * building every joint (Layout.h).
	 */

	/** A structure laid brick by brick, with handles kept. */
	struct FHandLaidStructure
	{
		FStructure Structure;
		TArray<int32> Pieces;
		TArray<int32> Joints;
	};

	constexpr double JointThicknessCm = 1.0;

	/** C24 softwood, ~0.42 g/cm3. */
	constexpr double TimberDensityGramsPerCubicCm = 0.42;

	/** Lay one box as a piece (mass via PieceMassKg); returns its handle, or INDEX_NONE. */
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
	 * Y-cantilever: a grounded brick with a second bedded on it, shifted TopCentreYCm along Y.
	 * The bed normal is +Z, so the normals say planar while the Ys do not. At 7.5 the patch spans
	 * Y 2.375..5.125 (centre 3.75). DryStone (no tension or cohesion) so only statics hold it up.
	 * TopCentreYCm = 0 is the planar control.
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
	 * Two grounded bearings 40 cm apart spanned by a 21.5 x 50 x 5 cm timber plank. Every bed
	 * normal is +/-Z either way, so this isolates condition (b). Along Y, three Ys enter the rows
	 * (bearings 0 and 40, plank 20), which one plane cannot represent; along X everything is at
	 * Y = 0 (the planar control).
	 */
	bool LayTwoWallsOnePlank(bool bSpanAlongY, FHandLaidStructure& Out)
	{
		// Bottom at 7.5: the bearings' 6.5 cm top plus the 1 cm joint.
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
 * Which pose the bridge builds, case by case. Cases one and three pin the rule (they failed
 * when Dim came from the flag alone). Cases two, four and five are the regression net: a real
 * corner stays 3D, a 2D-flagged wall stays 2D, a 2D-flagged corner stays refused.
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

	// One: a straight wall flagged 3D is posed in 2D.

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

	// Two: a real corner flagged 3D is still posed in 3D.

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

	// Three: an out-of-plane joint the bridge skips does not make the pose 3D.

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

	// Four: a 2D-flagged planar wall is unchanged.

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

	// Five: a 2D-flagged corner is still refused, never promoted.

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
 * Tripwire: flagging a planar structure 3D changes no answer. Both copies pose the same 2D LP,
 * so this is green by construction and goes red only if the poses are re-split (a reverted
 * pose choice, a predicate that stops skipping earth-to-earth joints, a 3D-only readout path).
 * It does not show the formulations agree; they do not (3D caps in-plane shear at 0.924x).
 *
 * Compared: pass count and every piece's support enum; per-joint HasGiven and break pass;
 * per-joint readout; and the fixture's shape. No displacement (DESIGN §4).
 *
 * Measured disagreements when re-split: the 3D readout lacks the shear-cap and first-crack
 * rows (CURRENT_STATE item 8), reading 0.006 vs 0.041 on one joint; and with the bottom course
 * removed the 3D pose severed joints 2, 4, 7, 14-18 that 2D left intact, because a falling
 * body's LP dual is degenerate.
 *
 * Five small fixtures, not the catalogue: OracleSweepFull is the verification. The 1e-9
 * tolerance allows for different simplex paths if re-split; every other reading is exact.
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

	/** One corpus fixture and its removals, named by course rather than by handle. */
	struct FParityCase
	{
		const TCHAR* Name;
		TFunction<bool(FBrickLayout&)> Lay;

		/** Every piece whose box centre sits at this Z is removed before the solve. */
		double RemoveCourseAtZCm;

		/** Of those, only the one nearest this X; all of them if Max. */
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
		{ TEXT("running 3x3, intact"),
			LayWall(3, 3, DestructionWallCases::EWallBond::Running, INDEX_NONE, INDEX_NONE),
			TNumericLimits<double>::Max(), TNumericLimits<double>::Max() },

		// Load must find another path.
		{ TEXT("running 3x3, one brick out of the bottom course"),
			LayWall(3, 3, DestructionWallCases::EWallBond::Running, INDEX_NONE, INDEX_NONE),
			Course0ZCm, 22.5 },

		// Aligned head joints give a different load graph.
		{ TEXT("stack 3x3, intact"),
			LayWall(3, 3, DestructionWallCases::EWallBond::Stack, INDEX_NONE, INDEX_NONE),
			TNumericLimits<double>::Max(), TNumericLimits<double>::Max() },

		// Eccentric load, where moments decide.
		{ TEXT("running 3x3 corbelling from course 1"),
			LayWall(3, 3, DestructionWallCases::EWallBond::Running, 1, INDEX_NONE),
			TNumericLimits<double>::Max(), TNumericLimits<double>::Max() },

		// Must fall: nothing touches the earth. Ensures the corpus contains a collapse.
		{ TEXT("running 3x3 with its whole bottom course removed"),
			LayWall(3, 3, DestructionWallCases::EWallBond::Running, INDEX_NONE, INDEX_NONE),
			Course0ZCm, TNumericLimits<double>::Max() },
	};

	/** 1e-9, relative for large numbers and absolute for small ones. */
	auto AgreesToNineFigures = [](double A, double B)
	{
		const double Scale = FMath::Max(1.0, FMath::Max(FMath::Abs(A), FMath::Abs(B)));

		// `<=` so a NaN answers false.
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

		// The cut, applied identically to both copies.

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

		// Fixture guards: planar, below the cap, not empty.

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

		// Verdict: every piece's support enum.

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

		// Severed set and the pass each joint went in.

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

		// Readout: the first thing a re-split would break.

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

	// Floors: the corpus must exercise both a collapse and a readout.

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
 * B2: the regional bridge applies the same pose rule to the joints it poses (region plus
 * grounded boundary ring). `BuildRegionalProblem` is a separate function from
 * `BuildRigidBlockProblem` (RigidBlockBridge.h), so its pose choice needs its own pin:
 * reverting it to the flag alone turns cases one and three red here while the whole-structure
 * tests stay green (measured 2026-09-16). The prover exists for latency, so a needless 3D pose
 * costs it most.
 *
 * Case three is the discriminator: Y-normal joints exist but the region poses none of them.
 * Case two is its mirror. Observable: `FOracleProblem::Dim`, plus the refusal string.
 * No ticking world; no force-vs-strength comparison.
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

	/** Every piece joined to `Piece` by a live joint. */
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

	// One: a course-1 brick and its ring from a 3D-flagged straight wall.

	{
		FBrickLayout Wall;

		if (!LayStraightWall(Wall))
		{
			AddError(TEXT("fixture: RunningBond must lay the two-course wall"));
			return true;
		}

		Wall.Structure.SetThreeDimensional(true);

		// The first ungrounded piece: a course-1 brick.
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

	// Two: a region whose interior holds a Y-normal joint is still posed in 3D.

	{
		FBrickLayout Corner;

		if (!LayCornerWithAPosedOutOfPlaneJoint(Corner))
		{
			AddError(TEXT("fixture: the six-piece L must lay through PlacePiece"));
			return true;
		}

		Corner.Structure.SetThreeDimensional(true);

		// The two course-1 bricks, bedded on the three course-0 returns.
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

	// Three: Y-normal joints the region does not pose do not make it 3D.

	{
		FBrickLayout Corner;

		if (!LayEarthLevelCorner(Corner))
		{
			AddError(TEXT("fixture: the five-piece earth-level L must lay through PlacePiece"));
			return true;
		}

		Corner.Structure.SetThreeDimensional(true);

		// The course-1 stretcher on the two below it; the Y-leg (pieces 2, 3) is outside R u B.
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

	// Four: unflagged path unchanged; 2D stays 2D and a corner stays refused.

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
 * B3: coplanar normals are not enough. A 3D-flagged structure with every posed normal in X-Z is
 * still posed 3D when the Ys entering its rows (ungrounded centroids, posed patch centres)
 * differ. A bed patch at one Y under a weight at another is a real out-of-plane moment; the 2D
 * projection erases the lever arm and a toppling overhang stands.
 *
 * Y-cantilever: shifted 7.5 cm, the weight acts 3.75 cm from a patch of half-extent 1.375 on
 * DryStone, so it must fall. Pose and verdict are both asserted (GetPieceSupport, not
 * displacement). With one joint the router's overturning gate (needs >= 2 load paths) cannot
 * act, so the LP decides. The Y = 0 control must stand.
 *
 * Plank on two walls: asserts the pose only, since a symmetric plank stands either way. Its
 * control spans X.
 *
 * Mutation-checked 2026-09-16: dropping the patch-centre check reddens both fixtures; dropping
 * the centroid check reddens only the cantilever. Do not drop the cantilever.
 * No ticking world; no force-vs-strength comparison.
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

	// One: the Y-cantilever is posed in 3D and falls.

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

	// Two: control. Fully bedded, posed in 2D, standing.

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

	// Three: plank on two walls spanning Y.

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

	// Four: control. The same plank spanning X, all at one Y.

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
