# Current State

**Living document.** This is the running TODO list for the project, not a historical record.

- **Finishing something? Remove it from this file** as part of that work. A completed item left here is worse than no list.
- **Deferring something? Add it here** before moving on, with enough context to act on it later.
- Check this file at the start of a work session and again before calling anything done.

Items are deliberately unnumbered — they get added and removed constantly, and renumbering churns the diff.

Last updated: 2026-07-30

---

## Snapshot

Design is well developed ([DESIGN.md](DESIGN.md)); implementation has just started. The FPS template is fully stripped and **the project runs**: `Lvl_Sandbox` loads, the flying pawn is the default pawn, and the automation suite is green.

- Git: on `main`. Working tree clean.
- **`Content/Maps/Lvl_Sandbox`** — floor, directional light, sky light, sky atmosphere, height fog, PlayerStart. Not World Partition, deliberately: WP writes one file per actor and the sandbox spawns its scenarios from code. Floor is currently the engine template mesh; swapping to `LevelPrototyping/SM_Plane` would give a grid material with visible scale reference.
- `Source/` is clean of template variants. Classes: the module, `GameMode`, `PlayerController`, `CameraManager`, `FlyingPawn`, and `Core/ConnectionLoad` + `Core/ConnectionStrength`.
- **Test infrastructure works.** Automation tests run headless, verified end to end on a real red → green cycle. Build and run commands are documented in [CLAUDE.md](../CLAUDE.md).
- **Two core systems are in**, both test-driven and green (7 tests, 0 errors):
  - `Core/ConnectionLoad` — resolves a force into compression / tension / shear relative to a connection's interface plane, with the normal-orientation convention pinned down by the Newton's-third-law invariant.
  - `Core/ConnectionStrength` — compares a load against directional strengths in MPa and returns a utilisation ratio (>1 = the joint gives). Shear capacity follows Mohr-Coulomb, growing with compression, so a wall sheds shear resistance as the load above it is removed. Carries `ForceUnitsPerMPaSqCm`, the single SI↔Unreal conversion boundary.
- **`Content/` is clean.** Template strip finished: 22 files remain, all of them in use — the 6 Enhanced Input assets the pawn and controller hard-reference by path, and the `LevelPrototyping` primitives and grid materials (`SM_Plane`, `SM_Cube`, `SM_Ramp`, …) that the sandbox level and brick scenarios will be built from.
- Everything else in Core systems below is still unbuilt.

---

## Conventions and gotchas

**Don't hard-reference content from C++ without checking it exists.** `FlyingPawn` and `PlayerController` both resolve Input assets by path in their constructors via `ConstructorHelpers::FObjectFinder`. That pattern makes content deletion silently dangerous — it fails at construction, not at compile time. Worth a helper or a startup assert if this spreads.

**Automation runs exit 0 even when tests fail**, and results never reach stdout. Always read `Saved/Logs/DestructionGame.log`. See [CLAUDE.md](../CLAUDE.md).

**1 newton = 100 Unreal force units.** World scale is 1 uu = 1 cm, so force and impulse need a ×100 conversion while mass (kg) and density (g/cm³) need none. Strengths are stored in SI and converted at one named boundary. Full table in [DESIGN.md §3](DESIGN.md).

**The NaN guards and the shear cap are coupled — don't remove one.** `FMath::Min` is `(A <= B) ? A : B`, so `Min(NaN, cap)` returns the **cap**: a NaN shear capacity silently becomes a plausible number. The result still fails closed only because the compression axis is separately guarded by `IsFinite` and returns `Max()`, which then dominates. Drop that guard and the cap will launder NaN into a believable value. Untested edge: a cap set *below* cohesion silently gives a joint less strength than its stated cohesion.

**Degenerate inputs fail closed, and `FMath::Max` is why it matters.** `Max` is `(A >= B) ? A : B`, so every comparison against NaN is false and `Max3` *silently discards* a NaN — returning whichever other axis was lowest. A NaN load therefore produced a confident utilisation of **0.0**, meaning "unstressed, perfectly fine", which nothing downstream could detect. `ComputeUtilisation` now returns `TNumericLimits<double>::Max()` for a non-positive interface area or any non-finite stress. Guards are written `!(x > 0.0)` rather than `x <= 0.0` so a NaN is caught by the same branch instead of slipping past it. Locked down by `ConnectionStrength.DegenerateInputs`.

---

## Core systems — designed, not built

DESIGN.md §2–3 specifies all of it.

**`FConnectionStrength` is a plain C++ struct, not a data asset.** DESIGN.md §2 calls for a data asset and for materials to be "data, not code", but today adding a material means editing C++ and recompiling — no `USTRUCT`, no `UPROPERTY`, nothing tunable in the editor. Correctly deferred rather than speculatively built, but this is the gap between the current shape and the stated design.

**Shear direction is discarded.** `FConnectionLoad::Shear` is a magnitude only. Fine for isotropic materials, but DESIGN.md notes wood fails by splintering *along the grain* — grain-relative shear direction will matter once anisotropic materials arrive. Revisit then; don't add it speculatively.

**Base destructible actor** wrapping a Chaos Geometry Collection, with damage threshold and connection strength, exposing apply-damage-at-location. Needs a live world and a ticking solver, so expect integration-shaped tests rather than unit tests.

**Material profile data asset** — directional strengths (compression / shear / tension), fracture pattern, density. Data, not code.

**Connection as a first-class object** with its own directional profile; connection types (mortar, nail, screw, bolt) as data profiles. The strength side now exists — `Core/ConnectionStrength` compares a classified load against directional strengths and returns a utilisation ratio. What is still missing is the connection *object*: something that owns an interface normal, an interface area and a strength profile, knows which two pieces it joins, and can be asked whether it has given.

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

**`README.md` is a stub** — one line, just the title.

**`.claude/settings.local.json` has accumulated a dozen auto-generated single-use PowerShell permission entries** from earlier exploration. They're noise and won't match future commands. Worth pruning to a few useful patterns. (Not in git — covered by a global gitignore.)

**The three local skills aren't invocable as `/test-expert` etc. yet.** They were created mid-session; Claude Code loads skills at startup, so they need a restart to register. Their instructions are being followed manually in the meantime.
