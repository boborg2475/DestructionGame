// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Structure.h"
#include "DestructionGameGameMode.h"
#include "HAL/PlatformTime.h"
#include "Tests/BrickWorldTestSupport.h"
#include "World/DestructionScenarios.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * A scenario level holds its structure exactly as laid until HoldSeconds expires, then runs:
 * applies the row's cuts and settles. Without the hold, a corbel condemned by its own geometry
 * would collapse during begin-play, before the player sees it. One clock (HoldSeconds) governs
 * both cuts and settling.
 *
 * "Held" is asserted on IsReleased, not displacement (DESIGN.md §4): bricks spawn kinematic and
 * only ABrickActor::Release makes them dynamic. The held structure must still be solved, since
 * an absent support answer reads as Falling (see World.Scenario.GameModeBuildsTheWallOnBeginPlay).
 * Held means solved but not settled.
 *
 * After the hold, the release count must exactly match the same row laid world-free through
 * DestructionScenarios::Build and settled with SolveAndBreak. Not an independent solver; it is
 * independent of the world wiring this slice changes.
 *
 * Rows:
 * - corbel-a-bare-4 (10 pieces): control, root reads 0.0223, must release nothing.
 * - corbel-e36 (519 pieces): was condemned, but reads ~0.145 at mean strength since the
 *   2026-08-14 re-anchor. A condemned replacement (~124 steps) is owed (CURRENT_STATE).
 * - corbel-e35 (496 pieces): world-free only; a second world adds nothing.
 * corbel-f-100 is omitted: too large, and covered by Core.Structure.AHundredStepCorbelMustComeDown.
 *
 * Needs a ticking world: the hold is measured in world seconds.
 */
namespace ScenarioHoldTestSupport
{
	using namespace DestructionLayout;

	/** A double printed at full precision. */
	inline FString ScenarioHoldBits(double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	/**
	 * One row this file checks. bCondemned is declared, then checked against the world-free
	 * reading, so a row whose status changes fails by name.
	 */
	struct FScenarioHoldCase
	{
		const TCHAR* Name;

		/** Piece count from World.Scenarios.CorbelRows' closed form. */
		int32 ExpectedPieces;

		/** Root joint over capacity as laid, so settling must release something. */
		bool bCondemned;

		/** False for a world-free-only row (E35). */
		bool bJoinAsLevel;
	};

	/*
	 * Since the mean re-anchor E36 reads ~0.145 (was 1.01625), so no row is condemned. A condemned
	 * replacement (~124 steps) is owed (CURRENT_STATE); until then CondemnedCases is zero.
	 */
	const FScenarioHoldCase ScenarioHoldCases[] =
	{
		{ TEXT("corbel-a-bare-4"), 10, false, true },
		{ TEXT("corbel-e35"), 496, false, false },
		{ TEXT("corbel-e36"), 519, false, true },
	};

	/** What settling a row does, world-free. */
	struct FScenarioHoldOracle
	{
		bool bBuilt = false;

		int32 Pieces = 0;
		int32 Joints = 0;

		/** SolveAndBreak passes that broke a joint. Zero for a standing structure. */
		int32 BreakPasses = 0;

		/** Pieces the settled solve no longer holds up; what ApplyResults releases. */
		int32 WouldRelease = 0;
	};

	/**
	 * Lay the row world-free, settle it, and count unheld pieces. Uses ApplyResults' predicate
	 * (Stranded and Falling come down; unanswered pieces never do), written out rather than shared
	 * so it cannot agree with a wrong production helper.
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
	 * Fraction of the row's hold at which "still held" is read. Leaves about half a second of
	 * margin on a 4 s hold, well above TickSeconds rounding.
	 */
	constexpr double ScenarioHoldMostOfTheHold = 0.875;

	/** Further fraction to tick so the level has certainly run: 1.25 holds in total. */
	constexpr double ScenarioHoldPastTheHold = 0.375;
}

/** Nothing is released while the level holds; exactly the world-free count is released when it runs. */
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

		// World-free reading of this row.

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

		// The row's declared status must match the world-free reading, both ways.
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

		// The level itself.

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

		// At begin-play: held as laid, and already solved.

		const int32 SolvesAtBeginPlay = Binding->GetStructure().NumSolves();

		AddInfo(FString::Printf(
			TEXT("'%s' at begin-play: %d of %d pieces live, %d bricks standing, %d released, %d ")
			TEXT("with no support answer, %d solve(s) so far"),
			Case.Name, Binding->GetStructure().NumLivePieces(), Binding->NumPieces(),
			ScenarioHoldLiveBrickCount(*Binding), ScenarioHoldReleasedCount(*Binding),
			ScenarioHoldUnansweredCount(*Binding), SolvesAtBeginPlay));

		// Nothing released is the mechanism reading of "held as laid" (not displacement, DESIGN.md §4).
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

		// Held must mean solved, not unsolved: an absent support answer reads as Falling.
		TestTrue(
			*FString::Printf(
				TEXT("AT BEGIN-PLAY '%s' must already be SOLVED while it is held — every one of its ")
				TEXT("%d live pieces must have a support answer, because an absent answer reads as ")
				TEXT("Falling; %d have none"),
				Case.Name, Binding->GetStructure().NumLivePieces(),
				ScenarioHoldUnansweredCount(*Binding)),
			ScenarioHoldUnansweredCount(*Binding) == 0);

		// Most of the way through the hold: still nothing.

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

		// Past the hold: the level has run and settled to the world-free count.

		const double AfterStart = FPlatformTime::Seconds();
		TestWorld.TickSeconds(Row->HoldSeconds * ScenarioHoldPastTheHold);
		const double AfterSeconds = FPlatformTime::Seconds() - AfterStart;

		const int32 ReleasedAfter = ScenarioHoldReleasedCount(*Binding);

		AddInfo(FString::Printf(
			TEXT("MEASURED: ticking past '%s''s hold took a further %.1f ms; %d released against ")
			TEXT("the catalogue's %d, %d solve(s) against %d at begin-play"),
			Case.Name, AfterSeconds * 1000.0, ReleasedAfter, Oracle.WouldRelease,
			Binding->GetStructure().NumSolves(), SolvesAtBeginPlay));

		// The solve count rising proves the level ran, even on a row that correctly releases nothing.
		TestTrue(
			*FString::Printf(
				TEXT("'%s' MUST HAVE RUN once its %s s hold expired: settling is a solve, so the ")
				TEXT("graph must have solved more than the %d times it had at begin-play; it has ")
				TEXT("solved %d"),
				Case.Name, *ScenarioHoldBits(Row->HoldSeconds), SolvesAtBeginPlay,
				Binding->GetStructure().NumSolves()),
			Binding->GetStructure().NumSolves() > SolvesAtBeginPlay);

		// Exact equality; an inequality would pass a level that dropped everything.
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
	 * The sweep needs both kinds: without a condemned row every release count is zero equals
	 * zero; without a standing row, dropping everything would pass. The condemned floor is
	 * suspended until the replacement condemned row lands (CURRENT_STATE); then restore
	 * CondemnedCases >= 1 here.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("fixture: this sweep must carry standing rows (condemned floor suspended at the ")
			TEXT("mean re-anchor — see the comment) — %d condemned as laid and %d standing as laid"),
			CondemnedCases, StandingCases),
		StandingCases >= 2);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
