// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "DestructionGameGameMode.h"
#include "HAL/PlatformTime.h"
#include "Tests/BrickWorldTestSupport.h"

#if WITH_DEV_AUTOMATION_TESTS

// Named, not anonymous: unity builds merge translation units. The harness is in BrickWorldTestSupport.h.
namespace BrickWallScenarioTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/**
	 * The wall the game mode builds on Play: 30 across, 40 courses, flush (so two piece sizes).
	 * Its size also measures whether the solver's O(pieces x connections) cost is affordable.
	 */
	inline FRunningBondSpec ScenarioWallSpec()
	{
		FRunningBondSpec Spec;
		Spec.BrickSizeCm = FVector(21.5, 10.25, 6.5);
		Spec.JointThicknessCm = 1.0;
		Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
		Spec.CoursesHigh = 40;
		Spec.BricksPerCourse = 30;
		Spec.End = EWallEnd::Flush;
		Spec.Strength = GeneralPurposeMortar;
		return Spec;
	}

	/**
	 * Counts by hand. Pieces: 20 even courses of 30 plus 20 odd courses of 31 (half bat each
	 * end) = 1,220. Joints: heads 20 x 29 + 20 x 30 = 1,180; beds 60 per course transition
	 * x 39 = 2,340; total 3,520.
	 */
	constexpr int32 ScenarioPieceCount = 1220;
	constexpr int32 ScenarioJointCount = 3520;

	/**
	 * The wall's extent, cm: X from -10.75 to 29 x 22.5 + 10.75 = 663.25; top at
	 * 3.25 + 39 x 7.5 + 3.25 = 299. Catches the right count laid in the wrong shape.
	 */
	constexpr double ScenarioWallMinXCm = -10.75;
	constexpr double ScenarioWallMaxXCm = 663.25;
	constexpr double ScenarioWallTopZCm = 299.0;

	const TCHAR* ScenarioSupportName(EPieceSupport Support)
	{
		switch (Support)
		{
		case EPieceSupport::Grounded:  return TEXT("Grounded");
		case EPieceSupport::Supported: return TEXT("Supported");
		case EPieceSupport::Stranded:  return TEXT("Stranded");
		default:                       return TEXT("Falling");
		}
	}

	/** Caps per-piece error output. */
	constexpr int32 ScenarioMaxReportedFailures = 8;

	/**
	 * Every piece is held up (mechanism) and none is released (outcome); the two can disagree.
	 * Errors name Stranded vs Falling, since Stranded means an unroutable knot, not a collapse.
	 */
	inline void CheckWallIsStanding(
		FAutomationTestBase& Test,
		FStructureBinding& Binding,
		const TCHAR* When)
	{
		int32 NotHeldUp = 0;
		int32 Released = 0;

		for (int32 Piece = 0; Piece < Binding.NumPieces(); ++Piece)
		{
			const EPieceSupport Support = Binding.GetStructure().GetPieceSupport(Piece);

			if (Support != EPieceSupport::Grounded && Support != EPieceSupport::Supported)
			{
				++NotHeldUp;

				if (NotHeldUp <= ScenarioMaxReportedFailures)
				{
					Test.AddError(FString::Printf(
						TEXT("%s: piece %d of a wall under nothing but its own weight reads %s"),
						When, Piece, ScenarioSupportName(Support)));
				}
			}

			if (Binding.IsReleased(Piece))
			{
				++Released;
			}
		}

		Test.TestEqual(
			FString::Printf(TEXT("%s: no piece of an untouched wall should have lost its support; %d did"),
				When, NotHeldUp),
			NotHeldUp, 0);

		Test.TestEqual(
			FString::Printf(TEXT("%s: no piece of an untouched wall should have been handed to physics; %d was"),
				When, Released),
			Released, 0);
	}
}

/**
 * A 30 x 40 wall lays, spawns, solves and stands, with each stage's time reported (not
 * asserted: wall-clock budgets flake). A scale regression net and cost measurement for the
 * O(pieces x connections) solve every click pays. Needs a world to spawn into; never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBrickWallAtScenarioScaleTest,
	"DestructionGame.World.Scenario.WallAtScenarioScale",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBrickWallAtScenarioScaleTest::RunTest(const FString& Parameters)
{
	using namespace BrickWallScenarioTestSupport;
	using namespace BrickWorldTestSupport;
	using namespace DestructionLayout;

	const FRunningBondSpec Spec = ScenarioWallSpec();

	// Time the world-free layout separately from spawning; they have different fixes.
	FBrickLayout Reference;

	const double LayoutStart = FPlatformTime::Seconds();
	const bool bLaid = RunningBond(Spec, Reference);
	const double LayoutSeconds = FPlatformTime::Seconds() - LayoutStart;

	TestTrue(TEXT("RunningBond should lay a 30-wide, 40-course wall"), bLaid);

	AddInfo(FString::Printf(
		TEXT("MEASURED: RunningBond laid %d pieces and %d joints in %.1f ms"),
		Reference.Structure.NumPieces(), Reference.Structure.NumConnections(),
		LayoutSeconds * 1000.0));

	TestEqual(
		FString::Printf(TEXT("a flush 30 x 40 wall should be %d pieces, got %d"),
			ScenarioPieceCount, Reference.Structure.NumPieces()),
		Reference.Structure.NumPieces(), ScenarioPieceCount);

	TestEqual(
		FString::Printf(TEXT("a flush 30 x 40 wall should carry %d joints, got %d"),
			ScenarioJointCount, Reference.Structure.NumConnections()),
		Reference.Structure.NumConnections(), ScenarioJointCount);

	// The right shape, not just the right count.
	{
		FBox WallBoundsCm(ForceInit);

		for (const FPieceBox& Box : Reference.Boxes)
		{
			WallBoundsCm += FBox(Box.CentreCm - Box.ExtentCm, Box.CentreCm + Box.ExtentCm);
		}

		AddInfo(FString::Printf(
			TEXT("the scenario wall spans X %g..%g, Y %g..%g, Z %g..%g cm"),
			WallBoundsCm.Min.X, WallBoundsCm.Max.X,
			WallBoundsCm.Min.Y, WallBoundsCm.Max.Y,
			WallBoundsCm.Min.Z, WallBoundsCm.Max.Z));

		TestTrue(
			FString::Printf(TEXT("the wall should run X %g..%g, it runs %g..%g"),
				ScenarioWallMinXCm, ScenarioWallMaxXCm, WallBoundsCm.Min.X, WallBoundsCm.Max.X),
			FMath::IsNearlyEqual(WallBoundsCm.Min.X, ScenarioWallMinXCm, BoundsToleranceCm)
			&& FMath::IsNearlyEqual(WallBoundsCm.Max.X, ScenarioWallMaxXCm, BoundsToleranceCm));

		TestTrue(
			FString::Printf(TEXT("40 courses should reach Z %g, they reach %g"),
				ScenarioWallTopZCm, WallBoundsCm.Max.Z),
			FMath::IsNearlyEqual(WallBoundsCm.Max.Z, ScenarioWallTopZCm, BoundsToleranceCm));
	}

	FBrickTestWorld TestWorld;

	if (!TestWorld.Begin(*this))
	{
		return true;
	}

	// The full build the scenario runs: lay, spawn and adopt.
	const double BuildStart = FPlatformTime::Seconds();
	const int32 StructureId = TestWorld.Subsystem->BuildRunningBond(Spec);
	const double BuildSeconds = FPlatformTime::Seconds() - BuildStart;

	AddInfo(FString::Printf(
		TEXT("MEASURED: BuildRunningBond (lay + spawn %d actors + adopt) took %.1f ms"),
		ScenarioPieceCount, BuildSeconds * 1000.0));

	FStructureBinding* Binding = TestWorld.Subsystem->Find(StructureId);

	TestNotNull(
		*FString::Printf(TEXT("BuildRunningBond returned %d and Find should hand back its binding"),
			StructureId),
		Binding);

	if (Binding == nullptr)
	{
		TestWorld.End();
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("the built wall should carry %d pieces, got %d"),
			ScenarioPieceCount, Binding->NumPieces()),
		Binding->NumPieces(), ScenarioPieceCount);

	// Every piece has a valid ABrickActor (the count alone is guaranteed by AdoptLayout).
	{
		int32 MissingBricks = 0;

		for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
		{
			const ABrickActor* Brick = Cast<ABrickActor>(Binding->GetActor(Piece));

			if (!IsValid(Brick))
			{
				++MissingBricks;

				if (MissingBricks <= ScenarioMaxReportedFailures)
				{
					AddError(FString::Printf(TEXT("piece %d has no live ABrickActor standing for it"), Piece));
				}
			}
		}

		TestEqual(
			FString::Printf(TEXT("every one of the %d pieces should have a live brick; %d do not"),
				ScenarioPieceCount, MissingBricks),
			MissingBricks, 0);
	}

	// The solve alone: what every click pays.
	const double SolveStart = FPlatformTime::Seconds();
	Binding->SolveLoads();
	const double SolveSeconds = FPlatformTime::Seconds() - SolveStart;

	AddInfo(FString::Printf(
		TEXT("MEASURED: SolveLoads over %d pieces and %d joints took %.1f ms"),
		ScenarioPieceCount, ScenarioJointCount, SolveSeconds * 1000.0));

	CheckWallIsStanding(*this, *Binding, TEXT("as built and solved"));

	// No joint near capacity; the worst is reported as headroom under self-weight.
	{
		double WorstUtilisation = 0.0;
		int32 WorstJoint = INDEX_NONE;

		for (int32 Joint = 0; Joint < Binding->GetStructure().NumConnections(); ++Joint)
		{
			const double Utilisation = Binding->GetStructure().GetConnectionUtilisation(Joint);

			if (Utilisation > WorstUtilisation)
			{
				WorstUtilisation = Utilisation;
				WorstJoint = Joint;
			}
		}

		AddInfo(FString::Printf(
			TEXT("the worst-loaded joint of the scenario wall is joint %d at %.6g of its capacity"),
			WorstJoint, WorstUtilisation));

		TestTrue(
			FString::Printf(
				TEXT("a wall standing under its own weight should have no joint over capacity; joint %d is at %.6g"),
				WorstJoint, WorstUtilisation),
			WorstUtilisation < 1.0);
	}

	// Pushed twice: SolveAndPush re-solves rather than caching, so both should cost about the same.
	const double FirstPushStart = FPlatformTime::Seconds();
	const int32 FirstReleased = TestWorld.Subsystem->SolveAndPush(StructureId);
	const double FirstPushSeconds = FPlatformTime::Seconds() - FirstPushStart;

	const double SecondPushStart = FPlatformTime::Seconds();
	const int32 SecondReleased = TestWorld.Subsystem->SolveAndPush(StructureId);
	const double SecondPushSeconds = FPlatformTime::Seconds() - SecondPushStart;

	AddInfo(FString::Printf(
		TEXT("MEASURED: SolveAndPush took %.1f ms, and again %.1f ms — this is the per-click cost"),
		FirstPushSeconds * 1000.0, SecondPushSeconds * 1000.0));

	TestEqual(
		FString::Printf(TEXT("pushing a standing wall should release nothing, it released %d"), FirstReleased),
		FirstReleased, 0);

	TestEqual(
		FString::Printf(TEXT("pushing it again should release nothing, it released %d"), SecondReleased),
		SecondReleased, 0);

	CheckWallIsStanding(*this, *Binding, TEXT("after two pushes"));

	AddInfo(FString::Printf(
		TEXT("MEASURED TOTAL: lay %.1f ms + build %.1f ms + solve %.1f ms + push %.1f ms"),
		LayoutSeconds * 1000.0, BuildSeconds * 1000.0,
		SolveSeconds * 1000.0, FirstPushSeconds * 1000.0));

	TestWorld.End();

	return true;
}

/**
 * Pressing Play builds the wall: ADestructionGameGameMode (named explicitly) builds a 30 x 40
 * structure on begin-play that the subsystem owns, with the predicted counts and a live brick
 * per piece, and it stands through a push. Visibility is not asserted (needs a viewport).
 * Needs begin-play; never ticks.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FGameModeBuildsTheWallTest,
	"DestructionGame.World.Scenario.GameModeBuildsTheWallOnBeginPlay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FGameModeBuildsTheWallTest::RunTest(const FString& Parameters)
{
	using namespace BrickWallScenarioTestSupport;
	using namespace BrickWorldTestSupport;

	FBrickTestWorld TestWorld;

	const double BeginStart = FPlatformTime::Seconds();
	const bool bBegun = TestWorld.Begin(*this, ADestructionGameGameMode::StaticClass());
	const double BeginSeconds = FPlatformTime::Seconds() - BeginStart;

	AddInfo(FString::Printf(
		TEXT("MEASURED: creating the world and running begin-play with the real game mode took %.1f ms"),
		BeginSeconds * 1000.0));

	if (!bBegun)
	{
		return true;
	}

	ADestructionGameGameMode* const GameMode =
		TestWorld.World->GetAuthGameMode<ADestructionGameGameMode>();

	TestNotNull(
		*FString::Printf(TEXT("fixture: the world should be running ADestructionGameGameMode, it is running %s"),
			*GetNameSafe(TestWorld.World->GetAuthGameMode())),
		GameMode);

	if (GameMode == nullptr)
	{
		TestWorld.End();
		return true;
	}

	// The subsystem must own it, or TracePiece cannot resolve a click on it.
	const int32 StructureId = GameMode->GetBuiltStructureId();

	TestTrue(
		FString::Printf(
			TEXT("the game mode should have built a structure when play began; GetBuiltStructureId returned %d"),
			StructureId),
		StructureId != INDEX_NONE);

	FStructureBinding* Binding = TestWorld.Subsystem->Find(StructureId);

	TestNotNull(
		*FString::Printf(TEXT("the subsystem should hold the structure the game mode built (id %d)"),
			StructureId),
		Binding);

	if (Binding == nullptr)
	{
		TestWorld.End();
		return true;
	}

	TestEqual(
		FString::Printf(TEXT("the wall on Play should be a flush 30 x 40 running bond, %d pieces; it has %d"),
			ScenarioPieceCount, Binding->NumPieces()),
		Binding->NumPieces(), ScenarioPieceCount);

	TestEqual(
		FString::Printf(TEXT("that wall should carry %d joints, it carries %d"),
			ScenarioJointCount, Binding->GetStructure().NumConnections()),
		Binding->GetStructure().NumConnections(), ScenarioJointCount);

	{
		int32 MissingBricks = 0;

		for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
		{
			const ABrickActor* Brick = Cast<ABrickActor>(Binding->GetActor(Piece));

			if (!IsValid(Brick))
			{
				++MissingBricks;

				if (MissingBricks <= ScenarioMaxReportedFailures)
				{
					AddError(FString::Printf(
						TEXT("piece %d of the wall built on Play has no live ABrickActor standing for it"),
						Piece));
				}
			}
		}

		TestEqual(
			FString::Printf(TEXT("every piece of the wall built on Play should have a live brick; %d do not"),
				MissingBricks),
			MissingBricks, 0);
	}

	// An unsolved wall also releases nothing, so the push is what makes "standing" meaningful.
	CheckWallIsStanding(*this, *Binding, TEXT("as the game mode left it"));

	const double PushStart = FPlatformTime::Seconds();
	const int32 Released = TestWorld.Subsystem->SolveAndPush(StructureId);
	const double PushSeconds = FPlatformTime::Seconds() - PushStart;

	AddInfo(FString::Printf(
		TEXT("MEASURED: SolveAndPush on the wall the game mode built took %.1f ms"),
		PushSeconds * 1000.0));

	TestEqual(
		FString::Printf(TEXT("the wall on Play should stand: pushing it must release nothing, it released %d"),
			Released),
		Released, 0);

	CheckWallIsStanding(*this, *Binding, TEXT("after a push"));

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
