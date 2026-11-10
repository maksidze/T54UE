#pragma once

#include "CoreMinimal.h"
#include "Widgets/SLeafWidget.h"

struct FT5CompassTarget
{
    double Bearing = 0;
    TOptional<FVector2D> ScreenUV;
    bool bDoor = false;
};

class ST5Compass final : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(ST5Compass) {}
        SLATE_ARGUMENT(TSharedPtr<class FT5UIResources>, Resources)
    SLATE_END_ARGS()
    void Construct(const FArguments& Args) { Resources = Args._Resources; }
    void Update(double Heading, TArray<FT5CompassTarget> Targets, bool bShowReticle);
    void SetResourcePools(FVector3f Ratios, bool bWeaponsDrawn)
    {
        PoolRatios = Ratios;
        bShowFullPools = bWeaponsDrawn;
    }

#if WITH_DEV_AUTOMATION_TESTS
    void UseImportedArtForReview(bool bEnabled)
    {
        bReviewFallback = !bEnabled;
        Invalidate(EInvalidateWidgetReason::Paint);
    }
    const TSet<FName>& PaintedArtForReview() const { return PaintedArt; }
#endif

private:
    virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D::ZeroVector; }
    virtual int32 OnPaint(const FPaintArgs&, const FGeometry&, const FSlateRect&, FSlateWindowElementList&,
        int32, const FWidgetStyle&, bool) const override;

    double Heading = 0;
    bool bShowReticle = false;
    bool bShowFullPools = false;
    FVector3f PoolRatios{-1};
    TArray<FT5CompassTarget> Targets;
    TSharedPtr<class FT5UIResources> Resources;
#if WITH_DEV_AUTOMATION_TESTS
    bool bReviewFallback = false;
    mutable TSet<FName> PaintedArt;
#endif
};
