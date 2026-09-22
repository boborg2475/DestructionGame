// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/*
 * Sparse LU with product-form eta updates, and the revised-simplex working state. Split out of
 * RigidBlockOracle.cpp (PROMOTION_DESIGN Slice 1) so tests can fuzz it. Internal, not a public API.
 */
namespace RigidBlockOracle
{
	namespace OracleDetail
	{
		/*
		 * An LU pivot at or below this means a singular basis. Absolute, since rows are
		 * equilibrated to max |coefficient| = 1.
		 */
		constexpr double SingularPivotTol = 1.0e-11;

		/**
		 * The LP in standard form, as an immutable sparse column matrix: rows equilibrated to
		 * max |coefficient| = 1 with non-negative RHS; columns [structural | slacks | one
		 * artificial per row]. Immutability makes refactorisation a clean reset.
		 */
		struct FStandardForm
		{
			int32 NumRows = 0;
			int32 NumCols = 0;
			int32 NumStructCols = 0;
			int32 ArtificialStart = 0;

			// Compressed sparse columns; row indices ascend within each column.
			TArray<int32> ColStart;
			TArray<int32> ColRow;
			TArray<double> ColVal;

			/** Per column, sqrt(1 + sum of squared coefficients): static steepest-edge weight. */
			TArray<double> ColNorm;

			/** Oriented right-hand side, non-negative. */
			TArray<double> Rhs;

			/**
			 * Per row, the signed scale applied to the assembly row (equilibration times
			 * orientation sign). The unscaled dual is y[r] * RowScaleSigned[r]; used only by
			 * ExtractMechanism (PROMOTION_DESIGN §3.3), not by the solve.
			 */
			TArray<double> RowScaleSigned;

			/** Per row: the slack column if feasible as a start, else the artificial. */
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
		 * Sparse LU of the basis with row partial pivoting, left-looking (Gilbert-Peierls: DFS
		 * reach over L's pattern, then eliminate on a dense workspace). Pivot is the largest
		 * magnitude, lowest row index on ties, so the result is deterministic. Reached positions
		 * are processed in sorted order rather than topological order, which is valid since L is
		 * lower triangular in position space.
		 *
		 * L's row indices are original rows during factorisation and remapped to positions at
		 * the end.
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

			// Workspaces, reused across columns and refactorisations.
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
			 * The initial-basis unit column of the lowest unpivoted row. It always pivots at
			 * magnitude 1, so one substitution always repairs.
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
			 * bRepairSingular is for warm starts only. A warm basis may have gone singular in
			 * the reduced problem; with the flag set, a column that cannot pivot is replaced by
			 * an unassigned row's cold column and retried. Basis is updated in place.
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

					// Largest magnitude among unassigned rows, lowest index on ties.
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

						// No replacement, or the same column, would loop forever; fall back to cold start.
						if (Replacement == INDEX_NONE || Replacement == Col)
						{
							return false;
						}

						// Retry this position; the loop's ++ undoes the --.
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

				// Remap L into position space.
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

			/** Solve yT B = c: input by basis slot (overwritten as scratch), output by original row. */
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
		 * x_B = B^-1 b with one pass of iterative refinement, which cuts ill-conditioned solve
		 * noise from ~1e-6 to rounding. Separate from Refactorise because warm-start seeding
		 * needs the values unclamped: there a negative is a real infeasibility.
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

			// r = b - B*x, x += B^-1 r.
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

		/** One product-form update: column at Slot replaced by one with FTRAN image w. */
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
			 * Instrumentation only (see FOracleResult). Columns priced against a dual vector;
			 * iterations entered under the Bland fallback; and the first pivot at which phase 1
			 * was feasible within tolerance (INDEX_NONE if never). The solver always continues
			 * to optimality.
			 */
			int64 PricingColumnScans = 0;

			int32 BlandDegenerateEntries = 0;

			int32 PivotsToFirstFeasible = INDEX_NONE;

			// Scratch buffers, reused so hot loops do not allocate.
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
			 * Error reset: refactorise from the original columns and recompute basic values
			 * (with refinement). Tiny negatives are rounding and are clamped; the final answer is
			 * verified against the unscaled rows anyway. Not valid for a warm-start basis, where
			 * negatives are real infeasibility, so SeedWarmStartBasis reads them first.
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
