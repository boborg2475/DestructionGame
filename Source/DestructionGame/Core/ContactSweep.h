// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Layout.h"

/**
 * THE GENERAL CONTACT-FINDING PRODUCER Core/Layout.h always said would replace the generative
 * pair supply: given a layout's boxes, find every pair that shares a face across the joint
 * thickness and form its joint.
 *
 * It is what lets a structure be DATA. RunningBond, the corbel and the sheds know which pairs
 * they laid; a layout read from a file knows only boxes and materials, so this is the one place
 * that finds the pairs — the interactive build, an authored layout file and a saved player build
 * all form their joints by the same rule.
 *
 * World-free, like everything under Core. One direction of inclusion.
 */
namespace DestructionLayout
{
	/**
	 * Offer every pair of boxes that could share a face to MakeInterface and keep what it accepts,
	 * the LOWER piece named A so a bed normal reads as a bed beneath the piece it carries. The
	 * profile is decided per contact by BuildMode::JointForContact's boxed overload from the two
	 * materials, the normal and the two boxes (a bed or bonded quoin is full mortar, a same-course
	 * head joint is the perpend, a contact touching Timber is a dry bearing).
	 *
	 * BUCKETED ON Z so a building of thousands of pieces is not millions of refusals: two boxes
	 * can only share a face if their Z spans are within a joint of each other, so a piece is
	 * offered only pieces whose bottoms lie within the tallest piece's height below its own
	 * bottom and one joint above its top. Every accepted pair is offered exactly once.
	 *
	 * Every piece must carry a material (SetPieceMaterial); a refusal adds nothing.
	 *
	 * @return true if every joint was formed; false if a piece has no material or a joint was refused.
	 */
	bool SweepContacts(FBrickLayout& Layout, double JointThicknessCm);
}
