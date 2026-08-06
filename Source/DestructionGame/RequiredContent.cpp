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
		DestructionContent::BrickNeighbourMaterialPaths[5]
	};
}

TArrayView<const TCHAR* const> DestructionContent::RequiredContentPaths()
{
	return TArrayView<const TCHAR* const>(RequiredContentRows);
}
