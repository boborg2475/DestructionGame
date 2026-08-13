# Traps — the live footguns, and the mutation registry

Everything here has been paid for at least once. Each entry is **still live**: the underlying hazard exists in the tools, the engine, or the codebase's own conventions, and forgetting it re-arms it. Physics-model hazards live with their mechanisms in [DESIGN.md](DESIGN.md); this file is the operational layer — build, engine, tooling, and test practice.

The recurring enemy in this project is **a wrong answer that looks plausible**: nothing crashes, nothing moves, every readout agrees, and the wall stands there being wrong. Most entries below are instances of that shape.

---

## Running the tests

- **Automation runs exit 0 even when tests fail, and results never reach stdout.** Read `Saved/Logs/DestructionGame.log` and grep `Test Completed` / `LogAutomationController: Error` (commands in [CLAUDE.md](../CLAUDE.md)). Every run also reports `6382 tests available` — the engine-wide list; **the count of `Test Completed` lines is the only guard** that the filter ran what you meant.
- **Any test flagged `EAutomationTestFlags::NonNullRHI` is invisible to the documented `-nullrhi` command** — filtered out, not skipped-with-a-note. One visual test rotted through five slices this way and produced twelve errors when finally run. Run the visual set deliberately in any verification claiming completeness:

  ```
  UnrealEditor-Cmd.exe <project>\DestructionGame.uproject /Game/Maps/Lvl_Sandbox
    -game -windowed -ResX=1920 -ResY=1080 -ForceRes -RenderOffScreen
    -nosplash -NoSound -unattended -nopause -log -abslog=<unique path>
    -ExecCmds="Automation RunTests DestructionGame.Visual"
    -TestExit="Automation Test Queue Empty"
  ```

  Under `-nullrhi` a `NonNullRHI` test is not listed at all — look for its absence, not for a pass. **Omit `-game -RenderOffScreen` and the screenshot test HANGS with no timeout** rather than failing (observed: seven silent minutes, zero frames).
- **A `+`-joined filter over two `DestructionGame.*` paths produces WRONG ANSWERS, not merely a wrong set** — measured: fabricated failures from functions that cannot return the values reported, while each filter alone and the full suite are green. `+` is safe only for joining distinct suites (`DestructionGame+Project.Functional Tests`). **Narrow with a single lengthened path**, and confirm anything surprising against the full run before believing it.
- **Functional tests (none exist yet)** register under `Project.Functional Tests.*`, not `DestructionGame.*`, discovery is an asset-registry tag written **when the map is saved in the editor** (a new C++ class alone registers nothing), and the cook gate's ini section must stay spelled `[/Script/UnrealEd.ProjectPackagingSettings]` — the legacy name is pinned by `OverrideConfigSection`, and "correcting" it to `DeveloperToolSettings` silently disables the whole section. `DirectoriesToNeverCook` takes a long package path and is applied only in by-the-book cooks. CLAUDE.md needs its two documented edits the day the first functional test lands.

## Builds

- **The unity-build anonymous-namespace landmine.** A unity blob is one translation unit, so every anonymous namespace in it is the *same* namespace, and adaptive unity keys the blob membership off `git status` — a collision appears when you commit and disappears when you touch the file. It has fired from **inside engine source** (`constexpr double F` in a test file colliding with locals in `Chaos/Utilities.h`, four C4459 errors naming the wrong culprit) and between production files (`BedJointCosine`, `GravityCmPerSecondSquared`). Rules: test files use named `*TestSupport` namespaces with `using namespace` **inside** each `RunTest` body; production file-locals carry a file prefix (`Solver…`, `Presenter…`, `GameMode…`); and verify with the blob build, because a normal green build proves nothing here:

  ```
  Build.bat DestructionGameEditor Win64 Development -Project=… -WaitMutex -DisableAdaptiveUnity -StressTestUnity
  ```

  The trigger is *adding an unrelated file*, so it can fire on anyone at any time. A `-Check` script over `Tests/` ("no anonymous namespace, no file-scope declaration") is wanted and unwritten — see CURRENT_STATE housekeeping.
- **"Target is up to date" can be a lie after a copy-restore.** UBT rebuilds by timestamp; `Copy-Item` preserves `LastWriteTime`, so restoring a mutated file can leave the correct source *older* than the object built from the mutation — the build links yesterday's arithmetic and every test reports pre-feature values while `git diff` shows the feature intact. Touch restored files and confirm the link ran. This matters here because **mutation testing is routine**, so half-restored builds are a routine hazard.
- **Reverting a mutation**: `git checkout -- <path>` only if HEAD holds what you want — it destroys uncommitted work with no reflog to recover from (this happened; `RemovePiece` and friends were rebuilt from context). For uncommitted files: copy aside first, restore by copying back, **then touch**.

## Engine and content

- **`SM_Cube`'s pivot is a corner, not its centre** (local bounds (0,0,0)–(100,100,100)). Placing an actor at a box's centre puts the brick a half-size out on all three axes while `GetActorLocation()` agrees with the layout perfectly. Derive scale as `DesiredSizeCm / MeshLocalSizeCm` from `GetBoundingBox()` — never `/ 100.0` — subtract the scaled local-bounds centre, and **assert on `GetComponentsBoundingBox()`, never `GetActorLocation()`** (bounds are pivot-agnostic *and* catch a 100× scale error in the same comparison).
- **Nanite silently ignores `SetOverlayMaterial`** — the Nanite scene proxy has no overlay member; no warning, no log line. Anything the game highlights with an overlay must be a non-Nanite mesh (`SM_Cube` now is; `Content.BrickMeshCanDrawTheHighlightOverlay` asserts both the authoring flag and the runtime reading). ISM/HISM **can** draw overlays — the ISM hazard is different: an overlay material without `bUsedWithInstancedStaticMeshes` silently falls back to the default material. The highlight was covered at three levels and all three were green while the player saw nothing, because the layer underneath was never exercised; the remaining hole of the same shape is **shader compilation** (a material whose shader failed to compile has a perfect node graph and renders as the checkerboard).
- **A scenario map must be DUPLICATED, never file-copied** — a map's `PrimaryAssetId` is the inner `UWorld` object's name, not the filename; a byte copy leaves every copy claiming the original's id and **the editor refuses to open any of them**, while the `-game` loader (which never consults the asset manager) plays them all happily. Twenty-eight levels shipped broken this way. **A `-game` load is not evidence a map can be opened**: content claims must ask the asset registry. `Scripts/New-ScenarioMap.ps1` is the supported route; `Content.ScenarioMapsAreDistinctAssets` is the net. (Full story in [LEVELS.md](LEVELS.md).)
- **`NewObject<UObject>` trips an engine ensure** (abstract class), which counts as a failure of whatever test is running. Use a concrete type: `NewObject<UStaticMeshComponent>(GetTransientPackage())`.
- **`TArray::Add(Array[0])` asserts and kills the whole automation run** (aliasing its own storage across a possible reallocation). Copy to a local first; same for `Emplace`/`Insert`/`Push`.
- **`TestTrue(FString::Printf(..., SideEffect()), Condition)` is unsequenced** — MSVC evaluates the condition first, so a state-changing call folded into the message reads the flag before setting it. Make the side effect its own statement.
- **Headless Python asset work: a surprising read-back is suspect before the asset is.** `get_editor_property` returns a *plausible empty value* for a UPROPERTY that moved between engine versions — `UInputMappingContext::Mappings` is deprecated and reads back empty (live array: `default_key_mappings.mappings`); `UMaterial` `expressions` reads 0 for a full graph (moved to editor-only data); editor subsystems are `None` under `-run=pythonscript`. Cross-check from C++ before acting. Also: `unreal.Key` rejects positional args, and `save_loaded_asset` defaults to only-if-dirty while `MapKey` does not dirty — pass `False` and call `modify()`.
- **`FBrickTestWorld::Begin` names the game mode; the default is Epic's plain `AGameModeBase`.** Without that, every world test silently runs the real game mode and would spawn a 1,220-brick wall each. Do not undo it. The shared `SpawnFloor` subtracts the scaled bounds centre (it was half-off for a long time with nothing noticing); leave it.
- **A backslash at the end of a `//` comment line is a real C++ line continuation** — the next physical line is spliced into the comment. Harmless while the next line is also a comment; a code line after it silently vanishes. This is why `Convert-CommentBlocks.ps1` refuses `StructureCascadeTest.cpp` (exit 2) rather than guessing — the refusal is the warning.

## Fail-closed conventions in the arithmetic

- **NaN is laundered by `FMath::Min`/`Max`** (`(A < B) ? A : B` — every NaN comparison is false), so a NaN can become a plausible capacity. Guards are spelled **`!(x > 0.0)`**, never `x <= 0.0`, so NaN lands *inside* the guard; degenerate inputs return `TNumericLimits<double>::Max()`, never zero. The NaN guards and the shear cap are **coupled** — the cap would launder a NaN into a believable number if the compression-axis `IsFinite` guard were dropped. The break comparison is written `!(x <= 1.0)`; don't "simplify" it back.
- **Fail-closed for a mass is NaN, never zero** — `AddPiece` deliberately accepts zero ("a massless piece is meaningful"), so a zero from a degenerate box would be laundered into a real piece that weighs nothing and never breaks anything. NaN is refused by the existing guard; no second guard exists anywhere.
- **A value that is safe as an ANSWER is unsafe as a COMMAND — fail-closed inverts polarity at every seam where a diagnostic becomes an action.** `EPieceSupport::Falling` is the zero enumerator so an absent answer promises least (right for a readout) — and one layer up, `Falling` triggers irreversible release (before the guard existed, an unsolved structure released *entire*, foundation included, permanently). The house pattern: anything irreversible first asks whether there is an answer at all (`FStructure::HasSupportAnswer`, per handle — empty array and short array are separate ways to be outside the extent). Look for this trap wherever a support state or utilisation crosses out of the world-free layer.
- **The degenerate-normal caller obligation** (`ClassifyForce` + `ComputeUtilisation` composed naively read a joint with no interface plane as healthy) is stated in DESIGN §2 — it applies to any new direct caller.
- **`AddConnection`'s normal guard has a known top-end hole** (components ~1e200 → `Normalize` returns true having produced the zero vector, classified as a head joint). Unreachable from real content; fixing it is a **lockstep change** with the fuzz oracle's deliberate transcription of the same normalisation — one ulp of drift is five spurious cascade-fuzz failures. Both sites carry the instruction.
- **An overlap is an interval intersection, `min(highs) − max(lows)` — never `reach − distance`.** The two agree for equal-sized and partially-overlapping boxes and diverge silently the moment one span *contains* the other, with the area and the rectangle wrong **together**, so a rectangle-times-extents consistency check passes on the wrong pair. This shipped in `MakeInterface` (fixed 2026-08-08, held by `Layout.Interface`'s contained-pier rows) and a copy still lives, latent, in `LayoutTest.cpp`'s `ContactsOf` oracle — see CURRENT_STATE, Layout producer.

## Test practice

- **A green-on-arrival test is indistinguishable from one that asserts nothing until production is mutated.** Every fuzz property and every green-on-arrival row is proven to bite by a recorded mutation before it is trusted. Chained reds (refusal rows written against a stub) prove nothing until the happy path exists — mutate the door guard instead.
- **An invariant asserted over fixtures that all share a hidden property is not an invariant.** When an invariant has never failed, check whether it *can*.
- **Fixed simulated seconds, never a settle-poll**; group world tests by world configuration; the integration entry rule is in DESIGN §4.
- **Reviews**: transcribe-and-fuzz found what reading did not; scale review depth to topological code with silent failure modes, not to tables of constants; never re-verify findings already closed.

### The mutation registry — reuse these, never invent from scratch

Run against otherwise-unmutated production; expected signatures below. **(b) hangs rather than fails — run under a timeout.** (g)/(h) are the only mutations that watch an over-eager break; every other property inherits production's break set as its premise, so do not use them to demonstrate anything else.

| # | Mutation | Signature |
|---|---|---|
| — | `Structure.cpp`: drop the falling-support filter from the split | `Structure.Fuzz` 4,604 failures (seed 20260754) |
| — | strand on un-orderability instead of `LoadReturnsToPiece` | `Structure.Fuzz` 4,025 (seed 20260742); `PieceSupportReason` 6 failures, the two downward-stranding rows are the only tests naming the fault |
| (a) | `HasGiven` filter moved out of the tier decision | CascadeFuzz 13,411; the cheap one, and proves the two fuzzes are not duplicates (`Structure.Fuzz` stays green) |
| (b) | delete the already-given skip in the break sweep | **HANG** — no result at all |
| (c) | cascade stops after the first breaking pass | 4,294 |
| (d) | count the final non-breaking pass | 17,031 |
| (e) | delete the `HasGiven` skip from `SolveLoads` entirely | 13,506 |
| (f) | never write the break-pass stamp | 14,651 |
| (g) | joints give at `!(u <= 0.1)` | 2,280 — **BROKE UNDER CAPACITY and nothing else** |
| (h) | scale force ×1e6 from pass 2 onward | 1,537 — later-pass sweep only; pass 1 bit-correct |
| (i)/(j) | test-side: rebuild `SurvivingGraphOf` at `Pass ∓ 1` | (i) all 706 later-pass breaks fail; (j) floors fire — the only check that the rebuild targets the right pass |
| (k) | `UtilisationUnder` returns 0 when given | exactly the 4 given-joint rows |
| (m) | recompute + bump one ulp | 11 failures — one ulp is the whole margin (a *faithful* recompute is invisible on unit normals; the identity guards drift, not duplication) |
| (n)/(o) | `AdoptLayout`: reversed actor array / boxes off-by-one | 10 failures each, actor-identity row / box row respectively; counts survive both |
| — | release-everything (delete the `IsReleased` skip in the push walk) | 5 tests, 85 assertions; 18 are the redistribution control — **the only thing stopping "release everything" passing the collapse tests** |
| (m1) | `ShowPieceMenu`: delete the leading dismiss | 13 failures, rows stack to 9 |
| (m3) | present only on hit (`if (Rows.Num() > 0)`) | exactly the 2 miss rows |
| (m6) | clamp instead of refuse in `ChoosePieceMenuRow` | 48 failures — every out-of-range choice commits row 0 |
| (m7) | solve per piece instead of once-last in `RunPieceActions` | 6 failures, **all solve counts, no outcome moves** — invisible without `NumSolves` |
| (m8) | menu folds with `\|\|` (union) | 6 failures, all the `{live, RELEASED}` rows |
| (q) | force `Hit.Ref.PieceIndex = 0` on any hit | 6 failures in `World.Inspect` — the wall-right-refs-wrong defect only a per-brick trace sees |
| (r) | game mode builds but skips `SolveAndPush` | 10 failures — an unsolved wall and free fall are the same answer |
| — | commit door: `Ref.PieceIndex` instead of `ResolvePiece` | 43 failures in `CommitRefFailsClosed`; menu door variant: 6 in `MenuOffersWhatCanRun` — the two structure-id rows are the only watch on resolve-not-trust |
| — | `CommitPieceAction`: no-op the `Orphan->Destroy()` | exactly 1 assertion — nothing else can see an orphaned collider |
| — | one-cell thrust gate, withhold everywhere (`HasArchingAbutment` returns false) | `AOneCellArchMustEarnItsThrust` lime row fails at ~1.29 (10 joints, 5 passes) — the anti-over-withhold half |
| — | one-cell thrust gate, grant everywhere (restore the pre-gate `HasArchingAbutment` / force it true) | same test, dry-stone row: springings read 0.00473 instead of over-capacity, both bricks Supported instead of Falling — failing lines `StructureOneCellThrustTest.cpp:600/683/703`. Also `Acceptance.Beam.Catalogue` 5→8 assertion failures: all six `DropsToday`/`PassesToday` pins fire (expected 3 got 0 / expected 1 got 0), the STANDS violations flip to pass, case 1 gains "broken beam takes itself down" |
| — | `ReseatSpannedGroups` early-return (spell `\|\| Pieces.Num() > INT32_MIN` — bare return trips C4702) | coarse (removes a routing mechanism): 36 errors across wall cases 7–12/19/20; the sharp half: case 8 drops exactly the two bricks the retired LOCAL LOSS named ({c4/5, c4/6}) — production's case-8 STANDS is spanned-group re-seating, proven |
| (M1) | LP oracle: delete the moment-balance rows | 37 failures across 6 tests (re-measured 2026-08-12; the fast sweeps and the twin row now watch it too) |
| (M2) | LP oracle: delete the friction rows | 8 (re-measured 2026-08-12; beam/corbel-B windows joined the original two sliding rows) |
| (M3) | LP oracle: unlimited tension | 13 (re-measured 2026-08-12) — mortared λ* ~3.3× high; dry rows stay green *correctly* (associative friction at c = 0 already implies no-tension) |
| (M4) | LP oracle: conversion open-coded as 100 | fast 36 / slow sweep 8 (re-measured 2026-08-12, superseding the reconstructed ~15 doubt) — every strength-governed λ* exactly 100× low |
| (M5) | LP oracle: delete the λ-cap row | exactly 1 — the unbreakable row (phase-2 unbounded → fail closed); unchanged under the sparse solver |
| (M6) | LP oracle: gut input validation | 63 — 21 poison rows × 3 assertions; catalogue untouched; unchanged under the sparse solver |
| (M7) | LP oracle: bridge ignores the latch | exactly 1 — the after-removal jamming row; unchanged under the sparse solver |
| (S2) | `BreakOverturnedBodies` early-return (spell as `if (Pass > INT32_MIN) { return false; }` — a bare `return false;` trips C4702) | exactly the Sweep.LeaningStack test, 4 assertions incl. both production drop-count pins (29/39 → 0) |
| (S3) | oracle verification tolerance 1e-6 → 1e-3 | **zero failures** — when the dense solver existed its envelope refusals were decisive at 1e-3 too; under the sparse solver every answer clears 1e-6 |
| (S3b) | ~~verification off (tolerance 1e30)~~ | **RETIRED 2026-08-12** with the canary's promotion — its "exactly 1" signature (the canary catching an uncertified λ* 257.24) can no longer occur; the verification gate's bite is now proven by S6-family mutations below |
| (S4) | test-side: flip the one-cell disagreement classification | exactly 1 — the pinned-relation row |
| (S6) | sparse solver: delete the iterative-refinement pass at refactorisation | 2 — the two λ* = 0 fixtures refuse via "the optimal basis failed verification" |
| (S5+S6) | pivot-out first-past-tolerance AND refinement deleted | 3 — all three λ* = 0 fixtures refuse via verification; the gate's bite-prover (S5 alone measures 0 — refinement absorbs it) |
| (S8) | test-side: perturb one half-area twin's area ×(1+1e-9) | exactly 1 — the cross-row SAME-NUMBER equality (1.241110018676219 vs …92967741); the old 1e-6 closed-form window passes this mutation silently, which is why the cross-row check exists |
| (M4″) | M4 against the classified sweep (2026-08-12) | slow sweep **26** (19 λ windows + 7 relation flips: corbel C/D, wall-09/10/12/19/20 fall below 1.0 — reconstructs exactly from λ*/100); fast **37** (16 sweep + 21 oracle test; the registry's earlier 36 was one low — correction, not caused by the slice) |
| (N1) | test-side: flip a slow row's pinned relation | exactly 1 — the pinned-relation line |
| (N2) | test-side: perturb the free-end 7×20 fixture's density ×(1+1e-9) | exactly 1 — the SAME-NUMBER λ identity (relative 1e-9); both λ windows and the reading ratio pass silently — the S8 lesson generalised |
| (N3) | test-side: the 7×20 row builds 10 courses (the pair quietly becomes one fixture) | 4 — relation, drop pin, reading ratio (reads 1.0), crossing; the λ identity PASSES, which is why a same-number pin always needs a stay-apart pin beside it |

**A mutation whose early `return` precedes code trips C4702 and the BUILD FAILS while the old DLL keeps running** — the run then reproduces the *previous* mutation's signature verbatim. Verify `Result: Succeeded` on every mutation build before believing its run; this has now bitten twice (the M6-stale incident, and once during the sparse rewrite).

Degenerate-fixture note: a row meaning to hit the harness bounds bug must name the **ungrounded** piece first (`{1, INDEX_NONE}`) — `||` short-circuits on a grounded piece 0 and the second handle is never evaluated.
