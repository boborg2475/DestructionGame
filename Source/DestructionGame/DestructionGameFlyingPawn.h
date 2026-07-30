// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/DefaultPawn.h"
#include "DestructionGameFlyingPawn.generated.h"

class UCameraComponent;
class UInputAction;
struct FInputActionValue;

/**
 *  Free-flying observer pawn.
 *  Movement is relative to the camera, so looking down and moving forward descends.
 *  There is no gravity and no ground contact: this exists to fly around a structure
 *  and watch it come apart, not to be a game character.
 */
UCLASS()
class DESTRUCTIONGAME_API ADestructionGameFlyingPawn : public ADefaultPawn
{
	GENERATED_BODY()

public:

	ADestructionGameFlyingPawn();

protected:

	/** Camera the player sees through */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCameraComponent> CameraComponent;

	/** Ascend Input Action (bound to the Jump action asset) */
	UPROPERTY(EditAnywhere, Category = "Input")
	TObjectPtr<UInputAction> AscendAction;

	/** Move Input Action */
	UPROPERTY(EditAnywhere, Category = "Input")
	TObjectPtr<UInputAction> MoveAction;

	/** Look Input Action (gamepad) */
	UPROPERTY(EditAnywhere, Category = "Input")
	TObjectPtr<UInputAction> LookAction;

	/** Mouse Look Input Action */
	UPROPERTY(EditAnywhere, Category = "Input")
	TObjectPtr<UInputAction> MouseLookAction;

	/** Flight speed in cm/s. Deliberately slow: test structures are at real-world scale. */
	UPROPERTY(EditAnywhere, Category = "Movement", meta = (ClampMin = "1.0"))
	float FlySpeed = 600.0f;

	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	/** Camera-relative planar movement */
	void MoveInput(const FInputActionValue& Value);

	/** Look/aim */
	void LookInput(const FInputActionValue& Value);

	/** Straight-up movement in world space */
	void AscendInput(const FInputActionValue& Value);

public:

	/** Returns the camera component **/
	UCameraComponent* GetCameraComponent() const { return CameraComponent; }
};
