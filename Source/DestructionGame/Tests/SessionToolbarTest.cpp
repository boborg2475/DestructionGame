// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#include "Core/BuildMode/DemoBuilding.h"
#include "Core/BuildMode/SnapSolver.h"
#include "Core/Layout.h"
#include "Core/Profiles/ConnectionProfiles.h"
#include "Core/Profiles/MaterialProfiles.h"
#include "Core/SessionToolbar.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Named, and uniquely: an anonymous namespace is private to a translation unit, not a file, and a
 * unity build merges files. See Tests/ConnectionLoadTest.cpp. The `using namespace` inside each
 * RunTest is for the same reason.
 */
namespace SessionToolbarTestSupport
{
	/** Brick course pitch, 7.5 cm (6.5 brick + 1.0 joint), transcribed not imported so a retune of the brick or joint fails here rather than agreeing with the plane function. */
	constexpr double BrickCoursePitchCm = 7.5;

	/** The brick's half height — half of FSnapSettings::BrickSizeCm.Z, the standard 6.5 cm unit. */
	constexpr double BrickHalfHeightCm = 3.25;

	/** The wall plate's half height, from the demo building's own plate extent (33.75, 5.125, 5.0). */
	constexpr double PlateHalfHeightCm = 5.0;

	/**
	 * Placement indices into the demo building's layout; heights are read off it, not transcribed,
	 * so the comparison stays live when the demo moves. Builder's order (Tests/DemoBuildingTest.cpp):
	 * 0-3 grounded course, 4-6 staggered course above, 7 the timber wall plate.
	 */
	constexpr int32 DemoCourseZeroPieceIndex = 0;
	constexpr int32 DemoCourseOnePieceIndex = 4;
	constexpr int32 DemoPlatePieceIndex = 7;

	const TCHAR* NameOfMode(DestructionSession::ESessionMode Mode)
	{
		switch (Mode)
		{
		case DestructionSession::ESessionMode::Build:   return TEXT("Build");
		case DestructionSession::ESessionMode::Destroy: return TEXT("Destroy");
		}

		return TEXT("<not a session mode>");
	}

	const TCHAR* NameOfPiece(DestructionSession::EBuildPieceKind Kind)
	{
		switch (Kind)
		{
		case DestructionSession::EBuildPieceKind::Brick:        return TEXT("Brick");
		case DestructionSession::EBuildPieceKind::TimberPlate:  return TEXT("TimberPlate");
		case DestructionSession::EBuildPieceKind::TimberLintel: return TEXT("TimberLintel");
		}

		return TEXT("<not a piece kind>");
	}

	const TCHAR* NameOfPlacement(DestructionSession::EPlacementMode Placement)
	{
		switch (Placement)
		{
		case DestructionSession::EPlacementMode::Snap: return TEXT("Snap");
		case DestructionSession::EPlacementMode::Free: return TEXT("Free");
		}

		return TEXT("<not a placement mode>");
	}

	const TCHAR* NameOfJoint(DestructionSession::EJointChoice Joint)
	{
		using namespace DestructionSession;

		switch (Joint)
		{
		case EJointChoice::Auto:   return TEXT("Auto");
		case EJointChoice::Mortar: return TEXT("Mortar");
		case EJointChoice::Dry:    return TEXT("Dry");
		case EJointChoice::Nail:   return TEXT("Nail");
		case EJointChoice::Screw:  return TEXT("Screw");
		case EJointChoice::Bolt:   return TEXT("Bolt");
		}

		return TEXT("<not a joint choice>");
	}

	const TCHAR* NameOfButton(DestructionSession::EToolbarButtonId Id)
	{
		using namespace DestructionSession;

		switch (Id)
		{
		case EToolbarButtonId::JointAuto:         return TEXT("JointAuto");
		case EToolbarButtonId::JointMortar:       return TEXT("JointMortar");
		case EToolbarButtonId::JointDry:          return TEXT("JointDry");
		case EToolbarButtonId::JointNail:         return TEXT("JointNail");
		case EToolbarButtonId::JointScrew:        return TEXT("JointScrew");
		case EToolbarButtonId::JointBolt:         return TEXT("JointBolt");
		case EToolbarButtonId::ModeBuild:         return TEXT("ModeBuild");
		case EToolbarButtonId::ModeDestroy:       return TEXT("ModeDestroy");
		case EToolbarButtonId::PieceBrick:        return TEXT("PieceBrick");
		case EToolbarButtonId::PieceTimberPlate:  return TEXT("PieceTimberPlate");
		case EToolbarButtonId::PieceTimberLintel: return TEXT("PieceTimberLintel");
		case EToolbarButtonId::RotatePiece:       return TEXT("RotatePiece");
		case EToolbarButtonId::PlacementSnap:     return TEXT("PlacementSnap");
		case EToolbarButtonId::PlacementFree:     return TEXT("PlacementFree");
		case EToolbarButtonId::CourseDown:        return TEXT("CourseDown");
		case EToolbarButtonId::CourseUp:          return TEXT("CourseUp");
		case EToolbarButtonId::ToggleLoadOverlay: return TEXT("ToggleLoadOverlay");
		case EToolbarButtonId::ClearBuild:        return TEXT("ClearBuild");
		case EToolbarButtonId::RunStructure:      return TEXT("RunStructure");
		}

		return TEXT("<not a toolbar button>");
	}

	const TCHAR* NameOfGroup(DestructionSession::EToolbarGroup Group)
	{
		switch (Group)
		{
		case DestructionSession::EToolbarGroup::Mode:     return TEXT("Mode");
		case DestructionSession::EToolbarGroup::Settings: return TEXT("Settings");
		case DestructionSession::EToolbarGroup::Command:  return TEXT("Command");
		}

		return TEXT("<not a toolbar group>");
	}

	const TCHAR* NameOfSwatch(DestructionSession::EToolbarSwatch Swatch)
	{
		switch (Swatch)
		{
		case DestructionSession::EToolbarSwatch::None:   return TEXT("None");
		case DestructionSession::EToolbarSwatch::Brick:  return TEXT("Brick");
		case DestructionSession::EToolbarSwatch::Timber: return TEXT("Timber");
		}

		return TEXT("<not a toolbar swatch>");
	}

	/**
	 * Which region of the strip each button belongs to, transcribed from SESSION_UI_DESIGN §b (mode
	 * tabs, settings, command) rather than read off the model it checks.
	 */
	DestructionSession::EToolbarGroup ExpectedGroupOf(DestructionSession::EToolbarButtonId Id)
	{
		using namespace DestructionSession;

		switch (Id)
		{
		case EToolbarButtonId::ModeBuild:
		case EToolbarButtonId::ModeDestroy:
			return EToolbarGroup::Mode;

		case EToolbarButtonId::PieceBrick:
		case EToolbarButtonId::PieceTimberPlate:
		case EToolbarButtonId::PieceTimberLintel:

		/* Rotate is a Setting: a property of the next placement, so it latches and sits before the command rule. */
		case EToolbarButtonId::RotatePiece:

		case EToolbarButtonId::PlacementSnap:
		case EToolbarButtonId::PlacementFree:

		/* The six joint chips are Settings too: a property of the next placement, so they latch. */
		case EToolbarButtonId::JointAuto:
		case EToolbarButtonId::JointMortar:
		case EToolbarButtonId::JointDry:
		case EToolbarButtonId::JointNail:
		case EToolbarButtonId::JointScrew:
		case EToolbarButtonId::JointBolt:

		case EToolbarButtonId::CourseDown:
		case EToolbarButtonId::CourseUp:

		/* The load overlay is a Setting, not a Command: it changes how the session looks, not the structure, so it sits before the command rule. */
		case EToolbarButtonId::ToggleLoadOverlay:
			return EToolbarGroup::Settings;

		case EToolbarButtonId::ClearBuild:
		case EToolbarButtonId::RunStructure:
			return EToolbarGroup::Command;
		}

		return EToolbarGroup::Command;
	}

	/** Which swatch a chip carries: the piece it lays, or none. */
	DestructionSession::EToolbarSwatch ExpectedSwatchOf(DestructionSession::EToolbarButtonId Id)
	{
		using namespace DestructionSession;

		switch (Id)
		{
		case EToolbarButtonId::PieceBrick:        return EToolbarSwatch::Brick;
		case EToolbarButtonId::PieceTimberPlate:  return EToolbarSwatch::Timber;
		case EToolbarButtonId::PieceTimberLintel: return EToolbarSwatch::Timber;
		default:                                  return EToolbarSwatch::None;
		}
	}

	/**
	 * The design's linear colours, transcribed from SESSION_UI_DESIGN §e (which gives each twice,
	 * linear and sRGB) so a production constant retuned away from the design fails here. Build amber
	 * and destroy red are the Caution and destructive colours; the two swatches are M_Shed_Brick and
	 * M_Shed_Timber's base colours (Scripts/Author-ShedMaterials.py).
	 */
	const FLinearColor DesignBuildAccent(0.95f, 0.66f, 0.13f, 1.0f);
	const FLinearColor DesignDestroyAccent(0.72f, 0.16f, 0.14f, 1.0f);
	const FLinearColor DesignBrickSwatch(0.35f, 0.06f, 0.04f, 1.0f);
	const FLinearColor DesignTimberSwatch(0.45f, 0.22f, 0.09f, 1.0f);

	/** Rounded chips with a drop edge — §a principle 1, "chunky rounded chips with a 2 px drop edge". */
	constexpr float DesignChipCornerRadiusPx = 10.0f;
	constexpr float DesignChipOutlineWidthPx = 2.0f;

	/**
	 * Alpha ceilings for a faded chip's fill and caption (§e). The number is loose; what matters is
	 * that a greyed chip reads as visibly not live.
	 */
	constexpr float DisabledFillAlphaCeiling = 0.35f;
	constexpr float DisabledCaptionAlphaCeiling = 0.5f;

	bool ColoursExactlyEqual(const FLinearColor& A, const FLinearColor& B)
	{
		return A.R == B.R && A.G == B.G && A.B == B.B && A.A == B.A;
	}

	bool ColoursExactlyEqualRGB(const FLinearColor& A, const FLinearColor& B)
	{
		return A.R == B.R && A.G == B.G && A.B == B.B;
	}

	FString DescribeColour(const FLinearColor& C)
	{
		return FString::Printf(TEXT("(%g, %g, %g, a %g)"), C.R, C.G, C.B, C.A);
	}

	/** Linear luminance of a colour. Captions are asserted as "dark" or "readable" bands, not exact triples, so a nudge to the ink is not a failure. */
	double LinearLuminance(const FLinearColor& C)
	{
		return 0.2126 * C.R + 0.7152 * C.G + 0.0722 * C.B;
	}

	FString DescribeLook(const DestructionSession::FChipLook& Look)
	{
		return FString::Printf(
			TEXT("fill %s, outline %s, caption %s, radius %g, edge %g, %s"),
			*DescribeColour(Look.Fill), *DescribeColour(Look.Outline), *DescribeColour(Look.Caption),
			Look.CornerRadiusPx, Look.OutlineWidthPx,
			Look.bBoldCaption ? TEXT("BOLD") : TEXT("regular"));
	}

	bool ColourIsFinite(const FLinearColor& C)
	{
		return FMath::IsFinite(C.R) && FMath::IsFinite(C.G) && FMath::IsFinite(C.B)
			&& FMath::IsFinite(C.A);
	}

	/** Every button the model knows, so a sweep covers the whole vocabulary. */
	TArray<DestructionSession::EToolbarButtonId> AllButtonIds()
	{
		using namespace DestructionSession;

		return {
			EToolbarButtonId::ModeBuild,
			EToolbarButtonId::ModeDestroy,
			EToolbarButtonId::PieceBrick,
			EToolbarButtonId::PieceTimberPlate,
			EToolbarButtonId::PieceTimberLintel,
			EToolbarButtonId::RotatePiece,
			EToolbarButtonId::PlacementSnap,
			EToolbarButtonId::PlacementFree,
			EToolbarButtonId::JointAuto,
			EToolbarButtonId::JointMortar,
			EToolbarButtonId::JointDry,
			EToolbarButtonId::JointNail,
			EToolbarButtonId::JointScrew,
			EToolbarButtonId::JointBolt,
			EToolbarButtonId::CourseDown,
			EToolbarButtonId::CourseUp,
			EToolbarButtonId::ToggleLoadOverlay,
			EToolbarButtonId::ClearBuild,
			EToolbarButtonId::RunStructure,
		};
	}

	/** Which choice each joint chip stands for, transcribed rather than read off the model it checks. */
	DestructionSession::EJointChoice JointChoiceOfButton(DestructionSession::EToolbarButtonId Id)
	{
		using namespace DestructionSession;

		switch (Id)
		{
		case EToolbarButtonId::JointMortar: return EJointChoice::Mortar;
		case EToolbarButtonId::JointDry:    return EJointChoice::Dry;
		case EToolbarButtonId::JointNail:   return EJointChoice::Nail;
		case EToolbarButtonId::JointScrew:  return EJointChoice::Screw;
		case EToolbarButtonId::JointBolt:   return EJointChoice::Bolt;
		default:                            return EJointChoice::Auto;
		}
	}

	/** Every joint choice the model knows, so a sweep covers the whole segmented control. */
	TArray<DestructionSession::EJointChoice> AllJointChoices()
	{
		using namespace DestructionSession;

		return {
			EJointChoice::Auto,
			EJointChoice::Mortar,
			EJointChoice::Dry,
			EJointChoice::Nail,
			EJointChoice::Screw,
			EJointChoice::Bolt,
		};
	}

	/** Which shipped connection profile an override points at, by address, never by value: equal-field profiles differ only in which library row a retune moves. */
	const TCHAR* NameOfConnectionProfileByAddress(const FConnectionStrength* Profile)
	{
		using namespace DestructionProfiles;

		if (Profile == nullptr)                          { return TEXT("<nullptr — inferred>"); }
		if (Profile == &GeneralPurposeMortar)            { return TEXT("GeneralPurposeMortar"); }
		if (Profile == &GeneralPurposeMortarPerpend)     { return TEXT("GeneralPurposeMortarPerpend"); }
		if (Profile == &LimeMortar)                      { return TEXT("LimeMortar"); }
		if (Profile == &DryStone)                        { return TEXT("DryStone"); }
		if (Profile == &Nail)                            { return TEXT("Nail"); }
		if (Profile == &Screw)                           { return TEXT("Screw"); }
		if (Profile == &Bolt)                            { return TEXT("Bolt"); }
		if (Profile == &Unbreakable)                     { return TEXT("Unbreakable"); }
		if (Profile == &CohesionlessBond)                { return TEXT("CohesionlessBond"); }

		return TEXT("<not a shipped library row>");
	}

	/** Whether an override points at a row of the shipped library at all, or at nothing. */
	bool IsAShippedConnectionProfileOrNull(const FConnectionStrength* Profile)
	{
		using namespace DestructionProfiles;

		return Profile == nullptr
			|| Profile == &GeneralPurposeMortar
			|| Profile == &GeneralPurposeMortarPerpend
			|| Profile == &LimeMortar
			|| Profile == &DryStone
			|| Profile == &Nail
			|| Profile == &Screw
			|| Profile == &Bolt
			|| Profile == &Unbreakable
			|| Profile == &CohesionlessBond;
	}

	FString DescribeState(const DestructionSession::FSessionToolbarState& State)
	{
		return FString::Printf(
			TEXT("{%s, %s, %s, joint %s, Course %d, %s, %s}"),
			NameOfMode(State.Mode),
			NameOfPiece(State.Piece),
			NameOfPlacement(State.Placement),
			NameOfJoint(State.Joint),
			State.Course,
			State.bHasStructure ? TEXT("has a structure") : TEXT("no structure"),
			State.bLoadOverlay ? TEXT("load overlay ON") : TEXT("load overlay off"));
	}

	FString DescribeButtons(const TArray<DestructionSession::FToolbarButton>& Buttons)
	{
		if (Buttons.Num() == 0)
		{
			return TEXT("<empty>");
		}

		FString Line;

		for (int32 Index = 0; Index < Buttons.Num(); ++Index)
		{
			Line += FString::Printf(
				TEXT("%s%s('%s'%s%s)"),
				Index == 0 ? TEXT("") : TEXT(", "),
				NameOfButton(Buttons[Index].Id),
				*Buttons[Index].Label,
				Buttons[Index].bActive ? TEXT(" active") : TEXT(""),
				Buttons[Index].bEnabled ? TEXT("") : TEXT(" DISABLED"));
		}

		return Line;
	}

	/**
	 * Compares field by field, not memcmp: padding is not required to be copied, so a memcmp would
	 * report differences intermittently. Adding a field without extending this helper is the only
	 * way a change can hide.
	 */
	bool StatesEqual(
		const DestructionSession::FSessionToolbarState& A,
		const DestructionSession::FSessionToolbarState& B)
	{
		return A.Mode == B.Mode
			&& A.Piece == B.Piece
			&& A.Placement == B.Placement
			&& A.Joint == B.Joint
			&& A.Course == B.Course
			&& A.bHasStructure == B.bHasStructure
			&& A.bLoadOverlay == B.bLoadOverlay
			&& A.bRotated == B.bRotated;
	}

	const DestructionSession::FToolbarButton* FindButton(
		const TArray<DestructionSession::FToolbarButton>& Buttons,
		DestructionSession::EToolbarButtonId Id)
	{
		for (const DestructionSession::FToolbarButton& Button : Buttons)
		{
			if (Button.Id == Id)
			{
				return &Button;
			}
		}

		return nullptr;
	}

	DestructionSession::FSessionToolbarState MakeState(
		DestructionSession::ESessionMode Mode,
		DestructionSession::EBuildPieceKind Piece,
		DestructionSession::EPlacementMode Placement,
		int32 Course,
		bool bHasStructure,
		bool bLoadOverlay = false,
		DestructionSession::EJointChoice Joint = DestructionSession::EJointChoice::Auto,
		bool bRotated = false)
	{
		DestructionSession::FSessionToolbarState State;
		State.Mode = Mode;
		State.Piece = Piece;
		State.Placement = Placement;
		State.Course = Course;
		State.bHasStructure = bHasStructure;
		State.bLoadOverlay = bLoadOverlay;
		State.Joint = Joint;
		State.bRotated = bRotated;
		return State;
	}

	/**
	 * The whole state space the flag rules are claimed over, not a hand-picked handful (DESIGN §4:
	 * an invariant over fixtures sharing a hidden property is not an invariant). 2 modes x 3 pieces
	 * x 2 placements x 3 courses x 2 structure flags x 2 overlay flags x 6 joint choices = 864
	 * states, milliseconds to sweep.
	 *
	 * Courses are 0 (grounded, CourseDown refused), 1 (first course it is offered) and 5 (clear of
	 * the boundary). The load overlay and joint choice are dimensions because each is drawn in one
	 * mode alone yet must survive a trip through the other: a model that reset either on the mode it
	 * is not drawn in would pass a sweep that only set it in the drawing mode. Six joint values, not
	 * two, because "exactly one of six is lit" cannot be checked on a control a boolean can light.
	 *
	 * bRotated is deliberately NOT a dimension: it would double 864 to 1728 for a flag that changes
	 * exactly one chip's bActive. Every state carries bRotated == false; the both-ways sweep lives in
	 * Core.SessionToolbar.RotateChip. Anybody adding a flag that moves the strip's shape adds it here.
	 */
	TArray<DestructionSession::FSessionToolbarState> AllStates()
	{
		using namespace DestructionSession;

		const ESessionMode Modes[] = { ESessionMode::Build, ESessionMode::Destroy };
		const EBuildPieceKind Pieces[] = {
			EBuildPieceKind::Brick, EBuildPieceKind::TimberPlate, EBuildPieceKind::TimberLintel };
		const EPlacementMode Placements[] = { EPlacementMode::Snap, EPlacementMode::Free };
		const int32 Courses[] = { 0, 1, 5 };
		const bool Structures[] = { false, true };
		const bool Overlays[] = { false, true };

		TArray<FSessionToolbarState> States;

		for (ESessionMode Mode : Modes)
		{
			for (EBuildPieceKind Piece : Pieces)
			{
				for (EPlacementMode Placement : Placements)
				{
					for (int32 Course : Courses)
					{
						for (bool bHasStructure : Structures)
						{
							for (bool bLoadOverlay : Overlays)
							{
								for (EJointChoice Joint : AllJointChoices())
								{
									States.Add(MakeState(
										Mode, Piece, Placement, Course, bHasStructure,
										bLoadOverlay, Joint));
								}
							}
						}
					}
				}
			}
		}

		return States;
	}

	bool VectorsExactlyEqual(const FVector& A, const FVector& B)
	{
		return A.X == B.X && A.Y == B.Y && A.Z == B.Z;
	}

	FString DescribeVector(const FVector& V)
	{
		return FString::Printf(TEXT("(%g, %g, %g)"), V.X, V.Y, V.Z);
	}

	/** Which shipped profile a returned reference IS, by address. Never a value comparison. */
	const TCHAR* NameOfMaterialByAddress(const DestructionProfiles::FMaterialProfile& Material)
	{
		if (&Material == &DestructionProfiles::ClayBrick)         { return TEXT("ClayBrick"); }
		if (&Material == &DestructionProfiles::Timber)            { return TEXT("Timber"); }
		if (&Material == &DestructionProfiles::StructuralConcrete) { return TEXT("StructuralConcrete"); }

		return TEXT("<not a shipped library row>");
	}

	bool IsAShippedProfile(const DestructionProfiles::FMaterialProfile& Material)
	{
		return &Material == &DestructionProfiles::ClayBrick
			|| &Material == &DestructionProfiles::Timber
			|| &Material == &DestructionProfiles::StructuralConcrete;
	}
}

/**
 * The toolbar draws one fixed list per mode, in one fixed order, mode pair always first.
 *
 * The list is a model answer, not a Slate layout: asserting widgets appeared needs a viewport and
 * cannot see a button offered in the wrong mode, a missing button, or the strip reordering under
 * the cursor. The mode pair being first is the one ordering claim with a player-facing reason — a
 * strip whose first slots shifted would move a button under a stationary cursor. Swept over the
 * whole state space because the list's content depends on the mode alone.
 *
 * Needs a ticking world: no.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarButtonsByModeTest,
	"DestructionGame.Core.SessionToolbar.ButtonsByMode",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarButtonsByModeTest::RunTest(const FString& Parameters)
{
	using namespace SessionToolbarTestSupport;
	using namespace DestructionSession;

	const TArray<EToolbarButtonId> ExpectedInBuild = {
		EToolbarButtonId::ModeBuild,
		EToolbarButtonId::ModeDestroy,
		EToolbarButtonId::PieceBrick,
		EToolbarButtonId::PieceTimberPlate,
		EToolbarButtonId::PieceTimberLintel,

		/* Rotate sits with the palette, after the three pieces and before Snap/Free: a modifier on the lit piece, not a fourth piece, and clear of the irreversible Clear build. */
		EToolbarButtonId::RotatePiece,

		EToolbarButtonId::PlacementSnap,
		EToolbarButtonId::PlacementFree,

		/*
		 * The joint chips sit after the placement pair and before the course stepper: both are
		 * segmented controls over the next placement. Auto is first because it is the default the
		 * session opens on.
		 */
		EToolbarButtonId::JointAuto,
		EToolbarButtonId::JointMortar,
		EToolbarButtonId::JointDry,
		EToolbarButtonId::JointNail,
		EToolbarButtonId::JointScrew,
		EToolbarButtonId::JointBolt,

		EToolbarButtonId::CourseDown,
		EToolbarButtonId::CourseUp,
		EToolbarButtonId::ClearBuild,
	};

	/*
	 * The Destroy strip's order (§b): the mode pair, then the load overlay setting, then Run
	 * structure past a rule. The toggle sits between the pair and Run, not beside it. It is drawn
	 * in Destroy alone; bLoadOverlay is swept both ways over both modes below.
	 */
	const TArray<EToolbarButtonId> ExpectedInDestroy = {
		EToolbarButtonId::ModeBuild,
		EToolbarButtonId::ModeDestroy,
		EToolbarButtonId::ToggleLoadOverlay,
		EToolbarButtonId::RunStructure,
	};

	for (const FSessionToolbarState& State : AllStates())
	{
		const TArray<FToolbarButton> Buttons = SessionToolbarButtons(State);
		const TArray<EToolbarButtonId>& Expected =
			State.Mode == ESessionMode::Build ? ExpectedInBuild : ExpectedInDestroy;

		TestEqual(
			FString::Printf(
				TEXT("%s: the toolbar must draw %d buttons, it drew %d — [%s]"),
				*DescribeState(State), Expected.Num(), Buttons.Num(), *DescribeButtons(Buttons)),
			Buttons.Num(), Expected.Num());

		const int32 Common = FMath::Min(Buttons.Num(), Expected.Num());

		for (int32 Index = 0; Index < Common; ++Index)
		{
			TestEqual(
				FString::Printf(
					TEXT("%s: slot %d must be %s, it is %s — [%s]"),
					*DescribeState(State), Index, NameOfButton(Expected[Index]),
					NameOfButton(Buttons[Index].Id), *DescribeButtons(Buttons)),
				static_cast<int32>(Buttons[Index].Id), static_cast<int32>(Expected[Index]));
		}

		/* No button appears twice: two slots with one id is a click that cannot be attributed, which the ordered comparison above can miss. */
		TSet<EToolbarButtonId> Seen;

		for (const FToolbarButton& Button : Buttons)
		{
			bool bAlreadyThere = false;
			Seen.Add(Button.Id, &bAlreadyThere);

			TestFalse(
				*FString::Printf(
					TEXT("%s: %s appears more than once — [%s]"),
					*DescribeState(State), NameOfButton(Button.Id), *DescribeButtons(Buttons)),
				bAlreadyThere);
		}
	}

	return true;
}

/**
 * Exactly one button in each setting group is lit, the one the state names; a command button is
 * never lit.
 *
 * bActive is "this is what you chose", not bEnabled: two lit piece buttons say the ghost is two
 * pieces, none lit says the last click did nothing. Commands are never active — a latched Clear
 * reads as a mode the player is stuck in.
 *
 * Needs a ticking world: no.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarActiveFlagsTest,
	"DestructionGame.Core.SessionToolbar.ActiveFlags",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarActiveFlagsTest::RunTest(const FString& Parameters)
{
	using namespace SessionToolbarTestSupport;
	using namespace DestructionSession;

	for (const FSessionToolbarState& State : AllStates())
	{
		const TArray<FToolbarButton> Buttons = SessionToolbarButtons(State);
		const bool bBuilding = State.Mode == ESessionMode::Build;

		int32 ActiveModeButtons = 0;
		int32 ActivePieceButtons = 0;
		int32 ActivePlacementButtons = 0;
		int32 ActiveJointButtons = 0;
		int32 JointButtonsSeen = 0;
		int32 LoadOverlayButtonsSeen = 0;
		int32 RotateButtonsSeen = 0;

		for (const FToolbarButton& Button : Buttons)
		{
			switch (Button.Id)
			{
			case EToolbarButtonId::ModeBuild:
				ActiveModeButtons += Button.bActive ? 1 : 0;
				TestEqual(
					*FString::Printf(
						TEXT("%s: ModeBuild is lit exactly when the session is in Build mode — [%s]"),
						*DescribeState(State), *DescribeButtons(Buttons)),
					Button.bActive, bBuilding);
				break;

			case EToolbarButtonId::ModeDestroy:
				ActiveModeButtons += Button.bActive ? 1 : 0;
				TestEqual(
					*FString::Printf(
						TEXT("%s: ModeDestroy is lit exactly when the session is in Destroy mode — [%s]"),
						*DescribeState(State), *DescribeButtons(Buttons)),
					Button.bActive, !bBuilding);
				break;

			case EToolbarButtonId::PieceBrick:
				ActivePieceButtons += Button.bActive ? 1 : 0;
				TestEqual(
					*FString::Printf(
						TEXT("%s: PieceBrick is lit exactly when Brick is the chosen piece — [%s]"),
						*DescribeState(State), *DescribeButtons(Buttons)),
					Button.bActive, State.Piece == EBuildPieceKind::Brick);
				break;

			case EToolbarButtonId::PieceTimberPlate:
				ActivePieceButtons += Button.bActive ? 1 : 0;
				TestEqual(
					*FString::Printf(
						TEXT("%s: PieceTimberPlate is lit exactly when TimberPlate is the chosen piece — [%s]"),
						*DescribeState(State), *DescribeButtons(Buttons)),
					Button.bActive, State.Piece == EBuildPieceKind::TimberPlate);
				break;

			case EToolbarButtonId::PieceTimberLintel:
				ActivePieceButtons += Button.bActive ? 1 : 0;
				TestEqual(
					*FString::Printf(
						TEXT("%s: PieceTimberLintel is lit exactly when TimberLintel is the chosen piece — [%s]"),
						*DescribeState(State), *DescribeButtons(Buttons)),
					Button.bActive, State.Piece == EBuildPieceKind::TimberLintel);
				break;

			case EToolbarButtonId::PlacementSnap:
				ActivePlacementButtons += Button.bActive ? 1 : 0;
				TestEqual(
					*FString::Printf(
						TEXT("%s: PlacementSnap is lit exactly when placement is Snap — [%s]"),
						*DescribeState(State), *DescribeButtons(Buttons)),
					Button.bActive, State.Placement == EPlacementMode::Snap);
				break;

			case EToolbarButtonId::PlacementFree:
				ActivePlacementButtons += Button.bActive ? 1 : 0;
				TestEqual(
					*FString::Printf(
						TEXT("%s: PlacementFree is lit exactly when placement is Free — [%s]"),
						*DescribeState(State), *DescribeButtons(Buttons)),
					Button.bActive, State.Placement == EPlacementMode::Free);
				break;

			case EToolbarButtonId::JointAuto:
			case EToolbarButtonId::JointMortar:
			case EToolbarButtonId::JointDry:
			case EToolbarButtonId::JointNail:
			case EToolbarButtonId::JointScrew:
			case EToolbarButtonId::JointBolt:
			{
				/* Exactly one of six is lit, the one the state names: this choice commits the physics of the next piece's joints, so it is not cosmetic. */
				++JointButtonsSeen;
				ActiveJointButtons += Button.bActive ? 1 : 0;

				const EJointChoice ChoiceOfThisChip = JointChoiceOfButton(Button.Id);

				TestEqual(
					*FString::Printf(
						TEXT("%s: %s is lit exactly when %s is the chosen joint — [%s]"),
						*DescribeState(State), NameOfButton(Button.Id),
						NameOfJoint(ChoiceOfThisChip), *DescribeButtons(Buttons)),
					Button.bActive, State.Joint == ChoiceOfThisChip);
				break;
			}

			case EToolbarButtonId::RotatePiece:
				/*
				 * Rotate latches: which way the next piece lies is a setting that stays until
				 * changed, so the chip must say so. Swept only at bRotated == false here (AllStates
				 * says why); Core.SessionToolbar.RotateChip sweeps both.
				 */
				++RotateButtonsSeen;
				TestEqual(
					*FString::Printf(
						TEXT("%s: RotatePiece is lit exactly when the next piece is rotated — [%s]"),
						*DescribeState(State), *DescribeButtons(Buttons)),
					Button.bActive, State.bRotated);
				break;

			case EToolbarButtonId::ToggleLoadOverlay:
				/* The overlay latches: it is a way of looking at the wall that stays on until turned off, so the chip must show its state. */
				++LoadOverlayButtonsSeen;
				TestEqual(
					*FString::Printf(
						TEXT("%s: ToggleLoadOverlay is lit exactly when the overlay is on — [%s]"),
						*DescribeState(State), *DescribeButtons(Buttons)),
					Button.bActive, State.bLoadOverlay);
				break;

			default:
				TestFalse(
					*FString::Printf(
						TEXT("%s: %s is a COMMAND and must never be lit — [%s]"),
						*DescribeState(State), NameOfButton(Button.Id), *DescribeButtons(Buttons)),
					Button.bActive);
				break;
			}
		}

		TestEqual(
			FString::Printf(
				TEXT("%s: exactly one of ModeBuild/ModeDestroy must be lit — [%s]"),
				*DescribeState(State), *DescribeButtons(Buttons)),
			ActiveModeButtons, 1);

		/* Piece and placement are not drawn in Destroy, so "exactly one lit" is a claim about the buttons that exist. */
		TestEqual(
			FString::Printf(
				TEXT("%s: exactly one Piece button must be lit when the group is drawn — [%s]"),
				*DescribeState(State), *DescribeButtons(Buttons)),
			ActivePieceButtons, bBuilding ? 1 : 0);

		TestEqual(
			FString::Printf(
				TEXT("%s: exactly one Placement button must be lit when the group is drawn — [%s]"),
				*DescribeState(State), *DescribeButtons(Buttons)),
			ActivePlacementButtons, bBuilding ? 1 : 0);

		/* All six joint chips or none, exactly one lit when drawn. The count floor stops this passing on a strip that drew Auto alone. */
		TestEqual(
			FString::Printf(
				TEXT("%s: the six joint chips must be drawn in Build mode and nowhere else — [%s]"),
				*DescribeState(State), *DescribeButtons(Buttons)),
			JointButtonsSeen, bBuilding ? 6 : 0);

		TestEqual(
			FString::Printf(
				TEXT("%s: exactly one Joint button must be lit when the group is drawn — [%s]"),
				*DescribeState(State), *DescribeButtons(Buttons)),
			ActiveJointButtons, bBuilding ? 1 : 0);

		/* The toggle was actually on the strip: the claim above loops over the buttons returned, so a strip that omitted the chip would satisfy it with nothing to check. */
		TestEqual(
			FString::Printf(
				TEXT("%s: the load overlay toggle must be on the strip in Destroy mode and nowhere else "
					 "— [%s]"),
				*DescribeState(State), *DescribeButtons(Buttons)),
			LoadOverlayButtonsSeen, bBuilding ? 0 : 1);

		/* The rotate chip was actually there: drawn in Build alone, since Destroy lays no next piece. */
		TestEqual(
			FString::Printf(
				TEXT("%s: the rotate chip must be on the strip in Build mode and nowhere else — [%s]"),
				*DescribeState(State), *DescribeButtons(Buttons)),
			RotateButtonsSeen, bBuilding ? 1 : 0);
	}

	return true;
}

/**
 * A button is greyed exactly when the thing behind it cannot happen: CourseDown on the ground
 * course, Clear and Run with nothing built.
 *
 * The model owns the greying because it owns the refusal — if the widget decided separately, the
 * two copies would disagree the day the floor moves and leave a lit button that does nothing.
 * bHasStructure is the only precondition either command has; everything else is always enabled,
 * asserted rather than assumed (a greyed mode button traps the player in a mode).
 *
 * Needs a ticking world: no.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarEnabledFlagsTest,
	"DestructionGame.Core.SessionToolbar.EnabledFlags",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarEnabledFlagsTest::RunTest(const FString& Parameters)
{
	using namespace SessionToolbarTestSupport;
	using namespace DestructionSession;

	for (const FSessionToolbarState& State : AllStates())
	{
		const TArray<FToolbarButton> Buttons = SessionToolbarButtons(State);

		/*
		 * The floor: everything below sweeps over the buttons returned, so a model returning nothing
		 * passes every claim with nothing to check (measured: a stub returning {} reported Success
		 * until this block was added). The named buttons are the only ones with a precondition.
		 */
		const bool bBuilding = State.Mode == ESessionMode::Build;

		TestTrue(
			*FString::Printf(
				TEXT("%s: the toolbar must draw buttons at all — [%s]"),
				*DescribeState(State), *DescribeButtons(Buttons)),
			Buttons.Num() > 0);

		TestTrue(
			*FString::Printf(
				TEXT("%s: %s must be on the strip for its enablement to mean anything — [%s]"),
				*DescribeState(State),
				bBuilding ? TEXT("CourseDown and ClearBuild")
					: TEXT("RunStructure and ToggleLoadOverlay"),
				*DescribeButtons(Buttons)),
			bBuilding
				? (FindButton(Buttons, EToolbarButtonId::CourseDown) != nullptr
					&& FindButton(Buttons, EToolbarButtonId::ClearBuild) != nullptr)
				: (FindButton(Buttons, EToolbarButtonId::RunStructure) != nullptr
					&& FindButton(Buttons, EToolbarButtonId::ToggleLoadOverlay) != nullptr));

		for (const FToolbarButton& Button : Buttons)
		{
			bool bExpectedEnabled = true;
			const TCHAR* Why = TEXT("it has no precondition at all");

			switch (Button.Id)
			{
			case EToolbarButtonId::CourseDown:
				bExpectedEnabled = State.Course >= 1;
				Why = TEXT("there is no course below the grounded one");
				break;

			case EToolbarButtonId::ClearBuild:
			case EToolbarButtonId::RunStructure:
				bExpectedEnabled = State.bHasStructure;
				Why = TEXT("it acts on a live structure and there must be one");
				break;

			case EToolbarButtonId::ToggleLoadOverlay:
				/* Same precondition: the overlay solves and tints the structure, so with nothing built a live chip would latch on and colour nothing. */
				bExpectedEnabled = State.bHasStructure;
				Why = TEXT("it tints a live structure and there must be one");
				break;

			default:
				break;
			}

			TestEqual(
				*FString::Printf(
					TEXT("%s: %s must be %s (%s), it is %s — [%s]"),
					*DescribeState(State), NameOfButton(Button.Id),
					bExpectedEnabled ? TEXT("ENABLED") : TEXT("DISABLED"), Why,
					Button.bEnabled ? TEXT("enabled") : TEXT("disabled"), *DescribeButtons(Buttons)),
				Button.bEnabled, bExpectedEnabled);
		}
	}

	return true;
}

/**
 * Every button carries a caption, no two on one strip read the same, and the five a player
 * navigates by contain the word they navigate by.
 *
 * The caption is data on the row (as FPieceMenuRow::Label is), because a widget spelling its own
 * text holds load-bearing wording where no test can reach — Snap vs Free is the difference between
 * a piece on the bond and one at the cursor. Wording is bounded, not pinned: what may not drift is
 * that a button contains the word a player hunts by, and that no two on a strip match. Distinctness
 * is within one strip (ModeBuild and ClearBuild may both say "Build").
 *
 * Needs a ticking world: no.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarLabelsTest,
	"DestructionGame.Core.SessionToolbar.Labels",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarLabelsTest::RunTest(const FString& Parameters)
{
	using namespace SessionToolbarTestSupport;
	using namespace DestructionSession;

	struct FWordCase
	{
		EToolbarButtonId Id;
		const TCHAR* Word;
	};

	const FWordCase RequiredWords[] = {
		{ EToolbarButtonId::ModeBuild,     TEXT("Build") },
		{ EToolbarButtonId::ModeDestroy,   TEXT("Destroy") },
		{ EToolbarButtonId::PlacementSnap, TEXT("Snap") },
		{ EToolbarButtonId::PlacementFree, TEXT("Free") },
		{ EToolbarButtonId::RunStructure,  TEXT("Run") },

		/* The word is "Load"; the rest is the widget's. Pinning the whole caption would fail on a harmless retune. */
		{ EToolbarButtonId::ToggleLoadOverlay, TEXT("Load") },

		/*
		 * The six joint chips name their fastener; the word is the claim, the rest the widget's. This
		 * control commits physics, so a chip must be identifiable without clicking. "Dry" not
		 * "DryStone": one slot of six on a 48 px bar, and both spellings contain the word.
		 */
		{ EToolbarButtonId::JointAuto,   TEXT("Auto") },
		{ EToolbarButtonId::JointMortar, TEXT("Mortar") },
		{ EToolbarButtonId::JointDry,    TEXT("Dry") },
		{ EToolbarButtonId::JointNail,   TEXT("Nail") },
		{ EToolbarButtonId::JointScrew,  TEXT("Screw") },
		{ EToolbarButtonId::JointBolt,   TEXT("Bolt") },

		/* The rotate chip says "Rotate"; the word is the claim, the rest the widget's. It is the only way to lay a header. */
		{ EToolbarButtonId::RotatePiece, TEXT("Rotate") },
	};

	/*
	 * How many word checks the sweep owes, accumulated from the mode and settled at the end — the
	 * same floor EnabledFlags carries: every claim below loops over the buttons returned, so a model
	 * returning nothing passes them all. Accumulated, not a literal, so it tracks AllStates().
	 */
	int32 WordChecksOwed = 0;
	int32 WordChecksMade = 0;

	for (const FSessionToolbarState& State : AllStates())
	{
		const TArray<FToolbarButton> Buttons = SessionToolbarButtons(State);

		/*
		 * Eleven in Build (mode pair, Snap, Free, six joint chips, Rotate), four in Destroy (mode
		 * pair, Run, Load). The mode pair is the only rows both strips share.
		 */
		WordChecksOwed += State.Mode == ESessionMode::Build
			? 2 + 2 + 6 + 1   /* the mode pair, Snap and Free, the six joint chips, then Rotate */
			: 2 + 2;          /* the mode pair, then Run structure and Load overlay */

		TestTrue(
			*FString::Printf(
				TEXT("%s: the toolbar must draw buttons at all — [%s]"),
				*DescribeState(State), *DescribeButtons(Buttons)),
			Buttons.Num() > 0);

		TSet<FString> SeenLabels;

		for (const FToolbarButton& Button : Buttons)
		{
			TestFalse(
				*FString::Printf(
					TEXT("%s: %s must carry a caption, it is empty — [%s]"),
					*DescribeState(State), NameOfButton(Button.Id), *DescribeButtons(Buttons)),
				Button.Label.IsEmpty());

			bool bAlreadyThere = false;
			SeenLabels.Add(Button.Label, &bAlreadyThere);

			TestFalse(
				*FString::Printf(
					TEXT("%s: two buttons on one strip both read '%s' — [%s]"),
					*DescribeState(State), *Button.Label, *DescribeButtons(Buttons)),
				bAlreadyThere);
		}

		for (const FWordCase& Case : RequiredWords)
		{
			const FToolbarButton* Button = FindButton(Buttons, Case.Id);

			if (Button == nullptr)
			{
				// Not drawn in this mode; the list test owns which modes draw what.
				continue;
			}

			++WordChecksMade;

			TestTrue(
				*FString::Printf(
					TEXT("%s: %s's caption must contain '%s', it reads '%s'"),
					*DescribeState(State), NameOfButton(Case.Id), Case.Word, *Button->Label),
				Button->Label.Contains(Case.Word));
		}
	}

	TestEqual(
		FString::Printf(
			TEXT("the sweep must actually have read %d captions, it read %d — a strip that drew none would pass every claim above by having nothing to check"),
			WordChecksOwed, WordChecksMade),
		WordChecksMade, WordChecksOwed);

	return true;
}

/**
 * One click changes one thing and leaves everything else alone; a click on a greyed or off-screen
 * button changes nothing.
 *
 * The transition is a pure function because that is the only way it is assertable — a controller
 * mutating its own fields from a Slate callback puts the behaviour behind a click only a human can
 * perform. The rows start from non-default piece, placement and course so a transition that
 * rebuilt the state from scratch fails rather than agreeing with a default expectation. The course
 * floor is the fail-closed row: a negative course is a plane under the ground, so greyed CourseDown
 * is a bitwise no-op, not a clamp that lands on the same number. The last block holds the two
 * functions against each other: whatever SessionToolbarButtons will not draw or draws greyed,
 * ApplyToolbarButton must refuse to act on.
 *
 * Needs a ticking world: no.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarTransitionsTest,
	"DestructionGame.Core.SessionToolbar.Transitions",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarTransitionsTest::RunTest(const FString& Parameters)
{
	using namespace SessionToolbarTestSupport;
	using namespace DestructionSession;

	struct FTransitionCase
	{
		const TCHAR* Description;
		FSessionToolbarState Before;
		EToolbarButtonId Click;
		FSessionToolbarState Expected;
	};

	const FTransitionCase Cases[] = {
		{
			TEXT("Destroy takes the session into destroy mode and keeps every build setting for the way back"),
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberLintel, EPlacementMode::Free, 3, true),
			EToolbarButtonId::ModeDestroy,
			MakeState(ESessionMode::Destroy, EBuildPieceKind::TimberLintel, EPlacementMode::Free, 3, true),
		},
		{
			TEXT("Build takes it back, and the settings are still the ones it left with"),
			MakeState(ESessionMode::Destroy, EBuildPieceKind::TimberLintel, EPlacementMode::Free, 3, true),
			EToolbarButtonId::ModeBuild,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberLintel, EPlacementMode::Free, 3, true),
		},
		{
			TEXT("clicking the mode you are already in changes nothing"),
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 2, true),
			EToolbarButtonId::ModeBuild,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 2, true),
		},
		{
			TEXT("choosing the plate changes the piece and nothing else"),
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Free, 4, true),
			EToolbarButtonId::PieceTimberPlate,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 4, true),
		},
		{
			TEXT("choosing the lintel changes the piece and nothing else"),
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Snap, 4, false),
			EToolbarButtonId::PieceTimberLintel,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberLintel, EPlacementMode::Snap, 4, false),
		},
		{
			TEXT("and choosing the brick back again"),
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberLintel, EPlacementMode::Free, 1, true),
			EToolbarButtonId::PieceBrick,
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Free, 1, true),
		},
		{
			TEXT("Free placement changes the placement and nothing else"),
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Snap, 2, true),
			EToolbarButtonId::PlacementFree,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 2, true),
		},
		{
			TEXT("Snap placement changes the placement and nothing else"),
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 2, true),
			EToolbarButtonId::PlacementSnap,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Snap, 2, true),
		},
		{
			TEXT("up a course from the ground keeps the piece and the placement"),
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 0, true),
			EToolbarButtonId::CourseUp,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 1, true),
		},
		{
			TEXT("down a course from two"),
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberLintel, EPlacementMode::Snap, 2, false),
			EToolbarButtonId::CourseDown,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberLintel, EPlacementMode::Snap, 1, false),
		},
		{
			TEXT("down a course from one lands on the grounded course"),
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Snap, 1, false),
			EToolbarButtonId::CourseDown,
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Snap, 0, false),
		},
		{
			TEXT("THE FLOOR: down a course from the grounded one is refused outright, not clamped into a new state"),
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 0, true),
			EToolbarButtonId::CourseDown,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 0, true),
		},
		{
			TEXT("Clear is a command the controller runs; the toolbar's own state is untouched by it"),
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 3, true),
			EToolbarButtonId::ClearBuild,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 3, true),
		},
		{
			TEXT("and Clear with nothing built is the same untouched state"),
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Snap, 3, false),
			EToolbarButtonId::ClearBuild,
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Snap, 3, false),
		},
		{
			TEXT("Run is a command too, and in Destroy mode it still changes no state"),
			MakeState(ESessionMode::Destroy, EBuildPieceKind::TimberLintel, EPlacementMode::Free, 5, true),
			EToolbarButtonId::RunStructure,
			MakeState(ESessionMode::Destroy, EBuildPieceKind::TimberLintel, EPlacementMode::Free, 5, true),
		},
		{
			TEXT("Run is not on the strip in Build mode, so a click carrying it does nothing"),
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Snap, 2, true),
			EToolbarButtonId::RunStructure,
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Snap, 2, true),
		},
		{
			TEXT("a piece button is not on the strip in Destroy mode, so it must not quietly change the piece"),
			MakeState(ESessionMode::Destroy, EBuildPieceKind::Brick, EPlacementMode::Snap, 2, true),
			EToolbarButtonId::PieceTimberPlate,
			MakeState(ESessionMode::Destroy, EBuildPieceKind::Brick, EPlacementMode::Snap, 2, true),
		},
		{
			TEXT("nor a placement button"),
			MakeState(ESessionMode::Destroy, EBuildPieceKind::Brick, EPlacementMode::Snap, 2, true),
			EToolbarButtonId::PlacementFree,
			MakeState(ESessionMode::Destroy, EBuildPieceKind::Brick, EPlacementMode::Snap, 2, true),
		},
		{
			TEXT("nor the course buttons, in either direction"),
			MakeState(ESessionMode::Destroy, EBuildPieceKind::Brick, EPlacementMode::Snap, 3, true),
			EToolbarButtonId::CourseUp,
			MakeState(ESessionMode::Destroy, EBuildPieceKind::Brick, EPlacementMode::Snap, 3, true),
		},
		{
			TEXT("nor CourseDown, which would otherwise be perfectly legal at course 3"),
			MakeState(ESessionMode::Destroy, EBuildPieceKind::Brick, EPlacementMode::Snap, 3, true),
			EToolbarButtonId::CourseDown,
			MakeState(ESessionMode::Destroy, EBuildPieceKind::Brick, EPlacementMode::Snap, 3, true),
		},

		/* --- The load overlay: a setting, so it latches and survives a mode change ------------- */

		{
			TEXT("the load overlay goes on, and changes nothing else about the session"),
			MakeState(ESessionMode::Destroy, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 4, true, false),
			EToolbarButtonId::ToggleLoadOverlay,
			MakeState(ESessionMode::Destroy, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 4, true, true),
		},
		{
			TEXT("and the same click takes it off again — a toggle, not a latch that only latches"),
			MakeState(ESessionMode::Destroy, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 4, true, true),
			EToolbarButtonId::ToggleLoadOverlay,
			MakeState(ESessionMode::Destroy, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 4, true, false),
		},
		{
			TEXT("with nothing built the chip is greyed, so the click is a bitwise no-op"),
			MakeState(ESessionMode::Destroy, EBuildPieceKind::Brick, EPlacementMode::Snap, 2, false, false),
			EToolbarButtonId::ToggleLoadOverlay,
			MakeState(ESessionMode::Destroy, EBuildPieceKind::Brick, EPlacementMode::Snap, 2, false, false),
		},
		{
			TEXT("and a greyed chip cannot turn an overlay OFF either, which is the sharper half"),
			MakeState(ESessionMode::Destroy, EBuildPieceKind::Brick, EPlacementMode::Snap, 2, false, true),
			EToolbarButtonId::ToggleLoadOverlay,
			MakeState(ESessionMode::Destroy, EBuildPieceKind::Brick, EPlacementMode::Snap, 2, false, true),
		},
		{
			TEXT("the chip is not on the Build strip, so a click carrying it must not quietly toggle"),
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Snap, 2, true, false),
			EToolbarButtonId::ToggleLoadOverlay,
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Snap, 2, true, false),
		},
		{
			/* The preservation row: Build takes the chip off the strip, but the player's choice must survive, as the piece, placement and course do. */
			TEXT("switching to Build does NOT clear a load overlay the player turned on"),
			MakeState(ESessionMode::Destroy, EBuildPieceKind::TimberLintel, EPlacementMode::Free, 3, true, true),
			EToolbarButtonId::ModeBuild,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberLintel, EPlacementMode::Free, 3, true, true),
		},
		{
			TEXT("and coming back to Destroy finds it still on"),
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberLintel, EPlacementMode::Free, 3, true, true),
			EToolbarButtonId::ModeDestroy,
			MakeState(ESessionMode::Destroy, EBuildPieceKind::TimberLintel, EPlacementMode::Free, 3, true, true),
		},
		{
			TEXT("a Build-mode setting click leaves the overlay flag alone as well"),
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Snap, 1, true, true),
			EToolbarButtonId::PieceTimberPlate,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Snap, 1, true, true),
		},
		{
			TEXT("and so does Run, which is a command and touches no state at all"),
			MakeState(ESessionMode::Destroy, EBuildPieceKind::Brick, EPlacementMode::Snap, 1, true, true),
			EToolbarButtonId::RunStructure,
			MakeState(ESessionMode::Destroy, EBuildPieceKind::Brick, EPlacementMode::Snap, 1, true, true),
		},

		/* --- The joint choice: a segmented setting, so it latches and survives a mode change ---- */

		{
			TEXT("choosing Screw changes the joint and nothing else"),
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 3, true,
				true, EJointChoice::Auto),
			EToolbarButtonId::JointScrew,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 3, true,
				true, EJointChoice::Screw),
		},
		{
			TEXT("and Dry after it — one chip of a segmented control REPLACES the choice, never adds to it"),
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 3, true,
				true, EJointChoice::Screw),
			EToolbarButtonId::JointDry,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 3, true,
				true, EJointChoice::Dry),
		},
		{
			TEXT("Mortar, from a fastener"),
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Snap, 1, false,
				false, EJointChoice::Bolt),
			EToolbarButtonId::JointMortar,
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Snap, 1, false,
				false, EJointChoice::Mortar),
		},
		{
			TEXT("Nail"),
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberLintel, EPlacementMode::Snap, 2, true,
				false, EJointChoice::Mortar),
			EToolbarButtonId::JointNail,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberLintel, EPlacementMode::Snap, 2, true,
				false, EJointChoice::Nail),
		},
		{
			TEXT("Bolt"),
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberLintel, EPlacementMode::Snap, 2, true,
				false, EJointChoice::Nail),
			EToolbarButtonId::JointBolt,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberLintel, EPlacementMode::Snap, 2, true,
				false, EJointChoice::Bolt),
		},
		{
			/* Back to Auto, which must be reachable: it hands the joint back to BuildMode::JointForContact, the only way to get an ordinary bedded brick after screwing one. */
			TEXT("and back to Auto, which hands the joint back to the inference"),
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Free, 4, true,
				true, EJointChoice::Bolt),
			EToolbarButtonId::JointAuto,
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Free, 4, true,
				true, EJointChoice::Auto),
		},
		{
			TEXT("clicking the joint you already have changes nothing"),
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Snap, 2, true,
				false, EJointChoice::Screw),
			EToolbarButtonId::JointScrew,
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Snap, 2, true,
				false, EJointChoice::Screw),
		},
		{
			TEXT("the joint chips are not on the Destroy strip, so a click carrying one must not change it"),
			MakeState(ESessionMode::Destroy, EBuildPieceKind::Brick, EPlacementMode::Snap, 2, true,
				false, EJointChoice::Screw),
			EToolbarButtonId::JointDry,
			MakeState(ESessionMode::Destroy, EBuildPieceKind::Brick, EPlacementMode::Snap, 2, true,
				false, EJointChoice::Screw),
		},
		{
			/* The preservation pair: Destroy takes the six chips off the strip, but a reset to Auto would dry-bed the next plate the player asked to screw, invisible until the structure runs. */
			TEXT("switching to Destroy does NOT clear the joint the player chose"),
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 3, true,
				false, EJointChoice::Screw),
			EToolbarButtonId::ModeDestroy,
			MakeState(ESessionMode::Destroy, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 3, true,
				false, EJointChoice::Screw),
		},
		{
			TEXT("and coming back to Build finds it still screwed"),
			MakeState(ESessionMode::Destroy, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 3, true,
				false, EJointChoice::Screw),
			EToolbarButtonId::ModeBuild,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberPlate, EPlacementMode::Free, 3, true,
				false, EJointChoice::Screw),
		},
		{
			TEXT("and every other Build-mode setting click leaves the joint alone"),
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Snap, 1, true,
				false, EJointChoice::Bolt),
			EToolbarButtonId::PieceTimberLintel,
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberLintel, EPlacementMode::Snap, 1, true,
				false, EJointChoice::Bolt),
		},
		{
			TEXT("including the course stepper"),
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Snap, 1, true,
				false, EJointChoice::Dry),
			EToolbarButtonId::CourseUp,
			MakeState(ESessionMode::Build, EBuildPieceKind::Brick, EPlacementMode::Snap, 2, true,
				false, EJointChoice::Dry),
		},
	};

	for (const FTransitionCase& Case : Cases)
	{
		const FSessionToolbarState After = ApplyToolbarButton(Case.Before, Case.Click);

		TestTrue(
			*FString::Printf(
				TEXT("%s: %s + %s must give %s, it gave %s"),
				Case.Description, *DescribeState(Case.Before), NameOfButton(Case.Click),
				*DescribeState(Case.Expected), *DescribeState(After)),
			StatesEqual(After, Case.Expected));
	}

	/*
	 * The two functions held against each other, over every state and button (864 x 18 = 15,552
	 * clicks). One-directional: an absent or greyed button must leave the state alone; the table
	 * above owns the lit-and-enabled ones.
	 */
	for (const FSessionToolbarState& State : AllStates())
	{
		const TArray<FToolbarButton> Buttons = SessionToolbarButtons(State);

		for (EToolbarButtonId Id : AllButtonIds())
		{
			const FToolbarButton* Drawn = FindButton(Buttons, Id);
			const bool bActionable = Drawn != nullptr && Drawn->bEnabled;
			const FSessionToolbarState After = ApplyToolbarButton(State, Id);

			if (!bActionable)
			{
				TestTrue(
					*FString::Printf(
						TEXT("%s: %s is %s, so clicking it must change nothing — it gave %s"),
						*DescribeState(State), NameOfButton(Id),
						Drawn == nullptr ? TEXT("not on the strip") : TEXT("greyed out"),
						*DescribeState(After)),
					StatesEqual(After, State));
			}

			/* No click may produce a negative course: this is the only door into the state, and a below-ground plane is nonsense. */
			TestTrue(
				*FString::Printf(
					TEXT("%s: %s must never leave the course negative, it gave %s"),
					*DescribeState(State), NameOfButton(Id), *DescribeState(After)),
				After.Course >= 0);
		}
	}

	return true;
}

/**
 * Each piece kind is one half-extent and one material, both the ones the already-built demo uses.
 *
 * The sizes are not new geometry: the brick is the standard 21.5 x 10.25 x 6.5 unit halved, the
 * plate is DemoBuilding.cpp's wall plate (33.75, 5.125, 5.0), the lintel its 90 cm sibling.
 * Pinning them here stops the toolbar being a third place brick dimensions live. The material is
 * compared by address, never value: a by-value match would pass against a stale private copy of
 * Timber. The unknown-kind row is fail-closed: whatever an undeclared kind gets, it must be a
 * real extent (no NaN or negative mass) and a real library row, not which one.
 *
 * Needs a ticking world: no.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarPaletteTest,
	"DestructionGame.Core.SessionToolbar.Palette",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarPaletteTest::RunTest(const FString& Parameters)
{
	using namespace SessionToolbarTestSupport;
	using namespace DestructionSession;

	struct FPaletteCase
	{
		const TCHAR* Description;
		EBuildPieceKind Kind;
		FVector ExpectedHalfExtentCm;
		const DestructionProfiles::FMaterialProfile* ExpectedMaterial;
	};

	const FPaletteCase Cases[] = {
		{
			TEXT("the standard 21.5 x 10.25 x 6.5 cm clay brick, halved"),
			EBuildPieceKind::Brick,
			FVector(10.75, 5.125, 3.25),
			&DestructionProfiles::ClayBrick,
		},
		{
			TEXT("the demo building's own 67.5 cm timber wall plate"),
			EBuildPieceKind::TimberPlate,
			FVector(33.75, 5.125, 5.0),
			&DestructionProfiles::Timber,
		},
		{
			TEXT("a 90 cm timber lintel — the plate's longer sibling, for spanning an opening"),
			EBuildPieceKind::TimberLintel,
			FVector(45.0, 5.125, 5.0),
			&DestructionProfiles::Timber,
		},
	};

	for (const FPaletteCase& Case : Cases)
	{
		const FVector HalfExtentCm = BuildPieceHalfExtentCm(Case.Kind);

		TestTrue(
			*FString::Printf(
				TEXT("%s (%s): the half extent must be %s cm, it is %s cm"),
				Case.Description, NameOfPiece(Case.Kind),
				*DescribeVector(Case.ExpectedHalfExtentCm), *DescribeVector(HalfExtentCm)),
			VectorsExactlyEqual(HalfExtentCm, Case.ExpectedHalfExtentCm));

		const DestructionProfiles::FMaterialProfile& Material = BuildPieceMaterial(Case.Kind);

		TestTrue(
			*FString::Printf(
				TEXT("%s (%s): the material must BE the library's %s row, by address — it is %s"),
				Case.Description, NameOfPiece(Case.Kind),
				NameOfMaterialByAddress(*Case.ExpectedMaterial), NameOfMaterialByAddress(Material)),
			&Material == Case.ExpectedMaterial);
	}

	/* An undeclared kind: neither answer may be garbage — a NaN extent becomes a NaN mass, and a non-library reference is a dangling read. */
	const EBuildPieceKind UnknownKind = static_cast<EBuildPieceKind>(200);
	const FVector UnknownExtentCm = BuildPieceHalfExtentCm(UnknownKind);

	TestTrue(
		*FString::Printf(
			TEXT("a piece kind this build has never heard of must still answer with a real extent, it answered %s cm"),
			*DescribeVector(UnknownExtentCm)),
		UnknownExtentCm.IsZero()
			|| (!UnknownExtentCm.ContainsNaN()
				&& UnknownExtentCm.X > 0.0 && UnknownExtentCm.Y > 0.0 && UnknownExtentCm.Z > 0.0));

	TestTrue(
		*FString::Printf(
			TEXT("a piece kind this build has never heard of must answer with a shipped library row, it answered %s"),
			NameOfMaterialByAddress(BuildPieceMaterial(UnknownKind))),
		IsAShippedProfile(BuildPieceMaterial(UnknownKind)));

	return true;
}

/**
 * A course's build plane puts the piece on top of the ground, not half in it: course N's bottom
 * is N brick courses up, and the plane is the piece's own half-height above that.
 *
 * This is a convention change: earlier harnesses centre course 0 at Z = 0, half below the ground.
 * A player's first brick must sit ON the ground, so a brick on course 0 answers 3.25, not 0. The
 * structure is unchanged — the solver reads relative positions, so this is the same building
 * lifted one brick half-height. The pitch is the brick's whatever the piece is: 7.5 cm is 6.5
 * brick plus a 1 cm bed, and a course is a property of the wall, not the thing laid into it. The
 * test spells 7.5 out and also derives it from a default FSnapSettings, so a retune fails on a row
 * that names it (DESIGN §3). A negative course is treated as course 0, fail-closed.
 *
 * Needs a ticking world: no.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarCoursePlaneZTest,
	"DestructionGame.Core.SessionToolbar.CoursePlaneZ",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarCoursePlaneZTest::RunTest(const FString& Parameters)
{
	using namespace SessionToolbarTestSupport;
	using namespace DestructionSession;

	/* The pitch, from the snap settings the solver uses. Asserted before the heights below, which are stale if the brick or joint was retuned. */
	const BuildMode::FSnapSettings Settings;

	TestEqual(
		FString::Printf(
			TEXT("the brick course pitch must be %g cm (%g of brick + %g of joint), the settings give %g"),
			BrickCoursePitchCm, Settings.BrickSizeCm.Z, Settings.JointThicknessCm,
			Settings.BrickSizeCm.Z + Settings.JointThicknessCm),
		Settings.BrickSizeCm.Z + Settings.JointThicknessCm, BrickCoursePitchCm);

	TestEqual(
		FString::Printf(
			TEXT("the brick's half height must be %g cm, the settings give %g"),
			BrickHalfHeightCm, 0.5 * Settings.BrickSizeCm.Z),
		0.5 * Settings.BrickSizeCm.Z, BrickHalfHeightCm);

	struct FPlaneCase
	{
		const TCHAR* Description;
		int32 Course;
		double PieceHalfHeightCm;
		double ExpectedZCm;
	};

	const FPlaneCase Cases[] = {
		{
			TEXT("a brick on the grounded course rests ON the ground, not half in it"),
			0, BrickHalfHeightCm, 3.25,
		},
		{
			TEXT("the next course up: its bottom is one 7.5 cm pitch above the ground"),
			1, BrickHalfHeightCm, 10.75,
		},
		{
			TEXT("a timber plate on course 2 bears on two brick courses and their joints"),
			2, PlateHalfHeightCm, 20.0,
		},
		{
			TEXT("a brick five courses up"),
			5, BrickHalfHeightCm, 40.75,
		},
		{
			TEXT("FAIL CLOSED: a negative course is course 0, for a brick"),
			-1, BrickHalfHeightCm, 3.25,
		},
		{
			TEXT("FAIL CLOSED: and a very negative one, for a plate"),
			-7, PlateHalfHeightCm, 5.0,
		},
	};

	for (const FPlaneCase& Case : Cases)
	{
		const double ZCm = CoursePlaneZCm(Case.Course, Case.PieceHalfHeightCm);

		TestEqual(
			FString::Printf(
				TEXT("%s: course %d with a half height of %g cm must put the build plane at %g cm, it is at %g cm"),
				Case.Description, Case.Course, Case.PieceHalfHeightCm, Case.ExpectedZCm, ZCm),
			ZCm, Case.ExpectedZCm);
	}

	/* The ladder climbs exactly one pitch per course, for both half-heights: the rows above pin three points, this pins every step, so a 6.5-not-7.5 pitch fails on the step. */
	const double HalfHeights[] = { BrickHalfHeightCm, PlateHalfHeightCm };

	for (double HalfHeightCm : HalfHeights)
	{
		for (int32 Course = 0; Course < 8; ++Course)
		{
			const double Lower = CoursePlaneZCm(Course, HalfHeightCm);
			const double Upper = CoursePlaneZCm(Course + 1, HalfHeightCm);

			TestEqual(
				FString::Printf(
					TEXT("a half height of %g cm: course %d to %d must climb exactly one %g cm pitch, it climbed %g cm"),
					HalfHeightCm, Course, Course + 1, BrickCoursePitchCm, Upper - Lower),
				Upper - Lower, BrickCoursePitchCm);

			TestTrue(
				*FString::Printf(
					TEXT("a half height of %g cm: the plane at course %d must be a number, it is %g"),
					HalfHeightCm, Course, Lower),
				FMath::IsFinite(Lower));
		}
	}

	return true;
}

/**
 * Course 0 is the one that touches the earth, and the course readout names the course it shows.
 *
 * Grounded is the flag FStructure routes load to, so which course is grounded is not cosmetic — a
 * building marked grounded cannot fall. One course has earth under it, course 0; everything above
 * is held by what is below. The label is the model's, bounded the same way as every caption: it
 * contains the course number, is never blank, and no two courses read alike.
 *
 * The printed number counts from ONE, which is not the stored index (owner-delegated ruling
 * 2026-09-15, P1): the piece menu already counts from one, and the two surfaces disagreed. Course
 * stays zero-based; only the label function changes, so grounding and plane arithmetic are
 * untouched. A negative course answers as course 0 throughout, so the two functions cannot
 * disagree about a state that should not exist.
 *
 * Needs a ticking world: no.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarCourseGroundingAndLabelTest,
	"DestructionGame.Core.SessionToolbar.CourseGroundingAndLabel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarCourseGroundingAndLabelTest::RunTest(const FString& Parameters)
{
	using namespace SessionToolbarTestSupport;
	using namespace DestructionSession;

	TestTrue(
		TEXT("course 0 sits on the earth and must read as grounded"),
		IsCourseGrounded(0));

	TestFalse(
		TEXT("course 1 is held up by course 0, not by the earth"),
		IsCourseGrounded(1));

	TestFalse(
		TEXT("course 5 is nowhere near the earth"),
		IsCourseGrounded(5));

	TestTrue(
		TEXT("FAIL CLOSED: a negative course answers as course 0 does — grounded"),
		IsCourseGrounded(-1));

	/* Two exact pins: "contains its number" is satisfied by either convention on most courses, so the rows separating them are spelled out in full. */
	TestEqual(
		FString::Printf(
			TEXT("the GROUNDED course is the player's FIRST course: CourseLabel(0) must read 'Course 1', it reads '%s'"),
			*CourseLabel(0)),
		CourseLabel(0), FString(TEXT("Course 1")));

	TestEqual(
		FString::Printf(
			TEXT("and the count keeps step: CourseLabel(2) must read 'Course 3', it reads '%s'"),
			*CourseLabel(2)),
		CourseLabel(2), FString(TEXT("Course 3")));

	const FString LabelForThree = CourseLabel(3);

	TestTrue(
		*FString::Printf(
			TEXT("the course readout must name its course ONE-BASED: CourseLabel(3) reads '%s' and must contain '4'"),
			*LabelForThree),
		LabelForThree.Contains(TEXT("4")));

	/* And must not still print the index: without this, "Course 3 (index 3)" would satisfy the row above and put two numbers on one course again. */
	TestFalse(
		*FString::Printf(
			TEXT("and must NOT print the zero-based index beside it: CourseLabel(3) reads '%s' and must not contain '3'"),
			*LabelForThree),
		LabelForThree.Contains(TEXT("3")));

	TSet<FString> SeenLabels;

	for (int32 Course = 0; Course <= 5; ++Course)
	{
		const FString Label = CourseLabel(Course);

		TestFalse(
			*FString::Printf(TEXT("the course readout must never be blank, course %d reads nothing"), Course),
			Label.IsEmpty());

		TestTrue(
			*FString::Printf(
				TEXT("the course readout must contain its own ONE-BASED number: index %d must read '%d', it reads '%s'"),
				Course, Course + 1, *Label),
			Label.Contains(FString::FromInt(Course + 1)));

		bool bAlreadyThere = false;
		SeenLabels.Add(Label, &bAlreadyThere);

		TestFalse(
			*FString::Printf(
				TEXT("two different courses must not read the same: course %d also reads '%s'"),
				Course, *Label),
			bAlreadyThere);
	}

	TestEqual(
		FString::Printf(
			TEXT("FAIL CLOSED: a negative course must read as course 0 ('%s'), it reads '%s'"),
			*CourseLabel(0), *CourseLabel(-2)),
		CourseLabel(-2), CourseLabel(0));

	TestEqual(
		FString::Printf(
			TEXT("FAIL CLOSED, AND IT IS THE PLAYER'S FIRST COURSE IT FALLS BACK TO: CourseLabel(-1) must read 'Course 1', it reads '%s'"),
			*CourseLabel(-1)),
		CourseLabel(-1), FString(TEXT("Course 1")));

	return true;
}

/**
 * Walking the demo building through the model reproduces it exactly, lifted by one brick
 * half-height: the same building, resting on the ground instead of straddling it.
 *
 * This is the integration claim, still world-free. The toolbar model is a presenter over the snap
 * solver and placement API DemoBuilding.cpp already drives; pointing it at the one building this
 * project has pinned is how we know it invented no geometry. Every demo-side number is read out of
 * the built FBrickLayout, not transcribed, so a drift in the palette or plane fails here rather
 * than in a screenshot — and it goes red AT the demo's migration onto this convention, not before.
 *
 * The assertion is a CONSTANT offset on every course, not any one height: a constant offset is the
 * whole ruling ("same relative geometry, shifted to rest on the ground") and guarantees the solver
 * reads the identical structure. And the offset must equal the brick's own half-height, or it is a
 * pinned coincidence.
 *
 * Needs a ticking world: no.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarDemoBuildingHeightsTest,
	"DestructionGame.Core.SessionToolbar.DemoBuildingHeights",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarDemoBuildingHeightsTest::RunTest(const FString& Parameters)
{
	using namespace SessionToolbarTestSupport;
	using namespace DestructionSession;

	const FVector PlateHalfExtentCm = BuildPieceHalfExtentCm(EBuildPieceKind::TimberPlate);
	const FVector BrickHalfExtentCm = BuildPieceHalfExtentCm(EBuildPieceKind::Brick);

	/* The demo building, built: BuildDemoBuilding grows an FBrickLayout through PlacePiece, so the boxes are the poses the solver committed, not a description. */
	DestructionLayout::FBrickLayout DemoLayout;
	BuildMode::BuildDemoBuilding(DemoLayout, BuildMode::FSnapSettings{});

	if (DemoLayout.Boxes.Num() <= DemoPlatePieceIndex)
	{
		AddError(FString::Printf(
			TEXT("fixture: the demo building laid only %d boxes, too few to read its plate at index %d"),
			DemoLayout.Boxes.Num(), DemoPlatePieceIndex));
		return true;
	}

	const DestructionLayout::FPieceBox& DemoPlateBox = DemoLayout.Boxes[DemoPlatePieceIndex];

	/* The palette's plate IS the demo's plate, or the heights below are about a different board. */
	TestTrue(
		*FString::Printf(
			TEXT("the palette's plate must be the board the demo actually lays, which is %s cm; the palette's is %s cm"),
			*DescribeVector(DemoPlateBox.ExtentCm), *DescribeVector(PlateHalfExtentCm)),
		VectorsExactlyEqual(PlateHalfExtentCm, DemoPlateBox.ExtentCm));

	const double PlateZCm = CoursePlaneZCm(2, PlateHalfExtentCm.Z);

	TestEqual(
		FString::Printf(
			TEXT("the plate on course 2 must sit at 20 cm, it sits at %g cm"), PlateZCm),
		PlateZCm, 20.0);

	struct FLiftCase
	{
		const TCHAR* Description;
		int32 Course;
		double HalfHeightCm;
		double DemoCentreZCm;
	};

	const FLiftCase Cases[] = {
		{ TEXT("the grounded brick course"), 0, BrickHalfHeightCm,
			DemoLayout.Boxes[DemoCourseZeroPieceIndex].CentreCm.Z },
		{ TEXT("the staggered course above it"), 1, BrickHalfHeightCm,
			DemoLayout.Boxes[DemoCourseOnePieceIndex].CentreCm.Z },
		{ TEXT("the timber wall plate bearing across the top"), 2, PlateHalfExtentCm.Z,
			DemoPlateBox.CentreCm.Z },
	};

	for (const FLiftCase& Case : Cases)
	{
		const double ModelZCm = CoursePlaneZCm(Case.Course, Case.HalfHeightCm);
		const double LiftCm = ModelZCm - Case.DemoCentreZCm;

		TestEqual(
			FString::Printf(
				TEXT("%s: the model puts it at %g cm and the demo at %g cm, so the lift must be exactly one brick half-height (%g cm), it is %g cm"),
				Case.Description, ModelZCm, Case.DemoCentreZCm, BrickHalfHeightCm, LiftCm),
			LiftCm, BrickHalfHeightCm);
	}

	/* And that lift is the brick's own half-height rather than a number that happens to be 3.25. */
	TestEqual(
		FString::Printf(
			TEXT("the ground offset must BE the palette brick's half height, which is %g cm"),
			BrickHalfExtentCm.Z),
		BrickHalfExtentCm.Z, BrickHalfHeightCm);

	return true;
}

/**
 * Every chip knows which region of the strip it is in and which piece it lays, the three regions
 * are contiguous and in order, and the mode pair is exactly the first region.
 *
 * SessionToolbarButtons answers, per button, its region (Mode, Settings, Command) and swatch
 * (Brick, Timber, None), and returns those regions in order with no interleaving.
 *
 * The group is a model answer, not a run of AddSlot calls: §b separates the regions by rules so a
 * destructive click is never adjacent to a setting click, and a widget counting slots would hold
 * that decision where no test can reach. Contiguity is what makes a divider drawable — a group
 * appearing twice would grow a rule inside a region. The mode pair being exactly first restates
 * the ordering promise. The swatch is a KIND, not a colour: the model says brick, the widget says
 * which red. Swept over all states.
 *
 * Needs a ticking world: no.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarGroupsAndSwatchesTest,
	"DestructionGame.Core.SessionToolbar.GroupsAndSwatches",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarGroupsAndSwatchesTest::RunTest(const FString& Parameters)
{
	using namespace SessionToolbarTestSupport;
	using namespace DestructionSession;

	/* The groups as a rank, not the enumerator's value, so the claim is "tabs then settings then command" rather than "declared in that order". */
	const auto RankOf = [](EToolbarGroup Group)
	{
		switch (Group)
		{
		case EToolbarGroup::Mode:     return 0;
		case EToolbarGroup::Settings: return 1;
		case EToolbarGroup::Command:  return 2;
		}

		return 3;
	};

	/* How many buttons the sweep owes, accumulated from the mode — the same floor Labels carries: a model returning nothing satisfies every claim below with nothing to check. */
	int32 ButtonsOwed = 0;
	int32 ButtonsSeen = 0;

	for (const FSessionToolbarState& State : AllStates())
	{
		const TArray<FToolbarButton> Buttons = SessionToolbarButtons(State);
		const bool bBuilding = State.Mode == ESessionMode::Build;

		/* Seventeen in Build since the rotate chip joined the palette; four in Destroy. */
		ButtonsOwed += bBuilding ? 17 : 4;

		int32 ModeGroupButtons = 0;
		int32 BrickSwatches = 0;
		int32 TimberSwatches = 0;

		for (int32 Index = 0; Index < Buttons.Num(); ++Index)
		{
			const FToolbarButton& Button = Buttons[Index];

			++ButtonsSeen;

			const EToolbarGroup ExpectedGroup = ExpectedGroupOf(Button.Id);
			const EToolbarSwatch ExpectedSwatch = ExpectedSwatchOf(Button.Id);

			TestEqual(
				FString::Printf(
					TEXT("%s: %s belongs to the %s group, it reports %s — [%s]"),
					*DescribeState(State), NameOfButton(Button.Id), NameOfGroup(ExpectedGroup),
					NameOfGroup(Button.Group), *DescribeButtons(Buttons)),
				static_cast<int32>(Button.Group), static_cast<int32>(ExpectedGroup));

			TestEqual(
				FString::Printf(
					TEXT("%s: %s carries the %s swatch, it reports %s — the chip must look like the "
						 "thing the player is about to lay — [%s]"),
					*DescribeState(State), NameOfButton(Button.Id), NameOfSwatch(ExpectedSwatch),
					NameOfSwatch(Button.Swatch), *DescribeButtons(Buttons)),
				static_cast<int32>(Button.Swatch), static_cast<int32>(ExpectedSwatch));

			ModeGroupButtons += Button.Group == EToolbarGroup::Mode ? 1 : 0;
			BrickSwatches += Button.Swatch == EToolbarSwatch::Brick ? 1 : 0;
			TimberSwatches += Button.Swatch == EToolbarSwatch::Timber ? 1 : 0;

			/* Non-decreasing rank is contiguity and order in one comparison: a repeat or a reorder both step down here. */
			if (Index > 0)
			{
				TestTrue(
					*FString::Printf(
						TEXT("%s: the strip's groups must run Mode, Settings, Command with no "
							 "interleaving — %s (%s) follows %s (%s). A group that appeared twice "
							 "would put a divider in the middle of a region — [%s]"),
						*DescribeState(State), NameOfButton(Button.Id), NameOfGroup(Button.Group),
						NameOfButton(Buttons[Index - 1].Id), NameOfGroup(Buttons[Index - 1].Group),
						*DescribeButtons(Buttons)),
					RankOf(Button.Group) >= RankOf(Buttons[Index - 1].Group));
			}
		}

		/* The mode pair is exactly the first group. With the non-decreasing claim above, "exactly two are Mode" places them; the two slots are also asserted directly, to fail by name. */
		TestEqual(
			FString::Printf(
				TEXT("%s: exactly two buttons are mode tabs — [%s]"),
				*DescribeState(State), *DescribeButtons(Buttons)),
			ModeGroupButtons, 2);

		if (Buttons.Num() >= 2)
		{
			TestTrue(
				*FString::Printf(
					TEXT("%s: the first two slots ARE the mode group — a strip whose first slots "
						 "shifted would put a different button under a cursor that has not moved. "
						 "They are %s and %s — [%s]"),
					*DescribeState(State), NameOfGroup(Buttons[0].Group),
					NameOfGroup(Buttons[1].Group), *DescribeButtons(Buttons)),
				Buttons[0].Group == EToolbarGroup::Mode && Buttons[1].Group == EToolbarGroup::Mode);
		}

		/* The palette carries one brick and two planks, counted as well as checked per button so a model answering None everywhere fails on a row naming the missing swatches. */
		TestEqual(
			FString::Printf(
				TEXT("%s: the strip must carry %d brick swatch(es) — [%s]"),
				*DescribeState(State), bBuilding ? 1 : 0, *DescribeButtons(Buttons)),
			BrickSwatches, bBuilding ? 1 : 0);

		TestEqual(
			FString::Printf(
				TEXT("%s: the strip must carry %d timber swatch(es) — [%s]"),
				*DescribeState(State), bBuilding ? 2 : 0, *DescribeButtons(Buttons)),
			TimberSwatches, bBuilding ? 2 : 0);
	}

	TestEqual(
		FString::Printf(
			TEXT("the sweep must actually have read %d buttons, it read %d — a strip that drew none "
				 "would pass every claim above by having nothing to check"),
			ButtonsOwed, ButtonsSeen),
		ButtonsSeen, ButtonsOwed);

	return true;
}

/**
 * A chip's look is a model answer: rounded and edged always, the mode's accent when chosen, faded
 * when it cannot be clicked, and the "go" chip lit without being latched.
 *
 * ChipLookFor(Button, Mode) turns bActive and bEnabled plus the button's identity into fill,
 * caption colour, caption weight and geometry, so three visual states are three and a lit chip's
 * accent is the mode's.
 *
 * The look is decided here, not in Slate: §e wants three distinct states because bActive and
 * bEnabled are different questions, and a widget drawing them alike makes a lit-but-dead chip look
 * like a greyed-but-live one — today two ternaries in BuildSessionToolbarPanel that no test can
 * read. What is pinned exactly vs as a relation, deliberately:
 *   - The accent is exact: the design's build amber and destroy red, reused not re-picked.
 *   - The idle fill is not: what may not drift is that it is neither accent nor faded.
 *   - The caption is a luminance band, not a triple.
 *
 * The "go" chip is the row that is not a restatement: Run structure is a command, never bActive,
 * yet §e fills it in the destroy accent — only possible if the fill is a function of the BUTTON,
 * not bActive. And the Destroy tab and Run may not be the same chip (C2): both come out destroy
 * accent, so the claim is an inequality in fill or outline. Clear build is danger in the caption,
 * not the fill: a red fill on an amber strip reads as the mode. Every look is finite (fail-closed:
 * DESIGN §4, FMath::Max discards a NaN).
 *
 * Needs a ticking world: no. World.Session.ToolbarChipsAreRoundedAndGrouped holds the widget to it.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarChipLookTest,
	"DestructionGame.Core.SessionToolbar.ChipLook",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarChipLookTest::RunTest(const FString& Parameters)
{
	using namespace SessionToolbarTestSupport;
	using namespace DestructionSession;

	/* --- ONE: the two accents and the two swatches are the design's own colours ------------- */

	TestTrue(
		*FString::Printf(
			TEXT("BUILD MODE'S ACCENT IS THE DESIGN'S AMBER %s — the Caution band's gold and the "
				 "ghost's own colour, reused rather than re-picked. It is %s"),
			*DescribeColour(DesignBuildAccent), *DescribeColour(ModeAccent(ESessionMode::Build))),
		ColoursExactlyEqual(ModeAccent(ESessionMode::Build), DesignBuildAccent));

	TestTrue(
		*FString::Printf(
			TEXT("AND DESTROY MODE'S IS THE DESIGN'S RED %s — the destructive row's. It is %s"),
			*DescribeColour(DesignDestroyAccent), *DescribeColour(ModeAccent(ESessionMode::Destroy))),
		ColoursExactlyEqual(ModeAccent(ESessionMode::Destroy), DesignDestroyAccent));

	/* And they are different colours: "the lit chip is the mode's accent" says nothing if the two modes share one. */
	TestFalse(
		*FString::Printf(
			TEXT("the two modes must not share an accent — which mode you are in is the highest-order "
				 "fact on the screen. Both read %s"),
			*DescribeColour(ModeAccent(ESessionMode::Build))),
		ColoursExactlyEqualRGB(ModeAccent(ESessionMode::Build), ModeAccent(ESessionMode::Destroy)));

	TestTrue(
		*FString::Printf(
			TEXT("THE BRICK SWATCH IS THE CLAY BRICK'S OWN COLOUR %s, which is M_Shed_Brick's base "
				 "colour — the chip and the brick the player lays are one colour. It is %s"),
			*DescribeColour(DesignBrickSwatch),
			*DescribeColour(SwatchColour(EToolbarSwatch::Brick))),
		ColoursExactlyEqual(SwatchColour(EToolbarSwatch::Brick), DesignBrickSwatch));

	TestTrue(
		*FString::Printf(
			TEXT("AND THE TIMBER SWATCH IS M_Shed_Timber'S %s. It is %s"),
			*DescribeColour(DesignTimberSwatch),
			*DescribeColour(SwatchColour(EToolbarSwatch::Timber))),
		ColoursExactlyEqual(SwatchColour(EToolbarSwatch::Timber), DesignTimberSwatch));

	TestEqual(
		FString::Printf(
			TEXT("A CHIP WITH NO SWATCH DRAWS NOTHING, so the no-swatch colour is transparent rather "
				 "than a colour that would paint a block on every command chip. Its alpha is %g"),
			SwatchColour(EToolbarSwatch::None).A),
		SwatchColour(EToolbarSwatch::None).A, 0.0f);

	/* Fail closed on an undeclared value (both enums are uint8): an unknown swatch draws nothing, an unknown mode's accent is still a drawable colour. */
	TestEqual(
		FString::Printf(
			TEXT("FAIL CLOSED: a swatch kind this build has never heard of must draw nothing; its "
				 "alpha is %g"),
			SwatchColour(static_cast<EToolbarSwatch>(200)).A),
		SwatchColour(static_cast<EToolbarSwatch>(200)).A, 0.0f);

	TestTrue(
		*FString::Printf(
			TEXT("FAIL CLOSED: a session mode this build has never heard of must still answer with a "
				 "real colour, it answered %s"),
			*DescribeColour(ModeAccent(static_cast<ESessionMode>(200)))),
		ColourIsFinite(ModeAccent(static_cast<ESessionMode>(200))));

	/* --- ONE-B: A LATCHED TAB AND A VERB MAY NOT BE THE SAME CHIP ---------------------------- */

	/*
	 * The C2 row, a legibility defect. On today's Destroy strip the lit Destroy tab and Run structure
	 * come out identical — both destroy accent, edged, bold dark ink — two slots apart; one says
	 * where you are, the other settles the wall irreversibly. Asserted as an inequality, not a look,
	 * since there are two honest fixes (outlined go, or a tab treatment) and picking one is the
	 * design's call: they must differ in fill or outline, RGB-exactly. Fill or outline, not caption,
	 * because the failure is the glance that does not read the words. Alpha is excluded, as in every
	 * "must differ" row. Swept over Destroy states with something to run (else Run is greyed anyway).
	 */
	int32 DestroyPairsCompared = 0;

	for (const FSessionToolbarState& State : AllStates())
	{
		if (State.Mode == ESessionMode::Build || !State.bHasStructure)
		{
			continue;
		}

		const TArray<FToolbarButton> Buttons = SessionToolbarButtons(State);

		const FToolbarButton* const Run = FindButton(Buttons, EToolbarButtonId::RunStructure);
		const FToolbarButton* const Tab = FindButton(Buttons, EToolbarButtonId::ModeDestroy);

		if (Run == nullptr || Tab == nullptr)
		{
			continue;
		}

		++DestroyPairsCompared;

		const FChipLook RunLook = ChipLookFor(*Run, State.Mode);
		const FChipLook TabLook = ChipLookFor(*Tab, State.Mode);

		TestTrue(
			*FString::Printf(
				TEXT("%s: THE LIT `Destroy` TAB AND THE `Run structure` COMMAND MUST NOT BE THE SAME "
					 "CHIP. A latched tab says where you are; a command settles the wall and releases "
					 "bricks, and there is no way back. Drawn alike, two slots apart, the only thing "
					 "between a player and an irreversible click is reading the caption. They must "
					 "differ in the fill or in the outline (RGB, alpha aside). Run reads [%s]; the tab "
					 "reads [%s]"),
				*DescribeState(State), *DescribeLook(RunLook), *DescribeLook(TabLook)),
			!ColoursExactlyEqualRGB(RunLook.Fill, TabLook.Fill)
				|| !ColoursExactlyEqualRGB(RunLook.Outline, TabLook.Outline));
	}

	TestTrue(
		*FString::Printf(
			TEXT("the sweep must actually have found some Destroy strips with a live Run chip to "
				 "compare; it compared %d pairs"),
			DestroyPairsCompared),
		DestroyPairsCompared > 0);

	/* --- TWO: the three visual states, over every chip of every strip ----------------------- */

	int32 IdleChips = 0;
	int32 ActiveChips = 0;
	int32 DisabledChips = 0;
	int32 GoChips = 0;
	int32 DangerChips = 0;

	for (const FSessionToolbarState& State : AllStates())
	{
		const TArray<FToolbarButton> Buttons = SessionToolbarButtons(State);
		const FLinearColor Accent = ModeAccent(State.Mode);

		int32 AccentLitModeChips = 0;
		int32 AccentLitPieceChips = 0;
		int32 AccentLitPlacementChips = 0;

		for (const FToolbarButton& Button : Buttons)
		{
			const FChipLook Look = ChipLookFor(Button, State.Mode);

			const FString Where = FString::Printf(
				TEXT("%s: %s reads [%s]"),
				*DescribeState(State), NameOfButton(Button.Id), *DescribeLook(Look));

			/* Every chip is a number: the fail-closed row. */
			TestTrue(
				*FString::Printf(
					TEXT("%s — every channel of every colour must be finite; a NaN becomes whatever "
						 "the clamp returns rather than an obvious fault"),
					*Where),
				ColourIsFinite(Look.Fill) && ColourIsFinite(Look.Outline)
					&& ColourIsFinite(Look.Caption)
					&& FMath::IsFinite(Look.CornerRadiusPx) && FMath::IsFinite(Look.OutlineWidthPx));

			/* Rounded and edged, always — §a principle 1's chip with its 2 px drop edge. */
			TestEqual(
				FString::Printf(
					TEXT("%s — every chip is rounded at %g px"), *Where, DesignChipCornerRadiusPx),
				Look.CornerRadiusPx, DesignChipCornerRadiusPx);

			TestEqual(
				FString::Printf(
					TEXT("%s — and carries a %g px drop edge"), *Where, DesignChipOutlineWidthPx),
				Look.OutlineWidthPx, DesignChipOutlineWidthPx);

			if (!Button.bEnabled)
			{
				++DisabledChips;

				/* Disabled is faded and not accented: the model refuses the click, so the strip must not say otherwise. */
				TestTrue(
					*FString::Printf(
						TEXT("%s — A GREYED CHIP MUST BE FADED: its fill alpha must be at or under "
							 "%g, it is %g"),
						*Where, DisabledFillAlphaCeiling, Look.Fill.A),
					Look.Fill.A <= DisabledFillAlphaCeiling);

				TestTrue(
					*FString::Printf(
						TEXT("%s — and its caption dimmed to at or under %g alpha, it is %g"),
						*Where, DisabledCaptionAlphaCeiling, Look.Caption.A),
					Look.Caption.A <= DisabledCaptionAlphaCeiling);

				TestFalse(
					*FString::Printf(
						TEXT("%s — a greyed chip must NOT wear the mode's accent %s; a lit chip that "
							 "does nothing is the failure bEnabled exists for"),
						*Where, *DescribeColour(Accent)),
					ColoursExactlyEqualRGB(Look.Fill, Accent));

				TestFalse(
					*FString::Printf(TEXT("%s — nor a bold caption, which is the lit chip's weight"),
						*Where),
					Look.bBoldCaption);
			}
			else if (Button.bActive)
			{
				++ActiveChips;

				TestTrue(
					*FString::Printf(
						TEXT("%s — THE CHIP YOU HAVE CHOSEN IS FILLED WITH THE MODE'S ACCENT %s "
							 "exactly. Everything lit on a Build strip is amber and everything lit "
							 "on a Destroy strip is red, so the colour of the strip is itself a "
							 "reading of which mode you are in"),
						*Where, *DescribeColour(Accent)),
					ColoursExactlyEqual(Look.Fill, Accent));

				TestTrue(
					*FString::Printf(
						TEXT("%s — and its caption is DARK INK on that fill: luminance must be under "
							 "0.1, it is %g. A pale caption on amber is unreadable at a glance, "
							 "which is the one thing the lit chip exists for"),
						*Where, LinearLuminance(Look.Caption)),
					LinearLuminance(Look.Caption) < 0.1);

				TestTrue(
					*FString::Printf(
						TEXT("%s — and it is BOLD. The chip says bActive twice, in the fill and in "
							 "the weight: one is read from peripheral vision and the other by a "
							 "player looking straight at it"),
						*Where),
					Look.bBoldCaption);
			}
			else if (Button.Id == EToolbarButtonId::RunStructure)
			{
				++GoChips;

				/* The "go" chip: a command is never bActive, so a widget filling on bActive alone could never draw this, yet §e asks for it. */
				TestTrue(
					*FString::Printf(
						TEXT("%s — RUN STRUCTURE IS THE 'GO' CHIP and is filled in the destroy accent "
							 "%s even though it is never bActive. A command that looked like every "
							 "other chip is a command nobody finds"),
						*Where, *DescribeColour(ModeAccent(ESessionMode::Destroy))),
					ColoursExactlyEqual(Look.Fill, ModeAccent(ESessionMode::Destroy)));
			}
			else
			{
				++IdleChips;

				/* Idle: not chosen and not greyed, and must look like neither. The exact fill is the widget's; these two may not drift. */
				TestFalse(
					*FString::Printf(
						TEXT("%s — an idle chip must NOT wear the mode's accent %s, or the player "
							 "cannot tell what they have chosen"),
						*Where, *DescribeColour(Accent)),
					ColoursExactlyEqualRGB(Look.Fill, Accent));

				TestTrue(
					*FString::Printf(
						TEXT("%s — nor may it be faded like a greyed one: its fill alpha must be "
							 "above %g, it is %g"),
						*Where, DisabledFillAlphaCeiling, Look.Fill.A),
					Look.Fill.A > DisabledFillAlphaCeiling);

				TestFalse(
					*FString::Printf(
						TEXT("%s — and an idle caption is not bold; the weight is how bActive is said "
							 "in type"),
						*Where),
					Look.bBoldCaption);

				TestTrue(
					*FString::Printf(
						TEXT("%s — and its caption is readable: alpha must be above %g, it is %g"),
						*Where, DisabledCaptionAlphaCeiling, Look.Caption.A),
					Look.Caption.A > DisabledCaptionAlphaCeiling);

				if (Button.Id == EToolbarButtonId::ClearBuild)
				{
					++DangerChips;

					/* Danger in the caption, not the fill: a red fill on an amber strip reads as the mode; a warm caption reads as a warning. */
					TestTrue(
						*FString::Printf(
							TEXT("%s — CLEAR BUILD IS THE ONE IRREVERSIBLE CONTROL IN THE BUILD GROUP "
								 "and its caption must be tinted toward the danger red: R must "
								 "exceed both G and B"),
							*Where),
						Look.Caption.R > Look.Caption.G && Look.Caption.R > Look.Caption.B);
				}
				else
				{
					TestTrue(
						*FString::Printf(
							TEXT("%s — an idle caption is bright: luminance must be at least 0.5, it "
								 "is %g"),
							*Where, LinearLuminance(Look.Caption)),
						LinearLuminance(Look.Caption) >= 0.5);
				}
			}

			/* No Build-strip chip may wear the destroy accent: a red chip on an amber strip makes "which mode am I in" unreadable at a glance. */
			if (State.Mode == ESessionMode::Build)
			{
				TestFalse(
					*FString::Printf(
						TEXT("%s — nothing on a BUILD strip may be filled with the destroy accent %s"),
						*Where, *DescribeColour(ModeAccent(ESessionMode::Destroy))),
					ColoursExactlyEqualRGB(Look.Fill, ModeAccent(ESessionMode::Destroy)));
			}

			if (Button.bActive && ColoursExactlyEqual(Look.Fill, Accent))
			{
				AccentLitModeChips += Button.Group == EToolbarGroup::Mode ? 1 : 0;

				AccentLitPieceChips += (Button.Id == EToolbarButtonId::PieceBrick
					|| Button.Id == EToolbarButtonId::PieceTimberPlate
					|| Button.Id == EToolbarButtonId::PieceTimberLintel) ? 1 : 0;

				AccentLitPlacementChips += (Button.Id == EToolbarButtonId::PlacementSnap
					|| Button.Id == EToolbarButtonId::PlacementFree) ? 1 : 0;
			}
		}

		/* Exactly one lit chip per setting group, counted through the fill: ActiveFlags counts bActive, this counts the accented chips, so a look ignoring bActive is caught here. */
		const bool bBuilding = State.Mode == ESessionMode::Build;

		TestEqual(
			FString::Printf(
				TEXT("%s: exactly one MODE tab must come out accented — [%s]"),
				*DescribeState(State), *DescribeButtons(Buttons)),
			AccentLitModeChips, 1);

		TestEqual(
			FString::Printf(
				TEXT("%s: exactly one PIECE chip must come out accented when the palette is drawn — "
					 "[%s]"),
				*DescribeState(State), *DescribeButtons(Buttons)),
			AccentLitPieceChips, bBuilding ? 1 : 0);

		TestEqual(
			FString::Printf(
				TEXT("%s: exactly one PLACEMENT chip must come out accented when the pair is drawn — "
					 "[%s]"),
				*DescribeState(State), *DescribeButtons(Buttons)),
			AccentLitPlacementChips, bBuilding ? 1 : 0);
	}

	/* Every one of the five shapes was actually reached: four arms sit inside an if/else, so a model that never greyed anything would leave them unentered. */
	AddInfo(FString::Printf(
		TEXT("the sweep read %d idle chips, %d lit, %d greyed, %d 'go' and %d danger-captioned"),
		IdleChips, ActiveChips, DisabledChips, GoChips, DangerChips));

	TestTrue(
		*FString::Printf(
			TEXT("the sweep must have reached all five shapes for its claims to mean anything: idle "
				 "%d, active %d, disabled %d, go %d, danger %d"),
			IdleChips, ActiveChips, DisabledChips, GoChips, DangerChips),
		IdleChips > 0 && ActiveChips > 0 && DisabledChips > 0 && GoChips > 0 && DangerChips > 0);

	return true;
}

/**
 * UI-6 — the player chooses what fastens the next piece: six chips, one lit, each naming the
 * shipped library row every joint the next placement forms will carry.
 *
 * FSessionToolbarState carries an EJointChoice; the Build strip draws it as a six-chip segmented
 * control in Settings between the placement pair and the course stepper, the chosen one lit and
 * all six live; and JointOverrideFor turns the choice into nothing (Auto — the inference decides)
 * or the ADDRESS of one shipped connection profile.
 *
 * The override is a pointer and nullptr is a real answer: UI-6 rides it through
 * Preview/PlaceBuildPiece as an optional const FConnectionStrength*, and a sentinel "infer" profile
 * would be a seventh row every consumer must special-case — the first to forget would bond a joint
 * with it. The address is the claim, never the fields: this library is siblings by construction, so
 * a by-value answer passes against a stale copy. It is why FNamedConnectionProfile::Strength became
 * a reference to the extern this slice, the same trap the material lookup once hit.
 *
 * AllStates now carries the joint choice, so the sweeps above cover which chips exist, one lit, all
 * live, the captions and the transitions. Left here: the chips' position in the strip and the
 * choice-to-profile map.
 *
 * Needs a ticking world: no.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarJointChoiceTest,
	"DestructionGame.Core.SessionToolbar.JointChoice",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarJointChoiceTest::RunTest(const FString& Parameters)
{
	using namespace SessionToolbarTestSupport;
	using namespace DestructionSession;

	/* --- ONE: the six chips are a contiguous run between Free and Course down ---------------- */

	const TArray<EToolbarButtonId> JointIds = {
		EToolbarButtonId::JointAuto,
		EToolbarButtonId::JointMortar,
		EToolbarButtonId::JointDry,
		EToolbarButtonId::JointNail,
		EToolbarButtonId::JointScrew,
		EToolbarButtonId::JointBolt,
	};

	int32 StatesRead = 0;

	for (const FSessionToolbarState& State : AllStates())
	{
		const TArray<FToolbarButton> Buttons = SessionToolbarButtons(State);

		if (State.Mode != ESessionMode::Build)
		{
			/* The Destroy strip draws none of them; ButtonsByMode owns that list. */
			continue;
		}

		++StatesRead;

		const int32 FreeAt = Buttons.IndexOfByPredicate(
			[](const FToolbarButton& B) { return B.Id == EToolbarButtonId::PlacementFree; });

		const int32 CourseDownAt = Buttons.IndexOfByPredicate(
			[](const FToolbarButton& B) { return B.Id == EToolbarButtonId::CourseDown; });

		const bool bBracketed = FreeAt != INDEX_NONE && CourseDownAt != INDEX_NONE
			&& CourseDownAt == FreeAt + 1 + JointIds.Num();

		TestTrue(
			*FString::Printf(
				TEXT("%s: the six joint chips must fill the slots BETWEEN Free (%d) and Course down "
					 "(%d) — one band of settings about the next placement, and never a chip dropped "
					 "in beside the irreversible Clear — [%s]"),
				*DescribeState(State), FreeAt, CourseDownAt, *DescribeButtons(Buttons)),
			bBracketed);

		for (int32 Offset = 0; Offset < JointIds.Num(); ++Offset)
		{
			const int32 Slot = FreeAt + 1 + Offset;

			const bool bRightChipInRightSlot = FreeAt != INDEX_NONE
				&& Buttons.IsValidIndex(Slot)
				&& Buttons[Slot].Id == JointIds[Offset];

			TestTrue(
				*FString::Printf(
					TEXT("%s: slot %d (the %dth after Free) must be %s — [%s]"),
					*DescribeState(State), Slot, Offset + 1, NameOfButton(JointIds[Offset]),
					*DescribeButtons(Buttons)),
				bRightChipInRightSlot);
		}

		/* --- TWO: every one of them is a Settings chip with no swatch, and always live ------- */

		for (EToolbarButtonId Id : JointIds)
		{
			const FToolbarButton* const Button = FindButton(Buttons, Id);

			if (Button == nullptr)
			{
				/* The bracket claim above has already failed; nothing further to read. */
				continue;
			}

			TestEqual(
				*FString::Printf(
					TEXT("%s: %s must be in the %s group, it is in %s — [%s]"),
					*DescribeState(State), NameOfButton(Id), NameOfGroup(EToolbarGroup::Settings),
					NameOfGroup(Button->Group), *DescribeButtons(Buttons)),
				static_cast<int32>(Button->Group), static_cast<int32>(EToolbarGroup::Settings));

			/* No swatch, and that is a claim: a swatch names the piece a chip lays, and a joint chip lays none. */
			TestEqual(
				*FString::Printf(
					TEXT("%s: %s must carry no swatch, it carries %s — [%s]"),
					*DescribeState(State), NameOfButton(Id), NameOfSwatch(Button->Swatch),
					*DescribeButtons(Buttons)),
				static_cast<int32>(Button->Swatch), static_cast<int32>(EToolbarSwatch::None));

			/* All six are live in every Build state: a joint choice describes the next placement, so it has no precondition. */
			TestTrue(
				*FString::Printf(
					TEXT("%s: %s must be live — a joint choice has no precondition, it describes the "
						 "next placement — [%s]"),
					*DescribeState(State), NameOfButton(Id), *DescribeButtons(Buttons)),
				Button->bEnabled);
		}
	}

	TestTrue(
		*FString::Printf(
			TEXT("the sweep must actually have read some Build strips, it read %d — a model drawing "
				 "no Build strip at all would satisfy every claim above by having nothing to check"),
			StatesRead),
		StatesRead > 0);

	/* --- THREE: the choice-to-profile map, by address --------------------------------------- */

	struct FOverrideCase
	{
		const TCHAR* Description;
		EJointChoice Choice;
		const FConnectionStrength* Expected;
	};

	const FOverrideCase Cases[] = {
		{
			TEXT("Auto overrides NOTHING — BuildMode::JointForContact goes on deciding, which is what "
				 "makes a brick bed in mortar and a plank bear dry without the player saying so"),
			EJointChoice::Auto,
			nullptr,
		},
		{
			TEXT("Mortar is the general-purpose bed bond, the strongest thing the library ships for "
				 "masonry — NOT the weak perpend, which is a thing the inference chooses for a "
				 "vertical face rather than a thing a player asks for"),
			EJointChoice::Mortar,
			&DestructionProfiles::GeneralPurposeMortar,
		},
		{
			TEXT("Dry is DryStone: compression and friction and no bond at all, so a player can lay a "
				 "wall that stands only while it is being squeezed"),
			EJointChoice::Dry,
			&DestructionProfiles::DryStone,
		},
		{
			TEXT("Nail, the weakest of the three fasteners"),
			EJointChoice::Nail,
			&DestructionProfiles::Nail,
		},
		{
			TEXT("Screw"),
			EJointChoice::Screw,
			&DestructionProfiles::Screw,
		},
		{
			TEXT("Bolt, the strongest"),
			EJointChoice::Bolt,
			&DestructionProfiles::Bolt,
		},
	};

	for (const FOverrideCase& Case : Cases)
	{
		const FConnectionStrength* const Got = JointOverrideFor(Case.Choice);

		TestTrue(
			*FString::Printf(
				TEXT("%s (%s): the override must BE %s, by address — it is %s"),
				Case.Description, NameOfJoint(Case.Choice),
				NameOfConnectionProfileByAddress(Case.Expected),
				NameOfConnectionProfileByAddress(Got)),
			Got == Case.Expected);
	}

	/* No two choices may answer with the same row: six chips all overriding with mortar look fine and commit one physics until the structure runs. This pins six distinct answers. */
	for (int32 A = 0; A < AllJointChoices().Num(); ++A)
	{
		for (int32 B = A + 1; B < AllJointChoices().Num(); ++B)
		{
			const EJointChoice ChoiceA = AllJointChoices()[A];
			const EJointChoice ChoiceB = AllJointChoices()[B];

			TestTrue(
				*FString::Printf(
					TEXT("%s and %s must not override with the same profile; both answered %s"),
					NameOfJoint(ChoiceA), NameOfJoint(ChoiceB),
					NameOfConnectionProfileByAddress(JointOverrideFor(ChoiceA))),
				JointOverrideFor(ChoiceA) != JointOverrideFor(ChoiceB));
		}
	}

	/*
	 * An undeclared choice (EJointChoice is a uint8) overrides NOTHING, and the stricter claim is
	 * now the honest one: JointOverrideFor's header commits to it, and SessionToolbarIsActive derives
	 * the Auto chip's lit state from JointOverrideFor(State.Joint) == nullptr — so a row here would
	 * draw a six-chip control with none lit over a session the inference is deciding. The shipped-row
	 * check is kept as the second half, so a garbage pointer fails as garbage, not merely "not null".
	 */
	const EJointChoice UnknownChoice = static_cast<EJointChoice>(200);

	TestTrue(
		*FString::Printf(
			TEXT("a joint choice this build has never heard of must override NOTHING — the header "
				 "commits to it, and the Auto chip's lit state is derived from it, so a row here "
				 "leaves the strip with no chip lit over a session the inference is deciding. It "
				 "answered %s"),
			NameOfConnectionProfileByAddress(JointOverrideFor(UnknownChoice))),
		JointOverrideFor(UnknownChoice) == nullptr);

	TestTrue(
		*FString::Printf(
			TEXT("and whatever it answers must be a shipped library row or nothing, never a pointer "
				 "into something that is not one — it answered %s"),
			NameOfConnectionProfileByAddress(JointOverrideFor(UnknownChoice))),
		IsAShippedConnectionProfileOrNull(JointOverrideFor(UnknownChoice)));

	return true;
}

/**
 * CR-2b — the player turns the next piece ninety degrees: one latching chip at the end of the
 * palette, and it survives a trip through Destroy mode.
 *
 * FSessionToolbarState carries a bRotated flag; the Build strip draws a single latching RotatePiece
 * chip in Settings, after the three piece chips and before the placement pair, lit when the flag is
 * set, live in every Build state; and ApplyToolbarButton toggles the flag and touches nothing else.
 *
 * A flag, not a fourth piece kind: a rotated brick is the same brick. EBuildPieceKind names an
 * extent and a library row; BrickRotated etc. would triple a table whose whole point is that brick
 * dimensions live in one place. Rotation is orthogonal — one bit beside the kind, one chip beside
 * the three, the palette's tail.
 *
 * It latches rather than being a verb: a chip drawn unlit while every ghost lands turned would
 * leave the player with a rotated wall and no control admitting it (the ToggleLoadOverlay failure).
 * And it survives a round trip through Destroy, which draws no chip — a model that rebuilt state on
 * a mode change would lose the rotation and lay a stretcher across the corner being built.
 *
 * AllStates deliberately omits bRotated (864 would become 1728 for a flag that moves one chip's
 * bActive), so the sweeps see the chip only at bRotated == false. Left here: every claim that needs
 * the flag set — chip lit, toggle both ways, the round trip through Destroy, refusal in Destroy.
 *
 * Needs a ticking world: no.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarRotateChipTest,
	"DestructionGame.Core.SessionToolbar.RotateChip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarRotateChipTest::RunTest(const FString& Parameters)
{
	using namespace SessionToolbarTestSupport;
	using namespace DestructionSession;

	/* The whole matrix, each state both ways round: the one test that pays for the doubling AllStates refuses, over its own claims alone. */
	TArray<FSessionToolbarState> RotatedBothWays;

	for (const FSessionToolbarState& State : AllStates())
	{
		FSessionToolbarState Upright = State;
		Upright.bRotated = false;
		RotatedBothWays.Add(Upright);

		FSessionToolbarState Turned = State;
		Turned.bRotated = true;
		RotatedBothWays.Add(Turned);
	}

	/* --- ZERO: a session opens upright ------------------------------------------------------ */

	/* The default decides least, as Mode's is Build: a session opening rotated would lay its first brick turned across the grid. */
	{
		const FSessionToolbarState Fresh;

		TestFalse(
			*FString::Printf(
				TEXT("a default-constructed session must NOT be rotated — the first brick of every "
					 "build lies along the grid. It reads %s"),
				*DescribeState(Fresh)),
			Fresh.bRotated);
	}

	/* --- ONE: the slot, the group, the swatch, the greying and the latch ---------------------- */

	int32 BuildStripsRead = 0;
	int32 LitChipsSeen = 0;
	int32 UnlitChipsSeen = 0;

	for (const FSessionToolbarState& State : RotatedBothWays)
	{
		const TArray<FToolbarButton> Buttons = SessionToolbarButtons(State);

		if (State.Mode != ESessionMode::Build)
		{
			/* The Destroy strip draws no rotate chip, even with the flag set: Destroy lays no next piece. ButtonsByMode sweeps at bRotated == false; this is the other half. */
			TestNull(
				*FString::Printf(
					TEXT("%s: the DESTROY strip must not draw the rotate chip, not even with the flag "
						 "set — [%s]"),
					*DescribeState(State), *DescribeButtons(Buttons)),
				FindButton(Buttons, EToolbarButtonId::RotatePiece));

			continue;
		}

		++BuildStripsRead;

		const int32 LintelAt = Buttons.IndexOfByPredicate(
			[](const FToolbarButton& B) { return B.Id == EToolbarButtonId::PieceTimberLintel; });

		const int32 RotateAt = Buttons.IndexOfByPredicate(
			[](const FToolbarButton& B) { return B.Id == EToolbarButtonId::RotatePiece; });

		const int32 SnapAt = Buttons.IndexOfByPredicate(
			[](const FToolbarButton& B) { return B.Id == EToolbarButtonId::PlacementSnap; });

		/* The slot as a relation to its neighbours, not an index: pinning "slot 5" would fail on any chip added before it. What may not drift is that rotation reads as the palette's tail. */
		TestTrue(
			*FString::Printf(
				TEXT("%s: the rotate chip must sit IMMEDIATELY after the three piece chips (Lintel at "
					 "%d) and IMMEDIATELY before the placement pair (Snap at %d); it is at %d — "
					 "[%s]"),
				*DescribeState(State), LintelAt, SnapAt, RotateAt, *DescribeButtons(Buttons)),
			LintelAt != INDEX_NONE && RotateAt == LintelAt + 1 && SnapAt == RotateAt + 1);

		const FToolbarButton* const Rotate = FindButton(Buttons, EToolbarButtonId::RotatePiece);

		if (Rotate == nullptr)
		{
			/* The slot claim has already failed; there is nothing further to read. */
			continue;
		}

		TestEqual(
			*FString::Printf(
				TEXT("%s: RotatePiece must be in the %s group, it is in %s — it changes what the NEXT "
					 "placement is rather than doing anything, so it sits in front of the rule the "
					 "commands are past — [%s]"),
				*DescribeState(State), NameOfGroup(EToolbarGroup::Settings),
				NameOfGroup(Rotate->Group), *DescribeButtons(Buttons)),
			static_cast<int32>(Rotate->Group), static_cast<int32>(EToolbarGroup::Settings));

		/* No swatch, and that is a claim: a swatch would make the rotate chip read as a fourth piece, the model this slice rejected. */
		TestEqual(
			*FString::Printf(
				TEXT("%s: RotatePiece must carry no swatch, it carries %s — a swatch would make it "
					 "read as a fourth piece rather than as a modifier on the three — [%s]"),
				*DescribeState(State), NameOfSwatch(Rotate->Swatch), *DescribeButtons(Buttons)),
			static_cast<int32>(Rotate->Swatch), static_cast<int32>(EToolbarSwatch::None));

		/* Always live: rotation describes the next placement, so it has no precondition. */
		TestTrue(
			*FString::Printf(
				TEXT("%s: RotatePiece must be live — rotation has no precondition, it describes the "
					 "next placement — [%s]"),
				*DescribeState(State), *DescribeButtons(Buttons)),
			Rotate->bEnabled);

		/* The latch, both ways round — the claim AllStates cannot make. */
		TestEqual(
			*FString::Printf(
				TEXT("%s: RotatePiece is lit exactly when the next piece is rotated; it reads %s — "
					 "[%s]"),
				*DescribeState(State), Rotate->bActive ? TEXT("LIT") : TEXT("unlit"),
				*DescribeButtons(Buttons)),
			Rotate->bActive, State.bRotated);

		LitChipsSeen += Rotate->bActive ? 1 : 0;
		UnlitChipsSeen += Rotate->bActive ? 0 : 1;
	}

	/* Both shapes were actually reached: the latch claim is an equality inside a loop, so a chip only ever seen unlit would satisfy it. */
	TestTrue(
		*FString::Printf(
			TEXT("the sweep must have read some Build strips (%d) and must have seen the chip BOTH "
				 "lit (%d) and unlit (%d) — a chip only ever seen one way is a constant, not a latch"),
			BuildStripsRead, LitChipsSeen, UnlitChipsSeen),
		BuildStripsRead > 0 && LitChipsSeen > 0 && UnlitChipsSeen > 0);

	/* --- TWO: the click toggles the flag, and moves nothing else ----------------------------- */

	int32 TogglesMade = 0;

	for (const FSessionToolbarState& State : RotatedBothWays)
	{
		if (State.Mode != ESessionMode::Build)
		{
			/* A click in a mode that does not draw the chip is a bitwise no-op. Transitions sweeps this at bRotated == false only, so a model toggling regardless of the strip would un-rotate on a Destroy click. */
			TestTrue(
				*FString::Printf(
					TEXT("%s: clicking RotatePiece in a mode that does not draw it must change nothing "
						 "at all; it produced %s"),
					*DescribeState(State),
					*DescribeState(ApplyToolbarButton(State, EToolbarButtonId::RotatePiece))),
				StatesEqual(ApplyToolbarButton(State, EToolbarButtonId::RotatePiece), State));

			continue;
		}

		++TogglesMade;

		const FSessionToolbarState After = ApplyToolbarButton(State, EToolbarButtonId::RotatePiece);

		TestEqual(
			*FString::Printf(
				TEXT("%s: one click must TOGGLE the rotation rather than latch it on — the same chip "
					 "is the only way back, and a player who cannot un-rotate is a player who must "
					 "reopen the level. It produced %s"),
				*DescribeState(State), *DescribeState(After)),
			After.bRotated, !State.bRotated);

		/* And nothing else moved: built by flipping the one field the transition names, compared whole, so a transition that also reset the piece or course fails here. */
		FSessionToolbarState OnlyRotationMoved = State;
		OnlyRotationMoved.bRotated = !State.bRotated;

		TestTrue(
			*FString::Printf(
				TEXT("%s: and the rotate click must move NOTHING else — the piece, the placement, the "
					 "joint, the course and both flags survive it. It produced %s"),
				*DescribeState(State), *DescribeState(After)),
			StatesEqual(After, OnlyRotationMoved));

		/* Two clicks land where you started: the honest reading of "toggle". */
		const FSessionToolbarState Back =
			ApplyToolbarButton(After, EToolbarButtonId::RotatePiece);

		TestTrue(
			*FString::Printf(
				TEXT("%s: and two clicks must land back on the state it started in; it landed on %s"),
				*DescribeState(State), *DescribeState(Back)),
			StatesEqual(Back, State));
	}

	TestTrue(
		*FString::Printf(
			TEXT("the toggle sweep must actually have clicked some live rotate chips; it made %d"),
			TogglesMade),
		TogglesMade > 0);

	/* --- THREE: the round trip through Destroy brings the rotation back ----------------------- */

	/*
	 * The failure this row exists for: a player turns a piece for the second leg of an L, checks it
	 * in Destroy, and comes back — a model that rebuilt state on the mode change hands back an
	 * upright ghost and their next click lays a stretcher across the corner. The whole state is
	 * compared, not just the flag, since fields a transition does not name must survive it.
	 */
	{
		FSessionToolbarState Turned =
			MakeState(ESessionMode::Build, EBuildPieceKind::TimberLintel, EPlacementMode::Free,
				4, true, false, EJointChoice::Screw, /*bRotated*/ true);

		const FSessionToolbarState InDestroy =
			ApplyToolbarButton(Turned, EToolbarButtonId::ModeDestroy);

		TestTrue(
			*FString::Printf(
				TEXT("going to Destroy must keep the rotation the player chose; the state reads %s"),
				*DescribeState(InDestroy)),
			InDestroy.bRotated);

		const FSessionToolbarState BackInBuild =
			ApplyToolbarButton(InDestroy, EToolbarButtonId::ModeBuild);

		TestTrue(
			*FString::Printf(
				TEXT("AND COMING BACK MUST RETURN THE WHOLE SESSION: the lintel, Free, course 4, the "
					 "screws AND the rotation. It went out as %s and came back as %s"),
				*DescribeState(Turned), *DescribeState(BackInBuild)),
			StatesEqual(BackInBuild, Turned));

		const TArray<FToolbarButton> Buttons = SessionToolbarButtons(BackInBuild);

		const FToolbarButton* const Rotate = FindButton(Buttons, EToolbarButtonId::RotatePiece);

		TestTrue(
			*FString::Printf(
				TEXT("and the chip must be drawn LIT on the way back in, or the strip is telling the "
					 "player a rotation it is about to commit is off — [%s]"),
				*DescribeButtons(Buttons)),
			Rotate != nullptr && Rotate->bActive);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
