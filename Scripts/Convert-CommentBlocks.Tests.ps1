<#
.SYNOPSIS
    Tests for Convert-CommentBlocks.ps1.

.DESCRIPTION
    Self-contained: no Pester, no modules. Run it directly.

        powershell -NoProfile -File Scripts/Convert-CommentBlocks.Tests.ps1

    Exits 0 if every test passes, 1 otherwise. These must be green before the
    script is turned loose on the repository - the failure mode of a comment
    rewriter is silent corruption, so the cases below are deliberately the nasty
    ones: `//` inside a string, `//` inside an existing block comment, a `*/`
    inside the comment text, and CRLF line endings.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Convert-CommentBlocks.ps1') -DotSourceOnly

$script:Passed = 0
$script:Failed = 0

function Show-Ws {
    param([string] $s)
    if ($null -eq $s) { return '<null>' }
    return ($s -replace "`r", '<CR>' -replace "`n", "<LF>`n")
}

function Assert-Equal {
    param([string] $Name, $Expected, $Actual)
    if ($Expected -ceq $Actual) {
        $script:Passed++
        Write-Host "  PASS  $Name"
    }
    else {
        $script:Failed++
        Write-Host "  FAIL  $Name" -ForegroundColor Red
        Write-Host "    expected:"
        Write-Host (Show-Ws $Expected)
        Write-Host "    actual:"
        Write-Host (Show-Ws $Actual)
    }
}

function Assert-True {
    param([string] $Name, [bool] $Condition, [string] $Detail = '')
    if ($Condition) { $script:Passed++; Write-Host "  PASS  $Name" }
    else { $script:Failed++; Write-Host "  FAIL  $Name  $Detail" -ForegroundColor Red }
}

function Assert-Unchanged {
    param([string] $Name, [string] $Text)
    $r = Convert-CommentBlockText -Text $Text
    Assert-Equal -Name $Name -Expected $Text -Actual $r.Text
    Assert-True -Name "$Name (0 blocks)" -Condition ($r.Blocks -eq 0) -Detail "got $($r.Blocks)"
    Assert-True -Name "$Name (not refused)" -Condition ($null -eq $r.Refused) -Detail "$($r.Refused)"
}

function LF { param([string[]] $Lines) return (($Lines -join "`n") + "`n") }
function CRLF { param([string[]] $Lines) return (($Lines -join "`r`n") + "`r`n") }

Write-Host ''
Write-Host 'Convert-CommentBlocks tests'
Write-Host '---------------------------'

# -- 1. the happy path ------------------------------------------------------
Write-Host 'basic conversion'
$src = LF @('// alpha', '// beta', 'int X = 1;')
$exp = LF @('/*', ' * alpha', ' * beta', ' */', 'int X = 1;')
$r = Convert-CommentBlockText -Text $src
Assert-Equal -Name 'two-line block becomes one block comment' -Expected $exp -Actual $r.Text
Assert-True  -Name 'reports one block' -Condition ($r.Blocks -eq 1) -Detail "got $($r.Blocks)"

# -- 2. a single isolated // line is left alone -----------------------------
Write-Host 'single isolated line'
Assert-Unchanged -Name 'lone // line untouched' -Text (LF @('// just the one', 'int X = 1;'))
Assert-Unchanged -Name 'two lone // lines split by code' -Text (LF @('// one', 'int X = 1;', '// two', 'int Y = 2;'))

# -- 3. trailing comments after code ----------------------------------------
Write-Host 'trailing comments'
Assert-Unchanged -Name 'trailing comment after code' -Text (LF @('int X = 1; // note', 'int Y = 2; // note two'))
Assert-Unchanged -Name 'trailing comment then pure line' -Text (LF @('int X = 1; // note', '// lone pure line', 'int Y = 2;'))

# a trailing comment must not glue itself onto a following block
$src = LF @('int X = 1; // trailing', '// block a', '// block b')
$exp = LF @('int X = 1; // trailing', '/*', ' * block a', ' * block b', ' */')
$r = Convert-CommentBlockText -Text $src
Assert-Equal -Name 'block after a trailing comment converts, trailing one does not' -Expected $exp -Actual $r.Text

# -- 4. // inside a string literal ------------------------------------------
Write-Host 'string literals'
Assert-Unchanged -Name '// inside TEXT() is not a comment' -Text (LF @(
    'const TCHAR* A = TEXT("http://example.com");',
    'const TCHAR* B = TEXT("a // b");'))
Assert-Unchanged -Name '// inside a char literal is not a comment' -Text (LF @(
    "char S = '/';",
    "char T = '/';"))
Assert-Unchanged -Name 'escaped quote does not leak out of the string' -Text (LF @(
    'const TCHAR* A = TEXT("she said \" // not a comment");',
    'int Y = 2;'))

# code before a real block, with a string on the same file, still converts
$src = LF @('FString U = TEXT("http://x");', '// real one', '// real two')
$exp = LF @('FString U = TEXT("http://x");', '/*', ' * real one', ' * real two', ' */')
$r = Convert-CommentBlockText -Text $src
Assert-Equal -Name 'string line untouched while a real block converts' -Expected $exp -Actual $r.Text

# -- 5. // inside an existing block comment ---------------------------------
Write-Host 'existing block comments'
Assert-Unchanged -Name '// lines inside /* */ untouched' -Text (LF @(
    '/*', '// looks like a comment line', '// so does this', '*/', 'int X = 1;'))
Assert-Unchanged -Name '// lines inside /** */ doc comment untouched' -Text (LF @(
    '/**', ' * doc', ' * // not a line comment', ' * // nor this', ' */', 'int X = 1;'))

# a real block after a closed block comment still converts
$src = LF @('/* one liner */', '// a', '// b')
$exp = LF @('/* one liner */', '/*', ' * a', ' * b', ' */')
$r = Convert-CommentBlockText -Text $src
Assert-Equal -Name 'block after a closed /* */ converts' -Expected $exp -Actual $r.Text

# -- 6. indentation ----------------------------------------------------------
Write-Host 'indentation'
$src = LF @("`tif (X)", "`t{", "`t`t// inner one", "`t`t// inner two", "`t`tDoThing();", "`t}")
$exp = LF @("`tif (X)", "`t{", "`t`t/*", "`t`t * inner one", "`t`t * inner two", "`t`t */", "`t`tDoThing();", "`t}")
$r = Convert-CommentBlockText -Text $src
Assert-Equal -Name 'tab indentation preserved exactly' -Expected $exp -Actual $r.Text

# relative indentation inside the comment survives (ASCII diagrams)
$src = LF @('//   ===   ===', '//    |     |')
$exp = LF @('/*', ' *   ===   ===', ' *    |     |', ' */')
$r = Convert-CommentBlockText -Text $src
Assert-Equal -Name 'relative indentation inside the comment preserved' -Expected $exp -Actual $r.Text

# mixed indentation inside one run is refused, not guessed at
$r = Convert-CommentBlockText -Text (LF @('// flush', "`t// indented"))
Assert-True -Name 'mixed indentation refused' -Condition ($r.Refused -like '*mixed indentation*') -Detail "$($r.Refused)"

# -- 7. */ and /* in the comment text ---------------------------------------
Write-Host 'dangerous comment text'
$src = LF @('// see the */ terminator', '// second line')
$r = Convert-CommentBlockText -Text $src
Assert-True  -Name '*/ in comment text refused loudly' -Condition ($r.Refused -like "*'*/'*") -Detail "$($r.Refused)"
Assert-Equal -Name '*/ refusal leaves text untouched' -Expected $src -Actual $r.Text

$src = LF @('// an opener /* here', '// second line')
$r = Convert-CommentBlockText -Text $src
Assert-True  -Name '/* in comment text refused loudly' -Condition ($null -ne $r.Refused) -Detail 'expected a refusal'
Assert-Equal -Name '/* refusal leaves text untouched' -Expected $src -Actual $r.Text

# -- 8. blank lines split blocks --------------------------------------------
Write-Host 'blank lines'
$src = LF @('// para one a', '// para one b', '', '// para two a', '// para two b')
$exp = LF @('/*', ' * para one a', ' * para one b', ' */', '', '/*', ' * para two a', ' * para two b', ' */')
$r = Convert-CommentBlockText -Text $src
Assert-Equal -Name 'blank line splits one run into two blocks' -Expected $exp -Actual $r.Text
Assert-True  -Name 'blank line split reports two blocks' -Condition ($r.Blocks -eq 2) -Detail "got $($r.Blocks)"

# a blank line either side of a lone // line leaves it alone
Assert-Unchanged -Name 'blank-separated lone lines untouched' -Text (LF @('// one', '', '// two'))

# an empty // line inside a run does NOT split it
$src = LF @('// para one', '//', '// para two')
$exp = LF @('/*', ' * para one', ' *', ' * para two', ' */')
$r = Convert-CommentBlockText -Text $src
Assert-Equal -Name 'empty // line is a comment line, not a split' -Expected $exp -Actual $r.Text

# -- 9. line endings ---------------------------------------------------------
Write-Host 'line endings'
$src = CRLF @('// alpha', '// beta', 'int X = 1;')
$exp = CRLF @('/*', ' * alpha', ' * beta', ' */', 'int X = 1;')
$r = Convert-CommentBlockText -Text $src
Assert-Equal -Name 'CRLF preserved throughout' -Expected $exp -Actual $r.Text
Assert-True  -Name 'no bare LF introduced into a CRLF file' `
    -Condition (($r.Text -split "`n").Length - 1 -eq ($r.Text -split "`r`n").Length - 1) `
    -Detail 'LF count must equal CRLF count'

# no trailing newline at end of file
$src = "// alpha`r`n// beta"
$exp = "/*`r`n * alpha`r`n * beta`r`n */"
$r = Convert-CommentBlockText -Text $src
Assert-Equal -Name 'missing final newline stays missing' -Expected $exp -Actual $r.Text

# -- 10. whitespace-only comment content -------------------------------------
Write-Host 'whitespace handling'
$src = LF @('//   ', '// text  ')
$exp = LF @('/*', ' *', ' * text', ' */')
$r = Convert-CommentBlockText -Text $src
Assert-Equal -Name 'trailing whitespace dropped, no trailing-space lines emitted' -Expected $exp -Actual $r.Text

# -- 11. divider / banner lines are just comment text ------------------------
Write-Host 'banners'
$src = LF @('// --- section -------------', '// what it does')
$exp = LF @('/*', ' * --- section -------------', ' * what it does', ' */')
$r = Convert-CommentBlockText -Text $src
Assert-Equal -Name 'banner line converts like any other comment text' -Expected $exp -Actual $r.Text

# -- 12. backslash continuation refused --------------------------------------
Write-Host 'backslash continuation'
$src = LF @('// continued \', '// onto here')
$r = Convert-CommentBlockText -Text $src
Assert-True  -Name 'backslash-continued comment refused' -Condition ($r.Refused -like '*backslash*') -Detail "$($r.Refused)"
Assert-Equal -Name 'continuation refusal leaves text untouched' -Expected $src -Actual $r.Text

# -- 13. constructs the lexer does not model ---------------------------------
Write-Host 'unsupported constructs'
$r = Convert-CommentBlockText -Text (LF @('auto S = R"(raw // text)";', '// a', '// b'))
Assert-True -Name 'raw string literal refuses the file' -Condition ($r.Refused -like '*raw string*') -Detail "$($r.Refused)"

$r = Convert-CommentBlockText -Text (LF @('/* never closed', '// a', '// b'))
Assert-True -Name 'unterminated block comment refuses the file' -Condition ($r.Refused -like '*unterminated block*') -Detail "$($r.Refused)"

# C++14 digit separators must not be read as character literals
$src = LF @("int N = 1'000'000;", '// a', '// b')
$exp = LF @("int N = 1'000'000;", '/*', ' * a', ' * b', ' */')
$r = Convert-CommentBlockText -Text $src
Assert-Equal -Name 'digit separator is not a character literal' -Expected $exp -Actual $r.Text

# -- 14. the self-check machinery itself -------------------------------------
Write-Host 'self-check streams'
$before = LF @('// alpha beta', '// gamma', 'int X = 1;')
$after = (Convert-CommentBlockText -Text $before).Text
Assert-Equal -Name 'code stream unchanged by conversion' `
    -Expected (Get-CodeStream -Text $before) -Actual (Get-CodeStream -Text $after)
Assert-Equal -Name 'comment tokens unchanged by conversion' `
    -Expected ((Get-CommentTokens -Text $before) -join '|') -Actual ((Get-CommentTokens -Text $after) -join '|')
Assert-True -Name 'code stream actually contains the code' `
    -Condition ((Get-CodeStream -Text $after) -eq 'intX=1;') -Detail (Get-CodeStream -Text $after)
Assert-True -Name 'comment tokens actually contain the words' `
    -Condition (((Get-CommentTokens -Text $after) -join '|') -eq 'alpha|beta|gamma') `
    -Detail ((Get-CommentTokens -Text $after) -join '|')

# -- 15. file IO round trip ---------------------------------------------------
Write-Host 'file IO'
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("ccb_" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    # CRLF, with a UTF-8 BOM, and a non-ASCII character in the comment.
    $crlfFile = Join-Path $tmp 'Crlf.cpp'
    $content = CRLF @('// mortar - a joint', '// area in cm2', 'int X = 1;')
    $enc = New-Object System.Text.UTF8Encoding($true)
    [System.IO.File]::WriteAllBytes($crlfFile, $enc.GetPreamble() + $enc.GetBytes($content))

    $rc = Invoke-CommentBlockConversion -Path @($tmp) -Include @('*.cpp')
    Assert-True -Name 'conversion run returns 0' -Condition ($rc -eq 0) -Detail "got $rc"

    $bytes = [System.IO.File]::ReadAllBytes($crlfFile)
    Assert-True -Name 'BOM preserved' `
        -Condition ($bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    $round = (Read-SourceFile -File $crlfFile).Text
    Assert-Equal -Name 'file on disk has the converted CRLF text' `
        -Expected (CRLF @('/*', ' * mortar - a joint', ' * area in cm2', ' */', 'int X = 1;')) -Actual $round
    Assert-True -Name 'no bare LF on disk' `
        -Condition ((($round -split "`n").Length) -eq (($round -split "`r`n").Length))

    # -Check writes nothing.
    $checkFile = Join-Path $tmp 'Check.cpp'
    $before = CRLF @('// a', '// b', 'int Y = 2;')
    [System.IO.File]::WriteAllBytes($checkFile, ([System.Text.Encoding]::UTF8).GetBytes($before))
    $rc = Invoke-CommentBlockConversion -Path @($checkFile) -Check -Include @('*.cpp')
    Assert-True -Name '-Check returns 1 when there is work to do' -Condition ($rc -eq 1) -Detail "got $rc"
    Assert-Equal -Name '-Check writes nothing' -Expected $before -Actual (Read-SourceFile -File $checkFile).Text

    # A refused file returns 2 and is left alone.
    $badFile = Join-Path $tmp 'Bad.cpp'
    $bad = CRLF @('// contains */ here', '// second')
    [System.IO.File]::WriteAllBytes($badFile, ([System.Text.Encoding]::UTF8).GetBytes($bad))
    $rc = Invoke-CommentBlockConversion -Path @($badFile) -Include @('*.cpp')
    Assert-True -Name 'refusal returns 2' -Condition ($rc -eq 2) -Detail "got $rc"
    Assert-Equal -Name 'refused file untouched on disk' -Expected $bad -Actual (Read-SourceFile -File $badFile).Text

    # Build.cs is not in the default include set.
    $csFile = Join-Path $tmp 'Thing.Build.cs'
    $cs = CRLF @('// a', '// b')
    [System.IO.File]::WriteAllBytes($csFile, ([System.Text.Encoding]::UTF8).GetBytes($cs))
    $rc = Invoke-CommentBlockConversion -Path @($tmp) -Include @('*.h', '*.cpp')
    Assert-Equal -Name 'Build.cs left alone by the default include set' -Expected $cs -Actual (Read-SourceFile -File $csFile).Text

    # Idempotence: a second run over already-converted files changes nothing.
    $secondBefore = (Read-SourceFile -File $crlfFile).Text
    $rc = Invoke-CommentBlockConversion -Path @($crlfFile) -Include @('*.cpp')
    Assert-Equal -Name 'second run is a no-op' -Expected $secondBefore -Actual (Read-SourceFile -File $crlfFile).Text
}
finally {
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ''
Write-Host "passed: $script:Passed   failed: $script:Failed"
if ($script:Failed -gt 0) { exit 1 }
exit 0
