// Copyright Epic Games, Inc. All Rights Reserved.

#include "World/ScenarioLabel.h"

/*
 * File-local names use a ScenarioLabel prefix inside the named namespace, not an anonymous one,
 * so they cannot collide in a unity build.
 */
namespace DestructionScenarios
{
	/** The fixed line for a row with no cut; it never changes, so it cannot read as a countdown. */
	const TCHAR* const ScenarioLabelNoCutLine =
		TEXT("Nothing is cut here — what you are watching is how it was laid.");

	/** The line after the cut, distinct from every countdown. */
	const TCHAR* const ScenarioLabelFiredLine =
		TEXT("The brick is out — what the rest of it does now is the answer.");

	/** For an invalid row: readable, and borrowing no real scenario's title. */
	const TCHAR* const ScenarioLabelNoRowTitle = TEXT("No scenario");

	const TCHAR* const ScenarioLabelNoRowExpectation =
		TEXT("This level names no scenario in the catalogue, so nothing here says what it should do.");

	/** The countdown line, to one decimal so it visibly runs. */
	static FString ScenarioLabelCountdownLine(double SecondsUntilCut)
	{
		return FString::Printf(
			TEXT("A brick comes out in %.1f s — watch what the rest of the wall does."),
			SecondsUntilCut);
	}

	FScenarioLabel BuildScenarioLabel(
		int32 ScenarioRow,
		double SecondsUntilCut,
		bool bCutHasFired)
	{
		FScenarioLabel Label;

		const TArray<FScenario>& Rows = Catalogue();

		// Fail closed: an invalid row still reads, but claims no scenario and no cut.
		if (!Rows.IsValidIndex(ScenarioRow))
		{
			Label.TitleText = ScenarioLabelNoRowTitle;
			Label.ExpectationText = ScenarioLabelNoRowExpectation;
			Label.CutText = ScenarioLabelNoCutLine;

			return Label;
		}

		const FScenario& Row = Rows[ScenarioRow];

		Label.TitleText = Row.Title;
		Label.ExpectationText = Row.Expectation;

		/*
		 * The build plot has no cut line: nothing is laid yet (SESSION_UI_DESIGN.md §f). Checked
		 * before the no-cut arm, since a pre-laid plot would no longer coincide with it.
		 */
		if (Row.bBuildSandbox)
		{
			return Label;
		}

		// No cut, no clock.
		if (Row.CutCentresCm.Num() == 0)
		{
			Label.CutText = ScenarioLabelNoCutLine;

			return Label;
		}

		if (bCutHasFired)
		{
			Label.CutState = EScenarioCutState::Fired;
			Label.CutText = ScenarioLabelFiredLine;

			return Label;
		}

		/*
		 * Clamp to [0, HoldSeconds]. Callers can pass negative, -1 or NaN. `!(X > 0.0)` sends NaN
		 * to 0; `Delay < Remaining` leaves a finite value alone if the delay is NaN.
		 */
		double RemainingSeconds = SecondsUntilCut;

		if (!(RemainingSeconds > 0.0))
		{
			RemainingSeconds = 0.0;
		}

		if (Row.HoldSeconds < RemainingSeconds)
		{
			RemainingSeconds = Row.HoldSeconds;
		}

		Label.CutState = EScenarioCutState::Armed;
		Label.CutText = ScenarioLabelCountdownLine(RemainingSeconds);
		Label.SecondsUntilCut = RemainingSeconds;

		return Label;
	}
}
