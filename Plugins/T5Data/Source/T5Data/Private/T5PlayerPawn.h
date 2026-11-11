#pragma once

#include "GameFramework/Character.h"
#include "T5PlayerPawn.generated.h"

UCLASS()
class AT5PlayerPawn : public ACharacter
{
    GENERATED_BODY()

public:
    AT5PlayerPawn();
    FVector Feet() const;
    FVector GetPawnViewLocation() const override;
    void PlaceFeet(const FVector& Location);
    void SetEyeHeight(float Height);
    void SetScriptedEye(TOptional<FVector> Eye);
    void SuspendMovement(bool bSuspend);
    void SetCameraDistance(float Distance);
    bool IsThirdPerson() const { return !ScriptedEye && CameraDistance > 0; }

protected:
    void SetupPlayerInputComponent(UInputComponent* Input) override;
    void CalcCamera(float DeltaTime, FMinimalViewInfo& View) override;

private:
    void MoveForward(float Value);
    void MoveRight(float Value);
    void Turn(float Rate);
    void LookUp(float Rate);
    void StartJump();
    void ToggleWalk();

    TOptional<FVector> ScriptedEye;
    float StandingEyeHeight = 164.f;
    float CameraDistance = 0.f;
    bool bWalkByDefault = false;
};
