<#
.SYNOPSIS
    Converts blocks of consecutive `//` comment lines in C++ source to a single
    `/* ... */` block comment.

.DESCRIPTION
    House style for this project (see CLAUDE.md, "Comment style") is that a run of
    two or more consecutive whole-line `//` comments is written as one block
    comment. This script finds those runs and rewrites them, and can also just
    report them (-Check) so it can be re-run later or wired into a hook.

    It is NOT a regex over lines. It runs a small C++ lexer that tracks string
    literals, character literals and existing block comments, because a line-based
    regex corrupts things like TEXT("http://example") on its first outing.

    WHAT IT CONVERTS
      Two or more consecutive lines that are ENTIRELY a `//` comment (only
      whitespace may precede the `//`), at a uniform indentation. Emitted as:

          <indent>/*
          <indent> * first line of text
          <indent> * second line of text
          <indent> */

      matching the `/** ... */` continuation style already used in this codebase.
      One leading space after `//` is absorbed by the ` * ` prefix, so relative
      indentation inside the comment (ASCII diagrams, indented sub-points) is
      preserved. Trailing whitespace on a comment line is dropped.

    WHAT IT DELIBERATELY DOES NOT TOUCH
      * A single isolated `//` line. The rule is about blocks; a lone line stays.
      * Trailing comments after code (`int X = 1; // note`), including runs of them
        on consecutive code lines. Those are not comment-line blocks.
      * Anything already inside a `/* */` or `/** */` comment.
      * `//` inside a string or character literal.
      * Line endings. Each line keeps its own terminator, so a CRLF file stays CRLF.
      * A UTF-8 BOM, if the file has one.
      * `*.Build.cs` / `*.Target.cs` and anything else that is not `*.h` / `*.cpp`.
        Those are C#, and Epic's build tooling, not this project's C++ house style.
      * Blocks separated by a blank line: a blank line is a deliberate paragraph
        break, so it splits one run into two blocks rather than being merged away.

    WHAT IT REFUSES (reports loudly, leaves the file untouched)
      * A block whose text contains `*/` or `/*`, which would terminate or nest the
        emitted block comment.
      * A block whose lines are not all at the same indentation.
      * A `//` comment continued onto the next line with a trailing backslash.
      * A file containing a raw string literal (R"...") or an unterminated literal,
        which the lexer does not model.
      * Any file where the post-transform self-check fails (see below).

    SELF-CHECK (runs before anything is written)
      For every file, the transformed text is re-lexed and compared with the
      original on two axes:
        1. Code stream  - every character outside a comment, whitespace removed,
                          must be byte-identical. No line of code can be lost.
        2. Comment text - every whitespace-separated token inside a comment, with
                          markers stripped, must be identical in the same order.
                          The words survive; only the delimiters change.
      A file that fails either check is refused, not written.

.PARAMETER Path
    Files or directories to process. Defaults to the Source directory beside this
    script's repository root.

.PARAMETER Check
    Dry run. Reports what would change and writes nothing.

.PARAMETER Include
    Filename patterns to process under any directory in -Path. Default *.h, *.cpp.

.PARAMETER DotSourceOnly
    Define the functions and return without doing any work. Used by the tests.

.OUTPUTS
    Exit code 0 = clean (nothing to do, or everything written successfully).
    Exit code 1 = -Check found blocks that would be converted.
    Exit code 2 = at least one file was refused.

.EXAMPLE
    ./Scripts/Convert-CommentBlocks.ps1 -Check
    ./Scripts/Convert-CommentBlocks.ps1
#>
[CmdletBinding()]
param(
    [string[]] $Path,
    [switch]   $Check,
    [string[]] $Include = @('*.h', '*.cpp'),
    [switch]   $DotSourceOnly
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------------------
# Lexing
# ---------------------------------------------------------------------------

function Split-SourceLines {
    <#
        Splits text into physical lines, returning for each one its body (no
        terminator) and its exact terminator, so the terminator can be put back
        verbatim. A final line with no terminator gets an empty one.
    #>
    param([string] $Text)

    $lines = New-Object System.Collections.ArrayList
    $start = 0
    for ($i = 0; $i -lt $Text.Length; $i++) {
        if ($Text[$i] -eq "`n") {
            $raw = $Text.Substring($start, $i - $start + 1)
            [void] $lines.Add($raw)
            $start = $i + 1
        }
    }
    if ($start -lt $Text.Length) {
        [void] $lines.Add($Text.Substring($start))
    }

    $out = New-Object System.Collections.ArrayList
    foreach ($raw in $lines) {
        $term = ''
        $body = $raw
        if ($body.EndsWith("`r`n")) {
            $term = "`r`n"
            $body = $body.Substring(0, $body.Length - 2)
        }
        elseif ($body.EndsWith("`n")) {
            $term = "`n"
            $body = $body.Substring(0, $body.Length - 1)
        }
        elseif ($body.EndsWith("`r")) {
            # A lone CR at end of file, with no newline after it.
            $term = "`r"
            $body = $body.Substring(0, $body.Length - 1)
        }
        [void] $out.Add([pscustomobject] @{ Body = $body; Terminator = $term })
    }
    return , $out.ToArray()
}

function Get-LexedLines {
    <#
        Runs the C++ lexer over the split lines and annotates each with where a
        line comment starts (if any), whether the line began inside a block
        comment, and whether the line is entirely a `//` comment.

        Returns @{ Lines = <array>; Problem = <string or $null> }. A non-null
        Problem means the file must be refused.
    #>
    param([object[]] $Split)

    $inBlock = $false
    $problem = $null
    $result = New-Object System.Collections.ArrayList

    for ($n = 0; $n -lt $Split.Count; $n++) {
        $body = $Split[$n].Body
        $len = $body.Length
        $startedInBlock = $inBlock
        $commentStart = -1
        $i = 0

        while ($i -lt $len) {
            $c = $body[$i]

            if ($inBlock) {
                if ($c -eq '*' -and ($i + 1) -lt $len -and $body[$i + 1] -eq '/') {
                    $inBlock = $false
                    $i += 2
                    continue
                }
                $i++
                continue
            }

            # Raw string literals are not modelled. Refuse rather than guess.
            if (($c -eq 'R' -or $c -eq 'u' -or $c -eq 'U' -or $c -eq 'L') -and
                ($i + 1) -lt $len -and $body[$i + 1] -eq '"' -and $c -eq 'R') {
                $problem = "line $($n + 1): raw string literal (R`"...`") is not supported by the lexer"
                break
            }

            if ($c -eq '"' -or $c -eq "'") {
                # A single quote preceded by an identifier character is a C++14
                # digit separator (1'000), not the start of a character literal.
                if ($c -eq "'" -and $i -gt 0 -and $body[$i - 1] -match '[A-Za-z0-9_]') {
                    $i++
                    continue
                }

                $quote = $c
                $i++
                $closed = $false
                while ($i -lt $len) {
                    if ($body[$i] -eq '\') { $i += 2; continue }
                    if ($body[$i] -eq $quote) { $i++; $closed = $true; break }
                    $i++
                }
                if (-not $closed) {
                    $problem = "line $($n + 1): unterminated string or character literal"
                    break
                }
                continue
            }

            if ($c -eq '/' -and ($i + 1) -lt $len) {
                if ($body[$i + 1] -eq '/') { $commentStart = $i; break }
                if ($body[$i + 1] -eq '*') { $inBlock = $true; $i += 2; continue }
            }

            $i++
        }

        if ($problem) { break }

        $isPure = $false
        $indent = ''
        $content = ''
        if (-not $startedInBlock -and $commentStart -ge 0) {
            $indent = $body.Substring(0, $commentStart)
            if ($indent -match '^\s*$') {
                $isPure = $true
                $content = $body.Substring($commentStart + 2)
            }
        }

        [void] $result.Add([pscustomobject] @{
            Number         = $n + 1
            Body           = $body
            Terminator     = $Split[$n].Terminator
            StartedInBlock = $startedInBlock
            CommentStart   = $commentStart
            IsPureComment  = $isPure
            Indent         = $indent
            Content        = $content
        })
    }

    if (-not $problem -and $inBlock) {
        $problem = 'file ends inside an unterminated block comment'
    }

    return @{ Lines = $result.ToArray(); Problem = $problem }
}

# ---------------------------------------------------------------------------
# Self-check extraction
# ---------------------------------------------------------------------------

function Get-CodeStream {
    <#
        Every character outside a comment, with all whitespace removed. If the
        transform ever drops a line of code, this changes.
    #>
    param([string] $Text)

    $split = Split-SourceLines -Text $Text
    $lexed = Get-LexedLines -Split $split
    if ($lexed.Problem) { return $null }

    $sb = New-Object System.Text.StringBuilder
    $inBlock = $false
    foreach ($line in $lexed.Lines) {
        $body = $line.Body
        $len = $body.Length
        $i = 0
        while ($i -lt $len) {
            $c = $body[$i]
            if ($inBlock) {
                if ($c -eq '*' -and ($i + 1) -lt $len -and $body[$i + 1] -eq '/') { $inBlock = $false; $i += 2; continue }
                $i++
                continue
            }
            if ($c -eq '"' -or ($c -eq "'" -and -not ($i -gt 0 -and $body[$i - 1] -match '[A-Za-z0-9_]'))) {
                $quote = $c
                [void] $sb.Append($c)
                $i++
                while ($i -lt $len) {
                    [void] $sb.Append($body[$i])
                    if ($body[$i] -eq '\') { if (($i + 1) -lt $len) { [void] $sb.Append($body[$i + 1]) }; $i += 2; continue }
                    if ($body[$i] -eq $quote) { $i++; break }
                    $i++
                }
                continue
            }
            if ($c -eq '/' -and ($i + 1) -lt $len) {
                if ($body[$i + 1] -eq '/') { break }
                if ($body[$i + 1] -eq '*') { $inBlock = $true; $i += 2; continue }
            }
            if ($c -notmatch '\s') { [void] $sb.Append($c) }
            $i++
        }
    }
    return $sb.ToString()
}

function Get-CommentTokens {
    <#
        Every whitespace-separated token inside a comment, in order, with comment
        markers and per-line ` * ` continuation prefixes stripped. Identical for
        `// foo bar` and for the block comment it becomes.
    #>
    param([string] $Text)

    $split = Split-SourceLines -Text $Text
    $lexed = Get-LexedLines -Split $split
    if ($lexed.Problem) { return $null }

    $pieces = New-Object System.Collections.ArrayList
    $inBlock = $false
    foreach ($line in $lexed.Lines) {
        $body = $line.Body
        $len = $body.Length
        $i = 0
        while ($i -lt $len) {
            $c = $body[$i]
            if ($inBlock) {
                $end = $len
                for ($j = $i; $j -lt ($len - 1); $j++) {
                    if ($body[$j] -eq '*' -and $body[$j + 1] -eq '/') { $end = $j; break }
                }
                $chunk = $body.Substring($i, $end - $i)
                # Strip a leading ` * ` continuation prefix when the chunk is the
                # whole line's comment content.
                if ($i -eq 0) { $chunk = $chunk -replace '^\s*\*(?!/)', '' }
                [void] $pieces.Add($chunk)
                if ($end -lt $len) { $inBlock = $false; $i = $end + 2 } else { $i = $len }
                continue
            }
            if ($c -eq '"' -or ($c -eq "'" -and -not ($i -gt 0 -and $body[$i - 1] -match '[A-Za-z0-9_]'))) {
                $quote = $c
                $i++
                while ($i -lt $len) {
                    if ($body[$i] -eq '\') { $i += 2; continue }
                    if ($body[$i] -eq $quote) { $i++; break }
                    $i++
                }
                continue
            }
            if ($c -eq '/' -and ($i + 1) -lt $len) {
                if ($body[$i + 1] -eq '/') { [void] $pieces.Add($body.Substring($i + 2)); $i = $len; continue }
                if ($body[$i + 1] -eq '*') { $inBlock = $true; $i += 2; continue }
            }
            $i++
        }
    }

    $all = ($pieces -join " `n ")
    $tokens = $all -split '\s+' | Where-Object { $_ -ne '' }
    return , @($tokens)
}

# ---------------------------------------------------------------------------
# Transform
# ---------------------------------------------------------------------------

function Convert-CommentBlockText {
    <#
        The whole transform for one file's text. Returns
        @{ Text = <string>; Blocks = <int>; Refused = <string or $null>;
           BlockLines = <array of "start-end"> }.
        Refused non-null means Text is the original, unchanged.
    #>
    param([string] $Text)

    $fail = {
        param($why)
        return @{ Text = $Text; Blocks = 0; Refused = $why; BlockLines = @() }
    }

    $split = Split-SourceLines -Text $Text
    $lexed = Get-LexedLines -Split $split
    if ($lexed.Problem) { return (& $fail $lexed.Problem) }
    $lines = $lexed.Lines

    # Gather maximal runs of >= 2 pure comment lines. A blank line, or any line
    # that is not a pure comment line, ends the run.
    $blocks = New-Object System.Collections.ArrayList
    $runStart = -1
    for ($i = 0; $i -le $lines.Count; $i++) {
        $isPure = $false
        if ($i -lt $lines.Count) { $isPure = $lines[$i].IsPureComment }
        if ($isPure) {
            if ($runStart -lt 0) { $runStart = $i }
        }
        else {
            if ($runStart -ge 0 -and ($i - $runStart) -ge 2) {
                [void] $blocks.Add(@{ Start = $runStart; End = $i - 1 })
            }
            $runStart = -1
        }
    }

    if ($blocks.Count -eq 0) {
        return @{ Text = $Text; Blocks = 0; Refused = $null; BlockLines = @() }
    }

    # Vet every block before touching anything, so a bad block refuses the file
    # rather than half-converting it.
    foreach ($b in $blocks) {
        $indent = $lines[$b.Start].Indent
        for ($i = $b.Start; $i -le $b.End; $i++) {
            $ln = $lines[$i]
            if ($ln.Indent -ne $indent) {
                return (& $fail "lines $($lines[$b.Start].Number)-$($lines[$b.End].Number): block has mixed indentation")
            }
            if ($ln.Content -like '*`*/*') {
                return (& $fail "line $($ln.Number): comment text contains '*/', which would close the emitted block comment early")
            }
            if ($ln.Content -like '*/`**') {
                return (& $fail "line $($ln.Number): comment text contains '/*', which would nest inside the emitted block comment")
            }
            if ($ln.Content.EndsWith('\')) {
                return (& $fail "line $($ln.Number): comment is continued onto the next line with a backslash")
            }
        }
    }

    # The terminator to use when a line has none of its own - only possible for
    # the very last line of a file that does not end in a newline. Take whatever
    # the rest of the file uses so a CRLF file never gains a bare LF.
    $crlfCount = 0
    $lfCount = 0
    foreach ($ln in $lines) {
        if ($ln.Terminator -eq "`r`n") { $crlfCount++ }
        elseif ($ln.Terminator -eq "`n") { $lfCount++ }
    }
    $defaultTerm = "`r`n"
    if ($lfCount -gt $crlfCount) { $defaultTerm = "`n" }

    # Rebuild.
    $emitted = New-Object 'System.Collections.Generic.List[string]'
    $blockLines = New-Object System.Collections.ArrayList
    $byStart = @{}
    foreach ($b in $blocks) { $byStart[$b.Start] = $b }

    $i = 0
    while ($i -lt $lines.Count) {
        if ($byStart.ContainsKey($i)) {
            $b = $byStart[$i]
            $indent = $lines[$b.Start].Indent
            [void] $blockLines.Add("$($lines[$b.Start].Number)-$($lines[$b.End].Number)")

            $openTerm = $lines[$b.Start].Terminator
            if ($openTerm -eq '') { $openTerm = $defaultTerm }
            $emitted.Add($indent + '/*' + $openTerm)

            for ($j = $b.Start; $j -le $b.End; $j++) {
                $content = $lines[$j].Content
                if ($content.StartsWith(' ')) { $content = $content.Substring(1) }
                $content = $content.TrimEnd()
                # The ` */` closer always needs a line of its own, so a last line
                # with no terminator (end of a file with no trailing newline)
                # borrows the file's usual one.
                $term = $lines[$j].Terminator
                if ($term -eq '') { $term = $defaultTerm }
                if ($content -eq '') { $emitted.Add($indent + ' *' + $term) }
                else { $emitted.Add($indent + ' * ' + $content + $term) }
            }

            # The closer inherits the last comment line's own terminator, so a
            # file with no trailing newline keeps not having one.
            $emitted.Add($indent + ' */' + $lines[$b.End].Terminator)
            $i = $b.End + 1
            continue
        }

        $emitted.Add($lines[$i].Body + $lines[$i].Terminator)
        $i++
    }

    $newText = -join $emitted

    # Self-check. Nothing is written unless both streams survive intact.
    $codeBefore = Get-CodeStream -Text $Text
    $codeAfter = Get-CodeStream -Text $newText
    if ($null -eq $codeAfter) { return (& $fail 'self-check: transformed text failed to lex') }
    if ($codeBefore -ne $codeAfter) { return (& $fail 'self-check: code outside comments changed') }

    $tokBefore = Get-CommentTokens -Text $Text
    $tokAfter = Get-CommentTokens -Text $newText
    if ($null -eq $tokAfter) { return (& $fail 'self-check: transformed text failed to lex') }
    if ($tokBefore.Count -ne $tokAfter.Count) {
        return (& $fail "self-check: comment token count changed ($($tokBefore.Count) -> $($tokAfter.Count))")
    }
    for ($k = 0; $k -lt $tokBefore.Count; $k++) {
        if ($tokBefore[$k] -ne $tokAfter[$k]) {
            return (& $fail "self-check: comment text changed at token $k ('$($tokBefore[$k])' -> '$($tokAfter[$k])')")
        }
    }

    return @{ Text = $newText; Blocks = $blocks.Count; Refused = $null; BlockLines = $blockLines.ToArray() }
}

# ---------------------------------------------------------------------------
# File IO - byte-exact apart from the comment rewrite
# ---------------------------------------------------------------------------

function Read-SourceFile {
    param([string] $File)
    $bytes = [System.IO.File]::ReadAllBytes($File)
    $hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
    $offset = 0
    if ($hasBom) { $offset = 3 }
    $text = [System.Text.Encoding]::UTF8.GetString($bytes, $offset, $bytes.Length - $offset)
    return @{ Text = $text; HasBom = $hasBom }
}

function Write-SourceFile {
    param([string] $File, [string] $Text, [bool] $HasBom)
    $enc = New-Object System.Text.UTF8Encoding($HasBom)
    [System.IO.File]::WriteAllBytes($File, $enc.GetPreamble() + $enc.GetBytes($Text))
}

# ---------------------------------------------------------------------------

function Invoke-CommentBlockConversion {
    param([string[]] $Path, [switch] $Check, [string[]] $Include)

    $files = New-Object System.Collections.ArrayList
    foreach ($p in $Path) {
        if (Test-Path -LiteralPath $p -PathType Container) {
            foreach ($pattern in $Include) {
                Get-ChildItem -LiteralPath $p -Recurse -File -Filter $pattern |
                    ForEach-Object { [void] $files.Add($_.FullName) }
            }
        }
        elseif (Test-Path -LiteralPath $p -PathType Leaf) {
            [void] $files.Add((Resolve-Path -LiteralPath $p).ProviderPath)
        }
        else {
            Write-Warning "Not found: $p"
        }
    }

    $totalBlocks = 0
    $changedFiles = 0
    $refusals = New-Object System.Collections.ArrayList

    foreach ($file in ($files | Sort-Object -Unique)) {
        $read = Read-SourceFile -File $file
        $res = Convert-CommentBlockText -Text $read.Text

        if ($res.Refused) {
            [void] $refusals.Add("$file : $($res.Refused)")
            Write-Host "REFUSED  $file"
            Write-Host "         $($res.Refused)"
            continue
        }
        if ($res.Blocks -eq 0) { continue }

        $totalBlocks += $res.Blocks
        $changedFiles++
        $verb = 'converted'
        if ($Check) { $verb = 'would convert' }
        Write-Host "$verb $($res.Blocks) block(s)  $file"
        Write-Host "         at lines: $($res.BlockLines -join ', ')"

        if (-not $Check) {
            Write-SourceFile -File $file -Text $res.Text -HasBom $read.HasBom
        }
    }

    Write-Host ''
    Write-Host "Files scanned : $(($files | Sort-Object -Unique).Count)"
    Write-Host "Files changed : $changedFiles"
    Write-Host "Blocks        : $totalBlocks"
    Write-Host "Refused       : $($refusals.Count)"

    if ($refusals.Count -gt 0) { return 2 }
    if ($Check -and $changedFiles -gt 0) { return 1 }
    return 0
}

if ($DotSourceOnly) { return }

if (-not $Path -or $Path.Count -eq 0) {
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $Path = @((Join-Path $repoRoot 'Source'))
}

exit (Invoke-CommentBlockConversion -Path $Path -Check:$Check -Include $Include)
