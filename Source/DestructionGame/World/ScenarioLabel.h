// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "World/DestructionScenarios.h"

/**
 * The on-screen scenario label: title, what to watch for, and the cut. A pure function of row,
 * seconds remaining and fired state (the Core/PieceMenu.h presenter pattern), so it is testable
 * headless.
 */
namespace DestructionScenarios
{
	/** Cut state. NoCut is zero so a default label never shows a countdown. */
	enum class EScenarioCutState : uint8
	{
		/** The row cuts nothing (corbels, the sandbox). */
		NoCut,

		/** A cut is counting down. */
		Armed,

		/** The cut has fired. */
		Fired,
	};

	/** The scenario label as displayed. */
	struct FScenarioLabel
	{
		/** The row's Title, verbatim. Never empty, even for an unknown row. */
		FString TitleText;

		/** The row's Expectation: what to watch for. Never empty. */
		FString ExpectationText;

		/**
		 * The cut line: countdown, fired, or no cut. Empty only for a build sandbox (a two-line
		 * banner); elsewhere empty is a defect. World.Scenarios.Label asserts both.
		 */
		FString CutText;

		/** Which kind CutText is. The widget colours from this, not the words. */
		EScenarioCutState CutState = EScenarioCutState::NoCut;

		/** Seconds remaining: finite, non-negative, zero when no cut is coming. NaN is guarded explicitly. */
		double SecondsUntilCut = 0.0;
	};

	/**
	 * Build the label for a scenario row.
	 *
	 * @param ScenarioRow      The row the game mode actually built, not the one requested (they
	 *                         differ on a typo).
	 * @param SecondsUntilCut  Time left on the armed cut.
	 * @param bCutHasFired     Whether the cut has run.
	 *
	 * Fails closed on an unknown row or a NaN time: readable, finite, and no cut claimed.
	 */
	FScenarioLabel BuildScenarioLabel(
		int32 ScenarioRow,
		double SecondsUntilCut,
		bool bCutHasFired);
}
