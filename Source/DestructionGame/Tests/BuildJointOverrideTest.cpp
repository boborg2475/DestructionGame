// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Connection.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/SessionToolbar.h"
#include "Core/Structure.h"
#include "Tests/BrickWorldTestSupport.h"
#include "World/DestructionStructureSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Named namespace, not anonymous, and named differently from every other one in this directory. An
 * anonymous namespace is private to a translation unit rather than a file, and a unity build merges
 * many files into one — two file-local names that collide are then a hard compile error between
 * files that never refer to each other (CURRENT_STATE.md); `using namespace` lives inside each
 * RunTest for the same reason.
 */
namespace BuildJointOverrideTestSupport
{
	/* The running-bond grid, transcribed rather than imported, and the rests-on-the-ground
	 * convention with it (DESIGN §8, 2026-09-15). A 21.5 x 10.25 x 6.5 cm brick on 1 cm joints
	 * gives the 22.5 x 11.25 x 7.5 coordinating grid, and course 0 centres a brick at its own
	 * half height — 3.25 — so its underside is on the earth rather than half inside it. Every
	 * pose below is that arithmetic spelled out, so a retune of the brick or the joint fails on
	 * a fixture row rather than quietly agreeing with whatever the solver now snaps to. */
	const FVector OverrideHalfBrickCm(10.75, 5.125, 3.25);

	/** The demo building's own wall plate, half sizes. Timber, and 67.5 cm long. */
	const FVector OverrideHalfPlateCm(33.75, 5.125, 5.0);

	/** Course 0's centre for a brick: half a brick above the earth. */
	constexpr double OverrideCourseZeroBrickZCm = 3.25;

	/** The seed brick, and the same-course pose one 22.5 cm pitch along from it. */
	const FVector OverrideSeedCentreCm(0.0, 0.0, OverrideCourseZeroBrickZCm);
	const FVector OverrideSameCourseCentreCm(22.5, 0.0, OverrideCourseZeroBrickZCm);

	/**
	 * Where the cursor asks for the second brick, 1.5 cm short of the bond — off the grid
	 * deliberately, so the assertion that the piece lands at 22.5 is a reading of the solver
	 * rather than of the request. The nearest next-course pose is at (11.25, 0, 10.75), which is
	 * sqrt(9.75^2 + 7.5^2) = 12.3 cm from this cursor against the same-course pose's 1.5, so the
	 * same-course snap wins by a factor of eight and the fixture is nowhere near its own boundary.
	 */
	const FVector OverrideSecondCursorCm(21.0, 0.0, OverrideCourseZeroBrickZCm);

	/** The head joint's area: two bricks abutting end to end share their Y-Z face, 10.25 cm deep
	 * by 6.5 cm high = 66.625 cm2. MakeInterface reports the intersection of the two spans, which
	 * for equal boxes is the whole face. */
	constexpr double OverrideHeadAreaSqCm = 66.625;

	/** Where a plate bears on that course: the brick's top face is at 3.25 + 3.25 = 6.5, one 1 cm
	 * joint above that is 7.5, and the plate's own 5 cm half height puts its centre at 12.5. */
	constexpr double OverridePlateBearingZCm = 12.5;
	const FVector OverridePlateCentreCm(0.0, 0.0, OverridePlateBearingZCm);

	/** Each bearing's area: the 67.5 cm plate wholly covers each 21.5 cm brick in X and both are
	 * 10.25 deep, so the intersection is 21.5 x 10.25 = 220.375 cm2 on both supports. It is the
	 * brick's face rather than the plate's because MakeInterface intersects the spans — the number
	 * a lintel on a wide pier gets wrong if it subtracts reach from distance instead. */
	constexpr double OverrideBearingAreaSqCm = 220.375;

	/**
	 * Full-field profile identity, because FConnectionStrength has no operator==. All five fields,
	 * because this library is siblings by construction: GeneralPurposeMortar and its perpend
	 * differ on the two bond axes alone, and Nail, Screw and Bolt are one shape at three scales.
	 * Asserting one field would let a sibling through — exactly the failure an override has, since
	 * the claim is that the player got the profile they asked for, not a plausible neighbour.
	 */
	void CheckOverrideProfile(
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

	/** One joint's geometry, lifted off a connection so two runs can be held against each other. */
	struct FJointShape
	{
		int32 PieceA = INDEX_NONE;
		int32 PieceB = INDEX_NONE;
		double AreaSqCm = 0.0;
		FVector NormalUnit = FVector::ZeroVector;
	};

	FJointShape ShapeOf(const FConnection& Conn)
	{
		FJointShape Shape;
		Shape.PieceA = Conn.PieceA;
		Shape.PieceB = Conn.PieceB;
		Shape.AreaSqCm = Conn.InterfaceAreaSqCm;
		Shape.NormalUnit = Conn.InterfaceNormal;
		return Shape;
	}

	FString DescribeShape(const FJointShape& Shape)
	{
		return FString::Printf(
			TEXT("{%d-%d, %g cm2, normal (%g, %g, %g)}"),
			Shape.PieceA, Shape.PieceB, Shape.AreaSqCm,
			Shape.NormalUnit.X, Shape.NormalUnit.Y, Shape.NormalUnit.Z);
	}

	bool ShapesAreTheSameJoint(const FJointShape& A, const FJointShape& B)
	{
		return A.PieceA == B.PieceA
			&& A.PieceB == B.PieceB
			&& A.AreaSqCm == B.AreaSqCm
			&& A.NormalUnit == B.NormalUnit;
	}

	/** Which shipped connection profile a pointer IS, by address. Never a value comparison. */
	const TCHAR* OverrideProfileName(const FConnectionStrength* Profile)
	{
		using namespace DestructionProfiles;

		if (Profile == nullptr)                      { return TEXT("<nullptr>"); }
		if (Profile == &GeneralPurposeMortar)        { return TEXT("GeneralPurposeMortar"); }
		if (Profile == &GeneralPurposeMortarPerpend) { return TEXT("GeneralPurposeMortarPerpend"); }
		if (Profile == &LimeMortar)                  { return TEXT("LimeMortar"); }
		if (Profile == &DryStone)                    { return TEXT("DryStone"); }
		if (Profile == &Nail)                        { return TEXT("Nail"); }
		if (Profile == &Screw)                       { return TEXT("Screw"); }
		if (Profile == &Bolt)                        { return TEXT("Bolt"); }

		return TEXT("<not a shipped library row>");
	}

	/**
	 * Open a build and lay one grounded brick into it, Auto, through the real door.
	 *
	 * A fresh structure per case rather than one wall grown through every override: the snap
	 * solver's answer is a function of the pieces already standing, so laying the Auto brick and
	 * then the Mortar brick into one structure would compare two placements that saw different
	 * neighbourhoods. Identical seeds are what make "only the Strength differs" a claim about
	 * the override rather than about the wall.
	 */
	int32 SeedOneBrick(UDestructionStructureSubsystem& Subsystem)
	{
		using namespace DestructionProfiles;

		const int32 StructureId = Subsystem.BeginBuild();

		Subsystem.PlaceBuildPiece(
			StructureId, OverrideSeedCentreCm, OverrideHalfBrickCm, ClayBrick);

		return StructureId;
	}

	/**
	 * Where a piece forms no joint at all: 500 cm along, on the grounded course. Far enough that it
	 * is jointless for two independent reasons: Free placement honours the cursor verbatim and
	 * bonds to nothing beside it, and 500 cm is sixteen times the 30 cm snap radius from the seed
	 * brick — so the fixture cannot quietly become a one-joint case if either half ever changes.
	 */
	const FVector OverrideLonelyCentreCm(500.0, 0.0, OverrideCourseZeroBrickZCm);

	/** Open a build and lay a TWO-brick grounded course into it, both Auto. */
	int32 SeedTwoBrickCourse(UDestructionStructureSubsystem& Subsystem)
	{
		using namespace DestructionProfiles;

		const int32 StructureId = SeedOneBrick(Subsystem);

		Subsystem.PlaceBuildPiece(
			StructureId, OverrideSecondCursorCm, OverrideHalfBrickCm, ClayBrick);

		return StructureId;
	}
}

/**
 * UI-6, the world half — a joint-profile override rides through both build doors and replaces the
 * profile of EVERY joint the placed piece forms, changing nothing else about the placement.
 *
 * THE BEHAVIOUR IN ONE SENTENCE. `PreviewBuildPiece` and `PlaceBuildPiece` take an optional
 * `const FConnectionStrength*`; when non-null every joint the piece forms carries that profile
 * instead of the one `BuildMode::JointForContact` inferred, and when null the inference decides
 * exactly as it does today.
 *
 * WHY "EVERY JOINT" AND WHY A ONE-JOINT FIXTURE CANNOT MAKE THE CLAIM. A brick laid into a bond
 * forms one joint, and an override applied to `Joints[0]` alone would satisfy every brick case in
 * this file forever. A timber plate laid across a two-brick course forms two bearings from one
 * placement, and that is the fixture that tells "the override replaced the profile" from "the
 * override replaced the first profile" — the difference a player meets when they screw a plate
 * down and find one end of it resting dry.
 *
 * WHY THE GEOMETRY IS HELD AGAINST AN AUTO RUN RATHER THAN MERELY CHECKED. The override is a
 * substitution, not a different placement: same pose, same joint count, same pieces jointed, same
 * normals and areas. So each override case runs against a separate, identically seeded structure
 * and is compared joint by joint with the Auto run — which is what would catch an implementation
 * that reached the right profiles by re-solving with different materials, or by dropping a joint
 * it could not re-infer. Asserting the profile alone would let all of that through.
 *
 * The pose is also pinned, never a displacement: `PlaceBuildPiece` moves nothing, so the reading
 * is the box the binding recorded. DESIGN §4's rule bites here in its other direction too — an
 * override that severed a joint would leave two pieces resting exactly where they were.
 *
 * Needs a ticking world: a real world, because both doors spawn an `ABrickActor` — but it never
 * ticks, and nothing is ever solved. Every assertion is on the structure graph.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBuildJointOverrideRidesThroughPlacementTest,
	"DestructionGame.World.BuildMode.JointOverrideRidesThroughPlacement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBuildJointOverrideRidesThroughPlacementTest::RunTest(const FString& Parameters)
{
	using namespace BrickWorldTestSupport;
	using namespace BuildJointOverrideTestSupport;
	using namespace DestructionProfiles;
	using namespace DestructionSession;

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		TestWorld.End();
		return true;
	}

	UDestructionStructureSubsystem& Subsystem = *TestWorld.Subsystem;

	/* The overrides come from the toolbar's own map rather than being named here, because that is
	 * the wire under test: the player picks a chip, `JointOverrideFor` turns it into a pointer,
	 * and the pointer is what the door takes. Naming `&Screw` directly would test the door with a
	 * hand-made argument the UI has no way to produce. */
	const FConnectionStrength* const AutoOverride = JointOverrideFor(EJointChoice::Auto);
	const FConnectionStrength* const MortarOverride = JointOverrideFor(EJointChoice::Mortar);
	const FConnectionStrength* const DryOverride = JointOverrideFor(EJointChoice::Dry);
	const FConnectionStrength* const ScrewOverride = JointOverrideFor(EJointChoice::Screw);

	/* --- ONE: the AUTO baseline. A same-course brick beds into the WEAK perpend ------------- */

	FJointShape AutoHeadShape;

	{
		const int32 StructureId = SeedOneBrick(Subsystem);

		const FBuildPreview Preview = Subsystem.PreviewBuildPiece(
			StructureId, OverrideSecondCursorCm, OverrideHalfBrickCm, ClayBrick,
			EPlacementMode::Snap, AutoOverride);

		TestTrue(TEXT("fixture: the Auto preview of the same-course brick must be valid"),
			Preview.bValid);

		TestEqual(
			FString::Printf(
				TEXT("the Auto same-course placement forms exactly one head joint, the preview says %d"),
				Preview.JointCount),
			Preview.JointCount, 1);

		/* The ghost card's own reading, and it is an address: `FBuildPreview::JointProfile` names
		 * the library row the joints would carry, so the card can say "perpend" before the click.
		 * It has to be the shipped row's own address rather than a copy — the inferred profile is
		 * returned by value by JointForContact and lives inside the decision, so a pointer into
		 * that decision would dangle the moment the preview returned, a card reading a freed stack
		 * frame that prints something plausible for exactly as long as it takes to ship. */
		TestTrue(
			*FString::Printf(
				TEXT("with no override the preview must name the INFERRED profile, "
					 "GeneralPurposeMortarPerpend, by address — it named %s"),
				OverrideProfileName(Preview.JointProfile)),
			Preview.JointProfile == &GeneralPurposeMortarPerpend);

		const FPieceRef Ref = Subsystem.PlaceBuildPiece(
			StructureId, OverrideSecondCursorCm, OverrideHalfBrickCm, ClayBrick,
			EPlacementMode::Snap, AutoOverride);

		FStructureBinding* const Binding = Subsystem.Find(StructureId);

		if (Binding == nullptr || Ref.PieceIndex != 1 || Binding->GetStructure().NumConnections() != 1)
		{
			AddError(FString::Printf(
				TEXT("fixture: the Auto same-course brick must land as piece 1 with one joint; it "
					 "landed as %d with %d joints"),
				Ref.PieceIndex,
				Binding != nullptr ? Binding->GetStructure().NumConnections() : INDEX_NONE));

			TestWorld.End();
			return true;
		}

		const FVector CentreCm = Binding->GetBinding(1).Box.CentreCm;

		TestTrue(
			FString::Printf(
				TEXT("fixture: the cursor at x = 21 must be PULLED onto the bond at (22.5, 0, 3.25); "
					 "it landed at (%g, %g, %g)"),
				CentreCm.X, CentreCm.Y, CentreCm.Z),
			CentreCm.Equals(OverrideSameCourseCentreCm, KINDA_SMALL_NUMBER));

		const FConnection& Head = Binding->GetStructure().GetConnection(0);

		AutoHeadShape = ShapeOf(Head);

		/* An end face, so the normal is horizontal — this is what makes the joint a head joint and
		 * therefore what makes the perpend the right inference; if it ever read vertical the
		 * baseline below would be the strong bed mortar and every "the override changed it" claim
		 * would be comparing against the wrong thing. */
		TestEqual(
			FString::Printf(
				TEXT("fixture: a head joint's normal is horizontal, |X| should be 1, got %g"),
				FMath::Abs(Head.InterfaceNormal.X)),
			FMath::Abs(Head.InterfaceNormal.X), 1.0, 1.0e-9);

		TestEqual(
			FString::Printf(
				TEXT("fixture: the head face is 10.25 x 6.5 = 66.625 cm2, got %g"),
				Head.InterfaceAreaSqCm),
			Head.InterfaceAreaSqCm, OverrideHeadAreaSqCm, 1.0e-6);

		CheckOverrideProfile(
			*this,
			TEXT("AUTO baseline: the same-course joint is the inferred perpend: "),
			Head.Strength,
			GeneralPurposeMortarPerpend);
	}

	/* --- TWO: the same placement with MORTAR and with DRY, against that baseline ------------ */

	struct FBrickOverrideCase
	{
		const TCHAR* Description;
		const FConnectionStrength* Override;
		const FConnectionStrength* Expected;
	};

	const FBrickOverrideCase BrickCases[] = {
		{
			TEXT("MORTAR: the player asks for a full bed bond in a head joint the inference would "
				 "have weakened to a perpend — a wall that is stronger than a bonded one, which is "
				 "the whole reason the choice exists"),
			MortarOverride,
			&GeneralPurposeMortar,
		},
		{
			TEXT("DRY: the same two bricks laid with no bond at all, so the course holds by friction "
				 "and stands only while it is squeezed"),
			DryOverride,
			&DryStone,
		},
		{
			TEXT("SCREW: a fastener in a masonry head joint — physically odd and deliberately "
				 "allowed, because the override is the PLAYER's and the model must not quietly "
				 "second-guess which pairings are sensible"),
			ScrewOverride,
			&Screw,
		},
	};

	for (const FBrickOverrideCase& Case : BrickCases)
	{
		const int32 StructureId = SeedOneBrick(Subsystem);

		const FBuildPreview Preview = Subsystem.PreviewBuildPiece(
			StructureId, OverrideSecondCursorCm, OverrideHalfBrickCm, ClayBrick,
			EPlacementMode::Snap, Case.Override);

		TestEqual(
			FString::Printf(
				TEXT("%s: the override must not change the joint COUNT, the preview says %d against "
					 "the Auto run's 1"),
				Case.Description, Preview.JointCount),
			Preview.JointCount, 1);

		TestTrue(
			*FString::Printf(
				TEXT("%s: the preview must name the OVERRIDE, %s, by address — it named %s"),
				Case.Description, OverrideProfileName(Case.Expected),
				OverrideProfileName(Preview.JointProfile)),
			Preview.JointProfile == Case.Expected);

		Subsystem.PlaceBuildPiece(
			StructureId, OverrideSecondCursorCm, OverrideHalfBrickCm, ClayBrick,
			EPlacementMode::Snap, Case.Override);

		FStructureBinding* const Binding = Subsystem.Find(StructureId);

		if (Binding == nullptr || Binding->GetStructure().NumConnections() != 1)
		{
			AddError(FString::Printf(
				TEXT("%s: the overridden placement must still form exactly ONE joint, it formed %d"),
				Case.Description,
				Binding != nullptr ? Binding->GetStructure().NumConnections() : INDEX_NONE));

			continue;
		}

		const FConnection& Head = Binding->GetStructure().GetConnection(0);

		/* The same joint, and only the strength moved: an override is a substitution rather than a
		 * different placement, so the pieces jointed, the face area and the normal must all be
		 * bit for bit what the Auto run produced — what an implementation that re-solved with a
		 * substituted material (a plausible way to "get mortar") would fail. */
		TestTrue(
			*FString::Printf(
				TEXT("%s: the joint's GEOMETRY must be the Auto run's exactly — Auto gave %s, this "
					 "gave %s"),
				Case.Description, *DescribeShape(AutoHeadShape), *DescribeShape(ShapeOf(Head))),
			ShapesAreTheSameJoint(ShapeOf(Head), AutoHeadShape));

		TestTrue(
			FString::Printf(
				TEXT("%s: and the pose must still be the bond's (22.5, 0, 3.25)"), Case.Description),
			Binding->GetBinding(1).Box.CentreCm.Equals(
				OverrideSameCourseCentreCm, KINDA_SMALL_NUMBER));

		CheckOverrideProfile(
			*this,
			FString::Printf(TEXT("%s: the committed joint carries the override: "), Case.Description),
			Head.Strength,
			*Case.Expected);
	}

	/* --- THREE: EVERY joint, not the first — a plate bearing on two bricks ------------------- */

	TArray<FJointShape> AutoBearingShapes;

	{
		const int32 StructureId = SeedTwoBrickCourse(Subsystem);

		const FBuildPreview Preview = Subsystem.PreviewBuildPiece(
			StructureId, OverridePlateCentreCm, OverrideHalfPlateCm, Timber,
			EPlacementMode::Snap, AutoOverride);

		TestEqual(
			FString::Printf(
				TEXT("fixture: a 67.5 cm plate laid across a two-brick course bears on BOTH of them, "
					 "so the Auto preview must say 2 joints; it says %d"),
				Preview.JointCount),
			Preview.JointCount, 2);

		TestTrue(
			*FString::Printf(
				TEXT("with no override the plate's bearings are the inferred DryStone — timber is not "
					 "compression-dominant, so the inference hands out the weakest bearing it has. "
					 "The preview named %s"),
				OverrideProfileName(Preview.JointProfile)),
			Preview.JointProfile == &DryStone);

		Subsystem.PlaceBuildPiece(
			StructureId, OverridePlateCentreCm, OverrideHalfPlateCm, Timber,
			EPlacementMode::Snap, AutoOverride);

		FStructureBinding* const Binding = Subsystem.Find(StructureId);

		/* One head joint from the seeded course, then the plate's two bearings. */
		if (Binding == nullptr || Binding->GetStructure().NumConnections() != 3)
		{
			AddError(FString::Printf(
				TEXT("fixture: the seeded course plus the Auto plate must give 3 joints (one head, "
					 "two bearings); it gave %d"),
				Binding != nullptr ? Binding->GetStructure().NumConnections() : INDEX_NONE));

			TestWorld.End();
			return true;
		}

		const FVector PlateCentreCm = Binding->GetBinding(2).Box.CentreCm;

		TestTrue(
			FString::Printf(
				TEXT("fixture: the plate must bear one joint above the course, at (0, 0, 12.5); it is "
					 "at (%g, %g, %g)"),
				PlateCentreCm.X, PlateCentreCm.Y, PlateCentreCm.Z),
			PlateCentreCm.Equals(OverridePlateCentreCm, KINDA_SMALL_NUMBER));

		for (int32 Index = 1; Index < 3; ++Index)
		{
			const FConnection& Bearing = Binding->GetStructure().GetConnection(Index);

			AutoBearingShapes.Add(ShapeOf(Bearing));

			TestEqual(
				FString::Printf(
					TEXT("fixture: bearing %d is a horizontal face, so |normal.Z| should be 1, got %g"),
					Index, FMath::Abs(Bearing.InterfaceNormal.Z)),
				FMath::Abs(Bearing.InterfaceNormal.Z), 1.0, 1.0e-9);

			TestEqual(
				FString::Printf(
					TEXT("fixture: bearing %d covers the whole brick, 21.5 x 10.25 = 220.375 cm2, got "
						 "%g"),
					Index, Bearing.InterfaceAreaSqCm),
				Bearing.InterfaceAreaSqCm, OverrideBearingAreaSqCm, 1.0e-6);

			CheckOverrideProfile(
				*this,
				FString::Printf(TEXT("AUTO baseline: bearing %d is the inferred DryStone: "), Index),
				Bearing.Strength,
				DryStone);
		}
	}

	/* --- FOUR: the same plate, SCREWED — and BOTH bearings must be screws -------------------- */

	{
		const int32 StructureId = SeedTwoBrickCourse(Subsystem);

		const FBuildPreview Preview = Subsystem.PreviewBuildPiece(
			StructureId, OverridePlateCentreCm, OverrideHalfPlateCm, Timber,
			EPlacementMode::Snap, ScrewOverride);

		TestEqual(
			FString::Printf(
				TEXT("the screwed plate must form the SAME two bearings, not fewer; the preview says "
					 "%d"),
				Preview.JointCount),
			Preview.JointCount, 2);

		TestTrue(
			*FString::Printf(
				TEXT("and the preview must name Screw by address, it named %s"),
				OverrideProfileName(Preview.JointProfile)),
			Preview.JointProfile == ScrewOverride);

		Subsystem.PlaceBuildPiece(
			StructureId, OverridePlateCentreCm, OverrideHalfPlateCm, Timber,
			EPlacementMode::Snap, ScrewOverride);

		FStructureBinding* const Binding = Subsystem.Find(StructureId);

		if (Binding == nullptr || Binding->GetStructure().NumConnections() != 3)
		{
			AddError(FString::Printf(
				TEXT("the screwed plate must leave the structure with 3 joints exactly as the Auto "
					 "one did; it has %d"),
				Binding != nullptr ? Binding->GetStructure().NumConnections() : INDEX_NONE));

			TestWorld.End();
			return true;
		}

		/* The course's own head joint is untouched — the other half of "the override is the next
		 * placement's". It was laid under Auto and must still be the perpend: an override that
		 * re-priced joints already in the structure would let a player change a wall they finished
		 * an hour ago by picking a chip, with nothing on screen to say it had happened. */
		CheckOverrideProfile(
			*this,
			TEXT("the course's existing head joint is NOT re-priced by a later override: "),
			Binding->GetStructure().GetConnection(0).Strength,
			GeneralPurposeMortarPerpend);

		for (int32 Index = 1; Index < 3; ++Index)
		{
			const FConnection& Bearing = Binding->GetStructure().GetConnection(Index);

			/* Both bearings, and the second one is the test: an override written onto Joints[0]
			 * passes every brick case in this file and leaves the far end of the plate resting
			 * dry — a plate the player screwed down that is held at one end by friction. */
			CheckOverrideProfile(
				*this,
				FString::Printf(
					TEXT("SCREW: bearing %d of 2 carries the override: "), Index),
				Bearing.Strength,
				Screw);

			const bool bGeometryMatches = AutoBearingShapes.IsValidIndex(Index - 1)
				&& ShapesAreTheSameJoint(ShapeOf(Bearing), AutoBearingShapes[Index - 1]);

			TestTrue(
				*FString::Printf(
					TEXT("SCREW: bearing %d must be the SAME joint the Auto run formed — Auto gave "
						 "%s, this gave %s"),
					Index,
					AutoBearingShapes.IsValidIndex(Index - 1)
						? *DescribeShape(AutoBearingShapes[Index - 1])
						: TEXT("<no Auto bearing>"),
					*DescribeShape(ShapeOf(Bearing))),
				bGeometryMatches);
		}
	}

	/* --- FIVE: a placement that forms NO joint names NO profile, override or not ------------- */

	/*
	 * A chosen chip is not a joint. `FBuildPreview::JointProfile`'s own header says the reading is
	 * "the override when one was given, the row the first joint's inferred profile matches when it
	 * was not, and null when the placement forms no joint at all" — and the third arm is the one a
	 * player meets constantly, since the first brick of every build and every Free placement over
	 * empty ground is jointless.
	 *
	 * Why a ghost card reading "Screw" over a brick bonded to nothing is a defect: the card exists
	 * to answer "what will this click build", and the answer here is "a piece held by nothing" —
	 * the single most important thing the player can be told before they commit, since a jointless
	 * piece is one Run away from lying on the ground. A row naming the fastener they picked says
	 * the opposite, that the click will screw the piece to something. The override is what the
	 * joints would carry; where there are none, there is nothing for it to answer.
	 *
	 * Both ways round, and the null-override row is the control: Auto over empty ground already
	 * reads null today (no first joint to look up), so a "fix" that nulled the reading
	 * unconditionally would be indistinguishable from the right one.
	 */
	{
		struct FLonelyCase
		{
			const TCHAR* Description;
			const FConnectionStrength* Override;
		};

		const FLonelyCase LonelyCases[] = {
			{
				TEXT("SCREW over empty ground: the player has chosen a fastener and there is nothing "
					 "within reach to fasten to"),
				ScrewOverride,
			},
			{
				TEXT("AUTO over empty ground — the control, which already reads null, so a reading "
					 "nulled unconditionally cannot pass for the fix"),
				AutoOverride,
			},
		};

		for (const FLonelyCase& Case : LonelyCases)
		{
			const int32 StructureId = SeedOneBrick(Subsystem);

			const FBuildPreview Preview = Subsystem.PreviewBuildPiece(
				StructureId, OverrideLonelyCentreCm, OverrideHalfBrickCm, ClayBrick,
				EPlacementMode::Free, Case.Override);

			TestTrue(
				*FString::Printf(
					TEXT("fixture: %s — Free honours the cursor verbatim, so the preview must be valid"),
					Case.Description),
				Preview.bValid);

			TestEqual(
				FString::Printf(
					TEXT("fixture: %s — the piece must form NO joint, or there is nothing to claim; the "
						 "preview says %d"),
					Case.Description, Preview.JointCount),
				Preview.JointCount, 0);

			TestTrue(
				*FString::Printf(
					TEXT("%s: A JOINTLESS PLACEMENT MUST NAME NO PROFILE. The ghost card's job is to say "
						 "what this click builds, and what it builds here is a piece held by NOTHING — a "
						 "card reading '%s' tells the player the opposite, over the one placement that "
						 "is one Run away from lying on the ground. It named %s"),
					Case.Description, OverrideProfileName(Case.Override),
					OverrideProfileName(Preview.JointProfile)),
				Preview.JointProfile == nullptr);

			/* And the commit agrees: the piece lands, bonded to nothing. Without this the claim
			 * above could be satisfied by a preview that refused the pose outright — a different
			 * behaviour, and one the Free escape exists to forbid. */
			const FPieceRef Ref = Subsystem.PlaceBuildPiece(
				StructureId, OverrideLonelyCentreCm, OverrideHalfBrickCm, ClayBrick,
				EPlacementMode::Free, Case.Override);

			FStructureBinding* const Binding = Subsystem.Find(StructureId);

			TestTrue(
				*FString::Printf(
					TEXT("%s: the commit must still LAY the piece — Free is the deliberate escape — and "
						 "form no joint; it landed as piece %d with %d joint(s)"),
					Case.Description, Ref.PieceIndex,
					Binding != nullptr ? Binding->GetStructure().NumConnections() : INDEX_NONE),
				Binding != nullptr && Ref.PieceIndex == 1
					&& Binding->GetStructure().NumConnections() == 0);
		}
	}

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
