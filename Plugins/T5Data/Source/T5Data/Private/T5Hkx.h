#pragma once

#include "CoreMinimal.h"

class USkeletalMeshComponent;
class FT5MotionDatabase;
struct FT5MotionCurve;

struct FT5HkxBone
{
    FName Name;
    int32 Parent = INDEX_NONE;
    FTransform Rest;
};

struct FT5HkxAnnotation
{
    float Time = 0;
    FString Event;
    bool bOnce = false;
};

struct FT5HkxRagdoll
{
    TArray<FName> Bodies;
    TArray<TPair<FName, FName>> Joints;
};

struct FT5HkxClip
{
    TArray<FT5HkxBone> Bones;
    TArray<int32> BoneToTrack;
    TArray<FTransform> Samples;
    TArray<FT5HkxAnnotation> Annotations;
    int32 Frames = 0;
    int32 Tracks = 0;
    float Duration = 0;
    TSharedPtr<const FT5MotionCurve> Motion;
};

struct FT5HkxPlayback
{
    struct FTrigger
    {
        float Time = 0;
        FString Event;
        bool bRelativeToEnd = false;
        bool bOnce = false;
        bool operator==(const FTrigger&) const = default;
    };

    FString Path;
    float Speed = 1;
    float CropStart = 0;
    float CropEnd = 0;
    float StartTime = 0;
    bool bLoop = false;
    TArray<FTrigger> Triggers;
    TArray<FString> EnterEvents, ExitEvents;
    FString GeneratorName;
    // Normalized per-bone contribution of transitions inside a masked blender.
    TArray<float> BoneWeights;
    bool bReturnToLocomotion = false;
    bool bClearLayer = false;
};

bool ReadT5HkxClip(TConstArrayView<uint8> Skeleton, TConstArrayView<uint8> Animation, FT5HkxClip& Out, FString& Error);
bool ReadT5HkxRagdoll(TConstArrayView<uint8> Skeleton, FT5HkxRagdoll& Out);
bool RemapT5HkxBoneWeights(TConstArrayView<uint8> ReferenceRig, const TArray<FT5HkxBone>& Target,
    TArray<float>& Weights, FString& Error);
bool ResolveT5HkxEvent(TConstArrayView<uint8> Graph, const FString& Event, FT5HkxPlayback& Out, FString& Error);
bool ReadT5HkxClipEvents(TConstArrayView<uint8> Graph, const FString& ClipName, FT5HkxPlayback& Playback);
bool ResolveT5HkxGraph(const FString& Path, const FString& Event,
    TFunctionRef<bool(const FString&, TArray<uint8>&)> Read, FT5HkxPlayback& Out, FString& Error);
FString ReadT5HkxProjectCharacter(TConstArrayView<uint8> Project);
bool ReadT5HkxCharacterClips(TConstArrayView<uint8> Character, FString& Rig, TArray<FString>& Clips);
bool LoadT5HkxCharacterProject(const FString& Project,
    TFunctionRef<bool(const FString&, TArray<uint8>&)> Read, FString& Rig, TArray<FString>& Clips);
void BindT5HkxEvents(FT5HkxClip& Clip, const FT5HkxPlayback& Playback);

bool PlayT5Hkx(USkeletalMeshComponent& Mesh, const FString& Behavior, const FString& Event, const FString& DirectClip,
    TFunctionRef<bool(const FString&, TArray<uint8>&)> Read, FString& Error, const FString& SkeletonPath = {},
    TFunction<void(const FString&)> OnAnnotation = {}, float DirectSpeed = 1.f, bool bNotifyEnd = false,
    bool bLoop = false, TOptional<float> ResumeAt = {});

struct FT5HkxPlaybackClock
{
    FT5HkxClip Clip;
    FT5HkxPlayback Playback;
    double LastTravel = -1;
    TBitArray<> Fired;
};
float T5HkxPosition(const FT5HkxPlaybackClock& Clock, float Elapsed);
FTransform T5HkxMotion(const FT5HkxPlaybackClock& Clock, float Elapsed);
bool T5HkxFinished(const FT5HkxPlaybackClock& Clock, float Elapsed);
void DispatchT5HkxEvents(FT5HkxPlaybackClock& Clock, float Elapsed, TFunctionRef<void(const FString&)> Dispatch);
bool LoadT5HkxAnimation(const FString& Behavior, const FString& Event, const FString& DirectClip,
    TFunctionRef<bool(const FString&, TArray<uint8>&)> Read, FString& Error, const FString& SkeletonPath,
    float DirectSpeed, bool bNotifyEnd, bool bLoop, FT5HkxPlaybackClock& Out,
    const FT5MotionDatabase* MotionData = nullptr, const FString& Project = {});
