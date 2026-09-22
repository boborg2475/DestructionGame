// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Math/RandomStream.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockFactor.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Fuzz of the shipping revised-simplex factorisation (PROMOTION_DESIGN.md slice 1): the LU with
 * partial pivoting (FBasisFactor), FTRAN/BTRAN with their permutations, and the product-form eta
 * file (forward in FTRAN, reverse in BTRAN). For random non-singular bases:
 *
 *     FTRAN:  solve  B x = b,   assert  ||B*x - b||_inf  <= 1e-10 * (1 + ||b||_inf)
 *     BTRAN:  solve  B^T y = c, assert  ||B^T*y - c||_inf <= 1e-10 * (1 + ||c||_inf)
 *
 * Residuals use a dense matrix multiplied by hand, and non-singularity is certified by dense
 * Gaussian elimination, an independent algorithm that also solves the base system for comparison.
 * Both residuals are checked before any eta and after each of 1-12 eta replacements; BTRAN after
 * etas catches a wrong eta order. One FRevisedState is reused across all cases so workspace leakage
 * would show.
 *
 * Seeded per case (BaseSeed + index); a breach prints seed and M. It does not pin the pivot
 * tie-break. Green on arrival; mutation-proven: BTRAN etas forward (~7e4), FTRAN without row perm
 * (~1e1), BTRAN without output perm (~1e0). Also the only test of Factorise refusing a singular basis.
 */

namespace FactorisationFuzzSupport
{
	using namespace RigidBlockOracle::OracleDetail;

	/** Infinity norm of a vector. */
	double MaxAbs(const TArray<double>& V)
	{
		double M = 0.0;
		for (double X : V)
		{
			M = FMath::Max(M, FMath::Abs(X));
		}
		return M;
	}

	/**
	 * Dense Gaussian elimination with partial pivoting, the independent oracle. Solves A x = b for a
	 * row-major N-by-N A; returns false if A is singular to a relative tolerance.
	 */
	bool GaussSolve(TArray<double> A, int32 N, const TArray<double>& InB, TArray<double>& OutX)
	{
		TArray<double> B = InB;

		double Scale = 0.0;
		for (double V : A)
		{
			Scale = FMath::Max(Scale, FMath::Abs(V));
		}
		const double Threshold = 1.0e-12 * (1.0 + Scale);

		for (int32 Col = 0; Col < N; ++Col)
		{
			int32 Pivot = Col;
			double Best = FMath::Abs(A[Col * N + Col]);
			for (int32 Row = Col + 1; Row < N; ++Row)
			{
				const double Mag = FMath::Abs(A[Row * N + Col]);
				if (Mag > Best)
				{
					Best = Mag;
					Pivot = Row;
				}
			}

			if (Best <= Threshold)
			{
				return false;
			}

			if (Pivot != Col)
			{
				for (int32 J = 0; J < N; ++J)
				{
					Swap(A[Pivot * N + J], A[Col * N + J]);
				}
				Swap(B[Pivot], B[Col]);
			}

			const double Diag = A[Col * N + Col];
			for (int32 Row = Col + 1; Row < N; ++Row)
			{
				const double Factor = A[Row * N + Col] / Diag;
				if (Factor != 0.0)
				{
					for (int32 J = Col; J < N; ++J)
					{
						A[Row * N + J] -= Factor * A[Col * N + J];
					}
					B[Row] -= Factor * B[Col];
				}
			}
		}

		OutX.SetNumUninitialized(N);
		for (int32 Col = N - 1; Col >= 0; --Col)
		{
			double Sum = B[Col];
			for (int32 J = Col + 1; J < N; ++J)
			{
				Sum -= A[Col * N + J] * OutX[J];
			}
			OutX[Col] = Sum / A[Col * N + Col];
		}

		return true;
	}

	/** A generated fuzz basis: the dense columns and the CSC standard form built from them. */
	struct FGeneratedBasis
	{
		int32 M = 0;

		// Dense columns: [0, M) basis, [M, M+K) entering columns, M+K the FTRAN probe.
		TArray<TArray<double>> Cols;
		int32 ProbeCol = 0;

		FStandardForm Form;
	};

	/** A dense random column of M entries in [-1, 1]. */
	void RandomColumn(FRandomStream& Rng, int32 M, TArray<double>& Out)
	{
		Out.SetNumUninitialized(M);
		for (int32 R = 0; R < M; ++R)
		{
			Out[R] = double(Rng.FRandRange(-1.0f, 1.0f));
		}
	}

	/**
	 * An orthonormal basis (condition number 1, via modified Gram-Schmidt) plus generic entering and
	 * probe columns. The orthonormal basis keeps honest residuals at rounding; generic entering
	 * columns give etas large off-pivot entries, so a wrong eta order breaches by orders of magnitude.
	 * A diagonally-dominant basis would make every eta near-identity.
	 */
	void Generate(FRandomStream& Rng, int32 InM, int32 K, FGeneratedBasis& Out)
	{
		const int32 M = InM;
		Out.M = M;

		const int32 NumCols = M + K + 1;
		Out.ProbeCol = M + K;
		Out.Cols.SetNum(NumCols);

		for (int32 S = 0; S < M; ++S)
		{
			TArray<double>& V = Out.Cols[S];
			RandomColumn(Rng, M, V);

			for (int32 T = 0; T < S; ++T)
			{
				const TArray<double>& Q = Out.Cols[T];
				double Dot = 0.0;
				for (int32 R = 0; R < M; ++R)
				{
					Dot += V[R] * Q[R];
				}
				for (int32 R = 0; R < M; ++R)
				{
					V[R] -= Dot * Q[R];
				}
			}

			double Norm = 0.0;
			for (int32 R = 0; R < M; ++R)
			{
				Norm += V[R] * V[R];
			}
			Norm = FMath::Sqrt(Norm);
			for (int32 R = 0; R < M; ++R)
			{
				V[R] /= Norm;
			}
		}

		for (int32 Kk = 0; Kk < K; ++Kk)
		{
			RandomColumn(Rng, M, Out.Cols[M + Kk]);
		}
		RandomColumn(Rng, M, Out.Cols[Out.ProbeCol]);

		// CSC standard form: full dense columns, row indices ascending.
		FStandardForm& Form = Out.Form;
		Form = FStandardForm();
		Form.NumRows = M;
		Form.NumCols = NumCols;
		Form.NumStructCols = NumCols;
		Form.ArtificialStart = NumCols;

		Form.ColStart.SetNumUninitialized(NumCols + 1);
		Form.ColStart[0] = 0;
		for (int32 C = 0; C < NumCols; ++C)
		{
			Form.ColStart[C + 1] = Form.ColStart[C] + M;
		}
		Form.ColRow.SetNumUninitialized(NumCols * M);
		Form.ColVal.SetNumUninitialized(NumCols * M);
		Form.ColNorm.SetNumUninitialized(NumCols);
		for (int32 C = 0; C < NumCols; ++C)
		{
			double SumSq = 0.0;
			for (int32 R = 0; R < M; ++R)
			{
				const int32 At = Form.ColStart[C] + R;
				Form.ColRow[At] = R;
				Form.ColVal[At] = Out.Cols[C][R];
				SumSq += Out.Cols[C][R] * Out.Cols[C][R];
			}
			Form.ColNorm[C] = FMath::Sqrt(1.0 + SumSq);
		}

		Form.Rhs.Init(1.0, M);
		Form.InitialBasis.SetNumUninitialized(M);
		for (int32 S = 0; S < M; ++S)
		{
			Form.InitialBasis[S] = S;
		}
	}

	/** Dense row-major basis matrix, column s = Cols[Basis[s]]. */
	void DenseBasis(const FGeneratedBasis& G, const TArray<int32>& Basis, TArray<double>& OutRowMajor)
	{
		const int32 M = G.M;
		OutRowMajor.SetNumUninitialized(M * M);
		for (int32 S = 0; S < M; ++S)
		{
			const TArray<double>& Col = G.Cols[Basis[S]];
			for (int32 R = 0; R < M; ++R)
			{
				OutRowMajor[R * M + S] = Col[R];
			}
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FFactorisationFuzzTest,
	"DestructionGame.Oracle.RigidBlock.FactorisationFuzz",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FFactorisationFuzzTest::RunTest(const FString& Parameters)
{
	using namespace RigidBlockOracle::OracleDetail;
	using namespace FactorisationFuzzSupport;

	// The GE oracle refuses a duplicated-column basis and accepts a well-conditioned control.
	{
		const int32 N = 4;
		TArray<double> Dup;
		Dup.Init(0.0, N * N);
		for (int32 R = 0; R < N; ++R)
		{
			for (int32 C = 0; C < N; ++C)
			{
				Dup[R * N + C] = (R == C) ? 5.0 : 0.5;
			}
		}
		// Column 1 copies column 0.
		for (int32 R = 0; R < N; ++R)
		{
			Dup[R * N + 1] = Dup[R * N + 0];
		}
		TArray<double> Rhs;
		Rhs.Init(1.0, N);
		TArray<double> X;
		TestFalse(TEXT("GE oracle refuses a duplicated-column (singular) basis"),
			GaussSolve(Dup, N, Rhs, X));

		TArray<double> Good;
		Good.Init(0.0, N * N);
		for (int32 R = 0; R < N; ++R)
		{
			for (int32 C = 0; C < N; ++C)
			{
				Good[R * N + C] = (R == C) ? 5.0 : 0.5;
			}
		}
		TestTrue(TEXT("GE oracle accepts a well-conditioned basis"),
			GaussSolve(Good, N, Rhs, X));
	}

	{
		// Factorise must fail closed on a duplicated-column basis.
		FStandardForm SingularForm;
		SingularForm.NumRows = 4;
		SingularForm.NumCols = 4;
		SingularForm.NumStructCols = 4;
		SingularForm.ArtificialStart = 4;
		SingularForm.ColStart.SetNumUninitialized(5);
		for (int32 C = 0; C <= 4; ++C)
		{
			SingularForm.ColStart[C] = C * 4;
		}
		SingularForm.ColRow.SetNumUninitialized(16);
		SingularForm.ColVal.SetNumUninitialized(16);
		SingularForm.ColNorm.Init(1.0, 4);
		for (int32 C = 0; C < 4; ++C)
		{
			// Columns 0 and 1 are identical.
			const int32 SrcRow = (C == 1) ? 0 : C;
			for (int32 R = 0; R < 4; ++R)
			{
				const int32 At = C * 4 + R;
				SingularForm.ColRow[At] = R;
				SingularForm.ColVal[At] = (R == SrcRow) ? 3.0 : 0.0;
			}
		}
		SingularForm.Rhs.Init(1.0, 4);
		SingularForm.InitialBasis = { 0, 1, 2, 3 };

		FRevisedState SingularState;
		TestFalse(TEXT("Factorise fails closed on a singular (duplicated-column) basis"),
			SingularState.Init(SingularForm));
	}

	// One state reused across every case, so workspace leakage between solves would show.
	FRevisedState State;

	const int32 BaseSeed = 0x00F17E11;
	const int32 NumCases = 300;
	const double BoundK = 1.0e-10;

	int32 PostEtaBtranChecks = 0;
	int32 MaxKReached = 0;
	int32 MaxEtasApplied = 0;

	for (int32 CaseIndex = 0; CaseIndex < NumCases; ++CaseIndex)
	{
		const int32 Seed = BaseSeed + CaseIndex;
		FRandomStream Rng(Seed);

		const int32 M = Rng.RandRange(3, 40);
		const int32 K = Rng.RandRange(1, 12);
		MaxKReached = FMath::Max(MaxKReached, K);

		FGeneratedBasis G;
		Generate(Rng, M, K, G);

		// Certify the draw is non-singular (never expected otherwise).
		{
			TArray<double> Dense;
			DenseBasis(G, G.Form.InitialBasis, Dense);
			TArray<double> Rhs;
			Rhs.Init(1.0, M);
			TArray<double> Xge;
			if (!GaussSolve(Dense, M, Rhs, Xge))
			{
				AddError(FString::Printf(
					TEXT("FactorisationFuzz seed=%d M=%d: generated basis was singular — "
						 "the generator must refuse it, not test it"), Seed, M));
				return false;
			}
		}

		if (!State.Init(G.Form))
		{
			AddError(FString::Printf(
				TEXT("FactorisationFuzz seed=%d M=%d: Factorise refused a non-singular basis"),
				Seed, M));
			return false;
		}

		// Residual checks against the current basis; false on breach, after logging the seed.
		auto CheckFtran = [&](const FString& Phase) -> bool
		{
			TArray<double> X;
			State.FtranColumn(G.ProbeCol, X);

			TArray<double> Resid;
			Resid.Init(0.0, M);
			for (int32 R = 0; R < M; ++R)
			{
				double Sum = 0.0;
				for (int32 S = 0; S < M; ++S)
				{
					Sum += G.Cols[State.Basis[S]][R] * X[S];
				}
				Resid[R] = Sum - G.Cols[G.ProbeCol][R];
			}

			const double R = MaxAbs(Resid);
			const double Bound = BoundK * (1.0 + MaxAbs(G.Cols[G.ProbeCol]));
			if (!(R <= Bound))
			{
				AddError(FString::Printf(
					TEXT("FactorisationFuzz FTRAN seed=%d M=%d %s: ||B x - b|| = %.6e exceeds %.6e"),
					Seed, M, *Phase, R, Bound));
				return false;
			}
			return true;
		};

		auto CheckBtran = [&](const FString& Phase, double CScale) -> bool
		{
			TArray<double> C;
			C.SetNumUninitialized(M);
			for (int32 I = 0; I < M; ++I)
			{
				C[I] = CScale * double(Rng.FRandRange(-1.0f, 1.0f));
			}

			State.ScratchSlot = C;
			TArray<double> Y;
			State.BtranScratchSlot(Y);

			TArray<double> Resid;
			Resid.Init(0.0, M);
			for (int32 S = 0; S < M; ++S)
			{
				double Sum = 0.0;
				for (int32 R = 0; R < M; ++R)
				{
					Sum += G.Cols[State.Basis[S]][R] * Y[R];
				}
				Resid[S] = Sum - C[S];
			}

			const double R = MaxAbs(Resid);
			const double Bound = BoundK * (1.0 + MaxAbs(C));
			if (!(R <= Bound))
			{
				AddError(FString::Printf(
					TEXT("FactorisationFuzz BTRAN seed=%d M=%d %s: ||B^T y - c|| = %.6e exceeds %.6e"),
					Seed, M, *Phase, R, Bound));
				return false;
			}
			return true;
		};

		// Before any eta: residuals, plus production's FTRAN compared to an independent GE solve.
		if (!CheckFtran(TEXT("pre-eta")))
		{
			return false;
		}
		if (!CheckBtran(TEXT("pre-eta"), 1.0))
		{
			return false;
		}

		{
			TArray<double> Dense;
			DenseBasis(G, State.Basis, Dense);

			TArray<double> Xprod;
			State.FtranColumn(G.ProbeCol, Xprod);
			TArray<double> Xge;
			GaussSolve(Dense, M, G.Cols[G.ProbeCol], Xge);

			double Diff = 0.0;
			double GeScale = 0.0;
			for (int32 I = 0; I < M; ++I)
			{
				Diff = FMath::Max(Diff, FMath::Abs(Xprod[I] - Xge[I]));
				GeScale = FMath::Max(GeScale, FMath::Abs(Xge[I]));
			}
			if (!(Diff <= 1.0e-8 * (1.0 + GeScale)))
			{
				AddError(FString::Printf(
					TEXT("FactorisationFuzz seed=%d M=%d: production FTRAN disagrees with the "
						 "dense GE oracle by %.6e"), Seed, M, Diff));
				return false;
			}
		}

		/*
		 * Eta replacements: bring in column M+k, pivot out the slot with the largest FTRAN entry
		 * (keeps the basis well-conditioned), then re-check both residuals. BTRAN here catches an
		 * eta file applied in the wrong order.
		 */
		int32 AppliedEtas = 0;
		for (int32 Kk = 0; Kk < K; ++Kk)
		{
			const int32 Entering = M + Kk;

			TArray<double> W;
			State.FtranColumn(Entering, W);

			int32 Leaving = 0;
			double Best = FMath::Abs(W[0]);
			for (int32 S = 1; S < M; ++S)
			{
				if (FMath::Abs(W[S]) > Best)
				{
					Best = FMath::Abs(W[S]);
					Leaving = S;
				}
			}

			// Skip a near-dependent entering column rather than build an ill-conditioned basis.
			if (!(Best > 0.05))
			{
				continue;
			}

			State.ApplyPivot(Leaving, Entering, W, 1.0);

			// Certify the updated basis is still non-singular.
			{
				TArray<double> Dense;
				DenseBasis(G, State.Basis, Dense);
				TArray<double> Rhs;
				Rhs.Init(1.0, M);
				TArray<double> Xge;
				if (!GaussSolve(Dense, M, Rhs, Xge))
				{
					break;
				}
			}

			++AppliedEtas;
			const FString Phase = FString::Printf(TEXT("after eta %d"), AppliedEtas);
			if (!CheckFtran(Phase))
			{
				return false;
			}
			if (!CheckBtran(Phase, 1.0))
			{
				return false;
			}
			++PostEtaBtranChecks;
			MaxEtasApplied = FMath::Max(MaxEtasApplied, AppliedEtas);
		}
	}

	// Coverage floors, so a loop that silently did nothing cannot pass.
	TestTrue(TEXT("the eta-update path ran across the fuzz"), PostEtaBtranChecks >= 500);
	TestEqual(TEXT("at least one case drew the full 12 eta replacements"), MaxKReached, 12);
	TestTrue(TEXT("at least one case stacked a deep eta file (>= 8)"), MaxEtasApplied >= 8);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
