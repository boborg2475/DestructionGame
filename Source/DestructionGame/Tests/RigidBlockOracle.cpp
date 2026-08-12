// Copyright Epic Games, Inc. All Rights Reserved.

#include "Tests/RigidBlockOracle.h"

#if WITH_DEV_AUTOMATION_TESTS

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
		 * An LU pivot at or below this is a singular basis, refused rather than divided
		 * by. Absolute, because rows are equilibrated to max |coefficient| = 1 before
		 * anything reaches the factorisation.
		 */
		constexpr double SingularPivotTol = 1.0e-11;

		/*
		 * A strength at or beyond this is "no cap at all" (Unbreakable's 1e12, the
		 * MaxShear default of DBL_MAX): the row is omitted rather than written with an
		 * astronomically large right-hand side that would wreck the problem's scaling.
		 */
		constexpr double UncappedStrengthMPa = 1.0e9;

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

			/** Oriented right-hand side, non-negative by construction. */
			TArray<double> Rhs;

			/** Per row: the slack column where feasible as a start, else the artificial. */
			TArray<int32> InitialBasis;
		};

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
		}

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

			bool Factorise(const FStandardForm& Form, const TArray<int32>& Basis)
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

						return false;
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
			 * and recompute the basic values from the original right-hand side, with ONE
			 * PASS OF ITERATIVE REFINEMENT — the residual b - B*x is formed against the
			 * original columns and a correction solved, which knocks the solve noise of
			 * an ill-conditioned basis (the lambda cap's 1e6 right-hand side amplifies
			 * it) from ~1e-6 down to rounding, so the verification gate judges the basis
			 * itself and not the solver's arithmetic. Tiny negative basic values after
			 * that are rounding at degenerate vertices and are clamped to zero; a
			 * genuinely infeasible basis cannot hide behind the clamp because the final
			 * answer is verified against the original unscaled rows.
			 */
			bool Refactorise()
			{
				if (!Factor.Factorise(*Form, Basis))
				{
					return false;
				}

				Etas.Reset();
				PivotsSinceRefactor = 0;

				Factor.FTranFactor(Form->Rhs, XB);

				/* One refinement pass: r = b - B*x, x += B^-1 r. */
				ScratchOrig.Init(0.0, Form->NumRows);

				for (int32 Slot = 0; Slot < Form->NumRows; ++Slot)
				{
					const int32 Col = Basis[Slot];
					const double Value = XB[Slot];

					if (Value != 0.0)
					{
						for (int32 At = Form->ColStart[Col]; At < Form->ColStart[Col + 1]; ++At)
						{
							ScratchOrig[Form->ColRow[At]] += Form->ColVal[At] * Value;
						}
					}
				}

				for (int32 Row = 0; Row < Form->NumRows; ++Row)
				{
					ScratchOrig[Row] = Form->Rhs[Row] - ScratchOrig[Row];
				}

				Factor.FTranFactor(ScratchOrig, ScratchSlot);

				for (int32 Slot = 0; Slot < Form->NumRows; ++Slot)
				{
					XB[Slot] += ScratchSlot[Slot];
				}

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

		enum class ESimplexEnd : uint8
		{
			Optimal,
			Unbounded,
			IterationCap,
			NumericalFailure,
		};

		/**
		 * Minimise the given objective. Every choice below is INDEX-DETERMINISTIC — no
		 * randomness, no hashing — so the pivot path, and therefore the last bit of
		 * lambda*, is a pure function of the input arrays.
		 *
		 * The entering rule is DANTZIG (most negative reduced cost, lowest index on
		 * ties), priced EXACTLY each iteration from a fresh BTRAN rather than from a
		 * maintained row — the maintained row's drift was the dense solver's disease.
		 * The ratio test breaks near-ties by the LARGEST PIVOT ELEMENT (then lowest
		 * basic index): Bland's rule alone was measured accepting a basis 0.98% outside
		 * the crushing envelope on the dry 8-course stack, because it happily pivots on
		 * near-tolerance elements. Bland remains as the ANTI-CYCLING FALLBACK: after a
		 * long streak of zero-length steps the entering rule drops to lowest-index,
		 * which restores the termination guarantee where it is needed.
		 */
		ESimplexEnd RunRevisedSimplex(
			FRevisedState& S, const TArray<double>& Cost, int32 AllowedCols,
			int32& InOutIterations)
		{
			const FStandardForm& Form = *S.Form;
			int32 DegenerateStreak = 0;

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

				/* Price: y solves yT B = c_B, then d_j = c_j - y . A_j, exact. */
				S.ScratchSlot.SetNumUninitialized(Form.NumRows);

				for (int32 Slot = 0; Slot < Form.NumRows; ++Slot)
				{
					S.ScratchSlot[Slot] = Cost[S.Basis[Slot]];
				}

				S.BtranScratchSlot(S.YRow);

				int32 Entering = INDEX_NONE;
				double MostNegative = -CostTol;

				for (int32 Col = 0; Col < AllowedCols; ++Col)
				{
					if (S.bIsBasic[Col])
					{
						continue;
					}

					const double Reduced = S.ReducedCost(Col, Cost);

					if (Reduced < MostNegative)
					{
						Entering = Col;

						if (bBlandFallback)
						{
							break;
						}

						MostNegative = Reduced;
					}
				}

				if (Entering == INDEX_NONE)
				{
					return ESimplexEnd::Optimal;
				}

				S.FtranColumn(Entering, S.EnteringW);

				int32 Leaving = INDEX_NONE;
				double BestRatio = 0.0;
				double LeavingMagnitude = 0.0;

				for (int32 Row = 0; Row < Form.NumRows; ++Row)
				{
					const double Coefficient = S.EnteringW[Row];

					if (Coefficient <= PivotTol)
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
			}
		}
	}

	FOracleResult SolveRigidBlock(const FOracleProblem& Problem)
	{
		using namespace OracleDetail;

		FOracleResult Result;
		Result.bAnswered = false;
		Result.Lambda = 0.0;

		Result.WhyNot = ValidateProblem(Problem);

		if (!Result.WhyNot.IsEmpty())
		{
			return Result;
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

		/* ---- Equilibrium: three equalities per non-grounded block. -------------- */
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

		/* ---- Standard form and the revised simplex's working state. ------------- */
		FStandardForm Form;
		BuildStandardForm(AssemblyRows, NumStructCols, Form);

		FRevisedState State;

		if (!State.Init(Form))
		{
			Result.WhyNot = TEXT("phase-1 simplex failed");
			return Result;
		}

		const auto BasicArtificialInfeasibility = [&Form, &State]()
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
		};

		const auto InfeasibilityTolerance = [&Form, &State]()
		{
			double LargestRhs = 0.0;

			for (int32 Row = 0; Row < Form.NumRows; ++Row)
			{
				LargestRhs = FMath::Max(LargestRhs, FMath::Abs(State.XB[Row]));
			}

			return (1.0 + LargestRhs) * 1.0e-9 * double(Form.NumRows);
		};

		/* ---- Phase 1: drive the artificials to zero. ---------------------------- */
		int32 Iterations = 0;

		/*
		 * A gravity-live problem starts feasible (every equality's right-hand side is
		 * zero, so its artificial is basic AT ZERO): the phase-1 objective is already
		 * optimal and running the simplex would only churn degenerate pivots. Dead
		 * loads put real values on the artificials and phase 1 must genuinely run.
		 */
		if (BasicArtificialInfeasibility() > InfeasibilityTolerance())
		{
			TArray<double> PhaseOneCost;
			PhaseOneCost.SetNumZeroed(Form.NumCols);

			for (int32 Col = Form.ArtificialStart; Col < Form.NumCols; ++Col)
			{
				PhaseOneCost[Col] = 1.0;
			}

			const ESimplexEnd PhaseOneEnd = RunRevisedSimplex(
				State, PhaseOneCost, Form.NumCols, Iterations);
			Result.SimplexIterations = Iterations;

			if (PhaseOneEnd != ESimplexEnd::Optimal)
			{
				Result.WhyNot = TEXT("phase-1 simplex failed");
				return Result;
			}

			if (BasicArtificialInfeasibility() > InfeasibilityTolerance())
			{
				/*
				 * The DEAD loads alone admit no equilibrium (lambda = 0 is in the
				 * feasible set of every gravity-live problem, so this is only reachable
				 * with dead loads). That is an answer, not a failure: nothing stands,
				 * lambda* = 0.
				 */
				Result.bAnswered = true;
				Result.Lambda = 0.0;
				return Result;
			}
		}

		Result.SimplexIterations = Iterations;

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
					Result.WhyNot = TEXT("phase-1 simplex failed");
					return Result;
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
				State, PhaseTwoCost, Form.ArtificialStart, Iterations);
			Result.SimplexIterations = Iterations;

			if (PhaseTwoEnd != ESimplexEnd::Optimal)
			{
				/* With the cap row a real unbounded ray is impossible; fail closed. */
				Result.WhyNot = TEXT("phase-2 simplex failed");
				return Result;
			}
		}

		/*
		 * One final refactorisation so the values verification judges are the cleanest
		 * solve the final basis admits — factorised from original columns, basic values
		 * from the original right-hand side, no eta in sight.
		 */
		if (!State.Refactorise())
		{
			Result.WhyNot = TEXT("phase-2 simplex failed");
			return Result;
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
				Result.WhyNot = FString::Printf(
					TEXT("the optimal basis failed verification against row %d"), RowIndex);
				return Result;
			}
		}

		Result.bAnswered = true;
		Result.Lambda = FMath::Clamp(StructValues[0], 0.0, LambdaCap);
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

	bool BuildRigidBlockProblem(
		const FStructure& Structure,
		FOracleProblem& OutProblem,
		FString& OutWhyNot)
	{
		OutProblem = FOracleProblem();
		OutWhyNot.Empty();

		/*
		 * A defaulted centre or rectangle would silently become a lever arm "at the
		 * origin"; the structure's own completeness question is exactly this guard.
		 */
		if (!Structure.HasCompleteGeometry())
		{
			OutWhyNot = TEXT("the structure does not have complete geometry, so honest "
				"lever arms cannot be built");
			return false;
		}

		TArray<int32> BlockOfPiece;
		BlockOfPiece.Init(INDEX_NONE, Structure.NumPieces());

		for (int32 Piece = 0; Piece < Structure.NumPieces(); ++Piece)
		{
			if (Structure.IsPieceRemoved(Piece))
			{
				continue;
			}

			const FStructurePiece& Data = Structure.GetPiece(Piece);

			if (!Data.bIsInTheStructure)
			{
				continue;
			}

			BlockOfPiece[Piece] = OutProblem.Blocks.Num();

			FOracleBlock Block;
			Block.MassKg = Data.MassKg;
			Block.CentroidXCm = Data.CentreOfMassCm.X;
			Block.CentroidZCm = Data.CentreOfMassCm.Z;
			Block.bGrounded = Data.bIsGrounded;
			OutProblem.Blocks.Add(Block);
		}

		for (int32 Index = 0; Index < Structure.NumConnections(); ++Index)
		{
			const FConnection& Joint = Structure.GetConnection(Index);

			/* A joint that has given is out of the structure — latch included. */
			if (Joint.HasGiven())
			{
				continue;
			}

			if (Joint.PieceA < 0 || Joint.PieceA >= BlockOfPiece.Num()
				|| Joint.PieceB < 0 || Joint.PieceB >= BlockOfPiece.Num()
				|| BlockOfPiece[Joint.PieceA] == INDEX_NONE
				|| BlockOfPiece[Joint.PieceB] == INDEX_NONE)
			{
				/* A live joint on a removed piece is the known tombstone hole. */
				OutWhyNot = FString::Printf(
					TEXT("joint %d is live but names a piece that is not"), Index);
				OutProblem = FOracleProblem();
				return false;
			}

			const FOracleBlock& BlockA = OutProblem.Blocks[BlockOfPiece[Joint.PieceA]];
			const FOracleBlock& BlockB = OutProblem.Blocks[BlockOfPiece[Joint.PieceB]];

			/* Two grounded ends constrain nothing the earth does not already absorb. */
			if (BlockA.bGrounded && BlockB.bGrounded)
			{
				continue;
			}

			FVector Normal = Joint.InterfaceNormal;

			if (!Normal.Normalize())
			{
				OutWhyNot = FString::Printf(TEXT("joint %d has a degenerate normal"), Index);
				OutProblem = FOracleProblem();
				return false;
			}

			if (FMath::Abs(Normal.Y) > 1.0e-9)
			{
				OutWhyNot = FString::Printf(
					TEXT("joint %d has an out-of-plane (Y) normal, which a 2D X-Z ")
					TEXT("oracle must refuse rather than project"), Index);
				OutProblem = FOracleProblem();
				return false;
			}

			FOracleJoint Out;
			Out.BlockA = BlockOfPiece[Joint.PieceA];
			Out.BlockB = BlockOfPiece[Joint.PieceB];
			Out.NormalX = Normal.X;
			Out.NormalZ = Normal.Z;
			Out.CentreXCm = Joint.InterfaceCentreCm.X;
			Out.CentreZCm = Joint.InterfaceCentreCm.Z;

			/*
			 * The in-plane half length: the rectangle's extent on the X-Z axis that is
			 * not the separation axis. The wythe (Y) extent enters through the area
			 * alone, exactly as it does in production's stress arithmetic.
			 */
			Out.HalfLengthCm = FMath::Abs(Normal.Z) >= FMath::Abs(Normal.X)
				? Joint.InterfaceHalfExtentCm.X
				: Joint.InterfaceHalfExtentCm.Z;

			Out.AreaSqCm = Joint.InterfaceAreaSqCm;
			Out.Strength = Joint.Strength;
			OutProblem.Joints.Add(Out);
		}

		return true;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
