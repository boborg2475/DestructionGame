// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/PieceMenu.h"
#include "Core/PieceSelection.h"
#include "Core/SessionToolbar.h"
#include "GameFramework/PlayerController.h"
#include "Input/Reply.h"
#include "Layout/Margin.h"
// Full include: SessionChipStyles is a member array of FButtonStyle, which a forward declaration can't express.
#include "Styling/SlateTypes.h"
#include "World/ScenarioLabel.h"
#include "DestructionGamePlayerController.generated.h"

class SBox;
class SWidget;
class UBuildModeComponent;
class UGameViewportClient;
class UInputAction;
class UInputMappingContext;

// Forward-declared to keep Slate's event headers out of every test that includes this controller.
struct FGeometry;
struct FPointerEvent;

// Defined in World/BrickActor.h, which only the .cpp needs.
enum class EBrickHighlight : uint8;

/** Player controller for the destruction sandbox: input, session toolbar, piece menu and highlights. */
UCLASS(config="Game")
class DESTRUCTIONGAME_API ADestructionGamePlayerController : public APlayerController
{
	GENERATED_BODY()

public:

	ADestructionGamePlayerController();

	/** The build loop this session lays pieces with. A default subobject, so it is never missing. */
	UBuildModeComponent* GetBuildComponent() const;

	/** Session state the toolbar draws. Read-only: OnToolbarButton is the single mutator. */
	const DestructionSession::FSessionToolbarState& GetSessionToolbarState() const;

	/**
	 * Apply one toolbar click to the session and the world. Every input (chips and shortcuts)
	 * goes through here so the model's refusals (Core/SessionToolbar.h) apply uniformly. A refused
	 * click changes nothing.
	 *
	 * @return whether the click landed; false is a refusal, not a failure.
	 */
	bool OnToolbarButton(DestructionSession::EToolbarButtonId Id);

	/**
	 * Tab/G: read the current state and dispatch the other id of the pair through OnToolbarButton.
	 *
	 * @return whether the toggle landed; false is a refusal, not a failure.
	 */
	bool ToggleSessionMode();
	bool ToggleSessionPlacement();

	/** The structure the session's commands act on (the player's build first, else the level's wall), or INDEX_NONE. */
	int32 GetSessionStructureId() const;

	/**
	 * Pointer moved along this ray: in Build the ghost follows it, in Destroy the brick under it is
	 * highlighted. Takes a ray rather than a cursor so tests need no viewport.
	 */
	void PointerAlongRay(const FVector& StartCm, const FVector& EndCm);

	/**
	 * Primary click along this ray: in Build it lays a piece, in Destroy it inspects one. No piece
	 * menu in Build, or the click that laid a brick would open a Delete menu over it.
	 *
	 * @return whether a piece landed or a menu came up.
	 */
	bool PrimaryAlongRay(const FVector& StartCm, const FVector& EndCm);

	/**
	 * Move the ghost to where this ray points; the testable half of the per-tick cursor refresh.
	 * Does nothing in Destroy mode, so no ghost appears over a wall being demolished.
	 *
	 * @return whether a valid preview is up: false in Destroy mode, or on a ray missing the plane.
	 */
	bool RefreshBuildPreviewFromRay(const FVector& OriginCm, const FVector& Direction);

	/** Build the toolbar strip and return it without drawing it. Public so headless tests can arrange it. */
	TSharedRef<SWidget> BuildSessionToolbarPanel();

	/**
	 * The style a chip wears. Returns a reference into fixed storage because SButton keeps the raw
	 * pointer and never copies it. Public because SButton has no style getter. An unknown id gets
	 * the greyed, fail-closed style.
	 */
	const FButtonStyle& SessionChipStyleFor(const DestructionSession::FToolbarButton& Button) const;

	/** Put the strip on screen, and rebuild it when the session changes. No-op without a viewport (headless). */
	void ShowSessionToolbar();
	void RefreshSessionToolbar();

	/**
	 * Inspect whatever this ray hits: open the piece menu for it, or dismiss the menu. Takes a ray
	 * so tests need no viewport; the input handler deprojects and calls this.
	 *
	 * @return the rows now presented. Empty means dismissed (floor, nothing, or a removed piece), so
	 *         a stale menu never names a brick the player is no longer pointing at.
	 */
	TArray<FPieceMenuRow> InspectAlongRay(const FVector& StartCm, const FVector& EndCm);

	/**
	 * Highlight whatever this ray hits. Changes only the hovered brick: no selection or menu change.
	 * A selected brick stays Selected when hovered (the stronger state wins).
	 *
	 * @return the piece now hovered, or a default ref when the ray hit nothing.
	 */
	FPieceRef HoverAlongRay(const FVector& StartCm, const FVector& EndCm);

	/** The bricks the player has picked. The menu is rebuilt from this and is up exactly when it is non-empty. */
	const FPieceSelection& GetPieceSelection() const;

	/**
	 * Single out one selected brick for the joint readout, without changing the selection or rows.
	 * A default ref singles out nothing (cursor left the list).
	 */
	void SetInspectedPiece(const FPieceRef& Ref);

	/**
	 * Present these rows as the piece menu, replacing any existing one. No separate ref parameter:
	 * each row carries its own ref (Core/PieceMenu.h), so a stale ref can't target the wrong brick.
	 * An empty list dismisses, so every miss route takes the old menu down.
	 *
	 * @return whether a menu is now presented; false for an empty list.
	 */
	bool ShowPieceMenu(TArrayView<const FPieceMenuRow> Rows);

	/** Take the piece menu down. @return whether there was one to take down. */
	bool DismissPieceMenu();

	/** Whether a piece menu is currently presented. */
	bool IsPieceMenuShown() const;

	/** The rows currently presented, empty when no menu is up. */
	TArrayView<const FPieceMenuRow> GetShownPieceMenuRows() const;

	/**
	 * Commit the presented row at this index and take the menu down.
	 *
	 * @return whether the chosen row's action actually committed.
	 */
	bool ChoosePieceMenuRow(int32 RowIndex);

	/**
	 * Build the whole menu panel and return it without drawing it. Public so headless tests can
	 * prepass and arrange its layout. Assigns PieceMenuInspectorBox, so SetInspectedPiece can swap
	 * the readout without rebuilding the panel.
	 */
	TSharedRef<SWidget> BuildPieceMenuPanel();

	/**
	 * The readout model for the current selection and inspected brick, or an empty one when there
	 * is no world, subsystem or structure. Public because headless worlds have no viewport, so its
	 * panel callers never reach it in tests.
	 *
	 * Defaults to Full because NeighbourHighlightForPiece and NeighbourPieces key highlights on
	 * Inspector.Joints; a compact answer would silently drop every neighbour colour. Only the two
	 * panel builders pass PieceMenuPanelDetail.
	 */
	FPieceMenuInspector PieceMenuInspectorForSelection(
		EPieceMenuDetail Detail = EPieceMenuDetail::Full) const;

	/**
	 * Set the detail the next BuildPieceMenuPanel draws at. Test seam: the real toggle
	 * (double-click) needs a viewport. Sets the field only; no rebuild.
	 */
	void SetPieceMenuDetail(EPieceMenuDetail Detail);

protected:

	/** Input mapping contexts applied on possession. */
	UPROPERTY(EditAnywhere, Category="Input|Input Mappings")
	TArray<TObjectPtr<UInputMappingContext>> DefaultMappingContexts;

	/** Opens the piece context menu. Path comes from RequiredContent.h. */
	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> InspectPieceAction;

	/**
	 * Keeps the highlight following the cursor. Separate from IA_MouseLook (also Mouse2D) because
	 * free-look needs the right button held and hover must work with none. RequiredContent.h says
	 * why the asset must not consume the axis.
	 */
	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputAction> HoverPieceAction;

	/**
	 * The free-look context; the same pointer as its entry in DefaultMappingContexts. Its Mouse2D
	 * mapping is chorded on IA_LookModifier, so look only happens with the right button held
	 * (SESSION_UI_DESIGN §d, S6).
	 */
	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputMappingContext> MouseLookMappingContext;

	/**
	 * The session keyboard context; the same pointer as its entry in DefaultMappingContexts. Applied
	 * one priority above the others because IMC_MouseLook's chord needs IA_LookModifier (mapped
	 * here) evaluated first in the frame. See SessionMappingContextPriority in the .cpp.
	 */
	UPROPERTY(EditAnywhere, Category="Input")
	TObjectPtr<UInputMappingContext> SessionMappingContext;

	/**
	 * The eight session shortcuts, one property each since each binds a different dispatch. Paths
	 * come from RequiredContent.h. IA_LookModifier is not here: it only feeds IMC_MouseLook's chord.
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
	 * Per-frame cursor refresh of the build ghost. On tick rather than IA_HoverPiece because an axis
	 * action only fires on movement, so a still mouse would show no ghost (owner report 2026-09-16).
	 */
	virtual void PlayerTick(float DeltaTime) override;

	virtual void SetupInputComponent() override;

	/** Show the scenario banner, and remove it with the world. */
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:

	/**
	 * IA_InspectPiece handler: deproject the cursor and call InspectAlongRay. Untestable (needs a
	 * viewport), so keep it to that one call.
	 */
	void OnInspectPiece();

	/**
	 * IA_HoverPiece handler: deproject the cursor and call HoverAlongRay. Untestable, so keep it to
	 * that one call. Bound on Triggered, so it traces once per frame the mouse moves.
	 */
	void OnHoverPiece();

	/**
	 * PlayerTick handler: deproject the cursor and call RefreshBuildPreviewFromRay. Untestable, so
	 * keep it to that. Skipped while the look chord is held (the camera is turning and the cursor
	 * is hidden) and when the mouse has not moved.
	 */
	void RefreshBuildPreviewFromCursor();

	/** Cursor position at the last refresh. The flag makes a first reading at (0, 0) refresh rather than skip. */
	FVector2D LastBuildCursorPx = FVector2D::ZeroVector;

	bool bHasBuildCursorPx = false;

	/**
	 * Set up the session's cursor and input mode once; there is no restore. The old per-menu
	 * apply/restore is gone because IMC_MouseLook is now chorded on IA_LookModifier
	 * (SESSION_UI_DESIGN §d, S6), so the toolbar can stay clickable all session.
	 *
	 * The cursor hides during the right-drag capture. The mouse is not locked to the viewport. The
	 * cursor flag is set first because a controller without a ULocalPlayer has no viewport for the
	 * input mode.
	 */
	void SetSessionControls();

	/**
	 * Enhanced Input handlers for the shortcuts. Six share OnSessionShortcut with the id bound as
	 * payload; the two toggles wrap the public ToggleSession* seams.
	 */
	void OnSessionShortcut(DestructionSession::EToolbarButtonId Id);
	void OnSessionToggleMode();
	void OnSessionTogglePlacement();

	/**
	 * Add and remove the piece menu viewport widget. Untestable (headless worlds have no
	 * UGameViewportClient), so it holds no logic: a button per row calling ChoosePieceMenuRow, and
	 * readout strings taken verbatim from FPieceMenuInspector (Core/PieceMenu.h). A branch here is
	 * drift; a missing string belongs in the model.
	 *
	 * Paired with ShowPieceMenu and DismissPieceMenu, which dismiss before building so an add is
	 * never left without its remove.
	 */
	void BuildPieceMenuWidget();
	void RemovePieceMenuWidget();

	/**
	 * Redraw only the joint readout for the inspected brick. Rebuilding the whole panel would
	 * destroy the entry button being hovered and re-fire OnHovered on its replacement.
	 */
	void RefreshPieceMenuInspectorWidget();

	/** Row button click: calls ChoosePieceMenuRow. */
	FReply OnPieceMenuRowClicked(int32 RowIndex);

	/** Chip click: calls OnToolbarButton. */
	FReply OnSessionToolbarButtonClicked(DestructionSession::EToolbarButtonId Id);

	/** Remove the strip. Paired with ShowSessionToolbar; run on EndPlay. */
	void RemoveSessionToolbarWidget();

	/**
	 * Rewrite every chip's style for the current state, in place, before the chips are made. In
	 * place because the slots must not move while any chip points at one.
	 */
	void RebuildSessionChipStyles(const TArray<DestructionSession::FToolbarButton>& Buttons);

	/**
	 * Recompute bHasStructure from the world. Derived rather than set, so no removal route (delete,
	 * cascade, Clear) can leave it stale. Counts live pieces, since RemovePiece leaves tombstones.
	 */
	void RefreshSessionHasStructure();

	/**
	 * Recompute the load overlay, or clear it. When on, solves non-destructively
	 * (FStructureBinding::SolveLoads breaks nothing) and gives each live piece its worst joint's
	 * band. It feeds HighlightForPiece as the weakest state rather than painting bricks directly,
	 * so it never fights hover or selection.
	 *
	 * Called on every change that can move the answer (toggle, Run, Delete, placement), never per
	 * frame; solve cost is why the overlay is optional.
	 */
	void RefreshLoadOverlay();

	/**
	 * Add and remove the scenario banner. Untestable headless, so it holds no logic: three text
	 * blocks reading FScenarioLabel (built by DestructionScenarios::BuildScenarioLabel, tested by
	 * World.Scenarios.Label). A branch here is drift.
	 */
	void BuildScenarioLabelWidget();
	void RemoveScenarioLabelWidget();

	/**
	 * The banner's current label, re-read on every paint because the cut countdown changes each
	 * frame. Empty when the game mode is not ours.
	 */
	DestructionScenarios::FScenarioLabel ScenarioLabelNow() const;

	FText ScenarioLabelTitleText() const;
	FText ScenarioLabelExpectationText() const;
	FText ScenarioLabelCutText() const;

	/**
	 * Title-strip drag: press, move, release. All the limits are ClampPanelOffset's (Core, tested by
	 * Presenter.PanelOffsetClamp), so these only compute a delta.
	 *
	 * The cursor is converted with the strip's geometry (AbsoluteToLocal) rather than read in screen
	 * pixels, because the canvas lays out in DPI-scaled space. The press captures the mouse so a fast
	 * drag keeps receiving moves after leaving the strip.
	 */
	FReply OnPieceMenuPanelGrabbed(const FGeometry& Geometry, const FPointerEvent& Event);
	FReply OnPieceMenuPanelDragged(const FGeometry& Geometry, const FPointerEvent& Event);
	FReply OnPieceMenuPanelReleased(const FGeometry& Geometry, const FPointerEvent& Event);

	/**
	 * Title-strip double-click: toggle the panel's detail mode. A gesture rather than a captioned
	 * button because a caption would need a model field; that follow-up is in CURRENT_STATE.md.
	 */
	FReply OnPieceMenuPanelDetailToggled(const FGeometry& Geometry, const FPointerEvent& Event);

	/** PieceMenuPanelOffsetPx as the canvas slot's offset. An attribute, so a drag moves the panel without a rebuild. */
	FMargin PieceMenuPanelSlotOffset() const;

	/**
	 * Viewport size in the canvas's DPI-scaled units, read from the panel's root. Not
	 * GetViewportSize, which is in screen pixels and would be off by the DPI scale. Zero with no
	 * panel, which clamps offsets to the origin (fail-closed).
	 */
	FVector2D PieceMenuViewportSizePx() const;

	/**
	 * The same size from the viewport client, divided by the DPI scale, for when the panel was
	 * built this frame and its root has no size yet. Zero when the scale is not positive, which puts
	 * the home at the origin.
	 */
	FVector2D PieceMenuViewportSizeAtOpenPx(const UGameViewportClient& Viewport) const;

	/** Entry-row hover: single that brick out for the readout, and release it. */
	void OnPieceMenuEntryHovered(FPieceRef Ref);
	void OnPieceMenuEntryUnhovered();

	/**
	 * The highlight state a brick should show, and applying it. The single place precedence is
	 * decided (a selected brick under the cursor reads Selected), derived from the selection and
	 * refs rather than stored per brick. A ref naming no brick is a no-op, so destroyed pieces are
	 * safe to pass.
	 */
	EBrickHighlight HighlightForPiece(const FPieceRef& Ref) const;
	void RefreshPieceHighlight(const FPieceRef& Ref);

	/**
	 * The neighbour colour this brick wears, read from FInspectorJointRow::ColourSlot rather than
	 * re-derived, so the panel and the bricks cannot disagree. None when it has no row, its row is
	 * past the palette, or nothing is singled out.
	 */
	EBrickHighlight NeighbourHighlightForPiece(const FPieceRef& Ref) const;

	/**
	 * The load-overlay band for this brick, read from LoadOverlayStates rather than re-solved. None
	 * when the overlay is off, the ref is another structure's, the piece is newer than the last
	 * refresh, or it is a hole.
	 */
	EBrickHighlight LoadHighlightForPiece(const FPieceRef& Ref) const;

	/**
	 * Every brick the readout points at, in row order. Callers take it before and after a change
	 * and refresh both, or the old neighbours stay lit. Includes removed pieces; RefreshPieceHighlight
	 * handles refs with no actor.
	 */
	TArray<FPieceRef> NeighbourPieces() const;

	/**
	 * Refresh the current neighbours and the former ones (WereNeighbours, taken via NeighbourPieces
	 * before the change). Former neighbours must be passed in because nothing else touches them.
	 * Called when the inspected brick changes and when the selection changes under it.
	 */
	void RefreshNeighbourHighlights(TArrayView<const FPieceRef> WereNeighbours);

	/** Hover this piece (or nothing), releasing the previous one. */
	void SetHoveredPiece(const FPieceRef& Ref);

	/** Clear the selection and un-highlight its bricks. Copies the refs first because Clear empties their array. */
	void ClearPieceSelection();

	/**
	 * The build loop: live build, ghost and piece settings. A default subobject. SessionToolbarState
	 * mirrors its settings, kept in step only by OnToolbarButton.
	 */
	UPROPERTY()
	TObjectPtr<UBuildModeComponent> BuildComponent;

	/**
	 * Session state, changed only by OnToolbarButton. Opens in Destroy, unlike the model's default
	 * (see the constructor), because nearly every level starts with a wall to pull apart.
	 */
	DestructionSession::FSessionToolbarState SessionToolbarState;

	/** The toolbar strip widget, rebuilt on every state change. Separate handle from the banner. */
	TSharedPtr<SWidget> SessionToolbarWidget;

	/**
	 * One style slot per EToolbarButtonId plus one for an out-of-range id (greyed). Derived from the
	 * last enumerator so a new button grows the storage.
	 */
	static constexpr int32 SessionChipStyleCount =
		static_cast<int32>(DestructionSession::EToolbarButtonId::RunStructure) + 2;

	/**
	 * Chip styles in a fixed array: SButton keeps the raw `const FButtonStyle*` it is given, so the
	 * storage must never move (a TMap would rehash, a local would dangle). Rebuilds overwrite in
	 * place. Not a UPROPERTY: the brushes hold no UObject resources.
	 */
	FButtonStyle SessionChipStyles[SessionChipStyleCount];

	/** The rows on screen. Empty means no menu. */
	TArray<FPieceMenuRow> ShownPieceMenuRows;

	/** The picked bricks. The menu is built from this. */
	FPieceSelection PieceSelection;

	/** The brick under the cursor, or a default ref. Combined with the selection only in HighlightForPiece. */
	FPieceRef HoveredPiece;

	/** The selected brick the readout details, or a default ref. Combined only in HighlightForPiece. */
	FPieceRef InspectedPiece;

	/**
	 * The load overlay: the structure it was solved for, and a state per piece handle (handles are
	 * never compacted, so a hole is None in its slot). The id stops one structure's bands tinting
	 * another's same-index piece. Both empty means no overlay.
	 */
	int32 LoadOverlayStructureId = INDEX_NONE;

	TArray<EBrickHighlight> LoadOverlayStates;

	/** The menu widget, valid only while a menu is up. */
	TSharedPtr<SWidget> PieceMenuWidget;

	/** The scenario banner widget. Its own handle, so dismissing the menu cannot remove it. */
	TSharedPtr<SWidget> ScenarioLabelWidget;

	/** The box inside the panel the readout is swapped into. Valid exactly while PieceMenuWidget is. */
	TSharedPtr<SBox> PieceMenuInspectorBox;

	/**
	 * The title strip, held so a press can capture the mouse to it (a UObject has no SharedThis).
	 * Valid exactly while PieceMenuWidget is.
	 */
	TSharedPtr<SWidget> PieceMenuGrabStrip;

	/**
	 * The panel's top-left corner in canvas pixels. Starts at the origin (on screen at any size or
	 * DPI) and moves to its home on first show. Always an output of ClampPanelOffset or
	 * PieceMenuHomeOffset, never a raw drag; since it is re-clamped on every rebuild, a home the
	 * clamp would move makes the panel jump (Presenter.PanelHomeOffset).
	 */
	FVector2D PieceMenuPanelOffsetPx = FVector2D::ZeroVector;

	/** Whether the home has been taken, so a player's placement survives the menu's per-click rebuild. */
	bool bPieceMenuPanelHasOpened = false;

	/**
	 * Panel corner and cursor at the press, and whether a drag is on. Anchored at the press, not
	 * the last frame, so rounding does not accumulate and dragging back returns to the start.
	 */
	FVector2D PieceMenuPanelGrabbedFromPx = FVector2D::ZeroVector;
	FVector2D PieceMenuCursorGrabbedAtPx = FVector2D::ZeroVector;
	bool bPieceMenuPanelIsHeld = false;

	/**
	 * The panel's detail mode. Held here because the widget is rebuilt on every open. Not read by
	 * the neighbour highlights; see PieceMenuInspectorForSelection.
	 */
	EPieceMenuDetail PieceMenuPanelDetail = EPieceMenuDetail::Full;
};
