# The warehouse — a large real-brick building to play with

**Owner ask (2026-09-18):** "I want to make a realistic large building. That can test our current
work. Please create a large building so that I can play with it ... find an efficient way to create
the building inside a level ... It is a 3d building, make sure you make it 3d." The reference image
is a Minecraft brick warehouse: a long two-storey block of red brick, pilasters between tall
windows on the long sides, a stone plinth, a stone string course between the floors, a stone
cornice under the eaves, a stepped dark roof on gable ends, a tall doorway in the near gable end,
and two tall chimneys with stone caps at that same end.

**Standing rulings for this piece of work, from the owner, verbatim:** "Do not work on the solver
or anything like that. Just build it." and "If it falls, that is ok, I just want it built." So:
this document specifies GEOMETRY. No solver change, no profile retune, no strength constant moves
to make it stand. What the router says about it as laid is MEASURED and recorded, never asserted
as a verdict, and never chased.

## It is DATA, not code (owner ruling, 2026-09-18: "Pivot to data driven")

The owner's second ask, after seeing a C++ builder: "is there a better way to do it where you
don't have to write c++ code? ... users will be able to build on a level in the game at some point
and that won't need c++ code." So the building is a **layout file**, `Content/Layouts/Warehouse.json`
(format in `Core/LayoutFile.h`: a JSON list of boxes and materials; joints are swept on load by
`Core/ContactSweep.h`, the same rule the interactive build uses), and the catalogue row just loads
it. **`Scripts/New-WarehouseLayout.ps1` writes that file from the arithmetic below** and refuses to
write if any two boxes overlap. Change a number in the script, re-run it, and the level changes with
no build. The C++ builder that existed for an hour was deleted; nothing below depends on it.

The same loader is the door a player's saved build will come back through — the saver from the
build session is the owed half (CURRENT_STATE).

## What "efficient" means here

Nothing is authored in a map. Like every other level, the map is an untouched duplicate of
`Lvl_Sandbox` and the row's `LayStructure` lays the building — here by reading the file — so
"efficient" is a GENERATOR with a small vocabulary that produces thousands of pieces from a few
dozen numbers:

1. one **course-filling rule** that lays every wall, band, gable and jamb (below);
2. a wall is a **run** plus a list of **openings** (a gap, its lintel band, its sill band);
3. the joints are **swept, not authored**: offer every pair to `MakeInterface` and keep what it
   accepts, with the profile chosen by `BuildMode::JointForContact` (the boxed overload) — the
   helper that exists precisely so programmatic builders and the interactive build share one
   judgement. The realistic shed hand-rolled this rule; the warehouse uses the helper.

`DestructionShed3D::BuildRealistic` is the template for every convention below (the coordinating
grid, the sweep, `SetThreeDimensional`); the generator script generalises its `LayWall` into the
one fill rule.

## The grid

Real UK metric brick, exactly as the realistic shed: 21.5 × 10.25 × 6.5 cm on 1 cm joints.

| name | cm |
|---|---|
| `PitchCm` (stretcher pitch along a run) | 22.5 |
| `HalfPitchCm` | 11.25 |
| `CoursePitchCm` | 7.5 |
| `BrickLenCm` / `WytheCm` / `CourseCm` / `JointCm` | 21.5 / 10.25 / 6.5 / 1 |
| `HalfBatLenCm` | 10.25 |

**Everything along a run is addressed in HALF-PITCH GRID UNITS** from the run's start `S`: grid
unit `u` is the coordinate `S + u × 11.25`. Wall ends, opening edges, gable steps, pilaster and
chimney positions are all whole grid units. That one constraint is what makes the fill rule below
produce nothing but full bricks and exact half bats.

Course `c` occupies `Z ∈ [c × 7.5, c × 7.5 + 6.5]`. Course 0 is grounded.

## The course-filling rule

To fill a **segment** `[ga, gb)` (grid units, `gb > ga`) of course `c` with pieces of length
`L` cells (a brick is `L = 2` units = 21.5 cm + joint; a stone block is `L = 4` units = 44 cm +
joint), on parity `p = c mod 2`:

- cell `k` (for `k = -1, 0, 1, …`) covers grid `[k·L + p, (k+1)·L + p)`; as material it spans
  `[S + (k·L + p)·11.25, S + ((k+1)·L + p)·11.25 − 1]`;
- clip that material span to `[S + ga·11.25, S + gb·11.25 − 1]`;
- lay it if what is left is at least `HalfBatLenCm − 1e-6` long; otherwise skip it.

With `L = 2` this reproduces the realistic shed's walls exactly (an odd course is a half bat, full
bricks, a half bat) and gives every opening a FLUSH jamb of alternating full brick / half bat (a
closer), rather than the shed's toothed jamb. With `L = 4` the end residuals are 10.25, 21.5 or
32.75 cm blocks, all kept.

A course of a wall is: its run `[0, N)` minus every opening whose gap course range contains `c`
(each gap `[glo, ghi)` splits the run into segments), and minus every band (lintel band, sill band)
whose course range contains `c`, which likewise clears its footprint. Whatever segments remain are
filled by the rule with the course's material (brick, or stone on a stone course).

## The building, in numbers

Axes as the shed: X is the long axis, Y is depth, Z is up. Walls are one wythe (10.25) thick.

| | value |
|---|---|
| `LongWallPitches` (N_x) | 29 |
| `EndWallPitches` (N_y) | 14 |
| `EavesCourses` | 64 (courses 0..63; eaves top Z = 479) |
| Long-wall run | `S = 0`, 58 grid units, X ∈ [0, 651.5] |
| Front long wall | Y ∈ [0, 10.25] |
| Back long wall | Y ∈ [326.25, 336.5] (`BackY0 = 11.25 + 14 × 22.5`) |
| End-wall run | `S_e = 11.25`, 28 grid units, Y ∈ [11.25, 325.25] |
| Left end wall (back end) | X ∈ [0, 10.25] |
| Right end wall (door end) | X ∈ [641.25, 651.5] |
| Outer box | 651.5 × 336.5 × 479 to the eaves |

The end walls fit BETWEEN the long walls exactly as the shed's side walls do, starting one bond
offset in, so each corner closes on a genuine 1 cm Y-normal joint out of the X-Z plane.

### Courses (the same on every wall)

| courses | what | material | notes |
|---|---|---|---|
| 0–1 | plinth | StructuralConcrete | one 2-course-tall block per `L = 4` cell (Z ∈ [0, 14]); grounded; flush |
| 2–6 | wall | ClayBrick | |
| 7 | ground-floor sills | StructuralConcrete | only inside a sill band, see openings |
| 8–27 | ground-floor window gaps | | 20 courses = 150 cm clear |
| 28–29 | ground-floor lintels | Timber | 2-course-tall board, see openings |
| 33 | string course | StructuralConcrete | every wall, whole run, `L = 4`, flush |
| 37 | upper sills | StructuralConcrete | |
| 38–53 | upper window gaps | | 16 courses = 120 cm clear |
| 54–55 | upper lintels | Timber | |
| 62–63 | cornice | StructuralConcrete | one 2-course block per `L = 4` cell (Z ∈ [465, 479]); PROJECTS 5 cm outward on the LONG walls only (front Y ∈ [−5, 10.25], back Y ∈ [326.25, 341.5]); flush on the end walls |

Every other course below the eaves is brick. Brick and stone courses alike use the fill rule with
the wall's parity.

### Openings

An opening is a gap `[glo, ghi)` over courses `[clo, chi]`, a lintel band (Timber board) over
courses `[chi+1, chi+2]` spanning `[glo−2, ghi+2)` (one pitch of bearing each side, the masonry
cleared to the same footprint), and, for a window, a sill band (StructuralConcrete, 1 course at
`clo−1`) spanning `[glo−1, ghi+1)`. The lintel and sill are single pieces of the wall's wythe.

**Long walls (both identical), grid units along X:** pilasters at `[3,5) [13,15) [23,25) [33,35)
[43,45) [53,55)`; bays between them; a window centred in each bay at
`[7,11) [17,21) [27,31) [37,41) [47,51)` on BOTH floors (ground: courses 8–27, lintel 28–29, sill 7;
upper: 38–53, lintel 54–55, sill 37). Five bays, ten windows per long wall.

**Back end wall (X ∈ [0, 10.25]), grid units along Y:** windows `[6,10)` and `[18,22)` on both
floors, same courses as above.

**Door end wall (X ∈ [641.25, 651.5]):** the DOOR: gap `[8,20)` (clear 136 cm) over courses 0–29
(clear 225 cm), Timber lintel courses 30–31 spanning `[6,22)`; no sill. Upper floor: windows
`[6,10)` and `[18,22)`, courses 38–53, sill 37, lintel 54–55.

### Pilasters (12)

On the long walls only, one per pilaster slot above: a single-brick STACK-BOND column, one
stretcher per course laid along X at `X ∈ [S + g·11.25, S + g·11.25 + 21.5]` for
`g ∈ {3, 13, 23, 33, 43, 53}`, projecting one wythe outward: front `Y ∈ [−11.25, −1]`, back
`Y ∈ [337.5, 347.75]`. Courses 0..61 (top Z = 464, so the cornice block above beds on it across
the 1 cm joint). Course 0 grounded. ClayBrick.

### Gables (both end walls)

Courses 64..76 continue the end walls above the eaves. Gable course `g = 0..12` (course `64+g`)
fills grid `[g+1, 28−(g+1))` — narrowing HALF A PITCH per side per course (a ~34° pitch), down to
the apex `[13,15)` on course 76 (top Z = 576.5). Brick, free (not grounded), the same fill rule
and parity as the wall below, so every course is symmetric about the run's material centre
(Y = 168.25) and beds fully on the one beneath. Course 64 spans Y ∈ [22.5, 314]. Course 76 is an
EVEN course, so the rule gives the apex as TWO HALF BATS, Y ∈ [157.5, 167.75] and [168.75, 179],
bedding on course 75's centre brick (Y ∈ [157.5, 179]) and its two flanking half bats.

### Roof (27 Timber boards, 5 cm thick, each spanning the FULL length X ∈ [0, 651.5])

- two EAVES boards on the long-wall tops (bearing on the cornice stone, and on the end walls'
  eaves course): `Y ∈ [0, 21.5]` and `Y ∈ [315, 336.5]`, `Z ∈ [480, 485]`;
- twelve STEP boards per side, `g = 0..11`, on the shoulder each gable course leaves exposed:
  `Y ∈ [S_e + (g+1)·11.25, S_e + (g+2)·11.25 − 1]` and its mirror about Y = 168.25,
  `Y ∈ [326.25 − (g+2)·11.25, 326.25 − (g+1)·11.25 − 1]` (g = 0: `[22.5, 32.75]` and
  `[303.75, 314]`), `Z ∈ [487.5 + 7.5g, 492.5 + 7.5g]`;
- one RIDGE board over the two apex half bats: `Y ∈ [157.5, 179]`, `Z ∈ [577.5, 582.5]`.

A board bears on both gable ends through the sweep's 1 cm bed; the next gable course up stands
1 cm from its long edge, which the sweep reads as a harmless Y-normal dry contact.

### Chimneys (2) with caps

Two stacks OUTSIDE the door-end wall, against its outer face, rising from the ground:
`X ∈ [652.5, 674]`, `Y ∈ [33.75, 55.25]` and `Y ∈ [281.25, 302.75]` (grid `[2,4)` and `[24,26)`
of the end run, clear of the door and the upper windows), courses 0..89 (top Z = 674, 92 cm above
the ridge board). Even courses: two stretchers side by side along X (Y halves `[y, y+10.25]`,
`[y+11.25, y+21.5]`); odd courses: two along Y (X halves `[652.5, 662.75]`, `[663.75, 674]`) — so
the stack is bonded. Course 0 grounded. ClayBrick. The stack's inner face stands 1 cm off the
wall's outer face, so the sweep ties it to every wall brick it overlaps.

Cap: one StructuralConcrete block on course 90, overhanging 5.625 cm on every side:
`X ∈ [646.875, 679.625]`, `Y ∈ [y − 5.625, y + 27.125]`, `Z ∈ [675, 681.5]`.

### Envelope

`X ∈ [0, 679.625]` (chimney caps), `Y ∈ [−11.25, 347.75]` (the pilasters, which project past
the cornice), `Z ∈ [0, 681.5]`.

A scratch simulation of this exact specification (2026-09-18, the fill rule and every list above,
with an exhaustive AABB overlap check) lays **5,612 pieces: 5,397 ClayBrick, 161
StructuralConcrete, 54 Timber (27 lintels + 27 roof boards); 57 grounded; zero overlapping
pairs.** Those are the numbers the builder should reproduce. About twice `corbel-f-100`'s 3,015 —
the largest existing level — and thirteen times the realistic shed.

## Joints

After every piece is laid, sweep every pair once through `MakeInterface` (`JointThicknessCm = 1`),
the LOWER piece named A so a bed normal reads as a bed beneath the piece it carries, and take the
profile from `BuildMode::JointForContact(MaterialA, MaterialB, normal, BoxA, BoxB)` — the five-
argument overload, so a quoin is full mortar, a head joint is the perpend, and a contact touching
Timber is a dry bearing. No joint is authored by hand: the warehouse has no screwed cleat.

The naive sweep is ~17 M pair offers at this size. Bucket it (by course band, say) if a Development
build of `Build` takes more than about two seconds; correctness first.

Then `SetThreeDimensional(true)`. Above the 200-block cap the router is the break authority, as
for every large level; the flag still selects the genuinely-3D geometry every reader sees.

## The row

`DestructionScenarios::Catalogue()` gains a row: `Name = "warehouse"`, `MapName = "Lvl_Warehouse"`,
`LayStructure` → `DestructionLayoutFile::LoadFile(ContentPath("Warehouse"), Out)`, `Framing = ThreeQuarter`,
NO cut, the default hold. Its title and expectation say what it is and — honestly — that nothing
is cut and what it does when it settles is whatever the router decides. The map
`Content/Maps/Scenarios/Lvl_Warehouse.umap` is made by `Scripts/New-ScenarioMap.ps1` (done
2026-09-18). `Content.ScenarioMapsExist` / `ScenarioMapsAreDistinctAssets` cover the content
step; the row sweeps in `DestructionScenariosTest` cover Build succeeding.

The level opens in Destroy with the session strip up (every laid row does), so the owner can walk
it, hover pieces, pull bricks and Run — the "play with it".

## What to measure, not assert

A probe test outside the default suite's name filter (name it `WarehouseProbe.*`, like
`ShedRealisticLatency.*`) that builds the layout, times `Build`, times one production
`SolveAndBreak` (the router), and logs pieces / joints / passes / stranded / lost-earth as laid. It
exists to be read. Whatever it says is recorded in CURRENT_STATE as the warehouse's as-laid
reading, and the owner's ruling stands: if it falls, it falls.
