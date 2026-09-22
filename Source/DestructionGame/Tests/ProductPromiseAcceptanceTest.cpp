// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Connection.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"
#include "Core/StructureBinding.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Product-promise acceptance tests (DESIGN.md §7 step 4): a change far from the cut stays put, and a
 * structure that should stand does not collapse. Each enters through FStructureBinding with the
 * player's sequence (RemovePiece, SolveAndBreak, ApplyResults) and reads support state and what was
 * released, never displacement (DESIGN.md §4). World-free; gravity is inside FStructure.
 *
 * Only the producers (RunningBond, MakeInterface) and profiles are imported; masses are derived here.
 * Named namespace because unity builds merge translation units.
 */
namespace ProductPromiseSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	// Standard brick, cm.
	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickWidthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;

	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** 1 cm bed; course pitch 7.5 cm. */
	constexpr double BedJointThicknessCm = 1.0;
	constexpr double CoursePitchCm = BrickHeightCm + BedJointThicknessCm;

	/** MassKg * 980 is already a weight in uu; do not apply 1 N = 100 uu again. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	// Fixture plumbing: lay into an FBrickLayout, then adopt via AdoptLayout (the only public route).

	FPieceBox MakeBox(const FVector& CentreCm, const FVector& FullSizeCm)
	{
		FPieceBox Box;
		Box.CentreCm = CentreCm;
		Box.ExtentCm = FullSizeCm * 0.5;
		return Box;
	}

	double BoxMassKg(const FPieceBox& Box)
	{
		return ClayDensityGramsPerCubicCm
			* (Box.ExtentCm.X * 2.0) * (Box.ExtentCm.Y * 2.0) * (Box.ExtentCm.Z * 2.0) / 1000.0;
	}

	/** Adds one box as a piece, keeping boxes parallel to pieces. */
	int32 AddBrick(FBrickLayout& L, const FVector& CentreCm, const FVector& FullSizeCm, bool bGrounded)
	{
		const FPieceBox Box = MakeBox(CentreCm, FullSizeCm);
		const int32 Handle = L.Structure.AddPiece(BoxMassKg(Box), bGrounded, Box.CentreCm);
		L.Boxes.Add(Box);
		return Handle;
	}

	/** Joins two laid boxes via MakeInterface. */
	bool Join(FBrickLayout& L, int32 A, int32 B, const FConnectionStrength& Strength)
	{
		FConnection Joint;
		if (MakeInterface(A, L.Boxes[A], B, L.Boxes[B], BedJointThicknessCm, Strength, Joint))
		{
			return L.Structure.AddConnection(Joint) != INDEX_NONE;
		}
		return false;
	}

	/**
	 * Lays a running-bond wall and appends it to Combined, shifted DxCm on X.
	 *
	 * @return the first appended piece index (the region split point), or INDEX_NONE.
	 */
	int32 AppendRunningBond(FBrickLayout& Combined, const FRunningBondSpec& Spec, double DxCm)
	{
		FBrickLayout Wall;
		if (!RunningBond(Spec, Wall))
		{
			return INDEX_NONE;
		}

		const int32 Base = Combined.Structure.NumPieces();

		for (int32 i = 0; i < Wall.Structure.NumPieces(); ++i)
		{
			const FStructurePiece& Piece = Wall.Structure.GetPiece(i);
			FPieceBox Box = Wall.Boxes[i];
			Box.CentreCm.X += DxCm;

			Combined.Structure.AddPiece(Piece.MassKg, Piece.bIsGrounded, Box.CentreCm);
			Combined.Boxes.Add(Box);
		}

		for (int32 j = 0; j < Wall.Structure.NumConnections(); ++j)
		{
			FConnection C = Wall.Structure.GetConnection(j);
			C.PieceA += Base;
			C.PieceB += Base;
			C.InterfaceCentreCm.X += DxCm;
			Combined.Structure.AddConnection(C);
		}

		return Base;
	}

	/** First piece whose box centre matches, or INDEX_NONE. */
	int32 FindPiece(const FBrickLayout& L, const FVector& CentreCm)
	{
		for (int32 i = 0; i < L.Boxes.Num(); ++i)
		{
			if (L.Boxes[i].CentreCm.Equals(CentreCm, 1.0e-3))
			{
				return i;
			}
		}
		return INDEX_NONE;
	}

	/** Adopts a laid layout into a binding. */
	bool Adopt(const FBrickLayout& L, FStructureBinding& Out)
	{
		TArray<UObject*> Actors;
		Actors.Init(nullptr, L.Structure.NumPieces());
		return AdoptLayout(L, Actors, Out);
	}

	// Reading the verdict off the settled binding.

	bool StillStanding(const FStructure& S, int32 Piece)
	{
		const EPieceSupport Support = S.GetPieceSupport(Piece);
		return Support == EPieceSupport::Grounded || Support == EPieceSupport::Supported;
	}

	/** Live pieces that lost their path to the earth (Falling or Stranded). */
	TArray<int32> PiecesThatLostTheEarth(const FStructure& S)
	{
		TArray<int32> Lost;
		for (int32 i = 0; i < S.NumPieces(); ++i)
		{
			if (!S.IsPieceRemoved(i) && !StillStanding(S, i))
			{
				Lost.Add(i);
			}
		}
		return Lost;
	}

	int32 StrandedCount(const FStructure& S)
	{
		int32 Stranded = 0;
		for (int32 i = 0; i < S.NumPieces(); ++i)
		{
			if (!S.IsPieceRemoved(i) && S.GetPieceSupport(i) == EPieceSupport::Stranded)
			{
				++Stranded;
			}
		}
		return Stranded;
	}

	/** A joint that failed under load, not one severed by removal. */
	bool BrokeUnderLoad(const FStructure& S, int32 Joint)
	{
		return S.GetBreakPass(Joint) != INDEX_NONE;
	}
}

/**
 * Locality: removing a piece affects only what shares a load path with it, not a radius.
 *
 * Region A is a six-brick mortared column at X = 0; pulling its base drops everything above.
 * Region B is a 3 x 4 running-bond wall shifted +40 cm, 18.5 cm clear, with its own grounded base.
 * No connection links them (asserted); they are close so a radius-based spread would reach across.
 *
 * After cutting the column base: every wall piece keeps its support state, no wall joint gives,
 * nothing in the wall is released, and everything affected lies in the column. Green on arrival;
 * a radius-based sever in RemovePiece was confirmed to fail it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLocalityChangeFarFromCutStaysPutTest,
	"DestructionGame.Acceptance.Locality.AChangeFarFromTheCutStaysPut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLocalityChangeFarFromCutStaysPutTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace ProductPromiseSupport;

	// Region A: the mortared column, grounded at its foot.

	FBrickLayout L;

	constexpr int32 ColumnCourses = 6;
	const FVector BrickSize(BrickLengthCm, BrickWidthCm, BrickHeightCm);

	int32 ColumnBase = INDEX_NONE;
	for (int32 Course = 0; Course < ColumnCourses; ++Course)
	{
		const FVector Centre(0.0, 0.0, BrickHeightCm / 2.0 + double(Course) * CoursePitchCm);
		const int32 Handle = AddBrick(L, Centre, BrickSize, /*bGrounded*/ Course == 0);

		if (Course == 0)
		{
			ColumnBase = Handle;
		}
		else
		{
			Join(L, Handle - 1, Handle, GeneralPurposeMortar);
		}
	}

	// Region B: the running-bond wall, +40 cm on X, its own grounded base.

	FRunningBondSpec Spec;
	Spec.DensityGramsPerCubicCm = ClayDensityGramsPerCubicCm;
	Spec.CoursesHigh = 4;
	Spec.BricksPerCourse = 3;
	Spec.End = EWallEnd::Flush;
	Spec.Strength = GeneralPurposeMortar;

	const int32 Split = AppendRunningBond(L, Spec, /*DxCm*/ 40.0);

	if (Split == INDEX_NONE || ColumnBase == INDEX_NONE)
	{
		AddError(TEXT("FIXTURE: the column and the producer's wall must both lay"));
		return false;
	}

	// Precondition: no connection links a column piece (< Split) to a wall piece.

	bool bCrossJoint = false;
	for (int32 j = 0; j < L.Structure.NumConnections(); ++j)
	{
		const FConnection& C = L.Structure.GetConnection(j);
		const bool bAInA = C.PieceA < Split;
		const bool bBInA = C.PieceB < Split;
		if (bAInA != bBInA)
		{
			bCrossJoint = true;
		}
	}

	TestFalse(
		TEXT("INDEPENDENCE: no connection may link region A to region B, or they are not two "
			 "load-path-independent regions and the whole test means nothing"),
		bCrossJoint);

	// Snapshot region B while intact.

	FStructureBinding Binding;
	if (!Adopt(L, Binding))
	{
		AddError(TEXT("FIXTURE: the layout must adopt into a binding"));
		return false;
	}

	Binding.SolveLoads();

	const FStructure& S = Binding.GetStructure();

	TArray<EPieceSupport> WallBefore;
	for (int32 i = Split; i < S.NumPieces(); ++i)
	{
		WallBefore.Add(S.GetPieceSupport(i));
	}

	// Pull the column's base, run the cascade, push.

	Binding.RemovePiece(ColumnBase);
	const int32 Passes = Binding.SolveAndBreak();
	const int32 Released = Binding.ApplyResults();

	const TArray<int32> LostEarth = PiecesThatLostTheEarth(S);
	const int32 Stranded = StrandedCount(S);

	AddInfo(FString::Printf(
		TEXT("PRODUCTION: cut the column base; cascade ran %d pass(es); %d piece(s) released; "
			 "%d lost the earth; %d stranded (split at %d, %d pieces total)"),
		Passes, Released, LostEarth.Num(), Stranded, Split, S.NumPieces()));

	TestEqual(
		TEXT("PRECONDITION: no piece may be Stranded — a routing limitation must not wear a "
			 "collapse's clothes"),
		Stranded, 0);

	// Region B is untouched.

	for (int32 i = Split; i < S.NumPieces(); ++i)
	{
		TestEqual(
			*FString::Printf(
				TEXT("LOCALITY: wall piece %d must keep the exact support state it had before the "
					 "distant cut"),
				i),
			static_cast<int32>(S.GetPieceSupport(i)),
			static_cast<int32>(WallBefore[i - Split]));

		TestFalse(
			*FString::Printf(TEXT("LOCALITY: wall piece %d must not have been released to physics"), i),
			Binding.IsReleased(i));
	}

	// No wall joint broke or was severed.
	for (int32 j = 0; j < S.NumConnections(); ++j)
	{
		const FConnection& C = S.GetConnection(j);
		if (C.PieceA >= Split && C.PieceB >= Split)
		{
			TestFalse(
				*FString::Printf(TEXT("LOCALITY: wall joint %d must not have given"), j),
				C.HasGiven());
			TestFalse(
				*FString::Printf(TEXT("LOCALITY: wall joint %d must not have broken under load"), j),
				BrokeUnderLoad(S, j));
		}
	}

	// Everything affected lies in region A.

	for (int32 Piece : LostEarth)
	{
		TestTrue(
			*FString::Printf(
				TEXT("CONFINEMENT: only region A may lose the earth; piece %d (split %d) did"),
				Piece, Split),
			Piece < Split);
	}

	for (int32 j = 0; j < S.NumConnections(); ++j)
	{
		if (BrokeUnderLoad(S, j))
		{
			const FConnection& C = S.GetConnection(j);
			TestTrue(
				*FString::Printf(
					TEXT("CONFINEMENT: only region A joints may break under load; joint %d did"), j),
				C.PieceA < Split && C.PieceB < Split);
		}
	}

	return true;
}

/**
 * Removing a piece that carries nothing moves nothing. A parapet brick on a 3 x 3 wall carries no
 * load, so removing it only unloads the joints below: nothing breaks, is released or loses the
 * ground. Green on arrival; a radius-based sever in RemovePiece fails it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMustNotFallCutFarFromLoadPathTest,
	"DestructionGame.Acceptance.MustNotFall.ACutFarFromTheLoadPathLeavesItStanding",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMustNotFallCutFarFromLoadPathTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace ProductPromiseSupport;

	FBrickLayout L;

	FRunningBondSpec Spec;
	Spec.DensityGramsPerCubicCm = ClayDensityGramsPerCubicCm;
	Spec.CoursesHigh = 3;
	Spec.BricksPerCourse = 3;
	Spec.End = EWallEnd::Flush;
	Spec.Strength = GeneralPurposeMortar;

	if (AppendRunningBond(L, Spec, /*DxCm*/ 0.0) == INDEX_NONE)
	{
		AddError(TEXT("FIXTURE: the producer's wall must lay"));
		return false;
	}

	const int32 WallPieces = L.Structure.NumPieces();

	// Middle top-course brick: course 2 (Z = 18.25), X = 22.5.
	const FVector TopMiddleCentre(22.5, 0.0, BrickHeightCm / 2.0 + 2.0 * CoursePitchCm);
	const int32 Support = FindPiece(L, TopMiddleCentre);

	if (Support == INDEX_NONE)
	{
		AddError(TEXT("FIXTURE: the middle top-course brick the parapet sits on must exist"));
		return false;
	}

	// The parapet, one bed above it.
	const FVector ParapetCentre(22.5, 0.0, TopMiddleCentre.Z + CoursePitchCm);
	const int32 Parapet = AddBrick(
		L, ParapetCentre, FVector(BrickLengthCm, BrickWidthCm, BrickHeightCm), /*bGrounded*/ false);

	if (!Join(L, Support, Parapet, GeneralPurposeMortar))
	{
		AddError(TEXT("FIXTURE: the parapet must form a bed joint on the top brick"));
		return false;
	}

	FStructureBinding Binding;
	if (!Adopt(L, Binding))
	{
		AddError(TEXT("FIXTURE: the layout must adopt into a binding"));
		return false;
	}

	const FStructure& S = Binding.GetStructure();

	// Precondition: the parapet rests on the wall; its only joint is beneath it.
	Binding.SolveLoads();
	TestTrue(
		TEXT("FIXTURE: the parapet must rest on the wall before removal (Supported)"),
		S.GetPieceSupport(Parapet) == EPieceSupport::Supported);

	Binding.RemovePiece(Parapet);
	const int32 Passes = Binding.SolveAndBreak();
	const int32 Released = Binding.ApplyResults();

	const TArray<int32> LostEarth = PiecesThatLostTheEarth(S);
	const int32 Stranded = StrandedCount(S);

	AddInfo(FString::Printf(
		TEXT("PRODUCTION: removed the parapet; cascade ran %d pass(es); %d released; %d lost the "
			 "earth; %d stranded"),
		Passes, Released, LostEarth.Num(), Stranded));

	TestEqual(TEXT("PRECONDITION: no piece may be Stranded"), Stranded, 0);

	TestEqual(
		TEXT("MUST STAND: removing a non-load-bearing parapet must break nothing under load"),
		Passes, 0);

	TestEqual(
		TEXT("MUST STAND: removing a non-load-bearing parapet must release nothing to physics"),
		Released, 0);

	TestEqual(
		TEXT("MUST STAND: removing a non-load-bearing parapet must leave nothing without a path "
			 "to the earth"),
		LostEarth.Num(), 0);

	for (int32 i = 0; i < WallPieces; ++i)
	{
		TestTrue(
			*FString::Printf(TEXT("MUST STAND: wall piece %d must still stand"), i),
			StillStanding(S, i));
	}

	return true;
}

/**
 * Removing a redundant support leaves the load standing. A beam on three grounded piers at
 * X = -40, 0, +40 loses the middle one; its centre of mass at X = 0 stays between the end piers, so
 * it is still Supported and nothing breaks or is released. (Removing an end pier would be a real
 * overturn.) Green on arrival; a radius-based sever in RemovePiece fails it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMustNotFallRedundantMemberTest,
	"DestructionGame.Acceptance.MustNotFall.RemovingARedundantMemberLeavesItStanding",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMustNotFallRedundantMemberTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace ProductPromiseSupport;

	FBrickLayout L;

	// Three grounded piers, 10 wide, 20 tall.
	constexpr double PierWidthX = 10.0;
	constexpr double PierHeightZ = 20.0;
	const FVector PierSize(PierWidthX, BrickWidthCm, PierHeightZ);

	const int32 PierLeft = AddBrick(L, FVector(-40.0, 0.0, PierHeightZ / 2.0), PierSize, /*bGrounded*/ true);
	const int32 PierMid = AddBrick(L, FVector(0.0, 0.0, PierHeightZ / 2.0), PierSize, /*bGrounded*/ true);
	const int32 PierRight = AddBrick(L, FVector(40.0, 0.0, PierHeightZ / 2.0), PierSize, /*bGrounded*/ true);

	// The beam, 100 long and 13 thick, one bed above the pier tops.
	constexpr double BeamLengthX = 100.0;
	constexpr double BeamThickZ = 13.0;
	const double BeamBottomZ = PierHeightZ + BedJointThicknessCm;
	const FVector BeamCentre(0.0, 0.0, BeamBottomZ + BeamThickZ / 2.0);
	const int32 Beam = AddBrick(
		L, BeamCentre, FVector(BeamLengthX, BrickWidthCm, BeamThickZ), /*bGrounded*/ false);

	const bool bJoined =
		Join(L, PierLeft, Beam, GeneralPurposeMortar)
		&& Join(L, PierMid, Beam, GeneralPurposeMortar)
		&& Join(L, PierRight, Beam, GeneralPurposeMortar);

	if (!bJoined)
	{
		AddError(TEXT("FIXTURE: all three piers must form a bed joint beneath the beam"));
		return false;
	}

	FStructureBinding Binding;
	if (!Adopt(L, Binding))
	{
		AddError(TEXT("FIXTURE: the layout must adopt into a binding"));
		return false;
	}

	const FStructure& S = Binding.GetStructure();

	Binding.SolveLoads();
	TestTrue(
		TEXT("FIXTURE: the beam must be Supported with all three piers present"),
		S.GetPieceSupport(Beam) == EPieceSupport::Supported);

	Binding.RemovePiece(PierMid);
	const int32 Passes = Binding.SolveAndBreak();
	const int32 Released = Binding.ApplyResults();

	const TArray<int32> LostEarth = PiecesThatLostTheEarth(S);
	const int32 Stranded = StrandedCount(S);

	AddInfo(FString::Printf(
		TEXT("PRODUCTION: removed the redundant middle pier; cascade ran %d pass(es); %d released; "
			 "%d lost the earth; %d stranded; beam support now %d (2=Supported)"),
		Passes, Released, LostEarth.Num(), Stranded, static_cast<int32>(S.GetPieceSupport(Beam))));

	TestEqual(TEXT("PRECONDITION: no piece may be Stranded"), Stranded, 0);

	TestEqual(
		TEXT("MUST STAND: removing one of two remaining paths must break nothing under load"),
		Passes, 0);

	TestEqual(
		TEXT("MUST STAND: the beam must not be released — the two end piers still carry it"),
		Released, 0);

	TestEqual(
		TEXT("MUST STAND: nothing may lose the earth when a redundant support is removed"),
		LostEarth.Num(), 0);

	TestTrue(
		TEXT("MUST STAND: the beam must still be Supported by the two end piers"),
		S.GetPieceSupport(Beam) == EPieceSupport::Supported);

	TestTrue(
		TEXT("MUST STAND: both end piers must still be Grounded"),
		S.GetPieceSupport(PierLeft) == EPieceSupport::Grounded
			&& S.GetPieceSupport(PierRight) == EPieceSupport::Grounded);

	return true;
}

/**
 * A dry stack that is stable by geometry stands without tension. A DryStone brick (c = 0, f_t = 0)
 * sits 8 cm off-centre on a grounded base; its centre of mass is 2.75 cm inside the bearing edge,
 * so a rigid block neither tips nor slides.
 *
 * Can go red on the known no-rocking-model gap (CURRENT_STATE.md): e = 4.0 cm is outside the
 * 2.25 cm kern, and with f_t = 0 the uncracked check gives the joint. A red is only valid if the
 * base stays grounded and nothing is Stranded.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMustNotFallDryStackStandsByGeometryTest,
	"DestructionGame.Acceptance.MustNotFall.APureCompressionDryStackStandsByGeometry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMustNotFallDryStackStandsByGeometryTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace ProductPromiseSupport;

	// Precondition: a no-tension, no-cohesion joint.
	TestEqual(TEXT("FIXTURE: dry stone carries exactly zero tensile strength"),
		DryStone.TensileStrengthMPa, 0.0);
	TestEqual(TEXT("FIXTURE: dry stone carries exactly zero cohesion"),
		DryStone.ShearCohesionMPa, 0.0);

	constexpr double OffsetXCm = 8.0;
	const FVector BrickSize(BrickLengthCm, BrickWidthCm, BrickHeightCm);

	// Precondition: the resultant is inside the bearing.
	const double BaseRightEdgeXCm = BrickLengthCm / 2.0;
	TestTrue(
		*FString::Printf(
			TEXT("FIXTURE: the upper brick's centre of mass (X = %.4g) must sit inside the base "
				 "bearing edge (X = %.4g) — a rigid block here does not tip"),
			OffsetXCm, BaseRightEdgeXCm),
		OffsetXCm < BaseRightEdgeXCm);

	FBrickLayout L;
	const int32 Base = AddBrick(L, FVector(0.0, 0.0, BrickHeightCm / 2.0), BrickSize, /*bGrounded*/ true);
	const int32 Upper = AddBrick(
		L, FVector(OffsetXCm, 0.0, BrickHeightCm / 2.0 + CoursePitchCm), BrickSize, /*bGrounded*/ false);

	if (!Join(L, Base, Upper, DryStone))
	{
		AddError(TEXT("FIXTURE: the two dry-stone bricks must form a bed joint"));
		return false;
	}

	FStructureBinding Binding;
	if (!Adopt(L, Binding))
	{
		AddError(TEXT("FIXTURE: the layout must adopt into a binding"));
		return false;
	}

	const FStructure& S = Binding.GetStructure();

	// No removal; just settle under gravity.
	const int32 Passes = Binding.SolveAndBreak();
	const int32 Released = Binding.ApplyResults();

	const TArray<int32> LostEarth = PiecesThatLostTheEarth(S);
	const int32 Stranded = StrandedCount(S);

	AddInfo(FString::Printf(
		TEXT("PRODUCTION: dry stack settled; cascade ran %d pass(es); %d released; %d lost the "
			 "earth; %d stranded; base support %d, upper support %d (1=Grounded,2=Supported,3=Stranded,0=Falling); "
			 "the one joint gave: %d"),
		Passes, Released, LostEarth.Num(), Stranded,
		static_cast<int32>(S.GetPieceSupport(Base)), static_cast<int32>(S.GetPieceSupport(Upper)),
		S.GetConnection(0).HasGiven() ? 1 : 0));

	// No routing limitation involved, whichever way the verdict goes.
	TestEqual(TEXT("PRECONDITION: no piece may be Stranded"), Stranded, 0);

	TestTrue(
		TEXT("KNOWN-GAP CHECK: whatever the verdict, the grounded base must keep the earth — a red "
			 "here must be the upper brick's no-tension joint, not a broken base"),
		S.GetPieceSupport(Base) == EPieceSupport::Grounded);

	// Aspirational while there is no dry-stone rocking model.
	TestEqual(
		TEXT("MUST STAND: a geometrically-stable dry stack must break nothing under load"),
		Passes, 0);

	TestEqual(
		TEXT("MUST STAND: a geometrically-stable dry stack must release nothing to physics"),
		Released, 0);

	TestTrue(
		TEXT("MUST STAND: the upper brick must stand on the base by geometry alone, without tension"),
		StillStanding(S, Upper));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
