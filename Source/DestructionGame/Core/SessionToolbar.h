// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/Profiles/MaterialProfiles.h"

/**
 * The session toolbar's presenter model — what the build/destroy strip offers, which button is
 * lit, which is greyed, and what one click does to the session.
 *
 * Core/PieceMenu.h's role for the toolbar, for the same reason: a strip of buttons is a list of
 * decisions (which buttons, which order, caption, lit or greyed), and spelled as AddSlot calls in
 * Slate that decision sits where no test can reach it. The widget is left only to draw.
 *
 * No Slate, no world, no UObject — plain structs in and out, exactly as BuildPieceMenuRows and
 * PieceMenuPanelSizePx take, so every claim about the strip is a headless microsecond.
 */
namespace DestructionSession
{
	/**
	 * What the session is doing: laying pieces, or pulling them out.
	 *
	 * Build is enumerator zero because a default-constructed session must be the one that cannot
	 * destroy anything: opening in Build offers an unwanted ghost, opening in Destroy offers a
	 * click that removes a brick.
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
	 * A kind rather than an extent and a material on the state: the palette is data (an extent and
	 * a library row per kind, below), and carrying the numbers instead of the name would be a
	 * fourth place brick dimensions are written down, free to drift from the three that agree.
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
	 * Auto is enumerator zero and is not "no choice" — it hands every joint back to
	 * BuildMode::JointForContact, the same reason Mode defaults to Build: the default decides least.
	 * A choice rather than a profile pointer, for the reason EBuildPieceKind is a kind rather than an
	 * extent and a material: a raw pointer would be a library address serialised into a session.
	 *
	 * The five named ones are library rows, one each — JointOverrideFor below is the map. There is
	 * deliberately no perpend chip: the weak perpend is what the inference chooses for a vertical
	 * face, not a thing anybody asks for.
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
	 * Durable state rather than widget state: a controller spreading mode, piece and course across
	 * separate owners can have them disagree, where one struct makes every transition
	 * state-in/state-out and therefore a table row in a test.
	 */
	struct FSessionToolbarState
	{
		ESessionMode Mode = ESessionMode::Build;

		EBuildPieceKind Piece = EBuildPieceKind::Brick;

		EPlacementMode Placement = EPlacementMode::Snap;

		/**
		 * What fastens every joint the next placement forms.
		 *
		 * Survives a trip through Destroy mode: the Destroy strip draws none of the six chips, and
		 * ApplyToolbarButton's "fields a transition does not name survive it" rule is what brings the
		 * player back to the screws they chose rather than to Auto — invisible until the structure runs.
		 */
		EJointChoice Joint = EJointChoice::Auto;

		/**
		 * Which course the build plane is on. Never negative.
		 *
		 * Course 0 has the earth under it. A negative course is a build plane below the ground and
		 * an unreadable readout, so ApplyToolbarButton refuses to produce one and the three course
		 * functions below all treat one as course 0 — one clamp spelled once rather than four
		 * functions disagreeing about a state that should not exist.
		 */
		int32 Course = 0;

		/**
		 * Whether the next piece lies the OTHER way round — turned a quarter turn about Z.
		 *
		 * A bit beside the kind rather than three more kinds: a rotated brick is the same brick, and
		 * a BrickRotated / TimberPlateRotated / TimberLintelRotated triple would treble the one table
		 * this model keeps brick dimensions out of. It is also what makes the corner vocabulary
		 * reachable at all — the snap solver offers a quoin when two brick-sized boxes cross long
		 * axes, but until a player could turn a piece, CR-2a was code no click could reach.
		 *
		 * Upright by default, for the reason Mode defaults to Build (the default decides least), and
		 * it survives a trip through Destroy mode like the piece, placement and joint do: a player
		 * who turned a piece for the second leg of an L must find it still turned on return.
		 */
		bool bRotated = false;

		/**
		 * Whether there is a live structure for the commands to act on.
		 *
		 * The only precondition either command has: Clear or Run with nothing built are silent
		 * no-ops, indistinguishable from the game missing the click.
		 */
		bool bHasStructure = false;

		/**
		 * Whether the Destroy-mode load overlay is on — every live piece tinted by its worst
		 * joint's margin band.
		 *
		 * A setting rather than a command, so it latches: a way of looking at the structure rather
		 * than a thing that happens to it, so its chip stays lit until a second click turns it off.
		 *
		 * Off by default, for the reason Mode defaults to Build: the overlay costs a solve per
		 * toggle-on and per mutation, and a session that opened paying it would pay it on every
		 * scenario level that never asked. It survives a trip through Build mode — the Build strip
		 * does not draw the chip, and ApplyToolbarButton's "fields a transition does not name survive
		 * it" rule keeps the player's choice waiting for them.
		 */
		bool bLoadOverlay = false;
	};

	/**
	 * Every button the strip can carry.
	 *
	 * An id rather than a caption is what makes a click attributable: reporting a press by its text
	 * would route behaviour through wording, the one thing on a row allowed to be retuned.
	 */
	enum class EToolbarButtonId : uint8
	{
		ModeBuild,
		ModeDestroy,
		PieceBrick,
		PieceTimberPlate,
		PieceTimberLintel,

		/**
		 * Turn the next piece a quarter turn about Z — the tail of the palette, and a latch.
		 *
		 * Drawn with the pieces, not as one of them: it modifies whichever of the three is lit,
		 * so it carries no swatch and sits right after them, ahead of the placement pair — the order
		 * the player asks the questions in: which piece, lying which way, snapped or free, fastened how.
		 */
		RotatePiece,

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
	 * A model answer rather than a run of AddSlot calls: SESSION_UI_DESIGN §b draws the strip as
	 * mode tabs, then the mode's settings, then its one command, separated by 1 px rules "so that a
	 * destructive click is never adjacent to a setting click" — a decision a widget counting slots
	 * could not hold.
	 *
	 * The three are contiguous and in this order, which is what makes a rule drawable at all: the
	 * widget draws a hairline where neighbours' groups change, so a group appearing twice would
	 * break the three-region reading a player navigates by from peripheral vision.
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
	 * A kind rather than a colour, for the reason EJointMarginBand is a band rather than a green
	 * (SESSION_UI_DESIGN §a principle 6): the model decides a brick chip carries a brick, and which
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
		 * Data on the row, for the reason FPieceMenuRow::Label is: a widget spelling its own text
		 * holds the wording where no test can read it, and this strip's wording is load-bearing —
		 * Snap versus Free is the difference between a piece on the bond and one where the cursor was.
		 */
		FString Label;

		/**
		 * "This is what you have chosen" — not the same question as bEnabled.
		 *
		 * Exactly one button of each setting group carries it, and a command never does: a latched
		 * Clear button would read as a mode the player is stuck in.
		 */
		bool bActive = false;

		/**
		 * Whether the thing behind the button can actually happen.
		 *
		 * The model owns the greying because it owns the refusal — ApplyToolbarButton reads this
		 * same answer rather than deciding again; two derivations of "can this happen" is how a lit
		 * button that does nothing gets shipped. False by default, so a half-built row cannot claim
		 * a click will land.
		 */
		bool bEnabled = false;

		/**
		 * Which region of the strip the button is in.
		 *
		 * Command by default, the fail-closed end: a row nobody filled in draws past the last rule
		 * on its own rather than claiming to be one of the two mode tabs that may never move —
		 * visibly odd, rather than a strip whose first slots have silently shifted.
		 */
		EToolbarGroup Group = EToolbarGroup::Command;

		/** Which piece the chip lays, if it lays one. Nothing to draw, by default. */
		EToolbarSwatch Swatch = EToolbarSwatch::None;
	};

	/**
	 * How one chip is drawn — the fill, the edge, the caption and the chip's own geometry.
	 *
	 * Decided here and not in Slate, per SESSION_UI_DESIGN §e: bActive and bEnabled are different
	 * questions, and drawing them alike would make a lit button that does nothing indistinguishable
	 * from a greyed one that works. Spelled as ternaries inside the panel builder that decision is
	 * unreadable — a widget test can only say "the lit one looks different", which a decorative
	 * alternation satisfies forever.
	 *
	 * Colours, not brushes: nothing here knows what Slate is. The widget turns these into a
	 * rounded-box brush and its hover/press variants — drawing, not deciding.
	 */
	struct FChipLook
	{
		/** What the chip is filled with. The mode's accent when it is the one you have chosen. */
		FLinearColor Fill = FLinearColor::Transparent;

		/**
		 * The 2 px drop edge around it — §a principle 1's "press me" cue.
		 *
		 * Also the one channel that tells the "go" chip from a lit mode tab: `Run structure` is
		 * filled with the destroy accent by identity, and the lit `Destroy` tab is filled with the
		 * mode's — the same red two slots along. Both fills are spoken for, so the ring carries the
		 * difference: the go chip wears a bright rim, everything else wears the shadow.
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
		 * The chip says bActive twice, in fill and weight: the fill is what a player flying a camera
		 * catches from the corner of an eye, and the weight is what they read looking straight at it.
		 */
		bool bBoldCaption = false;
	};

	/**
	 * The strip for a session state: one fixed list per mode, in one fixed order.
	 *
	 * The mode pair is always first — the two buttons that switch modes may not move, or a strip
	 * whose first slots shifted would put a different button under a cursor that has not moved.
	 *
	 * The list's content depends on the mode alone: a piece selection or course number that added,
	 * removed or reordered a button would be a strip rearranging itself while a player uses it.
	 */
	TArray<FToolbarButton> SessionToolbarButtons(const FSessionToolbarState& State);

	/**
	 * One click, as a pure function: the state before and the button, in; the state after, out.
	 *
	 * Pure because that is the only way it is assertable — a controller mutating its own fields from
	 * a Slate callback puts the toolbar's behaviour behind a click only a human can perform.
	 *
	 * Refuses whatever the strip refuses, by asking SessionToolbarButtons rather than re-deciding:
	 * a button the state does not draw, or draws greyed, is a bitwise no-op — including the course
	 * floor, refused rather than clamped, which stops agreeing with a clamp the moment anything
	 * else on the state moves with a course change.
	 *
	 * The fields a transition does not name survive it: going to Destroy and back must return a
	 * player to the piece, placement and course they left with.
	 */
	FSessionToolbarState ApplyToolbarButton(const FSessionToolbarState& State, EToolbarButtonId Id);

	/**
	 * A mode's accent — build amber, destroy red.
	 *
	 * The two colours this UI already uses: amber is the Caution band's gold, red is the destructive
	 * row's. The mode's accent rather than the button's, so every lit chip on a Build strip is amber
	 * and every lit chip on a Destroy strip is red — the strip's colour reads the mode from
	 * peripheral vision while flying a camera. A mode this build has never heard of answers with the
	 * destroy accent, agreeing with SessionToolbarButtons, which draws the Destroy strip for anything
	 * that is not Build.
	 */
	FLinearColor ModeAccent(ESessionMode Mode);

	/**
	 * What a swatch kind is painted in — the colour of the thing the player will lay.
	 *
	 * The shed materials' own base colours, so the palette chip and the brick that lands are one
	 * decision rather than two people picking the same red.
	 *
	 * None is transparent, and so is a kind nobody declared: the swatch is drawn through one widget
	 * whatever kind it is, so "nothing to draw" has to be a colour, and a block of colour on a
	 * command chip would name a piece that chip does not lay.
	 */
	FLinearColor SwatchColour(EToolbarSwatch Swatch);

	/**
	 * How one chip of one mode's strip is drawn.
	 *
	 * Precedence is !bEnabled, then bActive, then the "go" chip, then idle: a greyed Run structure
	 * must read as greyed even though Run is filled without being lit. The "go" chip is why this
	 * takes a button rather than two flags — a command is never bActive, yet §e asks for Run
	 * structure "filled in the destroy accent", which `bActive ? Accent : Idle` cannot express.
	 *
	 * Clear build carries its danger in the caption, not the fill: a chip filled destroy-red on an
	 * amber strip would read as the mode you are in, so the warning is a warm caption instead.
	 */
	FChipLook ChipLookFor(const FToolbarButton& Button, ESessionMode Mode);

	/**
	 * What a joint choice overrides every formed joint's profile with — or nothing, for Auto.
	 *
	 * A pointer, and nullptr is a real answer rather than a failure: the override rides through
	 * PreviewBuildPiece / PlaceBuildPiece as an optional profile, so "let the inference decide" must
	 * be expressible in that type — a sentinel profile meaning "infer" would be a seventh library row
	 * every consumer has to special-case.
	 *
	 * The shipped row's own address, never a copy — the identity rule BuildPieceMaterial keeps for
	 * the palette, since Nail, Screw and Bolt are one shape at three scales and a copy would go on
	 * serving stale numbers after a re-anchor.
	 *
	 * A choice this build has never heard of overrides nothing, the fail-closed end: a plausible row
	 * would fasten a joint with a profile nobody picked, where nothing hands it back to the
	 * inference that decided every joint before the chip existed.
	 */
	const FConnectionStrength* JointOverrideFor(EJointChoice Joint);

	/**
	 * A piece kind's HALF extent, in centimetres.
	 *
	 * Transcribed from what the demo building already lays rather than newly authored: the brick is
	 * the standard 21.5 x 10.25 x 6.5 unit halved, the plate is Core/BuildMode/DemoBuilding.cpp's own
	 * (33.75, 5.125, 5.0), and the lintel is that plate's 90 cm sibling.
	 *
	 * Fails closed on a kind this build does not know with the zero extent: an extent that is not a
	 * number is a mass that is not a number two calls later, and a piece of no size is an obvious
	 * refusal rather than a plausible brick.
	 */
	FVector BuildPieceHalfExtentCm(EBuildPieceKind Kind);

	/**
	 * A piece kind's material, by reference to the shipped library row.
	 *
	 * Never a copy — identity is the point. Two profiles with equal fields differ in the one thing
	 * that matters: which row a future retune moves. A private copy of Timber would go on serving
	 * stale numbers after a re-anchor.
	 *
	 * Fails closed on a kind this build does not know, and Timber is the fail-closed answer rather
	 * than an arbitrary one: it is not compression-dominant, so BuildMode::JointForContact infers
	 * DryStone against it — compression and friction, no tension, the weakest joint the inference
	 * can hand out.
	 */
	const DestructionProfiles::FMaterialProfile& BuildPieceMaterial(EBuildPieceKind Kind);

	/**
	 * The Z a piece of the given half-height takes when it is laid on the given course, in cm.
	 *
	 * The rests-on-the-ground convention — owner-delegated ruling, 2026-09-15. Every harness before
	 * this model centred course 0 at Z = 0, putting the grounded course half below the ground plane;
	 * a player laying the first brick must see it sitting on the ground, so course 0 answers with
	 * the piece's own half-height. Nothing about the structure changes — the solver reads relative
	 * positions only — so this is the same building lifted by one brick half-height, on every course.
	 *
	 * The pitch is the brick's, whatever the piece is: BrickSizeCm.Z plus one bed joint, read from
	 * BuildMode::FSnapSettings rather than written down again. A course is a property of the wall
	 * rather than of the thing laid into it — a timber plate on course 2 bears on two brick courses
	 * and their joints, exactly where the demo building puts its own plate; a pitch derived from the
	 * piece would put the plate at its own doubled height with the bearing imaginary.
	 *
	 * A negative course is course 0, like every other course function here.
	 */
	double CoursePlaneZCm(int32 Course, double PieceHalfHeightCm);

	/**
	 * Whether the toolbar INTENDS the build plane to sit on the earth — true of course 0 and
	 * nothing else. This is the toolbar's intent for the plane, not the committed piece's flag.
	 *
	 * Grounded is the flag FStructure routes load to, so this is not cosmetic: a piece laid with it
	 * set absorbs whatever reaches it, and a whole building marked grounded cannot fall. The snap
	 * solver ranks candidates by raw distance, so a cursor on the course-0 plane beside a standing
	 * brick can snap UP onto its next-course bed, 7.5 cm above the earth, that this function would
	 * still call grounded — so the committed piece's flag must be derived from the snapped pose
	 * instead, never from the course this reports; this answer only says which readout the toolbar
	 * shows. A negative course answers as course 0, the same clamp every course function here uses.
	 */
	bool IsCourseGrounded(int32 Course);

	/**
	 * The course readout, naming its own course — and it counts from one where the index it is
	 * given counts from zero. CourseLabel(0) reads "Course 1".
	 *
	 * Owner-delegated ruling, 2026-09-15: the session had two surfaces naming a course that
	 * disagreed, this strip printing "Course 0" while the piece menu's rows count from one
	 * (Core/PieceMenu.cpp, "both numbers count from one"), so a player laying a brick on "Course 0"
	 * saw the details window call it "course 1 · #1". This strip moved to match, since it is the
	 * readout a player reads longest.
	 *
	 * FSessionToolbarState::Course is still the zero-based index; CoursePlaneZCm, IsCourseGrounded
	 * and the stepper's floor are unchanged arithmetic over it.
	 */
	FString CourseLabel(int32 Course);
}
