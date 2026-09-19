// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Layout.h"

/**
 * The named catalogue of playable destruction scenarios — the fixtures the suite proves
 * headlessly, addressable as levels a human can join and watch.
 *
 * World-free, like Core/Layout: no UWorld, no actors, no UObject, which keeps the
 * catalogue's own tests in the fast suite and lets the game mode be the only thing that
 * knows a level exists.
 *
 * One direction of inclusion: this includes Core/Layout.h and the profile headers, nothing
 * from Tests/. A test may reach the other way — that drift check is what keeps a level
 * honest — but production may not.
 */
namespace DestructionScenarios
{
	/**
	 * How a row wants to be framed — data on the row, defaulting to the head-on view every
	 * existing level was designed around.
	 *
	 * A flat wall reads perfectly from straight in front; a genuinely 3D structure reads as a
	 * flat front face from there, so its row opts into a three-quarter view instead: orbited
	 * off-axis and raised, so depth and fall are visible rather than foreshortened.
	 */
	enum class EScenarioFraming : uint8
	{
		/** Straight in front along -Y, level — the original view; the default for every row. */
		HeadOn,

		/** Orbited off-axis and elevated, looking down at the centre, framing the full 3D box. */
		ThreeQuarter,
	};

	/** One playable scenario: the wall, what to cut out of it, and what a human should see. */
	struct FScenario
	{
		/** What ?Scenario= names on a URL, and what IndexOfName looks up. */
		FName Name;

		/** The map that selects this row, e.g. "Lvl_FreeEnd40". */
		const TCHAR* MapName = nullptr;

		const TCHAR* Title = nullptr;

		/** One line: what a human should see happen. */
		const TCHAR* Expectation = nullptr;

		DestructionLayout::FRunningBondSpec Wall;

		/**
		 * What lays this row, when a running bond cannot — the producer as data on the row.
		 *
		 * Unset means the `Wall` spec above, laid by `RunningBond`. A corbel steps half a cell
		 * per course off an immovable base, which `RunningBond` cannot express, so its row
		 * carries the call that lays it instead.
		 *
		 * A function, not an enum with a switch in `Build`: scenarios are data here for the
		 * same reason materials and connection types are, so a row carrying its own producer
		 * is one row of a table and `Build` never learns what a corbel is.
		 */
		TFunction<bool(DestructionLayout::FBrickLayout&)> LayStructure;

		/** Centres of the bricks removed AFTER the player has had a look, cm. */
		TArray<FVector> CutCentresCm;

		/**
		 * This row lays nothing, because the player lays it — the build sandbox.
		 *
		 * A build sandbox is an empty plot: the game mode opens no layout, spawns no brick, and
		 * arms no hold timer. `Build` refuses such a row outright (`RunningBond` refuses zero
		 * courses), so the game mode must branch on this before it asks for a layout.
		 */
		bool bBuildSandbox = false;

		/**
		 * How long the level holds this structure exactly as laid before it runs, seconds.
		 *
		 * One clock, on every row, named for what it does rather than for the cut:
		 * `CutDelaySeconds` lied on seven of nine rows, since a corbel cuts nothing at all.
		 *
		 * "Hold" means as laid, not unsolved: every brick spawns kinematic and only
		 * `ABrickActor::Release` makes it dynamic, so holding is simply not releasing
		 * anything, while the structure is still solved — `EPieceSupport::Falling` is what an
		 * absent answer reads as. Solved and not settled: no joint has been asked to give yet.
		 *
		 * At this moment the level runs: it applies whatever cuts the row names and settles —
		 * every joint over capacity gives. A row with no cut still has a moment; that is the
		 * whole point of a corbel level, whose story is as-laid versus settled.
		 */
		double HoldSeconds = 4.0;

		/** How this row wants to be viewed. A 3D row sets ThreeQuarter to show its depth. */
		EScenarioFraming Framing = EScenarioFraming::HeadOn;
	};

	/** Every scenario, in menu order. */
	const TArray<FScenario>& Catalogue();

	/** The row with this name, or INDEX_NONE. */
	int32 IndexOfName(FName Name);

	/** The row whose map this is, case-insensitively, or INDEX_NONE. */
	int32 IndexOfMapName(const FString& MapName);

	/**
	 * How a scenario came to be chosen — so a fallback is never mistaken for a default.
	 *
	 * The index alone cannot carry this: a URL naming a nonexistent scenario and a URL naming
	 * nothing at all both end up on `sandbox`, and the player must be told which happened —
	 * a typo they can fix, or the game opening normally. Indistinguishable, they are a player
	 * staring at the wrong wall wondering why.
	 */
	enum class EScenarioSelection : uint8
	{
		/** A `Scenario=` option on the URL named this row. */
		ByOption,

		/** No option, and the map name named this row. */
		ByMapName,

		/** Nothing named anything, so the default row was taken deliberately. */
		Default,

		/** A `Scenario=` option named a row that does not exist. The default was a FALLBACK. */
		OptionNamedNoScenario,
	};

	/**
	 * Which scenario a level should build, from its URL options and its map name.
	 *
	 * Always a valid row, never INDEX_NONE: an empty world is a worse failure than the
	 * default wall.
	 *
	 * @param Options  the URL options string, `?Key=Value?Key=Value`, as
	 *                 `AGameModeBase::OptionsString` carries it.
	 * @param MapName  the map, as `UWorld::GetMapName()` gives it — a `UEDPIE_0_`-prefixed
	 *                 name in PIE, or a long package path elsewhere.
	 * @param OutHow   how the answer was reached. See EScenarioSelection.
	 */
	int32 IndexForOptionsAndMap(
		const FString& Options,
		const FString& MapName,
		EScenarioSelection& OutHow);

	/**
	 * Lay a row's wall and resolve the bricks its cut names.
	 *
	 * Refuses a cut centre that names no brick, writing nothing: a silently dropped cut is
	 * indistinguishable from a level whose wall correctly stood.
	 *
	 * @return true if the wall was laid and every cut centre found its brick.
	 */
	bool Build(
		const FScenario& Scenario,
		DestructionLayout::FBrickLayout& OutLayout,
		TArray<int32>& OutCutPieces);

	/** Where the player stands, and which way they are facing. */
	struct FViewpoint
	{
		FVector LocationCm = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
	};

	/**
	 * Where to stand so the whole of a structure is in frame.
	 *
	 * `Framing` selects head-on or a three-quarter angle that orbits and elevates the camera
	 * to frame the full 3D bounds including Y-depth.
	 */
	FViewpoint ViewpointFor(
		const FBox& BoundsCm,
		double AspectHeightOverWidth,
		EScenarioFraming Framing = EScenarioFraming::HeadOn);
}
