# Structural arching: results and visual validation

**Commit under test:** `4d158ef` — *Arching 5: a corbel resists with its whole depth, and the wall stands*
**Captured:** 2026-08-07 02:30 local
**Engine:** Unreal Engine 5.8, `DestructionGameEditor` Win64 Development

This report is evidence, not advocacy. Where a case passes for an incidental reason, it says so.
Where an image does not show what you would expect it to show, the caption says that instead.

---

## 1. The headline result, in one line

A player cut a staircase-shaped hole out of the bottom corner of the game's 30 x 40 brick wall.
**Before this work the wall came down in a stepping triangular cascade and left a rubble field.
After it, the wall stands and not one brick moves.**

That is the change the four images below show, and it is the change the user originally reported.

---

## 2. The images

All four are 1920 x 1080 PNGs rendered from the real renderer through
`DestructionGame.Visual.StaircaseScreenshot`, which cuts the 36-brick staircase void into the game
mode's own 1220-piece scenario wall exactly as a player does — by pointing at each brick and
choosing **Delete** once for the whole selection.

| File | Bytes | What it actually shows |
|---|---:|---|
| `01_BeforeFix_TheCutIsMade.png` | 2,682,395 | **Pre-fix, cut just made.** The staircase void is open in the bottom-right corner; the wall's remaining edge steps down to the right and the brickwork above it hangs out over open air. Nothing has moved yet. |
| `02_BeforeFix_5sLater_Collapsed.png` | 2,559,956 | **Pre-fix, 5 seconds later — the failure.** A large stepped triangular fracture surface runs diagonally up and to the left, most of the right-hand half of the wall is gone, and a deep field of loose bricks covers the ground. This is the reported bug. |
| `03_AfterFix_TheCutIsMade.png` | 2,669,506 | **Post-fix, cut just made.** Visually the same frame as image 01, as it should be — the same hole in the same wall. |
| `04_AfterFix_5sLater_Standing.png` | 2,550,202 | **Post-fix, 5 seconds later — the fix.** Indistinguishable from image 03. The corbelled bricks still step out over open air and the wall is intact. |

**Read images 03 and 04 as a pair, and understand that their being identical is the point.**
A reader glancing at them alone would see nothing happen. That *is* the result: five seconds of
simulated time pass over an opened staircase void and the measured movement of every one of the
eleven corbelled bricks is **0.000 cm**. Images 01 and 02 are what the same five seconds used to do.

### How "before the cut" is kept honest

The cascade now runs inside the commit, so there is no moment where the hole exists and the joints
do not. The harness therefore pins world time dilation at its 0.0001 floor for the frames between
the cut and the shutter, and then *measures* what it claimed: in both runs the worst-moved corbelled
brick had travelled **0.000000000 cm** when the "before" frame was written. It is an assertion, not
a caption.

### The far-side control

In both runs a control brick 5.6 m along the wall from the void drifted **0.000000 cm**. This is
what separates "the wall stands" from "the whole world froze" in image 04, and "the corner failed"
from "the wall fell over or fell through the floor" in image 02.

### Measured fall of the eleven corbel steps

Same wall, same cut, same five seconds. Course 2 is the bottom step; course 12 the top.

| Corbel course | Pre-fix fall | Post-fix fall |
|---:|---:|---:|
| 2 | 2.659 cm | 0.000 cm |
| 3 | 16.475 cm | 0.000 cm |
| 4 | 24.188 cm | 0.000 cm |
| 5 | 38.037 cm | 0.000 cm |
| 6 | 45.515 cm | 0.000 cm |
| 7 | 45.084 cm | 0.000 cm |
| 8 | 53.515 cm | 0.000 cm |
| 9 | 57.749 cm | 0.000 cm |
| 10 | 63.359 cm | 0.000 cm |
| 11 | 72.742 cm | 0.000 cm |
| 12 | 90.515 cm | 0.000 cm |

The top step fell **90.5 cm — the full height it started at — to the ground.** Post-fix it does not
move, and none of the eleven is released to physics at all.

### How the pre-fix images were obtained, and why that is disclosed

Images 01 and 02 could not be rendered from `4d158ef`, because at `4d158ef` the wall does not fall.
They were produced by checking out **`b69830b`** — *"Design the arching work"*, the immediate parent
of arching slice 1 and the last commit with the pre-arching physics — rebuilding, running the same
unmodified harness, and then restoring `main` and rebuilding. No source was edited to produce them
and nothing was committed. The working tree is back at `4d158ef` with only this untracked
`TestResults/` folder added.

### On the second image pair that was hoped for

The more valuable pair would have been the user's literal reported case — **one brick deleted at a
free end** (acceptance case 3). It was not rendered, and the reason is that the existing harness
cannot be pointed at it cheaply: the void geometry, the 36-brick cut, the eleven-step corbel ladder
and every assertion in `StaircaseScreenshotTest.cpp` are baked into
`Tests/StaircaseWallTestSupport.h` as staircase-specific constants. Retargeting it is a new visual
harness, which under this project's mandatory-TDD rule is new test code that must be driven by
`test-expert` rather than improvised inside a reporting task. **What is rendered is the staircase,
and this section is the disclosure that it is not the free-end case.**

Case 3 is nonetheless *fixed*, and is reported numerically in section 4: it drops **0 pieces** where
it previously dropped 219 and ran 24 breaking passes.

---

## 3. The test suite as run today

Two runs, because one test needs a real renderer and the standard suite runs headless.

### Run A — the headless suite

```
UnrealEditor-Cmd.exe DestructionGame.uproject
  -ExecCmds="Automation RunTests DestructionGame"
  -TestExit="Automation Test Queue Empty"
  -unattended -nopause -nosplash -nullrhi -NoSound -log
```

**130 tests performed. 127 passed. 3 failed. 0 warnings.**

The three failures are all in the wall acceptance suite, and all three are expected:

| Failing test | Why |
|---|---|
| `DestructionGame.Acceptance.Wall.Catalogue` | 8 of the 20 catalogue cases disagree with their agreed verdict — see section 4. |
| `DestructionGame.Acceptance.Wall.MatchedPairs` | 4 of the 6 pair claims do not yet separate. |
| `DestructionGame.Acceptance.Wall.StackBondColumnShearIsHeightIndependent` | Case 18's *property*: the reading should not move with height. It reads 0.040033 at 10 courses and 0.1000825 at 16 — a factor of 2.5 — against an expected 0.01000825 at both. |

The fourth acceptance test, `Acceptance.Wall.TheFixtureLaysTheWallTheProducerLays`, is green.

All five arching slice tests are green:

- `Core.Structure.AMissingBrickIsBridgedNotCantilevered`
- `Core.Structure.AHoleWiderThanOneBrickIsSpanned`
- `Core.Structure.AnArchThrustsAndTheSpringingMustCarryIt`
- `Core.Structure.AnArchNeedsMasonryOverIt`
- `Core.Structure.ACorbelResistsWithItsWholeDepth`

### Run B — the visual test, and a fourth failure worth naming

`DestructionGame.Visual.StaircaseScreenshot` carries `EAutomationTestFlags::NonNullRHI`, so the
`-nullrhi` suite above **does not run it at all**. Run separately against the real renderer, it is a
**131st test, and it fails.**

It fails for an honest and slightly awkward reason: **its assertions still encode the bug.**
The test asserts that the overhang *must come down* —

```
Expected 'the overhang must have come down before the after-frame is taken:
          the top of the corbel fell 0.000 cm and must fall more than 5' to be true.
```

plus eleven more errors of the form *"corbel course N must have been released to physics, not left
standing"*. Twelve failures in total, every one of them saying the wall did not collapse.

`git log` on that file shows it has been touched by exactly one commit, `69ba662`
(*"Bring the overhang down"*), which predates all five arching slices. Its sibling integration test
was updated at slice 5 — it is now named `Integration.AStaircaseVoidLeavesTheOverhangStanding` and
passes. **The visual harness was missed, and it was missed because the NonNullRHI flag keeps it out
of the suite everyone runs.** That is a gap in the safety net as much as a stale test, and it should
be fixed by `test-expert` inverting those assertions.

The file checks in that same test still ran and still passed, which is why usable images exist:
both PNGs were confirmed present, over the 32 kB floor, carrying a real PNG signature and IHDR, and
1920 x 1080. No material fell back to `WorldGridMaterial`, so nothing in these frames is the
checkerboard.

---

## 4. The acceptance catalogue, case by case

`claude_plans/WALL_CASES.html` is the agreed 20-case set — twenty wall configurations with an
expected verdict each, drawn from how real masonry behaves rather than from what the solver
computes. `Tests/WallAcceptanceTest.cpp` implements it. Three verdicts: **stands** (nothing left the
structure and no joint over the cut gave), **local loss** (a named, bounded set fell and nothing
else), **collapse** (a named region came down and named survivors did not).

Measured today, in full. "Worst" is the highest joint utilisation anywhere in the wall.

| # | Case | Expected | Fell | Worst | Verdict |
|---:|---|---|---:|---:|:--:|
| 1 | Intact wall | stands | 0 | 0.00368054 | PASS |
| 2 | One brick out, mid-wall | stands | 0 | 0.0142144 | PASS |
| 3 | One brick out at the free end | stands | 0 | 0.00706893 | PASS |
| 4 | One brick out of the bottom course | stands | 0 | 0.0147221 | PASS |
| 5 | Alternate bricks out of one course | stands | 0 | 0.0142144 | PASS |
| 6 | Two-brick opening, deep cover | stands | 0 | 0.0374509 | PASS |
| 7 | Four-brick opening, eight courses over | stands | 0 | 0.160132 | PASS |
| 8 | Four-brick opening, one course over | local loss | 0 | 0.218869 | **FAIL** |
| 9 | Ten-brick opening, eight courses over | collapse | 0 | 0.98502 | **FAIL** |
| 10 | Opening at a free end, no abutment | collapse | 12 | 0.299975 | **FAIL** |
| 11 | Wall on two piers, six-brick clear span | stands | 0 | 0.362193 | PASS |
| 12 | The same span on one-brick piers | collapse | 76 | 0.267538 | **FAIL** |
| 13 | Corbel, quarter brick per course | stands | 0 | 0.0702368 | PASS |
| 14 | Corbel, half brick per course | collapse | 0 | 0.195161 | **FAIL** |
| 15 | Header out half a brick, six courses on top | stands | 0 | 0.00184437 | PASS |
| 16 | The same header at the top, nothing on it | local loss | 0 | 0.0582038 | **FAIL** |
| 17 | Stack bond, intact | stands | 0 | 0.00108927 | PASS |
| 18 | Stack bond, one brick out | stands | 0 | 0.040033 | PASS (verdict only) |
| 19 | Bottom course out under half the wall | collapse | 34 | 0.31804 | **FAIL** |
| 20 | Staircase void | local loss | 9 | 0.296506 | **FAIL** |

**12 of 20 pass, 8 fail.** That is down from 10 failing before slice 5.

Three of the passes deserve an asterisk, because a green row is not always a modelled row:

- **Case 18 passes on its verdict and fails on its substance.** "Stands" is easy — a hanging
  stack-bond column sits at a few percent of capacity, so a model that got the load path badly wrong
  would still say stands. The property that made the case interesting is that the reading should be
  *height-independent*, and it is not: the model routes the whole column down onto the joints at its
  foot, giving 0.040033 at 10 courses and 0.1000825 at 16 rather than 0.01000825 at both. That is
  the separate red test in section 3.
- **Cases 1 and 17 are intact walls.** They are regression anchors, not achievements.
- **Cases 13 and 14 are a matched pair that now answers both halves the same way** — see below.

### What moved across the five slices

- **Slice 1 flipped three rows:** cases 2, 4 and 5, every one-brick deletion.
- **Slices 2, 3 and 4 flipped nothing.** Slice 2 moved three numbers (case 8's drop count 2 to 0,
  case 20's 69 to 59, and cases 7/9/11 *worse*, 50 to 66, 108 to 116 and 66 to 92). Slice 3 moved
  nothing at all — all 22 failure lines were byte-identical with the thrust on and off. Slice 4
  moved exactly one number, case 12's drop count, 105 to 102.
- **Slice 5 flipped four rows and regressed two.** Fixed: **3, 6, 7 and 11**. Case 3 is the user's
  reported case and went from 219 dropped pieces and 24 breaking passes to none; cases 6, 7 and 11
  are the jamb reveal, from 66, 50 and 92 dropped pieces to zero. Regressed: **12 and 14**.
- **Case 20, the staircase, went from 59 dropped pieces to 9** — against an agreed local-loss set
  that names 2 (`c3/4.5` and `c5/2.5`). Both named bricks do come down; seven others come with them.
- **Case 8 has never moved.**

### The matched pairs

Five variables, each isolated by two cases differing in exactly one thing. Currently **2 of 6 pass**
(up from 1 of 6 before slice 5):

| Variable | Pair | Result |
|---|---|---|
| Depth of cover | 7 vs 8 | **FAIL** — both drop 0 |
| Span | 7 vs 9 | **FAIL** — both drop 0 |
| Abutment | 7 vs 10 | PASS — 0 against 12 |
| Pier width | 11 vs 12 | PASS — 0 against 76 |
| Corbel step | 13 vs 14 | **FAIL** — both drop 0 |
| Superimposed load | 15 vs 16 | **FAIL** — both drop 0 |

Note the shape of the remaining failures. Before this work the predicted signature of a model with
no lateral compression path was *every falling half passing and every standing half failing* — a
model that can only say "falls". **The failures have inverted.** Every one of the four now fails
because the *falling* half stands. The model has acquired the ability to say "stands" and has, in
these four cases, overshot. That is a different and more tractable defect than the one the
catalogue was written to expose.

---

## 5. The five slices

### Slice 1 — a missing brick is bridged, not hung from
A brick with one of its two bed joints cut away was being modelled as a cantilever hanging off the
remaining half-patch. Real masonry does not do that: the course above spans the gap and sheds the
load sideways in compression. The slice caps the bending stress at the point the joint would open,
`k = min(1, |sigma_n| / sigma_b)`, giving exactly zero peak tension and exactly `2|sigma_n|` peak
compression. **A half-seated joint went from 1.6274919747 to 0.0141885219.** One interior deletion
in a 7 x 30 wall went from **7 cascade passes shedding 55 joints to breaking nothing at all.**
Property fuzzes stayed green at 12,000 and 8,000 cases.

### Slice 2 — a hole wider than one brick is spanned
One brick can be bridged by its neighbours; three cannot, and the course above a wider hole was
still being cantilevered. The slice re-seats a spanning group onto the abutments either side. The
strongest evidence is conservation rather than any single reading: intact, the four group bricks
carry 74435.5, 74534.1, 74566.1 and 74534.1 uu; after the cut the two surviving seats carry
**148969.56 + 149100.16 = 298069.72268342471 uu — the intact sum reproduced exactly.** The cut wall
that previously left 2 pieces stranded and 1 falling, with springings cantilevered at 10.46 of
capacity over 5 cascade passes breaking 43 joints, now breaks nothing.

### Slice 3 — an arch thrusts, and the springing must carry it
An arch does not just press down; it pushes sideways, and something has to push back. The slice
gives the springing a horizontal component, `H = W·L/(8r)` with `r = d_e/3`, so its force becomes
`(H, 0, -V)` rather than `(0, 0, -V)`. With the depth taken as `d_e = 0.866·L` this reduces to
**H/V = 3L/(4·d_e) = 0.86605080831408776** at every span, load and material. The important
structural property is that **the sum of the horizontal thrusts is exactly 0** — not checked to a
tolerance but zero bit-for-bit, because there is one direction vector and two signs, so `+H·D` and
`−H·D` cancel. On a 30 x 40 wall a ten-cell opening reads 0.88649 and stands; a twenty-cell opening
reads 1.08603 and comes down. This slice moved no acceptance row.

### Slice 4 — an arch needs masonry over it
The thrust line has to fit *inside* the masonry, so an arch with nothing on top of it is not an arch.
The slice caps the depth at the cover actually found by walking up from the opening:
`d_e = min(0.866·L, cover)`. Ten cells under a **single course** go from **H/V 0.866 to 22.5**, and
the springing's shear from **0.058031337290390166 to 1.5076541428043366** — over capacity, so the
hole comes down (11 cascade passes, 40 joints failed, 31 pieces left with no path to ground). The
guard that this is a `min` and not a substitution is that the two deep-cover rows, where the 0.866
angle governs, stay bit-identical; an implementation taking the cover outright would read 0.833 and
0.592 there.

### Slice 5 — a corbel resists with its whole depth
This is the slice that produced image 04. A stack of courses over a lost support does not resist its
overturning moment as a sequence of independent bed patches — the wall acts as a **deep beam**, and
the plane taking the moment is a vertical section through the bonded masonry standing above,
`W = t·D²/6`. Eleven courses over the staircase's bottom step is 11,627 cm³ against the bed patch's
179.48 cm³, a factor of 64.8. **The staircase's bottom rung went from 22.929528199727653 to
0.36903147272727271, and the ladder went from 8 of 11 rungs over capacity to 0 of 11.** The joint
gives at the *lesser* of the two readings, which keeps the model nested rather than replaced and is
load-bearing at the top of the ladder: one course of depth is 96.09 cm³, *shallower* than the bed
patch, so the top step keeps its own 0.058203838 to the last bit. Fuzzes green at 20,000 cases.

---

## 6. What is still wrong

Stated plainly, and not buried.

### 6.1 Case 14 is a genuine contradiction and needs the user's ruling

This is the most important item here, and it is a **product decision, not a bug fix**.

- Acceptance case 14 asserts that a four-course, half-brick-per-course corbel **COLLAPSES**.
- `Core.Structure.ACorbelResistsWithItsWholeDepth` asserts that a five-step one **STANDS**, at 0.219.

Mechanically these are the same fixture. One was laid that way and one was cut that way, and
**there is nothing in the physics that separates them.** Case 14 measures 0.195161 today and stands.
Either the catalogue is wrong about built corbels or the 2026-08-06 free-end ruling is wrong about
raking ones. The project's own note is explicit that a threshold must **not** be sited between them,
because that would be tuning a number to make two contradictory statements both come out true.

Case 12 regressed alongside it at slice 5 (it keeps 24 of its named bricks up) and belongs to the
same decision.

### 6.2 Case 8 cannot be fixed by the thrust check, and this is structural

Case 8 is a four-brick opening with a **single course** over it. It should lose the two bricks with
no bed patch at all. It drops nothing, and it has never moved across all five slices.

The reason is worth stating precisely, because it looks like a tuning problem and is not. The
springing's shear utilisation is

```
(H/V)·σ_n / (c + μ·σ_n)
```

**As `σ_n` approaches zero the cohesion term `c` dominates the denominator and the whole reading
goes to zero — however large `H/V` becomes.** Case 8's wall is five courses, so its springing carries
about two brick weights and `σ_n ≈ 0.005 MPa`, about 2.5% of the 0.2 MPa of cohesion sitting there
unused. Slice 4's cover cap does raise `H/V` from 0.866 to roughly 6.75, and the springing still
reads about 0.17. The thrust check needs a thin cover **and** enough weight above it for friction to
be what is being spent; case 8 has the first and not the second.

So case 8's four bricks come down for the reason the catalogue itself gives — **flexure of a single
spanning course** — which is a claim about *head joints*, and slice 4 does not touch head joints.
**It needs its own slice or its own ruling, and neither exists.** This was predicted while writing
slice 4's red test and confirmed the same day when slice 4 landed and case 8 still dropped 0.

### 6.3 The visual test encodes the old behaviour

Covered in section 3. `Tests/StaircaseScreenshotTest.cpp` asserts the overhang falls, which is now
wrong. It is invisible to the normal suite because of its `NonNullRHI` flag. Two things to fix: the
assertions, and the fact that a whole test can rot unnoticed.

### 6.4 The rest of the outstanding list

- **Case 16's expected verdict is probably wrong**, and the pair has already made its point anyway.
  Cases 15 and 16 are identical geometry differing only in what sits on the header's tail, and read
  0.00184 against 0.05820 — a factor of 32. The superimposed-load term exists and works; only the
  threshold is in question. The catalogue's "local loss" was a rigid-body overturning reading that
  ignored the bond.
- **Case 18's catalogue figure of 0.0200 does not reproduce and is arithmetically wrong** — it
  divided by `f_xk1` = 0.1 MPa, the flexural figure, where a head joint in shear is measured against
  `f_vk0` = 0.2 MPa. The correct expectation is 0.01000825. Separately, the model is not
  height-independent, which is the real defect.
- **An arched joint cannot be explained to the player.** `PieceInspection` and
  `BuildPieceMenuInspector` have no arithmetic a player can follow for a joint reading 0.014 beside
  a visible moment. Slice 3 added a second unexplained thing: a springing's force now points
  sideways on a bed joint with nothing on screen saying why.
- **Slice 2's head-joint route caps an opening at roughly 50 brick weights a side**, about two
  columns, because it routes load outward in pure shear with no normal force to buy friction with.
- **Dry stone can never arch and no per-material branch says so** — it reads a constant
  1.237215440448697 at any opening width, because `sigma_n` cancels.
- **The cycle rule is still missing.** Slice 2 avoids the loop by construction rather than supplying
  a rule for dividing load around one; a genuine cycle still strands.
- **Three untested choices inside slice 4**: cover is measured at the abutments and reduced to the
  thinnest single number; the walk follows a chain rather than exploring both parents; and the walk
  is unobservable from outside. Distinguishing the first needs a stepped or gabled wall, and no
  fixture has one.
- **Case 20's remaining seven pieces** beyond the two the agreed set names are a smaller version of
  the same question as case 8.
- **The user-reported Delete lag has not been re-measured** since the adjacency index landed. The
  1.2 s figure on record is pre-fix only.
- **`Scripts/Convert-CommentBlocks.ps1 -Check` exits 2**, refusing
  `Tests/StructureCascadeTest.cpp` at line 707. Pre-existing and unrelated to this work.

### 6.5 One numeric discrepancy in the brief for this report

The brief for this report gave case 3 as reading **0.01588**. The measured worst joint anywhere in
case 3's wall today is **0.00706893**. The verdict is unaffected — the case passes and drops nothing
— but the figure quoted is not the one this row produces, and it is recorded here rather than
silently reconciled.

---

## 7. A note on the process

Worth recording as a fact about the method rather than hidden as an embarrassment: **the design
documents were wrong repeatedly during this work, and the tests caught it every time.**

1. `ARCHING_DESIGN.md` gave the springing figure as **0.036**; that was the four-cell arithmetic
   applied to a three-cell fixture. Measured 0.028371. Document corrected.
2. `ARCHING_DESIGN.md`'s depth table **pointed the wrong way** — every row held the staircase's
   moment fixed while shrinking the depth, which no real wall presents. Corrected in place.
3. `CURRENT_STATE.md` claimed that after slice 2 and before slice 3 "every opening of every width
   stands". Wrong, and measured wrong: both of slice 3's test openings came down *before* slice 3
   landed, at the re-seat head joint and for the wrong reason.
4. The design's cantilever figure of **1.62971** was derived by a method the fixture could not
   satisfy; the fixture-derived figures are 1.62649 and 1.62719.
5. `WALL_CASES.html` is wrong in at least two places — case 16's verdict, and case 18's 0.0200
   against the wrong strength constant.
6. Case 18 had itself already been drafted as a collapse and overturned by arithmetic before
   implementation began.

Two failure modes were caught by deliberately-designed negative tests rather than by luck. A
**moment scale** would have been the tempting one-line implementation of slice 5 and reads the load
ladder 18x low. Relieving **only the tension edge** would have masked the entire slice — every
"must fall" row would have passed for the wrong reason. And a mutation test proved that the
cantilever oracle was self-fulfilling: with slice 1's `min` deleted, the headline test still
reported Success, because its expectation was read back off the very function the defect lived in.
The row that actually catches it derives the moment from the fixture's own geometry instead.

---

## 8. Reproducing this

```bash
# Close the Unreal editor first — it locks the DLL and the build fails at link.

"C:\Program Files\Epic Games\UE_5.8\Engine\Build\BatchFiles\Build.bat" \
  DestructionGameEditor Win64 Development \
  -Project="<repo>\DestructionGame.uproject" -WaitMutex -abslog="<unique path>"

# Run A — the headless suite (130 tests)
UnrealEditor-Cmd.exe "<repo>\DestructionGame.uproject" \
  -ExecCmds="Automation RunTests DestructionGame" \
  -TestExit="Automation Test Queue Empty" \
  -unattended -nopause -nosplash -nullrhi -NoSound -log -abslog="<path>"

# Run B — the visual test (needs a real RHI; -nullrhi MUST be absent)
UnrealEditor-Cmd.exe "<repo>\DestructionGame.uproject" /Game/Maps/Lvl_Sandbox \
  -game -windowed -ResX=1920 -ResY=1080 -ForceRes -RenderOffScreen \
  -nosplash -NoSound -unattended -nopause -log -abslog="<path>" \
  -ExecCmds="Automation RunTests DestructionGame.Visual.StaircaseScreenshot" \
  -TestExit="Automation Test Queue Empty"
```

Two traps, both of which have cost time on this project before:

- **Results never reach stdout and the command exits 0 even when tests fail.** Read the log and grep
  `LogAutomationController`. Both runs above exited 0; one of them had three failures and the other
  had twelve.
- **`Build.bat` reporting "Target is up to date" does not mean your source is linked.** Use
  `-abslog=` with a unique path and confirm you see `Link [x64] UnrealEditor-DestructionGame.dll`.
  Both builds for this report were verified by watching the link step and the DLL's size change
  (2,076,672 bytes pre-arching against 2,387,968 at `4d158ef`).

Logs from these runs: `Saved/Logs/SuiteRun_20260807.log`, `Saved/Logs/ShotRun_20260807.log`,
`Saved/Logs/ShotRun_PreArching.log`.
