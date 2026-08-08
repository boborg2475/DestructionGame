// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "World/DestructionScenarios.h"

/**
 * WHAT THE LABEL ON SCREEN SAYS: which scenario this is, what to watch for, and where the cut is.
 *
 * THE PRESENTER PATTERN Core/PieceMenu.h ALREADY ESTABLISHES. The label's CONTENT is a pure
 * function of the row, the seconds still to wait and whether the cut has fired, so every word and
 * every number a player will read is reachable from a headless test; what is left for Slate is
 * drawing the strings. A widget that composed its own sentence would be holding the decision in
 * the one place no test can reach.
 */
namespace DestructionScenarios
{
	/**
	 * WHERE A SCENARIO'S CUT IS: not coming at all, coming, or done.
	 *
	 * `NoCut` IS ENUMERATOR ZERO, for the reason `EPieceSupport::Falling` is: zero has to be the
	 * value that promises least, and both of the others assert that this level takes a brick out.
	 * A default-constructed label must not put a countdown on screen.
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
		 * The cut line: a countdown, a report that it has fired, or an honest statement that this
		 * scenario cuts nothing at all. NEVER EMPTY, and never a countdown to something that will
		 * not happen — a corbel's story is the settle, and a clock ticking down to nothing is a lie.
		 */
		FString CutText;

		/** Which of the three CutText is. The widget colours from this, never from the words. */
		EScenarioCutState CutState = EScenarioCutState::NoCut;

		/**
		 * Seconds still to wait, ALWAYS FINITE AND NEVER NEGATIVE, and zero when nothing is coming.
		 *
		 * The clamp is the fail-closed half: `FMath::Max` discards a NaN and `FMath::Min` replaces
		 * one, so an unguarded countdown turns a NaN into a plausible-looking number on screen.
		 */
		double SecondsUntilCut = 0.0;
	};

	/**
	 * Build the label for a scenario row at a moment in its life.
	 *
	 * @param ScenarioRow       the row the game mode RECORDED building — never the one an option
	 *                          asked for. Those differ exactly when the player mistyped, which is
	 *                          the case where a wrong label is worst.
	 * @param SecondsUntilCut   how long the armed cut still has, as the caller measured it.
	 * @param bCutHasFired      whether the cut has already run.
	 *
	 * IT FAILS CLOSED ON A ROW THAT NAMES NOTHING and on a time that is not a number: the label is
	 * still readable, still finite, and still claims no cut.
	 */
	FScenarioLabel BuildScenarioLabel(
		int32 ScenarioRow,
		double SecondsUntilCut,
		bool bCutHasFired);
}
