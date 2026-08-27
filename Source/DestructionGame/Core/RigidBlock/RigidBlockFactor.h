// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/*
 * THE SPARSE LU + PRODUCT-FORM ETA FACTORISATION AND THE REVISED-SIMPLEX WORKING
 * STATE, LIFTED OUT OF RigidBlockOracle.cpp VERBATIM so the factorisation the solver
 * now ships (PROMOTION_DESIGN Slice 1) can be fuzzed from a separate translation unit
 * against an independent oracle. Nothing here changed in the move; the assembly,
 * pricing, warm-start and SolveRigidBlock entry points stay in the .cpp. These are
 * OracleDetail internals, not a public API: the only non-test consumer is the .cpp
 * that used to define them inline.
 */
namespace RigidBlockOracle
{
	namespace OracleDetail
	{
		/*
		 * An LU pivot at or below this is a singular basis, refused rather than divided
		 * by. Absolute, because rows are equilibrated to max |coefficient| = 1 before
		 * anything reaches the factorisation.
		 */
		constexpr double SingularPivotTol = 1.0e-11;

		/**
		 * The LP in standard form, held as an UNCHANGING sparse column matrix: rows
		 * equilibrated to max |coefficient| = 1 and oriented to a non-negative
		 * right-hand side, columns [structural | slacks | one artificial per row].
		 * Nothing here is ever written after construction — the revised simplex reads
		 * columns out of it and represents the basis separately, which is what makes
		 * refactorisation a genuine reset to clean data.
		 */
		struct FStandardForm
		{
			int32 NumRows = 0;
			int32 NumCols = 0;
			int32 NumStructCols = 0;
			int32 ArtificialStart = 0;

			/* Compressed sparse columns. Row indices ascend within each column. */
			TArray<int32> ColStart;
			TArray<int32> ColRow;
			TArray<double> ColVal;

			/**
			 * Per column, sqrt(1 + sum of its squared coefficients) — the static
			 * steepest-edge weight the entering choice ranks by. Constant data like
			 * everything else here: computed once from the untouched matrix.
			 */
			TArray<double> ColNorm;

			/** Oriented right-hand side, non-negative by construction. */
			TArray<double> Rhs;

			/** Per row: the slack column where feasible as a start, else the artificial. */
			TArray<int32> InitialBasis;
		};

		/** One column of the L or U factor, sparse. */
		struct FFactorColumn
		{
			TArray<int32> Row;
			TArray<double> Val;

			void Reset()
			{
				Row.Reset();
				Val.Reset();
			}
		};

		/**
		 * A sparse LU factorisation of the basis with row partial pivoting: left-looking
		 * column-at-a-time (Gilbert-Peierls shape — a depth-first reach per column over
		 * L's pattern, then a scatter/eliminate/split on a dense workspace), pivot row
		 * chosen by LARGEST MAGNITUDE with LOWEST ORIGINAL INDEX on exact ties, which is
		 * what keeps the whole factorisation a pure function of the input arrays. The
		 * reached positions are processed in ascending pivot order — a valid elimination
		 * order because L is lower triangular in position space — rather than the
		 * classic topological order, trading a sort for simplicity at validation scale.
		 *
		 * During factorisation L's row indices are ORIGINAL row numbers (the rows below
		 * the diagonal have no position yet); a single remap after the last column turns
		 * everything into position space, where both triangular solves are ordinary.
		 */
		struct FBasisFactor
		{
			int32 M = 0;
			TArray<FFactorColumn> LCols;
			TArray<FFactorColumn> UCols;
			TArray<double> UDiag;

			/** Position -> original row, and its inverse. */
			TArray<int32> Perm;
			TArray<int32> Pinv;

			/* Workspaces, reused across columns and across refactorisations. */
			TArray<double> Work;
			TArray<int32> VisitStamp;
			int32 Stamp = 0;
			TArray<int32> DfsStack;
			TArray<int32> Pattern;
			TArray<int32> Positions;

			void ReachFrom(int32 StartRow)
			{
				DfsStack.Reset();
				DfsStack.Push(StartRow);

				while (DfsStack.Num() > 0)
				{
					const int32 RowIndex = DfsStack.Pop();

					if (VisitStamp[RowIndex] == Stamp)
					{
						continue;
					}

					VisitStamp[RowIndex] = Stamp;
					Pattern.Add(RowIndex);

					const int32 Pos = Pinv[RowIndex];

					if (Pos != INDEX_NONE)
					{
						Positions.Add(Pos);

						for (int32 Child : LCols[Pos].Row)
						{
							if (VisitStamp[Child] != Stamp)
							{
								DfsStack.Push(Child);
							}
						}
					}
				}
			}

			/**
			 * The cold-start column of the LOWEST row that has no pivot yet — the slack or
			 * artificial FStandardForm::InitialBasis gave it, which is a unit column in that
			 * row alone. That is what makes it a repair the factorisation can always take: an
			 * unassigned row's unit column pivots at magnitude 1 whatever else the basis
			 * holds, so a substitution never needs a second substitution.
			 */
			int32 ColdColumnForAnUnassignedRow(const FStandardForm& Form) const
			{
				for (int32 Row = 0; Row < Form.NumRows; ++Row)
				{
					if (Pinv[Row] == INDEX_NONE)
					{
						return Form.InitialBasis[Row];
					}
				}

				return INDEX_NONE;
			}

			/**
			 * bRepairSingular IS FOR A WARM START AND NOTHING ELSE, and it is false on every
			 * cold path, so the arithmetic below is untouched by its existence. A caller's
			 * basis is a HINT: its columns were independent in the problem it came from, and
			 * dropping some of that problem's rows and columns does not inherit independence.
			 * With the flag set, a column that cannot pivot is REPLACED by the cold default of
			 * an unassigned row and the position retried, so a hint that has gone singular
			 * costs the caller the rows it named rather than the whole answer — a warm start
			 * that failed closed would measure nothing. Basis is written in place, which is
			 * how the caller learns what survived.
			 */
			bool Factorise(
				const FStandardForm& Form, TArray<int32>& Basis, bool bRepairSingular = false)
			{
				M = Form.NumRows;
				LCols.SetNum(M);
				UCols.SetNum(M);
				UDiag.SetNum(M);
				Perm.Init(INDEX_NONE, M);
				Pinv.Init(INDEX_NONE, M);
				Work.Init(0.0, M);
				VisitStamp.Init(0, M);
				Stamp = 0;

				for (int32 Position = 0; Position < M; ++Position)
				{
					LCols[Position].Reset();
					UCols[Position].Reset();
				}

				for (int32 Position = 0; Position < M; ++Position)
				{
					++Stamp;
					Pattern.Reset();
					Positions.Reset();

					const int32 Col = Basis[Position];

					for (int32 At = Form.ColStart[Col]; At < Form.ColStart[Col + 1]; ++At)
					{
						ReachFrom(Form.ColRow[At]);
					}

					Positions.Sort();

					for (int32 At = Form.ColStart[Col]; At < Form.ColStart[Col + 1]; ++At)
					{
						Work[Form.ColRow[At]] = Form.ColVal[At];
					}

					for (int32 Reached : Positions)
					{
						const double Value = Work[Perm[Reached]];

						if (Value != 0.0)
						{
							const FFactorColumn& L = LCols[Reached];

							for (int32 Entry = 0; Entry < L.Row.Num(); ++Entry)
							{
								Work[L.Row[Entry]] -= L.Val[Entry] * Value;
							}
						}
					}

					/* Pivot: largest magnitude among unassigned rows, lowest index tied. */
					int32 PivotRow = INDEX_NONE;
					double PivotAbs = 0.0;

					for (int32 RowIndex : Pattern)
					{
						if (Pinv[RowIndex] != INDEX_NONE)
						{
							continue;
						}

						const double Abs = FMath::Abs(Work[RowIndex]);

						if (Abs > PivotAbs
							|| (Abs == PivotAbs && PivotRow != INDEX_NONE && RowIndex < PivotRow))
						{
							PivotRow = RowIndex;
							PivotAbs = Abs;
						}
					}

					if (PivotRow == INDEX_NONE || PivotAbs <= SingularPivotTol)
					{
						for (int32 RowIndex : Pattern)
						{
							Work[RowIndex] = 0.0;
						}

						if (!bRepairSingular)
						{
							return false;
						}

						const int32 Replacement = ColdColumnForAnUnassignedRow(Form);

						/*
						 * A replacement that is the column which just failed, or no unassigned
						 * row at all, would repair nothing and loop forever; both are refused
						 * rather than retried, which drops the caller back to a cold start.
						 */
						if (Replacement == INDEX_NONE || Replacement == Col)
						{
							return false;
						}

						/* Retry THIS position with the replacement: the for's ++ undoes the --. */
						Basis[Position] = Replacement;
						--Position;
						continue;
					}

					const double Pivot = Work[PivotRow];

					for (int32 Reached : Positions)
					{
						const double Value = Work[Perm[Reached]];

						if (Value != 0.0)
						{
							UCols[Position].Row.Add(Reached);
							UCols[Position].Val.Add(Value);
						}
					}

					UDiag[Position] = Pivot;

					for (int32 RowIndex : Pattern)
					{
						if (Pinv[RowIndex] == INDEX_NONE && RowIndex != PivotRow
							&& Work[RowIndex] != 0.0)
						{
							LCols[Position].Row.Add(RowIndex);
							LCols[Position].Val.Add(Work[RowIndex] / Pivot);
						}
					}

					Perm[Position] = PivotRow;
					Pinv[PivotRow] = Position;

					for (int32 RowIndex : Pattern)
					{
						Work[RowIndex] = 0.0;
					}
				}

				/* Everything has a position now: move L into position space. */
				for (int32 Position = 0; Position < M; ++Position)
				{
					for (int32& RowIndex : LCols[Position].Row)
					{
						RowIndex = Pinv[RowIndex];
					}
				}

				return true;
			}

			/** Solve B z = a: input indexed by original row, output by basis slot. */
			void FTranFactor(const TArray<double>& OrigVec, TArray<double>& OutSlot) const
			{
				OutSlot.SetNumUninitialized(M);

				for (int32 Position = 0; Position < M; ++Position)
				{
					OutSlot[Position] = OrigVec[Perm[Position]];
				}

				for (int32 Position = 0; Position < M; ++Position)
				{
					const double Value = OutSlot[Position];

					if (Value != 0.0)
					{
						const FFactorColumn& L = LCols[Position];

						for (int32 Entry = 0; Entry < L.Row.Num(); ++Entry)
						{
							OutSlot[L.Row[Entry]] -= L.Val[Entry] * Value;
						}
					}
				}

				for (int32 Position = M - 1; Position >= 0; --Position)
				{
					OutSlot[Position] /= UDiag[Position];

					const double Value = OutSlot[Position];

					if (Value != 0.0)
					{
						const FFactorColumn& U = UCols[Position];

						for (int32 Entry = 0; Entry < U.Row.Num(); ++Entry)
						{
							OutSlot[U.Row[Entry]] -= U.Val[Entry] * Value;
						}
					}
				}
			}

			/**
			 * Solve yT B = c: input indexed by basis slot (CONSUMED as scratch), output
			 * by original row — the shape pricing needs to dot against original columns.
			 */
			void BTranFactor(TArray<double>& SlotVec, TArray<double>& OutOrig) const
			{
				for (int32 Position = 0; Position < M; ++Position)
				{
					double Sum = SlotVec[Position];
					const FFactorColumn& U = UCols[Position];

					for (int32 Entry = 0; Entry < U.Row.Num(); ++Entry)
					{
						Sum -= U.Val[Entry] * SlotVec[U.Row[Entry]];
					}

					SlotVec[Position] = Sum / UDiag[Position];
				}

				for (int32 Position = M - 1; Position >= 0; --Position)
				{
					double Sum = SlotVec[Position];
					const FFactorColumn& L = LCols[Position];

					for (int32 Entry = 0; Entry < L.Row.Num(); ++Entry)
					{
						Sum -= L.Val[Entry] * SlotVec[L.Row[Entry]];
					}

					SlotVec[Position] = Sum;
				}

				OutOrig.Init(0.0, M);

				for (int32 Position = 0; Position < M; ++Position)
				{
					OutOrig[Perm[Position]] = SlotVec[Position];
				}
			}
		};

		/**
		 * THE BASIC VALUES OF A BASIS, x_B = B^-1 b, WITH ONE PASS OF ITERATIVE REFINEMENT:
		 * the residual b - B*x is formed against the original columns and a correction
		 * solved, which knocks the solve noise of an ill-conditioned basis (the lambda cap's
		 * 1e6 right-hand side amplifies it) from ~1e-6 down to rounding.
		 *
		 * It is a free function rather than part of Refactorise because TWO places need it —
		 * the refactorisation, which clamps the tiny negatives it leaves, and the warm-start
		 * seeding, which must read them UNCLAMPED because on a warm basis a negative basic
		 * value is not rounding at a degenerate vertex but a real infeasibility to repair.
		 * Two transcriptions of one arithmetic is how the two quietly stop agreeing.
		 */
		inline void SolveBasicValues(
			const FStandardForm& Form,
			const FBasisFactor& Factor,
			const TArray<int32>& Basis,
			TArray<double>& OutXB,
			TArray<double>& ScratchOrig,
			TArray<double>& ScratchSlot)
		{
			Factor.FTranFactor(Form.Rhs, OutXB);

			/* One refinement pass: r = b - B*x, x += B^-1 r. */
			ScratchOrig.Init(0.0, Form.NumRows);

			for (int32 Slot = 0; Slot < Form.NumRows; ++Slot)
			{
				const int32 Col = Basis[Slot];
				const double Value = OutXB[Slot];

				if (Value != 0.0)
				{
					for (int32 At = Form.ColStart[Col]; At < Form.ColStart[Col + 1]; ++At)
					{
						ScratchOrig[Form.ColRow[At]] += Form.ColVal[At] * Value;
					}
				}
			}

			for (int32 Row = 0; Row < Form.NumRows; ++Row)
			{
				ScratchOrig[Row] = Form.Rhs[Row] - ScratchOrig[Row];
			}

			Factor.FTranFactor(ScratchOrig, ScratchSlot);

			for (int32 Slot = 0; Slot < Form.NumRows; ++Slot)
			{
				OutXB[Slot] += ScratchSlot[Slot];
			}
		}

		/**
		 * One product-form update: the basis column at Slot was replaced by a column
		 * whose FTRAN image was w, held as its pivot element and off-pivot nonzeros.
		 */
		struct FEta
		{
			int32 Slot = 0;
			double Diag = 1.0;
			TArray<int32> Idx;
			TArray<double> Val;

			void ApplyFtran(TArray<double>& SlotVec) const
			{
				const double Pivot = SlotVec[Slot] / Diag;
				SlotVec[Slot] = Pivot;

				if (Pivot != 0.0)
				{
					for (int32 Entry = 0; Entry < Idx.Num(); ++Entry)
					{
						SlotVec[Idx[Entry]] -= Val[Entry] * Pivot;
					}
				}
			}

			void ApplyBtran(TArray<double>& SlotVec) const
			{
				double Sum = SlotVec[Slot];

				for (int32 Entry = 0; Entry < Idx.Num(); ++Entry)
				{
					Sum -= Val[Entry] * SlotVec[Idx[Entry]];
				}

				SlotVec[Slot] = Sum / Diag;
			}
		};

		/** The revised simplex's working state: basis, factorisation, etas, values. */
		struct FRevisedState
		{
			const FStandardForm* Form = nullptr;

			TArray<int32> Basis;
			TArray<bool> bIsBasic;
			TArray<double> XB;

			FBasisFactor Factor;
			TArray<FEta> Etas;
			int32 PivotsSinceRefactor = 0;

			/*
			 * Instrumentation only — nothing branches on it. One count per column priced
			 * against a dual vector, wherever that happens; FOracleResult's field carries
			 * the reasoning about why the number is worth reporting.
			 */
			int64 PricingColumnScans = 0;

			/*
			 * Instrumentation only — nothing branches on it. One count per iteration
			 * entered with the Bland fallback in force; FOracleResult's field carries the
			 * reasoning about why an unfired branch is worth counting.
			 */
			int32 BlandDegenerateEntries = 0;

			/*
			 * Instrumentation only — nothing branches on it, AND THAT IS THE CONTRACT OF
			 * THIS SLICE. The pivot at which phase 1's basic-artificial infeasibility sum
			 * first came inside tolerance, which is the first moment an early exit COULD
			 * have fired; the solver keeps going to optimality exactly as before, because
			 * an early exit returns a feasible point rather than an optimal one and that is
			 * a different contract nobody has taken. INDEX_NONE means the sum never came
			 * inside tolerance — an infeasible problem has no such pivot.
			 */
			int32 PivotsToFirstFeasible = INDEX_NONE;

			/* Scratch buffers, reused so the hot loops never allocate. */
			TArray<double> ScratchOrig;
			TArray<double> ScratchSlot;
			TArray<double> YRow;
			TArray<double> EnteringW;

			bool Init(const FStandardForm& InForm)
			{
				Form = &InForm;
				Basis = InForm.InitialBasis;
				bIsBasic.Init(false, InForm.NumCols);

				for (int32 Col : Basis)
				{
					bIsBasic[Col] = true;
				}

				return Refactorise();
			}

			/**
			 * THE ERROR RESET: refactorise the basis from the original sparse columns
			 * and recompute the basic values from the original right-hand side, one pass
			 * of iterative refinement included (SolveBasicValues), so the verification
			 * gate judges the basis itself and not the solver's arithmetic. Tiny negative
			 * basic values after that are rounding at degenerate vertices and are clamped
			 * to zero; a genuinely infeasible basis cannot hide behind the clamp because
			 * the final answer is verified against the original unscaled rows.
			 *
			 * THAT JUSTIFICATION IS ABOUT A BASIS THE SIMPLEX ITSELF BUILT, and it is
			 * exactly false of one handed in from outside. A warm start is normally PRIMAL
			 * INFEASIBLE — the deleted joints were carrying force — and there a negative
			 * basic value is not rounding at all but the infeasibility phase 1 exists to
			 * repair, which the clamp would launder into a plausible feasible-looking
			 * point. That is why SeedWarmStartBasis reads the values BEFORE this function
			 * ever runs on them, and turns each one into phase-1 work that can be seen.
			 */
			bool Refactorise()
			{
				if (!Factor.Factorise(*Form, Basis))
				{
					return false;
				}

				Etas.Reset();
				PivotsSinceRefactor = 0;

				SolveBasicValues(*Form, Factor, Basis, XB, ScratchOrig, ScratchSlot);

				for (double& Value : XB)
				{
					if (Value < 0.0)
					{
						Value = 0.0;
					}
				}

				return true;
			}

			/** w = B^-1 * (column Col of the original matrix), in slot space. */
			void FtranColumn(int32 Col, TArray<double>& OutSlot)
			{
				ScratchOrig.Init(0.0, Form->NumRows);

				for (int32 At = Form->ColStart[Col]; At < Form->ColStart[Col + 1]; ++At)
				{
					ScratchOrig[Form->ColRow[At]] = Form->ColVal[At];
				}

				Factor.FTranFactor(ScratchOrig, OutSlot);

				for (const FEta& Eta : Etas)
				{
					Eta.ApplyFtran(OutSlot);
				}
			}

			/** y (original-row space) with yT B = the slot-space vector in ScratchSlot. */
			void BtranScratchSlot(TArray<double>& OutOrig)
			{
				for (int32 Index = Etas.Num() - 1; Index >= 0; --Index)
				{
					Etas[Index].ApplyBtran(ScratchSlot);
				}

				Factor.BTranFactor(ScratchSlot, OutOrig);
			}

			double ReducedCost(int32 Col, const TArray<double>& Cost) const
			{
				double Sum = Cost[Col];

				for (int32 At = Form->ColStart[Col]; At < Form->ColStart[Col + 1]; ++At)
				{
					Sum -= ColValDot(At);
				}

				return Sum;
			}

			double ColValDot(int32 At) const
			{
				return Form->ColVal[At] * YRow[Form->ColRow[At]];
			}

			/** Replace the basis column at Leaving with Entering, eta-recorded. */
			void ApplyPivot(int32 Leaving, int32 Entering, const TArray<double>& W, double Theta)
			{
				for (int32 Row = 0; Row < Form->NumRows; ++Row)
				{
					XB[Row] -= Theta * W[Row];

					if (XB[Row] < 0.0)
					{
						XB[Row] = 0.0;
					}
				}

				XB[Leaving] = Theta > 0.0 ? Theta : 0.0;

				FEta Eta;
				Eta.Slot = Leaving;
				Eta.Diag = W[Leaving];

				for (int32 Row = 0; Row < Form->NumRows; ++Row)
				{
					if (Row != Leaving && W[Row] != 0.0)
					{
						Eta.Idx.Add(Row);
						Eta.Val.Add(W[Row]);
					}
				}

				Etas.Add(MoveTemp(Eta));

				bIsBasic[Basis[Leaving]] = false;
				bIsBasic[Entering] = true;
				Basis[Leaving] = Entering;
				++PivotsSinceRefactor;
			}
		};

	}
}
