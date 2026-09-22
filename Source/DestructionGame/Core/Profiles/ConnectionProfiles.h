// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Core/ConnectionStrength.h"

/**
 * The shared connection profile library, so retuning mortar changes it everywhere. Values and
 * citations are in ConnectionProfiles.cpp.
 */
namespace DestructionProfiles
{
	/**
	 * A profile's joint kind, which decides which physical invariants apply. Also marks test
	 * fixtures so an unbreakable joint can't reach a scenario.
	 */
	enum class EConnectionProfileClass : uint8
	{
		/** Mortar: cohesion and friction. */
		Bonded,

		/** Dry stone: shear by friction only. */
		Frictional,

		/** Nail, screw, bolt: friction is zero, since a fastener ignores face pressure. */
		MechanicalFastener,

		/** TEST FIXTURE ONLY. Must never be used by a scenario. */
		TestFixture,
	};

	/**
	 * One library row. Strength is a reference to the shipped constant, not a copy, so a pointer
	 * lookup finds the row (as FNamedMaterialProfile::Profile).
	 */
	struct FNamedConnectionProfile
	{
		const TCHAR* Name = nullptr;
		EConnectionProfileClass Class = EConnectionProfileClass::TestFixture;
		const FConnectionStrength& Strength;
	};

	extern const FConnectionStrength GeneralPurposeMortar;

	/** General purpose mortar in a vertical perpend, with weaker bond axes (item 6b; see the .cpp). */
	extern const FConnectionStrength GeneralPurposeMortarPerpend;

	extern const FConnectionStrength LimeMortar;
	extern const FConnectionStrength DryStone;
	extern const FConnectionStrength Nail;
	extern const FConnectionStrength Screw;
	extern const FConnectionStrength Bolt;

	/** Test fixture only: a joint that never gives. Never ship it in a scenario. */
	extern const FConnectionStrength Unbreakable;

	/** Test fixture only: no cohesion but real tension, for the composite-depth tests (see the .cpp). */
	extern const FConnectionStrength CohesionlessBond;

	/** Every connection profile, for sweeps over the whole library. */
	TArrayView<const FNamedConnectionProfile> AllConnectionProfiles();

	/**
	 * The library row whose five fields exactly equal this strength, or null. By value because
	 * AddConnection stores a copy, so a built joint has no address to compare. Exact, not a
	 * tolerance: the input is a copy of a row's bits, and a NaN field finds nothing. Returns the
	 * first of any identical rows (none ship today).
	 */
	const FNamedConnectionProfile* FindConnectionProfileRow(const FConnectionStrength& Strength);
}
