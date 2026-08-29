// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/DestructionShed.h"

#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

/*
 * File-local names sit in the NAMED namespace, like Core/Corbel: an anonymous namespace is
 * private to a TRANSLATION UNIT rather than to a file, and a unity build merges many files
 * into one, so two file-local names that collide are a hard compile error between files that
 * never refer to each other. See CURRENT_STATE.md.
 */
namespace DestructionShed
{
	using namespace DestructionLayout;

	bool Build(const FShedSpec& Spec, DestructionLayout::FBrickLayout& OutLayout)
	{
		/*
		 * EMPTIED FIRST AND FILLED LAST, exactly as the corbel and the wall producers do. A refused
		 * spec must leave a caller who ignored the return value with nothing, rather than with
		 * whatever was laid before the builder gave up.
		 *
		 * THE GUARDS ARE WRITTEN `!(x > 0.0)`, NEVER `x <= 0.0`, because every comparison against a
		 * NaN is false: a NaN dimension would slip PAST the second spelling and be laid as a shed of
		 * NaN-sized boxes whose every joint reads as intact. The wythe and the joint thickness are
		 * the two lengths every piece and every bed share, so a bad one poisons the whole section.
		 */
		OutLayout = FBrickLayout();

		if (!(Spec.WytheCm > 0.0) || !(Spec.JointThicknessCm > 0.0)
			|| !(Spec.PierWidthCm > 0.0) || !(Spec.BaseHeightCm > 0.0) || !(Spec.HeadHeightCm > 0.0)
			|| !(Spec.PierSeparationCm > 0.0) || !(Spec.RoofThicknessCm > 0.0)
			|| !(Spec.OverhangLengthCm > 0.0) || !(Spec.OverhangThicknessCm > 0.0)
			|| !(Spec.PostWidthCm > 0.0))
		{
			return false;
		}

		/*
		 * THE COORDINATES, WORKED FROM THE SPEC ONCE. X is depth (back pier toward front pier toward
		 * the door, increasing X); Z is height; Y is the single wythe, centred on 0. Every Z boundary
		 * carries a joint thickness of mortar or bearing gap above the piece below it, so the roof and
		 * the overhang bottoms sit one JointThicknessCm clear of the heads and the post they land on —
		 * which is exactly the separation MakeInterface reads as a bed joint.
		 */
		const double HalfWytheCm = Spec.WytheCm / 2.0;

		const double FrontPierLeftCm = Spec.BackPierLeftCm + Spec.PierSeparationCm;
		const double HeadBottomZCm = Spec.BaseHeightCm + Spec.JointThicknessCm;
		const double HeadTopZCm = HeadBottomZCm + Spec.HeadHeightCm;
		const double BeamBottomZCm = HeadTopZCm + Spec.JointThicknessCm;
		const double BeamTopZCm = BeamBottomZCm + Spec.RoofThicknessCm;

		const double RoofFrontCm = Spec.RoofFrontCm;
		const double OverhangRightCm = Spec.OverhangBackCm + Spec.OverhangLengthCm;
		const double PostLeftCm = Spec.PostCentreCm - Spec.PostWidthCm / 2.0;
		const double PostRightCm = Spec.PostCentreCm + Spec.PostWidthCm / 2.0;

		FBrickLayout Laid;

		/*
		 * ONE DOOR FOR EVERY PIECE: a box from its X and Z spans (Y is always the full wythe), the
		 * mass derived from that same box via the shared PieceMassKg so a piece cannot weigh a
		 * different size than it sits, and the authored material recorded so the cross-material
		 * physics survives into play. The handle IS the box index — FBrickLayout's parallel-array
		 * contract — so the box is appended in the same breath the piece is added.
		 */
		const auto AddPiece =
			[&](double LeftXCm, double RightXCm, double BottomZCm, double TopZCm,
				const DestructionProfiles::FMaterialProfile& Material, bool bGrounded) -> int32
		{
			FPieceBox Box;
			Box.CentreCm = FVector(
				(LeftXCm + RightXCm) / 2.0, 0.0, (BottomZCm + TopZCm) / 2.0);
			Box.ExtentCm = FVector(
				(RightXCm - LeftXCm) / 2.0, HalfWytheCm, (TopZCm - BottomZCm) / 2.0);

			const int32 Piece = Laid.Structure.AddPiece(
				PieceMassKg(Box, Material.DensityGramsPerCubicCm), bGrounded, Box.CentreCm);

			if (Piece == INDEX_NONE)
			{
				return INDEX_NONE;
			}

			Laid.Boxes.Add(Box);
			Laid.Structure.SetPieceMaterial(Piece, &Material);

			return Piece;
		};

		/*
		 * A masonry pier is a grounded BASE (its fused lower courses) plus one removable HEAD course;
		 * the roof and the overhang are the two timber beams; the post is the grounded timber under
		 * the overhang's front. Seven pieces, laid back-to-front so the picture and the code agree.
		 */
		const int32 BackBase = AddPiece(
			Spec.BackPierLeftCm, Spec.BackPierLeftCm + Spec.PierWidthCm,
			0.0, Spec.BaseHeightCm, DestructionProfiles::ClayBrick, true);
		const int32 BackHead = AddPiece(
			Spec.BackPierLeftCm, Spec.BackPierLeftCm + Spec.PierWidthCm,
			HeadBottomZCm, HeadTopZCm, DestructionProfiles::ClayBrick, false);

		const int32 FrontBase = AddPiece(
			FrontPierLeftCm, FrontPierLeftCm + Spec.PierWidthCm,
			0.0, Spec.BaseHeightCm, DestructionProfiles::ClayBrick, true);
		const int32 FrontHead = AddPiece(
			FrontPierLeftCm, FrontPierLeftCm + Spec.PierWidthCm,
			HeadBottomZCm, HeadTopZCm, DestructionProfiles::ClayBrick, false);

		const int32 Roof = AddPiece(
			Spec.BackPierLeftCm, RoofFrontCm,
			BeamBottomZCm, BeamTopZCm, DestructionProfiles::Timber, false);
		const int32 Overhang = AddPiece(
			Spec.OverhangBackCm, OverhangRightCm,
			BeamBottomZCm, BeamTopZCm, DestructionProfiles::Timber, false);

		const int32 Post = AddPiece(
			PostLeftCm, PostRightCm,
			0.0, HeadTopZCm, DestructionProfiles::Timber, true);

		if (BackBase == INDEX_NONE || BackHead == INDEX_NONE || FrontBase == INDEX_NONE
			|| FrontHead == INDEX_NONE || Roof == INDEX_NONE || Overhang == INDEX_NONE
			|| Post == INDEX_NONE)
		{
			return false;
		}

		/*
		 * THE SIX BED JOINTS, EACH THROUGH MakeInterface — the same door the wall and the corbel use,
		 * so the areas, the normals and the rectangles are that producer's rather than this one's.
		 * The brick beds are bonded mortar; the roof and post bearings are compression-only DryStone;
		 * the fixing is a tension-capable Screw. That per-contact connection choice IS the shed's
		 * cross-material authoring, and MakeInterface refuses any pair that does not actually share a
		 * bed face — so a mis-sized piece is caught here rather than solved as a healthy joint.
		 */
		const auto Join =
			[&](int32 A, int32 B, const FConnectionStrength& Strength) -> bool
		{
			FConnection Connection;

			if (!MakeInterface(
					A, Laid.Boxes[A], B, Laid.Boxes[B], Spec.JointThicknessCm, Strength, Connection))
			{
				return false;
			}

			return Laid.Structure.AddConnection(Connection) != INDEX_NONE;
		};

		if (!Join(BackBase, BackHead, DestructionProfiles::GeneralPurposeMortar)
			|| !Join(FrontBase, FrontHead, DestructionProfiles::GeneralPurposeMortar)
			|| !Join(BackHead, Roof, DestructionProfiles::DryStone)
			|| !Join(FrontHead, Roof, DestructionProfiles::DryStone)
			|| !Join(FrontHead, Overhang, DestructionProfiles::Screw)
			|| !Join(Post, Overhang, DestructionProfiles::DryStone))
		{
			return false;
		}

		OutLayout = MoveTemp(Laid);

		return true;
	}
}
