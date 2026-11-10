#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "Widgets/SLeafWidget.h"

class FT5PapyrusRuntime;

class ST5Titles final : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(ST5Titles) {}
    SLATE_END_ARGS()
    void Construct(const FArguments&) {}
    void Command(const TArray<TSharedPtr<FJsonValue>>& Args, double Now, const FT5PapyrusRuntime& Runtime);
    void Advance(double Now);
    void SerializeCheckpoint(FArchive& Ar, const FT5PapyrusRuntime& Runtime);
    void SetWorldReady(bool bReady) { bWorldReady = bReady; }

private:
    virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D::ZeroVector; }
    virtual int32 OnPaint(const FPaintArgs&, const FGeometry&, const FSlateRect&, FSlateWindowElementList&,
        int32, const FWidgetStyle&, bool) const override;

    TSharedPtr<struct FT5TitleMovie> Movie;
    FString Sequence;
    FString HelpText;
    double HelpUntil = 0;
    mutable bool bPainted = false;
    double Started = 0, CurrentTime = 0;
    double FadeStart = 0, FadeLength = 0;
    float Fade = 0;
    bool bMenu = false, bFading = false, bFadeOut = false, bBlack = true;
    bool bWorldReady = true;
};
