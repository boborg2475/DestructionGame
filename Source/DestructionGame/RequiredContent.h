// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

/**
 * Every content path this module hard-references from C++.
 *
 * WHY A TABLE AT ALL. ConstructorHelpers::FObjectFinder resolves a path AT CONSTRUCTION,
 * not at compile time, so deleting or renaming an asset leaves the reference null and
 * nothing says so — the pawn simply stops responding to input, or a brick spawns with no
 * mesh. CURRENT_STATE.md records that hazard against ADestructionGameFlyingPawn,
 * ADestructionGamePlayerController and ABrickActor. A table turns "content deletion
 * silently breaks the game" into "a test goes red", which is the whole point of it.
 *
 * ADDING A HARD REFERENCE IS ADDING A ROW, the same shape as the profile libraries in
 * Core/Profiles: the sweep in Tests/RequiredContentTest.cpp picks a new row up for free,
 * and cross-checks in the other direction too, so a reference resolved on a CDO that is
 * NOT in this table fails as well.
 *
 * THE PATHS ARE NAMED CONSTANTS AND THE CONSTRUCTORS USE THESE, NOT LITERALS OF THEIR
 * OWN. A table of paths that all load is worth nothing if it is not the list the game
 * actually resolves — the test's cross-check is what catches that drift after the fact,
 * and one spelling of each path is what stops it happening. There is no second copy of
 * these strings anywhere in the module.
 */
namespace DestructionContent
{
	/* The four input actions ADestructionGameFlyingPawn binds. */
	inline constexpr const TCHAR* MoveActionPath = TEXT("/Game/Input/Actions/IA_Move.IA_Move");
	inline constexpr const TCHAR* LookActionPath = TEXT("/Game/Input/Actions/IA_Look.IA_Look");
	inline constexpr const TCHAR* MouseLookActionPath = TEXT("/Game/Input/Actions/IA_MouseLook.IA_MouseLook");
	inline constexpr const TCHAR* AscendActionPath = TEXT("/Game/Input/Actions/IA_Jump.IA_Jump");

	/**
	 * The input action that opens the piece context menu, bound on the PLAYER CONTROLLER.
	 *
	 * ON THE CONTROLLER RATHER THAN THE PAWN, deliberately: the controller already owns the
	 * mapping contexts, it outlives any pawn, and it is what carries the cursor and the
	 * deprojection this action needs. A pawn-side binding would go away with the pawn.
	 */
	inline constexpr const TCHAR* InspectPieceActionPath =
		TEXT("/Game/Input/Actions/IA_InspectPiece.IA_InspectPiece");

	/* The two mapping contexts ADestructionGamePlayerController adds for a local player. */
	inline constexpr const TCHAR* DefaultMappingContextPath = TEXT("/Game/Input/IMC_Default.IMC_Default");
	inline constexpr const TCHAR* MouseLookMappingContextPath = TEXT("/Game/Input/IMC_MouseLook.IMC_MouseLook");

	/**
	 * ABrickActor's placeholder mesh, and the one reference reflection cannot see: it is
	 * set onto a mesh component's own asset pointer rather than being a UPROPERTY of the
	 * actor, so Tests/RequiredContentTest.cpp reads it back by hand.
	 */
	inline constexpr const TCHAR* BrickPlaceholderMeshPath = TEXT("/Game/LevelPrototyping/Meshes/SM_Cube.SM_Cube");

	TArrayView<const TCHAR* const> RequiredContentPaths();
}
