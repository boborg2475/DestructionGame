// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/BuildMode/Placement.h"
#include "Core/Layout.h"
#include "Core/PieceMenu.h"
#include "Core/SessionToolbar.h"
#include "Core/StructureBinding.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Uniquely named, not anonymous: a unity build merges translation units, so file-local names can
 * collide. The `using namespace` stays inside RunTest for the same reason.
 */
namespace CourseNumberingTestSupport
{
	/** The structure id this fixture uses; any id will do. */
	constexpr int32 CourseNumberingStructure = 11;

	/**
	 * The first integer after the word "course" (case-insensitive), or INDEX_NONE. Compares only the
	 * number, since "Course 1" and "course 1 · #2" differ in wording. Fails closed to INDEX_NONE, not
	 * zero, so two unparseable strings cannot agree.
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

		// Only whitespace may separate the word from its number.
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
 * The toolbar and the piece menu must name a brick's course by the same number. The toolbar once
 * printed a zero-based subscript ("Course 0") while the menu counted from one; the ruling moved the
 * toolbar to one-based.
 *
 * Asserts agreement, not a literal: each presenter's own test pins its wording. The brick is laid
 * through CoursePlaneZCm and BuildMode::PlacePiece rather than at a hand-written Z. No world needed.
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

	// Course 0 is the grounded one, asked of the model rather than assumed.
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

	// A second brick on the same plane, so a presenter printing position instead of course cannot pass by coincidence.
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

	// Both on one course, checked from geometry: the snap solver could pull the second brick up a course.
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

	// Both must parse first, or two unparseable labels would compare equal.
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
