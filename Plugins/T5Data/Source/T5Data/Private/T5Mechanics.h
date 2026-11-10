#pragma once

#include "CoreMinimal.h"

class AActor;
class FJsonObject;
class FJsonValue;
class FT5PapyrusRuntime;
class USkeletalMeshComponent;
class UStaticMeshComponent;

class FT5Mechanics
{
public:
    ~FT5Mechanics();
    void Load(AActor& Owner, const FJsonObject& Models);
    void Tick(AActor& Owner, FT5PapyrusRuntime& Papyrus, bool bExterior);
    void SerializeCheckpoint(FArchive& Ar);
    void ContinueMotionFrom(const FT5Mechanics& Previous);
    void SetActive(bool bActive);
    void SetSequence(uint32 Id, const FString& Name, double Elapsed);
    void SetHkxSequence(uint32 Id, const FString& Rig, const FString& Animation, double Elapsed,
        FT5PapyrusRuntime& Papyrus, const FString& Layer, double LayerElapsed);
    void SetPaused(bool bPaused);
    bool StartRagdoll(uint32 Id, USkeletalMeshComponent& Mesh, const TArray<TSharedPtr<FJsonValue>>* Authored,
        const TArray<FTransform>* Previous);
    void ResistRagdollDrift(uint32 Id, USkeletalMeshComponent& Mesh, float DeltaTime) const;
    UStaticMeshComponent* DetachBodyPart(uint32 Id, FName Bone, AActor& Owner, const FTransform& Pose) const;
    void Configure(const FString& Data, const FString& Plugins) { DataPath=Data; PluginPath=Plugins; }
    void LoadCollisions(AActor& Owner, const FJsonObject& Manifest,
        const TMultiMap<uint32, USkeletalMeshComponent*>& Skeletons,
        TFunctionRef<void(uint32, UStaticMeshComponent*, const FString&)> Register);
    int32 WheelContacts(uint32 Id) const;
    bool HasVehicleSurface(uint32 Id) const;
    bool IsAnimatedDoor(uint32 Id) const { return Doors.Contains(Id); }
    TOptional<double> ArticulationError(uint32 Id) const;
    FTransform CorrectPose(uint32 Id, uint32 Parent, const FTransform& Pose) const;
    FTransform DoorDelta(uint32 Id, const FString& Node, const FTransform& Authored, const FTransform& Pose) const;

private:
    friend class FT5LoadingSmoke;
    friend class FT5ArticulationSmoke;
    struct FPart
    {
        FString Node;
        FTransform Rest;
        TWeakObjectPtr<UStaticMeshComponent> Collision;
    };

    struct FCart
    {
        TSharedPtr<class FT5Articulation> Physics;
        FPart Chassis;
        TArray<FPart> Wheels;
        FTransform Route, Pose;
        int32 Contacts = 0;
        int32 TerrainContacts = 0;
        bool bGrounded = false;
        double LastLoggedLift = 0;
        double LastSample = 0;
    };

    struct FDoorPart
    {
        FTransform Rest;
        TArray<FTransform> Closed, Open;
        FTransform Pose;
    };

    struct FDoor
    {
        TMap<FString, FDoorPart> Parts;
        double OpenSeconds = 0, CloseSeconds = 0;
        TOptional<bool> OpenState;
    };

    TMap<uint32, FCart> Carts;
    struct FGroundActor
    {
        TWeakObjectPtr<USkeletalMeshComponent> Mesh;
        FTransform Original, Route, Pose;
        TArray<FVector> Feet;
        bool bGrounded = false;
    };
    TMap<uint32, FGroundActor> GroundActors;
    TMap<uint32, FDoor> Doors;
    struct FSequenceTrack
    {
        FTransform Rest;
        TArray<FTransform> Frames;
    };
    struct FModelSequence
    {
        double Seconds = 0;
        TMap<FString, FSequenceTrack> Tracks;
    };
    struct FModelPlayback
    {
        TMap<FString, FModelSequence> Sequences;
        FString Name, Layer;
        double Elapsed = 0, LayerElapsed = 0;
    };
    TMap<uint32, FModelPlayback> ModelsByReference;
    TMap<uint32, TSharedPtr<struct FT5RagdollRig>> RagdollRigs;
    TArray<TWeakObjectPtr<UPrimitiveComponent>> CollisionParts;
    TWeakObjectPtr<AActor> CollisionOwner;
    TWeakObjectPtr<class UT5WeatherFogComponent> WeatherFog;
    TWeakObjectPtr<class UPostProcessComponent> WeatherFogVolume;
    TWeakObjectPtr<class UMaterialInstanceDynamic> WeatherFogMaterial;
    bool bTriedWeatherFogMaterial = false;
    uint32 ActiveWeather = 0;
    double WeatherUpdatedAt = -1;
    TWeakObjectPtr<class UTextureCube> WeatherAmbientCube;
    double LastSupportLog = -1;
    FString DataPath, PluginPath;
    bool bSimulationPaused = false;
};
