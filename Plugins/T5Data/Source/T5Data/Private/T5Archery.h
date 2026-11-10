#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"

class FT5PapyrusRuntime;
class USkeletalMeshComponent;
class UStaticMesh;
class UStaticMeshComponent;
class UMeshComponent;

struct FT5ArcheryAsset
{
    TWeakObjectPtr<UObject> Mesh;
    FString Project;
};

class FT5Archery
{
public:
    ~FT5Archery();
    void Command(AActor& Owner, FT5PapyrusRuntime& Papyrus, const TArray<TSharedPtr<FJsonValue>>& Args,
        const TMap<FString, TSharedPtr<FT5ArcheryAsset>>& Assets,
        TFunctionRef<USkeletalMeshComponent*(uint32)> Skeleton,
        const TMap<TWeakObjectPtr<UPrimitiveComponent>, uint32>& Contacts);
    void LaunchSpell(AActor& Owner, const TArray<TSharedPtr<FJsonValue>>& Args,
        const TMap<FString, TSharedPtr<FT5ArcheryAsset>>& Assets,
        TFunctionRef<USkeletalMeshComponent*(uint32)> Skeleton);
    void Tick(float Delta, AActor& Owner, FT5PapyrusRuntime& Papyrus,
        const TMap<FString, TSharedPtr<FT5ArcheryAsset>>& Assets,
        TFunctionRef<USkeletalMeshComponent*(uint32)> Skeleton,
        const TMap<TWeakObjectPtr<UPrimitiveComponent>, uint32>& Contacts);
    void SerializeCheckpoint(FArchive& Ar, AActor& Owner, const FT5PapyrusRuntime& Papyrus,
        const TMap<FString, TSharedPtr<FT5ArcheryAsset>>& Assets,
        TFunctionRef<USkeletalMeshComponent*(uint32)> Skeleton);
    void AnimateBow(uint32 Archer, const FString& ActorClip, float Speed, const FT5PapyrusRuntime& Papyrus);

private:
    struct FHands
    {
        TWeakObjectPtr<UMeshComponent> Bow, Arrow;
        TWeakObjectPtr<USkeletalMeshComponent> Archer;
        FString BowPath, ArrowPath, Project, Rig;
        TArray<FString> Clips;
    };
    struct FFlight
    {
        uint32 Id = 0, Archer = 0, Target = 0;
        FVector Position, Velocity, Gravity;
        double Remaining = 0, Range = 0, Radius = 0, FlightToTarget = 0;
        bool bAlwaysHit = false;
        TWeakObjectPtr<UStaticMeshComponent> Mesh;
        FString ModelPath;
    };
    struct FStuckArrow
    {
        TWeakObjectPtr<UStaticMeshComponent> Mesh;
        FString ModelPath;
        double Expires = 0;
    };
    TMap<uint32, FHands> Hands;
    TArray<FFlight> Flights;
    TArray<FStuckArrow> Stuck;
};
