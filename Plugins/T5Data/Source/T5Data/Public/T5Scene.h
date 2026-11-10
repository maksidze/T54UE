#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "GameFramework/Actor.h"
#include "T5Scene.generated.h"

struct FT5LoadResult;
struct FT5LoadRequest;
struct FT5Location;
struct FT5HkxCameraClip;
class ST5MapBrowser;
class SWidget;
class FT5PapyrusRuntime;
class FT5Mechanics;
class FT5AudioRuntime;
class USkeletalMeshComponent;

UCLASS(Config=Game)
class T5DATA_API AT5Scene : public AActor
{
    GENERATED_BODY()

public:
    AT5Scene();

    UPROPERTY(EditAnywhere, Category="T5")
    bool bLoadAtStart = true;

    UPROPERTY(EditAnywhere, Config, Category="T5|Map", meta=(GetOptions="GetMapOptions"))
    FString SelectedMap;

    UPROPERTY(EditAnywhere, Config, Category="T5|Map")
    bool bUseStartCell = false;

    UPROPERTY(EditAnywhere, Config, Category="T5|Map", meta=(EditCondition="bUseStartCell"))
    FIntPoint StartCell = FIntPoint::ZeroValue;

    UPROPERTY(EditAnywhere, Config, Category="T5|Map", meta=(ClampMin="1", ClampMax="3"))
    int32 CellRadius = 2;

    // Extra distance across a cell boundary before moving the resident window.
    UPROPERTY(EditAnywhere, Config, Category="T5|Map", meta=(ClampMin="0", ClampMax="0.45"))
    float StreamingHysteresis = .2f;

    UPROPERTY(EditAnywhere, Config, Category="T5|Cache", meta=(ClampMin="0", ClampMax="2048"))
    int32 StaticMeshCacheMB = 256;

    UFUNCTION(CallInEditor, BlueprintCallable, Category="T5")
    void RefreshMapCatalog();

    UFUNCTION(BlueprintPure, Category="T5")
    TArray<FString> GetMapOptions() const;

    UFUNCTION(BlueprintCallable, Category="T5")
    void ToggleMapBrowser();

    UFUNCTION(BlueprintCallable, Category="T5")
    void OpenT5Map(const FString& Name);

    UFUNCTION(CallInEditor, BlueprintCallable, Category="T5")
    void LoadFromT5();

    UFUNCTION(CallInEditor, BlueprintCallable, Category="T5")
    void ClearT5Preview();

    UFUNCTION(BlueprintCallable, Category="T5|Save")
    void QuickSave();

    UFUNCTION(BlueprintCallable, Category="T5|Save")
    void QuickLoad();

    UFUNCTION(BlueprintCallable, Category="T5|Save")
    void OpenSaveBrowser();

    void CloseSaveBrowser();
    bool SaveGameSlot(const FString& Slot);
    void LoadGameSlot(const FString& Slot);

    UFUNCTION(BlueprintCallable, Category="T5|Character")
    void OpenRaceMenu();

protected:
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void Destroyed() override;
    virtual void Tick(float DeltaTime) override;

private:
    friend class FT5CharacterCreator;
    friend class FT5RaceMenuSmoke;
    friend class FT5LightingSmoke;
    void TickCharacterCreation();
    AActor* BuildCharacterPreview(const FT5LoadResult& Data, FString& Error);
    TSharedPtr<class FT5CharacterCreator> CharacterCreator;
    bool bAutoRaceMenuUsed = false;
    friend class ST5MapBrowser;
    friend class FT5SaveSmoke;
    friend class FT5SaveBrowserSmoke;
    friend class FT5ExecutionFlowSmoke;
    friend class FT5ReferenceAudit;
    friend class ST5Inventory;
    friend class ST5SaveBrowser;
    TSharedPtr<class ST5SaveBrowser> SaveBrowser;
    FString PendingScriptSave;
    bool bSaveBrowserWasPaused = false;
    void TickRequestedSave();
    void SerializeCheckpoint(FArchive& Ar);
    void RestoreCheckpointHost();
    void FinishCheckpointLoad();
    void SaveMessage(const FString& Message, bool bError = false) const;
    TArray<uint8> PendingNativeCheckpoint, PendingHostCheckpoint, PendingPhysicsCheckpoint, PendingArcheryCheckpoint;
    TOptional<FTransform> CheckpointPlayer;
    FRotator CheckpointView = FRotator::ZeroRotator;
    bool bRestoringCheckpoint = false;
    uint32 CheckpointLocation = 0;
    FIntPoint CheckpointCell = FIntPoint::ZeroValue;
    friend class FT5MapSwitchSmoke;
    friend class FT5ResourceCacheSmoke;
    friend class FT5PapyrusSmoke;
    friend class FT5MQ101Smoke;
    friend class FT5AudioSmoke;
    friend class FT5FaceRuntimeSmoke;
    friend class FT5LoadingSmoke;
    bool ResolvePaths(FString& Data, FString& Plugins, FString& Bridge) const;
    void RequestCatalog(TFunction<void(bool)> OnReady);
    void CloseMapBrowser();
    void LoadSelectedMap(const FString& Name, TOptional<FIntPoint> Cell);
    void LoadLocation(uint32 FormId, FIntPoint Cell, bool bRecenter);
    void RequestScene(const FT5LoadRequest& Request, bool bRecenter);
    class FT5FrameTask BuildScene(TUniquePtr<FT5LoadResult> Data, FT5LoadRequest Request, bool bRecenter);
    void TickSceneBuild();
    void ShowLoadingScreen();
    TSharedPtr<struct FT5LoadProgress> LoadProgress;
    TSharedPtr<SWidget> LoadingScreen;
    bool bStartupPreparing = false, bLoadingFailed = false, bFullLoadingScreen = false, bCatalogLoading = false;
    TSharedPtr<class FT5FrameTask> PendingBuild;
    double BuildStarted = 0, BuildMaxStepMs = 0;
    FString BuildStep;
    TArray<TWeakObjectPtr<AActor>> RetiredScenes;
    void InteractT5();
    friend class ST5Dialogue;
    void OpenDialogue(uint32 Actor);
    void CloseDialogue();
    void TickDialogue();
    TSharedPtr<class ST5Dialogue> DialogueMenu;
    TWeakObjectPtr<class APlayerController> DialogueController;
    void OpenPlayerInventory();
    void OpenInventory(uint32 Container);
    void CloseInventory();
    void ReadyPlayerWeapon();
    void AttackPlayerWeapon();
    void TickFirstPerson();
    void PlayViewAttack();
    bool BuildFirstPersonView(const struct FT5LoadResult& Data);
    bool PlayFirstPersonClip(const FString& Clip, bool bLoop);
    void DestroyFirstPersonView();
    TWeakObjectPtr<AActor> FirstPersonActor;
    TWeakObjectPtr<USkeletalMeshComponent> FirstPersonArms;
    TWeakObjectPtr<class UStaticMeshComponent> FirstPersonWeapon;
    uint64 FirstPersonKey = 0;
    int32 FirstPersonWeaponType = 0;
    FString FirstPersonClip;
    TSet<FString> MissingViewClips;
    bool bViewDrawn = false, bViewTransition = false, bViewWeaponVisible = false;
    uint64 ViewScriptSerial = 0;
    uint32 FindInteractionReference(const FVector& Start, const FVector& End, bool bIncludeLoot = false) const;
    TSharedPtr<class ST5Inventory> InventoryMenu;
    uint32 OpenInventoryOwner = 0;
    bool bInventoryWasPaused = false;
    void ApplyPapyrusState();
    void ApplyPapyrusWorld();
    void TickAnimations();
    void AnimationVisualEvent(uint32 Id, const FString& Event);
    void TickAnimationVisuals();
    void ClearAnimationVisuals();
    struct FAnimationProp
    {
        TWeakObjectPtr<class UStaticMeshComponent> Component;
        FName Socket;
        FString Unload;
        bool bAttempted = false;
    };
    FAnimationProp BuildAnimationProp(const struct FT5LoadResult& Data);
    struct FDamageVisual
    {
        int32 Stage = INDEX_NONE;
        bool bNoCollision = false;
        FAnimationProp Replacement;
    };
    TMap<uint32, FDamageVisual> DamageVisuals;
    void ApplyDestruction(uint32 Id, int32 Stage, bool bNoCollision, bool bHasModel);
    TMap<uint32, TMap<FString, FAnimationProp>> AnimationProps;
    TMap<uint32, TSet<FString>> DrawnAnimationObjects;
    TMap<uint32, TWeakObjectPtr<USkeletalMeshComponent>> HeadlessMeshes;
    TMap<uint32, TWeakObjectPtr<AActor>> DetachedHeads;
    TMap<uint32, FTransform> DetachedHeadTransforms;
    void TickRagdolls();
    void CaptureRagdollPoses();
    void SweepActorFall(uint32 Actor, FVector Velocity, double Delta);
    void FindFiringPosition(uint32 Actor, uint32 Target, uint32 Marker, double Range, double ProjectileRadius);
    TMap<uint32, TSharedPtr<struct FT5ReferenceAnimation>> ReferenceAnimations;
    void ShowPapyrusErrors();
    void TickPresentation();
    void TickFaces();
    struct FFaceState { uint32 Target = 0; bool bTracking = true; int32 Expression = -1; float Strength = 0; };
    TMap<uint32, FFaceState> Faces;
    USkeletalMeshComponent* FindReferenceSkeleton(uint32 Id) const;
    bool PlayLocomotion(USkeletalMeshComponent& Mesh, const FString& Project, const FString& Gait,
        TOptional<float> ResumeAt, FString& Error);

    struct FReferenceVisual
    {
        TWeakObjectPtr<USceneComponent> Component;
        ECollisionEnabled::Type Collision = ECollisionEnabled::NoCollision;
        FTransform Original;
        FString SkeletonPath;
        FString MotionNode;
        bool bColliderOnly = false;
        uint64 AnimationSerial = 0;
        uint32 AnimationMount = 0;
        bool bPlayedIdle = false;
        FString AnimationProject;
        uint64 ResumeSerial = 0;
        TOptional<float> ResumeAt;
    };

    UPROPERTY(VisibleAnywhere, Transient, Category="T5|Map")
    FString CatalogStatus;

    UPROPERTY(VisibleAnywhere, Transient, Category="T5|Map")
    FString LoadStatus;

    UPROPERTY(Transient, DuplicateTransient)
    TObjectPtr<AActor> LoadedScene;

    TMap<uint32, TSharedPtr<FT5Location>> Locations;
    TSharedPtr<ST5MapBrowser> MapBrowser;
    TSharedPtr<FT5PapyrusRuntime> Papyrus;
    TSharedPtr<FT5Mechanics> Mechanics;
    TSharedPtr<FT5AudioRuntime> RuntimeAudio;
    TSharedPtr<class FT5Archery> RuntimeArchery;
    TSharedPtr<class ST5Titles> Titles;
    TSharedPtr<class ST5Compass> Compass;
    TMap<FString, TSharedPtr<struct FT5ArcheryAsset>> ArcheryAssets;
    TSharedPtr<SWidget> PapyrusErrors;
    TMap<uint32, TArray<FReferenceVisual>> ReferenceVisuals;
    TSet<uint32> LootableReferences;
    TMap<uint32, TSharedPtr<class FJsonValue>> AuthoredCorpses;
    TSet<uint32> Ragdolled;
    struct FCorpsePose { FTransform Transform; TArray<FTransform> Bones; };
    TMap<uint32, FCorpsePose> CorpsePoses;
    struct FLocomotionSet { FString Rig, Idle, Walk, Jog, Run; };
    TMap<FString, FLocomotionSet> Locomotion;
    TMap<uint64, TWeakObjectPtr<class UTexture2D>> RecentTextures;
    UPROPERTY(Transient, DuplicateTransient)
    TMap<FString, TObjectPtr<class UStaticMesh>> CachedStaticMeshes;
    TArray<FString> StaticMeshCacheOrder;
    uint64 StaticMeshCacheBytes = 0;
    int32 LastMeshCacheHits = 0;
    TWeakObjectPtr<class UPostProcessComponent> VehiclePostProcess;
    TWeakObjectPtr<class UPostProcessComponent> ScriptPostProcess;
    bool bOpeningViewReady = false;
    TMap<TWeakObjectPtr<UPrimitiveComponent>, uint32> InteractionComponents;
    uint32 ActiveWorld = 0;
    uint32 ActiveLocation = 0;
    uint32 OpeningQuest = 0;
    uint32 AppliedControls = 0;
    uint32 AppliedGameFlags = 1;
    float AppliedSittingRotation = 0;
    uint32 LoadedSpawnCount = 0;
    TSet<uint32> LoadedInteriorArrivals;
    double SpawnReloadDue = 0;
    uint64 PlayerAnimationSerial = 0;
    double PlayerAnimationStart = 0;
    double SimulationTime = 0;
    TSharedPtr<FT5HkxCameraClip> PlayerCamera;
    TOptional<FQuat> LastPlayerHeading;
    TOptional<FTransform> LastPlayerScriptPose;
    uint32 LastPlayerVehicle = 0;
    TOptional<FVector> LastLoggedPlayerPose;
    FIntPoint RequestedCell = FIntPoint::ZeroValue;
    bool bSceneLoading = false;
    uint64 LoadGeneration = 0;
    uint64 CatalogGeneration = 0;
};
