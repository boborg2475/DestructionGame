# Real-world check — which of our assumptions survive published practice

Researched 2026-08-07 against building codes and masonry design guidance. **Every claim below says whether it was verified, refuted, or could not be confirmed**, because this project has cited from memory before and been wrong.

---

## CONFIRMED EXACTLY

### Initial shear strength `f_vk0 = 0.20 N/mm²`

EN 1996-1-1 gives **0.20 N/mm²** for clay units in general purpose mortar (M4 and M6 classes), with a permissible range of 0.15–0.30. `GeneralPurposeMortar.ShearCohesionMPa = 0.2` is right, and so is the Mohr-Coulomb form it sits in.

**This is the number that corrected my stack-bond arithmetic** — case 18 reads 0.01000825 rather than the 0.0200 I first quoted, because I had divided by the *tensile* figure.

### The shear cap `0.065·f_b`

Characteristic shear strength "is limited to not greater than `0.065·f_b`". `MaxShearStrengthMPa = 1.3` against a 20 MPa unit is exactly `0.065 × 20`. The comment in `ConnectionProfiles.cpp` is correct as written.

---

## COULD NOT CONFIRM — and it is the most load-bearing number we have

### Flexural strength `f_xk1 = 0.10 N/mm²`

**Not verified.** The full Table 3.2 is behind the standard and the PDFs found were unparseable. One source referenced a UK National Annex table giving **`f_xk1 = 0.167 N/mm²`** for a case it did not fully qualify — which, if it applied to our unit and mortar, would make every tension reading in the project **1.67× too pessimistic**.

`f_xk1` divides the peak tension in every joint we compute. It is the single number most worth confirming from the standard itself. **Do not change it on the strength of one unqualified search result** — but do not treat 0.10 as verified either.

---

## CONTRADICTED — and this bears directly on the case-14 ruling

### Corbelling limits

Published limits, consistent across US model codes and UK practice:

- **Per course:** the lesser of **one-third the unit bed depth** and **one-half the unit height**.
- **Total projection:** not more than the **wall thickness** (UK traditional practice), or **one-third the wall thickness** (US IRC), depending on wythe construction.
- Arching/corbelling assumes **solid masonry** and an **overlapping bond**.

For this project's brick — 21.5 × 10.25 × 6.5 cm:

```
per course  = min( 10.25/3 , 6.5/2 ) = min(3.417, 3.25)  =  3.25 cm
total       = 10.25 cm
```

Against what the fixtures actually do:

| | code allows | we use | over by |
|---|---|---|---|
| step per course | 3.25 cm | **11.25 cm** (half a cell) | **3.46×** |
| total projection, case 14 (4 courses) | 10.25 cm | **45 cm** | **4.4×** |
| total projection, the staircase (11 steps) | 10.25 cm | **123.75 cm** | **12×** |

**So published practice says case 14 and case A both fail, decisively — and the staircase fails by an order of magnitude.**

### And the angle is the tell

"Projection per course ≤ half the unit height" is a statement about *slope*: the corbel face may advance at most `h/2` for every `h` of rise, which is **at least about 63° from horizontal**.

Our half-cell step advances 11.25 cm per 7.5 cm course — **33.69° from horizontal**. That is exactly the stepping-front angle seen in game when a single brick was deleted. **The cascade's natural angle is one that real corbelling forbids**, and by roughly a factor of two in slope.

### How much weight this deserves

**Not unlimited.** These are *design* limits carrying safety factors, for load-bearing masonry that must never fail — not collapse predictions. A corbel at 1.5× the limit very likely stands. At **4.4×**, standing is a much harder claim, and at 12× it is not credible.

**It does not override the ruling — the user made it and it is theirs — but it was made without this information and that is worth saying.** The strongest version of the counter-argument is that the fixtures are not really corbels in the code's sense: a raking cut through a *wall* leaves masonry bonded behind the projection, which is not the free-standing corbelled ledge the code limits address. That is a real distinction and it is exactly what composite action models. It is also the argument that has to be made explicitly rather than assumed.

---

## PARTLY CONFIRMED, WITH A SHAPE WE DID NOT IMPLEMENT

### Arching over an opening

Published: arching action occurs **provided an overlapping bond pattern is used and the masonry above the apex of a 45° isosceles triangle over the opening exceeds 300 mm**. BS 5977's interaction zone is a 60° triangle with a base of **1.1 × the clear span**. Practice advises using the **45°** triangle rather than 60° because it is conservative.

Two mismatches with `ARCHING_DESIGN.md`:

1. **We use a smooth cap, `d_e = min(cover, 0.866·L)`. The published rule is a THRESHOLD** — arching either happens or it does not, gated on 300 mm of cover above a 45° apex. Our slice 4 grades continuously where practice switches. The continuous form is defensible and arguably better behaved for a game, but the design should say it diverges rather than implying it follows BS 5977.
2. **0.866 is the 60° figure.** Practice prefers 45° as the conservative choice, which would make the constant `0.5`. Our slice 4 uses the *less* conservative angle.

### Effective span

"The effective span is the clear span of the opening plus the depth of the beam." Our slice 4 measures **abutment centre to abutment centre**, which is close but not identical. Worth a note; unlikely to matter at our tolerances.

---

## SUPPORTS λ, WITH A CAVEAT WORTH KNOWING

### Deep beam definitions

- **EN 1992-1-1:** a member is a deep beam when its effective span is **less than three times** its overall depth. *(Confirmed — this is the clause `COMPOSITE_DEPTH_DESIGN.md` cites.)*
- Other standards: span/depth **< 2.0** simply supported, **< 2.5** continuous.
- For masonry beams specifically: **"the beam's depth may be limited by the height of the wall above an opening"** — which is exactly what `MasonryDepthAboveCm` walks.

**The caveat that matters**: the deep-beam definition bounds *when beam theory applies*, not how much depth may be credited. Reading it as a cap gives `λ ≈ 0.67`, which the design already notes fails the free end by a factor of four.

**But there is a defensible reading that lands close to our λ.** A cantilever's structural analogue of a simply-supported span is the **mirror span, `2 × projection`**. Applying "depth may be taken up to the span" to that mirror span gives `D ≤ 2 × projection`. Since for a distributed corbel load the effective arm `e ≈ projection/2`, our `λ·e = 3.464·e ≈ 1.73 × projection` sits **just inside** that limit.

So **λ = 3.464 is defensible as slightly stricter than the mirror-span deep-beam limit** — a better justification than "2 × 0.866", which is a coincidence of two unrelated constants. **Rewrite the comment to say this instead.**

---

## What to do about it

1. **Confirm `f_xk1` from EN 1996-1-1 Table 3.2 directly.** It divides every tension reading in the project and it is currently unverified.
2. **Re-justify λ as the mirror-span deep-beam bound**, not as `2 × SolverArchingDepthPerSpan`. Same number, honest derivation.
3. **Record that slice 4 diverges from BS 5977** — continuous where practice is a threshold, and 60° where practice advises 45°.
4. **Put the corbelling limits in front of the user.** The case-14 ruling stands unless they change it, but they ruled without knowing that half-brick-per-course is 3.46× the per-course limit and that the staircase is 12× the total. The counter-argument — that a raking cut through a bonded wall is not the free-standing ledge the code addresses — is real and should be made explicitly rather than left implied.

---

## Sources

- [Corbelling — UpCodes](https://up.codes/s/corbelling)
- [Corbel Projection — UpCodes](https://up.codes/s/corbel-projection)
- [IRC 2015 R606.5.2 Corbel projection — ICC](https://codes.iccsafe.org/s/IRC2015/chapter-6-wall-construction/IRC2015-Pt03-Ch06-SecR606.5.2)
- [Corbelling Brickwork: Projection Limits and Rules](https://squote.app/knowledge/brickwork/corbelling)
- [Unreinforced masonry: shear loading — JRC Eurocodes](https://eurocodes.jrc.ec.europa.eu/sites/default/files/2022-06/3_am_Jaeger.pdf)
- [Eurocode 6 — JRC](https://eurocodes.jrc.ec.europa.eu/sites/default/files/2022-06/3_pm_Roberts.pdf)
- [Introduction to Eurocode 6 — Brick Development Association](https://www.brick.org.uk/uploads/downloads/d-eurocode-6-masonry-introduction.pdf)
- [Types of Design Loads for Masonry Lintel — The Constructor](https://theconstructor.org/practical-guide/loads-masonry-lintel-calculation/15551/)
- [Lintel loading method: overview of BS 5977 — Stressline](https://www.stressline.net/lintel-loading-method-overview-of-bs-5977/)
- [Technical Notes 17B: Reinforced Brick Masonry Beams — BIA](https://www.gobrick.com/media/file/17b-reinforced-brick-masonry---beams.pdf)
- [Design of Deep Beams — Structville](https://structville.com/2022/03/design-of-deep-beams.html)
