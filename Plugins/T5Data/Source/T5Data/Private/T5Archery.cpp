#include "T5Archery.h"

#include "T5Bridge.h"
#include "T5Coordinates.h"
#include "T5Hkx.h"

#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogT5Archery, Log, All);

namespace
{
    template <class T> void Destroy(TWeakObjectPtr<T> Mesh)
    {
        if (auto* Component = Mesh.Get())
            Component->DestroyComponent();
    }

    FName Bone(const USkeletalMeshComponent& Mesh, std::initializer_list<const TCHAR*> Names)
    {
        for (const auto* Name : Names)
            if (Mesh.GetBoneIndex(FName(Name)) != INDEX_NONE)
                return FName(Name);
        return NAME_None;
    }

    UMeshComponent* Model(AActor& Owner, const FT5ArcheryAsset* Asset)
    {
        if (!Asset || !Asset->Mesh.IsValid())
            return nullptr;
        UMeshComponent* Component = nullptr;
        if (auto* Static = Cast<UStaticMesh>(Asset->Mesh.Get()))
        {
            auto* Mesh = NewObject<UStaticMeshComponent>(&Owner, NAME_None, RF_Transient | RF_DuplicateTransient);
            Mesh->SetStaticMesh(Static);
            Component = Mesh;
        }
        else if (auto* Skeletal = Cast<USkeletalMesh>(Asset->Mesh.Get()))
        {
            auto* Mesh = NewObject<USkeletalMeshComponent>(&Owner, NAME_None, RF_Transient | RF_DuplicateTransient);
            Mesh->SetSkeletalMesh(Skeletal);
            Component = Mesh;
        }
        if (!Component)
            return nullptr;
        Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
        Component->RegisterComponent();
        return Component;
    }

    FString ResourcePath(const FString& Directory, FString Name)
    {
        Name.ReplaceInline(TEXT("\\"), TEXT("/"));
        FString Path = Directory / Name;
        return FPaths::CollapseRelativeDirectories(Path) ? Path : FString{};
    }

    UMeshComponent* AttachEquipment(AActor& Owner, USkeletalMeshComponent& Rig, const FT5ArcheryAsset* Asset, bool bBow)
    {
        auto* Mesh = Model(Owner, Asset);
        if (Mesh)
        {
            const auto Socket = bBow ? Bone(Rig, {TEXT("SHIELD"), TEXT("NPC L Hand [LHnd]")})
                : Bone(Rig, {TEXT("WEAPON"), TEXT("NPC R Hand [RHnd]")});
            Mesh->AttachToComponent(&Rig, FAttachmentTransformRules::SnapToTargetIncludingScale, Socket);
        }
        return Mesh;
    }

    FCollisionQueryParams ArrowTrace(uint32 Archer, const TMap<TWeakObjectPtr<UPrimitiveComponent>, uint32>& Contacts)
    {
        FCollisionQueryParams Trace(SCENE_QUERY_STAT(T5Arrow), false);
        for (const auto& [Component, Id] : Contacts)
            if (Id == Archer && Component.IsValid())
                Trace.AddIgnoredComponent(Component.Get());
        return Trace;
    }

    FVector Torso(const USkeletalMeshComponent& Mesh)
    {
        const auto Name = Bone(Mesh, {TEXT("NPC Spine2 [Spn2]"), TEXT("NPC Spine1 [Spn1]"), TEXT("NPC Spine [Spn0]")});
        return Name.IsNone() ? Mesh.Bounds.Origin : Mesh.GetSocketLocation(Name);
    }

    FQuat ArrowRotation(const FVector& Direction)
    {
        return FRotationMatrix::MakeFromX(Direction).ToQuat();
    }

    void PlaceFlyingArrow(UStaticMeshComponent& Mesh, const FVector& Tip, const FVector& Velocity)
    {
        const auto Rotation = ArrowRotation(Velocity);
        const auto Box = Mesh.GetStaticMesh()->GetBoundingBox();
        Mesh.SetWorldLocationAndRotation(Tip - Rotation.RotateVector(FVector(Box.Max.X, 0, 0)), Rotation);
    }
}

FT5Archery::~FT5Archery()
{
    for (const auto& [Actor, Equipment] : Hands)
    {
        Destroy(Equipment.Arrow);
        Destroy(Equipment.Bow);
    }
    for (const auto& Flight : Flights)
        Destroy(Flight.Mesh);
    for (const auto& Arrow : Stuck)
        Destroy(Arrow.Mesh);
}

void FT5Archery::Command(AActor& Owner, FT5PapyrusRuntime& Papyrus, const TArray<TSharedPtr<FJsonValue>>& Args,
    const TMap<FString, TSharedPtr<FT5ArcheryAsset>>& Assets, TFunctionRef<USkeletalMeshComponent*(uint32)> Skeleton,
    const TMap<TWeakObjectPtr<UPrimitiveComponent>, uint32>& Contacts)
{
    if (Args.Num() != 13)
        return;
    const int32 Type = Args[1]->AsNumber();
    const uint32 Archer = Args[2]->AsNumber();
    auto* Rig = Skeleton(Archer);
    if (Type == 3)
    {
        if (const auto* Equipment = Hands.Find(Archer))
        {
            Destroy(Equipment->Arrow);
            Destroy(Equipment->Bow);
            Hands.Remove(Archer);
        }
        return;
    }
    if (!Rig)
        return;
    const FString Path = Args[5]->AsString();
    const auto* Asset = Assets.FindRef(Path).Get();
    auto& Equipment = Hands.FindOrAdd(Archer);
    Equipment.Archer = Rig;
    if (Type == 0 || Type == 1)
    {
        auto& Held = Type == 0 ? Equipment.Bow : Equipment.Arrow;
        auto& HeldPath = Type == 0 ? Equipment.BowPath : Equipment.ArrowPath;
        Destroy(Held);
        HeldPath = Path;
        Held = AttachEquipment(Owner, *Rig, Asset, Type == 0);
        if (Type == 0 && Asset)
        {
            Equipment.Project = Asset->Project;
            Equipment.Rig.Reset();
            Equipment.Clips.Reset();
        }
        return;
    }
    const uint32 Target = Args[3]->AsNumber(), Shot = Args[4]->AsNumber();
    const double Speed = Args[6]->AsNumber() * T5CentimetersPerUnit;
    const auto* TargetRig = Skeleton(Target);
    if (Type != 2 || !Shot || !TargetRig || Speed <= 0)
        return;
    Equipment.ArrowPath.Reset();
    const auto Socket = Bone(*Rig, {TEXT("SHIELD"), TEXT("NPC L Hand [LHnd]")});
    const auto Start = Socket.IsNone() ? Torso(*Rig) : Rig->GetSocketLocation(Socket);
    const auto End = Torso(*TargetRig);
    const auto Delta = End - Start;
    if (Delta.IsNearlyZero())
        return;
    const double Radius = FMath::Max(.05, Args[9]->AsNumber() * T5CentimetersPerUnit);
    const bool bHoldWhenBlocked = Args[12]->AsNumber() != 0;
    if (bHoldWhenBlocked)
    {
        FHitResult Obstacle;
        if (Owner.GetWorld()->SweepSingleByChannel(Obstacle, Start, End, FQuat::Identity, ECC_Visibility,
                FCollisionShape::MakeSphere(Radius), ArrowTrace(Archer, Contacts)))
        {
            const auto* Hit = Contacts.Find(Obstacle.GetComponent());
            if (!Hit || *Hit != Target)
            {
                Destroy(Equipment.Arrow);
                Papyrus.ArrowImpact(Shot, 0);
                Papyrus.AnimationEvent(Archer, TEXT("T5BowObstructed"));
                UE_LOG(LogT5Archery, Display, TEXT("T5_ARROW_BLOCKED shot=%u archer=%08X"), Shot, Archer);
                return;
            }
        }
    }
    Destroy(Equipment.Arrow);
    const FVector Gravity(0, 0, Owner.GetWorld()->GetGravityZ() * Args[7]->AsNumber());
    double Arrival = Delta.Length() / Speed;
    for (int32 Pass = 0; Pass < 6; ++Pass)
        Arrival = (Delta - Gravity * (.5 * Arrival * Arrival)).Length() / Speed;
    const auto Velocity = (Delta - Gravity * (.5 * Arrival * Arrival)).GetSafeNormal() * Speed;
    const double Range = Args[8]->AsNumber() * T5CentimetersPerUnit;
    const double Life = Args[10]->AsNumber();
    auto* Mesh = Cast<UStaticMeshComponent>(Model(Owner, Asset));
    if (Mesh)
        PlaceFlyingArrow(*Mesh, Start, Velocity);
    Flights.Add({Shot, Archer, Target, Start, Velocity, Gravity, Life > 0 ? Life : FMath::Max(1., Range / Speed),
        Range, Radius, Arrival, Args[11]->AsNumber() != 0, Mesh});
    Flights.Last().ModelPath = Path;
    UE_LOG(LogT5Archery, Display, TEXT("T5_ARROW_RELEASE shot=%u archer=%08X target=%08X mesh=%d speed=%.1f life=%.3f distance=%.1f"),
        Shot, Archer, Target, Mesh != nullptr, Speed, Life, Delta.Length());
}

void FT5Archery::LaunchSpell(AActor& Owner, const TArray<TSharedPtr<FJsonValue>>& Args,
    const TMap<FString, TSharedPtr<FT5ArcheryAsset>>& Assets, TFunctionRef<USkeletalMeshComponent*(uint32)> Skeleton)
{
    if (Args.Num() != 17)
        return;
    const uint32 Shot = Args[1]->AsNumber(), Caster = Args[2]->AsNumber(), Source = Args[3]->AsNumber();
    const uint32 Target = Args[4]->AsNumber();
    auto Start = T5ToUnrealPosition(FVector(Args[5]->AsNumber(), Args[6]->AsNumber(), Args[7]->AsNumber()));
    auto End = T5ToUnrealPosition(FVector(Args[8]->AsNumber(), Args[9]->AsNumber(), Args[10]->AsNumber()));
    if (Shot < 0xff800000u)
    {
        Start = Owner.GetActorTransform().TransformPosition(Start);
        End = Owner.GetActorTransform().TransformPosition(End);
    }
    if (const auto* Rig = Source ? Skeleton(Source) : nullptr)
    {
        const auto Mouth = Bone(*Rig, {TEXT("NPC Head MagicNode [Hmag]"), TEXT("NPC Jaw"), TEXT("NPC Jaw [Jaw]"), TEXT("NPC Head"), TEXT("NPC Head [Head]")});
        Start = Mouth.IsNone() ? Torso(*Rig) : Rig->GetSocketLocation(Mouth);
    }
    if (const auto* Rig = Target ? Skeleton(Target) : nullptr)
        End = Torso(*Rig);
    const auto Direction = (End - Start).GetSafeNormal();
    const double Speed = Args[11]->AsNumber() * T5CentimetersPerUnit;
    if (!Shot || Speed <= 0 || Direction.IsNearlyZero())
        return;
    const FVector Gravity(0, 0, Owner.GetWorld()->GetGravityZ() * Args[12]->AsNumber());
    const double Range = Args[13]->AsNumber() * T5CentimetersPerUnit;
    const double Radius = FMath::Max(.05, Args[14]->AsNumber() * T5CentimetersPerUnit);
    const double Life = Args[15]->AsNumber();
    FString Path = Args[16]->AsString();
    Path.ReplaceInline(TEXT("\\"), TEXT("/"));
    Path.ToLowerInline();
    if (!Path.IsEmpty() && !Path.StartsWith(TEXT("meshes/")))
        Path = TEXT("meshes/") + Path;
    auto* Mesh = Cast<UStaticMeshComponent>(Model(Owner, Assets.FindRef(Path).Get()));
    if (Mesh)
        Mesh->SetWorldLocationAndRotation(Start, ArrowRotation(Direction));
    Flights.Add({Shot, Caster, Target, Start, Direction * Speed, Gravity, Life, Range, Radius, 0, false, Mesh, Path});
    UE_LOG(LogT5Archery, Display, TEXT("T5_MAGIC_LAUNCH shot=%u caster=%08X target=%08X mesh=%d"), Shot, Caster, Target, Mesh != nullptr);
}

void FT5Archery::AnimateBow(uint32 Archer, const FString& ActorClip, float Speed, const FT5PapyrusRuntime& Papyrus)
{
    auto* Equipment = Hands.Find(Archer);
    if (!Equipment || Equipment->Project.IsEmpty())
        return;
    auto* Mesh = Cast<USkeletalMeshComponent>(Equipment->Bow.Get());
    if (!Mesh)
        return;
    const auto Read = [&](const FString& Path, TArray<uint8>& Bytes) { return Papyrus.ReadResource(Path, Bytes); };
    if (Equipment->Clips.IsEmpty())
    {
        TArray<uint8> Bytes;
        if (!Read(Equipment->Project, Bytes))
            return;
        const auto Name = ReadT5HkxProjectCharacter(Bytes);
        const auto Character = ResourcePath(FPaths::GetPath(Equipment->Project), Name);
        if (Name.IsEmpty() || Character.IsEmpty() || !Read(Character, Bytes))
            return;
        FString Rig;
        TArray<FString> Clips;
        if (!ReadT5HkxCharacterClips(Bytes, Rig, Clips))
            return;
        const auto Directory = FPaths::GetPath(FPaths::GetPath(Character));
        Equipment->Rig = ResourcePath(Directory, Rig);
        for (auto& Clip : Clips)
            Equipment->Clips.Add(ResourcePath(Directory, MoveTemp(Clip)));
    }
    const auto File = FPaths::GetCleanFilename(ActorClip);
    for (const auto& Clip : Equipment->Clips)
        if (FPaths::GetCleanFilename(Clip).Equals(File, ESearchCase::IgnoreCase))
        {
            FString Error;
            if (!PlayT5Hkx(*Mesh, {}, {}, Clip, Read, Error, Equipment->Rig, {}, Speed))
                UE_LOG(LogT5Archery, Warning, TEXT("T5_BOW_HKX_ERROR %s"), *Error);
            return;
        }
}

void FT5Archery::Tick(float Delta, AActor& Owner, FT5PapyrusRuntime& Papyrus,
    const TMap<FString, TSharedPtr<FT5ArcheryAsset>>& Assets,
    TFunctionRef<USkeletalMeshComponent*(uint32)> Skeleton,
    const TMap<TWeakObjectPtr<UPrimitiveComponent>, uint32>& Contacts)
{
    UWorld& World = *Owner.GetWorld();
    const auto Now = World.GetTimeSeconds();
    for (auto& [Id, Equipment] : Hands)
    {
        auto* Current = Skeleton(Id);
        if (!Current)
            continue;
        if (Current != Equipment.Archer.Get())
        {
            Destroy(Equipment.Bow);
            Destroy(Equipment.Arrow);
            Equipment.Archer = Current;
            Equipment.Bow = AttachEquipment(Owner, *Current, Assets.FindRef(Equipment.BowPath).Get(), true);
            Equipment.Arrow = AttachEquipment(Owner, *Current, Assets.FindRef(Equipment.ArrowPath).Get(), false);
            if (const auto* Request = Papyrus.AnimationRequests().Find(Id); Request && Request->Event == TEXT("T5Bow"))
                AnimateBow(Id, Request->Clip, Request->Speed, Papyrus);
        }
        if (auto* Arrow = Equipment.Arrow.Get())
            if (const auto* Rig = Equipment.Archer.Get())
            {
                const auto Left = Bone(*Rig, {TEXT("SHIELD"), TEXT("NPC L Hand [LHnd]")});
                const auto Right = Bone(*Rig, {TEXT("WEAPON"), TEXT("NPC R Hand [RHnd]")});
                if (!Left.IsNone() && !Right.IsNone())
                {
                    const auto Nock = Rig->GetSocketLocation(Right);
                    const auto Direction = (Rig->GetSocketLocation(Left) - Nock).GetSafeNormal();
                    if (!Direction.IsNearlyZero())
                        Arrow->SetWorldLocationAndRotation(Nock, ArrowRotation(Direction));
                }
            }
    }
    Stuck.RemoveAllSwap([&](const auto& Arrow)
    {
        if (Arrow.Expires > Now && Arrow.Mesh.IsValid())
            return false;
        Destroy(Arrow.Mesh);
        return true;
    });
    for (int32 Index = Flights.Num() - 1; Index >= 0; --Index)
    {
        auto& Shot = Flights[Index];
        const auto Trace = ArrowTrace(Shot.Archer, Contacts);
        const bool bMagic = Shot.Id >= 0xff800000u;
        double Time = FMath::Min(static_cast<double>(Delta), Shot.Remaining);
        bool bContact = false;
        while (Time > UE_SMALL_NUMBER && !bContact)
        {
            double Step = FMath::Min(Time, 1. / 120.);
            if (Shot.bAlwaysHit && Shot.FlightToTarget > UE_SMALL_NUMBER)
                Step = FMath::Min(Step, Shot.FlightToTarget);
            const FVector End = Shot.Position + (Shot.Velocity + Shot.Gravity * (.5 * Step)) * Step;
            FHitResult Hit;
            bContact = World.SweepSingleByChannel(Hit, Shot.Position, End, FQuat::Identity, ECC_Visibility,
                FCollisionShape::MakeSphere(Shot.Radius), Trace);
            uint32 Target = 0;
            if (bContact)
                Target = Contacts.FindRef(Hit.GetComponent());
            Shot.FlightToTarget -= Step;
            if (!bContact && Shot.bAlwaysHit && Shot.FlightToTarget <= UE_SMALL_NUMBER)
            {
                // AlwaysHit is an authored UseWeapon option. Still sweep the path
                // to the moving target, so a wall can intercept the shot.
                if (const auto* Rig = Skeleton(Shot.Target); Rig && Rig->IsVisible())
                {
                    const auto Aim = Torso(*Rig);
                    bContact = World.SweepSingleByChannel(Hit, End, Aim, FQuat::Identity, ECC_Visibility,
                        FCollisionShape::MakeSphere(Shot.Radius), Trace);
                    Target = bContact ? Contacts.FindRef(Hit.GetComponent()) : Shot.Target;
                    if (!bContact)
                    {
                        bContact = true;
                        Hit.ImpactPoint = Aim;
                    }
                }
                Shot.bAlwaysHit = false;
            }
            Shot.Range -= (End - Shot.Position).Length();
            Shot.Position = bContact ? Hit.ImpactPoint : End;
            Shot.Velocity += Shot.Gravity * Step;
            Shot.Remaining -= Step;
            Time -= Step;
            if (auto* Mesh = Shot.Mesh.Get())
            {
                if (bMagic)
                    Mesh->SetWorldLocationAndRotation(Shot.Position, ArrowRotation(Shot.Velocity));
                else
                    PlaceFlyingArrow(*Mesh, Shot.Position, Shot.Velocity);
                if (bContact && !bMagic)
                {
                    if (auto* Component = Hit.GetComponent())
                        Mesh->AttachToComponent(Component, FAttachmentTransformRules::KeepWorldTransform, Hit.BoneName);
                    else if (auto* Rig = Skeleton(Target))
                        Mesh->AttachToComponent(Rig, FAttachmentTransformRules::KeepWorldTransform,
                            Bone(*Rig, {TEXT("NPC Spine2 [Spn2]"), TEXT("NPC Spine1 [Spn1]")}));
                    Stuck.Add({Mesh, Shot.ModelPath, Now + 20.});
                    Shot.Mesh.Reset();
                }
            }
            if (bContact)
            {
                if (bMagic)
                {
                    UE_LOG(LogT5Archery, Display, TEXT("T5_MAGIC_CONTACT shot=%u ref=%08X initial=%d component=%s"),
                        Shot.Id, Target, Hit.bStartPenetrating, *GetNameSafe(Hit.GetComponent()));
                }
                else
                {
                    UE_LOG(LogT5Archery, Display, TEXT("T5_ARROW_CONTACT shot=%u ref=%08X"), Shot.Id, Target);
                }
                Papyrus.ArrowImpact(Shot.Id, Target);
            }
            if (Shot.Range < 0)
                break;
        }
        if (bContact || Shot.Remaining <= UE_SMALL_NUMBER || Shot.Range < 0)
        {
            if (!bContact)
                Papyrus.ArrowImpact(Shot.Id, 0);
            Destroy(Shot.Mesh);
            Flights.RemoveAtSwap(Index, EAllowShrinking::No);
        }
    }
}

void FT5Archery::SerializeCheckpoint(FArchive& Ar, AActor& Owner, const FT5PapyrusRuntime& Papyrus,
    const TMap<FString, TSharedPtr<FT5ArcheryAsset>>& Assets, TFunctionRef<USkeletalMeshComponent*(uint32)> Skeleton)
{
    int32 Count = Hands.Num(); Ar << Count;
    if (Count < 0 || Count > 4096) { Ar.SetError(); return; }
    const auto Equipment = [&](uint32 Actor, FHands& Hand) {
        bool Bow = Hand.Bow.IsValid(), Arrow = Hand.Arrow.IsValid();
        Ar << Hand.BowPath << Hand.ArrowPath << Hand.Project << Hand.Rig << Hand.Clips << Bow << Arrow;
        if (Ar.IsLoading())
        {
            auto* Rig = Skeleton(Actor); Hand.Archer = Rig;
            if (Rig && Bow) Hand.Bow = AttachEquipment(Owner, *Rig, Assets.FindRef(Hand.BowPath).Get(), true);
            if (Rig && Arrow) Hand.Arrow = AttachEquipment(Owner, *Rig, Assets.FindRef(Hand.ArrowPath).Get(), false);
            if (const auto* Request = Papyrus.AnimationRequests().Find(Actor); Request && Request->Event == TEXT("T5Bow"))
                AnimateBow(Actor, Request->Clip, Request->Speed, Papyrus);
        }
    };
    if (Ar.IsLoading())
        for (int32 I = 0; I < Count && !Ar.IsError(); ++I) { uint32 Actor = 0; Ar << Actor; Equipment(Actor, Hands.FindOrAdd(Actor)); }
    else for (auto& [Actor, Hand] : Hands) { uint32 Key = Actor; Ar << Key; Equipment(Actor, Hand); }
    Count = Flights.Num(); Ar << Count;
    if (Count < 0 || Count > 4096) { Ar.SetError(); return; }
    if (Ar.IsLoading()) Flights.SetNum(Count);
    for (auto& Flight : Flights)
    {
        Ar << Flight.Id << Flight.Archer << Flight.Target << Flight.Position << Flight.Velocity << Flight.Gravity
            << Flight.Remaining << Flight.Range << Flight.Radius << Flight.FlightToTarget << Flight.bAlwaysHit << Flight.ModelPath;
        if (Ar.IsLoading())
        {
            Flight.Mesh = Cast<UStaticMeshComponent>(Model(Owner, Assets.FindRef(Flight.ModelPath).Get()));
            if (auto* Mesh = Flight.Mesh.Get())
            {
                if (Flight.Id >= 0xff800000u)
                    Mesh->SetWorldLocationAndRotation(Flight.Position, ArrowRotation(Flight.Velocity));
                else
                    PlaceFlyingArrow(*Mesh, Flight.Position, Flight.Velocity);
            }
        }
    }
    Count = Stuck.Num(); Ar << Count;
    if (Count < 0 || Count > 4096) { Ar.SetError(); return; }
    if (Ar.IsLoading()) Stuck.SetNum(Count);
    for (auto& Arrow : Stuck)
    {
        FTransform Pose = Arrow.Mesh.IsValid() ? Arrow.Mesh->GetComponentTransform() : FTransform::Identity;
        double Remaining = Arrow.Expires - Owner.GetWorld()->GetTimeSeconds();
        Ar << Arrow.ModelPath << Pose << Remaining;
        if (Ar.IsLoading())
        {
            Arrow.Expires = Owner.GetWorld()->GetTimeSeconds() + Remaining;
            Arrow.Mesh = Cast<UStaticMeshComponent>(Model(Owner, Assets.FindRef(Arrow.ModelPath).Get()));
            if (auto* Mesh = Arrow.Mesh.Get()) Mesh->SetWorldTransform(Pose);
        }
    }
}
