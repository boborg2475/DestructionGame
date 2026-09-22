// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Core/StructureBinding.h"
#include "GameFramework/Actor.h"
#include "BrickActor.generated.h"

class UMaterialInterface;
class UStaticMeshComponent;

// Forward-declared so World does not include the whole presenter.
enum class EJointMarginBand : uint8;

/**
 * How a brick is called out to the player: hovered, selected, inspected, the far end of joint
 * row N, or a load-overlay band. Separate states so a hovered brick never looks like a chosen one.
 *
 * Precedence is decided in ADestructionGamePlayerController::HighlightForPiece (pinned by
 * World.Select.ClickingTogglesTheSelectionAndHoverHighlights), not by enum order. None is zero so
 * a zero-initialised brick is plain. Which material each state uses is tested; its look is not.
 */
enum class EBrickHighlight : uint8
{
	None = 0,
	Hovered,
	Selected,
	Inspected,

	/*
	 * Far end of joint row N, one per readout colour slot. Separate enumerators rather than a slot
	 * field, so the state alone is the whole answer and SetHighlighted stays idempotent.
	 */
	Neighbour0,
	Neighbour1,
	Neighbour2,
	Neighbour3,
	Neighbour4,
	Neighbour5,

	/*
	 * Load overlay, one per EJointMarginBand. Lowest precedence, so hover, selection and readout
	 * colours still show with the overlay on.
	 */
	LoadComfortable,
	LoadCaution,
	LoadCritical
};

/**
 * The highlight for the far end of the joint row with this colour slot. The single slot-to-state
 * mapping; a switch, not enum arithmetic. INDEX_NONE or out of range fails closed to None.
 */
EBrickHighlight BrickHighlightForNeighbourSlot(int32 ColourSlot);

/**
 * The load-overlay highlight for a band. The single band-to-state mapping. An unknown band fails
 * closed to None rather than a misleading green.
 */
EBrickHighlight BrickHighlightForLoadBand(EJointMarginBand Band);

/**
 * One brick in the world: the actor a piece handle points at. In World/ to keep Core free of
 * actors. Does not tick; solver results are pushed via FStructureBinding::ApplyResults.
 */
UCLASS()
class DESTRUCTIONGAME_API ABrickActor : public AActor
{
	GENERATED_BODY()

public:

	ABrickActor();

	/** The brick's body. Movable from spawn, kinematic until Release. */
	UStaticMeshComponent* GetMesh() const;

	/** Hand this brick to physics. Idempotent and irreversible. */
	void Release();

	/**
	 * Set the brick's highlight state. Idempotent and order-free. Uses an overlay material so
	 * clearing it needs no memory of the base material.
	 */
	void SetHighlighted(EBrickHighlight Highlight);

	/** Current highlight. None by default. */
	EBrickHighlight GetHighlight() const;

	/** The solver's ref for this brick. */
	const FPieceRef& GetPieceRef() const;

	/** Set once, at spawn. */
	void SetPieceRef(const FPieceRef& Ref);

private:

	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> Mesh;

	/**
	 * Overlay materials, resolved onto the CDO from RequiredContent.h paths. Named properties, not
	 * an enum-indexed array, since None's null slot would look like a broken reference to the
	 * required-content sweep.
	 */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> HoverMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> SelectedMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> InspectedMaterial;

	/** Load-overlay materials, named for the same reason. */
	UPROPERTY()
	TObjectPtr<UMaterialInterface> LoadComfortableMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> LoadCautionMaterial;

	UPROPERTY()
	TObjectPtr<UMaterialInterface> LoadCriticalMaterial;

	/** Joint-readout colour materials, indexed by slot. An array is fine here: every entry is real. */
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInterface>> NeighbourMaterials;

	FPieceRef PieceRef;

	EBrickHighlight Highlight = EBrickHighlight::None;
};
