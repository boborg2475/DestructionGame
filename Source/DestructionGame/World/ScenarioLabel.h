// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "World/DestructionScenarios.h"

/**
 * The label on screen: which scenario this is, what to watch for, and where the cut is.
 *
 * The presenter pattern Core/PieceMenu.h already establishes: content is a pure function of
 * the row, seconds still to wait, and whether the cut has fired, so every word a player reads
 * is reachable from a headless test. A widget composing its own sentence would hold the
 * decision where no test can reach it.
 */
namespace DestructionScenarios
{
	/**
	 * Where a scenario's cut is: not coming at all, coming, or done.
	 *
	 * `NoCut` is enumerator zero, as with `EPieceSupport::Falling`: zero must be the value
	 * that promises least, so a default-constructed label never shows a countdown.
	 */
	enum class EScenarioCutState : uint8
	{
		/** This row cuts nothing — every corbel, and the sandbox. There is no clock. */
		NoCut,

		/** A cut is armed and the player is being counted down to it. */
		Armed,

		/** The cut has fired. What is on screen now is the answer. */
		Fired,
	};

	/** The scenario label, in the words and numbers a human reads. */
	struct FScenarioLabel
	{
		/** The row's own Title, verbatim. Never empty, including for a row that does not exist. */
		FString TitleText;

		/** The row's own Expectation — the one line saying what to watch for. Never empty. */
		FString ExpectationText;

		/**
		 * The cut line: a countdown, a report that it has fired, or a statement that this
		 * scenario cuts nothing. Never a countdown to something that will not happen.
		 *
		 * Empty on exactly one kind of row, and it is a claim, not a gap: a build sandbox has
		 * nothing laid on it, so the banner is two lines rather than three. Everywhere else a
		 * blank line reads as a broken readout — World.Scenarios.Label asserts both directions.
		 */
		FString CutText;

		/** Which of the three CutText is. The widget colours from this, never from the words. */
		EScenarioCutState CutState = EScenarioCutState::NoCut;

		/**
		 * Seconds still to wait — always finite and never negative, zero when nothing is coming.
		 *
		 * Fail-closed: `FMath::Max` discards a NaN and `FMath::Min` replaces one, so an
		 * unguarded countdown would otherwise turn a NaN into a plausible number on screen.
		 */
		double SecondsUntilCut = 0.0;
	};

	/**
	 * Build the label for a scenario row at a moment in its life.
	 *
	 * @param ScenarioRow      the row the game mode recorded building — never the one an option
	 *                         asked for. Those differ exactly when the player mistyped.
	 * @param SecondsUntilCut  how long the armed cut still has, as the caller measured it.
	 * @param bCutHasFired     whether the cut has already run.
	 *
	 * Fails closed on a row that names nothing, and on a time that is not a number: the label
	 * stays readable, finite, and claims no cut.
	 */
	FScenarioLabel BuildScenarioLabel(
		int32 ScenarioRow,
		double SecondsUntilCut,
		bool bCutHasFired);
}
