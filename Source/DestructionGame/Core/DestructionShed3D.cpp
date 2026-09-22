// Copyright Epic Games, Inc. All Rights Reserved.

#include "Core/DestructionShed3D.h"

#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"

// Helpers live in the named namespace, not an anonymous one, to avoid unity-build name collisions.
namespace DestructionShed3D
{
	using namespace DestructionLayout;

	bool Build(const FShed3DSpec& Spec, DestructionLayout::FBrickLayout& OutLayout)
	{
		/*
		 * Emptied first and filled last, so a refusal leaves nothing. Guards use `!(x > 0.0)` so a
		 * NaN dimension is rejected; `x <= 0.0` is false for NaN.
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
		 * X is width, Y depth, Z height. Side walls sit between back and front with a joint gap on
		 * Y, so each corner is a Y-normal joint; beams leave the same gap in Z for their bed joints.
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

		// Adds a box, its mass and material; the piece handle equals the box index.
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

		// Four grounded brick walls, free timber roof and overhang, grounded timber post.
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
		 * Eight joints: mortar corners (Y-normal), DryStone bearings, a Screw fixing. The lower piece
		 * is A so the normal is a bed under the piece carried. MakeInterface rejects mis-sized pieces.
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

		// 3D: use the Dim3D pose and accept the Y-normal corner joints.
		Laid.Structure.SetThreeDimensional(true);

		OutLayout = MoveTemp(Laid);

		return true;
	}

	bool BuildRecognizable(DestructionLayout::FBrickLayout& OutLayout)
	{
		/*
		 * Emptied first and filled last. Dimensions are hardcoded, so the fail-closed points are
		 * AddPiece and Join.
		 */
		OutLayout = FBrickLayout();

		const double JointThicknessCm = 1.0;

		FBrickLayout Laid;

		// Same as Build's AddPiece.
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
		 * 24 pieces: walls, door and window, stepped gables, purlin-and-ridge roof, and a porch (two
		 * posts, overhang, cleat). Brick masonry, timber lintels/roof/porch. The cleat is not grounded.
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
		 * The cleat: 20 cm wide, mortared to the DoorHeader and screwed to the overhang (180 cm2).
		 * Its narrow couple arm cannot resist the torsion from a lost post, so pulling either post
		 * drops the porch.
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

		// Lower piece is A. MakeInterface rejects any pair not sharing a face on one axis.
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
		 * Door (2) and window (4) lintels on DryStone. Corners (4) mortared; all grounded-grounded, so
		 * the oracle skips them. Gables (6) on mortar beds. Roof (10) purlins on DryStone. Porch (4):
		 * posts carry the overhang, the cleat ties its back down.
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

		// 3D: use the Dim3D pose and accept the Y-normal corner joints.
		Laid.Structure.SetThreeDimensional(true);

		OutLayout = MoveTemp(Laid);

		return true;
	}

	/*
	 * An opening in a wall panel: bricks are omitted in the clear gap, and separately in the wider
	 * lintel band (one course) so the lintel bears on the flanking bricks. A default opening omits
	 * nothing, since its ranges are empty (Hi < Lo).
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
		 * Emptied first and filled last. Dimensions are hardcoded; AddPiece refusals set bLayoutValid,
		 * checked once at the end.
		 */
		OutLayout = FBrickLayout();

		/*
		 * 21.5 x 10.25 x 6.5 cm brick, 1 cm joint: 22.5 cm pitch, 7.5 cm course pitch, alternate
		 * courses offset 11.25. A half bat closes each odd course flush.
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
		 * Footprint X[0,180], Y[0,134], walls one wythe thick. Side walls run along Y between the
		 * front and back walls. Depth 134 makes a five-brick side run close both corners on a 1 cm
		 * joint; 140 would leave a 7 cm gap.
		 */
		const double SideRunStartCm = BondOffsetCm;
		const int32 FrontBackBricks = 8;
		const int32 SideBricks = 5;
		const double BackWallY0Cm = SideRunStartCm + SideBricks * PitchCm;
		const double RightWallX0Cm = 180.0 - WytheCm;

		FBrickLayout Laid;
		bool bLayoutValid = true;

		// As above, but a refusal sets bLayoutValid false instead of returning.
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

		// Strict overlap, so a brick ending exactly on an opening edge stays as a jamb.
		const auto StrictlyOverlaps =
			[](double LoA, double HiA, double LoB, double HiB) -> bool
		{
			const double Eps = 1.0e-6;
			return LoA < HiB - Eps && HiA > LoB + Eps;
		};

		/*
		 * Lay one running-bond panel along X or Y. Odd courses are offset with half bats at each end.
		 * Bricks in the opening are skipped. Course 0 is grounded; joints are formed later.
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
		 * Door (front wall): gap X[57.5,122.5] over courses 0-11, lintel X[50,130] on course 12.
		 * Window (left wall): gap Y[66.5,90] over courses 6-11, lintel Y[61.5,95] on course 12.
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

		// Side walls start one bond offset in, leaving a joint gap at each corner.
		LayWall(true, 0.0, WytheCm, 0.0, FrontBackBricks, DoorOpening);
		LayWall(true, BackWallY0Cm, BackWallY0Cm + WytheCm, 0.0, FrontBackBricks, NoOpening);
		LayWall(false, 0.0, WytheCm, SideRunStartCm, SideBricks, WindowOpening);
		LayWall(false, RightWallX0Cm, RightWallX0Cm + WytheCm, SideRunStartCm, SideBricks, NoOpening);

		// Timber lintels on course 12; their bed joints come from the sweep below.
		const int32 DoorLintel = AddPiece(
			DoorOpening.LintelRunLoCm, DoorOpening.LintelRunHiCm, 0.0, WytheCm, 90.0, 96.5, Timber, false);
		const int32 WindowLintel = AddPiece(
			0.0, WytheCm, WindowOpening.LintelRunLoCm, WindowOpening.LintelRunHiCm, 90.0, 96.5, Timber, false);

		/*
		 * Stepped gables on front and back walls: courses 16-19 step in one pitch per side, 8 bricks
		 * down to 2. Symmetric, so each course's centroid stays over the one below.
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
		 * Five timber members span front to back, simply supported on the gable shoulders (eaves on
		 * course 16, mid on 18, ridge on 19). The ~8.5 cm gap over the side walls forms no joint.
		 */
		const double RoofDepthFrontYCm = BackWallY0Cm + WytheCm;
		const double RoofThicknessCm = 5.0;

		// Bottom Z of a board bearing one joint above a gable course.
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
		 * Offer every pair to MakeInterface and keep what it accepts (pairs sharing a face across
		 * one joint thickness). Lower piece is A. Brick-brick is mortar; anything touching timber is
		 * compression-only DryStone.
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
					 * Non-vertical brick-brick joints (perpends and corners) get the weaker perpend
					 * mortar (owner item 6b). Normals are axis-aligned, so |Z| is 1 or 0.
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
		 * The porch, laid after the sweep so its joints can be Screw (tension-capable) where the
		 * sweep would give DryStone.
		 *
		 * It cantilevers in -Y from the front wall (Y = 0). Two grounded posts at Y[-25,-15] carry a
		 * plank overhang (Y[-70,-2]); its centroid (Y = -36) is outboard of the posts, so its back
		 * lifts and a 10 cm cleat screwed to a course-13 wall brick holds it down. The cleat is too
		 * narrow to resist the torsion from a lost post, so pulling one drops the porch.
		 */
		const int32 PostL = AddPiece(50.0, 60.0, -25.0, -15.0, 0.0, 104.0, Timber, true);
		const int32 PostR = AddPiece(120.0, 130.0, -25.0, -15.0, 0.0, 104.0, Timber, true);
		const int32 PorchOverhang = AddPiece(50.0, 130.0, -70.0, -2.0, 105.0, 110.0, Timber, false);
		const int32 Cleat = AddPiece(85.0, 95.0, -11.0, -1.0, 97.5, 104.0, Timber, false);

		// The course-13 brick over the door centre, found by point so the bond layout doesn't matter.
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
		 * Two DryStone post bearings (~100 cm2 each), a Screw tie from cleat to overhang (~90 cm2),
		 * and a Y-normal Screw anchor from cleat to wall (~65 cm2). Lower piece is A.
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
		 * 3D: use the Dim3D pose and accept Y-normal joints. Above the block cap the router decides
		 * breaks, but the flag still sets the geometry readers see.
		 */
		Laid.Structure.SetThreeDimensional(true);

		OutLayout = MoveTemp(Laid);

		return true;
	}
}
