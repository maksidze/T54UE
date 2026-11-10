#include "T5Scene.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "T5CharacterCreator.h"
#include "T5Bridge.h"
#include "T5Mechanics.h"
#include "T5HkxAnimInstance.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/PointLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "Engine/GameViewportClient.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformOutputDevices.h"
#include "HAL/FileManager.h"
#include "UnrealClient.h"

class FT5RaceMenuSmoke : public IAutomationLatentCommand
{
    FAutomationTestBase& Test;
    TWeakObjectPtr<AT5Scene> Scene;
    double Started = FPlatformTime::Seconds(), PausedTime = 0, WaitUntil = 0;
    double PausedWorldTime = 0;
    TWeakObjectPtr<USkeletalMeshComponent> FrozenNPC;
    FVector FrozenHead = FVector::ZeroVector;
    FVector PlayerFeet = FVector::ZeroVector;
    TWeakObjectPtr<AActor> OriginalView;
    TWeakObjectPtr<AActor> OriginalWorld;
    int Phase = 0;
    int BeastIndex = 0;
    bool bBeastConfigured = false;
    FString ScreenshotPath() const
    {
        static const TCHAR* Names[]={TEXT("ArgonianMale"),TEXT("KhajiitMale"),TEXT("ArgonianFemale"),TEXT("KhajiitFemale")};
        return FPaths::ProjectSavedDir()/TEXT("Screenshots")/(FParse::Param(FCommandLine::Get(),TEXT("T5RaceMenuSmokeBeasts"))
            ? FString::Printf(TEXT("T5RaceMenu_%s.png"),Names[BeastIndex]) : FString(TEXT("T5RaceMenu.png")));
    }
    void SelectBeast(FT5CharacterCreator& Creator)
    {
        Creator.Choice.Race=(BeastIndex&1) ? 0x13745 : 0x13740;
        Creator.Choice.Female=BeastIndex/2;
        Creator.Choice.Preset=0; FMemory::Memzero(Creator.Choice.Parts);
        Creator.Morphs.Reset(); Creator.Changed();
    }
public:
    FT5RaceMenuSmoke(FAutomationTestBase& InTest, AT5Scene* InScene) : Test(InTest), Scene(InScene) {}
    bool Update() override
    {
        auto* S = Scene.Get();
        if (!S || S->bLoadingFailed || FPlatformTime::Seconds() - Started > 600)
        { Test.AddError(TEXT("Character creation failed or timed out")); return true; }
        if (!S->Papyrus || S->bStartupPreparing || S->bRestoringCheckpoint || S->bSceneLoading) return false;
        if (Phase == 0)
        {
            const bool RequireRequest=FParse::Param(FCommandLine::Get(),TEXT("T5RaceMenuRequireRequest"));
            if (RequireRequest && !S->CharacterCreator) return false;
            if (!S->CharacterCreator) S->OpenRaceMenu();
            if (!S->CharacterCreator || !S->CharacterCreator->bReady) return false;
            auto& Creator = *S->CharacterCreator;
            if (FParse::Param(FCommandLine::Get(),TEXT("T5RaceMenuSmokeBeasts")) && !bBeastConfigured)
            { bBeastConfigured=true; SelectBeast(Creator); return false; }
            Test.TestTrue(TEXT("Character has original NIF geometry"),Creator.Preview.IsValid());
            Test.TestTrue(TEXT("RaceMenu TRI extensions are available"),Creator.AvailableMorphs.Num() > 20);
            if (RequireRequest) Test.TestTrue(TEXT("Original ShowRaceMenu opened mandatory creation"),Creator.bRequired);
            Test.TestTrue(TEXT("The entire Unreal world is paused"),UGameplayStatics::IsGamePaused(S->GetWorld()));
            PlayerFeet=Creator.PlayerPlacement().GetLocation();
            Test.TestTrue(TEXT("Character is at the player's world position"),Creator.Preview->GetActorLocation().Equals(PlayerFeet,.01));
            Test.TestTrue(TEXT("Character is in the game world, not an isolated studio"),FVector::Dist(PlayerFeet,S->GetWorld()->GetFirstPlayerController()->GetPawnOrSpectator()->GetActorLocation())<300);
            if (auto* Camera=Cast<ACameraActor>(Creator.Camera.Get()))
                Test.TestEqual(TEXT("Camera uses cell exposure without a manual override"),Camera->GetCameraComponent()->PostProcessBlendWeight,0.f);
            TInlineComponentArray<UPointLightComponent*> Lights(Creator.Preview.Get());
            Test.TestEqual(TEXT("Avatar has no studio lights"),Lights.Num(),0);
            OriginalWorld=S->LoadedScene;
            OriginalView=S->GetWorld()->GetFirstPlayerController()->GetPawnOrSpectator();
            for (const auto& [Id,Parts] : S->ReferenceVisuals)
                if (Id!=0x14 && !FrozenNPC.IsValid())
                    for (const auto& Part : Parts)
                        if (auto* Mesh=Cast<USkeletalMeshComponent>(Part.Component.Get()); Mesh && Mesh->GetBoneIndex(TEXT("NPC Head [Head]"))!=INDEX_NONE)
                        { FrozenNPC=Mesh; FrozenHead=Mesh->GetBoneLocation(TEXT("NPC Head [Head]")); break; }
            PausedWorldTime=S->GetWorld()->GetTimeSeconds();
            PausedTime = S->SimulationTime; WaitUntil = FPlatformTime::Seconds() + 3;
            ++Phase; return false;
        }
        if (Phase == 1)
        {
            if (FPlatformTime::Seconds() < WaitUntil) return false;
            Test.TestEqual(TEXT("Simulation is frozen while RaceMenu is open"),S->SimulationTime,PausedTime);
            Test.TestEqual(TEXT("World clock does not advance on pause"),double(S->GetWorld()->GetTimeSeconds()),PausedWorldTime);
            if (FrozenNPC.IsValid()) Test.TestTrue(TEXT("NPC animation stays frozen"),FrozenNPC->GetBoneLocation(TEXT("NPC Head [Head]")).Equals(FrozenHead,.001));
            auto& Creator=*S->CharacterCreator;
            Test.TestTrue(TEXT("Original scene stays loaded behind character creation"),S->LoadedScene==OriginalWorld.Get());
            TArray<TWeakObjectPtr<UMeshComponent>> InitiallyHidden;
            for (const auto& Group : Creator.ViewOccluders) if (Group.bHidden) InitiallyHidden.Append(Group.Parts);
            if (FParse::Param(FCommandLine::Get(),TEXT("T5RaceMenuRequireRequest")))
                Test.TestTrue(TEXT("Foreground NPCs and objects are hidden at the Helgen boundary"),!InitiallyHidden.IsEmpty());
            Creator.Orbit(180);
            bool RestoredBehindCamera=false;
            for (const auto& Part : InitiallyHidden) if (Part.IsValid() && Part->IsVisible()) RestoredBehindCamera=true;
            if (!InitiallyHidden.IsEmpty()) Test.TestTrue(TEXT("Obstructions become visible again after orbiting past them"),RestoredBehindCamera);
            Creator.Orbit(-180);
            const FVector CameraLocation=Creator.Camera->GetActorLocation();
            Creator.Orbit(15);
            Test.TestTrue(TEXT("Orbit updates while world is paused"),!Creator.Camera->GetActorLocation().Equals(CameraLocation,.01));
            if (auto* Manager=S->GetWorld()->GetFirstPlayerController()->PlayerCameraManager.Get())
                Test.TestTrue(TEXT("Paused camera cache follows the menu camera"),Manager->GetCameraLocation().Equals(Creator.Camera->GetActorLocation(),.1));
            Creator.Orbit(-15);
            FScreenshotRequest::RequestScreenshot(ScreenshotPath(),true,false);
            WaitUntil = FPlatformTime::Seconds() + 1; ++Phase; return false;
        }
        if (Phase == 2)
        {
            if (FScreenshotRequest::IsScreenshotRequested() || FPlatformTime::Seconds() < WaitUntil) return false;
            auto& Creator = *S->CharacterCreator;
            if (FParse::Param(FCommandLine::Get(),TEXT("T5RaceMenuSmokeBeasts")) && BeastIndex<3)
            { ++BeastIndex; SelectBeast(Creator); Phase=20; return false; }
            Creator.Choice.Female = !Creator.Choice.Female;
            Creator.Choice.Preset = 0; FMemory::Memzero(Creator.Choice.Parts);
            Creator.Choice.Weight = 78; Creator.Changed(); ++Phase; return false;
        }
        if (Phase == 3)
        {
            auto& Creator = *S->CharacterCreator;
            if (!Creator.bReady) return false;
            Test.TestTrue(TEXT("Changing sex rebuilds the preview"), Creator.PreviewRevision >= 2);
            Test.TestTrue(TEXT("Rebuilding appearance preserves the actual player position"),Creator.Preview->GetActorLocation().Equals(PlayerFeet,.01));
            if (!Creator.AvailableMorphs.IsEmpty())
            { Creator.Morphs.Add(Creator.AvailableMorphs[0],.35f); Creator.ApplyMorphs(); }
            Creator.Name = TEXT("RaceMenu smoke");
            TArray<uint8> Before; Test.TestTrue(TEXT("Native checkpoint is serializable before commit"),S->Papyrus->SaveCheckpoint(Before));
            TArray<TWeakObjectPtr<UMeshComponent>> HiddenParts;
            for (const auto& Group : Creator.ViewOccluders) if (Group.bHidden) HiddenParts.Append(Group.Parts);
            Test.TestTrue(TEXT("Character can be accepted"), Creator.Accept());
            for (const auto& Part : HiddenParts) if (Part.IsValid())
                Test.TestTrue(TEXT("Menu close restores the original visibility of every obstruction"),Part->IsVisible());
            Test.TestFalse(TEXT("World resumes after confirmation"),UGameplayStatics::IsGamePaused(S->GetWorld()));
            auto* Controller=S->GetWorld()->GetFirstPlayerController();
            Test.TestTrue(TEXT("The original player camera is restored"),Controller->GetViewTarget()==OriginalView.Get());
            const FVector Eye=Controller->GetPawnOrSpectator()->GetActorLocation();
            Test.TestTrue(TEXT("First person returns at character eye level"),FMath::IsNearlyEqual(Eye.Z-PlayerFeet.Z,S->Papyrus->PlayerEyeHeight()*S->GetActorScale3D().Z,.1));
            Test.TestTrue(TEXT("Standing eye level is above the old crouched offset"),S->Papyrus->PlayerEyeHeight()>160);
            uint32 Controls, Flags; float Rotation; S->Papyrus->GameState(Controls,Flags,Rotation);
            Test.TestFalse(TEXT("ShowRaceMenu pending flag clears"),(Flags & FT5PapyrusRuntime::RaceMenuPending) != 0);
            auto Catalog = S->Papyrus->CharacterCatalog();
            Test.TestEqual(TEXT("Committed choice is stored in native state"),Catalog->Read(TEXT("choice.bin")).Num(),int64(sizeof(FT5CharacterChoice)));
            TArray<uint8> After; Test.TestTrue(TEXT("Character state can be saved"),S->Papyrus->SaveCheckpoint(After));
            Test.TestTrue(TEXT("Checkpoint includes character state"), After != Before);
            WaitUntil = FPlatformTime::Seconds() + .5; ++Phase; return false;
        }
        if (Phase == 4)
        {
            if (FPlatformTime::Seconds() < WaitUntil) return false;
            Test.TestFalse(TEXT("Menu closes after acceptance"),S->CharacterCreator.IsValid());
            Test.TestTrue(TEXT("Simulation resumes after creation"),S->SimulationTime > PausedTime);
            S->OpenRaceMenu();
            if (!S->CharacterCreator) { Test.AddError(TEXT("Could not reopen RaceMenu")); return true; }
            Test.TestEqual(TEXT("Reopening retains character name"),S->CharacterCreator->Name,FString(TEXT("RaceMenu smoke")));
            S->CharacterCreator->Cancel(); ++Phase; return false;
        }
        if (Phase==20)
        {
            if (!S->CharacterCreator->bReady) return false;
            WaitUntil=FPlatformTime::Seconds()+1; Phase=21; return false;
        }
        if (Phase==21)
        {
            if (FPlatformTime::Seconds()<WaitUntil) return false;
            FScreenshotRequest::RequestScreenshot(ScreenshotPath(),true,false);
            WaitUntil=FPlatformTime::Seconds()+1; Phase=2; return false;
        }
        Test.TestFalse(TEXT("Voluntary RaceMenu can be cancelled"),S->CharacterCreator.IsValid());
        if (FParse::Param(FCommandLine::Get(),TEXT("T5RaceMenuContinueScene")))
        {
            if (S->SimulationTime<PausedTime+30) return false;
            Test.TestFalse(TEXT("Race-dependent dialogue no longer fails after confirmation"),S->Papyrus->RecentErrors().Contains(TEXT("Unsupported CTDA")));
            FString Log;
            const FString TestLogPath=FPlatformOutputDevices::GetAbsoluteLogFilename();
            GLog->Flush();
            Test.TestTrue(TEXT("Continuation log can be read"),FFileHelper::LoadFileToString(Log,*TestLogPath,
                FFileHelper::EHashOptions::None,FILEREAD_AllowWrite));
            const int32 Accepted=Log.Find(TEXT("T5_CHARACTER_COMMIT"),ESearchCase::CaseSensitive,ESearchDir::FromEnd);
            Test.TestTrue(TEXT("The original scene speaks again after creation"),Accepted!=INDEX_NONE && Log.Mid(Accepted).Contains(TEXT("script=SCEN event=Dialogue")));
        }
        return true;
    }
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FT5RaceMenuTest,"T5Data.RaceMenu",EAutomationTestFlags::ClientContext|EAutomationTestFlags::EngineFilter)
bool FT5RaceMenuTest::RunTest(const FString&)
{
    if (!FParse::Param(FCommandLine::Get(),TEXT("T5RaceMenuSmoke")))
    { AddInfo(TEXT("Use -T5LoadGame -T5RaceMenuSmoke with an installed official-master profile")); return true; }
    for (const auto& Context : GEngine->GetWorldContexts())
        if (auto* World=Context.World(); World && World->IsGameWorld())
            for (TActorIterator<AT5Scene> It(World); It; ++It)
            { ADD_LATENT_AUTOMATION_COMMAND(FT5RaceMenuSmoke(*this,*It)); return true; }
    AddError(TEXT("Startup scene not found")); return false;
}
#endif
