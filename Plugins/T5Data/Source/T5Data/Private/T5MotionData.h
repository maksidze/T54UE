#pragma once

#include "CoreMinimal.h"

// Engine movement curves are separate from the HKX bone pose in T5.
struct FT5MotionCurve
{
    struct FPosition { float Time; FVector Value; };
    struct FRotation { float Time; FQuat Value; };
    float Duration = 0;
    TArray<FPosition> Positions;
    TArray<FRotation> Rotations;
    FTransform Sample(float Time) const;
    bool Equals(const FT5MotionCurve& Other) const;
};

class FT5MotionDatabase
{
public:
    bool Read(TConstArrayView<uint8> Bytes, FString& Error);
    // Generator names, not filenames, identify clips within a character project.
    // With no project, accept only an unambiguous skeleton/generator match.
    TSharedPtr<const FT5MotionCurve> Find(const FString& Project, const FString& Skeleton,
        const FString& Generator, FString& Error) const;
    int32 ProjectCount() const { return Projects.Num(); }
private:
    struct FGeneratorKey : TDefaultMapHashableKeyFuncs<FString, int32, false>
    {
        static bool Matches(const FString& A, const FString& B) { return A.Equals(B, ESearchCase::CaseSensitive); }
        static uint32 GetKeyHash(const FString& Key) { return FCrc::StrCrc32(*Key); }
    };
    struct FProject
    {
        TArray<FString> Files;
        TMap<FString, int32, FDefaultSetAllocator, FGeneratorKey> Generators;
        TMap<int32, TSharedPtr<const FT5MotionCurve>> Curves;
    };
    TMultiMap<FString, FProject> Projects;
};
