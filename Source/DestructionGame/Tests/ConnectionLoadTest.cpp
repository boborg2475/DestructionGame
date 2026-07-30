// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/ConnectionLoad.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Unit test for directional force classification.
 *
 * Gravity is irrelevant here by design: this is pure geometry on a force vector,
 * with no world, no solver and no ticking. Per DESIGN.md these assertions are on
 * the mechanism (which load type, what magnitude), never on displacement.
 *
 * One parameterised test over a table of cases, so adding a case is data rather
 * than code.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConnectionLoadClassificationTest,
	"DestructionGame.Core.ConnectionLoad.Classification",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

namespace
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
	const TArray<FClassificationCase> Cases = {
		// The two cases DESIGN.md calls out explicitly. Same downward gravity,
		// opposite classification, decided purely by the joint's orientation.
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

		// A non-unit normal must be normalised, not used raw. Skipping this
		// scales every load by the normal's length and silently inflates strain.
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

		// Compression and tension are opposite signs of one axis; both being
		// live at once means the sign convention has broken down.
		TestFalse(
			FString::Printf(TEXT("%s: compression and tension are both non-zero"), Case.Description),
			Load.Compression > Tolerance && Load.Tension > Tolerance);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
