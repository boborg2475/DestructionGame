# Destruction Game — Design Summary

A realistic destruction game built in **Unreal Engine 5.8** using **Chaos** (physics + destruction), written in **C++**. Players build structures and destroy them, with every object destructible in a physically believable way — wood splinters, concrete/stone fractures, glass shatters, and structures collapse under their own weight when their support is removed.

This document collects the design decisions ("checkpoints") made during planning. It's meant as the source of truth to pick up from at implementation time.

---

## 1. Core Vision

- Everything the player builds is destructible.
- Breaking behavior is material-specific and physically believable.
- Structures have **real-time structural integrity**: pull out enough support and the structure collapses under gravity alone — no shove required, the last piece removed is the "final straw."
- Multiple force types can be applied: explosions (radial) and kinetic impacts from large objects.
- Debris stays live: broken pieces carry momentum into other pieces, causing secondary collisions.

---

## 2. Core Architecture

Three cooperating pieces sit at the center of the design:

1. **Base destructible actor** — a C++ actor holding the shared logic for taking damage, applying impulses, and reporting hits to the structural layer. Every wall, floor, and column inherits from it. Key properties: a **damage threshold** (how much force before it breaks) and a **connection strength** (how stubbornly pieces cling together — this is what makes a structure topple believably instead of crumbling in place). It exposes a function like *apply damage at a location* that takes a hit point and a force.

   **Superseded in one respect *(2026-08-01)*:** this originally wrapped a Chaos **Geometry Collection** component and let Chaos decide which chunks come loose. It does not. A spike settled that pieces are **kinematic while intact**, that we compute the loads and decide the breaks ourselves in SI units, and that Chaos takes over only once a piece is released and is falling. The reason is directional strength — see "Geometry collections: complementary, not an alternative" in §3. Geometry collections remain the right tool for a *single piece fracturing into fragments*, which is a later, additive concern.

2. **Material profile system** — each destructible carries a **data asset** describing its material (wood, concrete, stone, glass, etc.). This drives fracture pattern, damage thresholds, and break behavior. Materials are **data, not code**.

3. **Damage / force manager** — handles incoming hits, explosions, and radial forces, then tells the right actors how hard they were hit.

### How a force becomes a broken joint

The directional layer is a pipeline of small, world-free steps. Each stage is plain arithmetic on plain structs — no actor, no world, no ticking solver — which is what keeps it cheap to test and cheap to reason about:

```
world-space force
        │
        │  FConnection::ApplyForce()        Core/Connection
        │  owns: interface normal, interface area, strength profile
        │
        │     ├─ ClassifyForce()            Core/ConnectionLoad
        │     │      ▼
        │     │  FConnectionLoad { Compression, Tension, Shear }
        │     │
        │     └─ ComputeUtilisation()       Core/ConnectionStrength
        ▼
utilisation ratio   (0 = unloaded, 1 = at the limit, >1 = the joint gives)
```

`FConnection` is the entry point: a caller supplies only a force, because the joint already knows its own orientation, area and material. The two stages beneath it stay separately testable, but nothing outside should be calling them directly — see the caller obligation below.

Two properties of that shape are deliberate:

- **A ratio, not a boolean.** The same number drives the break decision *and* the on-screen strain readouts, so what the player sees is the quantity the simulation actually used.
- **Stress, not force.** Strengths are in megapascals, so the comparison needs an interface area. The same force through half the area is twice as punishing; a force-only model would let thin joints survive loads that should part them.

**Giving is irreversible, and a given joint carries exactly nothing.** Once a connection's utilisation passes 1 it has given, and it stays given however the load changes afterwards. Mortar does not re-bond. A joint that healed itself when the load dropped would make collapse non-monotonic — a wall could shed a joint, redistribute, recover it, and stand back up mid-fall. The zero is equally load-bearing: redistribution works by pushing load onto neighbours, which is only correct if the broken joint is genuinely out of the structure.

One asymmetry is deliberate: the *breaking* call reports the ratio that broke it, above 1, and only subsequent calls return zero. That distinguishes "gave, at this strain" from "gave silently", and it is the number a strain readout should show at the moment of failure.

**Caller obligation.** A degenerate interface normal — zero length, or NaN — makes `ClassifyForce` return a zero load. That is right in isolation, but downstream a zero load is indistinguishable from "unloaded and perfectly healthy", so composing the two stages naively produces a joint with no interface plane reporting itself as fine. `FConnection` closes this by substituting a zero interface area, routing the case through the area guard that already fails closed. **Anything that calls `ClassifyForce` and `ComputeUtilisation` directly rather than going through `FConnection` re-opens the hole and must make the same check.**

Everything above sits *in front of* Chaos. Chaos still owns the rigid bodies, the contacts and the debris — this layer decides when a connection has had enough.

> For what is built versus still planned, [CURRENT_STATE.md](CURRENT_STATE.md) is authoritative. This document describes the intended design and does not track progress.

### Materials are directional
Materials do **not** have a single strength value. They respond differently depending on the **direction/type** of force:

- **Stone / concrete**: strong in compression, brittle under shear and tension (crack fast when twisted or bent).
- **Wood**: flexible; handles shear and bending far better; fails by splintering along the grain rather than shattering.

Chaos connections, out of the box, use a **single strain threshold** and don't distinguish force types. To get real behavior, the material profile stores **separate directional strengths** (at minimum compression and shear; tension too). Custom C++ computes an incoming force's direction relative to each connection and applies the right threshold.

### Connections are first-class objects
A **connection** between two pieces is its own thing with its own directional strength profile (compression, shear, tension) — just like a material.

- Mirrors reality: a brick wall usually fails because the **mortar** gives before the brick; a wood joint fails because the **nail/screw** goes before the timber.
- Connection types are data profiles: mortar, nail, screw, bolt all differ.
- The in-game builder **chooses the connection type**, so the same wood frame built with nails vs. screws vs. bolts genuinely behaves differently under load. (Realism + a gameplay hook.)

### Force direction is relative to the connection interface
Force is classified by its direction **relative to each connection's interface** (the plane where two pieces meet), **not** by world direction:

- **Compression**: force perpendicular into the interface, squeezing the two faces together.
- **Shear**: force parallel to the interface, sliding the two faces past each other.
- **Tension**: force pulling the two faces apart.

> Example: remove the brick beneath another, and gravity (pointing straight down) becomes a **shear** load on the *vertical* joint to the neighbor. The same downward gravity is *compression* on a horizontal joint and *shear* on a vertical one. "Sideways" is the wrong mental model — orientation relative to each joint is what matters.

### Geometry lives above the solver, and produces the graph *(settled 2026-08-02)*

Everything above **consumes** interface areas and normals. Something has to **produce** them: a scenario places bricks, and the areas, the normals and the pairs have to come from that placement. That producer is `Core/Layout`, and three things about it are design decisions rather than implementation.

**The dependency runs one way only.** The producer knows about the solver; the solver never knows about the producer. `FStructure` stays position-free — a piece is a mass and an identity — which is exactly why the load maths needs no world and runs deterministically in milliseconds. Geometry sits above it and hands down handles.

**The interface normal is the axis of separation, oriented by which handle is `PieceB`.** It is *never* the direction between the two pieces' centroids, and the sign is read on the separation axis alone. This is the one rule the producer exists to get right, because getting it wrong is silent: a running-bond bed joint's centroid direction has `|Z| = 0.5547`, which is **below cos 45°**, so a centroid normal makes every bed joint in a wall classify as a **head** joint. The head tier is sign-blind, so each brick then treats the joints *above* it as supports as well, and gravity resolves as **shear against mortar's cohesion** instead of compression against its compressive strength — an entirely wrong support graph, and nothing breaks, moves or crashes. Displacement could never detect it; only the classification can.

Worked through on the spanning-brick case (1333.5993125 uu over 105.0625 cm², general-purpose mortar): the centroid normal splits the load into 739.7478 uu compression and 1109.6217 uu shear, giving a worst utilisation of **5.269638e-3 against the correct 1.269339e-4** — a factor of **41.5**. An earlier draft of this paragraph said 79×; that figure does not reproduce and was arrived at by comparing shear utilisation to compression utilisation *under the wrong normal*, which is not this comparison.

The failure mode that matters is not a *flipped* normal. `GetJointRole` turns the normal toward whichever piece it is asked about and the accumulation signs the force by which end is loaded, so a **consistently** flipped joint reports identical loads. The bug is a normal **inconsistent with its A/B pairing** — which is why the pair and the normal are emitted as one atomic value, by one function, rather than assembled at a call site.

**Deciding *whether* two pieces touch and deciding *which pairs to consider* are separate jobs**, and only the second is bond-specific. A running-bond producer is generative: it knows what it laid, so it offers its own neighbours rather than searching for them. A future contact-finding producer replaces exactly that half and inherits the areas, normals, orientation and validation unchanged.

A refused pair **fails closed**, by zeroing the interface area rather than leaving the joint untouched — the same trick `FConnection` uses for a degenerate normal, routing the case through the area guard that already exists so a caller who ignores the return value gets a joint that reads as *failed* rather than one that reads as fine.

---

## 3. Physics Model

### Structural integrity under gravity
- Each piece has real **mass** based on its size and material density (a brick weighs like a brick).
- Pieces are held by connections; each connection bears a limited load.
- Remove a piece and its load **redistributes** to its neighbors. If they can carry it, the structure stands. Keep removing pieces and eventually a connection overloads, snaps, and the failure **cascades** — collapse under the structure's own weight. **We compute this ourselves** — see "How a structure comes apart" below. Chaos is not consulted about whether a joint gives; it takes over once a piece is released and is falling.

### How a standing structure holds itself up

Decided 2026-07-31, by measurement rather than argument.

**Pieces in an intact structure do not simulate.** They are kinematic, and we compute the load each connection carries ourselves. When a connection gives, the pieces it freed switch to dynamic and Chaos takes over the falling, the collisions and the debris.

The alternative — letting Chaos hold the structure together with physics constraints and reading each constraint's force — was built as a two-brick spike and measured against this one. It lost on both counts:

| | Kinematic | Chaos constraint |
|---|---|---|
| Drift while nominally still | **0.000000 cm** | 0.62–0.70 cm, oscillating |
| Reported joint force | n/a | 16,873–24,147 uu, ±20% |
| *Expected* joint force | — | 2,670 uu |
| Lateral force under a purely vertical load | n/a | 3,400–6,300 uu |

Two things follow. The constraint force is roughly **8× too large, unstable, and carries a large spurious sideways component** — and since our whole model turns on splitting a force by direction, a bogus lateral term corrupts precisely the quantity we care about. And a "locked" constraint **sags 6–7 mm and never settles**, which makes the redistribution test in §4 unwritable: that test reads strain while requiring that *nothing moves*, and calls any drifting brick a hard fail. Kinematic pieces drift exactly zero.

The cost is that we own the load maths instead of asking Chaos for it. That is a fair price: it is plain arithmetic over a graph, needs no world, and is therefore fast and deterministic to test — the same property that makes the force classification cheap to trust.

Releasing a piece to dynamic was measured too: it settles within 0.018 cm and stays put. That transition is the mechanism collapse is built on.

> Also established by the spike: physics simulates normally under `-nullrhi`, so integration tests can run headless.

**Read a mesh's scale AND its pivot off its own bounds. Never hard-code either.** *(Corrected 2026-08-04 — an earlier version of this note said `SM_Cube` is authored at 100 uu "so brick scale is simply dimension ÷ 100", which invited exactly the two constants that produce a plausible-looking wrong wall.)*

The 100 uu figure is true of the current placeholder and of nothing else, so `/ 100.0` is silently wrong the day a real brick mesh lands — and a wrong scale gives a wall that looks entirely reasonable. Worse, and only found by measuring: **`SM_Cube`'s pivot is at a corner**, min (0,0,0) max (100,100,100), not at its centre. Placing an actor at a piece's box centre therefore puts the brick a half-size out on **all three axes** — 10.75 × 5.125 × 3.25 cm for a full brick — while `GetActorLocation()` agrees with the layout perfectly. The wall sits visibly off its own grid and every position readout says it is fine.

Both come off `GetBoundingBox()`: scale is `BoxSize / LocalBounds.GetSize()`, and the origin is `Box.CentreCm - Scale * LocalBounds.GetCenter()`. This is also why a spawned-brick test must assert on `GetComponentsBoundingBox()` rather than `GetActorLocation()` — bounds-versus-box is pivot-agnostic *and* catches a 100× scale error in the same comparison.

### How load reaches the ground

Decided 2026-07-31. DESIGN.md had long said load "redistributes" without ever saying how; this is how.

Pieces carry a mass and a flag for whether they are **grounded**. Load flows downward: a piece transmits its own weight plus everything it receives from above, into the connections that support it. A grounded piece absorbs what reaches it — that is the earth. Where several connections support a piece the load **splits weighted by interface area**, so equal areas split evenly.

**A piece is unsupported when it genuinely has no load path to the ground.** That is what "it fell" means, and it is a statement about the structure rather than about the solver. A brick resting squarely on the earth is standing up no matter what is happening above it, and a piece whose load merely cannot be *computed* is not thereby falling.

There is exactly one deliberate exception, below: pieces caught in a knot the ordering genuinely cannot resolve are reported falling, because the alternative is inventing a division rule this design does not have. It is conservative and it is confined to the knot itself.

Three things follow, and they are separate:

- **Supports that are themselves falling take none of the load.** They are excluded from the split entirely, not merely credited and ignored, so the whole load goes to the supports that actually reach earth. Otherwise the joint that *is* carrying the piece under-reports by the share that evaporated — and a joint at 1.9× utilisation reading 0.95× is a joint that never breaks.
- **Load that cannot be routed is reported as falling, and only for the pieces actually caught in it.** Mutually supporting pieces with no resolvable order cannot have their load divided without a rule this design does not yet have, so they are conservatively treated as unsupported. Pieces *beneath* such a knot keep their support and carry everything except the unroutable contribution. Letting un-orderability cascade downward strands foundations and is simply wrong.
- **Stranding travels upward, never downward.** A piece resting only on a knot has no load path to the ground either, so it falls with it. This is the other half of the rule above and follows from the same definition, but both halves need saying: the failure that reached review was the downward direction, and the upward direction is what makes the collapse propagate correctly.

> A cycle is therefore **reported, not solved**, and §4's shear test will eventually want a real rule for dividing load round a loop.
>
> **The arching case is CLOSED, and it was closed by avoiding the loop rather than by supplying one** *(2026-08-06, [ARCHING_DESIGN.md](ARCHING_DESIGN.md) slice 2)*. A course spanning a wide gap used to read as falling; it now spans. The naive fix — letting a seatless piece be *supported* by its neighbour through the head joint — is precisely the two-node cycle described above, so it would have turned "the wall bridges the hole" into "the wall is unroutable and falls", which is a worse answer reached through correct-looking physics. Instead a **group** of seatless pieces re-seats onto abutments *outside* the group, which is acyclic by construction. **The loop-division rule is still absent**; nothing here supplies it.

Because Unreal's gravity is 980 cm/s² and mass is in kilograms, `weight_in_force_units = mass_kg × 980` directly — the 1 N = 100 uu conversion is already inside that number and must not be applied again.

**Support is two-tiered: a piece rests on what bears it, and only hangs from its neighbours if nothing does.**

A connection's interface normal already says which kind of joint it is. A substantially vertical normal is a **bed joint**, which can bear weight in compression; a substantially horizontal one is a **head joint**, which can only carry weight in shear. So a piece's supports are its bed joints below it — and *only* if it has none does it fall back to its head joints.

**The line is at 45°** — a joint counts as bearing when its normal is closer to vertical than horizontal. That is the one angle needing no justification beyond symmetry, and anything else would be a tuning knob pretending to be physics.

Two consequences of the tiering are easy to state wrongly, so state them precisely:

- **A bed joint *above* a piece is not a support.** You do not rest on the thing over your head. It is excluded from the bearing tier entirely rather than bearing in tension.
- **A head joint is not purely a shear joint.** The head tier spans everything from 45° to 90° off vertical, and the tier is deliberately blind to which side the joint sits on. So a piece hanging from a steeply inclined face above it takes a genuine **tensile** component — at 46° off vertical, most of the load. That is not a modelling error; a brick hanging off an inclined face really is being pulled off it. Because mortar's tensile limit is roughly a hundredth of its compressive one, tension becomes the governing axis and such a joint gives far sooner than a bearing joint carrying the same load would.

  That is a statement about which axis decides, not about any particular load. A single brick on a 100 cm² joint reaches about 0.017 utilisation and breaks nothing at all — the tensile axis governing means it fails first, not that it fails immediately.

The 45° line therefore looks like a cliff and is not one. Just below it a joint above a piece bears nothing and the piece falls; just above it the joint becomes a support that immediately fails in tension, and the piece falls. Same answer, different route.

**Accumulation runs in dependency order, not by distance to the ground.** Distance is not a valid ordering here: a piece spanning a gap and the piece resting on top of it are the same distance from the earth, yet one loads the other. The order has to come from the support relation itself — process a piece only once everything resting on it has been processed.

This tiering is not decoration, and the first attempt without it was wrong in the exact case the game is about. Routing purely by graph distance to the ground let a short sideways path exclude a bed joint entirely: a brick spanning a gap ended up at the same distance as the brick resting on top of it, so the joint between them carried **zero** and the keystone bore none of the wall above. The utilisation on its head joints was 0.010 and stayed there however many courses you piled on. It also made the answer depend on bond pattern — running bond routed 0% onto the spanning brick, stack bond 33% — with no physical reason for either.

The consequence matters for §4's shear test: *"pull bricks from below so a section spans a gap like a keystone; gravity shears it against the vertical joints."* That only fires if the keystone genuinely receives the weight above it, which needs the bed joint onto it to win over a lateral path.

The lesson generalises: **classification was direction-aware from the start, but routing was blind, and routing decides where the load ends up.** Anything added here that reasons about connections without consulting their orientation should be treated as suspect.

> **Known limitation.** Tiering on "substantially vertical" bakes in gravity as the only load direction. Once explosions and kinetic impacts arrive (§6), routing has to generalise to the direction of the applied load rather than assuming down.

### How a structure comes apart

Decided 2026-07-31. This design has said from the start that load "redistributes" and that failure "cascades"; this is the rule that makes those words mean something.

**Break every joint over capacity in the same pass, stamp each with the pass number that broke it, then re-solve and repeat until a pass breaks nothing.**

A pass is a complete solve followed by one sweep. The sweep evaluates every intact joint against the load the solve gave it and breaks all of them that are over their own capacity — not the worst one, all of them. Re-solving then moves their share onto whatever supports are left, which may put a neighbour over capacity and cause the next pass. A pass that breaks nothing is the last one and is not counted; the structure has settled, and nothing standing is above its capacity.

**A joint that has given leaves the support relation entirely.** It is not merely unloaded — it drops out of the two-tier decision, out of the reachability walk, and out of the load paths, exactly as if it had never been built. That is what redistribution *is*: the share moves onto the neighbours rather than evaporating.

The tier is the part that is easy to get half right, and getting it half right produces a plausible-looking wrong answer. A broken joint that stops carrying load but still *wins* the bearing tier leaves the piece above it with an empty support list, so the piece is reported falling and its perfectly good head joint reports zero — self-consistent, and a wall that collapses where it should have leaned. The piece is meant to fall back onto its head joints and load them in shear.

**The pass number is the record of the order.** `FConnection`'s latch says only *whether* a joint gave; the stamp says *when*, and it is never rewritten, because joints never heal. It is what §5's visualisation and the piece-creation timestamps play back — the difference between watching a building come down and being handed a pile of rubble.

**Pass numbers belong to the structure, not to the cascade that wrote them.** A structure's collapse is one sequence however many times it is cascaded, so a later cascade continues the numbering from the highest number already stamped rather than starting again at 1. This only becomes observable once **removal** is in the loop — a second cascade on a settled structure breaks nothing, so before removal existed no two cascades could both stamp — and removal is the player's move, so it is the normal case rather than a corner of it. Numbering that restarted would put the joint that failed first and the joint that failed after a player pulled a brick out on the *same* number, and consumers read a shared number as "these gave simultaneously": the collapse would replay in the wrong order with nothing reading as inconsistent anywhere. **How many passes a given cascade ran is a separate question** — useful to whoever asked for it, to know whether their removal did anything — and the two must not be conflated.

Two alternatives were considered and rejected:

- **Strict worst-joint-first** — break only the single most overloaded joint, then re-solve. It reaches the *same settled state* at the cost of one full solve per broken joint, and in exchange it invents a sequence where there is none: three independently overloaded joints on three unconnected piers become passes 1, 2 and 3 despite nothing connecting them and their utilisations being equal to the last bit. A false ordering is worse than no ordering, because the visualisation will show it.
- **Unordered all-at-once** — break everything over capacity and re-solve, keeping no pass number. Cheapest, same final state, and it throws away the one thing that makes a collapse watchable. Losing the sequence loses the collapse.

**Termination is structural rather than a bound to be tuned.** Joints never heal, so every counted pass permanently removes at least one connection, and there can be no more passes than there are connections. That is a different and much larger bound than the fixpoint *inside* a single solve, which exists because stranding changes who reaches the ground and which runs nested inside each of these passes.

> Solving stays non-destructive. `SolveLoads` computes what a structure carries and breaks nothing however overloaded it is, so anything — a strain readout, a what-if — can ask a structure what it is carrying without damaging it. Breaking is only ever the deliberate step.

### Removing a piece takes it out of the graph

Decided 2026-08-01. Removal is the player's move: the MVP brings a wall down by **pulling pieces out of it** and by **loading it from above**, both of which the existing model already supports, while arbitrary-direction force is deferred because the support routing is gravity-specific. That decision and its reasoning live in [CURRENT_STATE.md](CURRENT_STATE.md) under the force-delivery entry. What removal means *to the load model* belongs here, beside how load reaches the ground, rather than in the code that happens to implement it.

**A removed piece is not in the structure at all.** Every joint that held it goes with it, so it supports nothing and nothing supports it. If it was **grounded it stops being ground**: it is no longer resting on the earth, and it no longer conducts the earth to anything above it. That last clause is the one that has to be said out loud, because a grounded piece is a root of the reachability walk on its own account rather than through any joint, so removal expressed purely as "take away its joints" leaves it reporting itself held up by an earth it is not touching.

**Removal is not failure, and the two must stay distinguishable forever.** A joint that went with a removed piece never snapped — it was deleted — so it carries no pass number and never appears in the collapse sequence §5's visualisation plays back. Whether a joint is still in the structure and which pass broke it are different questions, and the pair answers all three states with no sentinel value:

| | still in the structure | broke in pass |
|---|---|---|
| intact | yes | — |
| went with a removed piece | no | — |
| failed under load in pass N | no | N ≥ 1 |

A sentinel for the middle row was considered and rejected: it would cost every consumer of the sequence a special case, in exchange for a distinction the pair already draws.

**What follows is an ordinary cascade.** Removal itself breaks nothing. It changes the graph, the next solve re-routes what the removed piece was carrying onto whatever is left, and if that puts a joint over capacity the rule above takes over unchanged. So "pull a brick and the wall comes down" needs no separate mechanism — it is redistribution and cascade, on a graph with a hole in it.

> **Handles are stable across removal, and that is a model-level promise rather than an implementation detail.** A piece handle means the same piece for the lifetime of the structure; removal leaves a hole rather than closing the gap. Anything that renumbered pieces would silently re-point the joints above the hole, and the break stamps — the only record of the order a collapse happened in — cannot be recomputed from anything, so they would be wrong permanently and consistently.

### Shear capacity depends on load (Mohr-Coulomb)

Decided 2026-07-30. A joint's resistance to sliding is **not a fixed number**:

```
shear capacity = cohesion + μ × compressive stress
```

Cohesion is the bond itself; the second term is friction, and it grows with how hard the joint is being squeezed. Only **compression** contributes — a joint being pulled open gets no friction benefit. Compression and tension keep their own independent limits; only shear became load-dependent.

Why it earns its place:

- **It is why masonry stands.** A dry-stone wall has no bond at all — cohesion is zero and it holds purely on friction from its own weight. Without coupling it cannot be modelled.
- **It makes damage progressive.** Cohesion is a bond: once cracked it is gone, and friction is the only thing left. Real damaged masonry keeps carrying load this way, which is why partly collapsed buildings stand rather than unzipping.
- **It compounds collapse.** Load redistribution alone already cascades; this adds a second mechanism where removing support *lowers the capacity* of the joints above, not just raises their load.

Honest scope: for a low mortared wall the friction term is only a few percent of capacity — bond dominates. It becomes first-order in tall structures (~50% at 10 m), in dry-stone, and in any joint whose bond has already broken.

**μ = 0 reduces the model exactly to independent axes**, which is the right answer for mechanical fasteners — a bolt does not care how hard the pieces are pressed together. That is what keeps connection types data rather than separate code paths.

### A fastener's strength is smeared over the interface

Decided 2026-07-31, and it is a modelling approximation rather than a detail.

Everything in this design compares a **stress** against a strength, because that is what makes a joint's area matter. But a fastener's published capacity is a **force per fastener** — EN 1995-1-1 gives a nail so many newtons in withdrawal, a bolt so many kilonewtons in shear — and a joint has no idea how many nails are in it.

So the nail, screw and bolt profiles hold their code-derived capacities **divided by a reference density of one fastener per 100 cm² of interface**. That number is an assumption the data cannot currently express, and every figure in those three profiles scales with it.

Two consequences worth stating plainly:

- **A `Nail` profile does not mean "a nail".** It means "a nailed joint at the reference fastener density". Doubling the nails in a real joint should double its capacity, and today it does not — the profile is the only knob.
- **When fastener count becomes a property of a joint, these are re-derived, not re-tuned.** The kN figures and the formulae they come from are recorded in the source comments precisely so that recalculation is possible rather than archaeological.

The masonry profiles have no equivalent problem: mortar genuinely is a continuous bond across the whole face, so its MPa figures are physical as they stand.

**The envelope is truncated.** Friction cannot help forever — past a point the material gives rather than the faces sliding — so capacity is clamped to a per-profile ceiling. Eurocode 6 puts that near 0.065 of the unit's compressive strength, around 1.3–2.0 MPa for clay brick.

Without the cap, capacity climbs with depth and joints at the base of a tall structure become effectively uncuttable, which is backwards twice over: unphysical, and wrong for a demolition game where the base is exactly where cutting should work. The ceiling defaults to unbounded rather than zero, so an unset cap behaves as plain Mohr-Coulomb instead of producing an accidentally rigid joint.

### Known limitation: stress is averaged over a joint, so there are no moments *(recorded 2026-08-04)*

A joint's utilisation is computed from a force over an **area** — one average stress across the whole interface. Real joints do not fail that way. A brick overhanging its support pivots on the outer edge of its bearing: compression spikes at that edge while the inner edge is pulled *open*. The opened edge governs, because mortar holds roughly **10 MPa in compression and about 0.1 MPa in tension** — a hundred to one.

So the model reports a bed joint under a modest, uniform, comfortably-safe compression while the real joint peels apart from the back. It is the same signature as every other defect this subsystem has produced: **a plausible number, and a piece that is not really being held.** This is why real masonry limits how far a course may corbel out, and it is the same mechanism that makes arches and vaults work — thrust kept inside the joint so no part of it opens.

Closing it needs edge stresses rather than an average, which needs a section modulus, which needs joints to have extent and pieces to have position — see §6's spatial layer. **CLOSED for a piece on exactly one support** *(2026-08-06)* — see [MOMENTS_DESIGN.md](MOMENTS_DESIGN.md). A brick on one head joint reads 0.4157 in tension where it read 0.020 in shear, and the cascade now runs on the world wire, so cantilevers that should peel do peel. Still open for a piece on several supports, where the area split keeps the moment at zero.

### Arching: the load path, not the joint *(recorded 2026-08-07, [ARCHING_DESIGN.md](ARCHING_DESIGN.md))*

Moments made cantilevers peel correctly, and that immediately exposed the larger error: **the model only ever routed load downward**, so a brick that lost a seat was asked to *hang*, which is the one thing masonry cannot do. Deleting one brick made the failure walk across a wall at 33.69° — the bond pattern, one half-cell per course. The strengths were never the problem; the published 0.10 MPa flexural and 0.20 MPa cohesion are unchanged and must stay so.

Five slices, all on top of the moment work, and **none of them added a material property or a per-material branch** — the only new constant is `SolverArchingDepthPerSpan = 0.866` with its BS 5977-1 citation:

- **The moment cap.** A half-seated joint with an intact head joint on its *eccentric* side, to a neighbour that reaches the ground independently, has its moment capped so the thrust line sits on the kern edge. Peak tension goes to exactly zero and peak compression to exactly `2|σ_n|`.
- **Spanning.** Seatless pieces form a group and re-seat onto abutments outside it. This is what closes §3's arching gap above.
- **The thrust.** An arch pushes sideways; the springing carries it in shear on its own bed joint. `ΣH = 0` is structural — one stored direction, two signs.
- **The cover cap.** Arching depth is `min(cover, 0.866·L)`, so an opening near the top of a wall cannot arch under nothing.
- **Composite vertical action.** A corbelled joint resists with `t·D²/6` over the bonded depth rather than one bed patch, floored by a `min` against the patch.

**Two decisions here were the user's, not the model's**, and both are recorded because no arithmetic settles them: a brick deleted at a free end must not bring a wall down *(2026-08-06)*, and a bonded corbel stands however far it steps *(2026-08-07)*. The second knowingly gives up the ability to express a corbel failing by projecting too far — that failure is **global overturning of the corbelled mass about the wall face**, a stability check this model has never had, because it only ever checks joints.

**And the composite depth is UNBOUNDED, which is a live hole rather than a settled choice** *(found in review, 2026-08-07)*. Every corbel fixture happens to have its wall stop where its corbel stops, so nothing distinguishes a bounded depth from an unbounded one. In a wall taller than the cut — which is the wall the game actually renders — the reading goes as `k³/m²` rather than `k³/k²`, and the "it crosses over near 36 courses" argument in ARCHING_DESIGN does not hold. See CURRENT_STATE.md.

### Force delivery (designed in principle, not yet detailed)
- **Explosions**: radial falloff — a blast on a corner hits that point hardest and weakens as it spreads. (Chaos fields do this.)
- **Kinetic impacts**: large objects striking the structure.
- Secondary collisions: debris carries momentum into other pieces and can knock more loose.

### Real-world scale and units

Objects are built at **true real-world dimensions** — a brick measures what a brick measures.

**World scale is Unreal's default: 1 uu = 1 cm.** Decided 2026-07-30. Unreal's physics is a cm / kg / second system throughout, and fighting that default costs more than it returns.

Most of the physics already speaks real-world values, verified against the UE 5.8 source:

| Quantity | Unreal unit | Conversion |
|---|---|---|
| Mass | kilograms (`SetMassOverrideInKg`) | none — use published values directly |
| Density | g/cm³ (`UPhysicalMaterial::Density`) | none — concrete 2.4, steel 7.85, oak ~0.75 |
| Gravity | `-980.0` (= 9.8 m/s²) | none |
| Length | centimetres | mm ÷ 10 |
| **Force** | kg·cm/s² | **1 N = 100 uu of force** |
| **Impulse** | kg·cm/s | **1 N·s = 100 uu of impulse** |

Force is the only real trap. Because length is centimetres rather than metres, a newton is 100 Unreal force units. Left implicit, everything ends up wrong by exactly 100× — which is easy to paper over with fudged thresholds and hard to spot afterwards.

**Convention:** material and connection strengths are stored in **real SI units (MPa)** in their data assets, so they stay checkable against published tables. Conversion to Unreal force units happens at **one named, tested boundary** where loads meet strengths — never scattered through call sites.

> **Chaos strain is not a physical quantity**, which is part of why we do not use it for the structural decision. The strain thresholds on a geometry collection's connections are solver-tuning numbers, not newtons, and they are **scalar** — one value per bond, with no notion of which direction the load came from. Our whole premise is that direction decides: the same force is harmless in compression and fatal in tension, roughly a hundred to one for mortar. A calibration from "this joint carries X newtons" to "this Chaos connection has strain threshold Y" would be the seam where physical realism quietly leaks, and we avoid needing it by deciding breakage ourselves in SI units.

### Geometry collections: complementary, not an alternative *(settled 2026-08-01)*

The question comes up naturally — Chaos already has a connection graph with breakable bonds, so why not use it? Because **it answers a different question**, and the two layers compose rather than compete.

- **This system decides which piece gives way, and when.** Whole pieces with real mass, joints whose strength depends on the direction of the load, and a graph built at runtime from what is actually touching what — which changes as things break, and which a player-built structure has no authored asset for.
- **A geometry collection decides what a single piece looks like as it dies** — the splintering, fracturing and shattering of §6's visual break patterns. Its graph is baked into the asset at authoring time from fragment proximity, and it breaks on scalar strain.

Adopting geometry collections for the *structural* decision would trade direction-dependent strength for a single threshold, which is the one thing this project exists to do differently. Adopting them *later, per piece*, is additive and costs nothing now: `FStructure::RemovePiece` is already the seam — the structural layer says "this piece is gone", and what replaces it visually is a separate concern.

**Note what this does not solve.** Force delivery is often assumed to be the blocker here; it is not. Chaos field systems can apply radial force and strain, and ordinary actors take radial impulses without any of that. The blocker for sideways loads is that **our support routing is gravity-specific** — a piece's supports are the bed joints *beneath* it, and a blast from the side makes "beneath" meaningless. That is our limitation to generalise, and no geometry collection fixes it.

**Still open:** fragments that fall and come to rest cannot support anything, because joints are only ever destroyed, never created. Same gap as rubble bearing load.

### Minimum size floor (piece identity) + three tunable modes
Pieces do **not** subdivide infinitely. There's a **tunable minimum volume** (start ~thumbnail size, roughly 15–20 mm across — that's only 1.5–2 uu at 1 uu = 1 cm, so expect small numbers here) below which a piece cannot break further. This also solves *piece identity*: each piece has a finite, traceable life — born at a timestamp and never subdividing past the floor.

When a piece would break below the floor, one of three **selectable modes** applies:

| Mode | Behavior | Purpose |
|------|----------|---------|
| **1 — Indestructible** | Piece can't get smaller, but stays a full physics object (still collides and moves). | Baseline that proves the floor works. |
| **2 — Dust particles** | Piece becomes **Niagara** particles that settle as dust on the ground (likely via decals). No per-particle physics objects. | The full realistic feature (a feature in its own right). |
| **3 — Disappear** | Piece simply vanishes. | Proves threshold detection fired, without needing the particle system built yet. |

**Implementation path:** build modes 1 and 3 first (they prove the threshold logic simply), then layer in mode 2 for the full effect. Working and testable at every stage. The minimum volume itself stays a single tunable setting.

### Piece timestamps (inspect, don't log)
Rather than external logs, **each piece carries its own field: a creation timestamp**, stamped the moment it breaks free. You inspect the object to see when it fractured, and recover the **sequence** of failure by comparing timestamps across pieces. This is inspectable live during gameplay and lets you analyze a collapse after the fact.

---

## 4. Testing Strategy

The whole approach separates **unit tests** (isolate one behavior) from **integration tests** (realistic scene), and defines failure differently for each.

### Force isolation: two layers
- **Unit tests — gravity off, objects floating.** With gravity gone and only the test force present, the connection's response is a clean reaction to one input.
  - **Tension**: pull the object / connected pieces apart.
  - **Shear**: slide two objects across their shared connection (tested both on a single material and across a connection).
  - **Compression (crushing)**: pin the object against an immovable surface (e.g. the ground), then press from the opposite side.
- **Integration tests — gravity on, everything connected.** The grounded brick-wall tests prove it still holds up in a messy, realistic scene. If a grounded test fails while its floating counterpart passed, the problem is **interaction**, not the core force math.

### Defining "failure" (matched to test intent)
Failure is defined by **what the test is trying to prove**, not one universal rule.

- **Unit tests → assert on the mechanism.** Signal is **connection state** (intact → severed) or a brick breaking into pieces. Binary, repeatable, immune to physics jitter. *Distance traveled is a trap* — two pieces can sever their bond but stay resting exactly in place (zero distance, yet genuinely broken).
- **Integration tests → assert on the outcome.** For the wall, a connection breaking is only a step; the real intent is *did the wall collapse?* A single mortar joint could sever while the bricks stay leaning together and the wall holds — measuring only connection state would falsely pass that. So integration tests need a measure of the whole structure actually moving/falling.

### The test catalog

**Structural behavior (integration, gravity on):**
- **Redistribution test** — single brick wall; remove one brick; read the actual strain on surrounding connections. **Pass:** neighbors' combined strain increases by ~what the removed brick was carrying (within tolerance) **and** the wall doesn't move. Any brick drifting is a hard fail.
- **Collapse test** — same wall; pull bricks until it topples; confirm it falls at the **predicted number** from the baseline brick strength (e.g. stands at 4 removed, falls at 5). Topples too early → too weak; survives too long → too strong.

**Directional force validation:**
- **Shear failure** — pull bricks from below so a section spans a gap like a keystone; gravity shears it against the vertical joints (where brick/stone is weak). Validates the shear threshold.
- **Compression failure** — stack weight on top, loading horizontal joints perpendicularly; brick is strong here. **Key validation:** compression should tolerate **significantly more** force than shear. If the two numbers come out close, the directional logic isn't really working.

**Fracture isolation (change one variable at a time):**
- **Single brick** — apply a known force; confirm the brick fractures **at** its threshold (holds just below, breaks just above); no connections involved. Isolates pure material fracture.
- **A few connected bricks** — force in a window **above** the connection threshold but **below** the brick's own; confirm **only connections break** and bricks stay whole and countable.
- **Full wall** — confirm both failure modes coexist correctly, with connections generally giving before bricks.

**Size-floor behavior (same setup, opposite outcomes):** object already at minimum size + a crushing force clearly above the fracture threshold.
- **Mode 1 (indestructible)** — assert the object **still exists** and its piece count hasn't increased (may be knocked around, but nothing subdivides).
- **Mode 3 (disappear)** — assert the object count is **zero** afterward (detection fired, piece crossed the floor).

### The material × force matrix
- One **reusable, parameterized** test, written once, taking a **material + force type** (and later **+ connection type**) as inputs and checking the outcome against an expected value.
- Forms a grid: materials down one side, forces across the top; each cell is an expected behavior. This grid doubles as documentation of how the material system should behave.
- **Core force types:** compression, shear, tension. (Bending = tension on one side + compression on the other; torsion = twisting — both are combinations, noted for later.)
- **Grounded in real-world data:** expected values come from published material strengths (compressive / shear / tensile strength, in megapascals). Pick **one reference material** as the baseline (concrete or steel are well-characterized), calibrate it to feel right in-game, then express every other material as a **proportional ratio** of that baseline so the whole matrix stays grounded in reality.

  **The baseline is structural concrete C30/37**, chosen 2026-07-31 and now built. The numbers live in **`Core/Profiles/`** — `MaterialProfiles` (density, directional strengths, an unused bond factor) and `ConnectionProfiles` (mortar, lime mortar, dry stone, nail, screw, bolt, plus a test-only unbreakable fixture). **Every value carries a citation comment** naming the code or table it came from: EN 1992-1-1 for concrete, EN 1996-1-1 and BS EN 998-2 / BS EN 459-1 for the masonry joints, EN 1995-1-1 and EN 338 for the fasteners.

  Two properties are structural rather than incidental. Each profile row carries a **class** (bonded / frictional / mechanical fastener / test fixture), which is what lets the physical invariants be checked *per kind of joint* — "compression dominates" is true of a mortared joint and false of a bolt — and what makes an unbreakable fixture machine-recognisable so it cannot quietly ship. And **adding a profile is adding a row**: no new class, no new branch, with the well-formedness sweep picking it up automatically.
- **Testing behaviors, not materials:** don't write a test per material. Add a second material (e.g. **wood**) **once**, only to prove the directional code genuinely reads the material profile — run the same shear scenario on wood and confirm it survives where brick failed. If that passes, the system is proven data-driven and any future material is just numbers, no new code or tests. The same principle extends to connection types (join two pieces with a specific connection, apply force, confirm the connection fails at its own threshold before either material does).

---

## 5. Project Scaffolding

Goal: a rudimentary skeleton you can **press Play** on in the editor and watch demos.

**Minimum pieces:**
- A C++ **game module** (the project shell).
- A simple **level**: floor plane + lighting.
- A **player setup** — even just a flying spectator camera.
- One spawned **test object** (the brick wall).

That same empty-world-with-floor is also the home where unit and integration tests run.

**Structure — two layers:**
- **Folders** separate reusable **core systems** (base destructible actor, material/connection profiles, force logic) from throwaway **demo/test scenarios**, from **content** (meshes, geometry collections). Someone should be able to glance at the folders and know instantly what's a reusable system vs. a throwaway demo.
- **Code:** the scaffold knows nothing special about bricks — it just spawns a **scenario**. A **scenario base class** exists; a brick wall is one scenario that inherits from it. Pressing Play can load any scenario, and adding demos never touches the core scaffold.

**In-world main menu (not a separate screen):**
- The game **always** has a scenario loaded — never an empty void.
- At startup it loads a deliberately **tiny, lightweight default scenario** so the game opens fast.
- The **main menu overlays within the current world**; your current scenario stays loaded right behind it. No hard load transitions.
- The menu lets you **switch scenarios, restart** the current one, and shows **on-screen readouts** (strain numbers, "connection broke"). It doubles as the scenario switcher; every new demo is just another entry on the list. (It doesn't need to be polished at first.)

---

## 6. Open Threads / Next Steps

Named during planning but not yet designed — good places to resume:

1. **Force delivery systems** — explosions with radial falloff and kinetic impacts from large objects. *(Suggested next: everything so far is about receiving/distributing force; this is the piece that delivers it.)*
2. **Visual break patterns** — the *look* of breaking per material: wood splintering, concrete fracturing, glass shattering (distinct from *when* things collapse).
3. **Secondary debris collisions** — pieces carrying momentum into other pieces and knocking more loose.
4. **Performance at full-building scale** — a whole building of individually-massed pieces with live debris can get heavy fast.

### The spatial layer: threads 1 and 3 and the moment limitation are one dependency *(found 2026-08-04)*

Do not plan 1 and 3 as separate slices. Together with the eccentric-load limitation in §3 they are **three faces of one missing thing: the solver has no positions.**

`FStructure` knows pieces, masses, joints, normals and areas — and deliberately nothing about *where* anything is. That is a good decision and the reason the solver is world-free, fast and testable at twelve thousand fuzzed cases per run. But it means there is no lever arm to compute a moment from, no impact point for a collision to arrive at, and no way to ask which joints are near a blast. Each thread needs the same addition:

| Thread | What it needs from the spatial layer |
|---|---|
| Moments (§3 limitation) | Load position relative to the joint centroid; the joint's extent and axis (a section modulus), not just its area |
| Force delivery (1) | A point and direction in space to apply from, and the support relation generalised beyond "beneath" |
| Debris damage (3) | An impact point and impulse crossing from Chaos back into the graph |

**Design it once, for all three.** Building it for any one of them alone produces half a spatial layer that the other two then re-litigate or duplicate. The sequencing consequence is that this is a **design slice before it is a code slice**, and it is the one that changes how the game looks more than anything else outstanding — cantilevers that currently stand because shear is barely stressed are the visible symptom.

Note the knock-on recorded in CURRENT_STATE: today gravity through an axis-aligned bed joint never produces tension, and several tests and comments are scoped around that. Once moments exist, gravity produces tension routinely and that scoping becomes wrong rather than merely narrow.
