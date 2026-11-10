#pragma once

#include "T5Coordinates.h"

class FJsonObject;
class FJsonValue;
class USkeletalMeshComponent;
class UMaterialInterface;
class USceneComponent;
class AActor;

struct FT5SourceVertex
{
    FVector3f Position, Normal;
    FVector2f UV;
    FVector4f Color;
};
static_assert(sizeof(FT5SourceVertex) == 48);

struct FT5SkinSection
{
    TSharedPtr<FJsonObject> Data;
    UMaterialInterface* Material = nullptr;
};

USkeletalMeshComponent* BuildT5SkeletalMesh(AActor& Owner, USceneComponent& Root,
    const TArray<TSharedPtr<FJsonValue>>& Joints, TConstArrayView<FT5SkinSection> Sections,
    TConstArrayView64<uint8> Geometry, TConstArrayView64<uint8> Skin, TConstArrayView64<uint8> Morphs = {});
