# Current State

**Living document.** This is the running TODO list for the project, not a historical record.

- **Finishing something? Remove it from this file** as part of that work. A completed item left here is worse than no list.
- **Deferring something? Add it here** before moving on, with enough context to act on it later.
- Check this file at the start of a work session and again before calling anything done.

Items are deliberately unnumbered — they get added and removed constantly, and renumbering churns the diff.

Last updated: 2026-07-30

---

## Snapshot

Design is well developed ([DESIGN.md](DESIGN.md)); implementation has just started. The repo is mid-way through stripping the Epic FPS template to make room for the destruction scaffold — that strip is committed but **still incomplete**.

- Git: on `main`, in sync with `origin/main`. Working tree clean.
- `Source/` is clean of template variants. Classes: the module, `GameMode`, `PlayerController`, `CameraManager`, `FlyingPawn`, and `Core/ConnectionLoad`.
- **Test infrastructure works.** Automation tests run headless, verified end to end on a real red → green cycle. Build and run commands are documented in [CLAUDE.md](../CLAUDE.md).
- **First core system is in:** directional force classification (`Core/ConnectionLoad`) resolves a force into compression / tension / shear relative to a connection's interface plane. Test-driven, 8 cases, green.
- **`Content/` is clean.** Template strip finished: 22 files remain, all of them in use — the 6 Enhanced Input assets the pawn and controller hard-reference by path, and the `LevelPrototyping` primitives and grid materials (`SM_Plane`, `SM_Cube`, `SM_Ramp`, …) that the sandbox level and brick scenarios will be built from.
- Everything else in Core systems below is still unbuilt.

---

## Blocking — the project has no level

**The startup map doesn't exist, and there are now zero maps in the project.**
`Config/DefaultEngine.ini` sets both `EditorStartupMap` and `GameDefaultMap` to `/Game/Maps/Lvl_Sandbox`, but there is no `Content/Maps/` directory and no `.umap` anywhere. Create the sandbox level — floor plane + lighting per DESIGN.md §5. `Content/LevelPrototyping/Meshes/SM_Plane` and the prototype grid materials are there for exactly this.

Requires the editor; a `.umap` can't be hand-written.

Note: this does *not* block headless automation tests. The editor logs `Warning: Can't find file '.../Lvl_Sandbox.umap'`, carries on, and tests run and pass normally. Verified.

**Do not hard-reference content from C++ without checking it exists.** `FlyingPawn` and `PlayerController` both resolve Input assets by path in their constructors via `ConstructorHelpers::FObjectFinder`. That pattern makes content deletion silently dangerous — it fails at construction, not at compile time. Worth a helper or a startup assert if this spreads.

---

## Core systems — designed, not built

DESIGN.md §2–3 specifies all of it.

**Connection normal orientation is untested and undocumented.** *(Follow-up from the ConnectionLoad review.)*
`ClassifyForce` depends on the caller passing an interface normal that points away from the face receiving the force. Flip the normal and compression/tension swap. Nothing tests or documents this, and each of the two pieces in a joint sees the interface normal pointing the opposite way — so the same force reads as compression for one and tension for the other. Harmless now (one caller, the test), but a real bug once connections resolve loads for both pieces. **Next test:** flipping the normal swaps compression and tension with magnitudes unchanged. Then state the convention in the header.

**Shear direction is discarded.** `FConnectionLoad::Shear` is a magnitude only. Fine for isotropic materials, but DESIGN.md notes wood fails by splintering *along the grain* — grain-relative shear direction will matter once anisotropic materials arrive. Revisit then; don't add it speculatively.

**Base destructible actor** wrapping a Chaos Geometry Collection, with damage threshold and connection strength, exposing apply-damage-at-location. Needs a live world and a ticking solver, so expect integration-shaped tests rather than unit tests.

**Material profile data asset** — directional strengths (compression / shear / tension), fracture pattern, density. Data, not code.

**Connection as a first-class object** with its own directional profile; connection types (mortar, nail, screw, bolt) as data profiles. This is the natural next build on top of `ConnectionLoad`: the classification exists, nothing yet compares it against a threshold.

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

**`README.md` is a stub** — one line, just the title.

**`.claude/settings.local.json` has accumulated a dozen auto-generated single-use PowerShell permission entries** from earlier exploration. They're noise and won't match future commands. Worth pruning to a few useful patterns. (Not in git — covered by a global gitignore.)

**The three local skills aren't invocable as `/test-expert` etc. yet.** They were created mid-session; Claude Code loads skills at startup, so they need a restart to register. Their instructions are being followed manually in the meantime.
