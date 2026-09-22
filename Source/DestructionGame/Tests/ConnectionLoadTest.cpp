// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/ConnectionLoad.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Directional force classification over a table of cases. Pure geometry: no world, solver or
 * gravity. Asserts load type and magnitude, never displacement (DESIGN.md).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConnectionLoadClassificationTest,
	"DestructionGame.Core.ConnectionLoad.Classification",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

/**
 * Uniquely named namespace, with `using namespace` inside RunTest, for unity builds. When `F` was at
 * anonymous-namespace scope it shadowed locals named `F` in Chaos/Utilities.h once the unity blobs
 * repacked, breaking the module build.
 */
namespace ConnectionLoadTestSupport
{
	struct FClassificationCase
	{
		const TCHAR* Description;
		FVector Force;
		FVector InterfaceNormal;
		double ExpectedCompression;
		double ExpectedTension;
		double ExpectedShear;
	};

	/** Newtons. Arbitrary but non-unit, so a dropped scale factor shows up. */
	constexpr double F = 1000.0;

	/** F resolved onto a 45 degree interface: F / sqrt(2) into each component. */
	const double FDiagonal = F / FMath::Sqrt(2.0);
}

bool FConnectionLoadClassificationTest::RunTest(const FString& Parameters)
{
	using namespace ConnectionLoadTestSupport;

	const TArray<FClassificationCase> Cases = {
		// DESIGN.md's two cases: same gravity, classification decided by joint orientation.
		{
			TEXT("gravity on a horizontal joint is pure compression"),
			FVector(0.0, 0.0, -F), FVector(0.0, 0.0, 1.0),
			/*Compression*/ F, /*Tension*/ 0.0, /*Shear*/ 0.0
		},
		{
			TEXT("gravity on a vertical joint is pure shear"),
			FVector(0.0, 0.0, -F), FVector(1.0, 0.0, 0.0),
			/*Compression*/ 0.0, /*Tension*/ 0.0, /*Shear*/ F
		},

		{
			TEXT("force along the normal is pure tension"),
			FVector(0.0, 0.0, F), FVector(0.0, 0.0, 1.0),
			/*Compression*/ 0.0, /*Tension*/ F, /*Shear*/ 0.0
		},
		{
			TEXT("a 45 degree force splits evenly into compression and shear"),
			FVector(F / FMath::Sqrt(2.0), 0.0, -F / FMath::Sqrt(2.0)), FVector(0.0, 0.0, 1.0),
			/*Compression*/ FDiagonal, /*Tension*/ 0.0, /*Shear*/ FDiagonal
		},
		{
			TEXT("a 45 degree force splits evenly into tension and shear"),
			FVector(F / FMath::Sqrt(2.0), 0.0, F / FMath::Sqrt(2.0)), FVector(0.0, 0.0, 1.0),
			/*Compression*/ 0.0, /*Tension*/ FDiagonal, /*Shear*/ FDiagonal
		},

		// A raw non-unit normal would scale every load by its length.
		{
			TEXT("a non-unit normal is normalised before use"),
			FVector(0.0, 0.0, -F), FVector(0.0, 0.0, 5.0),
			/*Compression*/ F, /*Tension*/ 0.0, /*Shear*/ 0.0
		},

		// Degenerate inputs return a zero load rather than NaN.
		{
			TEXT("a zero-length normal yields no load"),
			FVector(0.0, 0.0, -F), FVector::ZeroVector,
			/*Compression*/ 0.0, /*Tension*/ 0.0, /*Shear*/ 0.0
		},
		{
			TEXT("a zero force yields no load"),
			FVector::ZeroVector, FVector(0.0, 0.0, 1.0),
			/*Compression*/ 0.0, /*Tension*/ 0.0, /*Shear*/ 0.0
		},
	};

	// Loads are in newtons and around 1e3, so an absolute tolerance is fine.
	constexpr double Tolerance = 1e-6;

	for (const FClassificationCase& Case : Cases)
	{
		const FConnectionLoad Load = DestructionForce::ClassifyForce(Case.Force, Case.InterfaceNormal);

		TestTrue(
			FString::Printf(TEXT("%s: compression expected %f, got %f"),
				Case.Description, Case.ExpectedCompression, Load.Compression),
			FMath::IsNearlyEqual(Load.Compression, Case.ExpectedCompression, Tolerance));

		TestTrue(
			FString::Printf(TEXT("%s: tension expected %f, got %f"),
				Case.Description, Case.ExpectedTension, Load.Tension),
			FMath::IsNearlyEqual(Load.Tension, Case.ExpectedTension, Tolerance));

		TestTrue(
			FString::Printf(TEXT("%s: shear expected %f, got %f"),
				Case.Description, Case.ExpectedShear, Load.Shear),
			FMath::IsNearlyEqual(Load.Shear, Case.ExpectedShear, Tolerance));

		// Compression and tension are opposite signs of one axis; never both non-zero.
		TestFalse(
			FString::Printf(TEXT("%s: compression and tension are both non-zero"), Case.Description),
			Load.Compression > Tolerance && Load.Tension > Tolerance);
	}

	return true;
}

/**
 * Normal orientation convention: flipping the normal swaps compression and tension with equal
 * magnitudes and leaves shear unchanged. Checked as a property over each case.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConnectionLoadNormalOrientationTest,
	"DestructionGame.Core.ConnectionLoad.NormalOrientation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FConnectionLoadNormalOrientationTest::RunTest(const FString& Parameters)
{
	using namespace ConnectionLoadTestSupport;

	struct FOrientationCase
	{
		const TCHAR* Description;
		FVector Force;
		FVector InterfaceNormal;
	};

	const TArray<FOrientationCase> Cases = {
		{ TEXT("pure compression seen from the far side"), FVector(0.0, 0.0, -F), FVector(0.0, 0.0, 1.0) },
		{ TEXT("pure tension seen from the far side"),     FVector(0.0, 0.0,  F), FVector(0.0, 0.0, 1.0) },
		{ TEXT("pure shear is orientation-independent"),   FVector(0.0, 0.0, -F), FVector(1.0, 0.0, 0.0) },
		{ TEXT("oblique load splits and swaps"),           FVector(FDiagonal, 0.0, -FDiagonal), FVector(0.0, 0.0, 1.0) },
		{ TEXT("a non-unit normal flips the same way"),    FVector(0.0, 0.0, -F), FVector(0.0, 0.0, 5.0) },
	};

	constexpr double Tolerance = 1e-6;

	for (const FOrientationCase& Case : Cases)
	{
		const FConnectionLoad Near = DestructionForce::ClassifyForce(Case.Force, Case.InterfaceNormal);
		const FConnectionLoad Far = DestructionForce::ClassifyForce(Case.Force, -Case.InterfaceNormal);

		TestTrue(
			FString::Printf(TEXT("%s: flipped tension %f should equal original compression %f"),
				Case.Description, Far.Tension, Near.Compression),
			FMath::IsNearlyEqual(Far.Tension, Near.Compression, Tolerance));

		TestTrue(
			FString::Printf(TEXT("%s: flipped compression %f should equal original tension %f"),
				Case.Description, Far.Compression, Near.Tension),
			FMath::IsNearlyEqual(Far.Compression, Near.Tension, Tolerance));

		TestTrue(
			FString::Printf(TEXT("%s: shear %f should be unchanged by the flip, got %f"),
				Case.Description, Near.Shear, Far.Shear),
			FMath::IsNearlyEqual(Far.Shear, Near.Shear, Tolerance));

		/*
		 * Newton's third law: flipping both the normal and the force (the reaction) must classify
		 * identically. This is the invariant callers rely on; flipping the normal alone is a misuse.
		 */
		const FConnectionLoad Reaction = DestructionForce::ClassifyForce(-Case.Force, -Case.InterfaceNormal);

		TestTrue(
			FString::Printf(TEXT("%s: reaction compression %f should match %f"),
				Case.Description, Reaction.Compression, Near.Compression),
			FMath::IsNearlyEqual(Reaction.Compression, Near.Compression, Tolerance));

		TestTrue(
			FString::Printf(TEXT("%s: reaction tension %f should match %f"),
				Case.Description, Reaction.Tension, Near.Tension),
			FMath::IsNearlyEqual(Reaction.Tension, Near.Tension, Tolerance));

		TestTrue(
			FString::Printf(TEXT("%s: reaction shear %f should match %f"),
				Case.Description, Reaction.Shear, Near.Shear),
			FMath::IsNearlyEqual(Reaction.Shear, Near.Shear, Tolerance));
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
