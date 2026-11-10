#include "T5Scene.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Components/PoseableMeshComponent.h"
#include "Components/PostProcessComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "InputKeyEventArgs.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "UnrealClient.h"

DEFINE_LATENT_AUTOMATION_COMMAND_FOUR_PARAMETER(FT5MapSwitchSmoke, FAutomationTestBase*, Test, TWeakObjectPtr<AT5Scene>, Loader,
    int32, Phase, double, InteriorReadyAt);

namespace
{
    bool TestT5Skin(AActor& Actor)
    {
        auto* Visible = Actor.FindComponentByClass<USkeletalMeshComponent>();
        auto* Mesh = Visible ? Visible->GetSkeletalMeshAsset() : nullptr;
        if (!Mesh || Mesh->GetRefSkeleton().GetNum() < 2)
            return false;
        const auto& LOD = Mesh->GetResourceForRendering()->LODRenderData[0];
        auto& Weights = *Visible->GetSkinWeightBuffer(0);
        auto* Pose = NewObject<UPoseableMeshComponent>(&Actor);
        Pose->SetVisibility(false);
        Pose->SetSkinnedAssetAndUpdate(Mesh);
        Pose->RegisterComponent();
        bool bDeforms = false;
        for (const auto& Section : LOD.RenderSections)
        {
            if (Section.NumVertices == 0)
                continue;
            const int32 Vertex = Section.BaseVertexIndex;
            const auto Influence = Weights.GetVertexSkinWeights(Vertex);
            if (!Section.BoneMap.IsValidIndex(Influence.InfluenceBones[0]) || Influence.InfluenceWeights[0] == 0)
                continue;
            const auto Joint = Section.BoneMap[Influence.InfluenceBones[0]];
            if (!Joint)
                continue;
            const FName Bone = Mesh->GetRefSkeleton().GetBoneName(Joint);
            const auto Before = USkinnedMeshComponent::GetSkinnedVertexPosition(Pose, Vertex, LOD, Weights);
            const auto Position = Pose->GetBoneLocationByName(Bone, EBoneSpaces::ComponentSpace);
            Pose->SetBoneLocationByName(Bone, Position + FVector(20, 0, 0), EBoneSpaces::ComponentSpace);
            Pose->RefreshBoneTransforms();
            const auto After = USkinnedMeshComponent::GetSkinnedVertexPosition(Pose, Vertex, LOD, Weights);
            bDeforms = FVector3f::Distance(Before, After) > 1.0f;
            if (bDeforms)
                break;
        }
        Pose->DestroyComponent();
        return bDeforms;
    }
}

bool FT5MapSwitchSmoke::Update()
{
    auto* Scene = Loader.Get();
    if (!Scene || GetCurrentRunTime() > 180)
    {
        Test->AddError(TEXT("T5 map smoke test timed out."));
        return true;
    }
    auto* Controller = Scene->GetWorld()->GetFirstPlayerController();
    if (!Controller || !Controller->GetPawnOrSpectator() || Scene->bSceneLoading)
        return false;
    switch (Phase)
    {
    case 0:
        if (Scene->ActiveWorld != 0x3c)
            return false;
        Test->TestEqual(TEXT("Initial cell"), Scene->RequestedCell, FIntPoint(4, -20));
        // Cross the east boundary of cell (4,-20) without changing its north coordinate.
        Controller->GetPawnOrSpectator()->SetActorLocation(FVector(-114103, 32183, 12700));
        ++Phase;
        break;
    case 1:
        if (Scene->RequestedCell != FIntPoint(5, -20))
            return false;
        Test->TestTrue(TEXT("Streamed scene"), Scene->LoadStatus.IsEmpty());
        Scene->OpenT5Map(TEXT("WhiterunWorld"));
        ++Phase;
        break;
    case 2:
        if (Scene->ActiveWorld != 0x1a26f)
            return false;
        Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::M, IE_Pressed, FPlatformTime::Cycles64()));
        Controller->InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::M, IE_Released, FPlatformTime::Cycles64()));
        ++Phase;
        break;
    case 3:
        if (!Scene->MapBrowser)
            return false;
        FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Screenshots/T5Maps.png"), true, false);
        ++Phase;
        break;
    case 4:
        if (FScreenshotRequest::IsScreenshotRequested())
            return false;
        Scene->ToggleMapBrowser();
        Scene->OpenT5Map(TEXT("RiverwoodSleepingGiantInn"));
        ++Phase;
        break;
    case 5:
        if (Scene->ActiveWorld != 0 || !IsValid(Scene->LoadedScene))
            return false;
        if (InteriorReadyAt == 0)
            InteriorReadyAt = GetCurrentRunTime();
        if (GetCurrentRunTime() - InteriorReadyAt < 2)
            return false;
        Test->TestTrue(TEXT("Loaded interior"), Scene->LoadStatus.IsEmpty());
        Test->TestTrue(TEXT("A weighted NIF vertex follows its authored bone"), TestT5Skin(*Scene->LoadedScene));
        if (const auto* Post = Scene->LoadedScene->FindComponentByClass<UPostProcessComponent>())
        {
            const FVector4 Gain = Post->Settings.ColorGain;
            Test->TestTrue(TEXT("Authored brightness applies to every color channel"),
                Gain.X == 1 && Gain.Y == 1 && Gain.Z == 1 && FMath::IsNearlyEqual(Gain.W, 1.15, 0.001));
        }
        else
            Test->AddError(TEXT("Interior post-process component missing."));
        FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Screenshots/T5Interior.png"), false, false);
        ++Phase;
        break;
    default:
        if (!FScreenshotRequest::IsScreenshotRequested())
        {
            return true;
        }
        break;
    }
    return false;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FT5MapSwitchTest, "T5Data.MapSwitchSmoke", EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FT5MapSwitchTest::RunTest(const FString&)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("T5MapSmoke")))
    {
        AddInfo(TEXT("Pass -T5MapSmoke -T5Map=Tamriel -T5CellX=4 -T5CellY=-20 with the official masters to run the map smoke test."));
        return true;
    }
    FString Map;
    FParse::Value(FCommandLine::Get(), TEXT("T5Map="), Map);
    const int32 FirstPhase = Map.Equals(TEXT("RiverwoodSleepingGiantInn"), ESearchCase::IgnoreCase) ? 5 : 0;
    for (const auto& Context : GEngine->GetWorldContexts())
        if (UWorld* World = Context.World(); World && World->IsGameWorld())
            for (TActorIterator<AT5Scene> It(World); It; ++It)
            {
                ADD_LATENT_AUTOMATION_COMMAND(FT5MapSwitchSmoke(this, *It, FirstPhase, 0.0));
                return true;
            }
    AddError(TEXT("Run the game startup map with T5 Data and an active plugin list."));
    return false;
}
#endif
