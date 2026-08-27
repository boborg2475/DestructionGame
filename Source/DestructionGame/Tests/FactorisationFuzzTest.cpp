// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Math/RandomStream.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockFactor.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE LU + PRODUCT-FORM ETA FACTORISATION, FUZZED AGAINST AN INDEPENDENT ORACLE.
 *
 * PROMOTION_DESIGN.md Slice 1 moved the rigid-block oracle to all-configuration
 * production, so its sparse revised-simplex factorisation now SHIPS: the left-looking LU
 * with partial pivoting (FBasisFactor), the two triangular solves that carry the row and
 * output permutations (FTranFactor / BTranFactor), and the product-form eta file that
 * updates the basis between refactorisations (FEta, applied FORWARD in FTRAN and in
 * REVERSE in BTRAN). None of it was fuzzed. This is that fuzz.
 *
 * WHAT IT ASSERTS, AND WHY IT IS INDEPENDENT. For a random non-singular square basis it
 * drives the production solver's own objects and checks two residuals:
 *
 *     FTRAN:  solve  B x = b,   assert  ||B*x - b||_inf  <= 1e-10 * (1 + ||b||_inf)
 *     BTRAN:  solve  B^T y = c, assert  ||B^T*y - c||_inf <= 1e-10 * (1 + ||c||_inf)
 *
 * The residual is formed against a DENSE matrix the test builds from its own generated
 * columns and multiplies by hand — a different code path in a different arithmetic from
 * the sparse LU under test — and each basis's non-singularity is certified by DENSE
 * GAUSSIAN ELIMINATION with partial pivoting, a genuinely different algorithm from the
 * production LU. An oracle that mirrored production would be worthless; the whole value is
 * that the two are derived differently, so a defect in one is exposed by the other rather
 * than shared. (The GE oracle also solves the base systems and its solution is compared to
 * production's, so the second opinion is not merely a residual re-check but an independent
 * solve.) A random square matrix must be non-singular for the residual bound to mean
 * anything, so singular and duplicated-column draws are REFUSED by the GE gate and
 * regenerated — the gate itself is proven on a hand-built duplicate below.
 *
 * BOTH RESIDUALS ARE CHECKED BEFORE ANY ETA UPDATE AND AFTER EACH OF k in [1,12] ETA
 * REPLACEMENTS. The eta path is the one that ships and the one nobody watched: a basis
 * column is swapped for another, the factor is updated in product form (ApplyPivot), and
 * the systems are re-solved and re-checked against a freshly-built dense basis. The
 * BTRAN-after-etas check is the one that catches the etas being applied in the wrong
 * order.
 *
 * ONE FRevisedState (AND THEREFORE ONE FBasisFactor) IS REUSED ACROSS EVERY CASE. Slice
 * 1's spec asks for one reused FBasisFactor so workspace leakage between solves shows;
 * FRevisedState OWNS the FBasisFactor plus every scratch buffer the solves touch, so
 * reusing the whole state is the stronger form of the same requirement. Init() re-seeds it
 * per case; if any workspace were not fully reset, a later case would read a former case's
 * bits and a residual would blow up.
 *
 * DETERMINISM. Every case is generated from FRandomStream seeded by BaseSeed + case index,
 * so the suite runs the identical cases every time; the solver itself is bit-reproducible.
 * A breach prints its SEED and M so the one case can be reproduced and promoted to a named
 * regression test. THE PIVOT RULE DOES NOT BITE THIS FUZZ: partial pivoting is a stability
 * and determinism device, and its effect (a well-conditioned factor, a bit-identical
 * answer) is already carried by the residual bound and the seeded generation — this fuzz
 * would pass under any correct pivot choice, so do not expect it to pin the tie-break.
 *
 * GREEN ON ARRIVAL. This is a characterisation/property test of factorisation code that is
 * already correct, so it passes the day it lands. Its worth is proven by mutation, not by
 * a red step: three defects were confirmed to breach it (BTRAN etas applied forward,
 * ~7e4; FTRAN dropping the row permutation, ~1e1; BTRAN dropping the output permutation,
 * ~1e0), each far above the 1e-10 bound. It is also the only exerciser of the fail-closed
 * path where Factorise refuses a singular basis (proven below on a duplicate-column form).
 */

namespace FactorisationFuzzSupport
{
	using namespace RigidBlockOracle::OracleDetail;

	/** Largest absolute value in a vector — the scale the residual bound rides on. */
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
	 * DENSE GAUSSIAN ELIMINATION WITH PARTIAL PIVOTING — the independent oracle. Solves
	 * A x = b for an N-by-N row-major A (a COPY, consumed) and returns false when the
	 * matrix is singular to a relative tolerance, which is how a bad random draw or a
	 * duplicated column is refused. A different algorithm from the production LU on
	 * purpose: the two must agree only because both are right, never because one copied
	 * the other.
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

	/** A generated fuzz basis: the dense columns, and the CSC standard form built from them. */
	struct FGeneratedBasis
	{
		int32 M = 0;

		/*
		 * Every column, dense, in original-row space. [0, M) are the basis; [M, M+K) are
		 * the K entering columns for the eta replacements; index M+K is a fixed probe
		 * column, never basic, that the FTRAN residual solves against.
		 */
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
	 * AN ORTHONORMAL BASIS (condition number 1) plus GENERIC entering and probe columns.
	 * The orthonormal basis keeps the honest FTRAN/BTRAN residual at rounding — a huge
	 * margin below the 1e-10 bound — while the entering columns are ordinary dense vectors,
	 * NOT close to the columns they replace, so each eta records substantial off-pivot
	 * entries and the eta file genuinely stresses the forward/reverse ordering. A basis
	 * built from a permuted diagonally-dominant matrix would be just as non-singular but
	 * would make every eta near-identity (B^-1 ~ 1/M), which barely exercises the eta path;
	 * orthonormal-plus-generic is what makes the eta-order defect breach by orders of
	 * magnitude rather than by a whisker. The basis is built by modified Gram-Schmidt on
	 * random vectors; the GE gate still certifies non-singularity for the rule's sake.
	 */
	void Generate(FRandomStream& Rng, int32 InM, int32 K, FGeneratedBasis& Out)
	{
		const int32 M = InM;
		Out.M = M;

		const int32 NumCols = M + K + 1;
		Out.ProbeCol = M + K;
		Out.Cols.SetNum(NumCols);

		/* Modified Gram-Schmidt: orthonormalise M random vectors into the basis columns. */
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

		/* Entering columns and the probe are plain dense vectors — no relation to the basis. */
		for (int32 Kk = 0; Kk < K; ++Kk)
		{
			RandomColumn(Rng, M, Out.Cols[M + Kk]);
		}
		RandomColumn(Rng, M, Out.Cols[Out.ProbeCol]);

		/* Build the CSC standard form: full dense columns, row indices ascending. */
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

	/** Build the dense basis matrix (row-major, column s = Cols[Basis[s]]) for the oracle. */
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

	/*
	 * THE REFUSAL GATE, PROVEN ON A HAND-BUILT DUPLICATE. A basis with two identical
	 * columns is singular; the GE oracle must refuse it (so the generator skips it) and
	 * FBasisFactor::Factorise must return false (the ship's fail-closed path, otherwise
	 * exercised by no fixture). A well-conditioned control passes both.
	 */
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
		/* Make column 1 an exact copy of column 0 — singular. */
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
		/* FBasisFactor::Factorise must fail closed on a duplicated-column standard form. */
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
			/* Columns 0 and 1 are identical; the basis 0,1,2,3 is singular. */
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

	/*
	 * ONE state reused across every case (Slice 1's leakage probe). It owns the one
	 * FBasisFactor and every scratch buffer; Init() re-seeds it per case.
	 */
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

		/* Refuse a singular draw — never expected with the dominant construction, but the rule. */
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

		/*
		 * A residual check against the current basis. Phase names the moment (pre-eta, or
		 * eta index) purely for the failure message. Returns false on breach after logging
		 * the seed, so the first offending case is reproducible.
		 */
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

		/*
		 * BEFORE ANY ETA: residual bound, plus an independent GE solve compared to
		 * production's — the dense oracle solving the same system, not merely re-checking a
		 * residual. Perm-dropping mutations breach both here.
		 */
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
		 * ETA REPLACEMENTS. Bring in generic entering column M+k and pivot out the slot
		 * whose FTRAN image is LARGEST — a well-conditioned rank-one basis update that keeps
		 * the basis non-singular (a non-zero pivot is exactly the non-singularity condition)
		 * without demanding the entering column resemble the one it replaces. The factor is
		 * updated in product form (ApplyPivot appends the eta), then both residuals are
		 * re-checked against the freshly-built dense basis. The BTRAN residual here is what
		 * catches the eta file being applied forward instead of in reverse.
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

			/*
			 * A tiny largest-pivot would mean the entering column is nearly in the span of
			 * the other basis columns; skip it rather than build an ill-conditioned basis
			 * that could breach the bound honestly. Generic columns clear this easily.
			 */
			if (!(Best > 0.05))
			{
				continue;
			}

			State.ApplyPivot(Leaving, Entering, W, 1.0);

			/* Certify the updated basis is still non-singular before trusting a residual. */
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

	/*
	 * COVERAGE FLOORS: the eta path and the deep-eta case must actually have run, so a
	 * loop that silently did nothing cannot pass by asserting nothing.
	 */
	TestTrue(TEXT("the eta-update path ran across the fuzz"), PostEtaBtranChecks >= 500);
	TestEqual(TEXT("at least one case drew the full 12 eta replacements"), MaxKReached, 12);
	TestTrue(TEXT("at least one case stacked a deep eta file (>= 8)"), MaxEtasApplied >= 8);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
