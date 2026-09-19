// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Core/StructureBinding.h"

/**
 * The set of pieces the player has picked out, in the order they were picked.
 *
 * A plain struct with no world, for the same reason FStructureBinding is one: what a
 * selection can get wrong — a toggle that adds twice, a toggle that fails to remove, a
 * default ref smuggled in as if it were a brick — is arithmetic on a list, and arithmetic
 * belongs in the suite that runs in milliseconds.
 *
 * Order is insertion order, not handle order, and is kept rather than sorted: a player who
 * picks three bricks has said something about the order they picked them in, and a TSet
 * would throw away that order along with the stable iteration a presented menu needs.
 *
 * A ref missing either half is refused rather than stored. A click on the floor arrives as
 * a default FPieceRef, and a default is also what "we have no answer" looks like — the same
 * polarity trap Core/PieceMenu.h records. A selection is a command's target list, so it
 * takes the cautious reading: nothing selected rather than "piece nothing of structure
 * nothing" is. Whether a miss should also clear the set is the presenter's policy,
 * deliberately not decided here.
 */
struct FPieceSelection
{
	/**
	 * Add this ref if it is not in the set, remove it if it is.
	 *
	 * @return whether the ref is SELECTED once the call is done — so a refused ref answers
	 *         false, which is the same answer a deselection gives and is the correct one:
	 *         it is not in the set either way.
	 */
	bool Toggle(const FPieceRef& Ref);

	/** Whether this ref is in the set. Compared by VALUE: a ref is two integers. */
	bool Contains(const FPieceRef& Ref) const;

	/** Empty the set. @return whether there was anything to empty. */
	bool Clear();

	/** How many pieces are selected. This is the count a menu reports. */
	int32 Num() const;

	/** The selected refs, in the order they were picked. */
	TArrayView<const FPieceRef> Refs() const;

private:

	TArray<FPieceRef> Selected;
};
