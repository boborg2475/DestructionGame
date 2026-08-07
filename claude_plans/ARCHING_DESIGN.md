# Arching action and the load path — design

**Status: ALL FIVE SLICES ARE BUILT.** [CURRENT_STATE.md](CURRENT_STATE.md) records where the code has got to; this file stays the authority on what it should do.

**THE URGENCY FRAMING THIS FILE CARRIED FOR SLICE 3 WAS WRONG, AND THE MEASUREMENT IS RECORDED HERE RATHER THAN QUIETLY DELETED.** It said "after 2 and before 3 every opening of every width stands", i.e. that slice 2 alone was the indestructible failure. It was not. Slice 2 routes a spanned group's **whole** load outward through **head joints**, in pure shear against 0.2 MPa of cohesion with **no normal force to buy friction with** — which caps an opening at roughly 50 brick weights a side, about two columns. The 7 × 30 three-cell fixture sits at **0.5594** of that joint, and anything wider than about three cells already failed there. Both of slice 3's own test openings, 10 cells and 20 cells, came down **before** slice 3 landed, at the re-seat head joint and for the wrong reason. The thrust was still worth doing on schedule — it is the mechanism that decides, and the head-joint limit is an accident of where slice 2 left the load — but "every opening stands" was never measured and was never true. Read [DESIGN.md §3](DESIGN.md) (how load reaches the ground, the two-tier support rule) and all of [MOMENTS_DESIGN.md](MOMENTS_DESIGN.md) first; this sits directly on top of the moment work and revises one of its rules. **The staircase corbel is an anchor, not a target** — the brief assumed a slice here would make it stand, and the arithmetic says the opposite. See "The decision this design cannot make".

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

**MEASURED AGAINST THE FIXTURE, 2026-08-06 — two rows are 0.1-0.2% optimistic and the cause is the fixture, not the physics.** On the 7 x 30 flush wall the two half-seated joints read **1.62749 and 1.62719** today, arching to **0.0141885 and 0.0141946** — against 1.62971 / 0.0142166 above. A flush wall's odd courses are 6 full bricks plus 2 half bats, which weigh 0.65% less than an even course's 7, so the joints carry **27.9447 and 27.9567 brick weights rather than a round 28**; "about N − j brick weights" is an idealisation. **The 114.63 ratio and the load-independence reproduce exactly.** `StructureArchingTest` therefore asserts the *derived identity* — `2|σ_n|/f_c` recomputed from the force the solver itself reports, at 1e-12 — and keeps the literals above only as a 2% cross-check, so that a future disagreement fails in the test rather than being tuned away.

**And the 0.00495 anchor in the table above is the 40-course SCENARIO wall, which this document does not say.** The 7 x 30 flush wall's worst joint is **0.0036748258197270385**. Both are the same statement — `e = 0` everywhere, so no moment anywhere — but a guard row has to name its wall.

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

If `d_e` were assumed to be `0.866·L` without measuring the cover, that second row reads **0.101** and ten bricks hang in mid-air. That is the permissive failure the brief warns about and it was worth a slice of its own — **slice 4, built 2026-08-07**. Its red number came out 0.058 → 1.51 rather than the 0.101 → 2.635 above: the same 25.98× step, from a lighter springing than this table assumes.

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

A factor of **62**, and it flips the outcome. ~~**So arching does not fix 22.93, and neither should a deep-beam slice** — adopting composite vertical action would make the photographed failure stand, which is the one thing this subsystem's whole test suite exists to prevent.~~ **REVERSED 2026-08-06 BY THE USER'S RULING, AND BUILT AT SLICE 5.** Composite vertical action *is* adopted, it *does* make the photographed failure stand, and that is now the intended outcome — the ruling was made on the free end, and the free end and the raking corbel are locally indistinguishable. The paragraph is kept because the argument it makes is the one whoever reads this next will make again, and because it names the risk correctly: the thing that was supposed to stop it becoming "nothing can ever be destroyed" is that the moment grows as *k³* while the section grows only as *k²*, so a corbel taller than about thirty-six courses still comes down. **That defence does not survive a wall taller than its cut — see the warning under the depth table below, found in review on 2026-08-07 — so the risk this paragraph names is currently LIVE rather than answered.** The deep-beam effect over an *opening*, where the courses are continuous across the span, is not a separate mechanism at all: it **is** arching, and slices 1–3 are it.

---

## The decision — MADE BY THE USER, 2026-08-06

**Case 3 stands. Composite vertical action is adopted, and the two staircase tests are the things that are wrong.**

The user was given the choice below and ruled that a brick deleted at a free end must **not** bring the wall down. Since any rule local enough to save the free end also saves the staircase corbel, that ruling adopts composite vertical action and moves `AStaircaseVoidCondemnsTheCorbel` and `AStaircaseVoidBringsTheOverhangDown` out of the regression anchors and into the set of tests this work must **change**. They are re-derived, not re-tuned: the new expected values come from the composite section modulus, not from whatever makes the suite green.

**The ruling is internally consistent, which is worth recording because it is evidence rather than preference.** The user independently agreed case 20 (the staircase void) as *local loss* — the loose toothed bricks at the cut edge drop, the mass stands. Composite action puts that wedge at **0.369**, which stands, while the individually unseated teeth at the cut face have no seat at all and still drop. Two separate judgements, made at different times, landing on the same mechanism.

**What this does NOT change:** slice 1 still leaves the staircase at 22.92952589, because the arching cap requires an abutment on the eccentric side and the staircase cut removed exactly that neighbour. So slice 1's guard row is still correct as written; the number changes only when the composite slice lands. Slices 1 through 4 are unaffected by this ruling and their anchors stand.

**The original framing, kept because the argument still has to be answered by whoever implements slice 5:**

**The free end and the staircase corbel are locally indistinguishable, and any rule local enough to save one saves the other.**

The user deleted a brick at the **end** of a wall. Trace it: the half bat above loses its only seat and hangs from one head joint (N = 1, determinate, MOMENTS_DESIGN case (b)) and goes; the full brick beside it is now half-seated overhanging **outward**, and there is nothing outboard to abut against — so the arch is refused, correctly, and the ladder starts. Worked from the top of the wall the loads come out **1, 2.5, 4.5, 7, 10, 13.5, 17.5, 22, 27, 32.5, 38.5** and the moments **5.625, 22.5, 56.25 … 1608.75** — *character for character the staircase fixture's own hand-counted ladder*. The utilisations march 0.058, 0.271, **0.722, 1.494** …, so every corbel with four or more courses above it fails, and the front climbs to within three courses of the top while walking inward 11.25 cm per course. In the 30 × 40 wall that is roughly 17 cells inward over 35 courses: **the upper half of the wall, from one brick.**

So: **slice 1 fixes every interior deletion exactly, and does not fix the outermost one.** The only mechanism that could is composite vertical action, and the table above shows it would put the staircase at 0.369 and make it stand. ~~Checking the free end under the same idealisation gives **0.0082** — a factor of 45 apart, so a threshold *could* separate them~~ — **the 0.0082 is WRONG and no fixture in this project produces it; see "the free end reads 0.0159" below** — but siting that threshold means deciding which of these two is wrong:

- the staircase overhang is genuinely wrong to fall, and `Core.Structure.AStaircaseVoidCondemnsTheCorbel` plus `Integration.AStaircaseVoidBringsTheOverhangDown` are asserting the wrong thing; or
- the free-end wedge is genuinely right to fall, and what the user saw is correct behaviour that merely looks alarming.

**They cannot both be true, and this is a product decision, not an engineering one.** *(Ruled 2026-08-06: the first. The staircase overhang is wrong to fall, and the two tests asserting it falls are asserting the wrong thing.)*

### Slice 5, which the ruling turns on — composite vertical action

The mechanism is that a stack of courses over a lost support does not resist as a sequence of independent bed patches; the wall acts as a **deep beam**, and the section resisting the overturning moment is the full vertical depth of bonded masonry above the cut rather than one 179.48 cm³ patch. That is the 11,627 cm³ / factor-of-62 row in the table above.

**The hard part is not the modulus, it is the DEPTH — how far up the bonding is continuous enough to act compositely.** Take it as unbounded and every wall becomes a monolith, which is the indestructible failure wearing new clothes; take it as one course and nothing changes. Three constraints the slice must satisfy, and the numbers to hit:

- the free-end wedge stands. ~~reads **0.0082**~~ — **it reads 0.0159, and 0.0082 was wrong; see below**;
- the staircase wedge reads **0.369** and stands (this follows from the ruling, and is the same idealisation);
- **something must still come down.** Slice 5 without a depth limit removes the last mechanism by which a wall can fail at all. Whatever bounds the composite depth needs its own red test showing a wall that still collapses *because* the depth ran out.

### The two published figures, MEASURED 2026-08-07, and one of them was wrong

**The staircase's 0.369 reproduces exactly** — `M/(W·f_xk1)` with M = 1608.75 brick-weight-cm and W = 10.25 × 82.5²/6 = 11,627.34 cm³ gives **0.36903147272727271**, and that is what the built solver reads to the last digit.

**The free end's 0.0082 reproduces from NO FIXTURE and is not a target.** It is one row of an identity rather than a measurement: a half-seated brick carrying *n* brick weights under *m* courses of depth reads `0.1561287·n/m²`, and 0.0082173 is the `n = m = 19` row of it. No wall in this project presents that row. The 7 × 30 flush wall the ruling was made about has **29 courses over its free-end joint carrying 43.27 brick weights**, and it reads **0.01588** — measured. Two further corrections fall out of the same walk: the free-end rung's moment is a **two-arm** walk, its own weight at 5.625 cm and everything arriving from above at 11.25 cm, so `M = 5.625 + 11.25(n−1)` and not `5.625·n`; and the ratio between the two cases is a factor of 23, not 45.

### The depth table below points the WRONG WAY, and the crossover is at 36 courses

**Every row of `4 → 2.79 … 30 → 0.0496` holds the STAIRCASE's own moment fixed while shrinking the depth, and no wall can present that pair.** In a real *k*-course corbel the moment shrinks with the depth as well — the ladder is shorter — so the reading is **monotonically INCREASING in k**, and the shallow corbels are the *safe* ones:

| corbel steps *k* | 3 | 5 | 7 | 9 | 11 | 30 | 35 | 37 | 45 |
|---|---|---|---|---|---|---|---|---|---|
| reads | 0.173 | 0.219 | 0.268 | 0.318 | **0.369** | 0.860 | 0.990 | 1.042 | **1.250** |

**The crossover is at about thirty-six courses, not under seven.** The moment grows roughly as *k³* while the section grows only as *k²*, which is why a tall enough corbel still comes down and a short one never does.

> ### ⚠ THAT ARGUMENT IS ONLY TRUE WHEN THE WALL AND THE CORBEL ARE THE SAME HEIGHT, AND IT WAS STATED GENERALLY. *(Found in review, 2026-08-07.)*
>
> `k³/k²` assumes `D = k · pitch` — the depth credited to the joint comes from the corbel's own steps. **Every corbel fixture in the suite happens to satisfy that**: all four rows of `FCorbelCase` are `CoursesHigh = Steps + 2`, so no test in the project has a corbel shorter than its wall.
>
> In a wall *taller* than the cut, `D` is set by the **wall** and `k` by the **cut**, so the two exponents stop cancelling and the thirty-six-course crossover does not survive as stated.
>
> **MEASURED 2026-08-07, and it corrects the two numbers this block first carried.** A 1.5× depth *mutation* moves one term; a wall 1.5× *taller* moves both, because the extra courses stand on the corbel and bear down through every rung.
>
> - **45 steps under 67 courses is GREEN, not red.** 47→67 courses multiplies that rung's moment by 2.2754 and its section by 2.0864, so the reading goes **up** — `1.2501861088888881 → 1.3634027672024234` — and the corbel still comes down. Extrapolating, a 45-step corbel needs about **160 courses, twelve metres of wall**, before depth rescues it.
> - **The scenario joint reads `0.22300137936935951`, not "of order 0.03".** The 0.0309 figure was the 13-course fixture's moment evaluated at 285 cm of depth; the real joint carries **5.199× the force and 7.212× the moment**.
>
> **Severity is set by the ratio `m/k`, not by `m`** — a short cut under a tall wall. At 67/45 the added load nearly keeps up; at 40/11 it loses by a factor of two.
>
> **The defect is real and the property that catches it is ONE-SIDED.** "Same ladder, so same reading" is too strong and the fixture disproves it. What survives is: **adding masonry on top of a corbel must not make the corbel safer.** The scenario wall reads `0.22300` against the matched wall's `0.36903` — ×0.604, more load for less utilisation. Asserted with no tolerance, preceded by a row proving the taller wall really does bend the rung harder.
>
> **AND NO OBVIOUS BOUND SATISFIES BOTH ENDS.** PART 2 of the same test pins the free end — *the user's own ruling* — at the unbounded depth: that half seat needs `W ≥ 1283.5 cm³`, i.e. **`D ≥ 27.4 cm ≈ 3.7 courses**, or it goes over capacity and the ruling breaks. So "the cut's own height" is unavailable, since a single deleted brick is a one-course cut and would read 6.05. And a span-to-depth bound sized to save the free end gives the scenario corbel `D ≤ 302 cm`, above the 285 it already takes, changing nothing.
>
> **The window is `27.4 cm ≤ D ≤ 221.55 cm`** — the ceiling being `D_short·√(M_tall/M_short)`, above which more wall makes the corbel safer. The fixture **refuses today's rule** and **ranks none of the alternatives**; on the scenario rung they give 0.2230 (today, 285 cm), 2.6613 (the corbel's own 82.5 cm, comes down) and 167.32 (bed patch alone). Choosing inside that window is a ruling, and the scenario row's outcome is deliberately left `Unasserted` so the test cannot pre-empt it.
>
> **So the composite depth is effectively unbounded in play, and "the exponent is the bound" is not a defence.** dev-expert's refusal to build a depth bound was correct TDD — nothing distinguished bounded from unbounded — but the missing fixture is two rows in a table that already exists, and it is the fixture that makes the difference observable. Add the rows first, then rule.

That is the whole of the "something must still come down" constraint as it currently stands, and the paragraph above is why it needs re-testing rather than trusting.

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
| intact wall worst joint — **0.00495** on the 40-course scenario wall, **0.0036748258197270385** on the 7 x 30 flush wall | every seat has `e = 0` exactly, so `k = 1`; no piece has an incomplete seat, so no group forms; no arch, no thrust. **Name the wall in the guard row** — the two figures are the same statement about different fixtures, and the original table said only the first. |
| every zero-moment answer, **bit for bit** | `min(1, …)` with `M = 0` is `M`; the group rule is gated on `HasCompleteGeometry()` |
| **both fuzzes**, all 12,000+ cases | the generators emit no geometry, so the gate is closed and *neither* the cap *nor* the re-seat can fire |
| **every `Core.Layout` test** | no producer change; `MakeInterface` is untouched |
| ~~`Structure.AStaircaseVoidCondemnsTheCorbel` — **22.92952589, 8 of 11**~~ **NOW 0.3690314727, 0 of 11** | **CHANGED AT SLICE 5, deliberately, by the ruling.** The arch is still refused — the corbel's eccentric side is where the cut removed the neighbour — and what moved is the SECTION. `StructureArchingTest`'s ANCHOR 2 carries the same figure and moved with it; the anchor still bites, because an ungated arch would read 0.0195 and that is a factor of 19 away. |
| ~~`Integration.AStaircaseVoidBringsTheOverhangDown`~~ **renamed `AStaircaseVoidLeavesTheOverhangStanding`** | inverted at slice 5: no corbel joint breaks, no corbelled brick moves, and nothing at all comes down on this wall |
| `StructureBinding.AdoptedWallLoadsItsWaistEccentrically` — **0.058203838** | bricks 3 and 4 overhang the waist *outward*; their shared head joint is on their **seated** sides. This fixture is the direction check for the abutment rule and is worth naming as such in the test's comment. **It survived slice 5 too, and structurally rather than luckily**: both bricks are the TOP course of their wall, so nothing rests on them and there is no stack. |
| `StructurePushTest`'s ragged-end **0.0582038382** | a ragged end brick overhangs toward the free end — no neighbour outboard. Also the top course, so slice 5 leaves it alone. **Its sibling did move**: the same file's `MortarRaggedWorstAsBuilt` is the joint under a corbel with one brick standing on it, two courses of composite section, and went **0.0455104479 → 0.0390321745**. |
| MOMENTS_DESIGN case (b): **0.4157273077**, chain 0.8315, ≈3 bricks | a head joint is not a springing; the cap is `BedBeneath`-only |

**A wall with no void is bit-identical, and that is the strongest anchor available** — the same property that made the moments slicing safe. Every one of the four gates (complete geometry, compressive normal, outside the kern, abutment on the eccentric side) is false everywhere in an intact wall, so the new code is not merely inert, it is unreached.

**May legitimately move:** the utilisation of any joint next to a deliberately cut hole; the break count and pass count of any world test that deletes bricks; `PieceInspection` readouts for arched joints; and whatever `Core.Structure` tests exist for a piece that has lost one of two supports *and has a live neighbour on the overhang side* — moving is the point of the work, and each such number should be re-derived, not re-tuned.

---

## Slices, red-test-first

**1. A missing brick is bridged, not cantilevered.** The cap, plus all four gates. Red: `Structure.AMissingBrickIsBridgedNotCantilevered` on a 7 × 30 flush wall, deleting one interior brick from course 1, asserting the two half-seated bed joints read **0.0142166** and that `SolveAndBreak` breaks **zero** joints — it currently reads **1.62971** and takes the wall. Guard rows in the same test: the intact wall's worst joint is **exactly** its current value; the staircase corbel is **exactly** 22.92952589. *Note the staircase test already fails on its own if the cap is applied without the direction check — the suite contains its own guard, which is worth saying out loud to whoever implements this.* **Confirmed by mutation 2026-08-06**: an ungated cap takes the staircase 22.9295 → 0.019548 and 8 of 11 over capacity → 0 of 11, the waisted brick 0.0582038 → 0.00050774, and MOMENTS case (b) 0.415727 → 0.0200165 — the last because a head joint has `σ_n = 0` exactly, so an ungated `k` *deletes* the moment rather than capping it.

**TWO OF THE FOUR GATES CANNOT BE PROVED TO BITE BY ANY SLICE 1 TEST, and that is a hazard worth naming rather than discovering.** Gate 1 (complete geometry) is inert here because with no rectangle there is no moment to cap at all; its real job is protecting both fuzzes at slice 2 and refusing an implementation that reaches for the abutment before checking `HasCompleteGeometry()`. Gate 3 (outside the kern) is inert because `min(1, ·)` is already 1 inside the kern; its real job is catching the version written *without* the `min`, which would scale that joint's bending stress up nearly sevenfold. **Both must be implemented properly even though slice 1's test cannot punish skipping them** — and slice 2 should add the rows that can.

**2. A hole wider than one brick is spanned.** Fully unseated pieces stop hanging from head joints and are re-seated onto the group's abutment joints; contiguous incomplete-seat pieces form a group through head joints, abutted only if the group has a live seat on **both** sides of its centre. Red: a 3-cell interior cut in the same wall, asserting nothing breaks. Slice 1 alone does **not** fix this — the edge brick's neighbour hangs *from it*, so trap (3) refuses the arch, correctly. Expected springing utilisation **0.02837070531192621** on the compression axis for a three-cell cut. ~~0.036~~ **was wrong**, and the error is worth keeping visible: 0.036 is the arithmetic for *five* columns through two seats, but **an N-cell cut leaves N − 1 unseated bricks plus 2 half-seated ones**, so three cells give four columns, not five. 0.036 is the four-cell figure. `StructureSpannedHoleTest` derives the value from **conservation** rather than copying it — nothing above the spanned course changes, so each group brick's column is identical in the intact and cut walls (74435.5 / 74534.1 / 74566.1 / 74534.1 uu) and all of it must leave through the two surviving seats. That holds whatever rule slice 2 picks for dividing the group's load, which is why the sum is pinned to 1e-9 while each springing gets 2%. **This is the slice that breaks MOMENTS_DESIGN's discipline line**; land the revised wording with it.

**3. The thrust exists and the springing has to carry it.** *(BUILT 2026-08-07. `StructureThrustTest`.)* `H = W·L/(8r)`, applied as the horizontal component of the springing force, equal and opposite at the two ends. Red: a 20-cell cut must still come down; control at 10 cells; and `ΣH = 0` across the arch. What was actually built, and where it differs from the paragraph above:

- **It fires only where slice 2 SPANS a group**, i.e. at the abutments of a hole wider than one brick. A one-cell hole is slice 1's moment restraint and no load crosses it, so no thrust is developed there — which is also the only reading under which `Structure.AMissingBrickIsBridgedNotCantilevered` stays bit-identical, since that test asserts its springing carries **exactly zero** shear.
- **`W` is the total vertical the arch delivers into both abutments' seats**, springings' own columns included, and `H = W·L/(8r)` is applied as one number pushed out at both ends. Per-end `H = 0.866·V` would read the ratio more exactly at each springing and would leave `ΣH ≠ 0`; the design's own one-number-per-arch form is what makes trap 2 exact.
- ~~**`d_e` is `0.866·L` and the cover cap is NOT implemented**~~ — **landed at slice 4.** As slice 3 shipped it, `L` cancelled out of `H/V` entirely and the thrust ratio was the constant `3/(4·0.866) = 0.866051` at every span. Measured then: 10 cells **0.88649** of shear (stands), 20 cells **1.08603** (comes down), dry stone **1.237215** at every span and every load, `ΣH` exactly **0**. The first, third and fourth of those are unchanged by slice 4, because the angle still governs them.
- ~~**The 20-cell case reads 1.086 rather than the 1.365 this document predicts**~~ — with the cover cap it reads **1.19940 MPa of shear against 0.80771 of capacity**, `H/V = 1.18424` against the predicted 1.1842. The ordering the document says is the solid part held throughout.

**4. An arch needs masonry over it.** *(BUILT 2026-08-07. `StructureCoverTest`.)* `d_e` capped by the cover actually found, by a bounded upward walk over bed joints (at most `ceil(0.866L / course pitch)` steps) rather than by any spatial query. What was actually built, and where it differs from the paragraph above:

- **The red was 0.058 → 1.51, not 0.101 → 2.635.** The design's pair is kept as a factor-of-two cross-check and the *step* between them reproduces exactly — 25.98×, which is `0.866·L/cover` — but the fixture's springing is lighter than the one the table was worked for, so both ends of the step are lower. The **ordering** the design calls solid is what held.
- **`L` comes from the abutments' own centres**, one mean per end, recorded on `FSpannedArch` by the group pass. Measured: the two ends of a ten-cell arch are **exactly 225 cm** apart, so the clear opening is available with no new query, as this file predicted.
- **The depth is carried as `d_e/L` rather than as `d_e`.** The thrust only ever depends on the ratio, and holding it that way keeps the angle-governed branch character for character slice 3's `3W/(8·0.866)` — so every figure slice 3 pinned is bit-identical, including the dry-stone 1.237215440448697. Dividing `0.866·L` back out of `L` does not cancel in IEEE and moves it.
- **The cover is measured at the ABUTMENTS, not over the middle of the span, and reduced to the thinnest of the two.** One number per arch, which is what keeps trap 2 exact. Both ends of every fixture stand under identical cover, so nothing here distinguishes it from a genuinely per-abutment measurement; that needs a stepped or gabled wall and no fixture has one.
- **Slice 3's two deferred rows went green with it**: the 20-cell `H/V` reads **1.18424 / 1.18418** against the asserted `3L/(4·d_e) = 1.1842`.
- **Acceptance case 8 still drops 0, and slice 4 was never going to fix it** — see the shear-utilisation argument in CURRENT_STATE.md. It needs its own slice or a ruling.

**5. Composite vertical action, and the depth that bounds it.** *(BUILT 2026-08-07. `StructureCompositeDepthTest`, `Core.Structure.ACorbelResistsWithItsWholeDepth`.)* All three constraints hold: a five-step and an eleven-step raking corbel stand, a forty-five-step one comes down, the free end stands, and the same staircase laid dry is still condemned. What was actually built, and where it differs from the paragraph above:

- **The joint reads the LESSER of two limit states, per axis, and both edges move together.** `peak tension = min(patch tension, M/W_c)` where `W_c = t·D²/6`; and where that composite reading governs, the squeezed edge falls to `max(M/W_c, |σ_n|)` because the bed patch is then not bending at all. **Relieving only the tension edge hides the whole slice**: a forty-five course corbel then fails in COMPRESSION at 13.69 against the 1.25 in tension it is supposed to fail at, and the mechanism is masked by an axis nobody was measuring. Three readings were considered and only this one works — composite modulus *plus* composite bearing area gives 0.2476, and leaving the axial term on the patch reads **exactly zero** and makes everything indestructible.
- **No axial term is subtracted from the composite reading.** The plane resisting a deep-beam moment is vertical; the weight over the joint is shear on it rather than load across it. The relief is refused outright while the joint is in net tension, which is unreachable from gravity on a bed joint and is guarded anyway.
- **THE DEPTH IS BOUNDED BY THE WALL AND BY NOTHING ELSE, AND THE SHEAR-TRANSFER BOUND WAS NOT BUILT.** `FStructure::MasonryDepthAboveCm` — slice 4's cover walk, generalised and now serving both callers — walks up over intact bed joints until the masonry stops, so a raking cut, a missing course and a broken bond each shorten it. No new material data, no tunable constant. A shear-transfer bound was considered and deliberately **not** implemented: it satisfies the same three constraints, it is branching arithmetic, and **no assertion in the suite distinguishes it from this one** — the dry-stone row is vacuous as a discriminator because `DryStone.TensileStrengthMPa` is an exact zero, so any tension is over capacity at any modulus. Building it would be uncovered capability. It stays available if a fixture ever needs it.
- **A composite of one is not a composite.** A piece with nothing resting on it is one unit, so it keeps its own bed patch. That gate changes no corbel row — one course of depth is a *shallower* section than the patch anyway — but without it a lone overhanging brick taller than its own seat is deep gets relief no deep beam is there to supply, which moved three readout fixtures that had nothing to do with this mechanism.
- **The moment is untouched.** Composite action changes the SECTION, never what the wall hands down, so the eleven-rung force and moment ladders are bit-identical and are asserted as such. A moment scale would have been the tempting one-line edit and reads the ladder 18× low.
- **The depth is plumbed as a LENGTH through a third solver-output array**, `ConnectionCompositeDepthCm`, beside the forces and the moments; `UtilisationUnder` and `ApplyForce` gained a defaulted third parameter, and the identity `GetConnectionUtilisation == UtilisationUnder(force, moment, depth)` was extended with it. The joint pairs the depth with its own half-extents, so the axis pairing stays in the one place that knows it.

**Stop at slice 5.** The remaining threads (the triangle as a real load reduction; the thrust propagated down the pier) are genuine follow-ups with nothing blocking them and nothing depending on them.

**AND SLICE 5 PUT TWO ACCEPTANCE CASES INTO DIRECT CONFLICT WITH THE RULING, WHICH IS A PRODUCT DECISION AND NOT AN IMPLEMENTATION DEFECT.** `Acceptance.Wall.Catalogue` went from **10 failing cases to 8** — cases 3, 6, 7 and 11 now pass, case 3 being the ruling itself — but cases **12** and **14** regressed from passing to failing:

- **Case 14, "Corbel, half brick per course", is asserted to COLLAPSE.** It is a ten-course wall corbelling four courses at half a brick each, which is *mechanically the same fixture* as the five-step raking corbel `ACorbelResistsWithItsWholeDepth` asserts **stands** at 0.219. The two cannot both be right. Either the catalogue is wrong about built corbels, or the ruling is wrong about raking ones, and nothing in the physics separates them — a corbel is a corbel whether it was laid that way or cut that way. **This needs the user, not a threshold.**
- **Case 12, "The same span on one-brick piers", is asserted to COLLAPSE** and now keeps 24 of its named bricks up. Same mechanism, less clear-cut.
- **Case 20, the staircase void, improved but did not reach its target**: 59 pieces dropped → **9**, against the 2 the agreed local-loss set names. The remaining seven are bricks the raking cut leaves with a seat that the composite section is not enough for; they are a smaller version of the same question.

**Which slice makes the staircase stop collapsing: slice 5, and that is now the intended outcome rather than a hazard.** The slice that makes the *reported interior* symptom stop is slice 1.

---

## What breaks

1. **MOMENTS_DESIGN's discipline line** — revised, not violated silently. See the top.
2. **`ReceivedMomentUuCm` transfers the capped moment**, so relief propagates downward. That is required by `Structure.cpp`'s own "what travels is what the joint reads", and it means moment is **not conserved** at an arched joint — the missing couple is taken by a neighbour we do not load until slice 3. Same shape and same honesty as MOMENTS_DESIGN's "unconservative otherwise, and recorded as such".
3. ~~**`ConnectionForces` stops being purely vertical** at slice 3.~~ **DONE, 2026-08-07.** A springing now carries `(H, 0, −V)`. `Structure.cpp`'s "straight down, and pointing at whichever end of the joint is being held up" comment is narrowed in place — it still describes what the *accumulation* produces, and says out loud that `ApplyArchingThrust` adds a sideways component afterwards — and `GetConnectionForce`'s contract names the springing as the one exception and gives the readout its line of explanation. **The thrust is applied OUTSIDE the fixpoint**, once the accumulation has settled, which is what keeps every vertical answer in the structure bit-identical: a horizontal component changes no split, no support list, no accumulation order and no moment.
4. **`SolveLoads` gains an O(pieces) group pass.** Measured baseline is 6.4 ms over 1,220 pieces and 3,520 joints since the adjacency index landed, and the gates are false for every piece of an intact wall, so the added cost is a predicate per piece. The abutment predicate must be the cheap one-step form (trap 3), not an exclusion-reachability query.
5. **`PieceInspection` / `BuildPieceMenuInspector` cannot explain an arched joint.** A joint reading 0.014 beside a moment of 570,116 uu·cm has no arithmetic a player can follow. The cap needs to be visible — the same defect class as MOMENTS_DESIGN's item 4, and it should not be a follow-up.
6. **`CURRENT_STATE.md`'s "a cycle is reported, not solved"** entry, which explicitly names the true arch as the case where the model is wrong, is partly answered by slice 2 and should be rewritten rather than deleted: the loop-division rule is still absent, and the arch avoids needing it rather than supplying it.
7. **The staircase support header's ladder comment** claims the corbel receives "NOTHING AT ALL" from the brick along the course, on the grounds that indeterminate loaders carry no moment. Still true, but the reason needs the arch clause added or it reads as a general rule.

---

## What not to do

- **Do not make the arch a support edge.** Trap 1. Two bricks over a hole become a cycle, the solver strands them, and the wall falls for a *new* reason.
- ~~**Do not add composite vertical (deep-beam) action** to fix the free end.~~ **REVERSED 2026-08-06 by the user's ruling, and BUILT 2026-08-07 as slice 5.** It does put the staircase at 0.369 and make the photographed failure stand — the intended outcome, not a side effect.
- **Do not relieve only the tension edge.** Where the deep beam takes the moment the bed patch is not bending, so the squeezed edge falls with the opened one. Relieve one and not the other and a deep corbel fails in COMPRESSION at 13.69 instead of in tension at 1.25, and the mechanism is masked by an axis nobody is measuring.
- **Do not scale the moment instead of changing the section.** It is the tempting one-line edit, it reads the corbel ladder 18× low, and it relieves every joint below the one that arched by a factor nobody derived. The moment is what the wall hands down; the section is what resists it.
- **Do not credit a lone brick with composite action.** One unit is not a composite of anything. Without that gate three readout fixtures with no stack over them move for no reason anybody chose.
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
