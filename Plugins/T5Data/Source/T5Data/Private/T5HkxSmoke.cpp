#include "T5Hkx.h"
#include "T5MotionData.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "T5HkxCamera.h"
#include "T5SkeletalMesh.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

namespace
{
    USkeletalMeshComponent* MakePoseProbe(AActor& Actor, FName Bone)
    {
        auto* Root = NewObject<USceneComponent>(&Actor);
        Actor.SetRootComponent(Root);
        Root->RegisterComponent();
        TArray<TSharedPtr<FJsonValue>> Matrix;
        for (int32 I = 0; I < 16; ++I)
            Matrix.Add(MakeShared<FJsonValueNumber>(I % 5 == 0 ? 1 : 0));
        const TArray<TSharedPtr<FJsonValue>> BoneRow{MakeShared<FJsonValueString>(Bone.ToString()),
            MakeShared<FJsonValueNumber>(INDEX_NONE), MakeShared<FJsonValueArray>(Matrix)};
        const TArray<TSharedPtr<FJsonValue>> Bones{MakeShared<FJsonValueArray>(BoneRow)};
        FT5SkinSection Section{MakeShared<FJsonObject>(), UMaterial::GetDefaultMaterial(MD_Surface)};
        Section.Data->SetArrayField(TEXT("placement"), Matrix);
        Section.Data->SetNumberField(TEXT("vertices"), 3);
        Section.Data->SetNumberField(TEXT("indices"), 3);
        Section.Data->SetNumberField(TEXT("offset"), 0);
        Section.Data->SetNumberField(TEXT("skinOffset"), 0);

        struct
        {
            FT5SourceVertex Vertices[3];
            uint32 Indices[3] = {0, 1, 2};
        } Geometry;
        struct FProbeWeights
        {
            uint16 Bones[8] = {};
            float Weights[8] = {1};
        } Skin[3];
        for (int32 I = 0; I < 3; ++I)
            Geometry.Vertices[I] = {FVector3f(I == 1 ? 1 : 0, I == 2 ? 1 : 0, 0), FVector3f::UpVector,
                FVector2f(I == 1 ? 1 : 0, I == 2 ? 1 : 0), FVector4f(1, 1, 1, 1)};
        return BuildT5SkeletalMesh(Actor, *Root, Bones, MakeArrayView(&Section, 1),
            MakeArrayView64(reinterpret_cast<const uint8*>(&Geometry), sizeof(Geometry)),
            MakeArrayView64(reinterpret_cast<const uint8*>(Skin), sizeof(Skin)));
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FT5HkxSmoke, "T5Data.HkxCart",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FT5HkxSmoke::RunTest(const FString&)
{
    FString Directory;
    if (!FParse::Value(FCommandLine::Get(), TEXT("T5HkxSmokeData="), Directory))
    {
        AddInfo(TEXT("Supply -T5HkxSmokeData=<directory containing the original cart HKX files and their first-person variants>"));
        return true;
    }
    TArray<uint8> Graph, Rig, Animation;
    if (!TestTrue(TEXT("Load original MQ101 behavior"), FFileHelper::LoadFileToArray(Graph, *(Directory / TEXT("0_master.hkx"))))
        || !TestTrue(TEXT("Load original character skeleton"), FFileHelper::LoadFileToArray(Rig, *(Directory / TEXT("skeleton.hkx"))))
        || !TestTrue(TEXT("Load original player cart clip"), FFileHelper::LoadFileToArray(Animation, *(Directory / TEXT("cartprisonercidle.hkx")))))
        return false;
    FString Error;
    FT5HkxPlayback Play;
    if (!ResolveT5HkxEvent(Graph, TEXT("IdleCartDriverSway"), Play, Error))
    {
        AddError(Error);
        return false;
    }
    TestTrue(TEXT("Driver sway uses its actual generator, whose clip has a different name"),
        Play.Path.EndsWith(TEXT("CartDriverIdleSway.hkx")));
    TestTrue(TEXT("Driver sway loops"), Play.bLoop);
    if (!ResolveT5HkxEvent(Graph, TEXT("IdleCartDriverExit"), Play, Error))
    {
        AddError(Error);
        return false;
    }
    TestTrue(TEXT("The driver exit generator has its own graph trigger"), Play.Triggers.ContainsByPredicate([](const auto& Trigger) {
        return Trigger.Event == TEXT("CartExit") && Trigger.bRelativeToEnd && FMath::IsNearlyEqual(Trigger.Time, -0.2f);
    }));
    TestTrue(TEXT("The driver exit state notifies the Papyrus listener"), Play.ExitEvents.Contains(TEXT("ExitCartEnd")));
    if (!ResolveT5HkxEvent(Graph, TEXT("IdleCartPrisonerCIdle"), Play, Error))
    {
        AddError(Error);
        return false;
    }
    TestTrue(TEXT("Player event resolves from its graph transition"), Play.Path.EndsWith(TEXT("CartPrisonerCIdle.hkx")));
    FT5HkxClip Clip;
    if (!ReadT5HkxClip(Rig, Animation, Clip, Error))
    {
        AddError(Error);
        return false;
    }
    TestEqual(TEXT("Player clip includes both spline blocks"), Clip.Frames, 275);
    TestEqual(TEXT("Player clip bone tracks"), Clip.Tracks, 99);
    TestTrue(TEXT("Player clip duration"), FMath::IsNearlyEqual(Clip.Duration, 9.133333f, 0.001f));
    TestEqual(TEXT("Cart seat placement comes from the animated COM bone"), Clip.Bones[4].Name, FName(TEXT("NPC COM [COM ]")));
    TestTrue(TEXT("Havok seat uses the same north and east axes as the world"),
        Clip.Samples[4].GetTranslation().Equals(FVector(-149.61559, -60.00975, 198.28428), 0.01));
    TestTrue(TEXT("40-bit quaternion curve varies over the clip"), Clip.Samples[4].GetRotation().AngularDistance(
        Clip.Samples[137 * Clip.Tracks + 4].GetRotation()) > 0.001);
    TestTrue(TEXT("Pose is decoded at the last sample after the overlap"),
        Clip.Samples.Last().IsValid() && Clip.Samples[(Clip.Frames - 1) * Clip.Tracks + 4].GetTranslation().Equals(
            Clip.Samples[4].GetTranslation(), 0.1));
    TestFalse(TEXT("Reject truncated packfile"), ReadT5HkxClip(Rig, TConstArrayView<uint8>(Animation).Left(220), Clip, Error));
    FT5HkxCameraClip Camera;
    if (TestTrue(TEXT("Resolve the original first-person graph, rig and camera track"),
        LoadT5HkxPlayerCamera(TEXT("actors/character/behaviors/0_master.hkx"), TEXT("IdleCartPrisonerCIdle"),
            [&Directory](const FString& Path, TArray<uint8>& Data) {
                FString Name = FPaths::GetCleanFilename(Path);
                Name.ToLowerInline();
                return Path.Contains(TEXT("/_1stperson/"))
                    && FFileHelper::LoadFileToArray(Data, *(Directory / TEXT("first-person") / Name));
            }, Camera, Error)))
    {
        const auto First = SampleT5HkxPlayerCamera(Camera, 0);
        const auto Later = SampleT5HkxPlayerCamera(Camera, 137.0f / 30);
        // Camera1st is a child of NPC Root, whose first-frame rotation also rotates the offset.
        TestEqual(TEXT("Camera keeps the authored cart-relative position after composing NPC Root"),
            First.GetLocation(), FVector(-153.1833, -54.6365, 267.5387), 0.05f);
        TestTrue(TEXT("The original camera track sways"), FVector::Dist(First.GetLocation(), Later.GetLocation()) > 0.5
            && First.GetRotation().AngularDistance(Later.GetRotation()) > 0.01);
    }
    else
        AddError(Error);
    for (const auto& Context : GEngine->GetWorldContexts())
    {
        if (Context.WorldType != EWorldType::Game || !Context.World())
            continue;
        FActorSpawnParameters Spawn;
        Spawn.ObjectFlags = RF_Transient;
        auto* Actor = Context.World()->SpawnActor<AActor>(Spawn);
        if (!TestNotNull(TEXT("Spawn a transient probe actor"), Actor))
            return false;
        auto* Mesh = MakePoseProbe(*Actor, Clip.Bones[4].Name);
        if (TestNotNull(TEXT("Build a runtime skeletal component"), Mesh)
            && TestTrue(TEXT("Play the original cart clip on that component"),
                PlayT5Hkx(*Mesh, TEXT("actors/character/behaviors/0_master.hkx"), TEXT("IdleCartPrisonerCIdle"), {},
                    [&Directory](const FString& Path, TArray<uint8>& Data) {
                        FString Name = FPaths::GetCleanFilename(Path);
                        Name.ToLowerInline();
                        return FFileHelper::LoadFileToArray(Data, *(Directory / Name));
                    }, Error)))
        {
            Mesh->TickAnimation(0, false);
            Mesh->RefreshBoneTransforms();
            const auto FirstPose = Mesh->GetBoneTransform(0, FTransform::Identity);
            TestTrue(TEXT("Runtime visibility bounds contain the animated seat, outside the bind-pose bounds"),
                Mesh->CalcBounds(FTransform::Identity).GetBox().ExpandBy(.01).IsInside(FirstPose.GetLocation()));
            Mesh->TickAnimation(137.0f / 30, false);
            Mesh->RefreshBoneTransforms();
            const auto LaterPose = Mesh->GetBoneTransform(0, FTransform::Identity);
            TestTrue(TEXT("Runtime visibility bounds follow the sampled sway pose"),
                Mesh->CalcBounds(FTransform::Identity).GetBox().ExpandBy(.01).IsInside(LaterPose.GetLocation()));
            TestTrue(TEXT("Engine pose follows the seat position"), FirstPose.GetLocation().Size() > 100);
            TestTrue(TEXT("Engine pose advances with the HKX clip"),
                FirstPose.GetRotation().AngularDistance(LaterPose.GetRotation()) > 0.001);
        }
        else if (!Error.IsEmpty())
            AddError(Error);
        Actor->Destroy();
        break;
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FT5MotionSmoke, "T5Data.MotionData",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FT5MotionSmoke::RunTest(const FString&)
{
    FString Directory;
    if (!FParse::Value(FCommandLine::Get(), TEXT("T5HkxSmokeData="), Directory))
    {
        AddError(TEXT("MotionData requires -T5HkxSmokeData with the original animation data."));
        return false;
    }
    TArray<uint8> Bytes;
    FString Error;
    FT5MotionDatabase Database;
    if (!TestTrue(TEXT("Load original movement cache"), FFileHelper::LoadFileToArray(Bytes, *(Directory / TEXT("animationdatasinglefile.txt"))))
        || !TestTrue(TEXT("Parse all original character and object projects"), Database.Read(Bytes, Error)))
    { AddError(Error); return false; }
    TestTrue(TEXT("The database includes creatures and animated objects, not only humans"), Database.ProjectCount() > 100);
    const auto Upper = Database.Find(TEXT("DefaultFemale.hkx"), {}, TEXT("CrossBow_IdleHeld"), Error);
    const auto Lower = Database.Find(TEXT("DefaultFemale.hkx"), {}, TEXT("Crossbow_IdleHeld"), Error);
    TestTrue(TEXT("Case-distinct generator names retain separate motion bindings"), Upper && Lower && Upper != Lower);
    const auto Motion = Database.Find(TEXT("DefaultMale.hkx"), TEXT("skeleton.hkx"), TEXT("CartPrisonerCExit.hkx"), Error);
    if (!TestTrue(TEXT("Resolve generator through the project's motion index"), Motion.IsValid())) return false;
    TestTrue(TEXT("Implicit zero-time key has identity motion"), Motion->Sample(0).Equals(FTransform::Identity));
    const auto End = Motion->Sample(Motion->Duration);
    TestTrue(TEXT("Authored displacement, with the world axis and unit conversion"),
        End.GetLocation().Equals(FVector(-330.398, -7.50556, -2.19986) * (100.0 / 70.0), .001));
    TestTrue(TEXT("Rotation is read independently of bone pose"), End.GetRotation().AngularDistance(FQuat::Identity) > 2.8);
    TestTrue(TEXT("Motion progresses at its authored timestamps"),
        Motion->Sample(5.2f).GetLocation().Equals(FVector(-99.5646, 1.37165, .104069) * (100.0 / 70.0), .001));
    const auto FirstPerson = Database.Find(TEXT("FirstPerson.hkx"), {}, TEXT("CartPrisonerCExit.hkx"), Error);
    TestTrue(TEXT("First-person data is a different project and duration"), FirstPerson && FirstPerson->Duration < Motion->Duration - 4);
    const auto Driver = Database.Find(TEXT("DefaultMale.hkx"), {}, TEXT("CartDriverExit.hkx"), Error);
    TestTrue(TEXT("The same parser reads lateral driver motion"), Driver && Driver->Sample(Driver->Duration).GetLocation().Y > 150);
    FT5HkxPlaybackClock Clock;
    if (!TestTrue(TEXT("Load the original full-body generator and bind its motion"), LoadT5HkxAnimation(
        TEXT("actors/character/behaviors/0_master.hkx"), TEXT("IdleCartPrisonerCExit"), {},
        [&Directory](const FString& Path, TArray<uint8>& Data) {
            return FFileHelper::LoadFileToArray(Data, *(Directory / FPaths::GetCleanFilename(Path).ToLower()));
        }, Error, {}, 1, false, false, Clock, &Database, TEXT("DefaultMale.hkx"))))
    { AddError(Error); return false; }
    int32 Exits = 0;
    DispatchT5HkxEvents(Clock, 11.5f, [&](const FString& Event) { Exits += Event == TEXT("ExitCartEnd"); });
    TestEqual(TEXT("Camera completion does not complete the body animation"), Exits, 0);
    DispatchT5HkxEvents(Clock, 16.f, [&](const FString& Event) { Exits += Event == TEXT("ExitCartEnd"); });
    DispatchT5HkxEvents(Clock, 17.f, [&](const FString& Event) { Exits += Event == TEXT("ExitCartEnd"); });
    TestEqual(TEXT("The full-body clock emits one exit"), Exits, 1);
    TestTrue(TEXT("The terminal pose retains movement after completion"), T5HkxMotion(Clock, 17).GetLocation().Equals(End.GetLocation(), .001));
    const FUTF8ToTCHAR SourceText(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
    FString Renamed(SourceText.Length(), SourceText.Get());
    Renamed.ReplaceInline(TEXT("DefaultMale.txt"), TEXT("UnrelatedCreature.txt"), ESearchCase::CaseSensitive);
    Renamed.ReplaceInline(TEXT("CartPrisonerCExit.hkx"), TEXT("ArbitraryGenerator"), ESearchCase::CaseSensitive);
    const FTCHARToUTF8 RenamedBytes(*Renamed);
    FT5MotionDatabase Generic;
    TestTrue(TEXT("No project or animation names are built into the parser"), Generic.Read(
        MakeArrayView(reinterpret_cast<const uint8*>(RenamedBytes.Get()), RenamedBytes.Length()), Error));
    const auto Arbitrary = Generic.Find(TEXT("UnrelatedCreature.hkx"), {}, TEXT("ArbitraryGenerator"), Error);
    TestTrue(TEXT("Renaming the source resources preserves their trajectory"), Arbitrary && Arbitrary->Equals(*Motion));
    Clock.Playback.Speed = 2;
    TestTrue(TEXT("Playback speed changes time, not distance"), T5HkxMotion(Clock, 2.6f).Equals(Motion->Sample(5.2f), .001));
    Clock.Playback.CropStart = 5.2f;
    TestTrue(TEXT("Cropped playback starts without teleporting by the omitted displacement"), T5HkxMotion(Clock, 0).Equals(FTransform::Identity, .001));
    TestFalse(TEXT("Truncated caches are rejected transactionally"), Database.Read(MakeArrayView(Bytes).Left(Bytes.Num() / 2), Error));
    TestTrue(TEXT("A failed read does not replace the working database"), Database.Find(TEXT("DefaultMale.hkx"), {}, TEXT("CartDriverExit.hkx"), Error).IsValid());
    return true;
}
#endif
