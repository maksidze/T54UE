#include "T5MotionData.h"
#include "T5Coordinates.h"
#include "String/LexFromString.h"
#include "Misc/Paths.h"

namespace
{
    FString Key(FString Name)
    {
        Name.ReplaceInline(TEXT("\\"), TEXT("/"));
        return Name.ToLower();
    }
    struct FReader
    {
        const TArray<FString>& Lines;
        int32 At, End;
        bool Valid = true;
        FString Text()
        {
            if (At >= End) { Valid = false; return {}; }
            return Lines[At++];
        }
        int32 Count()
        {
            int32 Value = -1;
            if (!LexTryParseString(Value, *Text()) || Value < 0 || Value > 1000000) Valid = false;
            return Valid ? Value : 0;
        }
        float Number()
        {
            float Value = 0;
            if (!LexTryParseString(Value, *Text()) || !FMath::IsFinite(Value)) Valid = false;
            return Value;
        }
        bool Row(int32 Count, float* Values)
        {
            TArray<FString> Parts;
            Text().ParseIntoArrayWS(Parts);
            if (Parts.Num() != Count) return Valid = false;
            for (int32 I = 0; I < Count; ++I)
                if (!LexTryParseString(Values[I], *Parts[I]) || !FMath::IsFinite(Values[I])) return Valid = false;
            return Valid;
        }
        void Blank() { while (At < End && Lines[At].IsEmpty()) ++At; }
    };
}

FTransform FT5MotionCurve::Sample(float Time) const
{
    Time = FMath::Clamp(Time, 0.f, Duration);
    FVector Position = FVector::ZeroVector;
    FQuat Rotation = FQuat::Identity;
    float Previous = 0;
    for (const auto& Key : Positions)
    {
        if (Time < Key.Time)
        {
            Position = FMath::Lerp(Position, Key.Value, (Time - Previous) / (Key.Time - Previous));
            break;
        }
        Position = Key.Value;
        Previous = Key.Time;
    }
    Previous = 0;
    for (const auto& Key : Rotations)
    {
        if (Time < Key.Time)
        {
            Rotation = FQuat::Slerp(Rotation, Key.Value, (Time - Previous) / (Key.Time - Previous)).GetNormalized();
            break;
        }
        Rotation = Key.Value;
        Previous = Key.Time;
    }
    return FTransform(Rotation, Position);
}

bool FT5MotionCurve::Equals(const FT5MotionCurve& Other) const
{
    if (!FMath::IsNearlyEqual(Duration, Other.Duration) || Positions.Num() != Other.Positions.Num()
        || Rotations.Num() != Other.Rotations.Num()) return false;
    for (int32 I = 0; I < Positions.Num(); ++I)
        if (Positions[I].Time != Other.Positions[I].Time || !Positions[I].Value.Equals(Other.Positions[I].Value)) return false;
    for (int32 I = 0; I < Rotations.Num(); ++I)
        if (Rotations[I].Time != Other.Rotations[I].Time || !Rotations[I].Value.Equals(Other.Rotations[I].Value)) return false;
    return true;
}

bool FT5MotionDatabase::Read(TConstArrayView<uint8> Bytes, FString& Error)
{
    const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
    TArray<FString> Lines;
    FString(Text.Length(), Text.Get()).ParseIntoArrayLines(Lines, false);
    for (auto& Line : Lines) Line.TrimStartAndEndInline();
    FReader Input{Lines, 0, Lines.Num()};
    const int32 Count = Input.Count();
    TArray<FString> Names;
    for (int32 I = 0; Input.Valid && I < Count; ++I) Names.Add(Key(FPaths::GetBaseFilename(Input.Text())));
    TMultiMap<FString, FProject> Parsed;
    for (const auto& Name : Names)
    {
        const int32 Size = Input.Count();
        if (!Input.Valid || Size > Input.End - Input.At) { Input.Valid = false; break; }
        FReader Block{Lines, Input.At, Input.At + Size};
        Input.At += Size;
        FProject Project;
        Block.Count(); // project active flag
        const int32 Files = Block.Count();
        for (int32 I = 0; Block.Valid && I < Files; ++I) Project.Files.Add(Key(Block.Text()));
        const bool bMotion = Block.Count() != 0;
        while (Block.Valid && Block.At < Block.End)
        {
            Block.Blank();
            if (Block.At == Block.End) break;
            const FString Generator = Block.Text();
            const int32 Index = Block.Count();
            Block.Number(); Block.Number(); Block.Number(); // playback metadata
            const int32 Events = Block.Count();
            for (int32 I = 0; Block.Valid && I < Events; ++I) Block.Text();
            if (Project.Generators.Contains(Generator) && Project.Generators[Generator] != Index) Block.Valid = false;
            Project.Generators.Add(Generator, Index);
        }
        if (!Block.Valid) { Input.Valid = false; Input.At = Block.At; break; }
        if (bMotion)
        {
            const int32 MotionSize = Input.Count();
            if (!Input.Valid || MotionSize > Input.End - Input.At) { Input.Valid = false; break; }
            FReader Motion{Lines, Input.At, Input.At + MotionSize};
            Input.At += MotionSize;
            while (Motion.Valid && Motion.At < Motion.End)
            {
                Motion.Blank();
                if (Motion.At == Motion.End) break;
                const int32 Index = Motion.Count();
                auto Curve = MakeShared<FT5MotionCurve>();
                Curve->Duration = Motion.Number();
                if (Curve->Duration <= 0) Motion.Valid = false;
                const int32 Positions = Motion.Count();
                float Previous = -1;
                for (int32 I = 0; Motion.Valid && I < Positions; ++I)
                {
                    float V[4];
                    if (!Motion.Row(4, V)) break;
                    if (V[0] < 0 || V[0] <= Previous || V[0] > Curve->Duration + .001f) { Motion.Valid = false; break; }
                    Curve->Positions.Add({V[0], T5ToUnrealPosition(FVector(V[1], V[2], V[3]))});
                    Previous = V[0];
                }
                const int32 Rotations = Motion.Count();
                Previous = -1;
                for (int32 I = 0; Motion.Valid && I < Rotations; ++I)
                {
                    float V[5];
                    if (!Motion.Row(5, V)) break;
                    FQuat Q(V[1], V[2], V[3], V[4]);
                    if (V[0] < 0 || V[0] <= Previous || V[0] > Curve->Duration + .001f || Q.SizeSquared() < .001)
                    { Motion.Valid = false; break; }
                    Curve->Rotations.Add({V[0], T5ToUnrealRotation(Q.GetNormalized())});
                    Previous = V[0];
                }
                if (Project.Curves.Contains(Index)) Motion.Valid = false;
                Project.Curves.Add(Index, MoveTemp(Curve));
            }
            if (!Motion.Valid) { Input.Valid = false; Input.At = Motion.At; break; }
        }
        Parsed.Add(Name, MoveTemp(Project));
    }
    Input.Blank();
    if (!Input.Valid || Input.At != Input.End)
    {
        Error = FString::Printf(TEXT("Invalid animationdatasinglefile.txt near line %d"), Input.At + 1);
        return false;
    }
    Projects = MoveTemp(Parsed);
    return true;
}

TSharedPtr<const FT5MotionCurve> FT5MotionDatabase::Find(const FString& ProjectName, const FString& Skeleton,
    const FString& Generator, FString& Error) const
{
    const FString Name = Key(FPaths::GetBaseFilename(ProjectName));
    const FString Rig = Key(FPaths::GetCleanFilename(Key(Skeleton)));
    const FString Clip = Generator;
    TSharedPtr<const FT5MotionCurve> Result;
    for (const auto& [Id, Project] : Projects)
    {
        if (!Name.IsEmpty() ? Id != Name : !Project.Files.ContainsByPredicate([&](const FString& File) {
            return FPaths::GetCleanFilename(File) == Rig; })) continue;
        const auto* Index = Project.Generators.Find(Clip);
        if (!Index) continue;
        const auto* Curve = Project.Curves.Find(*Index);
        if (!Curve) continue;
        if (Result && !Result->Equals(**Curve))
        {
            Error = FString::Printf(TEXT("Ambiguous movement curve for generator '%s'; character project is required"), *Generator);
            return {};
        }
        Result = *Curve;
    }
    return Result;
}
