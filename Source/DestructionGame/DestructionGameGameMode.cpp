// Copyright Epic Games, Inc. All Rights Reserved.

#include "DestructionGameGameMode.h"
#include "Core/Layout.h"
#include "Core/PieceActions.h"
#include "Core/SessionToolbar.h"
#include "DestructionGameFlyingPawn.h"
#include "DestructionGamePlayerController.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"
#include "World/DestructionScenarios.h"
#include "World/DestructionStructureSubsystem.h"

// Named, GameMode-prefixed namespace: unity builds merge translation units (CURRENT_STATE.md).
namespace DestructionGameModeScenario
{
	/** Framing aspect, assumed 16:9: no viewport exists before the first frame, or ever under -nullrhi. */
	constexpr double GameModeFrameAspectHeightOverWidth = 1080.0 / 1920.0;

	/** The Delete action, looked up by label rather than table position. */
	const FPieceAction* GameModeScenarioCutAction()
	{
		for (const FPieceAction& Action : AllPieceActions())
		{
			if (Action.Label != nullptr && FCString::Strcmp(Action.Label, TEXT("Delete")) == 0)
			{
				return &Action;
			}
		}

		return nullptr;
	}

	/** How a scenario was selected, for the log. */
	FString GameModeSelectionName(DestructionScenarios::EScenarioSelection How)
	{
		switch (How)
		{
		case DestructionScenarios::EScenarioSelection::ByOption:  return TEXT("by ?Scenario=");
		case DestructionScenarios::EScenarioSelection::ByMapName: return TEXT("by map name");
		case DestructionScenarios::EScenarioSelection::Default:   return TEXT("as the default");
		default:                                                  return TEXT("as a FALLBACK");
		}
	}

	/** Every scenario name, logged when a requested one does not exist. */
	FString GameModeScenarioNames()
	{
		TArray<FString> Names;

		for (const DestructionScenarios::FScenario& Row : DestructionScenarios::Catalogue())
		{
			Names.Add(Row.Name.ToString());
		}

		return FString::Join(Names, TEXT(", "));
	}

	/** Half-size of the build sandbox's framed plot, cm (3 x 3 m, 1 m headroom); an empty level has no bounds. */
	const FVector GameModeBuildPlotHalfSizeCm(150.0, 150.0, 50.0);

	/** Set every player's control rotation and pawn location to the viewpoint. */
	void GameModeFramePlayers(UWorld& World, const DestructionScenarios::FViewpoint& Viewpoint)
	{
		// Control rotation, not pawn rotation: the ADefaultPawn takes its facing from the controller.
		for (FConstPlayerControllerIterator It = World.GetPlayerControllerIterator(); It; ++It)
		{
			APlayerController* const Controller = It->Get();

			if (Controller == nullptr)
			{
				continue;
			}

			Controller->SetControlRotation(Viewpoint.Rotation);

			if (APawn* const Pawn = Controller->GetPawn())
			{
				Pawn->SetActorLocation(Viewpoint.LocationCm);
			}
		}
	}

	/**
	 * Show the session toolbar for every player, and on the build sandbox enter Build mode via
	 * OnToolbarButton (which also opens the build and raises the cursor).
	 */
	void GameModeOpenSession(UWorld& World, bool bBuildSandbox)
	{
		for (FConstPlayerControllerIterator It = World.GetPlayerControllerIterator(); It; ++It)
		{
			ADestructionGamePlayerController* const Controller =
				Cast<ADestructionGamePlayerController>(It->Get());

			if (Controller == nullptr)
			{
				continue;
			}

			Controller->ShowSessionToolbar();

			if (bBuildSandbox)
			{
				Controller->OnToolbarButton(DestructionSession::EToolbarButtonId::ModeBuild);
			}
		}
	}

	/** Union of every box in the layout. */
	FBox GameModeScenarioBounds(const DestructionLayout::FBrickLayout& Layout)
	{
		FBox BoundsCm(ForceInit);

		for (const DestructionLayout::FPieceBox& Box : Layout.Boxes)
		{
			BoundsCm += FBox(Box.CentreCm - Box.ExtentCm, Box.CentreCm + Box.ExtentCm);
		}

		return BoundsCm;
	}
}

ADestructionGameGameMode::ADestructionGameGameMode()
{
	DefaultPawnClass = ADestructionGameFlyingPawn::StaticClass();
	PlayerControllerClass = ADestructionGamePlayerController::StaticClass();
}

void ADestructionGameGameMode::BeginPlay()
{
	using namespace DestructionGameModeScenario;

	Super::BeginPlay();

	UWorld* const World = GetWorld();

	UDestructionStructureSubsystem* const Subsystem =
		World != nullptr ? World->GetSubsystem<UDestructionStructureSubsystem>() : nullptr;

	if (Subsystem == nullptr)
	{
		return;
	}

	// Option, else map name, else default; the rule is world-free so it can be tested.
	DestructionScenarios::EScenarioSelection How = DestructionScenarios::EScenarioSelection::Default;

	const int32 Row = DestructionScenarios::IndexForOptionsAndMap(
		OptionsString, World->GetMapName(), How);

	// Row and how, since a mistyped scenario and none at all both land on the default row.
	SelectedScenarioRow = Row;
	SelectedScenarioHow = How;

	if (!DestructionScenarios::Catalogue().IsValidIndex(Row))
	{
		return;
	}

	const DestructionScenarios::FScenario& Scenario = DestructionScenarios::Catalogue()[Row];

	UE_LOG(LogTemp, Log, TEXT("Scenario '%s' (%s), selected %s"),
		*Scenario.Name.ToString(), Scenario.Title, *GameModeSelectionName(How));

	if (How == DestructionScenarios::EScenarioSelection::OptionNamedNoScenario)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("?Scenario= named a scenario that does not exist, so '%s' was a FALLBACK rather ")
			TEXT("than a default. The scenarios that do exist are: %s"),
			*Scenario.Name.ToString(), *GameModeScenarioNames());
	}

	/*
	 * The build sandbox lays nothing, so branch before Build (which would refuse it) and still
	 * frame the player on the plot. No structure, no hold.
	 */
	if (Scenario.bBuildSandbox)
	{
		GameModeFramePlayers(
			*World,
			DestructionScenarios::ViewpointFor(
				FBox(-GameModeBuildPlotHalfSizeCm, GameModeBuildPlotHalfSizeCm),
				GameModeFrameAspectHeightOverWidth,
				Scenario.Framing));

		GameModeOpenSession(*World, /*bBuildSandbox*/ true);

		return;
	}

	// Lays the wall and resolves the cut together; refuses both if either fails.
	DestructionLayout::FBrickLayout Layout;
	TArray<int32> CutPieces;

	if (!DestructionScenarios::Build(Scenario, Layout, CutPieces))
	{
		return;
	}

	// INDEX_NONE on refusal.
	BuiltStructureId = Subsystem->BuildLayout(Layout);

	/*
	 * Solved but not settled. Solved, since without a support answer every piece reads Falling.
	 * Not settled, since settling is what the player watches: rows that cut nothing fail by their
	 * own geometry, and settling now would drop pieces before the first frame. RunScenario settles
	 * after the hold.
	 */
	if (FStructureBinding* const Binding = Subsystem->Find(BuiltStructureId))
	{
		Binding->SolveLoads();
	}

	// Frame on the laid bounds, since rows differ in shape (some height-bound, some width-bound).
	GameModeFramePlayers(
		*World,
		DestructionScenarios::ViewpointFor(
			GameModeScenarioBounds(Layout), GameModeFrameAspectHeightOverWidth, Scenario.Framing));

	GameModeOpenSession(*World, /*bBuildSandbox*/ false);

	/*
	 * Arm the hold on every row, so the player sees the structure as laid before anything happens.
	 * A zero or NaN hold arms nothing (SetTimer rejects rates failing `> 0`), so the structure just
	 * holds.
	 */
	ScenarioCutRefs.Reset();

	if (BuiltStructureId != INDEX_NONE)
	{
		for (const int32 CutPiece : CutPieces)
		{
			FPieceRef Ref;
			Ref.StructureId = BuiltStructureId;
			Ref.PieceIndex = CutPiece;

			ScenarioCutRefs.Add(Ref);
		}

		World->GetTimerManager().SetTimer(
			ScenarioHoldTimer,
			this,
			&ADestructionGameGameMode::RunScenario,
			static_cast<float>(Scenario.HoldSeconds),
			/*bLoop*/ false);
	}
}

DestructionScenarios::FScenarioLabel ADestructionGameGameMode::GetScenarioLabel() const
{
	const UWorld* const World = GetWorld();

	/*
	 * Read the timer's own remainder so pauses and dilation cannot make a second clock drift. An
	 * unarmed handle (-1) or missing world both read as no time left.
	 */
	const double SecondsUntilCut = World != nullptr
		? static_cast<double>(World->GetTimerManager().GetTimerRemaining(ScenarioHoldTimer))
		: 0.0;

	return DestructionScenarios::BuildScenarioLabel(
		SelectedScenarioRow, SecondsUntilCut, bScenarioCutHasFired);
}

void ADestructionGameGameMode::RunScenario()
{
	UWorld* const World = GetWorld();

	UDestructionStructureSubsystem* const Subsystem =
		World != nullptr ? World->GetSubsystem<UDestructionStructureSubsystem>() : nullptr;

	const FPieceAction* const CutAction = DestructionGameModeScenario::GameModeScenarioCutAction();

	if (Subsystem == nullptr)
	{
		return;
	}

	if (ScenarioCutRefs.Num() > 0 && CutAction != nullptr)
	{
		// The player's own batched delete path: one settle for the whole cut, so no second settle below.
		Subsystem->CommitPieceActionForAll(ScenarioCutRefs, *CutAction);

		// Set after the commit so a failed cut never reads as done.
		bScenarioCutHasFired = true;
	}
	else
	{
		// A row with no cut just settles. Nothing was cut, so the flag stays clear.
		Subsystem->SolveAndPush(BuiltStructureId);
	}
}
