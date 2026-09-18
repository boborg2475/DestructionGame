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
| `BackWallAndChimneyEndCourse37` | course 37 along the back wall wrapping around the corner onto the chimney-end wall (front wall intact) — an L-shaped cut |

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
| `break_report.txt` | the baseline settle and the cut's settle, pass by pass, per fixpoint iteration, and per prover pose (blocks / pivots / ms / fell) |
| `selection.csv` | the selected pieces as laid |
| `pieces_after_cut.csv` | every piece the instant after the cut |
| `joints.csv` | every joint: ends, normal, area, which pass severed it, which authority severed it (`severedBy`: 0 none, 1 gate, 2 capacity sweep, 3 regional prover), utilisation |
| `fall_summary.csv` | per frame for 8 s, including real ms/frame |
| `fall_pieces.csv` | per released piece every 6th frame |
| `pieces_final.csv` | laid vs final position |
| `headless_timing.txt` | five headless repeats of the decision |
| `mechanism_map_v2.svg` | (L-cut only) an elevation of the run: amber = load-solve-released, white = the cut, teal = sweep-severed joints, red diamonds = prover-severed joints (the certified mechanism surface) |
| `*.png` | the selection and the fall (gitignored, on disk for review) |

## Findings so far

There are **two distinct failure mechanisms**, and which one you get depends on the *shape* of the
cut, not its size.

| cut | removed | released | passes | joints severed | break decision | mechanism |
|---|---|---|---|---|---|---|
| back wall | 31 | 718 | 0 | 0 | ~201 ms | load-path loss |
| front wall | 31 | 718 | 0 | 0 | ~190 ms | load-path loss |
| both walls | 62 | 1,436 | 0 | 0 | ~340 ms | load-path loss |
| back + chimney-end (L) | 42 | 974 | 2 | 51 | ~961 ms | **joint failure** |

**The straight cuts fail by load-path loss.** Removing a whole course severs a wall horizontally,
and the masonry above becomes a disconnected island with no path to the ground, so the load solve
releases it. No joint is ever over its capacity — the wall above does not "break", it is simply no
longer held up. Both-walls costs more than one wall not because more falls but because the
regional-prover LP grows with the disturbed region (142 blocks, 453 pivots against 75 and 183).

**The L-cut fails by joints breaking.** Wrapping the cut around the corner leaves the masonry above
*still connected* — through the corner return and the intact end wall — but no longer able to carry
its load, so the corner is over-stressed rather than orphaned. The capacity sweep severs 41 joints
over two breaking passes, and the regional prover certifies a genuine collapse mechanism, severing 10
more and felling 4 pieces the sweep alone would have left standing. This is the first cut in the
series where the break authority does real work: **51 joints severed, 2 breaking passes, a terminal
third**, versus zero for every straight cut.

**The L-cut is ~5× slower to decide (~961 ms), and almost all of the extra cost is one LP.** The load
solve is flat across every cut (~95 ms/pass, it re-runs the whole building regardless). What explodes
is the pass-2 regional prover: **~600 ms, 813 simplex pivots, 2 poses**. Per-pose profiling
(`RegionalPoseBreakdown`, dumped per pose in `break_report.txt`) now says exactly where that goes,
and it is **one oversized pose, not two large ones**:

| pass-2 pose | blocks | pivots | ms | fell |
|---|---|---|---|---|
| 1 (modest initial flood) | 35 | 196 | 24 | yes |
| 2 (doubled-budget re-flood) | 70 | 617 | **574** | yes |

Pose 1 certifies a fall that touches a cut-artifact grounded boundary, which triggers a re-flood at a
**doubled** budget — and that second pose, at 70 blocks, is 96% of the pass's prover cost. The actual
mechanism only fells 4 pieces / severs 10 joints (a ~14-block collapse), so the 70-block pose is a
~5× overshoot, and the rigid-block LP is strongly super-linear: 35 blocks → 24 ms, 70 blocks → 574 ms
(2× the blocks, ~24× the time). A straight cut never triggers this: reachability has already released
everything, so the prover finds nothing and returns cheap. The clear lever the data now points at is
to **size the re-flood to the moved-set plus a ring instead of blindly doubling** — that alone would
bring pose 2 back toward pose-1 cost and roughly halve the L-cut decision. Carrying the disturbed
region across passes (rather than re-flooding cold each pass) is the larger structural win behind it.
See `../../claude_plans/CURRENT_STATE.md` "SOLVER SPEED at building scale".
