#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class AT5Scene;
class ST5CellGrid;

struct FT5Location
{
    uint32 Id = 0;
    uint32 Parent = 0;
    uint32 Map = 0;
    uint32 Lod = 0;
    uint32 Land = 0;
    FString EditorId;
    FString Plugin;
    TSet<FIntPoint> Cells;
    TMap<FIntPoint, FString> NamedCells;
    FIntPoint MinCell = FIntPoint::ZeroValue;
    FIntPoint MaxCell = FIntPoint::ZeroValue;
    TOptional<FIntRect> MapBounds;
    FIntPoint SuggestedCell = FIntPoint::ZeroValue;
    double MapScale = 1;
    FVector2D MapOffset = FVector2D::ZeroVector;
    bool bInterior = false;

    FString Key() const;
    FString Label() const;
};

class ST5MapBrowser : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(ST5MapBrowser) {}
    SLATE_END_ARGS()
    void Construct(const FArguments& Args, AT5Scene* InOwner);
    void Refresh();
    virtual bool SupportsKeyboardFocus() const override { return true; }
    virtual FReply OnKeyDown(const FGeometry& Geometry, const FKeyEvent& Event) override;

private:
    void Filter(const FText& Text);
    void Select(TSharedPtr<FT5Location> Location, ESelectInfo::Type);
    void RefreshOverview();
    FText Details() const;
    FReply Load();

    TWeakObjectPtr<AT5Scene> Owner;
    TArray<TSharedPtr<FT5Location>> Items;
    TSharedPtr<SListView<TSharedPtr<FT5Location>>> List;
    TSharedPtr<ST5CellGrid> Overview;
    TSharedPtr<FT5Location> Selected;
    FString Query;
    int32 CellX = 0;
    int32 CellY = 0;
};
