#pragma once

#include "CoreMinimal.h"
#include "Widgets/SLeafWidget.h"

struct FT5CompassTarget
{
    double Bearing = 0;
    TOptional<FVector2D> ScreenUV;
};

class ST5Compass final : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(ST5Compass) {}
    SLATE_END_ARGS()
    void Construct(const FArguments&) {}
    void Update(double Heading, TArray<FT5CompassTarget> Targets);

private:
    virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D::ZeroVector; }
    virtual int32 OnPaint(const FPaintArgs&, const FGeometry&, const FSlateRect&, FSlateWindowElementList&,
        int32, const FWidgetStyle&, bool) const override;

    double Heading = 0;
    TArray<FT5CompassTarget> Targets;
};
