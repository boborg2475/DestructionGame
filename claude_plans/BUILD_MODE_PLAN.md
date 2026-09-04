# Build Mode — interactive building with brick and wood (feature plan)

**Status: IN PROGRESS (started 2026-09-03).** Landed so far: `BuildMode::JointForContact` (the automatic
joint inference, see the ruling at the bottom) and the snap-candidate solver's FIRST kind —
`BuildMode::SolveSnapCandidates` (`Core/BuildMode/SnapSolver.*`) offering the running-bond next-course
brick snap with auto-formed bed joints (a straddling brick correctly forms a bed to each neighbour below),
ranked by proximity ahead of a free-placement fallback. Next: the remaining snap kinds (same-course,
corner return, timber centered/edge-flush), then the placement API loop and the UI. Read
[CLAUDE.md](../CLAUDE.md), [DESIGN.md](DESIGN.md) and
[CURRENT_STATE.md](CURRENT_STATE.md) first — they own the model, the constants and the standing rulings.

## The feature (owner's words, distilled)
Let the player **build different buildings** out of **brick and timber**, bringing pieces together
through the existing **joints**. Place a piece anywhere, OR **snap** it to logical positions relative
to what's already there, so pieces connect the way they really would: bricks continue to **stack with
running-bond overlap and the normal waste** you'd expect; timber sits **centered on a brick** or
**flush to a brick edge**. Different configurations of brick and wood compose into different buildings
(walls, corners, openings, framing, roofs). The result is a live `FStructure`, so anything built is
immediately subject to the existing integrity/collapse system — build it, then knock it down.

## The agreed approach: BRAIN FIRST, UI SECOND
The hard, interesting, physics-connected part is the *logical-building brain* (snap-candidate solving +
automatic joint formation). The interactive editor UX (mouse/gamepad placement, ghost preview, snap
highlight, palette, save/load) is the biggest genuinely-new lump and the least test-covered. **Build
the brain first as TDD-tested code, drivable by a thin harness (a scripted place/place/place sequence
that renders), then layer the interactive UI on a proven core.** Do NOT start with the full editor UX.

## Three parts, in order of how much is new
1. **Joint formation — now inference (`BuildMode::JointForContact`, LANDED).** When a placed piece abuts
   others, the connection profile is chosen automatically by material pairing + contact direction — see
   the ruling at the bottom for the exact contract: brick–brick bed → `GeneralPurposeMortar`; the
   vertical head/corner joint → `GeneralPurposeMortarPerpend`; **any timber contact → `DryStone` passive
   bearing** (a placed plank is resting, not fixed — fastening with `Screw`/`Nail`/`Bolt` is a later
   slice's explicit override, never auto-inferred). This encodes the same judgment
   `DestructionShed3D::BuildRealistic` applies by hand per joint; that builder's sweep is not yet wired
   onto the function (see CURRENT_STATE) — today they agree on every axis-aligned contact.
2. **Snap-candidate solver — the NEW BRAIN.** A well-bounded, testable unit:
   *(piece being placed + nearby pieces + their materials) → ranked candidate poses.* This is where
   the feature's expressiveness lives. Snap vocabulary below.
3. **Interaction / UI — the biggest new lump, SECOND.** Placement input, ghost preview, snap
   highlighting, a material palette, and save/load. Least existing coverage; hardest to unit-test
   (use functional tests + the `Visual.ScenarioLevelScreenshots`-style render harness).

## Snap vocabulary (the brain's rules)
- **Brick on brick:** the running-bond offset (half-brick stagger, 1 cm mortar gap) on the next
  course; same-course end-to-end; the corner return where two walls meet; optionally stack-bond as an
  alternate. Bricks "want" to overlap and stagger the way they're laid. (Brick geometry — 21.5 × 10.25
  × 6.5 cm on 1 cm joints, running-bond pitch — lives in `Core/DestructionShed3D.cpp` / the layout
  producers; read it, don't re-derive.)
- **Timber on brick:** centered on a brick (a lintel/plate over a course) OR flush to a brick edge —
  the two the owner named — plus post-on-plate and beam-on-post for framing.
- **Free placement** as the fallback when no snap is wanted.
Candidates are ranked; the caller (harness now, UI later) picks among them.

## Reuse map — what already exists (do NOT rebuild)
- `FStructure` (`Core/Structure.h/.cpp`) — pieces (mass, material, grounded), connections (joints with
  direction-dependent strength), `SolveLoads`, the break authority (LP ≤ 200 blocks, router above, plus
  the regional collapse prover). A built structure adopts into this and is immediately destructible.
- `DestructionLayout::FBrickLayout` / `FPieceBox {CentreCm, ExtentCm}` (`Core/Layout.h`) — the layout
  the world layer adopts.
- Programmatic builders as the proof-of-concept: `FRunningBondSpec`/RunningBond, `DestructionShed3D::
  BuildRealistic`, the corbel builders — they already lay running-bond brick + timber with correct
  joints. The build mode makes this *interactive* instead of *coded*.
- Connection profiles (`Core/Profiles/ConnectionProfiles.cpp`): `GeneralPurposeMortar`,
  `GeneralPurposeMortarPerpend`, `DryStone`, `Nail`/`Screw`/`Bolt`, `LimeMortar`. Material profiles
  (`Core/Profiles/MaterialProfiles.cpp`): `ClayBrick`, `Timber`.
- The geometry→profile selection in `BuildRealistic` (material pairing + interface normal: bed vs
  perpend vs corner vs timber-bearing) — factor this into a shared "joint for this contact" helper the
  build mode and the builders both call.
- World layer: `UDestructionStructureSubsystem` (`BuildLayout`, `AddPiece`, `Destroy`, `AdoptLayout`),
  `ABrickActor`, the piece-menu presenter (designed, largely not built — see CURRENT_STATE backlog).

## New parts
- The **snap-candidate solver** (part 2 above) — pure, testable geometry+materials logic.
- A **placement API** on top of the subsystem: place a piece at a pose (free or snapped), form its
  joints automatically, keep the structure live. Incremental — one piece at a time, not a whole layout.
- The **interactive UI** (part 3) — later.
- A **save/load format** for a player-built building (today buildings are programmatic builders
  selected by map name; a player building needs serialization). Later.

## Scope — first cut, then grow
- **First cut:** brick walls + corners + a door/window opening with a timber lintel + one timber-on-brick
  case (centered and edge-flush). This is the shed vocabulary, made placeable.
- **Grow:** posts/beams/framing, gable roofs, stack-bond, alternate materials, the interactive UI, save/load.

## Open decisions — surface to the owner before they change committed behavior
- **Joint override:** CLOSED for the default direction (owner-delegated 2026-09-03, see the ruling at
  the bottom) — infer the passive resting joint from materials + contact; masonry beds → mortar,
  heads/corners → perpend, any timber → DryStone bearing. Still OPEN: the explicit per-joint override
  ("screw this one vs let it rest") — a later slice, once fastening is added.
- **Live structural feedback while building:** the build mode *can* tell you when what you placed is
  over-capacity / would fall (same `SolveLoads`), or leave it to be discovered on "run." (Owner call.)
- **Persistence / save format:** a real new piece; shape it when the first cut works.

## Process (mandatory)
TDD, no exceptions: `test-expert` → `dev-expert` → `review-expert`; commit at every verified point;
house comment style (`/* */` blocks); assert on mechanism, never displacement. **Slice 1 = the
snap-candidate solver (running-bond brick overlap + timber centered-on/edge-of a brick + free
placement) plus automatic joint formation reusing the material-pairing/interface-normal profile
selection** — driven by unit tests (snap poses, chosen joint profiles, running-bond overlap) and a
render of a small placed structure. Later slices add the placement API loop, the interactive UI, and
save/load.

## Ruling — automatic joint inference default (decided 2026-09-03, owner-delegated)
The owner delegated this call ("make the best decision... I'll be expanding features... don't stop to
ask"), so it is decided rather than deferred:

**`JointForContact(materialA, materialB, interfaceNormal)` picks the PASSIVE resting joint:**
- both faces compression-dominant masonry (`bCompressionDominant`), interface normal ~ vertical (bed) →
  `GeneralPurposeMortar`.
- both masonry, interface normal horizontal (head / corner return) → `GeneralPurposeMortarPerpend`.
- **any face NOT compression-dominant (timber today) → `DryStone`** — a compression + friction BEARING,
  carrying no tension. A placed plank/lintel is *resting*, not *fixed*.
- Fastening (`Screw`/`Nail`/`Bolt`, tension-capable) is an **explicit override**, a later slice — never
  auto-inferred. This is the "joint override" open decision, now closed for the default direction.

Rationale: physically honest (nothing is tension-capable unless you say so), never hides an instability,
matches every roof-purlin/post bearing in `BuildRealistic`, and keys off a real material property
(`bCompressionDominant`) rather than profile identity, so it extends to new materials without a rewrite.
The discriminator is a *hint* to dev-expert; the test asserts on the returned profile, not the branch.
