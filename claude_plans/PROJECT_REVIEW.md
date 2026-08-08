# Project review — physics realism, acceptance tests, and the plans folder

*Reviewed 2026-08-08. Three passes: a code-level review of the structural model (every unit conversion,
section identity and guard hand-verified), a full evaluation of the acceptance suite, and fresh web
research against published masonry data. Nothing in the codebase was changed. Every claim below says
where it came from.*

---

## The one-paragraph verdict

The **stress pipeline is physically correct and unusually well-engineered** — force classification,
Mohr-Coulomb, extreme-fibre bending, the kern-limited arching cap, and the unit discipline all check
out by hand, and the NaN/fail-closed hygiene is better than most shipping engineering software. The
**routing layer is where realism is bought on credit**: it is a stack of masonry-specific heuristics
(bed/head tiers, downward-only accumulation, arching groups, composite depth) each patching the hole
the previous one exposed, and the patches now interact. Six of the model's known-wrong verdicts trace
to **one missing mechanism — per-piece equilibrium (force *and moment* balance), i.e. a global
stability check** — and the same absence is what blocks wood, steel, and sideways forces. The
recommended path is not more patches: it is a **rigid-block limit-analysis solve**, built first as a
test oracle against the existing fixtures, then promoted. Separately, the strength data is
systematically conservative: the profiles hold *characteristic design* values, and real mean masonry
bond strength runs roughly **3–8× higher in tension** — which moves every corbel/cantilever verdict
toward "stands longer" and is a calibration decision the user should make once, deliberately.

---

## 1. The published-figures question is now settled (research, 2026-08-08)

REAL_WORLD_CHECK.md called `f_xk1 = 0.10 MPa` "the most load-bearing number we have" and could not
verify it. Verified now:

- **0.10 N/mm² IS the EN 1996-1-1 recommended value** for clay units in general-purpose mortar,
  plane of failure parallel to the bed joints, at *both* mortar classes (fm < 5 and ≥ 5 N/mm²).
  Confirmed against a reproduction of the Eurocode table at
  [RoyMech, Principles of Masonry Design](https://www.roymech.co.uk/Related/Construction/Masonry_design.html).
- **The worrying 0.167 figure does not apply.** It comes from a
  [Structville worked example](https://structville.com/design-of-masonry-walls-en-1996) reading UK NA
  Table NA.6 for **aggregate concrete blocks**, not clay brick. REAL_WORLD_CHECK's caveat can be
  closed.
- **But the UK National Annex replaces 0.10 with much larger values for clay.** Table NA.6 of the
  [UK NA to BS EN 1996-1-1](https://dn710306.ca.archive.org/0/items/bs.na.en.1996.1.1.2005/bs.na.en.1996.1.1.2005.html)
  — built on decades of BS 5628 test data — gives clay brick in M4/M6 general-purpose mortar:

  | water absorption | f_xk1 (M4/M6) | f_xk2 (M4/M6) |
  |---|---|---|
  | < 7% | 0.5 | 1.5 |
  | 7–12% (the common case) | **0.4** | 1.1 |
  | > 12% | 0.3 | 0.9 |

- **And all of these are characteristic (5%-fractile) *design* values**, intended to sit under
  partial safety factors of 2.0–2.7. Mean tested flexural bond strength runs roughly 2× the
  characteristic value again. The same is true of `f_vk0 = 0.20` (cohesion): correct as a
  characteristic value, roughly half of typical mean test results.

**What this means for the game.** A game predicting what *actually* stands should be closer to mean
strength than to design strength: realistically **f_xk1 ≈ 0.3–0.8 MPa** for decent brickwork, not
0.10. Every tension verdict in the project — corbels, cantilevers, free ends — is currently 3–8×
pessimistic relative to real walls. That is *the direction that makes the corbel rulings more
defensible*: corbel A standing at "0.156 of capacity" against 0.10 MPa would read ~0.02–0.05 against
mean strength, i.e. comfortably standing, which matches the intuition that small well-bonded
cantilevers really do hold.

**Recommendation:** do not retune the number quietly — every pinned anchor in the suite moves with
it. Decide once: (a) keep characteristic values and accept "the game's masonry is weak, brittle,
worst-case masonry" as the calibration statement, or (b) adopt mean-strength profiles
(f_xk1 ≈ 0.4, f_vk0 ≈ 0.4, capped per Eurocode form) and re-anchor in one deliberate pass —
ideally *after* the equilibrium oracle (item 4 below) exists, so re-anchoring is one diff, not
twenty hand-derivations. Option (b) is the realistic one. Either way, record the choice in
DESIGN.md §3 and mark each profile row `characteristic` or `mean`.

> **DECIDED 2026-08-08: the user chose (b), mean values, for realism.** The re-anchor runs as its
> own slice after the LP oracle: every strength row re-derived from mean test data with citations,
> every pinned anchor recomputed in one verified pass, and any acceptance verdict that moves gets
> re-evaluated on physics rather than on code thresholds (see the case-11 note in §3). Where a
> verdict-discriminating case stops discriminating under mean strength, a replacement case that
> shows the same mechanism at realistic strength gets added rather than the coverage silently lost.

---

## 2. The physics model — what to keep, what is wrong, where to go

### Keep (verified sound)

- **Units** end to end: `ForceUnitsPerMPaSqCm = 10000` is exactly right; `MassKg × 980` already
  contains 1 N = 100 uu; moments divide by the same constant, so no second boundary is needed. No
  double or missing conversion found anywhere.
- **Classification** (compression/shear/tension against the interface normal), with the PieceB
  orientation convention enforced consistently at every site.
- **Mohr-Coulomb** with the truncated envelope and the `μ = 0 → independent axes` degeneracy that
  keeps fasteners data-not-code.
- **The extreme-fibre/kern machinery**: `W = (4/3)h_u h_v²` is exactly `bd²/6`; the kern test is
  exact; the capped state is precisely the triangular stress block.
- **Thrust arithmetic** `H = W·L/(8r)` with structural ΣH = 0.
- **Numerical discipline**: the NaN-latching comparisons, fail-closed area guards, atomic
  pair+normal emission, bit-stable iteration-order contracts, monotone termination arguments,
  non-destructive solve, break-pass stamps.
- **The profile system** and its class taxonomy. Adding a material is genuinely adding a row.

### Wrong or unrealistic, ranked by severity

1. **No global equilibrium — statically impossible states read as safe.** Corbel A's load resultant
   sits 22.5 cm outboard of a bearing whose outer edge is 5.125 cm out; no equilibrium solution
   exists on that joint, and the model reports 0.156 and stands. Because the model only ever checks
   joints, a rigid body past its tipping point is *inexpressible*. This is the missing mechanism
   behind the corbel ruling's known cost, acceptance case 12's pier, and (partially) cases 10/19/20.
2. **Load routes only downward; tension support does not exist.** Nothing can hang, so a
   counterweight buys nothing (`CorbelStepsBeforeTensionWins`'s finding), buttresses do nothing, and
   the nail/screw withdrawal capacities in `ConnectionProfiles.cpp` are dead data with no reachable
   code path.
3. **Multi-support pieces carry zero moment** (the area split). Exact for symmetric running bond,
   unconservative otherwise; a lintel or floor slab on two walls can never fail in midspan bending.
4. **The one-cell arching moment cap is granted fail-open.** The 114× relief fires on topology
   (`HasArchingAbutment`) but `ApplyArchingThrust` only runs for spanned *groups*, so in the
   one-cell case the equilibrating thrust is never applied to or checked against anything.
   ARCHING_DESIGN itself names this as where "unbreakable" would come from. Small fix; do it soon.
5. **Composite depth: λ = 3.464 is a ruling wearing a constant's clothes**, and the deep beam's
   horizontal shear flow is never applied as a demand on the bed joints that must carry it. The code
   is candid about both. This is the knob the whole corbel behaviour turns on, and it is the one
   number that is neither published nor derived.
6. **Arch thrust neither walks down the pier nor overturns it.** A flat span on one-brick piers
   should shove them over ([BIA Technical Note 31](https://www.gobrick.com/media/file/31-brick-masonry-arches.pdf):
   abutments must resist the thrust, and low-rise arches thrust hardest; the thrust line must stay
   in the abutment's middle third). This is acceptance case 12, and it is the same missing stability
   check as item 1.
7. **Pieces never fail — only joints do.** Masked today because mortar is weaker than brick by data;
   fatal for wood, where the *member* bending failure is the primary mode.

### Generality: wood and steel do not work under the current routing — and the fix is known

The stress pipeline, profiles, section machinery, graph/handle infrastructure and cascade all
generalize; the **routing** (bed/head tiers, downward Kahn accumulation, arching groups, composite
depth, joints-fail-before-members) is masonry baked into the solver. Concretely: hanging joists and
withdrawal loads read Falling with intact joints (nothing can hang); beams on two supports can never
fail in midspan (moment zeroed, and no member check anyway); a portal frame's moment transfer
sideways/upward has no vocabulary at all.

**The current data model — rigid pieces with mass and centroid, rectangular joints with position,
extent, and a Mohr-Coulomb/no-tension/crushing strength triple — is exactly the input to rigid-block
limit analysis** (Livesley 1978; Gilbert et al.; Lourenço's simplified micro-model, which
COMPOSITE_DEPTH_DESIGN itself cites as "essentially FConnection"). Per-piece equilibrium (3 DOF in
2D, 6 in 3D) with joint forces constrained by the existing strength surface, solved as a small
sparse LP. That single formulation **subsumes**: overturning (1), tension support and counterweights
(2), multi-support moments (3), arching/thrust with the kern behaviour *emergent* rather than
patched (4, 6), composite depth with the wall engaging through its real joints (5), cycles (no
accumulation order exists to defeat), and — the DESIGN.md §6 blocker — arbitrary load directions
("beneath" never enters). It stays deterministic, world-free, fast at scenario scale (~1,220 pieces
/ ~3,520 joints is a small LP; warm starts across cascade passes), and every existing anchor number
becomes an oracle case. It needs **only the strengths the profiles already carry** — unlike a
stiffness network, which would demand per-profile stiffness data.

### Recommended evolution path (ordered)

1. **Close the fail-open one-cell arching gate** (apply or capacity-check the thrust). Small.
2. **Interim overturning guard**: free-body check of the maximal bonded body about its bearing
   edge. Explicitly disposable — it exists to stop impossible-states-read-safe until step 4, and the
   leaning-stack acceptance case (below) is its red test.
3. **Build a 2D rigid-block LP as a test oracle only.** Run every existing fixture and anchor
   through it and diff verdicts. This converts "is composite action right?", "is λ right?", and the
   six red acceptance rows from rulings into measurements, at zero risk to the shipping solver.
4. **Promote equilibrium to the cascade authority**; keep the two-tier router as a warm start;
   delete the arching-group and composite-depth heuristics once the diff shows the emergent
   behaviour reproduces the intended anchors.
5. **Signed tension support** falls out of 4 → wood hangers, buttresses, counterweights, and the
   fastener withdrawal data go live.
6. **Member failure** (piece-level bending/crushing against `FMaterialProfile`) — the prerequisite
   for wood beams; the section machinery already exists.
7. **Arbitrary-direction force** (explosions, impacts) — nearly free after 4; the gravity-specific
   router was the admitted blocker.

Steps 1–2 are days; step 3 is the pivotal investment; steps 4–7 each become tractable only after it.

---

## 3. The acceptance suite — evaluation and rewrite plan

### Realism of the twenty verdicts

Verdicts checked against the published arching gate (≥ 300 mm of masonry above the apex of a 45°
triangle on the clear span, per BS 5977 practice) and the corbelling limits already in
REAL_WORLD_CHECK.md. Sound as encoded: **1, 2, 3, 4, 5, 6, 9, 10, 15, 16, 19** (case 3's verdict is
right but the model reaches it by a 2.1 m composite section, which is not the real mechanism).
Flagged:

- **Case 7** (4-cell opening, 8 courses over) — *marginal*. It fails the published arching gate
  (150–205 mm above apex vs 300 required); this is exactly where practice inserts a lintel. It
  probably stands, but it is the standing half of **three** matched pairs, so its uncertainty
  leaks. The lintel pair (below) is the answer.
- **Case 8** — *class sound (local loss), named set doubted*. The 2-brick set is a precision claim
  derived from the project's own arithmetic; a bricklayer would expect the whole unsupported course
  to go. Needs physical evidence, not more derivation.
- **Case 11** (wall on 3-cell piers, 6-cell span) — *RESOLVED 2026-08-08, twice, and the second
  ruling is the keeper: it STANDS.* First ruled local-loss on BS 5977's 300-mm-above-apex gate;
  the user then directed that published design rules not be treated as gospel, and the genuine
  physics agrees with them: the ~120 kg panel carried by the 60-cm-deep bonded wall above
  (span/depth ≈ 2.3, deep-beam territory) puts ~0.04 MPa of bending on the worst joint — 5–10% of
  mean bond strength, under half of even the conservative 0.10 — and the 66-cm piers take the
  thrust easily. The 300-mm rule is never-even-crack design conservatism, not a collapse
  predictor; real walls bridge spans like this routinely. The local-loss ruling **has been
  reverted**; the improved matched-pair claim and the geometry derivation from that pass are kept
  (`Acceptance.Wall.Catalogue` is back to six red rows and case 11 is green). The genuine
  "no room to arch" discriminator remains case 8 (one course of cover), which stays red.
- **Case 12** — *verdict sound, pair broken*. As encoded it varies span (6→10 cells) **and** pier
  width (3→1) at once — contradicting the file's own one-variable-per-pair rule — and it
  near-duplicates case 9. **Rewrite as a 6-cell span on 1-cell piers with a survivor region**, so
  it isolates pier width. The real failure mechanism is the pier overturning under thrust
  (physics item 6), not the span.
- **Cases 13/14** — the corbel ruling. Internally consistent and honestly recorded; note the
  published limits cited against 13 (stepping along the wall's *length*) don't cleanly apply, since
  those limits bound out-of-plane face corbelling. With mean-strength profiles (§1) both verdicts
  become more defensible.
- **Case 17** (stack bond intact) — *zero discriminating power*: an intact wall routes only through
  bed joints, identical to case 1. Drop, or repurpose as the **wide stack-bond removal**
  WALL_CASES.html already names (a hanging column n bricks across still hangs on two head joints,
  so utilisation grows with n while running bond arches — a real bond discriminator).
- **Case 18** — verdict fine ("stands if well built"); the height-independence *property* test
  belongs at Core level on a cheap 3-column fixture (CURRENT_STATE already concluded this).
- **Case 20** — *class defensible, named 2-tooth set doubted*. The cut is ~twice as steep as the
  Core staircase fixture (22.5 cm oversail per course vs 11.25) — do not assume the composite
  relief that saves one saves the other. Real answer is more than 2 bricks, fewer than the model's
  69. Physical evidence would settle it.
- **Case 6** — keep, and re-justify: it is **the only opening in the set that meets the published
  arching gate** (420 mm above apex). Saying so turns 6/7 into a genuine threshold pair.

### Test-quality holes (all cheap, fix first)

1. **Six rows can pass while shedding joints.** `bIntactStood` is only asserted for rows that cut
   something; the no-cut rows (1, 13, 14, 15, 16, 17) never assert `IntactPasses == 0`.
2. **Two tautologies**: the `Result.Worst < 1.0` assertions after `SolveAndBreak` cannot fail by
   construction, but read as though they confirm the verdict.
3. **`Stranded` is folded into the verdict** with no per-row check, so a verdict can be decided by
   the solver declining to divide load round a loop rather than by physics. Add `Stranded == 0` as
   a precondition on all rows (the staircase test already does exactly this).
4. **No regression net over the six red rows.** A change that makes 8/9/10/12/19/20 *worse* is
   invisible. Copy `CorbelBuilderTest`'s pattern: a characterisation anchor per red row pinning
   what the model does *today* (e.g. "case 20 drops 69"), separate from the aspiration.
5. **`KnownDisagreements` is a hand-maintained list** and its tripwire misdiagnoses a genuine
   regression as caption drift. Split the message: "set grew" (regression) vs "set changed shape".
6. Cosmetics: header says "nineteen" ×4 and "FIVE MATCHED PAIRS" where there are four; case 13's
   0.0702 is unpinned; the `Isolates` field is print-only.
7. Cost: the twenty walls are laid ~60× and solved ~50× per run; caching one solve per case would
   roughly halve it.

### The right division of labour

The mechanism ladders (cover, span, corbel step, free-end height) already live at the unit layer,
better than the acceptance pairs that duplicate them. **The acceptance set should become purely the
real-world verdict oracle on configurations the unit layer has no fixture for.** New cases,
prioritised:

| P | Case | Expected real-world verdict | Why |
|---|---|---|---|
| 1 | **Leaning/offset stack** (2 cm offset per course, at 5/10/15/20 courses) | Stands while the resultant is inside the base; falls when it leaves | The minimal fixture that demands a *stability* check rather than a joint check — the red test for physics items 1/2, and possibly the open composite-depth question |
| 2 | **Compression vs shear capacity ratio** (one joint, two load directions; assert the 50× ratio the mortar profile implies) | n/a — capacity comparison | DESIGN.md §4 calls it "key validation" and nothing anywhere does it. Belongs beside `ConnectionStrengthTest` |
| 3 | **L-corner / return** (brick out at a corner vs at a free end of a straight wall) | The corner reads materially lower — returns are how real masonry stabilises free ends. Assert the *relation*, not an absolute | The solver has never seen two orthogonal joint families |
| 4 | **Lintel over case 7's opening** (with/without) | With: STANDS, high confidence | The pair case 7 actually needs, and the model's first spanning member |
| 5 | **Dry-stone acceptance rows** (intact; one stone out) | Intact stands; one out → local loss or worse (no bond, nothing arches) | The only configuration where Mohr-Coulomb coupling is first-order; the acceptance set has none |
| 6 | **Two-wythe wall** (same cut, one vs two wythes) | Two-wythe reads about half | The composite `t` term has never been exercised at any value but 10.25 cm |
| 7 | **Eccentric surcharge** (heavy piece on top, off-centre, over an opening) | Tension governs, not crushing (crushing a 12-cell base needs ~126 t) | No test anywhere applies a discrete external load |
| 8 | **Progressive removal to a predicted count** (12×12 wall, free-end bottom bricks out one at a time; derive N) | Stands at N−1, falls at N | DESIGN.md §4's headline integration test exists only on a 6-piece toy |
| 9 | **Removal order independence** ({A,B} settled state == {B,A}, bit for bit; pass sequence differs) | Must hold | Not covered; cheap; Core-level |

~~A wood acceptance case is *not yet* worth writing~~ **SUPERSEDED 2026-08-08 — the user ruled, and
a framing exists that makes it worth writing today.** A simply-supported beam laid as collinear
segments whose glue-line connections carry the *member material's* own published strengths (C24
timber, S275 steel) makes member failure expressible as the midspan glue line parting in bending —
no new production API. Three rows: heavy load on wood (beam parts at midspan — deliberately RED,
because the model zeroes the moment on any two-seat piece), light load on wood (stands), the steel
twin under the heavy load (stands — the member-level data-drivenness proof). The pair is the
acceptance anchor for the multi-support moment gap (C3) and, later, true member failure (C7).
`Tests/BeamAcceptanceTest.cpp`, written 2026-08-08.

---

## 4. The plans folder — state assessment

| Document | State | Action |
|---|---|---|
| `DESIGN.md` | Current, authoritative, high quality | Add the fuzz/independent-oracle section it self-admits is missing; record the §1 strength-basis decision when made |
| `MOMENTS_DESIGN.md`, `ARCHING_DESIGN.md`, `COMPOSITE_DEPTH_DESIGN.md` | Current; corrected in place as lemmas fell | Keep as is |
| `REAL_WORLD_CHECK.md` | Current | Close its item 1 with §1 above (0.10 confirmed as EN recommended; 0.167 was concrete block; UK NA gives 0.3–0.5 for clay); its items 2–4 stand |
| `CURRENT_STATE.md` | **386 KB and violating its own charter** — it has become an archive of resolved reasoning with a TODO list embedded in it. The ranked index is good; the entries underneath hold pages of settled history ("measured, was wrong, corrected, superseded") that belongs in the design docs or git | Split: keep the index + genuinely-open items (~a tenth of the file); move settled reasoning into the design doc it belongs to; rely on git for the rest |
| `WALL_CASES.html` | Semi-load-bearing (the acceptance catalogue's source of truth) | Fix "nineteen"×4; note case 20's `falls` region is known-unusable; add the per-row `revised` date convention already proposed |
| Other HTML files | Companions to the design docs | Fine |
| `CODE_TOUR.md` | **Deleted in the working tree, uncommitted**, still referenced by CLAUDE.md and CURRENT_STATE.md | **User decision**: restore (`git checkout -- claude_plans/CODE_TOUR.md`) or delete the references |
| `html/destruction-explainer.html` | **Deleted in the working tree, uncommitted**, while CURRENT_STATE lists *updating* it as an explicitly-requested open item | Same decision — the deletion and the TODO contradict each other |

---

## 5. Decisions that needed the user — ALL RESOLVED 2026-08-08

1. **Strength basis** → **mean values, for realism.** Re-anchor as one slice after the LP oracle;
   see the DECIDED note in §1.
2. **Case 11's verdict** → ruled local-loss on the published gate, then **re-ruled STANDS** when
   the user directed that published design rules not be gospel and the physics was worked
   honestly; see §3. The revert has landed.
3. **Case 12's rewrite** → approved; queued in CURRENT_STATE.md (item 2 of the review queue).
4. **The two deleted files** → intentional; references cleaned up, deletions committed.
5. **Leaning stack + interim overturning guard before the LP oracle** → yes. The guard is
   *interim* because it is a bolt-on special case (gravity-only, hand-picked assembly, its own
   pivot rule — a second referee beside the joint checks); the LP equilibrium solve subsumes it,
   since a body past its balance point simply has no equilibrium solution and falls out naturally
   for any load direction. The guard is built to be deleted when step 4 of the evolution path
   lands.

Also ruled the same day: **the wood/steel beam acceptance pair exists now** (see §3), as
deliberate reds anchoring evolution steps 5–6.
