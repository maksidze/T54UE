#include "T5Hkx.h"

#include "T5SkeletalMesh.h"
#include "Misc/Paths.h"

#include <cstring>

namespace
{
    // HKX v8 layout and spline encoding reference:
    // https://github.com/BadDogSkyrim/PyNifly/blob/main/docs/hkx_animation_format.md
    // https://github.com/SARDONYX-sard/serde-hkx/tree/main/assets/classes
    class FBytes
    {
    public:
        explicit FBytes(TConstArrayView<uint8> In) : Bytes(In) {}

        bool Has(int64 Offset, int64 Size) const
        {
            return Offset >= 0 && Size >= 0 && Offset <= Bytes.Num() && Size <= Bytes.Num() - Offset;
        }

        template <typename T>
        T At(int64 Offset) const
        {
            T Value{};
            if (Has(Offset, sizeof(T)))
                FMemory::Memcpy(&Value, Bytes.GetData() + Offset, sizeof(T));
            else
                bValid = false;
            return Value;
        }

        FString String(int64 Offset, int64 Limit) const
        {
            if (!Has(Offset, Limit))
                return {};
            const auto* First = reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + Offset);
            const auto* Last = static_cast<const ANSICHAR*>(std::memchr(First, 0, Limit));
            if (!Last)
                return {};
            const FUTF8ToTCHAR Converted(First, static_cast<int32>(Last - First));
            return FString(Converted.Length(), Converted.Get());
        }

        TConstArrayView<uint8> View(int32 Offset, int32 Size) const
        {
            return Has(Offset, Size) ? Bytes.Slice(Offset, Size) : TConstArrayView<uint8>{};
        }

        mutable bool bValid = true;

    private:
        TConstArrayView<uint8> Bytes;
    };

    struct FSection
    {
        int32 Data, Local, Global, Virtual, Exports, End;
    };

    class FPack
    {
    public:
        explicit FPack(TConstArrayView<uint8> Bytes) : File(Bytes) {}

        bool Open()
        {
            if (File.At<uint32>(0) != 0x57e0e057 || File.At<uint32>(4) != 0x10c0c010
                || File.At<uint32>(12) != 8 || File.At<uint8>(16) != 8 || File.At<uint8>(17) != 1
                || File.String(40, 16) != TEXT("hk_2010.2.0-r1"))
                return false;
            const auto Count = File.At<int32>(20);
            if (Count < 3 || Count > 16)
                return false;
            int32 DataIndex = INDEX_NONE;
            for (int32 I = 0; I < Count; ++I)
            {
                const int32 H = 64 + I * 48;
                const int64 Start = File.At<uint32>(H + 20);
                const uint32 Local = File.At<uint32>(H + 24);
                const uint32 Global = File.At<uint32>(H + 28);
                const uint32 Virtual = File.At<uint32>(H + 32);
                const uint32 Exports = File.At<uint32>(H + 36);
                const uint32 End = File.At<uint32>(H + 44);
                if (Local > Global || Global > Virtual || Virtual > Exports || Exports > End
                    || !File.Has(Start, End))
                    return false;
                Sections.Add({static_cast<int32>(Start), static_cast<int32>(Start + Local),
                    static_cast<int32>(Start + Global), static_cast<int32>(Start + Virtual),
                    static_cast<int32>(Start + Exports), static_cast<int32>(Start + End)});
                if (File.String(H, 16) == TEXT("__data__"))
                    DataIndex = I;
            }
            if (DataIndex == INDEX_NONE)
                return false;
            const auto& Data = Sections[DataIndex];
            for (int32 Pos = Data.Local; Pos <= Data.Global - 8; Pos += 8)
            {
                const auto From = File.At<uint32>(Pos);
                if (From == MAX_uint32)
                    break;
                const auto To = File.At<uint32>(Pos + 4);
                if (!AddFixup(Data, From, Data, To))
                    return false;
            }
            for (int32 Pos = Data.Global; Pos <= Data.Virtual - 12; Pos += 12)
            {
                const auto From = File.At<uint32>(Pos);
                if (From == MAX_uint32)
                    break;
                const int32 Target = File.At<int32>(Pos + 4);
                const auto To = File.At<uint32>(Pos + 8);
                if (!Sections.IsValidIndex(Target) || !AddFixup(Data, From, Sections[Target], To))
                    return false;
            }
            for (int32 Pos = Data.Virtual; Pos <= Data.Exports - 12; Pos += 12)
            {
                const auto From = File.At<uint32>(Pos);
                if (From == MAX_uint32)
                    break;
                const int32 Target = File.At<int32>(Pos + 4);
                const auto Name = File.At<uint32>(Pos + 8);
                if (!Sections.IsValidIndex(Target) || From >= static_cast<uint32>(Data.Local - Data.Data))
                    return false;
                const auto& Strings = Sections[Target];
                if (Name >= static_cast<uint32>(Strings.Local - Strings.Data))
                    return false;
                const int32 NamePos = Strings.Data + Name;
                const FString Class = File.String(NamePos, FMath::Min(128, Strings.Local - NamePos));
                if (Class.IsEmpty())
                    return false;
                Objects.Add(Data.Data + From, FName(*Class));
            }
            DataStart = Data.Data;
            DataEnd = Data.Local;
            return File.bValid;
        }

        int32 Ref(int32 Field) const
        {
            const auto* Value = Pointers.Find(Field);
            return Value ? *Value : INDEX_NONE;
        }

        FString RefString(int32 Field) const
        {
            const int32 Pos = Ref(Field);
            return Pos >= DataStart && Pos < DataEnd ? File.String(Pos, FMath::Min(1024, DataEnd - Pos)) : FString{};
        }

        TPair<int32, int32> Array(int32 Field, int32 Stride) const
        {
            const int32 Count = File.At<int32>(Field + 8);
            const int32 Pos = Ref(Field);
            if (Count == 0)
                return {INDEX_NONE, 0};
            if (Count < 0 || Pos < DataStart || static_cast<int64>(Stride) * Count > DataEnd - Pos)
            {
                File.bValid = false;
                return {INDEX_NONE, 0};
            }
            return {Pos, Count};
        }

        int32 Find(FName Name) const
        {
            for (const auto& [Pos, Type] : Objects)
                if (Type == Name)
                    return Pos;
            return INDEX_NONE;
        }

        FBytes File;
        TMap<int32, FName> Objects;

    private:
        bool AddFixup(const FSection& From, uint32 Field, const FSection& To, uint32 Value)
        {
            if (Field >= static_cast<uint32>(From.Local - From.Data)
                || Value >= static_cast<uint32>(To.Local - To.Data))
                return false;
            Pointers.Add(From.Data + Field, To.Data + Value);
            return true;
        }

        TArray<FSection> Sections;
        TMap<int32, int32> Pointers;
        int32 DataStart = 0;
        int32 DataEnd = 0;
    };

    FTransform ConvertTransform(FVector Position, FQuat Rotation, FVector Scale)
    {
        return FTransform(T5ToUnrealRotation(Rotation), T5ToUnrealPosition(Position), T5ToUnrealDirection(Scale));
    }

    FVector VectorAt(const FBytes& Bytes, int32 Pos)
    {
        return FVector(Bytes.At<float>(Pos), Bytes.At<float>(Pos + 4), Bytes.At<float>(Pos + 8));
    }

    FQuat QuatAt(const FBytes& Bytes, int32 Pos)
    {
        return FQuat(Bytes.At<float>(Pos), Bytes.At<float>(Pos + 4), Bytes.At<float>(Pos + 8), Bytes.At<float>(Pos + 12));
    }

    bool ReadSkeleton(const FPack& Pack, FString& Name, TArray<FT5HkxBone>& Out)
    {
        // Character skeleton packfiles can also contain separate ragdoll skeletons.
        for (const auto& [Pos, Class] : Pack.Objects)
        {
            if (Class != TEXT("hkaSkeleton") || (!Name.IsEmpty() && Pack.RefString(Pos + 16) != Name))
                continue;
            const auto [Parents, NumParents] = Pack.Array(Pos + 24, 2);
            const auto [Bones, NumBones] = Pack.Array(Pos + 40, 16);
            const auto [Poses, NumPoses] = Pack.Array(Pos + 56, 48);
            if (NumBones <= Out.Num() || NumBones != NumParents || NumBones != NumPoses || NumBones > 2048)
                continue;
            TArray<FT5HkxBone> Candidate;
            TSet<FName> Names;
            for (int32 I = 0; I < NumBones; ++I)
            {
                const FName BoneName(*Pack.RefString(Bones + 16 * I));
                const int32 Parent = Pack.File.At<int16>(Parents + 2 * I);
                const int32 Rest = Poses + 48 * I;
                const auto Position = VectorAt(Pack.File, Rest);
                const auto Rotation = QuatAt(Pack.File, Rest + 16);
                const auto Scale = VectorAt(Pack.File, Rest + 32);
                if (BoneName.IsNone() || Names.Contains(BoneName) || Parent < INDEX_NONE || Parent >= I
                    || Position.ContainsNaN() || Rotation.ContainsNaN() || Scale.ContainsNaN())
                    return false;
                Names.Add(BoneName);
                Candidate.Add({BoneName, Parent, ConvertTransform(Position, Rotation, Scale)});
            }
            Out = MoveTemp(Candidate);
        }
        return !Out.IsEmpty() && Pack.File.bValid;
    }

    class FBlockReader
    {
    public:
        explicit FBlockReader(TConstArrayView<uint8> In, int32 Start) : Bytes(In), Cursor(Start) {}

        template <typename T>
        T Read()
        {
            const auto Value = Bytes.At<T>(Cursor);
            Cursor += sizeof(T);
            return Value;
        }

        void Align(int32 Boundary) { Cursor = ::Align(Cursor, Boundary); }

        FBytes Bytes;
        int32 Cursor;
    };

    struct FKnots
    {
        int32 Points = 0;
        int32 Degree = 0;
        TArray<uint8, TInlineAllocator<64>> Values;

        bool Read(FBlockReader& Reader)
        {
            Points = Reader.Read<uint16>() + 1;
            Degree = Reader.Read<uint8>();
            if (Degree > 7 || Degree >= Points || Points > 1024)
                return false;
            const int32 Count = Points + Degree + 1;
            for (int32 I = 0; I < Count; ++I)
            {
                const auto Knot = Reader.Read<uint8>();
                if (I > 0 && Knot < Values.Last())
                    return false;
                Values.Add(Knot);
            }
            return Reader.Bytes.bValid;
        }

        template <typename T>
        T Sample(const TArray<T>& Controls, int32 Frame) const
        {
            const int32 Time = FMath::Clamp(Frame, static_cast<int32>(Values[Degree]), static_cast<int32>(Values[Points]));
            int32 Span = Degree;
            while (Span < Points - 1 && Values[Span + 1] <= Time)
                ++Span;
            T Work[8];
            for (int32 J = 0; J <= Degree; ++J)
                Work[J] = Controls[Span - Degree + J];
            for (int32 Pass = 1; Pass <= Degree; ++Pass)
                for (int32 J = Degree; J >= Pass; --J)
                {
                    const int32 I = Span - Degree + J;
                    const int32 Width = Values[I + Degree - Pass + 1] - Values[I];
                    const double Alpha = Width > 0 ? static_cast<double>(Time - Values[I]) / Width : 0;
                    Work[J] = Work[J - 1] * (1.0 - Alpha) + Work[J] * Alpha;
                }
            return Work[Degree];
        }
    };

    struct FVectorChannel
    {
        FVector Constant;
        TArray<double> Controls[3];
        FKnots Knots;

        bool Read(FBlockReader& Reader, uint8 Flags, uint8 Quant, double Identity)
        {
            Constant = FVector(Identity);
            if (Quant > 1)
                return false;
            if ((Flags & 0x70) && !Knots.Read(Reader))
                return false;
            Reader.Align(4);
            FVector Min(0), Max(0);
            for (int32 Axis = 0; Axis < 3; ++Axis)
            {
                if (Flags & (0x10 << Axis))
                {
                    Min[Axis] = Reader.Read<float>();
                    Max[Axis] = Reader.Read<float>();
                }
                else if (Flags & (1 << Axis))
                    Constant[Axis] = Reader.Read<float>();
            }
            if (Min.ContainsNaN() || Max.ContainsNaN() || Constant.ContainsNaN())
                return false;
            for (int32 I = 0; I < Knots.Points; ++I)
                for (int32 Axis = 0; Axis < 3; ++Axis)
                    if (Flags & (0x10 << Axis))
                    {
                        const double Fraction = Quant == 0 ? Reader.Read<uint8>() / 255.0 : Reader.Read<uint16>() / 65535.0;
                        Controls[Axis].Add(FMath::Lerp(Min[Axis], Max[Axis], Fraction));
                    }
            Reader.Align(4);
            return Reader.Bytes.bValid;
        }

        FVector Sample(int32 Frame) const
        {
            FVector Out = Constant;
            for (int32 Axis = 0; Axis < 3; ++Axis)
                if (!Controls[Axis].IsEmpty())
                    Out[Axis] = Knots.Sample(Controls[Axis], Frame);
            return Out;
        }
    };

    struct FRotationChannel
    {
        FQuat Constant = FQuat::Identity;
        TArray<FQuat> Controls;
        FKnots Knots;

        static FQuat ReadValue(FBlockReader& Reader, uint8 Quant)
        {
            if (Quant == 5)
            {
                const auto Value = QuatAt(Reader.Bytes, Reader.Cursor);
                Reader.Cursor += 16;
                return Value;
            }
            double Stored[3];
            int32 Missing;
            bool bNegative;
            if (Quant == 1)
            {
                uint64 Bits = 0;
                for (int32 I = 0; I < 5; ++I)
                    Bits |= static_cast<uint64>(Reader.Read<uint8>()) << (8 * I);
                for (int32 I = 0; I < 3; ++I)
                    Stored[I] = (static_cast<int32>((Bits >> (12 * I)) & 4095) - 2049) * 0.000345436;
                Missing = (Bits >> 36) & 3;
                bNegative = (Bits >> 38) & 1;
            }
            else
            {
                const auto A = Reader.Read<uint16>();
                const auto B = Reader.Read<uint16>();
                const auto C = Reader.Read<uint16>();
                const uint16 Raw[3] = {A, B, C};
                for (int32 I = 0; I < 3; ++I)
                    Stored[I] = ((Raw[I] & 32767) - 16383) * 0.000043161;
                Missing = (A >> 15) | ((B >> 14) & 2);
                bNegative = C & 32768;
            }
            double Components[4];
            for (int32 I = 0, J = 0; I < 4; ++I)
                Components[I] = I == Missing
                    ? (bNegative ? -1 : 1) * FMath::Sqrt(FMath::Max(0.0, 1.0 - FVector(Stored[0], Stored[1], Stored[2]).SizeSquared()))
                    : Stored[J++];
            return FQuat(Components[0], Components[1], Components[2], Components[3]);
        }

        bool Read(FBlockReader& Reader, uint8 Flags, uint8 Quant)
        {
            if (Flags == 0)
                return true;
            if (Quant != 1 && Quant != 2 && Quant != 5)
                return false;
            if ((Flags & 0xf0) && !Knots.Read(Reader))
                return false;
            Reader.Align(Quant == 5 ? 4 : Quant);
            if (Knots.Points == 0)
            {
                Constant = ReadValue(Reader, Quant);
                if (Constant.ContainsNaN())
                    return false;
            }
            else
                for (int32 I = 0; I < Knots.Points; ++I)
                {
                    FQuat Value = ReadValue(Reader, Quant);
                    if (Value.ContainsNaN())
                        return false;
                    if (!Controls.IsEmpty() && (Value | Controls.Last()) < 0)
                        Value = Value * -1;
                    Controls.Add(Value);
                }
            Reader.Align(4);
            return Reader.Bytes.bValid;
        }

        FQuat Sample(int32 Frame) const { return Controls.IsEmpty() ? Constant : Knots.Sample(Controls, Frame); }
    };

    bool ReadBlocks(const FPack& Pack, int32 Anim, FT5HkxClip& Clip)
    {
        const auto [Offsets, Blocks] = Pack.Array(Anim + 88, 4);
        const auto [Bytes, Size] = Pack.Array(Anim + 152, 1);
        const int32 Stride = Pack.File.At<int32>(Anim + 64) - 1;
        const int32 MaskSize = Pack.File.At<int32>(Anim + 68);
        if (Stride <= 0 || Stride > 255 || MaskSize < 4 * Clip.Tracks || MaskSize > Size
            || Blocks != Pack.File.At<int32>(Anim + 60)
            || Blocks != FMath::Max(1, FMath::DivideAndRoundUp(Clip.Frames - 1, Stride)))
            return false;
        Clip.Samples.SetNumUninitialized(Clip.Tracks * Clip.Frames);
        for (int32 Block = 0; Block < Blocks; ++Block)
        {
            const int32 Start = Pack.File.At<int32>(Offsets + 4 * Block);
            const int32 End = Block + 1 < Blocks ? Pack.File.At<int32>(Offsets + 4 * (Block + 1)) : Size;
            if (Start < 0 || End > Size || End <= Start || MaskSize > End - Start || Start % 4)
                return false;
            const auto Data = Pack.File.View(Bytes + Start, End - Start);
            FBlockReader Reader(Data, MaskSize);
            for (int32 Track = 0; Track < Clip.Tracks; ++Track)
            {
                const uint8 Quant = Data[Track * 4];
                FVectorChannel Position, Scale;
                FRotationChannel Rotation;
                if (!Position.Read(Reader, Data[Track * 4 + 1], Quant & 3, 0)
                    || !Rotation.Read(Reader, Data[Track * 4 + 2], (Quant >> 2) & 15)
                    || !Scale.Read(Reader, Data[Track * 4 + 3], Quant >> 6, 1))
                    return false;
                const int32 FrameCount = FMath::Min(Stride + 1, Clip.Frames - Block * Stride);
                for (int32 I = Block == 0 ? 0 : 1; I < FrameCount; ++I)
                {
                    const int32 Index = (Block * Stride + I) * Clip.Tracks + Track;
                    Clip.Samples[Index] = ConvertTransform(Position.Sample(I), Rotation.Sample(I), Scale.Sample(I));
                }
            }
        }
        return Pack.File.bValid;
    }

    int32 ResolveGenerator(const FPack& Pack, int32 Generator, int32 NestedState, bool bScoped, int32 Depth = 0)
    {
        const auto* Class = Pack.Objects.Find(Generator);
        if (!Class || Depth >= 16)
            return INDEX_NONE;
        if (*Class == TEXT("hkbClipGenerator"))
            return Generator;
        if (*Class == TEXT("hkbModifierGenerator"))
            return ResolveGenerator(Pack, Pack.Ref(Generator + 80), NestedState, bScoped, Depth + 1);
        if (*Class == TEXT("hkbStateMachine"))
        {
            const int32 Target = NestedState != INDEX_NONE ? NestedState : Pack.File.At<int32>(Generator + 104);
            const auto [States, Count] = Pack.Array(Generator + 144, 8);
            for (int32 I = 0; I < Count; ++I)
            {
                const int32 State = Pack.Ref(States + 8 * I);
                if (State != INDEX_NONE && Pack.File.At<int32>(State + 104) == Target)
                    return ResolveGenerator(Pack, Pack.Ref(State + 88), INDEX_NONE, bScoped, Depth + 1);
            }
        }
        if (bScoped && *Class == TEXT("hkbManualSelectorGenerator"))
        {
            const auto [Children, Count] = Pack.Array(Generator + 72, 8);
            const int32 Selected = Pack.File.At<int16>(Generator + 88);
            if (Selected >= 0 && Selected < Count)
                return ResolveGenerator(Pack, Pack.Ref(Children + 8 * Selected), NestedState, bScoped, Depth + 1);
        }
        if (bScoped && *Class == TEXT("hkbBlenderGenerator"))
        {
            const auto [Children, Count] = Pack.Array(Generator + 96, 8);
            const bool bParametric = (Pack.File.At<uint16>(Generator + 90) & 0x10) != 0;
            const float Parameter = Pack.File.At<float>(Generator + 72);
            float Best = -TNumericLimits<float>::Max();
            int32 Result = INDEX_NONE;
            for (int32 I = 0; I < Count; ++I)
            {
                const int32 Child = Pack.Ref(Children + I * 8);
                const float Weight = Pack.File.At<float>(Child + 64);
                if (!FMath::IsFinite(Weight) || (!bParametric && Weight <= 0))
                    continue;
                float Score = bParametric ? -FMath::Abs(Weight - Parameter) : Weight;
                if (!bParametric)
                    if (const int32 Mask = Pack.Ref(Child + 56); Mask != INDEX_NONE)
                    {
                        const auto [Values, Num] = Pack.Array(Mask + 48, 4);
                        float Coverage = 0;
                        for (int32 J = 0; J < Num; ++J)
                            Coverage += Pack.File.At<float>(Values + 4 * J);
                        Score *= Coverage;
                    }
                if (!FMath::IsFinite(Score) || Score <= Best)
                    continue;
                const auto Clip = ResolveGenerator(Pack, Pack.Ref(Child + 48), NestedState, bScoped, Depth + 1);
                if (Clip != INDEX_NONE) { Best = Score; Result = Clip; }
            }
            return Result;
        }
        return INDEX_NONE;
    }

}

bool ReadT5HkxRagdoll(TConstArrayView<uint8> Skeleton, FT5HkxRagdoll& Out)
{
    FPack Pack(Skeleton);
    if (!Pack.Open()) return false;
    const int32 Instance = Pack.Find(TEXT("hkaRagdollInstance"));
    if (Instance == INDEX_NONE) return false;
    // hkaRagdollInstance contains two pointer arrays followed by the bone-to-body map and its skeleton.
    const auto [Bodies, BodyCount] = Pack.Array(Instance + 16, 8);
    const auto [Joints, JointCount] = Pack.Array(Instance + 32, 8);
    const auto [BodyMap, BoneCount] = Pack.Array(Instance + 48, 4);
    const int32 RagdollSkeleton = Pack.Ref(Instance + 64);
    if (BodyCount <= 0 || BodyCount > 2048 || JointCount <= 0 || JointCount > 4096 || BoneCount > 2048
        || Pack.Objects.FindRef(RagdollSkeleton) != TEXT("hkaSkeleton"))
        return false;

    FT5HkxRagdoll Rig;
    Rig.Bodies.SetNum(BodyCount);
    // The simple skeleton mappings associate the ragdoll bones with the animated mesh bone names.
    for (const auto& [Pos, Class] : Pack.Objects)
    {
        if (Class != TEXT("hkaSkeletonMapper") || Pack.Ref(Pos + 16) != RagdollSkeleton) continue;
        const int32 AnimatedSkeleton = Pack.Ref(Pos + 24);
        if (Pack.Objects.FindRef(AnimatedSkeleton) != TEXT("hkaSkeleton")) return false;
        const auto [AnimatedBones, AnimatedCount] = Pack.Array(AnimatedSkeleton + 40, 16);
        const auto [Mappings, MappingCount] = Pack.Array(Pos + 32, 64);
        if (MappingCount > 2048) return false;
        for (int32 Index = 0; Index < MappingCount; ++Index)
        {
            const int32 At = Mappings + Index * 64;
            const int32 RagdollBone = Pack.File.At<int16>(At);
            const int32 AnimatedBone = Pack.File.At<int16>(At + 2);
            if (RagdollBone < 0 || RagdollBone >= BoneCount || AnimatedBone < 0 || AnimatedBone >= AnimatedCount)
                return false;
            const int32 Body = Pack.File.At<int32>(BodyMap + RagdollBone * 4);
            if (Body == INDEX_NONE) continue;
            if (!Rig.Bodies.IsValidIndex(Body) || !Rig.Bodies[Body].IsNone()) return false;
            Rig.Bodies[Body] = FName(*Pack.RefString(AnimatedBones + AnimatedBone * 16));
        }
        break;
    }
    TMap<int32, FName> BodyNames;
    TSet<FName> UniqueNames;
    for (int32 Index = 0; Index < BodyCount; ++Index)
    {
        const int32 Body = Pack.Ref(Bodies + Index * 8);
        const FName Name = Rig.Bodies[Index];
        if (Pack.Objects.FindRef(Body) != TEXT("hkpRigidBody") || BodyNames.Contains(Body)
            || Name.IsNone() || UniqueNames.Contains(Name))
            return false;
        BodyNames.Add(Body, Name);
        UniqueNames.Add(Name);
    }
    for (int32 Index = 0; Index < JointCount; ++Index)
    {
        const int32 Joint = Pack.Ref(Joints + Index * 8);
        if (Pack.Objects.FindRef(Joint) != TEXT("hkpConstraintInstance")) return false;
        // The two entries in hkpConstraintInstance::entities point at the joined rigid bodies.
        const FName A = BodyNames.FindRef(Pack.Ref(Joint + 40)), B = BodyNames.FindRef(Pack.Ref(Joint + 48));
        if (A.IsNone() || B.IsNone() || A == B) return false;
        Rig.Joints.Emplace(A, B);
    }
    if (!Pack.File.bValid) return false;
    Out = MoveTemp(Rig);
    return true;
}

bool ReadT5HkxClip(TConstArrayView<uint8> Skeleton, TConstArrayView<uint8> Animation, FT5HkxClip& Out, FString& Error)
{
    FPack Rig(Skeleton), File(Animation);
    if (!Rig.Open() || !File.Open())
    {
        Error = TEXT("Expected a 64-bit T5 HKX packfile");
        return false;
    }
    const int32 Anim = File.Find(TEXT("hkaSplineCompressedAnimation"));
    const int32 Binding = File.Find(TEXT("hkaAnimationBinding"));
    if (Anim == INDEX_NONE || Binding == INDEX_NONE || File.File.At<int32>(Binding + 64) != 0)
    {
        Error = TEXT("Expected a normal spline-compressed HKX animation binding");
        return false;
    }
    FString SkeletonName = File.RefString(Binding + 16);
    FT5HkxClip Clip;
    bool bFoundSkeleton = ReadSkeleton(Rig, SkeletonName, Clip.Bones);
    // Shipped horse_rider clips retain the export rig's gender suffix. The
    // distributed character skeleton uses the same root without that suffix.
    if (!bFoundSkeleton && (SkeletonName == TEXT("NPC Root [Root] Male")
        || SkeletonName == TEXT("NPC Root [Root] Female")))
    {
        FString CanonicalRoot = TEXT("NPC Root [Root]");
        bFoundSkeleton = ReadSkeleton(Rig, CanonicalRoot, Clip.Bones);
    }
    if (!bFoundSkeleton && SkeletonName.StartsWith(TEXT("exp_"), ESearchCase::CaseSensitive))
    {
        // The shipped weapon clips retain the exporter prefix in the binding name.
        FString Unprefixed = SkeletonName.RightChop(4);
        bFoundSkeleton = ReadSkeleton(Rig, Unprefixed, Clip.Bones);
    }
    if (!bFoundSkeleton)
    {
        Error = FString::Printf(TEXT("HKX skeleton has no matching binding for '%s'"), *SkeletonName);
        return false;
    }
    Clip.Duration = File.File.At<float>(Anim + 20);
    Clip.Tracks = File.File.At<int32>(Anim + 24);
    Clip.Frames = File.File.At<int32>(Anim + 56);
    if (!FMath::IsFinite(Clip.Duration) || Clip.Duration <= 0 || Clip.Tracks <= 0 || Clip.Frames <= 0
        || static_cast<int64>(Clip.Tracks) * Clip.Frames > 1000000 || Clip.Tracks > Clip.Bones.Num())
    {
        Error = TEXT("Invalid HKX clip dimensions");
        return false;
    }
    const auto [Map, Count] = File.Array(Binding + 32, 2);
    if (Count != 0 && Count != Clip.Tracks)
    {
        Error = TEXT("Invalid HKX track-to-bone binding size");
        return false;
    }
    Clip.BoneToTrack.Init(INDEX_NONE, Clip.Bones.Num());
    for (int32 Track = 0; Track < Clip.Tracks; ++Track)
    {
        const int32 Bone = Count > 0 ? File.File.At<int16>(Map + Track * 2) : Track;
        if (Bone == INDEX_NONE)
            continue;
        if (!Clip.Bones.IsValidIndex(Bone) || Clip.BoneToTrack[Bone] != INDEX_NONE)
        {
            Error = TEXT("Invalid HKX bone index in the animation binding");
            return false;
        }
        Clip.BoneToTrack[Bone] = Track;
    }
    if (!ReadBlocks(File, Anim, Clip))
    {
        Error = TEXT("Unsupported or invalid HKX spline block");
        return false;
    }
    const auto [Tracks, TrackCount] = File.Array(Anim + 40, 24);
    for (int32 I = 0; I < TrackCount; ++I)
    {
        const auto [Events, EventCount] = File.Array(Tracks + 24 * I + 8, 16);
        for (int32 J = 0; J < EventCount; ++J)
        {
            const int32 Entry = Events + 16 * J;
            const float Time = File.File.At<float>(Entry);
            const auto Name = File.RefString(Entry + 8);
            if (FMath::IsFinite(Time) && Time >= 0 && Time <= Clip.Duration && !Name.IsEmpty())
                Clip.Annotations.Add({Time, Name});
        }
    }
    if (!File.File.bValid)
    {
        Error = TEXT("Invalid HKX annotations");
        return false;
    }
    Clip.Annotations.StableSort([](const auto& A, const auto& B) { return A.Time < B.Time; });
    Out = MoveTemp(Clip);
    return true;
}

namespace
{
    TArray<int32> GeneratorChildren(const FPack& Pack, int32 Node)
    {
        TArray<int32> Result;
        const auto* Class = Pack.Objects.Find(Node);
        if (!Class) return Result;
        if (*Class == TEXT("hkbModifierGenerator")) Result.Add(Pack.Ref(Node + 80));
        if (*Class == TEXT("hkbStateMachine"))
        {
            const auto [States, Count] = Pack.Array(Node + 144, 8);
            for (int32 I = 0; I < Count; ++I) Result.Add(Pack.Ref(Pack.Ref(States + 8 * I) + 88));
        }
        if (*Class == TEXT("hkbManualSelectorGenerator"))
        {
            const auto [Values, Count] = Pack.Array(Node + 72, 8);
            for (int32 I = 0; I < Count; ++I) Result.Add(Pack.Ref(Values + 8 * I));
        }
        return Result;
    }

    // Find the nearest authored bone-weighted blender above this state machine.
    // A transition in an arm layer must not replace the locomotion pose of the legs.
    TArray<float> GraphBoneWeights(const FPack& Pack, int32 Machine)
    {
        TSet<int32> Ancestors{Machine};
        bool bChanged = true;
        while (bChanged)
        {
            bChanged = false;
            for (const auto& [Node, Class] : Pack.Objects)
                if (!Ancestors.Contains(Node))
                    for (int32 Child : GeneratorChildren(Pack, Node))
                        if (Ancestors.Contains(Child)) { Ancestors.Add(Node); bChanged = true; break; }
        }
        for (const auto& [Node, Class] : Pack.Objects)
        {
            if (Class != TEXT("hkbBlenderGenerator")) continue;
            const auto [Children, Count] = Pack.Array(Node + 96, 8);
            TArray<float> Selected, Total;
            bool bSelected = false;
            for (int32 I = 0; I < Count; ++I)
            {
                const int32 Child = Pack.Ref(Children + I * 8);
                const int32 Weights = Pack.Ref(Child + 56);
                if (Weights == INDEX_NONE) continue;
                const auto [Values, Num] = Pack.Array(Weights + 48, 4);
                const float Weight = Pack.File.At<float>(Child + 64);
                if (!FMath::IsFinite(Weight) || Weight < 0) { Pack.File.bValid = false; return {}; }
                const bool bThis = Ancestors.Contains(Pack.Ref(Child + 48));
                bSelected |= bThis;
                Selected.SetNumZeroed(FMath::Max(Selected.Num(), Num));
                Total.SetNumZeroed(FMath::Max(Total.Num(), Num));
                for (int32 J = 0; J < Num; ++J)
                {
                    const float Value = Pack.File.At<float>(Values + 4 * J) * Weight;
                    if (!FMath::IsFinite(Value) || Value < 0) { Pack.File.bValid = false; return {}; }
                    Total[J] += Value;
                    if (bThis) Selected[J] += Value;
                }
            }
            if (bSelected)
            {
                for (int32 I = 0; I < Selected.Num(); ++I)
                    Selected[I] = Total[I] > 0 ? Selected[I] / Total[I] : 0;
                return Selected;
            }
        }
        return {};
    }
}

bool RemapT5HkxBoneWeights(TConstArrayView<uint8> ReferenceRig, const TArray<FT5HkxBone>& Target,
    TArray<float>& Weights, FString& Error)
{
    FPack Rig(ReferenceRig); TArray<FT5HkxBone> Source;
    FString Name=Target.IsEmpty()?FString{}:Target[0].Name.ToString();
    if (!Rig.Open() || !ReadSkeleton(Rig,Name,Source) || Source.Num()!=Weights.Num())
    { Error=TEXT("No reference skeleton matching the behavior bone mask"); return false; }
    TMap<FName,int32> TargetIndices;
    for (int32 I=0;I<Target.Num();++I) TargetIndices.Add(Target[I].Name,I);
    TArray<float> Mapped; Mapped.Init(0.f,Target.Num());
    for (int32 I=0;I<Source.Num();++I)
    {
        const auto* Index=TargetIndices.Find(Source[I].Name);
        if (!Index && Weights[I]!=0)
        { Error=TEXT("Masked bone absent from target skeleton: ")+Source[I].Name.ToString(); return false; }
        if (Index) Mapped[*Index]=Weights[I];
    }
    Weights=MoveTemp(Mapped); Error.Reset(); return true;
}

namespace
{
    TArray<FString> GraphEvents(const FPack& Pack)
    {
        const int32 Root = Pack.Find(TEXT("hkbBehaviorGraph"));
        const int32 Data = Root == INDEX_NONE ? INDEX_NONE : Pack.Ref(Root + 136);
        const int32 Names = Data == INDEX_NONE ? INDEX_NONE : Pack.Ref(Data + 120);
        TArray<FString> Result;
        if (Names != INDEX_NONE)
        {
            const auto [Events, Count] = Pack.Array(Names + 16, 8);
            for (int32 I = 0; I < Count; ++I) Result.Add(Pack.RefString(Events + 8 * I));
        }
        return Result;
    }

    FString GraphEvent(const FPack& Pack, const TArray<FString>& Names, int32 Event)
    {
        const int32 Id = Pack.File.At<int32>(Event);
        if (!Names.IsValidIndex(Id) || Names[Id].IsEmpty()) return {};
        const int32 Payload = Pack.Ref(Event + 8);
        if (Names[Id].StartsWith(TEXT("AnimObj")) && Pack.Objects.FindRef(Payload) == TEXT("hkbStringEventPayload"))
            if (const auto Text = Pack.RefString(Payload + 16); !Text.IsEmpty())
                return Names[Id] + TEXT(".") + Text;
        return Names[Id];
    }

    void GraphNotify(const FPack& Pack, const TArray<FString>& Names, int32 Field, TArray<FString>& Out)
    {
        if (const int32 List = Pack.Ref(Field); List != INDEX_NONE)
        {
            const auto [Events, Count] = Pack.Array(List + 16, 16);
            for (int32 I = 0; I < Count; ++I)
                if (auto Event = GraphEvent(Pack, Names, Events + I * 16); !Event.IsEmpty())
                    Out.AddUnique(MoveTemp(Event));
        }
    }

    void GraphTriggers(const FPack& Pack, const TArray<FString>& Names, int32 Clip, FT5HkxPlayback& Out)
    {
        if (const int32 List = Pack.Ref(Clip + 80); List != INDEX_NONE)
        {
            const auto [Triggers, Count] = Pack.Array(List + 16, 32);
            for (int32 I = 0; I < Count; ++I)
            {
                const int32 Entry = Triggers + I * 32;
                const float Time = Pack.File.At<float>(Entry);
                if (auto Event = GraphEvent(Pack, Names, Entry + 8); !Event.IsEmpty() && FMath::IsFinite(Time))
                    Out.Triggers.Add({Time, MoveTemp(Event), Pack.File.At<uint8>(Entry + 24) != 0,
                        Pack.File.At<uint8>(Entry + 25) != 0});
            }
        }
    }

    TArray<int32> GraphOwnerStates(const FPack& Pack, int32 Clip)
    {
        TArray<int32> States;
        TSet<int32> Visited;
        TArray<int32> Pending{Clip};
        for (int32 I = 0; I < Pending.Num(); ++I)
        {
            const int32 Node = Pending[I];
            if (Visited.Contains(Node)) continue;
            Visited.Add(Node);
            for (const auto& [Parent, Type] : Pack.Objects)
            {
                if (Type == TEXT("hkbStateMachineStateInfo") && Pack.Ref(Parent + 88) == Node)
                {
                    States.AddUnique(Parent);
                    Pending.Add(Parent);
                }
                else if (Type == TEXT("hkbStateMachine") && Pack.Objects.FindRef(Node) == TEXT("hkbStateMachineStateInfo"))
                {
                    const auto [Rows, Count] = Pack.Array(Parent + 144, 8);
                    for (int32 State = 0; State < Count; ++State)
                        if (Pack.Ref(Rows + State * 8) == Node) { Pending.Add(Parent); break; }
                }
                else if (Type == TEXT("hkbModifierGenerator") && Pack.Ref(Parent + 80) == Node)
                    Pending.Add(Parent);
            }
        }
        return States;
    }

    void GraphPrependEnter(const FPack& Pack, const TArray<FString>& Names, int32 State,
        TArray<FString>& Out, bool bPropsOnly)
    {
        TArray<FString> Events;
        GraphNotify(Pack, Names, State + 64, Events);
        for (int32 I = Events.Num() - 1; I >= 0; --I)
            if ((!bPropsOnly || Events[I].StartsWith(TEXT("AnimObjLoad."))
                    || Events[I].StartsWith(TEXT("AnimObjDraw."))) && !Out.Contains(Events[I]))
                Out.Insert(MoveTemp(Events[I]), 0);
    }
}

bool ReadT5HkxClipEvents(TConstArrayView<uint8> Graph, const FString& ClipName, FT5HkxPlayback& Playback)
{
    FPack Pack(Graph);
    if (!Pack.Open()) return false;
    int32 Clip = INDEX_NONE;
    for (const auto& [Node, Type] : Pack.Objects)
        if (Type == TEXT("hkbClipGenerator"))
        {
            auto Generator = Pack.RefString(Node + 56);
            Generator.RemoveFromEnd(TEXT(".hkx"));
            if (!Generator.Equals(ClipName, ESearchCase::IgnoreCase)) continue;
            if (Clip != INDEX_NONE) return false;
            Clip = Node;
        }
    if (Clip == INDEX_NONE) return false;
    const auto Names = GraphEvents(Pack);
    GraphTriggers(Pack, Names, Clip, Playback);
    // Walk upward through state ownership. Entering a paired clip also needs
    // the resources loaded by its enclosing states. Their exits belong to the state machine.
    for (const int32 Owner : GraphOwnerStates(Pack, Clip))
    {
        GraphPrependEnter(Pack, Names, Owner, Playback.EnterEvents, false);
        if (Pack.Ref(Owner + 88) == Clip) GraphNotify(Pack, Names, Owner + 72, Playback.ExitEvents);
    }
    return Pack.File.bValid;
}

bool ResolveT5HkxEvent(TConstArrayView<uint8> Graph, const FString& Event, FT5HkxPlayback& Out, FString& Error)
{
    FPack Pack(Graph);
    if (!Pack.Open())
    {
        Error = TEXT("Expected a 64-bit T5 HKX behavior packfile");
        return false;
    }
    const int32 Root = Pack.Find(TEXT("hkbBehaviorGraph"));
    const auto EventNames = GraphEvents(Pack);
    FString MachineName, EventName, SourceState;
    const bool bScoped = Event.Split(TEXT("::"), &MachineName, &EventName);
    if (!bScoped) EventName = Event;
    if (bScoped)
    {
        FString StateMachine;
        if (MachineName.Split(TEXT("/"), &StateMachine, &SourceState)) MachineName = MoveTemp(StateMachine);
    }
    const int32 EventId = EventNames.IndexOfByPredicate(
        [&EventName](const FString& Name) { return Name.Equals(EventName, ESearchCase::IgnoreCase); });
    if (EventId == INDEX_NONE)
    {
        Error = FString::Printf(TEXT("HKX graph has no event '%s'"), *Event);
        return false;
    }
    FT5HkxPlayback Found;
    for (const auto& [Machine, Class] : Pack.Objects)
    {
        if (Class != TEXT("hkbStateMachine") || (bScoped && !Pack.RefString(Machine + 56).Equals(MachineName, ESearchCase::IgnoreCase)))
            continue;
        TSet<int32> Transitions;
        if (const int32 Wildcards = Pack.Ref(Machine + 160); Wildcards != INDEX_NONE)
            Transitions.Add(Wildcards);
        const auto [States, Count] = Pack.Array(Machine + 144, 8);
        TMap<int32, int32> StateIds;
        TSet<int32> AppliedClips;
        for (int32 I = 0; I < Count; ++I)
        {
            const int32 State = Pack.Ref(States + 8 * I);
            if (State == INDEX_NONE)
                continue;
            StateIds.Add(Pack.File.At<int32>(State + 104), State);
            if (const int32 List = Pack.Ref(State + 80); List != INDEX_NONE
                && (SourceState.IsEmpty() || Pack.RefString(State + 96).Equals(SourceState, ESearchCase::IgnoreCase)))
                Transitions.Add(List);
        }
        for (const int32 List : Transitions)
        {
            const auto [Records, Num] = Pack.Array(List + 16, 72);
            for (int32 I = 0; I < Num; ++I)
            {
                const int32 Row = Records + I * 72;
                if (Pack.File.At<int32>(Row + 48) != EventId)
                    continue;
                const auto* State = StateIds.Find(Pack.File.At<int32>(Row + 52));
                if (!State || Pack.File.At<uint8>(*State + 112) == 0)
                    continue;
                const uint16 Flags = Pack.File.At<uint16>(Row + 66);
                if ((Flags & 0x23) || (!(Flags & 0x100) && Pack.Ref(Row + 40) != INDEX_NONE))
                    continue;
                const int32 Nested = (Flags & 0x2000) ? Pack.File.At<int32>(Row + 60) : INDEX_NONE;
                const int32 Clip = ResolveGenerator(Pack, Pack.Ref(*State + 88), Nested, bScoped);
                if (Clip == INDEX_NONE)
                {
                    // A masked state machine can return to its ordinary arm
                    // generator without selecting a standalone animation clip.
                    if (Pack.File.At<int32>(Row+52)==Pack.File.At<int32>(Machine+104)
                        && !GraphBoneWeights(Pack, Machine).IsEmpty() && Pack.File.bValid)
                    { Out={}; Out.bClearLayer=true; Error.Reset(); return true; }
                    // An authored transition back to the root machine's default
                    // graph releases a full-body idle. It selects locomotion,
                    // not a standalone clip (e.g. the walking-camera exit).
                    if (Machine==Pack.Ref(Root+128)
                        && Pack.File.At<int32>(Row+52)==Pack.File.At<int32>(Machine+104)
                        && Pack.Ref(*State+88)!=INDEX_NONE && Pack.File.bValid)
                    { Out={}; Out.bReturnToLocomotion=true; Error.Reset(); return true; }
                    continue;
                }
                // Several source states can share the same destination. Count this
                // machine's layer once; distinct arm machines still contribute separately.
                if (AppliedClips.Contains(Clip)) continue;
                AppliedClips.Add(Clip);
                const FString Path = Pack.RefString(Clip + 72);
                const auto Mode = Pack.File.At<uint8>(Clip + 114);
                if (Path.IsEmpty() || Mode > 1 || (Pack.File.At<uint8>(Clip + 115) & 4) != 0)
                    continue;
                FT5HkxPlayback Candidate{Path, Pack.File.At<float>(Clip + 100), Pack.File.At<float>(Clip + 88),
                    Pack.File.At<float>(Clip + 92), Pack.File.At<float>(Clip + 96), Mode == 1};
                Candidate.GeneratorName = Pack.RefString(Clip + 56);
                Candidate.BoneWeights = GraphBoneWeights(Pack, Machine);
                GraphTriggers(Pack, EventNames, Clip, Candidate);
                GraphNotify(Pack, EventNames, *State + 64, Candidate.EnterEvents);
                GraphNotify(Pack, EventNames, *State + 72, Candidate.ExitEvents);
                // A transition may select a clip inside a nested generator.
                // Restore that state's animation objects when starting the clip.
                for (const int32 Owner : GraphOwnerStates(Pack, Clip))
                    if (Owner != *State)
                        GraphPrependEnter(Pack, EventNames, Owner, Candidate.EnterEvents, true);
                const bool bParallelLayers = !Found.BoneWeights.IsEmpty() && !Candidate.BoneWeights.IsEmpty();
                if (!Found.Path.IsEmpty() && (!Found.Path.Equals(Path, ESearchCase::IgnoreCase)
                    || Found.bLoop != Candidate.bLoop || Found.Speed != Candidate.Speed || Found.CropStart != Candidate.CropStart
                    || Found.CropEnd != Candidate.CropEnd || Found.StartTime != Candidate.StartTime
                    || (!bParallelLayers && (Found.Triggers != Candidate.Triggers || Found.EnterEvents != Candidate.EnterEvents
                    || Found.ExitEvents != Candidate.ExitEvents))))
                {
                    Error = FString::Printf(TEXT("HKX event '%s' selects different clips depending on graph state"), *Event);
                    return false;
                }
                if (!Found.Path.IsEmpty() && !Found.BoneWeights.IsEmpty() && !Candidate.BoneWeights.IsEmpty())
                {
                    if (Found.BoneWeights.Num() != Candidate.BoneWeights.Num())
                    { Error = TEXT("Incompatible HKX blender bone masks"); return false; }
                    for (int32 J = 0; J < Found.BoneWeights.Num(); ++J)
                        Candidate.BoneWeights[J] = FMath::Min(1.f, Found.BoneWeights[J] + Candidate.BoneWeights[J]);
                    Candidate.EnterEvents.Append(Found.EnterEvents);
                    Candidate.ExitEvents.Append(Found.ExitEvents);
                    Candidate.Triggers.Append(Found.Triggers);
                }
                Found = MoveTemp(Candidate);
            }
        }
    }
    if (Found.Path.IsEmpty() || !Pack.File.bValid)
    {
        Error = FString::Printf(TEXT("HKX event '%s' has no supported transition to a clip"), *Event);
        return false;
    }
    Out = MoveTemp(Found);
    Error.Reset();
    return true;
}

bool ResolveT5HkxGraph(const FString& Path, const FString& Event,
    TFunctionRef<bool(const FString&, TArray<uint8>&)> Read, FT5HkxPlayback& Out, FString& Error)
{
    TArray<FString> Pending{Path};
    TSet<FString> Visited;
    TOptional<FT5HkxPlayback> Found;
    uint64 TotalBytes=0;
    for (int32 I = 0; I < Pending.Num(); ++I)
    {
        FString Current = Pending[I].Replace(TEXT("\\"), TEXT("/"));
        Current.ToLowerInline();
        if (Visited.Contains(Current)) continue;
        if (Visited.Num() >= 4096) { Error = TEXT("HKX reference graph exceeds 4096 unique files"); return false; }
        Visited.Add(Current);
        TArray<uint8> Bytes;
        if (!Read(Current, Bytes)) { Error = TEXT("Missing referenced HKX behavior: ") + Current; return false; }
        TotalBytes+=Bytes.Num();
        if (TotalBytes>256ull*1024*1024) { Error=TEXT("HKX reference graph exceeds 256 MiB"); return false; }
        FT5HkxPlayback Candidate;
        FString LocalError;
        if (ResolveT5HkxEvent(Bytes, Event, Candidate, LocalError))
        {
            if (Found && (!Found->Path.Equals(Candidate.Path, ESearchCase::IgnoreCase)
                || Found->BoneWeights != Candidate.BoneWeights || Found->Speed != Candidate.Speed
                || Found->bLoop != Candidate.bLoop || Found->CropStart != Candidate.CropStart || Found->CropEnd != Candidate.CropEnd))
            { Error = TEXT("Ambiguous transition in referenced HKX graphs for ") + Event; return false; }
            Found = MoveTemp(Candidate);
            // The root graph owns explicit top-level actions. Descendants handle forwarded events.
            if (I == 0) { Out = MoveTemp(*Found); Error.Reset(); return true; }
        }
        else if (!LocalError.Contains(TEXT("has no event")) && !LocalError.Contains(TEXT("no supported transition")))
        { Error = Current + TEXT(": ") + LocalError; return false; }
        FPack Pack(Bytes);
        if (!Pack.Open()) { Error = TEXT("Invalid referenced HKX graph: ") + Current; return false; }
        const int32 Root = Current.Find(TEXT("/behaviors/"), ESearchCase::IgnoreCase);
        for (const auto& [Node, Class] : Pack.Objects)
            if (Class == TEXT("hkbBehaviorReferenceGenerator"))
            {
                FString Reference = Pack.RefString(Node + 72).Replace(TEXT("\\"), TEXT("/"));
                if (Reference.StartsWith(TEXT("meshes/"), ESearchCase::IgnoreCase)) Pending.Add(Reference);
                else if (Reference.StartsWith(TEXT("actors/"), ESearchCase::IgnoreCase)) Pending.Add(TEXT("meshes/") + Reference);
                else if (Root != INDEX_NONE) Pending.Add(Current.Left(Root + 1) + Reference);
                else { Error = TEXT("Cannot resolve HKX reference relative to ") + Current; return false; }
            }
    }
    if (!Found) { Error = FString::Printf(TEXT("HKX event '%s' has no supported transition in referenced graphs"), *Event); return false; }
    Out = MoveTemp(*Found); Error.Reset(); return true;
}

FString ReadT5HkxProjectCharacter(TConstArrayView<uint8> Project)
{
    FPack Pack(Project);
    if (!Pack.Open())
        return {};
    const int32 Strings = Pack.Find(TEXT("hkbProjectStringData"));
    if (Strings == INDEX_NONE)
        return {};
    const auto [Names, Count] = Pack.Array(Strings + 48, 8);
    return Count == 1 ? Pack.RefString(Names) : FString{};
}

bool ReadT5HkxCharacterClips(TConstArrayView<uint8> Character, FString& Rig, TArray<FString>& Clips)
{
    FPack Pack(Character);
    if (!Pack.Open())
        return false;
    const int32 Strings = Pack.Find(TEXT("hkbCharacterStringData"));
    if (Strings == INDEX_NONE)
        return false;
    const auto [Names, Count] = Pack.Array(Strings + 48, 8);
    for (int32 I = 0; I < Count; ++I)
        Clips.Add(Pack.RefString(Names + I * 8));
    Rig = Pack.RefString(Strings + 168);
    return Pack.File.bValid && !Rig.IsEmpty() && !Clips.IsEmpty();
}

bool LoadT5HkxCharacterProject(const FString& Project, TFunctionRef<bool(const FString&, TArray<uint8>&)> Read,
    FString& Rig, TArray<FString>& Clips)
{
    const auto Resolve = [](const FString& Parent, FString Name)
    {
        Name.ReplaceInline(TEXT("\\"), TEXT("/"));
        FString Result = Parent / Name;
        return !Name.IsEmpty() && FPaths::CollapseRelativeDirectories(Result) ? Result : FString{};
    };
    TArray<uint8> Bytes;
    if (Project.IsEmpty() || !Read(Project, Bytes))
        return false;
    const auto Character = Resolve(FPaths::GetPath(Project), ReadT5HkxProjectCharacter(Bytes));
    if (Character.IsEmpty() || !Read(Character, Bytes) || !ReadT5HkxCharacterClips(Bytes, Rig, Clips))
        return false;
    const auto Root = FPaths::GetPath(FPaths::GetPath(Character));
    Rig = Resolve(Root, MoveTemp(Rig));
    for (auto& Clip : Clips)
        Clip = Resolve(Root, MoveTemp(Clip));
    return !Rig.IsEmpty();
}

void BindT5HkxEvents(FT5HkxClip& Clip, const FT5HkxPlayback& Playback)
{
    const float Begin = FMath::Max(Playback.CropStart, 0.0f);
    const float End = Clip.Duration - FMath::Max(Playback.CropEnd, 0.0f);
    if (End <= Begin)
        return;
    const auto Add = [&](float Time, const FString& Event, bool bOnce) {
        if (Time >= Begin && Time <= End && !Clip.Annotations.ContainsByPredicate([&](const auto& Annotation) {
                return Annotation.Time == Time && Annotation.Event == Event;
            }))
            Clip.Annotations.Add({Time, Event, bOnce});
    };
    for (const auto& Trigger : Playback.Triggers)
        Add(Trigger.Time + (Trigger.bRelativeToEnd ? End : Begin), Trigger.Event, Trigger.bOnce);
    for (const auto& Event : Playback.EnterEvents)
        Add(FMath::Clamp(Begin + Playback.StartTime, Begin, End), Event, true);
    if (!Playback.bLoop)
        for (const auto& Event : Playback.ExitEvents)
            Add(End, Event, true);
    Clip.Annotations.StableSort([](const auto& A, const auto& B) { return A.Time < B.Time; });
}
