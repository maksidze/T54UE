#pragma once
#include "CoreMinimal.h"

// Skyrim uses vertex A for wind/LOD data in these shader permutations.
// Treating it as opacity removes most pine needles (many vertices have A=0).
// Texture alpha and NiAlphaProperty's threshold still provide the cutout.
inline bool T5UsesVertexOpacity(uint32 Flags1, uint32 Flags2, int32 Shader)
{
    const bool bWind = (Flags2 & 0x20000000u) != 0 || Shader == 12;
    const bool bObjectLod = Shader == 13 || Shader == 15;
    return (Flags1 & 8u) != 0 && !bWind && !bObjectLod;
}
