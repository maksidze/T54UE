#pragma once

#include "CoreMinimal.h"

class AActor;
class FT5PapyrusRuntime;

struct FT5AudioRequest
{
    enum ECommand { Play, Stop, Volume };
    ECommand Command;
    int32 Instance;
    uint32 Source = 0;
    FString Path;
    double Delay = 0;
    float Gain = 1, Pitch = 1;
    float MinDistance = 0, MaxDistance = 0;
    bool bSpatial = true, bLoop = false;
    float Fade = 0, LoopBegin = 0, LoopEnd = 0;
    uint32 LoopCount = 0;
    bool bMusic = false;
    FString Finale;
    TArray<float> Cues;
    int32 Emotion = -1;
    float EmotionStrength = 0;
};

void SerializeT5AudioRequest(FArchive& Ar, FT5AudioRequest& Request);

struct FT5DecodedAudio
{
    uint32 SampleRate = 0, Channels = 0;
    uint32 LoopStart = 0, LoopEnd = 0;
    TArray<uint8> Pcm;
    FString Error;
    TSharedPtr<struct FT5LipTrack> Lip;
};

using FT5AudioDecoder = TFunction<FT5DecodedAudio(TConstArrayView<uint8>)>;

class FT5AudioRuntime
{
public:
    FT5AudioRuntime();
    ~FT5AudioRuntime();

    void Tick(AActor& Owner, FT5PapyrusRuntime& Papyrus, TFunctionRef<TOptional<FVector>(uint32)> Position,
        double SimulationTime, bool bPaused);
    void Reset();
    void SerializeCheckpoint(FArchive& Ar, FT5PapyrusRuntime& Papyrus, double Now);
    TPair<int32, float> Expression(uint32 Actor) const;
    void FacialWeights(uint32 Actor, double AudioTime, TArray<float>& Weights) const;
#if WITH_DEV_AUTOMATION_TESTS
    bool HasMixed(const TCHAR* Suffix) const;
#endif

private:
#if WITH_DEV_AUTOMATION_TESTS
    friend class FT5AudioPauseTest;
#endif
    struct FState;
    TUniquePtr<FState> State;
};
