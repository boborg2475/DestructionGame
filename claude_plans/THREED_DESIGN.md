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
replace with a k=8 inscribed octagon. Each facet `cosθ_i·(p_u−q_u) + sinθ_i·(p_v−q_v) ≤ d`, `θ_i=i·π/4`,
constrains the shear to a half-plane at distance `d` from the axis. **CRITICAL — the facet distance is
`d = cos(π/8)·(cA+μn)`, NOT `cA+μn` [corrected 2026-08-30 after the E1b review caught the original
formula was CIRCUMSCRIBED].** A facet at distance `d = cA+μn` (the true cone radius R) puts the octagon's
VERTICES at `R/cos(π/8) ≈ 1.082R` — the octagon then CONTAINS the cone (circumscribed), letting a contact
carry up to 8.2% more shear than it has in the 22.5°/67.5° vertex directions → certifies a sliding
collapse as standing (the "plausible number, wrong statics" failure the project exists to avoid). To
INSCRIBE (vertices ON the cone, facets pulled in), scale the capacity by `cos(π/8) ≈ 0.9239`, so per
corner (tributary A/4): `cosθ_i·(p_u−q_u) + sinθ_i·(p_v−q_v) − cos(π/8)·μ·(n+−n-) ≤ cos(π/8)·c·Conv·A/4`.
Then every direction reads ≤ the true cone (facets 0.9239·R conservative, vertices exactly R). **Inscribed
(conservative) is not a free choice:** the oracle is the safe lower-bound theorem ("is there ANY admissible
equilibrium"); inscribing occasionally fells a structure the true cone stands — the honest, safe direction
for a demolition gate. `k` is a constant so the accuracy/cost trade is one number. **The test that pins
this axis is a 22.5° VERTEX-direction push** (a facet/diagonal push cannot distinguish inscribed from
circumscribed k=8): it must read ≤ R/P (the true-cone value), which circumscribed violates by 1.082×.

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

- **E1a — 3D equilibrium + 4-corner assembly — DONE (2026-08-30).** `AssembleThreeD` (`RigidBlockOracle.cpp`):
  `DeriveInPlaneAxes` (deterministic right-handed frame from N̂), `BuildThreeDContacts` (4 corners, a point
  patch collapsing to coincident normal contacts, A/4 tributary), `AppendThreeDContactCoeffs` (force rows =
  components of N̂/û/v̂, moment rows = components of `r×ê` — the My row reproduces the 2D moment coefficient,
  Mx/Mz added), six equilibrium equalities per non-grounded block, tension `n-≤f_t·Conv·A/4` + crush
  `n+−n-≤f_c·Conv·A/4` (friction OFF — E1b). Branched at `SolveRigidBlockOnce` (maximise-λ) and via a
  standalone `SolveMinViolationReadoutThreeD` (the 2D readout untouched; a single min-sum solve — determinate
  tripod). Driven by `Oracle.RigidBlock.ThreeD.TripodMatchesHandStatics`: reactions (4900,2450,2450) =
  (W/2,W/4,W/4), λ*=2; the moment machinery bites (zeroing My reverts to the 2D-degenerate (7350,0,2450)/2.667).
  **2D BIT-IDENTITY PROVEN: `OracleSweepFull` 5/5 byte-identical, suite 206 = 202 green + 4 standing reds**
  (the 3D branch never runs for a Dim2D problem). **E0 tractability MEASURED (R-Scope gate — 3D is tractable
  toy-first):** 3D tripod vs 2D two-support — rows 1.19×, cols 4.3×, nonzeros 3.6×, pivots 2×, pricing scans
  7.3× (per-fixture, 3 vs 2 supports; friction-off FLOOR — E1b's k=8 pyramid adds 8 rows/contact and will
  dominate; consistent with the ~8–12× compact / up to ~25–30× estimate). **E1a scope boundaries (deferred, sound):**
  applied forces are not posed on the 3D path (no fixture needs one; flagged before adding untested branching);
  `ValidateProblem` doesn't yet unit-check `NormalY` (E3 bridge); 3D mechanism extraction is E2 (`EqFxRowOfBlock`
  left INDEX_NONE — safe, the E1a load-factor pose is gravity-live/feasible so the infeasible arm never reads it);
  the 3D readout uses a single min-sum not the per-group lex-minimax (E2, for the indeterminate E0-B). Cosmetic:
  the 2D `else`-block is left at its original (one-level-shallow) indentation to avoid a 350-line re-indent diff —
  functionally verbatim, byte-identical by the sweep; re-indent is a possible follow-up.
- **E1b — friction pyramid — DONE (2026-08-30, inscribed + reviewed).** The E1b review caught the pyramid was
  first CIRCUMSCRIBED (a safety bug); the fix inscribes it (`ThreeDPyramidInscribeFactor = cos(π/8)` scaling
  both the μ term and the cohesion RHS at both sites), re-review confirmed genuinely conservative. k=8 pyramid
  (`ThreeDFrictionPyramidFacets=8`): 8 facet rows/contact, θ_i=i·2π/8, gated on cohesion as the 2D Coulomb rows;
  a frictionless contact (μ=c=0) writes 8 zero-RHS rows → shear pinned to 0. The `r_app×F` 3D applied-force
  posing (E1a deferred) is CORRECT and reviewed (force + moment into the six rows, live→λ / dead→RHS, mirrored in
  the readout), and the E1a-review sign-lock `ThreeD.AsymmetricMomentMatchesHandStatics` is GREEN and non-vacuous
  (5-support, off-diagonal CoM, supports at different heights → reactions 3675/1225/4900/4900/4900, λ*=2; Mx↔My
  swap and Mz-negate each red it). **THE PYRAMID BUG:** the first implementation wrote the facet distance at the
  true cone radius R (`≤ c·Conv·A/4`), which is CIRCUMSCRIBED — the octagon contains the cone, permitting 8.2%
  excess shear in the 22.5° vertex directions (a 22.5° push read λ*=2.165 = 1.082·R/P, certifying a slide as
  standing). The two original arms (facet 0° / diagonal 45°) push along facet normals and cannot see it. **Fix:
  inscribe** — scale the facet capacity by cos(π/8) (§3 corrected), so vertices sit on the cone; the sliding test
  gets a 22.5° VERTEX arm asserting ≤ R/P, and its facet/diagonal expectations move to 2·cos(π/8)=1.848. Bites verified: inscribed→circumscribed moves the
  diagonal 2.0→2.828. **2D byte-identical (OracleSweepFull 5/5); suite 208 = 204 green + 4 standing reds.**
  Deferred (E3/validation): `ValidateProblem` doesn't finiteness-check `ForceYUu`/`AtYCm` (with the `NormalY`
  unit-check gap) — a NaN 3D applied-force Y would flow unchecked; harden when the bridge carries real 3D forces.
- **E1c — tension/biaxial first-crack — DEFER to E-tail** unless a shed fixture needs bonded biaxial
  discrimination.

**Follow-ons:** **E2 — mechanism in 3D. E2a DONE (2026-08-30):** `AssembleThreeD` populates the six
`EqFxRowOfBlock` rows; `ExtractMechanism` branches on `Dim` (2D byte-identical) to read the six-row dual
into `(u,ω)` 6-vectors, computes the virtual velocity `v=u+ω×(p−c)` with a full 3-vector cross product
(a separate `VelocityAt3D` — the 3D My row is `+(r×e)_y`, so the standard cross product is right, not the
2D scalar sign-flip), derives the moving set + opening joints from the 3D 4-corner block kinematics
(`BlockL1` over all six components), and reuses the dimension-agnostic Farkas. Driven by
`ThreeD.MechanismTipsAboutABaseEdge` (a block tipping about the +Y base edge → ω about X, out of plane,
unrepresentable by the 2D scalar ω; Uz<0 descends). 2D mechanism tests + OracleSweepFull byte-identical;
suite 209 = 205 green + 4 standing reds. **E2c DONE (2026-08-30) — the top risk is retired for the shed's
cases: the 3D mechanism is PERMUTATION-DETERMINISTIC.** `ThreeD.MechanismIsPermutationDeterministic` fuzzes
block/joint/column order across 8 seeds over TWO falling fixtures — the determinate tipping block AND a
redundant FOUR-support fixture (1× statically indeterminate + k=8 pyramid shear freedom, CoM past the +Y
support line) — asserting the moving set, the opening set, and each block's (u,ω) 6-vector NORMALISED to a
unit ray (the Farkas ray is defined up to positive scale) are invariant. BOTH green, worst unit-drift
0.000e+00; gate bites (a column-order-dependent term injected into the extraction reds it). This VALIDATES
the design bet — reading the break set from the stable block KINEMATICS, not the non-unique multipliers,
keeps 3D deterministic just as 2D's Slice 3a. So **E2b canonicalisation is NOT needed for these cases** and
E2 is complete for the shed (whose blocks bear on faces/edges → pinned axes → deterministic). **CAVEAT
(deferred hardening, not shed-relevant):** the redundant fixture's non-uniqueness lived in the PRIMAL, but
its block-velocity DUAL came out unique (0 Bland pivots), so it did not exercise a heavily-degenerate
multiplier regime; a genuinely 2-DOF KINEMATICALLY-AMBIGUOUS mechanism (a block point-pivoting with an
unpinned rotation axis — a different fixture shape) is the harder stressor and IS where determinism could
break, needing the D7 minimal-support tie-break / E2b canonicalisation. That is a degenerate point-contact
corner case a face/edge-bearing shed does not hit; build it only if a 3D shed fixture ever shows a
point-pivot non-determinism. **E3 — the 3D bridge — DONE (2026-08-30): 3D IS NOW REACHABLE FROM PRODUCTION.**
`BuildRigidBlockProblem` branches on a new `FStructure::IsThreeDimensional()` flag (a structure-level SIGNAL,
NOT inference — a 2D structure's stray Y-normal is still refused). `FStructure`/`FConnection` already carried
full 3D joint geometry (3D `InterfaceNormal`/`InterfaceCentreCm`/per-axis `InterfaceHalfExtentCm`, 3D
`CentreOfMassCm`; `AddConnection` validates the axis-aligned Y-normal rectangle), so E3 just POSES it: sets
`Dim=Dim3D`, the block's `CentroidYCm`, the joint `NormalY`/`CentreYCm`, and `HalfUCm/HalfVCm` mapped via the
now-SHARED `DeriveInPlaneAxes` (bridge + oracle use the same (U,V) frame — the bridge projects the per-axis
half-extents onto U,V). `EffectiveJointStrength`/`ConnectionOfJoint`/`PieceOfBlock` carry over unchanged.
**Also closed the E1a-flagged NormalY validation gap:** `ValidateProblem`'s normal-unit-length check is now
dimension-aware (Dim3D includes NormalY in finiteness + length; Dim2D byte-identical, so a 2D structure with a
pure-Y normal still fails its X-Z unit check and is refused). Driven by `ThreeD.BridgePosesAnOutOfPlaneJoint`
(a block bonded to a grounded wall across a +Y joint, unequal half-extents {10,15}: bridge accepts, Dim3D,
normal (0,1,0), centreY 5.5, halfU/halfV 10/15, area 600, block CentroidY 11; STANDS with a huge bond → shear
carries the −Z weight, FALLS bondless → free block descends). 2D untouched (the 3D branch runs only under the
flag): OracleSweepFull 5/5 byte-identical, narrowed 27/27, the 2D refusal intact at both the bridge and the
validator. Suite 211 = 207 green + 4 standing reds. **So the 3D LP is COMPLETE and REACHABLE** — forward
statics (E1) + deterministic mechanism (E2) + the production bridge (E3). Next: a 3D shed builder + scenario +
rendered playtest. **E4** — block coarsening (coarse blocks + per-brick refinement near a cut; the real
tractability lever, only if the 3D shed exceeds the cap). **E5** — 3D latency ladder + block-cap re-measure + R7 Shipping-FP in 3D.

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
