#include "T5Scene.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Engine/Engine.h"
#include "Components/StaticMeshComponent.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpectatorPawn.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "T5Coordinates.h"
namespace { constexpr double CacheSmokeCellSize = 4096.; }

DEFINE_LATENT_AUTOMATION_COMMAND_FOUR_PARAMETER(FT5ResourceCacheSmoke, FAutomationTestBase*, Test,
    TWeakObjectPtr<AT5Scene>, Loader, int32, Phase, double, ReadyAt);

bool FT5ResourceCacheSmoke::Update()
{
    auto* Scene = Loader.Get();
    if (!Scene || GetCurrentRunTime() > 420) { Test->AddError(TEXT("Resource cache smoke timed out")); return true; }
    if (Scene->bSceneLoading || !Scene->ActiveLocation) return false;
    if (Phase == 10)
    {
        if (Scene->bRestoringCheckpoint || Scene->bStartupPreparing || !IsValid(Scene->LoadedScene)) return false;
        Test->TestTrue(TEXT("Saved location is fully restored"), Scene->LoadStatus.IsEmpty());
        Test->TestTrue(TEXT("Static mesh retention is enabled in the packaged runtime"), !Scene->CachedStaticMeshes.IsEmpty());
        Test->AddInfo(FString::Printf(TEXT("T5_CACHE_STARTUP_READY location=%08X cell=%d,%d"), Scene->ActiveLocation, Scene->RequestedCell.X, Scene->RequestedCell.Y));
        return true;
    }
    auto* Controller = Scene->GetWorld()->GetFirstPlayerController();
    auto* Pawn = Controller ? Controller->GetPawnOrSpectator() : nullptr;
    if (!Pawn) return false;
    if (Phase == 0)
    {
        Test->TestTrue(TEXT("Static meshes retained"), !Scene->CachedStaticMeshes.IsEmpty());
        Test->TestEqual(TEXT("Full-detail radius"), Scene->CellRadius, 2);
        const auto Cell = Scene->RequestedCell;
        const FVector Position((Cell.X + 1.1) * CacheSmokeCellSize, (Cell.Y + .5) * CacheSmokeCellSize, 15000);
        Pawn->SetActorLocation(Scene->GetActorTransform().TransformPosition(T5ToUnrealPosition(Position)));
        ReadyAt = GetCurrentRunTime(); ++Phase;
    }
    else if (Phase == 1)
    {
        if (GetCurrentRunTime() - ReadyAt < .5) return false;
        Test->TestEqual(TEXT("Boundary margin prevents reload"), Scene->RequestedCell, FIntPoint(4, -20));
        const FVector Position(5.35 * CacheSmokeCellSize, -19.5 * CacheSmokeCellSize, 15000);
        Pawn->SetActorLocation(Scene->GetActorTransform().TransformPosition(T5ToUnrealPosition(Position)));
        ++Phase;
    }
    else if (Phase == 2)
    {
        if (Scene->RequestedCell != FIntPoint(5, -20)) return false;
        TInlineComponentArray<UStaticMeshComponent*> StaticMeshes(Scene->LoadedScene);
        int32 EligibleMeshes = 0;
        for (const auto* Mesh : StaticMeshes)
            if (Mesh->ComponentTags.ContainsByPredicate([](FName Tag) { return Tag.ToString().StartsWith(TEXT("T5Cache:")); }))
                ++EligibleMeshes;
        Test->AddInfo(FString::Printf(TEXT("Neighbor reused %d/%d render meshes"), Scene->LastMeshCacheHits, EligibleMeshes));
        Test->TestTrue(TEXT("Neighbor window reuses most static meshes"), EligibleMeshes > 0 && Scene->LastMeshCacheHits * 2 > EligibleMeshes);
        Test->TestTrue(TEXT("Neighbor window completes"), Scene->LoadStatus.IsEmpty());
        Scene->LoadLocation(Scene->ActiveWorld, Scene->RequestedCell, false); ++Phase;
    }
    else
    {
        Test->TestTrue(TEXT("Repeated window reuses built meshes"), Scene->LastMeshCacheHits > 0);
        Test->TestTrue(TEXT("Repeated window completes"), Scene->LoadStatus.IsEmpty());
        return true;
    }
    return false;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FT5ResourceCacheTest, "T5Data.ResourceCache",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FT5ResourceCacheTest::RunTest(const FString&)
{
    const bool bStartup = FParse::Param(FCommandLine::Get(), TEXT("T5CacheStartupSmoke"));
    if (!bStartup && !FParse::Param(FCommandLine::Get(), TEXT("T5CacheSmoke")))
    { AddInfo(TEXT("Pass -T5CacheSmoke -T5Map=Tamriel -T5CellX=4 -T5CellY=-20 with official masters")); return true; }
    for (const auto& Context : GEngine->GetWorldContexts())
        if (auto* World = Context.World(); World && World->IsGameWorld())
            for (TActorIterator<AT5Scene> It(World); It; ++It)
            { ADD_LATENT_AUTOMATION_COMMAND(FT5ResourceCacheSmoke(this, *It, bStartup ? 10 : 0, 0.)); return true; }
    AddError(TEXT("T5 scene missing")); return false;
}
#endif
