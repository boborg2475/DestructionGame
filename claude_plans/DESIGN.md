# DestructionGame — Design

A realistic physics-destruction game in **Unreal Engine 5.8** on **Chaos**, written in C++. Players build structures and destroy them; every piece is destructible in a materially believable way, and structures carry **real-time structural integrity**: pull out enough support and a wall collapses under its own weight, with load redistributing as pieces go.

**This is the single design authority.** It states the model **as it is today** — present-tense rules, the constants with their citations, the worked numbers the tests pin, and the traps that were paid for. Derivation history lives in git (the former `MOMENTS_DESIGN.md`, `ARCHING_DESIGN.md`, `COMPOSITE_DEPTH_DESIGN.md`, `REAL_WORLD_CHECK.md` and `PROJECT_REVIEW.md` were consolidated here 2026-08-08; source comments naming those files point at git history and at the matching section of this document).

Reading order for a newcomer: this file → [LEVELS.md](LEVELS.md) (the playable levels) → [TRAPS.md](TRAPS.md) (the live engine/build/testing traps) → [CURRENT_STATE.md](CURRENT_STATE.md) (the living TODO).

---

## 1. Vision

- Everything the player builds is destructible; breaking behaviour is material-specific (wood splinters, concrete fractures, glass shatters).
- Structures stand or fall by real statics: pieces carry real mass, joints carry directional strength, load redistributes as pieces are removed, and the last support pulled is the final straw.
- Materials and connection types (mortar, nail, screw, bolt) are **data, not code** — behaviour is tuned by profile row, never by a new class or branch.
- Force arrives eventually as explosions and kinetic impacts, and debris stays live. (Deferred; see §7 — the blocker is structural, not priority.)

---

## 2. Architecture

### The split: we decide the breaks, Chaos owns the falling

**Pieces in an intact structure do not simulate.** They are kinematic; the loads on every connection are computed by our own world-free solver in SI units, and a joint gives when *we* say it does. When a piece is released it switches to dynamic and Chaos takes over the falling, the collisions and the debris.

Decided 2026-07-31 by a two-brick spike, measured rather than argued:

| | Kinematic | Chaos constraint |
|---|---|---|
| Drift while nominally still | **0.000000 cm** | 0.62–0.70 cm, oscillating |
| Reported joint force | n/a | 16,873–24,147 uu, ±20% (expected 2,670) |
| Lateral force under a purely vertical load | n/a | 3,400–6,300 uu |

The constraint force is ~8× too large, unstable, and carries a spurious sideways component — fatal for a model whose whole premise is splitting force by direction. A released piece settles within 0.018 cm; that transition is what collapse is built on. Physics simulates normally under `-nullrhi`, so integration tests run headless.

**Geometry collections are complementary, not an alternative.** Chaos's own connection graph breaks on a *scalar* strain with no notion of direction — adopting it for the structural decision would surrender the one thing this project exists to do differently. A geometry collection answers *what one piece looks like as it dies* (visual fracture, later and additive, seamed at `FStructure::RemovePiece`); this system answers *which piece gives way and when*.

### The pipeline: how a force becomes a broken joint

```
world-space force
        │  FConnection::ApplyForce()        Core/Connection
        │  owns: interface normal, area, extents, strength profile
        │     ├─ ClassifyForce()            Core/ConnectionLoad
        │     │      FConnectionLoad { Compression, Tension, Shear, moments }
        │     └─ ComputeUtilisation()       Core/ConnectionStrength
        ▼
utilisation ratio   (0 = unloaded, 1 = at the limit, >1 = the joint gives)
```

Plain arithmetic on plain structs — no actor, no world, no ticking solver — which is what keeps it cheap to test and cheap to reason about. Load-bearing properties:

- **A ratio, not a boolean** — the same number drives the break decision and the on-screen strain readout.
- **Stress, not force** — strengths are in MPa, so every comparison needs an area. The same force through half the area is twice as punishing.
- **Giving is irreversible, and a given joint carries exactly nothing.** The latch never resets — mortar does not re-bond, and redistribution is only correct if a broken joint is genuinely out of the structure. One asymmetry is deliberate: the *breaking* call reports the ratio that broke it (>1); only subsequent calls return zero.
- **Caller obligation.** A degenerate (zero/NaN) interface normal makes `ClassifyForce` return a zero load, which downstream reads as "healthy". `FConnection` closes the hole by substituting a zero area, routing through the fail-closed area guard. Anything calling the two stages directly must make the same check (`FVector::Normalize()` returns false for both cases).
- `FConnection::UtilisationUnder` is the non-mutating query and `ApplyForce` is *defined* as that query plus the latch — exactly one copy of the arithmetic decides breaks. A second copy agrees to 1e-9 forever and still differs in the last bit.

### Connections are first-class, and force is classified against the interface

A **connection** between two pieces has its own directional strength profile (compression / shear / tension), like a material: a brick wall fails in the **mortar**, a wood joint in the **nail**. The builder chooses the connection type, so nails vs. screws vs. bolts genuinely differ. Force is classified by direction **relative to each joint's interface plane**, never by world direction — remove the brick beneath another and the same downward gravity is *compression* on a horizontal joint and *shear* on a vertical one.

### Geometry lives above the solver, and produces the graph

`FStructure` is deliberately **position-free** — a piece is a mass and an identity — which is why the load maths needs no world and runs deterministically in milliseconds. `Core/Layout` (and the other producers, `Core/Corbel` and `Core/WallCases`) own geometry and hand down handles; the dependency runs one way only. Three rules are design, not implementation:

- **The interface normal is the axis of separation, oriented by which handle is `PieceB`** — *never* the direction between centroids. A running-bond bed joint's centroid direction has |Z| = 0.5547, below cos 45°, so a centroid normal silently classifies **every bed joint in a wall as a head joint** and the support graph is entirely wrong with nothing crashing or moving. Worked on the spanning-brick case: utilisation 5.269638e-3 against the correct 1.269339e-4 — a factor of **41.5**. The dangerous bug is a normal *inconsistent with its A/B pairing* (a consistently flipped joint reports identical loads), which is why the pair, the normal, the area and the extents are **emitted as one atomic value by one function** (`MakeInterface`), and `AddConnection` rejects extents disagreeing with the area or sitting on a non-axis-aligned normal.
- **Deciding whether two pieces touch and deciding which pairs to consider are separate jobs**; only the second is bond-specific. A generative producer offers its own neighbours; a future contact-finding producer replaces exactly that half.
- **A refused pair fails closed by zeroing the interface area** — a caller who ignores the return value gets a joint that reads *failed*, never one that reads fine.
- **Joints carry their own rectangle** (`InterfaceCentreCm`, `InterfaceHalfExtentCm`, zero on the normal axis) and pieces carry a centre of mass. Geometry-free fixtures are not a special case — they are `e = 0`, reproducing the old answers bit for bit, and `FStructure::HasCompleteGeometry()` makes "nobody supplied positions" askable rather than silently identical to "the load happens to be centred".
- **`GetJointRole` may never read geometry** — the tier of one joint stays a fact about one normal and one pairing. What may read geometry is a *group* rule above the tier (`ReseatSpannedGroups` is the one function licensed), gated on `HasCompleteGeometry()` and a **no-op without it**. That gate is what keeps both fuzz generators — which emit no geometry, and are the only property tests over routing — provably untouched by every geometric mechanism.

### A scenario is a row of data, not a subclass *(decided in code 2026-08-07)*

`World/DestructionScenarios` is a catalogue of `FScenario` rows — name, map, title, expectation, hold time, cut centres, and **the producer that lays it**, carried as a `TFunction` on the row rather than named by an enum a `switch` reads. Same rule that governs materials: a base class or a switch grows a branch per family and every family pays for every other. Twenty-nine rows over three producers today. If a scenario ever appears to need a class of its own, that is the drift to stop. Adding a level = a catalogue row + a **duplicated** map (never file-copied — see [LEVELS.md](LEVELS.md)).

### What a "piece" is at building scale *(decided 2026-08-03)*

One actor per brick for now — but individual bricks are never the unit for a large building. A 20-storey building is ~295,000 bricks single-leaf, yet is held up by ~3,000–5,000 frame members; the intended answer is **a wall is a few large panels while intact, and a panel becomes bricks when something hits it** (the piece-size floor below, and geometry collections per piece). Consequences held today: `ABrickActor` never ticks (the solver is pushed, not polled), `bReleased` is one-way, and `ApplyResults` is an explicit push — so intact pieces could become instanced later with no change to the actor class, the ref, or the release call.

### Piece identity, size floor and timestamps *(designed, mostly unbuilt)*

Pieces do not subdivide infinitely: a tunable minimum volume (~1.5–2 uu across) below which a piece cannot break further, with three selectable modes when a break would cross it — **1 indestructible** (floor proven), **3 disappear** (detection proven), then **2 Niagara dust** (the real feature). Each piece carries a creation timestamp stamped when it breaks free, so a collapse's sequence is recovered by inspection rather than logs.

---

## 3. Units and constants — the easiest way to be wrong by 100×

World scale is Unreal's default **1 uu = 1 cm** *(decided 2026-07-30)*. Verified against UE 5.8 source:

| Quantity | Unreal unit | Conversion |
|---|---|---|
| Mass | kg (`SetMassOverrideInKg`) | none — published values directly |
| Density | g/cm³ (`UPhysicalMaterial::Density`) | none — concrete 2.4, oak ~0.75 |
| Gravity | `-980.0` | none |
| Length | cm | mm ÷ 10 |
| **Force** | kg·cm/s² | **1 N = 100 uu** |
| **Impulse** | kg·cm/s | **1 N·s = 100 uu** |

Strengths are stored in **SI megapascals** so they stay checkable against published tables; the one conversion boundary is **`ForceUnitsPerMPaSqCm = 10000`** in `Core/ConnectionStrength.h` (the newton factor × cm²→mm²). Never open-code the factor — a missing or doubled conversion is out by exactly 100×, which tuned thresholds conceal. Two corollaries:

- `weight_in_force_units = MassKg × 980` **already contains** the conversion; applying it again is the recurring mistake.
- Moments are uu·cm; `M/W` with `W` in cm³ gives uu/cm², the identical quantity — **no second boundary exists or is needed** (verified end to end in the 2026-08-08 review: no double or missing conversion anywhere).

### The named constants, each with its citation

| Constant | Value | Source and status |
|---|---|---|
| `BedJointCosine` | cos 45° | The bearing/shear tier line. Symmetry — the one angle needing no justification; anything else is a tuning knob pretending to be physics. Pinned from both sides at 40°/50°; the exact tie is deliberately unasserted. |
| `SolverArchingDepthPerSpan` | **0.866** (√3/2) | BS 5977-1's equilateral (60°) lintel-loading triangle. **Two honest divergences from the published rule**: practice is a *threshold* (arching happens iff ≥ 300 mm of masonry above a 45° apex) where we grade *continuously* via `d_e = min(cover, 0.866·L)`; and 0.866 is the 60° figure where practice advises the conservative 45°. Defensible for a game; do not cite it as following BS 5977. Effective span is measured abutment-centre to abutment-centre (published: clear span + beam depth; close, unverified to matter). |
| λ (composite-depth arm) | **3.464 = 2√3** | **A ruling wearing a constant's clothes — the one number neither published nor derived.** The honest justification is the **mirror span**: a cantilever's analogue of a simply-supported span is 2 × projection; masonry beam practice takes depth up to the span, so D ≤ 2 × projection, and with `e ≈ projection/2`, `λ·e ≈ 1.73 × projection` sits just inside. *(Not* "2 × 0.866" — that is a coincidence of unrelated constants, and 2 × 0.866 = 1.732 is not even λ.) The measured window is **λ ≥ 2.8438** (40-course free end must stand, reads 0.6740) and **λ ≤ 3.8224** (the tall-wall property); 3.464 sits inside with 22% below and 9% above. EN 1992-1-1 §5.3.1(3)'s span < 3 × depth is a *scope* clause, not a cap — read as a cap it gives λ ≈ 0.67 and fails the free end by 4×. |
| `GeneralPurposeMortar.TensileStrengthMPa` (`f_xk1`) | **0.10** | EN 1996-1-1 Table 3.2 recommended value, clay units, general-purpose mortar, plane of failure parallel to bed joints, both mortar classes — **verified 2026-08-08** (RoyMech reproduction of the Eurocode table). A once-worrying 0.167 figure traced to UK NA Table NA.6 for *aggregate concrete blocks* and never applied. |
| `GeneralPurposeMortar.ShearCohesionMPa` (`f_vk0`) | **0.20** | EN 1996-1-1, clay units in general-purpose mortar M4/M6 (permissible range 0.15–0.30). Confirmed exactly. |
| `MaxShearStrengthMPa` | **1.3** | The Eurocode 6 truncation `0.065·f_b` against a 20 MPa unit. Confirmed exactly. Defaults to *unbounded* when unset, so an unset cap is plain Mohr-Coulomb rather than an accidentally rigid joint. |
| Mortar μ | 0.6; dry stone c = 0, μ = 0.7, tensile **exactly 0.0** | Profile rows in `Core/Profiles/ConnectionProfiles.cpp`, every value carrying a citation comment (EN 1992-1-1, EN 1996-1-1, BS EN 998-2, BS EN 459-1, EN 1995-1-1, EN 338). |
| Fastener reference density | 1 fastener / 100 cm² | Nail/screw/bolt profiles hold EN 1995-1-1 *force-per-fastener* capacities smeared over this density, because the model compares stresses and a joint has no fastener count. A `Nail` profile means "a nailed joint at reference density", not "a nail". When fastener count becomes a joint property, **re-derive from the recorded kN figures, never re-tune**. |
| Standard brick | 21.5 × 10.25 × 6.5 cm, 1.9 g/cm³ | Mass **2.72163125 kg** (density-first multiplication order — see the contract in `PieceMassKg`), weight **2667.198625 uu**. Half bat **1.297521875 kg** (a circulated 1.29777 does not reproduce). Bed joint 105.0625 cm², head joint 66.625 cm², bed-patch section modulus 179.4817708 cm³, kern ±1.708 cm. |

### Strength basis — DECIDED 2026-08-08: mean values, for realism

All code figures are **characteristic (5%-fractile) design values** meant to sit under safety factors of 2.0–2.7. UK NA Table NA.6 (decades of BS 5628 test data) gives clay in M4/M6 mortar `f_xk1` of **0.5 / 0.4 / 0.3** by water absorption (<7% / 7–12% / >12%; `f_xk2` 1.5 / 1.1 / 0.9), and mean tested bond strength runs ~2× characteristic again — so the project's tension verdicts are **3–8× pessimistic** against real walls, in the direction that makes the corbel rulings *more* defensible. **The user chose mean values.** The re-anchor runs as its own slice **after the LP oracle** (§7): every strength row re-derived from mean test data with citations, every pinned anchor recomputed in one verified pass, any acceptance verdict that moves re-evaluated on physics, and any case that stops discriminating replaced rather than silently lost. Until that pass runs, 0.10 / 0.20 stand and **must not be tuned piecemeal** — every number in this document is a consequence of leaving them alone.

### Published corbelling limits — calibration reference, not a constraint

Codes cap corbel projection per course at min(⅓ bed depth, ½ unit height) = **3.25 cm** for this brick, and total projection at roughly the wall thickness (10.25 cm). The fixtures' half-cell step is 11.25 cm — 3.46× the per-course limit — and the cascade's natural 33.69° stepping front is about twice as steep as the ~63° corbelling permits. These are *design* limits with safety factors, not collapse predictors; the user's stated purpose is to test past the codes, and the corbel rulings (§8) were made knowing these figures. The counter-argument on record: a raking cut through a *bonded wall* is not the free-standing corbelled ledge the code limits address — which is exactly what composite action models.

---

## 4. Testing strategy

### The two layers, and what "failure" means in each

- **Unit tests — gravity off, world-free.** One behaviour, one clean input. Assert on the **mechanism** (connection state, a ratio) — *distance travelled is a trap*: two pieces can sever their bond and stay resting exactly in place. Displacement is never a valid break assertion.
- **Integration tests — gravity on, everything connected.** Assert on the **outcome** (did the wall physically move after N simulated seconds), because mechanism readings are exactly what stayed green while a real wall stood still with orphans hanging in mid-air. Tick a **fixed number of simulated seconds, never a settle-poll** (settling is non-deterministic and a poll turns failure into a timeout).
- **The entry rule** (earned by a real player-visible bug that 71 green tests missed): *if what can be wrong is arithmetic on a graph, it belongs in the fast world-free suite; if it is a call nobody makes — a result computed and never pushed, a wire between two correct halves — only an integration test entering through the player's own doors can reach it.* Integration tests enter only through player-shaped calls (`InspectAlongRay`, `ChoosePieceMenuRow`) and group by **world configuration**, not by assertion.
- Collapse tests must assert **no piece is `Stranded` at the moment it goes**, so a solver limitation cannot wear a collapse's clothes.

### The material × force matrix

One reusable parameterised test taking material + force type (+ connection type), grounded in published strengths, everything expressed as ratios of the **C30/37 structural-concrete baseline**. **Test behaviours, not materials**: wood gets added exactly once, to prove the directional code genuinely reads the profile (same shear scenario, wood survives where brick failed) — after that, any material is just numbers. Deliberately not added yet; adding it beside the calibration data would spend that proof for nothing. The compression-vs-shear capacity ratio check (one joint, two directions, assert the ~50× the mortar profile implies) is named "key validation" and still exists nowhere — queued.

### Fuzzing against an independent oracle — the most productive technique this project has

Two permanent seeded fuzzes in `Tests/StructureFuzzTest.cpp` share one generator shape and one support namespace; both print the failing seed and the whole structure so any case reproduces exactly. **`Structure.Fuzz`**: 12,000 random structures against universal properties (conservation, finiteness, supported-implies-routed, stranding never travelling downward) and an **independently derived oracle** — least-fixed-point support + Warshall closure where production uses BFS + Kahn, Jacobi relaxation where production accumulates. ~98 ms. **`Structure.CascadeFuzz`**: 8,000 breakable structures cascaded to a standstill; checks termination, latch-agrees-with-stamp, broken-carries-zero, no survivor over capacity, and — the sharpest property — **no joint broke that was not over capacity on the graph its own pass was solved on**, with later passes judged against the intermediate graph rebuilt from the break stamps. Floors its own generator (a fuzz that stops generating cascades fails rather than passing silently). ~100 ms.

Both found real solver defects that review-by-reading missed. Rules of the culture:

- **Extend the fuzzes; never rebuild a scratch harness.**
- **A green-on-arrival test is indistinguishable from one that asserts nothing until you mutate production.** Every property added to either fuzz is proven to bite by mutation first; the live mutation registry is in [TRAPS.md](TRAPS.md).
- **An invariant asserted over fixtures that all share a hidden property is not an invariant** ("gravity never pulls a joint open" was green because every fixture normal was axis-aligned; tilt one and 337 of 6,000 produce tension). When an invariant has never failed, check whether it *can*.
- The oracle transcribes production's normalisation **deliberately and exactly** — cascade-fuzz joints settle at utilisation exactly 1.0, so one ulp of drift is five spurious failures. Lockstep changes are marked at both ends.

### The acceptance suite, and its sync convention

`WALL_CASES.html` is the **source of truth** for the twenty wall cases — verdicts (*stands* / *local loss* / *collapse* — the middle verdict is the point), matched pairs each isolating one variable, and the per-row prose rulings. `Tests/WallAcceptanceTest.cpp` encodes it; the twenty are also playable levels whose captions are pinned to the verdicts, with the `THE MODEL CURRENTLY DISAGREES` marker *computed* by running each case (see [LEVELS.md](LEVELS.md)). **The catalogue and the test drifted twice in one day** (cases 8 and 16 corrected in the html, left stale in the test; found only by a full side-by-side sweep) — hence the standing convention: **every revised html row carries a `revised` date, and the matching test row names the same date**, making divergence grep-visible. Generating one from the other was rejected (the html's `falls` regions are drawing coordinates, known-unusable for case 20; the prose is where rulings live). A pair whose two halves answer identically discriminates nothing — move it onto a quantity that still separates them (13/14 and 15/16 both did this) or delete it; never leave it reading the right answer off two identical verdicts. The mechanism *ladders* (cover, span, corbel step, free-end height) live at the unit layer; the acceptance set is the real-world verdict oracle for configurations the unit layer has no fixture for.

### Running the suite

Commands, log-grepping, and the `NonNullRHI` visual runs are documented in [CLAUDE.md](../CLAUDE.md) and [TRAPS.md](TRAPS.md) (exit codes lie; results only in the log; `-nullrhi` filters visual tests out silently). Functional-test actors live only under `Content/Maps/FunctionalTests/` (cook-gated; the rule and the gated dependency form are in CLAUDE.md) — but no functional test exists yet: every world test builds its own world in code via `FTestWorldWrapper` and runs under the ordinary `DestructionGame` filter.

---

## 5. The physics model, as it stands

Everything in this section is a present-tense rule of the shipped solver. Hazards that were paid for are flagged inline — the recurring enemy in this codebase is **a wrong answer that looks plausible**: a joint reading a comfortable number while the piece it holds is not really being held.

### 5.1 How load reaches the ground

Pieces carry a mass and a **grounded** flag. Load flows **downward**: a piece transmits its own weight plus everything received from above into the connections that support it; a grounded piece absorbs what reaches it. Where several connections support a piece, the load **splits weighted by interface area**.

**Support is two-tiered.** A substantially vertical normal (within 45° of vertical) is a **bed joint**; a substantially horizontal one is a **head joint**. A piece's supports are its bed joints *beneath* it — and only if it has none does it fall back to its head joints. Consequences stated precisely:

- **A bed joint above a piece is not a support** — excluded from the bearing tier entirely, not bearing in tension.
- **The head tier is sign-blind**, so a steeply inclined face *above* a piece is its fallback support and takes genuine tension — physically right (a brick hanging off an inclined face is being pulled off it), and tension governs because mortar's tensile limit is ~1/100 of its compressive one. Characterised and accepted (`Structure.TiltedJointClassification`); **do not "fix" it**. The 45° line looks like a cliff and is not: just below, the joint bears nothing and the piece falls; just above, it becomes a support that fails in tension — same answer, different route.
- **Accumulation runs in dependency order (Kahn), never by distance to the ground** — a spanning brick and the brick on it are equidistant from the earth, yet one loads the other. Distance-based routing produced a keystone whose joints carried zero however much wall was piled on it, with the answer depending on bond pattern for no physical reason. The lesson generalises: classification was direction-aware from the start; **routing was blind, and routing decides where the load ends up**.
- **Supports that are themselves falling take none of the split** — excluded entirely, so the joint actually carrying the piece never under-reports (a joint at 1.9× reading 0.95× never breaks).
- **A piece is unsupported when it genuinely has no load path to the ground.** Cycles are **reported, not solved**: pieces caught in a knot (identified by their own load returning to them, not by mere un-orderability — un-orderability over-strands downward to the foundations) are conservatively `Stranded`; pieces beneath a knot keep their support; a piece resting *only* on a knot falls with it (stranding travels upward, never downward). `SolveLoads` iterates to a fixpoint because stranding changes who reaches the ground. The four states are `Grounded` / `Supported` / `Stranded` / `Falling`, with `Falling` deliberately the zero enumerator — an absent answer must read "nothing holds this up", never "resting on the earth". The loop-division rule for genuine cycles (a true voussoir arch) is **still absent**; arching (§5.4) closed its cases by *avoiding* the loop, not by supplying the rule.
- Non-unit normals are stored as given (normalised at use); anything comparing normals must normalise first.

**Solving is non-destructive.** `SolveLoads` computes what a structure carries and breaks nothing, so readouts and what-ifs can ask without damaging. `NumSolves()` is a pure observability counter (it exists so "the batch solves once" is falsifiable).

### 5.2 Strength: Mohr-Coulomb, and the truncated envelope

```
shear capacity = cohesion + μ × compressive stress     (capped at MaxShearStrengthMPa)
```

Only **compression** buys friction — a joint pulled open gets none. Compression and tension keep independent limits. Why it earns its place: it is why dry-stone stands at all (c = 0, pure friction); it makes damage progressive (cohesion once cracked is gone, friction remains — why damaged masonry stands rather than unzipping); and it compounds collapse (removing weight above *lowers* the capacity of the joints below, a second cascade mechanism beside redistribution). **μ = 0 reduces exactly to independent axes** — the right answer for mechanical fasteners, and what keeps connection types data rather than code paths. Honest scope: for a low mortared wall the friction term is a few percent; it is first-order in tall structures, dry stone, and any joint whose bond has broken. Without the cap, base joints of tall structures become uncuttable — unphysical, and backwards for a demolition game.

### 5.3 Moments: eccentric loads open joints

A joint's utilisation is no longer one averaged stress. With the joint rectangle and piece centres of mass:

```
σ_n = (Tension − Compression) / A          signed, positive in tension
σ_b = |M_u|/W_u + |M_v|/W_v                worst-corner biaxial,  W_u = (4/3)·h_u·h_v²  (≡ bd²/6)
peak tension     = max(0, σ_n + σ_b)
peak compression = max(0, σ_b − σ_n)
```

- **Fail-closed**: a non-zero moment against a zero modulus returns `Max()` (moment checked first so 0/0 never happens).
- **Friction keeps using the mean compressive stress**, never the peak: it can never make a joint look stronger than before, EN 1996-1-1 §6.2 defines `f_vk = f_vk0 + 0.4·σ_d` with σ_d an average, and the refinement is its own slice.
- **The statics: a piece on several supports is indeterminate; determinate on exactly one.** N = 1: `M_j = (p − c_j) × F_tot + Σ M_received`, exact. **N ≥ 2: keep the area split and zero the moment** — exact when the centre of mass sits at the area-weighted support centroid (every symmetric running bond, which is why an intact wall does not move by one bit), unconservative otherwise, and recorded as such. The naive per-joint rule (`M_j = (p − c_j) × S_j`) makes every bed joint in a *standing* wall read 0.029 in tension — half-peeled everywhere — because the moments cancel across the pair but not on either joint. N = 1 is where the entire visible symptom lives: head-joint fallback with one neighbour, and any piece that lost one of two seats.
- The moment travels as a **vector along the load path** (`ReceivedMomentUuCm`), never recomputed per joint from a scalar — that is what makes a future applied force at a point (`r × F`) strictly additive.
- **What was deliberately not done, each for a reason**: no cracked-section model (our mortar has tensile strength, so the joint reaches `f_xk1` *before* a crack propagates — the uncracked formula is valid right up to the break); no `f_xk1`/`f_xk2` split (`f_xk2` is a wallette property that already contains bond interlock, which our graph represents explicitly — using it per head joint double-counts the bond); no torsion (second-order for gravity on rectangular joints); no oriented joint frames (every producible joint is an axis-aligned rectangle); and the solver may know where its own joints are but **may never be asked what is near a point** — spatial queries belong above it (force delivery's problem, §7).
- Gravity now produces tension **routinely** — any comment or assertion scoped on "axis-aligned normals never produce tension" is scoped on *geometry-free fixtures*, not on axis-alignment.

### 5.4 Arching: the load path, not the joint

Moments made cantilevers peel correctly, which exposed the larger error: the model only ever routed load *downward*, so a brick that lost a seat was asked to hang — the one thing masonry cannot do. One deletion walked a 33.69° stepping failure across the wall (the bond pattern: half a cell per course, stopping when the remaining column is under 17.18 brick weights). Five mechanisms close it, none adding a material property or per-material branch; the only new constant is 0.866 (§3).

**(1) The moment cap.** A `BedBeneath` joint may develop arching relief when **all four gates** hold: complete geometry; the normal force is compressive (no compression → no thrust line); the resultant is outside the kern (`e > h/3` — inside it, nothing is opening); and there is an **intact head joint on the eccentric side** to a neighbour that is `Supported`/`Grounded` and does not count this piece among its supports. The relief caps the whole moment vector:

```
k = min(1, |σ_n| / σ_b);   M ← k·M      →  at the cap: peak tension = 0, peak compression = 2|σ_n| exactly
```

The thrust line sits on the kern edge, which is what an arch *is*. The capped value is what travels ("what travels is what the joint reads — there is no second, private quantity"). Hazards, each measured: an **ungated** cap deletes MOMENTS case (b)'s moment outright (a head joint has σ_n = 0 exactly) and un-condemns the staircase by a factor of 19; capping to `M = 0` instead is wrong by exactly 2× on the compression axis; the abutment must be on the **eccentric** side or the free end and the ragged end break; and utilisation becomes **non-monotonic in load** (adding centred load *reduces* tension — real pre-compression, safe for the cascade because the latch is irreversible, but it will look wrong on a readout).

**(2) Spanning.** A hole wider than one brick leaves seatless pieces; the naive fix ("supported by the neighbour through the head joint") is a two-node cycle and turns "the wall bridges the hole" into "the wall falls, unroutably" — a worse answer through correct-looking physics. Instead `ReseatSpannedGroups` collects the contiguous seatless run through intact head joints, requires a seated abutment on **both** sides of its centre, and re-seats each member toward an abutment — **acyclic by construction**. Load is conserved to the last bit (the intact columns' sum leaves through the two surviving seats). A re-seated piece is **indeterminate**: its remaining head joint is the bookkeeping route for the vertical share and carries force only — treated as determinate it would carry its whole column across 11.25 cm and snap the joint just granted.

**(3) The thrust.** An arch pushes sideways; the springing carries it in **shear on its own bed joint** against `c + μ·σ_n` — no new axis, no new data. Applied *outside* the fixpoint once accumulation settles (which keeps every vertical answer bit-identical), as **one number per arch** pushed equally and oppositely at both ends:

```
H = W·L/(8r),  r = d_e/3  (kern-limited rise),  V = W/2         ΣH = 0 is structural — one direction, two signs
```

The kern-limited rise is 3× stricter than Heyman's hinge limit and is the choice consistent with the uncracked-section model — it is also the **only** choice under which a finite span limit exists at all. **Sliding at the springing is what governs** (compression needs ~1.5 km of span; the thrust line is inside the kern by construction). Emergent, not tuned: deeper burial widens the limit (friction grows with cover), and **dry stone can never flat-arch** — c = 0 against a demand ratio ≥ 0.866 fails at every span (reads 1.237215440448697 = 0.866/0.7 identically at every width), with no per-material branch saying so.

**(4) The cover cap.** `d_e = min(cover actually found, 0.866·L)`, the cover measured by a bounded upward walk over intact bed joints (never a spatial query), at the **abutments**, reduced to the thinner of the two ends (fail-closed: an arch is only as good as its shallower haunch). As cover thins, `H/V = 3L/(4·d_e)` blows up and the thrust check fails — an opening near the top of a wall cannot arch under nothing. The depth is carried as **`d_e/L`**, not `d_e`, so the angle-governed branch is character-for-character the constant-ratio form and IEEE non-cancellation cannot move it. The load is the whole column the solver accumulated, **not** BS 5977's triangle — strictly harsher, no dispersion angle becomes data, and in the regime where the limit bites the cover caps `d_e` anyway.

**(5) Composite vertical action** — §5.5.

**Interaction fine print.** The thrust is *applied* only where a group was *spanned* (a hole wider than one brick), and there the springing's own shear axis is what measures it. A one-cell hole is the moment cap's case: no load crosses it, no arch is recorded, and its springing carries exactly zero shear — so **since 2026-08-09 the one-cell relief is capacity-checked and withheld rather than pushed** (the former fail-open gate, §7 item 4). The cap deletes a couple `dM = (1 − k)·|M|`; the only thing on the half-seated brick's free body that can supply it is a horizontal pair — a push out through the intact head joint gate four already insists on, with its equal and opposite reaction as **sliding in the seat's own bed plane** — so the seat must afford `dM/z` against `c + μ·σ_n`, with **z the head joint's centroid above the bed plane, 3.75 cm** for this brick and joint. The ratio is bond geometry alone, the load cancelling out exactly as it does in the spanned case's `3L/(4·d_e)`: `(e − h/6)/z = 1.0444` for a running-bond half seat, which general-purpose mortar affords at **0.305** of capacity and lime at **0.520**, while **dry stone reads 1.4921 at every load and every wall height** — the one-cell twin of the spanned case's 0.866/0.7 — and is refused, with no per-material branch saying so. **Withheld rather than delivered, deliberately**: pushing it in as real shear would move every *earned* one-cell arch on an axis that reads zero today (the 114.63 anchor by ~21×), and belongs with the post-LP re-anchor. The residue is gap 6's, not this one's — nothing loads the abutment with the push it is credited for supplying. Moment is not conserved at an arched joint (the missing couple belongs to a neighbour the thrust loads only in the spanned case) — recorded honesty, same shape as the N ≥ 2 rule.

### 5.5 Composite depth: how far up a wall deep-beam action reaches

A corbelled bed joint resists its overturning moment with the **composite vertical section of bonded masonry standing over it**, not its own 179.48 cm³ bed patch — this is the user's free-end ruling (§8) made mechanism, and it is what lets a bonded wall shrug off a free-end deletion (factor ~62 on the staircase: 11,627 cm³ vs 179.48).

```
W_c = t·D²/6         t from the joint's own half-extents
D   = min( masonry above the joint , max( the corbelling body's own depth , λ·|M|/|F| ) )
```

- **The joint gives at the lesser of two limit states, and both edges move together**: `peak tension = min(patch tension, M/W_c)`, and where the composite governs the squeezed edge falls to `max(M/W_c, |σ_n|)` — a patch that is not resisting the moment is not bending on either edge. **Relieving only the tension edge masks the whole mechanism** (a 45-course corbel then fails in *compression* at 13.69 instead of tension at 1.25, on an axis nobody measures). The discarded case keeps the patch's value *verbatim* — `(4/3)·h_u·h_v²` and `t·D²/6` at D = t are one ulp apart, and a recompute moves anchors for no chosen reason.
- **`M/F` is the statical lever arm, measured not assumed** — it is "the overhang", computed (the staircase's bottom rung: 1608.75/38.5 = 41.79 cm, agreeing with the composite wedge's 41.25 cm centroid to 1.3%, as statics requires). The clear projection does **not** work in its place (infeasible: the free end needs λ ≥ 2.436 and the scenario ≤ 0.895). Scale-covariant: every dimension × k gives e ~ k, D ~ k, σ ~ k — Galileo's square-cube law, correct rather than a defect.
- **The floor is the cut**: the corbelling courses *generate* the moment, are bonded into one cantilevering body, and need no shear transfer to be engaged — they resist with full depth unconditionally; masonry **above** the cut has to be dragged in by shear over a distance, which is what `λ·e` bounds. The floor can never credit a course of the wall above the cut. Without the floor the ladder went **U-shaped** — the 3.25 cm code-compliant step read *worse* than a 5.375 cm one, penalising exactly the geometries closest to compliant.
- **`λ·e` must clamp the result, not merely limit the walk** — the walk stops at whole courses and overshoots by up to one, 1.7% permissive with nothing able to see it.
- **The moment is untouched.** Composite action changes the SECTION, never what the wall hands down. A moment scale is the tempting one-line edit; it reads the ladder 18× low and relieves every joint below by an underived factor.
- **A composite of one is not a composite** — a piece with nothing resting on it keeps its own patch (changes no corbel row, but without the gate three unrelated readout fixtures moved).
- **Two lemmas asserted during design were false and tests caught both** — keep the corrected forms, not the lemmas: the *half-seat lemma* ("every increment arrives eccentric" — a **centred** increment grows F without M, so e can be as low as 3.75 cm and shallow joints *can* be capped); and the *matched-corbel lemma* ("λ > 2 never fires on a matched corbel" — silently assumed the half-cell step; the true condition is step < 22.5/λ = **6.4954 cm, exactly 2.00× the published per-course limit**, k cancelling).
- **Rejected bounds, with the numbers**: shear flow `q = VQ/I` is a **floor, not a ceiling** (`τ_max ∝ 1/D`, binding at mid-depth and zero at both extreme fibres; mortar never binds by 7.8×, reading 0.1278 of capacity; solving gives a *minimum* D ≥ 39.06 cm) — kept as a future *eligibility gate*, where it condemns dry stone at every depth from two existing fields; "apply the shear and cascade" is the honest physics and the eventual direction, but is bimodal (breaking a connector *raises* demand on survivors) and needs a "slipped but still closed" joint state the model's giving-is-total rule forbids; "the cut's own height alone" leaves a 57% empty window and reads a single deleted brick at 6.05.
- **Consequence to know**: the free end's margin under λ is ~2×, not 62× — the reading is linear in wall height, crossing 1.0 at ~61 courses (λ = 3.464). Live behaviour, arguably correct physics, stated rather than discovered.
- **⚠ Open hazard — the `h_body` predicate.** "Seated on exactly one course" is true of every piece of a stack-bond column, where the walk would run to the top and the floor would become the whole wall. The direction test that refuses it was built and reverted: **byte-identical suite output**, because stack bond has zero eccentricity at every seat, so no fixture can punish either spelling. Three plausible predicate spellings agree on every fixture the project owns — exactly the condition under which this suite has been wrong before. The distinguishing fixture is a **stack-bond or single-wythe column loaded eccentrically**; until one exists, the direction test is uncovered capability. Related open question: the section credits courses that do not cross the plane they resist across (a stepped corbel of single bricks has ~1.9 courses intersecting any vertical plane, credited its whole height) — under investigation, and it may subsume the missing stability check (§7 item 1) rather than needing one.

### 5.6 How a structure comes apart, and what removal means

**The cascade** *(decided 2026-07-31)*: solve; break **every** joint over its own capacity in the same pass; stamp each with the pass number; re-solve so the share moves onto the neighbours; repeat until a pass breaks nothing (the last pass is not counted). Worst-joint-first was rejected (invents a sequence where none exists — three equal independent overloads become passes 1, 2, 3); unordered all-at-once was rejected (loses the sequence, and losing the sequence loses the collapse — the stamps are what a visualisation plays back). Termination is structural: joints never heal, so every counted pass removes at least one connection.

- **A broken joint leaves the support relation entirely** — the tier, the walk, the paths and the split, together. Half-right is the trap: a broken joint that still *wins* the bearing tier leaves the piece above with an empty support list, reported falling beside a perfectly good head joint reading zero — self-consistent, and a wall that collapses where it should have leaned.
- **Pass numbers belong to the structure, not the call.** A later cascade continues from the highest stamp (restarting would replay a collapse in the wrong order with nothing reading as inconsistent); how many passes *this call* broke in is the separate per-call return.

**Removal** *(decided 2026-08-01)* is the player's move and is not failure. A removed piece is not in the structure at all: every joint that held it is **severed** (latched without a stamp), and a grounded piece **stops being ground** — the root-collection guard on `bIsInTheStructure` is the one line of removal that is not free, because a grounded piece seeds the reachability walk on its own account. What follows is an ordinary cascade on a graph with a hole in it. The three states need no sentinel:

| | still in the structure | broke in pass |
|---|---|---|
| intact | yes | — |
| went with a removed piece | no | — |
| failed under load in pass N | no | N ≥ 1 |

**Handles are stable across removal — a model-level promise.** Removal tombstones the slot; nothing renumbers, because the break stamps cannot be recomputed from anything and renumbering would silently re-point every joint above the hole. Slot reuse would need generational handles, not a free list.

### 5.7 Overturning: the reduction, and where it stops

**On an immovable base, global overturning IS the bottom bed joint opening** — a rigid body cannot rotate about a fixed base without separating from it, and the model evaluates every joint, so it implicitly checks every candidate free body. The capability missing is therefore not a mechanism but an *equilibrium discipline*: `ComputeUtilisation` happily reports a confident number for a joint on which **no equilibrium solution exists** (corbel A: the resultant sits 22.5 cm out on a bearing whose outer edge is 5.125 cm out — reads 0.156 and stands). Recording that state (`Stranded`'s precedent) is currently refused because **the resultant is outside the bearing on every corbel the user ruled must stand** — the predicate would condemn the ruling.

**Since 2026-08-09 the discipline is enforced on exactly one topology**: `FStructure::BreakOverturnedBodies` (evolution step 2, the interim guard) severs the bearing of a bonded body whose *sole* connection to the rest of the structure is one bed-joint bridge, when the body's weight past the bearing edge exceeds what standing weight plus a guard-local **mean-basis** bond (`SolverInterimOverturningMeanBondMPa = 0.6` — mean because the coded characteristic 0.10 would condemn both a standing control and corbel A against the §8 ruling) can restore. Everything with a second load path — walls, filled corbels, beams, spanned holes — is deliberately outside its reach, so off-bridge the old sentence still holds: **a bonded corbel standing is the model declining to enforce statical equilibrium at the bearing** (a *bare arm* is a chain of bridges and ≥9 single-brick steps would now fire the guard; the filled corbels the rulings name are untouched). The guard is disposable by design — one method, one constant, one call site, deleted at evolution step 4 when the LP makes equilibrium the authority everywhere. The reduction is valid *only* while every load path terminates at a grounded piece — debris and free-standing assemblies are outside it; do not quote it as a general truth once rubble can bear.

---

## 6. Anchors — the worked numbers the tests pin

Each of these is pinned by a test, most with exact `==`. **Re-derive, never re-tune**; when quoting a wall-dependent figure, *name the wall* — two fixtures one digit apart in description can be a factor of two apart in answer. These all move together in the deliberate mean-strength re-anchor pass (§3), and not before.

| Anchor | Value | Where |
|---|---|---|
| Intact 30 × 40 scenario wall, worst joint | **0.00495** | `StructurePushTest` |
| Intact 7 × 30 flush wall, worst joint | **0.0036748258197270385** | arching guard rows |
| Intact 12 × 30 acceptance wall, worst joint | 0.00368054 | unasserted, quoted in catalogue |
| Half-seat, per brick weight carried | **0.058203838191552663** (limit 17.181 bw = 45,825 uu) | waist / ragged-end fixtures |
| One hanging brick (head joint, case (b)) | **0.4157273077**; chain of two 0.8315 | `Structure.HangingBrickPeelsRatherThanShears` |
| One-cell hole, arched half-seat | 0.0141885 / 0.0141946 (design 0.0142166; ratio to un-arched **114.63**, load-independent) | `StructureArchingTest` (derived identity at 1e-12, literals as 2% cross-check) |
| One-cell springing thrust, demand over sliding capacity | mortar **0.30547**, lime **0.51973**, dry stone **1.4920634920634921** exactly (= 1.0444444/0.7, load- and height-independent) at H/V = (5.625 − 1.7083333)/3.75. The mortar/lime figures are *measured* at the wall's real 27.94-bw seat load; the test header's at-28-bw hand figures (0.30596/0.52045) are the same truth at the round load — reconcile through the load, don't "fix" either | `AOneCellArchMustEarnItsThrust` (zero-cohesion row pinned as an identity at 1e-12, the other two to 2%) |
| 3-cell spanned hole, springings | 0.0283583 / 0.0283831; conservation **298069.72268342471 uu** to 1e-9; re-seat head joint **0.55935518961841602** | `StructureSpannedHoleTest`, `StructureThrustTest` |
| 20-cell opening H/V | **1.18424** vs asserted 3L/(4·d_e) = 1.1842 | `StructureThrustTest` |
| Dry-stone springing, any span | **1.237215440448697** = 0.866/0.7 | `StructureThrustTest` |
| Staircase corbel bottom rung | **0.36903147272727271** (was 22.929528199727653 pre-composite) | `StructureCompositeDepthTest` |
| Corbel family roots A/B/C/D/E35/E36 | 0.15612870000000001 / 0.19516087500000001 / 0.34481267346093736 / 0.34348314000000008 / 0.99046165225599581 / **1.0164705641576046** | `Core.Corbel.LaysTheJointsTheGridImplies`, `World.Scenarios.CorbelRows` |
| E35/E36 crossover; C-vs-D crossover | **36 steps**, C and D identically — the counterweight buys nothing (downward-only routing) | `CorbelStepsBeforeTensionWins` (deliberate red as a finding) |
| Hundred-step corbel F | 2.68132, 2,981 of 3,015 pieces lost — **no absolute anchor in the suite** (open item) | `AHundredStepCorbelMustComeDown` |
| Scenario corbel rung under the λ bound | **0.44942108329043645**, ratio 1.217839443256848 (was 0.22300137936935951 unbounded) | `StructureCompositeDepthTest` |
| Free-end ladder (10/20/30/40/50 courses) | 0.3283 / 0.5011 / **0.6740** / 0.8469 / 1.0198 — crosses 1.0 ≈ 61 courses | `StructureFreeEndHeightTest` |
| `MortarRaggedWorstAsBuilt` | 0.0455104479 (moved from 0.0390321745 when the half-seat lemma fell; the test pins the measured value and is **green** — what is still owed is the independent hand derivation of it) | `StructurePushTest` |
| Acceptance case 11 worst joint | **0.362193** at `c3/8.5-c4/8` (the toothed reveal corner, not midspan) | `WallAcceptanceTest` |
| Case 13 / case 14 corbel readings | 0.0702368 / **0.195160875** (× 2.78 — the projection term, asserted since the outcomes no longer separate) | `CorbelProjectionIsReadInTheJointNotInTheOutcome` |
| Case 15 / case 16 header readings | 0.00184437 / 0.058203838191552663 (× 31.6, floor of 10 asserted) | `SuperimposedLoadIsReadInTheJointNotInTheOutcome` |
| Case 18 stack-bond figure | correct per-pair figure **0.01000825** (against `f_vk0`, not `f_xk1`); the model instead reads n/2 × 0.0200165, height-**linear** | `StackBondColumnShearIsHeightIndependent` (deliberate red) |
| Beam pair: bearings | 932,215.2 × 2 = **1,864,430.4 uu**, exactly the total weight (green anchor) | `BeamAcceptanceTest` |
| Beam pair: midspan | derived M = 139,288,968 uu·cm → 83.57 MPa = **3.48×** C24's f_m,k; the solver reads **|M| = 0** (deliberate red). **Since 2026-08-09 (the one-cell thrust gate) all three rows also FALL whole in production** — the dry bearings lost the kern cap and read Max(); the pre-gate ~0.31/~0.34-and-stand readings are historical. The LP oracle stands all three (λ* 1.76/19.2/17.4); flagged user ruling in CURRENT_STATE | `BeamAcceptanceTest`, `RigidBlockOracleSweepTest` |
| Layout spanning brick | 1333.5993125 uu/side, utilisation **1.2693390e-4**; centroid-normal error factor **41.5** | `Layout.SpanningBrick` |
| Brick mass | **2.72163125 kg** exactly, density-first order (volume-first is one ulp low) | `Layout.PieceMass` |

---

## 7. What the model cannot do yet — and the path

From the 2026-08-08 full review (three passes: code-level verification of every conversion and guard, full acceptance-suite evaluation, fresh research against published data). The stress pipeline is physically correct and verified end to end; **the routing layer is where realism is bought on credit** — a stack of masonry-specific heuristics (tiers, downward accumulation, arching groups, composite depth) each patching the hole the previous one exposed. Six of the known-wrong verdicts trace to **one missing mechanism: per-piece equilibrium (force and moment balance)**, and the same absence blocks wood, steel, and sideways force.

### The gaps, ranked by severity

1. **No global equilibrium — statically impossible states read as safe.** A rigid body past its tipping point is inexpressible to the *joint* checks (§5.7); since 2026-08-09 the interim guard catches exactly the bridge topology (a body hanging on one bed joint), and everything else — anything with a second load path — still reads safe past tipping. Behind the corbel ruling's cost and partially 10/19/20. (Case 12's pier was thought to sit here too, until the 2026-08-09 re-ruling worked the numbers — see §8.)
2. **Load routes only downward; tension support does not exist.** Nothing can hang: counterweights buy nothing (the C-vs-D finding), buttresses do nothing, and the nail/screw withdrawal capacities in `ConnectionProfiles.cpp` are dead data with no reachable code path.
3. **Multi-support pieces carry zero moment** (the N ≥ 2 rule). Exact for symmetric running bond; a lintel or beam on two walls can never fail in midspan bending — the beam pair's red.
4. ~~**The one-cell arching moment cap is granted fail-open**~~ — **closed 2026-08-09** (§5.4's fine print): the relief is now earned, the springing's sliding capacity being asked whether it can deliver `dM/z` before the cap is applied, and dry stone is refused it at 1.4921 with no per-material branch. Driven by `Core.Structure.AOneCellArchMustEarnItsThrust`. **What remains is not this gap**: the push is checked, never delivered, so the abutment is still never loaded by it — that absence is item 6.
5. **λ = 3.464 is a ruling wearing a constant's clothes**, and the deep beam's horizontal shear flow is never applied as a demand on the bed joints that must carry it.
6. **Arch thrust neither walks down the pier nor overturns it** — the check simply does not exist; same absence as item 1. The gap no longer has an acceptance case condemning it: the 2026-08-09 case-12 ruling (§8) found that at **self-weight** no pier in this family honestly fails while its span survives — the restoring side is dominated by the bonded bed joint at mean strength, a *constant* term that alone covers even the kern-limited thrust, with the standing weight on top (4–8× margin even on a one-cell pier). The honest discriminators are the **leaning stack** (review-queue item, the fixture that fails by stability alone) and an **eccentric surcharge** driving thrust past what self-weight can produce; BIA TN 31's shove-them-over warning is about loaded lintel bearings, not bare walls.
7. **Pieces never fail — only joints do.** Masked today because mortar is weaker than brick *by data*; fatal for wood, where member bending is the primary mode.

**Wood and steel do not work under the current routing, and the fix is known.** The current data model — rigid pieces with mass and centroid; rectangular joints with position, extent, and a Mohr-Coulomb / no-tension / crushing strength triple — is **exactly the input to rigid-block limit analysis** (Livesley 1978; Gilbert; Lourenço's simplified micro-model, essentially `FConnection`). Per-piece equilibrium with joint forces constrained by the existing strength surface, solved as a small sparse LP, **subsumes** gaps 1–6 with kern behaviour *emergent*, handles cycles (no accumulation order to defeat), and removes the "beneath" assumption entirely — arbitrary load direction stops being special. It stays deterministic, world-free, fast at scenario scale (~1,220 pieces / ~3,520 joints is a small LP), and needs only the strengths the profiles already carry.

### The evolution path (ordered; user approved 2026-08-08)

1. ~~Close the fail-open one-cell arching gate (apply or capacity-check the thrust)~~ — **built 2026-08-09**, by capacity-checking and withholding rather than applying, so every earned arch stayed bit-identical (§5.4's fine print records why that choice, and what it defers to step 4).
2. **Interim overturning guard** — **built 2026-08-09** (`FStructure::BreakOverturnedBodies`, driven red-first by the leaning-stack acceptance case; §5.7 records its scope and constant). Explicitly disposable: one method, one constant, one call site, deleted at step 4.
3. **Build a 2D rigid-block LP as a test oracle only** — **built 2026-08-09/11** (`Tests/RigidBlockOracle.*` core + `RigidBlockOracleSweepTest` sweep). What the measurements said: every §8 corbel ruling upheld by the limit theorem at characteristic bond (the counterweight priced at 22.6× where production reads 0.4%); composite depth cross-validated to 0.03% on the 13/14 pair; the beam rows stand at λ* 1.76/19.2/17.4 with 9.8× material discrimination; wall-08's catalogue verdict was the outlier of three methods (λ* 324.7 — **ruled STANDS 2026-08-11**, §8); the leaning stack's 130× true-margin swing confirmed behind production's height-flat 0.1388. **The sparse rewrite landed 2026-08-12** (sparse revised simplex, LU basis + eta file, clean refactorisation every 64 pivots — the error reset; independently verified in review against an exact-arithmetic reference): every fixture the dense solver refused now answers with verified residuals (corbel D 21× faster; the envelope canary flipped and was promoted at λ* 256.82), and the measurements it unblocked — rows 9/10/19/20, the 11/12 pier pair (now discriminating 1.44× where production reads 0.03%), the 7v8 cover pair, the λ ladder (free-end λ* height-independent to nine digits) — are measured in `RigidBlockOracleSweepTest`'s header and await pinning in the classification slice. Still beyond the practical envelope: the 30-course walls (representable, but full-Dantzig pricing over ~34k columns is the measured driver — partial pricing is the lever), corbels E35/E36, and corbel F.
4. **Promote equilibrium to the cascade authority**; keep the two-tier router as a warm start; delete the arching-group and composite-depth heuristics once the diff reproduces the intended anchors.
5. **Signed tension support** falls out of 4 → hangers, buttresses, counterweights, fastener withdrawal data goes live.
6. **Member failure** (piece-level bending/crushing against `FMaterialProfile`) — the wood prerequisite; the section machinery exists.
7. **Arbitrary-direction force** (explosions, impacts) — nearly free after 4; the gravity-specific router was the admitted blocker. Until then: the MVP brings walls down by **removal** and by **downward load**, both of which the model already supports; sideways force is deferred *structurally*, not as scope-trimming.

The mean-strength re-anchor (§3) runs after step 3. The other spatial threads — visual break patterns, secondary debris damage (Chaos resolves the collision but nothing tells the solver), performance at full-building scale — remain open design threads; debris damage needs an impulse channel from Chaos into the graph and joins force delivery on the far side of step 4.

---

## 8. Decisions record — the rulings no arithmetic settles

Kept because they are load-bearing and because several cost something that must stay visible.

- **2026-08-06 — a brick deleted at a free end must not bring a wall down** *(user)*. The free end and the raking corbel are locally indistinguishable, so any rule local enough to save one saves the other: the ruling adopted **composite vertical action** and inverted the two staircase tests (re-derived from the composite modulus, not re-tuned). Internally consistent evidence: the user independently ruled case 20 *local loss* — the seatless teeth drop (`c3/4.5`, `c5/2.5`), the wedge stands at 0.369 — two judgements at different times landing on one mechanism.
- **2026-08-07 — a bonded corbel stands however far it steps** (case 14; *user*). **The recorded cost**: there is now no way to express a corbel failing by projecting too far. That failure is global overturning of the corbelled mass about the wall face — a stability check the model has never had (§5.7 states the true price: equilibrium is not enforced at the bearing). No joint-level threshold can recover it: 0.195 (case 14) sits between the five-step raking corbel that must stand (0.219) and everything below. Not built, chosen knowingly, kept visible. Whoever picks it up needs: the assembly rule, the pivot (outer face of the bearing course), and the fixture family (13/14 are two rows of it).
- **2026-08-08 — case 11 ruled twice; the second ruling is the keeper: it STANDS.** First ruled local-loss on BS 5977's 300-mm-above-apex gate; then the user directed that **published design rules are not collapse predictors and must not be treated as gospel — evaluate the physics honestly.** Worked honestly: the ~120 kg panel rides a 60-cm-deep bonded deep beam (span/depth ≈ 2.3) at ~0.03–0.04 MPa of bending — 5–10% of mean bond strength — and the 66-cm piers take the thrust easily. The model agrees (0.362193, at the reveal corner). That direction is a **standing instruction** for every future verdict. (Case 8 — one course of cover — was thought to remain the genuine "no room to arch" discriminator; the 2026-08-11 re-ruling below retired that too.)
- **2026-08-08 — strength basis: mean values, for realism** *(user)*. See §3; re-anchor after the LP oracle, in one verified pass.
- **2026-08-08 — case 12's rewrite approved** (6-cell span on 1-cell piers with a survivor region, isolating pier width; the encoded case varies span *and* pier at once and near-duplicates case 9). Built 2026-08-09 — see the next entry for what the arithmetic did to the approval's own expectation.
- **2026-08-09 — rewritten case 12 ruled STANDS; the approval's "survivor region" is moot.** Case 11's exact span and cover carried on a single one-cell pier (cut `{0,3,0.75,6.25}`, 22 bricks). Worked per the case-11 standing instruction: minimum thrust from the panel placing its line in its own 60-cm depth is H = W·L/(8r) ≈ 400 N; hinge demand at the course-0/1 bed ≈ 88 N·m; restoring from the 550–1,050 N standing on the pier plus the bonded bed at mean strength is 375–745 N·m — **4–8× margin**, and still 1.7–3.4× against the solver's deliberately harsher kern-limited thrust. Sliding never governs. Reality check: a 1.2–1.4 m garden-wall opening on a single 215 mm jamb is common construction. The model agrees (0.362067) — so case 12 **left the known-red list by an expectation moving, not a solver fix**, and there is no survivor region because there is no collapse. **The corollary is the load-bearing part**: at self-weight, *no* fixture in this family can honestly fail a pier while its span survives — and the mechanism to credit is the **bonded bed joint's constant 316–632 N·m**, which covers even the kern demand with no standing weight at all (verified at the family's extreme, a ten-cell span on a one-cell pier, still ~1.7–2.9×) — so §7 gaps 1/6 get their red test from the leaning stack or a surcharge, never from a bare pier. Whoever designs the leaning-stack case must account for that bond term: a fixture meant to fail by stability alone has to put the resultant far enough out (or choose its joints weak enough) that bond tension cannot quietly rescue it. One asymmetric pier (not two symmetric ones) because a symmetric failure leaves nothing nameable — old case 12's exact weakness. Cost kept visible: the pier-width *pair* (11 vs 12) now has no measurable discrimination — the solver reads them 0.03% apart because it carries thrust as springing shear with no pier-width term; the LP oracle owns measuring that.
- **2026-08-08 — leaning stack + interim overturning guard before the LP oracle: yes** *(user)*. The guard is interim because it is a bolt-on second referee beside the joint checks; the LP subsumes it (a body past balance simply has no equilibrium solution), and it is built to be deleted at step 4.
- **2026-08-08 — the wood/steel beam pair exists now, as three deliberate reds** *(user, superseding "not worth writing until steps 5–6")*. A simply-supported beam laid as two collinear segments whose glue-line joint carries the member material's own published strengths (EN 338 C24, EN 10025-2 S275) makes member failure expressible with no new production API. Two segments and not five — more leaves middles seatless, the arch fires at H/V = 7.2 against dry stone's μ = 0.7, and the "wood falls" row passes for the wrong reason; an even count is also what puts a glue line at exact midspan. Row 1's outcome half needs global equilibrium as well as member failure (a parted half-beam still seated on its pier has nothing saying it pivots off).
- **2026-08-08 — `CODE_TOUR.md` and `html/destruction-explainer.html` deleted intentionally**; references cleaned up. *(2026-08-08, this consolidation: the plans folder rebuilt around DESIGN / TRAPS / CURRENT_STATE / LEVELS + the three load-bearing HTML catalogues; superseded design docs deleted, preserved in git.)*
- **2026-08-11 — case 8 re-ruled STANDS** *(user)*. The catalogue's LOCAL LOSS (two seatless bricks drop) was the outlier of three methods: production drops nothing (worst 0.218869) and the rigid-block LP stands the fixture at **λ* = 324.73** — still 18.0 after discounting /3 for plastic-vs-first-crack and /6 as a characteristic-vs-mean budget at once. The mechanism is a flat arch: the single coverless course jams into the toothed jambs in head-joint compression, and the two bare bricks hang on their head joints (this profile's compressive capacity is 100× its tensile — 10.0/0.1 MPa — so four bricks of self-weight is a rounding error). **Two recorded costs**: (1) the catalogue now has *no* case that refuses arching for lack of cover — cases 9/10 refuse it for span and abutment; a replacement cover discriminator that starves the *abutment* is on the wanted list; (2) the 7v8 matched pair left MatchedPairs entirely rather than moving onto readings, because production reads case 7 *worse* than case 8 at the same joint (0.269 vs 0.219 — a downward router reads cover as load, never capacity), and pinning that would encode the defect as a discrimination. The cover pair's honest measurement arrived with the 2026-08-12 sparse rewrite — wall-07 answers at λ* = 296.2 vs wall-08's 324.7 — and awaits pinning in the classification slice.
- **2026-08-11 — beams-unzip-on-dry-bearings accepted as a known cost until evolution step 4** *(user)*. The one-cell thrust gate (2026-08-09, correct for its target) strips the beam fixtures' dry bearings of the kern cap; the uncapped eccentric moment on a zero-tension joint reads Max() and all three beam rows fall whole in production — including the two light rows whose real-world verdict is STANDS, and which stood at ~0.31/~0.34 before the gate. The LP oracle stands all three (λ* 1.76/19.2/17.4, 9.8× material discrimination), so the falling is the missing global equilibrium, not the fixture's physics. Chosen over re-bearing the fixture in mortar (which would change its dry-friction premise) and over scoping the gate away from bearings (riskier than the cost). **The enforcement**: `FBeamCase::DropsToday`/`PassesToday` pin production's wrong answer (3 fallen / 1 pass per row) inside the already-red beam tests, so the next behaviour change inside those reds fails loudly — the two-day invisibility of this very change is why the pins exist. Both pins and this entry are deleted at step 4 when the rows go green.
- Earlier structural decisions, dated in place through this document: 1 uu = 1 cm and Mohr-Coulomb (2026-07-30); kinematic-while-intact, the routing and cascade rules, the C30/37 baseline and fastener smearing (2026-07-31); removal semantics and sideways-force deferral (2026-08-01); geometry-above-the-solver (2026-08-02); one-actor-per-brick and the building-scale unit (2026-08-03); scenario-is-a-row (2026-08-07).

---

## 9. Scaffolding

- **`Lvl_Sandbox`**: floor, lighting, PlayerStart, flying spectator pawn — the only authored map; everything else is spawned by scenario row (§2). The same empty world hosts the tests.
- **Folders** separate reusable core systems (`Core/`), world wiring (`World/`), tests, and content; someone should know at a glance what is reusable versus throwaway.
- **In-world main menu** (still to build): the game always has a scenario loaded — a tiny lightweight default at startup so it opens fast, the menu overlaying the running world (no hard load transitions), doubling as the scenario switcher, with on-screen strain readouts.
- Remaining designed-not-built core items are tracked in [CURRENT_STATE.md](CURRENT_STATE.md).
