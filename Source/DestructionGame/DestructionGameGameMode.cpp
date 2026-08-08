// Copyright Epic Games, Inc. All Rights Reserved.

#include "DestructionGameGameMode.h"
#include "Core/Layout.h"
#include "Core/PieceActions.h"
#include "DestructionGameFlyingPawn.h"
#include "DestructionGamePlayerController.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "TimerManager.h"
#include "World/DestructionScenarios.h"
#include "World/DestructionStructureSubsystem.h"

/*
 * File-local names sit in a NAMED namespace and carry a GameMode prefix. An anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges
 * many files into one — so two file-local names that collide are a hard compile error between
 * files that never refer to each other. See CURRENT_STATE.md.
 */
namespace DestructionGameModeScenario
{
	/**
	 * THE ASPECT A LEVEL FRAMES FOR, and 16:9 because nothing here can measure a viewport.
	 *
	 * DestructionScenarios::ViewpointFor needs the viewport's height over its width to know how
	 * far back a TALL structure has to be seen from — a 90-degree field of view is horizontal,
	 * so height is the requirement that does not come free. There is no UGameViewportClient to
	 * ask before the first frame, and every automation run is -nullrhi and has none at all, so
	 * this is the standing answer rather than a fallback behind a measurement.
	 */
	constexpr double GameModeFrameAspectHeightOverWidth = 1080.0 / 1920.0;

	/**
	 * The row that takes a brick out of the world, looked up by label.
	 *
	 * BY LABEL SO NOTHING HARD-CODES A POSITION IN THE TABLE, which is the same lookup
	 * Tests/StructureIntegrationTest.cpp makes for the same reason: actions are data, the table
	 * grows by rows, and a level that reached for AllPieceActions()[0] would silently cut with
	 * whatever action was added first.
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
	 * WHICH SCENARIO, AND BOTH INPUTS ARE READ WHATEVER THE WORLD IS. The option wins, else the
	 * map, else the default — the whole rule lives in the world-free
	 * DestructionScenarios::IndexForOptionsAndMap, because a map name arrives in half a dozen
	 * decorations and none of them can be produced by a code-built test world.
	 */
	DestructionScenarios::EScenarioSelection How = DestructionScenarios::EScenarioSelection::Default;

	const int32 Row = DestructionScenarios::IndexForOptionsAndMap(
		OptionsString, World->GetMapName(), How);

	/*
	 * RECORDED BEFORE ANYTHING IS BUILT, AND AS A PAIR. The row alone cannot carry it: a URL
	 * naming a scenario that does not exist and a URL naming nothing at all both land on the
	 * default row, and a player who mistyped has to be able to tell those apart from outside.
	 */
	SelectedScenarioRow = Row;
	SelectedScenarioHow = How;

	if (!DestructionScenarios::Catalogue().IsValidIndex(Row))
	{
		return;
	}

	const DestructionScenarios::FScenario& Scenario = DestructionScenarios::Catalogue()[Row];

	/*
	 * AND SAID OUT LOUD, WHICH IS A THIN READ OF THE TWO VALUES ABOVE RATHER THAN A SECOND
	 * DECISION. Until now a level loaded, a wall appeared, and nothing anywhere named which
	 * scenario it was — so a mistyped `?Scenario=` gave a silently different wall.
	 */
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
	 * THE CATALOGUE LAYS THE WALL AND RESOLVES THE CUT TOGETHER, and it refuses both if either
	 * fails — a cut centre naming no brick would otherwise give a level that stands there
	 * looking intact and never does anything, which reads exactly like a level whose wall
	 * correctly stood.
	 */
	DestructionLayout::FBrickLayout Layout;
	TArray<int32> CutPieces;

	if (!DestructionScenarios::Build(Scenario, Layout, CutPieces))
	{
		return;
	}

	/*
	 * A REFUSED BUILD LEAVES BuiltStructureId AT INDEX_NONE, which is what it already means:
	 * the game mode built no structure. BuildLayout spends no id on a refusal either, so there
	 * is no half-built wall to reconcile.
	 */
	BuiltStructureId = Subsystem->BuildLayout(Layout);

	/*
	 * AND THE STRUCTURE IS LEFT SOLVED BUT NOT SETTLED, WHICH IS WHAT HOLDING IT AS LAID
	 * MEANS.
	 *
	 * SOLVED, BECAUSE AN UNSOLVED STRUCTURE IS NOT A HELD ONE. A structure nobody has solved
	 * has no support answer for any piece, and EPieceSupport::Falling is what an ABSENT answer
	 * reads as — so an unsolved wall is indistinguishable from one in free fall to everything
	 * that asks, the strain readout would colour every brick unsupported, and the first click
	 * anywhere would push against an answer nobody computed.
	 *
	 * AND NOT SETTLED, BECAUSE SETTLING IS THE THING THE PLAYER CAME TO WATCH. Seven of the
	 * nine rows cut nothing and are condemned by their own geometry instead: a corbel's root
	 * joint is over capacity the moment it exists, so a settle here handed 36 of E36's 519
	 * pieces to physics on the frame the level was built, before the player's first frame was
	 * drawn. SolveLoads is the non-destructive half — every joint's share is known and no
	 * joint has been asked to give — and RunScenario below is where the giving happens, once
	 * the hold has expired.
	 *
	 * A REFUSED BUILD LEAVES NO BINDING TO FIND, so there is no branch of its own here.
	 */
	if (FStructureBinding* const Binding = Subsystem->Find(BuiltStructureId))
	{
		Binding->SolveLoads();
	}

	/*
	 * THE PLAYER IS PUT IN FRONT OF IT, WHICH IS THE WHOLE POINT OF A LEVEL AS OPPOSED TO THE
	 * HEADLESS FIXTURE IT IS MADE OF. The standoff is derived from what was actually laid
	 * rather than from a constant, because the rows are different shapes: the seven-wide wall
	 * is governed by its height and the thirty-wide one by its width, so a viewpoint framed on
	 * either extent alone puts one of the two off the edge of the screen.
	 *
	 * THE CONTROL ROTATION IS THE ROTATION THAT MATTERS. The flying pawn is an ADefaultPawn and
	 * takes its facing from its controller, so setting the pawn's own would be overwritten on
	 * the next tick by whatever the controller still believed.
	 */
	const DestructionScenarios::FViewpoint Viewpoint = DestructionScenarios::ViewpointFor(
		GameModeScenarioBounds(Layout), GameModeFrameAspectHeightOverWidth);

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
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

	/*
	 * AND THE LEVEL IS ARMED RATHER THAN RUN. A player who joins to find the hole already
	 * there, or the arm already down, has watched nothing happen; the point of these levels is
	 * seeing the structure whole, seeing what the row does to it, and seeing what the rest of
	 * it does about that.
	 *
	 * ARMED ON EVERY ROW, INCLUDING THE ONES THAT CUT NOTHING, because the hold is the level's
	 * one moment rather than the cut's. A corbel names no brick and still has something to
	 * show — how it was laid, and then what settling does to it — and `sandbox` holds like
	 * everything else and then settles to the same wall it was laid as.
	 *
	 * A HOLD OF ZERO, OR A NaN, ARMS NOTHING: FTimerManager::SetTimer clears the timer for any
	 * rate its own `> 0` test rejects, and every comparison against a NaN is false — so the
	 * degenerate case lands on "the level holds the structure as laid and never runs", which
	 * is a player looking at an intact structure rather than one that vanished on them.
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
	 * THE HOLD TIMER'S OWN REMAINDER, ASKED EVERY TIME, rather than a start time this class
	 * subtracts from. The level is armed on the timer manager and runs off world time including
	 * whatever the world does about pauses and time dilation, so a second clock kept here would be
	 * a second answer to when the brick goes — and the label would drift away from the event it
	 * announces. A row that names no cut has its hold armed too, and BuildScenarioLabel ignores
	 * whatever is measured for it: there is no brick for a clock to count towards.
	 *
	 * AN UNARMED HANDLE ANSWERS -1 AND A TORN-DOWN WORLD ANSWERS NOTHING AT ALL, both of which
	 * BuildScenarioLabel takes as no time left. Nothing is dereferenced here that the world does
	 * not still own: the timer manager dies with the world, and this is null-checked.
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
		 * THE BATCHED COMMIT, WHICH IS THE DOOR THE PLAYER'S OWN DELETE GOES THROUGH. It runs the
		 * action against every named brick, settles the wall exactly once behind the last of them,
		 * pushes that one answer onto the world and destroys the orphaned meshes — so a level shows
		 * a player nothing they could not reproduce by clicking, and a multi-brick cut costs one
		 * settle rather than one per brick.
		 *
		 * SO A CUTTING ROW IS ALREADY SETTLED BY THIS, and must not be settled a second time
		 * afterwards: a second full solve doubles the cost for nothing, and a second CASCADE would
		 * stamp a second collapse for one moment.
		 */
		Subsystem->CommitPieceActionForAll(ScenarioCutRefs, *CutAction);

		/*
		 * AND THE LABEL IS TOLD, AFTER THE COMMIT RATHER THAN BEFORE IT. The flag is what turns the
		 * countdown into a report, so it states that the brick has GONE — a level that announced the
		 * cut and then failed to make it would otherwise read as done while the wall stood whole.
		 */
		bScenarioCutHasFired = true;
	}
	else
	{
		/*
		 * A ROW THAT CUTS NOTHING STILL RUNS, AND SETTLING IS WHAT IT RUNS. Every joint over its
		 * own capacity gives, the share moves onto whatever is left, and what the solver stops
		 * holding up is handed to physics — which for a corbel is the entire story the level
		 * exists to tell, and for the sandbox wall is one solve that breaks nothing.
		 *
		 * THE FLAG IS NOT SET HERE. Nothing was cut, so a level that claimed a brick was out
		 * would be lying about the one thing the label is for.
		 */
		Subsystem->SolveAndPush(BuiltStructureId);
	}
}
