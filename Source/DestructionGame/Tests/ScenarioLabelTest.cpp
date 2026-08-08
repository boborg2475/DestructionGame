// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "DestructionGameGameMode.h"
#include "HAL/PlatformTime.h"
#include "Tests/BrickWorldTestSupport.h"
#include "World/DestructionScenarios.h"
#include "World/ScenarioLabel.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE PLAYER IS TOLD WHAT THEY ARE LOOKING AT: WHICH SCENARIO, WHAT TO WATCH FOR, AND WHERE THE
 * CUT IS.
 *
 * =====================================================================================
 * THE GAP, WHICH IS THE OTHER HALF OF "OBSERVE WHAT HAPPENS"
 * =====================================================================================
 *
 * Joining a scenario level today gives you a wall and nothing else. Nothing says which of the
 * nine rows it is, nothing says what it is supposed to do, and nothing says that in four seconds
 * a brick is going to vanish. Two of the nine walls are seven metres of running bond that differ
 * only in which brick goes, and seven of them are corbels that differ by one course — so a human
 * watching cannot tell a correct level from a broken one, which is the entire point of making
 * these joinable in the first place.
 *
 * =====================================================================================
 * A PRESENTER, EXACTLY AS Core/PieceMenu.h IS ONE
 * =====================================================================================
 *
 * The label's CONTENT is a pure function of (which row, seconds still to wait, has it fired), so
 * it is tested as arithmetic and Slate is left with nothing to do but draw strings. Nothing here
 * scrapes a viewport: a test that hunted a rendered frame for words would be slow, would need an
 * RHI the automation run does not have, and would pass against a label drawn off the bottom of
 * the screen.
 *
 * =====================================================================================
 * WHAT IS ASSERTED, AND WHY IT IS NOT THE WORDING
 * =====================================================================================
 *
 * `Tests/ScenarioReportTest.cpp` already records why matching on TEXT is fragile — a reworded
 * string breaks a test that was never about the wording. So the claims here are STRUCTURAL, and
 * each one bites:
 *
 *   - THE TITLE AND THE EXPECTATION ARE THE ROW'S OWN, verbatim and by exact equality. Those two
 *     strings already exist in the catalogue and are already asserted non-empty on every row, so
 *     "the label carries them" is a claim with no wording in it at all.
 *
 *   - A ROW WITH NO CUT SAYS THE SAME THING WHATEVER THE CLOCK DOES. That is the honest-instead-
 *     of-counting-down requirement, stated as a property rather than as a sentence: a countdown
 *     to nothing CHANGES as time passes, so text that is invariant across every time and both
 *     fired states cannot be one. Every corbel and the sandbox are in this case.
 *
 *   - A ROW WITH A CUT SAYS SOMETHING DIFFERENT AT DIFFERENT TIMES, and something different again
 *     once it has fired. That is "it counts down and then reports", with no format pinned.
 *
 *   - AND A NO-CUT ROW'S LINE IS NONE OF THE CUT ROWS' LINES. Without this the three claims above
 *     are all satisfied by a presenter that writes "cut in 4 s" onto a corbel, which is the exact
 *     lie this whole readout exists not to tell.
 *
 * The STATE and the NUMBER sit beside the words for the reason `EJointMarginBand` sits beside
 * `MarginText`: a widget colouring a countdown by comparing the sentence against a string literal
 * would be a policy in the one place no test can reach.
 *
 * NEEDS A TICKING WORLD: NO. Nor a world at all. A row index, a double and a bool in; strings,
 * an enum and a double out.
 */
namespace ScenarioLabelTestSupport
{
	using namespace DestructionScenarios;

	inline FString ScenarioLabelBits(double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	inline FString ScenarioLabelStateName(EScenarioCutState State)
	{
		switch (State)
		{
		case EScenarioCutState::NoCut: return TEXT("NoCut");
		case EScenarioCutState::Armed: return TEXT("Armed");
		case EScenarioCutState::Fired: return TEXT("Fired");
		default:                       return TEXT("<unknown>");
		}
	}

	inline FString ScenarioLabelRowName(int32 Index)
	{
		return Catalogue().IsValidIndex(Index)
			? Catalogue()[Index].Name.ToString()
			: FString::Printf(TEXT("<no row: %d>"), Index);
	}

	/** A real IEEE NaN, produced the way Tests/LayoutTest.cpp produces one. */
	inline double ScenarioLabelMakeNaN()
	{
		volatile double Zero = 0.0;
		return Zero / Zero;
	}

	/** One moment in a scenario's life, as the caller would measure it. */
	struct FScenarioLabelMoment
	{
		const TCHAR* Why;

		/** Seconds still to wait, as a MULTIPLE of the row's own delay, so rows may retune it. */
		double DelayFraction;

		/** Or a value that is not a multiple of anything — see bIsAbsolute. */
		double AbsoluteSeconds;

		/** True to use AbsoluteSeconds. The degenerate rows are absolute; the rest scale. */
		bool bIsAbsolute;
	};

	/** The row with this name, or null with the reason reported. */
	inline const FScenario* ScenarioLabelRowNamed(FAutomationTestBase& Test, const TCHAR* Name)
	{
		const int32 Index = IndexOfName(FName(Name));

		if (!Catalogue().IsValidIndex(Index))
		{
			Test.AddError(FString::Printf(
				TEXT("fixture: the catalogue must carry a row named '%s'"), Name));

			return nullptr;
		}

		return &Catalogue()[Index];
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScenarioLabelTest,
	"DestructionGame.World.Scenarios.Label",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FScenarioLabelTest::RunTest(const FString& Parameters)
{
	using namespace ScenarioLabelTestSupport;
	using namespace DestructionScenarios;

	const TArray<FScenario>& Rows = Catalogue();

	/*
	 * A FLOOR, so a catalogue that emptied fails here rather than turning the sweep below into a
	 * loop over nothing that passes in silence. Nine is what slices A to C left, and the sweep
	 * NEEDS BOTH KINDS: at least one row that cuts and at least one that does not, or half the
	 * claims in this file are vacuous. Both are checked explicitly under it.
	 */
	constexpr int32 ScenarioLabelRowFloor = 9;

	TestTrue(
		*FString::Printf(
			TEXT("fixture: the catalogue must carry at least the %d rows slices A to C left; it ")
			TEXT("carries %d"),
			ScenarioLabelRowFloor, Rows.Num()),
		Rows.Num() >= ScenarioLabelRowFloor);

	int32 RowsThatCut = 0;
	int32 RowsThatDoNot = 0;

	for (const FScenario& Row : Rows)
	{
		(Row.CutCentresCm.Num() > 0 ? RowsThatCut : RowsThatDoNot) += 1;
	}

	TestTrue(
		*FString::Printf(
			TEXT("fixture: the catalogue must carry BOTH kinds of row for this to mean anything — ")
			TEXT("%d cut something and %d cut nothing"),
			RowsThatCut, RowsThatDoNot),
		RowsThatCut >= 1 && RowsThatDoNot >= 1);

	const double NotANumber = ScenarioLabelMakeNaN();

	/*
	 * THE MOMENTS, AND THE DEGENERATE ONES ARE NOT DECORATION. A countdown is arithmetic on a
	 * clock somebody else owns: a late tick hands over a NEGATIVE remainder, a timer that was
	 * never armed hands over whatever its manager answers for an unknown handle, and a NaN passes
	 * straight through FMath::Max and is replaced by FMath::Min — so an unguarded label puts a
	 * plausible-looking number on screen for a clock that is not running.
	 */
	const FScenarioLabelMoment Moments[] =
	{
		{ TEXT("the whole delay, as the level begins"),      1.0,   0.0, false },
		{ TEXT("half the delay gone"),                       0.5,   0.0, false },
		{ TEXT("a quarter of the delay left"),               0.25,  0.0, false },
		{ TEXT("one second left"),                           0.0,   1.0, true },
		{ TEXT("no time left at all"),                       0.0,   0.0, true },
		{ TEXT("a late tick, past the deadline"),            0.0,  -1.0, true },
		{ TEXT("further past it than the delay itself"),    -2.0,   0.0, false },
		{ TEXT("longer than any level will ever run"),       0.0,   1.0e9, true },
		{ TEXT("no clock at all"),                           0.0,   NotANumber, true },
	};

	int32 SweptCases = 0;

	/*
	 * ONE ERROR PER INVARIANT, NOT ONE PER CASE, which is the shape Tests/StructureFuzzTest.cpp
	 * and Tests/ScenarioSelectionTest.cpp already use. Nine rows times nine moments times two
	 * fired states is 162 labels, and a presenter that returns nothing at all fails most of the
	 * assertions on every one of them — five hundred identical errors that bury the two or three
	 * that say something different. So the FIRST of each kind is reported in full and the rest are
	 * counted, and the counts are what is asserted at the end.
	 */
	int32 TitleFailures = 0;
	int32 ExpectationFailures = 0;
	int32 BlankLineFailures = 0;
	int32 CountdownRangeFailures = 0;
	int32 StateFailures = 0;
	int32 IdleCountdownFailures = 0;
	int32 ArmedCountdownFailures = 0;

	/*
	 * THE LINES EACH KIND OF ROW PRODUCES, collected so the cross-check below can hold them
	 * against each other. A no-cut row's line matching any cut row's is the failure this whole
	 * readout exists to stop — a corbel with a clock ticking down to nothing.
	 */
	TSet<FString> NoCutLines;
	TSet<FString> CutLines;

	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		const FScenario& Row = Rows[Index];

		const bool bRowCuts = Row.CutCentresCm.Num() > 0;

		const FString RowLabel =
			FString::Printf(TEXT("row %d ('%s')"), Index, *Row.Name.ToString());

		/* Every line this row produces across every moment: they must all be one string if it cuts nothing. */
		TSet<FString> LinesForThisRow;

		for (const FScenarioLabelMoment& Moment : Moments)
		{
			const double SecondsIn = Moment.bIsAbsolute
				? Moment.AbsoluteSeconds
				: Moment.DelayFraction * Row.HoldSeconds;

			for (int32 FiredCase = 0; FiredCase < 2; ++FiredCase)
			{
				++SweptCases;

				const bool bFired = FiredCase == 1;

				const FScenarioLabel Label = BuildScenarioLabel(Index, SecondsIn, bFired);

				const FString Where = FString::Printf(
					TEXT("%s at %s (%s s in, fired = %s)"),
					*RowLabel, Moment.Why, *ScenarioLabelBits(SecondsIn),
					bFired ? TEXT("true") : TEXT("false"));

				/* --- the title and the expectation are the row's own, verbatim ------------- */

				if (Label.TitleText != FString(Row.Title) || Label.TitleText.IsEmpty())
				{
					if (TitleFailures == 0)
					{
						AddError(FString::Printf(
							TEXT("%s: the label must carry the row's OWN title, '%s'; it carries ")
							TEXT("'%s'"),
							*Where, Row.Title, *Label.TitleText));
					}

					++TitleFailures;
				}

				if (Label.ExpectationText != FString(Row.Expectation)
					|| Label.ExpectationText.IsEmpty())
				{
					if (ExpectationFailures == 0)
					{
						AddError(FString::Printf(
							TEXT("%s: the label must carry the row's OWN expectation — the one ")
							TEXT("line saying what a human should watch for — '%s'; it carries ")
							TEXT("'%s'"),
							*Where, Row.Expectation, *Label.ExpectationText));
					}

					++ExpectationFailures;
				}

				/* --- the cut line is always there, and the number behind it is always sane -- */

				if (Label.CutText.IsEmpty())
				{
					if (BlankLineFailures == 0)
					{
						AddError(FString::Printf(
							TEXT("%s: the cut line must never be blank — a hole where the state of ")
							TEXT("the level should be reads as a readout that failed"),
							*Where));
					}

					++BlankLineFailures;
				}

				if (!FMath::IsFinite(Label.SecondsUntilCut)
					|| Label.SecondsUntilCut < 0.0
					|| Label.SecondsUntilCut > Row.HoldSeconds)
				{
					if (CountdownRangeFailures == 0)
					{
						AddError(FString::Printf(
							TEXT("%s: the countdown must be FINITE and never negative, and never ")
							TEXT("more than the %s s this row waits; it reads %s"),
							*Where, *ScenarioLabelBits(Row.HoldSeconds),
							*ScenarioLabelBits(Label.SecondsUntilCut)));
					}

					++CountdownRangeFailures;
				}

				/* --- and the state says which of the three lines it is --------------------- */

				const EScenarioCutState ExpectedState = !bRowCuts
					? EScenarioCutState::NoCut
					: (bFired ? EScenarioCutState::Fired : EScenarioCutState::Armed);

				if (Label.CutState != ExpectedState)
				{
					if (StateFailures == 0)
					{
						AddError(FString::Printf(
							TEXT("%s: this row cuts %d brick(s), so its state must be %s; it is %s"),
							*Where, Row.CutCentresCm.Num(),
							*ScenarioLabelStateName(ExpectedState),
							*ScenarioLabelStateName(Label.CutState)));
					}

					++StateFailures;
				}

				/*
				 * NOTHING LEFT TO WAIT FOR ONCE THERE IS NOTHING COMING. A state that promises no
				 * cut beside a number that says how long until one is the readout disagreeing with
				 * itself, and the widget draws both.
				 */
				if (Label.CutState != EScenarioCutState::Armed && Label.SecondsUntilCut != 0.0)
				{
					if (IdleCountdownFailures == 0)
					{
						AddError(FString::Printf(
							TEXT("%s: %s claims nothing is on its way, so the countdown must read ")
							TEXT("0; it reads %s"),
							*Where, *ScenarioLabelStateName(Label.CutState),
							*ScenarioLabelBits(Label.SecondsUntilCut)));
					}

					++IdleCountdownFailures;
				}

				/*
				 * AND AN ARMED COUNTDOWN IS THE CALLER'S OWN NUMBER, unchanged, whenever that
				 * number is one a clock could actually produce. Only the out-of-range and
				 * non-finite moments are clamped — see the bound asserted above, which covers
				 * those without dictating WHICH end of the range a NaN lands on.
				 */
				if (Label.CutState == EScenarioCutState::Armed
					&& FMath::IsFinite(SecondsIn)
					&& SecondsIn >= 0.0 && SecondsIn <= Row.HoldSeconds
					&& Label.SecondsUntilCut != SecondsIn)
				{
					if (ArmedCountdownFailures == 0)
					{
						AddError(FString::Printf(
							TEXT("%s: an armed countdown must report the %s s it was given; it ")
							TEXT("reports %s"),
							*Where, *ScenarioLabelBits(SecondsIn),
							*ScenarioLabelBits(Label.SecondsUntilCut)));
					}

					++ArmedCountdownFailures;
				}

				LinesForThisRow.Add(Label.CutText);

				(bRowCuts ? CutLines : NoCutLines).Add(Label.CutText);
			}
		}

		/* --- the property that makes "honest" mean something ------------------------------- */

		if (!bRowCuts)
		{
			/*
			 * ONE LINE, WHATEVER THE CLOCK SAYS. A corbel is condemned by its own geometry and its
			 * whole story is as-laid versus settled — there is no cut to wait for, so there is
			 * nothing for a delay to do. A countdown CHANGES with time by definition, so text that
			 * is identical across every moment and both fired states cannot be one, and this holds
			 * without pinning a single word of it.
			 */
			TestTrue(
				*FString::Printf(
					TEXT("%s CUTS NOTHING, so its line must say so and go on saying it — a clock ")
					TEXT("ticking down to something that will never happen is a lie on screen. It ")
					TEXT("produced %d different line(s) across %d moments: [%s]"),
					*RowLabel, LinesForThisRow.Num(),
					static_cast<int32>(UE_ARRAY_COUNT(Moments)) * 2,
					*FString::Join(LinesForThisRow.Array(), TEXT(" | "))),
				LinesForThisRow.Num() == 1);
		}
		else
		{
			/*
			 * A ROW THAT CUTS COUNTS DOWN AND THEN REPORTS, which is three different lines: the
			 * full delay, one second left, and afterwards. Written as "these three differ" rather
			 * than as three literals, so the format stays the presenter's business.
			 */
			const FScenarioLabel AtStart = BuildScenarioLabel(Index, Row.HoldSeconds, false);
			const FScenarioLabel AlmostThere = BuildScenarioLabel(Index, 1.0, false);
			const FScenarioLabel Afterwards = BuildScenarioLabel(Index, 0.0, true);

			TestTrue(
				*FString::Printf(
					TEXT("%s must COUNT DOWN: with the whole %s s to go it says '%s' and with 1 s ")
					TEXT("to go it says '%s'. Identical, the number on screen is not a clock."),
					*RowLabel, *ScenarioLabelBits(Row.HoldSeconds),
					*AtStart.CutText, *AlmostThere.CutText),
				AtStart.CutText != AlmostThere.CutText);

			TestTrue(
				*FString::Printf(
					TEXT("%s must REPORT once the cut has fired: it says '%s' afterwards against ")
					TEXT("'%s' while waiting. A player who looked away has to be able to tell that ")
					TEXT("it already happened."),
					*RowLabel, *Afterwards.CutText, *AlmostThere.CutText),
				Afterwards.CutText != AlmostThere.CutText
					&& Afterwards.CutText != AtStart.CutText);
		}
	}

	/* --- and the two kinds of line are never the same line ------------------------------- */

	/*
	 * THE CROSS-CHECK, AND IT IS THE ONE THAT STOPS THE OTHERS BEING SATISFIED BY A LIE. Every
	 * claim above holds against a presenter that writes a cut row's countdown onto a corbel and
	 * simply never varies it — one line, invariant, non-empty. Held against the cut rows' lines,
	 * it cannot.
	 */
	for (const FString& NoCutLine : NoCutLines)
	{
		TestTrue(
			*FString::Printf(
				TEXT("a scenario that cuts nothing must not say what a scenario that cuts says: ")
				TEXT("'%s' is a line BOTH kinds of row produce"),
				*NoCutLine),
			!CutLines.Contains(NoCutLine));
	}

	AddInfo(FString::Printf(
		TEXT("swept %d labels over %d rows: %d distinct lines from rows that cut, %d from rows ")
		TEXT("that do not"),
		SweptCases, Rows.Num(), CutLines.Num(), NoCutLines.Num()));

	/* --- the sweep's verdicts, one per invariant ------------------------------------------ */

	TestTrue(
		*FString::Printf(
			TEXT("every one of the %d swept labels must carry its own row's title; %d did not"),
			SweptCases, TitleFailures),
		TitleFailures == 0);

	TestTrue(
		*FString::Printf(
			TEXT("every one of the %d swept labels must carry its own row's expectation; %d did ")
			TEXT("not"),
			SweptCases, ExpectationFailures),
		ExpectationFailures == 0);

	TestTrue(
		*FString::Printf(
			TEXT("every one of the %d swept labels must say SOMETHING about the cut; %d were ")
			TEXT("blank"),
			SweptCases, BlankLineFailures),
		BlankLineFailures == 0);

	TestTrue(
		*FString::Printf(
			TEXT("every countdown must be finite and inside [0, the row's delay] — a negative or ")
			TEXT("a NaN on screen is a clock that is not running; %d of %d were not"),
			CountdownRangeFailures, SweptCases),
		CountdownRangeFailures == 0);

	TestTrue(
		*FString::Printf(
			TEXT("every label must report the cut state its row and its clock actually imply; %d ")
			TEXT("of %d did not"),
			StateFailures, SweptCases),
		StateFailures == 0);

	TestTrue(
		*FString::Printf(
			TEXT("no label that claims nothing is coming may still be counting down; %d of %d ")
			TEXT("were"),
			IdleCountdownFailures, SweptCases),
		IdleCountdownFailures == 0);

	TestTrue(
		*FString::Printf(
			TEXT("every armed countdown must report the time it was given; %d of %d reported ")
			TEXT("something else"),
			ArmedCountdownFailures, SweptCases),
		ArmedCountdownFailures == 0);

	/* --- a row that names nothing fails closed ------------------------------------------- */

	/*
	 * THE LABEL IS A READOUT RATHER THAN A COMMAND, so the fail-closed direction is that it still
	 * says something — an empty banner reads as a readout that broke — while claiming NOTHING it
	 * cannot support. In particular it must not borrow a real scenario's title: a level whose row
	 * index went wrong showing a confident "Corbel F — a hundred steps" is worse than one showing
	 * a blank, because the first is believed.
	 */
	const int32 NoSuchRows[] = { INDEX_NONE, -7, Rows.Num(), Rows.Num() + 100 };

	for (const int32 NoSuchRow : NoSuchRows)
	{
		const FScenarioLabel Label = BuildScenarioLabel(NoSuchRow, 2.0, false);

		TestTrue(
			*FString::Printf(
				TEXT("row %d names no scenario, and the label must still be readable: title '%s', ")
				TEXT("expectation '%s', cut line '%s'"),
				NoSuchRow, *Label.TitleText, *Label.ExpectationText, *Label.CutText),
			!Label.TitleText.IsEmpty() && !Label.ExpectationText.IsEmpty()
				&& !Label.CutText.IsEmpty());

		TestTrue(
			*FString::Printf(
				TEXT("row %d names no scenario, so nothing is armed and nothing is counting: it ")
				TEXT("reads %s with %s s to go"),
				NoSuchRow, *ScenarioLabelStateName(Label.CutState),
				*ScenarioLabelBits(Label.SecondsUntilCut)),
			Label.CutState == EScenarioCutState::NoCut && Label.SecondsUntilCut == 0.0);

		for (const FScenario& Row : Rows)
		{
			TestTrue(
				*FString::Printf(
					TEXT("row %d names no scenario, so its label must NOT claim to be '%s' — a ")
					TEXT("confident title over the wrong wall is worse than a blank one"),
					NoSuchRow, Row.Title),
				Label.TitleText != FString(Row.Title));
		}
	}

	return true;
}

/**
 * AND THE LABEL ON A LEVEL NAMES THE SCENARIO THE GAME MODE ACTUALLY BUILT, COUNTS ITS CUT DOWN,
 * AND SAYS WHEN IT HAS FIRED.
 *
 * =====================================================================================
 * THE RECORDED ROW, NOT THE REQUESTED ONE — WHICH IS THE CASE A WRONG LABEL IS WORST IN
 * =====================================================================================
 *
 * A `?Scenario=` that names nothing falls back to `sandbox` and says so through
 * `EScenarioSelection::OptionNamedNoScenario`, and `World.Scenario.GameModeRecordsWhichScenarioIt
 * Built` already pins that pair. The label is a thin read of it — but "thin read" is exactly
 * where this goes wrong: a label built from the option string, or from the map, would print
 * "One brick out of a free end, under forty courses" over the sandbox wall, and the player would
 * spend four seconds waiting for a cut that is never coming on a wall that is not the one named.
 * So the assertion is the label against `GetSelectedScenarioRow`'s OWN row, and against the other
 * row by name, in both directions.
 *
 * =====================================================================================
 * THE COUNTDOWN IS ASSERTED AS A DECREASE, NOT ONLY AS A NUMBER
 * =====================================================================================
 *
 * A label reading "4 s" forever satisfies any single-sample assertion about the value. So it is
 * read three times through one world — at begin-play, most of the way through the delay, and past
 * it — and the middle reading has to be STRICTLY LESS than the first. That is what makes it a
 * clock rather than a constant.
 *
 * =====================================================================================
 * AND A CORBEL NEVER CLAIMS A CUT IS COMING, HOWEVER LONG IT IS LEFT
 * =====================================================================================
 *
 * Ticked well past the delay every cutting row uses, so "it never counts down" is a claim about
 * ever rather than about yet.
 *
 * NEEDS A TICKING WORLD: YES, and the ticking is the point — the countdown is measured in world
 * seconds. The naming half needs only a world with begin-play run.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScenarioLabelOnALevelTest,
	"DestructionGame.World.Scenario.GameModeLabelsTheScenarioItBuilt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FScenarioLabelOnALevelTest::RunTest(const FString& Parameters)
{
	using namespace ScenarioLabelTestSupport;
	using namespace BrickWorldTestSupport;
	using namespace DestructionScenarios;

	const FScenario* const CutRow = ScenarioLabelRowNamed(*this, TEXT("free-end-40"));
	const FScenario* const DefaultRow = ScenarioLabelRowNamed(*this, TEXT("sandbox"));
	const FScenario* const CorbelRow = ScenarioLabelRowNamed(*this, TEXT("corbel-a-bare-4"));

	if (CutRow == nullptr || DefaultRow == nullptr || CorbelRow == nullptr)
	{
		return true;
	}

	/* --- ONE: a cutting row is named, counted down, and then reported -------------------- */

	{
		FBrickTestWorld TestWorld;

		TestWorld.Wrapper.BeginPlayURL.AddOption(TEXT("Scenario=free-end-40"));

		if (!TestWorld.Begin(*this, ADestructionGameGameMode::StaticClass()))
		{
			return true;
		}

		ADestructionGameGameMode* const GameMode =
			TestWorld.World->GetAuthGameMode<ADestructionGameGameMode>();

		if (GameMode == nullptr)
		{
			AddError(FString::Printf(
				TEXT("fixture: the world must be running ADestructionGameGameMode, it is running %s"),
				*GetNameSafe(TestWorld.World->GetAuthGameMode())));

			TestWorld.End();
			return true;
		}

		const FScenarioLabel AtBeginPlay = GameMode->GetScenarioLabel();

		AddInfo(FString::Printf(
			TEXT("at begin-play the label reads title '%s', %s, %s s to go"),
			*AtBeginPlay.TitleText, *ScenarioLabelStateName(AtBeginPlay.CutState),
			*ScenarioLabelBits(AtBeginPlay.SecondsUntilCut)));

		/*
		 * THE LABEL IS THE RECORDED ROW'S, read back through GetSelectedScenarioRow rather than
		 * assumed — so this stays a statement about the two agreeing even if selection changes.
		 */
		const int32 Recorded = GameMode->GetSelectedScenarioRow();

		TestTrue(
			*FString::Printf(
				TEXT("fixture: ?Scenario=free-end-40 must be recorded as that row; it recorded %d ")
				TEXT("('%s')"),
				Recorded, *ScenarioLabelRowName(Recorded)),
			Catalogue().IsValidIndex(Recorded)
				&& Catalogue()[Recorded].Name == CutRow->Name);

		TestTrue(
			*FString::Printf(
				TEXT("the label must name the scenario the game mode RECORDED — '%s'; it says '%s'"),
				Catalogue().IsValidIndex(Recorded) ? Catalogue()[Recorded].Title : TEXT("<none>"),
				*AtBeginPlay.TitleText),
			Catalogue().IsValidIndex(Recorded)
				&& AtBeginPlay.TitleText == FString(Catalogue()[Recorded].Title));

		TestTrue(
			*FString::Printf(
				TEXT("and it must tell the player what to watch for — '%s'; it says '%s'"),
				CutRow->Expectation, *AtBeginPlay.ExpectationText),
			AtBeginPlay.ExpectationText == FString(CutRow->Expectation));

		/*
		 * AND THE CUT IS ANNOUNCED BEFORE IT HAPPENS. A brick vanishing out of a wall with no
		 * warning is the same picture as a brick vanishing out of a wall that was never armed.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("at begin-play the cut must read Armed with about the whole %s s still to ")
				TEXT("go; it reads %s with %s s"),
				*ScenarioLabelBits(CutRow->HoldSeconds),
				*ScenarioLabelStateName(AtBeginPlay.CutState),
				*ScenarioLabelBits(AtBeginPlay.SecondsUntilCut)),
			AtBeginPlay.CutState == EScenarioCutState::Armed
				&& AtBeginPlay.SecondsUntilCut > CutRow->HoldSeconds - 0.1
				&& AtBeginPlay.SecondsUntilCut <= CutRow->HoldSeconds);

		/* --- most of the way through: still armed, and STRICTLY less time left ------------ */

		const double AlmostTheDelaySeconds = CutRow->HoldSeconds * 0.875;

		const double PartWayStart = FPlatformTime::Seconds();
		TestWorld.TickSeconds(AlmostTheDelaySeconds);
		const double PartWaySeconds = FPlatformTime::Seconds() - PartWayStart;

		const FScenarioLabel PartWay = GameMode->GetScenarioLabel();

		AddInfo(FString::Printf(
			TEXT("MEASURED: ticking %s of the %s s delay took %.1f ms; the label now reads %s ")
			TEXT("with %s s to go ('%s')"),
			*ScenarioLabelBits(AlmostTheDelaySeconds),
			*ScenarioLabelBits(CutRow->HoldSeconds), PartWaySeconds * 1000.0,
			*ScenarioLabelStateName(PartWay.CutState),
			*ScenarioLabelBits(PartWay.SecondsUntilCut), *PartWay.CutText));

		TestTrue(
			*FString::Printf(
				TEXT("IT MUST COUNT DOWN: after %s s the label must still read Armed with STRICTLY ")
				TEXT("less time left than the %s s it began with — a number that never changes is ")
				TEXT("not a clock. It reads %s with %s s."),
				*ScenarioLabelBits(AlmostTheDelaySeconds),
				*ScenarioLabelBits(AtBeginPlay.SecondsUntilCut),
				*ScenarioLabelStateName(PartWay.CutState),
				*ScenarioLabelBits(PartWay.SecondsUntilCut)),
			PartWay.CutState == EScenarioCutState::Armed
				&& PartWay.SecondsUntilCut < AtBeginPlay.SecondsUntilCut
				&& PartWay.SecondsUntilCut > 0.0
				&& FMath::IsFinite(PartWay.SecondsUntilCut));

		/*
		 * AND THE NUMBER IS THE REAL REMAINDER RATHER THAN MERELY SMALLER. Half a second is what
		 * is left of four after three and a half, and a tenth is far more slack than the 1/60
		 * ticking introduces.
		 */
		const double ExpectedRemainingSeconds =
			CutRow->HoldSeconds - AlmostTheDelaySeconds;

		TestTrue(
			*FString::Printf(
				TEXT("and it must be the REAL remainder, about %s s; it reads %s"),
				*ScenarioLabelBits(ExpectedRemainingSeconds),
				*ScenarioLabelBits(PartWay.SecondsUntilCut)),
			FMath::Abs(PartWay.SecondsUntilCut - ExpectedRemainingSeconds) <= 0.1);

		/* --- past it: fired, and saying so -------------------------------------------------- */

		TestWorld.TickSeconds(CutRow->HoldSeconds * 0.375);

		const FScenarioLabel Afterwards = GameMode->GetScenarioLabel();

		AddInfo(FString::Printf(
			TEXT("past the delay the label reads %s with %s s to go ('%s')"),
			*ScenarioLabelStateName(Afterwards.CutState),
			*ScenarioLabelBits(Afterwards.SecondsUntilCut), *Afterwards.CutText));

		TestTrue(
			*FString::Printf(
				TEXT("ONCE THE CUT HAS FIRED the label must say so and stop counting: it reads %s ")
				TEXT("with %s s to go, and its line is '%s' against the '%s' it showed while ")
				TEXT("waiting"),
				*ScenarioLabelStateName(Afterwards.CutState),
				*ScenarioLabelBits(Afterwards.SecondsUntilCut),
				*Afterwards.CutText, *PartWay.CutText),
			Afterwards.CutState == EScenarioCutState::Fired
				&& Afterwards.SecondsUntilCut == 0.0
				&& Afterwards.CutText != PartWay.CutText);

		/* And it is still the same scenario: nothing about the clock renames the level. */
		TestTrue(
			*FString::Printf(
				TEXT("and it still names '%s'; it says '%s'"),
				CutRow->Title, *Afterwards.TitleText),
			Afterwards.TitleText == FString(CutRow->Title)
				&& Afterwards.ExpectationText == FString(CutRow->Expectation));

		TestWorld.End();
	}

	/* --- TWO: a MISTYPED option is labelled with what was BUILT, not what was asked for --- */

	{
		FBrickTestWorld TestWorld;

		TestWorld.Wrapper.BeginPlayURL.AddOption(TEXT("Scenario=free-end-4O"));

		if (!TestWorld.Begin(*this, ADestructionGameGameMode::StaticClass()))
		{
			return true;
		}

		ADestructionGameGameMode* const GameMode =
			TestWorld.World->GetAuthGameMode<ADestructionGameGameMode>();

		if (GameMode == nullptr)
		{
			AddError(TEXT("fixture: the world must be running ADestructionGameGameMode"));

			TestWorld.End();
			return true;
		}

		const int32 Recorded = GameMode->GetSelectedScenarioRow();
		const FScenarioLabel Label = GameMode->GetScenarioLabel();

		AddInfo(FString::Printf(
			TEXT("a mistyped ?Scenario= recorded row %d ('%s') and the label reads '%s' (%s)"),
			Recorded, *ScenarioLabelRowName(Recorded), *Label.TitleText,
			*ScenarioLabelStateName(Label.CutState)));

		TestTrue(
			*FString::Printf(
				TEXT("fixture: a mistyped ?Scenario= must fall back to '%s'; it recorded %d ('%s')"),
				*DefaultRow->Name.ToString(), Recorded, *ScenarioLabelRowName(Recorded)),
			Catalogue().IsValidIndex(Recorded)
				&& Catalogue()[Recorded].Name == DefaultRow->Name);

		TestTrue(
			*FString::Printf(
				TEXT("THE LABEL NAMES THE WALL IN FRONT OF THE PLAYER, which is '%s' — the row the ")
				TEXT("game mode RECORDED — and not '%s', which is what the URL asked for and did ")
				TEXT("not get. It says '%s'."),
				DefaultRow->Title, CutRow->Title, *Label.TitleText),
			Label.TitleText == FString(DefaultRow->Title)
				&& Label.TitleText != FString(CutRow->Title));

		TestTrue(
			*FString::Printf(
				TEXT("and its expectation is that row's too — '%s'; it says '%s'"),
				DefaultRow->Expectation, *Label.ExpectationText),
			Label.ExpectationText == FString(DefaultRow->Expectation)
				&& Label.ExpectationText != FString(CutRow->Expectation));

		/*
		 * AND THE FALLBACK WALL CUTS NOTHING, so no clock starts. A player who mistyped and is
		 * then counted down to a cut that will never come has been told two wrong things.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("'%s' cuts nothing, so the label must not count down: it reads %s with %s s ")
				TEXT("to go"),
				*DefaultRow->Name.ToString(), *ScenarioLabelStateName(Label.CutState),
				*ScenarioLabelBits(Label.SecondsUntilCut)),
			Label.CutState == EScenarioCutState::NoCut && Label.SecondsUntilCut == 0.0);

		TestWorld.End();
	}

	/* --- THREE: a corbel never claims a cut is coming, however long it is left ------------ */

	{
		FBrickTestWorld TestWorld;

		TestWorld.Wrapper.BeginPlayURL.AddOption(
			*FString::Printf(TEXT("Scenario=%s"), *CorbelRow->Name.ToString()));

		if (!TestWorld.Begin(*this, ADestructionGameGameMode::StaticClass()))
		{
			return true;
		}

		ADestructionGameGameMode* const GameMode =
			TestWorld.World->GetAuthGameMode<ADestructionGameGameMode>();

		if (GameMode == nullptr)
		{
			AddError(TEXT("fixture: the world must be running ADestructionGameGameMode"));

			TestWorld.End();
			return true;
		}

		TestTrue(
			*FString::Printf(
				TEXT("fixture: '%s' must name no cut — a corbel is condemned by its own geometry ")
				TEXT("and its whole story is as-laid versus settled; it names %d"),
				*CorbelRow->Name.ToString(), CorbelRow->CutCentresCm.Num()),
			CorbelRow->CutCentresCm.Num() == 0);

		const FScenarioLabel AtBeginPlay = GameMode->GetScenarioLabel();

		TestTrue(
			*FString::Printf(
				TEXT("the label must name the corbel — '%s' — and say what to watch for; it says ")
				TEXT("'%s' / '%s'"),
				CorbelRow->Title, *AtBeginPlay.TitleText, *AtBeginPlay.ExpectationText),
			AtBeginPlay.TitleText == FString(CorbelRow->Title)
				&& AtBeginPlay.ExpectationText == FString(CorbelRow->Expectation));

		/* Well past the delay every cutting row uses, so "never" means never rather than not yet. */
		const double PastEveryDelaySeconds = FMath::Max(1.0, CutRow->HoldSeconds * 1.5);

		const double TickStart = FPlatformTime::Seconds();
		TestWorld.TickSeconds(PastEveryDelaySeconds);
		const double TickedForSeconds = FPlatformTime::Seconds() - TickStart;

		const FScenarioLabel Afterwards = GameMode->GetScenarioLabel();

		AddInfo(FString::Printf(
			TEXT("MEASURED: ticking %s s of the corbel took %.1f ms; the label reads %s with %s s ")
			TEXT("to go ('%s')"),
			*ScenarioLabelBits(PastEveryDelaySeconds), TickedForSeconds * 1000.0,
			*ScenarioLabelStateName(Afterwards.CutState),
			*ScenarioLabelBits(Afterwards.SecondsUntilCut), *Afterwards.CutText));

		TestTrue(
			*FString::Printf(
				TEXT("A SCENARIO THAT CUTS NOTHING MUST NEVER SAY A CUT IS COMING, at begin-play ")
				TEXT("or %s s later: it reads %s then %s, with %s s and %s s to go"),
				*ScenarioLabelBits(PastEveryDelaySeconds),
				*ScenarioLabelStateName(AtBeginPlay.CutState),
				*ScenarioLabelStateName(Afterwards.CutState),
				*ScenarioLabelBits(AtBeginPlay.SecondsUntilCut),
				*ScenarioLabelBits(Afterwards.SecondsUntilCut)),
			AtBeginPlay.CutState == EScenarioCutState::NoCut
				&& Afterwards.CutState == EScenarioCutState::NoCut
				&& AtBeginPlay.SecondsUntilCut == 0.0
				&& Afterwards.SecondsUntilCut == 0.0);

		/*
		 * AND ITS LINE DOES NOT CHANGE, which is the same property the world-free sweep asserts
		 * and is worth restating here because this one is a real clock rather than a passed-in
		 * double: a game mode that fed elapsed world time to a row with no cut would produce a
		 * line that drifts even though the state stays NoCut.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("and its line does not drift with the clock: '%s' at begin-play, '%s' %s s ")
				TEXT("later"),
				*AtBeginPlay.CutText, *Afterwards.CutText,
				*ScenarioLabelBits(PastEveryDelaySeconds)),
			!AtBeginPlay.CutText.IsEmpty() && Afterwards.CutText == AtBeginPlay.CutText);

		TestWorld.End();
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
