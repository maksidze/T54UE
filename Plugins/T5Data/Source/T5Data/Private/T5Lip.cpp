#include "T5Lip.h"

namespace
{
    template<class T> T Read(const uint8* Data) { T Value; FMemory::Memcpy(&Value, Data, sizeof(T)); return Value; }
}

const TArray<FName>& T5LipMorphNames()
{
    static const TArray<FName> Names = {
        TEXT("Aah"), TEXT("BigAah"), TEXT("BMP"), TEXT("ChJSh"), TEXT("DST"), TEXT("Eee"), TEXT("Eh"), TEXT("FV"),
        TEXT("I"), TEXT("K"), TEXT("N"), TEXT("Oh"), TEXT("OohQ"), TEXT("R"), TEXT("Th"), TEXT("W"),
        TEXT("BlinkLeft"), TEXT("BlinkRight"), TEXT("BrowDownLeft"), TEXT("BrowDownRight"), TEXT("BrowInLeft"),
        TEXT("BrowInRight"), TEXT("BrowUpLeft"), TEXT("BrowUpRight"), TEXT("LookDown"), TEXT("LookLeft"),
        TEXT("LookRight"), TEXT("LookUp"), TEXT("SquintLeft"), TEXT("SquintRight") };
    return Names;
}

TSharedPtr<FT5LipTrack> DecodeT5FuzLip(TConstArrayView<uint8> Fuz, FString& Error)
{
    if (Fuz.Num() < 12 || FMemory::Memcmp(Fuz.GetData(), "FUZE", 4)) return {};
    const uint32 Length = Read<uint32>(Fuz.GetData()+8);
    if (!Length) return {};
    const auto Fail = [&]() -> TSharedPtr<FT5LipTrack> { Error = TEXT("Unsupported or truncated T5 LIP data"); return {}; };
    if (Length > uint32(Fuz.Num()-12) || Length < 12) return Fail();
    const auto Lip = Fuz.Slice(12, Length);
    const uint32 LipVersion = Read<uint32>(Lip.GetData());
    const uint32 Allocation = Read<uint32>(Lip.GetData()+4);
    const uint32 Compressed = Read<uint32>(Lip.GetData()+8);
    if (LipVersion != 1 || Allocation < 24 || Allocation > 64*1024*1024 || Compressed > 1) return Fail();
    const uint32 Expected = Allocation-16;
    TArray<uint8> Decoded;
    Decoded.Reserve(Expected);
    for (int32 I = 12; I < Lip.Num();)
    {
        if (Compressed && Lip[I] == 0)
        {
            if (I+3 > Lip.Num()) return Fail();
            const uint16 Run = Read<uint16>(Lip.GetData()+I+1);
            if (!Run || Run > Expected-uint32(Decoded.Num())) return Fail();
            Decoded.AddZeroed(Run); I += 3;
        }
        else
        {
            if (uint32(Decoded.Num()) >= Expected) return Fail();
            Decoded.Add(Lip[I++]);
        }
    }
    if (uint32(Decoded.Num()) != Expected) return Fail();
    const int32 Frames = Read<int32>(Decoded.GetData());
    const int32 First = Read<int32>(Decoded.GetData()+4);
    if (Frames <= 0 || int64(Frames)*33*4+8 != Expected || FMath::Abs(int64(First)) > Frames) return Fail();
    auto Track = MakeShared<FT5LipTrack>();
    Track->FirstFrame = First;
    Track->Samples.SetNumUninitialized(Frames*33);
    FMemory::Memcpy(Track->Samples.GetData(), Decoded.GetData()+8, Frames*33*4);
    for (float Value : Track->Samples) if (!FMath::IsFinite(Value)) return Fail();
    return Track;
}

float FT5LipTrack::Sample(int32 Channel, double Seconds) const
{
    // The original sampler adds and cancels the negative first-frame offset.
    // Samples are indexed by elapsed voice time, not by Seconds - FirstFrame/30.
    const double Frame = Seconds*30.;
    const int32 Frames = Samples.Num()/33;
    if (Channel < 0 || Channel >= 33 || Frame < 0 || Frame > Frames-1) return 0;
    const int32 A = FMath::FloorToInt(Frame), B = FMath::Min(A+1, Frames-1);
    return FMath::Clamp(FMath::Lerp(Samples[A*33+Channel], Samples[B*33+Channel], float(Frame-A)), 0.f, 1.f);
}
