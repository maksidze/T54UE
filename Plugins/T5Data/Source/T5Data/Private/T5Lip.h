#pragma once

#include "CoreMinimal.h"

// T5 LIP v1: 12-byte container, byte-level zero-run compression, then
// frame count + first frame and 33 float channels per 30 Hz sample.
// Channels are 16 phonemes followed by 17 facial modifiers. The last three
// modifiers describe head angles; they are retained but not applied as morphs.
struct FT5LipTrack
{
    int32 FirstFrame = 0;
    TArray<float> Samples;
    float Sample(int32 Channel, double Seconds) const;
};

TSharedPtr<FT5LipTrack> DecodeT5FuzLip(TConstArrayView<uint8> Fuz, FString& Error);
const TArray<FName>& T5LipMorphNames();
