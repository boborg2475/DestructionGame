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
 * THE SCENARIO CATALOGUE: the fixtures this suite has been proving headlessly, named so they
 * can be joined as levels and watched.
 *
 * WORLD-FREE, AND THAT IS THE POINT OF TESTING IT HERE. DestructionScenarios is boxes and
 * doubles — a spec, a list of brick centres, a camera position — so everything below runs in
 * the fast suite. Nothing here builds a world, spawns an actor or ticks a solver; the game
 * mode's own wiring is a later slice and will need one.
 *
 * =====================================================================================
 * WHY THE SPECS ARE PINNED FIELD FOR FIELD RATHER THAN JUST BUILT
 * =====================================================================================
 *
 * Two rows, and both of them exist to be THE SAME WALL as something that already exists:
 *
 *   - `sandbox` is the wall ADestructionGameGameMode::BeginPlay hardcodes today, so moving
 *     the default into the catalogue must change nothing about what Play gives you.
 *   - `free-end-40` is `StructureArchingTestSupport::ArchWallSpecOfHeight(40)`, the wall
 *     `Core.Structure.AFreeEndDeletionInATallWall` reads. A level that is a LOOKALIKE of
 *     that wall — one course taller, one brick wider — proves nothing about the number that
 *     test prints, and nothing about it would look wrong on screen.
 *
 * So each row's spec is compared against an independently written expectation, field by
 * field, with exact equality. The sandbox expectation is TRANSCRIBED from
 * `GameModeScenarioWallSpec()` in DestructionGameGameMode.cpp rather than imported, because
 * that function is file-local to a translation unit this test cannot reach — and because a
 * test that reaches for production's own constant agrees with it instead of disagreeing when
 * it is wrong.
 *
 * THE TEST INCLUDES Tests/ArchingWallTestSupport.h AND PRODUCTION MUST NOT. That direction
 * is the drift check; the reverse would make the level and the fixture one definition that
 * cannot be caught disagreeing, which is the same thing as not checking.
 */
namespace ScenariosTestSupport
{
	using namespace DestructionLayout;
	using namespace DestructionProfiles;

	/** Print a double so a comparison that failed in the last bit is readable as one. */
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
	 * --- the brick, restated ---------------------------------------------------------
	 *
	 * DESIGN.md's standard UK metric clay brick, and the 1 cm mortar joint that makes the
	 * coordinating grid 22.5 x 11.25 x 7.5. Written out here rather than imported, so a
	 * catalogue row that quietly changed brick format would fail rather than agree.
	 */
	constexpr double ScenariosTestBrickLengthCm = 21.5;
	constexpr double ScenariosTestBrickWidthCm = 10.25;
	constexpr double ScenariosTestBrickHeightCm = 6.5;
	constexpr double ScenariosTestMortarCm = 1.0;

	/**
	 * THE ONE BRICK `free-end-40` CUTS: the outermost full brick of the grounded course.
	 *
	 * Course 0 is an even course, so its full bricks run from x = 0 at one brick pitch each
	 * and the outermost is at x = 0 exactly. Its centre height is half a brick above the
	 * ground, 3.25 cm. Both are derived here from the brick above rather than copied from
	 * the fixture header; the fixture's own `ArchWallEvenBrickXCm(0)` and
	 * `ArchWallCourseZCm(0)` are asserted to agree, which is a cross-check rather than a
	 * definition.
	 */
	constexpr double ScenariosTestFreeEndCutXCm = 0.0;
	constexpr double ScenariosTestFreeEndCutZCm = ScenariosTestBrickHeightCm / 2.0;

	/**
	 * THE WALL THE GAME MODE HARDCODES TODAY, TRANSCRIBED FROM `GameModeScenarioWallSpec()`.
	 *
	 * 30 bricks across, 40 courses, flush ends, standard brick, 1 cm general-purpose mortar
	 * and clay brick's own density. If this and the catalogue's `sandbox` row ever disagree,
	 * the default a player gets on Play has drifted and one of the two is wrong.
	 */
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
	 * EVERY FIELD OF A WALL SPEC, COMPARED EXACTLY.
	 *
	 * `FRunningBondSpec` has no operator==, and writing one in production for a test's
	 * benefit would be production code with no failing test behind it. Spelling the fields
	 * out here also means a field ADDED to the spec later is a visible omission in this
	 * function rather than a silent hole in the comparison.
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
	 * --- the viewpoint, derived from the field of view rather than imported -----------
	 *
	 * UCameraComponent's default FOV is 90 degrees HORIZONTALLY, so at a standoff s the
	 * visible half-width is s exactly and the visible half-height is s * aspect, where the
	 * aspect is the viewport's height over its width. Framing a bounding box therefore needs
	 *
	 *     s >= halfX          and          s * aspect >= halfZ
	 *
	 * and the margin is what keeps the structure off the edges of the frame. The floor stops
	 * a four-brick arm filling the screen with one brick.
	 */
	constexpr double ScenariosTestAspectHeightOverWidth = 1080.0 / 1920.0;
	constexpr double ScenariosTestFrameMargin = 1.25;
	constexpr double ScenariosTestMinimumStandoffCm = 120.0;

	/** Yaw -90 looks along -Y with +X to the right — see the Viewpoint test's header. */
	constexpr double ScenariosTestCameraYawDegrees = -90.0;

	/** A real IEEE NaN, produced the way Tests/LayoutTest.cpp produces one. */
	inline double ScenariosTestMakeNaN()
	{
		volatile double Zero = 0.0;
		return Zero / Zero;
	}
}

/**
 * SLICE A'S TWO ROWS, AND WHAT MAKES EACH OF THEM THE WALL IT CLAIMS TO BE.
 *
 * The count is a FLOOR rather than an equality: later slices add the corbel family and the
 * twenty acceptance walls, and adding a scenario should be adding a row rather than editing
 * this test. What is pinned instead is every property that must hold of EVERY row — a name,
 * a map, a title, a line telling a human what to look for, a wall that could actually be
 * laid, and a lookup that finds it — plus the exact contents of the two rows that exist now.
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

	/* --- what must be true of every row, however many there are ---------------------- */

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

		/*
		 * THE EXPECTATION IS NOT DECORATION. The whole request was to be able to WATCH the
		 * fixtures, and a level with nothing saying what should happen leaves a human unable
		 * to tell a correct wall from a broken one.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s must say in one line what a human should see happen"), *Label),
			Row.Expectation != nullptr && FCString::Strlen(Row.Expectation) > 0);

		/*
		 * EVERY ROW HOLDS, INCLUDING — ESPECIALLY — THE ONES THAT CUT NOTHING.
		 *
		 * ONE CLOCK, AND IT IS NOT THE CUT'S. `HoldSeconds` is how long the level leaves the
		 * structure exactly as laid before it runs: it applies whatever cuts the row names, and it
		 * settles. Seven of the nine rows name no cut at all, so a field that only meant "when the
		 * brick goes" would have nothing to say about them — and a corbel with nothing holding it
		 * is a level that has already collapsed before the player's first frame is drawn, which is
		 * exactly the one thing those levels exist to let a human watch.
		 *
		 * A SECOND OF SLACK IS NOT ENOUGH FOR A HUMAN, so the floor is a whole second rather than
		 * merely positive. A hold of a few frames satisfies "greater than zero" and shows a player
		 * nothing; the assertion is about someone having time to look, so it is written against
		 * that rather than against the degenerate value alone. Non-finite fails the same test,
		 * because every comparison against a NaN is false.
		 */
		TestTrue(
			*FString::Printf(
				TEXT("%s must HOLD its structure as laid long enough for a human to look at it ")
				TEXT("before the level runs — whether or not it cuts anything, and this row cuts ")
				TEXT("%d; its hold is %s s"),
				*Label, Row.CutCentresCm.Num(), *ScenariosTestBits(Row.HoldSeconds)),
			Row.HoldSeconds > 1.0 && FMath::IsFinite(Row.HoldSeconds));

		/*
		 * A ROW THAT CARRIES A RUNNING-BOND SPEC MUST CARRY A COMPLETE ONE.
		 *
		 * NOT EVERY ROW DOES, AND THAT IS THE POINT OF THE GUARD RATHER THAN A HOLE IN THE
		 * CHECK. A corbel is not a running bond — it steps half a cell per course and stands on
		 * an immovable base — so its row is laid by a different producer and leaves `Wall`
		 * untouched. What must hold of EVERY row whatever laid it is that it BUILDS, and
		 * `World.Scenarios.Build` asserts exactly that by building all of them.
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

		/*
		 * THE MAP LOOKUP IS CASE-INSENSITIVE, and it is asserted in both directions rather
		 * than at the one spelling the row happens to use: a map name arrives from a URL or
		 * from UWorld::GetMapName, and neither guarantees the case the catalogue was typed in.
		 */
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

	/* --- the lookups fail closed ----------------------------------------------------- */

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

	/* --- row 1: the sandbox, which must be the wall Play already gives you ------------ */

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

		/*
		 * AND AGAINST THE WORLD-FREE FIXTURE THAT CLAIMS TO BE THE SAME WALL.
		 * ArchingWallTestSupport's ScenarioWallSpec is documented as "the game mode's own
		 * scenario wall" and every figure in ARCHING_DESIGN's span table was worked against
		 * it, so the three definitions have to be one wall or the design table is about a
		 * wall nobody can join.
		 */
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

	/* --- row 2: the free end, which must be the wall the solver suite reads ----------- */

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

	/*
	 * THE CROSS-CHECK, and it is a cross-check rather than the definition: the cut centre
	 * above is derived from the brick's own dimensions, and this says the fixture header the
	 * solver suite reads agrees about where that brick is.
	 */
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
 * BUILDING A ROW LAYS ITS WALL THROUGH RunningBond AND RESOLVES ITS CUT — OR REFUSES OUTRIGHT.
 *
 * =====================================================================================
 * THE WALL IS RunningBond'S AND NOBODY ELSE'S
 * =====================================================================================
 *
 * Asserted by laying the same spec here and comparing the two layouts bit for bit: every
 * box, every mass, every joint. A catalogue that placed its own bricks — even correctly —
 * would be a second opinion about where a brick goes, and the level would stop being the
 * fixture the solver suite measures the moment the two drifted.
 *
 * =====================================================================================
 * THE CUT IS RESOLVED, NOT APPLIED
 * =====================================================================================
 *
 * Build hands back HANDLES and removes nothing: the whole point is that the player watches
 * the brick go, so the removal belongs to the game mode and its delay. So the wall that comes
 * back must be whole — every piece live — and the handles must name real bricks.
 *
 * AND THE HANDLE NUMBER IS NEVER ASSERTED. Which index RunningBond gives a brick is its own
 * business; what the level cares about is that the piece it is about to delete is the
 * outermost full brick of the grounded course. So the assertion is on the resolved piece's
 * BOX — its centre, its full-brick extent — and on its being grounded.
 *
 * =====================================================================================
 * A CUT CENTRE THAT NAMES NO BRICK IS A REFUSED BUILD, AND THIS IS THE ASSERTION THAT MATTERS
 * =====================================================================================
 *
 * A silently dropped cut gives a level that looks intact, never does anything, and reads
 * EXACTLY like a level whose wall correctly stood — which is the one thing this whole
 * exercise exists to let a human tell apart. So a miss refuses, and refusing means writing
 * nothing: the outputs are pre-poisoned here, so a refusal that left a usable-looking wall
 * and a stale cut list behind it fails.
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

	/* --- every row builds, and builds exactly the wall RunningBond lays --------------- */

	for (const FScenario& Row : Catalogue())
	{
		const FString Label = FString::Printf(TEXT("'%s'"), *Row.Name.ToString());

		FBrickLayout Built;
		TArray<int32> Cut;

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
		 * AND A ROW LAID FROM A RUNNING-BOND SPEC MUST BE RunningBond'S WALL AND NOBODY ELSE'S.
		 *
		 * GUARDED, BECAUSE NOT EVERY ROW IS A WALL. A corbel steps half a cell per course off an
		 * immovable base, which `RunningBond` cannot express at all — so the comparison applies
		 * to the rows that carry a spec, and `World.Scenarios.CorbelRows` holds the corbel rows
		 * against the world-free fixture the solver suite measures instead. What is asserted of
		 * every row regardless is above: it builds, it lays something, and its boxes match its
		 * pieces.
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

			/*
			 * BIT FOR BIT, AND THE FIRST DISAGREEMENT IS REPORTED RATHER THAN ALL OF THEM. A wall
			 * is 1,220 pieces; a producer that moved every brick would otherwise print 1,220
			 * failures and bury everything else in this file.
			 */
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

		/* --- the cut is resolved, and nothing has been removed ------------------------ */

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

	/* --- the free-end cut is genuinely the grounded free-end brick -------------------- */

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

			/*
			 * A FULL BRICK AND NOT A HALF BAT. A flush wall's odd courses start with a half
			 * bat, so "the outermost brick of a course" is ambiguous by position alone —
			 * and half a brick coming out of the end of a wall is a different experiment.
			 */
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

	/* --- and a cut that names no brick refuses, writing nothing ----------------------- */

	if (const FScenario* const FreeEnd = ScenariosTestRowNamed(*this, TEXT("free-end-40")))
	{
		const double NotANumber = ScenariosTestMakeNaN();

		struct FScenariosTestMissRow
		{
			const TCHAR* Why;
			TArray<FVector> CutCentresCm;
		};

		/*
		 * THE MISSES ARE CHOSEN TO LOOK PLAUSIBLE, not to look wrong. A number nobody would
		 * type is caught by any guard at all; the head-joint gap and the bed-joint plane are
		 * the two a person actually mistypes, and both would produce a level that stands
		 * there being correct-looking forever.
		 */
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

			/*
			 * PRE-POISONED, so that "wrote nothing" is a claim with teeth. A caller that
			 * ignores the return value must not be handed a wall that looks buildable and a
			 * cut list left over from whatever it did last.
			 */
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
 * WHERE THE PLAYER STANDS SO THE WHOLE STRUCTURE IS IN FRONT OF THEM.
 *
 * =====================================================================================
 * THE ARITHMETIC, DERIVED FROM THE FIELD OF VIEW RATHER THAN FROM PRODUCTION
 * =====================================================================================
 *
 * UCameraComponent's default FOV is 90 degrees HORIZONTALLY, so at a standoff s the visible
 * half-width is s and the visible half-height is s * aspect. Framing a box therefore needs
 * s >= halfX and s >= halfZ / aspect, and the margin keeps it off the edges:
 *
 *     standoff = max(120, 1.25 * max(halfX, halfZ / aspect))
 *     location = (centre.X, centre.Y + standoff, centre.Z)
 *     rotation = (0, -90, 0)
 *
 * THE THREE NAMED ROWS PICK THE THREE BRANCHES DELIBERATELY, and each is worked through in
 * its own comment, because a row where the wrong term governs would still print a perfectly
 * plausible number.
 *
 * =====================================================================================
 * WHY YAW -90 IS ASSERTED AS A DIRECTION AND NOT ONLY AS A NUMBER
 * =====================================================================================
 *
 * Yaw -90 puts the view along -Y with +X to the RIGHT, so a level reads the same way round
 * as claude_plans/CORBEL_CASES.html and every elevation in the design documents. At yaw +90
 * the camera also faces the wall — the picture is not obviously broken — but +X is drawn to
 * the left and every structure comes out mirrored. So the rotation is checked both as the
 * number and as the two axes it produces.
 *
 * =====================================================================================
 * AND THE PROPERTY IS WORTH MORE THAN THE ROWS
 * =====================================================================================
 *
 * What a human cares about is "I can see all of it", which is a relation between the returned
 * standoff and the box rather than a literal. The sweep asserts it over a deterministic grid
 * of shapes, so it keeps holding if somebody retunes the margin or the floor.
 *
 * THE SWEEP ASSERTS TWO SEPARATE PROPERTIES, AND THEY ARE SEPARATE BECAUSE ONE OF THEM CANNOT
 * CARRY THE OTHER'S ROUNDING — see the sweep's own comment for why reading the standoff back
 * out of the stored location is not the number the function returned.
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

	/* --- the three worked rows -------------------------------------------------------- */

	struct FScenariosTestViewRow
	{
		const TCHAR* Why;
		FVector CentreCm;
		FVector HalfSizeCm;
		double ExpectedStandoffCm;
	};

	const FScenariosTestViewRow ViewRows[] =
	{
		/*
		 * WIDTH GOVERNS. halfZ / aspect is 150 / 0.5625 = 266.666..., which is less than the
		 * 350 of half-width, so the standoff is 1.25 * 350 = 437.5 exactly.
		 */
		{ TEXT("a wide wall, where half-width governs"),
			FVector(350.0, 0.0, 150.0), FVector(350.0, 5.125, 150.0), 437.5 },

		/*
		 * HEIGHT GOVERNS, and it governs BECAUSE OF THE ASPECT rather than because it is the
		 * larger extent: 200 / 0.5625 = 355.555..., which beats the 10 of half-width, and
		 * 1.25 x that is 444.44444444444446. An implementation that compared halfZ against
		 * halfX without dividing would pick 10 here and frame a sliver of a tall wall.
		 */
		{ TEXT("a tall thin wall, where half-height over aspect governs"),
			FVector(0.0, 0.0, 200.0), FVector(10.0, 5.125, 200.0), 444.44444444444446 },

		/*
		 * THE FLOOR GOVERNS. 1.25 * (10 / 0.5625) = 22.22..., far below 120, so a four-brick
		 * arm is framed at the floor rather than filling the screen with one brick.
		 */
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

		/*
		 * THE YAW AS A DIRECTION. Looking along -Y is what puts the camera in front of a wall
		 * standing at y = 0 when the camera stands at +y; +X to the right is what stops every
		 * level coming out mirrored against the design drawings.
		 */
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

	/* --- the property: all of it is inside the frustum, with the margin to spare ------- */

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
	 * FRAMING AND PLACEMENT ARE TWO DIFFERENT CLAIMS, AND ONLY ONE OF THEM IS ALLOWED TO CARRY
	 * THE ROUNDING OF A LARGE CENTRE.
	 *
	 *   - FRAMING says the structure is wholly inside the frustum with the margin to spare. It
	 *     is a statement about the STANDOFF and the extents, and it does not stop being true
	 *     when the structure is translated across the world.
	 *   - PLACEMENT says the camera sits directly in front of the centre at that standoff. That
	 *     is the statement about LocationCm, and it is the one that legitimately owns whatever
	 *     the store of centre.Y + standoff rounded to.
	 *
	 * SO THE STANDOFF IS READ FROM A BOX OF THE SAME HALF-SIZE CENTRED AT THE ORIGIN, WHERE IT
	 * IS EXACT. The standoff depends only on the extents, and at the origin the returned Y is
	 * 0 + standoff, which is standoff itself in every rounding mode — so LocationCm.Y there IS
	 * the number the function computed, with nothing lost.
	 *
	 * Reading it back out of the TRANSLATED viewpoint instead, as LocationCm.Y - CentreCm.Y,
	 * does lose something, and the loss is not small in the units that matter: storing
	 * -1500 + 133.33333333333334 rounds to -1366.6666666666667, and subtracting the centre back
	 * out is exact by Sterbenz — so it faithfully recovers 133.33333333333326, three ulps below
	 * what was returned. That is enough to put a 60 cm half-height outside a 75 cm half-frustum
	 * by 4e-14 cm. The error is bounded by half an ulp of |centre.Y + standoff|, so it grows
	 * without bound as the structure moves away from the origin while the standoff does not:
	 * framing asserted that way fails for a reason that is purely about where in the world the
	 * structure sits, which is exactly the thing framing is not about.
	 *
	 * NOTHING IS LOOSENED BY THE SPLIT. Placement is still exact equality against
	 * centre + standoff, and because that standoff came from the origin call it also pins the
	 * standoff as a function of the SHAPE ALONE — a viewpoint that framed a translated
	 * structure differently from the same structure at the origin fails here.
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

				/*
				 * STRICTLY INSIDE, AND WITH THE MARGIN TO SPARE. The first pair is what a
				 * human means by "I can see all of it"; the second is the margin that keeps
				 * the structure off the very edge of the frame, and it is written as the
				 * inequality rather than as the constant so it survives a retune.
				 */
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

	/* --- degenerate bounds fail closed to the floor ----------------------------------- */

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

	/*
	 * AN INVERTED BOX IS THE ONE THAT FAILS OPEN IF NOBODY THOUGHT ABOUT IT. Its extents are
	 * NEGATIVE, so a standoff computed from them alone is negative — the camera would stand
	 * BEHIND the structure looking away from it, which is a level that renders perfectly and
	 * shows nothing at all.
	 */
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

#endif // WITH_DEV_AUTOMATION_TESTS
