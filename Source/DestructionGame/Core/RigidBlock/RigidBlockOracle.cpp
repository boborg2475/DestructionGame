// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockFactor.h"

/*
 * THE ORACLE'S IMPLEMENTATION. The formulation and every modelling decision are
 * documented in RigidBlockOracle.h; this file is the LP assembly and a SPARSE REVISED
 * SIMPLEX (rewritten 2026-08-12 from the original dense two-phase tableau, whose
 * accumulated pivot error refused every fixture past ~4,000 pivots — the measured
 * envelope in RigidBlockOracleSweepTest.cpp's header). Independence rules observed
 * here: no production arithmetic is called (the only production types read are the
 * plain data structs), the unit conversion is derived locally, and every pivoting
 * tie-break is index-based over the input order so results are bit-reproducible.
 *
 * WHY REVISED, AND WHAT RESETS THE ERROR. A dense tableau updates O(rows x columns)
 * numbers per pivot and every one carries one rounding forward forever — the error
 * is cumulative and unrepairable, which is exactly what the post-solve verification
 * kept refusing. The revised method keeps the constraint matrix UNTOUCHED in sparse
 * column form and represents only the BASIS: an LU factorisation plus a short
 * product-form eta file, refactorised FROM THE ORIGINAL CLEAN DATA every
 * RefactoriseEvery pivots. Refactorisation both bounds the per-iteration cost (the
 * eta file never grows past the cadence) and DISCARDS all accumulated rounding —
 * after it, the only error in play is one clean factorisation and at most 64 eta
 * applications, regardless of how many thousand pivots came before. The basic values
 * are recomputed from the original right-hand side at every refactorisation, and one
 * final refactorisation precedes extraction so verification judges the cleanest
 * solve the basis admits.
 */
namespace RigidBlockOracle
{
	namespace OracleDetail
	{
		/* Pivot / reduced-cost tolerances on ROW-SCALED data (max |coefficient| = 1). */
		constexpr double PivotTol = 1.0e-9;
		constexpr double CostTol = 1.0e-9;

		/*
		 * A ratio-test pivot must ALSO stand up against the entering column's own scale,
		 * and the absolute tolerance above cannot express that. The rows are equilibrated
		 * but B^-1 is not: the FTRAN image w = B^-1 A_j of an ordinary column was measured
		 * on the opening-ladder family with |w| running to 3e7, and the ratio test then
		 * accepted pivots of 4.7e-9, 6.9e-9 and 6.7e-9 out of such columns — RELATIVE
		 * pivots of ~1e-16, which is to say it pivoted on the rounding of the solve that
		 * produced w. Two refactorisations later the LU found the basis those pivots built
		 * singular at 1.3e-13 and the whole solve was refused. Whatever error w carries
		 * scales with |w|, so the floor under a believable pivot has to as well.
		 *
		 * A floor at a fixed fraction of the column's largest magnitude sits above the
		 * noise that reaches w through the LU and the eta file, and orders below any
		 * element carrying information: on the measured columns 1e-11 rejected 4.7e-9 out
		 * of 3e7 (a floor of 3e-4) while admitting the 0.010, 0.026 and 0.033 pivots taken
		 * in the same stretch.
		 *
		 * 1e-11 WAS NOT ENOUGH, AND 1e-9 IS THE SAME FIX ONE NOTCH FURTHER (2026-08-15).
		 * The eight-course-cover family refused two walls — an 8-cell opening at 128 blocks
		 * and a 16-cell at 200 — while every neighbour either side answered, and the arm was
		 * NumericalFailure from the periodic refactorisation: Factorise finding an LU pivot
		 * of 4.38e-12 at position 4018 of 4021, and 4.93e-12 at 6322 of 6325. What makes
		 * that a statement about THIS constant rather than about the factorisation is where
		 * the factorisation gets its data: Refactorise builds the LU from the ORIGINAL
		 * sparse columns and never from the eta file, so an LU that goes singular is saying
		 * the BASIS COLUMN SET is numerically dependent — a basis the ratio test chose. The
		 * product of U's diagonal IS det(B), so once the set is dependent no ordering of the
		 * elimination can make its pivots large; a fill-reducing or Markowitz ordering
		 * (roadmapped, and worth having for speed) is not a repair for this. The repair is
		 * not to build that basis.
		 *
		 * 1e-9 of the entering column's own scale is still seven orders above where double
		 * epsilon puts the noise, and the band 1e-11 left admissible was rounding-grade by
		 * exactly that argument. Both refusers answer with the change (lambda* =
		 * 155.63200561101226 and 43.132916253688222), and a SECOND, independent variant —
		 * RefactoriseEvery 64 -> 16, this constant untouched, a visibly different pivot
		 * path — answers 155.63200342392039 and 43.132560867194137, agreeing to 1.4e-8 and
		 * 8.2e-6. The 128-block reading lands on the family's own L^-2.4 interpolation
		 * between its neighbours (~156), which is a third derivation, from the physics
		 * rather than from the solver. The cadence lever was not the one taken:
		 * refactorisation is already a third of runtime, so quadrupling it pays everywhere
		 * for a defect that lives in one place.
		 *
		 * WHAT ELSE WAS TRIED AND MEASURED, so nobody spends the instrumented build again:
		 *
		 *   - A RELATIVE CANDIDACY TOLERANCE IN THE BLAND BRANCH IS INERT. Scaling
		 *     -CostTol by the largest term in the reduced cost's own dot product changed
		 *     not one pivot on any of the six ladder rungs: at the failure the column's
		 *     terms are ~1.2e-9, so the scaled tolerance is the absolute one.
		 *
		 *   - THE ~1e-9 OF DRIFT IS INHERITED FROM THE DUAL SOLVE, not from cancellation
		 *     in that column. Measured at the refusal: ||y||inf = 6.7e6, and machine
		 *     epsilon against that IS 6.7e-10. A reduced cost of zero cannot be computed
		 *     to better than the duals it is priced against.
		 *
		 *   - SCALING THE OPTIMALITY TOLERANCE BY ||y|| WAS REJECTED, and this is the one
		 *     worth remembering: it would put the tolerance at ~6.7e-3, and a simplex that
		 *     stops while real improvements of 1e-3 remain reports a FEASIBLE point with a
		 *     lambda* that is too LOW. The post-solve verification checks admissibility,
		 *     not optimality, so it certifies such an answer happily — under-reporting is
		 *     the one direction this oracle cannot catch, and no tolerance here may buy
		 *     an answer at that price.
		 *
		 * ONE CONSEQUENCE OF THE FLOOR, stated because it is a real (bounded) cost rather
		 * than a free win: a coefficient in (PivotTol, PivotFloor] is now skipped, so the
		 * ratio test can choose a longer step than that row would have allowed and drive
		 * its basic value slightly negative. ApplyPivot clamps that to zero; the magnitude
		 * is bounded by theta * PivotFloor, and the final refactorisation recomputes every
		 * basic value from the original right-hand side before the 1e-6 verification
		 * judges it — in the direction verification can see, which is admissibility. Raising
		 * the floor raises that bound with it, which is why the whole slow group is re-run
		 * against the 1e-6 gate whenever this number moves rather than the two walls alone.
		 */
		constexpr double RelativePivotTol = 1.0e-9;

		/*
		 * This cap IS the termination guarantee, not defence in depth: the pivoting is
		 * Dantzig with an entering-only Bland fallback, whose leaving rule is not
		 * Bland's, so the classical no-cycling theorem does not apply. Hitting the cap
		 * reports failure rather than a number.
		 */
		constexpr int32 MaxPivots = 100000;

		/*
		 * The refactorisation cadence: how many eta updates accumulate before the basis
		 * is refactorised from the original columns and the basic values recomputed
		 * from the original right-hand side. Smaller resets error more often and keeps
		 * FTRAN/BTRAN shorter; larger amortises the factorisation. 64 mirrors the dense
		 * solver's reduced-cost rebuild cadence and measured comfortably inside every
		 * validation tolerance.
		 */
		constexpr int32 RefactoriseEvery = 64;


		/*
		 * THE RELATIVE THRESHOLD tau THAT TURNS THE NON-UNIQUE DEGENERATE DUAL INTO A STABLE
		 * NAMED SET (PROMOTION_DESIGN §3.3, §12 D7). The Farkas certificate is defined only up
		 * to a positive scale, so the mechanism is normalized — each block's virtual-motion
		 * triple against the LARGEST triple, each joint's relative-velocity against the largest
		 * relative velocity — and a block MOVES (a joint OPENS) iff its normalized magnitude
		 * clears tau. It is deliberately RELATIVE: a block's translation and rotation duals
		 * differ by orders of magnitude (a rotation about a far fulcrum makes the centroid
		 * velocity |u| ~ omega * lever), so no absolute cut-off could separate "moves" from
		 * "still" across fixtures of different sizes.
		 *
		 * WHY 1e-6, DERIVED AGAINST THE FUZZ NOT EYEBALLED. Two floors bracket it. BELOW: a block
		 * that does not participate reads a dual of pure rounding — grounded blocks are EXACTLY
		 * zero (they write no rows), and an uninvolved free block's triple is the drift the dual
		 * carries, bounded by the ~1e-9 relative floor the pivot tolerances are built on
		 * (RelativePivotTol's note: a reduced cost of zero is computable only to ~1e-9 of ||y||).
		 * ABOVE: a participating block reads an O(1) fraction of the normaliser. The permutation
		 * fuzz (OracleMechanismExtractionTest's IsPermutationDeterministic) MEASURES the gap on
		 * every fixture and seed — including a 24-course dry leaning stack whose block velocities
		 * agree across permutations to 1e-15 — and the smallest moving magnitude and largest still
		 * magnitude are separated by many orders, so 1e-6 sits three orders above the rounding
		 * floor and well below any genuine motion and every block/joint lands on the SAME side of
		 * tau under every column permutation. THE JOINT SET IS READ FROM THE BLOCK KINEMATICS, NOT
		 * THE RAW PLASTIC MULTIPLIERS: those same 24 courses proved the multipliers are the part
		 * of the degenerate dual that is non-unique (they named 8 opening joints one column order,
		 * 13-16 another) while the block velocities are unique, so a joint gives iff its two
		 * blocks have a non-zero relative velocity at a contact — a function of the stable triples
		 * (see ExtractMechanism). That stability, not the exact value of tau, is the contract; the
		 * test is what forbids picking either by eye, because only a tau in the gap and a joint
		 * criterion off the unique kinematics pass it.
		 */
		constexpr double MechanismRelativeTol = 1.0e-6;


		/*
		 * A strength at or beyond this is "no cap at all" (Unbreakable's 1e12, the
		 * MaxShear default of DBL_MAX): the row is omitted rather than written with an
		 * astronomically large right-hand side that would wreck the problem's scaling.
		 */
		constexpr double UncappedStrengthMPa = 1.0e9;

		/*
		 * PARTIAL PRICING: how many columns one refill window prices, and how many
		 * candidates it keeps. Full Dantzig prices every non-basic column every
		 * iteration, which is pivots x columns and is what left the 30-course walls
		 * pivoting for tens of minutes; the window prices a bounded slice instead and
		 * the queue spreads that slice's cost over the iterations that follow it.
		 *
		 * WHAT THE WINDOW ACTUALLY COSTS, measured rather than assumed. A refill that
		 * finds nothing takes the NEXT window and keeps going, so what a refill really
		 * spends is the distance from the cursor to the next attractive column, rounded
		 * up to a window — the size buys a bigger pool for the ranking to choose from,
		 * and pays for it in the rounding. Measured on the 8x10 wall row of
		 * Oracle.RigidBlock.PricingCost, whose budget is the reason this exists:
		 *
		 *     384 -> 2,016 pivots, 538,200 scans      (chosen: budgets are 4,000/1,000,000)
		 *     768 -> 1,836 pivots, 651,203 scans
		 *
		 * — a wider window really does shorten the pivot path, and really does cost more
		 * than it saves in scans, which is why the scan-heavy end of the trade is the one
		 * taken here. Both answers are the same lambda* to the last bit.
		 */
		constexpr int32 PricingWindowCols = 384;
		constexpr int32 PricingQueueDepth = 12;

		/** One structural row before slacks: sparse coefficients over structural columns. */
		struct FAssemblyRow
		{
			TArray<int32> Col;
			TArray<double> Val;
			double Rhs = 0.0;
			bool bEquality = true;

			void Add(int32 InCol, double InVal)
			{
				if (InVal != 0.0)
				{
					Col.Add(InCol);
					Val.Add(InVal);
				}
			}
		};

		bool FiniteNonNegative(double Value)
		{
			/* IsFinite is what rejects NaN — the sign clause alone would pass it. */
			return FMath::IsFinite(Value) && !(Value < 0.0);
		}

		/** Empty string means valid; otherwise the reason, named to the offending row. */
		FString ValidateProblem(const FOracleProblem& Problem)
		{
			for (int32 Index = 0; Index < Problem.Blocks.Num(); ++Index)
			{
				const FOracleBlock& Block = Problem.Blocks[Index];

				if (!FiniteNonNegative(Block.MassKg))
				{
					return FString::Printf(TEXT("block %d: mass must be finite and non-negative"), Index);
				}

				if (!FMath::IsFinite(Block.CentroidXCm) || !FMath::IsFinite(Block.CentroidZCm))
				{
					return FString::Printf(TEXT("block %d: centroid must be finite"), Index);
				}
			}

			for (int32 Index = 0; Index < Problem.Joints.Num(); ++Index)
			{
				const FOracleJoint& Joint = Problem.Joints[Index];

				if (Joint.BlockA < 0 || Joint.BlockA >= Problem.Blocks.Num()
					|| Joint.BlockB < 0 || Joint.BlockB >= Problem.Blocks.Num())
				{
					return FString::Printf(TEXT("joint %d: block index out of range"), Index);
				}

				if (Joint.BlockA == Joint.BlockB)
				{
					return FString::Printf(TEXT("joint %d: joins a block to itself"), Index);
				}

				if (!FMath::IsFinite(Joint.NormalX) || !FMath::IsFinite(Joint.NormalZ))
				{
					return FString::Printf(TEXT("joint %d: normal must be finite"), Index);
				}

				const double NormalLength =
					FMath::Sqrt(Joint.NormalX * Joint.NormalX + Joint.NormalZ * Joint.NormalZ);

				/* Validates, never normalises — same door policy as AddConnection. */
				if (!(FMath::Abs(NormalLength - 1.0) <= 1.0e-9))
				{
					return FString::Printf(TEXT("joint %d: normal must be unit length"), Index);
				}

				if (!FMath::IsFinite(Joint.CentreXCm) || !FMath::IsFinite(Joint.CentreZCm))
				{
					return FString::Printf(TEXT("joint %d: centre must be finite"), Index);
				}

				if (!FiniteNonNegative(Joint.HalfLengthCm))
				{
					return FString::Printf(
						TEXT("joint %d: half length must be finite and non-negative"), Index);
				}

				if (!(Joint.AreaSqCm > 0.0) || !FMath::IsFinite(Joint.AreaSqCm))
				{
					return FString::Printf(TEXT("joint %d: area must be finite and positive"), Index);
				}

				const FConnectionStrength& S = Joint.Strength;

				/*
				 * NaN anywhere in a strength is REFUSED — including the shear ceiling,
				 * deliberately the opposite polarity to production's recorded
				 * NaN-laundering hazard, where a NaN cap silently compares as uncapped.
				 */
				if (!FiniteNonNegative(S.CompressiveStrengthMPa)
					|| !FiniteNonNegative(S.ShearCohesionMPa)
					|| !FiniteNonNegative(S.TensileStrengthMPa)
					|| !FiniteNonNegative(S.FrictionCoefficient)
					|| S.MaxShearStrengthMPa != S.MaxShearStrengthMPa
					|| S.MaxShearStrengthMPa < 0.0)
				{
					return FString::Printf(
						TEXT("joint %d: strengths must be finite (or the documented ")
						TEXT("unbounded ceiling) and non-negative"), Index);
				}
			}

			for (int32 Index = 0; Index < Problem.AppliedForces.Num(); ++Index)
			{
				const FOracleAppliedForce& Applied = Problem.AppliedForces[Index];

				if (Applied.Block < 0 || Applied.Block >= Problem.Blocks.Num())
				{
					return FString::Printf(
						TEXT("applied force %d: block index out of range"), Index);
				}

				if (!FMath::IsFinite(Applied.ForceXUu) || !FMath::IsFinite(Applied.ForceZUu)
					|| !FMath::IsFinite(Applied.AtXCm) || !FMath::IsFinite(Applied.AtZCm))
				{
					return FString::Printf(TEXT("applied force %d: must be finite"), Index);
				}
			}

			return FString();
		}


		/**
		 * Build the standard form from the assembly rows. Same conventions as the dense
		 * solver, kept deliberately: equilibrate over the COEFFICIENTS ONLY, never the
		 * right-hand side (scaling by the lambda cap's 1e6 shrinks a row's real
		 * coefficients toward the pivot tolerance and an uncapped problem then reads
		 * "unbounded" — measured before that comment was first written); flip any row
		 * whose scaled right-hand side is negative; start from the slack where it
		 * survives the flip at +1, else from an artificial.
		 */
		void BuildStandardForm(
			const TArray<FAssemblyRow>& AssemblyRows, int32 NumStructCols, FStandardForm& Out)
		{
			const int32 NumRows = AssemblyRows.Num();

			int32 NumSlacks = 0;

			for (const FAssemblyRow& Row : AssemblyRows)
			{
				if (!Row.bEquality)
				{
					++NumSlacks;
				}
			}

			Out.NumRows = NumRows;
			Out.NumStructCols = NumStructCols;
			Out.ArtificialStart = NumStructCols + NumSlacks;
			Out.NumCols = Out.ArtificialStart + NumRows;
			Out.Rhs.SetNumZeroed(NumRows);
			Out.RowScaleSigned.SetNumZeroed(NumRows);
			Out.InitialBasis.SetNum(NumRows);

			TArray<double> RowScale;
			TArray<bool> RowFlip;
			TArray<int32> RowSlackCol;
			RowScale.SetNum(NumRows);
			RowFlip.SetNum(NumRows);
			RowSlackCol.Init(INDEX_NONE, NumRows);

			int32 NextSlack = NumStructCols;

			for (int32 RowIndex = 0; RowIndex < NumRows; ++RowIndex)
			{
				const FAssemblyRow& Row = AssemblyRows[RowIndex];

				double Largest = 0.0;

				for (double Coefficient : Row.Val)
				{
					Largest = FMath::Max(Largest, FMath::Abs(Coefficient));
				}

				const double Scale = Largest > 0.0 ? 1.0 / Largest : 1.0;
				const double ScaledRhs = Row.Rhs * Scale;
				const bool bFlip = ScaledRhs < 0.0;

				RowScale[RowIndex] = Scale;
				RowFlip[RowIndex] = bFlip;
				Out.Rhs[RowIndex] = bFlip ? -ScaledRhs : ScaledRhs;
				Out.RowScaleSigned[RowIndex] = bFlip ? -Scale : Scale;

				if (!Row.bEquality)
				{
					RowSlackCol[RowIndex] = NextSlack++;
				}

				/* Basis: the slack where it is still +1 after orientation, else artificial. */
				if (RowSlackCol[RowIndex] != INDEX_NONE && !bFlip)
				{
					Out.InitialBasis[RowIndex] = RowSlackCol[RowIndex];
				}
				else
				{
					Out.InitialBasis[RowIndex] = Out.ArtificialStart + RowIndex;
				}
			}

			/* Count nonzeros per column, then fill by ascending row for determinism. */
			TArray<int32> Count;
			Count.SetNumZeroed(Out.NumCols);

			for (int32 RowIndex = 0; RowIndex < NumRows; ++RowIndex)
			{
				for (int32 Col : AssemblyRows[RowIndex].Col)
				{
					++Count[Col];
				}

				if (RowSlackCol[RowIndex] != INDEX_NONE)
				{
					++Count[RowSlackCol[RowIndex]];
				}

				if (Out.InitialBasis[RowIndex] >= Out.ArtificialStart)
				{
					++Count[Out.InitialBasis[RowIndex]];
				}
			}

			Out.ColStart.SetNum(Out.NumCols + 1);
			Out.ColStart[0] = 0;

			for (int32 Col = 0; Col < Out.NumCols; ++Col)
			{
				Out.ColStart[Col + 1] = Out.ColStart[Col] + Count[Col];
			}

			Out.ColRow.SetNum(Out.ColStart[Out.NumCols]);
			Out.ColVal.SetNum(Out.ColStart[Out.NumCols]);

			TArray<int32> Cursor = Out.ColStart;

			for (int32 RowIndex = 0; RowIndex < NumRows; ++RowIndex)
			{
				const FAssemblyRow& Row = AssemblyRows[RowIndex];
				const double Sign = RowFlip[RowIndex] ? -1.0 : 1.0;
				const double Scale = RowScale[RowIndex] * Sign;

				for (int32 Entry = 0; Entry < Row.Col.Num(); ++Entry)
				{
					const int32 Col = Row.Col[Entry];
					const int32 At = Cursor[Col]++;
					Out.ColRow[At] = RowIndex;
					Out.ColVal[At] = Row.Val[Entry] * Scale;
				}

				if (RowSlackCol[RowIndex] != INDEX_NONE)
				{
					const int32 At = Cursor[RowSlackCol[RowIndex]]++;
					Out.ColRow[At] = RowIndex;
					Out.ColVal[At] = Sign;
				}

				if (Out.InitialBasis[RowIndex] >= Out.ArtificialStart)
				{
					const int32 At = Cursor[Out.InitialBasis[RowIndex]]++;
					Out.ColRow[At] = RowIndex;
					Out.ColVal[At] = 1.0;
				}
			}

			/* The static pricing weights, off the finished matrix and nothing else. */
			Out.ColNorm.SetNum(Out.NumCols);

			for (int32 Col = 0; Col < Out.NumCols; ++Col)
			{
				double SumOfSquares = 0.0;

				for (int32 At = Out.ColStart[Col]; At < Out.ColStart[Col + 1]; ++At)
				{
					SumOfSquares += Out.ColVal[At] * Out.ColVal[At];
				}

				Out.ColNorm[Col] = FMath::Sqrt(1.0 + SumOfSquares);
			}
		}



		/**
		 * APPEND -A_Source TO THE MATRIX AND RETURN ITS INDEX. It lands past every
		 * artificial, so it is an artificial by index: phase 1 prices it and pays 1 per unit
		 * of it, phase 2 (which prices only [0, ArtificialStart)) cannot enter it, the
		 * pivot-out pass treats it as one to clear, and the answer extraction ignores it.
		 * Negation preserves a column's sum of squares exactly, so its pricing norm is the
		 * source's own bits rather than a re-derivation of them.
		 *
		 * ONLY A WARM START EVER REACHES HERE. Appending columns moves nothing that already
		 * exists — the structural, slack and artificial blocks keep their indices — but a
		 * column the cold pricer could see would move the window, the queue and therefore
		 * every pinned cold pivot count in the sweep.
		 */
		int32 AppendNegatedColumn(FStandardForm& Form, int32 Source)
		{
			const int32 Appended = Form.NumCols;
			const int32 End = Form.ColStart[Source + 1];

			for (int32 At = Form.ColStart[Source]; At < End; ++At)
			{
				/* Its own locals: Add(Array[At]) can alias its storage across a grow (TRAPS). */
				const int32 Row = Form.ColRow[At];
				const double Value = Form.ColVal[At];

				Form.ColRow.Add(Row);
				Form.ColVal.Add(-Value);
			}

			const double SourceNorm = Form.ColNorm[Source];

			Form.ColStart.Add(Form.ColRow.Num());
			Form.ColNorm.Add(SourceNorm);
			Form.NumCols = Form.ColStart.Num() - 1;

			return Appended;
		}

		/**
		 * SEED THE BASIS FROM A CALLER'S WARM START, AND RETURN HOW MUCH OF IT SURVIVED.
		 * PROMOTION_DESIGN.md §5.4's lever. The hint is one column per row; every entry that
		 * cannot be used is REPAIRED to that row's cold default rather than refused, because
		 * a warm start that fails closed measures nothing (FOracleResult's field is what keeps
		 * the repair honest — a hint thrown away and a hint that saved nothing are opposite
		 * findings and only the count tells them apart).
		 *
		 * IT MAY NOT CHANGE THE ANSWER, AND HERE IS WHY IT CANNOT. Everything below either
		 * chooses a starting basis — which the simplex is free to choose anyway — or ADDS an
		 * artificial column. Artificials only enlarge the feasible set of the phase-1 problem
		 * and are priced out of phase 2 entirely, so the phase-1 optimum is still zero exactly
		 * when the original rows admit a solution, and the answer phase 2 then maximises is
		 * over the same columns as ever. The post-solve verification against the ORIGINAL
		 * assembly rows is unweakened and remains the gate.
		 *
		 * THREE THINGS GO WRONG WITH A HINT AND ALL THREE ARE REPAIRED IN PLACE:
		 *
		 *   - IT NAMES A COLUMN THAT NO LONGER EXISTS. Refused by range and left cold.
		 *   - IT IS SINGULAR IN THE NEW MATRIX. Independence is not inherited across a
		 *     changed problem, so Factorise runs in repair mode and swaps out what cannot
		 *     pivot. A duplicated column is the same event seen from the other end: the
		 *     second copy finds its pivot row already taken.
		 *   - IT IS PRIMAL INFEASIBLE, WHICH IS THE NORMAL CASE AND THE DANGEROUS ONE. The
		 *     deleted joints were carrying force, so B^-1 b has genuinely negative entries —
		 *     and phase 1 would never look at them, because it is skipped whenever the basic
		 *     artificials sum to zero, which a warm basis satisfies by construction (its
		 *     artificials were driven out by the previous solve), while Refactorise would
		 *     clamp the evidence away as rounding. So the repair is reached DELIBERATELY:
		 *     for each slot whose value is negative, the basis column there is replaced by
		 *     its own NEGATION, which flips exactly that component of x_B and leaves every
		 *     other one alone (B' = B*D for a diagonal sign matrix D, so x' = D*x). The seed
		 *     is then primal feasible by construction, the flipped columns are artificials by
		 *     index and so carry phase-1 cost, and phase 1 genuinely runs and drives them out
		 *     — which IS the repair, priced where the measurement can see it.
		 */
		int32 SeedWarmStartBasis(FStandardForm& Form, const FOracleBasis& Hint)
		{
			const TArray<int32> ColdBasis = Form.InitialBasis;
			TArray<int32> Seed = ColdBasis;

			const int32 Offered = FMath::Min(Hint.Columns.Num(), Form.NumRows);

			for (int32 Row = 0; Row < Offered; ++Row)
			{
				const int32 Column = Hint.Columns[Row];

				/* INDEX_NONE is "no hint for this row"; so is any index out of the matrix. */
				if (Column >= 0 && Column < Form.NumCols)
				{
					Seed[Row] = Column;
				}
			}

			FBasisFactor Factor;

			if (!Factor.Factorise(Form, Seed, true))
			{
				return 0;
			}

			TArray<double> XB;
			TArray<double> ScratchOrig;
			TArray<double> ScratchSlot;

			SolveBasicValues(Form, Factor, Seed, XB, ScratchOrig, ScratchSlot);

			double LargestValue = 0.0;

			for (const double Value : XB)
			{
				/*
				 * A non-finite basic value means the seeded basis is arithmetic garbage, and
				 * the cold start is an answer this solver can still stand behind: abandon the
				 * whole warm start rather than repair around a NaN. Spelled as a refused
				 * IsFinite because every comparison against NaN is false, so a magnitude test
				 * would wave it through.
				 */
				if (!FMath::IsFinite(Value))
				{
					return 0;
				}

				LargestValue = FMath::Max(LargestValue, FMath::Abs(Value));
			}

			/*
			 * WHAT COUNTS AS GENUINELY NEGATIVE, relative to the values in play — the same
			 * shape of scale the phase-1 infeasibility test uses. Below it a negative value is
			 * the rounding the cold path clamps, and flipping on that would cost a pivot on a
			 * basis that is already optimal; above it, it is force the deletion took away.
			 */
			const double NegativeTolerance = (1.0 + LargestValue) * 1.0e-9;

			for (int32 Slot = 0; Slot < Form.NumRows; ++Slot)
			{
				if (XB[Slot] < -NegativeTolerance)
				{
					Seed[Slot] = AppendNegatedColumn(Form, Seed[Slot]);
				}
			}

			Form.InitialBasis = Seed;

			/*
			 * ACCEPTED MEANS THE SOLVE STARTED FROM THAT COLUMN — nothing weaker. A row whose
			 * hint was repaired away is not accepted, and neither is one whose hint was
			 * negated to make the start feasible: that slot holds a column the caller never
			 * named, and counting it would report a warm start richer than the one taken.
			 */
			int32 Accepted = 0;

			for (int32 Row = 0; Row < Offered; ++Row)
			{
				if (Hint.Columns[Row] >= 0 && Seed[Row] == Hint.Columns[Row])
				{
					++Accepted;
				}
			}

			return Accepted;
		}

		/**
		 * REPORT THE BASIS THE SOLVE IS STANDING ON, with the shape it belongs to. A column
		 * index means nothing without knowing where the structural columns end and the
		 * artificials begin, which is what a caller needs to map this basis onto the next
		 * problem's columns — so the two integers travel with it rather than beside it.
		 */
		void ReportBasis(
			const FStandardForm& Form, const TArray<int32>& Basis, FOracleBasis& Out)
		{
			Out.Columns = Basis;
			Out.NumStructCols = Form.NumStructCols;
			Out.ArtificialStart = Form.ArtificialStart;
		}


		/**
		 * PHASE 1'S OBJECTIVE, READ FROM THE BASIS: the sum of the basic artificials'
		 * values. Zero (within tolerance) is exactly the statement that the original rows
		 * have an admissible solution.
		 *
		 * It is a free function rather than the lambda it used to be because TWO places
		 * need it — the solve, which decides whether phase 1 runs at all and whether it
		 * succeeded, and the pivot loop, which records where feasibility was first reached
		 * — and two transcriptions of one sum is how a measurement quietly measures
		 * something else. The summation order is over basis slots, unchanged.
		 */
		double BasicArtificialInfeasibility(const FStandardForm& Form, const FRevisedState& State)
		{
			double Infeasibility = 0.0;

			for (int32 Row = 0; Row < Form.NumRows; ++Row)
			{
				if (State.Basis[Row] >= Form.ArtificialStart)
				{
					Infeasibility += State.XB[Row];
				}
			}

			return Infeasibility;
		}

		/** The scale the sum above is judged against: relative to the largest basic value. */
		double InfeasibilityTolerance(const FStandardForm& Form, const FRevisedState& State)
		{
			double LargestRhs = 0.0;

			for (int32 Row = 0; Row < Form.NumRows; ++Row)
			{
				LargestRhs = FMath::Max(LargestRhs, FMath::Abs(State.XB[Row]));
			}

			return (1.0 + LargestRhs) * 1.0e-9 * double(Form.NumRows);
		}

		enum class ESimplexEnd : uint8
		{
			Optimal,
			Unbounded,
			IterationCap,
			NumericalFailure,
		};

		/**
		 * A non-optimal phase-2 termination as the reason a caller can count. Optimal has
		 * no reason and never reaches here; it is mapped to the numerical arm anyway
		 * because a termination this function cannot name is a fault, and the fault-shaped
		 * answer is the fail-closed one.
		 */
		EOracleRefusal PhaseTwoRefusalFor(ESimplexEnd End)
		{
			switch (End)
			{
			case ESimplexEnd::IterationCap:     return EOracleRefusal::PhaseTwoIterationCap;
			case ESimplexEnd::Unbounded:        return EOracleRefusal::PhaseTwoUnbounded;
			case ESimplexEnd::NumericalFailure: return EOracleRefusal::PhaseTwoNumericalFailure;
			default:                            return EOracleRefusal::PhaseTwoNumericalFailure;
			}
		}

		/**
		 * THE ENTERING CHOICE: candidate-list partial pricing over a rotating window.
		 *
		 * A refill prices one window of PricingWindowCols consecutive columns — starting
		 * where the last refill stopped and wrapping, so the window position is pure
		 * index arithmetic over the input column order — and keeps the PricingQueueDepth
		 * best-ranked of them. Later iterations re-price only what is queued, which is a
		 * dozen dot products instead of the whole non-basic set.
		 *
		 * THE QUEUE IS A CANDIDATE LIST, NOT A DECISION. Every entry is re-priced against
		 * the CURRENT duals before it can be chosen, and one whose reduced cost has risen
		 * to non-negative is discarded rather than pivoted on: a stale price is exactly
		 * how a candidate list turns into a wrong pivot. The choice among the survivors is
		 * the BEST of them (earliest queue position on exact ties), never
		 * first-past-the-tolerance — that shortcut was measured costing 44x the pivots,
		 * which is the failure the pivot budget beside the scan budget exists to refuse.
		 *
		 * BEST MEANS THE STATIC STEEPEST-EDGE RATIO d_j / ||A_j||, NOT d_j ALONE, and that
		 * is the difference between passing the pivot budget and missing it by half. A
		 * reduced cost says how fast the objective falls per unit of the entering
		 * variable; dividing by the column's norm asks how fast it falls per unit of
		 * MOVEMENT, which is what actually shortens a pivot path (Forrest-Goldfarb; the
		 * static form is the cheap approximation, precomputed once in FStandardForm and
		 * never updated, so it costs one array and no per-iteration work). Measured on the
		 * PricingCost wall row, the same 384-column window either way: ranked by the raw
		 * reduced cost it took 6,128 pivots and 1,030,437 scans against full Dantzig's
		 * 1,942 and 3,131,528 — cheap scans bought with a grinding path, exactly what the
		 * pivot budget beside the scan budget refuses — and ranked by the ratio it takes
		 * 2,016 pivots and 538,200 scans.
		 *
		 * The ratio is a RANKING only: candidacy stays the raw `d_j < -CostTol`, because
		 * optimality is a statement about reduced costs and dividing by a norm must never
		 * be able to promote a column across the tolerance.
		 *
		 * THE FULL SCAN IS PART OF THE ANSWER, NOT A FALLBACK FOR TIDINESS. Optimality is
		 * the claim that NO column has a negative reduced cost, and a window has seen only
		 * a slice; so a refill that finds nothing keeps taking windows until it has priced
		 * every column against the current duals, and only that exhausted sweep may return
		 * "optimal". Those scans are counted like every other.
		 *
		 * WITH THE BLAND FALLBACK IT STANDS ASIDE ENTIRELY. After a long degenerate streak
		 * the entering rule becomes lowest-index-negative, which is the anti-cycling
		 * guarantee and is a statement about ALL columns; a window could offer the lowest
		 * index of a slice and cycle happily. So the fallback prices the full set in index
		 * order, takes the first negative, and empties the queue — the window resumes from
		 * wherever it was once the streak breaks.
		 *
		 * DETERMINISM. Cursor and window are index arithmetic, the queue is filled in scan
		 * order and ordered by value with position breaking ties, and nothing here reads a
		 * hash, a pointer or a clock. Same problem, same pivot path, same scan count.
		 */
		struct FPartialPricer
		{
			struct FCandidate
			{
				int32 Col = INDEX_NONE;

				/** The RANKING value: the reduced cost over the column's static norm. */
				double Weighted = 0.0;
			};

			/** Where the next refill starts. Advanced by exactly what it scanned. */
			int32 Cursor = 0;

			/** Best-weighted first, at most PricingQueueDepth deep. */
			TArray<FCandidate> Queue;

			/**
			 * Offer a freshly priced column to the queue. The caller has already applied
			 * the negativity test, which is written as `Reduced < -CostTol` so a NaN
			 * reduced cost is never offered at all.
			 */
			void Offer(int32 Col, double Weighted)
			{
				int32 At = 0;

				while (At < Queue.Num() && Queue[At].Weighted <= Weighted)
				{
					++At;
				}

				if (At >= PricingQueueDepth)
				{
					return;
				}

				Queue.Insert({ Col, Weighted }, At);

				if (Queue.Num() > PricingQueueDepth)
				{
					Queue.Pop();
				}
			}

			/**
			 * The entering column, or INDEX_NONE when every column in [0, AllowedCols)
			 * has been priced against the current duals and none of them prices negative.
			 */
			int32 ChooseEntering(
				FRevisedState& S, const TArray<double>& Cost, int32 AllowedCols, bool bBland)
			{
				if (AllowedCols <= 0)
				{
					return INDEX_NONE;
				}

				if (bBland)
				{
					Queue.Reset();

					for (int32 Col = 0; Col < AllowedCols; ++Col)
					{
						if (S.bIsBasic[Col])
						{
							continue;
						}

						const double Reduced = S.ReducedCost(Col, Cost);
						++S.PricingColumnScans;

						if (Reduced < -CostTol)
						{
							return Col;
						}
					}

					return INDEX_NONE;
				}

				/* Re-price what is queued; a candidate that went stale is dropped. */
				int32 Kept = 0;
				int32 Best = INDEX_NONE;
				double BestWeighted = 0.0;

				for (int32 Entry = 0; Entry < Queue.Num(); ++Entry)
				{
					const int32 Col = Queue[Entry].Col;

					if (S.bIsBasic[Col])
					{
						continue;
					}

					const double Reduced = S.ReducedCost(Col, Cost);
					++S.PricingColumnScans;

					if (!(Reduced < -CostTol))
					{
						continue;
					}

					const double Weighted = Reduced / S.Form->ColNorm[Col];

					Queue[Kept].Col = Col;
					Queue[Kept].Weighted = Weighted;
					++Kept;

					if (Best == INDEX_NONE || Weighted < BestWeighted)
					{
						Best = Col;
						BestWeighted = Weighted;
					}
				}

				Queue.SetNum(Kept, EAllowShrinking::No);

				if (Best != INDEX_NONE)
				{
					return Best;
				}

				/* The queue is spent: refill from the window, widening until it bites. */
				if (Cursor >= AllowedCols)
				{
					Cursor = 0;
				}

				int32 Scanned = 0;

				while (Scanned < AllowedCols)
				{
					const int32 Take = FMath::Min(PricingWindowCols, AllowedCols - Scanned);

					for (int32 Step = 0; Step < Take; ++Step)
					{
						int32 Col = Cursor + Scanned + Step;

						if (Col >= AllowedCols)
						{
							Col -= AllowedCols;
						}

						if (S.bIsBasic[Col])
						{
							continue;
						}

						const double Reduced = S.ReducedCost(Col, Cost);
						++S.PricingColumnScans;

						if (Reduced < -CostTol)
						{
							Offer(Col, Reduced / S.Form->ColNorm[Col]);
						}
					}

					Scanned += Take;

					if (Queue.Num() > 0)
					{
						break;
					}
				}

				Cursor += Scanned;

				if (Cursor >= AllowedCols)
				{
					Cursor -= AllowedCols;
				}

				return Queue.Num() > 0 ? Queue[0].Col : INDEX_NONE;
			}
		};

		/**
		 * Minimise the given objective. Every choice below is INDEX-DETERMINISTIC — no
		 * randomness, no hashing — so the pivot path, and therefore the last bit of
		 * lambda*, is a pure function of the input arrays.
		 *
		 * The entering rule is CANDIDATE-LIST PARTIAL PRICING (FPartialPricer above: the
		 * best static steepest-edge ratio within a rotating window's queue, every queued
		 * candidate re-priced against the current duals before it can be chosen, a full
		 * sweep required before "optimal"), priced EXACTLY each iteration from a fresh
		 * BTRAN rather than from a maintained row — the maintained row's drift was the
		 * dense solver's disease.
		 *
		 * The ratio test breaks near-ties by the LARGEST PIVOT ELEMENT (then lowest
		 * basic index): Bland's rule alone was measured accepting a basis 0.98% outside
		 * the crushing envelope on the dry 8-course stack, because it happily pivots on
		 * near-tolerance elements. Bland remains as the ANTI-CYCLING FALLBACK: after a
		 * long streak of zero-length steps the entering rule drops to lowest-index over
		 * a FULL scan — the window stands aside, because lowest-index-of-a-slice is not
		 * Bland's rule and would not stop cycling — which restores the termination
		 * guarantee where it is needed.
		 *
		 * A CANDIDATE PIVOT MUST ALSO CLEAR THE ENTERING COLUMN'S OWN SCALE
		 * (RelativePivotTol), because an absolute tolerance says nothing about a column
		 * whose FTRAN image runs to 1e7. That one change is what turned the two
		 * opening-ladder refusals into certified answers; the measurements sit with the
		 * constant.
		 *
		 * bWatchArtificialFeasibility IS AN OBSERVATION AND NOTHING ELSE. Phase 1 passes it
		 * true so the loop can record the pivot at which the problem first read feasible;
		 * the loop then carries on to optimality exactly as it always has, so every pivot
		 * path, every lambda* and every count in the suite is unchanged. It costs one walk
		 * of the basis per pivot, and only until the moment it records.
		 */
		ESimplexEnd RunRevisedSimplex(
			FRevisedState& S, const TArray<double>& Cost, int32 AllowedCols,
			int32& InOutIterations, bool bWatchArtificialFeasibility)
		{
			const FStandardForm& Form = *S.Form;
			int32 DegenerateStreak = 0;
			FPartialPricer Pricer;

			while (true)
			{
				if (InOutIterations >= MaxPivots)
				{
					return ESimplexEnd::IterationCap;
				}

				if (S.PivotsSinceRefactor >= RefactoriseEvery)
				{
					if (!S.Refactorise())
					{
						return ESimplexEnd::NumericalFailure;
					}
				}

				const bool bBlandFallback = DegenerateStreak >= 500;

				if (bBlandFallback)
				{
					++S.BlandDegenerateEntries;
				}

				/* Price: y solves yT B = c_B, then d_j = c_j - y . A_j, exact. */
				S.ScratchSlot.SetNumUninitialized(Form.NumRows);

				for (int32 Slot = 0; Slot < Form.NumRows; ++Slot)
				{
					S.ScratchSlot[Slot] = Cost[S.Basis[Slot]];
				}

				S.BtranScratchSlot(S.YRow);

				const int32 Entering =
					Pricer.ChooseEntering(S, Cost, AllowedCols, bBlandFallback);

				if (Entering == INDEX_NONE)
				{
					return ESimplexEnd::Optimal;
				}

				S.FtranColumn(Entering, S.EnteringW);

				int32 Leaving = INDEX_NONE;
				double BestRatio = 0.0;
				double LeavingMagnitude = 0.0;

				/*
				 * THE FLOOR UNDER A BELIEVABLE PIVOT, taken from the entering column
				 * itself: an element 1e-16 of the column's own largest is the rounding of
				 * the solve that produced the column, and pivoting on it is what was
				 * measured driving the basis singular. FMath::Max discards a NaN rather
				 * than propagating it, which is harmless here because the guard below is
				 * spelled so that a NaN coefficient fails it whatever the floor reads.
				 */
				double LargestMagnitude = 0.0;

				for (int32 Row = 0; Row < Form.NumRows; ++Row)
				{
					LargestMagnitude =
						FMath::Max(LargestMagnitude, FMath::Abs(S.EnteringW[Row]));
				}

				const double PivotFloor =
					FMath::Max(PivotTol, RelativePivotTol * LargestMagnitude);

				for (int32 Row = 0; Row < Form.NumRows; ++Row)
				{
					const double Coefficient = S.EnteringW[Row];

					/*
					 * Written as a refused negation rather than `<=` so a NaN coefficient
					 * lands INSIDE the guard: every comparison against NaN is false, so
					 * `Coefficient <= PivotFloor` would wave it through into the ratio.
					 */
					if (!(Coefficient > PivotFloor))
					{
						continue;
					}

					const double Ratio = S.XB[Row] / Coefficient;

					if (Leaving == INDEX_NONE)
					{
						Leaving = Row;
						BestRatio = Ratio;
						LeavingMagnitude = Coefficient;
						continue;
					}

					const double NearTie = 1.0e-12 * (1.0 + FMath::Abs(BestRatio));

					if (Ratio < BestRatio - NearTie)
					{
						Leaving = Row;
						BestRatio = Ratio;
						LeavingMagnitude = Coefficient;
					}
					else if (Ratio <= BestRatio + NearTie)
					{
						/* Same step length: prefer the numerically strongest pivot. */
						if (Coefficient > LeavingMagnitude
							|| (Coefficient == LeavingMagnitude
								&& S.Basis[Row] < S.Basis[Leaving]))
						{
							Leaving = Row;
							BestRatio = FMath::Min(BestRatio, Ratio);
							LeavingMagnitude = Coefficient;
						}
					}
				}

				if (Leaving == INDEX_NONE)
				{
					/*
					 * WITH THE CAP ROW A REAL UNBOUNDED RAY IS IMPOSSIBLE, so reaching here
					 * on a bounded problem is a numerical event and refusing is fail-closed
					 * rather than an answer. It IS reachable: measured on the 107-block
					 * abutment rung before the pivot floor above existed, an entering column
					 * whose exact reduced cost is ZERO — the formulation splits shear into
					 * p - q whose columns are bitwise negations, and its partner was basic —
					 * priced at -1.12e-9 of drift inherited from a dual vector running to
					 * 6.7e6 (1e-16 of that IS 1e-9), FTRANned to a single -1, and the solve
					 * was refused.
					 *
					 * A repair for that seam (refactorise, re-price, then set the column
					 * aside and take the next candidate) was written, MEASURED NOT TO FIRE,
					 * and removed: with the pivot floor in place both fixtures answer with
					 * bit-identical lambda* and identical pivot paths whether the seam is
					 * repaired or left to refuse. The floor stops the basis from becoming
					 * the ill-conditioned one whose duals produce the drift, so this arm is
					 * never reached on them. That does NOT close the mode in general —
					 * CURRENT_STATE books it as a live candidate for the refusals that
					 * remain — and the repair is not reinstated without a fixture that
					 * genuinely drives it.
					 */
					return ESimplexEnd::Unbounded;
				}

				if (BestRatio <= 1.0e-12)
				{
					++DegenerateStreak;
				}
				else
				{
					DegenerateStreak = 0;
				}

				S.ApplyPivot(Leaving, Entering, S.EnteringW, BestRatio);
				++InOutIterations;

				/*
				 * THE EARLY-EXIT SEAM, MEASURED AND NOT TAKEN. The comparison is spelled so
				 * that a NaN sum records nothing: every comparison against NaN is false, so
				 * garbage arithmetic leaves the field saying "feasibility was never reached"
				 * rather than claiming an exit could have fired at this pivot.
				 */
				if (bWatchArtificialFeasibility && S.PivotsToFirstFeasible == INDEX_NONE)
				{
					if (BasicArtificialInfeasibility(Form, S) <= InfeasibilityTolerance(Form, S))
					{
						S.PivotsToFirstFeasible = InOutIterations;
					}
				}
			}
		}
	}

	/*
	 * THE THREE PHASE-2 PHRASES SHARE THEIR FIRST FIVE WORDS ON PURPOSE. "phase-2 simplex
	 * failed" is the sentence every phase-2 refusal has printed until now, and two branches
	 * of the sweep test still recognise a refusal by that literal (as one does the
	 * verification refusal by "failed verification"); keeping it as the prefix means the
	 * split ADDS the arm's name rather than renaming the event, so nothing that reads the
	 * old sentence stops recognising it. Distinctness — which is what the taxonomy is for —
	 * comes from the clause after the colon, and is asserted pairwise over the enumerators.
	 *
	 * None is empty rather than "no reason": a caller checks the reason against bAnswered,
	 * and a sentence on the answering path would have to be filtered out of every message
	 * that prints WhyNot.
	 */
	FString RefusalText(EOracleRefusal Refusal)
	{
		switch (Refusal)
		{
		case EOracleRefusal::None:
			return FString();

		case EOracleRefusal::InvalidProblem:
			return TEXT("input validation refused the problem");

		case EOracleRefusal::PhaseOneFailure:
			return TEXT("phase-1 simplex failed");

		case EOracleRefusal::PhaseTwoIterationCap:
			return TEXT("phase-2 simplex failed: the iteration cap");

		case EOracleRefusal::PhaseTwoUnbounded:
			return TEXT("phase-2 simplex failed: an unbounded ray on a capped problem");

		case EOracleRefusal::PhaseTwoNumericalFailure:
			return TEXT("phase-2 simplex failed: the basis went singular");

		case EOracleRefusal::VerificationFailure:
			return TEXT("the optimal basis failed verification");
		}

		/*
		 * An enumerator this function does not know is a refusal it cannot name, which is
		 * the one thing this whole taxonomy exists to prevent — so it is reported as such
		 * rather than answered with an empty string, which would read as "it answered".
		 */
		return TEXT("the oracle refused for an unnamed reason");
	}

	/**
	 * EXTRACT THE COLLAPSE MECHANISM from phase 1's dual at the infeasible arm — the Farkas
	 * certificate that IS the kinematic upper-bound mechanism (PROMOTION_DESIGN §3.3). Called
	 * only when the dead loads admit no equilibrium (the Lambda = 0 arm), with State holding the
	 * phase-1 OPTIMAL basis: no pivot happens between phase 1 returning and this call, so the
	 * dual recomputed here is bit-for-bit the one the pricer proved optimal against.
	 *
	 * ONE BTRAN, and it increments no counted quantity. y = c_B B^-1 with the phase-1 cost (1 on
	 * a basic artificial, 0 else) is the dual on the SCALED standard-form rows; multiplied by
	 * RowScaleSigned it is the PHYSICAL certificate on the original assembly rows, whose per-block
	 * equilibrium-row triple (Fx, Fz, moment) is that block's virtual (u_x, u_z, omega). The
	 * global sign is fixed so a DESCENDING centroid reads VirtualUz < 0: with the dual negated,
	 * the virtual work of gravity equals yb (the phase-1 objective) and is positive by
	 * construction, which is the yb > 0 half of Farkas the caller and the test both rely on.
	 *
	 * FARKAS, FAIL CLOSED (§3.6), the mirror of the primal admissibility gate with the opposite
	 * polarity: yb > 0 (infeasibility is certified) AND yA_j <= tol on every STRUCTURAL column
	 * (which is exactly the phase-1 optimality the basis already satisfies) AND the named set is
	 * non-empty. Any one failing returns false, and the caller refuses with VerificationFailure
	 * rather than name bricks on a certificate it could not check — an ill-conditioned dual must
	 * produce a refusal, never a plausible-looking wrong collapse.
	 *
	 * Returns true and fills OutMechanism (bPresent, bIsCertified, triples, opening flags) iff the
	 * certificate verifies; false with OutMechanism left as the caller default otherwise.
	 */
	bool ExtractMechanism(
		const FOracleProblem& Problem,
		const OracleDetail::FStandardForm& Form,
		OracleDetail::FRevisedState& State,
		const TArray<int32>& EqFxRowOfBlock,
		FOracleMechanism& OutMechanism)
	{
		using namespace OracleDetail;

		const int32 NumRows = Form.NumRows;
		const int32 NumBlocks = EqFxRowOfBlock.Num();
		const int32 NumJoints = Problem.Joints.Num();

		/* Phase-1 dual: y solves yT B = c_B with c_B the phase-1 cost. One BTRAN. */
		State.ScratchSlot.SetNumUninitialized(NumRows);
		for (int32 Slot = 0; Slot < NumRows; ++Slot)
		{
			State.ScratchSlot[Slot] = State.Basis[Slot] >= Form.ArtificialStart ? 1.0 : 0.0;
		}
		State.BtranScratchSlot(State.YRow);

		/* The physical certificate on the ORIGINAL assembly rows: undo the row equilibration. */
		TArray<double> YPhys;
		YPhys.SetNumUninitialized(NumRows);
		for (int32 Row = 0; Row < NumRows; ++Row)
		{
			YPhys[Row] = State.YRow[Row] * Form.RowScaleSigned[Row];
		}

		/*
		 * yb > 0, computed in the scaled space the problem was solved in (yb is scale-invariant:
		 * y_scaled . b_scaled = y_phys . b_assembly). It equals the phase-1 objective, so it is
		 * positive by construction here; the explicit strict check is the fail-closed guard.
		 */
		double Yb = 0.0;
		double YbMagnitude = 0.0;
		for (int32 Row = 0; Row < NumRows; ++Row)
		{
			const double Term = State.YRow[Row] * Form.Rhs[Row];
			Yb += Term;
			YbMagnitude += FMath::Abs(Term);
		}

		if (!(Yb > 1.0e-9 * (1.0 + YbMagnitude)))
		{
			return false;
		}

		/*
		 * yA_j <= tol on every structural column. Phase 1's cost is zero on these columns, so
		 * optimality already guarantees the reduced cost -yA_j >= -CostTol; the loose relative
		 * tolerance here only refuses a certificate that is violated well past rounding.
		 */
		for (int32 Col = 0; Col < Form.NumStructCols; ++Col)
		{
			double YA = 0.0;
			double Magnitude = 0.0;

			for (int32 At = Form.ColStart[Col]; At < Form.ColStart[Col + 1]; ++At)
			{
				const double Term = Form.ColVal[At] * State.YRow[Form.ColRow[At]];
				YA += Term;
				Magnitude += FMath::Abs(Term);
			}

			if (YA > 1.0e-6 * (1.0 + Magnitude))
			{
				return false;
			}
		}

		/* ---- Block triples, negated so a descending centroid reads VirtualUz < 0. ---- */
		OutMechanism.Blocks.SetNum(NumBlocks);
		double LargestBlockMagnitude = 0.0;

		for (int32 Block = 0; Block < NumBlocks; ++Block)
		{
			const int32 Fx = EqFxRowOfBlock[Block];

			if (Fx == INDEX_NONE)
			{
				/* Grounded: writes no rows, so its triple is exactly zero and it never moves. */
				continue;
			}

			FOracleMechanismBlock& Triple = OutMechanism.Blocks[Block];
			Triple.VirtualUx = -YPhys[Fx];
			Triple.VirtualUz = -YPhys[Fx + 1];
			Triple.VirtualOmega = -YPhys[Fx + 2];

			const double BlockMagnitude = FMath::Abs(Triple.VirtualUx)
				+ FMath::Abs(Triple.VirtualUz) + FMath::Abs(Triple.VirtualOmega);
			LargestBlockMagnitude = FMath::Max(LargestBlockMagnitude, BlockMagnitude);
		}

		/*
		 * ---- Joint give: the RELATIVE virtual velocity across the contact, KINEMATIC. ----
		 *
		 * The raw plastic multipliers on the strength rows are the WRONG thing to read here:
		 * they are the part of the degenerate dual that is NON-UNIQUE (the permutation fuzz
		 * measures the block velocities agreeing to 1e-15 while the strength-row multipliers
		 * name 8 opening joints on one column order and 13-16 on another — dual degeneracy
		 * spreads the plastic flow over redundant contacts differently each pivot path). The
		 * block velocity triples ARE unique, so the joint set is derived from THEM instead: a
		 * joint gives iff its two blocks have a non-zero relative velocity at a contact point,
		 * which is the associated-flow definition of the contact opening or sliding and is a
		 * pure function of the (stable) triples and the fixed geometry. That is the canonical
		 * tie-break the degenerate dual needs (PROMOTION_DESIGN §3.3, §12 D7): the SET is read
		 * off the unique kinematics, never off the non-unique multipliers.
		 */
		auto VelocityAt = [](
			const FOracleMechanismBlock& Triple, double CentroidX, double CentroidZ,
			double PointX, double PointZ, double& OutVx, double& OutVz)
		{
			/* Rigid-body velocity v = u + omega x r, in the moment convention r_x*F_z - r_z*F_x. */
			OutVx = Triple.VirtualUx - Triple.VirtualOmega * (PointZ - CentroidZ);
			OutVz = Triple.VirtualUz + Triple.VirtualOmega * (PointX - CentroidX);
		};

		TArray<double> JointRelativeVelocity;
		JointRelativeVelocity.Init(0.0, NumJoints);
		double LargestJointRelativeVelocity = 0.0;

		for (int32 Joint = 0; Joint < NumJoints; ++Joint)
		{
			const FOracleJoint& J = Problem.Joints[Joint];
			const FOracleBlock& A = Problem.Blocks[J.BlockA];
			const FOracleBlock& B = Problem.Blocks[J.BlockB];
			const FOracleMechanismBlock& TripleA = OutMechanism.Blocks[J.BlockA];
			const FOracleMechanismBlock& TripleB = OutMechanism.Blocks[J.BlockB];

			/* The two contact points sit at Centre -/+ HalfLength along the in-plane tangent. */
			const double TangentX = -J.NormalZ;
			const double TangentZ = J.NormalX;

			double Worst = 0.0;

			for (int32 End = 0; End < 2; ++End)
			{
				const double Sign = End == 0 ? -1.0 : 1.0;
				const double PointX = J.CentreXCm + Sign * J.HalfLengthCm * TangentX;
				const double PointZ = J.CentreZCm + Sign * J.HalfLengthCm * TangentZ;

				double AVx, AVz, BVx, BVz;
				VelocityAt(TripleA, A.CentroidXCm, A.CentroidZCm, PointX, PointZ, AVx, AVz);
				VelocityAt(TripleB, B.CentroidXCm, B.CentroidZCm, PointX, PointZ, BVx, BVz);

				Worst = FMath::Max(Worst, FMath::Abs(BVx - AVx) + FMath::Abs(BVz - AVz));
			}

			JointRelativeVelocity[Joint] = Worst;
			LargestJointRelativeVelocity = FMath::Max(LargestJointRelativeVelocity, Worst);
		}

		/* ---- Canonicalize: the named set is what clears the relative threshold. ---- */
		OutMechanism.JointOpensOrSlides.Init(false, NumJoints);
		int32 MovingCount = 0;

		if (LargestBlockMagnitude > 0.0)
		{
			for (int32 Block = 0; Block < NumBlocks; ++Block)
			{
				const FOracleMechanismBlock& Triple = OutMechanism.Blocks[Block];
				const double BlockMagnitude = FMath::Abs(Triple.VirtualUx)
					+ FMath::Abs(Triple.VirtualUz) + FMath::Abs(Triple.VirtualOmega);

				if (BlockMagnitude > MechanismRelativeTol * LargestBlockMagnitude)
				{
					OutMechanism.Blocks[Block].bMoves = true;
					++MovingCount;
				}
			}
		}

		if (LargestJointRelativeVelocity > 0.0)
		{
			for (int32 Joint = 0; Joint < NumJoints; ++Joint)
			{
				if (JointRelativeVelocity[Joint] > MechanismRelativeTol * LargestJointRelativeVelocity)
				{
					OutMechanism.JointOpensOrSlides[Joint] = true;
				}
			}
		}

		/* A mechanism that moves nothing is not a mechanism — fail closed. */
		if (MovingCount == 0)
		{
			return false;
		}

		OutMechanism.bPresent = true;
		OutMechanism.bIsCertified = true;
		return true;
	}

	/** One attempt at the problem exactly as posed, warm start included. */
	FOracleResult SolveRigidBlockOnce(const FOracleProblem& Problem)
	{
		using namespace OracleDetail;

		FOracleResult Result;
		Result.bAnswered = false;
		Result.Lambda = 0.0;

		/*
		 * ONE STATEMENT SETS BOTH HALVES OF A REFUSAL. The reason and the sentence are two
		 * spellings of one fact, and a site that set only one of them would leave a result
		 * whose enumerator and whose text disagree — which is worse than either alone,
		 * because a reader would have to know which to believe.
		 */
		const auto Refuse =
			[&Result](EOracleRefusal Reason, const FString& Detail = FString()) -> FOracleResult&
		{
			Result.Refusal = Reason;
			Result.WhyNot = Detail.IsEmpty()
				? RefusalText(Reason)
				: RefusalText(Reason) + TEXT(": ") + Detail;

			return Result;
		};

		const FString InvalidReason = ValidateProblem(Problem);

		if (!InvalidReason.IsEmpty())
		{
			return Refuse(EOracleRefusal::InvalidProblem, InvalidReason);
		}

		/*
		 * STRUCTURAL COLUMNS: lambda, then per contact point [n+, n-, p, q] with
		 * n = n+ - n- (compression positive) and v = p - q. Splitting n rather than
		 * shifting it by the tension bound keeps huge bounds out of the right-hand
		 * sides; the tension bound becomes its own row, n- <= f_t*Conv*A/2.
		 */
		const int32 NumJoints = Problem.Joints.Num();
		const int32 NumContacts = NumJoints * 2;
		const int32 NumStructCols = 1 + 4 * NumContacts;

		struct FContact
		{
			int32 Joint = 0;
			double PosX = 0.0;
			double PosZ = 0.0;
			double TributaryAreaSqCm = 0.0;

			/*
			 * AN EXACTLY-ZERO TENSILE STRENGTH MEANS THE n- VARIABLE DOES NOT EXIST,
			 * not that it exists bounded by a zero-right-hand-side row. The two are the
			 * same feasible set, but a dry-stone stack writes hundreds of those fully
			 * degenerate rows and Bland's rule can grind on their zero-length pivots for
			 * the whole iteration budget; leaving the column identically zero makes it
			 * unenterable instead. Dry stone reduces to classic no-tension by DATA,
			 * exactly as the header promises.
			 */
			bool bCanTension = false;
		};

		TArray<FContact> Contacts;
		Contacts.Reserve(NumContacts);

		for (int32 JointIndex = 0; JointIndex < NumJoints; ++JointIndex)
		{
			const FOracleJoint& Joint = Problem.Joints[JointIndex];

			/* In-plane tangent, fixed as (-Nz, Nx); contacts at centre -/+ h along it. */
			const double TangentX = -Joint.NormalZ;
			const double TangentZ = Joint.NormalX;

			for (int32 End = 0; End < 2; ++End)
			{
				const double Sign = End == 0 ? -1.0 : 1.0;

				FContact Contact;
				Contact.Joint = JointIndex;
				Contact.PosX = Joint.CentreXCm + Sign * Joint.HalfLengthCm * TangentX;
				Contact.PosZ = Joint.CentreZCm + Sign * Joint.HalfLengthCm * TangentZ;
				Contact.TributaryAreaSqCm = Joint.AreaSqCm / 2.0;
				Contact.bCanTension = Joint.Strength.TensileStrengthMPa > 0.0;
				Contacts.Add(Contact);
			}
		}

		TArray<FAssemblyRow> AssemblyRows;

		/*
		 * ROW -> BLOCK BOOKKEEPING for the mechanism extraction (PROMOTION_DESIGN §3.3, §12 D7).
		 * EqFxRowOfBlock[b] is the assembly-row index of block b's Fx equilibrium row — its Fz
		 * and moment rows follow at +1 and +2, in that fixed order — or INDEX_NONE for a grounded
		 * block, which writes no rows. That triple is where the infeasible arm reads block b's
		 * virtual-motion dual. Pure index-ordered bookkeeping, built as the rows are, deterministic
		 * by construction. Nothing in the solve reads it — it is the infeasible arm's alone.
		 */
		TArray<int32> EqFxRowOfBlock;
		EqFxRowOfBlock.Init(INDEX_NONE, Problem.Blocks.Num());

		/* ---- Equilibrium: three equalities per non-grounded block. -------------- */
		for (int32 BlockIndex = 0; BlockIndex < Problem.Blocks.Num(); ++BlockIndex)
		{
			const FOracleBlock& Block = Problem.Blocks[BlockIndex];

			if (Block.bGrounded)
			{
				continue;
			}

			/* Fx lands at the current end; Fz and M follow it in the two next slots. */
			EqFxRowOfBlock[BlockIndex] = AssemblyRows.Num();

			FAssemblyRow RowFx;
			FAssemblyRow RowFz;
			FAssemblyRow RowM;

			/* Loads: live into the lambda column, dead into the right-hand side. */
			double LiveX = 0.0, LiveZ = 0.0, LiveM = 0.0;
			double DeadX = 0.0, DeadZ = 0.0, DeadM = 0.0;

			const double WeightUu = Block.MassKg * OracleGravityCmPerSecondSquared;

			/* Gravity acts at the centroid: force only, no moment about it. */
			if (Problem.bGravityIsLive)
			{
				LiveZ -= WeightUu;
			}
			else
			{
				DeadZ -= WeightUu;
			}

			for (const FOracleAppliedForce& Applied : Problem.AppliedForces)
			{
				if (Applied.Block != BlockIndex)
				{
					continue;
				}

				const double Rx = Applied.AtXCm - Block.CentroidXCm;
				const double Rz = Applied.AtZCm - Block.CentroidZCm;
				const double Torque = Rx * Applied.ForceZUu - Rz * Applied.ForceXUu;

				if (Applied.bLive)
				{
					LiveX += Applied.ForceXUu;
					LiveZ += Applied.ForceZUu;
					LiveM += Torque;
				}
				else
				{
					DeadX += Applied.ForceXUu;
					DeadZ += Applied.ForceZUu;
					DeadM += Torque;
				}
			}

			RowFx.Add(0, LiveX);
			RowFz.Add(0, LiveZ);
			RowM.Add(0, LiveM);

			for (int32 ContactIndex = 0; ContactIndex < Contacts.Num(); ++ContactIndex)
			{
				const FContact& Contact = Contacts[ContactIndex];
				const FOracleJoint& Joint = Problem.Joints[Contact.Joint];

				double SignForBlock = 0.0;

				if (Joint.BlockB == BlockIndex)
				{
					SignForBlock = 1.0;
				}
				else if (Joint.BlockA == BlockIndex)
				{
					SignForBlock = -1.0;
				}
				else
				{
					continue;
				}

				const double TangentX = -Joint.NormalZ;
				const double TangentZ = Joint.NormalX;

				const double Rx = Contact.PosX - Block.CentroidXCm;
				const double Rz = Contact.PosZ - Block.CentroidZCm;

				/* Torque per unit force: r_x*F_z - r_z*F_x, one convention throughout. */
				const double TorquePerNormal = Rx * Joint.NormalZ - Rz * Joint.NormalX;
				const double TorquePerShear = Rx * TangentZ - Rz * TangentX;

				const int32 Base = 1 + 4 * ContactIndex;

				RowFx.Add(Base + 0, SignForBlock * Joint.NormalX);
				RowFz.Add(Base + 0, SignForBlock * Joint.NormalZ);
				RowM.Add(Base + 0, SignForBlock * TorquePerNormal);

				if (Contact.bCanTension)
				{
					RowFx.Add(Base + 1, -SignForBlock * Joint.NormalX);
					RowFz.Add(Base + 1, -SignForBlock * Joint.NormalZ);
					RowM.Add(Base + 1, -SignForBlock * TorquePerNormal);
				}

				RowFx.Add(Base + 2, SignForBlock * TangentX);
				RowFz.Add(Base + 2, SignForBlock * TangentZ);
				RowM.Add(Base + 2, SignForBlock * TorquePerShear);

				RowFx.Add(Base + 3, -SignForBlock * TangentX);
				RowFz.Add(Base + 3, -SignForBlock * TangentZ);
				RowM.Add(Base + 3, -SignForBlock * TorquePerShear);
			}

			RowFx.Rhs = -DeadX;
			RowFz.Rhs = -DeadZ;
			RowM.Rhs = -DeadM;

			AssemblyRows.Add(MoveTemp(RowFx));
			AssemblyRows.Add(MoveTemp(RowFz));
			AssemblyRows.Add(MoveTemp(RowM));
		}

		/* ---- The lambda cap. ---------------------------------------------------- */
		{
			FAssemblyRow Cap;
			Cap.Add(0, 1.0);
			Cap.Rhs = LambdaCap;
			Cap.bEquality = false;
			AssemblyRows.Add(MoveTemp(Cap));
		}

		/* ---- Strength rows per contact point. ----------------------------------- */
		for (int32 ContactIndex = 0; ContactIndex < Contacts.Num(); ++ContactIndex)
		{
			const FContact& Contact = Contacts[ContactIndex];
			const FConnectionStrength& S = Problem.Joints[Contact.Joint].Strength;
			const int32 Base = 1 + 4 * ContactIndex;

			const double Conv = OracleForceUnitsPerMPaSqCm;
			const double AreaSqCm = Contact.TributaryAreaSqCm;

			/*
			 * Tension: n- <= f_t * Conv * A/2. At an exactly-zero strength the column
			 * itself is absent (see FContact::bCanTension) and no row is written.
			 */
			if (Contact.bCanTension && S.TensileStrengthMPa < UncappedStrengthMPa)
			{
				FAssemblyRow Tension;
				Tension.Add(Base + 1, 1.0);
				Tension.Rhs = S.TensileStrengthMPa * Conv * AreaSqCm;
				Tension.bEquality = false;
				AssemblyRows.Add(MoveTemp(Tension));
			}

			/* Coulomb: +-(p - q) - mu*(n+ - n-) <= c * Conv * A/2. */
			if (S.ShearCohesionMPa < UncappedStrengthMPa)
			{
				for (int32 Orientation = 0; Orientation < 2; ++Orientation)
				{
					const double ShearSign = Orientation == 0 ? 1.0 : -1.0;

					FAssemblyRow Friction;
					Friction.Add(Base + 0, -S.FrictionCoefficient);

					if (Contact.bCanTension)
					{
						Friction.Add(Base + 1, S.FrictionCoefficient);
					}

					Friction.Add(Base + 2, ShearSign);
					Friction.Add(Base + 3, -ShearSign);
					Friction.Rhs = S.ShearCohesionMPa * Conv * AreaSqCm;
					Friction.bEquality = false;
					AssemblyRows.Add(MoveTemp(Friction));
				}
			}

			/* Crushing: n+ - n- <= f_c * Conv * A/2. */
			if (S.CompressiveStrengthMPa < UncappedStrengthMPa)
			{
				FAssemblyRow Crush;
				Crush.Add(Base + 0, 1.0);

				if (Contact.bCanTension)
				{
					Crush.Add(Base + 1, -1.0);
				}

				Crush.Rhs = S.CompressiveStrengthMPa * Conv * AreaSqCm;
				Crush.bEquality = false;
				AssemblyRows.Add(MoveTemp(Crush));
			}

			/* The truncated envelope: +-(p - q) <= f_v,max * Conv * A/2. */
			if (S.MaxShearStrengthMPa < UncappedStrengthMPa)
			{
				for (int32 Orientation = 0; Orientation < 2; ++Orientation)
				{
					const double ShearSign = Orientation == 0 ? 1.0 : -1.0;

					FAssemblyRow Ceiling;
					Ceiling.Add(Base + 2, ShearSign);
					Ceiling.Add(Base + 3, -ShearSign);
					Ceiling.Rhs = S.MaxShearStrengthMPa * Conv * AreaSqCm;
					Ceiling.bEquality = false;
					AssemblyRows.Add(MoveTemp(Ceiling));
				}
			}
		}

		/* ---- First-crack rows: uncracked peak-fibre limit for bonded joints. ---- */
		if (Problem.bFirstCrackRows)
		{
			for (int32 JointIndex = 0; JointIndex < NumJoints; ++JointIndex)
			{
				const FOracleJoint& Joint = Problem.Joints[JointIndex];
				const double FtMPa = Joint.Strength.TensileStrengthMPa;

				/*
				 * Keyed on DATA, not material: a joint with no tensile bond has nothing
				 * to crack, so it keeps the plastic no-tension form and no first-crack row
				 * is written — which is exactly what leaves every dry-stone (f_t = 0) joint
				 * bit-identical whether the flag is on or off. The guard is written as
				 * !(f_t > 0) so a NaN strength lands inside it rather than slipping past.
				 *
				 * DEFERRED, and benign (review 2026-08-21): unlike the tension row, this is
				 * NOT also gated on f_t < UncappedStrengthMPa. A bonded joint carrying an
				 * uncapped tensile strength would therefore get a first-crack row with RHS
				 * ~ 1e13 * A -- trivially slack, so a non-binding inequality that cannot change
				 * the optimum, and no sweep fixture drives it. Left ungated on purpose; if a
				 * fixture ever carries an uncapped-yet-bonded joint, add the same < Uncapped
				 * guard the tension row uses. Recorded in CURRENT_STATE.
				 */
				if (!(FtMPa > 0.0))
				{
					continue;
				}

				/*
				 * The joint's two contacts sit at contact indices 2J and 2J+1, at -/+ h,
				 * with signed normal (compression positive) n = n+ - n- in columns Base+0
				 * and Base+1. The uncracked peak-fibre condition -(n1+n2) + 3|n1-n2| <=
				 * f_t*A is two linear inequalities, one per sign of n1-n2, over the FULL
				 * joint face area (Joint.AreaSqCm = 2 * tributary), cutting the bonded
				 * section's plastic bending capacity to a third (PROMOTION_DESIGN Sec 4.3).
				 * f_t > 0 makes bCanTension true for both contacts, so the n- columns exist.
				 */
				const int32 Base1 = 1 + 4 * (2 * JointIndex);
				const int32 Base2 = 1 + 4 * (2 * JointIndex + 1);
				const double Rhs =
					FtMPa * OracleForceUnitsPerMPaSqCm * Joint.AreaSqCm;

				/* n1 >= n2 branch: -(n1+n2) + 3(n1-n2) = 2*n1 - 4*n2 <= f_t*A. */
				FAssemblyRow FirstCrackA;
				FirstCrackA.Add(Base1 + 0, 2.0);
				FirstCrackA.Add(Base1 + 1, -2.0);
				FirstCrackA.Add(Base2 + 0, -4.0);
				FirstCrackA.Add(Base2 + 1, 4.0);
				FirstCrackA.Rhs = Rhs;
				FirstCrackA.bEquality = false;
				AssemblyRows.Add(MoveTemp(FirstCrackA));

				/* n2 >= n1 branch: -(n1+n2) + 3(n2-n1) = -4*n1 + 2*n2 <= f_t*A. */
				FAssemblyRow FirstCrackB;
				FirstCrackB.Add(Base1 + 0, -4.0);
				FirstCrackB.Add(Base1 + 1, 4.0);
				FirstCrackB.Add(Base2 + 0, 2.0);
				FirstCrackB.Add(Base2 + 1, -2.0);
				FirstCrackB.Rhs = Rhs;
				FirstCrackB.bEquality = false;
				AssemblyRows.Add(MoveTemp(FirstCrackB));
			}
		}

		/* ---- Standard form and the revised simplex's working state. ------------- */
		FStandardForm Form;
		BuildStandardForm(AssemblyRows, NumStructCols, Form);

		/*
		 * THE WARM START, AND IT IS THE WHOLE OF WHAT A SUPPLIED BASIS DOES. An empty one
		 * touches nothing — no column is appended, no basis is reseeded, no branch below is
		 * taken — which is what lets every pinned cold pivot count in the sweep be the
		 * assertion that this seam changed nothing.
		 */
		if (Problem.StartingBasis.Columns.Num() > 0)
		{
			Result.WarmStartColumnsAccepted = SeedWarmStartBasis(Form, Problem.StartingBasis);
		}

		FRevisedState State;

		if (!State.Init(Form))
		{
			return Refuse(EOracleRefusal::PhaseOneFailure);
		}

		/* ---- Phase 1: drive the artificials to zero. ---------------------------- */
		int32 Iterations = 0;

		/*
		 * A gravity-live problem starts feasible (every equality's right-hand side is
		 * zero, so its artificial is basic AT ZERO): the phase-1 objective is already
		 * optimal and running the simplex would only churn degenerate pivots. Dead
		 * loads put real values on the artificials and phase 1 must genuinely run.
		 */
		if (BasicArtificialInfeasibility(Form, State) > InfeasibilityTolerance(Form, State))
		{
			TArray<double> PhaseOneCost;
			PhaseOneCost.SetNumZeroed(Form.NumCols);

			for (int32 Col = Form.ArtificialStart; Col < Form.NumCols; ++Col)
			{
				PhaseOneCost[Col] = 1.0;
			}

			const ESimplexEnd PhaseOneEnd = RunRevisedSimplex(
				State, PhaseOneCost, Form.NumCols, Iterations, true);
			Result.SimplexIterations = Iterations;
			Result.PricingColumnScans = State.PricingColumnScans;
			Result.BlandDegenerateEntries = State.BlandDegenerateEntries;
			Result.PhaseOnePivots = Iterations;
			Result.PivotsToFirstFeasible = State.PivotsToFirstFeasible;

			if (PhaseOneEnd != ESimplexEnd::Optimal)
			{
				/*
				 * Phase 1's three non-optimal ends are one reason rather than three: its
				 * objective is bounded below by zero so it cannot be unbounded, and no
				 * fixture has ever reached any of them. Splitting it would be inventing a
				 * distinction nothing can observe.
				 */
				return Refuse(EOracleRefusal::PhaseOneFailure);
			}

			if (BasicArtificialInfeasibility(Form, State) > InfeasibilityTolerance(Form, State))
			{
				/*
				 * The DEAD loads alone admit no equilibrium (lambda = 0 is in the
				 * feasible set of every gravity-live problem, so this is only reachable
				 * with dead loads). That is an answer, not a failure: nothing stands,
				 * lambda* = 0.
				 *
				 * This is the mechanism seam (PROMOTION_DESIGN §3.3, §12 D7). The phase-1
				 * dual here IS the Farkas certificate = the kinematic collapse mechanism;
				 * ExtractMechanism reads it (one BTRAN, no counted quantity moved) and
				 * Farkas-verifies it. A certificate that will not verify makes the solve
				 * REFUSE rather than hand out named bricks — the fail-closed direction §3.6
				 * adds on this arm.
				 */
				if (!ExtractMechanism(Problem, Form, State, EqFxRowOfBlock, Result.Mechanism))
				{
					return Refuse(EOracleRefusal::VerificationFailure, TEXT("Farkas certificate"));
				}

				Result.bAnswered = true;
				Result.Lambda = 0.0;
				ReportBasis(Form, State.Basis, Result.FinalBasis);
				return Result;
			}
		}
		else
		{
			/*
			 * The problem was feasible before a single pivot, so phase 1 spent nothing and
			 * an early exit at pivot zero would have saved nothing. Written as the branch's
			 * OTHER ARM rather than as an initialiser above the branch: a default of zero
			 * would let a phase 1 that forgot to report read as "already feasible", which is
			 * a plausible number where INDEX_NONE is a loud one.
			 */
			Result.PhaseOnePivots = 0;
			Result.PivotsToFirstFeasible = 0;
		}

		Result.SimplexIterations = Iterations;
		Result.PricingColumnScans = State.PricingColumnScans;
		Result.BlandDegenerateEntries = State.BlandDegenerateEntries;

		/*
		 * Pivot lingering zero-value artificials out where a real column allows it:
		 * row r's tableau entry for column j is (B^-1 A_j)[r] = rho . A_j with
		 * rho = B^-T e_r, so one BTRAN prices the whole candidate scan. The entering
		 * column is the LARGEST |alpha| (lowest index on exact ties), not the dense
		 * solver's first-past-the-tolerance: these pivots pick the basis every later
		 * phase-2 solve stands on, and a near-tolerance choice here was measured
		 * leaving a basis so ill-conditioned that a refactorised solve carried ~1e-6
		 * of noise into the verification gate on a problem whose answer was exact.
		 */
		for (int32 Row = 0; Row < Form.NumRows; ++Row)
		{
			if (State.Basis[Row] < Form.ArtificialStart)
			{
				continue;
			}

			if (State.PivotsSinceRefactor >= RefactoriseEvery)
			{
				if (!State.Refactorise())
				{
					return Refuse(EOracleRefusal::PhaseOneFailure);
				}
			}

			State.ScratchSlot.Init(0.0, Form.NumRows);
			State.ScratchSlot[Row] = 1.0;
			State.BtranScratchSlot(State.YRow);

			int32 Entering = INDEX_NONE;
			double EnteringAbs = PivotTol;

			for (int32 Col = 0; Col < Form.ArtificialStart; ++Col)
			{
				if (State.bIsBasic[Col])
				{
					continue;
				}

				double Alpha = 0.0;

				for (int32 At = Form.ColStart[Col]; At < Form.ColStart[Col + 1]; ++At)
				{
					Alpha += Form.ColVal[At] * State.YRow[Form.ColRow[At]];
				}

				++State.PricingColumnScans;

				if (FMath::Abs(Alpha) > EnteringAbs)
				{
					Entering = Col;
					EnteringAbs = FMath::Abs(Alpha);
				}
			}

			if (Entering == INDEX_NONE)
			{
				/*
				 * A genuinely redundant row: no real column can pivot the artificial out.
				 * It stays basic at zero — harmless, since pricing scans every real column
				 * exactly and complementary slackness certifies phase 2's optimum
				 * regardless of what sits in this row, and the post-solve verification
				 * gate fails closed if that certification is ever wrong.
				 */
				continue;
			}

			State.FtranColumn(Entering, State.EnteringW);

			const double Theta = State.XB[Row] / State.EnteringW[Row];
			State.ApplyPivot(Row, Entering, State.EnteringW, Theta);
		}

		/* ---- Phase 2: maximise lambda (minimise -lambda). ----------------------- */
		{
			TArray<double> PhaseTwoCost;
			PhaseTwoCost.SetNumZeroed(Form.NumCols);
			PhaseTwoCost[0] = -1.0;

			const ESimplexEnd PhaseTwoEnd = RunRevisedSimplex(
				State, PhaseTwoCost, Form.ArtificialStart, Iterations, false);
			Result.SimplexIterations = Iterations;
			Result.PricingColumnScans = State.PricingColumnScans;
			Result.BlandDegenerateEntries = State.BlandDegenerateEntries;

			if (PhaseTwoEnd != ESimplexEnd::Optimal)
			{
				/* With the cap row a real unbounded ray is impossible; fail closed. */
				return Refuse(PhaseTwoRefusalFor(PhaseTwoEnd));
			}
		}

		/*
		 * One final refactorisation so the values verification judges are the cleanest
		 * solve the final basis admits — factorised from original columns, basic values
		 * from the original right-hand side, no eta in sight.
		 */
		if (!State.Refactorise())
		{
			/* The same event the pivot loop's periodic refactorisation reports: the basis. */
			return Refuse(EOracleRefusal::PhaseTwoNumericalFailure);
		}

		/*
		 * VERIFY THE ANSWER AGAINST THE ORIGINAL ROWS, because a simplex basis that
		 * drifted through near-tolerance pivots can report "optimal" while standing
		 * outside the feasible region — measured at 0.98% over a crushing bound before
		 * this existed. The static theorem's whole output is "an admissible force
		 * system exists"; a solution that is not admissible is not an answer, so a
		 * verification failure fails CLOSED rather than returning a plausible number.
		 */
		TArray<double> StructValues;
		StructValues.SetNumZeroed(NumStructCols);

		for (int32 Row = 0; Row < Form.NumRows; ++Row)
		{
			if (State.Basis[Row] < NumStructCols)
			{
				StructValues[State.Basis[Row]] = FMath::Max(0.0, State.XB[Row]);
			}
		}

		for (int32 RowIndex = 0; RowIndex < AssemblyRows.Num(); ++RowIndex)
		{
			const FAssemblyRow& Assembly = AssemblyRows[RowIndex];

			double LeftHandSide = 0.0;
			double Magnitude = FMath::Abs(Assembly.Rhs);

			for (int32 Entry = 0; Entry < Assembly.Col.Num(); ++Entry)
			{
				const double Term = Assembly.Val[Entry] * StructValues[Assembly.Col[Entry]];
				LeftHandSide += Term;
				Magnitude += FMath::Abs(Term);
			}

			const double Tolerance = 1.0e-6 * (1.0 + Magnitude);

			const bool bViolated = Assembly.bEquality
				? FMath::Abs(LeftHandSide - Assembly.Rhs) > Tolerance
				: LeftHandSide > Assembly.Rhs + Tolerance;

			if (bViolated)
			{
				return Refuse(
					EOracleRefusal::VerificationFailure,
					FString::Printf(TEXT("against row %d"), RowIndex));
			}
		}

		Result.bAnswered = true;
		Result.Lambda = FMath::Clamp(StructValues[0], 0.0, LambdaCap);
		ReportBasis(Form, State.Basis, Result.FinalBasis);
		return Result;
	}

	/**
	 * ONE MIN-VIOLATION SUB-SOLVE: minimise the given per-structural-column cost over the assembly
	 * rows (equilibrium equalities HARD, everything else as posed), and hand back the structural
	 * primal. Factored out of SolveMinViolationReadout so the lexicographic-minimax loop can pose
	 * one LP per overload level without transcribing the two-phase machinery each time. Same
	 * machinery as the maximise arm — phase 1 to feasibility, pivot the zero artificials out,
	 * phase 2 to optimality, one clean refactorisation before extraction — and bOk is false with a
	 * refusal reason set on any non-optimal termination, so the caller fails closed.
	 */
	struct FSubSolve
	{
		bool bOk = false;
		EOracleRefusal Refusal = EOracleRefusal::None;
		TArray<double> StructValues;
		int32 Iterations = 0;
		int32 PricingColumnScans = 0;
		int32 BlandDegenerateEntries = 0;
	};

	FSubSolve SolveMinViolationLP(
		const TArray<OracleDetail::FAssemblyRow>& AssemblyRows, int32 NumStructCols,
		const TArray<double>& StructCost)
	{
		using namespace OracleDetail;

		FSubSolve Out;

		FStandardForm Form;
		BuildStandardForm(AssemblyRows, NumStructCols, Form);

		FRevisedState State;

		if (!State.Init(Form))
		{
			Out.Refusal = EOracleRefusal::PhaseOneFailure;
			return Out;
		}

		int32 Iterations = 0;

		/* ---- Phase 1: drive the equilibrium artificials to zero. ---- */
		if (BasicArtificialInfeasibility(Form, State) > InfeasibilityTolerance(Form, State))
		{
			TArray<double> PhaseOneCost;
			PhaseOneCost.SetNumZeroed(Form.NumCols);

			for (int32 Col = Form.ArtificialStart; Col < Form.NumCols; ++Col)
			{
				PhaseOneCost[Col] = 1.0;
			}

			const ESimplexEnd PhaseOneEnd =
				RunRevisedSimplex(State, PhaseOneCost, Form.NumCols, Iterations, false);

			if (PhaseOneEnd != ESimplexEnd::Optimal)
			{
				Out.Refusal = EOracleRefusal::PhaseOneFailure;
				return Out;
			}

			/*
			 * With the violations free the equilibrium equalities are always satisfiable, so a
			 * residual infeasibility means a block no force system can balance (a floating block).
			 * That is not a readout, it is a degenerate structure — fail closed.
			 */
			if (BasicArtificialInfeasibility(Form, State) > InfeasibilityTolerance(Form, State))
			{
				Out.Refusal = EOracleRefusal::PhaseOneFailure;
				return Out;
			}
		}

		/* Pivot any zero-value artificial out where a real column can, exactly as the maximise arm does. */
		for (int32 Row = 0; Row < Form.NumRows; ++Row)
		{
			if (State.Basis[Row] < Form.ArtificialStart)
			{
				continue;
			}

			if (State.PivotsSinceRefactor >= RefactoriseEvery)
			{
				if (!State.Refactorise())
				{
					Out.Refusal = EOracleRefusal::PhaseOneFailure;
					return Out;
				}
			}

			State.ScratchSlot.Init(0.0, Form.NumRows);
			State.ScratchSlot[Row] = 1.0;
			State.BtranScratchSlot(State.YRow);

			int32 Entering = INDEX_NONE;
			double EnteringAbs = PivotTol;

			for (int32 Col = 0; Col < Form.ArtificialStart; ++Col)
			{
				if (State.bIsBasic[Col])
				{
					continue;
				}

				double Alpha = 0.0;

				for (int32 At = Form.ColStart[Col]; At < Form.ColStart[Col + 1]; ++At)
				{
					Alpha += Form.ColVal[At] * State.YRow[Form.ColRow[At]];
				}

				++State.PricingColumnScans;

				if (FMath::Abs(Alpha) > EnteringAbs)
				{
					Entering = Col;
					EnteringAbs = FMath::Abs(Alpha);
				}
			}

			if (Entering == INDEX_NONE)
			{
				continue;
			}

			State.FtranColumn(Entering, State.EnteringW);

			const double Theta = State.XB[Row] / State.EnteringW[Row];
			State.ApplyPivot(Row, Entering, State.EnteringW, Theta);
		}

		/* ---- Phase 2: minimise the supplied structural cost. ---- */
		{
			TArray<double> PhaseTwoCost;
			PhaseTwoCost.SetNumZeroed(Form.NumCols);

			for (int32 Col = 0; Col < NumStructCols && Col < StructCost.Num(); ++Col)
			{
				PhaseTwoCost[Col] = StructCost[Col];
			}

			const ESimplexEnd PhaseTwoEnd =
				RunRevisedSimplex(State, PhaseTwoCost, Form.ArtificialStart, Iterations, false);

			if (PhaseTwoEnd != ESimplexEnd::Optimal)
			{
				Out.Refusal = PhaseTwoRefusalFor(PhaseTwoEnd);
				return Out;
			}
		}

		if (!State.Refactorise())
		{
			Out.Refusal = EOracleRefusal::PhaseTwoNumericalFailure;
			return Out;
		}

		Out.StructValues.SetNumZeroed(NumStructCols);

		for (int32 Row = 0; Row < Form.NumRows; ++Row)
		{
			if (State.Basis[Row] < NumStructCols)
			{
				Out.StructValues[State.Basis[Row]] = FMath::Max(0.0, State.XB[Row]);
			}
		}

		Out.Iterations = Iterations;
		Out.PricingColumnScans = State.PricingColumnScans;
		Out.BlandDegenerateEntries = State.BlandDegenerateEntries;
		Out.bOk = true;
		return Out;
	}

	/**
	 * THE MIN-VIOLATION (GOAL-PROGRAMMING) LP THAT SOURCES THE PER-JOINT STRAIN READOUT
	 * (PROMOTION_DESIGN §3.1/§3.5/§3.6, Slice 6a). A DIFFERENT solve from the maximise-lambda
	 * one above, and DELIBERATELY separate from it so that path stays bit-identical: nothing
	 * here touches SolveRigidBlockOnce, and SolveRigidBlock routes to one or the other on the
	 * flag alone.
	 *
	 * WHY A DIFFERENT SOLVE. At lambda >= 1 the maximise-lambda primal is DEFINED as the search
	 * for a force system in which no joint reads over 1.0, so feeding those forces through a
	 * utilisation reads "misleadingly comfortable" for every joint (§3.1). The readout instead
	 * fixes the load at lambda = 1 (self-weight as the dead load — gravity enters the equality
	 * rows as a constant, never scaled), keeps the per-block equilibrium EQUALITY rows HARD, and
	 * relaxes every STRENGTH inequality a_k.x <= b_k to a_k.x - s_k <= b_k with a non-negative
	 * violation s_k. Because equilibrium stays hard and only strength gives, a solution ALWAYS
	 * exists — the least-infeasible force system — so a STANDING structure reads every s_k zero
	 * and an OVER-capacity one reads positive s_k exactly on the over joints.
	 *
	 * THE CANONICALIZATION, AND WHY A SINGLE GLOBAL MINIMAX IS WRONG. A bare minimise-sum-of-slack
	 * is constant over a statically indeterminate free family (its total is fixed at demand minus
	 * capacity), so it lands on a permutation-dependent vertex and the readout WOBBLES with column
	 * order (§3.5). Adding a single global minimax t with s_k <= t evens the ONE globally-maximal
	 * group — but that t is pinned by that group alone, so any INDEPENDENT overload group at a
	 * lower level is a free family the objective then ignores: its distribution is still
	 * permutation-unstable and, worse, physically wrong (its slacks pile toward the global t
	 * instead of evening at their own centroid). Two independent groups at different levels is the
	 * counter-example the determinism gate carries.
	 *
	 * THE FIX IS PER-GROUP CANONICALIZATION VIA LEXICOGRAPHIC MINIMAX. Minimise the MAX slack;
	 * FIX the slacks that are critical at that max (stuck there in every optimum); then minimise
	 * the NEXT max over what remains; recurse until every slack is pinned. Each independent free
	 * family is thereby evened at ITS OWN level — its centroid, a function of the geometry and not
	 * the column indices, hence permutation-stable. Because the solver is LP-only, this is the
	 * LP-compatible route to what a strictly-convex (min-L2) slack objective would give in one
	 * solve: a bounded number of LPs, one pair per distinct overload level, and the readout is
	 * cached (solve-on-settle), not per frame.
	 *
	 * "CRITICAL" IS THE CRUX, AND A BARE "FIX EVERYTHING BINDING AT t*" IS WRONG. The level solve
	 * is free to leave a lower group's slack sitting at t* on a whim of the pivot path, and fixing
	 * it there would pin the wrong distribution (exactly the single-global-t failure, one level
	 * deep). So each level is TWO solves: a min-t to find t*, then a min-(sum of the candidates at
	 * t*) that pushes every candidate that CAN come down off t*. Only those that survive that
	 * second solve at t* are stuck in every optimum, and those are pinned — at the scalar t*, which
	 * is even by construction and therefore permutation-invariant. Termination is guaranteed: t* is
	 * achievable, so at least one slack is critical at every level, and there are finitely many.
	 */
	FOracleResult SolveMinViolationReadout(const FOracleProblem& Problem)
	{
		using namespace OracleDetail;

		FOracleResult Result;
		Result.bAnswered = false;
		Result.Lambda = 0.0;

		const auto Refuse =
			[&Result](EOracleRefusal Reason, const FString& Detail = FString()) -> FOracleResult&
		{
			Result.Refusal = Reason;
			Result.WhyNot = Detail.IsEmpty()
				? RefusalText(Reason)
				: RefusalText(Reason) + TEXT(": ") + Detail;

			return Result;
		};

		const FString InvalidReason = ValidateProblem(Problem);

		if (!InvalidReason.IsEmpty())
		{
			return Refuse(EOracleRefusal::InvalidProblem, InvalidReason);
		}

		const int32 NumJoints = Problem.Joints.Num();
		const int32 NumContacts = NumJoints * 2;

		/*
		 * STRUCTURAL COLUMNS: per contact [n+, n-, p, q] with n = n+ - n- (compression
		 * positive) and v = p - q, then one non-negative violation variable per strength row
		 * (violation column k is NumForceCols + k), and — only while a level solve is posed — ONE
		 * minimax variable t appended past them all. No lambda column and no cap row: the load is
		 * fixed at lambda = 1, so there is nothing to maximise and gravity is a dead constant.
		 */
		const int32 NumForceCols = 4 * NumContacts;

		struct FContact
		{
			int32 Joint = 0;
			double PosX = 0.0;
			double PosZ = 0.0;
			double TributaryAreaSqCm = 0.0;
			bool bCanTension = false;
		};

		TArray<FContact> Contacts;
		Contacts.Reserve(NumContacts);

		for (int32 JointIndex = 0; JointIndex < NumJoints; ++JointIndex)
		{
			const FOracleJoint& Joint = Problem.Joints[JointIndex];

			const double TangentX = -Joint.NormalZ;
			const double TangentZ = Joint.NormalX;

			for (int32 End = 0; End < 2; ++End)
			{
				const double Sign = End == 0 ? -1.0 : 1.0;

				FContact Contact;
				Contact.Joint = JointIndex;
				Contact.PosX = Joint.CentreXCm + Sign * Joint.HalfLengthCm * TangentX;
				Contact.PosZ = Joint.CentreZCm + Sign * Joint.HalfLengthCm * TangentZ;
				Contact.TributaryAreaSqCm = Joint.AreaSqCm / 2.0;
				Contact.bCanTension = Joint.Strength.TensileStrengthMPa > 0.0;
				Contacts.Add(Contact);
			}
		}

		TArray<FAssemblyRow> AssemblyRows;

		/* ---- Equilibrium: three HARD equalities per non-grounded block, load fixed. ---- */
		for (int32 BlockIndex = 0; BlockIndex < Problem.Blocks.Num(); ++BlockIndex)
		{
			const FOracleBlock& Block = Problem.Blocks[BlockIndex];

			if (Block.bGrounded)
			{
				continue;
			}

			FAssemblyRow RowFx;
			FAssemblyRow RowFz;
			FAssemblyRow RowM;

			/* Every load enters the right-hand side: dead self-weight, live forces held at lambda = 1. */
			double LoadX = 0.0, LoadZ = 0.0, LoadM = 0.0;

			const double WeightUu = Block.MassKg * OracleGravityCmPerSecondSquared;
			LoadZ -= WeightUu;

			for (const FOracleAppliedForce& Applied : Problem.AppliedForces)
			{
				if (Applied.Block != BlockIndex)
				{
					continue;
				}

				const double Rx = Applied.AtXCm - Block.CentroidXCm;
				const double Rz = Applied.AtZCm - Block.CentroidZCm;

				LoadX += Applied.ForceXUu;
				LoadZ += Applied.ForceZUu;
				LoadM += Rx * Applied.ForceZUu - Rz * Applied.ForceXUu;
			}

			for (int32 ContactIndex = 0; ContactIndex < Contacts.Num(); ++ContactIndex)
			{
				const FContact& Contact = Contacts[ContactIndex];
				const FOracleJoint& Joint = Problem.Joints[Contact.Joint];

				double SignForBlock = 0.0;

				if (Joint.BlockB == BlockIndex)
				{
					SignForBlock = 1.0;
				}
				else if (Joint.BlockA == BlockIndex)
				{
					SignForBlock = -1.0;
				}
				else
				{
					continue;
				}

				const double TangentX = -Joint.NormalZ;
				const double TangentZ = Joint.NormalX;

				const double Rx = Contact.PosX - Block.CentroidXCm;
				const double Rz = Contact.PosZ - Block.CentroidZCm;

				const double TorquePerNormal = Rx * Joint.NormalZ - Rz * Joint.NormalX;
				const double TorquePerShear = Rx * TangentZ - Rz * TangentX;

				const int32 Base = 4 * ContactIndex;

				RowFx.Add(Base + 0, SignForBlock * Joint.NormalX);
				RowFz.Add(Base + 0, SignForBlock * Joint.NormalZ);
				RowM.Add(Base + 0, SignForBlock * TorquePerNormal);

				if (Contact.bCanTension)
				{
					RowFx.Add(Base + 1, -SignForBlock * Joint.NormalX);
					RowFz.Add(Base + 1, -SignForBlock * Joint.NormalZ);
					RowM.Add(Base + 1, -SignForBlock * TorquePerNormal);
				}

				RowFx.Add(Base + 2, SignForBlock * TangentX);
				RowFz.Add(Base + 2, SignForBlock * TangentZ);
				RowM.Add(Base + 2, SignForBlock * TorquePerShear);

				RowFx.Add(Base + 3, -SignForBlock * TangentX);
				RowFz.Add(Base + 3, -SignForBlock * TangentZ);
				RowM.Add(Base + 3, -SignForBlock * TorquePerShear);
			}

			RowFx.Rhs = -LoadX;
			RowFz.Rhs = -LoadZ;
			RowM.Rhs = -LoadM;

			AssemblyRows.Add(MoveTemp(RowFx));
			AssemblyRows.Add(MoveTemp(RowFz));
			AssemblyRows.Add(MoveTemp(RowM));
		}

		/*
		 * ---- Strength rows, each RELAXED by its own violation variable. ----
		 *
		 * The coefficients are exactly the maximise-lambda solver's (the FINITE-TENSION
		 * mapping, the linearised Coulomb pair, crushing and the shear ceiling); the only
		 * change is that a_k.x <= b_k becomes a_k.x - s_k <= b_k. Each row records the joint it
		 * belongs to, its violation column and its capacity, so the readout can total the slack
		 * per joint and read a per-row utilisation demand / capacity back off the primal.
		 */
		struct FStrengthRowInfo
		{
			int32 Joint = INDEX_NONE;
			int32 ViolationCol = INDEX_NONE;
			int32 RowIndex = INDEX_NONE;
			double Capacity = 0.0;
		};

		TArray<FStrengthRowInfo> StrengthInfos;

		const auto AddStrengthRow =
			[&](int32 JointIndex, FAssemblyRow&& Row, double Capacity)
		{
			const int32 ViolationCol = NumForceCols + StrengthInfos.Num();
			Row.Add(ViolationCol, -1.0);
			Row.Rhs = Capacity;
			Row.bEquality = false;

			FStrengthRowInfo Info;
			Info.Joint = JointIndex;
			Info.ViolationCol = ViolationCol;
			Info.RowIndex = AssemblyRows.Num();
			Info.Capacity = Capacity;
			StrengthInfos.Add(Info);

			AssemblyRows.Add(MoveTemp(Row));
		};

		for (int32 ContactIndex = 0; ContactIndex < Contacts.Num(); ++ContactIndex)
		{
			const FContact& Contact = Contacts[ContactIndex];
			const FConnectionStrength& S = Problem.Joints[Contact.Joint].Strength;
			const int32 Base = 4 * ContactIndex;

			const double Conv = OracleForceUnitsPerMPaSqCm;
			const double AreaSqCm = Contact.TributaryAreaSqCm;

			if (Contact.bCanTension && S.TensileStrengthMPa < UncappedStrengthMPa)
			{
				FAssemblyRow Tension;
				Tension.Add(Base + 1, 1.0);
				AddStrengthRow(Contact.Joint, MoveTemp(Tension), S.TensileStrengthMPa * Conv * AreaSqCm);
			}

			if (S.ShearCohesionMPa < UncappedStrengthMPa)
			{
				for (int32 Orientation = 0; Orientation < 2; ++Orientation)
				{
					const double ShearSign = Orientation == 0 ? 1.0 : -1.0;

					FAssemblyRow Friction;
					Friction.Add(Base + 0, -S.FrictionCoefficient);

					if (Contact.bCanTension)
					{
						Friction.Add(Base + 1, S.FrictionCoefficient);
					}

					Friction.Add(Base + 2, ShearSign);
					Friction.Add(Base + 3, -ShearSign);
					AddStrengthRow(Contact.Joint, MoveTemp(Friction), S.ShearCohesionMPa * Conv * AreaSqCm);
				}
			}

			if (S.CompressiveStrengthMPa < UncappedStrengthMPa)
			{
				FAssemblyRow Crush;
				Crush.Add(Base + 0, 1.0);

				if (Contact.bCanTension)
				{
					Crush.Add(Base + 1, -1.0);
				}

				AddStrengthRow(Contact.Joint, MoveTemp(Crush), S.CompressiveStrengthMPa * Conv * AreaSqCm);
			}

			if (S.MaxShearStrengthMPa < UncappedStrengthMPa)
			{
				for (int32 Orientation = 0; Orientation < 2; ++Orientation)
				{
					const double ShearSign = Orientation == 0 ? 1.0 : -1.0;

					FAssemblyRow Ceiling;
					Ceiling.Add(Base + 2, ShearSign);
					Ceiling.Add(Base + 3, -ShearSign);
					AddStrengthRow(Contact.Joint, MoveTemp(Ceiling), S.MaxShearStrengthMPa * Conv * AreaSqCm);
				}
			}
		}

		/*
		 * ---- First-crack rows, each RELAXED by its own violation variable. ----
		 *
		 * The maximise-lambda path writes the uncracked peak-fibre limit for every bonded joint
		 * behind bFirstCrackRows, and below the cap the break authority poses it (Structure.cpp),
		 * so a bonded joint cracks at three times the plastic no-tension bending stiffness. The
		 * readout must assemble the SAME rows or its utilisation reports the plastic capacity a
		 * bonded bending joint is never actually held to — four times too comfortable at e = 3h.
		 * Each of the two sign branches of |n1 - n2| is one relaxed inequality carrying its OWN
		 * violation slack through AddStrengthRow, so it joins the per-group lexicographic-minimax
		 * canonicalization and the fixed-point reduction exactly like every other strength row
		 * rather than being special-cased out of it.
		 *
		 * Keyed on DATA (f_t > 0), not material: a dry joint has nothing to crack, gets no row and
		 * stays bit-identical whether the flag is on or off. The guard is written !(f_t > 0) so a
		 * NaN strength lands inside it. Columns follow the min-violation layout (Base = 4 *
		 * ContactIndex, no lambda column), the joint's two contacts sit at contact indices 2J and
		 * 2J+1 at -/+ h, and the RHS is over the FULL joint face (Joint.AreaSqCm = 2 * tributary),
		 * cutting the plastic bending capacity to a third (PROMOTION_DESIGN Sec 4.3). Like the
		 * maximise-lambda path this is NOT gated on f_t < UncappedStrengthMPa: an uncapped-yet-bonded
		 * joint's row is trivially slack, and no fixture drives it (CURRENT_STATE 0d residue c).
		 */
		if (Problem.bFirstCrackRows)
		{
			for (int32 JointIndex = 0; JointIndex < NumJoints; ++JointIndex)
			{
				const FOracleJoint& Joint = Problem.Joints[JointIndex];
				const double FtMPa = Joint.Strength.TensileStrengthMPa;

				if (!(FtMPa > 0.0))
				{
					continue;
				}

				const int32 Base1 = 4 * (2 * JointIndex);
				const int32 Base2 = 4 * (2 * JointIndex + 1);
				const double Rhs = FtMPa * OracleForceUnitsPerMPaSqCm * Joint.AreaSqCm;

				/* n1 >= n2 branch: -(n1+n2) + 3(n1-n2) = 2*n1 - 4*n2 <= f_t*A. */
				FAssemblyRow FirstCrackA;
				FirstCrackA.Add(Base1 + 0, 2.0);
				FirstCrackA.Add(Base1 + 1, -2.0);
				FirstCrackA.Add(Base2 + 0, -4.0);
				FirstCrackA.Add(Base2 + 1, 4.0);
				AddStrengthRow(JointIndex, MoveTemp(FirstCrackA), Rhs);

				/* n2 >= n1 branch: -(n1+n2) + 3(n2-n1) = -4*n1 + 2*n2 <= f_t*A. */
				FAssemblyRow FirstCrackB;
				FirstCrackB.Add(Base1 + 0, -4.0);
				FirstCrackB.Add(Base1 + 1, 4.0);
				FirstCrackB.Add(Base2 + 0, 2.0);
				FirstCrackB.Add(Base2 + 1, -2.0);
				AddStrengthRow(JointIndex, MoveTemp(FirstCrackB), Rhs);
			}
		}

		const int32 NumStrengthRows = StrengthInfos.Num();
		const int32 NumStructBase = NumForceCols + NumStrengthRows;

		/*
		 * BaseRows holds the invariant physics — the per-block equilibrium equalities and the
		 * relaxed strength rows. Every level of the lexicographic minimax below reuses these and
		 * appends only its own per-slack rows (a minimax bound while a slack is free, an equality
		 * once it is pinned), so the heavy assembly is built once. A level solve appends ONE
		 * minimax column t at NumStructBase; a bound/final solve appends none.
		 */
		const TArray<FAssemblyRow> BaseRows = MoveTemp(AssemblyRows);

		TArray<bool> bPinned;
		TArray<double> PinnedValue;
		bPinned.Init(false, NumStrengthRows);
		PinnedValue.Init(0.0, NumStrengthRows);

		int32 TotalIterations = 0;
		int32 TotalScans = 0;
		int32 TotalBland = 0;

		const auto Accumulate = [&](const FSubSolve& Sub)
		{
			TotalIterations += Sub.Iterations;
			TotalScans += Sub.PricingColumnScans;
			TotalBland += Sub.BlandDegenerateEntries;
		};

		/*
		 * Assemble BaseRows plus one row per strength slack: an equality s_k = pinned value once
		 * the slack is pinned, otherwise a minimax bound s_k - t <= 0 (level mode) or a constant
		 * bound s_k <= LevelBound (reduction / final mode).
		 */
		const auto AssembleWithSlackRows =
			[&](bool bLevelMode, double LevelBound) -> TArray<FAssemblyRow>
		{
			TArray<FAssemblyRow> Rows = BaseRows;

			for (int32 Info = 0; Info < NumStrengthRows; ++Info)
			{
				const int32 ViolationCol = NumForceCols + Info;

				FAssemblyRow Row;
				Row.Add(ViolationCol, 1.0);

				if (bPinned[Info])
				{
					Row.Rhs = PinnedValue[Info];
					Row.bEquality = true;
				}
				else if (bLevelMode)
				{
					Row.Add(NumStructBase, -1.0); /* s_k - t <= 0 */
					Row.Rhs = 0.0;
					Row.bEquality = false;
				}
				else
				{
					Row.Rhs = LevelBound; /* s_k <= LevelBound */
					Row.bEquality = false;
				}

				Rows.Add(MoveTemp(Row));
			}

			return Rows;
		};

		/*
		 * HOW BINDING AT t* IS DECIDED. A slack within BindingRelativeTol of t* (relative to t*,
		 * floored at 1) counts as binding — chosen against the two-group fixture, whose level gap
		 * is ~38000 uu while a reducible slack falls clear to its floor and a critical slack reads
		 * t* to solver precision (~1e-9 of scale). Any tolerance between those two extremes selects
		 * the same critical set, so this is a ruling in a constant's clothes: 1e-6 sits three
		 * orders above the solve noise and orders below the gap, and the determinism gate is what
		 * forbids picking it by eye. It never decides the pinned VALUE — that is always the scalar
		 * t*, even by construction and hence permutation-invariant.
		 */
		constexpr double BindingRelativeTol = 1.0e-6;

		while (true)
		{
			bool bAnyFree = false;

			for (int32 Info = 0; Info < NumStrengthRows; ++Info)
			{
				if (!bPinned[Info])
				{
					bAnyFree = true;
					break;
				}
			}

			if (!bAnyFree)
			{
				break;
			}

			/* ---- Level solve: minimise the max free slack t. ---- */
			const TArray<FAssemblyRow> LevelRows = AssembleWithSlackRows(true, 0.0);

			TArray<double> LevelCost;
			LevelCost.Init(0.0, NumStructBase + 1);
			LevelCost[NumStructBase] = 1.0;

			const FSubSolve Level = SolveMinViolationLP(LevelRows, NumStructBase + 1, LevelCost);
			Accumulate(Level);

			if (!Level.bOk)
			{
				return Refuse(Level.Refusal);
			}

			const double TStar = Level.StructValues[NumStructBase];
			const double BindTol = BindingRelativeTol * FMath::Max(1.0, FMath::Abs(TStar));

			TArray<int32> Candidates;

			for (int32 Info = 0; Info < NumStrengthRows; ++Info)
			{
				if (!bPinned[Info] && Level.StructValues[NumForceCols + Info] >= TStar - BindTol)
				{
					Candidates.Add(Info);
				}
			}

			/*
			 * ---- Reduction to a FIXED POINT: shrink the at-t* set until only the truly-critical
			 * slacks remain. ----
			 *
			 * A single min-Sigma reduction pushes the candidate SUM down but is INDIFFERENT to a
			 * trade WITHIN a reducible sub-family that shares a fixed subtotal — two contacts of one
			 * joint, or any pair whose net force is what equilibrium fixes. When both members are in
			 * the cost, rebalancing one down and its partner up is net-zero in the sum, so the simplex
			 * leaves an arbitrary (column-order-dependent) vertex where one member is stranded at t*
			 * while its partner sits low. Pinning that stranded member is the false-critical the Degen
			 * fixture exposes: it is reducible, not critical, and its split then wobbles with column
			 * order.
			 *
			 * The cure is to re-minimise over ONLY the slacks still reading t*. A member reducible
			 * solely by rebalancing to a partner OUTSIDE that shrunk set now has an unpenalised
			 * partner, so the trade strictly cuts the objective and the member drops clear of t*. A
			 * member that stays at t* through this — because reducing it would need another at-ceiling
			 * slack to rise past t*, which the bound forbids — is genuinely stuck in every optimum.
			 * The at-t* set is monotonically non-increasing (nothing exceeds the t* bound, so a
			 * dropped slack never returns), so the iteration reaches a stable set in at most one LP per
			 * candidate. That stable set is the critical set — a function of the physics, not the
			 * column order — and its members pin at the scalar t*, even by construction.
			 */
			const TArray<FAssemblyRow> BoundRows = AssembleWithSlackRows(false, TStar);

			while (true)
			{
				TArray<double> ReduceCost;
				ReduceCost.Init(0.0, NumStructBase);

				for (int32 Candidate : Candidates)
				{
					ReduceCost[NumForceCols + Candidate] = 1.0;
				}

				const FSubSolve Reduce = SolveMinViolationLP(BoundRows, NumStructBase, ReduceCost);
				Accumulate(Reduce);

				if (!Reduce.bOk)
				{
					return Refuse(Reduce.Refusal);
				}

				TArray<int32> StillAtLevel;

				for (int32 Candidate : Candidates)
				{
					if (Reduce.StructValues[NumForceCols + Candidate] >= TStar - BindTol)
					{
						StillAtLevel.Add(Candidate);
					}
				}

				if (StillAtLevel.Num() == Candidates.Num())
				{
					break;
				}

				Candidates = MoveTemp(StillAtLevel);
			}

			int32 PinnedThisLevel = 0;

			for (int32 Candidate : Candidates)
			{
				bPinned[Candidate] = true;
				PinnedValue[Candidate] = TStar;
				++PinnedThisLevel;
			}

			/*
			 * At least one candidate is critical whenever a free slack remains — t* is achievable,
			 * so some slack is stuck at it in every optimum. If the arithmetic ever disagrees the
			 * loop would not progress; fail closed rather than spin.
			 */
			if (PinnedThisLevel == 0)
			{
				return Refuse(
					EOracleRefusal::VerificationFailure,
					TEXT("lexicographic minimax made no progress at a level"));
			}
		}

		/* ---- Final solve: every slack pinned, read the canonical equilibrium force system back. ---- */
		const TArray<FAssemblyRow> FinalRows = AssembleWithSlackRows(false, 0.0);

		TArray<double> FinalCost;
		FinalCost.Init(0.0, NumStructBase);

		const FSubSolve Final = SolveMinViolationLP(FinalRows, NumStructBase, FinalCost);
		Accumulate(Final);

		if (!Final.bOk)
		{
			return Refuse(Final.Refusal);
		}

		const TArray<double>& StructValues = Final.StructValues;

		/* Verify the primal against the ORIGINAL physics — equilibrium HARD, strength relaxed. */
		for (int32 RowIndex = 0; RowIndex < BaseRows.Num(); ++RowIndex)
		{
			const FAssemblyRow& Assembly = BaseRows[RowIndex];

			double LeftHandSide = 0.0;
			double Magnitude = FMath::Abs(Assembly.Rhs);

			for (int32 Entry = 0; Entry < Assembly.Col.Num(); ++Entry)
			{
				const double Term = Assembly.Val[Entry] * StructValues[Assembly.Col[Entry]];
				LeftHandSide += Term;
				Magnitude += FMath::Abs(Term);
			}

			const double Tolerance = 1.0e-6 * (1.0 + Magnitude);

			const bool bViolated = Assembly.bEquality
				? FMath::Abs(LeftHandSide - Assembly.Rhs) > Tolerance
				: LeftHandSide > Assembly.Rhs + Tolerance;

			if (bViolated)
			{
				return Refuse(
					EOracleRefusal::VerificationFailure,
					FString::Printf(TEXT("against row %d"), RowIndex));
			}
		}

		Result.SimplexIterations = TotalIterations;
		Result.PricingColumnScans = TotalScans;
		Result.BlandDegenerateEntries = TotalBland;

		/*
		 * ---- The per-joint readout. ----
		 *
		 * NormalUu = n1 + n2 (compression positive), MomentUuCm = HalfLength * (n1 - n2), read off
		 * the two contacts' net normals n = n+ - n-. ViolationUu totals the joint's slack. The
		 * utilisation is the WORST per-row demand / capacity, which matches FConnection's worst-axis
		 * logic and equals 1 + ViolationUu / capacity on the governing axis so Slice 6b can consume
		 * it. Fail closed: a non-positive or non-finite capacity carrying real demand reads OVER, and
		 * a NaN demand reads OVER, rather than the comfortable side.
		 */
		FOracleReadout& Readout = Result.Readout;
		Readout.Joints.SetNum(NumJoints);

		/* Capacities run to tens of thousands of force units; this only separates a real cap from zero. */
		constexpr double CapacityFloorUu = 1.0e-6;
		constexpr double FailClosedUtilisation = 1.0e12;

		for (int32 JointIndex = 0; JointIndex < NumJoints; ++JointIndex)
		{
			const FOracleJoint& Joint = Problem.Joints[JointIndex];
			const int32 Base1 = 4 * (2 * JointIndex);
			const int32 Base2 = 4 * (2 * JointIndex + 1);

			const double N1 = StructValues[Base1 + 0] - StructValues[Base1 + 1];
			const double N2 = StructValues[Base2 + 0] - StructValues[Base2 + 1];

			FOracleJointReadout& Out = Readout.Joints[JointIndex];
			Out.NormalUu = N1 + N2;
			Out.MomentUuCm = Joint.HalfLengthCm * (N1 - N2);

			double Violation = 0.0;
			double Utilisation = 0.0;

			for (const FStrengthRowInfo& Info : StrengthInfos)
			{
				if (Info.Joint != JointIndex)
				{
					continue;
				}

				Violation += StructValues[Info.ViolationCol];

				/* The row's demand is its force-column terms alone, i.e. the LHS without the -s term. */
				double Demand = 0.0;

				const FAssemblyRow& Row = BaseRows[Info.RowIndex];

				for (int32 Entry = 0; Entry < Row.Col.Num(); ++Entry)
				{
					if (Row.Col[Entry] != Info.ViolationCol)
					{
						Demand += Row.Val[Entry] * StructValues[Row.Col[Entry]];
					}
				}

				double RowUtilisation;

				if (Info.Capacity > CapacityFloorUu)
				{
					RowUtilisation = Demand / Info.Capacity;

					if (!FMath::IsFinite(RowUtilisation))
					{
						RowUtilisation = FailClosedUtilisation;
					}
				}
				else
				{
					/*
					 * Zero-capacity axis: any real demand is over. Written as a refused negation
					 * so a NaN demand lands here too — !(Demand <= floor) is true for NaN (every
					 * comparison against NaN is false), which is the fail-closed side.
					 */
					RowUtilisation = !(Demand <= CapacityFloorUu) ? FailClosedUtilisation : 0.0;
				}

				Utilisation = FMath::Max(Utilisation, RowUtilisation);
			}

			Out.ViolationUu = Violation;
			Out.Utilisation = Utilisation;
		}

		Readout.bPresent = true;
		Result.bAnswered = true;
		Result.Lambda = 1.0;
		return Result;
	}

	/**
	 * A WARM START THAT LEADS THE SOLVER INTO A DEAD END IS THROWN AWAY, NOT BELIEVED, AND
	 * NOT ALLOWED TO COST AN ANSWER. A supplied basis is a hint; a hint that ends in a
	 * refusal — the basis going singular under refactorisation, or an optimum that fails
	 * verification against the original rows — says only that this starting point was a bad
	 * one, and the problem still has the answer the cold start would have found. So a refused
	 * warm attempt is followed by ONE cold solve, and that is the answer.
	 *
	 * MEASURED 2026-08-16, and this arm is reached: wall-01's mapped basis (12,459 of 13,362
	 * columns carried) goes singular at the first periodic refactorisation, pivot 64 — with
	 * AND WITHOUT the infeasibility repair, so it is the mapped basis that is fragile and not
	 * anything the repair adds. The 84- and 150-block rows never reach here.
	 *
	 * TWO THINGS THIS MUST NOT DO, both of which would make the fallback a place for a wrong
	 * answer to hide. It does not weaken the verification gate — a verification failure is
	 * still a refusal, and the retry is a fresh solve of the ORIGINAL problem that must pass
	 * the same gate. And it does not report the discarded attempt as free: the wasted pivots
	 * and scans are added to the counts, and WarmStartColumnsAccepted is reported as ZERO,
	 * which is the field's documented "asked and got nothing" — because a warm start that was
	 * abandoned IS a cold start wearing a hat, and reporting the columns it briefly held would
	 * hide exactly the thing the count exists to expose.
	 */
	FOracleResult SolveRigidBlock(const FOracleProblem& Problem)
	{
		/*
		 * THE READOUT IS A DIFFERENT SOLVE, ROUTED HERE AND NOWHERE ELSE, so the maximise-lambda
		 * path below stays bit-identical with the flag off — no sweep pin (OracleSweepFull
		 * included) can move. The min-violation LP takes no warm start; it always solves cold.
		 */
		if (Problem.bMinViolationReadout)
		{
			return SolveMinViolationReadout(Problem);
		}

		if (Problem.StartingBasis.Columns.Num() == 0)
		{
			return SolveRigidBlockOnce(Problem);
		}

		const FOracleResult Warm = SolveRigidBlockOnce(Problem);

		if (Warm.bAnswered)
		{
			return Warm;
		}

		FOracleProblem Cold = Problem;
		Cold.StartingBasis = FOracleBasis();

		FOracleResult Result = SolveRigidBlockOnce(Cold);
		Result.SimplexIterations += Warm.SimplexIterations;
		Result.PricingColumnScans += Warm.PricingColumnScans;
		Result.BlandDegenerateEntries += Warm.BlandDegenerateEntries;

		/* INDEX_NONE is "not reported" and adding to it would make it a plausible number. */
		if (Result.PhaseOnePivots >= 0 && Warm.PhaseOnePivots >= 0)
		{
			Result.PhaseOnePivots += Warm.PhaseOnePivots;
		}

		Result.WarmStartColumnsAccepted = 0;
		return Result;
	}

	EOracleOutcome OutcomeOf(const FOracleResult& Result)
	{
		if (!Result.bAnswered)
		{
			return EOracleOutcome::Unanswerable;
		}

		return Result.Lambda >= 1.0 ? EOracleOutcome::Stands : EOracleOutcome::Falls;
	}
}
