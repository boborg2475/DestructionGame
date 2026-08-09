# Current State

**Living document.** This is the running TODO list for the project, not a historical record.

- **Finishing something? Remove it from this file** as part of that work. A completed item left here is worse than no list.
- **Deferring something? Add it here** before moving on, with enough context to act on it later.
- Check this file at the start of a work session and again before calling anything done.
- Items are deliberately unnumbered where possible — they churn constantly.

**This file assumes you already know the design.** [DESIGN.md](DESIGN.md) is the authority on the model, the constants, the anchors and the evolution path; [TRAPS.md](TRAPS.md) holds the live footguns; [LEVELS.md](LEVELS.md) indexes the twenty-nine playable levels. Settled reasoning lives there and in git — not here.

Last updated: **2026-08-09** (review-queue item 1 — case 12 rewritten and synced to the production scenario catalogue).

## Where the suite stands

**161 tests, 154 green, 7 deliberate reds.** Do not "fix" a deliberate red by weakening its assertion. The reds and what each anchors:

| Red | What it anchors |
|---|---|
| `Acceptance.Wall.Catalogue` — 5 rows: 8, 9, 10, 19, 20 | The model's known-wrong verdicts (downward-only routing / missing stability check — DESIGN §7). The five levels' captions carry the computed `THE MODEL CURRENTLY DISAGREES` marker. Each red row also pins today's behaviour (`DropsToday`/`StrandsToday` in `WallAcceptanceTest.cpp`), so a regression *inside* a known failure fails loudly instead of hiding behind the expected red. Case 12 came out of this set on 2026-08-09: rewritten to case 11's span on a one-cell pier, the honest pier arithmetic (and the model) both read STANDS, so the exit is an expectation moving, not a solver fix. |
| `Acceptance.Wall.MatchedPairs` — 7 vs 8, 7 vs 9 | Cover-depth and span discrimination over the openings |
| `Acceptance.Wall.StackBondColumnShearIsHeightIndependent` | The model routes a hanging column's whole load to its foot (height-linear, n/2 × 0.0200165); same defect family as the spanned-group head-joint limit. Never changes a verdict below ~106 courses; needs the "load sheds as it rises" design pass before code (see below). |
| `Core.Structure.CorbelStepsBeforeTensionWins` | A **finding**, not a wrong expectation: the counterweight buys a corbel nothing (C and D cross at 36 steps identically) because masonry behind a joint never reaches it. Goes green at evolution step 5 (tension support). |
| `Acceptance.Beam.*` — 3 rows | Member failure: midspan reads \|M\| = 0 under 3.48× C24 bending capacity. Red until evolution steps 5–6; row 1's outcome half also needs global equilibrium. |

## What to do next — ranked

### Decide before building

- **What a selection MEANS when a cascade releases a picked brick** — a product decision with both readings written out (see "piece menu presenter" below). Latent only because nothing calls `SolveAndBreak` in a world yet; the cascade-on-the-wire item makes it reachable. **Decide before that lands.**

### Then build, in this order

1. **The cascade on the world wire.** `SolveAndPush` solves but never breaks; `FStructureBinding::SolveAndBreak` was deliberately **removed, not stubbed** (a stub returning 0 is silent fail-open) and must be re-added with a red test pinning what the binding owes on top of the forward: release what the cascade strands, and return the per-call pass count. The game currently does less than the design says — a joint loaded past capacity never gives in a running game. Also unblocks the deliberately-missing strength-driven integration test (needs a fixture that can actually overload a joint — a mortared wall sits at 0.005 of capacity under gravity). When built, the presenter's "broken (went with a removed piece)" wording becomes a lie — write its failing test in the same slice.
2. **The 2026-08-08 review queue**, in its approved order — next section.
3. **The per-brick joint-force breakout: a human presses Play.** Built end to end and green; the suite proves the model and nothing about the panel (every run is `-nullrhi`, and this project has twice shipped a green suite over something invisible). The check list when someone does: panel legibility (engine-default text, no scroll — a large selection runs off screen), magenta-vs-cyan strength at playing distance, hover routing on the opening frame, whether an entry click is swallowed, the odd reading order (Delete above the readout — accepted for the no-moving-buttons safety property, wants human eyes).
4. **Scenario system — the switching half.** Restart-current, a tiny lightweight default so the game opens fast (today's default is the 30 × 40 wall, ~190 ms of begin-play), and in-game scenario change (the in-world menu of DESIGN §9). Rows are data; a scenario needing a class is the drift to stop.

## The 2026-08-08 review queue — approved by the user, item 1 built

In sequence (rationale in DESIGN §7). Item 1 (rewrite case 12) landed 2026-08-09: the test rewrite and the production catalogue sync (`ScenariosWall12Cuts`, the `wall-12` title and caption in `DestructionScenarios.cpp`) are both in; `Acceptance.Wall.EveryCaseIsAPlayableLevel` and `Acceptance.Wall.EveryLevelsCaptionTellsTheTruth` are green on it, and case 12 has come off the `Acceptance.Wall.Catalogue` known-red list.

1. **Leaning-stack acceptance case + interim overturning guard**: a column offset 2 cm/course at 5/10/15/20 courses — stands while the resultant is inside the base. The red test for the missing stability check, and possibly the answer to the open composite-depth courses-crossing-the-plane question. Guard is disposable by design (deleted at evolution step 4).
2. **Close the fail-open one-cell arching gate** (apply or capacity-check the thrust the moment cap assumes).
3. **Rigid-block LP as a test oracle only** — the pivotal investment; converts λ, composite depth and the red rows from rulings into measurements.
4. **Promote equilibrium to cascade authority** → tension support → member failure → arbitrary force (DESIGN §7 path).

Also queued from the same session: **a deliberately-red hanging acceptance test** (a piece screwed to the underside of a grounded slab — reads Falling today, should stand while EN 1995 withdrawal capacity holds; a second over-capacity row must fall). Makes the currently-dead fastener withdrawal data load-bearing; anchors step 5.

**After the LP oracle: the mean-strength re-anchor** (DECIDED 2026-08-08 — DESIGN §3): every strength row re-derived from mean data with citations, every pinned anchor recomputed in one verified pass, moved verdicts re-evaluated on physics, lost discrimination replaced.

### New acceptance cases wanted (priority order, from the review)

| P | Case | Why |
|---|---|---|
| 1 | Leaning/offset stack | queue item 1 — the minimal fixture demanding a stability check; per the 2026-08-09 case-12 ruling it is now the *only* self-weight fixture that can honestly demand the stability check (DESIGN §8) |
| 2 | Compression-vs-shear capacity ratio (one joint, two directions, ~50×) | DESIGN §4 calls it key validation; nothing anywhere does it. Belongs beside `ConnectionStrengthTest` |
| 3 | L-corner / return (brick out at a corner vs a free end; assert the *relation*) | the solver has never seen two orthogonal joint families |
| 4 | Lintel over case 7's opening (with/without) | the pair case 7 needs (it fails the published arching gate and is the standing half of three pairs); the model's first spanning member |
| 5 | Dry-stone acceptance rows (intact; one out) | the only configuration where Mohr-Coulomb coupling is first-order; the set has none |
| 6 | Two-wythe wall (same cut, one vs two wythes) | the composite `t` term has only ever been 10.25 cm |
| 7 | Eccentric surcharge over an opening | no test applies a discrete external load |
| 8 | Progressive removal to a predicted count (12×12 wall; stands at N−1, falls at N) | DESIGN §4's headline integration test exists only on a 6-piece toy |
| 9 | Removal order independence ({A,B} == {B,A} settled state, bit for bit) | cheap, Core-level, uncovered |

Standing doubts on existing cases (physical evidence would settle, not more derivation): case 8's named 2-brick set (a bricklayer expects the whole course); case 17 has zero discriminating power intact (repurpose as the *wide* stack-bond removal — utilisation grows with width while running bond arches); case 18's height-independence property belongs on a cheap 3-column Core fixture, parameterised over n; case 20's true count is more than 2, fewer than the model's 9-today. Case 3's verdict is right but reached via a 2.1 m composite section, which is not the real mechanism.

## Deliberately left alone — known, unfixed, with pickup context

None of these block the list above. Each needs its own red test first; most are branching logic that nothing drives yet.

### Load solver

- **`FConnection` is copyable and the latch is per-copy** — `for (FConnection C : Connections)` latches temporaries and reports a structure that never breaks. Closed at the one site that latches; nothing prevents the next. Revisit if a second site latches.
- **`StructureFuzzSupport::UtilisationUnder` is a deliberate hand transcription** — redirect it at production as its own change (never inside a solver refactor), re-running mutations (g)/(h) before and after ([TRAPS.md](TRAPS.md) registry).
- **`AddConnection` happily joins a removed piece** (a tombstone is a valid index) and such a joint is never severed — fail-open, reachable the day anything adds bracing to a damaged wall. One `GraphValidation` row + a guard reusing `IsPieceRemoved`.
- **A pre-latched `FConnection` can be added through the front door** (`ApplyForce` then `AddConnection`): given, unstamped, invisible to the solver, counted by `NumConnections`. Matters for save/load; reject or accept-and-stamp, red test first.
- **`CheckCascadeCase` has a spec-vs-structure index desync one level up** — a fixture row `AddPiece` rejects would shift every per-piece assertion onto the wrong piece. Needs a decision on what such a row asserts.
- **Cycle-division rule absent** (a true voussoir arch still strands; DESIGN §5.1). Deliberately later; make the *reason* observable meanwhile. Now observable per acceptance row: cases 10 and 19 strand 3/6 **live** pieces — part of those two collapse verdicts is unroutability, not masonry — pinned by `FWallCase::StrandsToday` in `WallAcceptanceTest.cpp` (the other eighteen rows claim zero; old case 12's 11-strand pin went with its 2026-08-09 rewrite). Those pins go to zero when this rule lands.
- **Neither fuzz covers piece removal — the reason to wait has expired.** Extend `Structure.CascadeFuzz`: remove 0–2 seeded pieces before cascading, floor the removing-case count, assert (a joint touching a removed piece has given and carries no stamp) and the converse, plus stamps strictly increasing across a cascade–remove–cascade sequence. Oracle drops the node outright (including grounded-ness) where production severs — the divergence is the value.
- **`CascadeFuzz` doesn't check a joint broke in the *earliest* deserved pass** — sweep every joint in the pass-N graph and require over-capacity ⇒ stamped exactly N. Its own mutation needed.
- **`CascadeFuzz` mostly probes the obliterated regime** (median break 9.9× capacity; only ~20% below 2×). Biasing the mass table toward the marginal band is the cheapest improvement; it moves every reported distribution figure, so do it deliberately.
- **Neither fuzz asserts stranded-set equality both ways** — `GetPieceSupport` now exposes it; promote `STRANDING` from diagnostic to property, mutation to prove it bites (un-orderability mutation is recorded live).
- **The accumulation ordering contract is named in a comment and asserted nowhere.** Specified test: three bed joints with areas `{1.0, 1e-16, 1e-16}`, exact `==` against the ascending-order sum (the tolerance *is* the assertion); second row mixing PieceA/PieceB ends; prove by descending-order mutation.
- **The knot check is O(pieces × (pieces + edges)) per fixpoint pass** — Tarjan SCC + one reusable visited buffer is the known fix. **Re-measure the cascade by hand first**: the 1.2 s bottom-course delete predates the adjacency index (which took `SolveLoads` 33.6 → 6.4 ms) and nobody has repeated it, so it is unknown whether the user-visible Delete lag still exists. Nothing in the suite exercises `SolveAndBreak` at scenario scale.
- **Pass-bound observations unasserted**: inner fixpoint provably ≤ 2 passes (no seam to observe it); outer cascade measured ≤ 4 (generator property — pin only as a generous ceiling if ever).
- **`AddConnection` normal top-end hole** (~1e200 components) — lockstep change with the fuzz transcription; see TRAPS.
- **`ConnectionStrength.ShearCap`'s literal compressions** (3.0/4.0 MPa) sit in a window that a mortar retune could silently move out of; derive from the bite point with a fixture precondition when next touched.
- **`UtilisationTolerance`'s 1e-12 absolute floor** must stay below the smallest non-zero expectation (`Unbreakable`'s 1.96e-11) or that row can't fail.
- **An unsupported island reads zero on its internal joints** — "carries nothing" is ambiguous between given / grounded-both-sides / falling; a strain readout will someday need more than the force.
- **`StructureCascadeTest.cpp` hard-sets `BrickMassKg = 2.72`** — used only for grounded pads; two-line tidy when the file is next open.

### Layout producer

- **`ContactsOf`, the RunningBond oracle in `Tests/LayoutTest.cpp` (~line 360), still computes its *area* as `Reach − Distance`** while building its rectangle from bounds — the formula the 2026-08-08 MakeInterface fix retired from production. Latent-only: `RunningBond` emits no containment topology, and the divergence direction now fails *loudly* against correct production rather than agreeing with a wrong one. Same one-line change, whenever that sweep gains mixed sizes.
- **`Core.Layout.InterfaceFuzz` is specified and unwritten** (from the MakeInterface review): random touching pairs, oracle `HalfExtent[a] == 0.5·min(2eA, 2eB, eA+eB−|Δc|)` per in-plane axis — a different expression for the same set, so it cannot agree with a wrong producer; generator snaps ~30% of offsets to exactly `0` or `±|eA−eB|` to strike the switchover; measured at 400k pairs (old formula over-reports 301,493 of them, worst 12,210×); mutation that proves it bites: restore `Reach − Distance`. Two cheap example rows also wanted: a containment where the separation axis is X (all three current rows are Z-separated), and strengthening the inside-both-boxes property with the min-form oracle (today it transcribes the fixed code's own expression, so it can only catch the rectangle decoupling from the bounds again).
- **`DBL_MAX` extent → infinite area accepted** (finite, so it passes `IsFinite`; the *computed* area overflows). Needs a finiteness check on `AreaSqCm` after the multiply, own red row. Not reachable into a structure (`AddConnection` rejects non-finite areas).
- **`RunningBond` neither clears nor refuses a non-empty out-layout; `AdoptLayout` has the identical question** — answer the two together, same policy.
- **`RunningBond` ignores `AddPiece`/`AddConnection` returns**, and an infinite `BrickSizeCm` passes the `!(x > 0.0)` guard (`+inf > 0` is true) — desyncs boxes from handles, then `JoinIfTouching` indexes `Boxes[INDEX_NONE]` and aborts the run. Fix: `IsFinite` on the spec guard + honour `LayBrick`'s handle; red rejection row first.

### World binding and world

- **`FStructureBinding::AddPiece` ignores the structure's refusal** — appends unconditionally, arrays off by one forever. The next change here; one red row (NaN mass → refused, nothing appended, `INDEX_NONE`).
- **`FStructure::HasSupportAnswer` has no direct test** — the uncovered row worth writing: a removed piece inside the extent answers TRUE (documented staleness), currently unobservable only because `ApplyResults` skips removed pieces first.
- **`ABrickActor::SetPieceRef` accepts a re-point** — a second call is always a bug (click one brick, delete another). Prefer a one-way setter with the test asserting the ref unchanged, over ensure-matching.
- **A structure can be built and never destroyed** — no `Destroy(StructureId)`, spawned actors leak on rebuild. The scenario switcher hits this immediately.
- **`TracePiece`'s null-`Find` and removed-piece steps are written but undriven**; two cheap red rows.
- **`BuildLayout` crashes on fewer boxes than pieces** (reads past the end before `AdoptLayout` can refuse), **and a refused build leaves already-spawned bricks standing** — one guard at the top, refusing before the spawn, closes both.
- **`BuildRunningBond`'s three refusal paths are written, unverified.**
- **Brick scale has no degenerate-mesh guard** — a zero-extent mesh gives infinite scale; if written, fail closed, spelled `!(Size > 0.0)`.
- **The scenario cut's batching is unfalsifiable** until a catalogue row cuts ≥ 2 bricks (must arrive as a test change, not a data edit).
- **A mistyped `?Scenario=` is never surfaced to the player** — `OptionNamedNoScenario` is computed and read by nobody but a log line.
- **Degenerate `HoldSeconds` means "never runs"** (timer refuses rate ≤ 0/NaN) — a condemned structure stands forever. Guarded by the catalogue sweep (`> 1.0`, finite); left.
- **The label banner covers the top courses of the tallest structures** — geometrically certain (height-governed rows get 10% clear above; the banner is taller). First user-visible defect in the levels; own slice; candidate fixes: bottom-anchor the banner (cheap) or have `ViewpointFor` reserve headroom (moves every swept inequality).
- **The label says nothing about the hold on the ten no-cut rows** — the one-invariant-line claim in `World.Scenarios.Label` must be restated first; no quiet edit.
- **The scenario banner's draw has no test** (content swept 162 ways; the blit unseen) — same standing gap as the piece-menu panel.
- **The framing aspect is a hard 16:9, no viewport ever measured** — ultrawide players get a wrongly framed structure; the two framing tests pin 16:9 as the unmeasurable-case answer, which any fix must keep.
- **Corbel case F has no absolute anchor** — 2.68132 lives only in comments; the only family row far above 1.0, so the only one that would catch a scaling solver change. Cheap root-utilisation anchor, own red step.
- **`World.Scenario.GameModeBuildsTheWallOnBeginPlay` asserts the model, not the outcome** — the fourth integration test (tick, then assert bricks unmoved and kinematic), grouped into the existing scenario world for cost. Mutation (r) exists.
- **`Visual.ScenarioLevelScreenshots`**: row floor still 9 while `Content.ScenarioMapsExist`'s is 29, and its "eighteen files" comment is stale (58 now). Cost note: 29 PIE worlds.

### Piece menu presenter

- **Rank-0 product decision** (selection meaning on cascade-release) — see the top of this file.
- **The compact toggle is an undiscoverable double-click** — needs a model-side caption string (red test), then an `SButton` in the title strip.
- **A short selection leaves dead panel** (158 px compact / 262 full) — the list's 190 px cap is reserved, not taken; making panel height a function of selection count is legal (no feedback path) but a new behaviour, held against `PanelDoesNotGrowWithTheSelection`.
- **Compact keeps the corner full was opened at** (192 px short of the right edge) — re-homing needs a real "has the player moved it" signal; the obvious flag is set by the toggle gesture itself (Slate sends a press before a double-click and synthesises cursor moves).
- **Compact still lays out the empty headroom-scale rows** (~27 px) — suppressing needs a model-side "is there a bar" field (the no-logic-in-Slate rule forbids the branch).
- **A viewport resize is only corrected on the next grab**; a resize that puts the title strip off screen leaves nothing to grab. Cheap fix if it bites: hold the last known viewport size.
- **`Delete` still shifts sideways when the readout collapses** (bounded, safe direction — dead click at worst); **a fourth pick pushes Delete down 22 px** (follows the player's own click; watch if selection ever grows from anything else).
- **"broken (went with a removed piece)" wording** — becomes a lie with the cascade wire (item 1 above); zero-cost interim: drop to bare "broken".
- **A live brick with no joints has no model-side sentence** (the floater case) — a widget would have to branch; `CountText`'s empty-case precedent says give it one.
- **`ForceN` discards the sign** — `Role` recovers bed-above, not head-joint push-vs-pull; a destruction game whose readout can't show tension is worth settling before the shape sets.
- **The entry list carries no liveness** (`FInspectorPieceEntry` presents removed/foreign/live identically) — decide with the widget; it is the surface where the rank-0 decision becomes legible.
- **`PieceMenuInspectorForSelection()` is private** — no test can assert the controller feeds the widget the right inspector; making it public is the highest-value small test left here.
- **`OnHoverPiece` line-traces every moved-mouse frame, unmeasured** — do not pre-optimise (throttling reintroduces the staleness `Triggered` was chosen against); measure in the same session that judges the overlays.
- **`ChoosePieceMenuRow` dereferences a null `Action` unguarded** (crash is the loud direction; hand-built rows only); one red `World.Choose` row when closed. Same family: **`ShowPieceMenu` aliasing its own `ShownPieceMenuRows`** — copy-to-local first is the worked pattern already in `ChoosePieceMenuRow`.
- **Costs recorded, unpaid**: `bIsLivePiece` costs a full `InspectPiece` per entry per hover; the inspector is built twice per build; `SetInspectedPiece` unguarded for idempotence.
- **Unobserved on a screen** (the press-Play list, item 4 of the ranked list): shader compilation for all three overlay materials, hover routing on the opening frame, entry-click swallowing, panel legibility at scale, magenta-vs-cyan legibility. The recommended real-RHI test is the engine's own `ShouldCreateNaniteProxy == false` on a registered in-world brick (boolean, no baseline) — not a screenshot; if a screenshot is ever written it must be a four-state difference test, never absolute RGB.

### Core systems designed, not built

- **Profiles as editor data assets** (`FConnectionStrength` is a plain struct; adding a row needs a recompile) — the remaining gap to "materials are data".
- **Wood, added exactly once** to prove data-drivenness (deliberately not added with the profile library — adding it beside the calibration data would spend the proof).
- **Connection-to-material pairing** — weakest-link: joint fails when connection gives OR bond peels (`connection × material BondFactor`) OR material fails. Build when the second material exists.
- **Damage/force manager; piece-size floor modes 1/3 then 2; piece timestamps** (DESIGN §2).
- **Shear direction is discarded** (`FConnectionLoad::Shear` is a magnitude) — matters when anisotropic wood arrives, not before.
- **In-game builder choosing connection types**; **a real brick mesh** (one-line asset-path change; keep the Nanite rule in TRAPS in mind).

## Housekeeping

- **`BeamAcceptanceTest` no longer needs to sidestep containment** (the MakeInterface fix landed): moving its piers inboard would be the suite's only contained bearing exercised through a real solve. Its own slice — the readings are anchored to the current geometry and must be re-derived, not nudged.

- **Source comments cite the deleted design docs by name** (`ARCHING_DESIGN.md` ×80, `MOMENTS_DESIGN.md` and `COMPOSITE_DEPTH_DESIGN.md` ×28 each, `PROJECT_REVIEW.md` ×7, `REAL_WORLD_CHECK.md` ×2). Those now point at git history and at the matching DESIGN.md section. Not urgent — the reasoning they cite is preserved verbatim in git — but when a cited comment is next touched, repoint it. Find them: `grep -rE "ARCHING_DESIGN|MOMENTS_DESIGN|COMPOSITE_DEPTH_DESIGN|REAL_WORLD_CHECK|PROJECT_REVIEW" Source/`
- **`Scripts/Convert-CommentBlocks.ps1 -Check` exits 2** on `StructureCascadeTest.cpp:707` (backslash-continued comment diagram — the script is behaving correctly; see TRAPS). Either rewrite the diagram line or teach the script to pass such files through — exit 2 on every run trains people to ignore the check.
- **The anonymous-namespace sweep has no check behind it** — the discipline is prose in header comments; a `-Check` script over `Tests/` ("no anonymous namespace, no file-scope declaration") would turn an engine-source build error into a named refusal. `Convert-CommentBlocks.ps1` could plausibly host it.
- **`Tests/CorbelScreenshotTest.cpp` has not been through review-expert** — a second `NonNullRHI` visual harness written inside a reporting task. Review targets: the duplicated brick-spawn recipe, the hand-written release rule (reproduces the `HasSupportAnswer` guard), the 15 m Y-offset applied to spawn only.
- **`PresenterHeadroomScale`'s doc comment is stranded** at `Core/PieceMenu.cpp:604`, eight helpers above its function. Cosmetic.
- **CLAUDE.md needs two edits the moment the first functional test lands** (the `+`-joined filter; "a new test is just a new file" scoped to arithmetic tests). Not yet due — no functional test exists.
- **Promote the integration-test entry rule into CLAUDE.md?** Open call, **owner: the user** (it earned its place by catching a real bug; the case against is CLAUDE.md staying short). Likewise **the visual-run command as Run B** in CLAUDE.md's testing section, owner: the user.
- **Prune `.claude/settings.local.json`** — a dozen stale single-use permission entries.
- **Case 12's standing-weight bracket (550–1,050 N) has a generous floor** — the reviewer's most conservative recount reads ≈ 363 N. Verdict-insensitive (still ≥ 4× on honest thrust; bond alone covers the kern demand); tighten the row comment's bracket if that row is next touched.
- **`SolveKeyOf` in `WallAcceptanceTest.cpp` prints `-0.0` and `0.0` as distinct cache keys** for identical geometry — one redundant solve at worst, never a wrong answer. Normalise the zero if the key is next touched.
- **The load fuzz's inline unbreakable strength literal** (`Tests/StructureFuzzTest.cpp`, grep `1.0e9`) — one-line swap to `DestructionProfiles::Unbreakable`; input change, do it deliberately.
- **Unqualifying the 11 redundant `LayoutTestSupport::`/`StructureFuzzSupport::` qualifications** left from the collision workaround — harmless; belongs to whoever next owns those files.
- **Watch for the transient** `joint #4 must be carrying a bend … it carries 0.000000` (fired once 2026-08-06 during overlapping builds, never reproduced) — a moment going to exactly zero in a corbel fixture is what a mis-firing arching cap looks like; if it recurs, treat as signal.
- **`FStructurePiece` should take a material rather than a bare mass** (every caller re-derives from density) — small change, do it with the brick-actor work it serves.
