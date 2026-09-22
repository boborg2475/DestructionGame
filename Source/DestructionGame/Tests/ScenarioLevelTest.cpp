// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "DestructionGameFlyingPawn.h"
#include "DestructionGameGameMode.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Tests/BrickWorldTestSupport.h"
#include "UObject/WeakObjectPtr.h"
#include "World/DestructionScenarios.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Pressing Play on a scenario: the game mode builds it, puts the player in front of it, and cuts it
 * while they watch. The point of a scenario level, as opposed to the headless fixture it is made of,
 * is that a human can watch it:
 *
 *   - The wall is in front of the player the instant they join. Asserted on the pawn's real
 *     transform and the controller's real control rotation, never a stored intent, since a game
 *     mode that stashed a perfect viewpoint in a field would satisfy that and show the player the
 *     inside of a brick. Asserted twice: the exact place the framing arithmetic names, and the
 *     property that every corner of the structure is inside the frustum from where the pawn landed.
 *
 *   - The cut happens after a delay, not at begin-play. This needs a before as well as an after: a
 *     test checking only the end state would pass against a game mode that deleted the brick on the
 *     frame it built the wall — the bug, since a player who joins to a hole has watched nothing.
 *
 * Displacement is never read as evidence (DESIGN.md §4): the assertions are the mechanism — the
 * piece is tombstoned, its actor destroyed, the live count down by one, the solver has an answer for
 * every piece left, and the release count says the wall stood.
 *
 * Selection in a code-built world: UWorld::GetMapName() answers with the test world's own package
 * name, so the map branch is unreachable here (pinned separately by World.Scenarios.Selection). The
 * option branch runs the real wire: BeginPlayURL puts ?Scenario=free-end-40 on the URL,
 * InitializeActorsForPlay turns that into the OptionsString InitGame receives.
 *
 * The pawn is spawned before begin-play because UEngine::LoadMap spawns each local player's
 * controller and pawn between InitializeActorsForPlay and BeginPlay, so a real begin-play runs with
 * a possessed pawn present. The BeforeBeginPlay hook is that gap. It is spawned somewhere absurd
 * (five metres under the floor, fifty off) so "it was moved" is never "it happened to be there". One
 * test has no pawn at all, since a level with nobody in it yet must still build and cut.
 *
 * No ticking world except where the delay is measured in world seconds; the framing test runs
 * begin-play and never ticks.
 */
namespace ScenarioLevelTestSupport
{
	using namespace DestructionLayout;

	/** The row the option selects, and the row the default gives — named, never indexed. */
	const TCHAR* const ScenarioLevelCutRowName = TEXT("free-end-40");
	const TCHAR* const ScenarioLevelDefaultRowName = TEXT("sandbox");

	/** The level that lays nothing, because the player lays it. */
	const TCHAR* const ScenarioLevelBuildRowName = TEXT("build");

	/**
	 * How near the origin the player must stand on an empty plot, cm. Twenty metres is a ceiling, not
	 * a target: the exact standoff is ViewpointFor's, pinned by World.Scenarios.Viewpoint, and
	 * pinning it again here would only break when the framing is retuned. What must hold is that the
	 * player stands on the plot, not half a kilometre away or where the harness spawned them.
	 */
	constexpr double ScenarioLevelBuildPlotReachCm = 2000.0;

	/**
	 * How nearly the view must point at the plot — the cosine of the angle between the camera's look
	 * and the origin. 0.9 is about twenty-five degrees, loose on purpose: the game mode may aim at
	 * the centre of whatever default box it frames. What it may not do is look past it, which a
	 * camera carrying the previous scenario's yaw, or an unset rotation, would.
	 */
	constexpr double ScenarioLevelBuildAimDot = 0.9;

	/**
	 * The aspect a level frames for when nothing can tell it the viewport's. Every run is -nullrhi
	 * over a code-built world, so there is no UGameViewportClient to ask. 16:9 is the fallback,
	 * stated as a requirement: 1080 high per 1920 wide.
	 */
	constexpr double ScenarioLevelAspectHeightOverWidth = 1080.0 / 1920.0;

	/*
	 * What the two walls measure, derived by hand from the grid. Both are flush running bonds on the
	 * 22.5 x 11.25 x 7.5 grid, so a course runs from X -10.75 to (N-1)*22.5 + 10.75, the top face is
	 * at Z 299, and the wall is one 10.25 cm brick centred on Y zero. Pieces: even courses are N full
	 * bricks, odd courses N+1 (a half bat at each end), 20 each = 40N + 20 — 300 at 7 wide, 1,220 at 30.
	 */
	constexpr double ScenarioLevelWallMinXCm = -10.75;
	constexpr double ScenarioLevelWallMinZCm = 0.0;
	constexpr double ScenarioLevelWallMaxZCm = 299.0;
	constexpr double ScenarioLevelWallHalfYCm = 5.125;

	/** 7 wide: 6 x 22.5 + 10.75, and 40 x 7 + 20. */
	constexpr double ScenarioLevelCutWallMaxXCm = 145.75;
	constexpr int32 ScenarioLevelCutWallPieceCount = 300;

	/** 30 wide: 29 x 22.5 + 10.75, and 40 x 30 + 20. */
	constexpr double ScenarioLevelDefaultWallMaxXCm = 663.25;
	constexpr int32 ScenarioLevelDefaultWallPieceCount = 1220;

	/**
	 * Where the player must end up, worked through here rather than asked of ViewpointFor. A 90-degree
	 * horizontal FOV makes the visible half-width at standoff s equal to s and the half-height s*aspect,
	 * so framing a box needs standoff = max(120, 1.25 * max(halfX, halfZ/aspect)), the 1.25 keeping the
	 * structure off the edges.
	 *
	 * The two walls pick different terms, the point of checking both. The 7-wide wall (156.5 across,
	 * 299 tall) has halfZ/aspect = 265.78 beat half-width, giving standoff 332.2222222222222 — height
	 * governs. The 30-wide wall (674 across) has 1.25 * 337 = 421.25 beat it — width governs. A game
	 * mode framing on one extent only would put one wall off the screen.
	 *
	 * The camera stands on the +Y side of the wall's centre at that standoff, level, looking along -Y
	 * with +X to the right — the way every design elevation is drawn, so a level reads the same.
	 */
	constexpr double ScenarioLevelCutWallStandoffCm = 332.2222222222222;
	constexpr double ScenarioLevelDefaultWallStandoffCm = 421.25;
	constexpr double ScenarioLevelCameraYawDegrees = -90.0;

	/** Bounds and placements are exact arithmetic on boxes, so this is slack rather than signal. */
	constexpr double ScenarioLevelPlacementToleranceCm = 0.01;

	/** A yaw is stored as a float somewhere along the way; a thousandth of a degree is slack. */
	constexpr double ScenarioLevelRotationToleranceDegrees = 1.0e-3;

	/** Where the pawn is put before begin-play: nowhere anything would legitimately frame it. */
	const FVector ScenarioLevelPawnStartsAtCm = FVector(-5000.0, -5000.0, -500.0);

	/** The brick free-end-40 cuts: the outermost full brick of the grounded course. */
	const FVector ScenarioLevelCutCentreCm = FVector(0.0, 0.0, 3.25);

	/** How near a centre has to be to name a brick — see DestructionScenarios.cpp's own note. */
	constexpr double ScenarioLevelCutMatchToleranceCm = 1.0e-6;

	inline FString ScenarioLevelBits(double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	inline FString ScenarioLevelVectorBits(const FVector& Value)
	{
		return FString::Printf(TEXT("(%.17g, %.17g, %.17g)"), Value.X, Value.Y, Value.Z);
	}

	/** The row with this name, or null with the reason reported. */
	inline const DestructionScenarios::FScenario* ScenarioLevelRowNamed(
		FAutomationTestBase& Test, const TCHAR* Name)
	{
		const int32 Index = DestructionScenarios::IndexOfName(FName(Name));

		if (!DestructionScenarios::Catalogue().IsValidIndex(Index))
		{
			Test.AddError(FString::Printf(
				TEXT("fixture: the catalogue must carry a row named '%s'"), Name));

			return nullptr;
		}

		return &DestructionScenarios::Catalogue()[Index];
	}

	/** Every live piece's box, unioned: the structure a human is being shown. */
	inline FBox ScenarioLevelBoundsOf(const FStructureBinding& Binding)
	{
		FBox BoundsCm(ForceInit);

		for (int32 Piece = 0; Piece < Binding.NumPieces(); ++Piece)
		{
			if (Binding.IsPieceRemoved(Piece))
			{
				continue;
			}

			const FPieceBox& Box = Binding.GetBinding(Piece).Box;

			BoundsCm += FBox(Box.CentreCm - Box.ExtentCm, Box.CentreCm + Box.ExtentCm);
		}

		return BoundsCm;
	}

	/**
	 * Is all of it on screen from here — the property, checked corner by corner. Separate from
	 * placement: that pins where the arithmetic says to stand, this pins what a human means by seeing
	 * the thing, and it survives a retuned margin. Written against the camera's own axes, not an
	 * assumed -Y, so a viewpoint facing from the wrong side or mirrored fails here. A 90-degree
	 * horizontal FOV puts the frustum half-width at the corner's depth and its half-height at that
	 * depth times aspect; every corner must be in front and inside both.
	 */
	inline bool ScenarioLevelIsWhollyInFrame(
		const FVector& CameraCm,
		const FRotator& Rotation,
		double AspectHeightOverWidth,
		const FBox& BoundsCm,
		FString& OutWhyNot)
	{
		const FRotationMatrix Frame(Rotation);

		const FVector Forward = Frame.GetScaledAxis(EAxis::X);
		const FVector Right = Frame.GetScaledAxis(EAxis::Y);
		const FVector Up = Frame.GetScaledAxis(EAxis::Z);

		for (int32 Corner = 0; Corner < 8; ++Corner)
		{
			const FVector Point(
				(Corner & 1) ? BoundsCm.Max.X : BoundsCm.Min.X,
				(Corner & 2) ? BoundsCm.Max.Y : BoundsCm.Min.Y,
				(Corner & 4) ? BoundsCm.Max.Z : BoundsCm.Min.Z);

			const FVector Offset = Point - CameraCm;

			const double DepthCm = FVector::DotProduct(Offset, Forward);
			const double AcrossCm = FVector::DotProduct(Offset, Right);
			const double UpCm = FVector::DotProduct(Offset, Up);

			if (!(DepthCm > 0.0) || !(FMath::Abs(AcrossCm) <= DepthCm)
				|| !(FMath::Abs(UpCm) <= DepthCm * AspectHeightOverWidth))
			{
				OutWhyNot = FString::Printf(
					TEXT("corner %s is %s cm in front of the camera, %s cm across and %s cm up, ")
					TEXT("against a frustum %s cm half-wide and %s cm half-high there"),
					*ScenarioLevelVectorBits(Point), *ScenarioLevelBits(DepthCm),
					*ScenarioLevelBits(AcrossCm), *ScenarioLevelBits(UpCm),
					*ScenarioLevelBits(DepthCm),
					*ScenarioLevelBits(DepthCm * AspectHeightOverWidth));

				return false;
			}
		}

		OutWhyNot.Reset();

		return true;
	}

	/**
	 * The piece whose box is centred here, live or not, or INDEX_NONE. Removed pieces are
	 * deliberately included: FPieceBinding::Box is kept on removal so where a brick was stays
	 * answerable, and this is the lookup that must find a brick a game mode cut too early. Skipping
	 * tombstones would turn "it cut at begin-play" into a fixture error saying the wall lacks the
	 * brick, reading like a broken test rather than a broken level.
	 */
	inline int32 ScenarioLevelPieceAtCentre(
		const FStructureBinding& Binding, const FVector& CentreCm)
	{
		for (int32 Piece = 0; Piece < Binding.NumPieces(); ++Piece)
		{
			if (Binding.GetBinding(Piece).Box.CentreCm.Equals(
					CentreCm, ScenarioLevelCutMatchToleranceCm))
			{
				return Piece;
			}
		}

		return INDEX_NONE;
	}

	/** How many live pieces have been handed to physics. */
	inline int32 ScenarioLevelReleasedCount(const FStructureBinding& Binding)
	{
		int32 Released = 0;

		for (int32 Piece = 0; Piece < Binding.NumPieces(); ++Piece)
		{
			if (!Binding.IsPieceRemoved(Piece) && Binding.IsReleased(Piece))
			{
				++Released;
			}
		}

		return Released;
	}

	/** How many live pieces the last solve has no answer for. */
	inline int32 ScenarioLevelUnansweredCount(const FStructureBinding& Binding)
	{
		int32 Unanswered = 0;

		for (int32 Piece = 0; Piece < Binding.NumPieces(); ++Piece)
		{
			if (!Binding.IsPieceRemoved(Piece)
				&& !Binding.GetStructure().HasSupportAnswer(Piece))
			{
				++Unanswered;
			}
		}

		return Unanswered;
	}

	/** How many live pieces still have a brick standing for them. */
	inline int32 ScenarioLevelLiveBrickCount(const FStructureBinding& Binding)
	{
		int32 Bricks = 0;

		for (int32 Piece = 0; Piece < Binding.NumPieces(); ++Piece)
		{
			if (IsValid(Cast<ABrickActor>(Binding.GetActor(Piece))))
			{
				++Bricks;
			}
		}

		return Bricks;
	}

	/**
	 * Every brick in the whole world, bound or not — the count that says whether anything was laid.
	 * Asked of the world, not a binding, because a build sandbox has no structure to ask: the honest
	 * outside measure of "nothing was built" is that no brick exists anywhere, which also catches a
	 * wall laid, spawned, and its id dropped.
	 */
	inline int32 ScenarioLevelBricksInWorld(UWorld& World)
	{
		int32 Bricks = 0;

		for (TActorIterator<ABrickActor> It(&World); It; ++It)
		{
			if (IsValid(*It))
			{
				++Bricks;
			}
		}

		return Bricks;
	}

	/**
	 * Spawn the player the way LoadMap does — a controller possessing a pawn — before begin-play. No
	 * ULocalPlayer: everything here reads the controller and its pawn, and GetControlRotation needs
	 * nothing else. Attaching one would drag in Enhanced Input and a viewport-less UGameViewportClient
	 * ensure this project has been bitten by, for no claim this file makes.
	 */
	inline void ScenarioLevelSpawnPlayer(
		UWorld& World, APlayerController*& OutController, APawn*& OutPawn)
	{
		OutController = World.SpawnActor<ADestructionGamePlayerController>();

		OutPawn = World.SpawnActor<ADestructionGameFlyingPawn>(
			ScenarioLevelPawnStartsAtCm, FRotator::ZeroRotator);

		if (OutController != nullptr && OutPawn != nullptr)
		{
			OutController->Possess(OutPawn);
		}
	}
}

/**
 * The player is standing in front of the scenario the moment they join. On the pawn's real
 * transform and the controller's real control rotation, so a game mode that stored a perfect
 * viewpoint could not satisfy it while showing the inside of a brick.
 *
 * Two assertions on the same placement. The arithmetic, worked from the 90-degree FOV rather than
 * asked of ViewpointFor — height governs here at 332.2222222222222 cm of standoff, which reading
 * half-extents instead of half-height-over-aspect would get wrong by nearly two while still
 * producing a picture. And the property: every corner inside the frustum from where the pawn landed.
 *
 * Needs a world with begin-play run (when the game mode acts) and the pawn present before it; never
 * ticks one.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScenarioLevelFramesThePlayerTest,
	"DestructionGame.World.Scenario.GameModeFramesTheScenarioForThePlayer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FScenarioLevelFramesThePlayerTest::RunTest(const FString& Parameters)
{
	using namespace ScenarioLevelTestSupport;
	using namespace BrickWorldTestSupport;

	const DestructionScenarios::FScenario* const Row =
		ScenarioLevelRowNamed(*this, ScenarioLevelCutRowName);

	if (Row == nullptr)
	{
		return true;
	}

	FBrickTestWorld TestWorld;

	APlayerController* Controller = nullptr;
	APawn* Pawn = nullptr;

	TestWorld.Wrapper.BeginPlayURL.AddOption(
		*FString::Printf(TEXT("Scenario=%s"), ScenarioLevelCutRowName));

	TestWorld.Wrapper.BeforeBeginPlay = [&Controller, &Pawn](UWorld& World)
	{
		ScenarioLevelSpawnPlayer(World, Controller, Pawn);
	};

	if (!TestWorld.Begin(*this, ADestructionGameGameMode::StaticClass()))
	{
		return true;
	}

	if (Controller == nullptr || Pawn == nullptr)
	{
		AddError(FString::Printf(
			TEXT("fixture: a controller (%s) possessing a pawn (%s) must exist before begin-play, ")
			TEXT("as UEngine::LoadMap arranges in a real game"),
			*GetNameSafe(Controller), *GetNameSafe(Pawn)));

		TestWorld.End();
		return true;
	}

	TestTrue(
		*FString::Printf(
			TEXT("fixture: the controller must be possessing the pawn; it possesses %s"),
			*GetNameSafe(Controller->GetPawn())),
		Controller->GetPawn() == Pawn);

	ADestructionGameGameMode* const GameMode =
		TestWorld.World->GetAuthGameMode<ADestructionGameGameMode>();

	if (GameMode == nullptr)
	{
		AddError(FString::Printf(
			TEXT("fixture: the world must be running ADestructionGameGameMode, it is running %s"),
			*GetNameSafe(TestWorld.World->GetAuthGameMode())));

		TestWorld.End();
		return true;
	}

	FStructureBinding* const Binding =
		TestWorld.Subsystem->Find(GameMode->GetBuiltStructureId());

	if (Binding == nullptr)
	{
		AddError(FString::Printf(
			TEXT("the game mode must have built the scenario the URL named ('%s'); ")
			TEXT("GetBuiltStructureId returned %d and the subsystem holds no such structure"),
			ScenarioLevelCutRowName, GameMode->GetBuiltStructureId()));

		TestWorld.End();
		return true;
	}

	// One: it built the scenario the option named, not the default.

	TestTrue(
		*FString::Printf(
			TEXT("?Scenario=%s must build that row's wall — a flush 7 x 40 running bond of %d ")
			TEXT("pieces; the game mode built %d"),
			ScenarioLevelCutRowName, ScenarioLevelCutWallPieceCount, Binding->NumPieces()),
		Binding->NumPieces() == ScenarioLevelCutWallPieceCount);

	const FBox BoundsCm = ScenarioLevelBoundsOf(*Binding);

	AddInfo(FString::Printf(
		TEXT("the built scenario spans X %s..%s, Y %s..%s, Z %s..%s cm"),
		*ScenarioLevelBits(BoundsCm.Min.X), *ScenarioLevelBits(BoundsCm.Max.X),
		*ScenarioLevelBits(BoundsCm.Min.Y), *ScenarioLevelBits(BoundsCm.Max.Y),
		*ScenarioLevelBits(BoundsCm.Min.Z), *ScenarioLevelBits(BoundsCm.Max.Z)));

	TestTrue(
		*FString::Printf(
			TEXT("fixture: that wall must span X %s..%s and Z %s..%s; it spans %s..%s and %s..%s"),
			*ScenarioLevelBits(ScenarioLevelWallMinXCm),
			*ScenarioLevelBits(ScenarioLevelCutWallMaxXCm),
			*ScenarioLevelBits(ScenarioLevelWallMinZCm),
			*ScenarioLevelBits(ScenarioLevelWallMaxZCm),
			*ScenarioLevelBits(BoundsCm.Min.X), *ScenarioLevelBits(BoundsCm.Max.X),
			*ScenarioLevelBits(BoundsCm.Min.Z), *ScenarioLevelBits(BoundsCm.Max.Z)),
		FMath::IsNearlyEqual(BoundsCm.Min.X, ScenarioLevelWallMinXCm, BoundsToleranceCm)
			&& FMath::IsNearlyEqual(BoundsCm.Max.X, ScenarioLevelCutWallMaxXCm, BoundsToleranceCm)
			&& FMath::IsNearlyEqual(BoundsCm.Min.Z, ScenarioLevelWallMinZCm, BoundsToleranceCm)
			&& FMath::IsNearlyEqual(BoundsCm.Max.Z, ScenarioLevelWallMaxZCm, BoundsToleranceCm)
			&& FMath::IsNearlyEqual(BoundsCm.Max.Y, ScenarioLevelWallHalfYCm, BoundsToleranceCm));

	// Two: the pawn is where the framing arithmetic says, and it moved to get there.

	const FVector ExpectedPawnCm(
		(ScenarioLevelWallMinXCm + ScenarioLevelCutWallMaxXCm) / 2.0,
		ScenarioLevelCutWallStandoffCm,
		(ScenarioLevelWallMinZCm + ScenarioLevelWallMaxZCm) / 2.0);

	const FVector PawnCm = Pawn->GetActorLocation();
	const FRotator ControlRotation = Controller->GetControlRotation();

	AddInfo(FString::Printf(
		TEXT("the pawn was spawned at %s and begin-play left it at %s, facing (%s, %s, %s)"),
		*ScenarioLevelVectorBits(ScenarioLevelPawnStartsAtCm),
		*ScenarioLevelVectorBits(PawnCm),
		*ScenarioLevelBits(ControlRotation.Pitch), *ScenarioLevelBits(ControlRotation.Yaw),
		*ScenarioLevelBits(ControlRotation.Roll)));

	TestTrue(
		*FString::Printf(
			TEXT("the player's PAWN must stand at %s — half-height over a 0.5625 aspect governs ")
			TEXT("this wall, so the standoff is %s cm and not the %s cm its half-width would ")
			TEXT("earn; the pawn is at %s"),
			*ScenarioLevelVectorBits(ExpectedPawnCm),
			*ScenarioLevelBits(ScenarioLevelCutWallStandoffCm),
			*ScenarioLevelBits(1.25 * (ScenarioLevelCutWallMaxXCm - ScenarioLevelWallMinXCm) / 2.0),
			*ScenarioLevelVectorBits(PawnCm)),
		PawnCm.Equals(ExpectedPawnCm, ScenarioLevelPlacementToleranceCm));

	TestTrue(
		*FString::Printf(
			TEXT("the CONTROL ROTATION must be (0, %s, 0) — level, looking along -Y with +X to ")
			TEXT("the right, so the level reads the same way round as the design elevations; it ")
			TEXT("is (%s, %s, %s)"),
			*ScenarioLevelBits(ScenarioLevelCameraYawDegrees),
			*ScenarioLevelBits(ControlRotation.Pitch), *ScenarioLevelBits(ControlRotation.Yaw),
			*ScenarioLevelBits(ControlRotation.Roll)),
		ControlRotation.Equals(
			FRotator(0.0, ScenarioLevelCameraYawDegrees, 0.0),
			ScenarioLevelRotationToleranceDegrees));

	// Three: and the property — all of it is actually on screen from there.

	{
		FString WhyNot;

		const bool bFramed = ScenarioLevelIsWhollyInFrame(
			PawnCm, ControlRotation, ScenarioLevelAspectHeightOverWidth, BoundsCm, WhyNot);

		TestTrue(
			*FString::Printf(
				TEXT("the WHOLE structure must be in frame from where the pawn actually ended up ")
				TEXT("(%s, facing (%s, %s, %s), 90 degrees horizontal at a %s aspect): %s"),
				*ScenarioLevelVectorBits(PawnCm),
				*ScenarioLevelBits(ControlRotation.Pitch),
				*ScenarioLevelBits(ControlRotation.Yaw),
				*ScenarioLevelBits(ControlRotation.Roll),
				*ScenarioLevelBits(ScenarioLevelAspectHeightOverWidth),
				WhyNot.IsEmpty() ? TEXT("it is") : *WhyNot),
			bFramed);
	}

	TestWorld.End();

	return true;
}

/**
 * The cut fires after the delay and not before — the player sees the wall stand, then react.
 *
 * The before is the half that matters. A test checking only the end state would pass against a game
 * mode that removed the brick on the frame it built the wall, the bug this exists to stop. So the
 * cut brick is asserted live at begin-play, still live after most of the delay, and only then gone.
 * Ticked in fixed 1/60 steps so the delay is a deterministic number of frames; the catalogue's own
 * HoldSeconds is read, not repeated, so a retuned delay retunes this test.
 *
 * Not on displacement, and not on one joint (DESIGN.md §4): a brick can be deleted and its
 * neighbours not move, the correct outcome here. So the assertions are the mechanism —
 *
 *   - the piece is tombstoned and the live count down by one, so a cut that took the wrong brick fails;
 *   - the brick's actor has been destroyed, so the orphan the commit handed back was consumed rather
 *     than left as a collider nothing in the model knows about;
 *   - the solver has an answer for every piece left, so the wall was re-solved rather than emptied;
 *   - and at most one piece has been released.
 *
 * The release bound is derived, not picked: Core.Structure.AFreeEndDeletionInATallWall rules that
 * exactly one piece is available to lose — course 1's flush half bat sat entirely on the deleted
 * brick and has no bed patch left — so one is the ceiling and zero is allowed.
 *
 * No pawn in this world: a level whose player has not arrived must still build and cut; a game mode
 * that failed closed on framing and never armed the timer would pass the framing test and fail here.
 *
 * Needs a ticking world — the delay is measured in world seconds.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScenarioLevelCutsAfterTheDelayTest,
	"DestructionGame.World.Scenario.GameModeCutsAfterTheDelayAndNotBefore",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FScenarioLevelCutsAfterTheDelayTest::RunTest(const FString& Parameters)
{
	using namespace ScenarioLevelTestSupport;
	using namespace BrickWorldTestSupport;

	const DestructionScenarios::FScenario* const Row =
		ScenarioLevelRowNamed(*this, ScenarioLevelCutRowName);

	if (Row == nullptr)
	{
		return true;
	}

	TestTrue(
		*FString::Printf(
			TEXT("fixture: '%s' must name exactly one brick to cut, after a delay of more than a ")
			TEXT("second so a human can see the wall first; it names %d after %s s"),
			ScenarioLevelCutRowName, Row->CutCentresCm.Num(),
			*ScenarioLevelBits(Row->HoldSeconds)),
		Row->CutCentresCm.Num() == 1 && Row->HoldSeconds > 1.0
			&& FMath::IsFinite(Row->HoldSeconds));

	FBrickTestWorld TestWorld;

	TestWorld.Wrapper.BeginPlayURL.AddOption(
		*FString::Printf(TEXT("Scenario=%s"), ScenarioLevelCutRowName));

	if (!TestWorld.Begin(*this, ADestructionGameGameMode::StaticClass()))
	{
		return true;
	}

	ADestructionGameGameMode* const GameMode =
		TestWorld.World->GetAuthGameMode<ADestructionGameGameMode>();

	FStructureBinding* const Binding = GameMode != nullptr
		? TestWorld.Subsystem->Find(GameMode->GetBuiltStructureId())
		: nullptr;

	if (Binding == nullptr)
	{
		AddError(FString::Printf(
			TEXT("the game mode (%s) must have built the scenario the URL named ('%s') even with ")
			TEXT("no player in the world"),
			*GetNameSafe(TestWorld.World->GetAuthGameMode()), ScenarioLevelCutRowName));

		TestWorld.End();
		return true;
	}

	TestTrue(
		*FString::Printf(
			TEXT("?Scenario=%s must build that row's wall of %d pieces; it built %d"),
			ScenarioLevelCutRowName, ScenarioLevelCutWallPieceCount, Binding->NumPieces()),
		Binding->NumPieces() == ScenarioLevelCutWallPieceCount);

	// Before: the wall is whole, and the brick the level will cut is standing.

	const int32 CutPiece = ScenarioLevelPieceAtCentre(*Binding, ScenarioLevelCutCentreCm);

	if (CutPiece == INDEX_NONE)
	{
		AddError(FString::Printf(
			TEXT("fixture: the wall must contain the brick '%s' cuts, at %s"),
			ScenarioLevelCutRowName, *ScenarioLevelVectorBits(ScenarioLevelCutCentreCm)));

		TestWorld.End();
		return true;
	}

	const TWeakObjectPtr<ABrickActor> CutBrick(Cast<ABrickActor>(Binding->GetActor(CutPiece)));

	TestTrue(
		*FString::Printf(
			TEXT("AT BEGIN-PLAY the brick at %s (piece %d) must still be standing — the player ")
			TEXT("has to see the wall whole before anything happens to it"),
			*ScenarioLevelVectorBits(ScenarioLevelCutCentreCm), CutPiece),
		!Binding->IsPieceRemoved(CutPiece) && CutBrick.IsValid());

	TestTrue(
		*FString::Printf(
			TEXT("AT BEGIN-PLAY the whole wall must be there: %d of %d pieces live, %d bricks ")
			TEXT("standing, %d released"),
			Binding->GetStructure().NumLivePieces(), Binding->NumPieces(),
			ScenarioLevelLiveBrickCount(*Binding), ScenarioLevelReleasedCount(*Binding)),
		Binding->GetStructure().NumLivePieces() == Binding->NumPieces()
			&& ScenarioLevelLiveBrickCount(*Binding) == Binding->NumPieces()
			&& ScenarioLevelReleasedCount(*Binding) == 0);

	/*
	 * And the held wall is solved, which is not the same claim as "nothing was released". A level
	 * could hold its structure by never solving it, and every assertion above would still pass —
	 * nothing is released because nothing asked. That is the wrong hold: an absent support answer
	 * reads as Falling, so an unsolved wall and one in free fall answer alike, the strain readout
	 * would colour every brick unsupported, and the first click would push against an uncomputed
	 * answer. So holding is solved-and-not-settled: loads known, no joint asked to give. Green today,
	 * a fence around the fix — a hold that deleted the begin-play solve would fail it.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("AT BEGIN-PLAY the held wall must already be SOLVED — every one of its %d live ")
			TEXT("pieces must have a support answer, because an absent answer reads as Falling and ")
			TEXT("a wall nobody solved is indistinguishable from one in free fall. %d have none."),
			Binding->GetStructure().NumLivePieces(), ScenarioLevelUnansweredCount(*Binding)),
		ScenarioLevelUnansweredCount(*Binding) == 0);

	const int32 SolvesAtBeginPlay = Binding->GetStructure().NumSolves();

	// And still not cut with most of the delay gone.

	/*
	 * Seven eighths of the delay, so this stays true if the catalogue retunes it. The margin either
	 * side is half a second on today's four (thirty ticks) — far more than TickSeconds' rounding.
	 */
	const double AlmostTheDelaySeconds = Row->HoldSeconds * 0.875;

	const double BeforeStart = FPlatformTime::Seconds();
	TestWorld.TickSeconds(AlmostTheDelaySeconds);
	const double BeforeSeconds = FPlatformTime::Seconds() - BeforeStart;

	AddInfo(FString::Printf(
		TEXT("MEASURED: ticking %s of the %s s delay over %d pieces took %.1f ms"),
		*ScenarioLevelBits(AlmostTheDelaySeconds), *ScenarioLevelBits(Row->HoldSeconds),
		Binding->NumPieces(), BeforeSeconds * 1000.0));

	TestTrue(
		*FString::Printf(
			TEXT("THE CUT MUST NOT HAVE FIRED after only %s of the %s s the row asks for: piece ")
			TEXT("%d must still be live with its brick standing, and %d of %d pieces must be ")
			TEXT("live. A level that cuts at begin-play passes every AFTER assertion in this ")
			TEXT("test and shows the player nothing happening."),
			*ScenarioLevelBits(AlmostTheDelaySeconds), *ScenarioLevelBits(Row->HoldSeconds),
			CutPiece, Binding->GetStructure().NumLivePieces(), Binding->NumPieces()),
		!Binding->IsPieceRemoved(CutPiece) && CutBrick.IsValid()
			&& Binding->GetStructure().NumLivePieces() == Binding->NumPieces());

	// After: the brick has gone, and the solver has answered for what is left.

	const double AfterStart = FPlatformTime::Seconds();
	TestWorld.TickSeconds(Row->HoldSeconds * 0.375);
	const double AfterSeconds = FPlatformTime::Seconds() - AfterStart;

	AddInfo(FString::Printf(
		TEXT("MEASURED: ticking past the delay took a further %.1f ms; %d of %d pieces are live, ")
		TEXT("%d bricks stand, %d pieces are released, and the graph has solved %d times (it had ")
		TEXT("solved %d when play began)"),
		AfterSeconds * 1000.0, Binding->GetStructure().NumLivePieces(), Binding->NumPieces(),
		ScenarioLevelLiveBrickCount(*Binding), ScenarioLevelReleasedCount(*Binding),
		Binding->GetStructure().NumSolves(), SolvesAtBeginPlay));

	TestTrue(
		*FString::Printf(
			TEXT("THE CUT MUST HAVE FIRED once %s s of the %s s delay had passed: piece %d must ")
			TEXT("be removed from the graph, and it %s"),
			*ScenarioLevelBits(Row->HoldSeconds * 1.25),
			*ScenarioLevelBits(Row->HoldSeconds), CutPiece,
			Binding->IsPieceRemoved(CutPiece) ? TEXT("is") : TEXT("is NOT")),
		Binding->IsPieceRemoved(CutPiece));

	/*
	 * And its actor is gone. RunPieceActions is world-free and hands the orphan back, so a cut
	 * through another path would leave the brick's mesh standing in the hole — a collider nothing in
	 * the model knows about, and a brick the player can still click.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("the cut brick's ACTOR must have been destroyed, not merely unbound; it is %s"),
			CutBrick.IsValid() ? TEXT("still in the world") : TEXT("gone")),
		!CutBrick.IsValid());

	TestTrue(
		*FString::Printf(
			TEXT("EXACTLY ONE brick may go: '%s' names %d cut(s), so %d of the %d pieces must be ")
			TEXT("live; %d are"),
			ScenarioLevelCutRowName, Row->CutCentresCm.Num(),
			Binding->NumPieces() - Row->CutCentresCm.Num(), Binding->NumPieces(),
			Binding->GetStructure().NumLivePieces()),
		Binding->GetStructure().NumLivePieces()
			== Binding->NumPieces() - Row->CutCentresCm.Num());

	/*
	 * The solver has answered for what is left — the mechanism assertion that the wall was re-solved
	 * after the removal, not merely emptied. ApplyResults refuses to release a piece the last solve
	 * has no answer for, so an unanswered piece can never be handed to physics.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("every live piece must have a support answer after the cut; %d of %d do not"),
			ScenarioLevelUnansweredCount(*Binding), Binding->GetStructure().NumLivePieces()),
		ScenarioLevelUnansweredCount(*Binding) == 0);

	TestTrue(
		*FString::Printf(
			TEXT("the cut must have re-solved the wall: the graph had solved %d times at ")
			TEXT("begin-play and has solved %d now"),
			SolvesAtBeginPlay, Binding->GetStructure().NumSolves()),
		Binding->GetStructure().NumSolves() > SolvesAtBeginPlay);

	/*
	 * And the wall did not come down. Core.Structure.AFreeEndDeletionInATallWall rules exactly one
	 * piece is available to lose — course 1's flush half bat, entirely on the deleted brick with no
	 * bed patch left — so one is the ceiling. Three hundred pieces, at most one released; a collapse
	 * would release scores.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("A BRICK DELETED AT A FREE END MUST NOT BRING THE WALL DOWN: at most 1 of the %d ")
			TEXT("live pieces may be released (course 1's half bat is the only one the cut leaves ")
			TEXT("with no bed patch); %d were"),
			Binding->GetStructure().NumLivePieces(), ScenarioLevelReleasedCount(*Binding)),
		ScenarioLevelReleasedCount(*Binding) <= 1);

	TestWorld.End();

	return true;
}

/**
 * THE SANDBOX CUTS NOTHING, HOWEVER LONG IT IS LEFT RUNNING — AND IT IS FRAMED TOO.
 *
 * TWO CLAIMS IN ONE WORLD, because the world is the 1,220-brick scenario wall and building it
 * twice would be the most expensive thing in the suite for no extra claim.
 *
 * NOTHING FIRES. `sandbox` names no cut at all, so a level that quietly deleted a brick nobody
 * asked for is the failure this pins — and it pins it on the ACTORS as well as on the graph,
 * because a delete that removed a piece and left its mesh standing, or destroyed a mesh and left
 * its piece in the graph, are both wrong in ways one count alone would miss. The world is ticked
 * well past the delay every other row uses, so "nothing fires" means nothing fires ever rather
 * than nothing fires yet.
 *
 * AND IT IS FRAMED — on a wall of a DIFFERENT SHAPE from the other framing test's. This one is
 * 674 cm across and 299 tall, so half-WIDTH governs and the standoff is 421.25; the free-end
 * wall's height governs and its standoff is 332.222. A game mode that framed on one extent, or
 * that hardcoded a viewpoint, gets one of the two right and the other badly wrong — so the pair
 * is what says the placement is derived from the structure rather than from a constant.
 *
 * NEEDS A TICKING WORLD: YES. "Nothing ever fires" is a claim about elapsed time.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScenarioLevelSandboxCutsNothingTest,
	"DestructionGame.World.Scenario.SandboxIsFramedAndNeverCuts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FScenarioLevelSandboxCutsNothingTest::RunTest(const FString& Parameters)
{
	using namespace ScenarioLevelTestSupport;
	using namespace BrickWorldTestSupport;

	const DestructionScenarios::FScenario* const Row =
		ScenarioLevelRowNamed(*this, ScenarioLevelDefaultRowName);

	if (Row == nullptr)
	{
		return true;
	}

	TestTrue(
		*FString::Printf(
			TEXT("fixture: '%s' must name no cut at all — it is what Play gives you today, and ")
			TEXT("today nothing is removed from it; it names %d"),
			ScenarioLevelDefaultRowName, Row->CutCentresCm.Num()),
		Row->CutCentresCm.Num() == 0);

	FBrickTestWorld TestWorld;

	APlayerController* Controller = nullptr;
	APawn* Pawn = nullptr;

	/*
	 * NO OPTION ON THE URL AND NO MAP THE CATALOGUE KNOWS, which is the DEFAULT branch of
	 * selection — the game opening on its own wall, deliberately.
	 */
	TestWorld.Wrapper.BeforeBeginPlay = [&Controller, &Pawn](UWorld& World)
	{
		ScenarioLevelSpawnPlayer(World, Controller, Pawn);
	};

	if (!TestWorld.Begin(*this, ADestructionGameGameMode::StaticClass()))
	{
		return true;
	}

	ADestructionGameGameMode* const GameMode =
		TestWorld.World->GetAuthGameMode<ADestructionGameGameMode>();

	FStructureBinding* const Binding = GameMode != nullptr
		? TestWorld.Subsystem->Find(GameMode->GetBuiltStructureId())
		: nullptr;

	if (Binding == nullptr || Controller == nullptr || Pawn == nullptr)
	{
		AddError(FString::Printf(
			TEXT("fixture: the game mode (%s) must build a structure and the player (%s / %s) ")
			TEXT("must exist"),
			*GetNameSafe(TestWorld.World->GetAuthGameMode()),
			*GetNameSafe(Controller), *GetNameSafe(Pawn)));

		TestWorld.End();
		return true;
	}

	TestTrue(
		*FString::Printf(
			TEXT("with nothing naming a scenario, the game mode must build the DEFAULT row's ")
			TEXT("wall — the flush 30 x 40 running bond of %d pieces Play has always given you; ")
			TEXT("it built %d"),
			ScenarioLevelDefaultWallPieceCount, Binding->NumPieces()),
		Binding->NumPieces() == ScenarioLevelDefaultWallPieceCount);

	/* --- framed, on a wall whose WIDTH governs rather than its height -------------------- */

	const FBox BoundsCm = ScenarioLevelBoundsOf(*Binding);

	const FVector ExpectedPawnCm(
		(ScenarioLevelWallMinXCm + ScenarioLevelDefaultWallMaxXCm) / 2.0,
		ScenarioLevelDefaultWallStandoffCm,
		(ScenarioLevelWallMinZCm + ScenarioLevelWallMaxZCm) / 2.0);

	const FVector PawnCm = Pawn->GetActorLocation();
	const FRotator ControlRotation = Controller->GetControlRotation();

	AddInfo(FString::Printf(
		TEXT("the sandbox wall spans X %s..%s and Z %s..%s; the pawn was left at %s facing ")
		TEXT("(%s, %s, %s)"),
		*ScenarioLevelBits(BoundsCm.Min.X), *ScenarioLevelBits(BoundsCm.Max.X),
		*ScenarioLevelBits(BoundsCm.Min.Z), *ScenarioLevelBits(BoundsCm.Max.Z),
		*ScenarioLevelVectorBits(PawnCm),
		*ScenarioLevelBits(ControlRotation.Pitch), *ScenarioLevelBits(ControlRotation.Yaw),
		*ScenarioLevelBits(ControlRotation.Roll)));

	TestTrue(
		*FString::Printf(
			TEXT("the player's PAWN must stand at %s — this wall is %s cm across, so half-WIDTH ")
			TEXT("governs and the standoff is %s cm rather than the %s cm its height would earn; ")
			TEXT("the pawn is at %s"),
			*ScenarioLevelVectorBits(ExpectedPawnCm),
			*ScenarioLevelBits(ScenarioLevelDefaultWallMaxXCm - ScenarioLevelWallMinXCm),
			*ScenarioLevelBits(ScenarioLevelDefaultWallStandoffCm),
			*ScenarioLevelBits(
				1.25 * ((ScenarioLevelWallMaxZCm - ScenarioLevelWallMinZCm) / 2.0)
					/ ScenarioLevelAspectHeightOverWidth),
			*ScenarioLevelVectorBits(PawnCm)),
		PawnCm.Equals(ExpectedPawnCm, ScenarioLevelPlacementToleranceCm));

	{
		FString WhyNot;

		const bool bFramed = ScenarioLevelIsWhollyInFrame(
			PawnCm, ControlRotation, ScenarioLevelAspectHeightOverWidth, BoundsCm, WhyNot);

		TestTrue(
			*FString::Printf(
				TEXT("the WHOLE sandbox wall must be in frame from where the pawn ended up (%s): ")
				TEXT("%s"),
				*ScenarioLevelVectorBits(PawnCm),
				WhyNot.IsEmpty() ? TEXT("it is") : *WhyNot),
			bFramed);
	}

	/* --- and nothing is ever removed from it --------------------------------------------- */

	const int32 LiveAtBeginPlay = Binding->GetStructure().NumLivePieces();
	const int32 BricksAtBeginPlay = ScenarioLevelLiveBrickCount(*Binding);

	TestTrue(
		*FString::Printf(
			TEXT("AT BEGIN-PLAY the sandbox wall must be whole: %d of %d pieces live and %d ")
			TEXT("bricks standing"),
			LiveAtBeginPlay, Binding->NumPieces(), BricksAtBeginPlay),
		LiveAtBeginPlay == Binding->NumPieces() && BricksAtBeginPlay == Binding->NumPieces());

	/*
	 * WELL PAST THE DELAY EVERY OTHER ROW USES, so "nothing fires" is a claim about ever rather
	 * than about yet. Read off the cut row's own figure so the two cannot drift.
	 */
	const DestructionScenarios::FScenario* const CutRow =
		ScenarioLevelRowNamed(*this, ScenarioLevelCutRowName);

	const double PastEveryDelaySeconds =
		FMath::Max(1.0, (CutRow != nullptr ? CutRow->HoldSeconds : 4.0) * 1.5);

	const double TickStart = FPlatformTime::Seconds();
	TestWorld.TickSeconds(PastEveryDelaySeconds);
	const double TickedForSeconds = FPlatformTime::Seconds() - TickStart;

	AddInfo(FString::Printf(
		TEXT("MEASURED: ticking %s s over %d pieces took %.1f ms"),
		*ScenarioLevelBits(PastEveryDelaySeconds), Binding->NumPieces(),
		TickedForSeconds * 1000.0));

	TestTrue(
		*FString::Printf(
			TEXT("A SCENARIO THAT NAMES NO CUT MUST NEVER REMOVE A BRICK: after %s s, %d of %d ")
			TEXT("pieces are live (it began with %d) and %d bricks stand (it began with %d)"),
			*ScenarioLevelBits(PastEveryDelaySeconds),
			Binding->GetStructure().NumLivePieces(), Binding->NumPieces(), LiveAtBeginPlay,
			ScenarioLevelLiveBrickCount(*Binding), BricksAtBeginPlay),
		Binding->GetStructure().NumLivePieces() == Binding->NumPieces()
			&& ScenarioLevelLiveBrickCount(*Binding) == Binding->NumPieces());

	TestTrue(
		*FString::Printf(
			TEXT("and nothing may have been handed to physics either: %d piece(s) were released"),
			ScenarioLevelReleasedCount(*Binding)),
		ScenarioLevelReleasedCount(*Binding) == 0);

	TestWorld.End();

	return true;
}

/**
 * JOINING THE BUILD LEVEL OPENS AN EMPTY PLOT, FRAMED, WITH NOTHING ARMED AND NOTHING LAID.
 *
 * =====================================================================================
 * THE BEHAVIOUR IN ONE SENTENCE
 * =====================================================================================
 *
 * On a build-sandbox row the game mode builds NO structure, spawns NO brick, arms NO run, and still
 * puts the player in front of the plot with the level's own label on screen.
 *
 * =====================================================================================
 * WHY "NOTHING WAS BUILT" IS ASSERTED TWICE, IN TWO DIFFERENT CURRENCIES
 * =====================================================================================
 *
 * `GetBuiltStructureId() == INDEX_NONE` is the game mode's own record, and on its own it is a claim
 * about a field. A game mode that laid the wall, spawned twelve hundred bricks and then forgot to
 * keep the id would satisfy it exactly, and the player would be looking at a wall on the level
 * whose whole point is that there is nothing there. So the second currency is the WORLD: not one
 * `ABrickActor` exists anywhere in it. The subsystem exposes no count of the structures it holds,
 * and the brick count is the better measure anyway — it is what a human sees.
 *
 * =====================================================================================
 * THE RUN TIMER IS ASSERTED ON ITS CONSEQUENCE, AND THIS IS A DELIBERATE WEAKENING
 * =====================================================================================
 *
 * `ScenarioHoldTimer` is private and `FTimerManager` exposes no way to ask whether an OBJECT has
 * timers pending, so there is no handle to read from out here. What is asserted instead is that the
 * moment never arrives: the world is ticked well past the longest hold any row uses, and afterwards
 * nothing has been laid, nothing has been cut, and the label still reports no cut. A timer armed on
 * a callback that provably does nothing is indistinguishable from an unarmed one FROM OUTSIDE — and
 * with no structure to name, `RunScenario` is exactly that. The pairing with the INDEX_NONE
 * assertion is what closes the gap: the only run this game mode can arm is one it arms because it
 * built something, and it must not have built anything.
 *
 * =====================================================================================
 * THE FRAMING IS PINNED AS A PROPERTY, NEVER AS A PLACE
 * =====================================================================================
 *
 * The other two tests in this file pin the pawn to the exact centimetre, because their walls have
 * bounds and the arithmetic over those bounds is the thing under test. An empty plot has NO bounds
 * — the box the game mode frames is a default one it invents — so pinning a number here would pin
 * the invention rather than the requirement, and `World.Scenarios.Viewpoint` already owns the
 * arithmetic. What is required is what a human would notice:
 *
 *   - the pawn ends up somewhere FINITE, NEAR the plot (inside twenty metres of the origin) and
 *     ABOVE the ground rather than in it;
 *   - the view is pitched DOWN, because a level camera on an empty plot shows the horizon and
 *     nothing else — which means the row must ask for a framing that looks down at the ground it
 *     is framing rather than head-on at a wall that is not there;
 *   - and the view points AT the plot rather than past it.
 *
 * AND THE PAWN IS SPAWNED FIFTY METRES OUT AND FIVE BELOW THE FLOOR, so every one of those is a
 * claim about a MOVE. A game mode that framed nothing at all leaves the pawn there, which fails the
 * reach, the height and the aim together rather than passing one of them by luck.
 *
 * NEEDS A TICKING WORLD: YES. The "nothing ever runs" half is a claim about elapsed time.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScenarioLevelBuildSandboxTest,
	"DestructionGame.World.Scenario.GameModeOpensAnEmptyBuildSandbox",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FScenarioLevelBuildSandboxTest::RunTest(const FString& Parameters)
{
	using namespace ScenarioLevelTestSupport;
	using namespace BrickWorldTestSupport;

	const DestructionScenarios::FScenario* const Row =
		ScenarioLevelRowNamed(*this, ScenarioLevelBuildRowName);

	if (Row == nullptr)
	{
		return true;
	}

	TestTrue(
		*FString::Printf(
			TEXT("fixture: '%s' must be the row that lays NOTHING — bBuildSandbox set, no cut; it ")
			TEXT("is %s and names %d cut(s)"),
			ScenarioLevelBuildRowName,
			Row->bBuildSandbox ? TEXT("flagged") : TEXT("NOT flagged"),
			Row->CutCentresCm.Num()),
		Row->bBuildSandbox && Row->CutCentresCm.Num() == 0);

	FBrickTestWorld TestWorld;

	APlayerController* Controller = nullptr;
	APawn* Pawn = nullptr;

	TestWorld.Wrapper.BeginPlayURL.AddOption(
		*FString::Printf(TEXT("Scenario=%s"), ScenarioLevelBuildRowName));

	TestWorld.Wrapper.BeforeBeginPlay = [&Controller, &Pawn](UWorld& World)
	{
		ScenarioLevelSpawnPlayer(World, Controller, Pawn);
	};

	if (!TestWorld.Begin(*this, ADestructionGameGameMode::StaticClass()))
	{
		return true;
	}

	ADestructionGameGameMode* const GameMode =
		TestWorld.World->GetAuthGameMode<ADestructionGameGameMode>();

	if (GameMode == nullptr || Controller == nullptr || Pawn == nullptr)
	{
		AddError(FString::Printf(
			TEXT("fixture: the world must run ADestructionGameGameMode (%s) with a player (%s / %s) ")
			TEXT("already in it"),
			*GetNameSafe(TestWorld.World->GetAuthGameMode()),
			*GetNameSafe(Controller), *GetNameSafe(Pawn)));

		TestWorld.End();
		return true;
	}

	/* --- ONE: the URL selected the build row, and it selected it BY OPTION ---------------- */

	const int32 BuildRowIndex =
		DestructionScenarios::IndexOfName(FName(ScenarioLevelBuildRowName));

	TestTrue(
		*FString::Printf(
			TEXT("?Scenario=%s must select the build row (%d) by option; the game mode recorded row ")
			TEXT("%d, selection %d (ByOption is %d)"),
			ScenarioLevelBuildRowName, BuildRowIndex, GameMode->GetSelectedScenarioRow(),
			static_cast<int32>(GameMode->GetScenarioSelection()),
			static_cast<int32>(DestructionScenarios::EScenarioSelection::ByOption)),
		GameMode->GetSelectedScenarioRow() == BuildRowIndex
			&& GameMode->GetScenarioSelection()
				== DestructionScenarios::EScenarioSelection::ByOption);

	/* --- TWO: nothing was built, in both currencies --------------------------------------- */

	const int32 BricksAtBeginPlay = ScenarioLevelBricksInWorld(*TestWorld.World);

	AddInfo(FString::Printf(
		TEXT("the build level began play with built structure id %d and %d brick(s) in the world; ")
		TEXT("the pawn was spawned at %s and left at %s facing (%s, %s, %s)"),
		GameMode->GetBuiltStructureId(), BricksAtBeginPlay,
		*ScenarioLevelVectorBits(ScenarioLevelPawnStartsAtCm),
		*ScenarioLevelVectorBits(Pawn->GetActorLocation()),
		*ScenarioLevelBits(Controller->GetControlRotation().Pitch),
		*ScenarioLevelBits(Controller->GetControlRotation().Yaw),
		*ScenarioLevelBits(Controller->GetControlRotation().Roll)));

	TestTrue(
		*FString::Printf(
			TEXT("A BUILD SANDBOX LAYS NOTHING: the game mode must build no structure at all, so ")
			TEXT("GetBuiltStructureId stays INDEX_NONE (%d); it is %d, and the subsystem %s a ")
			TEXT("structure by that id"),
			INDEX_NONE, GameMode->GetBuiltStructureId(),
			TestWorld.Subsystem->Find(GameMode->GetBuiltStructureId()) != nullptr
				? TEXT("holds") : TEXT("holds no")),
		GameMode->GetBuiltStructureId() == INDEX_NONE
			&& TestWorld.Subsystem->Find(GameMode->GetBuiltStructureId()) == nullptr);

	TestTrue(
		*FString::Printf(
			TEXT("and NOT ONE BRICK may stand in the world — the plot is empty until the player ")
			TEXT("lays something on it; %d brick(s) are there"),
			BricksAtBeginPlay),
		BricksAtBeginPlay == 0);

	/* --- THREE: the level still says what it is ------------------------------------------ */

	const DestructionScenarios::FScenarioLabel Label = GameMode->GetScenarioLabel();

	TestTrue(
		*FString::Printf(
			TEXT("the build level must still carry its OWN label — a level with no structure is ")
			TEXT("exactly the one a player needs told what it is for. Title '%s' against the row's ")
			TEXT("'%s'; expectation '%s' against '%s'"),
			*Label.TitleText, Row->Title, *Label.ExpectationText, Row->Expectation),
		Label.TitleText == FString(Row->Title)
			&& Label.ExpectationText == FString(Row->Expectation));

	TestTrue(
		*FString::Printf(
			TEXT("and it must claim NO CUT and no clock — there is nothing laid to take out. Its ")
			TEXT("cut state is %d (NoCut is %d) with %s s on the clock, and it reads '%s'"),
			static_cast<int32>(Label.CutState),
			static_cast<int32>(DestructionScenarios::EScenarioCutState::NoCut),
			*ScenarioLevelBits(Label.SecondsUntilCut), *Label.CutText),
		Label.CutState == DestructionScenarios::EScenarioCutState::NoCut
			&& Label.SecondsUntilCut == 0.0);

	/* --- FOUR: the player is standing on the plot, looking down at it --------------------- */

	const FVector PawnCm = Pawn->GetActorLocation();
	const FRotator ControlRotation = Controller->GetControlRotation();

	const FVector Forward = FRotationMatrix(ControlRotation).GetScaledAxis(EAxis::X);
	const FVector ToPlot = (FVector::ZeroVector - PawnCm).GetSafeNormal();
	const double AimDot = FVector::DotProduct(Forward, ToPlot);

	TestTrue(
		*FString::Printf(
			TEXT("the player must be MOVED onto the plot: a finite place within %s cm of the origin ")
			TEXT("and above the ground, rather than left at the %s they were spawned at; the pawn ")
			TEXT("is at %s, %s cm out"),
			*ScenarioLevelBits(ScenarioLevelBuildPlotReachCm),
			*ScenarioLevelVectorBits(ScenarioLevelPawnStartsAtCm),
			*ScenarioLevelVectorBits(PawnCm), *ScenarioLevelBits(PawnCm.Size())),
		PawnCm.ContainsNaN() == false && PawnCm.Size() <= ScenarioLevelBuildPlotReachCm
			&& PawnCm.Z > 0.0);

	TestTrue(
		*FString::Printf(
			TEXT("and the view must look DOWN at the ground it is framing — an empty plot seen from ")
			TEXT("a level camera is the horizon and nothing else, so the row must ask for a framing ")
			TEXT("that pitches down; the pitch is %s and the forward is %s"),
			*ScenarioLevelBits(ControlRotation.Pitch), *ScenarioLevelVectorBits(Forward)),
		ControlRotation.Pitch < 0.0 && Forward.Z < 0.0);

	TestTrue(
		*FString::Printf(
			TEXT("and it must point AT the plot rather than past it: the forward %s against the ")
			TEXT("direction to the origin %s reads %s, and must be over %s"),
			*ScenarioLevelVectorBits(Forward), *ScenarioLevelVectorBits(ToPlot),
			*ScenarioLevelBits(AimDot), *ScenarioLevelBits(ScenarioLevelBuildAimDot)),
		AimDot > ScenarioLevelBuildAimDot);

	/* --- FIVE: and the moment never arrives ---------------------------------------------- */

	/*
	 * PAST THE LONGEST HOLD ANY ROW USES, read off the cutting row's own figure so the two cannot
	 * drift, so "nothing runs" is a claim about ever rather than about yet.
	 */
	const DestructionScenarios::FScenario* const CutRow =
		ScenarioLevelRowNamed(*this, ScenarioLevelCutRowName);

	const double PastEveryDelaySeconds =
		FMath::Max(1.0, (CutRow != nullptr ? CutRow->HoldSeconds : 4.0) * 1.5);

	TestWorld.TickSeconds(PastEveryDelaySeconds);

	const DestructionScenarios::FScenarioLabel AfterLabel = GameMode->GetScenarioLabel();

	TestTrue(
		*FString::Printf(
			TEXT("NO RUN MAY BE ARMED ON A BUILD SANDBOX: after %s s the plot must still be empty ")
			TEXT("(%d brick(s), structure id %d) and the label must still claim no cut (state %d, ")
			TEXT("'%s')"),
			*ScenarioLevelBits(PastEveryDelaySeconds),
			ScenarioLevelBricksInWorld(*TestWorld.World), GameMode->GetBuiltStructureId(),
			static_cast<int32>(AfterLabel.CutState), *AfterLabel.CutText),
		ScenarioLevelBricksInWorld(*TestWorld.World) == 0
			&& GameMode->GetBuiltStructureId() == INDEX_NONE
			&& AfterLabel.CutState == DestructionScenarios::EScenarioCutState::NoCut);

	TestWorld.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
