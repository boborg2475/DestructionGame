// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/BuildMode/Placement.h"
#include "Core/Layout.h"
#include "Core/PieceMenu.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/SessionToolbar.h"
#include "Core/StructureBinding.h"

#if WITH_DEV_AUTOMATION_TESTS

/** Uniquely named namespace: unity builds merge files, so anonymous-namespace names can collide. */
namespace PieceIdentityTestSupport
{
	/** Two structures, so a ref from one names another wall to the other. */
	constexpr int32 IdentityStructure = 6;
	constexpr int32 RunningBondStructure = 8;

	/*
	 * Published densities, transcribed rather than imported and checked against the library, so a
	 * re-anchor fails the precondition instead of silently re-deriving the answer. No unit
	 * conversion: kg = cm3 x g/cm3 / 1000.
	 */
	constexpr double ClayBrickDensityGPerCm3 = 1.9;
	constexpr double TimberDensityGPerCm3 = 0.42;

	/*
	 * The two masses, worked in full (the design document's 2.9 kg for the brick is wrong):
	 *
	 *   brick: 21.5 x 10.25 x 6.5 = 1432.4375 cm3, x 1.9 g/cm3 = 2721.63125 g = 2.72163125 kg
	 *   plate: 67.5 x 10.25 x 10  = 6918.75   cm3, x 0.42 g/cm3 = 2905.875   g = 2.905875   kg
	 */
	constexpr double BrickMassKg = 2.72163125;
	constexpr double PlateMassKg = 2.905875;

	/** Tight tolerance: these are exact products, not measurements. */
	constexpr double IdentityMassToleranceKg = 1e-9;

	/** The library row with this name, or null. By name, since row order is not guaranteed. */
	const DestructionProfiles::FNamedMaterialProfile* LibraryRowNamed(const TCHAR* Name)
	{
		for (const DestructionProfiles::FNamedMaterialProfile& Row :
			DestructionProfiles::AllMaterialProfiles())
		{
			if (Row.Name != nullptr && FString(Row.Name) == FString(Name))
			{
				return &Row;
			}
		}

		return nullptr;
	}

	/**
	 * A copy of ClayBrick that is not a library row. Materials are named by identity, not by
	 * value, and this is the only row that tells the two apart. A function-local static because
	 * FStructurePiece::Material needs a program-lifetime profile, and lazy init avoids copying
	 * ClayBrick before it is initialised.
	 */
	const DestructionProfiles::FMaterialProfile& ClayBrickImpostor()
	{
		static const DestructionProfiles::FMaterialProfile Impostor = DestructionProfiles::ClayBrick;

		return Impostor;
	}

	/** Make a piece ref. */
	FPieceRef MakeIdentityRef(int32 StructureId, int32 PieceIndex)
	{
		FPieceRef Ref;
		Ref.StructureId = StructureId;
		Ref.PieceIndex = PieceIndex;

		return Ref;
	}
}

/**
 * The inspected brick's material, size and mass as one line (SESSION_UI_DESIGN §c, S7). Composed
 * in the model, since the piece menu widget must hold no formatting logic.
 *
 * The material is named by pointer identity, not value; the impostor row fails if anything
 * compares by value again (AllMaterialProfiles once held copies, so every piece read "Unknown").
 *
 * Size is the box's full dimensions, twice the stored half extent. Up to two decimals, no trailing
 * zeros ("10", not "10.00"). Mass is the piece's own MassKg, checked against hand arithmetic first.
 *
 * No world needed; nothing is solved.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPieceIdentityTextTest,
	"DestructionGame.Presenter.PieceIdentityText",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FPieceIdentityTextTest::RunTest(const FString& Parameters)
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;
	using namespace DestructionSession;
	using namespace PieceIdentityTestSupport;

	// The library matches this file's assumptions.

	const FNamedMaterialProfile* const BrickRow = LibraryRowNamed(TEXT("ClayBrick"));
	const FNamedMaterialProfile* const TimberRow = LibraryRowNamed(TEXT("Timber"));

	if (BrickRow == nullptr || TimberRow == nullptr)
	{
		AddError(TEXT("fixture: the library must carry rows named 'ClayBrick' and 'Timber' — the "
					  "expected strings below spell those names as the library spells them"));

		return true;
	}

	TestEqual(
		FString::Printf(
			TEXT("fixture: the clay brick's published density is %g g/cm3, the library says %g"),
			ClayBrickDensityGPerCm3, ClayBrick.DensityGramsPerCubicCm),
		ClayBrick.DensityGramsPerCubicCm, ClayBrickDensityGPerCm3);

	TestEqual(
		FString::Printf(
			TEXT("fixture: timber's published density is %g g/cm3, the library says %g"),
			TimberDensityGPerCm3, Timber.DensityGramsPerCubicCm),
		Timber.DensityGramsPerCubicCm, TimberDensityGPerCm3);

	// Lay three pieces through the build path.

	const FVector HalfBrickCm = BuildPieceHalfExtentCm(EBuildPieceKind::Brick);
	const FVector HalfPlateCm = BuildPieceHalfExtentCm(EBuildPieceKind::TimberPlate);

	FBrickLayout Layout;
	const BuildMode::FSnapSettings Settings;

	// Hundreds of cm apart (snap radius is 30), so no snapping and no joints.
	const BuildMode::FPlacementResult BrickPlaced = BuildMode::PlacePiece(
		Layout,
		FVector(0.0, 0.0, CoursePlaneZCm(0, HalfBrickCm.Z)),
		HalfBrickCm,
		BuildPieceMaterial(EBuildPieceKind::Brick),
		/*bGrounded*/ true,
		Settings);

	const BuildMode::FPlacementResult PlatePlaced = BuildMode::PlacePiece(
		Layout,
		FVector(500.0, 0.0, CoursePlaneZCm(0, HalfPlateCm.Z)),
		HalfPlateCm,
		BuildPieceMaterial(EBuildPieceKind::TimberPlate),
		/*bGrounded*/ true,
		Settings);

	const BuildMode::FPlacementResult ImpostorPlaced = BuildMode::PlacePiece(
		Layout,
		FVector(1000.0, 0.0, CoursePlaneZCm(0, HalfBrickCm.Z)),
		HalfBrickCm,
		ClayBrickImpostor(),
		/*bGrounded*/ true,
		Settings);

	if (BrickPlaced.PieceHandle != 0 || PlatePlaced.PieceHandle != 1 || ImpostorPlaced.PieceHandle != 2)
	{
		AddError(FString::Printf(
			TEXT("fixture: the three placements should be handles 0, 1 and 2, they are %d, %d and %d"),
			BrickPlaced.PieceHandle, PlatePlaced.PieceHandle, ImpostorPlaced.PieceHandle));

		return true;
	}

	// By address: the shipped rows are used, and the impostor is a different object.
	TestTrue(
		TEXT("fixture: the placed brick must carry the SHIPPED ClayBrick profile, by address"),
		Layout.Structure.GetPiece(0).Material == &ClayBrick);

	TestTrue(
		TEXT("fixture: the placed plate must carry the SHIPPED Timber profile, by address"),
		Layout.Structure.GetPiece(1).Material == &Timber);

	TestTrue(
		TEXT("fixture: the impostor must be a DIFFERENT object from ClayBrick, or it proves nothing"),
		Layout.Structure.GetPiece(2).Material != &ClayBrick
			&& Layout.Structure.GetPiece(2).Material == &ClayBrickImpostor());

	TestEqual(
		FString::Printf(
			TEXT("fixture: the impostor must be field-for-field ClayBrick's density, it is %g"),
			ClayBrickImpostor().DensityGramsPerCubicCm),
		ClayBrickImpostor().DensityGramsPerCubicCm, ClayBrick.DensityGramsPerCubicCm);

	// The masses match the hand-worked ones.
	TestTrue(
		*FString::Printf(
			TEXT("fixture: a 21.5 x 10.25 x 6.5 clay brick weighs %.8f kg, the graph says %.8f"),
			BrickMassKg, Layout.Structure.GetPiece(0).MassKg),
		FMath::IsNearlyEqual(Layout.Structure.GetPiece(0).MassKg, BrickMassKg, IdentityMassToleranceKg));

	TestTrue(
		*FString::Printf(
			TEXT("fixture: a 67.5 x 10.25 x 10 timber plate weighs %.8f kg, the graph says %.8f"),
			PlateMassKg, Layout.Structure.GetPiece(1).MassKg),
		FMath::IsNearlyEqual(Layout.Structure.GetPiece(1).MassKg, PlateMassKg, IdentityMassToleranceKg));

	TArray<UObject*> NoActors;
	NoActors.SetNumZeroed(Layout.Structure.NumPieces());

	FStructureBinding Built;
	Built.StructureId = IdentityStructure;

	TestTrue(
		TEXT("fixture: the laid layout should be adopted into a binding the presenter can read"),
		AdoptLayout(Layout, NoActors, Built));

	/*
	 * A running-bond wall: a real producer that sets no material, only a density. Density matches
	 * the clay brick so the row differs from the first only in material name.
	 */
	FRunningBondSpec Spec;
	Spec.DensityGramsPerCubicCm = ClayBrickDensityGPerCm3;
	Spec.CoursesHigh = 1;
	Spec.BricksPerCourse = 2;
	Spec.Strength = DestructionProfiles::GeneralPurposeMortar;

	FBrickLayout Wall;

	if (!RunningBond(Spec, Wall) || Wall.Structure.NumPieces() < 1)
	{
		AddError(TEXT("fixture: RunningBond should lay a one-course wall for the unknown-material row"));

		return true;
	}

	TestNull(
		TEXT("fixture: a RunningBond brick must carry NO material — that is the state this row is about"),
		Wall.Structure.GetPiece(0).Material);

	TestTrue(
		*FString::Printf(
			TEXT("fixture: the wall's bricks must weigh what the placed one does, %.8f vs %.8f kg"),
			Wall.Structure.GetPiece(0).MassKg, BrickMassKg),
		FMath::IsNearlyEqual(Wall.Structure.GetPiece(0).MassKg, BrickMassKg, IdentityMassToleranceKg));

	TArray<UObject*> NoWallActors;
	NoWallActors.SetNumZeroed(Wall.Structure.NumPieces());

	FStructureBinding Unnamed;
	Unnamed.StructureId = RunningBondStructure;

	TestTrue(
		TEXT("fixture: the running-bond wall should be adopted too"),
		AdoptLayout(Wall, NoWallActors, Unnamed));

	struct FIdentityCase
	{
		const TCHAR* Description = nullptr;

		/** The binding the refs belong to. */
		const FStructureBinding* Binding = nullptr;

		FPieceRef Inspected;

		TArray<FPieceRef> Selected;

		/** Exact expected line, or empty when nothing is inspected. */
		const TCHAR* Expected = nullptr;

		/** Whether a brick is inspected; the line's emptiness must agree. */
		bool bExpectInspected = false;
	};

	const FPieceRef BrickRef = MakeIdentityRef(IdentityStructure, 0);
	const FPieceRef PlateRef = MakeIdentityRef(IdentityStructure, 1);
	const FPieceRef ImpostorRef = MakeIdentityRef(IdentityStructure, 2);
	const FPieceRef UnnamedRef = MakeIdentityRef(RunningBondStructure, 0);

	const TArray<FIdentityCase> Cases = {
		{
			TEXT("a clay brick laid through the build path"),
			&Built, BrickRef, { BrickRef },
			TEXT("ClayBrick · 21.5 × 10.25 × 6.5 cm · 2.7 kg"), true
		},
		{
			// Formatting: 67.5 needs one decimal, 10.25 two, 10 none.
			TEXT("a timber wall plate, whose depth is a whole number of centimetres"),
			&Built, PlateRef, { PlateRef },
			TEXT("Timber · 67.5 × 10.25 × 10 cm · 2.9 kg"), true
		},
		{
			// Identity, not equality: unknown material, but size and mass still shown.
			TEXT("a brick made of a profile that is not a library row, but looks exactly like one"),
			&Built, ImpostorRef, { ImpostorRef },
			TEXT("Unknown material · 21.5 × 10.25 × 6.5 cm · 2.7 kg"), true
		},
		{
			TEXT("a running-bond brick, which nobody ever said what was made of"),
			&Unnamed, UnnamedRef, { UnnamedRef },
			TEXT("Unknown material · 21.5 × 10.25 × 6.5 cm · 2.7 kg"), true
		},
		{
			// Nothing inspected: the line is empty, like SupportText and JointsText.
			TEXT("bricks picked but none pointed at"),
			&Built, FPieceRef(), { BrickRef, PlateRef },
			TEXT(""), false
		},
		{
			// A ref to another structure fails closed to nothing inspected.
			TEXT("a ref naming a brick of another structure"),
			&Built, UnnamedRef, { UnnamedRef },
			TEXT(""), false
		},
	};

	for (const FIdentityCase& Case : Cases)
	{
		const FPieceMenuInspector Inspector =
			BuildPieceMenuInspector(*Case.Binding, Case.Selected, Case.Inspected);

		TestEqual(
			FString::Printf(
				TEXT("%s: must read '%s', it reads '%s'"),
				Case.Description, Case.Expected, *Inspector.IdentityText),
			Inspector.IdentityText, FString(Case.Expected));

		// The line is present exactly when a brick is inspected.
		TestEqual(
			FString::Printf(
				TEXT("%s: a brick should%s be singled out"),
				Case.Description, Case.bExpectInspected ? TEXT("") : TEXT(" NOT")),
			Inspector.bHasInspectedPiece, Case.bExpectInspected);

		TestEqual(
			FString::Printf(
				TEXT("%s: the identity line must be present exactly when a brick is singled out; "
					 "singled out = %s, line = '%s'"),
				Case.Description,
				Inspector.bHasInspectedPiece ? TEXT("yes") : TEXT("no"),
				*Inspector.IdentityText),
			!Inspector.IdentityText.IsEmpty(), Inspector.bHasInspectedPiece);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
