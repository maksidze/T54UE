#pragma once

#include "T5UIFont.h"
#include "Async/Future.h"
#include "CoreMinimal.h"
#include "Fonts/SlateFontInfo.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateTypes.h"
#include "UObject/StrongObjectPtr.h"

struct FT5UIBitmap
{
    FIntPoint Size = FIntPoint::ZeroValue;
    TArray<FColor> Pixels;
    FString Source, Symbol;
};

struct FT5UIArtSpec
{
    FName Name;
    const TCHAR* Symbol;
};

TMap<FName, FT5UIBitmap> ImportT5UIArt(TConstArrayView<uint8> Swf, const TCHAR* Source, TConstArrayView<FT5UIArtSpec> Specs);

class FT5UIResources
{
public:
    FT5UIResources(const FString& Data, const FString& Bridge);

    void Poll();
    const FSlateBrush* GetBrush(FName Name);
    const FSliderStyle* GetRaceSliderStyle();
    FSlateFontInfo GetFont(FName Role, int32 Size);
    int32 GetEntryCount() const { return Catalog.EntryCount; }
    bool IsReady() const { return !Pending.IsValid(); }

#if WITH_DEV_AUTOMATION_TESTS
    const FString& FontConfigForReview() const { return Catalog.FontConfig; }
    void TrackMenuArtForReview() { bTrackArt = true; AccessedArt.Reset(); }
    const TSet<FName>& MenuArtForReview() const { return AccessedArt; }
#endif

private:
    struct FCatalog
    {
        int32 EntryCount = 0;
        TMap<FName, FT5UIBitmap> Art;
        FString FontConfig;
        FT5UIFontSet Fonts;
    };

    struct FImage
    {
        TStrongObjectPtr<class UTexture2D> Texture;
        FSlateBrush Brush;
    };

    static FCatalog Build(const FString& Archive, const FString& Bridge, const FString& Cache, const FString& Language);

    TFuture<FCatalog> Pending;
    FCatalog Catalog;
    TMap<FName, TUniquePtr<FImage>> Images;
    TUniquePtr<FSliderStyle> RaceSlider;
    TSharedPtr<struct FStandaloneCompositeFont> Typeface;
    TArray<TStrongObjectPtr<class UFontFace>> FontFaces;
#if WITH_DEV_AUTOMATION_TESTS
    bool bTrackArt = false;
    TSet<FName> AccessedArt;
#endif
};
