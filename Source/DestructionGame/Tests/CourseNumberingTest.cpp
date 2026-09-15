// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/BuildMode/Placement.h"
#include "Core/Layout.h"
#include "Core/PieceMenu.h"
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
namespace CourseNumberingTestSupport
{
	/** The structure this fixture identifies itself as. Any id will do; it just has to be one. */
	constexpr int32 CourseNumberingStructure = 11;

	/**
	 * The first integer printed after the word "course", or INDEX_NONE if there is not one.
	 *
	 * CASE-INSENSITIVE AND WORD-ANCHORED, WHICH IS WHAT MAKES THIS A COMPARISON OF CONVENTIONS
	 * RATHER THAN OF WORDING. The two surfaces are allowed to read differently — the strip is a
	 * control and says "Course 1", the entry row is one line of a list and says "course 1 · #2" —
	 * and demanding one be a prefix of the other would pin their CAPITALISATION and their
	 * punctuation, neither of which anything here has an opinion about. What may not differ is the
	 * number, so the number is what is lifted out of each.
	 *
	 * IT FAILS CLOSED TO INDEX_NONE RATHER THAN TO ZERO, and the callers assert both readings
	 * parsed before comparing them. Zero would make two unparseable strings agree with each other,
	 * which is precisely the shape of a test that stops asserting anything the day a label is
	 * retuned — and "brick 11:0", the presenter's own fallback label for a ref it cannot place, is
	 * a string this would otherwise have to have an opinion about.
	 */
	int32 CourseNumberIn(const FString& Text)
	{
		const FString Lower = Text.ToLower();
		const int32 WordAt = Lower.Find(TEXT("course"));

		if (WordAt == INDEX_NONE)
		{
			return INDEX_NONE;
		}

		int32 At = WordAt + 6;

		/* Whatever separates the word from its number may be spaces and nothing else. */
		while (At < Lower.Len() && FChar::IsWhitespace(Lower[At]))
		{
			++At;
		}

		if (At >= Lower.Len() || !FChar::IsDigit(Lower[At]))
		{
			return INDEX_NONE;
		}

		int32 Number = 0;

		while (At < Lower.Len() && FChar::IsDigit(Lower[At]))
		{
			Number = Number * 10 + (Lower[At] - TEXT('0'));
			++At;
		}

		return Number;
	}
}

/**
 * THE TWO SURFACES THAT NAME A COURSE MUST NAME IT THE SAME.
 *
 * WHY THIS TEST EXISTS, IN ONE SENTENCE FROM THE PROOF FRAMES: the committed session screenshots
 * show a player laying a brick while the toolbar reads "Course 0", and the details window on that
 * same brick a moment later calling it "course 1 · #1". Both readouts are correct about their own
 * convention — the toolbar was printing FSessionToolbarState::Course, an array subscript, and
 * Core/PieceMenu.cpp has counted courses of brick from one since it was written ("BOTH NUMBERS
 * COUNT FROM ONE") — and that is exactly what made it survive: neither presenter's own test could
 * see the other, so each swept its own convention and passed. CURRENT_STATE records it as finding
 * P1; the owner-delegated ruling is that the TOOLBAR moves to one-based.
 *
 * SO THE ASSERTION IS AGREEMENT, NOT A LITERAL. Core.SessionToolbar.CourseGroundingAndLabel pins
 * what the strip reads and Presenter.PieceMenuInspector pins what an entry row reads; this pins
 * the one claim neither of them can make alone, and it goes on holding if both wordings are
 * retuned together. A test here that spelled "Course 1" would be a third place the convention is
 * written down and the first to rot.
 *
 * THE BRICK IS LAID THROUGH THE REAL SEAM RATHER THAN POSITIONED BY HAND. Its Z comes from
 * DestructionSession::CoursePlaneZCm(0, half height) — the toolbar's own answer for "where does
 * course 0 put a brick" — and it is placed by BuildMode::PlacePiece, the build path. That is what
 * makes the two ends of the comparison genuinely the same course: a hand-written Z would be this
 * test's opinion about where course 0 is, and the presenter would be labelling a course the
 * toolbar never claimed.
 *
 * NEEDS A TICKING WORLD: no. Nothing is spawned, nothing is solved and nothing moves — a course
 * number is a fact about where a box was laid, and both presenters are pure functions over a
 * binding. It never calls SolveLoads: a label is drawn before anything has been solved, and
 * a solve would only add a second reason for the fixture to change.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCourseNumberingAgreesTest,
	"DestructionGame.Presenter.CourseNumberingAgrees",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FCourseNumberingAgreesTest::RunTest(const FString& Parameters)
{
	using namespace CourseNumberingTestSupport;
	using namespace DestructionLayout;
	using namespace DestructionSession;

	const FVector HalfBrickCm = BuildPieceHalfExtentCm(EBuildPieceKind::Brick);

	/*
	 * COURSE 0 IS THE GROUNDED ONE, ASKED OF THE MODEL RATHER THAN ASSUMED. If the grounded course
	 * ever stops being index 0 this fixture is laying on the wrong plane, and it would go on
	 * comparing two numbers that agree about a course nobody is standing on.
	 */
	TestTrue(
		TEXT("fixture: index 0 must be the course with the earth under it"),
		IsCourseGrounded(0));

	const double CourseZeroZCm = CoursePlaneZCm(0, HalfBrickCm.Z);

	FBrickLayout Layout;
	const BuildMode::FSnapSettings Settings;

	const BuildMode::FPlacementResult First = BuildMode::PlacePiece(
		Layout,
		FVector(0.0, 0.0, CourseZeroZCm),
		HalfBrickCm,
		BuildPieceMaterial(EBuildPieceKind::Brick),
		/*bGrounded*/ true,
		Settings);

	/*
	 * A SECOND BRICK BESIDE IT, ON THE SAME PLANE. One brick would exercise only "#1", and the
	 * label's two numbers are composed together — a presenter that printed the position where the
	 * course goes would read "course 1" for a single brick by coincidence.
	 */
	const BuildMode::FPlacementResult Second = BuildMode::PlacePiece(
		Layout,
		FVector(22.5, 0.0, CourseZeroZCm),
		HalfBrickCm,
		BuildPieceMaterial(EBuildPieceKind::Brick),
		/*bGrounded*/ true,
		Settings);

	TestTrue(
		FString::Printf(
			TEXT("fixture: both bricks must land as live pieces, the placer returned handles %d and %d"),
			First.PieceHandle, Second.PieceHandle),
		First.PieceHandle == 0 && Second.PieceHandle == 1);

	if (First.PieceHandle != 0 || Second.PieceHandle != 1)
	{
		return true;
	}

	/*
	 * AND BOTH MUST BE ON ONE COURSE, ASKED OF THE GEOMETRY. The snap solver ranks by distance and
	 * a cursor 22.5 cm along can be pulled up a course; if it ever were, the entry labels would
	 * read two different courses and this test would be comparing the toolbar's course 0 against
	 * whatever the second brick happened to land on.
	 */
	TestEqual(
		FString::Printf(
			TEXT("fixture: both bricks must sit on the course-0 plane at Z %g, they sit at %g and %g"),
			CourseZeroZCm, Layout.Boxes[0].CentreCm.Z, Layout.Boxes[1].CentreCm.Z),
		Layout.Boxes[1].CentreCm.Z, Layout.Boxes[0].CentreCm.Z);

	TArray<UObject*> NoActors;
	NoActors.SetNumZeroed(Layout.Structure.NumPieces());

	FStructureBinding Binding;
	Binding.StructureId = CourseNumberingStructure;

	TestTrue(
		TEXT("fixture: the laid layout should be adopted into a binding the presenter can read"),
		AdoptLayout(Layout, NoActors, Binding));

	FPieceRef GroundBrick;
	GroundBrick.StructureId = CourseNumberingStructure;
	GroundBrick.PieceIndex = 0;

	FPieceRef SecondBrick;
	SecondBrick.StructureId = CourseNumberingStructure;
	SecondBrick.PieceIndex = 1;

	const TArray<FPieceRef> Selected = { GroundBrick, SecondBrick };

	const FPieceMenuInspector Inspector =
		BuildPieceMenuInspector(Binding, Selected, GroundBrick);

	if (Inspector.Pieces.Num() != 2)
	{
		AddError(FString::Printf(
			TEXT("fixture: two picked bricks should present two entries, they present %d"),
			Inspector.Pieces.Num()));

		return true;
	}

	const FString ToolbarWord = CourseLabel(0);
	const FString MenuWord = Inspector.Pieces[0].Label;

	AddInfo(FString::Printf(
		TEXT("the strip reads '%s'; the details window calls the brick laid on that plane '%s' (and its neighbour '%s')"),
		*ToolbarWord, *MenuWord, *Inspector.Pieces[1].Label));

	const int32 ToolbarCourse = CourseNumberIn(ToolbarWord);
	const int32 MenuCourse = CourseNumberIn(MenuWord);

	/*
	 * BOTH READINGS MUST HAVE PARSED BEFORE THEY ARE COMPARED. Two unparseable labels are equal to
	 * each other, so without these rows a presenter that stopped naming courses at all would make
	 * this test greener than it is now.
	 */
	TestTrue(
		*FString::Printf(
			TEXT("the toolbar's course readout must name a course: it reads '%s'"), *ToolbarWord),
		ToolbarCourse != INDEX_NONE);

	TestTrue(
		*FString::Printf(
			TEXT("the piece menu's entry must name the brick's course: it reads '%s'"), *MenuWord),
		MenuCourse != INDEX_NONE);

	if (ToolbarCourse == INDEX_NONE || MenuCourse == INDEX_NONE)
	{
		return true;
	}

	TestEqual(
		FString::Printf(
			TEXT("ONE CONVENTION, TWO SURFACES: a brick laid on the plane the strip calls '%s' must be "
				 "named by the same course number in the details window, which calls it '%s' — %d vs %d"),
			*ToolbarWord, *MenuWord, ToolbarCourse, MenuCourse),
		MenuCourse, ToolbarCourse);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
