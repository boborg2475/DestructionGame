# Composite depth — how far up a wall deep-beam action reaches

**Status: THE ARM CAP AND ITS FLOOR ARE BOTH BUILT (2026-08-07), and their two moved anchors are re-derived. Uncommitted.** `ACorbelResistsWithItsWholeDepth` is green, `ACorbelReadsItsOwnStepSize` is green, and the suite stands at **138 tests, 4 failures** — three pre-existing `Acceptance.Wall.*` rows and `CorbelStepsBeforeTensionWins`, whose red row is a *finding about the model* (the counterweight buys nothing: case D crosses at 36 steps and case C at 36, identically) rather than a wrong expectation. **Slice 3 — confirming λ — is the only step of this design left.** Read [DESIGN.md §2 and §3](DESIGN.md), all of [MOMENTS_DESIGN.md](MOMENTS_DESIGN.md) and all of [ARCHING_DESIGN.md](ARCHING_DESIGN.md) — in particular slice 5 and the ⚠ block under its depth table — before this. This sits directly on slice 5 and revises one of its claims, one of CURRENT_STATE's, and one of DESIGN.md §3's.

**THE HEADLINE, SO NOBODY READS PAST IT.** Three mechanisms were worked through with numbers. **The two that are physically honest do not bound the depth, and the one that bounds it is not derived from a published figure.** That is the whole difficulty and it is stated up front rather than discovered in slice 3.

| candidate | bounds mortar? | dry stone for free? | verdict |
|---|---|---|---|
| shear flow `q = VQ/I` as a depth cap | **NO — it is a FLOOR, not a ceiling** | yes, exactly | **rejected as the bound**, kept as a gate |
| apply the composite shear and let the cascade break joints | **NO — reads 0.128 of capacity, nothing breaks** | yes, exactly | **rejected now**, right direction later |
| ~~`D = min(masonry above, λ·M/F)`~~ | yes — **but it inverted step-size ordering** | no | **superseded**: it capped the whole depth where only the wall above should be capped |
| `D = min(masonry above, max(cut's own depth, λ·M/F))` | yes, and only where it should | no — inert for dry stone | **ADOPTED AND BUILT**, λ unchanged, **no new constant** |
| the cut's own height *alone* | yes | no | **infeasible — a 57% empty window** between the free-end floor (λ ≥ 4.229) and the property ceiling (λ ≤ 2.6854) |

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

~~Second lemma: **any half seat has `e ≥ 5.625 cm`**, so `λ·e ≥ 19.49 cm ≈ 2.6 courses`. **No joint with two or fewer courses over it can be touched** — which is why the waist, the ragged end and `MortarRaggedWorstAsBuilt` are untouched.~~

> ### ⚠ THE HALF-SEAT LEMMA IS FALSE, AND IT COST AN ANCHOR. *(Measured at slice 1, 2026-08-07.)*
>
> `MortarRaggedWorstAsBuilt` moved **0.0390321745 → 0.0455104479**. The solver prints that joint carrying **1.5 brick weights and 5.625 brick-weight-cm**, so **`e = 3.75 cm`, not 5.625** — because the extra half brick weight arrives **centred**: `F` grows and `M` does not. The lemma assumed every increment arrives at the half-seat eccentricity, and a centred one does not.
>
> `λe = 12.99 cm` then trims the two-course walk to 1.73 courses, whose 288.3 cm³ section is deeper than the patch but whose 0.0052044 MPa exceeds the patch's 0.0045510 — so `ComputeUtilisation` refuses the relief and the patch reading stands. **The rule is being applied faithfully; only the lemma justifying the anchor was wrong**, and the direction is conservative (it reads higher).
>
> **The waist and the ragged end are still untouched**, verified by measurement rather than by the lemma. What is gone is the general guarantee, so any future "this shallow joint cannot be capped" claim has to be measured rather than argued.

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

**λ = 3.464 is the recommendation, and it is `2√3` = `4 × 0.866`, NOT `2 × 0.866`.** *(Arithmetic slip corrected at slice 1: `2 × 0.866 = 1.732`. Every figure in this document — the table, `K = 1.56129/λ² = 0.130117`, 0.4493, 0.5064 — requires 3.464, confirmed by back-solving the scenario reading at `λ² = 11.9992`. The prose had slipped between λ and the `λ/2` its own matched-corbel lemma uses; only the label was wrong.)*

**And the honest justification is the MIRROR SPAN, not the arching constant** *(from `REAL_WORLD_CHECK.md`, 2026-08-07)*. A cantilever's structural analogue of a simply-supported span is `2 × projection`. Masonry beam practice takes the effective depth up to the span, so `D ≤ 2 × projection`; and since a distributed corbel load gives `e ≈ projection/2`, `λ·e = 3.464·e ≈ **1.73 × projection**` — **just inside** that limit. That is a derivation. "It is a multiple of `SolverArchingDepthPerSpan`" is a coincidence of two unrelated constants and should not be written as though it were a reason.

**What it is still NOT:** EN 1992-1-1 §5.3.1(3) calls a member a deep beam when its span is under three times its depth, which read as a *cap* gives `λ ≈ 0.67` — and **that fails the free end by a factor of four.** That clause bounds when beam theory applies, not how much depth may be credited. **The free-end ruling is what ultimately sets λ**, and slice 3 exists so somebody makes that call knowingly.

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
| all four original `FCorbelCase` rows — **0.218580**, **0.36903147272727271**, **1.2501861088888881**, **1.3634027672024234** | **the floor is the cut and the cut is the wall**, so `D = D_avail` by construction. *(This used to cite the matched-corbel lemma, which is false — see the ⚠ block below.)* |
| waist **0.058203838191552663**, ragged end **0.0582038382**, `MortarRaggedWorstAsBuilt` **0.0390321745** | the half-seat lemma: `λe ≥ 19.49 cm`, so ≤ 2 courses can never be capped |
| every zero-moment answer, bit for bit | `min(D_avail, λ·M/F)` is never reached with `M = 0` |
| MOMENTS case (b) **0.4157273077** | head joint; the composite block is `BedBeneath`-only |
| slices 1–4 entirely | only `ConnectionCompositeDepthCm` moves |
| dry stone, 11 steps laid dry | condemned by `TensileStrengthMPa == 0.0` at any section |

**May legitimately move, each re-derived rather than re-tuned:** the free end (`0.0160469 → ~0.5064`); every rung of the scenario corbel; `Acceptance.Wall.Catalogue`'s pass count **only if a catalogue wall is taller than its cut — check before slice 1, because case 20's staircase is cut into a 13-course wall and should not move**; `Visual.StaircaseScreenshot`, which is invisible to the documented command behind `NonNullRHI`.

---

## Slices, red-test-first

**1. A corbel is not helped by masonry it has no part in. BUILT 2026-08-07.** PART 1B green at **0.44942108329043645**, ratio **1.217839443256848**. All four original `FCorbelCase` rows, both intact walls, the waist, the ragged end, MOMENTS case (b) and both fuzzes bit-identical; `Acceptance.Wall.Catalogue`'s pass set unmoved.

> **PASSING `λ·e` AS THE WALK'S LIMIT IS NOT ENOUGH — IT MUST BE CLAMPED AS WELL.** `MasonryDepthAboveCm` stops once it *has* enough, so it returns the first whole course count that **reaches** the limit and **overshoots by up to one course**. Limit-only gives 0.44172 and ratio 1.19697 — green on the one-sided property, and **1.7% permissive with nothing able to see it**. Clamping to `min(walk, λ·e)` reproduces the design's own numbers to four digits. Slice 4 of ARCHING_DESIGN does exactly this with `AngleCappedDepthCm`; the same discipline was needed here and was nearly missed.
>
> **One anchor moved and one lemma died** — see the ⚠ block above. `MortarRaggedWorstAsBuilt` is `0.0390321745 → 0.0455104479` and `World.Push.AWallOverCapacityDoesNotWaitForAClick` is red on it, awaiting re-derivation with slice 2.

> ### AND THE ARM MAY ONLY TRIM THE MASONRY ABOVE THE CUT. **FLOOR BUILT 2026-08-07**, driven by `Core.Structure.ACorbelReadsItsOwnStepSize`.
>
> Applying `λ·e` to the WHOLE depth taxes a corbel for its own height, and the ladder went **U-shaped**: at ten steps 0.1876 for the 3.25 cm code-compliant step against 0.1540 for 5.375 cm, because where the arm caps the section the reading is `K·F/e` and `F/e` FALLS with the step. The rule is now
>
> ```
> D  =  min( masonry above ,  max( the corbelling body's own depth ,  lambda*|M|/|F| ) )
> ```
>
> **λ is unchanged at 3.464 and no constant was added** — `h_body` is a measured length. The corbelling courses GENERATE the moment, are bonded into one cantilevering body and need no shear transfer to be engaged, so they resist with their full depth unconditionally; masonry above the cut is not being bent by that moment and has to be dragged in by shear over a distance, which is what `λ·e` bounds. **The floor can never credit a single course of the wall above the cut, because the floor IS the cut.**
>
> **THE WALK IS NOT `PieceRestingOn`'s CHAIN, AND THIS DOCUMENT WAS WRONG TO SAY IT COULD BE.** Measured: that chain steps from a corbel's stepping front onto the 2.25 cm lap on the course INBOARD rather than onto the 18.25 cm seat under the front, so any predicate ends the body at its second course on every corbel in the suite. `CorbellingBodyDepthCm` walks the corbelling chain instead — the first piece resting here that is itself seated on exactly one course.
>
> **PREDICATE (1) SHIPPED AND (2) DID NOT, MEASURED RATHER THAN ARGUED.** The direction test was built and the whole suite re-run: **byte-identical output, 138 tests**, cases 17 and 18 and `StackBondColumnShearIsHeightIndependent` included — because a stack-bond wall has zero eccentricity at every seat, so no bed joint in one ever carries a moment and the block is never reached. The hazard and the fixture that would be needed are recorded in `CURRENT_STATE.md`.
>
> Ten steps: **0.046620 / 0.099100 / 0.168695 / 0.344813 / 0.667037**. Twenty: **0.062462 / 0.151348 / 0.277994 / 0.603230 / 1.214368** — the two left-hand twenty-step figures come out LOWER than this document's predicted 0.0683 / 0.1706, because the wall caps both at the full 150 cm. Everything else in the suite is bit-identical except `Acceptance.Wall` case 13, printed and unasserted, **0.0772373 → 0.0702368** with its verdict and governing joint unmoved.

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

### ⚠ THE MATCHED-CORBEL LEMMA IS FALSE BELOW A 6.5 cm STEP, AND IT COST THE ORDERING PROPERTY

The lemma said *"for any λ > 2 the bound never fires on a corbel whose wall stops at its top."* It silently assumed the half-cell step, exactly as the half-seat lemma silently assumed every increment arrives eccentric — **defect number nine, same shape as the eighth.**

The true relation is `e = D_corbel·s/22.5`, **not** `D_corbel/2`. So the arm caps a *matched* corbel iff

```
λ·D_corbel·s/22.5  <  D_corbel     ⟺     s  <  22.5/λ  =  6.4954 cm      — k CANCELS
```

which is why the U-shape was identical at 10 and 20 steps. Measured: **0.1876 at 3.25 cm against 0.1540 at 5.375 cm** — a longer overhang reading *safer*.

**And 6.4954 cm is 2.00× the 3.25 cm published per-course limit.** The rule was penalising precisely the geometries closest to code-compliant and leaving the reckless ones untouched. **What is gone is the general guarantee that λ > 2 protects a matched corbel; what replaces it is the floor, which protects it by construction.**

### λ IS NO LONGER THE LEAST TRUSTWORTHY THING, AND ITS WINDOW IS NOW MEASURED ON BOTH SIDES

The 40-course free end was extrapolated when this document was written and is now measured at **0.6740** — the prediction of "~0.68" was right.

```
floor  (the free end must stand at 40 courses):  λ ≥ 3.464·√0.6740  =  2.8438
ceiling (the tall-wall property):                λ ≤ 221.5476/57.9608 =  3.8224
```

**λ = 3.464 sits inside with 22% below and 9% above.** The earlier worry — *"the window is much narrower than [2.465, 3.822] ... λ must sit at the upper end"* — is **measured and wrong**. Slice 3 becomes *confirm 3.464 against the measured window*, not *choose*.

**The least trustworthy thing is now the `h_body` predicate.** Its *value* is a measured length; its *definition* is a rule about which courses belong to the cantilevering body, and **three plausible spellings agree on every fixture this project owns** — which is exactly the condition under which this suite has been wrong before. Predicate (2), the direction test, was built and diffed against predicate (1) across all 138 tests: **zero differing lines**, so it was reverted as uncovered capability. The fixture that would tell them apart is a **stack-bond or single-wythe column loaded eccentrically**, and nothing in the project has one.
