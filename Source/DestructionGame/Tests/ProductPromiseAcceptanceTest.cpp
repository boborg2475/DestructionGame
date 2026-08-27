// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Connection.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Structure.h"
#include "Core/StructureBinding.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * THE PRODUCT-PROMISE ACCEPTANCE SUITE — the player-facing safety net for the equilibrium-gate
 * promotion (DESIGN.md §7 step 4). These do not test a heuristic; they test PROMISES a believable
 * structural sim makes to the person pulling bricks out of a wall: a change far from the cut stays
 * put, and a structure that should stand does not eagerly collapse. The upcoming break-cascade
 * change (delete BreakOverturnedBodies, promote the LP to authority) must not break these.
 *
 * WHAT PIPELINE THESE DRIVE, AND WHY THAT ONE. Every test here enters through FStructureBinding —
 * the same door PieceActions' commit and the spawn settle go through — and takes the player's own
 * sequence: RemovePiece, then SolveAndBreak, then ApplyResults (Core/StructureBinding.cpp). The
 * verdict read back is the settled SUPPORT STATE of every piece (Grounded / Supported / Stranded /
 * Falling) and what ApplyResults actually released — i.e. what the game would hand to Chaos. That
 * is the player-facing outcome, and it is what a demolition player sees. It is NOT displacement:
 * DESIGN.md §4 forbids reading distance travelled, because two pieces can sever and rest exactly in
 * place — so nothing here reads a position after the fact. It reads which joints broke under load,
 * which pieces lost the earth, and which the binding released.
 *
 * NEEDS A TICKING WORLD: NO, for all four. The break DECISION is headless — SolveAndBreak settles
 * the graph and ApplyResults is a pure push over support states — so these assert the decision, not
 * a physics settle. Gravity is on the ordinary way every acceptance fixture in this suite has it
 * on: weight is MassKg * 980 inside FStructure (DESIGN.md §3), so the structures are loaded by
 * their own mass with no world. Same footing as the leaning-stack, beam and two-load-path tests.
 *
 * NOTHING IS IMPORTED FROM THE CODE UNDER TEST EXCEPT THE PRODUCERS (RunningBond / MakeInterface)
 * and the mortar/dry-stone profiles the joints are laid in. Masses are derived here from density
 * and geometry, so a wrong production constant disagrees with this file rather than agreeing.
 *
 * NAMED NAMESPACE, not anonymous: a unity build merges many files into one translation unit.
 */
namespace ProductPromiseSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/* ================================================================================
	 * SHARED GEOMETRY. Every length is centimetres, at Unreal's default 1 uu = 1 cm.
	 * ================================================================================ */

	/** The standard brick every anchor in this project is derived from. */
	constexpr double BrickLengthCm = 21.5;
	constexpr double BrickWidthCm = 10.25;
	constexpr double BrickHeightCm = 6.5;

	/** Fired clay, 1.9 g/cm3 — the same figure every wall fixture uses. */
	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** A 1 cm mortar/contact bed. Course pitch is 7.5 cm; the coordinating grid on X is 22.5 cm. */
	constexpr double BedJointThicknessCm = 1.0;
	constexpr double CoursePitchCm = BrickHeightCm + BedJointThicknessCm;

	/** MassKg * 980 IS a weight in uu — the 1 N = 100 uu conversion is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/* ================================================================================
	 * FIXTURE PLUMBING — build into an FBrickLayout, then adopt into a binding and drive
	 * the player's own removal path. AdoptLayout is the only public route from a laid
	 * layout to a binding (StructureBinding.h), so this is the production wall wire.
	 * ================================================================================ */

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

	/** Hand-lay one box as a piece, keeping the box index-parallel to the piece array. */
	int32 AddBrick(FBrickLayout& L, const FVector& CentreCm, const FVector& FullSizeCm, bool bGrounded)
	{
		const FPieceBox Box = MakeBox(CentreCm, FullSizeCm);
		const int32 Handle = L.Structure.AddPiece(BoxMassKg(Box), bGrounded, Box.CentreCm);
		L.Boxes.Add(Box);
		return Handle;
	}

	/** Join two already-laid boxes; MakeInterface owns the normal, area and orientation. */
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
	 * Lay a running-bond wall with the PRODUCER and copy it into a combined layout, shifted by
	 * DxCm on X, re-basing every piece handle and translating every joint's own rectangle. The
	 * area, extent and normal are translation-invariant, so AddConnection re-validates unchanged.
	 *
	 * @return the index the appended wall starts at (the region split point), or INDEX_NONE.
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

	/** First piece whose box centre matches, within a tight epsilon. INDEX_NONE if none. */
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

	/** Adopt a laid layout into a binding through the only public door there is. */
	bool Adopt(const FBrickLayout& L, FStructureBinding& Out)
	{
		TArray<UObject*> Actors;
		Actors.Init(nullptr, L.Structure.NumPieces());
		return AdoptLayout(L, Actors, Out);
	}

	/* ================================================================================
	 * READING THE PLAYER-FACING VERDICT off the settled binding.
	 * ================================================================================ */

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

	/** A joint that FAILED UNDER LOAD in a cascade pass — not one merely severed by removal. */
	bool BrokeUnderLoad(const FStructure& S, int32 Joint)
	{
		return S.GetBreakPass(Joint) != INDEX_NONE;
	}
}

/**
 * GROUP 1 — LOCALITY: A CHANGE FAR FROM THE CUT STAYS PUT.
 *
 * THE PROMISE. Removing a piece is a LOCAL event: its effect follows load paths, not a radius, and
 * cannot reach a region that shares no load path with the cut. This one tripwire guards three
 * vision statements at once — "one brick out of a large wall is a local event", "a failure does not
 * run further than its cause", and "redistribution is path-based, not radius-based".
 *
 * THE FIXTURE — TWO GENUINELY INDEPENDENT REGIONS IN ONE STRUCTURE.
 *   Region A: a six-brick mortared COLUMN at X = 0, grounded at its foot. Pulling its base out is a
 *             real, vivid local collapse: every course above has only the one bed joint beneath it,
 *             so the whole column loses the earth. That is the cut's cause running its full course.
 *   Region B: a three-wide, four-course running-bond WALL laid by the PRODUCER (RunningBond),
 *             shifted +40 cm on X so it sits 18.5 cm clear of the column with its own grounded base.
 *
 * WHY THEY ARE INDEPENDENT, STATED SO NOBODY LATER SIMPLIFIES IT OUT. The two regions share NO
 * connection: the column is joined only to the column, the wall only to the wall, and no joint links
 * a column piece to a wall piece. The solver routes load over the SUPPORT GRAPH (its edges are the
 * connections), so a region reachable from the cut only across an edge that does not exist has no
 * admissible way to be affected — the wall's subgraph is a separate connected component from the
 * column's. This test ASSERTS that no A-B connection exists, so the independence is a checked
 * precondition and not an accident of layout. They are deliberately CLOSE in space (18.5 cm) so a
 * radius-based redistribution — the bug this guards — would reach across; only a path-based one does
 * not. (Ground is a load SINK, not a conductor: two grounded pieces do not share a path THROUGH the
 * earth, so "meeting at the ground" is not a shared load path.)
 *
 * THE ASSERTION — MECHANISM, NOT DISPLACEMENT. Snapshot every wall piece's support state with the
 * structure intact, cut the column's base through the binding, run the production cascade, and
 * assert: (a) every wall piece has the IDENTICAL support state it had before; (b) no wall joint
 * broke under load and none was severed; (c) no wall piece was released to physics; and (d) the
 * whole affected set — pieces that lost the earth, joints that broke — lies entirely in the column.
 *
 * GREEN ON ARRIVAL, AND PROVEN TO BITE. Production routes by path, so this passes today. A
 * green-on-arrival test is indistinguishable from one that asserts nothing until mutated: a
 * radius-based spread added to FStructure::RemovePiece (sever every joint within ~50 cm of the cut)
 * reaches the wall 18.5 cm away and changes it — the report accompanying this file records that
 * mutation firing assertions (a)-(d), reverting, and the suite returning to green.
 *
 * NEEDS A TICKING WORLD: NO.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FLocalityChangeFarFromCutStaysPutTest,
	"DestructionGame.Acceptance.Locality.AChangeFarFromTheCutStaysPut",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FLocalityChangeFarFromCutStaysPutTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace ProductPromiseSupport;

	/* ---- Region A: the mortared column, grounded at its foot. ---- */

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

	/* ---- Region B: the running-bond wall, +40 cm on X, its own grounded base. ---- */

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

	/* ------------------------------------------------------------------ *
	 * PRECONDITION: the two regions genuinely share no load path — no
	 * connection links a column piece (< Split) to a wall piece (>= Split).
	 * ------------------------------------------------------------------ */

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

	/* ------------------------------------------------------------------ *
	 * Adopt into a binding and snapshot region B WHILE THE STRUCTURE IS INTACT.
	 * ------------------------------------------------------------------ */

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

	/* ------------------------------------------------------------------ *
	 * THE PLAYER'S MOVE: pull the column's base, run the cascade, push.
	 * ------------------------------------------------------------------ */

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

	/* ------------------------------------------------------------------ *
	 * THE PROMISE: region B is untouched, bit for bit in support state.
	 * ------------------------------------------------------------------ */

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

	/* Every wall joint: neither broke under load nor was severed. */
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

	/* ------------------------------------------------------------------ *
	 * CONFINEMENT: the whole affected set lives in region A.
	 * ------------------------------------------------------------------ */

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
 * GROUP 2a — A CUT FAR FROM THE LOAD PATH LEAVES IT STANDING.
 *
 * THE PROMISE. A believable sim is as much about what STANDS as what falls. Removing a piece that
 * carries nothing must move nothing: a parapet, a top brick, a decoration is not structure.
 *
 * THE FIXTURE. A three-wide, three-course running-bond wall (the PRODUCER), plus ONE parapet brick
 * laid on top of a middle top-course brick. It is genuinely NON-load-bearing: nothing rests on a
 * top-course brick, so it has no piece above it to support, and its own weight rests DOWN onto the
 * wall — removing it can only UNLOAD the joints below, never overload them. The test removes it.
 *
 * THE ASSERTION — OUTCOME, gravity on. After the removal cascade: zero pieces released, zero broke,
 * zero lost the earth, and every wall piece still Grounded or Supported. Displacement is never read.
 *
 * GREEN ON ARRIVAL, PROVEN TO BITE. Passes today. Bite: a radius-based sever in RemovePiece drops
 * the top course when the parapet goes — recorded in the report.
 *
 * NEEDS A TICKING WORLD: NO.
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

	/* The middle top-course brick: course 2 (Z = 3.25 + 2*7.5 = 18.25), X = 22.5. */
	const FVector TopMiddleCentre(22.5, 0.0, BrickHeightCm / 2.0 + 2.0 * CoursePitchCm);
	const int32 Support = FindPiece(L, TopMiddleCentre);

	if (Support == INDEX_NONE)
	{
		AddError(TEXT("FIXTURE: the middle top-course brick the parapet sits on must exist"));
		return false;
	}

	/* The parapet: a full brick one bed above that top brick, carrying nothing. */
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

	/* PRECONDITION: the parapet is really non-load-bearing — it has no bed joint above it, so
	 * nothing rests on it; its only joint is the one BENEATH it that this test then removes. */
	Binding.SolveLoads();
	TestTrue(
		TEXT("FIXTURE: the parapet must rest on the wall before removal (Supported)"),
		S.GetPieceSupport(Parapet) == EPieceSupport::Supported);

	/* THE PLAYER'S MOVE: pull the parapet. */
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
 * GROUP 2b — REMOVING A GENUINELY REDUNDANT MEMBER LEAVES IT STANDING.
 *
 * THE PROMISE. A load with more than one path to the ground survives losing one of them: pull a
 * redundant support and the remaining paths carry it.
 *
 * THE FIXTURE. One heavy beam resting on THREE grounded piers, at X = -40, 0, +40. Each pier is a
 * bed joint beneath the beam — three independent paths to the earth. The beam's centre of mass sits
 * at X = 0, squarely between the two END piers, so with the MIDDLE pier gone the two ends still
 * bracket the load: the beam is not overhanging, it is spanning. The middle pier is genuinely
 * REDUNDANT — the two end piers alone reach the ground and carry the split — which is why removing
 * it must leave the beam standing. (The middle, not an end: removing an end would put the centre of
 * mass outside the remaining pair, a real overturn, which is a different promise.)
 *
 * THE ASSERTION — OUTCOME. After removing the middle pier: the beam is still Supported, both end
 * piers still Grounded, zero pieces released, zero broke, zero lost the earth. Displacement unread.
 *
 * GREEN ON ARRIVAL, PROVEN TO BITE. Passes today. Bite: a radius-based sever in RemovePiece cuts the
 * beam's other two bed joints when the middle pier goes, dropping the beam — recorded in the report.
 *
 * NEEDS A TICKING WORLD: NO.
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

	/* Three grounded piers: 10 wide on X, full wythe on Y, 20 tall, tops at Z = 20. */
	constexpr double PierWidthX = 10.0;
	constexpr double PierHeightZ = 20.0;
	const FVector PierSize(PierWidthX, BrickWidthCm, PierHeightZ);

	const int32 PierLeft = AddBrick(L, FVector(-40.0, 0.0, PierHeightZ / 2.0), PierSize, /*bGrounded*/ true);
	const int32 PierMid = AddBrick(L, FVector(0.0, 0.0, PierHeightZ / 2.0), PierSize, /*bGrounded*/ true);
	const int32 PierRight = AddBrick(L, FVector(40.0, 0.0, PierHeightZ / 2.0), PierSize, /*bGrounded*/ true);

	/* The beam: 100 long on X, full wythe, 13 thick, its bottom one bed above the pier tops. */
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

	/* PRECONDITION: with all three paths present the beam is carried. */
	Binding.SolveLoads();
	TestTrue(
		TEXT("FIXTURE: the beam must be Supported with all three piers present"),
		S.GetPieceSupport(Beam) == EPieceSupport::Supported);

	/* THE PLAYER'S MOVE: pull the redundant middle pier. */
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
 * GROUP 2c — A PURE-COMPRESSION DRY STACK, STABLE BY GEOMETRY, MUST STAND (WITHOUT TENSION).
 *
 * THE PROMISE. A dry-stacked assembly holds itself up by geometry and friction alone — no bond, no
 * tension. If its resultant sits within the bearing, it stands, exactly as a real dry-stone wall
 * does. Needing tension to stand a plainly-stable dry stack is the model getting statics wrong.
 *
 * THE FIXTURE. Two DryStone bricks: a grounded base at X = 0, and one brick resting on it OFFSET by
 * 8 cm on X. The bond has zero tensile and zero cohesive strength (DryStone: c = 0, f_t = 0, mu =
 * 0.7). The stack is STABLE BY GEOMETRY: the upper brick's centre of mass sits at X = 8, and the
 * base bearing extends to X = 10.75 — the resultant is 2.75 cm inside the bearing edge, well within
 * the contact, with no horizontal force to slide it. A rigid block on this bearing does not tip and
 * does not slide: it stands, and it needs no tension to do so.
 *
 * WHY IT MAY BE RED, AND WHY THAT IS A FINDING NOT A BUG IN THE TEST. DESIGN.md's own gap list
 * (CURRENT_STATE.md, "Deliberately left alone"): "Dry stone has no rocking model ... a dry stack
 * that plainly stands reads as falling." An 8 cm offset puts the joint's resultant OUTSIDE the kern
 * (e = 4.0 cm against a kern of 2.25 cm), and with f_t = 0 the uncracked check reports net edge
 * tension against a zero limit — utilisation Max() — so the one bed joint gives and the upper brick
 * is released, though rigid-block statics stands it comfortably. If production fells the upper brick
 * for exactly that reason, this is an ASPIRATIONAL / KNOWN-GAP red (the no-rocking-model gap), to be
 * marked an in-flight red like the two-load-path overturning test — NOT a regression. It is checked
 * to be red for the RIGHT reason: the base keeps the earth, nothing is Stranded, and it is the
 * upper brick's own bed joint that gives. The interim overturning guard is NOT the cause: the upper
 * brick's centre of mass (X = 8) sits inside the bearing edge (X = 10.75), so no body has walked
 * past its seat — the guard cannot fire, and the fall is purely the joint's no-tension kern check.
 *
 * NEEDS A TICKING WORLD: NO.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMustNotFallDryStackStandsByGeometryTest,
	"DestructionGame.Acceptance.MustNotFall.APureCompressionDryStackStandsByGeometry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FMustNotFallDryStackStandsByGeometryTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace ProductPromiseSupport;

	/* PRECONDITION on the profile: this is genuinely a no-tension, no-cohesion joint. */
	TestEqual(TEXT("FIXTURE: dry stone carries exactly zero tensile strength"),
		DryStone.TensileStrengthMPa, 0.0);
	TestEqual(TEXT("FIXTURE: dry stone carries exactly zero cohesion"),
		DryStone.ShearCohesionMPa, 0.0);

	constexpr double OffsetXCm = 8.0;
	const FVector BrickSize(BrickLengthCm, BrickWidthCm, BrickHeightCm);

	/* PRECONDITION: the resultant is inside the bearing — geometrically stable, no rocking needed. */
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

	/* No removal — the action is simply settling under gravity. */
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

	/* PRECONDITION for an honest verdict either way: no routing limitation is involved. */
	TestEqual(TEXT("PRECONDITION: no piece may be Stranded"), Stranded, 0);

	TestTrue(
		TEXT("KNOWN-GAP CHECK: whatever the verdict, the grounded base must keep the earth — a red "
			 "here must be the upper brick's no-tension joint, not a broken base"),
		S.GetPieceSupport(Base) == EPieceSupport::Grounded);

	/* THE PROMISE. Aspirational: production has no dry-stone rocking model, so this may be red. */
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
