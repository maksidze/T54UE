#include "T5HkxCamera.h"
#include "T5MotionData.h"

#include <cmath>
#include "Misc/Paths.h"

bool LoadT5HkxPlayerCamera(const FString& Behavior, const FString& Event,
    TFunctionRef<bool(const FString&, TArray<uint8>&)> Read, FT5HkxCameraClip& Out, FString& Error, const FString& DirectClip)
{
    FString Path = Behavior;
    Path.ReplaceInline(TEXT("\\"), TEXT("/"));
    if (!Path.StartsWith(TEXT("meshes/"), ESearchCase::IgnoreCase))
        Path.InsertAt(0, TEXT("meshes/"));
    const int32 BehaviorDir = Path.Find(TEXT("/behaviors/"), ESearchCase::IgnoreCase);
    if (BehaviorDir == INDEX_NONE)
    {
        Error = FString::Printf(TEXT("No first-person HKX graph for '%s'"), *Behavior);
        return false;
    }
    FString ActorRoot = Path.Left(BehaviorDir + 1);
    if (!ActorRoot.EndsWith(TEXT("/_1stperson/"), ESearchCase::IgnoreCase))
        ActorRoot += TEXT("_1stperson/");
    const FString GraphPath = ActorRoot + Path.Mid(BehaviorDir + 1);
    TArray<uint8> Rig, Animation;
    FT5HkxCameraClip Candidate;
    if (!DirectClip.IsEmpty())
        Candidate.Playback.Path = ActorRoot + TEXT("animations/") + FPaths::GetCleanFilename(DirectClip);
    else if (!ResolveT5HkxGraph(GraphPath, Event, Read, Candidate.Playback, Error))
    {
        // Some paired player actions have no entry event in the first-person
        // root graph. Its camera clip shares the selected body clip's filename.
        FT5HkxPlayback Body;
        if (!ResolveT5HkxGraph(Path, Event, Read, Body, Error)) return false;
        Candidate.Playback = Body;
        Candidate.Playback.Path = ActorRoot + TEXT("animations/") + FPaths::GetCleanFilename(Body.Path.Replace(TEXT("\\"), TEXT("/")));
    }
    Candidate.Playback.Path.ReplaceInline(TEXT("\\"), TEXT("/"));
    if (Candidate.Playback.Path.StartsWith(TEXT("actors/"), ESearchCase::IgnoreCase))
        Candidate.Playback.Path.InsertAt(0, TEXT("meshes/"));
    else if (!Candidate.Playback.Path.StartsWith(TEXT("meshes/"), ESearchCase::IgnoreCase))
        Candidate.Playback.Path.InsertAt(0, ActorRoot);
    const FString RigPath = ActorRoot + TEXT("characterassets/skeletonfirst.hkx");
    if (!Read(RigPath, Rig) || !Read(Candidate.Playback.Path, Animation))
    {
        Error = FString::Printf(TEXT("Missing first-person HKX skeleton '%s' or clip '%s'"), *RigPath, *Candidate.Playback.Path);
        return false;
    }
    if (!ReadT5HkxClip(Rig, Animation, Candidate.Clip, Error))
        return false;
    Candidate.Camera = Candidate.Clip.Bones.IndexOfByPredicate([](const FT5HkxBone& Bone) {
        return Bone.Name == TEXT("Camera1st [Cam1]");
    });
    const auto& Play = Candidate.Playback;
    if (Candidate.Camera == INDEX_NONE || Candidate.Clip.BoneToTrack[Candidate.Camera] == INDEX_NONE
        || !FMath::IsFinite(Play.Speed) || Play.Speed <= 0 || !FMath::IsFinite(Play.StartTime)
        || !FMath::IsFinite(Play.CropStart) || !FMath::IsFinite(Play.CropEnd)
        || FMath::Max(0.0f, Play.CropStart) + FMath::Max(0.0f, Play.CropEnd) >= Candidate.Clip.Duration)
    {
        Error = FString::Printf(TEXT("No playable first-person camera track in '%s'"), *Play.Path);
        return false;
    }
    BindT5HkxEvents(Candidate.Clip, Play);
    Candidate.Fired.Init(false, Candidate.Clip.Annotations.Num());
    Out = MoveTemp(Candidate);
    return true;
}

void DispatchT5HkxEvents(FT5HkxPlaybackClock& Camera, float Elapsed, TFunctionRef<void(const FString&)> Dispatch)
{
    const auto& Play = Camera.Playback;
    const double Begin = FMath::Max(0.f, Play.CropStart);
    const double Length = Camera.Clip.Duration - FMath::Max(0.f, Play.CropEnd) - Begin;
    const double Start = FMath::Clamp(static_cast<double>(Play.StartTime), 0., Length);
    const double Travel = Start + FMath::Max(0.f, Elapsed) * Play.Speed;
    const double Previous = Camera.LastTravel;
    if (Camera.Fired.Num() != Camera.Clip.Annotations.Num())
    {
        // Older checkpoints indexed a different list of annotations. Recover
        // once-only events from the saved playback cursor before dispatching.
        Camera.Fired.Init(false, Camera.Clip.Annotations.Num());
        for (int32 Index = 0; Index < Camera.Clip.Annotations.Num(); ++Index)
        {
            const auto& Annotation = Camera.Clip.Annotations[Index];
            const double Time = Annotation.Time - Begin;
            if (!Annotation.bOnce || Time < 0 || Time > Length) continue;
            const bool bFirstPass = Time >= Start && Time <= Previous;
            const bool bLaterPass = Play.bLoop && Length + Time <= Previous;
            Camera.Fired[Index] = bFirstPass || bLaterPass;
        }
    }
    Camera.LastTravel = Travel;
    const auto Emit = [&](double From, double To) {
        for (int32 Index = 0; Index < Camera.Clip.Annotations.Num(); ++Index)
        {
            const auto& Annotation = Camera.Clip.Annotations[Index];
            const double Time = Annotation.Time - Begin;
            if (Time > From && Time <= To && (!Annotation.bOnce || !Camera.Fired[Index]))
            {
                Camera.Fired[Index] = true;
                Dispatch(Annotation.Event);
            }
        }
    };
    const double From = Previous < 0 ? std::nextafter(Start, -1.) : Previous;
    if (!Play.bLoop)
        Emit(From, FMath::Min(Travel, Length));
    else if (FMath::FloorToDouble(From / Length) == FMath::FloorToDouble(Travel / Length) || Previous < 0)
        Emit(Previous < 0 ? From : FMath::Fmod(From, Length), FMath::Fmod(Travel, Length));
    else
    {
        Emit(FMath::Fmod(From, Length), Length);
        Emit(-1., FMath::Fmod(Travel, Length));
    }
}

FTransform SampleT5HkxPlayerCamera(const FT5HkxCameraClip& Camera, float Elapsed)
{
    const auto& Clip = Camera.Clip;
    const auto& Play = Camera.Playback;
    if (!Clip.Bones.IsValidIndex(Camera.Camera))
        return FTransform::Identity;
    const float Begin = FMath::Max(0.0f, Play.CropStart);
    const float End = Clip.Duration - FMath::Max(0.0f, Play.CropEnd);
    const float Travel = FMath::Clamp(Play.StartTime, 0.0f, End - Begin) + FMath::Max(0.0f, Elapsed) * Play.Speed;
    const float Position = Begin + (Play.bLoop ? FMath::Fmod(Travel, End - Begin) : FMath::Min(Travel, End - Begin));
    const float Frame = (Clip.Frames - 1) * FMath::Clamp(Position / Clip.Duration, 0.0f, 1.0f);
    const int32 First = FMath::FloorToInt(Frame);
    const int32 Second = FMath::Min(First + 1, Clip.Frames - 1);
    FTransform Model = FTransform::Identity;
    for (int32 Bone = Camera.Camera; Bone != INDEX_NONE; Bone = Clip.Bones[Bone].Parent)
    {
        FTransform Local = Clip.Bones[Bone].Rest;
        if (const int32 Track = Clip.BoneToTrack[Bone]; Track != INDEX_NONE)
            Local.Blend(Clip.Samples[First * Clip.Tracks + Track], Clip.Samples[Second * Clip.Tracks + Track], Frame - First);
        Model = Model * Local;
    }
    return Model;
}

void DispatchT5HkxCameraEvents(FT5HkxCameraClip& Camera, float Elapsed, TFunctionRef<void(const FString&)> Dispatch)
{
    DispatchT5HkxEvents(Camera, Elapsed, Dispatch);
}

float T5HkxPosition(const FT5HkxPlaybackClock& Clock, float Elapsed)
{
    const auto& Play = Clock.Playback;
    const float Begin = FMath::Max(0.f, Play.CropStart);
    const float Length = Clock.Clip.Duration - FMath::Max(0.f, Play.CropEnd) - Begin;
    const float Travel = FMath::Clamp(Play.StartTime, 0.f, Length) + FMath::Max(0.f, Elapsed) * Play.Speed;
    return Begin + (Play.bLoop ? FMath::Fmod(Travel, Length) : FMath::Min(Travel, Length));
}

bool T5HkxFinished(const FT5HkxPlaybackClock& Clock, float Elapsed)
{
    return !Clock.Playback.bLoop && T5HkxPosition(Clock, Elapsed)
        >= Clock.Clip.Duration - FMath::Max(0.f, Clock.Playback.CropEnd);
}

FTransform T5HkxMotion(const FT5HkxPlaybackClock& Clock, float Elapsed)
{
    if (!Clock.Clip.Motion) return FTransform::Identity;
    const auto& Play = Clock.Playback;
    const float Begin = FMath::Max(0.f, Play.CropStart);
    const auto Initial = Clock.Clip.Motion->Sample(Begin);
    FTransform Motion = Clock.Clip.Motion->Sample(T5HkxPosition(Clock, Elapsed)).GetRelativeTransform(Initial);
    if (Play.bLoop)
    {
        const float Length = Clock.Clip.Duration - FMath::Max(0.f, Play.CropEnd) - Begin;
        int64 Cycles = FMath::FloorToInt64((FMath::Clamp(Play.StartTime, 0.f, Length)
            + FMath::Max(0.f, Elapsed) * Play.Speed) / Length);
        FTransform Cycle = Clock.Clip.Motion->Sample(Begin + Length).GetRelativeTransform(Initial);
        // Exponentiation keeps long-running looping clips bounded in work.
        while (Cycles > 0)
        {
            if (Cycles & 1) Motion = Motion * Cycle;
            Cycle = Cycle * Cycle;
            Cycles >>= 1;
        }
    }
    return Motion;
}
