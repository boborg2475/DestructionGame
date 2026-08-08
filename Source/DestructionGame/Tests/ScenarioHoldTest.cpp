// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Structure.h"
#include "DestructionGameGameMode.h"
#include "HAL/PlatformTime.h"
#include "Tests/BrickWorldTestSupport.h"
#include "World/DestructionScenarios.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A SCENARIO LEVEL HOLDS ITS STRUCTURE EXACTLY AS LAID UNTIL ITS HOLD EXPIRES, AND ONLY THEN
 * SETTLES IT.
 *
 * =====================================================================================
 * THE DEFECT, WHICH IS THAT SEVEN OF THE NINE LEVELS ARE OVER BEFORE THEY BEGIN
 * =====================================================================================
 *
 * The user asked to "join the level, see the wall in front of the player start and be able to
 * observe what happens to the wall section". Slice B delivered half of that: a CUTTING row stands
 * whole, and the player watches a brick go after `HoldSeconds`.
 *
 * But `free-end-40` is the only row that cuts. Every corbel is condemned by its own GEOMETRY
 * rather than by anything removed from it — its root joint is over capacity the moment it exists —
 * and the game mode settles the structure on the frame it builds it. So on a corbel level the
 * collapse begins during begin-play, before the player's first frame is drawn, and the one thing
 * the level exists to show is the thing they miss. `Tests/CorbelScreenshotTest.cpp` already draws
 * the distinction the levels need: "before" is the structure AS LAID with every brick still
 * kinematic and nothing released, "after" is once it has been settled.
 *
 * ONE CLOCK, NOT TWO, AND ITS NAME IS NO LONGER THE CUT'S. `HoldSeconds` is how long the level
 * leaves the structure as laid; at that moment it RUNS — applying whatever cuts the row names and
 * settling what is left. A separate per-row hold would be a second answer to "when does this level
 * do its thing", and the first thing to go wrong with two answers is a label counting one of them
 * down while the other fires.
 *
 * =====================================================================================
 * "HELD AS LAID" IS ASSERTED ON THE MECHANISM, AND IT IS NOT THE ABSENCE OF A SOLVE
 * =====================================================================================
 *
 * DESIGN.md §4 forbids reading movement as evidence, and it applies with full force here: two
 * pieces can sever and stay resting exactly where they were. So the observable is `IsReleased` —
 * a brick spawns KINEMATIC and only `ABrickActor::Release` makes it dynamic, so "nothing has been
 * handed to physics" is a countable, binary fact about the level rather than a measurement.
 *
 * AND THE HELD STRUCTURE IS STILL SOLVED. A level could hold by simply never solving, and every
 * release count would read zero because nothing ever asked — which is the wrong hold and a
 * regression the suite has already been bitten by from the other end: `EPieceSupport::Falling` is
 * what an ABSENT answer reads as, so an unsolved wall and a wall in free fall are one answer to
 * everything that looks. CURRENT_STATE.md records the mutation that proves it —
 * `World.Scenario.GameModeBuildsTheWallOnBeginPlay` reports every piece Falling "as the game mode
 * left it" the moment the begin-play solve is removed. So holding is SOLVED AND NOT SETTLED: the
 * loads are known, and no joint has been asked to give yet.
 *
 * =====================================================================================
 * THE COUNT AFTER THE HOLD COMES FROM AN INDEPENDENT WORLD-FREE READING, NOT FROM A LITERAL
 * =====================================================================================
 *
 * "Something fell" would pass against a level that dropped every brick it owns, so the AFTER
 * assertion is an EXACT count and the count is taken from the world-free catalogue: the same row
 * laid through `DestructionScenarios::Build`, settled with `FStructure::SolveAndBreak`, and the
 * pieces the settled solve stops holding up counted. That is the same drift check
 * `World.Scenarios.CorbelRows` already makes on the root joint's reading, pointed at the release
 * set instead — it says the LEVEL and the HEADLESS FIXTURE are about one structure. It is
 * explicitly not an independent solver, and the header says so rather than implying otherwise;
 * what it is independent of is the world wire, which is the thing this slice changes.
 *
 * =====================================================================================
 * WHY THESE ROWS, AND WHAT EACH COSTS
 * =====================================================================================
 *
 *   - `corbel-a-bare-4` (10 pieces) IS THE CONTROL, AND IT IS THE SMALLEST STRUCTURE THAT CAN
 *     CARRY THE CLAIM. Its root reads 0.15613 — `COMPOSITE_DEPTH_DESIGN.md` works the bare arm
 *     through by hand — so settling it must release NOTHING, ever. Without a row like this, every
 *     assertion in the file is satisfied by a level that hands its whole structure to physics the
 *     moment the hold expires.
 *
 *   - `corbel-e36` (519 pieces) IS THE CONDEMNED ONE, and it is the row the crossover is measured
 *     at: `Core.Structure.CorbelStepsBeforeTensionWins` bisects the family and finds 36 the
 *     smallest step count whose root joint reads over 1.0. Under today's game mode it is released
 *     at begin-play, so this is where the red lands.
 *
 *   - `corbel-e35` (496 pieces) IS ARBITRATED WORLD-FREE ONLY. It is E36's straddle partner, one
 *     course shorter and reading just UNDER 1.0, so settling it must break nothing — the tightest
 *     control there is. It is not joined as a level because 496 more spawned actors buy nothing
 *     the ten-piece control does not already buy, and `World.Scenarios.CorbelRows` already holds
 *     the pair against each other.
 *
 * `corbel-f-100` IS DELIBERATELY ABSENT. 3,015 bricks is the largest structure in the suite and
 * `Core.Structure.AHundredStepCorbelMustComeDown` already lays, solves and cascades it; standing
 * it up in a world as well would be the most expensive thing here for a claim E36 already carries.
 *
 * NEEDS A TICKING WORLD: YES, and the ticking is the whole point — the hold is measured in world
 * seconds, and the claim is about what has NOT happened while it runs.
 */
namespace ScenarioHoldTestSupport
{
	using namespace DestructionLayout;

	/** How near two release counts have to be to be the same count: they are integers. */
	inline FString ScenarioHoldBits(double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	/**
	 * One row this file arbitrates, and how far it is taken.
	 *
	 * `bCondemned` IS DECLARED HERE AND THEN HELD AGAINST THE WORLD-FREE READING rather than being
	 * discovered from it. A table that read its own expectation off the fixture would agree with
	 * whatever the fixture said, including the day every row started collapsing; declared, a row
	 * whose status moved fails as a fixture error naming the row.
	 */
	struct FScenarioHoldCase
	{
		const TCHAR* Name;

		/** Pieces the closed form in `World.Scenarios.CorbelRows` derives for this row. */
		int32 ExpectedPieces;

		/** Whether its root joint is over capacity as laid, so settling it must release something. */
		bool bCondemned;

		/** False for a row arbitrated world-free only — see the header on E35. */
		bool bJoinAsLevel;
	};

	const FScenarioHoldCase ScenarioHoldCases[] =
	{
		{ TEXT("corbel-a-bare-4"), 10, false, true },
		{ TEXT("corbel-e35"), 496, false, false },
		{ TEXT("corbel-e36"), 519, true, true },
	};

	/** What settling a row does, read off the world-free catalogue. */
	struct FScenarioHoldOracle
	{
		bool bBuilt = false;

		int32 Pieces = 0;
		int32 Joints = 0;

		/** Passes `SolveAndBreak` broke at least one joint in. Zero for a structure that stands. */
		int32 BreakPasses = 0;

		/** Pieces the SETTLED solve stops holding up — exactly what `ApplyResults` releases. */
		int32 WouldRelease = 0;
	};

	/**
	 * Lay this row world-free, settle it, and count what the settle leaves unheld.
	 *
	 * THE SAME PREDICATE `FStructureBinding::ApplyResults` USES, written out here rather than
	 * called: Grounded and Supported stay put, Stranded and Falling both come down, and a piece the
	 * last solve never answered for is never released. Written out because the point of the
	 * comparison is that the LEVEL and the CATALOGUE agree — a helper shared with production would
	 * agree with production however wrong production was.
	 */
	inline FScenarioHoldOracle ScenarioHoldSettleWorldFree(
		const DestructionScenarios::FScenario& Row)
	{
		FScenarioHoldOracle Oracle;

		FBrickLayout Laid;
		TArray<int32> CutPieces;

		if (!DestructionScenarios::Build(Row, Laid, CutPieces))
		{
			return Oracle;
		}

		Oracle.bBuilt = true;
		Oracle.Pieces = Laid.Structure.NumPieces();
		Oracle.Joints = Laid.Structure.NumConnections();
		Oracle.BreakPasses = Laid.Structure.SolveAndBreak();

		for (int32 Piece = 0; Piece < Laid.Structure.NumPieces(); ++Piece)
		{
			if (Laid.Structure.IsPieceRemoved(Piece)
				|| !Laid.Structure.HasSupportAnswer(Piece))
			{
				continue;
			}

			const EPieceSupport Support = Laid.Structure.GetPieceSupport(Piece);

			if (Support != EPieceSupport::Grounded && Support != EPieceSupport::Supported)
			{
				++Oracle.WouldRelease;
			}
		}

		return Oracle;
	}

	/** The row with this name, or null with the reason reported. */
	inline const DestructionScenarios::FScenario* ScenarioHoldRowNamed(
		FAutomationTestBase& Test, const TCHAR* Name)
	{
		const int32 Index = DestructionScenarios::IndexOfName(FName(Name));

		if (!DestructionScenarios::Catalogue().IsValidIndex(Index))
		{
			Test.AddError(FString::Printf(
				TEXT("fixture: the catalogue must carry a row named '%s'"), Name));

			return nullptr;
		}

		return &DestructionScenarios::Catalogue()[Index];
	}

	/** How many live pieces have been handed to physics. */
	inline int32 ScenarioHoldReleasedCount(const FStructureBinding& Binding)
	{
		int32 Released = 0;

		for (int32 Piece = 0; Piece < Binding.NumPieces(); ++Piece)
		{
			if (!Binding.IsPieceRemoved(Piece) && Binding.IsReleased(Piece))
			{
				++Released;
			}
		}

		return Released;
	}

	/** How many live pieces the last solve has no answer for. */
	inline int32 ScenarioHoldUnansweredCount(const FStructureBinding& Binding)
	{
		int32 Unanswered = 0;

		for (int32 Piece = 0; Piece < Binding.NumPieces(); ++Piece)
		{
			if (!Binding.IsPieceRemoved(Piece)
				&& !Binding.GetStructure().HasSupportAnswer(Piece))
			{
				++Unanswered;
			}
		}

		return Unanswered;
	}

	/** How many pieces still have a live brick standing for them. */
	inline int32 ScenarioHoldLiveBrickCount(const FStructureBinding& Binding)
	{
		int32 Bricks = 0;

		for (int32 Piece = 0; Piece < Binding.NumPieces(); ++Piece)
		{
			if (IsValid(Cast<ABrickActor>(Binding.GetActor(Piece))))
			{
				++Bricks;
			}
		}

		return Bricks;
	}

	/**
	 * HOW FAR THROUGH THE HOLD THE "STILL HELD" READING IS TAKEN, as a fraction of the row's own
	 * hold, so a catalogue that retunes the hold retunes this with it. Seven eighths leaves half a
	 * second of margin either side on today's four — thirty ticks, far more than the rounding in
	 * `TickSeconds` and far less than the hold itself.
	 */
	constexpr double ScenarioHoldMostOfTheHold = 0.875;

	/** And enough past it that the level has certainly run: 1.25 holds in total. */
	constexpr double ScenarioHoldPastTheHold = 0.375;
}

/**
 * NOTHING IS RELEASED WHILE THE LEVEL HOLDS, AND EXACTLY WHAT THE CATALOGUE SAYS IS RELEASED WHEN
 * IT RUNS.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScenarioHoldsAsLaidTest,
	"DestructionGame.World.Scenario.GameModeHoldsTheStructureAsLaid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FScenarioHoldsAsLaidTest::RunTest(const FString& Parameters)
{
	using namespace ScenarioHoldTestSupport;
	using namespace BrickWorldTestSupport;
	using namespace DestructionScenarios;

	int32 CondemnedCases = 0;
	int32 StandingCases = 0;

	for (const FScenarioHoldCase& Case : ScenarioHoldCases)
	{
		const FScenario* const Row = ScenarioHoldRowNamed(*this, Case.Name);

		if (Row == nullptr)
		{
			continue;
		}

		/* --- what the world-free catalogue says settling this row does ---------------------- */

		const double OracleStart = FPlatformTime::Seconds();
		const FScenarioHoldOracle Oracle = ScenarioHoldSettleWorldFree(*Row);
		const double OracleSeconds = FPlatformTime::Seconds() - OracleStart;

		if (!Oracle.bBuilt)
		{
			AddError(FString::Printf(
				TEXT("fixture: '%s' must build world-free, or there is nothing to hold the level ")
				TEXT("against"),
				Case.Name));

			continue;
		}

		AddInfo(FString::Printf(
			TEXT("MEASURED: '%s' laid and settled world-free in %.1f ms — %d pieces, %d joints, ")
			TEXT("%d breaking pass(es), %d piece(s) the settle stops holding up"),
			Case.Name, OracleSeconds * 1000.0, Oracle.Pieces, Oracle.Joints,
			Oracle.BreakPasses, Oracle.WouldRelease));

		TestTrue(
			*FString::Printf(
				TEXT("fixture: '%s' must lay the %d pieces World.Scenarios.CorbelRows derives for ")
				TEXT("it; it laid %d"),
				Case.Name, Case.ExpectedPieces, Oracle.Pieces),
			Oracle.Pieces == Case.ExpectedPieces);

		TestTrue(
			*FString::Printf(
				TEXT("fixture: '%s' must cut NOTHING — a corbel is condemned by its own geometry, ")
				TEXT("and a row that cut would be a different experiment; it names %d cut(s)"),
				Case.Name, Row->CutCentresCm.Num()),
			Row->CutCentresCm.Num() == 0);

		/*
		 * AND THE ROW IS THE KIND OF ROW THE TABLE SAYS IT IS, held BOTH ways round. A condemned
		 * corbel must give at least one pass AND leave something unheld; a standing one must give
		 * no pass at all and leave nothing. If a row's status ever moves, this fails naming the
		 * row rather than silently turning the assertions below into a tautology.
		 */
		if (Case.bCondemned)
		{
			++CondemnedCases;

			TestTrue(
				*FString::Printf(
					TEXT("fixture: '%s' IS CONDEMNED AS LAID — its root joint reads over 1.0 ")
					TEXT("(Core.Structure.CorbelStepsBeforeTensionWins puts the crossover at 36 ")
					TEXT("steps), so settling it must break something and leave something unheld; ")
					TEXT("it broke in %d pass(es) and left %d piece(s) unheld"),
					Case.Name, Oracle.BreakPasses, Oracle.WouldRelease),
				Oracle.BreakPasses > 0 && Oracle.WouldRelease > 0);
		}
		else
		{
			++StandingCases;

			TestTrue(
				*FString::Printf(
					TEXT("fixture: '%s' STANDS AS LAID — its root joint reads under 1.0, so settling ")
					TEXT("it must break nothing and release nothing; it broke in %d pass(es) and ")
					TEXT("left %d piece(s) unheld"),
					Case.Name, Oracle.BreakPasses, Oracle.WouldRelease),
				Oracle.BreakPasses == 0 && Oracle.WouldRelease == 0);
		}

		if (!Case.bJoinAsLevel)
		{
			continue;
		}

		/* --- and now the level itself ------------------------------------------------------- */

		FBrickTestWorld TestWorld;

		TestWorld.Wrapper.BeginPlayURL.AddOption(
			*FString::Printf(TEXT("Scenario=%s"), Case.Name));

		const double BeginStart = FPlatformTime::Seconds();
		const bool bBegun = TestWorld.Begin(*this, ADestructionGameGameMode::StaticClass());
		const double BeginSeconds = FPlatformTime::Seconds() - BeginStart;

		if (!bBegun)
		{
			return true;
		}

		ADestructionGameGameMode* const GameMode =
			TestWorld.World->GetAuthGameMode<ADestructionGameGameMode>();

		FStructureBinding* const Binding = GameMode != nullptr
			? TestWorld.Subsystem->Find(GameMode->GetBuiltStructureId())
			: nullptr;

		if (Binding == nullptr)
		{
			AddError(FString::Printf(
				TEXT("the game mode (%s) must have built the scenario the URL named ('%s')"),
				*GetNameSafe(TestWorld.World->GetAuthGameMode()), Case.Name));

			TestWorld.End();
			continue;
		}

		AddInfo(FString::Printf(
			TEXT("MEASURED: joining '%s' — world, begin-play and %d spawned bricks — took %.1f ms"),
			Case.Name, Binding->NumPieces(), BeginSeconds * 1000.0));

		TestTrue(
			*FString::Printf(
				TEXT("?Scenario=%s must build the SAME structure the catalogue lays world-free: %d ")
				TEXT("pieces and %d joints against the fixture's %d and %d. A lookalike would settle ")
				TEXT("to a different count and nothing about it would look wrong on screen."),
				Case.Name, Binding->NumPieces(), Binding->GetStructure().NumConnections(),
				Oracle.Pieces, Oracle.Joints),
			Binding->NumPieces() == Oracle.Pieces
				&& Binding->GetStructure().NumConnections() == Oracle.Joints);

		/* --- AT BEGIN-PLAY: held exactly as laid, and already solved ---------------------- */

		const int32 SolvesAtBeginPlay = Binding->GetStructure().NumSolves();

		AddInfo(FString::Printf(
			TEXT("'%s' at begin-play: %d of %d pieces live, %d bricks standing, %d released, %d ")
			TEXT("with no support answer, %d solve(s) so far"),
			Case.Name, Binding->GetStructure().NumLivePieces(), Binding->NumPieces(),
			ScenarioHoldLiveBrickCount(*Binding), ScenarioHoldReleasedCount(*Binding),
			ScenarioHoldUnansweredCount(*Binding), SolvesAtBeginPlay));

		/*
		 * THE ASSERTION THE WHOLE SLICE IS ABOUT. A brick spawns kinematic and only
		 * ABrickActor::Release makes it dynamic, so "nothing has been handed to physics" is the
		 * mechanism reading of "the structure is standing exactly as it was laid". NOT
		 * displacement: DESIGN.md §4 is explicit, and a condemned corbel whose arm has been
		 * released has moved nothing at all on the frame it was released.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("AT BEGIN-PLAY '%s' MUST BE HELD EXACTLY AS LAID — not one of its %d pieces ")
				TEXT("may have been handed to physics, and %d %s. A player joining this level has ")
				TEXT("to see the structure whole; a corbel that is released on the frame it is ")
				TEXT("built has already collapsed before their first frame is drawn, and the one ")
				TEXT("thing the level exists to show is the thing they miss."),
				Case.Name, Binding->NumPieces(), ScenarioHoldReleasedCount(*Binding),
				ScenarioHoldReleasedCount(*Binding) == 1 ? TEXT("was") : TEXT("were")),
			ScenarioHoldReleasedCount(*Binding) == 0);

		TestTrue(
			*FString::Printf(
				TEXT("AT BEGIN-PLAY '%s' must still be whole: %d of %d pieces live and %d bricks ")
				TEXT("standing. Holding is about what has not been RELEASED, but a level that ")
				TEXT("removed pieces instead would satisfy that and show the same empty air."),
				Case.Name, Binding->GetStructure().NumLivePieces(), Binding->NumPieces(),
				ScenarioHoldLiveBrickCount(*Binding)),
			Binding->GetStructure().NumLivePieces() == Binding->NumPieces()
				&& ScenarioHoldLiveBrickCount(*Binding) == Binding->NumPieces());

		/*
		 * AND HELD IS SOLVED-AND-NOT-SETTLED RATHER THAN UNSOLVED. A level could hold by never
		 * solving at all and the release count would read zero because nothing ever asked — the
		 * wrong hold, and one the suite has been bitten by from the other end: an ABSENT support
		 * answer reads as Falling, so an unsolved structure and one in free fall are one answer to
		 * every readout, and the first click anywhere would push against an answer nobody computed.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("AT BEGIN-PLAY '%s' must already be SOLVED while it is held — every one of its ")
				TEXT("%d live pieces must have a support answer, because an absent answer reads as ")
				TEXT("Falling; %d have none"),
				Case.Name, Binding->GetStructure().NumLivePieces(),
				ScenarioHoldUnansweredCount(*Binding)),
			ScenarioHoldUnansweredCount(*Binding) == 0);

		/* --- MOST OF THE WAY THROUGH THE HOLD: still nothing ------------------------------ */

		const double MostOfTheHoldSeconds = Row->HoldSeconds * ScenarioHoldMostOfTheHold;

		TestTrue(
			*FString::Printf(
				TEXT("fixture: '%s' must hold long enough for a human to look — more than a second, ")
				TEXT("and finite; its hold is %s s"),
				Case.Name, *ScenarioHoldBits(Row->HoldSeconds)),
			Row->HoldSeconds > 1.0 && FMath::IsFinite(Row->HoldSeconds));

		const double DuringStart = FPlatformTime::Seconds();
		TestWorld.TickSeconds(MostOfTheHoldSeconds);
		const double DuringSeconds = FPlatformTime::Seconds() - DuringStart;

		AddInfo(FString::Printf(
			TEXT("MEASURED: ticking %s of '%s''s %s s hold over %d pieces took %.1f ms; %d ")
			TEXT("released, %d solve(s)"),
			*ScenarioHoldBits(MostOfTheHoldSeconds), Case.Name,
			*ScenarioHoldBits(Row->HoldSeconds), Binding->NumPieces(), DuringSeconds * 1000.0,
			ScenarioHoldReleasedCount(*Binding), Binding->GetStructure().NumSolves()));

		TestTrue(
			*FString::Printf(
				TEXT("STILL HELD after %s of '%s''s %s s hold: %d piece(s) released and %d of %d ")
				TEXT("live. The hold is the whole point — a level that settles early shows a human ")
				TEXT("nothing they had a chance to watch."),
				*ScenarioHoldBits(MostOfTheHoldSeconds), Case.Name,
				*ScenarioHoldBits(Row->HoldSeconds), ScenarioHoldReleasedCount(*Binding),
				Binding->GetStructure().NumLivePieces(), Binding->NumPieces()),
			ScenarioHoldReleasedCount(*Binding) == 0
				&& Binding->GetStructure().NumLivePieces() == Binding->NumPieces());

		/* --- PAST IT: the level has run, and settled to exactly the catalogue's count ------ */

		const double AfterStart = FPlatformTime::Seconds();
		TestWorld.TickSeconds(Row->HoldSeconds * ScenarioHoldPastTheHold);
		const double AfterSeconds = FPlatformTime::Seconds() - AfterStart;

		const int32 ReleasedAfter = ScenarioHoldReleasedCount(*Binding);

		AddInfo(FString::Printf(
			TEXT("MEASURED: ticking past '%s''s hold took a further %.1f ms; %d released against ")
			TEXT("the catalogue's %d, %d solve(s) against %d at begin-play"),
			Case.Name, AfterSeconds * 1000.0, ReleasedAfter, Oracle.WouldRelease,
			Binding->GetStructure().NumSolves(), SolvesAtBeginPlay));

		/*
		 * THE LEVEL RAN, which is a separate claim from anything having fallen — and it is the ONLY
		 * claim available on a row that correctly releases nothing. A settle is a solve, so the
		 * graph's own solve count going up is what says the hold expired and something happened;
		 * without it, a level that simply never ran would be indistinguishable from one that ran
		 * and correctly did nothing.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("'%s' MUST HAVE RUN once its %s s hold expired: settling is a solve, so the ")
				TEXT("graph must have solved more than the %d times it had at begin-play; it has ")
				TEXT("solved %d"),
				Case.Name, *ScenarioHoldBits(Row->HoldSeconds), SolvesAtBeginPlay,
				Binding->GetStructure().NumSolves()),
			Binding->GetStructure().NumSolves() > SolvesAtBeginPlay);

		/*
		 * AND IT SETTLED TO EXACTLY WHAT THE CATALOGUE SAYS. An inequality would pass against a
		 * level that dropped everything it owns; the equality is what makes the control row mean
		 * something and what makes the condemned row a statement about THIS structure.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("'%s' MUST SETTLE TO EXACTLY THE %d PIECE(S) THE WORLD-FREE CATALOGUE SAYS THE ")
				TEXT("SAME ROW SETTLES TO; the level released %d of its %d. A level and a headless ")
				TEXT("reading that disagree here are about two different structures."),
				Case.Name, Oracle.WouldRelease, ReleasedAfter, Binding->NumPieces()),
			ReleasedAfter == Oracle.WouldRelease);

		TestTrue(
			*FString::Printf(
				TEXT("'%s' NAMES NO CUT, so settling it may not REMOVE anything either: %d of %d ")
				TEXT("pieces are live and %d bricks stand"),
				Case.Name, Binding->GetStructure().NumLivePieces(), Binding->NumPieces(),
				ScenarioHoldLiveBrickCount(*Binding)),
			Binding->GetStructure().NumLivePieces() == Binding->NumPieces()
				&& ScenarioHoldLiveBrickCount(*Binding) == Binding->NumPieces());

		TestWorld.End();
	}

	/*
	 * --- and the sweep was not vacuous ---------------------------------------------------
	 *
	 * BOTH KINDS OR THE FILE PROVES NOTHING. With no condemned row nothing here would ever have
	 * been released and "it settled to the catalogue's count" would be "zero equals zero" three
	 * times over; with no standing row, a level that handed its entire structure to physics on the
	 * frame the hold expired would pass every assertion above.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("fixture: this sweep must carry BOTH kinds of row or it asserts nothing — %d ")
			TEXT("condemned as laid and %d standing as laid"),
			CondemnedCases, StandingCases),
		CondemnedCases >= 1 && StandingCases >= 1);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
