#include "T5UIFont.h"

namespace
{
    constexpr uint16 UnitsPerEm = 1024;

    struct FFontBytes : TArray<uint8>
    {
        void U16(uint16 Value)
        {
            Add(Value >> 8);
            Add(Value & 255);
        }

        void U32(uint32 Value)
        {
            U16(Value >> 16);
            U16(Value & 65535);
        }

        void Words(std::initializer_list<int32> Values)
        {
            for (const int32 Value : Values)
                U16(uint16(Value));
        }

        void Pad(int32 Alignment)
        {
            AddZeroed((Alignment - Num() % Alignment) % Alignment);
        }

        uint32 Checksum() const
        {
            uint32 Sum = 0;
            for (int32 Index = 0; Index < Num(); ++Index)
                Sum += uint32((*this)[Index]) << (8 * (3 - Index % 4));
            return Sum;
        }
    };

    struct FFontBounds
    {
        int16 MinX = 0, MinY = 0, MaxX = 0, MaxY = 0;

        void Include(int16 X, int16 Y)
        {
            MinX = FMath::Min(MinX, X);
            MinY = FMath::Min(MinY, Y);
            MaxX = FMath::Max(MaxX, X);
            MaxY = FMath::Max(MaxY, Y);
        }

        void Write(FFontBytes& Bytes) const
        {
            Bytes.Words({MinX, MinY, MaxX, MaxY});
        }
    };

    void WriteGlyph(const FT5FontGlyph& Glyph, const FFontBounds& Box, FFontBytes& Data)
    {
        if (Glyph.Points.IsEmpty())
            return;
        Data.U16(Glyph.ContourEnds.Num());
        Box.Write(Data);
        for (const uint16 End : Glyph.ContourEnds)
            Data.U16(End);
        Data.U16(0); // No TrueType instructions. Flash grid fitting is not encoded as bytecode.
        for (const auto& Point : Glyph.Points)
            Data.Add(Point.bOnCurve ? 1 : 0);
        int16 Previous = 0;
        for (const auto& Point : Glyph.Points)
        {
            Data.U16(Point.X - Previous);
            Previous = Point.X;
        }
        Previous = 0;
        for (const auto& Point : Glyph.Points)
        {
            Data.U16(Point.Y - Previous);
            Previous = Point.Y;
        }
        Data.Pad(2);
    }

    void WriteCmap(TMap<uint16, uint16>& Codes, FFontBytes& Data)
    {
        struct FGroup
        {
            uint32 First, Last, Glyph;
        };
        TArray<FGroup> Groups;
        Codes.KeySort(TLess<uint16>());
        for (const auto& [Code, Id] : Codes)
        {
            if (!Groups.IsEmpty() && Groups.Last().Last + 1 == Code
                && Groups.Last().Glyph + Code - Groups.Last().First == Id)
                Groups.Last().Last = Code;
            else
                Groups.Add({Code, Code, Id});
        }
        Data.Words({0, 1, 3, 10}); // Windows Unicode, full repertoire.
        Data.U32(12);
        Data.Words({12, 0});
        Data.U32(16 + Groups.Num() * 12);
        Data.U32(0); // Language.
        Data.U32(Groups.Num());
        for (const auto& Group : Groups)
        {
            Data.U32(Group.First);
            Data.U32(Group.Last);
            Data.U32(Group.Glyph);
        }
    }

    void WriteName(const FT5FontOutline& Font, FFontBytes& Data)
    {
        const TCHAR* Styles[] = {TEXT("Regular"), TEXT("Bold"), TEXT("Italic"), TEXT("Bold Italic")};
        const FString Style = Styles[Font.Style];
        const FString FullName = Font.Style ? Font.Name + TEXT(" ") + Style : Font.Name;
        const FString PostScript = FullName.Replace(TEXT(" "), TEXT(""));
        const FString Names[] = {Font.Name, Style, PostScript, FullName, PostScript};
        const uint16 Ids[] = {1, 2, 3, 4, 6};
        Data.Words({0, UE_ARRAY_COUNT(Names), 6 + UE_ARRAY_COUNT(Names) * 12});
        uint16 Offset = 0;
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(Names); ++Index)
        {
            const uint16 Length = Names[Index].Len() * 2;
            Data.Words({3, 1, 0x409, Ids[Index], Length, Offset});
            Offset += Length;
        }
        for (const FString& Name : Names)
            for (const TCHAR Character : Name)
                Data.U16(Character);
    }

    void WriteKerning(const FT5FontOutline& Font, const TMap<uint16, uint16>& Codes, FFontBytes& Data)
    {
        TMap<uint32, int16> Pairs;
        for (const FT5FontKerning& Pair : Font.Kerning)
        {
            const uint16 Left = Codes.FindRef(Pair.Left), Right = Codes.FindRef(Pair.Right);
            if (Left && Right)
                Pairs.Add((uint32(Left) << 16) | Right, Pair.Adjustment);
        }
        if (Pairs.IsEmpty() || Pairs.Num() > (65535 - 14) / 6)
            return;
        Pairs.KeySort(TLess<uint32>());
        const int32 Count = Pairs.Num(), Log = FMath::FloorLog2(Count);
        const int32 SearchRange = 6 << Log;
        Data.Words({0, 1, 0, 14 + Count * 6, 1, Count, SearchRange, Log, Count * 6 - SearchRange});
        for (const auto& [Pair, Adjustment] : Pairs)
        {
            Data.U32(Pair);
            Data.U16(Adjustment);
        }
    }

    bool IsSpacingCode(uint16 Code)
    {
        return Code <= 0x20 || Code == 0xA0 || Code == 0xAD || Code == 0x1680
            || (Code >= 0x2000 && Code <= 0x200F) || Code == 0x202F
            || Code == 0x205F || Code == 0x2060 || Code == 0x3000 || Code == 0xFEFF;
    }
}

TArray<uint8> MakeT5TrueType(const FT5FontOutline& Font)
{
    TMap<FString, FFontBytes> Tables;
    Tables.Reserve(11);
    for (const TCHAR* Name : {TEXT("head"), TEXT("hhea"), TEXT("maxp"), TEXT("OS/2"), TEXT("hmtx"),
                             TEXT("cmap"), TEXT("loca"), TEXT("glyf"), TEXT("name"), TEXT("post"), TEXT("kern")})
        Tables.Add(Name);
    auto& Loca = Tables.FindChecked(TEXT("loca"));
    auto& Glyf = Tables.FindChecked(TEXT("glyf"));
    auto& Metrics = Tables.FindChecked(TEXT("hmtx"));
    TMap<uint16, uint16> Codes;
    FFontBounds Bounds;
    int32 AdvanceMax = 0, AdvanceSum = 0, RightMin = 32767;
    int32 MaxPoints = 0, MaxContours = 0;
    Loca.U32(0); // Reserved empty .notdef glyph.
    Metrics.Words({UnitsPerEm / 2, 0});
    for (const FT5FontGlyph& Glyph : Font.Glyphs)
    {
        // Some localized movies have empty placeholder outlines for letters they do not cover.
        if (Glyph.Code == 0xFFFF || Codes.Contains(Glyph.Code)
            || (Glyph.Points.IsEmpty() && !IsSpacingCode(Glyph.Code)))
            continue;
        Codes.Add(Glyph.Code, Codes.Num() + 1);
        FFontBounds Box;
        if (!Glyph.Points.IsEmpty())
        {
            const auto& First = Glyph.Points[0];
            Box = {First.X, First.Y, First.X, First.Y};
            for (const auto& Point : Glyph.Points)
                Box.Include(Point.X, Point.Y);
            Bounds.Include(Box.MinX, Box.MinY);
            Bounds.Include(Box.MaxX, Box.MaxY);
        }
        AdvanceMax = FMath::Max(AdvanceMax, int32(Glyph.Advance));
        AdvanceSum += Glyph.Advance;
        RightMin = FMath::Min(RightMin, Glyph.Advance - Box.MaxX);
        MaxPoints = FMath::Max(MaxPoints, Glyph.Points.Num());
        MaxContours = FMath::Max(MaxContours, Glyph.ContourEnds.Num());
        Loca.U32(Glyf.Num());
        Metrics.Words({Glyph.Advance, Box.MinX});
        WriteGlyph(Glyph, Box, Glyf);
    }
    Loca.U32(Glyf.Num());
    if (Codes.IsEmpty() || Glyf.IsEmpty())
        return {};
    const int32 GlyphCount = Codes.Num() + 1;

    auto& Head = Tables.FindChecked(TEXT("head"));
    Head.U32(0x00010000);
    Head.U32(0x00010000);
    Head.U32(0); // Checksum adjustment, filled after assembling the sfnt.
    Head.U32(0x5F0F3CF5);
    Head.Words({3, UnitsPerEm});
    for (int32 Index = 0; Index < 2; ++Index)
    {
        Head.U32(0);
        Head.U32(2082844800u); // Fixed 1970-01-01 timestamp in the TrueType epoch.
    }
    Bounds.Write(Head);
    Head.Words({Font.Style, 8, 2, 1, 0}); // Long loca offsets.

    auto& Hhea = Tables.FindChecked(TEXT("hhea"));
    Hhea.U32(0x00010000);
    Hhea.Words({Font.Ascent, Font.Descent, Font.Leading, AdvanceMax, Bounds.MinX, RightMin, Bounds.MaxX,
        1, 0, 0, 0, 0, 0, 0, 0, GlyphCount});

    auto& Maxp = Tables.FindChecked(TEXT("maxp"));
    Maxp.U32(0x00010000);
    Maxp.Words({GlyphCount, MaxPoints, MaxContours, 0, 0, 1});
    Maxp.AddZeroed(16);

    auto& OS2 = Tables.FindChecked(TEXT("OS/2"));
    OS2.Words({0, AdvanceSum / Codes.Num(), (Font.Style & 1) ? 700 : 400, 5, 0});
    OS2.AddZeroed(22); // Subscript, superscript, strikeout and family class.
    OS2.AddZeroed(10); // PANOSE.
    uint32 UnicodeRange1 = 0, UnicodeRange2 = 0;
    uint16 FirstCode = 0xFFFF, LastCode = 0;
    for (const auto& [Code, Glyph] : Codes)
    {
        FirstCode = FMath::Min(FirstCode, Code);
        LastCode = FMath::Max(LastCode, Code);
        if (Code >= 0x20 && Code <= 0x7F) UnicodeRange1 |= 1u << 0;
        if (Code >= 0xA0 && Code <= 0xFF) UnicodeRange1 |= 1u << 1;
        if (Code >= 0x100 && Code <= 0x17F) UnicodeRange1 |= 1u << 2;
        if (Code >= 0x400 && Code <= 0x4FF) UnicodeRange1 |= 1u << 9;
        if (Code >= 0x3040 && Code <= 0x309F) UnicodeRange2 |= 1u << (49 - 32);
        if (Code >= 0x30A0 && Code <= 0x30FF) UnicodeRange2 |= 1u << (50 - 32);
        if (Code >= 0x4E00 && Code <= 0x9FFF) UnicodeRange2 |= 1u << (59 - 32);
    }
    OS2.U32(UnicodeRange1);
    OS2.U32(UnicodeRange2);
    OS2.AddZeroed(8);
    OS2.Append({uint8(' '), uint8(' '), uint8(' '), uint8(' ')});
    const int32 Selection = Font.Style ? ((Font.Style & 1) << 5) | ((Font.Style & 2) >> 1) : 0x40;
    OS2.Words({Selection, FirstCode, LastCode, Font.Ascent, Font.Descent, Font.Leading, Bounds.MaxY, -Bounds.MinY});

    auto& Post = Tables.FindChecked(TEXT("post"));
    Post.U32(0x00030000); // No PostScript glyph names.
    Post.AddZeroed(28);
    WriteName(Font, Tables.FindChecked(TEXT("name")));
    WriteKerning(Font, Codes, Tables.FindChecked(TEXT("kern")));
    if (Tables.FindChecked(TEXT("kern")).IsEmpty())
        Tables.Remove(TEXT("kern"));
    WriteCmap(Codes, Tables.FindChecked(TEXT("cmap")));

    Tables.KeySort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
    FFontBytes Result;
    const int32 Count = Tables.Num(), Log = FMath::FloorLog2(Count), SearchRange = 16 << Log;
    Result.U32(0x00010000);
    Result.Words({Count, SearchRange, Log, Count * 16 - SearchRange});
    int32 Offset = 12 + Count * 16;
    int32 ChecksumOffset = 0;
    for (const auto& [Name, Data] : Tables)
    {
        for (const TCHAR Character : Name)
            Result.Add(Character);
        Result.U32(Data.Checksum());
        Result.U32(Offset);
        Result.U32(Data.Num());
        if (Name == TEXT("head"))
            ChecksumOffset = Offset + 8;
        Offset += Align(Data.Num(), 4);
    }
    for (const auto& [Name, Data] : Tables)
    {
        Result.Append(Data);
        Result.Pad(4);
    }
    const uint32 Adjustment = 0xB1B0AFBA - Result.Checksum();
    for (int32 Index = 0; Index < 4; ++Index)
        Result[ChecksumOffset + Index] = Adjustment >> (8 * (3 - Index));
    return Result;
}
