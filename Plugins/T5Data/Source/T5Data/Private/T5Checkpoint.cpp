#include "T5Scene.h"
#include "T5Coordinates.h"
#include "T5Bridge.h"
#include "T5Audio.h"
#include "T5Archery.h"
#include "T5Mechanics.h"
#include "GameFramework/SpectatorPawn.h"
#include "T5HkxAnimInstance.h"
#include "T5HkxCamera.h"
#include "T5ReferenceAnimation.h"
#include "T5Titles.h"
#include "T5MenuStyle.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PawnMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/SecureHash.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SConstraintCanvas.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Input/Reply.h"

namespace
{
    constexpr uint32 Magic = 0x54355356, Version = 1;
    constexpr int32 MaxSaveBytes = 256 * 1024 * 1024;
    bool SmokeSave() { return FParse::Param(FCommandLine::Get(), TEXT("T5CheckpointSmoke")); }
    FString NewSlot(const TCHAR* Kind)
    {
        return FString::Printf(TEXT("T5%s_%s_%s"), Kind, *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));
    }
    bool ValidSlot(const FString& Slot)
    {
        return !Slot.IsEmpty() && Slot.Len() <= 128 && Slot == FPaths::GetCleanFilename(Slot)
            && !Slot.Contains(TEXT("..")) && !Slot.Contains(TEXT(":")) && !Slot.Contains(TEXT("/")) && !Slot.Contains(TEXT("\\"));
    }
    struct SaveEntry { FString Slot, Label; FDateTime Date; bool bValid = false; };
    TArray<SaveEntry> SavedGames()
    {
        TArray<FString> Files;
        const auto Directory = FPaths::ProjectSavedDir() / TEXT("SaveGames");
        IFileManager::Get().FindFiles(Files, *(Directory / TEXT("*.sav")), true, false);
        TArray<SaveEntry> Entries;
        for (const auto& File : Files)
        {
            const auto Slot = FPaths::GetBaseFilename(File);
            if (!ValidSlot(Slot) || Slot.EndsWith(TEXT("Backup")) || (Slot.StartsWith(TEXT("T5CheckpointSmoke")) || Slot.StartsWith(TEXT("T5SaveBrowserSmoke")))) continue;
            TArray<uint8> Data;
            if (!UGameplayStatics::LoadDataFromSlot(Data, Slot, 0) || Data.Num() < 28) continue;
            uint32 FileMagic = 0; FMemory::Memcpy(&FileMagic, Data.GetData() + 20, 4);
            if (FileMagic != Magic) continue;
            uint8 Hash[20]; FSHA1::HashBuffer(Data.GetData() + 20, Data.Num() - 20, Hash);
            const bool bValid = !FMemory::Memcmp(Hash, Data.GetData(), 20);
            const auto Date = IFileManager::Get().GetTimeStamp(*(Directory / File));
            const FString Kind = Slot.StartsWith(TEXT("T5Auto")) ? TEXT("Автосохранение")
                : Slot.StartsWith(TEXT("T5Quick")) ? TEXT("Быстрое сохранение") : TEXT("Ручное сохранение");
            Entries.Add({Slot, FString::Printf(TEXT("%s  •  %s%s"), *Date.ToString(TEXT("%d.%m.%Y  %H:%M:%S")),
                *Kind, bValid ? TEXT("") : TEXT(" — повреждено")), Date, bValid});
        }
        Entries.Sort([](const auto& A, const auto& B) { return A.Date > B.Date; });
        return Entries;
    }
    template<class T> void Optional(FArchive& Ar, TOptional<T>& Value)
    {
        bool Present = Value.IsSet(); Ar << Present;
        T Item{};
        if (Present) { if (Ar.IsSaving()) Item = *Value; Ar << Item; Value = Item; }
        else Value.Reset();
    }
    void References(FArchive& Ar, FT5LoadRequest& Request)
    {
        Ar << Request.ScriptRevision << Request.TrackedReferences;
        int32 Count = Request.SpawnedReferences.Num(); Ar << Count;
        if (Count < 0 || Count > 100000) { Ar.SetError(); return; }
        if (Ar.IsLoading()) Request.SpawnedReferences.SetNum(Count);
        for (auto& Ref : Request.SpawnedReferences)
        {
            Ar << Ref.Id << Ref.Base << Ref.Anchor << Ref.Flags << Ref.Cell;
            for (float& Value : Ref.Position) Ar << Value;
            Ar << Ref.Scale;
        }
    }
    template<class VisualMap> void VisualPositions(FArchive& Ar, VisualMap& Map)
    {
        int32 Count = 0;
        if (Ar.IsSaving()) for (auto& [Id, Parts] : Map) for (auto& Part : Parts)
            if (auto* Mesh = Cast<USkeletalMeshComponent>(Part.Component.Get()); Mesh && Cast<UT5HkxAnimInstance>(Mesh->GetAnimInstance())) ++Count;
        Ar << Count;
        if (Count < 0 || Count > 100000) { Ar.SetError(); return; }
        if (Ar.IsSaving())
            for (auto& [Id, Parts] : Map) for (auto& Part : Parts)
                if (auto* Mesh = Cast<USkeletalMeshComponent>(Part.Component.Get()))
                    if (auto* Animation = Cast<UT5HkxAnimInstance>(Mesh->GetAnimInstance()))
                    { uint32 Key = Id; float Position = Animation->PlaybackPosition(); Ar << Key << Part.SkeletonPath << Part.AnimationSerial << Position; }
        if (Ar.IsLoading())
            for (int32 I = 0; I < Count && !Ar.IsError(); ++I)
            {
                uint32 Id = 0; uint64 Serial = 0; FString Rig; float Position = 0; Ar << Id << Rig << Serial << Position;
                if (auto* Parts = Map.Find(Id)) for (auto& Part : *Parts) if (Part.SkeletonPath == Rig)
                { Part.ResumeSerial = Serial; Part.ResumeAt = Position; Part.AnimationSerial = 0; }
            }
    }
    bool ReadSave(TArray<uint8>& Data)
    {
        if (Data.Num() < 28 || Data.Num() > MaxSaveBytes) return false;
        uint8 Hash[20]; FSHA1::HashBuffer(Data.GetData() + 20, Data.Num() - 20, Hash);
        if (FMemory::Memcmp(Hash, Data.GetData(), 20)) return false;
        Data.RemoveAt(0, 20);
        return true;
    }
}

void AT5Scene::SaveMessage(const FString& Message, bool bError) const
{
    UE_LOG(LogTemp, Display, TEXT("T5_CHECKPOINT %s"), *Message);
    if (GEngine) GEngine->AddOnScreenDebugMessage(-1, 7.f, bError ? FColor::Red : FColor::Green, Message);
}

void AT5Scene::QuickSave()
{
    SaveGameSlot(SmokeSave() ? TEXT("T5CheckpointSmoke") : NewSlot(TEXT("Quick")));
}

bool AT5Scene::SaveGameSlot(const FString& Slot)
{
    if (!ValidSlot(Slot)) return false;
    if (!GetWorld() || !GetWorld()->IsGameWorld() || !Papyrus || !ActiveLocation || bSceneLoading
        || bStartupPreparing || bLoadingFailed || bCatalogLoading || MapBrowser || SaveBrowser || InventoryMenu || DialogueMenu || CharacterCreator || bRestoringCheckpoint)
    { SaveMessage(TEXT("Сохранение доступно после завершения загрузки локации"), true); return false; }
    TArray<uint8> Native, Host;
    if (!Papyrus->SaveCheckpoint(Native))
    { SaveMessage(TEXT("Не удалось сохранить Papyrus. Обновите T5DataBridge.dll"), true); return false; }
    FMemoryWriter HostWriter(Host);
    SerializeCheckpoint(HostWriter);
    if (HostWriter.IsError()) { SaveMessage(TEXT("Ошибка сохранения состояния сцены"), true); return false; }
    TArray<uint8> Data;
    Data.SetNumZeroed(20);
    FMemoryWriter Writer(Data); Writer.Seek(20);
    uint32 FileMagic = Magic, FileVersion = Version;
    Writer << FileMagic << FileVersion << ActiveLocation << RequestedCell << OpeningQuest;
    FT5LoadRequest Snapshot; Papyrus->Capture(Snapshot); References(Writer, Snapshot);
    Writer << Native << Host;
    if (Writer.IsError() || Data.Num() > MaxSaveBytes)
    { SaveMessage(TEXT("Сохранение превышает допустимый размер"), true); return false; }
    FSHA1::HashBuffer(Data.GetData() + 20, Data.Num() - 20, Data.GetData());
    // Keep the last valid save if a write is interrupted or the main file is damaged.
    TArray<uint8> Previous;
    if (UGameplayStatics::LoadDataFromSlot(Previous, Slot, 0))
    {
        auto Validated = Previous;
        if (ReadSave(Validated) && !UGameplayStatics::SaveDataToSlot(Previous, Slot + TEXT("Backup"), 0))
        { SaveMessage(TEXT("Не удалось записать резервную копию сохранения"), true); return false; }
    }
    if (!UGameplayStatics::SaveDataToSlot(Data, Slot, 0))
    { SaveMessage(TEXT("Не удалось записать сохранение на диск"), true); return false; }
    SaveMessage(TEXT("Игра сохранена — F9: выбор сохранения"));
    UE_LOG(LogTemp, Display, TEXT("T5_SAVE_OK time=%.6f bytes=%d location=%08X cell=%d,%d"),
        SimulationTime, Data.Num(), ActiveLocation, RequestedCell.X, RequestedCell.Y);
    UE_LOG(LogTemp, Display, TEXT("T5_SAVE_SLOT %s"), *Slot);
    return true;
}

void AT5Scene::QuickLoad()
{
    FString Slot;
    FParse::Value(FCommandLine::Get(), TEXT("T5SaveSlot="), Slot);
    if (!Slot.IsEmpty()) { LoadGameSlot(Slot); return; }
    if (SmokeSave()) { LoadGameSlot(TEXT("T5CheckpointSmoke")); return; }
    for (const auto& Entry : SavedGames())
        if (Entry.bValid) { LoadGameSlot(Entry.Slot); return; }
    LoadGameSlot(TEXT("T5QuickSave"));
}

void AT5Scene::LoadGameSlot(const FString& Slot)
{
    if (!ValidSlot(Slot)) return;
    if (!GetWorld() || !GetWorld()->IsGameWorld() || bSceneLoading || bCatalogLoading || CharacterCreator || bRestoringCheckpoint)
    { SaveMessage(TEXT("Дождитесь завершения текущей загрузки"), true); return; }
    TArray<uint8> Data;
    bool bBackup = false;
    if (!UGameplayStatics::LoadDataFromSlot(Data, Slot, 0) || !ReadSave(Data))
    {
        bBackup = true; Data.Reset();
        if (!UGameplayStatics::LoadDataFromSlot(Data, Slot + TEXT("Backup"), 0) || !ReadSave(Data))
        { SaveMessage(TEXT("Сохранение не найдено или повреждено. Сначала сохраните игру клавишей F5"), true); return; }
    }
    FMemoryReader Reader(Data); Reader.ArMaxSerializeSize = MaxSaveBytes;
    uint32 FileMagic = 0, FileVersion = 0, Location = 0, Quest = 0;
    FIntPoint Cell;
    TArray<uint8> Native, Host;
    Reader << FileMagic << FileVersion;
    if (FileMagic != Magic || FileVersion != Version)
    { SaveMessage(TEXT("Неподдерживаемая версия сохранения"), true); return; }
    Reader << Location << Cell << Quest;
    FT5LoadRequest Snapshot; References(Reader, Snapshot);
    Reader << Native << Host;
    if (Reader.IsError() || Reader.Tell() != Data.Num() || !Location || Native.IsEmpty() || Host.IsEmpty()
        || FMath::Abs(int64(Cell.X)) > 32767 || FMath::Abs(int64(Cell.Y)) > 32767)
    { SaveMessage(TEXT("Некорректные данные сохранения"), true); return; }
    CloseSaveBrowser();
    CloseMapBrowser();
    if (PapyrusErrors)
        if (auto* Viewport = GetWorld()->GetGameViewport()) Viewport->RemoveViewportWidgetContent(PapyrusErrors.ToSharedRef());
    PapyrusErrors.Reset();
    ClearT5Preview(); Papyrus.Reset();
    if (auto* Controller = GetWorld()->GetFirstPlayerController())
    { Controller->ResetIgnoreMoveInput(); Controller->ResetIgnoreLookInput(); if (auto* HUD = Controller->GetHUD()) HUD->bShowHUD = true; }
    AppliedControls = 0; AppliedGameFlags = 1; AppliedSittingRotation = 0;
    PendingScriptSave.Reset();
    PendingNativeCheckpoint = MoveTemp(Native); PendingHostCheckpoint = MoveTemp(Host);
    CheckpointLocation = Location; CheckpointCell = Cell;
    OpeningQuest = Quest;
    bRestoringCheckpoint = bStartupPreparing = true;
    SaveMessage(bBackup ? TEXT("Загрузка резервного сохранения…") : TEXT("Загрузка сохранения…"));
    Snapshot.FormId = Location; Snapshot.Cell = Cell; Snapshot.Radius = FMath::Clamp(CellRadius, 1, 3);
    RequestScene(Snapshot, true);
}

class ST5SaveBrowser : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(ST5SaveBrowser) {} SLATE_END_ARGS()
    void Construct(const FArguments&, AT5Scene* Scene)
    {
        Owner = Scene;
        const auto Resources = Scene->UIResources;
        const auto CanSave = TAttribute<bool>::CreateLambda([Weak = Owner] {
            const auto* S = Weak.Get();
            return S && S->Papyrus && S->ActiveLocation && !S->bSceneLoading && !S->bStartupPreparing
                && !S->bRestoringCheckpoint;
        });
        auto Rows = SNew(SVerticalBox);
        const auto Entries = SavedGames();
        for (const auto& Entry : Entries)
        {
            const auto Slot = Entry.Slot;
            Rows->AddSlot().AutoHeight().Padding(0, 4)
            [ SNew(SButton).ButtonStyle(T5MenuButtonStyle()).IsEnabled(Entry.bValid)
                .OnClicked_Lambda([Weak = Owner, Slot] {
                    if (auto* S = Weak.Get()) S->LoadGameSlot(Slot);
                    return FReply::Handled();
                })
                [SNew(STextBlock).Text(FText::FromString(Entry.Label)).Font(T5MenuFont(Resources, 21))] ];
        }
        if (Entries.IsEmpty()) Rows->AddSlot().AutoHeight().Padding(10)
            [SNew(STextBlock).Text(FText::FromString(TEXT("Сохранений пока нет"))).Font(T5MenuFont(Resources, 21))];
        ChildSlot
        [
            SNew(SConstraintCanvas)
            + SConstraintCanvas::Slot().Anchors(FAnchors(0, 0, 1, 1)).Offset(FMargin(0))
            [T5MenuBackdrop()]
            + SConstraintCanvas::Slot().Anchors(FAnchors(.12f, .065f, .88f, .845f)).Offset(FMargin(0))
            [
                SNew(SOverlay)
                + SOverlay::Slot()
                [
                    SNew(SBorder).BorderImage(T5MenuArt(Resources, TEXT("JournalPanel")))
                        .BorderBackgroundColor(FLinearColor(1, 1, 1, .72f))
                    [
                        SNew(SConstraintCanvas)
                        + SConstraintCanvas::Slot().Anchors(FAnchors(.12f, .0f, .91f, .079f)).Offset(FMargin(0))
                        [SNew(SBox).HAlign(HAlign_Right).VAlign(VAlign_Center)
                            [SNew(STextBlock).Text(FText::FromString(TEXT("СИСТЕМА"))).Font(T5MenuFont(Resources, 30))]]
                        + SConstraintCanvas::Slot().Anchors(FAnchors(.055f, .15f, .25f, .94f)).Offset(FMargin(0))
                        [
                            SNew(SBox).VAlign(VAlign_Center)
                            [
                                SNew(SVerticalBox)
                                + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
                                [
                                    SNew(SButton).ButtonStyle(T5MenuButtonStyle()).IsEnabled(CanSave)
                                        .OnClicked_Lambda([Weak = Owner] {
                                            if (auto* S = Weak.Get()) { S->CloseSaveBrowser(); S->QuickSave(); }
                                            return FReply::Handled();
                                        })
                                    [SNew(STextBlock).Text(FText::FromString(TEXT("БЫСТРОЕ СОХРАНЕНИЕ"))).Font(T5MenuFont(Resources, 21))]
                                ]
                                + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(0, 5)
                                [
                                    SNew(SButton).ButtonStyle(T5MenuButtonStyle()).IsEnabled(CanSave)
                                        .OnClicked_Lambda([Weak = Owner] {
                                            if (auto* S = Weak.Get()) { S->CloseSaveBrowser(); S->SaveGameSlot(NewSlot(TEXT("Manual"))); }
                                            return FReply::Handled();
                                        })
                                    [SNew(STextBlock).Text(FText::FromString(TEXT("СОХРАНИТЬ"))).Font(T5MenuFont(Resources, 21))]
                                ]
                                + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right).Padding(7, 3)
                                [SNew(STextBlock).Text(FText::FromString(TEXT("ЗАГРУЗИТЬ"))).Font(T5MenuFont(Resources, 26))]
                            ]
                        ]
                        + SConstraintCanvas::Slot().Anchors(FAnchors(.265f, .117f, .278f, .97f)).Offset(FMargin(0))
                        [SNew(SImage).Image(T5MenuArt(Resources, TEXT("JournalDivider"), false))]
                        + SConstraintCanvas::Slot().Anchors(FAnchors(.31f, .205f, .935f, .93f)).Offset(FMargin(0))
                        [
                            SNew(SVerticalBox)
                            + SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 17)
                            [SNew(STextBlock).Text(FText::FromString(TEXT("СОХРАНЕНИЯ"))).Font(T5MenuFont(Resources, 28))]
                            + SVerticalBox::Slot().FillHeight(1)
                            [SNew(SScrollBox) + SScrollBox::Slot()[Rows]]
                        ]
                    ]
                ]
            ]
            + SConstraintCanvas::Slot().Anchors(FAnchors(0, .893f, 1, 1)).Offset(FMargin(0))
            [
                T5MenuBar(Resources, TEXT("JournalBottom"),
                    SNew(SButton).ButtonStyle(T5MenuButtonStyle()).HAlign(HAlign_Left)
                        .OnClicked_Lambda([Weak = Owner] { if (auto* S = Weak.Get()) S->CloseSaveBrowser(); return FReply::Handled(); })
                        [SNew(STextBlock).Text(FText::FromString(TEXT("[Esc]  НАЗАД"))).Font(T5MenuFont(Resources, 20))])
            ]
        ];
    }
    virtual bool SupportsKeyboardFocus() const override { return true; }
    virtual FReply OnKeyDown(const FGeometry&, const FKeyEvent& Event) override
    {
        if (Event.GetKey() == EKeys::Escape || Event.GetKey() == EKeys::F9)
        { if (auto* S = Owner.Get()) S->CloseSaveBrowser(); return FReply::Handled(); }
        return FReply::Unhandled();
    }
private:
    TWeakObjectPtr<AT5Scene> Owner;
};

void AT5Scene::OpenSaveBrowser()
{
    if (SaveBrowser) { CloseSaveBrowser(); return; }
    if (CharacterCreator || DialogueMenu || bRestoringCheckpoint) return;
    auto* World = GetWorld();
    auto* Controller = World ? World->GetFirstPlayerController() : nullptr;
    auto* Viewport = World ? World->GetGameViewport() : nullptr;
    if (!World || !World->IsGameWorld() || !Controller || !Viewport) return;
    CloseMapBrowser();
    bSaveBrowserWasPaused = UGameplayStatics::IsGamePaused(World);
    UGameplayStatics::SetGamePaused(World, true);
    SAssignNew(SaveBrowser, ST5SaveBrowser, this);
    Viewport->AddViewportWidgetContent(SaveBrowser.ToSharedRef(), 1500);
    FInputModeUIOnly Mode; Mode.SetWidgetToFocus(SaveBrowser);
    Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
    Controller->SetInputMode(Mode); Controller->bShowMouseCursor = true;
    Controller->SetIgnoreMoveInput(true); Controller->SetIgnoreLookInput(true);
    if (Mechanics) Mechanics->SetPaused(true);
    UE_LOG(LogTemp, Display, TEXT("T5_SAVE_BROWSER_OPEN entries=%d"), SavedGames().Num());
}

void AT5Scene::CloseSaveBrowser()
{
    if (!SaveBrowser) return;
    if (auto* World = GetWorld())
    {
        if (auto* Viewport = World->GetGameViewport()) Viewport->RemoveViewportWidgetContent(SaveBrowser.ToSharedRef());
        if (auto* Controller = World->GetFirstPlayerController())
        {
            Controller->SetInputMode(FInputModeGameOnly()); Controller->bShowMouseCursor = false;
            Controller->SetIgnoreMoveInput(false); Controller->SetIgnoreLookInput(false);
        }
    }
    SaveBrowser.Reset();
    if (GetWorld()) UGameplayStatics::SetGamePaused(GetWorld(), bSaveBrowserWasPaused);
    if (Mechanics) Mechanics->SetPaused(false);
}

void AT5Scene::TickRequestedSave()
{
    if (FParse::Param(FCommandLine::Get(), TEXT("T5ReadOnly"))) { PendingScriptSave.Reset(); return; }
    if (PendingScriptSave.IsEmpty() || bSceneLoading || bStartupPreparing || bRestoringCheckpoint
        || MapBrowser || SaveBrowser || DialogueMenu || CharacterCreator) return;
    const auto Kind = MoveTemp(PendingScriptSave);
    PendingScriptSave.Reset();
    if (!SaveGameSlot(NewSlot(*Kind))) PendingScriptSave = Kind;
}

void AT5Scene::SerializeCheckpoint(FArchive& Ar)
{
    Ar << SimulationTime << bOpeningViewReady << PlayerAnimationSerial << PlayerAnimationStart;
    Optional(Ar, LastPlayerHeading); Optional(Ar, LastPlayerScriptPose);
    Ar << LastPlayerVehicle << AppliedSittingRotation;
    FTransform Player = FTransform::Identity; FRotator View = FRotator::ZeroRotator;
    if (Ar.IsSaving())
        if (auto* Controller = GetWorld()->GetFirstPlayerController())
        { if (auto* Pawn = Controller->GetPawnOrSpectator()) Player = Pawn->GetActorTransform(); View = Controller->GetControlRotation(); }
    Ar << Player << View;
    if (Ar.IsLoading()) { CheckpointPlayer = Player; CheckpointView = View; }
    Papyrus->SerializeCheckpoint(Ar);
    // Only references present in the final restored cell have host physics bodies.
    TArray<uint8> Physics;
    if (Ar.IsSaving() && Mechanics) { FMemoryWriter Writer(Physics); Mechanics->SerializeCheckpoint(Writer); VisualPositions(Writer, ReferenceVisuals); }
    Ar << Physics;
    if (Ar.IsLoading()) PendingPhysicsCheckpoint = MoveTemp(Physics);
    TArray<uint8> Archery;
    if (Ar.IsSaving() && RuntimeArchery && LoadedScene)
    {
        FMemoryWriter Writer(Archery);
        RuntimeArchery->SerializeCheckpoint(Writer, *LoadedScene, *Papyrus, ArcheryAssets,
            [this](uint32 Id) { return FindReferenceSkeleton(Id); });
    }
    Ar << Archery;
    if (Ar.IsLoading()) PendingArcheryCheckpoint = MoveTemp(Archery);
    bool HasAudio = RuntimeAudio.IsValid(); Ar << HasAudio;
    if (HasAudio)
    {
        if (Ar.IsLoading()) RuntimeAudio = MakeShared<FT5AudioRuntime>();
        RuntimeAudio->SerializeCheckpoint(Ar, *Papyrus, SimulationTime);
    }
    int32 Count = ReferenceAnimations.Num(); Ar << Count;
    if (Count < 0 || Count > 100000) { Ar.SetError(); return; }
    const auto Animation = [&](uint32& Id, TSharedPtr<FT5ReferenceAnimation>& State)
    {
        FString Rig, Project;
        if (Ar.IsSaving())
            if (const auto* Visuals = ReferenceVisuals.Find(Id))
                for (const auto& Visual : *Visuals)
                    if (!Visual.SkeletonPath.IsEmpty()) { Rig = Visual.SkeletonPath; Project = Visual.AnimationProject; break; }
        Ar << Id << Rig << Project << State->Serial << State->Parent << State->Start << State->Anchor << State->Pose;
        Ar << State->bLoaded << State->bMoving << State->bFinished;
        bool bRestore = true;
        if (Ar.IsLoading() && State->bLoaded)
        {
            const auto* Request = Papyrus->AnimationRequests().Find(Id);
            FString Error;
            bRestore = Request && LoadT5HkxAnimation(Request->Behavior, Request->Event, Request->Clip,
                [this](const FString& Path, TArray<uint8>& Bytes) { return Papyrus->ReadResource(Path, Bytes); },
                Error, Rig, Request->Speed, Request->Event == TEXT("T5Bow") || Request->Event == TEXT("T5Execution")
                    || Request->Event == TEXT("T5Shout") || Request->Event.Contains(TEXT("::")), false,
                State->Clock, Papyrus->MotionData(), Project);
            if (!bRestore)
                UE_LOG(LogTemp, Display, TEXT("T5_CHECKPOINT_ANIMATION_SKIP ref=%08X %s"), Id, *Error);
        }
        // Read the stored clock even if the actor's clip is no longer available.
        Ar << State->Clock.LastTravel << State->Clock.Fired;
        return bRestore;
    };
    if (Ar.IsLoading())
    {
        ReferenceAnimations.Reset();
        for (int32 I = 0; I < Count && !Ar.IsError(); ++I)
        {
            uint32 Id = 0;
            TSharedPtr<FT5ReferenceAnimation> State = MakeShared<FT5ReferenceAnimation>();
            if (Animation(Id, State)) ReferenceAnimations.Add(Id, State);
        }
    }
    else for (auto& [Id, State] : ReferenceAnimations) { uint32 Key = Id; Animation(Key, State); }
    Count = Faces.Num(); Ar << Count;
    if (Count < 0 || Count > 100000) { Ar.SetError(); return; }
    if (Ar.IsLoading())
    {
        Faces.Reset();
        for (int32 I = 0; I < Count && !Ar.IsError(); ++I)
        { uint32 Id = 0; FFaceState Face; Ar << Id << Face.Target << Face.bTracking << Face.Expression << Face.Strength; Faces.Add(Id, Face); }
    }
    else for (auto& [Id, Face] : Faces) { uint32 Key = Id; Ar << Key << Face.Target << Face.bTracking << Face.Expression << Face.Strength; }
    bool HasTitles = Titles.IsValid(); Ar << HasTitles;
    if (HasTitles)
    {
        if (Ar.IsLoading())
        {
            Titles = SNew(ST5Titles);
            if (auto* Viewport = GetWorld()->GetGameViewport()) Viewport->AddViewportWidgetContent(Titles.ToSharedRef(), 50);
        }
        Titles->SerializeCheckpoint(Ar, *Papyrus);
    }
    if (Ar.IsLoading()) CorpsePoses.Reset();
    if (Ar.IsSaving()) CaptureRagdollPoses();
    if (Ar.IsSaving() || !Ar.AtEnd())
    {
        int32 CorpseCount = CorpsePoses.Num();
        Ar << CorpseCount;
        if (CorpseCount < 0 || CorpseCount > 100000) { Ar.SetError(); return; }
        const auto Pose = [&](uint32 Id, FCorpsePose& Value) {
            Ar << Id << Value.Transform << Value.Bones;
            if (Ar.IsLoading()) CorpsePoses.Add(Id, MoveTemp(Value));
        };
        if (Ar.IsSaving())
            for (auto& [Id, Value] : CorpsePoses) Pose(Id, Value);
        else
            for (int32 Index = 0; Index < CorpseCount && !Ar.IsError(); ++Index)
            {
                FCorpsePose Value;
                Pose(0, Value);
            }
    }
    if (Ar.IsLoading()) ClearAnimationVisuals();
    if (Ar.IsSaving())
        for (const auto& [Id, Head] : DetachedHeads)
            if (const auto* Actor = Head.Get()) DetachedHeadTransforms.Add(Id, Actor->GetActorTransform());
    if (Ar.IsSaving() || !Ar.AtEnd()) Ar << DrawnAnimationObjects << DetachedHeadTransforms;
}

void AT5Scene::RestoreCheckpointHost()
{
    FMemoryReader Reader(PendingHostCheckpoint); Reader.ArMaxSerializeSize = MaxSaveBytes;
    SerializeCheckpoint(Reader);
    if (Reader.IsError() || Reader.Tell() != PendingHostCheckpoint.Num())
    { bLoadingFailed = true; SaveMessage(TEXT("Не удалось восстановить состояние сцены"), true); return; }
    // Older saves can be inside a state whose entry props were not imported.
    // Rebuild only presentation for that active clip; keep saved prop state and
    // leave the gameplay event cursor where it was.
    for (const auto& [Id, Animation] : ReferenceAnimations)
        if (Animation->bLoaded && Animation->Clock.LastTravel >= 0 && !DrawnAnimationObjects.Contains(Id)
            && Animation->Clock.Playback.ExitEvents.IsEmpty() && !Papyrus->WasKilled(Id))
            for (const auto& Event : Animation->Clock.Playback.EnterEvents)
                if (Event.StartsWith(TEXT("AnimObjDraw.")))
                    AnimationVisualEvent(Id, Event);
    // Rebuild the camera from original resources, preserving its saved simulation time.
    if (LastPlayerVehicle || (Papyrus->AnimationRequests().Contains(0x14)
        && (Papyrus->AnimationRequests().Find(0x14)->Event == TEXT("IdleExecutionerChop_Player")
            || Papyrus->AnimationRequests().Find(0x14)->Event == TEXT("IdleExecutionerAlduinReactionDeath")
            || Papyrus->AnimationRequests().Find(0x14)->Event.StartsWith(TEXT("Behavior37/")))))
        if (const auto* Request = Papyrus->AnimationRequests().Find(0x14))
        {
            PlayerCamera = MakeShared<FT5HkxCameraClip>(); FString Error;
            if (!LoadT5HkxPlayerCamera(Request->Behavior, Request->Event,
                [this](const FString& Path, TArray<uint8>& Bytes) { return Papyrus->ReadResource(Path, Bytes); }, *PlayerCamera, Error, Request->Clip))
                PlayerCamera.Reset();
        }
    PendingNativeCheckpoint.Reset(); PendingHostCheckpoint.Reset();
}

void AT5Scene::FinishCheckpointLoad()
{
    if (!bRestoringCheckpoint || bLoadingFailed || bSceneLoading || !PendingNativeCheckpoint.IsEmpty()
        || !Papyrus || LoadedSpawnCount != Papyrus->SpawnedCount()) return;
    if (Mechanics && !PendingPhysicsCheckpoint.IsEmpty())
    {
        FMemoryReader Reader(PendingPhysicsCheckpoint); Reader.ArMaxSerializeSize = MaxSaveBytes;
        Mechanics->SerializeCheckpoint(Reader); VisualPositions(Reader, ReferenceVisuals); PendingPhysicsCheckpoint.Reset();
        if (Reader.IsError()) { bLoadingFailed = true; SaveMessage(TEXT("Не удалось восстановить физику сохранения"), true); return; }
    }
    bRestoringCheckpoint = false;
    ApplyPapyrusState(); ApplyPapyrusWorld();
    if (!PendingArcheryCheckpoint.IsEmpty() && LoadedScene)
    {
        RuntimeArchery = MakeShared<FT5Archery>();
        FMemoryReader Reader(PendingArcheryCheckpoint); Reader.ArMaxSerializeSize = MaxSaveBytes;
        RuntimeArchery->SerializeCheckpoint(Reader, *LoadedScene, *Papyrus, ArcheryAssets,
            [this](uint32 Id) { return FindReferenceSkeleton(Id); });
        PendingArcheryCheckpoint.Reset();
        if (Reader.IsError()) { bLoadingFailed = true; SaveMessage(TEXT("Не удалось восстановить стрелы сохранения"), true); return; }
    }
    if (auto* Controller = GetWorld()->GetFirstPlayerController())
    {
        if (auto* Pawn = Controller->GetPawnOrSpectator(); Pawn && CheckpointPlayer)
        {
            if (auto* Movement = Pawn->GetMovementComponent()) Movement->StopMovementImmediately();
            Pawn->SetActorTransform(*CheckpointPlayer, false, nullptr, ETeleportType::TeleportPhysics);
            // Older checkpoints used a fixed 92-unit eye offset after dismount.
            // Normalize that legacy on-foot view without moving the player's feet.
            FTransform Authored; uint32 Parent=0;
            if (const auto Pose=Papyrus->ReferenceTransform(0x14,Authored,Parent); Pose && !Parent && !LastPlayerVehicle)
            {
                const auto Standing=Mechanics ? Mechanics->CorrectPose(0x14,0,*Pose) : *Pose;
                const FVector Feet=GetActorTransform().TransformPosition(Standing.GetLocation());
                const FVector View=Pawn->GetActorLocation();
                const float Scale=GetActorScale3D().Z;
                if (FVector::DistSquared2D(View,Feet)<FMath::Square(15.f)
                    && FMath::Abs((View.Z-Feet.Z)-92*T5CentimetersPerUnit*Scale)<5)
                {
                    Pawn->SetActorLocation(FVector(View.X,View.Y,Feet.Z+Papyrus->PlayerEyeHeight()*Scale),false,nullptr,ETeleportType::TeleportPhysics);
                    UE_LOG(LogTemp,Display,TEXT("T5_STANDING_CAMERA_RESTORED old=%.2f new=%.2f"),View.Z-Feet.Z,Papyrus->PlayerEyeHeight()*Scale);
                }
            }
        }
        Controller->SetControlRotation(CheckpointView);
    }
    CheckpointPlayer.Reset();
    bRestoringCheckpoint = bStartupPreparing = false;
    if (Locations.IsEmpty()) RequestCatalog([](bool) {});
    SaveMessage(TEXT("Игра загружена"));
    UE_LOG(LogTemp, Display, TEXT("T5_LOAD_OK time=%.6f location=%08X cell=%d,%d"),
        SimulationTime, ActiveLocation, RequestedCell.X, RequestedCell.Y);
}
