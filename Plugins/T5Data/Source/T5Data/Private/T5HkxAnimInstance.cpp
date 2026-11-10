#include "T5HkxAnimInstance.h"
#include "T5MotionData.h"

#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimNodeBase.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogT5Hkx, Log, All);

class FT5HkxAnimProxy final : public FAnimInstanceProxy
{
    TArray<FTransform> FixedPose;
public:
    explicit FT5HkxAnimProxy(UAnimInstance* Instance) : FAnimInstanceProxy(Instance) {}

    void SetClip(const UT5HkxAnimInstance& Instance)
    {
        Clip = Instance.Clip;
        MeshToSource = Instance.MeshToSource;
        BindFromHkx = Instance.BindFromHkx;
        Position = Instance.Position;
        HeadIndex = Instance.HeadIndex;
        HeadForward = Instance.HeadForward;
        Layer = Instance.Layer;
        LayerPlayback = Instance.LayerPlayback;
        LayerPosition = Instance.LayerPosition;
    }

    void PreUpdate(UAnimInstance* Instance, float DeltaSeconds) override
    {
        FAnimInstanceProxy::PreUpdate(Instance, DeltaSeconds);
        const auto& Source = static_cast<const UT5HkxAnimInstance&>(*Instance);
        FixedPose = Source.FixedPose;
        Position = Source.Position;
        Layer = Source.Layer;
        LayerPlayback = Source.LayerPlayback;
        LayerPosition = Source.LayerPosition;
        LookTarget = Source.LookTarget;
        LookBlend = FMath::FInterpTo(LookBlend, LookTarget.IsSet() ? 1.f : 0.f, DeltaSeconds, 6.f);
        if (LookTarget) LastLookTarget = *LookTarget;
    }

    bool Evaluate(FPoseContext& Output) override
    {
        if (!FixedPose.IsEmpty())
        {
            Output.ResetToRefPose();
            const auto& Bones = Output.Pose.GetBoneContainer();
            for (const auto Bone : Output.Pose.ForEachBoneIndex())
            {
                const auto Index = Bones.MakeMeshPoseIndex(Bone).GetInt();
                if (FixedPose.IsValidIndex(Index)) Output.Pose[Bone] = FixedPose[Index];
            }
            return true;
        }
        if (!Clip)
            return false;
        Output.ResetToRefPose();
        const float Frame = (Clip->Frames - 1) * FMath::Clamp(Position / Clip->Duration, 0.0f, 1.0f);
        const int32 First = FMath::FloorToInt(Frame);
        const int32 Second = FMath::Min(First + 1, Clip->Frames - 1);
        const float Alpha = Frame - First;
        TArray<FTransform, TInlineAllocator<128>> Source;
        Source.SetNumUninitialized(Clip->Bones.Num());
        for (int32 Bone = 0; Bone < Clip->Bones.Num(); ++Bone)
        {
            const auto& Entry = Clip->Bones[Bone];
            FTransform Local = Entry.Rest;
            if (const int32 Track = Clip->BoneToTrack[Bone]; Track != INDEX_NONE)
                Local.Blend(Clip->Samples[First * Clip->Tracks + Track],
                    Clip->Samples[Second * Clip->Tracks + Track], Alpha);
            if (Layer && LayerPlayback.BoneWeights.IsValidIndex(Bone) && LayerPlayback.BoneWeights[Bone] > 0)
            {
                FTransform Overlay = Layer->Bones[Bone].Rest;
                if (const int32 Track = Layer->BoneToTrack[Bone]; Track != INDEX_NONE)
                {
                    const float At = (Layer->Frames - 1) * FMath::Clamp(LayerPosition / Layer->Duration, 0.f, 1.f);
                    const int32 A = FMath::FloorToInt(At), B = FMath::Min(A + 1, Layer->Frames - 1);
                    Overlay.Blend(Layer->Samples[A * Layer->Tracks + Track], Layer->Samples[B * Layer->Tracks + Track], At - A);
                }
                Local.Blend(Local, Overlay, LayerPlayback.BoneWeights[Bone]);
            }
            Source[Bone] = Entry.Parent != INDEX_NONE ? Local * Source[Entry.Parent] : Local;
        }
        TArray<FTransform, TInlineAllocator<128>> Target, Original;
        Original.SetNumUninitialized(Output.Pose.GetNumBones());
        Target.SetNumUninitialized(Output.Pose.GetNumBones());
        const auto& Container = Output.Pose.GetBoneContainer();
        for (const FCompactPoseBoneIndex Bone : Output.Pose.ForEachBoneIndex())
        {
            const int32 MeshIndex = Container.MakeMeshPoseIndex(Bone).GetInt();
            const int32 Parent = Output.Pose.GetParentBoneIndex(Bone).GetInt();
            const int32 SourceIndex = MeshToSource.IsValidIndex(MeshIndex) ? MeshToSource[MeshIndex] : INDEX_NONE;
            auto& Unmodified = Original[Bone.GetInt()];
            Unmodified = SourceIndex != INDEX_NONE ? BindFromHkx[MeshIndex] * Source[SourceIndex]
                : Parent != INDEX_NONE ? Output.Pose[Bone] * Original[Parent] : Output.Pose[Bone];
            const FTransform Local = Parent != INDEX_NONE ? Unmodified.GetRelativeTransform(Original[Parent]) : Unmodified;
            auto& Model = Target[Bone.GetInt()];
            Model = Parent != INDEX_NONE ? Local * Target[Parent] : Local;
            if (MeshIndex == HeadIndex && LookBlend > KINDA_SMALL_NUMBER)
            {
                const FVector Direction = (LastLookTarget - Model.GetLocation()).GetSafeNormal();
                const FVector Forward = Model.TransformVectorNoScale(HeadForward).GetSafeNormal();
                if (!Direction.IsNearlyZero())
                {
                    FQuat Turn = FQuat::FindBetweenNormals(Forward, Direction);
                    // Anatomical limit, independent of the quest. Never turn a head backwards.
                    const float Angle = Turn.GetAngle();
                    const float Limit = FMath::DegreesToRadians(70.f);
                    Turn = FQuat::Slerp(FQuat::Identity, Turn, LookBlend * FMath::Min(1.f, Limit / FMath::Max(Angle, .001f)));
                    Model.SetRotation((Turn * Model.GetRotation()).GetNormalized());
                }
            }
            Output.Pose[Bone] = Parent != INDEX_NONE ? Model.GetRelativeTransform(Target[Parent]) : Model;
        }
        return true;
    }

private:
    TSharedPtr<const FT5HkxClip> Clip;
    TSharedPtr<const FT5HkxClip> Layer;
    FT5HkxPlayback LayerPlayback;
    float LayerPosition = 0;
    TArray<int32> MeshToSource;
    TArray<FTransform> BindFromHkx;
    TOptional<FVector> LookTarget;
    FVector LastLookTarget = FVector::ZeroVector, HeadForward = FVector::ForwardVector;
    float LookBlend = 0;
    int32 HeadIndex = INDEX_NONE;
    float Position = 0;
};

FAnimInstanceProxy* UT5HkxAnimInstance::CreateAnimInstanceProxy() { return new FT5HkxAnimProxy(this); }

void UT5HkxAnimInstance::DestroyAnimInstanceProxy(FAnimInstanceProxy* Proxy)
{
    delete static_cast<FT5HkxAnimProxy*>(Proxy);
}

bool UT5HkxAnimInstance::Start(TSharedPtr<const FT5HkxClip> InClip, const FT5HkxPlayback& Playback,
    TFunction<void(const FString&)> InAnnotation)
{
    auto* Mesh = GetSkelMeshComponent()->GetSkeletalMeshAsset();
    if (!Mesh || !InClip || !FMath::IsFinite(Playback.Speed) || Playback.Speed <= 0
        || !FMath::IsFinite(Playback.CropStart) || !FMath::IsFinite(Playback.CropEnd)
        || !FMath::IsFinite(Playback.StartTime))
        return false;
    const float First = FMath::Max(Playback.CropStart, 0.0f);
    const float Last = InClip->Duration - FMath::Max(Playback.CropEnd, 0.0f);
    if (Last - First <= UE_SMALL_NUMBER)
        return false;

    TMap<FName, int32> SourceNames;
    TArray<FTransform> SourceRest;
    SourceRest.SetNumUninitialized(InClip->Bones.Num());
    for (int32 I = 0; I < InClip->Bones.Num(); ++I)
    {
        const auto& Bone = InClip->Bones[I];
        SourceNames.Add(Bone.Name, I);
        SourceRest[I] = Bone.Parent == INDEX_NONE ? Bone.Rest : Bone.Rest * SourceRest[Bone.Parent];
    }
    const auto& Reference = Mesh->GetRefSkeleton();
    const auto& TargetPose = Reference.GetRefBonePose();
    TArray<FTransform> TargetRest, NewBind;
    TArray<int32> NewMap;
    TargetRest.SetNumUninitialized(TargetPose.Num());
    NewBind.SetNum(TargetPose.Num());
    NewMap.Init(INDEX_NONE, TargetPose.Num());
    int32 Mapped = 0;
    for (int32 I = 0; I < TargetPose.Num(); ++I)
    {
        const int32 Parent = Reference.GetParentIndex(I);
        TargetRest[I] = Parent == INDEX_NONE ? TargetPose[I] : TargetPose[I] * TargetRest[Parent];
        if (const int32* SourceIndex = SourceNames.Find(Reference.GetBoneName(I)))
        {
            NewMap[I] = *SourceIndex;
            NewBind[I] = TargetRest[I].GetRelativeTransform(SourceRest[*SourceIndex]);
            ++Mapped;
        }
    }
    if (!Mapped)
        return false;

    HeadIndex = Reference.FindBoneIndex(TEXT("NPC Head [Head]"));
    if (HeadIndex != INDEX_NONE)
        HeadForward = TargetRest[HeadIndex].InverseTransformVectorNoScale(FVector::ForwardVector);
    Clip = MoveTemp(InClip);
    if (Layer)
    {
        bool bCompatible=Layer->Bones.Num()==Clip->Bones.Num();
        for (int32 I=0; bCompatible && I<Clip->Bones.Num(); ++I)
            bCompatible=Layer->Bones[I].Name==Clip->Bones[I].Name && Layer->Bones[I].Parent==Clip->Bones[I].Parent;
        if (!bCompatible) Layer.Reset();
    }
    MeshToSource = MoveTemp(NewMap);
    BindFromHkx = MoveTemp(NewBind);
    Begin = First;
    End = Last;
    Speed = Playback.Speed;
    Position = FMath::Clamp(Begin + Playback.StartTime, Begin, End);
    bLoop = Playback.bLoop;
    bFirstUpdate = true;
    bExternalClock = false;
    OnAnnotation = MoveTemp(InAnnotation);
    FiredAnnotations.Init(false, Clip->Annotations.Num());
    GetProxyOnGameThread<FT5HkxAnimProxy>().SetClip(*this);
    UE_LOG(LogT5Hkx, Display, TEXT("T5_HKX_PLAY %s frames=%d duration=%.3f bones=%d mapped=%d loop=%d"),
        *Playback.Path, Clip->Frames, Clip->Duration, Clip->Bones.Num(), Mapped, bLoop);
    return true;
}

bool UT5HkxAnimInstance::StartLayer(TSharedPtr<const FT5HkxClip> InClip, const FT5HkxPlayback& Playback)
{
    if (!InClip || Playback.BoneWeights.Num() != InClip->Bones.Num()) return false;
    const float BeginLayer=FMath::Max(0.f,Playback.CropStart),EndLayer=InClip->Duration-FMath::Max(0.f,Playback.CropEnd);
    if (!FMath::IsFinite(Playback.Speed) || Playback.Speed<=0 || !FMath::IsFinite(BeginLayer)
        || !FMath::IsFinite(EndLayer) || EndLayer-BeginLayer<=UE_SMALL_NUMBER || !FMath::IsFinite(Playback.StartTime)) return false;
    if (!Clip)
    {
        auto Rest = MakeShared<FT5HkxClip>(*InClip);
        Rest->Frames = 1; Rest->Tracks = 0; Rest->Duration = 1;
        Rest->Samples.Reset(); Rest->Annotations.Reset(); Rest->Motion.Reset();
        Rest->BoneToTrack.Init(INDEX_NONE, Rest->Bones.Num());
        FT5HkxPlayback Idle; Idle.bLoop = true;
        if (!Start(Rest, Idle, {})) return false;
    }
    if (Clip->Bones.Num() != InClip->Bones.Num()) return false;
    for (int32 I = 0; I < Clip->Bones.Num(); ++I)
        if (Clip->Bones[I].Name != InClip->Bones[I].Name || Clip->Bones[I].Parent != InClip->Bones[I].Parent) return false;
    Layer = MoveTemp(InClip);
    LayerPlayback = Playback;
    LayerPosition = FMath::Clamp(BeginLayer+Playback.StartTime,BeginLayer,EndLayer);
    bLayerExternalClock = false;
    UE_LOG(LogT5Hkx,Display,TEXT("T5_HKX_LAYER %s bones=%d weighted=%d loop=%d"),*Playback.Path,Layer->Bones.Num(),
        Playback.BoneWeights.FilterByPredicate([](float Weight){return Weight>0;}).Num(),Playback.bLoop);
    return true;
}

void UT5HkxAnimInstance::PreUpdateAnimation(float DeltaSeconds)
{
    if (Layer && !bLayerExternalClock)
    {
        const float First = FMath::Max(0.f, LayerPlayback.CropStart), Last = Layer->Duration - FMath::Max(0.f, LayerPlayback.CropEnd);
        LayerPosition += FMath::Max(0.f, DeltaSeconds) * LayerPlayback.Speed;
        LayerPosition = LayerPlayback.bLoop ? First + FMath::Fmod(LayerPosition - First, Last - First) : FMath::Min(LayerPosition, Last);
    }
    if (Clip && !bExternalClock)
    {
        const float Previous = Position;
        const float Range = End - Begin;
        const float Travel = FMath::Max(0.0f, DeltaSeconds) * Speed;
        const bool bWrap = bLoop && Travel >= End - Previous;
        Position = bWrap ? Begin + FMath::Fmod(Travel - (End - Previous), Range)
                         : FMath::Min(Previous + Travel, End);
        EmitAnnotations(Previous, bWrap ? End : Position, bFirstUpdate);
        if (bWrap)
            EmitAnnotations(Begin, Position, true);
        bFirstUpdate = false;
    }
    Super::PreUpdateAnimation(DeltaSeconds);
}

void UT5HkxAnimInstance::EmitAnnotations(float From, float To, bool bIncludeStart)
{
    if (!OnAnnotation)
        return;
    for (int32 I = 0; I < Clip->Annotations.Num(); ++I)
    {
        const auto& Annotation = Clip->Annotations[I];
        if ((!Annotation.bOnce || !FiredAnnotations[I]) && Annotation.Time <= To
            && (Annotation.Time > From || (bIncludeStart && Annotation.Time == From)))
        {
            FiredAnnotations[I] = true;
            OnAnnotation(Annotation.Event);
        }
    }
}

namespace
{
    FString MeshPath(FString Value)
    {
        Value.ReplaceInline(TEXT("\\"), TEXT("/"));
        if (!Value.StartsWith(TEXT("meshes/"), ESearchCase::IgnoreCase))
            Value.InsertAt(0, TEXT("meshes/"));
        return Value;
    }
}

bool LoadT5HkxAnimation(const FString& Behavior, const FString& Event, const FString& DirectClip,
    TFunctionRef<bool(const FString&, TArray<uint8>&)> Read, FString& Error, const FString& SkeletonPath,
    float DirectSpeed, bool bNotifyEnd, bool bLoop, FT5HkxPlaybackClock& Out,
    const FT5MotionDatabase* MotionData, const FString& Project)
{
    const FString BehaviorPath = Behavior.IsEmpty() ? FString{} : MeshPath(Behavior);
    const int32 BehaviorDir = BehaviorPath.Find(TEXT("/behaviors/"), ESearchCase::IgnoreCase);
    FT5HkxPlayback Playback;
    TArray<uint8> Graph;
    if (!DirectClip.IsEmpty())
    {
        Playback.Path = MeshPath(DirectClip);
        Playback.Speed = DirectSpeed;
        Playback.bLoop = bLoop;
        if (!BehaviorPath.IsEmpty() && (Event == TEXT("T5Execution") || Event == TEXT("T5PairedExecution"))
            && Read(BehaviorPath, Graph))
            ReadT5HkxClipEvents(Graph, FPaths::GetBaseFilename(Playback.Path), Playback);
    }
    else if (BehaviorDir == INDEX_NONE || !Read(BehaviorPath, Graph))
    {
        Error = FString::Printf(TEXT("Missing HKX behavior '%s'"), *BehaviorPath);
        return false;
    }
    else
    {
        if (!ResolveT5HkxGraph(BehaviorPath, Event, Read, Playback, Error))
            return false;
        if (Playback.bReturnToLocomotion || Playback.bClearLayer)
        { Out={}; Out.Playback=MoveTemp(Playback); Error.Reset(); return true; }
        Playback.Path.ReplaceInline(TEXT("\\"), TEXT("/"));
        if (Playback.Path.StartsWith(TEXT("actors/"), ESearchCase::IgnoreCase)
            || Playback.Path.StartsWith(TEXT("meshes/"), ESearchCase::IgnoreCase))
            Playback.Path = MeshPath(Playback.Path);
        else
            Playback.Path = BehaviorPath.Left(BehaviorDir + 1) + Playback.Path;
    }
    FString RigPath = SkeletonPath;
    if (RigPath.IsEmpty())
    {
        const int32 AnimationsDir = Playback.Path.Find(TEXT("/animations/"), ESearchCase::IgnoreCase);
        if (AnimationsDir == INDEX_NONE)
        {
            Error = FString::Printf(TEXT("No skeleton path for HKX clip '%s'"), *Playback.Path);
            return false;
        }
        RigPath = Playback.Path.Left(AnimationsDir + 1) + TEXT("character assets/skeleton.hkx");
    }
    TArray<uint8> Rig, Animation;
    if (!Read(MeshPath(RigPath), Rig) || !Read(Playback.Path, Animation))
    {
        Error = FString::Printf(TEXT("Missing HKX skeleton '%s' or clip '%s'"), *RigPath, *Playback.Path);
        return false;
    }
    auto Clip = MakeShared<FT5HkxClip>();
    if (!ReadT5HkxClip(Rig, Animation, *Clip, Error))
        return false;
    if (!FMath::IsFinite(Playback.Speed) || Playback.Speed <= 0
        || !FMath::IsFinite(Playback.StartTime) || !FMath::IsFinite(Playback.CropStart)
        || !FMath::IsFinite(Playback.CropEnd)
        || FMath::Max(0.f, Playback.CropStart) + FMath::Max(0.f, Playback.CropEnd) >= Clip->Duration)
    {
        Error = FString::Printf(TEXT("Invalid playback timing for HKX clip '%s'"), *Playback.Path);
        return false;
    }
    BindT5HkxEvents(*Clip, Playback);
    if (bNotifyEnd && !Playback.bLoop)
        Clip->Annotations.Add({Clip->Duration - FMath::Max(0.f, Playback.CropEnd), TEXT("T5ClipEnd")});
    if (!Playback.BoneWeights.IsEmpty())
    {
        // Vanilla and XP32 use the same authored archive masks. Resolve by name
        // whenever the source rig is available, including equal-size replacements
        // with a different bone order. Extension bones retain their base pose.
        TArray<uint8> ReferenceRig;
        const bool bHasReference = Read(TEXT("@base-animation/")+MeshPath(RigPath),ReferenceRig);
        if ((bHasReference && !RemapT5HkxBoneWeights(ReferenceRig,Clip->Bones,Playback.BoneWeights,Error))
            || (!bHasReference && Playback.BoneWeights.Num() != Clip->Bones.Num()))
        {
            if (Error.IsEmpty()) Error=FString::Printf(TEXT("No source rig for %d mask weights and %d bones: %s"),
                Playback.BoneWeights.Num(),Clip->Bones.Num(),*RigPath);
            return false;
        }
    }
    // The FlightGrab procedure positions the entry against its target. The
    // exit is animation-driven, including when the graph carries a bone mask.
    const bool bPackageAlignedEntry = Event.EndsWith(TEXT("::to_Flight_Kill_Grab_Enter"));
    if (MotionData && !bPackageAlignedEntry && (Playback.BoneWeights.IsEmpty() || Event.Contains(TEXT("::")))
        && !Playback.GeneratorName.IsEmpty())
    {
        Clip->Motion = MotionData->Find(Project, RigPath, Playback.GeneratorName, Error);
        if (!Error.IsEmpty()) return false;
        if (Clip->Motion && !FMath::IsNearlyEqual(Clip->Motion->Duration, Clip->Duration, .002f))
        {
            Error = FString::Printf(TEXT("Movement/HKX duration mismatch for '%s': %.4f / %.4f"),
                *Playback.GeneratorName, Clip->Motion->Duration, Clip->Duration);
            return false;
        }
    }
    Out.Clip = MoveTemp(*Clip);
    Out.Playback = MoveTemp(Playback);
    Out.Fired.Init(false, Out.Clip.Annotations.Num());
    return true;
}

bool PlayT5Hkx(USkeletalMeshComponent& Mesh, const FString& Behavior, const FString& Event, const FString& DirectClip,
    TFunctionRef<bool(const FString&, TArray<uint8>&)> Read, FString& Error, const FString& SkeletonPath,
    TFunction<void(const FString&)> OnAnnotation, float DirectSpeed, bool bNotifyEnd, bool bLoop, TOptional<float> ResumeAt)
{
    FT5HkxPlaybackClock Loaded;
    if (!LoadT5HkxAnimation(Behavior, Event, DirectClip, Read, Error, SkeletonPath, DirectSpeed, bNotifyEnd, bLoop, Loaded))
        return false;
    if (Loaded.Playback.bClearLayer)
    {
        if (auto* Instance = Cast<UT5HkxAnimInstance>(Mesh.GetAnimInstance())) Instance->ClearLayer();
        Error.Reset(); return true;
    }
    if (Loaded.Playback.bReturnToLocomotion)
    { Error=TEXT("Root graph reset requires the actor locomotion host"); return false; }
    auto Clip = MakeShared<FT5HkxClip>(MoveTemp(Loaded.Clip));
    auto Playback = MoveTemp(Loaded.Playback);
    if (ResumeAt)
        Playback.StartTime = FMath::Max(0.f, *ResumeAt - Playback.CropStart);

    if (!Cast<UT5HkxAnimInstance>(Mesh.GetAnimInstance()))
        Mesh.SetAnimInstanceClass(UT5HkxAnimInstance::StaticClass());
    auto* Instance = Cast<UT5HkxAnimInstance>(Mesh.GetAnimInstance());
    if (!Instance || !(Playback.BoneWeights.IsEmpty()
        ? Instance->Start(MoveTemp(Clip), Playback, MoveTemp(OnAnnotation))
        : Instance->StartLayer(MoveTemp(Clip), Playback)))
    {
        Error = FString::Printf(TEXT("No compatible UE pose for HKX clip '%s'"), *Playback.Path);
        return false;
    }
    Mesh.VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
    return true;
}
