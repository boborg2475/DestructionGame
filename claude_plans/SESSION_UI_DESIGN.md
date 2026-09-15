# Session UI — the build/destroy toolbar and the details panel

**Status: DESIGN, 2026-09-15.** Nothing here is built yet except the presenter model it is designed
onto (`Core/SessionToolbar.h`, landed and tested; `Core/PieceMenu.h`, landed and drawn).

The owner's ask, verbatim: *"design a strong ui/toolbar that I'm talking about. I should be able to
select items on the screen and see details about them as well."* And earlier: *"a Toolbar type UI
like in city skylines for building the structure where I can choose settings like snapping and free
placement, then move to a destroy mode that has different settings."*

Read [CLAUDE.md](../CLAUDE.md), [DESIGN.md](DESIGN.md), [BUILD_MODE_PLAN.md](BUILD_MODE_PLAN.md)
§"Interactive UI" and [CURRENT_STATE.md](CURRENT_STATE.md) first. This document owns the *shape* of
the screen; those own the model, the constants and the standing rulings, and where they disagree
with this they win.

---

## (a) Design principles

**1. The readout is an instrument; the toolbar is a toy box.** Every number on screen is a real
one the solver computed — newtons, megapascals, percentages of capacity, kilograms — and the
details WINDOW that explains a brick looks like a strain-gauge readout: thin rules, a monospaced
column for anything numeric, generous use of the word "why". The TOOLBAR is the opposite (owner
feedback 2026-09-15: "make the toolbar and the buttons look more fun"): chunky rounded chips with a
2 px drop edge that press down on click and lift on hover, a warm glow on the lit chip in the mode's
colour, a little brick / plank swatch drawn on each piece chip so the chip looks like the thing you
are about to lay, a bold rounded caption face, and small keycap glyphs for the shortcuts. Fun on the
strip, sober in the window.

**2. Readable at a glance while flying a camera.** The player is moving through the world with WASD
at all times. Two consequences that dominate every layout decision below: (i) **nothing important
may live in the middle third of the screen**, because that is where the wall is; (ii) **nothing may
move under a stationary cursor.** `Core/SessionToolbar.h` already encodes (ii) as a rule — the mode
pair is always the first two slots, and the strip's content depends on the mode alone — and this
document extends it: the details panel's dock does not move between modes, only its contents do.

**3. Two surfaces, and only two.** A bottom **toolbar** for *what the session is doing*, and a
right-edge **details panel** for *what one piece is doing*. Everything else — the scenario banner, a
transient warning — is a line of text inside one of those two. A third floating thing is how a
sandbox ends up with a HUD nobody can read.

**4. Slate only, no UMG, no new content except materials.** The project has no UMG assets, no
`UUserWidget`, no `FSlateStyleSet`. It has `FCoreStyle::Get().GetBrush("WhiteBrush")` and
`FCoreStyle::GetDefaultFontStyle`, and that is deliberate — `DestructionGamePlayerController.cpp`
records why (`FAppStyle` differs between the editor and `-game`, which are exactly the two places
this UI is looked at). Every panel in this design is flat-filled `SBorder` + `STextBlock` +
`SButton` + boxes. The only new *assets* anywhere in this plan are three overlay materials for the
load overlay (§b, Destroy settings), and each gets a `RequiredContent.h` row.

**5. The model decides, the widget draws.** This is the project's hardest UI rule and it is already
paid for twice. `Core/SessionToolbar.h` states it for the strip and `Core/PieceMenu.h` states it for
the readout: *which buttons exist, in what order, with what caption, lit or greyed, and what one
click does* is a list of decisions, and a decision spelled as a run of `AddSlot` calls is a decision
in the one place no test can reach. **Every control in §b maps to an `EToolbarButtonId`** — an
existing one where one exists, a named new one where it does not — and every string in §c maps to a
field on a presenter struct. If a control in this document cannot be expressed that way, it is not
designed yet.

**6. The colour of a thing is the model's decision; the hue is the widget's.** `EJointMarginBand`
and `EPieceSupportBand` exist for exactly this, and the pattern extends to everything new here. A
widget comparing a string against `"grounded"` to pick a green is the failure mode.

---

## (b) The toolbar

A strip across the bottom of the viewport, full width, **48 px tall** in Slate's scaled space
(owner feedback 2026-09-15: the first 72 px cut was "way too big"), sitting on the toolbar fill. It
is always up during a session. It never scrolls and never wraps: the longest configuration (Build
mode, ten chips) measures under 900 px, which clears 1280 with room.

```
[ Mode tabs ] │ [ ------------- the current mode's settings ------------- ] │ [ command ]
```

Three regions, left to right, separated by 1 px vertical rules at 14 % white:

- **Mode tabs** — fixed, always the first two slots, per `SessionToolbarButtons`' ordering rule.
- **Settings group** — everything that changes with the mode.
- **Command** — the mode's one big command (`Run structure`) at the far right, on its own.

**The toolbar carries NO readouts** (owner feedback 2026-09-15): piece counts, joint counts and
"what is selected" belong in the details WINDOW (§c), never on the strip. The strip is settings and
commands only. (The earlier status-readout idea is dropped; `FSessionStatusReadout` moves into the
window's footer if it is built at all.)

The commands (`Clear build`, `Run structure`) sit at the **right end of the settings group**,
separated from the settings by a rule, so a destructive click is never adjacent to a setting click.

### Mode tabs

| Control | Id | Caption | Active when | Enabled when | Shortcut |
|---|---|---|---|---|---|
| Build tab | `ModeBuild` | `Build` | `Mode == Build` | always | `Tab` (toggles) |
| Destroy tab | `ModeDestroy` | `Destroy` | `Mode != Build` | always | `Tab` (toggles) |

Both are always enabled — `SessionToolbarIsEnabled`'s default arm, and its comment already says
why: *a mode button greyed by an over-eager precondition is a player who cannot get out of the mode
they are in.* They are drawn as **tabs, not buttons**: the active one is filled with the panel's own
background and carries a 2 px top accent bar in the mode's accent colour (build amber, destroy red);
the inactive one is a flat 8 % white fill with no accent. That is the one place in this design where
a control's *shape* changes with state, and it is worth it — mode is the highest-order fact on the
screen and it must be legible from peripheral vision.

`Tab` toggles rather than binding a key per tab, because there are exactly two and a toggle is one
binding instead of two. `ApplyToolbarButton` is still what runs: the key handler reads the current
mode and dispatches `ModeBuild` or `ModeDestroy`. **No new model function** — the toggle is a
one-line read in the controller and the transition is still the tested pure one.

### Build settings group

| Control | Id | Caption | Caption 2 (small) | Active when | Enabled when | Shortcut |
|---|---|---|---|---|---|---|
| Piece: brick | `PieceBrick` | `Brick` | `21.5 × 10.25 × 6.5 cm` | `Piece == Brick` | always | `1` |
| Piece: plate | `PieceTimberPlate` | `Timber plate` | `67.5 × 10.25 × 10 cm` | `Piece == TimberPlate` | always | `2` |
| Piece: lintel | `PieceTimberLintel` | `Timber lintel` | `90 × 10.25 × 10 cm` | `Piece == TimberLintel` | always | `3` |
| Placement: snap | `PlacementSnap` | `Snap` | — | `Placement == Snap` | always | `G` (toggles) |
| Placement: free | `PlacementFree` | `Free` | — | `Placement != Snap` | always | `G` (toggles) |
| Course down | `CourseDown` | `−` | — | never (command) | `Course >= 1` | `[` |
| Course readout | *(not a button)* | `Course 3` | — | — | — | — |
| Course up | `CourseUp` | `+` | — | never (command) | always | `]` |
| Clear build | `ClearBuild` | `Clear build` | — | never (command) | `bHasStructure` | `Backspace` |

Four notes, each of which is a decision rather than a transcription:

**The size captions are DERIVED, never typed.** `BuildPieceHalfExtentCm(Kind)` already owns the
numbers and its header says why (the brick is read from `FSnapSettings`, so the toolbar does not
become a third place brick dimensions are written down). The caption is `2 × HalfExtent` formatted,
and it belongs on `FToolbarButton` as a new `FString SubLabel` field — **not** composed in the
widget, for the reason `FToolbarButton::Label` is not composed in the widget. A kind this build does
not know answers the zero extent, so its caption is empty rather than `0 × 0 × 0 cm`.

**Snap/Free is drawn as a segmented control**, two buttons sharing one outline with no gap, because
they are one setting with two values rather than two settings. `G` toggles between them (`G` for
grid; `S` is unavailable — it is strafe-back on the flying pawn, and §d's key budget is dominated by
`W A S D Q E` being spoken for).

**The course stepper is `−  Course 3  +`**, with the readout between the two buttons and coming from
`CourseLabel(Course)` — which already clamps a negative course to 0 and already spells the word. The
`−` button greys at course 0; `ApplyToolbarButton` *refuses* rather than clamps there, and its
comment explains why that distinction will matter later. The stepper also takes the **mouse wheel**
while the cursor is over the viewport in Build mode, which is the city-builder gesture and costs one
binding.

**`Clear build` asks first.** It is the only irreversible control in the Build group and
`FPieceAction::bIsDestructive` already establishes the house rule that destructiveness is data. The
confirm is not a modal dialog — it is the button **arming**: first click swaps its caption to
`Clear build — confirm?` and its fill to the destructive red for three seconds; second click within
that window runs. This needs a state field (`double ClearArmedAtSeconds`) and therefore a new model
transition, so it is its own slice (§g, S5b), and until it lands `ClearBuild` is a plain button.

### Destroy settings group

The owner's brief named three candidates. Two are buildable today with what the engine already
answers; one is not, and saying so is more useful than shipping a toggle that fights an invariant.

| Control | Id (new) | Caption | Active when | Enabled when | Shortcut |
|---|---|---|---|---|---|
| Run structure | `RunStructure` *(exists)* | `Run structure` | never (command) | `bHasStructure` | `Enter` |
| Load overlay | `ToggleLoadOverlay` | `Load overlay` | `bLoadOverlay` | `bHasStructure` | `L` |
| Removal forecast | `ToggleRemovalForecast` | `Removal forecast` | `bRemovalForecast` | `bHasStructure` | `X` |
| Joint lines | `ToggleJointLines` | `Joint lines` | `bJointLines` | `bHasStructure` | `J` |

Each adds one `bool` to `FSessionToolbarState` and one arm to `SessionToolbarIsActive` — they are
**settings, so they latch**, unlike `RunStructure` which is a command and per the model's own rule is
never lit.

**Load overlay (`L`) — tint every piece by its worst joint's utilisation.** Buildable exactly as
written: `FStructureBinding::SolveLoads()` is documented as non-destructive and re-runnable
("solving must leave every connection exactly as intact as it found it"), and
`FStructure::GetConnectionUtilisation(i)` is the same number the break decision uses. Per piece, take
the max utilisation over its joints (the scan `InspectPiece` already does), bucket it with
`EJointMarginBand`, and set a new highlight. Cost is one solve per toggle-on and one per mutation,
not per frame. **It is the single most valuable thing this UI can add**: it turns "pull bricks until
something falls" into "see where the load is, then pull *that* one", which is the whole promise of a
structural-integrity game.

*Implementation shape:* three new `EBrickHighlight` enumerators — `LoadComfortable`, `LoadCaution`,
`LoadCritical` — placed at the **weakest** end of `HighlightForPiece`'s precedence, below `Hovered`,
so the overlay never hides what the player is pointing at. Three new materials, three
`RequiredContent.h` rows, and three emissive constants beside them. The precedence order is a
judgement and therefore belongs in `HighlightForPiece` with a test, exactly as the existing four do.

**Removal forecast (`X`) — "removing this drops 34 pieces".** Also buildable, and the reason is
worth stating: `FStructure` is a plain struct of `TArray`s with no `TUniquePtr` and no pimpl, so it
**copies by value**. A forecast is therefore: copy the structure, `RemovePiece(handle)` on the copy,
`SolveAndBreak()` on the copy, count the pieces the copy lost and the joints it broke. Nothing about
the live structure is touched, which matters because `SolveAndPush`'s header is emphatic that
settling is destructive and irreversible and *"nothing asking what-if may call it."* This is that
what-if, done the only way that respects that sentence.

*It is expensive* — a full cascade — so it is on-demand rather than continuous: it runs when the
hovered piece has been stable for ~200 ms, and it refuses above a piece cap (start at 400, the
neighbourhood of the LP's own 200-block gate) with the honest readout `Too large to forecast`
rather than a stall. The result lands as two new lines in the details panel and, optionally, the
forecast-doomed pieces lit in the critical colour.

**Joint lines (`J`) — draw the inspected piece's joints in the world.** Buildable with
`DrawDebugLine` between brick actor locations, coloured by `FInspectorJointRow::ColourSlot` through
the **same** `DestructionContent::BrickNeighbourSwatchColours` array the panel's swatches use — so
the line, the swatch and the far brick's overlay are one decision drawn three ways. It is the
cheapest of the three and the least essential (the neighbour overlays already say *which* brick);
what it adds is *where the joint is*, which matters for a bearing under a lintel.

**"Auto-run after delete" vs "hold" — NOT in the first cut, and here is why.**
`RunPieceActions`' header states the invariant plainly: *"There is deliberately no public 'run
without settling': a caller who forgot it and then pushed would release pieces against a stale
answer, and releasing is irreversible."* A `hold` toggle is a public run-without-settling by another
name. The player-facing want behind it is real and good — *carve a shape, then hit Run and watch* —
but the honest way to build it is a settle **policy** threaded through `RunPieceActions` such that
the no-settle path also refuses to push, which is a change to a load-bearing Core invariant and
needs its own red test and its own review. **Recommendation: defer, and log it in CURRENT_STATE.**
In the meantime `Run structure` is the explicit settle, and the toolbar's Destroy group reads
honestly: every delete settles, and Run re-settles whatever the player has been staring at.

### Status readout — NOT on the toolbar

Dropped from the strip (owner feedback 2026-09-15). If a session summary line is ever wanted —
`18 pieces · 31 joints · 4 grounded · worst joint 62 %`, every figure an existing accessor
(`NumPieces`, the connection count, `GetPieceSupport`, `GetConnectionUtilisation`), `not solved`
until the load overlay has solved — it goes in the details WINDOW's footer as an
`FSessionStatusReadout` presenter struct, never on the strip.

---

## (c) Selection and details

### What clicking and hovering do, per mode

| | Hover | Left click | Right click | Escape |
|---|---|---|---|---|
| **Build** | ghost follows the cursor on the build plane; neighbours the snap would joint to are tinted by kind | **place** (`ConfirmPlace`) | *held* = camera look (§d) | clears the ghost's held preview |
| **Destroy** | `HoverAlongRay` → `Hovered` overlay; details panel shows a *peek* of the piece | `InspectAlongRay` → add to selection, panel shows it | *held* = camera look | clear selection, dismiss panel |

Two changes from today, both small and both worth arguing:

**Hover in Destroy mode populates the panel, read-only.** Today the panel exists only once something
is selected. That makes the common motion — sweep the cursor along a wall looking for the loaded
joint — silent. The peek is the *same* `FPieceMenuInspector`, built with the hovered ref as a
one-element selection, drawn at `EPieceMenuDetail::Compact`, and visibly marked as a peek (a
`Pointing at` header instead of `Selected`). It costs one `InspectPiece` per hovered-piece *change*
— not per frame; `HoverAlongRay` already early-outs when the ref is unchanged. When a real selection
exists, the selection wins and the peek is suppressed: a panel that flickered between the brick you
picked and the brick your cursor drifted over would be unreadable.

**Build mode has no selection at all.** Clicking places. There is no "select a placed piece in Build
mode" — that is what Destroy mode is for, and a mode that did both would need a modifier key to
disambiguate every click, which is the thing city-builder toolbars exist to avoid.

### The details panel — where it lives

**It is its own WINDOW, not part of the toolbar** (owner feedback 2026-09-15: "it can be a window
that defaults to opening on the right hand side of the screen and can click and drag"). **Docked at
the right edge by default, vertically centred, draggable by its grab strip, clamped to the screen.**
That is exactly what `PieceMenuHomeOffset` and `ClampPanelOffset` already do for the piece menu, and
the reasoning in their headers stands unchanged — the session's details window IS the piece menu
panel, grown by the fields below. **One change is required**: both take the viewport, and the
viewport now has a 48 px toolbar across the bottom of it. A panel dragged into the bottom-right
corner would sit *under* the toolbar.

*The fix is a new pure function, not an inset baked into the widget:*

```cpp
/** The viewport minus the furniture that is always on top of it. */
FVector2D SessionSafeAreaPx(FVector2D ViewportSizePx, double ToolbarHeightPx);
```

…whose result is what gets handed to `PieceMenuHomeOffset` and `ClampPanelOffset` in place of the
raw viewport. A non-finite or negative toolbar height is treated as zero (the panel keeps the whole
screen — the fail-open direction is right here, because the failure being guarded against is a panel
that cannot be reached, not one that overlaps a strip). One function, one red test, and both
existing clamps compose with it unchanged.

Sizes: `PieceMenuPanelSizePx` already answers **640 × h** for `Full` and a measured-narrower size for
`Compact`, both derived from the longest line the readout can compose. The Build-mode variant is a
third detail mode (below) and gets its own measured size by the same rule.

### The details panel — what a selected piece shows (Destroy mode)

Top to bottom. Everything marked *(exists)* is already a field on `FPieceMenuInspector` and already
drawn; everything marked *(new)* is a new field and therefore a new red test.

```
┌ grab strip ────────────────────────────────── ⌄ ┐   ← drag to move, double-click to compact
│ PIECE INSPECTOR                          3 bricks│   HeaderText (exists) · CountText (exists)
├──────────────────────────────────────────────────┤
│ ● course 2 · #14        Supported                 │   entry rows (exist): Label, SupportText,
│ ● course 2 · #15        Supported                 │   SupportBand dot (exists)
│ ◆ course 3 · #27        Supported                 │   ◆ = bIsInspected (exists)
├──────────────────────────────────────────────────┤
│ course 3 · #27                                    │   InspectedLabel (exists)
│ Clay brick · 21.5 × 10.25 × 6.5 cm · 2.9 kg       │   (new) IdentityText
│ ● Supported — held by a path to the earth         │   SupportText (exists), SupportBand (exists)
│                                                   │
│ 4 joints, worst at 38 % of capacity               │   JointsText (exists)
│ ■ bed mortar      below  ████████░░  38 %  262 N  │   joint rows (exist): ColourSlot swatch,
│ ■ perpend head    left   ██░░░░░░░░  12 %   41 N  │   Text (exists) — whole line composed in
│ ■ perpend head    right  ██░░░░░░░░  11 %   38 N  │   the model
│ ■ bed mortar      above  █░░░░░░░░░   6 %   19 N  │
│      10×      100×     1000× margin               │   HeadroomScale ticks (exist)
├──────────────────────────────────────────────────┤
│ REMOVING THIS                                     │   (new) forecast block, Destroy mode only
│ 6 pieces fall · 9 joints give · 2 cascade passes  │   (new) ForecastText
├──────────────────────────────────────────────────┤
│ [ Delete ]                                        │   FPieceMenuRow buttons (exist),
└──────────────────────────────────────────────────┘   bIsDestructive → red (exists)
```

Three new model fields, each its own slice:

- **`IdentityText`** *(new)* — `"Clay brick · 21.5 × 10.25 × 6.5 cm · 2.9 kg"`. Every part is
  already in the graph: the material name by pointer identity against `AllMaterialProfiles()` (the
  identity rule `BuildPieceMaterial` states is what makes this safe), the size from
  `FPieceBinding::Box.ExtentCm` doubled, the mass from `FStructure::GetPiece(i).MassKg`. It is one
  composed string for the reason every other string here is: choosing a unit and a precision is
  logic. A piece whose material is not a library row reads `Unknown material`, not a blank.
- **`GroundedText`** *(new, folded into `SupportText`'s line)* — `Grounded` already comes through
  `EPieceSupportBand::Grounded`; nothing new is needed. Listed here only so nobody adds a second
  field for it.
- **`ForecastText`** *(new)* — `"6 pieces fall · 9 joints give · 2 cascade passes"`, or
  `"Nothing falls"`, or `"Too large to forecast"`, or empty when the forecast setting is off. The
  three-state emptiness follows `InspectedHintText`'s pattern exactly: an absence is a fact to be
  stated, and the state where the block is *not drawn at all* is distinct from the state where it is
  drawn saying nothing happens.

The **joint rows already carry everything** the wireframe shows — `Text` is composed in the model
(`FInspectorJointRow::Text`, and its comment explains at length why the widget may not compose it),
`HeadroomFraction` drives the bar, `MarginBand` picks its colour, `ColourSlot` picks the swatch that
matches the far brick's overlay. **Nothing in the joint table needs to change.** That is the payoff
of the model-decides rule: the hardest part of this panel is already built and already tested.

### The details panel — Build mode (the ghost card)

Same dock, same width, different contents. It is a third `EPieceMenuDetail` — `GhostCard` — so that
`PieceMenuPanelSizePx` keeps owning the size, and a new sibling presenter struct
`FBuildPreviewInspector` (a sibling of `FPieceMenuInspector` for the reason that one is a sibling of
`FPieceMenuRow`: a readout and a command have different fail-closed polarities).

```
┌ grab strip ─────────────────────────────┐
│ PLACING                        Snap      │  HeaderText, PlacementText
├──────────────────────────────────────────┤
│ Brick                                    │  PieceText (from the palette caption)
│ Clay brick · 21.5 × 10.25 × 6.5 cm · 2.9 kg │  IdentityText — SAME composer as the inspector's
│ Course 3 · centre Z 25.75 cm             │  CourseText (from CourseLabel + CoursePlaneZCm)
│                                          │
│ Running bond, next course                │  KindText — the ESnapKind word
│ Would form 3 joints                      │  JointsText
│   ■ bed mortar to 2 bricks below         │  joint preview rows (from FSnapCandidate::Joints)
│   ■ perpend head to 1 brick alongside    │
│                                          │
│ ⚠ Nothing to snap to — free placement     │  WarningText, present only when it applies
└──────────────────────────────────────────┘
```

`FBuildPreview` already carries `bValid`, `Kind`, `CentreCm` and `JointCount`. Three prerequisites
from CURRENT_STATE have to land for this card to be honest, and they are already logged there:

- **(b2)** `bValid` means "the structure id is known", not "a piece would land". The card needs
  `bWouldPlace` or it will show a confident preview for a placement the commit then refuses.
- **(v-a)** the `bRequestedPoseOccupied` signal, so the `⚠` line can distinguish *"you are building
  in open space, free placement is fine"* from *"every snap here is blocked — you are about to
  overlap a brick."* Those are completely different facts and today they produce the same `Free`.
- **(vii)** `Kind` on a merged candidate is order-dependent. A brick past course 1 correctly forms
  both a bed and a head joint but reports whichever neighbour was listed first. **`KindText` must
  not be keyed off `Kind` until that is a truthful label** — a `Mixed`/`Wall` kind or a bitset. Until
  then the card should read the *joints* (which are correct and complete) and describe those, which
  is what the wireframe above does: the joint rows are the truth and `KindText` is the summary.

The warning line is the only red thing in Build mode, and it is text rather than an icon, because a
`⚠` with no sentence beside it is a mystery.

---

## (d) Cursor and camera

### What is true today

`IMC_MouseLook` binds the raw `Mouse2D` axis **unconditionally, with no held button**, so the camera
follows the mouse all the time and there is no pointer. The only way a cursor appears is
`SetPieceMenuControls(true)`, which *removes the whole context* for as long as a menu is up. That is
a coherent design for a game whose only UI is a transient context menu. **It is incompatible with a
toolbar that is always on screen** — a permanent strip you can only click by first opening a piece
menu is not a toolbar.

### The proposed scheme

**The cursor is visible for the whole session, in both modes. Camera look is a held right-drag.**

| Input | Build | Destroy |
|---|---|---|
| `W A S D` | fly | fly |
| `Q` / `E` (`IA_Jump`) | descend / ascend | descend / ascend |
| Mouse move | ghost follows the build plane; cursor over UI | hover highlight; cursor over UI |
| **RMB held** + mouse move | **camera look** (cursor hidden, recentred on release) | same |
| LMB | place | select / toggle selection |
| Mouse wheel | course ± | *(free — reserve for fly speed)* |

This is the scheme every building game converges on, and it costs one held button. It also makes the
two modes *identical* in camera behaviour, which is the point: switching mode must not change how
the camera works, or the player has to re-learn flying twice a minute.

### Input-asset implications — precisely

1. **New asset `IA_LookModifier`** (Digital / bool), and a `RequiredContent.h` row for it beside
   `MouseLookActionPath`.
2. **`IMC_MouseLook`'s `IA_MouseLook` mapping gains a `Chorded Action` trigger** referencing
   `IA_LookModifier`; `IMC_Default` (or a new `IMC_Session`) maps `IA_LookModifier` to the right
   mouse button. This is an **asset edit, not code** — `*.uasset` changes are TDD-exempt per
   CLAUDE.md, but the *consequence* is not: a test that asserts the mapping context's shape is the
   right way to keep it, and `Tests/RequiredContentTest.cpp` is where it goes.
3. **`SetPieceMenuControls` loses its context removal entirely** and is replaced by a session-wide
   `SetSessionControls()` called once in `BeginPlay`: `bShowMouseCursor = true` and
   `FInputModeGameAndUI` with `SetHideCursorDuringCapture(false)` and no widget to focus. The
   function's own header currently argues for one-function-with-one-branch so the restore cannot
   fall out of step with the apply — with a permanent cursor there is no apply/restore pair left to
   fall out of step, which is strictly simpler.
4. **`IA_HoverPiece` can now be folded back into `IA_MouseLook`** — it exists *only* because
   `IMC_MouseLook` was being removed under the menu (both headers say so explicitly). Once nothing
   removes the context, the separate action has no reason to exist. **Recommendation: keep it
   anyway, for now.** It costs one asset, it consumes nothing, and deleting it is a content change
   that would make the hover path depend on a chorded trigger firing while un-chorded. Log the
   simplification rather than taking it in the same slice.
5. **Keyboard must keep reaching the pawn after a toolbar click.** A Slate `SButton` takes user focus
   on click by default, and the flying pawn then stops responding to `W`. Every button in this design
   is built with **`.IsFocusable(false)`**, and the toolbar's root border does not accept focus. This
   is a one-line-per-button discipline and it is the single most likely thing to be got wrong; it
   deserves a line in the slice's acceptance notes and a human check on the screenshot proof.
6. **`F1` hides all session UI.** One bool, both panels' visibility bound to it. It is worth its own
   binding because the screenshot harnesses (`Visual.BuildDemoScreenshot` and friends) currently
   capture the scenario banner and CURRENT_STATE already logs that as a wart — a hide-UI key makes
   the clean plate a keystroke rather than a code change.

---

## (e) Visual language

### Palette

Slate takes **linear** `FLinearColor`s; the hexes below are the **sRGB display** values the same
colours land at. Both are given because the implementer writes the linear triple and the designer
checks the hex, and confusing the two is how a palette drifts. Values marked *(exists)* are already
in `DestructionGamePlayerController.cpp` or `RequiredContent.h` and **must be reused, not re-picked**.

| Role | Linear | sRGB | Note |
|---|---|---|---|
| Panel fill | `0.014, 0.016, 0.022` @ .94 | `#1F2229` | *(exists)* — the whole UI sits on this |
| Toolbar fill | `0.020, 0.023, 0.030` @ .96 | `#252932` | one step lighter than the panel, so the two read as separate objects |
| Grab strip / inactive tab | `0.16, 0.18, 0.24` @ .75 | `#6F7686` | *(exists)* |
| Rule | white @ .16 | — | *(exists)* — every divider in this design |
| Header text | `1, 1, 1` | `#FFFFFF` | *(exists)* |
| Readout text | `0.82, 0.86, 0.92` | `#EAEFF6` | *(exists)* — every number |
| Secondary text | `0.62, 0.68, 0.78` | `#CED7E5` | *(exists)* — counts, captions |
| Hint text | `0.55, 0.60, 0.68` | `#C4CBD7` | *(exists)* — "hover an entry…" |
| **Build accent** | `0.95, 0.66, 0.13` | `#F9D465` | *(exists as Caution)* — the build tab's bar, the ghost's gold |
| **Destroy accent** | `0.72, 0.16, 0.14` | `#DD6F69` | *(exists as DestructiveRow)* — the destroy tab's bar |
| Band: comfortable | `0.18, 0.76, 0.55` | `#76E2C4` | *(exists)* |
| Band: caution | `0.95, 0.66, 0.13` | `#F9D465` | *(exists)* |
| Band: critical | `0.95, 0.24, 0.20` | `#F9867C` | *(exists)* |
| Support: grounded | `0.22, 0.56, 0.86` | `#81C5EF` | *(exists)* |
| Support: not solved | `0.45, 0.55, 0.78` | `#B3C4E5` | *(exists)* |
| Support: not a piece | `0.36, 0.37, 0.40` | `#A2A4AA` | *(exists)* |

**World colours the UI borrows from** (these are the game's identity — brick-red, timber-tan,
mortar-grey — and the toolbar's piece swatches use them so the palette chip and the thing you are
about to place are the same colour):

| Role | sRGB | Source |
|---|---|---|
| Clay brick | `#9E3B2E` | `M_Shed_Brick`, as it reads in `BuildGhost_Preview.png` |
| Timber | `#C2A06A` | `M_Shed_Timber` |
| Mortar / ground | `#8E8B85` | the sandbox floor |

**Highlight overlays** (`RequiredContent.h`, additive over a lit surface — its comment is explicit
that *"equality here is not equality on screen"*, so nobody may retune one of these to match a
screenshot):

| State | Reads as | Asset |
|---|---|---|
| `Hovered` | gold | `M_BrickHover` — and it is also the ghost's colour today |
| `Selected` | teal | `M_BrickSelected` |
| `Inspected` | magenta | `M_BrickInspected` |
| Neighbour slots 0–5 | `#00E700` `#6C89FF` `#B35900` `#FF0061` `#DDC4FF` `#7CB33F` | `BrickNeighbourSwatchColours`, index for index |

### Type

`FCoreStyle::GetDefaultFontStyle`, which is what the existing panel uses and what works identically
in the editor and in `-game`.

| Use | Style | Size |
|---|---|---|
| Panel / section header | `Bold` | 13 *(exists)* |
| Toolbar button caption | `Bold` | 11 |
| Body, joint rows, readout | `Regular` | 9 *(exists)* |
| Size captions, shortcuts, scale ticks | `Regular` | 7 *(exists)* |

Only four sizes. Numbers and captions are drawn at the same size as body text — a readout where the
number is bigger than its label is a scoreboard, not an instrument.

### Spacing

An **8 px grid**, everywhere, with two exceptions inherited from the existing panel (its 14/10
banner padding and its 10 px indent). Toolbar: 48 px tall, 34 px chips, 5 px vertical padding,
10 px between groups (with a 1 px rule), 5 px between chips within a group, 0 px inside a segmented
control. Panel: 12 px padding, 8 px between rows, 16 px above a rule and 12 px below it.

### How active state is rendered (the chips)

**Three visual states, and they must be three, for the reason `EBrickHighlight` has ten and not one:
`bActive` and `bEnabled` are different questions and a widget that drew them alike would make a lit
button that does nothing indistinguishable from a greyed one that works.**

| State | Fill | Caption | Edge |
|---|---|---|---|
| Idle (`!bActive, bEnabled`) | chip fill (light: white; dark: `#2A2F3A`), 10 px radius | body text | 2 px drop edge below (the "press me" cue) |
| Hovered | chip fill one step warmer, lifted 1 px | header text | drop edge |
| Pressed | pushed down 1 px | — | drop edge collapses to 0 |
| **Active** (`bActive`) | vertical gradient of the mode accent (light→accent), dark ink caption | header text | drop edge in the accent's shadow + a soft glow in the accent |
| **Disabled** (`!bEnabled`) | chip fill @ 3 %, 50 % opacity | dim text | none |

The mode tabs are the same chip, one size up, with a 16 px icon (a brick bond for Build, a burst for
Destroy). `Run structure` is a command chip filled in the destroy accent. The three piece chips carry
a CSS-drawn swatch — a brick-red block, a timber-tan plank at two lengths — in place of a size
caption; the size lives in a hover tooltip and on the ghost card.

A **command** (`ClearBuild`, `RunStructure`, `CourseUp/Down`) never reaches the Active row —
`SessionToolbarIsActive`'s default arm guarantees it, and its comment says why: *a latched Clear
button reads as a mode the player is stuck in.* The widget does not need to know that; it just draws
`bActive`.

Shortcuts are drawn as a 7 pt glyph in the button's top-right corner at 45 % opacity. They come from
the model (`FToolbarButton::ShortcutText`), for the reason captions do.

---

## (f) Wireframes

### Build mode, 1280 × 720

```
┌────────────────────────────────────────────────────────────────────────────────────────────┐
│  Build sandbox — your own building                                                          │  ← scenario banner (exists)
│  Nothing is cut. Lay bricks, then switch to Destroy and pull one out.                       │
│                                                                                             │
│                                                                                             │
│                                                                                             │
│                             ░░░░░░░░░░░░                          ┌ ─────────────────── ⌄ ┐│
│                             ░ gold ghost ░                        │ PLACING        Snap    ││
│                    ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓                   ├────────────────────────┤│
│                    ▓▓▓▓▓▓▓▓▓▓ brick wall ▓▓▓▓▓▓                   │ Brick                  ││
│                    ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓                   │ Clay brick · 21.5 ×    ││
│  ──────────────────────────────────────────────────────────────── │ 10.25 × 6.5 cm · 2.9 kg││
│                                                                   │ Course 3 · centre Z    ││
│                                                                   │ 25.75 cm               ││
│                                                                   │                        ││
│                                                                   │ Running bond, next     ││
│                                                                   │ course                 ││
│                                                                   │ Would form 3 joints    ││
│                                                                   │  ■ bed mortar × 2      ││
│                                                                   │  ■ perpend head × 1    ││
│                                                                   └────────────────────────┘│
├────────────────────────────────────────────────────────────────────────────────────────────┤
│▁▁▁▁▁▁▁▁▁                                                                                    │
│┃ BUILD  ┃ DESTROY │ Brick    Timber plate  Timber lintel │ Snap│Free │ −  Course 3  + │ Clear│
│┃ Tab    ┃         │ 21.5×10… 67.5×10.25…   90×10.25…     │ G   │     │ [       ]     │ build│
│                                          18 pieces · 31 joints · 4 grounded · not solved    │
└────────────────────────────────────────────────────────────────────────────────────────────┘
```

### Destroy mode, 1280 × 720

```
┌────────────────────────────────────────────────────────────────────────────────────────────┐
│  Build sandbox — your own building                                                          │
│  Nothing is cut. Lay bricks, then switch to Destroy and pull one out.                       │
│                                                                                             │
│                                                          ┌ ──────────────────────────── ⌄ ┐ │
│                    ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓          │ PIECE INSPECTOR       3 bricks │ │
│                    ▓▓▓▓▒▒▓▓✦▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓          ├────────────────────────────────┤ │
│                    ▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓▓          │ ● course 2 · #14     Supported │ │
│  ─────────────────────────────────────────────────       │ ● course 2 · #15     Supported │ │
│      ✦ = inspected (magenta)   ▒ = selected (teal)        │ ◆ course 3 · #27     Supported │ │
│                                                          ├────────────────────────────────┤ │
│                                                          │ course 3 · #27                 │ │
│                                                          │ Clay brick · 21.5 × 10.25 ×    │ │
│                                                          │ 6.5 cm · 2.9 kg                │ │
│                                                          │ ● Supported — held by a path   │ │
│                                                          │   to the earth                 │ │
│                                                          │ 4 joints, worst at 38 %        │ │
│                                                          │ ■ bed mortar   ████████░░ 38 % │ │
│                                                          │ ■ perpend head ██░░░░░░░░ 12 % │ │
│                                                          │ ■ perpend head ██░░░░░░░░ 11 % │ │
│                                                          │ ■ bed mortar   █░░░░░░░░░  6 % │ │
│                                                          │     10×   100×   1000× margin  │ │
│                                                          ├────────────────────────────────┤ │
│                                                          │ REMOVING THIS                  │ │
│                                                          │ 6 pieces fall · 9 joints give  │ │
│                                                          ├────────────────────────────────┤ │
│                                                          │ [        Delete 3 bricks      ]│ │
│                                                          └────────────────────────────────┘ │
├────────────────────────────────────────────────────────────────────────────────────────────┤
│▁▁▁▁▁▁▁▁▁▁▁                                                                                  │
│┃ BUILD ┃ DESTROY ┃│ Load overlay │ Removal forecast │ Joint lines ││ Run structure          │
│        ┃   Tab   ┃│      L       │        X         │      J      ││    Enter               │
│                                        18 pieces · 31 joints · 4 grounded · worst joint 62 %│
└────────────────────────────────────────────────────────────────────────────────────────────┘
```

### Degrading to a narrow viewport

Below ~1000 px the toolbar drops the size captions first, then the shortcut glyphs, then the status
readout. It **never wraps to two rows** — the mode tabs must stay at a fixed screen position, which
is `SessionToolbarButtons`' ordering rule taken to its conclusion. What it may do is drop the piece
captions to initials (`Brick` / `Plate` / `Lintel`). All three of those are *model* decisions and
belong on a `FToolbarLayout` that takes the available width — not `if (Width < 1000)` in Slate.

---

## (g) Implementation order

Thin slices, each with a red test first, each mapped to an existing seam. The public
`Build*Panel() -> TSharedRef<SWidget>` shape and the ray-not-cursor controller seams are what make
most of these headless-testable; `BuildPieceMenuPanel`'s header explains why that matters and it is
the pattern every new panel follows.

| # | Slice | Seam | Red test | Screenshot proof shows |
|---|---|---|---|---|
| **S0** | A playable build level | `DestructionScenarios.cpp` catalogue row + `Scripts/New-ScenarioMap.ps1` + a `LEVELS.md` entry | `Content.ScenarioMapsAreDistinctAssets` (exists) + a catalogue-row test | the empty build sandbox, no content placed |
| **S1** | Toolbar groups + sub-captions | `FToolbarButton` gains `Group` (`EToolbarGroup`) and `SubLabel` | `Core.SessionToolbar.Groups` — group per button per mode; sub-caption is `2 ×` the half extent and empty for an unknown kind | — (model only) |
| **S2** | Shortcuts on the model | `FToolbarButton::ShortcutText` + `EToolbarButtonId ToolbarButtonForShortcut(TCHAR)` | `Core.SessionToolbar.Shortcuts` — every drawn button has one, unique within its mode, round-trips through the reverse lookup, and never collides with `W A S D Q E` | — |
| **S3** | Safe area | `SessionSafeAreaPx(Viewport, ToolbarHeight)`, fed into `PieceMenuHomeOffset` + `ClampPanelOffset` | `Presenter.SessionSafeArea` — a panel dragged to the bottom-right never overlaps the strip; non-finite/negative height is treated as zero; the existing clamps are unchanged by composition | — |
| **S4** | **The strip on screen** | `ADestructionGamePlayerController::BuildSessionToolbarPanel() -> TSharedRef<SWidget>`, `GetSessionToolbarState()`, `OnToolbarButton(EToolbarButtonId)` | `World.Session.ToolbarDrivesTheSession` — headless: clicking through `OnToolbarButton` moves the state *and* pushes it onto `UBuildModeComponent` via `SetPieceKind` / `SetCourse` / `PlacementMode` ONLY (the component derives material, extent and `BuildPlaneZCm` itself; **grounded is never pushed** — it is derived from the snapped pose inside the subsystem, DESIGN §8 2026-09-15, and `IsCourseGrounded` is only the readout's intent) | Build tab lit with its accent bar, Brick lit, Snap lit, `Course 0`, `Clear build` visibly greyed |
| **S5** | Mode switch is total | `OnToolbarButton(ModeDestroy)` hides the ghost, `Destroy` disables placement; and back | `World.Session.ModeSwitchIsTotal` — no ghost survives into Destroy, the build settings survive the round trip (the model already promises this) | one shot per mode, same camera |
| **S5b** | Arming `Clear build` | `FSessionToolbarState::ClearArmedAtSeconds` + a transition | `Core.SessionToolbar.ClearArms` — first click arms and changes the caption, second within the window clears, a click elsewhere disarms | armed state, red caption |
| **S6** | **Cursor + RMB look** | `SetSessionControls()`; `IA_LookModifier` asset + chorded trigger; `.IsFocusable(false)` everywhere | `Content.RequiredContent` gains the new action; a mapping-shape assertion | cursor visible over the toolbar, the wall not spinning, and **`W` still flies after a button click** (the human check) |
| **S7** | Identity line | `FPieceMenuInspector::IdentityText` — material name by library-row identity, size from the box, mass from `GetPiece` | `Presenter.PieceIdentityText` — a clay brick reads exactly `Clay brick · 21.5 × 10.25 × 6.5 cm · 2.9 kg`; an unlisted material reads `Unknown material`; a ref naming nothing reads empty | panel with the line under the inspected label |
| **S8** | Hover peek | `HoverAlongRay` also builds a `Compact` inspector when nothing is selected | `World.Session.HoverPeek` — peek appears on hover, is suppressed by a real selection, and changes nothing about the selection | cursor on an un-selected brick, compact panel up |
| **S9** | **Ghost card** | `EPieceMenuDetail::GhostCard`, `FBuildPreviewInspector BuildBuildPreviewInspector(...)`; needs CURRENT_STATE (b2) `bWouldPlace` and (v-a) `bRequestedPoseOccupied` first | `Presenter.BuildPreviewInspector` — a kind table → words; free-in-open-space vs every-snap-occupied are different sentences; `KindText` summarises the *joints* rather than the ambiguous merged `Kind` | ghost in the world and a card that agrees with it, joint for joint |
| **S10** | Removal forecast (model) | `FRemovalForecast ForecastRemoval(const FStructure&, int32 PieceIndex, int32 PieceCap)` — copy, remove, `SolveAndBreak` on the copy | `Core.RemovalForecast` — a corbel keystone drops N and the live structure is **bit-identical afterwards**; an isolated grounded pad drops 0; above the cap it refuses rather than running | — |
| **S11** | Removal forecast (panel) | `ForecastText` + the `X` toggle + the 200 ms hover settle | `Presenter.ForecastText` — four states (off / nothing falls / N fall / too large) are four distinct sentences | the block populated on a load-bearing brick |
| **S12** | **Load overlay** | 3 × `EBrickHighlight`, 3 materials, 3 `RequiredContent.h` rows + swatch constants, `RefreshLoadOverlay()`, precedence below `Hovered` | `World.Session.LoadOverlayPrecedence` — an overlaid brick that is hovered still draws `Hovered`; the band per piece is its worst joint's band; toggling off restores `None` | a wall tinted green through amber to red at its base — the money shot |
| **S13** | Joint lines | `DrawDebugLine` between the inspected piece and each neighbour, coloured through `BrickNeighbourSwatchColours` | `World.Session.JointLineColours` — line *i*'s colour is swatch *i*, which is the far brick's overlay slot | a brick with six coloured lines matching six coloured neighbours |
| **S14** | Status readout | `FSessionStatusReadout` | `Presenter.SessionStatus` — every figure, including `not solved` before any solve | the strip's right end |
| **S15** | `F1` hides the UI; narrow-width layout | a `bool` + `FToolbarLayout(WidthPx)` | `Core.SessionToolbar.LayoutDropsCaptionsBeforeButtons` | 1280 and 1000 side by side; a clean plate with `F1` down |

**Ordering rationale.** S0–S6 is the *frame*: after S6 there is a playable level with a working
toolbar, a cursor, and a camera — the thing the owner asked for, end to end, before any readout work.
S7–S9 is *"see details about them"*, the second half of the ask. S10–S13 are the settings that make
Destroy mode worth having a settings group at all, and S12 is the one to reach for first if the
budget runs short: the load overlay is what makes this game's physics visible without reading a
single number.

**Prerequisites owed from CURRENT_STATE** before S9 can be honest: `bWouldPlace` (b2),
`bRequestedPoseOccupied` (v-a), and a truthful merged-candidate `Kind` (vii). Before S12 is
trustworthy on a big wall, the snap solver's NaN guard (i) is worth closing, because a NaN pose
becomes a NaN utilisation and the overlay would paint it whatever `FMath::Max` happens to return.

**Deferred, log in CURRENT_STATE when this design is accepted:**
- the settle-policy change that would make an "auto-run / hold" toggle honest (§b);
- folding `IA_HoverPiece` back into `IA_MouseLook` once nothing removes the context (§d);
- a captioned detail toggle to replace the double-click gesture on the grab strip — the existing
  header already logs that it needs a model field and a red test.
