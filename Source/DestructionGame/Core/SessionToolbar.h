// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Profiles/MaterialProfiles.h"

/**
 * THE SESSION TOOLBAR'S PRESENTER MODEL — what the build/destroy strip offers, which button is lit,
 * which is greyed, and what one click does to the session.
 *
 * IT IS Core/PieceMenu.h'S ROLE FOR THE TOOLBAR, and it is here for the same argument. A strip of
 * buttons is a list of decisions — which buttons exist in which mode, in which order, with which
 * caption, lit or not, live or greyed — and a decision spelled as a run of AddSlot calls in Slate is
 * a decision in the one place no test can reach. What is left for the widget is drawing: which
 * green, which font, which margin.
 *
 * NO SLATE, NO WORLD, NO UObject. One plain struct in, plain structs out, exactly as
 * BuildPieceMenuRows and PieceMenuPanelSizePx take. That is what makes every claim about the strip
 * a headless microsecond rather than a viewport somebody has to look at.
 */
namespace DestructionSession
{
	/**
	 * What the session is doing: laying pieces, or pulling them out.
	 *
	 * Build IS ENUMERATOR ZERO because a default-constructed session must be the one that CANNOT
	 * destroy anything. The two modes are not symmetric in what a mistake costs: opening in Build
	 * offers a ghost nobody asked for, opening in Destroy offers a click that removes a brick.
	 */
	enum class ESessionMode : uint8
	{
		/** Placing pieces. The palette, the placement mode and the course are all live. */
		Build,

		/** Pulling pieces out and watching what happens. The build settings are kept for the way back. */
		Destroy,
	};

	/**
	 * Which piece the ghost is.
	 *
	 * A KIND RATHER THAN AN EXTENT AND A MATERIAL ON THE STATE. The palette is data — an extent and
	 * a library row per kind, below — and a session state that carried the numbers instead of the
	 * name would be a fourth place brick dimensions are written down, free to drift from the three
	 * that already agree.
	 */
	enum class EBuildPieceKind : uint8
	{
		/** The standard 21.5 x 10.25 x 6.5 cm clay unit this whole project is calibrated on. */
		Brick,

		/** The demo building's own 67.5 cm timber wall plate. */
		TimberPlate,

		/** That plate's 90 cm sibling, for spanning an opening. */
		TimberLintel,
	};

	/** Whether a placement is pulled onto the bond, or dropped exactly where the cursor is. */
	enum class EPlacementMode : uint8
	{
		/** The solver's ranked snap wins. The normal way to lay a wall. */
		Snap,

		/** The requested pose is honoured verbatim, overlaps and all. The deliberate escape. */
		Free,
	};

	/**
	 * What fastens the next piece the player lays.
	 *
	 * Auto IS ENUMERATOR ZERO AND IT IS NOT "NO CHOICE". It is the choice that hands every joint
	 * back to BuildMode::JointForContact, which is what makes a brick bed in mortar and a plank bear
	 * dry without the player having to say so — the answer a default-constructed session must give,
	 * for the reason Mode defaults to Build: the default is the one that decides least.
	 *
	 * A CHOICE RATHER THAN A PROFILE POINTER ON THE STATE, for the reason EBuildPieceKind is a kind
	 * rather than an extent and a material. The state is what the strip draws and what a save file
	 * would carry; a raw pointer on it would be a library address serialised into a session, and the
	 * chip that is lit would be decided by comparing one.
	 *
	 * THE FIVE NAMED ONES ARE LIBRARY ROWS, ONE EACH, and they are the whole vocabulary a player
	 * gets — JointOverrideFor below is the map. There is deliberately no perpend chip: the weak
	 * perpend is a thing the INFERENCE chooses for a vertical face, not a thing anybody asks for.
	 */
	enum class EJointChoice : uint8
	{
		/** The inference decides, per joint, exactly as it did before there was a chip. */
		Auto,

		/** The full general-purpose bed bond, in every joint the piece forms. */
		Mortar,

		/** No bond at all: compression and friction only. */
		Dry,

		Nail,
		Screw,
		Bolt,
	};

	/**
	 * Everything the toolbar knows about the session, and the only thing a click changes.
	 *
	 * DURABLE STATE RATHER THAN WIDGET STATE. A controller that kept the mode in one field, the
	 * piece in a combo box and the course in a spinner has the session spread across four owners
	 * that can disagree; kept as one struct, every transition is state-in/state-out and every
	 * transition is therefore a table row in a test.
	 */
	struct FSessionToolbarState
	{
		ESessionMode Mode = ESessionMode::Build;

		EBuildPieceKind Piece = EBuildPieceKind::Brick;

		EPlacementMode Placement = EPlacementMode::Snap;

		/**
		 * What fastens every joint the next placement forms.
		 *
		 * IT SURVIVES A TRIP THROUGH DESTROY MODE, which is the half this field exists to get wrong.
		 * The Destroy strip draws none of the six chips, and ApplyToolbarButton's "the fields a
		 * transition does not name survive it" rule is what brings the player back to the screws they
		 * chose rather than to Auto — a difference they cannot see until the structure is run.
		 */
		EJointChoice Joint = EJointChoice::Auto;

		/**
		 * Which course the build plane is on. NEVER NEGATIVE.
		 *
		 * Course 0 is the one with the earth under it. A negative course is a build plane below the
		 * ground and a readout nobody can make sense of, so ApplyToolbarButton refuses to produce
		 * one and the three course functions below all treat one as course 0 — one clamp spelled
		 * once, rather than four functions each with their own opinion about a state that is not
		 * supposed to exist.
		 */
		int32 Course = 0;

		/**
		 * Whether there is a live structure for the commands to act on.
		 *
		 * THE ONLY PRECONDITION EITHER COMMAND HAS. Clear with nothing built clears nothing and Run
		 * with nothing built solves an empty structure; both are silent no-ops, and a silent no-op
		 * on a command button is indistinguishable from the game having missed the click.
		 */
		bool bHasStructure = false;

		/**
		 * Whether the DESTROY-mode load overlay is on — every live piece tinted by its worst
		 * joint's margin band.
		 *
		 * A SETTING RATHER THAN A COMMAND, SO IT LATCHES. It is a way of looking at the structure
		 * rather than a thing that happens to it, which is what makes its chip lit while it is on
		 * and what makes a second click turn it off.
		 *
		 * OFF BY DEFAULT, for the reason Mode defaults to Build: a default-constructed session must
		 * be the one that has done the least. The overlay costs a solve per toggle-on and per
		 * mutation, and a session that opened paying it would pay it on twenty-eight scenario levels
		 * that never asked.
		 *
		 * IT SURVIVES A TRIP THROUGH BUILD MODE. The Build strip does not draw the chip, and
		 * ApplyToolbarButton's "the fields a transition does not name survive it" rule is what keeps
		 * the player's choice waiting for them when they come back.
		 */
		bool bLoadOverlay = false;
	};

	/**
	 * Every button the strip can carry.
	 *
	 * AN ID RATHER THAN A CAPTION IS WHAT MAKES A CLICK ATTRIBUTABLE. A widget that reported which
	 * button was pressed by handing back its text would be routing behaviour through wording, which
	 * is the one thing on the row that is allowed to be retuned.
	 */
	enum class EToolbarButtonId : uint8
	{
		ModeBuild,
		ModeDestroy,
		PieceBrick,
		PieceTimberPlate,
		PieceTimberLintel,
		PlacementSnap,
		PlacementFree,

		/**
		 * The six joint chips, in the enum's own order, drawn between the placement pair and the
		 * course stepper. Both are segmented controls over the NEXT placement — how the piece is
		 * positioned, then how it is fastened — so they read as one band, and neither is next to the
		 * irreversible command past the rule.
		 */
		JointAuto,
		JointMortar,
		JointDry,
		JointNail,
		JointScrew,
		JointBolt,

		CourseDown,
		CourseUp,

		/** The Destroy strip's one setting: tint every piece by its worst joint's margin band. */
		ToggleLoadOverlay,

		ClearBuild,
		RunStructure,
	};

	/**
	 * Which of the three regions of the strip a button sits in.
	 *
	 * A REGION IS A MODEL ANSWER RATHER THAN A RUN OF AddSlot CALLS, for the reason the list itself
	 * is. SESSION_UI_DESIGN §b draws the strip as mode tabs, then the current mode's settings, then
	 * the mode's one command, separated by 1 px rules — and the reason it gives is not decoration:
	 * the commands sit past a rule "so that a destructive click is never adjacent to a setting
	 * click". That is a decision about where Clear build may be, and a widget that decided it by
	 * counting slots would hold it where nothing can read it.
	 *
	 * THE THREE ARE CONTIGUOUS AND IN THIS ORDER, which is what makes a rule drawable at all: the
	 * widget compares neighbours and draws a hairline where the group changes. A group appearing
	 * twice would put a rule in the middle of a region and the three-region reading — the thing a
	 * player navigates by from peripheral vision — would be gone.
	 */
	enum class EToolbarGroup : uint8
	{
		/** The two mode tabs. Always the first slots, and they never move. */
		Mode,

		/** Everything that changes with the mode: the palette, the placement pair, the course stepper. */
		Settings,

		/** The mode's one command, past a rule from the settings. */
		Command,
	};

	/**
	 * Which little block of colour a chip carries: the thing the player is about to lay, or nothing.
	 *
	 * A KIND RATHER THAN A COLOUR, for the reason EJointMarginBand is a band rather than a green
	 * (SESSION_UI_DESIGN §a principle 6). The model decides that a brick chip carries a brick; which
	 * red that is is the widget's, through SwatchColour below.
	 */
	enum class EToolbarSwatch : uint8
	{
		/** Nothing to draw. Every chip that is not a piece chip. */
		None,

		/** The clay unit. */
		Brick,

		/** Both boards — the plate and the lintel are one material at two lengths. */
		Timber,
	};

	/** One button on the strip: what it is, what it reads, whether it is lit, and whether it is live. */
	struct FToolbarButton
	{
		/** Which button this is. What a click reports back, and never the caption. */
		EToolbarButtonId Id = EToolbarButtonId::ModeBuild;

		/**
		 * What the button reads.
		 *
		 * DATA ON THE ROW FOR THE REASON FPieceMenuRow::Label IS: a widget spelling its own text
		 * holds the wording where no test can read it, and this strip's wording is load-bearing —
		 * Snap versus Free is the difference between a piece that lands on the bond and one that
		 * lands where the cursor was.
		 */
		FString Label;

		/**
		 * "THIS IS WHAT YOU HAVE CHOSEN", which is not the same question as bEnabled.
		 *
		 * Exactly one button of each setting group carries it, and a COMMAND never does. A latched
		 * Clear button reads as a mode the player is stuck in, and the toolbar has two real modes
		 * already.
		 */
		bool bActive = false;

		/**
		 * Whether the thing behind the button can actually happen.
		 *
		 * THE MODEL OWNS THE GREYING BECAUSE THE MODEL OWNS THE REFUSAL — ApplyToolbarButton reads
		 * this same answer rather than deciding again. Two derivations of "can this happen" is how
		 * a lit button that does nothing gets shipped.
		 *
		 * FALSE BY DEFAULT: a half-built row must not claim a click will land.
		 */
		bool bEnabled = false;

		/**
		 * Which region of the strip the button is in.
		 *
		 * Command BY DEFAULT, AND THAT IS THE FAIL-CLOSED END. A row nobody filled in draws past the
		 * last rule, on its own, rather than claiming to be one of the two mode tabs that may never
		 * move — an undeclared button at the far right is visibly odd, and an undeclared button
		 * sitting in the mode pair is a strip whose first slots have shifted.
		 */
		EToolbarGroup Group = EToolbarGroup::Command;

		/** Which piece the chip lays, if it lays one. Nothing to draw, by default. */
		EToolbarSwatch Swatch = EToolbarSwatch::None;
	};

	/**
	 * HOW ONE CHIP IS DRAWN — the fill, the edge, the caption and the chip's own geometry.
	 *
	 * DECIDED HERE AND NOT IN SLATE, which is SESSION_UI_DESIGN §e's rule with its reason attached:
	 * "three visual states, and they must be three, for the reason EBrickHighlight has ten and not
	 * one — bActive and bEnabled are different questions and a widget that drew them alike would make
	 * a lit button that does nothing indistinguishable from a greyed one that works". Spelled as two
	 * ternaries inside the panel builder, that decision is unreadable: a widget test can only say
	 * "the lit one looks different from the idle one", which a decorative alternation satisfies
	 * forever.
	 *
	 * COLOURS, NOT BRUSHES. Nothing in this file knows what Slate is. The widget turns these into a
	 * rounded-box brush and its hover and press variants, which is drawing rather than deciding.
	 */
	struct FChipLook
	{
		/** What the chip is filled with. The mode's accent when it is the one you have chosen. */
		FLinearColor Fill = FLinearColor::Transparent;

		/**
		 * The 2 px drop edge around it — §a principle 1's "press me" cue.
		 *
		 * AND THE ONE CHANNEL THAT TELLS THE "GO" CHIP FROM A LIT MODE TAB. `Run structure` is filled
		 * with the destroy accent by identity and the lit `Destroy` tab is filled with the mode's,
		 * which on a Destroy strip is the same red two slots along — a statement about where the
		 * player is, drawn exactly like the command that settles the wall. Both fills are spoken for,
		 * so the ring is where the difference lives: the go chip wears a bright rim and everything
		 * else wears the shadow.
		 */
		FLinearColor Outline = FLinearColor::Transparent;

		/** What the caption is written in. Dark ink on a lit chip, a readable grey on an idle one. */
		FLinearColor Caption = FLinearColor::White;

		/** How round the chip's corners are, in pixels. The same on every chip of every strip. */
		float CornerRadiusPx = 0.0f;

		/** How thick the drop edge is, in pixels. Likewise the same on every chip. */
		float OutlineWidthPx = 0.0f;

		/**
		 * Whether the caption is set in the bold face.
		 *
		 * THE CHIP SAYS bActive TWICE, in the fill and in the weight, because they are two readings
		 * of one decision: the fill is what a player flying a camera catches from the corner of an
		 * eye, and the weight is what a player looking straight at the strip reads.
		 */
		bool bBoldCaption = false;
	};

	/**
	 * The strip for a session state: one fixed list per mode, in one fixed order.
	 *
	 * THE MODE PAIR IS ALWAYS FIRST, and it is the one ordering claim with a player-facing reason.
	 * Everything else on the strip changes with the mode; the two buttons that switch modes may not
	 * move, because a strip whose first two slots shifted would put a different button under a
	 * cursor that has not moved.
	 *
	 * THE LIST'S CONTENT DEPENDS ON THE MODE ALONE. A piece selection or a course number that added,
	 * removed or reordered a button would be a strip that rearranges itself while a player uses it.
	 */
	TArray<FToolbarButton> SessionToolbarButtons(const FSessionToolbarState& State);

	/**
	 * One click, as a pure function: the state before and the button, in; the state after, out.
	 *
	 * PURE BECAUSE THAT IS THE ONLY WAY IT IS ASSERTABLE. A controller mutating its own fields from
	 * a Slate callback puts the whole of the toolbar's behaviour behind a click only a human can
	 * perform; state-in/state-out makes every transition a table row.
	 *
	 * IT REFUSES WHATEVER THE STRIP REFUSES, by asking SessionToolbarButtons rather than by
	 * re-deciding: a button that state does not draw, or draws greyed, is a bitwise no-op. That
	 * includes the course floor — down from the grounded course is REFUSED rather than clamped into
	 * a new state, which is the same answer today and stops being the same answer the moment
	 * anything else on the state moves with a course change.
	 *
	 * THE FIELDS A TRANSITION DOES NOT NAME SURVIVE IT. Going to Destroy and back must return a
	 * player to the piece, the placement and the course they left with.
	 */
	FSessionToolbarState ApplyToolbarButton(const FSessionToolbarState& State, EToolbarButtonId Id);

	/**
	 * A MODE'S ACCENT — build amber, destroy red.
	 *
	 * THE TWO COLOURS THIS UI ALREADY USES, reused rather than re-picked: the amber is the Caution
	 * band's gold and the ghost's own colour, and the red is the destructive row's. A third and
	 * fourth hue for the same two ideas would be two more things to keep in step with nothing
	 * holding them there.
	 *
	 * IT IS THE MODE'S RATHER THAN THE BUTTON'S, and that is the whole point of taking a mode: every
	 * lit chip on a Build strip is amber and every lit chip on a Destroy strip is red, so the colour
	 * of the strip is itself a reading of which mode the player is in — the fact they need from
	 * peripheral vision while flying a camera.
	 *
	 * A MODE THIS BUILD HAS NEVER HEARD OF ANSWERS WITH THE DESTROY ACCENT, which is the answer that
	 * agrees with the rest of the model rather than an arbitrary one: SessionToolbarButtons draws the
	 * Destroy strip for anything that is not Build, so the strip and its accent stay one reading.
	 */
	FLinearColor ModeAccent(ESessionMode Mode);

	/**
	 * What a swatch kind is painted in — and it is the colour of the thing the player will lay.
	 *
	 * THE SHED MATERIALS' OWN BASE COLOURS, so the palette chip and the brick that lands are one
	 * decision rather than two people picking the same red.
	 *
	 * None IS TRANSPARENT, AND SO IS A KIND NOBODY DECLARED. The swatch is drawn through one widget
	 * whichever kind it is, so "nothing to draw" has to be a colour; a plausible block of colour on
	 * a command chip would name a piece that chip does not lay.
	 */
	FLinearColor SwatchColour(EToolbarSwatch Swatch);

	/**
	 * How one chip of one mode's strip is drawn.
	 *
	 * THE PRECEDENCE IS !bEnabled, THEN bActive, THEN THE "GO" CHIP, THEN IDLE, and the order is the
	 * claim rather than an implementation detail. A greyed Run structure must read as greyed even
	 * though Run is the one chip that is filled without being lit; a lit chip must read as lit even
	 * though it is also enabled.
	 *
	 * THE "GO" CHIP IS WHY THIS TAKES A BUTTON AND NOT MERELY TWO FLAGS. A command is never bActive
	 * — a latched Clear button reads as a mode the player is stuck in — and §e still asks for Run
	 * structure "filled in the destroy accent". The two are only compatible if the fill is a function
	 * of the BUTTON, which is exactly what a widget writing `bActive ? Accent : Idle` cannot express.
	 *
	 * AND Clear build IS DANGER IN THE CAPTION, NOT IN THE FILL. It is the one irreversible control
	 * in the Build group, but a chip filled destroy-red sitting on an amber strip would read as the
	 * mode you are in — so the warning is a warm caption on an ordinary idle chip.
	 */
	FChipLook ChipLookFor(const FToolbarButton& Button, ESessionMode Mode);

	/**
	 * What a joint choice OVERRIDES every formed joint's profile with — or NOTHING, for Auto.
	 *
	 * A POINTER, AND nullptr IS A REAL ANSWER RATHER THAN A FAILURE. The override rides through
	 * PreviewBuildPiece / PlaceBuildPiece as an optional profile, and "let the inference decide" has
	 * to be expressible in that same type: a sentinel profile meaning "infer" would be a seventh
	 * library row every consumer has to know to special-case, and the first one that forgot would
	 * BOND A JOINT WITH IT.
	 *
	 * IT IS THE SHIPPED ROW'S OWN ADDRESS, NEVER A COPY, which is the identity rule
	 * BuildPieceMaterial keeps for the palette and for the same reason. Two FConnectionStrengths with
	 * equal fields are equal in everything except which row a retune moves, and this library is
	 * siblings by construction — Nail, Screw and Bolt are one shape at three scales. A copy would go
	 * on serving stale numbers after a re-anchor, and every joint the player screwed would be screwed
	 * with them.
	 *
	 * A CHOICE THIS BUILD HAS NEVER HEARD OF OVERRIDES NOTHING, AND THAT IS THE FAIL-CLOSED END.
	 * EJointChoice is a uint8 and a cast is all it takes to make one; answering with a plausible row
	 * would fasten a joint with a profile nobody picked, where answering with nothing hands it back
	 * to the inference that decided every joint in this game before the chip existed.
	 */
	const FConnectionStrength* JointOverrideFor(EJointChoice Joint);

	/**
	 * A piece kind's HALF extent, in centimetres.
	 *
	 * TRANSCRIBED FROM WHAT THE DEMO BUILDING ALREADY LAYS rather than newly authored: the brick is
	 * the standard 21.5 x 10.25 x 6.5 unit halved, and the plate is
	 * Core/BuildMode/DemoBuilding.cpp's own (33.75, 5.125, 5.0). The lintel is that plate's 90 cm
	 * sibling.
	 *
	 * IT FAILS CLOSED ON A KIND THIS BUILD DOES NOT KNOW. EBuildPieceKind is a uint8 and a cast is
	 * all it takes to make one; the answer is the ZERO extent, because an extent that is not a
	 * number is a mass that is not a number two calls later, and a piece of no size is an obvious
	 * refusal rather than a plausible brick.
	 */
	FVector BuildPieceHalfExtentCm(EBuildPieceKind Kind);

	/**
	 * A piece kind's material, BY REFERENCE TO THE SHIPPED LIBRARY ROW.
	 *
	 * NEVER A COPY, AND IDENTITY IS THE POINT. Two profiles with equal fields are equal in every way
	 * except the one that matters: which row a future retune moves. A private copy of Timber would
	 * go on serving stale numbers after a re-anchor, and every joint the snap solver infers off that
	 * piece would be inferred from them. It is the same identity rule PieceActionsFor keeps.
	 *
	 * IT FAILS CLOSED ON A KIND THIS BUILD DOES NOT KNOW, and Timber is the fail-closed answer
	 * rather than an arbitrary one: it is not compression-dominant, so BuildMode::JointForContact
	 * infers DryStone for it — a bearing that carries compression and friction and NO tension, the
	 * weakest joint the inference can hand out. An unknown piece is credited with nothing it has not
	 * earned.
	 */
	const DestructionProfiles::FMaterialProfile& BuildPieceMaterial(EBuildPieceKind Kind);

	/**
	 * The Z a piece of the given half-height takes when it is laid on the given course, in cm.
	 *
	 * THE RESTS-ON-THE-GROUND CONVENTION — OWNER-DELEGATED RULING, 2026-09-15. Every harness this
	 * project had built before this model centred course 0 at Z = 0, which puts the grounded course
	 * half BELOW the ground plane; the build-mode render follow-up records the half-buried bottom
	 * row that produces. A player laying the first brick of their own building must see it sitting
	 * ON the ground, so course 0 answers with the piece's own half-height. Nothing about the
	 * STRUCTURE changes — the solver reads relative positions only — so this is the same building
	 * lifted by exactly one brick half-height, on every course.
	 *
	 * THE PITCH IS THE BRICK'S, WHATEVER THE PIECE IS: BrickSizeCm.Z plus one bed joint, read from
	 * BuildMode::FSnapSettings rather than written down again. A course is a property of the WALL
	 * rather than of the thing being laid into it — a timber plate on course 2 bears on two brick
	 * courses and their joints, which is exactly where the demo building puts its own plate, and a
	 * pitch derived from the piece would put the plate at its own doubled height with the bearing
	 * imaginary.
	 *
	 * A NEGATIVE COURSE IS COURSE 0, like every other course function here.
	 */
	double CoursePlaneZCm(int32 Course, double PieceHalfHeightCm);

	/**
	 * Whether the toolbar INTENDS the build plane to sit on the earth, which is true of course 0
	 * and nothing else. THIS IS THE TOOLBAR'S INTENT FOR THE PLANE, NOT THE COMMITTED PIECE'S FLAG.
	 *
	 * GROUNDED IS THE FLAG FStructure ROUTES LOAD TO, so this is not a cosmetic question: a piece
	 * laid with it set absorbs whatever reaches it, and a whole building marked grounded cannot
	 * fall. And the snap solver ranks candidates by raw distance, so a cursor on the course-0 plane
	 * beside a standing brick can be snapped UP onto its next-course bed — a piece bedded on another
	 * brick, 7.5 cm above the earth, that this function would still call grounded. The flag the
	 * committed piece carries must therefore be derived from the SNAPPED POSE (its bottom face
	 * within a joint of the ground), never from the course this reports; this answer only says
	 * which readout the toolbar shows. A negative course answers as course 0 — the clamp is a
	 * property of the whole course vocabulary, because a below-ground course reading "not
	 * grounded" on one call and getting a course-0 build plane on the next is two functions
	 * disagreeing about a state that is not supposed to exist.
	 */
	bool IsCourseGrounded(int32 Course);

	/**
	 * The course readout, naming its own course — AND IT COUNTS FROM ONE WHERE THE INDEX IT IS
	 * GIVEN COUNTS FROM ZERO. CourseLabel(0) reads "Course 1".
	 *
	 * OWNER-DELEGATED RULING, 2026-09-15. The session has two surfaces that name a course and they
	 * disagreed: this strip printed the grounded course as "Course 0" while the piece menu's entry
	 * rows have counted from one since they were written (Core/PieceMenu.cpp, "BOTH NUMBERS COUNT
	 * FROM ONE"), so the proof frames show a player laying a brick on "Course 0" that the details
	 * window then calls "course 1 · #1". The readout a player spends longest reading is the one
	 * naming individual bricks, so the strip is what moved.
	 *
	 * FSessionToolbarState::Course IS STILL THE ZERO-BASED INDEX, and that separation is the whole
	 * of the change: CoursePlaneZCm, IsCourseGrounded and the stepper's floor are arithmetic over
	 * an index and none of them moved. A negative course still reads as course 0 does, which is
	 * now "Course 1".
	 */
	FString CourseLabel(int32 Course);
}
