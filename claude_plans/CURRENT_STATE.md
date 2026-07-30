# Current State

**Living document.** This is the running TODO list for the project, not a historical record.

- **Finishing something? Remove it from this file** as part of that work. A completed item left here is worse than no list.
- **Deferring something? Add it here** before moving on, with enough context to act on it later.
- Check this file at the start of a work session and again before calling anything done.

Last updated: 2026-07-30

---

## Snapshot

Design is well developed ([DESIGN.md](DESIGN.md)); implementation has barely started. The repo is mid-way through stripping the Epic FPS template to make room for the destruction scaffold, and that strip is **incomplete and uncommitted**.

- Git: on `main`, one commit (`a1d61af` Initial commit), up to date with `origin/main`. Working tree has ~451 deletions, 10 modifications, 5 untracked — none committed.
- `Source/` is clean: Variant_Shooter and Variant_Horror C++ are gone. Remaining classes are the module, `GameMode`, `PlayerController`, `CameraManager`, and the new untracked `FlyingPawn`.
- `GameMode` correctly wires `DefaultPawnClass` to `ADestructionGameFlyingPawn` — the flying-spectator player setup DESIGN.md §5 asks for exists in code.
- `Content/` still holds 132 files of template leftovers (see TODO 1).
- No destruction code exists yet: no base destructible actor, no material profile, no connection profile, no force manager.
- No test infrastructure exists at all.

---

## Blocking — project is not in a runnable state

**1. The startup map doesn't exist.**
`Config/DefaultEngine.ini` sets both `EditorStartupMap` and `GameDefaultMap` to `/Game/Maps/Lvl_Sandbox`, but there is no `Content/Maps/` directory and no `Lvl_Sandbox.umap` anywhere on disk. The only map present is `Content/FirstPerson/Lvl_FirstPerson.umap`. Need to create the sandbox level (floor plane + lighting per DESIGN.md §5) or repoint the config.

**2. Template deletion pass is unfinished.**
Still on disk and still template:
- `Content/FirstPerson/` (7 files) — includes `BP_FirstPersonCharacter`, `BP_FirstPersonGameMode`, `BP_FirstPersonPlayerController`, `Lvl_FirstPerson.umap`. **Verify before deleting:** `BP_FirstPersonCharacter` is likely reparented from the now-deleted `DestructionGameCharacter` C++ class, which will throw load errors when the editor next opens. Confirm in-editor rather than assuming.
- `Content/__ExternalActors__/` (63 files) and `Content/__ExternalObjects__/` (1 file) — these belonged to the deleted `Lvl_Horror` and `Lvl_Shooter` levels and are almost certainly orphaned. Confirm they aren't referenced by the FirstPerson map before removing.
- `Content/Characters/` (36 files) — mannequin leftovers. Decide whether any is worth keeping as a scale reference; DESIGN.md uses real-world metric scale, so a human-scale reference has some value.
- `Content/Input/` (9 files) — check what the FlyingPawn actually consumes before deleting.
- `Content/Collections/` and `Content/Developers/` — empty directories, safe to remove.

**3. Nothing is committed.**
The entire strip plus the new FlyingPawn, `CLAUDE.md`, `claude_plans/`, and `.claude/skills/` are uncommitted. Once the deletion set is verified intentional, commit it — ideally as a couple of coherent commits (template strip / scaffold + process docs) rather than one large mixed one.

---

## Test infrastructure — needed before any feature work

TDD is mandatory (see [CLAUDE.md](../CLAUDE.md)), but there is currently nothing to run, which means the gate can't actually be satisfied yet. This is the real first task.

**4. Create the test module.** No `Source/DestructionGame/Tests/` exists. Needs to exist, guarded by `#if WITH_DEV_AUTOMATION_TESTS`.

**5. Confirm the automation test macro shape against the installed engine.** The `EAutomationTestFlags` enum spelling changed across UE 5.x — copy a current in-tree example from the UE 5.8 source rather than writing it from memory.

**6. Establish and document the headless test-run command.** Needs to be verified against this engine install, then recorded in CLAUDE.md so it's a one-liner every session.

---

## Core systems — designed, not built

Nothing below exists in code. DESIGN.md §2–3 specifies all of it.

**7. Base destructible actor** wrapping a Chaos Geometry Collection, with damage threshold and connection strength, exposing apply-damage-at-location.

**8. Material profile data asset** — directional strengths (compression / shear / tension), fracture pattern, density. Data, not code.

**9. Connection as a first-class object** with its own directional profile; connection types (mortar, nail, screw, bolt) as data profiles.

**10. Directional force classification** — classify incoming force relative to each connection's *interface plane*, not world axes. DESIGN.md §2 flags this as the thing Chaos does not do out of the box (single strain threshold, force-type agnostic). Keep this math in world-free plain functions so it stays cheaply unit-testable.

**11. Damage / force manager** — routes hits, explosions, radial forces to the right actors.

**12. Piece-size floor + three modes.** Build modes 1 (indestructible) and 3 (disappear) first — they prove threshold detection simply. Mode 2 (Niagara dust) after.

**13. Piece creation timestamps** — stamped when a piece breaks free, inspectable live, used to recover failure sequence.

**14. Scenario base class + tiny default scenario.** DESIGN.md §5: the scaffold spawns a *scenario* and knows nothing about bricks. Brick wall is one scenario. Also needs the in-world overlay menu (scenario switcher + strain readouts) — the game should never be an empty void.

---

## Open design threads (DESIGN.md §6 — not yet designed)

**15. Force delivery systems** — explosions with radial falloff, kinetic impacts from large objects. DESIGN.md suggests this as the natural next design topic: everything so far is about *receiving* force, nothing *delivers* it.

**16. Visual break patterns** per material — wood splintering, concrete fracturing, glass shattering. Distinct from *when* things collapse.

**17. Secondary debris collisions** — debris carrying momentum into other pieces and knocking more loose.

**18. Performance at full-building scale** — individually-massed pieces plus live debris gets heavy fast. Flagged as a known risk, no plan yet.

**19. Pull real material strength numbers (MPa).** Pick one well-characterized baseline (concrete or steel), calibrate it to feel right, express every other material as a ratio of it. Needed before the material × force matrix has real expected values.

---

## Housekeeping

**20. `README.md` is a stub** — one line, just the title.

**21. `.claude/settings.local.json` has accumulated a dozen auto-generated single-use PowerShell permission entries** from earlier exploration. They're noise and won't match future commands. Worth pruning to a few useful patterns.

**22. `DerivedDataCache/`, `Intermediate/`, `Binaries/`, `Saved/`, `.vs/` are on disk** — confirm `.gitignore` covers all of them (it was modified but not reviewed).
