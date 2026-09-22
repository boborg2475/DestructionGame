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
 * Pressing Play on a scenario: the game mode builds it, frames the player on it, and cuts it after
 * a delay. Framing is asserted on the pawn's real transform and control rotation, not a stored
 * intent. The cut needs a before as well as an after, or cutting at begin-play would pass.
 *
 * The map branch of selection is unreachable in a code-built world (World.Scenarios.Selection pins
 * it); these use ?Scenario= on BeginPlayURL. The pawn is spawned in BeforeBeginPlay, matching
 * UEngine::LoadMap, at an absurd spot so "it was moved" is never luck.
 */
namespace ScenarioLevelTestSupport
{
	using namespace DestructionLayout;

	const TCHAR* const ScenarioLevelCutRowName = TEXT("free-end-40");
	const TCHAR* const ScenarioLevelDefaultRowName = TEXT("sandbox");
	const TCHAR* const ScenarioLevelBuildRowName = TEXT("build");

	/**
	 * Max distance from the origin on an empty plot, cm. A ceiling, not the exact standoff, which
	 * World.Scenarios.Viewpoint pins.
	 */
	constexpr double ScenarioLevelBuildPlotReachCm = 2000.0;

	/** Min cosine between the camera's look and the direction to the plot (~25 degrees, loose on purpose). */
	constexpr double ScenarioLevelBuildAimDot = 0.9;

	/** Fallback 16:9 aspect; -nullrhi runs have no viewport to ask. */
	constexpr double ScenarioLevelAspectHeightOverWidth = 1080.0 / 1920.0;

	/*
	 * Wall extents, derived by hand. Flush running bonds on the 22.5 x 11.25 x 7.5 grid: X from -10.75
	 * to (N-1)*22.5 + 10.75, top at Z 299, 10.25 thick centred on Y 0. Pieces = 40N + 20.
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
	 * Expected standoff, worked by hand: with a 90-degree horizontal FOV, standoff =
	 * max(120, 1.25 * max(halfX, halfZ/aspect)). Height governs the 7-wide wall (332.222), width the
	 * 30-wide one (421.25). The camera is on +Y, level, looking along -Y like the design elevations.
	 */
	constexpr double ScenarioLevelCutWallStandoffCm = 332.2222222222222;
	constexpr double ScenarioLevelDefaultWallStandoffCm = 421.25;
	constexpr double ScenarioLevelCameraYawDegrees = -90.0;

	constexpr double ScenarioLevelPlacementToleranceCm = 0.01;

	/** Yaw passes through a float somewhere. */
	constexpr double ScenarioLevelRotationToleranceDegrees = 1.0e-3;

	/** Spawn point before begin-play, somewhere nothing would legitimately frame. */
	const FVector ScenarioLevelPawnStartsAtCm = FVector(-5000.0, -5000.0, -500.0);

	/** The brick free-end-40 cuts: the outermost full brick of the grounded course. */
	const FVector ScenarioLevelCutCentreCm = FVector(0.0, 0.0, 3.25);

	/** See DestructionScenarios.cpp. */
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

	/** Union of every live piece's box. */
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
	 * Whether every corner of the box is inside a 90-degree-FOV frustum. Uses the camera's own axes,
	 * so a view from the wrong side fails. Survives a retuned framing margin.
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
	 * The piece centred here, live or removed, or INDEX_NONE. Includes tombstones so a brick cut too
	 * early reads as a broken level, not a fixture error.
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

	/** Every brick in the world, bound or not. Catches a wall laid and its id dropped. */
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
	 * Spawn a controller possessing a pawn, as LoadMap does. No ULocalPlayer: it would pull in Enhanced
	 * Input and a viewport-less UGameViewportClient ensure.
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
 * The player is framed on the scenario at begin-play: the exact place (height governs, standoff
 * 332.222 cm) and the property that every corner is in frame. Never ticks.
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
 * The cut fires after the delay, not before: the brick is live at begin-play, still live at 7/8 of
 * HoldSeconds, then gone. Afterwards the piece is tombstoned, its actor destroyed, every live piece
 * has a support answer, and at most one piece is released (course 1's half bat loses its whole bed;
 * see Core.Structure.AFreeEndDeletionInATallWall). No pawn: a level must build and cut before the
 * player arrives.
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
	 * The held wall must be solved, not just unreleased: an absent support answer reads as Falling,
	 * so an unsolved wall would show every brick unsupported.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("AT BEGIN-PLAY the held wall must already be SOLVED — every one of its %d live ")
			TEXT("pieces must have a support answer, because an absent answer reads as Falling and ")
			TEXT("a wall nobody solved is indistinguishable from one in free fall. %d have none."),
			Binding->GetStructure().NumLivePieces(), ScenarioLevelUnansweredCount(*Binding)),
		ScenarioLevelUnansweredCount(*Binding) == 0);

	const int32 SolvesAtBeginPlay = Binding->GetStructure().NumSolves();

	// 7/8 of the delay: half a second of margin on today's four, far more than tick rounding.
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

	// RunPieceActions hands the orphan back; a cut by another path would leave its mesh standing.
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

	// The wall was re-solved after the removal, not merely emptied.
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

	// At most one release: course 1's half bat (Core.Structure.AFreeEndDeletionInATallWall).
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
 * The sandbox is framed and never cuts. One world for both claims, since the 1,220-brick wall is
 * expensive. Width governs here (standoff 421.25), height in the free-end test, so together they
 * show the placement is derived. Pieces and actors are both counted, ticked past every row's delay.
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

	// No option and no known map: the default branch of selection.
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

	const int32 LiveAtBeginPlay = Binding->GetStructure().NumLivePieces();
	const int32 BricksAtBeginPlay = ScenarioLevelLiveBrickCount(*Binding);

	TestTrue(
		*FString::Printf(
			TEXT("AT BEGIN-PLAY the sandbox wall must be whole: %d of %d pieces live and %d ")
			TEXT("bricks standing"),
			LiveAtBeginPlay, Binding->NumPieces(), BricksAtBeginPlay),
		LiveAtBeginPlay == Binding->NumPieces() && BricksAtBeginPlay == Binding->NumPieces());

	// Past the cut row's own delay, so the two cannot drift.
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
 * The build level opens an empty plot: no structure, no brick, no run armed, but the player is
 * framed and the label shown.
 *
 * "Nothing built" is checked on the game mode's id and on the world's brick count, which also
 * catches a wall laid with its id dropped. The hold timer is private, so "no run armed" is checked
 * by ticking past every hold and finding nothing laid or cut.
 *
 * Framing is a property, not a place: an empty plot has no bounds (World.Scenarios.Viewpoint owns
 * the arithmetic). The pawn must end up near the origin and above ground, pitched down, aimed at
 * the plot.
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

	// Past the cut row's own delay, so the two cannot drift.
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
