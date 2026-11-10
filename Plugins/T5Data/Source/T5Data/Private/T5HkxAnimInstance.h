#pragma once

#include "Animation/AnimInstance.h"
#include "T5Hkx.h"
#include "T5HkxAnimInstance.generated.h"

class FT5HkxAnimProxy;

UCLASS(Transient)
class UT5HkxAnimInstance final : public UAnimInstance
{
    GENERATED_BODY()

public:
    bool Start(TSharedPtr<const FT5HkxClip> Clip, const FT5HkxPlayback& Playback,
        TFunction<void(const FString&)> OnAnnotation);
    void ClearLayer() { Layer.Reset(); }
    void SetFixedPose(TArray<FTransform> Pose) { FixedPose = MoveTemp(Pose); }
    bool StartLayer(TSharedPtr<const FT5HkxClip> InClip, const FT5HkxPlayback& Playback);
    void SetLookTarget(TOptional<FVector> Target) { LookTarget = Target; }
    float PlaybackPosition() const { return Position; }
    void Synchronize(float InPosition, bool bLayer = false)
    {
        if (bLayer) { bLayerExternalClock = true; LayerPosition = InPosition; return; }
        bExternalClock = true;
        Position = FMath::Clamp(InPosition, Begin, End);
    }

private:
    FAnimInstanceProxy* CreateAnimInstanceProxy() override;
    void DestroyAnimInstanceProxy(FAnimInstanceProxy* Proxy) override;
    void PreUpdateAnimation(float DeltaSeconds) override;
    void EmitAnnotations(float From, float To, bool bIncludeStart);

    TSharedPtr<const FT5HkxClip> Clip;
    TSharedPtr<const FT5HkxClip> Layer;
    FT5HkxPlayback LayerPlayback;
    float LayerPosition = 0;
    bool bLayerExternalClock = false;
    TArray<int32> MeshToSource;
    TArray<FTransform> BindFromHkx;
    TArray<FTransform> FixedPose;
    TFunction<void(const FString&)> OnAnnotation;
    TBitArray<> FiredAnnotations;
    TOptional<FVector> LookTarget;
    int32 HeadIndex = INDEX_NONE;
    FVector HeadForward = FVector::ForwardVector;
    float Position = 0;
    float Begin = 0;
    float End = 0;
    float Speed = 1;
    bool bLoop = false;
    bool bFirstUpdate = true;
    bool bExternalClock = false;

    friend class FT5HkxAnimProxy;
};
