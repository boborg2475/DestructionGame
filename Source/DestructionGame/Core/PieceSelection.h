// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Core/StructureBinding.h"

/**
 * The set of pieces the player has picked out, in the order they were picked.
 *
 * A PLAIN STRUCT WITH NO WORLD, for the same reason FStructureBinding is one: what a
 * selection can get wrong — a toggle that adds twice, a toggle that fails to remove, a
 * default ref smuggled in as if it were a brick — is arithmetic on a list, and arithmetic
 * belongs in the suite that runs in milliseconds. The controller owns one; nothing about
 * that ownership needs to be simulated to be checked.
 *
 * ORDER IS INSERTION ORDER, NOT HANDLE ORDER, and it is kept rather than sorted. What the
 * menu commits against is this list, and a player who picks three bricks has said something
 * about the order they picked them in; sorting would throw that away for no gain, and a
 * TSet would throw away the order AND the stable iteration a presented menu needs.
 *
 * A REF MISSING EITHER HALF IS REFUSED RATHER THAN STORED. A click on the floor arrives as a
 * default FPieceRef, and a default is also what "we have no answer" looks like — the same
 * polarity trap Core/PieceMenu.h records. A selection is a COMMAND's target list, so it must
 * take the cautious reading: nothing is selected rather than "piece nothing of structure
 * nothing" is. Whether a miss should also CLEAR the set is the presenter's policy and is
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
