# Current State

**Living document.** This is the running TODO list for the project, not a historical record.

- **Finishing something? Remove it from this file** as part of that work. A completed item left here is worse than no list.
- **Deferring something? Add it here** before moving on, with enough context to act on it later.
- Check this file at the start of a work session and again before calling anything done.

Items are deliberately unnumbered — they get added and removed constantly, and renumbering churns the diff.

Last updated: 2026-07-31

---

## Snapshot

Design is well developed ([DESIGN.md](DESIGN.md)); implementation has just started. The FPS template is fully stripped and **the project runs**: `Lvl_Sandbox` loads, the flying pawn is the default pawn, and the automation suite is green.

- Git: on `main`.
- **`Content/Maps/Lvl_Sandbox`** — floor, directional light, sky light, sky atmosphere, height fog, PlayerStart. Not World Partition, deliberately: WP writes one file per actor and the sandbox spawns its scenarios from code. Floor is currently the engine template mesh; swapping to `LevelPrototyping/SM_Plane` would give a grid material with visible scale reference.
- `Source/` is clean of template variants. Classes: the module, `GameMode`, `PlayerController`, `CameraManager`, `FlyingPawn`, and `Core/ConnectionLoad` + `Core/ConnectionStrength` + `Core/Connection`.
- **Test infrastructure works.** Automation tests run headless, verified end to end on a real red → green cycle. Build and run commands are documented in [CLAUDE.md](../CLAUDE.md).
- **Three core systems are in**, all test-driven and green (10 tests, 0 errors):
  - `Core/ConnectionLoad` — resolves a force into compression / tension / shear relative to a connection's interface plane, with the normal-orientation convention pinned down by the Newton's-third-law invariant.
  - `Core/ConnectionStrength` — compares a load against directional strengths in MPa and returns a utilisation ratio (>1 = the joint gives). Shear capacity follows Mohr-Coulomb, growing with compression, so a wall sheds shear resistance as the load above it is removed. Carries `ForceUnitsPerMPaSqCm`, the single SI↔Unreal conversion boundary.
  - `Core/Connection` — `FConnection`, the joint itself: two piece handles, an interface normal, an area and a strength profile, so a caller supplies only a force. Composes the two above and returns the utilisation. Giving **latches** — the breaking call reports the ratio that broke it (>1), every call after it returns 0, because a joint that has given is out of the structure and carries nothing. Phase 2's redistribution depends on that zero.
- **`Content/` is clean.** Template strip finished: 22 files remain, all of them in use — the 6 Enhanced Input assets the pawn and controller hard-reference by path, and the `LevelPrototyping` primitives and grid materials (`SM_Plane`, `SM_Cube`, `SM_Ramp`, …) that the sandbox level and brick scenarios will be built from.
- The load solver that owns pieces and connections is the next thing, and the largest.

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

1. **Structure and load solver** — owns pieces and connections, accumulates weight down the structure, redistributes when a connection breaks, cascades. The connection half now exists (`Core/Connection`); what is missing is the graph that owns them and works out what each one carries. **The real work, and genuinely new design** — DESIGN.md says load "redistributes" but never says how. *No world; existing harness.*

   Three constraints this phase inherits, all found during phase 1 review:

   - **`FConnection` is copyable and the latch is per-copy.** `for (FConnection C : Connections)` — a missing `&` — compiles clean, evaluates every joint, latches every overloaded one *on the temporary*, and leaves the real connections untouched. The wall then reports zero broken joints under any load and never falls. Nothing in the type prevents it and no current test would catch it. Hold connections by reference or index, and consider whether the type should resist copying.
   - **There is no non-mutating way to evaluate a joint.** `ApplyForce` is the only evaluator and it latches. An iterative solver that wants to trial a load distribution, find it inconsistent and re-solve will permanently destroy joints on the first trial. If phase 2 needs iteration, it needs a `const` "what would this utilisation be" alongside the committing call.
   - **Piece handles are unvalidated.** A connection with `PieceA == PieceB`, or both left at `INDEX_NONE`, currently reads as a perfectly healthy joint under any load — while a zero interface area correctly reads as failed. The graph owner is the right place to reject those.
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
