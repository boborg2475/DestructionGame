// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/DestructionShed3D.h"

#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

/*
 * File-local names sit in the named namespace, like Core/DestructionShed and Core/Corbel: an
 * anonymous namespace is private to a translation unit rather than to a file, and a unity build
 * merges many files into one, so two colliding file-local names are a hard compile error
 * between files that never refer to each other. See CURRENT_STATE.md.
 */
namespace DestructionShed3D
{
	using namespace DestructionLayout;

	bool Build(const FShed3DSpec& Spec, DestructionLayout::FBrickLayout& OutLayout)
	{
		/*
		 * Emptied first and filled last: a refused spec must leave a caller who ignored the return
		 * value with nothing, not a partial lay. Guards are written `!(x > 0.0)`, never `x <= 0.0`
		 * — every comparison against NaN is false, so `<= 0.0` would let a NaN dimension slip
		 * through and lay a shed of NaN-sized boxes whose every joint reads intact.
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
		 * Coordinates, worked from the spec once. X is width, Y is depth (into the door), Z is
		 * height. The side walls fit between the back and front walls with a JointThicknessCm gap
		 * on Y, so each corner is a genuine face-sharing joint whose normal points along +/-Y. The
		 * roof, overhang and post leave the same Z gap under the beam they carry — the separation
		 * MakeInterface reads as a bed joint.
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

		/* One door for every piece: a box from its spans, mass via the shared PieceMassKg, and the
		 * material recorded so cross-material physics survives into play. The handle is the box
		 * index (FBrickLayout's parallel-array contract), appended in the same breath as the piece. */
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
		 * Seven pieces: four grounded ClayBrick walls, a free Timber roof beam and overhang, and a
		 * grounded Timber post under the overhang's front.
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
		 * Eight joints through MakeInterface. The four corners are bonded GeneralPurposeMortar across
		 * the walls' shared out-of-plane (Y-normal) faces; the roof and post bearings are
		 * compression-only DryStone; the fixing is a tension-capable Screw. Each bearing names the
		 * lower piece as A so the normal orients as a bed beneath the piece it carries; a mis-sized
		 * piece is caught here rather than solved as a healthy joint.
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
		 * The 3D flag routes this structure to the Dim3D pose and lifts the bridge's Y-normal
		 * refusal, so the four out-of-plane corner joints reach the 3D LP rather than being rejected.
		 */
		Laid.Structure.SetThreeDimensional(true);

		OutLayout = MoveTemp(Laid);

		return true;
	}

	bool BuildRecognizable(DestructionLayout::FBrickLayout& OutLayout)
	{
		/*
		 * Emptied first and filled last: a path that gives up partway must leave a caller who
		 * ignored the return value with nothing, not a half-built shed. There is no spec to guard —
		 * dimensions are canonical, hardcoded and pinned by the test's local constants — so the
		 * fail-closed points are AddPiece and Join, which refuse a degenerate box or a non-face pair.
		 */
		OutLayout = FBrickLayout();

		const double JointThicknessCm = 1.0;

		FBrickLayout Laid;

		// One door for every piece — the same lambda Build uses. See Build's AddPiece above.
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
		 * The 24 pieces: walls and door base (Z < 200), the window course in the left wall, the two
		 * stepped gable ends, the timber roof of purlins and a ridge, and the porch — two posts, an
		 * overhang, and the narrow central Cleat tying the overhang's back down to the wall. ClayBrick
		 * masonry, Timber lintels/roof/porch. Grounded: the four walls' feet, both door piers, the
		 * sill, and the two posts; the cleat is not grounded — the wall holds it.
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
		 * The cleat: a narrow central Timber bracket, X[140,160] (20 cm wide), bonded to the
		 * DoorHeader's front face across a 1 cm mortar joint and projecting 10 cm past the wall. The
		 * overhang laps only this cleat — a Z-normal screwed tie of 180 cm2 — so the withdrawal
		 * reaction holding the cantilever's back down acts on just a 10 cm half-width in X. That
		 * narrow couple arm cannot answer the X-torsion a lost post throws at the overhang, which is
		 * why pulling either post drops the porch.
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
		 * The joints, each through MakeInterface — the same door the wall and 2D shed use. Every
		 * bearing names the lower piece as A and the upper as B, so the normal orients as a bed
		 * beneath the piece it carries. MakeInterface refuses any pair that does not share a face on
		 * exactly one axis, so a mis-sized piece is caught here.
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
		 * Door (2) and window (4): lintels bear on piers/jambs through DryStone. Corners (4) close
		 * the box across the walls' out-of-plane (Y-normal) faces with bonded mortar; all four are
		 * grounded-grounded, so the oracle skips them as closure, not structure. Gables (6) stack on
		 * bonded mortar beds. Roof (10) rests each purlin on its gable shoulders through DryStone.
		 * Porch (4): the overhang is carried primarily by its two posts; the Cleat withdrawal tie and
		 * wall anchor are the mechanism described above, carrying no shear since the overhang's
		 * weight sits forward of the post line.
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
		 * The 3D flag routes this structure to the Dim3D pose and lifts the bridge's Y-normal
		 * refusal, so the four out-of-plane corner joints reach the 3D LP rather than being rejected.
		 */
		Laid.Structure.SetThreeDimensional(true);

		OutLayout = MoveTemp(Laid);

		return true;
	}

	/*
	 * One opening in a wall panel, as the coordinates the panel lays around. A brick is omitted in
	 * the clear gap over the opening's course band, and separately in the lintel band — the one
	 * course the timber lintel occupies, cleared so the lintel can bear on the flanking piers/jambs
	 * and the course above can bed back onto it. The two are kept apart because the lintel footprint
	 * is wider than the clear gap. A default-constructed opening omits nothing: its course ranges
	 * are empty (`Hi < Lo`), so a windowless / doorless wall passes it unchanged.
	 */
	struct FWallOpening
	{
		int32 GapCourseLo = 0;
		int32 GapCourseHi = -1;
		double GapRunLoCm = 0.0;
		double GapRunHiCm = -1.0;

		int32 LintelBandCourse = -1;
		double LintelRunLoCm = 0.0;
		double LintelRunHiCm = -1.0;
	};

	bool BuildRealistic(DestructionLayout::FBrickLayout& OutLayout)
	{
		using namespace DestructionProfiles;

		/*
		 * Emptied first and filled last, as Build and BuildRecognizable do. There is no spec to
		 * guard: dimensions are canonical, hardcoded and pinned by the test; the fail-closed points
		 * are AddPiece (a degenerate box) and MakeInterface (a non-face pair), with bLayoutValid
		 * carrying either refusal out to a single closing check.
		 */
		OutLayout = FBrickLayout();

		/*
		 * The real brick and its coordinating grid. A 21.5 x 10.25 x 6.5 cm brick on a 1 cm mortar
		 * joint sits on a 22.5 (pitch along a course) x 7.5 (course pitch up) grid, and a running
		 * bond offsets alternate courses by half a cell, 11.25. A half bat — what is left of a brick
		 * when a joint is taken out and the remainder halved — fills the half cell a flush end
		 * leaves, so every course finishes flush without any brick but a full one or a half bat.
		 */
		const double BrickLenCm = 21.5;
		const double WytheCm = 10.25;
		const double CourseCm = 6.5;
		const double JointCm = 1.0;
		const double PitchCm = BrickLenCm + JointCm;
		const double CoursePitchCm = CourseCm + JointCm;
		const double BondOffsetCm = PitchCm / 2.0;
		const double HalfBatLenCm = (BrickLenCm - JointCm) / 2.0;
		const int32 NumCourses = 16;

		/*
		 * The footprint. Outer box X[0,180], Y[0,134]; every wall one wythe thick. Front and back
		 * walls run along X; side walls run along Y, fitting between them one bond offset in. A
		 * five-brick side run ends at 122.75, so the back corner closes on the same 1 cm Y-normal
		 * mortar joint the front corner makes, giving a depth of 134 — the only whole coordinating
		 * run that lets the side bricks meet both end walls on a genuine joint out of the X-Z plane a
		 * 2D section cannot hold. An outer 140 leaves a 7 cm gap and the box never closes.
		 */
		const double SideRunStartCm = BondOffsetCm;
		const int32 FrontBackBricks = 8;
		const int32 SideBricks = 5;
		const double BackWallY0Cm = SideRunStartCm + SideBricks * PitchCm;
		const double RightWallX0Cm = 180.0 - WytheCm;

		FBrickLayout Laid;
		bool bLayoutValid = true;

		/*
		 * One door for every piece, as above, but a refused piece trips bLayoutValid rather than
		 * returning early, so the closing check is the single fail-closed exit.
		 */
		const auto AddPiece =
			[&](double LeftXCm, double RightXCm, double BackYCm, double FrontYCm,
				double BottomZCm, double TopZCm,
				const FMaterialProfile& Material, bool bGrounded) -> int32
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
				bLayoutValid = false;
				return INDEX_NONE;
			}

			Laid.Boxes.Add(Box);
			Laid.Structure.SetPieceMaterial(Piece, &Material);

			return Piece;
		};

		/*
		 * A half-open overlap with a hair of tolerance, so a brick whose end sits exactly on an
		 * opening edge stays as the jamb rather than being cleared with the gap — giving the lintel
		 * a pier to bear on.
		 */
		const auto StrictlyOverlaps =
			[](double LoA, double HiA, double LoB, double HiB) -> bool
		{
			const double Eps = 1.0e-6;
			return LoA < HiB - Eps && HiA > LoB + Eps;
		};

		/*
		 * Lay one running-bond wall panel. bRunsX chooses whether courses run along X (front/back
		 * walls) or Y (side walls); the thin span is the wythe on the other horizontal axis. Even
		 * courses are full bricks from RunStart; odd courses are the same bond offset in, with a half
		 * bat closing each end flush, so a full brick above always laps two below. A brick is skipped
		 * where the opening clears it. Course 0 is grounded; joints are formed once, below.
		 */
		const auto LayWall =
			[&](bool bRunsX, double ThinLoCm, double ThinHiCm, double RunStartCm, int32 FullPerEven,
				const FWallOpening& Opening)
		{
			const double RunEndCm = RunStartCm + FullPerEven * PitchCm - JointCm;

			for (int32 Course = 0; Course < NumCourses; ++Course)
			{
				const double BottomZCm = Course * CoursePitchCm;
				const double TopZCm = BottomZCm + CourseCm;
				const bool bGrounded = (Course == 0);

				TArray<TPair<double, double>> Spans;

				if ((Course % 2) == 0)
				{
					for (int32 Brick = 0; Brick < FullPerEven; ++Brick)
					{
						const double LoCm = RunStartCm + Brick * PitchCm;
						Spans.Add(TPair<double, double>(LoCm, LoCm + BrickLenCm));
					}
				}
				else
				{
					Spans.Add(TPair<double, double>(RunStartCm, RunStartCm + HalfBatLenCm));
					for (int32 Brick = 0; Brick < FullPerEven - 1; ++Brick)
					{
						const double LoCm = RunStartCm + BondOffsetCm + Brick * PitchCm;
						Spans.Add(TPair<double, double>(LoCm, LoCm + BrickLenCm));
					}
					Spans.Add(TPair<double, double>(RunEndCm - HalfBatLenCm, RunEndCm));
				}

				for (const TPair<double, double>& Span : Spans)
				{
					const bool bInGap =
						Course >= Opening.GapCourseLo && Course <= Opening.GapCourseHi
						&& StrictlyOverlaps(Span.Key, Span.Value, Opening.GapRunLoCm, Opening.GapRunHiCm);
					const bool bInLintelBand =
						Course == Opening.LintelBandCourse
						&& StrictlyOverlaps(Span.Key, Span.Value, Opening.LintelRunLoCm, Opening.LintelRunHiCm);

					if (bInGap || bInLintelBand)
					{
						continue;
					}

					if (bRunsX)
					{
						AddPiece(Span.Key, Span.Value, ThinLoCm, ThinHiCm, BottomZCm, TopZCm, ClayBrick, bGrounded);
					}
					else
					{
						AddPiece(ThinLoCm, ThinHiCm, Span.Key, Span.Value, BottomZCm, TopZCm, ClayBrick, bGrounded);
					}
				}
			}
		};

		/*
		 * The door, in the front wall: a clear gap X[57.5,122.5] over the bottom twelve courses
		 * (Z[0,90]); the lintel occupies course 12 (Z[90,96.5]) with footprint X[50,130] lapping a
		 * pier either side. The gap edges sit on the coordinating grid so a course-11 pier brick
		 * reaches X=55.25 / X=123.75 — the bearing the lintel drops onto. The window, in the left
		 * wall: a sill of six courses, a clear gap Y[66.5,90] over courses 6-11, its lintel on course
		 * 12 with footprint Y[61.5,95] lapping a jamb brick either side.
		 */
		FWallOpening DoorOpening;
		DoorOpening.GapCourseLo = 0;
		DoorOpening.GapCourseHi = 11;
		DoorOpening.GapRunLoCm = 57.5;
		DoorOpening.GapRunHiCm = 122.5;
		DoorOpening.LintelBandCourse = 12;
		DoorOpening.LintelRunLoCm = 50.0;
		DoorOpening.LintelRunHiCm = 130.0;

		FWallOpening WindowOpening;
		WindowOpening.GapCourseLo = 6;
		WindowOpening.GapCourseHi = 11;
		WindowOpening.GapRunLoCm = 66.5;
		WindowOpening.GapRunHiCm = 90.0;
		WindowOpening.LintelBandCourse = 12;
		WindowOpening.LintelRunLoCm = 61.5;
		WindowOpening.LintelRunHiCm = 95.0;

		const FWallOpening NoOpening;

		/*
		 * Four walls closing the box. Front (door) and back run along X; left (window) and right run
		 * along Y between them, starting one bond offset in from the front wall so their front-end
		 * bricks stand a JointCm clear of it — the corner mortar joint.
		 */
		LayWall(true, 0.0, WytheCm, 0.0, FrontBackBricks, DoorOpening);
		LayWall(true, BackWallY0Cm, BackWallY0Cm + WytheCm, 0.0, FrontBackBricks, NoOpening);
		LayWall(false, 0.0, WytheCm, SideRunStartCm, SideBricks, WindowOpening);
		LayWall(false, RightWallX0Cm, RightWallX0Cm + WytheCm, SideRunStartCm, SideBricks, NoOpening);

		/*
		 * The two Timber lintels, each a real board section (smallest half-extent the 3.25 cm course
		 * height) spanning its opening on course 12, bearing on the pier/jamb tops one JointCm below
		 * and carrying the courses that bed back onto it — both formed as bed joints by the sweep
		 * below, so a lintel that failed to overlap a bearing would read unsupported rather than fine.
		 */
		const int32 DoorLintel = AddPiece(
			DoorOpening.LintelRunLoCm, DoorOpening.LintelRunHiCm, 0.0, WytheCm, 90.0, 96.5, Timber, false);
		const int32 WindowLintel = AddPiece(
			0.0, WytheCm, WindowOpening.LintelRunLoCm, WindowOpening.LintelRunHiCm, 90.0, 96.5, Timber, false);

		/*
		 * The stepped gables. Both gable ends — front (door side) and back wall — continue ClayBrick
		 * courses above the eaves (course 15 top Z = 119), each higher course stepping in one brick
		 * pitch (22.5) per side toward the box centre, narrowing symmetrically from an 8-brick course
		 * 16 to a 2-brick apex on course 19 — keeping every course's centroid over the one below so
		 * the corbel cannot overturn. The gables are free; the eaves course beneath is the grounded path.
		 */
		const int32 GableBaseCourse = NumCourses;
		const int32 GableSteps = 4;

		const auto LayGable =
			[&](double ThinLoCm, double ThinHiCm)
		{
			for (int32 Step = 0; Step < GableSteps; ++Step)
			{
				const int32 Course = GableBaseCourse + Step;
				const int32 NumBricks = FrontBackBricks - 2 * Step;
				const double StartXCm = Step * PitchCm;
				const double BottomZCm = Course * CoursePitchCm;
				const double TopZCm = BottomZCm + CourseCm;

				for (int32 Brick = 0; Brick < NumBricks; ++Brick)
				{
					const double LoXCm = StartXCm + Brick * PitchCm;
					AddPiece(LoXCm, LoXCm + BrickLenCm, ThinLoCm, ThinHiCm, BottomZCm, TopZCm, ClayBrick, false);
				}
			}
		};

		LayGable(0.0, WytheCm);
		LayGable(BackWallY0Cm, BackWallY0Cm + WytheCm);

		/*
		 * The timber gable roof. Five board members span the full depth and bear on both gable ends
		 * as a simply-supported beam — centroid between the two bearings, so it cannot overturn —
		 * stepping up onto successively higher gable shoulders (eaves purlins on course-16, mid
		 * purlins on course-18, ridge on the apex course-19). The ~8.5 cm gap over the low side walls
		 * is far more than a joint, so no spurious bearing forms there. Load path: roof member ->
		 * gable shoulder -> gable courses -> eaves wall -> ground.
		 */
		const double RoofDepthFrontYCm = BackWallY0Cm + WytheCm;
		const double RoofThicknessCm = 5.0;

		/* The top Z of a gable course, and the bottom Z of a board bearing one JointCm above it. */
		const auto ShoulderBottomZ =
			[&](int32 Course) -> double
		{
			return Course * CoursePitchCm + CourseCm + JointCm;
		};

		const double EavesBottomZCm = ShoulderBottomZ(GableBaseCourse);           // course 16 shoulder
		const double MidBottomZCm = ShoulderBottomZ(GableBaseCourse + 2);         // course 18 shoulder
		const double RidgeBottomZCm = ShoulderBottomZ(GableBaseCourse + 3);       // course 19 apex shoulder

		const double RunEndXCm = FrontBackBricks * PitchCm - JointCm;             // 179 — the gable run end

		const int32 EavesPurlinL = AddPiece(
			0.0, PitchCm, 0.0, RoofDepthFrontYCm,
			EavesBottomZCm, EavesBottomZCm + RoofThicknessCm, Timber, false);
		const int32 EavesPurlinR = AddPiece(
			RunEndXCm - PitchCm, RunEndXCm, 0.0, RoofDepthFrontYCm,
			EavesBottomZCm, EavesBottomZCm + RoofThicknessCm, Timber, false);

		const int32 MidPurlinL = AddPiece(
			2.0 * PitchCm, 3.0 * PitchCm, 0.0, RoofDepthFrontYCm,
			MidBottomZCm, MidBottomZCm + RoofThicknessCm, Timber, false);
		const int32 MidPurlinR = AddPiece(
			5.0 * PitchCm - JointCm, 6.0 * PitchCm - JointCm, 0.0, RoofDepthFrontYCm,
			MidBottomZCm, MidBottomZCm + RoofThicknessCm, Timber, false);

		const int32 Ridge = AddPiece(
			3.0 * PitchCm, 3.0 * PitchCm + 2.0 * PitchCm - JointCm, 0.0, RoofDepthFrontYCm,
			RidgeBottomZCm, RidgeBottomZCm + RoofThicknessCm, Timber, false);

		if (!bLayoutValid || DoorLintel == INDEX_NONE || WindowLintel == INDEX_NONE
			|| EavesPurlinL == INDEX_NONE || EavesPurlinR == INDEX_NONE
			|| MidPurlinL == INDEX_NONE || MidPurlinR == INDEX_NONE || Ridge == INDEX_NONE)
		{
			return false;
		}

		/*
		 * The joints, each through MakeInterface — the one door that owns areas, normals and
		 * rectangles, and refuses any pair that does not share a face on exactly one axis by exactly
		 * the joint thickness. Because it refuses everything else, the graph forms by offering every
		 * pair once and keeping what MakeInterface accepts: bed joints, perpends, corner joints and
		 * lintel bearings all fall out, while diagonals, edge-touches and far walls are rejected. The
		 * lower piece is named A so a bed joint's normal reads as a bed beneath the piece it carries;
		 * brick-to-brick is bonded mortar, a joint touching timber is compression-only dry bearing —
		 * the same per-contact authoring the coarser sheds do, decided from the materials.
		 */
		const int32 NumPieces = Laid.Structure.NumPieces();
		for (int32 A = 0; A < NumPieces; ++A)
		{
			for (int32 B = A + 1; B < NumPieces; ++B)
			{
				const FMaterialProfile* MaterialA = Laid.Structure.GetPiece(A).Material;
				const FMaterialProfile* MaterialB = Laid.Structure.GetPiece(B).Material;
				const bool bTimberBearing = (MaterialA == &Timber) || (MaterialB == &Timber);
				const FConnectionStrength& Strength = bTimberBearing ? DryStone : GeneralPurposeMortar;

				int32 Lower = A;
				int32 Upper = B;
				if (Laid.Boxes[B].CentreCm.Z < Laid.Boxes[A].CentreCm.Z)
				{
					Lower = B;
					Upper = A;
				}

				FConnection Connection;
				if (MakeInterface(
						Lower, Laid.Boxes[Lower], Upper, Laid.Boxes[Upper], JointCm, Strength, Connection))
				{
					/*
					 * A brick-brick joint whose normal is not vertical is a perpend — an in-course head
					 * joint or a wall-corner joint — which real masonry treats as the weak link
					 * (owner-approved item 6b), authored with the knocked-down bond row while horizontal
					 * beds keep the strong GeneralPurposeMortar. MakeInterface emits only axis-aligned
					 * normals, so |Z| is exactly 1 for a bed and 0 for a perpend or corner; timber
					 * bearings authored DryStone above are left untouched.
					 */
					if (!bTimberBearing
						&& FMath::Abs(Connection.InterfaceNormal.GetSafeNormal().Z) < 0.5)
					{
						Connection.Strength = GeneralPurposeMortarPerpend;
					}

					Laid.Structure.AddConnection(Connection);
				}
			}
		}

		/*
		 * The porch — a door canopy on two Timber posts, laid after the shell sweep so its four
		 * joints are authored explicitly: a swept timber pair would get compression-only DryStone,
		 * but the overhang fixing and wall anchor must be tension-capable Screw fasteners (mortar
		 * does not bond to a timber cleat), so these pieces go past NumPieces and are joined by hand.
		 *
		 * Geometry: the front wall's outer face is Y = 0, so the porch cantilevers out in negative Y.
		 * Two grounded posts (Y[-25,-15]) flank the door gap. A free plank overhang (Y[-70,-2],
		 * Z[105,110]) bears on both post tops, cantilevering 70 cm; its weight centroid (Y = -36)
		 * sits outboard of the post line (Y = -20), tipping it back-up, which a narrow central Timber
		 * cleat (X[85,95], Z[97.5,104]) holds down with a Z-normal Screw withdrawal tie, itself
		 * anchored to a course-13 front-wall brick by a Y-normal Screw withdrawal joint.
		 *
		 * Above the block cap the router splits the overhang's weight among the two posts and the
		 * cleat, but the cleat is only 10 cm wide on purpose: its restoring couple is too small to
		 * answer the X-torsion a lost post throws at the now-asymmetric overhang, so the posts are
		 * the genuine support and pulling one drops the porch.
		 */
		const int32 PostL = AddPiece(50.0, 60.0, -25.0, -15.0, 0.0, 104.0, Timber, true);
		const int32 PostR = AddPiece(120.0, 130.0, -25.0, -15.0, 0.0, 104.0, Timber, true);
		const int32 PorchOverhang = AddPiece(50.0, 130.0, -70.0, -2.0, 105.0, 110.0, Timber, false);
		const int32 Cleat = AddPiece(85.0, 95.0, -11.0, -1.0, 97.5, 104.0, Timber, false);

		/*
		 * The front-wall brick the cleat anchors to — the course-13 stretcher over the door
		 * (Z[97.5,104], the first full-masonry course above the door lintel band), found by the point
		 * at the door centre on that course rather than by handle, so the exact bond the wall producer
		 * chose does not matter. Its outer face stands one JointCm off the cleat's back, so
		 * MakeInterface reads a Y-normal Screw anchor fixing the cleat to real, load-bearing masonry.
		 */
		int32 CleatWallBrick = INDEX_NONE;
		{
			const FVector AnchorPtCm(90.0, 5.125, 100.75);
			for (int32 P = 0; P < Laid.Structure.NumPieces(); ++P)
			{
				if (Laid.Structure.GetPiece(P).Material != &ClayBrick || !Laid.Boxes.IsValidIndex(P))
				{
					continue;
				}
				const FVector LoCm = Laid.Boxes[P].CentreCm - Laid.Boxes[P].ExtentCm;
				const FVector HiCm = Laid.Boxes[P].CentreCm + Laid.Boxes[P].ExtentCm;
				if (AnchorPtCm.X >= LoCm.X && AnchorPtCm.X <= HiCm.X
					&& AnchorPtCm.Y >= LoCm.Y && AnchorPtCm.Y <= HiCm.Y
					&& AnchorPtCm.Z >= LoCm.Z && AnchorPtCm.Z <= HiCm.Z)
				{
					CleatWallBrick = P;
					break;
				}
			}
		}

		if (!bLayoutValid || PostL == INDEX_NONE || PostR == INDEX_NONE
			|| PorchOverhang == INDEX_NONE || Cleat == INDEX_NONE || CleatWallBrick == INDEX_NONE)
		{
			return false;
		}

		/*
		 * The four porch joints, each through MakeInterface with the lower piece named A so a bed
		 * normal reads as a bed beneath the piece it carries. Two Z-normal DryStone bearings carry
		 * the overhang on the post tops (~100 cm2 each); a Z-normal Screw tie (~90 cm2) holds its
		 * back down in withdrawal; a Y-normal Screw anchor (~65 cm2) fixes the cleat to the wall.
		 */
		const auto PorchJoin =
			[&](int32 A, int32 B, const FConnectionStrength& Strength) -> bool
		{
			FConnection Connection;

			if (!MakeInterface(A, Laid.Boxes[A], B, Laid.Boxes[B], JointCm, Strength, Connection))
			{
				return false;
			}

			return Laid.Structure.AddConnection(Connection) != INDEX_NONE;
		};

		if (!PorchJoin(PostL, PorchOverhang, DryStone)
			|| !PorchJoin(PostR, PorchOverhang, DryStone)
			|| !PorchJoin(Cleat, PorchOverhang, Screw)
			|| !PorchJoin(Cleat, CleatWallBrick, Screw))
		{
			return false;
		}

		/*
		 * The 3D flag routes this structure to the Dim3D pose and lifts the bridge's Y-normal
		 * refusal, so the Y-normal perpends and out-of-plane corner joints are honoured rather than
		 * rejected. Above the block cap the router, not the LP, is the break authority — but the flag
		 * still selects the genuinely-3D geometry every reader downstream sees.
		 */
		Laid.Structure.SetThreeDimensional(true);

		OutLayout = MoveTemp(Laid);

		return true;
	}
}
