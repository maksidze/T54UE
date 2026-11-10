#include "T5Scene.h"
#include "T5Bridge.h"
#include "T5HkxAnimInstance.h"
#include "T5Mechanics.h"

#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Rendering/SkeletalMeshRenderData.h"

namespace
{
    TArray<int32> HeadSections(const USkeletalMesh& Asset, FName Bone)
    {
        const auto& Skeleton = Asset.GetRefSkeleton();
        const auto Index = Skeleton.FindBoneIndex(Bone);
        const auto* Render = Asset.GetResourceForRendering();
        if (Index == INDEX_NONE || !Render || Render->LODRenderData.IsEmpty()) return {};
        TBitArray<> Head(false, Skeleton.GetNum());
        for (int32 I = Index; I < Head.Num(); ++I)
        {
            const int32 Parent = Skeleton.GetParentIndex(I);
            Head[I] = I == Index || (Parent != INDEX_NONE && Head[Parent]);
        }
        const auto& LOD = Render->LODRenderData[0];
        TArray<int32> Sections;
        for (int32 I = 0; I < LOD.RenderSections.Num(); ++I)
        {
            const auto& Section = LOD.RenderSections[I];
            if (!Section.BoneMap.ContainsByPredicate([&](int32 Joint) { return Head.IsValidIndex(Joint) && Head[Joint]; }))
                continue;
            uint64 Weight = 0;
            for (uint32 Vertex = Section.BaseVertexIndex; Vertex < Section.BaseVertexIndex + Section.NumVertices; ++Vertex)
                for (int32 Influence = 0; Influence < Section.MaxBoneInfluences; ++Influence)
                {
                    const auto Joint = LOD.SkinWeightVertexBuffer.GetBoneIndex(Vertex, Influence);
                    if (Section.BoneMap.IsValidIndex(Joint) && Head[Section.BoneMap[Joint]])
                        Weight += LOD.SkinWeightVertexBuffer.GetBoneWeight(Vertex, Influence);
                }
            // FaceGen includes some neck-seam weights outside the head branch.
            if (2 * Weight > uint64(Section.NumVertices) * MAX_uint16) Sections.Add(I);
        }
        return Sections;
    }
}

void AT5Scene::AnimationVisualEvent(uint32 Id, const FString& Event)
{
    FString Action, Name;
    if (Event.Split(TEXT("."), &Action, &Name) && Action.Equals(TEXT("AnimObjDraw"), ESearchCase::IgnoreCase))
    {
        DrawnAnimationObjects.FindOrAdd(Id).Add(MoveTemp(Name));
        return;
    }
    if (const auto* Props = AnimationProps.Find(Id))
        if (auto* Drawn = DrawnAnimationObjects.Find(Id))
            for (const auto& [EditorId, Prop] : *Props)
                if (Event.Equals(Prop.Unload, ESearchCase::IgnoreCase) && !Prop.Unload.IsEmpty())
                    Drawn->Remove(EditorId);
}

void AT5Scene::TickAnimationVisuals()
{
    if (!LoadedScene || !Mechanics) return;
    for (const auto& [Id, Names] : DrawnAnimationObjects)
    {
        auto* Rig = FindReferenceSkeleton(Id);
        if (!Rig || !Rig->IsVisible()) continue;
        auto& Props = AnimationProps.FindOrAdd(Id);
        for (const auto& Name : Names)
        {
            auto& Prop = Props.FindOrAdd(Name);
            // The visible rig can draw its prop while the next cell streams.
            if (!Prop.bAttempted)
            {
                Prop.bAttempted = true;
                if (const auto Data = Papyrus->AnimationObject(Name)) Prop = BuildAnimationProp(*Data);
            }
            if (auto* Part = Prop.Component.Get(); Part && Rig->DoesSocketExist(Prop.Socket))
            {
                if (Part->GetAttachParent() != Rig)
                {
                    Part->AttachToComponent(Rig, FAttachmentTransformRules::SnapToTargetIncludingScale, Prop.Socket);
                    UE_LOG(LogTemp, Display, TEXT("T5_ANIMATION_PROP ref=%08X object=%s socket=%s"), Id, *Name, *Prop.Socket.ToString());
                }
                Part->SetVisibility(true);
            }
        }
    }
    for (auto& [Id, Props] : AnimationProps)
    {
        const auto* Drawn = DrawnAnimationObjects.Find(Id);
        const auto* Rig = FindReferenceSkeleton(Id);
        for (auto& [Name, Prop] : Props)
            if (auto* Part = Prop.Component.Get())
                if (!Rig || !Rig->IsVisible() || !Drawn || !Drawn->Contains(Name)) Part->SetVisibility(false);
    }

    for (const auto& [Id, Visuals] : ReferenceVisuals)
    {
        const FName Bone = Papyrus->SeveredHead(Id);
        if (Bone.IsNone()) continue;
        auto* Mesh = FindReferenceSkeleton(Id);
        if (!Mesh || !Mesh->IsVisible() || HeadlessMeshes.FindRef(Id).Get() == Mesh) continue;
        auto* Asset = Mesh->GetSkeletalMeshAsset();
        if (!Asset) continue;
        const auto Parts = HeadSections(*Asset, Bone);
        if (Parts.IsEmpty()) continue;
        const auto& Sections = Asset->GetResourceForRendering()->LODRenderData[0].RenderSections;
        if (!DetachedHeads.FindRef(Id).IsValid())
        {
            const auto& Model = Mesh->GetComponentSpaceTransforms();
            const auto& Skeleton = Asset->GetRefSkeleton();
            if (Model.Num() != Skeleton.GetNum()) continue;
            const auto HeadPose = Mesh->GetSocketTransform(Bone);
            FActorSpawnParameters Spawn;
            Spawn.ObjectFlags = RF_Transient;
            auto* Actor = GetWorld()->SpawnActor<AActor>(HeadPose.GetLocation(), HeadPose.Rotator(), Spawn);
            if (!Actor) continue;
            const auto* Saved = DetachedHeadTransforms.Find(Id);
            auto* Body = Mechanics->DetachBodyPart(Id, Bone, *Actor, Saved ? *Saved : HeadPose);
            if (!Body) { Actor->Destroy(); continue; }
            auto* Head = NewObject<USkeletalMeshComponent>(Actor, NAME_None, RF_Transient);
            Head->SetupAttachment(Body);
            Head->SetRelativeTransform(Mesh->GetComponentTransform().GetRelativeTransform(HeadPose));
            Head->SetSkeletalMesh(Asset);
            Head->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            Head->SetAnimInstanceClass(UT5HkxAnimInstance::StaticClass());
            Head->RegisterComponent();
            if (auto* Animation = Cast<UT5HkxAnimInstance>(Head->GetAnimInstance()))
            {
                TArray<FTransform> Local;
                for (int32 I = 0; I < Model.Num(); ++I)
                {
                    const auto Parent = Skeleton.GetParentIndex(I);
                    Local.Add(Parent == INDEX_NONE ? Model[I] : Model[I].GetRelativeTransform(Model[Parent]));
                }
                Animation->SetFixedPose(MoveTemp(Local));
                Head->TickAnimation(0, false);
                Head->RefreshBoneTransforms();
            }
            for (int32 I = 0; I < Sections.Num(); ++I)
                Head->ShowMaterialSection(Sections[I].MaterialIndex, I, Parts.Contains(I), 0);
            Body->SetSimulatePhysics(true);
            if (Saved) Body->PutRigidBodyToSleep();
            DetachedHeads.Add(Id, Actor);
            UE_LOG(LogTemp, Display, TEXT("T5_HEAD_DETACHED ref=%08X bone=%s sections=%d simulating=%d"),
                Id, *Bone.ToString(), Parts.Num(), Body->IsSimulatingPhysics());
        }
        for (const int32 Index : Parts) Mesh->ShowMaterialSection(Sections[Index].MaterialIndex, Index, false, 0);
        HeadlessMeshes.Add(Id, Mesh);
    }
    for (auto& [Id, Head] : DetachedHeads)
        if (auto* Actor = Head.Get())
        {
            const auto* Mesh = FindReferenceSkeleton(Id);
            const bool bVisible = Mesh && Mesh->IsVisible();
            Actor->SetActorHiddenInGame(!bVisible);
            if (Actor->GetActorEnableCollision() != bVisible) Actor->SetActorEnableCollision(bVisible);
        }
}

void AT5Scene::ClearAnimationVisuals()
{
    for (auto& [Id, Props] : AnimationProps)
        for (auto& [Name, Prop] : Props)
            if (auto* Part = Prop.Component.Get()) Part->DestroyComponent();
    for (auto& [Id, Head] : DetachedHeads)
        if (auto* Actor = Head.Get()) Actor->Destroy();
    AnimationProps.Reset();
    DrawnAnimationObjects.Reset();
    for (auto& [Id, Damage] : DamageVisuals)
        if (auto* Component = Damage.Replacement.Component.Get()) Component->DestroyComponent();
    DamageVisuals.Reset();
    DetachedHeads.Reset();
    DetachedHeadTransforms.Reset();
    HeadlessMeshes.Reset();
}
