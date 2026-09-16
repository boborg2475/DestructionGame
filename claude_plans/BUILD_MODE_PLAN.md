# Build Mode — interactive building with brick and wood (feature plan)

**Status: IN PROGRESS (started 2026-09-03).** Landed so far: `BuildMode::JointForContact` (the automatic
joint inference, see the ruling at the bottom) and the snap-candidate solver `BuildMode::SolveSnapCandidates`
(`Core/BuildMode/SnapSolver.*`) with these kinds: brick running-bond NEXT-COURSE bed, brick SAME-COURSE
end-to-end head (each auto-forming its joint by inference — bed → mortar, head → perpend; a brick in a wall
past course 1 coalesces into one candidate carrying both its bed and head joints), and TIMBER CENTERED-ON a
brick and TIMBER EDGE-FLUSH to a brick's near X-face (both bearings, DryStone by the ruling) whose bearings
are CONTACT-BASED so a lintel/plate forms a DryStone bearing to every brick it spans, deduped by neighbour.
Candidates are ranked by proximity ahead of a free-placement fallback. **The /goal's slice-1 snap
vocabulary is COMPLETE** (brick overlap, timber centered-on/edge-of, free). The PLACEMENT API
`BuildMode::PlacePiece` (`Core/BuildMode/Placement.*`) has also landed: it runs the solver against the
pieces already placed, adopts the piece at the best-ranked pose (mass from geometry, material set), and
forms that candidate's joints as real `FConnection`s via `MakeInterface` — so a scripted place/place/place
grows a live, jointed `FStructure`. And `BuildMode::BuildDemoBuilding` (`Core/BuildMode/DemoBuilding.*`)
scripts a small running-bond wall + timber wall-plate via `PlacePiece` and PROVES it STANDS (8 pieces, 14
joints — 6 mortar beds, 5 perpend heads, 3 DryStone bearings — every non-grounded piece Supported after
`SolveLoads`). And the RENDER landed — `DestructionGame.Visual.BuildDemoScreenshot` (a self-contained
NonNullRHI screenshot harness) stands the demo up and writes `Saved/Screenshots/WindowsEditor/BuildDemo.png`,
verified by eye as a faithful head-on view of the standing wall+plate. **THE FIRST PART IS CLOSED** (brain +
render). Follow-ups logged in CURRENT_STATE (half-buried grounded course, HUD banner, harness hardening).
NOW STARTING THE INTERACTIVE UI (the /goal's "after the first part") — see the UI design section below. CORNER-return is its own
later slice (needs the head-vs-corner inference extension — see CURRENT_STATE). Occupancy is handled — snap
candidates into an occupied cell are dropped; free placement is honoured verbatim (the owner-delegated
"place anywhere OR snap" ruling, DESIGN §8). (Open follow-ups — ranking policy, merged-candidate labelling,
half-bat bearings, the UI's occupied-pose warning signal — are in CURRENT_STATE.)
Read
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

## Interactive UI — the second part (design, 2026-09-04)
**The SCREEN's shape — the Cities-Skylines-style toolbar, the details panel, the cursor/camera scheme
and the S0–S15 slice order — is designed in [SESSION_UI_DESIGN.md](SESSION_UI_DESIGN.md) (2026-09-15).
This section owns the seams the UI drives; that document owns what the player sees.**
The /goal: "start the interactive UI after you are done with the first part." The brain (snap solver +
`JointForContact` + `PlacePiece` + occupancy) and its render close the first part. The UI layers input,
preview and a palette on that proven core — it invents no physics.

### The build loop (one placement)
1. Cursor/aim → a **requested world pose** (raycast onto a build plane, or onto the face of an existing
   piece for stacking). 2. Run `BuildMode::SolveSnapCandidates` against the current structure's boxes →
   ranked candidates. 3. Show a **ghost** of `Candidates[0]` at its `CentreCm`, tinted by `Kind` (and/or
   showing the joints it would form). 4. On confirm (click) → an incremental world-layer place: call
   `BuildMode::PlacePiece` on the bound `FBrickLayout`, then spawn ONE `ABrickActor` for the new piece and
   adopt the delta — the structure stays live and immediately destructible. 5. The existing destroy loop
   (cut/Release/settle) knocks it down.

### Reuse (do NOT rebuild)
- `BuildMode::PlacePiece` / `SolveSnapCandidates` / occupancy — the whole placement decision.
- `UDestructionStructureSubsystem` (`BuildLayout`, `AdoptLayout`, `Destroy`, `Find`, `CommitPieceAction`),
  `ABrickActor` (`Release`, **`SetHighlighted(EBrickHighlight)`** for the snap highlight, movable/kinematic
  body, `SetPieceRef`). `BrickHighlightForNeighbourSlot` already maps a slot → highlight colour.
- The scenario/world framing + the screenshot harness for functional-test coverage.

### New parts (each its own TDD slice)
- **UI-1 — incremental world place (the core new seam). LANDED 2026-09-04.**
  `UDestructionStructureSubsystem::BeginBuild() -> StructureId` opens an empty live structure;
  `PlaceBuildPiece(StructureId, RequestedCentreCm, ExtentCm, Material, Placement = Snap) -> FPieceRef` runs
  the snap brain (`SolveSnapCandidates`) against the structure's live pieces (Snap = `Candidates[0]`; Free =
  the Free candidate, the requested pose verbatim with no joints), spawns the ONE new `ABrickActor`, and
  forms the candidate's joints via `MakeInterface`/`AddConnection` — one call grows a live, jointed,
  actor-backed structure. Since 2026-09-15 the piece's GROUNDED flag is DERIVED from the snapped pose
  (bottom face within one joint of Z=0; DESIGN §8), never passed in. Fails closed (refused piece → actor destroyed, no desync); does NOT solve
  (live-feedback-off default). Driven by `World.BuildMode.PlaceBuildPieceGrowsALiveStructure`. Follow-ups
  (CURRENT_STATE): factor the shared decision→add helper as UI-2's first step; return `Kind`/joints not just
  a ref; the UI owns the cancel/Destroy of an abandoned build.
- **UI-2 — ghost preview + snap highlight.** Data source LANDED 2026-09-04:
  `UDestructionStructureSubsystem::PreviewBuildPiece(...) const -> FBuildPreview{bValid,Kind,CentreCm,
  JointCount}` — the non-mutating query the ghost draws from (and the world gather+decision is now one
  shared `ComputeBuildPlacement`). Driven by `World.BuildMode.PreviewBuildPieceIsNonMutatingAndPredictsTheCommit`.
  The preview ACTOR also landed via UI-4a's `UBuildModeComponent` (a standalone unbound ghost positioned by
  the shared `BrickSpawnTransform`, collision off). STILL TO DO: `SetHighlighted` on the neighbour(s) the
  snap would joint to tinted by `Kind`, a genuinely translucent ghost material, and the owed ghost-vs-placed
  screenshot (CURRENT_STATE) — judge the picture by eye.
- **UI-3 — material / piece palette.** Pick brick vs timber and a size; drives the `Material`+`ExtentCm`
  handed to `PlaceBuildPiece`. (First cut: ClayBrick full brick, Timber plate/lintel.) MODEL LANDED
  2026-09-15: `DestructionSession::EBuildPieceKind` + `BuildPieceHalfExtentCm`/`BuildPieceMaterial`
  (`Core/SessionToolbar.*`); the component/Slate wiring is the next slice.
- **UI-4 — build/destroy mode toggle + input.** The TOOLBAR MODEL landed 2026-09-15 (`DestructionSession::
  FSessionToolbarState` / `SessionToolbarButtons` / `ApplyToolbarButton`, the strip as data with Build and
  Destroy modes, Snap/Free, the course stepper and the Run command; the rests-on-the-ground course
  convention is a DESIGN §8 ruling). UI-4a LANDED 2026-09-04: `UBuildModeComponent` — the
  interactive loop's testable core (`BeginBuild`/`UpdatePreviewAt(cursor)`/`ConfirmPlace`, one preview →
  one commit). UI-4b ray seam LANDED: `UpdatePreviewFromRay(rayOrigin, rayDir)` intersects a world ray with
  the horizontal plane `Z == BuildPlaneZCm` and previews the hit (fails closed on parallel/behind/non-finite
  rays), so the controller's ONLY untestable part is the mouse deprojection. Driven by
  `World.BuildMode.Component*Ray*`. STILL TO DO (UI-4b wiring, functional/manual — the logic it calls is
  tested): on a build player controller/pawn, `DeprojectMousePositionToWorld` → `UpdatePreviewFromRay` on
  mouse-move, LMB → `ConfirmPlace`, keys to switch material and to raise the build plane a course / toggle
  destroy; a playable build level. LANDED 2026-09-15 (slice 2): the component's settings — `SetPieceKind`
  (material + half extent from the palette), `SetCourse` (`BuildPlaneZCm` from `CoursePlaneZCm`, the
  rests-on-the-ground ruling), `PlacementMode` Snap/Free, `CancelBuild` (and `BeginBuild` cancels an open
  build), and pose-derived grounded (`FBuildPreview::bGrounded`). The max-ray-distance clamp landed earlier.
  Still deferred (CURRENT_STATE): the ray-vs-brick-face "auto-plane from the piece under the cursor" variant.
- **UI-5 — save/load** a player building (OPEN decision below).
- **UI-6 — the JOINT CHOICE (owner ask, 2026-09-15: "build mode should allow the user to choose what type of
  joint between materials").** A Build-settings control `Joint: Auto | Mortar | Dry | Nail | Screw | Bolt`
  (`EJointChoice` on `FSessionToolbarState`, one `EToolbarButtonId` per choice, Settings group, a segmented
  chip like Snap/Free). `Auto` = today's inference (`JointForContact`). Any other choice OVERRIDES the profile
  of EVERY joint the placed piece forms (the snap candidates' `FFormedJoint.Profile` — the override rides
  through `PreviewBuildPiece`/`PlaceBuildPiece` as an optional `const FConnectionStrength*`; nullptr = auto),
  so a plank can be screwed to a plate or a brick laid dry. The choice is recorded per placement for save/load
  (it changes committed physics). The ghost card (S9) shows the joint that would form. This is the "fastening
  override" the 2026-09-03 ruling deferred, now explicit and player-driven.

### Open decisions (owner-delegated; decide when the slice is reached, record in DESIGN §8)
- **Live structural feedback while building:** RECOMMEND default OFF — build freely, discover on "run"
  (matches the existing game loop and keeps the loop simple); add an optional overlay later that runs
  `SolveLoads` and tints a piece that is over-capacity / would fall. Revisit if the owner wants it live.
- **Save/load format:** RECOMMEND the build is a serialized ordered list of placements
  `{RequestedCentreCm, PieceKind (→ extent + material), Placement (Snap/Free)}` replayed through the
  same placement door — deterministic (grounded is derived from the replayed pose, so it is NOT recorded;
  Placement IS, because it changes the committed pose and joints),
  tiny, and it reproduces the exact same joints because the solver is pure. (A raw piece/joint dump is the
  alternative; the replay list is smaller and self-validating.) Shape it when UI-1..4 work.
- **Occupied-pose warning:** the `bRequestedPoseOccupied` signal (CURRENT_STATE) so the UI can warn when
  only Free was available.
