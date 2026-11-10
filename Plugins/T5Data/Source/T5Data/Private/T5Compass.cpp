#include "T5Compass.h"
#include "T5UIResources.h"

#include "Rendering/DrawElements.h"
#include "Layout/Clipping.h"
#include "Styling/CoreStyle.h"

void ST5Compass::Update(double NewHeading, TArray<FT5CompassTarget> NewTargets, bool bNewShowReticle)
{
    Heading = NewHeading;
    Targets = MoveTemp(NewTargets);
    bShowReticle = bNewShowReticle;
    Invalidate(EInvalidateWidgetReason::Paint);
}

int32 ST5Compass::OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&,
    FSlateWindowElementList& Draw, int32 Layer, const FWidgetStyle&, bool) const
{
    const FVector2D Size = Geometry.GetLocalSize();
    if (Size.X <= 0 || Size.Y <= 0)
        return Layer;
    const double Scale = Size.Y / 1080.;
    const double Center = Size.X * .5, Y = Size.Y * .051;
    const double Half = FMath::Min(Size.X * .1415, 273. * Scale);
    const auto* Brush = FCoreStyle::Get().GetBrush("WhiteBrush");
#if WITH_DEV_AUTOMATION_TESTS
    PaintedArt.Reset();
#endif
    const auto ImportedBrush = [&](FName Name) -> const FSlateBrush*
    {
#if WITH_DEV_AUTOMATION_TESTS
        if (bReviewFallback)
            return nullptr;
#endif
        const FSlateBrush* Art = Resources ? Resources->GetBrush(Name) : nullptr;
#if WITH_DEV_AUTOMATION_TESTS
        if (Art)
            PaintedArt.Add(Name);
#endif
        return Art;
    };
    const auto Line = [&](int32 Depth, TArray<FVector2D> Points, FLinearColor Color, float Thickness) {
        FSlateDrawElement::MakeLines(Draw, Layer + Depth, Geometry.ToPaintGeometry(), MoveTemp(Points),
            ESlateDrawEffect::None, Color, true, FMath::Max(.6f, Thickness * float(Scale)));
    };
    const auto Image = [&](int32 Depth, const FSlateBrush* Art, FVector2D At, FVector2D Extent) {
        FSlateDrawElement::MakeBox(Draw, Layer + Depth,
            Geometry.ToPaintGeometry(FVector2f(Extent), FSlateLayoutTransform(FVector2f(At - Extent * .5))),
            Art, ESlateDrawEffect::None);
    };
    if (const auto* Frame = ImportedBrush(TEXT("CompassFrame")))
    {
        const double Width = 2 * Half;
        Image(0, Frame, {Center, Y}, {Width, Width * Frame->ImageSize.Y / Frame->ImageSize.X});
    }
    else
    {
        FSlateDrawElement::MakeBox(Draw, Layer, Geometry.ToPaintGeometry(
            FVector2f(2 * Half - 40 * Scale, 34 * Scale), FSlateLayoutTransform(FVector2f(Center - Half + 20 * Scale, Y - 17 * Scale))),
            Brush, ESlateDrawEffect::None, FLinearColor(0.02f, .018f, .015f, .68f));
        const FLinearColor Edge(.62f, .58f, .51f, .95f);
        for (double Side : {-1., 1.})
        {
            const double Tip = Center + Side * Half;
            const double Inset = Tip - Side * 24. * Scale;
            Line(1, {{Inset, Y-17*Scale}, {Tip, Y}, {Inset, Y+17*Scale}}, FLinearColor(.08f,.07f,.06f,1), 7);
            Line(2, {{Inset, Y-17*Scale}, {Tip, Y}, {Inset, Y+17*Scale}}, Edge, 2.5f);
            const double Knot = Tip - Side * 15. * Scale;
            Line(2, {{Knot-7*Scale,Y},{Knot,Y-6*Scale},{Knot+7*Scale,Y},{Knot,Y+6*Scale},{Knot-7*Scale,Y}}, Edge, 2);
        }
        for (double Dy : {-17., 17.})
            Line(2, {{Center-Half+24*Scale,Y+Dy*Scale}, {Center+Half-24*Scale,Y+Dy*Scale}}, Edge, 2);
    }
    constexpr double Span = 75;
    const auto Offset = [&](double Bearing) { return FMath::FindDeltaAngleDegrees(Heading, Bearing); };
    const auto X = [&](double Angle) { return Center + Angle / Span * (Half - 27 * Scale); };
    const TCHAR* Directions[] = {TEXT("N"), TEXT("E"), TEXT("S"), TEXT("W")};
    const int32 FontSize = FMath::Max(8, FMath::RoundToInt(23 * Scale));
    const auto Font = Resources ? Resources->GetFont(TEXT("Regular"), FontSize)
        : FCoreStyle::GetDefaultFontStyle("Regular", FontSize);
    for (int32 Index = 0; Index < 4; ++Index)
    {
        const double Angle = Offset(Index * 90.);
        if (FMath::Abs(Angle) >= Span) continue;
        FSlateDrawElement::MakeText(Draw, Layer+3, Geometry.ToPaintGeometry(FVector2f(22*Scale, 31*Scale),
            FSlateLayoutTransform(FVector2f(X(Angle)-7*Scale,Y-16*Scale))), FString(Directions[Index]), Font,
            ESlateDrawEffect::None, FLinearColor(.90f,.86f,.80f,1));
    }
    const auto Marker = [&](const FSlateBrush* Art, FVector2D At, double R) {
        if (Art)
        {
            const double Height = R * 1.8;
            Image(5, Art, At, {Height * Art->ImageSize.X / Art->ImageSize.Y, Height});
            return;
        }
        const TArray<FVector2D> Shape{{At.X,At.Y-R*1.34},{At.X+R*.25,At.Y-R*.51},{At.X+R*.55,At.Y-R*.51},
            {At.X,At.Y+R},{At.X-R*.55,At.Y-R*.51},{At.X-R*.25,At.Y-R*.51},{At.X,At.Y-R*1.34}};
        Line(4, Shape, FLinearColor::Black, 6);
        Line(5, Shape, FLinearColor(.88f,.9f,.92f,1), 2.6f);
        Line(5, {{At.X,At.Y+R*.72},{At.X,At.Y-R*.43}}, FLinearColor(.88f,.9f,.92f,1), 2.4f);
    };
    for (const auto& Target : Targets)
    {
        const auto* Art = Target.bDoor ? ImportedBrush(TEXT("QuestDoor")) : nullptr;
        if (!Art)
            Art = ImportedBrush(TEXT("QuestMarker"));
        const double Angle = FMath::Clamp(Offset(Target.Bearing), -Span, Span);
        Marker(Art, {X(Angle), Y + 2*Scale}, 25*Scale);
        if (Target.ScreenUV && Target.ScreenUV->X >= .025 && Target.ScreenUV->X <= .975
            && Target.ScreenUV->Y >= .11 && Target.ScreenUV->Y <= .97)
            Marker(Art, {Target.ScreenUV->X * Size.X, Target.ScreenUV->Y * Size.Y}, 16*Scale);
    }
    if (bShowReticle)
    {
        if (const auto* Reticle = ImportedBrush(TEXT("Crosshair")))
            Image(6, Reticle, Size * .5, FVector2D(22 * Scale, 22 * Scale));
        const FName PoolArt[] = {TEXT("HealthFill"), TEXT("MagickaFill"), TEXT("StaminaFill")};
        const double Centers[] = {.5, .162, .838};
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(PoolArt); ++Index)
        {
            const float Ratio = PoolRatios[Index];
            if (Ratio < 0 || (!bShowFullPools && Ratio >= .999f))
                continue;
            const auto* Frame = ImportedBrush(TEXT("ResourceMeter"));
            const auto* Fill = ImportedBrush(PoolArt[Index]);
            if (!Frame || !Fill)
                continue;
            const double Width = Size.X * .218;
            const FVector2D Extent(Width, Width * Frame->ImageSize.Y / Frame->ImageSize.X);
            const FVector2D At(Size.X * Centers[Index], Size.Y * .94);
            Image(7, Frame, At, Extent);
            const FVector2D Inner(Extent.X * .862, Extent.Y * .59);
            if (Ratio > 0)
            {
                Draw.PushClip(FSlateClippingZone(Geometry.ToPaintGeometry(
                    FVector2f(Inner.X * Ratio, Inner.Y), FSlateLayoutTransform(FVector2f(At - Inner * .5)))));
                Image(8, Fill, At, Inner);
                Draw.PopClip();
            }
        }
    }
    return Layer + 8;
}
