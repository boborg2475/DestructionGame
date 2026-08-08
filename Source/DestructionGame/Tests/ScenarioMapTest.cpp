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
 * EVERY SCENARIO IN THE CATALOGUE IS A MAP A HUMAN CAN ACTUALLY JOIN, AND THAT MAP SELECTS IT.
 *
 * =====================================================================================
 * THE GAP THIS CLOSES, AND WHY NOTHING ELSE COULD HAVE CAUGHT IT
 * =====================================================================================
 *
 * Every row of the catalogue carries a `MapName` — `Lvl_CorbelE35`, `Lvl_CorbelF100` — and
 * `World.Scenarios.Catalogue` already proves `IndexOfMapName` finds each row from that string.
 * What no test anywhere checks is whether the string names a `.umap` THAT EXISTS. Today only two
 * do, so the map branch of selection is DEAD for the other seven: they can never be reached by
 * opening a level, and `?Scenario=` on a URL is the only door into them. A player handed a list
 * of scenarios finds most of the doors are painted on.
 *
 * `Content.RequiredAssetsResolve` does not cover it — it sweeps the paths C++ resolves through
 * `ConstructorHelpers` and the references sitting on CDOs, and a catalogue row's map name is
 * neither. So this is a fourth kind of content claim rather than a row added to that table.
 *
 * =====================================================================================
 * THE PATH IS DERIVED FROM THE ROW, NOT LISTED
 * =====================================================================================
 *
 * A hardcoded table of nine paths would have to be edited alongside every scenario added, and
 * the edit is exactly what gets forgotten — which is the failure this test exists for, one layer
 * up. So the candidates are the row's own `MapName` under each folder a map of this project may
 * live in: `/Game/Maps/Scenarios/` for the scenario levels, and `/Game/Maps/` for `Lvl_Sandbox`,
 * which is the gameplay map the game already ships and stays where it is. Adding a scenario
 * without adding its map therefore fails HERE rather than at the player.
 *
 * RESOLVED AS A PACKAGE ON DISK, NOT BY LOADING A WORLD. `FPackageName::DoesPackageExist` asks
 * the mounted content tree whether the file is there and loads nothing, so this runs at
 * arithmetic speed under `-nullrhi` alongside the solver tests. Loading nine worlds — one of
 * them the hundred-step corbel — would be the most expensive thing in the suite, and it would be
 * testing the engine's map loader rather than this project's catalogue.
 *
 * =====================================================================================
 * AND THE MAP MUST SELECT THE ROW BACK, IN EVERY FORM ITS NAME ARRIVES IN
 * =====================================================================================
 *
 * A map that exists but selects the wrong scenario is worse than a map that is missing: the
 * missing one fails to open, and the wrong one opens and shows the player somebody else's wall.
 * So the round trip is asserted from the package path THAT WAS ACTUALLY FOUND rather than from
 * the name the row spells — a map placed in the wrong folder would otherwise pass the lookup
 * assertions on a string nothing on disk corresponds to.
 *
 * The decorations are `Tests/ScenarioSelectionTest.cpp`'s, and deliberately the same list: bare
 * asset name, the `UEDPIE_<n>_` stamp PIE puts on it, a long package path, a PIE-prefixed
 * package path, and the full object path `DefaultEngine.ini` uses. That file pins them against
 * one hand-written map name; this one pins them against every map that exists on disk, which is
 * the direction that keeps working as rows are added.
 *
 * NEEDS A TICKING WORLD: no, and deliberately not — nor a world at all. A package query and a
 * pair of string lookups.
 */
namespace ScenarioMapTestSupport
{
	using namespace DestructionScenarios;

	/**
	 * WHERE A SCENARIO'S MAP MAY LIVE, and why there are exactly two.
	 *
	 * `/Game/Maps/Scenarios/` is where the scenario levels go — one `.umap` per catalogue row,
	 * each a byte copy of the sandbox map with the game mode doing all the work, so no map is
	 * ever authored or edited. `/Game/Maps/` is where `Lvl_Sandbox` already is: it is the
	 * gameplay map the game ships and moving it would change what pressing Play opens.
	 *
	 * `Content/Maps/FunctionalTests/` is deliberately NOT here. CLAUDE.md reserves it for
	 * functional-test maps and excludes it from the cook, so a scenario found there would be a
	 * level that works in the editor and is absent from a build.
	 */
	const TCHAR* const ScenarioMapFolders[] =
	{
		TEXT("/Game/Maps/Scenarios/"),
		TEXT("/Game/Maps/"),
	};

	/** PIE stamps `UEDPIE_<instance>_` onto the ASSET name, inside the path. */
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

		/** The file behind it, so a failure can be taken straight to Explorer. */
		FString Filename;

		/** Every candidate, in the order they were tried — the whole of the failure message. */
		FString Tried;
	};

	/**
	 * The `.umap` for a row, looked for under each folder a map of this project may live in.
	 *
	 * THE FIRST HIT WINS AND EVERY CANDIDATE IS RECORDED. A row whose map is missing has to say
	 * exactly which paths were checked, or "the map does not exist" is a statement nobody can act
	 * on without reading this file.
	 */
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

	/**
	 * THE ROOT OF THE CONTENT TREE THE SCENARIO MAPS LIVE UNDER, scanned as one directory.
	 *
	 * `/Game/Maps` covers `Lvl_Sandbox` at its top and every scenario in `Scenarios/` beneath it,
	 * and `ScanPathsSynchronous` is recursive — so one path rather than the two folders above,
	 * which are about where a row's `.umap` may be FOUND rather than about what to scan.
	 */
	const TCHAR* const ScenarioMapScanRoot = TEXT("/Game/Maps");

	/**
	 * A FLOOR ON WHAT THE REGISTRY CAME BACK WITH, and the single most important line here.
	 *
	 * An asset registry that has not scanned `/Game/Maps` answers every query with an empty array,
	 * and every per-row assertion below is written as "the world in this package must be named
	 * X" — which a package with NO worlds in it never contradicts. A test that read an unscanned
	 * registry would therefore go green by finding nothing at all, which is the worst possible
	 * outcome for a test whose whole job is to notice that content is wrong.
	 *
	 * TWENTY-NINE: the twenty-eight scenario maps plus `Lvl_Sandbox`, which is the same count the
	 * catalogue floor above asserts and for the same reason.
	 */
	constexpr int32 ScenarioMapWorldAssetFloor = 29;

	/** What the registry knows about one row's map, without any of it having been loaded. */
	struct FScenarioMapAsset
	{
		bool bResolved = false;

		/** The long package name that exists, e.g. `/Game/Maps/Scenarios/Lvl_Wall01`. */
		FString PackageName;

		/** The package's own short name — what the UWorld inside it must be called. */
		FString ExpectedWorldName;

		/** What the UWorld inside it is ACTUALLY called. */
		FString ActualWorldName;

		FPrimaryAssetId PrimaryAssetId;
	};

	/** Every UWorld the registry reports in one package, filtered by class rather than by count. */
	inline void ScenarioMapWorldsInPackage(
		const IAssetRegistry& Registry, const FString& PackageName, TArray<FAssetData>& Out)
	{
		Out.Reset();

		TArray<FAssetData> InPackage;

		/*
		 * ON DISK RATHER THAN FROM MEMORY. The claim is about the FILE — a package that was copied
		 * rather than duplicated carries the original's object name inside it — so an in-memory
		 * UWorld that happened to be loaded under a different name would answer the wrong question.
		 */
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
	 * A FLOOR ON THE SWEEP, so a catalogue that emptied — or a lookup that started answering
	 * nothing — fails here rather than turning this into a loop over no rows that passes in
	 * silence. A floor rather than an equality because adding a scenario is meant to be adding a
	 * row.
	 *
	 * TWENTY-NINE: the nine slices A to C left, plus THE TWENTY ACCEPTANCE WALLS the user drew and
	 * reviewed. Those twenty are the whole reason the acceptance set exists, and a wall that is
	 * only a fixture is a wall nobody can stand in front of — so the floor is raised WITH them
	 * rather than after them, and a catalogue that never grew the rows fails here rather than
	 * sweeping the nine it already had and passing.
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

		/*
		 * BUILT FROM THE PACKAGE THAT WAS FOUND, NOT FROM THE NAME THE ROW SPELLS. A map dropped
		 * into the wrong folder still carries the right asset name, so a round trip written
		 * against `Row.MapName` would pass against a level nothing can open. Split back out of
		 * the found package so the asset name and the folder are the ones on disk.
		 */
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

		/* The bare asset name, which is what a cooked game's UWorld::GetMapName answers. */
		Forms.Add(FoundAssetName);
		Forms.Add(FoundAssetName.ToUpper());
		Forms.Add(FoundAssetName.ToLower());

		/* The long package path and the full object path a URL or DefaultEngine.ini hands over. */
		Forms.Add(Location.PackageName);
		Forms.Add(FString::Printf(TEXT("%s.%s"), *Location.PackageName, *FoundAssetName));

		/* And PIE, whose instance number is not always zero, bare and inside the path. */
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

		/* The direct lookup too, which is the promise IndexOfMapName itself makes. */
		TestTrue(
			*FString::Printf(
				TEXT("%s: IndexOfMapName('%s') must answer row %d; it answered %d"),
				*Label, *FoundAssetName, Index, IndexOfMapName(FoundAssetName)),
			IndexOfMapName(FoundAssetName) == Index);
	}

	/* --- THREE: and no two rows claim the same map -------------------------------------- */

	/*
	 * ON THE PACKAGE THAT WAS FOUND rather than on the two name strings, which is the stronger
	 * claim of the two: two rows spelling different names that resolve to one file on disk would
	 * pass a comparison of the spellings and still make one of the two unreachable.
	 */
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
 * AND EACH OF THOSE MAPS IS ITS OWN ASSET — A DISTINCT WORLD WITH A DISTINCT PrimaryAssetId.
 *
 * =====================================================================================
 * THE DEFECT, AND WHY THE TEST ABOVE COULD NOT SEE IT
 * =====================================================================================
 *
 * Every scenario map was made by BYTE-COPYING `Lvl_Sandbox.umap` to a new filename. A `.umap` is
 * a package, and the thing a level actually IS lives inside it as a `UWorld` object with a name
 * of its own — a name that a file copy does not touch. So twenty-eight files called
 * `Lvl_Wall01`, `Lvl_CorbelE35` and so on each contained a world still called `Lvl_Sandbox`.
 *
 * `UWorld::GetPrimaryAssetId` answers `Map:<package name>`, and that string is BAKED INTO THE
 * PACKAGE AS A TAG WHEN IT IS SAVED. A copy therefore carries the ORIGINAL's tag, so all
 * twenty-nine maps claimed `Map:/Game/Maps/Lvl_Sandbox`. The asset manager refuses the
 * collision — "Two different primary assets can not have the same type and name" — and the maps
 * cannot be opened in the editor.
 *
 * `Content.ScenarioMapsExist` asserts a `.umap` is on disk for every row and that the map's name
 * selects that row back. BOTH ARE TRUE OF A BYTE COPY. It asks about the FILE, and the whole of
 * this defect lives in the difference between the file and the asset inside it. `-game` loading
 * missed it for a related reason: a cooked-style load never consults the asset manager at all.
 *
 * =====================================================================================
 * TWO CLAIMS, AND THE SECOND IS NOT IMPLIED BY THE FIRST
 * =====================================================================================
 *
 * THE WORLD IS NAMED AFTER ITS OWN PACKAGE. `/Game/Maps/Scenarios/Lvl_Wall01` must contain
 * `Lvl_Wall01`, not `Lvl_Sandbox`. This is the direct statement of the defect, and it is the
 * property the fix — duplicating through the editor rather than copying the file — restores.
 *
 * AND NO TWO ROWS CLAIM THE SAME PrimaryAssetId. The failure the user hit is a COLLISION, so
 * the collision is what gets asserted rather than only the naming rule that happens to imply it
 * today. The id is read as a saved tag, not recomputed from the name: a package saved under one
 * name and renamed afterwards can carry a stale tag while its world reads correctly, and that
 * would pass the first claim and still collide.
 *
 * READ THROUGH THE ASSET REGISTRY, WHICH LOADS NOTHING. `FAssetData` carries the package name,
 * the object name and the saved tags straight from each package's header, so twenty-nine worlds
 * are inspected without a single one being loaded — and it runs headless under `-nullrhi`
 * alongside the solver tests. The paths are RESCANNED first: a cached registry from before the
 * content was touched would otherwise be answering about a different set of files.
 *
 * NEEDS A TICKING WORLD: no, and deliberately not — nor a loaded world. A rescan and a set of
 * header reads.
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

	/*
	 * FORCED, so the answer is about the files as they are NOW. The registry is normally served
	 * from `Intermediate/CachedAssetRegistry`, which is exactly as old as the last time anything
	 * scanned — and content regenerated outside the editor is precisely the case where the cache
	 * and the disk disagree. A stale cache would let a fixed map read broken, or a broken one read
	 * fixed, and both are worse than a slow test.
	 */
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

	/*
	 * ONE FAILURE PER COLLIDING ROW, NAMING THE ROW IT COLLIDES WITH, rather than a comparison of
	 * every pair. Twenty-eight maps sharing one id is 378 colliding PAIRS, and a wall of those
	 * says nothing a reader can act on; the first row to claim an id keeps it and every later
	 * claimant is one named failure. That is also the shape the fix is checked against — a single
	 * map regenerated wrongly is one row named, not a shower.
	 */
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
