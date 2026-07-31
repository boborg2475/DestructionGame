// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/Connection.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	/**
	 * Structural concrete, the calibration baseline DESIGN.md §4 asks for, and the
	 * same figures ConnectionStrengthTest uses. Every other material is eventually
	 * a ratio of these rather than an independently invented number.
	 */
	constexpr double ConcreteCompressiveMPa = 30.0;
	constexpr double ConcreteShearMPa = 6.0;
	constexpr double ConcreteTensileMPa = 3.0;

	/**
	 * DELIBERATELY UNCOUPLED: friction coefficient is zero.
	 *
	 * With mu = 0 the Mohr-Coulomb shear capacity collapses to plain cohesion, so
	 * the three axes stay strictly independent and every expected number below can
	 * be traced to exactly one strength. Coupling is already covered by
	 * ConnectionStrength.FrictionCoupling; these tests are about the connection
	 * object composing the pipeline, not about re-testing the strength maths.
	 */
	const FConnectionStrength ConcreteUncoupled{
		ConcreteCompressiveMPa,
		ConcreteShearMPa,
		ConcreteTensileMPa,
		/*FrictionCoefficient*/ 0.0
	};

	/** Mortar with real bond and real friction — used only in the property sweep. */
	const FConnectionStrength Mortar{
		/*Compressive*/ 10.0,
		/*ShearCohesion*/ 0.2,
		/*Tensile*/ 0.1,
		/*FrictionCoefficient*/ 0.6,
		/*MaxShear*/ 1.4
	};

	/** Dry stone: no bond at all, so two of its three strengths are genuine zeroes. */
	const FConnectionStrength DryStone{
		/*Compressive*/ 30.0,
		/*ShearCohesion*/ 0.0,
		/*Tensile*/ 0.0,
		/*FrictionCoefficient*/ 0.7,
		/*MaxShear*/ 10.0
	};

	/** A 10 cm x 10 cm interface. Round, so the arithmetic stays checkable by eye. */
	constexpr double JointAreaSqCm = 100.0;

	/**
	 * Force, in Unreal units, that loads the given area to the given stress.
	 *
	 * Spelled out here rather than reusing ForceUnitsPerMPaSqCm so the test fails
	 * if that constant is wrong instead of agreeing with it.
	 * 1 N = 100 uu, 1 cm2 = 100 mm2, 1 MPa = 1 N/mm2 -> 10000 uu per MPa per cm2.
	 */
	constexpr double ForceForMPa(double MPa, double AreaSqCm)
	{
		return MPa * 100.0 * 100.0 * AreaSqCm;
	}

	/**
	 * A bed joint: horizontal interface, normal pointing up at the piece above it.
	 * The force passed in is therefore the force on the upper piece, so its weight
	 * squeezes the joint (compression) per the convention in ConnectionLoad.h.
	 */
	const FVector BedJointNormal(0.0, 0.0, 1.0);

	/**
	 * A head joint: vertical interface, normal pointing sideways at the neighbour.
	 * The same downward weight is pure shear here — which is the whole point of
	 * the connection owning its own normal.
	 */
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
}

/**
 * A connection resolves a world-space force through its OWN normal, area and
 * strength profile, and reports the resulting utilisation.
 *
 * Pure arithmetic: no world, no solver, no ticking, so gravity is irrelevant by
 * design and the assertions are on the mechanism (the ratio) rather than on
 * anything moving. The cases are chosen so each of the three stored properties
 * changes the answer on its own — same force through a different normal, same
 * force through a different area — because a connection that quietly used a unit
 * normal or a unit area would still pass a single-case test.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConnectionUtilisationTest,
	"DestructionGame.Core.Connection.Utilisation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FConnectionUtilisationTest::RunTest(const FString& Parameters)
{
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

		// One axis at a time, each against its own strength. Compression 30 MPa,
		// shear 6 MPa, tension 3 MPa; the other two axes are exactly zero in each
		// case, so there is no question which one governs.
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

		// The pair that proves the connection applies its stored normal: the SAME
		// downward force, 6 MPa worth, reads 1.0 on the head joint above and only
		// 0.2 here. A connection ignoring its normal cannot produce both.
		{
			TEXT("the same weight on a bed joint is comfortable compression"),
			BedJointNormal, JointAreaSqCm,
			FVector(0.0, 0.0, -ForceForMPa(ConcreteShearMPa, JointAreaSqCm)),
			ConcreteShearMPa / ConcreteCompressiveMPa
		},

		// And the pair that proves it applies its stored area: identical force to
		// the compression case, half the interface, twice the stress. NOTE this
		// one gives (2.0 > 1.0) — the breaking call still reports the ratio that
		// broke it, which the loop's HasGiven assertion below pins down.
		{
			TEXT("halving the interface area doubles the utilisation"),
			BedJointNormal, JointAreaSqCm / 2.0,
			FVector(0.0, 0.0, -ForceForMPa(ConcreteCompressiveMPa, JointAreaSqCm)),
			2.0
		},

		// Oblique load: 3 MPa into the face and 3 MPa across it. Compression is at
		// 3/30 = 0.1 and shear at 3/6 = 0.5, so SHEAR governs. Worked through both
		// axes deliberately — pick the force carelessly and this measures whichever
		// axis happens to be worse, not the one the case is named for.
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
 * Giving latches, and a joint that has given carries nothing.
 *
 * These two are the substance of the connection object; reporting a utilisation
 * is only composition of maths that already exists.
 *
 * Latching is not tidiness. Mortar does not re-bond, and a joint that healed
 * itself when the load dropped would make collapse non-monotonic — a wall could
 * shed a joint, redistribute, and then recover it mid-fall. Carrying nothing
 * afterwards is what phase 2 depends on: redistribution only works if a broken
 * joint transmits no load rather than continuing to report a share of it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConnectionLatchingTest,
	"DestructionGame.Core.Connection.Latching",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FConnectionLatchingTest::RunTest(const FString& Parameters)
{
	constexpr double Tolerance = 1e-9;

	const FVector HalfLoad(0.0, 0.0, -ForceForMPa(ConcreteCompressiveMPa / 2.0, JointAreaSqCm));
	const FVector AtTheLimit(0.0, 0.0, -ForceForMPa(ConcreteCompressiveMPa, JointAreaSqCm));
	const FVector Overload(0.0, 0.0, -ForceForMPa(ConcreteCompressiveMPa * 2.0, JointAreaSqCm));

	// Pure compression throughout: shear and tension are exactly zero on a bed
	// joint under vertical load, so the compression axis is unambiguously the one
	// driving every number here.

	// A joint under half its limit does not give, however many times it is asked.
	{
		FConnection Connection = MakeConnection(BedJointNormal, JointAreaSqCm, ConcreteUncoupled);

		Connection.ApplyForce(HalfLoad);
		const double Second = Connection.ApplyForce(HalfLoad);

		TestFalse(TEXT("a joint at half load has not given"), Connection.HasGiven());
		TestTrue(
			FString::Printf(TEXT("an intact joint keeps reporting its load, expected 0.5, got %f"), Second),
			FMath::IsNearlyEqual(Second, 0.5, Tolerance));
	}

	// Exactly at the limit is fully utilised but still holding: the boundary is
	// "gives above 1", not "gives at 1".
	{
		FConnection Connection = MakeConnection(BedJointNormal, JointAreaSqCm, ConcreteUncoupled);

		const double Utilisation = Connection.ApplyForce(AtTheLimit);

		TestTrue(
			FString::Printf(TEXT("at the limit utilisation should be 1.0, got %f"), Utilisation),
			FMath::IsNearlyEqual(Utilisation, 1.0, Tolerance));
		TestFalse(TEXT("a joint exactly at its limit has not given"), Connection.HasGiven());
	}

	// The core sequence: overload it, then take the load away.
	{
		FConnection Connection = MakeConnection(BedJointNormal, JointAreaSqCm, ConcreteUncoupled);

		const double Breaking = Connection.ApplyForce(Overload);

		TestTrue(
			FString::Printf(TEXT("the breaking call reports the utilisation that broke it, expected 2.0, got %f"),
				Breaking),
			FMath::IsNearlyEqual(Breaking, 2.0, Tolerance));
		TestTrue(TEXT("a joint over its limit has given"), Connection.HasGiven());

		// Point 3: the load drops back to something it could easily have carried.
		const double AfterRelief = Connection.ApplyForce(HalfLoad);

		TestTrue(TEXT("a given joint stays given when the load drops"), Connection.HasGiven());

		// Point 4: and it is out of the structure, so it carries nothing. Zero
		// exactly, not "a smaller number" — phase 2 sums these to redistribute.
		TestTrue(
			FString::Printf(TEXT("a given joint carries nothing, expected 0.0, got %f"), AfterRelief),
			FMath::IsNearlyEqual(AfterRelief, 0.0, Tolerance));

		const double AfterUnloading = Connection.ApplyForce(FVector::ZeroVector);

		TestTrue(TEXT("a given joint stays given with no load at all"), Connection.HasGiven());
		TestTrue(
			FString::Printf(TEXT("an unloaded given joint still carries nothing, got %f"), AfterUnloading),
			FMath::IsNearlyEqual(AfterUnloading, 0.0, Tolerance));
	}

	// Latching is not compression-specific. A head joint sheared apart must stay
	// apart too, or the latch lives on the wrong axis.
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

	// Stated as a property rather than a sequence of examples: given-ness only
	// ever goes one way. This is the invariant that makes collapse monotonic, so
	// it is asserted over a load history that rises and then decays away.
	{
		FConnection Connection = MakeConnection(BedJointNormal, JointAreaSqCm, ConcreteUncoupled);

		const TArray<double> LoadHistoryMPa = { 5.0, 15.0, 29.0, 31.0, 29.0, 15.0, 5.0, 0.0 };

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

		// 31 MPa against a 30 MPa limit is the only entry that crosses, so the
		// history must end broken. Without this the loop above would be satisfied
		// by a connection that never gives at all.
		TestTrue(TEXT("the load history peaked past the limit, so the joint must have given"),
			Connection.HasGiven());
	}

	return true;
}

/**
 * Degenerate configuration and degenerate loads must fail closed, and a broken
 * joint must never read as an intact one.
 *
 * A property test, not an example test: it sweeps normals, areas, loads and
 * profiles and asserts invariants across all of them. The particular numbers are
 * covered above.
 *
 * TWO TRAPS THIS EXISTS TO KEEP SHUT.
 *
 * ComputeUtilisation already guards area and stress, returning a huge finite
 * number rather than a NaN, because NaN compares false against everything and a
 * joint that returned one would report itself INTACT — a structure quietly
 * refusing to collapse is far harder to diagnose than one falling apart. The
 * connection must route through those guards rather than around them.
 *
 * The normal is a NEW hole that only exists once the two halves are composed.
 * ClassifyForce answers a degenerate normal with a zero load, which is the right
 * answer in isolation but becomes "utilisation 0.0, perfectly fine" here — a
 * joint with no interface plane reading as the healthiest in the structure. It
 * has no interface, so it is not a joint, so it must read as given.
 *
 * Note the direction of the given-ness assertions: a joint that HAS given
 * reports zero utilisation, so "not broken" can never be inferred from the ratio
 * alone. HasGiven is the authoritative state and is what gets asserted.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConnectionDegenerateInputTest,
	"DestructionGame.Core.Connection.DegenerateInputs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FConnectionDegenerateInputTest::RunTest(const FString& Parameters)
{
	struct FNamedNormal { const TCHAR* Description; FVector Normal; bool bIsUsable; };
	struct FNamedArea { const TCHAR* Description; double AreaSqCm; bool bIsUsable; };
	struct FNamedForce { const TCHAR* Description; FVector Force; bool bIsWellFormed; };
	struct FNamedProfile { const TCHAR* Description; FConnectionStrength Strength; };

	const double NaNValue = MakeNaN();
	const double InfinityValue = MakeInfinity();

	const TArray<FNamedNormal> Normals = {
		{ TEXT("bed joint normal"), BedJointNormal, true },
		{ TEXT("head joint normal"), HeadJointNormal, true },
		{ TEXT("non-unit normal"), FVector(0.0, 0.0, 5.0), true },
		{ TEXT("zero-length normal"), FVector::ZeroVector, false },
		{ TEXT("NaN normal"), FVector(NaNValue, NaNValue, NaNValue), false },
	};

	const TArray<FNamedArea> Areas = {
		{ TEXT("valid area"), JointAreaSqCm, true },
		{ TEXT("zero area"), 0.0, false },
		{ TEXT("negative area"), -JointAreaSqCm, false },
		{ TEXT("NaN area"), NaNValue, false },
	};

	// Chaos can produce a NaN velocity in a pathological contact, which arrives
	// here as a NaN force. Unguarded it poisons the result and the joint reports
	// itself intact during a physics blowup — the one moment it should certainly
	// be giving.
	const TArray<FNamedForce> Forces = {
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

	// Dry stone matters here: real zeroes in two of its three strengths, so it
	// reaches the degenerate paths without anything being misconfigured.
	const TArray<FNamedProfile> Profiles = {
		{ TEXT("concrete (uncoupled)"), ConcreteUncoupled },
		{ TEXT("mortar"), Mortar },
		{ TEXT("dry stone, zero cohesion and zero tensile strength"), DryStone },
	};

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

					// On a joint's first evaluation the two answers must agree:
					// it has given exactly when the reported ratio exceeded 1.
					TestTrue(
						FString::Printf(TEXT("%s: utilisation %f gave %d, expected %d"),
							*Context, Utilisation,
							Connection.HasGiven() ? 1 : 0,
							Utilisation > 1.0 ? 1 : 0),
						Connection.HasGiven() == (Utilisation > 1.0));

					const bool bIsRealJoint = Normal.bIsUsable && Area.bIsUsable;

					if (!bIsRealJoint || !Force.bIsWellFormed)
					{
						// Fail closed. A joint with no interface plane, no area, or
						// one handed a load nobody can make sense of, must not read
						// as intact — that is the single answer that must never
						// come back, because nothing downstream can detect it.
						TestTrue(
							FString::Printf(TEXT("%s: a degenerate joint must read as given, got utilisation %f"),
								*Context, Utilisation),
							Connection.HasGiven());
					}

					// Latching and carrying nothing hold across the whole matrix,
					// not just the hand-picked sequences above.
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

#endif // WITH_DEV_AUTOMATION_TESTS
