#pragma once

#include "CoreMinimal.h"

struct FT5FontPoint
{
    int16 X, Y;
    bool bOnCurve;
};

struct FT5FontGlyph
{
    uint16 Code = 0;
    uint16 Advance = 0;
    TArray<FT5FontPoint> Points;
    TArray<uint16> ContourEnds;
};

struct FT5FontKerning
{
    uint16 Left, Right;
    int16 Adjustment;
};

struct FT5FontOutline
{
    FString Name;
    uint8 Style = 0; // DefineFont3 bold and italic flags.
    int16 Ascent = 0, Descent = 0, Leading = 0;
    TArray<FT5FontGlyph> Glyphs;
    TArray<FT5FontKerning> Kerning;
};

struct FT5UIFontSet
{
    TMap<FName, FName> Roles;
    TMap<FName, TArray<uint8>> Faces;
};

inline FName T5FontKey(const FString& Face, uint8 Style)
{
    return FName(FString::Printf(TEXT("%s#%u"), *Face, Style));
}

TArray<uint8> MakeT5TrueType(const FT5FontOutline& Font);
void ImportT5UIFonts(TConstArrayView<uint8> Swf, FT5UIFontSet& Fonts);
