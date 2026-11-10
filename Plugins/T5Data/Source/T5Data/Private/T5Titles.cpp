#include "T5Titles.h"

#include "T5Bridge.h"
#include "T5UIFont.h"
#include "T5UIResources.h"
#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"
#include "UObject/StrongObjectPtr.h"

THIRD_PARTY_INCLUDES_START
#include "zlib.h"
THIRD_PARTY_INCLUDES_END

DEFINE_LOG_CATEGORY_STATIC(LogT5Titles, Log, All);

namespace
{
    // The shipped title movie is a SWF timeline of vector shapes and sprites.
    // This reader plays that subset directly from the archive, including alpha keyframes.
    struct FBits
    {
        TConstArrayView<uint8> Bytes;
        int32 Cursor = 0;
        bool bValid = true;

        uint32 Read(int32 Count)
        {
            if (Count < 0 || Count > 32 || Count > Bytes.Num() * 8 - Cursor)
            {
                bValid = false;
                return 0;
            }
            uint32 Value = 0;
            while (Count--)
            {
                Value = (Value << 1) | ((Bytes[Cursor / 8] >> (7 - Cursor % 8)) & 1);
                ++Cursor;
            }
            return Value;
        }

        int32 Signed(int32 Count)
        {
            const uint32 Value = Read(Count);
            if (Count <= 0 || Count >= 32)
                return static_cast<int32>(Value);
            const uint32 Sign = 1u << (Count - 1);
            return static_cast<int32>((Value ^ Sign) - Sign);
        }

        void Align() { Cursor = (Cursor + 7) & ~7; }
        uint32 Byte() { return Read(8); }
        uint32 Word()
        {
            const uint32 Low = Byte();
            return Low | (Byte() << 8);
        }
        uint32 DWord()
        {
            const uint32 Low = Word();
            return Low | (Word() << 16);
        }
        FString String()
        {
            FString Result;
            while (bValid)
            {
                const auto C = Byte();
                if (!C)
                    break;
                Result.AppendChar(static_cast<TCHAR>(C));
            }
            return Result;
        }
        FBox2f Rect()
        {
            const int32 Width = Read(5);
            const float X0 = Signed(Width), X1 = Signed(Width), Y0 = Signed(Width), Y1 = Signed(Width);
            Align();
            return {{X0, Y0}, {X1, Y1}};
        }
        TConstArrayView<uint8> Payload(uint32 Length)
        {
            if (Length > static_cast<uint32>(Bytes.Num() - Cursor / 8))
            {
                bValid = false;
                return {};
            }
            const auto Data = Bytes.Slice(Cursor / 8, Length);
            Cursor += Length * 8;
            return Data;
        }
    };

    struct FMatrix2
    {
        FVector2f X{1, 0}, Y{0, 1}, T{0, 0};

        FVector2f Point(FVector2f P) const { return Vector(P) + T; }
        FVector2f Vector(FVector2f P) const { return X * P.X + Y * P.Y; }
        FMatrix2 Child(const FMatrix2& Other) const { return {Vector(Other.X), Vector(Other.Y), Point(Other.T)}; }

        static FMatrix2 Read(FBits& Bits)
        {
            FMatrix2 Out;
            if (Bits.Read(1))
            {
                const int32 N = Bits.Read(5);
                Out.X.X = Bits.Signed(N) / 65536.f;
                Out.Y.Y = Bits.Signed(N) / 65536.f;
            }
            if (Bits.Read(1))
            {
                const int32 N = Bits.Read(5);
                Out.X.Y = Bits.Signed(N) / 65536.f;
                Out.Y.X = Bits.Signed(N) / 65536.f;
            }
            const int32 N = Bits.Read(5);
            Out.T.X = Bits.Signed(N);
            Out.T.Y = Bits.Signed(N);
            Bits.Align();
            return Out;
        }
    };

    struct FPlaced
    {
        int32 Character = 0, First = 0;
        FMatrix2 Matrix;
        float Alpha = 1;
        FString Name;
    };
    using FTitleFrame = TArray<FPlaced>;

    struct FShape
    {
        FBox2f Bounds;
        TStrongObjectPtr<UTexture2D> Texture;
        FSlateBrush Brush;
    };

    struct FFill
    {
        struct FStop { float Ratio; FColor Color; };
        FMatrix2 Matrix;
        FColor Solid = FColor::Transparent;
        TArray<FStop> Stops;
        uint32 Type = 0, Spread = 0;
        bool bLinear = false;
        float Focus = 0;

        FColor Sample(FVector2f Point) const
        {
            if (Stops.IsEmpty())
                return Solid;
            const float Det = Matrix.X.X * Matrix.Y.Y - Matrix.Y.X * Matrix.X.Y;
            if (FMath::IsNearlyZero(Det))
                return Stops[0].Color;
            const auto D = (Point - Matrix.T) / (Det * 16384.f);
            const FVector2f U(D.X * Matrix.Y.Y - D.Y * Matrix.Y.X, -D.X * Matrix.X.Y + D.Y * Matrix.X.X);
            float Ratio = Type == 16 ? (U.X + 1.f) * .5f : U.Length();
            if (Type == 19)
            {
                const auto FromFocus = U - FVector2f(Focus, 0);
                const float D2 = FromFocus.SquaredLength(), B = Focus * FromFocus.X;
                const float Root = FMath::Sqrt(B * B + D2 * (1.f - Focus * Focus));
                Ratio = (Root + B) > UE_SMALL_NUMBER ? D2 / (Root - B) : 0.f;
            }
            if (Spread == 1)
                Ratio = 1.f - FMath::Abs((Ratio - 2.f * FMath::FloorToFloat(Ratio * .5f)) - 1.f);
            else if (Spread == 2)
                Ratio -= FMath::FloorToFloat(Ratio);
            for (int32 I = 0; I < Stops.Num(); ++I)
                if (Ratio <= Stops[I].Ratio)
                {
                    if (!I || Stops[I].Ratio == Stops[I - 1].Ratio)
                        return Stops[I].Color;
                    const float T = (Ratio - Stops[I - 1].Ratio) / (Stops[I].Ratio - Stops[I - 1].Ratio);
                    const auto A = Stops[I - 1].Color, B = Stops[I].Color;
                    if (bLinear)
                        return FMath::Lerp(FLinearColor(A), FLinearColor(B), T).ToFColorSRGB();
                    return {static_cast<uint8>(FMath::Lerp(float(A.R), float(B.R), T)),
                        static_cast<uint8>(FMath::Lerp(float(A.G), float(B.G), T)),
                        static_cast<uint8>(FMath::Lerp(float(A.B), float(B.B), T)),
                        static_cast<uint8>(FMath::Lerp(float(A.A), float(B.A), T))};
                }
            return Stops.Last().Color;
        }
    };

    bool Styles(FBits& Bits, bool bAlpha, TArray<FFill>& Fills, int32& Base)
    {
        Base = Fills.Num() - 1;
        int32 Count = Bits.Byte();
        if (Count == 255 && bAlpha)
            Count = Bits.Word();
        const auto ReadColor = [&]()
        {
            const uint8 R = Bits.Byte(), G = Bits.Byte(), B = Bits.Byte(), A = bAlpha ? Bits.Byte() : 255;
            return FColor(R, G, B, A);
        };
        for (int32 I = 0; I < Count && Bits.bValid; ++I)
        {
            auto& Fill = Fills.AddDefaulted_GetRef();
            Fill.Type = Bits.Byte();
            if (!Fill.Type)
            {
                Fill.Solid = ReadColor();
                continue;
            }
            if (Fill.Type != 16 && Fill.Type != 18 && Fill.Type != 19)
                return false;
            Fill.Matrix = FMatrix2::Read(Bits);
            const uint32 Flags = Bits.Byte();
            Fill.Spread = Flags >> 6;
            Fill.bLinear = ((Flags >> 4) & 3) == 1;
            for (uint32 J = 0; J < (Flags & 15) && Bits.bValid; ++J)
            {
                const float Ratio = Bits.Byte() / 255.f;
                Fill.Stops.Add({Ratio, ReadColor()});
            }
            if (Fill.Type == 19)
                Fill.Focus = FMath::Clamp(static_cast<int16>(Bits.Word()) / 256.f, -.999f, .999f);
        }
        Count = Bits.Byte();
        if (Count == 255 && bAlpha)
            Count = Bits.Word();
        for (int32 I = 0; I < Count && Bits.bValid; ++I)
        {
            Bits.Word();
            Bits.Read(bAlpha ? 32 : 24);
        }
        return Bits.bValid;
    }

    bool RasterizeShape(FBits& Bits, bool bAlpha, FBox2f& Bounds, FT5UIBitmap& Out, int32 MaxExtent = 2048)
    {
        Bounds = Bits.Rect();
        TArray<FFill> Fills;
        Fills.AddDefaulted();
        int32 Base = 0;
        if (!Styles(Bits, bAlpha, Fills, Base))
            return false;
        int32 FillBits = Bits.Read(4), LineBits = Bits.Read(4);
        struct FEdge { FVector2f A, B; int32 Fill; };
        TArray<FEdge> Edges;
        FVector2f Pen(0, 0);
        int32 Left = 0, Right = 0;
        const auto Edge = [&](FVector2f A, FVector2f B)
        {
            if (A.Y == B.Y)
                return;
            if (Right)
                Edges.Add({A, B, Right});
            if (Left)
                Edges.Add({B, A, Left});
        };
        while (Bits.bValid && Edges.Num() < 100000)
        {
            if (!Bits.Read(1))
            {
                const uint32 Flags = Bits.Read(5);
                if (!Flags)
                    break;
                if (Flags & 1)
                {
                    const int32 N = Bits.Read(5);
                    Pen.X = Bits.Signed(N);
                    Pen.Y = Bits.Signed(N);
                }
                if (Flags & 2)
                {
                    Left = Bits.Read(FillBits);
                    if (Left)
                        Left += Base;
                }
                if (Flags & 4)
                {
                    Right = Bits.Read(FillBits);
                    if (Right)
                        Right += Base;
                }
                if (Flags & 8)
                    Bits.Read(LineBits);
                if (Flags & 16)
                {
                    Bits.Align();
                    if (!Styles(Bits, bAlpha, Fills, Base))
                        return false;
                    FillBits = Bits.Read(4);
                    LineBits = Bits.Read(4);
                }
                continue;
            }
            const bool bStraight = Bits.Read(1) != 0;
            const int32 N = Bits.Read(4) + 2;
            const auto Pair = [&]()
            {
                const float X = Bits.Signed(N), Y = Bits.Signed(N);
                return FVector2f(X, Y);
            };
            if (bStraight)
            {
                FVector2f Next = Pen;
                if (Bits.Read(1))
                    Next += Pair();
                else if (Bits.Read(1))
                    Next.Y += Bits.Signed(N);
                else
                    Next.X += Bits.Signed(N);
                Edge(Pen, Next);
                Pen = Next;
            }
            else
            {
                const auto Control = Pen + Pair();
                const auto Next = Control + Pair();
                const int32 Steps = FMath::Clamp(FMath::CeilToInt(FMath::Sqrt((Pen - Control * 2 + Next).Length() / 8)), 2, 32);
                auto Point = Pen;
                for (int32 I = 1; I <= Steps; ++I)
                {
                    const float T = float(I) / Steps;
                    const auto Sample = FMath::Lerp(FMath::Lerp(Pen, Control, T), FMath::Lerp(Control, Next, T), T);
                    Edge(Point, Sample);
                    Point = Sample;
                }
                Pen = Next;
            }
        }
        if (!Bits.bValid || Edges.IsEmpty() || Edges.Num() >= 100000)
            return false;
        const float Texel = FMath::Max(10.f, Bounds.GetSize().GetMax() / (MaxExtent - 2));
        Bounds.Min -= FVector2f(Texel);
        const auto Size = Bounds.GetSize();
        const int32 W = FMath::CeilToInt(Size.X / Texel) + 1, H = FMath::CeilToInt(Size.Y / Texel) + 1;
        if (W <= 1 || H <= 1 || W > 2048 || H > 2048 || Fills.Num() > 256)
            return false;
        Bounds.Max = Bounds.Min + FVector2f(W * Texel, H * Texel);
        Out.Size = {W, H};
        auto& Pixels = Out.Pixels;
        Pixels.Init(FColor::Transparent, W * H);
        struct FCross { float X; int32 Style, Winding; };
        TArray<FCross> Crossings;
        TArray<int32> Winding;
        Winding.SetNumZeroed(Fills.Num());
        for (int32 Y = 0; Y < H; ++Y)
        {
            const float Row = Bounds.Min.Y + (Y + .5f) * Texel;
            Crossings.Reset();
            for (const auto& E : Edges)
                if (Fills.IsValidIndex(E.Fill) && Row >= FMath::Min(E.A.Y, E.B.Y) && Row < FMath::Max(E.A.Y, E.B.Y))
                    Crossings.Add({FMath::Lerp(E.A.X, E.B.X, (Row - E.A.Y) / (E.B.Y - E.A.Y)), E.Fill, E.B.Y > E.A.Y ? 1 : -1});
            Crossings.Sort([](const auto& A, const auto& B) { return A.X < B.X; });
            FMemory::Memzero(Winding.GetData(), Winding.Num() * sizeof(int32));
            int32 Cross = 0;
            for (int32 X = 0; X < W; ++X)
            {
                const float Column = Bounds.Min.X + (X + .5f) * Texel;
                while (Crossings.IsValidIndex(Cross) && Crossings[Cross].X <= Column)
                {
                    const auto& Item = Crossings[Cross++];
                    Winding[Item.Style] += Item.Winding;
                }
                for (int32 I = Fills.Num() - 1; I > 0; --I)
                    if (Winding[I] & 1)
                    {
                        Pixels[Y * W + X] = Fills[I].Sample({Column, Row});
                        break;
                    }
            }
        }
        return true;
    }

    bool Shape(FBits& Bits, bool bAlpha, FShape& Out)
    {
        FT5UIBitmap Bitmap;
        if (!RasterizeShape(Bits, bAlpha, Out.Bounds, Bitmap))
            return false;
        UTexture2D* Texture = UTexture2D::CreateTransient(Bitmap.Size.X, Bitmap.Size.Y, PF_B8G8R8A8, NAME_None,
            MakeArrayView(reinterpret_cast<const uint8*>(Bitmap.Pixels.GetData()), Bitmap.Pixels.Num() * sizeof(FColor)));
        if (!Texture)
            return false;
        Texture->SRGB = true;
        Texture->NeverStream = true;
        Texture->UpdateResource();
        Out.Texture.Reset(Texture);
        Out.Brush.SetResourceObject(Texture);
        Out.Brush.ImageSize = FVector2D(Bitmap.Size);
        return true;
    }

    bool Place(FBits& Bits, bool bExtended, int32 Frame, TMap<int32, FPlaced>& Display)
    {
        const uint32 Flags = Bits.Byte(), Extra = bExtended ? Bits.Byte() : 0;
        const int32 Depth = Bits.Word();
        auto& Item = Display.FindOrAdd(Depth);
        if (!(Flags & 1))
            Item = {};
        if ((Extra & 8) || ((Extra & 16) && (Flags & 2)))
            Bits.String();
        if (Flags & 2)
        {
            Item.Character = Bits.Word();
            Item.First = Frame;
        }
        if (Flags & 4)
            Item.Matrix = FMatrix2::Read(Bits);
        if (Flags & 8)
        {
            const bool bAdd = Bits.Read(1) != 0, bMult = Bits.Read(1) != 0;
            const int32 N = Bits.Read(4);
            float Mult = 1, Add = 0;
            if (bMult)
            {
                Bits.Signed(N); Bits.Signed(N); Bits.Signed(N);
                Mult = Bits.Signed(N) / 256.f;
            }
            if (bAdd)
            {
                Bits.Signed(N); Bits.Signed(N); Bits.Signed(N);
                Add = Bits.Signed(N) / 255.f;
            }
            Item.Alpha = FMath::Clamp(Mult + Add, 0.f, 1.f);
            Bits.Align();
        }
        if (Flags & 16)
            Bits.Word();
        if (Flags & 32)
            Item.Name = Bits.String().ToLower();
        return Bits.bValid;
    }

    bool ReadSwf(TConstArrayView<uint8> Swf, TArray<uint8>& Storage, FBits& Body, uint32 MaxBytes = 8 * 1024 * 1024)
    {
        if (Swf.Num() < 8 || Swf[1] != 'W' || Swf[2] != 'S')
            return false;
        FBits Header{Swf.Slice(4, 4)};
        const uint32 Length = Header.DWord();
        if (Length < 8 || Length > MaxBytes)
            return false;
        Body = FBits{Swf.Slice(8, Swf.Num() - 8)};
        if (Swf[0] == 'F')
            return Length == static_cast<uint32>(Swf.Num());
        if (Swf[0] != 'C')
            return false;
        // The shipped Polish font movie declares its body length instead of the total SWF length.
        Storage.SetNumUninitialized(Length);
        uLongf BodyLength = Length;
        if (uncompress(Storage.GetData(), &BodyLength, Body.Bytes.GetData(), Body.Bytes.Num()) != Z_OK
            || (BodyLength != Length - 8 && BodyLength != Length))
            return false;
        Storage.SetNum(BodyLength, EAllowShrinking::No);
        Body = FBits{Storage};
        return true;
    }

    uint32 ReadTag(FBits& Bits, FBits& Tag)
    {
        const uint32 Header = Bits.Word();
        const uint32 Length = (Header & 63) == 63 ? Bits.DWord() : Header & 63;
        Tag = FBits{Bits.Payload(Length)};
        return Header >> 6;
    }

    int32 FontUnits(int32 Value)
    {
        // DefineFont3 stores 20 shape units per unit of its 1024-unit em.
        return FMath::RoundToInt(Value / 20.0);
    }

    bool ReadGlyph(FBits Shape, FT5FontGlyph& Glyph)
    {
        const int32 FillBits = Shape.Read(4), LineBits = Shape.Read(4);
        FIntPoint Pen(0, 0);
        int32 Start = 0;
        bool bHasStart = false;
        const auto Close = [&]
        {
            if (Glyph.Points.Num() > Start)
                Glyph.ContourEnds.Add(Glyph.Points.Num() - 1);
            Start = Glyph.Points.Num();
            bHasStart = false;
        };
        const auto Point = [&](bool bOnCurve)
        {
            const int32 X = FontUnits(Pen.X), Y = -FontUnits(Pen.Y);
            if (FMath::Abs(X) > 8192 || FMath::Abs(Y) > 8192 || Glyph.Points.Num() >= 8192)
                Shape.bValid = false;
            else
                Glyph.Points.Add({int16(X), int16(Y), bOnCurve});
        };
        while (Shape.bValid)
        {
            if (!Shape.Read(1))
            {
                const uint32 Flags = Shape.Read(5);
                if (!Flags)
                {
                    Close();
                    return Shape.bValid;
                }
                if (Flags & 16)
                    return false;
                if (Flags & 1)
                {
                    Close();
                    const int32 Width = Shape.Read(5);
                    Pen.X = Shape.Signed(Width);
                    Pen.Y = Shape.Signed(Width);
                }
                if (Flags & 2) Shape.Read(FillBits);
                if (Flags & 4) Shape.Read(FillBits);
                if (Flags & 8) Shape.Read(LineBits);
                continue;
            }

            if (!bHasStart)
            {
                Point(true);
                bHasStart = true;
            }
            const bool bStraight = Shape.Read(1) != 0;
            const int32 Width = Shape.Read(4) + 2;
            if (bStraight)
            {
                const bool bGeneral = Shape.Read(1) != 0;
                const bool bVertical = !bGeneral && Shape.Read(1);
                if (!bVertical) Pen.X += Shape.Signed(Width);
                if (bGeneral || bVertical) Pen.Y += Shape.Signed(Width);
            }
            else
            {
                Pen.X += Shape.Signed(Width);
                Pen.Y += Shape.Signed(Width);
                Point(false);
                Pen.X += Shape.Signed(Width);
                Pen.Y += Shape.Signed(Width);
            }
            Point(true);
        }
        return false;
    }

    bool ReadFont(FBits Tag, uint32 Flags, FT5FontOutline& Font)
    {
        const bool bWideOffsets = (Flags & 8) != 0;
        const bool bWideCodes = (Flags & 4) != 0;
        const int32 Count = Tag.Word();
        if (!Tag.bValid || !(Flags & 128) || Count <= 1 || Count > 32768)
            return false;
        const int32 Base = Tag.Cursor / 8;
        TArray<uint32> Offsets;
        Offsets.Reserve(Count + 1);
        for (int32 Index = 0; Index <= Count; ++Index)
            Offsets.Add(bWideOffsets ? Tag.DWord() : Tag.Word());
        if (!Tag.bValid || Offsets[0] < uint32(Tag.Cursor / 8 - Base)
            || Offsets.Last() > uint32(Tag.Bytes.Num() - Base))
            return false;
        Font.Glyphs.SetNum(Count);
        Tag.Cursor = (Base + Offsets.Last()) * 8;
        for (int32 Index = 0; Index < Count; ++Index)
        {
            if (Offsets[Index] >= Offsets[Index + 1] || Offsets[Index + 1] > Offsets.Last()
                || !ReadGlyph(FBits{Tag.Bytes.Slice(Base + Offsets[Index], Offsets[Index + 1] - Offsets[Index])},
                    Font.Glyphs[Index]))
                return false;
            Font.Glyphs[Index].Code = bWideCodes ? Tag.Word() : Tag.Byte();
        }
        Font.Ascent = FontUnits(int16(Tag.Word()));
        Font.Descent = -FMath::Abs(FontUnits(int16(Tag.Word())));
        Font.Leading = FontUnits(int16(Tag.Word()));
        for (FT5FontGlyph& Glyph : Font.Glyphs)
            Glyph.Advance = FMath::Max(0, FontUnits(int16(Tag.Word())));
        for (int32 Index = 0; Index < Count && Tag.bValid; ++Index)
            Tag.Rect(); // TrueType bounds are derived from the converted outlines.
        const uint32 KerningCount = Tag.Word();
        for (uint32 Index = 0; Index < KerningCount && Tag.bValid; ++Index)
        {
            const uint16 Left = bWideCodes ? Tag.Word() : Tag.Byte();
            const uint16 Right = bWideCodes ? Tag.Word() : Tag.Byte();
            const int16 Adjustment = FontUnits(int16(Tag.Word()));
            if (Adjustment)
                Font.Kerning.Add({Left, Right, Adjustment});
        }
        return Tag.bValid;
    }

    struct FArtSymbol
    {
        uint32 Code;
        FBits Tag;
    };

    TMap<int32, FPlaced> FirstFrame(const FArtSymbol* Sprite)
    {
        TMap<int32, FPlaced> Display;
        if (!Sprite || Sprite->Code != 39)
            return Display;
        FBits Timeline = Sprite->Tag;
        Timeline.Word(); // Frame count.
        while (Timeline.bValid)
        {
            FBits Tag;
            const uint32 Code = ReadTag(Timeline, Tag);
            if (Timeline.bValid && Code == 1)
                return Display;
            if (!Code || ((Code == 26 || Code == 70) && !Place(Tag, Code == 70, 0, Display)))
                break;
            if (Code == 28)
                Display.Remove(Tag.Word());
        }
        return {};
    }

    const FArtSymbol* FindVector(const TMap<int32, FArtSymbol>& Symbols, int32 Id, int32 Depth = 0)
    {
        const FArtSymbol* Symbol = Symbols.Find(Id);
        if (!Symbol || Depth >= 8)
            return nullptr;
        if (Symbol->Code != 39)
            return Symbol;
        auto Frame = FirstFrame(Symbol);
        Frame.KeySort(TLess<int32>());
        for (const auto& [Layer, Item] : Frame)
            if (const FArtSymbol* Child = FindVector(Symbols, Item.Character, Depth + 1))
                return Child;
        return nullptr;
    }
}

void ImportT5UIFonts(TConstArrayView<uint8> Swf, FT5UIFontSet& Fonts)
{
    TSet<FName> Needed;
    for (const auto& [Role, Face] : Fonts.Roles)
        if (!Fonts.Faces.Contains(Face))
            Needed.Add(Face);
    TArray<uint8> Storage;
    FBits Body;
    if (Needed.IsEmpty() || !ReadSwf(Swf, Storage, Body, 32 * 1024 * 1024))
        return;
    Body.Rect();
    Body.DWord(); // Frame rate and frame count.
    while (Body.bValid && !Needed.IsEmpty())
    {
        FBits Tag;
        const uint32 Code = ReadTag(Body, Tag);
        if (!Code)
            break;
        if (Code != 75)
            continue;
        Tag.Word(); // Character ID.
        const uint32 Flags = Tag.Byte();
        Tag.Byte(); // Language code.
        const auto Name = Tag.Payload(Tag.Byte());
        if (!Tag.bValid || Name.IsEmpty())
            continue;
        FT5FontOutline Font;
        Font.Style = Flags & 3;
        for (const uint8 Byte : Name)
            if (Byte)
                Font.Name.AppendChar(Byte);
        const FName Key = T5FontKey(Font.Name, Font.Style);
        if (Needed.Contains(Key) && ReadFont(Tag, Flags, Font))
        {
            auto Data = MakeT5TrueType(Font);
            if (!Data.IsEmpty())
            {
                Fonts.Faces.Add(Key, MoveTemp(Data));
                Needed.Remove(Key);
            }
        }
    }
}

TMap<FName, FT5UIBitmap> ImportT5UIArt(TConstArrayView<uint8> Swf, const TCHAR* Source, TConstArrayView<FT5UIArtSpec> Specs)
{
    TArray<uint8> Storage;
    FBits Body;
    if (!ReadSwf(Swf, Storage, Body))
        return {};
    Body.Rect();
    Body.Word(); // Frame rate.
    Body.Word(); // Frame count.
    TMap<int32, FArtSymbol> Symbols;
    TMap<FString, int32> Exports;
    while (Body.bValid)
    {
        FBits Tag;
        const uint32 Code = ReadTag(Body, Tag);
        if (!Code)
            break;
        if (Code == 2 || Code == 22 || Code == 32 || Code == 39)
        {
            const int32 Id = Tag.Word();
            if (!Tag.bValid)
                return {};
            Symbols.Add(Id, {Code, Tag});
        }
        if (Code == 56)
        {
            const int32 Count = Tag.Word();
            for (int32 Index = 0; Index < Count && Tag.bValid; ++Index)
            {
                const int32 Id = Tag.Word();
                Exports.Add(Tag.String().ToLower(), Id);
            }
            if (!Tag.bValid)
                return {};
        }
    }
    if (!Body.bValid)
        return {};

    const auto Child = [&](int32 Id, const FString& Name)
    {
        auto Frame = FirstFrame(Symbols.Find(Id));
        if (Name.StartsWith(TEXT("#")))
            if (const auto* Item = Frame.Find(FCString::Atoi(*Name + 1)))
                return Item->Character;
        for (const auto& [Depth, Item] : Frame)
            if (Item.Name == Name)
                return Item.Character;
        return 0;
    };
    TMap<FName, FT5UIBitmap> Result;
    for (const auto& [Name, Path] : Specs)
    {
        TArray<FString> Parts;
        FString(Path).ToLower().ParseIntoArray(Parts, TEXT("/"));
        int32 Id = Exports.FindRef(Parts[0]);
        for (int32 Index = 1; Id && Index < Parts.Num(); ++Index)
            Id = Child(Id, Parts[Index]);
        const FArtSymbol* Symbol = FindVector(Symbols, Id);
        if (!Symbol)
            continue;
        FT5UIBitmap Bitmap;
        FBox2f Bounds;
        FBits Tag = Symbol->Tag;
        if (RasterizeShape(Tag, Symbol->Code == 32, Bounds, Bitmap, 768)
            && Bitmap.Pixels.ContainsByPredicate([](FColor Pixel) { return Pixel.A > 0; }))
        {
            Bitmap.Source = Source;
            Bitmap.Symbol = Path;
            Result.Add(Name, MoveTemp(Bitmap));
        }
    }
    return Result;
}

struct FT5TitleMovie
{
    TMap<int32, FShape> Shapes;
    TMap<int32, TArray<FTitleFrame>> Sprites;
    TMap<FString, FPlaced> Sequences;
    FBox2f Stage;
    float FPS = 30;

    bool Timeline(FBits& Bits, TArray<FTitleFrame>& Frames, bool bRoot)
    {
        TMap<int32, FPlaced> Display;
        while (Bits.bValid && Bits.Cursor < Bits.Bytes.Num() * 8)
        {
            const uint32 Header = Bits.Word(), Code = Header >> 6;
            const uint32 Length = (Header & 63) == 63 ? Bits.DWord() : Header & 63;
            FBits Body{Bits.Payload(Length)};
            if (!Bits.bValid)
                return false;
            if (!Code)
                return true;
            if (Code == 1)
            {
                if (Frames.Num() >= 1200 || Display.Num() > 128)
                    return false;
                Display.KeySort(TLess<int32>());
                auto& Frame = Frames.AddDefaulted_GetRef();
                Display.GenerateValueArray(Frame);
            }
            else if (Code == 26 || Code == 70)
            {
                if (!Place(Body, Code == 70, Frames.Num(), Display))
                    return false;
            }
            else if (Code == 28)
                Display.Remove(Body.Word());
            else if (bRoot && (Code == 2 || Code == 22 || Code == 32))
            {
                const int32 Id = Body.Word();
                if (!Shape(Body, Code == 32, Shapes.FindOrAdd(Id)))
                {
                    UE_LOG(LogT5Titles, Warning, TEXT("T5_TITLES_ERROR invalid shape id=%d tag=%u offset=%d"), Id, Code, Body.Cursor / 8);
                    return false;
                }
            }
            else if (bRoot && Code == 39)
            {
                const int32 Id = Body.Word(), Count = Body.Word();
                auto& Sprite = Sprites.FindOrAdd(Id);
                if (!Timeline(Body, Sprite, false) || Count != Sprite.Num())
                {
                    UE_LOG(LogT5Titles, Warning, TEXT("T5_TITLES_ERROR invalid sprite id=%d frames=%d expected=%d"), Id, Sprite.Num(), Count);
                    return false;
                }
            }
        }
        return false;
    }

    bool Load(TConstArrayView<uint8> Swf)
    {
        TArray<uint8> Decompressed;
        FBits Body;
        if (!ReadSwf(Swf, Decompressed, Body))
            return false;
        Stage = Body.Rect();
        FPS = Body.Word() / 256.f;
        Body.Word();
        TArray<FTitleFrame> Root;
        if (FPS <= 0 || Stage.GetSize().GetMin() <= 0 || !Timeline(Body, Root, true) || Root.IsEmpty())
            return false;
        for (const auto& Item : Root[0])
            if (!Item.Name.IsEmpty() && Sprites.Contains(Item.Character))
                Sequences.Add(Item.Name, Item);
        return !Sequences.IsEmpty();
    }

    void Paint(const FPlaced& Item, const FMatrix2& Parent, float Opacity, int32 Frame, int32 Depth,
        const FGeometry& Geometry, FSlateWindowElementList& Elements, int32& Layer) const
    {
        const auto Matrix = Parent.Child(Item.Matrix);
        Opacity *= Item.Alpha;
        if (Opacity <= 0 || Depth >= 16)
            return;
        if (const auto* Shape = Shapes.Find(Item.Character))
        {
            const auto Resource = FSlateApplication::Get().GetRenderer()->GetResourceHandle(Shape->Brush);
            const auto Min = Shape->Bounds.Min, Max = Shape->Bounds.Max;
            const auto& Transform = Geometry.GetAccumulatedRenderTransform();
            const FColor Color(255, 255, 255, FMath::RoundToInt(255 * Opacity));
            const TArray<FSlateVertex> Vertices{
                FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, Matrix.Point(Min), FVector2f(0, 0), Color),
                FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, Matrix.Point({Max.X, Min.Y}), FVector2f(1, 0), Color),
                FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, Matrix.Point(Max), FVector2f(1, 1), Color),
                FSlateVertex::Make<ESlateVertexRounding::Disabled>(Transform, Matrix.Point({Min.X, Max.Y}), FVector2f(0, 1), Color)};
            const TArray<SlateIndex> Indices{0, 1, 2, 0, 2, 3};
            FSlateDrawElement::MakeCustomVerts(Elements, ++Layer, Resource, Vertices, Indices, nullptr, 0, 0);
            return;
        }
        if (const auto* Sprite = Sprites.Find(Item.Character); Sprite && !Sprite->IsEmpty())
        {
            const int32 At = FMath::Clamp(Frame - Item.First, 0, Sprite->Num() - 1);
            for (const auto& Child : (*Sprite)[At])
                Paint(Child, Matrix, Opacity, At, Depth + 1, Geometry, Elements, Layer);
        }
    }
};

void ST5Titles::Command(const TArray<TSharedPtr<FJsonValue>>& Args, double Now, const FT5PapyrusRuntime& Runtime)
{
    const auto Action = Args[0]->AsString();
    if (Action == TEXT("help") && Args.Num() == 3)
    {
        HelpText = Args[1]->AsString(); HelpUntil = Now + Args[2]->AsNumber();
        HelpText.ReplaceInline(TEXT("[Forward]"), TEXT("W"));
        HelpText.ReplaceInline(TEXT("[Strafe Left]"), TEXT("A"));
        HelpText.ReplaceInline(TEXT("[Back]"), TEXT("S"));
        HelpText.ReplaceInline(TEXT("[Strafe Right]"), TEXT("D"));
    }
    else if (Action == TEXT("showtitlesequencemenu"))
    {
        bMenu = true;
        if (!Movie)
        {
            TArray<uint8> Bytes;
            auto NewMovie = MakeShared<FT5TitleMovie>();
            const bool bRead = Runtime.ReadResource(TEXT("interface/titles.swf"), Bytes);
            if (bRead && NewMovie->Load(Bytes))
            {
                Movie = MoveTemp(NewMovie);
                UE_LOG(LogT5Titles, Display, TEXT("T5_TITLES_LOADED sequences=%d shapes=%d fps=%.2f"),
                    Movie->Sequences.Num(), Movie->Shapes.Num(), Movie->FPS);
            }
            else
                UE_LOG(LogT5Titles, Warning, TEXT("T5_TITLES_ERROR interface/titles.swf read=%d bytes=%d"), bRead, Bytes.Num());
        }
    }
    else if (Action == TEXT("hidetitlesequencemenu"))
    {
        bMenu = false;
        Sequence.Reset();
    }
    else if (Action == TEXT("starttitlesequence") && Args.Num() == 2)
    {
        Sequence = Args[1]->AsString().ToLower();
        bPainted = false;
        Started = Now;
        if (Movie && Movie->Sequences.Contains(Sequence))
        {
            UE_LOG(LogT5Titles, Display, TEXT("T5_TITLE_SEQUENCE %s"), *Sequence);
        }
        else
            UE_LOG(LogT5Titles, Warning, TEXT("T5_TITLES_ERROR missing sequence %s"), *Sequence);
    }
    else if (Action == TEXT("fadeoutgame") && Args.Num() == 5)
    {
        bFadeOut = Args[1]->AsNumber() != 0;
        bBlack = Args[2]->AsNumber() != 0;
        FadeStart = Now + Args[3]->AsNumber();
        FadeLength = Args[4]->AsNumber();
        bFading = true;
    }
}

void ST5Titles::Advance(double Now)
{
    CurrentTime = Now;
    if (bFading)
    {
        const double Progress = FadeLength > 0 ? FMath::Clamp((Now - FadeStart) / FadeLength, 0., 1.) : Now >= FadeStart ? 1. : 0.;
        Fade = bFadeOut ? Progress : 1. - Progress;
        bFading = Progress < 1.;
    }
    Invalidate(EInvalidateWidgetReason::Paint);
}

int32 ST5Titles::OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&, FSlateWindowElementList& Elements,
    int32 Layer, const FWidgetStyle&, bool) const
{
    const float Overlay = bWorldReady ? Fade : 1.f;
    if (bWorldReady && CurrentTime < HelpUntil && !HelpText.IsEmpty())
    {
        const auto Size = Geometry.GetLocalSize();
        const auto Font = FCoreStyle::GetDefaultFontStyle("Regular", 22);
        FSlateDrawElement::MakeText(Elements, ++Layer,
            Geometry.ToPaintGeometry(FVector2f(Size.X * .8f, 80.f), FSlateLayoutTransform(FVector2f(Size.X * .1f, Size.Y * .2f))),
            HelpText, Font, ESlateDrawEffect::None, FLinearColor::White);
    }
    if (Overlay > 0)
    {
        const FLinearColor Color(bBlack ? 0.f : 1.f, bBlack ? 0.f : 1.f, bBlack ? 0.f : 1.f, Overlay);
        FSlateDrawElement::MakeBox(Elements, ++Layer, Geometry.ToPaintGeometry(), FCoreStyle::Get().GetBrush("WhiteBrush"),
            ESlateDrawEffect::None, Color);
    }
    if (bMenu && Movie)
        if (const auto* Item = Movie->Sequences.Find(Sequence))
        {
            const auto Size = FVector2f(Geometry.GetLocalSize());
            const auto StageSize = Movie->Stage.GetSize();
            const float Scale = FMath::Min(Size.X / StageSize.X, Size.Y / StageSize.Y);
            const FMatrix2 Matrix{{Scale, 0}, {0, Scale}, (Size - StageSize * Scale) * .5f - Movie->Stage.Min * Scale};
            const int32 PreviousLayer = Layer;
            Movie->Paint(*Item, Matrix, 1.f, FMath::FloorToInt((CurrentTime - Started) * Movie->FPS), 0, Geometry, Elements, Layer);
            if (!bPainted && Layer > PreviousLayer)
            {
                bPainted = true;
                UE_LOG(LogT5Titles, Display, TEXT("T5_TITLE_PAINT %s"), *Sequence);
            }
        }
    return Layer;
}

void ST5Titles::SerializeCheckpoint(FArchive& Ar, const FT5PapyrusRuntime& Runtime)
{
    Ar << Sequence << Started << CurrentTime << FadeStart << FadeLength << Fade
        << bMenu << bFading << bFadeOut << bBlack << bWorldReady;
    if (Ar.IsLoading() && bMenu && !Movie)
    {
        TArray<uint8> Bytes;
        auto NewMovie = MakeShared<FT5TitleMovie>();
        if (Runtime.ReadResource(TEXT("interface/titles.swf"), Bytes) && NewMovie->Load(Bytes)) Movie = MoveTemp(NewMovie);
        else Ar.SetError();
    }
    bPainted = false;
}
