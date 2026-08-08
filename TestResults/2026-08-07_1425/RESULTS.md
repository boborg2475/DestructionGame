# The corbel family, photographed: before the cascade and after it settles

**Captured:** 2026-08-07 14:25 local
**Engine:** Unreal Engine 5.8, `DestructionGameEditor` Win64 Development
**Working tree:** `main` at `69ba662` plus the uncommitted composite-depth work. **Nothing was committed.**

This report is evidence, not advocacy. Where a picture does not show what its caption would like it
to show, the caption says so instead. Where a number in the brief disagrees with what was measured
today, the measurement wins and the disagreement is written down.

---

## 1. What had to be solved before a single picture could be taken

**Tests E to J are world-free.** They are arithmetic over `FStructure` and `Layout` — no actors, no
RHI, no viewport — which is exactly why the whole solver suite runs in about a second, and exactly
why none of them can produce an image as written.

`Tests/StaircaseScreenshotTest.cpp` is the project's only rendering harness, and every constant in
it is the 36-brick staircase cut into the game mode's own wall. Its **plumbing** was reused rather
than reinvented — the `Shot showui` exec routed through the *game viewport client*, the latent
command timings, the delete-the-file-first rule, the PNG signature and IHDR read by hand — and a new
file, `Source/DestructionGame/Tests/CorbelScreenshotTest.cpp`, supplies the one thing that could not
be reused: a builder that takes any `FStructure` plus its boxes, stands it up in the world as
`ABrickActor`s, photographs it, settles it, and photographs it again.

Three things about that harness are worth stating plainly, because each was a decision:

- **No production code was written or changed.** The whole file is test code under
  `Tests/`, guarded by `#if WITH_DEV_AUTOMATION_TESTS`. The brick spawn recipe is duplicated from
  `World/DestructionStructureSubsystem.cpp` (where it is file-local, with no header to reach it
  through) rather than exposing it, because exposing it would be production code with no failing
  test behind it.

- **The corbels are built 15 m down the Y axis from the scenario wall, and the offset is applied to
  the SPAWN only.** The `FStructure` being photographed is bit-identical to the one
  `Core.Structure.*` solves — a constant translation along an axis gravity does not act on changes
  no mass, no interface, no lever arm and no reading. The alternative, destroying the game mode's
  1,220-brick wall to clear the stage, was implemented first and then **backed out**: automation
  tests share one process and one world, so it would have left `Visual.StaircaseScreenshot` looking
  at a structure whose every actor is null, failing for a reason nothing in its own file mentions.

- **The camera is computed, not named.** These eight structures span from a 4-brick arm 89 cm wide
  to an 11.25 m one; a standoff that frames any of them frames none of the others. The harness takes
  the bounding box and inverts the 90° horizontal FOV for it. The camera also looks along **-Y**
  rather than the staircase harness's +Y, so that increasing X is drawn to the **right** and the
  photographs read the same way round as `claude_plans/CORBEL_CASES.html` instead of mirrored.

### The two traps, both avoided, both real

- **`Shot showui`, never `HighResShot`.** `HighResShot` renders scene-only through an
  `FDummyViewport`/`FCanvas` and `ProcessScreenShots` only takes the Slate path when
  `ShouldShowUI()` is set, which `HandleHighresScreenshotCommand` never sets.
- **`EAutomationTestFlags::NonNullRHI` is carried.** Without it the documented headless suite runs
  the test, finds no viewport, writes no file, and goes green. With it, the documented headless
  suite never mentions the test exists.

**So it was run explicitly, and this is the command that was run:**

```
UnrealEditor-Cmd.exe DestructionGame.uproject /Game/Maps/Lvl_Sandbox
  -game -windowed -ResX=1920 -ResY=1080 -ForceRes -RenderOffScreen
  -nosplash -NoSound -unattended -nopause -log
  -abslog="Saved/Logs/Visual_2026-08-07_final.log"
  -ExecCmds="Automation RunTests DestructionGame.Visual"
  -TestExit="Automation Test Queue Empty"
```

`RunTests DestructionGame.Visual` — the whole filter, not just the new test. **All three visual
tests passed in one process:**

| Visual test | Result |
|---|---|
| `DestructionGame.Visual.CorbelScreenshots` | **Success** |
| `DestructionGame.Visual.PieceMenuScreenshot` | **Success** |
| `DestructionGame.Visual.StaircaseScreenshot` | **Success** |

That the third one still passes is the evidence that the Y offset above did its job.

---

## 2. The images

All sixteen are 1920 x 1080 PNGs from the real renderer. **Every one was opened and looked at**
before it was described below; nothing here is a caption written from a log line.

| File | Bytes | What it actually shows |
|---|---:|---|
| `Corbel_A_BareArm4_Before.png` | 2,281,641 | Case A as laid. A three-course, two-cell grounded pedestal at bottom left; four **single** bricks stepping up and to the right, each overhanging the one below by half its length. The top brick is 45 cm out over open air on a 10.25 cm bearing. |
| `Corbel_A_BareArm4_After.png` | 2,192,240 | **Identical.** Nothing released, nothing moved: 0.000 cm. |
| `Corbel_B_Filled4_Before.png` | 2,344,408 | Case B: the same four-step profile with every course filled inboard. Reads as a squat mushroom — a narrow pedestal under a cap that widens with each course. |
| `Corbel_B_Filled4_After.png` | 2,208,396 | **Identical.** 0 of 18 released, 0.000 cm. |
| `Corbel_C_Filled10_Before.png` | 2,927,068 | Case C: ten filled steps on the bare two-cell base. The raking underside is now unmistakable — a staircase of bed joints running up to the right — and the alternating half-cell bond shows as a saw-tooth on the left edge. |
| `Corbel_C_Filled10_After.png` | 2,374,680 | **Identical.** 0 of 51 released. |
| `Corbel_D_Filled10Counterweight_Before.png` | 2,594,634 | Case D: case C with three cells of masonry opposite. The left two-thirds of the frame is now solid wall to the ground; the corbel arm on the right is **the same arm, in the same place, at the same reach**. |
| `Corbel_D_Filled10Counterweight_After.png` | 2,297,696 | **Identical.** 0 of 90 released. See §4 — this identity is the finding. |
| `Corbel_E35_JustUnder_Before.png` | 2,614,822 | 35 steps: a 3.9 m triangular corbel, 496 bricks, filling the frame. The raking soffit is a clean 33.7° line from the base to the tip. |
| `Corbel_E35_JustUnder_After.png` | 2,470,536 | **Identical.** 0 of 496 released, 0.000 cm, at a root reading of **0.99046**. This is the last corbel the model says stands, and it stands by 1%. |
| `Corbel_E36_JustOver_Before.png` | 2,510,191 | 36 steps: visually indistinguishable from E35 at a glance — one more course, 23 more bricks. |
| `Corbel_E36_JustOver_After.png` | 2,550,795 | **The tip.** The lowest raking courses at the root have gone and lie as a long line of tumbled bricks along the ground; the soffit line now starts one step higher and the bulk of the triangle — 483 of 519 pieces — is still standing. 36 released, 2 breaking passes, root joint broke in pass 1, worst survivor moved 281 cm. |
| `Corbel_F_HundredSteps_Before.png` | 2,791,776 | 100 steps: 3,015 bricks, 11.25 m of overhang, 7.7 m tall. **It is legible in one frame** — see §5. |
| `Corbel_F_HundredSteps_After.png` | 2,549,025 | **Total collapse.** A rubble field 15 m wide covering the bottom third of the frame, with one small fragment of the grounded base surviving at the far left. 2,981 of 3,015 released, 10 breaking passes, worst survivor moved 820 cm. |
| `Corbel_J_FreeEnd40_Before.png` | 2,590,407 | Case J: the flush 7 x 40 running-bond wall, 300 bricks, 3 m tall, intact. |
| `Corbel_J_FreeEnd40_After.png` | 2,450,644 | **The user's reported case, fixed.** The outermost brick of the ground course has been deleted and the half bat that sat entirely on it has toppled onto the floor beside the wall. **Nothing else moved.** The other 298 bricks are exactly where they were laid. |

### The numbers beside the pictures

Every row measured in the same run that took the photographs. Projection is the arm's tip past the
base's outer face; the multiple is against the **10.25 cm total projection** published corbelling
practice allows for this 21.5 x 10.25 x 6.5 cm brick.

| Case | Pieces | Joints | Root reads | e = \|M\|/\|F\| | Credited depth | Projection | vs. code | Released | Worst movement |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **A** bare arm, 4 | 10 | 10 | **0.15613** | 22.50 cm | 30 cm | 45 cm | 4.4x | 0 | 0.000 cm |
| **B** filled, 4 | 18 | 35 | 0.19516 | 15.79 cm | 30 cm | 45 cm | 4.4x | 0 | 0.000 cm |
| **C** filled, 10 | 51 | 119 | 0.34481 | 37.31 cm | 75 cm | 112.5 cm | 11.0x | 0 | 0.000 cm |
| **D** filled, 10 + counterweight | 90 | 230 | 0.34348 | 38.07 cm | 75 cm | 112.5 cm | 11.0x | 0 | 0.000 cm |
| **E35** | 496 | 1,386 | 0.99046 | 131.26 cm | 262.5 cm | 393.75 cm | 38.4x | 0 | 0.000 cm |
| **E36** | 519 | 1,452 | **1.01647** | 134.98 cm | 270 cm | 405 cm | 39.5x | 36 | 281.19 cm |
| **F** | 3,015 | 8,780 | **2.68132** | 374.32 cm | 750 cm | 1,125 cm | 109.8x | 2,981 | 820.23 cm |
| **J** free end, 40 courses | 300 | 806 | **0.67403** | 11.24 cm | 38.93 cm | — | — | 1 | 7.99 cm |

**A reads 0.15613 exactly as `COMPOSITE_DEPTH_DESIGN.md` derived it by hand. F reads 2.68132 and
loses 2,981 of its 3,000 arm pieces. J reads 0.67403 at forty courses and stands.** All three match
the brief to every digit given.

---

## 3. Case A is the one to look at hardest, and it is the least convincing picture here

The A pair is two identical frames of four bricks stepping out over nothing, and a reader glancing
at them would say "that looks about right, brickwork does that". It is worth saying what the model
is actually claiming there.

The root joint's bearing is 10.25 cm wide, so its outer edge is **5.125 cm** from its own centroid.
The resultant of everything above it acts **22.5 cm** out — 17.375 cm *beyond the bearing*. **There
is no compressive stress block on that joint that balances, at all.** No arrangement of contact
pressure on a 10.25 cm patch puts its resultant 22.5 cm outside it. Statics says this arrangement of
four bricks cannot stand; the model returns a confident 0.15613 and it stands.

`COMPOSITE_DEPTH_DESIGN.md` works this through and calls it out: **a bonded corbel standing is the
model declining to enforce statical equilibrium at the bearing.** That is defensible — a real corbel
is held by the bond into the masonry behind it, not by the bottom unit's own bearing — but it is a
choice, and the A pair is a photograph of that choice rather than a photograph of a validated
result. The picture cannot show you that. This paragraph is the only thing that can.

**And A vs. B is a genuinely useful pair.** They have the same base, the same four steps and the
same 45 cm projection. Filling the courses raises the reading from 0.15613 to 0.19516 — 25% worse,
not better — because the fill adds mass outboard of the root faster than it adds anything the root
can use. The bare arm is the *lighter* structure and the filled one is the *more heavily loaded*
one, and the two frames show exactly why.

---

## 4. THE COUNTERWEIGHT FINDING — and yes, the C and D frames looking the same IS the finding

**Case C and case D cross 1.0 at the SAME step count: 36.**

| | 35 steps | 36 steps | Crossover |
|---|---:|---:|---:|
| **C**, bare two-cell base | 0.99258 | 1.01871 | **36** |
| **D**, three cells of masonry opposite | 0.99046 | 1.01647 | **36** |

Three whole cells of masonry standing behind the root joint — 39 extra bricks at ten steps, 23 at
every step beyond — **buy the corbel exactly nothing.** Not one more course. D's reading is in fact a
hair *lower* than C's (0.34348 against 0.34481 at ten steps), and that difference comes from the
lever arm shifting, not from the counterweight bearing on anything.

**Read `Corbel_C_Filled10_*.png` and `Corbel_D_Filled10Counterweight_*.png` side by side.** The two
structures are visibly different — D has a block of solid masonry to the ground where C has open
air — and the corbel arms in the two frames are *the same arm doing the same thing*. That is not a
framing coincidence. It is the result.

**Why.** The counterweight carries its own weight straight down its own columns to the ground. None
of it passes through the corbel's root joint, so the joint sees the same axial compression, the same
section, and returns the same number. This is the same downward-only routing defect the free end and
the jamb reveal already fail on — its third face.

**This is what makes `Core.Structure.CorbelStepsBeforeTensionWins` red**, and the red row is:

```
MASONRY OPPOSITE MUST BUY THE CORBEL SOMETHING — case D crosses at 36 steps and case C at 36,
and D's must be STRICTLY LATER. If they are equal the counterweight is carrying its own weight
to the ground without ever reaching the root joint.
```

**That row is a finding, not a wrong expectation.** It was written as a strict inequality on
integers precisely so it could not pass by a tolerance, it was predicted to fail before it was run,
and it failed for the predicted reason. It should stay red until the routing is fixed.

---

## 5. Case F, and whether one frame can honestly show 3,015 bricks

**It can, and better than expected.** `Corbel_F_HundredSteps_Before.png` frames 17.1 x 9.6 m at an
857 cm standoff. Each brick is about 24 px long and 8 px tall — small, but the running bond texture
resolves, the raking soffit is a clean unbroken diagonal from the base to the tip, and the
stepped-in left face of the base column is visible. **Nothing about it reads as a smear.** No second
framing was needed and none was produced.

The after frame is unambiguous: a rubble field roughly 15 m wide, individual bricks distinguishable,
one small remnant of grounded base at the far left. The structure is gone.

**What the F pair cannot show, and is worth saying:** the *mechanism*. 2,981 pieces losing their
path to the ground over 10 breaking passes is a graph event, and what you see three seconds later is
a pile. If the question is "does the corbel come down", the pair answers it completely. If the
question is "how does it come down", it does not — a video would, and this harness does not make
videos.

---

## 6. The calibration curve, which is what this family was actually built to produce

The question behind all of this is not any single verdict but **how permissive the model is against
published practice.** For this brick, US model codes and UK practice give (see
`claude_plans/REAL_WORLD_CHECK.md`):

```
per course  = min( bed depth / 3 , unit height / 2 ) = min(3.417, 3.25) = 3.25 cm
total       = wall thickness                                            = 10.25 cm
```

**Measured, at the half-cell step every fixture in this project uses (11.25 cm, 3.46x the per-course
limit):** the crossover is at **36 steps** — a **405 cm projection, 39.5x the 10.25 cm total-projection
limit.** That is the one point on the curve that was *measured* rather than extrapolated, and it is
the E36 photograph.

**Extrapolated across the step ladder**, from the measured slope at 20 steps (test I):

| Step per course | vs. 3.25 cm limit | Crosses at | Projection there | vs. 10.25 cm limit |
|---:|---:|---:|---:|---:|
| 3.25 cm | 1.00x | ~320 steps | 1,041 cm | **101.5x** |
| 5.375 cm | 1.65x | ~132 steps | 710 cm | 69.3x |
| 7.5 cm | 2.31x | ~72 steps | 540 cm | 52.6x |
| 11.25 cm | 3.46x | ~33 steps | 373 cm | 36.4x |
| 16.125 cm | 4.96x | ~16 steps | 266 cm | **25.9x** |

**So the model tolerates roughly 26x to 102x the published total-projection limit** before the root
joint crosses 1.0, depending on how steeply the corbel rakes.

**This disagrees with the brief, which said 23–53x, and the disagreement is reported rather than
smoothed.** The measured span today is 25.9x to 101.5x. The top of the range is the one that moved:
the shallowest, code-compliant 3.25 cm step now extrapolates to 101.5x rather than to something in
the fifties. That row is also the one test I flags as anomalous — see §7 — so it is the least
trustworthy number in the table, and the honest summary is: **at the steep end of the ladder the
model is about 26x permissive, at the half-cell bond the project actually uses it is 36–40x, and at
the shallow end the extrapolation is unreliable and reads 100x.**

**Every one of those numbers is far past where an engineer stops guaranteeing anything, and that was
ruled deliberately.** These are design limits with safety factors for masonry that must never fail,
not collapse predictions, and this is a destruction game where the interesting behaviour lives past
the guarantee. The number that matters is not whether 36x is "too much" — it is that the model has a
crossover at all, that it moves the right way with step size, and that it is now measured.

---

## 7. What is still red, and why

**138 tests performed. 134 passed. 4 failed.** Run headless with `-nullrhi` as documented; results
read from `Saved/Logs/Suite_2026-08-07_final.log` via `LogAutomationController`.

| Failing test | Status |
|---|---|
| `DestructionGame.Acceptance.Wall.Catalogue` | **Pre-existing.** Catalogue cases disagree with their agreed verdicts. Not touched by this work. |
| `DestructionGame.Acceptance.Wall.MatchedPairs` | **Pre-existing.** Not touched by this work. |
| `DestructionGame.Acceptance.Wall.StackBondColumnShearIsHeightIndependent` | **Pre-existing.** Not touched by this work. |
| `DestructionGame.Core.Structure.CorbelStepsBeforeTensionWins` | **The counterweight defect of §4.** A finding, not a wrong expectation. |

**None of the four is caused by the work in this report.** The new visual test added nothing to the
headless count — it carries `NonNullRHI` and the headless filter excludes it, which is the whole
point of §1.

### Two further things the suite says out loud and this report will not bury

- **Test I's ordering row is red inside a passing test**, in the sense that the file documents a
  U-shaped ladder: readings at ten steps go 0.0466, 0.0991, 0.1687, 0.3448, 0.6670 across the step
  sizes — monotone as designed — but the file records that at the two smallest steps it is the
  **arm** that caps the credited depth rather than the wall, at which point the whole reading
  collapses to `K*F/e` and the ordering stops being the moment's. **The `lambda*e` depth cap
  proposed to fix one case breaks the ordering property the design calls the only genuinely emergent
  one here.** That is a ruling to be made, not a tuning, and it is why the 3.25 cm row of §6's table
  should be read with suspicion.

- **The free-end ladder crosses capacity at about 59 courses.** J stands at 40 (0.674) and at 50
  (0.847), and at 60 courses it reads 1.020 and loses 49 pieces. The game builds 40. **The margin on
  the user's own ruling is about 19 courses, not a factor of anything comfortable.**

---

## 8. What was rendered, what was skipped, and why

**Rendered: A, B, C, D, E (as a pair either side of the crossover), F, J.** Eight structures,
sixteen frames.

**E was rendered as 35 and 36 rather than 36 alone** because the whole point of that row is *where
it tips*, and one frame of a tipped corbel cannot show a tipping point. The pair costs one extra
build and it is the most informative thing in the folder: two structures one course apart, one
standing at 0.99046 and one shedding its root at 1.01647.

**Skipped: G, H and I. Deliberately.**

- **G** is a claim about a **ratio** between three scales — a corbel at half and double size must
  read half and double. A photograph of a corbel at scale 2.0 looks exactly like a photograph of the
  same corbel at scale 1.0, because scaling every length scales the frame with it. There is
  literally no image that distinguishes the two.
- **H** is a claim about a **ratio** between three mortar profiles. All three structures are the same
  bricks in the same places; only the numbers differ. The frames would be identical triplets.
- **I** is a claim about an **ordering across five step sizes**. Five pairs of near-identical corbels
  would be ten more files, and the property lives in the relation between the readings rather than
  in any one of them.

**J was rendered even though it is also a property test over five wall heights**, and the reason is
that one of its rows is the user's originally reported case — a single brick deleted at a free end
of the wall the game actually builds — and that row **now stands**. That is a thing a photograph can
show and a number cannot: the before/after pair is a 40-course wall, one brick gone, one half bat on
the floor, and nothing else moved.

**The folder was not padded to look complete.** Sixteen files is what had something to say.

---

## 9. A fact about the process, recorded because it is worth recording

The design documents behind this family have been **wrong nine times in this session**, and a test
caught every one. The cases are in the working tree's own comment history: the case-14 ruling made
without the corbelling limits in front of it, the free-end anchor moving by a factor of 30, the
lambda justification that turned out to be a coincidence of two unrelated constants, the U-shaped
step ladder that the depth cap causes, the counterweight prediction in §4 — which was written down
as an expected failure *before* it was run and then failed exactly as predicted.

That is not an embarrassment to hide in a footnote. **A design document that is wrong nine times and
caught nine times is a working process; one that is never wrong is one where nobody is checking.**
The thing that makes it work is that the tests assert *orderings and ratios between structures*
rather than expected values — a crossover that must move, a reading that must scale linearly, an
ordering that must follow the profile numbers — so a wrong constant cannot make them agree by
accident.

---

## 10. Caveats on this deliverable itself

- **The harness in `Tests/CorbelScreenshotTest.cpp` is new test code that has not been through
  `test-expert` or `review-expert`.** It was written inside a reporting task because the alternative
  was no pictures. It changes no production code and adds no test to the headless suite, so its blast
  radius is small — but it should go to `review-expert` before it is called done, and the two things
  a reviewer should look at hardest are the duplicated brick-spawn recipe (§1) and whether writing
  out `FStructureBinding::ApplyResults`' release rule by hand in the cascade command is acceptable or
  should be a binding.
- **Nothing was committed.** The working tree still carries the uncommitted composite-depth work
  exactly as it was found, plus the new test file and this `TestResults/` folder.
- **The images are of the model's verdict, not of physics.** What falls is what `SolveAndBreak`
  condemned and `Release` handed to Chaos; the three seconds afterwards are Chaos settling debris.
  No picture here validates the solver — they show what it decided, legibly enough for a human to
  disagree with it.
