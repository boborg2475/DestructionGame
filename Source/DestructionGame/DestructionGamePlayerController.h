// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/PieceMenu.h"
#include "Core/PieceSelection.h"
#include "Core/SessionToolbar.h"
#include "GameFramework/PlayerController.h"
#include "Input/Reply.h"
#include "Layout/Margin.h"
/*
 * The one Slate style header this file pulls: SButton keeps a raw `const FButtonStyle*` and
 * never copies it, so the chips' styles need storage that outlives the widgets and does not
 * move — a member array of FButtonStyles, which a forward declaration can't express.
 */
#include "Styling/SlateTypes.h"
#include "World/ScenarioLabel.h"
#include "DestructionGamePlayerController.generated.h"

class SBox;
class SWidget;
class UBuildModeComponent;
class UGameViewportClient;
class UInputAction;
class UInputMappingContext;

/*
 * The drag handlers take both by const reference, so a declaration suffices — pulling Slate's
 * geometry/event headers in for two parameter types would put them in front of every test that
 * includes this controller.
 */
struct FGeometry;
struct FPointerEvent;

/* Declared with its underlying type in World/BrickActor.h, which only the .cpp needs. */
enum class EBrickHighlight : uint8;

/**
 *  Player Controller for the destruction sandbox.
 *  Applies the input mapping contexts and overrides the Player Camera Manager class.
 */
UCLASS(config="Game")
class DESTRUCTIONGAME_API ADestructionGamePlayerController : public APlayerController
{
	GENERATED_BODY()

public:

	/** Constructor */
	ADestructionGamePlayerController();

	/**
	 * The build loop this session lays pieces with.
	 *
	 * A default subobject, not something somebody remembers to add — a session with no build
	 * component would put the Build tab in a mode whose every click fails closed silently.
	 */
	UBuildModeComponent* GetBuildComponent() const;

	/**
	 * What the session is doing, and the only thing the strip draws.
	 *
	 * Read-only: OnToolbarButton is the single mutator, so every change goes through
	 * DestructionSession::ApplyToolbarButton's refusals and keeps the build component in step.
	 */
	const DestructionSession::FSessionToolbarState& GetSessionToolbarState() const;

	/**
	 * One toolbar click: move the session, and move the world with it.
	 *
	 * The one door: every input, strip buttons and keyboard shortcuts alike, comes through it, so
	 * a controller setting fields directly would be a second opinion about what is greyed
	 * (Core/SessionToolbar.h). A refused click changes nothing, state and side effects alike.
	 *
	 * @return whether the click landed; false is a refusal, not a failure.
	 */
	bool OnToolbarButton(DestructionSession::EToolbarButtonId Id);

	/**
	 * The two keys that stand for a pair of chips: read where the session is, dispatch the other.
	 *
	 * Goes through OnToolbarButton rather than setting the field directly, so the model's
	 * refusals apply to a key exactly as to a chip. `Tab`/`G` each carry a read of the current
	 * state and a choice between two ids — the only decision in the session's keyboard; the other
	 * six shortcuts dispatch one constant id straight at the door.
	 *
	 * @return whether the toggle landed; false is a refusal, not a failure.
	 */
	bool ToggleSessionMode();
	bool ToggleSessionPlacement();

	/**
	 * The structure this session's commands act on, or INDEX_NONE.
	 *
	 * The player's own build first, the level's wall behind it. The build plot lays nothing, so
	 * until the first brick lands there is genuinely nothing to run and the button is greyed.
	 */
	int32 GetSessionStructureId() const;

	/**
	 * Point along this ray: in Build the ghost follows it, in Destroy the brick under it is called
	 * out.
	 *
	 * A ray in rather than a cursor — the deprojection needs a viewport, this half needs only a
	 * world. The session's mode decides which seam the cursor is driving.
	 */
	void PointerAlongRay(const FVector& StartCm, const FVector& EndCm);

	/**
	 * The primary click along this ray: in Build it lays a piece, in Destroy it inspects one.
	 *
	 * No piece menu in Build mode — a Delete menu over the brick just laid, with the cursor already
	 * on it, would be today's click leaking through, so the two are dispatched rather than layered.
	 *
	 * @return whether the click did its mode's job: a piece landed, or a menu came up.
	 */
	bool PrimaryAlongRay(const FVector& StartCm, const FVector& EndCm);

	/**
	 * Put the ghost where this ray points, without the player having clicked anything.
	 *
	 * The testable half of the per-tick cursor refresh — PlayerTick's own deprojection is
	 * untestable by construction, so everything that can go wrong lives behind this call instead.
	 *
	 * Does nothing in Destroy mode; run every frame, a leak would hang a gold ghost over the wall
	 * the player is demolishing and re-arm a preview a stray confirm could commit.
	 *
	 * @return whether a valid preview is up: false in Destroy mode, or on a ray missing the plane.
	 */
	bool RefreshBuildPreviewFromRay(const FVector& OriginCm, const FVector& Direction);

	/**
	 * Build the session's toolbar strip, and hand it back instead of drawing it.
	 *
	 * Public, for the reason BuildPieceMenuPanel is: which buttons draw, greyed or live, focusable
	 * or not, and what each press calls are all decided here — kept private, none of it would be
	 * reachable, and a focusable SButton stealing focus on click is a defect only a headless
	 * arrange of this tree can see.
	 */
	TSharedRef<SWidget> BuildSessionToolbarPanel();

	/**
	 * The style one chip is wearing: a rounded box filled the way the model says, with its hover
	 * and press variants.
	 *
	 * A reference into storage that outlives the widget — SButton keeps the raw pointer it is
	 * given and never copies it, so a per-call temporary or a TMap value (which rehashes) would
	 * dangle. The styles live in a fixed array whose slots never relocate; rebuilding the strip
	 * overwrites them in place.
	 *
	 * Public because SButton exposes no style getter (World.Session.ToolbarChipsAreRoundedAndGrouped
	 * checks the border brush by address). An undeclared button id gets the greyed, fail-closed style.
	 */
	const FButtonStyle& SessionChipStyleFor(const DestructionSession::FToolbarButton& Button) const;

	/**
	 * Put the strip on screen, and redraw it when the session moves.
	 *
	 * Rebuilt rather than re-bound, so a state change is never drawn by stale widgets. A world
	 * with no viewport — every headless test — draws nothing and carries on, the ordinary case.
	 */
	void ShowSessionToolbar();
	void RefreshSessionToolbar();

	/**
	 * Inspect whatever this ray hits: open the piece menu for it, or dismiss the menu.
	 *
	 * A ray in rather than a click, deliberately: turning the cursor into a world ray needs a
	 * viewport a headless test does not have, while everything after — trace, resolve, build the
	 * menu, present or dismiss it — can be wrong in a way a player would notice and needs only a
	 * world. The input handler deprojects and calls this, which is what a test drives.
	 *
	 * @return the rows now presented. Empty means the menu was dismissed — the answer for a ray
	 *         that hit the floor, hit nothing, or hit a brick for a piece that has gone — and it
	 *         must dismiss rather than leave the previous piece's menu naming a brick the player
	 *         is no longer pointing at.
	 */
	TArray<FPieceMenuRow> InspectAlongRay(const FVector& StartCm, const FVector& EndCm);

	/**
	 * Call out whatever this ray hits, so the player can see what they would hit.
	 *
	 * Separate from InspectAlongRay because hovering is not clicking: this changes nothing but
	 * which brick is called out, no selection touched, no menu opened or closed. The same seam is
	 * drawn at the same place for the same reason — the cursor-to-ray half needs a viewport and
	 * this half needs only a world.
	 *
	 * A selected brick stays selected when hovered — the stronger state wins, so a brick the menu
	 * is about must not start reading as merely-pointed-at because the cursor drifted over it.
	 *
	 * @return the piece now hovered, or a default ref when the ray hit nothing.
	 */
	FPieceRef HoverAlongRay(const FVector& StartCm, const FVector& EndCm);

	/**
	 * The bricks the player has picked, which is what the menu is about.
	 *
	 * The count a menu reports is Num() on this, and the rows carry the same refs: the menu is a
	 * projection of this set, rebuilt whenever it changes rather than a second record of it. A
	 * menu is up exactly when this is non-empty.
	 */
	const FPieceSelection& GetPieceSelection() const;

	/**
	 * Single out one of the selected bricks as the one the joint readout describes.
	 *
	 * A third thing from Hovered and Selected: hovering an entry in the menu singles that brick
	 * out without disturbing the rest of the selection, so this touches neither the selection
	 * nor the presented rows — only which brick is called out how.
	 *
	 * A default ref singles out nothing, which is how the cursor leaving the list is said.
	 */
	void SetInspectedPiece(const FPieceRef& Ref);

	/**
	 * Put these rows on screen as the piece menu, replacing whatever was up.
	 *
	 * Rows only, no separate ref: FPieceMenuRow already carries the ref it commits against, and
	 * Core/PieceMenu.h says why that is the whole reason the row is a struct — a presenter that
	 * remembered the ref beside the rows is one stale field away from committing the right action
	 * against the wrong brick. A second ref parameter here would re-open exactly that, one layer up.
	 *
	 * An empty list dismisses rather than showing an empty box. Every miss route out of
	 * InspectAlongRay produces an empty list, so "the ray hit nothing" and "take the menu down"
	 * have to be the same call or the previous brick's menu stays on screen.
	 *
	 * @return whether a menu is now presented — false for an empty list.
	 */
	bool ShowPieceMenu(TArrayView<const FPieceMenuRow> Rows);

	/** Take the piece menu down. @return whether there was one to take down. */
	bool DismissPieceMenu();

	/** Whether a piece menu is currently presented. */
	bool IsPieceMenuShown() const;

	/** The rows currently presented, empty when no menu is up. */
	TArrayView<const FPieceMenuRow> GetShownPieceMenuRows() const;

	/**
	 * Choose a presented row: commit it and take the menu down.
	 *
	 * An index into the presented rows is the whole input, which keeps the widget half to a
	 * single call — a button knows which entry it is and nothing else.
	 *
	 * @return whether the chosen row's action actually committed.
	 */
	bool ChoosePieceMenuRow(int32 RowIndex);

	/**
	 * Build the whole panel the viewport is handed, and hand it back instead of drawing it.
	 *
	 * Public, and that is the point rather than an accident: everything the panel's layout can
	 * get wrong — a row that moves out from under a stationary cursor when the readout beside it
	 * changes size — is decided here and needs no viewport, renderer or RHI to measure, since a
	 * widget tree can be prepassed and arranged in a headless test. Keeping the build private and
	 * reachable only through AddViewportWidgetContent made that whole surface unreachable.
	 *
	 * It assigns PieceMenuInspectorBox, so the readout can be swapped afterwards by
	 * SetInspectedPiece without rebuilding the panel — the behaviour under test.
	 */
	TSharedRef<SWidget> BuildPieceMenuPanel();

	/**
	 * The readout model for the current selection and inspected brick, or an empty one.
	 *
	 * The guards here are the same plumbing InspectAlongRay has, and for the same reason: a
	 * binding has to be found before the model can be asked, and no world, no subsystem and no
	 * such structure all mean the same thing — nothing to read out. It lives on the controller
	 * rather than the widget so the widget itself resolves nothing.
	 *
	 * Public because it is a wire between two correct halves and nothing else reaches it. Every
	 * headless world is built in code and has no UGameViewportClient, so the only two callers —
	 * BuildPieceMenuPanel and RefreshPieceMenuInspectorWidget — used to return early before ever
	 * getting here; CURRENT_STATE.md's integration entry rule names exactly this shape: a call
	 * nobody makes cannot be reached by testing the halves.
	 *
	 * Detail is asked for by the caller and defaults to Full. This accessor is not only the
	 * panel's: NeighbourHighlightForPiece and NeighbourPieces also walk Inspector.Joints, so the
	 * joint table is an index the brick highlights are keyed on as well as a readout. A compact
	 * answer handed to those would black out every neighbour colour at once with nothing on
	 * screen to say why — so the two panel builders pass PieceMenuPanelDetail explicitly and
	 * everything else takes the default, the mode that withholds nothing.
	 */
	FPieceMenuInspector PieceMenuInspectorForSelection(
		EPieceMenuDetail Detail = EPieceMenuDetail::Full) const;

	/**
	 * Which detail the next BuildPieceMenuPanel draws at. A field's value, and nothing else.
	 *
	 * Public for the same reason BuildPieceMenuPanel is. The mode is toggled today by a double
	 * click on the title strip, and OnPieceMenuPanelDetailToggled tears the whole widget down
	 * through the viewport, so the only route into compact runs through the one layer a headless
	 * run has none of — the panel a compact mode produces is exactly what
	 * World.Menu.TheReadoutFitsInsideThePanel has to measure, so the state needs a seam a test can
	 * set. It decides nothing: no rebuild, no clamp, no redraw.
	 */
	void SetPieceMenuDetail(EPieceMenuDetail Detail);

protected:

	/** Input Mapping Contexts applied on possession */
	UPROPERTY(EditAnywhere, Category="Input|Input Mappings")
	TArray<TObjectPtr<UInputMappingContext>> DefaultMappingContexts;

	/**
	 * The input that opens the piece context menu.
	 *
	 * On the controller rather than the pawn: it already owns the mapping contexts, outlives
	 * any pawn, and carries the cursor this action needs. Resolved by the path RequiredContent.h
	 * names, so this and the required-content table cannot become two lists that disagree.
	 */
	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> InspectPieceAction;

	/**
	 * The input that keeps the highlight following the cursor.
	 *
	 * A second action rather than IA_MouseLook, though both read Mouse2D. Free-look is chorded to
	 * a held right mouse button now, and hovering has to work while no button is held at all — a
	 * brick is called out the moment the player points at it rather than spins the camera. It
	 * shares the axis with free-look, and RequiredContent.h records why the asset must not
	 * consume it.
	 */
	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> HoverPieceAction;

	/**
	 * The always-on free-look context, named separately so the constructor reads plainly.
	 *
	 * Also in DefaultMappingContexts, and the same pointer — a name for one of the applied
	 * contexts, not a second one. It used to matter because the piece menu removed it: IMC_MouseLook
	 * binds the raw Mouse2D axis, so with no held button the camera followed the mouse and a cursor
	 * drawn over it was unusable. That removal is gone (SESSION_UI_DESIGN §d, S6) — the mapping now
	 * carries a Chorded Action trigger on IA_LookModifier, so look happens only while the right
	 * mouse button is down and there is nothing for a panel to take away.
	 */
	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputMappingContext> MouseLookMappingContext;

	/**
	 * The session's own keyboard, named for the same reason and used for one more.
	 *
	 * Also in DefaultMappingContexts, and the same pointer — a name for one of the applied
	 * contexts, not a second one. The name is for the priority: the apply loop gives this context
	 * one step above the others, because IMC_MouseLook's Mouse2D mapping is chorded on the
	 * IA_LookModifier this context maps, and a chord only sees its modifier if the modifier's
	 * mapping was evaluated earlier in the same frame. See SessionMappingContextPriority in the .cpp.
	 */
	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputMappingContext> SessionMappingContext;

	/**
	 * The eight session shortcuts, one property per action.
	 *
	 * Eight rather than a list, because each is bound to a different dispatch and the binding is
	 * where a list would have to be turned back into names anyway. IA_LookModifier is deliberately
	 * not among them: it feeds IMC_MouseLook's chord and has no handler to reach, so a C++ binding
	 * on it would be dead code.
	 *
	 * Resolved by the paths RequiredContent.h names, so these and the required-content table
	 * cannot become two lists that disagree.
	 */
	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> SessionToggleModeAction;

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> SessionPieceBrickAction;

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> SessionPiecePlateAction;

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> SessionPieceLintelAction;

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> SessionSnapToggleAction;

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> SessionCourseUpAction;

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> SessionCourseDownAction;

	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> SessionRunAction;

	/**
	 * The session's per-frame cursor refresh, and nothing else this controller does per frame.
	 *
	 * Here rather than on IA_HoverPiece because an axis action only fires when it moves — the
	 * owner's report (2026-09-16): with the ghost driven only by Mouse2D, a session that begins
	 * with a still mouse shows nothing until something actuates the axis, and a settings change
	 * waits for the same event. The setters carry the settings half; this carries "the player is
	 * pointing somewhere and has not clicked".
	 */
	virtual void PlayerTick(float DeltaTime) override;

	/** Input mapping context setup */
	virtual void SetupInputComponent() override;

	/** Puts the scenario banner on screen, and takes it off again with the world. */
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:

	/**
	 * IA_InspectPiece's handler: turn the cursor into a world ray and inspect along it.
	 *
	 * A deprojection plus one call, and nothing else, by design: DeprojectMousePositionToWorld
	 * needs a viewport a headless run has none of, so this is the one link in the chain no test
	 * can reach — exactly why everything that can be wrong in a way a player would notice lives
	 * behind InspectAlongRay instead. Anything added here grows the untested surface.
	 */
	void OnInspectPiece();

	/**
	 * IA_HoverPiece's handler: turn the cursor into a world ray and call out what it points at.
	 *
	 * The same deprojection-plus-one-call shape as OnInspectPiece and for the same reason —
	 * untestable by construction, so everything that can be wrong in a way a player would notice
	 * lives behind HoverAlongRay.
	 *
	 * Runs on every frame the mouse moves: one line trace per moved frame against whatever wall
	 * is up. That is the price of a highlight that is never stale, and why the binding is on
	 * Triggered rather than Started; see SetupInputComponent.
	 */
	void OnHoverPiece();

	/**
	 * PlayerTick's handler: turn the cursor into a world ray and re-drive the build ghost along it.
	 *
	 * The same deprojection-plus-one-call shape as OnHoverPiece, untestable for the same reason —
	 * everything that can be wrong in a way a player would notice lives behind
	 * RefreshBuildPreviewFromRay. Anything added here grows the untested surface.
	 *
	 * Skipped while the look chord is held: the right button turns the camera, and re-previewing
	 * every frame of a drag would drag the ghost across the plot behind the player's back, with the
	 * cursor hidden so there is nothing on screen for the ghost to follow.
	 *
	 * Also skipped when the mouse has not moved, which keeps a per-frame handler cheap — a still
	 * cursor names the same point on the same plane and re-solves the same snap. The settings half
	 * is already covered by the component's own setters, so nothing needs this to run on a frame the
	 * pointer did not move.
	 */
	void RefreshBuildPreviewFromCursor();

	/**
	 * Where the cursor was on the last frame this refreshed, and whether it has ever been read.
	 *
	 * The flag makes a first frame with the pointer at exactly (0, 0) a refresh rather than a skip
	 * — the ordinary "no reading yet" case rather than a coordinate worth special-casing.
	 */
	FVector2D LastBuildCursorPx = FVector2D::ZeroVector;

	bool bHasBuildCursorPx = false;

	/**
	 * Put the session's controls up, once, and never take them down again.
	 *
	 * Replaces an apply/restore pair, and the absence of the restore is the point. There was a
	 * SetPieceMenuControls(bool) here that raised the cursor and removed the free-look context
	 * while a menu was up, putting both back on the way out — a correct answer to a real hazard
	 * (IMC_MouseLook binds the raw Mouse2D axis, so a pointer over a camera that follows every
	 * mouse movement is unusable), but incompatible with a toolbar on screen for the whole session:
	 * a strip only clickable by first opening a piece menu is not a toolbar. The hazard is closed
	 * at the asset now, by IMC_MouseLook's Chorded Action trigger on IA_LookModifier, so there is
	 * nothing for a panel to take away and no restore any route can forget (SESSION_UI_DESIGN §d, S6).
	 *
	 * The cursor hides while the button is held — SetHideCursorDuringCapture(true): the right-drag
	 * that turns the camera takes capture, the pointer vanishes for the drag and returns on release.
	 * The mouse is not locked to the viewport, since a session is played in a window as often as not.
	 *
	 * A controller with no ULocalPlayer has no viewport to set an input mode against, so the cursor
	 * flag, which needs nothing, is set first.
	 */
	void SetSessionControls();

	/**
	 * The session shortcuts' handlers: one call each, and nothing else belongs in them.
	 *
	 * Six of the eight share one function with the id as a bound payload, since they differ by
	 * nothing but that id — six one-line functions would be six places for a copy-paste to put the
	 * wrong constant. The two toggles cannot: each reads the session before it chooses, which is
	 * why ToggleSessionMode/ToggleSessionPlacement are public seams a test can drive, and these are
	 * the void wrappers Enhanced Input's delegate signature needs.
	 */
	void OnSessionShortcut(DestructionSession::EToolbarButtonId Id);
	void OnSessionToggleMode();
	void OnSessionTogglePlacement();

	/**
	 * Put one button per presented row on screen, and take them off again.
	 *
	 * The one inch of the loop no test can reach, deliberately kept to exactly that: a code-built
	 * test world has no UGameViewportClient at all, so AddViewportWidgetContent has nothing to add
	 * to and no headless assertion can see a button, click or leak. Everything a menu could get
	 * wrong — which rows, which ref, which action, the dismissal, the controls restore, an index
	 * naming no row — is already decided by ChoosePieceMenuRow and the presenter, both covered. So
	 * there is no logic here to be wrong: a button per row, its label off the row, and a click that
	 * calls ChoosePieceMenuRow with its own index and nothing else.
	 *
	 * It draws the readout too, and every string in it is already decided. HeaderText, CountText,
	 * each entry's Label, InspectedLabel, SupportText and JointsText, the headroom caption and its
	 * decade ticks, and the hint standing in for the breakout when no brick is singled out — all
	 * come off FPieceMenuInspector, worded there (Core/PieceMenu.h) precisely so nothing here
	 * chooses a unit, a precision, a singular or a plural. Even the emptiness is the model's:
	 * InspectedLabel and InspectedHintText are empty in exactly the states the other is not, so
	 * drawing both unconditionally draws whichever exists. A branch appearing here is the drift to
	 * watch for; a missing string belongs in the model.
	 *
	 * The build sits beside the Append in ShowPieceMenu and the removal beside the Reset in
	 * DismissPieceMenu, inheriting the dismiss-then-build discipline that keeps a second add with
	 * no matching remove on the same code path as the asserted one.
	 */
	void BuildPieceMenuWidget();
	void RemovePieceMenuWidget();

	/**
	 * Redraw the joint readout for whichever brick is now singled out.
	 *
	 * Only the readout — a correctness decision rather than an economy. The entry buttons are
	 * what the cursor is sitting on when this runs (hovering one is what changed the inspected
	 * brick), so rebuilding the whole panel would destroy the very button being hovered, and
	 * Slate would fire OnHovered on its replacement next frame, and again after. Swapping the
	 * contents of a box that holds nothing interactive leaves the buttons alone.
	 */
	void RefreshPieceMenuInspectorWidget();

	/** A button's click: choose the row it stands for. Nothing else belongs here. */
	FReply OnPieceMenuRowClicked(int32 RowIndex);

	/** A chip's click: put its id through the one door. Nothing else belongs here either. */
	FReply OnSessionToolbarButtonClicked(DestructionSession::EToolbarButtonId Id);

	/** Take the strip off screen. Paired with ShowSessionToolbar's add, and run on EndPlay. */
	void RemoveSessionToolbarWidget();

	/**
	 * Write every chip's style into the fixed array, from the look the model decided for this state.
	 *
	 * Run before the chips are made, on every rebuild, since the look follows the state — which
	 * chip is filled with the mode's accent changes with the very click that rebuilds the strip. In
	 * place rather than fresh storage, since the slots may not move while any chip points at one.
	 */
	void RebuildSessionChipStyles(const TArray<DestructionSession::FToolbarButton>& Buttons);

	/**
	 * Ask the world whether there is anything for the commands to act on, and record the answer.
	 *
	 * Derived, never set: `bHasStructure` is the only session-state field that is a fact about the
	 * world rather than a player choice, so setting it on placement would mean remembering to unset
	 * it on every route a piece can leave by — delete, cascade, Clear. Asked afresh, it cannot go
	 * stale.
	 *
	 * Live pieces, because a structure of tombstones is nothing to run: RemovePiece tombstones
	 * rather than compacting, so a plot whose every brick has been deleted would still answer a
	 * piece count and leave Clear and Run lit over an empty plot.
	 */
	void RefreshSessionHasStructure();

	/**
	 * Re-paint the session's structure by load, or take the paint off again.
	 *
	 * While it is on: solve the structure non-destructively — `FStructureBinding::SolveLoads` is
	 * documented as leaving every connection exactly as intact as it found it, since an overlay is
	 * a way of looking at a wall and a look that broke joints would be the most expensive instrument
	 * ever shipped — then give every live piece the band of its worst joint.
	 *
	 * An input to HighlightForPiece, not a SetHighlighted of its own: a refresh that painted bricks
	 * directly would fight the cursor, with the next hover clearing the overlay off one brick and
	 * the next refresh painting over the selection. The overlay is the weakest state in the
	 * precedence, and only one function can honour a precedence.
	 *
	 * When it is off, the load input is cleared, so every piece falls back through the same
	 * precedence — to Hovered or Selected if it is one of those, else None.
	 *
	 * Called whenever the answer could have moved: the toggle itself, a Run, a committed Delete, a
	 * Build-mode placement. An overlay computed once is a photograph of a structure the player has
	 * since changed, and a green brick over a hole is exactly the lie this is for.
	 *
	 * A solve per mutation and never per frame — cost is why the overlay is a setting rather than
	 * always on.
	 */
	void RefreshLoadOverlay();

	/**
	 * Put the scenario banner on screen, and take it off again.
	 *
	 * The second inch of the game no test can reach, kept to exactly that. Every word and number
	 * in it is already decided by DestructionScenarios::BuildScenarioLabel, which
	 * World.Scenarios.Label sweeps 162 ways, and by the game mode's GetScenarioLabel, which
	 * World.Scenario.GameModeLabelsTheScenarioItBuilt ticks against a real world. What is left here
	 * is three text blocks reading three strings — a branch appearing in this pair is the drift to
	 * watch for, and a missing string belongs on FScenarioLabel.
	 *
	 * A code-built test world has no UGameViewportClient at all, so this draws nothing headless,
	 * the ordinary case rather than an error, exactly as for the piece menu.
	 */
	void BuildScenarioLabelWidget();
	void RemoveScenarioLabelWidget();

	/**
	 * What the banner reads, right now, asked again on every paint.
	 *
	 * Bound as attributes rather than baked in at build time, since half the label is a clock: the
	 * countdown to an armed cut changes every frame, and a banner built once would show the whole
	 * delay for as long as the level ran. The game mode is the only thing that knows which row it
	 * recorded and how much delay is left, and a level with no game mode of this class reads as an
	 * empty line rather than a crash.
	 */
	DestructionScenarios::FScenarioLabel ScenarioLabelNow() const;

	FText ScenarioLabelTitleText() const;
	FText ScenarioLabelExpectationText() const;
	FText ScenarioLabelCutText() const;

	/**
	 * The panel's title strip, dragged: press, move, release.
	 *
	 * Every decision these three could make is ClampPanelOffset's, which is why they are this
	 * short. Where a drag is allowed to leave the panel is the half that can strand it — an offset
	 * that puts the title strip off the top of the screen leaves the player nothing to grab and no
	 * way back short of restarting — and it is arithmetic on six doubles, so it lives in Core where
	 * Presenter.PanelOffsetClamp can hold it. What is left here is a cursor position and a
	 * subtraction.
	 *
	 * The cursor is taken through the strip's own geometry rather than in screen pixels. A
	 * viewport widget is laid out under Slate's DPI scaler, so a screen-space delta moves the
	 * panel by the DPI scale rather than by the distance the cursor travelled, and the offset the
	 * canvas is given is in the scaled space, not the screen's. AbsoluteToLocal on both ends puts
	 * the delta in the space the offset is stated in, whatever the scale, and the translation
	 * cancels in the subtraction.
	 *
	 * The press takes mouse capture, so a drag survives the cursor outrunning a 640 px strip —
	 * without it Slate stops sending moves the instant the pointer leaves the widget, and a quick
	 * drag drops the panel wherever the cursor crossed the edge.
	 */
	FReply OnPieceMenuPanelGrabbed(const FGeometry& Geometry, const FPointerEvent& Event);
	FReply OnPieceMenuPanelDragged(const FGeometry& Geometry, const FPointerEvent& Event);
	FReply OnPieceMenuPanelReleased(const FGeometry& Geometry, const FPointerEvent& Event);

	/**
	 * The same strip, double-clicked: swap the panel between its two detail modes.
	 *
	 * A gesture rather than a captioned button, for the no-logic rule this widget was landed
	 * under: a button needs a caption, a caption is a word, and choosing a word is a decision that
	 * belongs on FPieceMenuInspector where a test can read it — no such field exists today, and
	 * inventing one here would put the decision in the one place nothing can reach. Double-clicking
	 * a title bar to roll a panel up is also the gesture every desktop already uses for this.
	 * CURRENT_STATE.md carries the follow-up: a captioned toggle needs a model field and a red test.
	 */
	FReply OnPieceMenuPanelDetailToggled(const FGeometry& Geometry, const FPointerEvent& Event);

	/**
	 * Where the panel's top-left corner is, as the constraint canvas wants it.
	 *
	 * An attribute rather than a value baked into the slot, so a drag moves the panel without
	 * rebuilding it — the same reason the readout is a box whose content is swapped rather than
	 * torn down and put back. It marshals PieceMenuPanelOffsetPx into an FMargin and does nothing
	 * else; the size is the child's own, which is what AutoSize on the slot is for.
	 */
	FMargin PieceMenuPanelSlotOffset() const;

	/**
	 * The viewport, in the units the offset above is stated in.
	 *
	 * Read off the panel's own root rather than UGameViewportClient::GetViewportSize — the
	 * difference is the DPI scale. GetViewportSize answers in screen pixels; the constraint canvas
	 * lays out in Slate's scaled space, and a clamp fed one and applied to the other lets the panel
	 * off the edge of the screen by exactly the scale factor, the one outcome ClampPanelOffset
	 * exists to prevent. The root canvas is the viewport, so its own local size is the answer in
	 * its own units.
	 *
	 * Zero when there is no panel, which clamps every offset to the origin — the fail-closed
	 * corner rather than a special case, unreachable from the drag handlers since the strip being
	 * dragged lives inside the widget being asked about.
	 */
	FVector2D PieceMenuViewportSizePx() const;

	/**
	 * The same screen, asked of the viewport client because there is no panel to ask yet.
	 *
	 * A second route to one answer, split by when rather than what. The function above reads the
	 * panel's own laid-out root, exact and needing no conversion, but zero for a widget built this
	 * frame, so the home cannot come from it. This one asks the client, whose answer is in screen
	 * pixels and so is out by exactly the DPI scale the canvas lays out under, and divides it back.
	 * Both are the viewport in the units PieceMenuPanelOffsetPx is stated in.
	 *
	 * Zero when the scale is not a positive number, which puts the home at the origin — the corner
	 * on screen at every viewport size and scale, and the one both PieceMenuHomeOffset and
	 * ClampPanelOffset already fail closed to.
	 */
	FVector2D PieceMenuViewportSizeAtOpenPx(const UGameViewportClient& Viewport) const;

	/**
	 * An entry row's hover: single that brick out, and let it go again.
	 *
	 * The cursor on an entry is the whole input. A player reading a joint breakout needs to say
	 * which brick it is about without disturbing what they have picked, and moving the mouse down
	 * the list is the cheapest way to say it. Both are one call each.
	 */
	void OnPieceMenuEntryHovered(FPieceRef Ref);
	void OnPieceMenuEntryUnhovered();

	/**
	 * What state this brick should now be called out in, and putting it there.
	 *
	 * One function decides, and every route asks it, which makes "the stronger state wins" true
	 * everywhere rather than at call sites somebody remembered: a selected brick under the cursor
	 * reads Selected, so it cannot flicker back to merely-pointed-at the moment the player looks
	 * at it. Both halves are read back off the selection and the hovered ref rather than remembered
	 * per brick, so there is no third record to drift.
	 *
	 * A ref naming no brick refreshes nothing, which makes pieces a commit has just destroyed
	 * harmless to pass in.
	 */
	EBrickHighlight HighlightForPiece(const FPieceRef& Ref) const;
	void RefreshPieceHighlight(const FPieceRef& Ref);

	/**
	 * Which of the readout's colour slots this brick is the far end of, as the state that wears it.
	 *
	 * The panel's own answer, read back rather than worked out again. FInspectorJointRow::ColourSlot
	 * has already decided that joint row i takes slot i, and a second derivation here — walking the
	 * graph's connections, or numbering neighbours some other way — is the drift this project keeps
	 * paying for: two answers to "which brick is the blue one", drawn two inches apart.
	 *
	 * None when it is not one, which lets HighlightForPiece fall through to Hovered — a brick with
	 * no joint row, a row past the end of the palette (ColourSlot is INDEX_NONE by the model's own
	 * rule), and every state where nothing is singled out.
	 */
	EBrickHighlight NeighbourHighlightForPiece(const FPieceRef& Ref) const;

	/**
	 * Which band this brick is in under the load overlay, as the state that wears it.
	 *
	 * The overlay's answer, read back rather than worked out again — the same discipline
	 * NeighbourHighlightForPiece keeps. RefreshLoadOverlay computed a state per piece from one
	 * solve; re-deriving it here would mean a solve per brick per refresh, two answers to "what
	 * band is this" drawn a frame apart.
	 *
	 * None when there is no answer, which lets HighlightForPiece end on it: the overlay off, a ref
	 * naming another structure, a piece added since the last refresh, or a hole.
	 */
	EBrickHighlight LoadHighlightForPiece(const FPieceRef& Ref) const;

	/**
	 * Every brick the readout is currently pointing at, in its rows' order.
	 *
	 * What the set was and what it becomes are both needed, which is why this is a list rather
	 * than a test. The neighbours change wholesale when the readout moves to a different brick, so
	 * telling only the new ones leaves the old ones lit — running the cursor down a list of six
	 * entries would end with the whole wall coloured. Asked before the change and again after, with
	 * both answers refreshed.
	 *
	 * Includes bricks that are gone: a joint that went with a removed piece is still one of the
	 * inspected brick's rows and still takes a slot, so the ref is handed to RefreshPieceHighlight,
	 * which already answers "this ref names no actor".
	 */
	TArray<FPieceRef> NeighbourPieces() const;

	/**
	 * Put every brick the readout points at into the colour it should now be in — including the
	 * ones it has stopped pointing at, which is the half that has to be passed in.
	 *
	 * The set changes wholesale and its old members are the ones nothing else will tell. Every
	 * other refresh in this class is keyed on a ref something already has in hand — the brick
	 * clicked, the brick left, the bricks picked — but a brick stops being a neighbour without
	 * being touched at all, so the caller has to have asked NeighbourPieces() before anything
	 * changed. A refresh that told only the new members would leave the old ones lit.
	 *
	 * The readout moves for two reasons and both call this: which brick is singled out, and the
	 * selection changing underneath it — a brick that leaves the selection stops being singled
	 * out and takes every neighbour colour with it.
	 */
	void RefreshNeighbourHighlights(TArrayView<const FPieceRef> WereNeighbours);

	/** Point at this piece, or at nothing, letting whatever was pointed at before go. */
	void SetHoveredPiece(const FPieceRef& Ref);

	/**
	 * Pick nothing, and stop calling out everything that was picked.
	 *
	 * The refs are copied out first, since Clear empties the very array they live in — the same
	 * trap ChoosePieceMenuRow copies its row for. A cleared set that left bricks lit would tell the
	 * player they still have a selection.
	 */
	void ClearPieceSelection();

	/**
	 * The build loop, owned by the controller that drives it.
	 *
	 * A default subobject, so every controller in every level has one and nothing has to remember
	 * to attach it. It holds the player's live build, its ghost and the piece settings the toolbar
	 * pushes onto it; the session state above is the presenter's record of the same three choices,
	 * kept in step by OnToolbarButton alone.
	 */
	UPROPERTY()
	TObjectPtr<UBuildModeComponent> BuildComponent;

	/**
	 * What the session is doing. One struct, changed only by OnToolbarButton.
	 *
	 * Opens in Destroy, not the model's own default — the constructor says why: a
	 * default-constructed session must be the one that cannot destroy anything, but a controller is
	 * a different question. Twenty-eight of the twenty-nine levels lay a wall for the player to
	 * pull apart, and opening those in Build would put a ghost over it and swallow the first click.
	 */
	DestructionSession::FSessionToolbarState SessionToolbarState;

	/**
	 * The strip that state is drawn as, valid for as long as the session is on screen.
	 *
	 * A third viewport widget with its own add and remove, held so the remove has something to
	 * hand back — its own rather than the banner's, since the banner is built once and this is
	 * torn down and rebuilt on every click that changes what the strip says.
	 */
	TSharedPtr<SWidget> SessionToolbarWidget;

	/**
	 * How many chip styles are kept: one per button the model knows, plus one for the ones it
	 * does not.
	 *
	 * Derived from the last enumerator rather than written as a count, so adding a button to
	 * EToolbarButtonId grows the storage with it instead of silently overflowing it. The extra slot
	 * is where an id outside the enumeration lands — including one a cast invented, all it takes
	 * with a uint8 enum — and SessionChipStyleFor fills it with the greyed look.
	 */
	static constexpr int32 SessionChipStyleCount =
		static_cast<int32>(DestructionSession::EToolbarButtonId::RunStructure) + 2;

	/**
	 * The chips' styles, in storage that does not move.
	 *
	 * A fixed array, and the fixedness is the feature. SButton stores the `const FButtonStyle*` it
	 * is handed and never copies it, so every chip on screen is reading these objects for as long
	 * as it is up. A TMap would relocate its values on the next rehash and a local would not
	 * outlive the call — either way the next placement would paint through a dangling pointer.
	 * Rebuilding the strip overwrites these in place, so the look follows the session's state and
	 * every pointer already handed out stays good.
	 *
	 * Not a UPROPERTY, and it does not need to be: these brushes are rounded boxes with no resource
	 * object, so there is no UObject reference here for the collector to keep alive.
	 */
	FButtonStyle SessionChipStyles[SessionChipStyleCount];

	/** The rows on screen right now. Empty means no menu. */
	TArray<FPieceMenuRow> ShownPieceMenuRows;

	/** The bricks picked so far. Empty means no menu, because the menu is built from it. */
	FPieceSelection PieceSelection;

	/**
	 * The one brick under the cursor, or a default ref for none.
	 *
	 * One ref rather than a set, since there is one cursor. Not a second record of anything: which
	 * bricks are picked is the selection's, this is only what is being pointed at — the two are
	 * combined by HighlightForPiece and nowhere else.
	 */
	FPieceRef HoveredPiece;

	/**
	 * The one selected brick being read in detail, or a default ref for none.
	 *
	 * A third ref rather than a flag on the selection, for the reason HoveredPiece is one: there is
	 * one readout, the selection is the durable state, and the two are combined by
	 * HighlightForPiece and nowhere else.
	 */
	FPieceRef InspectedPiece;

	/**
	 * Which structure the load overlay was computed over, and what each of its pieces came out as.
	 *
	 * An array indexed by the piece handle, since handles are dense and never compacted — removal
	 * tombstones — so the subscript is the piece and a hole is simply a None in its own slot.
	 *
	 * The structure id is held beside it and is not decoration: piece 4 of the level's wall is not
	 * piece 4 of the player's build, and an overlay that answered on the index alone would tint one
	 * structure with another's bands the moment the session named a different one.
	 *
	 * Both empty means no overlay — the state a session opens in, and the state the second click on
	 * the chip returns it to. Not a second record of the toolbar's flag: the flag is what the
	 * player asked for, this is the answer the last solve gave for it.
	 */
	int32 LoadOverlayStructureId = INDEX_NONE;

	TArray<EBrickHighlight> LoadOverlayStates;

	/** The widget those rows are drawn as, valid only while a menu is up. */
	TSharedPtr<SWidget> PieceMenuWidget;

	/**
	 * The banner naming the scenario, valid for as long as this controller is playing.
	 *
	 * A second viewport widget with its own add and remove, held so the remove has something to
	 * hand back. Not the menu's: the menu comes and goes with every click while the banner is up
	 * the whole time, so sharing one handle would take the banner down with the first dismissal.
	 */
	TSharedPtr<SWidget> ScenarioLabelWidget;

	/**
	 * The one part of that widget the readout is swapped in and out of.
	 *
	 * A handle into the panel rather than a second viewport widget, so there is still exactly one
	 * add and one remove and the leak the dismiss-then-build discipline guards against stays a
	 * single pair. Valid exactly while PieceMenuWidget is.
	 */
	TSharedPtr<SBox> PieceMenuInspectorBox;

	/**
	 * The strip the player grabs to move the panel, held only so a press can capture to it.
	 *
	 * FReply::CaptureMouse wants the widget, and a UObject handler has no SharedThis. A handle
	 * into the panel exactly as PieceMenuInspectorBox is, valid for exactly as long, released
	 * beside it.
	 */
	TSharedPtr<SWidget> PieceMenuGrabStrip;

	/**
	 * Where the panel's top-left corner sits, in the canvas's own pixels.
	 *
	 * The panel's position is state now, which is the whole of what makes it draggable. It was an
	 * alignment before — right edge, vertically centred — a placement no player could argue with,
	 * and the complaint this answers is exactly that they could not.
	 *
	 * Starts at the origin and is moved to its home the first time a menu is shown. The origin is
	 * the one corner on screen at every viewport size and DPI scale, which is why both
	 * PieceMenuHomeOffset and ClampPanelOffset fail closed to it — the value this holds until
	 * there is a screen to measure a home against, rather than a placement anybody chose.
	 *
	 * Always the output of ClampPanelOffset or PieceMenuHomeOffset, never a raw drag. That is what
	 * the idempotence sweep in Presenter.PanelOffsetClamp is for, and why Presenter.PanelHomeOffset
	 * clamps every home it produces and insists nothing moves: this value is stored, fed back in on
	 * the next drag and clamped again on every rebuild, so a home the clamp would move is a panel
	 * that jumps the first time it is picked up.
	 */
	FVector2D PieceMenuPanelOffsetPx = FVector2D::ZeroVector;

	/**
	 * Whether the panel has ever been placed, so its home is taken once rather than on every build.
	 *
	 * The menu is torn down and rebuilt on every click, and the corner above is controller state
	 * precisely so a placement the player chose survives that. A home re-taken on each build would
	 * overwrite it and snap the panel back across the screen the next time they picked a brick —
	 * the complaint the drag affordance exists to answer, arrived at backwards.
	 */
	bool bPieceMenuPanelHasOpened = false;

	/**
	 * Where the panel was and where the cursor was when this drag began, and whether one is on.
	 *
	 * The anchor is the corner at the press rather than the last frame's, so the panel follows the
	 * cursor exactly instead of accumulating a frame of rounding per move, and so dragging into a
	 * corner and back out returns to where it started rather than wherever the clamp pinned it on
	 * the way. The flag tells a move that is a drag from one that is the cursor merely crossing
	 * the strip.
	 */
	FVector2D PieceMenuPanelGrabbedFromPx = FVector2D::ZeroVector;
	FVector2D PieceMenuCursorGrabbedAtPx = FVector2D::ZeroVector;
	bool bPieceMenuPanelIsHeld = false;

	/**
	 * How much of the readout the panel is asked for. Nothing else ever asks for less.
	 *
	 * On the controller rather than inside the widget, since the panel is rebuilt from scratch
	 * every time a menu opens and a mode held in the widget tree would be forgotten on every
	 * click. Deliberately not read by NeighbourHighlightForPiece or NeighbourPieces — see
	 * PieceMenuInspectorForSelection.
	 */
	EPieceMenuDetail PieceMenuPanelDetail = EPieceMenuDetail::Full;
};
