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

1. **Base destructible actor** — a C++ actor that wraps a Chaos **Geometry Collection** component and holds the shared logic for taking damage, applying impulses, and deciding when something breaks. Every wall, floor, and column inherits from it. Key properties: a **damage threshold** (how much force before it breaks) and a **connection strength** (how stubbornly pieces cling together — this is what makes a structure topple believably instead of crumbling in place). It exposes a function like *apply damage at a location* that takes a hit point and a force and lets Chaos decide which chunks come loose.

2. **Material profile system** — each destructible carries a **data asset** describing its material (wood, concrete, stone, glass, etc.). This drives fracture pattern, damage thresholds, and break behavior. Materials are **data, not code**.

3. **Damage / force manager** — handles incoming hits, explosions, and radial forces, then tells the right actors how hard they were hit.

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

---

## 3. Physics Model

### Structural integrity under gravity
- Each piece has real **mass** based on its size and material density (a brick weighs like a brick).
- Pieces are held by connections; each connection bears a limited load.
- Remove a piece and its load **redistributes** to its neighbors. If they can carry it, the structure stands. Keep removing pieces and eventually a connection overloads, snaps, and the failure **cascades** — collapse under the structure's own weight (Chaos strain-based damage).

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

> **Chaos strain is not a physical quantity.** The strain thresholds on a geometry collection's connections are solver-tuning numbers, not newtons. There will need to be an explicit calibration mapping from "this joint carries X newtons" to "this Chaos connection has strain threshold Y". That seam is where physical realism quietly leaks if nobody is watching it.

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
- **Grounded in real-world data:** expected values come from published material strengths (compressive / shear / tensile strength, in megapascals). Pick **one reference material** as the baseline (concrete or steel are well-characterized), calibrate it to feel right in-game, then express every other material as a **proportional ratio** of that baseline so the whole matrix stays grounded in reality. (Pulling actual numbers is an implementation-time task.)
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
