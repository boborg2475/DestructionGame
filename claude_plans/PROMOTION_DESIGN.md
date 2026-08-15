# Promotion Design — evolution step 4

**Status: RULED 2026-08-14 — all five decisions approved as recommended (§12).** This designs DESIGN §7 evolution step 4 — promoting the rigid-block equilibrium LP from test oracle to production cascade authority. It is a design document, not an implementation. Nothing in it has been built; every number is labelled **MEASURED** (read off the tree at HEAD 74f6488), **DERIVED** (arithmetic on measured numbers), or **ESTIMATE** (a guess, with its basis stated).

**What the user ruled**, each recorded at its decision in §12: **D1** first-crack rows for bonded joints plus the active-set governance rule, no ductility discount in any form, case 21 stays an inverted red with a *computed* bond-mobilised flag. **D2** budget ≤ 100 ms per player action with fail-closed fallback to the router, and Slice 0b is a **hard gate with its kill criterion binding** — if an 84-block region will not reach ~50 ms, synchronous LP authority is off the table and this design changes shape rather than proceeding. **D3** the 2D/3D gap does not block, with the seam kept parametric and out-of-plane refusals loud. **D4** the conservative sequence of §6, deletions one at a time, and case 21 explicitly must not block the other twenty-one rows. **D5** the coarser collapse sequence is accepted and the router stays the strain readout through Slice 5, with the readout-versus-verdict inconsistency logged as a known cost.

**The standing consequence of D1**, worth stating where nobody can miss it: the 2026-08-13 brittleness precondition is satisfied by a principled rule plus a computed carve-out, **not** by making case 21 fall. Zeroing that fixture's jamb cohesion outright still reads λ\* = 4.768, so the discount family is refuted by measurement — and any future proposal to "discount the bond until the disputed row falls" is re-litigating a closed question.

Its two preconditions are the 2026-08-13 brittleness ruling (§8) and latency at scenario scale. Neither is assumed away; §4 and §5 answer them, and both answers include the finding that the obvious form of the answer does not work.

---

## 1. What this document decides, and what it deliberately does not

Step 4 as recorded says: promote equilibrium to the cascade authority; keep the two-tier router as a warm start; delete the arching-group and composite-depth heuristics once the diff reproduces the intended anchors; `FStructure::BreakOverturnedBodies` and `SolverInterimOverturningMeanBondMPa` go with it.

Three things in that sentence are underspecified and this document specifies them. **What "authority" means arithmetically** — λ\* is one number for a whole structure and the cascade needs a per-piece verdict, which §3 answers. **What the diff-against-anchors checkpoint is** — which anchors, at what tolerance, and what happens to one that legitimately moves, which §7 answers. **What has to be true before any of it is safe to run on a player's machine** — §5.

What this document does not decide: it does not re-litigate any §8 ruling, it does not choose the strength basis (that is settled and spent — TRAPS), and it does not design steps 5–7 beyond naming the seams step 4 must leave open for them.

One framing that runs through everything below. The project's recurring enemy is *a wrong answer that looks plausible*. An LP makes that enemy stronger, not weaker: a simplex answers with seventeen digits of confidence whether or not the formulation it was handed means what the person handing it meant. Every gate proposed here exists because the number itself is not evidence.

---

## 2. The shape of the change

Today the cascade is: `SolveLoads` routes load downward through a two-tier support graph, `SolveAndBreak` sweeps every joint against its own capacity and latches those over 1.0, `BreakOverturnedBodies` catches the one topology the joint checks cannot express, and `ApplyResults` releases every piece whose support state is `Falling` or `Stranded`. Four mechanisms decide who comes down: strength, unroutability, stranding, and the interim guard.

After promotion there is one: **does an admissible equilibrium exist, and if not, what moves.** Strength is inside it (the strength rows), unroutability disappears (an LP has no routing to fail), stranding disappears (cycles are ordinary constraints, no accumulation order to defeat), and the interim guard disappears (a body past balance simply has no solution).

The router does not disappear, and this is worth stating positively rather than as a concession. It has three jobs after step 4, all real:

- **The warm start.** A support graph is a nearly-admissible force system; feeding it in as an initial basis is the cheapest lever on pivot path length there is (§5).
- **The readout.** The shipped strain display (`Core/PieceMenu`, the magenta/cyan overlays) is per-joint utilisation. λ\* cannot produce that, and an LP-optimal force system produces a *misleadingly comfortable* one (§3.6). Until something better is measured, the router remains what the player sees.
- **The fallback.** An LP that refuses must fail closed to something that keeps the game playable, and that something is the answer production already gives. *(Written when case 22's family was believed to refuse; Slice 0a closed the two real refusers 2026-08-15 and found case 22 itself answers. The fallback requirement stands on its own — a refusal is always possible and under authority it is a frozen or wrongly-collapsing wall — but no fixture the project owns reaches a phase-2 refusal arm today.)*

So the honest description of step 4 is not "the LP replaces the solver". It is **the LP becomes the authority on verdicts; the router remains the estimator.** The heuristics that get deleted are the ones that exist only to make the *router's* verdicts right — arching groups, composite depth, the one-cell thrust gate, the overturning guard. What survives is the load path itself.

---

## 3. The crux: how an LP answer becomes a break set

### 3.1 Why λ\* cannot name bricks, restated as arithmetic

The 2026-08-12 case-20 ruling records this as a fact ("λ\* = 82.629597 is global and cannot name bricks"), and it is worth seeing why it is structural rather than an artefact of how the oracle happens to be written. λ\* is the objective value of one LP over the whole structure. Its optimality says: at load λ\*·W there exists an admissible force system, and at any larger load there does not. It says nothing about *where* the obstruction is, because the objective has one component and the structure has 1,220 pieces. Two bricks dropping off a 30-course wall and the whole wall going over produce the same shape of answer — the sweep header already records this as "a LOCAL LOSS reads Falls exactly like a collapse".

Reading the optimum harder does not fix this. The primal solution at the optimum is a force system, and it is *admissible by construction* — every joint in it is at or under capacity. Feeding those forces through `ComputeUtilisation` to find "the joint that broke" is vacuous: **at λ\* ≥ 1 no joint reads over 1.0, because the LP is defined as the search for a system in which none does.** This is a trap worth writing into the eventual source comment, because it is the most natural-looking wrong idea in the whole design and it would produce a cascade that never breaks anything while every number on screen looked right.

### 3.2 The reformulation: production asks feasibility, not λ\*

The cascade does not want λ\*. It wants a boolean and a set. Posed directly:

> Is there an admissible equilibrium of this structure under its own dead weight, and if not, which blocks move?

That is a **feasibility problem at λ = 1**, not a maximisation. It is posed with the fields that already exist: `FOracleProblem::bGravityIsLive = false` and no live applied forces, so gravity enters the equality rows as a constant and phase 1 must genuinely run. The oracle's own code comment records the mechanics — a gravity-live problem starts feasible with every equality RHS at zero, which is exactly why **all 58,605 of wall-01's pivots are phase 2 climbing λ from 0 to 272.2** (MEASURED, from the code path plus the pinned pivot count). Under the feasibility formulation that climb does not happen at all. §5 prices this; here the point is a modelling one:

- **λ\* answers a question production never asks.** "How many times its own weight" is the measurement that makes the sweep a good oracle. It is not the verdict.
- **Feasibility separates local from global for free.** A local loss is an infeasible problem whose mechanism has non-zero motion on two teeth and zero everywhere else. The vocabulary problem in §3.1 is a property of the *objective*, not of the LP.
- ~~**Standing is cheap and falling is expensive**~~ — **MEASURED 2026-08-15 (Slice 0b) AND IT IS NOT.** The reasoning was that proving feasibility can stop the moment the phase-1 infeasibility sum reaches zero while proving *in*feasibility needs phase 1 run to optimality. Both halves collapse: phase 1's objective **is** the infeasibility sum, so "the sum reaches tolerance" and "phase 1 is optimal" coincide by construction and there is no optimality-proof tail to skip — the measured saving across eight fixtures is **0.63%** (58 of 9,245 pivots, max 3.85% on any one). And the one infeasible fixture answered in 163 pivots / 0.006 s, the same as its λ\* solve, so falling is not expensive either. The asymmetry this bullet promised does not exist; nothing downstream should be budgeted on it.

λ\* does not go away — it stays exactly where it is useful, in the test oracle. Two questions, two entry points, one assembly and one solver.

### 3.3 The answer: the mechanism is the dual

When the feasibility problem is infeasible, phase 1 terminates with a positive optimum and a dual vector **y**. That vector is not diagnostic noise; by LP duality it is the **Farkas certificate of infeasibility**, and in rigid-block limit analysis the dual of the static (lower-bound) formulation is the *kinematic* (upper-bound) formulation. The correspondence is exact and is the standard result the whole method rests on (Livesley 1978; Gilbert & Melbourne):

| primal row | dual variable | what it is physically |
|---|---|---|
| block *i*'s ΣF_x, ΣF_z, ΣM | (u_x, u_z, ω)_i | that block's virtual translation and rotation |
| a contact's tension row | plastic multiplier | that contact opening |
| a contact's friction row | plastic multiplier | that contact sliding |
| a contact's crushing row | plastic multiplier | that contact crushing |

So the answer to "which bricks" is: **the blocks whose dual triple is non-zero are the blocks that move, and the contacts whose multipliers are non-zero are the joints that give.** That is a per-piece verdict *and* a per-joint break set, from one solve, with the mechanism named rather than inferred.

The cost of extraction is near zero. The revised simplex maintains y = c_B B⁻¹ every iteration (that is what BTRAN computes, 44% of runtime); at termination it is already in hand. The extraction seam is small — `FRevisedState` already exposes `Basis`, `XB` and `BtranScratchSlot`, and the post-solve verification already reconstructs `StructValues` from the basis. **The deleted active-set probe is not needed to do this and re-instrumenting it is not on the critical path**; that probe read the *primal* active set at a λ\* optimum, which is a different question from reading the dual at a phase-1 optimum.

Two honest limits, both of which need naming in the source and testing:

- **Degeneracy makes the mechanism non-unique.** The oracle enters its Bland anti-cycling fallback 170–358 times on a single opening-ladder rung (MEASURED), which is a direct statement that these problems are massively degenerate. Under degeneracy several distinct dual solutions certify the same infeasibility, and the simplex returns whichever its deterministic pivot order reaches. The answer is *reproducible* — the determinism contract guarantees that — but reproducible is not the same as unique-in-physics, and a design that quietly conflates the two will one day name the wrong two bricks with total confidence. Mitigations in §3.5.
- **The mechanism is instantaneous and unordered.** It names the whole collapse at once. Today's pass stamps are the record of *sequence* — DESIGN §5.6 says explicitly that the stamps are what a visualisation plays back. §3.4 works out what the cascade becomes and what that costs.

### 3.4 What the cascade becomes

Today: solve, break everything over capacity, re-solve, repeat. A bottom-course delete was measured at 31 passes. Under LP authority the loop is:

1. Solve the feasibility problem on the current graph.
2. Feasible → nothing comes down; the structure stands; stop.
3. Infeasible → extract the mechanism; release every block it moves; sever every contact it opens or slides; stamp them all with this pass number; go to 1.

Termination is the same structural argument as today's: the released set is non-empty on every counted pass (a mechanism with no moving block is not a mechanism), pieces never come back, so there can be no more passes than there are pieces. The expected pass count is *much* lower than today's 31, because one mechanism names a whole collapsing region where the joint sweep discovers it a course at a time. **ESTIMATE: 2–5 passes for a typical delete**, on the reasoning that the mechanism is the complete instantaneous failure and the re-solve exists only to find what the remainder cannot then carry.

The recorded cost: **collapse sequence gets coarser.** Today thirty-one stamps describe a stepping failure walking across a wall; tomorrow three describe the same collapse in three groups. That is a visible product consequence, not just a bookkeeping one, and it is on the user-decision list (§12). There is a mitigation available later — the mechanism carries relative *magnitudes* of motion, so a playback could order within a pass by how far each block moves — but it is speculative and should not be designed in now.

### 3.5 The options that were considered and not chosen as primary

**Per-region interrogation** (delete a candidate piece, ask whether the remainder is feasible, repeat). It gives an unambiguous per-piece answer and is immune to the degeneracy worry. It costs one solve per candidate piece, which at scenario scale is 1,220 solves per pass. Dead on latency by three orders of magnitude. **Kept for one narrow job**: settling the local-versus-global question on a *specific* disputed row — case 20's "more than 2, fewer than 9" is exactly a question you can afford to ask nine times in a test and never in a frame.

**Minimum-violation (goal-programming) LP.** Add a non-negative slack to every strength row with a penalty in the objective; minimise total violation at λ = 1. If the structure stands the answer is zero; if not, the joints with positive slack are the over-stressed set with magnitudes. It is one LP, it degrades gracefully, and it produces something the mechanism dual does not: **a per-joint number, which is what the readout wants.** Why it is not primary: the penalty weights are a modelling choice, and a weighted L₁ objective picks *a* sparse violation set rather than *the* violation set — a knob that decides which bricks get named is exactly the "ruling wearing a constant's clothes" the project already regrets once (λ = 3.464). Recommendation: build it later, as the readout's source and as a cross-check on the mechanism, not as the authority.

**Hybrid — the LP rules on stability, the joint checks name the pieces.** Cheapest, lowest risk, and it is what the interim guard already is in miniature. It is rejected as the *destination* because it cannot deliver the step: the joint checks only name the right pieces because arching groups and composite depth exist to make them, so the heuristics cannot be deleted and gaps 2, 3 and 7 stay open. It is however an excellent *waypoint*, and §6 sequences it as one.

**LP forces through `ComputeUtilisation`.** Vacuous at λ ≥ 1 — §3.1. Not an option, and worth a comment at the seam so nobody re-invents it.

### 3.6 The verification gate becomes two-sided, and that closes an open seam

The oracle verifies its answer today by checking the primal solution against the original assembly rows, and refuses if it violates one — a gate proven to bite by mutations S6 and S5+S6 (MEASURED). That gate can only see one direction: **admissibility**. CURRENT_STATE records the consequence as an open seam — "one seam where λ could read LOW rather than refuse" — and notes that under-report is the direction verification cannot catch.

Under production authority the polarity of that seam gets much worse. A λ read low, or an infeasibility declared on a feasible problem, is no longer a test refusal: it is **a wall that falls down in front of the player for no reason**, un-reproducible from the physics and invisible to every existing check.

The reformulation closes it, and cheaply. When phase 1 reports infeasible it hands back a dual vector y, and infeasibility is *provable* by Farkas: yᵀA ≤ 0 componentwise on the constraint columns while yᵀb > 0 certifies that no admissible force system exists, arithmetic that costs one pass over the sparse matrix — the same order as the admissibility check already performed. So:

- feasible → verify the force system is admissible (exists today);
- infeasible → **verify the Farkas certificate** (new);
- either verification fails → refuse, fail closed to the router, log.

This is the single highest-value small piece of the design. It turns "the LP said so" into "the LP proved it, and the proof was checked", in both directions, and it makes an ill-conditioned basis produce a refusal rather than a spurious collapse.

### 3.7 What happens to the release rule

`FStructureBinding::ApplyResults` today releases on `Falling` or `Stranded`, and its own comment argues correctly that branching on the difference "would amount to claiming a stranded brick should hang in the air". Under LP authority both enumerators stop being release criteria: a piece with no downward route is not falling if the LP finds a force system that carries it, and that is precisely the arithmetic that closes rows 10 and 19. **Release becomes: the mechanism moved it.**

This is a genuine behaviour change at the world seam and needs its own red. It also retires an item that has been sitting in CURRENT_STATE for a long time: the absent cycle-division rule (§5.1) stops mattering, because there is no accumulation order to be defeated by a loop. `StrandsToday = 3` and `= 6` on rows 10 and 19 go to zero, which is what those pins were written to await.

---

## 4. Brittleness — the 2026-08-13 precondition

### 4.1 What the ruling requires

The ruling: case 21's Collapse verdict stands, the LP's dissent is booked as a rigid-plastic scope limit, and *"evolution step 4's promotion design must carry a brittleness answer — a first-crack/ductility discount, a cohesion-mobilisation rule, or an explicit scope carve-out — before an LP verdict overrides a cohesion-bound catalogue row."*

The ruling was made on 2026-08-13 against characteristic-basis data, where zeroing the jamb bed joints' cohesion collapsed λ\* from 5.511 to 0.785 — the cohesion *was* essentially the whole of the disagreement. **That attribution did not survive the mean re-anchor**, and this is the first thing the design has to say.

### 4.2 The measurement that kills the discount family

MEASURED, at the mean basis, on the wall-21 family (`OracleSlowSweep.RigidBlock.OpeningStrengthProbes`, pinned):

| probe | λ\* | ratio to control |
|---|---|---|
| control (case 21 as built) | 17.23854 – 17.23923 | 1.0000 |
| **jamb bed cohesion zeroed outright** | **4.76789 – 4.76808** | **0.2766** |
| cover head tension ×0.5 | 16.76947 – 16.77014 | 0.9728 |
| cover head tension ×2 | 17.90343 – 17.90415 | 1.0676 |

Read that second row as an upper bound on what any brittleness treatment of cohesion can achieve. **Taking the jamb's cohesion to exactly zero — an infinite discount — leaves λ\* at 4.768, still 4.8× over the line.** No first-crack factor, no ductility discount, no mobilisation rule applied to cohesion can bring case 21 below 1.0, because 72% is the *whole* of what cohesion is buying and the remaining 27.7% is friction and compression, which are not brittle and which a brittleness argument has no standing to discount.

A uniform discount across all bond terms fails from the other end. To flip case 21 you need a divisor of 17.24. Applying that to the pinned λ\* of every other slow row (DERIVED — arithmetic on pinned values, and note per TRAPS that scaling a λ\* is only valid where the discounted term is the binding one, so these are indicative rather than predictions):

| row | λ\* today | ÷17.24 | verdict after |
|---|---|---|---|
| wall-21 (the target) | 17.239 | 1.00 | knife edge — barely achieves the goal |
| wall-19 (ruled STANDS) | 47.963 | 2.78 | survives |
| corbel C | 17.970 | 1.04 | knife edge |
| corbel A | 20.333 | 1.18 | knife edge |
| beam row 1 | 2.6461 | **0.153** | falls |
| beam rows 2/3 | 28.801 / 18.299 | 1.67 / 1.06 | knife edge |

A rule that has to be tuned to 17× to move one row, and that puts four other rows on a knife edge and destroys the beam family in passing, is a tuning knob, not physics. **The discount family is refused, and the evidence is measured rather than argued.**

### 4.3 The answer that is principled: first-crack rows in the LP

The oracle's own header names the gap precisely: *"CAPACITY IS READ AT THE PLASTIC LIMIT, NOT FIRST CRACK. Concentrating the tension resultant at the edge contact point reads up to 3× the uncracked-elastic first-crack moment (f·b·d²/2 against f·b·d²/6)."* That 3× is not a fudge factor to be applied post hoc — it is the exact consequence of a constraint the LP does not currently carry, and the constraint is linear, so it can simply be carried.

Take the two contact points of a joint at ±h from its centre (h = `HalfLengthCm`), with normal forces n₁ and n₂ (compression positive, the oracle's convention). The section's total normal force is N = n₁ + n₂ and its moment about the centre is |M| = h·|n₁ − n₂|. Production's own §5.3 criterion — peak fibre tension of an uncracked section — is σ = −N/A + |M|/W ≤ f_t with W = A·h/3 for the rectangle, which reduces to

```
 −(n₁ + n₂) + 3·|n₁ − n₂|  ≤  f_t · A
```

— two linear rows per joint (one per sign of the difference), **no new constant, nothing to tune**. Its properties are what recommend it:

- **In pure bending it is exactly the 3× the header names.** Plastic: T ≤ f_t·A/2 giving M = f_t·A·h. First-crack: T ≤ f_t·A/6 giving M = f_t·A·h/3.
- **At f_t = 0 it degenerates to the kern condition** e ≤ h/3 — precisely the rule production's uncracked model and §5.4's kern-limited thrust line already enforce. The LP would stop being 3× more charitable than production about bending and start agreeing with it, from one rule rather than from a family of caps.
- **It reproduces production's criterion rather than approximating it**, which is what makes the two methods' disagreements attributable afterwards.

The catch, and it is a real one: at f_t = 0 the kern condition is *stricter* than plastic no-tension, and applying it to dry joints would import production's worst recorded defect — "dry stone has no rocking model … a 5-course 2 cm/course dry stack that plainly stands in reality reads as falling". It would also move the LP's dry-stone identities, which are load- and height-independent invariants the suite leans on (1.237215440448697 = 0.866/0.7 and 1.4920634920634921 = 1.0444/0.7, both verified bit-identical across the mean flip).

**So the rule is keyed on data, not on material**: a joint with a tensile bond (f_t > 0) carries the first-crack rows, because a bond is the brittle thing and first crack is where it goes; a joint with f_t = 0 has no bond to crack and keeps the plastic no-tension form, its contact simply opening. That is the same reduction-by-data pattern that keeps μ = 0 fasteners out of the code paths, and it leaves every dry-stone row bit-identical — which is a testable prediction, and the same one the mean re-anchor demanded of its dry rows.

Predicted effects, all **ESTIMATE** until run, and the running of them is a specified experiment in §6:

- **Case 21: does not flip.** The rows bite tension-in-bending, and the probe says bond tension is a minor term here; expect λ\* somewhere in 6–14, still standing. The brittleness rule is *right*, and it does not close the disagreement.
- **Beam row 1 (glue line, C24, the row whose real verdict is a member-bending failure): may flip to Falls**, from 2.6461 toward ~0.9 if bending across the glue line governs. That would be the correct verdict arriving by a slightly wrong route (the glue line cracking, where reality has the member breaking). Worth knowing about before it is read as a success.
- **Beam rows 2/3, and the mortared wall rows: expect a fall of up to 3× where tension-in-bending binds, none where cohesion or crushing binds.** Every window in the sweep moves. This is a re-anchor-shaped slice and must be run with written predictions first, exactly as the mean flip was.

### 4.4 And the answer that is honest: attribute the residue, then govern by the active set

First-crack rows do not close case 21, and neither does anything else in the discount family. That leaves two moves, and both should be made.

**Attribute the 4.768.** The 2026-08-13 ruling's stated reason — the LP mobilises brittle cohesion along a whole chain — is now known to account for 72% of the stand and not for the rest. Nobody can say what the remaining 4.768 is. That is not a good state in which to promote the LP to authority, and it is *cheap* to fix: three more probe rows of exactly the shape of the three that exist (zero the jamb friction; zero the crushing cap; both), on the same 83-block fixture, at the same 4.4 s for four solves (MEASURED cost of the existing probe test). Plus the already-specified chain-versus-bearing split. **This is a precondition on the promotion, and it costs one afternoon.**

**Govern by the active set rather than by a hand-kept exception list.** The mechanism extraction of §3.3 hands back, for free, which constraint families are active at the answer. That makes the ruling's requirement computable:

> An LP verdict of STANDS is authoritative only where the binding constraints at the optimum are non-brittle — friction and crushing under genuine compression. Where the active set is dominated by bond terms (the tension cut-off, or cohesion at low or negative normal force), the verdict is **flagged bond-mobilised** and does not override a catalogue row that says otherwise.

That is the "explicit scope carve-out" the ruling allows, in a form that is derived per fixture instead of listed per case, that costs nothing extra to compute, and that will keep working when a fixture nobody has thought of yet arrives at the same shape of disagreement.

### 4.5 What the answer does to the rows the LP already agreed with

The first-crack rows move *every* strength-governed λ\* window in the sweep, which is a large but well-understood cost — the project has done exactly this once, on 2026-08-13/14, and the protocol is recorded. Concretely:

- **Rows where compression or friction governs do not move.** MEASURED evidence they exist: the free-end height identity died at the mean flip because its binding constraint scales with stacked load; wall-17's λ\* did not move at the flip at all while wall-18's moved ×1.38; the one-cell mortared half seat is crush-governed (1877.92298545). These rows are the control group and are the prediction the run should be judged on.
- **Rows where bending across a bonded joint governs fall by up to 3×.** None of them fall below 1.0 with any margin to spare except the beam family (§4.3).
- **Every dry-stone row must come back bit-identical.** Non-negotiable, and the sharpest single check that the rule was keyed correctly.
- **AgreeStands rows that become AgreeFalls are findings, not regressions** — the flip's own precedent — and each one needs a ruling before it is written down as a verdict.

---

## 5. Latency — the production path, and what is unproven in it

### 5.1 The size of the gap, honestly

MEASURED, from the tree:

| fixture | blocks / joints | rows × cols | pivots | time |
|---|---|---|---|---|
| 8×10 wall (`PricingCost`) | 84 / 207 | 2,707 × 6,849 | 1,942 | 4.716 s Dantzig → **0.61 s** partial pricing |
| corbel D | 90 / 200 | — | 8,439 | ~9.0 s |
| wall-18 | 119 / 203 | — | 791 | 1.3 s |
| wall-15 | 125 / 320 | — | 3,745 | 25.7 s → 4.9 s |
| wall-01 (30-course scenario) | 375 / ~1,000 | ~13k × ~34k | **58,605** | **~252 s** |

Per-iteration profile: **BTRAN 44% / refactorisation 33% / FTRAN 18%**, pricing 0.5% — MEASURED. Pricing is finished as a cost driver; what remains is linear algebra × path length.

Scenario scale is 1,220 pieces / 3,520 joints. DERIVED problem size, from the assembly (3 equality rows per non-grounded block + 1 cap + 6 strength rows per contact × 2 contacts per joint; 8 structural columns per joint + one slack per inequality + artificials): **~45,800 rows and ~116,000 columns**, which is 3.5× wall-01's rows and 3.4× its columns. **ESTIMATE for a λ\* solve at that size: pivots scale roughly with rows (wall-01 runs 4.46 pivots per row) giving ~200,000, and per-pivot cost scales at least with basis size, giving 252 s × 3.5 × 3.5 ≈ 3,100 s — about fifty minutes per player action.**

The budget on the other side: `SolveLoads` at scenario scale is **6.4 ms** MEASURED (33.6 → 6.4 after the adjacency index), and a bottom-course delete was once measured at ~1.25 s of visible lag across 31 passes — pre-index, never repeated, and itself recorded as a known user-visible problem. A defensible target is **≤ 100 ms for a complete player action** with a soft goal of a single frame.

**So the gap is roughly 3×10⁴, and no combination of the roadmap's per-iteration levers closes a gap of that size.** Presolve, Markowitz ordering, Devex and a better refactorisation cadence are worth perhaps one order between them (ESTIMATE — the profile says refactorisation is 33% and path length is the rest, so halving both is 4×, not 10,000×). The plan has to change the *problem*, not just the solver.

### 5.2 Lever 1 — stop asking for λ\* — **MEASURED 2026-08-15, and it is the lever that works**

Discussed in §3.2. The whole of wall-01's 58,605 pivots is phase 2 climbing λ from 0 to 272.2; a feasibility problem at λ = 1 does not take that walk.

**The estimate written here was 10–100× on standing structures and less on falling ones. Both halves were wrong, and the measurement is better than the estimate where it counts.** From `OracleSlowSweep.RigidBlock.FeasibilityReformulationCost` (eight fixtures, predictions written before the run):

| fixture | blocks | λ\* pose | feasibility pose | pivots | seconds |
|---|---|---|---|---|---|
| leaning stack 5 | 5 | 4 pv | 22 pv | **0.18×** | — |
| leaning stack 30 (infeasible) | 30 | 29 pv | 163 pv | **0.18×** | 0.99× |
| **8×10 wall — the gate fixture** | **84** | 2,606 pv / 0.768 s | **491 pv / 0.0666 s** | 5.31× | **11.5×** |
| corbel D | 90 | 9,490 pv | 1,022 pv | 9.29× | 37.3× |
| wall-18 | 119 | 3,885 pv | 468 pv | 8.30× | 44.6× |
| wall-15 | 125 | 6,076 pv | 803 pv | 7.57× | 18.7× |
| wall-06 | 146 | 14,209 pv | 876 pv | 16.22× | 51.2× |
| **wall-01** | **375** | 58,806 pv / 280.5 s | **5,407 pv / 26.0 s** | 10.88× | 10.8× |

What that corrects: the two small stacks cost **5.5× MORE** pivots in the feasibility pose, so the lever is not universal; the effect **weakens with scale** (dead-pose pivots are 2.2× the equality-row count at 84 blocks and ~5× at 375), so wall-01's 10.9× is the *smallest* large-fixture speedup rather than the largest; and **seconds beat pivots by up to 5×** (wall-18 buys 44.6× of time from 8.3× of pivots) because a phase-1 pivot is cheaper than a phase-2 one — unattributed, logged, and a reason never to budget on the pivot ratio. Cross-validation held on all eight: feasibility at λ = 1 agreed with λ\* ≥ 1 every time, including the infeasible arm.

**The early-exit half of this lever is REFUTED at 0.63%** — see §3.2. It is structural, not a bad fixture list, and no tightening rescues it: the phase-1 tolerance is already loose (≈13 at wall-01 scale), so a stricter gate can only move first-feasibility *later*; and the one residue the two counters cannot see — the pricer's closing full sweep — is bounded above by pricing's measured 0.5% share of runtime.

### 5.3 Lever 2 — regional decomposition — **THE SANDWICH BELOW IS REFUTED (Slice 0b, 2026-08-15)**

> **Read this box before the section.** The two-sided sandwich as specified here was measured and **fails in three separate ways, one of them fatal**. The section is kept because its diagnosis of the roadmap's original intuition is still right and because the repair is defined against it — but nothing below the box may be built as written. The user ruled **repair and re-measure** (§12 D2) on 2026-08-15.
>
> 1. **It does not decompose.** The sandwich closes at radius 5, where the region is **142 of 149 blocks — 95% of the wall**. On the collapse arm (a 30-course leaning stack) it certifies only at radius 14, where the region is **all thirty blocks**.
> 2. **It costs more than what it replaces.** At the closing radius its two solves take **753 + 872 = 1,625 pivots against the whole wall's 965 — 1.68×**.
> 3. **The pessimistic side is not a bound, and a closed sandwich can be a FALSE CERTIFICATE.** This is the fatal one. The leaning stack's grounded bottom band is feasible with a free boundary *and* with a grounded one, so the two sides agree and the rule below certifies "the full structure is feasible" — about a stack whose real λ\* is **0.4405**. Independent arithmetic in review priced the band at λ\* ≈ 18.7, so the certificate is wrong by a factor of ~40, not a knife edge. The cause: **dropping material outside a region removes its WEIGHT along with its restraint**, and in this fixture the omitted 24 courses were carrying the destabilising moment. The claim "the found force system extends" is simply false.
>
> **What survives, and it is half the lever**: restricting a full-structure admissible force system to a grounded-shell region *is* still admissible (core blocks keep all their joints; shell blocks write no rows), so **optimistic-infeasible ⇒ globally infeasible holds**. Only the pessimistic side needs repairing.
>
> **The repair to measure next** (user-ruled 2026-08-15): the pessimistic side keeps the omitted material's **weight as a dead surcharge** while removing its restraint, so the region is never asked to support nothing; and regions grow **ground-anchored by construction** — a strip to the foundation rather than a ball around the deletion — because the free-boundary side is provably infeasible for *any* region containing no grounded block (summing its vertical equilibrium rows gives 0 = −W, now asserted executably).

The roadmap states the intuition: "a deleted brick provably cannot affect masonry beyond a bonded distance". As stated it is **not true in this model and the tree contains the counter-example**: case 21's mechanism runs the full length of the jamb chain to the ground, and the abutment ladder measured λ\* responding to jamb width. A thrust path is not local. *(That diagnosis stands; it is why a sandwich was reached for at all.)*

What was believed sound — **and is not, per the box above** — was a two-sided sandwich:

- Solve the region with its boundary blocks pinned **grounded**. That grants the region an artificial foundation, so it is **optimistic**: if this problem is *infeasible*, the full structure is infeasible too, and the mechanism is genuine. ✅ *Verified sound in review.*
- Solve the same region with its boundary **free** (the boundary blocks carry their own weight and nothing holds them). That is **pessimistic**: if this problem is *feasible*, the full structure is feasible, because the found force system extends. ❌ **FALSE — measured. The force system does not extend, because the omitted weight left with it.**
- If the two agree, the answer is certified and the rest of the wall was never needed. If they disagree, grow the region and repeat. ❌ **Agreement is not a certificate under the free-boundary side above.**

Region growth was expected to be bounded, agreeing at a small radius for the common case. **Measured: it agrees at 95–100% of the structure, and 5 of 10 deletions certify at radius 4 — exactly the five whose region already reaches the foundation.**

MEASURED anchor for what a region costs today, and this part improved: the 84-block / 207-joint fixture answered λ\* in 0.61 s when this was written; **posed as feasibility it answers in 0.0666 s**. Against the ~50 ms a region needs to fit two solves inside a 100 ms budget, the shortfall is **1.33×, not the 12× estimated here** — with warm starts still unbuilt. The gate did not fire on §5.2; it fired on this section.

### 5.4 Lever 3 — warm starts and the dual simplex

After a deletion the previous optimal basis remains dual-feasible for the reduced problem, so a dual-simplex re-solve is the standard incremental playbook and the roadmap names it. Two honest notes: **the oracle has no dual simplex at all**, so this is new machinery of roughly the size of the existing phase-2 loop (reusing `FBasisFactor`, `FStandardForm` and the eta file); and the roadmap's own "~10× pivot collapse expected" for warm starts across sweep rows is an expectation, not a measurement.

The router is the other warm start, and it is free: a support graph is a nearly-admissible force system, and turning it into a starting basis is a translation rather than an algorithm. Whether it lands close enough to help is unmeasured.

### 5.5 Lever 4 — the per-iteration levers, and the one that is refused

Devex/dynamic weights (the roadmapped fix for the known partial-pricing trade, which cost wall-09 50→125 s while buying corbel D 80→9 s), presolve, Markowitz/fill-reducing LU ordering (now material at 33% of runtime), and `RefactoriseEvery = 64` as an actual tuning point rather than an untested constant. Each is its own measured slice; together **ESTIMATE ~1 order**.

Refused, and it stays refused: **threading the scans.** Parallel floating-point sums are order-dependent, bit-reproducibility is a load-bearing contract, and under LP authority it becomes *more* load-bearing, not less — the break set now depends on the last bits of an LP.

### 5.6 The policy when the budget is blown

Every solve carries a pivot cap and a wall-clock-free deterministic budget. On exceeding it, or on a verification failure (§3.6), or on a refusal of any kind: **fail closed to the router's answer**, log the reason, and count it. That is the only policy that keeps the game playable while the LP is young, and it has a happy side effect — the count of fallbacks is a measurable quality metric that can gate later work.

### 5.7 A determinism hazard that promotion creates

The determinism contract has so far been tested in one build configuration (Development editor). Under promotion, the break set depends on LP last bits in *every* build the player might run, and a Shipping build with different floating-point flags could take a different pivot path to a different degenerate optimum and therefore name a different mechanism. Cheapest experiment: build the module Shipping and compare λ\* and mechanism on three fixtures. This has never been done and it is cheap.

---

## 6. Sequencing

Each slice states what it proves, and no slice starts before its predecessor's proof is in hand. Slices 0a–0d are measurement only and produce no production code; under the project's convention a measurement lands as a pinned sweep row whose unmeasured state is its red.

**Slice 0a — close case 22's refusal. DONE 2026-08-15, and the wall it named was the wrong one.** The 218–371-block refusal was measured at the characteristic strengths and did not survive the mean re-anchor: all three of those walls answer today (case 22 at λ\* = 8.415, 88,810 pivots), and the genuine refusals were two SMALLER members at 128 and 200 blocks — which strengthens this slice's argument rather than weakening it, since 128 blocks sits well inside any region §5.3 would use. Both live hypotheses below were REFUTED by measurement (the spurious-unbounded seam fired on no solve in the family; NB-7's pivot-out took no pivot under 1e-6 absolute or 1e-7 relative). The arm was `NumericalFailure` from the periodic refactorisation, and the fix was the same constant the 2026-08-13 round moved, one notch further: `RelativePivotTol` 1e-11 → 1e-9, on the reasoning that `Refactorise` factorises from the ORIGINAL columns, so a singular LU indicts the ratio test's BASIS choice and not the elimination order. The `WhyNot` split landed first, as this slice's order requires, and §11 R4's refusal-counting experiment now has the enumerator it needs. Three residues, in CURRENT_STATE: case 22 answers at 89% of the pivot cap and nothing watches it; no fixture reaches any phase-2 arm now, so five of the seven refusal enumerators are reported by no solve; and the spurious-unbounded arm is still unreached-rather-than-sound. **The original statement of the slice follows.** The oracle refuses the whole 8-course-opening family at 218–371 blocks. A refusal at 218 blocks sits *inside* any plausible region size, so this is not a scale curiosity, it is a blocker. Order: split the `WhyNot` string first (the ~5-line change CURRENT_STATE specifies, driven by a red asserting the cap and the numerical refusal are distinguishable — the unbreakable λ-cap fixture and the case-22 wall reach different arms); then instrument. Live hypotheses, both recorded: the spurious-unbounded arm (a mirrored ± column pricing at dual-solve drift, FTRANning to no positive entry, reported as an unbounded ray on a λ-capped problem) and NB-7 (the artificial pivot-out pass still accepting an entering column on the *absolute* `PivotTol` alone — the same defect class as the one fixed on 2026-08-13, sitting on the pass that picks the basis every phase-2 solve stands on). **Proves: the oracle answers every fixture the catalogue owns.** Without it, promotion means shipping a solver with a known refusal on a known wall.

**Slice 0b — the latency spike. RUN 2026-08-15 (`OracleFeasibilitySpikeTest.cpp`, 347 s added to the opt-in group). SPLIT VERDICT: R1 did not fire; R2 FIRED.** The feasibility reformulation is the lever that works — the 84-block gate fixture answers in **0.0666 s against a 50 ms target, 1.33× short** where this design assumed 12×, with warm starts unbuilt, so the kill criterion as written ("cannot be brought under ~50 ms by the feasibility reformulation *plus warm starts*") is not satisfied and synchronous authority stays alive. The regional sandwich is **refuted** — see §5.3's box: 95% regions, 1.68× the cost of a global solve, and a demonstrated false certificate. Also refuted in passing: the early-exit saving (0.63%, §5.2) and §3.2's standing-cheap/falling-expensive asymmetry. The user ruled **repair regions and re-measure** (§12 D2); the repaired sandwich is the next measurement slice, and it must be run before 0c/0d as it is the one that decides the step's shape.
*Original text, kept because the kill criterion was judged against it:* Pose six existing fixtures as feasibility problems at λ = 1 (`bGravityIsLive = false`); pin pivot counts and wall time beside the λ\* rows already there. Then the regional sandwich on one wall: delete a brick from wall-06 or wall-01, solve globally, and solve regions of radius 2/4/8 courses under both boundary conditions; pin the radius at which the sandwich closes. **Proves, or kills, the whole latency plan.** Kill criterion: if an 84-block region cannot be brought under ~50 ms by the feasibility reformulation plus warm starts, synchronous LP authority at scenario scale is off the table and §12's latency decision changes shape.

**Slice 0c — attribute case 21's residual capacity.** Three probe rows (friction zeroed, crushing zeroed, both) on the existing 83-block fixture, plus the specified chain-versus-bearing split. **Proves: what the 4.768 is.** Precondition on the brittleness answer, per §4.4.

**Slice 0d — the first-crack rows, behind a problem flag, measured across the whole sweep.** Written predictions first, in the shape of the mean re-anchor's derivation record; then the run; then the disagreements recorded as findings. **Proves: what a principled brittleness treatment does to every row, including that dry rows come back bit-identical.**

**Slice 1 — relocation, with the gate slice.** Move `RigidBlockOracle.*` from `Tests/` to `Core/RigidBlock/`, out from under `WITH_DEV_AUTOMATION_TESTS`, splitting the `FStructure` bridge into its own file so the solver core depends on nothing but `ConnectionStrength`. **Relocation is not a behaviour and does not get its own slice** — it lands as the first half of Slice 2, whose red is what licenses it. Two rules carried across the move: the oracle keeps deriving its own MPa conversion (an assertion that the two constants agree, never a shared symbol — an oracle that imports the constant it checks agrees with a wrong value instead of failing against it), and its header's "test support only, and that is a rule not a phase" paragraph is rewritten rather than deleted, because the *independence* it protects is what makes the sweep able to catch production being wrong. The whole slow group must return bit-identical λ\* across the move.

**Slice 2 — the equilibrium gate replaces the interim guard.** Smallest authority increment: the LP answers the stability question globally; `BreakOverturnedBodies` and `SolverInterimOverturningMeanBondMPa` are deleted; the joint sweep still decides everything else. Red: a body with two load paths past its tipping point — the guard's known blind spot, which no fixture exercises today. Consequence to plan for: `TheOverturningGuardRestoresAtTheProfilesMeanBond` and its (0.60854, 0.75975) MPa bracket are *about the guard* and die with it; the bare arm's firing rung will not be 9/10 under the LP, and the replacement must be re-measured, never scaled. **Proves: the LP can hold authority over a verdict in production, on one topology, with a fallback.**

**Slice 3 — the mechanism becomes the break set.** Feasibility formulation, dual extraction, Farkas verification, the cascade loop of §3.4, and the release rule of §3.7. Red: rows 10 and 19 — the two STANDS rows production drops in zero cascade passes. They are the ideal red because their failure mode is *exactly* the one the LP has no vocabulary for, and because they cannot be made to pass by any tuning of the router. **Proves: per-piece verdicts from the LP.**

**Slice 4 — the diff-against-anchors checkpoint (§7).** No deletions. Both mechanisms live; the LP is the authority; the whole anchor set is re-measured and re-pinned. **Proves: the change reproduces the rulings.**

**Slice 5 — delete the heuristics.** `ReseatSpannedGroups`, the composite-depth walk, the one-cell thrust gate and the arching moment cap. Each deletion must be a **no-op on the gate sets of §7**; a deletion that moves a gate anchor is a finding that stops the slice. The mutation registry entries for those mechanisms retire with them.

**Slice 6 — the residues:** the dead/live split made real (the surcharge rows, which unlock the 15/16 discrimination the pair currently lacks and which are the prerequisite for step 7's impulses), the readout's source decided, the fuzzes re-aimed (§8).

---

## 7. The anchors: which, at what tolerance, and what happens when one moves

### 7.1 The finding that shapes the checkpoint

DESIGN §6 lists roughly thirty anchors. Working through them against §2's description of the change: **almost every utilisation anchor in the table is a reading of a mechanism this step deletes.** The arched half-seat, the one-cell springings, the dry-stone springing identity, the 3-cell spanned springings and the re-seat head joint, the 20-cell H/V, the staircase corbel rung, the whole corbel root family, the free-end ladder, the scenario corbel rung, cases 9/11/13/14/15/16/18/22's readings, `MortarRaggedWorstAsBuilt`, and the two intact-wall worsts — every one of them is composite depth, arching relief, or downward routing, reported as a number.

So **the rule "delete the heuristics once the diff reproduces the intended anchors" cannot mean "the same numbers", because the numbers are the heuristics.** It has to mean something checkable, and this is the proposal.

### 7.2 Three classes, three tolerances

**Class A — the gate. Verdicts and topology. Exact.** These are what "reproduces the intended anchors" means, and Slice 5's deletions must not move any of them.

- All 22 wall cases: catalogue verdict, `MustFall` and `MustStand` region membership as **set equality** (not counts), drop counts exact, `StrandsToday` exactly zero everywhere (rows 10 and 19's 3 and 6 are expected to go to zero at Slice 3 and are pinned at zero from then on).
- The four leaning-stack rows, including the dry ladder.
- Corbel family A/B/C/D/E35/E36/F verdicts, and the two §8 corbel rulings as behaviour: *a brick deleted at a free end must not bring a wall down* (2026-08-06) and *a bonded corbel stands however far it steps* (2026-08-07).
- Three beam rows' verdicts — noting §8's 2026-08-11 entry deletes `DropsToday`/`PassesToday` here when the rows go green.
- Conservation, re-expressed as the property the LP enforces exactly: the sum of ground reactions equals the total weight. The 298,069.72268342471 uu figure is a router quantity; the *property* survives and is the right thing to pin.
- Geometry and mass: brick mass 2.72163125 kg, joint areas, section moduli, the spanning-brick side load 1333.5993125 uu. Untouched by any of this and pinned bit-identical.

**Class B — the discriminations. Ratios and orderings. ±2%, and each must name its axis.** These carry the physics the pairs exist to measure, and the mean re-anchor taught precisely why they need a tolerance rather than a pin: a ratio that divides two different governing axes is not strength-invariant and its agreement can be an accident.

- 13/14 projection (production 2.7786; LP 1.856 post-flip and an open re-derivation).
- 15/16 superimposed load (production 4.5082330043756453; LP identical to one ulp until the live-load split lands, then expected to separate).
- 7-vs-9 span: **the thrust row, which is the strong one** — 0 uu of horizontal thrust into the narrow jamb against 270,024 uu into the wide one — plus the reaction ratio 1.9046, currently pinned as a *characterisation of a sublinearity*. CURRENT_STATE records the standing debt here: if the promotion gives the reaction an honest span term, that pin fires and the ≥ linear arm can be restored as a real assertion. **That is a promise this design should keep, and it is a Class B success criterion, not a regression.**
- 11-vs-12 pier (LP 1.958 today; production reads them 0.03% apart, which the LP owns measuring).
- 7-vs-10 abutment (LP 456.874 / 111.495 = 4.10 post-flip).
- The λ\* ordering across the wall set: the fixtures the catalogue found hardest stay the lowest-priced. Case 21 is the lowest of all at 17.239 and wall-19 second at 47.963.

**Class C — the readings. Re-measured once, pinned at measured bits, axis named by a one-formula identity.** Everything else in §6. The discipline is TRAPS', verbatim and non-negotiable: an axis claim in prose with no formula beside it is unverified; the /7 shortcut is only valid after the axis is proven tension on both sides; a kern-clamped joint's flexural tension is *exactly* zero, so "the tension fell below the compression" can be a wildly wrong description. For anything at ≥ 120 blocks, two certified readings per window and never tighter than ~1e-5 relative — the pivot-path lesson, which cost five re-pins once already.

### 7.3 The protocol for an anchor that legitimately moves

The mean re-anchor is the precedent and it worked; copy it exactly rather than inventing.

1. **Predict in writing before the run**, as a derivation record with a revision number. This is what makes agreement evidence instead of coincidence.
2. **Measure. Never scale.** The flip's own record is the argument: the predicted case-21 worst of ~0.549 measured 0.9354; the predicted λ\* factor of ×4.5 measured ×3.128; "the spanning brick falls" was wrong (it is compression-governed and invariant); the `AgreeFalls` arm predicted to survive died. Governing axes flip, and a flipped axis misses by far more than an ulp.
3. **A disagreement between prediction and measurement is a finding and gets written down**, in the file where it lives, not smoothed over.
4. **An exact-`==` pin that moves is re-pinned at its measured bits with the axis stated in place**, never nudged toward the old value.
5. **Lost discrimination gets a specified replacement recorded in CURRENT_STATE, never silently dropped.** There will be a lot of it: every arching and composite-depth identity in §6 is lost discrimination the moment its mechanism goes. Expect the replacement list from this step to be longer than the flip's.
6. **A pair whose two halves come to answer identically is moved onto a quantity that still separates them, or deleted.** Never left reading the right answer off two identical verdicts.

---

## 8. The six standing deliberate reds

| red | closed by this design? | why |
|---|---|---|
| `Acceptance.Wall.Catalogue` rows **10 and 19** | **YES**, at Slice 3 | Both are STANDS rows production drops (12+3 and 34+6) in **zero cascade passes** — unroutability, not strength. The LP has no routing to fail and stands both (λ\* 111.495 and 47.963). Closure depends on the release rule change of §3.7, not merely on the LP existing; `StrandsToday` goes to zero with them, and the absent cycle-division rule stops mattering. |
| `Acceptance.Wall.Catalogue` row **20** | **NO — but it becomes answerable** | §8 records the standing doubt (true count more than 2, fewer than the model's 9) as waiting on equilibrium promotion, "which answers per-brick". Mechanism extraction supplies the per-brick answer for the first time. What that answer *is* is unknown, and if it is neither 2 nor 9 it is a fresh user ruling. This design removes the blocking absence; it does not promise green. |
| `Acceptance.Wall.Catalogue` row **21** | **NO** | The ruled Collapse that production now stands at 0.93542327561664174 and the LP stands at 17.239. §4 shows no brittleness treatment plausibly brings the LP under 1.0 — cohesion zeroed outright still reads 4.768. Under §4.4's governance rule it is a *computed* bond-mobilised flag rather than a hand-kept exception, and it stays red with a named reason. Sequencing consequence: **case 21 must not be allowed to block the promotion of the other twenty-one rows.** |
| `StackBondColumnShearIsHeightIndependent` | **mechanism yes, assertion must be redefined** | Production routes a hanging column's whole load to its foot and reads (courses − 6) × the correct per-pair 0.0022240556. Equilibrium distributes it and the height-independence should emerge — but the test asserts a *utilisation*, and utilisations come from the router until the readout question is settled (§2, §3.5). The "load sheds as it rises" design pass CURRENT_STATE already demands is still owed, and the re-pinned 12/20-course pair and its ≥ 1.15 head-over-bed margin must be re-derived, not carried. |
| `CorbelStepsBeforeTensionWins` | **NO as scoped — but earlier than promised** | Promised at step 5. The counterweight buying nothing is downward-only routing; the LP already has signed tension columns and priced the counterweight at 22.6× in the characteristic era, so **the separation becomes measurable at Slice 3, one step early.** The red asserts production readings, so it closes when its assertion is redefined against LP-side quantities — which belongs with the E35/E36 crossover replacement pair already owed. |
| `Acceptance.Beam.Catalogue` | **YES**, at Slice 3 | All three rows fall whole in production (3 pieces / 1 pass each) because the one-cell thrust gate strips the dry bearings of their kern cap and an uncapped eccentric moment on a zero-tension joint reads `Max()`. The LP stands all three (λ\* 2.6461 / 28.801 / 18.299). §8's 2026-08-11 entry says the `DropsToday`/`PassesToday` pins and the entry itself are deleted at step 4 — this is that. **Caveat from §4.3**: if the first-crack rows take beam row 1 below 1.0, the LP falls the row that ought to fail by member bending, which is the right verdict by a suspect route and needs saying out loud rather than banking. |
| `Acceptance.Beam.MidspanCarriesTheMembersBendingMoment` | **half** | The solver reads \|M\| = 0 under 2.32× mean C24 capacity because of the N ≥ 2 zero-moment rule — §7 gap 3, which the LP subsumes: contact forces at the glue line give a real moment. The *moment* becomes non-zero at Slice 3. The *outcome* still needs member failure, which is step 6. |
| `Acceptance.Beam.TheMemberMaterialDecidesTheOutcome` | **NO** | Step 6. Nothing here touches piece-level failure. |

Summary: **three reds close** (rows 10 and 19 inside `Catalogue`, and `Beam.Catalogue`), **one closes halfway**, **one becomes answerable without being closed** (row 20), **one arrives a step early but needs its assertion rewritten** (`CorbelSteps`), and **two do not close** (row 21 for the reason §4 measures, and member failure because it is step 6). Note that `Acceptance.Wall.Catalogue` is one test carrying four red rows, so the suite's count does not move until all four are green — rows 20 and 21 keep the test red past step 4, and that is the honest expectation to set rather than a promise of 173 green.

---

## 9. Test strategy — how a change this large stays TDD

### 9.1 The problem, named

The suite's whole discriminating power currently rests on production's readings. Replace production's mechanism and most of the assertions become statements about a thing that no longer exists. That is not a reason to suspend TDD; it is a reason to be explicit about which net catches what, in advance.

### 9.2 What survives by construction

- **The determinism contracts.** Same problem, bit-identical λ\*, pivot count and scan count. These are the safety net for Slice 1's relocation and for every refactor after it.
- **The LP's own validation catalogue** and its M-family mutations: M1 (delete moment-balance rows, 37 failures), M2 (friction rows, 8), M3 (unlimited tension, 13), M4 (conversion open-coded, fast 36 / slow 8), M5 (λ-cap row, exactly 1), M6 (gut input validation, 63), M7 (bridge ignores the latch, exactly 1). All stay valid across the move and are the proof the formulation still means what it means.
- **Class A anchors** (§7.2): verdicts, regions, counts, masses, geometry.
- **The hand-statics validation rows** — the crush closed form, the stack closed form, the jamming closed form — none of which involve the router.
- **Property fuzz invariants that are about arithmetic rather than routing**: conservation, finiteness, termination, latch-agrees-with-stamp, broken-carries-zero, stamps strictly increasing.

### 9.3 What must be re-measured

Every Class C anchor. Both fuzz oracles are affected more deeply than a re-pin: `Structure.Fuzz`'s oracle is an independent derivation of *the routing* (least-fixed-point support, Warshall closure, Jacobi relaxation), and `CascadeFuzz`'s sharpest property — "no joint broke that was not over capacity on the graph its own pass was solved on" — is a statement about the joint sweep. When the mechanism decides breaks, that property has to become **"no joint broke that was not in the mechanism"**, and supported-implies-routed and stranding-never-travels-downward stop being invariants at all. Every re-aimed property must be proven to bite by mutation before it is trusted; the registry is the place for the new signatures, beside the old ones rather than instead of them.

Two new mutation families are needed and their expected signatures should be recorded as they are measured: **the mechanism read** (return an empty mechanism → nothing ever breaks; return every block → everything releases; both should be catastrophic and loud) and **the first-crack rows** (delete them → the whole sweep returns to today's pinned λ\*, which makes Slice 0d's re-pin its own bite-prover).

And one specified-but-unwritten test stops being optional: **the LU/eta property fuzz.** While the solver is test support, its absence is a gap; the day it ships, it is production code with a factorisation nobody fuzzes. Its spec is already written in CURRENT_STATE, with three mutations proven to bite on the review's Python transcription (BTRAN etas forward → 7.2e4, FTRAN drops the row perm → 17, BTRAN drops the output perm → 2.0). Build it in Slice 1.

### 9.4 The RED that drives step one

For the measurement slices (0a–0d) the red is the project's established one: a sweep row whose relation is `Unmeasured` — the zero enumerator, which is a failure when asserted. That is a real red, it fails for the right reason, and it is how every pinned measurement in this file arrived.

For the first production slice, the red is **Slice 2's**: a fixture the interim guard structurally cannot judge — a body past its tipping point with a *second* load path, which the guard is documented to exclude ("everything with a second load path — walls, filled corbels, beams, spanned holes — is deliberately outside its reach"). It reads safe today, it must fall, and nothing in production can make it fall without the equilibrium gate. That test is what licenses moving the oracle out of `Tests/`, and it is why relocation is not a slice of its own.

### 9.5 The discipline that must not be lost in the size of this

A green-on-arrival test is indistinguishable from one that asserts nothing until production is mutated. This change will generate dozens of green-on-arrival rows, because that is what happens when a better mechanism lands under an old assertion. Every one of them needs a mutation, and the temptation to skip that on row forty will be strong. A change that ends with a bigger suite and a weaker net is the worst available outcome here, and it is the failure mode this project has hit before — "a green suite over something invisible", twice.

---

## 10. The residues, addressed or deferred with reasons

- **~~Case 22's 218–371-block refusal~~ — CLOSED 2026-08-15 by Slice 0a, and the premise was wrong.** Those three refusals were measured 2026-08-13 at the *characteristic* strengths, one day before the mean re-anchor changed every number feeding the LP, and nobody re-ran them — so this entry named a blocker that had already stopped existing. Re-measured: all three answer, **case 22 included** (371 blocks, λ\* = 8.4149459982219277, 88,810 pivots). The real refusers were two *smaller* walls (128 and 200 blocks) sitting as isolated holes in an otherwise monotone λ\* curve, and **both hypotheses named here were refuted** — the spurious-unbounded seam fired on no solve in the whole sweep, and NB-7's pivot-out took no pivot under either tolerance. The actual arm was the periodic refactorisation finding a near-singular LU pivot within three columns of the end of the basis; the fix was `RelativePivotTol` 1e-11 → 1e-9, causally indicated (the LU is rebuilt from the *original* columns, so a singular factorisation indicts the ratio test's basis choice, and no elimination ordering can make a dependent set's pivots large). The `WhyNot` split landed with it as `EOracleRefusal`. **The lesson this entry now carries**: a measurement is only current until the data under it moves, and the re-anchor moved all of it.
- **The "λ could read LOW rather than refuse" seam — IN SCOPE, ADDRESSED, and upgraded.** §3.6. Under production authority it stops being a test refusal and becomes a wall falling for no reason; the Farkas certificate check closes the direction verification cannot see. This is the highest-value small item in the design.
- **The dead/live split (`bGravityIsLive`, `FOracleAppliedForce::bLive`) — IN SCOPE, Slice 6, and it becomes load-bearing.** The feasibility formulation makes gravity dead by construction, so the fields stop being decorative immediately. Marking wall-15/16's six surcharge courses live is the small slice that unlocks the LP-side discrimination the pair lacks (λ\* identical to one ulp today), and it is the prerequisite for step 7's impulses, which are live loads by definition.
- **2D versus 3D — DEFERRED with reasons, and the seam designed for.** Every fixture the project owns is single-wythe and laid in X-Z, and the bridge refuses an out-of-plane normal rather than projecting it, so 2D is *sufficient today* and 3D blocks nothing in step 4. It is not sufficient for long: three of the wanted acceptance cases (the L-corner return, the two-wythe wall, and eventually arbitrary-direction force) need it. **ESTIMATE of the cost**: 6 equilibrium rows per block instead of 3, 4 contact points per rectangular joint instead of 2, and a linearised friction pyramid instead of a signed pair — roughly 2× rows, ~4× columns, and a superlinear path effect, so **perhaps 10–30× the 2D solve cost**, which lands directly on §5's budget. Two obligations now: keep the assembly written so the dimension is a parameter rather than a shape assumption, and make the production refusal of an out-of-plane joint *loud* (a fail-closed fallback to the router with a logged reason), because in production a silently refused joint is a silently wrong wall. The honest caveat to carry forward: **latency conclusions measured in 2D do not transfer to 3D.**
- **The deleted active-set probe** — not on the critical path (§3.3), and the `FOracleResult` field it wanted is subsumed by the mechanism extraction, which is a superset of it.
- **The mean-basis case-21 mechanism split, and the two dead cross-fixture identities** (the free-end height identity, the 13/14 projection cross-validation) — Slice 0c and the Class B re-derivations. Naming their binding constraints was already logged as belonging with the step-4 design; the mechanism extraction is what makes it a five-minute question instead of a re-instrumentation.

---

## 11. Risk register

| # | risk | how it kills the step | cheapest experiment that reveals it early |
|---|---|---|---|
| R1 | ~~Latency does not close.~~ **DID NOT FIRE, 2026-08-15.** A region solve is **0.0666 s**, not the feared ~0.6 s — 1.33× short of target with warm starts unbuilt. *Residual risk, unchanged:* wall-01 still needs 26 s and scenario scale is ~3.3× that, so synchronous authority depends entirely on regions being small — which R2 now owns. | Synchronous LP authority is impossible at scenario scale; the design becomes asynchronous or hybrid, which changes everything downstream. | Run. The remaining measurement is warm starts (unbuilt, roadmap expects ~10×). |
| R2 | ~~The regional sandwich never certifies.~~ **FIRED, and worse than the risk as written.** It certifies at 95–100% of the structure, costs 1.68× a global solve, **and its pessimistic side is not a bound at all** — a demonstrated false certificate (§5.3 box), wrong by ~40× on the fixture that exposed it. The risk anticipated "never certifies"; the measurement found "certifies wrongly", which is the more dangerous failure. | Lever 2 is worthless and R1 fires. | Measured: 5 of 10 deletions agree at radius 4 — exactly the five whose region reaches the foundation. **Next:** the repaired sandwich (omitted weight as dead surcharge; ground-anchored growth), re-measured against these same fixtures. |
| R3 | **The mechanism names the wrong bricks** under degeneracy (170–358 Bland entries per solve is a lot of degeneracy). | Verdicts become arbitrary-but-reproducible; the case-20 answer is unbelievable; players see the wrong bricks fall. | Extract the mechanism on wall-20 (where the true count is disputed) and on rows 10/19/21; then re-solve with a permuted column order in a test and check the named set is stable. Instability found here reshapes §3 toward per-region interrogation for disputed rows. |
| R4 | **The LP refuses in production.** *(Partly retired 2026-08-15 — Slice 0a closed the two real refusers and found the recorded case-22 family answers; `EOracleRefusal` landed, so refusals now name their arm. The risk is not gone: the spurious-unbounded arm is still **unreached rather than sound**, no fixture reaches any phase-2 arm so five of seven refusal reasons are exercised by nothing, and case 22 answers at 88.8% of `MaxPivots` with nothing watching it — a pivot-path change tips it into a refusal by a different route.)* | A refusal mid-cascade with no fallback is a frozen or wrongly-collapsing wall. | Run the whole catalogue at production scale counting refusals by reason (now possible); watch the case-22 cap headroom; find or build a fixture that genuinely reaches a phase-2 arm — the `IterationCap` arm is the cheap candidate, since a slightly wider member of case 22's family hits the cap deterministically. |
| R5 | **Brittleness cannot be answered at all** — the first-crack rows do not flip case 21 (expected) *and* the residual 4.768 turns out to be something that ought not to be there. | The 2026-08-13 precondition is unsatisfiable as written and the user must rule again before promotion. | **Slice 0c**, four solves, 4.4 s. |
| R6 | **The anchor re-measure invalidates more discrimination than it replaces**, leaving a bigger suite with a weaker net. | The step lands and the project loses the ability to detect its next regression. | Write the anchor classification (§7) *first*, with a predicted fate per anchor, and count how many Class C anchors have no Class A or B sibling. Any anchor family that is entirely Class C is a hole being opened. |
| R7 | **Bit-reproducibility breaks across build configurations** now that the break set depends on LP last bits. | Different players see different collapses; the determinism contract silently stops being one. | Build the module Shipping; compare λ\* and mechanism on three fixtures. Never done, cheap. |
| R8 | **The readout and the verdict visibly disagree** — a joint drawn comfortable that the LP breaks. | A user-visible inconsistency in a shipped feature, in a game whose whole premise is legible structural strain. | Once Slice 3 is green, run the piece-menu readout on a collapsing fixture and look at it. This one needs eyes, not a test. |
| R9 | **The collapse sequence gets coarser** (§3.4) and the playback reads worse than today's 31-pass stepping failure. | A quality regression in the thing the stamps exist for. | Compare stamp counts and grouping on the same bottom-course delete, before and after Slice 3. |
| R10 | **Scope creep through the deletions.** Slice 5 removes four mechanisms at once and something moves that nobody predicted. | A regression buried inside a large green diff. | Delete them one at a time, each with the Class A gate re-run, and treat any moved gate anchor as a stop rather than a re-pin. |

---

## 12. The decisions that are the user's

Five, with a recommendation and the evidence for each.

**D1 — Brittleness treatment.**
*Recommendation:* adopt the **first-crack (uncracked-elastic peak-fibre) rows for joints with a tensile bond**, keep the plastic no-tension form where f_t = 0, add the **active-set governance rule** (§4.4), and **accept that case 21 stays an inverted deliberate red** with a computed bond-mobilised flag rather than a hand-kept exception. Do not adopt a ductility discount in any form.
*Evidence:* zeroing case 21's jamb cohesion outright leaves λ\* = 4.768 (MEASURED, pinned), so no cohesion-based brittleness rule can flip it — the discount family is refuted by measurement, not by argument. A uniform 17.24× divisor, the size needed, destroys the beam family and puts corbels A and C on a knife edge (DERIVED from pinned λ\*). The first-crack rows are the same 3× the oracle header already names, cost no new constant, degenerate exactly to the kern condition production already enforces, and leave dry stone bit-identical — a testable prediction. **What this asks the user to accept:** the 2026-08-13 precondition is satisfied by a principled rule plus a computed carve-out, not by making the disputed row fall.

**D2 — Latency budget and approach.** *(Ruled 2026-08-14 as recommended; **the gate then ran and this decision was re-taken 2026-08-15** — see the second block below.)*
*Recommendation:* set the budget at **≤ 100 ms per player action** with fail-closed fallback to the router, and approve **Slice 0b as a gate on the whole step** — measure the feasibility reformulation and the regional sandwich before a line of production code is written.
*Evidence:* wall-01 (375 pieces) answers in ~252 s and 58,605 pivots, all of it phase 2 climbing λ (MEASURED); scenario scale is ~3.5× that problem, **ESTIMATE ~50 minutes**, against a 6.4 ms router. The gap is ~3×10⁴ and the roadmap's per-iteration levers are worth ~1 order between them. The two levers that could close it — abandoning λ\* and regional decomposition — are **both unmeasured**, and one of them (the sandwich) is not even on the recorded roadmap in a provable form. This is the risk most likely to kill the step, and it is the cheapest to test first.

**D2′ — 2026-08-15, after the gate ran: REPAIR REGIONS AND RE-MEASURE** *(user, with the split verdict in hand)*. The gate paid for itself: one lever beat its estimate, one was refuted, and a third — the early exit — turned out to be worth 0.63% rather than the free win §3.2 promised. The budget and the fail-closed fallback stand unchanged. What changes is lever 2: the sandwich is not built as designed, because its pessimistic side is not a bound and a closed sandwich can certify a structure that falls (§5.3 box, wrong by ~40× on the exposing fixture).
**The repair to measure, and it is the next slice, ahead of 0c and 0d**: (i) the pessimistic side keeps the **omitted material's weight as a dead surcharge** while removing its restraint — the false certificate arose precisely because dropping material dropped the destabilising moment it was carrying; (ii) regions grow **ground-anchored by construction** (a strip to the foundation, not a ball around the deletion), because the free-boundary side is provably infeasible for any region containing no grounded block. The optimistic side needs no repair — restriction of an admissible system to a grounded-shell region stays admissible, verified in review.
**What is still unmeasured and now matters more**, because it is the only lever left between a 26 s wall-01 and a playable budget: **warm starts and the dual simplex** (§5.4). If the repaired regions stay small, warm starts are the margin; if they do not, warm starts are the whole answer. Neither has been run.
*The honest read to carry into that slice*: full-structure synchronous authority at scenario scale is **unlikely** on this evidence — 26 s at 375 blocks against 33 ms of frame, with ~3.3× more structure to come and 3D estimated at another 10–30×. The step survives on regions being small, and that is now a single measurable question rather than a hope.

**D3 — Does the 3D gap block?**
*Recommendation:* **no.** Ship 2D, keep the assembly dimension-parametric, make the production refusal of an out-of-plane joint loud and logged, and record 3D as the prerequisite for the L-corner case, the two-wythe case and step 7.
*Evidence:* every fixture the catalogue owns is single-wythe X-Z and the bridge already refuses out-of-plane normals rather than projecting them, so nothing in step 4 needs 3D. **ESTIMATE of the cost of doing it later: 10–30× the solve cost**, which lands on D2's budget — so the honest caveat is that the latency answer will have to be re-measured when 3D arrives, and 3D should not be attempted until D2's levers are proven.

**D4 — Sequencing risk appetite.**
*Recommendation:* the **conservative order of §6** — four measurement slices before any production code, then the gate slice, then the mechanism, then the checkpoint, then the deletions one at a time. Specifically: **do not let case 21 block the other twenty-one rows**, and **do not delete a heuristic until the Class A gate is green with both mechanisms live**.
*Evidence:* the project's own history. The one-cell thrust gate's beam fallout was invisible for two days and is the reason the `DropsToday` pins exist; the mean re-anchor's predictions were wrong on five separate counts and only measurement caught them; and — added 2026-08-15, because it is the sharpest instance yet — **Slice 0a found that the case-22 refusal cited here as a live unexplained finding had already stopped existing**, its measurements having expired silently when the re-anchor moved the data under them, while both recorded hypotheses about it were refuted. The aggressive alternative — promote and delete in one slice — would land a change touching every verdict in the catalogue behind a single green run, which is exactly the shape this codebase has been burned by.

**D5 — The collapse sequence, and what the readout shows.** *(A product decision this design surfaces rather than solves.)*
*Recommendation:* accept the coarser sequence for now (**ESTIMATE 2–5 stamps where there are 31 today**) and keep the router as the strain readout through Slice 5, logging the readout-versus-verdict inconsistency as a known cost with the minimum-violation LP as its specified replacement.
*Evidence:* the mechanism is instantaneous and unordered by construction (§3.4), and DESIGN §5.6 says the stamps are what a visualisation plays back — so this is a real, visible trade, not bookkeeping. The alternative sources for a per-joint number are the LP's optimal forces, which read misleadingly comfortable (§3.1), or a second LP per solve, which D2 cannot afford yet.
