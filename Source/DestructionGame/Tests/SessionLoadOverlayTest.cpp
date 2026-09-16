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
 * SESSION S12 — THE LOAD OVERLAY: THE WALL TINTED BY WHERE THE LOAD IS, WITHOUT HIDING WHAT THE
 * PLAYER IS POINTING AT AND WITHOUT TOUCHING A SINGLE JOINT.
 *
 * =====================================================================================
 * THE BEHAVIOUR IN ONE SENTENCE
 * =====================================================================================
 *
 * With the Destroy strip's `Load overlay` chip latched on, every LIVE piece of the session's
 * structure wears the highlight of its worst joint's margin band; the overlay is the WEAKEST state in
 * `HighlightForPiece`'s precedence, it is recomputed after anything that changes the structure, it
 * clears completely when the chip is clicked again, and the solve it runs to compute it leaves every
 * connection exactly as intact as it found it.
 *
 * =====================================================================================
 * WHY THE OVERLAY IS AN INPUT TO THE PRECEDENCE AND NOT A SetHighlighted OF ITS OWN
 * =====================================================================================
 *
 * This is the whole design risk of the slice, and it is why `LoadOverlayYieldsToHover` exists as a
 * separate test rather than as a line in the first one. The overlay covers EVERY live piece at once —
 * unlike the hover (one), the selection (a handful) and the neighbour hues (six) — so a refresh that
 * painted bricks directly would be in a fight with the cursor that it wins: the next overlay refresh
 * would repaint over the selection, and the one thing a player must be able to check before pressing
 * Delete is which bricks are going. `HighlightForPiece` is already the one function that decides where
 * states coincide, and the correct shape is for the overlay to be one more question it asks, at the
 * bottom of the order.
 *
 * =====================================================================================
 * WHAT IS ASSERTED, AND WHY IT IS NEVER A COLOUR
 * =====================================================================================
 *
 * Every claim here is a MECHANISM reading — `GetHighlight()`, the toolbar state's own flag, the
 * returned bool, `FConnection::HasGiven`, `IsPieceRemoved`. Nothing asks what anything looks like:
 * which asset a state wears is `World.Brick.HighlightWearsAMaterial`'s, and whether that asset looks
 * green is a question for a human at a running game. Nothing is read as a distance moved either —
 * the overlay moves nothing, and a displacement assertion could not tell a tinted wall from an
 * untinted one.
 *
 * AND THE EXPECTED STATE IS COMPOSED FROM THE TWO PURE FUNCTIONS rather than written down as a
 * literal: `BrickHighlightForLoadBand(WorstJointBandForPiece(...))`. Which band a given brick of a
 * five-brick wall lands in is a solver answer this file has no business pinning — `Core.LoadOverlay.*`
 * is where the bands and the mapping are pinned, and the claim HERE is that the world wears what
 * those two say. THAT ALONE WOULD BE VACUOUS IF EVERYTHING ANSWERED None, which is exactly what a
 * stub does, so the sweep also insists that at least one piece comes out Comfortable.
 *
 * NEEDS A TICKING WORLD: a WORLD, yes — real `ABrickActor`s, a real subsystem, a real line trace for
 * the Destroy ray. None of these ticks one; nothing here is about anything moving.
 *
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous namespace
 * is private to a TRANSLATION UNIT rather than to a file, and a unity build merges many files into
 * one. See CURRENT_STATE.md.
 */
namespace SessionLoadOverlayTestSupport
{
	using namespace DestructionSession;

	/*
	 * THE RESTS-ON-THE-GROUND ARITHMETIC, SPELLED OUT RATHER THAN IMPORTED (DESIGN §8, 2026-09-15).
	 *
	 * A brick is 21.5 x 10.25 x 6.5 cm on a 1 cm joint, so the coordinating grid is 22.5 across, 11.25
	 * for the half stagger and 7.5 up. Course 0 rests a brick ON the earth, centred at 3.25; course 1
	 * centres at 10.75. Calling `DestructionSession::CoursePlaneZCm` here would make this file agree
	 * with the plane function however wrong it is.
	 */
	constexpr double OverlayBrickPlaneCourse0Cm = 3.25;
	constexpr double OverlayBrickPlaneCourse1Cm = 10.75;

	/**
	 * THE RUNNING-BOND WALL THE PLAYER LAYS, three on the ground and two straddling them.
	 *
	 * THE SAME WALL `Visual.SessionScreenshots` PHOTOGRAPHS, and it is the smallest thing that is
	 * actually a STRUCTURE rather than a row of loose boxes: the bond gives two head joints on course
	 * 0 and four bed joints up to course 1, so a piece in the middle has several joints and "the worst
	 * of them" is a question with more than one answer. A single brick would let a first-joint-wins
	 * overlay pass everything in this file.
	 */
	constexpr double OverlayCourse0CursorsXCm[] = { 0.0, 22.5, 45.0 };
	constexpr double OverlayCourse1CursorsXCm[] = { 11.25, 33.75 };

	/** The middle brick of the bottom course — the one every ray in this file is aimed at. */
	constexpr double OverlayMiddleBottomXCm = 22.5;

	/** How many pieces the wall above is. Asserted rather than assumed; a short wall is a bad fixture. */
	constexpr int32 OverlayWallPieceCount = 5;

	/**
	 * How far above the build plane a LAYING ray starts, and how far below it ends.
	 *
	 * THE RAY IS A DIRECTION, NOT A POINT: the controller hands the component a direction and the
	 * component intersects it with the build plane, so the end's own Z is irrelevant — which is why it
	 * is taken to Z = 0 rather than to the plane. A test that aimed the end AT the plane would pass
	 * against a controller that ignored the plane entirely.
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

	/**
	 * How far along Y a DESTROY ray runs, either side of the wall.
	 *
	 * A brick is 10.25 cm deep centred on Y = 0, so +/- 100 cm is far outside it on both sides and the
	 * ray crosses the whole thickness. Along Y so that no other brick of the bond is in the way.
	 */
	constexpr double OverlayInspectReachCm = 100.0;

	FVector OverlayInspectStart(const FVector& CentreCm)
	{
		return FVector(CentreCm.X, CentreCm.Y - OverlayInspectReachCm, CentreCm.Z);
	}

	FVector OverlayInspectEnd(const FVector& CentreCm)
	{
		return FVector(CentreCm.X, CentreCm.Y + OverlayInspectReachCm, CentreCm.Z);
	}

	/**
	 * A RAY AIMED AT NOTHING AT ALL, for letting go of the hover.
	 *
	 * A HUNDRED METRES OUT AND BELOW THE EARTH, so it cannot graze the wall, the plot or anything a
	 * later fixture might add. `HoverAlongRay` answers a ray that hit nothing with a default ref,
	 * which is what puts the previously-hovered brick back to whatever it is underneath — and
	 * "underneath" is the claim this file keeps making.
	 */
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

	/** Every live piece of a binding, as handles. Tombstones excluded; a hole wears nothing. */
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

	/** Which state every live piece is in, on one line, so a failure reads without a debugger. */
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
	 * A controller in the world with a REAL ULocalPlayer, and its build component.
	 *
	 * THE LOCAL PLAYER IS NOT DECORATION — `SetSessionControls` sets the input mode and the session's
	 * mapping contexts go through the Enhanced Input LOCAL PLAYER subsystem, and a controller without
	 * one silently fails that half closed. The same fixture `SessionControllerTest.cpp` uses, spelled
	 * again here because its namespace is private to that translation unit.
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
	 * LAY THE FIVE-BRICK BOND THROUGH THE PLAYER'S OWN CLICKS AND LEAVE THE SESSION IN DESTROY MODE.
	 *
	 * EVERY PLACEMENT IS `PrimaryAlongRay`, WHICH IS THE PLAYER'S CLICK — not `AddPiece`, not the
	 * subsystem's door. The thing under test is a session, and a fixture that assembled the structure
	 * behind the session's back would leave the toolbar's own precondition, the build component's
	 * course and the binding's identity all untested on the way in.
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

		/*
		 * AND IT MUST BE A STRUCTURE RATHER THAN FIVE LOOSE BOXES. The running bond's two head joints
		 * and four bed joints are what make "the worst of a piece's joints" a question worth asking;
		 * a jointless heap would make every piece answer the same way whatever the implementation did.
		 */
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

	/** Every live piece's support verdict, on one line, so a failure reads without a debugger. */
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

	/**
	 * Step the toolbar's course to a given one, THROUGH THE CHIPS.
	 *
	 * The build plane is derived from the course, and the course is only reachable one click at a
	 * time — which is the player's own route and the only one the controller offers.
	 */
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
	 * ASSERT THE WHOLE WALL WEARS WHAT THE TWO PURE FUNCTIONS SAY IT SHOULD, and that it is not all
	 * `None`.
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
	 * =====================================================================================
	 * THE KNOT: A COURSE SPANNING A VOID, WHERE THE TWO AUTHORITIES DISAGREE ABOUT SUPPORT
	 * =====================================================================================
	 *
	 * ONE brick on the earth, and THREE laid on the course above it:
	 *
	 *        A        R        L
	 *      +----+   +----+   +----+      ### = head joints (vertical faces)
	 *      | A  |###| R  |###| L  |      A straddles B0 and is genuinely SEATED
	 *      +----+   +----+   +----+      R and L have NOTHING beneath them
	 *   +----+   void     void
	 *   | B0 |                           B0 rests on the earth
	 *   +====+
	 *     earth
	 *
	 * THIS IS THE `SupportAuthority` FIXTURE'S SHAPE, LAID THROUGH THE PLAYER'S OWN CLICKS. A seatless
	 * piece falls back to its head joints as supports, sign-blind, so R's supports are {A, L} and L's
	 * are {R} — a two-node cycle the ROUTER's downward flood has no rule to divide, which is why it
	 * cannot carry either of them to the earth. The LP has no routing to fail: if an admissible
	 * equilibrium exists at self-weight it stands the whole thing, and below the block cap
	 * `ApplyLimitAnalysisSupport` OVERWRITES the router's per-piece verdict with the LP's.
	 *
	 * WHICH IS WHY THIS SHAPE IS HERE RATHER THAN THE FIVE-BRICK BOND. That overwrite happens inside
	 * the equilibrium gate, and the gate runs in the BREAK path — `SolveAndBreak`. A bare `SolveLoads`
	 * over a settled structure therefore rewrites the support arrays from the router's own flood, and
	 * the LP's answer is gone. On a wall that stands every which way that is invisible.
	 *
	 * MEASURED, AND IT DOES NOT DIVERGE HERE: laid through the session the head joints are PERPENDS
	 * (0.2 MPa cohesion, 0.1 MPa flexural bond), which a two-brick cantilever's ~0.17 MPa bending
	 * demand at the inner head joint is past — so the LP falls R and L as well and both authorities
	 * agree. The shape is kept because it is the one that would diverge if the session could ever lay a
	 * head joint the LP can carry two bricks on; see the test header.
	 *
	 * @return the binding, left in Destroy mode, or null with the reason already reported.
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

		/*
		 * AND IT MUST BE THE SHAPE DESCRIBED, WHICH IS A CLAIM ABOUT WHERE THE BRICKS LANDED. The snap
		 * solver ranks by raw distance and the labels are its own; what makes this a knot is that the
		 * two outer bricks have nothing under them, and the only way to say so is where they are.
		 */
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
 * THE OVERLAY TINTS EVERY LIVE PIECE BY ITS WORST JOINT, AND BREAKS NOTHING DOING IT.
 *
 * TWO CLAIMS, AND THE SECOND IS THE ONE THAT COULD COST A PLAYER THEIR BUILDING.
 * `FStructureBinding::SolveLoads` is documented as leaving every connection exactly as intact as it
 * found it, and the overlay is the first thing in this game to lean on that sentence in anger — it
 * solves on a toggle and again on every mutation, so a solve that severed anything would make LOOKING
 * at a wall a way of demolishing it. The intactness of every connection is therefore recorded before
 * the toggle and checked after, joint for joint, rather than being taken on the header's word.
 *
 * THE ANTI-VACUITY FLOOR IS "AT LEAST ONE PIECE IS COMFORTABLE", and it is the whole reason this test
 * can fail against a stub. The per-piece claim is an equality against
 * `BrickHighlightForLoadBand(WorstJointBandForPiece(...))`, and a build where both of those answer
 * nothing satisfies it on every brick — a wall that is entirely untinted agreeing with an overlay that
 * computes nothing. A five-brick wall at a fraction of a per cent of its capacity is comfortable
 * everywhere, so at least one is a floor the true behaviour clears by five.
 *
 * AND THE OVERLAY AGREES WITH THE DETAILS WINDOW, which is the cross-surface claim and the reason the
 * band is a MODEL answer at all. `BuildPieceMenuInspector` buckets each joint of one brick with
 * `PresenterMarginBand`; the overlay buckets the worst of them. Drawn two inches apart, a brick tinted
 * amber beside a panel calling every one of its joints comfortable is two answers to one question —
 * the same defect `Content.NeighbourSwatchesMatchTheirMaterials` exists to prevent one layer out.
 *
 * NEEDS A TICKING WORLD: a world, but it never ticks one.
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

	/* --- ONE: what the wall's joints are BEFORE anything looks at them ---------------------- */

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

	/* --- TWO: the chip is live, and the click lands ----------------------------------------- */

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

	/* --- THREE: every live piece wears its worst joint's band ------------------------------- */

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

	/* --- FOUR: and the solve it ran broke nothing ------------------------------------------- */

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

	/* --- FIVE: the overlay and the details window bucket with the same two numbers ----------- */

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
				/* Critical is enumerator ZERO, so "worst" is the SMALLEST value, not the largest. */
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
 * THE OVERLAY IS THE WEAKEST STATE THERE IS: A BRICK UNDER THE CURSOR DRAWS Hovered, AND GOES BACK TO
 * ITS BAND WHEN THE CURSOR LEAVES.
 *
 * THE PRECEDENCE IS THE POINT, AND BOTH HALVES OF IT ARE. That a hovered brick reads `Hovered` is the
 * first half; that it comes back up as its own band when the cursor moves off is the second, and it is
 * the half that tells an overlay which is an INPUT to `HighlightForPiece` apart from one that painted
 * bricks directly and was then overwritten. A direct painter passes the first claim and leaves the
 * brick `None` forever after the cursor leaves — `World.Select`'s "the hover is masked, not lost"
 * argument, applied to the state at the other end of the order.
 *
 * AND THE REST OF THE WALL MUST NOT MOVE WHILE ONE BRICK IS POINTED AT. Hovering is one brick's event;
 * a refresh that recomputed the whole overlay on every mouse move would be a solve per frame, and the
 * cheapest way to notice it here is that nothing else changed.
 *
 * NEEDS A TICKING WORLD: a world with real collision — the Destroy ray is a genuine line trace — but
 * it never ticks one.
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

	/* What every brick wears with nothing under the cursor — the states to come back to. */
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

	/* --- ONE: the cursor lands on the middle brick of the bottom course ---------------------- */

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

	/* --- TWO: and nothing else on the wall moved -------------------------------------------- */

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

	/* --- THREE: the cursor leaves, and the band is still underneath -------------------------- */

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
 * CLICKING THE CHIP AGAIN TAKES THE TINT OFF EVERY BRICK.
 *
 * A SETTING THAT CANNOT BE UNSET IS NOT A SETTING. The claim is not merely that the flag flips — the
 * model's own `Core.SessionToolbar.Transitions` pins that, purely — but that the WORLD follows it, and
 * the failure this catches is the obvious implementation: a refresh whose "off" arm does nothing at
 * all, leaving a wall tinted by a solve nobody can see the age of.
 *
 * NOTHING IS HOVERED OR SELECTED WHEN THE CHECK IS MADE, AND THAT IS ARRANGED RATHER THAN ASSUMED.
 * `None` is only the right answer for a brick with nothing else claiming it, so the cursor is taken
 * off the wall first; otherwise this would be asserting that turning the overlay off also drops the
 * hover, which is the opposite of the precedence the previous test pins.
 *
 * NEEDS A TICKING WORLD: a world, never ticked.
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

	/* Nothing may be pointed at when the check is made; None is only right for an unclaimed brick. */
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
 * WITH NOTHING BUILT THE CHIP IS GREYED AND THE CLICK IS REFUSED.
 *
 * THE SAME PRECONDITION `Run structure` AND `Clear build` HAVE, AND FOR THE SAME REASON. An overlay
 * over an empty plot latches on, colours exactly zero bricks and leaves the player looking for the
 * wall it lit — a silent no-op on a button, which is indistinguishable from the game having missed
 * the click.
 *
 * THE REFUSAL IS ASSERTED IN BOTH CURRENCIES: the strip greys the chip, AND the door says no. Two
 * separately-derived answers to "can this happen" is exactly how a lit button that does nothing gets
 * shipped, which is why `ApplyToolbarButton` asks the strip rather than re-deciding — and why this
 * checks that the controller's door inherited the same refusal.
 *
 * NEEDS A TICKING WORLD: a world for the controller and its subsystem, never ticked.
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
 * PULLING A BRICK OUT RECOMPUTES THE OVERLAY RATHER THAN LEAVING A PHOTOGRAPH BEHIND.
 *
 * THIS IS THE CLAIM THE WHOLE FEATURE IS FOR. The overlay's promise is "see where the load is, then
 * pull THAT one" — which is worth nothing if the picture does not move when the player pulls. A
 * refresh that ran only on the toggle would leave the wall coloured by the structure as it was before
 * the delete: a green brick standing over a hole, which is not merely stale but actively the wrong
 * advice about what to pull next.
 *
 * THE DELETE GOES THROUGH THE MENU ROW, which is the player's own route and the one that commits. The
 * cursor is taken off the wall afterwards so that `None` and the load states are the only two things
 * in play — `ChoosePieceMenuRow` dismisses the panel and clears the selection, so the hover is the
 * only other claimant left.
 *
 * WHAT IS ASSERTED IS THAT THE SURVIVORS STILL WEAR WHAT THE MODEL NOW SAYS, and that the removed
 * piece is genuinely gone. Not that any particular brick changed band: five bricks in a running bond
 * are comfortable with or without one of them, and demanding a visible change would be demanding a
 * physical result this fixture cannot honestly promise. What it CAN promise is that a wall recomputed
 * against the new structure is still a tinted wall rather than a blank one.
 *
 * NEEDS A TICKING WORLD: a world with real collision for the Destroy ray, never ticked.
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

	/* --- ONE: pull the left brick of the TOP course, which nothing else stands on ------------ */

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

	/* The cursor off the wall, so the hover is not a second claimant on any survivor. */
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

	/* --- TWO: and what is left is still tinted, against the structure as it NOW is ----------- */

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
 * A BRICK THAT HAS BEEN LET GO OF WEARS NO BAND AT ALL.
 *
 * =====================================================================================
 * THE BEHAVIOUR IN ONE SENTENCE
 * =====================================================================================
 *
 * A piece the settle RELEASED to physics is no longer part of the structure the overlay is describing,
 * so it draws plain — while every piece still standing keeps the band its worst joint says it should
 * have.
 *
 * =====================================================================================
 * WHY A RELEASED BRICK IS NOT A RED BRICK
 * =====================================================================================
 *
 * The overlay is an instrument for choosing what to pull next: it answers "how hard is this piece
 * working, and is it about to go". A released piece has already gone. It is a rigid body falling
 * through the air under Chaos, and its connections mean nothing about it any more — so the numbers
 * behind any band it could be given are a reading of a structure it left. `IsPieceRemoved` is already
 * treated this way (a hole wears nothing); `IsReleased` is the same fact one step earlier, and the two
 * differ only in whether the brick is still on screen.
 *
 * AND THE COLOUR IT WEARS TODAY IS THE WORST ONE THERE IS. A released free brick has no live joints
 * and no support, so both of the fail-closed arms answer `Critical` — which is exactly right as a
 * READING of a handle and is a lie as paint: a brick bouncing across the floor drawn in the same red
 * as the one about to fail under the wall. The whole point of the band is to pick out that second
 * brick, and a falling brick wearing the same colour is the instrument spending its loudest signal on
 * something the player can already see.
 *
 * =====================================================================================
 * WHAT IS ASSERTED
 * =====================================================================================
 *
 * `IsReleased` and `GetHighlight` — both binary, both mechanism. NEVER how far the brick fell: the
 * world is never ticked here, so the released brick has not moved a millimetre, and that is the point
 * rather than a limitation. Release is a fact about the binding, not about a distance.
 *
 * AND THE SURVIVORS ARE CHECKED IN THE SAME BREATH, because "released pieces wear None" is satisfied
 * completely by an overlay that has stopped working — five bricks drawn plain beside a released one
 * drawn plain. The five must still wear what the two pure functions say, and at least one of them must
 * wear a load state at all.
 *
 * NEEDS A TICKING WORLD: a world, with real collision for the Destroy ray. It is never ticked.
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

	/* --- ONE: one FREE brick in mid-air, three courses up, bonded to nothing ----------------- */

	/*
	 * THE SAME FIXTURE `Visual.SessionScreenshots` FRAME 4 USES, AND FOR THE SAME REASON: a Run over a
	 * build that already stands releases nothing, so a structure that CANNOT stand is the only thing
	 * that tells a Run which ran from a Run which did not. Free placement honours the cursor verbatim
	 * and forms no joint; 120 cm is in any case four snap radii from the nearest brick, so this piece
	 * is jointless for two independent reasons.
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

	/*
	 * THE THREE FIXTURE PRECONDITIONS, AND EVERY ONE OF THEM IS LOAD-BEARING. A brick that turned out
	 * to be grounded, or bonded to the wall, would STAND — and this file would then be asserting that
	 * an unreleased brick wears no band, which is the opposite of what the other four tests here pin.
	 */
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

	/* --- TWO: the overlay on, then Run ------------------------------------------------------- */

	if (!Controller.OnToolbarButton(EToolbarButtonId::ModeDestroy)
		|| !Controller.OnToolbarButton(EToolbarButtonId::ToggleLoadOverlay))
	{
		AddError(TEXT("fixture: Destroy is always live, and the overlay chip is live with a wall laid"));
		Fixture.End();
		return true;
	}

	/* Nothing may be pointed at when the check is made; None is only right for an unclaimed brick. */
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

	/* --- THREE: the released brick is plain, and the survivors are not ----------------------- */

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
 * LOOKING AT A SETTLED STRUCTURE READS THE SETTLE — IT DOES NOT RUN ANOTHER SOLVE OVER IT.
 *
 * =====================================================================================
 * THE BEHAVIOUR IN ONE SENTENCE
 * =====================================================================================
 *
 * Toggling the overlay on over a structure that has already been settled costs no solve and changes no
 * support verdict; toggling it on over a structure NOTHING has solved costs exactly one.
 *
 * =====================================================================================
 * WHY A SECOND SOLVE IS NOT MERELY WASTE
 * =====================================================================================
 *
 * A solve is documented as non-destructive and deterministic, which makes "it solved twice" invisible
 * in every reading of the graph but one — and that is exactly why `FStructure::NumSolves` exists. But
 * the overlay's second solve is not the same solve. A settle runs `SolveAndBreak`, whose equilibrium
 * gate calls `ApplyLimitAnalysisSupport` and OVERWRITES the router's per-piece support with the LP's
 * verdict below the block cap. A bare `SolveLoads` has no gate: it rebuilds the support arrays from the
 * router's downward flood alone. So on any structure where the two authorities disagree, looking at the
 * wall silently REPLACES the settled answer with a worse one — and `ApplyResults` releases on exactly
 * that answer.
 *
 * THE KNOT IS THAT DISAGREEMENT'S SHAPE — see `OverlayLayTheKnot` — BUT IT DOES NOT REACH IT TODAY, AND
 * THAT IS MEASURED RATHER THAN ASSUMED. A course spanning a void is where the router gives up (it
 * cannot divide load round the head-joint cycle) and the LP normally does not. Laid through the
 * SESSION, though, the head joints are perpends — `JointForContact` gives any horizontal masonry normal
 * `GeneralPurposeMortarPerpend`, 0.2 MPa cohesion and 0.1 MPa flexural bond — and a two-brick
 * cantilever's bending demand at the inner head joint is of the order of 0.17 MPa, comfortably past
 * that bond. So the LP falls the two outer bricks as well, and both authorities happen to agree:
 * `[0:Grounded, 1:Supported, 2:Falling, 3:Falling]` before the settle and after it.
 *
 * THE VERDICT CLAIM BELOW IS THEREFORE A GUARD, NOT THE DRIVER, and it is written down as such so that
 * nobody reads its green as evidence. What drives this test is the solve COUNT. The verdict claim earns
 * its place anyway: it is the reading that would change first if a re-solve ever did diverge, it costs
 * nothing, and it is what stops a "cache the answer" implementation leaving the support arrays stale or
 * empty. To make it bite, the fixture needs head joints the LP can carry two bricks on — a bonded
 * corner or a full-mortar head joint, neither of which the session's own inference can produce today
 * (CURRENT_STATE's corner-return slice). `Acceptance.SupportAuthority.*` pins the disagreement itself,
 * at Core level, on hand-laid `GeneralPurposeMortar`.
 *
 * =====================================================================================
 * WHAT IS ASSERTED, AND WHY THE THIRD CLAIM IS NOT OPTIONAL
 * =====================================================================================
 *
 * The solve COUNT, the support ENUMERATOR per piece, and that the wall is tinted at all. The third is
 * what stops the first two being satisfied by the cheapest possible implementation: an overlay that
 * refuses to solve AND refuses to paint costs no solve and disturbs no verdict, and is also no overlay.
 *
 * AND THE OTHER HALF IS PINNED IN THE SAME FILE: a FRESHLY LAID wall has never been solved, nothing has
 * answered for any of its pieces, and switching the overlay on there MUST solve — once. "Never solve"
 * is not the rule; "solve when there is no answer, read when there is" is.
 *
 * NEEDS A TICKING WORLD: a world, never ticked. The Run hands bodies to physics and nothing here waits
 * for them; every reading is of the binding and the graph.
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

	/* --- ONE: an unsolved structure DOES need a solve, and gets exactly one ------------------ */

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

	/* --- TWO: the overlay off, then the settle ----------------------------------------------- */

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

	/* --- THREE: what the settle decided, recorded before anything looks at it ---------------- */

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

	/* Nothing may be pointed at, so a hover cannot stand in for a band below. */
	Controller.PointerAlongRay(OverlayEmptyRayStart(), OverlayEmptyRayEnd());

	/* --- FOUR: and LOOKING at it changes neither the count nor a single verdict -------------- */

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

	/* --- FIVE: and it drew the bands anyway, which is what stops FOUR being free ------------- */

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
