#pragma once

#include "T5Hkx.h"

struct FT5HkxCameraClip : FT5HkxPlaybackClock
{
    int32 Camera = INDEX_NONE;
};

bool LoadT5HkxPlayerCamera(const FString& Behavior, const FString& Event,
    TFunctionRef<bool(const FString&, TArray<uint8>&)> Read, FT5HkxCameraClip& Out, FString& Error, const FString& DirectClip = {});
FTransform SampleT5HkxPlayerCamera(const FT5HkxCameraClip& Camera, float Elapsed);
void DispatchT5HkxCameraEvents(FT5HkxCameraClip& Camera, float Elapsed, TFunctionRef<void(const FString&)> Dispatch);
