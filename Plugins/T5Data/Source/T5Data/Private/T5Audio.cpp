#include "T5Audio.h"
#include "T5Lip.h"

#include "T5Bridge.h"
#include "T5Coordinates.h"
#include "Async/Async.h"
#include "AudioDevice.h"
#include "Components/AudioComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Sound/SoundWaveProcedural.h"

DEFINE_LOG_CATEGORY_STATIC(LogT5Audio, Log, All);

namespace
{
    constexpr int32 MaxActiveSounds = 128;

    struct FVoice
    {
        FT5AudioRequest Request;
        TFuture<FT5DecodedAudio> Decode;
        TSharedPtr<FT5LipTrack> Lip;
        TWeakObjectPtr<UAudioComponent> Audio;
        double Due = 0, Start = 0, End = 0, StopAt = 0, DestroyAt = 0;
        float StopFade = 0;
        bool bStarted = false, bReportedFrames = false, bStopping = false;
        int32 QueuedBytes = 0;
        double ResumeSeconds = 0;
        TSharedPtr<TAtomic<int32>, ESPMode::ThreadSafe> LoopRepeats;
    };

    void StopVoice(FVoice& Voice)
    {
        if (auto* Audio = Voice.Audio.Get())
        {
            Audio->Stop();
            Audio->DestroyComponent();
        }
    }

    UAudioComponent* PlayVoice(AActor& Owner, FVoice& Voice, FT5DecodedAudio&& Decoded, const FVector& Location, double Now)
    {
        auto* Audio = NewObject<UAudioComponent>(&Owner, NAME_None, RF_Transient);
        auto* Wave = NewObject<USoundWaveProcedural>(Audio, NAME_None, RF_Transient);
        Wave->SetSampleRate(Decoded.SampleRate);
        Wave->NumChannels = Decoded.Channels;
        Voice.Lip = MoveTemp(Decoded.Lip);
        if (Voice.Lip) UE_LOG(LogT5Audio, Display, TEXT("T5_LIP_PLAY source=%08X frames=%d first=%d path=%s"),
            Voice.Request.Source, Voice.Lip->Samples.Num()/33, Voice.Lip->FirstFrame, *Voice.Request.Path);
        auto Request = Voice.Request;
        const double Resume = Voice.ResumeSeconds;
        if (Resume > 0 && Request.LoopCount && Request.LoopEnd > Request.LoopBegin)
        {
            const int32 FrameBytes = Decoded.Channels * sizeof(int16);
            const int32 Frames = Decoded.Pcm.Num() / FrameBytes;
            const int32 Begin = FMath::Clamp(FMath::RoundToInt(Request.LoopBegin * Decoded.SampleRate), 0, Frames) * FrameBytes;
            const int32 End = FMath::Clamp(FMath::RoundToInt(Request.LoopEnd * Decoded.SampleRate), Begin / FrameBytes, Frames) * FrameBytes;
            const int64 ExpandedSize = int64(Decoded.Pcm.Num()) + int64(End - Begin) * Request.LoopCount;
            if (ExpandedSize < 128 * 1024 * 1024)
            {
                TArray<uint8> Expanded; Expanded.Reserve(int32(ExpandedSize)); Expanded.Append(Decoded.Pcm.GetData(), End);
                for (uint32 I = 0; I < Request.LoopCount; ++I) Expanded.Append(Decoded.Pcm.GetData() + Begin, End - Begin);
                Expanded.Append(Decoded.Pcm.GetData() + End, Decoded.Pcm.Num() - End);
                Decoded.Pcm = MoveTemp(Expanded); Request.LoopCount = 0;
            }
        }
        const uint32 FrameBytes = Decoded.Channels * sizeof(int16);
        const int32 Frames = Decoded.Pcm.Num() / FrameBytes;
        const double BytesPerSecond = double(FrameBytes) * Decoded.SampleRate;
        double Duration = Decoded.Pcm.Num() / BytesPerSecond;
        const int32 LoopStart = FMath::RoundToInt(FMath::Clamp(double(Request.LoopBegin) * Decoded.SampleRate, 0., double(Frames)));
        const int32 LoopEnd = FMath::RoundToInt(FMath::Clamp(double(Request.LoopEnd) * Decoded.SampleRate, 0., double(Frames)));
        const bool bFiniteLoop = Request.LoopCount && LoopEnd > LoopStart;
        if (bFiniteLoop)
            Duration += double(LoopEnd - LoopStart) * Request.LoopCount / Decoded.SampleRate;
        Wave->Duration = Voice.Request.bLoop ? INDEFINITELY_LOOPING_DURATION : Duration;
        Voice.QueuedBytes = Decoded.Pcm.Num();
        if (Voice.Request.bLoop)
        {
            const bool bAuthoredLoop = Decoded.LoopStart < Decoded.LoopEnd && Decoded.LoopEnd <= Decoded.Pcm.Num() / FrameBytes;
            const int32 Start = bAuthoredLoop ? Decoded.LoopStart * FrameBytes : 0;
            const int32 End = bAuthoredLoop ? Decoded.LoopEnd * FrameBytes : Decoded.Pcm.Num();
            const TSharedRef<const TArray<uint8>, ESPMode::ThreadSafe> Samples =
                MakeShared<TArray<uint8>, ESPMode::ThreadSafe>(MoveTemp(Decoded.Pcm));
            const auto Repeats = MakeShared<TAtomic<int32>, ESPMode::ThreadSafe>(0);
            Voice.LoopRepeats = Repeats;
            Voice.QueuedBytes = End;
            const int64 PlayedBytes = int64(Resume * Decoded.SampleRate) * FrameBytes;
            const int32 Offset = int32(PlayedBytes < End ? PlayedBytes : Start + (PlayedBytes - End) % FMath::Max(1, End - Start));
            Wave->QueueAudio(Samples->GetData() + Offset, End - Offset);
            Wave->OnSoundWaveProceduralUnderflow.BindLambda(
                [Samples, Repeats, Start, End](USoundWaveProcedural* Sound, int32)
                {
                    Sound->QueueAudio(Samples->GetData() + Start, End - Start);
                    ++*Repeats;
                });
        }
        else if (bFiniteLoop)
        {
            const int32 Start = LoopStart * FrameBytes;
            const int32 End = LoopEnd * FrameBytes;
            const TSharedRef<const TArray<uint8>, ESPMode::ThreadSafe> Samples =
                MakeShared<TArray<uint8>, ESPMode::ThreadSafe>(MoveTemp(Decoded.Pcm));
            Voice.QueuedBytes = End;
            const int64 PlayedBytes = int64(Resume * Decoded.SampleRate) * FrameBytes;
            const int32 Offset = int32(PlayedBytes < End ? PlayedBytes : Start + (PlayedBytes - End) % FMath::Max(1, End - Start));
            Wave->QueueAudio(Samples->GetData() + Offset, End - Offset);
            Wave->OnSoundWaveProceduralUnderflow.BindLambda(
                [Samples, Start, End, Remaining = Request.LoopCount, bQueuedTail = false](USoundWaveProcedural* Sound, int32) mutable
                {
                    if (Remaining > 0)
                    {
                        Sound->QueueAudio(Samples->GetData() + Start, End - Start);
                        --Remaining;
                    }
                    else if (!bQueuedTail && End < Samples->Num())
                    {
                        Sound->QueueAudio(Samples->GetData() + End, Samples->Num() - End);
                        bQueuedTail = true;
                    }
                });
        }
        else
        {
            const int32 Offset = int32(FMath::Min(double(Decoded.Pcm.Num()), Resume * BytesPerSecond)) / FrameBytes * FrameBytes;
            Wave->QueueAudio(Decoded.Pcm.GetData() + Offset, Decoded.Pcm.Num() - Offset);
        }

        Audio->bAutoActivate = false;
        Audio->bAllowSpatialization = Request.bSpatial;
        if (Request.bSpatial)
        {
            FSoundAttenuationSettings Attenuation;
            Attenuation.bSpatialize = true;
            Attenuation.bAttenuate = Request.MaxDistance > Request.MinDistance;
            if (Attenuation.bAttenuate)
            {
                Attenuation.AttenuationShapeExtents = FVector(Request.MinDistance * T5CentimetersPerUnit, 0, 0);
                Attenuation.FalloffDistance = (Request.MaxDistance - Request.MinDistance) * T5CentimetersPerUnit;
            }
            Audio->bOverrideAttenuation = true;
            Audio->SetAttenuationOverrides(Attenuation);
        }
        Audio->SetVolumeMultiplier(Request.bMusic ? 1 : Request.Gain);
        Audio->SetPitchMultiplier(Request.Pitch);
        Audio->SetSound(Wave);
        Audio->SetWorldLocation(Location);
        Audio->RegisterComponentWithWorld(Owner.GetWorld());
        if (Request.bMusic)
            Audio->FadeIn(Request.Fade, Request.Gain);
        else
            Audio->Play();

        Voice.Start = Now - Resume / Request.Pitch;
        Voice.End = Voice.Start + Duration / Request.Pitch;
        UE_LOG(LogT5Audio, Display, TEXT("T5_AUDIO_PLAY instance=%d source=%08X sample_rate=%u channels=%u frames=%d loop=%d range=%u-%u path=%s"),
            Request.Instance, Request.Source, Decoded.SampleRate, Decoded.Channels,
            Voice.QueuedBytes / static_cast<int32>(FrameBytes), Request.bLoop, Decoded.LoopStart, Decoded.LoopEnd, *Request.Path);
        return Audio;
    }
}

struct FT5AudioRuntime::FState
{
    TMap<int32, FVoice> Voices;
    uint64 Revision = 0;
    int32 NextFinale = -1;
    bool bPaused = false;
#if WITH_DEV_AUTOMATION_TESTS
    TSet<FString> MixedPaths;
#endif
};

FT5AudioRuntime::FT5AudioRuntime() : State(MakeUnique<FState>()) {}
FT5AudioRuntime::~FT5AudioRuntime() { Reset(); }

void FT5AudioRuntime::Reset()
{
    for (auto& Entry : State->Voices)
        StopVoice(Entry.Value);
    State->Voices.Reset();
    State->bPaused = false;
#if WITH_DEV_AUTOMATION_TESTS
    State->MixedPaths.Reset();
#endif
}

#if WITH_DEV_AUTOMATION_TESTS
bool FT5AudioRuntime::HasMixed(const TCHAR* Suffix) const
{
    for (const auto& Path : State->MixedPaths)
        if (Path.EndsWith(Suffix))
            return true;
    return false;
}
#endif

void FT5AudioRuntime::Tick(AActor& Owner, FT5PapyrusRuntime& Papyrus, TFunctionRef<TOptional<FVector>(uint32)> Position,
    double SimulationTime, bool bPaused)
{
    if (State->Revision != Papyrus.Revision())
    {
        Reset();
        State->Revision = Papyrus.Revision();
    }
    auto Requests = Papyrus.TakeAudioRequests();
    auto* World = Owner.GetWorld();
    if (!World || !World->GetAudioDevice().IsValid() || !GEngine || !GEngine->UseSound())
        return;
    const double Now = SimulationTime;
    if (State->bPaused != bPaused)
    {
        State->bPaused = bPaused;
        UE_LOG(LogT5Audio, Display, TEXT("T5_AUDIO_PAUSE paused=%d time=%.4f voices=%d"),
            bPaused, Now, State->Voices.Num());
    }
    const auto Queue = [&](FT5AudioRequest Request, double Due)
    {
        TArray<uint8> Bytes;
        if (State->Voices.Num() >= MaxActiveSounds || !Papyrus.ReadResource(Request.Path, Bytes))
        {
            UE_LOG(LogT5Audio, Warning, TEXT("Cannot queue T5 audio resource %s"), *Request.Path);
            if (Request.bMusic)
                Papyrus.AudioFinished(Request.Instance, false);
            return;
        }
        FVoice Voice;
        Voice.Due = Due;
        Voice.Request = MoveTemp(Request);
        Voice.Decode = Async(EAsyncExecution::ThreadPool, [Decode = Papyrus.AudioDecoder(), Bytes = MoveTemp(Bytes)]() mutable
        {
            auto Result = Decode(Bytes);
            FString LipError;
            Result.Lip = DecodeT5FuzLip(Bytes, LipError);
            if (!LipError.IsEmpty()) UE_LOG(LogT5Audio, Warning, TEXT("%s"), *LipError);
            return Result;
        });
        State->Voices.Add(Voice.Request.Instance, MoveTemp(Voice));
    };
    double NextTrackAt = Now;
    for (auto& Request : Requests)
    {
        auto* Existing = State->Voices.Find(Request.Instance);
        if (Request.Command == FT5AudioRequest::Stop)
        {
            if (Existing)
            {
                if (Existing->Request.bLoop)
                    UE_LOG(LogT5Audio, Display, TEXT("T5_AUDIO_STOP instance=%d path=%s"), Request.Instance, *Existing->Request.Path);
                if (Existing->bStarted && (Request.Fade > 0 || (Request.bMusic && !Existing->Request.Finale.IsEmpty())))
                {
                    double At = Now;
                    if (Request.bMusic && !Existing->Request.Finale.IsEmpty())
                    {
                        const double TrackPosition = (Now - Existing->Start) * Existing->Request.Pitch;
                        if (const auto* Cue = Existing->Request.Cues.FindByPredicate([&](float Point) { return Point >= TrackPosition; }))
                            At = FMath::Min(Existing->Start + *Cue / Existing->Request.Pitch, Existing->End);
                    }
                    Existing->bStopping = true;
                    Existing->StopAt = At;
                    Existing->StopFade = Request.Fade;
                    Existing->DestroyAt = At + Request.Fade + .05;
                    if (Request.bMusic && !Existing->Request.Finale.IsEmpty())
                    {
                        FT5AudioRequest Finale{FT5AudioRequest::Play, State->NextFinale--, 0, Existing->Request.Finale};
                        Finale.bSpatial = false;
                        Finale.Gain = Existing->Request.Gain;
                        Queue(MoveTemp(Finale), At);
                        NextTrackAt = FMath::Max(NextTrackAt, At);
                    }
                    continue;
                }
                StopVoice(*Existing);
                State->Voices.Remove(Request.Instance);
            }
            continue;
        }
        if (Request.Command == FT5AudioRequest::Volume)
        {
            if (Existing)
            {
                Existing->Request.Gain = Request.Gain;
                if (auto* Audio = Existing->Audio.Get())
                {
                    if (Existing->Request.bMusic)
                        Audio->AdjustVolume(Request.Fade, Request.Gain);
                    else
                        Audio->SetVolumeMultiplier(Request.Gain);
                }
            }
            continue;
        }
        if (!Existing)
        {
            const double Due = (Request.bMusic ? NextTrackAt : Now) + Request.Delay;
            Queue(MoveTemp(Request), Due);
        }
    }

    for (auto It = State->Voices.CreateIterator(); It; ++It)
    {
        auto& Voice = It.Value();
        // Dialogue, queued responses and facial playback share the world simulation clock.
        // Streaming can suspend this clock while the audio device keeps processing.
        if (auto* Audio = Voice.Audio.Get()) Audio->SetPaused(bPaused);
        if (bPaused) continue;
        const auto Source = Position(Voice.Request.Source);
        if (!Voice.bStarted)
        {
            if (Now < Voice.Due || !Voice.Decode.IsReady())
                continue;
            auto Decoded = Voice.Decode.Get();
            if (Decoded.Pcm.IsEmpty() || (Voice.Request.bSpatial && !Source))
            {
                UE_LOG(LogT5Audio, Warning, TEXT("Cannot play %s from %08X: %s"), *Voice.Request.Path, Voice.Request.Source,
                    *Decoded.Error);
                if (Voice.Request.bMusic)
                    Papyrus.AudioFinished(Voice.Request.Instance, false);
                It.RemoveCurrent();
                continue;
            }
            Voice.Audio = PlayVoice(Owner, Voice, MoveTemp(Decoded), Source.Get(Owner.GetActorLocation()), Now);
            Voice.bStarted = true;
            continue;
        }
        auto* Audio = Voice.Audio.Get();
        if (Voice.bStopping && Audio && Now >= Voice.StopAt)
        {
            Audio->FadeOut(Voice.StopFade, 0);
            Voice.StopAt = TNumericLimits<double>::Max();
        }
        if (Voice.bStopping && Now >= Voice.DestroyAt)
        {
            StopVoice(Voice);
            It.RemoveCurrent();
            continue;
        }
        if (!Audio || (!Voice.Request.bLoop && Now > Voice.End + .15))
        {
            StopVoice(Voice);
            if (Voice.Request.bMusic && !Voice.bStopping)
                Papyrus.AudioFinished(Voice.Request.Instance, Audio != nullptr);
            It.RemoveCurrent();
            continue;
        }
        if (Source)
            Audio->SetWorldLocation(*Source);
        if (auto* Wave = Cast<USoundWaveProcedural>(Audio->GetSound());
            !Voice.bReportedFrames && Wave && Wave->GetAvailableAudioByteCount() < Voice.QueuedBytes)
        {
            Voice.bReportedFrames = true;
#if WITH_DEV_AUTOMATION_TESTS
            if (State->MixedPaths.Num() < 4096)
                State->MixedPaths.Add(Voice.Request.Path);
#endif
            UE_LOG(LogT5Audio, Display, TEXT("T5_AUDIO_MIX instance=%d path=%s"), Voice.Request.Instance, *Voice.Request.Path);
        }
    }
}

TPair<int32, float> FT5AudioRuntime::Expression(uint32 Actor) const
{
    const FVoice* Latest = nullptr;
    for (const auto& Entry : State->Voices)
        if (Entry.Value.bStarted && Entry.Value.Request.Source == Actor && Entry.Value.Request.Emotion >= 0
            && (!Latest || Latest->Start < Entry.Value.Start))
            Latest = &Entry.Value;
    return Latest ? TPair<int32, float>(Latest->Request.Emotion, Latest->Request.EmotionStrength)
        : TPair<int32, float>(-1, 0);
}

void FT5AudioRuntime::FacialWeights(uint32 Actor, double AudioTime, TArray<float>& Weights) const
{
    Weights.Init(0, T5LipMorphNames().Num());
    for (const auto& Entry : State->Voices)
    {
        const auto& Voice = Entry.Value;
        if (Voice.bStarted && Voice.Request.Source == Actor && Voice.Lip)
            for (int32 Channel = 0; Channel < Weights.Num(); ++Channel)
                Weights[Channel] = FMath::Max(Weights[Channel], Voice.Lip->Sample(Channel,
                    (AudioTime - Voice.Start) * Voice.Request.Pitch));
    }
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FT5AudioPauseTest, "T5Data.AudioSimulationClock",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FT5AudioPauseTest::RunTest(const FString&)
{
    UWorld* World = nullptr;
    for (const auto& Context : GEngine->GetWorldContexts())
        if (Context.World() && Context.World()->IsGameWorld()) { World = Context.World(); break; }
    if (!TestTrue(TEXT("An audio-enabled game world is available"),
        World && World->GetAudioDevice().IsValid() && GEngine->UseSound())) return false;
    auto* Owner = World->SpawnActor<AActor>();
    FT5PapyrusRuntime Papyrus;
    FT5AudioRuntime Runtime;
    FVoice Queued;
    Queued.Request = {FT5AudioRequest::Play, 1};
    Queued.Request.bSpatial = false;
    Queued.Due = 5;
    TPromise<FT5DecodedAudio> Promise;
    Queued.Decode = Promise.GetFuture();
    FT5DecodedAudio Silent;
    Silent.SampleRate = 22050;
    Silent.Channels = 1;
    Silent.Pcm.SetNumZeroed(22050 * sizeof(int16) * 2);
    Promise.SetValue(MoveTemp(Silent));
    Runtime.State->Voices.Add(1, MoveTemp(Queued));
    const auto Position = [](uint32) -> TOptional<FVector> { return FVector::ZeroVector; };
    Runtime.Tick(*Owner, Papyrus, Position, 4, false);
    TestFalse(TEXT("Queued response respects its simulation deadline"), Runtime.State->Voices[1].bStarted);
    Runtime.Tick(*Owner, Papyrus, Position, 5, true);
    TestFalse(TEXT("A ready response does not start during a streaming pause"), Runtime.State->Voices[1].bStarted);
    Runtime.Tick(*Owner, Papyrus, Position, 5, false);
    auto& Voice = Runtime.State->Voices[1];
    TestTrue(TEXT("Resume starts the ready response"), Voice.bStarted && Voice.Audio.IsValid());
    TestEqual(TEXT("Voice timestamps use simulation time, not world audio time"), Voice.Start, 5.0);
    Runtime.Tick(*Owner, Papyrus, Position, 6, true);
    TestTrue(TEXT("An active audio component pauses with the simulation"), Voice.Audio.IsValid() && Voice.Audio->bIsPaused);
    Runtime.Tick(*Owner, Papyrus, Position, 6, false);
    TestTrue(TEXT("Resume keeps the same audio instance and cursor"), Voice.Audio.IsValid() && !Voice.Audio->bIsPaused && Voice.Start == 5.0);
    Runtime.Tick(*Owner, Papyrus, Position, 8, false);
    TestTrue(TEXT("Completion follows the simulation deadline"), Runtime.State->Voices.IsEmpty());
    Runtime.Reset();
    Owner->Destroy();
    return true;
}
#endif

void SerializeT5AudioRequest(FArchive& Ar, FT5AudioRequest& R)
{
    int32 Command = int32(R.Command); Ar << Command; R.Command = FT5AudioRequest::ECommand(Command);
    Ar << R.Instance << R.Source << R.Path << R.Delay << R.Gain << R.Pitch << R.MinDistance << R.MaxDistance
        << R.bSpatial << R.bLoop << R.Fade << R.LoopBegin << R.LoopEnd << R.LoopCount << R.bMusic << R.Finale
        << R.Cues << R.Emotion << R.EmotionStrength;
}

void FT5AudioRuntime::SerializeCheckpoint(FArchive& Ar, FT5PapyrusRuntime& Papyrus, double Now)
{
    if (Ar.IsLoading()) { Reset(); State->Revision = Papyrus.Revision(); }
    Ar << State->NextFinale;
    int32 Count = State->Voices.Num(); Ar << Count;
    if (Count < 0 || Count > MaxActiveSounds) { Ar.SetError(); return; }
    const auto VoiceState = [&](FVoice& Voice) {
        SerializeT5AudioRequest(Ar, Voice.Request);
        Ar << Voice.Due << Voice.StopAt << Voice.DestroyAt << Voice.StopFade << Voice.bStopping;
        double Elapsed = Voice.bStarted ? FMath::Max(0., (Now - Voice.Start) * Voice.Request.Pitch) : Voice.ResumeSeconds;
        Ar << Elapsed;
        if (Ar.IsLoading())
        {
            Voice.ResumeSeconds = Elapsed;
            TArray<uint8> Bytes;
            if (!Papyrus.ReadResource(Voice.Request.Path, Bytes)) { Ar.SetError(); return; }
            Voice.Decode = Async(EAsyncExecution::ThreadPool, [Decode = Papyrus.AudioDecoder(), Bytes = MoveTemp(Bytes)]() mutable {
                auto Result = Decode(Bytes); FString Error; Result.Lip = DecodeT5FuzLip(Bytes, Error); return Result;
            });
        }
    };
    if (Ar.IsLoading())
        for (int32 I = 0; I < Count && !Ar.IsError(); ++I)
        { FVoice Voice; VoiceState(Voice); State->Voices.Add(Voice.Request.Instance, MoveTemp(Voice)); }
    else for (auto& [Id, Voice] : State->Voices) VoiceState(Voice);
}
