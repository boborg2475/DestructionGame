// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "DestructionGameGameMode.h"
#include "HAL/PlatformTime.h"
#include "Tests/BrickWorldTestSupport.h"
#include "World/DestructionScenarios.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * The game mode records which scenario row it built and how it was selected
 * (EScenarioSelection). State is asserted, not log text. A typo and no option both land on
 * `sandbox`, so they are asserted as a pair: same row, different selection. Each case also
 * checks the built piece count matches the recorded row. Needs begin-play; never ticks.
 */
namespace ScenarioReportTestSupport
{
	using namespace DestructionScenarios;

	const TCHAR* const ScenarioReportDefaultRowName = TEXT("sandbox");
	const TCHAR* const ScenarioReportOptionRowName = TEXT("free-end-40");

	/** 40N + 20 pieces for a flush 40-course wall N bricks wide (N = 30 and 7). */
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

	/** What one begin-play recorded and built. */
	struct FScenarioReportOutcome
	{
		bool bRan = false;

		int32 Row = INDEX_NONE;
		EScenarioSelection How = EScenarioSelection::ByOption;

		int32 BuiltStructureId = INDEX_NONE;
		int32 BuiltPieces = 0;
	};

	/** Begin play once under the given URL options and read back the game mode's state. No pawn. */
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

	/** The row with that name, or INDEX_NONE with an error. */
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

	// A valid option records that row, ByOption.

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

		// The record must match the wall actually built.
		TestTrue(
			*FString::Printf(
				TEXT("and the wall in front of the player must be that row's — a flush 7 x 40 ")
				TEXT("running bond of %d pieces; it built %d"),
				ScenarioReportOptionWallPieceCount, ByOption.BuiltPieces),
			ByOption.BuiltPieces == ScenarioReportOptionWallPieceCount);
	}

	// No option: the default row, recorded as Default.

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
		 * Default, not ByMapName: a code-built world's map name matches no row. Map-name
		 * selection is covered world-free by World.Scenarios.Selection.
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

	// A typo falls back to the same row, recorded as OptionNamedNoScenario.

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

	// The pair: same row, distinguishable selection.

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

	// Every run records a real row.

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
