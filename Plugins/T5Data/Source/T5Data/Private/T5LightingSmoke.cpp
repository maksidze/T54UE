#include "T5Scene.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "T5Bridge.h"
#include "Components/PostProcessComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/SkyLight.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "UnrealClient.h"

class FT5LightingSmoke : public IAutomationLatentCommand
{
    FAutomationTestBase& Test;
    TWeakObjectPtr<AT5Scene> Scene;
    double Started = FPlatformTime::Seconds(), Ready = 0;
    bool Captured = false;
public:
    FT5LightingSmoke(FAutomationTestBase& InTest, AT5Scene* InScene) : Test(InTest), Scene(InScene) {}
    virtual bool Update() override
    {
        if (!Scene.IsValid()) { Test.AddError(TEXT("Scene destroyed")); return true; }
        if (FPlatformTime::Seconds() - Started > 240) { Test.AddError(TEXT("Lighting startup timed out")); return true; }
        if (Scene->bSceneLoading || Scene->bRestoringCheckpoint || Scene->bStartupPreparing || !Scene->Papyrus) return false;
        if (!Ready) Ready = FPlatformTime::Seconds();
        if (FPlatformTime::Seconds() - Ready < 8) return false;
        const auto Path = FPaths::ProjectSavedDir() / TEXT("Screenshots/T5Lighting.png");
        if (!Captured)
        {
            Test.TestTrue(TEXT("Saved game restored and clock resumed"), Scene->SimulationTime > 1 && Scene->PendingNativeCheckpoint.IsEmpty());
            bool Adaptive = false;
            for (TObjectIterator<UPostProcessComponent> It; It; ++It)
                if (It->GetWorld() == Scene->GetWorld() && It->IsRegistered() && It->Priority == 1000)
                    Adaptive |= It->Settings.bOverride_AutoExposureMinBrightness && It->Settings.bOverride_AutoExposureMaxBrightness
                        && It->Settings.AutoExposureMinBrightness < It->Settings.AutoExposureMaxBrightness;
            Test.TestTrue(TEXT("Adaptive cell exposure overrides cooked fixed exposure"), Adaptive);
            bool bWeatherSky = false;
            for (TActorIterator<ASkyLight> It(Scene->GetWorld()); It; ++It)
            {
                auto* Sky = Cast<USkyLightComponent>(It->GetLightComponent());
                if (!Sky || !Sky->IsVisible()) continue;
                bWeatherSky = true;
                Test.TestEqual(TEXT("Captured sky is not attenuated twice"), Sky->LightColor, FColor::White);
                Test.TestTrue(TEXT("Weather ambient reaches runtime characters"), Sky->SourceType == SLS_SpecifiedCubemap
                    && Sky->Cubemap && Sky->Intensity > 1);
            }
            Test.TestEqual(TEXT("Cached indirect lighting covers the active exposure range"),
                IConsoleManager::Get().FindConsoleVariable(TEXT("r.EyeAdaptation.CachedLightingPreExposure"))->GetFloat(),
                bWeatherSky ? 8.f : 2.f);
            FScreenshotRequest::RequestScreenshot(Path, true, false);
            Captured = true; return false;
        }
        if (FScreenshotRequest::IsScreenshotRequested()) return false;
        Test.TestTrue(TEXT("Rendered lighting screenshot written"), IFileManager::Get().FileSize(*Path) > 10000);
        Test.AddInfo(FString::Printf(TEXT("Screenshot: %s"), *Path));
        return true;
    }
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FT5LightingTest, "T5Data.Lighting",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FT5LightingTest::RunTest(const FString&)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("T5LightingSmoke")))
    { AddInfo(TEXT("Use -T5LoadGame -T5ReadOnly -T5LightingSmoke")); return true; }
    for (const auto& Context : GEngine->GetWorldContexts())
        if (auto* World = Context.World(); World && World->IsGameWorld())
            for (TActorIterator<AT5Scene> It(World); It; ++It)
            { ADD_LATENT_AUTOMATION_COMMAND(FT5LightingSmoke(*this, *It)); return true; }
    AddError(TEXT("T5 scene missing")); return false;
}
#endif
