// Copyright Epic Games, Inc. All Rights Reserved.

#include "World/ScenarioLabel.h"

/*
 * File-local names carry a ScenarioLabel prefix and sit in the NAMED namespace rather than in an
 * anonymous one. An anonymous namespace is private to a TRANSLATION UNIT rather than to a file,
 * and a unity build merges many files into one — so two file-local names that collide are a hard
 * compile error between files that never refer to each other. See CURRENT_STATE.md.
 */
namespace DestructionScenarios
{
	/**
	 * WHAT A ROW WITH NO CUT SAYS, AND IT SAYS THE SAME THING FOREVER.
	 *
	 * ONE STRING WITH NO NUMBER IN IT, which is the whole requirement rather than a wording choice:
	 * every corbel and the sandbox are condemned or safe by how they were laid, so there is nothing
	 * for a clock to count towards. A countdown CHANGES as time passes, so a line that cannot change
	 * cannot be mistaken for one — and this is the only line here that names no cut, so it can never
	 * collide with a cutting row's.
	 */
	const TCHAR* const ScenarioLabelNoCutLine =
		TEXT("Nothing is cut here — what you are watching is how it was laid.");

	/** Once the brick is out. Distinct from every countdown, so a player who looked away can tell. */
	const TCHAR* const ScenarioLabelFiredLine =
		TEXT("The brick is out — what the rest of it does now is the answer.");

	/** A row index that names nothing: readable, and borrowing no real scenario's title. */
	const TCHAR* const ScenarioLabelNoRowTitle = TEXT("No scenario");

	const TCHAR* const ScenarioLabelNoRowExpectation =
		TEXT("This level names no scenario in the catalogue, so nothing here says what it should do.");

	/**
	 * The countdown, with the seconds in it — which is what makes it a clock rather than a caption.
	 *
	 * ONE DECIMAL, so the line changes on every tenth of a second and a player can see it running.
	 */
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

		/*
		 * A ROW THAT NAMES NOTHING STILL READS, AND CLAIMS NOTHING. An empty banner reads as a
		 * readout that broke, and a banner confidently carrying some other row's title over the
		 * wrong wall is worse than either — it is believed.
		 */
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

		/* A row that names no brick has no clock, whatever the caller measured. */
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
		 * AND THE COUNTDOWN IS CLAMPED INTO THE DELAY THE ROW ACTUALLY WAITS, WITH BOTH GUARDS
		 * WRITTEN AGAINST THE DEGENERATE VALUE RATHER THAN FOR THE GOOD ONE.
		 *
		 * The number arrives from a clock somebody else owns: a late tick hands over a NEGATIVE
		 * remainder, a timer that was never armed answers -1, and a world torn down mid-delay can
		 * answer a NaN. `!(X > 0.0)` puts a NaN INSIDE the floor, where FMath::Max would have
		 * discarded it and FMath::Min replaced it — either way turning it into a plausible-looking
		 * number on screen for a clock that is not running.
		 *
		 * The ceiling is then written the other way round, `Delay < Remaining`, so that a row whose
		 * OWN delay was somehow not a number leaves the caller's finite value alone rather than
		 * copying the NaN over it. Every comparison against a NaN is false, so whichever operand
		 * carries one, no clamp happens and what reaches the screen is still a number.
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
