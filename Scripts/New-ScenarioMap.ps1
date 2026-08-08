<#
.SYNOPSIS
    Make a new scenario map that the editor can actually open.

.DESCRIPTION
    A scenario map holds no placed content. It is a duplicate of Content/Maps/Lvl_Sandbox.umap
    under a new name, and DestructionScenarios matches that name to a catalogue row which lays
    the structure, spawns the bricks and frames the player.

    SO WHY IS THIS A SCRIPT RATHER THAN Copy-Item?

    Because Copy-Item is exactly the thing that does not work, and it fails in a way that looks
    like success. A map's PrimaryAssetId is Map:<the inner UWorld object's name> and NOT its
    filename. Copying the file renames the file; the UWorld inside it is still called
    Lvl_Sandbox. Every copy then claims Map:/Game/Maps/Lvl_Sandbox, the asset manager keeps one
    and drops the rest, and the editor refuses to open any of them:

        LogAssetManager: Warning: Found duplicate PrimaryAssetID Map:/Game/Maps/Lvl_Sandbox,
        path /Game/Maps/Lvl_Sandbox.Lvl_Sandbox conflicts with existing path
        /Game/Maps/Scenarios/Lvl_CorbelABare4.Lvl_Sandbox. Two different primary assets can not
        have the same type and name.

    THE -game LOADER NEVER CONSULTS THE ASSET MANAGER, so a byte copy loads and plays perfectly
    headlessly — the only symptom is the world name in the log:

        Bringing World /Game/Maps/Scenarios/Lvl_Wall01.Lvl_Sandbox up for play

    Twenty-eight unopenable maps shipped past a headless load on exactly that reasoning. A
    headless load is not evidence that a map can be opened.

    So this copies the file and then RESAVES it through the editor, which loads the package and
    renames its world to match. That is the same repair the ResavePackagesCommandlet performs,
    and it is the only step that makes the asset distinct.

.PARAMETER MapName
    The map's name, with the Lvl_ prefix and without the extension: Lvl_MyNewLevel. This must
    equal the MapName on the catalogue row in World/DestructionScenarios.cpp.

.EXAMPLE
    powershell -NoProfile -File Scripts/New-ScenarioMap.ps1 -MapName Lvl_Wall21

.NOTES
    Close the editor first — it holds a write lock on the packages, and the resave step will
    report "Error saving" for anything it cannot move.

    Verify with the two tests that exist for this, rather than by opening the editor:
        Automation RunTests DestructionGame.Content.ScenarioMaps
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^Lvl_[A-Za-z0-9]+$')]
    [string] $MapName,

    [string] $ProjectRoot = (Split-Path -Parent $PSScriptRoot),

    [string] $UnrealCmd = "C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
)

$ErrorActionPreference = 'Stop'

$Project     = Join-Path $ProjectRoot 'DestructionGame.uproject'
$SourceMap   = Join-Path $ProjectRoot 'Content\Maps\Lvl_Sandbox.umap'
$ScenarioDir = Join-Path $ProjectRoot 'Content\Maps\Scenarios'
$TargetMap   = Join-Path $ScenarioDir "$MapName.umap"

if (-not (Test-Path $Project))   { throw "no project at '$Project'" }
if (-not (Test-Path $SourceMap)) { throw "no sandbox map to duplicate at '$SourceMap'" }
if (-not (Test-Path $UnrealCmd)) { throw "no UnrealEditor-Cmd at '$UnrealCmd'" }

if (Test-Path $TargetMap)
{
    throw "'$TargetMap' already exists. Delete it first if you mean to replace it."
}

if (-not (Test-Path $ScenarioDir))
{
    New-Item -ItemType Directory -Path $ScenarioDir | Out-Null
}

Copy-Item -Path $SourceMap -Destination $TargetMap
Write-Host "copied  -> $TargetMap"

<#
 # THE RESAVE IS THE WHOLE POINT AND IT RUNS OVER THE FOLDER, NOT THE ONE FILE.
 #
 # ResavePackagesCommandlet loads every package it is given and writes it back, and loading is
 # what renames the world to match its package. Pointing it at the folder is idempotent — a map
 # already correct is resaved to the same content — and it repairs any earlier copy that was
 # made with Copy-Item alone, which is worth more than the seconds it costs.
 #>
$LogPath = Join-Path $ProjectRoot 'Saved\Logs\New-ScenarioMap.log'

& $UnrealCmd $Project -run=ResavePackages -PackageFolder=/Game/Maps/Scenarios `
    -nosplash -NoSound -unattended -nopause -abslog="$LogPath" | Out-Null

Write-Host "resaved -> /Game/Maps/Scenarios (log: $LogPath)"
Write-Host ""
Write-Host "Now add the matching row to World/DestructionScenarios.cpp, then verify with:"
Write-Host '  Automation RunTests DestructionGame.Content.ScenarioMaps'
