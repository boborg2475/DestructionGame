// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Math/RandomStream.h"
#include "World/DestructionScenarios.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Which scenario a level builds: the `Scenario=` option, else the map, else the default.
 *
 * WORLD-FREE, THE WHOLE REASON THIS FUNCTION EXISTS. Choosing a scenario is a lookup over two
 * strings. Left inside `BeginPlay` it would be reachable only by a world test, and a world test
 * cannot easily produce the inputs that matter here — `UWorld::GetMapName()` in a code-built
 * world answers with the test world's own package name, and no harness can make it say
 * `UEDPIE_0_Lvl_FreeEnd40`. Pulled out as a free function, every form a map name arrives in is
 * one row of a table.
 *
 * THE RULE, IN ORDER: (1) a `Scenario=` option on the URL wins, whatever the map says; (2)
 * otherwise the map name selects, case-insensitively; (3) otherwise `sandbox`; (4) an option
 * naming a scenario that does not exist also falls back to `sandbox`, but must be
 * distinguishable from clause 3. Clause 4 is two claims, both asserted: falling back rather than
 * building nothing (a level showing an empty world is worse than one showing the default wall —
 * a mistyping player gets something, immediately), and reporting the miss, since a fallback that
 * reads exactly like a deliberate default leaves that player with no idea why they see the wrong
 * wall. `EScenarioSelection::OptionNamedNoScenario` is that distinction; a test checking only the
 * returned index would pass an implementation that silently swallowed every typo.
 *
 * THE MAP NAME IS THE THING MOST LIKELY TO BREAK WHEN A HUMAN ACTUALLY PLAYS. `GetMapName()`
 * answers `Lvl_FreeEnd40` in a cooked game, `UEDPIE_0_Lvl_FreeEnd40` in PIE (instance number not
 * always 0), and a URL or streamed level hands over a long package path or a full object path.
 * Every one of those is the same map, and selecting on only one form would work for whoever
 * wrote it and silently give everyone else the wrong wall — cheap to pin, so pinned in all forms.
 *
 * AND A SWEEP, because the option parser's near misses are not things anyone would list by hand:
 * `?MyScenario=`, `?Scenarios=`, `?ScenarioX=` and a decoy that merely contains the word must all
 * fail to match, and a hand-rolled `Contains(TEXT("Scenario="))` matches every one of them while
 * passing every example row above. The sweep builds option strings out of decoy and real keys in
 * a deterministic, seeded order and asserts invariants rather than recomputing the answer — an
 * oracle that reimplemented the rule would agree with a wrong implementation as readily as a
 * right one.
 *
 * Needs a ticking world: no, nor a world at all — two strings in, an index and an enum out.
 */
namespace ScenarioSelectionTestSupport
{
	using namespace DestructionScenarios;

	/** The two rows slice A put in the catalogue, named rather than indexed. */
	const TCHAR* const ScenarioSelectionDefaultRowName = TEXT("sandbox");
	const TCHAR* const ScenarioSelectionCutRowName = TEXT("free-end-40");

	/** The maps those two rows are reached by, transcribed rather than imported. */
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

	/** The row an index names, printable even when the index names nothing. */
	inline FString ScenarioSelectionRowName(int32 Index)
	{
		return Catalogue().IsValidIndex(Index)
			? Catalogue()[Index].Name.ToString()
			: FString::Printf(TEXT("<no row: %d>"), Index);
	}

	/** One example: two strings in, a row name and a reason out. */
	struct FScenarioSelectionRow
	{
		const TCHAR* Why;
		const TCHAR* Options;
		const TCHAR* MapName;
		const TCHAR* ExpectedRowName;
		EScenarioSelection ExpectedHow;
	};

	/* The sweep's alphabets. Decoy keys that must not match, and each is a real near miss rather
	 * than a random string: `Game`, `Listen` and `SplitJoin` are options the engine itself puts
	 * on a URL, and the other four are the ways a substring test for "Scenario" goes wrong. None
	 * of them is `Scenario` under a case fold, so "must not match" is a sound claim about all of
	 * them at once. */
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

	/** Where a map name can be reached from — a bare name, a package path, a full object path. */
	const TCHAR* const ScenarioSelectionPathPrefixes[] =
	{
		TEXT(""),
		TEXT("/Game/Maps/"),
		TEXT("/Game/Maps/Scenarios/"),
	};

	/** PIE stamps `UEDPIE_<instance>_` onto the ASSET name, inside the path. */
	const TCHAR* const ScenarioSelectionPiePrefixes[] =
	{
		TEXT(""),
		TEXT("UEDPIE_0_"),
		TEXT("UEDPIE_7_"),
		TEXT("UEDPIE_11_"),
	};

	/** Neither a URL nor GetMapName guarantees the case the catalogue was typed in. */
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

	/* --- the fixture: the two rows this file is written about must be there --------------- */

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

	/* --- the worked examples ------------------------------------------------------------- */

	const FScenarioSelectionRow ExampleRows[] =
	{
		/* Nothing names anything: the game opens on its default wall, deliberately. */
		{ TEXT("no options and no map"),
			TEXT(""), TEXT(""),
			ScenarioSelectionDefaultRowName, EScenarioSelection::Default },

		{ TEXT("a map no row carries"),
			TEXT(""), TEXT("Lvl_NoSuchMap"),
			ScenarioSelectionDefaultRowName, EScenarioSelection::Default },

		/* The map selects, in every form it arrives in: the bare asset name is the cooked game;
		 * the UEDPIE_ prefix is PIE, whose instance number is not always zero; the package path
		 * and the full object path are what a URL and a streamed level hand over. */
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

		/* The option wins over the map, asserted in both directions: only checking that
		 * `?Scenario=free-end-40` beats `Lvl_Sandbox` would pass an implementation that simply
		 * preferred the cut row; the second row is the same claim with the answers swapped. */
		{ TEXT("an option overriding the map"),
			TEXT("?Scenario=free-end-40"), TEXT("Lvl_Sandbox"),
			ScenarioSelectionCutRowName, EScenarioSelection::ByOption },

		{ TEXT("an option overriding the map, the other way round"),
			TEXT("?Scenario=sandbox"), TEXT("Lvl_FreeEnd40"),
			ScenarioSelectionDefaultRowName, EScenarioSelection::ByOption },

		/* FName comparison is case-insensitive and FString::operator== is too, so neither the
		 * scenario's name nor the option's key has to be typed the way the catalogue spells it.
		 * That comes free from using the engine's own parser, which is why the engine's own
		 * parser is the requirement rather than a second one written here. */
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

		/* Repeated, the first wins — UGameplayStatics::ParseOption's own behaviour. Pinning it is
		 * what "use the engine's parser rather than writing a second one" means. */
		{ TEXT("the option given twice — the first wins, as ParseOption does"),
			TEXT("?Scenario=free-end-40?Scenario=sandbox"), TEXT(""),
			ScenarioSelectionCutRowName, EScenarioSelection::ByOption },

		/* Keys that merely look like the option: a Contains(TEXT("Scenario=")) passes every row
		 * above and fails all three of these. */
		{ TEXT("a key that ENDS with the option's name"),
			TEXT("?MyScenario=free-end-40"), TEXT("Lvl_Sandbox"),
			ScenarioSelectionDefaultRowName, EScenarioSelection::ByMapName },

		{ TEXT("a key that BEGINS with the option's name"),
			TEXT("?Scenarios=free-end-40"), TEXT("Lvl_Sandbox"),
			ScenarioSelectionDefaultRowName, EScenarioSelection::ByMapName },

		{ TEXT("a key one letter longer"),
			TEXT("?ScenarioX=free-end-40"), TEXT("Lvl_FreeEnd40"),
			ScenarioSelectionCutRowName, EScenarioSelection::ByMapName },

		/* And the clause this whole function is for. Both of these name a scenario that is not
		 * there, so both must land on the default row and say so — the returned index alone is
		 * identical to the deliberate default two rows above, and a mistyping player needs to be
		 * told rather than left wondering which wall they are looking at. The empty value is the
		 * second half: `?Scenario=` with nothing after it is a typo, not an absent option, and
		 * must not fall through to the map. The map here is the cut row's, so an implementation
		 * that treated the miss as "no option given" would return the cut row and fail on the
		 * index as well as the reason. */
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

	/* --- the sweep: the invariants, over generated URLs and map names -------------------- */

	/** Seeded and fixed, so the suite runs the same cases every time and a failure names the one
	 * case to reproduce — a fresh seed per run is a flaky test, worse than none (discipline
	 * borrowed from Tests/StructureFuzzTest.cpp). */
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

		/* The option half: a random number of decoy options, and — half the time — one real
		 * `Scenario=` among them at a random position, naming either a row that exists or one
		 * that does not. Both key and value are re-cased at random, since neither a URL nor a
		 * person types either the way the catalogue does. */
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

		/* The map half, decorated the way the engine decorates one: a package path, a PIE
		 * instance stamp on the asset name, an optional object suffix, and any casing. */
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

		/* Invariant one, the fail-closed one: whatever arrives, a valid row comes back. A level
		 * that builds nothing at all is the outcome this function exists to make unreachable. */
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

		/* INVARIANT TWO: an option naming a row wins, whatever surrounds it and whatever the map says. */
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

		/* INVARIANT THREE: with no real option, the map decides — in every decoration of it. */
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

		/* Invariant four: the reason and the index cannot disagree. Both fallback reasons must
		 * land on the default row, and neither naming reason may land anywhere but the row it
		 * claims to have been given. */
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
