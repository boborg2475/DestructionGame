// Copyright Epic Games, Inc. All Rights Reserved.

#include "World/ScenarioLabel.h"

/*
 * File-local names carry a ScenarioLabel prefix and sit in the named namespace rather than an
 * anonymous one: an anonymous namespace is private to a translation unit, not a file, and a
 * unity build merges many files into one, so colliding file-local names become a hard compile
 * error between files that never refer to each other. See CURRENT_STATE.md.
 */
namespace DestructionScenarios
{
	/**
	 * What a row with no cut says, and it says the same thing forever.
	 *
	 * One string with no number in it: every corbel and the sandbox are condemned or safe by
	 * how they were laid, so there is nothing for a clock to count towards. A countdown
	 * changes as time passes, so a line that cannot change can never be mistaken for one.
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
	 * The countdown, with the seconds in it, which makes it a clock rather than a caption.
	 * One decimal, so the line changes every tenth of a second and reads as running.
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
		 * A row that names nothing still reads, and claims nothing: an empty banner reads as a
		 * broken readout, and one carrying some other row's title over the wrong wall is worse
		 * — it is believed.
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

		/*
		 * The build plot has nothing to report, so it reports nothing. Every other row says
		 * something about a cut because there is a building to say it about; on a sandbox
		 * nothing has been laid, and the row's own Expectation already says nothing is cut here.
		 *
		 * An absence rather than a third sentence — FPieceMenuInspector::InspectedHintText's
		 * rule one document over: a line not drawn differs from one drawn saying nothing
		 * happens. SESSION_UI_DESIGN.md §f draws this banner with the title and expectation and
		 * nothing under them.
		 *
		 * Checked before the no-cut arm rather than inside it: the two coincide today only
		 * because a sandbox lays nothing, and will stop the moment a plot can come pre-laid.
		 */
		if (Row.bBuildSandbox)
		{
			return Label;
		}

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
		 * Clamped into the delay the row actually waits, both guards written against the
		 * degenerate value rather than for the good one. The number arrives from a clock
		 * somebody else owns: a late tick can hand back a negative remainder, an unarmed timer
		 * answers -1, and a torn-down world can answer a NaN. `!(X > 0.0)` catches a NaN in the
		 * floor the way `FMath::Max`/`Min` would, and the ceiling is written `Delay < Remaining`
		 * so a NaN delay leaves the caller's finite value alone rather than overwriting it —
		 * every comparison against NaN is false.
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
