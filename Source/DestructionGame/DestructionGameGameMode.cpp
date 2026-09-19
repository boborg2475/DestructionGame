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

/*
 * File-local names sit in a named namespace and carry a GameMode prefix. An anonymous
 * namespace is private to a translation unit rather than to a file, and a unity build merges
 * many files into one, so two file-local names that collide are a hard compile error between
 * files that never refer to each other. See CURRENT_STATE.md.
 */
namespace DestructionGameModeScenario
{
	/**
	 * The aspect a level frames for; 16:9 because nothing here can measure a viewport.
	 *
	 * DestructionScenarios::ViewpointFor needs height-over-width to know how far back a tall
	 * structure must be seen from. There is no UGameViewportClient to ask before the first frame
	 * (and every automation run is -nullrhi and has none at all), so this is the standing answer.
	 */
	constexpr double GameModeFrameAspectHeightOverWidth = 1080.0 / 1920.0;

	/**
	 * The row that takes a brick out of the world, looked up by label.
	 *
	 * By label so nothing hard-codes a position in the table — the same lookup
	 * Tests/StructureIntegrationTest.cpp makes, since actions are data and a level reaching for
	 * AllPieceActions()[0] would silently cut with whatever action was added first.
	 */
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

	/** How a selection was reached, as a word a human reading the log can act on. */
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

	/** Every row's name, so a player who mistyped is told what they could have typed. */
	FString GameModeScenarioNames()
	{
		TArray<FString> Names;

		for (const DestructionScenarios::FScenario& Row : DestructionScenarios::Catalogue())
		{
			Names.Add(Row.Name.ToString());
		}

		return FString::Join(Names, TEXT(", "));
	}

	/**
	 * The plot an empty level is framed on, half-size in cm.
	 *
	 * A build sandbox has no structure, so it has no bounds — every other row frames on the
	 * union of the boxes it laid, but an empty box collapses onto ViewpointFor's minimum standoff
	 * and aims at the origin from wherever that happens to be. So the level invents the ground
	 * instead: three by three metres, a metre of headroom, centred on the origin.
	 */
	const FVector GameModeBuildPlotHalfSizeCm(150.0, 150.0, 50.0);

	/** Put every player in front of a viewpoint: the control rotation, and the pawn's place. */
	void GameModeFramePlayers(UWorld& World, const DestructionScenarios::FViewpoint& Viewpoint)
	{
		// The control rotation is the one that matters: the flying pawn is an ADefaultPawn and takes its facing from its controller, overwriting anything set on the pawn directly.
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
	 * Open the session for every player: the strip up, and on a plot they lay themselves, Build mode.
	 *
	 * Through the controller's one door rather than by setting a mode field: OnToolbarButton is
	 * what opens the build, raises the cursor and redraws the strip, so setting the field
	 * directly would put a player in Build mode with no structure behind it.
	 *
	 * The strip goes up on every row, including the twenty-eight that open in Destroy — it is how
	 * the player reaches Run structure on the wall just laid, and reaches Build mode at all.
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

	/** Every box of a laid layout, unioned: the structure a player is about to be shown. */
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

	/*
	 * Which scenario: the option wins, else the map, else the default. The whole rule lives in
	 * the world-free DestructionScenarios::IndexForOptionsAndMap, since a map name arrives in
	 * half a dozen decorations that a code-built test world cannot produce.
	 */
	DestructionScenarios::EScenarioSelection How = DestructionScenarios::EScenarioSelection::Default;

	const int32 Row = DestructionScenarios::IndexForOptionsAndMap(
		OptionsString, World->GetMapName(), How);

	/*
	 * Recorded before anything is built, and as a pair: the row alone can't carry it, since a
	 * URL naming a nonexistent scenario and one naming nothing at all both land on the default
	 * row, and a mistyped one must be tellable apart from outside.
	 */
	SelectedScenarioRow = Row;
	SelectedScenarioHow = How;

	if (!DestructionScenarios::Catalogue().IsValidIndex(Row))
	{
		return;
	}

	const DestructionScenarios::FScenario& Scenario = DestructionScenarios::Catalogue()[Row];

	// Said aloud, a thin read of the two values above: a mistyped `?Scenario=` used to give a silently different wall.
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
	 * A build sandbox is opened empty, and the branch comes before the build rather than after:
	 * the row describes no structure — no producer, a default wall spec, no cut — so
	 * DestructionScenarios::Build would refuse it, returning from begin-play before anything is
	 * framed and leaving the player facing wherever they spawned. So the row is read first:
	 * nothing is laid, BuiltStructureId stays INDEX_NONE, no hold is armed, and the player is
	 * put on the plot anyway. The row is still recorded above, so the level still names itself.
	 */
	if (Scenario.bBuildSandbox)
	{
		GameModeFramePlayers(
			*World,
			DestructionScenarios::ViewpointFor(
				FBox(-GameModeBuildPlotHalfSizeCm, GameModeBuildPlotHalfSizeCm),
				GameModeFrameAspectHeightOverWidth,
				Scenario.Framing));

		// The one level that lays nothing opens in Build mode — Destroy here would offer an empty plot and no way to fill it.
		GameModeOpenSession(*World, /*bBuildSandbox*/ true);

		return;
	}

	/*
	 * The catalogue lays the wall and resolves the cut together, and refuses both if either fails —
	 * a cut centre naming no brick would otherwise give a level that stands there looking intact
	 * and never does anything, reading exactly like a level whose wall correctly stood.
	 */
	DestructionLayout::FBrickLayout Layout;
	TArray<int32> CutPieces;

	if (!DestructionScenarios::Build(Scenario, Layout, CutPieces))
	{
		return;
	}

	// A refused build leaves BuiltStructureId at INDEX_NONE, which already means "built no structure"; BuildLayout spends no id on a refusal either.
	BuiltStructureId = Subsystem->BuildLayout(Layout);

	/*
	 * Left solved but not settled, which is what holding a structure as laid means.
	 *
	 * Solved, because an unsolved structure has no support answer for any piece —
	 * EPieceSupport::Falling is what an absent answer reads as, so the strain readout would
	 * paint every brick unsupported and the first click would push against nothing computed.
	 *
	 * Not settled, because settling is what the player came to watch: seven of the nine rows cut
	 * nothing and are condemned by their own geometry instead (a corbel's root joint is over
	 * capacity the moment it exists), so settling here would hand 36 of E36's 519 pieces to
	 * physics before the player's first frame was drawn. RunScenario below is where the giving
	 * happens, once the hold has expired.
	 */
	if (FStructureBinding* const Binding = Subsystem->Find(BuiltStructureId))
	{
		Binding->SolveLoads();
	}

	/*
	 * The player is put in front of it — the point of a level as opposed to the headless fixture
	 * it's made of. The standoff is derived from what was actually laid, not a constant, because
	 * the rows are different shapes: the seven-wide wall is governed by height, the thirty-wide
	 * one by width, and framing on either extent alone crops the other.
	 */
	GameModeFramePlayers(
		*World,
		DestructionScenarios::ViewpointFor(
			GameModeScenarioBounds(Layout), GameModeFrameAspectHeightOverWidth, Scenario.Framing));

	// Strip up, session in Destroy — where a level that has already laid a wall wants the player, since the click they're about to make is on it.
	GameModeOpenSession(*World, /*bBuildSandbox*/ false);

	/*
	 * The level is armed rather than run: a player who joins to find the hole already there has
	 * watched nothing happen. Armed on every row, including the ones that cut nothing, because
	 * the hold is the level's one moment rather than the cut's — a corbel still has something to
	 * show, how it was laid and then what settling does to it.
	 *
	 * A hold of zero, or a NaN, arms nothing: FTimerManager::SetTimer clears the timer for any
	 * rate its own `> 0` test rejects, and every comparison against NaN is false — the safe
	 * degenerate reading is "holds the structure as laid and never runs", not one that vanished.
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
	 * The hold timer's own remainder, asked every time, rather than a start time this class
	 * subtracts from — the level runs off world time including pauses and dilation, so a second
	 * clock here would drift from the event it announces.
	 *
	 * An unarmed handle answers -1 and a torn-down world answers nothing, both of which
	 * BuildScenarioLabel takes as no time left; the world is null-checked before it is touched.
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
		/*
		 * The batched commit, the door the player's own delete goes through: runs the action
		 * against every named brick, settles the wall once behind the last, pushes that one
		 * answer onto the world and destroys the orphaned meshes — a multi-brick cut costs one
		 * settle. This already settles a cutting row, so it must not be settled again below.
		 */
		Subsystem->CommitPieceActionForAll(ScenarioCutRefs, *CutAction);

		// The label is told after the commit, not before: the flag states the brick has gone, so a cut that failed would otherwise read as done while the wall stood whole.
		bScenarioCutHasFired = true;
	}
	else
	{
		/*
		 * A row that cuts nothing still runs, and settling is what it runs: every joint over
		 * capacity gives, and what the solver stops holding up goes to physics — for a corbel,
		 * the entire story the level exists to tell. The flag is not set here: nothing was cut.
		 */
		Subsystem->SolveAndPush(BuiltStructureId);
	}
}
