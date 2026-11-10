#pragma once

#include "Components/SkeletalMeshComponent.h"
#include "T5SkinnedComponent.generated.h"

// Runtime NIF meshes have no cooked physics asset from which UE can derive bounds.
UCLASS()
class UT5SkinnedComponent : public USkeletalMeshComponent
{
    GENERATED_BODY()
public:
    TArray<FBox> BoneLocalBounds;
    FVector MorphBoundsExpansion = FVector::ZeroVector;
    FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
};
