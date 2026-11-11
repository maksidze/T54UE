#include "T5Scene.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "T5Bridge.h"
#include "T5Mechanics.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "GameFramework/SpectatorPawn.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

class FT5SaveSmoke final : public IAutomationLatentCommand
{
    FAutomationTestBase& Test;
    TWeakObjectPtr<AT5Scene> Scene;
    double Started = FPlatformTime::Seconds(), SavedTime = 0;
    FVector SavedPlayer = FVector::ZeroVector;
    uint32 SavedFlags = 0, SavedControls = 0;
    int32 Phase = 0;
    double ObserveUntil = 0, MaxJointError = 0;
    TMap<uint32, FTransform> CartPoses;
public:
    FT5SaveSmoke(FAutomationTestBase& InTest, AT5Scene* InScene) : Test(InTest), Scene(InScene) {}
    bool Update() override
    {
        auto* S = Scene.Get();
        if (!S || S->bLoadingFailed || FPlatformTime::Seconds() - Started > 900)
        { Test.AddError(TEXT("Checkpoint smoke failed or timed out; see T5Papyrus.log")); return true; }
        if (!S->Papyrus || !S->LoadedScene || S->bSceneLoading || S->bStartupPreparing || S->bRestoringCheckpoint) return false;
        auto* Controller = S->GetWorld()->GetFirstPlayerController();
        auto* Pawn = Controller ? Controller->GetPawnOrSpectator() : nullptr;
        if (!Pawn) return false;
        uint32 Controls, Flags; float Sitting;
        S->Papyrus->GameState(Controls, Flags, Sitting);
        const bool ReadOnly = FParse::Param(FCommandLine::Get(), TEXT("T5LoadGame"));
        if (Phase == 0 && !ReadOnly)
        {
            double SaveAfter = 12;
            FParse::Value(FCommandLine::Get(), TEXT("T5SaveAfter="), SaveAfter);
            if (S->SimulationTime < SaveAfter) return false;
            if (FParse::Param(FCommandLine::Get(), TEXT("T5CheckpointAfterRaceMenu")) && !(Flags & FT5PapyrusRuntime::RaceMenuPending)) return false;
            SavedTime = S->SimulationTime; SavedPlayer = Pawn->GetPawnViewLocation(); SavedFlags = Flags; SavedControls = Controls;
            S->QuickSave();
            TArray<uint8> Data;
            Test.TestTrue(TEXT("Checkpoint written to the isolated smoke slot"), UGameplayStatics::LoadDataFromSlot(Data, TEXT("T5CheckpointSmoke"), 0));
            TArray<uint8> Expected; FMemoryWriter Writer(Expected);
            Writer << SavedTime << SavedPlayer << SavedFlags << SavedControls;
            UGameplayStatics::SaveDataToSlot(Expected, TEXT("T5CheckpointSmokeExpected"), 0);
            if (FParse::Param(FCommandLine::Get(), TEXT("T5CheckpointWriteOnly"))) return true;
            ++Phase; return false;
        }
        if (Phase == 1)
        {
            if (S->SimulationTime < SavedTime + .5 && !(Flags & FT5PapyrusRuntime::RaceMenuPending)) return false;
            Pawn->AddActorWorldOffset(FVector(100, 0, 0));
            S->QuickLoad(); ++Phase; return false;
        }
        if (ReadOnly && Phase == 0)
        {
            TArray<uint8> Expected;
            if (FParse::Param(FCommandLine::Get(), TEXT("T5CheckpointObserveOnly")))
            { SavedTime = S->SimulationTime; SavedPlayer = Pawn->GetPawnViewLocation(); SavedFlags = Flags; SavedControls = Controls; }
            else if (!UGameplayStatics::LoadDataFromSlot(Expected, TEXT("T5CheckpointSmokeExpected"), 0))
            { Test.AddError(TEXT("Cross-process expected checkpoint is missing")); return true; }
            if (!Expected.IsEmpty()) { FMemoryReader Reader(Expected); Reader << SavedTime << SavedPlayer << SavedFlags << SavedControls; }
            Phase = 2;
        }
        if (Phase == 2)
        {
            Test.TestTrue(TEXT("Simulation resumes at the saved clock"), FMath::Abs(S->SimulationTime - SavedTime) < .3);
            Test.TestEqual(TEXT("Player control mask restored"), Controls, SavedControls);
            Test.TestEqual(TEXT("Quest/game flags restored"), Flags, SavedFlags);
            Test.TestTrue(TEXT("Player restored near the saved position"), Pawn->GetPawnViewLocation().Equals(SavedPlayer, 100));
            Test.TestEqual(TEXT("All spawned references restored before reveal"), S->LoadedSpawnCount, S->Papyrus->SpawnedCount());
            Test.TestTrue(TEXT("Native state can be saved again after load"), [&] { TArray<uint8> Bytes; return S->Papyrus->SaveCheckpoint(Bytes); }());
            FT5LoadRequest References; S->Papyrus->Capture(References);
            TArray<uint32> Ids = References.TrackedReferences;
            for (const auto& Ref : References.SpawnedReferences) Ids.AddUnique(Ref.Id);
            for (uint32 Id : Ids)
                if (S->Mechanics && S->Mechanics->ArticulationError(Id))
                {
                    FTransform Authored; uint32 Parent;
                    if (const auto Pose = S->Papyrus->ReferenceTransform(Id, Authored, Parent))
                        CartPoses.Add(Id, S->Mechanics->CorrectPose(Id, Parent, *Pose));
                }
            if (FParse::Param(FCommandLine::Get(), TEXT("T5CheckpointRequireCarts")))
                Test.TestTrue(TEXT("A live articulated cart is exercised after loading"), CartPoses.Num() > 0);
            double Seconds = 10; FParse::Value(FCommandLine::Get(), TEXT("T5CheckpointObserve="), Seconds);
            ObserveUntil = S->GetWorld()->GetTimeSeconds() + Seconds;
            Phase = 3;
            return false;
        }
        if (Phase == 3)
        {
            for (const auto& [Id, Initial] : CartPoses)
            {
                if (const auto Error = S->Mechanics->ArticulationError(Id))
                {
                    MaxJointError = FMath::Max(MaxJointError, *Error);
                    if (*Error > 20)
                    { Test.AddError(FString::Printf(TEXT("Loaded cart %08X exploded: joint error %.3f cm"), Id, *Error)); return true; }
                }
                FTransform Authored; uint32 Parent;
                if (const auto Pose = S->Papyrus->ReferenceTransform(Id, Authored, Parent))
                {
                    const auto Current = S->Mechanics->CorrectPose(Id, Parent, *Pose);
                    if (FVector::DotProduct(Initial.GetRotation().GetUpVector(), Current.GetRotation().GetUpVector()) < .5)
                    { Test.AddError(FString::Printf(TEXT("Loaded cart %08X overturned after resuming physics"), Id)); return true; }
                }
            }
            if (S->GetWorld()->GetTimeSeconds() < ObserveUntil) return false;
            Test.AddInfo(FString::Printf(TEXT("Observed %d loaded carts; maximum joint error %.3f cm"), CartPoses.Num(), MaxJointError));
            return true;
        }
        return false;
    }
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FT5CheckpointTest, "T5Data.Checkpoint",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FT5CheckpointTest::RunTest(const FString&)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("T5CheckpointSmoke")))
    { AddError(TEXT("Run with -T5CheckpointSmoke to isolate the save slot")); return false; }
    for (const auto& Context : GEngine->GetWorldContexts())
        if (auto* World = Context.World(); World && World->IsGameWorld())
            for (TActorIterator<AT5Scene> It(World); It; ++It)
            { ADD_LATENT_AUTOMATION_COMMAND(FT5SaveSmoke(*this, *It)); return true; }
    AddError(TEXT("No T5 scene")); return false;
}

class FT5SaveBrowserSmoke final : public IAutomationLatentCommand
{
    FAutomationTestBase& Test;
    TWeakObjectPtr<AT5Scene> Scene;
    double Start = FPlatformTime::Seconds(), WaitUntil = 0, TimeA = 0, TimeB = 0;
    int Phase = 0;
public:
    FT5SaveBrowserSmoke(FAutomationTestBase& InTest, AT5Scene* InScene) : Test(InTest), Scene(InScene) {}
    bool Update() override
    {
        auto* S = Scene.Get();
        if (!S || S->bLoadingFailed || FPlatformTime::Seconds() - Start > 600)
        { Test.AddError(TEXT("Save browser smoke failed or timed out")); return true; }
        if (!S->Papyrus || !S->LoadedScene || S->bStartupPreparing || S->bSceneLoading || S->bRestoringCheckpoint) return false;
        if (Phase == 0)
        {
            TimeA = S->SimulationTime;
            if (!S->SaveGameSlot(TEXT("T5SaveBrowserSmokeA"))) { Test.AddError(TEXT("First slot failed")); return true; }
            ++Phase; return false;
        }
        if (Phase == 1)
        {
            if (S->SimulationTime < TimeA + .5) return false;
            TimeB = S->SimulationTime;
            if (!S->SaveGameSlot(TEXT("T5SaveBrowserSmokeB"))) { Test.AddError(TEXT("Second slot failed")); return true; }
            S->OpenSaveBrowser();
            Test.TestTrue(TEXT("Save selection menu opens"), S->SaveBrowser.IsValid());
            Test.TestTrue(TEXT("Selection pauses the world"), UGameplayStatics::IsGamePaused(S->GetWorld()));
            WaitUntil = FPlatformTime::Seconds() + 1;
            ++Phase; return false;
        }
        if (Phase == 2)
        {
            if (FPlatformTime::Seconds() < WaitUntil) return false;
            Test.TestEqual(TEXT("Menu keeps the simulation clock paused"), S->SimulationTime, TimeB);
            S->LoadGameSlot(TEXT("T5SaveBrowserSmokeA")); ++Phase; return false;
        }
        if (Phase == 3)
        {
            Test.TestFalse(TEXT("Choosing a slot closes the menu"), S->SaveBrowser.IsValid());
            Test.TestFalse(TEXT("Choosing a slot resumes the world"), UGameplayStatics::IsGamePaused(S->GetWorld()));
            Test.TestTrue(TEXT("First slot restored its own clock"), FMath::Abs(S->SimulationTime - TimeA) < .2);
            S->LoadGameSlot(TEXT("T5SaveBrowserSmokeB")); ++Phase; return false;
        }
        Test.TestTrue(TEXT("Second slot restored a different clock"), FMath::Abs(S->SimulationTime - TimeB) < .2);
        Test.TestTrue(TEXT("Both save files remain independently loadable"),
            UGameplayStatics::DoesSaveGameExist(TEXT("T5SaveBrowserSmokeA"), 0)
            && UGameplayStatics::DoesSaveGameExist(TEXT("T5SaveBrowserSmokeB"), 0));
        return true;
    }
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FT5SaveBrowserTest, "T5Data.SaveBrowser",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FT5SaveBrowserTest::RunTest(const FString&)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("T5SaveBrowserSmoke")))
    { AddError(TEXT("Run with -T5SaveBrowserSmoke to isolate the two test slots")); return false; }
    for (const auto& Context : GEngine->GetWorldContexts())
        if (auto* World = Context.World(); World && World->IsGameWorld())
            for (TActorIterator<AT5Scene> It(World); It; ++It)
            { ADD_LATENT_AUTOMATION_COMMAND(FT5SaveBrowserSmoke(*this, *It)); return true; }
    AddError(TEXT("No T5 scene")); return false;
}
#endif
