#include "T5Scene.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "T5Bridge.h"
#include "T5Coordinates.h"
#include "T5Mechanics.h"
#include "T5CharacterCreator.h"
#include "T5ReferenceAnimation.h"
#include "GameFramework/PawnMovementComponent.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "UObject/UObjectIterator.h"
#include "HAL/IConsoleManager.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "SceneInterface.h"
#include "RenderingThread.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "HAL/FileManager.h"
#include "Serialization/JsonSerializer.h"
#include "UnrealClient.h"

namespace {
TArray<TSharedPtr<FJsonValue>> AuditVector(const FVector& V) {
    return {MakeShared<FJsonValueNumber>(V.X), MakeShared<FJsonValueNumber>(V.Y), MakeShared<FJsonValueNumber>(V.Z)};
}
TSharedRef<FJsonObject> AuditTransform(const FTransform& T) {
    auto J = MakeShared<FJsonObject>();
    J->SetArrayField(TEXT("position_ue_cm"), AuditVector(T.GetLocation()));
    const auto Q = T.GetRotation();
    J->SetArrayField(TEXT("quaternion_xyzw"), {MakeShared<FJsonValueNumber>(Q.X), MakeShared<FJsonValueNumber>(Q.Y),
        MakeShared<FJsonValueNumber>(Q.Z), MakeShared<FJsonValueNumber>(Q.W)});
    J->SetArrayField(TEXT("scale_xyz"), AuditVector(T.GetScale3D()));
    return J;
}
}

// Read-only game regression: normal Papyrus/PACK/HKX/physics drive the world.
// Reference images only supply observation times and optional comparison views.
class FT5ReferenceAudit final : public IAutomationLatentCommand {
    FAutomationTestBase& Test;
    TWeakObjectPtr<AT5Scene> Scene;
    FString Source, Output;
    TArray<TSharedPtr<FJsonObject>> Frames;
    TArray<double> Times;
    TSet<uint32> Tracked;
    double Started = FPlatformTime::Seconds(), NextSample = 0, Limit = 0;
    int32 NextFrame = 0, Samples = 0, CapturePhase = 0;
    TWeakObjectPtr<ACameraActor> ComparisonCamera;
    TWeakObjectPtr<AActor> OriginalView;
    TSharedPtr<FJsonObject> Capturing;
    bool bInitialized = false;
    FString RoutePath, SaveSlot;
    TArray<TSharedPtr<FJsonValue>> Route;
    int32 RouteIndex = 0;
    double StepStarted = 0;
    bool bStepActionStarted = false;
    bool bObservedFallenActor = false;
    bool bObservedExecutionProp = false, bObservedSeveredHead = false;
    uint32 FallenActor = 0;
    double RagdollStarted = 0, MaxHipRise = 0;
    TOptional<double> LastAnimatedHipZ;
    double FallenAt = 0, MaxCorpseDrift = 0;
    TOptional<FVector> CorpseRestPosition;

    bool FollowRoute(AT5Scene& S, APlayerController& PC)
    {
        for (int32 Index = RouteIndex; Index < Route.Num(); ++Index)
        {
            double Cell = 0;
            if (!Route[Index]->AsObject()->TryGetNumberField(TEXT("cell"), Cell)) continue;
            if (S.ActiveLocation == uint32(Cell))
            {
                UE_LOG(LogTemp, Display, TEXT("T5_ROUTE_ARRIVAL cell=%08X"), S.ActiveLocation);
                RouteIndex = Index + 1;
                StepStarted = 0;
            }
            break;
        }
        if (RouteIndex >= Route.Num())
        {
            if (S.bSceneLoading) return false;
            if (!SaveSlot.IsEmpty())
                Test.TestTrue(TEXT("Captured the played checkpoint"), S.SaveGameSlot(SaveSlot));
            Test.AddInfo(FString::Printf(TEXT("Completed %d route steps by player movement"), RouteIndex));
            return true;
        }
        if (RouteIndex == 0 && ((S.AppliedControls & 1u) || (S.AppliedGameFlags & (4u | 64u | 128u)))) return false;
        if (StepStarted == 0) StepStarted = S.SimulationTime;
        const auto& Step = Route[RouteIndex]->AsObject();
        auto* Pawn = PC.GetPawnOrSpectator();
        const auto Root = S.GetActorTransform();
        const auto Foot = Pawn ? Pawn->GetActorLocation() - Root.TransformVectorNoScale(FVector(0,0,S.Papyrus->PlayerEyeHeight())) : FVector::ZeroVector;
        const auto Advance = [&] {
            UE_LOG(LogTemp, Display, TEXT("T5_ROUTE_STEP %d %s"), RouteIndex, *Step->GetStringField(TEXT("name")));
            ++RouteIndex; StepStarted = 0; bStepActionStarted = false;
        };
        double Location = 0;
        if (Step->TryGetNumberField(TEXT("cell"), Location) && S.ActiveLocation == uint32(Location))
        {
            Advance(); return false;
        }
        double Timeout = 45;
        Step->TryGetNumberField(TEXT("timeout"), Timeout);
        if (S.SimulationTime - StepStarted > Timeout)
        {
            Test.AddError(FString::Printf(TEXT("Route step %d timed out (%s) at %s"), RouteIndex,
                *Step->GetStringField(TEXT("name")), *UnrealToT5Position(Root.InverseTransformPosition(Foot)).ToCompactString()));
            return true;
        }
        double Delay = 0;
        if (Step->TryGetNumberField(TEXT("wait_seconds"), Delay))
        {
            if (S.SimulationTime - StepStarted >= Delay) Advance();
            return false;
        }
        double Controls = 0;
        if (Step->TryGetNumberField(TEXT("controls_clear"), Controls))
        {
            if (!(S.AppliedControls & uint32(Controls)) && !(S.AppliedGameFlags & (4u | 128u))) Advance();
            return false;
        }
        double RefId = 0;
        if (Step->TryGetNumberField(TEXT("model_motion"), RefId))
        {
            FString Node;
            double Distance = 0;
            Step->TryGetStringField(TEXT("motion_node"), Node);
            Step->TryGetNumberField(TEXT("distance_cm"), Distance);
            if (const auto* Parts = S.ReferenceVisuals.Find(uint32(RefId)))
                for (const auto& Part : *Parts)
                    if (const auto* Component = Cast<UPrimitiveComponent>(Part.Component.Get());
                        Component && Part.MotionNode == Node && !Part.bColliderOnly)
                    {
                        const auto Center = Component->CalcBounds(FTransform::Identity).Origin;
                        const auto Moved = FVector::Distance(Part.Original.TransformPosition(Center),
                            Component->GetRelativeTransform().TransformPosition(Center));
                        if (Moved < Distance || Moved > 1000.) continue;
                        UE_LOG(LogTemp, Display, TEXT("T5_ROUTE_MODEL_MOTION ref=%08X node=%s distance=%.2f"), uint32(RefId), *Node, Moved);
                        FScreenshotRequest::RequestScreenshot(Output / TEXT("wall-collapse.png"), false, false);
                        Advance();
                        break;
                    }
            return false;
        }
        if (Step->TryGetNumberField(TEXT("visible_actor"), RefId))
        {
            if (const auto* Mesh = S.FindReferenceSkeleton(uint32(RefId)); Mesh && Mesh->IsVisible())
            {
                FScreenshotRequest::RequestScreenshot(Output / TEXT("keep-companion.png"), false, false);
                Advance();
            }
            return false;
        }
        double BodyId = 0;
        if (Step->TryGetNumberField(TEXT("ragdoll"), BodyId))
        {
            if (const auto* Body = S.FindReferenceSkeleton(uint32(BodyId)); Body && S.Ragdolled.Contains(uint32(BodyId)))
            {
                const FVector Hip = Body->GetSocketLocation(TEXT("NPC COM [COM ]"));
                const FVector Head = Body->GetSocketLocation(TEXT("NPC Head [Head]"));
                if (FMath::Abs(Head.Z - Hip.Z) < 55)
                {
                    UE_LOG(LogTemp, Display, TEXT("T5_ROUTE_CORPSE %08X hip=%s head=%s"), uint32(BodyId),
                        *Hip.ToCompactString(), *Head.ToCompactString());
                    Advance();
                }
            }
            return false;
        }
        FString ViewAction;
        if (Step->TryGetStringField(TEXT("view_test"), ViewAction))
        {
            if (!bStepActionStarted)
            {
                if (S.bSceneLoading && S.bFullLoadingScreen) return false;
                bStepActionStarted = true;
                if (ViewAction == TEXT("attack")) S.AttackPlayerWeapon();
                else S.ReadyPlayerWeapon();
                return false;
            }
            const bool bWeapon = S.FirstPersonWeapon.IsValid();
            const bool bReady = ViewAction == TEXT("draw") && bWeapon && S.bViewDrawn && S.bViewWeaponVisible && !S.bViewTransition;
            const bool bAttacking = ViewAction == TEXT("attack") && bWeapon && S.FirstPersonClip.Contains(TEXT("attack"));
            const bool bHolstered = ViewAction == TEXT("holster") && bWeapon && !S.bViewDrawn && !S.bViewTransition;
            if (bReady || bAttacking || bHolstered)
            {
                FScreenshotRequest::RequestScreenshot(Output / (TEXT("view-") + ViewAction + TEXT(".png")), false, false);
                Advance();
            }
            return false;
        }
        double Owner = 0, Item = 0;
        if (Step->TryGetNumberField(TEXT("inventory_owner"), Owner) && Step->TryGetNumberField(TEXT("item"), Item))
        {
            if (!S.OpenInventoryOwner)
            {
                if (uint32(Owner) == 0x14) S.OpenPlayerInventory();
                else
                {
                    FTransform Authored;
                    uint32 Parent = 0;
                    if (const auto Pose = S.Papyrus->ReferenceTransform(uint32(Owner), Authored, Parent))
                    {
                        FVector Eye; FRotator Rotation;
                        PC.GetPlayerViewPoint(Eye, Rotation);
                        FBox Bounds(ForceInit);
                        if (const auto* Parts = S.ReferenceVisuals.Find(uint32(Owner)))
                            for (const auto& Part : *Parts)
                                if (const auto* Mesh = Cast<UPrimitiveComponent>(Part.Component.Get());
                                    Mesh && Mesh->IsVisible() && !Part.bColliderOnly)
                                    Bounds += Mesh->Bounds.GetBox();
                        const auto Target = Bounds.IsValid ? Bounds.GetCenter()
                            : Root.TransformPosition(Pose->GetLocation() + FVector(0, 0, 35));
                        PC.SetControlRotation((Target - Eye).Rotation());
                        if (S.SimulationTime - StepStarted > Timeout - .1)
                        {
                            FHitResult Hit;
                            FCollisionQueryParams Query(SCENE_QUERY_STAT(T5AuditLoot), true, Pawn);
                            S.GetWorld()->LineTraceSingleByChannel(Hit, Eye, Target, ECC_Visibility, Query);
                            UE_LOG(LogTemp, Display, TEXT("T5_ROUTE_LOOT_AIM ref=%08X bounds=%d eye=%s aim=%s hit=%08X at=%s"),
                                uint32(Owner), Bounds.IsValid, *Eye.ToCompactString(), *Target.ToCompactString(),
                                S.InteractionComponents.FindRef(Hit.GetComponent()), *Hit.ImpactPoint.ToCompactString());
                        }
                        S.InteractT5();
                    }
                }
                return false;
            }
            const auto FindItem = [&](uint32 Inventory) -> TArray<TSharedPtr<FJsonValue>>
            {
                TArray<TSharedPtr<FJsonValue>> Rows;
                if (S.Papyrus->Inventory(Inventory, Rows))
                    for (int32 I = 1; I < Rows.Num(); ++I)
                        if (const auto& Entry = Rows[I]->AsArray(); Entry.Num() == 5 && uint32(Entry[0]->AsNumber()) == uint32(Item))
                            return Entry;
                return {};
            };
            const auto Original = FindItem(uint32(Owner));
            const bool bCorrectMenu = S.OpenInventoryOwner == uint32(Owner);
            const bool bWorked = bCorrectMenu && !Original.IsEmpty() && S.Papyrus->InventoryAction(uint32(Owner), uint32(Item), 1);
            const auto Current = FindItem(0x14);
            bool bConsume = false;
            Step->TryGetBoolField(TEXT("consume"), bConsume);
            bool bExpected = bWorked;
            if (bExpected && uint32(Owner) != 0x14)
            {
                const auto Remaining = FindItem(uint32(Owner));
                const auto Count = Remaining.IsEmpty() ? 0 : Remaining[1]->AsNumber();
                bExpected = !Current.IsEmpty() && Count == Original[1]->AsNumber() - 1;
            }
            else if (bExpected && bConsume)
                bExpected = Current.IsEmpty() || Current[1]->AsNumber() == Original[1]->AsNumber() - 1;
            else if (bExpected)
                bExpected = !Current.IsEmpty() && Current[4]->AsBool();
            S.CloseInventory();
            if (!bExpected)
            {
                Test.AddError(FString::Printf(TEXT("Inventory action failed for %08X in %08X (menu=%d, found=%d, action=%d)"),
                    uint32(Item), uint32(Owner), bCorrectMenu, !Original.IsEmpty(), bWorked));
                return true;
            }
            Advance(); return false;
        }
        FString Quest;
        double Stage = 0;
        if (Step->TryGetStringField(TEXT("quest"), Quest) && Step->TryGetNumberField(TEXT("stage"), Stage))
        {
            if (S.Papyrus->ObservedQuestStage(Quest) >= int32(Stage)) Advance();
            return false;
        }
        if (!Pawn || (S.AppliedControls & 1u) || (S.AppliedGameFlags & (4u | 64u | 128u))) return false;
        const auto& XYZ = Step->GetArrayField(TEXT("point"));
        const auto Goal = Root.TransformPosition(T5ToUnrealPosition(FVector(XYZ[0]->AsNumber(),XYZ[1]->AsNumber(),XYZ[2]->AsNumber())));
        const auto Delta = Goal - Foot;
        double Radius = 50;
        Step->TryGetNumberField(TEXT("radius_cm"), Radius);
        bool bUse = false;
        Step->TryGetBoolField(TEXT("use"), bUse);
        if (Delta.Size() <= Radius)
        {
            if (auto* Movement = Pawn->GetMovementComponent()) Movement->StopMovementImmediately();
            if (bUse)
            {
                FVector Eye; FRotator View; PC.GetPlayerViewPoint(Eye, View);
                PC.SetControlRotation((Goal + FVector(0,0,S.Papyrus->PlayerEyeHeight()*.5) - Eye).Rotation());
                S.InteractT5();
                if (Location == 0) Advance();
            }
            else if (Location == 0) Advance();
            return false;
        }
        PC.SetControlRotation(FRotator(0, Delta.Rotation().Yaw, 0));
        Pawn->AddMovementInput(Delta.GetSafeNormal(), FMath::Clamp(Delta.Size()/250., .18, 1.));
        return false;
    }

    void Write(const TSharedRef<FJsonObject>& J) {
        FString Text;
        FJsonSerializer::Serialize(J, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
        FFileHelper::SaveStringToFile(Text + TEXT("\n"), *(Output / TEXT("telemetry.jsonl")),
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
    }
    bool Initialize() {
        if (!RoutePath.IsEmpty())
        {
            FString Text;
            if (!FFileHelper::LoadFileToString(Text, *RoutePath)
                || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Route) || Route.IsEmpty())
            {
                Test.AddError(TEXT("Could not read the player route")); return false;
            }
        }
        TArray<FString> Names;
        IFileManager::Get().FindFiles(Names, *(Source / TEXT("frame_*.json")), true, false); Names.Sort();
        double FirstDay = -1;
        for (const auto& Name : Names) {
            FString Text; TSharedPtr<FJsonObject> J;
            if (!FFileHelper::LoadFileToString(Text, *(Source / Name)) ||
                !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J) || !J) continue;
            if (J->GetStringField(TEXT("schema")) != TEXT("t54.skyrim-reference/1")) continue;
            const auto Time = J->GetObjectField(TEXT("game_time"));
            const double Day = Time->GetNumberField(TEXT("days_passed"));
            if (FirstDay < 0) FirstDay = Day;
            const double Scale = Time->GetNumberField(TEXT("timescale"));
            if (Scale <= 0) { Test.AddError(TEXT("Reference needs a positive timescale")); return false; }
            Frames.Add(J); Times.Add((Day - FirstDay) * 86400. / Scale);
            for (const auto& V : J->GetArrayField(TEXT("references"))) {
                const auto R = V->AsObject();
                if (R->HasField(TEXT("actor"))) Tracked.Add(FCString::Strtoui64(*R->GetStringField(TEXT("form_id")), nullptr, 16));
            }
        }
        if (Frames.IsEmpty()) { Test.AddError(TEXT("No reference frames")); return false; }
        for (uint32 Id : {0x14u, 0xBB970u, 0xB9DF3u, 0xBB971u, 0xB9DF2u}) Tracked.Add(Id);
        if (!Limit) Limit = Times.Last() + 10;
        IFileManager::Get().MakeDirectory(*Output, true);
        FFileHelper::SaveStringToFile(TEXT(""), *(Output / TEXT("telemetry.jsonl")));
        return true;
    }
    TSharedRef<FJsonObject> Snapshot(AT5Scene& S, const TSharedPtr<FJsonObject>& Reference = {}) {
        auto J = MakeShared<FJsonObject>();
        J->SetNumberField(TEXT("simulation_time"), S.SimulationTime);
        J->SetNumberField(TEXT("mq101_stage"), S.Papyrus->ObservedQuestStage(TEXT("MQ101")));
        J->SetNumberField(TEXT("attack_stage"), S.Papyrus->ObservedQuestStage(TEXT("MQ101DragonAttack")));
        J->SetNumberField(TEXT("cell"), S.ActiveLocation);
        uint32 Controls, Flags; float Sitting; S.Papyrus->GameState(Controls, Flags, Sitting);
        J->SetNumberField(TEXT("controls"), Controls); J->SetNumberField(TEXT("flags"), Flags);
        J->SetNumberField(TEXT("sitting_rotation"), Sitting);
        float Weather[12]{}; J->SetStringField(TEXT("weather_id"), FString::Printf(TEXT("%08X"), S.Papyrus->Weather(Weather)));
        float Environment[15]{};
        J->SetStringField(TEXT("weather_id"),FString::Printf(TEXT("%08X"),S.Papyrus->Environment(Environment)));
        auto Sky=MakeShared<FJsonObject>();
        Sky->SetNumberField(TEXT("game_hour"),Environment[12]);
        Sky->SetNumberField(TEXT("transition"),Environment[13]);
        Sky->SetNumberField(TEXT("fog_near"),Environment[9]);Sky->SetNumberField(TEXT("fog_far"),Environment[10]);
        Sky->SetNumberField(TEXT("fog_max"),Environment[11]);Sky->SetNumberField(TEXT("fog_power"),Environment[14]);
        Sky->SetArrayField(TEXT("sunlight_rgb"),AuditVector(FVector(Environment[0],Environment[1],Environment[2])));
        Sky->SetArrayField(TEXT("ambient_rgb"),AuditVector(FVector(Environment[3],Environment[4],Environment[5])));
        Sky->SetArrayField(TEXT("fog_rgb"),AuditVector(FVector(Environment[6],Environment[7],Environment[8])));
        J->SetObjectField(TEXT("environment"),Sky);
        TArray<TSharedPtr<FJsonValue>> Fogs;
        for(TObjectIterator<UExponentialHeightFogComponent> It;It;++It) if(It->GetWorld()==S.GetWorld()) {
            auto Fog=MakeShared<FJsonObject>();
            Fog->SetStringField(TEXT("path"),It->GetPathName());
            Fog->SetBoolField(TEXT("registered"),It->IsRegistered());
            Fog->SetBoolField(TEXT("visible"),It->IsVisible());
            Fog->SetBoolField(TEXT("should_render"),It->ShouldRender());
            Fog->SetBoolField(TEXT("owner_hidden"),It->GetOwner() && It->GetOwner()->IsHidden());
            Fog->SetNumberField(TEXT("density"),It->FogDensity);
            Fog->SetNumberField(TEXT("falloff"),It->FogHeightFalloff);
            Fogs.Add(MakeShared<FJsonValueObject>(Fog));
        }
        J->SetArrayField(TEXT("fog_components"),Fogs);
        if(auto* FogVar=IConsoleManager::Get().FindConsoleVariable(TEXT("r.Fog"))) J->SetNumberField(TEXT("r_fog"),FogVar->GetInt());
        if(GEngine && GEngine->GameViewport) J->SetBoolField(TEXT("viewport_fog"),GEngine->GameViewport->EngineShowFlags.Fog);
        if(auto* RenderScene=S.GetWorld()->Scene) { ENQUEUE_RENDER_COMMAND(T5AuditFogPresence)([RenderScene](FRHICommandListImmediate&) {
            UE_LOG(LogTemp,Display,TEXT("T5_RENDER_FOG present=%d"),RenderScene->HasAnyExponentialHeightFog());
        }); }
        if (auto* PC = S.GetWorld()->GetFirstPlayerController()) {
            FVector Eye; FRotator Rotation; PC->GetPlayerViewPoint(Eye, Rotation);
            J->SetObjectField(TEXT("camera"), AuditTransform(FTransform(Rotation, Eye)));
            J->SetNumberField(TEXT("fov"), PC->PlayerCameraManager->GetFOVAngle());
        }
        TArray<TSharedPtr<FJsonValue>> References;
        for (uint32 Id : Tracked) {
            FTransform Authored; uint32 Parent;
            const auto Logical = S.Papyrus->ReferenceTransform(Id, Authored, Parent);
            if (!Logical) continue;
            auto R = AuditTransform(*Logical);
            R->SetStringField(TEXT("form_id"), FString::Printf(TEXT("%08X"), Id));
            R->SetStringField(TEXT("parent"), FString::Printf(TEXT("%08X"), Parent));
            const auto Corrected = S.Mechanics ? S.Mechanics->CorrectPose(Id, Parent, *Logical) : *Logical;
            R->SetObjectField(TEXT("corrected"), AuditTransform(Corrected));
            if (auto* Mesh = S.FindReferenceSkeleton(Id)) {
                R->SetObjectField(TEXT("render_root"), AuditTransform(Mesh->GetComponentTransform()));
                auto Bones = MakeShared<FJsonObject>();
                for (const TCHAR* Name : {TEXT("NPC Root [Root]"), TEXT("NPC Pelvis [Pelv]"), TEXT("NPC Spine2 [Spn2]"),
                    TEXT("NPC Head [Head]"), TEXT("NPC L UpperArm [LUar]"), TEXT("NPC L Forearm [LLar]"),
                    TEXT("NPC L Hand [LHnd]"), TEXT("NPC R UpperArm [RUar]"), TEXT("NPC R Forearm [RLar]"), TEXT("NPC R Hand [RHnd]"),
                    TEXT("NPC L Thigh [LThg]"), TEXT("NPC L Calf [LClf]"), TEXT("NPC L Foot [Lft ]"),
                    TEXT("NPC R Thigh [RThg]"), TEXT("NPC R Calf [RClf]"), TEXT("NPC R Foot [Rft ]")})
                    if (Mesh->GetBoneIndex(Name) != INDEX_NONE)
                        Bones->SetArrayField(Name, AuditVector(Mesh->GetBoneLocation(Name)));
                R->SetObjectField(TEXT("bones_world"), Bones);
            }
            if (const auto* A = S.Papyrus->AnimationRequests().Find(Id)) {
                R->SetStringField(TEXT("animation_event"), A->Event); R->SetStringField(TEXT("animation_clip"), A->Clip);
            }
            References.Add(MakeShared<FJsonValueObject>(R));
        }
        J->SetArrayField(TEXT("references"), References);
        if (Reference) J->SetNumberField(TEXT("reference_capture_id"), Reference->GetNumberField(TEXT("capture_id")));
        return J;
    }
public:
    FT5ReferenceAudit(FAutomationTestBase& T, AT5Scene& S, FString InSource, FString InOutput)
        : Test(T), Scene(&S), Source(MoveTemp(InSource)), Output(MoveTemp(InOutput)) {
        FParse::Value(FCommandLine::Get(), TEXT("T5ReferenceLimit="), Limit);
        FParse::Value(FCommandLine::Get(), TEXT("T5ReferenceRoute="), RoutePath);
        FParse::Value(FCommandLine::Get(), TEXT("T5ReferenceSave="), SaveSlot);
        FString Actor;
        if (FParse::Value(FCommandLine::Get(), TEXT("T5ReferenceFallenActor="), Actor))
            FallenActor = uint32(FCString::Strtoui64(*Actor, nullptr, 16));
    }
    bool Update() override {
        auto* S = Scene.Get();
        if (!S || S->bLoadingFailed || FPlatformTime::Seconds() - Started > 1500) {
            Test.AddError(TEXT("Reference audit failed to finish")); return true;
        }
        if (!bInitialized) { bInitialized = true; if (!Initialize()) return true; }
        auto* PC = S->GetWorld()->GetFirstPlayerController();
        if (!PC || !S->Papyrus) return false;
        if (CapturePhase) {
            if (FScreenshotRequest::IsScreenshotRequested()) return false;
            if (CapturePhase == 1 && FParse::Param(FCommandLine::Get(), TEXT("T5ReferenceViews"))) {
                const auto Camera = Capturing->GetObjectField(TEXT("camera"));
                const auto& Position = Camera->GetObjectField(TEXT("world"))->GetArrayField(TEXT("position_ue_cm"));
                const auto& Matrix = Camera->GetObjectField(TEXT("world"))->GetArrayField(TEXT("rotation_matrix"));
                const auto Column = [&](int32 I) { return FVector(Matrix[1]->AsArray()[I]->AsNumber(), Matrix[0]->AsArray()[I]->AsNumber(), Matrix[2]->AsArray()[I]->AsNumber()); };
                const auto Root = S->GetActorTransform();
                const FVector Eye = Root.TransformPosition(FVector(Position[0]->AsNumber(), Position[1]->AsNumber(), Position[2]->AsNumber()));
                const FRotator View = FRotationMatrix::MakeFromXZ(Root.TransformVectorNoScale(Column(0)), Root.TransformVectorNoScale(Column(1))).Rotator();
                ComparisonCamera = S->GetWorld()->SpawnActor<ACameraActor>(Eye, View);
                const auto Frustum = Camera->GetObjectField(TEXT("frustum"));
                ComparisonCamera->GetCameraComponent()->SetFieldOfView(FMath::RadiansToDegrees(
                    FMath::Atan(Frustum->GetNumberField(TEXT("right"))) - FMath::Atan(Frustum->GetNumberField(TEXT("left")))));
                PC->SetViewTarget(ComparisonCamera.Get()); CapturePhase = 2; return false;
            }
            if (CapturePhase == 2) {
                FScreenshotRequest::RequestScreenshot(Output / FString::Printf(TEXT("matched-%08d.png"), int32(Capturing->GetNumberField(TEXT("capture_id")))), true, false);
                CapturePhase = 3; return false;
            }
            if (OriginalView.IsValid()) PC->SetViewTarget(OriginalView.Get());
            if (ComparisonCamera.IsValid()) ComparisonCamera->Destroy();
            PC->SetPause(false); CapturePhase = 0; Capturing.Reset(); return false;
        }
        if (S->CharacterCreator) {
            if (S->CharacterCreator->bReady) S->CharacterCreator->Accept();
            return false;
        }
        if ((S->bSceneLoading && S->bFullLoadingScreen) || S->bStartupPreparing || S->bRestoringCheckpoint) return false;
        FString Execution;
        if (FParse::Value(FCommandLine::Get(), TEXT("T5ReferenceExecution="), Execution))
        {
            const uint32 Victim = FCString::Strtoui64(*Execution, nullptr, 16);
            if (!bObservedExecutionProp)
                for (const auto& [Id, Props] : S->AnimationProps)
                    if (const auto* Prop = Props.Find(TEXT("AnimObjectExecutionerAxe")); Prop && Prop->Component.IsValid()
                        && Prop->Component->IsVisible() && Prop->Component->GetAttachParent() == S->FindReferenceSkeleton(Id))
                        if (const auto* Animation = S->ReferenceAnimations.Find(Id);
                            Animation && (*Animation)->Clock.Playback.GeneratorName.Equals(TEXT("AOExecutionerEnter.hkx"), ESearchCase::IgnoreCase))
                        {
                            bObservedExecutionProp = true;
                            FScreenshotRequest::RequestScreenshot(Output / TEXT("execution-axe.png"), false, false);
                        }
            if (!bObservedSeveredHead)
                if (const auto Head = S->DetachedHeads.FindRef(Victim); Head.IsValid()
                    && S->HeadlessMeshes.FindRef(Victim).IsValid())
                    if (const auto* Body = Cast<UPrimitiveComponent>(Head->GetRootComponent()); Body && Body->IsSimulatingPhysics())
                    {
                        bObservedSeveredHead = true;
                        FScreenshotRequest::RequestScreenshot(Output / TEXT("execution-head.png"), false, false);
                    }
        }
        if (FallenActor)
        {
            if (const auto* Rig = S->FindReferenceSkeleton(FallenActor); Rig && Rig->IsVisible())
            {
                const auto Hip = Rig->GetSocketLocation(TEXT("NPC Pelvis [Pelv]"));
                const auto Head = Rig->GetSocketLocation(TEXT("NPC Head [Head]"));
                if (!S->Ragdolled.Contains(FallenActor) && !RagdollStarted)
                    LastAnimatedHipZ = Hip.Z;
                else if (S->Ragdolled.Contains(FallenActor))
                {
                    if (!RagdollStarted) RagdollStarted = S->SimulationTime;
                    if (LastAnimatedHipZ && S->SimulationTime - RagdollStarted <= .5)
                        MaxHipRise = FMath::Max(MaxHipRise, Hip.Z - *LastAnimatedHipZ);
                    if (!bObservedFallenActor && FMath::Abs(Head.Z - Hip.Z) < 45)
                    {
                        bObservedFallenActor = true;
                        FallenAt = S->SimulationTime;
                        UE_LOG(LogTemp, Display, TEXT("T5_REFERENCE_FALLEN ref=%08X hip=%s head=%s"), FallenActor,
                            *Hip.ToCompactString(), *Head.ToCompactString());
                    }
                }
            }
        }
        if (bObservedFallenActor && FParse::Param(FCommandLine::Get(), TEXT("T5ReferenceCorpseRest")))
        {
            const double SinceFall = S->SimulationTime - FallenAt;
            if (SinceFall >= 4 && SinceFall <= 11)
                if (const auto* Rig = S->FindReferenceSkeleton(FallenActor))
                {
                    const auto Pelvis = Rig->GetSocketLocation(TEXT("NPC Pelvis [Pelv]"));
                    if (!CorpseRestPosition) CorpseRestPosition = Pelvis;
                    MaxCorpseDrift = FMath::Max(MaxCorpseDrift, FVector::Dist2D(*CorpseRestPosition, Pelvis));
                }
        }
        if (S->SimulationTime >= NextSample) { Write(Snapshot(*S)); ++Samples; NextSample = S->SimulationTime + 1; }
        if (!RoutePath.IsEmpty()) return FollowRoute(*S, *PC);
        if (NextFrame < Frames.Num() && S->SimulationTime >= Times[NextFrame]) {
            auto Reference = Frames[NextFrame++];
            const int32 Id = Reference->GetNumberField(TEXT("capture_id"));
            // Six-second image cadence is unnecessary for the audit; retain twelve landmarks.
            if (Id == 2 || Id == 7 || Id == 17 || Id == 25 || Id == 35 || Id == 41 || Id == 47 || Id == 59 || Id == 71 || Id == 81 || Id == 83 || Id == 88) {
                Write(Snapshot(*S, Reference)); Capturing = Reference; OriginalView = PC->GetViewTarget(); PC->SetPause(true);
                FScreenshotRequest::RequestScreenshot(Output / FString::Printf(TEXT("natural-%08d.png"), Id), true, false);
                CapturePhase = 1; return false;
            }
        }
        if (S->SimulationTime < Limit) return false;
        if (!SaveSlot.IsEmpty() && S->bSceneLoading) return false;
        Test.TestTrue(TEXT("Normal simulation produced a useful reference trace"), Samples > 20);
        if (!Execution.IsEmpty())
        {
            Test.TestTrue(TEXT("The execution prop is attached during the authored entry"), bObservedExecutionProp);
            Test.TestTrue(TEXT("Decapitate leaves the body headless and spawns a physical head"), bObservedSeveredHead);
        }
        if (FallenActor)
        {
            Test.TestTrue(TEXT("The shot actor settled horizontally as a ragdoll"), bObservedFallenActor);
            Test.AddInfo(FString::Printf(TEXT("Pelvis rise in the first half-second of ragdoll: %.2f cm"), MaxHipRise));
            Test.TestTrue(TEXT("The actor does not launch upward at the start of ragdoll"),
                LastAnimatedHipZ.IsSet() && RagdollStarted > 0 && MaxHipRise < 25);
        }
        if (FParse::Param(FCommandLine::Get(), TEXT("T5ReferenceCorpseRest")))
        {
            Test.AddInfo(FString::Printf(TEXT("Corpse drift over seven seconds after impact: %.2f cm"), MaxCorpseDrift));
            Test.TestTrue(TEXT("Ragdoll stops sliding after the fall"), CorpseRestPosition.IsSet() && MaxCorpseDrift < 20);
        }
        FString ExpectedWeather;
        if (FParse::Value(FCommandLine::Get(), TEXT("T5ReferenceExpectedWeather="), ExpectedWeather))
        {
            float Values[12]{};
            Test.TestEqual(TEXT("Scripted weather transition completed"), S->Papyrus->Weather(Values),
                static_cast<uint32>(FCString::Strtoui64(*ExpectedWeather, nullptr, 16)));
        }
        if (!SaveSlot.IsEmpty())
        {
            Test.TestTrue(TEXT("Captured the played checkpoint"), S->SaveGameSlot(SaveSlot));
        }
        Test.AddInfo(FString::Printf(TEXT("Reference audit: %d observations, %s"), Samples, *Output)); return true;
    }
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FT5ReferenceAuditTest, "T5Data.ReferenceAudit",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FT5ReferenceAuditTest::RunTest(const FString&) {
    FString Source, Output;
    if (!FParse::Value(FCommandLine::Get(), TEXT("T5ReferenceSource="), Source) ||
        !FParse::Value(FCommandLine::Get(), TEXT("T5ReferenceOutput="), Output)) {
        AddError(TEXT("Pass -T5ReferenceSource=<SKSE session> -T5ReferenceOutput=<directory> with -T5NewGame -T5ReadOnly")); return false;
    }
    for (const auto& Context : GEngine->GetWorldContexts())
        if (auto* World = Context.World(); World && World->IsGameWorld())
            for (TActorIterator<AT5Scene> It(World); It; ++It) {
                FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FT5ReferenceAudit>(*this, **It, Source, Output)); return true;
            }
    AddError(TEXT("No game scene")); return false;
}
#endif
