#include "T5Scene.h"
#include "T5ReferenceAnimation.h"
#include "T5MotionData.h"
#include "T5Bridge.h"
#include "T5Mechanics.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"

void AT5Scene::SweepActorFall(uint32 Actor, FVector Velocity, double Delta)
{
    if (!LoadedScene || Delta <= 0 || Delta > 1) return;
    FTransform Authored;
    uint32 Parent;
    const auto Pose = Papyrus->ReferenceTransform(Actor, Authored, Parent);
    const auto* Rig = FindReferenceSkeleton(Actor);
    if (!Pose || !Rig || !Rig->GetSkeletalMeshAsset()) return;
    const auto Bounds = Rig->GetSkeletalMeshAsset()->GetBounds();
    const double Radius = FMath::Max(1., FMath::Min(Bounds.BoxExtent.X, Bounds.BoxExtent.Y));
    const double HalfHeight = FMath::Max(Radius, Bounds.BoxExtent.Z);
    const auto Origin = GetActorTransform();
    const FVector Offset(0, 0, HalfHeight);
    const FVector Start = Origin.TransformPosition(Pose->GetLocation()) + Offset;
    Velocity = Origin.TransformVectorNoScale(Velocity);
    const FVector Gravity(0, 0, GetWorld()->GetGravityZ());
    const FVector End = Start + Velocity * Delta + Gravity * (.5 * Delta * Delta);
    FCollisionQueryParams Query(SCENE_QUERY_STAT(T5Fall), true);
    for (const auto& [Weak, Id] : InteractionComponents)
        if (auto* Component = Weak.Get(); Component && (Id == Actor || Component->IsA<USkeletalMeshComponent>()))
            Query.AddIgnoredComponent(Component);
    FHitResult Hit;
    const bool bHit = GetWorld()->SweepSingleByChannel(Hit, Start, End, FQuat::Identity, ECC_Visibility,
        FCollisionShape::MakeCapsule(Radius, HalfHeight), Query);
    const bool bGrounded = bHit && Hit.ImpactNormal.Z > .5 && Velocity.Z + Gravity.Z * Delta <= 0;
    Velocity += Gravity * Delta;
    if (bHit) Velocity = FVector::VectorPlaneProject(Velocity, Hit.ImpactNormal);
    Papyrus->FallResult(Actor, Origin.InverseTransformPosition((bHit ? Hit.Location : End) - Offset),
        Origin.InverseTransformVectorNoScale(Velocity), bGrounded);
}

void AT5Scene::FindFiringPosition(uint32 Actor, uint32 Target, uint32 Marker, double Range, double ProjectileRadius)
{
    if (!LoadedScene || Range <= 0) return;
    FTransform Authored;
    uint32 Parent;
    const auto Pose = Papyrus->ReferenceTransform(Actor, Authored, Parent);
    const auto* Rig = FindReferenceSkeleton(Actor);
    const auto* Enemy = FindReferenceSkeleton(Target);
    if (!Pose || !Rig || !Enemy || !Rig->GetSkeletalMeshAsset()) return;
    const auto Origin = GetActorTransform();
    const auto Start = Origin.TransformPosition(Pose->GetLocation());
    const auto Bounds = Rig->GetSkeletalMeshAsset()->GetBounds().BoxExtent;
    const double Radius = FMath::Max(1., FMath::Min(Bounds.X, Bounds.Y));
    const FVector Center(0, 0, Bounds.Z);
    const auto Aim = Enemy->Bounds.Origin;
    const FName Hand(TEXT("NPC L Hand [LHnd]"));
    const FVector Eye = (Rig->DoesSocketExist(Hand) ? Rig->GetSocketLocation(Hand) : Rig->Bounds.Origin) - Start;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(T5FiringPosition), true);
    for (const auto& [Weak, Id] : InteractionComponents)
        if (auto* Component = Weak.Get(); Component && (Id == Actor || Id == Target))
            Query.AddIgnoredComponent(Component);
    const FVector Step(0, 0, Radius);
    const auto Ground = [&](FVector Point) -> TOptional<FVector> {
        FHitResult Hit;
        if (GetWorld()->LineTraceSingleByChannel(Hit, Point + Step, Point - Step, ECC_Visibility, Query)
            && Hit.ImpactNormal.Z > .5) return Hit.ImpactPoint;
        return {};
    };
    const auto Reachable = [&](FVector Point) -> TOptional<FVector> {
        const auto Floor = Ground(Point);
        if (!Floor || FVector::DistSquared2D(*Floor, Start) < FMath::Square(Radius)
            || FVector::DistSquared(*Floor, Aim) > FMath::Square(Range) || !Ground((Start + *Floor) * .5))
            return {};
        if (GetWorld()->SweepTestByChannel(Start + Center, *Floor + Center, FQuat::Identity, ECC_Visibility,
                FCollisionShape::MakeSphere(Radius), Query)
            || GetWorld()->SweepTestByChannel(*Floor + Eye, Aim, FQuat::Identity, ECC_Visibility,
                FCollisionShape::MakeSphere(FMath::Max(.05, ProjectileRadius)), Query))
            return {};
        return Floor;
    };
    TOptional<FVector> Result;
    if (Marker)
        if (const auto Linked = Papyrus->ReferenceTransform(Marker, Authored, Parent))
            Result = Reachable(Origin.TransformPosition(Linked->GetLocation()));
    const FVector Forward = (Aim - Start).GetSafeNormal2D();
    for (double Distance = 2 * Radius; !Result && Distance <= FMath::Min(Range, 8 * Radius); Distance *= 2)
        for (int32 Angle = 0; Angle < 360 && !Result; Angle += 45)
            Result = Reachable(Start + Forward.RotateAngleAxis(Angle, FVector::UpVector) * Distance);
    if (Result) Papyrus->FiringPosition(Actor, Target, Origin.InverseTransformPosition(*Result));
}

void AT5Scene::TickAnimations()
{
    for (const auto& [Id, Request] : Papyrus->AnimationRequests())
    {
        if (Ragdolled.Contains(Id)) continue;
        const bool bLocomotion = Request.Event == TEXT("T5Idle") || Request.Event == TEXT("T5Walk")
            || Request.Event == TEXT("T5Jog") || Request.Event == TEXT("T5Run");
        auto* Existing = ReferenceAnimations.Find(Id);
        if (Existing && (*Existing)->Serial != Request.Serial)
        {
            if ((*Existing)->bMoving && !(*Existing)->bFinished)
            {
                FTransform AuthoredPose;
                uint32 BoundTo;
                if (const auto Pose = Papyrus->ReferenceTransform(Id, AuthoredPose, BoundTo))
                    Papyrus->AnimationPose(Id, *Pose, false);
            }
            ReferenceAnimations.Remove(Id);
            Existing = nullptr;
        }
        if (bLocomotion || Request.Event == TEXT("BHR_Default::FlyStartCruise"))
            continue; // PACK owns locomotion; do not integrate its displacement twice.
        FTransform Authored;
        uint32 Parent = 0;
        const auto Current = Papyrus->ReferenceTransform(Id, Authored, Parent);
        if (!Current) continue;
        if (!Existing)
        {
            auto State = MakeShared<FT5ReferenceAnimation>();
            State->Serial = Request.Serial;
            State->Start = SimulationTime;
            State->Parent = Parent;
            State->Anchor = Mechanics ? Mechanics->CorrectPose(Id, Parent, *Current) : *Current;
            FString Rig, Project, Error;
            if (const auto* Visuals = ReferenceVisuals.Find(Id))
                for (const auto& Visual : *Visuals)
                    if (!Visual.SkeletonPath.IsEmpty()) { Rig = Visual.SkeletonPath; Project = Visual.AnimationProject; break; }
            State->bLoaded = LoadT5HkxAnimation(Request.Behavior, Request.Event, Request.Clip,
                [this](const FString& Path, TArray<uint8>& Bytes) { return Papyrus->ReadResource(Path, Bytes); },
                Error, Rig, Request.Speed, Request.Event == TEXT("T5Bow") || Request.Event == TEXT("T5Execution")
                    || Request.Event == TEXT("T5Shout") || Request.Event.Contains(TEXT("::")), false,
                State->Clock, Papyrus->MotionData(), Project);
            if (!State->bLoaded)
            {
                UE_LOG(LogTemp, Error, TEXT("T5_ANIMATION_LOAD_ERROR ref=%08X event=%s %s"), Id, *Request.Event, *Error);
                Papyrus->ReportHostError(FString::Printf(TEXT("Animation %08X / %s | %s"), Id, *Request.Event, *Error));
            }
            else if (const auto& Motion = State->Clock.Clip.Motion; Motion && !State->Clock.Playback.bLoop)
                State->bMoving = Motion->Positions.ContainsByPredicate([](const auto& Key) { return !Key.Value.IsNearlyZero(); })
                    || Motion->Rotations.ContainsByPredicate([](const auto& Key) { return !Key.Value.Equals(FQuat::Identity); });
            ReferenceAnimations.Add(Id, MoveTemp(State));
            Existing = ReferenceAnimations.Find(Id);
        }
        auto& State = **Existing;
        if (!State.bLoaded) continue;
        if (State.bMoving && State.Clock.Playback.bLoop)
        {
            // Older checkpoints could give a looping idle control of the actor's world pose.
            State.bMoving = false;
            Papyrus->AnimationPose(Id, *Current, false);
        }
        if (State.Clock.Playback.bReturnToLocomotion || State.Clock.Playback.bClearLayer)
        {
            State.bFinished=true; State.Pose=*Current;
            Papyrus->AnimationPose(Id,*Current,false);
            continue;
        }
        if (State.Parent && State.Parent == Parent)
        {
            FTransform ParentAuthored;
            uint32 Ancestor;
            if (const auto Pose = Papyrus->ReferenceTransform(Parent, ParentAuthored, Ancestor))
                State.Anchor = Mechanics ? Mechanics->CorrectPose(Parent, Ancestor, *Pose) : *Pose;
        }
        const float Elapsed = SimulationTime - State.Start;
        if (!Parent && !State.bMoving)
            State.Anchor = Mechanics ? Mechanics->CorrectPose(Id, Parent, *Current) : *Current;
        State.Pose = State.bFinished && !Parent ? *Current : T5HkxMotion(State.Clock, Elapsed) * State.Anchor;
        if (State.bMoving && !State.bFinished)
            Papyrus->AnimationPose(Id, State.Pose, !T5HkxFinished(State.Clock, Elapsed));
        // One clock owns gameplay annotations, even if the actor has several meshes,
        // is offscreen, or the first-person camera clip ends earlier than the body.
        DispatchT5HkxEvents(State.Clock, Elapsed, [this, Id, &State](const FString& Event) {
            UE_LOG(LogTemp, Display, TEXT("T5_ANIMATION_EVENT ref=%08X event=%s clipTime=%.4f root=%s"),
                Id, *Event, T5HkxPosition(State.Clock, SimulationTime - State.Start), *State.Pose.ToHumanReadableString());
            AnimationVisualEvent(Id, Event);
            Papyrus->AnimationEvent(Id, Event);
        });
        if (!State.bFinished && T5HkxFinished(State.Clock, Elapsed))
        {
            State.bFinished = true;
            UE_LOG(LogTemp, Display, TEXT("T5_ANIMATION_FINISH ref=%08X generator=%s moving=%d root=%s"),
                Id, *State.Clock.Playback.GeneratorName, State.bMoving, *State.Pose.ToHumanReadableString());
        }
    }
}

void AT5Scene::TickRagdolls()
{
    if (!Mechanics) return;
    for (auto& [Id, Parts] : ReferenceVisuals)
    {
        if (Ragdolled.Contains(Id))
        {
            if (auto* Mesh = FindReferenceSkeleton(Id); Mesh && Mesh->IsVisible())
                Mechanics->ResistRagdollDrift(Id, *Mesh, GetWorld()->GetDeltaSeconds());
            continue;
        }
        const auto* Authored = AuthoredCorpses.Find(Id);
        if (!Authored && !Papyrus->WasKilled(Id)) continue;
        auto* Mesh = FindReferenceSkeleton(Id);
        if (!Mesh || !Mesh->IsVisible()) continue;
        const auto* Previous = CorpsePoses.Find(Id);
        if (Previous) Mesh->SetRelativeTransform(Previous->Transform);
        if (!Mechanics->StartRagdoll(Id, *Mesh, Authored ? &(*Authored)->AsArray() : nullptr,
                Previous ? &Previous->Bones : nullptr)) continue;
        if (const FName Head = Papyrus->SeveredHead(Id); !Head.IsNone()) Mesh->TermBodiesBelow(Head);
        Ragdolled.Add(Id);
        ReferenceAnimations.Remove(Id);
        for (auto& Part : Parts)
        {
            if (Part.Component == Mesh) Part.Collision = ECollisionEnabled::QueryAndPhysics;
            else if (auto* Collider = Cast<UPrimitiveComponent>(Part.Component.Get()); Collider && Part.bColliderOnly)
            {
                Part.Collision = ECollisionEnabled::NoCollision;
                Collider->SetCollisionEnabled(ECollisionEnabled::NoCollision);
            }
            else if (auto* Skin = Cast<USkeletalMeshComponent>(Part.Component.Get()); Skin && Skin != Mesh)
                Skin->SetLeaderPoseComponent(Mesh);
        }
    }
}

void AT5Scene::CaptureRagdollPoses()
{
    for (const uint32 Id : Ragdolled)
        if (const auto* Mesh = FindReferenceSkeleton(Id); Mesh && Mesh->GetSkeletalMeshAsset())
        {
            const auto& Model = Mesh->GetComponentSpaceTransforms();
            const auto& Skeleton = Mesh->GetSkeletalMeshAsset()->GetRefSkeleton();
            if (Model.Num() != Skeleton.GetNum()) continue;
            auto& Pose = CorpsePoses.FindOrAdd(Id);
            Pose.Transform = Mesh->GetRelativeTransform();
            Pose.Bones.Reset(Model.Num());
            for (int32 Index = 0; Index < Model.Num(); ++Index)
            {
                const int32 Parent = Skeleton.GetParentIndex(Index);
                Pose.Bones.Add(Parent == INDEX_NONE ? Model[Index] : Model[Index].GetRelativeTransform(Model[Parent]));
            }
        }
}
