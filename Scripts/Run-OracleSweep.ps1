<#
.SYNOPSIS
    Runs the opt-in oracle sweep, splitting the expensive tier across parallel processes.

.DESCRIPTION
    The sweep is dominated by three tests (measured 2026-08-16: WallsAndLadders 455 s,
    PhaseTwoMustNotRefuseTheCoveredOpeningFamily 432 s, FeasibilityReformulationCost 313 s)
    against seven cheap ones totalling 80 s. Run serially that is ~22 minutes.

    MEASURED, 2026-08-16: -Tier All takes 648 s -- 2.0x, not the 2.9x the bucket split
    predicts. All three processes finished within a tenth of a second of each other, which
    is the signature of contention rather than of balance: three editor instances want more
    than the two physical cores each that six cores allow. Adding a fourth bucket would not
    help, and rebalancing the buckets would not either -- the limit is the machine. Treat
    2x as the ceiling here.

    WHAT VERIFIES THE SPLIT, and it is cheaper than it looks: every lambda* and pivot count
    that matters is PINNED INSIDE the tests, so a parallel run that passes all ten has
    already proved it changed no answer. -Verify exists for the diagnostic lines that carry
    no assertion, and for the day a bucket layout changes -- not for routine use.

    WHY THIS DOES NOT BREAK THE DETERMINISM CONTRACT. The contract is about floating-point
    SUMMATION ORDER WITHIN a solve -- parallel sums are order-dependent, so the solver is
    deliberately single-threaded and never reorders. Running two TESTS in two OS processes
    changes neither the order of any sum nor the pivot path of any solve: each process
    builds its own fixtures and the solver holds no state between calls (verified
    2026-08-16 -- no static storage anywhere in RigidBlockOracle or either sweep file).
    Nothing here threads a solve. If a future test ever depends on sharing a process with
    another, that test is the thing that is wrong, and -Verify is what catches it.

    ASCII ONLY, DELIBERATELY. PowerShell 5.1 reads a BOM-less UTF-8 .ps1 as the ANSI
    codepage, so a single em-dash in a comment turns into mojibake and breaks the PARSE --
    the whole script fails to run, with errors pointing at unrelated lines. Keep this file
    to plain ASCII. See TRAPS.

.PARAMETER Tier
    Fast  -- the seven cheap tests only (~80 s, one process). Use while iterating.
    Full  -- the three expensive tests, one process each.
    All   -- both tiers. THE ONE TO RUN BEFORE A COMMIT.

.PARAMETER Serial
    Run everything in a single process. Slower, and the reference a parallel run is
    checked against.

.PARAMETER Verify
    Run serially AND in parallel, then compare every reported lambda*, pivot count and
    verdict line. Proves the split changed no answer. Slow by design; run it when the
    bucket layout changes, not routinely.

.EXAMPLE
    powershell -NoProfile -File Scripts/Run-OracleSweep.ps1 -Tier Fast
    powershell -NoProfile -File Scripts/Run-OracleSweep.ps1 -Tier All
    powershell -NoProfile -File Scripts/Run-OracleSweep.ps1 -Verify
#>

[CmdletBinding()]
param(
    [ValidateSet('Fast', 'Full', 'All')]
    [string] $Tier = 'All',

    [switch] $Serial,

    [switch] $Verify
)

$ErrorActionPreference = 'Stop'

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$UProject    = Join-Path $ProjectRoot 'DestructionGame.uproject'
$EditorCmd   = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
$LogDir      = Join-Path $ProjectRoot 'Saved\Logs\SweepRuns'

if (-not (Test-Path $UProject))  { throw "Project not found: $UProject" }
if (-not (Test-Path $EditorCmd)) { throw "UnrealEditor-Cmd not found: $EditorCmd" }

<#
    The buckets. Each entry runs in its own process, so wall time is the slowest bucket
    rather than the sum. The expensive three get a bucket each; the fast tier rides with
    the shortest of them (313 + 80 = 393 s, still under WallsAndLadders' 455 s).

    Seconds are MEASURED, 2026-08-16, and are here to keep the buckets balanced -- they are
    not assertions. Re-measure when the fixtures change; a bucket that has drifted well
    past the others is just wasted wall time.
#>
$FastFilter = 'OracleSweepFast'

$FullBuckets = @(
    @{ Name = 'walls';   Filter = 'OracleSweepFull.RigidBlock.WallsAndLadders';                              Seconds = 455 },
    @{ Name = 'covered'; Filter = 'OracleSweepFull.RigidBlock.PhaseTwoMustNotRefuseTheCoveredOpeningFamily'; Seconds = 432 },
    @{ Name = 'spike';   Filter = 'OracleSweepFull.RigidBlock.FeasibilityReformulationCost';                 Seconds = 313 }
)

function Get-Buckets {
    param([string] $ForTier)

    if ($ForTier -eq 'Fast') {
        return @( @{ Name = 'fast'; Filters = @($FastFilter); Seconds = 80 } )
    }

    $Buckets = @()
    foreach ($B in $FullBuckets) {
        $Buckets += @{ Name = $B.Name; Filters = @($B.Filter); Seconds = $B.Seconds }
    }

    if ($ForTier -eq 'All') {
        $Lightest = $Buckets | Sort-Object { $_.Seconds } | Select-Object -First 1
        $Lightest.Filters = $Lightest.Filters + $FastFilter
        $Lightest.Seconds = $Lightest.Seconds + 80
    }

    return $Buckets
}

function Invoke-Bucket {
    <#
        Starts one UnrealEditor-Cmd on one bucket and returns the process plus its log path.
        Each process gets its own -abslog: the default log is a single shared file and
        concurrent writers would interleave into nonsense.

        Note the filter join. A '+' between two DestructionGame.* paths produces FABRICATED
        results (TRAPS), but these are distinct opt-in suites, which is the one case the
        engine documents it for -- and -Verify is what proves it on this project's fixtures.
    #>
    param(
        [hashtable] $Bucket,
        [string]    $RunTag
    )

    $LogPath = Join-Path $LogDir ("{0}-{1}.log" -f $RunTag, $Bucket.Name)
    if (Test-Path $LogPath) { Remove-Item -LiteralPath $LogPath -Force }

    $Filter = ($Bucket.Filters -join '+')

    $Args = @(
        ('"{0}"' -f $UProject)
        ('-ExecCmds="Automation RunTests {0}"' -f $Filter)
        '-TestExit="Automation Test Queue Empty"'
        '-unattended', '-nopause', '-nosplash', '-nullrhi', '-NoSound', '-log'
        ('-abslog="{0}"' -f $LogPath)
    )

    $Process = Start-Process -FilePath $EditorCmd -ArgumentList $Args -PassThru -WindowStyle Hidden

    return [pscustomobject]@{
        Name    = $Bucket.Name
        Filter  = $Filter
        Log     = $LogPath
        Process = $Process
        Started = Get-Date
    }
}

function Read-Results {
    <#
        Parses one run log. Exit codes are meaningless for automation (TRAPS), so the
        COMPLETED LINES ARE THE ONLY TRUTH -- including their count, which is the only
        guard that the filter ran what was meant.
    #>
    param([string] $LogPath)

    if (-not (Test-Path $LogPath)) {
        return [pscustomobject]@{ Completed = @(); Errors = @(); Readings = @(); Missing = $true }
    }

    $Lines = Get-Content -LiteralPath $LogPath

    $Completed = @()
    foreach ($M in ($Lines | Select-String -Pattern 'Test Completed\. Result=\{(\w+)\} Name=\{([^}]*)\}')) {
        $Completed += [pscustomobject]@{
            Result = $M.Matches[0].Groups[1].Value
            Name   = $M.Matches[0].Groups[2].Value
        }
    }

    $Errors = @()
    foreach ($M in ($Lines | Select-String -Pattern 'LogAutomationController: Error')) {
        $Errors += $M.Line
    }

    <#
        Every reported lambda / pivot line, stripped of timestamps and of wall-clock
        seconds so two runs can be compared for bit-identity. Timings legitimately differ
        run to run; the numbers that must not are lambda*, pivot counts and verdicts.
    #>
    $Readings = @()
    foreach ($M in ($Lines | Select-String -Pattern '(lambda\*?=|pivots=)')) {
        $Line = $M.Line -replace '^\[[^\]]*\]\[[^\]]*\]', ''
        $Line = $Line -replace 'secs=[0-9.]+', 'secs=X'
        $Readings += $Line.Trim()
    }

    return [pscustomobject]@{
        Completed = $Completed
        Errors    = $Errors
        Readings  = ($Readings | Sort-Object)
        Missing   = $false
    }
}

function Invoke-Sweep {
    param(
        [string] $ForTier,
        [switch] $RunSerially,
        [string] $RunTag
    )

    <#
        The @() is load-bearing. PowerShell unrolls a single-element array to the element
        itself, so a one-bucket tier would arrive here as a bare hashtable and .Count would
        report its KEY count (3) instead of 1 -- a wrong number in the one place a reader
        checks that the split did what they asked.
    #>
    $Buckets = @(Get-Buckets -ForTier $ForTier)

    if ($RunSerially) {
        $Filters = @()
        $Total   = 0
        foreach ($B in $Buckets) { $Filters += $B.Filters; $Total += $B.Seconds }
        $Buckets = @( @{ Name = 'serial'; Filters = $Filters; Seconds = $Total } )
    }

    New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

    Write-Host ""
    Write-Host ("Oracle sweep - tier {0}, {1} process(es)" -f $ForTier, $Buckets.Count)
    foreach ($B in $Buckets) {
        Write-Host ("  {0,-9} ~{1,4}s  {2}" -f $B.Name, $B.Seconds, ($B.Filters -join ' + '))
    }
    Write-Host ""

    $Start = Get-Date
    $Runs  = @()
    foreach ($B in $Buckets) { $Runs += Invoke-Bucket -Bucket $B -RunTag $RunTag }

    foreach ($R in $Runs) {
        $R.Process.WaitForExit()
        $Elapsed = (Get-Date) - $R.Started
        Write-Host ("  finished {0,-9} in {1,6:N1}s" -f $R.Name, $Elapsed.TotalSeconds)
    }

    $Wall = (Get-Date) - $Start

    $Completed  = @()
    $Errors     = @()
    $Readings   = @()
    $AnyMissing = $false

    foreach ($R in $Runs) {
        $Parsed     = Read-Results -LogPath $R.Log
        $Completed += $Parsed.Completed
        $Errors    += $Parsed.Errors
        $Readings  += $Parsed.Readings
        if ($Parsed.Missing) { $AnyMissing = $true }
    }

    return [pscustomobject]@{
        Completed = $Completed
        Errors    = $Errors
        Readings  = ($Readings | Sort-Object)
        Wall      = $Wall
        Missing   = $AnyMissing
        Logs      = @($Runs | ForEach-Object { $_.Log })
    }
}

function Write-Summary {
    param($Result, [string] $Label)

    $Passed = @($Result.Completed | Where-Object { $_.Result -eq 'Success' })
    $Failed = @($Result.Completed | Where-Object { $_.Result -ne 'Success' })

    Write-Host ""
    Write-Host ("{0}: {1} completed, {2} passed, {3} failed, {4:N1}s wall" -f `
        $Label, $Result.Completed.Count, $Passed.Count, $Failed.Count, $Result.Wall.TotalSeconds)

    foreach ($F in $Failed) { Write-Host ("  FAIL  {0}" -f $F.Name) }

    if ($Result.Missing) {
        Write-Host "  WARNING: a bucket produced no log - its process died before writing one"
    }

    <#
        A zero-completed run is the failure mode that looks like success: the filter matched
        nothing, the process exited 0, and nobody noticed. Never let it pass silently.
    #>
    if ($Result.Completed.Count -eq 0) {
        Write-Host "  ERROR: no tests ran. The filter matched nothing - check the tier names."
        return $false
    }

    return ($Failed.Count -eq 0)
}

if ($Verify) {
    Write-Host "VERIFY: running serially, then in parallel, then comparing every reading."

    $SerialResult   = Invoke-Sweep -ForTier $Tier -RunSerially -RunTag 'verify-serial'
    $SerialOk       = Write-Summary -Result $SerialResult -Label 'serial'

    $ParallelResult = Invoke-Sweep -ForTier $Tier -RunTag 'verify-parallel'
    $ParallelOk     = Write-Summary -Result $ParallelResult -Label 'parallel'

    $Diff = Compare-Object -ReferenceObject $SerialResult.Readings -DifferenceObject $ParallelResult.Readings

    Write-Host ""
    if ($null -eq $Diff) {
        Write-Host ("READINGS IDENTICAL across {0} lines - the split changed no answer." -f $SerialResult.Readings.Count)
    }
    else {
        Write-Host ("READINGS DIFFER on {0} line(s) - the split is NOT sound, investigate:" -f $Diff.Count)
        $Diff | Select-Object -First 20 | ForEach-Object { Write-Host ("  {0} {1}" -f $_.SideIndicator, $_.InputObject) }
    }

    if (-not ($SerialOk -and $ParallelOk -and ($null -eq $Diff))) { exit 1 }
    exit 0
}

$Result = Invoke-Sweep -ForTier $Tier -RunSerially:$Serial -RunTag ('run-' + $Tier.ToLower())
$Ok     = Write-Summary -Result $Result -Label ("tier {0}" -f $Tier)

Write-Host ""
Write-Host "Logs:"
foreach ($L in $Result.Logs) { Write-Host ("  {0}" -f $L) }

if ($Result.Errors.Count -gt 0) {
    Write-Host ""
    Write-Host ("First {0} error line(s):" -f [Math]::Min(10, $Result.Errors.Count))
    $Result.Errors | Select-Object -First 10 | ForEach-Object { Write-Host ("  {0}" -f $_) }
}

if (-not $Ok) { exit 1 }
exit 0
