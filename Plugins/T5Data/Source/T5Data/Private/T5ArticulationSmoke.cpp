#include "T5Articulation.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "T5Mechanics.h"
#include "Components/StaticMeshComponent.h"
#include "Components/BoxComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Serialization/JsonSerializer.h"

class FT5AssemblyDynamics final : public IAutomationLatentCommand
{
public:
    FT5AssemblyDynamics(FAutomationTestBase& T,TSharedPtr<FT5Mechanics> M,TSharedPtr<FT5Articulation> A,AActor* O)
        : Test(T),Mechanics(M),Assembly(A),Owner(O),Start(O->GetWorld()->GetTimeSeconds()), Initial(A->Pose()), Anchor(A->Anchor->GetComponentTransform()) {}
    bool Update() override
    {
        const double Elapsed=Owner->GetWorld()->GetTimeSeconds()-Start;
        auto Target=Anchor; Target.AddToTranslation(Initial.GetRotation().RotateVector(FVector(FMath::Max(0.,Elapsed-1.)*200.,0,0)));
        Assembly->Drive(Target); Assembly->Tick();
        const double Error=Assembly->JointError(); Max=FMath::Max(Max,Error);
        if (Elapsed<8) return false;
        Test.AddInfo(FString::Printf(TEXT("Dynamics: error %.3f cm, root travel %.3f cm, roll %.3f"),Max,FVector::Distance(Initial.GetLocation(),Assembly->Pose().GetLocation()),Assembly->Pose().Rotator().Roll));
        Test.TestTrue(TEXT("Assembly moves under hitch traction"),FVector::Distance(Initial.GetLocation(),Assembly->Pose().GetLocation())>500);
        Test.TestTrue(FString::Printf(TEXT("Dynamic NIF assembly retains anchors, maximum %.3f cm"),Max),Max<5);
        Assembly->SetMotionType(4); Assembly->SetPaused(false);
        for (const auto& [Name,B]:Assembly->Bodies) Test.TestFalse(TEXT("Keyframed mode stops dynamic simulation"),B.Component->IsSimulatingPhysics());
        Assembly->SetMotionType(1); Assembly->SetPaused(false);
        for (const auto& [Name,B]:Assembly->Bodies) Test.TestTrue(TEXT("Dynamic mode restores simulation"),B.Component->IsSimulatingPhysics());
        Assembly->Detach(); Test.TestFalse(TEXT("Untether destroys the external constraint"),Assembly->bAttached);
        Owner->Destroy(); Mechanics.Reset(); Assembly.Reset(); return true;
    }
private:
    FAutomationTestBase& Test; TSharedPtr<FT5Mechanics> Mechanics; TSharedPtr<FT5Articulation> Assembly;
    TWeakObjectPtr<AActor> Owner; double Start,Max=0; FTransform Initial,Anchor;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FT5ArticulationSmoke,"T5Data.NifArticulation",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FT5ArticulationSmoke::RunTest(const FString&)
{
    FString Directory;
    if (!FParse::Value(FCommandLine::Get(),TEXT("T5HkxSmokeData="),Directory))
    { AddError(TEXT("Original NIF mechanics fixtures required")); return false; }
    UWorld* World=nullptr;
    for (const auto& Context:GEngine->GetWorldContexts()) if (Context.World() && Context.World()->IsGameWorld()) { World=Context.World(); break; }
    if (!TestNotNull(TEXT("Game world"),World)) return false;
    AActor* RoadActor=nullptr;
    // Original NIF road: reflected render winding must not become underside-only Chaos collision.
    {
        FString Text; TArray<TSharedPtr<FJsonValue>> Parts;
        if (!FFileHelper::LoadFileToString(Text,*(Directory/TEXT("road.json.collision.json")))
            || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Parts))
        { AddError(TEXT("Original road collision fixture required")); return false; }
        auto* Actor=World->SpawnActor<AActor>(); auto* Root=NewObject<USceneComponent>(Actor);
        Actor->SetRootComponent(Root); Root->RegisterComponent();
        auto Models=MakeShared<FJsonObject>(); Models->SetArrayField(TEXT("road"),Parts);
        auto Instance=MakeShared<FJsonObject>(); Instance->SetStringField(TEXT("model"),TEXT("road"));
        Instance->SetNumberField(TEXT("ref"),0); Instance->SetBoolField(TEXT("skeleton"),false);
        TArray<TSharedPtr<FJsonValue>> Pose;
        for (double V:{14000.,14000.,70000.,0.,0.,0.,1.,1.}) Pose.Add(MakeShared<FJsonValueNumber>(V));
        Instance->SetArrayField(TEXT("pose"),Pose);
        FJsonObject Manifest; Manifest.SetObjectField(TEXT("models"),Models);
        Manifest.SetArrayField(TEXT("instances"),{MakeShared<FJsonValueObject>(Instance)});
        FT5Mechanics Mechanics; Mechanics.LoadCollisions(*Actor,Manifest,{},[](uint32,UStaticMeshComponent*,const FString&){});
        FHitResult Hit; FCollisionQueryParams Q; Q.bTraceComplex=true;
        const bool Found=World->SweepSingleByChannel(Hit,FVector(20000,20000,100500),FVector(20000,20000,99500),
            FQuat::Identity,ECC_Visibility,FCollisionShape::MakeSphere(10),Q);
        TestTrue(TEXT("Original NIF road supports a shape swept down from above"),Found && Hit.ImpactNormal.Z>.9);
        RoadActor=Actor;
    }
    for (const TCHAR* File:{TEXT("articulation.json"),TEXT("articulation-renamed.json")})
    {
        FString Text; TSharedPtr<FJsonObject> Model;
        if (!FFileHelper::LoadFileToString(Text,*(Directory / File))
            || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Model))
        { AddError(FString(TEXT("Cannot read "))+File); return false; }
        auto* Actor=World->SpawnActor<AActor>();
        auto* Root=NewObject<USceneComponent>(Actor); Actor->SetRootComponent(Root); Root->RegisterComponent();
        auto Record=MakeShared<FJsonObject>(); Record->SetObjectField(TEXT("model"),Model); Record->SetNumberField(TEXT("scale"),1);
        FJsonObject Manifest; Manifest.SetObjectField(TEXT("123"),Record);
        {
            auto Mechanics=MakeShared<FT5Mechanics>(); Mechanics->Load(*Actor,Manifest);
            const auto* Cart=Mechanics->Carts.Find(123);
            if (!TestTrue(TEXT("Construct assembly from NIF graph"),Cart && Cart->Physics)) { Actor->Destroy(); return false; }
            TArray<FHitResult> WheelHits; FComponentQueryParams WheelQuery; WheelQuery.bTraceComplex=true;
            World->ComponentSweepMultiByChannel(WheelHits,Cart->Wheels[0].Collision.Get(),FVector(20000,20000,100500),
                FVector(20000,20000,99500),FQuat::Identity,ECC_Visibility,WheelQuery);
            TestTrue(TEXT("An unstarted authored wheel can find the original road for placement"),WheelHits.ContainsByPredicate([](const auto& H){return H.bBlockingHit && H.ImpactNormal.Z>.9;}));
            auto& Assembly=*Cart->Physics;
            TestEqual(TEXT("All original rigid bodies, including front axle"),Assembly.Bodies.Num(),6);
            TestEqual(TEXT("All original joints"),Assembly.Joints.Num(),5);
            Assembly.Start(FTransform(FQuat(FVector::UpVector,.7),FVector(100,200,100000)));
            TestTrue(TEXT("Joint frames agree after world basis conversion"),Assembly.JointError()<.02);
            for (const auto& [Name,Body]:Assembly.Bodies)
            {
                TestEqual(TEXT("Position iterations are actually enabled, not ignored by Chaos"),Body.Component->BodyInstance.GetPositionSolverIterationCount(),16);
                TestEqual(TEXT("Velocity iterations are actually enabled"),Body.Component->BodyInstance.GetVelocitySolverIterationCount(),8);
                TestTrue(TEXT("Chaos uses authored mass"),FMath::IsNearlyEqual(double(Body.Component->GetMass()),Body.Mass,.01));
                const FVector Actual=Body.Component->BodyInstance.GetBodyInertiaTensor();
                TestTrue(FString::Printf(TEXT("Authored inertia %s: actual %s expected %s"),*Name,*Actual.ToString(),*Body.Inertia.ToString()),
                    ((Actual-Body.Inertia)/Body.Inertia).GetAbsMax()<1.e-6);
                TestTrue(TEXT("Physical body initially follows its own NIF transform"),
                    Assembly.NodePose(Name).GetValue().Equals(Body.Rest*Assembly.Pose(),.01));
            }
            TestTrue(TEXT("Hitch axle is inferred from topology, including renamed nodes"),Assembly.Bodies.Contains(Assembly.HitchNode));
            TestTrue(TEXT("Prismatic hitch attaches to an authored body"),Assembly.Attach(FTransform::Identity));
            TestTrue(TEXT("Preserved source slide limits"),Assembly.LimitMin==-.75 && Assembly.LimitMax==.75);
            Assembly.Drive(Assembly.HitchB.Inverse()*(Assembly.HitchA*Assembly.Bodies.FindChecked(Assembly.HitchNode).Component->GetComponentTransform()));
            if (FString(File)==TEXT("articulation.json"))
            {
                auto* Floor=NewObject<UBoxComponent>(Actor); Floor->SetBoxExtent(FVector(5000,5000,50));
                Floor->SetCollisionProfileName(TEXT("BlockAll")); Floor->RegisterComponent();
                double Bottom=1.e10;
                for (const auto& [Name,B]:Assembly.Bodies) Bottom=FMath::Min(Bottom,B.Component->Bounds.Origin.Z-B.Component->Bounds.BoxExtent.Z);
                Floor->SetWorldLocation(FVector(100,200,Bottom-52));
                FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FT5AssemblyDynamics>(*this,Mechanics,Cart->Physics,Actor));
                continue;
            }
        }
        Actor->Destroy();
    }
    if (RoadActor) RoadActor->Destroy();
    return true;
}
#endif
