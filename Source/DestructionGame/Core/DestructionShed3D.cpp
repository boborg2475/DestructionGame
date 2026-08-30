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

	/*
	 * ONE OPENING IN A WALL PANEL, AS THE COORDINATES THE PANEL LAYS AROUND. A brick is omitted where
	 * it falls in the CLEAR GAP over the opening's course band, and separately where it falls in the
	 * LINTEL BAND — the one course the timber lintel occupies, which the masonry clears so the lintel
	 * can bear on the flanking piers/jambs and the course above can bed back onto it. The two are
	 * kept apart because the lintel footprint is wider than the clear gap (it laps onto the piers),
	 * so the same interval cannot describe both. A default-constructed opening omits nothing: its
	 * course ranges are empty (`Hi < Lo`), so a windowless / doorless wall passes it unchanged.
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
		 * EMPTIED FIRST AND FILLED LAST, exactly as Build and BuildRecognizable do. A path that gives
		 * up partway must leave a caller who ignored the return value with nothing, rather than a
		 * half-built shell whose missing joints would read as a healthy structure.
		 *
		 * There is no spec to guard: the realistic shell's dimensions are canonical, hardcoded here to
		 * the file-header geometry and pinned by the test. Every coordinate is a positive literal, so
		 * the NaN-safe `!(x > 0.0)` guarding Build has nothing to reject; the fail-closed points are
		 * AddPiece (a degenerate box) and MakeInterface (a non-face pair), and bLayoutValid below
		 * carries either refusal out to a single closing check.
		 */
		OutLayout = FBrickLayout();

		/*
		 * THE REAL BRICK AND ITS COORDINATING GRID (file header). A 21.5 x 10.25 x 6.5 cm brick on a
		 * 1 cm mortar joint sits on a 22.5 (pitch along a course) x 7.5 (course pitch up) grid, and a
		 * running bond offsets alternate courses by half a cell, 11.25. A half bat — what is left of a
		 * brick when a joint is taken out and the remainder halved — fills the half cell a flush end
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
		 * THE FOOTPRINT (file header). Outer box X[0,180], Y[0,134]; every wall one wythe (10.25)
		 * thick. The front and back walls run along X; the side walls run along Y and fit BETWEEN
		 * them, starting one bond offset in. A five-brick side run ends at SideRunStartCm +
		 * SideBricks*PitchCm - JointCm = 122.75, so the back wall's inner face sits one JointCm beyond
		 * it at SideRunStartCm + SideBricks*PitchCm = 123.75 and the back corner closes on the SAME
		 * 1 cm Y-normal mortar joint the front corner makes (front inner face at 10.25, side start at
		 * 11.25). The depth is thus 21.5 + SideBricks*22.5 = 134 — the only whole coordinating run that
		 * lets the side bricks meet BOTH end walls on a genuine joint out of the X-Z plane that a 2D
		 * section cannot hold. An outer 140 leaves a 7 cm gap at the back and the box never closes.
		 */
		const double SideRunStartCm = BondOffsetCm;
		const int32 FrontBackBricks = 8;
		const int32 SideBricks = 5;
		const double BackWallY0Cm = SideRunStartCm + SideBricks * PitchCm;
		const double RightWallX0Cm = 180.0 - WytheCm;

		FBrickLayout Laid;
		bool bLayoutValid = true;

		/*
		 * ONE DOOR FOR EVERY PIECE — the same lambda Build and BuildRecognizable use: an axis-aligned
		 * box from its X, Y and Z spans, the mass derived from that same box via the shared PieceMassKg
		 * so a piece cannot weigh a different size than it sits, and the authored material recorded. The
		 * handle IS the box index (FBrickLayout's parallel-array contract), so the box is appended in the
		 * same breath. A refused piece trips bLayoutValid rather than returning early, so the closing
		 * check is the single fail-closed exit.
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
		 * A HALF-OPEN OVERLAP with a hair of tolerance, so a brick whose end sits EXACTLY on an opening
		 * edge stays as the jamb rather than being cleared with the gap. The masonry either side of an
		 * opening is exactly the run of bricks whose spans touch but do not cross the gap, which is what
		 * gives the lintel a pier to bear on.
		 */
		const auto StrictlyOverlaps =
			[](double LoA, double HiA, double LoB, double HiB) -> bool
		{
			const double Eps = 1.0e-6;
			return LoA < HiB - Eps && HiA > LoB + Eps;
		};

		/*
		 * LAY ONE RUNNING-BOND WALL PANEL. bRunsX chooses whether the courses run along X (front/back
		 * walls) or along Y (side walls); the thin span is the single wythe on the other horizontal
		 * axis. Even courses are full bricks from RunStart; odd courses are the same bond offset in,
		 * with a half bat closing each end flush — so both courses span [RunStart, RunEnd] identically
		 * and a full brick above always laps two below. A brick is skipped where the opening clears it.
		 * Course 0 is grounded; nothing is joined here — the whole shell's joints are formed once, below.
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
		 * THE DOOR, in the front wall. A clear gap X[57.5,122.5] rising the bottom twelve courses
		 * (Z[0,90]); the lintel occupies course 12 (Z[90,96.5]) and its footprint X[50,130] laps a
		 * pier either side, so the masonry is cleared to X[50,130] in that one band. The gap edges are
		 * chosen on the coordinating grid so a course-11 pier brick reaches X=55.25 / X=123.75 — the
		 * bearing the lintel drops onto. The WINDOW, in the left wall: a sill of six courses, then a
		 * clear gap Y[66.5,90] over courses 6-11, its lintel on course 12 with footprint Y[61.5,95]
		 * lapping a jamb brick either side.
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
		 * FOUR WALLS CLOSING THE BOX. Front (door) and back run along X; left (window) and right run
		 * along Y between them. The side walls start one bond offset in from the front wall, so their
		 * front-end bricks stand a JointCm clear of it — the corner mortar joint.
		 */
		LayWall(true, 0.0, WytheCm, 0.0, FrontBackBricks, DoorOpening);
		LayWall(true, BackWallY0Cm, BackWallY0Cm + WytheCm, 0.0, FrontBackBricks, NoOpening);
		LayWall(false, 0.0, WytheCm, SideRunStartCm, SideBricks, WindowOpening);
		LayWall(false, RightWallX0Cm, RightWallX0Cm + WytheCm, SideRunStartCm, SideBricks, NoOpening);

		/*
		 * THE TWO TIMBER LINTELS, each a real board section (its smallest half-extent is the 3.25 cm
		 * course height) spanning its opening on course 12. Each bears on the pier/jamb tops one JointCm
		 * below and carries the courses that bed back down onto it — both formed as bed joints by the
		 * sweep below, so a lintel that failed to overlap a bearing would read as unsupported rather
		 * than as fine.
		 */
		const int32 DoorLintel = AddPiece(
			DoorOpening.LintelRunLoCm, DoorOpening.LintelRunHiCm, 0.0, WytheCm, 90.0, 96.5, Timber, false);
		const int32 WindowLintel = AddPiece(
			0.0, WytheCm, WindowOpening.LintelRunLoCm, WindowOpening.LintelRunHiCm, 90.0, 96.5, Timber, false);

		/*
		 * THE STEPPED GABLES (file header, slice 2). Both gable ENDS — the front wall (which carries the
		 * door) and the back wall, each a thin Y band one wythe deep — continue real ClayBrick courses
		 * ABOVE the eaves (course 15 top Z = 119). Each higher course steps IN one brick pitch (22.5) per
		 * side toward the box centre X = 89.5, narrowing symmetrically from an 8-brick course 16 to a
		 * 2-brick apex on course 19. The symmetric narrowing keeps every course's centroid over the course
		 * below, so the corbel cannot overturn; each course is stack-aligned with the one below (a full
		 * overlap), and the lowest beds on the intact eaves course 15, so the joint sweep forms every 1 cm
		 * bed. The bricks are laid ClayBrick and free — the eaves course beneath them is the grounded path.
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
		 * THE TIMBER GABLE ROOF (file header, slice 2). Five real board members run the full depth
		 * Y[0,RoofFrontYCm] and bear on BOTH gable ends as a simply-supported beam — its centroid between
		 * the two bearings, so it cannot overturn — stepping UP onto successively higher gable SHOULDERS
		 * (the part of each gable course the course above steps back from). The eaves purlins sit on the
		 * course-16 shoulder, the mid purlins on the course-18 shoulder, and the ridge caps the apex
		 * course-19 shoulder. Each board's bottom is one JointCm above its shoulder brick top, so the
		 * sweep forms a DryStone bed there; the ~8.5 cm gap over the low side walls is far more than a
		 * joint, so MakeInterface forms no spurious bearing across it. Each is ~5 cm thick — a real board.
		 * Load path: roof member -> gable shoulder -> gable courses -> eaves wall -> ground.
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
		 * THE JOINTS, EACH THROUGH MakeInterface — the ONE door that owns areas, normals and rectangles,
		 * and refuses any pair that does not share a face on exactly one axis by exactly the joint
		 * thickness. Because it refuses everything else, the graph can be formed by OFFERING every pair
		 * once and keeping what MakeInterface accepts: bed joints to the course below, perpends to the
		 * in-course neighbour, the corner joints where perpendicular walls meet, and the lintel bearings
		 * all fall out, while diagonals, edge-touches and the metres between opposite walls are rejected.
		 * The lower piece is named A so a bed joint's normal reads as a bed BENEATH the piece it carries;
		 * a brick-to-brick joint is bonded mortar, a joint touching a timber board is a compression-only
		 * dry bearing — the same per-contact authoring the coarser sheds do, decided from the materials.
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
					Laid.Structure.AddConnection(Connection);
				}
			}
		}

		/*
		 * THE PORCH — A DOOR CANOPY ON TWO TIMBER POSTS (slice 3), laid AFTER the shell sweep so its four
		 * joints are authored EXPLICITLY rather than swept. A swept timber-touching pair would author a
		 * compression-only DryStone bearing, but the wall FIXING must be a tension-capable Screw and the
		 * wall ANCHOR a bonded mortar joint — neither of which the sweep can produce — so the porch pieces
		 * are added past NumPieces (the sweep's captured bound) and joined by hand below.
		 *
		 * THE GEOMETRY. The front wall's OUTER face is Y = 0 and the box interior is +Y, so the porch
		 * cantilevers OUT over the door in NEGATIVE Y. Two GROUNDED 10 x 10 cm posts (PostL X[50,60], PostR
		 * X[120,130], both Y[-25,-15], Z[0,104]) flank the door gap X[57.5,122.5] (centre 90). A free plank
		 * OVERHANG (X[50,130], Y[-70,-2], Z[105,110]) bears on both post tops through 1 cm DryStone beds and
		 * cantilevers 70 cm out; its back stands 2 cm off the wall (Y = -2, wall face Y = 0) so no spurious
		 * wall bearing forms. Its weight centroid Y = -36 sits OUTBOARD of the post line Y = -20, so the
		 * board tips front-down / back-up about the posts; a narrow central Timber CLEAT (X[85,95], Y[-11,-1],
		 * Z[97.5,104]) holds that back-up DOWN with a Z-normal Screw WITHDRAWAL tie (cleat top 104 one joint
		 * under the overhang bottom 105) and is itself anchored to a real course-13 front-wall brick over the
		 * door by a Y-normal mortar joint (cleat back Y = -1 one joint off the wall face Y = 0).
		 *
		 * WHY IT STANDS, AND WHY IT IS POST-DEPENDENT. Above the block cap the router splits the overhang's
		 * weight in compression among the three beds beneath it (two posts + the cleat), so nothing is
		 * stranded. But the cleat is only 10 cm wide on purpose: its restoring couple about the Y axis is the
		 * withdrawal force acting over a 5 cm HALF-WIDTH in X, far too small to answer the X-torsion a LOST
		 * post throws at the now-asymmetric overhang. The posts are therefore the genuine support, so pulling
		 * one drops the porch — the collapse arm of slice 4, NOT cut here.
		 */
		const int32 PostL = AddPiece(50.0, 60.0, -25.0, -15.0, 0.0, 104.0, Timber, true);
		const int32 PostR = AddPiece(120.0, 130.0, -25.0, -15.0, 0.0, 104.0, Timber, true);
		const int32 PorchOverhang = AddPiece(50.0, 130.0, -70.0, -2.0, 105.0, 110.0, Timber, false);
		const int32 Cleat = AddPiece(85.0, 95.0, -11.0, -1.0, 97.5, 104.0, Timber, false);

		/*
		 * THE FRONT-WALL BRICK THE CLEAT ANCHORS TO — the course-13 stretcher over the door (Z[97.5,104],
		 * the first full-masonry course above the door lintel band on course 12), found by the point at the
		 * door centre on that course rather than by handle, so the exact bond the wall producer chose does
		 * not matter. Its outer face Y = 0 stands one JointCm off the cleat's back Y = -1, so MakeInterface
		 * reads a Y-normal mortar anchor bonding the cleat to real, load-bearing masonry.
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
		 * THE FOUR PORCH JOINTS, each through MakeInterface with the LOWER piece named A so a bed normal
		 * reads as a bed BENEATH the piece it carries. Two Z-normal DryStone bearings carry the overhang on
		 * the post tops (~100 cm2 each); a Z-normal Screw tie (~90 cm2) holds the overhang's back down in
		 * withdrawal; a Y-normal mortar anchor (~65 cm2) bonds the cleat to the wall.
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
			|| !PorchJoin(Cleat, CleatWallBrick, GeneralPurposeMortar))
		{
			return false;
		}

		/*
		 * THE 3D FLAG routes this structure to the Dim3D pose and lifts the bridge's Y-normal refusal,
		 * so the side walls' Y-normal perpends and the out-of-plane corner joints are honoured rather
		 * than rejected. Above the block cap the router (not the LP) is the break authority, but the
		 * flag still selects the genuinely-3D geometry every reader downstream sees.
		 */
		Laid.Structure.SetThreeDimensional(true);

		OutLayout = MoveTemp(Laid);

		return true;
	}
}
