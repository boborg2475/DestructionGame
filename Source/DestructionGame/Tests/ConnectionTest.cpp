// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/Connection.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

// Named, not anonymous: a unity build merges anonymous namespaces across test files.
namespace ConnectionTestSupport
{
	using namespace DestructionProfiles;

	/**
	 * Structural concrete, the calibration baseline (DESIGN.md §4). Zero friction keeps the three axes
	 * independent. A reference, not a copy: copying would be a dynamic initialiser across TUs.
	 */
	const FConnectionStrength& ConcreteUncoupled = StructuralConcrete.Strength;

	/** A 10 cm x 10 cm interface. */
	constexpr double JointAreaSqCm = 100.0;

	/**
	 * Force, in Unreal units, that loads the given area to the given stress. Derived from SI rather
	 * than ForceUnitsPerMPaSqCm so a wrong constant fails here: 10000 uu per MPa per cm2.
	 */
	constexpr double ForceForMPa(double MPa, double AreaSqCm)
	{
		return MPa * 100.0 * 100.0 * AreaSqCm;
	}

	/** Bed joint: normal points up at the piece above, so its weight is compression. */
	const FVector BedJointNormal(0.0, 0.0, 1.0);

	/** Head joint: normal points sideways, so the same weight is pure shear. */
	const FVector HeadJointNormal(1.0, 0.0, 0.0);

	/** Built through volatile locals so the optimiser cannot fold them away. */
	double MakeNaN()
	{
		volatile double Zero = 0.0;
		return Zero / Zero;
	}

	double MakeInfinity()
	{
		volatile double Zero = 0.0;
		return 1.0 / Zero;
	}

	FConnection MakeConnection(
		const FVector& InterfaceNormal,
		double InterfaceAreaSqCm,
		const FConnectionStrength& Strength)
	{
		FConnection Connection;
		Connection.PieceA = 0;
		Connection.PieceB = 1;
		Connection.InterfaceNormal = InterfaceNormal;
		Connection.InterfaceAreaSqCm = InterfaceAreaSqCm;
		Connection.Strength = Strength;
		return Connection;
	}

	/** A double with its bit pattern, for exact-comparison messages (%f hides a one-ulp difference). */
	FString Bits(double Value)
	{
		uint64 Raw = 0;
		FMemory::Memcpy(&Raw, &Value, sizeof(Raw));
		return FString::Printf(TEXT("%.17g [%016llx]"), Value, Raw);
	}

	/*
	 * The sweep matrix shared by DegenerateInputs and UtilisationQuery. The degenerate rows matter
	 * most: diverging on a joint with no interface plane reopens DESIGN.md §2's fail-open hole.
	 */

	struct FNamedNormal { const TCHAR* Description; FVector Normal; bool bIsUsable; };
	struct FNamedArea { const TCHAR* Description; double AreaSqCm; bool bIsUsable; };
	struct FNamedForce { const TCHAR* Description; FVector Force; bool bIsWellFormed; };
	struct FNamedProfile { const TCHAR* Description; FConnectionStrength Strength; };

	TArray<FNamedNormal> MatrixNormals()
	{
		const double NaNValue = MakeNaN();

		return {
			{ TEXT("bed joint normal"), BedJointNormal, true },
			{ TEXT("head joint normal"), HeadJointNormal, true },
			{ TEXT("non-unit normal"), FVector(0.0, 0.0, 5.0), true },
			{ TEXT("zero-length normal"), FVector::ZeroVector, false },
			{ TEXT("NaN normal"), FVector(NaNValue, NaNValue, NaNValue), false },
		};
	}

	TArray<FNamedArea> MatrixAreas()
	{
		const double NaNValue = MakeNaN();

		return {
			{ TEXT("valid area"), JointAreaSqCm, true },
			{ TEXT("zero area"), 0.0, false },
			{ TEXT("negative area"), -JointAreaSqCm, false },
			{ TEXT("NaN area"), NaNValue, false },
		};
	}

	// Chaos can produce a NaN force in a pathological contact; unguarded, the joint would read intact.
	TArray<FNamedForce> MatrixForces()
	{
		const double NaNValue = MakeNaN();
		const double InfinityValue = MakeInfinity();

		return {
			{ TEXT("no force"), FVector::ZeroVector, true },
			{ TEXT("moderate downward force"), FVector(0.0, 0.0, -ForceForMPa(1.0, JointAreaSqCm)), true },
			{ TEXT("moderate upward force"), FVector(0.0, 0.0, ForceForMPa(0.05, JointAreaSqCm)), true },
			{
				TEXT("oblique force"),
				FVector(ForceForMPa(0.5, JointAreaSqCm), 0.0, -ForceForMPa(1.0, JointAreaSqCm)),
				true
			},
			{ TEXT("crushing force"), FVector(0.0, 0.0, -ForceForMPa(1000.0, JointAreaSqCm)), true },
			{ TEXT("NaN force"), FVector(0.0, 0.0, NaNValue), false },
			{ TEXT("infinite force"), FVector(0.0, 0.0, -InfinityValue), false },
		};
	}

	// Dry stone has real zero strengths, so it reaches the degenerate paths legitimately.
	TArray<FNamedProfile> MatrixProfiles()
	{
		return {
			{ TEXT("concrete (uncoupled)"), ConcreteUncoupled },
			{ TEXT("mortar"), GeneralPurposeMortar },
			{ TEXT("dry stone, zero cohesion and zero tensile strength"), DryStone },
		};
	}
}

/**
 * A connection resolves a world-space force through its own normal, area and strength and reports
 * utilisation. Paired cases vary one stored property so ignoring the normal or area cannot pass.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConnectionUtilisationTest,
	"DestructionGame.Core.Connection.Utilisation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FConnectionUtilisationTest::RunTest(const FString& Parameters)
{
	using namespace ConnectionTestSupport;

	// Read at test time, not during static initialisation.
	const double ConcreteCompressiveMPa = ConcreteUncoupled.CompressiveStrengthMPa;
	const double ConcreteShearMPa = ConcreteUncoupled.ShearCohesionMPa;
	const double ConcreteTensileMPa = ConcreteUncoupled.TensileStrengthMPa;

	struct FUtilisationCase
	{
		const TCHAR* Description;
		FVector InterfaceNormal;
		double AreaSqCm;
		FVector Force;
		double ExpectedUtilisation;
	};

	// At JointAreaSqCm, one MPa of stress is 1,000,000 uu of force.
	const TArray<FUtilisationCase> Cases = {
		{
			TEXT("an unloaded joint is at zero utilisation"),
			BedJointNormal, JointAreaSqCm,
			FVector::ZeroVector,
			0.0
		},

		// One axis at a time against its own strength; the other two are exactly zero.
		{
			TEXT("weight on a bed joint is compression, at its limit"),
			BedJointNormal, JointAreaSqCm,
			FVector(0.0, 0.0, -ForceForMPa(ConcreteCompressiveMPa, JointAreaSqCm)),
			1.0
		},
		{
			TEXT("lift on a bed joint is tension, at its limit"),
			BedJointNormal, JointAreaSqCm,
			FVector(0.0, 0.0, ForceForMPa(ConcreteTensileMPa, JointAreaSqCm)),
			1.0
		},
		{
			TEXT("weight on a head joint is shear, at its limit"),
			HeadJointNormal, JointAreaSqCm,
			FVector(0.0, 0.0, -ForceForMPa(ConcreteShearMPa, JointAreaSqCm)),
			1.0
		},

		// Same force as the head joint above, different answer: proves the stored normal is used.
		{
			TEXT("the same weight on a bed joint is comfortable compression"),
			BedJointNormal, JointAreaSqCm,
			FVector(0.0, 0.0, -ForceForMPa(ConcreteShearMPa, JointAreaSqCm)),
			ConcreteShearMPa / ConcreteCompressiveMPa
		},

		// Half the area, twice the stress: proves the stored area is used. This one gives.
		{
			TEXT("halving the interface area doubles the utilisation"),
			BedJointNormal, JointAreaSqCm / 2.0,
			FVector(0.0, 0.0, -ForceForMPa(ConcreteCompressiveMPa, JointAreaSqCm)),
			2.0
		},

		// Oblique 3 MPa each way: compression 3/30 = 0.1, shear 3/6 = 0.5, so shear governs.
		{
			TEXT("the worst axis governs an oblique load, here shear over compression"),
			BedJointNormal, JointAreaSqCm,
			FVector(
				ForceForMPa(ConcreteTensileMPa, JointAreaSqCm),
				0.0,
				-ForceForMPa(ConcreteTensileMPa, JointAreaSqCm)),
			ConcreteTensileMPa / ConcreteShearMPa
		},

		// A non-unit normal describes the same plane and must not scale the load.
		{
			TEXT("a non-unit normal is normalised, not multiplied through"),
			FVector(0.0, 0.0, 5.0), JointAreaSqCm,
			FVector(0.0, 0.0, -ForceForMPa(ConcreteCompressiveMPa, JointAreaSqCm)),
			1.0
		},
	};

	constexpr double Tolerance = 1e-9;

	for (const FUtilisationCase& Case : Cases)
	{
		FConnection Connection = MakeConnection(Case.InterfaceNormal, Case.AreaSqCm, ConcreteUncoupled);

		TestFalse(
			FString::Printf(TEXT("%s: a fresh connection must start intact"), Case.Description),
			Connection.HasGiven());

		const double Utilisation = Connection.ApplyForce(Case.Force);

		TestTrue(
			FString::Printf(TEXT("%s: expected utilisation %f, got %f"),
				Case.Description, Case.ExpectedUtilisation, Utilisation),
			FMath::IsNearlyEqual(Utilisation, Case.ExpectedUtilisation, Tolerance));

		// Above 1 the joint gives; at exactly 1 it is fully loaded but holding.
		TestTrue(
			FString::Printf(TEXT("%s: utilisation %f gave %d, expected %d"),
				Case.Description, Utilisation,
				Connection.HasGiven() ? 1 : 0,
				Case.ExpectedUtilisation > 1.0 ? 1 : 0),
			Connection.HasGiven() == (Case.ExpectedUtilisation > 1.0));
	}

	return true;
}

/**
 * Giving latches (mortar does not re-bond, and healing would make collapse non-monotonic), and a
 * given joint carries nothing, which redistribution depends on.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConnectionLatchingTest,
	"DestructionGame.Core.Connection.Latching",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FConnectionLatchingTest::RunTest(const FString& Parameters)
{
	using namespace ConnectionTestSupport;

	const double ConcreteCompressiveMPa = ConcreteUncoupled.CompressiveStrengthMPa;
	const double ConcreteShearMPa = ConcreteUncoupled.ShearCohesionMPa;

	constexpr double Tolerance = 1e-9;

	const FVector HalfLoad(0.0, 0.0, -ForceForMPa(ConcreteCompressiveMPa / 2.0, JointAreaSqCm));
	const FVector AtTheLimit(0.0, 0.0, -ForceForMPa(ConcreteCompressiveMPa, JointAreaSqCm));
	const FVector Overload(0.0, 0.0, -ForceForMPa(ConcreteCompressiveMPa * 2.0, JointAreaSqCm));

	// Pure compression throughout: a vertical load on a bed joint has no shear or tension.

	{
		FConnection Connection = MakeConnection(BedJointNormal, JointAreaSqCm, ConcreteUncoupled);

		Connection.ApplyForce(HalfLoad);
		const double Second = Connection.ApplyForce(HalfLoad);

		TestFalse(TEXT("a joint at half load has not given"), Connection.HasGiven());
		TestTrue(
			FString::Printf(TEXT("an intact joint keeps reporting its load, expected 0.5, got %f"), Second),
			FMath::IsNearlyEqual(Second, 0.5, Tolerance));
	}

	// Exactly at the limit still holds: a joint gives above 1, not at 1.
	{
		FConnection Connection = MakeConnection(BedJointNormal, JointAreaSqCm, ConcreteUncoupled);

		const double Utilisation = Connection.ApplyForce(AtTheLimit);

		TestTrue(
			FString::Printf(TEXT("at the limit utilisation should be 1.0, got %f"), Utilisation),
			FMath::IsNearlyEqual(Utilisation, 1.0, Tolerance));
		TestFalse(TEXT("a joint exactly at its limit has not given"), Connection.HasGiven());
	}

	{
		FConnection Connection = MakeConnection(BedJointNormal, JointAreaSqCm, ConcreteUncoupled);

		const double Breaking = Connection.ApplyForce(Overload);

		TestTrue(
			FString::Printf(TEXT("the breaking call reports the utilisation that broke it, expected 2.0, got %f"),
				Breaking),
			FMath::IsNearlyEqual(Breaking, 2.0, Tolerance));
		TestTrue(TEXT("a joint over its limit has given"), Connection.HasGiven());

		const double AfterRelief = Connection.ApplyForce(HalfLoad);

		TestTrue(TEXT("a given joint stays given when the load drops"), Connection.HasGiven());

		// Exactly zero, not merely smaller: redistribution sums these.
		TestTrue(
			FString::Printf(TEXT("a given joint carries nothing, expected 0.0, got %f"), AfterRelief),
			FMath::IsNearlyEqual(AfterRelief, 0.0, Tolerance));

		const double AfterUnloading = Connection.ApplyForce(FVector::ZeroVector);

		TestTrue(TEXT("a given joint stays given with no load at all"), Connection.HasGiven());
		TestTrue(
			FString::Printf(TEXT("an unloaded given joint still carries nothing, got %f"), AfterUnloading),
			FMath::IsNearlyEqual(AfterUnloading, 0.0, Tolerance));
	}

	// Latching is not compression-specific: a sheared head joint stays given too.
	{
		FConnection Connection = MakeConnection(HeadJointNormal, JointAreaSqCm, ConcreteUncoupled);

		Connection.ApplyForce(FVector(0.0, 0.0, -ForceForMPa(ConcreteShearMPa * 2.0, JointAreaSqCm)));

		TestTrue(TEXT("a joint sheared past its limit has given"), Connection.HasGiven());

		const double AfterRelief = Connection.ApplyForce(
			FVector(0.0, 0.0, -ForceForMPa(ConcreteShearMPa / 2.0, JointAreaSqCm)));

		TestTrue(TEXT("a sheared joint stays given when the load drops"), Connection.HasGiven());
		TestTrue(
			FString::Printf(TEXT("a sheared given joint carries nothing, expected 0.0, got %f"), AfterRelief),
			FMath::IsNearlyEqual(AfterRelief, 0.0, Tolerance));
	}

	// As a property: over a rising-then-decaying load history, given-ness only goes one way.
	{
		FConnection Connection = MakeConnection(BedJointNormal, JointAreaSqCm, ConcreteUncoupled);

		const TArray<double> LoadHistoryMPa = { 5.0, 15.0, 37.0, 39.0, 37.0, 15.0, 5.0, 0.0 };

		bool bWasGiven = false;
		for (const double LoadMPa : LoadHistoryMPa)
		{
			const double Utilisation =
				Connection.ApplyForce(FVector(0.0, 0.0, -ForceForMPa(LoadMPa, JointAreaSqCm)));

			const bool bIsGiven = Connection.HasGiven();

			TestFalse(
				FString::Printf(TEXT("a joint healed itself at %f MPa (utilisation %f)"), LoadMPa, Utilisation),
				bWasGiven && !bIsGiven);

			if (bWasGiven)
			{
				TestTrue(
					FString::Printf(TEXT("a given joint carried %f at %f MPa"), Utilisation, LoadMPa),
					FMath::IsNearlyEqual(Utilisation, 0.0, Tolerance));
			}

			bWasGiven = bIsGiven;
		}

		/*
		 * Only 39 MPa crosses the 38 MPa limit (mean f_cm), so the joint must end given; otherwise the
		 * loop passes on a joint that never gives.
		 */
		TestTrue(TEXT("the load history peaked past the limit, so the joint must have given"),
			Connection.HasGiven());
	}

	return true;
}

/**
 * Degenerate configuration and loads fail closed; a broken joint never reads as intact. Swept over
 * normals, areas, loads and profiles.
 *
 * ComputeUtilisation returns a huge finite number rather than NaN (NaN compares false, so it would
 * read intact). ClassifyForce answers a degenerate normal with a zero load, so the connection must
 * treat a joint with no interface plane as given. HasGiven is asserted, not the ratio, since a given
 * joint reports zero.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConnectionDegenerateInputTest,
	"DestructionGame.Core.Connection.DegenerateInputs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FConnectionDegenerateInputTest::RunTest(const FString& Parameters)
{
	using namespace ConnectionTestSupport;

	const TArray<FNamedNormal> Normals = MatrixNormals();
	const TArray<FNamedArea> Areas = MatrixAreas();
	const TArray<FNamedForce> Forces = MatrixForces();
	const TArray<FNamedProfile> Profiles = MatrixProfiles();

	constexpr double Tolerance = 1e-9;

	for (const FNamedNormal& Normal : Normals)
	{
		for (const FNamedArea& Area : Areas)
		{
			for (const FNamedProfile& Profile : Profiles)
			{
				for (const FNamedForce& Force : Forces)
				{
					const FString Context = FString::Printf(TEXT("%s / %s / %s / %s"),
						Normal.Description, Area.Description, Profile.Description, Force.Description);

					FConnection Connection = MakeConnection(Normal.Normal, Area.AreaSqCm, Profile.Strength);

					const double Utilisation = Connection.ApplyForce(Force.Force);

					TestFalse(
						FString::Printf(TEXT("%s: utilisation must never be NaN, got %f"), *Context, Utilisation),
						FMath::IsNaN(Utilisation));

					TestTrue(
						FString::Printf(TEXT("%s: utilisation must be finite, got %f"), *Context, Utilisation),
						FMath::IsFinite(Utilisation));

					// On first evaluation it has given exactly when the ratio exceeded 1.
					TestTrue(
						FString::Printf(TEXT("%s: utilisation %f gave %d, expected %d"),
							*Context, Utilisation,
							Connection.HasGiven() ? 1 : 0,
							Utilisation > 1.0 ? 1 : 0),
						Connection.HasGiven() == (Utilisation > 1.0));

					const bool bIsRealJoint = Normal.bIsUsable && Area.bIsUsable;

					if (!bIsRealJoint || !Force.bIsWellFormed)
					{
						// Fail closed: nothing downstream can detect a degenerate joint reading intact.
						TestTrue(
							FString::Printf(TEXT("%s: a degenerate joint must read as given, got utilisation %f"),
								*Context, Utilisation),
							Connection.HasGiven());
					}

					// Latching and carrying nothing hold across the whole matrix.
					const bool bGaveOnFirstCall = Connection.HasGiven();
					const double Repeated = Connection.ApplyForce(Force.Force);

					if (bGaveOnFirstCall)
					{
						TestTrue(
							FString::Printf(TEXT("%s: a given joint must stay given"), *Context),
							Connection.HasGiven());

						TestTrue(
							FString::Printf(TEXT("%s: a given joint must carry nothing, got %f"),
								*Context, Repeated),
							FMath::IsNearlyEqual(Repeated, 0.0, Tolerance));
					}
				}
			}
		}
	}

	return true;
}

/**
 * UtilisationUnder must not latch and must match the break decision. ApplyForce latches, so a
 * display colouring joints by utilisation each frame would otherwise break the wall by drawing it.
 *
 * The match is bitwise, not a tolerance: joints in Structure.CascadeFuzz settle at exactly 1.0, so one
 * ulp of drift causes spurious failures. Only bitwise equality forces ApplyForce to be
 * UtilisationUnder plus the latch rather than a second implementation. Swept over the shared matrix,
 * degenerate rows included.
 *
 * A given joint's query answers the arithmetic, ignoring the latch, so it is a pure function of its
 * inputs and matches a fresh joint.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConnectionUtilisationQueryTest,
	"DestructionGame.Core.Connection.UtilisationQuery",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FConnectionUtilisationQueryTest::RunTest(const FString& Parameters)
{
	using namespace ConnectionTestSupport;

	const TArray<FNamedNormal> Normals = MatrixNormals();
	const TArray<FNamedArea> Areas = MatrixAreas();
	const TArray<FNamedForce> Forces = MatrixForces();
	const TArray<FNamedProfile> Profiles = MatrixProfiles();

	constexpr double Tolerance = 1e-9;

	/*
	 * Each cell queries a fresh joint twice, then applies the force; all three must agree bitwise.
	 * Querying first catches a latching query, which would leave ApplyForce answering zero.
	 */
	for (const FNamedNormal& Normal : Normals)
	{
		for (const FNamedArea& Area : Areas)
		{
			for (const FNamedProfile& Profile : Profiles)
			{
				for (const FNamedForce& Force : Forces)
				{
					const FString Context = FString::Printf(TEXT("%s / %s / %s / %s"),
						Normal.Description, Area.Description, Profile.Description, Force.Description);

					FConnection Connection = MakeConnection(Normal.Normal, Area.AreaSqCm, Profile.Strength);

					const double Queried = Connection.UtilisationUnder(Force.Force);

					TestFalse(
						FString::Printf(TEXT("%s: a query must not break the joint it asked about"), *Context),
						Connection.HasGiven());

					const double QueriedAgain = Connection.UtilisationUnder(Force.Force);

					TestTrue(
						FString::Printf(TEXT("%s: repeated queries must agree exactly, got %s then %s"),
							*Context, *Bits(Queried), *Bits(QueriedAgain)),
						Queried == QueriedAgain);

					TestFalse(
						FString::Printf(TEXT("%s: a second query must not break the joint either"), *Context),
						Connection.HasGiven());

					const double Applied = Connection.ApplyForce(Force.Force);

					TestTrue(
						FString::Printf(
							TEXT("%s: the query must match ApplyForce BITWISE, query %s vs applied %s"),
							*Context, *Bits(Queried), *Bits(Applied)),
						Queried == Applied);

					// The shared answer on degenerate rows must be the failing one, not a shared zero.
					const bool bIsRealJoint = Normal.bIsUsable && Area.bIsUsable;

					if (!bIsRealJoint || !Force.bIsWellFormed)
					{
						TestTrue(
							FString::Printf(
								TEXT("%s: a degenerate joint must query as over capacity, got %s"),
								*Context, *Bits(Queried)),
							!(Queried <= 1.0));
					}

					TestFalse(
						FString::Printf(TEXT("%s: a queried utilisation must never be NaN, got %s"),
							*Context, *Bits(Queried)),
						FMath::IsNaN(Queried));

					TestTrue(
						FString::Printf(TEXT("%s: a queried utilisation must be finite, got %s"),
							*Context, *Bits(Queried)),
						FMath::IsFinite(Queried));
				}
			}
		}
	}

	const double ConcreteCompressiveMPa = ConcreteUncoupled.CompressiveStrengthMPa;

	const FVector Overload(0.0, 0.0, -ForceForMPa(ConcreteCompressiveMPa * 2.0, JointAreaSqCm));
	const FVector HalfLoad(0.0, 0.0, -ForceForMPa(ConcreteCompressiveMPa / 2.0, JointAreaSqCm));

	// Non-latching under many queries: the evidence is that the joint is still breakable afterwards.
	{
		FConnection Connection = MakeConnection(BedJointNormal, JointAreaSqCm, ConcreteUncoupled);

		constexpr int32 FrameCount = 240;

		const double FirstAnswer = Connection.UtilisationUnder(Overload);

		int32 LatchedOnQuery = INDEX_NONE;
		int32 DriftedOnQuery = INDEX_NONE;

		for (int32 Frame = 0; Frame < FrameCount; ++Frame)
		{
			const double Answer = Connection.UtilisationUnder(Overload);

			if (Connection.HasGiven() && LatchedOnQuery == INDEX_NONE)
			{
				LatchedOnQuery = Frame;
			}

			if (!(Answer == FirstAnswer) && DriftedOnQuery == INDEX_NONE)
			{
				DriftedOnQuery = Frame;
			}
		}

		TestEqual(
			FString::Printf(TEXT("a joint queried %d times must still be intact (latched on query %d)"),
				FrameCount, LatchedOnQuery),
			LatchedOnQuery, INDEX_NONE);

		TestEqual(
			FString::Printf(TEXT("every query must give the same answer (drifted on query %d)"), DriftedOnQuery),
			DriftedOnQuery, INDEX_NONE);

		TestTrue(
			FString::Printf(TEXT("the query reports the ratio that WOULD break it, expected 2.0, got %s"),
				*Bits(FirstAnswer)),
			FMath::IsNearlyEqual(FirstAnswer, 2.0, Tolerance));

		// A query that had latched would make this answer zero.
		const double Applied = Connection.ApplyForce(Overload);

		TestTrue(
			FString::Printf(TEXT("after %d queries the joint must still break, expected 2.0, got %s"),
				FrameCount, *Bits(Applied)),
			FMath::IsNearlyEqual(Applied, 2.0, Tolerance));

		TestTrue(TEXT("and it must be given afterwards, because ApplyForce is what latches"),
			Connection.HasGiven());
	}

	// On a given joint the query answers the arithmetic while ApplyForce answers zero, deliberately.
	{
		FConnection Given = MakeConnection(BedJointNormal, JointAreaSqCm, ConcreteUncoupled);
		Given.ApplyForce(Overload);

		TestTrue(TEXT("fixture precondition: the joint has given"), Given.HasGiven());

		const double Queried = Given.UtilisationUnder(HalfLoad);

		TestTrue(
			FString::Printf(TEXT("a given joint still answers the arithmetic, expected 0.5, got %s"),
				*Bits(Queried)),
			FMath::IsNearlyEqual(Queried, 0.5, Tolerance));

		// The latch is not an input: a given joint queries the same as a fresh one.
		FConnection Fresh = MakeConnection(BedJointNormal, JointAreaSqCm, ConcreteUncoupled);
		const double FreshAnswer = Fresh.ApplyForce(HalfLoad);

		TestTrue(
			FString::Printf(
				TEXT("a given joint must query the same as a fresh one, given %s vs fresh %s"),
				*Bits(Queried), *Bits(FreshAnswer)),
			Queried == FreshAnswer);

		const double Applied = Given.ApplyForce(HalfLoad);

		TestTrue(
			FString::Printf(TEXT("but ApplyForce on a given joint still carries nothing, got %s"),
				*Bits(Applied)),
			FMath::IsNearlyEqual(Applied, 0.0, Tolerance));

		TestTrue(TEXT("and querying a given joint must not un-give it"), Given.HasGiven());
	}

	// A severed joint (piece removed, never failed) answers the same way.
	{
		FConnection Severed = MakeConnection(BedJointNormal, JointAreaSqCm, ConcreteUncoupled);
		Severed.Sever();

		const double Queried = Severed.UtilisationUnder(HalfLoad);

		TestTrue(
			FString::Printf(TEXT("a severed joint still answers the arithmetic, expected 0.5, got %s"),
				*Bits(Queried)),
			FMath::IsNearlyEqual(Queried, 0.5, Tolerance));

		TestTrue(TEXT("and querying a severed joint must not restore it"), Severed.HasGiven());
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
