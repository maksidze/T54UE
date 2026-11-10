#include "T5Hkx.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FT5HkxGraphSmoke, "T5Data.HkxGraph",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FT5HkxGraphSmoke::RunTest(const FString&)
{
    FString Directory, Error;
    if (!FParse::Value(FCommandLine::Get(), TEXT("T5HkxSmokeData="), Directory))
    { AddError(TEXT("Original HKX fixtures required")); return false; }
    auto Read = [&Directory](const FString& Path, TArray<uint8>& Bytes) {
        return FFileHelper::LoadFileToArray(Bytes, *(Directory / FPaths::GetCleanFilename(Path).ToLower()));
    };
    FT5HkxPlayback Playback;
    if (!TestTrue(TEXT("Event forwards through behavior references"), ResolveT5HkxGraph(
        TEXT("meshes/actors/character/behaviors/0_master.hkx"), TEXT("OffsetBoundStandingStart"), Read, Playback, Error)))
    { AddError(Error); return false; }
    TestTrue(TEXT("Looping original bound-standing clip"), Playback.bLoop && Playback.Path.EndsWith(TEXT("OffsetBoundStanding.hkx")));
    TArray<uint8> Rig, Bytes;
    Read(TEXT("skeleton.hkx"), Rig); Read(TEXT("offsetboundstanding.hkx"), Bytes);
    FT5HkxClip Clip;
    if (!TestTrue(TEXT("Decode original layered clip"), ReadT5HkxClip(Rig, Bytes, Clip, Error)))
    { AddError(Error); return false; }
    TestEqual(TEXT("Blender mask follows rig order"), Playback.BoneWeights.Num(), Clip.Bones.Num());
    for (const TCHAR* Name : {TEXT("NPC Root [Root]"), TEXT("NPC L Thigh [LThg]"), TEXT("NPC R Thigh [RThg]")})
    {
        int32 Index = Clip.Bones.IndexOfByPredicate([Name](const auto& Bone) { return Bone.Name == Name; });
        TestTrue(FString::Printf(TEXT("Base locomotion retained: %s"), Name),
            Playback.BoneWeights.IsValidIndex(Index) && Playback.BoneWeights[Index] == 0);
    }
    for (const TCHAR* Name : {TEXT("NPC L Hand [LHnd]"), TEXT("NPC R Hand [RHnd]")})
    {
        int32 Index = Clip.Bones.IndexOfByPredicate([Name](const auto& Bone) { return Bone.Name == Name; });
        TestTrue(FString::Printf(TEXT("Both arm state machines contribute: %s"), Name),
            Playback.BoneWeights.IsValidIndex(Index) && Playback.BoneWeights[Index] > .99f);
    }
    // Same-length replacements keep the packfile's fixups valid.
    auto Renamed = [&Read](const FString& Path, TArray<uint8>& Data) {
        if (!Read(Path, Data)) return false;
        const ANSICHAR* A = "OffsetBoundStandingStart";
        const ANSICHAR* B = "CustomBoundStandingStart";
        for (int32 I = 0; I + 24 <= Data.Num(); ++I)
            if (FMemory::Memcmp(Data.GetData() + I, A, 24) == 0) FMemory::Memcpy(Data.GetData() + I, B, 24);
        for (int32 I = 0; I + 20 <= Data.Num(); ++I)
            if (FMemory::Memcmp(Data.GetData()+I,"IdleWalkingCameraEnd",20)==0)
                FMemory::Memcpy(Data.GetData()+I,"TestWalkingCameraEnd",20);
        return true;
    };
    FT5HkxPlayback Generic;
    TestTrue(TEXT("No event-name special case"), ResolveT5HkxGraph(TEXT("meshes/actors/character/behaviors/0_master.hkx"),
        TEXT("CustomBoundStandingStart"), Renamed, Generic, Error));
    TestTrue(TEXT("Renamed event retains authored masks"), Generic.BoneWeights == Playback.BoneWeights);
    FT5HkxPlayback Reset;
    TestTrue(TEXT("Walking-camera end returns to the authored root graph"),ResolveT5HkxGraph(
        TEXT("meshes/actors/character/behaviors/0_master.hkx"),TEXT("IdleWalkingCameraEnd"),Read,Reset,Error));
    TestTrue(TEXT("Root graph reset releases the idle instead of loading an empty clip"),Reset.bReturnToLocomotion && Reset.Path.IsEmpty());
    FT5HkxPlayback RenamedReset;
    TestTrue(TEXT("Root resets are resolved from graph data, not event names"),ResolveT5HkxGraph(
        TEXT("meshes/actors/character/behaviors/0_master.hkx"),TEXT("TestWalkingCameraEnd"),Renamed,RenamedReset,Error)
        && RenamedReset.bReturnToLocomotion);
    int32 GameFiles=0;
    auto ReadGame=[&](const FString& Path,TArray<uint8>& Bytes) {
        ++GameFiles;
        if (Path.StartsWith(TEXT("@base-animation/")))
            return FFileHelper::LoadFileToArray(Bytes,*(Directory/TEXT("base-rigs")/Path.RightChop(16)));
        return FFileHelper::LoadFileToArray(Bytes,*(Directory/TEXT("game-graphs")/Path));
    };
    FT5HkxPlayback GamePlayback;
    const bool Resolved=ResolveT5HkxGraph(TEXT("meshes/actors/character/behaviors/0_master.hkx"),
        TEXT("OffsetBoundStandingStart"),ReadGame,GamePlayback,Error);
    TestTrue(TEXT("Actual game Data graph resolves the bound standing event"),Resolved);
    if (!Resolved) AddError(Error);
    TestTrue(TEXT("Regression fixture traverses more than 64 referenced files"),GameFiles>64);
    TestTrue(TEXT("Game graph retains both original arm masks"),GamePlayback.BoneWeights==Playback.BoneWeights);
    AddInfo(FString::Printf(TEXT("Actual Data: %d behavior files visited"),GameFiles));
    FT5HkxPlaybackClock Loaded;
    const bool LoadedGame=LoadT5HkxAnimation(TEXT("actors/character/behaviors/0_master.hkx"),TEXT("OffsetBoundStandingStart"),{},
        ReadGame,Error,{},1,false,false,Loaded);
    TestTrue(TEXT("Full runtime load with the actual extended loose skeleton"),LoadedGame);
    if (!LoadedGame) AddError(Error);
    else
    {
        TestEqual(TEXT("Mask is remapped onto the active skeleton"),Loaded.Playback.BoneWeights.Num(),Loaded.Clip.Bones.Num());
        TestTrue(TEXT("Regression has additional skeleton bones"),Loaded.Clip.Bones.Num()>Clip.Bones.Num());
        for (int32 I=0;I<Loaded.Clip.Bones.Num();++I)
        {
            const int32 Base=Clip.Bones.IndexOfByPredicate([&](const auto& Bone){return Bone.Name==Loaded.Clip.Bones[I].Name;});
            TestEqual(TEXT("Weights follow named bones; extension bones retain base pose"),Loaded.Playback.BoneWeights[I],
                Base==INDEX_NONE?0.f:Playback.BoneWeights[Base]);
        }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FT5HkxSkeletonSmoke, "T5Data.HkxSkeletons",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FT5HkxSkeletonSmoke::RunTest(const FString&)
{
    FString Directory, Skeletons, Error;
    if (!FParse::Value(FCommandLine::Get(), TEXT("T5HkxSkeletonData="), Skeletons))
    { AddInfo(TEXT("Supply -T5HkxSkeletonData=<stock and xp32 fixture directory>")); return true; }
    if (!FParse::Value(FCommandLine::Get(), TEXT("T5HkxSmokeData="), Directory))
    { AddError(TEXT("Original HKX fixtures required")); return false; }
    for (const TCHAR* Variant : {TEXT("stock"), TEXT("xp32")})
    for (const TCHAR* Sex : {TEXT("male"), TEXT("female")})
    {
        const FString RigName = FString(Sex) + TEXT(".hkx");
        auto Read = [&](const FString& Path, TArray<uint8>& Bytes) {
            if (Path.EndsWith(TEXT("skeleton.hkx")))
                return FFileHelper::LoadFileToArray(Bytes, *(Skeletons /
                    (Path.StartsWith(TEXT("@base-animation/")) ? TEXT("stock") : Variant) / RigName));
            return FFileHelper::LoadFileToArray(Bytes, *(Directory / FPaths::GetCleanFilename(Path).ToLower()));
        };
        FT5HkxPlaybackClock Cart, Layer;
        const FString Label = FString(Variant) + TEXT("/") + Sex;
        Error.Reset();
        if (!TestTrue(Label + TEXT(" loads the original cart animation"), LoadT5HkxAnimation(
            TEXT("actors/character/behaviors/0_master.hkx"), TEXT("IdleCartPrisonerCIdle"), {},
            Read, Error, {}, 1, false, false, Cart)))
        { AddError(Error); continue; }
        Error.Reset();
        if (!TestTrue(Label + TEXT(" loads the layered bound-standing animation"), LoadT5HkxAnimation(
            TEXT("actors/character/behaviors/0_master.hkx"), TEXT("OffsetBoundStandingStart"), {},
            Read, Error, {}, 1, false, false, Layer)))
        { AddError(Error); continue; }
        TestEqual(Label + TEXT(" has one mask weight per active bone"),
            Layer.Playback.BoneWeights.Num(), Layer.Clip.Bones.Num());
        for (const TCHAR* BoneName : {TEXT("NPC L Hand [LHnd]"), TEXT("NPC R Hand [RHnd]")})
        {
            const int32 Index = Layer.Clip.Bones.IndexOfByPredicate([&](const auto& Bone) { return Bone.Name == BoneName; });
            TestTrue(Label + TEXT(" preserves arm contribution: ") + BoneName,
                Layer.Playback.BoneWeights.IsValidIndex(Index) && Layer.Playback.BoneWeights[Index] > .99f);
        }
        TArray<uint8> BaseRig;
        Read(TEXT("@base-animation/skeleton.hkx"), BaseRig);
        FT5HkxClip Base;
        TArray<uint8> Animation;
        Read(TEXT("offsetboundstanding.hkx"), Animation);
        if (!TestTrue(Label + TEXT(" decodes the stock reference rig"), ReadT5HkxClip(BaseRig, Animation, Base, Error)))
        { AddError(Error); continue; }
        TArray<float> Weights;
        for (int32 I = 0; I < Base.Bones.Num(); ++I) Weights.Add(float(I) / Base.Bones.Num());
        auto Reordered = Base.Bones;
        Reordered.Swap(1, Reordered.Num() - 1);
        TestTrue(Label + TEXT(" remaps equal-size reordered bones"), RemapT5HkxBoneWeights(BaseRig, Reordered, Weights, Error));
        TestEqual(Label + TEXT(" weight follows the last bone to index one"), Weights[1], float(Base.Bones.Num() - 1) / Base.Bones.Num());
        TestEqual(Label + TEXT(" weight follows the moved first child"), Weights.Last(), 1.f / Base.Bones.Num());
        AddInfo(FString::Printf(TEXT("%s: %d bones, %d animation tracks"), *Label, Cart.Clip.Bones.Num(), Cart.Clip.Tracks));
    }
    return true;
}
#endif
