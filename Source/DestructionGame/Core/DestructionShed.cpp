// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/DestructionShed.h"

#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

/*
 * File-local names sit in the named namespace, like Core/Corbel: an anonymous namespace is
 * private to a translation unit rather than to a file, and a unity build merges many files
 * into one, so two colliding file-local names are a hard compile error between files that
 * never refer to each other. See CURRENT_STATE.md.
 */
namespace DestructionShed
{
	using namespace DestructionLayout;

	bool Build(const FShedSpec& Spec, DestructionLayout::FBrickLayout& OutLayout)
	{
		/*
		 * Emptied first and filled last: a refused spec must leave a caller who ignored the return
		 * value with nothing, not a partial lay. Guards are written `!(x > 0.0)`, never `x <= 0.0`
		 * — every comparison against NaN is false, so `<= 0.0` would let a NaN dimension slip
		 * through and lay a shed of NaN-sized boxes whose every joint reads intact.
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
		 * Coordinates, worked from the spec once. X is depth (back pier toward front); Z is height;
		 * Y is the single wythe, centred on 0. Every Z boundary carries a joint thickness above the
		 * piece below it — the separation MakeInterface reads as a bed.
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
		 * One door for every piece: a box from its X and Z spans (Y is the full wythe), mass via the
		 * shared PieceMassKg, and the material recorded so cross-material physics survives into
		 * play. The handle is the box index (FBrickLayout's parallel-array contract).
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
		 * A masonry pier is a grounded base (fused lower courses) plus one removable head course.
		 * Seven pieces, laid back-to-front so the picture and the code agree.
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
		 * Six bed joints through MakeInterface. Brick beds are bonded mortar; roof and post bearings
		 * are compression-only DryStone; the fixing is a tension-capable Screw — that per-contact
		 * choice is the shed's cross-material authoring. MakeInterface refuses any pair that does
		 * not share a bed face, so a mis-sized piece is caught here rather than solved as healthy.
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
