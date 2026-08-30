// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/DestructionShed3D.h"

#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

/*
 * File-local names sit in the NAMED namespace, like Core/DestructionShed and Core/Corbel: an
 * anonymous namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build
 * merges many files into one, so two file-local names that collide are a hard compile error
 * between files that never refer to each other. See CURRENT_STATE.md.
 */
namespace DestructionShed3D
{
	using namespace DestructionLayout;

	bool Build(const FShed3DSpec& Spec, DestructionLayout::FBrickLayout& OutLayout)
	{
		/*
		 * EMPTIED FIRST AND FILLED LAST, exactly as the 2D shed and the wall producers do. A refused
		 * spec must leave a caller who ignored the return value with nothing, rather than with
		 * whatever was laid before the builder gave up.
		 *
		 * THE GUARDS ARE WRITTEN `!(x > 0.0)`, NEVER `x <= 0.0`, because every comparison against a
		 * NaN is false: a NaN dimension would slip PAST the second spelling and be laid as a shed of
		 * NaN-sized boxes whose every joint reads as intact.
		 */
		OutLayout = FBrickLayout();

		if (!(Spec.WallThicknessCm > 0.0) || !(Spec.WallHeightCm > 0.0)
			|| !(Spec.JointThicknessCm > 0.0) || !(Spec.BoxWidthXCm > 0.0) || !(Spec.BoxDepthYCm > 0.0)
			|| !(Spec.RoofInsetXCm > 0.0) || !(Spec.RoofBearingLapYCm > 0.0) || !(Spec.RoofThicknessCm > 0.0)
			|| !(Spec.OverhangWidthXCm > 0.0) || !(Spec.OverhangLengthYCm > 0.0)
			|| !(Spec.OverhangThicknessCm > 0.0) || !(Spec.FixingLapYCm > 0.0)
			|| !(Spec.PostWidthYCm > 0.0))
		{
			return false;
		}

		/*
		 * THE COORDINATES, WORKED FROM THE SPEC ONCE. X is width, Y is depth (into the door), Z is
		 * height. The side walls fit BETWEEN the back and front walls with a JointThicknessCm gap on
		 * Y, so each corner is a genuine face-sharing joint whose normal points along +/-Y. The roof,
		 * overhang and post all leave a JointThicknessCm gap in Z under the beam they carry, which is
		 * exactly the separation MakeInterface reads as a bed joint.
		 */
		const double WallTopZCm = Spec.WallHeightCm;
		const double BeamBottomZCm = Spec.WallHeightCm + Spec.JointThicknessCm;
		const double BeamTopZCm = BeamBottomZCm + Spec.RoofThicknessCm;
		const double OverhangTopZCm = BeamBottomZCm + Spec.OverhangThicknessCm;

		const double SideWallBackYCm = Spec.WallThicknessCm + Spec.JointThicknessCm;
		const double SideWallFrontYCm = Spec.BoxDepthYCm - Spec.WallThicknessCm - Spec.JointThicknessCm;

		const double RoofLeftXCm = Spec.RoofInsetXCm;
		const double RoofRightXCm = Spec.BoxWidthXCm - Spec.RoofInsetXCm;
		const double RoofBackYCm = Spec.WallThicknessCm - Spec.RoofBearingLapYCm;
		const double RoofFrontYCm = Spec.BoxDepthYCm - Spec.WallThicknessCm + Spec.RoofBearingLapYCm;

		const double OverhangLeftXCm = Spec.OverhangCentreXCm - Spec.OverhangWidthXCm / 2.0;
		const double OverhangRightXCm = Spec.OverhangCentreXCm + Spec.OverhangWidthXCm / 2.0;
		const double OverhangBackYCm = Spec.BoxDepthYCm - Spec.FixingLapYCm;
		const double OverhangFrontYCm = OverhangBackYCm + Spec.OverhangLengthYCm;

		const double PostBackYCm = Spec.PostCentreYCm - Spec.PostWidthYCm / 2.0;
		const double PostFrontYCm = Spec.PostCentreYCm + Spec.PostWidthYCm / 2.0;

		FBrickLayout Laid;

		/*
		 * ONE DOOR FOR EVERY PIECE: an axis-aligned box from its X, Y and Z spans, the mass derived
		 * from that same box via the shared PieceMassKg so a piece cannot weigh a different size than
		 * it sits, and the authored material recorded so the cross-material physics survives into
		 * play. The handle IS the box index — FBrickLayout's parallel-array contract — so the box is
		 * appended in the same breath the piece is added.
		 */
		const auto AddPiece =
			[&](double LeftXCm, double RightXCm, double BackYCm, double FrontYCm,
				double BottomZCm, double TopZCm,
				const DestructionProfiles::FMaterialProfile& Material, bool bGrounded) -> int32
		{
			FPieceBox Box;
			Box.CentreCm = FVector(
				(LeftXCm + RightXCm) / 2.0, (BackYCm + FrontYCm) / 2.0, (BottomZCm + TopZCm) / 2.0);
			Box.ExtentCm = FVector(
				(RightXCm - LeftXCm) / 2.0, (FrontYCm - BackYCm) / 2.0, (TopZCm - BottomZCm) / 2.0);

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
		 * SEVEN PIECES: four grounded ClayBrick walls closing the box, one Timber roof beam and one
		 * Timber overhang (both free), and a grounded Timber post under the overhang's front.
		 */
		const int32 BackWall = AddPiece(
			0.0, Spec.BoxWidthXCm, 0.0, Spec.WallThicknessCm,
			0.0, WallTopZCm, DestructionProfiles::ClayBrick, true);
		const int32 FrontWall = AddPiece(
			0.0, Spec.BoxWidthXCm, Spec.BoxDepthYCm - Spec.WallThicknessCm, Spec.BoxDepthYCm,
			0.0, WallTopZCm, DestructionProfiles::ClayBrick, true);
		const int32 LeftWall = AddPiece(
			0.0, Spec.WallThicknessCm, SideWallBackYCm, SideWallFrontYCm,
			0.0, WallTopZCm, DestructionProfiles::ClayBrick, true);
		const int32 RightWall = AddPiece(
			Spec.BoxWidthXCm - Spec.WallThicknessCm, Spec.BoxWidthXCm, SideWallBackYCm, SideWallFrontYCm,
			0.0, WallTopZCm, DestructionProfiles::ClayBrick, true);

		const int32 Roof = AddPiece(
			RoofLeftXCm, RoofRightXCm, RoofBackYCm, RoofFrontYCm,
			BeamBottomZCm, BeamTopZCm, DestructionProfiles::Timber, false);
		const int32 Overhang = AddPiece(
			OverhangLeftXCm, OverhangRightXCm, OverhangBackYCm, OverhangFrontYCm,
			BeamBottomZCm, OverhangTopZCm, DestructionProfiles::Timber, false);

		const int32 Post = AddPiece(
			OverhangLeftXCm, OverhangRightXCm, PostBackYCm, PostFrontYCm,
			0.0, WallTopZCm, DestructionProfiles::Timber, true);

		if (BackWall == INDEX_NONE || FrontWall == INDEX_NONE || LeftWall == INDEX_NONE
			|| RightWall == INDEX_NONE || Roof == INDEX_NONE || Overhang == INDEX_NONE
			|| Post == INDEX_NONE)
		{
			return false;
		}

		/*
		 * THE EIGHT JOINTS, EACH THROUGH MakeInterface — the same door the wall and the 2D shed use,
		 * so the areas, the normals and the rectangles are that producer's rather than this one's. The
		 * four corners are bonded GeneralPurposeMortar across the walls' shared out-of-plane (Y-normal)
		 * faces; the two roof bearings and the post bearing are compression-only DryStone; the fixing
		 * is a tension-capable Screw. Each bearing names the LOWER piece as A and the UPPER as B, so
		 * the axis-of-separation normal (oriented by B) makes the joint read as a bed beneath the piece
		 * it carries. MakeInterface refuses any pair that does not actually share a face — so a
		 * mis-sized piece is caught here rather than solved as a healthy joint.
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

		if (!Join(BackWall, LeftWall, DestructionProfiles::GeneralPurposeMortar)
			|| !Join(BackWall, RightWall, DestructionProfiles::GeneralPurposeMortar)
			|| !Join(FrontWall, LeftWall, DestructionProfiles::GeneralPurposeMortar)
			|| !Join(FrontWall, RightWall, DestructionProfiles::GeneralPurposeMortar)
			|| !Join(BackWall, Roof, DestructionProfiles::DryStone)
			|| !Join(FrontWall, Roof, DestructionProfiles::DryStone)
			|| !Join(FrontWall, Overhang, DestructionProfiles::Screw)
			|| !Join(Post, Overhang, DestructionProfiles::DryStone))
		{
			return false;
		}

		/*
		 * THE 3D FLAG is what routes this structure to the Dim3D pose and lifts the bridge's Y-normal
		 * refusal, so the four out-of-plane corner joints reach the 3D LP rather than being rejected.
		 */
		Laid.Structure.SetThreeDimensional(true);

		OutLayout = MoveTemp(Laid);

		return true;
	}
}
