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
- **Slice 3 — monotone growth on boundary contact.** Fixture whose mechanism straddles the initial
  boundary. First solve names a boundary-touching mechanism → grow → strictly larger, stable, equals
  the whole-structure LP set once grown. Assert monotone (no block un-falls). Cap-bound case: partial
  sound subset returned, router stands the rest.
- **Slice 4 — seed derivation + real above-cap disturbance (CHANGES COMMITTED VERDICTS — needs
  sign-off).** Wire seed derivation into `SolveAndBreak`; drive `RemovePiece`+`SolveAndBreak` above the
  real cap (e.g. the porch-post torsion the router over-holds, `DESIGN.md:385`). Assert the prover
  fells what the router stood, 0 stranded, and `World.Scenarios.ShedRealisticCollapseRow` is unchanged.
- **Slice 5 — determinism + `OracleSweepFull` byte-identity gate.** Permuted-seed / permuted-region
  determinism (mirroring `OracleMechanismMultiModeDeterminismTest.cpp`); `OracleSweepFull` 5/5
  byte-identical (core untouched by construction).

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

## Physics-model calls (OWNER SIGN-OFF before slice 4) vs pure engineering
**Owner sign-off:**
1. **Override direction & authority** — the prover may upgrade an above-cap router *stand* to a proven
   *fall* but never the reverse; this ratifies changing committed above-cap verdicts (may fell things
   the flagship scenarios currently stand). (Tie to case-21: this is the collapse direction, so case-21
   does not bar it.)
2. **Region cap and growth/stopping policy** — how large a neighbourhood is worth proving, and the
   accepted false-stand miss when a mechanism exceeds the cap ("thrust is not local",
   `PROMOTION_DESIGN.md:293`, means a bounded region *will* miss some collapses — a knowingly-accepted
   limitation like `DESIGN.md:385`).
3. **Crediting the first-crack LP locally above the cap** at all (consistency with case-21 and the
   442-shed router ruling).
4. **Seed definition** — what counts as "the disturbance" (removal neighbours vs previous-pass severed
   joints).

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
