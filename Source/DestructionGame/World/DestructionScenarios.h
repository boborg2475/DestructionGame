// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Layout.h"

/**
 * THE NAMED CATALOGUE OF PLAYABLE DESTRUCTION SCENARIOS — the fixtures the suite has been
 * proving headlessly, addressable as levels a human can join and watch.
 *
 * WORLD-FREE, exactly like Core/Layout. Boxes and doubles: no UWorld, no actors, no
 * UObject anywhere. That is what makes the catalogue's own tests part of the fast suite
 * rather than a world harness, and it is what lets the game mode be the only thing that
 * knows a level exists.
 *
 * ONE DIRECTION OF INCLUSION. This includes Core/Layout.h and the profile headers, and
 * nothing from Tests/. A test may reach the other way — asserting that a row's wall is the
 * same wall a world-free fixture measures is exactly the drift check that keeps a level
 * honest — but production may not.
 */
namespace DestructionScenarios
{
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
		 * WHAT LAYS THIS ROW, WHEN A RUNNING BOND CANNOT — the producer as DATA on the row.
		 *
		 * Unset means the `Wall` spec above, laid by `RunningBond`. A corbel steps half a cell per
		 * course off an immovable base, which `RunningBond` cannot express at all, so its row
		 * carries the call that lays it instead.
		 *
		 * A FUNCTION AND NOT AN ENUM WITH A SWITCH IN `Build`. Scenarios are data in this project
		 * for the same reason materials and connection types are: a switch grows a branch per
		 * family, and every family added afterwards pays for every family already there. A row that
		 * carries its own producer is one row of a table, and `Build` never learns what a corbel is.
		 */
		TFunction<bool(DestructionLayout::FBrickLayout&)> LayStructure;

		/** Centres of the bricks removed AFTER the player has had a look, cm. */
		TArray<FVector> CutCentresCm;

		/**
		 * HOW LONG THE LEVEL HOLDS THIS STRUCTURE EXACTLY AS LAID BEFORE IT RUNS, seconds.
		 *
		 * ONE CLOCK, ON EVERY ROW, AND THE NAME IS NOT THE CUT'S. It was `CutDelaySeconds`, which
		 * lied on seven of the nine rows: a corbel cuts nothing at all, so a field named for the
		 * cut said "four seconds until the thing that never happens". The same class of dishonesty
		 * as the countdown-to-nothing the label already had taken out of it.
		 *
		 * WHAT "HOLD" MEANS IS AS LAID, WHICH IS NOT THE SAME AS UNSOLVED. Every brick spawns
		 * kinematic and only `ABrickActor::Release` makes it dynamic, so holding is simply not
		 * releasing anything — but the structure is still SOLVED while it is held, because
		 * `EPieceSupport::Falling` is what an ABSENT answer reads as and a level whose strain
		 * readout said every brick was falling would be lying in the other direction. Solved and
		 * not settled: no joint has been asked to give yet.
		 *
		 * AND AT THIS MOMENT THE LEVEL RUNS: it applies whatever cuts the row names, and it
		 * SETTLES — every joint over its own capacity gives, and what the solver stops holding up
		 * is handed to physics. A row with no cut still has a moment; that is the whole point of
		 * a corbel level, whose story is AS LAID versus SETTLED and which until now had already
		 * collapsed before the player's first frame was drawn.
		 */
		double HoldSeconds = 4.0;
	};

	/** Every scenario, in menu order. */
	const TArray<FScenario>& Catalogue();

	/** The row with this name, or INDEX_NONE. */
	int32 IndexOfName(FName Name);

	/** The row whose map this is, case-insensitively, or INDEX_NONE. */
	int32 IndexOfMapName(const FString& MapName);

	/**
	 * HOW A SCENARIO CAME TO BE CHOSEN — so a FALLBACK is never mistaken for a default.
	 *
	 * The index alone cannot carry this. A URL naming a scenario that does not exist and a URL
	 * naming nothing at all both end up on `sandbox`, and the player must be TOLD which of the
	 * two happened: the first is a typo they can fix if something names the rows that do exist,
	 * and the second is the game opening normally. Indistinguishable, they are a player staring
	 * at the wrong wall wondering why.
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
	 * ALWAYS A VALID ROW, never INDEX_NONE: a level showing an empty world is a worse failure
	 * than a level showing the default wall.
	 *
	 * @param Options  the URL options string, `?Key=Value?Key=Value`, as
	 *                 `AGameModeBase::OptionsString` carries it.
	 * @param MapName  the map, as `UWorld::GetMapName()` gives it — which carries a `UEDPIE_0_`
	 *                 prefix in PIE and can be a long package path elsewhere.
	 * @param OutHow   how the answer was reached. See EScenarioSelection.
	 */
	int32 IndexForOptionsAndMap(
		const FString& Options,
		const FString& MapName,
		EScenarioSelection& OutHow);

	/**
	 * Lay a row's wall and resolve the bricks its cut names.
	 *
	 * REFUSES A CUT CENTRE THAT NAMES NO BRICK, writing nothing: a silently dropped cut is a
	 * level that looks intact, never does anything, and is indistinguishable from a level
	 * whose wall correctly stood.
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

	/** Where to stand so the whole of a structure is in frame. */
	FViewpoint ViewpointFor(const FBox& BoundsCm, double AspectHeightOverWidth);
}
