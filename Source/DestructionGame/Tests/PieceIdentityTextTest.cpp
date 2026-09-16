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

/**
 * NAMED NAMESPACE, not anonymous, and named differently from every other one in this directory.
 * An anonymous namespace is private to a TRANSLATION UNIT rather than to a file, and a unity
 * build merges many files into one — at which point two file-local names that collide are a hard
 * compile error between files that never refer to each other. See CURRENT_STATE.md; the
 * `using namespace` lives inside RunTest for the same reason.
 */
namespace PieceIdentityTestSupport
{
	/** Two structures, so a ref from one is a ref naming another wall to the other. */
	constexpr int32 IdentityStructure = 6;
	constexpr int32 RunningBondStructure = 8;

	/*
	 * THE PUBLISHED DENSITIES, TRANSCRIBED RATHER THAN IMPORTED, AND CHECKED AGAINST THE LIBRARY AS
	 * A PRECONDITION.
	 *
	 * Every mass below is arithmetic on these two numbers, so reading them off
	 * DestructionProfiles would make the expected strings agree with the library whatever the
	 * library said — a test that cannot fail when the number it depends on is wrong. Written out
	 * here and held against the shipped profile, a re-anchor of either density fails the
	 * precondition loudly instead of silently re-deriving the answer.
	 *
	 * NO UNIT CONVERSION IS INVOLVED AND THAT IS WORTH SAYING. Density is g/cm3 and length is cm,
	 * both of which are Unreal's own units, so a mass in kilograms is (volume in cm3 x density in
	 * g/cm3) / 1000 and nothing else. The 1 N = 100 uu boundary that catches everything else in
	 * this project is nowhere near this line: no force is computed here at all.
	 */
	constexpr double ClayBrickDensityGPerCm3 = 1.9;
	constexpr double TimberDensityGPerCm3 = 0.42;

	/*
	 * THE TWO MASSES, WORKED THROUGH IN FULL — the design document's own example line says 2.9 kg
	 * for the brick and it is WRONG, which is exactly why these are spelled out rather than copied.
	 *
	 *   brick: 21.5 x 10.25 x 6.5 = 1432.4375 cm3, x 1.9 g/cm3 = 2721.63125 g = 2.72163125 kg
	 *   plate: 67.5 x 10.25 x 10  = 6918.75   cm3, x 0.42 g/cm3 = 2905.875   g = 2.905875   kg
	 *
	 * Printed to one decimal those are "2.7 kg" and "2.9 kg" — so the plate happens to read what
	 * the design document claimed for the brick, which is the coincidence that would have made a
	 * copied expectation look plausible on one of the two rows.
	 */
	constexpr double BrickMassKg = 2.72163125;
	constexpr double PlateMassKg = 2.905875;

	/** Millimetre-scale slack on a mass in kilograms: these are exact products, not measurements. */
	constexpr double IdentityMassToleranceKg = 1e-9;

	/**
	 * The library row with this name, or null.
	 *
	 * BY NAME RATHER THAN BY INDEX, because the row order is not a promise the library makes and
	 * an index would quietly retarget the day a fourth material is added in the middle.
	 */
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
	 * A material that is NOT a library row but is field-for-field identical to one.
	 *
	 * THE DISCRIMINATOR BETWEEN IDENTITY AND EQUALITY, AND THE ONLY ROW THAT SEPARATES THEM. The
	 * rule the design document states is that a piece's material is named by IDENTITY — it is the
	 * same rule BuildPieceMaterial keeps when it hands out a reference rather than a copy, and it
	 * is what makes a future retune of ClayBrick reach every brick in the world instead of half of
	 * them. An implementation that named a material by comparing its numbers would satisfy every
	 * other row in the table below and would confidently call this copy a clay brick — at which
	 * point "the readout names the shipped profile this piece was built from" has quietly become
	 * "the readout names a profile that currently looks similar".
	 *
	 * A FUNCTION-LOCAL STATIC, WHICH IS THE LIFETIME FStructurePiece::Material REQUIRES. The
	 * contract on that field is a non-owning pointer to a program-lifetime profile; a stack local
	 * would violate it even though nothing would notice inside one test body. Initialised on first
	 * use, so it also cannot be copied out of ClayBrick before ClayBrick's own translation unit has
	 * initialised it.
	 */
	const DestructionProfiles::FMaterialProfile& ClayBrickImpostor()
	{
		static const DestructionProfiles::FMaterialProfile Impostor = DestructionProfiles::ClayBrick;

		return Impostor;
	}

	/** A ref, spelled in one place so a table row is two numbers rather than four lines. */
	FPieceRef MakeIdentityRef(int32 StructureId, int32 PieceIndex)
	{
		FPieceRef Ref;
		Ref.StructureId = StructureId;
		Ref.PieceIndex = PieceIndex;

		return Ref;
	}
}

/**
 * WHAT THE INSPECTED BRICK IS: ITS MATERIAL, ITS SIZE AND ITS MASS, AS ONE COMPOSED LINE.
 *
 * SESSION_UI_DESIGN §c, S7. The details window already says where a brick is ("course 3 · #27"),
 * why it is standing up and what its joints carry — and never says what it IS. Two pieces of the
 * same wall can differ by a factor of four in weight and by a whole material, and today they
 * present identically; a player asking "why did the timber hold and the brick crush" has every
 * number except the ones that answer it.
 *
 * IT IS ONE STRING, COMPOSED IN THE MODEL, FOR THE REASON EVERY OTHER STRING ON FPieceMenuInspector
 * IS. Choosing a unit, a precision and a separator is logic, and the piece menu widget was landed
 * under a recorded exception to the TDD gate on the condition that it contains none. A widget
 * printing "2.72163125 kg" would be the same defect as a widget picking its own colours.
 *
 * THE MATERIAL IS NAMED BY POINTER IDENTITY, NOT BY ITS NUMBERS, and the impostor row is what says
 * so. See ClayBrickImpostor above for the argument; the short form is that "what shipped profile is
 * this piece made of" is a question about which row, and two rows with equal fields are different
 * rows the moment one of them is retuned.
 *
 * THE TRAP THIS TEST WAS WRITTEN AGAINST, NOW CLOSED: until this slice,
 * DestructionProfiles::AllMaterialProfiles() held COPIES — its rows were
 * `{ TEXT("ClayBrick"), ClayBrick }` with an FMaterialProfile BY VALUE, so &Row.Profile was NOT
 * &DestructionProfiles::ClayBrick, and a lookup written as a pointer comparison against the rows
 * answered "Unknown material" for every piece in the game. The address a placed piece stores is the
 * named extern's (BuildPieceMaterial returns a reference to it and BuildMode::PlacePiece stores
 * &Material verbatim), so the slice made FNamedMaterialProfile::Profile a REFERENCE to the extern,
 * which is what lets the presenter look the name up by identity. This test asserts only the
 * strings, and it deliberately contains a row that fails if anything ever compares by value again.
 * (FNamedConnectionProfile::Strength is now a REFERENCE too, made one by UI-6 when the details
 * window first had to ask a joint which row fastens it — the same trap, closed the same way.)
 *
 * THE SIZE IS THE BOX'S FULL DIMENSIONS, WHICH IS TWICE WHAT THE GRAPH STORES. FPieceBox::ExtentCm
 * is a HALF size, matching FBox::GetExtent, and a readout printing it raw would present a standard
 * brick as 10.75 x 5.125 x 3.25 — a plausible-looking set of numbers for a brick half the size of
 * every brick in the game, which is the class of quiet wrongness a factor of two produces. The
 * plate's 10 cm depth is on the table for the formatting half of the same claim: up to two
 * decimals, and no trailing zeros, so a whole number reads "10" rather than "10.00".
 *
 * AND THE MASS IS THE PIECE'S OWN. It is FStructure::GetPiece(i).MassKg — the number the solver
 * routes as load — rather than a second derivation from the box, which would be a readout that can
 * disagree with the physics it is describing. The fixture asserts that number against hand
 * arithmetic before anything is asserted about how it is printed, so a formatting test cannot pass
 * over a mass that is wrong.
 *
 * NEEDS A TICKING WORLD: no, and it needs no world at all. Both producers are world-free —
 * BuildMode::PlacePiece grows a plain FBrickLayout and BuildPieceMenuInspector is a pure function
 * of a binding — so the whole of what a player will read is reachable from a headless microsecond.
 * Nothing is solved and nothing is released: an identity line is a fact about a piece that is true
 * before anybody asks what is holding it up.
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

	/* --- THE LIBRARY IS WHAT THIS FILE THINKS IT IS ---------------------------------------- */

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

	/* --- THE BUILD PATH LAYS THE THREE PIECES ---------------------------------------------- */

	const FVector HalfBrickCm = BuildPieceHalfExtentCm(EBuildPieceKind::Brick);
	const FVector HalfPlateCm = BuildPieceHalfExtentCm(EBuildPieceKind::TimberPlate);

	FBrickLayout Layout;
	const BuildMode::FSnapSettings Settings;

	/*
	 * THREE PIECES, EACH WELL OUTSIDE THE OTHERS' SNAP RADIUS. The snap solver's radius is 30 cm
	 * and these are hundreds apart, so every one of them lands exactly where it was asked for and
	 * forms no joints. That is deliberate: this readout is about what a piece IS, and a snapped
	 * pose or an inferred joint would add a reason for the fixture to move that has nothing to do
	 * with the claim.
	 */
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

	/*
	 * THE MATERIALS REALLY ARE THE SHIPPED ROWS AND THE IMPOSTOR REALLY IS NOT. Asked of the graph
	 * by address, because "is this the library's profile" is the whole question the readout is
	 * about — and if the build path ever started copying materials, every expectation below would
	 * be wrong in a way that reads as a presenter bug.
	 */
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

	/* And the two masses the readout will print are the hand-worked ones. */
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

	/* --- AND A RUNNING-BOND WALL, WHICH NEVER SAYS WHAT ITS BRICKS ARE MADE OF -------------- */

	/*
	 * THE UNKNOWN-MATERIAL CASE COMES FROM A REAL PRODUCER RATHER THAN FROM A NULL WRITTEN HERE.
	 * DestructionLayout::RunningBond lays every wall in this game and has never set a material —
	 * it carries a bare density instead — so "a piece nobody said what it is made of" is not a
	 * hypothetical, it is most of the pieces in the project. Its density is set to the clay
	 * brick's so this wall's bricks weigh exactly what the placed one does: the row then differs
	 * from the first row in the material NAME and in nothing else, which is what makes it a test
	 * of the unknown branch rather than of three things at once.
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

	/* --- THE TABLE ------------------------------------------------------------------------- */

	struct FIdentityCase
	{
		const TCHAR* Description = nullptr;

		/** Which binding the refs below belong to. */
		const FStructureBinding* Binding = nullptr;

		FPieceRef Inspected;

		/** Empty means "the same brick, picked" — spelled per row only where it differs. */
		TArray<FPieceRef> Selected;

		/** Exactly what the line reads, or empty for the states that have nothing to say. */
		const TCHAR* Expected = nullptr;

		/** Whether a brick is singled out at all. The line's emptiness must agree with it. */
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
			/*
			 * THE PLATE CARRIES THE FORMATTING HALF: 67.5 needs one decimal, 10.25 needs two, and
			 * 10 needs none at all. A printf of "%.2f" would read "67.50 × 10.25 × 10.00", and a
			 * "%g" would be right here and wrong the first time a dimension needs three decimals.
			 */
			TEXT("a timber wall plate, whose depth is a whole number of centimetres"),
			&Built, PlateRef, { PlateRef },
			TEXT("Timber · 67.5 × 10.25 × 10 cm · 2.9 kg"), true
		},
		{
			/*
			 * IDENTITY, NOT EQUALITY. Field-for-field a clay brick, and not the clay brick — so it
			 * is an undescribed material and reads as one, with the size and mass still told
			 * truthfully. A readout that refused to say anything at all about a piece it could not
			 * name would hide the two facts it does know.
			 */
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
			/*
			 * NOTHING SINGLED OUT IS THE INSPECTOR'S EXISTING THIRD STATE, and the line joins
			 * SupportText and JointsText in it rather than inventing a fourth. A default ref is
			 * what a panel holds while the cursor is anywhere but an entry row, which is most of
			 * the time the panel is on screen.
			 */
			TEXT("bricks picked but none pointed at"),
			&Built, FPieceRef(), { BrickRef, PlateRef },
			TEXT(""), false
		},
		{
			/*
			 * AND A REF NAMING ANOTHER WALL SINGLES OUT NOTHING, which is the fail-closed route
			 * rather than a second empty state: the alternative is a line describing a brick from
			 * a structure this panel is not about.
			 */
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

		/*
		 * AND THE LINE IS PRESENT EXACTLY WHEN A BRICK IS SINGLED OUT. Two fields answering one
		 * question is how a panel ends up with a heading over a brick and a body describing the
		 * last one — the same cross-check InspectedLabel, SupportText and JointsText already carry.
		 */
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
