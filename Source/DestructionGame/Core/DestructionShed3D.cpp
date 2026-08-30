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

	bool BuildRecognizable(DestructionLayout::FBrickLayout& OutLayout)
	{
		/*
		 * EMPTIED FIRST AND FILLED LAST, exactly as Build and the 2D shed do. A path that gives up
		 * partway must leave a caller who ignored the return value with nothing, rather than with a
		 * half-built shed whose missing joints would read as a healthy structure.
		 *
		 * There is no spec to guard: the recognizable shed's dimensions are canonical, hardcoded here
		 * and pinned by the test's local constants. Every coordinate is a positive literal, so the
		 * NaN-safe `!(x > 0.0)` guarding Build performs has nothing to reject; the fail-closed points
		 * are AddPiece and Join, which refuse a degenerate box or a non-face pair.
		 */
		OutLayout = FBrickLayout();

		const double JointThicknessCm = 1.0;

		FBrickLayout Laid;

		/*
		 * ONE DOOR FOR EVERY PIECE — the same lambda Build uses: an axis-aligned box from its X, Y and
		 * Z spans, the mass derived from that same box via the shared PieceMassKg so a piece cannot
		 * weigh a different size than it sits, and the authored material recorded. The handle IS the
		 * box index (FBrickLayout's parallel-array contract), so the box is appended in the same breath.
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
		 * THE 24 PIECES, in the order the file header lists them. Walls and door base (Z < 200), then
		 * the window course in the left wall, then the two stepped gable ends, the timber roof of
		 * purlins and a ridge, and the porch — its two posts, its overhang, and the narrow central
		 * Cleat that ties the overhang's back down to the wall. ClayBrick masonry, Timber
		 * lintels/roof/porch. Grounded: the four walls' feet (back wall, right wall, both door piers,
		 * the sill) and the two posts; the cleat is NOT grounded — the wall holds it.
		 */
		const int32 BackWall = AddPiece(
			0.0, 300.0, 0.0, 25.0, 0.0, 200.0, DestructionProfiles::ClayBrick, true);
		const int32 RightWall = AddPiece(
			275.0, 300.0, 26.0, 274.0, 0.0, 200.0, DestructionProfiles::ClayBrick, true);
		const int32 LeftPier = AddPiece(
			0.0, 110.0, 275.0, 300.0, 0.0, 175.0, DestructionProfiles::ClayBrick, true);
		const int32 RightPier = AddPiece(
			190.0, 300.0, 275.0, 300.0, 0.0, 175.0, DestructionProfiles::ClayBrick, true);
		const int32 DoorHeader = AddPiece(
			0.0, 300.0, 275.0, 300.0, 176.0, 200.0, DestructionProfiles::Timber, false);

		const int32 Sill = AddPiece(
			0.0, 25.0, 26.0, 274.0, 0.0, 89.0, DestructionProfiles::ClayBrick, true);
		const int32 WinJambBack = AddPiece(
			0.0, 25.0, 26.0, 120.0, 90.0, 170.0, DestructionProfiles::ClayBrick, false);
		const int32 WinJambFront = AddPiece(
			0.0, 25.0, 180.0, 274.0, 90.0, 170.0, DestructionProfiles::ClayBrick, false);
		const int32 WinLintel = AddPiece(
			0.0, 25.0, 26.0, 274.0, 171.0, 200.0, DestructionProfiles::Timber, false);

		const int32 FGableBase = AddPiece(
			0.0, 300.0, 275.0, 300.0, 201.0, 230.0, DestructionProfiles::ClayBrick, false);
		const int32 FGableMid = AddPiece(
			75.0, 225.0, 275.0, 300.0, 231.0, 260.0, DestructionProfiles::ClayBrick, false);
		const int32 FGableApex = AddPiece(
			125.0, 175.0, 275.0, 300.0, 261.0, 290.0, DestructionProfiles::ClayBrick, false);
		const int32 BGableBase = AddPiece(
			0.0, 300.0, 0.0, 25.0, 201.0, 230.0, DestructionProfiles::ClayBrick, false);
		const int32 BGableMid = AddPiece(
			75.0, 225.0, 0.0, 25.0, 231.0, 260.0, DestructionProfiles::ClayBrick, false);
		const int32 BGableApex = AddPiece(
			125.0, 175.0, 0.0, 25.0, 261.0, 290.0, DestructionProfiles::ClayBrick, false);

		const int32 EavesPurlinL = AddPiece(
			0.0, 75.0, 0.0, 300.0, 231.0, 255.0, DestructionProfiles::Timber, false);
		const int32 EavesPurlinR = AddPiece(
			225.0, 300.0, 0.0, 300.0, 231.0, 255.0, DestructionProfiles::Timber, false);
		const int32 MidPurlinL = AddPiece(
			75.0, 125.0, 0.0, 300.0, 261.0, 285.0, DestructionProfiles::Timber, false);
		const int32 MidPurlinR = AddPiece(
			175.0, 225.0, 0.0, 300.0, 261.0, 285.0, DestructionProfiles::Timber, false);
		const int32 Ridge = AddPiece(
			125.0, 175.0, 0.0, 300.0, 291.0, 315.0, DestructionProfiles::Timber, false);

		const int32 PostL = AddPiece(
			55.0, 85.0, 328.0, 352.0, 0.0, 200.0, DestructionProfiles::Timber, true);
		const int32 PostR = AddPiece(
			215.0, 245.0, 328.0, 352.0, 0.0, 200.0, DestructionProfiles::Timber, true);
		const int32 Overhang = AddPiece(
			40.0, 260.0, 301.0, 451.0, 201.0, 221.0, DestructionProfiles::Timber, false);

		/*
		 * THE CLEAT: a narrow central Timber bracket, X[140,160] (20 cm wide, centred on X=150),
		 * bonded to the DoorHeader's front face across a 1 cm mortar joint (its back sits at Y=301,
		 * the header ending at Y=300) and projecting to Y=310, 10 cm past the wall. The overhang laps
		 * ONLY this cleat — a Z-normal screwed tie over X[140,160] x Y[301,310] = 180 cm2 — so the
		 * withdrawal reaction that holds the cantilever's back down acts on a HALF-WIDTH of just 10 cm
		 * in X. That narrow X-couple arm is the whole point: it cannot answer the X-torsion a lost post
		 * throws at the overhang, which is why pulling either post drops the porch.
		 */
		const int32 Cleat = AddPiece(
			140.0, 160.0, 301.0, 310.0, 176.0, 200.0, DestructionProfiles::Timber, false);

		const int32 AllPieces[] = {
			BackWall, RightWall, LeftPier, RightPier, DoorHeader, Sill, WinJambBack, WinJambFront,
			WinLintel, FGableBase, FGableMid, FGableApex, BGableBase, BGableMid, BGableApex,
			EavesPurlinL, EavesPurlinR, MidPurlinL, MidPurlinR, Ridge, PostL, PostR, Overhang, Cleat };

		for (const int32 Piece : AllPieces)
		{
			if (Piece == INDEX_NONE)
			{
				return false;
			}
		}

		/*
		 * THE JOINTS, EACH THROUGH MakeInterface — the same door the wall and the 2D shed use. Every
		 * bearing names the LOWER piece as A and the UPPER as B, so the axis-of-separation normal
		 * (oriented by B) reads as a bed BENEATH the piece it carries. MakeInterface refuses any pair
		 * that does not share a face on exactly one axis, so a mis-sized piece is caught here.
		 */
		const auto Join =
			[&](int32 A, int32 B, const FConnectionStrength& Strength) -> bool
		{
			FConnection Connection;

			if (!MakeInterface(
					A, Laid.Boxes[A], B, Laid.Boxes[B], JointThicknessCm, Strength, Connection))
			{
				return false;
			}

			return Laid.Structure.AddConnection(Connection) != INDEX_NONE;
		};

		/*
		 * DOOR (2) and WINDOW (4) — the two lintels bear on their piers/jambs through compression-only
		 * DryStone beds. CORNERS (4) close the box across the walls' out-of-plane (Y-normal) faces with
		 * bonded mortar; all four are grounded-grounded, so the oracle skips them — they are closure,
		 * not structure. GABLES (6) stack the stepped courses on bonded mortar beds. ROOF (10) rests
		 * each purlin on its back and front gable shoulder through DryStone. PORCH (4): a CANTILEVER tied
		 * by the narrow central Cleat. The overhang is carried PRIMARILY by its two posts (DryStone
		 * bearings); a Z-normal Screw TIE (Cleat-Overhang, 180 cm2) holds its back down in WITHDRAWAL, and
		 * a Y-normal mortar ANCHOR (DoorHeader-Cleat, 480 cm2) bonds the cleat to the wall. The overhang's
		 * weight sits FORWARD of the post line (centroid Y=376 > posts Y=340), so the tie carries no shear.
		 * Its restoring couple about the X axis is that withdrawal force acting at the cleat's 10 cm
		 * HALF-WIDTH, far too small to answer the X-torsion a lost post throws at the load line: pull either
		 * post and the overhang tips toward the gap and drops. A WIDE bed would have held (its half-width
		 * supplies an enormous couple), which is exactly why the tie is a narrow central cleat.
		 */
		if (!Join(LeftPier, DoorHeader, DestructionProfiles::DryStone)
			|| !Join(RightPier, DoorHeader, DestructionProfiles::DryStone)
			|| !Join(Sill, WinJambBack, DestructionProfiles::DryStone)
			|| !Join(Sill, WinJambFront, DestructionProfiles::DryStone)
			|| !Join(WinJambBack, WinLintel, DestructionProfiles::DryStone)
			|| !Join(WinJambFront, WinLintel, DestructionProfiles::DryStone)
			|| !Join(Sill, BackWall, DestructionProfiles::GeneralPurposeMortar)
			|| !Join(Sill, LeftPier, DestructionProfiles::GeneralPurposeMortar)
			|| !Join(RightWall, BackWall, DestructionProfiles::GeneralPurposeMortar)
			|| !Join(RightWall, RightPier, DestructionProfiles::GeneralPurposeMortar)
			|| !Join(DoorHeader, FGableBase, DestructionProfiles::GeneralPurposeMortar)
			|| !Join(FGableBase, FGableMid, DestructionProfiles::GeneralPurposeMortar)
			|| !Join(FGableMid, FGableApex, DestructionProfiles::GeneralPurposeMortar)
			|| !Join(BackWall, BGableBase, DestructionProfiles::GeneralPurposeMortar)
			|| !Join(BGableBase, BGableMid, DestructionProfiles::GeneralPurposeMortar)
			|| !Join(BGableMid, BGableApex, DestructionProfiles::GeneralPurposeMortar)
			|| !Join(FGableBase, EavesPurlinL, DestructionProfiles::DryStone)
			|| !Join(BGableBase, EavesPurlinL, DestructionProfiles::DryStone)
			|| !Join(FGableBase, EavesPurlinR, DestructionProfiles::DryStone)
			|| !Join(BGableBase, EavesPurlinR, DestructionProfiles::DryStone)
			|| !Join(FGableMid, MidPurlinL, DestructionProfiles::DryStone)
			|| !Join(BGableMid, MidPurlinL, DestructionProfiles::DryStone)
			|| !Join(FGableMid, MidPurlinR, DestructionProfiles::DryStone)
			|| !Join(BGableMid, MidPurlinR, DestructionProfiles::DryStone)
			|| !Join(FGableApex, Ridge, DestructionProfiles::DryStone)
			|| !Join(BGableApex, Ridge, DestructionProfiles::DryStone)
			|| !Join(PostL, Overhang, DestructionProfiles::DryStone)
			|| !Join(PostR, Overhang, DestructionProfiles::DryStone)
			|| !Join(DoorHeader, Cleat, DestructionProfiles::GeneralPurposeMortar)
			|| !Join(Cleat, Overhang, DestructionProfiles::Screw))
		{
			return false;
		}

		/*
		 * THE 3D FLAG routes this structure to the Dim3D pose and lifts the bridge's Y-normal refusal,
		 * so the four out-of-plane (Y-normal) corner joints reach the 3D LP rather than being rejected.
		 */
		Laid.Structure.SetThreeDimensional(true);

		OutLayout = MoveTemp(Laid);

		return true;
	}
}
