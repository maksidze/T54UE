#include "T5CharacterCreator.h"
#include "T5Scene.h"
#include "T5Bridge.h"
#include "T5Mechanics.h"
#include "T5Titles.h"
#include "T5MenuStyle.h"
#include "Async/Async.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/MorphTarget.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PawnMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Interfaces/IPluginManager.h"
#include "InputCoreTypes.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonSerializer.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSearchBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
    TSharedPtr<FJsonObject> Json(const FT5LoadResult& Data, const TCHAR* Path)
    {
        const auto Bytes = Data.Read(Path); if (Bytes.IsEmpty()) return {};
        FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
        TSharedPtr<FJsonObject> Object;
        FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(Text.Length(), Text.Get())), Object);
        return Object;
    }
    FString StringBuffer(const FT5LoadResult& Data, const TCHAR* Path)
    {
        auto Bytes = Data.Read(Path); if (Bytes.IsEmpty()) return {};
        FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num()); return FString(Text.Length(), Text.Get());
    }
    TSharedRef<SWidget> Label(const TSharedPtr<FT5UIResources>& Resources, const FString& Text, int Size = 14)
    {
        return SNew(STextBlock).Text(FText::FromString(Text)).Font(T5MenuFont(Resources, Size, TEXT("Regular")))
            .ColorAndOpacity(FLinearColor(.86f,.84f,.78f));
    }
    TSharedRef<SWidget> Button(const TSharedPtr<FT5UIResources>& Resources, const FString& Text, TFunction<void()> Action)
    {
        return SNew(SButton).ButtonStyle(T5MenuButtonStyle()).ContentPadding(FMargin(10,6))
            .OnClicked_Lambda([Action] { Action(); return FReply::Handled(); })[Label(Resources, Text)];
    }
    TSharedRef<SWidget> Cycle(const TSharedPtr<FT5UIResources>& Resources, const FString& Title, TFunction<FString()> Value, TFunction<void(int)> Change)
    {
        return SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(0,6,0,3)[Label(Resources, Title,11)]
            + SVerticalBox::Slot().AutoHeight()
            [SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth()[Button(Resources, TEXT("‹"), [Change] { Change(-1); })]
                + SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center).Padding(8,0)
                [SNew(STextBlock).Text_Lambda([Value] { return FText::FromString(Value()); }).AutoWrapText(true).Font(T5MenuFont(Resources, 14, TEXT("Regular")))]
                + SHorizontalBox::Slot().AutoWidth()[Button(Resources, TEXT("›"), [Change] { Change(1); })]];
    }
    TSharedRef<SWidget> Slider(const TSharedPtr<FT5UIResources>& Resources, const FSliderStyle* Style, const FString& Title, float Min, float Max,
        TFunction<float()> Get, TFunction<void(float)> Set)
    {
        return SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().Padding(0,7,0,3)
            [SNew(STextBlock).Text_Lambda([Title, Get] { return FText::FromString(FString::Printf(TEXT("%s   %.2f"), *Title, Get())); }).Font(T5MenuFont(Resources, 14, TEXT("Regular")))]
            + SVerticalBox::Slot().AutoHeight().Padding(0,3)
            [SNew(SSlider).Style(Style).MinValue(Min).MaxValue(Max).Value_Lambda([Get] { return Get(); })
                .OnValueChanged_Lambda([Set](float Value) { Set(Value); })];
    }
    FString RaceName(const FString& Id)
    {
        static const TMap<FString,FString> Names = {{TEXT("NordRace"),TEXT("Норд")},{TEXT("BretonRace"),TEXT("Бретонец")},
            {TEXT("ImperialRace"),TEXT("Имперец")},{TEXT("RedguardRace"),TEXT("Редгард")},{TEXT("OrcRace"),TEXT("Орк")},
            {TEXT("HighElfRace"),TEXT("Альтмер")},{TEXT("WoodElfRace"),TEXT("Босмер")},{TEXT("DarkElfRace"),TEXT("Данмер")},
            {TEXT("KhajiitRace"),TEXT("Каджит")},{TEXT("ArgonianRace"),TEXT("Аргонианин")}};
        const auto* Name = Names.Find(Id); return Name ? *Name : Id;
    }
}

FT5CharacterCreator::~FT5CharacterCreator() { Close(); }

bool FT5CharacterCreator::Open(AT5Scene& Scene, bool Required)
{
    auto* Controller = Scene.GetWorld()->GetFirstPlayerController(); auto* Viewport = Scene.GetWorld()->GetGameViewport();
    if (!Controller || !Viewport) return false;
    Owner = &Scene; Runtime = Scene.Papyrus; bRequired = Required;
    FTransform Authored; uint32 Vehicle=0;
    const auto LogicalPlayer=Runtime->ReferenceTransform(0x14,Authored,Vehicle);
    if (Vehicle)
    { Scene.SaveMessage(TEXT("Создание персонажа доступно после выхода из повозки")); return false; }
    FString Data, Plugins;
    if (!Scene.ResolvePaths(Data, Plugins, BridgePath)) return false;
    ModPath = IPluginManager::Get().FindPlugin(TEXT("T5Data"))->GetBaseDir() / TEXT("Resources/RaceMenu");
    auto Catalog = Runtime->CharacterCatalog();
    auto Object = Catalog->Error.IsEmpty() ? Json(*Catalog, TEXT("catalog.json")) : nullptr;
    if (!Object) { Scene.SaveMessage(Catalog->Error, true); return false; }
    for (const auto& Item : Object->GetArrayField(TEXT("races"))) Races.Add(Item->AsObject());
    for (const auto& Item : Object->GetArrayField(TEXT("parts"))) HeadParts.Add(Item->AsObject());
    if (Races.IsEmpty()) { Scene.SaveMessage(TEXT("Нет доступных рас"), true); return false; }
    auto Bytes = Catalog->Read(TEXT("choice.bin"));
    if (Bytes.Num() == sizeof(Choice))
    {
        FMemory::Memcpy(&Choice, Bytes.GetData(), sizeof(Choice)); Name = StringBuffer(*Catalog,TEXT("name"));
        if (auto Saved = Json(*Catalog,TEXT("morphs.json")))
            for (auto& [Key,Value] : Saved->Values) Morphs.Add(FName(*Key), Value->AsNumber());
    }
    if (!Races.ContainsByPredicate([&](const auto& R) { return uint32(R->GetNumberField(TEXT("id"))) == Choice.Race; })) Choice.Race = Races[0]->GetNumberField(TEXT("id"));
    PreviousView = Controller->GetViewTarget(); bPreviousCursor = Controller->bShowMouseCursor;
    FRotator InitialView;
    Controller->GetPlayerViewPoint(InitialCameraPosition,InitialView);
    InitialCameraRotation=InitialView.Quaternion();
    if (Controller->PlayerCameraManager) InitialFieldOfView=Controller->PlayerCameraManager->GetFOVAngle();
    FVector Feet; FRotator Heading=Controller->GetControlRotation();
    const FTransform Origin=Scene.GetActorTransform();
    if (Required && LogicalPlayer)
    {
        const auto Pose=Scene.Mechanics ? Scene.Mechanics->CorrectPose(0x14,0,*LogicalPlayer) : *LogicalPlayer;
        Feet=Origin.TransformPosition(Pose.GetLocation());
        Heading=(Origin.GetRotation()*Pose.GetRotation()).Rotator();
    }
    else if (auto* Pawn=Controller->GetPawnOrSpectator())
        Feet=Pawn->GetActorLocation()-FVector(0,0,Runtime->PlayerEyeHeight()*Origin.GetScale3D().Z);
    else if (LogicalPlayer) Feet=Origin.TransformPosition(LogicalPlayer->GetLocation());
    else return false;
    FacingYaw=Heading.Yaw;
    // Native character buffers have a 180-degree portrait placement. Cancel it
    // here so the standing avatar faces the player's authored world heading.
    WorldPlacement=FTransform(FRotator(0,FacingYaw+180,0),Feet,Origin.GetScale3D());
    FActorSpawnParameters Spawn; Spawn.ObjectFlags = RF_Transient;
    auto* Cam = Scene.GetWorld()->SpawnActor<ACameraActor>(FVector::ZeroVector, FRotator::ZeroRotator, Spawn);
    if (!Cam) return false;
    Camera = Cam; Cam->SetActorLocationAndRotation(InitialCameraPosition,InitialCameraRotation);
    Cam->GetCameraComponent()->SetFieldOfView(InitialFieldOfView);
    Cam->GetCameraComponent()->bConstrainAspectRatio = false;
    Cam->GetCameraComponent()->PostProcessBlendWeight=0; // Inherit the actual cell's lighting and exposure.
    bWasWorldPaused=UGameplayStatics::IsGamePaused(Scene.GetWorld());
    bWasSceneTickableWhenPaused=Scene.PrimaryActorTick.bTickEvenWhenPaused;
    bActivated=true;
    Scene.SetTickableWhenPaused(true);
    if (Scene.PapyrusErrors) Scene.PapyrusErrors->SetVisibility(EVisibility::Collapsed);
    if (Scene.Titles) Scene.Titles->SetVisibility(EVisibility::Collapsed);
    if (GEngine) { bPreviousMessages = GEngine->bEnableOnScreenDebugMessages; GEngine->bEnableOnScreenDebugMessages = false; }
    if (auto* Pawn = Controller->GetPawnOrSpectator()) if (auto* Move = Pawn->GetMovementComponent()) Move->StopMovementImmediately();
    if (Scene.Mechanics) Scene.Mechanics->SetPaused(true);
    if (auto* Parts=Scene.ReferenceVisuals.Find(0x14))
        for (auto& Part : *Parts) if (auto* Component=Part.Component.Get())
        { HiddenPlayerParts.Add({Component,Component->IsVisible()}); Component->SetVisibility(false); }
    // Capture only originally visible meshes. Group each reference's parts so
    // an obstructing NPC disappears together with its hair and equipment.
    TSet<UMeshComponent*> Grouped;
    auto AddOccluder=[&](const TArray<UMeshComponent*>& Parts)
    {
        FViewOccluder Group;
        for (auto* Mesh : Parts)
            if (Mesh && Mesh->IsVisible() && !Mesh->bHiddenInGame && !Grouped.Contains(Mesh))
            { Group.Parts.Add(Mesh); Group.Bounds+=Mesh->Bounds.GetBox(); Grouped.Add(Mesh); }
        if (!Group.Parts.IsEmpty()) ViewOccluders.Add(MoveTemp(Group));
    };
    for (const auto& [Id,Parts] : Scene.ReferenceVisuals)
        if (Id!=0x14)
        {
            TArray<UMeshComponent*> Meshes;
            for (const auto& Part : Parts) if (auto* Mesh=Cast<UMeshComponent>(Part.Component.Get())) Meshes.Add(Mesh);
            AddOccluder(Meshes);
        }
    if (IsValid(Scene.LoadedScene))
    {
        TInlineComponentArray<UMeshComponent*> Meshes(Scene.LoadedScene);
        for (auto* Mesh : Meshes) AddOccluder({Mesh});
    }
    if (!bWasWorldPaused && !UGameplayStatics::SetGamePaused(Scene.GetWorld(),true))
    { Scene.SaveMessage(TEXT("Не удалось приостановить игровой мир"),true); return false; }
    Widget = SNew(ST5RaceMenu, this); Viewport->AddViewportWidgetContent(Widget.ToSharedRef(), 200);
    FInputModeUIOnly Mode; Mode.SetWidgetToFocus(Widget); Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
    Controller->SetInputMode(Mode); Controller->bShowMouseCursor = true;
    Controller->SetViewTarget(Cam); UpdateCamera();
    Status = TEXT("Подготовка персонажа…");
    UE_LOG(LogTemp,Display,TEXT("T5_RACE_MENU_OPEN required=%d races=%d parts=%d worldPaused=%d feet=%s yaw=%.2f mods=%s"),
        bRequired,Races.Num(),HeadParts.Num(),UGameplayStatics::IsGamePaused(Scene.GetWorld()),*Feet.ToCompactString(),FacingYaw,*ModPath);
    return true;
}

void FT5CharacterCreator::Changed(bool Rebuild)
{
    bReady = false;
    if (Rebuild) { bDirty = true; RebuildDue = FPlatformTime::Seconds() + .25; }
    else { ApplyMorphs(); bReady = !bDirty && !Pending.IsValid(); }
}

void FT5CharacterCreator::Tick()
{
    if (bClosed) return;
    if (Pending.IsValid() && Pending.IsReady())
    {
        auto Data = Pending.Consume();
        if (FMemory::Memcmp(&Requested, &Choice, sizeof(Choice)) != 0) { bDirty = true; return; }
        FString Error = Data->Error;
        if (Error.IsEmpty())
        {
            if (auto* Scene = Owner.Get())
                if (auto* Actor = Scene->BuildCharacterPreview(*Data, Error))
                {
                    Actor->SetActorTransform(WorldPlacement);
                    // Model-space normal maps were authored in the native placement;
                    // carry their basis into the actor's actual world orientation.
                    TSet<UMaterialInstanceDynamic*> Adjusted;
                    TInlineComponentArray<USkeletalMeshComponent*> NewMeshes(Actor);
                    for (auto* Mesh : NewMeshes)
                        for (int I=0;I<Mesh->GetNumMaterials();++I)
                            if (auto* MID=Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(I)); MID && !Adjusted.Contains(MID))
                            {
                                Adjusted.Add(MID); float HasBasis=0;
                                if (!MID->GetScalarParameterValue(FMaterialParameterInfo(TEXT("HasBasis")),HasBasis) || HasBasis<.5f) continue;
                                for (int Axis=0;Axis<3;++Axis)
                                {
                                    const FName Parameter(*FString::Printf(TEXT("Axis%d"),Axis)); FLinearColor Value;
                                    if (MID->GetVectorParameterValue(FMaterialParameterInfo(Parameter),Value))
                                    {
                                        const FVector Basis=WorldPlacement.GetRotation().RotateVector(FVector(Value.R,Value.G,Value.B));
                                        MID->SetVectorParameterValue(Parameter,FLinearColor(Basis.X,Basis.Y,Basis.Z,Value.A));
                                    }
                                }
                            }
                    if (Preview.IsValid()) Preview->Destroy(); Preview = Actor;
                    Actor->SetActorHiddenInGame(false);
                    if (PreviewRevision==0) CameraTransitionStarted=FPlatformTime::Seconds();
                    AvailableMorphs.Reset(); TInlineComponentArray<USkeletalMeshComponent*> Meshes(Actor);
                    for (auto* Mesh : Meshes)
                        for (const auto& Morph : Mesh->GetSkeletalMeshAsset()->GetMorphTargets())
                            if (Morph->GetName().StartsWith(TEXT("CharGen:"))) AvailableMorphs.AddUnique(Morph->GetFName());
                    AvailableMorphs.Sort([](FName A,FName B) { return A.LexicalLess(B); });
                    ApplyMorphs(); UpdateCamera(); RefreshMorphList(); bReady = true; ++PreviewRevision;
                    Status = FString::Printf(TEXT("%s · %d морфов лица"), *RaceLabel(), AvailableMorphs.Num());
                    UE_LOG(LogTemp,Display,TEXT("T5_CHARACTER_PREVIEW revision=%d race=%08X female=%d morphs=%d"), PreviewRevision,Choice.Race,Choice.Female,AvailableMorphs.Num());
                }
        }
        if (!Error.IsEmpty()) { Status = Error; bReady = false; }
    }
    if (bDirty && !Pending.IsValid() && FPlatformTime::Seconds() >= RebuildDue)
    {
        bDirty = false; Requested = Choice; bReady = false; Status = TEXT("Обновление внешности…");
        auto RuntimeCopy = Runtime; const auto Snapshot = Choice; const auto Bridge = BridgePath, Mods = ModPath;
        Pending = Async(EAsyncExecution::ThreadPool,[RuntimeCopy,Snapshot,Bridge,Mods] { return RuntimeCopy->CharacterPreview(Snapshot,Bridge,Mods); });
    }
    UpdateCamera();
}

void FT5CharacterCreator::ApplyMorphs()
{
    if (!Preview.IsValid()) return;
    TInlineComponentArray<USkeletalMeshComponent*> Meshes(Preview.Get());
    for (auto* Mesh : Meshes)
    {
        for (auto Morph : AvailableMorphs)
            Mesh->SetMorphTarget(Morph, MorphValue(Morph));
        // World time is paused; evaluate the standing pose and edited morphs
        // explicitly without advancing the animation or its annotations.
        Mesh->TickAnimation(0,false); Mesh->RefreshBoneTransforms(); Mesh->UpdateBounds();
    }
}
float FT5CharacterCreator::MorphValue(FName Key) const
{
    if (const auto* Value = Morphs.Find(Key)) return *Value;
    for (const auto& Race : Races)
        if (uint32(Race->GetNumberField(TEXT("id"))) == Choice.Race
            && Key == FName(*(TEXT("CharGen:") + Race->GetStringField(TEXT("name"))))) return 1;
    return 0;
}

void FT5CharacterCreator::Close()
{
    if (bClosed) return; bClosed = true;
    if (auto* Scene = Owner.Get(); Scene && bActivated)
    {
        if (auto* Viewport = Scene->GetWorld()->GetGameViewport(); Viewport && Widget) Viewport->RemoveViewportWidgetContent(Widget.ToSharedRef());
        if (auto* Controller = Scene->GetWorld()->GetFirstPlayerController())
        {
            if (PreviousView.IsValid()) Controller->SetViewTarget(PreviousView.Get());
            if (bAccepted)
                if (auto* Pawn=Controller->GetPawnOrSpectator())
                    Pawn->SetActorLocation(WorldPlacement.GetLocation()+FVector(0,0,Runtime->PlayerEyeHeight()*Scene->GetActorScale3D().Z),false,nullptr,ETeleportType::TeleportPhysics);
            Controller->SetInputMode(FInputModeGameOnly()); Controller->bShowMouseCursor = bPreviousCursor;
            if (Controller->PlayerCameraManager) Controller->PlayerCameraManager->UpdateCamera(0);
        }
        if (Scene->Mechanics) Scene->Mechanics->SetPaused(false);
        if (Scene->PapyrusErrors) Scene->PapyrusErrors->SetVisibility(EVisibility::HitTestInvisible);
        if (Scene->Titles) Scene->Titles->SetVisibility(EVisibility::HitTestInvisible);
        if (GEngine) GEngine->bEnableOnScreenDebugMessages = bPreviousMessages;
        for (auto& [Component,Visible] : HiddenPlayerParts) if (Component.IsValid()) Component->SetVisibility(Visible);
        for (auto& Group : ViewOccluders) if (Group.bHidden)
            for (auto& Part : Group.Parts) if (Part.IsValid()) Part->SetVisibility(true);
        ViewOccluders.Reset();
        if (!bWasWorldPaused) UGameplayStatics::SetGamePaused(Scene->GetWorld(),false);
        Scene->SetTickableWhenPaused(bWasSceneTickableWhenPaused);
    }
    if (Preview.IsValid()) Preview->Destroy(); if (Camera.IsValid()) Camera->Destroy();
    Preview.Reset(); Camera.Reset(); Widget.Reset();
}

FString FT5CharacterCreator::MorphJson() const
{
    auto Object = MakeShared<FJsonObject>();
    for (const auto& [Key,Value] : Morphs) Object->SetNumberField(Key.ToString(),Value);
    FString Text; FJsonSerializer::Serialize(Object,TJsonWriterFactory<>::Create(&Text)); return Text;
}
bool FT5CharacterCreator::Accept()
{
    Name.TrimStartAndEndInline();
    if (!bReady || Pending.IsValid() || bDirty || Name.IsEmpty() || FTCHARToUTF8(*Name).Length() > 128) { Status = TEXT("Введите имя и дождитесь предпросмотра"); return false; }
    if (!Runtime->CommitCharacter(Choice,Name,MorphJson())) { Status = TEXT("Не удалось применить персонажа. См. T5Papyrus.log"); return false; }
    bAccepted=true;
    // Keep this object alive until the Slate event returns; the owner releases it on the next frame.
    Close(); return true;
}
void FT5CharacterCreator::Cancel() { if (!bRequired) Close(); }

void FT5CharacterCreator::CycleRace(int Direction)
{
    int32 Index = Races.IndexOfByPredicate([&](const auto& R) { return uint32(R->GetNumberField(TEXT("id"))) == Choice.Race; });
    Index = (Index + Direction + Races.Num()) % Races.Num(); Choice.Race = Races[Index]->GetNumberField(TEXT("id"));
    FMemory::Memzero(Choice.Parts); Choice.Preset = 0; Morphs.Reset(); Changed();
}
FString FT5CharacterCreator::RaceLabel() const
{
    for (auto& Race : Races) if (uint32(Race->GetNumberField(TEXT("id"))) == Choice.Race) return RaceName(Race->GetStringField(TEXT("name")));
    return TEXT("Раса");
}
TArray<TSharedPtr<FJsonObject>> FT5CharacterCreator::Parts(int Type) const
{
    TArray<TSharedPtr<FJsonObject>> Result;
    for (auto& Part : HeadParts)
        if (Part->GetIntegerField(TEXT("type")) == Type)
            for (auto& Race : Part->GetArrayField(TEXT("races")))
            {
                auto& Pair = Race->AsArray();
                if (uint32(Pair[0]->AsNumber()) == Choice.Race && uint32(Pair[1]->AsNumber()) == Choice.Female) { Result.Add(Part); break; }
            }
    return Result;
}
FString FT5CharacterCreator::PartLabel(int Type) const
{
    for (auto& Part : HeadParts) if (uint32(Part->GetNumberField(TEXT("id"))) == Choice.Parts[Type]) return Part->GetStringField(TEXT("name"));
    return TEXT("По умолчанию");
}
void FT5CharacterCreator::CyclePart(int Type,int Direction)
{
    auto Items = Parts(Type); if (Items.IsEmpty()) return;
    int32 Index = Items.IndexOfByPredicate([&](auto& Part) { return uint32(Part->GetNumberField(TEXT("id"))) == Choice.Parts[Type]; });
    // Include the race/preset default as an explicit option.
    Index = ((Index + 1 + Direction + Items.Num() + 1) % (Items.Num() + 1)) - 1;
    Choice.Parts[Type] = Index < 0 ? 0 : uint32(Items[Index]->GetNumberField(TEXT("id"))); Changed();
}
TArray<uint32> FT5CharacterCreator::Presets() const
{
    TArray<uint32> Result;
    for (auto& Race : Races) if (uint32(Race->GetNumberField(TEXT("id"))) == Choice.Race)
        for (auto& Item : Race->GetArrayField(TEXT("presets")))
            if (uint32(Item->AsArray()[1]->AsNumber()) == Choice.Female) Result.Add(Item->AsArray()[0]->AsNumber());
    return Result;
}
void FT5CharacterCreator::CyclePreset(int Direction)
{
    auto Items = Presets(); if (Items.IsEmpty()) return;
    int32 Index = Items.IndexOfByKey(Choice.Preset);
    Index = ((Index + 1 + Direction + Items.Num() + 1) % (Items.Num() + 1)) - 1;
    Choice.Preset = Index < 0 ? 0 : Items[Index]; FMemory::Memzero(Choice.Parts); Morphs.Reset(); Changed();
}

void FT5CharacterCreator::UpdateCamera()
{
    auto* Scene=Owner.Get();
    if (!Scene || !Camera.IsValid() || !Preview.IsValid()) return;
    const FVector Feet=WorldPlacement.GetLocation();
    FVector Head=Feet+FVector(0,0,Runtime->PlayerEyeHeight()*WorldPlacement.GetScale3D().Z);
    TInlineComponentArray<USkeletalMeshComponent*> Meshes(Preview.Get());
    for (auto* Mesh : Meshes)
        if (Mesh->GetBoneIndex(TEXT("NPC Head [Head]"))!=INDEX_NONE)
        { Head=Mesh->GetBoneLocation(TEXT("NPC Head [Head]")); break; }
    const float FocusBelowHead=FMath::GetMappedRangeValueClamped(FVector2D(110,600),FVector2D(-2,65),Distance);
    const FVector Target=Head-FVector(0,0,FocusBelowHead);
    const FVector Forward=FRotator(0,FacingYaw+Angle,0).Vector();
    FVector Position=Target+Forward*Distance+FVector(0,0,20);
    const float Linear=FMath::Clamp(float((FPlatformTime::Seconds()-CameraTransitionStarted)/.4),0.f,1.f);
    const float Blend=Linear*Linear*(3-2*Linear);
    const FQuat Rotation=(Target-Position).Rotation().Quaternion();
    Camera->SetActorLocationAndRotation(FMath::Lerp(InitialCameraPosition,Position,Blend),FQuat::Slerp(InitialCameraRotation,Rotation,Blend));
    UpdateOcclusion(Camera->GetActorLocation(),Head);
    if (auto* Cam=Cast<ACameraActor>(Camera.Get())) Cam->GetCameraComponent()->SetFieldOfView(FMath::Lerp(InitialFieldOfView,42.f,Blend));
    // A paused player controller may only run its minimal tick. Refresh the
    // camera cache explicitly so Slate orbit/zoom remains interactive on pause.
    if (auto* Controller=Scene->GetWorld()->GetFirstPlayerController(); Controller && Controller->PlayerCameraManager)
    {
        Controller->PlayerCameraManager->UpdateCamera(0);
    }
}
void FT5CharacterCreator::UpdateOcclusion(const FVector& CameraPosition, const FVector& Head)
{
    const FVector Feet=WorldPlacement.GetLocation();
    const float Height=FMath::Max(Head.Z-Feet.Z,100.f);
    const FVector Side=FVector::CrossProduct((Head-CameraPosition).GetSafeNormal(),FVector::UpVector).GetSafeNormal();
    TArray<FVector,TInlineAllocator<15>> Targets;
    FBox ViewBounds(CameraPosition,CameraPosition);
    for (float Level : {.1f,.35f,.6f,.85f,1.03f})
        for (float Offset : {-1.f,0.f,1.f})
        {
            const FVector Target=Feet+FVector(0,0,Height*Level)+Side*(Offset*Height*.2f);
            Targets.Add(Target); ViewBounds+=Target;
        }
    for (auto& Group : ViewOccluders)
    {
        bool Hide=false;
        if (Group.Bounds.Intersect(ViewBounds))
            for (const FVector& Target : Targets)
            {
                const FVector End=Target+(CameraPosition-Target).GetSafeNormal()*5;
                if (!FMath::LineBoxIntersection(Group.Bounds,CameraPosition,End,End-CameraPosition)) continue;
                for (const auto& Part : Group.Parts) if (auto* Mesh=Part.Get())
                {
                    // Landscape tiles span large height ranges: their bounds
                    // can cover open air. Use their actual collision triangles
                    // so a clear view never removes the ground below the avatar.
                    if (Mesh->ComponentHasTag(TEXT("T5Terrain")))
                    {
                        FHitResult Hit;
                        FCollisionQueryParams Trace(SCENE_QUERY_STAT(T5RaceMenuOcclusion),true);
                        Hide=Mesh->LineTraceComponent(Hit,CameraPosition,End,Trace);
                    }
                    else Hide=FMath::LineBoxIntersection(Mesh->Bounds.GetBox(),CameraPosition,End,End-CameraPosition);
                    if (Hide) break;
                }
                if (Hide) break;
            }
        if (Hide==Group.bHidden) continue;
        for (auto& Part : Group.Parts) if (Part.IsValid()) Part->SetVisibility(!Hide);
        Group.bHidden=Hide;
    }
}
void FT5CharacterCreator::Orbit(float Degrees) { Angle += Degrees; UpdateCamera(); }
void FT5CharacterCreator::Zoom(float Amount) { Distance = FMath::Clamp(Distance + Amount,110.f,650.f); UpdateCamera(); }

void FT5CharacterCreator::RefreshMorphList()
{
    if (!MorphList) return; MorphList->ClearChildren();
    auto* Scene = Owner.Get();
    const auto Resources = Scene ? Scene->UIResources : nullptr;
    const auto* Style = Resources ? Resources->GetRaceSliderStyle()
        : &FCoreStyle::Get().GetWidgetStyle<FSliderStyle>(TEXT("Slider"));
    for (auto Key : AvailableMorphs)
    {
        const FString Text = Key.ToString().RightChop(8);
        if (!Query.IsEmpty() && !Text.Contains(Query)) continue;
        MorphList->AddSlot().AutoHeight().Padding(0,0,0,5)
            [Slider(Resources, Style,Text,-1,1,[this,Key] { return MorphValue(Key); },[this,Key](float V) { Morphs.Add(Key,V); ApplyMorphs(); })];
    }
    if (AvailableMorphs.IsEmpty()) MorphList->AddSlot().AutoHeight()[Label(Resources, TEXT("Для выбранной головы нет доступных TRI-морфов."))];
}

void FT5CharacterCreator::SavePreset()
{
    if (!bReady) { Status = TEXT("Дождитесь предпросмотра"); return; }
    auto Object = MakeShared<FJsonObject>(); Object->SetNumberField(TEXT("version"),1); Object->SetStringField(TEXT("name"),Name);
    Object->SetStringField(TEXT("choice"),FString::FromHexBlob(reinterpret_cast<const uint8*>(&Choice),sizeof(Choice)));
    Object->SetStringField(TEXT("morphs"),MorphJson());
    FString Text; FJsonSerializer::Serialize(Object,TJsonWriterFactory<>::Create(&Text));
    const FString Directory = FPaths::ProjectSavedDir() / TEXT("RaceMenu/Presets"); IFileManager::Get().MakeDirectory(*Directory,true);
    const FString Path = Directory / TEXT("Character.json"), Temporary = Path + TEXT(".tmp");
    const bool OK = FFileHelper::SaveStringToFile(Text,*Temporary,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
        && IFileManager::Get().Move(*Path,*Temporary,true,true);
    Status = OK ? TEXT("Пресет сохранён: Saved/RaceMenu/Presets/Character.json") : TEXT("Не удалось записать пресет");
}
void FT5CharacterCreator::LoadPreset()
{
    FString Text; TSharedPtr<FJsonObject> Object;
    if (!FFileHelper::LoadFileToString(Text,*(FPaths::ProjectSavedDir()/TEXT("RaceMenu/Presets/Character.json")))
        || Text.Len() > 131072 || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Object) || !Object
        || Object->GetIntegerField(TEXT("version")) != 1) { Status = TEXT("Пресет не найден или повреждён"); return; }
    FString Hex,NewName,Values;
    if (!Object->TryGetStringField(TEXT("choice"),Hex) || Hex.Len() != sizeof(Choice)*2
        || !Object->TryGetStringField(TEXT("name"),NewName) || !Object->TryGetStringField(TEXT("morphs"),Values)) { Status = TEXT("Некорректный пресет"); return; }
    FT5CharacterChoice NewChoice; TSharedPtr<FJsonObject> NewMorphs;
    if (!FString::ToHexBlob(Hex,reinterpret_cast<uint8*>(&NewChoice),sizeof(NewChoice))
        || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Values),NewMorphs) || !NewMorphs) { Status = TEXT("Некорректный пресет"); return; }
    TMap<FName,float> NewValues;
    for (auto& [Key,Value] : NewMorphs->Values)
    {
        double Number = 0;
        if (!FString(Key).StartsWith(TEXT("CharGen:")) || !Value->TryGetNumber(Number) || !FMath::IsFinite(Number) || Number < -1 || Number > 1) { Status = TEXT("Некорректный морф пресета"); return; }
        NewValues.Add(FName(*Key),Number);
    }
    Choice = NewChoice; Name = NewName; Morphs = MoveTemp(NewValues); Changed();
}

TSharedRef<SWidget> FT5CharacterCreator::MakePanel()
{
    auto* Scene = Owner.Get();
    const auto Resources = Scene ? Scene->UIResources : nullptr;
    const auto* Style = Resources ? Resources->GetRaceSliderStyle()
        : &FCoreStyle::Get().GetWidgetStyle<FSliderStyle>(TEXT("Slider"));
    auto Left = SNew(SVerticalBox);
    Left->AddSlot().AutoHeight()[Label(Resources, TEXT("ПЕРСОНАЖ"),18)];
    Left->AddSlot().AutoHeight().Padding(0,10)[SNew(SEditableTextBox).Text_Lambda([this] { return FText::FromString(Name); })
        .Font(T5MenuFont(Resources, 14, TEXT("Regular")))
        .HintText(FText::FromString(TEXT("Имя персонажа"))).OnTextChanged_Lambda([this](const FText& Text) { Name=Text.ToString().Left(64); })];
    Left->AddSlot().AutoHeight()[Cycle(Resources, TEXT("Раса"),[this] { return RaceLabel(); },[this](int D) { CycleRace(D); })];
    Left->AddSlot().AutoHeight()[Cycle(Resources, TEXT("Пол"),[this] { return Choice.Female ? TEXT("Женский") : TEXT("Мужской"); },[this](int) {
        Choice.Female = !Choice.Female; Choice.Preset=0; FMemory::Memzero(Choice.Parts); Morphs.Reset(); Changed(); })];
    Left->AddSlot().AutoHeight()[Cycle(Resources, TEXT("Набор частей головы"),[this] { return Choice.Preset ? FString::Printf(TEXT("%d / %d"),Presets().IndexOfByKey(Choice.Preset)+1,Presets().Num()) : TEXT("По умолчанию"); },[this](int D) { CyclePreset(D); })];
    const TPair<int,FString> Types[] = {{1,TEXT("Голова")},{2,TEXT("Глаза")},{3,TEXT("Волосы")},{4,TEXT("Борода")},{6,TEXT("Брови")}};
    for (auto Type : Types) Left->AddSlot().AutoHeight()[Cycle(Resources, Type.Value,[this,Type] { return PartLabel(Type.Key); },[this,Type](int D) { CyclePart(Type.Key,D); })];
    Left->AddSlot().AutoHeight()[Slider(Resources, Style,TEXT("Вес"),0,100,[this] { return Choice.Weight; },[this](float V) { Choice.Weight=V; Changed(); })];
    Left->AddSlot().AutoHeight()[Slider(Resources, Style,TEXT("Рост"),.8f,1.2f,[this] { return Choice.Height; },[this](float V) { Choice.Height=V; Changed(); })];
    const TCHAR* Channels[] = {TEXT("Кожа · красный"),TEXT("Кожа · зелёный"),TEXT("Кожа · синий")};
    for (int I=0;I<3;++I) Left->AddSlot().AutoHeight()[Slider(Resources, Style,Channels[I],0,1,[this,I] { return Choice.Skin[I]; },[this,I](float V) { Choice.Skin[I]=V; Changed(); })];
    auto Right = SNew(SVerticalBox)
        + SVerticalBox::Slot().AutoHeight()[Label(Resources, TEXT("МОРФЫ ЛИЦА"),18)]
        + SVerticalBox::Slot().AutoHeight().Padding(0,10)[SNew(SSearchBox).HintText(FText::FromString(TEXT("Поиск морфа")))
            .OnTextChanged_Lambda([this](const FText& Text) { Query=Text.ToString(); RefreshMorphList(); })]
        + SVerticalBox::Slot().FillHeight(1)[SNew(SScrollBox)+SScrollBox::Slot()[SAssignNew(MorphList,SVerticalBox)]]
        + SVerticalBox::Slot().AutoHeight().Padding(0,8)[Button(Resources, TEXT("Сбросить морфы"),[this] { Morphs.Reset(); ApplyMorphs(); })]
        + SVerticalBox::Slot().AutoHeight()[Button(Resources, TEXT("Сохранить пресет"),[this] { SavePreset(); })]
        + SVerticalBox::Slot().AutoHeight().Padding(0,5)[Button(Resources, TEXT("Загрузить пресет"),[this] { LoadPreset(); })];
    const auto Card = [Resources](TSharedRef<SWidget> Content) { return SNew(SBorder).Padding(12)
        .BorderImage(T5MenuArt(Resources, TEXT("RacePanel")))[T5MenuContrast(Content, .58f)]; };
    return SNew(SOverlay)
        + SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Top).Padding(0,22)
        [SNew(SBorder).Padding(FMargin(46, 9)).BorderImage(T5MenuArt(Resources, TEXT("RaceHeader")))
            [Label(Resources, TEXT("СОЗДАНИЕ ПЕРСОНАЖА"),22)]]
        + SOverlay::Slot().HAlign(HAlign_Left).Padding(24,80,0,110)
        [SNew(SBox).WidthOverride(310)[Card(SNew(SScrollBox)+SScrollBox::Slot()[Left])]]
        + SOverlay::Slot().HAlign(HAlign_Right).Padding(0,80,24,110)
        [SNew(SBox).WidthOverride(290)[Card(Right)]]
        + SOverlay::Slot().HAlign(HAlign_Center).VAlign(VAlign_Bottom).Padding(0,0,0,130)
        [SNew(SHorizontalBox)
            +SHorizontalBox::Slot().AutoWidth()[Button(Resources, TEXT("↶"),[this] { Orbit(-20); })]
            +SHorizontalBox::Slot().AutoWidth()[Button(Resources, TEXT("−"),[this] { Zoom(25); })]
            +SHorizontalBox::Slot().AutoWidth()[Button(Resources, TEXT("+"),[this] { Zoom(-25); })]
            +SHorizontalBox::Slot().AutoWidth()[Button(Resources, TEXT("↷"),[this] { Orbit(20); })]]
        + SOverlay::Slot().VAlign(VAlign_Bottom)
        [T5MenuBar(Resources, TEXT("RaceBottom"), SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center)
            [SNew(STextBlock).Text_Lambda([this] { return FText::FromString(Status); }).AutoWrapText(true).Font(T5MenuFont(Resources, 14, TEXT("Regular")))]
            + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Center).Padding(0,8,0,0)
            [SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth()[SNew(SButton).ButtonStyle(T5MenuButtonStyle()).IsEnabled_Lambda([this] { return bReady && !Name.TrimStartAndEnd().IsEmpty(); })
                    .ContentPadding(FMargin(24,8)).OnClicked_Lambda([this] { Accept(); return FReply::Handled(); })[Label(Resources, TEXT("Готово · продолжить"),15)]]
                + SHorizontalBox::Slot().AutoWidth().Padding(12,0)[SNew(SButton).ButtonStyle(T5MenuButtonStyle()).Visibility(bRequired ? EVisibility::Collapsed : EVisibility::Visible)
                    .OnClicked_Lambda([this] { Cancel(); return FReply::Handled(); })[Label(Resources, TEXT("Отмена"))]]])];
}
void ST5RaceMenu::Construct(const FArguments&,FT5CharacterCreator* Creator) { Model=Creator; ChildSlot[Model->MakePanel()]; }
FReply ST5RaceMenu::OnKeyDown(const FGeometry&,const FKeyEvent& Event)
{
    if (Event.GetKey()==EKeys::Escape) { Model->Cancel(); return FReply::Handled(); }
    if (Event.GetKey()==EKeys::Left) { Model->Orbit(-10); return FReply::Handled(); }
    if (Event.GetKey()==EKeys::Right) { Model->Orbit(10); return FReply::Handled(); }
    return FReply::Handled();
}
