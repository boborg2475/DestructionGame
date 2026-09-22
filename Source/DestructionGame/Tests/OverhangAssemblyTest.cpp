// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"

#include "Core/RigidBlock/RigidBlockOracle.h"
#include "Core/RigidBlock/RigidBlockBridge.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Front-door overhang (SHED_PATH.md Phase C, slice C2; R-Overhang). A timber beam is carried by a
 * post in compression at the front and a screw fixing in tension at the back, neither sufficient
 * alone: it stands assembled, and falls when either the post or the anchor brick is removed.
 *
 * 2D X-Z section, four pieces: WallBase (grounded brick pier, X -40..0), WallTop (anchor brick on a
 * mortar bed), Beam (timber, X -4..196, lapping 4 cm onto WallTop), Post (grounded timber, X 54..66).
 * Joints: WallTop on WallBase (mortar), Beam on WallTop (Screw, 80 cm2, withdrawal 0.54 MPa is the
 * weakest link), Beam on Post (DryStone, no tension, 240 cm2).
 *
 * The beam centroid (X 96) is outboard of the post (X 60), which makes all three arms work:
 *   (a) assembled: the back end lifts, so the fixing is in tension, T = W*(c - X_p)/(X_p - X_f);
 *       its withdrawal capacity exceeds that by ~37x.
 *   (b) post removed: the 4 cm fixing cannot resist W*(c - X_f); demand is ~2.1x its plastic capacity.
 *   (c) anchor removed: the beam topples off the post; W*(c - X_p) is ~6x the compression-only
 *       capacity halfPost*W.
 *
 * Asserted: the LP oracle agrees (removals infeasible, with a certified mechanism that moves the
 * beam), and production's SolveAndBreak outcome matches with nothing Stranded. Units are spelled
 * out locally (10000 uu per MPa per cm2) so a wrong production constant fails here.
 */
namespace OverhangAssemblyTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/** Timber C24, EN 338 mean density. g/cm3: 0.42, never 420. */
	constexpr double TimberDensityGramsPerCubicCm = 0.42;

	constexpr double ClayDensityGramsPerCubicCm = 1.9;

	/** MassKg * 980 is a weight in uu; the 1 N = 100 uu factor is already inside it. */
	constexpr double GravityCmPerSecondSquared = 980.0;

	/** 10000 uu per MPa per cm2, a local literal so a wrong production constant fails here. */
	constexpr double ForceUnitsPerMPaSqCmHere = 100.0 * 100.0;

	/** Every piece is this deep on Y, so every joint's Y overlap is full. */
	constexpr double WytheCm = 20.0;

	constexpr double JointThicknessCm = 1.0;

	// WallBase: the grounded pier.
	constexpr double WallLeftXCm = -40.0;
	constexpr double WallRightXCm = 0.0;
	constexpr double WallBaseBottomZCm = 0.0;
	constexpr double WallBaseTopZCm = 170.0;

	// WallTop: the anchor brick, one bed joint above WallBase.
	constexpr double WallTopBottomZCm = WallBaseTopZCm + JointThicknessCm;   // 171
	constexpr double WallTopTopZCm = WallTopBottomZCm + 10.0;                // 181

	// Beam: back end laps 4 cm onto WallTop.
	constexpr double BeamLeftXCm = -4.0;
	constexpr double BeamRightXCm = 196.0;
	constexpr double BeamBottomZCm = WallTopTopZCm + JointThicknessCm;       // 182
	constexpr double BeamThicknessZCm = 12.0;
	constexpr double BeamTopZCm = BeamBottomZCm + BeamThicknessZCm;          // 194
	constexpr double BeamLengthXCm = BeamRightXCm - BeamLeftXCm;             // 200
	constexpr double BeamCentroidXCm = (BeamLeftXCm + BeamRightXCm) / 2.0;   // 96

	// Post: grounded, under the front of the beam.
	constexpr double PostLeftXCm = 54.0;
	constexpr double PostRightXCm = 66.0;
	constexpr double PostWidthXCm = PostRightXCm - PostLeftXCm;              // 12
	constexpr double PostCentreXCm = (PostLeftXCm + PostRightXCm) / 2.0;     // 60
	constexpr double PostTopZCm = BeamBottomZCm - JointThicknessCm;          // 181
	constexpr double PostBottomZCm = 0.0;

	// Fixing patch: the beam's overlap onto WallTop.
	constexpr double FixingLeftXCm = BeamLeftXCm;                            // -4
	constexpr double FixingRightXCm = WallRightXCm;                          // 0
	constexpr double FixingWidthXCm = FixingRightXCm - FixingLeftXCm;        // 4
	constexpr double FixingCentreXCm = (FixingLeftXCm + FixingRightXCm) / 2.0; // -2
	constexpr double FixingAreaSqCm = FixingWidthXCm * WytheCm;              // 80

	constexpr double PostBearingAreaSqCm = PostWidthXCm * WytheCm;           // 240

	// Independent hand statics, not mirrored from the LP.

	/** The beam's weight, uu. */
	double BeamWeightUu()
	{
		const double MassKg =
			TimberDensityGramsPerCubicCm * BeamLengthXCm * BeamThicknessZCm * WytheCm / 1000.0;
		return MassKg * GravityCmPerSecondSquared;
	}

	/** The fixing's withdrawal capacity over the whole patch, uu. */
	double FixingWithdrawalCapacityUu(double ScrewTensileMPa)
	{
		return ScrewTensileMPa * ForceUnitsPerMPaSqCmHere * FixingAreaSqCm;
	}

	/** (a) The tension the fixing carries when assembled. */
	double AssembledFixingTensionUu()
	{
		return BeamWeightUu() * (BeamCentroidXCm - PostCentreXCm) / (PostCentreXCm - FixingCentreXCm);
	}

	/** (b) The cantilever moment about the fixing's centre. */
	double CantileverDemandUuCm()
	{
		return BeamWeightUu() * (BeamCentroidXCm - FixingCentreXCm);
	}

	/**
	 * (b) The fixing's plastic moment capacity: a compression edge and a withdrawal-limited tension
	 * edge, each a half-width from centre. M = d * (W + 2 * Cap_half).
	 */
	double CantileverCapacityUuCm(double ScrewTensileMPa)
	{
		const double HalfWidthCm = FixingWidthXCm / 2.0;
		const double CapHalfUu = ScrewTensileMPa * ForceUnitsPerMPaSqCmHere * (FixingAreaSqCm / 2.0);
		return HalfWidthCm * (BeamWeightUu() + 2.0 * CapHalfUu);
	}

	/** (c) The toppling moment about the post centre. */
	double ToppleDemandUuCm()
	{
		return BeamWeightUu() * (BeamCentroidXCm - PostCentreXCm);
	}

	/** (c) The post's capacity: no tension, so at most W at a half-post-width lever. */
	double ToppleCapacityUuCm()
	{
		return (PostWidthXCm / 2.0) * BeamWeightUu();
	}

	struct FOverhang
	{
		FStructure Structure;

		int32 WallBase = INDEX_NONE;
		int32 WallTop = INDEX_NONE;
		int32 Beam = INDEX_NONE;
		int32 Post = INDEX_NONE;

		int32 WallBedJoint = INDEX_NONE;
		int32 FixingJoint = INDEX_NONE;
		int32 PostJoint = INDEX_NONE;
	};

	FPieceBox MakeBox(double LeftX, double RightX, double BottomZ, double TopZ)
	{
		FPieceBox Box;
		Box.ExtentCm = FVector((RightX - LeftX) * 0.5, WytheCm * 0.5, (TopZ - BottomZ) * 0.5);
		Box.CentreCm = FVector((LeftX + RightX) * 0.5, 0.0, (BottomZ + TopZ) * 0.5);
		return Box;
	}

	double BoxMassKg(const FPieceBox& Box, double DensityGramsPerCubicCm)
	{
		return DensityGramsPerCubicCm
			* (Box.ExtentCm.X * 2.0) * (Box.ExtentCm.Y * 2.0) * (Box.ExtentCm.Z * 2.0) / 1000.0;
	}

	/** Lay the four pieces, set materials, and join the three bed joints. */
	void Build(FOverhang& Out)
	{
		const FPieceBox WallBaseBox = MakeBox(WallLeftXCm, WallRightXCm, WallBaseBottomZCm, WallBaseTopZCm);
		const FPieceBox WallTopBox = MakeBox(WallLeftXCm, WallRightXCm, WallTopBottomZCm, WallTopTopZCm);
		const FPieceBox BeamBox = MakeBox(BeamLeftXCm, BeamRightXCm, BeamBottomZCm, BeamTopZCm);
		const FPieceBox PostBox = MakeBox(PostLeftXCm, PostRightXCm, PostBottomZCm, PostTopZCm);

		Out.WallBase = Out.Structure.AddPiece(
			BoxMassKg(WallBaseBox, ClayDensityGramsPerCubicCm), /*bIsGrounded*/ true, WallBaseBox.CentreCm);
		Out.WallTop = Out.Structure.AddPiece(
			BoxMassKg(WallTopBox, ClayDensityGramsPerCubicCm), /*bIsGrounded*/ false, WallTopBox.CentreCm);
		Out.Beam = Out.Structure.AddPiece(
			BoxMassKg(BeamBox, TimberDensityGramsPerCubicCm), /*bIsGrounded*/ false, BeamBox.CentreCm);
		Out.Post = Out.Structure.AddPiece(
			BoxMassKg(PostBox, TimberDensityGramsPerCubicCm), /*bIsGrounded*/ true, PostBox.CentreCm);

		Out.Structure.SetPieceMaterial(Out.WallBase, &ClayBrick);
		Out.Structure.SetPieceMaterial(Out.WallTop, &ClayBrick);
		Out.Structure.SetPieceMaterial(Out.Beam, &Timber);
		Out.Structure.SetPieceMaterial(Out.Post, &Timber);

		FConnection Joint;

		if (MakeInterface(Out.WallBase, WallBaseBox, Out.WallTop, WallTopBox,
				JointThicknessCm, GeneralPurposeMortar, Joint))
		{
			Out.WallBedJoint = Out.Structure.AddConnection(Joint);
		}

		if (MakeInterface(Out.WallTop, WallTopBox, Out.Beam, BeamBox,
				JointThicknessCm, Screw, Joint))
		{
			Out.FixingJoint = Out.Structure.AddConnection(Joint);
		}

		if (MakeInterface(Out.Post, PostBox, Out.Beam, BeamBox,
				JointThicknessCm, DryStone, Joint))
		{
			Out.PostJoint = Out.Structure.AddConnection(Joint);
		}
	}

	int32 StrandedCount(const FStructure& S)
	{
		int32 Stranded = 0;
		for (int32 Piece = 0; Piece < S.NumPieces(); ++Piece)
		{
			if (!S.IsPieceRemoved(Piece) && S.GetPieceSupport(Piece) == EPieceSupport::Stranded)
			{
				++Stranded;
			}
		}
		return Stranded;
	}

	/** True when a live piece has no path to the earth. */
	bool HasLostTheEarth(const FStructure& S, int32 Piece)
	{
		if (S.IsPieceRemoved(Piece))
		{
			return false;
		}
		const EPieceSupport Support = S.GetPieceSupport(Piece);
		return Support != EPieceSupport::Grounded && Support != EPieceSupport::Supported;
	}

	bool IsStanding(EPieceSupport Support)
	{
		return Support == EPieceSupport::Grounded || Support == EPieceSupport::Supported;
	}

	/** The oracle block built from this piece. */
	int32 OracleBlockOfPiece(const RigidBlockOracle::FOracleProblem& Problem, int32 Piece)
	{
		for (int32 B = 0; B < Problem.PieceOfBlock.Num(); ++B)
		{
			if (Problem.PieceOfBlock[B] == Piece)
			{
				return B;
			}
		}
		return INDEX_NONE;
	}
}

/** The overhang stands on post plus fixing and falls when either is removed. See the file header. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FOverhangAssemblyTest,
	"DestructionGame.Acceptance.Overhang.CarriedByPostsInCompressionAndAWallFixingInTension",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FOverhangAssemblyTest::RunTest(const FString& Parameters)
{
	using namespace DestructionProfiles;
	using namespace OverhangAssemblyTestSupport;

	// Pin the published strengths the sizing was derived against.

	TestEqual(TEXT("FIXTURE: the wall-fixing is a Screw, withdrawal 0.54 MPa (EN 1995-1-1 8.7.2)"),
		Screw.TensileStrengthMPa, 0.54);
	TestEqual(TEXT("FIXTURE: the post bearing is DryStone — a frictional contact with NO tension"),
		DryStone.TensileStrengthMPa, 0.0);
	TestEqual(TEXT("FIXTURE: Timber C24 crushes at 29 MPa (mean f_c,0)"),
		Timber.Strength.CompressiveStrengthMPa, 29.0);
	TestEqual(TEXT("FIXTURE: Timber C24 pulls apart at 23 MPa (mean f_t,0) — genuinely tension-capable"),
		Timber.Strength.TensileStrengthMPa, 23.0);
	TestEqual(TEXT("FIXTURE: clay brick crushes at 20 MPa"),
		ClayBrick.Strength.CompressiveStrengthMPa, 20.0);

	// Weakest link on tension: min(Screw 0.54, Timber 23, ClayBrick 2) = the screw's withdrawal.
	const double FixingTensileMPa = FMath::Min3(
		Screw.TensileStrengthMPa, Timber.Strength.TensileStrengthMPa, ClayBrick.Strength.TensileStrengthMPa);
	TestEqual(TEXT("FIXTURE: the fixing's weakest-link withdrawal is the screw's 0.54 MPa"),
		FixingTensileMPa, 0.54);

	// Hand-derived sizing: each support alone is insufficient, together comfortable.

	const double W = BeamWeightUu();
	const double AssembledTension = AssembledFixingTensionUu();
	const double FixingCap = FixingWithdrawalCapacityUu(Screw.TensileStrengthMPa);

	const double CantDemand = CantileverDemandUuCm();
	const double CantCap = CantileverCapacityUuCm(Screw.TensileStrengthMPa);

	const double ToppleDemand = ToppleDemandUuCm();
	const double ToppleCap = ToppleCapacityUuCm();

	AddInfo(FString::Printf(
		TEXT("DERIVED: beam weight %.10g uu, centroid X %.10g, post X %.10g, fixing X %.10g. "
			 "(a) assembled fixing tension %.10g uu vs withdrawal capacity %.10g uu (stands, margin %.3gx). "
			 "(b) cantilever moment %.10g vs fixing capacity %.10g uu.cm (falls, %.3gx). "
			 "(c) topple moment %.10g vs post capacity %.10g uu.cm (falls, %.3gx)."),
		W, BeamCentroidXCm, PostCentreXCm, FixingCentreXCm,
		AssembledTension, FixingCap, FixingCap / AssembledTension,
		CantDemand, CantCap, CantDemand / CantCap,
		ToppleDemand, ToppleCap, ToppleDemand / ToppleCap));

	TestTrue(
		*FString::Printf(TEXT("SIZING (a): the fixing's withdrawal (%.10g) must comfortably exceed the "
			"assembled tension (%.10g) — a stand, not a knife edge"), FixingCap, AssembledTension),
		FixingCap > 3.0 * AssembledTension);

	TestTrue(
		*FString::Printf(TEXT("SIZING (b): the cantilever moment (%.10g) must outrun the fixing's plastic "
			"capacity (%.10g) — the fixing ALONE cannot cantilever the beam"), CantDemand, CantCap),
		CantDemand > 1.5 * CantCap);

	TestTrue(
		*FString::Printf(TEXT("SIZING (c): the topple moment (%.10g) must outrun the post's compression-only "
			"capacity (%.10g) — the post ALONE lets it topple"), ToppleDemand, ToppleCap),
		ToppleDemand > 1.5 * ToppleCap);

	// Each arm builds a fresh overhang, removes its piece, then reads the oracle and production.

	enum class EArm : uint8 { Assembled, PostsRemoved, FixingRemoved };

	struct FArm
	{
		EArm Arm;
		const TCHAR* Label;
		bool bExpectStands;
	};

	const FArm Arms[3] = {
		{ EArm::Assembled,     TEXT("(a) assembled"),        true },
		{ EArm::PostsRemoved,  TEXT("(b) posts removed"),    false },
		{ EArm::FixingRemoved, TEXT("(c) wall-fixing removed"), false },
	};

	for (const FArm& A : Arms)
	{
		FOverhang Fx;
		Build(Fx);

		if (Fx.WallBedJoint == INDEX_NONE || Fx.FixingJoint == INDEX_NONE || Fx.PostJoint == INDEX_NONE)
		{
			AddError(FString::Printf(TEXT("%s: FIXTURE: the producer must emit all three bed joints"), A.Label));
			return false;
		}

		if (A.Arm == EArm::Assembled)
		{
			TestEqual(TEXT("FIXTURE: four pieces — wall base, anchor brick, overhang beam, post"),
				Fx.Structure.NumPieces(), 4);
			TestEqual(TEXT("FIXTURE: three joints — wall bed, screw fixing, post bearing"),
				Fx.Structure.NumConnections(), 3);
			TestTrue(TEXT("FIXTURE: every piece and joint must know where it is, or there are no lever arms"),
				Fx.Structure.HasCompleteGeometry());
			TestTrue(TEXT("FIXTURE: the beam bears on the post through a BED joint (a compression bearing)"),
				Fx.Structure.GetJointRole(Fx.PostJoint, Fx.Beam) == EJointRole::BedBeneath);
			TestTrue(TEXT("FIXTURE: the fixing is a BED joint under the beam's back end (the tension tie)"),
				Fx.Structure.GetJointRole(Fx.FixingJoint, Fx.Beam) == EJointRole::BedBeneath);
			TestEqual(TEXT("FIXTURE: the fixing patch is the 4 cm x 20 cm lap, 80 cm2"),
				Fx.Structure.GetConnection(Fx.FixingJoint).InterfaceAreaSqCm, FixingAreaSqCm);
			TestEqual(TEXT("FIXTURE: the post bearing is the 12 cm x 20 cm face, 240 cm2"),
				Fx.Structure.GetConnection(Fx.PostJoint).InterfaceAreaSqCm, PostBearingAreaSqCm);
		}

		if (A.Arm == EArm::PostsRemoved)
		{
			Fx.Structure.RemovePiece(Fx.Post);
		}
		else if (A.Arm == EArm::FixingRemoved)
		{
			Fx.Structure.RemovePiece(Fx.WallTop);
		}

		RigidBlockOracle::FOracleProblem Problem;
		FString BridgeWhy;
		const bool bBridged = RigidBlockOracle::BuildRigidBlockProblem(Fx.Structure, Problem, BridgeWhy);

		TestTrue(
			*FString::Printf(TEXT("%s: the oracle bridge must accept this 2D overhang (%s)"), A.Label, *BridgeWhy),
			bBridged);

		if (bBridged)
		{
			const RigidBlockOracle::FOracleResult Live = RigidBlockOracle::SolveRigidBlock(Problem);
			const RigidBlockOracle::EOracleOutcome Outcome = RigidBlockOracle::OutcomeOf(Live);

			AddInfo(FString::Printf(
				TEXT("%s: oracle answered %d, lambda* %.10g, outcome %d (2=Stands,1=Falls,0=Unanswerable)"),
				A.Label, Live.bAnswered ? 1 : 0, Live.Lambda, static_cast<int32>(Outcome)));

			TestTrue(
				*FString::Printf(TEXT("%s: the oracle must ANSWER (a refusal proves nothing)"), A.Label),
				Live.bAnswered);

			TestEqual(
				*FString::Printf(TEXT("%s: the LP feasibility must match R-Overhang — %s"),
					A.Label, A.bExpectStands ? TEXT("assembled STANDS") : TEXT("the removal FALLS")),
				static_cast<int32>(Outcome),
				static_cast<int32>(A.bExpectStands
					? RigidBlockOracle::EOracleOutcome::Stands
					: RigidBlockOracle::EOracleOutcome::Falls));

			if (A.bExpectStands)
			{
				TestTrue(
					*FString::Printf(TEXT("%s: lambda* %.10g must sit at or above 1 — an admissible equilibrium"),
						A.Label, Live.Lambda),
					Live.bAnswered && Live.Lambda >= 1.0);
			}
			else
			{
				TestTrue(
					*FString::Printf(TEXT("%s: lambda* %.10g must sit clearly below 1 — no admissible equilibrium"),
						A.Label, Live.Lambda),
					Live.bAnswered && Live.Lambda < 0.9);

				// The gravity-dead mechanism must move the beam, so the fall is real, not a routing artefact.
				RigidBlockOracle::FOracleProblem Dead = Problem;
				Dead.bGravityIsLive = false;
				const RigidBlockOracle::FOracleResult DeadR = RigidBlockOracle::SolveRigidBlock(Dead);

				const int32 BeamBlock = OracleBlockOfPiece(Dead, Fx.Beam);
				const bool bBeamMoves = DeadR.Mechanism.bPresent
					&& DeadR.Mechanism.Blocks.IsValidIndex(BeamBlock)
					&& DeadR.Mechanism.Blocks[BeamBlock].bMoves;

				AddInfo(FString::Printf(
					TEXT("%s: mechanism present %d, certified %d, beam is oracle block %d, beam moves %d"),
					A.Label, DeadR.Mechanism.bPresent ? 1 : 0, DeadR.Mechanism.bIsCertified ? 1 : 0,
					BeamBlock, bBeamMoves ? 1 : 0));

				TestTrue(
					*FString::Printf(TEXT("%s: the LP must extract a certified collapse mechanism"), A.Label),
					DeadR.Mechanism.bPresent && DeadR.Mechanism.bIsCertified);

				TestTrue(
					*FString::Printf(TEXT("%s: the mechanism must NAME THE BEAM as a moving block — the "
						"overhang is what loses equilibrium"), A.Label),
					bBeamMoves);
			}
		}

		// Below the 200-block cap, so the LP is the break authority.
		const int32 Passes = Fx.Structure.SolveAndBreak();

		const EPieceSupport BeamSupport = Fx.Structure.GetPieceSupport(Fx.Beam);
		const int32 Stranded = StrandedCount(Fx.Structure);

		AddInfo(FString::Printf(
			TEXT("%s: PRODUCTION ran %d breaking pass(es); beam support %d "
				 "(1=Grounded,2=Supported,3=Stranded,0=Falling); %d stranded"),
			A.Label, Passes, static_cast<int32>(BeamSupport), Stranded));

		TestEqual(
			*FString::Printf(TEXT("%s: nothing may be Stranded — a verdict must be about the masonry, not the "
				"solver declining to route"), A.Label),
			Stranded, 0);

		TestTrue(
			*FString::Printf(TEXT("%s: the grounded wall base keeps the earth"), A.Label),
			Fx.Structure.GetPieceSupport(Fx.WallBase) == EPieceSupport::Grounded);

		if (A.bExpectStands)
		{
			TestEqual(
				*FString::Printf(TEXT("%s: the overhang STANDS — the beam reads Supported after the cascade, "
					"support %d"), A.Label, static_cast<int32>(BeamSupport)),
				static_cast<int32>(BeamSupport), static_cast<int32>(EPieceSupport::Supported));

			TestTrue(
				*FString::Printf(TEXT("%s: the grounded post keeps the earth (the compression support)"), A.Label),
				Fx.Structure.GetPieceSupport(Fx.Post) == EPieceSupport::Grounded);
		}
		else
		{
			TestTrue(
				*FString::Printf(TEXT("%s: the overhang FALLS — the beam loses the earth (support %d), because "
					"R-Overhang says neither support holds it alone"), A.Label, static_cast<int32>(BeamSupport)),
				HasLostTheEarth(Fx.Structure, Fx.Beam));

			// The remaining support keeps the earth.
			if (A.Arm == EArm::PostsRemoved)
			{
				TestTrue(
					*FString::Printf(TEXT("%s: the anchor brick it hung from keeps the earth"), A.Label),
					IsStanding(Fx.Structure.GetPieceSupport(Fx.WallTop)));
			}
			else
			{
				TestTrue(
					*FString::Printf(TEXT("%s: the grounded post keeps the earth"), A.Label),
					Fx.Structure.GetPieceSupport(Fx.Post) == EPieceSupport::Grounded);
			}
		}
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
