<#
.SYNOPSIS
    Generate Content/Layouts/Warehouse.json — the warehouse level's building, as data.

.DESCRIPTION
    The warehouse (claude_plans/WAREHOUSE_DESIGN.md) is not laid by C++. It is a layout file
    (Core/LayoutFile.h: a JSON list of boxes and materials; the joints are swept on load), and
    this script is what writes that file from the design's arithmetic: the coordinating grid, the
    one course-filling rule, and the lists of openings, pilasters, gables, roof boards and
    chimneys. Change a number here, re-run, and the level changes — no build.

    It checks its own output before writing: every box has positive size, and NO TWO BOXES
    OVERLAP (exhaustive, bucketed by course). A spec that produced an overlap would be refused
    rather than written, because an overlapping pair is a building that looks fine and has a
    joint nobody laid.

.PARAMETER OutPath
    Where to write. Defaults to Content/Layouts/Warehouse.json under the project.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File Scripts/New-WarehouseLayout.ps1
#>

[CmdletBinding()]
param(
    [string] $OutPath = ''
)

$ErrorActionPreference = 'Stop'

if (-not $OutPath)
{
    $ScriptDir = if ($PSScriptRoot) { $PSScriptRoot } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
    $OutPath = Join-Path (Split-Path -Parent $ScriptDir) 'Content\Layouts\Warehouse.json'
}

# --- the grid (WAREHOUSE_DESIGN.md) ------------------------------------------------------------
$Pitch = 22.5; $Half = 11.25; $CoursePitch = 7.5; $BrickLen = 21.5; $Wythe = 10.25; $Course = 6.5; $Joint = 1.0; $HalfBat = 10.25
$Nx = 29; $Ny = 14; $Eaves = 64; $GableCourses = 13; $PilasterTop = 61; $ChimneyCourses = 90
$W = $Nx * $Pitch - $Joint                 # 651.5
$Se = $Half; $BackY0 = $Se + $Ny * $Pitch  # 326.25
$D = $BackY0 + $Wythe                      # 336.5
$CorniceProjection = 5.0; $RoofThick = 5.0; $EavesBoardDepth = 21.5; $CapOverhang = 5.625

$Pieces = New-Object System.Collections.Generic.List[object]

function Add($xlo, $xhi, $ylo, $yhi, $zlo, $zhi, $mat, $grounded)
{
    if ($xhi -le $xlo -or $yhi -le $ylo -or $zhi -le $zlo) { throw "degenerate box $xlo $xhi $ylo $yhi $zlo $zhi" }
    $script:Pieces.Add([pscustomobject]@{ xlo = $xlo; xhi = $xhi; ylo = $ylo; yhi = $yhi; zlo = $zlo; zhi = $zhi; mat = $mat; g = $grounded })
}

<#
 # THE COURSE-FILLING RULE. Segment [ga,gb) in half-pitch grid units from run start S, course c,
 # pieces L units long, parity p = c mod 2: cell k covers grid [k*L+p, (k+1)*L+p), as material
 # [S+(k*L+p)*11.25, S+((k+1)*L+p)*11.25-1]; clip to the segment's material; keep a half bat or more.
 #>
function FillSpans($S, $ga, $gb, $c, $L)
{
    $p = $c % 2
    $segLo = $S + $ga * $Half; $segHi = $S + $gb * $Half - $Joint
    $out = @()
    for ($k = -1; ($S + ($k * $L + $p) * $Half) -lt ($segHi - 1e-6); $k++)
    {
        $lo = $S + ($k * $L + $p) * $Half; $hi = $S + (($k + 1) * $L + $p) * $Half - $Joint
        $a = [Math]::Max($lo, $segLo); $b = [Math]::Min($hi, $segHi)
        if (($b - $a) -ge ($HalfBat - 1e-6)) { $out += , @($a, $b) }
    }
    return , $out
}

function Segments($N, $gaps)
{
    $cuts = @(0)
    foreach ($g in ($gaps | Sort-Object { $_[0] })) { $cuts += $g[0]; $cuts += $g[1] }
    $cuts += $N
    $segs = @()
    for ($i = 0; $i -lt $cuts.Count; $i += 2) { if ($cuts[$i + 1] -gt $cuts[$i]) { $segs += , @($cuts[$i], $cuts[$i + 1]) } }
    return , $segs
}

function LayWall($runsX, $thinLo, $thinHi, $S, $N, $openings, $corniceLo, $corniceHi)
{
    for ($c = 0; $c -lt $Eaves; $c++)
    {
        if ($c -eq 1 -or $c -eq 63) { continue }   # the second course of a two-course stone block

        $gaps = @(); $bands = @()
        foreach ($o in $openings)
        {
            if ($c -ge $o.clo -and $c -le $o.chi) { $gaps += , @($o.glo, $o.ghi) }
            if ($o.lintel -and $c -ge ($o.chi + 1) -and $c -le ($o.chi + 2))
            {
                $gaps += , @(($o.glo - 2), ($o.ghi + 2))
                if ($c -eq ($o.chi + 1)) { $bands += [pscustomobject]@{ lo = $o.glo - 2; hi = $o.ghi + 2; mat = 'Timber'; two = $true } }
            }
            if ($o.sill -and $c -eq ($o.clo - 1))
            {
                $gaps += , @(($o.glo - 1), ($o.ghi + 1))
                $bands += [pscustomobject]@{ lo = $o.glo - 1; hi = $o.ghi + 1; mat = 'StructuralConcrete'; two = $false }
            }
        }

        $stone = ($c -eq 0) -or ($c -eq 33) -or ($c -eq 62)
        $twoCourse = ($c -eq 0) -or ($c -eq 62)
        $L = if ($stone) { 4 } else { 2 }
        $mat = if ($stone) { 'StructuralConcrete' } else { 'ClayBrick' }
        $zlo = $c * $CoursePitch
        $zhi = $zlo + $Course + $(if ($twoCourse) { $CoursePitch } else { 0 })
        $tLo = $thinLo; $tHi = $thinHi
        if ($c -eq 62 -and $null -ne $corniceLo) { $tLo = $corniceLo; $tHi = $corniceHi }

        foreach ($seg in (Segments $N $gaps))
        {
            foreach ($sp in (FillSpans $S $seg[0] $seg[1] $c $L))
            {
                if ($runsX) { Add $sp[0] $sp[1] $tLo $tHi $zlo $zhi $mat ($c -eq 0) }
                else        { Add $tLo $tHi $sp[0] $sp[1] $zlo $zhi $mat ($c -eq 0) }
            }
        }

        foreach ($b in $bands)
        {
            $bzhi = $zlo + $Course + $(if ($b.two) { $CoursePitch } else { 0 })
            $lo = $S + $b.lo * $Half; $hi = $S + $b.hi * $Half - $Joint
            if ($runsX) { Add $lo $hi $thinLo $thinHi $zlo $bzhi $b.mat $false }
            else        { Add $thinLo $thinHi $lo $hi $zlo $bzhi $b.mat $false }
        }
    }
}

function Win($glo, $ghi, $clo, $chi) { [pscustomobject]@{ glo = $glo; ghi = $ghi; clo = $clo; chi = $chi; lintel = $true; sill = $true } }

# --- the openings -------------------------------------------------------------------------------
$longOpenings = @()
foreach ($g in 7, 17, 27, 37, 47) { $longOpenings += (Win $g ($g + 4) 8 27); $longOpenings += (Win $g ($g + 4) 38 53) }
$backOpenings = @((Win 6 10 8 27), (Win 18 22 8 27), (Win 6 10 38 53), (Win 18 22 38 53))
$door = [pscustomobject]@{ glo = 8; ghi = 20; clo = 0; chi = 29; lintel = $true; sill = $false }
$doorOpenings = @($door, (Win 6 10 38 53), (Win 18 22 38 53))

# --- four walls closing the box -----------------------------------------------------------------
LayWall $true 0 $Wythe 0 (2 * $Nx) $longOpenings (-$CorniceProjection) $Wythe
LayWall $true $BackY0 ($BackY0 + $Wythe) 0 (2 * $Nx) $longOpenings $BackY0 ($BackY0 + $Wythe + $CorniceProjection)
LayWall $false 0 $Wythe $Se (2 * $Ny) $backOpenings $null $null
LayWall $false ($W - $Wythe) $W $Se (2 * $Ny) $doorOpenings $null $null

# --- pilasters: a stack-bond column per slot, projecting one wythe, both long walls -------------
foreach ($g in 3, 13, 23, 33, 43, 53)
{
    for ($c = 0; $c -le $PilasterTop; $c++)
    {
        $x = $g * $Half; $z = $c * $CoursePitch
        Add $x ($x + $BrickLen) (-$Half) (-$Joint) $z ($z + $Course) 'ClayBrick' ($c -eq 0)
        Add $x ($x + $BrickLen) ($BackY0 + $Half) ($BackY0 + $Half + $Wythe) $z ($z + $Course) 'ClayBrick' ($c -eq 0)
    }
}

# --- stepped gables on both end walls -----------------------------------------------------------
foreach ($xr in @(@(0, $Wythe), @(($W - $Wythe), $W)))
{
    for ($g = 0; $g -lt $GableCourses; $g++)
    {
        $c = $Eaves + $g; $z = $c * $CoursePitch
        foreach ($sp in (FillSpans $Se ($g + 1) (2 * $Ny - ($g + 1)) $c 2)) { Add $xr[0] $xr[1] $sp[0] $sp[1] $z ($z + $Course) 'ClayBrick' $false }
    }
}

# --- the stepped timber roof --------------------------------------------------------------------
$eavesTop = $Eaves * $CoursePitch - $Joint
Add 0 $W 0 $EavesBoardDepth ($eavesTop + $Joint) ($eavesTop + $Joint + $RoofThick) 'Timber' $false
Add 0 $W ($D - $EavesBoardDepth) $D ($eavesTop + $Joint) ($eavesTop + $Joint + $RoofThick) 'Timber' $false
for ($g = 0; $g -lt ($GableCourses - 1); $g++)
{
    $z = ($Eaves + $g) * $CoursePitch + $Course + $Joint
    Add 0 $W ($Se + ($g + 1) * $Half) ($Se + ($g + 2) * $Half - $Joint) $z ($z + $RoofThick) 'Timber' $false
    Add 0 $W ($BackY0 - ($g + 2) * $Half) ($BackY0 - ($g + 1) * $Half - $Joint) $z ($z + $RoofThick) 'Timber' $false
}
$apexCourse = $Eaves + $GableCourses - 1
$zr = $apexCourse * $CoursePitch + $Course + $Joint
Add 0 $W ($Se + $GableCourses * $Half) ($Se + ($GableCourses + 2) * $Half - $Joint) $zr ($zr + $RoofThick) 'Timber' $false

# --- two bonded chimneys outside the door end, with stone caps ----------------------------------
$cxlo = $W + $Joint; $cxhi = $cxlo + $BrickLen
foreach ($g in 2, 24)
{
    $y = $Se + $g * $Half; $yhi = $y + $BrickLen
    for ($c = 0; $c -lt $ChimneyCourses; $c++)
    {
        $z = $c * $CoursePitch
        if ($c % 2 -eq 0)
        {
            Add $cxlo $cxhi $y ($y + $Wythe) $z ($z + $Course) 'ClayBrick' ($c -eq 0)
            Add $cxlo $cxhi ($y + $Half) $yhi $z ($z + $Course) 'ClayBrick' ($c -eq 0)
        }
        else
        {
            Add $cxlo ($cxlo + $Wythe) $y $yhi $z ($z + $Course) 'ClayBrick' $false
            Add ($cxlo + $Half) $cxhi $y $yhi $z ($z + $Course) 'ClayBrick' $false
        }
    }
    $zc = $ChimneyCourses * $CoursePitch
    Add ($cxlo - $CapOverhang) ($cxhi + $CapOverhang) ($y - $CapOverhang) ($yhi + $CapOverhang) $zc ($zc + $Course) 'StructuralConcrete' $false
}

# --- self-check: no two boxes overlap ------------------------------------------------------------
$n = $Pieces.Count
$buckets = @{}
for ($i = 0; $i -lt $n; $i++)
{
    $p = $Pieces[$i]
    $k0 = [int][Math]::Floor($p.zlo / $CoursePitch); $k1 = [int][Math]::Floor(($p.zhi - 1e-6) / $CoursePitch)
    for ($k = $k0; $k -le $k1; $k++) { if (-not $buckets.ContainsKey($k)) { $buckets[$k] = New-Object System.Collections.Generic.List[int] }; $buckets[$k].Add($i) }
}
$overlaps = 0; $eps = 1e-6
foreach ($k in $buckets.Keys)
{
    $list = $buckets[$k]
    for ($a = 0; $a -lt $list.Count; $a++)
    {
        for ($b = $a + 1; $b -lt $list.Count; $b++)
        {
            $p = $Pieces[$list[$a]]; $q = $Pieces[$list[$b]]
            if ($p.xlo -lt $q.xhi - $eps -and $p.xhi -gt $q.xlo + $eps -and $p.ylo -lt $q.yhi - $eps -and $p.yhi -gt $q.ylo + $eps -and $p.zlo -lt $q.zhi - $eps -and $p.zhi -gt $q.zlo + $eps)
            {
                $overlaps++
                if ($overlaps -le 10) { Write-Host "OVERLAP: $($p.xlo),$($p.xhi) $($p.ylo),$($p.yhi) $($p.zlo),$($p.zhi) vs $($q.xlo),$($q.xhi) $($q.ylo),$($q.yhi) $($q.zlo),$($q.zhi)" }
            }
        }
    }
}
if ($overlaps -gt 0) { throw "$overlaps overlapping pair(s); nothing written" }

# --- write ---------------------------------------------------------------------------------------
$inv = [Globalization.CultureInfo]::InvariantCulture
function N($v) { ([double]$v).ToString('0.######', $inv) }

$sb = New-Object System.Text.StringBuilder
[void]$sb.Append("{`n  `"format`": `"DestructionGame.Layout`",`n  `"version`": 1,`n  `"jointThicknessCm`": 1,`n  `"threeDimensional`": true,`n  `"pieces`": [`n")
for ($i = 0; $i -lt $n; $i++)
{
    $p = $Pieces[$i]
    $comma = if ($i + 1 -lt $n) { ',' } else { '' }
    $gr = if ($p.g) { 'true' } else { 'false' }
    [void]$sb.Append("    { `"min`": [$(N $p.xlo), $(N $p.ylo), $(N $p.zlo)], `"max`": [$(N $p.xhi), $(N $p.yhi), $(N $p.zhi)], `"material`": `"$($p.mat)`", `"grounded`": $gr }$comma`n")
}
[void]$sb.Append("  ]`n}`n")

$dir = Split-Path -Parent $OutPath
if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
[IO.File]::WriteAllText($OutPath, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))

$byMat = $Pieces | Group-Object mat | ForEach-Object { "$($_.Name)=$($_.Count)" }
Write-Host "wrote $OutPath : $n pieces ($($byMat -join ', ')), $(($Pieces | Where-Object g).Count) grounded, 0 overlaps"
