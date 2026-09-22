// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/PieceMenu.h"
#include "Core/SessionToolbar.h"
#include "Core/StructureBinding.h"
#include "DestructionGamePlayerController.h"
#include "Tests/BrickWorldTestSupport.h"
#include "World/BrickActor.h"
#include "World/BuildModeComponent.h"
#include "World/DestructionStructureSubsystem.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Session S12, the load overlay. With the Destroy strip's `Load overlay` chip on, every live piece
 * wears the highlight of its worst joint's margin band. The overlay is the weakest state in
 * `HighlightForPiece`'s precedence, is recomputed after any structural change, clears when toggled
 * off, and its solve leaves every connection intact.
 *
 * The overlay is an input to the precedence, not its own SetHighlighted: it covers every piece, so
 * painting directly would overwrite the hover and selection a player checks before pressing Delete.
 *
 * Assertions read mechanism (`GetHighlight()`, toolbar flags, `HasGiven`, `IsPieceRemoved`), never
 * colour or displacement. The expected state is composed from
 * `BrickHighlightForLoadBand(WorstJointBandForPiece(...))` rather than a literal (bands are pinned in
 * `Core.LoadOverlay.*`), plus a floor of at least one Comfortable piece so an all-None stub fails.
 *
 * Needs a world (real bricks, subsystem, line trace) but never ticks it. Named namespace because unity
 * builds merge anonymous ones.
 */
namespace SessionLoadOverlayTestSupport
{
	using namespace DestructionSession;

	/*
	 * Course centre heights, written out rather than taken from CoursePlaneZCm so the test is
	 * independent of it (DESIGN §8). Brick 21.5 x 10.25 x 6.5 cm on a 1 cm joint: grid 22.5 across,
	 * 7.5 up; course 0 centres at 3.25.
	 */
	constexpr double OverlayBrickPlaneCourse0Cm = 3.25;
	constexpr double OverlayBrickPlaneCourse1Cm = 10.75;

	/**
	 * Running-bond wall: three on the ground, two straddling them (as in `Visual.SessionScreenshots`).
	 * Middle pieces have several joints, so a first-joint-wins overlay would fail.
	 */
	constexpr double OverlayCourse0CursorsXCm[] = { 0.0, 22.5, 45.0 };
	constexpr double OverlayCourse1CursorsXCm[] = { 11.25, 33.75 };

	/** The middle brick of the bottom course, which every ray here targets. */
	constexpr double OverlayMiddleBottomXCm = 22.5;

	constexpr int32 OverlayWallPieceCount = 5;

	/**
	 * Laying ray endpoints. The ray is a direction intersected with the build plane, so the end goes
	 * to Z = 0 rather than the plane; aiming at the plane would pass a controller that ignored it.
	 */
	constexpr double OverlayLayRayStartZCm = 300.0;
	constexpr double OverlayLayRayEndZCm = 0.0;

	FVector OverlayLayRayStart(double XCm)
	{
		return FVector(XCm, 0.0, OverlayLayRayStartZCm);
	}

	FVector OverlayLayRayEnd(double XCm)
	{
		return FVector(XCm, 0.0, OverlayLayRayEndZCm);
	}

	/** Destroy ray half-length along Y (clear of other bricks), well past the 10.25 cm depth. */
	constexpr double OverlayInspectReachCm = 100.0;

	FVector OverlayInspectStart(const FVector& CentreCm)
	{
		return FVector(CentreCm.X, CentreCm.Y - OverlayInspectReachCm, CentreCm.Z);
	}

	FVector OverlayInspectEnd(const FVector& CentreCm)
	{
		return FVector(CentreCm.X, CentreCm.Y + OverlayInspectReachCm, CentreCm.Z);
	}

	/** A ray 100 m away that hits nothing, to release the hover. */
	FVector OverlayEmptyRayStart()
	{
		return FVector(-10000.0, -10000.0, 500.0);
	}

	FVector OverlayEmptyRayEnd()
	{
		return FVector(-10000.0, -10000.0, -500.0);
	}

	const TCHAR* OverlayHighlightName(EBrickHighlight Highlight)
	{
		switch (Highlight)
		{
		case EBrickHighlight::None:            return TEXT("None");
		case EBrickHighlight::Hovered:         return TEXT("Hovered");
		case EBrickHighlight::Selected:        return TEXT("Selected");
		case EBrickHighlight::Inspected:       return TEXT("Inspected");
		case EBrickHighlight::Neighbour0:      return TEXT("Neighbour0");
		case EBrickHighlight::Neighbour1:      return TEXT("Neighbour1");
		case EBrickHighlight::Neighbour2:      return TEXT("Neighbour2");
		case EBrickHighlight::Neighbour3:      return TEXT("Neighbour3");
		case EBrickHighlight::Neighbour4:      return TEXT("Neighbour4");
		case EBrickHighlight::Neighbour5:      return TEXT("Neighbour5");
		case EBrickHighlight::LoadComfortable: return TEXT("LoadComfortable");
		case EBrickHighlight::LoadCaution:     return TEXT("LoadCaution");
		case EBrickHighlight::LoadCritical:    return TEXT("LoadCritical");
		}

		return TEXT("<a highlight this build has never heard of>");
	}

	const TCHAR* OverlayBandName(EJointMarginBand Band)
	{
		switch (Band)
		{
		case EJointMarginBand::Comfortable: return TEXT("Comfortable");
		case EJointMarginBand::Caution:     return TEXT("Caution");
		case EJointMarginBand::Critical:    return TEXT("Critical");
		}

		return TEXT("<a band this build has never heard of>");
	}

	bool OverlayIsALoadState(EBrickHighlight Highlight)
	{
		return Highlight == EBrickHighlight::LoadComfortable
			|| Highlight == EBrickHighlight::LoadCaution
			|| Highlight == EBrickHighlight::LoadCritical;
	}

	/** Handles of every live (non-removed) piece. */
	TArray<int32> OverlayLivePieces(const FStructureBinding& Binding)
	{
		TArray<int32> Live;

		for (int32 Index = 0; Index < Binding.NumPieces(); ++Index)
		{
			if (!Binding.IsPieceRemoved(Index))
			{
				Live.Add(Index);
			}
		}

		return Live;
	}

	/** Every live piece's highlight on one line, for failure messages. */
	FString OverlayDescribeWall(const FStructureBinding& Binding)
	{
		FString Line;

		for (const int32 Piece : OverlayLivePieces(Binding))
		{
			const ABrickActor* const Brick = Cast<ABrickActor>(Binding.GetActor(Piece));

			Line += FString::Printf(
				TEXT("%s%d:%s"),
				Line.IsEmpty() ? TEXT("") : TEXT(", "),
				Piece,
				Brick != nullptr ? OverlayHighlightName(Brick->GetHighlight()) : TEXT("<no actor>"));
		}

		return Line.IsEmpty() ? TEXT("<no live pieces>") : Line;
	}

	/**
	 * A controller with a real ULocalPlayer, and its build component. The local player is needed:
	 * session mapping contexts go through the Enhanced Input local player subsystem. Copied from
	 * `SessionControllerTest.cpp`.
	 */
	struct FOverlayFixture
	{
		BrickWorldTestSupport::FBrickTestWorld TestWorld;
		ADestructionGamePlayerController* Controller = nullptr;
		UBuildModeComponent* Build = nullptr;
		bool bWorldBegun = false;

		bool Begin(FAutomationTestBase& Test)
		{
			if (!TestWorld.Begin(Test))
			{
				return false;
			}

			bWorldBegun = true;

			Controller = BrickWorldTestSupport::SpawnControllerWithLocalPlayer(Test, TestWorld.World);

			if (Controller == nullptr)
			{
				return false;
			}

			Build = Controller->GetBuildComponent();

			Test.TestNotNull(
				TEXT("fixture: a spawned controller must already carry its UBuildModeComponent"),
				Build);

			return Build != nullptr;
		}

		void End()
		{
			if (bWorldBegun)
			{
				TestWorld.End();
				bWorldBegun = false;
			}
		}
	};

	/**
	 * Lay the five-brick bond through player clicks (`PrimaryAlongRay`, not `AddPiece`) and leave the
	 * session in Destroy mode.
	 *
	 * @return the binding, or null with the reason already reported.
	 */
	FStructureBinding* OverlayLayTheWall(FAutomationTestBase& Test, FOverlayFixture& Fixture)
	{
		ADestructionGamePlayerController& Controller = *Fixture.Controller;

		if (!Controller.OnToolbarButton(EToolbarButtonId::ModeBuild))
		{
			Test.AddError(TEXT("fixture: the Build tab must be clickable for any of this to run"));
			return nullptr;
		}

		const int32 StructureId = Fixture.Build->GetStructureId();

		for (const double XCm : OverlayCourse0CursorsXCm)
		{
			if (!Controller.PrimaryAlongRay(OverlayLayRayStart(XCm), OverlayLayRayEnd(XCm)))
			{
				Test.AddError(FString::Printf(
					TEXT("fixture: the course-0 click at x = %g must lay a brick"), XCm));

				return nullptr;
			}
		}

		if (!Controller.OnToolbarButton(EToolbarButtonId::CourseUp))
		{
			Test.AddError(TEXT("fixture: Course up is always live in Build mode"));
			return nullptr;
		}

		for (const double XCm : OverlayCourse1CursorsXCm)
		{
			if (!Controller.PrimaryAlongRay(OverlayLayRayStart(XCm), OverlayLayRayEnd(XCm)))
			{
				Test.AddError(FString::Printf(
					TEXT("fixture: the course-1 click at x = %g must lay a brick"), XCm));

				return nullptr;
			}
		}

		if (!Controller.OnToolbarButton(EToolbarButtonId::ModeDestroy))
		{
			Test.AddError(TEXT("fixture: the Destroy tab must be clickable"));
			return nullptr;
		}

		FStructureBinding* const Binding = Fixture.TestWorld.Subsystem->Find(StructureId);

		if (Binding == nullptr || Binding->NumPieces() != OverlayWallPieceCount)
		{
			Test.AddError(FString::Printf(
				TEXT("fixture: the build must hold exactly %d bricks; it holds %d"),
				OverlayWallPieceCount, Binding != nullptr ? Binding->NumPieces() : INDEX_NONE));

			return nullptr;
		}

		// The bond must form joints; a jointless heap makes every piece answer the same.
		if (Binding->GetStructure().NumConnections() < OverlayWallPieceCount)
		{
			Test.AddError(FString::Printf(
				TEXT("fixture: the bond must actually form joints — %d bricks formed only %d "
					 "connections, so nothing here is measuring a structure"),
				Binding->NumPieces(), Binding->GetStructure().NumConnections()));

			return nullptr;
		}

		Test.TestTrue(
			TEXT("fixture: with a wall laid, the session must know it has a structure to command"),
			Controller.GetSessionToolbarState().bHasStructure);

		return Binding;
	}

	const TCHAR* OverlaySupportName(EPieceSupport Support)
	{
		switch (Support)
		{
		case EPieceSupport::Falling:   return TEXT("Falling");
		case EPieceSupport::Grounded:  return TEXT("Grounded");
		case EPieceSupport::Supported: return TEXT("Supported");
		case EPieceSupport::Stranded:  return TEXT("Stranded");
		}

		return TEXT("<a support state this build has never heard of>");
	}

	/** Every live piece's support verdict on one line, for failure messages. */
	FString OverlayDescribeSupport(const FStructureBinding& Binding)
	{
		FString Line;

		for (const int32 Piece : OverlayLivePieces(Binding))
		{
			Line += FString::Printf(
				TEXT("%s%d:%s"),
				Line.IsEmpty() ? TEXT("") : TEXT(", "),
				Piece,
				OverlaySupportName(Binding.GetStructure().GetPieceSupport(Piece)));
		}

		return Line.IsEmpty() ? TEXT("<no live pieces>") : Line;
	}

	/** Step the toolbar's course to the given one, one chip click at a time. */
	bool OverlaySetCourse(
		FAutomationTestBase& Test,
		ADestructionGamePlayerController& Controller,
		int32 Course)
	{
		for (int32 Guard = 0; Guard < 32; ++Guard)
		{
			const int32 Now = Controller.GetSessionToolbarState().Course;

			if (Now == Course)
			{
				return true;
			}

			if (!Controller.OnToolbarButton(
					Now < Course ? EToolbarButtonId::CourseUp : EToolbarButtonId::CourseDown))
			{
				break;
			}
		}

		Test.AddError(FString::Printf(
			TEXT("fixture: the course could not be stepped to %d; it reads %d"),
			Course, Controller.GetSessionToolbarState().Course));

		return false;
	}

	/**
	 * Assert every live piece wears what the two pure functions say.
	 *
	 * @return how many pieces came out Comfortable.
	 */
	int32 OverlayCheckWholeWall(
		FAutomationTestBase& Test,
		FStructureBinding& Binding,
		const TCHAR* When)
	{
		const FStructure& Structure = Binding.GetStructure();

		int32 Comfortable = 0;

		for (const int32 Piece : OverlayLivePieces(Binding))
		{
			ABrickActor* const Brick = BrickWorldTestSupport::BrickAt(Test, Binding, Piece);

			if (Brick == nullptr)
			{
				continue;
			}

			const EJointMarginBand Band = WorstJointBandForPiece(Structure, Piece);
			const EBrickHighlight Expected = BrickHighlightForLoadBand(Band);

			Comfortable += Expected == EBrickHighlight::LoadComfortable ? 1 : 0;

			Test.TestEqual(
				FString::Printf(
					TEXT("%s: piece %d's worst joint is %s, so the brick must wear %s. It wears %s. "
						 "The wall reads [%s]"),
					When, Piece, OverlayBandName(Band), OverlayHighlightName(Expected),
					OverlayHighlightName(Brick->GetHighlight()),
					*OverlayDescribeWall(Binding)),
				static_cast<int32>(Brick->GetHighlight()), static_cast<int32>(Expected));
		}

		return Comfortable;
	}

	/*
	 * The knot: one brick on the earth and three on the course above it.
	 *
	 *        A        R        L
	 *      +----+   +----+   +----+      ### = head joints
	 *      | A  |###| R  |###| L  |      A is seated on B0
	 *      +----+   +----+   +----+      R and L have nothing beneath them
	 *   +----+   void     void
	 *   | B0 |
	 *   +====+
	 *     earth
	 *
	 * The `SupportAuthority` fixture's shape, laid by clicks. Seatless pieces use head joints as
	 * supports, so R and L form a cycle the router's flood cannot carry to earth; the LP can, and
	 * `ApplyLimitAnalysisSupport` overwrites the router's verdict, but only in the break path
	 * (`SolveAndBreak`). A bare `SolveLoads` restores the router's answer.
	 *
	 * Measured: session-laid head joints are perpends (0.2 MPa cohesion, 0.1 MPa flexural bond), below
	 * the ~0.17 MPa bending demand, so the LP falls R and L too and the authorities agree. The shape is
	 * kept for when a stronger head joint can be laid.
	 */
	constexpr double OverlayKnotBaseXCm = 0.0;
	constexpr double OverlayKnotAbutmentXCm = 11.25;
	constexpr double OverlayKnotInnerXCm = 33.75;
	constexpr double OverlayKnotOuterXCm = 56.25;

	constexpr int32 OverlayKnotPieceCount = 4;

	FStructureBinding* OverlayLayTheKnot(FAutomationTestBase& Test, FOverlayFixture& Fixture)
	{
		ADestructionGamePlayerController& Controller = *Fixture.Controller;

		if (!Controller.OnToolbarButton(EToolbarButtonId::ModeBuild))
		{
			Test.AddError(TEXT("fixture: the Build tab must be clickable for any of this to run"));
			return nullptr;
		}

		const int32 StructureId = Fixture.Build->GetStructureId();

		if (!Controller.PrimaryAlongRay(
				OverlayLayRayStart(OverlayKnotBaseXCm), OverlayLayRayEnd(OverlayKnotBaseXCm)))
		{
			Test.AddError(TEXT("fixture: the one course-0 brick must land"));
			return nullptr;
		}

		if (!OverlaySetCourse(Test, Controller, 1))
		{
			return nullptr;
		}

		const double CourseOneXCm[] = {
			OverlayKnotAbutmentXCm, OverlayKnotInnerXCm, OverlayKnotOuterXCm };

		for (const double XCm : CourseOneXCm)
		{
			if (!Controller.PrimaryAlongRay(OverlayLayRayStart(XCm), OverlayLayRayEnd(XCm)))
			{
				Test.AddError(FString::Printf(
					TEXT("fixture: the course-1 click at x = %g must lay a brick"), XCm));

				return nullptr;
			}
		}

		if (!Controller.OnToolbarButton(EToolbarButtonId::ModeDestroy))
		{
			Test.AddError(TEXT("fixture: the Destroy tab must be clickable"));
			return nullptr;
		}

		FStructureBinding* const Binding = Fixture.TestWorld.Subsystem->Find(StructureId);

		if (Binding == nullptr || Binding->NumPieces() != OverlayKnotPieceCount)
		{
			Test.AddError(FString::Printf(
				TEXT("fixture: the knot must hold exactly %d bricks; it holds %d"),
				OverlayKnotPieceCount, Binding != nullptr ? Binding->NumPieces() : INDEX_NONE));

			return nullptr;
		}

		// Log where each brick landed; the knot depends on the two outer bricks having nothing under them.
		for (int32 Piece = 0; Piece < Binding->NumPieces(); ++Piece)
		{
			const FVector Centre = Binding->GetBinding(Piece).Box.CentreCm;

			Test.AddInfo(FString::Printf(
				TEXT("knot piece %d at (%.2f, %.2f, %.2f), grounded %d"),
				Piece, Centre.X, Centre.Y, Centre.Z,
				Binding->GetStructure().GetPiece(Piece).bIsGrounded ? 1 : 0));
		}

		Test.TestTrue(
			*FString::Printf(
				TEXT("fixture: the knot's bricks must form joints — %d bricks formed %d connections, and "
					 "the head joints ARE the cycle"),
				Binding->NumPieces(), Binding->GetStructure().NumConnections()),
			Binding->GetStructure().NumConnections() >= 3);

		return Binding;
	}
}

/**
 * The overlay tints every live piece by its worst joint and breaks nothing. The overlay solves on
 * every toggle and mutation, so each connection's intactness is recorded before and checked after.
 * At least one piece must be Comfortable, so an all-None stub fails. The overlay must also agree with
 * the details window (`BuildPieceMenuInspector`), which buckets each joint with `PresenterMarginBand`.
 * Needs a world but never ticks it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionLoadOverlayTintsTest,
	"DestructionGame.World.Session.LoadOverlayTintsByWorstJoint",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionLoadOverlayTintsTest::RunTest(const FString& Parameters)
{
	using namespace DestructionSession;
	using namespace SessionLoadOverlayTestSupport;

	FOverlayFixture Fixture;

	if (!Fixture.Begin(*this))
	{
		Fixture.End();
		return true;
	}

	FStructureBinding* const Binding = OverlayLayTheWall(*this, Fixture);

	if (Binding == nullptr)
	{
		Fixture.End();
		return true;
	}

	ADestructionGamePlayerController& Controller = *Fixture.Controller;

	// ONE: record every joint's state before the overlay is switched on.
	TArray<bool> WasIntact;

	for (int32 Index = 0; Index < Binding->GetStructure().NumConnections(); ++Index)
	{
		WasIntact.Add(!Binding->GetStructure().GetConnection(Index).HasGiven());
	}

	TestTrue(
		*FString::Printf(
			TEXT("fixture: a freshly laid wall must start with every joint intact, or 'still intact "
				 "afterwards' says nothing. %d of %d are"),
			WasIntact.FilterByPredicate([](bool b) { return b; }).Num(), WasIntact.Num()),
		WasIntact.Num() > 0 && !WasIntact.Contains(false));

	// TWO: the chip is live and the click lands.
	{
		const TArray<FToolbarButton> Buttons =
			SessionToolbarButtons(Controller.GetSessionToolbarState());

		const FToolbarButton* const Chip = Buttons.FindByPredicate(
			[](const FToolbarButton& Candidate)
			{
				return Candidate.Id == EToolbarButtonId::ToggleLoadOverlay;
			});

		TestTrue(
			TEXT("the Destroy strip must draw the load overlay chip, and with a wall laid it must be "
				 "live"),
			Chip != nullptr && Chip->bEnabled);
	}

	TestTrue(
		TEXT("THE CLICK MUST LAND. A command button that silently does nothing is indistinguishable "
			 "from the game having missed the click, which is why OnToolbarButton says so out loud"),
		Controller.OnToolbarButton(EToolbarButtonId::ToggleLoadOverlay));

	TestTrue(
		*FString::Printf(
			TEXT("and the session must record that the overlay is on — the chip is drawn from this "
				 "flag, so an unrecorded toggle is a tinted wall with an unlit chip over it. It reads "
				 "%d"),
			Controller.GetSessionToolbarState().bLoadOverlay ? 1 : 0),
		Controller.GetSessionToolbarState().bLoadOverlay);

	// THREE: every live piece wears its worst joint's band.
	const int32 Comfortable = OverlayCheckWholeWall(*this, *Binding, TEXT("with the overlay on"));

	AddInfo(FString::Printf(
		TEXT("with the overlay on the wall reads [%s]"), *OverlayDescribeWall(*Binding)));

	TestTrue(
		*FString::Printf(
			TEXT("AT LEAST ONE PIECE MUST COME OUT COMFORTABLE. Every claim above is an equality "
				 "against BrickHighlightForLoadBand(WorstJointBandForPiece(...)), and an overlay that "
				 "computes nothing satisfies all of them by answering None on both sides — an untinted "
				 "wall agreeing with itself. Five bricks at a fraction of a per cent of capacity are "
				 "comfortable everywhere; %d came out so"),
			Comfortable),
		Comfortable >= 1);

	// FOUR: the overlay's solve broke nothing.
	for (int32 Index = 0; Index < WasIntact.Num(); ++Index)
	{
		if (!Binding->GetStructure().GetConnection(Index).HasGiven())
		{
			continue;
		}

		AddError(FString::Printf(
			TEXT("LOOKING AT A WALL MUST NOT DEMOLISH IT. Joint %d was intact before the overlay was "
				 "switched on and has GIVEN after. SolveLoads is documented as non-destructive and the "
				 "overlay is the first thing that leans on that in anger — it solves on every toggle "
				 "and every mutation, so a solve that severs is a game where reading the load is how "
				 "you lose the building"),
			Index));
	}

	TestEqual(
		FString::Printf(
			TEXT("and the structure must still have all %d of its joints — a non-destructive solve "
				 "adds and removes nothing"),
			WasIntact.Num()),
		Binding->GetStructure().NumConnections(), WasIntact.Num());

	// FIVE: the overlay and the details window agree on the band.
	{
		int32 Middle = INDEX_NONE;

		for (const int32 Piece : OverlayLivePieces(*Binding))
		{
			const FVector Centre = Binding->GetBinding(Piece).Box.CentreCm;

			if (FMath::IsNearlyEqual(Centre.X, OverlayMiddleBottomXCm, 0.5)
				&& FMath::IsNearlyEqual(Centre.Z, OverlayBrickPlaneCourse0Cm, 0.5))
			{
				Middle = Piece;
				break;
			}
		}

		if (Middle == INDEX_NONE)
		{
			AddError(TEXT("fixture: the middle brick of the bottom course was not laid where the bond "
						  "puts it, so the cross-check has no subject"));
		}
		else
		{
			FPieceRef Ref;
			Ref.StructureId = Fixture.Build->GetStructureId();
			Ref.PieceIndex = Middle;

			const FPieceRef Selection[] = { Ref };
			const FPieceMenuInspector Inspector = BuildPieceMenuInspector(*Binding, Selection, Ref);

			EJointMarginBand WorstRow = EJointMarginBand::Comfortable;

			for (const FInspectorJointRow& Row : Inspector.Joints)
			{
				// Critical is enumerator zero, so the worst band is the smallest value.
				WorstRow = static_cast<EJointMarginBand>(
					FMath::Min(static_cast<uint8>(WorstRow), static_cast<uint8>(Row.MarginBand)));
			}

			const EJointMarginBand Overlay = WorstJointBandForPiece(Binding->GetStructure(), Middle);

			TestTrue(
				*FString::Printf(
					TEXT("fixture: the middle brick must actually have joints for the panel to bucket; "
						 "the inspector found %d"),
					Inspector.Joints.Num()),
				Inspector.Joints.Num() > 0);

			TestEqual(
				FString::Printf(
					TEXT("THE OVERLAY AND THE DETAILS WINDOW MUST BUCKET WITH THE SAME TWO NUMBERS. "
						 "The panel's %d joint rows for piece %d are worst at %s; the overlay calls the "
						 "piece %s. A brick tinted amber beside a panel calling all its joints "
						 "comfortable is two answers to one question, drawn two inches apart. (If this "
						 "is the only row failing, check whether a settle has left a min-violation "
						 "readout cached — the rows prefer it and the overlay reads the router.)"),
					Inspector.Joints.Num(), Middle, OverlayBandName(WorstRow), OverlayBandName(Overlay)),
				static_cast<int32>(Overlay), static_cast<int32>(WorstRow));
		}
	}

	Fixture.End();

	return true;
}

/**
 * The overlay is the weakest state: a hovered brick draws Hovered and returns to its band when the
 * cursor leaves. The return is what distinguishes an input to `HighlightForPiece` from a direct painter,
 * which would leave the brick None. Other bricks must not change while one is hovered. Needs a world
 * with real collision, never ticked.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionLoadOverlayYieldsToHoverTest,
	"DestructionGame.World.Session.LoadOverlayYieldsToHover",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionLoadOverlayYieldsToHoverTest::RunTest(const FString& Parameters)
{
	using namespace DestructionSession;
	using namespace SessionLoadOverlayTestSupport;

	FOverlayFixture Fixture;

	if (!Fixture.Begin(*this))
	{
		Fixture.End();
		return true;
	}

	FStructureBinding* const Binding = OverlayLayTheWall(*this, Fixture);

	if (Binding == nullptr)
	{
		Fixture.End();
		return true;
	}

	ADestructionGamePlayerController& Controller = *Fixture.Controller;

	if (!Controller.OnToolbarButton(EToolbarButtonId::ToggleLoadOverlay))
	{
		AddError(TEXT("fixture: the load overlay chip must be clickable with a wall laid"));
		Fixture.End();
		return true;
	}

	// Each brick's state with nothing hovered, to compare against later.
	TMap<int32, EBrickHighlight> Tinted;

	for (const int32 Piece : OverlayLivePieces(*Binding))
	{
		if (const ABrickActor* const Brick = Cast<ABrickActor>(Binding->GetActor(Piece)))
		{
			Tinted.Add(Piece, Brick->GetHighlight());
		}
	}

	int32 LoadStates = 0;

	for (const TPair<int32, EBrickHighlight>& Entry : Tinted)
	{
		LoadStates += OverlayIsALoadState(Entry.Value) ? 1 : 0;
	}

	TestTrue(
		*FString::Printf(
			TEXT("fixture: the overlay must actually have tinted something before 'the hover beats it' "
				 "can mean anything — %d of %d bricks wear a load state. The wall reads [%s]"),
			LoadStates, Tinted.Num(), *OverlayDescribeWall(*Binding)),
		LoadStates > 0);

	// ONE: hover the middle brick of the bottom course.
	int32 Pointed = INDEX_NONE;

	{
		const FVector Aim(OverlayMiddleBottomXCm, 0.0, OverlayBrickPlaneCourse0Cm);

		Controller.PointerAlongRay(OverlayInspectStart(Aim), OverlayInspectEnd(Aim));

		for (const TPair<int32, EBrickHighlight>& Entry : Tinted)
		{
			const ABrickActor* const Brick = Cast<ABrickActor>(Binding->GetActor(Entry.Key));

			if (Brick != nullptr && Brick->GetHighlight() == EBrickHighlight::Hovered)
			{
				Pointed = Entry.Key;
				break;
			}
		}

		TestTrue(
			*FString::Printf(
				TEXT("A HOVERED BRICK STILL DRAWS Hovered WITH THE OVERLAY ON. The overlay covers every "
					 "piece at once, so it has to be the weakest state there is — an instrument that "
					 "hid what the player is pointing at would take away the one reading they need "
					 "before pressing Delete. Nothing under the cursor reads Hovered; the wall reads "
					 "[%s]"),
				*OverlayDescribeWall(*Binding)),
			Pointed != INDEX_NONE);
	}

	// TWO: no other brick changed.
	for (const TPair<int32, EBrickHighlight>& Entry : Tinted)
	{
		if (Entry.Key == Pointed)
		{
			continue;
		}

		const ABrickActor* const Brick = Cast<ABrickActor>(Binding->GetActor(Entry.Key));

		if (Brick == nullptr)
		{
			continue;
		}

		TestEqual(
			FString::Printf(
				TEXT("pointing at one brick must not disturb the others: piece %d wore %s before the "
					 "cursor moved and wears %s now. The wall reads [%s]"),
				Entry.Key, OverlayHighlightName(Entry.Value),
				OverlayHighlightName(Brick->GetHighlight()), *OverlayDescribeWall(*Binding)),
			static_cast<int32>(Brick->GetHighlight()), static_cast<int32>(Entry.Value));
	}

	// THREE: the cursor leaves and the band returns.
	Controller.PointerAlongRay(OverlayEmptyRayStart(), OverlayEmptyRayEnd());

	if (Pointed != INDEX_NONE)
	{
		const ABrickActor* const Brick = Cast<ABrickActor>(Binding->GetActor(Pointed));

		if (Brick != nullptr)
		{
			TestEqual(
				FString::Printf(
					TEXT("THE BAND WAS MASKED, NOT LOST. Piece %d wore %s before the cursor arrived, "
						 "and must wear it again now the cursor has gone — an overlay that PAINTED "
						 "bricks rather than answering HighlightForPiece would pass the hover claim "
						 "above and leave this brick plain forever. It wears %s; the wall reads [%s]"),
					Pointed, OverlayHighlightName(Tinted[Pointed]),
					OverlayHighlightName(Brick->GetHighlight()),
					*OverlayDescribeWall(*Binding)),
				static_cast<int32>(Brick->GetHighlight()), static_cast<int32>(Tinted[Pointed]));
		}
	}

	Fixture.End();

	return true;
}

/**
 * Clicking the chip again clears the tint from every brick in the world, not just the flag (which
 * `Core.SessionToolbar.Transitions` pins). The cursor is moved off the wall first, since None is only
 * correct for an unclaimed brick. Needs a world, never ticked.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionLoadOverlayClearsTest,
	"DestructionGame.World.Session.LoadOverlayClearsWhenOff",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionLoadOverlayClearsTest::RunTest(const FString& Parameters)
{
	using namespace DestructionSession;
	using namespace SessionLoadOverlayTestSupport;

	FOverlayFixture Fixture;

	if (!Fixture.Begin(*this))
	{
		Fixture.End();
		return true;
	}

	FStructureBinding* const Binding = OverlayLayTheWall(*this, Fixture);

	if (Binding == nullptr)
	{
		Fixture.End();
		return true;
	}

	ADestructionGamePlayerController& Controller = *Fixture.Controller;

	if (!Controller.OnToolbarButton(EToolbarButtonId::ToggleLoadOverlay))
	{
		AddError(TEXT("fixture: the load overlay chip must be clickable with a wall laid"));
		Fixture.End();
		return true;
	}

	int32 TintedBefore = 0;

	for (const int32 Piece : OverlayLivePieces(*Binding))
	{
		const ABrickActor* const Brick = Cast<ABrickActor>(Binding->GetActor(Piece));

		TintedBefore += Brick != nullptr && OverlayIsALoadState(Brick->GetHighlight()) ? 1 : 0;
	}

	TestTrue(
		*FString::Printf(
			TEXT("fixture: something must be tinted before 'it all comes off' can mean anything — %d "
				 "bricks wear a load state. The wall reads [%s]"),
			TintedBefore, *OverlayDescribeWall(*Binding)),
		TintedBefore > 0);

	// Nothing may be hovered; None is only right for an unclaimed brick.
	Controller.PointerAlongRay(OverlayEmptyRayStart(), OverlayEmptyRayEnd());

	TestTrue(
		TEXT("the second click on the chip must land too — a toggle that only latches is a setting the "
			 "player cannot undo"),
		Controller.OnToolbarButton(EToolbarButtonId::ToggleLoadOverlay));

	TestFalse(
		*FString::Printf(
			TEXT("and the session must record that it is off; it reads %d"),
			Controller.GetSessionToolbarState().bLoadOverlay ? 1 : 0),
		Controller.GetSessionToolbarState().bLoadOverlay);

	for (const int32 Piece : OverlayLivePieces(*Binding))
	{
		const ABrickActor* const Brick = Cast<ABrickActor>(Binding->GetActor(Piece));

		if (Brick == nullptr)
		{
			continue;
		}

		TestEqual(
			FString::Printf(
				TEXT("WITH THE OVERLAY OFF AND NOTHING POINTED AT, EVERY BRICK IS PLAIN. Piece %d "
					 "wears %s. A refresh whose 'off' arm does nothing leaves the wall coloured by a "
					 "solve the player has no way of dating. The wall reads [%s]"),
				Piece, OverlayHighlightName(Brick->GetHighlight()),
				*OverlayDescribeWall(*Binding)),
			static_cast<int32>(Brick->GetHighlight()), static_cast<int32>(EBrickHighlight::None));
	}

	Fixture.End();

	return true;
}

/**
 * With nothing built the chip is greyed and the click is refused, the same precondition as
 * `Run structure` and `Clear build`. Both the strip's greying and the controller's refusal are checked,
 * since `ApplyToolbarButton` must defer to the strip rather than re-decide. Needs a world, never ticked.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionLoadOverlayGreyedTest,
	"DestructionGame.World.Session.LoadOverlayIsGreyedWithoutAStructure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionLoadOverlayGreyedTest::RunTest(const FString& Parameters)
{
	using namespace DestructionSession;
	using namespace SessionLoadOverlayTestSupport;

	FOverlayFixture Fixture;

	if (!Fixture.Begin(*this))
	{
		Fixture.End();
		return true;
	}

	ADestructionGamePlayerController& Controller = *Fixture.Controller;

	if (!Controller.OnToolbarButton(EToolbarButtonId::ModeDestroy))
	{
		AddError(TEXT("fixture: the Destroy tab must be clickable"));
		Fixture.End();
		return true;
	}

	TestEqual(
		FString::Printf(
			TEXT("fixture: this bare world lays no wall of its own and the player has laid nothing, so "
				 "the session must name no structure; it names %d"),
			Controller.GetSessionStructureId()),
		Controller.GetSessionStructureId(), static_cast<int32>(INDEX_NONE));

	{
		const TArray<FToolbarButton> Buttons =
			SessionToolbarButtons(Controller.GetSessionToolbarState());

		const FToolbarButton* const Chip = Buttons.FindByPredicate(
			[](const FToolbarButton& Candidate)
			{
				return Candidate.Id == EToolbarButtonId::ToggleLoadOverlay;
			});

		TestNotNull(
			TEXT("the chip must be DRAWN even with nothing built — a control that vanishes is a strip "
				 "that rearranges itself under a stationary cursor"),
			Chip);

		if (Chip != nullptr)
		{
			TestFalse(
				TEXT("and it must be GREYED: there is nothing to solve and nothing to tint, so a live "
					 "chip would latch on and colour zero bricks"),
				Chip->bEnabled);
		}
	}

	TestFalse(
		TEXT("AND THE DOOR MUST REFUSE IT TOO. OnToolbarButton says false out loud rather than "
			 "quietly doing nothing, because a silent no-op on a button reads as a dropped click"),
		Controller.OnToolbarButton(EToolbarButtonId::ToggleLoadOverlay));

	TestFalse(
		*FString::Printf(
			TEXT("and the refused click must be a bitwise no-op — the overlay flag reads %d"),
			Controller.GetSessionToolbarState().bLoadOverlay ? 1 : 0),
		Controller.GetSessionToolbarState().bLoadOverlay);

	Fixture.End();

	return true;
}

/**
 * Deleting a brick recomputes the overlay. The delete goes through the menu row, then the cursor is
 * moved off the wall. Asserts the removed piece is gone and the survivors wear what the model now says;
 * not that any band changed, since five bricks stay comfortable either way. Needs a world with real
 * collision, never ticked.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionLoadOverlayAfterDeleteTest,
	"DestructionGame.World.Session.LoadOverlayRefreshesAfterDelete",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionLoadOverlayAfterDeleteTest::RunTest(const FString& Parameters)
{
	using namespace DestructionSession;
	using namespace SessionLoadOverlayTestSupport;

	FOverlayFixture Fixture;

	if (!Fixture.Begin(*this))
	{
		Fixture.End();
		return true;
	}

	FStructureBinding* Binding = OverlayLayTheWall(*this, Fixture);

	if (Binding == nullptr)
	{
		Fixture.End();
		return true;
	}

	ADestructionGamePlayerController& Controller = *Fixture.Controller;
	const int32 StructureId = Fixture.Build->GetStructureId();

	if (!Controller.OnToolbarButton(EToolbarButtonId::ToggleLoadOverlay))
	{
		AddError(TEXT("fixture: the load overlay chip must be clickable with a wall laid"));
		Fixture.End();
		return true;
	}

	// ONE: delete the top course's left brick, which nothing stands on.
	int32 Doomed = INDEX_NONE;

	{
		const FVector Aim(OverlayCourse1CursorsXCm[0], 0.0, OverlayBrickPlaneCourse1Cm);

		Controller.PrimaryAlongRay(OverlayInspectStart(Aim), OverlayInspectEnd(Aim));

		const TArrayView<const FPieceMenuRow> Rows = Controller.GetShownPieceMenuRows();

		int32 DeleteRow = INDEX_NONE;

		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			if (Rows[Index].Label == FString(TEXT("Delete")))
			{
				DeleteRow = Index;
				break;
			}
		}

		if (DeleteRow == INDEX_NONE || Rows[DeleteRow].Ref.StructureId != StructureId)
		{
			AddError(FString::Printf(
				TEXT("fixture: the click on the top-left brick must put a Delete row up against the "
					 "player's own build; %d rows came back"),
				Rows.Num()));

			Fixture.End();
			return true;
		}

		Doomed = Rows[DeleteRow].Ref.PieceIndex;

		TestTrue(
			TEXT("fixture: choosing Delete must report that it committed"),
			Controller.ChoosePieceMenuRow(DeleteRow));
	}

	// Move the cursor off so the hover claims no survivor.
	Controller.PointerAlongRay(OverlayEmptyRayStart(), OverlayEmptyRayEnd());

	Binding = Fixture.TestWorld.Subsystem->Find(StructureId);

	if (Binding == nullptr)
	{
		AddError(TEXT("the build structure vanished under the delete"));
		Fixture.End();
		return true;
	}

	TestTrue(
		*FString::Printf(
			TEXT("fixture: piece %d must actually be gone from the structure"), Doomed),
		Binding->IsPieceRemoved(Doomed));

	// TWO: survivors are still tinted against the current structure.
	const int32 Comfortable =
		OverlayCheckWholeWall(*this, *Binding, TEXT("after a brick has been pulled"));

	int32 StillTinted = 0;

	for (const int32 Piece : OverlayLivePieces(*Binding))
	{
		const ABrickActor* const Brick = Cast<ABrickActor>(Binding->GetActor(Piece));

		StillTinted += Brick != nullptr && OverlayIsALoadState(Brick->GetHighlight()) ? 1 : 0;
	}

	AddInfo(FString::Printf(
		TEXT("after the delete the wall reads [%s]"), *OverlayDescribeWall(*Binding)));

	TestTrue(
		*FString::Printf(
			TEXT("THE SURVIVORS MUST STILL BE TINTED AFTER A DELETE. The overlay's whole promise is "
				 "'see where the load is, then pull that one', and a refresh that ran only on the "
				 "toggle leaves the wall coloured by a structure that no longer exists — a green brick "
				 "standing over a hole, which is worse than no advice at all. %d of the %d survivors "
				 "wear a load state, and %d are comfortable"),
			StillTinted, OverlayLivePieces(*Binding).Num(), Comfortable),
		StillTinted > 0);

	Fixture.End();

	return true;
}

/**
 * A piece the settle released to physics draws plain, while standing pieces keep their bands. A
 * released piece is no longer part of the structure (like `IsPieceRemoved`, one step earlier). Without
 * this it reads fail-closed Critical, the same red as a brick about to fail.
 *
 * Asserts `IsReleased` and `GetHighlight`, never distance (the world is not ticked). Survivors are
 * checked too, so an overlay that stopped working entirely cannot pass. Needs a world with real
 * collision, never ticked.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionLoadOverlayIgnoresReleasedTest,
	"DestructionGame.World.Session.LoadOverlayIgnoresReleasedPieces",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionLoadOverlayIgnoresReleasedTest::RunTest(const FString& Parameters)
{
	using namespace DestructionSession;
	using namespace SessionLoadOverlayTestSupport;

	FOverlayFixture Fixture;

	if (!Fixture.Begin(*this))
	{
		Fixture.End();
		return true;
	}

	FStructureBinding* Binding = OverlayLayTheWall(*this, Fixture);

	if (Binding == nullptr)
	{
		Fixture.End();
		return true;
	}

	ADestructionGamePlayerController& Controller = *Fixture.Controller;
	const int32 StructureId = Fixture.Build->GetStructureId();

	/*
	 * ONE: a free brick in mid-air, three courses up, bonded to nothing (as in
	 * `Visual.SessionScreenshots` frame 4), so Run has something to release. Free placement forms no
	 * joint, and 120 cm is four snap radii from the wall anyway.
	 */
	constexpr double FreeBrickXCm = 120.0;
	constexpr int32 FreeBrickCourse = 3;

	if (!Controller.OnToolbarButton(EToolbarButtonId::ModeBuild)
		|| !Controller.OnToolbarButton(EToolbarButtonId::PieceBrick)
		|| !Controller.OnToolbarButton(EToolbarButtonId::PlacementFree))
	{
		AddError(TEXT("fixture: Build, Brick and Free are all always live in Build mode"));
		Fixture.End();
		return true;
	}

	if (!OverlaySetCourse(*this, Controller, FreeBrickCourse))
	{
		Fixture.End();
		return true;
	}

	const int32 ConnectionsBefore = Binding->GetStructure().NumConnections();
	const int32 Free = Binding->NumPieces();

	if (!Controller.PrimaryAlongRay(
			OverlayLayRayStart(FreeBrickXCm), OverlayLayRayEnd(FreeBrickXCm)))
	{
		AddError(TEXT("fixture: the Free click in mid-air must lay a brick"));
		Fixture.End();
		return true;
	}

	Binding = Fixture.TestWorld.Subsystem->Find(StructureId);

	if (Binding == nullptr || Binding->NumPieces() != Free + 1)
	{
		AddError(FString::Printf(
			TEXT("fixture: the free placement did not produce piece %d"), Free));

		Fixture.End();
		return true;
	}

	// Preconditions: a grounded or bonded brick would stand, inverting the claim.
	TestEqual(
		FString::Printf(
			TEXT("fixture: the Free brick must form NO joint — the build held %d connection(s) before it "
				 "and holds %d after"),
			ConnectionsBefore, Binding->GetStructure().NumConnections()),
		Binding->GetStructure().NumConnections(), ConnectionsBefore);

	TestFalse(
		*FString::Printf(
			TEXT("fixture: and it must not read grounded, or nothing in this build can fall. Its box is "
				 "centred at Z = %.2f"),
			Binding->GetBinding(Free).Box.CentreCm.Z),
		Binding->GetStructure().GetPiece(Free).bIsGrounded);

	TestFalse(
		TEXT("fixture: and nothing may have let go of it before Run"),
		Binding->IsReleased(Free));

	// TWO: overlay on, then Run.
	if (!Controller.OnToolbarButton(EToolbarButtonId::ModeDestroy)
		|| !Controller.OnToolbarButton(EToolbarButtonId::ToggleLoadOverlay))
	{
		AddError(TEXT("fixture: Destroy is always live, and the overlay chip is live with a wall laid"));
		Fixture.End();
		return true;
	}

	// Nothing may be hovered; None is only right for an unclaimed brick.
	Controller.PointerAlongRay(OverlayEmptyRayStart(), OverlayEmptyRayEnd());

	AddInfo(FString::Printf(
		TEXT("with the overlay on and before the Run the wall reads [%s]"), *OverlayDescribeWall(*Binding)));

	TestTrue(
		TEXT("fixture: Run structure must be live over a build with six pieces in it"),
		Controller.OnToolbarButton(EToolbarButtonId::RunStructure));

	Binding = Fixture.TestWorld.Subsystem->Find(StructureId);

	if (Binding == nullptr)
	{
		AddError(TEXT("the build structure vanished under Run"));
		Fixture.End();
		return true;
	}

	TestTrue(
		*FString::Printf(
			TEXT("fixture: RUN MUST LET GO OF THE FLOATING BRICK, or there is no released piece to make "
				 "any claim about. IsReleased(%d) reports %d"),
			Free, Binding->IsReleased(Free) ? 1 : 0),
		Binding->IsReleased(Free));

	// THREE: the released brick is plain and the survivors are tinted.
	{
		const ABrickActor* const Brick = Cast<ABrickActor>(Binding->GetActor(Free));

		TestNotNull(
			TEXT("fixture: the released brick must still have an actor to read a highlight off — release "
				 "hands a body to physics, it does not destroy it"),
			Brick);

		if (Brick != nullptr)
		{
			TestEqual(
				FString::Printf(
					TEXT("A RELEASED BRICK WEARS NO BAND. Piece %d has been let go of — it is a rigid body "
						 "falling through the air, and its joints say nothing about it any more — so it must "
						 "draw plain. It wears %s, and today that is the fail-closed Critical: the same red "
						 "as the brick about to fail under the wall, spent on one the player can already see "
						 "moving. The wall reads [%s]"),
					Free, OverlayHighlightName(Brick->GetHighlight()), *OverlayDescribeWall(*Binding)),
				static_cast<int32>(Brick->GetHighlight()), static_cast<int32>(EBrickHighlight::None));
		}
	}

	int32 Survivors = 0;
	int32 SurvivorsTinted = 0;

	for (const int32 Piece : OverlayLivePieces(*Binding))
	{
		if (Piece == Free || Binding->IsReleased(Piece))
		{
			continue;
		}

		++Survivors;

		const ABrickActor* const Brick = Cast<ABrickActor>(Binding->GetActor(Piece));

		if (Brick == nullptr)
		{
			continue;
		}

		const EJointMarginBand Band = WorstJointBandForPiece(Binding->GetStructure(), Piece);
		const EBrickHighlight Expected = BrickHighlightForLoadBand(Band);

		SurvivorsTinted += OverlayIsALoadState(Brick->GetHighlight()) ? 1 : 0;

		TestEqual(
			FString::Printf(
				TEXT("AND THE WALL THAT IS STILL STANDING MUST STILL BE TINTED: piece %d's worst joint is "
					 "%s, so it must wear %s. It wears %s. The wall reads [%s]"),
				Piece, OverlayBandName(Band), OverlayHighlightName(Expected),
				OverlayHighlightName(Brick->GetHighlight()), *OverlayDescribeWall(*Binding)),
			static_cast<int32>(Brick->GetHighlight()), static_cast<int32>(Expected));
	}

	TestEqual(
		FString::Printf(
			TEXT("fixture: the five-brick bond must still be standing after the Run — %d of its pieces "
				 "are unreleased"),
			Survivors),
		Survivors, OverlayWallPieceCount);

	TestTrue(
		*FString::Printf(
			TEXT("AND AT LEAST ONE SURVIVOR MUST WEAR A LOAD STATE. Every claim above is an equality "
				 "against the two pure functions, and an overlay that had simply stopped working would "
				 "satisfy all of them AND the released-brick claim by drawing the whole plot plain. %d of "
				 "%d survivors are tinted"),
			SurvivorsTinted, Survivors),
		SurvivorsTinted >= 1);

	Fixture.End();

	return true;
}

/**
 * Toggling the overlay on over a settled structure costs no solve and changes no support verdict; over
 * an unsolved structure it costs exactly one.
 *
 * A second solve is not just waste. The settle's `SolveAndBreak` lets `ApplyLimitAnalysisSupport`
 * overwrite the router's support with the LP's; a bare `SolveLoads` rebuilds it from the router alone,
 * so re-solving could replace the settled answer that `ApplyResults` releases from.
 *
 * The knot does not currently diverge (measured): session-laid head joints are perpends too weak for
 * the LP to carry the cantilever, so both authorities give [Grounded, Supported, Falling, Falling]. The
 * solve count drives this test; the per-piece verdict check is a guard. `Acceptance.SupportAuthority.*`
 * pins the disagreement at Core level. The wall must also be tinted, so an overlay that neither solves
 * nor paints fails. Needs a world, never ticked.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionLoadOverlayNoResolveTest,
	"DestructionGame.World.Session.LoadOverlayDoesNotResolveASettledStructure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionLoadOverlayNoResolveTest::RunTest(const FString& Parameters)
{
	using namespace DestructionSession;
	using namespace SessionLoadOverlayTestSupport;

	FOverlayFixture Fixture;

	if (!Fixture.Begin(*this))
	{
		Fixture.End();
		return true;
	}

	FStructureBinding* Binding = OverlayLayTheKnot(*this, Fixture);

	if (Binding == nullptr)
	{
		Fixture.End();
		return true;
	}

	ADestructionGamePlayerController& Controller = *Fixture.Controller;
	const int32 StructureId = Fixture.Build->GetStructureId();

	// ONE: an unsolved structure gets exactly one solve.
	TestEqual(
		FString::Printf(
			TEXT("fixture: laying bricks must not solve — the live-feedback-off default is what makes "
				 "the details window read 'not solved yet'. The knot reads %d solve(s)"),
			Binding->GetStructure().NumSolves()),
		Binding->GetStructure().NumSolves(), 0);

	TestFalse(
		TEXT("fixture: and nothing has answered for piece 0's support, which is what 'unsolved' means "
			 "to every reader of this graph"),
		Binding->GetStructure().HasSupportAnswer(0));

	TestTrue(
		TEXT("fixture: the load overlay chip must be clickable with a knot laid"),
		Controller.OnToolbarButton(EToolbarButtonId::ToggleLoadOverlay));

	TestEqual(
		FString::Printf(
			TEXT("A STRUCTURE WITH NO ANSWER MUST BE SOLVED — ONCE. Switching the overlay on over a wall "
				 "nothing has looked at has nothing to read, so it must run the solve it needs and no "
				 "more than that. It ran %d"),
			Binding->GetStructure().NumSolves()),
		Binding->GetStructure().NumSolves(), 1);

	for (const int32 Piece : OverlayLivePieces(*Binding))
	{
		TestTrue(
			*FString::Printf(
				TEXT("and that solve must have answered for piece %d — an overlay drawn off an answer "
					 "that does not exist is the full-green-bar defect one layer out"),
				Piece),
			Binding->GetStructure().HasSupportAnswer(Piece));
	}

	AddInfo(FString::Printf(
		TEXT("after the first toggle the knot reads support [%s] and wears [%s]"),
		*OverlayDescribeSupport(*Binding), *OverlayDescribeWall(*Binding)));

	// TWO: overlay off, then settle.
	{
		const int32 SolvesBeforeOff = Binding->GetStructure().NumSolves();

		TestTrue(
			TEXT("fixture: the chip must turn off again"),
			Controller.OnToolbarButton(EToolbarButtonId::ToggleLoadOverlay));

		TestEqual(
			FString::Printf(
				TEXT("AND TAKING THE OVERLAY OFF SOLVES NOTHING AT ALL. There is nothing to compute for a "
					 "wall that is about to be drawn plain, and a solve spent on it is a whole-structure "
					 "pass paid for putting a colour AWAY. It ran %d, having run %d"),
				Binding->GetStructure().NumSolves(), SolvesBeforeOff),
			Binding->GetStructure().NumSolves(), SolvesBeforeOff);
	}

	TestTrue(
		TEXT("fixture: Run structure must be live over the knot"),
		Controller.OnToolbarButton(EToolbarButtonId::RunStructure));

	Binding = Fixture.TestWorld.Subsystem->Find(StructureId);

	if (Binding == nullptr)
	{
		AddError(TEXT("the knot's structure vanished under Run"));
		Fixture.End();
		return true;
	}

	// THREE: record what the settle decided.
	const int32 SettledSolves = Binding->GetStructure().NumSolves();

	TArray<int32> SettledSupport;
	TArray<int32> LivePieces;

	for (const int32 Piece : OverlayLivePieces(*Binding))
	{
		LivePieces.Add(Piece);
		SettledSupport.Add(static_cast<int32>(Binding->GetStructure().GetPieceSupport(Piece)));
	}

	AddInfo(FString::Printf(
		TEXT("the settle ran %d solve(s) in total and left support [%s]"),
		SettledSolves, *OverlayDescribeSupport(*Binding)));

	TestTrue(
		*FString::Printf(
			TEXT("fixture: the settle must actually have solved something — it ran %d"), SettledSolves),
		SettledSolves > 0);

	Controller.PointerAlongRay(OverlayEmptyRayStart(), OverlayEmptyRayEnd());

	// FOUR: toggling on changes neither the solve count nor any verdict.
	TestTrue(
		TEXT("fixture: the chip must turn back on after the Run"),
		Controller.OnToolbarButton(EToolbarButtonId::ToggleLoadOverlay));

	TestEqual(
		FString::Printf(
			TEXT("THE OVERLAY MUST READ THE SETTLE, NOT RE-RUN IT. The structure was settled by "
				 "SolveAndBreak, whose equilibrium gate makes the LP the support authority below the "
				 "block cap; a bare SolveLoads has no gate and rebuilds the same arrays from the router's "
				 "flood alone. So a refresh that solves does not merely cost a whole-structure pass per "
				 "click — it REPLACES the settled answer with a different one, on the very array "
				 "ApplyResults releases from. The settle left %d solve(s); after one look there are %d"),
			SettledSolves, Binding->GetStructure().NumSolves()),
		Binding->GetStructure().NumSolves(), SettledSolves);

	for (int32 Index = 0; Index < LivePieces.Num(); ++Index)
	{
		const int32 Piece = LivePieces[Index];

		const int32 Now = static_cast<int32>(Binding->GetStructure().GetPieceSupport(Piece));

		TestEqual(
			FString::Printf(
				TEXT("AND NO PIECE MAY CHANGE ITS MIND ABOUT WHETHER IT IS HELD UP. Piece %d read %s when "
					 "the settle finished and reads %s after the overlay looked at it. A GUARD RATHER "
					 "THAN THE DRIVER, and the file header says why: on this fixture both authorities "
					 "agree, because the session's own head joints are perpends too weak for the LP to "
					 "carry a two-brick cantilever on. It is the reading that would move first if they "
					 "ever diverged, and the one a 'cache it' implementation could leave stale. The wall "
					 "reads support [%s]"),
				Piece, OverlaySupportName(static_cast<EPieceSupport>(SettledSupport[Index])),
				OverlaySupportName(static_cast<EPieceSupport>(Now)),
				*OverlayDescribeSupport(*Binding)),
			Now, SettledSupport[Index]);
	}

	// FIVE: bands are still drawn, so FOUR cannot pass by doing nothing.
	int32 Tinted = 0;
	int32 Standing = 0;

	for (const int32 Piece : OverlayLivePieces(*Binding))
	{
		if (Binding->IsReleased(Piece))
		{
			continue;
		}

		++Standing;

		const ABrickActor* const Brick = Cast<ABrickActor>(Binding->GetActor(Piece));

		Tinted += Brick != nullptr && OverlayIsALoadState(Brick->GetHighlight()) ? 1 : 0;
	}

	AddInfo(FString::Printf(
		TEXT("after the second toggle the knot wears [%s]"), *OverlayDescribeWall(*Binding)));

	TestTrue(
		*FString::Printf(
			TEXT("AND THE WALL MUST STILL BE TINTED. An overlay that neither solves nor paints satisfies "
				 "every claim above and is not an overlay — the settled solve's own numbers are what it "
				 "has to read. %d of the %d pieces still standing wear a load state"),
			Tinted, Standing),
		Standing == 0 || Tinted >= 1);

	Fixture.End();

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
