# Composite depth — how far up a wall deep-beam action reaches

**Status: DESIGN ONLY. Nothing built. `DestructionGame.Core.Structure.ACorbelResistsWithItsWholeDepth` is RED at `e8db96c` and this document says what turns it green.** Read [DESIGN.md §2 and §3](DESIGN.md), all of [MOMENTS_DESIGN.md](MOMENTS_DESIGN.md) and all of [ARCHING_DESIGN.md](ARCHING_DESIGN.md) — in particular slice 5 and the ⚠ block under its depth table — before this. This sits directly on slice 5 and revises one of its claims, one of CURRENT_STATE's, and one of DESIGN.md §3's.

**THE HEADLINE, SO NOBODY READS PAST IT.** Three mechanisms were worked through with numbers. **The two that are physically honest do not bound the depth, and the one that bounds it is not derived from a published figure.** That is the whole difficulty and it is stated up front rather than discovered in slice 3.

| candidate | bounds mortar? | dry stone for free? | verdict |
|---|---|---|---|
| shear flow `q = VQ/I` as a depth cap | **NO — it is a FLOOR, not a ceiling** | yes, exactly | **rejected as the bound**, kept as a gate |
| apply the composite shear and let the cascade break joints | **NO — reads 0.128 of capacity, nothing breaks** | yes, exactly | **rejected now**, right direction later |
| `D = min(masonry above, λ·M/F)` | yes, and only where it should | **no — it is inert for dry stone** | **adopted, with λ a RULING** |

---

## The problem, restated as the one number that matters

The same eleven-step cut reads **0.22300137936935951** under 40 courses and **0.36903147272727271** under 13. Measured at `e8db96c`: the taller wall's bottom rung carries **5.199×** the force and **7.212×** the moment, while the credited section grows **11.933×** — and `7.212 / 11.933 = 0.6044`, which is the ratio the test prints. More load, less utilisation.

```
M_tall  = 11601.5102 brick-weight-cm      (7.21151 x the 13-course 1608.75)
F_tall  =   200.1615 brick weights        (5.199   x 38.5)
W(285)  = 10.25 x 285^2 / 6 = 138759.375 cm3
util    = 11601.5102 x 2667.198625 / (138759.375 x 10^4 x 0.10) = 0.22300...
```

**`MasonryDepthAboveCm` is asked for `TNumericLimits<double>::Max()` and answers with the whole wall.** The comment above it argues the bound is the `k³` moment against the `k²` section — true only when the wall and the cut are the same height, which every fixture was until `e8db96c`.

---

## Candidate 1 — shear flow as the depth cap. WORKED, AND IT PUSHES THE WRONG WAY.

Composite action needs the bed joints to carry the horizontal shear bending demands, so walk up and stop where `q = V·Q/I` exceeds `c + μ·σ_n`.

### The search is not an upward walk

Put the cut joint at `z = 0`, section `[0, D]`, thickness `t`, neutral axis at `D/2`:

```
I(D)   = t·D³/12
Q(z)   = t·z·(D − z)/2
τ(z)   = 6V·z(D − z)/(t·D³)
τ_max  = 1.5·V/(t·D)          at z = D/2, and ZERO at both extreme fibres
```

The binding joint is at **mid-depth**; demand at the cut joint itself and at the top is exactly zero. "Walk up until it fails" would never fail — it starts at zero.

### And for mortar it never binds, by a factor of 7.8

```
V        = 200.1615 x 2667.198625 = 533,870 uu
τ_max    = 1.5 x 533,870 / (10.25 x 285 x 10^4) = 0.027413 MPa
σ_n      = 0.024117 MPa at the neutral axis
capacity = 0.2 + 0.6 x 0.024117 = 0.214470 MPa       ratio = 0.1278
```

**`τ_max ∝ 1/D`, so a deeper section is EASIER to sustain, not harder.** Solving `1.5V/(t·D) = 0.2` gives `D ≥ 39.06 cm` — a **minimum** composite depth of 5.2 courses and no maximum whatever. Four pessimisms were checked (half transfer width 0.257, running bond's 45% contact 0.28, a blanket 4× 0.51) and none reach capacity.

**The suite already knew this.** ARCHING_DESIGN slice 5 records that a shear-transfer bound "satisfies the same three constraints" and that "no assertion distinguishes it from this one" — which is exactly the statement that it credits the same full depth. Two independent derivations, same answer.

### What it IS good for

Dry stone, `c = 0.0` exactly, `μ = 0.7`:

| bottom sub-section | τ_max | capacity `0.7·σ_n` | ratio |
|---|---|---|---|
| 82.5 cm | 0.01822 | 0.004887 | **3.73** |
| 41.25 | 0.03643 | 0.007331 | **4.97** |
| 20.625 | 0.07286 | 0.008552 | **8.52** |
| 10.3 | 0.14572 | 0.009163 | **15.90** |

Over capacity at every depth and worse as it shallows — a dry-stone wall gets **no composite depth at all**, from two existing profile fields with no branch.

**Keep it as an ELIGIBILITY GATE, not the bound — and do not build it yet.** `DryStone.TensileStrengthMPa` is an exact zero, so that row is condemned at any section and **no fixture can tell the gate from its absence.** It needs a test-only frictional profile with zero cohesion and non-zero tensile strength first. Slice 5 below.

---

## Candidate 2 — apply the shear and let the cascade decide. RIGHT DIRECTION, WRONG DECADE.

Real tools have no composite-depth parameter: continuum FE solves `Ku = f`; discrete-element codes get it from contact laws; Lourenço's simplified micro-model is blocks plus Mohr-Coulomb interfaces, essentially `FConnection`. So compute the demand, **apply it as a real load**, and let `SolveAndBreak` sever what cannot carry it. Depth becomes an output.

**It is the more honest physics.** It is also nearly free of plumbing — `MasonryDepthAboveCm` already stops at joints that have `HasGiven()`.

### It fails on the same number

Same arithmetic, only the *use* changes. **0.1278 of capacity. No joint breaks.** The emergent depth is the whole wall and the property stays red. That alone ends it.

**(a) The fixpoint is bimodal, so it can never produce a graded answer.** Severing at height `z` splits the section into two shallower ones and `τ ∝ 1/D` in each — **breaking a connector raises the demand on the survivors.** Positive feedback. Measured on dry stone: 3.73 → 4.97 → 8.52 → 15.90 as it shallows. The fixpoint is *all* or *one course*, never between. **"Larger overhang, less depth" cannot emerge from this; only "bonded or not" can.**

**(b) It needs a joint state the model has never had.** A broken bed joint **leaves the support relation entirely** (DESIGN.md §3), so a joint that slipped in composite shear would stop *bearing* and every course above would be reported falling. Real masonry delaminates and keeps bearing — which is why Lourenço's interfaces carry independent normal and shear behaviour. This needs **"slipped but still closed"**, contradicting "giving is irreversible and a given joint carries exactly nothing". A subsystem, not a slice.

**(c) Not regression-cheap.** Bed joints in a cut wall carry essentially zero shear today; adding 0.027 MPa takes them from ~0.005 to ~0.128. Every "worst joint" in every cut fixture moves. The intact wall is safe.

**Verdict: not now, and recorded as the direction.** When a joint can slip without falling out of the graph, come back — and the depth parameter disappears with it.

---

## Candidate 3 — ADOPTED. `D = min(masonry above, λ·M/F)`

**The bound is the joint's own effective lever arm.** `e = M/F` is a length the solver already publishes, derived from load and geometry, with no new field, no new constant on a profile and no per-material branch.

```
e   = |M| / |F|
D   = min( MasonryDepthAboveCm(...) , λ·e )
W_c = t·D²/6                             unchanged
```

**It is the "overhang" the user named, computed rather than measured.** ARCHING_DESIGN independently derived the same quantity: "an average arm of **41.79 cm**" — which is `1608.75 / 38.5` to five digits, exactly `e` for the staircase's bottom rung. Two derivations, months apart, same number.

### The rule collapses to one identity

Where `λ·e` governs, `σ_c = 6F²/(λ²·t·M)`, so with `F` in brick weights and `M` in brick-weight-centimetres:

```
utilisation  =  K · F²/M        K = 1.56129/λ²  =  0.130117 at λ = 3.464
```

Scenario `0.130117 × 200.1615²/11601.51 = 0.44934`; free end `0.130117 × 43.2742²/481.210 = 0.50637`. **The property becomes the statement that `F²/M` cannot fall** — one-sided, tolerance-free, and true whenever the added load's arm is at most `2e`. Measured on this pair: increment arm **61.813 cm against `2e = 83.571`**, a 26% margin.

### Where it fires, and it is a theorem rather than luck

For a raking corbel of `k` steps, `F(k) ≈ k²/4` and `M(k) ≈ 0.9375·k³`, so **`e ≈ 3.75k = half the corbel's own depth`**. Checked: k=5 → 19.6875 (theory 18.75); k=11 → 41.7857 (41.25); k=45 → 168.906 (168.75). Therefore `λ·e ≈ (λ/2)·D_corbel` against `D_avail = D_corbel` for a matched wall, **so for any λ > 2 the bound never fires on a corbel whose wall stops at its top.** All four original `FCorbelCase` rows are bit-identical structurally, not by luck. It fires exactly when the wall above is more than `λ/2 ≈ 1.73×` the corbel's height.

Second lemma: **any half seat has `e ≥ 5.625 cm`**, so `λ·e ≥ 19.49 cm ≈ 2.6 courses`. **No joint with two or fewer courses over it can be touched** — which is why the waist, the ragged end and `MortarRaggedWorstAsBuilt` are untouched.

### λ IS A RULING, NOT A DERIVATION

```
free end must stand:   D ≥ 27.4100 cm at e = 11.1200  →  λ ≥ 2.4649
the property:          D ≤ 221.5475 cm at e = 57.9608 →  λ ≤ 3.8224
```

| λ | free end (7 × 30) | scenario corbel | property (vs 0.36903) | free end under 40 courses |
|---|---|---|---|---|
| 3.000 | 0.6751 | 0.5991 | 1.62× ✓ | **~0.90 — uncomfortable** |
| 3.232 | 0.5817 | 0.5162 | 1.40× ✓ | ~0.78 |
| **3.464** | **0.5064** | **0.4493** | **1.217× ✓** | **~0.68** |
| 3.696 | 0.4448 | 0.3947 | 1.07× ✓ | ~0.60 |
| 3.928 | 0.3938 | 0.3495 | **0.947× ✗ FAILS** | ~0.53 |

**The window is much narrower than [2.465, 3.822] once the wall the game renders is included, and no fixture covers that wall.** λ must sit at the upper end, and λ = 3.8 leaves the property 1.4% of margin. **That squeeze is the real risk.**

**λ = 3.464 = 2 × 0.866 is the recommendation** — reusing `SolverArchingDepthPerSpan`, BS 5977-1's equilateral triangle applied to the mirror span `2e` of a cantilever, and the only value with two-sided margin. **Read that as a rationalisation, not a derivation.** EN 1992-1-1 §5.3.1(3) calls a member a deep beam when its span is under three times its depth, which as a validity limit gives `λ ≈ 2/3` — and **λ ≈ 0.67 fails the free end by a factor of four.** The free-end ruling is what sets λ.

**And `λ ∝ e` means the free end's margin is now 2× rather than 62×.** `e → 11.25` asymptotically, so `D → 39 cm` and the reading is **linear in wall height**, crossing 1.0 at about 61 courses (λ = 3.464) or 45 (λ = 3.0). A live behaviour change, arguably correct physics, and it must be stated rather than discovered.

---

## Scale invariance and shape

**Every dimension × k.** `M ~ k⁴`, `F ~ k³`, so `e ~ k` and `D ~ k` — **covariant, as a length must be.** `W_c ~ k³`, `σ ~ k` — stress grows linearly with scale, which is Galileo's square-cube law, correct rather than a defect. Nothing references 21.5 × 10.25 × 6.5: `t` comes from `InterfaceHalfExtentCm`, `e` from the solver's own accumulation.

**Brick shape.** Change the bond and `e` follows automatically, because it is measured rather than assumed. That is precisely the user's "a larger overhang requires more bricks on top of it", and it is the one property genuinely emergent here.

**The honest exception:** the *gate* is **not** scale-invariant — `D_min ~ k²` while `D ~ k`, because cohesion is an absolute stress. Physically right (a scaled-up wall loses composite action) and a reason to want the gate eventually.

---

## Overturning — the simplification, judged

**The claim: on an immovable base, global overturning IS the bed joint opening, so there is no second mechanism. THAT IS RIGHT, AND IT CORRECTS TWO DOCUMENTS. BUT THE CONCLUSION DRAWN FROM IT IS WRONG.**

### What holds

A rigid body cannot rotate about a fixed base without separating from it, and separation on a bed plane is that joint opening in tension. Since the model evaluates **every** joint, it implicitly checks **every** candidate free body. What is missing is not a mechanism but a **plane discipline**: `ComputeUtilisation` reads a horizontal bed joint against `σ_c`, a stress on a **vertical** plane.

The base is genuinely immovable — a grounded piece is a root of the reachability walk on its own account. If it were not, the hinge moves up a course and the argument is unchanged; the governing joint is always the lowest bed joint whose upper side is the rotating body.

**So DESIGN.md §3's "a stability check this model has never had" and CURRENT_STATE's "no check that can ever say that corbel projects too far" are both mis-stated.** The check exists; it is being fed the wrong section. Rewrite rather than delete — the *capability* is genuinely absent even though the mechanism is not.

### What does NOT hold — and this is the number

**A composite depth bound does not recover the case-14 capability.** Case 14's bottom rung: `F = 7`, `M = 112.5`, so `e = 16.071` and `λ·e = 55.67 cm` — against `D_avail ≈ 30–37.5`. **`D_avail` binds, the bound never fires, and case 14 reads 0.195160875 exactly as today.** The matched-corbel lemma says why in general.

### Case A, worked — and it deserves its own state

A four-step corbel off a single grounded column. Bearing 10.25 cm, half-extent 5.125. Bottom rung `F = 4`, `M = 90`, hence **`e = 22.5 cm`** — which is also, independently, the four bricks' centre of mass offset from the bearing centroid.

```
bearing outer edge      : 5.125 cm from the centroid
resultant               : 22.500 cm — 17.375 cm BEYOND THE BEARING
patch reading           : 1.23595   (fails)
composite over 4 courses: 0.15613   (stands)
under λ = 3.464         : λe = 77.9 > D_avail 30  →  UNCHANGED at 0.15613
```

**There is no equilibrium solution on that joint at all** — no compressive stress block on a 10.25 cm bearing has its resultant 22.5 cm out — and `ComputeUtilisation` returns a confident 0.156.

**Verdict: RECORD the state, do not BUILD it.** *For:* `EPieceSupport::Stranded` is the exact precedent — "the solver has no rule for this" told apart from "physics says it falls". *Against, and fatal for now:* `e = 41.786 > 5.125` for the staircase and `19.688 > 5.125` for the five-step corbel. **The resultant is outside the bearing on every corbel the user ruled must stand.** The predicate condemns the ruling.

**Which means the ruling has a consequence nobody has written down: a bonded corbel standing is the model declining to enforce statical equilibrium at the bearing.** Defensible — a corbel really is held by the bond into the wall behind, not by the bottom unit's own bearing — but it should be recorded in those words, because it is the actual price of the case-14 ruling, and "we cannot express projecting too far" is a weaker statement than the truth.

### Out of scope, explicitly

The reduction is valid **only** because every load path terminates at a grounded piece that cannot translate. Debris, a fallen slab on rubble, anything free-standing does not have its rotation expressed as any joint opening. Not reachable today (rubble can never bear). **Do not let the simplification be quoted as a general truth once it is.**

---

## What breaks

1. **The free-end anchor moves, and it is the ruling's own fixture.** `0.016046882393370248 → ~0.5064`. Re-derived: `0.130117 × 43.2742²/481.210`. The ruling survives by **1.97× instead of 62×**.
2. **The scenario corbel's whole ladder moves**, bottom rung `0.223001 → 0.4493`. The row is `Unasserted` precisely so this can be decided; keep it so until slice 3.
3. **`MasonryDepthAboveCm`'s walk is a CHAIN, not a traversal**, and this bound makes the untested branch matter *more*, not less, since `D_avail` now governs more fixtures. Still no fixture.
4. **Cost goes the right way** — the walk stops early, ~5 steps for a free end, ~27 for the scenario corbel. **But no measurement exists**; the only baseline is 6.4 ms, taken before slice 5. A measurement is owed on the 30 × 40 wall with the raking cut.
5. **`PieceInspection` gains a fourth unexplained thing — and this one a player could actually follow.** `GetConnectionCompositeDepthCm` already exists. Surfacing it is one field and one presenter line: *"this joint is read over 200.8 cm of bonded masonry, not over its own 10.25 cm patch."* **Not a follow-up.**
6. **Both fuzzes stay dark and must.** No geometry → no moment → the composite block is unreached.
7. **ARCHING_DESIGN slice 5's "nothing caps the depth but the wall itself"** becomes false; rewrite in place.
8. **DESIGN.md §3 and CURRENT_STATE** need the overturning correction above.

---

## The regression anchors

**Bit-identical, each with a reason rather than a hope:**

| what | why it cannot move |
|---|---|
| the intact wall entirely — 0.00495 and 0.0036748258197270385 | every seat has `e = 0`, so `M = 0`, so no composite section is measured and the block is unreached |
| both fuzzes, all 20,000 cases | no geometry → no moment → no depth |
| `AStaircaseVoidCondemnsTheCorbel` **0.36903147272727271** | `λe = 144.75` against `D_avail = 82.5`; `D_avail` binds |
| all four original `FCorbelCase` rows — **0.218580**, **0.36903147272727271**, **1.2501861088888881**, **1.3634027672024234** | the matched-corbel lemma: `λe ≈ 1.73·D_corbel > D_avail` for every λ > 2 |
| waist **0.058203838191552663**, ragged end **0.0582038382**, `MortarRaggedWorstAsBuilt` **0.0390321745** | the half-seat lemma: `λe ≥ 19.49 cm`, so ≤ 2 courses can never be capped |
| every zero-moment answer, bit for bit | `min(D_avail, λ·M/F)` is never reached with `M = 0` |
| MOMENTS case (b) **0.4157273077** | head joint; the composite block is `BedBeneath`-only |
| slices 1–4 entirely | only `ConnectionCompositeDepthCm` moves |
| dry stone, 11 steps laid dry | condemned by `TensileStrengthMPa == 0.0` at any section |

**May legitimately move, each re-derived rather than re-tuned:** the free end (`0.0160469 → ~0.5064`); every rung of the scenario corbel; `Acceptance.Wall.Catalogue`'s pass count **only if a catalogue wall is taller than its cut — check before slice 1, because case 20's staircase is cut into a 13-course wall and should not move**; `Visual.StaircaseScreenshot`, which is invisible to the documented command behind `NonNullRHI`.

---

## Slices, red-test-first

**1. A corbel is not helped by masonry it has no part in.** PART 1B of `ACorbelResistsWithItsWholeDepth`, already written and already red. Cap `EnoughDepthCm` at `λ·M/F` and pass it to `MasonryDepthAboveCm` instead of `Max()`. Red today at ×0.604; green after at **0.4493**, ratio **1.217**. Guard rows bit-identical: the four original rows and the intact wall on both walls. **One argument at one call site.**

**2. The free end still stands, and now for a reason you can see.** PART 2 re-derived — assert the identity `0.130117·F²/M` rather than the literal, because the identity survives a change of λ. Plus **a row this project does not have: the same deletion in a 40-course wall**, expecting ~0.68. **That row is what decides λ, and its absence is why the window looked wider than it is.**

**3. Rule on λ**, and turn the scenario row from `Unasserted` to `MustStand`. Not a code slice — a ruling, made with the table above in hand. **Do not do this before slices 1 and 2.**

**4. The readout can explain the number.** `FJointInspection::CompositeDepthCm`, held with exact `==` against the accessor, and one presenter line. **Not optional.**

**5. *(Deferrable, needs its own fixture first.)* Composite action requires shear transfer.** Add a test-only frictional profile with zero cohesion and non-zero tensile strength — a row, not a branch — and lay the staircase in it. Then implement the gate. **Confirm first that the mortared fixtures are untouched (measured at 0.1278) or the gate is silently doing something else.**

**Stop at slice 4** unless slice 5's fixture is wanted.

---

## What not to do

- **Do not implement the shear-flow rule as a depth cap.** It is a floor. 0.1278 on the scenario wall — today's defect wearing a citation.
- **Do not break bed joints in composite shear yet.** A broken bed joint stops bearing, so a delamination would drop the wall rather than shorten a section.
- **Do not add a `CompositeDepthCourses` or a `bDevelopsCompositeAction` to any profile.** Everything needed is `M`, `F`, `t` and the existing walk.
- **Do not cap the depth by the cut's own height.** A single deleted brick is a one-course cut and reads **6.05**.
- **Do not use the clear projection instead of `M/F`.** Linear in the projection is *infeasible*: the free end needs `λ ≥ 2.436` and the scenario needs `λ ≤ 0.895`. `M/F` works because it is the *statical* arm.
- **Do not fold overturning in.** Same joint on a fixed base, no new mechanism, and **not recovered by this bound**.
- **Do not relieve only the tension edge**, scale the moment instead of the section, or credit a lone brick — ARCHING_DESIGN slice 5's three warnings stand.
- **Do not tune `f_xk1`, `f_vk0` or `0.866`.**

---

## Verified versus assumed

**Worked through and reproduced to the last digit against the fixture's printed values:** `0.36903147272727271` from `1608.75` over `t·82.5²/6`; `1.2501861088888881` from `F = 540`, `M = 91209.375`; the bed patch being `t·D²/6` at `D = t` (179.4817708333 both ways); `M_tall = 11601.5102` and `e = 57.96075` back-derived from 0.22300137936935951; `D_req = 27.4100` and the ceiling `221.5475`, matching the test's printed 27.4 and 221.55; the bed-patch-alone **167.33** against the printed 167.32; `e ≈ 3.75k` at k = 5, 11, 45; the identity cross-checked on both fixtures; the shear-flow figures and the dry-stone ladder; the smeared 15.805621 uu/cm² reproducing ARCHING_DESIGN's 15.8056.

**Assumptions a test must confirm:**
- That **45 steps under 67 courses** stays capped by `D_avail` — needs `e₆₇ ≥ 140.7` against `e₄₇ = 168.906`. Almost certainly safe, but it is a "must come down" anchor and `F₆₇` was not measured.
- That the scenario wall's own free end reads ~0.68 under 39 courses. Extrapolated, not measured. **This is the number that decides λ and no fixture presents it.**
- That the property survives a single added course (needs increment arm ≤ `2e`; measured 61.813 vs 83.571 for 13→40, never for 13→14).
- That the half-seat lemma `e ≥ 5.625` holds for every seat geometry the producer can emit.
- That the cost goes down rather than up. Argued, unmeasured, no post-slice-5 baseline.

**Believed and cited, not verified against the source text:** EN 1992-1-1 §5.3.1(3); BS 5977-1's equilateral triangle; EN 1996-1-1 §3.6.2. **Check the first before λ = 2 × 0.866 goes into a comment**, because the comment will be read as a derivation and it is not one.

**The least trustworthy thing here is λ.** A single number chosen inside a window whose ceiling comes from one fixture pair and whose floor comes from a product ruling, with published deep-beam guidance pointing four times stricter. Its *ordering* is solid — a bound proportional to the effective arm is the only form that satisfies both ends, and shear flow provably cannot bound at all. Its value is a ruling, and slice 3 exists to make somebody make it.
