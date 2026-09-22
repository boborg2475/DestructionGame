// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Engine/World.h"
#include "Misc/PackageName.h"
#include "UObject/PrimaryAssetId.h"
#include "World/DestructionScenarios.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Every catalogue row's MapName names a .umap that exists, and that map selects the row back.
 * Nothing else checks the file exists (Content.RequiredAssetsResolve covers only C++ and CDO
 * references). Paths are derived from each row rather than listed, so a new row without a map
 * fails here. Checked with DoesPackageExist, which loads nothing. The round trip uses the found
 * package path, in every name form ScenarioSelectionTest.cpp covers (bare, PIE-prefixed, package
 * and object paths), since a map selecting the wrong row is worse than a missing one. No world.
 */
namespace ScenarioMapTestSupport
{
	using namespace DestructionScenarios;

	/**
	 * Folders a scenario map may live in: Scenarios/ for scenario levels, and Maps/ for
	 * Lvl_Sandbox. FunctionalTests/ is excluded because it is never cooked.
	 */
	const TCHAR* const ScenarioMapFolders[] =
	{
		TEXT("/Game/Maps/Scenarios/"),
		TEXT("/Game/Maps/"),
	};

	/** PIE stamps `UEDPIE_<instance>_` onto the asset name. */
	const TCHAR* const ScenarioMapPiePrefixes[] =
	{
		TEXT("UEDPIE_0_"),
		TEXT("UEDPIE_7_"),
		TEXT("UEDPIE_11_"),
	};

	inline FString ScenarioMapHowName(EScenarioSelection How)
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

	inline FString ScenarioMapRowName(int32 Index)
	{
		return Catalogue().IsValidIndex(Index)
			? Catalogue()[Index].Name.ToString()
			: FString::Printf(TEXT("<no row: %d>"), Index);
	}

	/** What was found on disk for one row, and everywhere that was looked. */
	struct FScenarioMapLocation
	{
		bool bFound = false;

		/** The long package name that exists, e.g. `/Game/Maps/Scenarios/Lvl_CorbelE35`. */
		FString PackageName;

		/** The file on disk. */
		FString Filename;

		/** Every candidate tried, for the failure message. */
		FString Tried;
	};

	/** A row's .umap under each allowed folder. The first hit wins; every candidate is recorded. */
	inline FScenarioMapLocation ScenarioMapFindPackage(const TCHAR* MapName)
	{
		FScenarioMapLocation Location;

		for (const TCHAR* const Folder : ScenarioMapFolders)
		{
			const FString Candidate = FString(Folder) + MapName;

			if (!Location.Tried.IsEmpty())
			{
				Location.Tried += TEXT(", ");
			}

			Location.Tried += FString::Printf(TEXT("'%s.umap'"), *Candidate);

			if (Location.bFound)
			{
				continue;
			}

			FString Filename;

			if (FPackageName::DoesPackageExist(Candidate, &Filename))
			{
				Location.bFound = true;
				Location.PackageName = Candidate;
				Location.Filename = Filename;
			}
		}

		return Location;
	}

	/** Root scanned for scenario maps; the scan is recursive, so it covers both folders. */
	const TCHAR* const ScenarioMapScanRoot = TEXT("/Game/Maps");

	/**
	 * Floor on UWorlds the registry reports. An unscanned registry returns nothing, and the per-row
	 * checks would pass vacuously. 29: 28 scenario maps plus Lvl_Sandbox.
	 */
	constexpr int32 ScenarioMapWorldAssetFloor = 29;

	/** What the registry knows about one row's map, without loading it. */
	struct FScenarioMapAsset
	{
		bool bResolved = false;

		/** The long package name that exists, e.g. `/Game/Maps/Scenarios/Lvl_Wall01`. */
		FString PackageName;

		/** The package's short name, which the UWorld inside must share. */
		FString ExpectedWorldName;

		/** The UWorld's actual name. */
		FString ActualWorldName;

		FPrimaryAssetId PrimaryAssetId;
	};

	/** Every UWorld the registry reports in one package. */
	inline void ScenarioMapWorldsInPackage(
		const IAssetRegistry& Registry, const FString& PackageName, TArray<FAssetData>& Out)
	{
		Out.Reset();

		TArray<FAssetData> InPackage;

		// On-disk only: the claim is about the file, not a loaded world.
		Registry.GetAssetsByPackageName(
			FName(*PackageName), InPackage, /*bIncludeOnlyOnDiskAssets*/ true);

		const FTopLevelAssetPath WorldClassPath = UWorld::StaticClass()->GetClassPathName();

		for (const FAssetData& Asset : InPackage)
		{
			if (Asset.AssetClassPath == WorldClassPath)
			{
				Out.Add(Asset);
			}
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScenarioMapsExistTest,
	"DestructionGame.Content.ScenarioMapsExist",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FScenarioMapsExistTest::RunTest(const FString& Parameters)
{
	using namespace ScenarioMapTestSupport;
	using namespace DestructionScenarios;

	const TArray<FScenario>& Rows = Catalogue();

	/*
	 * Floor, not equality, so the sweep cannot pass over an empty catalogue while rows can still be
	 * added. 29: nine scenario-slice rows plus the twenty acceptance walls.
	 */
	constexpr int32 ScenarioMapRowFloor = 29;

	TestTrue(
		*FString::Printf(
			TEXT("fixture: the catalogue must carry at least the %d rows the scenario slices and the ")
			TEXT("twenty acceptance walls leave, or this sweeps nothing; it carries %d"),
			ScenarioMapRowFloor, Rows.Num()),
		Rows.Num() >= ScenarioMapRowFloor);

	TArray<FScenarioMapLocation> Located;
	Located.Reserve(Rows.Num());

	int32 Missing = 0;

	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		const FScenario& Row = Rows[Index];

		const FString Label = FString::Printf(TEXT("row %d ('%s')"), Index, *Row.Name.ToString());

		if (Row.MapName == nullptr || FCString::Strlen(Row.MapName) == 0)
		{
			AddError(FString::Printf(
				TEXT("%s names no map at all, so there is nothing to look for"), *Label));

			Located.AddDefaulted();
			++Missing;

			continue;
		}

		const FScenarioMapLocation Location = ScenarioMapFindPackage(Row.MapName);

		Located.Add(Location);

		/* --- ONE: the map is on disk ----------------------------------------------------- */

		TestTrue(
			*FString::Printf(
				TEXT("%s names the map '%s', and a MAP THAT DOES NOT EXIST IS A SCENARIO NOBODY ")
				TEXT("CAN JOIN — the map branch of selection can never fire for it, so the only ")
				TEXT("door into it is ?Scenario= on a URL. Looked for: %s"),
				*Label, Row.MapName, *Location.Tried),
			Location.bFound);

		if (!Location.bFound)
		{
			++Missing;
			continue;
		}

		AddInfo(FString::Printf(
			TEXT("%s -> %s ('%s')"), *Label, *Location.PackageName, *Location.Filename));

		/* --- TWO: and that map selects this row back, in every form its name arrives in --- */

		// Built from the found package, not Row.MapName, so the folder and name are the ones on disk.
		FString FoundFolder;
		FString FoundAssetName;

		if (!Location.PackageName.Split(
				TEXT("/"), &FoundFolder, &FoundAssetName, ESearchCase::CaseSensitive,
				ESearchDir::FromEnd))
		{
			AddError(FString::Printf(
				TEXT("%s: '%s' is not a long package name"), *Label, *Location.PackageName));

			continue;
		}

		FoundFolder += TEXT("/");

		TArray<FString> Forms;

		// The bare asset name, as a cooked game's UWorld::GetMapName returns it.
		Forms.Add(FoundAssetName);
		Forms.Add(FoundAssetName.ToUpper());
		Forms.Add(FoundAssetName.ToLower());

		// The long package path and the full object path (URL or DefaultEngine.ini).
		Forms.Add(Location.PackageName);
		Forms.Add(FString::Printf(TEXT("%s.%s"), *Location.PackageName, *FoundAssetName));

		// PIE-prefixed, with non-zero instance numbers too, bare and inside the path.
		for (const TCHAR* const PiePrefix : ScenarioMapPiePrefixes)
		{
			Forms.Add(FString(PiePrefix) + FoundAssetName);
			Forms.Add(FoundFolder + PiePrefix + FoundAssetName);
		}

		for (const FString& Form : Forms)
		{
			EScenarioSelection How = EScenarioSelection::ByOption;

			const int32 Selected = IndexForOptionsAndMap(FString(), Form, How);

			TestTrue(
				*FString::Printf(
					TEXT("%s: opening its own map as '%s' must select it — row %d — and report ")
					TEXT("ByMapName; it selected %s (row %d), %s. A map that opens onto somebody ")
					TEXT("else's wall is worse than one that does not open."),
					*Label, *Form, Index, *ScenarioMapRowName(Selected), Selected,
					*ScenarioMapHowName(How)),
				Selected == Index && How == EScenarioSelection::ByMapName);
		}

		// The direct lookup too.
		TestTrue(
			*FString::Printf(
				TEXT("%s: IndexOfMapName('%s') must answer row %d; it answered %d"),
				*Label, *FoundAssetName, Index, IndexOfMapName(FoundAssetName)),
			IndexOfMapName(FoundAssetName) == Index);
	}

	/* --- THREE: and no two rows claim the same map -------------------------------------- */

	// Compares found packages, not name strings, so two spellings of one file are caught.
	for (int32 Index = 0; Index < Located.Num(); ++Index)
	{
		if (!Located[Index].bFound)
		{
			continue;
		}

		for (int32 Earlier = 0; Earlier < Index; ++Earlier)
		{
			if (!Located[Earlier].bFound)
			{
				continue;
			}

			TestTrue(
				*FString::Printf(
					TEXT("row %d ('%s') and row %d ('%s') both resolve to '%s' — one map cannot ")
					TEXT("select two scenarios, so one of the two is unreachable"),
					Index, *ScenarioMapRowName(Index), Earlier, *ScenarioMapRowName(Earlier),
					*Located[Index].PackageName),
				!Located[Index].PackageName.Equals(
					Located[Earlier].PackageName, ESearchCase::IgnoreCase));
		}
	}

	AddInfo(FString::Printf(
		TEXT("%d of %d catalogue rows have a .umap on disk; %d do not"),
		Rows.Num() - Missing, Rows.Num(), Missing));

	return true;
}

/**
 * Each scenario map is a distinct asset. Maps made by copying Lvl_Sandbox.umap kept the inner
 * UWorld named Lvl_Sandbox, so all claimed the same PrimaryAssetId and the editor refused to open
 * them; ScenarioMapsExist and -game loading both missed it. Asserts each package's world is named
 * after the package, and that no two rows share a PrimaryAssetId (read as the saved tag, since a
 * stale tag could collide even with a correct name). Uses the asset registry, rescanned first so a
 * stale cache cannot answer; nothing is loaded.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FScenarioMapsAreDistinctAssetsTest,
	"DestructionGame.Content.ScenarioMapsAreDistinctAssets",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FScenarioMapsAreDistinctAssetsTest::RunTest(const FString& Parameters)
{
	using namespace ScenarioMapTestSupport;
	using namespace DestructionScenarios;

	IAssetRegistry* const Registry = IAssetRegistry::Get();

	if (Registry == nullptr)
	{
		AddError(TEXT(
			"fixture: there is no asset registry, so nothing here can be asked about content at "
			"all. This test cannot report on the maps either way."));

		return false;
	}

	// Forced rescan: the cached registry can be stale when content is regenerated outside the editor.
	Registry->ScanPathsSynchronous({FString(ScenarioMapScanRoot)}, /*bForceRescan*/ true);
	Registry->WaitForCompletion();

	/* --- ZERO: the registry actually has the maps, or nothing below means anything ---------- */

	TArray<FAssetData> UnderMaps;
	Registry->GetAssetsByPath(
		FName(ScenarioMapScanRoot), UnderMaps, /*bRecursive*/ true, /*bIncludeOnlyOnDiskAssets*/ true);

	const FTopLevelAssetPath WorldClassPath = UWorld::StaticClass()->GetClassPathName();

	int32 WorldsFound = 0;

	for (const FAssetData& Asset : UnderMaps)
	{
		if (Asset.AssetClassPath == WorldClassPath)
		{
			++WorldsFound;
		}
	}

	TestTrue(
		*FString::Printf(
			TEXT("fixture: the registry must report at least %d UWorld assets under '%s' for this ")
			TEXT("test to be looking at anything; it reports %d out of %d assets of every kind. An ")
			TEXT("unscanned registry answers every query below with nothing, and 'no world here' ")
			TEXT("never contradicts 'the world here is named X'."),
			ScenarioMapWorldAssetFloor, ScenarioMapScanRoot, WorldsFound, UnderMaps.Num()),
		WorldsFound >= ScenarioMapWorldAssetFloor);

	const TArray<FScenario>& Rows = Catalogue();

	TArray<FScenarioMapAsset> Found;
	Found.Reserve(Rows.Num());

	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		const FScenario& Row = Rows[Index];

		const FString Label = FString::Printf(TEXT("row %d ('%s')"), Index, *Row.Name.ToString());

		FScenarioMapAsset Asset;

		if (Row.MapName == nullptr || FCString::Strlen(Row.MapName) == 0)
		{
			AddError(FString::Printf(
				TEXT("%s names no map at all, so there is no package to look inside"), *Label));

			Found.Add(Asset);
			continue;
		}

		const FScenarioMapLocation Location = ScenarioMapFindPackage(Row.MapName);

		if (!Location.bFound)
		{
			AddError(FString::Printf(
				TEXT("%s names the map '%s' and no such package exists, so there is nothing to ")
				TEXT("inspect. Content.ScenarioMapsExist owns that claim; looked in: %s"),
				*Label, Row.MapName, *Location.Tried));

			Found.Add(Asset);
			continue;
		}

		Asset.PackageName = Location.PackageName;
		Asset.ExpectedWorldName = FPackageName::GetShortName(Location.PackageName);

		TArray<FAssetData> Worlds;
		ScenarioMapWorldsInPackage(*Registry, Location.PackageName, Worlds);

		/* --- ONE: the package holds exactly one world ------------------------------------- */

		if (Worlds.Num() != 1)
		{
			AddError(FString::Printf(
				TEXT("%s: '%s' holds %d UWorld assets, not one. A map package with no world in it ")
				TEXT("is not a level, and one with two has no single asset to open."),
				*Label, *Location.PackageName, Worlds.Num()));

			Found.Add(Asset);
			continue;
		}

		Asset.bResolved = true;
		Asset.ActualWorldName = Worlds[0].AssetName.ToString();
		Asset.PrimaryAssetId = Worlds[0].GetPrimaryAssetId();

		Found.Add(Asset);

		AddInfo(FString::Printf(
			TEXT("%s -> package '%s', world '%s', %s"),
			*Label, *Asset.PackageName, *Asset.ActualWorldName,
			*Asset.PrimaryAssetId.ToString()));

		/* --- TWO: and that world is named after its own package --------------------------- */

		TestTrue(
			*FString::Printf(
				TEXT("%s: the package '%s' must contain a world called '%s', and it contains one ")
				TEXT("called '%s'. A map made by COPYING THE FILE keeps the original's world name, ")
				TEXT("and the world's name is what its PrimaryAssetId is built from — so a copy is ")
				TEXT("a level the editor refuses to open. Duplicate the asset instead of the file."),
				*Label, *Asset.PackageName, *Asset.ExpectedWorldName, *Asset.ActualWorldName),
			Asset.ActualWorldName.Equals(Asset.ExpectedWorldName, ESearchCase::CaseSensitive));

		/* --- THREE: and it has an id at all ----------------------------------------------- */

		TestTrue(
			*FString::Printf(
				TEXT("%s: '%s' reports no PrimaryAssetId at all. An id read as nothing compares ")
				TEXT("equal to every other nothing, so the uniqueness check below cannot speak for ")
				TEXT("this row either way."),
				*Label, *Asset.PackageName),
			Asset.PrimaryAssetId.IsValid());
	}

	/* --- FOUR: and no two rows claim the same PrimaryAssetId ----------------------------- */

	// One failure per colliding row, naming the first claimant, rather than one per pair.
	TMap<FString, int32> ClaimedBy;

	for (int32 Index = 0; Index < Found.Num(); ++Index)
	{
		const FScenarioMapAsset& Asset = Found[Index];

		if (!Asset.bResolved || !Asset.PrimaryAssetId.IsValid())
		{
			continue;
		}

		const FString Id = Asset.PrimaryAssetId.ToString();

		if (const int32* const Earlier = ClaimedBy.Find(Id))
		{
			TestTrue(
				*FString::Printf(
					TEXT("row %d ('%s'), package '%s', claims the PrimaryAssetId '%s' — which row ")
					TEXT("%d ('%s'), package '%s', already claims. Two different primary assets ")
					TEXT("cannot have the same type and name: the asset manager drops one of them ")
					TEXT("and neither map opens reliably."),
					Index, *ScenarioMapRowName(Index), *Asset.PackageName, *Id,
					*Earlier, *ScenarioMapRowName(*Earlier), *Found[*Earlier].PackageName),
				false);

			continue;
		}

		ClaimedBy.Add(Id, Index);
	}

	AddInfo(FString::Printf(
		TEXT("%d of %d catalogue rows resolved to a single world asset; %d distinct PrimaryAssetIds"),
		Found.FilterByPredicate([](const FScenarioMapAsset& A) { return A.bResolved; }).Num(),
		Rows.Num(), ClaimedBy.Num()));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
