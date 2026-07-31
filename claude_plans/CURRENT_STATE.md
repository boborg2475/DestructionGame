# Current State

**Living document.** This is the running TODO list for the project, not a historical record.

- **Finishing something? Remove it from this file** as part of that work. A completed item left here is worse than no list.
- **Deferring something? Add it here** before moving on, with enough context to act on it later.
- Check this file at the start of a work session and again before calling anything done.

Items are deliberately unnumbered — they get added and removed constantly, and renumbering churns the diff.

Last updated: 2026-07-31 (tilted-joint classification characterised)

---

## Snapshot

Design is well developed ([DESIGN.md](DESIGN.md)); implementation has just started. The FPS template is fully stripped and **the project runs**: `Lvl_Sandbox` loads, the flying pawn is the default pawn, and the automation suite is green.

- Git: on `main`.
- **`Content/Maps/Lvl_Sandbox`** — floor, directional light, sky light, sky atmosphere, height fog, PlayerStart. Not World Partition, deliberately: WP writes one file per actor and the sandbox spawns its scenarios from code. Floor is currently the engine template mesh; swapping to `LevelPrototyping/SM_Plane` would give a grid material with visible scale reference.
- `Source/` is clean of template variants. Classes: the module, `GameMode`, `PlayerController`, `CameraManager`, `FlyingPawn`, and `Core/ConnectionLoad` + `Core/ConnectionStrength` + `Core/Connection` + `Core/Structure`.
- **Test infrastructure works.** Automation tests run headless, verified end to end on a real red → green cycle. Build and run commands are documented in [CLAUDE.md](../CLAUDE.md).
- **Four core systems are in**, all test-driven. 19 tests, all green.
  - `Core/ConnectionLoad` — resolves a force into compression / tension / shear relative to a connection's interface plane, with the normal-orientation convention pinned down by the Newton's-third-law invariant.
  - `Core/ConnectionStrength` — compares a load against directional strengths in MPa and returns a utilisation ratio (>1 = the joint gives). Shear capacity follows Mohr-Coulomb, growing with compression, so a wall sheds shear resistance as the load above it is removed. Carries `ForceUnitsPerMPaSqCm`, the single SI↔Unreal conversion boundary.
  - `Core/Connection` — `FConnection`, the joint itself: two piece handles, an interface normal, an area and a strength profile, so a caller supplies only a force. Composes the two above and returns the utilisation. Giving **latches** — the breaking call reports the ratio that broke it (>1), every call after it returns 0, because a joint that has given is out of the structure and carries nothing. Phase 2's redistribution depends on that zero.
  - `Core/Structure` — `FStructure`, the graph that owns pieces and connections (**phase 2a only: load computation and grounding**). Support is **two-tiered** per DESIGN.md §3: a connection's interface normal, turned to point at the piece being asked about, makes it a **bed joint** beneath (substantially vertical, `|Z| > cos 45°`, pointing up at the piece — it bears), a bed joint **above** (points down — bears nothing, that is something resting on this piece), or a **head joint** (anything else). A piece's supports are its bed joints beneath, and *only* a piece with none falls back to its head joints. Everything downstream is computed over that **support relation**, not over raw connectivity: reachability is a breadth-first walk from the grounded pieces over "who rests on me", and accumulation runs in a topological order of the same relation (Kahn — distance to the ground is *not* a valid order, since a spanning brick and the brick on it are equidistant and the second loads the first). Load splits weighted by interface area, **across only those supports that themselves reach the ground** — a share given to a falling piece never arrives, and leaves the joint actually carrying the load reporting a fraction of it. Pieces in an unroutable knot (a cycle: a course spanning a two-brick gap, each brick falling back to its neighbours' head joints) are **reported unsupported**, so `IsPieceSupported` and `GetConnectionForce` agree rather than contradicting each other silently; it is not a claim the cycle was solved. **Only the pieces caught in the knot**, identified by their own load coming back round to them (`LoadReturnsToPiece`) rather than by the Kahn pass failing to order them — the ordering runs top-down, so un-orderability is a strictly larger set that reaches down to the foundations. Pieces beneath a knot keep their support and carry everything except the unroutable contribution. Those two rules feed each other — stranding changes who reaches the ground, which changes who may take a share, so a piece resting only on the knot loses its own support — so `SolveLoads` iterates to a fixpoint, stranding at least one more piece per pass and therefore running at most `NumPieces + 1` passes. The force handed to a joint is vertical, and **signed by which end is being held up** — `-magnitude` when the loaded piece is `PieceB`, `+magnitude` when it is `PieceA`, per `ConnectionLoad.h`'s convention that the force belongs to `PieceB`; `FConnection` then resolves it as compression on a bed joint and shear on a head joint. Solving is **non-destructive**: it never calls `ApplyForce`, so it can be re-run and nothing latches. `AddPiece` / `AddConnection` validate at the door and reject rather than store.
- **`Content/` is clean.** Template strip finished: 22 files remain, all of them in use — the 6 Enhanced Input assets the pawn and controller hard-reference by path, and the `LevelPrototyping` primitives and grid materials (`SM_Plane`, `SM_Cube`, `SM_Ramp`, …) that the sandbox level and brick scenarios will be built from.
- **Breaking and cascade (phase 2b) is the next thing, and the largest.** The graph and the load maths exist; what is missing is the step that compares each computed load against its joint's strength, latches the ones that give, and re-solves.

---

## Conventions and gotchas

**Don't hard-reference content from C++ without checking it exists.** `FlyingPawn` and `PlayerController` both resolve Input assets by path in their constructors via `ConstructorHelpers::FObjectFinder`. That pattern makes content deletion silently dangerous — it fails at construction, not at compile time. Worth a helper or a startup assert if this spreads.

**Automation runs exit 0 even when tests fail**, and results never reach stdout. Always read `Saved/Logs/DestructionGame.log`. See [CLAUDE.md](../CLAUDE.md).

**1 newton = 100 Unreal force units.** World scale is 1 uu = 1 cm, so force and impulse need a ×100 conversion while mass (kg) and density (g/cm³) need none. Strengths are stored in SI and converted at one named boundary. Full table in [DESIGN.md §3](DESIGN.md).

**The NaN guards and the shear cap are coupled — don't remove one.** `FMath::Min` is `(A <= B) ? A : B`, so `Min(NaN, cap)` returns the **cap**: a NaN shear capacity silently becomes a plausible number. The result still fails closed only because the compression axis is separately guarded by `IsFinite` and returns `Max()`, which then dominates. Drop that guard and the cap will launder NaN into a believable value. Untested edge: a cap set *below* cohesion silently gives a joint less strength than its stated cohesion.

  **Dependents of that never-NaN guarantee**, so a refactor can find them: the shear cap in `ComputeUtilisation` itself, and the break comparison in `FConnection::ApplyForce` — the one line that decides whether a joint gives. The latter is written `!(x <= 1.0)` so it is locally correct even if the guarantee were ever broken; don't "simplify" it back to `x > 1.0`.

**`ComputeUtilisation`'s "always finite for any input" is slightly overstated.** A subnormal-but-positive capacity (e.g. `5e-324`) passes the `> 0.0` guard and yields `+Inf`. Unreachable from any real profile, and the break decision stays correct since `Inf > 1.0` — but a caller *summing* utilisations for a strain readout would get infinity. Tighten the wording or the guard when the readouts land.

**Degenerate inputs fail closed, and `FMath::Max` is why it matters.** `Max` is `(A >= B) ? A : B`, so every comparison against NaN is false and `Max3` *silently discards* a NaN — returning whichever other axis was lowest. A NaN load therefore produced a confident utilisation of **0.0**, meaning "unstressed, perfectly fine", which nothing downstream could detect. `ComputeUtilisation` now returns `TNumericLimits<double>::Max()` for a non-positive interface area or any non-finite stress. Guards are written `!(x > 0.0)` rather than `x <= 0.0` so a NaN is caught by the same branch instead of slipping past it. Locked down by `ConnectionStrength.DegenerateInputs`.

**A degenerate interface normal only fails closed inside `FConnection`.** `ClassifyForce` answers a zero-length or NaN normal with a *zero load*, which is right in isolation but reads downstream as "unloaded, perfectly healthy" — the fail-open hole appears only once the two halves are composed. `FConnection::ApplyForce` closes it by substituting a zero interface area, routing the case through `ComputeUtilisation`'s existing area guard rather than adding a second one. **Anything else that calls `ClassifyForce` and `ComputeUtilisation` directly re-opens the hole** and must make the same check (`FVector::Normalize()` returns false for both the zero-length and the NaN case, so one test covers both). Locked down by the bad-normal rows of `Connection.DegenerateInputs`.

---

## MVP: a wall you can watch break

The agreed next milestone. **Two bricks and one joint first** — visible, breakable, exercising the whole pipeline end to end — then scale to a wall, so later problems are wall logic rather than plumbing.

Architecture settled by spike (DESIGN.md §3): pieces are **kinematic while intact**, we compute connection loads ourselves, and a piece switches to dynamic when the connection holding it gives. **No Geometry Collections** — those are for pieces fracturing into smaller pieces, which the MVP does not need. That drops the heaviest dependency.

Ordered so the world-free work, which is most of it, comes first:

1. **Breaking and cascade — phase 2b.** Phase 2a (the graph, grounding, and load accumulation) is done and green; `Core/Structure` computes what every joint carries but never decides whether that is too much. 2b is the deciding step: evaluate each computed load against its joint's strength, latch the ones that give, re-solve so the load moves onto the neighbours, and repeat until it settles or the structure has come apart. **The genuinely new design** — DESIGN.md says load "redistributes" but never says how. *No world; existing harness.*

   Two constraints still outstanding from the phase 1 review (the third, unvalidated piece handles, is closed — `FStructure::AddConnection` rejects them):

   - **`FConnection` is copyable and the latch is per-copy.** `for (FConnection C : Connections)` — a missing `&` — compiles clean, evaluates every joint, latches every overloaded one *on the temporary*, and leaves the real connections untouched. The wall then reports zero broken joints under any load and never falls. Nothing in the type prevents it and no current test would catch it. 2a sidesteps it by never latching at all; **2b is where it becomes live.** Hold connections by reference or index, and consider whether the type should resist copying.
   - **There is no non-mutating way to evaluate a joint.** `ApplyForce` is the only evaluator and it latches. An iterative solver that wants to trial a load distribution, find it inconsistent and re-solve will permanently destroy joints on the first trial. If 2b needs iteration, it needs a `const` "what would this utilisation be" alongside the committing call.

   Deliberately left out of 2a, for 2b to pick up:

   - **A given connection still conducts support.** `FStructure::SolveLoads` builds each piece's support list from every connection touching it; nothing consults `HasGiven()`. That is only correct while nothing can break. The moment 2b latches a joint, a broken connection must stop being a support — dropping out of the tier decision too, so a piece whose only bed joint has gone falls back to its head joints — and its share must move to the neighbours, which is what redistribution *is*. No test covers it yet because reaching it requires breaking something.
   - **A cycle in the support relation is reported, not solved.** Pieces caught in an unroutable knot — and only those; stranding is local, pinned by `Structure.StrandingIsLocal` and `Structure.StrandingPropagatesUpward` — are marked **unsupported**, so the stall is visible from outside and conservation holds. `Structure.SupportCycle` pins this, with a two-brick and a three-brick gap, plus two controls (a single grounded neighbour, and a cycle through a grounded piece that still resolves). What is still missing is any *rule for how load divides round a loop*: a course spanning a wide gap is reported as falling rather than as arching, which is physically the conservative answer but not the interesting one. Whoever wants real arching behaviour has to design that rule — see DESIGN.md §4's shear test, which is the scenario that will want it.
   - **The fixpoint loop is load-bearing, and exactly one test says so.** `SolveLoads` iterates because stranding changes who reaches the ground, so a piece resting *only* on the knot must itself come out unsupported — and the pass that stranded the knot has already written that piece's weight onto its joint, because it is a leaf and Kahn seeds with it first. Only the re-solve wipes that. **`Structure.StrandingPropagatesUpward`** is what discriminates the loop from a single pass; delete the loop and its first case reports the brick supported with 2665.6 uu on a joint whose far end is falling. Don't "simplify" the loop away without checking that test still fails when you do.
   - **The bed/head tier bakes in gravity as the only load direction.** "Substantially vertical" is `|normal.Z| > cos 45°`, which is meaningless once explosions and kinetic impacts arrive and the load no longer points down — routing has to generalise to the direction of the applied load. Flagged in DESIGN.md §3 as a known limitation of the model, not just of this code. The 45° line itself is pinned from both sides at 40° and 50° by `Structure.SupportTierThreshold`; the exact tie is deliberately unasserted, which is what makes `BedJointCosine` safe to spell as a `constexpr` literal — only a normal at *exactly* 45.000° could tell it from `1.0 / FMath::Sqrt(2.0)`. **The test file spells the same literal**, because `SpecSupportsOf` is the oracle for the `DegenerateInputs` matrix and has to agree with `RoleOf` bit for bit; change one and change the other.
   - **A tilted joint ABOVE a piece supports it in tension, and that is accepted.** `RoleOf` tests `|NormalZTowardPiece| > cos45` before it looks at the sign, so the head tier is sign-blind: an inclined face over a piece is still that piece's fallback support, and holding it up means pulling the face open. Characterised by `Structure.TiltedJointClassification` — at 50° a 2.72 kg brick gives compression 0 / shear 2041.97 / **tension 1713.41**. DESIGN.md §3 states this outcome and accepts it, so **do not "fix" it**; it is why the 45° line looks like a cliff and is not one (just below it the same face is `BedAbove`, bears nothing, and the piece falls instead). Two comments in `StructureTest.cpp` used to read as properties of the *model* — "tension is always zero", "a gravity load path never pulls a joint open" — and are properties of the *fixtures*, every one of which is axis-aligned; both are now scoped, and adding a tilted normal to the `DegenerateInputs` matrix will turn its tension row red as a discovery rather than a regression. Note the design's "fails almost at once" is about the *ratio* of mortar's tensile to compressive limit, not about this load: one brick over 100 cm² reaches only 0.017 utilisation and breaks nothing.
   - **`SolveLoads`'s pass bound is provably 2, not `NumPieces + 1`, and nothing asserts it.** Stranding only removes nodes and edges, and removal cannot create a cycle, so a second pass can never strand anything the first did not — the third exists only to observe that and stop. Measured max across 8,500 fuzzed graphs up to 9 pieces: 2. Left unasserted because `SolveLoads` returns `void` and there is no seam to observe a pass count through; adding one is a production change and needs its own red test. Worth doing if the per-solve cost ever matters, since the bound is what multiplies the O(pieces × connections) figure below.
   - **An unsupported island reports zero load on its internal joints.** Defensible for a static solver — nothing is holding it up, so there is no static load path — but it means "carries nothing" is ambiguous between *given*, *grounded on both sides* and *falling*. If a strain readout ever needs to tell those apart, it needs more than the force.
   - **The solver is O(pieces x connections), and the knot check adds a walk per piece.** Building the support lists scans the whole connection array once per piece; replacing it with an adjacency index built once per solve is the obvious fix (it is outside the fixpoint loop, so it stays a single scan). Inside the loop, each pass redoes reachability, the load-path filter, the accumulation, and then `LoadReturnsToPiece` **once per supported ungrounded piece** — each a fresh walk with its own `TArray<bool>` visited buffer, so step five alone is O(pieces x (pieces + edges)). One Tarjan SCC pass would give the same answer in linear time, and a single reusable visited buffer would kill the allocations; both were left undone because nothing measures slow yet. A structure that strands nothing still pays for the walks. Fine at MVP size and deliberately not optimised; flag rather than fix until something measures slow.
   - **`AddConnection`'s normal guard has a top-end hole.** A normal with components near 1e200 gives an infinite `SizeSquared`, `InvSqrt(inf)` is 0, so `FVector::Normalize()` returns **true** having produced the zero vector — and `RoleOf` then classifies that as a head joint, which is a support tier. Unreachable from real content, and left alone deliberately: closing it is branching logic, so it needs a red `GraphValidation` row (a huge-component normal, expected rejected) before the `IsFinite` check goes in.
   - **`AddConnection` stores the interface normal exactly as given, non-unit included.** It only checks the normal *can* be normalised; `FConnection::ApplyForce` normalises its own copy each time it is called. Correct, and asserted by the "a non-unit interface normal" acceptance row, but it means the stored normal is not canonical — anything that compares normals directly must normalise first.
2. **World-based test harness** — a world that ticks. Prefer `AFunctionalTest` so tests are watchable in-editor and still run headless. *Spike the headless path first.*
3. **Brick actor** — true dimensions, mass from density, kinematic → dynamic on release.
4. **Scenario system** — base class plus `BrickWallScenario` spawning courses in running bond (DESIGN.md §5), loaded by the GameMode so pressing Play shows something.
5. **Visualisation** — connections drawn coloured by utilisation, on-screen piece and broken-joint counts, max strain. Small, but it is what makes this watchable rather than merely passing.
6. **Integration tests** — collapse (assert the outcome: it fell) and redistribution (strain rises on neighbours, nothing moves).

---

## Core systems — designed, not built

DESIGN.md §2–3 specifies all of it.

**`FConnectionStrength` is a plain C++ struct, not a data asset.** DESIGN.md §2 calls for a data asset and for materials to be "data, not code", but today adding a material means editing C++ and recompiling — no `USTRUCT`, no `UPROPERTY`, nothing tunable in the editor. Correctly deferred rather than speculatively built, but this is the gap between the current shape and the stated design.

**Shear direction is discarded.** `FConnectionLoad::Shear` is a magnitude only. Fine for isotropic materials, but DESIGN.md notes wood fails by splintering *along the grain* — grain-relative shear direction will matter once anisotropic materials arrive. Revisit then; don't add it speculatively.

**Base destructible actor** wrapping a Chaos Geometry Collection, with damage threshold and connection strength, exposing apply-damage-at-location. Needs a live world and a ticking solver, so expect integration-shaped tests rather than unit tests.

**Material profile data asset** — directional strengths (compression / shear / tension), fracture pattern, density. Data, not code.

**Connection types as data profiles** — mortar, nail, screw, bolt, chosen by the in-game builder. The connection object itself now exists (`Core/Connection`) and takes an `FConnectionStrength` whole, so this is purely the data-asset gap noted above: the profiles are C++ literals in tests rather than anything authorable.

**Damage / force manager** — routes hits, explosions, radial forces to the right actors.

**Piece-size floor + three modes.** Build modes 1 (indestructible) and 3 (disappear) first — they prove threshold detection simply. Mode 2 (Niagara dust) after.

**Piece creation timestamps** — stamped when a piece breaks free, inspectable live, used to recover failure sequence.

**Scenario base class + tiny default scenario.** DESIGN.md §5: the scaffold spawns a *scenario* and knows nothing about bricks. Brick wall is one scenario. Also needs the in-world overlay menu (scenario switcher + strain readouts) — the game should never be an empty void.

---

## Open design threads (DESIGN.md §6 — not yet designed)

**Force delivery systems** — explosions with radial falloff, kinetic impacts from large objects. DESIGN.md suggests this as the natural next design topic: everything so far is about *receiving* force, nothing *delivers* it.

**Visual break patterns** per material — wood splintering, concrete fracturing, glass shattering. Distinct from *when* things collapse.

**Secondary debris collisions** — debris carrying momentum into other pieces and knocking more loose.

**Performance at full-building scale** — individually-massed pieces plus live debris gets heavy fast. Flagged as a known risk, no plan yet.

**Pull real material strength numbers (MPa).** Pick one well-characterized baseline (concrete or steel), calibrate it to feel right, express every other material as a ratio of it. Needed before the material × force matrix has real expected values.

---

## Housekeeping

**Two rename redirectors sit in `Content/Maps/`** — `NewMap.umap` and `Lvl1_Sandbox.umap`, left over from renaming the level into place. Nothing references either. Clear them the Unreal way: Content Browser → right-click the `Maps` folder → **Fix Up Redirectors in Folder**. Deliberately left untracked rather than committed.

**`.claude/settings.local.json` has accumulated a dozen auto-generated single-use PowerShell permission entries** from earlier exploration. They're noise and won't match future commands. Worth pruning to a few useful patterns. (Not in git — covered by a global gitignore.)

**The three local skills aren't invocable as `/test-expert` etc. yet.** They were created mid-session; Claude Code loads skills at startup, so they need a restart to register. Their instructions are being followed manually in the meantime.
