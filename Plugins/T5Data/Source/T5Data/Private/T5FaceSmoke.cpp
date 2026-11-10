#include "T5Scene.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "T5Lip.h"
#include "T5Bridge.h"
#include "T5Mechanics.h"
#include "T5HkxAnimInstance.h"
#include "Animation/MorphTarget.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "EngineUtils.h"
#include "Misc/AutomationTest.h"
#include "Materials/Material.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"

class FT5LoadingSmoke final : public IAutomationLatentCommand
{
public:
    FT5LoadingSmoke(FAutomationTestBase& InTest, AT5Scene& InScene) : Test(InTest), Scene(&InScene) {}
    bool Update() override
    {
        auto* S = Scene.Get();
        if (!S || GetCurrentRunTime() > 180) { Test.AddError(TEXT("Loading screen never reached ready state")); return true; }
        if (S->bLoadingFailed) { Test.AddError(S->LoadStatus); return true; }
        if (S->bSceneLoading)
        {
            bSawScreen |= S->LoadingScreen.IsValid();
            if (S->LoadProgress)
            {
                FScopeLock Lock(&S->LoadProgress->Mutex);
                const auto& P = *S->LoadProgress;
                bSawCounts |= P.Total > 0 && P.Done > 0;
                if (P.Total && P.Done > P.Total) { Test.AddError(TEXT("Progress exceeds stage total")); return true; }
                if (!bCaptured && P.Total && P.Done > 0)
                {
                    FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Screenshots/T5Loading.png"), true, false);
                    bCaptured = true;
                }
            }
            if (bWasBlocking && S->bFullLoadingScreen && Generation == S->LoadGeneration)
                if (!Test.TestEqual(TEXT("Simulation clock is frozen while initial location loads"), S->SimulationTime, PreviousTime)) return true;
        }
        bWasBlocking = S->bSceneLoading && S->bFullLoadingScreen;
        PreviousTime = S->SimulationTime;
        Generation = S->LoadGeneration;
        if (S->bStartupPreparing || S->bSceneLoading || !S->LoadedScene) return false;
        Test.TestTrue(TEXT("Loading screen was visible"), bSawScreen);
        Test.TestTrue(TEXT("Real progress counts were reported"), bSawCounts);
        Test.TestTrue(TEXT("Authored camera was applied before revealing the world"), S->bOpeningViewReady);
        Test.TestTrue(TEXT("Spawned references were loaded before revealing the world"),
            S->Papyrus && S->LoadedSpawnCount == S->Papyrus->SpawnedCount());
        if (Test.TestTrue(TEXT("Mechanics initialized"), S->Mechanics.IsValid()))
        {
            Test.TestTrue(TEXT("Horse skeletons registered for source-rig support"), !S->Mechanics->GroundActors.IsEmpty());
            for (const auto& [Id, Actor] : S->Mechanics->GroundActors)
                Test.TestTrue(TEXT("Registered horse has support from its authored hooves"), Actor.bGrounded && Actor.Feet.Num() >= 3);
        }
        Test.AddInfo(TEXT("Initial load and quest-created character reload completed before reveal."));
        return true;
    }
private:
    FAutomationTestBase& Test; TWeakObjectPtr<AT5Scene> Scene;
    bool bSawScreen = false, bSawCounts = false, bCaptured = false, bWasBlocking = false;
    double PreviousTime = 0;
    uint64 Generation = 0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FT5LoadingTest, "T5Data.Loading.Initial", EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FT5LoadingTest::RunTest(const FString&)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("T5NewGame")))
    { AddError(TEXT("Supply -T5NewGame for the initial loading test")); return false; }
    for (const auto& Context : GEngine->GetWorldContexts())
        if (auto* World = Context.World(); World && World->IsGameWorld())
            for (TActorIterator<AT5Scene> It(World); It; ++It)
            { FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FT5LoadingSmoke>(*this, **It)); return true; }
    AddError(TEXT("No runtime T5 scene")); return false;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FT5LipTest, "T5Data.Face.Lip", EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FT5LipTest::RunTest(const FString&)
{
    FT5LipTrack Timing;
    Timing.FirstFrame = -8;
    Timing.Samples.Init(0.f, 66);
    Timing.Samples[0] = .25f; Timing.Samples[33] = .75f;
    TestEqual(TEXT("Negative metadata start does not advance voice samples"), Timing.Sample(0, 0), .25f);
    TestEqual(TEXT("Voice time interpolates between adjacent phoneme frames"), Timing.Sample(0, 1./60.), .5f);
    FString Path;
    if (!FParse::Value(FCommandLine::Get(), TEXT("T5LipTest="), Path))
    { AddError(TEXT("Supply -T5LipTest with an original FUZ file.")); return false; }
    TArray<uint8> Bytes;
    if (!FFileHelper::LoadFileToArray(Bytes, *Path)) { AddError(TEXT("Cannot read FUZ test file")); return false; }
    FString Error;
    const auto Track = DecodeT5FuzLip(Bytes, Error);
    if (!TestTrue(TEXT("Original compressed LIP decodes"), Track.IsValid())) { AddError(Error); return false; }
    TestTrue(TEXT("LIP has samples"), Track->Samples.Num() > 33);
    TestTrue(TEXT("Original speech contains nonzero phonemes"), Track->Samples.ContainsByPredicate([](float V){ return V > .1f; }));
    TestEqual(TEXT("No animation beyond the voice"), Track->Sample(0, 100000), 0.f);
    uint32 LipBytes; FMemory::Memcpy(&LipBytes, Bytes.GetData()+8, 4);
    Bytes.SetNum(12+LipBytes/2);
    TestFalse(TEXT("Truncated FUZ is rejected"), DecodeT5FuzLip(Bytes, Error).IsValid());
    AddInfo(FString::Printf(TEXT("LIP frames=%d first=%d"), Track->Samples.Num()/33, Track->FirstFrame));
    return true;
}

class FT5FaceRuntimeSmoke final : public IAutomationLatentCommand
{
public:
    FT5FaceRuntimeSmoke(FAutomationTestBase& InTest, AT5Scene& InScene) : Test(InTest), Scene(&InScene) {}
    bool Update() override
    {
        if (bCaptured)
        {
            if (FScreenshotRequest::IsScreenshotRequested()) return false;
            Test.AddInfo(TEXT("Original TRI morphs, SCEN gaze and audible FUZ phonemes are active on runtime NPCs."));
            return true;
        }
        auto* Loader = Scene.Get();
        if (!Loader || GetCurrentRunTime() > 160) { Test.AddError(TEXT("No authored gaze and speech morphs observed in MQ101")); return true; }
        for (const auto& [Id, Parts] : Loader->ReferenceVisuals)
        {
            const auto* Face = Loader->Faces.Find(Id);
            for (const auto& Part : Parts)
                if (auto* Mesh = Cast<USkeletalMeshComponent>(Part.Component.Get()))
                {
                    if (!Mesh->GetSkeletalMeshAsset()->FindMorphTarget(TEXT("Aah"))) continue;
                    bMorphs = true;
                    if (Face && Face->bTracking && Face->Target && Cast<UT5HkxAnimInstance>(Mesh->GetAnimInstance())) bGaze = true;
                    for (int32 I=0; I<16; ++I)
                        bSpeech |= Mesh->GetMorphTarget(T5LipMorphNames()[I]) > .1f;
                }
        }
        if (!(bMorphs && bGaze && bSpeech)) return false;
        for (const auto& [Id, Parts] : Loader->ReferenceVisuals)
            for (const auto& Part : Parts)
                if (auto* Mesh = Cast<USkeletalMeshComponent>(Part.Component.Get()))
                    if (Mesh->GetSkeletalMeshAsset()->FindMorphTarget(TEXT("Aah")))
                        for (auto* Material : Mesh->GetMaterials())
                            Test.TestTrue(TEXT("Cooked facial material supports morph rendering"),
                                Material && Material->CheckMaterialUsage_Concurrent(MATUSAGE_MorphTargets));
        FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Screenshots/T5FaceSpeech.png"), false, false);
        bCaptured = true;
        return false;
    }
private:
    FAutomationTestBase& Test;
    TWeakObjectPtr<AT5Scene> Scene;
    bool bMorphs=false, bGaze=false, bSpeech=false, bCaptured=false;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FT5FaceRuntimeTest, "T5Data.Face.Runtime", EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FT5FaceRuntimeTest::RunTest(const FString&)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("T5FaceSmoke")))
    { AddError(TEXT("Supply -T5FaceSmoke with -T5NewGame -T5Quest=0003372B")); return false; }
    for (const auto& Context : GEngine->GetWorldContexts())
        if (auto* World=Context.World(); World && World->IsGameWorld())
            for (TActorIterator<AT5Scene> It(World); It; ++It)
            { FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FT5FaceRuntimeSmoke>(*this, **It)); return true; }
    AddError(TEXT("No runtime T5 scene")); return false;
}
#endif
