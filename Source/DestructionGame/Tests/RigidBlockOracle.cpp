// Copyright Epic Games, Inc. All Rights Reserved.

#include "Tests/RigidBlockOracle.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

/*
 * THE ORACLE'S IMPLEMENTATION. The formulation and every modelling decision are
 * documented in RigidBlockOracle.h; this file is the LP assembly and a small dense
 * two-phase primal simplex. Independence rules observed here: no production arithmetic
 * is called (the only production types read are the plain data structs), the unit
 * conversion is derived locally, and every pivoting tie-break is index-based over the
 * input order so results are bit-reproducible.
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
		 * A strength at or beyond this is "no cap at all" (Unbreakable's 1e12, the
		 * MaxShear default of DBL_MAX): the row is omitted rather than written with an
		 * astronomically large right-hand side that would wreck the tableau's scaling.
		 */
		constexpr double UncappedStrengthMPa = 1.0e9;

		/** One structural row before slacks: coefficients over the structural columns. */
		struct FAssemblyRow
		{
			TArray<double> Coeff;
			double Rhs = 0.0;
			bool bEquality = true;
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
		 * The simplex tableau: rows of [structural | slacks | artificials | RHS], the
		 * per-row basic column, and the maintained reduced-cost row. Everything dense —
		 * this is a validation-scale oracle, not a shipping solver.
		 */
		struct FTableau
		{
			TArray<TArray<double>> Rows;
			TArray<int32> Basis;
			TArray<double> ReducedCost;
			int32 NumCols = 0;
			int32 ArtificialStart = 0;
		};

		void Pivot(FTableau& T, int32 PivotRow, int32 PivotCol)
		{
			TArray<double>& Row = T.Rows[PivotRow];
			const double Divisor = Row[PivotCol];

			for (int32 Col = 0; Col <= T.NumCols; ++Col)
			{
				Row[Col] /= Divisor;
			}

			for (int32 Other = 0; Other < T.Rows.Num(); ++Other)
			{
				if (Other == PivotRow)
				{
					continue;
				}

				TArray<double>& OtherRow = T.Rows[Other];
				const double Factor = OtherRow[PivotCol];

				if (Factor != 0.0)
				{
					for (int32 Col = 0; Col <= T.NumCols; ++Col)
					{
						OtherRow[Col] -= Factor * Row[Col];
					}
				}
			}

			const double CostFactor = T.ReducedCost[PivotCol];

			if (CostFactor != 0.0)
			{
				for (int32 Col = 0; Col <= T.NumCols; ++Col)
				{
					T.ReducedCost[Col] -= CostFactor * Row[Col];
				}
			}

			T.Basis[PivotRow] = PivotCol;
		}

		/** Price the objective out for the current basis, from scratch. */
		void RebuildReducedCosts(FTableau& T, const TArray<double>& Cost)
		{
			T.ReducedCost.SetNumZeroed(T.NumCols + 1);

			for (int32 Col = 0; Col < T.NumCols; ++Col)
			{
				T.ReducedCost[Col] = Cost[Col];
			}

			for (int32 Row = 0; Row < T.Rows.Num(); ++Row)
			{
				const double BasicCost = Cost[T.Basis[Row]];

				if (BasicCost != 0.0)
				{
					for (int32 Col = 0; Col <= T.NumCols; ++Col)
					{
						T.ReducedCost[Col] -= BasicCost * T.Rows[Row][Col];
					}
				}
			}
		}

		enum class ESimplexEnd : uint8
		{
			Optimal,
			Unbounded,
			IterationCap,
		};

		/**
		 * Minimise the priced-out objective. Every choice below is INDEX-DETERMINISTIC
		 * — no randomness, no hashing — so the pivot path, and therefore the last bit
		 * of lambda*, is a pure function of the input arrays.
		 *
		 * The entering rule is DANTZIG (most negative reduced cost, lowest index on
		 * ties) and the ratio test breaks near-ties by the LARGEST PIVOT ELEMENT (then
		 * lowest basic index): Bland's rule alone was measured accepting a basis 0.98%
		 * outside the crushing envelope on the dry 8-course stack, because it happily
		 * pivots on near-tolerance elements, and grinding a 40-course degenerate
		 * plateau for the whole iteration budget. Bland remains as the ANTI-CYCLING
		 * FALLBACK: after a long streak of zero-length steps the entering rule drops to
		 * lowest-index, which restores the termination guarantee where it is needed.
		 *
		 * The reduced-cost row is rebuilt from scratch on a fixed cadence, because a
		 * maintained row drifts one rounding per pivot and drift is what lets a column
		 * that is truly at zero read as improving forever.
		 */
		ESimplexEnd RunSimplex(
			FTableau& T, const TArray<double>& Cost, int32 AllowedCols, int32& InOutIterations)
		{
			int32 PivotsSinceRebuild = 0;
			int32 DegenerateStreak = 0;

			while (true)
			{
				if (InOutIterations >= MaxPivots)
				{
					return ESimplexEnd::IterationCap;
				}

				if (PivotsSinceRebuild >= 64)
				{
					RebuildReducedCosts(T, Cost);
					PivotsSinceRebuild = 0;
				}

				const bool bBlandFallback = DegenerateStreak >= 500;

				int32 Entering = INDEX_NONE;
				double MostNegative = -CostTol;

				for (int32 Col = 0; Col < AllowedCols; ++Col)
				{
					if (T.ReducedCost[Col] < MostNegative)
					{
						Entering = Col;

						if (bBlandFallback)
						{
							break;
						}

						MostNegative = T.ReducedCost[Col];
					}
				}

				if (Entering == INDEX_NONE)
				{
					return ESimplexEnd::Optimal;
				}

				int32 Leaving = INDEX_NONE;
				double BestRatio = 0.0;

				for (int32 Row = 0; Row < T.Rows.Num(); ++Row)
				{
					const double Coefficient = T.Rows[Row][Entering];

					if (Coefficient <= PivotTol)
					{
						continue;
					}

					const double Ratio = T.Rows[Row][T.NumCols] / Coefficient;

					if (Leaving == INDEX_NONE)
					{
						Leaving = Row;
						BestRatio = Ratio;
						continue;
					}

					const double NearTie = 1.0e-12 * (1.0 + FMath::Abs(BestRatio));

					if (Ratio < BestRatio - NearTie)
					{
						Leaving = Row;
						BestRatio = Ratio;
					}
					else if (Ratio <= BestRatio + NearTie)
					{
						/* Same step length: prefer the numerically strongest pivot. */
						const double Incumbent = FMath::Abs(T.Rows[Leaving][Entering]);

						if (Coefficient > Incumbent
							|| (Coefficient == Incumbent && T.Basis[Row] < T.Basis[Leaving]))
						{
							Leaving = Row;
							BestRatio = FMath::Min(BestRatio, Ratio);
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

				Pivot(T, Leaving, Entering);
				++InOutIterations;
				++PivotsSinceRebuild;
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
			RowFx.Coeff.SetNumZeroed(NumStructCols);
			RowFz.Coeff.SetNumZeroed(NumStructCols);
			RowM.Coeff.SetNumZeroed(NumStructCols);

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

				RowFx.Coeff[Base + 0] += SignForBlock * Joint.NormalX;
				RowFx.Coeff[Base + 2] += SignForBlock * TangentX;
				RowFx.Coeff[Base + 3] -= SignForBlock * TangentX;

				RowFz.Coeff[Base + 0] += SignForBlock * Joint.NormalZ;
				RowFz.Coeff[Base + 2] += SignForBlock * TangentZ;
				RowFz.Coeff[Base + 3] -= SignForBlock * TangentZ;

				RowM.Coeff[Base + 0] += SignForBlock * TorquePerNormal;
				RowM.Coeff[Base + 2] += SignForBlock * TorquePerShear;
				RowM.Coeff[Base + 3] -= SignForBlock * TorquePerShear;

				if (Contact.bCanTension)
				{
					RowFx.Coeff[Base + 1] -= SignForBlock * Joint.NormalX;
					RowFz.Coeff[Base + 1] -= SignForBlock * Joint.NormalZ;
					RowM.Coeff[Base + 1] -= SignForBlock * TorquePerNormal;
				}
			}

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

			RowFx.Coeff[0] = LiveX;
			RowFz.Coeff[0] = LiveZ;
			RowM.Coeff[0] = LiveM;
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
			Cap.Coeff.SetNumZeroed(NumStructCols);
			Cap.Coeff[0] = 1.0;
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
				Tension.Coeff.SetNumZeroed(NumStructCols);
				Tension.Coeff[Base + 1] = 1.0;
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
					Friction.Coeff.SetNumZeroed(NumStructCols);
					Friction.Coeff[Base + 0] = -S.FrictionCoefficient;
					Friction.Coeff[Base + 1] = Contact.bCanTension ? S.FrictionCoefficient : 0.0;
					Friction.Coeff[Base + 2] = ShearSign;
					Friction.Coeff[Base + 3] = -ShearSign;
					Friction.Rhs = S.ShearCohesionMPa * Conv * AreaSqCm;
					Friction.bEquality = false;
					AssemblyRows.Add(MoveTemp(Friction));
				}
			}

			/* Crushing: n+ - n- <= f_c * Conv * A/2. */
			if (S.CompressiveStrengthMPa < UncappedStrengthMPa)
			{
				FAssemblyRow Crush;
				Crush.Coeff.SetNumZeroed(NumStructCols);
				Crush.Coeff[Base + 0] = 1.0;
				Crush.Coeff[Base + 1] = Contact.bCanTension ? -1.0 : 0.0;
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
					Ceiling.Coeff.SetNumZeroed(NumStructCols);
					Ceiling.Coeff[Base + 2] = ShearSign;
					Ceiling.Coeff[Base + 3] = -ShearSign;
					Ceiling.Rhs = S.MaxShearStrengthMPa * Conv * AreaSqCm;
					Ceiling.bEquality = false;
					AssemblyRows.Add(MoveTemp(Ceiling));
				}
			}
		}

		/* ---- Standard form: scale rows, add slacks, orient RHS non-negative. ---- */
		const int32 NumRows = AssemblyRows.Num();

		int32 NumSlacks = 0;

		for (const FAssemblyRow& Row : AssemblyRows)
		{
			if (!Row.bEquality)
			{
				++NumSlacks;
			}
		}

		FTableau T;
		T.ArtificialStart = NumStructCols + NumSlacks;
		T.NumCols = T.ArtificialStart + NumRows;
		T.Rows.SetNum(NumRows);
		T.Basis.SetNum(NumRows);

		int32 NextSlack = NumStructCols;

		for (int32 RowIndex = 0; RowIndex < NumRows; ++RowIndex)
		{
			const FAssemblyRow& Assembly = AssemblyRows[RowIndex];
			TArray<double>& Row = T.Rows[RowIndex];
			Row.SetNumZeroed(T.NumCols + 1);

			/*
			 * Row equilibration over the COEFFICIENTS ONLY, never the right-hand side:
			 * scaling by a large RHS (the lambda cap's 1e6) shrinks the row's real
			 * coefficients toward the pivot tolerance, and an uncapped problem then
			 * reads "unbounded" because the one row that bounds lambda has become
			 * numerically invisible. Measured before this comment was written.
			 */
			double Largest = 0.0;

			for (double Coefficient : Assembly.Coeff)
			{
				Largest = FMath::Max(Largest, FMath::Abs(Coefficient));
			}

			const double Scale = Largest > 0.0 ? 1.0 / Largest : 1.0;

			for (int32 Col = 0; Col < NumStructCols; ++Col)
			{
				Row[Col] = Assembly.Coeff[Col] * Scale;
			}

			Row[T.NumCols] = Assembly.Rhs * Scale;

			int32 SlackCol = INDEX_NONE;

			if (!Assembly.bEquality)
			{
				SlackCol = NextSlack++;
				Row[SlackCol] = 1.0;
			}

			if (Row[T.NumCols] < 0.0)
			{
				for (int32 Col = 0; Col <= T.NumCols; ++Col)
				{
					Row[Col] = -Row[Col];
				}
			}

			/* Basis: the slack where it is still +1 after orientation, else artificial. */
			if (SlackCol != INDEX_NONE && Row[SlackCol] > 0.0)
			{
				T.Basis[RowIndex] = SlackCol;
			}
			else
			{
				const int32 ArtificialCol = T.ArtificialStart + RowIndex;
				Row[ArtificialCol] = 1.0;
				T.Basis[RowIndex] = ArtificialCol;
			}
		}

		/* ---- Phase 1: drive the artificials to zero. ---------------------------- */
		TArray<double> PhaseOneCost;
		PhaseOneCost.SetNumZeroed(T.NumCols);

		for (int32 Col = T.ArtificialStart; Col < T.NumCols; ++Col)
		{
			PhaseOneCost[Col] = 1.0;
		}

		RebuildReducedCosts(T, PhaseOneCost);

		int32 Iterations = 0;
		ESimplexEnd PhaseOneEnd = RunSimplex(T, PhaseOneCost, T.NumCols, Iterations);
		Result.SimplexIterations = Iterations;

		if (PhaseOneEnd != ESimplexEnd::Optimal)
		{
			Result.WhyNot = TEXT("phase-1 simplex failed");
			return Result;
		}

		double Infeasibility = 0.0;

		for (int32 RowIndex = 0; RowIndex < NumRows; ++RowIndex)
		{
			if (T.Basis[RowIndex] >= T.ArtificialStart)
			{
				Infeasibility += T.Rows[RowIndex][T.NumCols];
			}
		}

		double LargestRhs = 0.0;

		for (int32 RowIndex = 0; RowIndex < NumRows; ++RowIndex)
		{
			LargestRhs = FMath::Max(LargestRhs, FMath::Abs(T.Rows[RowIndex][T.NumCols]));
		}

		if (Infeasibility > (1.0 + LargestRhs) * 1.0e-9 * double(NumRows))
		{
			/*
			 * The DEAD loads alone admit no equilibrium (lambda = 0 is in the feasible
			 * set of every gravity-live problem, so this is only reachable with dead
			 * loads). That is an answer, not a failure: nothing stands, lambda* = 0.
			 */
			Result.bAnswered = true;
			Result.Lambda = 0.0;
			return Result;
		}

		/* Pivot lingering zero-value artificials out where a real column allows it. */
		for (int32 RowIndex = 0; RowIndex < NumRows; ++RowIndex)
		{
			if (T.Basis[RowIndex] < T.ArtificialStart)
			{
				continue;
			}

			for (int32 Col = 0; Col < T.ArtificialStart; ++Col)
			{
				if (FMath::Abs(T.Rows[RowIndex][Col]) > PivotTol)
				{
					Pivot(T, RowIndex, Col);
					break;
				}
			}
		}

		/* ---- Phase 2: maximise lambda (minimise -lambda). ----------------------- */
		TArray<double> PhaseTwoCost;
		PhaseTwoCost.SetNumZeroed(T.NumCols);
		PhaseTwoCost[0] = -1.0;

		RebuildReducedCosts(T, PhaseTwoCost);

		const ESimplexEnd PhaseTwoEnd =
			RunSimplex(T, PhaseTwoCost, T.ArtificialStart, Iterations);
		Result.SimplexIterations = Iterations;

		if (PhaseTwoEnd != ESimplexEnd::Optimal)
		{
			/* With the cap row a real unbounded ray is impossible; fail closed. */
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

		for (int32 RowIndex = 0; RowIndex < NumRows; ++RowIndex)
		{
			if (T.Basis[RowIndex] < NumStructCols)
			{
				StructValues[T.Basis[RowIndex]] =
					FMath::Max(0.0, T.Rows[RowIndex][T.NumCols]);
			}
		}

		for (int32 RowIndex = 0; RowIndex < NumRows; ++RowIndex)
		{
			const FAssemblyRow& Assembly = AssemblyRows[RowIndex];

			double LeftHandSide = 0.0;
			double Magnitude = FMath::Abs(Assembly.Rhs);

			for (int32 Col = 0; Col < NumStructCols; ++Col)
			{
				const double Term = Assembly.Coeff[Col] * StructValues[Col];
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
