// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/BuildMode/JointInference.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Profiles/ConnectionProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Unit test for automatic joint inference — the owner-delegated ruling in
 * BUILD_MODE_PLAN.md (2026-09-03).
 *
 * Pure and world-free by design: no gravity, no solver, no ticking. The mechanism
 * under test is a material-pairing + interface-normal decision, so the assertion
 * is on the returned PROFILE'S IDENTITY, not on any load, displacement or solve.
 *
 * WHY COMPARE EVERY FIELD. FConnectionStrength has no operator==, so identity is
 * pinned by matching all five fields against the named library constant. The three
 * profiles the ruling can return are mutually distinguishable on those fields —
 *   GeneralPurposeMortar         C10 / coh0.9 / T0.7 / mu0.75 / maxS2.0
 *   GeneralPurposeMortarPerpend  C10 / coh0.2 / T0.1 / mu0.75 / maxS2.0
 *   DryStone                     C30 / coh0.0 / T0.0 / mu0.70 / maxS6.0
 * so a full-field match cannot accept a sibling profile: bed vs head differ on the
 * two bond axes, and either masonry row vs DryStone differ on compression, friction
 * and the shear ceiling. Asserting one field alone (say cohesion) would let a wrong
 * masonry/dry-stone swap through, hence all five.
 *
 * One parameterised test over a table, so adding a material pairing is data.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FJointInferenceTest,
	"DestructionGame.Core.BuildMode.JointInference",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

/*
 * NAMED NAMESPACE, named distinctly from every other one in this module — an
 * anonymous namespace is private to a TRANSLATION UNIT, not a file, and a unity
 * build merges files. See ConnectionLoadTest.cpp for the incident that established
 * this rule; the `using namespace` lives inside RunTest for the same reason.
 */
namespace JointInferenceTestSupport
{
	struct FInferenceCase
	{
		const TCHAR* Description;
		const DestructionProfiles::FMaterialProfile* FaceA;
		const DestructionProfiles::FMaterialProfile* FaceB;
		FVector InterfaceNormalUnit;
		const FConnectionStrength* Expected;
		const TCHAR* ExpectedName;
	};
}

bool FJointInferenceTest::RunTest(const FString& Parameters)
{
	using namespace JointInferenceTestSupport;
	using namespace DestructionProfiles;

	const TArray<FInferenceCase> Cases = {
		/*
		 * Both faces compression-dominant masonry, normal VERTICAL (|Z| dominant) —
		 * a bed joint. Passive bed → GeneralPurposeMortar. Both signs of Z, because
		 * which piece is "A" must not flip the answer.
		 */
		{
			TEXT("brick/brick, +Z bed joint -> GeneralPurposeMortar"),
			&ClayBrick, &ClayBrick, FVector(0.0, 0.0, 1.0),
			&GeneralPurposeMortar, TEXT("GeneralPurposeMortar")
		},
		{
			TEXT("brick/brick, -Z bed joint -> GeneralPurposeMortar"),
			&ClayBrick, &ClayBrick, FVector(0.0, 0.0, -1.0),
			&GeneralPurposeMortar, TEXT("GeneralPurposeMortar")
		},

		/*
		 * Both masonry, normal HORIZONTAL — a head joint (±X) or a corner return
		 * (±Y). The weak perpend → GeneralPurposeMortarPerpend.
		 */
		{
			TEXT("brick/brick, +X head joint -> GeneralPurposeMortarPerpend"),
			&ClayBrick, &ClayBrick, FVector(1.0, 0.0, 0.0),
			&GeneralPurposeMortarPerpend, TEXT("GeneralPurposeMortarPerpend")
		},
		{
			TEXT("brick/brick, +Y corner return -> GeneralPurposeMortarPerpend"),
			&ClayBrick, &ClayBrick, FVector(0.0, 1.0, 0.0),
			&GeneralPurposeMortarPerpend, TEXT("GeneralPurposeMortarPerpend")
		},

		/*
		 * NEGATIVE horizontal normals — regression pins. MakeInterface orients the
		 * normal by piece B, so a real head joint carries a negative normal half the
		 * time. Without these, an impl that dropped the Abs on one axis (e.g.
		 * `AbsZ >= InterfaceNormalUnit.X`) evaluates 0 >= -1 -> true, misreads a head
		 * joint at (-1,0,0) as a bed joint, and returns the 4.5x-too-strong mortar.
		 */
		{
			TEXT("brick/brick, -X head joint -> GeneralPurposeMortarPerpend"),
			&ClayBrick, &ClayBrick, FVector(-1.0, 0.0, 0.0),
			&GeneralPurposeMortarPerpend, TEXT("GeneralPurposeMortarPerpend")
		},
		{
			TEXT("brick/brick, -Y corner return -> GeneralPurposeMortarPerpend"),
			&ClayBrick, &ClayBrick, FVector(0.0, -1.0, 0.0),
			&GeneralPurposeMortarPerpend, TEXT("GeneralPurposeMortarPerpend")
		},

		/*
		 * Either face NOT compression-dominant (timber today) → DryStone, a passive
		 * compression + friction bearing carrying no tension, REGARDLESS of normal
		 * orientation and REGARDLESS of ordering. Both normals, both orderings.
		 */
		{
			TEXT("timber/brick, vertical normal -> DryStone"),
			&Timber, &ClayBrick, FVector(0.0, 0.0, 1.0),
			&DryStone, TEXT("DryStone")
		},
		{
			TEXT("brick/timber, vertical normal -> DryStone"),
			&ClayBrick, &Timber, FVector(0.0, 0.0, 1.0),
			&DryStone, TEXT("DryStone")
		},
		{
			TEXT("timber/brick, horizontal normal -> DryStone"),
			&Timber, &ClayBrick, FVector(1.0, 0.0, 0.0),
			&DryStone, TEXT("DryStone")
		},
		{
			TEXT("brick/timber, horizontal normal -> DryStone"),
			&ClayBrick, &Timber, FVector(0.0, 1.0, 0.0),
			&DryStone, TEXT("DryStone")
		},
	};

	for (const FInferenceCase& Case : Cases)
	{
		const FConnectionStrength Got = BuildMode::JointForContact(
			*Case.FaceA, *Case.FaceB, Case.InterfaceNormalUnit);
		const FConnectionStrength& Want = *Case.Expected;

		const FString Prefix = FString::Printf(
			TEXT("%s (expected %s): "), Case.Description, Case.ExpectedName);

		TestEqual(Prefix + TEXT("CompressiveStrengthMPa"),
			Got.CompressiveStrengthMPa, Want.CompressiveStrengthMPa);
		TestEqual(Prefix + TEXT("ShearCohesionMPa"),
			Got.ShearCohesionMPa, Want.ShearCohesionMPa);
		TestEqual(Prefix + TEXT("TensileStrengthMPa"),
			Got.TensileStrengthMPa, Want.TensileStrengthMPa);
		TestEqual(Prefix + TEXT("FrictionCoefficient"),
			Got.FrictionCoefficient, Want.FrictionCoefficient);
		TestEqual(Prefix + TEXT("MaxShearStrengthMPa"),
			Got.MaxShearStrengthMPa, Want.MaxShearStrengthMPa);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
