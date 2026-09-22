// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Layout.h"

/**
 * The catalogue of playable scenarios: the suite's fixtures, addressable as levels. World-free (no
 * UWorld, actors or UObject) so its tests stay fast. Production includes nothing from Tests/.
 */
namespace DestructionScenarios
{
	/** How a row is framed. 3D structures opt into ThreeQuarter so depth is visible. */
	enum class EScenarioFraming : uint8
	{
		/** Straight in front along -Y, level. The default. */
		HeadOn,

		/** Orbited off-axis and elevated, framing the full 3D box. */
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
		 * The producer, for rows a running bond cannot express (such as a corbel). Unset means `Wall`
		 * laid by `RunningBond`. A function rather than an enum so `Build` stays table-driven.
		 */
		TFunction<bool(DestructionLayout::FBrickLayout&)> LayStructure;

		/** Centres of the bricks removed after the hold, cm. */
		TArray<FVector> CutCentresCm;

		/**
		 * The build sandbox: an empty plot the player builds on. `Build` refuses such a row, so the
		 * game mode must check this before asking for a layout.
		 */
		bool bBuildSandbox = false;

		/**
		 * Seconds the structure is held as laid before the level runs: solved but not settled, no
		 * brick released. Then the row's cuts (if any) are applied and the structure settles.
		 */
		double HoldSeconds = 4.0;
		EScenarioFraming Framing = EScenarioFraming::HeadOn;
	};

	/** Every scenario, in menu order. */
	const TArray<FScenario>& Catalogue();

	/** The row with this name, or INDEX_NONE. */
	int32 IndexOfName(FName Name);

	/** The row whose map this is, case-insensitively, or INDEX_NONE. */
	int32 IndexOfMapName(const FString& MapName);

	/**
	 * How a scenario was chosen, so a fallback from a mistyped name is distinguishable from the
	 * default; both land on `sandbox`.
	 */
	enum class EScenarioSelection : uint8
	{
		/** A `Scenario=` option on the URL named this row. */
		ByOption,

		/** No option, and the map name named this row. */
		ByMapName,

		/** Nothing named anything, so the default row was taken deliberately. */
		Default,

		/** A `Scenario=` option named a row that does not exist; the default is a fallback. */
		OptionNamedNoScenario,
	};

	/**
	 * Which scenario a level should build. Always a valid row, never INDEX_NONE.
	 *
	 * @param Options  URL options, `?Key=Value?Key=Value`, as in `AGameModeBase::OptionsString`.
	 * @param MapName  `UWorld::GetMapName()`: `UEDPIE_0_`-prefixed in PIE, else a long package path.
	 */
	int32 IndexForOptionsAndMap(
		const FString& Options,
		const FString& MapName,
		EScenarioSelection& OutHow);

	/**
	 * Lay a row's structure and resolve its cut bricks. Refuses, writing nothing, if a cut centre
	 * names no brick; a dropped cut would look like a wall that correctly stood.
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

	/** Where to stand so the whole structure is in frame, head-on or three-quarter. */
	FViewpoint ViewpointFor(
		const FBox& BoundsCm,
		double AspectHeightOverWidth,
		EScenarioFraming Framing = EScenarioFraming::HeadOn);
}
