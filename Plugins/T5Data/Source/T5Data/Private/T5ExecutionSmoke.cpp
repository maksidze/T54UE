#include "T5Hkx.h"
#include "T5HkxCamera.h"
#include "T5Coordinates.h"
#include "T5Scene.h"
#include "T5Bridge.h"
#include "T5CharacterCreator.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "UnrealClient.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "HAL/FileManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FT5ExecutionAssetsTest, "T5Data.ExecutionAssets",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FT5ExecutionAssetsTest::RunTest(const FString&)
{
    FString Directory, Error;
    if (!FParse::Value(FCommandLine::Get(), TEXT("T5ExecutionData="), Directory))
    { AddError(TEXT("Pass -T5ExecutionData with the original execution fixtures")); return false; }
    auto Read = [&Directory](FString Path, TArray<uint8>& Bytes) {
        Path.ReplaceInline(TEXT("\\"), TEXT("/")); Path.ToLowerInline();
        return FFileHelper::LoadFileToArray(Bytes, *(Directory / Path));
    };
    const FVector Local(4.88, 88.38, 36.21);
    const FQuat Heading(FVector::UpVector, 1.37);
    TestTrue(TEXT("World and skeletal rotations use the same handedness conversion"),
        T5ToUnrealRotation(Heading).RotateVector(T5ToUnrealDirection(Local))
            .Equals(T5ToUnrealDirection(Heading.RotateVector(Local)), .00001));
    FT5HkxCameraClip Camera;
    const bool bCamera = LoadT5HkxPlayerCamera(TEXT("actors/character/behaviors/0_master.hkx"),
        TEXT("IdleExecutionerChop_Player"), Read, Camera, Error, TEXT("actors/character/animations/aoexecutioneechop.hkx"));
    TestTrue(TEXT("The player execution has an original first-person camera clip"), bCamera);
    if (!bCamera) AddError(Error);
    else TestFalse(TEXT("Player camera contains a posed camera bone"), SampleT5HkxPlayerCamera(Camera, 3).Equals(FTransform::Identity));
    for (const TCHAR* Clip : {TEXT("aoexecutioneechop.hkx"), TEXT("aoexecutionerchopplayer.hkx")})
    {
        FT5HkxPlaybackClock Clock;
        const bool bLoaded = LoadT5HkxAnimation(TEXT("actors/character/behaviors/0_master.hkx"),
            TEXT("IdleExecutionerChop_Player"), FString(TEXT("actors/character/animations/")) + Clip,
            Read, Error, {}, 1, false, false, Clock);
        TestTrue(FString(TEXT("Role-specific player execution loads: ")) + Clip, bLoaded);
        if (!bLoaded) AddError(Error);
    }
    TArray<uint8> Graph;
    TestTrue(TEXT("Original movement graph fixture exists"), Read(TEXT("meshes/actors/character/behaviors/mt_behavior.hkx"), Graph));
    FT5HkxPlayback Enter;
    const bool bEnter = ResolveT5HkxEvent(Graph, TEXT("IdleExecutionerIdle"), Enter, Error);
    TestTrue(TEXT("The executioner's entry already draws the axe declared by its parent state"), bEnter
        && Enter.EnterEvents.Contains(TEXT("AnimObjDraw.AnimObjectExecutionerAxe")));
    if (!bEnter) AddError(Error);
    FT5HkxPlayback Stop;
    const bool bStop = ResolveT5HkxEvent(Graph, TEXT("OffsetStop"), Stop, Error);
    TestTrue(TEXT("OffsetStop clears the arm layer through the authored reset"), bStop && Stop.bClearLayer);
    if (!bStop) AddError(Error);
    return true;
}

class FT5ExecutionFlowSmoke final : public IAutomationLatentCommand
{
    FAutomationTestBase& Test;
    TWeakObjectPtr<AT5Scene> Scene;
    double Started = FPlatformTime::Seconds(), Poll = 0;
public:
    FT5ExecutionFlowSmoke(FAutomationTestBase& InTest, AT5Scene* InScene) : Test(InTest), Scene(InScene) {}
    bool Update() override
    {
        auto* S = Scene.Get();
        if (!S || S->bLoadingFailed || FPlatformTime::Seconds() - Started > 600)
        { Test.AddError(TEXT("Actual execution flow failed or timed out before player control returned")); return true; }
        if (S->CharacterCreator && FParse::Param(FCommandLine::Get(), TEXT("T5ReadOnly")))
        {
            if (S->CharacterCreator->bReady)
                Test.TestTrue(TEXT("Read-only flow confirms the original character-creation boundary"), S->CharacterCreator->Accept());
            return false;
        }
        if (!S->Papyrus || S->bSceneLoading || S->bStartupPreparing || S->bRestoringCheckpoint) return false;
        if (FPlatformTime::Seconds() < Poll) return false;
        Poll = FPlatformTime::Seconds() + 1;
        FString Path, Log;
        FParse::Value(FCommandLine::Get(), TEXT("T5FlowLog="), Path);
        GLog->Flush();
        if (!FFileHelper::LoadFileToString(Log, *Path, FFileHelper::EHashOptions::None, FILEREAD_AllowWrite)) return false;
        const int32 Loaded = Log.Find(TEXT("T5_LOAD_OK"), ESearchCase::CaseSensitive, ESearchDir::FromStart);
        if (Loaded == INDEX_NONE) return false;
        Log.RightChopInline(Loaded);
        if (!Log.Contains(TEXT("T5_QUEST_STAGE MQ101 stage=160"))) return false;
        uint32 Controls, Flags; float Rotation;
        S->Papyrus->GameState(Controls, Flags, Rotation);
        Test.TestEqual(TEXT("Movement is restored by the original stage-160 fragment"), Controls & 1u, 0u);
        Test.TestEqual(TEXT("Player is neither AI-driven nor restrained"), Flags & (4u | 128u), 0u);
        Test.TestFalse(TEXT("Player execution clip resolves"), Log.Contains(TEXT("T5_ANIMATION_LOAD_ERROR ref=00000014")));
        Test.TestTrue(TEXT("On-foot execution uses an original first-person camera"), Log.Contains(TEXT("event=IdleExecutionerChop_Player"))
            && Log.Contains(TEXT("T5_HKX_CAMERA idle=")));
        Test.TestTrue(TEXT("Real player reaction dispatches the furniture exit"), Log.Contains(TEXT("ref=00000014 event=IdleFurnitureExit")));
        AddInfo();
        return true;
    }
    void AddInfo() { Test.AddInfo(TEXT("Original save progressed through execution, Alduin arrival and actual HKX events to stage 160")); }
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FT5ExecutionFlowTest, "T5Data.ExecutionFlow",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FT5ExecutionFlowTest::RunTest(const FString&)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("T5ExecutionFlowSmoke")))
    { AddError(TEXT("Pass -T5ExecutionFlowSmoke and -T5FlowLog")); return false; }
    for (const auto& Context : GEngine->GetWorldContexts())
        if (auto* World = Context.World(); World && World->IsGameWorld())
            for (TActorIterator<AT5Scene> It(World); It; ++It)
            { ADD_LATENT_AUTOMATION_COMMAND(FT5ExecutionFlowSmoke(*this, *It)); return true; }
    AddError(TEXT("No game scene")); return false;
}
#endif
