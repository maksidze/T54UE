#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Widgets/SCompoundWidget.h"

class AT5Scene;
class FT5PapyrusRuntime;
struct FT5LoadResult;
class FJsonObject;
class SVerticalBox;
class SEditableTextBox;
class USceneComponent;
class UMeshComponent;

struct FT5CharacterChoice
{
    uint32 Race = 0x13746, Female = 0, Preset = 0;
    uint32 Parts[7] = {};
    float Weight = 50, Height = 1;
    float Skin[3] = {.65f, .49f, .38f};
};
static_assert(sizeof(FT5CharacterChoice) == 60);

class FT5CharacterCreator
{
public:
    ~FT5CharacterCreator();
    bool Open(AT5Scene& Scene, bool Required);
    void Tick();
    void Close();
    bool Accept();
    void Cancel();
    void Changed(bool Rebuild = true);
    void ApplyMorphs();
    void CycleRace(int Direction);
    void CyclePart(int Type, int Direction);
    void CyclePreset(int Direction);
    void RefreshMorphList();
    void SavePreset();
    void LoadPreset();
    TSharedRef<SWidget> MakePanel();
    FString RaceLabel() const;
    FString PartLabel(int Type) const;
    TArray<uint32> Presets() const;
    TArray<TSharedPtr<FJsonObject>> Parts(int Type) const;
    void Orbit(float Degrees);
    void Zoom(float Amount);

    FT5CharacterChoice Choice;
    FString Name = TEXT("Довакин"), Status, Query;
    TMap<FName, float> Morphs;
    TArray<FName> AvailableMorphs;
    TWeakObjectPtr<AActor> Preview, Camera;
    bool bRequired = false, bReady = false, bDirty = true;
    double RebuildDue = 0;
    int32 PreviewRevision = 0;
    bool IsClosed() const { return bClosed; }
    const FTransform& PlayerPlacement() const { return WorldPlacement; }

private:
    friend class FT5RaceMenuSmoke;
    friend class FT5UISmoke;
    struct FViewOccluder
    {
        TArray<TWeakObjectPtr<UMeshComponent>> Parts;
        FBox Bounds = FBox(ForceInit);
        bool bHidden = false;
    };
    TArray<FViewOccluder> ViewOccluders;
    TWeakObjectPtr<AT5Scene> Owner;
    TSharedPtr<FT5PapyrusRuntime> Runtime;
    TSharedPtr<SWidget> Widget;
    TSharedPtr<SVerticalBox> MorphList;
    TArray<TSharedPtr<FJsonObject>> Races, HeadParts;
    TFuture<TUniquePtr<FT5LoadResult>> Pending;
    FT5CharacterChoice Requested;
    FString BridgePath, ModPath;
    TWeakObjectPtr<AActor> PreviousView;
    bool bPreviousCursor = false;
    bool bPreviousMessages = true;
    FTransform WorldPlacement = FTransform::Identity;
    FVector InitialCameraPosition = FVector::ZeroVector;
    FQuat InitialCameraRotation = FQuat::Identity;
    float FacingYaw = 0, Angle = 0, Distance = 320;
    float InitialFieldOfView = 75;
    double CameraTransitionStarted = 0;
    bool bWasWorldPaused = false, bWasSceneTickableWhenPaused = false;
    bool bActivated = false, bAccepted = false;
    TArray<TPair<TWeakObjectPtr<USceneComponent>,bool>> HiddenPlayerParts;
    FString MorphJson() const;
    float MorphValue(FName Key) const;
    void UpdateCamera();
    void UpdateOcclusion(const FVector& CameraPosition, const FVector& Head);
    bool bClosed = false;
};

class ST5RaceMenu : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(ST5RaceMenu) {} SLATE_END_ARGS()
    void Construct(const FArguments&, FT5CharacterCreator* Creator);
    virtual bool SupportsKeyboardFocus() const override { return true; }
    virtual FReply OnKeyDown(const FGeometry&, const FKeyEvent&) override;
private:
    FT5CharacterCreator* Model = nullptr;
};
