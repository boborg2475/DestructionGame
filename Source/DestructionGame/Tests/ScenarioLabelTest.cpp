// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "DestructionGameGameMode.h"
#include "HAL/PlatformTime.h"
#include "Tests/BrickWorldTestSupport.h"
#include "World/DestructionScenarios.h"
#include "World/ScenarioLabel.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The scenario label tells the player which scenario, what to watch for, and when the cut
 * comes. A pure presenter of (row, seconds to wait, fired), tested without a world. Claims are
 * structural, not wording: title and expectation equal the row's own; a no-cut row's line
 * never changes with the clock; a cut row's line changes as it counts down and again once
 * fired; and no no-cut line equals any cut line (else "cut in 4 s" on a corbel would pass).
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

		/** Seconds to wait, as a multiple of the row's delay. */
		double DelayFraction;

		/** Seconds to wait, used when bIsAbsolute. */
		double AbsoluteSeconds;

		/** True to use AbsoluteSeconds (the degenerate moments). */
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

	// Floor so an empty catalogue fails; the sweep also needs both cut and no-cut rows.
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
	 * Degenerate moments matter: a late tick gives a negative remainder, and NaN slips through
	 * FMath::Max/Min, so an unguarded label shows a plausible number for a stopped clock.
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

	// First failure of each invariant is reported in full; the rest are counted and asserted at the end.
	int32 TitleFailures = 0;
	int32 ExpectationFailures = 0;
	int32 BlankLineFailures = 0;
	int32 CountdownRangeFailures = 0;
	int32 StateFailures = 0;
	int32 IdleCountdownFailures = 0;
	int32 ArmedCountdownFailures = 0;

	// Lines from each kind of row, for the cross-check below.
	TSet<FString> NoCutLines;
	TSet<FString> CutLines;

	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		const FScenario& Row = Rows[Index];

		const bool bRowCuts = Row.CutCentresCm.Num() > 0;

		const FString RowLabel =
			FString::Printf(TEXT("row %d ('%s')"), Index, *Row.Name.ToString());

		// Every line this row produces; must be one string if it cuts nothing.
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

				// Title and expectation are the row's own.

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

				/*
				 * The cut line is blank on the build sandbox (nothing laid yet; SESSION_UI_DESIGN.md
				 * §f) and never blank elsewhere. Asserted both ways, not exempted.
				 */
				if (Label.CutText.IsEmpty() != Row.bBuildSandbox)
				{
					if (BlankLineFailures == 0)
					{
						AddError(FString::Printf(
							TEXT("%s: this row %s a build sandbox, so its cut line must %s; it reads ")
							TEXT("'%s'"),
							*Where,
							Row.bBuildSandbox ? TEXT("IS") : TEXT("is NOT"),
							Row.bBuildSandbox
								? TEXT("be blank — there is nothing laid for a line to describe")
								: TEXT("never be blank — a hole where the state of the level should "
									   "be reads as a readout that failed"),
							*Label.CutText));
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

				// A state with nothing coming must show a zero countdown.
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

				// An armed countdown passes an in-range caller value through unchanged; only bad values clamp.
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

		if (!bRowCuts)
		{
			// One line whatever the clock says, so it cannot be a countdown.
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
			// A cut row's line differs at the start, at 1 s left, and after firing.
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

	// No no-cut line may equal a cut line; otherwise a fixed countdown on a corbel would pass.
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
			TEXT("every one of the %d swept labels must say SOMETHING about the cut, unless it is the ")
			TEXT("build plot — where there is nothing laid to say anything about, and the line is ")
			TEXT("blank on purpose; %d rows said the wrong one of the two"),
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

	/*
	 * An invalid row fails closed: the label still says something, claims no cut, and never
	 * borrows a real scenario's title.
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
 * The game mode's label names the scenario it actually built (the recorded row, not the
 * requested one: a mistyped option falls back to sandbox), counts down, and reports the cut.
 * The countdown is read three times in one ticking world and must strictly decrease, so a
 * constant fails. A corbel is ticked well past every delay and must never claim a cut.
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

	// One: a cutting row is named, counted down, then reported.

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

		// Compared against GetSelectedScenarioRow, not assumed.
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

		// The cut is announced before it happens.
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

		// Most of the way through: still armed, strictly less time left.

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

		// The real remainder, not merely smaller; 0.1 s tolerance is well above 1/60 s ticking.
		const double ExpectedRemainingSeconds =
			CutRow->HoldSeconds - AlmostTheDelaySeconds;

		TestTrue(
			*FString::Printf(
				TEXT("and it must be the REAL remainder, about %s s; it reads %s"),
				*ScenarioLabelBits(ExpectedRemainingSeconds),
				*ScenarioLabelBits(PartWay.SecondsUntilCut)),
			FMath::Abs(PartWay.SecondsUntilCut - ExpectedRemainingSeconds) <= 0.1);

		// Past the delay: fired, and says so.

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

		// Still the same scenario.
		TestTrue(
			*FString::Printf(
				TEXT("and it still names '%s'; it says '%s'"),
				CutRow->Title, *Afterwards.TitleText),
			Afterwards.TitleText == FString(CutRow->Title)
				&& Afterwards.ExpectationText == FString(CutRow->Expectation));

		TestWorld.End();
	}

	// Two: a mistyped option is labelled with what was built, not what was asked for.

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

		// The fallback wall cuts nothing, so no clock starts.
		TestTrue(
			*FString::Printf(
				TEXT("'%s' cuts nothing, so the label must not count down: it reads %s with %s s ")
				TEXT("to go"),
				*DefaultRow->Name.ToString(), *ScenarioLabelStateName(Label.CutState),
				*ScenarioLabelBits(Label.SecondsUntilCut)),
			Label.CutState == EScenarioCutState::NoCut && Label.SecondsUntilCut == 0.0);

		TestWorld.End();
	}

	// Three: a corbel never claims a cut is coming.

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

		// Well past every cutting row's delay, so "never" means never.
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

		// The line must not drift with the real clock either.
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

/**
 * The build plot's banner has no cut line, since "how it was laid" is false on an empty plot
 * (SESSION_UI_DESIGN.md §f: title and expectation only). Every other no-cut row must still
 * share one non-empty line, so blanking the line everywhere fails. No world needed.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScenarioLabelBuildPlotHasNoCutLineTest,
	"DestructionGame.World.Scenarios.BuildPlotLabelHasNoCutLine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FScenarioLabelBuildPlotHasNoCutLineTest::RunTest(const FString& Parameters)
{
	using namespace ScenarioLabelTestSupport;
	using namespace DestructionScenarios;

	const TArray<FScenario>& Rows = Catalogue();

	const int32 BuildRow = IndexOfName(FName(TEXT("build")));

	if (!Rows.IsValidIndex(BuildRow))
	{
		AddError(TEXT("fixture: the catalogue must carry a row named 'build'"));
		return true;
	}

	TestTrue(
		*FString::Printf(
			TEXT("fixture: row %d ('build') must be the build sandbox and must cut nothing; it is %s ")
			TEXT("and names %d cut(s)"),
			BuildRow, Rows[BuildRow].bBuildSandbox ? TEXT("flagged") : TEXT("NOT flagged"),
			Rows[BuildRow].CutCentresCm.Num()),
		Rows[BuildRow].bBuildSandbox && Rows[BuildRow].CutCentresCm.Num() == 0);

	// One: the cut line is absent at every moment, swept so no branch leaks the sentence back.
	const double BuildMoments[] =
	{
		Rows[BuildRow].HoldSeconds, 1.0, 0.0, -1.0, 1.0e9, ScenarioLabelMakeNaN()
	};

	for (const double SecondsIn : BuildMoments)
	{
		for (int32 FiredCase = 0; FiredCase < 2; ++FiredCase)
		{
			const bool bFired = FiredCase == 1;

			const FScenarioLabel Label = BuildScenarioLabel(BuildRow, SecondsIn, bFired);

			TestTrue(
				*FString::Printf(
					TEXT("THE BUILD PLOT HAS NOTHING TO REPORT: its cut line must be EMPTY (%s s in, ")
					TEXT("fired = %s), because 'nothing is cut here — what you are watching is how it ")
					TEXT("was laid' is two lies over a plot nobody has laid anything on. It reads '%s'"),
					*ScenarioLabelBits(SecondsIn), bFired ? TEXT("true") : TEXT("false"),
					*Label.CutText),
				Label.CutText.IsEmpty());

			// The rest of the banner is untouched.
			TestTrue(
				*FString::Printf(
					TEXT("and the rest of the banner must still be there — a level with nothing in it ")
					TEXT("is exactly the one a player needs told what it is for. Title '%s', ")
					TEXT("expectation '%s'"),
					*Label.TitleText, *Label.ExpectationText),
				Label.TitleText == FString(Rows[BuildRow].Title)
					&& Label.ExpectationText == FString(Rows[BuildRow].Expectation));

			TestTrue(
				*FString::Printf(
					TEXT("and it still claims no cut and no clock: state %s with %s s to go"),
					*ScenarioLabelStateName(Label.CutState),
					*ScenarioLabelBits(Label.SecondsUntilCut)),
				Label.CutState == EScenarioCutState::NoCut && Label.SecondsUntilCut == 0.0);
		}
	}

	// Two: every other no-cut row still has its line.

	TSet<FString> OtherNoCutLines;
	int32 OtherNoCutRows = 0;

	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		if (Rows[Index].bBuildSandbox || Rows[Index].CutCentresCm.Num() > 0)
		{
			continue;
		}

		++OtherNoCutRows;

		const FScenarioLabel Label = BuildScenarioLabel(Index, Rows[Index].HoldSeconds, false);

		TestTrue(
			*FString::Printf(
				TEXT("row %d ('%s') cuts nothing but is NOT a build plot — it is a wall or a corbel "
					 "that was laid and is being watched — so it must still say so. It reads '%s'"),
				Index, *Rows[Index].Name.ToString(), *Label.CutText),
			!Label.CutText.IsEmpty());

		OtherNoCutLines.Add(Label.CutText);
	}

	TestTrue(
		*FString::Printf(
			TEXT("fixture: there must be at least one no-cut row that is NOT the build plot, or the "
				 "regression half of this test is vacuous; there are %d"),
			OtherNoCutRows),
		OtherNoCutRows >= 1);

	// "Unchanged", structurally: all other no-cut rows share one line.
	TestEqual(
		FString::Printf(
			TEXT("and all %d of them must share the ONE line they have always shared; they produced "
				 "%d: [%s]"),
			OtherNoCutRows, OtherNoCutLines.Num(),
			*FString::Join(OtherNoCutLines.Array(), TEXT(" | "))),
		OtherNoCutLines.Num(), 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
