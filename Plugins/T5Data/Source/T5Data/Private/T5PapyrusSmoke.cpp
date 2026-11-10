#include "T5ReferenceAnimation.h"
#include "T5Scene.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "T5Bridge.h"
#include "T5Coordinates.h"
#include "T5Mechanics.h"
#include "Camera/CameraActor.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "InputKeyEventArgs.h"
#include "Layout/Children.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UnrealClient.h"
#include "Widgets/SWidget.h"

class FT5PapyrusSmoke final : public IAutomationLatentCommand
{
public:
    FT5PapyrusSmoke(FAutomationTestBase& InTest, AT5Scene* InScene) : Test(InTest), Loader(InScene) {}
    bool Update() override;

private:
    static constexpr uint32 Lever = 0x01000a00, Target = 0x01000a01;
    bool AimAtLever(AT5Scene& Scene, APlayerController& Controller);
    static void PressInteract(APlayerController& Controller);
    void CheckTarget(AT5Scene& Scene, bool bDisabled);

    FAutomationTestBase& Test;
    TWeakObjectPtr<AT5Scene> Loader;
    TWeakObjectPtr<AActor> OriginalCell;
    int32 Phase = 0;
    double ActivationAt = 0;
};

bool FT5PapyrusSmoke::AimAtLever(AT5Scene& Scene, APlayerController& Controller)
{
    auto* Pawn = Controller.GetPawnOrSpectator();
    if (!Pawn)
        return false;
    for (const auto& [Key, Id] : Scene.InteractionComponents)
    {
        const auto* Component = Key.Get();
        if (Id != Lever || !Component || Component->GetCollisionEnabled() == ECollisionEnabled::NoCollision)
            continue;
        const auto Point = Component->Bounds.Origin;
        for (const auto& Direction : {FVector(0, -1, .35), FVector(-1, 0, .35), FVector(0, 1, .35), FVector(1, 0, .35)})
        {
            const auto Eye = Point + Direction * 225;
            FHitResult Hit;
            if (Scene.GetWorld()->LineTraceSingleByChannel(Hit, Eye, Point, ECC_Visibility,
                    FCollisionQueryParams(SCENE_QUERY_STAT(T5PapyrusSmoke), true, Pawn))
                && Hit.GetComponent() == Component)
            {
                Pawn->SetActorLocation(Eye, false, nullptr, ETeleportType::TeleportPhysics);
                Controller.SetControlRotation((Point - Eye).Rotation());
                return true;
            }
        }
    }
    Test.AddError(TEXT("The lever is missing a line trace hit."));
    return false;
}

void FT5PapyrusSmoke::PressInteract(APlayerController& Controller)
{
    Controller.InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::E, IE_Pressed, FPlatformTime::Cycles64()));
    Controller.InputKey(FInputKeyEventArgs(nullptr, INPUTDEVICEID_NONE, EKeys::E, IE_Released, FPlatformTime::Cycles64()));
}

void FT5PapyrusSmoke::CheckTarget(AT5Scene& Scene, bool bDisabled)
{
    Test.TestTrue(TEXT("Papyrus retains the target state"), Scene.Papyrus->IsDisabled(Target) == TOptional<bool>(bDisabled));
    const auto* Visuals = Scene.ReferenceVisuals.Find(Target);
    if (!Visuals || Visuals->IsEmpty())
    {
        Test.AddError(TEXT("The target mesh is missing from the loaded scene."));
        return;
    }
    bool bAuthoredCollider = false;
    for (const auto& Visual : *Visuals)
        if (const auto* Component = Visual.Component.Get())
        {
            if (!Visual.bColliderOnly)
                Test.TestEqual(TEXT("Target mesh visibility"), Component->IsVisible(), !bDisabled);
            else
                bAuthoredCollider |= Visual.Collision != ECollisionEnabled::NoCollision;
            if (const auto* Primitive = Cast<UPrimitiveComponent>(Component))
                Test.TestEqual(TEXT("Target collision"), Primitive->GetCollisionEnabled(),
                    bDisabled ? ECollisionEnabled::NoCollision : Visual.Collision);
        }
    Test.TestTrue(TEXT("Target has a separate authored collider"), bAuthoredCollider);
}

bool FT5PapyrusSmoke::Update()
{
    auto* Scene = Loader.Get();
    if (!Scene || GetCurrentRunTime() > 150)
    {
        Test.AddError(FString::Printf(TEXT("Papyrus lever test timed out in phase %d."), Phase));
        return true;
    }
    auto* Controller = Scene->GetWorld()->GetFirstPlayerController();
    if (!Controller || !IsValid(Scene->LoadedScene) || !Scene->Papyrus || Scene->bSceneLoading)
        return false;
    switch (Phase)
    {
    case 0:
        Test.TestTrue(TEXT("PEX is attached to the placed lever"), Scene->Papyrus->HasScripts(Lever));
        CheckTarget(*Scene, true);
        if (!AimAtLever(*Scene, *Controller))
            return true;
        break;
    case 1:
        PressInteract(*Controller);
        ActivationAt = GetCurrentRunTime();
        break;
    case 2:
        if (FParse::Param(FCommandLine::Get(), TEXT("T5QuestSmoke")) && GetCurrentRunTime() - ActivationAt < 2)
            return false;
        CheckTarget(*Scene, false);
        OriginalCell = Scene->LoadedScene;
        Scene->OpenT5Map(TEXT("RiverwoodSleepingGiantInn"));
        break;
    case 3:
        Test.TestFalse(TEXT("Original cell actor was destroyed"), OriginalCell.IsValid());
        Test.TestFalse(TEXT("Target has no component in the other cell"), Scene->ReferenceVisuals.Contains(Target));
        Test.TestTrue(TEXT("Unloaded target state is retained"), Scene->Papyrus->IsDisabled(Target) == TOptional<bool>(false));
        Scene->OpenT5Map(TEXT("PapyrusLeverInterior"));
        break;
    case 4:
        CheckTarget(*Scene, false);
        if (!AimAtLever(*Scene, *Controller))
            return true;
        break;
    case 5:
        PressInteract(*Controller);
        ActivationAt = GetCurrentRunTime();
        break;
    default:
        if (FParse::Param(FCommandLine::Get(), TEXT("T5QuestSmoke")) && GetCurrentRunTime() - ActivationAt < 2)
            return false;
        CheckTarget(*Scene, true);
        return true;
    }
    ++Phase;
    return false;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FT5PapyrusLeverTest, "T5Data.PapyrusLever", EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FT5PapyrusLeverTest::RunTest(const FString&)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("T5PapyrusSmoke")))
    {
        AddInfo(TEXT("Pass -T5PapyrusSmoke with Skyrim.esm and SceneLever.esp, starting at PapyrusLeverInterior."));
        return true;
    }
    for (const auto& Context : GEngine->GetWorldContexts())
        if (UWorld* World = Context.World(); World && World->IsGameWorld())
            for (TActorIterator<AT5Scene> It(World); It; ++It)
            {
                FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FT5PapyrusSmoke>(*this, *It));
                return true;
            }
    AddError(TEXT("Start a T5Data game world to run the lever test."));
    return false;
}

class FT5MQ101Smoke final : public IAutomationLatentCommand
{
public:
    FT5MQ101Smoke(FAutomationTestBase& InTest, AT5Scene& InScene) : Test(InTest), Loader(&InScene) {}

    bool Update() override
    {
        const double Now = GetCurrentRunTime();
        const bool bRaceMenu = FParse::Param(FCommandLine::Get(), TEXT("T5MQ101RaceMenuSmoke"));
        if (ScreenshotAt > 0)
            return Now >= ScreenshotAt;
        auto* Scene = Loader.Get();
        if (!Scene || Now > (bRaceMenu ? 720 : 130))
        {
            Test.AddError(bRaceMenu ? TEXT("Original MQ101 did not request ShowRaceMenu before the deadline.")
                : TEXT("Original MQ101 did not move the player's cart before the deadline."));
            return true;
        }
        if (bRaceMenu && Scene->Papyrus)
        {
            FTransform VehicleAuthored; uint32 VehicleId=0;
            Scene->Papyrus->ReferenceTransform(0x14,VehicleAuthored,VehicleId);
            if (VehicleId && Scene->Mechanics)
                if (auto Error=Scene->Mechanics->ArticulationError(VehicleId))
                {
                    bSawArticulation=true;
                    if (Scene->SimulationTime>5 && *Error>MaxJointError)
                    {
                        MaxJointError=*Error;
                        UE_LOG(LogTemp,Display,TEXT("T5_ARTICULATION_PEAK sim=%.4f ref=%08X errorCm=%.4f"),Scene->SimulationTime,VehicleId,MaxJointError);
                    }
                }
            FString CaptureRide;
            if (FParse::Value(FCommandLine::Get(),TEXT("T5RideScreenshots="),CaptureRide)
                && Scene->SimulationTime>=NextRideScreenshot)
            {
                FScreenshotRequest::RequestScreenshot(CaptureRide+FString::Printf(TEXT("-%03d.png"),int32(NextRideScreenshot)),true,false,false,FIntRect(),true);
                NextRideScreenshot+=30;
            }
            if (const auto* Animation = Scene->ReferenceAnimations.Find(0x14); Animation && (*Animation)->bMoving)
            {
                const auto& State = **Animation;
                FTransform Authored;
                uint32 Vehicle;
                if (const auto Pose = Scene->Papyrus->ReferenceTransform(0x14, Authored, Vehicle))
                {
                    bSawPlayerMotion = true;
                    PlayerMotionEnd = State.Start + State.Clock.Clip.Duration;
                    if (!State.bFinished)
                        MaxRootError = FMath::Max(MaxRootError, FVector::Dist(Pose->GetLocation(), State.Pose.GetLocation()));

                }
                FString Capture;
                if (FParse::Value(FCommandLine::Get(), TEXT("T5ExitScreenshots="), Capture))
                {
                    const float Times[] = {3, 7, 12, 15};
                    if (MotionScreenshot < 4 && Scene->SimulationTime - State.Start >= Times[MotionScreenshot])
                    {
                        if (!ExitCamera.IsValid())
                        {
                            const FVector Eye = State.Anchor.TransformPosition(FVector(-200, 800, 400));
                            const FVector Aim = State.Anchor.TransformPosition(FVector(-200, 0, 100));
                            ExitCamera = Scene->GetWorld()->SpawnActor<ACameraActor>(Eye, (Aim - Eye).Rotation());
                            Scene->GetWorld()->GetFirstPlayerController()->SetViewTarget(ExitCamera.Get());
                        }
                        FScreenshotRequest::RequestScreenshot(Capture + FString::Printf(TEXT("-%d.png"), ++MotionScreenshot),
                            true, false, false, FIntRect(), true);
                    }
                }
            }
        }
        if (bSawPlayerMotion && !bObservedDetachment && Scene->Papyrus)
        {
            FTransform Authored;
            uint32 Vehicle;
            if (Scene->Papyrus->ReferenceTransform(0x14, Authored, Vehicle) && !Vehicle)
            {
                bObservedDetachment = true;
                bDetachedAfterFullBody = Scene->SimulationTime >= PlayerMotionEnd - .05;
            }
        }
        if (bRaceMenu)
        {
            if (!Scene->Papyrus)
                return false;
            uint32 Controls, Flags, Vehicle;
            float SittingRotation;
            Scene->Papyrus->GameState(Controls, Flags, SittingRotation);
            if (!(Flags & FT5PapyrusRuntime::RaceMenuPending))
                return false;
            FTransform Authored;
            Test.TestTrue(TEXT("The player left the cart before the original ShowRaceMenu request"),
                Scene->Papyrus->ReferenceTransform(0x14, Authored, Vehicle).IsSet() && Vehicle == 0);
            Test.TestTrue(TEXT("Carriage actually uses the NIF articulation and horse hitch"),bSawArticulation);
            Test.TestTrue(FString::Printf(TEXT("NIF joint anchors stay within 5 cm after initial settling (max %.3f cm)"),MaxJointError),MaxJointError<5);
            Test.TestTrue(TEXT("Player motion is read from the original animation resources"), bSawPlayerMotion);
            Test.TestTrue(TEXT("Gameplay and visual root positions agree throughout the exit"), MaxRootError < 1.0);
            Test.TestTrue(TEXT("The player stays attached until the full-body animation ends"), bDetachedAfterFullBody);
            Test.TestTrue(TEXT("On-foot control rotation retains a level horizon"),
                FMath::Abs(Scene->GetWorld()->GetFirstPlayerController()->GetControlRotation().Roll) < .01);
            return true;
        }
        if (!Scene->Papyrus || !Scene->PlayerCamera)
            return false;
        FTransform Authored;
        uint32 Vehicle;
        const auto Player = Scene->Papyrus->ReferenceTransform(0x14, Authored, Vehicle);
        if (!Player || Vehicle != 0x000BB970)
            return false;
        if (!CartStart)
            CartStart = Player->GetLocation();
        if (FVector::DistSquared2D(*CartStart, Player->GetLocation()) < FMath::Square(600.))
            return false;
        // Skyrim.esm REFR 0004678E DATA and the first leg of the original horse PACK.
        const FVector ExpectedStart(-135517.991, 32142.575, 19865.473);
        const FVector ExpectedFacing(FMath::Cos(1.2250276804), FMath::Sin(1.2250276804), 0);
        Test.TestTrue(TEXT("Original start marker has the correct UE coordinates"), Authored.GetLocation().Equals(ExpectedStart, .5));
        Test.TestTrue(TEXT("Original start marker faces the correct side"), Authored.GetRotation().GetForwardVector().Equals(ExpectedFacing, .001));
        Test.TestTrue(TEXT("First patrol leg goes north along positive X in UE"), Player->GetLocation().X > CartStart->X + 400);
        Test.TestTrue(TEXT("All four original wheel colliders find support in Chaos"),
            Scene->Mechanics && Scene->Mechanics->WheelContacts(Vehicle) == 4);
        FTransform CartAuthored, OtherAuthored;
        uint32 Parent;
        const auto Cart = Scene->Papyrus->ReferenceTransform(Vehicle, CartAuthored, Parent);
        const auto Other = Scene->Papyrus->ReferenceTransform(0x000B9DF3, OtherAuthored, Parent);
        Test.TestTrue(TEXT("The other cart starts north and west, on the left when looking north"), Cart && Other
            && OtherAuthored.GetLocation().X > CartAuthored.GetLocation().X
            && OtherAuthored.GetLocation().Y < CartAuthored.GetLocation().Y);
        int32 PlayingActors = 0;
        int32 MountedActors = 0;
        for (const auto& [Id, Visuals] : Scene->ReferenceVisuals)
        {
            if (Visuals.ContainsByPredicate([](const auto& Visual) { return Visual.bPlayedIdle; }))
                ++PlayingActors;
            if (Visuals.ContainsByPredicate([](const auto& Visual) { return Visual.bPlayedIdle && Visual.AnimationMount; }))
            {
                ++MountedActors;
                Test.AddInfo(FString::Printf(TEXT("Mounted actor %08X plays the original rider clip"), Id));
            }
        }
        Test.TestTrue(TEXT("Both mounted escorts use saddle attachments and rider HKX"), MountedActors >= 2);
        Test.TestTrue(TEXT("Original HKX plays on the driver and multiple passengers"), PlayingActors >= 4);
        Test.TestTrue(TEXT("Missing natives are delivered with the Papyrus instruction"), Scene->Papyrus->RecentErrors().Contains(TEXT(" at instruction ")));
        auto* Children = Scene->PapyrusErrors ? Scene->PapyrusErrors->GetChildren() : nullptr;
        if (FParse::Param(FCommandLine::Get(), TEXT("T5Diagnostics")))
            Test.TestTrue(TEXT("The requested diagnostic panel is visible"), Children && Children->Num() == 1
                && Children->GetChildAt(0)->GetVisibility() == EVisibility::HitTestInvisible);
        else Test.TestFalse(TEXT("Normal play has no Papyrus debug panel"), Scene->PapyrusErrors.IsValid());
        FString Screenshot;
        if (FParse::Value(FCommandLine::Get(), TEXT("T5MQ101Screenshot="), Screenshot))
        {
            uint32 InspectRef = 0;
            if (FParse::Value(FCommandLine::Get(), TEXT("T5MQ101InspectRef="), InspectRef))
            {
                FTransform InspectAuthored;
                uint32 InspectParent;
                if (auto InspectPose = Scene->Papyrus->ReferenceTransform(InspectRef, InspectAuthored, InspectParent))
                {
                    if (Scene->Mechanics)
                        *InspectPose = Scene->Mechanics->CorrectPose(InspectRef, InspectParent, *InspectPose);
                    const FVector Eye = InspectPose->TransformPosition(FVector(100, 700, 300));
                    const FVector Aim = InspectPose->TransformPosition(FVector(100, 0, 130));
                    auto* Camera = Scene->GetWorld()->SpawnActor<ACameraActor>(Eye, (Aim - Eye).Rotation());
                    Scene->GetWorld()->GetFirstPlayerController()->SetViewTarget(Camera);
                }
            }
            FScreenshotRequest::RequestScreenshot(Screenshot, true, false, false, FIntRect(), true);
            ScreenshotAt = Now + 1.0;
            return false;
        }
        return true;
    }

private:
    FAutomationTestBase& Test;
    TWeakObjectPtr<AT5Scene> Loader;
    TOptional<FVector> CartStart;
    double ScreenshotAt = 0;
    bool bSawPlayerMotion = false, bDetachedAfterFullBody = false, bObservedDetachment = false;
    double PlayerMotionEnd = 0;
    double MaxRootError = 0;
    bool bSawArticulation=false;
    double MaxJointError=0,NextRideScreenshot=30;
    int32 MotionScreenshot = 0;
    TWeakObjectPtr<ACameraActor> ExitCamera;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FT5OriginalMQ101Test, "T5Data.MQ101",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FT5OriginalMQ101Test::RunTest(const FString&)
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("T5MQ101Smoke"))
        && !FParse::Param(FCommandLine::Get(), TEXT("T5MQ101RaceMenuSmoke")))
    {
        AddInfo(TEXT("Pass -T5MQ101Smoke with -T5NewGame -T5Quest=0003372B -T5PapyrusContinueOnMissingNative."));
        return true;
    }
    for (const auto& Context : GEngine->GetWorldContexts())
        if (UWorld* World = Context.World(); World && World->IsGameWorld())
            for (TActorIterator<AT5Scene> It(World); It; ++It)
            {
                FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FT5MQ101Smoke>(*this, **It));
                return true;
            }
    AddError(TEXT("Start a T5Data game world to run the original MQ101 test."));
    return false;
}
#endif
