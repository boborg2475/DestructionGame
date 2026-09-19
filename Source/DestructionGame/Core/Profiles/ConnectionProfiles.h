// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Core/ConnectionStrength.h"

/**
 * The shared connection profile library: one place a joint type's real-world
 * strengths live, so retuning mortar changes mortar everywhere. Values and
 * citations are in ConnectionProfiles.cpp.
 */
namespace DestructionProfiles
{
	/**
	 * What kind of joint a profile is, which decides which physical invariants apply.
	 *
	 * Data, not code: the class is a field on the row, not a branch — and it lets
	 * a test fixture be machine-recognisable, so an unbreakable joint can never
	 * quietly reach a scenario.
	 */
	enum class EConnectionProfileClass : uint8
	{
		/** A bond that grips as well as rubs — mortar. Real cohesion, real friction. */
		Bonded,

		/** No bond at all — dry stone. Carries shear purely by friction. */
		Frictional,

		/** Nail, screw, bolt. A discrete fastener does not care how hard the faces
		    are pressed together, so mu is exactly zero. */
		MechanicalFastener,

		/** TEST FIXTURE ONLY. Must never be used by a scenario. */
		TestFixture,
	};

	/**
	 * One row of the library. Adding a profile is adding one of these.
	 *
	 * Strength is a reference to the shipped constant, never a copy — `&Row.Strength`
	 * is the address a joint fastened with that row carries. A copy would make the
	 * obvious lookup (walk the library, compare pointers) silently find "no such row"
	 * for every joint. Same fix as FNamedMaterialProfile::Profile.
	 */
	struct FNamedConnectionProfile
	{
		const TCHAR* Name = nullptr;
		EConnectionProfileClass Class = EConnectionProfileClass::TestFixture;
		const FConnectionStrength& Strength;
	};

	extern const FConnectionStrength GeneralPurposeMortar;

	/**
	 * The weak-perpend row — general purpose mortar in a vertical head/corner joint, its two bond axes
	 * knocked down (owner-approved item 6b). See ConnectionProfiles.cpp for why a perpend is the weak link.
	 */
	extern const FConnectionStrength GeneralPurposeMortarPerpend;

	extern const FConnectionStrength LimeMortar;
	extern const FConnectionStrength DryStone;
	extern const FConnectionStrength Nail;
	extern const FConnectionStrength Screw;
	extern const FConnectionStrength Bolt;

	/** TEST FIXTURE ONLY — a joint that never gives. Never ship it in a scenario. */
	extern const FConnectionStrength Unbreakable;

	/**
	 * TEST FIXTURE ONLY — no cohesion at all, but a real tensile bond. Never ship it.
	 *
	 * Not a material. It exists because `DryStone` cannot answer the question the
	 * composite-depth work needs answered; see ConnectionProfiles.cpp.
	 */
	extern const FConnectionStrength CohesionlessBond;

	/**
	 * Every connection profile, so a sweep can check the whole library rather
	 * than whichever entries a test remembered to name.
	 */
	TArrayView<const FNamedConnectionProfile> AllConnectionProfiles();

	/**
	 * Which library row a joint's strength is, matched field for field — or null for one this
	 * library never shipped.
	 *
	 * By value because identity is already gone by the time anyone can ask: FStructure::AddConnection
	 * stores a copy of the profile, not a pointer to it, so a joint in a built wall has no address to
	 * compare. Matching the five fields back to the row is the honest route to the name — the answer
	 * is the extern itself, so `&FindConnectionProfileRow(S)->Strength` is the library's own address.
	 *
	 * Two rows with identical fields would be ambiguous; this returns the first. No two shipped rows
	 * are identical today (the closest pair, DryStone and the CohesionlessBond fixture, differ on
	 * tension), but a retune that collapsed two rows onto the same numbers would be a library bug,
	 * not a bug here.
	 *
	 * An exact comparison, not a tolerance: the strength being looked up is a copy of a library row's
	 * bits, not the result of arithmetic on one, so a near-match would answer a different question —
	 * and a NaN field correctly finds no row at all.
	 */
	const FNamedConnectionProfile* FindConnectionProfileRow(const FConnectionStrength& Strength);
}
