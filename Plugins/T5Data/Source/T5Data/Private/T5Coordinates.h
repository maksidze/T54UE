#pragma once

#include "CoreMinimal.h"

inline constexpr float T5CentimetersPerUnit = 100.0f / 70.0f;

// T5 uses east, north, up. UE uses forward, right, up.
// P swaps the horizontal axes so a zero-heading camera looks north with east on its right.
// P is its own inverse. Rotations use P * R * P, with det(P) = -1.
inline constexpr int32 T5AxisForUnreal(int32 Axis)
{
    return Axis < 2 ? 1 - Axis : Axis;
}

template <class T>
UE::Math::TVector<T> T5ToUnrealDirection(const UE::Math::TVector<T>& Value)
{
    return {Value.Y, Value.X, Value.Z};
}

template <class T>
UE::Math::TVector<T> T5ToUnrealPosition(const UE::Math::TVector<T>& Value)
{
    return T5ToUnrealDirection(Value) * T5CentimetersPerUnit;
}

inline FVector UnrealToT5Position(const FVector& Value)
{
    return T5ToUnrealDirection(Value) / T5CentimetersPerUnit;
}

inline FQuat T5ToUnrealRotation(const FQuat& Value)
{
    return FQuat(-Value.Y, -Value.X, -Value.Z, Value.W).GetNormalized();
}
