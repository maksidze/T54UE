#pragma once
#include "CoreMinimal.h"
class FJsonObject;
class UStaticMeshComponent;
class UPhysicsConstraintComponent;

// Transient Chaos bodies retain the NIF's separate rigid bodies and joint frames.
class FT5Articulation
{
public:
    bool Load(AActor& Owner, const FJsonObject& Model, double Scale,
        TFunctionRef<UStaticMeshComponent*(const TArray<TSharedPtr<class FJsonValue>>&)> MakeBody);
    void Configure(const FString& Data, const FString& Plugins);
    void Start(const FTransform& ModelWorld);
    void SetPaused(bool bPaused);
    void SetMotionType(int Type) { MotionType=Type; }
    void Detach();
    void SerializeCheckpoint(FArchive& Ar);
    void CopyState(const FT5Articulation& Previous);
    bool Attach(const FTransform& AnchorWorld);
    void Drive(const FTransform& AnchorWorld);
    void Tick();
    FTransform Pose() const;
    TOptional<FTransform> NodePose(const FString& Node) const;
    TOptional<FTransform> NodeDelta(const FString& Node, const FTransform& Authored, const FTransform& Origin) const;
    FString HorseBone = TEXT("HorseSpine2");
    bool bStarted = false;
    bool bAttached = false;
    double JointError() const;
private:
    friend class FT5ArticulationSmoke;
    friend class FT5AssemblyDynamics;
    struct FBody
    {
        TWeakObjectPtr<UStaticMeshComponent> Component;
        FTransform Rest;
        FVector Center, Inertia;
        FQuat MassRotation;
        double Mass = 0, Gravity = 1;
        bool bWheel = false;
        FVector SavedLinear, SavedAngular;
    };
    struct FJoint
    {
        FString A, B;
        FTransform FrameA, FrameB;
        TWeakObjectPtr<UPhysicsConstraintComponent> Component;
    };
    TWeakObjectPtr<AActor> Owner;
    TMap<FString, FBody> Bodies;
    TArray<FJoint> Joints;
    FString Root, HitchNode;
    TWeakObjectPtr<UStaticMeshComponent> Anchor;
    TWeakObjectPtr<UPhysicsConstraintComponent> Hitch;
    FTransform HitchA, HitchB;
    FVector CartPivot{0, 3, .7}, HorsePivot{0, 0, 0}; // [Cart], Havok metres
    double LimitMin = -.75, LimitMax = .75, CartRot = 10, HorseRot = 10;
    double WheelGravity = 3.5, WheelFriction = 100, WheelAngularDamping = .01;
    bool bPaused = false, bCollisionFiltersCreated = false;
    int MotionType = 0;
    double ModelScale = 1;
    void ApplyMass(FBody& Body);
};
