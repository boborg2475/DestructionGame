# Every test expectation that changed, and why

Written 2026-08-07, covering commits `b69830b` through `HEAD`. This is the document requested at the point the arching work began: *"If you update tests then please create a doc at the end to tell me what was updated."*

**The rule applied throughout: a changed expectation is re-derived from what the fixture actually presents, never adjusted to make the suite green.** Where a number moved, the new value comes from working the physics forward independently; where a verdict inverted, there is a mutation showing the old expectation was the old *behaviour* rather than a judgement.

---

## 1. The two staircase tests — inverted by the user's ruling

These are the changes the ruling was *about*. On 2026-08-06 the user ruled that a brick deleted at a free end must not bring a wall down. Any rule local enough to save the free end also saves the staircase corbel, so these two tests were asserting the wrong thing.

### `Core.Structure.AStaircaseVoidCondemnsTheCorbel`

Eleven rungs of a corbel ladder, re-derived from the composite section `t·D²/6` at the depth each rung actually presents.

| course | old | new |
|---|---|---|
| 2 | 22.929528199727653 | **0.36903147272727271** |
| 3 | 17.564865743604997 | 0.34348313999999996 |
| 4 | 13.107009008923262 | 0.31803994444444439 |
| 5 | 9.4723650707911968 | 0.29274131249999996 |
| 6 | 6.5773433105294483 | 0.26764919999999998 |
| 7 | 4.338353109458656 | 0.24286686666666668 |
| 8 | 2.6718038488994647 | 0.21858017999999996 |
| 9 | 1.4941049101725163 | 0.19516087500000001 |
| 10 | 0.72166567459845343 | 0.17347633333333334 |
| 11 | 0.27089552349791796 | 0.15612870000000001 |
| 12 | 0.058203838191552663 | **unchanged** |

Rungs over capacity: **8 of 11 → 0 of 11.**

Course 12 not moving is the strongest evidence this is a correction. It is the top course of the wall, so it has one course of depth — `W = 96.09 cm³`, *smaller* than the bed patch's 179.48 — and the `min` in the implementation floors it at the patch value. A change that moved course 12 would have been a change to the whole model rather than to the deep-beam case.

**The load path is unchanged and asserted so.** The force and moment ladders (38.5 brick weights, 1608.75 brick-weight-cm) reproduce to 1e-5. Only the section resisting them changed.

### `Integration.AStaircaseVoidBringsTheOverhangDown` → `…LeavesTheOverhangStanding`

**Renamed, because the name asserted the opposite of the ruling.** Eleven "this piece falls" rows became eleven "this piece does not move" rows; 0 of 11 joints break; 0 pieces lost.

---

## 2. Slice 5's consequences elsewhere

Composite action changes what a corbelled joint resists with, so anything measuring a corbelled joint moved. Each was re-derived.

| Test | Old | New | Why |
|---|---|---|---|
| `StructureArchingTest` anchor 2 | 22.92952589, 8 of 11 | 0.3690314727, 0 of 11 | A third copy of the staircase figure, used as slice 1's direction check. It still separates the mechanisms — an ungated arch reads 0.0195, a factor of 19 away. |
| `Core.Structure.MomentAccumulatesAlongTheLoadPath`, joint 1 | 0.24206 | 0.1491896467 | A 3-brick corbel with 2 courses of composite section. The head-joint headline (0.8314546153846154) is untouched — head joints are not bed patches. |
| `World.Push…MortarRaggedWorstAsBuilt` | 0.0455104479 | 0.0390321745 | The top corbel has one brick on it, so 2 courses of depth. Its sibling at 0.0582038382 is **unmoved**, being a top course with no stack. |
| `Core.Structure.ConnectionUtilisation` ×2 | — | Identity extended to 3 arguments | Mechanical. No value changed; without it the documented identity would have been false. |

---

## 3. Case 14 — the user's second ruling

`Acceptance.Wall.Catalogue` case 14 (a four-course, half-brick-per-course corbel) asserted **Collapse**. `ACorbelResistsWithItsWholeDepth` asserted a five-step corbel **stands** at 0.219. Mechanically the same fixture — one laid that way, one cut that way — with nothing in the physics separating them. On 2026-08-07 the user ruled that corbels stand.

**Derived, not flipped.** The ladder gives 0.058203838 / 0.156128700 / 0.173476333 / **0.195160875**, and the same four lines reproduce 0.219 at five steps and 0.36903147272727271 at eleven. Both cross-checks are asserted *in the test*, so a drifted derivation goes red rather than agreeing with itself.

**The mutation is the evidence.** Deleting slice 5 and re-running drops *exactly* the four bricks the old expectation named — the old verdict shown to be pre-slice-5 behaviour.

### The matched pair had to move rather than be dropped

Cases 13 and 14 isolated corbel projection **by outcome**, and "one loses nothing, one loses something" is unsatisfiable once both stand. So the pair moved into a new test, `Acceptance.Wall.CorbelProjectionIsReadInTheJointNotInTheOutcome`, which asserts the half-brick corbel reads **strictly more and by at least 2×** (measured 2.78×) — because a model with no projection term at all reads them *equal*, and a strict inequality alone is satisfiable by a last bit. It also pins *which* joint governs, since `Worst` is the worst joint anywhere in the wall and without that row the number could be right about the base compression instead.

### What this ruling costs, recorded rather than lost

The model now has **no way to express a corbel failing by projecting too far.** Case 14's corbel puts its mass 5.625 cm out on a 5.125 cm bearing and projects 43 cm from a 10.25 cm wall; real corbelling caps total projection near the wall thickness. That failure is *global overturning of the corbelled mass about the wall face* — a stability check this model has never had, because it only ever checks joints. The abandoned reading is written into the test file rather than deleted, and the gap is on the outstanding list.

---

## 4. A test that rotted for five slices

`Visual.StaircaseScreenshot` was **red with 12 errors** and had been since before any of this work. Its assertions still encoded the bug.

| Old | New |
|---|---|
| top corbel `fell > 5 cm` | 11 × `moved < 0.1 cm` (measured **0.000000 cm**) |
| 11 × `IsReleased == true` | 11 × `IsReleased == false` |
| — | **new**: elapsed world time ≥ 0.5 s (measured 5.025 s over 420 frames) |

The elapsed-time row exists because every physical assertion in that test is now a *non*-movement, and **a frozen world would satisfy all of them**. Non-displacement is safe where displacement is not: DESIGN.md bans using distance to prove a break, not to prove its absence.

**Why it rotted is the more important finding.** It carries `EAutomationTestFlags::NonNullRHI`, which keeps it out of the documented headless test command — so nothing ran it for five slices while its sibling integration test was being updated and renamed *in the same commit* that inverted the behaviour. Anything behind that flag is invisible to the ordinary suite. Two fixes are proposed and left for the user: a documented second run using `RunTests DestructionGame.Visual`, and — cheaper and more structural — a world-free assertion on the 30×40 scenario wall the screenshot test cuts, which nothing headless currently covers, and which would have gone red at slice 5 in the ordinary suite.

---

## 5. Test-side corrections that were not behaviour changes

| Test | What changed | Why |
|---|---|---|
| `StructureArchingTest` fixture rows ×6 | Cantilever oracle re-sourced from the fixture's own `\|F_z\| × 5.625 cm` instead of `GetConnectionMoment` | The design requires the capped moment to be both what travels and what the joint publishes, so an oracle built on the published moment necessarily returns the arched answer. The rows were authored against a trial mutation that capped in the wrong place. |
| `StructureThrustTest` dry stone | Openings 1 and 5 cells → **2 and 5**; wall 12 → **20 courses** | A one-cell opening is exactly the topology another test asserts carries shear *exactly zero*. No implementation satisfies both, and separating them by material is the per-material branch DESIGN.md forbids. Two cells is the narrowest opening that leaves a brick with no seat at all. The course increase pre-empts a failure that would only have appeared at slice 4. |
| `ConnectionLoadTest.cpp` | Anonymous namespace given a name | **Build blocker.** `constexpr double F` at anonymous-namespace scope leaks across a unity blob; `Chaos/Utilities.h` declares four locals named `F`, producing four C4459 errors *in engine source*. No value, expectation or assertion moved. |
| Case 13 header comment | "reads 0.081" → 0.0702368 | Stale pre-slice-5 figure, found while editing. |

---

## 6. Numbers in the design that the tests disproved

Recorded because it is a fact about the process, not a failure of it. **The design document was wrong five times and a test caught every one.** In no case was a test adjusted to agree with it.

1. **Slice 1's 1.62971** → measured 1.62749 / 1.62719. A flush wall's odd courses are six full bricks plus two half bats, weighing 0.65% less, so the joints carry 27.94 brick weights rather than a round 28.
2. **Slice 2's 0.036** → derived 0.02837070531192621. 0.036 is the *four*-cell figure; an N-cell cut leaves N−1 unseated bricks plus 2 half-seated ones, so three cells give four columns.
3. **"After slice 2 and before slice 3 every opening of every width stands"** → false and never measured. Slice 2's re-seat head joints carry pure shear against 0.2 MPa with no normal force to buy friction with, capping an opening at about two columns.
4. **"Acceptance case 8 will flip at slice 4"** → it does not and cannot. As `σ_n → 0` the cohesion term dominates and shear utilisation goes to zero *however large* `H/V` grows.
5. **The composite depth table, and 0.0082.** Every row of the table held one wall's moment fixed while shrinking the depth beneath it — a pair no wall presents. In a real k-step corbel the moment grows as `k³` against a section of `k²`, so the reading rises with depth and crosses capacity near **36 steps**, not under seven. And 0.0082 reproduces from no fixture; the 7×30 free end reads 0.015880, with a **two-arm** moment (own weight at 5.625 cm, everything above at 11.25).

### And one number quoted twice that turned out to be two numbers

**0.01588 and 0.00706893 are both correct, for different walls.** 0.01588 is the design's 7-per-course × 30-course flush wall: 29 courses over the joint, two-arm moment 481.1625, section 80,814.84 cm³. 0.00706893 is acceptance case 3's **12**-cell × 30-course wall with the deletion one course higher. The descriptions differ by one digit and the answers by a factor of two, so the convention now is to name the wall whenever either is quoted.

---

## 7. What is still red, and why

**131 tests, 128 pass, 3 fail** — all `Acceptance.Wall.*`. The catalogue is 7 rows red of 20 (was 8); MatchedPairs 3 of 5 (was 4 of 6).

- **Case 8** — a four-brick opening with one course over. Cannot be fixed by the thrust check for the reason in §6.4; its four bricks want to come down for flexure of a single spanning course, which is a claim about head joints. Needs its own slice or a ruling.
- **Case 12** — a wall on one-brick piers. Awaiting a ruling.
- **Cases 9, 10, 16, 19, 20** — remaining catalogue rows.

**The failure signature has inverted, which is the encouraging part.** The prediction before any of this work was that a model able only to say "falls" would pass every falling half of a matched pair and fail every standing half. All four remaining pair failures are now the opposite — the *falling* half standing. That is a different and more tractable defect than the one this work started from.
