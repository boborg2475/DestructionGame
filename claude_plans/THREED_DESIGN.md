# THREED_DESIGN — generalising the rigid-block equilibrium LP from 2D X-Z to full 3D

The design for Phase E of the shed (SHED_PATH). The full four-walls-and-roof shed watched toppling
in Chaos needs a 3D LP; this is how the 2D assembler generalises. Grounded in the 2D code at
`Core/RigidBlock/RigidBlockOracle.cpp` (assembler `:1457–1804`, tolerance `:695`, mechanism
`:1339–1373`) and `RigidBlockBridge.cpp` (Y-normal refusal `:126–133`). **Approved rulings are marked
[RULED].**

## The architecture — two assemblers, one solver [RULED]

The solver is a **two-assembler-over-one-simplex** machine. `BuildStandardForm` (`:334`) and the
revised simplex below it are **dimension-agnostic** — they take assembly rows + a struct-column count
and know no physics. So **3D is a SECOND physics assembler**, not a new solver — D3
("dimension-parametric assembly") with the least blast radius. One branch at the top of
`SolveRigidBlock`:

```
if (P.Dim == EOracleDim::Dim3D)  AssembleThreeD(P, Rows, NumStructCols);   // NEW
else                             AssembleTwoD(P,  Rows, NumStructCols);     // = today's code, VERBATIM
BuildStandardForm(...);   // unchanged;  simplex, warm-start, mechanism plumbing — all shared
```

**The 2D assembler stays a LITERAL separate branch — NOT the 3D code specialised to Y=0.** This is
load-bearing and makes 2D bit-identity a THEOREM: (a) `InfeasibilityTolerance = (1+LargestRhs)·1e-9·
NumRows` (`:695`) scales with `NumRows`, so a 3D-with-Y=0 block emitting 6 rows where 2D emits 3
silently shifts the tolerance and the pinned pivot path; (b) the friction-pyramid facets (`/√2`) ≠ the
2D `|v|≤R` rows; (c) 4 contacts vs 2 change column indexing. **E1a's definition of done includes:
OracleSweepFull + every `RigidBlockOracleTest` row byte-for-byte unchanged.**

## Data (additive, 2D-default) [RULED enum-not-bool]

- `FOracleProblem`: `enum class EOracleDim { Dim2D, Dim3D }; EOracleDim Dim = Dim2D;` (zero-enumerator
  is the safe default, matching house idiom).
- `FOracleBlock`: `double CentroidYCm = 0.0;` (inert in 2D).
- `FOracleJoint`: `NormalY`, `CentreYCm`, two in-plane axis vectors (or a canonical derivation), and
  `HalfUCm`/`HalfVCm` (keep `HalfLengthCm` for the 2D path).
- `FOracleMechanismBlock` (E2): `VirtualUy`, and `VirtualOmega` → a 3-vector `(Ωx,Ωy,Ωz)`.

## The 3D physics

**Six equilibrium rows / non-grounded block:** ΣFx,ΣFy,ΣFz,ΣMx,ΣMy,ΣMz. A contact force
`f = n·N̂ + s_u·û + s_v·v̂` (right-handed `û×v̂=N̂`); `r = contact − centroid` (3-vector). Force rows take
the components of each unit direction; **moment rows take the components of `r×ê`** for `ê∈{N̂,û,v̂}`.
The current 2D moment row (`Rx·Nz − Rz·Nx`) is exactly `−(r×ê)_y` — i.e. the **My row**; 3D adds Mx and
Mz with the same `r×ê` machinery (a globally-negated equality row is bit-identical after
`RowFlip`). Gravity `−Mass·980` into ΣFz (force only), unchanged. Trivially-satisfied rows (Fx/Fy/Mz on
a gravity-only block) are still EMITTED — emitting them is what a 3D block IS.

**4-corner contact geometry:** a rectangular interface (centre `C`, normal `N̂`, in-plane `(û,v̂)`,
half-extents `(h_u,h_v)`) puts a contact at each of the 4 corners `C ± h_u·û ± h_v·v̂`. Each contact:
`[n+, n-, p_u, q_u, p_v, q_v]` = 6 struct cols (`n-` data-gated on `bCanTension`, as 2D gates it);
tributary area `A/4`. Struct cols = `1 + 6·NumContacts`, `NumContacts = NumJoints·4`. `(û,v̂)` derived
deterministically from `N̂` (tie-broken by fixed axis index — the 3D analogue of the 2D fixed tangent);
the bridge (E3) carries the real interface axes.

**Friction PYRAMID [RULED k=8, INSCRIBED]:** the true cone `√(s_u²+s_v²) ≤ cA+μn` is not LP-able;
replace with k=8 inscribed facets `cosθ_i·(p_u−q_u) + sinθ_i·(p_v−q_v) − μ(n+−n-) ≤ c·Conv·A/4`,
`θ_i=i·π/4`. **Inscribed (conservative) is not a free choice:** the oracle is the safe lower-bound
theorem ("is there ANY admissible equilibrium"); a circumscribed pyramid would let a contact carry
shear it lacks and certify a sliding collapse as standing — the "plausible number, wrong statics"
failure the project exists to avoid. Inscribing occasionally fells a structure the true cone stands —
the honest, safe direction for a demolition gate. `k` is a constant so the accuracy/cost trade is one
number.

**Tension/crush** generalise trivially (A/4 tributary, `bCanTension`-gated). **Biaxial first-crack is an
E-TAIL [RULED defer]:** the peak fibre is now at a corner under Mx AND My
(`σ = N/A + |M_u|/W_u + |M_v|/W_v ≤ f_t`, 4 linear rows/joint), but the "3×" plastic/elastic ratio is a
uniaxial fact and the biaxial corner ratio needs deriving; mirror 2D's flagged default-off history and
land plastic no-tension + tension cut-off first.

## Cost (refines the 10–30× estimate)

Equilibrium rows/block 3→6 (2×); contacts/joint 2→4 (2×); cols/contact 4→6 (1.5×); cols/joint 8→24
(3×); friction rows/contact 2→8 (4×); nonzeros/force-col 3→6 (2×). Aggregate: rows ~2× (equilibrium)
/ ~3–4× (strength), cols ~3×, matrix nonzeros ~5–6×; simplex cost **~8–12× compact, up to ~25–30×**
with k and biaxial rows. So the 3D interactive band is **a few dozen blocks** (2D's ~84–104 ÷ 10–30×)
→ **R-Scale (toy-first) and E4 coarsening are the real levers**, not solver micro-opt. E0 measures where
it actually lands — the R-Scope re-confirmation gate.

## E0 — the toy-3D tractability measurement

**E0-A — TRIPOD (determinate, exact hand statics) [RULED first red].** One free block (weight W) on
THREE grounded point-patch supports (`N̂=+Ẑ`, zero in-plane half-extents → single normal contacts, the
3D `HalfLength=0` analogue) at P1=(0,0), P2=(L,0), P3=(0,L). From ΣFz,ΣMx,ΣMy:
`R2 = W·cx/L, R3 = W·cy/L, R1 = W(1−cx/L−cy/L)`. For CoM at (L/4,L/4): **R1=W/2, R2=R3=W/4** — unique,
positive, exact, irreducibly 3D (both Mx and My bind). Reds: (i) corner normals = (W/2,W/4,W/4); (ii)
with crushing cap C, λ* = C/max(Ri) = **2C/W**. Measure rows/cols/nonzeros/pivots vs the 2D two-support
fixture → the first MEASURED 3D multiplier.

**E0-B — FOUR corners (1× indeterminate).** The real shed case (redundant supports); assert λ* against a
bound, not unique reactions. This is where the friction-pyramid degeneracy + mechanism non-uniqueness
(R3) first bite — E2's determinism fuzz must own it.

Both hand-built (no bridge), mirroring `RigidBlockOracleTest.cpp:137–181`.

## E1 slice sequence

- **E1a — 3D equilibrium + 4-corner assembly behind the flag.** Red: `OracleThreeD.TripodMatchesHandStatics`
  (E0-A (i)+(ii)). Green: enum + data fields + `AssembleThreeD` (six rows, four contacts, tension/crush,
  friction off via huge μ or omitted). Guard: 2D bit-identity theorem (sweep + all 2D oracle rows
  unchanged). Bite: zeroing the Mx/My coefficients reds the tripod.
- **E1b — friction pyramid.** Red: `OracleThreeD.SlidingOnAPyramidFacetAndOnADiagonal` (a 3D push with u
  AND v components; facet push λ*=(cA+μW)/|push|; DIAGONAL push exposes inscribed-vs-circumscribed by √2).
  Bite: inscribed↔circumscribed moves the diagonal red by √2.
- **E1c — tension/biaxial first-crack — DEFER to E-tail** unless a shed fixture needs bonded biaxial
  discrimination.

**Follow-ons:** **E2** — mechanism in 3D (lift `v=u+ω×(p−c)` to a 3-vector ω; re-derive per-group
canonicalisation + Farkas; re-run R3's determinism fuzz on E0-B; **likely build the D7 minimal-support
tie-break 2D deferred** — highest-risk follow-on). **E3** — the 3D bridge (lift the Y-normal refusal for
Dim3D only, keep the 2D refusal loud; replace the `|Nz|vs|Nx|` half-extent shortcut with the two real
interface axes). **E4** — block coarsening (coarse blocks + per-brick refinement near a cut; the real
tractability lever). **E5** — 3D latency ladder + block-cap re-measure + R7 Shipping-FP in 3D.

## Biggest risks

1. **Friction-pyramid degeneracy worsens R3 mechanism determinism (TOP risk).** 6-DOF motion + k facets
   binding + redundant supports (E0-B) = multiple Farkas rays. E2 must re-run the determinism fuzz in 3D
   and likely build the D7 tie-break. **Mitigation:** run E0-B through a permutation fuzz EARLY (even
   during E1) to measure whether block-velocity triples stay permutation-unique under the pyramid.
2. **Solve-cost blow-up → interactive band a few dozen blocks.** Makes E4 coarsening load-bearing.
   **Mitigation:** E0's measured multiplier vs the 2D ladder is the R-Scope re-confirmation gate.
3. **2D bit-identity via the NumRows tolerance coupling.** Keep 2D a literal separate assembler; make
   "sweep + all 2D oracle tests unchanged" an E1a acceptance gate.

**Unsure / to resolve:** the biaxial plastic/elastic section-modulus ratio (short derivation before
E1c); the inscribed-pyramid normalisation constant (the E1b diagonal red forces the √2 right); whether
E0-B's indeterminate λ* pins by hand or needs a window (solve once and read the digits).
