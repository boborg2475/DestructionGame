# Moments and the spatial layer — design

**Status: slices 1, 2, 3 and 3b are BUILT and green; the cascade is on the world wire. Slice 4 is part-landed (`GetConnectionMoment` exists; the readout line does not). Slices 5 and 6 remain.** Numbers below marked superseded were corrected against the emitted joint centroid — see the note at case (b). Read [DESIGN.md §3](DESIGN.md) (the averaged-stress limitation) and [§6](DESIGN.md) (the spatial layer) first; this is the worked design that sits under them.

---

## The scope call, and it revises DESIGN.md §6

§6 says moments, arbitrary-direction force and debris damage are **one dependency wearing three hats**. That is **right about the data and wrong about the sequencing**, and acting on the wrong half is what has held this up.

What the three genuinely share is one thing: **a joint knowing its own extent and centroid** — a rectangle `MakeInterface` already computes at `Layout.cpp:219-254` and discards three lines later. Design that once, for all three.

What they do *not* share:

| Hat | Also needs | Shared? |
|---|---|---|
| Moments | a piece **centre of mass** | no |
| Force delivery | a **spatial query** over `FBrickLayout` | no |
| Debris damage | an **impulse channel** from Chaos into the graph | no |

Those are three unrelated pieces of engineering. **Settle the joint rectangle, land moments on it now**, and let the query index and the impulse channel be designed when their own work starts. Nothing below forecloses either.

---

## Where the data lives

**Joints carry their own geometry, emitted by `MakeInterface` as part of the same atomic value as the pair and the normal. Pieces gain exactly one field: a centre of mass.**

```cpp
// FConnection
FVector InterfaceCentreCm     = FVector::ZeroVector;
FVector InterfaceHalfExtentCm = FVector::ZeroVector;  // zero on the normal axis

// FStructurePiece
FVector CentreOfMassCm; bool bHasCentreOfMass = false;
```

**Atomic emission is the point.** `Layout.h` already argues the pair and normal are emitted together rather than assembled at a call site, because an inconsistent pairing is silent. An extent disagreeing with its area is the identical failure — a plausible area with the wrong lever arm. Emit all four together and have `AddConnection` reject `|4·h_u·h_v − Area| > tol`, and the inconsistent state becomes **inexpressible**, the standard `FStructureBinding` already sets.

### "World-free" is two properties wearing one name

1. **No world dependency** — no `UWorld`, no actor, no tick, no query. **Untouched.** The solver gains numbers, not a dependency.
2. **No positions at all.** Genuinely given up. It bought two things:
   - *Fixtures need no coordinates* — **preserved, and this is the crux.** Set the moment to zero and `σ = N/A ± M/W` collapses to `N/A`, today's answer bit for bit. A geometry-free fixture is not a special case, it is `e = 0` — the same shape as `FrictionCoefficient = 0` reducing Mohr-Coulomb exactly. **So the existing tests and both fuzz generators need no geometry**, which is the only reason a small first slice is possible.
   - *Routing cannot reason geometrically* — **genuinely lost, and this is the real price.** Someone can now write "supports are the joints whose centroid is below the piece" and it will read as reasonable, which is the class of defect where nothing crashes and the wall stands there being wrong.

**Mitigation, to be written on the function: `GetJointRole` may never read geometry.** It reads the normal and the A/B pairing, nothing else. **Positions are an input to the magnitude of a load, never to the routing of it.**

**Add `FStructure::HasCompleteGeometry()`** — same trick as `HasSupportAnswer`. "Nobody supplied positions, so there are no moments" must be *askable*, not silently identical to "the load happens to be centred".

**Rejected:** general positions on `FStructure` (makes every fixture a geometry problem, and a defaulted zero reads as *healthier* than reality); a parallel geometry side-table (a second array that must stay in step with the first — exactly what `StructureBinding` exists to outlaw, plus two authorities for area).

---

## The physics

**There is no new conversion boundary, and this is worth saying loudly in review because "moments" sounds like it should introduce one.** Length is cm, so a moment is uu·cm and `M/W` with `W` in cm³ gives uu/cm² — the identical quantity `StressMPa` already divides by `ForceUnitsPerMPaSqCm`. Same named constant.

```
σ_n = (Tension − Compression) / A          signed, positive in tension
σ_b = |M_u|/W_u + |M_v|/W_v                worst-corner biaxial
peak tension     = max(0, σ_n + σ_b)
peak compression = max(0, σ_b − σ_n)
```

`W_u = (4/3)·h_u·h_v²`. **The in-plane frame needs no extra data** — it is the two world axes that are not the separation axis. That imposes one rule: **extents may only be supplied on an axis-aligned normal**, refused at `AddConnection` otherwise. `MakeInterface` only emits axis-aligned normals, so nothing real is refused and every tilted fixture keeps zero extents.

| Axis | Interaction |
|---|---|
| **Tension** | The whole point — `f_xk1` is a hundredth of the compressive figure |
| **Compression** | Grows at the opposite edge; never governs (~1e-3 of capacity) |
| **Shear** | Applied shear unchanged; the **friction term keeps using mean compressive stress** |

Friction stays on the mean, for three reasons: it can never make a joint look *stronger* than today; EN 1996-1-1 §3.6.2/§6.2 defines `f_vk = f_vk0 + 0.4·σ_d` with σ_d an **average**; and the refinement (average over the compressed part only) is a real improvement that deserves its own slice.

`FConnectionLoad` gains `BendingMomentUUuCm` / `BendingMomentVUuCm`. `ComputeUtilisation`'s area parameter becomes `FJointSection { AreaSqCm; SectionModulusUCm3; SectionModulusVCm3; }`. **Fail-closed: a non-zero moment against a zero modulus returns `Max()`** — branch on the moment first so `0/0` never happens. `FConnection::UtilisationUnder` stays the single decision point.

### Standards

- **EN 1996-1-1 §3.6.3 Table 3.2** — characteristic *flexural* strength, `f_xk1 = 0.10 N/mm²`, failure plane parallel to the bed joints. **This is already the number in `GeneralPurposeMortar`**, and its comment already says "the joint being pulled open". **The profile has carried a bending strength since it was written; the model had no bending to use it on. `Core/Profiles` needs no change.**
- **EN 1996-1-1 §6.1.2** — eccentric vertical load, `Φ = 1 − 2e/t`, zero at `e = t/2`. Our uncracked-elastic rule is **stricter** — failure when the tension edge reaches `f_xk1`, well before overturning — which is the right direction for a game about things peeling off.
- **Middle-third / kern** — beam theory, not a code figure, and say so. For the bed patch `t = 10.25 cm`, the kern is **±1.708 cm**: the load path may wander 1.7 cm off centre before any part of the joint opens.
- **Corbelling limits** — real codes limit projection per course; no clause number verified, so state the behaviour and cite nothing.

---

## The worked numbers

Brick 21.5 × 10.25 × 6.5 at 1.9 g/cm³ = 2.72163125 kg → **W = 2667.198625 uu**.

**(a) One brick overhanging its bed joint by half its length.** Bearing 110.1875 cm², `W_sec = 197.4193 cm³`, `e = 5.375 cm` — **3× outside the kern**.

| | |
|---|---|
| opened edge | **4.8412e-3 MPa → 0.0484 of `f_xk1`** |
| today | 2.4206e-4 |

**200× worse, and it decomposes exactly: 2 (edge amplification, `σ_b/σ_n = 6e/t = 3`) × 100 (the tensile-to-compressive ratio).** It still stands at 4.8% — correct; a brick corbelled half its length really does stand.

**(b) The case actually standing in the game — a brick held by one head joint.** `e = 10.75 cm`, and the mean normal stress is **exactly zero** (gravity is parallel to a head joint).

| | today | with moments |
|---|---|---|
| one hanging brick | shear 0.02002 | **tension 0.4157273077** |
| two in a chain | 0.0400 | **0.8315 — it HOLDS** (1.626 needs slice 5) |
| bricks to failure | **≈ 50** | **3** (2 once slice 5 lands) |

The recorded "roughly fifty" reproduces exactly: `1/0.02002 = 49.95`.

**(c) A running-bond brick that lost one of its two bed supports.** `e = 5.625 cm` → **0.05820, a 229× change**; peels under **≈ 17 courses** rather than ≈ 3,900.

**And the intact wall does not move.** A brick on two symmetric bed patches has its CoM at the area-weighted centroid of its supports, so `e = 0` **exactly** — the measured worst joint stays at **0.00495**. That is the regression anchor, and it is exact rather than approximate.

---

## The trap in the statics, found while working the numbers

The naive rule — *each supporting joint carries `M_j = (p − c_j) × S_j`* — is **wrong, in the direction that would only be discovered after landing.** On an ordinary running-bond brick with two symmetric patches it gives each joint `σ_b = 4.18e-3` against `σ_n = 1.269e-3`, so **every bed joint in a standing wall would read 0.029 in tension** and the wall would be half-peeled everywhere. The moments cancel across the pair but not on either joint.

The correct statics: a piece on several supports is statically **indeterminate** — the reactions rearrange to satisfy moment equilibrium — and **determinate on exactly one**.

- **N = 1: exact.** `M_j = (p − c_j) × F_tot + Σ M_received`.
- **N ≥ 2: keep the area split, moment zero.** Exact when the CoM sits at the area-weighted centroid (every symmetric running bond), unconservative otherwise, **and recorded as such**.

That is not a compromise dressed up: **N = 1 is where the entire visible symptom lives.** Head-joint fallback with one neighbour is N = 1 by definition; a corbel that lost a support is N = 1.

---

## Slices, red-test-first. Visible at slice 3.

1. **A joint carrying an off-centre load reports its edge stress.** `Core.ConnectionStrength.EdgeStressUnderAMoment`, carrying case (b) as literals. Red: expects 0.39725, gets 0.02002. Plus a zero-moment row asserting **bit-identical** output, and a moment-against-zero-modulus row asserting `Max()`. *Note: the declarations make the test non-compiling first — the red signal is the value once it compiles, and that needs arbitrating with dev-expert so the gate does not stall.*
2. **A joint knows its own extent and centroid.** `MakeInterface` emits them; `AddConnection` refuses extents disagreeing with the area, and extents on a non-axis-aligned normal. This is **the shared rectangle all three hats need.**
3. **VISIBLE — a piece on one support loads it eccentrically.** `FStructurePiece` gains the CoM (fold in the recorded want for `AddPiece` to take a material, so the signature moves once); `SolveLoads` computes the moment for pieces with exactly one load path. Red: `Structure.HangingBrickPeelsRatherThanShears` expects 0.39725. Guard row: a two-support symmetric brick reports **exactly** its current utilisation.
4. **The readout can explain the number** — `GetConnectionMoment`, `FJointInspection::MomentUuCm`, the presenter line. **Not optional; see below.**
5. **A corbel feels the wall above it.** `ReceivedFromAboveUU` becomes force **and** moment about a reference point. Red: predicts a peel at ~17 courses. **Also the forward-compatibility slice** — once the accumulation carries a moment vector along the load path, an applied force at a point is strictly additive.
6. **Multiple supports carry moment properly.** *(Deferrable.)* Reactions rearrange to satisfy equilibrium. This is the slice that makes both fuzz generators emit **coherent geometry** — a real generator change — so enter it deliberately with its own mutation table, not folded into the run that makes cantilevers fall.

**Stop at slice 5.**

---

## What breaks

CURRENT_STATE names the first two. **#4 is recorded nowhere and is the one that will hurt.**

1. **`CheckJointLoad`'s hard `IsNearlyZero(Load.Tension)`** (`StructureTest.cpp` ~347), justified as a property of the *fixtures* because every normal routed through it is axis-aligned. **The justification stops being true; the assertion stays green.** Axis-alignment is no longer what guarantees zero tension — *absent geometry* is. Re-scope the comment from "axis-aligned" to "geometry-free", or it becomes a correct assertion with a false reason, which is worse than a wrong one because nobody re-derives it.
2. **`StructureTest.cpp` ~1257-1263** — same re-scoping; its claim that `TiltedJointClassification` is the only tension source becomes false.
3. **`Structure.TiltedJointClassification`** — reword the framing, do **not** touch the behaviour.
4. **`GetConnectionForce` and `FJointInspection::ForceUu` become unable to explain their own utilisation.** After slice 3 the panel shows a head joint at **0.397** beside a force of **2667 uu**, and no arithmetic a player can do connects them. That is this project's recurring signature — a plausible number that does not describe what is happening — which is why slice 4 is **part of the work, not a follow-up**.
5. **Both fuzz `UtilisationUnder` transcriptions** move in lockstep in the same commit. Signature-only (`{Area, 0, 0}`) *because* the generators emit no geometry — the payoff of degenerate-at-zero. The recorded ulp hazard means the transcription stays exact, not merely equivalent.
6. **`DryStone` has `TensileStrengthMPa = 0.0` exactly**, so any eccentricity outside the kern instantly fails it. Physically correct — dry stone genuinely cannot corbel — but it will move the cascade fuzz's recorded "11.5% infinite" distribution. **Expect it; do not treat the moved figure as a regression.**
7. **`ConnectionLoad.h`'s "at most one of Compression and Tension is non-zero"** stays true of the *resultant* and becomes false of the *stress*. Keep the invariant, document that the split now happens downstream.
8. **`Structure.cpp`'s "assignment, not accumulation"** justification survives, but the stored vector stops being the whole story.
9. **`NOT VERTICAL`** is the only X/Y-aware assertion in either fuzz, and a moment is inherently an X/Y quantity that gets **no** coverage from it.

---

## What not to do

**On load-path versus applied-force: eccentricity comes from the load path, and slices 1–4 must avoid answering the other half.** There are no applied forces in the game. The only eccentricity that exists is the geometric offset between a piece's mass and its supports, which `SolveLoads` can compute with nothing from outside. **The one thing that must be right for later is that the moment is accumulated as a vector along the load path (slice 5), not recomputed per joint from a scalar** — get that right and `r × F` drops in additively.

- **Do not model the cracked section.** Strongly tempting as "the real masonry model". Wrong here: our mortar *has* a tensile strength, so the joint reaches `f_xk1` **before** the crack propagates — the uncracked formula is valid right up to the break, and past it there is no joint. It would also be a nonlinear second evaluator beside the one `Connection.cpp` insists stays singular.
- **Do not split `TensileStrengthMPa` into `f_xk1`/`f_xk2`.** Very tempting — Table 3.2 gives both and the bed/head distinction maps onto them beautifully. Wrong because **`f_xk2` is a wallette property that already contains the bond interlock**, which our graph represents *explicitly* as a brick spanning two below. Using it per head joint double-counts the bond pattern.
- **Do not change the area-weighted split before slice 6.**
- **Do not add torsion.** Second-order for gravity on rectangular joints, needs a different modulus, and shear is nowhere near governing.
- **Do not give joints a transform, an `FBox` or an oriented frame.** Every joint `MakeInterface` can produce is an axis-aligned rectangle; centroid + two half-extents + the existing normal is complete.
- **Do not add positions to answer "what is near this point".** That is force delivery's *query* problem, it belongs above the solver, and it is the thing that would actually cost the world-free property. **The discipline line: the solver may know where its own joints are; it may never be asked what is near a point.**
- **Do not rewrite fixtures to carry geometry.** `e = 0` reproduces today exactly. Migrate opportunistically and let `HasCompleteGeometry()` make the gap observable.
- **Do not let `GetConnectionForce` encode the moment** by returning a longer or non-vertical vector. Separate accessor, separate field.
- **Do not "fix" the tilted-joint-above-a-piece behaviour** while you are in there — it is characterised and accepted in DESIGN.md §3.
