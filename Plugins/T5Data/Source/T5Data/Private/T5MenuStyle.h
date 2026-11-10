#pragma once

#include "T5UIResources.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateNoResource.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateTypes.h"
#include "Widgets/Layout/SBackgroundBlur.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"

inline TAttribute<FSlateFontInfo> T5MenuFont(const TSharedPtr<FT5UIResources>& Resources, int32 Size,
    FName Role = TEXT("Medium"))
{
    return TAttribute<FSlateFontInfo>::CreateLambda([Resources, Size, Role]
    {
        return Resources ? Resources->GetFont(Role, Size) : FCoreStyle::GetDefaultFontStyle(TEXT("BoldCondensed"), Size);
    });
}

inline TSharedRef<SWidget> T5MenuBackdrop()
{
    return SNew(SBackgroundBlur).BlurStrength(4.f).Visibility(EVisibility::HitTestInvisible)
        [SNew(SBorder).BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
            .BorderBackgroundColor(FLinearColor(.006f, .012f, .019f, .27f))];
}

inline TAttribute<const FSlateBrush*> T5MenuArt(const TSharedPtr<FT5UIResources>& Resources, FName Symbol,
    bool bPanel = true)
{
    return TAttribute<const FSlateBrush*>::CreateLambda([Resources, Symbol, bPanel]() -> const FSlateBrush*
    {
        if (Resources)
            if (const FSlateBrush* Brush = Resources->GetBrush(Symbol))
                return Brush;
        static const FSlateColorBrush Panel(FLinearColor(.015f, .015f, .015f, .91f));
        static const FSlateNoResource Empty;
        return bPanel ? static_cast<const FSlateBrush*>(&Panel) : &Empty;
    });
}

inline const FButtonStyle* T5MenuButtonStyle()
{
    static const FButtonStyle Style = FButtonStyle()
        .SetNormal(FSlateNoResource())
        .SetHovered(FSlateColorBrush(FLinearColor(1, 1, 1, .16f)))
        .SetPressed(FSlateColorBrush(FLinearColor(1, 1, 1, .29f)))
        .SetDisabled(FSlateNoResource())
        .SetNormalPadding(FMargin(6, 3)).SetPressedPadding(FMargin(6, 4, 6, 2));
    return &Style;
}

inline TSharedRef<SWidget> T5MenuContrast(const TSharedRef<SWidget>& Content, float Opacity)
{
    return SNew(SBorder).Padding(9).BorderImage(FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")))
        .BorderBackgroundColor(FLinearColor(.008f, .008f, .008f, Opacity))[Content];
}

inline TSharedRef<SWidget> T5MenuBar(const TSharedPtr<FT5UIResources>& Resources, FName Symbol,
    const TSharedRef<SWidget>& Content)
{
    return SNew(SBox).MinDesiredHeight(66)
        [SNew(SBorder).BorderImage(T5MenuArt(Resources, Symbol)).Padding(FMargin(28, 12))
            .VAlign(VAlign_Center)[Content]];
}
