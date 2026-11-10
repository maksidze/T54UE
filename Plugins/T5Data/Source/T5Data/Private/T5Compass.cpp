#include "T5Compass.h"

#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"

void ST5Compass::Update(double NewHeading, TArray<FT5CompassTarget> NewTargets)
{
    Heading = NewHeading;
    Targets = MoveTemp(NewTargets);
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
    const auto Line = [&](int32 Depth, TArray<FVector2D> Points, FLinearColor Color, float Thickness) {
        FSlateDrawElement::MakeLines(Draw, Layer + Depth, Geometry.ToPaintGeometry(), MoveTemp(Points),
            ESlateDrawEffect::None, Color, true, FMath::Max(.6f, Thickness * float(Scale)));
    };
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
    constexpr double Span = 75;
    const auto Offset = [&](double Bearing) { return FMath::FindDeltaAngleDegrees(Heading, Bearing); };
    const auto X = [&](double Angle) { return Center + Angle / Span * (Half - 27 * Scale); };
    const TCHAR* Directions[] = {TEXT("N"), TEXT("E"), TEXT("S"), TEXT("W")};
    auto Font = FCoreStyle::GetDefaultFontStyle("Regular", FMath::Max(8, FMath::RoundToInt(23 * Scale)));
    for (int32 Index = 0; Index < 4; ++Index)
    {
        const double Angle = Offset(Index * 90.);
        if (FMath::Abs(Angle) >= Span) continue;
        FSlateDrawElement::MakeText(Draw, Layer+3, Geometry.ToPaintGeometry(FVector2f(22*Scale, 31*Scale),
            FSlateLayoutTransform(FVector2f(X(Angle)-7*Scale,Y-16*Scale))), FString(Directions[Index]), Font,
            ESlateDrawEffect::None, FLinearColor(.90f,.86f,.80f,1));
    }
    const auto Marker = [&](FVector2D At, double R) {
        const TArray<FVector2D> Shape{{At.X,At.Y-R*1.34},{At.X+R*.25,At.Y-R*.51},{At.X+R*.55,At.Y-R*.51},
            {At.X,At.Y+R},{At.X-R*.55,At.Y-R*.51},{At.X-R*.25,At.Y-R*.51},{At.X,At.Y-R*1.34}};
        Line(4, Shape, FLinearColor::Black, 6);
        Line(5, Shape, FLinearColor(.88f,.9f,.92f,1), 2.6f);
        Line(5, {{At.X,At.Y+R*.72},{At.X,At.Y-R*.43}}, FLinearColor(.88f,.9f,.92f,1), 2.4f);
    };
    for (const auto& Target : Targets)
    {
        const double Angle = FMath::Clamp(Offset(Target.Bearing), -Span, Span);
        Marker({X(Angle), Y + 2*Scale}, 25*Scale);
        if (Target.ScreenUV && Target.ScreenUV->X >= .025 && Target.ScreenUV->X <= .975
            && Target.ScreenUV->Y >= .11 && Target.ScreenUV->Y <= .97)
            Marker({Target.ScreenUV->X * Size.X, Target.ScreenUV->Y * Size.Y}, 16*Scale);
    }
    return Layer + 5;
}
