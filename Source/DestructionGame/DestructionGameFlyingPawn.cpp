// Copyright Epic Games, Inc. All Rights Reserved.

#include "DestructionGameFlyingPawn.h"
#include "Camera/CameraComponent.h"
#include "Components/SphereComponent.h"
#include "EnhancedInputComponent.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "UObject/ConstructorHelpers.h"
#include "DestructionGame.h"

ADestructionGameFlyingPawn::ADestructionGameFlyingPawn()
{
	// fly straight through geometry; the observer should be able to sit inside a
	// collapsing structure without shoving it around
	GetCollisionComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// ADefaultPawn ships with legacy axis bindings; we bind Enhanced Input instead
	bAddDefaultMovementBindings = false;

	CameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	CameraComponent->SetupAttachment(RootComponent);
	CameraComponent->bUsePawnControlRotation = true;

	if (UFloatingPawnMovement* Movement = Cast<UFloatingPawnMovement>(GetMovementComponent()))
	{
		Movement->MaxSpeed = FlySpeed;
	}

	// bind the template's existing input assets so the pawn works with no Blueprint
	static ConstructorHelpers::FObjectFinder<UInputAction> MoveActionAsset(TEXT("/Game/Input/Actions/IA_Move.IA_Move"));
	static ConstructorHelpers::FObjectFinder<UInputAction> LookActionAsset(TEXT("/Game/Input/Actions/IA_Look.IA_Look"));
	static ConstructorHelpers::FObjectFinder<UInputAction> MouseLookActionAsset(TEXT("/Game/Input/Actions/IA_MouseLook.IA_MouseLook"));
	static ConstructorHelpers::FObjectFinder<UInputAction> AscendActionAsset(TEXT("/Game/Input/Actions/IA_Jump.IA_Jump"));

	MoveAction = MoveActionAsset.Object;
	LookAction = LookActionAsset.Object;
	MouseLookAction = MouseLookActionAsset.Object;
	AscendAction = AscendActionAsset.Object;
}

void ADestructionGameFlyingPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ADestructionGameFlyingPawn::MoveInput);
		EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &ADestructionGameFlyingPawn::LookInput);
		EnhancedInputComponent->BindAction(MouseLookAction, ETriggerEvent::Triggered, this, &ADestructionGameFlyingPawn::LookInput);
		EnhancedInputComponent->BindAction(AscendAction, ETriggerEvent::Triggered, this, &ADestructionGameFlyingPawn::AscendInput);
	}
	else
	{
		UE_LOG(LogDestructionGame, Error, TEXT("'%s' failed to find an Enhanced Input Component."), *GetNameSafe(this));
	}
}

void ADestructionGameFlyingPawn::MoveInput(const FInputActionValue& Value)
{
	const FVector2D MovementVector = Value.Get<FVector2D>();
	const FRotator ViewRotation = GetControlRotation();

	// full 3D camera-relative movement, so pitching down and pushing forward descends
	AddMovementInput(FRotationMatrix(ViewRotation).GetUnitAxis(EAxis::X), MovementVector.Y);
	AddMovementInput(FRotationMatrix(ViewRotation).GetUnitAxis(EAxis::Y), MovementVector.X);
}

void ADestructionGameFlyingPawn::LookInput(const FInputActionValue& Value)
{
	const FVector2D LookAxisVector = Value.Get<FVector2D>();

	AddControllerYawInput(LookAxisVector.X);
	AddControllerPitchInput(LookAxisVector.Y);
}

void ADestructionGameFlyingPawn::AscendInput(const FInputActionValue& Value)
{
	AddMovementInput(FVector::UpVector, Value.Get<bool>() ? 1.0f : 0.0f);
}
