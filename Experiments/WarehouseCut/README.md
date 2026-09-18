# Warehouse cut experiments

A reusable harness for pulling pieces out of `Lvl_Warehouse` and recording what happens. Each
experiment is one row in a table; running it produces a folder here with a plain-English
`SUMMARY.txt`, the data, and frames.

## Run one

```
# live capture (frames + data) — needs a real RHI, so NO -nullrhi:
UnrealEditor-Cmd DestructionGame.uproject /Game/Maps/Lvl_Sandbox -game -RenderOffScreen
  -ResX=1920 -ResY=1080 -ForceRes -nosplash -NoSound -unattended -nopause -log
  -ExecCmds="Automation RunTests Experiment.WarehouseCut.<RowName>"

# headless timing spread (five fresh loads of the same cut, decision time only):
UnrealEditor-Cmd DestructionGame.uproject -nullrhi -unattended -nopause -nosplash -NoSound -log
  -ExecCmds="Automation RunTests Experiment.WarehouseCutTiming.<RowName>"
```

Leave `.<RowName>` off to run every row.

## The rows

Defined in `Source/DestructionGame/Tests/WarehouseCutExperiment.cpp`, in the `Specs()` table.

| row | what it pulls |
|---|---|
| `BackWallCourse37` | course 37 (upper sill course) across the back long wall |
| `FrontWallCourse37` | the same course across the front long wall |
| `BothLongWallsCourse37` | course 37 across both long walls at once |

## Add a row

Append one `FCutSpec` to `Specs()` — a name, the course number, which long walls (`bFront`/`bBack`),
and which face to photograph close. It appears immediately as `Experiment.WarehouseCut.<YourName>`.
The cut geometry (which course, which wall by its Y band, pilaster vs wythe, the click ray for each
piece) is all shared; a new row writes no code.

To cut something the current spec shape cannot describe (a window jamb, a chimney, a diagonal),
widen `FCutSpec` and `IsCutPiece` — that is the one place the "which pieces" question lives.

## What each run folder holds

| file | what |
|---|---|
| `SUMMARY.txt` | the headline: pieces cut, released (by material and band), what stood, the break-decision timing, and the mechanism |
| `break_report.txt` | the baseline settle and the cut's settle, pass by pass, per fixpoint iteration |
| `selection.csv` | the selected pieces as laid |
| `pieces_after_cut.csv` | every piece the instant after the cut |
| `joints.csv` | every joint: ends, normal, area, which pass severed it, utilisation |
| `fall_summary.csv` | per frame for 8 s, including real ms/frame |
| `fall_pieces.csv` | per released piece every 6th frame |
| `pieces_final.csv` | laid vs final position |
| `headless_timing.txt` | five headless repeats of the decision |
| `*.png` | the selection and the fall (gitignored, on disk for review) |

## Findings so far

All three course-37 cuts fail the same way — a **load-path loss**, not a joint breaking. Removing a
whole course severs the wall horizontally, and the masonry above becomes a disconnected island with
no path to the ground, so the load solve releases it. No joint is ever over its capacity.

| cut | removed | released | break decision |
|---|---|---|---|
| back wall | 31 | 718 | ~201 ms |
| front wall | 31 | 718 | ~190 ms |
| both walls | 62 | 1,436 | ~340 ms |

Both walls costs more not because more falls but because the regional-prover LP grows with the
disturbed region — 142 blocks and 453 pivots against 75 and 183 for one wall. The load solve itself
barely moves (it re-runs the whole building either way). The speed notes in
`../WarehouseCourse37/REPORT.md` apply unchanged.
