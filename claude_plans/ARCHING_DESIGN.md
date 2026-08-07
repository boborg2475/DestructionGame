# Arching action and the load path — design

**Status: DESIGN ONLY, nothing built.** Read [DESIGN.md §3](DESIGN.md) (how load reaches the ground, the two-tier support rule) and all of [MOMENTS_DESIGN.md](MOMENTS_DESIGN.md) first; this sits directly on top of the moment work and revises one of its rules. **The staircase corbel is an anchor, not a target** — the brief assumed a slice here would make it stand, and the arithmetic says the opposite. See "The decision this design cannot make".

---

## The scope call, and it revises MOMENTS_DESIGN's discipline line

MOMENTS_DESIGN wrote a rule on the solver: **"`GetJointRole` may never read geometry. Positions are an input to the magnitude of a load, never to the routing of it."**

Slice 1 below obeys it exactly — it changes a *magnitude* (a moment) and no route. **Slice 2 breaks it deliberately**, because deciding that four bricks over a hole are an arch rather than a chain of hangers is a routing decision and it cannot be made without knowing where the hole is. The revision, and it should be written where the old line is:

> `GetJointRole` still may never read geometry — **the tier of one joint stays a fact about one normal and one pairing.** What may read geometry is a *group* rule sitting above the tier, which is gated on `HasCompleteGeometry()` and is a **no-op without it**. A geometry-free structure routes exactly as it does today, bit for bit.

That gate is not politeness. Both fuzz generators emit no geometry (MOMENTS_DESIGN, "the payoff of degenerate-at-zero"), and they are the only property tests over routing this project has. If the arch can fire without geometry, 12,000 fuzzed cases start disagreeing with an oracle that knows nothing about arches, and the single most valuable regression asset in the repo goes dark. **Gate it, and both fuzzes are provably untouched.**

---

## The dominant symptom is not the void. It is one brick.

Reported in game: deleting **one** brick makes the failure walk across the wall in a stepping triangle, one step per course. That fires on every deletion, so it dominates everything the original brief described. The mechanism, worked through:

In running bond a brick rests on **two** bed patches at ∓5.625 cm, each 105.0625 cm². Their area-weighted centroid coincides with the centre of mass, so `e = 0` **exactly** — MOMENTS_DESIGN's N ≥ 2 rule drops the moment, and that is the 0.00495 anchor. Delete one brick beneath and the brick above has **exactly one** seat. That is N = 1, statically determinate, so the moment is computed, and three things worsen at once: the area halves (σ_n doubles), `W_v = (4/3)·h_u·h_v²` falls by 4× as `h_v` halves, and `e` goes from exactly zero to 5.625 cm.

Per brick weight carried, on that surviving patch:

```
σ_b = 2667.198625 × 5.625 / 179.4817708  =  8.3590619e-3 MPa
σ_n = −2667.198625 / 105.0625            = −2.5386781e-3 MPa
peak tension                             =  5.8203838e-3 MPa  →  0.058203838 of f_xk1
```

Those are MOMENTS_DESIGN's case (c) figures and they already have a test — `Core.StructureBinding.AdoptedWallLoadsItsWaistEccentrically`. **Utilisation is exactly linear in the load**, so the joint reaches 1.0 at

```
1 / 0.058203838 = 17.1810 brick weights = 45,825.5 uu
```

which is the same 45,825 the staircase integration test already names. A brick in course *j* of an *N*-course wall carries about *N − j* brick weights, so **the brick above a deletion breaks whenever it has 18 or more courses above it.** In the 30 × 40 scenario wall that is any deletion below course 22. The two half-seated bricks either side of a deletion in course 1 read **2.2118**.

Then it repeats, and the geometry of the repeat is the user's picture. Deleting one brick leaves **two** half-seated bricks in the course above, spanning 44 cm — the hole has grown from one cell to two, half a cell each side. Each course adds another half cell each side. The front advances **11.25 cm horizontally per 7.5 cm of course**, so the wedge's sides sit at **33.69° to the horizontal**, and it stops only when the remaining column is under 17.18 brick weights.

**That slope is a prediction, and a test should assert it rather than assert "a lot broke".**

---

## The physics being added is a horizontal thrust

An arch exists only if the load path can develop a horizontal reaction and something can resist it. Both halves have to be answered.

**What carries it: the abutment's own bed joint, in shear, and the model already checks that.** The thrust arrives horizontally; a horizontal force on a bed joint is shear by `ClassifyForce`'s own definition, and shear capacity is already `c + μ·σ_compression`, truncated at `MaxShearStrengthMPa`. **No new axis, no new strength, no new data.** The thrust must also be walked down to the foundation, but the demand is constant with depth while the capacity grows with the weight above, so **the springing plane is the critical one** and checking it there is sufficient rather than convenient.

**What happens when nothing carries it: there is no arch, and today's cantilever answer stands.** A wall's free vertical end has no head joint on the outboard side — the graph simply has no edge — so an arch thrusting into thin air is not something the model has to refuse, it is something it cannot express. That is the good case. The bad cases are below.

### The traps, found while working the numbers

**(1) The arch must not be a support edge.** The obvious implementation — "a half-seated piece is also supported by its neighbour through the head joint" — makes the two bricks either side of a one-brick hole **each other's support**. That is a two-node cycle, `SolveLoads` reports cycles as unroutable, and DESIGN.md §3 strands them. So the naive arch turns "the wall bridges the hole" into "the wall is unroutable and falls" — a *worse* answer than today's, arrived at through correct-looking physics. This is the exact shape of MOMENTS_DESIGN's determinate/indeterminate trap.

**The resolution: an arch is a moment restraint, not a load path.** Slice 1 changes no support list at all. Slice 2 does change one, but re-seats the group onto joints **outside** the group, which is acyclic by construction.

**(2) The thrust must be equal and opposite, and nothing in the model would notice if it were not.** Every joint is evaluated independently, so applying `+H` at one springing and forgetting the other gives the structure a net horizontal force out of nowhere and every joint still reads plausibly. **A test must assert ΣH = 0 across each arch**, not merely that each springing reads something.

**(3) The abutment must not be hanging from the piece it is abutting.** Two unseated bricks propping each other over a wide hole are *the* thin-air case, and they look locally identical to a real arch. Cheap and exact test: the neighbour `Q` must be `Supported` or `Grounded`, and **`P` must not appear in `Q`'s support list**. The existing stranding machinery already reports mutual proppers as `Stranded`, so the pathological case is refused by a predicate the solver computes anyway.

**(4) The abutment must be on the overhanging side.** Refusing to check the direction is what would destroy the staircase anchor and MOMENTS_DESIGN's case (c) in one stroke. Both fixtures have a live neighbour — on the **seated** side. See the regression table.

---

## The rule, stated exactly

**A joint may develop arching relief when all of:**

- it is a `BedBeneath` joint of the loaded piece, and the piece has **complete geometry** (a joint rectangle and a centre of mass);
- the normal force is **compressive** — no compression, no thrust line, no arch;
- the load resultant is eccentric **beyond the kern**, `e > h/3` on the eccentric axis (10.25 cm deep patch → kern ±1.7083 cm), which is the only condition under which any part of the face is opening;
- there is an **intact head joint on the eccentric side** — the side the centre of mass sits, relative to the joint centroid — to a neighbour `Q` that is `Supported` or `Grounded` and does not count `P` among its supports.

**The relief is a cap on the moment vector:**

```
k = min(1, |σ_n| / σ_b)          σ_b = |M_u|/W_u + |M_v|/W_v
M ← k · M
```

Scaling the whole vector keeps direction, needs no per-axis branch, and is **exactly inert when `σ_b ≤ |σ_n|`** — an intact wall has `M = 0` and `k = 1`, so nothing moves, bit for bit. At the cap, `peak tension = 0` and `peak compression = 2|σ_n|` exactly: the arch keeps the thrust line at the kern edge, which is what an arch *is*.

The capped value is what travels. `Structure.cpp` already commits to this — *"WHAT TRAVELS IS WHAT THE JOINT READS — there is no second, private quantity"* — and handing down an uncapped moment while reading a capped one would create the second quantity that comment exists to forbid.

**Rejected: setting the moment to zero.** Tempting, one line, and wrong by a factor of two on the compression axis — `2|σ_n|` against `|σ_n|`. It is small in absolute terms today and it is the difference between a model and a hand-wave.

---

## What it does to the numbers

| Case | today | with the cap |
|---|---|---|
| per brick weight on a half seat | 0.058203838 (tension) | **5.07735e-4** (compression, `2σ_n/f_c`) |
| one interior delete, 30-course wall (n = 28) | **1.62971 → breaks** | **0.0142166** |
| one interior delete, 40-course wall (n = 38) | **2.21175 → breaks** | **0.0192939** |
| load at which the joint gives | 17.181 brick weights | **1969.5 brick weights** |
| intact wall worst joint | 0.00495 | **0.00495, bit for bit** |

The ratio is **114.63 and it is load-independent**, because both terms are linear in N. And the honest reading of the second row of that table: **an arched half-seat is unbreakable at any scale this game builds.** A 40-course wall reaches 38 brick weights against a 1969 brick-weight limit. That is not a comfort, it is the whole risk in this design, and it is why the thrust check in slice 3 is not optional.

---

## Conditionality: how much wall has to be above it

The published rule is the **triangle of loading**: masonry above roughly an equilateral triangle rising from the opening's edges arches around and never reaches the span. **BS 5977-1** (lintel loading) specifies the equilateral triangle — 60° base angles, height `√3/2 · L` — and requires adequate masonry above and beside. The 45° dispersion often quoted is the more conservative variant used for concentrated-load spread.

**The rule adopted here uses the angle only as a cap on the arching depth, and does not use it to reduce the load at all:**

```
d_e = min( cover above the span , 0.866 · L )      arching depth
r   = d_e / 3                                      thrust-line rise, kern-limited
W   = the load the solver actually computed        NOT a triangle
H   = W·L / (8r)      V = W/2                      per abutment
```

Two deliberate choices, both in the conservative direction.

**The load is the whole column, not the triangle.** Taking the triangle would divide the load by up to 10× and needs a dispersion angle as a tunable. Taking what the solver already accumulated needs nothing, is strictly harsher, and — worked through below — gives the *same* span limit to three significant figures, because in the regime where the limit bites the cover is what caps `d_e` anyway and the triangle never governs. **So the triangle is a cross-check on this design, not a component of it, and no dispersion angle becomes data.**

**The rise is kern-limited, not hinge-limited.** Limit analysis (Heyman, *The Stone Skeleton*, 1966) would allow the thrust line to reach the extreme fibre at each hinge, `r = d_e`. The kern rise `r = d_e/3` is three times stricter and is the choice consistent with this project's uncracked-section model, which already fails a joint when the extreme fibre reaches `f_xk1` rather than when it overturns. **It is also the only choice under which a finite span limit exists at all** — at `r = d_e` the thrust ratio drops below μ and sliding never governs, which would make every opening arch.

**A shallow-cover opening does not get a special case; it fails the thrust check.** With `r = d_e/3` and `d_e` capped by the cover, `H ∝ 1/cover` while `V` falls with it, so `H/V = 3L/(4·d_e)` blows up as the cover thins. Worked, on a 10-cell (225 cm) opening in the scenario wall:

| cover above the opening | `d_e` | shear utilisation at the springing |
|---|---|---|
| 285 cm (opening at course 1) | 194.9 | **0.763 — arches** |
| 7.5 cm (one course, opening near the top) | 7.5 | **2.635 — does not arch** |

If `d_e` were assumed to be `0.866·L` without measuring the cover, that second row reads **0.101** and ten bricks hang in mid-air. That is the permissive failure the brief warns about, it is worth a slice of its own, and its red number is 0.101 → 2.635.

---

## The span limit, and what governs

Three candidates, all worked for this project's brick (21.5 × 10.25 × 6.5 at 1.9 g/cm³, 1 cm joints; smeared wall weight 15.8056 uu per cm² of elevation; springing seat 105.0625 cm²; general-purpose mortar):

**Compression never governs.** With `d_e = 0.866L` the crown stress is `2H/(d_e·t) = 6.678e-5·L` MPa, so reaching 10 MPa needs `L ≈ 1.5 km`. Masonry is enormously overbuilt in compression, exactly as the 0.00495 anchor says.

**The thrust line leaving the section never governs either**, because `r = d_e/3` is *defined* as the largest rise that keeps it inside the kern. It does not disappear — it converts into a higher thrust — which is precisely why shallow cover kills arching in the table above.

**Sliding at the springing governs.** The horizontal thrust is shear on the abutment bed joint against `0.2 + 0.6·σ_n`, capped at 1.3 MPa. For a one-course-tall void at course 1 of the 30 × 40 scenario wall (285 cm of cover, abutment also carrying its own 38 brick weights):

| opening | `d_e` | shear demand | capacity | utilisation |
|---|---|---|---|---|
| 10 cells, 225 cm | 194.9 | 0.4177 MPa | 0.5473 | **0.763 — stands** |
| 15 cells, 337.5 cm | 285 | 0.6426 | 0.6920 | **0.929 — stands** |
| 16 cells, 360 cm | 285 | 0.7311 | 0.7209 | **1.014 — falls** |
| 20 cells, 450 cm | 285 | 1.1424 | 0.8367 | **1.365 — falls** |

**The critical span is L = 356.3 cm — 15.8 brick cells, about 3.6 m — and it is sliding at the springing that decides.** Solving `5.6414e-6·L² = 0.2578827 + 1.28626e-3·L` gives that root directly. The shear cap at 1.3 MPa is not reached (capacity 0.72 at the root), so the truncation is not what is deciding.

Two properties of that answer worth stating because they are emergent rather than tuned. **Deeper burial widens the limit**: `V` grows with cover so friction grows, while `H` in the cover-limited regime is `3ρ_e·L²/8` and does not. And **dry stone can never do this**: `c = 0` and `μ = 0.7` against a demand ratio `H/V = 0.866` fails at every span, which is right — you cannot span a hole in a dry-stone wall with a flat arch, you need a curved ring or a lintel. **That falls out of `ShearCohesionMPa` and `FrictionCoefficient` with no new field and no per-material branch.**

---

## Interaction with the moment work

**Compression suppressing bending tension is physically right.** `peak tension = max(0, σ_n + σ_b)` with a large negative `σ_n` is pre-compression, and it is the mechanism behind every prestressed section ever built and behind why masonry works at all. It is not a get-out-of-jail card. Two consequences to record rather than discover:

- **Utilisation is non-monotonic in load.** Adding centred load to an eccentric joint *reduces* its tension. That is real — loading the haunches is how you stabilise an arch — and it is safe for the cascade, because `FConnection`'s latch is irreversible and a joint that has given stays given. It will look wrong on a readout the first time someone sees it and it should be documented at `ComputeUtilisation`.
- **The arch cap is not the formula being clever; it is an explicit assertion that `σ_b ≤ |σ_n|`.** Granting it without checking the thrust is where "unbreakable" would come from, and slice 3 is the only thing standing between this design and it.

### The corbel ladder is not compounding, and the deep-beam effect must not be adopted

The brief expected the ladder to be an artefact of treating each course as an independent cantilever. **It is not.** The ladder's bottom rung carries 38.5 brick weights at 1608.75 brick-weight-centimetres, i.e. an average arm of **41.79 cm**. The centroid of the same overhanging wedge treated as one composite body sits about **41.25 cm** out. They agree to 1.3%, as statics requires them to — **the total overturning moment is the same either way, and the ladder is not compounding anything.**

What differs is the *section resisting it*:

| resisting section | modulus | tension at the root | verdict |
|---|---|---|---|
| one bed patch (today's ladder) | 179.48 cm³ | 22.93 × f_xk1 | falls |
| composite vertical section, 82.5 cm deep | 11,627 cm³ | **0.369** × f_xk1 | stands |

A factor of **62**, and it flips the outcome. **So arching does not fix 22.93, and neither should a deep-beam slice** — adopting composite vertical action would make the photographed failure stand, which is the one thing this subsystem's whole test suite exists to prevent. The deep-beam effect over an *opening*, where the courses are continuous across the span, is not a separate mechanism at all: it **is** arching, and slices 1–3 are it. Over a *raking cut*, where the failure surface is a pre-formed staircase of bed and head joints, it is a different claim and it is out of scope.

---

## The decision — MADE BY THE USER, 2026-08-06

**Case 3 stands. Composite vertical action is adopted, and the two staircase tests are the things that are wrong.**

The user was given the choice below and ruled that a brick deleted at a free end must **not** bring the wall down. Since any rule local enough to save the free end also saves the staircase corbel, that ruling adopts composite vertical action and moves `AStaircaseVoidCondemnsTheCorbel` and `AStaircaseVoidBringsTheOverhangDown` out of the regression anchors and into the set of tests this work must **change**. They are re-derived, not re-tuned: the new expected values come from the composite section modulus, not from whatever makes the suite green.

**The ruling is internally consistent, which is worth recording because it is evidence rather than preference.** The user independently agreed case 20 (the staircase void) as *local loss* — the loose toothed bricks at the cut edge drop, the mass stands. Composite action puts that wedge at **0.369**, which stands, while the individually unseated teeth at the cut face have no seat at all and still drop. Two separate judgements, made at different times, landing on the same mechanism.

**What this does NOT change:** slice 1 still leaves the staircase at 22.92952589, because the arching cap requires an abutment on the eccentric side and the staircase cut removed exactly that neighbour. So slice 1's guard row is still correct as written; the number changes only when the composite slice lands. Slices 1 through 4 are unaffected by this ruling and their anchors stand.

**The original framing, kept because the argument still has to be answered by whoever implements slice 5:**

**The free end and the staircase corbel are locally indistinguishable, and any rule local enough to save one saves the other.**

The user deleted a brick at the **end** of a wall. Trace it: the half bat above loses its only seat and hangs from one head joint (N = 1, determinate, MOMENTS_DESIGN case (b)) and goes; the full brick beside it is now half-seated overhanging **outward**, and there is nothing outboard to abut against — so the arch is refused, correctly, and the ladder starts. Worked from the top of the wall the loads come out **1, 2.5, 4.5, 7, 10, 13.5, 17.5, 22, 27, 32.5, 38.5** and the moments **5.625, 22.5, 56.25 … 1608.75** — *character for character the staircase fixture's own hand-counted ladder*. The utilisations march 0.058, 0.271, **0.722, 1.494** …, so every corbel with four or more courses above it fails, and the front climbs to within three courses of the top while walking inward 11.25 cm per course. In the 30 × 40 wall that is roughly 17 cells inward over 35 courses: **the upper half of the wall, from one brick.**

So: **slice 1 fixes every interior deletion exactly, and does not fix the outermost one.** The only mechanism that could is composite vertical action, and the table above shows it would put the staircase at 0.369 and make it stand. Checking the free end under the same idealisation gives **0.0082** — a factor of 45 apart, so a threshold *could* separate them — but siting that threshold means deciding which of these two is wrong:

- the staircase overhang is genuinely wrong to fall, and `Core.Structure.AStaircaseVoidCondemnsTheCorbel` plus `Integration.AStaircaseVoidBringsTheOverhangDown` are asserting the wrong thing; or
- the free-end wedge is genuinely right to fall, and what the user saw is correct behaviour that merely looks alarming.

**They cannot both be true, and this is a product decision, not an engineering one.** *(Ruled 2026-08-06: the first. The staircase overhang is wrong to fall, and the two tests asserting it falls are asserting the wrong thing.)*

### Slice 5, which the ruling turns on — composite vertical action

The mechanism is that a stack of courses over a lost support does not resist as a sequence of independent bed patches; the wall acts as a **deep beam**, and the section resisting the overturning moment is the full vertical depth of bonded masonry above the cut rather than one 179.48 cm³ patch. That is the 11,627 cm³ / factor-of-62 row in the table above.

**The hard part is not the modulus, it is the DEPTH — how far up the bonding is continuous enough to act compositely.** Take it as unbounded and every wall becomes a monolith, which is the indestructible failure wearing new clothes; take it as one course and nothing changes. Three constraints the slice must satisfy, and the numbers to hit:

- the free-end wedge reads **0.0082** and stands (this is the ruling);
- the staircase wedge reads **0.369** and stands (this follows from the ruling, and is the same idealisation);
- **something must still come down.** Slice 5 without a depth limit removes the last mechanism by which a wall can fail at all. Whatever bounds the composite depth needs its own red test showing a wall that still collapses *because* the depth ran out — most likely where the bond is interrupted, since a raking cut through bed and head joints is a pre-formed failure surface and composite action across it is exactly the claim that needs justifying.

**Both figures above are idealisations flagged in "Verified versus assumed" and neither has been measured.** They are the target, not the assertion — derive the expected value from the section actually implemented and say so if it disagrees.

---

## Where the data lives

**Nothing new on `FConnectionStrength`, and nothing new in `Core/Profiles`.** That is the headline, and it is checkable:

| quantity | comes from |
|---|---|
| kern, section moduli | `InterfaceHalfExtentCm` — already emitted by `MakeInterface` |
| span `L`, cover, eccentric side | joint centroids and `CentreOfMassCm` — already there |
| abutment existence | the connection graph and `PieceSupported` — already computed |
| thrust resistance | `ShearCohesionMPa`, `FrictionCoefficient`, `MaxShearStrengthMPa` — already there |
| "can this joint arch?" | **derived**: compressive normal force plus non-zero shear capacity. Dry stone fails it at every span; a nailed timber joint never develops a thrust line. |

The one genuine constant is `0.866` capping the arching depth, and it is a modelling constant with a published source (BS 5977-1's equilateral triangle), not a material property. It belongs beside `BedJointCosine` as a named `constexpr` with its citation — **one number, one place, not a column on every profile.** If anyone finds themselves adding `bDevelopsArchAction` or a per-profile dispersion angle, that is the regression DESIGN.md warns about and this table is the argument against it.

`FConnectionLoad` and `FJointSection` need no new fields. Slice 3 needs the horizontal component of `ConnectionForces[Index]`, which is already an `FVector` carrying `(0, 0, ±share)`.

---

## The regression anchors

**Hard invariants — bit-identical, and each has a reason rather than a hope:**

| what | why it cannot move |
|---|---|
| intact wall worst joint **0.00495** | every seat has `e = 0` exactly, so `k = 1`; no piece has an incomplete seat, so no group forms; no arch, no thrust |
| every zero-moment answer, **bit for bit** | `min(1, …)` with `M = 0` is `M`; the group rule is gated on `HasCompleteGeometry()` |
| **both fuzzes**, all 12,000+ cases | the generators emit no geometry, so the gate is closed and *neither* the cap *nor* the re-seat can fire |
| **every `Core.Layout` test** | no producer change; `MakeInterface` is untouched |
| `Structure.AStaircaseVoidCondemnsTheCorbel` — **22.92952589, 8 of 11** | **THROUGH SLICE 4 ONLY.** The corbel's eccentric side is where the cut removed the neighbour, so there is no head joint there and the arch is refused. **Slice 5 changes this test deliberately** — see the ruling. |
| `Integration.AStaircaseVoidBringsTheOverhangDown` | same, and it inverts at slice 5: the overhang must stand |
| `StructureBinding.AdoptedWallLoadsItsWaistEccentrically` — **0.058203838** | bricks 3 and 4 overhang the waist *outward*; their shared head joint is on their **seated** sides. This fixture is the direction check for the abutment rule and is worth naming as such in the test's comment. |
| `StructurePushTest`'s ragged-end **0.0582038382** | a ragged end brick overhangs toward the free end — no neighbour outboard |
| MOMENTS_DESIGN case (b): **0.4157273077**, chain 0.8315, ≈3 bricks | a head joint is not a springing; the cap is `BedBeneath`-only |

**A wall with no void is bit-identical, and that is the strongest anchor available** — the same property that made the moments slicing safe. Every one of the four gates (complete geometry, compressive normal, outside the kern, abutment on the eccentric side) is false everywhere in an intact wall, so the new code is not merely inert, it is unreached.

**May legitimately move:** the utilisation of any joint next to a deliberately cut hole; the break count and pass count of any world test that deletes bricks; `PieceInspection` readouts for arched joints; and whatever `Core.Structure` tests exist for a piece that has lost one of two supports *and has a live neighbour on the overhang side* — moving is the point of the work, and each such number should be re-derived, not re-tuned.

---

## Slices, red-test-first

**1. A missing brick is bridged, not cantilevered.** The cap, plus all four gates. Red: `Structure.AMissingBrickIsBridgedNotCantilevered` on a 7 × 30 flush wall, deleting one interior brick from course 1, asserting the two half-seated bed joints read **0.0142166** and that `SolveAndBreak` breaks **zero** joints — it currently reads **1.62971** and takes the wall. Guard rows in the same test: the intact wall's worst joint is **exactly** its current value; the staircase corbel is **exactly** 22.92952589. *Note the staircase test already fails on its own if the cap is applied without the direction check — the suite contains its own guard, which is worth saying out loud to whoever implements this.*

**2. A hole wider than one brick is spanned.** Fully unseated pieces stop hanging from head joints and are re-seated onto the group's abutment joints; contiguous incomplete-seat pieces form a group through head joints, abutted only if the group has a live seat on **both** sides of its centre. Red: a 3-cell interior cut in the same wall, asserting nothing breaks. Slice 1 alone does **not** fix this — the edge brick's neighbour hangs *from it*, so trap (3) refuses the arch, correctly. Expected springing utilisation ≈ **0.036** on the compression axis. **This is the slice that breaks MOMENTS_DESIGN's discipline line**; land the revised wording with it.

**3. The thrust exists and the springing has to carry it.** `H = W·L/(8r)`, applied as the horizontal component of the springing force, equal and opposite at the two ends. **Cannot be deferred past slice 2** — after slice 2 and before slice 3 every opening of every width stands, which is the indestructible failure and is worse than today. Red: a 20-cell cut must still come down, springing shear **1.365**; control at 10 cells reading **0.763**; and `ΣH = 0` across the arch.

**4. An arch needs masonry over it.** `d_e` capped by the cover actually found, by a bounded upward walk over bed joints (at most `ceil(0.866L / course pitch)` steps) rather than by any spatial query. Red: a 10-cell cut with one course above must come down — **0.101 → 2.635**.

**5. Composite vertical action, and the depth that bounds it.** *(Unblocked 2026-08-06 by the user's ruling; see the decision section for the constraints and the numbers.)* Red: the free-end wedge stands, the staircase wedge stands, **and a third case still collapses because the composite depth ran out.** The two staircase tests are updated as part of this slice, with values re-derived from the section actually implemented.

**Stop at slice 5.** The remaining threads (the triangle as a real load reduction; the thrust propagated down the pier) are genuine follow-ups with nothing blocking them and nothing depending on them.

**Which slice makes the staircase stop collapsing: slice 5, and that is now the intended outcome rather than a hazard.** The slice that makes the *reported interior* symptom stop is slice 1.

---

## What breaks

1. **MOMENTS_DESIGN's discipline line** — revised, not violated silently. See the top.
2. **`ReceivedMomentUuCm` transfers the capped moment**, so relief propagates downward. That is required by `Structure.cpp`'s own "what travels is what the joint reads", and it means moment is **not conserved** at an arched joint — the missing couple is taken by a neighbour we do not load until slice 3. Same shape and same honesty as MOMENTS_DESIGN's "unconservative otherwise, and recorded as such".
3. **`ConnectionForces` stops being purely vertical** at slice 3. `Structure.cpp`'s "straight down, and pointing at whichever end of the joint is being held up" comment becomes narrower, and `GetConnectionForce`'s contract with the readout changes — a player will see a non-vertical force on a springing and it needs a line of explanation, exactly as `GetConnectionMoment` did.
4. **`SolveLoads` gains an O(pieces) group pass.** Measured baseline is 6.4 ms over 1,220 pieces and 3,520 joints since the adjacency index landed, and the gates are false for every piece of an intact wall, so the added cost is a predicate per piece. The abutment predicate must be the cheap one-step form (trap 3), not an exclusion-reachability query.
5. **`PieceInspection` / `BuildPieceMenuInspector` cannot explain an arched joint.** A joint reading 0.014 beside a moment of 570,116 uu·cm has no arithmetic a player can follow. The cap needs to be visible — the same defect class as MOMENTS_DESIGN's item 4, and it should not be a follow-up.
6. **`CURRENT_STATE.md`'s "a cycle is reported, not solved"** entry, which explicitly names the true arch as the case where the model is wrong, is partly answered by slice 2 and should be rewritten rather than deleted: the loop-division rule is still absent, and the arch avoids needing it rather than supplying it.
7. **The staircase support header's ladder comment** claims the corbel receives "NOTHING AT ALL" from the brick along the course, on the grounds that indeterminate loaders carry no moment. Still true, but the reason needs the arch clause added or it reads as a general rule.

---

## What not to do

- **Do not make the arch a support edge.** Trap 1. Two bricks over a hole become a cycle, the solver strands them, and the wall falls for a *new* reason.
- ~~**Do not add composite vertical (deep-beam) action** to fix the free end.~~ **REVERSED 2026-08-06 by the user's ruling.** It is now slice 5, and it does put the staircase at 0.369 and make the photographed failure stand — which is the intended outcome, not a side effect. **Do not land it without the depth limit and its own red test showing a wall that still collapses**; unbounded composite depth makes every wall a monolith.
- **Do not add a dispersion angle, an arch flag or an arch strength to the profiles.** Everything needed is derivable; a per-material branch here is the regression DESIGN.md §2 names by name.
- **Do not tune `f_xk1` or `f_vk0` upward.** 0.10 and 0.20 MPa are EN 1996-1-1 Tables 3.2 and 3.4 and the whole model's credibility is that they are unmodified. Every number in this document is a consequence of leaving them alone.
- **Do not apply the cap to head joints.** A head joint is not a springing; doing so deletes MOMENTS_DESIGN case (b) and with it the only visible thing the moment work bought.
- **Do not let the arch fire without geometry.** Both fuzzes go dark the day it does, and they will go dark quietly.
- **Do not implement the cap as `M = 0`.** Off by exactly 2× on compression.
- **Do not ship slice 2 without slice 3.**

---

## Verified versus assumed

**Worked through arithmetically and reproducible from published figures already in the repo:** the 0.058203838-per-brick-weight half-seat figure and its 17.181 brick-weight limit (they reproduce the existing 45,825 uu and MOMENTS_DESIGN's case (c) exactly); the 114.63 ratio and its load-independence; the 1.62971 / 2.21175 / 0.0142166 / 0.0192939 table; the 33.69° front slope; the free-end ladder reproducing the staircase's hand-counted 1 … 38.5 and 5.625 … 1608.75 **exactly**, which is the strongest single check in this document because two independently derived walks landed on the same eleven numbers; the 41.79 vs 41.25 cm arm agreement; the 179.48 vs 11,627 cm³ section comparison and the resulting 0.369; that the intact wall is bit-identical.

**Believed and cited, but not verified against the source text:** BS 5977-1's equilateral (60°) triangle and its requirement of masonry above and beside; EN 1996-1-1's treatment of load dispersion for concentrated loads (§6.1.3 from memory — check the clause number before it goes in a comment). The 0.866 constant rides on the first of these.

**Assumptions a test will have to confirm, flagged individually:** that the arch load may be idealised as uniformly distributed (it is not — the triangle is triangular — and the parabolic `H = WL/8r` follows from the UDL); that `r = d_e/3` is the right rise, which is a *choice* consistent with this project's uncracked-section model and is what makes the span limit finite at all; that the springing bearing may be taken as one 105.0625 cm² bed patch rather than spread over the arching depth (conservative, but by an unquantified factor, and it is the single largest lever on the 356 cm figure); that checking the thrust at the springing plane alone is sufficient because capacity grows downward faster than demand.

**The 356.3 cm span limit is the least trustworthy number in this document** — it is a fourth-power-ish product of the rise choice, the bearing-area choice and the UDL idealisation. Its *ordering* (sliding governs, compression is 1.5 km away, the thrust line is inside by construction) is solid; the value is an order-of-magnitude claim dressed in four significant figures, and slice 3's test should pin 10 cells and 20 cells, not 15 and 16.
