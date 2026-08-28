# SHED_PATH — the route from the 2D masonry LP to a playtest-verified brick shed

This is the decision record and slice queue for the north-star **/goal**: build and correctly
simulate a **brick shed with a wooden roof and a wooden overhang over the front door, the overhang
carried by wooden posts** — remove bricks, or remove the posts, and it collapses *correctly*, and
it is **verified by playing the game**, not by green tests alone. The user set the priority
explicitly: **accuracy first, performance second.**

It supersedes nothing in [PROMOTION_DESIGN.md](PROMOTION_DESIGN.md); it is the layer *above* the
step-4 promotion arc, sequencing the deferred capabilities (wood, tension, member failure, 3D) plus
the build/play surface into verified TDD slices. Read PROMOTION_DESIGN.md §12 for the rulings this
builds on (D3 dimension-parametric assembly, D4 one-authority-increment, D8 scope-by-size).

---

## The four consciously-deferred capabilities the shed forces live

Grounded against today's code:

- **Wood as a material** — only `StructuralConcrete` + `ClayBrick` exist (`Profiles/MaterialProfiles`).
  Wood is a single data addition (a `Timber` C24 profile).
- **Cross-material bearings** — a per-joint `FConnectionStrength` already lets a wood-on-brick or
  post-on-ground *frictional* contact be laid today; what's missing is the connection × material
  weakest-link pairing (`BondFactor`, declared-and-unused until a 2nd material exists).
- **Tension support (step 5)** — nail/screw/bolt withdrawal capacities are dead data with no
  reachable code path. The overhang's wall-fixing needs this.
- **Member failure (step 6)** — pieces never fail, only joints do. Wood members snapping needs this.
- **3D** — the LP is strictly 2D X-Z (two contacts/joint, three equilibrium rows/block); the bridge
  *refuses* any Y-normal joint as "a plausible number with wrong statics." A shed is four walls in
  two orthogonal planes plus a two-way roof — inherently 3D.

Plus: the **block cap** (D8, 200 blocks) routes anything larger to the router, which cannot hang,
routes downward-only, and never fails a member, and whose masonry heuristics are 2D-validated. A
realistic brick shed is far larger than 200 blocks — so under today's rules the shed's masonry would
be judged by the *router*, not the LP. **Accuracy-first means the cap rises for the shed** (see Flags).

---

## Rulings (orchestrator, decide-don't-ask — the user delegated these)

**R-Scope — the authority that judges the shed is FULL 3D LP, staged behind a 2D-cross-section
proving ground, with BLOCK GRANULARITY (not dimensionality) as the primary accuracy/performance
lever.** The /goal's end-state (four walls + roof, watched toppling in 3D Chaos) is only satisfiable
by a 3D authority — a 2D slice can never judge out-of-plane collapse or the closed-box bracing that
is *why a shed wall stands*, so a 2D slice is disqualified as the *deliverable* under accuracy-first.
But 3D LP is the largest, riskiest piece, so it is not built cold: the material-physics slices
(wood, cross-material bearings, tension, member failure) are **dimension-independent** and land first
on the cheap 2D LP, where their reds are easy to reason about; only then is the *assembly* generalised
to 3D (D3 already mandates a dimension-parametric assembly). The real tractability lever is **block
count**: a shed's structural mechanism is a few dozen macro-elements, and per-brick granularity is
only needed *where bricks are removed* — coarse 3D blocks by default, refined to per-brick near a cut.
*This ruling is provisional until E0*, the point where the 3D work actually starts: it is re-confirmed
there against a measured toy-3D-LP tractability reading. Fallback if toy-3D proves intractable or its
mechanism can't be made deterministic under friction-pyramid degeneracy: ship the 2D-slice LP for the
in-plane overhang/post mechanism **with a loud, documented out-of-plane limitation** (D2 "state the
boundary honestly").

**R-Overhang — the front canopy is carried by POSTS IN COMPRESSION *and* a WALL-FIXING IN TENSION,
neither sufficient alone.** This is the reading the user's words force: a canopy "over the front door"
is attached to the wall at the back, and "remove the posts → it falls" is only *true* if the wall
fixing cannot cantilever it unaided. So both removal paths are meaningful — pull the posts *or* pull
the wall bricks/fixing and it drops. **Requires tension support (step 5, Phase C).**

**R-Member — member snapping (Phase D) is a fidelity add-on DEFERRED past the first playable shed.**
Every headline behaviour the user named is *removal* (pull bricks, pull posts), never overload-snapping.
The first shed sizes every wood member to stand; an over-spanned-beam-sags behaviour is added later
(or sooner iff the roof beam can't stand without it, discovered in Phase B/F).

**R-Scale — target a TOY-to-MODEST-scale genuinely-3D shed FIRST.** Accuracy-first does not mean
"1000 bricks on day one" — a full brick shed under 3D LP will not re-solve at interactive rates. A
toy-to-modest 3D shed (coarse blocks / modest brick count, within a tractable LP size) demonstrates
every required behaviour — brick walls, wood roof, posts, overhang, correct collapse on removal,
playtest — and is the honest first deliverable. Scale up via block-coarsening (E4) only after the
behaviours are proven.

---

## The phased slice queue (each phase = failing-test-first slices; test → dev → review)

**Phase A — finish step 4's tail (2D, existing infra).**
- **6b** — wire the min-violation readout into production (bridge sets `bMinViolationReadout`; overlay
  consumes `Utilisation`/`ViolationUu` via `ConnectionOfJoint`; decide the under-capacity util
  definition + whether first-crack rows join the min-violation LP). *Next up after 6a commits.*
- **6c** — re-aim the fuzzes to the mechanism ("no joint broke that wasn't in the mechanism").
- **6d** — make the dead/live split real (`bGravityIsLive`/`bLive` load-bearing; wall-15/16 separate
  once the surcharge is live). **Prerequisite for roof-as-surcharge and step-7 impulses — matters
  directly for the shed** (the roof is a dead surcharge on the walls).

**Phase B — wood + cross-material bearings (dimension-independent; 2D LP).**
- **B1** — add the `Timber` (C24) material profile (density ~0.42 g/cm³, EN 338 C24 strengths). The
  deliberate single addition that proves data-drivenness.
- **B2** — make the connection × material weakest-link pairing (`BondFactor`) live.
- **B3** — cross-material bearing acceptance: wood roof on brick wall head, post on ground, standing
  under the LP carrying compression, dropping when the bearing is removed.

**Phase C — tension support (step 5; dimension-independent; 2D LP). Required by R-Overhang.**
- **C1** — fastener withdrawal goes live in the LP (the hanging test: a piece screwed under a grounded
  slab stands while EN 1995 withdrawal holds, falls over-capacity).
- **C2** — overhang hung/fixed to the wall: stands on posts-plus-wall-fixing; distinct removal
  outcomes for pull-posts vs pull-wall-fixing.

**Phase D — member failure (step 6; 2D LP). DEFERRED past first playable shed per R-Member.**
- **D1** — piece-level member bending/crushing against `FMaterialProfile::Strength` (the standing
  `Acceptance.Beam.Midspan…` red).

**Phase E — the 3D decision and the 3D LP (the crux; gated on R-Scope re-confirmation).**
- **E0** — re-confirm R-Scope against a measured toy-3D-LP tractability reading.
- **E1** — dimension-parametric 3D assembly behind a flag (6 equilibrium rows/block, ≥4 contacts/joint,
  linearised friction pyramid; 2D path stays bit-identical).
- **E2** — re-derive mechanism extraction + per-group canonicalization + Farkas in 3D (determinism
  fuzz sharper here — friction pyramid degeneracy, R3).
- **E3** — the 3D bridge stops refusing Y-normals for 3D-flagged structures; 2D refusal stays loud.
- **E4** — block coarsening: coarse 3D blocks by default, per-brick refinement near a cut (the
  tractability lever; red = coarse verdict matches per-brick on the same cut).
- **E5** — 3D latency ladder + block-cap re-measure + R7 Shipping-FP re-confirm in 3D.

**Phase F — the build/scenario layer.**
- **F1** — a `LayStructure` shed builder + `FScenario` catalogue row + a duplicated map (via
  `New-ScenarioMap.ps1`): 4 brick walls + wood roof + posts + overhang with cross-material joints, a
  cut list (posts, wall bricks), a hold. World-free builder test asserts piece/joint counts + stands.
- **F2** (optional, later) — minimal interactive build mode (material/connection choice while placing).

**Phase G — play layer + playtest verification.**
- **G1** — world-free integration test: remove a post → LP mechanism (not router) releases the
  overhang; remove a wall brick → local masonry mechanism.
- **G2** — real-RHI playtest harness: join the shed level, settle held, script a post removal via the
  `FStructureBinding` path, tick a real Chaos world, photograph before/after — plus the **human
  eyes-on pass** the /goal demands (pull posts, pull bricks, watch it fall right).

Ordering rationale: finish step 4 → prove the material physics cheaply in 2D → generalise to 3D once
the physics is proven (de-risked) → author the shed → verify by playing. One authority increment at a
time (D4).

---

## Flags for the user (surfaced, not blocking — recorded here for the record)

- **A full brick shed under full LP will not re-solve at interactive rates.** Accuracy-first means the
  block cap rises for the shed and player actions on it will visibly pause (seconds-to-minutes on a
  large 3D structure). Block-coarsening (E4) is the mitigation; the toy-first ruling (R-Scale) keeps
  the first deliverable tractable.
- **3D solve cost is estimated (10–30×), not measured** — 2D latency conclusions do not transfer. E5
  re-measures before any interactivity promise.
- **Determinism (R7 Shipping-FP) is owed and becomes load-bearing** for a shipped shed collapse, and
  must be re-confirmed in 3D where the FP path is longer.
- **The final sign-off is eyes on Chaos**, not an automated frame (R8) — the human playtest pass is not
  optional.
