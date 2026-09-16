// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"
#include "Core/BuildMode/JointInference.h"
#include "Core/Connection.h"
#include "Core/Layout.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Profiles/ConnectionProfiles.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Unit test for automatic joint inference — the owner-delegated ruling in
 * BUILD_MODE_PLAN.md (2026-09-03).
 *
 * Pure and world-free by design: no gravity, no solver, no ticking. The mechanism
 * under test is a material-pairing + interface-normal decision, so the assertion
 * is on the returned PROFILE'S IDENTITY, not on any load, displacement or solve.
 *
 * WHY COMPARE EVERY FIELD. FConnectionStrength has no operator==, so identity is
 * pinned by matching all five fields against the named library constant. The three
 * profiles the ruling can return are mutually distinguishable on those fields —
 *   GeneralPurposeMortar         C10 / coh0.9 / T0.7 / mu0.75 / maxS2.0
 *   GeneralPurposeMortarPerpend  C10 / coh0.2 / T0.1 / mu0.75 / maxS2.0
 *   DryStone                     C30 / coh0.0 / T0.0 / mu0.70 / maxS6.0
 * so a full-field match cannot accept a sibling profile: bed vs head differ on the
 * two bond axes, and either masonry row vs DryStone differ on compression, friction
 * and the shear ceiling. Asserting one field alone (say cohesion) would let a wrong
 * masonry/dry-stone swap through, hence all five.
 *
 * One parameterised test over a table, so adding a material pairing is data.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FJointInferenceTest,
	"DestructionGame.Core.BuildMode.JointInference",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

/*
 * NAMED NAMESPACE, named distinctly from every other one in this module — an
 * anonymous namespace is private to a TRANSLATION UNIT, not a file, and a unity
 * build merges files. See ConnectionLoadTest.cpp for the incident that established
 * this rule; the `using namespace` lives inside RunTest for the same reason.
 */
namespace JointInferenceTestSupport
{
	struct FInferenceCase
	{
		const TCHAR* Description;
		const DestructionProfiles::FMaterialProfile* FaceA;
		const DestructionProfiles::FMaterialProfile* FaceB;
		FVector InterfaceNormalUnit;
		const FConnectionStrength* Expected;
		const TCHAR* ExpectedName;
	};

	/*
	 * THE THREE FOOTPRINTS THE CORNER RULING TURNS ON, as HALF-extents (FPieceBox
	 * stores half-sizes, matching FBox::GetExtent). A UK metric brick is 21.5 x 10.25
	 * x 6.5 cm, so a stretcher laid along X is (10.75, 5.125, 3.25) and the same brick
	 * turned to run along Y is (5.125, 10.75, 3.25) — the SAME piece, rotated, which is
	 * exactly what a quoin is made of. The square block has no long axis at all.
	 */
	const FVector XLongBrick(10.75, 5.125, 3.25);
	const FVector YLongBrick(5.125, 10.75, 3.25);
	const FVector SquareBlock(5.0, 5.0, 3.0);

	/*
	 * A box whose in-plane half-extents differ by LESS than the 0.5 cm tolerance the
	 * ruling gives "no long axis", and one that differs by MORE. 5.2 vs 5.0 is 0.2 apart
	 * (no long axis -> Head); 5.6 vs 5.0 is 0.6 apart (X-long, so it can make a corner).
	 * Together they pin the tolerance from both sides rather than only exact equality.
	 */
	const FVector NearSquareBlock(5.2, 5.0, 3.0);
	const FVector BarelyXLongBlock(5.6, 5.0, 3.0);

	double MakeNaN()
	{
		return std::numeric_limits<double>::quiet_NaN();
	}

	double MakeInf()
	{
		return std::numeric_limits<double>::infinity();
	}

	const TCHAR* ContactName(BuildMode::EMasonryContact Contact)
	{
		switch (Contact)
		{
		case BuildMode::EMasonryContact::Bed:
			return TEXT("Bed");
		case BuildMode::EMasonryContact::Head:
			return TEXT("Head");
		case BuildMode::EMasonryContact::Corner:
			return TEXT("Corner");
		default:
			return TEXT("<unknown>");
		}
	}

	/*
	 * Full-field profile identity, the same discipline the table above uses inline:
	 * FConnectionStrength has no operator==, and the three profiles the ruling can
	 * return are mutually distinguishable only across all five fields.
	 */
	void CheckProfileIdentity(
		FAutomationTestBase& Test,
		const FString& Prefix,
		const FConnectionStrength& Got,
		const FConnectionStrength& Want)
	{
		Test.TestEqual(Prefix + TEXT("CompressiveStrengthMPa"),
			Got.CompressiveStrengthMPa, Want.CompressiveStrengthMPa);
		Test.TestEqual(Prefix + TEXT("ShearCohesionMPa"),
			Got.ShearCohesionMPa, Want.ShearCohesionMPa);
		Test.TestEqual(Prefix + TEXT("TensileStrengthMPa"),
			Got.TensileStrengthMPa, Want.TensileStrengthMPa);
		Test.TestEqual(Prefix + TEXT("FrictionCoefficient"),
			Got.FrictionCoefficient, Want.FrictionCoefficient);
		Test.TestEqual(Prefix + TEXT("MaxShearStrengthMPa"),
			Got.MaxShearStrengthMPa, Want.MaxShearStrengthMPa);
	}

	/** One row of the orientation-classification table: two real boxes and a normal. */
	struct FClassifyCase
	{
		const TCHAR* Description;
		DestructionLayout::FPieceBox BoxA;
		DestructionLayout::FPieceBox BoxB;
		FVector InterfaceNormalUnit;
		BuildMode::EMasonryContact Expected;
	};

	/** One row of the boxed-overload table: materials, boxes, normal, expected profile. */
	struct FBoxedInferenceCase
	{
		const TCHAR* Description;
		const DestructionProfiles::FMaterialProfile* FaceA;
		const DestructionProfiles::FMaterialProfile* FaceB;
		DestructionLayout::FPieceBox BoxA;
		DestructionLayout::FPieceBox BoxB;
		FVector InterfaceNormalUnit;
		const FConnectionStrength* Expected;
		const TCHAR* ExpectedName;
	};
}

bool FJointInferenceTest::RunTest(const FString& Parameters)
{
	using namespace JointInferenceTestSupport;
	using namespace DestructionProfiles;

	const TArray<FInferenceCase> Cases = {
		/*
		 * Both faces compression-dominant masonry, normal VERTICAL (|Z| dominant) —
		 * a bed joint. Passive bed → GeneralPurposeMortar. Both signs of Z, because
		 * which piece is "A" must not flip the answer.
		 */
		{
			TEXT("brick/brick, +Z bed joint -> GeneralPurposeMortar"),
			&ClayBrick, &ClayBrick, FVector(0.0, 0.0, 1.0),
			&GeneralPurposeMortar, TEXT("GeneralPurposeMortar")
		},
		{
			TEXT("brick/brick, -Z bed joint -> GeneralPurposeMortar"),
			&ClayBrick, &ClayBrick, FVector(0.0, 0.0, -1.0),
			&GeneralPurposeMortar, TEXT("GeneralPurposeMortar")
		},

		/*
		 * Both masonry, normal HORIZONTAL — a head joint (±X) or a ±Y horizontal contact (the 3-arg cannot see a corner)
		 * (±Y). The weak perpend → GeneralPurposeMortarPerpend.
		 */
		{
			TEXT("brick/brick, +X head joint -> GeneralPurposeMortarPerpend"),
			&ClayBrick, &ClayBrick, FVector(1.0, 0.0, 0.0),
			&GeneralPurposeMortarPerpend, TEXT("GeneralPurposeMortarPerpend")
		},
		{
			TEXT("brick/brick, +Y horizontal (3-arg: no corner) -> GeneralPurposeMortarPerpend"),
			&ClayBrick, &ClayBrick, FVector(0.0, 1.0, 0.0),
			&GeneralPurposeMortarPerpend, TEXT("GeneralPurposeMortarPerpend")
		},

		/*
		 * NEGATIVE horizontal normals — regression pins. MakeInterface orients the
		 * normal by piece B, so a real head joint carries a negative normal half the
		 * time. Without these, an impl that dropped the Abs on one axis (e.g.
		 * `AbsZ >= InterfaceNormalUnit.X`) evaluates 0 >= -1 -> true, misreads a head
		 * joint at (-1,0,0) as a bed joint, and returns the 4.5x-too-strong mortar.
		 */
		{
			TEXT("brick/brick, -X head joint -> GeneralPurposeMortarPerpend"),
			&ClayBrick, &ClayBrick, FVector(-1.0, 0.0, 0.0),
			&GeneralPurposeMortarPerpend, TEXT("GeneralPurposeMortarPerpend")
		},
		{
			TEXT("brick/brick, -Y horizontal (3-arg: no corner) -> GeneralPurposeMortarPerpend"),
			&ClayBrick, &ClayBrick, FVector(0.0, -1.0, 0.0),
			&GeneralPurposeMortarPerpend, TEXT("GeneralPurposeMortarPerpend")
		},

		/*
		 * Either face NOT compression-dominant (timber today) → DryStone, a passive
		 * compression + friction bearing carrying no tension, REGARDLESS of normal
		 * orientation and REGARDLESS of ordering. Both normals, both orderings.
		 */
		{
			TEXT("timber/brick, vertical normal -> DryStone"),
			&Timber, &ClayBrick, FVector(0.0, 0.0, 1.0),
			&DryStone, TEXT("DryStone")
		},
		{
			TEXT("brick/timber, vertical normal -> DryStone"),
			&ClayBrick, &Timber, FVector(0.0, 0.0, 1.0),
			&DryStone, TEXT("DryStone")
		},
		{
			TEXT("timber/brick, horizontal normal -> DryStone"),
			&Timber, &ClayBrick, FVector(1.0, 0.0, 0.0),
			&DryStone, TEXT("DryStone")
		},
		{
			TEXT("brick/timber, horizontal normal -> DryStone"),
			&ClayBrick, &Timber, FVector(0.0, 1.0, 0.0),
			&DryStone, TEXT("DryStone")
		},
	};

	for (const FInferenceCase& Case : Cases)
	{
		const FConnectionStrength Got = BuildMode::JointForContact(
			*Case.FaceA, *Case.FaceB, Case.InterfaceNormalUnit);
		const FConnectionStrength& Want = *Case.Expected;

		const FString Prefix = FString::Printf(
			TEXT("%s (expected %s): "), Case.Description, Case.ExpectedName);

		TestEqual(Prefix + TEXT("CompressiveStrengthMPa"),
			Got.CompressiveStrengthMPa, Want.CompressiveStrengthMPa);
		TestEqual(Prefix + TEXT("ShearCohesionMPa"),
			Got.ShearCohesionMPa, Want.ShearCohesionMPa);
		TestEqual(Prefix + TEXT("TensileStrengthMPa"),
			Got.TensileStrengthMPa, Want.TensileStrengthMPa);
		TestEqual(Prefix + TEXT("FrictionCoefficient"),
			Got.FrictionCoefficient, Want.FrictionCoefficient);
		TestEqual(Prefix + TEXT("MaxShearStrengthMPa"),
			Got.MaxShearStrengthMPa, Want.MaxShearStrengthMPa);
	}

	return true;
}

/**
 * THE BEHAVIOUR, IN ONE SENTENCE. BuildMode::ClassifyMasonryContact tells a BED joint
 * from a same-course HEAD joint from a bonded CORNER (quoin) using the two pieces'
 * BOXES as well as the interface normal — because a head joint's normal is parallel to
 * both bricks' long axes while a corner's is parallel to one and perpendicular to the
 * other, and the normal alone therefore cannot tell them apart (DESIGN §8, 2026-09-15).
 *
 * THE ASSERTION IS THE MECHANISM, not a profile: this test pins the CLASSIFICATION
 * (an exact, three-valued enum), which is immune to jitter and is the thing the ruling
 * actually adds. The profile mapping that hangs off it is pinned separately by
 * CornerContactGetsFullMortar, so a bug in one cannot be masked by the other.
 *
 * WHY THE BOXES CARRY REAL CENTRES. Every row's two boxes genuinely abut across the
 * given normal (one joint's separation on the normal's axis, positive overlap on the
 * other two), so an implementation that tried to classify from the CENTRE DIFFERENCE
 * rather than from the extents is not accidentally excused by degenerate fixtures —
 * it would have to get the real geometry right, which for the quoin row it cannot
 * (Core/Layout.h documents why a centroid direction is not an interface normal).
 *
 * NEEDS A TICKING WORLD: NO. Pure arithmetic over boxes and a vector.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FClassifyMasonryContactByOrientationTest,
	"DestructionGame.Core.BuildMode.ClassifyMasonryContactByOrientation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FClassifyMasonryContactByOrientationTest::RunTest(const FString& Parameters)
{
	using namespace JointInferenceTestSupport;
	using namespace DestructionLayout;
	using namespace BuildMode;

	const double NaNValue = MakeNaN();
	const double InfValue = MakeInf();

	/*
	 * The geometry vocabulary, all on the 22.5 x 11.25 x 7.5 coordinating grid a
	 * 21.5 x 10.25 x 6.5 brick on 1 cm joints gives (Core/Layout.h), and all resting
	 * on the ground (bottom face at Z = 0) per the 2026-09-15 course convention.
	 */
	const FPieceBox Stretcher{ FVector(0.0, 0.0, 3.25), XLongBrick };
	const FPieceBox SameCourseNeighbour{ FVector(22.5, 0.0, 3.25), XLongBrick };
	const FPieceBox NextCourseAbove{ FVector(11.25, 0.0, 10.75), XLongBrick };

	/*
	 * THE QUOIN. The returning brick runs along Y, abuts the stretcher's +X END face
	 * across one 1 cm joint (its near face at X = 11.75 against the stretcher's at
	 * 10.75) and its outer Y face is FLUSH with the stretcher's (both at Y = -5.125),
	 * which is what makes it a corner rather than a stack of two independent walls.
	 */
	const FPieceBox QuoinReturnAcrossX{ FVector(16.875, 5.625, 3.25), YLongBrick };

	// The same perpendicular pairing met across a Y face instead — the other quoin normal.
	const FPieceBox QuoinReturnAcrossY{ FVector(0.0, 16.875, 3.25), YLongBrick };

	const FPieceBox YRunStretcher{ FVector(0.0, 0.0, 3.25), YLongBrick };
	const FPieceBox YRunNeighbour{ FVector(0.0, 22.5, 3.25), YLongBrick };
	const FPieceBox YRunAcrossX{ FVector(11.25, 0.0, 3.25), YLongBrick };

	const FPieceBox Square{ FVector(0.0, 0.0, 3.0), SquareBlock };
	const FPieceBox SquareNeighbour{ FVector(11.0, 0.0, 3.0), SquareBlock };
	const FPieceBox XLongBesideSquare{ FVector(16.75, 0.0, 3.0), XLongBrick };
	const FPieceBox XLongAheadOfSquare{ FVector(0.0, 11.125, 3.0), XLongBrick };

	const FPieceBox NearSquare{ FVector(11.2, 0.0, 3.0), NearSquareBlock };
	const FPieceBox BarelyXLong{ FVector(11.6, 0.0, 3.0), BarelyXLongBlock };
	const FPieceBox YLongBesideBlock{ FVector(11.125, 0.0, 3.0), YLongBrick };

	const FPieceBox NaNBox{
		FVector(11.0, 0.0, 3.0), FVector(NaNValue, NaNValue, NaNValue) };

	const TArray<FClassifyCase> Cases = {
		/*
		 * VERTICAL-DOMINANT NORMAL -> BED, whatever the two footprints are. Both signs,
		 * because MakeInterface orients the normal by piece B and a real bed joint
		 * carries a negative normal half the time.
		 */
		{ TEXT("two X-long bricks stacked, +Z"),
			Stretcher, NextCourseAbove, FVector(0.0, 0.0, 1.0), EMasonryContact::Bed },
		{ TEXT("two X-long bricks stacked, -Z"),
			Stretcher, NextCourseAbove, FVector(0.0, 0.0, -1.0), EMasonryContact::Bed },

		/*
		 * HORIZONTAL NORMAL, LONG AXES PARALLEL -> HEAD. Two stretchers end to end in
		 * the same course make the genuinely weak perpend the 2026-09-03 ruling named.
		 */
		{ TEXT("two X-long bricks side by side along X, +X"),
			Stretcher, SameCourseNeighbour, FVector(1.0, 0.0, 0.0), EMasonryContact::Head },
		{ TEXT("two X-long bricks side by side along X, -X"),
			Stretcher, SameCourseNeighbour, FVector(-1.0, 0.0, 0.0), EMasonryContact::Head },
		{ TEXT("two Y-long bricks along Y, +Y"),
			YRunStretcher, YRunNeighbour, FVector(0.0, 1.0, 0.0), EMasonryContact::Head },
		{ TEXT("two Y-long bricks along Y, -Y"),
			YRunStretcher, YRunNeighbour, FVector(0.0, -1.0, 0.0), EMasonryContact::Head },

		/*
		 * PARALLELISM IS A PROPERTY OF THE PIECES, NOT OF THE NORMAL. Two Y-long bricks
		 * meeting across an X face are still both running the same way, so this is a head
		 * joint too — a row that discriminates "long axes agree" from "the normal is the
		 * long axis of A".
		 */
		{ TEXT("two Y-long bricks meeting across an X face, +X"),
			YRunStretcher, YRunAcrossX, FVector(1.0, 0.0, 0.0), EMasonryContact::Head },

		/*
		 * HORIZONTAL NORMAL, LONG AXES PERPENDICULAR -> CORNER. The quoin, across either
		 * horizontal axis and in both signs: this is the case the ruling adds, and the one
		 * the COARSE sheds' hand-authored corners (Build / BuildRecognizable) treat as bonded masonry — BuildRealistic's sweep deliberately does not, and the ruling is scoped to the interactive build.
		 */
		{ TEXT("X-long meets Y-long across an X face, +X (the quoin)"),
			Stretcher, QuoinReturnAcrossX, FVector(1.0, 0.0, 0.0), EMasonryContact::Corner },
		{ TEXT("X-long meets Y-long across an X face, -X"),
			Stretcher, QuoinReturnAcrossX, FVector(-1.0, 0.0, 0.0), EMasonryContact::Corner },
		{ TEXT("X-long meets Y-long across a Y face, +Y"),
			Stretcher, QuoinReturnAcrossY, FVector(0.0, 1.0, 0.0), EMasonryContact::Corner },
		{ TEXT("X-long meets Y-long across a Y face, -Y"),
			Stretcher, QuoinReturnAcrossY, FVector(0.0, -1.0, 0.0), EMasonryContact::Corner },

		/*
		 * NO LONG AXIS -> HEAD, FAIL CLOSED. A square footprint has no orientation to
		 * agree or disagree with, so it takes the WEAKER of the two masonry answers: a
		 * square block can never be credited with a quoin's full mortar on geometry
		 * nobody can read as interlocked.
		 */
		{ TEXT("square block against an X-long brick, +X -> the weaker answer"),
			Square, XLongBesideSquare, FVector(1.0, 0.0, 0.0), EMasonryContact::Head },
		{ TEXT("square block against an X-long brick, +Y -> the weaker answer"),
			Square, XLongAheadOfSquare, FVector(0.0, 1.0, 0.0), EMasonryContact::Head },
		{ TEXT("two square blocks, +X -> the weaker answer"),
			Square, SquareNeighbour, FVector(1.0, 0.0, 0.0), EMasonryContact::Head },

		/*
		 * THE 0.5 cm TOLERANCE, BOTH SIDES, measured on the HALF-extents. 5.2 vs 5.0 is
		 * 0.2 apart and reads as square (Head); 5.6 vs 5.0 is 0.6 apart and is genuinely
		 * X-long, so against a Y-long piece it is a corner. Without the second row a
		 * "difference > 0 means long" implementation and a "difference > 0.5" one are
		 * indistinguishable; without the first, an exact-equality one is.
		 */
		{ TEXT("near-square block (0.2 cm apart) against a Y-long brick, +X"),
			YLongBesideBlock, NearSquare, FVector(1.0, 0.0, 0.0), EMasonryContact::Head },
		{ TEXT("barely X-long block (0.6 cm apart) against a Y-long brick, +X"),
			YLongBesideBlock, BarelyXLong, FVector(1.0, 0.0, 0.0), EMasonryContact::Corner },

		/*
		 * TILTED NORMALS — which component DOMINATES decides bed vs horizontal, and this
		 * pins CURRENT_STATE's deferral (c) for the orientation-aware path. (0.8, 0, 0.6)
		 * is |X|-dominant, so it is a HORIZONTAL contact and orientation decides it —
		 * Corner for the perpendicular pair, Head for the parallel one. (0.6, 0, 0.8) is
		 * |Z|-dominant and is a Bed for both, orientation never consulted.
		 */
		{ TEXT("perpendicular pair, tilted (0.8,0,0.6) -> horizontal, so orientation decides"),
			Stretcher, QuoinReturnAcrossX, FVector(0.8, 0.0, 0.6), EMasonryContact::Corner },
		{ TEXT("parallel pair, tilted (0.8,0,0.6) -> horizontal, so orientation decides"),
			Stretcher, SameCourseNeighbour, FVector(0.8, 0.0, 0.6), EMasonryContact::Head },
		{ TEXT("perpendicular pair, tilted (0.6,0,0.8) -> Z dominates, a bed"),
			Stretcher, QuoinReturnAcrossX, FVector(0.6, 0.0, 0.8), EMasonryContact::Bed },
		{ TEXT("parallel pair, tilted (0.6,0,0.8) -> Z dominates, a bed"),
			Stretcher, SameCourseNeighbour, FVector(0.6, 0.0, 0.8), EMasonryContact::Bed },

		/*
		 * DEGENERATE INPUT FAILS CLOSED, to Head — the weakest masonry answer. This is a
		 * DELIBERATE DIVERGENCE from the 3-arg JointForContact, whose `AbsZ >= |X| && >=
		 * |Y|` reads a ZERO normal as a bed and hands it the strongest bond (the fail-OPEN
		 * recorded as CURRENT_STATE build-mode item (b)); the boxed path must not inherit
		 * that. A non-finite normal likewise: with the house `!(x > y)` guard shape every
		 * comparison against a NaN is false, so the classification lands on Head rather
		 * than on whichever branch the comparison order happened to favour.
		 */
		{ TEXT("zero normal -> the weakest masonry answer"),
			Stretcher, QuoinReturnAcrossX, FVector::ZeroVector, EMasonryContact::Head },
		{ TEXT("NaN X in the normal -> the weakest masonry answer"),
			Stretcher, QuoinReturnAcrossX, FVector(NaNValue, 0.0, 0.0), EMasonryContact::Head },
		{ TEXT("all-NaN normal -> the weakest masonry answer"),
			Stretcher, QuoinReturnAcrossX,
			FVector(NaNValue, NaNValue, NaNValue), EMasonryContact::Head },
		{ TEXT("NaN Z in an otherwise horizontal normal -> the weakest masonry answer"),
			Stretcher, QuoinReturnAcrossX, FVector(1.0, 0.0, NaNValue), EMasonryContact::Head },
		{ TEXT("infinite normal -> the weakest masonry answer"),
			Stretcher, QuoinReturnAcrossX, FVector(InfValue, 0.0, 0.0), EMasonryContact::Head },

		/*
		 * A NON-FINITE BOX fails closed the same way: no readable long axis is not a
		 * corner. NaN extents make every `>` comparison false, so the `!(x > y)` guard
		 * form lands on "no long axis" -> Head without a special case.
		 */
		{ TEXT("NaN-extent box against an X-long brick, +X -> the weakest masonry answer"),
			Stretcher, NaNBox, FVector(1.0, 0.0, 0.0), EMasonryContact::Head },
	};

	for (const FClassifyCase& Case : Cases)
	{
		const EMasonryContact Got =
			ClassifyMasonryContact(Case.BoxA, Case.BoxB, Case.InterfaceNormalUnit);

		TestEqual(
			FString::Printf(
				TEXT("%s: expected %s, got %s"),
				Case.Description, ContactName(Case.Expected), ContactName(Got)),
			static_cast<int32>(Got),
			static_cast<int32>(Case.Expected));

		/*
		 * WHICH PIECE IS "A" MUST NOT FLIP THE ANSWER. The classification is a property of
		 * the contact, and the snap solver names the placed piece first while the shed
		 * sweep names the LOWER piece first — so the same physical quoin reaches this
		 * function in both orders.
		 */
		const EMasonryContact Swapped =
			ClassifyMasonryContact(Case.BoxB, Case.BoxA, Case.InterfaceNormalUnit);

		TestEqual(
			FString::Printf(
				TEXT("%s, A and B SWAPPED: expected %s, got %s"),
				Case.Description, ContactName(Case.Expected), ContactName(Swapped)),
			static_cast<int32>(Swapped),
			static_cast<int32>(Case.Expected));
	}

	return true;
}

/**
 * THE BEHAVIOUR, IN ONE SENTENCE. The orientation-aware five-argument
 * BuildMode::JointForContact returns FULL GeneralPurposeMortar for a bonded corner
 * between two masonry pieces, keeps GeneralPurposeMortarPerpend for a same-course head
 * joint, keeps GeneralPurposeMortar for a bed, and still returns DryStone whenever
 * either face is not compression-dominant (DESIGN §8, 2026-09-15).
 *
 * THE ASSERTION IS THE PROFILE'S IDENTITY across all five fields, for the reason the
 * file header gives: FConnectionStrength has no operator==, and mortar / perpend /
 * dry stone are only mutually distinguishable on the whole row. The corner row is the
 * one that matters — mortar and perpend share compression, friction and shear ceiling
 * and differ ONLY on the two bond axes (0.9 vs 0.2 cohesion, 0.7 vs 0.1 tension), so a
 * partial comparison would accept the perpend this ruling exists to reject.
 *
 * NEEDS A TICKING WORLD: NO.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCornerContactGetsFullMortarTest,
	"DestructionGame.Core.BuildMode.CornerContactGetsFullMortar",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCornerContactGetsFullMortarTest::RunTest(const FString& Parameters)
{
	using namespace JointInferenceTestSupport;
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;

	const FPieceBox Stretcher{ FVector(0.0, 0.0, 3.25), XLongBrick };
	const FPieceBox SameCourseNeighbour{ FVector(22.5, 0.0, 3.25), XLongBrick };
	const FPieceBox NextCourseAbove{ FVector(11.25, 0.0, 10.75), XLongBrick };
	const FPieceBox QuoinReturnAcrossX{ FVector(16.875, 5.625, 3.25), YLongBrick };
	const FPieceBox QuoinReturnAcrossY{ FVector(0.0, 16.875, 3.25), YLongBrick };
	const FPieceBox Square{ FVector(0.0, 0.0, 3.0), SquareBlock };
	const FPieceBox XLongBesideSquare{ FVector(16.75, 0.0, 3.0), XLongBrick };

	const TArray<FBoxedInferenceCase> Cases = {
		/*
		 * THE CORNER — the whole point of the slice. An interlocked quoin carries load
		 * like bonded masonry, so it gets the SAME row a bed joint does, not the perpend
		 * the normal alone used to earn it.
		 */
		{ TEXT("brick/brick quoin across an X face, +X"),
			&ClayBrick, &ClayBrick, Stretcher, QuoinReturnAcrossX, FVector(1.0, 0.0, 0.0),
			&GeneralPurposeMortar, TEXT("GeneralPurposeMortar") },
		{ TEXT("brick/brick quoin across an X face, -X"),
			&ClayBrick, &ClayBrick, Stretcher, QuoinReturnAcrossX, FVector(-1.0, 0.0, 0.0),
			&GeneralPurposeMortar, TEXT("GeneralPurposeMortar") },
		{ TEXT("brick/brick quoin across a Y face, +Y"),
			&ClayBrick, &ClayBrick, Stretcher, QuoinReturnAcrossY, FVector(0.0, 1.0, 0.0),
			&GeneralPurposeMortar, TEXT("GeneralPurposeMortar") },

		/*
		 * THE HEAD JOINT IS UNCHANGED — the collinear pair across the SAME normal that
		 * makes the quoin a corner. This row is what proves the upgrade is keyed on
		 * orientation and not on "horizontal normals are now mortar".
		 */
		{ TEXT("brick/brick collinear head joint, +X"),
			&ClayBrick, &ClayBrick, Stretcher, SameCourseNeighbour, FVector(1.0, 0.0, 0.0),
			&GeneralPurposeMortarPerpend, TEXT("GeneralPurposeMortarPerpend") },
		{ TEXT("brick/brick collinear head joint, -X"),
			&ClayBrick, &ClayBrick, Stretcher, SameCourseNeighbour, FVector(-1.0, 0.0, 0.0),
			&GeneralPurposeMortarPerpend, TEXT("GeneralPurposeMortarPerpend") },

		// A square footprint has no long axis, so it fails closed to the weaker joint.
		{ TEXT("square block against an X-long brick, +X"),
			&ClayBrick, &ClayBrick, Square, XLongBesideSquare, FVector(1.0, 0.0, 0.0),
			&GeneralPurposeMortarPerpend, TEXT("GeneralPurposeMortarPerpend") },

		// The bed joint is unchanged.
		{ TEXT("brick/brick bed joint, +Z"),
			&ClayBrick, &ClayBrick, Stretcher, NextCourseAbove, FVector(0.0, 0.0, 1.0),
			&GeneralPurposeMortar, TEXT("GeneralPurposeMortar") },

		/*
		 * A NON-COMPRESSION-DOMINANT FACE STILL WINS OUTRIGHT. Timber at a corner is a
		 * resting bearing, not a bonded quoin: mortar does not bond to a board, and
		 * fastening stays an explicit override. Both orderings, because "either face"
		 * must not depend on which one is named first.
		 */
		{ TEXT("brick/timber at a corner, +X"),
			&ClayBrick, &Timber, Stretcher, QuoinReturnAcrossX, FVector(1.0, 0.0, 0.0),
			&DryStone, TEXT("DryStone") },
		{ TEXT("timber/brick at a corner, +X"),
			&Timber, &ClayBrick, Stretcher, QuoinReturnAcrossX, FVector(1.0, 0.0, 0.0),
			&DryStone, TEXT("DryStone") },
	};

	for (const FBoxedInferenceCase& Case : Cases)
	{
		const FConnectionStrength Got = JointForContact(
			*Case.FaceA, *Case.FaceB, Case.InterfaceNormalUnit, Case.BoxA, Case.BoxB);

		CheckProfileIdentity(
			*this,
			FString::Printf(TEXT("%s (expected %s): "), Case.Description, Case.ExpectedName),
			Got,
			*Case.Expected);
	}

	return true;
}

/**
 * THE BEHAVIOUR, IN ONE SENTENCE. The five-argument JointForContact returns exactly
 * what the three-argument one returns for every axis-aligned normal, EXCEPT for a
 * horizontally-met pair whose long axes are perpendicular — the corner, which the
 * boxed overload upgrades from GeneralPurposeMortarPerpend to GeneralPurposeMortar.
 *
 * WHY THIS TEST EXISTS AT ALL. The ruling changes committed physics, so the size of
 * the change has to be pinned as tightly as the change itself: this asserts the
 * extension is a strict no-op on every contact the build mode forms today (bed joints,
 * head joints, timber bearings) and moves exactly one classification. The DIFFERING
 * rows assert BOTH directions — the new answer is mortar AND the old answer was the
 * perpend — so the test cannot pass by the two functions quietly agreeing again.
 *
 * SCOPE: AXIS-ALIGNED NORMALS ONLY, deliberately. On a ZERO or NON-FINITE normal the
 * two are SUPPOSED to disagree: the 3-arg fails OPEN (a zero normal satisfies its
 * `AbsZ >= |X| && AbsZ >= |Y|` bed test and earns the strongest bond — CURRENT_STATE
 * build-mode item (b)), while the boxed overload fails CLOSED to Head. That divergence
 * is pinned by ClassifyMasonryContactByOrientation, not excused here.
 *
 * NEEDS A TICKING WORLD: NO.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FBoxedOverloadAgreesWithTheUnboxedOneOffCornersTest,
	"DestructionGame.Core.BuildMode.BoxedOverloadAgreesWithTheUnboxedOneOffCorners",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FBoxedOverloadAgreesWithTheUnboxedOneOffCornersTest::RunTest(const FString& Parameters)
{
	using namespace JointInferenceTestSupport;
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;

	struct FPairing
	{
		const TCHAR* Description;
		FVector ExtentA;
		FVector ExtentB;
		bool bPerpendicular;
	};

	const TArray<FPairing> Pairings = {
		{ TEXT("X-long / X-long (collinear)"), XLongBrick, XLongBrick, false },
		{ TEXT("Y-long / Y-long (collinear)"), YLongBrick, YLongBrick, false },
		{ TEXT("square / X-long (no long axis)"), SquareBlock, XLongBrick, false },
		{ TEXT("X-long / Y-long (perpendicular)"), XLongBrick, YLongBrick, true },
		{ TEXT("Y-long / X-long (perpendicular, swapped)"), YLongBrick, XLongBrick, true },
	};

	const TArray<FVector> AxisNormals = {
		FVector(1.0, 0.0, 0.0), FVector(-1.0, 0.0, 0.0),
		FVector(0.0, 1.0, 0.0), FVector(0.0, -1.0, 0.0),
		FVector(0.0, 0.0, 1.0), FVector(0.0, 0.0, -1.0),
	};

	struct FMaterialPair
	{
		const TCHAR* Description;
		const FMaterialProfile* FaceA;
		const FMaterialProfile* FaceB;
		bool bBothMasonry;
	};

	const TArray<FMaterialPair> MaterialPairs = {
		{ TEXT("brick/brick"), &ClayBrick, &ClayBrick, true },
		{ TEXT("timber/brick"), &Timber, &ClayBrick, false },
		{ TEXT("brick/timber"), &ClayBrick, &Timber, false },
	};

	int32 DifferingRows = 0;

	for (const FMaterialPair& Materials : MaterialPairs)
	{
		for (const FPairing& Pairing : Pairings)
		{
			/*
			 * The two boxes are separated on the X axis by one 1 cm joint and overlap on
			 * the other two, so every row is a real face. The classification reads only the
			 * extents, so one pose serves all six normals.
			 */
			const FPieceBox BoxA{ FVector(0.0, 0.0, 3.25), Pairing.ExtentA };
			const FPieceBox BoxB{
				FVector(Pairing.ExtentA.X + 1.0 + Pairing.ExtentB.X, 0.0, 3.25), Pairing.ExtentB };

			for (const FVector& Normal : AxisNormals)
			{
				const FConnectionStrength Boxed = JointForContact(
					*Materials.FaceA, *Materials.FaceB, Normal, BoxA, BoxB);
				const FConnectionStrength Unboxed = JointForContact(
					*Materials.FaceA, *Materials.FaceB, Normal);

				const bool bHorizontal = FMath::Abs(Normal.Z) < 0.5;
				const bool bIsTheCorner =
					Materials.bBothMasonry && Pairing.bPerpendicular && bHorizontal;

				const FString Prefix = FString::Printf(
					TEXT("%s, %s, normal (%g,%g,%g): "),
					Materials.Description, Pairing.Description, Normal.X, Normal.Y, Normal.Z);

				if (!bIsTheCorner)
				{
					CheckProfileIdentity(
						*this, Prefix + TEXT("boxed must equal unboxed, "), Boxed, Unboxed);
					continue;
				}

				++DifferingRows;

				// The corner: the new answer is full mortar...
				CheckProfileIdentity(
					*this, Prefix + TEXT("the CORNER's boxed answer (GeneralPurposeMortar), "),
					Boxed, GeneralPurposeMortar);

				/*
				 * ...and the old answer was the perpend. Asserting this too is what makes
				 * "the extension changes exactly one case" a measurement rather than a claim:
				 * if the 3-arg ever started answering mortar here, this row would catch that
				 * the difference had evaporated instead of silently passing.
				 */
				CheckProfileIdentity(
					*this, Prefix + TEXT("the CORNER's unboxed answer (GeneralPurposeMortarPerpend), "),
					Unboxed, GeneralPurposeMortarPerpend);
			}
		}
	}

	/*
	 * TWO PERPENDICULAR PAIRINGS x FOUR HORIZONTAL NORMALS x ONE MASONRY MATERIAL PAIR
	 * = 8 differing readings, and 82 of the 90 readings unchanged. Pinned so a sweep
	 * that silently stopped exercising the corner (a pairing table edited, a normal
	 * dropped) cannot leave this test green and vacuous.
	 */
	TestEqual(TEXT("exactly the corner readings differ between the two overloads"),
		DifferingRows, 8);

	return true;
}

/**
 * THE BEHAVIOUR, IN ONE SENTENCE. Composed the way the placement path composes a joint
 * — DestructionLayout::MakeInterface fed the profile BuildMode::JointForContact infers
 * for the two boxes — a real quoin contact between an X-long and a Y-long brick yields
 * a LIVE FConnection carrying full GeneralPurposeMortar, not the perpend.
 *
 * WHY THIS SHAPE AND NOT THE SOLVER. There is no corner SNAP CANDIDATE yet; that is
 * CR-2, and it is the real driver for the solver call site. Today SolveSnapCandidates
 * offers a Y-long brick nothing at all (IsBrickSized rejects the returning orientation)
 * and PlacePiece has NO placement-mode argument, so neither the snap path nor a Free
 * placement forms a joint at a quoin — Free carries none by ruling. So this test pins
 * the COMPOSITION the placement path performs (Core/BuildMode/Placement.cpp's joint
 * loop: MakeInterface(handle, box, other, otherBox, joint, InferredProfile, out)) at
 * genuine quoin geometry, which is the part CR-2's candidate will hand straight to
 * PlacePiece.
 *
 * WHAT IT ADDS OVER THE UNIT ROWS. It proves the two halves fit: that this pose really
 * is a face MakeInterface accepts (one axis separated by exactly the joint, positive
 * overlap on the other two — an edge or corner TOUCH would be refused), that the face
 * it finds is the brick's 10.25 x 6.5 = 66.625 cm2 END face, that the normal it derives
 * is HORIZONTAL (so this is precisely the contact the old rule called a perpend), and
 * that the profile riding on the connection is the corner's mortar. A unit row on the
 * inference alone cannot see a geometry that never forms a joint.
 *
 * NEEDS A TICKING WORLD: NO. MakeInterface is arithmetic over two boxes.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FQuoinJointComposedLikeTheSolverIsFullMortarTest,
	"DestructionGame.Core.BuildMode.QuoinJointComposedLikeTheSolverIsFullMortar",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FQuoinJointComposedLikeTheSolverIsFullMortarTest::RunTest(const FString& Parameters)
{
	using namespace JointInferenceTestSupport;
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace BuildMode;

	const double JointCm = 1.0;

	/*
	 * THE QUOIN, laid as a player would: a grounded stretcher along X at the origin, and
	 * the returning brick abutting its +X END face across one joint with its outer Y face
	 * flush — centre (10.75 + 1 + 5.125, 5.625, 3.25). Both bottom faces at Z = 0, the
	 * 2026-09-15 rests-on-the-ground convention.
	 */
	const FPieceBox Stretcher{ FVector(0.0, 0.0, 3.25), XLongBrick };
	const FPieceBox Returning{ FVector(16.875, 5.625, 3.25), YLongBrick };

	const int32 StretcherHandle = 0;
	const int32 ReturnHandle = 1;

	/*
	 * The placement path names the PLACED piece first and the existing neighbour second,
	 * exactly as here, and passes the inferred profile straight into MakeInterface.
	 */
	const FConnectionStrength Inferred =
		JointForContact(ClayBrick, ClayBrick, FVector(1.0, 0.0, 0.0), Returning, Stretcher);

	FConnection Connection;
	const bool bFormed = MakeInterface(
		ReturnHandle, Returning, StretcherHandle, Stretcher, JointCm, Inferred, Connection);

	if (!TestTrue(
			TEXT("the quoin pose forms a face at all (one axis separated by the joint, "
				 "positive overlap on the other two)"),
			bFormed))
	{
		return true;
	}

	/*
	 * The contact is HORIZONTAL — this is the geometry the 2026-09-03 rule called a
	 * perpend on the strength of the normal alone, which is what makes the profile
	 * assertion below a measurement of the new ruling rather than of a bed joint.
	 */
	TestTrue(
		FString::Printf(
			TEXT("the interface normal is horizontal, got (%g,%g,%g)"),
			Connection.InterfaceNormal.X,
			Connection.InterfaceNormal.Y,
			Connection.InterfaceNormal.Z),
		FMath::Abs(Connection.InterfaceNormal.GetSafeNormal().Z) < 0.5);

	/*
	 * The face is the brick's END face: the 10.25 cm Y overlap by the 6.5 cm Z overlap of
	 * two course-0 bricks = 66.625 cm2, the same end-face area Core/Layout.h documents.
	 * Pinned so a pose that accidentally met on a different pair of axes could not pass.
	 */
	TestEqual(TEXT("interface area is the brick end face, cm2"),
		Connection.InterfaceAreaSqCm, 66.625);

	CheckProfileIdentity(
		*this,
		TEXT("the formed quoin connection (expected GeneralPurposeMortar): "),
		Connection.Strength,
		GeneralPurposeMortar);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
