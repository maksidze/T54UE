#include "T5PlayerPawn.h"

#include "Camera/CameraTypes.h"
#include "Components/CapsuleComponent.h"
#include "Components/InputComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"

AT5PlayerPawn::AT5PlayerPawn()
{
    GetCapsuleComponent()->InitCapsuleSize(26.f, 84.f);
    bUseControllerRotationYaw = false;
    auto* Movement = GetCharacterMovement();
    Movement->DefaultLandMovementMode = MOVE_None;
    Movement->MaxWalkSpeed = 370.f;
    Movement->MaxStepHeight = 40.f;
    Movement->JumpZVelocity = 380.f;
    Movement->AirControl = .3f;
    Movement->bAlwaysCheckFloor = true;
}

FVector AT5PlayerPawn::Feet() const
{
    return GetActorLocation() - FVector(0, 0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
}

FVector AT5PlayerPawn::GetPawnViewLocation() const
{
    return ScriptedEye.Get(Feet() + FVector(0, 0, StandingEyeHeight));
}

void AT5PlayerPawn::PlaceFeet(const FVector& Location)
{
    GetCharacterMovement()->StopMovementImmediately();
    SetActorLocation(Location + FVector(0, 0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight()),
        false, nullptr, ETeleportType::TeleportPhysics);
}

void AT5PlayerPawn::SetEyeHeight(float Height)
{
    StandingEyeHeight = Height;
}

void AT5PlayerPawn::SetScriptedEye(TOptional<FVector> Eye)
{
    ScriptedEye = Eye;
}

void AT5PlayerPawn::SuspendMovement(bool bSuspend)
{
    auto* Movement = GetCharacterMovement();
    const auto Collision = bSuspend ? ECollisionEnabled::NoCollision : ECollisionEnabled::QueryAndPhysics;
    if (GetCapsuleComponent()->GetCollisionEnabled() != Collision)
        GetCapsuleComponent()->SetCollisionEnabled(Collision);
    if (bSuspend && Movement->MovementMode != MOVE_None)
    {
        StopJumping();
        Movement->StopMovementImmediately();
        Movement->DisableMovement();
    }
    else if (!bSuspend && Movement->MovementMode == MOVE_None)
    {
        Movement->SetMovementMode(MOVE_Walking);
        if (!Movement->CurrentFloor.IsWalkableFloor())
            Movement->SetMovementMode(MOVE_Falling);
    }
}

void AT5PlayerPawn::SetCameraDistance(float Distance)
{
    CameraDistance = Distance;
}

void AT5PlayerPawn::CalcCamera(float, FMinimalViewInfo& View)
{
    View.Location = GetPawnViewLocation();
    View.Rotation = GetViewRotation();
    if (IsThirdPerson())
    {
        const FVector End = View.Location - View.Rotation.Vector() * CameraDistance;
        const FCollisionQueryParams Query(SCENE_QUERY_STAT(T5ThirdPersonCamera), false, this);
        FHitResult Hit;
        View.Location = GetWorld()->SweepSingleByChannel(Hit, View.Location, End, FQuat::Identity,
            ECC_Camera, FCollisionShape::MakeSphere(12.f), Query) ? Hit.Location : End;
    }
}

void AT5PlayerPawn::SetupPlayerInputComponent(UInputComponent* Input)
{
    static const bool bMapped = []
    {
        const FInputAxisKeyMapping Mappings[] = {
            FInputAxisKeyMapping(TEXT("T5Forward"), EKeys::W, 1), {TEXT("T5Forward"), EKeys::S, -1},
            {TEXT("T5Forward"), EKeys::Up, 1}, {TEXT("T5Forward"), EKeys::Down, -1},
            {TEXT("T5Forward"), EKeys::Gamepad_LeftY, 1},
            {TEXT("T5Right"), EKeys::D, 1}, {TEXT("T5Right"), EKeys::A, -1}, {TEXT("T5Right"), EKeys::Gamepad_LeftX, 1},
            {TEXT("T5MouseX"), EKeys::MouseX, 1}, {TEXT("T5MouseY"), EKeys::MouseY, -1},
            {TEXT("T5Turn"), EKeys::Gamepad_RightX, 1}, {TEXT("T5Turn"), EKeys::Right, 1},
            {TEXT("T5Turn"), EKeys::Left, -1}, {TEXT("T5LookUp"), EKeys::Gamepad_RightY, 1}};
        for (const auto& Mapping : Mappings)
            UPlayerInput::AddEngineDefinedAxisMapping(Mapping);
        return true;
    }();
    static_cast<void>(bMapped);
    Input->BindAxis(TEXT("T5Forward"), this, &AT5PlayerPawn::MoveForward);
    Input->BindAxis(TEXT("T5Right"), this, &AT5PlayerPawn::MoveRight);
    Input->BindAxis(TEXT("T5MouseX"), this, &APawn::AddControllerYawInput);
    Input->BindAxis(TEXT("T5MouseY"), this, &APawn::AddControllerPitchInput);
    Input->BindAxis(TEXT("T5Turn"), this, &AT5PlayerPawn::Turn);
    Input->BindAxis(TEXT("T5LookUp"), this, &AT5PlayerPawn::LookUp);
    Input->BindKey(EKeys::CapsLock, IE_Pressed, this, &AT5PlayerPawn::ToggleWalk);
    for (const auto Key : {EKeys::SpaceBar, EKeys::Gamepad_FaceButton_Bottom})
    {
        Input->BindKey(Key, IE_Pressed, this, &AT5PlayerPawn::StartJump);
        Input->BindKey(Key, IE_Released, this, &ACharacter::StopJumping);
    }
}

void AT5PlayerPawn::MoveForward(float Value)
{
    if (const auto* PC = Cast<APlayerController>(Controller))
        GetCharacterMovement()->MaxWalkSpeed = bWalkByDefault != PC->IsInputKeyDown(EKeys::LeftShift) ? 135.f : 370.f;
    if (Value != 0 && Controller)
        AddMovementInput(FRotator(0, Controller->GetControlRotation().Yaw, 0).Vector(), Value);
}

void AT5PlayerPawn::MoveRight(float Value)
{
    if (Value != 0 && Controller)
        AddMovementInput(FRotator(0, Controller->GetControlRotation().Yaw + 90, 0).Vector(), Value);
}

void AT5PlayerPawn::Turn(float Rate)
{
    AddControllerYawInput(Rate * 45.f * GetWorld()->GetDeltaSeconds());
}

void AT5PlayerPawn::LookUp(float Rate)
{
    AddControllerPitchInput(Rate * 45.f * GetWorld()->GetDeltaSeconds());
}

void AT5PlayerPawn::StartJump()
{
    if (!IsMoveInputIgnored() && !ScriptedEye) Jump();
}

void AT5PlayerPawn::ToggleWalk()
{
    if (!IsMoveInputIgnored()) bWalkByDefault = !bWalkByDefault;
}
