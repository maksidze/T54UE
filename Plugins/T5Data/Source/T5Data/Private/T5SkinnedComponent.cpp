#include "T5SkinnedComponent.h"

FBoxSphereBounds UT5SkinnedComponent::CalcBounds(const FTransform& LocalToWorld) const
{
    const auto& Pose = GetComponentSpaceTransforms();
    FBox PoseBounds(ForceInit);
    if (Pose.Num() == BoneLocalBounds.Num())
        for (int32 Bone = 0; Bone < Pose.Num(); ++Bone)
            if (BoneLocalBounds[Bone].IsValid)
                PoseBounds += BoneLocalBounds[Bone].TransformBy(Pose[Bone]);
    // The union contains every influence of every vertex. Normalized, nonnegative
    // skin weights keep the final vertex inside this union's enclosing AABB.
    return PoseBounds.IsValid ? FBoxSphereBounds(PoseBounds.ExpandBy(MorphBoundsExpansion)).TransformBy(LocalToWorld)
                          : Super::CalcBounds(LocalToWorld);
}
