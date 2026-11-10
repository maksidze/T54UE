#pragma once
#include "T5Hkx.h"

struct FT5ReferenceAnimation
{
    uint64 Serial = 0;
    uint32 Parent = 0;
    double Start = 0;
    FTransform Anchor, Pose;
    FT5HkxPlaybackClock Clock;
    bool bLoaded = false, bMoving = false, bFinished = false;
};
