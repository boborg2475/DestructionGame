// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Layout.h"

/**
 * A structure as a JSON file of boxes and materials under Content/Layouts/ (owner ruling
 * 2026-09-18: buildings are data, not C++). Joints are not stored; SweepContacts finds them on
 * load, as in interactive building, so authored, generated and saved builds load the same way.
 *
 * Format, version 1:
 *
 *   {
 *     "format": "DestructionGame.Layout",
 *     "version": 1,
 *     "jointThicknessCm": 1.0,
 *     "threeDimensional": true,
 *     "pieces": [
 *       { "min": [0, 0, 0], "max": [21.5, 10.25, 6.5], "material": "ClayBrick", "grounded": true },
 *       ...
 *     ]
 *   }
 *
 * `min` / `max` are world-axis corners in cm; `material` names a row of AllMaterialProfiles;
 * `grounded` defaults to false. Mass is derived from box and material on load, never stored.
 * Serialize writes one piece per line so files diff cleanly.
 *
 * Fails closed: any refusal leaves the output layout empty and, if asked, gives the reason.
 * World-free.
 */
namespace DestructionLayoutFile
{
	/** The `format` field every layout file carries. */
	extern const TCHAR* const FormatName;

	constexpr int32 FormatVersion = 1;

	/** Content/Layouts/<Name>.json. */
	FString ContentPath(const TCHAR* Name);

	/**
	 * Reads a layout from JSON: pieces, then joints by SweepContacts, then the 3D flag.
	 *
	 * @param OutError When given, a one-line reason on refusal.
	 */
	bool Parse(const FString& Json, DestructionLayout::FBrickLayout& OutLayout, FString* OutError = nullptr);

	/** The pieces of a layout as JSON text in the format above. Joints are not written. */
	FString Serialize(const DestructionLayout::FBrickLayout& Layout, double JointThicknessCm);

	/** Parse the file at Path. */
	bool LoadFile(const FString& Path, DestructionLayout::FBrickLayout& OutLayout, FString* OutError = nullptr);

	/** Serialize to the file at Path, creating directories as needed. */
	bool SaveFile(const FString& Path, const DestructionLayout::FBrickLayout& Layout, double JointThicknessCm);
}
