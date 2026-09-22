// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Math/RandomStream.h"
#include "World/DestructionScenarios.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Which scenario a level builds. World-free, so every form a map name arrives in is a table row.
 *
 * Rule: (1) a `Scenario=` URL option wins; (2) else the map name selects, case-insensitively;
 * (3) else `sandbox`; (4) an option naming no scenario also falls back to `sandbox` but reports
 * OptionNamedNoScenario, so a typo is distinguishable from the deliberate default.
 *
 * Map names arrive bare (cooked), with a UEDPIE_<n>_ prefix (PIE), or as a package or object path;
 * all forms are pinned.
 *
 * A seeded sweep generates URLs with near-miss keys (`MyScenario`, `Scenarios`, ...) that a naive
 * Contains("Scenario=") would match, and asserts invariants rather than recomputing the answer.
 *
 * No world needed.
 */
namespace ScenarioSelectionTestSupport
{
	using namespace DestructionScenarios;

	/** The two catalogue rows this file uses, by name. */
	const TCHAR* const ScenarioSelectionDefaultRowName = TEXT("sandbox");
	const TCHAR* const ScenarioSelectionCutRowName = TEXT("free-end-40");

	/** Their maps, transcribed rather than imported. */
	const TCHAR* const ScenarioSelectionDefaultMapName = TEXT("Lvl_Sandbox");
	const TCHAR* const ScenarioSelectionCutMapName = TEXT("Lvl_FreeEnd40");

	inline FString ScenarioSelectionHowName(EScenarioSelection How)
	{
		switch (How)
		{
		case EScenarioSelection::ByOption:             return TEXT("ByOption");
		case EScenarioSelection::ByMapName:            return TEXT("ByMapName");
		case EScenarioSelection::Default:              return TEXT("Default");
		case EScenarioSelection::OptionNamedNoScenario: return TEXT("OptionNamedNoScenario");
		default:                                       return TEXT("<unknown>");
		}
	}

	/** Printable row name for an index, including invalid ones. */
	inline FString ScenarioSelectionRowName(int32 Index)
	{
		return Catalogue().IsValidIndex(Index)
			? Catalogue()[Index].Name.ToString()
			: FString::Printf(TEXT("<no row: %d>"), Index);
	}

	/** One example: options and map in, row name and reason out. */
	struct FScenarioSelectionRow
	{
		const TCHAR* Why;
		const TCHAR* Options;
		const TCHAR* MapName;
		const TCHAR* ExpectedRowName;
		EScenarioSelection ExpectedHow;
	};

	/*
	 * Decoy keys that must not match: engine options (`Game`, `Listen`, `SplitJoin`) and four
	 * near misses a substring test for "Scenario" would wrongly accept.
	 */
	const TCHAR* const ScenarioSelectionDecoyKeys[] =
	{
		TEXT("Game"),
		TEXT("Listen"),
		TEXT("SplitJoin"),
		TEXT("MyScenario"),
		TEXT("Scenarios"),
		TEXT("ScenarioX"),
		TEXT("XScenario"),
	};

	/** Path prefixes a map name can arrive with. */
	const TCHAR* const ScenarioSelectionPathPrefixes[] =
	{
		TEXT(""),
		TEXT("/Game/Maps/"),
		TEXT("/Game/Maps/Scenarios/"),
	};

	/** PIE prefixes `UEDPIE_<instance>_` to the asset name, inside the path. */
	const TCHAR* const ScenarioSelectionPiePrefixes[] =
	{
		TEXT(""),
		TEXT("UEDPIE_0_"),
		TEXT("UEDPIE_7_"),
		TEXT("UEDPIE_11_"),
	};

	/** Recase a string: 0 as-is, 1 upper, 2 lower. Input case is not guaranteed. */
	inline FString ScenarioSelectionRecase(const FString& Value, int32 Which)
	{
		switch (Which)
		{
		case 1:  return Value.ToUpper();
		case 2:  return Value.ToLower();
		default: return Value;
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScenarioSelectionTest,
	"DestructionGame.World.Scenarios.Selection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FScenarioSelectionTest::RunTest(const FString& Parameters)
{
	using namespace ScenarioSelectionTestSupport;
	using namespace DestructionScenarios;

	// Fixture: both rows must exist.

	const int32 DefaultRow = IndexOfName(FName(ScenarioSelectionDefaultRowName));
	const int32 CutRow = IndexOfName(FName(ScenarioSelectionCutRowName));

	if (!Catalogue().IsValidIndex(DefaultRow) || !Catalogue().IsValidIndex(CutRow))
	{
		AddError(FString::Printf(
			TEXT("fixture: the catalogue must carry '%s' (it is row %d) and '%s' (row %d)"),
			ScenarioSelectionDefaultRowName, DefaultRow,
			ScenarioSelectionCutRowName, CutRow));

		return true;
	}

	TestTrue(
		*FString::Printf(
			TEXT("fixture: '%s' must be the FIRST row — it is the fallback every miss lands on, ")
			TEXT("and it is row %d"),
			ScenarioSelectionDefaultRowName, DefaultRow),
		DefaultRow == 0);

	// Worked examples.

	const FScenarioSelectionRow ExampleRows[] =
	{
		// Nothing names anything: the default, deliberately.
		{ TEXT("no options and no map"),
			TEXT(""), TEXT(""),
			ScenarioSelectionDefaultRowName, EScenarioSelection::Default },

		{ TEXT("a map no row carries"),
			TEXT(""), TEXT("Lvl_NoSuchMap"),
			ScenarioSelectionDefaultRowName, EScenarioSelection::Default },

		// The map selects in every form: bare (cooked), UEDPIE_<n>_ (PIE), package or object path.
		{ TEXT("the default row's own map"),
			TEXT(""), TEXT("Lvl_Sandbox"),
			ScenarioSelectionDefaultRowName, EScenarioSelection::ByMapName },

		{ TEXT("the cut row's own map, as a cooked game gives it"),
			TEXT(""), TEXT("Lvl_FreeEnd40"),
			ScenarioSelectionCutRowName, EScenarioSelection::ByMapName },

		{ TEXT("the same map in lower case"),
			TEXT(""), TEXT("lvl_freeend40"),
			ScenarioSelectionCutRowName, EScenarioSelection::ByMapName },

		{ TEXT("the same map in upper case"),
			TEXT(""), TEXT("LVL_FREEEND40"),
			ScenarioSelectionCutRowName, EScenarioSelection::ByMapName },

		{ TEXT("the same map as PIE instance 0 gives it"),
			TEXT(""), TEXT("UEDPIE_0_Lvl_FreeEnd40"),
			ScenarioSelectionCutRowName, EScenarioSelection::ByMapName },

		{ TEXT("the same map as PIE instance 11 gives it"),
			TEXT(""), TEXT("UEDPIE_11_Lvl_FreeEnd40"),
			ScenarioSelectionCutRowName, EScenarioSelection::ByMapName },

		{ TEXT("the same map as a long package path"),
			TEXT(""), TEXT("/Game/Maps/Scenarios/Lvl_FreeEnd40"),
			ScenarioSelectionCutRowName, EScenarioSelection::ByMapName },

		{ TEXT("the same map as a PIE-prefixed package path"),
			TEXT(""), TEXT("/Game/Maps/Scenarios/UEDPIE_0_Lvl_FreeEnd40"),
			ScenarioSelectionCutRowName, EScenarioSelection::ByMapName },

		{ TEXT("the same map as a full object path, the form DefaultEngine.ini uses"),
			TEXT(""), TEXT("/Game/Maps/Scenarios/Lvl_FreeEnd40.Lvl_FreeEnd40"),
			ScenarioSelectionCutRowName, EScenarioSelection::ByMapName },

		{ TEXT("a PIE prefix with no map after it"),
			TEXT(""), TEXT("UEDPIE_0_"),
			ScenarioSelectionDefaultRowName, EScenarioSelection::Default },

		{ TEXT("a path with no map after it"),
			TEXT(""), TEXT("/Game/Maps/"),
			ScenarioSelectionDefaultRowName, EScenarioSelection::Default },

		// The option beats the map, in both directions so no row is simply preferred.
		{ TEXT("an option overriding the map"),
			TEXT("?Scenario=free-end-40"), TEXT("Lvl_Sandbox"),
			ScenarioSelectionCutRowName, EScenarioSelection::ByOption },

		{ TEXT("an option overriding the map, the other way round"),
			TEXT("?Scenario=sandbox"), TEXT("Lvl_FreeEnd40"),
			ScenarioSelectionDefaultRowName, EScenarioSelection::ByOption },

		// Key and value are case-insensitive, which the engine's own parser provides.
		{ TEXT("an option value in upper case"),
			TEXT("?Scenario=FREE-END-40"), TEXT(""),
			ScenarioSelectionCutRowName, EScenarioSelection::ByOption },

		{ TEXT("an option key in lower case"),
			TEXT("?scenario=free-end-40"), TEXT(""),
			ScenarioSelectionCutRowName, EScenarioSelection::ByOption },

		{ TEXT("an option among the engine's own"),
			TEXT("?Game=/Script/Engine.GameModeBase?Scenario=free-end-40?Listen"),
			TEXT("Lvl_Sandbox"),
			ScenarioSelectionCutRowName, EScenarioSelection::ByOption },

		{ TEXT("an option last on the URL"),
			TEXT("?Listen?Scenario=free-end-40"), TEXT("Lvl_Sandbox"),
			ScenarioSelectionCutRowName, EScenarioSelection::ByOption },

		// Repeated, the first wins, as UGameplayStatics::ParseOption does.
		{ TEXT("the option given twice — the first wins, as ParseOption does"),
			TEXT("?Scenario=free-end-40?Scenario=sandbox"), TEXT(""),
			ScenarioSelectionCutRowName, EScenarioSelection::ByOption },

		// Keys that only look like the option; a Contains("Scenario=") check fails these.
		{ TEXT("a key that ENDS with the option's name"),
			TEXT("?MyScenario=free-end-40"), TEXT("Lvl_Sandbox"),
			ScenarioSelectionDefaultRowName, EScenarioSelection::ByMapName },

		{ TEXT("a key that BEGINS with the option's name"),
			TEXT("?Scenarios=free-end-40"), TEXT("Lvl_Sandbox"),
			ScenarioSelectionDefaultRowName, EScenarioSelection::ByMapName },

		{ TEXT("a key one letter longer"),
			TEXT("?ScenarioX=free-end-40"), TEXT("Lvl_FreeEnd40"),
			ScenarioSelectionCutRowName, EScenarioSelection::ByMapName },

		/*
		 * An option naming no scenario (including an empty value) falls back to the default and
		 * reports it. The map is the cut row's, so treating the miss as "no option" also fails.
		 */
		{ TEXT("an option naming a scenario that does not exist"),
			TEXT("?Scenario=no-such-scenario"), TEXT("Lvl_FreeEnd40"),
			ScenarioSelectionDefaultRowName, EScenarioSelection::OptionNamedNoScenario },

		{ TEXT("an option with no value at all"),
			TEXT("?Scenario="), TEXT("Lvl_FreeEnd40"),
			ScenarioSelectionDefaultRowName, EScenarioSelection::OptionNamedNoScenario },
	};

	for (const FScenarioSelectionRow& Row : ExampleRows)
	{
		const int32 Expected = IndexOfName(FName(Row.ExpectedRowName));

		EScenarioSelection How = EScenarioSelection::ByOption;

		const int32 Got = IndexForOptionsAndMap(
			FString(Row.Options), FString(Row.MapName), How);

		TestTrue(
			*FString::Printf(
				TEXT("%s: options '%s' and map '%s' must select '%s' (row %d); they selected %s ")
				TEXT("(row %d)"),
				Row.Why, Row.Options, Row.MapName, Row.ExpectedRowName, Expected,
				*ScenarioSelectionRowName(Got), Got),
			Got == Expected);

		TestTrue(
			*FString::Printf(
				TEXT("%s: options '%s' and map '%s' must report %s — a fallback that reads like a ")
				TEXT("deliberate default leaves the player with no way to tell; it reported %s"),
				Row.Why, Row.Options, Row.MapName,
				*ScenarioSelectionHowName(Row.ExpectedHow), *ScenarioSelectionHowName(How)),
			How == Row.ExpectedHow);
	}

	/*
	 * The sweep: invariants over generated URLs and map names. The seed is fixed so every run is
	 * the same and a failure is reproducible.
	 */
	constexpr int32 ScenarioSelectionSweepSeed = 20260807;
	constexpr int32 ScenarioSelectionSweepCases = 600;

	FRandomStream Stream(ScenarioSelectionSweepSeed);

	int32 SweptCases = 0;
	int32 NoRowFailures = 0;
	int32 OptionFailures = 0;
	int32 MapFailures = 0;
	int32 AgreementFailures = 0;

	for (int32 Case = 0; Case < ScenarioSelectionSweepCases; ++Case)
	{
		++SweptCases;

		/*
		 * Options: up to three decoys and, half the time, one real `Scenario=` at a random slot,
		 * naming a real or missing row. Key and value are randomly recased.
		 */
		const int32 DecoyCount = Stream.RandRange(0, 3);
		const int32 RealOptionAt = Stream.RandRange(0, DecoyCount);
		const bool bHasRealOption = Stream.RandRange(0, 1) == 1;
		const bool bRealOptionNamesARow = Stream.RandRange(0, 3) > 0;

		const int32 NamedRow = Stream.RandRange(0, Catalogue().Num() - 1);

		const FString RealOptionValue = bRealOptionNamesARow
			? ScenarioSelectionRecase(Catalogue()[NamedRow].Name.ToString(), Stream.RandRange(0, 2))
			: FString::Printf(TEXT("no-such-scenario-%d"), Case);

		FString Options;

		for (int32 Slot = 0; Slot <= DecoyCount; ++Slot)
		{
			if (bHasRealOption && Slot == RealOptionAt)
			{
				Options += TEXT("?");
				Options += ScenarioSelectionRecase(TEXT("Scenario"), Stream.RandRange(0, 2));
				Options += TEXT("=");
				Options += RealOptionValue;
			}

			if (Slot < DecoyCount)
			{
				const int32 Decoy = Stream.RandRange(
					0, static_cast<int32>(UE_ARRAY_COUNT(ScenarioSelectionDecoyKeys)) - 1);

				Options += TEXT("?");
				Options += ScenarioSelectionDecoyKeys[Decoy];
				Options += TEXT("=");
				Options += RealOptionValue;
			}
		}

		// Map: optional package path, PIE prefix, object suffix, and random case.
		const bool bMapNamesARow = Stream.RandRange(0, 3) > 0;
		const int32 MapRow = Stream.RandRange(0, Catalogue().Num() - 1);

		const FString BareMapName = bMapNamesARow
			? FString(Catalogue()[MapRow].MapName)
			: FString::Printf(TEXT("Lvl_NoSuchMap%d"), Case);

		const FString AssetName =
			FString(ScenarioSelectionPiePrefixes[Stream.RandRange(
				0, static_cast<int32>(UE_ARRAY_COUNT(ScenarioSelectionPiePrefixes)) - 1)])
			+ BareMapName;

		const FString PathPrefix = ScenarioSelectionPathPrefixes[Stream.RandRange(
			0, static_cast<int32>(UE_ARRAY_COUNT(ScenarioSelectionPathPrefixes)) - 1)];

		const bool bObjectPath = !PathPrefix.IsEmpty() && Stream.RandRange(0, 1) == 1;

		const FString MapName = ScenarioSelectionRecase(
			PathPrefix + AssetName + (bObjectPath ? TEXT(".") + AssetName : FString()),
			Stream.RandRange(0, 2));

		EScenarioSelection How = EScenarioSelection::ByOption;

		const int32 Got = IndexForOptionsAndMap(Options, MapName, How);

		const FString Where = FString::Printf(
			TEXT("seed %d, case %d: options '%s', map '%s' -> row %s (%d), %s"),
			ScenarioSelectionSweepSeed, Case, *Options, *MapName,
			*ScenarioSelectionRowName(Got), Got, *ScenarioSelectionHowName(How));

		// Invariant one (fail-closed): a valid row always comes back.
		if (!Catalogue().IsValidIndex(Got))
		{
			if (NoRowFailures == 0)
			{
				AddError(FString::Printf(
					TEXT("EVERY input must select a real scenario — a level showing an empty world ")
					TEXT("is worse than one showing the default wall. %s"),
					*Where));
			}

			++NoRowFailures;
			continue;
		}

		// Invariant two: an option naming a row wins, whatever else is present.
		if (bHasRealOption && bRealOptionNamesARow
			&& (Got != NamedRow || How != EScenarioSelection::ByOption))
		{
			if (OptionFailures == 0)
			{
				AddError(FString::Printf(
					TEXT("an option naming '%s' must select it (row %d) and report ByOption, ")
					TEXT("whatever else is on the URL and whatever the map says. %s"),
					*Catalogue()[NamedRow].Name.ToString(), NamedRow, *Where));
			}

			++OptionFailures;
		}

		// Invariant three: with no real option, the map decides in every form.
		if (!bHasRealOption && bMapNamesARow
			&& (Got != MapRow || How != EScenarioSelection::ByMapName))
		{
			if (MapFailures == 0)
			{
				AddError(FString::Printf(
					TEXT("with no Scenario option, map '%s' must select row %d and report ")
					TEXT("ByMapName — decoy keys are not the option and a PIE prefix, a package ")
					TEXT("path and a case change are all the same map. %s"),
					*BareMapName, MapRow, *Where));
			}

			++MapFailures;
		}

		// Invariant four: both fallback reasons land on the default row.
		const bool bAgrees =
			(How == EScenarioSelection::Default || How == EScenarioSelection::OptionNamedNoScenario)
				? Got == DefaultRow
				: true;

		if (!bAgrees)
		{
			if (AgreementFailures == 0)
			{
				AddError(FString::Printf(
					TEXT("a reported fallback must land on the default row %d ('%s'). %s"),
					DefaultRow, ScenarioSelectionDefaultRowName, *Where));
			}

			++AgreementFailures;
		}
	}

	AddInfo(FString::Printf(
		TEXT("swept %d generated URLs at seed %d: %d selected no row, %d ignored their option, ")
		TEXT("%d ignored their map, %d reported a fallback onto the wrong row"),
		SweptCases, ScenarioSelectionSweepSeed, NoRowFailures, OptionFailures, MapFailures,
		AgreementFailures));

	TestTrue(
		*FString::Printf(
			TEXT("every one of the %d swept URLs must select a real scenario; %d did not"),
			SweptCases, NoRowFailures),
		NoRowFailures == 0);

	TestTrue(
		*FString::Printf(
			TEXT("every swept URL whose option names a row must select that row; %d did not"),
			OptionFailures),
		OptionFailures == 0);

	TestTrue(
		*FString::Printf(
			TEXT("every swept URL with no option must select on its map; %d did not"),
			MapFailures),
		MapFailures == 0);

	TestTrue(
		*FString::Printf(
			TEXT("every swept fallback must land on the default row; %d did not"),
			AgreementFailures),
		AgreementFailures == 0);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
