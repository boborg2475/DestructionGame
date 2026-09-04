# Regional collapse prover — working plan (review item 12)

**Status: DESIGN, not yet ratified.** Owner chose this accuracy path 2026-09-02. Slices 1–3 (the
sound machinery, on isolated fixtures) are implementable now with no committed-verdict risk; slice 4
(integration into the real above-cap cascade) changes committed verdicts and is **gated on the
owner sign-off in §"Physics-model calls" below**, plus a DESIGN §8 ruling.

## The one result that decides the whole design

A region LP with its **boundary blocks pinned GROUNDED** is a **one-directional prover**: it can
prove *collapse* but never *standing*. Already measured and ruled in this project
(`PROMOTION_DESIGN.md:268, :297-301`, `DESIGN.md:379-380`):

- **Grounded boundary is SOUND for collapse.** Interior region pieces keep all their joints;
  boundary pieces are earth (write no equilibrium rows). That feasible set is a *relaxation* of the
  global problem restricted to the region's variables (grounding only *removes* constraint rows), so
  **region-with-grounded-boundary infeasible ⇒ the whole structure is infeasible, and the mechanism
  is genuine.** "Verified sound in review" (`PROMOTION_DESIGN.md:297`).
- **Free / imported-load boundary is REFUTED as a bound.** "The omitted weight left with it" — a
  closed sandwich issued four false certificates, one wrong by 1,412× (`PROMOTION_DESIGN.md:264`,
  `DESIGN.md:380`). **region-feasible ⇒ nothing.**

So the prover **never reports "stands"** (it defers standing to the router entirely). It can only
upgrade a router *stands* to a proven *falls* in the disturbed neighbourhood — which maps exactly
onto the router's actual weakness: it **over-holds** (stood the leaning stack, the two-load-path
body, the porch-post torsion; `DESIGN.md:385`). Because it only ever moves toward collapse, it
**structurally cannot commit the case-21 error** (a false *stand*, the opposite direction). The only
residual imprecision is false stands it *misses* (a region too small to contain the mechanism) —
acceptable, because the router already stands those and nothing regresses.

## 1. Region extraction
- **Seed** = pieces adjacent to a just-severed joint. Pass 1 of a cascade: neighbours of the
  removed (tombstoned) piece. Later passes: endpoints of joints stamped in the previous pass
  (`ConnectionBreakPass[j] == Pass-1`, `Structure.cpp:2855`).
- **Growth** = BFS over the connection graph by **joint-hops** (`PieceJoints` adjacency already built
  each solve, `Structure.cpp:678-694`). Graph-distance, not physical radius.
- **Stopping rule, two gates:** (1) region ∪ grounded boundary ≤ region cap (start at the D8 cap 200,
  `DESIGN.md:383`; likely tune toward the measured ~84–104 promotable band, `DESIGN.md:382`, for
  latency); (2) **mechanism interiority** (slice 3) — grow until the extracted mechanism does not
  touch the grounded boundary ring; if it does, the true mechanism may extend, so grow and re-solve.
- **Natural region exceeds cap:** shrink/stop at the cap and accept a possible miss (false stand →
  router covers it). *Never* widen past the cap, *never* fall back to a free boundary. (Contrast the
  refuted sandwich, which had to grow toward the foundation because its pessimistic side needed the
  real support; the prover never needs the real support, so it stays small.)

## 2. Boundary conditions — the pose and its soundness
**Pin the boundary ring GROUNDED. Import nothing.** Interior region pieces R bridged normally (real
mass/joints/strengths). Frontier ring B (blocks one hop outside R that R joins to) bridged with
`bGrounded = true`. Everything outside R∪B excluded exactly as `BuildRigidBlockProblem(Structure,
ExcludedPieces, …)` already excludes (`RigidBlockBridge.cpp:60-64, :112-115`). Joints with two
grounded ends skipped (`RigidBlockBridge.cpp:133-136`). Pose `bGravityIsLive=false` (feasibility) +
`bFirstCrackRows=true`, matching the below-cap authority (`Structure.cpp:2768,2779`).

- **No false stand (optimism impossible by construction):** the prover emits no stand verdict; "no
  opinion" leaves the router untouched.
- **No false collapse (no crying wolf):** grounding B *adds* support vs reality (ground is the
  strongest possible reaction); excluding the exterior *removes* both its restraint and its weight —
  the restraint removal is over-compensated by grounding B, and dropping exterior weight can only make
  R *more* likely to stand. Feasible set is a superset (relaxation) of the global restricted to R, so
  region-infeasible ⇒ global-infeasible; any certified mechanism is a genuine subset of a true global
  one (`PROMOTION_DESIGN.md:297`).
- **Forbidden (both are the refuted pessimistic side):** do NOT import the router's `SolveLoads`
  boundary loads; do NOT conclude "stands" from a feasible region.

## 3. Stitching — union, one direction only
After the router sweep for a pass, run the prover on the region. If it certifies a mechanism:
- Release the **interior** blocks it moves (`FOracleMechanismBlock::bMoves`, `RigidBlockOracle.h:517`,
  → pieces via `Problem.PieceOfBlock`). A grounded boundary block never moves (writes no rows), so
  "interior" falls out automatically.
- Sever the intact joints it opens (`JointOpensOrSlides`, `:577`, → `Problem.ConnectionOfJoint`) —
  the identical machinery `BreakByEquilibrium` uses (`Structure.cpp:2835-2857`).
- Mark moved interior pieces **Falling**. **Do NOT** mark the region's non-moving pieces Supported —
  that is an LP stand credit above the cap, which case-21 forbids. **The region overrides the router
  only toward Falling, never toward Supported** (contrast `ApplyLimitAnalysisSupport`
  `Structure.cpp:2950-2994`, which below the cap writes both directions because the LP is full
  authority there).
- **Mechanism past the boundary:** detected by a moved interior block adjacent to B, or an opened
  joint on the R↔B frontier. Growth is **monotone** (replacing a grounded boundary block with its
  real, weaker self can only let *more* move), so re-extract larger/shifted and re-solve — reveals
  only additional collapse, never retracts. Bounded loop (region grows ≥1 block/iteration, capped).
- **Cascade:** released blocks + severed joints feed the next pass via the existing `SolveAndBreak`
  loop (`Structure.cpp:3142-3191`) — no new driver.

## 4. Integration seam
A third arm in the `SolveAndBreak` pass loop (`Structure.cpp:3165-3174`). When the gate declines
(above cap / no geometry / LP refusal): run `BreakByCapacitySweep(Pass)` **then** union in
`ProveRegionalCollapse(SeedForThisPass, Pass)` (above-cap only; below the cap `BreakByEquilibrium`
is already full authority). `ProveRegionalCollapse` mirrors `BreakByEquilibrium`'s shape: compute
region+boundary from seed → pose → `SolveRigidBlock` → stitch Falling-only override → return whether
it broke anything. Gated on `HasCompleteGeometry()` exactly as `BreakByEquilibrium`
(`Structure.cpp:2743`) so it is a provable no-op for the two geometry-free fuzzes.

**Reuse (unchanged → protects `OracleSweepFull`):** the oracle core `SolveRigidBlock` and the whole
`RigidBlockOracle.cpp`; mechanism extraction / Farkas / `PieceOfBlock` / `ConnectionOfJoint`
(`RigidBlockOracle.h:438-439, :571-593`); the sever/release idioms (`Structure.cpp:2831-2857`) and the
Falling-half of `ApplyLimitAnalysisSupport`; `PieceJoints` adjacency; `EffectiveJointStrength`
(`RigidBlockBridge.cpp:211`).
**New:** a grounded-boundary bridge **overload** `BuildRegionalProblem(Structure, RegionPieces,
BoundaryPieces, OutProblem, OutWhyNot)` (~90% the existing excluded-pieces loop; delta = force
`bGrounded` on the boundary set, include only R∪B) — kept a SEPARATE function so the shared bridge
poses do not shift; the BFS flood; the `ProveRegionalCollapse` driver + monotone grow loop; seed
derivation in `SolveAndBreak`.

## 5. TDD slices (each independently committable; assert on mechanism, never displacement)
- **Slice 1 — the seam, on a router-vs-LP disagreement fixture.** Extraction → grounded-boundary pose
  → LP → Falling-only stitch, end to end. Region cap ≥ fixture size (boundary = ground only), seed
  supplied by the test. Red test below.
- **Slice 2 — the grounded boundary actually cuts.** Same stack, small region cap so the flood stops
  mid-stack and the frontier ring is pinned grounded. Assert the grounded-boundary region is still
  infeasible and fells the sub-stack; the whole-structure ground-only LP names a superset (soundness
  witness). Teeth: mutate the boundary pose to *free* → wrongly feasible (grounding is load-bearing).
- **Slice 3 — monotone growth on boundary contact. LANDED 2026-09-03 (grow-from-modest); default cap
  raised back to 200.** `ProveRegionalCollapse` now floods a MODEST initial region (~16 blocks) from the
  seed and grows only as the proof demands: an interior fall (bounded by genuine `bIsGrounded`
  foundations) is stitched; a fall touching a cut-artifact grounded boundary re-floods mechanism-directed
  from the moved set at a doubled budget up to the full cap; a no-fall pose grows a bounded speculative
  search capped at `min(cap,48)` (a blind standing flood must not balloon toward the cap). Monotone,
  terminating (interior / cap-bound / fixpoint / iteration bound). Every pose is mechanism-sized, so the
  cap-200 corner-hang is **0.53 s** (was >25 min greedy-at-200) and the default cap is back to **200** —
  the owner's original reach, now cheap. Driven by `RegionalProver.GrowOnContactReachesPastAStandingBranch`.
  The reframe below is kept as the historical reasoning that got us here.

  *(Historical — why the first attempt was reframed:)* The committed flood (slices 1-2) was
  *greedy-fill-to-cap*, which already fells everything within the cap-ball of the seed — when
  `cap ≥ structure` it fells the full truth (the seam test), and when `cap < structure` it fills
  exactly to the cap and fells the cap-bound partial (the grounded-cut test). There is **no gap**
  between them: "partial fell *with* cap headroom" is impossible on the greedy flood, so growth is
  unobservable on a chain (the leaning stack). Growth only becomes meaningful with a *different* flood
  strategy — a **modest initial region + grow-on-contact until interior or cap** — and its value shows
  only in **branching** structures, where the greedy cap-ball wastes budget on non-mechanism branches
  and so reaches less of a long mechanism than a mechanism-following grow would. So slice 3 is a
  **latency + branching-reach refinement, NOT a correctness change** (the greedy flood is already
  sound and within-cap-complete). Deferred: slice 4 proceeds on the greedy flood (sound, correct
  within the cap-ball, just not latency-optimal), and grow-on-contact lands later — driven by a
  BRANCHING fixture where greedy fells a partial with headroom that grow-on-contact completes, plus
  the flood-strategy change — when playability latency (posing a cap-sized LP per cascade pass)
  demands it. The cap-invariant property guard (`RegionUnionBoundaryCapInvariantHoldsAcrossSeedsAndCaps`)
  landed here and guards the flood arithmetic through that future change.
- **Slice 4a — cascade integration. DONE 2026-09-03 (committed).** Wired into `SolveAndBreak`'s
  above-cap decline arm (`SetRegionBlockCap` settable, default 200 (grow-from-modest makes it cheap — see slice 3);
  `ProveRegionalCollapse` factored from the test entry; seed = tombstone neighbours pass 1, else
  previous-pass severed-joint endpoints; gated on `HasCompleteGeometry` → fuzz no-op). Driven by the
  Timber-board-on-a-token-Nail over-hold (`RegionalProverCascadeSeamTest`): the router stands the
  cantilever (N≥2-load-path moment-zeroing + item-3's tension clause sparing it on the Nail's f_t>0),
  the whole-structure LP fells it, and the cascade now fells it too, 0 stranded. Full suite 246/242/4;
  **every committed scenario unchanged** — the prover is a sound no-op on today's scenarios (adds
  correctness for the token-Nail over-hold class the router can't see). Oracle untouched → OracleSweepFull
  byte-identity structural.
  - **B1 finding (2026-09-03): the cascade termination fix guards a scenario that is provably
    IMPOSSIBLE to reproduce at unit scale, so it has no small-fixture regression test — by proof, not
    omission.** A prover-felled piece ALWAYS fully disconnects from every standing piece (the 1e-6
    relative mechanism-extraction tolerance severs every moved-vs-standing joint; `SolveLoads` grants
    Supported only along intact ground paths), so the "router re-holds a felled-but-still-connected
    piece" case cannot exist. The real oscillation the `CountIntactJoints`-decreasing key prevents is
    the prover RE-FELLING already-disconnected pieces each pass (jointless blocks are trivially moving
    in the re-pose); termination is the monotone-intact-joints invariant (bounded by connection count),
    which is proven, not empirical. **Follow-up:** dev observed a shed-corner-hang hang under a
    Falling-count key at cap 200 — likely just the slow 200-block LPs (now cured by grow-from-modest — cap 200 is sub-second) rather than
    a true loop; if it recurs, reproduce it by INSTRUMENTING the actual shed scenario (which piece
    re-fells, via which pose), not a hand fixture. (`RigidBlockOracle.cpp` 1e-6 sever rule; `Structure.cpp`
    SolveLoads support BFS; the corrected oscillation comment at the cascade arm.)
- **Slice 5 (NEXT) — determinism + the DESIGN §8 ruling.** Permuted-seed / permuted-region determinism
  (mirroring `OracleMechanismMultiModeDeterminismTest.cpp`); record the new above-cap authority
  arrangement (one-directional collapse prover overriding the router toward Falling) as a dated DESIGN
  §8 ruling. `OracleSweepFull` byte-identity stays structural (oracle core untouched).

### Slice-1 red test (concrete)
Fixture: the **30-course, 10 cm/course mortared leaning stack** (`LeaningStackAcceptanceTest.cpp:57`)
— LP proves it FALLS but the router reads a height-independent 0.1388 utilisation and STANDS (`:79-82`;
single column so N=1, item-3 gate doesn't fire; bonded joints so item-2 edge rule doesn't fire).
(A) `SetEquilibriumGateBlockCap(5)` → 30>5 so `BreakByEquilibrium` declines; `SolveAndBreak` (router
only) → top block `Supported`, releasedCount 0 (router over-holds — teeth). (B) whole-structure
ground-only LP (`bGravityIsLive=false, bFirstCrackRows=true`) → Falls, certified; capture TruthMoving
set via `PieceOfBlock`. (C) NEW (red until built): `SolveAndBreak_WithRegionalProver(seed=baseCourse)`
with region cap ≥30 → every piece in TruthMoving is `Falling`, releasedCount == TruthMoving.Num(),
strandedCount 0. Red for the right reason: `ProveRegionalCollapse`/`BuildRegionalProblem` don't exist,
so (C) fails on missing behaviour while (A)/(B) pass.

## 6. Risks & gates
- **`OracleSweepFull` 2D byte-identity:** safe *by construction* iff `SolveRigidBlock` and the existing
  `BuildRigidBlockProblem` overloads are not edited. Trap: implementing `BuildRegionalProblem` by
  mutating the shared bridge instead of a new overload → shed/gate poses shift. Keep it separate.
  Slice 5 pins 5/5.
- **Geometry-free fuzzes** (`Structure.Fuzz`, `Structure.CascadeFuzz`): prover gated on
  `HasCompleteGeometry()` → provable no-op; add a fuzz-shaped tripwire.
- **Latency/scale:** an LP per above-cap cascade pass on flagship scenarios (1,220-block wall,
  442-block shed, many passes) is the real risk. Mitigations: region ≤ region-cap regardless of total
  size; run only when the router's pass leaves a non-empty seed; a **per-action wall-clock/block budget
  with fail-closed fallback to plain router** (D8 already flagged the cap "wants a per-action wall-clock
  guard", `DESIGN.md:383`). Pin block/pivot budgets, never ms.
- **Interaction with items 2/3/5:** prover's Falling-only override composes with the item-3
  `PieceOverturned` set and item-5 refused-arch reclassification; assert 0-stranded through the union.

## Physics-model calls — RATIFIED BY OWNER 2026-09-03
All four are settled; slice 4 is unblocked.
1. **Override direction & authority — RATIFIED: override toward Falling, LIVE in the cascade.** The
   prover upgrades an above-cap router *stand* to a proven *fall* but never the reverse. This will
   change committed above-cap verdicts (fell things the flagship scenarios currently over-hold, e.g.
   the porch-post torsion). Slice 4's test must assert the shed's committed collapse row is UNCHANGED
   and that a genuine over-hold now falls, 0 stranded — so every verdict change is caught before commit.
   (case-21 does not bar it: this is the collapse direction, the opposite of case-21's false *stand*.)
2. **Region cap — RATIFIED settable; DEFAULT LOWERED 200 → 48 (2026-09-03, owner-approved latency
   unblock).** Originally ratified "match the LP cap (200) as the default", but slice 4a measured that
   posing a 200-block LP on *every* above-cap cascade pass makes the 3D shed scenarios impractical
   (`RealisticBrickShedCornerHangFallsWithWeakPerpends` ran >36 min, unfinished). The over-holds the
   prover catches are LOCAL (the porch-torsion board is ~4 blocks), so the shipped default is now
   `RegionalProverBlockCap = 48` — the same corner-hang test runs in **17 s**, the full suite completes,
   and every committed scenario is unchanged (the prover is a sound no-op on them at 48). `SetRegionBlockCap`
   keeps it fully settable; **200 remains reachable via the setter as the ceiling**, and the accepted
   false-stand miss when a mechanism exceeds the cap stands ("thrust is not local"). **Grow-on-contact
   (slice 3) is the deferred proper fix** that combines cheap typical cost with the full 200 reach; the
   48 default was the interim unblock; grow-from-modest has since landed and the default is back to 200.
3. **Crediting the first-crack LP locally above the cap — RATIFIED (no objection).** The prover poses
   the region with `bFirstCrackRows=true` (the below-cap authority). case-21 distrusts LP bond credit
   above the cap only for *stands*; the prover never stands anything, so case-21 does not bar it.
4. **Seed definition — RATIFIED (no objection).** The disturbance = the removed piece's neighbours on
   pass 1, then previous-pass severed-joint endpoints thereafter.

**Pure engineering (no sign-off):** the grounded-boundary soundness itself (a proven relaxation bound,
`PROMOTION_DESIGN.md:297`); the BFS flood; the bridge overload; sever/release/support idioms;
determinism; the `OracleSweepFull`/fuzz guards; the monotone grow loop; the latency-budget mechanics.

## Key files
- `Source/DestructionGame/Core/Structure.cpp` — `SolveAndBreak` loop (`:3142-3191`),
  `BreakByEquilibrium` (`:2707`), `ApplyLimitAnalysisSupport` (`:2950`), `BreakByCapacitySweep`
  (`:2996`), `PieceJoints` adjacency (`:678`); new `ProveRegionalCollapse` seam.
- `Source/DestructionGame/Core/RigidBlock/RigidBlockBridge.cpp/.h` — excluded-pieces bridge
  (`:16-219`) that the new grounded-boundary `BuildRegionalProblem` overload extends.
- `Source/DestructionGame/Core/RigidBlock/RigidBlockOracle.h` — `FOracleProblem`/`FOracleMechanism`/
  `PieceOfBlock`/`ConnectionOfJoint`/`SolveRigidBlock` (`:359-593`), reused UNCHANGED.
- `Source/DestructionGame/Core/Structure.h` — `EPieceSupport` (`:137`), gate disposition,
  `RemovePiece`/`SolveAndBreak`/`GetPieceSupport`; add `ProveRegionalCollapse` + region-cap accessor.
- `Source/DestructionGame/Tests/LeaningStackAcceptanceTest.cpp` — the router-vs-LP disagreement
  fixture (`:57`, `:79-82`) for slice 1; `Tests/OracleMechanismExtractionTest.cpp` is the posing
  template.
