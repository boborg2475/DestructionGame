// Copyright Epic Games, Inc. All Rights Reserved.

#include "World/DestructionScenarios.h"

#include "Core/Corbel.h"
#include "Core/DestructionShed.h"
#include "Core/DestructionShed3D.h"
#include "Core/LayoutFile.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/WallCases.h"
#include "Kismet/GameplayStatics.h"

/*
 * File-local names carry a Scenarios prefix inside the named namespace, not an anonymous
 * one: unity builds merge translation units, so colliding file-local names break the build.
 */
namespace DestructionScenarios
{
	// UK metric clay brick on a 1 cm joint (22.5 x 11.25 x 7.5 grid). Every row uses it.
	constexpr double ScenariosBrickLengthCm = 21.5;
	constexpr double ScenariosBrickWidthCm = 10.25;
	constexpr double ScenariosBrickHeightCm = 6.5;
	constexpr double ScenariosMortarJointCm = 1.0;

	/**
	 * How near a cut centre must be to a brick's centre to name it, cm. Far below the
	 * closest distinct centres (5.625 cm) and far above grid rounding (~1e-13 cm).
	 */
	constexpr double ScenariosCutMatchToleranceCm = 1.0e-6;

	/**
	 * URL option key. Parsed with UGameplayStatics::ParseOption, which compares keys; a
	 * substring search would also match `?MyScenario=` or `?Scenarios=`.
	 */
	const TCHAR* const ScenariosOptionKey = TEXT("Scenario");

	/** Fallback row for every miss: `sandbox`. Better than an empty world. */
	constexpr int32 ScenariosDefaultRow = 0;

	/*
	 * The camera's default 90-degree horizontal FOV means at standoff s the visible half-width
	 * is s and half-height is s * aspect. Framing needs s >= halfX and s >= halfZ / aspect;
	 * the margin keeps the structure off the frame edges.
	 */
	constexpr double ScenariosFrameMargin = 1.25;

	/** Minimum standoff, so a small structure does not fill the screen. */
	constexpr double ScenariosMinimumStandoffCm = 120.0;

	/**
	 * The camera looks along -Y. At yaw +90 increasing X draws to the left, so every
	 * structure would be mirrored against the design docs' elevations.
	 */
	constexpr double ScenariosCameraYawDegrees = -90.0;

	/*
	 * Three-quarter view for 3D rows: azimuth orbits toward +X to show depth, elevation looks
	 * down. Chosen by eye; the standoff is sized from the bounding sphere, not the angle.
	 */
	constexpr double ScenariosThreeQuarterAzimuthDegrees = 40.0;
	constexpr double ScenariosThreeQuarterElevationDegrees = 30.0;

	/**
	 * The running-bond wall spec at a given size. Flush ends, because a ragged end starts with
	 * half-seated cantilevers before anything is cut.
	 */
	static DestructionLayout::FRunningBondSpec ScenariosWallSpec(
		int32 CoursesHigh, int32 BricksPerCourse)
	{
		DestructionLayout::FRunningBondSpec Spec;

		Spec.BrickSizeCm =
			FVector(ScenariosBrickLengthCm, ScenariosBrickWidthCm, ScenariosBrickHeightCm);

		Spec.JointThicknessCm = ScenariosMortarJointCm;
		Spec.DensityGramsPerCubicCm = DestructionProfiles::ClayBrick.DensityGramsPerCubicCm;
		Spec.CoursesHigh = CoursesHigh;
		Spec.BricksPerCourse = BricksPerCourse;
		Spec.End = DestructionLayout::EWallEnd::Flush;
		Spec.Strength = DestructionProfiles::GeneralPurposeMortar;

		return Spec;
	}

	/*
	 * Corbel family: three base courses, arm advancing 11.25 cm per course (3.46x published
	 * practice). Counterweight cases shift their origin by three cells so the root joint stays
	 * at the same X; only the masonry opposite differs.
	 */
	constexpr int32 ScenariosCorbelBaseCourses = 3;

	constexpr double ScenariosCorbelCellPitchCm = ScenariosBrickLengthCm + ScenariosMortarJointCm;
	constexpr double ScenariosCorbelStepCm = ScenariosCorbelCellPitchCm / 2.0;

	constexpr double ScenariosCorbelCounterweightOriginCm = -3.0 * ScenariosCorbelCellPitchCm;

	/** One corbel row. */
	struct FScenariosCorbelRow
	{
		const TCHAR* Name;
		const TCHAR* MapName;
		const TCHAR* Title;
		const TCHAR* Expectation;

		/** Base width in cells: two bare, five with a counterweight. */
		int32 BaseCells;

		/** Left edge of the base, cm. */
		double LeftOriginCm;

		int32 Steps;

		/** False only for case A, the bare stepped arm of single bricks. */
		bool bFilled;
	};

	/**
	 * The seven corbels. None is cut: a corbel stands or falls by its own geometry.
	 * E35/E36 were the pair either side of the old crossover in
	 * `Core.Structure.CorbelStepsBeforeTensionWins`.
	 */
	const FScenariosCorbelRow ScenariosCorbelRows[] =
	{
		{ TEXT("corbel-a-bare-4"), TEXT("Lvl_CorbelABare4"),
			TEXT("Corbel A — a bare stepped arm of four single bricks"),
			TEXT("A bare stepped arm of four single bricks — the minimal case, and the one with no "
				"equilibrium at its bearing at all. Nothing is cut: whatever happens, happens "
				"because of how it was laid."),
			2, 0.0, 4, false },

		{ TEXT("corbel-b-filled-4"), TEXT("Lvl_CorbelBFilled4"),
			TEXT("Corbel B — the same four steps, filled solid"),
			TEXT("The same four-step profile FILLED SOLID. Nothing is cut; compare it against the "
				"bare arm, which is the same reach carrying a different load path."),
			2, 0.0, 4, true },

		{ TEXT("corbel-c-10"), TEXT("Lvl_CorbelC10"),
			TEXT("Corbel C — ten steps off a two-cell base"),
			TEXT("Filled, ten steps, on the bare two-cell base. Nothing is cut: the whole story is "
				"whether it settles where it was laid."),
			2, 0.0, 10, true },

		{ TEXT("corbel-d-10-counterweight"), TEXT("Lvl_CorbelD10Counterweight"),
			TEXT("Corbel D — the same ten steps, with masonry opposite"),
			TEXT("Case C plus three cells of masonry opposite — the counterweight. Nothing is cut. "
				"Its root joint is the same joint in the same place as C's, carrying the same "
				"column, so what differs between the two levels is the masonry behind the joint."),
			5, ScenariosCorbelCounterweightOriginCm, 10, true },

		{ TEXT("corbel-e35"), TEXT("Lvl_CorbelE35"),
			TEXT("Corbel E35 — the old crossover's lower rung"),
			TEXT("Thirty-five steps, and it stands. This pair used to straddle the model's "
				"crossover — E36 was the first corbel condemned by its own root joint — until the "
				"mean-strength re-anchor moved that crossover far out past a hundred steps, where "
				"crushing takes over from the bond. Nothing is cut, and nothing should move."),
			5, ScenariosCorbelCounterweightOriginCm, 35, true },

		{ TEXT("corbel-e36"), TEXT("Lvl_CorbelE36"),
			TEXT("Corbel E36 — the old crossover's upper rung"),
			TEXT("One more course than E35 is the entire difference, and at the mean strengths it "
				"no longer decides anything: both rungs stand as laid. The step count where a "
				"corbel of this family really gives — by crushing its root, not opening it — is "
				"around a hundred and twenty-four."),
			5, ScenariosCorbelCounterweightOriginCm, 36, true },

		{ TEXT("corbel-f-100"), TEXT("Lvl_CorbelF100"),
			TEXT("Corbel F — a hundred steps, eleven metres out"),
			TEXT("The hundred-step corbel — 11.25 m of overhang off a one-metre base, 3,015 bricks. "
				"Nothing is cut: it comes down under its own weight or it does not."),
			5, ScenariosCorbelCounterweightOriginCm, 100, true },
	};

	/** What one corbel row asks production to lay. */
	static DestructionCorbel::FCorbelSpec ScenariosCorbelSpecOf(const FScenariosCorbelRow& Row)
	{
		DestructionCorbel::FCorbelSpec Spec;

		Spec.Scale = 1.0;
		Spec.StepCm = ScenariosCorbelStepCm;
		Spec.BaseCourses = ScenariosCorbelBaseCourses;
		Spec.BaseCells = Row.BaseCells;
		Spec.Steps = Row.Steps;
		Spec.bFilled = Row.bFilled;
		Spec.Strength = DestructionProfiles::GeneralPurposeMortar;

		Spec.LeftOriginCm = Row.LeftOriginCm;

		return Spec;
	}

	/*
	 * The acceptance walls: one level per row of claude_plans/WALL_CASES.html, laid by the
	 * same producer `Acceptance.Wall.Catalogue` measures. Geometry and cuts only; verdicts
	 * belong to the acceptance test.
	 */
	constexpr double ScenariosWallCellPitchCm = ScenariosBrickLengthCm + ScenariosMortarJointCm;
	constexpr double ScenariosWallHalfCellCm = ScenariosWallCellPitchCm * 0.5;

	// Thirty courses, not the drawing's ten: ten reads well under capacity and shows nothing.
	constexpr int32 ScenariosWallTallCourses = 30;
	constexpr int32 ScenariosWallStandardCells = 12;
	constexpr int32 ScenariosWallCoveredCourses = 12;

	constexpr int32 ScenariosWallCorbelCells = 8;
	constexpr int32 ScenariosWallCorbelFirstCourse = 6;
	constexpr double ScenariosWallQuarterBrickStepCm = ScenariosWallHalfCellCm * 0.5;
	constexpr double ScenariosWallHalfBrickStepCm = ScenariosWallHalfCellCm;

	// A: one brick out.

	const DestructionWallCases::FWallRegion ScenariosWall02Cuts[] = { { 1, 1, 5.25, 5.75 } };
	const DestructionWallCases::FWallRegion ScenariosWall03Cuts[] = { { 1, 1, 11.00, 11.50 } };
	const DestructionWallCases::FWallRegion ScenariosWall04Cuts[] = { { 0, 0, 4.75, 5.25 } };

	const DestructionWallCases::FWallRegion ScenariosWall05Cuts[] =
	{
		{ 1, 1, 1.25, 1.75 },
		{ 1, 1, 3.25, 3.75 },
		{ 1, 1, 5.25, 5.75 },
		{ 1, 1, 7.25, 7.75 },
		{ 1, 1, 9.25, 9.75 },
	};

	// B: openings and depth of cover.

	const DestructionWallCases::FWallRegion ScenariosWallTwoCellOpening[] = { { 1, 3, 4.75, 6.25 } };
	const DestructionWallCases::FWallRegion ScenariosWallFourCellOpening[] = { { 1, 3, 3.75, 7.25 } };

	/** Ten cells of opening in a fourteen-cell wall, two cells of jamb either side. */
	const DestructionWallCases::FWallRegion ScenariosWall09Cuts[] = { { 1, 3, 1.75, 11.25 } };

	/** The same four cells of cover as case 7, cut through to the free right end. */
	const DestructionWallCases::FWallRegion ScenariosWall10Cuts[] = { { 1, 3, 7.75, 11.50 } };

	// C: spanning between supports.

	const DestructionWallCases::FWallRegion ScenariosWall11Cuts[] = { { 0, 3, 2.75, 8.25 } };
	const DestructionWallCases::FWallRegion ScenariosWall12Cuts[] = { { 0, 3, 0.75, 6.25 } };

	// E and F: the bond, the lost base, and the staircase void.

	const DestructionWallCases::FWallRegion ScenariosWall18Cuts[] = { { 5, 5, 4.75, 5.25 } };
	const DestructionWallCases::FWallRegion ScenariosWall19Cuts[] = { { 0, 0, -0.50, 5.25 } };

	// G: openings too big for what covers them.

	/** An eighteen-cell opening (cells 2..19), two courses of cover. */
	const DestructionWallCases::FWallRegion ScenariosWall21Cuts[] = { { 1, 3, 1.75, 19.25 } };

	/** Case 9's own eight courses of cover, over a span grown to thirty-five cells. */
	const DestructionWallCases::FWallRegion ScenariosWall22Cuts[] = { { 1, 3, 1.75, 36.25 } };

	/** The raking cut: each course up is cut one cell less far right, so the masonry steps out over the hole. */
	const DestructionWallCases::FWallRegion ScenariosWall20Cuts[] =
	{
		{ 1, 1, 0.5, 6.5 },
		{ 2, 2, 0.5, 5.5 },
		{ 3, 3, 0.5, 4.5 },
		{ 4, 4, 0.5, 3.5 },
		{ 5, 5, 0.5, 2.5 },
		{ 6, 6, 0.5, 1.5 },
	};

	/** One acceptance wall as a level. */
	struct FScenariosWallRow
	{
		const TCHAR* Name;
		const TCHAR* MapName;
		const TCHAR* Title;
		const TCHAR* Expectation;

		int32 Courses;
		int32 Cells;

		DestructionWallCases::EWallBond Bond = DestructionWallCases::EWallBond::Running;

		int32 CorbelFromCourse = INDEX_NONE;
		double CorbelStepCm = 0.0;
		int32 ProjectingCourse = INDEX_NONE;

		TArrayView<const DestructionWallCases::FWallRegion> Cuts;
	};

	/**
	 * The acceptance walls. Each caption carries `Expected: STANDS`, `LOCAL LOSS` or `COLLAPSE`
	 * matching the acceptance row, plus "the model currently disagrees" exactly where the solver
	 * differs. `Acceptance.Wall.EveryLevelsCaptionTellsTheTruth` checks both directions.
	 */
	const FScenariosWallRow ScenariosWallRows[] =
	{
		{ TEXT("wall-01"), TEXT("Lvl_Wall01"), TEXT("Intact wall"),
			TEXT("Nothing is cut. Thirty courses of flush running bond, twelve bricks across. "
				"Expected: STANDS — this is the baseline every other wall in the set is a deletion "
				"from."),
			ScenariosWallTallCourses, ScenariosWallStandardCells },

		{ TEXT("wall-02"), TEXT("Lvl_Wall02"), TEXT("One brick out, mid-wall"),
			TEXT("One brick goes from the second course, in the middle of the wall, with twenty-eight "
				"courses standing over it. Expected: STANDS — a real wall does not notice a single "
				"brick; the two above the hole carry across it."),
			ScenariosWallTallCourses, ScenariosWallStandardCells,
			DestructionWallCases::EWallBond::Running, INDEX_NONE, 0.0, INDEX_NONE,
			ScenariosWall02Cuts },

		{ TEXT("wall-03"), TEXT("Lvl_Wall03"), TEXT("One brick out at the free end"),
			TEXT("The same deletion at the free right-hand end, where there is nothing beyond the "
				"hole to arch against and the masonry over it has to cantilever. Expected: STANDS — "
				"this is the case a player reported, and the wall coming down is the bug."),
			ScenariosWallTallCourses, ScenariosWallStandardCells,
			DestructionWallCases::EWallBond::Running, INDEX_NONE, 0.0, INDEX_NONE,
			ScenariosWall03Cuts },

		{ TEXT("wall-04"), TEXT("Lvl_Wall04"), TEXT("One brick out of the bottom course"),
			TEXT("One brick goes from the grounded course, so the wall loses a cell of its footing "
				"rather than a cell of itself. Expected: STANDS — it bridges the gap instead of "
				"sitting down into it."),
			ScenariosWallTallCourses, ScenariosWallStandardCells,
			DestructionWallCases::EWallBond::Running, INDEX_NONE, 0.0, INDEX_NONE,
			ScenariosWall04Cuts },

		{ TEXT("wall-05"), TEXT("Lvl_Wall05"), TEXT("Alternate bricks out of one course"),
			TEXT("Five bricks go from the second course, every other one, leaving a brick of bearing "
				"between each pair of holes. Expected: STANDS — five short spans are not one long "
				"one."),
			ScenariosWallTallCourses, ScenariosWallStandardCells,
			DestructionWallCases::EWallBond::Running, INDEX_NONE, 0.0, INDEX_NONE,
			ScenariosWall05Cuts },

		{ TEXT("wall-06"), TEXT("Lvl_Wall06"), TEXT("Two-brick opening, deep cover"),
			TEXT("A two-brick opening with eight courses of masonry over it. Expected: STANDS — a "
				"short span under deep cover is the easiest arch there is."),
			ScenariosWallCoveredCourses, ScenariosWallStandardCells,
			DestructionWallCases::EWallBond::Running, INDEX_NONE, 0.0, INDEX_NONE,
			ScenariosWallTwoCellOpening },

		{ TEXT("wall-07"), TEXT("Lvl_Wall07"), TEXT("Four-brick opening, eight courses over"),
			TEXT("A four-brick opening, still with eight courses over it. Expected: STANDS. This is "
				"the wall cases 8, 9 and 10 are each compared against, one variable at a time — "
				"depth of cover, span, and whether there is an abutment."),
			ScenariosWallCoveredCourses, ScenariosWallStandardCells,
			DestructionWallCases::EWallBond::Running, INDEX_NONE, 0.0, INDEX_NONE,
			ScenariosWallFourCellOpening },

		{ TEXT("wall-08"), TEXT("Lvl_Wall08"), TEXT("Four-brick opening, one course over"),
			TEXT("The same four-brick opening with ONE course over it. The two middle bricks of that "
				"course have no bed of their own, but the course does not need one: it jams into the "
				"toothed jambs either side as a flat arch, and the two bare bricks hang on their head "
				"joints rather than falling through them. Expected: STANDS — the limit theorem prices "
				"the margin at roughly 325 times the course's own weight, and both the model and the "
				"oracle stand it."),
			5, ScenariosWallStandardCells,
			DestructionWallCases::EWallBond::Running, INDEX_NONE, 0.0, INDEX_NONE,
			ScenariosWallFourCellOpening },

		{ TEXT("wall-09"), TEXT("Lvl_Wall09"), TEXT("Ten-brick opening, eight courses over"),
			TEXT("Ten bricks of opening under the same eight courses of cover as case 7 — two and a "
				"half times the span, everything else identical. Expected: STANDS — the 60 cm of "
				"masonry over the hole is a deep beam spanning 2.1 metres, not a triangle looking "
				"for somewhere to arch, and it carries its own weight in bending at a fraction of "
				"the mortar's mean bond strength. Production, the hand arithmetic and the "
				"limit-theorem oracle all agree it holds."),
			ScenariosWallCoveredCourses, 14,
			DestructionWallCases::EWallBond::Running, INDEX_NONE, 0.0, INDEX_NONE,
			ScenariosWall09Cuts },

		{ TEXT("wall-10"), TEXT("Lvl_Wall10"), TEXT("Opening at a free end, no abutment"),
			TEXT("Case 7's four cells of cover, cut through to the free end so one side of the "
				"opening has nothing beyond it. Expected: STANDS — the panel hanging off the jamb "
				"cantilevers as a bonded deep beam, easily under both its characteristic and mean "
				"bond strength, and the equilibrium LP now carries it exactly so."),
			ScenariosWallCoveredCourses, ScenariosWallStandardCells,
			DestructionWallCases::EWallBond::Running, INDEX_NONE, 0.0, INDEX_NONE,
			ScenariosWall10Cuts },

		{ TEXT("wall-11"), TEXT("Lvl_Wall11"), TEXT("Wall on two piers, six-brick clear span"),
			TEXT("The bottom four courses go from between two piers, leaving a six-brick clear span "
				"on three cells of bearing at each end. Expected: STANDS — the 60 cm of bonded "
				"masonry over the opening spans it as a deep beam, and the 66.5 cm piers take the "
				"thrust easily. Real walls bridge a gap this size routinely."),
			ScenariosWallCoveredCourses, ScenariosWallStandardCells,
			DestructionWallCases::EWallBond::Running, INDEX_NONE, 0.0, INDEX_NONE,
			ScenariosWall11Cuts },

		{ TEXT("wall-12"), TEXT("Lvl_Wall12"), TEXT("The same span on a one-brick pier"),
			TEXT("Case 11's own six-brick clear span, but the LEFT pier is cut back to ONE cell — a "
				"bonded column four courses to the springing — while the right side keeps its five. "
				"Expected: STANDS — the panel's own weight puts a modest ~400 N of thrust on the "
				"narrow pier, and the bonded pier resists four to eight times that. A garden-wall "
				"opening on a single 215 mm jamb under 60 cm of bonded brickwork is common "
				"construction, and it stands."),
			ScenariosWallCoveredCourses, ScenariosWallStandardCells,
			DestructionWallCases::EWallBond::Running, INDEX_NONE, 0.0, INDEX_NONE,
			ScenariosWall12Cuts },

		{ TEXT("wall-13"), TEXT("Lvl_Wall13"), TEXT("Corbel, quarter brick per course"),
			TEXT("Nothing is cut: this wall is CORBELLED as laid, its top four courses each stepping "
				"a quarter of a brick further out than the one below. Expected: STANDS — the joint "
				"at the bottom of the corbel reads about seven percent of what would tear it open."),
			10, ScenariosWallCorbelCells,
			DestructionWallCases::EWallBond::Running,
			ScenariosWallCorbelFirstCourse, ScenariosWallQuarterBrickStepCm, INDEX_NONE },

		{ TEXT("wall-14"), TEXT("Lvl_Wall14"), TEXT("Corbel, half brick per course"),
			TEXT("The same corbel stepping HALF a brick per course — twice the projection of case 13 "
				"and nothing else changed. Nothing is cut. Expected: STANDS, at about a fifth of "
				"what holds it: doubling the step nearly triples the reading, and that difference is "
				"the whole point of the pair."),
			10, ScenariosWallCorbelCells,
			DestructionWallCases::EWallBond::Running,
			ScenariosWallCorbelFirstCourse, ScenariosWallHalfBrickStepCm, INDEX_NONE },

		{ TEXT("wall-15"), TEXT("Lvl_Wall15"), TEXT("Header out half a brick, six courses on top"),
			TEXT("Nothing is cut: one brick of the fourth course is laid HALF A BRICK past the face "
				"of the wall, with six courses standing on its tail. Expected: STANDS — the load "
				"above closes the joint the overhang is trying to open."),
			10, ScenariosWallStandardCells,
			DestructionWallCases::EWallBond::Running, INDEX_NONE, 0.0, 3 },

		{ TEXT("wall-16"), TEXT("Lvl_Wall16"), TEXT("The same header at the top, nothing on it"),
			TEXT("The same projecting header, this time in the TOP course with nothing above it. "
				"Nothing is cut. Expected: STANDS — cured mortar carries a brick half over air at "
				"about six percent of its bond strength, thirty-two times the reading case 15 takes "
				"with the wall standing on it."),
			10, ScenariosWallStandardCells,
			DestructionWallCases::EWallBond::Running, INDEX_NONE, 0.0, 9 },

		{ TEXT("wall-17"), TEXT("Lvl_Wall17"), TEXT("Stack bond, intact"),
			TEXT("Nothing is cut. STACK BOND: every course identical, so every head joint lines up "
				"through the full height of the wall and no brick spans two below it. Expected: "
				"STANDS."),
			10, ScenariosWallStandardCells, DestructionWallCases::EWallBond::Stack },

		{ TEXT("wall-18"), TEXT("Lvl_Wall18"), TEXT("Stack bond, one brick out"),
			TEXT("One brick goes from the middle of a stack-bonded wall, so the column of bricks "
				"over the hole has no bond to hand its weight sideways through and hangs on its two "
				"head joints instead. Expected: STANDS."),
			10, ScenariosWallStandardCells, DestructionWallCases::EWallBond::Stack,
			INDEX_NONE, 0.0, INDEX_NONE, ScenariosWall18Cuts },

		{ TEXT("wall-19"), TEXT("Lvl_Wall19"), TEXT("Bottom course out under half the wall"),
			TEXT("Six bricks of the grounded course go, taking the footing out from under the "
				"left-hand half of the wall. Expected: STANDS — the wedge of masonry left without "
				"a footing cantilevers off the half that still has one, over more than a metre, the "
				"way a bay of underpinning relies on the wall above bridging it; the hand check straddles "
				"the published strength bases, the closest call in the set, and the equilibrium LP "
				"now carries the wedge instead of stranding it."),
			10, ScenariosWallStandardCells,
			DestructionWallCases::EWallBond::Running, INDEX_NONE, 0.0, INDEX_NONE,
			ScenariosWall19Cuts },

		{ TEXT("wall-20"), TEXT("Lvl_Wall20"), TEXT("Staircase void"),
			TEXT("A raking void cut up through the wall, each course reaching one cell less far "
				"right than the one below, so the masonry beside it steps out over the hole as it "
				"rises. Expected: STANDS — the equilibrium LP holds the whole overhang, the two "
				"half-seated teeth included, ~129x clear of any admissible collapse. The old reading "
				"expected the two end teeth to drop as a local loss; the LP finds a global force "
				"path that carries them, so nothing comes down."),
			ScenariosWallCoveredCourses, 14,
			DestructionWallCases::EWallBond::Running, INDEX_NONE, 0.0, INDEX_NONE,
			ScenariosWall20Cuts },

		{ TEXT("wall-21"), TEXT("Lvl_Wall21"), TEXT("Eighteen-brick opening, two courses over"),
			TEXT("Two courses of brick span an opening most of four metres wide. Expected: COLLAPSE — "
				"two courses cannot span that far: the hand deep-beam check reads 1.71x the mean "
				"bond the profiles now carry, so the cover sheds onto the floor while the jambs "
				"either side keep their footing. THE MODEL CURRENTLY DISAGREES: since the "
				"mean-strength re-anchor it stands the whole wall on a knife edge — its worst "
				"joint reads 0.9354 of capacity, 6.5 percent under the line, and it is not even "
				"the bending the hand check condemns (that tension reads 3.1x lower in the model; "
				"which axis the 0.9354 IS has not been decomposed) — so the ruled verdict rests on "
				"the hand statics. The limit-theorem oracle stands it too, by mobilising the jamb "
				"bed joints' cohesion in full — rigid-plastic charity toward a brittle bond, "
				"booked as a scope limit of the oracle rather than a load path the wall really "
				"has."),
			6, 22,
			DestructionWallCases::EWallBond::Running, INDEX_NONE, 0.0, INDEX_NONE,
			ScenariosWall21Cuts },

		{ TEXT("wall-22"), TEXT("Lvl_Wall22"), TEXT("Thirty-five-brick opening, eight courses over"),
			TEXT("The same eight courses of cover that easily carry case 9's two-metre opening are "
				"asked to carry nearly eight metres instead. Expected: COLLAPSE — bending grows with "
				"the square of the span, so the masonry that read 0.089 MPa there reads 1.16 MPa "
				"here, 1.66x the mean bond the profiles carry; production and the hand statics both "
				"agree it falls, and the limit-theorem oracle refuses to answer at this span rather "
				"than disagreeing with it."),
			ScenariosWallCoveredCourses, 39,
			DestructionWallCases::EWallBond::Running, INDEX_NONE, 0.0, INDEX_NONE,
			ScenariosWall22Cuts },
	};

	/** What one acceptance wall asks production to lay. */
	static DestructionWallCases::FWallSpec ScenariosWallCaseSpecOf(const FScenariosWallRow& Row)
	{
		DestructionWallCases::FWallSpec Spec;

		Spec.BrickSizeCm =
			FVector(ScenariosBrickLengthCm, ScenariosBrickWidthCm, ScenariosBrickHeightCm);

		Spec.JointThicknessCm = ScenariosMortarJointCm;
		Spec.DensityGramsPerCubicCm = DestructionProfiles::ClayBrick.DensityGramsPerCubicCm;
		Spec.CoursesHigh = Row.Courses;
		Spec.Cells = Row.Cells;
		Spec.Bond = Row.Bond;
		Spec.CorbelFromCourse = Row.CorbelFromCourse;
		Spec.CorbelStepCm = Row.CorbelStepCm;
		Spec.ProjectingCourse = Row.ProjectingCourse;
		Spec.Strength = DestructionProfiles::GeneralPurposeMortar;

		return Spec;
	}

	/** Every row, built once. */
	static TArray<FScenario> ScenariosBuildCatalogue()
	{
		TArray<FScenario> Rows;

		// The default wall: 30 bricks across, 40 courses, nothing cut.
		FScenario& Sandbox = Rows.AddDefaulted_GetRef();

		Sandbox.Name = FName(TEXT("sandbox"));
		Sandbox.MapName = TEXT("Lvl_Sandbox");
		Sandbox.Title = TEXT("Sandbox — the wall Play has always given you");

		Sandbox.Expectation = TEXT(
			"Nothing is cut: six and a half metres of wall stands there, and goes on standing "
			"until you pull a brick out of it yourself.");

		Sandbox.Wall = ScenariosWallSpec(40, 30);

		/*
		 * The build plot: lays nothing, the player builds. A catalogue row so `?Scenario=build`
		 * and `Lvl_Build` resolve like any level. `Build` refuses it (zero courses), so the
		 * game mode checks bBuildSandbox first (see BeginPlay). Three-quarter framing, since
		 * head-on over an empty plot shows only the horizon.
		 */
		FScenario& BuildSandbox = Rows.AddDefaulted_GetRef();

		BuildSandbox.Name = FName(TEXT("build"));
		BuildSandbox.MapName = TEXT("Lvl_Build");
		BuildSandbox.Title = TEXT("Build sandbox — your own building");

		BuildSandbox.Expectation = TEXT(
			"Nothing is cut. Lay bricks, then switch to Destroy and pull one out.");

		BuildSandbox.bBuildSandbox = true;
		BuildSandbox.Framing = EScenarioFraming::ThreeQuarter;

		/*
		 * The user's reported case: one brick out at a free end under forty courses. Same
		 * fixture as Core.Structure.AFreeEndDeletionInATallWall. Nothing else should move.
		 */
		FScenario& FreeEnd = Rows.AddDefaulted_GetRef();

		FreeEnd.Name = FName(TEXT("free-end-40"));
		FreeEnd.MapName = TEXT("Lvl_FreeEnd40");
		FreeEnd.Title = TEXT("One brick out of a free end, under forty courses");

		FreeEnd.Expectation = TEXT(
			"The outermost brick of the bottom course goes — and the wall must NOT come down. "
			"The brick above it keeps one seat and carries; anything else falling is a bug.");

		FreeEnd.Wall = ScenariosWallSpec(40, 7);

		// The outermost brick of the grounded course.
		FreeEnd.CutCentresCm.Add(FVector(0.0, 0.0, ScenariosBrickHeightCm / 2.0));

		// Corbels carry the call that lays them, and no cut.
		for (const FScenariosCorbelRow& Corbel : ScenariosCorbelRows)
		{
			FScenario& Row = Rows.AddDefaulted_GetRef();

			Row.Name = FName(Corbel.Name);
			Row.MapName = Corbel.MapName;
			Row.Title = Corbel.Title;
			Row.Expectation = Corbel.Expectation;

			const DestructionCorbel::FCorbelSpec Spec = ScenariosCorbelSpecOf(Corbel);

			Row.LayStructure = [Spec](DestructionLayout::FBrickLayout& OutLayout)
			{
				return DestructionCorbel::Build(Spec, OutLayout);
			};
		}

		/*
		 * Acceptance walls carry the call that lays them (none fits the `Wall` spec). Cuts are
		 * written as (course, cell) regions and resolved to brick centres here, once.
		 */
		for (const FScenariosWallRow& Wall : ScenariosWallRows)
		{
			FScenario& Row = Rows.AddDefaulted_GetRef();

			Row.Name = FName(Wall.Name);
			Row.MapName = Wall.MapName;
			Row.Title = Wall.Title;
			Row.Expectation = Wall.Expectation;

			const DestructionWallCases::FWallSpec Spec = ScenariosWallCaseSpecOf(Wall);

			Row.LayStructure = [Spec](DestructionLayout::FBrickLayout& OutLayout)
			{
				DestructionWallCases::FWallLayout Laid;

				if (!DestructionWallCases::Build(Spec, Laid))
				{
					return false;
				}

				OutLayout = MoveTemp(Laid.Layout);

				return true;
			};

			DestructionWallCases::FWallLayout Laid;

			if (!DestructionWallCases::Build(Spec, Laid))
			{
				continue;
			}

			TArray<int32> CutPieces;
			DestructionWallCases::PiecesInRegions(Laid, Wall.Cuts, CutPieces);

			for (const int32 Piece : CutPieces)
			{
				Row.CutCentresCm.Add(Laid.Layout.Boxes[Piece].CentreCm);
			}
		}

		/*
		 * The shed (SHED_PATH.md Phase F): brick piers, Timber roof, and a Timber overhang on a
		 * post plus a screwed fixing. The cut pulls the post; the screw alone cannot cantilever
		 * the overhang, so it drops while the piers stand.
		 */
		FScenario& Shed = Rows.AddDefaulted_GetRef();

		Shed.Name = FName(TEXT("shed"));
		Shed.MapName = TEXT("Lvl_Shed");
		Shed.Title = TEXT("A brick shed with a wooden roof and a post-supported overhang");

		Shed.Expectation = TEXT(
			"A brick shed: two clay-brick piers carry a wooden roof, and a wooden overhang reaches "
			"out over the door on a wooden post. Pull the post and the overhang drops, while the two "
			"piers keep standing.");

		Shed.LayStructure = [](DestructionLayout::FBrickLayout& OutLayout)
		{
			return DestructionShed::Build(DestructionShed::FShedSpec{}, OutLayout);
		};

		// The post's centre, from the same spec fields the builder reads.
		const DestructionShed::FShedSpec ShedSpec;
		const double ShedPostTopZCm =
			ShedSpec.BaseHeightCm + ShedSpec.JointThicknessCm + ShedSpec.HeadHeightCm;

		Shed.CutCentresCm.Add(FVector(ShedSpec.PostCentreCm, 0.0, ShedPostTopZCm / 2.0));

		/*
		 * The 3D shed (THREED_DESIGN.md Phase F), laid by BuildRecognizable and flagged 3D.
		 * The cut pulls a porch post: the narrow cleat has no useful X-couple, so the porch tips
		 * toward the gap and drops while the rest stands.
		 */
		FScenario& Shed3D = Rows.AddDefaulted_GetRef();

		Shed3D.Name = FName(TEXT("shed3d"));
		Shed3D.MapName = TEXT("Lvl_Shed3D");
		Shed3D.Title = TEXT("A brick shed with a gable roof, a door, a window and a post-supported porch");

		Shed3D.Expectation = TEXT(
			"A brick shed: clay-brick walls with a door and a window rise to stepped gables carrying a "
			"wooden gable roof, and a wooden porch overhang reaches out over the door on two wooden "
			"posts. Pull a post and the porch drops, while the shed keeps standing.");

		Shed3D.LayStructure = [](DestructionLayout::FBrickLayout& OutLayout)
		{
			return DestructionShed3D::BuildRecognizable(OutLayout);
		};

		// Head-on would hide both the depth and the fall.
		Shed3D.Framing = EScenarioFraming::ThreeQuarter;

		// Right-hand porch post, X[215,245] Y[328,352] Z[0,200].
		Shed3D.CutCentresCm.Add(FVector(230.0, 340.0, 100.0));

		/*
		 * The real-brick shed (BuildRealistic, 442 pieces, flagged 3D). The cut removes the
		 * back wall's eaves course (15), which the free-standing back gable (16..19) beds on;
		 * the gable loses its path down while the wall body deep-beams and stands. The back
		 * wall is the one the three-quarter camera faces. Above the 200-block cap, so the
		 * router is the break authority; measured, 24 pieces lose the earth.
		 */
		FScenario& ShedRealistic = Rows.AddDefaulted_GetRef();

		ShedRealistic.Name = FName(TEXT("shedrealistic"));
		ShedRealistic.MapName = TEXT("Lvl_ShedRealistic");
		ShedRealistic.Title =
			TEXT("A real-brick shed — knock out the back eaves course and the whole back gable end comes down");

		ShedRealistic.Expectation = TEXT(
			"A brick shed built from real-sized clay bricks: four running-bond walls with a door and a window "
			"under wooden lintels rise to stepped gables carrying a wooden gable roof, and a wooden porch stands "
			"over the door on two posts. It stands as laid, the whole 442-piece shed holding under its own weight "
			"— until you knock out the top (eaves) course of the back wall, and the entire back gable end above "
			"it comes down in a heap while the rest of the shed keeps standing.");

		ShedRealistic.LayStructure = [](DestructionLayout::FBrickLayout& OutLayout)
		{
			return DestructionShed3D::BuildRealistic(OutLayout);
		};

		ShedRealistic.Framing = EScenarioFraming::ThreeQuarter;

		/*
		 * The nine pieces of course 15 (odd: half bat, seven bricks, half bat) at
		 * Z = 15 * 7.5 + 3.25 = 115.75, on the back wall's Y centre 128.875.
		 */
		const double ShedRealisticEavesYCm = 128.875;
		const double ShedRealisticEavesZCm = 115.75;
		const double ShedRealisticEavesXsCm[] =
			{ 5.125, 22.0, 44.5, 67.0, 89.5, 112.0, 134.5, 157.0, 173.875 };

		for (const double EavesXCm : ShedRealisticEavesXsCm)
		{
			ShedRealistic.CutCentresCm.Add(
				FVector(EavesXCm, ShedRealisticEavesYCm, ShedRealisticEavesZCm));
		}

		/*
		 * The warehouse: the first data-driven level, loaded from Content/Layouts/Warehouse.json
		 * (Core/LayoutFile.h, written by Scripts/New-WarehouseLayout.ps1). 5,612 pieces, flagged
		 * 3D, router is the break authority. Owner ruling: buildings are data, not C++. Nothing
		 * is cut and no verdict is promised; it settles to whatever its joints carry.
		 */
		FScenario& Warehouse = Rows.AddDefaulted_GetRef();

		Warehouse.Name = FName(TEXT("warehouse"));
		Warehouse.MapName = TEXT("Lvl_Warehouse");
		Warehouse.Title = TEXT("The warehouse — a two-storey real-brick building, nothing cut");

		Warehouse.Expectation = TEXT(
			"A two-storey brick warehouse of real-sized clay bricks: pilastered walls with tall windows on "
			"both floors under wooden lintels, a stone plinth, string course and cornice, stepped gables under "
			"a stepped wooden roof, a doorway in the near end and two chimneys beside it. Nothing is cut: it "
			"is held as laid, then settles to whatever its joints can carry. Switch to Destroy and pull "
			"bricks out of it yourself.");

		Warehouse.LayStructure = [](DestructionLayout::FBrickLayout& OutLayout)
		{
			FString Why;
			const bool bLoaded = DestructionLayoutFile::LoadFile(
				DestructionLayoutFile::ContentPath(TEXT("Warehouse")), OutLayout, &Why);

			if (!bLoaded)
			{
				UE_LOG(LogTemp, Error, TEXT("the warehouse layout file could not be read: %s"), *Why);
			}

			return bLoaded;
		};

		Warehouse.Framing = EScenarioFraming::ThreeQuarter;

		return Rows;
	}

	/**
	 * The piece whose box is centred here, or INDEX_NONE. Compares all three axes, since near
	 * misses usually differ on one. A NaN centre matches nothing, so its build is refused.
	 */
	static int32 ScenariosPieceAtCentre(
		const DestructionLayout::FBrickLayout& Layout, const FVector& CentreCm)
	{
		for (int32 Piece = 0; Piece < Layout.Boxes.Num(); ++Piece)
		{
			if (Layout.Boxes[Piece].CentreCm.Equals(CentreCm, ScenariosCutMatchToleranceCm))
			{
				return Piece;
			}
		}

		return INDEX_NONE;
	}

	/**
	 * The bare asset name from a map name, package path, object path or PIE name
	 * (`UEDPIE_<n>_Lvl_X`). The PIE prefix is stripped case-insensitively;
	 * UWorld::RemovePIEPrefix is case-sensitive. All decoration and no map returns empty.
	 */
	static FString ScenariosBareMapName(const FString& MapName)
	{
		FString Bare = MapName;

		int32 At = INDEX_NONE;

		if (Bare.FindLastChar(TEXT('/'), At))
		{
			Bare.RightChopInline(At + 1);
		}

		if (Bare.FindChar(TEXT('.'), At))
		{
			Bare.LeftInline(At);
		}

		const FString PiePrefix = FString(PLAYWORLD_PACKAGE_PREFIX) + TEXT("_");

		if (Bare.StartsWith(PiePrefix, ESearchCase::IgnoreCase))
		{
			// Skip the instance number and its underscore.
			const FString AfterPrefix = Bare.RightChop(PiePrefix.Len());

			if (AfterPrefix.FindChar(TEXT('_'), At))
			{
				Bare = AfterPrefix.RightChop(At + 1);
			}
		}

		return Bare;
	}

	const TArray<FScenario>& Catalogue()
	{
		static const TArray<FScenario> Rows = ScenariosBuildCatalogue();
		return Rows;
	}

	int32 IndexOfName(FName Name)
	{
		const TArray<FScenario>& Rows = Catalogue();

		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			if (Rows[Index].Name == Name)
			{
				return Index;
			}
		}

		return INDEX_NONE;
	}

	int32 IndexOfMapName(const FString& MapName)
	{
		const TArray<FScenario>& Rows = Catalogue();

		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			// Case-insensitive: URLs and GetMapName do not guarantee case.
			if (MapName.Equals(FString(Rows[Index].MapName), ESearchCase::IgnoreCase))
			{
				return Index;
			}
		}

		return INDEX_NONE;
	}

	int32 IndexForOptionsAndMap(
		const FString& Options,
		const FString& MapName,
		EScenarioSelection& OutHow)
	{
		/*
		 * The option wins, parsed by the engine (key-compared, case-insensitive). HasOption is
		 * checked first because ParseOption returns empty both for an absent option and for
		 * `?Scenario=`, which is a typo and must not fall through to the map.
		 */
		if (UGameplayStatics::HasOption(Options, ScenariosOptionKey))
		{
			const int32 Named = IndexOfName(
				FName(*UGameplayStatics::ParseOption(Options, ScenariosOptionKey)));

			if (Catalogue().IsValidIndex(Named))
			{
				OutHow = EScenarioSelection::ByOption;

				return Named;
			}

			// Falls back, but reports it, so a mistyped name is distinguishable from the default.
			OutHow = EScenarioSelection::OptionNamedNoScenario;

			return ScenariosDefaultRow;
		}

		const int32 ByMap = IndexOfMapName(ScenariosBareMapName(MapName));

		if (Catalogue().IsValidIndex(ByMap))
		{
			OutHow = EScenarioSelection::ByMapName;

			return ByMap;
		}

		OutHow = EScenarioSelection::Default;

		return ScenariosDefaultRow;
	}

	bool Build(
		const FScenario& Scenario,
		DestructionLayout::FBrickLayout& OutLayout,
		TArray<int32>& OutCutPieces)
	{
		// Emptied first and filled last, so every refusal leaves the outputs empty.
		OutLayout = DestructionLayout::FBrickLayout();
		OutCutPieces.Reset();

		DestructionLayout::FBrickLayout Laid;

		const bool bLaid = Scenario.LayStructure
			? Scenario.LayStructure(Laid)
			: DestructionLayout::RunningBond(Scenario.Wall, Laid);

		if (!bLaid)
		{
			return false;
		}

		TArray<int32> CutPieces;
		CutPieces.Reserve(Scenario.CutCentresCm.Num());

		for (const FVector& CentreCm : Scenario.CutCentresCm)
		{
			const int32 Piece = ScenariosPieceAtCentre(Laid, CentreCm);

			// Refuse rather than drop: a silently skipped cut looks like a wall that stood.
			if (Piece == INDEX_NONE)
			{
				return false;
			}

			CutPieces.Add(Piece);
		}

		// Resolved, not applied: the caller removes the pieces after its on-screen delay.
		OutLayout = MoveTemp(Laid);
		OutCutPieces = MoveTemp(CutPieces);

		return true;
	}

	FViewpoint ViewpointFor(
		const FBox& BoundsCm, double AspectHeightOverWidth, EScenarioFraming Framing)
	{
		const FVector CentreCm = BoundsCm.GetCenter();
		const FVector HalfSizeCm = BoundsCm.GetExtent();

		/*
		 * Three-quarter frames the box's bounding sphere (radius = half-diagonal), since at an
		 * angle depth rotates into both extents. The floor is second in FMath::Max so a NaN
		 * radius lands on the floor.
		 */
		if (Framing == EScenarioFraming::ThreeQuarter)
		{
			const double RadiusCm = HalfSizeCm.Size();

			const double FromWidthCm = ScenariosFrameMargin * RadiusCm;
			const double FromHeightCm = (ScenariosFrameMargin * RadiusCm) / AspectHeightOverWidth;

			const double StandoffCm = FMath::Max(
				FMath::Max(FromWidthCm, FromHeightCm), ScenariosMinimumStandoffCm);

			// Centre-to-camera direction: +Y yawed toward +X by the azimuth, lifted by the elevation.
			const double AzimuthRad = FMath::DegreesToRadians(ScenariosThreeQuarterAzimuthDegrees);
			const double ElevationRad = FMath::DegreesToRadians(ScenariosThreeQuarterElevationDegrees);
			const double CosElevation = FMath::Cos(ElevationRad);

			const FVector ToCameraDir(
				CosElevation * FMath::Sin(AzimuthRad),
				CosElevation * FMath::Cos(AzimuthRad),
				FMath::Sin(ElevationRad));

			FViewpoint Angled;

			Angled.LocationCm = CentreCm + StandoffCm * ToCameraDir;
			Angled.Rotation = (CentreCm - Angled.LocationCm).Rotation();

			return Angled;
		}

		/*
		 * Multiply by the margin before dividing by aspect. The other order rounds wrong: a
		 * 1500 cm half-height frames to 1874.9999999999998 against 1875 asked, just outside.
		 */
		const double FromWidthCm = ScenariosFrameMargin * HalfSizeCm.X;
		const double FromHeightCm = (ScenariosFrameMargin * HalfSizeCm.Z) / AspectHeightOverWidth;

		/*
		 * Floor second, to fail closed: FMath::Max is `(B < A) ? A : B`, which discards a NaN
		 * in A but returns one in B. Empty or inverted boxes also land on the floor, instead of
		 * putting the camera behind the structure.
		 */
		const double StandoffCm = FMath::Max(
			FMath::Max(FromWidthCm, FromHeightCm), ScenariosMinimumStandoffCm);

		FViewpoint Viewpoint;

		Viewpoint.LocationCm = FVector(CentreCm.X, CentreCm.Y + StandoffCm, CentreCm.Z);
		Viewpoint.Rotation = FRotator(0.0, ScenariosCameraYawDegrees, 0.0);

		return Viewpoint;
	}
}
