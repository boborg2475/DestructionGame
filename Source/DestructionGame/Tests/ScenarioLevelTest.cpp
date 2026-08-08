// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "DestructionGameFlyingPawn.h"
#include "DestructionGameGameMode.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Tests/BrickWorldTestSupport.h"
#include "UObject/WeakObjectPtr.h"
#include "World/DestructionScenarios.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * PRESSING PLAY ON A SCENARIO: THE GAME MODE BUILDS IT, PUTS THE PLAYER IN FRONT OF IT, AND
 * CUTS IT WHILE THEY WATCH.
 *
 * =====================================================================================
 * WHAT THE USER ACTUALLY ASKED FOR, AND WHY EACH HALF IS ASSERTED THE WAY IT IS
 * =====================================================================================
 *
 * The whole point of a scenario LEVEL, as opposed to the headless fixture it is made of, is that
 * a human can watch it. That is two things the suite has never checked and one it has:
 *
 *   - THE WALL IS IN FRONT OF THE PLAYER THE INSTANT THEY JOIN. Asserted on the PAWN'S REAL
 *     TRANSFORM and on the CONTROLLER'S REAL CONTROL ROTATION, never on a stored intent: a game
 *     mode that computed a perfect viewpoint and put it in a field would satisfy any assertion
 *     about the field and show the player the inside of a brick. And it is asserted twice over —
 *     once as the exact place the framing arithmetic names, and once as the geometric property
 *     a human means by "I can see all of it", every corner of the structure inside the frustum
 *     from where the pawn actually ended up.
 *
 *   - THE CUT HAPPENS AFTER A DELAY, NOT AT BEGIN-PLAY. This is the claim that needs a BEFORE as
 *     well as an AFTER. A test that only checked the end state would pass, in full, against a
 *     game mode that deleted the brick on the frame it built the wall — which is precisely the
 *     bug, because a player who joins and sees a hole already there has watched nothing.
 *
 * DISPLACEMENT IS NEVER READ AS EVIDENCE OF ANYTHING. DESIGN.md §4 is explicit, and it applies
 * to the cut here as much as to a joint: the assertions are the MECHANISM — the piece is
 * tombstoned, its actor has been destroyed, the live count is down by exactly one, the solver has
 * an answer for every piece that is left, and the release count says the wall did not come down.
 *
 * =====================================================================================
 * HOW THE SCENARIO IS SELECTED IN A CODE-BUILT WORLD
 * =====================================================================================
 *
 * `UWorld::GetMapName()` in a code-built world answers with the test world's own package name,
 * so the MAP branch of selection is unreachable from here — which is exactly why selection was
 * pulled out as a world-free function and is pinned, in every form a map name arrives in, by
 * `World.Scenarios.Selection`. What IS reachable is the OPTION branch, and it is reached through
 * the real production wire rather than a back door: `FBrickTestWorldWrapper::BeginPlayURL` puts
 * `?Scenario=free-end-40` on the URL, `UWorld::InitializeActorsForPlay` turns the URL's options
 * into the string it hands `AGameModeBase::InitGame`, and that is what fills `OptionsString`.
 *
 * =====================================================================================
 * WHY THE PAWN IS SPAWNED BEFORE BEGIN-PLAY
 * =====================================================================================
 *
 * `UEngine::LoadMap` spawns each local player's controller and pawn BETWEEN
 * `InitializeActorsForPlay` and `UWorld::BeginPlay`, so in a real game the game mode's begin-play
 * runs with a possessed pawn already in the world. The harness's `BeforeBeginPlay` hook is that
 * gap, and putting the pawn there is what makes this a test of the game mode rather than of the
 * harness's ordering. It is spawned at a deliberately absurd place, five metres below the floor
 * and fifty metres off, so "it was moved" is never confusable with "it happened to be there".
 *
 * AND ONE TEST DELIBERATELY HAS NO PAWN AT ALL, because a level with nobody in it yet must still
 * build and still cut rather than failing closed on the framing step.
 *
 * NEEDS A TICKING WORLD: YES, and these are the only tests in the file that need the ticking
 * rather than merely the world — the delay is measured in world seconds, so the world is ticked
 * through it. The framing test needs a world with begin-play run and never ticks it.
 */
namespace ScenarioLevelTestSupport
{
	using namespace DestructionLayout;

	/** The row the option selects, and the row the default gives — named, never indexed. */
	const TCHAR* const ScenarioLevelCutRowName = TEXT("free-end-40");
	const TCHAR* const ScenarioLevelDefaultRowName = TEXT("sandbox");

	/**
	 * THE ASPECT A LEVEL FRAMES FOR WHEN NOTHING CAN TELL IT THE VIEWPORT'S.
	 *
	 * Every run in this suite is `-nullrhi` and every world in it is built in code, so there is
	 * no `UGameViewportClient` to ask and no viewport to measure — and a game mode that gave up
	 * and framed nothing under those conditions would be untestable by construction. 16:9 is the
	 * fallback, stated here as a requirement rather than discovered: 1080 high per 1920 wide.
	 */
	constexpr double ScenarioLevelAspectHeightOverWidth = 1080.0 / 1920.0;

	/*
	 * --- what the two walls measure, derived by hand from the coordinating grid -----------
	 *
	 * Both are flush running bonds on the 22.5 x 11.25 x 7.5 grid, so along X a course runs from
	 * -10.75 to (N - 1) x 22.5 + 10.75, up Z the top course's top face is 3.25 + 39 x 7.5 + 3.25
	 * = 299, and across Y the wall is one 10.25 cm brick centred on zero.
	 *
	 * PIECES. Even courses are N full bricks; odd courses fill the half cell at each end with a
	 * half bat, so N - 1 + 2 = N + 1. Twenty of each: 20N + 20(N + 1) = 40N + 20. Seven wide is
	 * 300 and thirty wide is 1,220.
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
	 * WHERE THE PLAYER MUST END UP, WORKED THROUGH HERE RATHER THAN ASKED OF ViewpointFor.
	 *
	 * A 90-degree HORIZONTAL field of view makes the visible half-width at a standoff s equal to
	 * s, and the visible half-height s x aspect. Framing a box therefore needs s >= halfX and
	 * s >= halfZ / aspect, and the 1.25 margin keeps the structure off the edges of the frame:
	 *
	 *     standoff = max(120, 1.25 x max(halfX, halfZ / aspect))
	 *
	 * THE TWO WALLS PICK DIFFERENT TERMS, WHICH IS THE POINT OF CHECKING BOTH. The 7-wide wall is
	 * 156.5 cm across and 299 tall, so halfZ / aspect = 149.5 / 0.5625 = 265.777... beats the
	 * 78.25 of half-width and the standoff is 1.25 x 265.777... = 332.2222222222222 — HEIGHT
	 * governs. The 30-wide wall is 674 across, so 1.25 x 337 = 421.25 beats the same 332.222 —
	 * WIDTH governs. A game mode that framed on one extent only would put one of the two walls
	 * off the sides of the screen, and would print a perfectly plausible number for the other.
	 *
	 * The camera stands on the +Y side of the wall's centre at that standoff, level, looking
	 * along -Y with +X to the right — the way round every elevation in the design documents is
	 * drawn, so a level reads the same as the drawings rather than mirrored.
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
	 * IS ALL OF IT ON SCREEN FROM HERE — the property, checked corner by corner.
	 *
	 * WORTH MORE THAN THE PLACEMENT ASSERTION AND SEPARATE FROM IT. The placement pins where the
	 * arithmetic says to stand; this pins what a human means by being able to see the thing, and
	 * it would still hold if somebody retuned the margin or the floor. It is written against the
	 * camera's OWN axes rather than against an assumed -Y, so a viewpoint that faced the wall
	 * from the wrong side, or that came out mirrored, fails here rather than passing on a yaw
	 * comparison that happened to be symmetric.
	 *
	 * A 90-degree HORIZONTAL field of view puts the frustum's half-width at the corner's own
	 * depth and its half-height at that depth times the aspect. Every corner must be IN FRONT of
	 * the camera and inside both.
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
	 * The piece whose box is centred here, LIVE OR NOT, or INDEX_NONE.
	 *
	 * REMOVED PIECES ARE DELIBERATELY INCLUDED, and it matters: `FPieceBinding::Box` is kept when
	 * a piece is removed precisely so that where a brick WAS is still answerable, and this is
	 * the lookup that has to find the brick a game mode cut too early. Skipping tombstones
	 * instead would turn "it cut at begin-play" — the exact bug this file exists to catch — into
	 * a fixture error saying the wall does not contain the brick, which reads like a broken test
	 * rather than like a broken level. (It really did: the mutation run said so.)
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
	 * Spawn the player the way LoadMap does — a controller possessing a pawn — before begin-play.
	 *
	 * NO ULocalPlayer, DELIBERATELY. Everything under test here reads the CONTROLLER and its
	 * PAWN: `GetPlayerControllerIterator` lists a spawned controller whether or not a local
	 * player has been attached to it, and `SetControlRotation`/`GetControlRotation` need
	 * nothing else. Attaching one would drag in Enhanced Input, a viewport-less
	 * `UGameViewportClient` and an `ensure` this project has already been bitten by — see
	 * `SpawnControllerWithLocalPlayer`'s own note — for no claim this file makes.
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
 * THE PLAYER IS STANDING IN FRONT OF THE SCENARIO THE MOMENT THEY JOIN.
 *
 * ON THE PAWN'S REAL TRANSFORM AND THE CONTROLLER'S REAL CONTROL ROTATION. A game mode that
 * worked out a perfect viewpoint and stored it would satisfy any assertion about the stored
 * value while showing the player the inside of a brick, so nothing here reads anything the game
 * mode remembers — only where the pawn actually is and which way the controller is actually
 * pointing it.
 *
 * TWO INDEPENDENT ASSERTIONS ON THE SAME PLACEMENT. The first is the arithmetic, worked through
 * in this file's own support namespace from the 90-degree horizontal field of view rather than
 * asked of `ViewpointFor` — height governs on this wall, and the number is 332.2222222222222 cm
 * of standoff, which an implementation reading half-EXTENTS instead of half-height-over-aspect
 * would get wrong by a factor of nearly two while still producing a picture. The second is the
 * property: every corner of the structure inside the frustum from where the pawn ended up.
 *
 * NEEDS A TICKING WORLD: it needs a WORLD with begin-play run, because that is when the game
 * mode acts, and it needs the pawn to exist before that. It never ticks one.
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

	/* --- ONE: it built the scenario the OPTION named, not the default -------------------- */

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

	/* --- TWO: the pawn is where the framing arithmetic says, and it MOVED to get there ---- */

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

	/* --- THREE: and the property — all of it is actually on screen from there ------------- */

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
 * THE CUT FIRES AFTER THE DELAY AND NOT BEFORE — THE PLAYER SEES THE WALL STAND, THEN SEES IT
 * REACT.
 *
 * =====================================================================================
 * THE BEFORE IS THE HALF THAT MATTERS
 * =====================================================================================
 *
 * A test that only checked the end state would pass, in full, against a game mode that removed
 * the brick on the frame it built the wall — and that is exactly the bug this exists to stop,
 * because a player who joins to find a hole already there has watched nothing happen. So the
 * cut brick is asserted LIVE at begin-play, asserted STILL LIVE after most of the delay has
 * elapsed, and only then asserted gone.
 *
 * TICKED THROUGH IN SIMULATED SECONDS RATHER THAN WAITED OUT. The world is advanced in fixed
 * 1/60 steps, which is what makes the delay a deterministic number of frames rather than a
 * wall-clock race; the catalogue's own `HoldSeconds` is read rather than repeated here, so a
 * row that retunes its delay retunes this test with it.
 *
 * =====================================================================================
 * NOT ON DISPLACEMENT, AND NOT ON A SINGLE JOINT EITHER
 * =====================================================================================
 *
 * DESIGN.md §4 forbids reading movement as evidence that anything broke, and the same objection
 * applies here in reverse: a brick can be deleted and its neighbours not move a millimetre,
 * which is the CORRECT outcome for this scenario. So the assertions are the mechanism —
 *
 *   - the piece is tombstoned and the live count is down by EXACTLY one, so a cut that took a
 *     brick nobody asked for fails;
 *   - the brick's ACTOR has been destroyed, which is what says the orphan the commit handed
 *     back was actually consumed rather than left standing in the hole as a collider nothing in
 *     the model knows about;
 *   - the solver has an answer for every piece that is left, which is what says the wall was
 *     re-solved after the removal rather than merely emptied;
 *   - and at most one piece has been released.
 *
 * THE RELEASE BOUND IS DERIVED, NOT PICKED. `Core.Structure.AFreeEndDeletionInATallWall` rules
 * that this deletion must not bring the wall down, and works out that exactly one piece is
 * available to lose: course 1's flush half bat sat entirely on the deleted brick, so it has no
 * bed patch left at all and composite action has nothing to offer a piece that is not resting on
 * anything. Everything else keeps a seat. So one is the ceiling and zero is allowed.
 *
 * NO PAWN IN THIS WORLD, DELIBERATELY. A level whose player has not arrived yet must still build
 * and still cut; a game mode that failed closed on the framing step and never armed the timer
 * would pass every assertion in the framing test and fail every one here.
 *
 * NEEDS A TICKING WORLD: YES, and the ticking is the point — the delay is measured in world
 * seconds.
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

	/* --- BEFORE: the wall is whole, and the brick the level will cut is standing ---------- */

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
	 * AND THE HELD WALL IS SOLVED, WHICH IS NOT THE SAME CLAIM AS "NOTHING WAS RELEASED".
	 *
	 * A LEVEL COULD HOLD ITS STRUCTURE BY SIMPLY NEVER SOLVING IT, and every assertion above
	 * would still pass: nothing is released because nothing ever asked. That is the wrong hold.
	 * `EPieceSupport::Falling` is what an ABSENT support answer reads as, so an unsolved wall and
	 * a wall in free fall are one answer to everything that asks — the strain readout would colour
	 * three hundred bricks as unsupported, `World.Scenario.GameModeBuildsTheWallOnBeginPlay` would
	 * report every piece Falling "as the game mode left it", and the first click anywhere would
	 * push against an answer nobody had computed.
	 *
	 * SO HOLDING IS SOLVED-AND-NOT-SETTLED: the loads are known and no joint has been asked to
	 * give yet. This assertion is GREEN TODAY and is here as the fence around the fix — it is the
	 * assertion that a hold implemented by deleting the begin-play solve would fail.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("AT BEGIN-PLAY the held wall must already be SOLVED — every one of its %d live ")
			TEXT("pieces must have a support answer, because an absent answer reads as Falling and ")
			TEXT("a wall nobody solved is indistinguishable from one in free fall. %d have none."),
			Binding->GetStructure().NumLivePieces(), ScenarioLevelUnansweredCount(*Binding)),
		ScenarioLevelUnansweredCount(*Binding) == 0);

	const int32 SolvesAtBeginPlay = Binding->GetStructure().NumSolves();

	/* --- AND STILL NOT CUT WITH MOST OF THE DELAY GONE ------------------------------------ */

	/*
	 * SEVEN EIGHTHS OF THE DELAY, so this row stays true if the catalogue retunes it. The margin
	 * either side is half a second on today's four, which is thirty ticks — far more than the
	 * rounding in TickSeconds, and far less than the delay itself.
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

	/* --- AFTER: the brick has gone, and the solver has answered for what is left ---------- */

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
	 * AND ITS ACTOR IS GONE. RunPieceActions is world-free and hands the orphan BACK rather than
	 * destroying it, so a cut that went through some other path would leave the brick's mesh
	 * standing in the hole it was deleted from — not merely untidy, but a collider nothing in the
	 * model knows about, and a brick the player can still click.
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
	 * THE SOLVER HAS ANSWERED FOR WHAT IS LEFT, and this is the mechanism assertion that says the
	 * wall was re-solved after the removal rather than merely emptied. FStructureBinding::
	 * ApplyResults refuses to release a piece the last solve has no answer for, so an unanswered
	 * piece is a brick nothing can ever hand to physics.
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
	 * AND THE WALL DID NOT COME DOWN, which is what '%s' says a human should see.
	 * Core.Structure.AFreeEndDeletionInATallWall rules that exactly one piece is available to
	 * lose — course 1's flush half bat, which sat entirely on the deleted brick and has no bed
	 * patch left — so one is the ceiling and zero is allowed. Three hundred pieces and at most
	 * one released is the outcome assertion; a collapse would release scores.
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

#endif // WITH_DEV_AUTOMATION_TESTS
