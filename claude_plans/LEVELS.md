# The levels

Twenty-nine playable maps, one per fixture the headless suite measures. You join, the structure is
already framed in front of you, a caption names it and says what to watch for, and four seconds
later it does whatever it was always going to do.

Nothing in any of these maps is authored. Every `.umap` under `Content/Maps/Scenarios/` is a
**duplicate** of `Content/Maps/Lvl_Sandbox.umap` with no actors placed and no edits — the map's
*name* is the only thing that distinguishes it. `ADestructionGameGameMode::BeginPlay` reads that
name, finds the matching catalogue row, lays the structure, spawns the bricks, puts the player where
the whole thing is in frame, and arms the hold.

### Duplicate the asset. Never copy the file.

This distinction cost twenty-eight unopenable levels, so it is worth stating plainly.

A map's `PrimaryAssetId` is `Map:<the inner UWorld object's name>` — **not** its filename. Copying
`Lvl_Sandbox.umap` to `Lvl_Wall01.umap` renames the file and leaves the world object inside it still
called `Lvl_Sandbox`, so every copy claims `Map:/Game/Maps/Lvl_Sandbox`. The asset manager keeps one
and drops the rest, and the editor refuses to open them:

```
LogAssetManager: Warning: Found duplicate PrimaryAssetID Map:/Game/Maps/Lvl_Sandbox,
path /Game/Maps/Lvl_Sandbox.Lvl_Sandbox conflicts with existing path
/Game/Maps/Scenarios/Lvl_CorbelABare4.Lvl_Sandbox.
Two different primary assets can not have the same type and name.
```

**The `-game` loader never consults the asset manager**, so it opens a byte copy without complaint —
`Bringing World /Game/Maps/Scenarios/Lvl_Wall01.Lvl_Sandbox up for play`, note the trailing name. A
headless load is therefore no evidence that a map can be opened, which is exactly how this got as
far as the user.

`DestructionGame.Content.ScenarioMapsAreDistinctAssets` is the standing net: it reads the asset
registry and requires every catalogue map to contain a world named after its own package, and every
`PrimaryAssetId` to be unique.

## How to join one

From the editor, **File → Open Level** and pick it out of `Content/Maps/Scenarios/`, then Play.

Headless or from a shortcut, name the map on the command line:

```bash
"C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\Users\bobby\Documents\Unreal Projects\DestructionGame\DestructionGame.uproject" /Game/Maps/Scenarios/Lvl_CorbelE36 -game
```

Or stay on one map and pick the scenario by name, which is the fast way to walk the whole set:

```bash
"C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\Users\bobby\Documents\Unreal Projects\DestructionGame\DestructionGame.uproject" /Game/Maps/Lvl_Sandbox?Scenario=corbel-e36 -game
```

The option wins over the map name, and the map name wins over the default. A `?Scenario=` naming
something that does not exist falls back to `sandbox` **and says so** — the log names every valid
scenario — so a typo leaves you looking at the default rather than silently at the wrong wall.

## What "hold" means, and why it exists

Every level solves its structure the moment it is built but **holds the release** for four seconds.
Bricks are kinematic until something releases them, so during the hold the structure stands exactly
as laid whatever the solver thinks of it. When the hold expires the level *runs*: it deletes
whatever bricks its row names, and settles.

This is not cosmetic. Before it existed, `corbel-e36` was measured **36 of its 519 pieces into its
collapse before the player's first frame was drawn** — you joined a level whose entire purpose was
to be watched, after the thing worth watching had happened.

The hold is a hold on the *release*, not on the solve. An absent support answer reads as `Falling`,
so a hold implemented by skipping the begin-play solve would make every wall in the game read as
being in free fall.

## The two starting walls

| Level | `?Scenario=` | What it is |
|---|---|---|
| `Lvl_Sandbox` | `sandbox` | The 30 × 40 wall Play has always given you. Cuts nothing. |
| `Lvl_FreeEnd40` | `free-end-40` | Your original bug report: one brick out of the free end of a forty-course wall. It must **not** come down. |

## The corbel family

The structures drawn in [CORBEL_CASES.html](CORBEL_CASES.html) and [CORBEL_CASES_EF.html](CORBEL_CASES_EF.html).
None of them cuts anything — a corbel is condemned by its own geometry, so the whole story is the
settle.

| Level | `?Scenario=` | Pieces | Root joint as laid |
|---|---|---|---|
| `Lvl_CorbelABare4` | `corbel-a-bare-4` | 10 | 0.15613 |
| `Lvl_CorbelBFilled4` | `corbel-b-filled-4` | 18 | 0.19516 |
| `Lvl_CorbelC10` | `corbel-c-10` | 51 | 0.34481 |
| `Lvl_CorbelD10Counterweight` | `corbel-d-10-counterweight` | 90 | 0.34348 |
| `Lvl_CorbelE35` | `corbel-e35` | 496 | 0.99046 |
| `Lvl_CorbelE36` | `corbel-e36` | 519 | 1.01647 |
| `Lvl_CorbelF100` | `corbel-f-100` | 3,015 | 2.68132 |

Three of these are worth standing in front of deliberately:

- **A** is four bricks stepping into open air off a two-cell base, each overhanging the one below by
  more than half its own length. The model reads it at 0.156 of capacity and it stands. That is the
  most questionable verdict in the whole set, and the level is the first place it is *visible*
  rather than a number in a log.
- **C against D** is the counterweight. D is C plus three cells of masonry opposite, and it reads
  0.34348 against C's 0.34481 — the counterweight buys essentially nothing, which is the
  downward-only routing finding, standing in front of you.
- **E35 against E36** is the crossover. One course of difference; E35 stands untouched and E36 sheds
  precisely its lowest step course, 36 bricks, with everything above it still up.

## The twenty acceptance walls

The configurations you reviewed in [WALL_CASES.html](WALL_CASES.html). Each level's caption carries
the verdict the acceptance test asserts.

| Level | `?Scenario=` | Case |
|---|---|---|
| `Lvl_Wall01` | `wall-01` | Intact wall |
| `Lvl_Wall02` | `wall-02` | One brick out, mid-wall |
| `Lvl_Wall03` | `wall-03` | One brick out at the free end |
| `Lvl_Wall04` | `wall-04` | One brick out of the bottom course |
| `Lvl_Wall05` | `wall-05` | Alternate bricks out of one course |
| `Lvl_Wall06` | `wall-06` | Two-brick opening, deep cover |
| `Lvl_Wall07` | `wall-07` | Four-brick opening, eight courses over |
| `Lvl_Wall08` | `wall-08` | Four-brick opening, one course over |
| `Lvl_Wall09` | `wall-09` | Ten-brick opening, eight courses over |
| `Lvl_Wall10` | `wall-10` | Opening at a free end, no abutment |
| `Lvl_Wall11` | `wall-11` | Wall on two piers, six-brick clear span |
| `Lvl_Wall12` | `wall-12` | The same span on one-brick piers |
| `Lvl_Wall13` | `wall-13` | Corbel, quarter brick per course |
| `Lvl_Wall14` | `wall-14` | Corbel, half brick per course |
| `Lvl_Wall15` | `wall-15` | Header out half a brick, six courses on top |
| `Lvl_Wall16` | `wall-16` | The same header at the top, nothing on it |
| `Lvl_Wall17` | `wall-17` | Stack bond, intact |
| `Lvl_Wall18` | `wall-18` | Stack bond, one brick out |
| `Lvl_Wall19` | `wall-19` | Bottom course out under half the wall |
| `Lvl_Wall20` | `wall-20` | Staircase void |

### Six of these levels contradict the model, and say so

Cases **8, 9, 10, 12, 19 and 20** are the rows currently failing `Acceptance.Wall.Catalogue`. Their
captions carry the words **THE MODEL CURRENTLY DISAGREES** alongside the expected verdict, because a
level captioned with a verdict the solver does not produce would be a lie told to somebody standing
in front of the counter-example.

That marker is not a hand-maintained list. `Acceptance.Wall.EveryLevelsCaptionTellsTheTruth`
*computes* which rows the model gets wrong by running each case, and requires the marker on exactly
those. When the underlying defect is fixed, that test goes red until the captions stop claiming a
disagreement that no longer exists.

All six trace to one finding: **load routes only downward**. Masonry beside or behind a joint never
reaches it, so a counterweight buys nothing and load funnels onto whatever column sits underneath.
`Core.Structure.CorbelStepsBeforeTensionWins` is the standing red for it.

## Adding a level

1. A row in `DestructionScenarios::Catalogue()` in `World/DestructionScenarios.cpp` — name, map
   name, title, expectation, the wall spec or a `LayStructure` builder, the brick centres it cuts,
   and how long it holds.
2. A **duplicate** of `Content/Maps/Lvl_Sandbox.umap` into `Content/Maps/Scenarios/`, named to
   match — either **right-click → Duplicate** in the content browser, or:

```bash
powershell -NoProfile -File Scripts/New-ScenarioMap.ps1 -MapName Lvl_MyNewLevel
```

Two tests fail if you do the first without the second. `Content.ScenarioMapsExist` derives the path
from the row rather than from a list, so it catches a missing map rather than needing to be told
about it; `Content.ScenarioMapsAreDistinctAssets` catches the one made by copying the file.

## Seeing them all without joining them

`DestructionGame.Visual.ScenarioLevelScreenshots` opens all twenty-nine as a player opens them and
photographs each held and run — 58 frames into `Saved/Screenshots/WindowsEditor/`. It needs a real
RHI, so **`-nullrhi` must be absent**:

```bash
"C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe" "C:\Users\bobby\Documents\Unreal Projects\DestructionGame\DestructionGame.uproject" /Game/Maps/Lvl_Sandbox -game -windowed -ResX=1920 -ResY=1080 -ForceRes -RenderOffScreen -nosplash -NoSound -unattended -nopause -ExecCmds="Automation RunTests DestructionGame.Visual.ScenarioLevelScreenshots" -TestExit="Automation Test Queue Empty"
```

**Under `-nullrhi` the test is FILTERED OUT, not run.** It carries
`EAutomationTestFlags::NonNullRHI`, so the ordinary headless suite never mentions it exists — which
is the point of the flag, and is what stops it going falsely green. (Without the flag it *would* be
the false green: `FApp::CanEverRender()` is false under `-nullrhi`, `UGameEngine::Init` only builds a
window and a viewport when that is true, so there would be nothing to photograph and nothing to
fail.) A run that neither errors nor writes a file is therefore the **filter**, and the log says so —
look for the test not being listed at all rather than for a pass.

**Omit `-game -RenderOffScreen` and it HANGS rather than failing.** Observed 2026-08-08: launched
without them the test started, wrote nothing for seven minutes, produced zero frames and had to be
killed. There is no timeout that turns this into an error, so the failure mode gives no clue what is
wrong. Use the invocation above verbatim.
