# The warehouse course-37 experiment

Deleting one course across a long wall of `Lvl_Warehouse`, and watching what falls and how long
the decision took. Recorded 2026-09-18. Everything cited here is a file in this folder; the run is
reproducible from the two automation tests named at the end.

## What was done

1. Joined `Lvl_Warehouse` exactly as a player does — the map name selects the `warehouse` catalogue
   row, the game mode lays the 5,612-piece layout from `Content/Layouts/Warehouse.json`, stands it
   up, and holds it as laid for four seconds.
2. Selected every piece of **course 37 of the back long wall** by a real primary click along a ray,
   one click per piece, through the same controller door the mouse uses. 31 pieces, all selected,
   all highlighted. This is the upper-window **sill course**: the wall's wythe, its five stone
   sills, and the six pilaster bricks at that height. See `selection.csv` and
   `Warehouse_C37_Before_NoUI.png`.
3. Let the four-second hold expire so the level ran its own settle on the **intact** building — the
   baseline. It broke nothing (`break_report.txt`, first block).
4. Deleted the 31 selected pieces through the player's own menu **Delete** row, which commits the
   whole selection in one batch: 31 removals, one settle behind them, the orphaned meshes destroyed,
   one push to physics. A wall-clock timer wrapped the commit.
5. Tracked every released brick frame by frame for eight seconds and photographed the fall at
   0, 0.25, 0.5, 1, 2, 3, 5 and 8 seconds, plus a final resting frame.

## What fell

**718 pieces were released.** Every one of them was above the cut, and all 718 came to rest more
than 5 cm from where they were laid, so nothing that was released jammed in place.

| band | released |
|---|---|
| upper back wall between the cut and the eaves (Z 284–479) | 717 |
| one gable/cornice piece above the eaves | 1 |
| **by material** | |
| clay brick | 697 |
| stone (sills, string, cornice) | 15 |
| timber (roof boards bearing on the back wall) | 6 |

The rest of the building stood: 57 grounded pieces and 4,806 still supported. The front wall, both
end walls, both chimneys and the front half of the roof are untouched. The back wall above the cut
lost its only path to the ground and came down, taking the roof boards that bore on it and one
gable piece with it. Frames `Warehouse_C37_After_*.png` show the sequence; by 3 seconds it has
essentially finished (26 pieces still moving), and by 5 seconds it is at rest.

## How long the decision took, and where the time went

The break decision is `FStructure::SolveAndBreak`, run once inside the batch commit. Measured on
this machine, five fresh headless repeats (`headless_timing.txt`) and the live run agree:

| stage | time |
|---|---|
| whole commit (remove 31 + decide + destroy 31 meshes + push) | ~222 ms |
| **the decision, `SolveAndBreak`** | **~201 ms** |
| — the load solve (`SolveLoads`), run 3 times | ~148 ms |
| — the regional collapse prover, one LP | ~51 ms |
| — the equilibrium gate (declined instantly) | 0.08 ms |
| — the per-joint capacity sweep | 1.2 ms |

The decision is one pass. It broke **zero joints**. All 718 pieces were released not because a joint
was over its capacity, but because the load solve's reachability walk could no longer find a path
from them down to the ground — removing the whole course severed the back wall horizontally, and
everything above it is a disconnected island hanging off nothing. This is the honest answer: pull
out a whole course and the masonry above does not "break", it is simply no longer held up.

The three load-solve iterations (`break_report.txt`) show the settle converging: iteration 1 drops
the reachable count from 5,167 to releasing 10 arch members, iteration 2 overturns one piece,
iteration 3 changes nothing and is the answer.

## Why it took that long

- **148 ms of it is the load solve, and it runs the whole structure three times from scratch.** Each
  iteration re-initialises the force, support and reachability arrays over all 5,581 live pieces and
  ~15,000 joints, rebuilds the per-piece joint adjacency, and walks reachability from every grounded
  piece. One iteration is ~49 ms; the fixpoint needs three because stranding one piece changes what
  the next iteration can reach. Almost none of the structure changed — only the neighbourhood of the
  cut — but the solve has no notion of "only re-examine what moved", so it pays for all 5,581 pieces
  three times.
- **51 ms is one regional-prover LP that certified nothing.** Because the building (5,581 pieces) is
  far above the equilibrium gate's 200-block cap, the gate declines and the router takes over. The
  router's regional prover then floods a 75-block neighbourhood of the cut and poses a rigid-block
  feasibility LP (183 simplex pivots) to look for a *global* collapse mechanism the per-joint sweep
  would miss. Here it found none — the reachability solve had already released everything that falls
  — so the 51 ms produced no verdict.
- The remaining ~21 ms of the commit is destroying 31 brick actors and pushing the answer onto the
  world; minor.

For contrast, the same building's **as-laid** settle (nothing cut) is ~65 ms: one load solve, no
prover pass worth the name. The cut roughly triples the cost because it forces the solve to iterate
three times and triggers the prover flood.

## How it could be made faster

Ranked by likely payoff:

1. **Re-solve only the disturbed region, not the whole building.** The 148 ms load solve is the
   target. Removing 31 pieces can only change support in their neighbourhood; the other ~5,500 pieces
   are provably unaffected. A solve that seeds from the removed pieces and walks outward until the
   support state stops changing would replace three 49 ms whole-structure passes with one small local
   one. This is the single biggest win and the same idea the regional prover already uses for its own
   flood.
2. **Skip the prover pass when reachability already released the neighbourhood.** The 51 ms LP looked
   for a mechanism among pieces that the load solve had *already* marked Falling. The prover only ever
   adds Falling pieces; if every piece in its seed neighbourhood is already Falling, it cannot change
   the answer and the pose can be skipped. That reclaims most of the 51 ms on cuts like this one,
   where the failure is a clean load-path loss rather than a subtle standing-vs-toppling mechanism.
3. **Kill the per-solve allocation churn.** `SolveLoads` rebuilds a `TArray<TArray<int32>>` of every
   piece's joints on every call — about 3,600 small heap allocations at this scale, noted in the code
   itself as replaceable by a count-then-fill flat layout. It is a constant-factor win under the whole
   148 ms, worth taking once the structural re-solve above is in.

None of these change the verdict — they change how long it takes to reach it. The verdict here (718
pieces lost the ground, nothing over-stressed) is a property of the geometry, not of the solver's
speed.

## The 3.2-second stall was the harness, not the physics

The first two runs of this experiment (discarded) showed a ~3.2 s pause on the frame the bricks were
released, which looked like Chaos choking on 718 new rigid bodies. It was not: it was this harness
writing 20,000 lines of per-joint and per-piece CSV one line at a time, reopening the file each time,
on that frame. Buffering the dumps to memory and writing them once, after the fall, removed it
entirely. The real cost of releasing 718 bodies is the frame-2 physics step, ~318 ms
(`fall_summary.csv`, `realMsSinceLastFrame`), after which frames settle to 12–23 ms. Worth
remembering: a "physics stall" in a headless capture is often the capture.

## Files

| file | what |
|---|---|
| `selection.csv` | the 31 selected pieces: material, mass, box, joint count, support as laid |
| `break_report.txt` | the baseline settle and the cut's settle, pass by pass, with per-fixpoint-iteration counts |
| `pieces_after_cut.csv` | every piece the instant after the cut: support state, released, position |
| `joints.csv` | every joint: ends, normal, area, which pass severed it, utilisation |
| `fall_summary.csv` | per frame for 8 s: released, moving, max/mean drop, max displacement, real ms/frame |
| `fall_pieces.csv` | per released piece every 6th frame: position and drop |
| `pieces_final.csv` | every piece's laid vs final position and displacement |
| `headless_timing.txt` | five fresh headless repeats of the decision, for a timing spread |
| `Warehouse_C37_Before*.png` | the selection, with and without the menu UI |
| `Warehouse_C37_After_*.png` | the fall at 0, 0.25, 0.5, 1, 2, 3, 5, 8 s and at rest |
| `engine*.log` | the raw engine logs of the live and headless runs |

## Reproduce

The break-decision instrumentation (`FStructure::GetLastSolveAndBreakReport`) is production code,
covered by `DestructionGame.Core.Structure.SolveAndBreakReportMatchesTheCascade`. The experiment
itself is two automation tests, both needing a real RHI for the live one:

```
# the live capture (no -nullrhi):
UnrealEditor-Cmd DestructionGame.uproject /Game/Maps/Lvl_Sandbox -game -RenderOffScreen
  -ResX=1920 -ResY=1080 -ForceRes -nosplash -NoSound -unattended -nopause -log
  -ExecCmds="Automation RunTests Experiment.WarehouseCourse37.Collapse"

# the timing spread (headless):
UnrealEditor-Cmd DestructionGame.uproject -nullrhi -unattended -nopause -nosplash -NoSound -log
  -ExecCmds="Automation RunTests Experiment.WarehouseCourse37.HeadlessTiming"
```
