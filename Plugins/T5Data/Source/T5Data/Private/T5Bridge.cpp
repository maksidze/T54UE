#include "T5Bridge.h"
#include "T5MotionData.h"
#include "T5SkeletalMesh.h"
#include "T5CharacterCreator.h"

#include "Dom/JsonValue.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
#include "Interfaces/IPluginManager.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, T5Data)

DEFINE_LOG_CATEGORY_STATIC(LogT5Papyrus, Log, All);
DEFINE_LOG_CATEGORY_STATIC(LogT5Resources, Log, All);

struct FT5PapyrusApi
{
    void* (*Create)(const char*);
    void (*Destroy)(void*);
    void (*Configure)(void*, int, uint32, int);
    int (*Attach)(void*, const void*);
    uint64 (*Revision)(const void*);
    uint32 (*Tracked)(const void*, uint32*, uint32);
    int (*IsDisabled)(const void*, uint32);
    int (*HasScripts)(const void*, uint32);
    int (*Activate)(void*, uint32, uint32);
    void (*Tick)(void*, double);
    const char* (*Messages)(const void*);
    const char* (*Errors)(const void*);
    const char* (*Presentation)(const void*);
    void (*ArrowImpact)(void*, uint32, uint32);
    const char* (*Animations)(const void*);
    const char* (*Audio)(const void*);
    void (*AudioFinished)(void*, int32, int);
    void (*AnimationPose)(void*, uint32, const float*, int);
    void (*AnimationEvent)(void*, uint32, const char*);
    void* (*ReadResource)(const void*, const char*);
    uint32 (*Spawned)(const void*, FT5SpawnedReference*, uint32);
    int (*Transform)(const void*, uint32, float*, uint32*);
    int (*MotionType)(const void*, uint32);
    int (*DoorState)(const void*, uint32, double*);
    uint32 (*Weather)(const void*, float*);
    void (*GameState)(const void*, uint32*, float*);
    void* (*DecodeAudio)(const void*, uint64);
    const char* (*GetError)(const void*);
    const void* (*GetBuffer)(const void*, const char*, uint64*);
    void (*Release)(void*);
    void* (*SaveCheckpoint)(const void*);
    int (*RestoreCheckpoint)(void*, const void*, const void*, uint64);
    void* (*CharacterCatalog)(const void*);
    void* (*CharacterPreview)(const void*, const void*, uint32, const char*, const char*, const char*);
    int (*CommitCharacter)(void*, const void*, uint32, const char*, const char*);
    float (*PlayerEyeHeight)(const void*);
    void (*SetGamepad)(void*, int);
    float (*VisualScale)(const void*, uint32);
    uint32 (*Environment)(const void*, float*, uint32);
    void (*BeginEnvironment)(void*, uint32, uint32);
    void (*FallResult)(void*, uint32, const float*, const float*, int);
    void (*FiringPosition)(void*, uint32, uint32, const float*);
    void (*SetLineOfSight)(void*, int (*)(void*, uint32, uint32), void*);
    const char* (*Inventory)(void*, uint32);
    int (*InventoryAction)(void*, uint32, uint32, int32);
    int (*PlayerWeapon)(void*, int, uint32, float*);
    int (*PlayerBow)(void*, const float*);
    int (*WasKilled)(const void*, uint32);
    uint64 (*ViewKey)(const void*, int*);
    void* (*PlayerView)(const void*);
    void* (*PlayerBody)(const void*);
    const char* (*SeveredHead)(const void*, uint32);
    void* (*AnimationObject)(const void*, const char*);
    const char* (*Dialogue)(void*, int, uint32);
    void* (*DestructionModel)(const void*, uint32);
    const char* (*InventoryPresentation)(void*, uint32);
    void* (*InventoryModel)(const void*, uint32);
    int (*ResourcePools)(const void*, float*);
};

void FT5PapyrusRuntime::SetLineOfSight(int (*Callback)(void*, uint32, uint32), void* Context)
{
    if (Handle && Api->SetLineOfSight) Api->SetLineOfSight(Handle, Callback, Context);
}

FT5PapyrusRuntime::~FT5PapyrusRuntime()
{
    if (Handle)
        Api->Destroy(Handle);
}

bool FT5PapyrusRuntime::Attach(const FT5LoadResult& Location, const TArray<uint8>* Checkpoint)
{
    if (!Location.Papyrus)
        return false;
    if (!Handle)
    {
        Api = Location.Papyrus;
        FString Path = FPaths::Combine(FPaths::ProjectLogDir(), TEXT("T5Papyrus.log"));
        FParse::Value(FCommandLine::Get(), TEXT("T5PapyrusLog="), Path);
        Path = FPaths::ConvertRelativePathToFull(Path);
        IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
        Handle = Api->Create(TCHAR_TO_UTF8(*Path));
        if (!Handle)
        {
            UE_LOG(LogT5Papyrus, Error, TEXT("Could not open Papyrus log %s"), *Path);
            return false;
        }
        FString Quest;
        FParse::Value(FCommandLine::Get(), TEXT("T5Quest="), Quest);
        Api->Configure(Handle, FParse::Param(FCommandLine::Get(), TEXT("T5NewGame")),
            static_cast<uint32>(FCString::Strtoui64(*Quest, nullptr, 16)),
            FParse::Param(FCommandLine::Get(), TEXT("T5PapyrusContinueOnMissingNative")));
        UE_LOG(LogT5Papyrus, Display, TEXT("T5 Papyrus errors are logged to %s"), *Path);
    }
    if (Checkpoint)
    {
        if (!Api->RestoreCheckpoint || !Api->RestoreCheckpoint(Handle, Location.Handle, Checkpoint->GetData(), Checkpoint->Num()))
        { LogEvents(); return false; }
        LogEvents();
        return true;
    }
    const uint64 PreviousRevision = Revision();
    const bool bAttached = Api->Attach(Handle, Location.Handle) != 0;
    if (PreviousRevision != Revision())
        AudioRequests.Reset();
        Presentation.Reset();
    LogEvents();
    return bAttached;
}

void FT5PapyrusRuntime::Capture(FT5LoadRequest& Request) const
{
    if (!Handle)
        return;
    Request.ScriptRevision = Api->Revision(Handle);
    Request.TrackedReferences.SetNumUninitialized(Api->Tracked(Handle, nullptr, 0));
    Api->Tracked(Handle, Request.TrackedReferences.GetData(), Request.TrackedReferences.Num());
    Request.SpawnedReferences.SetNumUninitialized(SpawnedCount());
    Api->Spawned(Handle, Request.SpawnedReferences.GetData(), Request.SpawnedReferences.Num());
}

bool FT5PapyrusRuntime::HasScripts(uint32 Id) const
{
    return Handle && Api->HasScripts(Handle, Id);
}

TOptional<bool> FT5PapyrusRuntime::IsDisabled(uint32 Id) const
{
    const int State = Handle ? Api->IsDisabled(Handle, Id) : -1;
    return State < 0 ? TOptional<bool>() : TOptional<bool>(State != 0);
}

bool FT5PapyrusRuntime::WasKilled(uint32 Id) const
{
    return Handle && Api->WasKilled && Api->WasKilled(Handle, Id);
}

FName FT5PapyrusRuntime::SeveredHead(uint32 Id) const
{
    const auto* Name = Handle && Api->SeveredHead ? Api->SeveredHead(Handle, Id) : nullptr;
    return Name ? FName(UTF8_TO_TCHAR(Name)) : NAME_None;
}

TUniquePtr<FT5LoadResult> FT5PapyrusRuntime::AnimationObject(const FString& EditorId) const
{
    if (!Handle || !Api->AnimationObject) return {};
    auto Result = MakeUnique<FT5LoadResult>();
    Result->GetBuffer = Api->GetBuffer;
    Result->Release = Api->Release;
    Result->Handle = Api->AnimationObject(Handle, TCHAR_TO_UTF8(*EditorId));
    Result->Error = UTF8_TO_TCHAR(Api->GetError(Result->Handle));
    return Result;
}

TUniquePtr<FT5LoadResult> FT5PapyrusRuntime::DestructionModel(uint32 Id) const
{
    if (!Handle || !Api->DestructionModel) return {};
    auto Result = MakeUnique<FT5LoadResult>();
    Result->GetBuffer = Api->GetBuffer;
    Result->Release = Api->Release;
    Result->Handle = Api->DestructionModel(Handle, Id);
    Result->Error = UTF8_TO_TCHAR(Api->GetError(Result->Handle));
    return Result;
}

bool FT5PapyrusRuntime::Activate(uint32 Id)
{
    if (!Handle)
        return false;
    const bool bActivated = Api->Activate(Handle, Id, 0x14) != 0;
    LogEvents();
    return bActivated;
}

FString FT5PapyrusRuntime::DialogueState() const
{
    const auto* State = Handle && Api->Dialogue ? Api->Dialogue(Handle, int32(ET5DialogueAction::Read), 0) : nullptr;
    return State ? FString(UTF8_TO_TCHAR(State)) : FString();
}

void FT5PapyrusRuntime::DialogueAction(ET5DialogueAction Action, uint32 Choice)
{
    if (!Handle || !Api->Dialogue) return;
    Api->Dialogue(Handle, int32(Action), Choice);
    LogEvents();
}

bool FT5PapyrusRuntime::Inventory(uint32 Owner, TArray<TSharedPtr<FJsonValue>>& Entries) const
{
    if (!Handle || !Api->Inventory)
        return false;
    const auto* Text = Api->InventoryPresentation ? Api->InventoryPresentation(Handle, Owner) : Api->Inventory(Handle, Owner);
    return Text && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(UTF8_TO_TCHAR(Text))), Entries)
        && !Entries.IsEmpty();
}

TUniquePtr<FT5LoadResult> FT5PapyrusRuntime::InventoryModel(uint32 Item) const
{
    if (!Handle || !Api->InventoryModel)
        return {};
    auto Result = MakeUnique<FT5LoadResult>();
    Result->GetBuffer = Api->GetBuffer;
    Result->Release = Api->Release;
    Result->Handle = Api->InventoryModel(Handle, Item);
    if (!Result->Handle)
        return {};
    Result->Error = UTF8_TO_TCHAR(Api->GetError(Result->Handle));
    return Result;
}

FVector3f FT5PapyrusRuntime::ResourcePoolRatios() const
{
    float Ratios[3];
    return Handle && Api->ResourcePools && Api->ResourcePools(Handle, Ratios)
        ? FVector3f(Ratios[0], Ratios[1], Ratios[2]) : FVector3f(-1);
}

bool FT5PapyrusRuntime::InventoryAction(uint32 Owner, uint32 Item, int32 Count)
{
    if (!Handle || !Api->InventoryAction) return false;
    const bool bAccepted = Api->InventoryAction(Handle, Owner, Item, Count) != 0;
    LogEvents();
    return bAccepted;
}

int32 FT5PapyrusRuntime::PlayerWeapon(int32 Action, uint32 Target, float* Reach)
{
    return Handle && Api->PlayerWeapon ? Api->PlayerWeapon(Handle, Action, Target, Reach) : 0;
}

bool FT5PapyrusRuntime::FirePlayerBow(const FVector& Origin, const FVector& Direction)
{
    if (!Handle || !Api->PlayerBow) return false;
    const float Aim[]{float(Origin.X), float(Origin.Y), float(Origin.Z),
        float(Direction.X), float(Direction.Y), float(Direction.Z)};
    const bool bFired = Api->PlayerBow(Handle, Aim) != 0;
    LogEvents();
    return bFired;
}

uint64 FT5PapyrusRuntime::PlayerViewKey(bool& bDrawn) const
{
    int Drawn = 0;
    const auto Key = Handle && Api->ViewKey ? Api->ViewKey(Handle, &Drawn) : 0;
    bDrawn = Drawn != 0;
    return Key;
}

TUniquePtr<FT5LoadResult> FT5PapyrusRuntime::PlayerView() const
{
    if (!Handle || !Api->PlayerView) return {};
    auto Result = MakeUnique<FT5LoadResult>();
    Result->GetBuffer = Api->GetBuffer;
    Result->Release = Api->Release;
    Result->Handle = Api->PlayerView(Handle);
    Result->Error = UTF8_TO_TCHAR(Api->GetError(Result->Handle));
    return Result;
}

TUniquePtr<FT5LoadResult> FT5PapyrusRuntime::PlayerBody() const
{
    if (!Handle || !Api->PlayerBody) return {};
    auto Result = MakeUnique<FT5LoadResult>();
    Result->GetBuffer = Api->GetBuffer;
    Result->Release = Api->Release;
    Result->Handle = Api->PlayerBody(Handle);
    Result->Error = UTF8_TO_TCHAR(Api->GetError(Result->Handle));
    return Result;
}

void FT5PapyrusRuntime::SetGamepad(bool bActive)
{
    if (Handle && Api->SetGamepad) Api->SetGamepad(Handle, bActive);
}

void FT5PapyrusRuntime::Tick(double Seconds)
{
    if (Handle)
    {
        Api->Tick(Handle, Seconds);
        LogEvents();
    }
}

bool FT5PapyrusRuntime::ReadResource(const FString& Path, TArray<uint8>& Bytes) const
{
    Bytes.Reset();
    if (!Handle || Path.IsEmpty())
        return false;
    FT5LoadResult Resource;
    Resource.GetBuffer = Api->GetBuffer;
    Resource.Release = Api->Release;
    Resource.Handle = Api->ReadResource(Handle, TCHAR_TO_UTF8(*Path));
    const auto Data = Resource.Read(TEXT("resource"));
    if (Data.IsEmpty() || Data.Num() > MAX_int32)
        return false;
    Bytes.Append(Data.GetData(), static_cast<int32>(Data.Num()));
    return true;
}

TOptional<FTransform> FT5PapyrusRuntime::ReferenceTransform(uint32 Id, FTransform& Authored, uint32& Parent) const
{
    float Poses[14];
    Parent = 0;
    if (!Handle || !Api->Transform(Handle, Id, Poses, &Parent))
        return {};
    const auto Convert = [](const float* Pose)
    {
        return FTransform(T5ToUnrealRotation(FQuat(Pose[3], Pose[4], Pose[5], Pose[6])),
            T5ToUnrealPosition(FVector(Pose[0], Pose[1], Pose[2])));
    };
    Authored = Convert(Poses);
    return Convert(Poses + 7);
}

int FT5PapyrusRuntime::MotionType(uint32 Id) const { return Handle ? Api->MotionType(Handle,Id) : 0; }

TOptional<bool> FT5PapyrusRuntime::DoorState(uint32 Id, double& Elapsed) const
{
    const int State = Handle ? Api->DoorState(Handle, Id, &Elapsed) : -1;
    return State < 0 ? TOptional<bool>{} : TOptional<bool>(State != 0);
}

uint32 FT5PapyrusRuntime::Weather(float (&Data)[12]) const
{
    return Handle ? Api->Weather(Handle, Data) : 0;
}

uint32 FT5PapyrusRuntime::Environment(TArrayView<float> Data) const
{
    if (Data.Num() < 15) return 0;
    if (Handle && Api->Environment) return Api->Environment(Handle, Data.GetData(), Data.Num());
    float Old[12]{};
    const auto Id = Weather(Old);
    FMemory::Memcpy(Data.GetData(), Old, sizeof(Old));
    Data[12] = 12;
    Data[13] = 1;
    Data[14] = 1;
    if (Data.Num() >= 18) FMemory::Memcpy(Data.GetData() + 15, Old + 6, 3 * sizeof(float));
    return Id;
}

void FT5PapyrusRuntime::BeginEnvironment(uint32 World)
{
    if(!Handle || !Api->BeginEnvironment)return;
    uint32 Initial=0;
    FString Text;TSharedPtr<FJsonObject> Profile;
    const auto Plugin=IPluginManager::Get().FindPlugin(TEXT("T5Data"));
    FString ProfilePath=Plugin?Plugin->GetBaseDir()/TEXT("Resources/IntroEnvironment.json"):FString{};
    FParse::Value(FCommandLine::Get(),TEXT("T5InitialEnvironment="),ProfilePath);
    if(!ProfilePath.IsEmpty() && FFileHelper::LoadFileToString(Text,*ProfilePath)
        && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),Profile) && Profile)
    {
        const auto ProfileWorld=uint32(FCString::Strtoui64(*Profile->GetStringField(TEXT("worldspace")),nullptr,16));
        if(ProfileWorld==World)Initial=uint32(FCString::Strtoui64(*Profile->GetStringField(TEXT("initial_weather")),nullptr,16));
    }
    Api->BeginEnvironment(Handle,World,Initial);
}

void FT5PapyrusRuntime::GameState(uint32& Controls, uint32& Flags, float& SittingRotation) const
{
    uint32 State[] = {0, 1};
    SittingRotation = 0;
    if (Handle)
        Api->GameState(Handle, State, &SittingRotation);
    Controls = State[0];
    Flags = State[1];
}

uint32 FT5PapyrusRuntime::SpawnedCount() const
{
    return Handle ? Api->Spawned(Handle, nullptr, 0) : 0;
}

void FT5PapyrusRuntime::AnimationPose(uint32 Id, const FTransform& Pose, bool bActive)
{
    const FVector P = UnrealToT5Position(Pose.GetLocation());
    const FQuat Q = T5ToUnrealRotation(Pose.GetRotation()); // involution: same conversion back
    const float Data[7] = {float(P.X), float(P.Y), float(P.Z), float(Q.X), float(Q.Y), float(Q.Z), float(Q.W)};
    if (Handle) Api->AnimationPose(Handle, Id, Data, bActive);
}

void FT5PapyrusRuntime::PhysicsPose(uint32 Id, const FTransform& Pose, bool bActive)
{
    const FVector P=UnrealToT5Position(Pose.GetLocation());
    const FQuat Q=T5ToUnrealRotation(Pose.GetRotation());
    const float Data[7]={float(P.X),float(P.Y),float(P.Z),float(Q.X),float(Q.Y),float(Q.Z),float(Q.W)};
    if (Handle) Api->AnimationPose(Handle,Id,Data,2 | int32(bActive));
}

void FT5PapyrusRuntime::FallResult(uint32 Actor, const FVector& Position, const FVector& Velocity, bool bGrounded)
{
    const auto P = UnrealToT5Position(Position);
    const auto V = UnrealToT5Position(Velocity);
    const float Pos[] = {float(P.X), float(P.Y), float(P.Z)};
    const float Vel[] = {float(V.X), float(V.Y), float(V.Z)};
    if (Handle && Api->FallResult) Api->FallResult(Handle, Actor, Pos, Vel, bGrounded);
}

void FT5PapyrusRuntime::FiringPosition(uint32 Actor, uint32 Target, const FVector& Position)
{
    const auto P = UnrealToT5Position(Position);
    const float Data[] = {float(P.X), float(P.Y), float(P.Z)};
    if (Handle && Api->FiringPosition) Api->FiringPosition(Handle, Actor, Target, Data);
}

const FT5MotionDatabase* FT5PapyrusRuntime::MotionData()
{
    if (!bMotionLoaded)
    {
        bMotionLoaded = true;
        TArray<uint8> Bytes;
        FString Error;
        auto Database = MakeShared<FT5MotionDatabase>();
        if (ReadResource(TEXT("meshes/animationdatasinglefile.txt"), Bytes) && Database->Read(Bytes, Error))
        {
            UE_LOG(LogT5Papyrus, Display, TEXT("T5_MOTION_CACHE projects=%d bytes=%d"), Database->ProjectCount(), Bytes.Num());
            MotionDatabase = MoveTemp(Database);
        }
        else UE_LOG(LogT5Papyrus, Error, TEXT("T5_MOTION_CACHE_ERROR %s"), Error.IsEmpty() ? TEXT("Missing animationdatasinglefile.txt") : *Error);
    }
    return MotionDatabase.Get();
}

void FT5PapyrusRuntime::AnimationEvent(uint32 Id, const FString& Event)
{
    if (Handle)
        Api->AnimationEvent(Handle, Id, TCHAR_TO_UTF8(*Event));
}

uint64 FT5PapyrusRuntime::Revision() const
{
    return Handle ? Api->Revision(Handle) : 0;
}

FT5AudioDecoder FT5PapyrusRuntime::AudioDecoder() const
{
    return [Functions = Api](TConstArrayView<uint8> Bytes)
    {
        FT5DecodedAudio Decoded;
        FT5LoadResult Result;
        Result.GetBuffer = Functions->GetBuffer;
        Result.Release = Functions->Release;
        Result.Handle = Functions->DecodeAudio(Bytes.GetData(), Bytes.Num());
        const auto Info = Result.Read(TEXT("audio.info"));
        const auto Pcm = Result.Read(TEXT("audio.pcm"));
        if (Info.Num() != 2 * sizeof(uint32) || Pcm.IsEmpty() || Pcm.Num() > MAX_int32)
        {
            Decoded.Error = UTF8_TO_TCHAR(Functions->GetError(Result.Handle));
            return Decoded;
        }
        FMemory::Memcpy(&Decoded.SampleRate, Info.GetData(), sizeof(uint32));
        FMemory::Memcpy(&Decoded.Channels, Info.GetData() + sizeof(uint32), sizeof(uint32));
        const auto Loop = Result.Read(TEXT("audio.loop"));
        if (Loop.Num() == 2 * sizeof(uint32))
        {
            FMemory::Memcpy(&Decoded.LoopStart, Loop.GetData(), sizeof(uint32));
            FMemory::Memcpy(&Decoded.LoopEnd, Loop.GetData() + sizeof(uint32), sizeof(uint32));
        }
        Decoded.Pcm.Append(Pcm.GetData(), static_cast<int32>(Pcm.Num()));
        return Decoded;
    };
}

TArray<FT5AudioRequest> FT5PapyrusRuntime::TakeAudioRequests()
{
    return MoveTemp(AudioRequests);
}

void FT5PapyrusRuntime::ArrowImpact(uint32 Shot, uint32 Target)
{
    if (Handle && Api->ArrowImpact)
        Api->ArrowImpact(Handle, Shot, Target);
}

void FT5PapyrusRuntime::AudioFinished(int32 Instance, bool bPlayed)
{
    if (Handle && Api->AudioFinished)
        Api->AudioFinished(Handle, Instance, bPlayed);
}

void FT5PapyrusRuntime::ReportHostError(const FString& Message)
{
    FString Display = Message.Left(340);
    ErrorLines.Remove(Display);
    ErrorLines.Insert(MoveTemp(Display), 0);
    if (ErrorLines.Num() > 5) ErrorLines.Pop();
    ErrorText = FString::Join(ErrorLines, TEXT("\n"));
}

void FT5PapyrusRuntime::LogEvents()
{
    const auto* Message = Api->Messages(Handle);
    if (Message && *Message)
    {
        TArray<FString> Lines;
        FString(UTF8_TO_TCHAR(Message)).ParseIntoArrayLines(Lines);
        for (const auto& Line : Lines)
        {
            if (Line.StartsWith(TEXT("T5_QUEST_STAGE ")))
            {
                FString Quest, Stage;
                if (Line.Mid(15).Split(TEXT(" stage="), &Quest, &Stage))
                    QuestStages.FindOrAdd(Quest) = FMath::Max(QuestStages.FindRef(Quest), FCString::Atoi(*Stage));
            }
            UE_LOG(LogT5Papyrus, Display, TEXT("%s"), *Line);
        }
    }
    const auto* Errors = Api->Errors(Handle);
    if (Errors && *Errors)
    {
        TArray<FString> Lines;
        FString(UTF8_TO_TCHAR(Errors)).ParseIntoArrayLines(Lines);
        for (const auto& Line : Lines)
        {
            UE_LOG(LogT5Papyrus, Warning, TEXT("T5_PAPYRUS_ERROR %s"), *Line);
            ReportHostError(Line);
        }
    }
    if (Api->Presentation)
        if (const char* Commands = Api->Presentation(Handle); Commands && *Commands)
        {
            TArray<TSharedPtr<FJsonValue>> Rows;
            if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(UTF8_TO_TCHAR(Commands))), Rows))
                Presentation.Append(MoveTemp(Rows));
        }
    if (const char* Commands = Api->Animations(Handle); Commands && *Commands)
    {
        TArray<TSharedPtr<FJsonValue>> Rows;
        if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(UTF8_TO_TCHAR(Commands))), Rows))
            for (const auto& Row : Rows)
            {
                const auto& Values = Row->AsArray();
                if (Values.Num() == 5 || Values.Num() == 6)
                    Animations.Add(static_cast<uint32>(Values[0]->AsNumber()), {static_cast<uint32>(Values[1]->AsNumber()), ++AnimationSerial,
                        Values[2]->AsString(), Values[3]->AsString(), Values[4]->AsString(),
                        Values.Num() == 6 ? static_cast<float>(Values[5]->AsNumber()) : 1.f});
            }
    }
    if (const char* Commands = Api->Audio(Handle); Commands && *Commands)
    {
        TArray<TSharedPtr<FJsonValue>> Rows;
        if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(UTF8_TO_TCHAR(Commands))), Rows))
            for (const auto& Row : Rows)
            {
                const auto& Values = Row->AsArray();
                if (Values.Num() != 11 && Values.Num() != 18 && Values.Num() != 20)
                    continue;
                const int32 Command = Values[0]->AsNumber();
                if (Command < FT5AudioRequest::Play || Command > FT5AudioRequest::Volume)
                    continue;
                FT5AudioRequest Request{static_cast<FT5AudioRequest::ECommand>(Command), static_cast<int32>(Values[1]->AsNumber()),
                    static_cast<uint32>(Values[2]->AsNumber()), Values[3]->AsString(), Values[4]->AsNumber(),
                    static_cast<float>(Values[5]->AsNumber()), static_cast<float>(Values[6]->AsNumber()),
                    static_cast<float>(Values[7]->AsNumber()), static_cast<float>(Values[8]->AsNumber()),
                    Values[9]->AsBool(), Values[10]->AsBool()};
                if (Values.Num() >= 18)
                {
                    Request.Fade = Values[11]->AsNumber();
                    Request.LoopBegin = Values[12]->AsNumber();
                    Request.LoopEnd = Values[13]->AsNumber();
                    Request.LoopCount = Values[14]->AsNumber();
                    Request.bMusic = Values[15]->AsBool();
                    Request.Finale = Values[16]->AsString();
                    for (const auto& Cue : Values[17]->AsArray())
                        Request.Cues.Add(Cue->AsNumber());
                }
                if (Values.Num() >= 20)
                {
                    Request.Emotion = static_cast<int32>(Values[18]->AsNumber());
                    Request.EmotionStrength = Values[19]->AsNumber();
                }
                AudioRequests.Add(MoveTemp(Request));
            }
    }
}

TConstArrayView64<uint8> FT5LoadResult::Read(const FString& Name) const
{
    if (!Handle || Name.IsEmpty())
        return {};
    uint64 Size = 0;
    const auto* Bytes = static_cast<const uint8*>(GetBuffer(Handle, TCHAR_TO_UTF8(*Name), &Size));
    return {Bytes, static_cast<int64>(Size)};
}

TUniquePtr<FT5LoadResult> LoadT5Scene(const FString& Data, const FString& Plugins, const FString& BridgeDirectory,
    const FT5LoadRequest& Request)
{
    auto Result = MakeUnique<FT5LoadResult>();
    // One native module per process. Its function pointers remain valid for pending loads.
    static void* Library = [&BridgeDirectory]
    {
        FPlatformProcess::PushDllDirectory(*BridgeDirectory);
        void* Handle = FPlatformProcess::GetDllHandle(*FPaths::Combine(BridgeDirectory, TEXT("T5DataBridge.dll")));
        FPlatformProcess::PopDllDirectory(*BridgeDirectory);
        return Handle;
    }();
    if (!Library)
    {
        Result->Error = FString::Printf(TEXT("Could not load T5DataBridge.dll from %s"), *BridgeDirectory);
        return Result;
    }

    const auto Version = reinterpret_cast<uint32 (*)()>(FPlatformProcess::GetDllExport(Library, TEXT("T5DataBridgeVersion")));
    const auto Load = reinterpret_cast<void* (*)(const char*, const char*, const char*, const char*)>(
        FPlatformProcess::GetDllExport(Library, TEXT("T5DataLoadHelgen")));
    const auto LoadCatalog = reinterpret_cast<void* (*)(const char*, const char*)>(
        FPlatformProcess::GetDllExport(Library, TEXT("T5DataCatalog")));
    const auto LoadLocation = reinterpret_cast<void* (*)(const char*, const char*, const char*, const char*, uint32, int32, int32, int32)>(
        FPlatformProcess::GetDllExport(Library, TEXT("T5DataLoadLocation")));
    const auto LoadPlayable = reinterpret_cast<void* (*)(const char*, const char*, const char*, const char*, uint32, int32, int32, int32,
        uint64, const uint32*, uint32, uint32, const FT5SpawnedReference*, uint32)>(
        FPlatformProcess::GetDllExport(Library, TEXT("T5DataLoadGameLocation")));
    const auto LoadStart = reinterpret_cast<void* (*)(const char*, const char*, uint32)>(
        FPlatformProcess::GetDllExport(Library, TEXT("T5DataNewGameStart")));
    static const FT5PapyrusApi Papyrus{
        reinterpret_cast<decltype(FT5PapyrusApi::Create)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusCreate"))),
        reinterpret_cast<decltype(FT5PapyrusApi::Destroy)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusDestroy"))),
        reinterpret_cast<decltype(FT5PapyrusApi::Configure)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusConfigure"))),
        reinterpret_cast<decltype(FT5PapyrusApi::Attach)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusAttach"))),
        reinterpret_cast<decltype(FT5PapyrusApi::Revision)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusRevision"))),
        reinterpret_cast<decltype(FT5PapyrusApi::Tracked)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusTracked"))),
        reinterpret_cast<decltype(FT5PapyrusApi::IsDisabled)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusIsDisabled"))),
        reinterpret_cast<decltype(FT5PapyrusApi::HasScripts)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusHasScripts"))),
        reinterpret_cast<decltype(FT5PapyrusApi::Activate)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusActivate"))),
        reinterpret_cast<decltype(FT5PapyrusApi::Tick)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusTick"))),
        reinterpret_cast<decltype(FT5PapyrusApi::Messages)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusMessages"))),
        reinterpret_cast<decltype(FT5PapyrusApi::Errors)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusErrors"))),
        reinterpret_cast<decltype(FT5PapyrusApi::Presentation)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusPresentation"))),
        reinterpret_cast<decltype(FT5PapyrusApi::ArrowImpact)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusArrowImpact"))),
        reinterpret_cast<decltype(FT5PapyrusApi::Animations)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusAnimations"))),
        reinterpret_cast<decltype(FT5PapyrusApi::Audio)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusAudio"))),
        reinterpret_cast<decltype(FT5PapyrusApi::AudioFinished)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusAudioFinished"))),
        reinterpret_cast<decltype(FT5PapyrusApi::AnimationPose)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusAnimationPose"))),
        reinterpret_cast<decltype(FT5PapyrusApi::AnimationEvent)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusAnimationEvent"))),
        reinterpret_cast<decltype(FT5PapyrusApi::ReadResource)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusReadResource"))),
        reinterpret_cast<decltype(FT5PapyrusApi::Spawned)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusSpawned"))),
        reinterpret_cast<decltype(FT5PapyrusApi::Transform)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusTransform"))),
        reinterpret_cast<decltype(FT5PapyrusApi::MotionType)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusMotionType"))),
        reinterpret_cast<decltype(FT5PapyrusApi::DoorState)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusDoorState"))),
        reinterpret_cast<decltype(FT5PapyrusApi::Weather)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusWeather"))),
        reinterpret_cast<decltype(FT5PapyrusApi::GameState)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusGameState"))),
        reinterpret_cast<decltype(FT5PapyrusApi::DecodeAudio)>(FPlatformProcess::GetDllExport(Library, TEXT("T5DataDecodeAudio"))),
        reinterpret_cast<decltype(FT5PapyrusApi::GetError)>(FPlatformProcess::GetDllExport(Library, TEXT("T5DataGetError"))),
        reinterpret_cast<decltype(FT5PapyrusApi::GetBuffer)>(FPlatformProcess::GetDllExport(Library, TEXT("T5DataGetBuffer"))),
        reinterpret_cast<decltype(FT5PapyrusApi::Release)>(FPlatformProcess::GetDllExport(Library, TEXT("T5DataRelease"))),
        reinterpret_cast<decltype(FT5PapyrusApi::SaveCheckpoint)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusSaveCheckpoint"))),
        reinterpret_cast<decltype(FT5PapyrusApi::RestoreCheckpoint)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusRestoreCheckpoint"))),
        reinterpret_cast<decltype(FT5PapyrusApi::CharacterCatalog)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusCharacterCatalog"))),
        reinterpret_cast<decltype(FT5PapyrusApi::CharacterPreview)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusCharacterPreview"))),
        reinterpret_cast<decltype(FT5PapyrusApi::CommitCharacter)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusCommitCharacter"))),
        reinterpret_cast<decltype(FT5PapyrusApi::PlayerEyeHeight)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusPlayerEyeHeight"))),
        reinterpret_cast<decltype(FT5PapyrusApi::SetGamepad)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusSetGamepad"))),
        reinterpret_cast<decltype(FT5PapyrusApi::VisualScale)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusVisualScale"))),
        reinterpret_cast<decltype(FT5PapyrusApi::Environment)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusEnvironment"))),
        reinterpret_cast<decltype(FT5PapyrusApi::BeginEnvironment)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusBeginEnvironment"))),
        reinterpret_cast<decltype(FT5PapyrusApi::FallResult)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusFallResult"))),
        reinterpret_cast<decltype(FT5PapyrusApi::FiringPosition)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusFiringPosition"))),
        reinterpret_cast<decltype(FT5PapyrusApi::SetLineOfSight)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusSetLineOfSight"))),
        reinterpret_cast<decltype(FT5PapyrusApi::Inventory)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusInventory"))),
        reinterpret_cast<decltype(FT5PapyrusApi::InventoryAction)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusInventoryAction"))),
        reinterpret_cast<decltype(FT5PapyrusApi::PlayerWeapon)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusPlayerWeapon"))),
        reinterpret_cast<decltype(FT5PapyrusApi::PlayerBow)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusPlayerBow"))),
        reinterpret_cast<decltype(FT5PapyrusApi::WasKilled)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusWasKilled"))),
        reinterpret_cast<decltype(FT5PapyrusApi::ViewKey)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusViewKey"))),
        reinterpret_cast<decltype(FT5PapyrusApi::PlayerView)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusPlayerView"))),
        reinterpret_cast<decltype(FT5PapyrusApi::PlayerBody)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusPlayerBody"))),
        reinterpret_cast<decltype(FT5PapyrusApi::SeveredHead)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusSeveredHead"))),
        reinterpret_cast<decltype(FT5PapyrusApi::AnimationObject)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusAnimationObject"))),
        reinterpret_cast<decltype(FT5PapyrusApi::Dialogue)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusDialogue"))),
        reinterpret_cast<decltype(FT5PapyrusApi::DestructionModel)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusDestructionModel"))),
        reinterpret_cast<decltype(FT5PapyrusApi::InventoryPresentation)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusInventoryPresentation"))),
        reinterpret_cast<decltype(FT5PapyrusApi::InventoryModel)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusInventoryModel"))),
        reinterpret_cast<decltype(FT5PapyrusApi::ResourcePools)>(FPlatformProcess::GetDllExport(Library, TEXT("T5PapyrusResourcePools")))
    };
    const auto GetError = reinterpret_cast<const char* (*)(const void*)>(
        FPlatformProcess::GetDllExport(Library, TEXT("T5DataGetError")));
    Result->GetBuffer = reinterpret_cast<decltype(Result->GetBuffer)>(FPlatformProcess::GetDllExport(Library, TEXT("T5DataGetBuffer")));
    Result->Release = reinterpret_cast<decltype(Result->Release)>(FPlatformProcess::GetDllExport(Library, TEXT("T5DataRelease")));
    if (!Version || Version() != 5 || !Load || !LoadCatalog || !LoadLocation || !GetError || !Result->GetBuffer || !Result->Release
        || !LoadPlayable || !Papyrus.Create || !Papyrus.Destroy || !Papyrus.Attach || !Papyrus.Revision || !Papyrus.Tracked
        || !Papyrus.IsDisabled || !Papyrus.HasScripts || !Papyrus.Activate || !Papyrus.Tick || !Papyrus.Messages
        || !LoadStart || !Papyrus.Configure || !Papyrus.Errors || !Papyrus.Animations || !Papyrus.ReadResource || !Papyrus.Spawned
        || !Papyrus.Transform || !Papyrus.GameState || !Papyrus.AnimationEvent || !Papyrus.AnimationPose || !Papyrus.Audio || !Papyrus.DecodeAudio
        || !Papyrus.MotionType || !Papyrus.DoorState || !Papyrus.Weather
        || !Papyrus.CharacterCatalog || !Papyrus.CharacterPreview || !Papyrus.CommitCharacter || !Papyrus.PlayerEyeHeight || !Papyrus.SetGamepad)
    {
        Result->Error = TEXT("Incompatible T5DataBridge API");
        return Result;
    }

    const auto SetCache = reinterpret_cast<void (*)(const char*)>(
        FPlatformProcess::GetDllExport(Library, TEXT("T5DataSetCacheDirectory")));
    if (!SetCache)
    {
        Result->Error = FString::Printf(TEXT("Outdated T5DataBridge.dll in %s: resource cache API is missing. Update the native bridge in this runtime."), *BridgeDirectory);
        UE_LOG(LogT5Resources, Error, TEXT("%s"), *Result->Error);
        return Result;
    }
    const FString CachePath = FParse::Param(FCommandLine::Get(), TEXT("T5NoResourceCache")) ? FString{}
        : FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("T5ResourceCache"));
    SetCache(TCHAR_TO_UTF8(*CachePath));
    struct FNativeLoadStats
    {
        TSharedPtr<FT5LoadProgress> Progress;
        uint64 PluginDiskReads = 0, ResidentScans = 0, Conversions = 0, CacheReads = 0;
    } Stats{Request.Progress};

    using FProgressCallback = void (*)(void*, const char*, const char*, uint64, uint64, uint64, uint64);
    const auto SetProgress = reinterpret_cast<void (*)(FProgressCallback, void*)>(
        FPlatformProcess::GetDllExport(Library, TEXT("T5DataSetLoadProgress")));
    struct FProgressScope
    {
        decltype(SetProgress) Setter;
        ~FProgressScope() { if (Setter) Setter(nullptr, nullptr); }
    } ProgressScope{SetProgress};
    if (SetProgress)
        SetProgress([](void* Context, const char* Phase, const char* Item, uint64 Done, uint64 Total, uint64 Textures, uint64 Bytes)
        {
            auto& NativeStats = *static_cast<FNativeLoadStats*>(Context);
            const FString Name = UTF8_TO_TCHAR(Phase);
            if (!Done && !*Item)
            {
                NativeStats.PluginDiskReads += Name == TEXT("Reading plugin bytes");
                NativeStats.ResidentScans += Name == TEXT("Scanning resident plugin records");
                NativeStats.Conversions += Name == TEXT("Converting models and textures");
                NativeStats.CacheReads += Name == TEXT("Reading converted resource cache");
            }
            if (NativeStats.Progress)
            {
                auto& Progress = *NativeStats.Progress;
                FScopeLock Lock(&Progress.Mutex);
                Progress.Phase = Name; Progress.Item = UTF8_TO_TCHAR(Item);
                Progress.Done = Done; Progress.Total = Total; Progress.Textures = Textures; Progress.Bytes = Bytes;
            }
        }, &Stats);

    const FString Resources = FPaths::Combine(BridgeDirectory, TEXT("Resources"));
    const FString OsgPlugins = FPaths::Combine(BridgeDirectory, TEXT("osgPlugins-3.6.5"));
    const FTCHARToUTF8 DataPath(*Data), PluginPath(*Plugins);
    const double NativeStarted = FPlatformTime::Seconds();
    if (Request.bNewGameStart)
        Result->Handle = LoadStart(DataPath.Get(), PluginPath.Get(), Request.OpeningQuest);
    else if (Request.bCatalog)
        Result->Handle = LoadCatalog(DataPath.Get(), PluginPath.Get());
    else if (Request.FormId && Request.bPlayable)
    {
        Result->Handle = LoadPlayable(DataPath.Get(), PluginPath.Get(), TCHAR_TO_UTF8(*Resources), TCHAR_TO_UTF8(*OsgPlugins),
            Request.FormId, Request.Cell.X, Request.Cell.Y, Request.Radius, Request.ScriptRevision,
            Request.TrackedReferences.GetData(), Request.TrackedReferences.Num(), Request.OpeningQuest,
            Request.SpawnedReferences.GetData(), Request.SpawnedReferences.Num());
        Result->Papyrus = &Papyrus;
    }
    else if (Request.FormId)
        Result->Handle = LoadLocation(DataPath.Get(), PluginPath.Get(), TCHAR_TO_UTF8(*Resources), TCHAR_TO_UTF8(*OsgPlugins),
            Request.FormId, Request.Cell.X, Request.Cell.Y, Request.Radius);
    else
        Result->Handle = Load(DataPath.Get(), PluginPath.Get(), TCHAR_TO_UTF8(*Resources), TCHAR_TO_UTF8(*OsgPlugins));
    Result->Error = UTF8_TO_TCHAR(GetError(Result->Handle));
    UE_LOG(LogT5Resources, Display, TEXT("T5_RESOURCE_LOAD id=%08X catalog=%d start=%d diskPlugins=%llu memoryScans=%llu conversions=%llu cacheReads=%llu seconds=%.3f cache=%s bridge=%s error=%s"),
        Request.FormId, Request.bCatalog, Request.bNewGameStart, Stats.PluginDiskReads, Stats.ResidentScans, Stats.Conversions, Stats.CacheReads,
        FPlatformTime::Seconds() - NativeStarted, *CachePath, *BridgeDirectory, *Result->Error);
    return Result;
}



bool FT5PapyrusRuntime::SaveCheckpoint(TArray<uint8>& Bytes) const
{
    if (!Handle || !Api->SaveCheckpoint) return false;
    FT5LoadResult Result;
    Result.GetBuffer = Api->GetBuffer; Result.Release = Api->Release;
    Result.Handle = Api->SaveCheckpoint(Handle);
    const auto Data = Result.Read(TEXT("checkpoint"));
    if (Data.IsEmpty() || Data.Num() > 256 * 1024 * 1024) return false;
    Bytes.Append(Data.GetData(), int32(Data.Num()));
    return true;
}

void FT5PapyrusRuntime::SerializeCheckpoint(FArchive& Ar)
{
    Ar << ErrorLines << ErrorText;
    if (Ar.IsLoading())
    {
        // Diagnostics in legacy saves describe the old runtime, not this session.
        for (const auto& Line : ErrorLines) UE_LOG(LogT5Papyrus, Display, TEXT("T5_SAVED_DIAGNOSTIC %s"), *Line);
        ErrorLines.Reset(); ErrorText.Reset();
    }
    int32 AudioCount = AudioRequests.Num(); Ar << AudioCount;
    if (AudioCount < 0 || AudioCount > 4096) { Ar.SetError(); return; }
    if (Ar.IsLoading()) AudioRequests.SetNum(AudioCount);
    for (auto& Request : AudioRequests) SerializeT5AudioRequest(Ar, Request);
    FString PresentationJson;
    if (Ar.IsSaving()) FJsonSerializer::Serialize(Presentation, TJsonWriterFactory<>::Create(&PresentationJson));
    Ar << PresentationJson;
    if (Ar.IsLoading()) FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(PresentationJson), Presentation);
    Ar << AnimationSerial;
    int32 Count = Animations.Num(); Ar << Count;
    if (Count < 0 || Count > 100000) { Ar.SetError(); return; }
    if (Ar.IsLoading()) Animations.Reset();
    const auto Serialize = [&](uint32& Id, FT5AnimationRequest& R) {
        Ar << Id << R.Idle << R.Serial << R.Behavior << R.Event << R.Clip << R.Speed;
    };
    if (Ar.IsLoading())
        for (int32 I = 0; I < Count && !Ar.IsError(); ++I)
        { uint32 Id = 0; FT5AnimationRequest R{}; Serialize(Id, R); Animations.Add(Id, MoveTemp(R)); }
    else for (auto& [Id, R] : Animations) { uint32 Key = Id; Serialize(Key, R); }
}

TUniquePtr<FT5LoadResult> FT5PapyrusRuntime::CharacterCatalog() const
{
    auto Result = MakeUnique<FT5LoadResult>();
    if (!Handle || !Api->CharacterCatalog) { Result->Error = TEXT("Character API unavailable"); return Result; }
    Result->GetBuffer = Api->GetBuffer; Result->Release = Api->Release;
    Result->Handle = Api->CharacterCatalog(Handle);
    Result->Error = UTF8_TO_TCHAR(Api->GetError(Result->Handle));
    return Result;
}
TUniquePtr<FT5LoadResult> FT5PapyrusRuntime::CharacterPreview(const FT5CharacterChoice& Choice, const FString& Bridge, const FString& Mods) const
{
    auto Result = MakeUnique<FT5LoadResult>();
    Result->GetBuffer = Api->GetBuffer; Result->Release = Api->Release;
    Result->Handle = Api->CharacterPreview(Handle, &Choice, sizeof(Choice),
        TCHAR_TO_UTF8(*(Bridge / TEXT("Resources"))), TCHAR_TO_UTF8(*(Bridge / TEXT("osgPlugins-3.6.5"))), TCHAR_TO_UTF8(*Mods));
    Result->Error = UTF8_TO_TCHAR(Api->GetError(Result->Handle));
    return Result;
}
bool FT5PapyrusRuntime::CommitCharacter(const FT5CharacterChoice& Choice, const FString& Name, const FString& Morphs)
{
    const bool OK = Handle && Api->CommitCharacter && Api->CommitCharacter(Handle, &Choice, sizeof(Choice), TCHAR_TO_UTF8(*Name), TCHAR_TO_UTF8(*Morphs));
    LogEvents(); return OK;
}
float FT5PapyrusRuntime::PlayerEyeHeight() const
{
    return (Handle && Api->PlayerEyeHeight ? Api->PlayerEyeHeight(Handle) : 124.343f*1.03f)*T5CentimetersPerUnit;
}

float FT5PapyrusRuntime::VisualScale(uint32 Id) const
{
    return Handle && Api->VisualScale ? Api->VisualScale(Handle, Id) : 1.f;
}
