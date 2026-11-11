#pragma once

#include "CoreMinimal.h"
#include "T5Audio.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeLock.h"

struct FT5LoadProgress
{
    mutable FCriticalSection Mutex;
    FString Phase = TEXT("Opening game files"), Item;
    uint64 Done = 0, Total = 0, Textures = 0, Bytes = 0;
    void Set(const FString& InPhase, const FString& InItem, uint64 InDone = 0, uint64 InTotal = 0)
    { FScopeLock Lock(&Mutex); Phase = InPhase; Item = InItem; Done = InDone; Total = InTotal; }
};

struct FT5PapyrusApi;

struct FT5SpawnedReference
{
    uint32 Id, Base, Anchor, Flags, Cell;
    float Position[6];
    float Scale;
};

struct FT5AnimationRequest
{
    uint32 Idle;
    uint64 Serial;
    FString Behavior, Event, Clip;
    float Speed = 1;
};

struct FT5LoadResult
{
    void* Handle = nullptr;
    const void* (*GetBuffer)(const void*, const char*, uint64*) = nullptr;
    void (*Release)(void*) = nullptr;
    const FT5PapyrusApi* Papyrus = nullptr;
    FString Error;

    // Parsed by the worker before the result is handed to the game thread.
    TMap<FString, TSharedPtr<class FJsonObject>> PreparedJson;

    FT5LoadResult() = default;
    FT5LoadResult(const FT5LoadResult&) = delete;
    FT5LoadResult& operator=(const FT5LoadResult&) = delete;
    ~FT5LoadResult() { if (Handle) Release(Handle); }

    TConstArrayView64<uint8> Read(const FString& Name) const;
};

struct FT5LoadRequest
{
    TSharedPtr<FT5LoadProgress> Progress;
    bool bCatalog = false;
    bool bNewGameStart = false;
    bool bPlayable = false;
    uint32 OpeningQuest = 0;
    uint32 FormId = 0;
    FIntPoint Cell = FIntPoint::ZeroValue;
    int32 Radius = 1;
    uint64 ScriptRevision = 0;
    TArray<uint32> TrackedReferences;
    TArray<FT5SpawnedReference> SpawnedReferences;
};

TUniquePtr<FT5LoadResult> LoadT5Scene(const FString& Data, const FString& Plugins, const FString& BridgeDirectory,
    const FT5LoadRequest& Request);

enum class ET5DialogueAction : int32
{
    Read, Choose, Close, Buy, Sell, LeaveTrade
};

class FT5PapyrusRuntime
{
public:
    static constexpr uint32 RaceMenuPending = 1u << 6;
    ~FT5PapyrusRuntime();
    bool Attach(const FT5LoadResult& Location, const TArray<uint8>* Checkpoint = nullptr);
    bool SaveCheckpoint(TArray<uint8>& Bytes) const;
    void SerializeCheckpoint(FArchive& Ar);
    void Capture(FT5LoadRequest& Request) const;
    bool HasScripts(uint32 Id) const;
    TOptional<bool> IsDisabled(uint32 Id) const;
    bool Activate(uint32 Id);
    FString DialogueState() const;
    void DialogueAction(ET5DialogueAction Action, uint32 Choice = 0);
    bool Inventory(uint32 Owner, TArray<TSharedPtr<FJsonValue>>& Entries) const;
    TUniquePtr<FT5LoadResult> InventoryModel(uint32 Item) const;
    FVector3f ResourcePoolRatios() const;
    bool InventoryAction(uint32 Owner, uint32 Item, int32 Count);
    int32 PlayerWeapon(int32 Action, uint32 Target = 0, float* Reach = nullptr);
    bool FirePlayerBow(const FVector& Origin, const FVector& Direction);
    bool WasKilled(uint32 Id) const;
    FName SeveredHead(uint32 Id) const;
    TUniquePtr<FT5LoadResult> AnimationObject(const FString& EditorId) const;
    TUniquePtr<FT5LoadResult> DestructionModel(uint32 Id) const;
    uint64 PlayerViewKey(bool& bDrawn) const;
    TUniquePtr<FT5LoadResult> PlayerView() const;
    TUniquePtr<FT5LoadResult> PlayerBody() const;
    void Tick(double Seconds);
    void SetGamepad(bool bActive);
    bool ReadResource(const FString& Path, TArray<uint8>& Bytes) const;
    TOptional<FTransform> ReferenceTransform(uint32 Id, FTransform& Authored, uint32& Parent) const;
    int MotionType(uint32 Id) const;
    TOptional<bool> DoorState(uint32 Id, double& Elapsed) const;
    uint32 Weather(float (&Data)[12]) const;
    void GameState(uint32& Controls, uint32& Flags, float& SittingRotation) const;
    uint32 SpawnedCount() const;
    void AnimationPose(uint32 Id, const FTransform& Pose, bool bActive);
    void PhysicsPose(uint32 Id, const FTransform& Pose, bool bActive);
    const class FT5MotionDatabase* MotionData();
    void AnimationEvent(uint32 Id, const FString& Event);
    uint64 Revision() const;
    FT5AudioDecoder AudioDecoder() const;
    TArray<FT5AudioRequest> TakeAudioRequests();
    void AudioFinished(int32 Instance, bool bPlayed);
    void ArrowImpact(uint32 Shot, uint32 Target);
    void FallResult(uint32 Actor, const FVector& Position, const FVector& Velocity, bool bGrounded);
    void FiringPosition(uint32 Actor, uint32 Target, const FVector& Position);
    void SetLineOfSight(int (*Callback)(void*, uint32, uint32), void* Context);
    TArray<TSharedPtr<FJsonValue>> TakePresentation() { return MoveTemp(Presentation); }
    const TMap<uint32, FT5AnimationRequest>& AnimationRequests() const { return Animations; }
    const FString& RecentErrors() const { return ErrorText; }
    int32 ObservedQuestStage(const FString& EditorId) const { return QuestStages.FindRef(EditorId); }
    void ReportHostError(const FString& Message);
    TUniquePtr<FT5LoadResult> CharacterCatalog() const;
    TUniquePtr<FT5LoadResult> CharacterPreview(const struct FT5CharacterChoice& Choice, const FString& Bridge, const FString& Mods) const;
    bool CommitCharacter(const struct FT5CharacterChoice& Choice, const FString& Name, const FString& Morphs);
    float PlayerEyeHeight() const;
    float VisualScale(uint32 Id) const;
    void BeginEnvironment(uint32 World);
    uint32 Environment(TArrayView<float> Data) const;

private:
    void LogEvents();
    TSharedPtr<class FT5MotionDatabase> MotionDatabase;
    bool bMotionLoaded = false;
    const FT5PapyrusApi* Api = nullptr;
    void* Handle = nullptr;
    TArray<FString> ErrorLines;
    FString ErrorText;
    TMap<FString, int32> QuestStages;
    TMap<uint32, FT5AnimationRequest> Animations;
    uint64 AnimationSerial = 0;
    TArray<FT5AudioRequest> AudioRequests;
    TArray<TSharedPtr<FJsonValue>> Presentation;
};
