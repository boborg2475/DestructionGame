// Copyright Epic Games, Inc. All Rights Reserved.

#include "RequiredContent.h"

/*
 * THE REQUIRED-CONTENT TABLE — one row per path this module resolves from C++.
 *
 * FILE-LOCAL NAMES CARRY A RequiredContent PREFIX. An anonymous namespace is private to a
 * TRANSLATION UNIT rather than to a file, and a unity build merges many files into one — at
 * which point two file-local arrays of the same name are one declaration twice. See
 * CURRENT_STATE.md, where the collisions this has already caused are recorded.
 */
namespace
{
	const TCHAR* const RequiredContentRows[] = {
		DestructionContent::MoveActionPath,
		DestructionContent::LookActionPath,
		DestructionContent::MouseLookActionPath,
		DestructionContent::AscendActionPath,
		DestructionContent::InspectPieceActionPath,
		DestructionContent::HoverPieceActionPath,
		DestructionContent::DefaultMappingContextPath,
		DestructionContent::MouseLookMappingContextPath,

		/*
		 * THE SESSION'S KEYBOARD: the modifier the camera is chorded to, the eight shortcuts the
		 * toolbar draws, and the context that maps them. The modifier is the row that earns its
		 * keep on its own — nothing resolves it onto a CDO, so this table is the only place that
		 * would notice it had gone, and what goes wrong when it does is that the camera stops
		 * turning entirely rather than that one key stops working.
		 */
		DestructionContent::LookModifierActionPath,
		DestructionContent::SessionToggleModeActionPath,
		DestructionContent::SessionPieceBrickActionPath,
		DestructionContent::SessionPiecePlateActionPath,
		DestructionContent::SessionPieceLintelActionPath,
		DestructionContent::SessionSnapToggleActionPath,
		DestructionContent::SessionCourseUpActionPath,
		DestructionContent::SessionCourseDownActionPath,
		DestructionContent::SessionRunActionPath,
		DestructionContent::SessionMappingContextPath,

		DestructionContent::BrickPlaceholderMeshPath,
		DestructionContent::BrickHoverMaterialPath,
		DestructionContent::BrickSelectedMaterialPath,
		DestructionContent::BrickInspectedMaterialPath,

		/*
		 * ONE ROW PER COLOUR SLOT, SPELLED OUT RATHER THAN SPLICED IN FROM THE ARRAY. This table is
		 * a LIST OF PATHS, and a loop appending six of them would make it a list that cannot be
		 * read at a glance — which is the one thing the table is for. The paths themselves are
		 * still named once, in RequiredContent.h, so there is nothing here to disagree with.
		 */
		DestructionContent::BrickNeighbourMaterialPaths[0],
		DestructionContent::BrickNeighbourMaterialPaths[1],
		DestructionContent::BrickNeighbourMaterialPaths[2],
		DestructionContent::BrickNeighbourMaterialPaths[3],
		DestructionContent::BrickNeighbourMaterialPaths[4],
		DestructionContent::BrickNeighbourMaterialPaths[5],

		DestructionContent::ShedBrickMaterialPath,
		DestructionContent::ShedTimberMaterialPath
	};
}

TArrayView<const TCHAR* const> DestructionContent::RequiredContentPaths()
{
	return TArrayView<const TCHAR* const>(RequiredContentRows);
}
