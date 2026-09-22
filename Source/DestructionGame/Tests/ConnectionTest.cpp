// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/Connection.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Named namespace, not anonymous: a unity build merges two test files' anonymous namespaces, and
 * this one used to declare a Mortar and a MakeNaN of its own. The strength profiles now come from
 * Core/Profiles, so retuning mortar retunes it here rather than leaving a stale copy that passes.
 */
namespace ConnectionTestSupport
{
	using namespace DestructionProfiles;

	/**
	 * Structural concrete, the calibration baseline DESIGN.md §4 asks for, from the shared material
	 * library. Uncoupled on purpose: zero friction, so shear collapses to plain cohesion, the three
	 * axes stay independent, and every expected number traces to one strength (coupling is covered by
	 * ConnectionStrength.FrictionCoupling). A reference, not a copy: binding to a static object is
	 * constant initialisation, where copying its fields would be a dynamic initialiser across TUs.
	 */
	const FConnectionStrength& ConcreteUncoupled = StructuralConcrete.Strength;

	/** A 10 cm x 10 cm interface. Round, so the arithmetic stays checkable by eye. */
	constexpr double JointAreaSqCm = 100.0;

	/**
	 * Force, in Unreal units, that loads the given area to the given stress. Spelled out rather than
	 * reusing ForceUnitsPerMPaSqCm so the test fails if that constant is wrong: 1 N = 100 uu,
	 * 1 cm2 = 100 mm2, 1 MPa = 1 N/mm2 -> 10000 uu per MPa per cm2.
	 */
	constexpr double ForceForMPa(double MPa, double AreaSqCm)
	{
		return MPa * 100.0 * 100.0 * AreaSqCm;
	}

	/**
	 * A bed joint: horizontal interface, normal pointing up at the piece above, so the force passed
	 * in is on the upper piece and its weight squeezes the joint in compression (ConnectionLoad.h).
	 */
	const FVector BedJointNormal(0.0, 0.0, 1.0);

	/**
	 * A head joint: vertical interface, normal pointing sideways at the neighbour. The same downward
	 * weight is pure shear here — the whole point of the connection owning its own normal.
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

	/**
	 * The bit pattern of a double, for assertions that compare exactly. %f rounds two numbers an ulp
	 * apart into the same text, so a bitwise failure printed that way reads "expected 0.500000, got
	 * 0.500000" and says nothing.
	 */
	FString Bits(double Value)
	{
		uint64 Raw = 0;
		FMemory::Memcpy(&Raw, &Value, sizeof(Raw));
		return FString::Printf(TEXT("%.17g [%016llx]"), Value, Raw);
	}

	/*
	 * The sweep matrix, shared by Connection.DegenerateInputs and Connection.UtilisationQuery. The
	 * degenerate rows are the point: an evaluator agreeing with ApplyForce on well-formed joints but
	 * diverging on a joint with no interface plane reopens the fail-open hole of DESIGN.md §2.
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

	/*
	 * Chaos can produce a NaN velocity in a pathological contact, arriving here as a NaN force.
	 * Unguarded it poisons the result and the joint reports itself intact during a physics blowup.
	 */
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

	/*
	 * Dry stone matters here: real zeroes in two of its three strengths, so it reaches the
	 * degenerate paths without anything being misconfigured.
	 */
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
 * A connection resolves a world-space force through its own normal, area and strength profile, and
 * reports the utilisation. Pure arithmetic: no world or solver, so the assertions are on the ratio,
 * not on movement. Each case changes the answer through one stored property — same force, different
 * normal or area — because a connection quietly using a unit normal or area would pass a single case.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FConnectionUtilisationTest,
	"DestructionGame.Core.Connection.Utilisation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FConnectionUtilisationTest::RunTest(const FString& Parameters)
{
	using namespace ConnectionTestSupport;

	// Read out of the shared profile at test time, not during static initialisation.
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

		/*
		 * One axis at a time, each against its own strength: compression 30 MPa, shear 6 MPa, tension
		 * 3 MPa. The other two axes are exactly zero, so which governs is unambiguous.
		 */
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

		/*
		 * The pair that proves the connection applies its stored normal: the same 6 MPa downward
		 * force reads 1.0 on the head joint above and 0.2 here. Ignoring the normal cannot produce both.
		 */
		{
			TEXT("the same weight on a bed joint is comfortable compression"),
			BedJointNormal, JointAreaSqCm,
			FVector(0.0, 0.0, -ForceForMPa(ConcreteShearMPa, JointAreaSqCm)),
			ConcreteShearMPa / ConcreteCompressiveMPa
		},

		/*
		 * And the pair that proves it applies its stored area: the compression case's force through
		 * half the interface, twice the stress. This one gives (2.0 > 1.0) — the breaking call still
		 * reports the ratio that broke it, pinned by the loop's HasGiven assertion below.
		 */
		{
			TEXT("halving the interface area doubles the utilisation"),
			BedJointNormal, JointAreaSqCm / 2.0,
			FVector(0.0, 0.0, -ForceForMPa(ConcreteCompressiveMPa, JointAreaSqCm)),
			2.0
		},

		/*
		 * Oblique load: 3 MPa into the face and 3 across it. Compression is 3/30 = 0.1 and shear
		 * 3/6 = 0.5, so shear governs. Worked through both axes so the force is not chosen carelessly.
		 */
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
 * Giving latches, and a joint that has given carries nothing — the substance of the connection
 * object (reporting a utilisation is only composition of existing maths). Latching is not tidiness:
 * mortar does not re-bond, and a joint that healed when the load dropped would make collapse
 * non-monotonic. Carrying nothing afterwards is what phase 2's redistribution depends on.
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

	/*
	 * Pure compression throughout: shear and tension are exactly zero on a bed joint under vertical
	 * load, so the compression axis drives every number here.
	 */

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

	/*
	 * Exactly at the limit is fully utilised but still holding: the boundary is "gives above 1", not
	 * "gives at 1".
	 */
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

		/*
		 * Point 4: out of the structure, so it carries nothing. Zero exactly, not "a smaller number"
		 * — phase 2 sums these to redistribute.
		 */
		TestTrue(
			FString::Printf(TEXT("a given joint carries nothing, expected 0.0, got %f"), AfterRelief),
			FMath::IsNearlyEqual(AfterRelief, 0.0, Tolerance));

		const double AfterUnloading = Connection.ApplyForce(FVector::ZeroVector);

		TestTrue(TEXT("a given joint stays given with no load at all"), Connection.HasGiven());
		TestTrue(
			FString::Printf(TEXT("an unloaded given joint still carries nothing, got %f"), AfterUnloading),
			FMath::IsNearlyEqual(AfterUnloading, 0.0, Tolerance));
	}

	/*
	 * Latching is not compression-specific: a head joint sheared apart must stay apart too, or the
	 * latch lives on the wrong axis.
	 */
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

	/*
	 * Stated as a property, not examples: given-ness only ever goes one way. This is what makes
	 * collapse monotonic, so it is asserted over a load history that rises then decays away.
	 */
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
		 * 39 MPa against the 38 MPa limit (the mean f_cm since the 2026-08-13 re-anchor) is the only
		 * entry that crosses, so the history must end broken — else the loop above passes on a
		 * connection that never gives.
		 */
		TestTrue(TEXT("the load history peaked past the limit, so the joint must have given"),
			Connection.HasGiven());
	}

	return true;
}

/**
 * Degenerate configuration and degenerate loads must fail closed, and a broken joint must never
 * read as intact. A property test sweeping normals, areas, loads and profiles; the numbers are
 * covered above.
 *
 * Two traps. ComputeUtilisation guards area and stress, returning a huge finite number rather than
 * a NaN (NaN compares false against everything, so a joint returning one would report itself
 * intact), and the connection must route through those guards. The normal is a new hole once the
 * halves compose: ClassifyForce answers a degenerate normal with a zero load — right in isolation
 * but "utilisation 0.0, fine" here — so a joint with no interface plane must read as given.
 *
 * The given-ness assertions have a direction: a given joint reports zero utilisation, so "not
 * broken" can never be inferred from the ratio. HasGiven is the authoritative state asserted.
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

					/*
					 * On a joint's first evaluation the two answers agree: it has given exactly
					 * when the reported ratio exceeded 1.
					 */
					TestTrue(
						FString::Printf(TEXT("%s: utilisation %f gave %d, expected %d"),
							*Context, Utilisation,
							Connection.HasGiven() ? 1 : 0,
							Utilisation > 1.0 ? 1 : 0),
						Connection.HasGiven() == (Utilisation > 1.0));

					const bool bIsRealJoint = Normal.bIsUsable && Area.bIsUsable;

					if (!bIsRealJoint || !Force.bIsWellFormed)
					{
						/*
						 * Fail closed: a joint with no interface plane, no area, or an unmakeable
						 * load must not read as intact — the one answer nothing downstream can detect.
						 */
						TestTrue(
							FString::Printf(TEXT("%s: a degenerate joint must read as given, got utilisation %f"),
								*Context, Utilisation),
							Connection.HasGiven());
					}

					/* Latching and carrying nothing hold across the whole matrix, not
					 * just the hand-picked sequences above. */
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
 * Asking a joint how loaded it is must not break it, and must give the same answer the break
 * decision would. The seam exists because ApplyForce is the only evaluator and it latches, so a
 * phase-5 display colouring every joint by utilisation each frame would take the wall apart by
 * drawing it.
 *
 * The number is already obtainable (ClassifyForce then ComputeUtilisation), so what is missing is a
 * single place that owns it. Every hand-composed route carries two hazards:
 *   - DESIGN.md §2's caller obligation: ClassifyForce answers a degenerate normal with a zero load,
 *     read downstream as "unloaded and healthy". ApplyForce closes that via ComputeUtilisation's
 *     guard; a naive renderer would paint a joint with no interface plane bright green.
 *   - The lockstep obligation: a transcription must track ApplyForce exactly. Five joints in
 *     Structure.CascadeFuzz settle at exactly 1.0 and one at 1 - 1 ulp, so one ulp of drift is five
 *     spurious failures.
 *
 * So the assertion is bitwise ==, not a tolerance: it is the only one that fails if UtilisationUnder
 * reimplements the evaluation instead of ApplyForce being re-expressed in terms of it. Two versions
 * can agree to 1e-9 and differ in the last bit. ApplyForce must become UtilisationUnder plus the latch.
 *
 * Asserted over the shared sweep matrix, degenerate rows included, since the well-formed rows are
 * the easy half.
 *
 * What a given joint reports: the arithmetic, ignoring the latch. Pinned below because it drifts. It
 * keeps one place knowing about latching and keeps the query a pure function of its inputs, and the
 * renderer checks HasGiven anyway. So a given joint and a fresh one of the same shape answer
 * identically, asserted directly.
 *
 * No world, solver or gravity: pure arithmetic on one struct.
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
	 * The matrix. Each cell queries a fresh joint twice, checks it is still intact, then lets the
	 * same joint take the force for real; the three answers must agree to the last bit. Querying
	 * first is what catches a latching query — a UtilisationUnder that broke the joint would leave
	 * ApplyForce answering zero, and every non-zero row would disagree.
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

					// The joint is still fresh here, so this is the evaluation the query reported.
					const double Applied = Connection.ApplyForce(Force.Force);

					TestTrue(
						FString::Printf(
							TEXT("%s: the query must match ApplyForce BITWISE, query %s vs applied %s"),
							*Context, *Bits(Queried), *Bits(Applied)),
						Queried == Applied);

					/*
					 * The query fails closed on the same rows ApplyForce does. Checked through
					 * HasGiven, not the ratio, since a given joint reports zero: the equality above
					 * tied the two together, and this adds the direction — the shared answer must be
					 * the failing one, not a shared zero.
					 */
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

	/*
	 * Non-latching, asked hard. A query that broke the joint would look identical from the ratio, so
	 * the evidence is that the joint is still breakable afterwards. Pure compression on a bed joint:
	 * shear and tension are zero, so the compression axis is what these numbers measure.
	 */
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

		/*
		 * And the joint is genuinely still loaded to breaking: a query that had latched would answer
		 * this call zero from ApplyForce's early-out, and the structure would report a wall that never fell.
		 */
		const double Applied = Connection.ApplyForce(Overload);

		TestTrue(
			FString::Printf(TEXT("after %d queries the joint must still break, expected 2.0, got %s"),
				FrameCount, *Bits(Applied)),
			FMath::IsNearlyEqual(Applied, 2.0, Tolerance));

		TestTrue(TEXT("and it must be given afterwards, because ApplyForce is what latches"),
			Connection.HasGiven());
	}

	/*
	 * What a given joint reports — pinned from both sides. The query answers the arithmetic and the
	 * live ApplyForce answers zero, so the two deliberately disagree; mirroring ApplyForce's latch
	 * would make both zero and look reasonable.
	 */
	{
		FConnection Given = MakeConnection(BedJointNormal, JointAreaSqCm, ConcreteUncoupled);
		Given.ApplyForce(Overload);

		TestTrue(TEXT("fixture precondition: the joint has given"), Given.HasGiven());

		const double Queried = Given.UtilisationUnder(HalfLoad);

		TestTrue(
			FString::Printf(TEXT("a given joint still answers the arithmetic, expected 0.5, got %s"),
				*Bits(Queried)),
			FMath::IsNearlyEqual(Queried, 0.5, Tolerance));

		/*
		 * Stated as the property, not a number: the query is a pure function of the joint's geometry,
		 * profile and force, so the latch cannot be an input to it.
		 */
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

	/*
	 * A severed joint is the other way into the latch — its piece was removed, it never failed — and
	 * it must answer the same way. Nothing about the query knows why a joint left the structure.
	 */
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
