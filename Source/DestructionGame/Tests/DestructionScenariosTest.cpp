// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/Structure.h"
#include "Tests/ArchingWallTestSupport.h"
#include "World/DestructionScenarios.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Scenario catalogue: the headless fixtures, named so they can be joined as levels. World-free
 * (specs, brick centres, camera positions), so it runs in the fast suite.
 *
 * `sandbox` must equal the game mode's default wall and `free-end-40` must equal
 * ArchWallSpecOfHeight(40), so each spec is compared field by field against an independently
 * written expectation. The sandbox one is transcribed, not imported, so a wrong production
 * constant is caught. Only the test includes ArchingWallTestSupport.h; production must not,
 * or the drift check disappears.
 */
namespace ScenariosTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/** Print a double at full precision so a last-bit mismatch is visible. */
	inline FString ScenariosTestBits(double Value)
	{
		return FString::Printf(TEXT("%.17g"), Value);
	}

	inline FString ScenariosTestVectorBits(const FVector& Value)
	{
		return FString::Printf(
			TEXT("(%.17g, %.17g, %.17g)"), Value.X, Value.Y, Value.Z);
	}

	/*
	 * DESIGN.md's standard brick and 1 cm joint (grid 22.5 x 11.25 x 7.5). Restated rather than
	 * imported so a row that changed brick format fails.
	 */
	constexpr double ScenariosTestBrickLengthCm = 21.5;
	constexpr double ScenariosTestBrickWidthCm = 10.25;
	constexpr double ScenariosTestBrickHeightCm = 6.5;
	constexpr double ScenariosTestMortarCm = 1.0;

	/**
	 * The brick `free-end-40` cuts: outermost full brick of course 0, at x = 0, z = 3.25. Derived
	 * from the brick size; the fixture's own helpers are cross-checked against it.
	 */
	constexpr double ScenariosTestFreeEndCutXCm = 0.0;
	constexpr double ScenariosTestFreeEndCutZCm = ScenariosTestBrickHeightCm / 2.0;

	/** The game mode's default wall, transcribed from GameModeScenarioWallSpec(): 30 x 40, flush, general-purpose mortar. */
	inline FRunningBondSpec ScenariosTestSandboxWallSpec()
	{
		FRunningBondSpec Spec;

		Spec.BrickSizeCm = FVector(
			ScenariosTestBrickLengthCm, ScenariosTestBrickWidthCm, ScenariosTestBrickHeightCm);

		Spec.JointThicknessCm = ScenariosTestMortarCm;
		Spec.DensityGramsPerCubicCm = ClayBrick.DensityGramsPerCubicCm;
		Spec.CoursesHigh = 40;
		Spec.BricksPerCourse = 30;
		Spec.End = EWallEnd::Flush;
		Spec.Strength = GeneralPurposeMortar;

		return Spec;
	}

	/**
	 * Every field of a wall spec, compared exactly. FRunningBondSpec has no operator==; listing
	 * fields here makes a newly added field a visible omission.
	 */
	inline void ScenariosTestCheckSpecsEqual(
		FAutomationTestBase& Test,
		const TCHAR* Label,
		const FRunningBondSpec& Actual,
		const FRunningBondSpec& Expected)
	{
		const auto CheckDouble =
			[&Test, Label](const TCHAR* Field, double Got, double Want)
		{
			Test.TestTrue(
				*FString::Printf(
					TEXT("%s: %s must be %s, it is %s"),
					Label, Field, *ScenariosTestBits(Want), *ScenariosTestBits(Got)),
				Got == Want);
		};

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: BrickSizeCm must be %s, it is %s"),
				Label, *ScenariosTestVectorBits(Expected.BrickSizeCm),
				*ScenariosTestVectorBits(Actual.BrickSizeCm)),
			Actual.BrickSizeCm == Expected.BrickSizeCm);

		CheckDouble(TEXT("JointThicknessCm"), Actual.JointThicknessCm, Expected.JointThicknessCm);

		CheckDouble(
			TEXT("DensityGramsPerCubicCm"),
			Actual.DensityGramsPerCubicCm,
			Expected.DensityGramsPerCubicCm);

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: CoursesHigh must be %d, it is %d"),
				Label, Expected.CoursesHigh, Actual.CoursesHigh),
			Actual.CoursesHigh == Expected.CoursesHigh);

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: BricksPerCourse must be %d, it is %d"),
				Label, Expected.BricksPerCourse, Actual.BricksPerCourse),
			Actual.BricksPerCourse == Expected.BricksPerCourse);

		Test.TestTrue(
			*FString::Printf(
				TEXT("%s: the wall end treatment must be %d (Flush is 1), it is %d"),
				Label, static_cast<int32>(Expected.End), static_cast<int32>(Actual.End)),
			Actual.End == Expected.End);

		CheckDouble(
			TEXT("Strength.CompressiveStrengthMPa"),
			Actual.Strength.CompressiveStrengthMPa,
			Expected.Strength.CompressiveStrengthMPa);

		CheckDouble(
			TEXT("Strength.ShearCohesionMPa"),
			Actual.Strength.ShearCohesionMPa,
			Expected.Strength.ShearCohesionMPa);

		CheckDouble(
			TEXT("Strength.TensileStrengthMPa"),
			Actual.Strength.TensileStrengthMPa,
			Expected.Strength.TensileStrengthMPa);

		CheckDouble(
			TEXT("Strength.FrictionCoefficient"),
			Actual.Strength.FrictionCoefficient,
			Expected.Strength.FrictionCoefficient);

		CheckDouble(
			TEXT("Strength.MaxShearStrengthMPa"),
			Actual.Strength.MaxShearStrengthMPa,
			Expected.Strength.MaxShearStrengthMPa);
	}

	/** The row with this name, or null with the reason reported. */
	inline const DestructionScenarios::FScenario* ScenariosTestRowNamed(
		FAutomationTestBase& Test, const TCHAR* Name)
	{
		const int32 Index = DestructionScenarios::IndexOfName(FName(Name));

		if (!DestructionScenarios::Catalogue().IsValidIndex(Index))
		{
			Test.AddError(FString::Printf(
				TEXT("the catalogue must carry a row named '%s'; IndexOfName returned %d against ")
				TEXT("%d row(s)"),
				Name, Index, DestructionScenarios::Catalogue().Num()));

			return nullptr;
		}

		return &DestructionScenarios::Catalogue()[Index];
	}

	/*
	 * Viewpoint constants. Default FOV is 90 degrees horizontal, so at standoff s the visible
	 * half-width is s and half-height is s * aspect (height/width). Framing needs s >= halfX and
	 * s * aspect >= halfZ; the margin keeps edges clear, the floor stops tiny structures filling
	 * the screen.
	 */
	constexpr double ScenariosTestAspectHeightOverWidth = 1080.0 / 1920.0;
	constexpr double ScenariosTestFrameMargin = 1.25;
	constexpr double ScenariosTestMinimumStandoffCm = 120.0;

	/** Yaw -90 looks along -Y with +X to the right. */
	constexpr double ScenariosTestCameraYawDegrees = -90.0;

	/** A real IEEE NaN. */
	inline double ScenariosTestMakeNaN()
	{
		volatile double Zero = 0.0;
		return Zero / Zero;
	}
}

/**
 * Properties every catalogue row must have (name, map, title, expectation, buildable wall,
 * lookups), plus the exact contents of `sandbox` and `free-end-40`. The row count is a floor so
 * adding a scenario does not mean editing this test.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDestructionScenariosCatalogueTest,
	"DestructionGame.World.Scenarios.Catalogue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FDestructionScenariosCatalogueTest::RunTest(const FString& Parameters)
{
	using namespace ScenariosTestSupport;
	using namespace DestructionScenarios;

	namespace ArchSupport = StructureArchingTestSupport;

	const TArray<FScenario>& Rows = Catalogue();

	TestTrue(
		*FString::Printf(
			TEXT("the catalogue must carry at least the two rows of slice A — the sandbox wall ")
			TEXT("and the free-end cut; it carries %d"),
			Rows.Num()),
		Rows.Num() >= 2);

	// Every row.

	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		const FScenario& Row = Rows[Index];

		const FString Label = FString::Printf(TEXT("row %d ('%s')"), Index, *Row.Name.ToString());

		TestTrue(
			*FString::Printf(TEXT("%s must be named — a nameless row cannot be selected"), *Label),
			!Row.Name.IsNone());

		TestTrue(
			*FString::Printf(TEXT("%s must name a map to be joined from"), *Label),
			Row.MapName != nullptr && FCString::Strlen(Row.MapName) > 0);

		TestTrue(
			*FString::Printf(TEXT("%s must carry a title"), *Label),
			Row.Title != nullptr && FCString::Strlen(Row.Title) > 0);

		// Without an expectation line a viewer cannot tell a correct result from a broken one.
		TestTrue(
			*FString::Printf(
				TEXT("%s must say in one line what a human should see happen"), *Label),
			Row.Expectation != nullptr && FCString::Strlen(Row.Expectation) > 0);

		/*
		 * HoldSeconds is how long the structure stays as laid before cuts apply and it settles.
		 * It applies to every row, including those with no cut, or a corbel collapses before the
		 * first frame. The floor is over a second so a human has time to look; NaN fails too.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s must HOLD its structure as laid long enough for a human to look at it ")
				TEXT("before the level runs — whether or not it cuts anything, and this row cuts ")
				TEXT("%d; its hold is %s s"),
				*Label, Row.CutCentresCm.Num(), *ScenariosTestBits(Row.HoldSeconds)),
			Row.HoldSeconds > 1.0 && FMath::IsFinite(Row.HoldSeconds));

		/*
		 * A row with a running-bond spec must have a complete one. Corbel rows use another
		 * producer and leave `Wall` empty; World.Scenarios.Build checks every row builds.
		 */
		if (Row.Wall.CoursesHigh > 0 || Row.Wall.BricksPerCourse > 0)
		{
			TestTrue(
				*FString::Printf(
					TEXT("%s carries a running-bond spec, so it must describe a wall that could be ")
					TEXT("laid: %d courses of %d at %s g/cm3"),
					*Label, Row.Wall.CoursesHigh, Row.Wall.BricksPerCourse,
					*ScenariosTestBits(Row.Wall.DensityGramsPerCubicCm)),
				Row.Wall.CoursesHigh > 0 && Row.Wall.BricksPerCourse > 0
					&& Row.Wall.DensityGramsPerCubicCm > 0.0 && Row.Wall.JointThicknessCm > 0.0
					&& Row.Wall.BrickSizeCm.GetMin() > 0.0);
		}

		TestTrue(
			*FString::Printf(TEXT("%s must be found by its own name"), *Label),
			IndexOfName(Row.Name) == Index);

		// Map lookup is case-insensitive: names from a URL or GetMapName may differ in case.
		const FString MapName(Row.MapName);

		TestTrue(
			*FString::Printf(TEXT("%s must be found by its map name '%s'"), *Label, *MapName),
			IndexOfMapName(MapName) == Index);

		TestTrue(
			*FString::Printf(
				TEXT("%s must be found by its map name in upper case ('%s')"),
				*Label, *MapName.ToUpper()),
			IndexOfMapName(MapName.ToUpper()) == Index);

		TestTrue(
			*FString::Printf(
				TEXT("%s must be found by its map name in lower case ('%s')"),
				*Label, *MapName.ToLower()),
			IndexOfMapName(MapName.ToLower()) == Index);

		for (int32 Other = 0; Other < Index; ++Other)
		{
			TestTrue(
				*FString::Printf(
					TEXT("%s must not share a name with row %d — a duplicate makes one of the ")
					TEXT("two unreachable"),
					*Label, Other),
				Rows[Other].Name != Row.Name);

			TestTrue(
				*FString::Printf(
					TEXT("%s must not share a map with row %d ('%s')"),
					*Label, Other, Rows[Other].MapName),
				!MapName.Equals(FString(Rows[Other].MapName), ESearchCase::IgnoreCase));
		}
	}

	// Lookups fail closed.

	TestTrue(
		TEXT("an unnamed lookup names no scenario"),
		IndexOfName(NAME_None) == INDEX_NONE);

	TestTrue(
		TEXT("a name no row carries names no scenario"),
		IndexOfName(FName(TEXT("no-such-scenario"))) == INDEX_NONE);

	TestTrue(
		TEXT("an empty map name names no scenario"),
		IndexOfMapName(FString()) == INDEX_NONE);

	TestTrue(
		TEXT("a map no row carries names no scenario"),
		IndexOfMapName(TEXT("Lvl_NoSuchMap")) == INDEX_NONE);

	// Sandbox: the wall Play gives you.

	if (const FScenario* const Sandbox = ScenariosTestRowNamed(*this, TEXT("sandbox")))
	{
		TestEqual(
			TEXT("the sandbox row is selected by the map the game already ships"),
			FString(Sandbox->MapName), FString(TEXT("Lvl_Sandbox")));

		ScenariosTestCheckSpecsEqual(
			*this,
			TEXT("sandbox (against GameModeScenarioWallSpec, transcribed)"),
			Sandbox->Wall,
			ScenariosTestSandboxWallSpec());

		// Also against the fixture ARCHING_DESIGN's span table was worked on; all three must agree.
		ScenariosTestCheckSpecsEqual(
			*this,
			TEXT("sandbox (against StructureArchingTestSupport::ScenarioWallSpec)"),
			Sandbox->Wall,
			ArchSupport::ScenarioWallSpec());

		TestTrue(
			*FString::Printf(
				TEXT("the sandbox wall cuts nothing — it is what Play gives you today, and ")
				TEXT("today nothing is removed from it; it names %d cut(s)"),
				Sandbox->CutCentresCm.Num()),
			Sandbox->CutCentresCm.Num() == 0);
	}

	// Free end: the wall the solver suite reads.

	if (const FScenario* const FreeEnd = ScenariosTestRowNamed(*this, TEXT("free-end-40")))
	{
		TestEqual(
			TEXT("the free-end row is selected by its own map"),
			FString(FreeEnd->MapName), FString(TEXT("Lvl_FreeEnd40")));

		ScenariosTestCheckSpecsEqual(
			*this,
			TEXT("free-end-40 (against ArchWallSpecOfHeight(40))"),
			FreeEnd->Wall,
			ArchSupport::ArchWallSpecOfHeight(40));

		TestTrue(
			*FString::Printf(
				TEXT("free-end-40 cuts exactly one brick — the user's reported case is one ")
				TEXT("deletion, and a second would be a different claim; it names %d"),
				FreeEnd->CutCentresCm.Num()),
			FreeEnd->CutCentresCm.Num() == 1);

		if (FreeEnd->CutCentresCm.Num() == 1)
		{
			const FVector Expected(
				ScenariosTestFreeEndCutXCm, 0.0, ScenariosTestFreeEndCutZCm);

			TestTrue(
				*FString::Printf(
					TEXT("free-end-40 cuts the outermost full brick of the grounded course, at ")
					TEXT("%s; it names %s"),
					*ScenariosTestVectorBits(Expected),
					*ScenariosTestVectorBits(FreeEnd->CutCentresCm[0])),
				FreeEnd->CutCentresCm[0] == Expected);
		}
	}

	// Cross-check: the fixture header agrees with the cut centre derived from the brick size.
	TestTrue(
		*FString::Printf(
			TEXT("fixture: the outermost even-course brick sits at x = %s and z = %s; the ")
			TEXT("arching fixture says %s and %s"),
			*ScenariosTestBits(ScenariosTestFreeEndCutXCm),
			*ScenariosTestBits(ScenariosTestFreeEndCutZCm),
			*ScenariosTestBits(ArchSupport::ArchWallEvenBrickXCm(0)),
			*ScenariosTestBits(ArchSupport::ArchWallCourseZCm(0))),
		ArchSupport::ArchWallEvenBrickXCm(0) == ScenariosTestFreeEndCutXCm
			&& ArchSupport::ArchWallCourseZCm(0) == ScenariosTestFreeEndCutZCm);

	return true;
}

/**
 * Build lays a row's wall through RunningBond and resolves its cuts, or refuses.
 *
 * The wall is compared bit for bit against RunningBond's own layout (boxes, masses, joints).
 * Cuts are resolved to handles, not applied: the game mode removes them later so the player
 * sees it, so the wall returned is whole. The handle index is not asserted, only the resolved
 * piece's box and grounding.
 *
 * A cut centre that names no brick must refuse the build: a dropped cut gives a level that
 * looks like a wall that correctly stood. Outputs are pre-poisoned so a refusal must clear them.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDestructionScenariosBuildTest,
	"DestructionGame.World.Scenarios.Build",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FDestructionScenariosBuildTest::RunTest(const FString& Parameters)
{
	using namespace ScenariosTestSupport;
	using namespace DestructionScenarios;
	using namespace DestructionLayout;

	// Every row builds exactly the wall RunningBond lays.

	for (const FScenario& Row : Catalogue())
	{
		const FString Label = FString::Printf(TEXT("'%s'"), *Row.Name.ToString());

		FBrickLayout Built;
		TArray<int32> Cut;

		/*
		 * A build sandbox lays nothing (the player fills it), so Build must refuse it and write
		 * nothing. That makes the game mode's separate branch for it necessary. Pre-poisoned so
		 * "wrote nothing" is checkable.
		 */
		if (Row.bBuildSandbox)
		{
			Built.Boxes.Add(FPieceBox());
			Cut.Add(4242);

			const bool bBuilt = Build(Row, Built, Cut);

			TestTrue(
				*FString::Printf(
					TEXT("%s is a BUILD SANDBOX — it lays nothing and the player builds — so Build ")
					TEXT("must REFUSE it rather than hand back an empty wall; it returned %s"),
					*Label, bBuilt ? TEXT("true") : TEXT("false")),
				!bBuilt);

			TestTrue(
				*FString::Printf(
					TEXT("%s: a refused build writes nothing — it left %d box(es), %d piece(s) and ")
					TEXT("%d cut piece(s) behind"),
					*Label, Built.Boxes.Num(), Built.Structure.NumPieces(), Cut.Num()),
				Built.Boxes.Num() == 0 && Built.Structure.NumPieces() == 0 && Cut.Num() == 0);

			continue;
		}

		if (!Build(Row, Built, Cut))
		{
			AddError(FString::Printf(
				TEXT("%s must build: a catalogue row that cannot be laid is a level that cannot ")
				TEXT("be joined"),
				*Label));

			continue;
		}

		TestTrue(
			*FString::Printf(
				TEXT("%s must lay SOMETHING — a row that builds an empty structure is a level ")
				TEXT("showing an empty world"),
				*Label),
			Built.Structure.NumPieces() > 0);

		TestTrue(
			*FString::Printf(
				TEXT("%s must hand back one box per piece: %d boxes for %d pieces"),
				*Label, Built.Boxes.Num(), Built.Structure.NumPieces()),
			Built.Boxes.Num() == Built.Structure.NumPieces());

		/*
		 * Rows with a running-bond spec must be exactly RunningBond's wall. Corbels cannot be
		 * expressed by RunningBond; World.Scenarios.CorbelRows covers them.
		 */
		if (Row.Wall.CoursesHigh > 0 && Row.Wall.BricksPerCourse > 0)
		{
			FBrickLayout Reference;

			if (!RunningBond(Row.Wall, Reference))
			{
				AddError(FString::Printf(
					TEXT("fixture: %s's own spec must lay a wall through RunningBond"), *Label));

				continue;
			}

			TestTrue(
				*FString::Printf(
					TEXT("%s must lay %d pieces and %d joints, exactly what RunningBond lays from ")
					TEXT("its spec; it laid %d and %d"),
					*Label, Reference.Structure.NumPieces(), Reference.Structure.NumConnections(),
					Built.Structure.NumPieces(), Built.Structure.NumConnections()),
				Built.Structure.NumPieces() == Reference.Structure.NumPieces()
					&& Built.Structure.NumConnections() == Reference.Structure.NumConnections());

			// Bit for bit; report only the first difference so 1,220 failures don't bury the log.
			int32 FirstDifferentPiece = INDEX_NONE;

			const int32 Common = FMath::Min(Built.Boxes.Num(), Reference.Boxes.Num());

			for (int32 Piece = 0; Piece < Common; ++Piece)
			{
				const bool bSame = Built.Boxes[Piece].CentreCm == Reference.Boxes[Piece].CentreCm
					&& Built.Boxes[Piece].ExtentCm == Reference.Boxes[Piece].ExtentCm
					&& Built.Structure.GetPiece(Piece).MassKg
						== Reference.Structure.GetPiece(Piece).MassKg
					&& Built.Structure.GetPiece(Piece).bIsGrounded
						== Reference.Structure.GetPiece(Piece).bIsGrounded;

				if (!bSame)
				{
					FirstDifferentPiece = Piece;
					break;
				}
			}

			TestTrue(
				*FString::Printf(
					TEXT("%s must lay every brick where RunningBond lays it, weighing what ")
					TEXT("RunningBond weighs it: piece %d is at %s weighing %s, RunningBond puts it ")
					TEXT("at %s weighing %s"),
					*Label, FirstDifferentPiece,
					FirstDifferentPiece == INDEX_NONE
						? TEXT("-")
						: *ScenariosTestVectorBits(Built.Boxes[FirstDifferentPiece].CentreCm),
					FirstDifferentPiece == INDEX_NONE
						? TEXT("-")
						: *ScenariosTestBits(Built.Structure.GetPiece(FirstDifferentPiece).MassKg),
					FirstDifferentPiece == INDEX_NONE
						? TEXT("-")
						: *ScenariosTestVectorBits(Reference.Boxes[FirstDifferentPiece].CentreCm),
					FirstDifferentPiece == INDEX_NONE
						? TEXT("-")
						: *ScenariosTestBits(
							Reference.Structure.GetPiece(FirstDifferentPiece).MassKg)),
				FirstDifferentPiece == INDEX_NONE);
		}

		// Cuts resolved, nothing removed.

		TestTrue(
			*FString::Printf(
				TEXT("%s must resolve one piece per cut centre: %d centre(s), %d piece(s)"),
				*Label, Row.CutCentresCm.Num(), Cut.Num()),
			Cut.Num() == Row.CutCentresCm.Num());

		TestTrue(
			*FString::Printf(
				TEXT("%s must hand back a WHOLE wall — the player watches the brick go, so ")
				TEXT("Build removes nothing: %d of %d pieces are live"),
				*Label, Built.Structure.NumLivePieces(), Built.Structure.NumPieces()),
			Built.Structure.NumLivePieces() == Built.Structure.NumPieces());

		for (int32 Entry = 0; Entry < Cut.Num(); ++Entry)
		{
			const int32 Piece = Cut[Entry];

			TestTrue(
				*FString::Printf(
					TEXT("%s: cut %d resolved to piece %d, which must be a live piece of the ")
					TEXT("wall it was resolved against"),
					*Label, Entry, Piece),
				Built.Boxes.IsValidIndex(Piece) && !Built.Structure.IsPieceRemoved(Piece));

			for (int32 Earlier = 0; Earlier < Entry; ++Earlier)
			{
				TestTrue(
					*FString::Printf(
						TEXT("%s: cut %d and cut %d both resolved to piece %d — two centres ")
						TEXT("cannot name one brick"),
						*Label, Earlier, Entry, Piece),
					Cut[Earlier] != Piece);
			}

			if (Built.Boxes.IsValidIndex(Piece))
			{
				TestTrue(
					*FString::Printf(
						TEXT("%s: cut %d asked for the brick at %s and got the one at %s"),
						*Label, Entry, *ScenariosTestVectorBits(Row.CutCentresCm[Entry]),
						*ScenariosTestVectorBits(Built.Boxes[Piece].CentreCm)),
					Built.Boxes[Piece].CentreCm == Row.CutCentresCm[Entry]);
			}
		}
	}

	// The free-end cut is the grounded end brick.

	if (const FScenario* const FreeEnd = ScenariosTestRowNamed(*this, TEXT("free-end-40")))
	{
		FBrickLayout Built;
		TArray<int32> Cut;

		if (Build(*FreeEnd, Built, Cut) && Cut.Num() == 1
			&& Built.Boxes.IsValidIndex(Cut[0]))
		{
			const FPieceBox& Box = Built.Boxes[Cut[0]];

			const FVector ExpectedCentre(
				ScenariosTestFreeEndCutXCm, 0.0, ScenariosTestFreeEndCutZCm);

			const FVector ExpectedExtent(
				ScenariosTestBrickLengthCm / 2.0,
				ScenariosTestBrickWidthCm / 2.0,
				ScenariosTestBrickHeightCm / 2.0);

			TestTrue(
				*FString::Printf(
					TEXT("free-end-40's cut must land on the brick at %s; it landed on %s"),
					*ScenariosTestVectorBits(ExpectedCentre),
					*ScenariosTestVectorBits(Box.CentreCm)),
				Box.CentreCm == ExpectedCentre);

			// A full brick, not a half bat (odd courses of a flush wall start with one).
			TestTrue(
				*FString::Printf(
					TEXT("free-end-40 cuts a FULL brick, half-extent %s; it cut one of %s"),
					*ScenariosTestVectorBits(ExpectedExtent),
					*ScenariosTestVectorBits(Box.ExtentCm)),
				Box.ExtentCm == ExpectedExtent);

			TestTrue(
				TEXT("free-end-40 cuts a brick off the GROUNDED course — the wall loses a seat ")
				TEXT("on the earth, which is what the reported case was about"),
				Built.Structure.GetPiece(Cut[0]).bIsGrounded);
		}
	}

	// A cut that names no brick refuses and writes nothing.

	if (const FScenario* const FreeEnd = ScenariosTestRowNamed(*this, TEXT("free-end-40")))
	{
		const double NotANumber = ScenariosTestMakeNaN();

		struct FScenariosTestMissRow
		{
			const TCHAR* Why;
			TArray<FVector> CutCentresCm;
		};

		// Plausible misses: the head-joint gap and bed-joint plane are the realistic typos.
		const FScenariosTestMissRow MissRows[] =
		{
			{ TEXT("nowhere near the wall"),
				{ FVector(10000.0, 0.0, 3.25) } },

			{ TEXT("the head-joint gap between two bricks of the grounded course"),
				{ FVector(11.25, 0.0, ScenariosTestFreeEndCutZCm) } },

			{ TEXT("the bed-joint plane between courses 0 and 1"),
				{ FVector(ScenariosTestFreeEndCutXCm, 0.0, 7.0) } },

			{ TEXT("a metre out of the plane of the wall"),
				{ FVector(ScenariosTestFreeEndCutXCm, 100.0, ScenariosTestFreeEndCutZCm) } },

			{ TEXT("not a number at all"),
				{ FVector(NotANumber, NotANumber, NotANumber) } },

			{ TEXT("one real brick and one that is not there"),
				{ FVector(ScenariosTestFreeEndCutXCm, 0.0, ScenariosTestFreeEndCutZCm),
					FVector(11.25, 0.0, ScenariosTestFreeEndCutZCm) } },
		};

		for (const FScenariosTestMissRow& Miss : MissRows)
		{
			FScenario Bad = *FreeEnd;
			Bad.CutCentresCm = Miss.CutCentresCm;

			// Pre-poisoned so a refusal must clear stale outputs.
			FBrickLayout Built;
			Built.Boxes.Add(FPieceBox());

			TArray<int32> Cut;
			Cut.Add(4242);

			const bool bBuilt = Build(Bad, Built, Cut);

			TestTrue(
				*FString::Printf(
					TEXT("a cut centre that names no brick must REFUSE the build (%s) — a ")
					TEXT("dropped cut is a level that looks intact and never does anything"),
					Miss.Why),
				!bBuilt);

			TestTrue(
				*FString::Printf(
					TEXT("a refused build (%s) writes nothing: it left %d box(es) and %d cut ")
					TEXT("piece(s) behind"),
					Miss.Why, Built.Boxes.Num(), Cut.Num()),
				Built.Boxes.Num() == 0 && Cut.Num() == 0
					&& Built.Structure.NumPieces() == 0);
		}
	}

	return true;
}

/**
 * Where the camera stands so the whole structure is in view:
 *
 *     standoff = max(120, 1.25 * max(halfX, halfZ / aspect))
 *     location = (centre.X, centre.Y + standoff, centre.Z)
 *     rotation = (0, -90, 0)
 *
 * Three worked rows exercise each branch. Yaw is also checked as axes: yaw +90 faces the wall
 * too but mirrors +X against the design elevations (CORBEL_CASES.html). A sweep then checks
 * framing and placement as separate properties over a grid of shapes.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDestructionScenariosViewpointTest,
	"DestructionGame.World.Scenarios.Viewpoint",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FDestructionScenariosViewpointTest::RunTest(const FString& Parameters)
{
	using namespace ScenariosTestSupport;
	using namespace DestructionScenarios;

	const double Aspect = ScenariosTestAspectHeightOverWidth;

	TestTrue(
		*FString::Printf(
			TEXT("fixture: a 1920x1080 viewport is 0.5625 high per unit wide, it is %s"),
			*ScenariosTestBits(Aspect)),
		Aspect == 0.5625);

	// The three worked rows.

	struct FScenariosTestViewRow
	{
		const TCHAR* Why;
		FVector CentreCm;
		FVector HalfSizeCm;
		double ExpectedStandoffCm;
	};

	const FScenariosTestViewRow ViewRows[] =
	{
		// Width governs: 150 / 0.5625 = 266.7 < 350, so 1.25 * 350 = 437.5.
		{ TEXT("a wide wall, where half-width governs"),
			FVector(350.0, 0.0, 150.0), FVector(350.0, 5.125, 150.0), 437.5 },

		/*
		 * Height over aspect governs: 200 / 0.5625 = 355.6 > 10, times 1.25. Catches an
		 * implementation that forgets to divide by the aspect.
		 */
		{ TEXT("a tall thin wall, where half-height over aspect governs"),
			FVector(0.0, 0.0, 200.0), FVector(10.0, 5.125, 200.0), 444.44444444444446 },

		// Floor governs: 1.25 * (10 / 0.5625) = 22.2 < 120.
		{ TEXT("a tiny structure, where the 120 cm floor governs"),
			FVector(0.0, 0.0, 10.0), FVector(10.0, 5.125, 10.0), 120.0 },
	};

	for (const FScenariosTestViewRow& Row : ViewRows)
	{
		const FBox Bounds = FBox::BuildAABB(Row.CentreCm, Row.HalfSizeCm);

		const FViewpoint Viewpoint = ViewpointFor(Bounds, Aspect);

		const FVector Expected(
			Row.CentreCm.X, Row.CentreCm.Y + Row.ExpectedStandoffCm, Row.CentreCm.Z);

		TestTrue(
			*FString::Printf(
				TEXT("%s: the viewpoint must be %s (a standoff of %s cm); it is %s"),
				Row.Why, *ScenariosTestVectorBits(Expected),
				*ScenariosTestBits(Row.ExpectedStandoffCm),
				*ScenariosTestVectorBits(Viewpoint.LocationCm)),
			Viewpoint.LocationCm.X == Expected.X && Viewpoint.LocationCm.Y == Expected.Y
				&& Viewpoint.LocationCm.Z == Expected.Z);

		TestTrue(
			*FString::Printf(
				TEXT("%s: the view is level and faces -Y, rotation (0, %s, 0); it is (%s, %s, %s)"),
				Row.Why, *ScenariosTestBits(ScenariosTestCameraYawDegrees),
				*ScenariosTestBits(Viewpoint.Rotation.Pitch),
				*ScenariosTestBits(Viewpoint.Rotation.Yaw),
				*ScenariosTestBits(Viewpoint.Rotation.Roll)),
			Viewpoint.Rotation.Pitch == 0.0 && Viewpoint.Rotation.Roll == 0.0
				&& Viewpoint.Rotation.Yaw == ScenariosTestCameraYawDegrees);

		// Yaw as axes: look along -Y, +X to the right, so levels are not mirrored.
		const FRotationMatrix Frame(Viewpoint.Rotation);

		const FVector Forward = Frame.GetScaledAxis(EAxis::X);
		const FVector Right = Frame.GetScaledAxis(EAxis::Y);

		TestTrue(
			*FString::Printf(
				TEXT("%s: the camera looks along -Y — its forward is %s"),
				Row.Why, *ScenariosTestVectorBits(Forward)),
			Forward.Y < -0.999);

		TestTrue(
			*FString::Printf(
				TEXT("%s: +X is to the RIGHT, so the level reads the same way round as the ")
				TEXT("design elevations — its right vector is %s"),
				Row.Why, *ScenariosTestVectorBits(Right)),
			Right.X > 0.999);
	}

	// Property sweep: all of it inside the frustum with the margin.

	const double SweptHalfWidthsCm[] =
		{ 0.1, 1.0, 5.125, 10.0, 50.0, 137.5, 350.0, 1000.0, 5000.0 };

	const double SweptHalfHeightsCm[] =
		{ 0.1, 1.0, 3.25, 10.0, 60.0, 150.0, 200.0, 300.0, 1500.0 };

	const FVector SweptCentresCm[] =
	{
		FVector(0.0, 0.0, 0.0),
		FVector(350.0, 0.0, 150.0),
		FVector(-1200.0, -1500.0, 800.0),
	};

	int32 SweptCases = 0;
	int32 FramingFailures = 0;
	int32 PlacementFailures = 0;

	/*
	 * Framing (inside the frustum with margin) depends only on standoff and extents; placement
	 * (camera at centre + standoff) owns the rounding of that sum. So the standoff is read from
	 * the same box centred at the origin, where LocationCm.Y is exactly the standoff.
	 *
	 * Recovering it as LocationCm.Y - CentreCm.Y loses bits: at centre.Y = -1500 it comes back
	 * three ulps low, enough to push a 60 cm half-height outside a 75 cm half-frustum, and the
	 * error grows with distance from the origin. Placement is still exact equality, and it also
	 * pins the standoff as a function of shape alone.
	 */
	for (const FVector& CentreCm : SweptCentresCm)
	{
		for (const double HalfWidthCm : SweptHalfWidthsCm)
		{
			for (const double HalfHeightCm : SweptHalfHeightsCm)
			{
				++SweptCases;

				const FVector HalfSizeCm(HalfWidthCm, 5.125, HalfHeightCm);

				const FViewpoint AtOrigin =
					ViewpointFor(FBox::BuildAABB(FVector::ZeroVector, HalfSizeCm), Aspect);

				const double StandoffCm = AtOrigin.LocationCm.Y;

				const FViewpoint Viewpoint =
					ViewpointFor(FBox::BuildAABB(CentreCm, HalfSizeCm), Aspect);

				const FVector ExpectedLocationCm(
					CentreCm.X, CentreCm.Y + StandoffCm, CentreCm.Z);

				const bool bPlaced = Viewpoint.LocationCm.X == ExpectedLocationCm.X
					&& Viewpoint.LocationCm.Y == ExpectedLocationCm.Y
					&& Viewpoint.LocationCm.Z == ExpectedLocationCm.Z
					&& FMath::IsFinite(StandoffCm)
					&& StandoffCm >= ScenariosTestMinimumStandoffCm
					&& Viewpoint.Rotation.Yaw == ScenariosTestCameraYawDegrees
					&& Viewpoint.Rotation.Pitch == 0.0 && Viewpoint.Rotation.Roll == 0.0;

				// Strictly inside, then the margin, written as inequalities so a retune survives.
				const bool bFramed = HalfWidthCm < StandoffCm
					&& HalfHeightCm < StandoffCm * Aspect
					&& StandoffCm >= ScenariosTestFrameMargin * HalfWidthCm
					&& StandoffCm * Aspect >= ScenariosTestFrameMargin * HalfHeightCm;

				if (!bPlaced && PlacementFailures == 0)
				{
					AddError(FString::Printf(
						TEXT("a structure %s cm half-size centred at %s must be viewed from ")
						TEXT("straight in front of its centre at the %s cm standoff its shape ")
						TEXT("earns (no closer than %s cm), which puts the camera at %s: the ")
						TEXT("viewpoint is %s facing (%s, %s, %s)"),
						*ScenariosTestVectorBits(HalfSizeCm),
						*ScenariosTestVectorBits(CentreCm),
						*ScenariosTestBits(StandoffCm),
						*ScenariosTestBits(ScenariosTestMinimumStandoffCm),
						*ScenariosTestVectorBits(ExpectedLocationCm),
						*ScenariosTestVectorBits(Viewpoint.LocationCm),
						*ScenariosTestBits(Viewpoint.Rotation.Pitch),
						*ScenariosTestBits(Viewpoint.Rotation.Yaw),
						*ScenariosTestBits(Viewpoint.Rotation.Roll)));
				}

				if (!bFramed && FramingFailures == 0)
				{
					AddError(FString::Printf(
						TEXT("a structure %s cm half-size (swept at centre %s, though framing ")
						TEXT("does not depend on it) is not wholly in frame from a standoff of ")
						TEXT("%s cm: the frustum there is %s cm half-wide and %s cm half-high, ")
						TEXT("against the %s and %s the %s margin asks for"),
						*ScenariosTestVectorBits(HalfSizeCm),
						*ScenariosTestVectorBits(CentreCm),
						*ScenariosTestBits(StandoffCm),
						*ScenariosTestBits(StandoffCm),
						*ScenariosTestBits(StandoffCm * Aspect),
						*ScenariosTestBits(ScenariosTestFrameMargin * HalfWidthCm),
						*ScenariosTestBits(ScenariosTestFrameMargin * HalfHeightCm),
						*ScenariosTestBits(ScenariosTestFrameMargin)));
				}

				PlacementFailures += bPlaced ? 0 : 1;
				FramingFailures += bFramed ? 0 : 1;
			}
		}
	}

	AddInfo(FString::Printf(
		TEXT("swept %d bounding boxes: %d placed wrongly, %d not wholly in frame"),
		SweptCases, PlacementFailures, FramingFailures));

	TestTrue(
		*FString::Printf(
			TEXT("every one of the %d swept structures must be wholly inside the frustum with ")
			TEXT("the framing margin to spare; %d were not"),
			SweptCases, FramingFailures),
		FramingFailures == 0);

	TestTrue(
		*FString::Printf(
			TEXT("every one of the %d swept structures must be viewed from in front of its ")
			TEXT("centre; %d were not"),
			SweptCases, PlacementFailures),
		PlacementFailures == 0);

	// Degenerate bounds fail closed to the floor.

	const FViewpoint OfNothing = ViewpointFor(FBox(ForceInit), Aspect);

	TestTrue(
		*FString::Printf(
			TEXT("an empty bounding box has nothing to frame, so the standoff falls back to ")
			TEXT("the %s cm floor and stays finite; the viewpoint is %s"),
			*ScenariosTestBits(ScenariosTestMinimumStandoffCm),
			*ScenariosTestVectorBits(OfNothing.LocationCm)),
		FMath::IsFinite(OfNothing.LocationCm.X) && FMath::IsFinite(OfNothing.LocationCm.Y)
			&& FMath::IsFinite(OfNothing.LocationCm.Z)
			&& OfNothing.LocationCm.Y >= ScenariosTestMinimumStandoffCm);

	// An inverted box has negative extents; unguarded, the camera would stand behind the structure.
	const FViewpoint OfInverted = ViewpointFor(
		FBox(FVector(10.0, 10.0, 10.0), FVector(-10.0, -10.0, -10.0)), Aspect);

	TestTrue(
		*FString::Printf(
			TEXT("an inverted bounding box has negative extents, and the standoff must still ")
			TEXT("land on the %s cm floor rather than behind the structure; the viewpoint is %s"),
			*ScenariosTestBits(ScenariosTestMinimumStandoffCm),
			*ScenariosTestVectorBits(OfInverted.LocationCm)),
		FMath::IsFinite(OfInverted.LocationCm.Y)
			&& OfInverted.LocationCm.Y >= ScenariosTestMinimumStandoffCm);

	return true;
}

/**
 * Optional three-quarter framing, so a 3D structure (shed3d) is not shot as a flat front wall
 * with its depth foreshortened away.
 *
 * Three claims: (1) regression pin, an unset row gets the bit-identical head-on view through
 * both the defaulted call and an explicit HeadOn; (2) the shed3d row opts into ThreeQuarter;
 * (3) the angled camera is orbited off X, elevated, pitched down, off the -90 yaw, aimed at the
 * centre, and far enough to frame the whole 3D box.
 *
 * Framing is asserted against the bounding sphere (radius = 3D half-diagonal), since at an angle
 * depth rotates into both screen axes. The vertical inequality is the binding one and is what a
 * standoff sized from halfZ alone fails for a box with depth.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDestructionScenariosViewpointFramingTest,
	"DestructionGame.World.Scenarios.ViewpointFraming",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FDestructionScenariosViewpointFramingTest::RunTest(const FString& Parameters)
{
	using namespace ScenariosTestSupport;
	using namespace DestructionScenarios;

	const double Aspect = ScenariosTestAspectHeightOverWidth;

	/*
	 * 1. Regression pin: the wide wall from the head-on test, standoff 1.25 * 350 = 437.5,
	 * camera at (350, 437.5, 150), level, facing -Y. Must match to the bit.
	 */
	const FVector HeadOnCentreCm(350.0, 0.0, 150.0);
	const FVector HeadOnHalfSizeCm(350.0, 5.125, 150.0);
	const double HeadOnStandoffCm = 437.5;

	const FBox HeadOnBounds = FBox::BuildAABB(HeadOnCentreCm, HeadOnHalfSizeCm);

	const FVector ExpectedHeadOnCm(
		HeadOnCentreCm.X, HeadOnCentreCm.Y + HeadOnStandoffCm, HeadOnCentreCm.Z);

	const FViewpoint Defaulted = ViewpointFor(HeadOnBounds, Aspect);
	const FViewpoint ExplicitHeadOn =
		ViewpointFor(HeadOnBounds, Aspect, EScenarioFraming::HeadOn);

	for (const TPair<const TCHAR*, FViewpoint>& Which :
		{ TPair<const TCHAR*, FViewpoint>(TEXT("the defaulted call"), Defaulted),
		  TPair<const TCHAR*, FViewpoint>(TEXT("an explicit HeadOn"), ExplicitHeadOn) })
	{
		TestTrue(
			*FString::Printf(
				TEXT("regression pin (%s): an unset/head-on row must still be viewed from %s, ")
				TEXT("level and facing -Y; it is %s facing (%s, %s, %s)"),
				Which.Key, *ScenariosTestVectorBits(ExpectedHeadOnCm),
				*ScenariosTestVectorBits(Which.Value.LocationCm),
				*ScenariosTestBits(Which.Value.Rotation.Pitch),
				*ScenariosTestBits(Which.Value.Rotation.Yaw),
				*ScenariosTestBits(Which.Value.Rotation.Roll)),
			Which.Value.LocationCm.X == ExpectedHeadOnCm.X
				&& Which.Value.LocationCm.Y == ExpectedHeadOnCm.Y
				&& Which.Value.LocationCm.Z == ExpectedHeadOnCm.Z
				&& Which.Value.Rotation.Pitch == 0.0
				&& Which.Value.Rotation.Yaw == ScenariosTestCameraYawDegrees
				&& Which.Value.Rotation.Roll == 0.0);
	}

	// 2. Data pin: the shed3d row opts into three-quarter framing.

	const int32 Shed3DIndex = IndexOfName(FName(TEXT("shed3d")));

	TestTrue(
		TEXT("the catalogue must carry a 'shed3d' row for the 3D framing to attach to"),
		Shed3DIndex != INDEX_NONE);

	if (Shed3DIndex != INDEX_NONE)
	{
		const FScenario& Shed3D = Catalogue()[Shed3DIndex];

		TestTrue(
			*FString::Printf(
				TEXT("the 3D shed reads as a flat front wall head-on, so its row must opt into ")
				TEXT("ThreeQuarter framing; its Framing is %d (HeadOn=%d, ThreeQuarter=%d)"),
				static_cast<int32>(Shed3D.Framing),
				static_cast<int32>(EScenarioFraming::HeadOn),
				static_cast<int32>(EScenarioFraming::ThreeQuarter)),
			Shed3D.Framing == EScenarioFraming::ThreeQuarter);
	}

	/*
	 * 3. The angled view, on a generic 3D box (every half-extent non-zero). Not the shed's
	 * bounds, to avoid coupling to the builder.
	 */
	const FVector Box3DCentreCm(300.0, 200.0, 150.0);
	const FVector Box3DHalfSizeCm(300.0, 200.0, 150.0);

	const FBox Box3D = FBox::BuildAABB(Box3DCentreCm, Box3DHalfSizeCm);

	const FViewpoint Angled = ViewpointFor(Box3D, Aspect, EScenarioFraming::ThreeQuarter);

	const FRotationMatrix AngledFrame(Angled.Rotation);
	const FVector Forward = AngledFrame.GetScaledAxis(EAxis::X);

	TestTrue(
		*FString::Printf(
			TEXT("orbited: the three-quarter camera must be OFF the centre's X (%s), so the box's ")
			TEXT("depth is not foreshortened; the camera X is %s"),
			*ScenariosTestBits(Box3DCentreCm.X),
			*ScenariosTestBits(Angled.LocationCm.X)),
		Angled.LocationCm.X != Box3DCentreCm.X);

	TestTrue(
		*FString::Printf(
			TEXT("elevated: the three-quarter camera must be ABOVE the centre's Z (%s), so it ")
			TEXT("looks down onto the box; the camera Z is %s"),
			*ScenariosTestBits(Box3DCentreCm.Z),
			*ScenariosTestBits(Angled.LocationCm.Z)),
		Angled.LocationCm.Z > Box3DCentreCm.Z);

	TestTrue(
		*FString::Printf(
			TEXT("pitched down: the view must look downward (forward.Z < 0) rather than level; ")
			TEXT("its forward is %s and its pitch is %s"),
			*ScenariosTestVectorBits(Forward),
			*ScenariosTestBits(Angled.Rotation.Pitch)),
		Forward.Z < 0.0 && Angled.Rotation.Pitch != 0.0);

	TestTrue(
		*FString::Printf(
			TEXT("off-axis yaw: the view must be orbited off the head-on -Y axis, both as a yaw ")
			TEXT("away from %s and as a forward with real horizontal X; yaw %s, forward %s"),
			*ScenariosTestBits(ScenariosTestCameraYawDegrees),
			*ScenariosTestBits(Angled.Rotation.Yaw),
			*ScenariosTestVectorBits(Forward)),
		Angled.Rotation.Yaw != ScenariosTestCameraYawDegrees
			&& FMath::Abs(Forward.X) > 0.05);

	// Still aimed at the centre.
	const FVector ToCentre = (Box3DCentreCm - Angled.LocationCm).GetSafeNormal();
	const double AimDot = FVector::DotProduct(Forward, ToCentre);

	TestTrue(
		*FString::Printf(
			TEXT("aimed at centre: the forward %s must point at the box centre (dir %s); their ")
			TEXT("dot is %s"),
			*ScenariosTestVectorBits(Forward), *ScenariosTestVectorBits(ToCentre),
			*ScenariosTestBits(AimDot)),
		AimDot > 0.99);

	// Bounding sphere in frame: 3D distance to centre vs the box's half-diagonal.
	const double DistanceCm = (Angled.LocationCm - Box3DCentreCm).Size();
	const double RadiusCm = Box3DHalfSizeCm.Size();

	TestTrue(
		*FString::Printf(
			TEXT("fully framed vertically: the camera must stand far enough (distance %s cm) that ")
			TEXT("the box's bounding sphere (radius %s cm) clears the vertical frustum with the ")
			TEXT("%s margin — distance * aspect %s must be >= %s"),
			*ScenariosTestBits(DistanceCm), *ScenariosTestBits(RadiusCm),
			*ScenariosTestBits(ScenariosTestFrameMargin),
			*ScenariosTestBits(DistanceCm * Aspect),
			*ScenariosTestBits(ScenariosTestFrameMargin * RadiusCm)),
		DistanceCm * Aspect >= ScenariosTestFrameMargin * RadiusCm);

	TestTrue(
		*FString::Printf(
			TEXT("fully framed horizontally: distance %s cm must clear the horizontal frustum for ")
			TEXT("the bounding sphere (radius %s) with the %s margin — %s must be >= %s"),
			*ScenariosTestBits(DistanceCm), *ScenariosTestBits(RadiusCm),
			*ScenariosTestBits(ScenariosTestFrameMargin),
			*ScenariosTestBits(DistanceCm),
			*ScenariosTestBits(ScenariosTestFrameMargin * RadiusCm)),
		DistanceCm >= ScenariosTestFrameMargin * RadiusCm);

	return true;
}

/**
 * The build level: a catalogue row that lays nothing, reached by `?Scenario=build` or map
 * `Lvl_Build` through the same IndexForOptionsAndMap as every other row.
 *
 * Pinned by absences, each asserted separately: no LayStructure, a default Wall, no cuts. Build
 * must refuse it and write nothing, which is why the game mode must branch on bBuildSandbox
 * before calling Build. World-free; the game-mode half is
 * World.Scenario.GameModeOpensAnEmptyBuildSandbox.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDestructionScenariosBuildSandboxRowTest,
	"DestructionGame.World.Scenarios.BuildSandboxRowExists",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FDestructionScenariosBuildSandboxRowTest::RunTest(const FString& Parameters)
{
	using namespace ScenariosTestSupport;
	using namespace DestructionScenarios;
	using namespace DestructionLayout;

	const int32 Index = IndexOfName(FName(TEXT("build")));

	if (!Catalogue().IsValidIndex(Index))
	{
		AddError(FString::Printf(
			TEXT("the catalogue must carry a row named 'build' — the level on which the player ")
			TEXT("builds their own structure from nothing; IndexOfName returned %d against %d row(s)"),
			Index, Catalogue().Num()));

		return true;
	}

	const FScenario& Row = Catalogue()[Index];

	// Reachable by option and by map.

	TestEqual(
		TEXT("the build row is selected by its own map"),
		FString(Row.MapName), FString(TEXT("Lvl_Build")));

	{
		EScenarioSelection How = EScenarioSelection::Default;

		const int32 ByOption =
			IndexForOptionsAndMap(TEXT("?Scenario=build"), TEXT("Lvl_NoSuchMap"), How);

		TestTrue(
			*FString::Printf(
				TEXT("?Scenario=build must select the build row (%d) BY OPTION; it selected %d ")
				TEXT("(selection %d, ByOption is %d)"),
				Index, ByOption, static_cast<int32>(How),
				static_cast<int32>(EScenarioSelection::ByOption)),
			ByOption == Index && How == EScenarioSelection::ByOption);
	}

	{
		EScenarioSelection How = EScenarioSelection::Default;

		// By map, using the PIE-decorated name GetMapName returns in the editor.
		const int32 ByMap = IndexForOptionsAndMap(FString(), TEXT("UEDPIE_0_Lvl_Build"), How);

		TestTrue(
			*FString::Printf(
				TEXT("the map Lvl_Build must select the build row (%d) BY MAP NAME, PIE prefix and ")
				TEXT("all; it selected %d (selection %d, ByMapName is %d)"),
				Index, ByMap, static_cast<int32>(How),
				static_cast<int32>(EScenarioSelection::ByMapName)),
			ByMap == Index && How == EScenarioSelection::ByMapName);
	}

	// Flagged a build sandbox, with nothing to lay.

	TestTrue(
		*FString::Printf(
			TEXT("the build row must be FLAGGED a build sandbox — bBuildSandbox is what the game ")
			TEXT("mode branches on, and a row that merely happens to lay nothing is a broken level ")
			TEXT("rather than a build one; it is %s"),
			Row.bBuildSandbox ? TEXT("set") : TEXT("NOT set")),
		Row.bBuildSandbox);

	TestTrue(
		*FString::Printf(
			TEXT("the build row must carry NO LayStructure — the player is the producer; it %s one"),
			static_cast<bool>(Row.LayStructure) ? TEXT("carries") : TEXT("carries no")),
		!static_cast<bool>(Row.LayStructure));

	// Empty wall: the two fields RunningBond and the catalogue sweep use to decide a wall exists.
	TestTrue(
		*FString::Printf(
			TEXT("the build row must carry an EMPTY wall spec — no courses and no bricks per ")
			TEXT("course; it carries %d courses of %d"),
			Row.Wall.CoursesHigh, Row.Wall.BricksPerCourse),
		Row.Wall.CoursesHigh == 0 && Row.Wall.BricksPerCourse == 0);

	TestTrue(
		*FString::Printf(
			TEXT("the build row must cut NOTHING — there is nothing laid to cut; it names %d cut(s)"),
			Row.CutCentresCm.Num()),
		Row.CutCentresCm.Num() == 0);

	// Title and expectation.

	const FString Title(Row.Title != nullptr ? Row.Title : TEXT(""));
	const FString Expectation(Row.Expectation != nullptr ? Row.Expectation : TEXT(""));

	TestTrue(
		*FString::Printf(
			TEXT("the build row's title must say it is the BUILD level — it is the one level whose ")
			TEXT("subject is the player rather than a structure; it reads '%s'"),
			*Title),
		Title.Contains(TEXT("Build")));

	TestTrue(
		*FString::Printf(
			TEXT("the build row must still say in one line what a human should do here; it reads ")
			TEXT("'%s'"),
			*Expectation),
		Expectation.Len() > 0);

	// Build refuses it and writes nothing.

	{
		FBrickLayout Built;
		Built.Boxes.Add(FPieceBox());

		TArray<int32> Cut;
		Cut.Add(4242);

		const bool bBuilt = Build(Row, Built, Cut);

		TestTrue(
			*FString::Printf(
				TEXT("Build must REFUSE the build sandbox — it describes no structure, so there is ")
				TEXT("nothing to lay and an empty-looking wall would be a lie; it returned %s"),
				bBuilt ? TEXT("true") : TEXT("false")),
			!bBuilt);

		TestTrue(
			*FString::Printf(
				TEXT("and the refusal writes nothing: %d box(es), %d piece(s), %d cut piece(s)"),
				Built.Boxes.Num(), Built.Structure.NumPieces(), Cut.Num()),
			Built.Boxes.Num() == 0 && Built.Structure.NumPieces() == 0 && Cut.Num() == 0);
	}

	return true;
}

/**
 * Exactly one catalogue row is a build sandbox. The flag set by accident (e.g. a copied row)
 * silently empties a real level, and no other test would notice since they measure the fixture,
 * not the level. World-free.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FDestructionScenariosOneBuildSandboxTest,
	"DestructionGame.World.Scenarios.EveryOtherRowIsNotABuildSandbox",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FDestructionScenariosOneBuildSandboxTest::RunTest(const FString& Parameters)
{
	using namespace ScenariosTestSupport;
	using namespace DestructionScenarios;

	const TArray<FScenario>& Rows = Catalogue();

	// Floor so an emptied catalogue fails: 34 levels in LEVELS.md plus the build level.
	constexpr int32 ScenariosTestRowFloor = 35;

	TestTrue(
		*FString::Printf(
			TEXT("fixture: the catalogue must carry at least the %d existing levels plus the build ")
			TEXT("sandbox, or this sweeps nothing; it carries %d"),
			ScenariosTestRowFloor - 1, Rows.Num()),
		Rows.Num() >= ScenariosTestRowFloor);

	TArray<FString> SandboxRowNames;

	for (int32 Index = 0; Index < Rows.Num(); ++Index)
	{
		const FScenario& Row = Rows[Index];

		if (!Row.bBuildSandbox)
		{
			continue;
		}

		SandboxRowNames.Add(FString::Printf(TEXT("row %d ('%s')"), Index, *Row.Name.ToString()));

		// A flagged row must also be empty; a flagged row with a real structure would lay nothing.
		TestTrue(
			*FString::Printf(
				TEXT("row %d ('%s') is flagged a build sandbox, so it must describe NO structure: ")
				TEXT("it carries %s LayStructure, %d courses of %d, and %d cut(s)"),
				Index, *Row.Name.ToString(),
				static_cast<bool>(Row.LayStructure) ? TEXT("a") : TEXT("no"),
				Row.Wall.CoursesHigh, Row.Wall.BricksPerCourse, Row.CutCentresCm.Num()),
			!static_cast<bool>(Row.LayStructure) && Row.Wall.CoursesHigh == 0
				&& Row.Wall.BricksPerCourse == 0 && Row.CutCentresCm.Num() == 0);
	}

	AddInfo(FString::Printf(
		TEXT("swept %d catalogue rows; the build sandboxes are: %s"),
		Rows.Num(),
		SandboxRowNames.Num() > 0 ? *FString::Join(SandboxRowNames, TEXT(", ")) : TEXT("none")));

	TestTrue(
		*FString::Printf(
			TEXT("EXACTLY ONE row may be a build sandbox — the flag deletes a level's structure, so ")
			TEXT("a copy-paste onto a real scenario is an empty plot where a wall should be; %d ")
			TEXT("row(s) carry it: %s"),
			SandboxRowNames.Num(),
			SandboxRowNames.Num() > 0 ? *FString::Join(SandboxRowNames, TEXT(", ")) : TEXT("none")),
		SandboxRowNames.Num() == 1);

	const int32 BuildRow = IndexOfName(FName(TEXT("build")));

	TestTrue(
		*FString::Printf(
			TEXT("and the one that carries it must be 'build' (row %d) — the flag is what makes ")
			TEXT("that row the build level rather than a broken one"),
			BuildRow),
		Catalogue().IsValidIndex(BuildRow) && Rows[BuildRow].bBuildSandbox);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
