// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockFactor.h"

/*
 * LP assembly and a sparse revised simplex; formulation is in RigidBlockOracle.h. Replaced a
 * dense tableau whose accumulated rounding failed verification past ~4,000 pivots. The basis
 * (LU plus eta file) is refactorised from the original data every RefactoriseEvery pivots and
 * once before extraction. Tie-breaks are index-based, so results are bit-reproducible.
 */
namespace RigidBlockOracle
{
	namespace OracleDetail
	{
		// Pivot and reduced-cost tolerances on row-scaled data (max |coefficient| = 1).
		constexpr double PivotTol = 1.0e-9;
		constexpr double CostTol = 1.0e-9;

		/*
		 * Ratio-test pivots must also exceed this fraction of the entering column's largest entry.
		 * B^-1 is not equilibrated, so FTRAN images reach 3e7 and a 4.7e-9 pivot is relative
		 * rounding that later factorises singular. 1e-11 still let two eight-course-cover walls
		 * through (2026-08-15); 1e-9 fixes both. Skipped coefficients can overstep the ratio test;
		 * ApplyPivot clamps the result to zero. Re-run the slow group whenever this moves.
		 */
		constexpr double RelativePivotTol = 1.0e-9;

		/*
		 * The termination guarantee: the Bland fallback is entering-only, so the no-cycling
		 * theorem does not apply. Hitting the cap reports failure.
		 */
		constexpr int32 MaxPivots = 100000;

		// Eta updates between refactorisations; 64 sits comfortably inside every validation tolerance.
		constexpr int32 RefactoriseEvery = 64;


		/*
		 * A block moves iff its virtual-motion magnitude, relative to the largest block's, exceeds
		 * this (PROMOTION_DESIGN §3.3, §12 D7). Relative because the certificate has arbitrary scale.
		 * Rounding reads ~1e-9, real motion O(1). Joints are read from block velocities, which are
		 * unique, not the degenerate plastic multipliers (see ExtractMechanism).
		 */
		constexpr double MechanismRelativeTol = 1.0e-6;


		// Strengths at or above this are uncapped; the row is omitted to protect scaling.
		constexpr double UncappedStrengthMPa = 1.0e9;

		/*
		 * Friction-pyramid facets approximating the 3D Coulomb cone, which is not linear
		 * (THREED_DESIGN §"The 3D physics"). 8 includes the 45-degree diagonals.
		 */
		constexpr int32 ThreeDFrictionPyramidFacets = 8;

		/*
		 * cos(pi/8): scales the octagon's facets so it is inscribed in the cone. Uninscribed, a
		 * shear between facets passes up to 8.2% past the Coulomb limit. Literal, not FMath::Cos.
		 */
		constexpr double ThreeDPyramidInscribeFactor = 0.92387953251128674;

		/*
		 * Partial pricing: columns per window and candidates kept. Full Dantzig left 30-course walls
		 * pivoting for minutes. On the 8x10 PricingCost wall, 384 took 2,016 pivots and 538,200
		 * scans; 768 fewer pivots but more scans. Same answer to the bit.
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
			// IsFinite rejects NaN; the sign clause alone would pass it.
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

				// Dim2D checks the normal over X and Z only (refusing a Y normal); Dim3D includes Y.
				const bool bThreeDimensional = Problem.Dim == EOracleDim::Dim3D;

				if (!FMath::IsFinite(Joint.NormalX) || !FMath::IsFinite(Joint.NormalZ)
					|| (bThreeDimensional && !FMath::IsFinite(Joint.NormalY)))
				{
					return FString::Printf(TEXT("joint %d: normal must be finite"), Index);
				}

				const double NormalLengthSq = Joint.NormalX * Joint.NormalX
					+ Joint.NormalZ * Joint.NormalZ
					+ (bThreeDimensional ? Joint.NormalY * Joint.NormalY : 0.0);
				const double NormalLength = FMath::Sqrt(NormalLengthSq);

				// Validates, never normalises; same door policy as AddConnection.
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

				// NaN anywhere is refused, including the shear ceiling (a NaN cap would read as uncapped).
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
		 * Build the standard form. Rows are scaled by their coefficients only, never the RHS (the
		 * lambda cap's 1e6 would push real coefficients under the pivot tolerance). Negative-RHS
		 * rows are flipped; each starts on its slack if still +1, else an artificial.
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

				if (RowSlackCol[RowIndex] != INDEX_NONE && !bFlip)
				{
					Out.InitialBasis[RowIndex] = RowSlackCol[RowIndex];
				}
				else
				{
					Out.InitialBasis[RowIndex] = Out.ArtificialStart + RowIndex;
				}
			}

			// Count nonzeros per column, then fill by ascending row for determinism.
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

			// Static pricing weights, off the finished matrix.
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
		 * Append -A_Source and return its index. Past ArtificialStart, so it is treated as an
		 * artificial: priced in phase 1, never in phase 2, ignored by extraction. Warm start only;
		 * the cold path must not see it.
		 */
		int32 AppendNegatedColumn(FStandardForm& Form, int32 Source)
		{
			const int32 Appended = Form.NumCols;
			const int32 End = Form.ColStart[Source + 1];

			for (int32 At = Form.ColStart[Source]; At < End; ++At)
			{
				// Own locals: Add(Array[At]) can alias its storage across a grow (TRAPS).
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
		 * Seed the basis from a warm-start hint and return how many hint columns were used
		 * (PROMOTION_DESIGN §5.4). Cannot change the answer: it only picks a starting basis or adds
		 * artificials.
		 *
		 * Repairs: out-of-range columns stay cold; singular columns are swapped out by Factorise.
		 * A primal-infeasible seed (deleted joints carried force, so B^-1 b has real negatives that
		 * phase 1 would skip and Refactorise clamp) has each negative slot's column negated, making
		 * it feasible; phase 1 then drives those artificials out.
		 */
		int32 SeedWarmStartBasis(FStandardForm& Form, const FOracleBasis& Hint)
		{
			const TArray<int32> ColdBasis = Form.InitialBasis;
			TArray<int32> Seed = ColdBasis;

			const int32 Offered = FMath::Min(Hint.Columns.Num(), Form.NumRows);

			for (int32 Row = 0; Row < Offered; ++Row)
			{
				const int32 Column = Hint.Columns[Row];

				// INDEX_NONE or out of range means no hint for this row.
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
				// A non-finite value abandons the whole warm start.
				if (!FMath::IsFinite(Value))
				{
					return 0;
				}

				LargestValue = FMath::Max(LargestValue, FMath::Abs(Value));
			}

			// Below this a negative value is rounding, not a real infeasibility.
			const double NegativeTolerance = (1.0 + LargestValue) * 1.0e-9;

			for (int32 Slot = 0; Slot < Form.NumRows; ++Slot)
			{
				if (XB[Slot] < -NegativeTolerance)
				{
					Seed[Slot] = AppendNegatedColumn(Form, Seed[Slot]);
				}
			}

			Form.InitialBasis = Seed;

			// Accepted only where the seed still holds the hinted column (not repaired or negated).
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

		/** Report the final basis with the shape integers needed to interpret its column indices. */
		void ReportBasis(
			const FStandardForm& Form, const TArray<int32>& Basis, FOracleBasis& Out)
		{
			Out.Columns = Basis;
			Out.NumStructCols = Form.NumStructCols;
			Out.ArtificialStart = Form.ArtificialStart;
		}


		/**
		 * Phase 1's objective: the sum of basic artificials. Zero means the original rows are
		 * feasible. Shared by the solve and the pivot loop so the two cannot diverge.
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

		/** Tolerance for the sum above, relative to the largest basic value. */
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

		/** Maps a non-optimal phase-2 end to its refusal; anything unexpected fails closed as numerical. */
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
		 * Entering choice: candidate-list partial pricing over a rotating window. A refill prices
		 * PricingWindowCols columns and keeps the best PricingQueueDepth; later iterations re-price
		 * only the queue. The best survivor is chosen, never the first (that cost 44x the pivots).
		 *
		 * Ranked by static steepest-edge d_j / ||A_j|| (Forrest-Goldfarb): 2,016 pivots vs 6,128
		 * for raw d_j on the PricingCost wall. Candidacy stays the raw d_j < -CostTol. "Optimal"
		 * only after a full sweep finds nothing. The Bland fallback prices all columns in index
		 * order, since lowest-index of a slice would not stop cycling.
		 */
		struct FPartialPricer
		{
			struct FCandidate
			{
				int32 Col = INDEX_NONE;

				/** Reduced cost over the column's static norm. */
				double Weighted = 0.0;
			};

			/** Where the next refill starts. */
			int32 Cursor = 0;

			/** Best-weighted first, at most PricingQueueDepth deep. */
			TArray<FCandidate> Queue;

			/** Insert a priced column; callers only offer d_j < -CostTol, so never NaN. */
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

			/** The entering column, or INDEX_NONE once all of [0, AllowedCols) price non-negative. */
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

				// Re-price the queue; a stale candidate is dropped.
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

				// Queue spent: refill from the window, widening until it bites.
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
		 * Minimise the objective. Index-deterministic, so the pivot path is a pure function of input.
		 * Duals come from a fresh BTRAN each iteration. Ratio-test near-ties go to the largest pivot,
		 * then lowest index: Bland alone accepted a basis 0.98% outside the crushing envelope. Bland
		 * remains the anti-cycling fallback after a long degenerate streak.
		 *
		 * bWatchArtificialFeasibility only records the pivot where phase 1 first reads feasible;
		 * it does not change the path.
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

				// Price: y solves yT B = c_B, then d_j = c_j - y . A_j.
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

				// Pivot floor relative to the entering column (see RelativePivotTol).
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

					// Negated so a NaN coefficient is skipped.
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
						// Same step length: prefer the numerically strongest pivot.
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
					 * The cap row makes a real unbounded ray impossible, so this is numerical and
					 * refused. Reachable via dual drift (107-block abutment rung). A repair was tried
					 * and never fired; do not reinstate it without a fixture (CURRENT_STATE).
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

				// A NaN sum fails the <= test and records nothing.
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
	 * The phase-2 texts share the prefix "phase-2 simplex failed", which the sweep test matches.
	 * None is empty so an answered solve adds nothing to WhyNot.
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

		// Never empty for an unknown enumerator, which would read as "answered".
		return TEXT("the oracle refused for an unnamed reason");
	}

	// Shared with the 3D assembler so mechanism corners match the assembled contacts.
	void DeriveInPlaneAxes(const double N[3], double U[3], double V[3]);

	/**
	 * Extract the collapse mechanism from phase 1's dual when the dead loads are infeasible: the
	 * Farkas certificate (PROMOTION_DESIGN §3.3). The unscaled dual's per-block equilibrium rows
	 * are that block's virtual (u, omega), negated so a falling centroid reads VirtualUz < 0.
	 *
	 * Fails closed (§3.6) unless yb > 0, yA_j <= tol for every structural column, and at least one
	 * block moves. Returns true and fills OutMechanism iff it verifies.
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

		// Phase-1 dual: yT B = c_B, with c_B = 1 on basic artificials.
		State.ScratchSlot.SetNumUninitialized(NumRows);
		for (int32 Slot = 0; Slot < NumRows; ++Slot)
		{
			State.ScratchSlot[Slot] = State.Basis[Slot] >= Form.ArtificialStart ? 1.0 : 0.0;
		}
		State.BtranScratchSlot(State.YRow);

		// Undo row scaling to get the physical certificate.
		TArray<double> YPhys;
		YPhys.SetNumUninitialized(NumRows);
		for (int32 Row = 0; Row < NumRows; ++Row)
		{
			YPhys[Row] = State.YRow[Row] * Form.RowScaleSigned[Row];
		}

		// yb > 0 (scale-invariant; equals the phase-1 objective). Checked explicitly to fail closed.
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

		// yA_j <= tol per structural column; optimality implies it, so this catches only gross error.
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

		const bool bThreeD = Problem.Dim == EOracleDim::Dim3D;

		// L1 over all six components; the out-of-plane three are zero in 2D.
		auto BlockL1 = [](const FOracleMechanismBlock& T)
		{
			return FMath::Abs(T.VirtualUx) + FMath::Abs(T.VirtualUy) + FMath::Abs(T.VirtualUz)
				+ FMath::Abs(T.VirtualOmegaX) + FMath::Abs(T.VirtualOmega) + FMath::Abs(T.VirtualOmegaZ);
		};

		// Block triples, negated so a descending centroid reads VirtualUz < 0.
		OutMechanism.Blocks.SetNum(NumBlocks);
		double LargestBlockMagnitude = 0.0;

		for (int32 Block = 0; Block < NumBlocks; ++Block)
		{
			const int32 Fx = EqFxRowOfBlock[Block];

			if (Fx == INDEX_NONE)
			{
				// Grounded: writes no rows, so its triple is exactly zero and it never moves.
				continue;
			}

			FOracleMechanismBlock& Triple = OutMechanism.Blocks[Block];

			if (bThreeD)
			{
				// Rows (Fx, Fy, Fz, Mx, My, Mz) map to (u, omega); Y rotation stays in VirtualOmega.
				Triple.VirtualUx = -YPhys[Fx + 0];
				Triple.VirtualUy = -YPhys[Fx + 1];
				Triple.VirtualUz = -YPhys[Fx + 2];
				Triple.VirtualOmegaX = -YPhys[Fx + 3];
				Triple.VirtualOmega = -YPhys[Fx + 4];
				Triple.VirtualOmegaZ = -YPhys[Fx + 5];
			}
			else
			{
				Triple.VirtualUx = -YPhys[Fx];
				Triple.VirtualUz = -YPhys[Fx + 1];
				Triple.VirtualOmega = -YPhys[Fx + 2];
			}

			LargestBlockMagnitude = FMath::Max(LargestBlockMagnitude, BlockL1(Triple));
		}

		/*
		 * A joint gives iff its blocks have non-zero relative velocity at a contact point. Block
		 * velocities are unique; the strength-row multipliers are not (8 vs 13-16 opening joints
		 * across column orders). PROMOTION_DESIGN §3.3, §12 D7.
		 */
		auto VelocityAt = [](
			const FOracleMechanismBlock& Triple, double CentroidX, double CentroidZ,
			double PointX, double PointZ, double& OutVx, double& OutVz)
		{
			// Rigid-body velocity v = u + omega x r, in the moment convention r_x*F_z - r_z*F_x.
			OutVx = Triple.VirtualUx - Triple.VirtualOmega * (PointZ - CentroidZ);
			OutVz = Triple.VirtualUz + Triple.VirtualOmega * (PointX - CentroidX);
		};

		/*
		 * 3D v = u + omega x r. The 3D moment rows carry +(r x e), so omega here is the standard
		 * angular velocity, unlike the sign-flipped 2D scalar.
		 */
		auto VelocityAt3D = [](
			const FOracleMechanismBlock& T,
			double Cx, double Cy, double Cz, double Px, double Py, double Pz,
			double& OutVx, double& OutVy, double& OutVz)
		{
			const double Rx = Px - Cx;
			const double Ry = Py - Cy;
			const double Rz = Pz - Cz;
			OutVx = T.VirtualUx + T.VirtualOmega * Rz - T.VirtualOmegaZ * Ry;
			OutVy = T.VirtualUy + T.VirtualOmegaZ * Rx - T.VirtualOmegaX * Rz;
			OutVz = T.VirtualUz + T.VirtualOmegaX * Ry - T.VirtualOmega * Rx;
		};

		static const double CornerU[4] = { -1.0, 1.0, -1.0, 1.0 };
		static const double CornerV[4] = { -1.0, -1.0, 1.0, 1.0 };

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

			double Worst = 0.0;

			if (bThreeD)
			{
				// The patch's four corners, Centre +/- h_u*U +/- h_v*V, in the assembler's frame.
				const double Normal[3] = { J.NormalX, J.NormalY, J.NormalZ };
				double U[3];
				double V[3];
				DeriveInPlaneAxes(Normal, U, V);

				for (int32 Corner = 0; Corner < 4; ++Corner)
				{
					const double Du = CornerU[Corner] * J.HalfUCm;
					const double Dv = CornerV[Corner] * J.HalfVCm;
					const double PointX = J.CentreXCm + Du * U[0] + Dv * V[0];
					const double PointY = J.CentreYCm + Du * U[1] + Dv * V[1];
					const double PointZ = J.CentreZCm + Du * U[2] + Dv * V[2];

					double AVx, AVy, AVz, BVx, BVy, BVz;
					VelocityAt3D(TripleA, A.CentroidXCm, A.CentroidYCm, A.CentroidZCm,
						PointX, PointY, PointZ, AVx, AVy, AVz);
					VelocityAt3D(TripleB, B.CentroidXCm, B.CentroidYCm, B.CentroidZCm,
						PointX, PointY, PointZ, BVx, BVy, BVz);

					Worst = FMath::Max(Worst,
						FMath::Abs(BVx - AVx) + FMath::Abs(BVy - AVy) + FMath::Abs(BVz - AVz));
				}
			}
			else
			{
				// The two contact points sit at Centre -/+ HalfLength along the in-plane tangent.
				const double TangentX = -J.NormalZ;
				const double TangentZ = J.NormalX;

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
			}

			JointRelativeVelocity[Joint] = Worst;
			LargestJointRelativeVelocity = FMath::Max(LargestJointRelativeVelocity, Worst);
		}

		OutMechanism.JointOpensOrSlides.Init(false, NumJoints);
		int32 MovingCount = 0;

		if (LargestBlockMagnitude > 0.0)
		{
			for (int32 Block = 0; Block < NumBlocks; ++Block)
			{
				const FOracleMechanismBlock& Triple = OutMechanism.Blocks[Block];
				const double BlockMagnitude = BlockL1(Triple);

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

		// Nothing moves: fail closed.
		if (MovingCount == 0)
		{
			return false;
		}

		OutMechanism.bPresent = true;
		OutMechanism.bIsCertified = true;
		return true;
	}

	/*
	 * 3D (Dim3D) assembly: six rows per block, four contact corners per joint, fed to the same
	 * solver (THREED_DESIGN, "The architecture"). Kept separate from 2D so 2D stays bit-identical.
	 */

	/** One 3D contact: position, right-handed frame (N, U, V), tributary area, tension bond. */
	struct FThreeDContact
	{
		int32 Joint = 0;
		double Pos[3] = { 0.0, 0.0, 0.0 };
		double N[3] = { 0.0, 0.0, 0.0 };
		double U[3] = { 0.0, 0.0, 0.0 };
		double V[3] = { 0.0, 0.0, 0.0 };
		double TributaryAreaSqCm = 0.0;
		bool bCanTension = false;
	};

	/**
	 * Deterministic in-plane axes: U is the world axis least aligned with N (lowest index on a
	 * tie) projected off N; V = N x U. For N = +Z, U = +X and V = +Y.
	 */
	void DeriveInPlaneAxes(const double N[3], double U[3], double V[3])
	{
		int32 Best = 0;
		double BestAbs = FMath::Abs(N[0]);

		for (int32 Axis = 1; Axis < 3; ++Axis)
		{
			const double AxisAbs = FMath::Abs(N[Axis]);

			if (AxisAbs < BestAbs)
			{
				BestAbs = AxisAbs;
				Best = Axis;
			}
		}

		double Ref[3] = { 0.0, 0.0, 0.0 };
		Ref[Best] = 1.0;

		const double Dot = Ref[0] * N[0] + Ref[1] * N[1] + Ref[2] * N[2];
		double Raw[3] = { Ref[0] - Dot * N[0], Ref[1] - Dot * N[1], Ref[2] - Dot * N[2] };

		const double Length = FMath::Sqrt(Raw[0] * Raw[0] + Raw[1] * Raw[1] + Raw[2] * Raw[2]);

		// A NaN length takes the fallback.
		if (Length > 0.0)
		{
			U[0] = Raw[0] / Length;
			U[1] = Raw[1] / Length;
			U[2] = Raw[2] / Length;
		}
		else
		{
			U[0] = Ref[0];
			U[1] = Ref[1];
			U[2] = Ref[2];
		}

		V[0] = N[1] * U[2] - N[2] * U[1];
		V[1] = N[2] * U[0] - N[0] * U[2];
		V[2] = N[0] * U[1] - N[1] * U[0];
	}

	/**
	 * Four corners per joint at C +/- h_u*U +/- h_v*V in fixed order (THREED_DESIGN §2), each with
	 * a quarter of the area. A point patch collapses them onto the centre.
	 */
	void BuildThreeDContacts(const FOracleProblem& Problem, TArray<FThreeDContact>& Out)
	{
		const int32 NumJoints = Problem.Joints.Num();
		Out.Reserve(NumJoints * 4);

		static const double CornerU[4] = { -1.0, 1.0, -1.0, 1.0 };
		static const double CornerV[4] = { -1.0, -1.0, 1.0, 1.0 };

		for (int32 JointIndex = 0; JointIndex < NumJoints; ++JointIndex)
		{
			const FOracleJoint& Joint = Problem.Joints[JointIndex];

			const double Normal[3] = { Joint.NormalX, Joint.NormalY, Joint.NormalZ };
			double U[3];
			double V[3];
			DeriveInPlaneAxes(Normal, U, V);

			for (int32 Corner = 0; Corner < 4; ++Corner)
			{
				const double Du = CornerU[Corner] * Joint.HalfUCm;
				const double Dv = CornerV[Corner] * Joint.HalfVCm;

				FThreeDContact Contact;
				Contact.Joint = JointIndex;
				Contact.N[0] = Normal[0];
				Contact.N[1] = Normal[1];
				Contact.N[2] = Normal[2];
				Contact.U[0] = U[0];
				Contact.U[1] = U[1];
				Contact.U[2] = U[2];
				Contact.V[0] = V[0];
				Contact.V[1] = V[1];
				Contact.V[2] = V[2];
				Contact.Pos[0] = Joint.CentreXCm + Du * U[0] + Dv * V[0];
				Contact.Pos[1] = Joint.CentreYCm + Du * U[1] + Dv * V[1];
				Contact.Pos[2] = Joint.CentreZCm + Du * U[2] + Dv * V[2];
				Contact.TributaryAreaSqCm = Joint.AreaSqCm / 4.0;
				Contact.bCanTension = Joint.Strength.TensileStrengthMPa > 0.0;
				Out.Add(Contact);
			}
		}
	}

	/**
	 * One contact's coefficients in the six equilibrium rows. Columns at Base are [n+, n-, p_u,
	 * q_u, p_v, q_v]; force rows take each direction e, moment rows r x e. n- only if the joint
	 * bonds in tension.
	 */
	void AppendThreeDContactCoeffs(
		OracleDetail::FAssemblyRow& Fx, OracleDetail::FAssemblyRow& Fy, OracleDetail::FAssemblyRow& Fz,
		OracleDetail::FAssemblyRow& Mx, OracleDetail::FAssemblyRow& My, OracleDetail::FAssemblyRow& Mz,
		int32 Base, double Sign, const FThreeDContact& Contact,
		double CentroidXCm, double CentroidYCm, double CentroidZCm)
	{
		const double Rx = Contact.Pos[0] - CentroidXCm;
		const double Ry = Contact.Pos[1] - CentroidYCm;
		const double Rz = Contact.Pos[2] - CentroidZCm;

		const double MnX = Ry * Contact.N[2] - Rz * Contact.N[1];
		const double MnY = Rz * Contact.N[0] - Rx * Contact.N[2];
		const double MnZ = Rx * Contact.N[1] - Ry * Contact.N[0];
		const double MuX = Ry * Contact.U[2] - Rz * Contact.U[1];
		const double MuY = Rz * Contact.U[0] - Rx * Contact.U[2];
		const double MuZ = Rx * Contact.U[1] - Ry * Contact.U[0];
		const double MvX = Ry * Contact.V[2] - Rz * Contact.V[1];
		const double MvY = Rz * Contact.V[0] - Rx * Contact.V[2];
		const double MvZ = Rx * Contact.V[1] - Ry * Contact.V[0];

		// n+: compression.
		Fx.Add(Base + 0, Sign * Contact.N[0]);
		Fy.Add(Base + 0, Sign * Contact.N[1]);
		Fz.Add(Base + 0, Sign * Contact.N[2]);
		Mx.Add(Base + 0, Sign * MnX);
		My.Add(Base + 0, Sign * MnY);
		Mz.Add(Base + 0, Sign * MnZ);

		// n-: tension.
		if (Contact.bCanTension)
		{
			Fx.Add(Base + 1, -Sign * Contact.N[0]);
			Fy.Add(Base + 1, -Sign * Contact.N[1]);
			Fz.Add(Base + 1, -Sign * Contact.N[2]);
			Mx.Add(Base + 1, -Sign * MnX);
			My.Add(Base + 1, -Sign * MnY);
			Mz.Add(Base + 1, -Sign * MnZ);
		}

		// p_u / q_u: +/- shear along U.
		Fx.Add(Base + 2, Sign * Contact.U[0]);
		Fy.Add(Base + 2, Sign * Contact.U[1]);
		Fz.Add(Base + 2, Sign * Contact.U[2]);
		Mx.Add(Base + 2, Sign * MuX);
		My.Add(Base + 2, Sign * MuY);
		Mz.Add(Base + 2, Sign * MuZ);

		Fx.Add(Base + 3, -Sign * Contact.U[0]);
		Fy.Add(Base + 3, -Sign * Contact.U[1]);
		Fz.Add(Base + 3, -Sign * Contact.U[2]);
		Mx.Add(Base + 3, -Sign * MuX);
		My.Add(Base + 3, -Sign * MuY);
		Mz.Add(Base + 3, -Sign * MuZ);

		// p_v / q_v: +/- shear along V.
		Fx.Add(Base + 4, Sign * Contact.V[0]);
		Fy.Add(Base + 4, Sign * Contact.V[1]);
		Fz.Add(Base + 4, Sign * Contact.V[2]);
		Mx.Add(Base + 4, Sign * MvX);
		My.Add(Base + 4, Sign * MvY);
		Mz.Add(Base + 4, Sign * MvZ);

		Fx.Add(Base + 5, -Sign * Contact.V[0]);
		Fy.Add(Base + 5, -Sign * Contact.V[1]);
		Fz.Add(Base + 5, -Sign * Contact.V[2]);
		Mx.Add(Base + 5, -Sign * MvX);
		My.Add(Base + 5, -Sign * MvY);
		Mz.Add(Base + 5, -Sign * MvZ);
	}

	/**
	 * Assemble the maximise-lambda 3D LP (THREED_DESIGN §"The 3D physics"). Columns: lambda, then
	 * six per contact. Rows: six equilibrium equalities per free block, the lambda cap, per-contact
	 * tension and crushing, the inscribed friction pyramid and shear-cap octagon, and optional
	 * first-crack rows for bonded joints.
	 *
	 * OutEqFxRowOfBlock[b] is block b's Fx row (the other five follow), or INDEX_NONE if grounded;
	 * the mechanism reads its dual there.
	 */
	void AssembleThreeD(
		const FOracleProblem& Problem, TArray<OracleDetail::FAssemblyRow>& AssemblyRows,
		int32& OutNumStructCols, TArray<int32>& OutEqFxRowOfBlock)
	{
		using namespace OracleDetail;

		const int32 NumJoints = Problem.Joints.Num();
		const int32 NumContacts = NumJoints * 4;
		OutNumStructCols = 1 + 6 * NumContacts;

		TArray<FThreeDContact> Contacts;
		BuildThreeDContacts(Problem, Contacts);

		OutEqFxRowOfBlock.Init(INDEX_NONE, Problem.Blocks.Num());

		for (int32 BlockIndex = 0; BlockIndex < Problem.Blocks.Num(); ++BlockIndex)
		{
			const FOracleBlock& Block = Problem.Blocks[BlockIndex];

			if (Block.bGrounded)
			{
				continue;
			}

			OutEqFxRowOfBlock[BlockIndex] = AssemblyRows.Num();

			FAssemblyRow Fx;
			FAssemblyRow Fy;
			FAssemblyRow Fz;
			FAssemblyRow Mx;
			FAssemblyRow My;
			FAssemblyRow Mz;

			const double WeightUu = Block.MassKg * OracleGravityCmPerSecondSquared;

			// Live loads go in the lambda column, dead loads in the RHS. Gravity has no moment.
			double LiveFx = 0.0, LiveFy = 0.0, LiveFz = 0.0;
			double LiveMx = 0.0, LiveMy = 0.0, LiveMz = 0.0;
			double DeadFx = 0.0, DeadFy = 0.0, DeadFz = 0.0;
			double DeadMx = 0.0, DeadMy = 0.0, DeadMz = 0.0;

			if (Problem.bGravityIsLive || Block.bLiveGravity)
			{
				LiveFz -= WeightUu;
			}
			else
			{
				DeadFz -= WeightUu;
			}

			for (const FOracleAppliedForce& Applied : Problem.AppliedForces)
			{
				if (Applied.Block != BlockIndex)
				{
					continue;
				}

				const double Rx = Applied.AtXCm - Block.CentroidXCm;
				const double Ry = Applied.AtYCm - Block.CentroidYCm;
				const double Rz = Applied.AtZCm - Block.CentroidZCm;

				const double Fpx = Applied.ForceXUu;
				const double Fpy = Applied.ForceYUu;
				const double Fpz = Applied.ForceZUu;

				const double Tx = Ry * Fpz - Rz * Fpy;
				const double Ty = Rz * Fpx - Rx * Fpz;
				const double Tz = Rx * Fpy - Ry * Fpx;

				if (Applied.bLive)
				{
					LiveFx += Fpx;
					LiveFy += Fpy;
					LiveFz += Fpz;
					LiveMx += Tx;
					LiveMy += Ty;
					LiveMz += Tz;
				}
				else
				{
					DeadFx += Fpx;
					DeadFy += Fpy;
					DeadFz += Fpz;
					DeadMx += Tx;
					DeadMy += Ty;
					DeadMz += Tz;
				}
			}

			Fx.Add(0, LiveFx);
			Fy.Add(0, LiveFy);
			Fz.Add(0, LiveFz);
			Mx.Add(0, LiveMx);
			My.Add(0, LiveMy);
			Mz.Add(0, LiveMz);

			for (int32 ContactIndex = 0; ContactIndex < Contacts.Num(); ++ContactIndex)
			{
				const FThreeDContact& Contact = Contacts[ContactIndex];
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

				AppendThreeDContactCoeffs(
					Fx, Fy, Fz, Mx, My, Mz, 1 + 6 * ContactIndex, SignForBlock, Contact,
					Block.CentroidXCm, Block.CentroidYCm, Block.CentroidZCm);
			}

			Fx.Rhs = -DeadFx;
			Fy.Rhs = -DeadFy;
			Fz.Rhs = -DeadFz;
			Mx.Rhs = -DeadMx;
			My.Rhs = -DeadMy;
			Mz.Rhs = -DeadMz;

			AssemblyRows.Add(MoveTemp(Fx));
			AssemblyRows.Add(MoveTemp(Fy));
			AssemblyRows.Add(MoveTemp(Fz));
			AssemblyRows.Add(MoveTemp(Mx));
			AssemblyRows.Add(MoveTemp(My));
			AssemblyRows.Add(MoveTemp(Mz));
		}

		// The lambda cap.
		{
			FAssemblyRow Cap;
			Cap.Add(0, 1.0);
			Cap.Rhs = LambdaCap;
			Cap.bEquality = false;
			AssemblyRows.Add(MoveTemp(Cap));
		}

		// Strength rows per contact: tension (gated), the friction pyramid, and crushing.
		for (int32 ContactIndex = 0; ContactIndex < Contacts.Num(); ++ContactIndex)
		{
			const FThreeDContact& Contact = Contacts[ContactIndex];
			const FConnectionStrength& S = Problem.Joints[Contact.Joint].Strength;
			const int32 Base = 1 + 6 * ContactIndex;

			const double Conv = OracleForceUnitsPerMPaSqCm;
			const double AreaSqCm = Contact.TributaryAreaSqCm;

			if (Contact.bCanTension && S.TensileStrengthMPa < UncappedStrengthMPa)
			{
				FAssemblyRow Tension;
				Tension.Add(Base + 1, 1.0);
				Tension.Rhs = S.TensileStrengthMPa * Conv * AreaSqCm;
				Tension.bEquality = false;
				AssemblyRows.Add(MoveTemp(Tension));
			}

			/*
			 * Inscribed friction pyramid: per facet theta_i, cos*s_u + sin*s_v - k*mu*n <= k*c*Conv*A/4
			 * with k = cos(pi/8). With mu = c = 0 the rows pin shear to zero.
			 */
			if (S.ShearCohesionMPa < UncappedStrengthMPa)
			{
				for (int32 Facet = 0; Facet < ThreeDFrictionPyramidFacets; ++Facet)
				{
					const double Theta = Facet * (2.0 * UE_DOUBLE_PI / ThreeDFrictionPyramidFacets);
					const double CosT = FMath::Cos(Theta);
					const double SinT = FMath::Sin(Theta);

					FAssemblyRow Friction;
					Friction.Add(Base + 0, -ThreeDPyramidInscribeFactor * S.FrictionCoefficient);

					if (Contact.bCanTension)
					{
						Friction.Add(Base + 1, ThreeDPyramidInscribeFactor * S.FrictionCoefficient);
					}

					Friction.Add(Base + 2, CosT);
					Friction.Add(Base + 3, -CosT);
					Friction.Add(Base + 4, SinT);
					Friction.Add(Base + 5, -SinT);
					Friction.Rhs = ThreeDPyramidInscribeFactor * S.ShearCohesionMPa * Conv * AreaSqCm;
					Friction.bEquality = false;
					AssemblyRows.Add(MoveTemp(Friction));
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

				Crush.Rhs = S.CompressiveStrengthMPa * Conv * AreaSqCm;
				Crush.bEquality = false;
				AssemblyRows.Add(MoveTemp(Crush));
			}

			/*
			 * Shear ceiling: caps in-plane shear at f_v,max regardless of compression, on the same
			 * inscribed octagon. A NaN cap fails the gate and writes no row.
			 */
			if (S.MaxShearStrengthMPa < UncappedStrengthMPa)
			{
				for (int32 Facet = 0; Facet < ThreeDFrictionPyramidFacets; ++Facet)
				{
					const double Theta = Facet * (2.0 * UE_DOUBLE_PI / ThreeDFrictionPyramidFacets);
					const double CosT = FMath::Cos(Theta);
					const double SinT = FMath::Sin(Theta);

					FAssemblyRow Ceiling;
					Ceiling.Add(Base + 2, CosT);
					Ceiling.Add(Base + 3, -CosT);
					Ceiling.Add(Base + 4, SinT);
					Ceiling.Add(Base + 5, -SinT);
					Ceiling.Rhs = ThreeDPyramidInscribeFactor * S.MaxShearStrengthMPa * Conv * AreaSqCm;
					Ceiling.bEquality = false;
					AssemblyRows.Add(MoveTemp(Ceiling));
				}
			}
		}

		// First-crack rows: biaxial uncracked peak-fibre limit for bonded joints.
		if (Problem.bFirstCrackRows)
		{
			for (int32 JointIndex = 0; JointIndex < NumJoints; ++JointIndex)
			{
				const FOracleJoint& Joint = Problem.Joints[JointIndex];
				const double FtMPa = Joint.Strength.TensileStrengthMPa;

				// Only bonded, capped joints; a NaN strength writes no row.
				if (!(FtMPa > 0.0) || !(FtMPa < UncappedStrengthMPa))
				{
					continue;
				}

				/*
				 * Peak corner fibre: -(sum n_c) + 3*(|bend_U| + |bend_V|) <= f_t*Conv*A, where bend_U =
				 * sum CornerU_c*n_c. The absolute values become four sign combinations. Under uniaxial
				 * bending this reduces to the 2D form, so 3D is no more permissive.
				 */
				static const double CornerU[4] = { -1.0, 1.0, -1.0, 1.0 };
				static const double CornerV[4] = { -1.0, -1.0, 1.0, 1.0 };

				const double Rhs = FtMPa * OracleForceUnitsPerMPaSqCm * Joint.AreaSqCm;

				for (int32 SignU = 0; SignU < 2; ++SignU)
				{
					for (int32 SignV = 0; SignV < 2; ++SignV)
					{
						const double Su = SignU == 0 ? 1.0 : -1.0;
						const double Sv = SignV == 0 ? 1.0 : -1.0;

						FAssemblyRow FirstCrack;

						for (int32 Corner = 0; Corner < 4; ++Corner)
						{
							const int32 Base = 1 + 6 * (4 * JointIndex + Corner);
							const double NormalCoeff =
								-1.0 + 3.0 * Su * CornerU[Corner] + 3.0 * Sv * CornerV[Corner];

							FirstCrack.Add(Base + 0, NormalCoeff);
							FirstCrack.Add(Base + 1, -NormalCoeff);
						}

						FirstCrack.Rhs = Rhs;
						FirstCrack.bEquality = false;
						AssemblyRows.Add(MoveTemp(FirstCrack));
					}
				}
			}
		}
	}

	/** One attempt at the problem exactly as posed, warm start included. */
	FOracleResult SolveRigidBlockOnce(const FOracleProblem& Problem)
	{
		using namespace OracleDetail;

		FOracleResult Result;
		Result.bAnswered = false;
		Result.Lambda = 0.0;

		// Sets reason and text together so they cannot disagree.
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
		 * The only dimension branch; everything downstream is dimension-agnostic. EqFxRowOfBlock
		 * records each free block's first equilibrium row for ExtractMechanism.
		 */
		TArray<FAssemblyRow> AssemblyRows;
		int32 NumStructCols = 0;
		TArray<int32> EqFxRowOfBlock;

		if (Problem.Dim == EOracleDim::Dim3D)
		{
			AssembleThreeD(Problem, AssemblyRows, NumStructCols, EqFxRowOfBlock);
		}
		else
		{
		/*
		 * Columns: lambda, then per contact [n+, n-, p, q], n = n+ - n- (compression positive),
		 * v = p - q. Splitting n keeps large bounds out of the RHS.
		 */
		const int32 NumJoints = Problem.Joints.Num();
		const int32 NumContacts = NumJoints * 2;
		NumStructCols = 1 + 4 * NumContacts;

		struct FContact
		{
			int32 Joint = 0;
			double PosX = 0.0;
			double PosZ = 0.0;
			double TributaryAreaSqCm = 0.0;

			/*
			 * Zero tensile strength omits n- entirely rather than bounding it by a zero-RHS row;
			 * hundreds of such degenerate rows stall Bland's rule on dry stacks.
			 */
			bool bCanTension = false;
		};

		TArray<FContact> Contacts;
		Contacts.Reserve(NumContacts);

		for (int32 JointIndex = 0; JointIndex < NumJoints; ++JointIndex)
		{
			const FOracleJoint& Joint = Problem.Joints[JointIndex];

			// In-plane tangent, fixed as (-Nz, Nx); contacts at centre -/+ h along it.
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

		// Block b's Fx row (Fz, M follow), or INDEX_NONE if grounded (PROMOTION_DESIGN §3.3).
		EqFxRowOfBlock.Init(INDEX_NONE, Problem.Blocks.Num());

		for (int32 BlockIndex = 0; BlockIndex < Problem.Blocks.Num(); ++BlockIndex)
		{
			const FOracleBlock& Block = Problem.Blocks[BlockIndex];

			if (Block.bGrounded)
			{
				continue;
			}

			EqFxRowOfBlock[BlockIndex] = AssemblyRows.Num();

			FAssemblyRow RowFx;
			FAssemblyRow RowFz;
			FAssemblyRow RowM;

			// Live loads into the lambda column, dead into the RHS.
			double LiveX = 0.0, LiveZ = 0.0, LiveM = 0.0;
			double DeadX = 0.0, DeadZ = 0.0, DeadM = 0.0;

			const double WeightUu = Block.MassKg * OracleGravityCmPerSecondSquared;

			/*
			 * Gravity at the centroid, no moment. Live if globally live or this block's
			 * bLiveGravity is set (the flag can only add liveness, e.g. a live surcharge).
			 */
			if (Problem.bGravityIsLive || Block.bLiveGravity)
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

				// Torque convention throughout: r_x*F_z - r_z*F_x.
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

		// The lambda cap.
		{
			FAssemblyRow Cap;
			Cap.Add(0, 1.0);
			Cap.Rhs = LambdaCap;
			Cap.bEquality = false;
			AssemblyRows.Add(MoveTemp(Cap));
		}

		// Strength rows per contact point.
		for (int32 ContactIndex = 0; ContactIndex < Contacts.Num(); ++ContactIndex)
		{
			const FContact& Contact = Contacts[ContactIndex];
			const FConnectionStrength& S = Problem.Joints[Contact.Joint].Strength;
			const int32 Base = 1 + 4 * ContactIndex;

			const double Conv = OracleForceUnitsPerMPaSqCm;
			const double AreaSqCm = Contact.TributaryAreaSqCm;

			// Tension: n- <= f_t * Conv * A/2.
			if (Contact.bCanTension && S.TensileStrengthMPa < UncappedStrengthMPa)
			{
				FAssemblyRow Tension;
				Tension.Add(Base + 1, 1.0);
				Tension.Rhs = S.TensileStrengthMPa * Conv * AreaSqCm;
				Tension.bEquality = false;
				AssemblyRows.Add(MoveTemp(Tension));
			}

			// Coulomb: +-(p - q) - mu*(n+ - n-) <= c * Conv * A/2.
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

			// Crushing: n+ - n- <= f_c * Conv * A/2.
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

			// The truncated envelope: +-(p - q) <= f_v,max * Conv * A/2.
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

		// First-crack rows: uncracked peak-fibre limit for bonded joints.
		if (Problem.bFirstCrackRows)
		{
			for (int32 JointIndex = 0; JointIndex < NumJoints; ++JointIndex)
			{
				const FOracleJoint& Joint = Problem.Joints[JointIndex];
				const double FtMPa = Joint.Strength.TensileStrengthMPa;

				/*
				 * Only bonded joints; dry joints are unchanged and a NaN strength writes no row.
				 * Unlike 3D, uncapped f_t is not skipped (benign slack row; CURRENT_STATE).
				 */
				if (!(FtMPa > 0.0))
				{
					continue;
				}

				/*
				 * Uncracked peak fibre, -(n1+n2) + 3|n1-n2| <= f_t*A over the full face, as two rows:
				 * a third of the plastic bending capacity (PROMOTION_DESIGN Sec 4.3).
				 */
				const int32 Base1 = 1 + 4 * (2 * JointIndex);
				const int32 Base2 = 1 + 4 * (2 * JointIndex + 1);
				const double Rhs =
					FtMPa * OracleForceUnitsPerMPaSqCm * Joint.AreaSqCm;

				// n1 >= n2 branch: -(n1+n2) + 3(n1-n2) = 2*n1 - 4*n2 <= f_t*A.
				FAssemblyRow FirstCrackA;
				FirstCrackA.Add(Base1 + 0, 2.0);
				FirstCrackA.Add(Base1 + 1, -2.0);
				FirstCrackA.Add(Base2 + 0, -4.0);
				FirstCrackA.Add(Base2 + 1, 4.0);
				FirstCrackA.Rhs = Rhs;
				FirstCrackA.bEquality = false;
				AssemblyRows.Add(MoveTemp(FirstCrackA));

				// n2 >= n1 branch: -(n1+n2) + 3(n2-n1) = -4*n1 + 2*n2 <= f_t*A.
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
		} /* end of the 2D assembly branch */

		FStandardForm Form;
		BuildStandardForm(AssemblyRows, NumStructCols, Form);

		// An empty basis leaves the cold path untouched.
		if (Problem.StartingBasis.Columns.Num() > 0)
		{
			Result.WarmStartColumnsAccepted = SeedWarmStartBasis(Form, Problem.StartingBasis);
		}

		FRevisedState State;

		if (!State.Init(Form))
		{
			return Refuse(EOracleRefusal::PhaseOneFailure);
		}

		// Phase 1: drive the artificials to zero.
		int32 Iterations = 0;

		// Gravity-live problems start feasible (zero RHS); only dead loads need phase 1.
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
				// One reason for all non-optimal phase-1 ends; it cannot be unbounded.
				return Refuse(EOracleRefusal::PhaseOneFailure);
			}

			if (BasicArtificialInfeasibility(Form, State) > InfeasibilityTolerance(Form, State))
			{
				/*
				 * Dead loads admit no equilibrium: an answer, lambda* = 0. The phase-1 dual is the
				 * collapse mechanism; an unverifiable certificate refuses (PROMOTION_DESIGN §3.3, §3.6).
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
			// Set here, not defaulted, so a phase 1 that forgot to report stays INDEX_NONE.
			Result.PhaseOnePivots = 0;
			Result.PivotsToFirstFeasible = 0;
		}

		Result.SimplexIterations = Iterations;
		Result.PricingColumnScans = State.PricingColumnScans;
		Result.BlandDegenerateEntries = State.BlandDegenerateEntries;

		/*
		 * Pivot zero-valued artificials out where a real column allows; one BTRAN per row prices
		 * all candidates. Take the largest |alpha|, not the first past tolerance: a near-tolerance
		 * choice left a basis ill-conditioned enough to fail verification.
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
				// Redundant row: the artificial stays basic at zero, which is harmless.
				continue;
			}

			State.FtranColumn(Entering, State.EnteringW);

			const double Theta = State.XB[Row] / State.EnteringW[Row];
			State.ApplyPivot(Row, Entering, State.EnteringW, Theta);
		}

		// Phase 2: maximise lambda (minimise -lambda).
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
				// With the cap row a real unbounded ray is impossible; fail closed.
				return Refuse(PhaseTwoRefusalFor(PhaseTwoEnd));
			}
		}

		// Final refactorisation so verification sees the cleanest values the basis admits.
		if (!State.Refactorise())
		{
			return Refuse(EOracleRefusal::PhaseTwoNumericalFailure);
		}

		/*
		 * Verify against the original rows: a drifted basis can report optimal while 0.98% outside
		 * a crushing bound. Inadmissible fails closed.
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
	 * One min-violation sub-solve: minimise StructCost over the rows and return the structural
	 * primal. Shared by each level of the lexicographic loop. bOk false with Refusal set on failure.
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

		// Phase 1: drive the equilibrium artificials to zero.
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

			// With free violations only a floating block can be infeasible: fail closed.
			if (BasicArtificialInfeasibility(Form, State) > InfeasibilityTolerance(Form, State))
			{
				Out.Refusal = EOracleRefusal::PhaseOneFailure;
				return Out;
			}
		}

		// Pivot out zero-valued artificials, as the maximise arm does.
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

		// Phase 2: minimise the supplied structural cost.
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
	 * 3D min-violation readout, kept separate so the 2D readout stays untouched. Hard equilibrium
	 * at lambda = 1, strength rows relaxed by per-row slacks.
	 *
	 * A single min-sum solve, not 2D's lexicographic minimax: enough for the determinate tripod.
	 * The minimax for indeterminate cases is deferred (CURRENT_STATE).
	 */
	FOracleResult SolveMinViolationReadoutThreeD(const FOracleProblem& Problem)
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

		TArray<FThreeDContact> Contacts;
		BuildThreeDContacts(Problem, Contacts);

		const int32 NumContacts = Contacts.Num();
		const int32 NumForceCols = 6 * NumContacts;

		TArray<FAssemblyRow> AssemblyRows;

		for (int32 BlockIndex = 0; BlockIndex < Problem.Blocks.Num(); ++BlockIndex)
		{
			const FOracleBlock& Block = Problem.Blocks[BlockIndex];

			if (Block.bGrounded)
			{
				continue;
			}

			FAssemblyRow Fx;
			FAssemblyRow Fy;
			FAssemblyRow Fz;
			FAssemblyRow Mx;
			FAssemblyRow My;
			FAssemblyRow Mz;

			// All loads are dead at lambda = 1 and go in the RHS.
			const double WeightUu = Block.MassKg * OracleGravityCmPerSecondSquared;
			double LoadX = 0.0, LoadY = 0.0, LoadZ = 0.0;
			double LoadMx = 0.0, LoadMy = 0.0, LoadMz = 0.0;
			LoadZ -= WeightUu;

			for (const FOracleAppliedForce& Applied : Problem.AppliedForces)
			{
				if (Applied.Block != BlockIndex)
				{
					continue;
				}

				const double Rx = Applied.AtXCm - Block.CentroidXCm;
				const double Ry = Applied.AtYCm - Block.CentroidYCm;
				const double Rz = Applied.AtZCm - Block.CentroidZCm;

				const double Fpx = Applied.ForceXUu;
				const double Fpy = Applied.ForceYUu;
				const double Fpz = Applied.ForceZUu;

				LoadX += Fpx;
				LoadY += Fpy;
				LoadZ += Fpz;
				LoadMx += Ry * Fpz - Rz * Fpy;
				LoadMy += Rz * Fpx - Rx * Fpz;
				LoadMz += Rx * Fpy - Ry * Fpx;
			}

			for (int32 ContactIndex = 0; ContactIndex < NumContacts; ++ContactIndex)
			{
				const FThreeDContact& Contact = Contacts[ContactIndex];
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

				AppendThreeDContactCoeffs(
					Fx, Fy, Fz, Mx, My, Mz, 6 * ContactIndex, SignForBlock, Contact,
					Block.CentroidXCm, Block.CentroidYCm, Block.CentroidZCm);
			}

			Fx.Rhs = -LoadX;
			Fy.Rhs = -LoadY;
			Fz.Rhs = -LoadZ;
			Mx.Rhs = -LoadMx;
			My.Rhs = -LoadMy;
			Mz.Rhs = -LoadMz;

			AssemblyRows.Add(MoveTemp(Fx));
			AssemblyRows.Add(MoveTemp(Fy));
			AssemblyRows.Add(MoveTemp(Fz));
			AssemblyRows.Add(MoveTemp(Mx));
			AssemblyRows.Add(MoveTemp(My));
			AssemblyRows.Add(MoveTemp(Mz));
		}

		// Each strength row gets its own violation variable.
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

		for (int32 ContactIndex = 0; ContactIndex < NumContacts; ++ContactIndex)
		{
			const FThreeDContact& Contact = Contacts[ContactIndex];
			const FConnectionStrength& S = Problem.Joints[Contact.Joint].Strength;
			const int32 Base = 6 * ContactIndex;

			const double Conv = OracleForceUnitsPerMPaSqCm;
			const double AreaSqCm = Contact.TributaryAreaSqCm;

			if (Contact.bCanTension && S.TensileStrengthMPa < UncappedStrengthMPa)
			{
				FAssemblyRow Tension;
				Tension.Add(Base + 1, 1.0);
				AddStrengthRow(Contact.Joint, MoveTemp(Tension), S.TensileStrengthMPa * Conv * AreaSqCm);
			}

			// Same friction pyramid as the maximise assembler, so both see the same statics.
			if (S.ShearCohesionMPa < UncappedStrengthMPa)
			{
				for (int32 Facet = 0; Facet < ThreeDFrictionPyramidFacets; ++Facet)
				{
					const double Theta = Facet * (2.0 * UE_DOUBLE_PI / ThreeDFrictionPyramidFacets);
					const double CosT = FMath::Cos(Theta);
					const double SinT = FMath::Sin(Theta);

					FAssemblyRow Friction;
					Friction.Add(Base + 0, -ThreeDPyramidInscribeFactor * S.FrictionCoefficient);

					if (Contact.bCanTension)
					{
						Friction.Add(Base + 1, ThreeDPyramidInscribeFactor * S.FrictionCoefficient);
					}

					Friction.Add(Base + 2, CosT);
					Friction.Add(Base + 3, -CosT);
					Friction.Add(Base + 4, SinT);
					Friction.Add(Base + 5, -SinT);
					AddStrengthRow(
						Contact.Joint, MoveTemp(Friction),
						ThreeDPyramidInscribeFactor * S.ShearCohesionMPa * Conv * AreaSqCm);
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
		}

		const int32 NumStrengthRows = StrengthInfos.Num();
		const int32 NumStructBase = NumForceCols + NumStrengthRows;

		TArray<double> Cost;
		Cost.Init(0.0, NumStructBase);

		for (const FStrengthRowInfo& Info : StrengthInfos)
		{
			Cost[Info.ViolationCol] = 1.0;
		}

		const FSubSolve Solve = SolveMinViolationLP(AssemblyRows, NumStructBase, Cost);

		if (!Solve.bOk)
		{
			return Refuse(Solve.Refusal);
		}

		Result.SimplexIterations = Solve.Iterations;
		Result.PricingColumnScans = Solve.PricingColumnScans;
		Result.BlandDegenerateEntries = Solve.BlandDegenerateEntries;

		const TArray<double>& StructValues = Solve.StructValues;

		// Verify the primal against the original physics: equilibrium hard, strength relaxed.
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

		/*
		 * Per joint: NormalUu sums the four corners' net normals; MomentUuCm is not yet computed
		 * (left zero). Utilisation is the worst row's demand / capacity, fail-closed as in 2D.
		 */
		FOracleReadout& Readout = Result.Readout;
		Readout.Joints.SetNum(NumJoints);

		constexpr double CapacityFloorUu = 1.0e-6;
		constexpr double FailClosedUtilisation = 1.0e12;

		for (int32 JointIndex = 0; JointIndex < NumJoints; ++JointIndex)
		{
			double NormalSum = 0.0;

			for (int32 Corner = 0; Corner < 4; ++Corner)
			{
				const int32 Base = 6 * (4 * JointIndex + Corner);
				NormalSum += StructValues[Base + 0] - StructValues[Base + 1];
			}

			FOracleJointReadout& Out = Readout.Joints[JointIndex];
			Out.NormalUu = NormalSum;
			Out.MomentUuCm = 0.0;

			double Violation = 0.0;
			double Utilisation = 0.0;

			for (const FStrengthRowInfo& Info : StrengthInfos)
			{
				if (Info.Joint != JointIndex)
				{
					continue;
				}

				Violation += StructValues[Info.ViolationCol];

				double Demand = 0.0;
				const FAssemblyRow& Row = AssemblyRows[Info.RowIndex];

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
	 * Min-violation LP for the strain readout (PROMOTION_DESIGN §3.1/§3.5/§3.6). Separate from the
	 * maximise-lambda solve, whose primal only looks for forces under 1.0 and so reads falsely
	 * comfortable. Load fixed at lambda = 1, equilibrium hard, each strength row relaxed by a
	 * slack s_k >= 0; always feasible, and s_k > 0 exactly on overloaded rows.
	 *
	 * Min-sum or a single global minimax is permutation-dependent on indeterminate groups. So:
	 * lexicographic minimax. Each level finds t* = min max slack, then minimises the sum of the
	 * candidates at t* to push off any that can come down; the survivors are pinned at t*. At least
	 * one pins per level, so it terminates.
	 */
	FOracleResult SolveMinViolationReadout(const FOracleProblem& Problem)
	{
		using namespace OracleDetail;

		if (Problem.Dim == EOracleDim::Dim3D)
		{
			return SolveMinViolationReadoutThreeD(Problem);
		}

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
		 * Columns: per contact [n+, n-, p, q], then one violation per strength row, then t during a
		 * level solve. No lambda column: the load is fixed.
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

			// All loads in the RHS at lambda = 1.
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
		 * The maximise solver's strength rows, each as a_k.x - s_k <= b_k. Joint, slack column and
		 * capacity are recorded for the per-joint readout.
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
		 * The same first-crack rows as the maximise path, relaxed, so utilisation is not reported
		 * against a plastic capacity the joint is never held to. Bonded joints only
		 * (PROMOTION_DESIGN Sec 4.3); uncapped f_t is not skipped (CURRENT_STATE 0d residue c).
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

				// n1 >= n2 branch: -(n1+n2) + 3(n1-n2) = 2*n1 - 4*n2 <= f_t*A.
				FAssemblyRow FirstCrackA;
				FirstCrackA.Add(Base1 + 0, 2.0);
				FirstCrackA.Add(Base1 + 1, -2.0);
				FirstCrackA.Add(Base2 + 0, -4.0);
				FirstCrackA.Add(Base2 + 1, 4.0);
				AddStrengthRow(JointIndex, MoveTemp(FirstCrackA), Rhs);

				// n2 >= n1 branch: -(n1+n2) + 3(n2-n1) = -4*n1 + 2*n2 <= f_t*A.
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

		// Built once; each level appends only its per-slack rows (and column t at NumStructBase).
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
		 * BaseRows plus one row per slack: s_k = pinned value, else s_k - t <= 0 (level mode) or
		 * s_k <= LevelBound.
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
		 * A slack within this of t* (relative, floored at 1) is binding: above ~1e-9 solve noise,
		 * far below the two-group fixture's ~38000 uu gap. The pinned value is always t* itself.
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

			// Level solve: minimise the max free slack t.
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
			 * Shrink the at-t* set to a fixed point. One min-sum pass is indifferent to trades
			 * between partners with a fixed subtotal, so it can strand a reducible slack at t* (the
			 * Degen fixture). Re-minimising over only the slacks still at t* frees such members;
			 * what remains is stuck in every optimum. The set only shrinks, so this terminates.
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

			// Something must pin each level; if not, fail closed rather than spin.
			if (PinnedThisLevel == 0)
			{
				return Refuse(
					EOracleRefusal::VerificationFailure,
					TEXT("lexicographic minimax made no progress at a level"));
			}
		}

		// Every slack pinned: read back the canonical force system.
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

		// Verify the primal against the original physics: equilibrium hard, strength relaxed.
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
		 * Per joint: NormalUu = n1 + n2, MomentUuCm = HalfLength * (n1 - n2), ViolationUu the slack
		 * total. Utilisation is the worst row's demand / capacity, as FConnection does; degenerate
		 * cases read over, not comfortable.
		 */
		FOracleReadout& Readout = Result.Readout;
		Readout.Joints.SetNum(NumJoints);

		// Only separates a real capacity from zero.
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

				// Demand excludes the slack term.
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
					// Zero capacity: any demand, or NaN, is over.
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
	 * A refused warm-start solve is retried once cold (wall-01's mapped basis goes singular at
	 * pivot 64). The retry passes the same verification gate; the wasted pivots are counted and
	 * WarmStartColumnsAccepted reads zero.
	 */
	FOracleResult SolveRigidBlock(const FOracleProblem& Problem)
	{
		// The readout is a separate, always-cold solve.
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

		// Do not add to INDEX_NONE ("not reported").
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
