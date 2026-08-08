// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "DestructionGameGameMode.h"
#include "HAL/PlatformTime.h"
#include "Tests/BrickWorldTestSupport.h"
#include "World/DestructionScenarios.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE GAME MODE SAYS WHICH SCENARIO IT BUILT, AND WHETHER IT WAS ASKED FOR OR FALLEN BACK TO.
 *
 * =====================================================================================
 * THE GAP THIS CLOSES, AND WHY IT IS STATE RATHER THAN A LOG LINE
 * =====================================================================================
 *
 * `IndexForOptionsAndMap` already computes `EScenarioSelection` and `World.Scenarios.Selection`
 * already pins every branch of it — but the game mode THROWS IT AWAY. Its own source says so:
 * "nothing reads `How` yet, and that is a gap rather than a decision". A headless smoke run on a
 * real `Lvl_FreeEnd40` confirmed it from the other end: the map loads, the wall builds, and
 * nothing anywhere says which scenario was chosen. A player who mistypes `?Scenario=` gets a
 * silently different wall and the log gives them no way to tell.
 *
 * WHAT IS SPECIFIED HERE IS THE STATE, NOT THE MESSAGE. A warning naming the valid rows and an
 * on-screen label are both thin reads of the same two values, and asserting on log TEXT is
 * fragile — a reworded string breaks a test that was never about the wording. So the pair
 * (which row, how it was reached) is the assertable shape, and whatever is printed sits
 * downstream of it.
 *
 * =====================================================================================
 * THE ROW ALONE CANNOT CARRY IT, WHICH IS THE WHOLE POINT
 * =====================================================================================
 *
 * A URL naming a scenario that does not exist and a URL naming nothing at all BOTH land on
 * `sandbox`. Indistinguishable, they are a player staring at the wrong wall wondering why. So the
 * two cases are asserted as a PAIR: same row, different `EScenarioSelection`. A test that only
 * checked the index would pass in full against a game mode that swallowed every typo, and a test
 * that only checked the enum would pass against one that fell back to the wrong row.
 *
 * =====================================================================================
 * AND IT IS ASSERTED ALONGSIDE WHAT WAS ACTUALLY BUILT
 * =====================================================================================
 *
 * A recorded row that disagreed with the wall in front of the player would be worse than no
 * record at all — it is a readout that lies. So each case also checks the piece count of the
 * structure the game mode actually stood up: 300 for the flush 7 x 40 wall, 1,220 for the 30 x 40
 * sandbox (40N + 20 pieces, since odd courses fill a half cell at each end with a half bat).
 *
 * NEEDS A TICKING WORLD: it needs a WORLD with begin-play run, because that is when the game mode
 * selects. It never ticks one — nothing here is about elapsed time.
 */
namespace ScenarioReportTestSupport
{
	using namespace DestructionScenarios;

	/** The rows slice A put in the catalogue, named rather than indexed. */
	const TCHAR* const ScenarioReportDefaultRowName = TEXT("sandbox");
	const TCHAR* const ScenarioReportOptionRowName = TEXT("free-end-40");

	/** 40N + 20: twenty even courses of N, twenty odd courses of N - 1 plus two half bats. */
	constexpr int32 ScenarioReportDefaultWallPieceCount = 1220;
	constexpr int32 ScenarioReportOptionWallPieceCount = 300;

	inline FString ScenarioReportHowName(EScenarioSelection How)
	{
		switch (How)
		{
		case EScenarioSelection::ByOption:              return TEXT("ByOption");
		case EScenarioSelection::ByMapName:             return TEXT("ByMapName");
		case EScenarioSelection::Default:               return TEXT("Default");
		case EScenarioSelection::OptionNamedNoScenario: return TEXT("OptionNamedNoScenario");
		default:                                        return TEXT("<unknown>");
		}
	}

	inline FString ScenarioReportRowName(int32 Index)
	{
		return Catalogue().IsValidIndex(Index)
			? Catalogue()[Index].Name.ToString()
			: FString::Printf(TEXT("<no row: %d>"), Index);
	}

	/** What one begin-play left behind: the row it chose, how it chose it, and what it stood up. */
	struct FScenarioReportOutcome
	{
		bool bRan = false;

		int32 Row = INDEX_NONE;
		EScenarioSelection How = EScenarioSelection::ByOption;

		int32 BuiltStructureId = INDEX_NONE;
		int32 BuiltPieces = 0;
	};

	/**
	 * Begin play once under the given URL options and read the game mode's own state back.
	 *
	 * NO PAWN, DELIBERATELY. Nothing here is about framing, and a level with nobody in it yet must
	 * still select and still build.
	 */
	inline FScenarioReportOutcome ScenarioReportRun(
		FAutomationTestBase& Test, const TCHAR* Label, const TCHAR* Options)
	{
		using namespace BrickWorldTestSupport;

		FScenarioReportOutcome Outcome;

		FBrickTestWorld TestWorld;

		if (Options != nullptr && FCString::Strlen(Options) > 0)
		{
			TestWorld.Wrapper.BeginPlayURL.AddOption(Options);
		}

		const double StartedAt = FPlatformTime::Seconds();

		if (!TestWorld.Begin(Test, ADestructionGameGameMode::StaticClass()))
		{
			return Outcome;
		}

		ADestructionGameGameMode* const GameMode =
			TestWorld.World->GetAuthGameMode<ADestructionGameGameMode>();

		if (GameMode == nullptr)
		{
			Test.AddError(FString::Printf(
				TEXT("%s: the world must be running ADestructionGameGameMode, it is running %s"),
				Label, *GetNameSafe(TestWorld.World->GetAuthGameMode())));

			TestWorld.End();
			return Outcome;
		}

		Outcome.bRan = true;
		Outcome.Row = GameMode->GetSelectedScenarioRow();
		Outcome.How = GameMode->GetScenarioSelection();
		Outcome.BuiltStructureId = GameMode->GetBuiltStructureId();

		if (const FStructureBinding* const Binding =
				TestWorld.Subsystem->Find(Outcome.BuiltStructureId))
		{
			Outcome.BuiltPieces = Binding->NumPieces();
		}

		Test.AddInfo(FString::Printf(
			TEXT("%s: begin-play under '%s' recorded row %d ('%s') selected %s, and stood up %d ")
			TEXT("pieces as structure %d, in %.1f ms"),
			Label, Options != nullptr ? Options : TEXT(""), Outcome.Row,
			*ScenarioReportRowName(Outcome.Row), *ScenarioReportHowName(Outcome.How),
			Outcome.BuiltPieces, Outcome.BuiltStructureId,
			(FPlatformTime::Seconds() - StartedAt) * 1000.0));

		TestWorld.End();

		return Outcome;
	}

	/** The row that name looks up, or INDEX_NONE with the reason reported. */
	inline int32 ScenarioReportRowNamed(FAutomationTestBase& Test, const TCHAR* Name)
	{
		const int32 Index = IndexOfName(FName(Name));

		if (!Catalogue().IsValidIndex(Index))
		{
			Test.AddError(FString::Printf(
				TEXT("fixture: the catalogue must carry a row named '%s'"), Name));
		}

		return Index;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScenarioReportSelectionTest,
	"DestructionGame.World.Scenario.GameModeRecordsWhichScenarioItBuilt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FScenarioReportSelectionTest::RunTest(const FString& Parameters)
{
	using namespace ScenarioReportTestSupport;
	using namespace DestructionScenarios;

	const int32 DefaultRow = ScenarioReportRowNamed(*this, ScenarioReportDefaultRowName);
	const int32 OptionRow = ScenarioReportRowNamed(*this, ScenarioReportOptionRowName);

	if (!Catalogue().IsValidIndex(DefaultRow) || !Catalogue().IsValidIndex(OptionRow))
	{
		return true;
	}

	TestTrue(
		*FString::Printf(
			TEXT("fixture: the default row ('%s', %d) and the option's row ('%s', %d) must be ")
			TEXT("DIFFERENT rows, or 'it built the one it was asked for' says nothing"),
			ScenarioReportDefaultRowName, DefaultRow, ScenarioReportOptionRowName, OptionRow),
		DefaultRow != OptionRow);

	/* --- ONE: a valid option records that row, and says it was asked for ------------------- */

	const FScenarioReportOutcome ByOption = ScenarioReportRun(
		*this, TEXT("VALID OPTION"),
		*FString::Printf(TEXT("Scenario=%s"), ScenarioReportOptionRowName));

	if (ByOption.bRan)
	{
		TestTrue(
			*FString::Printf(
				TEXT("?Scenario=%s must be RECORDED as row %d ('%s'); the game mode recorded %d ")
				TEXT("('%s')"),
				ScenarioReportOptionRowName, OptionRow, ScenarioReportOptionRowName,
				ByOption.Row, *ScenarioReportRowName(ByOption.Row)),
			ByOption.Row == OptionRow);

		TestTrue(
			*FString::Printf(
				TEXT("and it must be recorded as ByOption — the player asked for it by name; the ")
				TEXT("game mode recorded %s"),
				*ScenarioReportHowName(ByOption.How)),
			ByOption.How == EScenarioSelection::ByOption);

		/*
		 * AND THE RECORD MUST AGREE WITH THE WALL. A readout that named one row while the player
		 * looked at another is worse than no readout at all.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("and the wall in front of the player must be that row's — a flush 7 x 40 ")
				TEXT("running bond of %d pieces; it built %d"),
				ScenarioReportOptionWallPieceCount, ByOption.BuiltPieces),
			ByOption.BuiltPieces == ScenarioReportOptionWallPieceCount);
	}

	/* --- TWO: nothing named anything, so the default was taken DELIBERATELY ---------------- */

	const FScenarioReportOutcome ByDefault =
		ScenarioReportRun(*this, TEXT("NO OPTION"), nullptr);

	if (ByDefault.bRan)
	{
		TestTrue(
			*FString::Printf(
				TEXT("with nothing naming a scenario the game mode must record the default row %d ")
				TEXT("('%s'); it recorded %d ('%s')"),
				DefaultRow, ScenarioReportDefaultRowName, ByDefault.Row,
				*ScenarioReportRowName(ByDefault.Row)),
			ByDefault.Row == DefaultRow);

		/*
		 * `Default` AND NOT `ByMapName`. A code-built test world's GetMapName is the test world's
		 * own package name, which no catalogue row carries, so the map branch cannot fire here —
		 * which is exactly why selection was pulled out world-free and is pinned in every form a
		 * map name arrives in by World.Scenarios.Selection.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("and it must record Default — the game opening on its own wall, deliberately; ")
				TEXT("it recorded %s"),
				*ScenarioReportHowName(ByDefault.How)),
			ByDefault.How == EScenarioSelection::Default);

		TestTrue(
			*FString::Printf(
				TEXT("and it must have built the sandbox wall Play has always given you — %d ")
				TEXT("pieces; it built %d"),
				ScenarioReportDefaultWallPieceCount, ByDefault.BuiltPieces),
			ByDefault.BuiltPieces == ScenarioReportDefaultWallPieceCount);
	}

	/* --- THREE: a typo lands on the SAME row and must NOT read the same ------------------- */

	const FScenarioReportOutcome ByTypo = ScenarioReportRun(
		*this, TEXT("MISTYPED OPTION"), TEXT("Scenario=free-end-4O"));

	if (ByTypo.bRan)
	{
		TestTrue(
			*FString::Printf(
				TEXT("a mistyped ?Scenario= must still show the player SOMETHING — an empty world ")
				TEXT("is a worse failure than the default wall — so it must record the default row ")
				TEXT("%d ('%s'); it recorded %d ('%s')"),
				DefaultRow, ScenarioReportDefaultRowName, ByTypo.Row,
				*ScenarioReportRowName(ByTypo.Row)),
			ByTypo.Row == DefaultRow);

		TestTrue(
			*FString::Printf(
				TEXT("and it must record OptionNamedNoScenario — the row was a FALLBACK, not a ")
				TEXT("default; it recorded %s"),
				*ScenarioReportHowName(ByTypo.How)),
			ByTypo.How == EScenarioSelection::OptionNamedNoScenario);

		TestTrue(
			*FString::Printf(
				TEXT("and the fallback wall is the sandbox's %d pieces; it built %d"),
				ScenarioReportDefaultWallPieceCount, ByTypo.BuiltPieces),
			ByTypo.BuiltPieces == ScenarioReportDefaultWallPieceCount);
	}

	/* --- AND THE PAIR, WHICH IS THE CLAIM THE OTHER TWO ONLY SET UP ----------------------- */

	if (ByDefault.bRan && ByTypo.bRan)
	{
		TestTrue(
			*FString::Printf(
				TEXT("A TYPO AND A DELIBERATE DEFAULT LAND ON THE SAME ROW AND MUST BE ")
				TEXT("DISTINGUISHABLE FROM OUTSIDE — both recorded row %d ('%s'), and they read %s ")
				TEXT("and %s. Equal, they are a player staring at the wrong wall with nothing ")
				TEXT("anywhere to tell them why."),
				ByDefault.Row, *ScenarioReportRowName(ByDefault.Row),
				*ScenarioReportHowName(ByDefault.How), *ScenarioReportHowName(ByTypo.How)),
			ByDefault.Row == ByTypo.Row && ByDefault.How != ByTypo.How);
	}

	/* --- and no level ever records a row that does not exist ------------------------------ */

	const FScenarioReportOutcome* const AllRuns[] = { &ByOption, &ByDefault, &ByTypo };

	for (const FScenarioReportOutcome* const Run : AllRuns)
	{
		if (!Run->bRan)
		{
			continue;
		}

		TestTrue(
			*FString::Printf(
				TEXT("every begin-play must record a REAL catalogue row — a level always shows ")
				TEXT("something, so INDEX_NONE is never an answer; it recorded %d against %d row(s)"),
				Run->Row, Catalogue().Num()),
			Catalogue().IsValidIndex(Run->Row));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
