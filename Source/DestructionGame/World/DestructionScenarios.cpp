// Copyright Epic Games, Inc. All Rights Reserved.

#include "World/DestructionScenarios.h"

#include "Core/Corbel.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Kismet/GameplayStatics.h"

/*
 * File-local names carry a Scenarios prefix and sit in the NAMED namespace rather than in an
 * anonymous one. An anonymous namespace is private to a TRANSLATION UNIT rather than to a file,
 * and a unity build merges many files into one — so two file-local names that collide are a hard
 * compile error between files that never refer to each other. See CURRENT_STATE.md.
 */
namespace DestructionScenarios
{
	/*
	 * --- the brick every row is laid from -------------------------------------------------
	 *
	 * DESIGN.md's standard UK metric clay brick, and the standard 1 cm mortar joint that makes
	 * the coordinating grid 22.5 x 11.25 x 7.5. Every row in the catalogue is this brick; what
	 * differs between them is how many of them there are and what comes out.
	 */
	constexpr double ScenariosBrickLengthCm = 21.5;
	constexpr double ScenariosBrickWidthCm = 10.25;
	constexpr double ScenariosBrickHeightCm = 6.5;
	constexpr double ScenariosMortarJointCm = 1.0;

	/**
	 * HOW NEAR A CUT CENTRE HAS TO BE TO A BRICK'S CENTRE TO NAME THAT BRICK, cm.
	 *
	 * A hundredth of a micron, and the size is chosen against the coordinating grid rather than
	 * tuned. The closest two DISTINCT brick centres ever come in a running-bond wall is half a
	 * cell — 11.25 cm along a course, 7.5 cm up it, and 5.625 cm from a flush half bat to the
	 * full brick beside it — so anything below a millimetre already makes a wrong match
	 * impossible, and this is seven orders below that. It is still ten orders ABOVE the grid's
	 * own rounding: a brick centre at the far end of the scenario wall is 663 cm out, where one
	 * ulp is about 1e-13 cm. So there is a very wide band between "cannot mismatch" and "cannot
	 * miss its own brick", and this sits in the middle of it rather than at either edge.
	 */
	constexpr double ScenariosCutMatchToleranceCm = 1.0e-6;

	/**
	 * WHAT A URL NAMES A SCENARIO WITH, and it is a KEY handed to the engine's own parser
	 * rather than a substring anybody searches for.
	 *
	 * `?MyScenario=`, `?Scenarios=` and `?ScenarioX=` all CONTAIN this word, so a
	 * `Contains(TEXT("Scenario="))` matches three keys that are not this one and gives whoever
	 * typed them a wall they did not ask for. UGameplayStatics::ParseOption splits the URL into
	 * key/value pairs and compares the KEY, which is the whole reason it is used here.
	 */
	const TCHAR* const ScenariosOptionKey = TEXT("Scenario");

	/**
	 * THE ROW EVERY MISS FALLS BACK TO — the first one, which ScenariosBuildCatalogue makes
	 * `sandbox`. A level showing an empty world is a worse failure than one showing this.
	 */
	constexpr int32 ScenariosDefaultRow = 0;

	/*
	 * --- where the player stands ----------------------------------------------------------
	 *
	 * UCameraComponent's default field of view is 90 degrees HORIZONTALLY, so at a standoff s
	 * the visible half-width is s exactly and the visible half-height is s * aspect, where the
	 * aspect is the viewport's height over its width. Framing a bounding box therefore needs
	 * s >= halfX and s >= halfZ / aspect, and the margin is what keeps the structure off the
	 * edges of the frame.
	 */
	constexpr double ScenariosFrameMargin = 1.25;

	/** Nothing is framed closer than this, or a four-brick arm fills the screen with one brick. */
	constexpr double ScenariosMinimumStandoffCm = 120.0;

	/**
	 * THE CAMERA LOOKS ALONG -Y, AND THE REASON IS LEGIBILITY RATHER THAN TASTE.
	 *
	 * At yaw +90 the view direction is +Y and the camera's right vector is -X, so increasing X
	 * is drawn to the LEFT and every structure comes out MIRRORED against
	 * claude_plans/CORBEL_CASES.html and every elevation in the design documents. The picture is
	 * not obviously broken, which is what makes it worth pinning. Yaw -90 puts the view along -Y
	 * with +X to the right, so a level reads the same way round as the drawings.
	 */
	constexpr double ScenariosCameraYawDegrees = -90.0;

	/**
	 * The wall every row is laid from, at whatever size that row wants.
	 *
	 * FLUSH RATHER THAN RAGGED, and it is not a style choice: a ragged wall's alternate courses
	 * step in, so the end brick of every even course rests on ONE brick below it and is already
	 * a half-seated cantilever before anything has been cut. A flush end fills that half cell
	 * with a half bat, every brick has two seats, and an intact wall's eccentricity is exactly
	 * zero — which is the only baseline against which "one deletion did this" means anything.
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
	 * --- the corbel family -----------------------------------------------------------------
	 *
	 * Three courses of immovable base under every case, and the arm's outer face advances half a
	 * coordinating cell per course — 11.25 cm, which is what every corbel reading in this project
	 * was taken on and 3.46x the per-course projection published corbelling practice allows. The
	 * counterweight cases shift their left origin by exactly the three cells the base gains, so
	 * their root joint sits at the same absolute X as the bare-base cases' and the two structures
	 * differ in the masonry standing opposite and in nothing else.
	 */
	constexpr int32 ScenariosCorbelBaseCourses = 3;

	constexpr double ScenariosCorbelCellPitchCm = ScenariosBrickLengthCm + ScenariosMortarJointCm;
	constexpr double ScenariosCorbelStepCm = ScenariosCorbelCellPitchCm / 2.0;

	constexpr double ScenariosCorbelCounterweightOriginCm = -3.0 * ScenariosCorbelCellPitchCm;

	/** One row of the corbel family, as data: everything that differs between the seven. */
	struct FScenariosCorbelRow
	{
		const TCHAR* Name;
		const TCHAR* MapName;
		const TCHAR* Title;
		const TCHAR* Expectation;

		/** Cells wide the base is: two bare, five when three of them stand opposite. */
		int32 BaseCells;

		/** Left-hand edge of the base, cm. Written out per row rather than inferred from the cells. */
		double LeftOriginCm;

		int32 Steps;

		/** False is the bare stepped arm of single bricks — case A, and only case A. */
		bool bFilled;
	};

	/**
	 * THE SEVEN, AND NONE OF THEM CUTS ANYTHING.
	 *
	 * `free-end-40` is BEFORE AND AFTER A CUT: the wall stands, a brick goes, and the question is
	 * what the remainder does about it. A CORBEL IS CONDEMNED BY ITS OWN GEOMETRY — it is laid
	 * reaching too far and its root joint is over capacity the moment it exists — so the honest
	 * pair is AS LAID versus SETTLED, there is no cut to wait for, and nothing for a delay to do.
	 *
	 * E35 AND E36 EXIST AS A PAIR because `Core.Structure.CorbelStepsBeforeTensionWins` locates
	 * the crossover at 36 steps: the smallest step count whose root joint reads over 1.0, with the
	 * step below at or under capacity. One row could not show a tipping point.
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
			TEXT("Corbel E35 — the last one the model says stands"),
			TEXT("The step BELOW the crossover: the last corbel the model says stands. Nothing is "
				"cut, and nothing should move."),
			5, ScenariosCorbelCounterweightOriginCm, 35, true },

		{ TEXT("corbel-e36"), TEXT("Lvl_CorbelE36"),
			TEXT("Corbel E36 — the first one that does not"),
			TEXT("The crossover itself: the first corbel whose root joint reads over 1.0. Nothing "
				"is cut — one more course than E35 is the entire difference."),
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

	/** Every row, built once. */
	static TArray<FScenario> ScenariosBuildCatalogue()
	{
		TArray<FScenario> Rows;

		/*
		 * THE WALL PLAY ALREADY GIVES YOU, and it is in the catalogue so that naming a level
		 * stops being a different thing from starting the game. 30 bricks across, 40 courses,
		 * and nothing is taken out of it.
		 */
		FScenario& Sandbox = Rows.AddDefaulted_GetRef();

		Sandbox.Name = FName(TEXT("sandbox"));
		Sandbox.MapName = TEXT("Lvl_Sandbox");
		Sandbox.Title = TEXT("Sandbox — the wall Play has always given you");

		Sandbox.Expectation = TEXT(
			"Nothing is cut: six and a half metres of wall stands there, and goes on standing "
			"until you pull a brick out of it yourself.");

		Sandbox.Wall = ScenariosWallSpec(40, 30);

		/*
		 * THE USER'S OWN REPORTED CASE: one brick deleted at a free end, under forty courses of
		 * wall. It is the fixture Core.Structure.AFreeEndDeletionInATallWall reads, so the level
		 * and the number that test prints are about one wall rather than two that look alike.
		 *
		 * THE POINT OF THE LEVEL IS WATCHING IT NOT HAPPEN. The brick above the hole keeps one
		 * seat and carries across it, so a human joining this map should see the cut and then
		 * see nothing else move at all.
		 */
		FScenario& FreeEnd = Rows.AddDefaulted_GetRef();

		FreeEnd.Name = FName(TEXT("free-end-40"));
		FreeEnd.MapName = TEXT("Lvl_FreeEnd40");
		FreeEnd.Title = TEXT("One brick out of a free end, under forty courses");

		FreeEnd.Expectation = TEXT(
			"The outermost brick of the bottom course goes — and the wall must NOT come down. "
			"The brick above it keeps one seat and carries; anything else falling is a bug.");

		FreeEnd.Wall = ScenariosWallSpec(40, 7);

		/*
		 * THE OUTERMOST FULL BRICK OF THE GROUNDED COURSE. Course 0 is an even course, so its
		 * bricks run from x = 0 at one brick pitch each and the outermost of them is at x = 0
		 * exactly; its centre sits half a brick above the ground, and the wall is one brick
		 * thick and centred on y = 0.
		 */
		FreeEnd.CutCentresCm.Add(FVector(0.0, 0.0, ScenariosBrickHeightCm / 2.0));

		/*
		 * AND THE CORBEL FAMILY, ONE ROW PER STRUCTURE THE USER REVIEWED. Each carries the call
		 * that lays it, so the loop below knows nothing about corbels beyond the table above it.
		 * None of them names a cut: a corbel is condemned by its own geometry and the level's
		 * whole story is as-laid versus settled.
		 */
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

		return Rows;
	}

	/**
	 * The piece whose box is centred here, or INDEX_NONE.
	 *
	 * A GENUINE THREE-COMPONENT PROXIMITY TEST, because every plausible near miss differs from a
	 * real brick on ONE axis only: the head-joint gap between two bricks of a course is out in X
	 * alone, the bed-joint plane between two courses is out in Z alone, and a centre a metre in
	 * front of the wall is out in Y alone. Any of them compared on two axes matches a brick that
	 * is not there.
	 *
	 * AND A NaN CENTRE MATCHES NOTHING, which is the fail-closed direction here: FVector::Equals
	 * is three `FMath::Abs(difference) <= Tolerance` tests, every comparison against a NaN is
	 * false, so a NaN falls out as a miss and the build that asked for it is refused.
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
	 * THE BARE ASSET NAME UNDERNEATH EVERY WAY A MAP ARRIVES.
	 *
	 * `UWorld::GetMapName()` answers `Lvl_FreeEnd40` in a cooked game and
	 * `UEDPIE_<instance>_Lvl_FreeEnd40` in PIE, while a URL or a streamed level hands over a
	 * package path (`/Game/Maps/Lvl_FreeEnd40`) or a full object path
	 * (`/Game/Maps/Lvl_FreeEnd40.Lvl_FreeEnd40`). All of them are the same map, and a catalogue
	 * that matched only one of them would work perfectly for whoever wrote it and silently give
	 * the wrong wall to everyone else.
	 *
	 * THE PIE PREFIX IS STRIPPED CASE-INSENSITIVELY, which is why `UWorld::RemovePIEPrefix` is
	 * not used: it searches CaseSensitive, and neither a URL nor a config file guarantees the
	 * case anything was typed in. The word itself is the engine's own PLAYWORLD_PACKAGE_PREFIX
	 * rather than a second copy of it.
	 *
	 * A NAME THAT IS ALL DECORATION AND NO MAP — `UEDPIE_0_`, or a path ending in a slash —
	 * comes back EMPTY, which no row's map name is, so it selects nothing rather than the
	 * nearest thing to it.
	 */
	static FString ScenariosBareMapName(const FString& MapName)
	{
		FString Bare = MapName;

		int32 At = INDEX_NONE;

		/* A package path, and then the `.AssetName` an object path adds after it. */
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
			/* `UEDPIE_11_Lvl_X`: past the word, then past the instance number's own underscore. */
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
			/*
			 * CASE-INSENSITIVELY, because a map name arrives from a URL or from
			 * UWorld::GetMapName and neither guarantees the case the catalogue was typed in.
			 */
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
		 * THE OPTION WINS, AND IT IS READ WITH THE ENGINE'S OWN PARSER. HasOption and
		 * ParseOption walk the `?Key=Value` pairs and compare the KEY, so a decoy key that
		 * merely contains the word is not this option, the key and the value are both matched
		 * case-insensitively for free, and a repeated option resolves the way every other
		 * option on the same URL resolves. A parser written here would be a second answer to a
		 * question the engine already answers, and the first thing to go wrong with two answers
		 * is a level that disagrees with the URL it was launched from.
		 *
		 * ASKED AND THEN READ, RATHER THAN READ AND TESTED FOR EMPTINESS. `?Scenario=` with
		 * nothing after it is a typo, not an absent option, and ParseOption answers an empty
		 * string to both — so an implementation that only looked at the value would fall
		 * through to the map and quietly build whatever that names.
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

			/*
			 * AND A NAME THAT IS NOT IN THE CATALOGUE FALLS BACK WHILE SAYING SO. The index
			 * alone is identical to the deliberate default below it, which leaves a player who
			 * mistyped staring at the wrong wall with nothing to tell them why.
			 */
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
		/*
		 * EMPTIED FIRST AND FILLED LAST, so that every refusal below leaves a caller who ignored
		 * the return value with nothing rather than with a wall that looks buildable and a cut
		 * list left over from whatever it did last. The wall is laid into a local and moved out
		 * at the end for the same reason: a cut that names no brick is only discovered after
		 * 1,220 pieces have been placed.
		 */
		OutLayout = DestructionLayout::FBrickLayout();
		OutCutPieces.Reset();

		DestructionLayout::FBrickLayout Laid;

		/*
		 * THE ROW SAYS WHAT LAYS IT, and a row that says nothing is a running bond. Neither branch
		 * knows what kind of structure it is producing — the corbel rows carry a call to
		 * DestructionCorbel::Build, so adding a family adds rows rather than a case here.
		 */
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

			/*
			 * A CUT CENTRE THAT NAMES NO BRICK REFUSES THE WHOLE BUILD. Dropping it instead
			 * would give a level that looks intact, never does anything, and reads EXACTLY like
			 * a level whose wall correctly stood — which is the one distinction these levels
			 * exist to let a human make.
			 */
			if (Piece == INDEX_NONE)
			{
				return false;
			}

			CutPieces.Add(Piece);
		}

		/*
		 * THE CUT IS RESOLVED AND NOT APPLIED. Nothing is removed here: the player watches the
		 * brick go, so the removal belongs to whoever owns the delay.
		 */
		OutLayout = MoveTemp(Laid);
		OutCutPieces = MoveTemp(CutPieces);

		return true;
	}

	FViewpoint ViewpointFor(const FBox& BoundsCm, double AspectHeightOverWidth)
	{
		const FVector CentreCm = BoundsCm.GetCenter();
		const FVector HalfSizeCm = BoundsCm.GetExtent();

		/*
		 * THE MARGIN IS APPLIED TO EACH REQUIREMENT AND THE DIVISION COMES LAST, WHICH IS NOT
		 * THE SAME ARITHMETIC AS SCALING ONE MAXIMUM. Algebraically 1.25 * (halfZ / aspect) and
		 * (1.25 * halfZ) / aspect are one number; in doubles they are not, and the difference
		 * lands on the wrong side of the inequality this whole function exists to satisfy. A
		 * 1500 cm half-height framed the first way gives a standoff of 3333.333333333333, whose
		 * visible half-height is 1874.9999999999998 against the 1875 the margin asked for — so
		 * the structure is a fifth of a nanometre outside the frame it was placed to be inside.
		 * Written this way each requirement is one operation away from its own inequality, and
		 * every swept case in Scenarios.Viewpoint clears it.
		 */
		const double FromWidthCm = ScenariosFrameMargin * HalfSizeCm.X;
		const double FromHeightCm = (ScenariosFrameMargin * HalfSizeCm.Z) / AspectHeightOverWidth;

		/*
		 * THE FLOOR IS THE SECOND ARGUMENT, AND THAT ORDER IS THE FAIL-CLOSED ONE. FMath::Max is
		 * (A >= B) ? A : B, so it DISCARDS a NaN in B and RETURNS one in A — with the floor
		 * first, a bounding box that produced a NaN standoff would hand back a NaN camera
		 * position instead of the floor. Written this way round the floor is what survives.
		 *
		 * An EMPTY box has zero extents and an INVERTED one has negative extents, and both land
		 * on the floor by the same comparison. A negative standoff would put the camera behind
		 * the structure looking away from it, which renders perfectly and shows nothing at all.
		 */
		const double StandoffCm = FMath::Max(
			FMath::Max(FromWidthCm, FromHeightCm), ScenariosMinimumStandoffCm);

		FViewpoint Viewpoint;

		Viewpoint.LocationCm = FVector(CentreCm.X, CentreCm.Y + StandoffCm, CentreCm.Z);
		Viewpoint.Rotation = FRotator(0.0, ScenariosCameraYawDegrees, 0.0);

		return Viewpoint;
	}
}
