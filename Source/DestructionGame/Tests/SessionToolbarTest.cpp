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
 * NAMED NAMESPACE, and named differently from every other one in this module — an anonymous
 * namespace is private to a TRANSLATION UNIT rather than to a file, and a unity build merges many
 * files into one. See CURRENT_STATE.md and Tests/ConnectionLoadTest.cpp, which is where that rule
 * was paid for; the `using namespace` lives inside each RunTest for the same reason.
 */
namespace SessionToolbarTestSupport
{
	/**
	 * THE BRICK COURSE PITCH, TRANSCRIBED RATHER THAN IMPORTED, AND IT IS 7.5 cm.
	 *
	 * The production function is required to DERIVE it — FSnapSettings::BrickSizeCm.Z (6.5) plus
	 * JointThicknessCm (1.0) — because a course is the brick's own coordinating dimension and a
	 * second hand-written 7.5 in Core would be a coordinating grid spelled in two places. What this
	 * test does is the opposite of importing it: it spells the sum out itself, against a default
	 * FSnapSettings, so that a retune of the brick or the joint fails HERE rather than quietly
	 * agreeing with whatever the plane function now says.
	 */
	constexpr double BrickCoursePitchCm = 7.5;

	/** The brick's half height — half of FSnapSettings::BrickSizeCm.Z, the standard 6.5 cm unit. */
	constexpr double BrickHalfHeightCm = 3.25;

	/** The wall plate's half height, from the demo building's own plate extent (33.75, 5.125, 5.0). */
	constexpr double PlateHalfHeightCm = 5.0;

	/**
	 * WHICH PIECE OF THE DEMO BUILDING EACH COURSE'S HEIGHT IS READ OFF, by placement index.
	 *
	 * THE HEIGHTS THEMSELVES ARE NO LONGER TRANSCRIBED. They used to be three literals copied out
	 * of Core/BuildMode/DemoBuilding.cpp (0, 7.5, 16.75) — which pinned the demo's numbers in a
	 * second file, so migrating the demo onto the rests-on-the-ground convention would have left
	 * this test agreeing with a building that no longer exists. Running the builder and reading the
	 * placed centres out of its own FBrickLayout makes the comparison live: the day the demo moves,
	 * the lift here stops being one brick half-height and this test says so.
	 *
	 * The indices are the builder's documented placement order (Tests/DemoBuildingTest.cpp pins the
	 * whole eight-step sequence): 0-3 are the grounded course, 4-6 the staggered course above it,
	 * and 7 the timber wall plate bearing across the top.
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
	 * WHICH REGION OF THE STRIP EACH BUTTON BELONGS TO, WRITTEN OUT HERE RATHER THAN ASKED OF THE
	 * MODEL.
	 *
	 * SESSION_UI_DESIGN §b draws the strip as three regions — the mode tabs, the current mode's
	 * settings, and the mode's one command at the far right — separated by rules, so that "a
	 * destructive click is never adjacent to a setting click". A test that read the group off the
	 * same function it is checking would assert nothing; the table is the design transcribed.
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
		case EToolbarButtonId::PlacementSnap:
		case EToolbarButtonId::PlacementFree:

		/*
		 * AND THE SIX JOINT CHIPS ARE SETTINGS TOO, for the same reason the placement pair is one:
		 * the choice is a property of the NEXT placement rather than something that happens, so it
		 * latches, it is lit, and it sits in front of the rule the commands are past.
		 */
		case EToolbarButtonId::JointAuto:
		case EToolbarButtonId::JointMortar:
		case EToolbarButtonId::JointDry:
		case EToolbarButtonId::JointNail:
		case EToolbarButtonId::JointScrew:
		case EToolbarButtonId::JointBolt:

		case EToolbarButtonId::CourseDown:
		case EToolbarButtonId::CourseUp:

		/*
		 * AND THE LOAD OVERLAY IS A SETTING, NOT A COMMAND, WHICH IS §b's TABLE READ LITERALLY. It
		 * changes how the session LOOKS at the structure rather than doing anything to it, so it sits
		 * with the settings and past no rule — the rule exists so that "a destructive click is never
		 * adjacent to a setting click", and a toggle filed beside Run structure would put a harmless
		 * click hard against the one that settles the wall.
		 */
		case EToolbarButtonId::ToggleLoadOverlay:
			return EToolbarGroup::Settings;

		case EToolbarButtonId::ClearBuild:
		case EToolbarButtonId::RunStructure:
			return EToolbarGroup::Command;
		}

		return EToolbarGroup::Command;
	}

	/** Which little block of colour a chip carries: the thing the player is about to lay, or nothing. */
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
	 * THE DESIGN'S PALETTE, TRANSCRIBED RATHER THAN IMPORTED, AND IT IS LINEAR.
	 *
	 * SESSION_UI_DESIGN §e gives every colour twice — the LINEAR triple the implementer writes and the
	 * sRGB hex the designer checks — and says in as many words that confusing the two is how a palette
	 * drifts. These are the linear ones, spelled here so that a production constant retuned away from
	 * the design fails on a row that names the design rather than agreeing with whatever the widget
	 * now uses. Build amber is the Caution band's own gold and destroy red is the destructive row's;
	 * the two swatches are `M_Shed_Brick` and `M_Shed_Timber`'s own base colours, from
	 * Scripts/Author-ShedMaterials.py — the chip and the thing the player lays are one colour.
	 */
	const FLinearColor DesignBuildAccent(0.95f, 0.66f, 0.13f, 1.0f);
	const FLinearColor DesignDestroyAccent(0.72f, 0.16f, 0.14f, 1.0f);
	const FLinearColor DesignBrickSwatch(0.35f, 0.06f, 0.04f, 1.0f);
	const FLinearColor DesignTimberSwatch(0.45f, 0.22f, 0.09f, 1.0f);

	/** Rounded chips with a drop edge — §a principle 1, "chunky rounded chips with a 2 px drop edge". */
	constexpr float DesignChipCornerRadiusPx = 10.0f;
	constexpr float DesignChipOutlineWidthPx = 2.0f;

	/**
	 * THE ALPHA A FADED CHIP MUST BE AT OR UNDER, AND THE ONE ITS CAPTION MUST BE AT OR UNDER.
	 *
	 * §e's disabled row is "chip fill @ 3 %, 50 % opacity, dim text". What matters is not the exact
	 * number but that a greyed chip is VISIBLY not a live one: the model already refuses the click, so
	 * a chip drawn like its live neighbour tells the player the game missed the press.
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

	/**
	 * HOW BRIGHT A COLOUR READS, on the linear luminance weights.
	 *
	 * The active chip's caption is DARK INK on a lit fill and an idle chip's is a readable grey, and
	 * "dark" and "readable" are the claim rather than any one triple — pinning §e's exact
	 * (0.02, 0.015, 0.01) would make every future nudge of the ink a failure with nothing wrong
	 * behind it.
	 */
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

	/** Every button the model knows, so a sweep covers the whole vocabulary rather than a favourite few. */
	TArray<DestructionSession::EToolbarButtonId> AllButtonIds()
	{
		using namespace DestructionSession;

		return {
			EToolbarButtonId::ModeBuild,
			EToolbarButtonId::ModeDestroy,
			EToolbarButtonId::PieceBrick,
			EToolbarButtonId::PieceTimberPlate,
			EToolbarButtonId::PieceTimberLintel,
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

	/**
	 * WHICH CHOICE EACH JOINT CHIP STANDS FOR, WRITTEN OUT HERE RATHER THAN ASKED OF THE MODEL.
	 *
	 * The pairing is the design — one chip per choice, in the enum's own order — and a test that
	 * read it off the function it is checking would assert nothing at all. It is also the only
	 * place the "Screw chip lights when the choice is Screw" claim can come from: the model's own
	 * answer would agree with itself whichever way the six were wired up.
	 */
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

	/**
	 * Which shipped CONNECTION profile an override points at, BY ADDRESS. Never a value comparison.
	 *
	 * THE SAME IDENTITY RULE NameOfMaterialByAddress KEEPS, and it bites harder here. Two
	 * FConnectionStrengths with equal fields are equal in every way except which library row a
	 * retune moves, and this library's rows are siblings by construction — GeneralPurposeMortar and
	 * its perpend differ on two axes, Nail, Screw and Bolt on one scaling. A by-value answer would
	 * go on passing against a private copy the next re-anchor never reaches.
	 */
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
	 * FIELD BY FIELD RATHER THAN FMemory::Memcmp, WHICH IS THE HONEST SPELLING OF "UNCHANGED".
	 *
	 * The struct has padding between its bool and whatever follows, and padding is not required to
	 * be copied — so a memcmp would report a difference nobody can see and would do it
	 * intermittently. Every field the state HAS is compared here, so adding one without extending
	 * this helper is the only way a change can hide, and that is a mechanical omission rather than
	 * a silent one.
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
			&& A.bLoadOverlay == B.bLoadOverlay;
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
		DestructionSession::EJointChoice Joint = DestructionSession::EJointChoice::Auto)
	{
		DestructionSession::FSessionToolbarState State;
		State.Mode = Mode;
		State.Piece = Piece;
		State.Placement = Placement;
		State.Course = Course;
		State.bHasStructure = bHasStructure;
		State.bLoadOverlay = bLoadOverlay;
		State.Joint = Joint;
		return State;
	}

	/**
	 * THE WHOLE STATE SPACE THE FLAG RULES ARE CLAIMED OVER, rather than the three or four states
	 * somebody thought of.
	 *
	 * Every rule about which buttons exist, which is lit and which is greyed is a claim about ALL
	 * states, and a hand-picked handful covers only the shapes the author had in mind — DESIGN §4's
	 * "an invariant asserted over fixtures that all share a hidden property is not an invariant".
	 * Two modes x three pieces x two placements x three courses x two structure flags x two load
	 * overlay flags x SIX JOINT CHOICES is 864 states and costs milliseconds, so the sweep is the
	 * cheap way to be sure the hidden property is not "the author always wrote Course 0".
	 *
	 * The courses are 0 (the grounded course, where CourseDown must be refused), 1 (the first course
	 * where it must be offered) and 5 (well clear of the boundary, so an off-by-one at 1 cannot be
	 * the only thing the boundary rows see).
	 *
	 * AND THE LOAD OVERLAY IS A DIMENSION RATHER THAN A ROW OR TWO, BECAUSE IT IS ORTHOGONAL TO
	 * EVERYTHING ELSE ON THE STRIP. It survives a trip through Build mode, where it is not drawn at
	 * all, so every Build state has to be swept with it BOTH ways — a model that reset it whenever
	 * the Build strip was asked for would pass a sweep that only ever set it in Destroy.
	 *
	 * THE JOINT CHOICE IS A DIMENSION FOR THE MIRROR-IMAGE REASON. It is drawn in BUILD mode alone,
	 * so every DESTROY state has to be swept with all six — a model that reset the choice to Auto
	 * whenever the Destroy strip was asked for would pass a sweep that only ever set it in Build,
	 * and a player would come back from one Destroy click to find their screws turned into mortar.
	 * Six values rather than a pair, because "exactly one of six is lit" is the claim and a
	 * segmented control tested with two members can be lit by a boolean.
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
 * THE TOOLBAR DRAWS ONE FIXED LIST PER MODE, IN ONE FIXED ORDER, AND THE MODE PAIR IS ALWAYS FIRST.
 *
 * WHY THE LIST IS A MODEL ANSWER AND NOT A SLATE LAYOUT. What a player sees is a strip of buttons,
 * and asserting that widgets appeared needs a viewport, fails for reasons that have nothing to do
 * with the toolbar, and cannot see the failures that matter: a button offered in the mode it does
 * nothing in, a button missing, or the strip reordering under the player's cursor between two
 * clicks. All three are a pure function of the state, so they live here — exactly as
 * BuildPieceMenuRows and PieceMenuPanelSizePx do for the piece menu, and for the same reason.
 *
 * THE MODE PAIR BEING FIRST IS THE ONE ORDERING CLAIM WITH A PLAYER-FACING REASON. Everything else
 * on the strip changes with the mode; the two buttons that switch modes may not move, because a
 * strip whose first two slots shifted would put a different button under a cursor that has not
 * moved. That is a decision, and a decision expressed as the order of AddSlot calls in a widget is
 * a decision in the one place no test can reach.
 *
 * SWEPT OVER THE WHOLE STATE SPACE rather than one state per mode: the list's CONTENT depends on
 * the mode alone, and that is itself the claim — a piece selection or a course number that added,
 * removed or reordered a button would be a strip that rearranges itself while a player uses it.
 *
 * NEEDS A TICKING WORLD: no, and not even a world. One plain struct in, an array of plain structs
 * out.
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
		EToolbarButtonId::PlacementSnap,
		EToolbarButtonId::PlacementFree,

		/*
		 * THE JOINT CHOICE SITS AFTER THE PLACEMENT PAIR AND BEFORE THE COURSE STEPPER, and the
		 * position is the claim rather than a preference. Both are segmented controls over the
		 * NEXT placement — how the piece is positioned, then how it is fastened — so they read as
		 * one band; the course stepper past them is a different question (where the plane is)
		 * and the command past the rule is a different kind of thing entirely. A chip dropped in
		 * beside `Clear build` would put a harmless setting hard against the irreversible one.
		 *
		 * AND Auto IS FIRST, because it is the default the session opens on and the one a player
		 * returns to; a segmented control whose default is in the middle reads as a value on a
		 * scale rather than as "leave it to the game".
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
	 * THE DESTROY STRIP NOW HAS A SETTINGS GROUP, AND THE LOAD OVERLAY IS ITS FIRST MEMBER.
	 *
	 * SESSION_UI_DESIGN §b's Destroy table, in its order: the mode pair that may never move, then the
	 * mode's settings, then the mode's one command past a rule. The toggle is BETWEEN the pair and
	 * Run structure rather than beside it, which is the whole of the three-region rule — a click that
	 * only changes how the wall is coloured must not sit hard against the one that settles it.
	 *
	 * IT IS DRAWN IN DESTROY MODE ALONE. `bLoadOverlay` is swept both ways over both modes below, so
	 * a model that drew the chip on the Build strip whenever the overlay happened to be on fails here
	 * rather than in a screenshot.
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

		/*
		 * AND NO BUTTON APPEARS TWICE. Two slots carrying one id is a strip where a click cannot be
		 * attributed, and it is the failure an ordered element-by-element comparison happens not to
		 * catch when the duplicate lands where its twin was expected.
		 */
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
 * EXACTLY ONE BUTTON IN EACH SETTING GROUP IS LIT, AND IT IS THE ONE THE STATE NAMES; A COMMAND
 * BUTTON IS NEVER LIT.
 *
 * bActive IS "THIS IS WHAT YOU HAVE CHOSEN", WHICH IS NOT THE SAME QUESTION AS bEnabled. A toolbar
 * with two lit piece buttons tells the player the ghost is two pieces at once; a toolbar with none
 * lit tells them the click they just made did nothing. Both are states a widget that derived its
 * own highlight by comparing captions would reach, which is why the answer is data on the row.
 *
 * COMMAND BUTTONS ARE NEVER ACTIVE, and that is a claim rather than an omission. Clear and Run are
 * things that HAPPEN; a latched-looking Clear button reads as a mode the player is stuck in, and
 * the toolbar has two genuine modes already.
 *
 * NEEDS A TICKING WORLD: no.
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
				/*
				 * EXACTLY ONE OF SIX IS LIT, AND IT IS THE ONE THE STATE NAMES. A segmented control
				 * with two lit chips tells the player the next brick is screwed AND laid dry, and
				 * one with none lit tells them the click they just made did nothing — and this
				 * choice changes the COMMITTED PHYSICS of every joint the next piece forms, so
				 * "which one did I pick" is not a cosmetic question.
				 */
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

			case EToolbarButtonId::ToggleLoadOverlay:
				/*
				 * A SETTING LATCHES, WHICH IS THE WHOLE DIFFERENCE BETWEEN THIS CHIP AND Run structure
				 * SITTING TWO SLOTS AWAY. The overlay is a way of LOOKING at the wall and it stays on
				 * until it is turned off, so the chip has to say so — a toggle drawn unlit while the
				 * whole structure is tinted green and amber leaves the player with a coloured wall and
				 * no control that admits to having done it, and the obvious next move is to click the
				 * chip again and turn it OFF while expecting it to turn on.
				 */
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

		/*
		 * THE PIECE AND PLACEMENT GROUPS ARE NOT DRAWN IN DESTROY MODE, so "exactly one lit" is a
		 * claim about the buttons that EXIST. Expecting one in a mode that draws none would assert
		 * the group into existence and contradict the list this file already pins.
		 */
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

		/*
		 * AND THE SEGMENTED JOINT CONTROL IS ALL SIX CHIPS OR NONE, WITH EXACTLY ONE LIT WHEN IT IS
		 * DRAWN. The count floor is what stops this passing on a strip that drew Auto alone: a
		 * one-chip segmented control is always correctly lit, and it is also a control the player
		 * cannot change anything with.
		 */
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

		/*
		 * AND THE TOGGLE WAS ACTUALLY ON THE STRIP TO BE READ. The claim above lives inside the loop
		 * over the buttons that came back, so a Destroy strip that simply did not draw the chip would
		 * satisfy it by having nothing to check — the same measured floor every other sweep in this
		 * file carries.
		 */
		TestEqual(
			FString::Printf(
				TEXT("%s: the load overlay toggle must be on the strip in Destroy mode and nowhere else "
					 "— [%s]"),
				*DescribeState(State), *DescribeButtons(Buttons)),
			LoadOverlayButtonsSeen, bBuilding ? 0 : 1);
	}

	return true;
}

/**
 * A BUTTON IS GREYED EXACTLY WHEN THE THING BEHIND IT CANNOT HAPPEN: CourseDown ON THE GROUND
 * COURSE, AND Clear AND Run WITH NOTHING BUILT.
 *
 * THE MODEL OWNS THE GREYING BECAUSE THE MODEL OWNS THE REFUSAL. ApplyToolbarButton already refuses
 * to take the course below zero; if the widget decided separately whether to grey the button, the
 * refusal would be written twice and the two copies would disagree the day the floor moves — the
 * player would be left clicking a lit button that does nothing, which is the failure this project
 * keeps closing, one layer out.
 *
 * bHasStructure IS THE ONLY PRECONDITION EITHER COMMAND HAS. Run with nothing built solves an empty
 * structure and Clear with nothing built clears nothing; both are silent no-ops, and a silent no-op
 * on a command button is indistinguishable from the game having missed the click.
 *
 * EVERYTHING ELSE IS ALWAYS ENABLED, and that is asserted rather than assumed. A mode button greyed
 * by an over-eager precondition is a player who cannot get out of the mode they are in.
 *
 * NEEDS A TICKING WORLD: no.
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
		 * THE FLOOR, AND IT IS NOT DECORATION — IT IS THE ONLY THING BETWEEN THIS TEST AND PASSING
		 * ON AN EMPTY STRIP. Everything below is a sweep OVER the buttons that came back, so a model
		 * that returned nothing at all would satisfy every claim in the loop by having nothing to
		 * check. Measured: against a stub returning {} this test reported Success until this block
		 * was added. The three buttons named are exactly the ones this test exists for — the only
		 * three with a precondition — so requiring them present is requiring the subject to exist.
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
				/*
				 * THE SAME PRECONDITION, AND IT IS A PRECONDITION RATHER THAN TIDINESS. The overlay
				 * solves the session's structure and then tints its pieces; with nothing built there
				 * is nothing to solve and nothing to tint, so a live chip would latch on, colour
				 * exactly zero bricks, and leave the player looking for the wall it had lit.
				 */
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
 * EVERY BUTTON CARRIES A CAPTION, NO TWO CAPTIONS ON ONE STRIP READ THE SAME, AND THE FIVE CAPTIONS
 * A PLAYER NAVIGATES BY SAY THE WORD THEY NAVIGATE BY.
 *
 * THE CAPTION IS DATA ON THE ROW FOR THE REASON FPieceMenuRow::Label IS: a widget that spelled its
 * own button text would be holding wording in the one place no test can reach, and this strip's
 * wording is load-bearing — Snap versus Free is the difference between a piece that lands on the
 * bond and one that lands where the cursor was.
 *
 * WORDING IS BOUNDED, NOT PINNED, AND DELIBERATELY. Pinning "Snap" exactly would make every future
 * retune — "Snap to bond", a keyboard hint in the caption — a test failure with nothing wrong
 * behind it. What may not drift is that the button a player is hunting for contains the word they
 * are hunting for, and that no two buttons on one strip are indistinguishable.
 *
 * DISTINCTNESS IS WITHIN ONE STRIP RATHER THAN GLOBAL, which is what the rule is actually for: two
 * identical captions side by side is a strip a player cannot read. ModeBuild and ClearBuild may
 * both contain the word "Build" and that is fine — they are different sentences.
 *
 * NEEDS A TICKING WORLD: no.
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

		/*
		 * THE WORD IS "Load" AND THE REST IS THE WIDGET'S. "Load overlay", "Load view", "Show load" —
		 * all of them are the button the player is hunting for, and pinning the whole caption would
		 * make every retune a red test with nothing wrong behind it. What may not drift is that the
		 * chip that colours the wall by load says so.
		 */
		{ EToolbarButtonId::ToggleLoadOverlay, TEXT("Load") },

		/*
		 * AND THE SIX JOINT CHIPS SAY WHAT THEY FASTEN WITH. The word is the whole claim and the
		 * rest is the widget's — "Screw", "Screwed", "Screw joint" are all the chip a player is
		 * hunting for — but a chip that does not contain its own fastener's name is a segmented
		 * control the player has to click to identify, and this one commits physics.
		 *
		 * "Dry" RATHER THAN "DryStone": the library row is DryStone, and the chip is one slot of a
		 * six-slot control on a 48 px bar. What may not drift is the word a player reads as "no
		 * bond at all", and both spellings contain it.
		 */
		{ EToolbarButtonId::JointAuto,   TEXT("Auto") },
		{ EToolbarButtonId::JointMortar, TEXT("Mortar") },
		{ EToolbarButtonId::JointDry,    TEXT("Dry") },
		{ EToolbarButtonId::JointNail,   TEXT("Nail") },
		{ EToolbarButtonId::JointScrew,  TEXT("Screw") },
		{ EToolbarButtonId::JointBolt,   TEXT("Bolt") },
	};

	/*
	 * HOW MANY WORD CHECKS THIS SWEEP OWES, COUNTED AS IT GOES AND SETTLED AT THE END.
	 *
	 * THE SAME FLOOR THE ENABLED-FLAGS TEST CARRIES, FOR THE SAME MEASURED REASON: every claim below
	 * is a sweep over the buttons that came back and over the word rows that are actually on the
	 * strip, so a model returning nothing satisfies all of them by having nothing to check —
	 * measured, against a stub returning {}, as a Success. The owed count is accumulated from the
	 * mode rather than written as a literal so that it tracks AllStates() instead of going stale
	 * the day a state is added to the matrix.
	 */
	int32 WordChecksOwed = 0;
	int32 WordChecksMade = 0;

	for (const FSessionToolbarState& State : AllStates())
	{
		const TArray<FToolbarButton> Buttons = SessionToolbarButtons(State);

		/*
		 * TEN IN BUILD AND FOUR IN DESTROY. A Build strip owes ModeBuild, ModeDestroy, the two
		 * placement captions and the SIX joint chips; a Destroy strip owes ModeBuild, ModeDestroy,
		 * Run and the load toggle. The two mode captions are the only rows both strips share.
		 */
		WordChecksOwed += State.Mode == ESessionMode::Build
			? 2 + 2 + 6   /* the mode pair, Snap and Free, then the six joint chips */
			: 2 + 2;      /* the mode pair, then Run structure and Load overlay */

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
 * ONE CLICK CHANGES ONE THING AND LEAVES EVERYTHING ELSE ALONE; A CLICK ON A BUTTON THAT IS GREYED
 * OR NOT ON SCREEN CHANGES NOTHING AT ALL.
 *
 * THE TRANSITION IS A PURE FUNCTION BECAUSE THAT IS THE ONLY WAY IT IS ASSERTABLE. A controller that
 * mutated its own fields from a Slate callback would put the whole of the toolbar's behaviour behind
 * a click that only a human can perform; state-in, state-out makes every transition a table row.
 *
 * THE FIELDS A TRANSITION DOES NOT NAME MUST SURVIVE IT, and the rows below are built to catch the
 * opposite: they start from states with a non-default piece, a non-default placement and a non-zero
 * course, so a transition that rebuilt the state from scratch — the natural way to write one — fails
 * rather than agreeing by coincidence with a default-constructed expectation.
 *
 * THE COURSE FLOOR IS THE FAIL-CLOSED ROW. Course is a player-facing readout and a build plane; a
 * negative one is a plane under the ground, which is why the refusal is here rather than left to the
 * caller and why the greyed CourseDown must be a BITWISE no-op rather than a clamp that happens to
 * land on the same number. Those are the same answer today and stop being the same answer the moment
 * anything else on the state moves with a course change.
 *
 * AND THE LAST BLOCK HOLDS THE TWO FUNCTIONS AGAINST EACH OTHER over the whole state space: whatever
 * SessionToolbarButtons refuses to draw, or draws greyed, ApplyToolbarButton must refuse to act on.
 * Two separately-written answers to "can this happen" is exactly how a lit button that does nothing
 * gets shipped.
 *
 * NEEDS A TICKING WORLD: no.
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

		/* --- THE LOAD OVERLAY: A SETTING, SO IT LATCHES AND IT SURVIVES A MODE CHANGE ---------- */

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
			/*
			 * THE PRESERVATION ROW, AND IT IS THE ONE THIS SETTING EXISTS TO GET WRONG. Going to Build
			 * takes the chip off the strip; the player's choice must still be theirs when they come
			 * back, exactly as the piece, the placement and the course are. A controller that "tidied
			 * up" by clearing the flag on the way out would leave a wall that was tinted a moment ago
			 * plain, with no click anywhere having asked for that.
			 */
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

		/* --- THE JOINT CHOICE: A SEGMENTED SETTING, SO IT LATCHES AND IT SURVIVES A MODE CHANGE -- */

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
			/*
			 * AND BACK TO Auto, WHICH IS THE ONE THAT HAS TO BE REACHABLE. Auto is not "no choice",
			 * it is the choice that hands the joint back to BuildMode::JointForContact — and a
			 * player who has screwed one plate and now wants an ordinary bedded brick has no other
			 * way to say so.
			 */
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
			/*
			 * THE PRESERVATION PAIR, AND IT IS THE ONE THIS SETTING EXISTS TO GET WRONG. Going to
			 * Destroy takes the six chips off the strip; a session that "tidied up" on the way out
			 * would bring the player back to Auto, and the next plate they lay would be dry-bedded
			 * where they asked for screws — a difference they cannot see until the structure runs.
			 */
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
	 * THE TWO FUNCTIONS HELD AGAINST EACH OTHER, over every state and every button in the
	 * vocabulary — including the buttons that state does not draw. This is the property the table
	 * above cannot cover by enumeration: 864 states times 18 buttons is 15,552 clicks, and what it
	 * asserts is one-directional on purpose. A button that is absent or greyed MUST leave the state
	 * alone; nothing is claimed here about the ones that are lit and enabled, because that is what
	 * the table is for.
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

			/*
			 * AND NO CLICK MAY EVER PRODUCE A NEGATIVE COURSE. The struct says "never negative" and
			 * this is the only door into it; a plane below the ground is a readout nobody can make
			 * sense of and a build plane nothing can rest on.
			 */
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
 * THE PALETTE: EACH PIECE KIND IS ONE HALF-EXTENT AND ONE MATERIAL, AND BOTH ARE THE ONES THE
 * ALREADY-BUILT DEMO USES.
 *
 * THE SIZES ARE NOT NEW GEOMETRY. The brick is the standard 21.5 x 10.25 x 6.5 unit this whole
 * project is calibrated on, halved; the plate is Core/BuildMode/DemoBuilding.cpp's own wall plate
 * (33.75, 5.125, 5.0) transcribed; the lintel is that plate's 90 cm sibling. Pinning them here is
 * what stops the toolbar becoming a THIRD place brick dimensions are written down.
 *
 * THE MATERIAL IS COMPARED BY ADDRESS, NEVER BY VALUE. Two profiles with equal fields are equal in
 * every way except the one that matters: which library row a future retune moves. A by-value
 * comparison would go on passing against a private copy of Timber that the mean-strength re-anchor
 * never reached, and every joint the snap solver infers off that piece would be inferred from stale
 * numbers. It is also the same identity rule PieceActionsFor keeps for its action rows.
 *
 * THE UNKNOWN-KIND ROW IS THE FAIL-CLOSED ONE. EBuildPieceKind is a uint8 and a cast is all it takes
 * to produce a value nobody declared; what may not happen is a NaN or negative extent (a piece whose
 * mass is not a number) or a reference to something that is not a library row at all. This claim is
 * deliberately loose about WHICH answer an unknown kind gets — that is a decision this test is not
 * in a position to make — and strict about the answer being a real one.
 *
 * NEEDS A TICKING WORLD: no.
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

	/*
	 * A KIND THIS BUILD HAS NEVER HEARD OF. Neither answer may be garbage: an extent that is not a
	 * number becomes a mass that is not a number two calls later, and a reference to something
	 * outside the library is a dangling read dressed as a material.
	 */
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
 * THE BUILD PLANE OF A COURSE PUTS THE PIECE ON TOP OF THE GROUND, NOT HALF INSIDE IT: COURSE N'S
 * BOTTOM IS N BRICK COURSES UP AND THE PLANE IS THE PIECE'S OWN HALF-HEIGHT ABOVE THAT.
 *
 * THIS IS THE SLICE'S ONE RULING AND IT IS A CONVENTION CHANGE. Every harness this project has built
 * so far centres course 0 at Z = 0, which puts the grounded course half below the ground plane — a
 * render follow-up already records the picture that produces. A player placing the first brick of
 * their own building must see it sitting ON the ground, so the model answers 3.25 for a brick on
 * course 0 rather than 0. Nothing about the STRUCTURE changes: the solver reads relative positions
 * only, so this is the same building lifted by one brick half-height, which the integration claim
 * in this file measures rather than asserts by hand.
 *
 * THE PITCH IS THE BRICK'S, WHATEVER THE PIECE IS. 7.5 cm is 6.5 of brick plus a 1 cm bed joint, and
 * a course is a property of the WALL rather than of the thing being laid into it — a timber plate on
 * course 2 bears on two brick courses and their joints, which is exactly where the demo building
 * puts its own plate. A pitch derived from the piece would put the plate at its own doubled height
 * and the bearing would be imaginary.
 *
 * THE TEST SPELLS 7.5 OUT AND ALSO DERIVES IT. Production is required to read FSnapSettings rather
 * than hard-code the sum; this file asserts the sum against a default FSnapSettings SEPARATELY from
 * the plane, so a retune of the brick or the joint thickness fails on a row that names the retune
 * rather than silently agreeing with whatever the plane now returns. That is the DESIGN §3 rule
 * about conversion constants applied to a coordinating dimension.
 *
 * A NEGATIVE COURSE IS TREATED AS COURSE 0 — the fail-closed row, and the reason the state's own
 * comment says the course is never negative. The plane is a build height AND a player-facing
 * readout; below-ground is neither, and FMath::Max on an already-signed input is precisely the
 * spelling that turns a wrong course into a plausible height.
 *
 * NEEDS A TICKING WORLD: no. Two numbers in, one number out.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarCoursePlaneZTest,
	"DestructionGame.Core.SessionToolbar.CoursePlaneZ",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarCoursePlaneZTest::RunTest(const FString& Parameters)
{
	using namespace SessionToolbarTestSupport;
	using namespace DestructionSession;

	/*
	 * THE PITCH, DERIVED FROM THE SNAP SETTINGS THE SOLVER ALREADY USES. If this row fails, the
	 * brick or the joint has been retuned and every expected height below is stale — which is the
	 * whole reason it is asserted before them rather than folded into them.
	 */
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

	/*
	 * AND THE LADDER CLIMBS BY EXACTLY ONE PITCH PER COURSE, for both half-heights. The rows above
	 * pin three points; this pins the step between every pair of them, which is what makes an
	 * off-by-one-joint pitch — 6.5 instead of 7.5, the same mistake as forgetting the mortar — fail
	 * on the step rather than on a single arithmetic coincidence.
	 */
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
 * COURSE 0 IS THE ONE THAT TOUCHES THE EARTH, AND THE COURSE READOUT NAMES THE COURSE IT IS SHOWING.
 *
 * GROUNDED IS THE FLAG FStructure ROUTES LOAD TO, so "which course is grounded" is not a cosmetic
 * question — a piece laid with the flag set is a piece that absorbs whatever reaches it, and a whole
 * building marked grounded is a building that cannot fall. One course has the earth under it and
 * that is course 0; everything above is held up by what is below it or by nothing.
 *
 * THE LABEL IS THE MODEL'S FOR THE REASON EVERY OTHER CAPTION HERE IS, and the claim is bounded the
 * same way: it must contain the course number, it must never be blank, and two different courses
 * must never read the same — a readout that says the same thing on every course is a readout that
 * is not reading anything.
 *
 * AND THE NUMBER IT PRINTS COUNTS FROM ONE, WHICH IS NOT THE NUMBER IT IS STORING — OWNER-DELEGATED
 * RULING, 2026-09-15 (CURRENT_STATE finding P1). The session has TWO surfaces that name courses and
 * they disagreed: this strip printed the grounded course as "Course 0" while the piece menu's own
 * readout counts from one ("course 1 · #2" for a brick laid on that very plane, Core/PieceMenu.cpp,
 * "BOTH NUMBERS COUNT FROM ONE"). The session proof frames are what made it a defect rather than a
 * quibble — a player lays a brick on the strip's "Course 0" and the details window calls the brick
 * they just laid "course 1". One of the two had to move, and the one that moves is THIS one: a
 * person counting courses of brick starts at one and always has, and the readout the player spends
 * longest reading is the one naming individual bricks.
 *
 * FSessionToolbarState::Course STAYS ZERO-BASED, and that separation is the whole of the change.
 * CoursePlaneZCm, IsCourseGrounded and the stepper's floor are arithmetic over an index and are
 * untouched — course 0 is still the one with the earth under it and still planes at one half-height.
 * What moves is the one function that turns that index into words, which is why this is a label
 * change with no physical consequence and why the grounding rows above it are unchanged.
 *
 * A NEGATIVE COURSE ANSWERS AS COURSE 0 THROUGHOUT. The clamp is a property of the whole course
 * vocabulary rather than of one function: a below-ground course that reads "not grounded" on one
 * call and gets a course-0 build plane on the next is two functions disagreeing about a state that
 * is not supposed to exist, and that disagreement is what would make it survive.
 *
 * NEEDS A TICKING WORLD: no.
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

	/*
	 * THE TWO EXACT PINS, AND THEY ARE EXACT BECAUSE THE WHOLE CLAIM IS ONE OF AGREEMENT. A
	 * "contains its number" assertion is satisfied by either convention on most courses — "Course 1"
	 * contains a 1 whether it is the index or the index plus one — so the rows that separate the two
	 * conventions have to be spelled out in full.
	 */
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

	/*
	 * AND MUST NOT STILL PRINT THE INDEX. Without this row a label reading "Course 3 (index 3)" —
	 * or an implementation that printed both numbers to keep everyone happy — would satisfy the
	 * row above it, and the player would be back to reading two different numbers for one course.
	 */
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
 * WALKING THE DEMO BUILDING THROUGH THE MODEL REPRODUCES IT EXACTLY, LIFTED BY ONE BRICK
 * HALF-HEIGHT — THE SAME BUILDING, RESTING ON THE GROUND INSTEAD OF STRADDLING IT.
 *
 * WHY THIS IS THE INTEGRATION CLAIM AND WHY IT IS STILL WORLD-FREE. The toolbar model is not a new
 * way to describe buildings: it is a presenter over the snap solver and the placement API that
 * Core/BuildMode/DemoBuilding.cpp already drives, and the way to know it has not invented its own
 * geometry is to point it at the one building this project has already built and pinned. Every
 * number on the demo's side is READ OUT OF THE BUILDING ITSELF — BuildDemoBuilding is run into an
 * FBrickLayout here and the placed centres and extents are taken off Layout.Boxes — so if the
 * palette or the plane drifts away from what the demo uses, the two stop agreeing here rather than
 * in a screenshot somebody looks at later.
 *
 * READ, NOT TRANSCRIBED, AND THAT IS THE WHOLE POINT OF THE INDIRECTION. Three copied literals
 * (0, 7.5, 16.75) would keep passing after the demo migrated onto this very convention, because
 * they would still describe the building as it used to be. Reading the layout makes this test go
 * red AT the migration — which is exactly when somebody needs to be told that the lift is now zero
 * and this claim has been discharged — and not one commit before.
 *
 * THE ASSERTION IS THE OFFSET BEING THE SAME ON EVERY COURSE, not that any one height matches. A
 * model that put the plate at 20.0 by luck and the bricks somewhere else would satisfy a single
 * height check and would be a different building; a CONSTANT offset is the whole content of the
 * ruling — "same relative geometry, shifted to rest on the ground" — and it is what guarantees the
 * solver reads the identical structure, since the solver reads relative positions only.
 *
 * AND THE OFFSET IS NOT AN ARBITRARY CONSTANT. It must equal the brick's own half-height, because
 * that is what "course 0 centred at 0 becomes course 0 resting on the ground" means. Pinning it as a
 * free 3.25 would be pinning a coincidence.
 *
 * NEEDS A TICKING WORLD: no. The claim is about two sets of numbers, and the world-side proof that
 * the lifted building still stands belongs with the builder, not here.
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

	/*
	 * THE DEMO BUILDING, BUILT. World-free: BuildDemoBuilding grows an FBrickLayout through
	 * BuildMode::PlacePiece, so the boxes it leaves behind are the poses the snap solver actually
	 * committed — not a description of them.
	 */
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
 * EVERY CHIP KNOWS WHICH REGION OF THE STRIP IT IS IN AND WHICH PIECE IT LAYS, THE THREE REGIONS ARE
 * CONTIGUOUS AND IN ORDER, AND THE MODE PAIR IS EXACTLY THE FIRST REGION.
 *
 * =====================================================================================
 * THE BEHAVIOUR IN ONE SENTENCE
 * =====================================================================================
 *
 * `SessionToolbarButtons` answers, per button, which of the three regions of the strip it belongs to
 * (`Mode`, `Settings`, `Command`) and which piece swatch it carries (`Brick`, `Timber`, `None`), and
 * the list it returns is those regions in that order with no interleaving.
 *
 * =====================================================================================
 * WHY THE GROUP IS A MODEL ANSWER RATHER THAN A RUN OF AddSlot CALLS
 * =====================================================================================
 *
 * SESSION_UI_DESIGN §b draws the strip as three regions separated by 1 px rules, and the reason it
 * gives is not decoration: the commands sit past a rule "so that a destructive click is never
 * adjacent to a setting click". That is a decision about where `Clear build` may be, and a widget
 * that decided it by counting slots would hold the decision in the one place no test can reach —
 * Core/SessionToolbar.h's own argument, applied to the thing that separates the buttons rather than
 * to the buttons.
 *
 * CONTIGUITY IS THE CLAIM THAT MAKES A RULE DRAWABLE AT ALL. A widget draws a divider where the
 * group changes; if a group could appear twice, the strip would grow a rule in the middle of a
 * region and the three-region reading — the thing a player navigates by from peripheral vision —
 * would be gone. Asserting it here is what lets the Slate side simply compare neighbours.
 *
 * AND THE MODE PAIR IS EXACTLY THE FIRST GROUP, which is `SessionToolbarButtons`' existing ordering
 * promise restated in the new vocabulary: the two buttons that switch modes may not move, because a
 * strip whose first slots shifted would put a different button under a stationary cursor.
 *
 * THE SWATCH IS THE OTHER HALF OF §a's "the chip looks like the thing you are about to lay". It is
 * a KIND rather than a colour, for the reason `EJointMarginBand` is a band rather than a green: the
 * model decides that a brick chip carries a brick, and the widget decides which red.
 *
 * SWEPT OVER ALL 72 STATES, because both claims are about every strip the session can draw, and a
 * hand-picked state or two covers only the shapes the author had in mind.
 *
 * NEEDS A TICKING WORLD: no, and not even a world.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSessionToolbarGroupsAndSwatchesTest,
	"DestructionGame.Core.SessionToolbar.GroupsAndSwatches",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

bool FSessionToolbarGroupsAndSwatchesTest::RunTest(const FString& Parameters)
{
	using namespace SessionToolbarTestSupport;
	using namespace DestructionSession;

	/*
	 * THE GROUPS IN THE ORDER THEY MUST APPEAR, read as a rank rather than as the enumerator's own
	 * value — so the claim is "left to right, tabs then settings then command" rather than "the
	 * enumerators happen to be declared in that order", which is a different and much weaker thing.
	 */
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

	/*
	 * HOW MANY BUTTONS THE SWEEP OWES, ACCUMULATED FROM THE MODE AND SETTLED AT THE END. The same
	 * measured floor Core.SessionToolbar.Labels carries: every claim below is a loop over the buttons
	 * that came back, so a model returning nothing would satisfy all of them by having nothing to
	 * check.
	 */
	int32 ButtonsOwed = 0;
	int32 ButtonsSeen = 0;

	for (const FSessionToolbarState& State : AllStates())
	{
		const TArray<FToolbarButton> Buttons = SessionToolbarButtons(State);
		const bool bBuilding = State.Mode == ESessionMode::Build;

		ButtonsOwed += bBuilding ? 16 : 4;

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

			/*
			 * NON-DECREASING RANK IS CONTIGUITY AND ORDER IN ONE COMPARISON. Anything that appeared
			 * twice would have to come back after a higher rank, and anything out of order would
			 * step down; both show up here as the same failure.
			 */
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

		/*
		 * THE MODE PAIR IS EXACTLY THE FIRST GROUP: two buttons, both of them first. With the
		 * non-decreasing claim above, "exactly two are Mode" is enough to place them — but the two
		 * slots are asserted directly as well, because that is the player-facing promise and it
		 * deserves to fail by name.
		 */
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

		/*
		 * AND THE PALETTE CARRIES ONE BRICK AND TWO PLANKS, in the mode that draws it. Counted as
		 * well as checked per button so that a model answering `None` everywhere — the inert
		 * scaffold's own answer — fails on a row that says the swatches are missing rather than
		 * only on three per-button rows.
		 */
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
 * A CHIP'S LOOK IS A MODEL ANSWER: ROUNDED AND EDGED ALWAYS, THE MODE'S ACCENT WHEN IT IS THE ONE
 * YOU HAVE CHOSEN, FADED WHEN IT CANNOT BE CLICKED, AND THE "GO" CHIP LIT WITHOUT BEING LATCHED.
 *
 * =====================================================================================
 * THE BEHAVIOUR IN ONE SENTENCE
 * =====================================================================================
 *
 * `ChipLookFor(Button, Mode)` turns the two flags the model already decided — `bActive` and
 * `bEnabled` — plus the button's own identity into the fill, the caption colour, the caption weight
 * and the chip's geometry, so that three visual states are three and the accent on a lit chip is the
 * MODE's accent.
 *
 * =====================================================================================
 * WHY THE LOOK IS DECIDED HERE AND NOT IN SLATE
 * =====================================================================================
 *
 * §e states the rule and gives the reason in the same breath: "three visual states, and they must be
 * three, for the reason `EBrickHighlight` has ten and not one — `bActive` and `bEnabled` are
 * different questions and a widget that drew them alike would make a lit button that does nothing
 * indistinguishable from a greyed one that works." That is a decision with a player-facing
 * consequence, and today it is spelled as two ternaries inside `BuildSessionToolbarPanel` where
 * nothing can read it: the panel test can only say "the lit one looks different from the idle one",
 * which a decorative alternation would satisfy forever.
 *
 * WHAT IS PINNED EXACTLY AND WHAT IS PINNED AS A RELATION, deliberately:
 *
 *   - THE ACCENT IS EXACT. It is the design's own build amber and destroy red, which are the Caution
 *     band's gold and the destructive row's red — the same two colours this UI already uses — and
 *     equality is the point: a third and fourth hue for the same two ideas is two more things to keep
 *     in step with nothing holding them there.
 *
 *   - THE IDLE FILL IS NOT. §e's chip fill is a dark slate and nudging it is free; what may not drift
 *     is that it is NEITHER accent (an idle chip that reads as chosen is a lie about the session) and
 *     that it is not faded (an idle chip that reads as greyed is a lie about the click).
 *
 *   - THE CAPTION IS A LUMINANCE BAND rather than a triple, for the same reason.
 *
 * THE "GO" CHIP IS THE ROW THAT IS NOT A RESTATEMENT. `Run structure` is a COMMAND — the model's own
 * rule is that a command is never `bActive`, because "a latched Clear button reads as a mode the
 * player is stuck in" — and yet §e asks for it "filled in the destroy accent". Those two are only
 * compatible if the fill is a function of the BUTTON and not merely of `bActive`, which is exactly
 * what this test forces and what a widget writing `bActive ? Accent : Idle` cannot express.
 *
 * AND THE `Destroy` TAB AND `Run structure` MAY NOT BE THE SAME CHIP (the C2 row, section ONE-B).
 * That one is the "go" chip's own consequence rather than a new idea: the lit mode tab is filled with
 * the mode's accent and Run is filled with the destroy accent BY NAME, so on a Destroy strip they
 * come out identical — a latched tab and an irreversible verb, two slots apart, telling a glancing
 * player nothing. The claim is an INEQUALITY in fill or outline rather than a look, because which of
 * the two honest fixes is taken is the design's decision and not this test's.
 *
 * AND `Clear build` IS DANGER IN THE CAPTION, NOT IN THE FILL. It is the one irreversible control in
 * the Build group (`FPieceAction::bIsDestructive` is the house precedent that destructiveness is
 * data), but a chip filled destroy-red sitting on an amber strip would read as the mode you are in.
 * So: idle fill, warm caption — asserted as a relation between channels rather than as a hue.
 *
 * EVERY LOOK IS FINITE, which is the fail-closed row. A colour channel that is not a number becomes
 * a chip Slate draws as whatever the clamp happens to return, and DESIGN §4 is explicit that
 * `FMath::Max` discards a NaN and `FMath::Min` replaces it — so a degenerate look becomes a
 * plausible one rather than an obvious fault.
 *
 * NEEDS A TICKING WORLD: no, and not even a world — which is the whole reason the look is a free
 * function. `World.Session.ToolbarChipsAreRoundedAndGrouped` is where the widget is held against it.
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

	/*
	 * AND THEY ARE DIFFERENT COLOURS, which is the precondition every claim below leans on: "the lit
	 * chip is the MODE's accent" says nothing at all if the two modes share one.
	 */
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

	/*
	 * FAIL CLOSED ON A VALUE NOBODY DECLARED. Both enums are uint8 and a cast is all it takes; a
	 * swatch that is not a swatch must draw nothing rather than a plausible block, and an accent that
	 * is not a mode's must still be a colour Slate can draw.
	 */
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
	 * THE C2 ROW, AND IT IS A LEGIBILITY DEFECT RATHER THAN A PREFERENCE.
	 *
	 * On today's Destroy strip the lit `Destroy` tab and the `Run structure` command come out
	 * IDENTICAL — both are filled with the destroy accent, both carry the chip edge, both are bold
	 * dark ink — and they are two slots apart on the same bar. One of them is a statement about where
	 * the player already is; the other settles the wall, releases bricks and cannot be undone. A
	 * player scanning the strip has nothing to tell them apart but the words, which is exactly the
	 * reading §a principle 2 says must survive peripheral vision.
	 *
	 * ASSERTED AS AN INEQUALITY RATHER THAN AS A LOOK, DELIBERATELY. There are at least two honest
	 * fixes — Run keeps the idle fill and takes the accent in its OUTLINE ("outlined go"), or §b's tab
	 * treatment moves the mode pair to a fill-plus-top-bar of its own — and picking one here would be
	 * this test deciding the design. What may not stand is the two being indistinguishable, so the
	 * claim is that they differ in the FILL or in the OUTLINE, RGB-exactly, in at least one of the
	 * two.
	 *
	 * FILL OR OUTLINE AND NOT THE CAPTION, BECAUSE THE CAPTION IS NOT THE THING BEING READ HERE. A
	 * player who is reading the captions has already told them apart; the failure is the glance that
	 * does not.
	 *
	 * ALPHA IS EXCLUDED FROM THE COMPARISON — ColoursExactlyEqualRGB — for the reason every other
	 * "these must differ" row in this file excludes it: a chip distinguished only by being slightly
	 * more transparent over a near-black bar is not distinguished.
	 *
	 * SWEPT OVER EVERY DESTROY STATE THAT HAS SOMETHING TO RUN, because with nothing built Run is
	 * greyed and the two are ALREADY different for a reason that has nothing to do with this.
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

			/* EVERY CHIP IS A NUMBER. The fail-closed row, and it is cheap. */
			TestTrue(
				*FString::Printf(
					TEXT("%s — every channel of every colour must be finite; a NaN becomes whatever "
						 "the clamp returns rather than an obvious fault"),
					*Where),
				ColourIsFinite(Look.Fill) && ColourIsFinite(Look.Outline)
					&& ColourIsFinite(Look.Caption)
					&& FMath::IsFinite(Look.CornerRadiusPx) && FMath::IsFinite(Look.OutlineWidthPx));

			/* ROUNDED AND EDGED, ALWAYS — §a principle 1's chunky chip with its 2 px drop edge. */
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

				/*
				 * DISABLED IS FADED, AND FADED IS NOT ACCENTED. The model already refuses the click;
				 * what this stops is the strip telling the player the click is going to do something.
				 */
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

				/*
				 * THE "GO" CHIP. A command is never bActive — the model guarantees it — so a widget
				 * that filled on bActive alone could never draw this, and §e asks for it anyway.
				 */
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

				/*
				 * IDLE: NOT CHOSEN AND NOT GREYED, AND IT MUST LOOK LIKE NEITHER. The exact fill is
				 * the widget's to nudge; these two are what may not drift.
				 */
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

					/*
					 * DANGER IN THE CAPTION, NOT IN THE FILL. A chip filled destroy-red on an amber
					 * strip would read as the mode you are in; a warm caption reads as a warning.
					 */
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

			/*
			 * AND NO CHIP OF A BUILD STRIP MAY WEAR THE DESTROY ACCENT IN ITS FILL. The mode's accent
			 * is the MODE's: a red chip on an amber strip is the one thing that would make "which
			 * mode am I in" unreadable from the corner of an eye.
			 */
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

		/*
		 * EXACTLY ONE LIT CHIP PER SETTING GROUP, COUNTED THROUGH THE FILL. Core.SessionToolbar's
		 * ActiveFlags already counts bActive; what this counts is the chips that came out ACCENTED,
		 * which is the same claim one layer further on — a look function that ignored bActive would
		 * satisfy ActiveFlags forever and light nothing.
		 */
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

	/*
	 * AND EVERY ONE OF THE FIVE SHAPES WAS ACTUALLY REACHED. Four of the five arms above are inside
	 * an if/else over the model's own flags, so a model that never greyed anything — or a strip that
	 * drew nothing — would leave whole arms unentered and every claim in them unmade.
	 */
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
 * UI-6 — THE PLAYER CHOOSES WHAT FASTENS THE NEXT PIECE: SIX CHIPS, ONE LIT, AND EACH NAMES THE
 * SHIPPED LIBRARY ROW EVERY JOINT THE NEXT PLACEMENT FORMS WILL CARRY.
 *
 * =====================================================================================
 * THE BEHAVIOUR IN ONE SENTENCE
 * =====================================================================================
 *
 * `FSessionToolbarState` carries an `EJointChoice`; the Build strip draws it as a six-chip
 * segmented control in the Settings group between the placement pair and the course stepper, with
 * exactly the chosen one lit and all six live; and `JointOverrideFor` turns that choice into either
 * NOTHING (Auto — the inference keeps deciding) or the ADDRESS of one shipped connection profile.
 *
 * =====================================================================================
 * WHY THE OVERRIDE IS A POINTER AND WHY nullptr IS A REAL ANSWER
 * =====================================================================================
 *
 * BUILD_MODE_PLAN's UI-6 makes the override ride through `PreviewBuildPiece`/`PlaceBuildPiece` as
 * an optional `const FConnectionStrength*`, and Auto has to be expressible in the same type — a
 * sentinel profile meaning "infer" would be a seventh library row that every consumer has to know
 * to special-case, and the first one that forgot would BOND A JOINT WITH IT. So Auto is nullptr
 * and every other choice is one row's address.
 *
 * AND THE ADDRESS IS THE CLAIM, NEVER THE FIELDS. Two FConnectionStrengths with equal fields are
 * equal in everything except which row a retune moves, and this library is siblings by
 * construction: Nail, Screw and Bolt are one shape at three scales and the two mortars differ on
 * two axes. A by-value answer would go on passing against a private copy that the next re-anchor
 * never reaches, and every joint the player screwed would be screwed with stale numbers. It is the
 * same identity rule `BuildPieceMaterial` keeps for the palette — and it is why
 * `FNamedConnectionProfile::Strength` became a REFERENCE to the extern in this slice: held BY VALUE,
 * as it was, it repeated exactly the trap that made the MATERIAL lookup answer "no such row" for
 * every piece in the game until its own field became a reference.
 *
 * =====================================================================================
 * WHAT THIS TEST OWNS THAT THE SWEEPS ABOVE DO NOT
 * =====================================================================================
 *
 * `AllStates` now carries the joint choice as a dimension, so ButtonsByMode, ActiveFlags,
 * EnabledFlags, Labels and Transitions already sweep it — which chips exist in which mode, exactly
 * one lit, all six live, the six captions, and the transitions including the round trip through
 * Destroy. What is left here is the two claims none of those can make: the chips' POSITION inside
 * the Build strip (a contiguous run after Free and before Course down), and the choice-to-profile
 * map itself.
 *
 * NEEDS A TICKING WORLD: no. One plain struct in, an array of plain structs and one pointer out.
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

			/*
			 * NO SWATCH, AND THAT IS A CLAIM RATHER THAN A DEFAULT. A swatch names the PIECE the
			 * chip lays; a block of brick red on a chip that chooses mortar would say the chip
			 * lays a brick, which is the one thing it does not do.
			 */
			TestEqual(
				*FString::Printf(
					TEXT("%s: %s must carry no swatch, it carries %s — [%s]"),
					*DescribeState(State), NameOfButton(Id), NameOfSwatch(Button->Swatch),
					*DescribeButtons(Buttons)),
				static_cast<int32>(Button->Swatch), static_cast<int32>(EToolbarSwatch::None));

			/*
			 * AND ALL SIX ARE LIVE IN EVERY BUILD STATE. A joint choice has no precondition at all
			 * — it describes the NEXT placement, so it is settable before a single brick is laid
			 * and on the grounded course, which is exactly where a player decides how to start.
			 */
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

	/* --- THREE: the choice-to-profile map, BY ADDRESS ---------------------------------------- */

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

	/*
	 * AND NO TWO CHOICES MAY ANSWER WITH THE SAME ROW. Six chips that all override with mortar is a
	 * control that looks like it works, commits one physics whatever the player picks, and is
	 * invisible until the structure runs. The rows above pin each answer; this pins that they are
	 * six ANSWERS.
	 */
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
	 * A CHOICE THIS BUILD HAS NEVER HEARD OF OVERRIDES **NOTHING**. EJointChoice is a uint8 and a
	 * cast is all it takes to make one.
	 *
	 * THE ROW USED TO ACCEPT "nullptr OR ANY SHIPPED ROW", AND THAT IS NO LONGER THE HONEST CLAIM.
	 * Two things have since committed to the stricter one. `JointOverrideFor`'s own header says an
	 * unheard-of choice hands the joint back to the inference, because a plausible row would fasten
	 * it with a profile nobody picked. And `SessionToolbarIsActive` now DERIVES the Auto chip's lit
	 * state from exactly this answer (`JointOverrideFor(State.Joint) == nullptr`) rather than from
	 * `State.Joint == Auto` — so a build that answered an unknown choice with a row would draw a
	 * six-chip segmented control with NO chip lit at all, over a session that is in fact being
	 * fastened by the inference. The looser row would pass over that.
	 *
	 * AND IT IS STILL A POINTER WORTH FOLLOWING — the shipped-row check is kept as the second half,
	 * so a garbage pointer fails as a garbage pointer rather than merely as "not null".
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

#endif // WITH_DEV_AUTOMATION_TESTS
